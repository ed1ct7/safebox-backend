// Автоблокировка. Фронт шлет heartbeat каждые 20с с флагом active.
// 15 мин без активности или 2 мин вообще без heartbeat (закрыли вкладку) -> lock.
// Время монотонное, чтобы перевод часов ничего не ломал
#include "idle_policy.hpp"

#include <algorithm>

namespace safebox::app {

IdlePolicy::IdlePolicy(std::chrono::seconds idleTimeout, std::chrono::seconds presenceTimeout,
                       Time now) noexcept
    : idleTimeout_(idleTimeout), presenceTimeout_(presenceTimeout), lastActivity_(now),
      lastPresence_(now) {}

void IdlePolicy::heartbeat(Time now, bool active) noexcept {
    lastPresence_ = std::max(lastPresence_, now);
    if (active) {
        lastActivity_ = std::max(lastActivity_, now);
    }
}

void IdlePolicy::touch(Time now) noexcept {
    heartbeat(now, true);
}

bool IdlePolicy::expired(Time now) const noexcept {
    return now - lastActivity_ >= idleTimeout_ || now - lastPresence_ >= presenceTimeout_;
}

std::chrono::seconds IdlePolicy::idleRemaining(Time now) const noexcept {
    const auto left = idleTimeout_ - (now - lastActivity_);
    if (left <= Time::duration::zero()) {
        return std::chrono::seconds{0};
    }
    return std::chrono::ceil<std::chrono::seconds>(left);
}

} // namespace safebox::app
