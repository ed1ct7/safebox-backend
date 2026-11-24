// Сервисы с заранее заданными ответами для тестов http-слоя
#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "safebox/app/services.hpp"

namespace safebox::test {

inline constexpr std::string_view kApiToken = "api-token";
inline constexpr std::string_view kMediaToken = "media-token";

class FakeSafeService final : public app::SafeService {
public:
    app::Result<app::UnlockResult> create(const app::CreateSafeCmd& cmd) override {
        lastPath = cmd.path;
        if (createError) {
            return std::unexpected(*createError);
        }
        return session();
    }

    app::Result<app::UnlockResult> unlock(const app::UnlockCmd& cmd) override {
        lastPath = cmd.path;
        if (cmd.password == "wrong") {
            return domain::fail(domain::Error::Code::WrongPassword, "Неверный пароль");
        }
        if (cmd.path == "not-a-safe") {
            return domain::fail(domain::Error::Code::NotASafe, "Файл не является сейфом SafeBox");
        }
        return session();
    }

    void lock() noexcept override {
        ++lockCalls;
        unlocked = false;
    }

    app::PublicStatus publicStatus() override {
        return {unlocked, "C:/safes/my.safebox", "C:/docs"};
    }

    app::Result<app::SafeInfo> info(const app::Lease&) override {
        return app::SafeInfo{"C:/safes/my.safebox", 3, 900};
    }

    app::Status changePassword(const app::Lease&, const app::ChangePasswordCmd& cmd) override {
        if (cmd.oldPassword == "wrong") {
            return domain::fail(domain::Error::Code::WrongPassword, "Неверный пароль");
        }
        return {};
    }

    app::Result<app::Lease> authorize(std::string_view token, app::TokenScope scope) override {
        const bool ok = unlocked && ((scope == app::TokenScope::Api && token == kApiToken) ||
                                     (scope == app::TokenScope::Media && token == kMediaToken));
        if (!ok) {
            return domain::fail(domain::Error::Code::Locked, "Сейф заблокирован");
        }
        ++authorizations;
        return app::Lease(nullptr);
    }

    app::Result<app::HeartbeatResult> heartbeat(const app::Lease&, bool active) override {
        lastHeartbeatActive = active;
        return app::HeartbeatResult{active ? 900 : 600};
    }

    void tick() noexcept override {}

    bool unlocked = true;
    int lockCalls = 0;
    int authorizations = 0;
    std::optional<bool> lastHeartbeatActive;
    std::optional<domain::Error> createError;
    std::string lastPath;

private:
    static app::UnlockResult session() {
        return {{std::string(kApiToken), std::string(kMediaToken)},
                app::SafeInfo{"C:/safes/my.safebox", 0, 900}};
    }
};

inline domain::Entry makeEntry(domain::EntryId id, std::optional<domain::EntryId> parent,
                               domain::Kind kind, std::string name) {
    domain::Entry e;
    e.id = id;
    e.parentId = parent;
    e.name = std::move(name);
    e.meta.kind = kind;
    e.meta.mime = kind == domain::Kind::Photo ? "image/jpeg" : "application/octet-stream";
    e.meta.size = 11;
    e.meta.createdAt = e.meta.modifiedAt = 1'790'000'000'000;
    return e;
}

class FakeEntriesService final : public app::EntriesService {
public:
    FakeEntriesService() {
        entries[1] = makeEntry(1, std::nullopt, domain::Kind::Folder, "Отпуск");
        entries[2] = makeEntry(2, 1, domain::Kind::Photo, "море.jpg");
        entries[3] = makeEntry(3, std::nullopt, domain::Kind::Link, "site.url");
        entries[3].meta.url = "https://Example.com:8443/page";
        entries[4] = makeEntry(4, std::nullopt, domain::Kind::File, "<script>.html");
        entries[2].meta.description = "закат на пляже";
        entries[2].meta.sourceModifiedAt = 1'700'000'000'000;
        entries[2].meta.tags = {{7, true}, {9, false}};
        entries[2].inheritedTags = {{5, 1}};
        entries[1].childCount = 1;
    }

    using app::EntriesService::list;

    app::Result<app::FolderListing> list(const app::Lease&, std::optional<domain::EntryId> parent,
                                         const app::ListOptions& options) override {
        lastListOptions = options;
        app::FolderListing out;
        if (parent) {
            auto it = entries.find(*parent);
            if (it == entries.end()) {
                return domain::fail(domain::Error::Code::NotFound, "Объект не найден");
            }
            out.parent = it->second;
            out.path = {{it->second.id, it->second.name}};
        }
        for (const auto& [id, e] : entries) {
            if (e.parentId == parent) {
                out.entries.push_back(e);
            }
        }
        out.total = out.entries.size();
        return out;
    }

    app::Result<domain::Entry> get(const app::Lease&, domain::EntryId id) override {
        if (getError) {
            return std::unexpected(*getError);
        }
        auto it = entries.find(id);
        if (it == entries.end()) {
            return domain::fail(domain::Error::Code::NotFound, "Объект не найден");
        }
        return it->second;
    }

    app::Result<std::vector<app::FolderNode>> folders(const app::Lease&) override {
        return std::vector<app::FolderNode>{{1, std::nullopt, "Отпуск"}};
    }

    app::Result<domain::Entry> update(const app::Lease&, domain::EntryId id,
                                      const app::UpdateEntryCmd& cmd) override {
        if (updateError) {
            return std::unexpected(*updateError);
        }
        auto it = entries.find(id);
        if (it == entries.end()) {
            return domain::fail(domain::Error::Code::NotFound, "Объект не найден");
        }
        lastUpdate = cmd;
        if (cmd.name) {
            it->second.name = *cmd.name;
        }
        if (cmd.description) {
            it->second.meta.description = *cmd.description;
        }
        if (cmd.url) {
            it->second.meta.url = *cmd.url;
        }
        if (cmd.source) {
            it->second.meta.source = *cmd.source;
        }
        if (cmd.sourceUrl) {
            it->second.meta.sourceUrl = *cmd.sourceUrl;
        }
        if (cmd.sourceDate) {
            it->second.meta.sourceDate = *cmd.sourceDate;
        }
        return it->second;
    }

    app::Result<std::vector<app::MoveConflict>>
    planMove(const app::Lease&, std::span<const domain::EntryId> ids,
             std::optional<domain::EntryId> parent) override {
        if (moveError) {
            return std::unexpected(*moveError);
        }
        lastPlanIds.assign(ids.begin(), ids.end());
        lastPlanParent = parent;
        return conflicts;
    }

    app::Result<app::MoveResult> move(const app::Lease&, const app::MoveCmd& cmd) override {
        if (moveError) {
            return std::unexpected(*moveError);
        }
        lastMove = cmd;
        return moveResult;
    }

    app::Result<std::size_t> remove(const app::Lease&,
                                    std::span<const domain::EntryId> ids) override {
        removed.assign(ids.begin(), ids.end());
        return ids.size();
    }

    app::Result<std::vector<app::HashMatch>>
    lookup(const app::Lease&, std::span<const domain::Sha256> hashes) override {
        lastLookup.assign(hashes.begin(), hashes.end());
        std::vector<app::HashMatch> found;
        for (const auto& hash : hashes) {
            if (const auto it = known.find(hash); it != known.end()) {
                found.push_back({hash, it->second});
            }
        }
        return found;
    }

    std::map<domain::EntryId, domain::Entry> entries;
    std::optional<domain::Error> getError;
    std::optional<domain::Error> updateError;
    std::optional<domain::Error> moveError;
    std::vector<domain::EntryId> removed;
    std::vector<domain::Sha256> lastLookup;
    std::map<domain::Sha256, domain::EntryId> known; // для lookup
    std::optional<app::UpdateEntryCmd> lastUpdate;
    std::vector<domain::EntryId> lastPlanIds;
    std::optional<domain::EntryId> lastPlanParent;
    app::ListOptions lastListOptions;
    std::optional<app::MoveCmd> lastMove;
    std::vector<app::MoveConflict> conflicts;
    app::MoveResult moveResult{1, 0, 0};
};

class MemoryContentStream final : public app::ContentStream {
public:
    explicit MemoryContentStream(std::string data) : data_(std::move(data)) {}

    std::uint64_t size() const noexcept override { return data_.size(); }

    app::Status read(std::uint64_t offset, std::uint64_t length, domain::ByteSink& out) override {
        return out.write(domain::asBytes(std::string_view(data_).substr(offset, length)));
    }

private:
    std::string data_;
};

class FakeImportSession final : public app::ImportSession {
public:
    FakeImportSession(std::map<std::string, std::string>& files,
                      std::map<std::string, app::ImportFileOptions>& options)
        : files_(files), options_(options) {}

    app::Status beginFile(std::string_view path, const app::ImportFileOptions& options) override {
        current_ = std::string(path);
        files_[current_];
        options_[current_] = options;
        return {};
    }
    app::Status write(std::span<const std::byte> data) override {
        files_[current_].append(domain::asChars(data));
        return {};
    }
    app::Status endFile() override { return {}; }
    domain::ImportResult finish() override {
        domain::ImportResult r;
        r.imported = files_.size();
        return r;
    }

private:
    std::map<std::string, std::string>& files_;
    std::map<std::string, app::ImportFileOptions>& options_;
    std::string current_;
};

class FakeImportExportService final : public app::ImportExportService {
public:
    std::size_t activeImports() const override { return 0; }

    app::Result<std::unique_ptr<app::ImportSession>>
    beginImport(app::Lease, std::optional<domain::EntryId> parent) override {
        importParent = parent;
        return std::unique_ptr<app::ImportSession>(
            std::make_unique<FakeImportSession>(imported, importOptions));
    }

    app::Result<app::ImportPlan> planImport(const app::Lease&,
                                            std::optional<domain::EntryId> parent,
                                            std::span<const app::ImportPlanFile> files) override {
        if (planError) {
            return std::unexpected(*planError);
        }
        planParent = parent;
        planFiles.assign(files.begin(), files.end());
        return plan;
    }

    app::Result<app::OpenedContent> openContent(app::Lease, domain::EntryId id,
                                                app::ContentVariant variant) override {
        if (id == 404) {
            return domain::fail(domain::Error::Code::NotFound, "Объект не найден");
        }
        app::OpenedContent out;
        out.entry = makeEntry(id, std::nullopt, id == 4 ? domain::Kind::File : domain::Kind::Photo,
                              id == 4 ? "<script>.html" : "море.jpg");
        out.mime = variant == app::ContentVariant::Thumbnail ? "image/jpeg"
                   : id == 4                                 ? "text/html"
                                                             : "image/jpeg";
        out.stream = std::make_unique<MemoryContentStream>(content);
        return out;
    }

    app::Status exportZip(const app::Lease&, domain::EntryId id, domain::ByteSink& out) override {
        lastZipId = id;
        return out.write(domain::asBytes("PK-fake-zip"));
    }

    app::Result<domain::Entry> setThumbnail(const app::Lease&, domain::EntryId id,
                                            std::span<const std::byte> image,
                                            const app::MediaInfo& info) override {
        if (id == 404) {
            return domain::fail(domain::Error::Code::NotFound, "Объект не найден");
        }
        lastThumb.assign(reinterpret_cast<const char*>(image.data()), image.size());
        lastMediaInfo = info;
        if (lastThumb == "not an image") {
            return domain::fail(domain::Error::Code::InvalidArgument, "Не удалось прочитать");
        }
        auto entry = makeEntry(id, std::nullopt, domain::Kind::Video, "clip.mp4");
        entry.meta.dimensions = info.dimensions;
        entry.meta.durationMs = info.durationMs;
        return entry;
    }

    std::string content = "0123456789ABCDEF";
    std::map<std::string, std::string> imported;
    std::map<std::string, app::ImportFileOptions> importOptions; // что дошло в beginFile
    std::optional<domain::EntryId> importParent;
    app::ImportPlan plan;
    std::optional<domain::Error> planError;
    std::optional<domain::EntryId> planParent;
    std::optional<domain::EntryId> lastZipId;
    std::vector<app::ImportPlanFile> planFiles;
    std::string lastThumb;
    std::optional<app::MediaInfo> lastMediaInfo;
};

class FakeSearchService final : public app::SearchService {
public:
    app::Result<app::SearchResults> search(const app::Lease&,
                                           const app::SearchQuery& query) override {
        lastQuery = query;
        if (error) {
            return std::unexpected(*error);
        }
        domain::SearchHit hit;
        hit.entry = makeEntry(2, 1, domain::Kind::Photo, "море.jpg");
        hit.path = {{1, "Отпуск"}};
        hit.matchedIn = matchedIn;
        return app::SearchResults{{hit}, 1};
    }

    app::Result<std::vector<app::SourceCount>> sources(const app::Lease&) override {
        if (error) {
            return std::unexpected(*error);
        }
        return std::vector<app::SourceCount>{{"telegram", 3}, {"YouTube", 1}};
    }

    domain::MatchedIn matchedIn = domain::MatchedIn::None;
    std::optional<domain::Error> error;
    app::SearchQuery lastQuery;
};

// Отвечает заготовками и запоминает аргументы; error проваливает любую операцию.
class FakeTagsService final : public app::TagsService {
public:
    FakeTagsService() {
        categories = {{{1, "Люди"}, {{{10, 1, "Ирис"}, 2}, {{11, 1, "Рокси"}, 0}}},
                      {{2, "Язык"}, {}}};
    }

    app::Result<std::vector<app::CategoryWithTags>> list(const app::Lease&) override {
        if (error) {
            return std::unexpected(*error);
        }
        return categories;
    }

    app::Result<domain::TagCategory> createCategory(const app::Lease&, std::string_view name,
                                                    std::string_view nameEn) override {
        if (error) {
            return std::unexpected(*error);
        }
        lastName = std::string(name);
        return domain::TagCategory{3, std::string(name), std::string(nameEn)};
    }

    app::Result<app::CategoryWithTags> renameCategory(const app::Lease&, domain::CategoryId id,
                                                      const app::RenameCategoryCmd& cmd) override {
        const auto name = cmd.name.value_or("");
        if (error) {
            return std::unexpected(*error);
        }
        lastId = id;
        lastName = std::string(name);
        return app::CategoryWithTags{{id, std::string(name)}, categories[0].tags};
    }

    app::Result<app::RemovedTags> removeCategory(const app::Lease&,
                                                 domain::CategoryId id) override {
        if (error) {
            return std::unexpected(*error);
        }
        lastId = id;
        return app::RemovedTags{4, 7};
    }

    app::Result<app::CreatedTag> createTag(const app::Lease&,
                                           const app::CreateTagCmd& cmd) override {
        if (error) {
            return std::unexpected(*error);
        }
        lastCreate = cmd;
        return app::CreatedTag{domain::Tag{12, 1, cmd.name}, tagCreated};
    }

    app::Result<domain::Tag> updateTag(const app::Lease&, domain::TagId id,
                                       const app::UpdateTagCmd& cmd) override {
        if (error) {
            return std::unexpected(*error);
        }
        lastId = id;
        lastUpdate = cmd;
        return domain::Tag{id, cmd.categoryId.value_or(1), cmd.name.value_or("Ирис")};
    }

    app::Result<std::size_t> mergeTag(const app::Lease&, domain::TagId from,
                                      domain::TagId into) override {
        if (error) {
            return std::unexpected(*error);
        }
        lastId = from;
        lastInto = into;
        return 5;
    }

    app::Result<std::size_t> removeTag(const app::Lease&, domain::TagId id) override {
        if (error) {
            return std::unexpected(*error);
        }
        lastId = id;
        return 6;
    }

    app::Result<std::size_t> assign(const app::Lease&, const app::AssignTagsCmd& cmd) override {
        if (error) {
            return std::unexpected(*error);
        }
        lastAssign = cmd;
        return 3;
    }

    std::vector<app::CategoryWithTags> categories;
    std::optional<domain::Error> error;
    bool tagCreated = true;
    std::string lastName;
    std::int64_t lastId = 0;
    std::int64_t lastInto = 0;
    std::optional<app::CreateTagCmd> lastCreate;
    std::optional<app::UpdateTagCmd> lastUpdate;
    std::optional<app::AssignTagsCmd> lastAssign;
};

// Отвечает заготовками и запоминает аргументы; error проваливает любую операцию.
class FakeLinksService final : public app::LinksService {
public:
    app::Result<app::CreateLinksResult> create(const app::Lease&,
                                               const app::CreateLinksCmd& cmd) override {
        if (error) {
            return std::unexpected(*error);
        }
        lastCreate = cmd;
        return createResult;
    }

    app::Result<domain::Entry> refreshPreview(const app::Lease&, domain::EntryId id) override {
        if (error) {
            return std::unexpected(*error);
        }
        lastPreviewId = id;
        return refreshed;
    }

    bool previewPending(const app::Lease&, domain::EntryId) const override { return false; }
    void drain() override {}

    app::CreateLinksResult createResult;
    domain::Entry refreshed = makeEntry(3, std::nullopt, domain::Kind::Link, "Example");
    std::optional<domain::Error> error;
    std::optional<app::CreateLinksCmd> lastCreate;
    domain::EntryId lastPreviewId = 0;
};

class FakeSettingsService final : public app::SettingsService {
public:
    domain::AppSettings get() override { return settings; }

    app::Result<domain::AppSettings> update(const domain::AppSettings& next) override {
        if (error) {
            return std::unexpected(*error);
        }
        ++updates;
        settings = next;
        return settings;
    }

    domain::AppSettings settings;
    std::optional<domain::Error> error;
    int updates = 0;
};

class FakeContentHashService final : public app::ContentHashService {
public:
    app::HashProgress progress(const app::Lease&) const override { return value; }
    void drain() override {}

    app::HashProgress value{.pending = 12, .failed = 1};
};

class FakeBackupService final : public app::BackupService {
public:
    app::Result<app::BackupProgress> start(const app::Lease&, std::string_view path) override {
        lastPath = std::string(path);
        if (error) {
            return std::unexpected(*error);
        }
        value = {.state = app::BackupState::Running,
                 .path = domain::pathFromUtf8(lastPath),
                 .bytesDone = 0,
                 .bytesTotal = 4096};
        return value;
    }
    app::BackupProgress progress(const app::Lease&) const override { return value; }
    void drain() override {}

    std::string lastPath;
    std::optional<domain::Error> error;
    app::BackupProgress value;
};

class FakeTrashService final : public app::TrashService {
public:
    app::Result<app::TrashListing> list(const app::Lease&,
                                        const app::ListOptions& options) override {
        lastListOptions = options;
        if (error) {
            return std::unexpected(*error);
        }
        return listing;
    }
    app::Result<app::RestoreResult> restore(const app::Lease&,
                                            std::span<const domain::EntryId> ids) override {
        lastIds.assign(ids.begin(), ids.end());
        if (error) {
            return std::unexpected(*error);
        }
        return restored;
    }
    app::Result<std::size_t> purge(const app::Lease&, const app::PurgeCmd& cmd) override {
        lastPurge = cmd;
        if (error) {
            return std::unexpected(*error);
        }
        return purged;
    }

    std::optional<domain::Error> error;
    app::TrashListing listing;
    app::RestoreResult restored;
    std::size_t purged = 0;
    app::ListOptions lastListOptions;
    std::vector<domain::EntryId> lastIds;
    app::PurgeCmd lastPurge;
};

struct FakeServices {
    std::shared_ptr<FakeSafeService> safe = std::make_shared<FakeSafeService>();
    std::shared_ptr<FakeEntriesService> entries = std::make_shared<FakeEntriesService>();
    std::shared_ptr<FakeImportExportService> importExport =
        std::make_shared<FakeImportExportService>();
    std::shared_ptr<FakeSearchService> search = std::make_shared<FakeSearchService>();
    std::shared_ptr<FakeTagsService> tags = std::make_shared<FakeTagsService>();
    std::shared_ptr<FakeLinksService> links = std::make_shared<FakeLinksService>();
    std::shared_ptr<FakeSettingsService> settings = std::make_shared<FakeSettingsService>();
    std::shared_ptr<FakeTrashService> trash = std::make_shared<FakeTrashService>();
    std::shared_ptr<FakeBackupService> backup = std::make_shared<FakeBackupService>();
    std::shared_ptr<FakeContentHashService> hashes = std::make_shared<FakeContentHashService>();

    [[nodiscard]] app::Services services() const {
        return {safe, entries, importExport, search, tags, links, settings, trash, backup, hashes};
    }
};

} // namespace safebox::test
