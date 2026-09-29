// Импорт и экспорт.
// Импорт: файл пишется кусками в pending-блоб, в конце одной транзакцией создается
// запись + promote. Какой кусок последний, узнаем только когда кончился поток,
// поэтому полный кусок держим до прихода следующих байтов.
// Если упал один файл - удаляем только его pending, остальные импортируются дальше.
// Файл, который уже лежит в папке (то же имя без учета регистра и те же байты -
// сверяем по кускам, пока пишем), не вставляется: повторный импорт папки докачивает
// только новое. Папки по пути тоже сливаются по имени без учета регистра.
// Чтение: расшифровываем только нужные куски, последний кешируем. Размер и число
// кусков сверяем с метой записи, не сошлось -> IntegrityError
#include <algorithm>
#include <map>
#include <unordered_map>
#include <unordered_set>

#include "safebox/domain/model/rules.hpp"
#include "safebox/domain/model/safe_format.hpp"
#include "service_impls.hpp"
#include "vault_session.hpp"

namespace safebox::app {
namespace {

using domain::BlobId;
using domain::Bytes;
using domain::EntryId;
using domain::Error;
using domain::fail;
using domain::KeyPurpose;
using domain::Kind;

constexpr std::uint64_t kSliceSize = 1u << 20; // запись в сеть порциями: отмена не ждет 8 МиБ

[[nodiscard]] std::unexpected<Error> corruptedData() {
    return fail(Error::Code::IntegrityError, "Данные файла повреждены или подменены");
}

[[nodiscard]] std::unexpected<Error> cancelledByLock() {
    return fail(Error::Code::Cancelled, "Операция прервана блокировкой сейфа");
}

// Ошибки, после которых продолжать нельзя: сейф закрыт или закрывается.
[[nodiscard]] Status fatalOnly(const Error& error) {
    if (error.code == Error::Code::Locked || error.code == Error::Code::Cancelled) {
        return std::unexpected(error);
    }
    return {};
}

struct BlobRef {
    BlobId id = 0;
    KeyPurpose purpose = KeyPurpose::Content;
    std::uint64_t size = 0;
    std::uint32_t chunkCount = 0;
};

// Проверка блоба записи перед чтением: готов, размер и число кусков сходятся.
[[nodiscard]] Result<BlobRef> checkedBlob(domain::BlobStore& blobs, BlobId id, KeyPurpose purpose,
                                          std::optional<std::uint64_t> expectedSize,
                                          std::uint32_t chunkSize) {
    auto info = blobs.info(id);
    if (!info) {
        return info.error().code == Error::Code::NotFound ? corruptedData()
                                                          : std::unexpected(info.error());
    }
    if (info->status != domain::BlobStatus::Ready ||
        info->chunkCount != domain::chunkCountFor(info->size, chunkSize) ||
        (expectedSize && *expectedSize != info->size)) {
        return corruptedData();
    }
    return BlobRef{id, purpose, info->size, info->chunkCount};
}

// Куски одного блоба с кэшем последнего расшифрованного.
class ChunkReader {
public:
    ChunkReader(const Lease& lease, domain::BlobStore& blobs, std::shared_ptr<const Sealer> sealer,
                BlobRef blob, std::uint32_t chunkSize) noexcept
        : lease_(lease), blobs_(blobs), sealer_(std::move(sealer)), blob_(blob),
          chunkSize_(chunkSize) {}
    ChunkReader(const ChunkReader&) = delete;
    ChunkReader& operator=(const ChunkReader&) = delete;
    ~ChunkReader() { domain::secureWipe(cached_); }

    [[nodiscard]] std::uint64_t size() const noexcept { return blob_.size; }
    [[nodiscard]] std::uint32_t chunkSize() const noexcept { return chunkSize_; }

    [[nodiscard]] Result<std::span<const std::byte>> chunk(std::uint32_t index) {
        if (lease_.cancelled()) {
            return cancelledByLock();
        }
        if (hasCached_ && cachedIndex_ == index) {
            return std::span<const std::byte>(cached_);
        }
        if (index >= blob_.chunkCount) {
            return corruptedData();
        }
        auto sealed = blobs_.readChunk(blob_.id, index);
        if (!sealed) {
            return sealed.error().code == Error::Code::NotFound ? corruptedData()
                                                                : std::unexpected(sealed.error());
        }
        const bool last = index + 1 == blob_.chunkCount;
        auto plain = sealer_->openChunk(blob_.purpose, blob_.id, index, last, *sealed);
        if (!plain) {
            return std::unexpected(plain.error());
        }
        const std::uint64_t expected =
            last ? blob_.size - std::uint64_t{index} * chunkSize_ : std::uint64_t{chunkSize_};
        if (plain->size() != expected) {
            domain::secureWipe(*plain);
            return corruptedData();
        }
        domain::secureWipe(cached_);
        cached_ = std::move(*plain);
        cachedIndex_ = index;
        hasCached_ = true;
        return std::span<const std::byte>(cached_);
    }

private:
    const Lease& lease_;
    domain::BlobStore& blobs_;
    std::shared_ptr<const Sealer> sealer_;
    BlobRef blob_;
    std::uint32_t chunkSize_;
    Bytes cached_;
    std::uint32_t cachedIndex_ = 0;
    bool hasCached_ = false;
};

class BlobContentStream final : public ContentStream {
public:
    BlobContentStream(Lease lease, domain::BlobStore& blobs, std::shared_ptr<const Sealer> sealer,
                      BlobRef blob, std::uint32_t chunkSize)
        : lease_(std::move(lease)), reader_(lease_, blobs, std::move(sealer), blob, chunkSize) {}

    std::uint64_t size() const noexcept override { return reader_.size(); }

    Status read(std::uint64_t offset, std::uint64_t length, domain::ByteSink& out) override {
        if (offset > size() || length > size() - offset) {
            return fail(Error::Code::InvalidArgument, "Диапазон за пределами файла");
        }
        while (length > 0) {
            const auto index = static_cast<std::uint32_t>(offset / reader_.chunkSize());
            const auto within = static_cast<std::size_t>(offset % reader_.chunkSize());
            auto chunk = reader_.chunk(index);
            if (!chunk) {
                return std::unexpected(chunk.error());
            }
            if (within >= chunk->size()) {
                return corruptedData();
            }
            const auto take = std::min({length, std::uint64_t{chunk->size() - within}, kSliceSize});
            if (auto st = out.write(chunk->subspan(within, static_cast<std::size_t>(take))); !st) {
                return st;
            }
            offset += take;
            length -= take;
        }
        return {};
    }

private:
    Lease lease_; // объявлена первой: читатель ссылается на нее и умирает раньше
    ChunkReader reader_;
};

// Последовательный источник для zip поверх ChunkReader.
class BlobSource final : public domain::ByteSource {
public:
    explicit BlobSource(ChunkReader& reader) noexcept : reader_(reader) {}

    Result<std::size_t> read(std::span<std::byte> buffer) override {
        if (pos_ >= reader_.size() || buffer.empty()) {
            return std::size_t{0};
        }
        const auto index = static_cast<std::uint32_t>(pos_ / reader_.chunkSize());
        const auto within = static_cast<std::size_t>(pos_ % reader_.chunkSize());
        auto chunk = reader_.chunk(index);
        if (!chunk) {
            return std::unexpected(chunk.error());
        }
        if (within >= chunk->size()) {
            return corruptedData();
        }
        const auto take = std::min(buffer.size(), chunk->size() - within);
        std::copy_n(chunk->begin() + static_cast<std::ptrdiff_t>(within), take, buffer.begin());
        pos_ += take;
        return take;
    }

private:
    ChunkReader& reader_;
    std::uint64_t pos_ = 0;
};

// Уникальные имена в одной папке архива (без учета регистра - как у Windows).
class UniqueNames {
public:
    [[nodiscard]] std::string take(const std::string& name, bool folder) {
        auto unique = uniqueName(name, folder, used_);
        used_.insert(foldForSearch(unique));
        return unique;
    }

private:
    std::unordered_set<std::string> used_;
};

// импорт

class ImportSessionImpl final : public ImportSession {
public:
    ImportSessionImpl(Lease lease, Ports ports, VaultSession& session,
                      std::shared_ptr<const Sealer> sealer, std::optional<EntryId> root,
                      VaultSession::CatalogPtr snapshot)
        : lease_(std::move(lease)), ports_(ports), session_(session), sealer_(std::move(sealer)),
          root_(root), snapshot_(std::move(snapshot)), chunkSize_(session.chunkSize()) {}

    ~ImportSessionImpl() override {
        if (current_) {
            discardCurrent();
            wipeCurrent();
        }
    }

    Status beginFile(std::string_view relativePath) override {
        if (current_) {
            if (auto st = endFile(); !st) {
                return st;
            }
        }
        if (lease_.cancelled()) {
            return cancelledByLock();
        }
        current_.emplace();
        auto& file = *current_;

        std::vector<std::string> parts;
        bool invalid = false;
        std::size_t start = 0;
        while (start <= relativePath.size()) {
            const auto end = relativePath.find_first_of("/\\", start);
            const auto segment = relativePath.substr(
                start, end == std::string_view::npos ? std::string_view::npos : end - start);
            if (segment == "..") {
                invalid = true;
            } else if (!segment.empty() && segment != ".") {
                parts.push_back(domain::sanitizeName(segment));
            }
            if (end == std::string_view::npos) {
                break;
            }
            start = end + 1;
        }
        for (const auto& part : parts) {
            file.path.append(file.path.empty() ? "" : "/").append(part);
        }
        if (invalid || parts.empty()) {
            if (file.path.empty()) {
                file.path = domain::sanitizeName(relativePath);
            }
            failCurrent("Недопустимый путь файла");
            return {};
        }
        file.name = parts.back();
        parts.pop_back();

        auto parent = resolveFolders(parts);
        if (!parent) {
            failCurrent(parent.error().message);
            return fatalOnly(parent.error());
        }
        file.parent = *parent;
        if (auto st = findTwins(file); !st) {
            failCurrent(st.error().message);
            return st;
        }
        auto writer = ports_.store.blobs().create();
        if (!writer) {
            failCurrent(writer.error().message);
            return fatalOnly(writer.error());
        }
        file.writer = std::move(*writer);
        file.buffer.reserve(chunkSize_);
        return {};
    }

    Status write(std::span<const std::byte> data) override {
        if (lease_.cancelled()) {
            return cancelledByLock();
        }
        if (!current_ || current_->failed) {
            return {}; // поле формы или уже отвергнутый файл - байты просто пропускаем
        }
        auto& file = *current_;
        std::size_t offset = 0;
        while (offset < data.size()) {
            if (file.buffer.size() == chunkSize_) {
                // полный кусок и пришли еще байты -> он точно не последний
                if (auto st = flush(false); !st) {
                    return st;
                }
                if (file.failed) {
                    return {};
                }
            }
            const auto take =
                std::min<std::size_t>(chunkSize_ - file.buffer.size(), data.size() - offset);
            file.buffer.insert(file.buffer.end(),
                               data.begin() + static_cast<std::ptrdiff_t>(offset),
                               data.begin() + static_cast<std::ptrdiff_t>(offset + take));
            offset += take;
        }
        return {};
    }

    Status endFile() override {
        if (!current_) {
            return {};
        }
        Status fatal;
        if (!current_->failed) {
            fatal = flush(true);
        }
        if (fatal && !current_->failed) {
            fatal = complete();
        }
        if (current_->failed) {
            ++result_.failed;
            result_.failures.push_back({current_->path, current_->error});
            discardCurrent();
        } else if (current_->skipped) {
            ++result_.skipped;
            discardCurrent();
        } else {
            ++result_.imported;
        }
        wipeCurrent();
        current_.reset();
        return fatal;
    }

    domain::ImportResult finish() override {
        if (current_) {
            (void)endFile();
        }
        return std::move(result_);
    }

private:
    struct CurrentFile {
        std::string path; // для отчета: очищенный относительный путь
        std::string name;
        std::optional<EntryId> parent;
        std::unique_ptr<domain::BlobWriter> writer;
        std::optional<BlobId> thumbBlob;
        Bytes buffer;      // текущий кусок (<= chunkSize)
        Bytes thumbSource; // фото целиком для миниатюры (<= kThumbnailSourceLimit)
        // файлы папки с тем же именем, чье содержимое пока совпадает с записанным
        std::vector<std::unique_ptr<ChunkReader>> twins;
        bool collectThumb = false;
        std::uint64_t size = 0;
        std::uint32_t chunks = 0;
        Kind kind = Kind::File;
        std::string mime;
        std::string url;
        bool failed = false;
        bool committed = false;
        bool skipped = false;
        std::string error;
    };

    struct KnownFolder {
        EntryId id = 0;
        bool created = false; // создана этим импортом: в снимке ее детей нет
    };

    struct KnownFile {
        BlobId blob = 0;
        std::uint64_t size = 0;
    };
    using FileIndex = std::unordered_multimap<std::string, KnownFile>; // folded имя -> файлы

    // Запечатать и записать текущий кусок; тип файла - по первому куску.
    Status flush(bool last) {
        auto& file = *current_;
        if (file.chunks == 0) {
            const auto head = std::span<const std::byte>(file.buffer)
                                  .first(std::min(file.buffer.size(), domain::kSniffBytes));
            const auto detected = domain::detectKind(head, file.name);
            file.kind = detected.kind;
            file.mime = std::string(detected.mime);
            if (file.kind == Kind::Link) {
                if (auto url = domain::shortcutUrl(head)) {
                    file.url = std::move(*url);
                } else {
                    file.kind = Kind::File;
                }
            }
            file.collectThumb = file.kind == Kind::Photo;
        }
        if (file.collectThumb) {
            if (file.thumbSource.size() + file.buffer.size() > domain::kThumbnailSourceLimit) {
                file.collectThumb = false; // слишком большое фото - будет заглушка
                domain::secureWipe(file.thumbSource);
                file.thumbSource.shrink_to_fit();
            } else {
                file.thumbSource.insert(file.thumbSource.end(), file.buffer.begin(),
                                        file.buffer.end());
            }
        }
        matchTwins(file);
        auto sealed = sealer_->sealChunk(KeyPurpose::Content, file.writer->id(), file.chunks, last,
                                         file.buffer);
        if (!sealed) {
            failCurrent(sealed.error().message);
            return {};
        }
        if (auto st = file.writer->append(*sealed); !st) {
            failCurrent(st.error().message);
            return fatalOnly(st.error());
        }
        file.size += file.buffer.size();
        ++file.chunks;
        domain::secureWipe(file.buffer); // емкость остается под следующий кусок
        return {};
    }

    // Все куски записаны: размер, миниатюра, запись + promote одной транзакцией.
    Status complete() {
        auto& file = *current_;
        const auto failWith = [this](const Error& e) {
            failCurrent(e.message);
            return fatalOnly(e);
        };
        // Все куски совпали с файлом того же имени и размер тот же - это он и
        // есть, второй раз не вставляем.
        if (std::ranges::any_of(file.twins,
                                [&](const auto& twin) { return twin->size() == file.size; })) {
            file.skipped = true;
            return {};
        }
        file.twins.clear();
        if (auto st = file.writer->finish(file.size); !st) {
            return failWith(st.error());
        }
        if (file.collectThumb && !file.thumbSource.empty()) {
            auto thumb = ports_.thumbnailer.make(file.thumbSource);
            domain::secureWipe(file.thumbSource);
            if (thumb && !thumb->empty()) {
                auto blob = writeBlob(KeyPurpose::Thumbnails, *thumb);
                domain::secureWipe(*thumb);
                if (blob) {
                    file.thumbBlob = *blob;
                } else if (auto st = fatalOnly(blob.error()); !st) {
                    return failWith(blob.error());
                } // иначе - без миниатюры: карточка-заглушка, не ошибка
            }
        }

        auto uow = ports_.store.begin();
        if (!uow) {
            return failWith(uow.error());
        }
        domain::EntryRecord record;
        record.parentId = file.parent;
        record.isFolder = false;
        record.blobId = file.writer->id();
        record.thumbBlobId = file.thumbBlob;
        auto id = (*uow)->entries().insert(record);
        if (!id) {
            return failWith(id.error());
        }
        const auto now = domain::toUnixMillis(ports_.clock.now());
        domain::EntryMeta meta;
        meta.kind = file.kind;
        meta.mime = file.mime;
        meta.size = file.size;
        meta.url = file.url;
        meta.createdAt = now;
        meta.modifiedAt = now;
        meta.blobId = file.writer->id();
        meta.thumbBlobId = file.thumbBlob;
        auto encName = sealer_->sealName(*id, file.name);
        if (!encName) {
            return failWith(encName.error());
        }
        auto encMeta = sealer_->sealMeta(*id, meta);
        if (!encMeta) {
            return failWith(encMeta.error());
        }
        if (auto st = (*uow)->entries().updateSealed(*id, *encName, *encMeta); !st) {
            return failWith(st.error());
        }
        if (auto st = (*uow)->blobs().promote(file.writer->id()); !st) {
            return failWith(st.error());
        }
        if (file.thumbBlob) {
            if (auto st = (*uow)->blobs().promote(*file.thumbBlob); !st) {
                return failWith(st.error());
            }
        }
        if (auto st = (*uow)->commit(); !st) {
            return failWith(st.error());
        }
        file.committed = true;
        // дубликат внутри одной партии тоже пропустится
        filesOf(file.parent)
            .emplace(foldForSearch(file.name), KnownFile{file.writer->id(), file.size});
        session_.invalidateCatalog();
        return {};
    }

    // Файлы папки по именам: из снимка каталога плюс вставленные этим импортом.
    FileIndex& filesOf(std::optional<EntryId> parent) {
        const auto [it, inserted] = files_.try_emplace(parent);
        if (inserted) {
            for (const auto child : snapshot_->childrenOf(parent)) {
                const auto* node = snapshot_->find(child);
                if (node != nullptr && !node->entry.isFolder() && node->entry.meta.blobId) {
                    it->second.emplace(node->folded,
                                       KnownFile{*node->entry.meta.blobId, node->entry.meta.size});
                }
            }
        }
        return it->second;
    }

    // Кандидаты в дубликаты: файлы папки с тем же именем без учета регистра.
    // Битый кандидат просто не дубликат, прерывает только блокировка.
    Status findTwins(CurrentFile& file) {
        const auto [from, to] = filesOf(file.parent).equal_range(foldForSearch(file.name));
        for (auto it = from; it != to; ++it) {
            auto blob = checkedBlob(ports_.store.blobs(), it->second.blob, KeyPurpose::Content,
                                    it->second.size, chunkSize_);
            if (!blob) {
                if (auto st = fatalOnly(blob.error()); !st) {
                    return st;
                }
                continue;
            }
            file.twins.push_back(std::make_unique<ChunkReader>(lease_, ports_.store.blobs(),
                                                               sealer_, *blob, chunkSize_));
        }
        return {};
    }

    // Сверить текущий кусок с кандидатами: короче или байты разошлись -> не дубликат.
    void matchTwins(CurrentFile& file) {
        const auto end = file.size + file.buffer.size();
        std::erase_if(file.twins, [&](const std::unique_ptr<ChunkReader>& twin) {
            if (end > twin->size()) {
                return true;
            }
            auto chunk = twin->chunk(file.chunks);
            return !chunk || !std::ranges::equal(*chunk, file.buffer);
        });
    }

    Result<std::optional<EntryId>> resolveFolders(const std::vector<std::string>& dirs) {
        std::optional<EntryId> parent = root_;
        bool parentCreated = false;
        std::string key;
        for (const auto& dir : dirs) {
            // без учета регистра: "Pict" и "pict" на Windows - одна папка
            const auto foldedDir = foldForSearch(dir);
            key.append(foldedDir).push_back('/');
            if (const auto it = folders_.find(key); it != folders_.end()) {
                parent = it->second.id;
                parentCreated = it->second.created;
                continue;
            }
            std::optional<EntryId> found;
            if (!parentCreated) {
                for (const auto child : snapshot_->childrenOf(parent)) {
                    const auto* node = snapshot_->find(child);
                    if (node != nullptr && node->entry.isFolder() && node->folded == foldedDir) {
                        found = child;
                        break;
                    }
                }
            }
            bool created = false;
            if (!found) {
                auto made = createFolder(parent, dir);
                if (!made) {
                    return std::unexpected(made.error());
                }
                found = *made;
                created = true;
            }
            folders_.emplace(key, KnownFolder{*found, created});
            parent = found;
            parentCreated = created;
        }
        return parent;
    }

    Result<EntryId> createFolder(std::optional<EntryId> parent, const std::string& name) {
        auto uow = ports_.store.begin();
        if (!uow) {
            return std::unexpected(uow.error());
        }
        domain::EntryRecord record;
        record.parentId = parent;
        record.isFolder = true;
        auto id = (*uow)->entries().insert(record);
        if (!id) {
            return std::unexpected(id.error());
        }
        domain::EntryMeta meta;
        meta.kind = Kind::Folder;
        meta.createdAt = meta.modifiedAt = domain::toUnixMillis(ports_.clock.now());
        auto encName = sealer_->sealName(*id, name);
        if (!encName) {
            return std::unexpected(encName.error());
        }
        auto encMeta = sealer_->sealMeta(*id, meta);
        if (!encMeta) {
            return std::unexpected(encMeta.error());
        }
        if (auto st = (*uow)->entries().updateSealed(*id, *encName, *encMeta); !st) {
            return std::unexpected(st.error());
        }
        if (auto st = (*uow)->commit(); !st) {
            return std::unexpected(st.error());
        }
        session_.invalidateCatalog();
        return *id;
    }

    // Небольшой блоб целиком из памяти (миниатюра): pending, пока не promote.
    Result<BlobId> writeBlob(KeyPurpose purpose, std::span<const std::byte> data) {
        auto writer = ports_.store.blobs().create();
        if (!writer) {
            return std::unexpected(writer.error());
        }
        const auto id = (*writer)->id();
        const auto discard = [&](const Error& e) -> Result<BlobId> {
            (void)ports_.store.blobs().discard(id);
            return std::unexpected(e);
        };
        const auto count = domain::chunkCountFor(data.size(), chunkSize_);
        for (std::uint32_t i = 0; i < count; ++i) {
            const auto from = std::size_t{i} * chunkSize_;
            const auto part =
                data.subspan(from, std::min<std::size_t>(chunkSize_, data.size() - from));
            auto sealed = sealer_->sealChunk(purpose, id, i, i + 1 == count, part);
            if (!sealed) {
                return discard(sealed.error());
            }
            if (auto st = (*writer)->append(*sealed); !st) {
                return discard(st.error());
            }
        }
        if (auto st = (*writer)->finish(data.size()); !st) {
            return discard(st.error());
        }
        return id;
    }

    void failCurrent(std::string message) {
        current_->failed = true;
        current_->error = std::move(message);
        wipeCurrent();
    }

    void discardCurrent() noexcept {
        if (!current_ || current_->committed) {
            return;
        }
        if (current_->writer) {
            (void)ports_.store.blobs().discard(current_->writer->id());
        }
        if (current_->thumbBlob) {
            (void)ports_.store.blobs().discard(*current_->thumbBlob);
        }
    }

    void wipeCurrent() noexcept {
        if (current_) {
            domain::secureWipe(current_->buffer);
            domain::secureWipe(current_->thumbSource);
            current_->twins.clear(); // ChunkReader стирает свой кеш сам
        }
    }

    Lease lease_; // объявлена первой -> освобождается последней (после discard)
    Ports ports_;
    VaultSession& session_;
    std::shared_ptr<const Sealer> sealer_;
    std::optional<EntryId> root_;
    VaultSession::CatalogPtr snapshot_; // каталог на начало импорта
    std::uint32_t chunkSize_;
    std::map<std::string, KnownFolder> folders_;        // "a/b/" (folded) -> папка этого импорта
    std::map<std::optional<EntryId>, FileIndex> files_; // папка -> ее файлы, для дубликатов
    std::optional<CurrentFile> current_;
    domain::ImportResult result_;
};

// сервис

class ImportExportServiceImpl final : public ImportExportService {
public:
    explicit ImportExportServiceImpl(Ports ports) : ports_(ports) {}

    Result<std::unique_ptr<ImportSession>> beginImport(Lease lease,
                                                       std::optional<EntryId> parent) override {
        auto ctx = contextOf(lease);
        if (!ctx) {
            return std::unexpected(ctx.error());
        }
        auto catalog = catalogOf(*ctx, ports_.store);
        if (!catalog) {
            return std::unexpected(catalog.error());
        }
        if (parent) {
            const auto* node = (*catalog)->find(*parent);
            if (node == nullptr) {
                return fail(Error::Code::NotFound, "Папка не найдена");
            }
            if (!node->entry.isFolder()) {
                return fail(Error::Code::InvalidArgument, "Импорт возможен только в папку");
            }
        }
        VaultSession& session = ctx->session;
        auto sealer = ctx->sealer;
        return std::unique_ptr<ImportSession>(std::make_unique<ImportSessionImpl>(
            std::move(lease), ports_, session, std::move(sealer), parent, std::move(*catalog)));
    }

    Result<OpenedContent> openContent(Lease lease, EntryId id, ContentVariant variant) override {
        auto ctx = contextOf(lease);
        if (!ctx) {
            return std::unexpected(ctx.error());
        }
        auto catalog = catalogOf(*ctx, ports_.store);
        if (!catalog) {
            return std::unexpected(catalog.error());
        }
        const auto* node = (*catalog)->find(id);
        if (node == nullptr) {
            return fail(Error::Code::NotFound, "Объект не найден");
        }
        const domain::Entry& entry = node->entry;
        const auto chunkSize = ctx->session.chunkSize();
        Result<BlobRef> blob = corruptedData();
        std::string mime;
        if (variant == ContentVariant::Original) {
            if (entry.isFolder()) {
                return fail(Error::Code::InvalidArgument, "Папку нельзя открыть как файл");
            }
            if (!entry.meta.blobId) {
                return corruptedData();
            }
            blob = checkedBlob(ports_.store.blobs(), *entry.meta.blobId, KeyPurpose::Content,
                               entry.meta.size, chunkSize);
            mime = entry.meta.mime;
        } else {
            if (!entry.meta.thumbBlobId) {
                return fail(Error::Code::NotFound, "У объекта нет миниатюры");
            }
            blob = checkedBlob(ports_.store.blobs(), *entry.meta.thumbBlobId,
                               KeyPurpose::Thumbnails, std::nullopt, chunkSize);
            mime = "image/jpeg";
        }
        if (!blob) {
            return std::unexpected(blob.error());
        }
        OpenedContent out;
        out.entry = entry;
        out.mime = std::move(mime);
        auto sealer = ctx->sealer;
        out.stream = std::make_unique<BlobContentStream>(std::move(lease), ports_.store.blobs(),
                                                         std::move(sealer), *blob, chunkSize);
        return out;
    }

    Status exportZip(const Lease& lease, EntryId folder, domain::ByteSink& out) override {
        auto ctx = contextOf(lease);
        if (!ctx) {
            return std::unexpected(ctx.error());
        }
        auto catalog = catalogOf(*ctx, ports_.store);
        if (!catalog) {
            return std::unexpected(catalog.error());
        }
        const Catalog& cat = **catalog;
        const auto* root = cat.find(folder);
        if (root == nullptr) {
            return fail(Error::Code::NotFound, "Папка не найдена");
        }
        if (!root->entry.isFolder()) {
            return fail(Error::Code::InvalidArgument, "Это не папка");
        }
        const auto chunkSize = ctx->session.chunkSize();
        auto zip = ports_.zip.start(out);

        struct Item {
            EntryId id;
            std::string path;
        };
        std::vector<Item> stack{{folder, root->entry.name}};
        std::unordered_set<EntryId> visited;
        while (!stack.empty()) {
            Item item = std::move(stack.back());
            stack.pop_back();
            if (!visited.insert(item.id).second) {
                continue;
            }
            if (lease.cancelled()) {
                return cancelledByLock();
            }
            const auto& entry = cat.find(item.id)->entry;
            if (entry.isFolder()) {
                if (auto st = zip->addDirectory(item.path + "/", entry.meta.modifiedAt); !st) {
                    return st;
                }
                UniqueNames names;
                std::vector<Item> next;
                for (const auto child : cat.childrenOf(item.id)) {
                    const auto& c = cat.find(child)->entry;
                    next.push_back({child, item.path + "/" + names.take(c.name, c.isFolder())});
                }
                for (auto it = next.rbegin(); it != next.rend(); ++it) {
                    stack.push_back(std::move(*it));
                }
                continue;
            }
            if (!entry.meta.blobId) {
                return corruptedData();
            }
            auto blob = checkedBlob(ports_.store.blobs(), *entry.meta.blobId, KeyPurpose::Content,
                                    entry.meta.size, chunkSize);
            if (!blob) {
                return std::unexpected(blob.error());
            }
            ChunkReader reader(lease, ports_.store.blobs(), ctx->sealer, *blob, chunkSize);
            BlobSource source(reader);
            if (auto st = zip->addFile(item.path, blob->size, entry.meta.modifiedAt, source); !st) {
                return st;
            }
        }
        return zip->finish();
    }

private:
    Ports ports_;
};

} // namespace

std::shared_ptr<ImportExportService> makeImportExportService(Ports ports) {
    return std::make_shared<ImportExportServiceImpl>(ports);
}

} // namespace safebox::app
