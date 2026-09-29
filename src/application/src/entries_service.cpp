// EntriesService: список любой записи, дерево папок, правка полей, перенос, удаление.
// Перенос: сначала весь план по снимку каталога (ошибка -> ничего не изменено), потом одна
// транзакция. Замененные записи удаляются после переноса: перенесенная запись, лежавшая внутри
// замененной, к этому моменту уже вне ее поддерева.
// после удаления делаем incremental_vacuum, чтобы файл уменьшился
#include <unordered_map>
#include <unordered_set>

#include "safebox/domain/model/rules.hpp"
#include "service_impls.hpp"
#include "vault_session.hpp"

namespace safebox::app {
namespace {

using domain::EntryId;
using domain::Error;
using domain::fail;

struct MoveStep {
    EntryId id = 0;
    bool skip = false;                 // Skip: запись остается на месте
    std::optional<std::string> rename; // KeepBoth: уникальное имя у нового родителя
    std::optional<EntryId> replaces;   // Replace: занявшая имя запись удаляется
};

struct MovePlan {
    std::vector<MoveStep> steps; // только записи, у которых меняется родитель
    std::vector<MoveConflict> conflicts;
};

// Все проверки и разрешение конфликтов без записи в хранилище.
Result<MovePlan> planMoveOn(const VaultSession& session, const Catalog& cat,
                            std::span<const EntryId> ids, std::optional<EntryId> parent,
                            const std::unordered_map<EntryId, ConflictPolicy>& resolutions) {
    if (parent && cat.find(*parent) == nullptr) {
        return fail(Error::Code::NotFound, "Объект назначения не найден");
    }
    std::vector<const Catalog::Node*> nodes;
    std::unordered_set<EntryId> seen;
    for (const auto id : ids) {
        if (!seen.insert(id).second) {
            continue;
        }
        const auto* node = cat.find(id);
        if (node == nullptr) {
            return fail(Error::Code::NotFound, "Объект не найден");
        }
        if (parent && cat.isAncestorOrSelf(id, *parent)) {
            return fail(Error::Code::InvalidArgument,
                        "Нельзя переместить объект в самого себя или в его содержимое");
        }
        nodes.push_back(node);
    }

    std::unordered_map<std::string, EntryId> occupant; // folded имя -> запись у нового родителя
    std::unordered_set<std::string> taken;             // занятые имена, растет по ходу плана
    for (const auto child : cat.childrenOf(parent)) {
        const auto& folded = cat.find(child)->folded;
        occupant.try_emplace(folded, child);
        taken.insert(folded);
    }
    std::unordered_set<std::string> placed; // имена, занятые перенесенными в этом запросе

    MovePlan plan;
    for (const auto* node : nodes) {
        const auto& entry = node->entry;
        if (entry.parentId == parent) {
            continue; // уже на месте - не конфликт
        }
        MoveStep step{entry.id};
        // Вторая запись с тем же именем в одном запросе - всегда KeepBoth: конфликтовать ей
        // не с чем, кроме только что перенесенной.
        const bool clash = placed.contains(node->folded);
        const auto found = occupant.find(node->folded);
        auto policy = ConflictPolicy::KeepBoth;
        if (!clash && found != occupant.end()) {
            plan.conflicts.push_back(
                {entry.id, describeEntry(session, cat, *cat.find(found->second))});
            if (const auto chosen = resolutions.find(entry.id); chosen != resolutions.end()) {
                policy = chosen->second;
            }
            if (policy == ConflictPolicy::Replace) {
                step.replaces = found->second;
            }
        }
        if (policy == ConflictPolicy::Skip) {
            step.skip = true;
        } else {
            if (policy == ConflictPolicy::KeepBoth && (clash || found != occupant.end())) {
                step.rename = uniqueName(entry.name, entry.isFolder(), taken);
            }
            const auto folded = foldForSearch(step.rename ? *step.rename : entry.name);
            taken.insert(folded);
            placed.insert(folded);
        }
        plan.steps.push_back(std::move(step));
    }
    return plan;
}

class EntriesServiceImpl final : public EntriesService {
public:
    explicit EntriesServiceImpl(Ports ports) : ports_(ports) {}

    Result<FolderListing> list(const Lease& lease, std::optional<EntryId> parent) override {
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
        if (parent) {
            const auto* node = cat.find(*parent);
            if (node == nullptr) {
                return fail(Error::Code::NotFound, "Объект не найден");
            }
            out.parent = describeEntry(ctx->session, cat, *node);
            out.path = cat.pathTo(*parent);
        }
        const auto& ids = cat.childrenOf(parent);
        out.entries.reserve(ids.size());
        for (const auto id : ids) {
            out.entries.push_back(describeEntry(ctx->session, cat, *cat.find(id)));
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
        return describeEntry(ctx->session, **catalog, *node);
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
        // Только папки, и вниз только через папки: папка внутри файла в дерево не попадает.
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

    Result<domain::Entry> update(const Lease& lease, EntryId id,
                                 const UpdateEntryCmd& cmd) override {
        if (!cmd.name && !cmd.description && !cmd.url) {
            return fail(Error::Code::InvalidArgument, "Не указано, что менять");
        }
        std::string name;
        if (cmd.name) {
            name = std::string(domain::detail::trim(*cmd.name));
            if (auto st = domain::validateName(name); !st) {
                return std::unexpected(st.error());
            }
        }
        if (cmd.description) {
            if (cmd.description->size() > domain::kMaxDescriptionBytes) {
                return fail(Error::Code::InvalidArgument, "Описание длиннее 64 КиБ");
            }
            if (!domain::isValidUtf8(*cmd.description)) {
                return fail(Error::Code::InvalidArgument, "Описание содержит некорректные символы");
            }
        }
        std::string url;
        if (cmd.url) {
            url = std::string(domain::detail::trim(*cmd.url));
            if (!domain::isHttpUrl(url)) {
                return fail(Error::Code::InvalidArgument,
                            "Адрес должен начинаться с http:// или https://");
            }
        }
        auto ctx = contextOf(lease);
        if (!ctx) {
            return std::unexpected(ctx.error());
        }
        auto catalog = catalogOf(*ctx, ports_.store);
        if (!catalog) {
            return std::unexpected(catalog.error());
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
            if (cmd.url && entry.meta.kind != domain::Kind::Link) {
                return fail(Error::Code::InvalidArgument, "Адрес задается только у ссылки");
            }
            if (cmd.name) {
                entry.name = std::move(name);
                entry.meta.nameByUser = true;
            }
            if (cmd.description) {
                entry.meta.description = *cmd.description;
                entry.meta.descriptionByUser = true;
            }
            if (cmd.url) {
                entry.meta.url = std::move(url);
            }
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
        // Дети и унаследованные теги от правки этих полей не меняются - берем из снимка.
        if (const auto* node = (*catalog)->find(id)) {
            entry.childCount = node->childCount;
        }
        entry.inheritedTags = (*catalog)->inheritedTags(id);
        entry.previewPending =
            entry.meta.kind == domain::Kind::Link && ctx->session.previewPending(id);
        return entry;
    }

    Result<std::vector<MoveConflict>> planMove(const Lease& lease, std::span<const EntryId> ids,
                                               std::optional<EntryId> parent) override {
        auto ctx = contextOf(lease);
        if (!ctx) {
            return std::unexpected(ctx.error());
        }
        auto catalog = catalogOf(*ctx, ports_.store);
        if (!catalog) {
            return std::unexpected(catalog.error());
        }
        auto plan = planMoveOn(ctx->session, **catalog, ids, parent, {});
        if (!plan) {
            return std::unexpected(plan.error());
        }
        return std::move(plan->conflicts);
    }

    Result<MoveResult> move(const Lease& lease, const MoveCmd& cmd) override {
        auto ctx = contextOf(lease);
        if (!ctx) {
            return std::unexpected(ctx.error());
        }
        auto catalog = catalogOf(*ctx, ports_.store);
        if (!catalog) {
            return std::unexpected(catalog.error());
        }
        auto plan = planMoveOn(ctx->session, **catalog, cmd.ids, cmd.parent, cmd.resolutions);
        if (!plan) {
            return std::unexpected(plan.error());
        }
        MoveResult result;
        if (plan->steps.empty()) {
            return result;
        }
        const Sealer& sealer = *ctx->sealer;
        {
            auto uow = ports_.store.begin();
            if (!uow) {
                return std::unexpected(uow.error());
            }
            std::vector<EntryId> replaced;
            for (const auto& step : plan->steps) {
                if (step.skip) {
                    ++result.skipped;
                    continue;
                }
                auto record = (*uow)->entries().get(step.id);
                if (!record) {
                    return std::unexpected(record.error());
                }
                record->parentId = cmd.parent;
                if (step.rename) {
                    auto encName = sealer.sealName(step.id, *step.rename);
                    if (!encName) {
                        return std::unexpected(encName.error());
                    }
                    record->encName = std::move(*encName);
                }
                if (auto st = (*uow)->entries().update(*record); !st) {
                    return std::unexpected(st.error());
                }
                ++result.moved;
                if (step.replaces) {
                    replaced.push_back(*step.replaces);
                }
            }
            for (const auto id : replaced) {
                if (auto removed = (*uow)->entries().removeSubtree(id); !removed) {
                    return std::unexpected(removed.error());
                }
                ++result.replaced;
            }
            if (auto st = (*uow)->commit(); !st) {
                return std::unexpected(st.error());
            }
        }
        ctx->session.invalidateCatalog();
        if (result.replaced > 0) {
            (void)ports_.store.compact();
        }
        return result;
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
