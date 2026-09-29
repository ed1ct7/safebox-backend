// Проверки до роутинга. На 127.0.0.1 может постучаться любая страница в браузере, поэтому:
// - Host только из белого списка (DNS rebinding)
// - Origin на изменяющих запросах, Sec-Fetch-Site: cross-site режем
// - тело только JSON и с лимитом размера (кроме импорта; у плана импорта лимит свой)
#include <algorithm>
#include <charconv>
#include <format>

#include "api.hpp"
#include "safebox/domain/model/rules.hpp"

namespace safebox::http {

namespace {

using HandlerResponse = httplib::Server::HandlerResponse;

[[nodiscard]] std::string lower(std::string_view s) {
    std::string out(domain::detail::trim(s));
    for (auto& c : out) {
        c = domain::detail::asciiLower(c);
    }
    return out;
}

[[nodiscard]] bool listed(const std::vector<std::string>& list, const std::string& value) {
    return std::find(list.begin(), list.end(), value) != list.end();
}

[[nodiscard]] bool isMutating(const std::string& method) {
    return method == "POST" || method == "PUT" || method == "PATCH" || method == "DELETE";
}

HandlerResponse reject(httplib::Response& res, int status, std::string_view code,
                       std::string_view message) {
    sendError(res, status, code, message);
    return HandlerResponse::Handled;
}

} // namespace

ApiContext::ApiContext(app::Services services_, AssetProvider& assets_, HttpConfig config_)
    : services(std::move(services_)), assets(assets_), config(std::move(config_)) {
    setPort(config.port);
}

void ApiContext::setPort(int actualPort) {
    port = actualPort;
    allowedHosts.clear();
    allowedOrigins.clear();
    const auto addHost = [&](std::string_view host) {
        auto value = lower(host);
        if (value.empty()) {
            return;
        }
        // "хост" без порта -> с портом сервера; IPv6 - в скобках
        const bool bracketed = value.starts_with('[');
        const auto colon = value.rfind(':');
        const bool hasPort =
            bracketed ? (value.find("]:") != std::string::npos) : (colon != std::string::npos);
        if (!hasPort) {
            value += std::format(":{}", port);
        }
        if (!listed(allowedHosts, value)) {
            allowedHosts.push_back(value);
            allowedOrigins.push_back("http://" + value);
        }
    };
    addHost("127.0.0.1");
    addHost("localhost");
    addHost("[::1]");
    for (const auto& host : config.extraHosts) {
        addHost(host);
    }
    for (const auto& origin : config.extraOrigins) {
        auto value = lower(origin);
        while (value.ends_with('/')) {
            value.pop_back();
        }
        if (!value.empty() && !listed(allowedOrigins, value)) {
            allowedOrigins.push_back(value);
        }
    }
}

void ApiContext::log(std::string_view line) const {
    if (config.log) {
        config.log(line);
    }
}

HandlerResponse guardRequest(const ApiContext& ctx, const httplib::Request& req,
                             httplib::Response& res) {
    if (!listed(ctx.allowedHosts, lower(req.get_header_value("Host")))) {
        return reject(res, 403, "forbidden", "Недопустимый заголовок Host");
    }
    if (!isMutating(req.method)) {
        return HandlerResponse::Unhandled;
    }

    if (req.has_header("Origin") &&
        !listed(ctx.allowedOrigins, lower(req.get_header_value("Origin")))) {
        return reject(res, 403, "forbidden", "Запрос с чужого сайта отклонён");
    }
    if (lower(req.get_header_value("Sec-Fetch-Site")) == "cross-site") {
        return reject(res, 403, "forbidden", "Запрос с чужого сайта отклонён");
    }

    if (req.path == kImportPath) {
        return HandlerResponse::Unhandled; // multipart, поток без лимита длины
    }
    if (req.has_header("Transfer-Encoding")) {
        return reject(res, 411, "length_required", "Требуется заголовок Content-Length");
    }
    if (req.has_header("Content-Length")) {
        const auto text = req.get_header_value("Content-Length");
        std::uint64_t length = 0;
        const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), length);
        const auto maxBody =
            req.path == kImportPlanPath ? kMaxImportJsonBytes : ctx.config.maxJsonBody;
        if (ec != std::errc{} || length > maxBody) {
            return reject(res, 413, "payload_too_large", "Слишком большое тело запроса");
        }
    }
    if (req.has_header("Content-Type")) {
        const auto type = lower(req.get_header_value("Content-Type"));
        if (type != "application/json" && !type.starts_with("application/json;")) {
            return reject(res, 415, "unsupported_media_type",
                          "Ожидается Content-Type: application/json");
        }
    }
    return HandlerResponse::Unhandled;
}

void applySecurityHeaders(const httplib::Request& req, httplib::Response& res) {
    replaceHeader(res, "X-Content-Type-Options", "nosniff");
    replaceHeader(res, "Referrer-Policy", "no-referrer");
    replaceHeader(res, "X-Frame-Options", "DENY");
    replaceHeader(res, "Cross-Origin-Resource-Policy", "same-origin");
    const bool api = req.path.starts_with("/api/") || req.path == "/health";
    if (!api) {
        return; // статика: заголовки кэша и CSP ставит раздача ассетов
    }
    replaceHeader(res, "Cache-Control", "no-store");
    if (req.path.starts_with(kMediaPrefix)) {
        // медиа, открытое напрямую, не исполняет ничего
        replaceHeader(res, "Content-Security-Policy",
                      "default-src 'none'; img-src 'self'; media-src 'self'; sandbox");
    } else {
        replaceHeader(res, "Content-Security-Policy", "default-src 'none'; frame-ancestors 'none'");
    }
}

} // namespace safebox::http
