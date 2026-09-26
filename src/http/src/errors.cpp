// Error::Code -> HTTP статус. Ответ всегда {"error":{"code","message"}}.
// Cancelled тоже 401: операцию прервал lock, сессии уже нет
#include "api.hpp"

namespace safebox::http {

using Code = domain::Error::Code;

int statusFor(Code code) noexcept {
    switch (code) {
    case Code::Locked:
    case Code::Cancelled:
        return 401;
    case Code::WrongPassword:
        return 403;
    case Code::NotFound:
        return 404;
    case Code::AlreadyExists:
        return 409;
    case Code::InvalidArgument:
    case Code::NotASafe:
        return 422;
    case Code::IoError:
    case Code::IntegrityError:
    case Code::Internal:
        return 500;
    }
    return 500;
}

std::string_view wireCodeFor(Code code) noexcept {
    switch (code) {
    case Code::Locked:
    case Code::Cancelled:
        return "unauthorized";
    case Code::WrongPassword:
        return "wrong_password";
    case Code::NotFound:
        return "not_found";
    case Code::AlreadyExists:
        return "already_exists";
    case Code::InvalidArgument:
        return "invalid_argument";
    case Code::NotASafe:
        return "not_a_safe";
    case Code::IoError:
        return "io_error";
    case Code::IntegrityError:
        return "integrity_error";
    case Code::Internal:
        return "internal";
    }
    return "internal";
}

void replaceHeader(httplib::Response& res, const std::string& key, const std::string& value) {
    res.headers.erase(key);
    res.set_header(key, value);
}

void sendJson(httplib::Response& res, const Json& body, int status) {
    res.status = status;
    // replace: даже если где-то проскочит битый UTF-8, ответ не упадет исключением
    res.set_content(body.dump(-1, ' ', false, Json::error_handler_t::replace),
                    "application/json; charset=utf-8");
}

void sendNoContent(httplib::Response& res) {
    res.status = 204;
    res.body.clear();
}

void sendError(httplib::Response& res, int status, std::string_view code,
               std::string_view message) {
    sendJson(res, Json{{"error", Json{{"code", code}, {"message", message}}}}, status);
    if (status == 401) {
        replaceHeader(res, "WWW-Authenticate", "Bearer");
    }
}

void sendError(httplib::Response& res, const domain::Error& error) {
    sendError(res, statusFor(error.code), wireCodeFor(error.code), error.message);
}

namespace {

struct TransportError {
    std::string_view code;
    std::string_view message;
};

[[nodiscard]] TransportError transportError(int status) noexcept {
    switch (status) {
    case 400:
        return {"bad_request", "Некорректный запрос"};
    case 401:
        return {"unauthorized", "Требуется вход в сейф"};
    case 403:
        return {"forbidden", "Доступ запрещён"};
    case 404:
        return {"not_found", "Не найдено"};
    case 405:
        return {"method_not_allowed", "Метод не поддерживается"};
    case 411:
        return {"length_required", "Требуется заголовок Content-Length"};
    case 413:
        return {"payload_too_large", "Слишком большое тело запроса"};
    case 414:
        return {"uri_too_long", "Слишком длинный адрес"};
    case 415:
        return {"unsupported_media_type", "Неподдерживаемый тип содержимого"};
    case 416:
        return {"range_not_satisfiable", "Запрошенный диапазон вне файла"};
    default:
        return {status >= 500 ? "internal" : "error", "Ошибка сервера"};
    }
}

} // namespace

httplib::Server::HandlerResponse fillEmptyError(const httplib::Request& /*req*/,
                                                httplib::Response& res) {
    if (!res.body.empty() || res.content_provider_) {
        return httplib::Server::HandlerResponse::Unhandled; // тело уже наше
    }
    const auto error = transportError(res.status);
    sendError(res, res.status, error.code, error.message);
    return httplib::Server::HandlerResponse::Handled;
}

} // namespace safebox::http
