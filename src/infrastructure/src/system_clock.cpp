#include "safebox/domain/ports/clock.hpp"
#include "safebox/infra/factories.hpp"

namespace safebox::infra {
namespace {

class SystemClock final : public domain::Clock {
public:
    WallTime now() const override { return std::chrono::system_clock::now(); }
    MonoTime monotonic() const override { return std::chrono::steady_clock::now(); }
};

} // namespace

std::unique_ptr<domain::Clock> makeSystemClock() {
    return std::make_unique<SystemClock>();
}

} // namespace safebox::infra
