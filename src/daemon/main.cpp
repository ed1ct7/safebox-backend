// safeboxd: создает адаптеры, сервисы и http-сервер.
// На Windows по умолчанию открывает окно приложения (WebView2), --no-window - только сервер.
// По Ctrl+C / SIGTERM / закрытию консоли или окна останавливаем сервер и блокируем сейф
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <ctime>
#include <exception>
#include <format>
#include <mutex>
#include <optional>
#include <print>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>

#include <shellapi.h> // отдельным блоком: clang-format не должен ставить его раньше windows.h
#endif

#include "config.hpp"
#include "safebox/app/factory.hpp"
#include "safebox/domain/io.hpp"
#include "safebox/http/server.hpp"
#include "safebox/infra/factories.hpp"
#include "web_assets.hpp"

#if defined(_WIN32)
#include "app_window.hpp"
#include "single_instance.hpp"
#endif

#ifndef SAFEBOX_VERSION
#define SAFEBOX_VERSION "dev"
#endif

namespace {

std::atomic<bool> g_stopRequested{false};
std::atomic<bool> g_shutdownDone{false};

extern "C" void onSignal(int /*signal*/) {
    g_stopRequested.store(true);
}

#if defined(_WIN32)
BOOL WINAPI onConsoleEvent(DWORD event) {
    g_stopRequested.store(true);
    if (event == CTRL_CLOSE_EVENT || event == CTRL_LOGOFF_EVENT || event == CTRL_SHUTDOWN_EVENT) {
        // Windows завершит процесс сразу после возврата: дать main заблокировать сейф.
        for (int i = 0; i < 45 && !g_shutdownDone.load(); ++i) {
            Sleep(100);
        }
    }
    return TRUE;
}
#endif

// Аргументы в UTF-8 (на Windows argv - в ANSI-кодировке, кириллица теряется).
std::vector<std::string> utf8Arguments(int argc, char** argv) {
#if defined(_WIN32)
    (void)argc;
    (void)argv;
    int count = 0;
    wchar_t** wide = CommandLineToArgvW(GetCommandLineW(), &count);
    std::vector<std::string> args;
    for (int i = 1; i < count; ++i) {
        args.push_back(safebox::domain::pathToUtf8(std::filesystem::path(wide[i])));
    }
    LocalFree(wide);
    return args;
#else
    return std::vector<std::string>(argv + 1, argv + argc);
#endif
}

std::mutex g_logMutex;

void logLine(std::string_view line) {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    std::scoped_lock lock(g_logMutex);
    std::println("{:02}:{:02}:{:02} {}", local.tm_hour, local.tm_min, local.tm_sec, line);
    std::fflush(stdout);
}

#if defined(_WIN32)
// Подсистема WINDOWS: консоли у процесса нет. Из терминала подключаемся к родительской, чтобы
// журнал был виден; потоки, уже перенаправленные родителем (> файл, конвейер), не трогаем.
// Без консоли и перенаправления вывод уходит в NUL: запись в закрытый поток бросала бы исключение.
enum class Output { None, Console, Redirected };

Output g_output = Output::None;

// Поток открыт, если родитель передал дескриптор (перенаправление). OS-дескрипторы для этого не
// годятся: AttachConsole сама заполняет пустые, а поток CRT остается закрытым.
[[nodiscard]] bool isOpen(std::FILE* stream) {
    return _fileno(stream) >= 0;
}

void reopenStream(std::FILE* stream, const char* target) {
    std::FILE* reopened = nullptr;
    std::scoped_lock lock(g_logMutex); // журнал пишут и фоновые потоки
    // "w+": без права чтения GetConsoleMode отказывает, и std::println не узнает консоль
    // (кириллица уходит байтами UTF-8 в OEM-кодировку)
    (void)freopen_s(&reopened, target, "w+", stream);
}

void setupOutput() {
    const bool redirected = isOpen(stdout);
    const bool console = AttachConsole(ATTACH_PARENT_PROCESS) != 0;
    if (!redirected) {
        reopenStream(stdout, console ? "CONOUT$" : "NUL");
    }
    if (!isOpen(stderr)) {
        reopenStream(stderr, console ? "CONOUT$" : "NUL");
    }
    g_output = redirected ? Output::Redirected : console ? Output::Console : Output::None;
}

// Режим сервера, запущенный не из терминала: своя консоль, чтобы был журнал и Ctrl+C.
void ensureConsole() {
    if (g_output != Output::None || !AllocConsole()) {
        return;
    }
    g_output = Output::Console;
    SetConsoleTitleW(L"SafeBox");
    reopenStream(stdout, "CONOUT$");
    reopenStream(stderr, "CONOUT$");
}

// Запуск двойным щелчком: сообщение об ошибке показываем окном, консоли нет.
void showIfNoConsole(std::string_view message) {
    if (g_output == Output::None) {
        safebox::daemon::showMessage(message, true);
    }
}

// Адрес интерфейса для окна: 127.0.0.1 годится, пока сервер слушает loopback или все адреса.
[[nodiscard]] std::string windowUrl(const safebox::daemon::Config& config, int port) {
    std::string host = "127.0.0.1";
    if (config.host == "::1" || config.host == "[::1]") {
        host = "[::1]";
    } else if (!safebox::daemon::isLoopbackHost(config.host) && config.host != "0.0.0.0" &&
               config.host != "::") {
        host = config.host;
    }
    return std::format("http://{}:{}/", host, port);
}

// Окно не открылось (нет WebView2): интерфейс - в Edge или браузере по умолчанию,
// сервер продолжает работать как с --no-window.
void fallbackToBrowser(const std::string& url, const std::string& reason) {
    safebox::daemon::openInBrowser(url);
    safebox::daemon::showMessage(
        std::format("{}.\n\nИнтерфейс SafeBox открыт в браузере. Чтобы он работал в своем окне, "
                    "установите Microsoft Edge WebView2 Runtime: "
                    "https://developer.microsoft.com/microsoft-edge/webview2/\n\n"
                    "SafeBox продолжит работать как сервер: закройте окно консоли, которое "
                    "появится следом, чтобы остановить его и заблокировать сейф.",
                    reason),
        false);
    ensureConsole();
    logLine(reason);
}
#else
void showIfNoConsole(std::string_view /*message*/) {}
#endif

} // namespace

int main(int argc, char** argv) {
    namespace sb = safebox;

#if defined(_WIN32)
    setupOutput();
#endif
    const auto args = utf8Arguments(argc, argv);
    auto config = sb::daemon::parseConfig(args, sb::daemon::systemEnv);
    if (!config) {
        std::println(stderr, "safeboxd: {}\n\n{}", config.error().message, sb::daemon::usage());
        showIfNoConsole(config.error().message);
        return 2;
    }
    if (config->showHelp) {
        std::println("{}", sb::daemon::usage());
        return 0;
    }
    if (config->showVersion) {
        std::println("safeboxd {}", SAFEBOX_VERSION);
        return 0;
    }

#if defined(_WIN32)
    const bool window = !config->noWindow;
    // Одно окно на порт. С портом 0 (любой свободный) общего адреса нет - ограничивать нечего.
    std::optional<sb::daemon::SingleInstance> instance;
    if (window && config->port != 0) {
        instance.emplace(config->port);
        if (!instance->isFirst()) {
            if (!instance->activateExisting()) {
                // первый экземпляр без окна (запасной путь или --no-window): откроем интерфейс
                sb::daemon::openInBrowser(windowUrl(*config, config->port));
            }
            return 0;
        }
    }
    if (!window) {
        ensureConsole();
    }
#endif

    try {
        // адаптеры (infra)
        auto crypto = sb::infra::makeSodiumCrypto();
        auto store = sb::infra::makeSqliteVaultStore();
        auto thumbnailer = sb::infra::makeStbThumbnailer();
        auto zip = sb::infra::makeStreamZipWriter();
        auto clock = sb::infra::makeSystemClock();
        auto fetcher = sb::infra::makePageFetcher();
        auto settings =
            sb::infra::makeFileSettingsStore(sb::daemon::settingsFile(sb::daemon::systemEnv));

        // сценарии (application)
        sb::app::AppConfig appConfig;
        appConfig.idleTimeout = config->idleTimeout;
        appConfig.presenceTimeout = config->presenceTimeout;
        appConfig.defaultDirectory = config->defaultDirectory;
        auto services = sb::app::makeServices(
            {*crypto, *store, *thumbnailer, *zip, *clock, *fetcher, *settings}, appConfig);

        // транспорт (http)
        sb::daemon::WebAssets assets;
        sb::http::HttpConfig httpConfig;
        httpConfig.host = config->host;
        httpConfig.port = config->port;
        httpConfig.extraHosts = config->allowedHosts;
        httpConfig.extraOrigins = config->allowedOrigins;
        httpConfig.version = SAFEBOX_VERSION;
        if (!config->quiet) {
            httpConfig.log = logLine;
        }
        sb::http::HttpServer server(services, assets, httpConfig);
        auto port = server.bind();
        if (!port) {
            std::println(stderr, "safeboxd: {}", port.error().message);
#if defined(_WIN32)
            if (window) {
                sb::daemon::showMessage(
                    config->port != 0
                        ? std::format(
                              "Порт {} занят другой программой.\n\nЗакройте её или запустите "
                              "SafeBox с другим портом: safeboxd --port <номер>.",
                              config->port)
                        : port.error().message,
                    true);
            }
#endif
            return 1;
        }

        std::signal(SIGINT, onSignal);
        std::signal(SIGTERM, onSignal);
#if defined(_WIN32)
        SetConsoleCtrlHandler(onConsoleEvent, TRUE);
#endif

        const bool lan = !sb::daemon::isLoopbackHost(config->host);
        logLine(std::format("SafeBox {} - http://{}:{}/", SAFEBOX_VERSION,
                            lan ? config->host : std::string("127.0.0.1"), *port));
        logLine(assets.empty() ? "веб-интерфейс не встроен: только API (dev - через Vite-прокси)"
                               : "веб-интерфейс встроен");
        if (lan) {
            logLine("ВНИМАНИЕ: LAN-режим без HTTPS - только доверенная сеть");
        }
#if defined(_WIN32)
        logLine(window ? "Закрытие окна или Ctrl+C - остановить (сейф будет заблокирован)"
                       : "Ctrl+C - остановить (сейф будет заблокирован)");
#else
        logLine("Ctrl+C - остановить (сейф будет заблокирован)");
#endif

        std::jthread ticker([&](std::stop_token stop) {
            using namespace std::chrono_literals;
            while (!stop.stop_requested()) {
                for (int i = 0; i < 10 && !stop.stop_requested(); ++i) {
                    if (g_stopRequested.load()) {
                        server.stop(); // безопасно в любой момент, в том числе до run()
                    }
                    std::this_thread::sleep_for(100ms);
                }
                services.safe->tick(); // автоблокировка
            }
        });

        // сервер - в своем потоке: главный поток нужен окну (цикл сообщений)
        std::jthread serving([&] {
            try {
                server.run();
            } catch (const std::exception& e) {
                logLine(std::format("сервер остановлен из-за ошибки: {}", e.what()));
            }
            g_stopRequested.store(true); // окну тоже пора закрыться
        });

#if defined(_WIN32)
        if (window) {
            const std::string url = windowUrl(*config, *port);
            sb::daemon::AppWindowOptions options;
            options.url = url;
            options.port = *port;
            options.stopRequested = [] { return g_stopRequested.load(); };
            if (const auto problem = sb::daemon::runAppWindow(options)) {
                fallbackToBrowser(url, *problem);
            } else {
                g_stopRequested.store(true); // окно закрыто - та же остановка, что по Ctrl+C
            }
        }
#endif

        serving.join();
        ticker.request_stop();
        ticker.join();
        services.safe->lock(); // "остановка процесса -> lock"
        g_shutdownDone.store(true);
        logLine("сейф заблокирован, safeboxd остановлен");
        return 0;
    } catch (const std::exception& e) {
        std::println(stderr, "safeboxd: {}", e.what());
        showIfNoConsole(e.what());
        g_shutdownDone.store(true);
        return 1;
    }
}
