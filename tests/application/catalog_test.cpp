// Catalog: дети у любой записи, сироты и циклы, унаследованные теги; uniqueName.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <unordered_set>

#include "safebox/domain/model/rules.hpp"
#include "vault_session.hpp"

using namespace safebox;
using namespace safebox::app;

namespace {

domain::Entry entry(domain::EntryId id, std::optional<domain::EntryId> parent, domain::Kind kind,
                    std::string name, std::vector<domain::TagAssignment> tags = {}) {
    domain::Entry e;
    e.id = id;
    e.parentId = parent;
    e.name = std::move(name);
    e.meta.kind = kind;
    e.meta.tags = std::move(tags);
    return e;
}

std::vector<domain::EntryId> children(const Catalog& catalog, std::optional<domain::EntryId> id) {
    return catalog.childrenOf(id);
}

using Ids = std::vector<domain::EntryId>;

} // namespace

TEST_CASE("any entry can have children; folders first, then by folded name", "[catalog]") {
    Catalog catalog;
    catalog.add(entry(1, std::nullopt, domain::Kind::Photo, "Закат.jpg"));
    catalog.add(entry(2, 1, domain::Kind::File, "б.txt"));
    catalog.add(entry(3, 1, domain::Kind::Folder, "Я-папка"));
    catalog.add(entry(4, 1, domain::Kind::Link, "А-ссылка"));
    catalog.add(entry(5, 3, domain::Kind::File, "вглубине.txt"));
    catalog.add(entry(6, std::nullopt, domain::Kind::Folder, "Папка"));
    catalog.finalize();

    CHECK(children(catalog, std::nullopt) == Ids{6, 1});
    CHECK(children(catalog, 1) == Ids{3, 4, 2}); // папка, затем "а-ссылка", "б.txt"
    CHECK(children(catalog, 3) == Ids{5});
    CHECK(children(catalog, 2).empty());
    CHECK(catalog.find(1)->childCount == 3); // только прямые
    CHECK(catalog.find(3)->childCount == 1);
    CHECK(catalog.find(2)->childCount == 0);
    CHECK(catalog.find(6)->childCount == 0);

    const auto path = catalog.pathTo(5);
    REQUIRE(path.size() == 3);
    CHECK(path[0].name == "Закат.jpg"); // крошки идут и через файл
    CHECK(path[1].name == "Я-папка");

    const auto described = catalog.describe(*catalog.find(1));
    CHECK(described.childCount == 3);
    CHECK(described.name == "Закат.jpg");
    CHECK(catalog.find(1)->entry.childCount == 0); // внутри каталога счетчик отдельно
}

TEST_CASE("nodes keep folded names and descriptions", "[catalog]") {
    Catalog catalog;
    auto e = entry(1, std::nullopt, domain::Kind::File, "Ёлка.TXT");
    e.meta.description = "Новогодняя ЁЛКА";
    catalog.add(std::move(e));
    catalog.finalize();
    CHECK(catalog.find(1)->folded == "елка.txt");
    CHECK(catalog.find(1)->foldedDescription == "новогодняя елка");
}

TEST_CASE("orphans, self-parents and cycles land in the root", "[catalog]") {
    Catalog catalog;
    catalog.add(entry(1, 99, domain::Kind::File, "сирота.txt")); // родителя нет
    catalog.add(entry(2, 2, domain::Kind::Folder, "сам себе"));  // родитель - он сам
    catalog.add(entry(3, 4, domain::Kind::Folder, "цикл А"));
    catalog.add(entry(4, 3, domain::Kind::Photo, "цикл Б.jpg"));
    catalog.add(entry(5, std::nullopt, domain::Kind::Folder, "нормальная"));
    catalog.add(entry(6, 5, domain::Kind::File, "внутри.txt"));
    catalog.add(entry(7, 1, domain::Kind::File, "у сироты.txt")); // ребенок сироты
    catalog.finalize();

    const auto root = children(catalog, std::nullopt);
    for (const auto id : {1, 2, 3, 4, 5}) {
        INFO("в корне: " << id);
        CHECK(std::find(root.begin(), root.end(), id) != root.end());
    }
    CHECK(children(catalog, 5) == Ids{6});
    CHECK(children(catalog, 1) == Ids{7});
    // цикл держится на parent_id, ничего не зависает
    CHECK(catalog.pathTo(3).size() == 2);
    CHECK(catalog.pathTo(4).size() == 2);
    CHECK(catalog.pathTo(2).size() == 1);
    CHECK(catalog.isAncestorOrSelf(3, 4));
    CHECK(catalog.isAncestorOrSelf(4, 3));
    CHECK_FALSE(catalog.isAncestorOrSelf(5, 3));
}

TEST_CASE("inheritedTags: nearest ancestor wins, direct tags are not repeated", "[catalog][tags]") {
    Catalog catalog;
    catalog.add(entry(1, std::nullopt, domain::Kind::Folder, "корень", {{10, true}, {11, true}}));
    catalog.add(entry(2, 1, domain::Kind::Folder, "середина", {{10, true}, {12, false}}));
    catalog.add(entry(3, 2, domain::Kind::Photo, "лист.jpg", {{11, false}, {13, true}}));
    catalog.add(entry(4, 3, domain::Kind::File, "вложение.txt", {{14, false}}));
    catalog.finalize();

    // у середины: 11 от корня; 10 у нее и так прямой, а 12 без inherit вниз не идет
    CHECK(catalog.inheritedTags(2) == std::vector<domain::InheritedTag>{{11, 1}});
    // у листа: 10 от середины (она ближе корня), 11 - прямой у листа, не повторяется;
    // 12 без inherit не доходит
    CHECK(catalog.inheritedTags(3) == std::vector<domain::InheritedTag>{{10, 2}});
    // у вложения: 10 от середины, 11 от корня, 13 от листа; собственный 14 не считается
    CHECK(catalog.inheritedTags(4) == std::vector<domain::InheritedTag>{{10, 2}, {11, 1}, {13, 3}});
    CHECK(catalog.inheritedTags(1).empty());
    CHECK(catalog.inheritedTags(999).empty());

    CHECK(catalog.effectiveTags(1) == std::vector<domain::TagId>{10, 11});
    CHECK(catalog.effectiveTags(2) == std::vector<domain::TagId>{10, 11, 12});
    CHECK(catalog.effectiveTags(3) == std::vector<domain::TagId>{10, 11, 13});
    CHECK(catalog.effectiveTags(4) == std::vector<domain::TagId>{10, 11, 13, 14});
    CHECK(catalog.effectiveTags(999).empty());

    const auto described = catalog.describe(*catalog.find(4));
    CHECK(described.inheritedTags.size() == 3);
    CHECK(described.meta.tags == std::vector<domain::TagAssignment>{{14, false}});
}

TEST_CASE("a non-inheriting tag on a nearer ancestor does not block a farther one",
          "[catalog][tags]") {
    Catalog catalog;
    catalog.add(entry(1, std::nullopt, domain::Kind::Folder, "дальний", {{5, true}}));
    catalog.add(entry(2, 1, domain::Kind::Folder, "ближний", {{5, false}}));
    catalog.add(entry(3, 2, domain::Kind::File, "файл.txt"));
    catalog.finalize();
    CHECK(catalog.inheritedTags(3) == std::vector<domain::InheritedTag>{{5, 1}});
    CHECK(catalog.effectiveTags(2) == std::vector<domain::TagId>{5});
}

TEST_CASE("inheritance survives cycles in parent_id", "[catalog][tags]") {
    Catalog catalog;
    catalog.add(entry(1, 2, domain::Kind::Folder, "А", {{5, true}}));
    catalog.add(entry(2, 1, domain::Kind::Folder, "Б", {{6, true}}));
    catalog.finalize();
    CHECK(catalog.inheritedTags(1) == std::vector<domain::InheritedTag>{{6, 2}});
    CHECK(catalog.inheritedTags(2) == std::vector<domain::InheritedTag>{{5, 1}});
    CHECK(catalog.effectiveTags(1) == std::vector<domain::TagId>{5, 6});
}

TEST_CASE("isAncestorOrSelf walks the parent chain", "[catalog]") {
    Catalog catalog;
    catalog.add(entry(1, std::nullopt, domain::Kind::Folder, "а"));
    catalog.add(entry(2, 1, domain::Kind::Photo, "б"));
    catalog.add(entry(3, 2, domain::Kind::Folder, "в")); // папка внутри фото
    catalog.add(entry(4, std::nullopt, domain::Kind::Folder, "г"));
    catalog.finalize();
    CHECK(catalog.isAncestorOrSelf(1, 3));
    CHECK(catalog.isAncestorOrSelf(2, 3));
    CHECK(catalog.isAncestorOrSelf(3, 3)); // сама запись
    CHECK_FALSE(catalog.isAncestorOrSelf(3, 1));
    CHECK_FALSE(catalog.isAncestorOrSelf(4, 3));
    CHECK_FALSE(catalog.isAncestorOrSelf(1, 4));
    CHECK(catalog.isAncestorOrSelf(77, 77)); // даже неизвестная - сама себе
    CHECK_FALSE(catalog.isAncestorOrSelf(1, 77));
}

TEST_CASE("categories and tags are looked up by id with folded names", "[catalog][tags]") {
    Catalog catalog;
    catalog.addCategory({1, "Люди"});
    catalog.addCategory({2, "Места"});
    catalog.addTag({10, 1, "Ёжик"});
    catalog.addTag({11, 2, "Казань"});
    catalog.finalize();
    CHECK(catalog.categories().size() == 2);
    CHECK(catalog.tags().size() == 2);
    REQUIRE(catalog.findTag(10) != nullptr);
    CHECK(catalog.findTag(10)->folded == "ежик");
    CHECK(catalog.findTag(10)->tag.categoryId == 1);
    CHECK(catalog.findCategory(2)->category.name == "Места");
    CHECK(catalog.findTag(1) == nullptr);
    CHECK(catalog.findCategory(10) == nullptr);
}

TEST_CASE("uniqueName numbers duplicates before the extension", "[names]") {
    using Taken = std::unordered_set<std::string>;
    CHECK(uniqueName("a.txt", false, {}) == "a.txt");
    CHECK(uniqueName("a.txt", false, Taken{"b.txt"}) == "a.txt");
    CHECK(uniqueName("a.txt", false, Taken{"a.txt"}) == "a (2).txt");
    CHECK(uniqueName("a.txt", false, Taken{"a.txt", "a (2).txt", "a (3).txt"}) == "a (4).txt");
    // сравнение по folded-именам: регистр и "ё" не различаются
    CHECK(uniqueName("Ёлка.TXT", false, Taken{"елка.txt"}) == "Ёлка (2).TXT");
    CHECK(uniqueName("Ёлка.TXT", false, Taken{"елка.txt", "елка (2).txt"}) == "Ёлка (3).TXT");
    // у папки расширения нет, у файла без точки и у ".gitignore" тоже
    CHECK(uniqueName("Отчёты.2024", true, Taken{"отчеты.2024"}) == "Отчёты.2024 (2)");
    CHECK(uniqueName("README", false, Taken{"readme"}) == "README (2)");
    CHECK(uniqueName(".gitignore", false, Taken{".gitignore"}) == ".gitignore (2)");
    CHECK(uniqueName("архив.tar.gz", false, Taken{"архив.tar.gz"}) == "архив.tar (2).gz");
}

TEST_CASE("uniqueName keeps the name within the length limit", "[names]") {
    const std::string stem(domain::kMaxNameBytes - 4, 'x'); // "xxx...x.txt" ровно 255 байт
    const auto name = stem + ".txt";
    REQUIRE(name.size() == domain::kMaxNameBytes);
    const auto unique = uniqueName(name, false, {foldForSearch(name)});
    CHECK(unique.size() <= domain::kMaxNameBytes);
    CHECK(unique.ends_with(" (2).txt"));
    CHECK(domain::validateName(unique).has_value());

    // срез не рвет многобайтный символ
    std::string wide;
    while (wide.size() < domain::kMaxNameBytes - 1) {
        wide += "ж";
    }
    const auto uniqueWide = uniqueName(wide, true, {foldForSearch(wide)});
    CHECK(uniqueWide.size() <= domain::kMaxNameBytes);
    CHECK(domain::isValidUtf8(uniqueWide));
    CHECK(uniqueWide.ends_with(" (2)"));
}
