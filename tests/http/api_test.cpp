// Тесты http без сети: маршруты вешаются на httplib::Server, запросы подаем
// сырыми байтами в process_request
#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <map>
#include <sstream>

#include "api.hpp"
#include "fake_services.hpp"

using namespace safebox;
using namespace safebox::test;
using Code = domain::Error::Code;

namespace {

// Поток поверх строк: запрос читается из in, ответ копится в out.
class MemoryStream final : public httplib::Stream {
public:
    explicit MemoryStream(std::string in) : in_(std::move(in)) {}

    bool is_readable() const override { return pos_ < in_.size(); }
    bool wait_readable() const override { return true; }
    bool wait_writable() const override { return true; }
    ssize_t read(char* ptr, size_t size) override {
        const auto n = std::min(size, in_.size() - pos_);
        std::memcpy(ptr, in_.data() + pos_, n);
        pos_ += n;
        return static_cast<ssize_t>(n);
    }
    ssize_t write(const char* ptr, size_t size) override {
        out_.append(ptr, size);
        return static_cast<ssize_t>(size);
    }
    void get_remote_ip_and_port(std::string& ip, int& port) const override {
        ip = "127.0.0.1";
        port = 50000;
    }
    void get_local_ip_and_port(std::string& ip, int& port) const override {
        ip = "127.0.0.1";
        port = 8900;
    }
    socket_t socket() const override { return INVALID_SOCKET; }
    time_t duration() const override { return 0; }

    [[nodiscard]] const std::string& output() const { return out_; }

private:
    std::string in_;
    std::size_t pos_ = 0;
    std::string out_;
};

class TestServer final : public httplib::Server {
public:
    // httplib пишет тело из content provider, только пока сервер "не
    // останавливается" (svr_sock_ != INVALID_SOCKET). Сокета у теста нет -
    // ставим заглушку; accept-цикл не запускается, заглушка никуда не уходит.
    TestServer() { svr_sock_ = static_cast<socket_t>(0x7FFF0000); }
    ~TestServer() override { svr_sock_ = INVALID_SOCKET; }

    std::string handle(const std::string& raw) {
        MemoryStream stream(raw);
        bool closed = false;
        process_request(stream, "127.0.0.1", 50000, "127.0.0.1", 8900, true, closed, nullptr);
        return stream.output();
    }
};

struct Reply {
    int status = 0;
    std::multimap<std::string, std::string> headers; // ключи в нижнем регистре
    std::string body;

    [[nodiscard]] std::string header(const std::string& name) const {
        const auto it = headers.find(name);
        return it == headers.end() ? std::string{} : it->second;
    }
    [[nodiscard]] http::Json json() const { return http::Json::parse(body); }
    [[nodiscard]] std::string errorCode() const {
        return json()["error"]["code"].get<std::string>();
    }
};

std::string lower(std::string s) {
    for (auto& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

Reply parse(const std::string& raw) {
    Reply reply;
    const auto headerEnd = raw.find("\r\n\r\n");
    REQUIRE(headerEnd != std::string::npos);
    std::istringstream head(raw.substr(0, headerEnd));
    std::string line;
    std::getline(head, line);
    reply.status = std::stoi(line.substr(9, 3));
    while (std::getline(head, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const auto colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        auto value = line.substr(colon + 1);
        value.erase(0, value.find_first_not_of(' '));
        reply.headers.emplace(lower(line.substr(0, colon)), value);
    }
    auto body = raw.substr(headerEnd + 4);
    if (lower(reply.header("transfer-encoding")) == "chunked") {
        std::string decoded;
        std::size_t pos = 0;
        for (;;) {
            const auto eol = body.find("\r\n", pos);
            REQUIRE(eol != std::string::npos); // незавершенный chunked-ответ
            const auto size = std::stoul(body.substr(pos, eol - pos), nullptr, 16);
            if (size == 0) {
                break;
            }
            decoded += body.substr(eol + 2, size);
            pos = eol + 2 + size + 2;
        }
        body = decoded;
    }
    reply.body = body;
    return reply;
}

class TestAssets final : public http::AssetProvider {
public:
    std::optional<http::Asset> find(std::string_view path) const override {
        if (path == "/index.html") {
            return http::Asset{"text/html; charset=utf-8", "<!doctype html><title>SafeBox</title>"};
        }
        if (path == "/assets/app-1234.js") {
            return http::Asset{"text/javascript; charset=utf-8", "console.log(1)"};
        }
        return std::nullopt;
    }
    bool empty() const override { return !enabled; }

    bool enabled = false;
};

struct HttpFixture {
    HttpFixture() {
        http::HttpConfig config;
        config.port = 8900;
        config.extraHosts = {"192.168.1.5"};
        config.extraOrigins = {"http://localhost:5173/"};
        config.maxJsonBody = 1024;
        config.version = "test";
        ctx = std::make_unique<http::ApiContext>(fakes.services(), assets, config);
        http::registerRoutes(server, *ctx);
    }

    Reply request(const std::string& method, const std::string& target,
                  const std::vector<std::string>& headers = {}, const std::string& body = {},
                  bool withHost = true) {
        std::string raw = method + " " + target + " HTTP/1.1\r\n";
        if (withHost) {
            raw += "Host: 127.0.0.1:8900\r\n";
        }
        for (const auto& h : headers) {
            raw += h + "\r\n";
        }
        if (!body.empty()) {
            raw += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        }
        raw += "\r\n" + body;
        return parse(server.handle(raw));
    }

    Reply api(const std::string& method, const std::string& target,
              const std::string& jsonBody = {}, std::vector<std::string> headers = {}) {
        headers.push_back("Authorization: Bearer " + std::string(kApiToken));
        if (!jsonBody.empty()) {
            headers.push_back("Content-Type: application/json");
        }
        return request(method, target, headers, jsonBody);
    }

    FakeServices fakes;
    TestAssets assets;
    std::unique_ptr<http::ApiContext> ctx;
    TestServer server;
};

} // namespace

TEST_CASE("health answers without auth and carries security headers", "[http]") {
    HttpFixture f;
    auto r = f.request("GET", "/health");
    CHECK(r.status == 200);
    CHECK(r.json()["status"] == "ok");
    CHECK(r.json()["version"] == "test");
    CHECK(r.header("x-content-type-options") == "nosniff");
    CHECK(r.header("x-frame-options") == "DENY");
    CHECK(r.header("cache-control") == "no-store");
    CHECK(r.header("referrer-policy") == "no-referrer");
}

TEST_CASE("Host whitelist blocks DNS rebinding", "[http][guard]") {
    HttpFixture f;
    CHECK(f.request("GET", "/health", {"Host: evil.example:8900"}, {}, false).status == 403);
    CHECK(f.request("GET", "/health", {"Host: 127.0.0.1:9999"}, {}, false).status == 403);
    CHECK(f.request("GET", "/health", {}, {}, false).status == 403);
    auto rejected = f.request("GET", "/api/v1/entries", {"Host: evil.example"}, {}, false);
    CHECK(rejected.status == 403);
    CHECK(rejected.errorCode() == "forbidden");
    CHECK(f.fakes.safe->authorizations == 0); // до любой логики

    CHECK(f.request("GET", "/health", {"Host: localhost:8900"}, {}, false).status == 200);
    CHECK(f.request("GET", "/health", {"Host: LOCALHOST:8900"}, {}, false).status == 200);
    CHECK(f.request("GET", "/health", {"Host: [::1]:8900"}, {}, false).status == 200);
    CHECK(f.request("GET", "/health", {"Host: 192.168.1.5:8900"}, {}, false).status == 200);
}

TEST_CASE("Origin is checked on every mutating request", "[http][guard]") {
    HttpFixture f;
    const std::string bearer = "Authorization: Bearer " + std::string(kApiToken);
    auto evil = f.request("POST", "/api/v1/safe/lock", {bearer, "Origin: https://evil.example"});
    CHECK(evil.status == 403);
    CHECK(evil.errorCode() == "forbidden");
    CHECK(f.request("POST", "/api/v1/safe/lock", {bearer, "Origin: null"}).status == 403);
    CHECK(f.request("POST", "/api/v1/safe/lock", {bearer, "Sec-Fetch-Site: cross-site"}).status ==
          403);
    CHECK(f.fakes.safe->lockCalls == 0);

    const std::string multipart = "--b\r\nContent-Disposition: form-data; name=\"file\"; "
                                  "filename=\"a.txt\"\r\n\r\nx\r\n--b--\r\n";
    auto evilImport = f.request(
        "POST", "/api/v1/import",
        {bearer, "Origin: http://evil.example", "Content-Type: multipart/form-data; boundary=b"},
        multipart);
    CHECK(evilImport.status == 403);
    CHECK(f.fakes.importExport->imported.empty());

    CHECK(
        f.request("POST", "/api/v1/safe/lock", {bearer, "Origin: http://127.0.0.1:8900"}).status ==
        204);
    f.fakes.safe->unlocked = true; // фейк "заблокировался" - открываем снова
    CHECK(
        f.request("POST", "/api/v1/safe/lock", {bearer, "Origin: http://localhost:5173"}).status ==
        204);
}

TEST_CASE("JSON endpoints accept only small application/json bodies", "[http][guard]") {
    HttpFixture f;
    const std::string body = R"({"path":"x","password":"y"})";
    auto text = f.request("POST", "/api/v1/safe/unlock", {"Content-Type: text/plain"}, body);
    CHECK(text.status == 415);
    CHECK(text.errorCode() == "unsupported_media_type");
    CHECK(f.request("POST", "/api/v1/safe/unlock",
                    {"Content-Type: application/x-www-form-urlencoded"}, "a=b")
              .status == 415);
    auto big = f.request("POST", "/api/v1/safe/unlock", {"Content-Type: application/json"},
                         R"({"path":")" + std::string(2000, 'x') + R"("})");
    CHECK(big.status == 413);
    CHECK(f.request("POST", "/api/v1/safe/unlock", {"Content-Type: application/json"}, "{broken")
              .status == 400);
    CHECK(f.request("POST", "/api/v1/safe/unlock", {"Content-Type: application/json"},
                    R"({"path":1})")
              .status == 400);
    CHECK(f.request("POST", "/api/v1/safe/unlock",
                    {"Content-Type: application/json; charset=utf-8"}, body)
              .status == 200);
}

TEST_CASE("domain errors map to the documented HTTP table", "[http][errors]") {
    HttpFixture f;
    struct Row {
        Code code;
        int status;
        const char* wire;
    };
    const Row table[] = {
        {Code::Locked, 401, "unauthorized"},
        {Code::Cancelled, 401, "unauthorized"},
        {Code::WrongPassword, 403, "wrong_password"},
        {Code::NotFound, 404, "not_found"},
        {Code::AlreadyExists, 409, "already_exists"},
        {Code::InvalidArgument, 422, "invalid_argument"},
        {Code::NotASafe, 422, "not_a_safe"},
        {Code::IoError, 500, "io_error"},
        {Code::IntegrityError, 500, "integrity_error"},
        {Code::Internal, 500, "internal"},
        {Code::PreviewFailed, 502, "preview_failed"},
    };
    for (const auto& row : table) {
        f.fakes.entries->getError = domain::Error{row.code, "сообщение"};
        auto r = f.api("GET", "/api/v1/entries/1");
        CAPTURE(domain::toString(row.code));
        CHECK(r.status == row.status);
        CHECK(r.errorCode() == row.wire);
        CHECK(r.json()["error"]["message"] == "сообщение");
        CHECK((r.status == 401) == !r.header("www-authenticate").empty());
    }
}

TEST_CASE("Bearer guards the API, the media cookie only GET media", "[http][auth]") {
    HttpFixture f;
    CHECK(f.request("GET", "/api/v1/entries").status == 401);
    CHECK(f.request("GET", "/api/v1/entries", {"Authorization: Bearer nope"}).status == 401);
    CHECK(f.api("GET", "/api/v1/entries").status == 200);

    const std::string mediaCookie = "Cookie: theme=dark; sbx_media=" + std::string(kMediaToken);
    CHECK(f.request("GET", "/api/v1/entries", {mediaCookie}).status == 401);
    CHECK(f.request("GET", "/api/v1/search?q=a", {mediaCookie}).status == 401);
    CHECK(f.request("GET", "/api/v1/media/2/thumbnail", {mediaCookie}).status == 200);
    CHECK(f.request("GET", "/api/v1/media/2/thumbnail",
                    {"Cookie: sbx_media=" + std::string(kApiToken)})
              .status == 401); // api-токен в cookie не работает
    CHECK(f.api("GET", "/api/v1/media/2/thumbnail").status == 200);

    f.fakes.safe->unlocked = false; // сейф заблокирован -> любой токен мертв
    auto locked = f.api("GET", "/api/v1/entries");
    CHECK(locked.status == 401);
    CHECK(locked.errorCode() == "unauthorized");
}

TEST_CASE("safe lifecycle endpoints", "[http][safe]") {
    HttpFixture f;
    auto wrong = f.request("POST", "/api/v1/safe/unlock", {"Content-Type: application/json"},
                           R"({"path":"C:/s/my.safebox","password":"wrong"})");
    CHECK(wrong.status == 403);
    CHECK(wrong.errorCode() == "wrong_password");

    auto notSafe = f.request("POST", "/api/v1/safe/unlock", {"Content-Type: application/json"},
                             R"({"path":"not-a-safe","password":"secret1"})");
    CHECK(notSafe.status == 422);
    CHECK(notSafe.errorCode() == "not_a_safe");

    auto ok = f.request("POST", "/api/v1/safe/unlock", {"Content-Type: application/json"},
                        R"({"path":"C:/s/my.safebox","password":"secret1"})");
    CHECK(ok.status == 200);
    CHECK(ok.json()["token"] == std::string(kApiToken));
    CHECK(ok.json()["safe"]["idleRemainingSec"] == 900);
    const auto cookie = ok.header("set-cookie");
    CHECK(cookie.find("sbx_media=" + std::string(kMediaToken)) == 0);
    CHECK(cookie.find("HttpOnly") != std::string::npos);
    CHECK(cookie.find("SameSite=Strict") != std::string::npos);
    CHECK(cookie.find("Path=/api/v1/media") != std::string::npos);

    auto created = f.request("POST", "/api/v1/safe/create", {"Content-Type: application/json"},
                             R"({"path":"новый","password":"secret1","confirm":"secret1"})");
    CHECK(created.status == 201);
    CHECK(f.fakes.safe->lastPath == "новый");
    f.fakes.safe->createError = domain::Error{Code::AlreadyExists, "Файл уже существует"};
    CHECK(f.request("POST", "/api/v1/safe/create", {"Content-Type: application/json"},
                    R"({"path":"новый","password":"secret1","confirm":"secret1"})")
              .status == 409);
    CHECK(f.request("POST", "/api/v1/safe/create", {"Content-Type: application/json"},
                    R"({"path":"x"})")
              .status == 400);

    auto password = f.api("POST", "/api/v1/safe/password",
                          R"({"oldPassword":"wrong","newPassword":"secret2","confirm":"secret2"})");
    CHECK(password.status == 403);
    CHECK(f.api("POST", "/api/v1/safe/password",
                R"({"oldPassword":"secret1","newPassword":"secret2","confirm":"secret2"})")
              .status == 204);

    auto beat = f.api("POST", "/api/v1/safe/heartbeat", R"({"active":true})");
    CHECK(beat.status == 200);
    CHECK(beat.json()["idleRemainingSec"] == 900);
    CHECK(f.fakes.safe->lastHeartbeatActive == true);
    CHECK(f.api("POST", "/api/v1/safe/heartbeat").json()["idleRemainingSec"] ==
          600); // без тела -> active=false
    CHECK(f.api("POST", "/api/v1/safe/heartbeat", R"({"active":"yes"})").status == 400);

    auto status = f.request("GET", "/api/v1/safe/status");
    CHECK(status.json()["authorized"] == false);
    CHECK(status.json()["lastPath"] == "C:/safes/my.safebox");
    auto authed = f.api("GET", "/api/v1/safe/status");
    CHECK(authed.json()["authorized"] == true);
    CHECK(authed.json()["safe"]["entryCount"] == 3);

    auto lock = f.api("POST", "/api/v1/safe/lock");
    CHECK(lock.status == 204);
    CHECK(f.fakes.safe->lockCalls == 1);
    CHECK(lock.header("set-cookie").find("Max-Age=0") != std::string::npos);
    CHECK(f.api("POST", "/api/v1/safe/lock").status == 401);
}

TEST_CASE("entries endpoints speak the documented DTO", "[http][entries]") {
    HttpFixture f;
    auto root = f.api("GET", "/api/v1/entries");
    REQUIRE(root.status == 200);
    const auto body = root.json();
    CHECK(body["parent"].is_null());
    CHECK(body["path"].empty());
    REQUIRE(body["entries"].size() == 3);
    const auto& link = body["entries"][1];
    CHECK(link["kind"] == "link");
    CHECK(link["url"] == "https://Example.com:8443/page");
    CHECK(link["domain"] == "example.com");
    CHECK(body["entries"][0]["kind"] == "folder");
    CHECK(body["entries"][0]["parentId"].is_null());

    auto folder = f.api("GET", "/api/v1/entries?parentId=1");
    CHECK(folder.json()["parent"]["name"] == "Отпуск");
    CHECK(folder.json()["path"][0]["name"] == "Отпуск");
    CHECK(folder.json()["entries"][0]["hasThumbnail"] == false);
    CHECK(f.api("GET", "/api/v1/entries?parentId=abc").status == 400);
    CHECK(f.api("GET", "/api/v1/entries?parentId=77").status == 404);

    CHECK(f.api("GET", "/api/v1/entries/2").json()["name"] == "море.jpg");
    CHECK(f.api("GET", "/api/v1/entries/-5").status == 400);
    CHECK(f.api("GET", "/api/v1/folders").json()["folders"][0]["name"] == "Отпуск");

    CHECK(f.api("DELETE", "/api/v1/entries/3").json()["removed"] == 1);
    CHECK(f.fakes.entries->removed == std::vector<domain::EntryId>{3});
    CHECK(f.api("POST", "/api/v1/entries/delete", R"({"ids":[1,2]})").json()["removed"] == 2);
    CHECK(f.api("POST", "/api/v1/entries/delete", R"({"ids":[1,"x"]})").status == 400);
}

TEST_CASE("entries carry description, child count, tags and source time", "[http][entries]") {
    HttpFixture f;
    const auto photo = f.api("GET", "/api/v1/entries/2").json();
    CHECK(photo["description"] == "закат на пляже");
    CHECK(photo["childCount"] == 0);
    CHECK(photo["sourceModifiedAt"] == 1'700'000'000'000);
    REQUIRE(photo["tags"].size() == 2);
    CHECK(photo["tags"][0] == http::Json({{"tagId", 7}, {"inherit", true}}));
    CHECK(photo["tags"][1] == http::Json({{"tagId", 9}, {"inherit", false}}));
    REQUIRE(photo["inheritedTags"].size() == 1);
    CHECK(photo["inheritedTags"][0] == http::Json({{"tagId", 5}, {"fromId", 1}}));
    CHECK_FALSE(photo.contains("previewPending")); // пока не выводится

    const auto folder = f.api("GET", "/api/v1/entries/1").json();
    CHECK(folder["childCount"] == 1);
    CHECK(folder["description"] == "");
    CHECK(folder["sourceModifiedAt"].is_null());
    CHECK(folder["tags"].is_array());
    CHECK(folder["tags"].empty());
    CHECK(folder["inheritedTags"].is_array());
    CHECK(folder["inheritedTags"].empty());

    // те же поля в листинге и в результатах поиска
    const auto listing = f.api("GET", "/api/v1/entries?parentId=1").json();
    CHECK(listing["parent"]["childCount"] == 1);
    CHECK(listing["entries"][0]["tags"].size() == 2);
    const auto hit = f.api("GET", "/api/v1/search?q=a").json()["results"][0];
    CHECK(hit["entry"]["description"] == "");
    CHECK(hit["entry"]["childCount"] == 0);
    CHECK(hit["entry"]["tags"].is_array());
    CHECK(hit["entry"]["inheritedTags"].is_array());
}

TEST_CASE("search hits tell where the text matched", "[http][search]") {
    HttpFixture f;
    CHECK(f.api("GET", "/api/v1/search?q=a").json()["results"][0]["matchedIn"].is_null());
    f.fakes.search->matchedIn = domain::MatchedIn::Name;
    CHECK(f.api("GET", "/api/v1/search?q=a").json()["results"][0]["matchedIn"] == "name");
    f.fakes.search->matchedIn = domain::MatchedIn::Description;
    CHECK(f.api("GET", "/api/v1/search?q=a").json()["results"][0]["matchedIn"] == "description");
}

TEST_CASE("PATCH /entries/:id takes name, description and url", "[http][entries][update]") {
    HttpFixture f;
    auto renamed = f.api("PATCH", "/api/v1/entries/2", R"({"name":"берег.jpg"})");
    CHECK(renamed.status == 200);
    CHECK(renamed.json()["name"] == "берег.jpg");
    REQUIRE(f.fakes.entries->lastUpdate.has_value());
    CHECK(f.fakes.entries->lastUpdate->name == "берег.jpg");
    CHECK_FALSE(f.fakes.entries->lastUpdate->description.has_value()); // не тронуто
    CHECK_FALSE(f.fakes.entries->lastUpdate->url.has_value());

    auto all = f.api("PATCH", "/api/v1/entries/3",
                     R"({"name":"сайт","description":"про сайт","url":"https://example.org/x"})");
    CHECK(all.status == 200);
    CHECK(all.json()["url"] == "https://example.org/x");
    CHECK(all.json()["description"] == "про сайт");
    CHECK(f.fakes.entries->lastUpdate->name == "сайт");
    CHECK(f.fakes.entries->lastUpdate->description == "про сайт");
    CHECK(f.fakes.entries->lastUpdate->url == "https://example.org/x");

    // пустое описание - значение (очистить), а не отсутствие поля
    auto cleared = f.api("PATCH", "/api/v1/entries/2", R"({"description":""})");
    CHECK(cleared.status == 200);
    CHECK(f.fakes.entries->lastUpdate->description == "");
    CHECK_FALSE(f.fakes.entries->lastUpdate->name.has_value());

    // не то, что просили
    CHECK(f.api("PATCH", "/api/v1/entries/2", R"({"title":"x"})").status == 400);
    CHECK(f.api("PATCH", "/api/v1/entries/2", R"({})").status == 400);
    CHECK(f.api("PATCH", "/api/v1/entries/2", R"({"name":5})").status == 400);
    CHECK(f.api("PATCH", "/api/v1/entries/2", R"({"description":null})").status == 400);
    CHECK(f.api("PATCH", "/api/v1/entries/2", R"({"url":["a"]})").status == 400);
    CHECK(f.api("PATCH", "/api/v1/entries/abc", R"({"name":"x"})").status == 400);
    CHECK(f.api("PATCH", "/api/v1/entries/99", R"({"name":"x"})").status == 404);

    f.fakes.entries->updateError = domain::Error{Code::InvalidArgument, "Описание длиннее 64 КиБ"};
    auto tooLong = f.api("PATCH", "/api/v1/entries/2", R"({"description":"x"})");
    CHECK(tooLong.status == 422);
    CHECK(tooLong.errorCode() == "invalid_argument");
    CHECK(tooLong.json()["error"]["message"] == "Описание длиннее 64 КиБ");
}

TEST_CASE("move/plan lists name conflicts", "[http][entries][move]") {
    HttpFixture f;
    f.fakes.entries->conflicts = {{4, makeEntry(2, 1, domain::Kind::Photo, "море.jpg")}};
    auto plan = f.api("POST", "/api/v1/entries/move/plan", R"({"ids":[4,3],"parentId":1})");
    REQUIRE(plan.status == 200);
    REQUIRE(plan.json()["conflicts"].size() == 1);
    CHECK(plan.json()["conflicts"][0]["id"] == 4);
    CHECK(plan.json()["conflicts"][0]["existing"]["name"] == "море.jpg");
    CHECK(plan.json()["conflicts"][0]["existing"]["id"] == 2);
    CHECK(f.fakes.entries->lastPlanIds == std::vector<domain::EntryId>{4, 3});
    CHECK(f.fakes.entries->lastPlanParent == 1);

    // null - корень
    f.fakes.entries->conflicts.clear();
    auto root = f.api("POST", "/api/v1/entries/move/plan", R"({"ids":[2],"parentId":null})");
    REQUIRE(root.status == 200);
    CHECK(root.json()["conflicts"].is_array());
    CHECK(root.json()["conflicts"].empty());
    CHECK_FALSE(f.fakes.entries->lastPlanParent.has_value());

    for (const char* bad :
         {R"({"parentId":1})", R"({"ids":"1","parentId":1})", R"({"ids":[1,"x"],"parentId":1})",
          R"({"ids":[0],"parentId":1})", R"({"ids":[1]})", R"({"ids":[1],"parentId":"1"})",
          R"({"ids":[1],"parentId":0})", R"({"ids":[1],"parentId":1.5})"}) {
        INFO(bad);
        CHECK(f.api("POST", "/api/v1/entries/move/plan", bad).status == 400);
    }

    f.fakes.entries->moveError = domain::Error{Code::InvalidArgument, "Нельзя переместить"};
    auto refused = f.api("POST", "/api/v1/entries/move/plan", R"({"ids":[1],"parentId":2})");
    CHECK(refused.status == 422);
    f.fakes.entries->moveError = domain::Error{Code::NotFound, "Объект назначения не найден"};
    CHECK(f.api("POST", "/api/v1/entries/move/plan", R"({"ids":[1],"parentId":99})").status == 404);
}

TEST_CASE("move takes the resolutions and answers with the counts", "[http][entries][move]") {
    HttpFixture f;
    f.fakes.entries->moveResult = {3, 2, 1};
    auto moved = f.api(
        "POST", "/api/v1/entries/move",
        R"({"ids":[2,3,4],"parentId":1,"resolutions":{"2":"replace","3":"skip","4":"keepBoth"}})");
    REQUIRE(moved.status == 200);
    CHECK(moved.json() == http::Json({{"moved", 3}, {"replaced", 2}, {"skipped", 1}}));
    REQUIRE(f.fakes.entries->lastMove.has_value());
    const auto& cmd = *f.fakes.entries->lastMove;
    CHECK(cmd.ids == std::vector<domain::EntryId>{2, 3, 4});
    CHECK(cmd.parent == 1);
    REQUIRE(cmd.resolutions.size() == 3);
    CHECK(cmd.resolutions.at(2) == app::ConflictPolicy::Replace);
    CHECK(cmd.resolutions.at(3) == app::ConflictPolicy::Skip);
    CHECK(cmd.resolutions.at(4) == app::ConflictPolicy::KeepBoth);

    // resolutions необязательны, parentId null - корень
    auto plain = f.api("POST", "/api/v1/entries/move", R"({"ids":[2],"parentId":null})");
    REQUIRE(plain.status == 200);
    CHECK(f.fakes.entries->lastMove->resolutions.empty());
    CHECK_FALSE(f.fakes.entries->lastMove->parent.has_value());

    f.fakes.entries->lastMove.reset();
    for (const char* bad : {R"({"ids":[2],"parentId":1,"resolutions":[]})",
                            R"({"ids":[2],"parentId":1,"resolutions":{"2":"overwrite"}})",
                            R"({"ids":[2],"parentId":1,"resolutions":{"x":"skip"}})",
                            R"({"ids":[2],"parentId":1,"resolutions":{"2":1}})", R"({"ids":[2]})",
                            R"({"parentId":1})"}) {
        INFO(bad);
        CHECK(f.api("POST", "/api/v1/entries/move", bad).status == 400);
    }
    CHECK_FALSE(f.fakes.entries->lastMove.has_value()); // мусор до сервиса не доходит

    f.fakes.entries->moveError =
        domain::Error{Code::InvalidArgument, "Нельзя переместить объект в самого себя"};
    auto refused = f.api("POST", "/api/v1/entries/move", R"({"ids":[1],"parentId":1})");
    CHECK(refused.status == 422);
    CHECK(refused.errorCode() == "invalid_argument");
    // и без токена - 401, как у остальных
    CHECK(f.request("POST", "/api/v1/entries/move", {"Content-Type: application/json"},
                    R"({"ids":[1],"parentId":null})")
              .status == 401);
}

TEST_CASE("search passes the query and returns hits with paths", "[http][search]") {
    HttpFixture f;
    auto r = f.api("GET", "/api/v1/search?q=%D0%BC%D0%BE%D1%80%D0%B5&limit=5");
    REQUIRE(r.status == 200);
    CHECK(f.fakes.search->lastQuery.text == "море");
    CHECK(f.fakes.search->lastQuery.limit == 5);
    CHECK(r.json()["results"][0]["entry"]["name"] == "море.jpg");
    CHECK(r.json()["results"][0]["path"][0]["name"] == "Отпуск");
    CHECK(f.api("GET", "/api/v1/search?q=a&limit=zero").status == 400);
}

TEST_CASE("search takes tags, match mode and scope", "[http][search][UF-18]") {
    HttpFixture f;
    auto r = f.api("GET", "/api/v1/search?q=a&tags=1,2,3&match=any&within=7&limit=9");
    REQUIRE(r.status == 200);
    const auto& query = f.fakes.search->lastQuery;
    CHECK(query.text == "a");
    CHECK(query.tags == std::vector<domain::TagId>{1, 2, 3});
    CHECK(query.match == app::TagMatch::Any);
    CHECK(query.within == 7);
    CHECK(query.limit == 9);
    CHECK(r.json()["query"] == "a");
    CHECK(r.json()["results"].size() == 1);

    // по умолчанию: без тегов, И между категориями, весь сейф
    f.api("GET", "/api/v1/search?q=a");
    CHECK(query.tags.empty());
    CHECK(query.match == app::TagMatch::Categories);
    CHECK_FALSE(query.within.has_value());
    CHECK(query.limit == app::kDefaultSearchLimit);
    // одни теги, без текста; запятая может прийти закодированной
    auto tagsOnly = f.api("GET", "/api/v1/search?tags=5%2C6&match=all");
    CHECK(tagsOnly.status == 200);
    CHECK(tagsOnly.json()["query"] == "");
    CHECK(query.text.empty());
    CHECK(query.tags == std::vector<domain::TagId>{5, 6});
    CHECK(query.match == app::TagMatch::All);
    f.api("GET", "/api/v1/search?tags=5&match=categories&within=");
    CHECK(query.match == app::TagMatch::Categories);
    CHECK_FALSE(query.within.has_value());

    // кривые параметры не доходят до сервиса
    f.fakes.search->lastQuery.text = "untouched";
    for (const char* bad :
         {"tags=1,,2", "tags=1,", "tags=,1", "tags=a", "tags=0", "tags=-1", "tags=1.5",
          "match=some", "match=ALL", "within=abc", "within=0", "within=-3", "limit=0"}) {
        INFO(bad);
        auto refused = f.api("GET", std::string("/api/v1/search?q=a&") + bad);
        CHECK(refused.status == 400);
        CHECK(refused.errorCode() == "bad_request");
    }
    CHECK(query.text == "untouched");

    f.fakes.search->error = domain::Error{Code::InvalidArgument, "Неизвестный тег"};
    auto unknownTag = f.api("GET", "/api/v1/search?tags=999");
    CHECK(unknownTag.status == 422);
    CHECK(unknownTag.errorCode() == "invalid_argument");
    f.fakes.search->error = domain::Error{Code::NotFound, "Объект не найден"};
    CHECK(f.api("GET", "/api/v1/search?q=a&within=999").status == 404);
}

TEST_CASE("GET /tags answers with the categories, their tags and the counts", "[http][tags]") {
    HttpFixture f;
    auto r = f.api("GET", "/api/v1/tags");
    REQUIRE(r.status == 200);
    CHECK(r.json() == http::Json::parse(R"({"categories":[{"id":1,"name":"Люди","tags":[)"
                                        R"({"id":10,"categoryId":1,"name":"Ирис","count":2},)"
                                        R"({"id":11,"categoryId":1,"name":"Рокси","count":0}]},)"
                                        R"({"id":2,"name":"Язык","tags":[]}]})"));

    f.fakes.tags->categories.clear();
    CHECK(f.api("GET", "/api/v1/tags").json() == http::Json({{"categories", http::Json::array()}}));
    CHECK(f.request("GET", "/api/v1/tags").status == 401);
    f.fakes.tags->error = domain::Error{Code::Locked, "Сейф заблокирован"};
    CHECK(f.api("GET", "/api/v1/tags").status == 401);
}

TEST_CASE("category endpoints: create, rename, remove", "[http][tags][UF-17]") {
    HttpFixture f;
    auto created = f.api("POST", "/api/v1/tags/categories", R"({"name":"Новая"})");
    REQUIRE(created.status == 201);
    CHECK(created.json() ==
          http::Json({{"id", 3}, {"name", "Новая"}, {"tags", http::Json::array()}}));
    CHECK(f.fakes.tags->lastName == "Новая");

    auto renamed = f.api("PATCH", "/api/v1/tags/categories/1", R"({"name":"Персонажи"})");
    REQUIRE(renamed.status == 200);
    CHECK(renamed.json()["id"] == 1);
    CHECK(renamed.json()["name"] == "Персонажи");
    REQUIRE(renamed.json()["tags"].size() == 2); // с тегами и счетчиками
    CHECK(renamed.json()["tags"][0]["count"] == 2);
    CHECK(f.fakes.tags->lastId == 1);
    CHECK(f.fakes.tags->lastName == "Персонажи");

    auto removed = f.api("DELETE", "/api/v1/tags/categories/2");
    REQUIRE(removed.status == 200);
    CHECK(removed.json() == http::Json({{"removedTags", 4}, {"affectedEntries", 7}}));
    CHECK(f.fakes.tags->lastId == 2);

    // не то, что просили
    f.fakes.tags->lastName = "untouched";
    CHECK(f.api("POST", "/api/v1/tags/categories", R"({})").status == 400);
    CHECK(f.api("POST", "/api/v1/tags/categories", R"({"name":5})").status == 400);
    CHECK(f.api("POST", "/api/v1/tags/categories", R"([])").status == 400);
    CHECK(f.api("PATCH", "/api/v1/tags/categories/1", R"({})").status == 400);
    CHECK(f.api("PATCH", "/api/v1/tags/categories/abc", R"({"name":"x"})").status == 400);
    CHECK(f.api("PATCH", "/api/v1/tags/categories/0", R"({"name":"x"})").status == 400);
    CHECK(f.api("DELETE", "/api/v1/tags/categories/abc").status == 400);
    CHECK(f.fakes.tags->lastName == "untouched");

    // ответы сервиса: дубль, нет категории, плохое имя
    f.fakes.tags->error =
        domain::Error{Code::AlreadyExists, "Категория с таким названием уже есть"};
    auto duplicate = f.api("POST", "/api/v1/tags/categories", R"({"name":"Люди"})");
    CHECK(duplicate.status == 409);
    CHECK(duplicate.errorCode() == "already_exists");
    CHECK(f.api("PATCH", "/api/v1/tags/categories/2", R"({"name":"Люди"})").status == 409);
    f.fakes.tags->error = domain::Error{Code::NotFound, "Категория не найдена"};
    CHECK(f.api("PATCH", "/api/v1/tags/categories/99", R"({"name":"x"})").status == 404);
    CHECK(f.api("DELETE", "/api/v1/tags/categories/99").status == 404);
    f.fakes.tags->error = domain::Error{Code::InvalidArgument, "Название не может быть пустым"};
    auto empty = f.api("POST", "/api/v1/tags/categories", R"({"name":""})");
    CHECK(empty.status == 422);
    CHECK(empty.errorCode() == "invalid_argument");
}

TEST_CASE("POST /tags creates a tag: 201, or 200 when it was there", "[http][tags][UF-16]") {
    HttpFixture f;
    auto created = f.api("POST", "/api/v1/tags", R"({"category":"Люди","name":"Ирис"})");
    REQUIRE(created.status == 201);
    CHECK(created.json() == http::Json({{"id", 12}, {"categoryId", 1}, {"name", "Ирис"}}));
    REQUIRE(f.fakes.tags->lastCreate.has_value());
    CHECK(f.fakes.tags->lastCreate->category == "Люди");
    CHECK(f.fakes.tags->lastCreate->name == "Ирис");
    CHECK_FALSE(f.fakes.tags->lastCreate->createCategory);

    f.api("POST", "/api/v1/tags", R"({"category":"Новая","name":"Тег","createCategory":true})");
    CHECK(f.fakes.tags->lastCreate->createCategory);
    f.api("POST", "/api/v1/tags", R"({"category":"Новая","name":"Тег","createCategory":false})");
    CHECK_FALSE(f.fakes.tags->lastCreate->createCategory);

    f.fakes.tags->tagCreated = false;
    auto existing = f.api("POST", "/api/v1/tags", R"({"category":"Люди","name":"ирис"})");
    CHECK(existing.status == 200);
    CHECK(existing.json()["id"] == 12);

    f.fakes.tags->lastCreate.reset();
    for (const char* bad : {R"({})", R"({"category":"Люди"})", R"({"name":"Ирис"})",
                            R"({"category":1,"name":"Ирис"})", R"({"category":"Люди","name":null})",
                            R"({"category":"Люди","name":"Ирис","createCategory":"yes"})"}) {
        INFO(bad);
        CHECK(f.api("POST", "/api/v1/tags", bad).status == 400);
    }
    CHECK_FALSE(f.fakes.tags->lastCreate.has_value());

    f.fakes.tags->error = domain::Error{Code::NotFound, "Категория не найдена"};
    auto noCategory = f.api("POST", "/api/v1/tags", R"({"category":"Нет","name":"Тег"})");
    CHECK(noCategory.status == 404);
    CHECK(noCategory.errorCode() == "not_found");
    f.fakes.tags->error =
        domain::Error{Code::InvalidArgument, "Название не может содержать двоеточие"};
    CHECK(f.api("POST", "/api/v1/tags", R"({"category":"Люди","name":"a:b"})").status == 422);
}

TEST_CASE("tag endpoints: update, merge, remove", "[http][tags][UF-17]") {
    HttpFixture f;
    auto renamed = f.api("PATCH", "/api/v1/tags/10", R"({"name":"Ирис Грейрат"})");
    REQUIRE(renamed.status == 200);
    CHECK(renamed.json() == http::Json({{"id", 10}, {"categoryId", 1}, {"name", "Ирис Грейрат"}}));
    CHECK(f.fakes.tags->lastId == 10);
    CHECK(f.fakes.tags->lastUpdate->name == "Ирис Грейрат");
    CHECK_FALSE(f.fakes.tags->lastUpdate->categoryId.has_value());

    auto moved = f.api("PATCH", "/api/v1/tags/10", R"({"categoryId":2})");
    REQUIRE(moved.status == 200);
    CHECK(moved.json()["categoryId"] == 2);
    CHECK_FALSE(f.fakes.tags->lastUpdate->name.has_value());
    f.api("PATCH", "/api/v1/tags/10", R"({"name":"Ирис","categoryId":2})");
    CHECK(f.fakes.tags->lastUpdate->name == "Ирис");
    CHECK(f.fakes.tags->lastUpdate->categoryId == 2);

    auto merged = f.api("POST", "/api/v1/tags/10/merge", R"({"into":11})");
    REQUIRE(merged.status == 200);
    CHECK(merged.json() == http::Json({{"affectedEntries", 5}}));
    CHECK(f.fakes.tags->lastId == 10);
    CHECK(f.fakes.tags->lastInto == 11);

    auto removed = f.api("DELETE", "/api/v1/tags/10");
    REQUIRE(removed.status == 200);
    CHECK(removed.json() == http::Json({{"affectedEntries", 6}}));

    f.fakes.tags->lastUpdate.reset();
    f.fakes.tags->lastId = 0;
    for (const char* bad :
         {R"({})", R"({"name":5})", R"({"categoryId":"2"})", R"({"categoryId":0})",
          R"({"categoryId":1.5})", R"({"categoryId":null})"}) {
        INFO(bad);
        CHECK(f.api("PATCH", "/api/v1/tags/10", bad).status == 400);
    }
    CHECK(f.api("PATCH", "/api/v1/tags/abc", R"({"name":"x"})").status == 400);
    for (const char* bad : {R"({})", R"({"into":"11"})", R"({"into":0})", R"({"into":null})"}) {
        INFO(bad);
        CHECK(f.api("POST", "/api/v1/tags/10/merge", bad).status == 400);
    }
    CHECK(f.api("POST", "/api/v1/tags/abc/merge", R"({"into":1})").status == 400);
    CHECK(f.api("DELETE", "/api/v1/tags/-1").status == 400);
    CHECK_FALSE(f.fakes.tags->lastUpdate.has_value());
    CHECK(f.fakes.tags->lastId == 0);

    f.fakes.tags->error = domain::Error{Code::AlreadyExists, "Такой тег в этой категории уже есть"};
    CHECK(f.api("PATCH", "/api/v1/tags/10", R"({"name":"Рокси"})").status == 409);
    f.fakes.tags->error = domain::Error{Code::NotFound, "Тег не найден"};
    CHECK(f.api("PATCH", "/api/v1/tags/99", R"({"name":"x"})").status == 404);
    CHECK(f.api("POST", "/api/v1/tags/99/merge", R"({"into":1})").status == 404);
    CHECK(f.api("DELETE", "/api/v1/tags/99").status == 404);
    f.fakes.tags->error = domain::Error{Code::InvalidArgument, "Нельзя слить тег с самим собой"};
    CHECK(f.api("POST", "/api/v1/tags/10/merge", R"({"into":10})").status == 422);
}

TEST_CASE("POST /entries/tags adds and removes tags on several entries", "[http][tags][UF-16]") {
    HttpFixture f;
    auto r =
        f.api("POST", "/api/v1/entries/tags",
              R"({"ids":[2,3],"add":[{"tagId":10,"inherit":true},{"tagId":11}],"remove":[12]})");
    REQUIRE(r.status == 200);
    CHECK(r.json() == http::Json({{"updated", 3}}));
    REQUIRE(f.fakes.tags->lastAssign.has_value());
    const auto& cmd = *f.fakes.tags->lastAssign;
    CHECK(cmd.ids == std::vector<domain::EntryId>{2, 3});
    CHECK(cmd.add == std::vector<domain::TagAssignment>{{10, true}, {11, false}});
    CHECK(cmd.remove == std::vector<domain::TagId>{12});

    // одно из двух достаточно
    CHECK(f.api("POST", "/api/v1/entries/tags", R"({"ids":[2],"remove":[10]})").status == 200);
    CHECK(f.fakes.tags->lastAssign->add.empty());
    CHECK(f.api("POST", "/api/v1/entries/tags", R"({"ids":[2],"add":[]})").status == 200);
    CHECK(f.fakes.tags->lastAssign->remove.empty());

    f.fakes.tags->lastAssign.reset();
    for (const char* bad :
         {R"({"ids":[2]})", R"({"add":[{"tagId":1}]})", R"({"ids":"2","add":[]})",
          R"({"ids":[0],"add":[]})", R"({"ids":[2],"add":{"tagId":1}})", R"({"ids":[2],"add":[1]})",
          R"({"ids":[2],"add":[{}]})", R"({"ids":[2],"add":[{"tagId":0}]})",
          R"({"ids":[2],"add":[{"tagId":"1"}]})",
          R"({"ids":[2],"add":[{"tagId":1,"inherit":"yes"}]})", R"({"ids":[2],"remove":5})",
          R"({"ids":[2],"remove":["1"]})", R"({"ids":[2],"remove":[0]})"}) {
        INFO(bad);
        auto refused = f.api("POST", "/api/v1/entries/tags", bad);
        CHECK(refused.status == 400);
        CHECK(refused.errorCode() == "bad_request");
    }
    CHECK_FALSE(f.fakes.tags->lastAssign.has_value()); // мусор до сервиса не доходит
    CHECK(f.request("POST", "/api/v1/entries/tags", {"Content-Type: application/json"},
                    R"({"ids":[2],"add":[]})")
              .status == 401);

    f.fakes.tags->error = domain::Error{Code::InvalidArgument, "Неизвестный тег"};
    CHECK(f.api("POST", "/api/v1/entries/tags", R"({"ids":[2],"add":[{"tagId":99}]})").status ==
          422);
    f.fakes.tags->error = domain::Error{Code::NotFound, "Объект не найден"};
    CHECK(f.api("POST", "/api/v1/entries/tags", R"({"ids":[99],"add":[{"tagId":1}]})").status ==
          404);
}

TEST_CASE("import streams multipart parts into the import session", "[http][import]") {
    HttpFixture f;
    const std::string body =
        "--XyZ\r\n"
        "Content-Disposition: form-data; name=\"note\"\r\n\r\n"
        "text field is ignored\r\n"
        "--XyZ\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"Папка/sub/a.txt\"\r\n"
        "Content-Type: text/plain\r\n\r\n"
        "hello\r\nworld\r\n"
        "--XyZ\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"b.bin\"\r\n\r\n"
        "BINARY\r\n"
        "--XyZ--\r\n";
    auto empty =
        f.api("POST", "/api/v1/import", {}, {"Content-Type: multipart/form-data; boundary=XyZ"});
    CHECK(empty.status == 200); // пустая форма - ничего не импортировано
    CHECK(empty.json()["imported"] == 0);

    auto headers = std::vector<std::string>{"Authorization: Bearer " + std::string(kApiToken),
                                            "Content-Type: multipart/form-data; boundary=XyZ",
                                            "Origin: http://127.0.0.1:8900"};
    auto ok = f.request("POST", "/api/v1/import?parentId=1", headers, body);
    REQUIRE(ok.status == 200);
    CHECK(ok.json()["imported"] == 2);
    CHECK(ok.json()["replaced"] == 0);
    CHECK(ok.json()["skipped"] == 0);
    CHECK(f.fakes.importExport->importParent == 1);
    const auto& files = f.fakes.importExport->imported;
    REQUIRE(files.size() == 2);
    CHECK(files.at("Папка/sub/a.txt") == "hello\r\nworld");
    CHECK(files.at("b.bin") == "BINARY");

    auto notMultipart = f.api("POST", "/api/v1/import", R"({"x":1})");
    CHECK(notMultipart.status == 415);
}

namespace {

std::string multipartPart(const std::string& disposition, const std::string& content,
                          const std::string& type = {}) {
    return "--XyZ\r\nContent-Disposition: " + disposition + "\r\n" +
           (type.empty() ? "" : "Content-Type: " + type + "\r\n") + "\r\n" + content + "\r\n";
}

std::string manifestPart(const std::string& json) {
    return multipartPart("form-data; name=\"manifest\"", json, "application/json");
}

std::string filePart(const std::string& filename, const std::string& content) {
    return multipartPart("form-data; name=\"file\"; filename=\"" + filename + "\"", content);
}

Reply postImport(HttpFixture& f, const std::string& parts, const std::string& query = {}) {
    return f.request("POST", "/api/v1/import" + query,
                     {"Authorization: Bearer " + std::string(kApiToken),
                      "Content-Type: multipart/form-data; boundary=XyZ",
                      "Origin: http://127.0.0.1:8900"},
                     parts + "--XyZ--\r\n");
}

} // namespace

TEST_CASE("import applies the manifest to the files it names", "[http][import][manifest]") {
    HttpFixture f;
    const std::string manifest =
        R"({"files":{"Папка/a.txt":{"lastModified":1700000000123,"onConflict":"replace"},)"
        R"("b.bin":{"onConflict":"skip"},"d.txt":{"lastModified":1700000000000.9},)"
        R"("e.txt":{"lastModified":null,"onConflict":null},"unused.txt":{}}})";
    auto ok = postImport(f, manifestPart(manifest) + filePart("Папка/a.txt", "AAA") +
                                filePart("b.bin", "BBB") + filePart("c.txt", "CCC") +
                                filePart("d.txt", "DDD") + filePart("e.txt", "EEE"));
    REQUIRE(ok.status == 200);
    CHECK(ok.json()["imported"] == 5); // счет ведет фейковая сессия
    const auto& files = f.fakes.importExport->imported;
    CHECK(files.size() == 5); // сам manifest файлом не стал
    CHECK(files.at("Папка/a.txt") == "AAA");
    const auto& options = f.fakes.importExport->importOptions;
    CHECK(options.at("Папка/a.txt").sourceModifiedAt == 1'700'000'000'123);
    CHECK(options.at("Папка/a.txt").onConflict == app::ConflictPolicy::Replace);
    CHECK_FALSE(options.at("b.bin").sourceModifiedAt.has_value());
    CHECK(options.at("b.bin").onConflict == app::ConflictPolicy::Skip);
    // файла нет в manifest: без даты и keepBoth
    CHECK_FALSE(options.at("c.txt").sourceModifiedAt.has_value());
    CHECK(options.at("c.txt").onConflict == app::ConflictPolicy::KeepBoth);
    CHECK(options.at("d.txt").sourceModifiedAt == 1'700'000'000'000); // дробные мс отбрасываются
    CHECK(options.at("e.txt").onConflict == app::ConflictPolicy::KeepBoth); // null - как нет
    CHECK_FALSE(options.at("e.txt").sourceModifiedAt.has_value());

    SECTION("a part named manifest with a filename is a file") {
        f.fakes.importExport->imported.clear();
        auto plain =
            postImport(f, filePart("x.txt", "X") +
                              multipartPart("form-data; name=\"manifest\"; filename=\"manifest\"",
                                            "not json"));
        REQUIRE(plain.status == 200);
        CHECK(f.fakes.importExport->imported.size() == 2);
        CHECK(f.fakes.importExport->imported.at("manifest") == "not json");
    }
    SECTION("the manifest without files is fine") {
        f.fakes.importExport->imported.clear();
        CHECK(postImport(f, manifestPart("{}") + filePart("x.txt", "X")).status == 200);
        CHECK(f.fakes.importExport->imported.size() == 1);
    }
}

TEST_CASE("a bad manifest is refused before any file is imported", "[http][import][manifest]") {
    HttpFixture f;
    const auto refused = [&](const std::string& parts) {
        auto reply = postImport(f, parts);
        CHECK(reply.status == 400);
        CHECK(reply.errorCode() == "bad_request");
    };

    SECTION("broken JSON") {
        refused(manifestPart("{\"files\": ") + filePart("a.txt", "A"));
        CHECK(f.fakes.importExport->imported.empty());
    }
    SECTION("not an object") {
        refused(manifestPart("[1,2]") + filePart("a.txt", "A"));
        refused(manifestPart("\"text\"") + filePart("a.txt", "A"));
        CHECK(f.fakes.importExport->imported.empty());
    }
    SECTION("wrong shapes") {
        const auto file = filePart("a.txt", "A");
        refused(manifestPart(R"({"files":[]})") + file);
        refused(manifestPart(R"({"files":{"a.txt":5}})") + file);
        refused(manifestPart(R"({"files":{"a.txt":{"onConflict":"overwrite"}}})") + file);
        refused(manifestPart(R"({"files":{"a.txt":{"onConflict":1}}})") + file);
        refused(manifestPart(R"({"files":{"a.txt":{"lastModified":"yesterday"}}})") + file);
        refused(manifestPart(R"({"files":{"a.txt":{"lastModified":1e300}}})") + file);
        CHECK(f.fakes.importExport->imported.empty());
    }
    SECTION("the manifest is the last part") {
        refused(filePart("a.txt", "A") + manifestPart("{}"));
    }
    SECTION("the manifest after a text field is not the first part either") {
        refused(multipartPart("form-data; name=\"note\"", "text") + manifestPart("{}") +
                filePart("a.txt", "A"));
        CHECK(f.fakes.importExport->imported.empty());
    }
    SECTION("the manifest is over 32 MiB") {
        auto huge = postImport(f, manifestPart(std::string(32 * 1024 * 1024 + 1, ' ')) +
                                      filePart("a.txt", "A"));
        CHECK(huge.status == 413);
        CHECK(huge.errorCode() == "payload_too_large");
        CHECK(f.fakes.importExport->imported.empty());
        // ровно 32 МиБ еще проходят
        auto edge = postImport(f, manifestPart("{" + std::string(32 * 1024 * 1024 - 2, ' ') + "}") +
                                      filePart("a.txt", "A"));
        CHECK(edge.status == 200);
    }
    SECTION("two manifests") {
        refused(manifestPart("{}") + manifestPart("{}") + filePart("a.txt", "A"));
        CHECK(f.fakes.importExport->imported.empty()); // до файла дело не дошло
    }
}

TEST_CASE("import/plan passes the paths on and answers with the clashes", "[http][import][plan]") {
    HttpFixture f;
    auto existing = makeEntry(2, 1, domain::Kind::Photo, "море.jpg");
    existing.meta.sourceModifiedAt = 1'700'000'000'000;
    f.fakes.importExport->plan = app::ImportPlan{{{"Отпуск/море.jpg", existing}}, 2};

    auto r = f.api("POST", "/api/v1/import/plan?parentId=1",
                   R"({"files":[{"path":"Отпуск/море.jpg","size":11},{"path":"a.txt","size":5},)"
                   R"({"path":"b.txt","size":0}]})");
    REQUIRE(r.status == 200);
    CHECK(r.json()["newFiles"] == 2);
    REQUIRE(r.json()["conflicts"].size() == 1);
    CHECK(r.json()["conflicts"][0]["path"] == "Отпуск/море.jpg");
    CHECK(r.json()["conflicts"][0]["existing"]["id"] == 2);
    CHECK(r.json()["conflicts"][0]["existing"]["name"] == "море.jpg");
    CHECK(r.json()["conflicts"][0]["existing"]["sourceModifiedAt"] == 1'700'000'000'000);
    CHECK(f.fakes.importExport->planParent == 1);
    const auto& planned = f.fakes.importExport->planFiles;
    REQUIRE(planned.size() == 3);
    CHECK(planned[0].path == "Отпуск/море.jpg");
    CHECK(planned[0].size == 11);
    CHECK(planned[1].path == "a.txt");

    // без parentId - корень; размер необязателен
    auto root = f.api("POST", "/api/v1/import/plan", R"({"files":[{"path":"x"}]})");
    REQUIRE(root.status == 200);
    CHECK_FALSE(f.fakes.importExport->planParent.has_value());
    CHECK(f.fakes.importExport->planFiles[0].size == 0);
    auto none = f.api("POST", "/api/v1/import/plan", R"({"files":[]})");
    CHECK(none.status == 200);

    // без токена - как у остальных
    CHECK(f.request("POST", "/api/v1/import/plan", {"Content-Type: application/json"},
                    R"({"files":[]})")
              .status == 401);
}

TEST_CASE("import/plan rejects a malformed body and too many files", "[http][import][plan]") {
    HttpFixture f;
    const auto bad = [&](const std::string& body, const std::string& query = {}) {
        auto r = f.api("POST", "/api/v1/import/plan" + query, body);
        CHECK(r.status == 400);
        CHECK(r.errorCode() == "bad_request");
    };
    bad(R"({"files":)");
    bad(R"([])");
    bad(R"({})");
    bad(R"({"files":{}})");
    bad(R"({"files":[1]})");
    bad(R"({"files":[{"size":1}]})");
    bad(R"({"files":[{"path":5}]})");
    bad(R"({"files":[{"path":"a","size":-1}]})");
    bad(R"({"files":[{"path":"a","size":"big"}]})");
    bad(R"({"files":[]})", "?parentId=abc");
    CHECK(f.fakes.importExport->planFiles.empty());

    // предел - 100 000 файлов; тело такого плана намного больше обычного лимита JSON (1 КиБ в
    // тесте)
    const auto plan = [](std::size_t count) {
        std::string body = R"({"files":[)";
        for (std::size_t i = 0; i < count; ++i) {
            body += i == 0 ? "" : ",";
            body += R"({"path":"папка/файл)" + std::to_string(i) + R"(.txt","size":1})";
        }
        return body + "]}";
    };
    auto full = f.api("POST", "/api/v1/import/plan", plan(100'000));
    REQUIRE(full.status == 200);
    CHECK(f.fakes.importExport->planFiles.size() == 100'000);
    auto over = f.api("POST", "/api/v1/import/plan", plan(100'001));
    CHECK(over.status == 422);
    CHECK(over.errorCode() == "invalid_argument");

    // остальные JSON-маршруты по-прежнему держат малый лимит
    CHECK(f.api("POST", "/api/v1/entries/move/plan", plan(100)).status == 413);
}

TEST_CASE("import/plan maps service errors", "[http][import][plan]") {
    HttpFixture f;
    f.fakes.importExport->planError = domain::Error{Code::NotFound, "Объект не найден"};
    auto r = f.api("POST", "/api/v1/import/plan?parentId=99", R"({"files":[]})");
    CHECK(r.status == 404);
    CHECK(r.errorCode() == "not_found");
}

TEST_CASE("media content supports Range and never renders active content inline", "[http][media]") {
    HttpFixture f;
    auto full = f.api("GET", "/api/v1/media/2/content");
    CHECK(full.status == 200);
    CHECK(full.body == "0123456789ABCDEF");
    CHECK(full.header("content-type") == "image/jpeg");
    CHECK(full.header("accept-ranges") == "bytes");
    CHECK(full.header("content-disposition").rfind("inline;", 0) == 0);
    CHECK(full.header("content-security-policy").find("sandbox") != std::string::npos);

    auto part = f.api("GET", "/api/v1/media/2/content", {}, {"Range: bytes=4-9"});
    CHECK(part.status == 206);
    CHECK(part.body == "456789");
    CHECK(part.header("content-range") == "bytes 4-9/16");

    auto tail = f.api("GET", "/api/v1/media/2/content", {}, {"Range: bytes=-3"});
    CHECK(tail.status == 206);
    CHECK(tail.body == "DEF");
    CHECK(f.api("GET", "/api/v1/media/2/content", {}, {"Range: bytes=100-200"}).status == 416);

    auto html = f.api("GET", "/api/v1/media/4/content"); // файл-HTML из сейфа
    CHECK(html.status == 200);
    CHECK(html.header("content-type") == "application/octet-stream");
    CHECK(html.header("content-disposition").rfind("attachment;", 0) == 0);
    CHECK(html.header("x-content-type-options") == "nosniff");

    CHECK(f.api("GET", "/api/v1/media/404/content").status == 404);
    CHECK(f.api("GET", "/api/v1/media/2/thumbnail").header("content-type") == "image/jpeg");
}

TEST_CASE("downloads carry the original name; folders stream as zip", "[http][media][UF-11]") {
    HttpFixture f;
    auto file = f.api("GET", "/api/v1/media/2/download");
    CHECK(file.status == 200);
    CHECK(file.header("content-type") == "application/octet-stream");
    CHECK(file.header("content-disposition") ==
          "attachment; filename=\"____.jpg\"; filename*=UTF-8''%D0%BC%D0%BE%D1%80%D0%B5.jpg");

    auto zip = f.api("GET", "/api/v1/media/1/zip");
    CHECK(zip.status == 200);
    CHECK(zip.header("transfer-encoding") == "chunked");
    CHECK(zip.header("content-type") == "application/zip");
    CHECK(zip.body == "PK-fake-zip");
    CHECK(zip.header("content-disposition").find("filename*=UTF-8''%D0%9E") != std::string::npos);

    auto viaDownload = f.api("GET", "/api/v1/media/1/download"); // папка -> zip
    CHECK(viaDownload.body == "PK-fake-zip");
    auto notFolder = f.api("GET", "/api/v1/media/2/zip");
    CHECK(notFolder.status == 422); // не папка и без вложений
    CHECK(notFolder.errorCode() == "invalid_argument");
}

TEST_CASE("zip names: folder.zip, and 'name (вложения).zip' for an entry with attachments",
          "[http][media][UF-11][UF-14]") {
    HttpFixture f;
    auto folder = f.api("GET", "/api/v1/media/1/zip");
    CHECK(folder.header("content-disposition")
              .find("filename*=UTF-8''%D0%9E%D1%82%D0%BF%D1%83%D1%81%D0%BA.zip") !=
          std::string::npos);
    CHECK(f.fakes.importExport->lastZipId == 1);

    f.fakes.entries->entries[2].childCount = 2; // у фото появились вложения
    auto attachments = f.api("GET", "/api/v1/media/2/zip");
    REQUIRE(attachments.status == 200);
    CHECK(attachments.body == "PK-fake-zip");
    CHECK(attachments.header("content-type") == "application/zip");
    CHECK(f.fakes.importExport->lastZipId == 2);
    // "море.jpg (вложения).zip"
    CHECK(
        attachments.header("content-disposition")
            .find(
                "filename*=UTF-8''%D0%BC%D0%BE%D1%80%D0%B5.jpg%20%28%D0%B2%D0%BB%D0%BE%D0%B6%D0%B5"
                "%D0%BD%D0%B8%D1%8F%29.zip") != std::string::npos);

    // скачивание записи с вложениями отдает саму запись, а не архив
    auto file = f.api("GET", "/api/v1/media/2/download");
    CHECK(file.header("content-type") == "application/octet-stream");
    CHECK(file.body == "0123456789ABCDEF");
}

TEST_CASE("frontend assets and unknown routes", "[http][assets]") {
    HttpFixture f;
    auto stub = f.request("GET", "/");
    CHECK(stub.status == 200);
    CHECK(stub.header("content-type").rfind("text/html", 0) == 0);
    CHECK(stub.header("content-security-policy").find("frame-ancestors 'none'") !=
          std::string::npos);
    CHECK(f.request("GET", "/some/page").status == 404);

    auto unknownApi = f.api("GET", "/api/v1/nothing");
    CHECK(unknownApi.status == 404);
    CHECK(unknownApi.errorCode() == "not_found");
    auto unknownPost = f.api("POST", "/api/v1/nothing", R"({})");
    CHECK(unknownPost.status == 404);
    CHECK(unknownPost.errorCode() == "not_found");

    f.assets.enabled = true;
    auto index = f.request("GET", "/");
    CHECK(index.body == "<!doctype html><title>SafeBox</title>");
    CHECK(index.header("cache-control") == "no-cache");
    auto spa = f.request("GET", "/folder/42"); // клиентский маршрут -> index.html
    CHECK(spa.status == 200);
    CHECK(spa.body == index.body);
    auto js = f.request("GET", "/assets/app-1234.js");
    CHECK(js.body == "console.log(1)");
    CHECK(js.header("cache-control").find("immutable") != std::string::npos);
    CHECK(f.request("GET", "/missing.png").status == 404);
}
