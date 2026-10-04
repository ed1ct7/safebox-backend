// /api/v1/settings: настройки приложения. От сейфа не зависят, но, как и весь API, только с Bearer
#include <optional>

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
        // Частичный апдейт: передано то, что меняется; оба поля опциональны.
        std::optional<bool> linkPreviews;
        if (const auto it = body->find("linkPreviews"); it != body->end()) {
            if (!it->is_boolean()) {
                sendError(res, 400, "bad_request",
                          "Поле 'linkPreviews' должно быть true или false");
                return;
            }
            linkPreviews = it->get<bool>();
        }
        std::optional<domain::TagLanguage> tagLanguage;
        if (const auto it = body->find("tagLanguage"); it != body->end()) {
            if (!it->is_string() ||
                (it->get<std::string>() != "ru" && it->get<std::string>() != "en")) {
                sendError(res, 400, "bad_request",
                          "Поле 'tagLanguage' должно быть \"ru\" или \"en\"");
                return;
            }
            tagLanguage = domain::tagLanguage(it->get<std::string>());
        }
        if (!linkPreviews && !tagLanguage) {
            sendError(res, 400, "bad_request",
                      "Укажите хотя бы одно поле: linkPreviews, tagLanguage");
            return;
        }
        auto settings = ctx.services.settings->get();
        if (linkPreviews) {
            settings.linkPreviews = *linkPreviews;
        }
        if (tagLanguage) {
            settings.tagLanguage = *tagLanguage;
        }
        auto saved = ctx.services.settings->update(settings);
        if (!saved) {
            sendError(res, saved.error());
            return;
        }
        sendJson(res, toJson(*saved));
    });
}

} // namespace safebox::http
