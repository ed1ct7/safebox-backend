// Часы, которые двигаются руками (advance)
#pragma once

#include <chrono>
#include <mutex>

#include "safebox/domain/ports/clock.hpp"

namespace safebox::test {

class FakeClock final : public domain::Clock {
public:
    WallTime now() const override {
        std::scoped_lock lock(mutex_);
        return wall_;
    }

    MonoTime monotonic() const override {
        std::scoped_lock lock(mutex_);
        return mono_;
    }

    void advance(std::chrono::nanoseconds step) {
        std::scoped_lock lock(mutex_);
        wall_ += std::chrono::duration_cast<std::chrono::system_clock::duration>(step);
        mono_ += step;
    }

private:
    mutable std::mutex mutex_;
    WallTime wall_{std::chrono::seconds(1'790'000'000)}; // сентябрь 2026
    MonoTime mono_{std::chrono::hours(1)};
};

} // namespace safebox::test
