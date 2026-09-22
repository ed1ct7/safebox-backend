#include <atomic>
#include <thread>

#include "app_fixture.hpp"

using namespace safebox;
using namespace safebox::test;
using Code = domain::Error::Code;

TEST_CASE("create opens a new empty safe and issues working tokens", "[safe][UF-1]") {
    AppFixture f;
    auto session = f.createSafe("мой сейф");
    CHECK(session.safe.entryCount == 0);
    CHECK(session.safe.path == domain::pathToUtf8(f.safePath("мой сейф")));
    CHECK(session.tokens.api != session.tokens.media);
    CHECK(f.store.isOpen());

    auto lease = f.services.safe->authorize(session.tokens.api, app::TokenScope::Api);
    REQUIRE(lease.has_value());
    auto info = f.services.safe->info(*lease);
    REQUIRE(info.has_value());
    CHECK(info->idleRemainingSec == 15 * 60);

    const auto status = f.services.safe->publicStatus();
    CHECK(status.unlocked);
    CHECK(status.lastPath == session.safe.path);
}

TEST_CASE("create validates the form (UF-1 errors)", "[safe][UF-1]") {
    AppFixture f;
    auto shortPassword = f.services.safe->create({"a", "12345", "12345"});
    REQUIRE_FALSE(shortPassword.has_value());
    CHECK(shortPassword.error().code == Code::InvalidArgument);

    auto mismatch = f.services.safe->create({"a", "123456", "1234567"});
    REQUIRE_FALSE(mismatch.has_value());
    CHECK(mismatch.error().code == Code::InvalidArgument);

    auto noPath = f.services.safe->create({"   ", "123456", "123456"});
    REQUIRE_FALSE(noPath.has_value());
    CHECK(noPath.error().code == Code::InvalidArgument);

    // 6 символов кириллицей = 12 байт UTF-8: считаются символы, а не байты
    CHECK(f.services.safe->create({"cyr", "пароль", "пароль"}).has_value());
    f.services.safe->lock();

    auto exists = f.services.safe->create({"cyr", "другой1", "другой1"});
    REQUIRE_FALSE(exists.has_value());
    CHECK(exists.error().code == Code::AlreadyExists);
}

TEST_CASE("unlock checks the password and the file", "[safe][UF-2]") {
    AppFixture f;
    f.createSafe("vault", "correct-horse");
    f.services.safe->lock();
    CHECK_FALSE(f.store.isOpen());

    auto wrong = f.services.safe->unlock({"vault", "wrong-horse"});
    REQUIRE_FALSE(wrong.has_value());
    CHECK(wrong.error().code == Code::WrongPassword);
    CHECK_FALSE(f.store.isOpen());

    auto missing = f.services.safe->unlock({"nope", "correct-horse"});
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code == Code::NotFound);

    f.store.addForeignFile(f.safePath("photo"));
    auto foreign = f.services.safe->unlock({"photo.safebox", "correct-horse"});
    REQUIRE_FALSE(foreign.has_value());
    CHECK(foreign.error().code == Code::NotASafe);

    auto ok = f.services.safe->unlock({"vault", "correct-horse"}); // без .safebox
    REQUIRE(ok.has_value());
    CHECK(f.services.safe->authorize(ok->tokens.api, app::TokenScope::Api).has_value());
}

TEST_CASE("unlock of the same safe from another tab keeps the session", "[safe][UF-2]") {
    AppFixture f;
    auto first = f.createSafe("vault");
    auto second = f.services.safe->unlock({"vault.safebox", "secret1"});
    REQUIRE(second.has_value());
    CHECK(f.services.safe->authorize(first.tokens.api, app::TokenScope::Api).has_value());
    CHECK(f.services.safe->authorize(second->tokens.api, app::TokenScope::Api).has_value());

    auto wrong = f.services.safe->unlock({"vault", "bad-password"});
    REQUIRE_FALSE(wrong.has_value());
    CHECK(wrong.error().code == Code::WrongPassword);
    CHECK(f.services.safe->authorize(first.tokens.api, app::TokenScope::Api).has_value());
}

TEST_CASE("a failed attempt to open another safe does not lock the open one", "[safe][UF-2]") {
    AppFixture f;
    f.createSafe("other", "other-pass");
    f.services.safe->lock();
    auto current = f.createSafe("current", "current-pass");

    auto wrong = f.services.safe->unlock({"other", "typo-pass"});
    REQUIRE_FALSE(wrong.has_value());
    CHECK(wrong.error().code == Code::WrongPassword);
    auto missing = f.services.safe->unlock({"no-such-safe", "x-pass-1"});
    REQUIRE_FALSE(missing.has_value());
    CHECK(f.services.safe->authorize(current.tokens.api, app::TokenScope::Api).has_value());

    auto switched = f.services.safe->unlock({"other", "other-pass"});
    REQUIRE(switched.has_value());
    CHECK_FALSE(f.services.safe->authorize(current.tokens.api, app::TokenScope::Api).has_value());
    CHECK(f.services.safe->authorize(switched->tokens.api, app::TokenScope::Api).has_value());
}

TEST_CASE("tokens are scoped: media cookie is not an API token", "[safe][auth]") {
    AppFixture f;
    auto session = f.createSafe();
    CHECK(f.services.safe->authorize(session.tokens.media, app::TokenScope::Media).has_value());
    CHECK_FALSE(f.services.safe->authorize(session.tokens.media, app::TokenScope::Api).has_value());
    CHECK_FALSE(f.services.safe->authorize(session.tokens.api, app::TokenScope::Media).has_value());
    CHECK_FALSE(f.services.safe->authorize("", app::TokenScope::Api).has_value());
    CHECK_FALSE(f.services.safe->authorize("forged", app::TokenScope::Api).has_value());
}

TEST_CASE("lock kills every token and closes the store", "[safe][UF-13]") {
    AppFixture f;
    auto first = f.createSafe();
    auto second = f.services.safe->unlock({"test", "secret1"});
    REQUIRE(second.has_value());

    f.services.safe->lock();
    CHECK_FALSE(f.store.isOpen());
    CHECK_FALSE(f.services.safe->publicStatus().unlocked);
    for (const auto* token : {&first.tokens.api, &second->tokens.api}) {
        auto denied = f.services.safe->authorize(*token, app::TokenScope::Api);
        REQUIRE_FALSE(denied.has_value());
        CHECK(denied.error().code == Code::Locked);
    }
    CHECK_FALSE(f.services.safe->authorize(first.tokens.media, app::TokenScope::Media).has_value());

    auto again = f.services.safe->unlock({"test", "secret1"});
    REQUIRE(again.has_value());
    CHECK(f.services.safe->authorize(again->tokens.api, app::TokenScope::Api).has_value());
}

TEST_CASE("lock waits for active leases and cancels them", "[safe][UF-13][concurrency]") {
    AppFixture f;
    auto session = f.createSafe();
    auto lease = f.lease(session);
    CHECK_FALSE(lease.cancelled());

    std::atomic<bool> locked{false};
    std::thread locker([&] {
        f.services.safe->lock();
        locked = true;
    });
    // lock поднял флаг отмены и ждет аренду
    for (int i = 0; i < 200 && !lease.cancelled(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(lease.cancelled());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK_FALSE(locked.load());
    CHECK(f.store.isOpen()); // файл не закрыт, пока операция идет

    {
        auto done = std::move(lease);
    } // операция закончилась
    locker.join();
    CHECK(locked.load());
    CHECK_FALSE(f.store.isOpen());
}

TEST_CASE("changePassword re-seals only the envelope", "[safe][UF-12]") {
    AppFixture f;
    auto session = f.createSafe("vault", "old-password");
    auto imported = f.importFiles(session, {{"doc.txt", "содержимое"}});
    REQUIRE(imported.imported == 1);
    const auto before = f.store.database(f.safePath("vault"))->meta;

    auto wrong = f.services.safe->changePassword(f.lease(session),
                                                 {"not-it", "new-password", "new-password"});
    REQUIRE_FALSE(wrong.has_value());
    CHECK(wrong.error().code == Code::WrongPassword);
    const auto untouched = f.store.database(f.safePath("vault"))->meta;
    CHECK(untouched.envelope == before.envelope);
    CHECK(untouched.salt == before.salt);

    auto mismatch = f.services.safe->changePassword(f.lease(session),
                                                    {"old-password", "new-password", "other"});
    REQUIRE_FALSE(mismatch.has_value());
    CHECK(mismatch.error().code == Code::InvalidArgument);

    REQUIRE(f.services.safe
                ->changePassword(f.lease(session), {"old-password", "new-password", "new-password"})
                .has_value());
    const auto after = f.store.database(f.safePath("vault"))->meta;
    CHECK(after.envelope != before.envelope);
    CHECK(after.salt != before.salt);
    // текущая сессия продолжает жить
    CHECK(f.services.safe->authorize(session.tokens.api, app::TokenScope::Api).has_value());

    f.services.safe->lock();
    auto old = f.services.safe->unlock({"vault", "old-password"});
    REQUIRE_FALSE(old.has_value());
    CHECK(old.error().code == Code::WrongPassword);
    auto fresh = f.services.safe->unlock({"vault", "new-password"});
    REQUIRE(fresh.has_value());
    const auto doc = f.entryNamed(*fresh, "doc.txt");
    CHECK(f.readContent(*fresh, doc.id) == "содержимое");
}

TEST_CASE("a copied safe file opens with the same password", "[safe][transfer]") {
    AppFixture f;
    auto session = f.createSafe("vault");
    REQUIRE(f.importFiles(session, {{"a.txt", "hello"}}).imported == 1);
    f.services.safe->lock();

    f.store.copyFile(f.safePath("vault"), f.safePath("usb-copy"));
    auto copy = f.services.safe->unlock({"usb-copy", "secret1"});
    REQUIRE(copy.has_value());
    CHECK(f.readContent(*copy, f.entryNamed(*copy, "a.txt").id) == "hello");
}
