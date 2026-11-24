// Корзина: корни удаленных поддеревьев с meta.trashedAt. Каталог держит их отдельным списком
// (Catalog::trash), parent_id не меняется - восстановление возвращает запись на место, занятое
// имя дает "имя (2)". Стирание навсегда - removeSubtree и incremental_vacuum, как раньше удаление.
#include <algorithm>
#include <map>
#include <unordered_set>

#include "ordering.hpp"
#include "service_impls.hpp"
#include "vault_session.hpp"

namespace safebox::app {
namespace {

using domain::EntryId;
using domain::Error;
using domain::fail;

class TrashServiceImpl final : public TrashService {
public:
    explicit TrashServiceImpl(Ports ports) : ports_(ports) {}

    Result<TrashListing> list(const Lease& lease, const ListOptions& options) override {
        auto ctx = contextOf(lease);
        if (!ctx) {
            return std::unexpected(ctx.error());
        }
        auto catalog = catalogOf(*ctx, ports_.store);
        if (!catalog) {
            return std::unexpected(catalog.error());
        }
        const Catalog& cat = **catalog;
        std::vector<const Catalog::Node*> nodes;
        nodes.reserve(cat.trash().size());
        for (const auto id : cat.trash()) {
            nodes.push_back(cat.find(id));
        }
        if (options.sort != SortKey::Name || options.descending) { // корзина в каталоге - по имени
            std::sort(nodes.begin(), nodes.end(), [&options](const auto* a, const auto* b) {
                return listedBefore(*a, *b, options);
            });
        }
        TrashListing out;
        out.total = nodes.size();
        const auto [begin, end] = windowOf(nodes.size(), options);
        out.entries.reserve(end - begin);
        for (auto i = begin; i < end; ++i) {
            const auto& parent = nodes[i]->entry.parentId;
            out.entries.push_back({describeEntry(ctx->session, cat, *nodes[i]),
                                   parent ? cat.pathTo(*parent) : std::vector<domain::PathItem>{}});
        }
        return out;
    }

    Result<RestoreResult> restore(const Lease& lease, std::span<const EntryId> ids) override {
        RestoreResult result;
        if (ids.empty()) {
            return result;
        }
        auto ctx = contextOf(lease);
        if (!ctx) {
            return std::unexpected(ctx.error());
        }
        auto catalog = catalogOf(*ctx, ports_.store);
        if (!catalog) {
            return std::unexpected(catalog.error());
        }
        const Catalog& cat = **catalog;

        struct Step {
            EntryId id = 0;
            std::optional<std::string> rename;
        };
        std::vector<Step> steps;
        std::unordered_set<EntryId> seen;
        // занятые имена у каждого родителя: живые дети и уже восстановленные этим запросом
        std::map<std::optional<EntryId>, std::unordered_set<std::string>> taken;
        for (const auto id : ids) {
            if (!seen.insert(id).second) {
                continue;
            }
            const auto* node = cat.find(id);
            if (node == nullptr || !node->entry.meta.trashedAt) {
                return fail(Error::Code::NotFound, "Объекта нет в корзине");
            }
            auto parent = node->entry.parentId;
            if (parent && cat.find(*parent) == nullptr) {
                parent.reset(); // сирота покажется в корне - там и проверяем имя
            }
            if (parent && cat.inTrash(*parent)) {
                return fail(Error::Code::InvalidArgument,
                            "Папка «" + cat.find(*parent)->entry.name +
                                "» в корзине: сначала восстановите ее");
            }
            auto [names, fresh] = taken.try_emplace(parent);
            if (fresh) {
                for (const auto child : cat.childrenOf(parent)) {
                    names->second.insert(cat.find(child)->folded);
                }
            }
            Step step{id};
            if (names->second.contains(node->folded)) {
                step.rename = uniqueName(node->entry.name, node->entry.isFolder(), names->second);
                names->second.insert(foldForSearch(*step.rename));
            } else {
                names->second.insert(node->folded);
            }
            steps.push_back(std::move(step));
        }

        {
            auto uow = ports_.store.begin();
            if (!uow) {
                return std::unexpected(uow.error());
            }
            for (const auto& step : steps) {
                const auto* rename = step.rename ? &*step.rename : nullptr;
                if (auto st =
                        markTrashed((*uow)->entries(), *ctx->sealer, step.id, std::nullopt, rename);
                    !st) {
                    return std::unexpected(st.error());
                }
            }
            if (auto st = (*uow)->commit(); !st) {
                return std::unexpected(st.error());
            }
        }
        CatalogChanges changed;
        for (auto& step : steps) {
            changed.entries.push_back(step.id);
            if (step.rename) {
                result.renamed.push_back({step.id, std::move(*step.rename)});
            }
        }
        result.restored = steps.size();
        ctx->session.catalogChanged(changed);
        return result;
    }

    Result<std::size_t> purge(const Lease& lease, const PurgeCmd& cmd) override {
        const int modes = (cmd.ids.empty() ? 0 : 1) + (cmd.before ? 1 : 0) + (cmd.all ? 1 : 0);
        if (modes != 1) {
            return fail(Error::Code::InvalidArgument, "Укажите одно: ids, before или all");
        }
        auto ctx = contextOf(lease);
        if (!ctx) {
            return std::unexpected(ctx.error());
        }
        auto catalog = catalogOf(*ctx, ports_.store);
        if (!catalog) {
            return std::unexpected(catalog.error());
        }
        const Catalog& cat = **catalog;
        std::vector<EntryId> targets;
        if (!cmd.ids.empty()) {
            std::unordered_set<EntryId> seen;
            for (const auto id : cmd.ids) {
                if (!seen.insert(id).second) {
                    continue;
                }
                if (cat.find(id) == nullptr || !cat.inTrash(id)) {
                    return fail(Error::Code::NotFound, "Объекта нет в корзине");
                }
                targets.push_back(id);
            }
        } else {
            for (const auto id : cat.trash()) {
                if (cmd.all || *cat.find(id)->entry.meta.trashedAt < *cmd.before) {
                    targets.push_back(id);
                }
            }
        }
        if (targets.empty()) {
            return std::size_t{0};
        }

        std::unordered_set<EntryId> removed;
        {
            auto uow = ports_.store.begin();
            if (!uow) {
                return std::unexpected(uow.error());
            }
            for (const auto id : targets) {
                if (removed.contains(id)) {
                    continue; // ушла вместе с выбранным предком
                }
                auto gone = (*uow)->entries().removeSubtree(id);
                if (!gone) {
                    return std::unexpected(gone.error());
                }
                removed.insert(gone->entries.begin(), gone->entries.end());
            }
            if (auto st = (*uow)->commit(); !st) {
                return std::unexpected(st.error());
            }
        }
        ctx->session.catalogChanged({.entries = std::move(targets)});
        (void)ports_.store.compact(); // место вернется файлу; сбой здесь не отменяет стирание
        return removed.size();
    }

private:
    Ports ports_;
};

} // namespace

std::shared_ptr<TrashService> makeTrashService(Ports ports) {
    return std::make_shared<TrashServiceImpl>(ports);
}

} // namespace safebox::app
