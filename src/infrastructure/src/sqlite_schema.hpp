#pragma once

#include <filesystem>

#include "safebox/domain/ports/storage.hpp"
#include "sqlite_database.hpp"

namespace safebox::infra::sqlite {

[[nodiscard]] domain::Status probeHeader(const std::filesystem::path& path);
[[nodiscard]] domain::Status configure(Database& db);
[[nodiscard]] domain::Status createSchema(Database& db, const domain::SafeMeta& meta);
[[nodiscard]] domain::Status verifyOpened(Database& db);
// Поднимает user_version до kFormatVersion (сейчас миграция v2 -> v3: колонки enc_name_en).
// Звать после verifyOpened на открытом соединении; идемпотентна.
[[nodiscard]] domain::Status migrateIfNeeded(Database& db);

} // namespace safebox::infra::sqlite
