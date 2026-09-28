// EntriesService: список папки, дерево папок, переименование, удаление.
// после удаления делаем incremental_vacuum, чтобы файл уменьшился
#include <unordered_set>

#include "safebox/domain/model/rules.hpp"
#include "service_impls.hpp"
#include "vault_session.hpp"

namespace safebox::app {
namespace {

using domain::EntryId;
using domain::Error;
using domain::fail;

class EntriesServiceImpl final : public EntriesService {
public:
    explicit EntriesServiceImpl(Ports ports) : ports_(ports) {}

    Result<FolderListing> list(const Lease& lease, std::optional<EntryId> folder) override {
        auto ctx = contextOf(lease);
        if (!ctx) {
            return std::unexpected(ctx.error());
        }
        auto catalog = catalogOf(*ctx, ports_.store);
        if (!catalog) {
            return std::unexpected(catalog.error());
        }
        const Catalog& cat = **catalog;
        FolderListing out;
        if (folder) {
            const auto* node = cat.find(*folder);
            if (node == nullptr) {
                return fail(Error::Code::NotFound, "Папка не найдена");
            }
            if (!node->entry.isFolder()) {
                return fail(Error::Code::InvalidArgument, "Это не папка");
            }
            out.folder = node->entry;
            out.path = cat.pathTo(*folder);
        }
        const auto& ids = cat.childrenOf(folder);
        out.entries.reserve(ids.size());
        for (const auto id : ids) {
            out.entries.push_back(cat.find(id)->entry);
        }
        return out;
    }

    Result<domain::Entry> get(const Lease& lease, EntryId id) override {
        auto ctx = contextOf(lease);
        if (!ctx) {
            return std::unexpected(ctx.error());
        }
        auto catalog = catalogOf(*ctx, ports_.store);
        if (!catalog) {
            return std::unexpected(catalog.error());
        }
        const auto* node = (*catalog)->find(id);
        if (node == nullptr) {
            return fail(Error::Code::NotFound, "Объект не найден");
        }
        return node->entry;
    }

    Result<std::vector<FolderNode>> folders(const Lease& lease) override {
        auto ctx = contextOf(lease);
        if (!ctx) {
            return std::unexpected(ctx.error());
        }
        auto catalog = catalogOf(*ctx, ports_.store);
        if (!catalog) {
            return std::unexpected(catalog.error());
        }
        const Catalog& cat = **catalog;
        std::vector<FolderNode> out;
        std::unordered_set<EntryId> seen;
        std::vector<EntryId> stack;
        const auto pushFolders = [&](const std::vector<EntryId>& ids) {
            for (auto it = ids.rbegin(); it != ids.rend(); ++it) { // первый сосед - первым
                if (cat.find(*it)->entry.isFolder()) {
                    stack.push_back(*it);
                }
            }
        };
        pushFolders(cat.childrenOf(std::nullopt));
        while (!stack.empty()) {
            const auto id = stack.back();
            stack.pop_back();
            if (!seen.insert(id).second) {
                continue;
            }
            const auto& entry = cat.find(id)->entry;
            out.push_back(FolderNode{entry.id, entry.parentId, entry.name});
            pushFolders(cat.childrenOf(id));
        }
        return out;
    }

    Result<domain::Entry> rename(const Lease& lease, EntryId id, std::string_view name) override {
        const std::string clean(domain::detail::trim(name));
        if (auto st = domain::validateName(clean); !st) {
            return std::unexpected(st.error());
        }
        auto ctx = contextOf(lease);
        if (!ctx) {
            return std::unexpected(ctx.error());
        }
        const Sealer& sealer = *ctx->sealer;
        domain::Entry entry;
        {
            auto uow = ports_.store.begin();
            if (!uow) {
                return std::unexpected(uow.error());
            }
            auto record = (*uow)->entries().get(id);
            if (!record) {
                return std::unexpected(record.error());
            }
            auto opened = sealer.openEntry(*record);
            if (!opened) {
                return std::unexpected(opened.error());
            }
            entry = std::move(*opened);
            entry.name = clean;
            entry.meta.modifiedAt = domain::toUnixMillis(ports_.clock.now());
            auto encName = sealer.sealName(id, entry.name);
            if (!encName) {
                return std::unexpected(encName.error());
            }
            auto encMeta = sealer.sealMeta(id, entry.meta);
            if (!encMeta) {
                return std::unexpected(encMeta.error());
            }
            if (auto st = (*uow)->entries().updateSealed(id, *encName, *encMeta); !st) {
                return std::unexpected(st.error());
            }
            if (auto st = (*uow)->commit(); !st) {
                return std::unexpected(st.error());
            }
        }
        ctx->session.invalidateCatalog();
        return entry;
    }

    Result<std::size_t> remove(const Lease& lease, std::span<const EntryId> ids) override {
        if (ids.empty()) {
            return std::size_t{0};
        }
        auto ctx = contextOf(lease);
        if (!ctx) {
            return std::unexpected(ctx.error());
        }
        std::unordered_set<EntryId> removed;
        {
            auto uow = ports_.store.begin();
            if (!uow) {
                return std::unexpected(uow.error());
            }
            for (const auto id : ids) {
                if (removed.contains(id)) {
                    continue; // уже ушла вместе с выделенной папкой-предком
                }
                auto result = (*uow)->entries().removeSubtree(id);
                if (!result) {
                    return std::unexpected(result.error());
                }
                removed.insert(result->entries.begin(), result->entries.end());
            }
            if (auto st = (*uow)->commit(); !st) {
                return std::unexpected(st.error());
            }
        }
        ctx->session.invalidateCatalog();
        (void)ports_.store.compact(); // место вернется файлу; сбой здесь не отменяет удаление
        return removed.size();
    }

private:
    Ports ports_;
};

} // namespace

std::shared_ptr<EntriesService> makeEntriesService(Ports ports) {
    return std::make_shared<EntriesServiceImpl>(ports);
}

} // namespace safebox::app
