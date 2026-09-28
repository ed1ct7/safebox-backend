// общее для sqlite_vault_store.cpp и sqlite_blob_store.cpp
#pragma once

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>

#include "safebox/domain/ports/storage.hpp"
#include "sqlite_database.hpp"

namespace safebox::infra::sqlite {

struct Connection {
    std::mutex mutex;
    std::unique_ptr<Database> db; // nullptr - сейф закрыт
    std::filesystem::path path;
    std::atomic<bool> open{false};
    std::atomic<std::thread::id> uowOwner{};
};

[[nodiscard]] domain::Result<std::unique_lock<std::mutex>> lockOpen(Connection& conn);

class SqliteBlobStore final : public domain::BlobStore {
public:
    explicit SqliteBlobStore(Connection& conn) noexcept : conn_(conn) {}

    domain::Result<std::unique_ptr<domain::BlobWriter>> create() override;
    domain::Result<domain::BlobInfo> info(domain::BlobId id) override;
    domain::Result<Bytes> readChunk(domain::BlobId id, std::uint32_t index) override;
    domain::Status discard(domain::BlobId id) override;
    domain::Result<std::size_t> gcPending() override;

private:
    Connection& conn_;
};

class SqliteVaultStore final : public domain::VaultStore {
public:
    SqliteVaultStore() = default;
    ~SqliteVaultStore() override { close(); }

    domain::Status create(const std::filesystem::path& path, const domain::SafeMeta& meta) override;
    domain::Status open(const std::filesystem::path& path) override;
    domain::Result<domain::SafeMeta> inspect(const std::filesystem::path& path) override;
    void close() noexcept override;
    bool isOpen() const override { return conn_.open.load(); }

    domain::Result<domain::SafeMeta> meta() override;
    domain::Result<std::unique_ptr<domain::UnitOfWork>> begin() override;
    domain::BlobStore& blobs() override { return blobs_; }
    domain::Status compact() override;

private:
    Connection conn_;
    SqliteBlobStore blobs_{conn_};
};

} // namespace safebox::infra::sqlite
