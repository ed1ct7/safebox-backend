#pragma once

#include <filesystem>

#include "safebox/domain/ports/storage.hpp"
#include "sqlite_database.hpp"

namespace safebox::infra::sqlite {

[[nodiscard]] domain::Status probeHeader(const std::filesystem::path& path);
[[nodiscard]] domain::Status configure(Database& db);
[[nodiscard]] domain::Status createSchema(Database& db, const domain::SafeMeta& meta);
[[nodiscard]] domain::Status verifyOpened(Database& db);

} // namespace safebox::infra::sqlite
