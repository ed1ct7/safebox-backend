#include "ordering.hpp"

#include <algorithm>

namespace safebox::app {
namespace {

[[nodiscard]] bool isDigit(char c) noexcept {
    return c >= '0' && c <= '9';
}

// Серия цифр с позиции at: [начало значащих цифр, конец серии).
[[nodiscard]] std::pair<std::size_t, std::size_t> digitRun(std::string_view text,
                                                           std::size_t at) noexcept {
    while (at < text.size() && text[at] == '0') {
        ++at;
    }
    auto end = at;
    while (end < text.size() && isDigit(text[end])) {
        ++end;
    }
    return {at, end};
}

// Сравнение по значению с точностью до ведущих нулей; 0 - "равны" ("007" и "7").
[[nodiscard]] int naturalCompare(std::string_view a, std::string_view b) noexcept {
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < a.size() && j < b.size()) {
        if (isDigit(a[i]) && isDigit(b[j])) {
            const auto [ai, ae] = digitRun(a, i);
            const auto [bi, be] = digitRun(b, j);
            if (ae - ai != be - bi) {
                return ae - ai < be - bi ? -1 : 1;
            }
            if (const int c = a.substr(ai, ae - ai).compare(b.substr(bi, be - bi)); c != 0) {
                return c < 0 ? -1 : 1;
            }
            i = ae;
            j = be;
            continue;
        }
        // цифра против не-цифры решается одинаково для любой цифры: порядок транзитивен
        const auto ca = static_cast<unsigned char>(a[i]);
        const auto cb = static_cast<unsigned char>(b[j]);
        if (ca != cb) {
            return ca < cb ? -1 : 1;
        }
        ++i;
        ++j;
    }
    if (i < a.size()) {
        return 1;
    }
    return j < b.size() ? -1 : 0;
}

// Дата для SortKey::Date: из источника, иначе изменения исходного файла, иначе импорта.
[[nodiscard]] std::int64_t dateOf(const domain::EntryMeta& meta) noexcept {
    if (meta.sourceDate) {
        return *meta.sourceDate;
    }
    return meta.sourceModifiedAt ? *meta.sourceModifiedAt : meta.createdAt;
}

template <typename T>
[[nodiscard]] int threeWay(T a, T b) noexcept {
    return a < b ? -1 : (b < a ? 1 : 0);
}

[[nodiscard]] int compareKey(const domain::EntryMeta& a, const domain::EntryMeta& b,
                             SortKey key) noexcept {
    switch (key) {
    case SortKey::Name:
        return 0;
    case SortKey::Added:
        return threeWay(a.createdAt, b.createdAt);
    case SortKey::Modified:
        return threeWay(a.modifiedAt, b.modifiedAt);
    case SortKey::Date:
        return threeWay(dateOf(a), dateOf(b));
    case SortKey::Size:
        return threeWay(a.size, b.size);
    case SortKey::Trashed:
        return threeWay(a.trashedAt.value_or(0), b.trashedAt.value_or(0));
    }
    return 0;
}

} // namespace

int compareNames(std::string_view a, std::string_view b) noexcept {
    if (const int c = naturalCompare(a, b); c != 0) {
        return c;
    }
    return threeWay(a, b); // "007" и "7": побайтно, чтобы порядок был строгим
}

bool listedBefore(const Catalog::Node& a, const Catalog::Node& b,
                  const ListOptions& options) noexcept {
    if (a.entry.isFolder() != b.entry.isFolder()) {
        return a.entry.isFolder();
    }
    const bool byName = options.sort == SortKey::Name;
    int c = byName ? compareNames(a.folded, b.folded)
                   : compareKey(a.entry.meta, b.entry.meta, options.sort);
    if (c != 0) {
        return options.descending ? c > 0 : c < 0;
    }
    if (!byName) {
        c = compareNames(a.folded, b.folded);
        if (c != 0) {
            return c < 0;
        }
    }
    return byName && options.descending ? a.entry.id > b.entry.id : a.entry.id < b.entry.id;
}

std::pair<std::size_t, std::size_t> windowOf(std::size_t total,
                                             const ListOptions& options) noexcept {
    const auto begin = std::min(options.offset, total);
    const auto rest = total - begin;
    const auto count = options.limit == 0 ? rest : std::min(options.limit, rest);
    return {begin, begin + count};
}

} // namespace safebox::app
