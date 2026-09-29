// лимит тела у httplib снят, т.к. импорт потоковый. размер JSON ограничивает guard
#include "safebox/http/server.hpp"

#include <chrono>
#include <format>
#include <limits>

#include "api.hpp"

namespace safebox::http {

struct HttpServer::Impl {
    Impl(app::Services services, AssetProvider& assets, HttpConfig config)
        : ctx(std::move(services), assets, std::move(config)) {}

    ApiContext ctx;
    httplib::Server server;
};

HttpServer::HttpServer(app::Services services, AssetProvider& assets, HttpConfig config)
    : impl_(std::make_unique<Impl>(std::move(services), assets, std::move(config))) {
    auto& server = impl_->server;
    server.set_payload_max_length((std::numeric_limits<std::size_t>::max)());
    server.set_tcp_nodelay(true);
    // httplib по умолчанию ставит SO_REUSEPORT/SO_REUSEADDR, а с ними второй процесс может
    // занять уже слушаемый порт и перехватывать запросы. На Windows без опций занятый порт
    // дает ошибку bind; в других ОС SO_REUSEADDR пускает только мимо TIME_WAIT.
    server.set_socket_options([](auto sock) {
#if defined(_WIN32)
        (void)sock;
#else
        httplib::set_socket_opt(sock, SOL_SOCKET, SO_REUSEADDR, 1);
#endif
    });
    // Медленный источник импорта (флешка, большой файл в браузере) - не обрыв.
    server.set_read_timeout(std::chrono::seconds(30));
    if (impl_->ctx.config.log) {
        server.set_logger(
            [ctx = &impl_->ctx](const httplib::Request& req, const httplib::Response& res) {
                ctx->log(std::format("{} {} -> {}", req.method, req.path, res.status));
            });
    }
    registerRoutes(server, impl_->ctx);
}

HttpServer::~HttpServer() {
    stop();
}

domain::Result<int> HttpServer::bind() {
    const auto& config = impl_->ctx.config;
    int port = config.port;
    if (port == 0) {
        port = impl_->server.bind_to_any_port(config.host);
    } else if (!impl_->server.bind_to_port(config.host, port)) {
        port = -1;
    }
    if (port <= 0) {
        return domain::fail(domain::Error::Code::IoError,
                            std::format("Не удалось занять {}:{} - порт занят или адрес недоступен",
                                        config.host, config.port));
    }
    impl_->ctx.setPort(port);
    return port;
}

void HttpServer::run() {
    impl_->server.listen_after_bind();
}

void HttpServer::stop() {
    impl_->server.stop();
}

int HttpServer::port() const noexcept {
    return impl_->ctx.port;
}

} // namespace safebox::http
