// Импорт и экспорт.
// Импорт: файл пишется кусками в pending-блоб, в конце одной транзакцией создается
// запись + promote. Какой кусок последний, узнаем только когда кончился поток,
// поэтому полный кусок держим до прихода следующих байтов.
// Если упал один файл - удаляем только его pending, остальные импортируются дальше.
// Имя файла занято (у родителя: снимок каталога плюс созданное этим импортом, без учета
// регистра): Skip - файл не пишется вообще, KeepBoth - "имя (2).ext", Replace - файл пишется
// в новый блоб, а в конце у существующей записи меняются блобы и мета (id, имя, описание,
// теги и дети остаются). Папки по пути сливаются с существующими папками по имени без
// учета регистра.
// Чтение: расшифровываем только нужные куски, последний кешируем. Размер и число
// кусков сверяем с метой записи, не сошлось -> IntegrityError
#include <algorithm>
#include <map>
#include <unordered_map>
#include <unordered_set>

#include "path_folders.hpp"
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

constexpr std::string_view kAttachmentsSuffix = " (вложения)";

// Имя записи в архиве: ссылка без блоба лежит ярлыком "имя.url".
[[nodiscard]] std::string zipName(const domain::Entry& entry) {
    const bool shortcut = entry.meta.kind == Kind::Link && !entry.meta.blobId;
    return shortcut ? domain::shortcutFileName(entry.name) : entry.name;
}

// импорт

class ImportSessionImpl final : public ImportSession {
public:
    ImportSessionImpl(Lease lease, Ports ports, VaultSession& session,
                      std::shared_ptr<const Sealer> sealer, std::optional<EntryId> root,
                      VaultSession::CatalogPtr snapshot)
        : lease_(std::move(lease)), ports_(ports), session_(session), sealer_(sealer),
          paths_(ports, session, std::move(sealer), root, std::move(snapshot)),
          chunkSize_(session.chunkSize()) {}

    ~ImportSessionImpl() override {
        if (current_) {
            discardCurrent();
            wipeCurrent();
        }
    }

    Status beginFile(std::string_view relativePath, const ImportFileOptions& options) override {
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
        file.sourceModifiedAt = options.sourceModifiedAt;

        auto split = splitPath(relativePath);
        file.path = std::move(split.clean);
        if (!split.valid) {
            failCurrent("Недопустимый путь файла");
            return {};
        }
        file.name = std::move(split.name);

        auto parent = paths_.resolve(split.dirs);
        if (!parent) {
            failCurrent(parent.error().message);
            return fatalOnly(parent.error());
        }
        file.parent = *parent;
        resolveName(file, options.onConflict);
        if (file.skipped) {
            return {}; // в хранилище не пишем ничего
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
        if (!current_ || current_->failed || current_->skipped) {
            return {}; // поле формы, отвергнутый или пропущенный файл - байты просто пропускаем
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
        if (!current_->failed && !current_->skipped) {
            fatal = flush(true);
            if (fatal && !current_->failed) {
                fatal = complete();
            }
        }
        if (current_->failed) {
            ++result_.failed;
            result_.failures.push_back({current_->path, current_->error});
            discardCurrent();
        } else if (current_->skipped) {
            ++result_.skipped;
        } else if (current_->replaces) {
            ++result_.replaced;
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
        if (result_.replaced > 0) {
            (void)ports_.store.compact(); // старые блобы освободили место в файле
        }
        return std::move(result_);
    }

private:
    struct CurrentFile {
        std::string path; // для отчета: очищенный относительный путь
        std::string name; // имя новой записи; у замены не используется, только тип по расширению
        std::optional<EntryId> parent;
        std::optional<EntryId> replaces; // Replace: запись, чей блоб и мета заменяются
        std::optional<std::int64_t> sourceModifiedAt;
        std::unique_ptr<domain::BlobWriter> writer;
        std::optional<BlobId> thumbBlob;
        Bytes buffer;      // текущий кусок (<= chunkSize)
        Bytes thumbSource; // фото целиком для миниатюры (<= kThumbnailSourceLimit)
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

    // Имя занято -> по решению: пропуск, уникальное имя или запись поверх существующей.
    // Существующая папка не заменяется: файл получает уникальное имя рядом.
    void resolveName(CurrentFile& file, ConflictPolicy policy) {
        const auto& siblings = paths_.siblingsOf(file.parent);
        const auto* occupant = siblings.find(foldForSearch(file.name));
        if (occupant == nullptr) {
            return;
        }
        if (policy == ConflictPolicy::Replace && occupant->isFolder) {
            policy = ConflictPolicy::KeepBoth;
        }
        switch (policy) {
        case ConflictPolicy::Skip:
            file.skipped = true;
            break;
        case ConflictPolicy::Replace:
            file.replaces = occupant->id;
            break;
        case ConflictPolicy::KeepBoth:
            file.name = uniqueName(file.name, false, siblings.taken());
            break;
        }
    }

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
        if (auto st = file.writer->finish(file.size); !st) {
            return failWith(st.error());
        }
        if (file.collectThumb && !file.thumbSource.empty()) {
            auto thumb = ports_.thumbnailer.make(file.thumbSource);
            domain::secureWipe(file.thumbSource);
            if (thumb && !thumb->empty()) {
                auto blob = writeBlob(ports_.store.blobs(), *sealer_, chunkSize_,
                                      KeyPurpose::Thumbnails, *thumb);
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
        const auto now = domain::toUnixMillis(ports_.clock.now());
        auto id = file.replaces ? replaceEntry(**uow, now) : insertEntry(**uow, now);
        if (!id) {
            return failWith(id.error());
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
        if (!file.replaces) {
            // следующие файлы партии с этим именем уже конфликтуют с ним
            paths_.siblingsOf(file.parent).add(foldForSearch(file.name), {*id, false});
        }
        session_.invalidateCatalog();
        return {};
    }

    // Поля записи, которые определяются самим файлом.
    void applyContent(domain::EntryMeta& meta, std::int64_t now) const {
        const auto& file = *current_;
        meta.kind = file.kind;
        meta.mime = file.mime;
        meta.size = file.size;
        meta.url = file.url;
        meta.modifiedAt = now;
        meta.blobId = file.writer->id();
        meta.thumbBlobId = file.thumbBlob;
        meta.sourceModifiedAt = file.sourceModifiedAt;
    }

    Result<EntryId> insertEntry(domain::UnitOfWork& uow, std::int64_t now) {
        const auto& file = *current_;
        domain::EntryRecord record;
        record.parentId = file.parent;
        record.isFolder = false;
        record.blobId = file.writer->id();
        record.thumbBlobId = file.thumbBlob;
        auto id = uow.entries().insert(record);
        if (!id) {
            return std::unexpected(id.error());
        }
        domain::EntryMeta meta;
        meta.createdAt = now;
        applyContent(meta, now);
        auto encName = sealer_->sealName(*id, file.name);
        if (!encName) {
            return std::unexpected(encName.error());
        }
        auto encMeta = sealer_->sealMeta(*id, meta);
        if (!encMeta) {
            return std::unexpected(encMeta.error());
        }
        if (auto st = uow.entries().updateSealed(*id, *encName, *encMeta); !st) {
            return std::unexpected(st.error());
        }
        return *id;
    }

    // Существующая запись получает новые блобы и содержимое меты; старые блобы уходят здесь же.
    Result<EntryId> replaceEntry(domain::UnitOfWork& uow, std::int64_t now) {
        const auto& file = *current_;
        auto record = uow.entries().get(*file.replaces);
        if (!record) {
            return std::unexpected(record.error());
        }
        auto entry = sealer_->openEntry(*record);
        if (!entry) {
            return std::unexpected(entry.error());
        }
        applyContent(entry->meta, now);
        auto encMeta = sealer_->sealMeta(record->id, entry->meta);
        domain::secureWipe(entry->name);
        domain::secureWipe(entry->meta.description);
        if (!encMeta) {
            return std::unexpected(encMeta.error());
        }
        const auto oldBlob = record->blobId;
        const auto oldThumb = record->thumbBlobId;
        record->blobId = file.writer->id();
        record->thumbBlobId = file.thumbBlob;
        record->encMeta = std::move(*encMeta);
        if (auto st = uow.entries().update(*record); !st) {
            return std::unexpected(st.error());
        }
        for (const auto stale : {oldBlob, oldThumb}) {
            if (stale) {
                if (auto st = uow.blobs().remove(*stale); !st) {
                    return std::unexpected(st.error());
                }
            }
        }
        return record->id;
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
        }
    }

    Lease lease_; // объявлена первой -> освобождается последней (после discard)
    Ports ports_;
    VaultSession& session_;
    std::shared_ptr<const Sealer> sealer_;
    PathFolders paths_; // папки пути и занятые имена; каталог на начало импорта
    std::uint32_t chunkSize_;
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
        if (parent && (*catalog)->find(*parent) == nullptr) {
            return fail(Error::Code::NotFound, "Объект не найден");
        }
        VaultSession& session = ctx->session;
        auto sealer = ctx->sealer;
        return std::unique_ptr<ImportSession>(std::make_unique<ImportSessionImpl>(
            std::move(lease), ports_, session, std::move(sealer), parent, std::move(*catalog)));
    }

    Result<ImportPlan> planImport(const Lease& lease, std::optional<EntryId> parent,
                                  std::span<const ImportPlanFile> files) override {
        auto ctx = contextOf(lease);
        if (!ctx) {
            return std::unexpected(ctx.error());
        }
        auto catalog = catalogOf(*ctx, ports_.store);
        if (!catalog) {
            return std::unexpected(catalog.error());
        }
        const Catalog& cat = **catalog;
        if (parent && cat.find(*parent) == nullptr) {
            return fail(Error::Code::NotFound, "Объект не найден");
        }
        std::map<std::optional<EntryId>, Siblings> siblings;
        const auto siblingsOf = [&](std::optional<EntryId> id) -> const Siblings& {
            return siblings.try_emplace(id, cat, id).first->second;
        };

        ImportPlan plan;
        for (const auto& file : files) {
            if (lease.cancelled()) {
                return cancelledByLock();
            }
            const auto split = splitPath(file.path);
            if (!split.valid) {
                ++plan.newFiles;
                continue;
            }
            // Как при импорте, но ничего не создаем: нет папки пути - конфликтовать не с чем.
            std::optional<EntryId> dir = parent;
            bool exists = true;
            for (const auto& name : split.dirs) {
                const auto* folder = siblingsOf(dir).find(foldForSearch(name));
                if (folder == nullptr || !folder->isFolder) {
                    exists = false;
                    break;
                }
                dir = folder->id;
            }
            const auto* occupant =
                exists ? siblingsOf(dir).find(foldForSearch(split.name)) : nullptr;
            if (occupant == nullptr) {
                ++plan.newFiles;
                continue;
            }
            plan.conflicts.push_back(
                {file.path, describeEntry(ctx->session, cat, *cat.find(occupant->id))});
        }
        return plan;
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
                if (entry.meta.kind == Kind::Link) {
                    return fail(Error::Code::NotFound, "У ссылки нет содержимого, только адрес");
                }
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

    Status exportZip(const Lease& lease, EntryId id, domain::ByteSink& out) override {
        auto ctx = contextOf(lease);
        if (!ctx) {
            return std::unexpected(ctx.error());
        }
        auto catalog = catalogOf(*ctx, ports_.store);
        if (!catalog) {
            return std::unexpected(catalog.error());
        }
        const Catalog& cat = **catalog;
        const auto* root = cat.find(id);
        if (root == nullptr) {
            return fail(Error::Code::NotFound, "Объект не найден");
        }
        if (!root->entry.isFolder() && root->childCount == 0) {
            return fail(Error::Code::InvalidArgument,
                        "Архивом скачивается папка или запись с вложениями");
        }
        const auto chunkSize = ctx->session.chunkSize();
        auto zip = ports_.zip.start(out);

        struct Item {
            EntryId id;
            std::string path;        // путь записи в архиве
            std::string attachments; // каталог вложений не-папки с детьми, иначе пусто
        };
        std::vector<Item> stack;
        // Каталог и его дети (в стек - так, чтобы первый ребенок вышел первым). Имена
        // уникальны; каталоги вложений получают их после настоящих записей: настоящие
        // имена не сдвигаются.
        const auto openDirectory = [&](EntryId dir, const std::string& path) -> Status {
            if (auto st = zip->addDirectory(path + "/", cat.find(dir)->entry.meta.modifiedAt);
                !st) {
                return st;
            }
            struct Child {
                EntryId id;
                std::string leaf;
                std::string attachments;
            };
            UniqueNames names;
            std::vector<Child> children;
            for (const auto child : cat.childrenOf(dir)) {
                const auto& entry = cat.find(child)->entry;
                children.push_back({child, names.take(zipName(entry), entry.isFolder()), {}});
            }
            for (auto& child : children) {
                const auto* node = cat.find(child.id);
                if (!node->entry.isFolder() && node->childCount > 0) {
                    child.attachments =
                        names.take(child.leaf + std::string(kAttachmentsSuffix), true);
                }
            }
            for (auto it = children.rbegin(); it != children.rend(); ++it) {
                stack.push_back(
                    {it->id, path + "/" + it->leaf,
                     it->attachments.empty() ? std::string{} : path + "/" + it->attachments});
            }
            return {};
        };
        const auto addFile = [&](const std::string& path, const domain::Entry& entry) -> Status {
            if (!entry.meta.blobId) {
                if (entry.meta.kind != Kind::Link) {
                    return corruptedData();
                }
                // ссылка без блоба - ярлык, как его сделал бы проводник
                auto shortcut = domain::shortcutContent(entry.meta.url);
                domain::MemorySource source(domain::asBytes(shortcut));
                auto st = zip->addFile(path, shortcut.size(), entry.meta.modifiedAt, source);
                domain::secureWipe(shortcut);
                return st;
            }
            auto blob = checkedBlob(ports_.store.blobs(), *entry.meta.blobId, KeyPurpose::Content,
                                    entry.meta.size, chunkSize);
            if (!blob) {
                return std::unexpected(blob.error());
            }
            ChunkReader reader(lease, ports_.store.blobs(), ctx->sealer, *blob, chunkSize);
            BlobSource source(reader);
            return zip->addFile(path, blob->size, entry.meta.modifiedAt, source);
        };

        // Папка входит в архив своим каталогом; у записи - только каталог ее вложений.
        const auto& top = root->entry;
        const auto topPath = top.isFolder() ? top.name : top.name + std::string(kAttachmentsSuffix);
        if (auto st = openDirectory(id, topPath); !st) {
            return st;
        }
        std::unordered_set<EntryId> visited{id};
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
                if (auto st = openDirectory(item.id, item.path); !st) {
                    return st;
                }
                continue;
            }
            if (auto st = addFile(item.path, entry); !st) {
                return st;
            }
            if (!item.attachments.empty()) {
                if (auto st = openDirectory(item.id, item.attachments); !st) {
                    return st;
                }
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
