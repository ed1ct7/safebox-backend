// Порядок: дефолты -> SAFEBOX_* из окружения -> аргументы.
// На винде env читаем через UTF-16, иначе кириллица в пути ломается
#include "config.hpp"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <format>
#include <iterator>
#include <system_error>

#include "safebox/domain/io.hpp"
#include "safebox/domain/model/rules.hpp"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace safebox::daemon {

namespace {

using domain::Error;
using domain::fail;
using domain::Status;

[[nodiscard]] std::vector<std::string> splitList(std::string_view text) {
    std::vector<std::string> out;
    while (!text.empty()) {
        const auto comma = text.find(',');
        const auto item = domain::detail::trim(text.substr(0, comma));
        if (!item.empty()) {
            out.emplace_back(item);
        }
        text = comma == std::string_view::npos ? std::string_view{} : text.substr(comma + 1);
    }
    return out;
}

[[nodiscard]] domain::Result<long long> parseNumber(std::string_view text, std::string_view what,
                                                    long long min, long long max) {
    long long value = 0;
    const auto* end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(text.data(), end, value);
    if (text.empty() || ec != std::errc{} || ptr != end || value < min || value > max) {
        return fail(
            Error::Code::InvalidArgument,
            std::format("{}: ожидается число от {} до {}, получено '{}'", what, min, max, text));
    }
    return value;
}

[[nodiscard]] Status apply(Config& config, std::string_view key, std::string_view value) {
    if (key == "host") {
        if (domain::detail::trim(value).empty()) {
            return fail(Error::Code::InvalidArgument, "--host: пустой адрес");
        }
        config.host = std::string(domain::detail::trim(value));
    } else if (key == "port") {
        auto port = parseNumber(value, "--port", 0, 65535);
        if (!port) {
            return std::unexpected(port.error());
        }
        config.port = static_cast<int>(*port);
    } else if (key == "allow-host") {
        for (auto& host : splitList(value)) {
            config.allowedHosts.push_back(std::move(host));
        }
    } else if (key == "allow-origin") {
        for (auto& origin : splitList(value)) {
            if (!origin.starts_with("http://") && !origin.starts_with("https://")) {
                return fail(
                    Error::Code::InvalidArgument,
                    std::format("--allow-origin: '{}' должен начинаться с http:// или https://",
                                origin));
            }
            config.allowedOrigins.push_back(std::move(origin));
        }
    } else if (key == "idle-minutes") {
        auto minutes = parseNumber(value, "--idle-minutes", 1, 24 * 60);
        if (!minutes) {
            return std::unexpected(minutes.error());
        }
        config.idleTimeout = std::chrono::minutes(*minutes);
    } else if (key == "presence-seconds") {
        auto seconds = parseNumber(value, "--presence-seconds", 30, 24 * 60 * 60);
        if (!seconds) {
            return std::unexpected(seconds.error());
        }
        config.presenceTimeout = std::chrono::seconds(*seconds);
    } else if (key == "safe-dir") {
        config.defaultDirectory = domain::pathFromUtf8(value);
    } else {
        return fail(Error::Code::InvalidArgument, std::format("неизвестный параметр --{}", key));
    }
    return {};
}

} // namespace

std::optional<std::string> systemEnv(std::string_view name) {
#if defined(_WIN32)
    const std::wstring wide(name.begin(), name.end()); // имена переменных - ASCII
    const DWORD size = GetEnvironmentVariableW(wide.c_str(), nullptr, 0);
    if (size == 0) {
        return std::nullopt;
    }
    std::wstring value(size, L'\0');
    const DWORD written = GetEnvironmentVariableW(wide.c_str(), value.data(), size);
    value.resize(written);
    return domain::pathToUtf8(std::filesystem::path(value));
#else
    const char* value = std::getenv(std::string(name).c_str());
    if (value == nullptr) {
        return std::nullopt;
    }
    return std::string(value);
#endif
}

std::filesystem::path documentsDirectory(const EnvLookup& env) {
    if (auto dir = env("SAFEBOX_SAFE_DIR"); dir && !dir->empty()) {
        return domain::pathFromUtf8(*dir);
    }
#if defined(_WIN32)
    const auto home = env("USERPROFILE");
#else
    const auto home = env("HOME");
#endif
    if (!home || home->empty()) {
        return {};
    }
    const auto base = domain::pathFromUtf8(*home);
    std::error_code ec;
    if (std::filesystem::is_directory(base / "Documents", ec)) {
        return base / "Documents";
    }
    return base;
}

bool isLoopbackHost(std::string_view host) {
    return host == "localhost" || host == "::1" || host == "[::1]" || host.starts_with("127.");
}

domain::Result<Config> parseConfig(std::span<const std::string> args, const EnvLookup& env) {
    Config config;
    config.defaultDirectory = documentsDirectory(env);

    static constexpr std::pair<std::string_view, std::string_view> kEnv[] = {
        {"SAFEBOX_HOST", "host"},
        {"SAFEBOX_PORT", "port"},
        {"SAFEBOX_ALLOW_HOSTS", "allow-host"},
        {"SAFEBOX_ALLOW_ORIGINS", "allow-origin"},
        {"SAFEBOX_IDLE_MINUTES", "idle-minutes"},
        {"SAFEBOX_PRESENCE_SECONDS", "presence-seconds"},
    };
    for (const auto& [name, key] : kEnv) {
        if (auto value = env(name); value && !value->empty()) {
            if (auto st = apply(config, key, *value); !st) {
                return fail(Error::Code::InvalidArgument,
                            std::format("{} (переменная {})", st.error().message, name));
            }
        }
    }

    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        if (arg == "--help" || arg == "-h" || arg == "/?") {
            config.showHelp = true;
        } else if (arg == "--version") {
            config.showVersion = true;
        } else if (arg == "--quiet") {
            config.quiet = true;
        } else if (arg == "--no-window") {
            config.noWindow = true;
        } else if (arg.starts_with("--")) {
            auto key = arg.substr(2);
            std::string_view value;
            const auto eq = key.find('=');
            if (eq != std::string_view::npos) {
                value = key.substr(eq + 1);
                key = key.substr(0, eq);
            }
            static constexpr std::string_view kKnown[] = {
                "host",    "port", "allow-host", "allow-origin", "idle-minutes", "presence-seconds",
                "safe-dir"};
            if (std::find(std::begin(kKnown), std::end(kKnown), key) == std::end(kKnown)) {
                return fail(Error::Code::InvalidArgument,
                            std::format("неизвестный параметр --{}", key));
            }
            if (eq != std::string_view::npos) {
                // значение уже взято из "--ключ=значение"
            } else if (i + 1 < args.size()) {
                value = args[++i];
            } else {
                return fail(Error::Code::InvalidArgument, std::format("--{}: нет значения", key));
            }
            if (auto st = apply(config, key, value); !st) {
                return std::unexpected(st.error());
            }
        } else {
            return fail(Error::Code::InvalidArgument,
                        std::format("неизвестный аргумент '{}'", arg));
        }
    }
    return config;
}

std::string usage() {
    return "Использование: safeboxd [параметры]\n"
           "\n"
           "  --host <адрес>            адрес сервера (по умолчанию 127.0.0.1)\n"
           "  --port <порт>             порт (по умолчанию 8900; 0 - любой свободный)\n"
           "  --allow-host <хост>       LAN-режим: разрешённое значение Host (можно повторять)\n"
           "  --allow-origin <origin>   доп. Origin, например http://localhost:5173 "
           "(dev-фронтенд)\n"
           "  --idle-minutes <мин>      автоблокировка без активности (по умолчанию 15)\n"
           "  --presence-seconds <с>    блокировка, если окно закрыто (по умолчанию 120)\n"
           "  --safe-dir <папка>        база относительных путей к сейфу (по умолчанию "
           "'Документы')\n"
           "  --quiet                   не печатать журнал запросов\n"
           "  --no-window               режим сервера без окна: интерфейс открывается в браузере\n"
           "                            (на Windows окно приложения - по умолчанию)\n"
           "  --version, --help\n"
           "\n"
           "Те же параметры: SAFEBOX_HOST, SAFEBOX_PORT, SAFEBOX_ALLOW_HOSTS, "
           "SAFEBOX_ALLOW_ORIGINS,\n"
           "SAFEBOX_IDLE_MINUTES, SAFEBOX_PRESENCE_SECONDS, SAFEBOX_SAFE_DIR (аргументы важнее).\n"
           "LAN-режим (--host 0.0.0.0) - только в доверенной сети: HTTPS нет.";
}

} // namespace safebox::daemon
