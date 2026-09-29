// сериализация DTO и разбор тел запросов
#include <charconv>
#include <limits>

#include "dto.hpp"
#include "safebox/domain/model/rules.hpp"

namespace safebox::http {

// разбор запросов

std::optional<Json> readJsonObject(const httplib::Request& req, httplib::Response& res) {
    const std::string_view body = req.body;
    if (domain::detail::trim(body).empty()) {
        return Json::object(); // тело необязательно (например, POST /safe/lock)
    }
    Json parsed = Json::parse(body, nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        sendError(res, 400, "bad_request", "Тело запроса должно быть JSON-объектом");
        return std::nullopt;
    }
    return parsed;
}

std::optional<std::string> requireString(const Json& body, const char* field,
                                         httplib::Response& res) {
    const auto it = body.find(field);
    if (it == body.end() || !it->is_string()) {
        sendError(res, 400, "bad_request", std::string("Поле '") + field + "' должно быть строкой");
        return std::nullopt;
    }
    return it->get<std::string>();
}

std::optional<domain::EntryId> parseId(std::string_view text) noexcept {
    domain::EntryId id = 0;
    const auto* end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(text.data(), end, id);
    if (text.empty() || ec != std::errc{} || ptr != end || id <= 0) {
        return std::nullopt;
    }
    return id;
}

std::optional<app::ConflictPolicy> parseConflictPolicy(std::string_view text) noexcept {
    if (text == "keepBoth") {
        return app::ConflictPolicy::KeepBoth;
    }
    if (text == "replace") {
        return app::ConflictPolicy::Replace;
    }
    if (text == "skip") {
        return app::ConflictPolicy::Skip;
    }
    return std::nullopt;
}

std::string contentDisposition(std::string_view type, std::string_view fileName) {
    std::string ascii;
    std::string encoded;
    for (std::size_t i = 0; i < fileName.size();) {
        const auto len = domain::detail::utf8SequenceLength(fileName, i);
        const auto c = static_cast<unsigned char>(fileName[i]);
        if (len <= 1 && c >= 0x20 && c < 0x7F && c != '"' && c != '\\') {
            ascii.push_back(static_cast<char>(c));
        } else {
            ascii.push_back('_');
        }
        const auto step = len == 0 ? 1 : len;
        for (std::size_t k = 0; k < step; ++k) {
            const auto b = static_cast<unsigned char>(fileName[i + k]);
            const bool plain = (b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z') ||
                               (b >= '0' && b <= '9') ||
                               std::string_view("!#$&+-.^_`|~").find(static_cast<char>(b)) !=
                                   std::string_view::npos;
            if (plain) {
                encoded.push_back(static_cast<char>(b));
            } else {
                constexpr char hex[] = "0123456789ABCDEF";
                encoded.push_back('%');
                encoded.push_back(hex[b >> 4]);
                encoded.push_back(hex[b & 0x0F]);
            }
        }
        i += step;
    }
    return std::string(type) + "; filename=\"" + ascii + "\"; filename*=UTF-8''" + encoded;
}

// DTO

std::string_view kindName(domain::Kind kind) noexcept {
    switch (kind) {
    case domain::Kind::Folder:
        return "folder";
    case domain::Kind::File:
        return "file";
    case domain::Kind::Photo:
        return "photo";
    case domain::Kind::Video:
        return "video";
    case domain::Kind::Link:
        return "link";
    }
    return "file";
}

Json toJson(const domain::Entry& entry) {
    Json j{
        {"id", entry.id},
        {"parentId", entry.parentId ? Json(*entry.parentId) : Json(nullptr)},
        {"kind", kindName(entry.meta.kind)},
        {"name", entry.name},
        {"size", entry.meta.size},
        {"mime", entry.meta.mime},
        {"hasThumbnail", entry.hasThumbnail()},
        {"createdAt", entry.meta.createdAt},
        {"modifiedAt", entry.meta.modifiedAt},
        {"sourceModifiedAt",
         entry.meta.sourceModifiedAt ? Json(*entry.meta.sourceModifiedAt) : Json(nullptr)},
        {"description", entry.meta.description},
        {"childCount", entry.childCount},
    };
    Json tags = Json::array();
    for (const auto& tag : entry.meta.tags) {
        tags.push_back(Json{{"tagId", tag.tagId}, {"inherit", tag.inherit}});
    }
    j["tags"] = std::move(tags);
    Json inherited = Json::array();
    for (const auto& tag : entry.inheritedTags) {
        inherited.push_back(Json{{"tagId", tag.tagId}, {"fromId", tag.fromId}});
    }
    j["inheritedTags"] = std::move(inherited);
    if (entry.meta.kind == domain::Kind::Link) {
        j["url"] = entry.meta.url;
        j["domain"] = domain::urlHost(entry.meta.url);
    }
    return j;
}

Json toJson(const std::vector<domain::PathItem>& path) {
    Json items = Json::array();
    for (const auto& item : path) {
        items.push_back(Json{{"id", item.id}, {"name", item.name}});
    }
    return items;
}

Json toJson(const app::FolderListing& listing) {
    Json entries = Json::array();
    for (const auto& entry : listing.entries) {
        entries.push_back(toJson(entry));
    }
    return Json{
        {"parent", listing.parent ? toJson(*listing.parent) : Json(nullptr)},
        {"path", toJson(listing.path)},
        {"entries", std::move(entries)},
    };
}

Json toJson(const app::FolderNode& node) {
    return Json{
        {"id", node.id},
        {"parentId", node.parentId ? Json(*node.parentId) : Json(nullptr)},
        {"name", node.name},
    };
}

Json toJson(const app::MoveConflict& conflict) {
    return Json{{"id", conflict.id}, {"existing", toJson(conflict.existing)}};
}

Json toJson(const app::MoveResult& result) {
    return Json{
        {"moved", result.moved},
        {"replaced", result.replaced},
        {"skipped", result.skipped},
    };
}

Json toJson(const app::SafeInfo& info) {
    return Json{
        {"path", info.path},
        {"entryCount", info.entryCount},
        {"idleRemainingSec", info.idleRemainingSec},
    };
}

Json toJson(const app::PublicStatus& status) {
    return Json{
        {"unlocked", status.unlocked},
        {"lastPath", status.lastPath ? Json(*status.lastPath) : Json(nullptr)},
        {"defaultDirectory", status.defaultDirectory},
    };
}

Json toJson(const domain::SearchHit& hit) {
    Json matchedIn(nullptr);
    switch (hit.matchedIn) {
    case domain::MatchedIn::None:
        break;
    case domain::MatchedIn::Name:
        matchedIn = "name";
        break;
    case domain::MatchedIn::Description:
        matchedIn = "description";
        break;
    }
    return Json{
        {"entry", toJson(hit.entry)},
        {"path", toJson(hit.path)},
        {"matchedIn", std::move(matchedIn)},
    };
}

Json toJson(const app::ImportPlan& plan) {
    Json conflicts = Json::array();
    for (const auto& conflict : plan.conflicts) {
        conflicts.push_back(Json{{"path", conflict.path}, {"existing", toJson(conflict.existing)}});
    }
    return Json{{"conflicts", std::move(conflicts)}, {"newFiles", plan.newFiles}};
}

Json toJson(const domain::ImportResult& result) {
    Json failures = Json::array();
    for (const auto& f : result.failures) {
        failures.push_back(Json{{"path", f.path}, {"message", f.message}});
    }
    return Json{
        {"imported", result.imported}, {"replaced", result.replaced},     {"failed", result.failed},
        {"skipped", result.skipped},   {"failures", std::move(failures)},
    };
}

} // namespace safebox::http
