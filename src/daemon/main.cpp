// safeboxd: создает адаптеры, сервисы и http-сервер.
// По Ctrl+C / SIGTERM / закрытию консоли останавливаем сервер и блокируем сейф
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <ctime>
#include <exception>
#include <format>
#include <mutex>
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

} // namespace

int main(int argc, char** argv) {
    namespace sb = safebox;

    const auto args = utf8Arguments(argc, argv);
    auto config = sb::daemon::parseConfig(args, sb::daemon::systemEnv);
    if (!config) {
        std::println(stderr, "safeboxd: {}\n\n{}", config.error().message, sb::daemon::usage());
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

    try {
        // адаптеры (infra)
        auto crypto = sb::infra::makeSodiumCrypto();
        auto store = sb::infra::makeSqliteVaultStore();
        auto thumbnailer = sb::infra::makeStbThumbnailer();
        auto zip = sb::infra::makeStreamZipWriter();
        auto clock = sb::infra::makeSystemClock();

        // сценарии (application)
        sb::app::AppConfig appConfig;
        appConfig.idleTimeout = config->idleTimeout;
        appConfig.presenceTimeout = config->presenceTimeout;
        appConfig.defaultDirectory = config->defaultDirectory;
        auto services =
            sb::app::makeServices({*crypto, *store, *thumbnailer, *zip, *clock}, appConfig);

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
        logLine("Ctrl+C - остановить (сейф будет заблокирован)");

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

        server.run();
        ticker.request_stop();
        ticker.join();
        services.safe->lock(); // "остановка процесса -> lock"
        g_shutdownDone.store(true);
        logLine("сейф заблокирован, safeboxd остановлен");
        return 0;
    } catch (const std::exception& e) {
        std::println(stderr, "safeboxd: {}", e.what());
        g_shutdownDone.store(true);
        return 1;
    }
}
