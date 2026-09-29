// Загрузка страниц для предпросмотра ссылок: WinHTTP (системный прокси и хранилище
// сертификатов). Редиректы ведем сами, чтобы на каждом шаге заново проверить адрес:
// публичный сайт не должен перенаправить нас во внутреннюю сеть.
// Не Windows - заглушка: предпросмотр там не поддерживается
#include <memory>
#include <string>
#include <utility>

#include "safebox/domain/ports/web.hpp"
#include "safebox/infra/factories.hpp"

#if defined(_WIN32)

#include <winsock2.h>
#include <ws2tcpip.h>

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <climits>
#include <cstdint>
#include <iterator>
#include <optional>
#include <span>
#include <string_view>
#include <system_error>

#include "internal_address.hpp"
#include "safebox/domain/model/link_preview.hpp"

namespace safebox::infra {
namespace {

using SteadyClock = std::chrono::steady_clock;
using domain::Bytes;
using domain::Error;
using domain::FetchRequest;
using domain::FetchResponse;
using domain::Result;

constexpr int kMaxRedirects = 5;
constexpr wchar_t kUserAgent[] = L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
                                 L"(KHTML, like Gecko) Chrome/140.0.0.0 Safari/537.36 SafeBox/2";
constexpr wchar_t kRequestHeaders[] =
    L"Accept: text/html,application/xhtml+xml,image/png,image/jpeg,image/webp,image/gif;q=0.9,"
    L"*/*;q=0.5\r\n";

struct HandleCloser {
    void operator()(void* handle) const noexcept { WinHttpCloseHandle(handle); }
};
using Handle = std::unique_ptr<void, HandleCloser>;

[[nodiscard]] std::unexpected<Error> failIo(std::string message) {
    return domain::fail(Error::Code::IoError, std::move(message));
}

[[nodiscard]] std::wstring toWide(std::string_view utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                                         static_cast<int>(utf8.size()), nullptr, 0);
    if (size <= 0) {
        return {};
    }
    std::wstring out(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()),
                        out.data(), size);
    return out;
}

[[nodiscard]] std::string toUtf8(std::wstring_view wide) {
    if (wide.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }
    std::string out(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), size,
                        nullptr, nullptr);
    return out;
}

// Хост в ASCII: кириллические и прочие IDN-имена -> punycode (WinHTTP этого не делает).
[[nodiscard]] std::optional<std::string> toAsciiHost(std::string_view host) {
    const bool ascii =
        std::ranges::all_of(host, [](char c) { return static_cast<unsigned char>(c) < 0x80; });
    if (ascii) {
        return std::string(host);
    }
    const auto wide = toWide(host);
    if (wide.empty()) {
        return std::nullopt;
    }
    const int size = IdnToAscii(0, wide.c_str(), static_cast<int>(wide.size()), nullptr, 0);
    if (size <= 0) {
        return std::nullopt;
    }
    std::wstring converted(static_cast<std::size_t>(size), L'\0');
    const int written =
        IdnToAscii(0, wide.c_str(), static_cast<int>(wide.size()), converted.data(), size);
    if (written <= 0) {
        return std::nullopt;
    }
    std::string out;
    for (const wchar_t c :
         std::wstring_view(converted).substr(0, static_cast<std::size_t>(written))) {
        out.push_back(static_cast<char>(c));
    }
    return out;
}

// Путь и запрос для запроса: пробелы, не-ASCII и прочее "небезопасное" - %XX (UTF-8 байты).
[[nodiscard]] std::string escapeRequestTarget(std::string_view raw) {
    constexpr std::string_view kHex = "0123456789ABCDEF";
    constexpr std::string_view kUnsafe = "\"<>\\^`{|}";
    std::string out;
    out.reserve(raw.size());
    for (const char c : raw) {
        const auto u = static_cast<unsigned char>(c);
        if (u <= 0x20 || u >= 0x7F || kUnsafe.find(c) != std::string_view::npos) {
            out.push_back('%');
            out.push_back(kHex[u >> 4]);
            out.push_back(kHex[u & 0x0F]);
        } else {
            out.push_back(c);
        }
    }
    return out;
}

struct Target {
    std::string host; // ASCII; IPv6 без скобок
    INTERNET_PORT port = 0;
    bool secure = false;
    std::string requestPath; // путь и запрос, уже с экранированием
};

[[nodiscard]] Result<Target> parseTarget(std::string_view url) {
    constexpr auto kBadUrl = "Некорректный адрес";
    Target target;
    if (domain::detail::istartsWith(url, "https://")) {
        target.secure = true;
        url.remove_prefix(8);
    } else if (domain::detail::istartsWith(url, "http://")) {
        url.remove_prefix(7);
    } else {
        return failIo("Поддерживаются только адреса http и https");
    }
    url = url.substr(0, url.find('#'));
    auto authority = url.substr(0, url.find_first_of("/?"));
    const auto rest = url.substr(authority.size());
    if (const auto at = authority.rfind('@'); at != std::string_view::npos) {
        authority.remove_prefix(at + 1); // логин и пароль из адреса не отправляем
    }

    std::string_view host = authority;
    std::string_view port;
    if (authority.starts_with('[')) {
        const auto close = authority.find(']');
        if (close == std::string_view::npos) {
            return failIo(kBadUrl);
        }
        host = authority.substr(1, close - 1);
        const auto tail = authority.substr(close + 1);
        if (!tail.empty()) {
            if (tail.front() != ':') {
                return failIo(kBadUrl);
            }
            port = tail.substr(1);
        }
    } else if (const auto colon = authority.rfind(':'); colon != std::string_view::npos) {
        host = authority.substr(0, colon);
        port = authority.substr(colon + 1);
    }

    target.port = static_cast<INTERNET_PORT>(target.secure ? INTERNET_DEFAULT_HTTPS_PORT
                                                           : INTERNET_DEFAULT_HTTP_PORT);
    if (!port.empty()) {
        unsigned value = 0;
        const auto [end, error] = std::from_chars(port.data(), port.data() + port.size(), value);
        if (error != std::errc{} || end != port.data() + port.size() || value == 0 ||
            value > 65535) {
            return failIo(kBadUrl);
        }
        target.port = static_cast<INTERNET_PORT>(value);
    }
    auto ascii = host.empty() ? std::nullopt : toAsciiHost(host);
    if (!ascii) {
        return failIo(kBadUrl);
    }
    target.host = std::move(*ascii);
    target.requestPath =
        escapeRequestTarget(rest.starts_with('/') ? std::string(rest) : "/" + std::string(rest));
    return target;
}

// Все адреса хоста должны быть публичными: имя, у которого хоть один адрес внутренний,
// не пускаем (иначе можно выбрать, какой из них подставит соединение). WinHTTP потом
// резолвит имя сам, так что это защита от прямых ссылок и редиректов, не от DNS rebinding.
[[nodiscard]] domain::Status checkPublicHost(const std::string& host) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* found = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &found) != 0 || found == nullptr) {
        return failIo("Не удалось найти сайт");
    }
    const std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> results(found, &freeaddrinfo);
    for (const auto* entry = found; entry != nullptr; entry = entry->ai_next) {
        std::span<const std::uint8_t> ip;
        if (entry->ai_family == AF_INET) {
            const auto* address = reinterpret_cast<const sockaddr_in*>(entry->ai_addr);
            ip = {reinterpret_cast<const std::uint8_t*>(&address->sin_addr), 4};
        } else if (entry->ai_family == AF_INET6) {
            const auto* address = reinterpret_cast<const sockaddr_in6*>(entry->ai_addr);
            ip = {reinterpret_cast<const std::uint8_t*>(&address->sin6_addr), 16};
        }
        if (isInternalAddress(ip)) {
            return failIo("Адрес во внутренней сети - предпросмотр не загружается");
        }
    }
    return {};
}

[[nodiscard]] std::unexpected<Error> networkError(DWORD code) {
    switch (code) {
    case ERROR_WINHTTP_TIMEOUT:
        return failIo("Сайт не ответил вовремя");
    case ERROR_WINHTTP_NAME_NOT_RESOLVED:
        return failIo("Не удалось найти сайт");
    case ERROR_WINHTTP_CANNOT_CONNECT:
    case ERROR_WINHTTP_CONNECTION_ERROR:
        return failIo("Не удалось подключиться к сайту");
    case ERROR_WINHTTP_SECURE_FAILURE:
    case ERROR_WINHTTP_SECURE_CERT_DATE_INVALID:
    case ERROR_WINHTTP_SECURE_CERT_CN_INVALID:
    case ERROR_WINHTTP_SECURE_CERT_WRONG_USAGE:
    case ERROR_WINHTTP_SECURE_CERT_REV_FAILED:
    case ERROR_WINHTTP_SECURE_CERT_REVOKED:
    case ERROR_WINHTTP_SECURE_INVALID_CA:
    case ERROR_WINHTTP_SECURE_INVALID_CERT:
    case ERROR_WINHTTP_SECURE_CHANNEL_ERROR:
        return failIo("Сертификат сайта не принят");
    default:
        return failIo("Не удалось загрузить страницу (ошибка " + std::to_string(code) + ")");
    }
}

[[nodiscard]] std::optional<std::string> queryHeader(HINTERNET request, DWORD level) {
    DWORD bytes = 0;
    WinHttpQueryHeaders(request, level, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER,
                        &bytes, WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        return std::nullopt;
    }
    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    if (!WinHttpQueryHeaders(request, level, WINHTTP_HEADER_NAME_BY_INDEX, value.data(), &bytes,
                             WINHTTP_NO_HEADER_INDEX)) {
        return std::nullopt;
    }
    value.resize(bytes / sizeof(wchar_t));
    return toUtf8(value);
}

[[nodiscard]] bool isRedirect(int status) {
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

[[nodiscard]] bool isHtml(std::string_view contentType) {
    return contentType.starts_with("text/html") || contentType.starts_with("application/xhtml+xml");
}

struct Reply {
    int status = 0;
    std::string location;    // при редиректе
    std::string contentType; // при 2xx, в нижнем регистре
    Bytes body;              // при 2xx
};

class WinHttpPageFetcher final : public domain::PageFetcher {
public:
    WinHttpPageFetcher() {
        WSADATA data{};
        winsock_ = WSAStartup(MAKEWORD(2, 2), &data) == 0;
        session_.reset(WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                   WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
        if (session_) {
            // TLS 1.3 есть не везде: не вышло - хотя бы 1.2, остальное решает система
            DWORD protocols =
                WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
            if (!WinHttpSetOption(session_.get(), WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols,
                                  sizeof(protocols))) {
                protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
                WinHttpSetOption(session_.get(), WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols,
                                 sizeof(protocols));
            }
        }
    }

    ~WinHttpPageFetcher() override {
        session_.reset();
        if (winsock_) {
            WSACleanup();
        }
    }

    WinHttpPageFetcher(const WinHttpPageFetcher&) = delete;
    WinHttpPageFetcher& operator=(const WinHttpPageFetcher&) = delete;

    Result<FetchResponse> fetch(const FetchRequest& request) override {
        if (!winsock_ || !session_) {
            return failIo("Предпросмотр недоступен: не удалось инициализировать сеть");
        }
        const auto deadline = SteadyClock::now() + request.timeout;
        std::string url = request.url;
        for (int redirects = 0;; ++redirects) {
            auto target = parseTarget(url);
            if (!target) {
                return domain::fail(std::move(target.error()));
            }
            if (auto host = checkPublicHost(target->host); !host) {
                return domain::fail(std::move(host.error()));
            }
            auto reply = exchange(*target, request, deadline);
            if (!reply) {
                return domain::fail(std::move(reply.error()));
            }
            if (isRedirect(reply->status)) {
                if (redirects == kMaxRedirects) {
                    return failIo("Слишком много перенаправлений");
                }
                url = domain::detail::resolveUrl(url, reply->location);
                if (url.empty()) {
                    return failIo("Некорректное перенаправление");
                }
                continue;
            }
            if (reply->status < 200 || reply->status > 299) {
                return failIo("Сайт вернул ошибку: HTTP " + std::to_string(reply->status));
            }
            return FetchResponse{reply->status, std::move(url), std::move(reply->contentType),
                                 std::move(reply->body)};
        }
    }

private:
    // Один запрос без автоматических редиректов. Тело читается только для 2xx.
    [[nodiscard]] Result<Reply> exchange(const Target& target, const FetchRequest& request,
                                         SteadyClock::time_point deadline) const {
        const auto remaining = [&] {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - SteadyClock::now());
            return static_cast<int>(std::clamp<std::int64_t>(left.count(), 0, INT_MAX));
        };
        if (remaining() == 0) {
            return failIo("Сайт не ответил вовремя");
        }

        // IPv6-литерал WinHTTP принимает только в скобках
        const auto host = toWide(target.host.contains(':') ? "[" + target.host + "]" : target.host);
        const auto path = toWide(target.requestPath);
        const Handle connection(WinHttpConnect(session_.get(), host.c_str(), target.port, 0));
        if (!connection) {
            return networkError(GetLastError());
        }
        const Handle handle(WinHttpOpenRequest(connection.get(), L"GET", path.c_str(), nullptr,
                                               WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                               target.secure ? WINHTTP_FLAG_SECURE : 0));
        if (!handle) {
            return networkError(GetLastError());
        }

        // Редиректы только вручную (иначе не проверить адрес). Куки не храним и не шлем.
        DWORD disabled = WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_COOKIES;
        if (!WinHttpSetOption(handle.get(), WINHTTP_OPTION_DISABLE_FEATURE, &disabled,
                              sizeof(disabled))) {
            return networkError(GetLastError());
        }
        DWORD decompression = WINHTTP_DECOMPRESSION_FLAG_ALL; // не поддержано - придет без сжатия
        WinHttpSetOption(handle.get(), WINHTTP_OPTION_DECOMPRESSION, &decompression,
                         sizeof(decompression));
        // WinHTTP отсчитывает таймауты по фазам, поэтому общий срок может выйти за timeout;
        // между чтениями тела срок проверяем сами.
        const int budget = remaining();
        WinHttpSetTimeouts(handle.get(), budget, budget, budget, budget);

        if (!WinHttpSendRequest(handle.get(), kRequestHeaders,
                                static_cast<DWORD>(std::size(kRequestHeaders) - 1),
                                WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
            return networkError(GetLastError());
        }
        if (!WinHttpReceiveResponse(handle.get(), nullptr)) {
            return networkError(GetLastError());
        }

        DWORD status = 0;
        DWORD size = sizeof(status);
        if (!WinHttpQueryHeaders(
                handle.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX)) {
            return networkError(GetLastError());
        }
        Reply reply;
        reply.status = static_cast<int>(status);
        if (isRedirect(reply.status)) {
            reply.location = queryHeader(handle.get(), WINHTTP_QUERY_LOCATION).value_or("");
            return reply;
        }
        if (reply.status < 200 || reply.status > 299) {
            return reply;
        }

        auto contentType = queryHeader(handle.get(), WINHTTP_QUERY_CONTENT_TYPE).value_or("");
        reply.contentType = domain::detail::toLowerAscii(domain::detail::trim(contentType));

        // Читаем на байт больше лимита: так видно, что тело не поместилось.
        const std::size_t limit = request.maxBytes;
        while (reply.body.size() <= limit) {
            if (remaining() == 0) {
                return failIo("Сайт не ответил вовремя");
            }
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(handle.get(), &available)) {
                return networkError(GetLastError());
            }
            if (available == 0) {
                break;
            }
            const auto old = reply.body.size();
            const auto want = std::min<std::size_t>(available, limit - old + 1);
            reply.body.resize(old + want);
            DWORD read = 0;
            if (!WinHttpReadData(handle.get(), reply.body.data() + old, static_cast<DWORD>(want),
                                 &read)) {
                return networkError(GetLastError());
            }
            reply.body.resize(old + read);
            if (read == 0) {
                break;
            }
        }
        if (reply.body.size() > limit) {
            if (!isHtml(reply.contentType)) {
                return failIo("Файл слишком большой для предпросмотра");
            }
            reply.body.resize(limit); // начала страницы достаточно: нужен только <head>
        }
        return reply;
    }

    bool winsock_ = false;
    Handle session_;
};

} // namespace

std::unique_ptr<domain::PageFetcher> makePageFetcher() {
    return std::make_unique<WinHttpPageFetcher>();
}

} // namespace safebox::infra

#else

namespace safebox::infra {
namespace {

class UnsupportedPageFetcher final : public domain::PageFetcher {
public:
    domain::Result<domain::FetchResponse> fetch(const domain::FetchRequest&) override {
        return domain::fail(domain::Error::Code::IoError,
                            "Предпросмотр не поддерживается на этой платформе");
    }
};

} // namespace

std::unique_ptr<domain::PageFetcher> makePageFetcher() {
    return std::make_unique<UnsupportedPageFetcher>();
}

} // namespace safebox::infra

#endif
