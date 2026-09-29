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

        // повторный импорт того же: план видит три совпадения, manifest пропускает их -
        // ничего не записано, папки не задваиваются
        const auto plan = post(c, "/api/v1/import/plan",
                               {{"files",
                                 {{{"path", "Отпуск/фото.png"}, {"size", png.size()}},
                                  {{"path", "Отпуск/Вложенная/видео.mp4"}, {"size", video.size()}},
                                  {{"path", "документ.txt"}, {"size", doc.size()}}}}},
                               200, bearer(token));
        CHECK(plan["conflicts"].size() == 3);
        CHECK(plan["newFiles"] == 0);
        httplib::UploadFormDataItems again = {
            {"manifest",
             Json{{"files",
                   {{"Отпуск/фото.png", {{"onConflict", "skip"}}},
                    {"Отпуск/Вложенная/видео.mp4", {{"onConflict", "skip"}}},
                    {"документ.txt", {{"onConflict", "skip"}}}}}}
                 .dump(),
             "", "application/json"},
            items[0],
            items[1],
            items[2],
        };
        auto skipped = c.Post("/api/v1/import", bearer(token), again);
        REQUIRE(skipped);
        REQUIRE(skipped->status == 200);
        CHECK(Json::parse(skipped->body)["imported"] == 0);
        CHECK(Json::parse(skipped->body)["skipped"] == 3);

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
        CHECK(zipped->get_header_value("Content-Disposition").find("filename=\"") !=
              std::string::npos);

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

TEST_CASE("tags over HTTP: create, assign, filter, manage, survive a reopen",
          "[integration][UF-16][UF-17][UF-18]") {
    test::TempDir dir;
    Stack stack(dir.path());
    auto c = stack.client();
    const auto session =
        post(c, "/api/v1/safe/create",
             {{"path", "Теги"}, {"password", "пароль-1"}, {"confirm", "пароль-1"}}, 201);
    auto token = session["token"].get<std::string>();
    httplib::UploadFormDataItems items = {
        {"file", makePng(64, 48), "Отпуск/Море/фото.png", "image/png"},
        {"file", "заметка", "Отпуск/заметка.txt", "text/plain"},
        {"file", "отчет", "Работа/отчёт.txt", "text/plain"},
        {"file", "readme", "readme.txt", "text/plain"},
    };
    auto imported = c.Post("/api/v1/import", bearer(token), items);
    REQUIRE(imported);
    REQUIRE(imported->status == 200);

    const auto idOf = [](const Json& entry) { return entry["id"].get<std::int64_t>(); };
    const auto listing = [&](const Json& parent) {
        return get(c, "/api/v1/entries?parentId=" + std::to_string(idOf(parent)), token);
    };
    const auto root = get(c, "/api/v1/entries", token);
    const auto trip = byName(root, "Отпуск");
    const auto work = byName(root, "Работа");
    const auto readme = byName(root, "readme.txt");
    const auto sea = byName(listing(trip), "Море");
    const auto note = byName(listing(trip), "заметка.txt");
    const auto photo = byName(listing(sea), "фото.png");
    const auto report = byName(listing(work), "отчёт.txt");

    const auto send = [&](const char* method, const std::string& path, const Json& body,
                          int expected) {
        const auto headers = bearer(token);
        auto r = std::string(method) == "PATCH"
                     ? c.Patch(path, headers, body.dump(), "application/json")
                     : c.Delete(path, headers);
        REQUIRE(r);
        INFO(method << " " << path << " -> " << r->body);
        REQUIRE(r->status == expected);
        return Json::parse(r->body);
    };
    const auto assign = [&](const Json& body) {
        return post(c, "/api/v1/entries/tags", body, 200, bearer(token))["updated"];
    };
    const auto found = [&](const std::string& query) {
        std::vector<std::string> names;
        for (const auto& hit : get(c, "/api/v1/search?" + query, token)["results"]) {
            names.push_back(hit["entry"]["name"].get<std::string>());
        }
        return names;
    };
    using Names = std::vector<std::string>;

    // категории и теги: дубль без учета регистра, тег в несуществующей категории
    const auto people = post(c, "/api/v1/tags/categories", {{"name", "Люди"}}, 201, bearer(token));
    CHECK(people["tags"].is_array());
    CHECK(post(c, "/api/v1/tags/categories", {{"name", "люди"}}, 409,
               bearer(token))["error"]["code"] == "already_exists");
    const auto peopleId = idOf(people);
    const auto anna =
        post(c, "/api/v1/tags", {{"category", "Люди"}, {"name", "Анна"}}, 201, bearer(token));
    CHECK(anna["categoryId"] == peopleId);
    const auto sameAnna =
        post(c, "/api/v1/tags", {{"category", "люди"}, {"name", "АННА"}}, 200, bearer(token));
    CHECK(idOf(sameAnna) == idOf(anna));
    CHECK(post(c, "/api/v1/tags", {{"category", "Место"}, {"name", "Крым"}}, 404,
               bearer(token))["error"]["code"] == "not_found");
    post(c, "/api/v1/tags", {{"category", "Люди"}, {"name", "a:b"}}, 422, bearer(token));
    const auto crimea =
        post(c, "/api/v1/tags", {{"category", "Место"}, {"name", "Крым"}, {"createCategory", true}},
             201, bearer(token));
    const auto sochi =
        post(c, "/api/v1/tags", {{"category", "Место"}, {"name", "Сочи"}}, 201, bearer(token));
    const auto aniya =
        post(c, "/api/v1/tags", {{"category", "Люди"}, {"name", "Аня"}}, 201, bearer(token));

    // присвоение: Крым отпуску на все вложенное, остальное - точечно
    CHECK(assign({{"ids", {idOf(trip)}},
                  {"add", {{{"tagId", idOf(crimea)}, {"inherit", true}}}}}) == 1);
    CHECK(assign({{"ids", {idOf(note), idOf(report)}}, {"add", {{{"tagId", idOf(anna)}}}}}) == 2);
    CHECK(assign({{"ids", {idOf(readme)}}, {"add", {{{"tagId", idOf(aniya)}}}}}) == 1);
    CHECK(assign({{"ids", {idOf(photo)}}, {"add", {{{"tagId", idOf(sochi)}}}}}) == 1);
    post(c, "/api/v1/entries/tags", {{"ids", {idOf(photo)}}, {"add", {{{"tagId", 9999}}}}}, 422,
         bearer(token));
    post(c, "/api/v1/entries/tags", {{"ids", {424242}}, {"add", {{{"tagId", idOf(sochi)}}}}}, 404,
         bearer(token));

    const auto seaAgain = byName(listing(trip), "Море");
    CHECK(seaAgain["tags"].empty());
    CHECK(seaAgain["inheritedTags"] ==
          Json::parse(R"([{"tagId":)" + std::to_string(idOf(crimea)) + R"(,"fromId":)" +
                      std::to_string(idOf(trip)) + "}]"));
    CHECK(byName(listing(trip), "заметка.txt")["tags"][0] ==
          Json({{"tagId", idOf(anna)}, {"inherit", false}}));

    // фильтр: унаследованные теги, режимы, область, текст
    const auto crimeaId = std::to_string(idOf(crimea));
    const auto annaId = std::to_string(idOf(anna));
    const auto sochiId = std::to_string(idOf(sochi));
    CHECK(found("tags=" + crimeaId) == Names{"Море", "Отпуск", "заметка.txt", "фото.png"});
    CHECK(found("tags=" + annaId + "," + crimeaId) == Names{"заметка.txt"});
    CHECK(found("match=all&tags=" + annaId + "," + crimeaId) == Names{"заметка.txt"});
    CHECK(found("match=any&tags=" + annaId + "," + sochiId) ==
          Names{"заметка.txt", "отчёт.txt", "фото.png"});
    CHECK(found("tags=" + annaId + "&within=" + std::to_string(idOf(trip))) ==
          Names{"заметка.txt"});
    CHECK(found("q=%D0%B7%D0%B0%D0%BC%D0%B5%D1%82%D0%BA%D0%B0&tags=" + annaId) ==
          Names{"заметка.txt"}); // "заметка"
    CHECK(found("tags=" + sochiId + "&within=" + std::to_string(idOf(work))).empty());
    const auto unknown = c.Get("/api/v1/search?tags=9999", bearer(token));
    REQUIRE(unknown);
    CHECK(unknown->status == 422);
    const auto hit = get(c, "/api/v1/search?q=%D0%B7%D0%B0%D0%BC%D0%B5%D1%82%D0%BA%D0%B0", token);
    CHECK(hit["results"][0]["matchedIn"] == "name");

    // управление: переименование в занятое имя -> слияние; перенос; удаление тега
    CHECK(send("PATCH", "/api/v1/tags/" + annaId, {{"name", " аня "}}, 409)["error"]["code"] ==
          "already_exists");
    CHECK(send("PATCH", "/api/v1/tags/" + annaId, {{"name", "Анна К."}}, 200)["name"] == "Анна К.");
    const auto merged = post(c, "/api/v1/tags/" + std::to_string(idOf(aniya)) + "/merge",
                             {{"into", idOf(anna)}}, 200, bearer(token));
    CHECK(merged["affectedEntries"] == 1);
    CHECK(get(c, "/api/v1/entries/" + std::to_string(idOf(readme)), token)["tags"][0]["tagId"] ==
          idOf(anna));
    const auto placesId = crimea["categoryId"].get<std::int64_t>();
    CHECK(send("PATCH", "/api/v1/tags/categories/" + std::to_string(placesId), {{"name", "Места"}},
               200)["name"] == "Места");
    CHECK(send("PATCH", "/api/v1/tags/" + sochiId, {{"categoryId", peopleId}}, 200)["categoryId"] ==
          peopleId);
    CHECK(send("DELETE", "/api/v1/tags/" + sochiId, {}, 200)["affectedEntries"] == 1);
    CHECK(get(c, "/api/v1/entries/" + std::to_string(idOf(photo)), token)["tags"].empty());

    const auto checkCatalog = [&] {
        const auto all = get(c, "/api/v1/tags", token)["categories"];
        REQUIRE(all.size() == 2);
        CHECK(all[0]["name"] == "Люди");
        REQUIRE(all[0]["tags"].size() == 1);
        CHECK(all[0]["tags"][0]["name"] == "Анна К.");
        CHECK(all[0]["tags"][0]["count"] == 3); // заметка, отчёт, readme
        CHECK(all[1]["name"] == "Места");
        REQUIRE(all[1]["tags"].size() == 1);
        CHECK(all[1]["tags"][0]["name"] == "Крым");
        CHECK(all[1]["tags"][0]["count"] == 1);
    };
    checkCatalog();

    // блокировка и повторный вход: теги, счетчики и фильтр на месте
    post(c, "/api/v1/safe/lock", Json::object(), 204, bearer(token));
    const auto again = post(c, "/api/v1/safe/unlock",
                            {{"path", session["safe"]["path"]}, {"password", "пароль-1"}}, 200);
    token = again["token"].get<std::string>();
    checkCatalog();
    CHECK(found("tags=" + crimeaId) == Names{"Море", "Отпуск", "заметка.txt", "фото.png"});
    CHECK(found("tags=" + annaId + "," + crimeaId) == Names{"заметка.txt"});

    // категория уходит вместе с тегами, и с записей тоже, унаследованное в том числе
    const auto removed =
        send("DELETE", "/api/v1/tags/categories/" + std::to_string(placesId), {}, 200);
    CHECK(removed == Json({{"removedTags", 1}, {"affectedEntries", 1}}));
    CHECK(byName(listing(trip), "Море")["inheritedTags"].empty());
    CHECK(byName(get(c, "/api/v1/entries", token), "Отпуск")["tags"].empty());
    const auto gone = c.Get("/api/v1/search?tags=" + crimeaId, bearer(token));
    REQUIRE(gone);
    CHECK(gone->status == 422);
}

TEST_CASE("re-importing a folder over HTTP: plan, then a manifest with the decisions",
          "[integration][UF-15]") {
    test::TempDir dir;
    Stack stack(dir.path());
    auto c = stack.client();
    const auto session =
        post(c, "/api/v1/safe/create",
             {{"path", "Повторный"}, {"password", "пароль-1"}, {"confirm", "пароль-1"}}, 201);
    const auto token = session["token"].get<std::string>();

    const auto manifestOf = [](const Json& files) { return Json{{"files", files}}.dump(); };
    const auto upload = [&](const std::string& manifest, httplib::UploadFormDataItems files,
                            const std::string& query = {}) {
        httplib::UploadFormDataItems items;
        if (!manifest.empty()) {
            items.push_back({"manifest", manifest, "", "application/json"});
        }
        items.insert(items.end(), files.begin(), files.end());
        auto r = c.Post("/api/v1/import" + query, bearer(token), items);
        REQUIRE(r);
        INFO(r->body);
        return r;
    };
    const auto listing = [&](std::int64_t parent) {
        return get(c, "/api/v1/entries?parentId=" + std::to_string(parent), token);
    };
    const auto contentOf = [&](const Json& entry) {
        auto r =
            c.Get("/api/v1/media/" + std::to_string(entry["id"].get<std::int64_t>()) + "/content",
                  bearer(token));
        REQUIRE(r);
        return r->body;
    };

    // первый импорт папки: даты файлов на диске приходят в manifest
    auto first = upload(
        manifestOf({{"pict/a.txt", {{"lastModified", 1'700'000'000'000}}},
                    {"pict/b.txt", {{"lastModified", 1'700'000'100'000}}}}),
        {{"file", "AAA", "pict/a.txt", "text/plain"}, {"file", "BBB", "pict/b.txt", "text/plain"}});
    REQUIRE(first->status == 200);
    CHECK(Json::parse(first->body)["imported"] == 2);
    const auto pict = byName(get(c, "/api/v1/entries", token), "pict");
    const auto pictId = pict["id"].get<std::int64_t>();
    CHECK(byName(listing(pictId), "a.txt")["sourceModifiedAt"] == 1'700'000'000'000);

    // на диске добавился c.txt: план показывает совпадения (с датой файла в сейфе) и новый файл
    const auto plan = post(c, "/api/v1/import/plan",
                           {{"files",
                             {{{"path", "pict/a.txt"}, {"size", 3}},
                              {{"path", "pict/b.txt"}, {"size", 3}},
                              {{"path", "pict/c.txt"}, {"size", 3}}}}},
                           200, bearer(token));
    REQUIRE(plan["conflicts"].size() == 2);
    CHECK(plan["newFiles"] == 1);
    CHECK(plan["conflicts"][0]["path"] == "pict/a.txt");
    CHECK(plan["conflicts"][0]["existing"]["name"] == "a.txt");
    CHECK(plan["conflicts"][0]["existing"]["size"] == 3);
    CHECK(plan["conflicts"][0]["existing"]["sourceModifiedAt"] == 1'700'000'000'000);
    CHECK(plan["conflicts"][1]["existing"]["sourceModifiedAt"] == 1'700'000'100'000);

    // "Пропустить": на сервер идет только новый файл
    auto onlyNew = upload("", {{"file", "CCC", "pict/c.txt", "text/plain"}});
    REQUIRE(onlyNew->status == 200);
    CHECK(Json::parse(onlyNew->body) == Json({{"imported", 1},
                                              {"replaced", 0},
                                              {"skipped", 0},
                                              {"failed", 0},
                                              {"failures", Json::array()}}));
    CHECK(listing(pictId)["entries"].size() == 3);
    CHECK(contentOf(byName(listing(pictId), "a.txt")) == "AAA");

    // то же, но клиент отправил и совпадения с решением skip: результат тот же, лишнего в сейфе нет
    auto sentAll = upload(manifestOf({{"pict/a.txt", {{"onConflict", "skip"}}},
                                      {"pict/b.txt", {{"onConflict", "skip"}}},
                                      {"pict/c.txt", {{"onConflict", "skip"}}}}),
                          {{"file", "AAA", "pict/a.txt", "text/plain"},
                           {"file", "BBB", "pict/b.txt", "text/plain"},
                           {"file", "CCC", "pict/c.txt", "text/plain"}});
    REQUIRE(sentAll->status == 200);
    CHECK(Json::parse(sentAll->body)["skipped"] == 3);
    CHECK(Json::parse(sentAll->body)["imported"] == 0);
    CHECK(listing(pictId)["entries"].size() == 3);

    // "Заменить" a.txt (запись остается, меняется содержимое и дата), b.txt - оба
    const auto aBefore = byName(listing(pictId), "a.txt");
    auto mixed =
        upload(manifestOf({{"pict/a.txt",
                            {{"onConflict", "replace"}, {"lastModified", 1'800'000'000'000}}},
                           {"pict/b.txt", {{"onConflict", "keepBoth"}}}}),
               {{"file", "AAA-новый", "pict/a.txt", "text/plain"},
                {"file", "BBB-копия", "pict/b.txt", "text/plain"}});
    REQUIRE(mixed->status == 200);
    const auto result = Json::parse(mixed->body);
    CHECK(result["imported"] == 1);
    CHECK(result["replaced"] == 1);
    CHECK(result["skipped"] == 0);
    const auto after = listing(pictId);
    CHECK(after["entries"].size() == 4);
    const auto aAfter = byName(after, "a.txt");
    CHECK(aAfter["id"] == aBefore["id"]);
    CHECK(aAfter["sourceModifiedAt"] == 1'800'000'000'000);
    CHECK(aAfter["createdAt"] == aBefore["createdAt"]);
    CHECK(contentOf(aAfter) == "AAA-новый");
    CHECK(contentOf(byName(after, "b.txt")) == "BBB");
    CHECK(contentOf(byName(after, "b (2).txt")) == "BBB-копия");
    CHECK(byName(after, "b (2).txt")["sourceModifiedAt"].is_null());

    // импорт во вложения файла: имена сверяются у него
    const auto aId = aAfter["id"].get<std::int64_t>();
    auto attach = upload("", {{"file", "вложение", "n.txt", "text/plain"}},
                         "?parentId=" + std::to_string(aId));
    REQUIRE(attach->status == 200);
    CHECK(Json::parse(attach->body)["imported"] == 1);
    CHECK(listing(aId)["parent"]["name"] == "a.txt");
    const auto attachPlan =
        post(c, "/api/v1/import/plan?parentId=" + std::to_string(aId),
             {{"files", {{{"path", "n.txt"}, {"size", 1}}, {{"path", "a.txt"}, {"size", 1}}}}}, 200,
             bearer(token));
    CHECK(attachPlan["conflicts"].size() == 1);
    CHECK(attachPlan["newFiles"] == 1);

    // архив вложений: имя "имя (вложения).zip", внутри каталог с тем же именем
    auto zipped = c.Get("/api/v1/media/" + std::to_string(aId) + "/zip", bearer(token));
    REQUIRE(zipped);
    REQUIRE(zipped->status == 200);
    const auto files = unzip(zipped->body);
    CHECK(files.at("a.txt (вложения)/") == "<dir>");
    CHECK(files.at("a.txt (вложения)/n.txt") == "вложение");
    CHECK(files.size() == 2);
    auto plain =
        c.Get("/api/v1/media/" + std::to_string(byName(after, "b.txt")["id"].get<std::int64_t>()) +
                  "/zip",
              bearer(token));
    REQUIRE(plain);
    CHECK(plain->status == 422);
}

TEST_CASE("a broken or misplaced manifest imports nothing over HTTP", "[integration][UF-15]") {
    test::TempDir dir;
    Stack stack(dir.path());
    auto c = stack.client();
    const auto session =
        post(c, "/api/v1/safe/create",
             {{"path", "Манифест"}, {"password", "пароль-1"}, {"confirm", "пароль-1"}}, 201);
    const auto token = session["token"].get<std::string>();

    httplib::UploadFormDataItems broken = {{"manifest", "{\"files\": ", "", "application/json"},
                                           {"file", "A", "a.txt", "text/plain"}};
    auto refused = c.Post("/api/v1/import", bearer(token), broken);
    REQUIRE(refused);
    CHECK(refused->status == 400);
    CHECK(Json::parse(refused->body)["error"]["code"] == "bad_request");

    httplib::UploadFormDataItems late = {{"note", "text", "", ""},
                                         {"manifest", "{}", "", "application/json"},
                                         {"file", "A", "a.txt", "text/plain"}};
    auto misplaced = c.Post("/api/v1/import", bearer(token), late);
    REQUIRE(misplaced);
    CHECK(misplaced->status == 400);

    CHECK(get(c, "/api/v1/entries", token)["entries"].empty());
    // и сервер после отказов жив
    httplib::UploadFormDataItems fine = {{"file", "A", "a.txt", "text/plain"}};
    auto ok = c.Post("/api/v1/import", bearer(token), fine);
    REQUIRE(ok);
    CHECK(ok->status == 200);
}
