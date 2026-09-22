// Чистая логика без потоков, тестится на FakeClock. Синхронизация на стороне VaultSession
#pragma once

#include <chrono>

#include "safebox/domain/ports/clock.hpp"

namespace safebox::app {

class IdlePolicy {
public:
    using Time = domain::Clock::MonoTime;

    IdlePolicy(std::chrono::seconds idleTimeout, std::chrono::seconds presenceTimeout,
               Time now) noexcept;

    // Сигнал присутствия; active - с прошлого раза была активность пользователя.
    void heartbeat(Time now, bool active) noexcept;
    // Вход/повторный вход тоже активность: пользователь только что ввел пароль.
    void touch(Time now) noexcept;

    [[nodiscard]] bool expired(Time now) const noexcept;
    [[nodiscard]] std::chrono::seconds idleRemaining(Time now) const noexcept;

private:
    std::chrono::seconds idleTimeout_;
    std::chrono::seconds presenceTimeout_;
    Time lastActivity_;
    Time lastPresence_;
};

} // namespace safebox::app
