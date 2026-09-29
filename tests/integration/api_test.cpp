// Весь стек на настоящих адаптерах, HttpServer на свободном порту, запросы через настоящий http-клиент
#include <catch2/catch_test_macros.hpp>

#include <httplib.h>
#include <miniz.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <random>
#include <thread>

#include "../infra/temp_dir.hpp"
#include "safebox/app/factory.hpp"
#include "safebox/http/server.hpp"
#include "safebox/infra/factories.hpp"

using namespace safebox;
using Json = nlohmann::json;

namespace {

constexpr std::uint32_t kChunk = 64 * 1024;

class NoAssets final : public http::AssetProvider {
public:
    std::optional<http::Asset> find(std::string_view) const override { return std::nullopt; }
    bool empty() const override { return true; }
};

std::string randomData(std::size_t n, std::uint32_t seed) {
    std::mt19937 rng(seed);
    std::string out(n, '\0');
    for (auto& c : out) {
        c = static_cast<char>(rng());
    }
    return out;
}

std::string makePng(int w, int h) {
    std::vector<unsigned char> pixels(static_cast<std::size_t>(w * h * 3));
    for (std::size_t i = 0; i < pixels.size(); ++i) {
        pixels[i] = static_cast<unsigned char>(i * 7);
    }
    std::size_t size = 0;
    void* png = tdefl_write_image_to_png_file_in_memory(pixels.data(), w, h, 3, &size);
    REQUIRE(png != nullptr);
    std::string out(static_cast<const char*>(png), size);
    mz_free(png);
    return out;
}

std::string fakeMp4(std::size_t n) {
    auto data = randomData(n, 99);
    const std::string header("\x00\x00\x00\x18"
                             "ftypisom\x00\x00\x02\x00"
                             "isomiso2",
                             24);
    data.replace(0, header.size(), header);
    return data;
}

std::map<std::string, std::string> unzip(const std::string& archive) {
    mz_zip_archive zip{};
    REQUIRE(mz_zip_reader_init_mem(&zip, archive.data(), archive.size(), 0));
    std::map<std::string, std::string> files;
    for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&zip); ++i) {
        mz_zip_archive_file_stat stat{};
        REQUIRE(mz_zip_reader_file_stat(&zip, i, &stat));
        if (stat.m_is_directory) {
            files[stat.m_filename] = "<dir>";
            continue;
        }
        std::size_t size = 0;
        void* data = mz_zip_reader_extract_to_heap(&zip, i, &size, 0);
        REQUIRE(data != nullptr);
        files[stat.m_filename].assign(static_cast<const char*>(data), size);
        mz_free(data);
    }
    mz_zip_reader_end(&zip);
    return files;
}

// Композиция как в main.cpp, но с минимальным Argon2id и куском 64 КиБ.
struct Stack {
    explicit Stack(const std::filesystem::path& safeDir) {
        app::AppConfig config;
        config.kdf = domain::kMinimalKdf;
        config.chunkSize = kChunk;
        config.defaultDirectory = safeDir;
        services = app::makeServices({*crypto, *store, *thumbnailer, *zip, *clock}, config);
        http::HttpConfig httpConfig;
        httpConfig.port = 0;
        server = std::make_unique<http::HttpServer>(services, assets, httpConfig);
        auto bound = server->bind();
        REQUIRE(bound.has_value());
        port = *bound;
        thread = std::thread([this] { server->run(); });
        for (int i = 0; i < 200; ++i) {
            if (auto r = client().Get("/health"); r && r->status == 200) {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        server->stop();
        thread.join();
        FAIL("сервер не поднялся");
    }

    ~Stack() {
        server->stop();
        thread.join();
        services.safe->lock(); // "остановка процесса -> lock"
        server.reset();
        services = {};
    }

    [[nodiscard]] httplib::Client client() const {
        httplib::Client c("127.0.0.1", port);
        c.set_read_timeout(std::chrono::seconds(30));
        return c;
    }

    std::unique_ptr<domain::CryptoSuite> crypto = infra::makeSodiumCrypto();
    std::unique_ptr<domain::VaultStore> store = infra::makeSqliteVaultStore();
    std::unique_ptr<domain::Thumbnailer> thumbnailer = infra::makeStbThumbnailer();
    std::unique_ptr<domain::ZipWriter> zip = infra::makeStreamZipWriter();
    std::unique_ptr<domain::Clock> clock = infra::makeSystemClock();
    app::Services services;
    NoAssets assets;
    std::unique_ptr<http::HttpServer> server;
    std::thread thread;
    int port = 0;
};

httplib::Headers bearer(const std::string& token) {
    return {{"Authorization", "Bearer " + token}};
}

Json post(httplib::Client& c, const std::string& path, const Json& body, int expected,
          const httplib::Headers& headers = {}) {
    auto r = c.Post(path, headers, body.dump(), "application/json");
    REQUIRE(r);
    INFO(path << " -> " << r->body);
    REQUIRE(r->status == expected);
    return r->body.empty() ? Json() : Json::parse(r->body);
}

Json get(httplib::Client& c, const std::string& path, const std::string& token) {
    auto r = c.Get(path, bearer(token));
    REQUIRE(r);
    INFO(path << " -> " << r->body);
    REQUIRE(r->status == 200);
    return Json::parse(r->body);
}

Json byName(const Json& listing, const std::string& name) {
    const auto& entries = listing["entries"];
    const auto it = std::find_if(entries.begin(), entries.end(),
                                 [&](const Json& e) { return e["name"] == name; });
    INFO("запись " << name);
    REQUIRE(it != entries.end());
    return *it;
}

httplib::Headers headers(std::initializer_list<std::pair<std::string, std::string>> items) {
    httplib::Headers out;
    for (const auto& [key, value] : items) {
        out.emplace(key, value);
    }
    return out;
}

} // namespace

TEST_CASE("full user flow over HTTP: create, import, browse, stream, zip, lock, transfer",
          "[integration][UF]") {
    test::TempDir dir;
    const auto png = makePng(640, 480);
    const auto video = fakeMp4(300'000); // ~4,6 куска по 64 КиБ
    const std::string doc = "Документ\nстрока 2\n";
    std::string safePath;

    {
        Stack stack(dir.path());
        auto c = stack.client();

        // создание
        auto created = c.Post(
            "/api/v1/safe/create",
            Json{{"path", "Интеграция"}, {"password", "пароль-1"}, {"confirm", "пароль-1"}}.dump(),
            "application/json");
        REQUIRE(created);
        REQUIRE(created->status == 201);
        const auto session = Json::parse(created->body);
        const auto token = session["token"].get<std::string>();
        safePath = session["safe"]["path"].get<std::string>();
        const auto cookie = created->get_header_value("Set-Cookie")
                                .substr(0, created->get_header_value("Set-Cookie").find(';'));
        CHECK(cookie.starts_with("sbx_media="));

        // импорт с вложенными папками
        httplib::UploadFormDataItems items = {
            {"file", png, "Отпуск/фото.png", "image/png"},
            {"file", video, "Отпуск/Вложенная/видео.mp4", "video/mp4"},
            {"file", doc, "документ.txt", "text/plain"},
        };
        auto imported = c.Post("/api/v1/import", bearer(token), items);
        REQUIRE(imported);
        INFO(imported->body);
        REQUIRE(imported->status == 200);
        CHECK(Json::parse(imported->body)["imported"] == 3);
        CHECK(Json::parse(imported->body)["failed"] == 0);

        // повторный импорт того же - все уже есть, папки не задваиваются
        auto again = c.Post("/api/v1/import", bearer(token), items);
        REQUIRE(again);
        REQUIRE(again->status == 200);
        CHECK(Json::parse(again->body)["imported"] == 0);
        CHECK(Json::parse(again->body)["skipped"] == 3);

        // навигация
        const auto root = get(c, "/api/v1/entries", token);
        REQUIRE(root["entries"].size() == 2);
        const auto trip = byName(root, "Отпуск");
        CHECK(trip["kind"] == "folder");
        const auto tripId = trip["id"].get<std::int64_t>();
        const auto inTrip = get(c, "/api/v1/entries?parentId=" + std::to_string(tripId), token);
        const auto photo = byName(inTrip, "фото.png");
        CHECK(photo["kind"] == "photo");
        CHECK(photo["hasThumbnail"] == true);
        CHECK(photo["size"] == png.size());
        const auto nested = byName(inTrip, "Вложенная");
        const auto inNested =
            get(c, "/api/v1/entries?parentId=" + std::to_string(nested["id"].get<std::int64_t>()),
                token);
        const auto clip = byName(inNested, "видео.mp4");
        CHECK(clip["kind"] == "video");
        CHECK(clip["mime"] == "video/mp4");
        CHECK(inNested["path"].size() == 2);

        // миниатюра по cookie (как <img>)
        auto thumb =
            c.Get("/api/v1/media/" + std::to_string(photo["id"].get<std::int64_t>()) + "/thumbnail",
                  headers({{"Cookie", cookie}}));
        REQUIRE(thumb);
        REQUIRE(thumb->status == 200);
        CHECK(thumb->get_header_value("Content-Type") == "image/jpeg");
        CHECK(static_cast<unsigned char>(thumb->body[0]) == 0xFF);

        // поиск из корня находит файл во вложенной папке
        const auto found =
            get(c, "/api/v1/search?q=%D0%92%D0%98%D0%94%D0%95%D0%9E", token); // "ВИДЕО"
        REQUIRE(found["results"].size() == 1);
        CHECK(found["results"][0]["entry"]["name"] == "видео.mp4");
        CHECK(found["results"][0]["path"][1]["name"] == "Вложенная");

        // Range через границу кусков, по cookie (как <video>)
        const auto clipUrl =
            "/api/v1/media/" + std::to_string(clip["id"].get<std::int64_t>()) + "/content";
        auto part = c.Get(clipUrl, headers({{"Cookie", cookie}, {"Range", "bytes=65530-65545"}}));
        REQUIRE(part);
        CHECK(part->status == 206);
        CHECK(part->body == video.substr(65530, 16));
        CHECK(part->get_header_value("Content-Range") ==
              "bytes 65530-65545/" + std::to_string(video.size()));
        auto whole = c.Get(clipUrl, headers({{"Cookie", cookie}}));
        REQUIRE(whole);
        CHECK(whole->body == video);

        // скачивание файла и zip папки - побайтово
        const auto docId = byName(root, "документ.txt")["id"].get<std::int64_t>();
        auto download =
            c.Get("/api/v1/media/" + std::to_string(docId) + "/download", bearer(token));
        REQUIRE(download);
        CHECK(download->body == doc);
        CHECK(download->get_header_value("Content-Disposition").find("filename*=UTF-8''") !=
              std::string::npos);
        auto zipped = c.Get("/api/v1/media/" + std::to_string(tripId) + "/zip", bearer(token));
        REQUIRE(zipped);
        REQUIRE(zipped->status == 200);
        const auto files = unzip(zipped->body);
        CHECK(files.at("Отпуск/фото.png") == png);
        CHECK(files.at("Отпуск/Вложенная/видео.mp4") == video);
        CHECK(files.at("Отпуск/Вложенная/") == "<dir>");

        // смена пароля
        post(c, "/api/v1/safe/password",
             {{"oldPassword", "пароль-1"}, {"newPassword", "пароль-2"}, {"confirm", "пароль-2"}},
             204, bearer(token));

        // блокировка - все прежнее мертво
        post(c, "/api/v1/safe/lock", Json::object(), 204, bearer(token));
        auto after = c.Get("/api/v1/entries", bearer(token));
        REQUIRE(after);
        CHECK(after->status == 401);
        CHECK(Json::parse(after->body)["error"]["code"] == "unauthorized");
        auto media = c.Get(clipUrl, headers({{"Cookie", cookie}}));
        REQUIRE(media);
        CHECK(media->status == 401);

        auto wrong =
            c.Post("/api/v1/safe/unlock", Json{{"path", safePath}, {"password", "пароль-1"}}.dump(),
                   "application/json");
        REQUIRE(wrong);
        CHECK(wrong->status == 403);
        CHECK(Json::parse(wrong->body)["error"]["code"] == "wrong_password");
    }

    // "Перенос": после lock файл самодостаточен - копия открывается где угодно
    const auto original = domain::pathFromUtf8(safePath);
    for (const char* suffix : {"-journal", "-wal", "-shm"}) {
        auto side = original;
        side += suffix;
        CHECK_FALSE(std::filesystem::exists(side));
    }
    test::TempDir usb;
    const auto copy = usb / u8"копия.safebox";
    std::filesystem::copy_file(original, copy);
    {
        Stack other(usb.path());
        auto c = other.client();
        const auto session =
            post(c, "/api/v1/safe/unlock", {{"path", "копия"}, {"password", "пароль-2"}}, 200);
        const auto token = session["token"].get<std::string>();
        CHECK(session["safe"]["entryCount"] == 5); // 2 папки + 3 файла
        const auto found = get(c, "/api/v1/search?q=%D1%84%D0%BE%D1%82%D0%BE", token); // "фото"
        REQUIRE(found["results"].size() == 1);
        const auto id = found["results"][0]["entry"]["id"].get<std::int64_t>();
        auto content = c.Get("/api/v1/media/" + std::to_string(id) + "/content", bearer(token));
        REQUIRE(content);
        CHECK(content->body == png);
    }
}

TEST_CASE("guards hold on a real socket", "[integration][security]") {
    test::TempDir dir;
    Stack stack(dir.path());
    auto c = stack.client();
    auto rebinding =
        c.Get("/health", headers({{"Host", "attacker.example:" + std::to_string(stack.port)}}));
    REQUIRE(rebinding);
    CHECK(rebinding->status == 403);
    auto crossSite = c.Post("/api/v1/safe/unlock", {{"Origin", "https://attacker.example"}},
                            R"({"path":"x","password":"123456"})", "application/json");
    REQUIRE(crossSite);
    CHECK(crossSite->status == 403);
    auto form = c.Post("/api/v1/safe/unlock", R"(path=x&password=123456)",
                       "application/x-www-form-urlencoded");
    REQUIRE(form);
    CHECK(form->status == 415);
}

TEST_CASE("organizing over HTTP: description, move with conflicts, attachments, persistence",
          "[integration][UF]") {
    test::TempDir dir;
    const auto png = makePng(64, 48);
    Stack stack(dir.path());
    auto c = stack.client();

    const auto session =
        post(c, "/api/v1/safe/create",
             {{"path", "Организация"}, {"password", "пароль-1"}, {"confirm", "пароль-1"}}, 201);
    const auto token = session["token"].get<std::string>();
    httplib::UploadFormDataItems items = {
        {"file", png, "снимок.png", "image/png"},
        {"file", "из Папки", "Папка/заметка.txt", "text/plain"},
        {"file", "из Другой", "Другая/заметка.txt", "text/plain"},
        {"file", "из Цели", "Цель/заметка.txt", "text/plain"},
    };
    auto imported = c.Post("/api/v1/import", bearer(token), items);
    REQUIRE(imported);
    REQUIRE(imported->status == 200);
    CHECK(Json::parse(imported->body)["imported"] == 4);
    CHECK(Json::parse(imported->body)["replaced"] == 0);

    const auto idOf = [](const Json& entry) { return entry["id"].get<std::int64_t>(); };
    const auto listing = [&](std::int64_t parent) {
        return get(c, "/api/v1/entries?parentId=" + std::to_string(parent), token);
    };
    const auto root = get(c, "/api/v1/entries", token);
    const auto shot = byName(root, "снимок.png");
    const auto folder = byName(root, "Папка");
    const auto other = byName(root, "Другая");
    const auto target = byName(root, "Цель");
    CHECK(shot["description"] == "");
    CHECK(shot["childCount"] == 0);
    CHECK(shot["tags"].empty());
    CHECK(shot["sourceModifiedAt"].is_null());
    CHECK(target["childCount"] == 1);

    // описание и имя
    const auto patch = [&](std::int64_t id, const Json& body, int expected) {
        auto r = c.Patch("/api/v1/entries/" + std::to_string(id), bearer(token), body.dump(),
                         "application/json");
        REQUIRE(r);
        INFO(r->body);
        REQUIRE(r->status == expected);
        return Json::parse(r->body);
    };
    CHECK(patch(idOf(shot), {{"description", "закат над морем"}}, 200)["description"] ==
          "закат над морем");
    CHECK(patch(idOf(shot), {{"name", "рассвет.png"}}, 200)["name"] == "рассвет.png");
    CHECK(patch(idOf(shot), {{"name", "a/b"}}, 422)["error"]["code"] == "invalid_argument");
    CHECK(patch(idOf(shot), {{"url", "https://example.com"}}, 422)["error"]["code"] ==
          "invalid_argument"); // не ссылка
    CHECK(patch(idOf(shot), Json::object(), 400)["error"]["code"] == "bad_request");

    // конфликт имен при переносе: план, потом решение
    const auto noteOfOther = byName(listing(idOf(other)), "заметка.txt");
    const auto planned =
        post(c, "/api/v1/entries/move/plan",
             {{"ids", {idOf(noteOfOther)}}, {"parentId", idOf(target)}}, 200, bearer(token));
    REQUIRE(planned["conflicts"].size() == 1);
    CHECK(planned["conflicts"][0]["id"] == idOf(noteOfOther));
    CHECK(planned["conflicts"][0]["existing"]["name"] == "заметка.txt");

    const auto kept =
        post(c, "/api/v1/entries/move", {{"ids", {idOf(noteOfOther)}}, {"parentId", idOf(target)}},
             200, bearer(token));
    CHECK(kept == Json({{"moved", 1}, {"replaced", 0}, {"skipped", 0}}));
    CHECK(listing(idOf(target))["entries"].size() == 2);
    byName(listing(idOf(target)), "заметка (2).txt");

    const auto noteOfFolder = byName(listing(idOf(folder)), "заметка.txt");
    const auto replaced = post(c, "/api/v1/entries/move",
                               {{"ids", {idOf(noteOfFolder)}},
                                {"parentId", idOf(target)},
                                {"resolutions", {{std::to_string(idOf(noteOfFolder)), "replace"}}}},
                               200, bearer(token));
    CHECK(replaced == Json({{"moved", 1}, {"replaced", 1}, {"skipped", 0}}));
    const auto inTarget = listing(idOf(target));
    REQUIRE(inTarget["entries"].size() == 2);
    const auto survivor = byName(inTarget, "заметка.txt");
    CHECK(idOf(survivor) == idOf(noteOfFolder));
    auto content =
        c.Get("/api/v1/media/" + std::to_string(idOf(survivor)) + "/content", bearer(token));
    REQUIRE(content);
    CHECK(content->body == "из Папки");

    // вложения: папка внутрь фото; в дереве слева ее уже нет
    post(c, "/api/v1/entries/move", {{"ids", {idOf(target)}}, {"parentId", idOf(shot)}}, 200,
         bearer(token));
    const auto attachments = listing(idOf(shot));
    CHECK(attachments["parent"]["name"] == "рассвет.png");
    CHECK(attachments["parent"]["childCount"] == 1);
    CHECK(attachments["parent"]["description"] == "закат над морем");
    CHECK(byName(attachments, "Цель")["childCount"] == 2);
    CHECK(attachments["path"].size() == 1);
    const auto folders = get(c, "/api/v1/folders", token);
    std::vector<std::string> tree;
    for (const auto& node : folders["folders"]) {
        tree.push_back(node["name"].get<std::string>());
    }
    CHECK(tree == std::vector<std::string>{"Другая", "Папка"});

    // фото нельзя перенести в свои вложения
    const auto refused =
        post(c, "/api/v1/entries/move", {{"ids", {idOf(shot)}}, {"parentId", idOf(target)}}, 422,
             bearer(token));
    CHECK(refused["error"]["code"] == "invalid_argument");

    // блокировка и повторный вход: все на месте
    post(c, "/api/v1/safe/lock", Json::object(), 204, bearer(token));
    const auto again = post(c, "/api/v1/safe/unlock",
                            {{"path", session["safe"]["path"]}, {"password", "пароль-1"}}, 200);
    const auto token2 = again["token"].get<std::string>();
    const auto reopened = get(c, "/api/v1/entries?parentId=" + std::to_string(idOf(shot)), token2);
    CHECK(reopened["parent"]["description"] == "закат над морем");
    CHECK(byName(reopened, "Цель")["childCount"] == 2);
    CHECK(get(c, "/api/v1/entries/" + std::to_string(idOf(survivor)), token2)["name"] ==
          "заметка.txt");
}
