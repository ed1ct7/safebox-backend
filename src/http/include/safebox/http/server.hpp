// HttpServer: bind() -> run() (блокирует) -> stop().
// Сейф при остановке блокирует main, не сервер
#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "safebox/app/services.hpp"
#include "safebox/domain/model/error.hpp"

namespace safebox::http {

struct Asset {
    std::string_view contentType;
    std::string_view body;
};

class AssetProvider {
public:
    virtual ~AssetProvider() = default;

    // path - путь запроса без query ("/", "/assets/app.js"); nullopt - нет такого.
    [[nodiscard]] virtual std::optional<Asset> find(std::string_view path) const = 0;
    [[nodiscard]] virtual bool empty() const = 0;
};

struct HttpConfig {
    std::string host = "127.0.0.1";
    int port = 8900; // 0 - свободный порт (тесты)
    // LAN-режим: дополнительные значения заголовка Host ("192.168.1.5" или
    // "mypc.local:8900"; без порта - подставится порт сервера).
    std::vector<std::string> extraHosts;
    // Дополнительные Origin для изменяющих запросов (dev: "http://localhost:5173").
    std::vector<std::string> extraOrigins;
    std::size_t maxJsonBody = 256 * 1024;      // описание записи до 64 КиБ + запас на экранирование
    std::string version = "dev";               // отдается в GET /health
    std::function<void(std::string_view)> log; // журнал запросов; пусто - молчать
};

class HttpServer {
public:
    HttpServer(app::Services services, AssetProvider& assets, HttpConfig config);
    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;
    ~HttpServer();

    [[nodiscard]] domain::Result<int> bind();
    void run();
    void stop();
    [[nodiscard]] int port() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace safebox::http
