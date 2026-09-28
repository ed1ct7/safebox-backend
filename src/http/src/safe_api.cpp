// /health и /api/v1/safe/*
#include "dto.hpp"

namespace safebox::http {

namespace {

void wipe(std::optional<std::string>& secret) {
    if (secret) {
        domain::secureWipe(*secret);
    }
}

void sendSession(httplib::Response& res, const app::UnlockResult& result, int status) {
    replaceHeader(res, "Set-Cookie", mediaCookie(result.tokens.media));
    sendJson(res, Json{{"token", result.tokens.api}, {"safe", toJson(result.safe)}}, status);
}

} // namespace

void registerSafeApi(httplib::Server& server, ApiContext& ctx) {
    server.Get("/health", [&ctx](const httplib::Request&, httplib::Response& res) {
        sendJson(res, Json{{"status", "ok"}, {"version", ctx.config.version}});
    });

    server.Get("/api/v1/safe/status", [&ctx](const httplib::Request& req, httplib::Response& res) {
        const auto status = ctx.services.safe->publicStatus();
        Json body = toJson(status);
        body["authorized"] = false;
        if (const auto token = bearerToken(req); status.unlocked && !token.empty()) {
            if (auto lease = ctx.services.safe->authorize(token, app::TokenScope::Api)) {
                if (auto info = ctx.services.safe->info(*lease)) {
                    body["authorized"] = true;
                    body["safe"] = toJson(*info);
                }
            }
        }
        // сейф мог только что заблокироваться по неактивности
        body["unlocked"] = ctx.services.safe->publicStatus().unlocked;
        sendJson(res, body);
    });

    server.Post("/api/v1/safe/create", [&ctx](const httplib::Request& req, httplib::Response& res) {
        auto body = readJsonObject(req, res);
        if (!body) {
            return;
        }
        auto path = requireString(*body, "path", res);
        auto password = path ? requireString(*body, "password", res) : std::nullopt;
        auto confirm = password ? requireString(*body, "confirm", res) : std::nullopt;
        if (!confirm) {
            wipe(password);
            return;
        }
        auto result = ctx.services.safe->create({*path, *password, *confirm});
        wipe(password);
        wipe(confirm);
        if (!result) {
            sendError(res, result.error());
            return;
        }
        sendSession(res, *result, 201);
    });

    server.Post("/api/v1/safe/unlock", [&ctx](const httplib::Request& req, httplib::Response& res) {
        auto body = readJsonObject(req, res);
        if (!body) {
            return;
        }
        auto path = requireString(*body, "path", res);
        auto password = path ? requireString(*body, "password", res) : std::nullopt;
        if (!password) {
            return;
        }
        auto result = ctx.services.safe->unlock({*path, *password});
        wipe(password);
        if (!result) {
            sendError(res, result.error());
            return;
        }
        sendSession(res, *result, 200);
    });

    server.Post("/api/v1/safe/lock", [&ctx](const httplib::Request& req, httplib::Response& res) {
        {
            // аренда нужна только для проверки токена: lock ждет освобождения аренд
            auto lease = requireApi(ctx, req, res);
            if (!lease) {
                return;
            }
        }
        ctx.services.safe->lock();
        replaceHeader(res, "Set-Cookie", expiredMediaCookie());
        sendNoContent(res);
    });

    server.Post("/api/v1/safe/password", [&ctx](const httplib::Request& req,
                                                httplib::Response& res) {
        auto lease = requireApi(ctx, req, res);
        if (!lease) {
            return;
        }
        auto body = readJsonObject(req, res);
        if (!body) {
            return;
        }
        auto oldPassword = requireString(*body, "oldPassword", res);
        auto newPassword = oldPassword ? requireString(*body, "newPassword", res) : std::nullopt;
        auto confirm = newPassword ? requireString(*body, "confirm", res) : std::nullopt;
        if (!confirm) {
            wipe(oldPassword);
            wipe(newPassword);
            return;
        }
        auto result =
            ctx.services.safe->changePassword(*lease, {*oldPassword, *newPassword, *confirm});
        wipe(oldPassword);
        wipe(newPassword);
        wipe(confirm);
        if (!result) {
            sendError(res, result.error());
            return;
        }
        sendNoContent(res);
    });

    server.Post(
        "/api/v1/safe/heartbeat", [&ctx](const httplib::Request& req, httplib::Response& res) {
            auto lease = requireApi(ctx, req, res);
            if (!lease) {
                return;
            }
            auto body = readJsonObject(req, res);
            if (!body) {
                return;
            }
            bool active = false;
            if (const auto it = body->find("active"); it != body->end()) {
                if (!it->is_boolean()) {
                    sendError(res, 400, "bad_request", "Поле 'active' должно быть true/false");
                    return;
                }
                active = it->get<bool>();
            }
            auto result = ctx.services.safe->heartbeat(*lease, active);
            if (!result) {
                sendError(res, result.error());
                return;
            }
            sendJson(res, Json{{"idleRemainingSec", result->idleRemainingSec}});
        });
}

} // namespace safebox::http
