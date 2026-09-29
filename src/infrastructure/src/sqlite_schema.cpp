// Схема файла, подробнее в Linqtab, «Формат файла *.safebox»
// При создании: page_size=65536, auto_vacuum=INCREMENTAL (потом без VACUUM не поменять).
// На каждом соединении: journal_mode=DELETE (чтобы рядом не было -wal), locking_mode=EXCLUSIVE.
// id сделаны AUTOINCREMENT, чтобы не переиспользовались - они входят в AAD
#include "sqlite_schema.hpp"

#include <fmt/format.h>

#include <array>
#include <fstream>

namespace safebox::infra::sqlite {

using domain::Error;
using domain::fail;
using domain::Status;

namespace {

constexpr std::string_view kSchemaV1 = R"sql(
CREATE TABLE meta (
    id             INTEGER PRIMARY KEY CHECK (id = 1),
    format_version INTEGER NOT NULL,
    kdf_ops        INTEGER NOT NULL,
    kdf_mem        INTEGER NOT NULL,
    salt           BLOB    NOT NULL,
    chunk_size     INTEGER NOT NULL,
    envelope       BLOB    NOT NULL
) STRICT;

CREATE TABLE blobs (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    status      INTEGER NOT NULL DEFAULT 0 CHECK (status IN (0, 1)),
    size        INTEGER NOT NULL DEFAULT 0,
    chunk_count INTEGER NOT NULL DEFAULT 0
) STRICT;

CREATE TABLE chunks (
    blob_id INTEGER NOT NULL REFERENCES blobs(id) ON DELETE CASCADE,
    idx     INTEGER NOT NULL,
    data    BLOB    NOT NULL,
    PRIMARY KEY (blob_id, idx)
) STRICT;

CREATE TABLE entries (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    parent_id     INTEGER REFERENCES entries(id) ON DELETE CASCADE,
    is_folder     INTEGER NOT NULL CHECK (is_folder IN (0, 1)),
    blob_id       INTEGER REFERENCES blobs(id),
    thumb_blob_id INTEGER REFERENCES blobs(id),
    enc_name      BLOB    NOT NULL,
    enc_meta      BLOB    NOT NULL
) STRICT;

CREATE INDEX entries_parent ON entries(parent_id);
)sql";

[[nodiscard]] std::uint32_t readBe32(const std::array<unsigned char, 100>& h, std::size_t at) {
    return (std::uint32_t{h[at]} << 24) | (std::uint32_t{h[at + 1]} << 16) |
           (std::uint32_t{h[at + 2]} << 8) | std::uint32_t{h[at + 3]};
}

[[nodiscard]] std::unexpected<Error> notASafe() {
    return fail(Error::Code::NotASafe, "Файл не является сейфом SafeBox");
}

} // namespace

Status probeHeader(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return fail(Error::Code::IoError, "Нет доступа к файлу сейфа");
    }
    std::array<unsigned char, 100> header{};
    in.read(reinterpret_cast<char*>(header.data()), header.size());
    if (in.gcount() != static_cast<std::streamsize>(header.size())) {
        return notASafe();
    }
    constexpr std::string_view magic{"SQLite format 3\0", 16};
    if (std::string_view(reinterpret_cast<const char*>(header.data()), 16) != magic) {
        return notASafe();
    }
    if (readBe32(header, 68) != static_cast<std::uint32_t>(domain::kApplicationId)) {
        return notASafe();
    }
    const auto version = readBe32(header, 60);
    if (version == 0) {
        return notASafe();
    }
    if (version > domain::kFormatVersion) {
        return fail(Error::Code::NotASafe,
                    "Сейф создан более новой версией SafeBox - обновите приложение");
    }
    return {};
}

Status configure(Database& db) {
    if (auto st = db.exec("PRAGMA foreign_keys = ON;"
                          "PRAGMA synchronous = FULL;"
                          "PRAGMA journal_mode = DELETE;"
                          "PRAGMA temp_store = MEMORY;"
                          "PRAGMA cell_size_check = ON;"
                          "PRAGMA locking_mode = EXCLUSIVE;");
        !st) {
        return st;
    }
    // Захватить эксклюзивную блокировку сразу, а не при первой записи:
    // второй процесс получает "сейф уже открыт" на входе, а не посреди работы.
    return db.exec("BEGIN EXCLUSIVE; COMMIT;");
}

Status createSchema(Database& db, const domain::SafeMeta& meta) {
    // page_size и auto_vacuum - только до первой таблицы.
    if (auto st = db.exec("PRAGMA page_size = 65536; PRAGMA auto_vacuum = INCREMENTAL;"); !st) {
        return st;
    }
    if (auto st = configure(db); !st) {
        return st;
    }
    if (auto st = db.exec("BEGIN IMMEDIATE;"); !st) {
        return st;
    }
    auto body = [&]() -> Status {
        if (auto st = db.exec(kSchemaV1); !st) {
            return st;
        }
        auto insert = db.prepare(
            "INSERT INTO meta(id, format_version, kdf_ops, kdf_mem, salt, chunk_size, envelope)"
            " VALUES (1, ?1, ?2, ?3, ?4, ?5, ?6)");
        if (!insert) {
            return std::unexpected(insert.error());
        }
        insert->bind(1, static_cast<std::int64_t>(meta.formatVersion))
            .bind(2, static_cast<std::int64_t>(meta.kdf.ops))
            .bind(3, static_cast<std::int64_t>(meta.kdf.mem))
            .bind(4, std::span<const std::byte>(meta.salt))
            .bind(5, static_cast<std::int64_t>(meta.chunkSize))
            .bind(6, std::span<const std::byte>(meta.envelope));
        if (auto st = insert->run(); !st) {
            return st;
        }
        return db.exec(fmt::format("PRAGMA application_id = {}; PRAGMA user_version = {};",
                                   domain::kApplicationId, meta.formatVersion));
    };
    if (auto st = body(); !st) {
        (void)db.exec("ROLLBACK;");
        return st;
    }
    return db.exec("COMMIT;");
}

Status verifyOpened(Database& db) {
    auto appId = db.pragmaInt("application_id");
    if (!appId) {
        return std::unexpected(appId.error());
    }
    auto version = db.pragmaInt("user_version");
    if (!version) {
        return std::unexpected(version.error());
    }
    if (*appId != domain::kApplicationId || *version < 1 ||
        *version > static_cast<std::int64_t>(domain::kFormatVersion)) {
        return notASafe();
    }

    // Подложенный файл не должен тащить свои объекты схемы.
    auto st = db.prepare("SELECT"
                         " (SELECT count(*) FROM sqlite_schema WHERE type IN ('trigger', 'view')),"
                         " (SELECT count(*) FROM sqlite_schema WHERE type = 'table'"
                         "   AND name IN ('meta', 'entries', 'blobs', 'chunks')),"
                         " (SELECT count(*) FROM meta)");
    if (!st) {
        return notASafe(); // нет таблицы meta и т.п.
    }
    auto row = st->step();
    if (!row || !*row) {
        return notASafe();
    }
    if (st->int64(0) != 0 || st->int64(1) != 4 || st->int64(2) != 1) {
        return notASafe();
    }

    // Миграции по user_version: версия 1 - текущая, мигрировать нечего.
    switch (*version) {
    case 1:
        break;
    default:
        return notASafe();
    }
    return {};
}

} // namespace safebox::infra::sqlite
