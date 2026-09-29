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
    CHECK(f.fakes.search->lastQuery == "море");
    CHECK(f.fakes.search->lastLimit == 5);
    CHECK(r.json()["results"][0]["entry"]["name"] == "море.jpg");
    CHECK(r.json()["results"][0]["path"][0]["name"] == "Отпуск");
    CHECK(f.api("GET", "/api/v1/search?q=a&limit=zero").status == 400);
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
    CHECK(notFolder.status == 422);
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
