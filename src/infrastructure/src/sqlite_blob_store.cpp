// BlobStore для sqlite. Каждый append - своя короткая транзакция, чтобы импорт
// большого файла не держал мьютекс и журнал. gcPending чистит недописанные блобы
#include "sqlite_vault_store.hpp"

namespace safebox::infra::sqlite {

using domain::BlobId;
using domain::Error;
using domain::fail;
using domain::Result;
using domain::Status;

namespace {

class SqliteBlobWriter final : public domain::BlobWriter {
public:
    SqliteBlobWriter(Connection& conn, BlobId id) noexcept : conn_(conn), id_(id) {}

    BlobId id() const override { return id_; }

    Status append(std::span<const std::byte> sealedChunk) override {
        auto lock = lockOpen(conn_);
        if (!lock) {
            return std::unexpected(lock.error());
        }
        auto st = conn_.db->cached("INSERT INTO chunks(blob_id, idx, data) VALUES (?1, ?2, ?3)");
        if (!st) {
            return std::unexpected(st.error());
        }
        (*st)->bind(1, id_).bind(2, static_cast<std::int64_t>(next_)).bind(3, sealedChunk);
        if (auto done = (*st)->run(); !done) {
            return done;
        }
        ++next_;
        return {};
    }

    Status finish(std::uint64_t plainSize) override {
        auto lock = lockOpen(conn_);
        if (!lock) {
            return std::unexpected(lock.error());
        }
        auto st = conn_.db->cached(
            "UPDATE blobs SET size = ?1, chunk_count = ?2 WHERE id = ?3 AND status = 0");
        if (!st) {
            return std::unexpected(st.error());
        }
        (*st)
            ->bind(1, static_cast<std::int64_t>(plainSize))
            .bind(2, static_cast<std::int64_t>(next_))
            .bind(3, id_);
        if (auto done = (*st)->run(); !done) {
            return done;
        }
        if (conn_.db->changes() == 0) {
            return fail(Error::Code::NotFound, "Незавершённые данные файла не найдены");
        }
        return {};
    }

private:
    Connection& conn_;
    BlobId id_;
    std::uint32_t next_ = 0;
};

} // namespace

Result<std::unique_ptr<domain::BlobWriter>> SqliteBlobStore::create() {
    auto lock = lockOpen(conn_);
    if (!lock) {
        return std::unexpected(lock.error());
    }
    auto st = conn_.db->cached("INSERT INTO blobs(status, size, chunk_count) VALUES (0, 0, 0)");
    if (!st) {
        return std::unexpected(st.error());
    }
    if (auto done = (*st)->run(); !done) {
        return std::unexpected(done.error());
    }
    return std::unique_ptr<domain::BlobWriter>(
        std::make_unique<SqliteBlobWriter>(conn_, conn_.db->lastInsertRowId()));
}

Result<domain::BlobInfo> SqliteBlobStore::info(BlobId id) {
    auto lock = lockOpen(conn_);
    if (!lock) {
        return std::unexpected(lock.error());
    }
    auto st = conn_.db->cached("SELECT status, size, chunk_count FROM blobs WHERE id = ?1");
    if (!st) {
        return std::unexpected(st.error());
    }
    (*st)->bind(1, id);
    auto row = (*st)->step();
    if (!row) {
        return std::unexpected(row.error());
    }
    if (!*row) {
        return fail(Error::Code::NotFound, "Данные файла не найдены");
    }
    domain::BlobInfo info;
    info.id = id;
    info.status = (*st)->int64(0) == 1 ? domain::BlobStatus::Ready : domain::BlobStatus::Pending;
    const auto size = (*st)->int64(1);
    const auto count = (*st)->int64(2);
    if (size < 0 || count < 0 || count > 0xFFFFFFFF) {
        return fail(Error::Code::IntegrityError, "Повреждены служебные данные файла");
    }
    info.size = static_cast<std::uint64_t>(size);
    info.chunkCount = static_cast<std::uint32_t>(count);
    return info;
}

Result<domain::Bytes> SqliteBlobStore::readChunk(BlobId id, std::uint32_t index) {
    auto lock = lockOpen(conn_);
    if (!lock) {
        return std::unexpected(lock.error());
    }
    auto st = conn_.db->cached("SELECT data FROM chunks WHERE blob_id = ?1 AND idx = ?2");
    if (!st) {
        return std::unexpected(st.error());
    }
    (*st)->bind(1, id).bind(2, static_cast<std::int64_t>(index));
    auto row = (*st)->step();
    if (!row) {
        return std::unexpected(row.error());
    }
    if (!*row) {
        return fail(Error::Code::NotFound, "Кусок данных не найден");
    }
    return (*st)->blob(0);
}

Status SqliteBlobStore::discard(BlobId id) {
    auto lock = lockOpen(conn_);
    if (!lock) {
        return std::unexpected(lock.error());
    }
    auto st = conn_.db->cached("DELETE FROM blobs WHERE id = ?1 AND status = 0");
    if (!st) {
        return std::unexpected(st.error());
    }
    (*st)->bind(1, id);
    return (*st)->run();
}

Result<std::size_t> SqliteBlobStore::gcPending() {
    auto lock = lockOpen(conn_);
    if (!lock) {
        return std::unexpected(lock.error());
    }
    auto st = conn_.db->cached(
        "DELETE FROM blobs WHERE status = 0"
        " AND id NOT IN (SELECT blob_id FROM entries WHERE blob_id IS NOT NULL)"
        " AND id NOT IN (SELECT thumb_blob_id FROM entries WHERE thumb_blob_id IS NOT NULL)");
    if (!st) {
        return std::unexpected(st.error());
    }
    if (auto done = (*st)->run(); !done) {
        return std::unexpected(done.error());
    }
    const auto removed = static_cast<std::size_t>(conn_.db->changes());
    if (removed > 0) {
        if (auto vacuum = conn_.db->exec("PRAGMA incremental_vacuum;"); !vacuum) {
            return std::unexpected(vacuum.error());
        }
    }
    return removed;
}

} // namespace safebox::infra::sqlite
