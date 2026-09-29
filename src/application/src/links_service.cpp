// LinksService: ссылки без содержимого и их предпросмотр (название, описание, миниатюра).
// create: все ссылки запроса и их папки - одна транзакция. Дубль = у того же родителя уже есть
// ссылка с тем же normalizeUrl (в том числе созданная этим же запросом).
// Предпросмотр: страница -> заголовок, описание, картинка -> миниатюра. Сеть - всегда без аренды
// и без транзакции, чтобы lock не ждал сайт: аренда берется заново только для записи, и если
// сейф за это время закрылся, результат выбрасывается. Запись проверяет, что ссылка на месте и
// ее адрес прежний. Очередь заданий - на один фоновый поток; задания заблокированного сейфа
// отбрасываются, не дойдя до сети. Адреса и загруженные байты стираются после обработки
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "path_folders.hpp"
#include "safebox/domain/model/link_preview.hpp"
#include "safebox/domain/model/rules.hpp"
#include "service_impls.hpp"
#include "vault_session.hpp"

namespace safebox::app {
namespace {

using namespace std::chrono_literals;

using domain::BlobId;
using domain::Bytes;
using domain::EntryId;
using domain::Error;
using domain::fail;
using domain::KeyPurpose;
using domain::Kind;

// refreshPreview обязан уложиться в 15 с. Реальный таймаут WinHTTP бывает больше заданного на
// 2-4 с, поэтому страница получает 8 с, а картинка - остаток бюджета за вычетом запаса, но не
// больше 5 с, и совсем не запрашивается, если остаток меньше запаса + 1 с.
constexpr auto kPageTimeout = 8s;
constexpr auto kImageTimeout = 5s;
constexpr auto kPreviewBudget = 15s;
constexpr auto kTimeoutSlack = 4s;
constexpr std::size_t kMaxPageBytes = 1u << 20;
constexpr std::size_t kMaxImageBytes = 5u << 20;
constexpr std::size_t kMaxImageCandidates = 3; // og:image, потом значки - дальше не пробуем
constexpr std::size_t kMaxPreviewNameChars = 200;
constexpr std::string_view kShortcutMime = "application/internet-shortcut";

// Что нашлось на странице. thumbnail - jpeg; пусто - картинки нет.
struct Preview {
    std::string title;
    std::string description;
    Bytes thumbnail;

    void wipe() noexcept {
        domain::secureWipe(title);
        domain::secureWipe(description);
        domain::secureWipe(thumbnail);
    }
    [[nodiscard]] bool empty() const noexcept {
        return title.empty() && description.empty() && thumbnail.empty();
    }
};

struct PreviewJob {
    std::weak_ptr<VaultSession> session;
    EntryId id = 0;
    std::string url;
};

[[nodiscard]] std::string_view firstChars(std::string_view text, std::size_t chars) {
    std::size_t pos = 0;
    for (std::size_t n = 0; n < chars && pos < text.size(); ++n) {
        const auto len = domain::detail::utf8SequenceLength(text, pos);
        pos += len == 0 ? 1 : len;
    }
    return text.substr(0, pos);
}

// Название страницы -> имя записи: не длиннее 200 символов, без запрещенных в имени.
// Разделители заголовков ("HTML | MDN", "GitHub: ...") становятся тире, а не '_'.
[[nodiscard]] std::string previewName(std::string_view title) {
    std::string readable;
    readable.reserve(title.size());
    for (std::size_t i = 0; i < title.size(); ++i) {
        const char c = title[i];
        switch (c) {
        case '|':
        case '/':
        case '\\':
            readable.push_back('-');
            break;
        case ':': // "GitHub: Where" -> "GitHub - Where", "10:30" -> "10-30"
            if (i + 1 < title.size() && title[i + 1] == ' ' && !readable.empty() &&
                readable.back() != ' ') {
                readable.push_back(' ');
            }
            readable.push_back('-');
            break;
        case '"':
            readable.push_back('\'');
            break;
        case '?':
        case '*':
        case '<':
        case '>':
            break;
        default:
            if (c == ' ' && !readable.empty() && readable.back() == ' ') {
                break; // "a : b" -> "a - b", без двойных пробелов
            }
            readable.push_back(c);
        }
    }
    return domain::sanitizeName(firstChars(readable, kMaxPreviewNameChars));
}

// Тип содержимого без параметров, в нижнем регистре.
[[nodiscard]] std::string mediaType(std::string_view contentType) {
    auto type = std::string(domain::detail::trim(contentType.substr(0, contentType.find(';'))));
    for (auto& c : type) {
        c = domain::detail::asciiLower(c);
    }
    return type;
}

// Расширение последнего сегмента пути адреса, в нижнем регистре.
[[nodiscard]] std::string urlExtension(std::string_view url) {
    if (const auto scheme = url.find("://"); scheme != std::string_view::npos) {
        url = url.substr(scheme + 3);
    }
    url = url.substr(0, url.find_first_of("?#"));
    const auto slash = url.find('/');
    url = slash == std::string_view::npos ? std::string_view{} : url.substr(slash);
    url = url.substr(url.rfind('/') + 1);
    auto ext = std::string(domain::detail::extensionOf(url));
    for (auto& c : ext) {
        c = domain::detail::asciiLower(c);
    }
    return ext;
}

// ico и svg миниатюрой не станут (stb их не читает) - даже не скачиваем.
[[nodiscard]] bool skipImageUrl(std::string_view url) {
    const auto ext = urlExtension(url);
    return ext == ".ico" || ext == ".svg" || ext == ".svgz";
}

// Растр, который умеет Thumbnailer: png/jpg/webp/gif - по типу ответа, а если тип ничего не
// говорит (нет или octet-stream), по расширению адреса. Любой другой тип (html страницы ошибки,
// svg, ico) - не картинка.
[[nodiscard]] bool isRasterImage(std::string_view url, std::string_view contentType) {
    const auto type = mediaType(contentType);
    if (type.starts_with("image/")) {
        return type == "image/png" || type == "image/jpeg" || type == "image/jpg" ||
               type == "image/pjpeg" || type == "image/webp" || type == "image/gif";
    }
    if (!type.empty() && type != "application/octet-stream" && type != "binary/octet-stream") {
        return false;
    }
    const auto ext = urlExtension(url);
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".webp" || ext == ".gif";
}

// Ответ мог быть страницей: тип не задан, html/xml или текст.
[[nodiscard]] bool mayBeHtml(std::string_view contentType) {
    const auto type = mediaType(contentType);
    return type.empty() || type.find("html") != std::string::npos ||
           type.find("xml") != std::string::npos || type.starts_with("text/");
}

class LinksServiceImpl final : public LinksService {
public:
    LinksServiceImpl(Ports ports, std::shared_ptr<SettingsService> settings)
        : ports_(ports), settings_(std::move(settings)) {
        worker_ = std::jthread([this](std::stop_token stop) { run(stop); });
    }

    ~LinksServiceImpl() override {
        worker_.request_stop(); // задания, не начатые к этому моменту, пропадают
        worker_.join();
        for (auto& job : queue_) {
            domain::secureWipe(job.url);
        }
    }

    Result<CreateLinksResult> create(const Lease& lease, const CreateLinksCmd& cmd) override {
        if (cmd.links.size() > kMaxLinksPerRequest) {
            return fail(Error::Code::InvalidArgument, "Не больше 10 000 ссылок за один раз");
        }
        auto ctx = contextOf(lease);
        if (!ctx) {
            return std::unexpected(ctx.error());
        }
        auto catalog = catalogOf(*ctx, ports_.store);
        if (!catalog) {
            return std::unexpected(catalog.error());
        }
        if (cmd.parent && (*catalog)->find(*cmd.parent) == nullptr) {
            return fail(Error::Code::NotFound, "Объект не найден");
        }

        CreateLinksResult result;
        struct Created {
            EntryId id = 0;
            std::string url;
        };
        std::vector<Created> created;
        {
            auto uow = ports_.store.begin();
            if (!uow) {
                return std::unexpected(uow.error());
            }
            auto& tx = **uow;
            PathFolders paths(ports_, ctx->session, ctx->sealer, cmd.parent, *catalog);
            // родитель -> нормализованный адрес -> ссылка; строится по снимку, когда родитель
            // встретился впервые
            std::map<std::optional<EntryId>, std::unordered_map<std::string, EntryId>> known;
            const auto linksOf = [&](std::optional<EntryId> parent) -> auto& {
                auto [it, fresh] = known.try_emplace(parent);
                if (fresh) {
                    for (const auto child : (*catalog)->childrenOf(parent)) {
                        const auto& entry = (*catalog)->find(child)->entry;
                        if (entry.meta.kind != Kind::Link) {
                            continue;
                        }
                        if (auto normal = domain::normalizeUrl(entry.meta.url)) {
                            it->second.try_emplace(std::move(*normal), child);
                        }
                    }
                }
                return it->second;
            };
            // Номер, на котором остановился подбор суффикса для основы имени у родителя: тысячи
            // ссылок с одного хоста не пересчитывают все занятые номера заново.
            std::map<std::pair<std::optional<EntryId>, std::string>, int> suffixes;
            const auto now = domain::toUnixMillis(ports_.clock.now());

            for (const auto& link : cmd.links) {
                auto url = std::string(domain::detail::trim(link.url));
                const auto normal = domain::normalizeUrl(url);
                const auto dirs = splitDirectories(link.path);
                if (!normal || !dirs) {
                    result.invalid.push_back(link.url);
                    continue;
                }
                auto parent = paths.resolve(*dirs, &tx);
                if (!parent) {
                    return std::unexpected(parent.error());
                }
                auto& urls = linksOf(*parent);
                if (const auto found = urls.find(*normal); found != urls.end()) {
                    result.existing.push_back({std::move(url), found->second});
                    continue;
                }

                std::string name =
                    link.name ? std::string(domain::detail::trim(*link.name)) : std::string{};
                const bool named = !name.empty();
                name = domain::sanitizeName(named ? name : domain::urlHost(url));
                // у ссылки нет расширения: "example.com (2)", а не "example (2).com"
                auto& siblings = paths.siblingsOf(*parent);
                name = uniqueName(name, true, siblings.taken(),
                                  &suffixes[{*parent, foldForSearch(name)}]);

                auto id = insertLink(tx, *ctx->sealer, *parent, name, url, named, now);
                if (!id) {
                    return std::unexpected(id.error());
                }
                urls.emplace(*normal, *id);
                siblings.add(foldForSearch(name), {*id, false});
                created.push_back({*id, std::move(url)});
            }
            if (created.empty()) {
                return result; // ни ссылок, ни папок: откат ничего не стоит
            }
            if (auto st = tx.commit(); !st) {
                return std::unexpected(st.error());
            }
        }
        ctx->session.invalidateCatalog();

        if (settings_->get().linkPreviews) {
            std::vector<PreviewJob> jobs;
            jobs.reserve(created.size());
            for (const auto& link : created) {
                ctx->session.setPreviewPending(link.id, true);
                jobs.push_back({lease.weakSession(), link.id, link.url});
            }
            enqueue(std::move(jobs));
        }
        // Заново: у новых записей нужны childCount и унаследованные теги, и каталог все равно
        // понадобится следующему запросу.
        auto fresh = catalogOf(*ctx, ports_.store);
        if (!fresh) {
            return std::unexpected(fresh.error());
        }
        result.created.reserve(created.size());
        for (const auto& link : created) {
            if (const auto* node = (*fresh)->find(link.id)) { // могли удалить, пока создавали
                result.created.push_back(describeEntry(ctx->session, **fresh, *node));
            }
        }
        return result;
    }

    Result<domain::Entry> refreshPreview(const Lease& lease, EntryId id) override {
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
        if (node->entry.meta.kind != Kind::Link) {
            return fail(Error::Code::InvalidArgument, "Предпросмотр есть только у ссылок");
        }
        auto url = node->entry.meta.url;
        auto preview = loadPreview(url, [&lease] { return lease.cancelled(); });
        if (!preview) {
            domain::secureWipe(url);
            return std::unexpected(preview.error());
        }
        Result<domain::Entry> entry = fail(Error::Code::Cancelled, "Сейф заблокирован");
        if (!lease.cancelled()) {
            entry = writePreview(lease, id, url, *preview);
        }
        domain::secureWipe(url);
        preview->wipe();
        return entry;
    }

    bool previewPending(const Lease& lease, EntryId id) const override {
        const auto* session = lease.session();
        return session != nullptr && session->previewPending(id);
    }

    void drain() override {
        std::unique_lock lock(mutex_);
        idle_.wait(lock, [this] { return queue_.empty() && !busy_; });
    }

private:
    Result<EntryId> insertLink(domain::UnitOfWork& uow, const Sealer& sealer,
                               std::optional<EntryId> parent, const std::string& name,
                               const std::string& url, bool nameByUser, std::int64_t now) {
        domain::EntryRecord record;
        record.parentId = parent;
        record.isFolder = false;
        auto id = uow.entries().insert(record);
        if (!id) {
            return std::unexpected(id.error());
        }
        domain::EntryMeta meta;
        meta.kind = Kind::Link;
        meta.mime = std::string(kShortcutMime);
        meta.url = url;
        meta.createdAt = meta.modifiedAt = now;
        meta.nameByUser = nameByUser;
        auto encName = sealer.sealName(*id, name);
        if (!encName) {
            return std::unexpected(encName.error());
        }
        auto encMeta = sealer.sealMeta(*id, meta);
        if (!encMeta) {
            return std::unexpected(encMeta.error());
        }
        if (auto st = uow.entries().updateSealed(*id, *encName, *encMeta); !st) {
            return std::unexpected(st.error());
        }
        return *id;
    }

    // Загрузка предпросмотра: сеть, разбор, миниатюра. Ни аренды, ни транзакции. cancelled -
    // сейф закрылся: к картинкам уже не идем.
    Result<Preview> loadPreview(const std::string& url, const std::function<bool()>& cancelled) {
        const auto started = ports_.clock.monotonic();
        const auto elapsed = [&] { return ports_.clock.monotonic() - started; };

        domain::FetchRequest request;
        request.url = url;
        request.maxBytes = kMaxPageBytes;
        request.timeout = std::chrono::duration_cast<std::chrono::milliseconds>(kPageTimeout);
        auto page = ports_.fetcher.fetch(request);
        domain::secureWipe(request.url);
        if (!page) {
            const auto& message = page.error().message;
            return fail(Error::Code::PreviewFailed,
                        message.empty() ? "Страница не загрузилась" : message);
        }
        if (!mayBeHtml(page->contentType)) {
            domain::secureWipe(page->body);
            return fail(Error::Code::PreviewFailed, "Адрес ведет не на веб-страницу");
        }
        auto parsed = domain::parseLinkPreview(domain::asChars(page->body),
                                               page->finalUrl.empty() ? url : page->finalUrl);
        domain::secureWipe(page->body);
        domain::secureWipe(page->finalUrl);

        Preview preview;
        preview.title = std::move(parsed.title);
        preview.description = std::move(parsed.description);
        std::size_t tried = 0;
        for (const auto& imageUrl : parsed.imageUrls) {
            if (tried == kMaxImageCandidates || cancelled()) {
                break;
            }
            if (skipImageUrl(imageUrl)) {
                continue;
            }
            const auto left = kPreviewBudget - elapsed();
            if (left < kTimeoutSlack + 1s) {
                break; // 15 с не хватит даже на самый короткий запрос
            }
            ++tried;
            domain::FetchRequest imageRequest;
            imageRequest.url = imageUrl;
            imageRequest.maxBytes = kMaxImageBytes;
            imageRequest.timeout = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::min<std::chrono::nanoseconds>(kImageTimeout, left - kTimeoutSlack));
            auto image = ports_.fetcher.fetch(imageRequest);
            domain::secureWipe(imageRequest.url);
            if (!image) {
                continue;
            }
            const bool usable = !image->body.empty() && image->body.size() <= kMaxImageBytes &&
                                isRasterImage(imageUrl, image->contentType);
            auto thumbnail = usable ? ports_.thumbnailer.make(image->body) : Result<Bytes>(Bytes{});
            domain::secureWipe(image->body);
            domain::secureWipe(image->finalUrl);
            if (thumbnail && !thumbnail->empty()) {
                preview.thumbnail = std::move(*thumbnail);
                break;
            }
        }
        for (auto& imageUrl : parsed.imageUrls) {
            domain::secureWipe(imageUrl);
        }
        if (preview.empty()) {
            return fail(Error::Code::PreviewFailed, "На странице нет данных для предпросмотра");
        }
        return preview;
    }

    // Запись найденного одной транзакцией под арендой. Название и описание - только если их не
    // задавал пользователь; новая миниатюра заменяет старую. Нет записи -> NotFound, адрес
    // изменился, пока шла загрузка -> PreviewFailed (записывать в такую ссылку нечего).
    Result<domain::Entry> writePreview(const Lease& lease, EntryId id, const std::string& url,
                                       Preview& preview) {
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
        const Sealer& sealer = *ctx->sealer;

        // Миниатюра пишется до транзакции: blobs() и UnitOfWork - разные транзакции.
        std::optional<BlobId> newThumb;
        if (!preview.thumbnail.empty()) {
            auto blob = writeBlob(ports_.store.blobs(), sealer, ctx->session.chunkSize(),
                                  KeyPurpose::Thumbnails, preview.thumbnail);
            if (!blob) {
                return std::unexpected(blob.error());
            }
            newThumb = *blob;
        }
        std::optional<BlobId> oldThumb;
        auto entry = commitPreview(sealer, **catalog, id, url, preview, newThumb, oldThumb);
        if (!entry) {
            if (newThumb) {
                (void)ports_.store.blobs().discard(*newThumb);
            }
            return entry;
        }
        ctx->session.invalidateCatalog();
        if (oldThumb) {
            (void)ports_.store.compact(); // старая миниатюра освободила место в файле
        }
        entry->childCount = node->childCount;
        entry->inheritedTags = (*catalog)->inheritedTags(id);
        entry->previewPending = ctx->session.previewPending(id);
        return entry;
    }

    Result<domain::Entry> commitPreview(const Sealer& sealer, const Catalog& catalog, EntryId id,
                                        const std::string& url, const Preview& preview,
                                        std::optional<BlobId> newThumb,
                                        std::optional<BlobId>& oldThumb) {
        auto uow = ports_.store.begin();
        if (!uow) {
            return std::unexpected(uow.error());
        }
        auto record = (*uow)->entries().get(id);
        if (!record) {
            return std::unexpected(record.error());
        }
        auto opened = sealer.openEntry(*record);
        if (!opened) {
            return std::unexpected(opened.error());
        }
        domain::Entry entry = std::move(*opened);
        if (entry.meta.kind != Kind::Link || entry.meta.url != url) {
            return fail(Error::Code::PreviewFailed, "Адрес ссылки изменился во время загрузки");
        }

        if (!entry.meta.nameByUser && !preview.title.empty()) {
            std::unordered_set<std::string> taken;
            for (const auto sibling : catalog.childrenOf(entry.parentId)) {
                if (sibling != id) {
                    taken.insert(catalog.find(sibling)->folded);
                }
            }
            entry.name = uniqueName(previewName(preview.title), true, taken);
            auto encName = sealer.sealName(id, entry.name);
            if (!encName) {
                return std::unexpected(encName.error());
            }
            record->encName = std::move(*encName);
        }
        if (!entry.meta.descriptionByUser && !preview.description.empty()) {
            entry.meta.description = preview.description;
        }
        if (newThumb) {
            oldThumb = record->thumbBlobId;
            record->thumbBlobId = newThumb;
            entry.meta.thumbBlobId = newThumb;
        }
        auto encMeta = sealer.sealMeta(id, entry.meta);
        if (!encMeta) {
            return std::unexpected(encMeta.error());
        }
        record->encMeta = std::move(*encMeta);
        if (auto st = (*uow)->entries().update(*record); !st) {
            return std::unexpected(st.error());
        }
        if (newThumb) {
            if (auto st = (*uow)->blobs().promote(*newThumb); !st) {
                return std::unexpected(st.error());
            }
            if (oldThumb) {
                if (auto st = (*uow)->blobs().remove(*oldThumb); !st) {
                    return std::unexpected(st.error());
                }
            }
        }
        if (auto st = (*uow)->commit(); !st) {
            return std::unexpected(st.error());
        }
        return entry;
    }

    // очередь и воркер

    void enqueue(std::vector<PreviewJob> jobs) {
        {
            std::scoped_lock lock(mutex_);
            dropDeadLocked();
            for (auto& job : jobs) {
                queue_.push_back(std::move(job));
            }
        }
        wake_.notify_one();
    }

    // Задания сейфа, который уже заблокирован (или закрывается): сети им не видать.
    void dropDeadLocked() {
        std::erase_if(queue_, [](PreviewJob& job) {
            const auto session = job.session.lock();
            const bool dead = !session || session->cancelled();
            if (dead) {
                domain::secureWipe(job.url);
            }
            return dead;
        });
    }

    void run(std::stop_token stop) {
        std::unique_lock lock(mutex_);
        for (;;) {
            if (!wake_.wait(lock, stop, [this] { return !queue_.empty(); }) ||
                stop.stop_requested()) {
                return; // остановка: оставшиеся задания пропадают
            }
            dropDeadLocked();
            if (queue_.empty()) {
                idle_.notify_all();
                continue;
            }
            auto job = std::move(queue_.front());
            queue_.pop_front();
            busy_ = true;
            lock.unlock();
            process(job);
            lock.lock();
            busy_ = false;
            idle_.notify_all();
        }
    }

    // Никакая ошибка не должна остановить воркер: предпросмотр - удобство, не данные.
    void process(PreviewJob& job) noexcept {
        try {
            const auto alive = [&job] {
                const auto session = job.session.lock();
                return session && !session->cancelled();
            };
            if (alive()) {
                auto preview = loadPreview(job.url, [&alive] { return !alive(); });
                if (preview) {
                    if (auto lease = leaseFrom(job.session)) {
                        (void)writePreview(*lease, job.id, job.url, *preview);
                    }
                    preview->wipe();
                }
            }
            if (const auto session = job.session.lock()) {
                session->setPreviewPending(job.id, false);
            }
        } catch (...) {
        }
        domain::secureWipe(job.url);
    }

    Ports ports_;
    std::shared_ptr<SettingsService> settings_;
    std::mutex mutex_; // queue_, busy_
    std::condition_variable_any wake_;
    std::condition_variable idle_;
    std::deque<PreviewJob> queue_;
    bool busy_ = false;
    std::jthread worker_; // последним: останавливается раньше остальных полей
};

} // namespace

std::shared_ptr<LinksService> makeLinksService(Ports ports,
                                               std::shared_ptr<SettingsService> settings) {
    return std::make_shared<LinksServiceImpl>(ports, std::move(settings));
}

} // namespace safebox::app
