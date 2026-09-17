// Открываем с DEFENSIVE, trusted_schema=OFF и без триггеров/view - файл могут подсунуть
#include "sqlite_database.hpp"

#include <fmt/format.h>
#include <sqlite3.h>

#include <utility>

namespace safebox::infra::sqlite {

using domain::Error;
using domain::fail;

Error sqliteError(int code, std::string_view context, sqlite3* db) {
    const int primary = code & 0xFF;
    switch (primary) {
    case SQLITE_BUSY:
    case SQLITE_LOCKED:
        return {Error::Code::IoError, "Сейф уже открыт другим процессом"};
    case SQLITE_NOTADB:
        return {Error::Code::NotASafe, "Файл не является сейфом SafeBox"};
    case SQLITE_CORRUPT:
        return {Error::Code::IntegrityError, "Файл сейфа повреждён"};
    case SQLITE_CONSTRAINT:
        return {Error::Code::IntegrityError, "Нарушена целостность данных сейфа"};
    case SQLITE_FULL:
        return {Error::Code::IoError, "Недостаточно места на диске"};
    case SQLITE_READONLY:
        return {Error::Code::IoError, "Файл сейфа доступен только для чтения"};
    case SQLITE_PERM:
    case SQLITE_AUTH:
        return {Error::Code::IoError, "Нет доступа к файлу сейфа"};
    case SQLITE_CANTOPEN:
        return {Error::Code::IoError, "Не удалось открыть файл сейфа"};
    case SQLITE_NOMEM:
        return {Error::Code::Internal, "Недостаточно памяти"};
    default:
        break;
    }
    const char* detail = db != nullptr ? sqlite3_errmsg(db) : sqlite3_errstr(code);
    return {Error::Code::IoError, fmt::format("Ошибка файла сейфа ({}): {}", context, detail)};
}

// Statement

Statement::Statement(Statement&& other) noexcept
    : db_(std::exchange(other.db_, nullptr)), stmt_(std::exchange(other.stmt_, nullptr)) {}

Statement& Statement::operator=(Statement&& other) noexcept {
    if (this != &other) {
        sqlite3_finalize(stmt_);
        db_ = std::exchange(other.db_, nullptr);
        stmt_ = std::exchange(other.stmt_, nullptr);
    }
    return *this;
}

Statement::~Statement() {
    sqlite3_finalize(stmt_);
}

Statement& Statement::bind(int index, std::int64_t value) {
    sqlite3_bind_int64(stmt_, index, value);
    return *this;
}

Statement& Statement::bind(int index, std::optional<std::int64_t> value) {
    if (value) {
        sqlite3_bind_int64(stmt_, index, *value);
    } else {
        sqlite3_bind_null(stmt_, index);
    }
    return *this;
}

Statement& Statement::bind(int index, std::span<const std::byte> blob) {
    if (blob.empty()) {
        // nullptr в bind_blob означал бы NULL, а нужен пустой BLOB
        sqlite3_bind_zeroblob(stmt_, index, 0);
    } else {
        // STATIC: данные живут до step() в той же области - без копии 8 МиБ
        sqlite3_bind_blob64(stmt_, index, blob.data(), blob.size(), SQLITE_STATIC);
    }
    return *this;
}

Statement& Statement::bindNull(int index) {
    sqlite3_bind_null(stmt_, index);
    return *this;
}

Result<bool> Statement::step() {
    const int rc = sqlite3_step(stmt_);
    if (rc == SQLITE_ROW) {
        return true;
    }
    if (rc == SQLITE_DONE) {
        return false;
    }
    return std::unexpected(sqliteError(rc, "step", db_));
}

Status Statement::run() {
    for (;;) {
        auto more = step();
        if (!more) {
            return std::unexpected(more.error());
        }
        if (!*more) {
            return {};
        }
    }
}

std::int64_t Statement::int64(int column) const {
    return sqlite3_column_int64(stmt_, column);
}

std::optional<std::int64_t> Statement::optionalInt64(int column) const {
    if (isNull(column)) {
        return std::nullopt;
    }
    return sqlite3_column_int64(stmt_, column);
}

Bytes Statement::blob(int column) const {
    const auto* data = static_cast<const std::byte*>(sqlite3_column_blob(stmt_, column));
    const auto size = static_cast<std::size_t>(sqlite3_column_bytes(stmt_, column));
    if (data == nullptr || size == 0) {
        return {};
    }
    return Bytes(data, data + size);
}

bool Statement::isNull(int column) const {
    return sqlite3_column_type(stmt_, column) == SQLITE_NULL;
}

void Statement::reset() noexcept {
    sqlite3_reset(stmt_);
    sqlite3_clear_bindings(stmt_);
}

// Database

Result<std::unique_ptr<Database>> Database::open(const std::filesystem::path& path, OpenMode mode) {
    int flags = SQLITE_OPEN_EXRESCODE | SQLITE_OPEN_NOMUTEX | SQLITE_OPEN_PRIVATECACHE;
    switch (mode) {
    case OpenMode::Create:
        flags |= SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
        break;
    case OpenMode::ReadWrite:
        flags |= SQLITE_OPEN_READWRITE;
        break;
    case OpenMode::ReadOnly:
        flags |= SQLITE_OPEN_READONLY;
        break;
    }
    sqlite3* db = nullptr;
    const auto utf8 = domain::pathToUtf8(path);
    const int rc = sqlite3_open_v2(utf8.c_str(), &db, flags, nullptr);
    if (rc != SQLITE_OK) {
        auto error = sqliteError(rc, "open", db);
        sqlite3_close_v2(db);
        return std::unexpected(std::move(error));
    }
    sqlite3_busy_timeout(db, 200);
    sqlite3_db_config(db, SQLITE_DBCONFIG_DEFENSIVE, 1, nullptr);
    sqlite3_db_config(db, SQLITE_DBCONFIG_TRUSTED_SCHEMA, 0, nullptr);
    sqlite3_db_config(db, SQLITE_DBCONFIG_ENABLE_TRIGGER, 0, nullptr);
    sqlite3_db_config(db, SQLITE_DBCONFIG_ENABLE_VIEW, 0, nullptr);
    sqlite3_db_config(db, SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION, 0, nullptr);
    return std::unique_ptr<Database>(new Database(db));
}

Database::~Database() {
    cache_.clear(); // finalize до close
    sqlite3_close_v2(db_);
}

Status Database::exec(std::string_view sql) {
    const std::string text(sql);
    char* message = nullptr;
    const int rc = sqlite3_exec(db_, text.c_str(), nullptr, nullptr, &message);
    sqlite3_free(message);
    if (rc != SQLITE_OK) {
        return std::unexpected(sqliteError(rc, "exec", db_));
    }
    return {};
}

Result<Statement> Database::prepare(std::string_view sql) {
    sqlite3_stmt* stmt = nullptr;
    const int rc = sqlite3_prepare_v3(db_, sql.data(), static_cast<int>(sql.size()),
                                      SQLITE_PREPARE_PERSISTENT, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return std::unexpected(sqliteError(rc, "prepare", db_));
    }
    return Statement(db_, stmt);
}

Result<StatementRef> Database::cached(std::string_view sql) {
    const std::string key(sql);
    auto it = cache_.find(key);
    if (it == cache_.end()) {
        auto st = prepare(sql);
        if (!st) {
            return std::unexpected(st.error());
        }
        it = cache_.emplace(key, std::make_unique<Statement>(std::move(*st))).first;
    }
    return StatementRef(*it->second);
}

std::int64_t Database::lastInsertRowId() const {
    return sqlite3_last_insert_rowid(db_);
}

int Database::changes() const {
    return sqlite3_changes(db_);
}

Result<std::int64_t> Database::pragmaInt(std::string_view name) {
    auto st = prepare(fmt::format("PRAGMA {}", name));
    if (!st) {
        return std::unexpected(st.error());
    }
    auto row = st->step();
    if (!row) {
        return std::unexpected(row.error());
    }
    if (!*row) {
        return fail(Error::Code::IoError, fmt::format("PRAGMA {} не вернула значение", name));
    }
    return st->int64(0);
}

} // namespace safebox::infra::sqlite
