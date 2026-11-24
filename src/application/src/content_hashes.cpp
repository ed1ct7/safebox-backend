// Досчет sha256 содержимого у записей, сохраненных до того, как импорт стал его считать.
// Нужен поиску повторов: без хеша старая запись повтором не считается.
// Открытие сейфа ставит его сессию в очередь; один фоновый поток на сервис. Список записей без
// хеша берется из каталога один раз, дальше - пачками (256 записей / 64 МиБ): содержимое
// читается и хешируется под арендой, потом одна транзакция дописывает sha256 в мету. Аренда
// держится только на пачку и проверяет отмену между кусками, так что lock не ждет досчета.
// Мета меняется только полем sha256 (modifiedAt - нет: это не правка пользователя). Запись,
// чье содержимое заменили, пока шел счет (другой blobId), пропускается. Содержимое не
// прочиталось (повреждено) - запись пропускается до следующего открытия, счет в failed.
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <stop_token>
#include <thread>
#include <vector>

#include "safebox/domain/model/safe_format.hpp"
#include "service_impls.hpp"
#include "vault_session.hpp"

namespace safebox::app {
namespace {

using domain::BlobId;
using domain::EntryId;
using domain::Error;
using domain::fail;

constexpr std::size_t kBatchEntries = 256;
constexpr std::uint64_t kBatchBytes = 64ull << 20;

[[nodiscard]] bool fatal(const Error& error) {
    return error.code == Error::Code::Locked || error.code == Error::Code::Cancelled;
}

struct Todo {
    EntryId id = 0;
    BlobId blob = 0;
    std::uint64_t size = 0;
};

struct Hashed {
    EntryId id = 0;
    BlobId blob = 0;
    domain::Sha256 sha256{};
};

class ContentHashServiceImpl final : public ContentHashRunner {
public:
    explicit ContentHashServiceImpl(Ports ports) : ports_(ports) {
        worker_ = std::jthread([this](std::stop_token stop) { run(stop); });
    }

    ~ContentHashServiceImpl() override {
        worker_.request_stop();
        worker_.join();
    }

    void start(std::weak_ptr<VaultSession> session) override {
        {
            std::scoped_lock lock(mutex_);
            queue_.push_back(std::move(session));
        }
        wake_.notify_one();
    }

    HashProgress progress(const Lease& lease) const override {
        const auto* session = lease.session();
        if (session == nullptr) {
            return {};
        }
        return {session->hashesPending(), session->hashesFailed()};
    }

    void drain() override {
        std::unique_lock lock(mutex_);
        idle_.wait(lock, [this] { return queue_.empty() && !busy_; });
    }

private:
    void run(std::stop_token stop) {
        std::unique_lock lock(mutex_);
        for (;;) {
            if (!wake_.wait(lock, stop, [this] { return !queue_.empty(); }) ||
                stop.stop_requested()) {
                return;
            }
            auto session = std::move(queue_.front());
            queue_.pop_front();
            busy_ = true;
            lock.unlock();
            try {
                process(session, stop);
            } catch (...) {
                // досчет - удобство поиска повторов, не данные: поток живет дальше
            }
            lock.lock();
            busy_ = false;
            idle_.notify_all();
        }
    }

    void process(const std::weak_ptr<VaultSession>& weak, const std::stop_token& stop) {
        auto todo = collect(weak);
        std::size_t done = 0;
        std::size_t failed = 0;
        while (done < todo.size() && !stop.stop_requested()) {
            auto lease = leaseFrom(weak);
            if (!lease) {
                return; // сейф закрыт или закрывается
            }
            auto ctx = contextOf(*lease);
            if (!ctx) {
                return;
            }
            const auto take = batchSize(std::span(todo).subspan(done));
            std::vector<Hashed> hashed;
            hashed.reserve(take);
            for (std::size_t i = done; i < done + take; ++i) {
                auto sha = hashBlob(*lease, *ctx, todo[i].blob, stop);
                if (sha) {
                    hashed.push_back({todo[i].id, todo[i].blob, *sha});
                } else if (fatal(sha.error()) || stop.stop_requested()) {
                    return;
                } else {
                    ++failed;
                }
            }
            if (auto st = write(*ctx, hashed); !st) {
                if (fatal(st.error())) {
                    return;
                }
                failed += hashed.size();
            }
            done += take;
            ctx->session.setHashProgress(todo.size() - done, failed);
        }
    }

    // Записи с содержимым и без хеша, по id: так досчет идет от старых к новым.
    std::vector<Todo> collect(const std::weak_ptr<VaultSession>& weak) {
        std::vector<Todo> todo;
        auto lease = leaseFrom(weak);
        if (!lease) {
            return todo;
        }
        auto ctx = contextOf(*lease);
        if (!ctx) {
            return todo;
        }
        auto catalog = catalogOf(*ctx, ports_.store);
        if (!catalog) {
            return todo;
        }
        for (const auto& [id, node] : (*catalog)->nodes()) {
            const auto& meta = node->entry.meta;
            if (meta.blobId && !meta.sha256 && !node->entry.isFolder()) {
                todo.push_back({id, *meta.blobId, meta.size});
            }
        }
        std::sort(todo.begin(), todo.end(),
                  [](const Todo& a, const Todo& b) { return a.id < b.id; });
        ctx->session.setHashProgress(todo.size(), 0);
        return todo; // снимок каталога отпускается: правки не будут копировать его
    }

    static std::size_t batchSize(std::span<const Todo> rest) {
        std::size_t count = 0;
        std::uint64_t bytes = 0;
        while (count < rest.size() && count < kBatchEntries &&
               (count == 0 || bytes + rest[count].size <= kBatchBytes)) {
            bytes += rest[count].size;
            ++count;
        }
        return count;
    }

    Result<domain::Sha256> hashBlob(const Lease& lease, const OperationContext& ctx, BlobId id,
                                    const std::stop_token& stop) {
        auto& blobs = ports_.store.blobs();
        const auto chunkSize = ctx.session.chunkSize();
        auto info = blobs.info(id);
        if (!info) {
            return std::unexpected(info.error());
        }
        if (info->status != domain::BlobStatus::Ready ||
            info->chunkCount != domain::chunkCountFor(info->size, chunkSize)) {
            return fail(Error::Code::IntegrityError, "Данные файла повреждены");
        }
        auto hasher = ports_.crypto.sha256();
        for (std::uint32_t i = 0; i < info->chunkCount; ++i) {
            if (lease.cancelled() || stop.stop_requested()) {
                return fail(Error::Code::Cancelled, "Досчет прерван");
            }
            auto sealed = blobs.readChunk(id, i);
            if (!sealed) {
                return std::unexpected(sealed.error());
            }
            const bool last = i + 1 == info->chunkCount;
            auto plain = ctx.sealer->openChunk(domain::KeyPurpose::Content, id, i, last, *sealed);
            if (!plain) {
                return std::unexpected(plain.error());
            }
            const std::uint64_t expected =
                last ? info->size - std::uint64_t{i} * chunkSize : std::uint64_t{chunkSize};
            const bool sizeOk = plain->size() == expected;
            if (sizeOk) {
                hasher->update(*plain);
            }
            domain::secureWipe(*plain);
            if (!sizeOk) {
                return fail(Error::Code::IntegrityError, "Данные файла повреждены");
            }
        }
        return hasher->finish();
    }

    // Хеши пачки - одной транзакцией; запись удалили, заменили содержимое или хеш уже есть -
    // пропуск.
    Status write(const OperationContext& ctx, const std::vector<Hashed>& hashed) {
        if (hashed.empty()) {
            return {};
        }
        auto uow = ports_.store.begin();
        if (!uow) {
            return std::unexpected(uow.error());
        }
        std::vector<EntryId> changed;
        for (const auto& item : hashed) {
            auto record = (*uow)->entries().get(item.id);
            if (!record) {
                if (record.error().code == Error::Code::NotFound) {
                    continue;
                }
                return std::unexpected(record.error());
            }
            if (record->blobId != item.blob) {
                continue;
            }
            auto entry = ctx.sealer->openEntry(*record);
            if (!entry) {
                continue; // мета не читается: не наша забота, остальные пишем
            }
            domain::secureWipe(entry->name);
            if (entry->meta.sha256) {
                domain::secureWipe(entry->meta.description);
                continue;
            }
            entry->meta.sha256 = item.sha256;
            auto encMeta = ctx.sealer->sealMeta(record->id, entry->meta);
            domain::secureWipe(entry->meta.description);
            if (!encMeta) {
                return std::unexpected(encMeta.error());
            }
            record->encMeta = std::move(*encMeta);
            if (auto st = (*uow)->entries().update(*record); !st) {
                return st;
            }
            changed.push_back(item.id);
        }
        if (changed.empty()) {
            return {};
        }
        if (auto st = (*uow)->commit(); !st) {
            return st;
        }
        ctx.session.catalogChanged({.entries = std::move(changed)});
        return {};
    }

    Ports ports_;
    std::mutex mutex_; // queue_, busy_
    std::condition_variable_any wake_;
    std::condition_variable idle_;
    std::deque<std::weak_ptr<VaultSession>> queue_;
    bool busy_ = false;
    std::jthread worker_; // последним: останавливается раньше остальных полей
};

} // namespace

std::shared_ptr<ContentHashRunner> makeContentHashService(Ports ports) {
    return std::make_shared<ContentHashServiceImpl>(ports);
}

} // namespace safebox::app
