// /api/v1/settings: настройки приложения. От сейфа не зависят, но, как и весь API, только с Bearer
#include "dto.hpp"

namespace safebox::http {

void registerSettingsApi(httplib::Server& server, ApiContext& ctx) {
    server.Get("/api/v1/settings", [&ctx](const httplib::Request& req, httplib::Response& res) {
        if (!requireApi(ctx, req, res)) {
            return;
        }
        sendJson(res, toJson(ctx.services.settings->get()));
    });

    server.Patch("/api/v1/settings", [&ctx](const httplib::Request& req, httplib::Response& res) {
        if (!requireApi(ctx, req, res)) {
            return;
        }
        auto body = readJsonObject(req, res);
        if (!body) {
            return;
        }
        const auto it = body->find("linkPreviews");
        if (it == body->end()) {
            sendError(res, 400, "bad_request", "Укажите хотя бы одно поле: linkPreviews");
            return;
        }
        if (!it->is_boolean()) {
            sendError(res, 400, "bad_request", "Поле 'linkPreviews' должно быть true или false");
            return;
        }
        auto settings = ctx.services.settings->get();
        settings.linkPreviews = it->get<bool>();
        auto saved = ctx.services.settings->update(settings);
        if (!saved) {
            sendError(res, saved.error());
            return;
        }
        sendJson(res, toJson(*saved));
    });
}

} // namespace safebox::http
