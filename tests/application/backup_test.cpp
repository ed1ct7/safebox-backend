// BackupService: копия открытого сейфа в фоне, прогресс, отказы, обрыв блокировкой.
#include <catch2/catch_test_macros.hpp>

#include <condition_variable>
#include <mutex>
#include <thread>

#include "app_fixture.hpp"

using namespace safebox;
using namespace safebox::test;
using Code = domain::Error::Code;

namespace {

// Шаг копии ждет, пока тест не откроет ворота; arrived - шаг уже внутри (аренда взята).
struct Gate {
    std::mutex mutex;
    std::condition_variable cv;
    bool open = false;
    int arrived = 0;

    void pass() {
        std::unique_lock lock(mutex);
        ++arrived;
        cv.notify_all();
        cv.wait(lock, [this] { return open; });
    }
    void waitArrived(int n) {
        std::unique_lock lock(mutex);
        cv.wait(lock, [&] { return arrived >= n; });
    }
    void release() {
        std::scoped_lock lock(mutex);
        open = true;
        cv.notify_all();
    }
};

} // namespace

TEST_CASE("backup copies the open safe and reports progress", "[backup]") {
    AppFixture f;
    auto s = f.createSafe();
    REQUIRE(
        f.importFiles(s, {{"Отпуск/море.jpg", fakeJpeg(3000)}, {"notes.txt", "hello"}}).imported ==
        2);
    CHECK(f.services.backup->progress(f.lease(s)).state == app::BackupState::None);

    auto started = f.services.backup->start(f.lease(s), "copy"); // от папки по умолчанию
    REQUIRE(started.has_value());
    CHECK(started->path == f.safePath("copy"));
    CHECK(started->bytesTotal == 4000);
    f.services.backup->drain();

    const auto done = f.services.backup->progress(f.lease(s));
    CHECK(done.state == app::BackupState::Done);
    CHECK(done.bytesDone == done.bytesTotal);
    CHECK(done.error.empty());
    REQUIRE(f.store.database(f.safePath("copy")) != nullptr);
    CHECK(f.store.database(f.safePath("copy"))->entries.size() ==
          f.store.database(f.safePath())->entries.size());
    CHECK(f.store.abandonedCopies == 0);

    // копия открывается тем же паролем, в ней то же самое
    f.services.safe->lock();
    auto copy = f.services.safe->unlock({f.safePath("copy").string(), "secret1"});
    REQUIRE(copy.has_value());
    CHECK(f.readContent(*copy, f.entryNamed(*copy, "notes.txt").id) == "hello");
    // прогресс - только у сессии, начавшей копию
    CHECK(f.services.backup->progress(f.lease(*copy)).state == app::BackupState::None);
}

TEST_CASE("backup refuses bad targets and a second copy", "[backup]") {
    AppFixture f;
    auto s = f.createSafe();

    auto existing = f.services.backup->start(f.lease(s), "test"); // это сам сейф
    REQUIRE_FALSE(existing.has_value());
    CHECK(existing.error().code == Code::AlreadyExists);
    auto blank = f.services.backup->start(f.lease(s), "   ");
    REQUIRE_FALSE(blank.has_value());
    CHECK(blank.error().code == Code::InvalidArgument);

    f.store.copyError = domain::Error{Code::IoError, "Нет места"};
    auto refused = f.services.backup->start(f.lease(s), "full");
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == Code::IoError);
    f.store.copyError.reset();
    CHECK(f.services.backup->progress(f.lease(s)).state == app::BackupState::None);

    Gate gate;
    f.store.onCopyStep = [&gate] { gate.pass(); };
    REQUIRE(f.services.backup->start(f.lease(s), "one").has_value());
    gate.waitArrived(1);
    auto second = f.services.backup->start(f.lease(s), "two");
    REQUIRE_FALSE(second.has_value());
    CHECK(second.error().code == Code::AlreadyExists);
    CHECK(f.services.backup->progress(f.lease(s)).state == app::BackupState::Running);
    // пока копия идет, запросы работают
    CHECK(f.services.entries->list(f.lease(s), std::nullopt).has_value());
    gate.release();
    f.services.backup->drain();
    CHECK(f.services.backup->progress(f.lease(s)).state == app::BackupState::Done);
    CHECK(f.store.database(f.safePath("two")) == nullptr);

    // после конца - можно снова, но не в занятое имя
    auto again = f.services.backup->start(f.lease(s), "one.safebox");
    REQUIRE_FALSE(again.has_value());
    CHECK(again.error().code == Code::AlreadyExists);
    REQUIRE(f.services.backup->start(f.lease(s), "two").has_value());
    f.services.backup->drain();
    CHECK(f.store.database(f.safePath("two")) != nullptr);
}

TEST_CASE("lock stops a running backup and drops the unfinished copy", "[backup]") {
    AppFixture f;
    auto s = f.createSafe();
    Gate gate;
    f.store.copySteps = 10;
    f.store.onCopyStep = [&gate] { gate.pass(); };
    REQUIRE(f.services.backup->start(f.lease(s), "copy").has_value());
    gate.waitArrived(1); // шаг держит аренду

    std::jthread locker([&f] { f.services.safe->lock(); });
    // lock ждет аренду шага; следующий шаг аренду уже не получит
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    gate.release();
    locker.join();
    f.services.backup->drain();

    CHECK_FALSE(f.store.isOpen());
    CHECK(f.store.database(f.safePath("copy")) == nullptr);
    CHECK(f.store.abandonedCopies == 1);

    // новая сессия копию не видит и может начать свою
    f.store.onCopyStep = nullptr;
    auto again = f.services.safe->unlock({f.safePath().string(), "secret1"});
    REQUIRE(again.has_value());
    CHECK(f.services.backup->progress(f.lease(*again)).state == app::BackupState::None);
    REQUIRE(f.services.backup->start(f.lease(*again), "copy").has_value());
    f.services.backup->drain();
    CHECK(f.services.backup->progress(f.lease(*again)).state == app::BackupState::Done);
}

TEST_CASE("a failed step marks the backup failed", "[backup]") {
    AppFixture f;
    auto s = f.createSafe();
    int steps = 0;
    f.store.onCopyStep = [&f, &steps] {
        if (++steps == 2) {
            f.store.close(); // как будто хранилище закрылось под копией
        }
    };
    REQUIRE(f.services.backup->start(f.lease(s), "copy").has_value());
    f.services.backup->drain();
    const auto failed = f.services.backup->progress(f.lease(s));
    CHECK(failed.state == app::BackupState::Failed);
    CHECK_FALSE(failed.error.empty());
    CHECK(f.store.abandonedCopies == 1);
}
