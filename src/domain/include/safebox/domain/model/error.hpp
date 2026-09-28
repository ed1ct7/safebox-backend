// Ошибка домена + Result/Status поверх std::expected.
// В HTTP-коды переводится в одном месте - src/http/src/errors.cpp
#pragma once

#include <expected>
#include <string>
#include <string_view>
#include <utility>

namespace safebox::domain {

struct Error {
    enum class Code {
        WrongPassword,
        Locked,
        NotFound,
        AlreadyExists,
        InvalidArgument,
        NotASafe,
        IoError,
        IntegrityError,
        Cancelled,
        Internal,
    };

    Code code = Code::Internal;
    std::string message;
};

template <class T>
using Result = std::expected<T, Error>;

// Результат операции без значения: {} - успех, unexpected(Error) - ошибка.
using Status = Result<void>;

[[nodiscard]] inline std::unexpected<Error> fail(Error::Code code, std::string message) {
    return std::unexpected<Error>(Error{code, std::move(message)});
}

[[nodiscard]] inline std::unexpected<Error> fail(Error error) {
    return std::unexpected<Error>(std::move(error));
}

// Имя кода для логов и сообщений тестов (не проводной формат - он в http).
[[nodiscard]] constexpr std::string_view toString(Error::Code code) noexcept {
    switch (code) {
    case Error::Code::WrongPassword:
        return "WrongPassword";
    case Error::Code::Locked:
        return "Locked";
    case Error::Code::NotFound:
        return "NotFound";
    case Error::Code::AlreadyExists:
        return "AlreadyExists";
    case Error::Code::InvalidArgument:
        return "InvalidArgument";
    case Error::Code::NotASafe:
        return "NotASafe";
    case Error::Code::IoError:
        return "IoError";
    case Error::Code::IntegrityError:
        return "IntegrityError";
    case Error::Code::Cancelled:
        return "Cancelled";
    case Error::Code::Internal:
        return "Internal";
    }
    return "Unknown";
}

} // namespace safebox::domain
