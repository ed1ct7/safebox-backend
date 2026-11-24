#include "entry_details.hpp"

#include <algorithm>

#include "safebox/domain/model/rules.hpp"

namespace safebox::app {

using domain::Error;
using domain::fail;

Status checkDetails(EntryDetails& details, const Catalog& catalog) {
    if (details.description) {
        if (details.description->size() > domain::kMaxDescriptionBytes) {
            return fail(Error::Code::InvalidArgument, "Описание длиннее 64 КиБ");
        }
        if (!domain::isValidUtf8(*details.description)) {
            return fail(Error::Code::InvalidArgument, "Описание содержит некорректные символы");
        }
    }
    if (details.source) {
        auto valid = domain::validateSource(*details.source);
        if (!valid) {
            return std::unexpected(valid.error());
        }
        *details.source = std::move(*valid);
    }
    if (details.sourceUrl) {
        *details.sourceUrl = std::string(domain::detail::trim(*details.sourceUrl));
        if (!details.sourceUrl->empty() && !domain::isHttpUrl(*details.sourceUrl)) {
            return fail(Error::Code::InvalidArgument,
                        "Адрес источника должен начинаться с http:// или https://");
        }
    }
    auto& tags = details.tags;
    std::ranges::sort(tags);
    const auto repeated = std::ranges::unique(tags);
    tags.erase(repeated.begin(), repeated.end());
    if (tags.size() > domain::kMaxTagsPerEntry) {
        return fail(Error::Code::InvalidArgument, "На записи не может быть больше 1000 тегов");
    }
    for (const auto id : tags) {
        if (catalog.findTag(id) == nullptr) {
            return fail(Error::Code::InvalidArgument, "Неизвестный тег");
        }
    }
    return {};
}

Result<TagIds> liveTags(domain::UnitOfWork& uow) {
    auto tags = uow.tags().tags();
    if (!tags) {
        return std::unexpected(tags.error());
    }
    TagIds ids;
    ids.reserve(tags->size());
    for (const auto& tag : *tags) {
        ids.insert(tag.id);
    }
    return ids;
}

void applyDetails(domain::EntryMeta& meta, const EntryDetails& details, const TagIds& live) {
    if (details.description) {
        meta.description = *details.description;
        meta.descriptionByUser = !details.description->empty(); // предпросмотр его не заменит
    }
    if (details.source) {
        meta.source = *details.source;
    }
    if (details.sourceUrl) {
        meta.sourceUrl = *details.sourceUrl;
    }
    if (details.sourceDate) {
        meta.sourceDate = *details.sourceDate;
    }
    for (const auto id : details.tags) {
        if (!live.contains(id) || meta.tags.size() >= domain::kMaxTagsPerEntry) {
            continue;
        }
        const auto at = std::ranges::lower_bound(meta.tags, id, {}, &domain::TagAssignment::tagId);
        if (at == meta.tags.end() || at->tagId != id) {
            meta.tags.insert(at, {id, false});
        }
    }
}

} // namespace safebox::app
