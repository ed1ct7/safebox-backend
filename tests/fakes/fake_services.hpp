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

    app::Result<app::FolderListing> list(const app::Lease&,
                                         std::optional<domain::EntryId> parent) override {
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

    std::map<domain::EntryId, domain::Entry> entries;
    std::optional<domain::Error> getError;
    std::optional<domain::Error> updateError;
    std::optional<domain::Error> moveError;
    std::vector<domain::EntryId> removed;
    std::optional<app::UpdateEntryCmd> lastUpdate;
    std::vector<domain::EntryId> lastPlanIds;
    std::optional<domain::EntryId> lastPlanParent;
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
    explicit FakeImportSession(std::map<std::string, std::string>& files) : files_(files) {}

    app::Status beginFile(std::string_view path) override {
        current_ = std::string(path);
        files_[current_];
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
    std::string current_;
};

class FakeImportExportService final : public app::ImportExportService {
public:
    app::Result<std::unique_ptr<app::ImportSession>>
    beginImport(app::Lease, std::optional<domain::EntryId> parent) override {
        importParent = parent;
        return std::unique_ptr<app::ImportSession>(std::make_unique<FakeImportSession>(imported));
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

    app::Status exportZip(const app::Lease&, domain::EntryId, domain::ByteSink& out) override {
        return out.write(domain::asBytes("PK-fake-zip"));
    }

    std::string content = "0123456789ABCDEF";
    std::map<std::string, std::string> imported;
    std::optional<domain::EntryId> importParent;
};

class FakeSearchService final : public app::SearchService {
public:
    app::Result<std::vector<domain::SearchHit>> search(const app::Lease&, std::string_view query,
                                                       std::size_t limit) override {
        lastQuery = std::string(query);
        lastLimit = limit;
        domain::SearchHit hit;
        hit.entry = makeEntry(2, 1, domain::Kind::Photo, "море.jpg");
        hit.path = {{1, "Отпуск"}};
        hit.matchedIn = matchedIn;
        return std::vector<domain::SearchHit>{hit};
    }

    domain::MatchedIn matchedIn = domain::MatchedIn::None;
    std::string lastQuery;
    std::size_t lastLimit = 0;
};

struct FakeServices {
    std::shared_ptr<FakeSafeService> safe = std::make_shared<FakeSafeService>();
    std::shared_ptr<FakeEntriesService> entries = std::make_shared<FakeEntriesService>();
    std::shared_ptr<FakeImportExportService> importExport =
        std::make_shared<FakeImportExportService>();
    std::shared_ptr<FakeSearchService> search = std::make_shared<FakeSearchService>();

    [[nodiscard]] app::Services services() const { return {safe, entries, importExport, search}; }
};

} // namespace safebox::test
