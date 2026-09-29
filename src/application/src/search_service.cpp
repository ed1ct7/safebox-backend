// Поиск по кешу расшифрованных имен (без учета регистра, "ё" = "е"). На диск не ходит
#include <algorithm>

#include "safebox/domain/model/rules.hpp"
#include "service_impls.hpp"
#include "vault_session.hpp"

namespace safebox::app {
namespace {

constexpr std::size_t kMaxSearchLimit = 1000;

class SearchServiceImpl final : public SearchService {
public:
    explicit SearchServiceImpl(Ports ports) : ports_(ports) {}

    Result<std::vector<domain::SearchHit>> search(const Lease& lease, std::string_view query,
                                                  std::size_t limit) override {
        const auto needle = foldForSearch(domain::detail::trim(query));
        if (needle.empty()) {
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

        std::vector<const Catalog::Node*> matches;
        for (const auto& [id, node] : cat.nodes()) {
            if (node.folded.find(needle) != std::string::npos) {
                matches.push_back(&node);
            }
        }
        std::sort(matches.begin(), matches.end(),
                  [](const Catalog::Node* a, const Catalog::Node* b) {
                      if (a->entry.isFolder() != b->entry.isFolder()) {
                          return a->entry.isFolder();
                      }
                      if (a->folded != b->folded) {
                          return a->folded < b->folded;
                      }
                      return a->entry.id < b->entry.id;
                  });
        const auto cap = limit == 0 ? kDefaultSearchLimit : std::min(limit, kMaxSearchLimit);
        if (matches.size() > cap) {
            matches.resize(cap);
        }

        std::vector<domain::SearchHit> hits;
        hits.reserve(matches.size());
        for (const auto* node : matches) {
            domain::SearchHit hit;
            hit.entry = cat.describe(*node);
            if (node->entry.parentId) {
                hit.path = cat.pathTo(*node->entry.parentId);
            }
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
