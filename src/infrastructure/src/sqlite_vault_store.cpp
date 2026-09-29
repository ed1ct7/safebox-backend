// VaultStore на sqlite.
// removeSubtree через рекурсивный CTE с UNION, чтобы не зациклиться на битом parent_id.
// close(): в EXCLUSIVE режиме sqlite не удаляет -journal, а только обнуляет, так что удаляем сами
#include "sqlite_vault_store.hpp"

#include <array>
#include <fstream>
#include <system_error>

#include "safebox/infra/factories.hpp"
#include "sqlite_schema.hpp"

namespace safebox::infra::sqlite {

using domain::BlobId;
using domain::EntryId;
using domain::EntryRecord;
using domain::Error;
using domain::fail;
using domain::Result;
using domain::Status;

Result<std::unique_lock<std::mutex>> lockOpen(Connection& conn) {
    if (conn.uowOwner.load() == std::this_thread::get_id()) {
        return fail(Error::Code::Internal,
                    "Операция хранилища вызвана внутри открытого UnitOfWork");
    }
    std::unique_lock lock(conn.mutex);
    if (!conn.db) {
        return fail(Error::Code::Locked, "Сейф заблокирован");
    }
    return lock;
}

namespace {

constexpr std::string_view kEntryColumns =
    "id, parent_id, is_folder, blob_id, thumb_blob_id, enc_name, enc_meta";

[[nodiscard]] EntryRecord readEntry(const Statement& st) {
    EntryRecord r;
    r.id = st.int64(0);
    r.parentId = st.optionalInt64(1);
    r.isFolder = st.int64(2) != 0;
    r.blobId = st.optionalInt64(3);
    r.thumbBlobId = st.optionalInt64(4);
    r.encName = st.blob(5);
    r.encMeta = st.blob(6);
    return r;
}

[[nodiscard]] Result<std::vector<EntryRecord>> readEntries(Statement& st) {
    std::vector<EntryRecord> out;
    for (;;) {
        auto row = st.step();
        if (!row) {
            return std::unexpected(row.error());
        }
        if (!*row) {
            return out;
        }
        out.push_back(readEntry(st));
    }
}

class SqliteEntryRepository final : public domain::EntryRepository {
public:
    explicit SqliteEntryRepository(Database& db) noexcept : db_(db) {}

    Result<EntryId> insert(const EntryRecord& record) override {
        if (auto parent = requireParent(record.parentId); !parent) {
            return std::unexpected(parent.error());
        }
        auto st = db_.cached("INSERT INTO entries(parent_id, is_folder, blob_id, thumb_blob_id,"
                             " enc_name, enc_meta) VALUES (?1, ?2, ?3, ?4, ?5, ?6)");
        if (!st) {
            return std::unexpected(st.error());
        }
        (*st)
            ->bind(1, record.parentId)
            .bind(2, std::int64_t{record.isFolder ? 1 : 0})
            .bind(3, record.blobId)
            .bind(4, record.thumbBlobId)
            .bind(5, std::span<const std::byte>(record.encName))
            .bind(6, std::span<const std::byte>(record.encMeta));
        if (auto done = (*st)->run(); !done) {
            return std::unexpected(done.error());
        }
        return db_.lastInsertRowId();
    }

    Status updateSealed(EntryId id, std::span<const std::byte> encName,
                        std::span<const std::byte> encMeta) override {
        auto st = db_.cached("UPDATE entries SET enc_name = ?1, enc_meta = ?2 WHERE id = ?3");
        if (!st) {
            return std::unexpected(st.error());
        }
        (*st)->bind(1, encName).bind(2, encMeta).bind(3, id);
        if (auto done = (*st)->run(); !done) {
            return done;
        }
        if (db_.changes() == 0) {
            return fail(Error::Code::NotFound, "Объект не найден");
        }
        return {};
    }

    Status update(const EntryRecord& record) override {
        if (auto parent = requireParent(record.parentId); !parent) {
            return parent;
        }
        auto st = db_.cached("UPDATE entries SET parent_id = ?1, is_folder = ?2, blob_id = ?3,"
                             " thumb_blob_id = ?4, enc_name = ?5, enc_meta = ?6 WHERE id = ?7");
        if (!st) {
            return std::unexpected(st.error());
        }
        (*st)
            ->bind(1, record.parentId)
            .bind(2, std::int64_t{record.isFolder ? 1 : 0})
            .bind(3, record.blobId)
            .bind(4, record.thumbBlobId)
            .bind(5, std::span<const std::byte>(record.encName))
            .bind(6, std::span<const std::byte>(record.encMeta))
            .bind(7, record.id);
        if (auto done = (*st)->run(); !done) {
            return done;
        }
        if (db_.changes() == 0) {
            return fail(Error::Code::NotFound, "Объект не найден");
        }
        return {};
    }

    Result<EntryRecord> get(EntryId id) override {
        auto st = db_.cached(
            std::string("SELECT ").append(kEntryColumns).append(" FROM entries WHERE id = ?1"));
        if (!st) {
            return std::unexpected(st.error());
        }
        (*st)->bind(1, id);
        auto row = (*st)->step();
        if (!row) {
            return std::unexpected(row.error());
        }
        if (!*row) {
            return fail(Error::Code::NotFound, "Объект не найден");
        }
        return readEntry(**st);
    }

    Result<std::vector<EntryRecord>> children(std::optional<EntryId> parent) override {
        auto st = db_.cached(std::string("SELECT ")
                                 .append(kEntryColumns)
                                 .append(" FROM entries WHERE parent_id IS ?1 ORDER BY id"));
        if (!st) {
            return std::unexpected(st.error());
        }
        (*st)->bind(1, parent);
        return readEntries(**st);
    }

    Result<std::vector<EntryRecord>> all() override {
        auto st = db_.cached(
            std::string("SELECT ").append(kEntryColumns).append(" FROM entries ORDER BY id"));
        if (!st) {
            return std::unexpected(st.error());
        }
        return readEntries(**st);
    }

    Result<std::uint64_t> count() override {
        auto st = db_.cached("SELECT count(*) FROM entries");
        if (!st) {
            return std::unexpected(st.error());
        }
        auto row = (*st)->step();
        if (!row) {
            return std::unexpected(row.error());
        }
        return static_cast<std::uint64_t>((*st)->int64(0));
    }

    Result<domain::RemovedEntries> removeSubtree(EntryId id) override {
        static constexpr std::string_view kSubtree =
            "WITH RECURSIVE sub(id) AS ("
            " SELECT id FROM entries WHERE id = ?1"
            " UNION"
            " SELECT e.id FROM entries e JOIN sub ON e.parent_id = sub.id) ";

        domain::RemovedEntries removed;
        {
            auto st = db_.cached(std::string(kSubtree).append(
                "SELECT id, blob_id, thumb_blob_id FROM entries WHERE id IN (SELECT id FROM sub)"));
            if (!st) {
                return std::unexpected(st.error());
            }
            (*st)->bind(1, id);
            for (;;) {
                auto row = (*st)->step();
                if (!row) {
                    return std::unexpected(row.error());
                }
                if (!*row) {
                    break;
                }
                removed.entries.push_back((*st)->int64(0));
                for (int column : {1, 2}) {
                    if (auto blob = (*st)->optionalInt64(column)) {
                        removed.blobs.push_back(*blob);
                    }
                }
            }
        }
        if (removed.entries.empty()) {
            return fail(Error::Code::NotFound, "Объект не найден");
        }
        {
            auto st = db_.cached(std::string(kSubtree).append(
                "DELETE FROM entries WHERE id IN (SELECT id FROM sub)"));
            if (!st) {
                return std::unexpected(st.error());
            }
            (*st)->bind(1, id);
            if (auto done = (*st)->run(); !done) {
                return std::unexpected(done.error());
            }
        }
        // Куски уходят каскадом (chunks.blob_id ON DELETE CASCADE).
        auto st = db_.cached("DELETE FROM blobs WHERE id = ?1 AND NOT EXISTS ("
                             " SELECT 1 FROM entries WHERE blob_id = ?1 OR thumb_blob_id = ?1)");
        if (!st) {
            return std::unexpected(st.error());
        }
        for (const auto blob : removed.blobs) {
            (*st)->bind(1, blob);
            if (auto done = (*st)->run(); !done) {
                return std::unexpected(done.error());
            }
            (*st)->reset();
        }
        return removed;
    }

private:
    // Родителем может быть любая запись (у файла бывают вложения).
    Status requireParent(std::optional<EntryId> parent) {
        if (!parent) {
            return {};
        }
        auto st = db_.cached("SELECT 1 FROM entries WHERE id = ?1");
        if (!st) {
            return std::unexpected(st.error());
        }
        (*st)->bind(1, *parent);
        auto row = (*st)->step();
        if (!row) {
            return std::unexpected(row.error());
        }
        if (!*row) {
            return fail(Error::Code::NotFound, "Объект назначения не найден");
        }
        return {};
    }

    Database& db_;
};

class SqliteBlobRepository final : public domain::BlobRepository {
public:
    explicit SqliteBlobRepository(Database& db) noexcept : db_(db) {}

    Status promote(BlobId id) override {
        auto st = db_.cached("UPDATE blobs SET status = 1 WHERE id = ?1 AND status = 0");
        if (!st) {
            return std::unexpected(st.error());
        }
        (*st)->bind(1, id);
        if (auto done = (*st)->run(); !done) {
            return done;
        }
        if (db_.changes() == 0) {
            return fail(Error::Code::NotFound, "Данные файла не найдены");
        }
        return {};
    }

    Status remove(BlobId id) override {
        // Куски уходят каскадом (chunks.blob_id ON DELETE CASCADE).
        auto st = db_.cached("DELETE FROM blobs WHERE id = ?1");
        if (!st) {
            return std::unexpected(st.error());
        }
        (*st)->bind(1, id);
        return (*st)->run();
    }

private:
    Database& db_;
};

class SqliteTagRepository final : public domain::TagRepository {
public:
    explicit SqliteTagRepository(Database& db) noexcept : db_(db) {}

    Result<domain::CategoryId> insertCategory() override {
        auto st = db_.cached("INSERT INTO tag_categories(enc_name) VALUES (x'')");
        if (!st) {
            return std::unexpected(st.error());
        }
        if (auto done = (*st)->run(); !done) {
            return std::unexpected(done.error());
        }
        return db_.lastInsertRowId();
    }

    Status updateCategory(domain::CategoryId id, std::span<const std::byte> encName) override {
        auto st = db_.cached("UPDATE tag_categories SET enc_name = ?1 WHERE id = ?2");
        if (!st) {
            return std::unexpected(st.error());
        }
        (*st)->bind(1, encName).bind(2, id);
        return runChanging(**st, "Категория не найдена");
    }

    Status removeCategory(domain::CategoryId id) override {
        auto st = db_.cached("DELETE FROM tag_categories WHERE id = ?1");
        if (!st) {
            return std::unexpected(st.error());
        }
        (*st)->bind(1, id);
        return runChanging(**st, "Категория не найдена");
    }

    Result<std::vector<domain::TagCategoryRecord>> categories() override {
        auto st = db_.cached("SELECT id, enc_name FROM tag_categories ORDER BY id");
        if (!st) {
            return std::unexpected(st.error());
        }
        std::vector<domain::TagCategoryRecord> out;
        for (;;) {
            auto row = (*st)->step();
            if (!row) {
                return std::unexpected(row.error());
            }
            if (!*row) {
                return out;
            }
            out.push_back({(*st)->int64(0), (*st)->blob(1)});
        }
    }

    Result<domain::TagId> insertTag(domain::CategoryId category) override {
        if (auto found = requireCategory(category); !found) {
            return std::unexpected(found.error());
        }
        auto st = db_.cached("INSERT INTO tags(category_id, enc_name) VALUES (?1, x'')");
        if (!st) {
            return std::unexpected(st.error());
        }
        (*st)->bind(1, category);
        if (auto done = (*st)->run(); !done) {
            return std::unexpected(done.error());
        }
        return db_.lastInsertRowId();
    }

    Status updateTag(domain::TagId id, domain::CategoryId category,
                     std::span<const std::byte> encName) override {
        if (auto found = requireCategory(category); !found) {
            return found;
        }
        auto st = db_.cached("UPDATE tags SET category_id = ?1, enc_name = ?2 WHERE id = ?3");
        if (!st) {
            return std::unexpected(st.error());
        }
        (*st)->bind(1, category).bind(2, encName).bind(3, id);
        return runChanging(**st, "Тег не найден");
    }

    Status removeTag(domain::TagId id) override {
        auto st = db_.cached("DELETE FROM tags WHERE id = ?1");
        if (!st) {
            return std::unexpected(st.error());
        }
        (*st)->bind(1, id);
        return runChanging(**st, "Тег не найден");
    }

    Result<std::vector<domain::TagRecord>> tags() override {
        auto st = db_.cached("SELECT id, category_id, enc_name FROM tags ORDER BY id");
        if (!st) {
            return std::unexpected(st.error());
        }
        std::vector<domain::TagRecord> out;
        for (;;) {
            auto row = (*st)->step();
            if (!row) {
                return std::unexpected(row.error());
            }
            if (!*row) {
                return out;
            }
            out.push_back({(*st)->int64(0), (*st)->int64(1), (*st)->blob(2)});
        }
    }

private:
    Status runChanging(Statement& st, std::string_view notFound) {
        if (auto done = st.run(); !done) {
            return done;
        }
        if (db_.changes() == 0) {
            return fail(Error::Code::NotFound, std::string(notFound));
        }
        return {};
    }

    Status requireCategory(domain::CategoryId category) {
        auto st = db_.cached("SELECT 1 FROM tag_categories WHERE id = ?1");
        if (!st) {
            return std::unexpected(st.error());
        }
        (*st)->bind(1, category);
        auto row = (*st)->step();
        if (!row) {
            return std::unexpected(row.error());
        }
        if (!*row) {
            return fail(Error::Code::NotFound, "Категория не найдена");
        }
        return {};
    }

    Database& db_;
};

class SqliteUnitOfWork final : public domain::UnitOfWork {
public:
    SqliteUnitOfWork(std::unique_lock<std::mutex> lock, Connection& conn) noexcept
        : lock_(std::move(lock)), conn_(conn), entries_(*conn.db), blobs_(*conn.db),
          tags_(*conn.db) {}

    ~SqliteUnitOfWork() override {
        if (!finished_) {
            (void)conn_.db->exec("ROLLBACK");
        }
        conn_.uowOwner.store(std::thread::id{});
    }

    domain::EntryRepository& entries() override { return entries_; }
    domain::BlobRepository& blobs() override { return blobs_; }
    domain::TagRepository& tags() override { return tags_; }

    Status saveMeta(const domain::SafeMeta& meta) override {
        auto st = conn_.db->cached(
            "UPDATE meta SET format_version = ?1, kdf_ops = ?2, kdf_mem = ?3, salt = ?4,"
            " chunk_size = ?5, envelope = ?6 WHERE id = 1");
        if (!st) {
            return std::unexpected(st.error());
        }
        (*st)
            ->bind(1, static_cast<std::int64_t>(meta.formatVersion))
            .bind(2, static_cast<std::int64_t>(meta.kdf.ops))
            .bind(3, static_cast<std::int64_t>(meta.kdf.mem))
            .bind(4, std::span<const std::byte>(meta.salt))
            .bind(5, static_cast<std::int64_t>(meta.chunkSize))
            .bind(6, std::span<const std::byte>(meta.envelope));
        return (*st)->run();
    }

    Status commit() override {
        if (finished_) {
            return fail(Error::Code::Internal, "UnitOfWork уже завершён");
        }
        auto st = conn_.db->exec("COMMIT");
        if (!st) {
            (void)conn_.db->exec("ROLLBACK");
        }
        finished_ = true;
        return st;
    }

private:
    std::unique_lock<std::mutex> lock_;
    Connection& conn_;
    SqliteEntryRepository entries_;
    SqliteBlobRepository blobs_;
    SqliteTagRepository tags_;
    bool finished_ = false;
};

[[nodiscard]] std::filesystem::path journalOf(const std::filesystem::path& path) {
    auto journal = path;
    journal += "-journal";
    return journal;
}

// Удаляет -journal только если он не "горячий" (пустой или с обнуленным
// заголовком): горячий журнал нужен SQLite для отката при следующем открытии.
void removeStaleJournal(const std::filesystem::path& path) noexcept {
    std::error_code ec;
    const auto journal = journalOf(path);
    if (!std::filesystem::exists(journal, ec)) {
        return;
    }
    {
        std::ifstream in(journal, std::ios::binary);
        std::array<char, 8> magic{};
        in.read(magic.data(), magic.size());
        for (std::streamsize i = 0; i < in.gcount(); ++i) {
            if (magic[static_cast<std::size_t>(i)] != 0) {
                return;
            }
        }
    }
    std::filesystem::remove(journal, ec);
}

// Существует, обычный файл, заголовок SQLite с application_id сейфа.
[[nodiscard]] Status checkSafeFile(const std::filesystem::path& path) {
    std::error_code ec;
    const auto status = std::filesystem::status(path, ec);
    if (!std::filesystem::exists(status)) {
        return fail(Error::Code::NotFound, "Файл сейфа не найден");
    }
    if (!std::filesystem::is_regular_file(status)) {
        return fail(Error::Code::NotASafe, "Файл не является сейфом SafeBox");
    }
    return probeHeader(path);
}

[[nodiscard]] Result<domain::SafeMeta> readMeta(Database& db) {
    auto st = db.cached("SELECT format_version, kdf_ops, kdf_mem, salt, chunk_size, envelope"
                        " FROM meta WHERE id = 1");
    if (!st) {
        return std::unexpected(st.error());
    }
    auto row = (*st)->step();
    if (!row) {
        return std::unexpected(row.error());
    }
    if (!*row) {
        return fail(Error::Code::NotASafe, "В файле сейфа нет служебных данных");
    }
    domain::SafeMeta meta;
    meta.formatVersion = static_cast<std::uint32_t>((*st)->int64(0));
    meta.kdf.ops = static_cast<std::uint64_t>((*st)->int64(1));
    meta.kdf.mem = static_cast<std::uint64_t>((*st)->int64(2));
    meta.salt = (*st)->blob(3);
    const auto chunk = (*st)->int64(4);
    meta.chunkSize = chunk > 0 && chunk <= 0xFFFFFFFF ? static_cast<std::uint32_t>(chunk) : 0;
    meta.envelope = (*st)->blob(5);
    return meta;
}

} // namespace

Status SqliteVaultStore::create(const std::filesystem::path& path, const domain::SafeMeta& meta) {
    std::scoped_lock lock(conn_.mutex);
    if (conn_.db) {
        return fail(Error::Code::Internal, "Хранилище уже открыто");
    }
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
        return fail(Error::Code::AlreadyExists, "Файл уже существует");
    }
    const auto parent = path.parent_path();
    if (parent.empty() || !std::filesystem::is_directory(parent, ec)) {
        return fail(Error::Code::InvalidArgument,
                    "Папка не найдена: " + domain::pathToUtf8(parent));
    }
    auto db = Database::open(path, OpenMode::Create);
    if (!db) {
        return fail(Error::Code::IoError, "Не удалось создать файл сейфа: " + db.error().message);
    }
    if (auto st = createSchema(**db, meta); !st) {
        db->reset();
        std::filesystem::remove(path, ec);
        std::filesystem::remove(journalOf(path), ec);
        return fail(st.error().code, "Не удалось создать файл сейфа: " + st.error().message);
    }
    conn_.db = std::move(*db);
    conn_.path = path;
    conn_.open.store(true);
    return {};
}

Status SqliteVaultStore::open(const std::filesystem::path& path) {
    std::scoped_lock lock(conn_.mutex);
    if (conn_.db) {
        return fail(Error::Code::Internal, "Хранилище уже открыто");
    }
    if (auto st = checkSafeFile(path); !st) {
        return st;
    }
    auto db = Database::open(path, OpenMode::ReadWrite);
    if (!db) {
        return std::unexpected(db.error());
    }
    if (auto st = configure(**db); !st) {
        return st;
    }
    if (auto st = verifyOpened(**db); !st) {
        return st;
    }
    conn_.db = std::move(*db);
    conn_.path = path;
    conn_.open.store(true);
    return {};
}

Result<domain::SafeMeta> SqliteVaultStore::inspect(const std::filesystem::path& path) {
    // Отдельное соединение только на чтение: мьютекс и открытый сейф не трогаем.
    if (auto st = checkSafeFile(path); !st) {
        return std::unexpected(st.error());
    }
    auto db = Database::open(path, OpenMode::ReadOnly);
    if (!db) {
        return std::unexpected(db.error());
    }
    if (auto st = verifyOpened(**db); !st) {
        return std::unexpected(st.error());
    }
    return readMeta(**db);
}

void SqliteVaultStore::close() noexcept {
    std::scoped_lock lock(conn_.mutex);
    if (!conn_.db) {
        return;
    }
    conn_.open.store(false);
    conn_.db.reset();
    removeStaleJournal(conn_.path);
    conn_.path.clear();
}

Result<domain::SafeMeta> SqliteVaultStore::meta() {
    auto lock = lockOpen(conn_);
    if (!lock) {
        return std::unexpected(lock.error());
    }
    return readMeta(*conn_.db);
}

Result<std::unique_ptr<domain::UnitOfWork>> SqliteVaultStore::begin() {
    auto lock = lockOpen(conn_);
    if (!lock) {
        return std::unexpected(lock.error());
    }
    if (auto st = conn_.db->exec("BEGIN IMMEDIATE"); !st) {
        return std::unexpected(st.error());
    }
    conn_.uowOwner.store(std::this_thread::get_id());
    return std::unique_ptr<domain::UnitOfWork>(
        std::make_unique<SqliteUnitOfWork>(std::move(*lock), conn_));
}

Status SqliteVaultStore::compact() {
    auto lock = lockOpen(conn_);
    if (!lock) {
        return std::unexpected(lock.error());
    }
    return conn_.db->exec("PRAGMA incremental_vacuum;");
}

} // namespace safebox::infra::sqlite

namespace safebox::infra {

std::unique_ptr<domain::VaultStore> makeSqliteVaultStore() {
    return std::make_unique<sqlite::SqliteVaultStore>();
}

} // namespace safebox::infra
