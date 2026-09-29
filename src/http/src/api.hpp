// Внутренний заголовок http-слоя. Тесты тоже его подключают, чтобы гонять маршруты без сети
#pragma once

#include <httplib.h>

#include <nlohmann/json.hpp>

#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "safebox/app/services.hpp"
#include "safebox/domain/io.hpp"
#include "safebox/domain/model/entry.hpp"
#include "safebox/domain/model/error.hpp"
#include "safebox/http/server.hpp"

namespace safebox::http {

using Json = nlohmann::json;

inline constexpr std::string_view kImportPath = "/api/v1/import";
inline constexpr std::string_view kImportPlanPath = "/api/v1/import/plan";
// Лимит JSON плана и manifest импорта: на 100 000 файлов с длинными путями обычного лимита мало.
inline constexpr std::size_t kMaxImportJsonBytes = 32 * 1024 * 1024;
inline constexpr std::string_view kMediaPrefix = "/api/v1/media/";
inline constexpr std::string_view kMediaCookieName = "sbx_media";

struct ApiContext {
    ApiContext(app::Services services, AssetProvider& assets, HttpConfig config);

    app::Services services;
    AssetProvider& assets;
    HttpConfig config;
    int port = 0;
    std::vector<std::string> allowedHosts;   // нижний регистр: "127.0.0.1:8900"
    std::vector<std::string> allowedOrigins; // нижний регистр: "http://127.0.0.1:8900"

    void setPort(int actualPort); // после bind: порт известен -> белые списки
    void log(std::string_view line) const;
};

void registerRoutes(httplib::Server& server, ApiContext& ctx);

// guard_mw.cpp
[[nodiscard]] httplib::Server::HandlerResponse
guardRequest(const ApiContext& ctx, const httplib::Request& req, httplib::Response& res);
void applySecurityHeaders(const httplib::Request& req, httplib::Response& res);

// auth_mw.cpp
[[nodiscard]] std::string bearerToken(const httplib::Request& req);
[[nodiscard]] std::string cookieValue(const httplib::Request& req, std::string_view name);
// Нет/мертвый токен -> 401 уже отправлен, nullopt.
[[nodiscard]] std::optional<app::Lease> requireApi(ApiContext& ctx, const httplib::Request& req,
                                                   httplib::Response& res);
// Bearer или media-cookie - только для GET-медиа (теги <img>/<video>, скачивание).
[[nodiscard]] std::optional<app::Lease> requireMedia(ApiContext& ctx, const httplib::Request& req,
                                                     httplib::Response& res);
[[nodiscard]] std::string mediaCookie(std::string_view token);
[[nodiscard]] std::string expiredMediaCookie();

// errors.cpp
[[nodiscard]] int statusFor(domain::Error::Code code) noexcept;
[[nodiscard]] std::string_view wireCodeFor(domain::Error::Code code) noexcept;
void sendError(httplib::Response& res, const domain::Error& error);
void sendError(httplib::Response& res, int status, std::string_view code, std::string_view message);
void sendJson(httplib::Response& res, const Json& body, int status = 200);
void sendNoContent(httplib::Response& res);
void replaceHeader(httplib::Response& res, const std::string& key, const std::string& value);
// error_handler: тело пустое (ошибка самого httplib или неизвестный маршрут) -> JSON.
[[nodiscard]] httplib::Server::HandlerResponse fillEmptyError(const httplib::Request& req,
                                                              httplib::Response& res);

// json.cpp
// Тело -> JSON-объект; иначе 400 уже отправлен.
[[nodiscard]] std::optional<Json> readJsonObject(const httplib::Request& req,
                                                 httplib::Response& res);
// Обязательное строковое поле; иначе 400 уже отправлен.
[[nodiscard]] std::optional<std::string> requireString(const Json& body, const char* field,
                                                       httplib::Response& res);
// Необязательное строковое поле: нет - value не тронут; неверный тип -> 400 уже отправлен.
[[nodiscard]] bool readOptionalString(const Json& body, const char* field,
                                      std::optional<std::string>& value, httplib::Response& res);
[[nodiscard]] std::optional<domain::EntryId> parseId(std::string_view text) noexcept;
// "keepBoth" | "replace" | "skip"
[[nodiscard]] std::optional<app::ConflictPolicy>
parseConflictPolicy(std::string_view text) noexcept;
// Content-Disposition с ASCII-запасным именем и filename* (RFC 5987, UTF-8).
[[nodiscard]] std::string contentDisposition(std::string_view type, std::string_view fileName);

// ByteSink поверх DataSink cpp-httplib: запись в сокет ответа.
class DataSinkAdapter final : public domain::ByteSink {
public:
    explicit DataSinkAdapter(httplib::DataSink& sink) noexcept : sink_(sink) {}

    [[nodiscard]] domain::Status write(std::span<const std::byte> data) override {
        if (data.empty()) {
            return {};
        }
        if (!sink_.write(reinterpret_cast<const char*>(data.data()), data.size())) {
            return domain::fail(domain::Error::Code::IoError, "Соединение с клиентом прервано");
        }
        return {};
    }

private:
    httplib::DataSink& sink_;
};

// контроллеры
void registerSafeApi(httplib::Server& server, ApiContext& ctx);
void registerEntriesApi(httplib::Server& server, ApiContext& ctx);
void registerImportApi(httplib::Server& server, ApiContext& ctx);
void registerMediaApi(httplib::Server& server, ApiContext& ctx);
void registerSearchApi(httplib::Server& server, ApiContext& ctx);
void registerTagsApi(httplib::Server& server, ApiContext& ctx);

} // namespace safebox::http
