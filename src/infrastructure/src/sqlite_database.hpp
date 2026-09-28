// Тонкая RAII-обертка над sqlite3 / sqlite3_stmt.
// Потокобезопасность не обещает, мьютекс живет в SqliteVaultStore
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "safebox/domain/io.hpp"
#include "safebox/domain/model/error.hpp"

struct sqlite3;
struct sqlite3_stmt;

namespace safebox::infra::sqlite {

using domain::Bytes;
using domain::Result;
using domain::Status;

[[nodiscard]] domain::Error sqliteError(int code, std::string_view context, sqlite3* db = nullptr);

class Statement {
public:
    Statement(sqlite3* db, sqlite3_stmt* stmt) noexcept : db_(db), stmt_(stmt) {}
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    Statement(Statement&& other) noexcept;
    Statement& operator=(Statement&& other) noexcept;
    ~Statement();

    Statement& bind(int index, std::int64_t value);
    Statement& bind(int index, std::optional<std::int64_t> value);
    Statement& bind(int index, std::span<const std::byte> blob);
    Statement& bindNull(int index);

    // true - есть строка, false - выполнено до конца; ошибка - Error.
    [[nodiscard]] Result<bool> step();
    // Выполнить до конца, строки не ожидаются.
    [[nodiscard]] Status run();

    [[nodiscard]] std::int64_t int64(int column) const;
    [[nodiscard]] std::optional<std::int64_t> optionalInt64(int column) const;
    [[nodiscard]] Bytes blob(int column) const;
    [[nodiscard]] bool isNull(int column) const;

    void reset() noexcept;

private:
    sqlite3* db_ = nullptr;
    sqlite3_stmt* stmt_ = nullptr;
};

// Ссылка на закэшированное выражение: при уничтожении сбрасывает его (reset),
// чтобы следующий вызов начал с чистых параметров.
class StatementRef {
public:
    explicit StatementRef(Statement& st) noexcept : st_(&st) {}
    StatementRef(StatementRef&& other) noexcept : st_(std::exchange(other.st_, nullptr)) {}
    StatementRef& operator=(StatementRef&&) = delete;
    StatementRef(const StatementRef&) = delete;
    StatementRef& operator=(const StatementRef&) = delete;
    ~StatementRef() {
        if (st_ != nullptr) {
            st_->reset();
        }
    }

    Statement* operator->() const noexcept { return st_; }
    Statement& operator*() const noexcept { return *st_; }

private:
    Statement* st_;
};

enum class OpenMode {
    Create,    // новый файл (SQLITE_OPEN_CREATE)
    ReadWrite, // существующий файл
    ReadOnly,  // существующий файл, только чтение (inspect)
};

class Database {
public:
    [[nodiscard]] static Result<std::unique_ptr<Database>> open(const std::filesystem::path& path,
                                                                OpenMode mode);
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    ~Database();

    [[nodiscard]] Status exec(std::string_view sql);
    [[nodiscard]] Result<StatementRef> cached(std::string_view sql);
    [[nodiscard]] Result<Statement> prepare(std::string_view sql);

    [[nodiscard]] std::int64_t lastInsertRowId() const;
    [[nodiscard]] int changes() const;
    [[nodiscard]] Result<std::int64_t> pragmaInt(std::string_view name);

    [[nodiscard]] sqlite3* handle() const noexcept { return db_; }

private:
    explicit Database(sqlite3* db) noexcept : db_(db) {}

    sqlite3* db_ = nullptr;
    std::unordered_map<std::string, std::unique_ptr<Statement>> cache_;
};

} // namespace safebox::infra::sqlite
