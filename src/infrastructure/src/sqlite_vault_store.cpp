// VaultStore на sqlite.
// removeSubtree через рекурсивный CTE с UNION, чтобы не зациклиться на битом parent_id.
// close(): в EXCLUSIVE режиме sqlite не удаляет -journal, а только обнуляет, так что удаляем сами
#include "sqlite_vault_store.hpp"

#include <sqlite3.h>

#include <array>
#include <chrono>
#include <fstream>
#include <optional>
#include <system_error>

#include "safebox/domain/model/safe_format.hpp"
#include "safebox/infra/factories.hpp"
#include "sqlite_schema.hpp"

#if defined(_WIN32)
#include <windows.h> // WIN32_LEAN_AND_MEAN и NOMINMAX - из CMakeLists
#else
#include <cerrno>
#include <unistd.h>
#endif

namespace safebox::infra::sqlite {

using domain::BlobId;
using domain::EntryId;
using domain::EntryRecord;
using domain::Error;
using domain::fail;
using domain::Result;
using domain::Status;

std::unique_lock<std::mutex> lockConn(Connection& conn) {
    struct Waiting {
        std::atomic<int>& n;
        explicit Waiting(std::atomic<int>& count) : n(count) { n.fetch_add(1); }
        ~Waiting() { n.fetch_sub(1); }
        Waiting(const Waiting&) = delete;
        Waiting& operator=(const Waiting&) = delete;
    } waiting(conn.waiting);
    return std::unique_lock(conn.mutex);
}

Result<std::unique_lock<std::mutex>> lockOpen(Connection& conn) {
    if (conn.uowOwner.load() == std::this_thread::get_id()) {
        return fail(Error::Code::Internal,
                    "Операция хранилища вызвана внутри открытого UnitOfWork");
    }
    auto lock = lockConn(conn);
    if (!conn.db) {
        return fail(Error::Code::Locked, "Сейф заблокирован");
    }
    return lock;
}

namespace {

// Переименование без замены: файл, появившийся под именем to, не затирается. true - готово,
// false - имя занято; другая ошибка - в ec.
bool renameNoReplace(const std::filesystem::path& from, const std::filesystem::path& to,
                     std::error_code& ec) {
    ec.clear();
#if defined(_WIN32)
    if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH) != 0) {
        return true;
    }
    const auto err = GetLastError();
    if (err != ERROR_ALREADY_EXISTS && err != ERROR_FILE_EXISTS) {
        ec.assign(static_cast<int>(err), std::system_category());
    }
#else
    if (::link(from.c_str(), to.c_str()) == 0) {
        (void)::unlink(from.c_str()); // не удалось - останется .part, копия уже на месте
        return true;
    }
    const int err = errno;
    if (err == EPERM || err == ENOTSUP || err == EOPNOTSUPP) { // ФС без жестких ссылок (FAT)
        if (std::filesystem::exists(to, ec) || ec) {
            return false;
        }
        std::filesystem::rename(from, to, ec);
        return !ec;
    }
    if (err != EEXIST) {
        ec.assign(err, std::generic_category());
    }
#endif
    return false;
}

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

    Result<BlobId> create() override {
        auto st = db_.cached("INSERT INTO blobs(status, size, chunk_count) VALUES (0, 0, 0)");
        if (!st) {
            return std::unexpected(st.error());
        }
        if (auto done = (*st)->run(); !done) {
            return std::unexpected(done.error());
        }
        return db_.lastInsertRowId();
    }

    Status append(BlobId id, std::uint32_t index, std::span<const std::byte> sealedChunk) override {
        auto st = db_.cached("INSERT INTO chunks(blob_id, idx, data) VALUES (?1, ?2, ?3)");
        if (!st) {
            return std::unexpected(st.error());
        }
        (*st)->bind(1, id).bind(2, static_cast<std::int64_t>(index)).bind(3, sealedChunk);
        return (*st)->run();
    }

    Status finish(BlobId id, std::uint64_t plainSize, std::uint32_t chunkCount) override {
        auto st =
            db_.cached("UPDATE blobs SET size = ?1, chunk_count = ?2 WHERE id = ?3 AND status = 0");
        if (!st) {
            return std::unexpected(st.error());
        }
        (*st)
            ->bind(1, static_cast<std::int64_t>(plainSize))
            .bind(2, static_cast<std::int64_t>(chunkCount))
            .bind(3, id);
        if (auto done = (*st)->run(); !done) {
            return done;
        }
        if (db_.changes() == 0) {
            return fail(Error::Code::NotFound, "Незавершённые данные файла не найдены");
        }
        return {};
    }

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

    Status updateCategory(domain::CategoryId id, std::span<const std::byte> encName,
                          std::span<const std::byte> encNameEn) override {
        auto st =
            db_.cached("UPDATE tag_categories SET enc_name = ?1, enc_name_en = ?2 WHERE id = ?3");
        if (!st) {
            return std::unexpected(st.error());
        }
        (*st)->bind(1, encName).bind(2, encNameEn).bind(3, id);
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
        auto st = db_.cached("SELECT id, enc_name, enc_name_en FROM tag_categories ORDER BY id");
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
            out.push_back({(*st)->int64(0), (*st)->blob(1), (*st)->blob(2)});
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
                     std::span<const std::byte> encName,
                     std::span<const std::byte> encNameEn) override {
        if (auto found = requireCategory(category); !found) {
            return found;
        }
        auto st = db_.cached("UPDATE tags SET category_id = ?1, enc_name = ?2, enc_name_en = ?3"
                             " WHERE id = ?4");
        if (!st) {
            return std::unexpected(st.error());
        }
        (*st)->bind(1, category).bind(2, encName).bind(3, encNameEn).bind(4, id);
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
        auto st = db_.cached("SELECT id, category_id, enc_name, enc_name_en FROM tags ORDER BY id");
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
            out.push_back({(*st)->int64(0), (*st)->int64(1), (*st)->blob(2), (*st)->blob(3)});
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

// Копия: страниц за шаг. 256 * 64 КиБ = 16 МиБ - мьютекс соединения держится десятки мс.
constexpr int kCopyPagesPerStep = 256;
// Шаг уступает соединение ждущим его (lockConn): mutex не честный, и цикл копии, бравший его
// сразу снова, держал запрос (GET /safe/status) до 25 с на копии 40 ГБ. Не дольше этого за шаг,
// чтобы поток запросов не остановил копию совсем.
constexpr auto kCopyYieldAtMost = std::chrono::milliseconds(250);

// Ждущий обычно получает mutex за микросекунды - сначала yield; если он стоит за долгим
// держателем (транзакция), дальше спим по 1 мс, а не крутим ядро.
void yieldToWaiters(const Connection& conn) {
    const auto until = std::chrono::steady_clock::now() + kCopyYieldAtMost;
    for (int spins = 0; conn.waiting.load() > 0 && std::chrono::steady_clock::now() < until;
         ++spins) {
        if (spins < 64) {
            std::this_thread::yield();
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
}

[[nodiscard]] std::filesystem::path partOf(const std::filesystem::path& target) {
    auto part = target;
    part += ".part";
    return part;
}

void removeCopyFiles(const std::filesystem::path& part) noexcept {
    std::error_code ec;
    std::filesystem::remove(part, ec);
    std::filesystem::remove(journalOf(part), ec);
}

// Под conn.mutex: копия больше не связана с исходным соединением, файл копии закрыт.
void abortCopy(CopyState& state) noexcept {
    if (state.backup != nullptr) {
        sqlite3_backup_finish(state.backup);
        state.backup = nullptr;
    }
    state.dest.reset();
}

// sqlite3_backup_* поверх исходного соединения: шаги - под его мьютексом, между шагами идут
// обычные запросы; правки того же соединения SQLite сам переносит в копию.
class SqliteVaultCopy final : public domain::VaultCopy {
public:
    SqliteVaultCopy(Connection& conn, std::filesystem::path target, std::uint64_t pageSize,
                    std::uint64_t pages) noexcept
        : conn_(conn), target_(std::move(target)), part_(partOf(target_)), pageSize_(pageSize),
          pages_(pages), left_(pages) {}

    SqliteVaultCopy(const SqliteVaultCopy&) = delete;
    SqliteVaultCopy& operator=(const SqliteVaultCopy&) = delete;

    ~SqliteVaultCopy() override {
        {
            std::scoped_lock lock(conn_.mutex);
            if (conn_.copy == &state_) {
                conn_.copy = nullptr;
            }
            abortCopy(state_); // close() мог уже оборвать - тогда тут пусто
        }
        if (!done_) {
            removeCopyFiles(part_);
        }
    }

    // Под conn.mutex, до первого step.
    void attach(std::unique_ptr<Database> dest, sqlite3_backup* backup) noexcept {
        state_.dest = std::move(dest);
        state_.backup = backup;
        conn_.copy = &state_;
    }

    Result<bool> step() override {
        if (done_) {
            return true;
        }
        if (conn_.uowOwner.load() == std::this_thread::get_id()) {
            return fail(Error::Code::Internal, "Шаг копии вызван внутри открытого UnitOfWork");
        }
        yieldToWaiters(conn_);
        // не lockConn: шаг сам себе не ждущий
        std::unique_lock lock(conn_.mutex);
        if (!conn_.db) {
            return fail(Error::Code::Locked, "Сейф заблокирован");
        }
        if (conn_.copy != &state_ || state_.backup == nullptr) {
            return fail(Error::Code::Locked, "Копия прервана: сейф закрывался");
        }
        const int rc = sqlite3_backup_step(state_.backup, kCopyPagesPerStep);
        pages_ = static_cast<std::uint64_t>(sqlite3_backup_pagecount(state_.backup));
        left_ = static_cast<std::uint64_t>(sqlite3_backup_remaining(state_.backup));
        const int primary = rc & 0xFF; // открыто с расширенными кодами
        if (primary == SQLITE_OK || primary == SQLITE_BUSY || primary == SQLITE_LOCKED) {
            return false;
        }
        std::optional<Error> error;
        if (primary != SQLITE_DONE) {
            error = sqliteError(rc, "копия сейфа", state_.dest->handle());
        }
        const int finished = sqlite3_backup_finish(state_.backup);
        state_.backup = nullptr;
        conn_.copy = nullptr;
        if (!error && finished != SQLITE_OK) {
            error = sqliteError(finished, "копия сейфа", state_.dest->handle());
        }
        state_.dest.reset(); // при успехе транзакция копии уже зафиксирована (synchronous = FULL)
        if (error) {
            return std::unexpected(std::move(*error));
        }
        lock.unlock();
        return complete();
    }

    std::uint64_t bytesDone() const noexcept override { return (pages_ - left_) * pageSize_; }
    std::uint64_t bytesTotal() const noexcept override { return pages_ * pageSize_; }

private:
    // Копия читается как сейф и только потом получает свое имя.
    Result<bool> complete() {
        {
            auto db = Database::open(part_, OpenMode::ReadOnly);
            if (!db) {
                return std::unexpected(db.error());
            }
            if (auto st = verifyOpened(**db); !st) {
                return fail(Error::Code::IntegrityError,
                            "Копия сейфа не читается: " + st.error().message);
            }
        }
        std::error_code ec;
        const bool moved = renameNoReplace(part_, target_, ec);
        if (ec) {
            return fail(Error::Code::IoError,
                        "Не удалось переименовать копию сейфа: " + domain::pathToUtf8(target_));
        }
        if (!moved) { // файл появился под этим именем, пока шла копия
            return fail(Error::Code::AlreadyExists, "Файл уже существует");
        }
        done_ = true;
        return true;
    }

    Connection& conn_;
    CopyState state_;
    std::filesystem::path target_;
    std::filesystem::path part_;
    std::uint64_t pageSize_ = 0;
    std::uint64_t pages_ = 0;
    std::uint64_t left_ = 0;
    bool done_ = false;
};

} // namespace

Status SqliteVaultStore::create(const std::filesystem::path& path, const domain::SafeMeta& meta) {
    const auto lock = lockConn(conn_);
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
    conn_.schemaCurrent = true;
    conn_.open.store(true);
    return {};
}

Status SqliteVaultStore::open(const std::filesystem::path& path) {
    const auto lock = lockConn(conn_);
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
    auto version = (*db)->pragmaInt("user_version");
    if (!version) {
        return std::unexpected(version.error());
    }
    conn_.db = std::move(*db);
    conn_.path = path;
    conn_.schemaCurrent = *version == static_cast<std::int64_t>(domain::kFormatVersion);
    conn_.open.store(true);
    return {};
}

Status SqliteVaultStore::upgrade() {
    auto lock = lockOpen(conn_);
    if (!lock) {
        return std::unexpected(lock.error());
    }
    if (auto st = migrateIfNeeded(*conn_.db); !st) {
        return st;
    }
    conn_.schemaCurrent = true;
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
    const auto lock = lockConn(conn_);
    if (!conn_.db) {
        return;
    }
    conn_.open.store(false);
    conn_.schemaCurrent = false;
    if (conn_.copy != nullptr) { // копия не переживает закрытие: ее .part удалит владелец
        abortCopy(*conn_.copy);
        conn_.copy = nullptr;
    }
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
    if (!conn_.schemaCurrent) {
        return fail(Error::Code::Internal, "Схема сейфа не обновлена: сначала upgrade()");
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

Result<std::unique_ptr<domain::VaultCopy>>
SqliteVaultStore::backup(const std::filesystem::path& target) {
    auto lock = lockOpen(conn_);
    if (!lock) {
        return std::unexpected(lock.error());
    }
    if (conn_.copy != nullptr) {
        return fail(Error::Code::AlreadyExists, "Копия сейфа уже делается");
    }
    std::error_code ec;
    if (std::filesystem::exists(target, ec)) {
        return fail(Error::Code::AlreadyExists, "Файл уже существует");
    }
    const auto parent = target.parent_path();
    if (parent.empty() || !std::filesystem::is_directory(parent, ec)) {
        return fail(Error::Code::InvalidArgument,
                    "Папка не найдена: " + domain::pathToUtf8(parent));
    }
    auto pageSize = conn_.db->pragmaInt("page_size");
    if (!pageSize) {
        return std::unexpected(pageSize.error());
    }
    auto pages = conn_.db->pragmaInt("page_count");
    if (!pages) {
        return std::unexpected(pages.error());
    }
    const auto part = partOf(target);
    removeCopyFiles(part); // остаток оборванной копии (имя .part - наше)
    if (std::filesystem::exists(part, ec)) {
        return fail(Error::Code::IoError,
                    "Не удалось удалить незаконченную копию: " + domain::pathToUtf8(part));
    }
    auto dest = Database::open(part, OpenMode::Create);
    if (!dest) {
        removeCopyFiles(part);
        return fail(Error::Code::IoError, "Не удалось создать файл копии: " + dest.error().message);
    }
    if (auto st = (*dest)->exec("PRAGMA synchronous = FULL; PRAGMA journal_mode = DELETE;"); !st) {
        dest->reset();
        removeCopyFiles(part);
        return std::unexpected(st.error());
    }
    auto* handle = sqlite3_backup_init((*dest)->handle(), "main", conn_.db->handle(), "main");
    if (handle == nullptr) {
        auto error = sqliteError(sqlite3_extended_errcode((*dest)->handle()), "копия сейфа",
                                 (*dest)->handle());
        dest->reset();
        removeCopyFiles(part);
        return std::unexpected(std::move(error));
    }
    auto copy = std::make_unique<SqliteVaultCopy>(
        conn_, target, static_cast<std::uint64_t>(*pageSize), static_cast<std::uint64_t>(*pages));
    copy->attach(std::move(*dest), handle);
    lock->unlock(); // деструктор копии берет тот же мьютекс
    return std::unique_ptr<domain::VaultCopy>(std::move(copy));
}

} // namespace safebox::infra::sqlite

namespace safebox::infra {

std::unique_ptr<domain::VaultStore> makeSqliteVaultStore() {
    return std::make_unique<sqlite::SqliteVaultStore>();
}

} // namespace safebox::infra
