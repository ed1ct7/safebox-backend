// LinksService (пачка ссылок, предпросмотр в фоне и по требованию) и SettingsService.
// Сеть - FakePageFetcher; чтобы застать воркер посреди загрузки, запросы держит Gate
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>

#include "app_fixture.hpp"
#include "path_folders.hpp"
#include "safebox/domain/model/rules.hpp"

using namespace safebox;
using namespace safebox::test;
using namespace std::chrono_literals;
using Code = domain::Error::Code;
using Names = std::vector<std::string>;

namespace {

constexpr std::string_view kPage = "https://example.com/post";
constexpr std::string_view kCover = "https://example.com/cover.png";

std::string metaTag(std::string_view property, std::string_view content) {
    return "<meta property=\"" + std::string(property) + "\" content=\"" + std::string(content) +
           "\">";
}

std::string pageWith(std::string_view head) {
    return "<!doctype html><html><head><meta charset=\"utf-8\">" + std::string(head) +
           "</head><body><p>тело</p></body></html>";
}

domain::Bytes imageBytes(std::size_t size = 7) {
    return domain::toBytes(std::string(size, 'P'));
}

// Держит запросы страниц, пока тест не отпустит: то, что происходит, пока сеть "думает". Состояние
// общее с обработчиком, поэтому переживает порядок уничтожения; деструктор отпускает запросы, чтобы
// упавший тест не оставил воркер ждать вечно.
class Gate {
public:
    explicit Gate(FakePageFetcher& fetcher) : state_(std::make_shared<State>()) {
        fetcher.onFetch = [state = state_](const domain::FetchRequest&) {
            std::unique_lock lock(state->mutex);
            ++state->waiting;
            state->cv.notify_all();
            state->cv.wait(lock, [&] { return state->open; });
            --state->waiting;
        };
    }
    Gate(const Gate&) = delete;
    Gate& operator=(const Gate&) = delete;
    ~Gate() { open(); }

    // Воркер стоит в запросе.
    void waitForFetch() {
        std::unique_lock lock(state_->mutex);
        state_->cv.wait(lock, [&] { return state_->waiting > 0; });
    }
    void open() {
        {
            std::scoped_lock lock(state_->mutex);
            state_->open = true;
        }
        state_->cv.notify_all();
    }

private:
    struct State {
        std::mutex mutex;
        std::condition_variable cv;
        int waiting = 0;
        bool open = false;
    };
    std::shared_ptr<State> state_;
};

struct LinksFixture : AppFixture {
    // previews == false: воркер не запускается, тесты create видят только его результат
    explicit LinksFixture(bool previews = false) {
        if (!previews) {
            REQUIRE(services.settings->update({false}).has_value());
        }
    }

    app::CreateLinksResult create(const app::UnlockResult& s, std::vector<app::NewLink> links,
                                  std::optional<domain::EntryId> parent = std::nullopt) {
        auto made = services.links->create(lease(s), {parent, std::move(links)});
        REQUIRE(made.has_value());
        return std::move(*made);
    }

    domain::Entry get(const app::UnlockResult& s, domain::EntryId id) {
        auto entry = services.entries->get(lease(s), id);
        REQUIRE(entry.has_value());
        return *entry;
    }

    Names namesIn(const app::UnlockResult& s, std::optional<domain::EntryId> parent) {
        auto listing = services.entries->list(lease(s), parent);
        REQUIRE(listing.has_value());
        Names out;
        for (const auto& entry : listing->entries) {
            out.push_back(entry.name);
        }
        return out;
    }

    // Страница по адресу с og-разметкой и, если задана, картинкой обложки.
    void publish(const std::string& url, std::string_view title, std::string_view description,
                 std::string_view image = {}) {
        std::string head;
        if (!title.empty()) {
            head += metaTag("og:title", title);
        }
        if (!description.empty()) {
            head += metaTag("og:description", description);
        }
        if (!image.empty()) {
            head += metaTag("og:image", image);
        }
        fetcher.addHtml(url, pageWith(head));
    }
};

app::NewLink link(std::string url, std::optional<std::string> name = std::nullopt,
                  std::string path = {}) {
    return {std::move(url), std::move(name), std::move(path)};
}

} // namespace

// create

TEST_CASE("links are created without content, named after the host or as given",
          "[links][create]") {
    LinksFixture f;
    auto s = f.createSafe();

    auto made = f.create(s, {link("  https://Example.com/a  "),
                             link("http://sub.site.org:8080/x?y=1", "Моя ссылка")});
    REQUIRE(made.created.size() == 2);
    CHECK(made.existing.empty());
    CHECK(made.invalid.empty());

    const auto& byHost = made.created[0];
    CHECK(byHost.name == "example.com");
    CHECK(byHost.meta.kind == domain::Kind::Link);
    CHECK(byHost.meta.url == "https://Example.com/a"); // как ввел пользователь, без пробелов
    CHECK_FALSE(byHost.meta.nameByUser);
    CHECK_FALSE(byHost.meta.blobId.has_value()); // содержимого нет
    CHECK(byHost.meta.size == 0);
    CHECK(byHost.meta.mime == "application/internet-shortcut");
    CHECK_FALSE(byHost.hasThumbnail());
    CHECK_FALSE(byHost.parentId.has_value());
    CHECK(byHost.meta.createdAt == byHost.meta.modifiedAt);
    CHECK_FALSE(byHost.previewPending); // настройка выключена

    const auto& given = made.created[1];
    CHECK(given.name == "Моя ссылка");
    CHECK(given.meta.nameByUser);
    CHECK(given.meta.url == "http://sub.site.org:8080/x?y=1");

    // запись в сейфе такая же, и содержимого у нее нет
    const auto stored = f.get(s, byHost.id);
    CHECK(stored.name == "example.com");
    CHECK(stored.meta.url == byHost.meta.url);
    auto opened =
        f.services.importExport->openContent(f.lease(s), byHost.id, app::ContentVariant::Original);
    REQUIRE_FALSE(opened.has_value());
    CHECK(opened.error().code == Code::NotFound);
    auto thumb =
        f.services.importExport->openContent(f.lease(s), byHost.id, app::ContentVariant::Thumbnail);
    REQUIRE_FALSE(thumb.has_value());
    CHECK(thumb.error().code == Code::NotFound);
}

TEST_CASE("addresses that are not http(s) links, or have a bad path, are reported back as typed",
          "[links][create]") {
    LinksFixture f;
    auto s = f.createSafe();

    const Names bad = {"ftp://example.com/file",
                       "example.com",
                       "",
                       "   ",
                       "http://",
                       "https:///path",
                       "javascript:alert(1)",
                       "https://exa mple.com",
                       "file:///C:/secret.txt"};
    std::vector<app::NewLink> links;
    for (const auto& url : bad) {
        links.push_back(link(url));
    }
    links.push_back(link("https://ok.example/", std::nullopt, "../вверх")); // выход из корня
    links.push_back(link("https://ok.example/2", std::nullopt, "a/../b"));
    links.push_back(link("https://ok.example/"));
    auto made = f.create(s, links);

    REQUIRE(made.created.size() == 1);
    CHECK(made.created[0].meta.url == "https://ok.example/");
    CHECK(made.existing.empty());
    Names expected = bad;
    expected.push_back("https://ok.example/");
    expected.push_back("https://ok.example/2");
    CHECK(made.invalid == expected);
    CHECK(f.namesIn(s, std::nullopt) == Names{"ok.example"}); // ни папок, ни лишних записей
}

TEST_CASE("a link that is already there is reported as existing", "[links][create]") {
    LinksFixture f;
    auto s = f.createSafe();
    const auto first = f.create(s, {link("https://Example.com/page/")}).created.at(0);

    // регистр схемы и хоста, порт по умолчанию, "/" в конце и utm_ не в счет
    auto again = f.create(s, {link("HTTPS://EXAMPLE.COM/page"),
                              link("https://example.com:443/page?utm_source=news&utm_medium=x"),
                              link("https://example.com/page#top"), // фрагмент - другая ссылка
                              link("https://example.com/page?id=1")});
    CHECK(again.invalid.empty());
    REQUIRE(again.existing.size() == 2);
    CHECK(again.existing[0].url == "HTTPS://EXAMPLE.COM/page");
    CHECK(again.existing[0].entryId == first.id);
    CHECK(again.existing[1].url == "https://example.com:443/page?utm_source=news&utm_medium=x");
    CHECK(again.existing[1].entryId == first.id);
    REQUIRE(again.created.size() == 2);
    CHECK(again.created[0].name == "example.com (2)");
    CHECK(again.created[1].name == "example.com (3)");
}

TEST_CASE("the same address twice in one request is created once", "[links][create]") {
    LinksFixture f;
    auto s = f.createSafe();
    auto made = f.create(s, {link("https://a.example/x"), link("https://A.example/x/"),
                             link("https://b.example/y")});
    REQUIRE(made.created.size() == 2);
    REQUIRE(made.existing.size() == 1);
    CHECK(made.existing[0].url == "https://A.example/x/");
    CHECK(made.existing[0].entryId == made.created[0].id); // созданная этим же запросом
}

TEST_CASE("a duplicate is looked up in the target folder only", "[links][create]") {
    LinksFixture f;
    auto s = f.createSafe();
    REQUIRE(f.importFiles(s, {{"Закладки/note.txt", "n"}}).imported == 1);
    const auto folder = f.entryNamed(s, "Закладки");
    const auto inFolder = f.create(s, {link("https://site.example/")}, folder.id);
    REQUIRE(inFolder.created.size() == 1);
    CHECK(inFolder.created[0].parentId == folder.id);

    // корень - другой родитель: там ссылки еще нет
    CHECK(f.create(s, {link("https://site.example/")}).created.size() == 1);
    // а в папке уже есть - и когда папку называют путем от корня
    auto byPath = f.create(s, {link("https://site.example/", std::nullopt, "закладки")});
    CHECK(byPath.created.empty());
    REQUIRE(byPath.existing.size() == 1);
    CHECK(byPath.existing[0].entryId == inFolder.created[0].id);
    CHECK(f.namesIn(s, folder.id) == Names{"note.txt", "site.example"});
}

TEST_CASE("an imported .url shortcut counts as an existing link", "[links][create]") {
    LinksFixture f;
    auto s = f.createSafe();
    REQUIRE(
        f.importFiles(s, {{"site.url", "[InternetShortcut]\r\nURL=https://imported.example/x\r\n"}})
            .imported == 1);
    const auto imported = f.entryNamed(s, "site.url");
    REQUIRE(imported.meta.kind == domain::Kind::Link);

    auto made = f.create(s, {link("https://IMPORTED.example/x/")});
    CHECK(made.created.empty());
    REQUIRE(made.existing.size() == 1);
    CHECK(made.existing[0].entryId == imported.id);
}

TEST_CASE("folders are made from the path by the import rules", "[links][create][path]") {
    LinksFixture f;
    auto s = f.createSafe();

    auto made = f.create(s, {link("https://a.example/", "A", "Закладки/Работа"),
                             link("https://b.example/", "B", "закладки/работа"), // та же папка
                             link("https://c.example/", "C", "Закладки\\Личное"),
                             link("https://d.example/", "D", "//Закладки/./Работа/"),
                             link("https://e.example/", "E", "")});
    REQUIRE(made.created.size() == 5);
    CHECK(f.namesIn(s, std::nullopt) == Names{"Закладки", "E"});
    const auto bookmarks = f.entryNamed(s, "Закладки");
    CHECK(f.namesIn(s, bookmarks.id) == Names{"Личное", "Работа"});
    const auto work = f.entryNamed(s, "Работа", bookmarks.id);
    CHECK(f.namesIn(s, work.id) == Names{"A", "B", "D"});
    CHECK(f.namesIn(s, f.entryNamed(s, "Личное", bookmarks.id).id) == Names{"C"});
    CHECK(made.created[0].parentId == work.id);
    CHECK(made.created[4].parentId == std::nullopt);
    CHECK(f.get(s, bookmarks.id).childCount == 2);

    // существующие папки используются снова, а не задваиваются
    auto more = f.create(s, {link("https://f.example/", "F", "Закладки/Работа")});
    CHECK(more.created.at(0).parentId == work.id);
    CHECK(f.namesIn(s, std::nullopt) == Names{"Закладки", "E"});
}

TEST_CASE("path segments are cleaned like import paths", "[links][create][path]") {
    LinksFixture f;
    auto s = f.createSafe();
    auto made = f.create(s, {link("https://a.example/", "A", "a:b/c*d")});
    REQUIRE(made.created.size() == 1);
    CHECK(f.namesIn(s, std::nullopt) == Names{"a_b"});
    CHECK(f.namesIn(s, f.entryNamed(s, "a_b").id) == Names{"c_d"});
}

TEST_CASE("links can go under any entry, and paths count from it", "[links][create][path]") {
    LinksFixture f;
    auto s = f.createSafe();
    REQUIRE(f.importFiles(s, {{"фото.jpg", fakeJpeg(600)}}).imported == 1);
    const auto photo = f.entryNamed(s, "фото.jpg");

    auto made =
        f.create(s, {link("https://a.example/", "A"), link("https://b.example/", "B", "Источники")},
                 photo.id);
    REQUIRE(made.created.size() == 2);
    CHECK(made.created[0].parentId == photo.id);
    CHECK(f.namesIn(s, photo.id) == Names{"Источники", "A"});
    CHECK(f.get(s, photo.id).childCount == 2);
    CHECK(f.namesIn(s, std::nullopt) == Names{"фото.jpg"});
}

TEST_CASE("names are unique among the neighbours whatever their kind", "[links][create][names]") {
    LinksFixture f;
    auto s = f.createSafe();
    REQUIRE(f.importFiles(s, {{"note.txt", "n"}, {"Example.com", "e"}}).imported == 2);

    auto made = f.create(s, {link("https://example.com/1"), link("https://example.com/2"),
                             link("https://other.example/", "note.txt"),
                             link("https://third.example/", "NOTE.TXT")});
    REQUIRE(made.created.size() == 4);
    // расширения у ссылки нет: суффикс в конце, а не перед точкой
    CHECK(made.created[0].name == "example.com (2)"); // "Example.com" уже занят файлом
    CHECK(made.created[1].name == "example.com (3)");
    CHECK(made.created[2].name == "note.txt (2)");
    CHECK(made.created[3].name == "NOTE.TXT (3)");
    CHECK(made.created[3].meta.nameByUser);
}

TEST_CASE("many links from one host get numbered names without slowing down",
          "[links][create][names]") {
    LinksFixture f;
    auto s = f.createSafe();
    constexpr std::size_t kCount = 4000;
    std::vector<app::NewLink> links;
    for (std::size_t i = 0; i < kCount; ++i) {
        links.push_back(link("https://github.com/user/repo" + std::to_string(i)));
    }
    auto made = f.create(s, links);
    REQUIRE(made.created.size() == kCount);
    CHECK(made.created[0].name == "github.com");
    CHECK(made.created[1].name == "github.com (2)");
    CHECK(made.created[kCount - 1].name == "github.com (" + std::to_string(kCount) + ")");
    std::unordered_set<std::string> names;
    for (const auto& entry : made.created) {
        names.insert(entry.name);
    }
    CHECK(names.size() == kCount);
}
TEST_CASE("a given name is cleaned, an empty one means the host", "[links][create][names]") {
    LinksFixture f;
    auto s = f.createSafe();
    std::string tooLong;
    for (int i = 0; i < 300; ++i) {
        tooLong += "я"; // 600 байт
    }
    auto made = f.create(s, {link("https://a.example/", "  A/B: C?  "),
                             link("https://b.example/", "   "), link("https://c.example/", ""),
                             link("https://d.example/", tooLong), link("https://[::1]:8080/x")});
    REQUIRE(made.created.size() == 5);
    CHECK(made.created[0].name == "A_B_ C_");
    CHECK(made.created[0].meta.nameByUser);
    CHECK(made.created[1].name == "b.example");
    CHECK_FALSE(made.created[1].meta.nameByUser);
    CHECK(made.created[2].name == "c.example");
    CHECK(domain::validateName(made.created[3].name).has_value());
    CHECK(made.created[3].name.size() <= domain::kMaxNameBytes);
    CHECK(made.created[3].meta.nameByUser);
    CHECK(made.created[4].name == "[__1]"); // ":" в имени нельзя
    CHECK(made.created[4].meta.url == "https://[::1]:8080/x");
}

TEST_CASE("new links carry the tags inherited from the folder", "[links][create]") {
    LinksFixture f;
    auto s = f.createSafe();
    REQUIRE(f.importFiles(s, {{"Папка/x.txt", "x"}}).imported == 1);
    const auto folder = f.entryNamed(s, "Папка");
    const auto tag = f.tag(s, "Тема", "Работа");
    REQUIRE(f.tagEntries(s, {folder.id}, {tag}, true) == 1);

    auto made = f.create(s, {link("https://a.example/")}, folder.id);
    REQUIRE(made.created.size() == 1);
    REQUIRE(made.created[0].inheritedTags.size() == 1);
    CHECK(made.created[0].inheritedTags[0].tagId == tag);
    CHECK(made.created[0].inheritedTags[0].fromId == folder.id);
    CHECK(f.get(s, folder.id).childCount == 2);
}

TEST_CASE("a request is refused whole when it is too big or the parent is missing",
          "[links][create]") {
    LinksFixture f;
    auto s = f.createSafe();

    std::vector<app::NewLink> tooMany;
    for (std::size_t i = 0; i <= app::kMaxLinksPerRequest; ++i) {
        tooMany.push_back(link("https://h" + std::to_string(i) + ".example/", std::nullopt, "П"));
    }
    auto refused = f.services.links->create(f.lease(s), {std::nullopt, tooMany});
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == Code::InvalidArgument);

    auto missing = f.services.links->create(
        f.lease(s), {9999, {link("https://a.example/", std::nullopt, "Папка")}});
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code == Code::NotFound);

    CHECK(f.namesIn(s, std::nullopt).empty()); // ни ссылок, ни папки "П" или "Папка"
    auto none = f.create(s, {});
    CHECK(none.created.empty());
    CHECK(none.existing.empty());
    CHECK(none.invalid.empty());
}

TEST_CASE("the biggest allowed request goes through in one piece", "[links][create]") {
    LinksFixture f;
    auto s = f.createSafe();
    std::vector<app::NewLink> links;
    for (std::size_t i = 0; i < app::kMaxLinksPerRequest; ++i) {
        links.push_back(link("https://h" + std::to_string(i) + ".example/", std::nullopt,
                             i % 2 == 0 ? "Чётные" : "Нечётные"));
    }
    auto made = f.create(s, links);
    CHECK(made.created.size() == app::kMaxLinksPerRequest);
    CHECK(made.existing.empty());
    CHECK(made.invalid.empty());
    CHECK(f.namesIn(s, std::nullopt) == Names{"Нечётные", "Чётные"});
    CHECK(f.get(s, f.entryNamed(s, "Чётные").id).childCount == app::kMaxLinksPerRequest / 2);
}

TEST_CASE("links survive a lock and reopen with their fields", "[links][create]") {
    LinksFixture f;
    auto s = f.createSafe();
    const auto made = f.create(s, {link("https://a.example/x", "Имя", "Папка")});
    f.services.safe->lock();

    auto again = f.services.safe->unlock({"test", "secret1"});
    REQUIRE(again.has_value());
    const auto folder = f.entryNamed(*again, "Папка");
    const auto reopened = f.entryNamed(*again, "Имя", folder.id);
    CHECK(reopened.id == made.created[0].id);
    CHECK(reopened.meta.kind == domain::Kind::Link);
    CHECK(reopened.meta.url == "https://a.example/x");
    CHECK(reopened.meta.nameByUser);
    CHECK_FALSE(reopened.meta.blobId.has_value());
}

TEST_CASE("a link without content goes into a zip as a shortcut", "[links][create][zip]") {
    LinksFixture f;
    auto s = f.createSafe();
    const auto made = f.create(s, {link("https://a.example/x", "Сайт", "Закладки")});
    REQUIRE(made.created.size() == 1);
    const auto folder = f.entryNamed(s, "Закладки");

    domain::MemorySink sink;
    REQUIRE(f.services.importExport->exportZip(f.lease(s), folder.id, sink).has_value());
    const auto& files = f.zip.archives.back().files;
    const auto shortcut = files.find("Закладки/Сайт.url");
    REQUIRE(shortcut != files.end());
    CHECK(domain::asChars(shortcut->second) == "[InternetShortcut]\r\nURL=https://a.example/x\r\n");
}

// предпросмотр в фоне

TEST_CASE("the worker fills in name, description and thumbnail", "[links][preview]") {
    LinksFixture f(true);
    auto s = f.createSafe();
    Gate gate(f.fetcher);
    f.publish(std::string(kPage), "Пост &amp; котики", "Про котиков", "/cover.png");
    f.fetcher.addImage(std::string(kCover), imageBytes(7));

    auto made = f.create(s, {link(std::string(kPage))});
    REQUIRE(made.created.size() == 1);
    const auto id = made.created[0].id;
    CHECK(made.created[0].name == "example.com"); // название придет потом

    // пока страница грузится, запись помечена везде, где она видна
    gate.waitForFetch();
    CHECK(made.created[0].previewPending);
    CHECK(f.services.links->previewPending(f.lease(s), id));
    CHECK(f.get(s, id).previewPending);
    auto listing = f.services.entries->list(f.lease(s), std::nullopt);
    REQUIRE(listing.has_value());
    CHECK(listing->entries.at(0).previewPending);
    auto hits = f.services.search->search(f.lease(s), {.text = "example"});
    REQUIRE(hits.has_value());
    REQUIRE(hits->size() == 1);
    CHECK(hits->at(0).entry.previewPending);

    gate.open();
    f.services.links->drain();

    const auto entry = f.get(s, id);
    CHECK_FALSE(entry.previewPending);
    CHECK_FALSE(f.services.links->previewPending(f.lease(s), id));
    CHECK(entry.name == "Пост & котики");
    CHECK(entry.meta.description == "Про котиков");
    CHECK(entry.hasThumbnail());
    CHECK(f.readContent(s, id, app::ContentVariant::Thumbnail) == "THUMB:7");
    CHECK_FALSE(entry.meta.nameByUser); // не пользовательские: следующий предпросмотр их обновит
    CHECK_FALSE(entry.meta.descriptionByUser);
    CHECK(entry.meta.url == kPage);
    CHECK(f.fetcher.requested() == Names{std::string(kPage), std::string(kCover)});

    // лимиты, с которыми ходили в сеть
    const auto requests = f.fetcher.requests();
    REQUIRE(requests.size() == 2);
    CHECK(requests[0].maxBytes == (1u << 20));
    CHECK(requests[0].timeout == 8000ms);
    CHECK(requests[1].maxBytes == (5u << 20));
    CHECK(requests[1].timeout == 5000ms);
}

TEST_CASE("the preview leaves what the user set alone", "[links][preview]") {
    LinksFixture f(true);
    auto s = f.createSafe();
    Gate gate(f.fetcher);
    f.publish(std::string(kPage), "Чужое название", "Чужое описание", "/cover.png");
    f.fetcher.addImage(std::string(kCover), imageBytes());

    auto made = f.create(s, {link(std::string(kPage), "Моё название")});
    const auto id = made.created.at(0).id;
    gate.waitForFetch();
    REQUIRE(
        f.services.entries->update(f.lease(s), id, {.description = "моё описание"}).has_value());
    gate.open();
    f.services.links->drain();

    const auto entry = f.get(s, id);
    CHECK(entry.name == "Моё название");
    CHECK(entry.meta.description == "моё описание");
    CHECK(entry.meta.nameByUser);
    CHECK(entry.meta.descriptionByUser);
    CHECK(entry.hasThumbnail()); // а вот картинка своя у предпросмотра
}

TEST_CASE("only the parts the page has are written", "[links][preview]") {
    LinksFixture f(true);
    auto s = f.createSafe();
    f.publish(std::string(kPage), "", "Только описание");
    auto made = f.create(s, {link(std::string(kPage))});
    f.services.links->drain();

    const auto entry = f.get(s, made.created.at(0).id);
    CHECK(entry.name == "example.com"); // названия на странице нет
    CHECK(entry.meta.description == "Только описание");
    CHECK_FALSE(entry.hasThumbnail());
    CHECK(f.fetcher.calls == 1); // картинок не было - ходили только за страницей
}

TEST_CASE("page titles become clean, unique names", "[links][preview]") {
    LinksFixture f(true);
    auto s = f.createSafe();
    std::string long250;
    for (int i = 0; i < 250; ++i) {
        long250 += "ж";
    }
    f.publish("https://one.example/", "Одно и то же", "");
    f.publish("https://two.example/", "одно и то же", ""); // регистр не в счет
    f.publish("https://three.example/", "HTML | MDN: a/b?", ""); // разделители - тире
    f.publish("https://four.example/", long250, "");

    f.create(s, {link("https://one.example/")});
    f.services.links->drain(); // по одной: порядок задания фиксирует, кто первый занял имя
    f.create(s, {link("https://two.example/")});
    f.services.links->drain();
    f.create(s, {link("https://three.example/"), link("https://four.example/")});
    f.services.links->drain();

    auto names = f.namesIn(s, std::nullopt);
    std::sort(names.begin(), names.end());
    REQUIRE(names.size() == 4);
    CHECK(std::ranges::find(names, "Одно и то же") != names.end());
    CHECK(std::ranges::find(names, "одно и то же (2)") != names.end());
    CHECK(std::ranges::find(names, "HTML - MDN - a-b") != names.end());
    const auto cut = std::ranges::find_if(
        names, [](const std::string& name) { return name.starts_with("жжж"); });
    REQUIRE(cut != names.end());
    CHECK(domain::utf8Length(*cut) <= 200);
    CHECK(domain::validateName(*cut).has_value());
}

TEST_CASE("a page that does not load leaves the link as it was", "[links][preview]") {
    LinksFixture f(true);
    auto s = f.createSafe();
    f.publish("https://good.example/", "Хорошая", "");
    // bad.example не задан: "Сайт недоступен"; воркер идет дальше
    auto made = f.create(s, {link("https://bad.example/"), link("https://good.example/")});
    f.services.links->drain();

    const auto bad = f.get(s, made.created.at(0).id);
    CHECK(bad.name == "bad.example");
    CHECK_FALSE(bad.previewPending);
    CHECK(bad.meta.description.empty());
    CHECK(f.get(s, made.created.at(1).id).name == "Хорошая");
}

TEST_CASE("a locked safe drops the result of a preview that was on its way", "[links][preview]") {
    LinksFixture f(true);
    auto s = f.createSafe();
    Gate gate(f.fetcher);
    f.publish(std::string(kPage), "Название", "Описание", "/cover.png");
    f.fetcher.addImage(std::string(kCover), imageBytes());
    const auto id = f.create(s, {link(std::string(kPage))}).created.at(0).id;

    gate.waitForFetch();
    // воркер сидит в сети без аренды и без мьютекса хранилища: lock не ждет сайт
    auto locked = std::async(std::launch::async, [&] { f.services.safe->lock(); });
    const auto status = locked.wait_for(10s);
    gate.open();
    REQUIRE(status == std::future_status::ready);
    f.services.links->drain();

    auto again = f.services.safe->unlock({"test", "secret1"});
    REQUIRE(again.has_value());
    const auto entry = f.get(*again, id);
    CHECK(entry.name == "example.com");
    CHECK(entry.meta.description.empty());
    CHECK_FALSE(entry.hasThumbnail());
    CHECK_FALSE(entry.previewPending); // очередь и признак остались в прошлой сессии
    CHECK_FALSE(f.services.links->previewPending(f.lease(*again), id));
    CHECK(f.fetcher.requested() == Names{std::string(kPage)}); // к картинке уже не пошли
    CHECK(f.store.blobCount(f.safePath()) == 0);
}

TEST_CASE("the queue of a locked safe is dropped without going to the network",
          "[links][preview]") {
    LinksFixture f(true);
    auto s = f.createSafe();
    Gate gate(f.fetcher);
    for (const auto* url : {"https://a.example/", "https://b.example/", "https://c.example/"}) {
        f.publish(url, "Название", "");
    }
    f.create(s,
             {link("https://a.example/"), link("https://b.example/"), link("https://c.example/")});
    gate.waitForFetch(); // первое задание в сети, два других ждут

    auto locked = std::async(std::launch::async, [&] { f.services.safe->lock(); });
    const auto status = locked.wait_for(10s);
    gate.open();
    REQUIRE(status == std::future_status::ready);
    f.services.links->drain();

    CHECK(f.fetcher.requested() == Names{"https://a.example/"});
}

TEST_CASE("nothing is written into a link whose address changed meanwhile", "[links][preview]") {
    LinksFixture f(true);
    auto s = f.createSafe();
    Gate gate(f.fetcher);
    f.publish(std::string(kPage), "Название", "Описание", "/cover.png");
    f.fetcher.addImage(std::string(kCover), imageBytes());
    const auto id = f.create(s, {link(std::string(kPage))}).created.at(0).id;

    gate.waitForFetch();
    REQUIRE(
        f.services.entries->update(f.lease(s), id, {.url = "https://other.example/"}).has_value());
    gate.open();
    f.services.links->drain();

    const auto entry = f.get(s, id);
    CHECK(entry.meta.url == "https://other.example/");
    CHECK(entry.name == "example.com");
    CHECK(entry.meta.description.empty());
    CHECK_FALSE(entry.hasThumbnail());
    CHECK_FALSE(entry.previewPending);
    // скачанная миниатюра не осталась в файле недописанным блобом
    CHECK(f.store.blobCount(f.safePath()) == 0);
    CHECK(f.store.pendingBlobs(f.safePath()) == 0);
}

TEST_CASE("nothing is queued while link previews are switched off", "[links][preview][settings]") {
    LinksFixture f(true);
    auto s = f.createSafe();
    f.publish(std::string(kPage), "Название", "Описание");
    REQUIRE(f.services.settings->update({false}).has_value());

    auto made = f.create(s, {link(std::string(kPage))});
    f.services.links->drain();
    CHECK(f.fetcher.calls == 0);
    CHECK_FALSE(made.created.at(0).previewPending);
    const auto entry = f.get(s, made.created.at(0).id);
    CHECK(entry.name == "example.com");
    CHECK_FALSE(entry.previewPending);

    // включили - следующие ссылки снова идут в очередь
    REQUIRE(f.services.settings->update({true}).has_value());
    f.publish("https://next.example/", "Следующая", "");
    const auto next = f.create(s, {link("https://next.example/")}).created.at(0);
    f.services.links->drain();
    CHECK(f.get(s, next.id).name == "Следующая");
    CHECK(f.fetcher.calls == 1);
}

// предпросмотр по требованию

TEST_CASE("refreshPreview loads the preview right away, whatever the setting says",
          "[links][refresh]") {
    LinksFixture f; // фоновый предпросмотр выключен
    auto s = f.createSafe();
    f.publish(std::string(kPage), "Название", "Описание", "/cover.png");
    f.fetcher.addImage(std::string(kCover), imageBytes(9));
    const auto id = f.create(s, {link(std::string(kPage))}).created.at(0).id;
    CHECK(f.fetcher.calls == 0);

    auto refreshed = f.services.links->refreshPreview(f.lease(s), id);
    REQUIRE(refreshed.has_value());
    CHECK(refreshed->id == id);
    CHECK(refreshed->name == "Название");
    CHECK(refreshed->meta.description == "Описание");
    CHECK(refreshed->hasThumbnail());
    CHECK_FALSE(refreshed->previewPending);
    CHECK(f.readContent(s, id, app::ContentVariant::Thumbnail) == "THUMB:9");
    CHECK(f.get(s, id).name == "Название");
    CHECK(f.fetcher.calls == 2);
}

TEST_CASE("a new thumbnail replaces the old one in the file", "[links][refresh]") {
    LinksFixture f;
    auto s = f.createSafe();
    f.publish(std::string(kPage), "Название", "", "/cover.png");
    f.fetcher.addImage(std::string(kCover), imageBytes(5));
    const auto id = f.create(s, {link(std::string(kPage))}).created.at(0).id;

    REQUIRE(f.services.links->refreshPreview(f.lease(s), id).has_value());
    CHECK(f.readContent(s, id, app::ContentVariant::Thumbnail) == "THUMB:5");
    CHECK(f.store.blobCount(f.safePath()) == 1);

    f.fetcher.addImage(std::string(kCover), imageBytes(11));
    REQUIRE(f.services.links->refreshPreview(f.lease(s), id).has_value());
    CHECK(f.readContent(s, id, app::ContentVariant::Thumbnail) == "THUMB:11");
    CHECK(f.store.blobCount(f.safePath()) == 1); // старая миниатюра ушла
    CHECK(f.store.pendingBlobs(f.safePath()) == 0);

    // страница без картинки миниатюру не трогает
    f.publish(std::string(kPage), "Новое название", "");
    REQUIRE(f.services.links->refreshPreview(f.lease(s), id).has_value());
    CHECK(f.get(s, id).name == "Новое название");
    CHECK(f.readContent(s, id, app::ContentVariant::Thumbnail) == "THUMB:11");
    CHECK(f.store.blobCount(f.safePath()) == 1);
}

TEST_CASE("refreshPreview refuses what is not a link, and reports why a page failed",
          "[links][refresh]") {
    LinksFixture f;
    auto s = f.createSafe();
    REQUIRE(f.importFiles(s, {{"Папка/файл.txt", "x"}}).imported == 1);
    const auto id = f.create(s, {link("https://a.example/")}).created.at(0).id;
    const auto blobs = f.store.blobCount(f.safePath()); // блоб файла
    const auto refresh = [&](domain::EntryId what) {
        return f.services.links->refreshPreview(f.lease(s), what);
    };

    CHECK(refresh(9999).error().code == Code::NotFound);
    const auto folder = refresh(f.entryNamed(s, "Папка").id);
    REQUIRE_FALSE(folder.has_value());
    CHECK(folder.error().code == Code::InvalidArgument);
    const auto file = refresh(f.entryNamed(s, "файл.txt", f.entryNamed(s, "Папка").id).id);
    REQUIRE_FALSE(file.has_value());
    CHECK(file.error().code == Code::InvalidArgument);

    // сайт недоступен: причина от загрузчика доезжает до пользователя
    auto offline = refresh(id);
    REQUIRE_FALSE(offline.has_value());
    CHECK(offline.error().code == Code::PreviewFailed);
    CHECK(offline.error().message == "Сайт недоступен");
    f.fetcher.addError("https://a.example/", "Адрес во внутренней сети");
    CHECK(refresh(id).error().message == "Адрес во внутренней сети");

    // не страница
    f.fetcher.add("https://a.example/",
                  {200, "https://a.example/", "application/pdf", domain::toBytes("%PDF-1.4")});
    auto pdf = refresh(id);
    REQUIRE_FALSE(pdf.has_value());
    CHECK(pdf.error().code == Code::PreviewFailed);

    // страница без единого нужного тега
    f.fetcher.addHtml("https://a.example/", pageWith("<meta name=\"robots\" content=\"none\">"));
    auto empty = refresh(id);
    REQUIRE_FALSE(empty.has_value());
    CHECK(empty.error().code == Code::PreviewFailed);

    // ссылка осталась как была
    CHECK(f.get(s, id).name == "a.example");
    CHECK(f.store.blobCount(f.safePath()) == blobs);
}

TEST_CASE("a page that gave no picture still gives its name", "[links][refresh][image]") {
    LinksFixture f;
    auto s = f.createSafe();
    f.publish(std::string(kPage), "Название", "", "/cover.png"); // картинки нет: "Сайт недоступен"
    const auto id = f.create(s, {link(std::string(kPage))}).created.at(0).id;

    auto refreshed = f.services.links->refreshPreview(f.lease(s), id);
    REQUIRE(refreshed.has_value());
    CHECK(refreshed->name == "Название");
    CHECK_FALSE(refreshed->hasThumbnail());

    // картинка не превратилась в миниатюру (не изображение): то же самое
    f.fetcher.addImage(std::string(kCover), imageBytes());
    f.thumbnailer.enabled = false;
    f.publish(std::string(kPage), "Название 2", "", "/cover.png");
    refreshed = f.services.links->refreshPreview(f.lease(s), id);
    REQUIRE(refreshed.has_value());
    CHECK(refreshed->name == "Название 2");
    CHECK_FALSE(refreshed->hasThumbnail());
}

TEST_CASE("the picture is taken from og:image, then the touch icon, then the icon",
          "[links][refresh][image]") {
    LinksFixture f;
    auto s = f.createSafe();
    const auto id = f.create(s, {link(std::string(kPage))}).created.at(0).id;
    const auto refresh = [&] {
        auto entry = f.services.links->refreshPreview(f.lease(s), id);
        REQUIRE(entry.has_value());
        return *entry;
    };
    const std::string head = metaTag("og:title", "Заголовок") +
                             "<link rel=\"apple-touch-icon\" href=\"/touch.png\">"
                             "<link rel=\"icon\" href=\"/icon.png\">" +
                             metaTag("og:image", "https://cdn.example/og.jpg");

    // все три на месте: берется og:image, остальных не трогаем
    f.fetcher.addHtml(std::string(kPage), pageWith(head));
    f.fetcher.addImage("https://cdn.example/og.jpg", imageBytes(3), "image/jpeg");
    f.fetcher.addImage("https://example.com/touch.png", imageBytes(4));
    f.fetcher.addImage("https://example.com/icon.png", imageBytes(5));
    CHECK(refresh().hasThumbnail());
    CHECK(f.readContent(s, id, app::ContentVariant::Thumbnail) == "THUMB:3");
    CHECK(f.fetcher.requested() == Names{std::string(kPage), "https://cdn.example/og.jpg"});

    // og:image не скачивается - apple-touch-icon
    f.fetcher.addError("https://cdn.example/og.jpg", "Сайт недоступен");
    refresh();
    CHECK(f.readContent(s, id, app::ContentVariant::Thumbnail) == "THUMB:4");

    // и он не скачивается - icon
    f.fetcher.addError("https://example.com/touch.png", "Сайт недоступен");
    refresh();
    CHECK(f.readContent(s, id, app::ContentVariant::Thumbnail) == "THUMB:5");
}

TEST_CASE("only png, jpg, webp and gif small enough are taken as pictures",
          "[links][refresh][image]") {
    LinksFixture f;
    auto s = f.createSafe();
    const auto id = f.create(s, {link(std::string(kPage))}).created.at(0).id;
    // Дала ли эта загрузка новую миниатюру (у записи может остаться прежняя).
    const auto thumbnailed = [&](std::string_view head) {
        f.fetcher.addHtml(std::string(kPage),
                          pageWith("<title>Страница</title>" + std::string(head)));
        const auto before = f.get(s, id).meta.thumbBlobId;
        auto entry = f.services.links->refreshPreview(f.lease(s), id);
        REQUIRE(entry.has_value());
        return entry->meta.thumbBlobId != before;
    };
    const auto icon = [](std::string_view href) {
        return "<link rel=\"icon\" href=\"" + std::string(href) + "\">";
    };

    // ico и svg не скачиваются вовсе (favicon.ico - тем более)
    CHECK_FALSE(thumbnailed(icon("/favicon.ico") + icon("/logo.svg")));
    CHECK(f.fetcher.requested() == Names{std::string(kPage)});

    // тип ответа решает, если он что-то говорит: значок .png, а отвечают как html или svg
    f.fetcher.add("https://example.com/a.png", {200, "", "text/html", imageBytes()});
    CHECK_FALSE(thumbnailed(icon("/a.png")));
    f.fetcher.addImage("https://example.com/b", imageBytes(), "image/svg+xml");
    CHECK_FALSE(thumbnailed(icon("/b")));
    f.fetcher.addImage("https://example.com/c", imageBytes(), "image/x-icon");
    CHECK_FALSE(thumbnailed(icon("/c")));

    // тип ничего не говорит (нет или octet-stream) - расширение адреса
    f.fetcher.addImage("https://example.com/d.gif", imageBytes(), "application/octet-stream");
    CHECK(thumbnailed(icon("/d.gif")));
    f.fetcher.addImage("https://example.com/e", imageBytes(), "application/octet-stream");
    CHECK_FALSE(thumbnailed(icon("/e")));

    // по типу: jpeg/webp без расширения
    f.fetcher.addImage("https://example.com/f", imageBytes(), "image/jpeg; charset=binary");
    CHECK(thumbnailed(icon("/f")));
    f.fetcher.addImage("https://example.com/g", imageBytes(), "IMAGE/WEBP");
    CHECK(thumbnailed(icon("/g")));

    // до 5 МиБ включительно, больше - нет (сам загрузчик обрезал бы и вернул ошибку)
    f.fetcher.addImage("https://example.com/h.png", imageBytes(5u << 20));
    CHECK(thumbnailed(icon("/h.png")));
    f.fetcher.addImage("https://example.com/i.png", imageBytes((5u << 20) + 1));
    CHECK_FALSE(thumbnailed(icon("/i.png")));
    f.fetcher.addImage("https://example.com/j.png", {});
    CHECK_FALSE(thumbnailed(icon("/j.png")));
}

TEST_CASE("at most three pictures are tried", "[links][refresh][image]") {
    LinksFixture f;
    auto s = f.createSafe();
    const auto id = f.create(s, {link(std::string(kPage))}).created.at(0).id;
    std::string head = "<title>Страница</title>";
    for (const auto* name : {"1", "2", "3", "4"}) {
        head += metaTag("og:image", std::string("https://cdn.example/") + name + ".png");
    }
    f.fetcher.addHtml(std::string(kPage), pageWith(head));
    f.fetcher.addImage("https://cdn.example/4.png", imageBytes()); // до нее не дойдем

    auto refreshed = f.services.links->refreshPreview(f.lease(s), id);
    REQUIRE(refreshed.has_value());
    CHECK_FALSE(refreshed->hasThumbnail());
    CHECK(f.fetcher.requested() == Names{std::string(kPage), "https://cdn.example/1.png",
                                         "https://cdn.example/2.png", "https://cdn.example/3.png"});
}

TEST_CASE("refreshPreview stays within 15 seconds even if the network is slow",
          "[links][refresh][timeout]") {
    LinksFixture f;
    auto s = f.createSafe();
    const auto id = f.create(s, {link(std::string(kPage))}).created.at(0).id;
    f.publish(std::string(kPage), "Название", "", "/cover.png");
    f.fetcher.addImage(std::string(kCover), imageBytes());

    // сколько "проработала" страница -> с каким таймаутом (и был ли вообще) ушел запрос картинки
    const auto imageTimeout =
        [&](std::chrono::seconds pageTook) -> std::optional<std::chrono::milliseconds> {
        f.fetcher.onFetch = [&, pageTook](const domain::FetchRequest& request) {
            if (request.url == kPage) {
                f.clock.advance(pageTook);
            }
        };
        const auto before = f.fetcher.requests().size();
        REQUIRE(f.services.links->refreshPreview(f.lease(s), id).has_value());
        const auto requests = f.fetcher.requests();
        if (requests.size() == before + 1) {
            return std::nullopt; // картинку не запрашивали
        }
        REQUIRE(requests.size() == before + 2);
        CHECK(requests[before].timeout == 8000ms); // страница всегда с полным таймаутом
        return requests.back().timeout;
    };

    CHECK(imageTimeout(0s) == 5000ms);
    CHECK(imageTimeout(6s) == 5000ms);          // осталось 9 с, за вычетом запаса 4 с - как раз 5
    CHECK(imageTimeout(9s) == 2000ms);          // осталось 6 с
    CHECK(imageTimeout(10s) == 1000ms);         // 5 с: запас 4 с и еще секунда
    CHECK_FALSE(imageTimeout(11s).has_value()); // 4 с - на картинку уже не хватит
}

TEST_CASE("the link changing during a refresh is not written to", "[links][refresh]") {
    LinksFixture f;
    auto s = f.createSafe();
    const auto id = f.create(s, {link(std::string(kPage))}).created.at(0).id;
    f.publish(std::string(kPage), "Название", "");
    f.fetcher.onFetch = [&](const domain::FetchRequest&) {
        REQUIRE(f.services.entries->update(f.lease(s), id, {.url = "https://other.example/"})
                    .has_value());
    };

    auto refreshed = f.services.links->refreshPreview(f.lease(s), id);
    REQUIRE_FALSE(refreshed.has_value());
    CHECK(refreshed.error().code == Code::PreviewFailed);
    const auto entry = f.get(s, id);
    CHECK(entry.name == "example.com");
    CHECK(entry.meta.url == "https://other.example/");
}

TEST_CASE("an imported .url link can have a preview too", "[links][refresh]") {
    LinksFixture f;
    auto s = f.createSafe();
    REQUIRE(
        f.importFiles(s, {{"site.url", "[InternetShortcut]\r\nURL=https://example.com/post\r\n"}})
            .imported == 1);
    const auto id = f.entryNamed(s, "site.url").id;
    f.publish(std::string(kPage), "Название", "Описание");

    auto refreshed = f.services.links->refreshPreview(f.lease(s), id);
    REQUIRE(refreshed.has_value());
    CHECK(refreshed->name == "Название");
    CHECK(refreshed->meta.blobId.has_value()); // файл ярлыка остался на месте
    CHECK(f.readContent(s, id).find("[InternetShortcut]") == 0);
}

// пути (общий разбор импорта и ссылок)

TEST_CASE("uniqueName can carry on from the number it stopped at", "[links][names]") {
    std::unordered_set<std::string> taken{"a.b"};
    int resume = 0;
    // без подсказки каждый вызов начинает с двойки, с ней - с места остановки; результат тот же
    for (int expected = 2; expected <= 5; ++expected) {
        const auto slow = app::uniqueName("A.b", true, taken);
        const auto fast = app::uniqueName("A.b", true, taken, &resume);
        CHECK(fast == slow);
        CHECK(fast == "A.b (" + std::to_string(expected) + ")");
        CHECK(resume == expected);
        taken.insert(app::foldForSearch(fast));
    }
    // свободное имя подсказку не трогает
    CHECK(app::uniqueName("другое", true, taken, &resume) == "другое");
    CHECK(resume == 5);
    // подсказка мимо (меньше двух) не пропускает свободные номера
    int stale = -7;
    CHECK(app::uniqueName("A.b", true, {"a.b"}, &stale) == "A.b (2)");
    CHECK(stale == 2);
}

TEST_CASE("paths of links and imported files are split the same way", "[links][path]") {
    const auto dirs = app::splitDirectories("a//b/./c\\d:e");
    REQUIRE(dirs.has_value());
    CHECK(*dirs == Names{"a", "b", "c", "d_e"});
    CHECK(app::splitDirectories("").value().empty());
    CHECK(app::splitDirectories("/").value().empty());
    CHECK_FALSE(app::splitDirectories("..").has_value());
    CHECK_FALSE(app::splitDirectories("a/../b").has_value());

    const auto file = app::splitPath("a/b\\c.txt");
    CHECK(file.valid);
    CHECK(file.dirs == Names{"a", "b"});
    CHECK(file.name == "c.txt");
    CHECK(file.clean == "a/b/c.txt");
    CHECK_FALSE(app::splitPath("a/../c.txt").valid);
    CHECK_FALSE(app::splitPath("").valid);
    CHECK_FALSE(app::splitPath("//./").valid);
}

// SettingsService

TEST_CASE("settings work without any safe, start from the store and persist", "[settings]") {
    AppFixture f;                                   // сейф не создан и не открыт
    CHECK(f.services.settings->get().linkPreviews); // по умолчанию предпросмотр включен

    auto saved = f.services.settings->update({false});
    REQUIRE(saved.has_value());
    CHECK_FALSE(saved->linkPreviews);
    CHECK_FALSE(f.services.settings->get().linkPreviews);
    CHECK(f.settingsStore.saves == 1);
    CHECK_FALSE(f.settingsStore.saved.linkPreviews);

    // новый набор сервисов читает то, что лежит в хранилище
    auto second = app::makeServices(
        {*f.crypto, f.store, f.thumbnailer, f.zip, f.clock, f.fetcher, f.settingsStore}, f.config);
    CHECK_FALSE(second.settings->get().linkPreviews);
}

TEST_CASE("settings that cannot be read or saved are handled", "[settings]") {
    AppFixture f;
    f.settingsStore.saved.linkPreviews = false;
    f.settingsStore.failLoad = true;
    auto unreadable = app::makeServices(
        {*f.crypto, f.store, f.thumbnailer, f.zip, f.clock, f.fetcher, f.settingsStore}, f.config);
    CHECK(unreadable.settings->get().linkPreviews); // не прочитали - значения по умолчанию

    f.settingsStore.failSave = true;
    auto failed = f.services.settings->update({false});
    REQUIRE_FALSE(failed.has_value());
    CHECK(failed.error().code == Code::IoError);
    CHECK(f.services.settings->get().linkPreviews); // прежние настройки остались
    CHECK(f.settingsStore.saves == 0);
}
