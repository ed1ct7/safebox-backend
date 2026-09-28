// Авторизация: Bearer для всего API, для GET медиа еще можно HttpOnly cookie
// (в <img>/<video> заголовок не добавишь). 401 только когда сессия мертвая,
// неверный пароль это 403 - на 401 фронт выкидывает на экран входа
#include <format>

#include "api.hpp"
#include "safebox/domain/model/rules.hpp"

namespace safebox::http {

namespace {

[[nodiscard]] std::optional<app::Lease> authorizeWith(ApiContext& ctx, std::string_view token,
                                                      app::TokenScope scope,
                                                      httplib::Response& res) {
    auto lease = ctx.services.safe->authorize(token, scope);
    if (!lease) {
        sendError(res, lease.error());
        return std::nullopt;
    }
    return std::move(*lease);
}

} // namespace

std::string bearerToken(const httplib::Request& req) {
    const auto value = req.get_header_value("Authorization");
    constexpr std::string_view prefix = "Bearer ";
    if (value.size() <= prefix.size() ||
        !domain::detail::iequals(std::string_view(value).substr(0, prefix.size()), prefix)) {
        return {};
    }
    return std::string(domain::detail::trim(std::string_view(value).substr(prefix.size())));
}

std::string cookieValue(const httplib::Request& req, std::string_view name) {
    const auto count = req.get_header_value_count("Cookie");
    for (std::size_t i = 0; i < count; ++i) {
        const std::string header = req.get_header_value("Cookie", "", i);
        std::string_view line = header;
        while (!line.empty()) {
            const auto semi = line.find(';');
            const auto pair = domain::detail::trim(line.substr(0, semi));
            line = semi == std::string_view::npos ? std::string_view{} : line.substr(semi + 1);
            const auto eq = pair.find('=');
            if (eq != std::string_view::npos && domain::detail::trim(pair.substr(0, eq)) == name) {
                return std::string(domain::detail::trim(pair.substr(eq + 1)));
            }
        }
    }
    return {};
}

std::optional<app::Lease> requireApi(ApiContext& ctx, const httplib::Request& req,
                                     httplib::Response& res) {
    const auto token = bearerToken(req);
    if (token.empty()) {
        sendError(res, 401, "unauthorized", "Требуется вход в сейф");
        return std::nullopt;
    }
    return authorizeWith(ctx, token, app::TokenScope::Api, res);
}

std::optional<app::Lease> requireMedia(ApiContext& ctx, const httplib::Request& req,
                                       httplib::Response& res) {
    if (const auto token = bearerToken(req); !token.empty()) {
        return authorizeWith(ctx, token, app::TokenScope::Api, res);
    }
    const auto cookie = cookieValue(req, kMediaCookieName);
    if (cookie.empty()) {
        sendError(res, 401, "unauthorized", "Требуется вход в сейф");
        return std::nullopt;
    }
    return authorizeWith(ctx, cookie, app::TokenScope::Media, res);
}

std::string mediaCookie(std::string_view token) {
    return std::format("{}={}; Path=/api/v1/media; HttpOnly; SameSite=Strict", kMediaCookieName,
                       token);
}

std::string expiredMediaCookie() {
    return std::format("{}=; Path=/api/v1/media; HttpOnly; SameSite=Strict; Max-Age=0",
                       kMediaCookieName);
}

} // namespace safebox::http
