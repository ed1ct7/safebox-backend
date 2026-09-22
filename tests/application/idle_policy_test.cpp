#include "app_fixture.hpp"
#include "idle_policy.hpp"

using namespace safebox;
using namespace safebox::test;
using namespace std::chrono_literals;

TEST_CASE("IdlePolicy: activity and presence windows", "[idle][UF-13]") {
    FakeClock clock;
    app::IdlePolicy policy(15min, 2min, clock.monotonic());
    CHECK_FALSE(policy.expired(clock.monotonic()));
    CHECK(policy.idleRemaining(clock.monotonic()) == 15min);

    SECTION("heartbeats without activity lock after 15 minutes") {
        for (int i = 0; i < 44; ++i) { // 44 x 20 с = 14:40
            clock.advance(20s);
            policy.heartbeat(clock.monotonic(), false);
            REQUIRE_FALSE(policy.expired(clock.monotonic()));
        }
        CHECK(policy.idleRemaining(clock.monotonic()) == 20s);
        clock.advance(20s);
        policy.heartbeat(clock.monotonic(), false);
        CHECK(policy.expired(clock.monotonic()));
        CHECK(policy.idleRemaining(clock.monotonic()) == 0s);
    }

    SECTION("activity resets the 15-minute window") {
        clock.advance(14min);
        policy.heartbeat(clock.monotonic(), true);
        clock.advance(14min);
        policy.heartbeat(clock.monotonic(), false);
        CHECK_FALSE(policy.expired(clock.monotonic()));
        CHECK(policy.idleRemaining(clock.monotonic()) == 1min);
    }

    SECTION("no heartbeat for 2 minutes = window closed") {
        clock.advance(119s);
        CHECK_FALSE(policy.expired(clock.monotonic()));
        clock.advance(1s);
        CHECK(policy.expired(clock.monotonic()));
    }

    SECTION("page reload fits into the presence window") {
        clock.advance(20s);
        policy.heartbeat(clock.monotonic(), true);
        clock.advance(90s); // вкладка перезагружается
        policy.heartbeat(clock.monotonic(), true);
        clock.advance(20s);
        CHECK_FALSE(policy.expired(clock.monotonic()));
    }

    SECTION("remaining time is rounded up for the warning") {
        clock.advance(14min + 500ms);
        policy.heartbeat(clock.monotonic(), false);
        CHECK(policy.idleRemaining(clock.monotonic()) == 60s);
    }
}

TEST_CASE("SafeService auto-locks on tick after 15 idle minutes", "[idle][UF-13]") {
    AppFixture f;
    auto session = f.createSafe();
    for (int i = 0; i < 44; ++i) { // до 14:40 - окно присутствия держат heartbeat'ы
        f.clock.advance(20s);
        auto beat = f.services.safe->heartbeat(f.lease(session), false);
        REQUIRE(beat.has_value());
        CHECK(beat->idleRemainingSec == 15 * 60 - (i + 1) * 20);
        f.services.safe->tick();
        REQUIRE(f.store.isOpen());
    }
    // 15:00 без активности: tick заблокировал
    f.clock.advance(20s);
    f.services.safe->tick();
    CHECK_FALSE(f.store.isOpen());
    auto denied = f.services.safe->authorize(session.tokens.api, app::TokenScope::Api);
    REQUIRE_FALSE(denied.has_value());
    CHECK(denied.error().code == domain::Error::Code::Locked);
}

TEST_CASE("SafeService locks when the window is closed (no heartbeat)", "[idle][UF-13]") {
    AppFixture f;
    auto session = f.createSafe();
    f.clock.advance(100s);
    f.services.safe->tick();
    CHECK(f.store.isOpen());
    f.clock.advance(20s);
    f.services.safe->tick();
    CHECK_FALSE(f.store.isOpen());
}

TEST_CASE("an expired session is rejected on the next request even before tick", "[idle][UF-13]") {
    AppFixture f;
    auto session = f.createSafe();
    f.clock.advance(3min);
    auto denied = f.services.safe->authorize(session.tokens.api, app::TokenScope::Api);
    REQUIRE_FALSE(denied.has_value());
    CHECK(denied.error().code == domain::Error::Code::Locked);
    CHECK_FALSE(f.store.isOpen());
}

TEST_CASE("active heartbeats keep the safe open indefinitely", "[idle][UF-13]") {
    AppFixture f;
    auto session = f.createSafe();
    for (int i = 0; i < 180; ++i) { // час
        f.clock.advance(20s);
        auto beat = f.services.safe->heartbeat(f.lease(session), true);
        REQUIRE(beat.has_value());
        CHECK(beat->idleRemainingSec == 15 * 60);
        f.services.safe->tick();
    }
    CHECK(f.store.isOpen());
}
