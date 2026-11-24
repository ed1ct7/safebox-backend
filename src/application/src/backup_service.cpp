// Копия открытого сейфа (VaultStore::backup) в фоне. Один поток на сервис, одна копия за раз.
// Каждый шаг (16 МиБ страниц) - под своей арендой: lock ждет не дольше шага, после него аренда
// не выдается и копия обрывается (незаконченный ".part" удаляет хранилище). Прогресс виден
// только той сессии, что начала копию.
#include <condition_variable>
#include <mutex>
#include <stop_token>
#include <thread>

#include "safebox/domain/model/rules.hpp"
#include "service_impls.hpp"
#include "vault_session.hpp"

namespace safebox::app {
namespace {

using domain::Error;
using domain::fail;

class BackupServiceImpl final : public BackupService {
public:
    BackupServiceImpl(Ports ports, AppConfig config) : ports_(ports), config_(std::move(config)) {
        worker_ = std::jthread([this](std::stop_token stop) { run(stop); });
    }

    ~BackupServiceImpl() override {
        worker_.request_stop();
        worker_.join();
    }

    Result<BackupProgress> start(const Lease& lease, std::string_view raw) override {
        auto ctx = contextOf(lease);
        if (!ctx) {
            return std::unexpected(ctx.error());
        }
        auto resolved = resolveSafePath(raw, config_.defaultDirectory);
        if (!resolved) {
            return std::unexpected(resolved.error());
        }
        auto path = domain::withSafeExtension(*resolved);
        std::unique_lock lock(mutex_);
        if (job_ || busy_) {
            return fail(Error::Code::AlreadyExists, "Копия сейфа уже делается");
        }
        auto copy = ports_.store.backup(path);
        if (!copy) {
            return std::unexpected(copy.error());
        }
        progress_ = {.state = BackupState::Running,
                     .path = std::move(path),
                     .bytesDone = 0,
                     .bytesTotal = (*copy)->bytesTotal()};
        owner_ = lease.weakSession();
        job_ = std::move(*copy);
        lock.unlock();
        wake_.notify_one();
        return progress(lease);
    }

    BackupProgress progress(const Lease& lease) const override {
        std::scoped_lock lock(mutex_);
        const auto owner = owner_.lock();
        if (owner == nullptr || owner.get() != lease.session()) {
            return {};
        }
        return progress_;
    }

    void drain() override {
        std::unique_lock lock(mutex_);
        idle_.wait(lock, [this] { return !job_ && !busy_; });
    }

private:
    void run(std::stop_token stop) {
        std::unique_lock lock(mutex_);
        for (;;) {
            if (!wake_.wait(lock, stop, [this] { return job_ != nullptr; })) {
                return;
            }
            auto copy = std::move(job_);
            const auto owner = owner_;
            busy_ = true;
            lock.unlock();
            Status result;
            try {
                result = copyAll(*copy, owner, stop);
            } catch (const std::exception&) {
                result = fail(Error::Code::Internal, "Копия сейфа прервалась");
            }
            copy.reset(); // не дошла до конца -> хранилище удаляет ".part"
            lock.lock();
            if (result) {
                progress_.state = BackupState::Done;
                progress_.bytesDone = progress_.bytesTotal;
            } else {
                progress_.state = BackupState::Failed;
                progress_.error = result.error().message;
            }
            busy_ = false;
            idle_.notify_all();
        }
    }

    Status copyAll(domain::VaultCopy& copy, const std::weak_ptr<VaultSession>& owner,
                   const std::stop_token& stop) {
        for (;;) {
            if (stop.stop_requested()) {
                return fail(Error::Code::Cancelled, "Копия прервана: сервер останавливается");
            }
            auto lease = leaseFrom(owner);
            if (!lease || lease->cancelled()) {
                return fail(Error::Code::Locked, "Копия прервана: сейф заблокирован");
            }
            auto done = copy.step();
            if (!done) {
                return std::unexpected(done.error());
            }
            {
                std::scoped_lock lock(mutex_);
                progress_.bytesDone = copy.bytesDone();
                progress_.bytesTotal = copy.bytesTotal();
            }
            if (*done) {
                return {};
            }
        }
    }

    Ports ports_;
    AppConfig config_;
    mutable std::mutex mutex_; // job_, busy_, owner_, progress_
    std::condition_variable_any wake_;
    std::condition_variable idle_;
    std::unique_ptr<domain::VaultCopy> job_; // начатая, еще не взятая потоком
    bool busy_ = false;
    std::weak_ptr<VaultSession> owner_;
    BackupProgress progress_;
    std::jthread worker_; // последним: останавливается раньше остальных полей
};

} // namespace

std::shared_ptr<BackupService> makeBackupService(Ports ports, AppConfig config) {
    return std::make_shared<BackupServiceImpl>(ports, std::move(config));
}

} // namespace safebox::app
