// JSON для ответов, должно совпадать с контрактом REST API (Linqtab) и types.ts на фронте
#pragma once

#include <string_view>
#include <vector>

#include "api.hpp"

namespace safebox::http {

[[nodiscard]] std::string_view kindName(domain::Kind kind) noexcept;

[[nodiscard]] Json toJson(const domain::Entry& entry);
[[nodiscard]] Json toJson(const std::vector<domain::PathItem>& path);
[[nodiscard]] Json toJson(const app::FolderListing& listing);
[[nodiscard]] Json toJson(const app::FolderNode& node);
[[nodiscard]] Json toJson(const app::MoveConflict& conflict);
[[nodiscard]] Json toJson(const app::MoveResult& result);
[[nodiscard]] Json toJson(const app::SafeInfo& info);
[[nodiscard]] Json toJson(const app::PublicStatus& status);
[[nodiscard]] Json toJson(const domain::SearchHit& hit);
[[nodiscard]] Json toJson(const domain::Tag& tag);
[[nodiscard]] Json toJson(const app::CategoryWithTags& category);
[[nodiscard]] Json toJson(const app::RemovedTags& removed);
[[nodiscard]] Json toJson(const app::ImportPlan& plan);
[[nodiscard]] Json toJson(const domain::ImportResult& result);

} // namespace safebox::http
