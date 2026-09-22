// Токены и аренды (Lease) сессии.
// Lease держится на время одного запроса, в т.ч. стрима и импорта. lock() сначала отменяет
// операции, потом ждет пока все аренды отпустят и только потом стирает ключи.
// Поэтому lock() нельзя вызывать держа Lease - повиснет
#pragma once

#include <memory>
#include <string>

namespace safebox::app {

class VaultSession; // внутренность application (src/vault_session.hpp)

enum class TokenScope {
    Api,   // Authorization: Bearer - весь API
    Media, // HttpOnly-cookie - только GET-медиа
};

struct SessionTokens {
    std::string api;
    std::string media;
};

class Lease {
public:
    // Создает только SafeService::authorize (уже учтя аренду в сессии);
    // тесты транспорта передают nullptr - "пустая" аренда без сессии.
    explicit Lease(std::shared_ptr<VaultSession> session) noexcept;
    Lease(Lease&& other) noexcept;
    Lease& operator=(Lease&& other) noexcept;
    Lease(const Lease&) = delete;
    Lease& operator=(const Lease&) = delete;
    ~Lease();

    [[nodiscard]] bool cancelled() const noexcept;
    [[nodiscard]] VaultSession* session() const noexcept { return session_.get(); }

private:
    void release() noexcept;

    std::shared_ptr<VaultSession> session_;
};

} // namespace safebox::app
