// Поиск и фильтр по кешу расшифрованного каталога (без учета регистра, "ё" = "е"). На диск не
// ходит. Текст ищется в имени, иначе в описании; теги - по прямым и унаследованным
#include <algorithm>
#include <map>
#include <unordered_set>

#include "safebox/domain/model/rules.hpp"
#include "service_impls.hpp"
#include "vault_session.hpp"

namespace safebox::app {
namespace {

using domain::EntryId;
using domain::Error;
using domain::fail;
using domain::TagId;

// Запись подходит, если из каждой группы у нее есть хотя бы один тег.
using TagGroups = std::vector<std::vector<TagId>>;

Result<TagGroups> groupTags(const Catalog& cat, const SearchQuery& query) {
    std::vector<TagId> wanted = query.tags;
    std::sort(wanted.begin(), wanted.end());
    wanted.erase(std::unique(wanted.begin(), wanted.end()), wanted.end());
    for (const auto id : wanted) {
        if (cat.findTag(id) == nullptr) {
            return fail(Error::Code::InvalidArgument, "Неизвестный тег");
        }
    }
    TagGroups groups;
    switch (query.match) {
    case TagMatch::Any:
        if (!wanted.empty()) {
            groups.push_back(std::move(wanted));
        }
        break;
    case TagMatch::All:
        for (const auto id : wanted) {
            groups.push_back({id});
        }
        break;
    case TagMatch::Categories: {
        std::map<domain::CategoryId, std::vector<TagId>> byCategory;
        for (const auto id : wanted) {
            byCategory[cat.findTag(id)->tag.categoryId].push_back(id);
        }
        for (auto& [category, ids] : byCategory) {
            groups.push_back(std::move(ids));
        }
        break;
    }
    }
    return groups;
}

// effective - теги записи по возрастанию.
bool hasTags(const std::vector<TagId>& effective, const TagGroups& groups) {
    return std::ranges::all_of(groups, [&](const std::vector<TagId>& group) {
        return std::ranges::any_of(group, [&](TagId id) {
            return std::binary_search(effective.begin(), effective.end(), id);
        });
    });
}

class SearchServiceImpl final : public SearchService {
public:
    explicit SearchServiceImpl(Ports ports) : ports_(ports) {}

    Result<std::vector<domain::SearchHit>> search(const Lease& lease,
                                                  const SearchQuery& query) override {
        const auto needle = foldForSearch(domain::detail::trim(query.text));
        if (needle.empty() && query.tags.empty()) {
            return std::vector<domain::SearchHit>{};
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
        auto groups = groupTags(cat, query);
        if (!groups) {
            return std::unexpected(groups.error());
        }

        std::vector<const Catalog::Node*> scope;
        if (query.within) {
            if (cat.find(*query.within) == nullptr) {
                return fail(Error::Code::NotFound, "Объект не найден");
            }
            // потомки без самой записи; seen страхует от циклов в parent_id
            std::unordered_set<EntryId> seen{*query.within};
            std::vector<EntryId> stack(cat.childrenOf(*query.within));
            while (!stack.empty()) {
                const auto id = stack.back();
                stack.pop_back();
                if (!seen.insert(id).second) {
                    continue;
                }
                scope.push_back(cat.find(id));
                const auto& children = cat.childrenOf(id);
                stack.insert(stack.end(), children.begin(), children.end());
            }
        } else {
            scope.reserve(cat.nodes().size());
            for (const auto& [id, node] : cat.nodes()) {
                scope.push_back(&node);
            }
        }

        struct Match {
            const Catalog::Node* node = nullptr;
            domain::MatchedIn matchedIn = domain::MatchedIn::None;
        };
        std::vector<Match> matches;
        for (const auto* node : scope) {
            Match match{node, domain::MatchedIn::None};
            if (!needle.empty()) {
                if (node->folded.find(needle) != std::string::npos) {
                    match.matchedIn = domain::MatchedIn::Name;
                } else if (node->foldedDescription.find(needle) != std::string::npos) {
                    match.matchedIn = domain::MatchedIn::Description;
                } else {
                    continue;
                }
            }
            if (!groups->empty() && !hasTags(cat.effectiveTags(node->entry.id), *groups)) {
                continue;
            }
            matches.push_back(match);
        }
        std::sort(matches.begin(), matches.end(), [](const Match& x, const Match& y) {
            const auto& a = *x.node;
            const auto& b = *y.node;
            if (a.entry.isFolder() != b.entry.isFolder()) {
                return a.entry.isFolder();
            }
            if (a.folded != b.folded) {
                return a.folded < b.folded;
            }
            return a.entry.id < b.entry.id;
        });
        if (query.limit != 0 && matches.size() > query.limit) {
            matches.resize(query.limit);
        }

        std::vector<domain::SearchHit> hits;
        hits.reserve(matches.size());
        for (const auto& match : matches) {
            domain::SearchHit hit;
            hit.entry = describeEntry(ctx->session, cat, *match.node);
            if (match.node->entry.parentId) {
                hit.path = cat.pathTo(*match.node->entry.parentId);
            }
            hit.matchedIn = match.matchedIn;
            hits.push_back(std::move(hit));
        }
        return hits;
    }

private:
    Ports ports_;
};

} // namespace

std::shared_ptr<SearchService> makeSearchService(Ports ports) {
    return std::make_shared<SearchServiceImpl>(ports);
}

} // namespace safebox::app
