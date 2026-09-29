// Параметры запуска. Размер куска и kdf тут не настраиваются - они хранятся в файле сейфа
#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "safebox/domain/model/error.hpp"

namespace safebox::daemon {

struct Config {
    std::string host = "127.0.0.1";
    int port = 8900;
    std::vector<std::string> allowedHosts;
    std::vector<std::string> allowedOrigins;
    std::chrono::seconds idleTimeout{15 * 60};
    std::chrono::seconds presenceTimeout{2 * 60};
    std::filesystem::path defaultDirectory;
    bool quiet = false;
    bool noWindow = false; // сервер без окна приложения (Windows; на других системах окна нет)
    bool showHelp = false;
    bool showVersion = false;
};

// Имя переменной -> значение (UTF-8) или nullopt.
using EnvLookup = std::function<std::optional<std::string>(std::string_view)>;

// args - аргументы без имени программы, в UTF-8.
[[nodiscard]] domain::Result<Config> parseConfig(std::span<const std::string> args,
                                                 const EnvLookup& env);
[[nodiscard]] std::optional<std::string> systemEnv(std::string_view name);
[[nodiscard]] std::filesystem::path documentsDirectory(const EnvLookup& env);
// Файл настроек приложения: %APPDATA%\SafeBox\settings.ini; не Windows - $XDG_CONFIG_HOME или
// ~/.config, каталог safebox. Пусто - каталог пользователя определить не удалось.
[[nodiscard]] std::filesystem::path settingsFile(const EnvLookup& env);
[[nodiscard]] bool isLoopbackHost(std::string_view host);
[[nodiscard]] std::string usage();

} // namespace safebox::daemon
