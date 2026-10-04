// VaultStore в памяти для тестов application. Ведет себя как sqlite-версия
// (откат, id без повторов, каскад, pending/gc). database(path) - доступ к "файлу" для тестов на подмену
#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#include "safebox/domain/ports/storage.hpp"

namespace safebox::test {

class InMemoryVaultStore final : public domain::VaultStore {
public:
    struct Blob {
        domain::BlobStatus status = domain::BlobStatus::Pending;
        std::uint64_t size = 0;
        std::uint32_t chunkCount = 0;
        std::vector<std::shared_ptr<const domain::Bytes>> chunks;
    };

    struct Db {
        domain::SafeMeta meta;
        std::map<domain::EntryId, domain::EntryRecord> entries;
        std::map<domain::BlobId, Blob> blobs;
        std::map<domain::CategoryId, domain::TagCategoryRecord> categories;
        std::map<domain::TagId, domain::TagRecord> tags;
        domain::EntryId nextEntry = 1;
        domain::BlobId nextBlob = 1;
        domain::CategoryId nextCategory = 1;
        domain::TagId nextTag = 1;
    };

    // помощники тестов
    void addForeignFile(const std::filesystem::path& path) {
        std::scoped_lock lock(mutex_);
        files_[key(path)] = nullptr; // "файл" есть, но это не сейф
    }

    std::shared_ptr<Db> database(const std::filesystem::path& path) {
        std::scoped_lock lock(mutex_);
        const auto it = files_.find(key(path));
        return it == files_.end() ? nullptr : it->second;
    }

    void copyFile(const std::filesystem::path& from, const std::filesystem::path& to) {
        std::scoped_lock lock(mutex_);
        const auto& source = files_.at(key(from));
        files_[key(to)] = source ? std::make_shared<Db>(*source) : nullptr;
    }

    [[nodiscard]] std::size_t pendingBlobs(const std::filesystem::path& path) {
        auto db = database(path);
        std::scoped_lock lock(mutex_);
        std::size_t n = 0;
        for (const auto& [id, blob] : db->blobs) {
            n += blob.status == domain::BlobStatus::Pending ? 1 : 0;
        }
        return n;
    }

    [[nodiscard]] std::size_t blobCount(const std::filesystem::path& path) {
        auto db = database(path);
        std::scoped_lock lock(mutex_);
        return db->blobs.size();
    }

    std::atomic<int> compactions{0};

    // VaultStore
    domain::Status create(const std::filesystem::path& path,
                          const domain::SafeMeta& meta) override {
        std::scoped_lock lock(mutex_);
        if (open_) {
            return domain::fail(domain::Error::Code::Internal, "store already open");
        }
        if (files_.contains(key(path))) {
            return domain::fail(domain::Error::Code::AlreadyExists, "Файл уже существует");
        }
        auto db = std::make_shared<Db>();
        db->meta = meta;
        files_[key(path)] = db;
        open_ = db;
        isOpen_ = true;
        return {};
    }

    domain::Status open(const std::filesystem::path& path) override {
        std::scoped_lock lock(mutex_);
        if (open_) {
            return domain::fail(domain::Error::Code::Internal, "store already open");
        }
        auto db = find(path);
        if (!db) {
            return std::unexpected(db.error());
        }
        open_ = *db;
        isOpen_ = true;
        return {};
    }

    domain::Result<domain::SafeMeta> inspect(const std::filesystem::path& path) override {
        std::scoped_lock lock(mutex_);
        auto db = find(path);
        if (!db) {
            return std::unexpected(db.error());
        }
        return (*db)->meta;
    }

    void close() noexcept override {
        std::scoped_lock lock(mutex_);
        open_.reset();
        isOpen_ = false;
    }

    bool isOpen() const override { return isOpen_.load(); }

    domain::Result<domain::SafeMeta> meta() override {
        auto lock = lockOpen();
        if (!lock) {
            return std::unexpected(lock.error());
        }
        return open_->meta;
    }

    domain::Result<std::unique_ptr<domain::UnitOfWork>> begin() override {
        auto lock = lockOpen();
        if (!lock) {
            return std::unexpected(lock.error());
        }
        owner_.store(std::this_thread::get_id());
        return std::unique_ptr<domain::UnitOfWork>(std::make_unique<Uow>(*this, std::move(*lock)));
    }

    domain::BlobStore& blobs() override { return blobs_; }

    domain::Status compact() override {
        auto lock = lockOpen();
        if (!lock) {
            return std::unexpected(lock.error());
        }
        ++compactions;
        return {};
    }

private:
    static std::string key(const std::filesystem::path& path) {
        return domain::pathToUtf8(path.lexically_normal());
    }

    domain::Result<std::shared_ptr<Db>> find(const std::filesystem::path& path) {
        const auto it = files_.find(key(path));
        if (it == files_.end()) {
            return domain::fail(domain::Error::Code::NotFound, "Файл сейфа не найден");
        }
        if (!it->second) {
            return domain::fail(domain::Error::Code::NotASafe, "Файл не является сейфом SafeBox");
        }
        return it->second;
    }

    domain::Result<std::unique_lock<std::mutex>> lockOpen() {
        if (owner_.load() == std::this_thread::get_id()) {
            return domain::fail(domain::Error::Code::Internal, "store call inside UnitOfWork");
        }
        std::unique_lock lock(mutex_);
        if (!open_) {
            return domain::fail(domain::Error::Code::Locked, "Сейф заблокирован");
        }
        return lock;
    }

    // UnitOfWork: работа над копией, commit подменяет "файл"
    class Uow final : public domain::UnitOfWork, domain::EntryRepository, domain::BlobRepository {
    public:
        Uow(InMemoryVaultStore& store, std::unique_lock<std::mutex> lock)
            : store_(store), lock_(std::move(lock)), work_(*store.open_), tags_(work_) {}
        ~Uow() override { store_.owner_.store(std::thread::id{}); }

        domain::EntryRepository& entries() override { return *this; }
        domain::BlobRepository& blobs() override { return *this; }
        domain::TagRepository& tags() override { return tags_; }

        domain::Status saveMeta(const domain::SafeMeta& meta) override {
            work_.meta = meta;
            return {};
        }

        domain::Status commit() override {
            *store_.open_ = std::move(work_);
            return {};
        }

        // EntryRepository
        domain::Result<domain::EntryId> insert(const domain::EntryRecord& record) override {
            if (record.parentId && !work_.entries.contains(*record.parentId)) {
                return domain::fail(domain::Error::Code::NotFound, "Объект назначения не найден");
            }
            auto copy = record;
            copy.id = work_.nextEntry++;
            work_.entries[copy.id] = copy;
            return copy.id;
        }

        domain::Status updateSealed(domain::EntryId id, std::span<const std::byte> encName,
                                    std::span<const std::byte> encMeta) override {
            const auto it = work_.entries.find(id);
            if (it == work_.entries.end()) {
                return domain::fail(domain::Error::Code::NotFound, "Объект не найден");
            }
            it->second.encName.assign(encName.begin(), encName.end());
            it->second.encMeta.assign(encMeta.begin(), encMeta.end());
            return {};
        }

        domain::Status update(const domain::EntryRecord& record) override {
            const auto it = work_.entries.find(record.id);
            if (it == work_.entries.end()) {
                return domain::fail(domain::Error::Code::NotFound, "Объект не найден");
            }
            if (record.parentId && !work_.entries.contains(*record.parentId)) {
                return domain::fail(domain::Error::Code::NotFound, "Объект назначения не найден");
            }
            it->second = record;
            return {};
        }

        domain::Result<domain::EntryRecord> get(domain::EntryId id) override {
            const auto it = work_.entries.find(id);
            if (it == work_.entries.end()) {
                return domain::fail(domain::Error::Code::NotFound, "Объект не найден");
            }
            return it->second;
        }

        domain::Result<std::vector<domain::EntryRecord>>
        children(std::optional<domain::EntryId> parent) override {
            std::vector<domain::EntryRecord> out;
            for (const auto& [id, record] : work_.entries) {
                if (record.parentId == parent) {
                    out.push_back(record);
                }
            }
            return out;
        }

        domain::Result<std::vector<domain::EntryRecord>> all() override {
            std::vector<domain::EntryRecord> out;
            for (const auto& [id, record] : work_.entries) {
                out.push_back(record);
            }
            return out;
        }

        domain::Result<std::uint64_t> count() override { return work_.entries.size(); }

        domain::Result<domain::RemovedEntries> removeSubtree(domain::EntryId id) override {
            if (!work_.entries.contains(id)) {
                return domain::fail(domain::Error::Code::NotFound, "Объект не найден");
            }
            domain::RemovedEntries removed;
            std::set<domain::EntryId> subtree{id};
            for (bool grew = true; grew;) {
                grew = false;
                for (const auto& [eid, record] : work_.entries) {
                    if (record.parentId && subtree.contains(*record.parentId) &&
                        subtree.insert(eid).second) {
                        grew = true;
                    }
                }
            }
            for (const auto eid : subtree) {
                const auto& record = work_.entries.at(eid);
                for (const auto& blob : {record.blobId, record.thumbBlobId}) {
                    if (blob) {
                        removed.blobs.push_back(*blob);
                    }
                }
                removed.entries.push_back(eid);
            }
            for (const auto eid : subtree) {
                work_.entries.erase(eid);
            }
            for (const auto blob : removed.blobs) {
                work_.blobs.erase(blob);
            }
            return removed;
        }

        // BlobRepository
        domain::Status promote(domain::BlobId id) override {
            const auto it = work_.blobs.find(id);
            if (it == work_.blobs.end() || it->second.status != domain::BlobStatus::Pending) {
                return domain::fail(domain::Error::Code::NotFound, "Данные файла не найдены");
            }
            it->second.status = domain::BlobStatus::Ready;
            return {};
        }

        // как внешний ключ entries.blob_id/thumb_blob_id в sqlite: занятый блоб не удалить
        domain::Status remove(domain::BlobId id) override {
            for (const auto& [eid, record] : work_.entries) {
                if (record.blobId == id || record.thumbBlobId == id) {
                    return domain::fail(domain::Error::Code::IntegrityError,
                                        "Нарушена целостность данных сейфа");
                }
            }
            work_.blobs.erase(id);
            return {};
        }

    private:
        // TagRepository: отдельный объект, у него и у UnitOfWork есть метод tags()
        class Tags final : public domain::TagRepository {
        public:
            explicit Tags(Db& work) noexcept : work_(work) {}

            domain::Result<domain::CategoryId> insertCategory() override {
                const auto id = work_.nextCategory++;
                work_.categories[id] = domain::TagCategoryRecord{id, {}};
                return id;
            }

            domain::Status updateCategory(domain::CategoryId id,
                                          std::span<const std::byte> encName,
                                          std::span<const std::byte> encNameEn) override {
                const auto it = work_.categories.find(id);
                if (it == work_.categories.end()) {
                    return domain::fail(domain::Error::Code::NotFound, "Категория не найдена");
                }
                it->second.encName.assign(encName.begin(), encName.end());
                it->second.encNameEn.assign(encNameEn.begin(), encNameEn.end());
                return {};
            }

            domain::Status removeCategory(domain::CategoryId id) override {
                if (work_.categories.erase(id) == 0) {
                    return domain::fail(domain::Error::Code::NotFound, "Категория не найдена");
                }
                std::erase_if(work_.tags,
                              [id](const auto& item) { return item.second.categoryId == id; });
                return {};
            }

            domain::Result<std::vector<domain::TagCategoryRecord>> categories() override {
                std::vector<domain::TagCategoryRecord> out;
                for (const auto& [id, record] : work_.categories) {
                    out.push_back(record);
                }
                return out;
            }

            domain::Result<domain::TagId> insertTag(domain::CategoryId category) override {
                if (!work_.categories.contains(category)) {
                    return domain::fail(domain::Error::Code::NotFound, "Категория не найдена");
                }
                const auto id = work_.nextTag++;
                work_.tags[id] = domain::TagRecord{id, category, {}};
                return id;
            }

            domain::Status updateTag(domain::TagId id, domain::CategoryId category,
                                     std::span<const std::byte> encName,
                                     std::span<const std::byte> encNameEn) override {
                if (!work_.categories.contains(category)) {
                    return domain::fail(domain::Error::Code::NotFound, "Категория не найдена");
                }
                const auto it = work_.tags.find(id);
                if (it == work_.tags.end()) {
                    return domain::fail(domain::Error::Code::NotFound, "Тег не найден");
                }
                it->second.categoryId = category;
                it->second.encName.assign(encName.begin(), encName.end());
                it->second.encNameEn.assign(encNameEn.begin(), encNameEn.end());
                return {};
            }

            domain::Status removeTag(domain::TagId id) override {
                if (work_.tags.erase(id) == 0) {
                    return domain::fail(domain::Error::Code::NotFound, "Тег не найден");
                }
                return {};
            }

            domain::Result<std::vector<domain::TagRecord>> tags() override {
                std::vector<domain::TagRecord> out;
                for (const auto& [id, record] : work_.tags) {
                    out.push_back(record);
                }
                return out;
            }

        private:
            Db& work_;
        };

        InMemoryVaultStore& store_;
        std::unique_lock<std::mutex> lock_;
        Db work_;
        Tags tags_;
    };

    // BlobStore: сразу в "файл", под мьютексом на каждую операцию
    class Writer final : public domain::BlobWriter {
    public:
        Writer(InMemoryVaultStore& store, domain::BlobId id) : store_(store), id_(id) {}

        domain::BlobId id() const override { return id_; }

        domain::Status append(std::span<const std::byte> sealedChunk) override {
            auto lock = store_.lockOpen();
            if (!lock) {
                return std::unexpected(lock.error());
            }
            const auto it = store_.open_->blobs.find(id_);
            if (it == store_.open_->blobs.end() ||
                it->second.status != domain::BlobStatus::Pending) {
                return domain::fail(domain::Error::Code::IntegrityError, "blob gone");
            }
            it->second.chunks.push_back(
                std::make_shared<const domain::Bytes>(sealedChunk.begin(), sealedChunk.end()));
            ++appended;
            return {};
        }

        domain::Status finish(std::uint64_t plainSize) override {
            auto lock = store_.lockOpen();
            if (!lock) {
                return std::unexpected(lock.error());
            }
            const auto it = store_.open_->blobs.find(id_);
            if (it == store_.open_->blobs.end() ||
                it->second.status != domain::BlobStatus::Pending) {
                return domain::fail(domain::Error::Code::NotFound, "blob gone");
            }
            it->second.size = plainSize;
            it->second.chunkCount = static_cast<std::uint32_t>(it->second.chunks.size());
            return {};
        }

        std::uint32_t appended = 0;

    private:
        InMemoryVaultStore& store_;
        domain::BlobId id_;
    };

    class Blobs final : public domain::BlobStore {
    public:
        explicit Blobs(InMemoryVaultStore& store) : store_(store) {}

        domain::Result<std::unique_ptr<domain::BlobWriter>> create() override {
            auto lock = store_.lockOpen();
            if (!lock) {
                return std::unexpected(lock.error());
            }
            const auto id = store_.open_->nextBlob++;
            store_.open_->blobs[id] = Blob{};
            return std::unique_ptr<domain::BlobWriter>(std::make_unique<Writer>(store_, id));
        }

        domain::Result<domain::BlobInfo> info(domain::BlobId id) override {
            auto lock = store_.lockOpen();
            if (!lock) {
                return std::unexpected(lock.error());
            }
            const auto it = store_.open_->blobs.find(id);
            if (it == store_.open_->blobs.end()) {
                return domain::fail(domain::Error::Code::NotFound, "Данные файла не найдены");
            }
            return domain::BlobInfo{id, it->second.status, it->second.size, it->second.chunkCount};
        }

        domain::Result<domain::Bytes> readChunk(domain::BlobId id, std::uint32_t index) override {
            auto lock = store_.lockOpen();
            if (!lock) {
                return std::unexpected(lock.error());
            }
            const auto it = store_.open_->blobs.find(id);
            if (it == store_.open_->blobs.end() || index >= it->second.chunks.size()) {
                return domain::fail(domain::Error::Code::NotFound, "Кусок данных не найден");
            }
            ++store_.chunkReads;
            return *it->second.chunks[index];
        }

        domain::Status discard(domain::BlobId id) override {
            auto lock = store_.lockOpen();
            if (!lock) {
                return std::unexpected(lock.error());
            }
            const auto it = store_.open_->blobs.find(id);
            if (it != store_.open_->blobs.end() &&
                it->second.status == domain::BlobStatus::Pending) {
                store_.open_->blobs.erase(it);
            }
            return {};
        }

        domain::Result<std::size_t> gcPending() override {
            auto lock = store_.lockOpen();
            if (!lock) {
                return std::unexpected(lock.error());
            }
            std::set<domain::BlobId> referenced;
            for (const auto& [id, record] : store_.open_->entries) {
                for (const auto& blob : {record.blobId, record.thumbBlobId}) {
                    if (blob) {
                        referenced.insert(*blob);
                    }
                }
            }
            std::size_t removed = 0;
            for (auto it = store_.open_->blobs.begin(); it != store_.open_->blobs.end();) {
                if (it->second.status == domain::BlobStatus::Pending &&
                    !referenced.contains(it->first)) {
                    it = store_.open_->blobs.erase(it);
                    ++removed;
                } else {
                    ++it;
                }
            }
            return removed;
        }

    private:
        InMemoryVaultStore& store_;
    };

public:
    std::atomic<std::uint64_t> chunkReads{
        0}; // сколько кусков прочитано (Range читает только нужные)

private:
    std::mutex mutex_;
    std::map<std::string, std::shared_ptr<Db>> files_;
    std::shared_ptr<Db> open_;
    std::atomic<bool> isOpen_{false};
    std::atomic<std::thread::id> owner_{};
    Blobs blobs_{*this};
};

} // namespace safebox::test
