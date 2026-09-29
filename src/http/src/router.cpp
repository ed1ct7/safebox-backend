// Маршруты. Полный список с форматами - контракт REST API в Linqtab
#include "api.hpp"

namespace safebox::http {

namespace {

constexpr std::string_view kStubPage =
    "<!doctype html><html lang=\"ru\"><head><meta charset=\"utf-8\">"
    "<title>SafeBox</title></head><body style=\"font-family:sans-serif;background:#111;"
    "color:#ddd;padding:2em\"><h1>SafeBox API работает</h1><p>Веб-интерфейс не встроен в эту "
    "сборку (соберите с -DSAFEBOX_WEB_DIST=&lt;frontend/dist&gt;) или откройте dev-сервер "
    "фронтенда.</p></body></html>";

constexpr std::string_view kSpaCsp =
    "default-src 'self'; img-src 'self' data: blob:; media-src 'self' blob:; "
    "style-src 'self' 'unsafe-inline'; script-src 'self'; connect-src 'self'; "
    "object-src 'none'; base-uri 'none'; form-action 'self'; frame-ancestors 'none'";

[[nodiscard]] bool hasExtension(std::string_view path) {
    const auto slash = path.rfind('/');
    const auto dot = path.rfind('.');
    return dot != std::string_view::npos && (slash == std::string_view::npos || dot > slash);
}

void serveAsset(const ApiContext& ctx, const httplib::Request& req, httplib::Response& res) {
    if (req.path.starts_with("/api/")) {
        sendError(res, 404, "not_found", "Неизвестный адрес API");
        return;
    }
    if (ctx.assets.empty()) {
        if (req.path == "/" || req.path == "/index.html") {
            res.status = 200;
            res.set_content(std::string(kStubPage), "text/html; charset=utf-8");
            replaceHeader(res, "Cache-Control", "no-cache");
            replaceHeader(res, "Content-Security-Policy", std::string(kSpaCsp));
            return;
        }
        sendError(res, 404, "not_found", "Страница не найдена");
        return;
    }
    auto asset = ctx.assets.find(req.path == "/" ? "/index.html" : req.path);
    if (!asset && !hasExtension(req.path)) {
        asset = ctx.assets.find("/index.html"); // клиентский маршрут SPA
    }
    if (!asset) {
        sendError(res, 404, "not_found", "Файл не найден");
        return;
    }
    const auto body = asset->body; // вшитые данные живут все время работы процесса
    res.status = 200;
    res.set_content_provider(
        body.size(), std::string(asset->contentType),
        [body](std::size_t offset, std::size_t length, httplib::DataSink& sink) {
            return sink.write(body.data() + offset, length);
        });
    // Vite кладет хэшированные файлы в /assets/ - их можно кэшировать навсегда.
    replaceHeader(res, "Cache-Control",
                  req.path.starts_with("/assets/") ? "public, max-age=31536000, immutable"
                                                   : "no-cache");
    if (asset->contentType.starts_with("text/html")) {
        replaceHeader(res, "Content-Security-Policy", std::string(kSpaCsp));
    }
}

} // namespace

void registerRoutes(httplib::Server& server, ApiContext& ctx) {
    server.set_pre_routing_handler([&ctx](const httplib::Request& req, httplib::Response& res) {
        return guardRequest(ctx, req, res);
    });
    server.set_post_routing_handler([](const httplib::Request& req, httplib::Response& res) {
        applySecurityHeaders(req, res);
    });
    server.set_error_handler([](const httplib::Request& req, httplib::Response& res) {
        return fillEmptyError(req, res);
    });
    server.set_exception_handler(
        [&ctx](const httplib::Request& req, httplib::Response& res, std::exception_ptr error) {
            std::string what = "неизвестное исключение";
            try {
                if (error) {
                    std::rethrow_exception(error);
                }
            } catch (const std::exception& e) {
                what = e.what();
            } catch (...) {
            }
            ctx.log("exception in " + req.method + " " + req.path + ": " + what);
            sendError(res, 500, "internal", "Внутренняя ошибка сервера");
        });

    registerSafeApi(server, ctx);
    registerEntriesApi(server, ctx);
    registerImportApi(server, ctx);
    registerMediaApi(server, ctx);
    registerSearchApi(server, ctx);
    registerTagsApi(server, ctx);
    registerLinksApi(server, ctx);
    registerSettingsApi(server, ctx);

    // последним: все, что не API, - фронтенд
    server.Get(".*", [&ctx](const httplib::Request& req, httplib::Response& res) {
        serveAsset(ctx, req, res);
    });
}

} // namespace safebox::http
