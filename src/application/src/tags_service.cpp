// TagsService: категории, теги и их присвоения записям (лежат в enc_meta записи).
// Дубли имен ищем по снимку каталога, поэтому правки идут строго по очереди (mutex_): два
// одновременных запроса иначе не увидели бы друг друга.
// Правка, затрагивающая записи, переписывает их enc_meta в той же транзакции, что и сам тег.
// Дату изменения записи это не трогает: тег - не содержимое.
#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include "safebox/domain/model/rules.hpp"
#include "service_impls.hpp"
#include "vault_session.hpp"

namespace safebox::app {
namespace {

using domain::CategoryId;
using domain::EntryId;
using domain::Error;
using domain::fail;
using domain::TagId;

using Assignments = std::vector<domain::TagAssignment>;

struct Snapshot {
    OperationContext ctx;
    VaultSession::CatalogPtr catalog;
};

Result<Snapshot> snapshotOf(const Lease& lease, domain::VaultStore& store) {
    auto ctx = contextOf(lease);
    if (!ctx) {
        return std::unexpected(ctx.error());
    }
    auto catalog = catalogOf(*ctx, store);
    if (!catalog) {
        return std::unexpected(catalog.error());
    }
    return Snapshot{std::move(*ctx), std::move(*catalog)};
}

const Catalog::CategoryNode* categoryNamed(const Catalog& cat, const std::string& folded) {
    for (const auto& [id, node] : cat.categories()) {
        if (node.folded == folded) {
            return &node;
        }
    }
    return nullptr;
}

// except - тег, которого не считаем (переименование в собственное имя не дубль).
const Catalog::TagNode* tagNamed(const Catalog& cat, CategoryId category, const std::string& folded,
                                 TagId except = 0) {
    for (const auto& [id, node] : cat.tags()) {
        if (node.tag.categoryId == category && node.folded == folded && id != except) {
            return &node;
        }
    }
    return nullptr;
}

// Сколько записей держит каждый тег напрямую.
std::unordered_map<TagId, std::size_t> directCounts(const Catalog& cat) {
    std::unordered_map<TagId, std::size_t> counts;
    for (const auto& [id, node] : cat.nodes()) {
        for (const auto& assignment : node.entry.meta.tags) {
            ++counts[assignment.tagId];
        }
    }
    return counts;
}

CategoryWithTags describeCategory(const Catalog& cat, const Catalog::CategoryNode& category,
                                  const std::unordered_map<TagId, std::size_t>& counts) {
    std::vector<const Catalog::TagNode*> nodes;
    for (const auto& [id, node] : cat.tags()) {
        if (node.tag.categoryId == category.category.id) {
            nodes.push_back(&node);
        }
    }
    std::sort(nodes.begin(), nodes.end(), [](const Catalog::TagNode* a, const Catalog::TagNode* b) {
        return a->folded != b->folded ? a->folded < b->folded : a->tag.id < b->tag.id;
    });
    CategoryWithTags out;
    out.category = category.category;
    out.tags.reserve(nodes.size());
    for (const auto* node : nodes) {
        const auto found = counts.find(node->tag.id);
        out.tags.push_back({node->tag, found == counts.end() ? 0 : found->second});
    }
    return out;
}

// Записи, у которых один из тегов стоит напрямую; по возрастанию id.
std::vector<EntryId> holdersOf(const Catalog& cat, const std::unordered_set<TagId>& tags) {
    std::vector<EntryId> ids;
    for (const auto& [id, node] : cat.nodes()) {
        const auto& assigned = node.entry.meta.tags;
        if (std::any_of(assigned.begin(), assigned.end(),
                        [&](const auto& a) { return tags.contains(a.tagId); })) {
            ids.push_back(id);
        }
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

// Запись из транзакции: edit правит ее теги, при изменении мета перезапечатывается (имя и
// остальное как были). -> изменилась ли запись.
template <class Edit>
Result<bool> editTags(domain::UnitOfWork& uow, const Sealer& sealer, EntryId id, Edit&& edit) {
    auto record = uow.entries().get(id);
    if (!record) {
        return std::unexpected(record.error());
    }
    auto entry = sealer.openEntry(*record);
    if (!entry) {
        return std::unexpected(entry.error());
    }
    const auto wipe = [&] {
        domain::secureWipe(entry->name);
        domain::secureWipe(entry->meta.description);
        domain::secureWipe(entry->meta.url);
    };
    auto& tags = entry->meta.tags;
    const auto before = tags;
    edit(tags);
    std::ranges::sort(tags, {}, &domain::TagAssignment::tagId);
    if (tags == before) {
        wipe();
        return false;
    }
    if (tags.size() > domain::kMaxTagsPerEntry) {
        wipe();
        return fail(Error::Code::InvalidArgument, "На записи не может быть больше 1000 тегов");
    }
    auto encMeta = sealer.sealMeta(id, entry->meta);
    wipe();
    if (!encMeta) {
        return std::unexpected(encMeta.error());
    }
    if (auto st = uow.entries().updateSealed(id, record->encName, *encMeta); !st) {
        return std::unexpected(st.error());
    }
    return true;
}

// edit ко всем записям ids -> сколько изменилось.
template <class Edit>
Result<std::size_t> editEntries(domain::UnitOfWork& uow, const Sealer& sealer,
                                const std::vector<EntryId>& ids, Edit&& edit) {
    std::size_t changed = 0;
    for (const auto id : ids) {
        auto result = editTags(uow, sealer, id, edit);
        if (!result) {
            return std::unexpected(result.error());
        }
        changed += *result ? 1 : 0;
    }
    return changed;
}

// Теги набора снимаются с записи.
auto stripping(const std::unordered_set<TagId>& gone) {
    return [&gone](Assignments& tags) {
        std::erase_if(tags, [&](const auto& a) { return gone.contains(a.tagId); });
    };
}

Result<CategoryId> putCategory(domain::UnitOfWork& uow, const Sealer& sealer,
                               std::string_view name) {
    auto id = uow.tags().insertCategory();
    if (!id) {
        return std::unexpected(id.error());
    }
    auto encName = sealer.sealCategoryName(*id, name);
    if (!encName) {
        return std::unexpected(encName.error());
    }
    if (auto st = uow.tags().updateCategory(*id, *encName); !st) {
        return std::unexpected(st.error());
    }
    return *id;
}

Result<TagId> putTag(domain::UnitOfWork& uow, const Sealer& sealer, CategoryId category,
                     std::string_view name) {
    auto id = uow.tags().insertTag(category);
    if (!id) {
        return std::unexpected(id.error());
    }
    auto encName = sealer.sealTagName(*id, category, name);
    if (!encName) {
        return std::unexpected(encName.error());
    }
    if (auto st = uow.tags().updateTag(*id, category, *encName); !st) {
        return std::unexpected(st.error());
    }
    return *id;
}

class TagsServiceImpl final : public TagsService {
public:
    explicit TagsServiceImpl(Ports ports) : ports_(ports) {}

    Result<std::vector<CategoryWithTags>> list(const Lease& lease) override {
        auto snap = snapshotOf(lease, ports_.store);
        if (!snap) {
            return std::unexpected(snap.error());
        }
        const Catalog& cat = *snap->catalog;
        std::vector<const Catalog::CategoryNode*> nodes;
        for (const auto& [id, node] : cat.categories()) {
            nodes.push_back(&node);
        }
        std::sort(nodes.begin(), nodes.end(),
                  [](const Catalog::CategoryNode* a, const Catalog::CategoryNode* b) {
                      return a->folded != b->folded ? a->folded < b->folded
                                                    : a->category.id < b->category.id;
                  });
        const auto counts = directCounts(cat);
        std::vector<CategoryWithTags> out;
        out.reserve(nodes.size());
        for (const auto* node : nodes) {
            out.push_back(describeCategory(cat, *node, counts));
        }
        return out;
    }

    Result<domain::TagCategory> createCategory(const Lease& lease, std::string_view name) override {
        auto clean = domain::validateTagName(name);
        if (!clean) {
            return std::unexpected(clean.error());
        }
        std::scoped_lock lock(mutex_);
        auto snap = snapshotOf(lease, ports_.store);
        if (!snap) {
            return std::unexpected(snap.error());
        }
        if (categoryNamed(*snap->catalog, foldForSearch(*clean)) != nullptr) {
            return fail(Error::Code::AlreadyExists, "Категория с таким названием уже есть");
        }
        auto uow = ports_.store.begin();
        if (!uow) {
            return std::unexpected(uow.error());
        }
        auto id = putCategory(**uow, *snap->ctx.sealer, *clean);
        if (!id) {
            return std::unexpected(id.error());
        }
        if (auto st = (*uow)->commit(); !st) {
            return std::unexpected(st.error());
        }
        snap->ctx.session.invalidateCatalog();
        return domain::TagCategory{*id, std::move(*clean)};
    }

    Result<CategoryWithTags> renameCategory(const Lease& lease, CategoryId id,
                                            std::string_view name) override {
        auto clean = domain::validateTagName(name);
        if (!clean) {
            return std::unexpected(clean.error());
        }
        std::scoped_lock lock(mutex_);
        auto snap = snapshotOf(lease, ports_.store);
        if (!snap) {
            return std::unexpected(snap.error());
        }
        const Catalog& cat = *snap->catalog;
        const auto* node = cat.findCategory(id);
        if (node == nullptr) {
            return fail(Error::Code::NotFound, "Категория не найдена");
        }
        const auto folded = foldForSearch(*clean);
        if (const auto* same = categoryNamed(cat, folded); same != nullptr && same != node) {
            return fail(Error::Code::AlreadyExists, "Категория с таким названием уже есть");
        }
        auto out = describeCategory(cat, *node, directCounts(cat));
        if (*clean != node->category.name) {
            auto encName = snap->ctx.sealer->sealCategoryName(id, *clean);
            if (!encName) {
                return std::unexpected(encName.error());
            }
            auto uow = ports_.store.begin();
            if (!uow) {
                return std::unexpected(uow.error());
            }
            if (auto st = (*uow)->tags().updateCategory(id, *encName); !st) {
                return std::unexpected(st.error());
            }
            if (auto st = (*uow)->commit(); !st) {
                return std::unexpected(st.error());
            }
            snap->ctx.session.invalidateCatalog();
        }
        out.category.name = std::move(*clean);
        return out;
    }

    Result<RemovedTags> removeCategory(const Lease& lease, CategoryId id) override {
        std::scoped_lock lock(mutex_);
        auto snap = snapshotOf(lease, ports_.store);
        if (!snap) {
            return std::unexpected(snap.error());
        }
        const Catalog& cat = *snap->catalog;
        if (cat.findCategory(id) == nullptr) {
            return fail(Error::Code::NotFound, "Категория не найдена");
        }
        std::unordered_set<TagId> doomed;
        for (const auto& [tagId, node] : cat.tags()) {
            if (node.tag.categoryId == id) {
                doomed.insert(tagId);
            }
        }
        RemovedTags removed;
        removed.removedTags = doomed.size();
        {
            auto uow = ports_.store.begin();
            if (!uow) {
                return std::unexpected(uow.error());
            }
            auto changed =
                editEntries(**uow, *snap->ctx.sealer, holdersOf(cat, doomed), stripping(doomed));
            if (!changed) {
                return std::unexpected(changed.error());
            }
            removed.affectedEntries = *changed;
            if (auto st = (*uow)->tags().removeCategory(id); !st) { // теги уходят каскадом
                return std::unexpected(st.error());
            }
            if (auto st = (*uow)->commit(); !st) {
                return std::unexpected(st.error());
            }
        }
        snap->ctx.session.invalidateCatalog();
        return removed;
    }

    Result<CreatedTag> createTag(const Lease& lease, const CreateTagCmd& cmd) override {
        auto categoryName = domain::validateTagName(cmd.category);
        if (!categoryName) {
            return std::unexpected(categoryName.error());
        }
        auto tagName = domain::validateTagName(cmd.name);
        if (!tagName) {
            return std::unexpected(tagName.error());
        }
        std::scoped_lock lock(mutex_);
        auto snap = snapshotOf(lease, ports_.store);
        if (!snap) {
            return std::unexpected(snap.error());
        }
        const Catalog& cat = *snap->catalog;
        const auto* category = categoryNamed(cat, foldForSearch(*categoryName));
        if (category != nullptr) {
            if (const auto* found = tagNamed(cat, category->category.id, foldForSearch(*tagName))) {
                return CreatedTag{found->tag, false};
            }
        } else if (!cmd.createCategory) {
            return fail(Error::Code::NotFound, "Категория не найдена");
        }

        const Sealer& sealer = *snap->ctx.sealer;
        auto uow = ports_.store.begin();
        if (!uow) {
            return std::unexpected(uow.error());
        }
        CategoryId categoryId = 0;
        if (category != nullptr) {
            categoryId = category->category.id;
        } else {
            auto made = putCategory(**uow, sealer, *categoryName);
            if (!made) {
                return std::unexpected(made.error());
            }
            categoryId = *made;
        }
        auto tagId = putTag(**uow, sealer, categoryId, *tagName);
        if (!tagId) {
            return std::unexpected(tagId.error());
        }
        if (auto st = (*uow)->commit(); !st) {
            return std::unexpected(st.error());
        }
        snap->ctx.session.invalidateCatalog();
        return CreatedTag{domain::Tag{*tagId, categoryId, std::move(*tagName)}, true};
    }

    Result<domain::Tag> updateTag(const Lease& lease, TagId id, const UpdateTagCmd& cmd) override {
        if (!cmd.name && !cmd.categoryId) {
            return fail(Error::Code::InvalidArgument, "Не указано, что менять");
        }
        std::string name;
        if (cmd.name) {
            auto clean = domain::validateTagName(*cmd.name);
            if (!clean) {
                return std::unexpected(clean.error());
            }
            name = std::move(*clean);
        }
        std::scoped_lock lock(mutex_);
        auto snap = snapshotOf(lease, ports_.store);
        if (!snap) {
            return std::unexpected(snap.error());
        }
        const Catalog& cat = *snap->catalog;
        const auto* node = cat.findTag(id);
        if (node == nullptr) {
            return fail(Error::Code::NotFound, "Тег не найден");
        }
        domain::Tag updated = node->tag;
        if (cmd.name) {
            updated.name = std::move(name);
        }
        if (cmd.categoryId) {
            if (cat.findCategory(*cmd.categoryId) == nullptr) {
                return fail(Error::Code::NotFound, "Категория не найдена");
            }
            updated.categoryId = *cmd.categoryId;
        }
        if (updated.name == node->tag.name && updated.categoryId == node->tag.categoryId) {
            return updated;
        }
        if (tagNamed(cat, updated.categoryId, foldForSearch(updated.name), id) != nullptr) {
            return fail(Error::Code::AlreadyExists, "Такой тег в этой категории уже есть");
        }
        // категория входит в AAD имени: при переносе имя запечатывается заново
        auto encName = snap->ctx.sealer->sealTagName(id, updated.categoryId, updated.name);
        if (!encName) {
            return std::unexpected(encName.error());
        }
        auto uow = ports_.store.begin();
        if (!uow) {
            return std::unexpected(uow.error());
        }
        if (auto st = (*uow)->tags().updateTag(id, updated.categoryId, *encName); !st) {
            return std::unexpected(st.error());
        }
        if (auto st = (*uow)->commit(); !st) {
            return std::unexpected(st.error());
        }
        snap->ctx.session.invalidateCatalog();
        return updated;
    }

    Result<std::size_t> mergeTag(const Lease& lease, TagId from, TagId into) override {
        if (from == into) {
            return fail(Error::Code::InvalidArgument, "Нельзя слить тег с самим собой");
        }
        std::scoped_lock lock(mutex_);
        auto snap = snapshotOf(lease, ports_.store);
        if (!snap) {
            return std::unexpected(snap.error());
        }
        const Catalog& cat = *snap->catalog;
        if (cat.findTag(from) == nullptr) {
            return fail(Error::Code::NotFound, "Тег не найден");
        }
        if (cat.findTag(into) == nullptr) {
            return fail(Error::Code::NotFound, "Тег, в который сливаем, не найден");
        }
        const auto merge = [from, into](Assignments& tags) {
            const auto source = std::ranges::find(tags, from, &domain::TagAssignment::tagId);
            if (source == tags.end()) {
                return;
            }
            const bool inherit = source->inherit;
            tags.erase(source);
            const auto target = std::ranges::find(tags, into, &domain::TagAssignment::tagId);
            if (target != tags.end()) {
                target->inherit = target->inherit || inherit;
            } else {
                tags.push_back({into, inherit});
            }
        };
        return rewriteAndDrop(*snap, holdersOf(cat, {from}), merge, from);
    }

    Result<std::size_t> removeTag(const Lease& lease, TagId id) override {
        std::scoped_lock lock(mutex_);
        auto snap = snapshotOf(lease, ports_.store);
        if (!snap) {
            return std::unexpected(snap.error());
        }
        const Catalog& cat = *snap->catalog;
        if (cat.findTag(id) == nullptr) {
            return fail(Error::Code::NotFound, "Тег не найден");
        }
        const std::unordered_set<TagId> gone{id};
        return rewriteAndDrop(*snap, holdersOf(cat, gone), stripping(gone), id);
    }

    Result<std::size_t> assign(const Lease& lease, const AssignTagsCmd& cmd) override {
        std::scoped_lock lock(mutex_);
        auto snap = snapshotOf(lease, ports_.store);
        if (!snap) {
            return std::unexpected(snap.error());
        }
        const Catalog& cat = *snap->catalog;
        for (const auto tagId : cmd.remove) {
            if (cat.findTag(tagId) == nullptr) {
                return fail(Error::Code::InvalidArgument, "Неизвестный тег");
            }
        }
        for (const auto& assignment : cmd.add) {
            if (cat.findTag(assignment.tagId) == nullptr) {
                return fail(Error::Code::InvalidArgument, "Неизвестный тег");
            }
        }
        std::vector<EntryId> ids;
        std::unordered_set<EntryId> seen;
        for (const auto id : cmd.ids) {
            if (cat.find(id) == nullptr) {
                return fail(Error::Code::NotFound, "Объект не найден");
            }
            if (seen.insert(id).second) {
                ids.push_back(id);
            }
        }
        if (ids.empty() || (cmd.add.empty() && cmd.remove.empty())) {
            return std::size_t{0};
        }

        const std::unordered_set<TagId> removing(cmd.remove.begin(), cmd.remove.end());
        const auto strip = stripping(removing);
        const auto apply = [&](Assignments& tags) {
            strip(tags);
            for (const auto& wanted : cmd.add) {
                const auto found =
                    std::ranges::find(tags, wanted.tagId, &domain::TagAssignment::tagId);
                if (found != tags.end()) {
                    found->inherit = wanted.inherit;
                } else {
                    tags.push_back(wanted);
                }
            }
        };
        std::size_t changed = 0;
        {
            auto uow = ports_.store.begin();
            if (!uow) {
                return std::unexpected(uow.error());
            }
            auto result = editEntries(**uow, *snap->ctx.sealer, ids, apply);
            if (!result) {
                return std::unexpected(result.error());
            }
            changed = *result;
            if (changed == 0) {
                return changed;
            }
            if (auto st = (*uow)->commit(); !st) {
                return std::unexpected(st.error());
            }
        }
        snap->ctx.session.invalidateCatalog();
        return changed;
    }

private:
    // Записи ids правятся edit, тег dropped удаляется - все одной транзакцией.
    template <class Edit>
    Result<std::size_t> rewriteAndDrop(const Snapshot& snap, const std::vector<EntryId>& ids,
                                       Edit&& edit, TagId dropped) {
        std::size_t changed = 0;
        {
            auto uow = ports_.store.begin();
            if (!uow) {
                return std::unexpected(uow.error());
            }
            auto result = editEntries(**uow, *snap.ctx.sealer, ids, edit);
            if (!result) {
                return std::unexpected(result.error());
            }
            changed = *result;
            if (auto st = (*uow)->tags().removeTag(dropped); !st) {
                return std::unexpected(st.error());
            }
            if (auto st = (*uow)->commit(); !st) {
                return std::unexpected(st.error());
            }
        }
        snap.ctx.session.invalidateCatalog();
        return changed;
    }

    Ports ports_;
    std::mutex mutex_;
};

} // namespace

std::shared_ptr<TagsService> makeTagsService(Ports ports) {
    return std::make_shared<TagsServiceImpl>(ports);
}

} // namespace safebox::app
