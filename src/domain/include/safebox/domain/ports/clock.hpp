// now() - для дат записей, monotonic() - для таймеров блокировки
#pragma once

#include <chrono>
#include <cstdint>

namespace safebox::domain {

class Clock {
public:
    using WallTime = std::chrono::system_clock::time_point;
    using MonoTime = std::chrono::steady_clock::time_point;

    virtual ~Clock() = default;

    [[nodiscard]] virtual WallTime now() const = 0;
    [[nodiscard]] virtual MonoTime monotonic() const = 0;
};

[[nodiscard]] inline std::int64_t toUnixMillis(Clock::WallTime t) noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch()).count();
}

} // namespace safebox::domain
