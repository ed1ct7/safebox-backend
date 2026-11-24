// Catalog: дети у любой записи, сироты и циклы, унаследованные теги; uniqueName.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <unordered_set>

#include "safebox/domain/model/rules.hpp"
#include "safebox/infra/factories.hpp"
#include "sealing.hpp"
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
    CHECK(catalog.childCount(1) == 3); // только прямые
    CHECK(catalog.childCount(3) == 1);
    CHECK(catalog.childCount(2) == 0);
    CHECK(catalog.childCount(6) == 0);

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

// Инкрементальные правки: после любой цепочки upsert/erase каталог тот же, что построенный
// заново из тех же записей.

namespace {

using Model = std::map<domain::EntryId, domain::Entry>;

std::shared_ptr<Catalog> rebuilt(const Model& model) {
    auto catalog = std::make_shared<Catalog>();
    for (const auto& [id, e] : model) {
        catalog->add(e);
    }
    catalog->finalize();
    return catalog;
}

void requireSame(const Catalog& patched, const Model& model) {
    const auto fresh = rebuilt(model);
    REQUIRE(patched.nodes().size() == fresh->nodes().size());
    CHECK(patched.childrenOf(std::nullopt) == fresh->childrenOf(std::nullopt));
    for (const auto& [id, e] : model) {
        INFO("запись " << id);
        const auto* node = patched.find(id);
        REQUIRE(node != nullptr);
        CHECK(node->entry.name == e.name);
        CHECK(node->entry.parentId == e.parentId);
        CHECK(node->folded == foldForSearch(e.name));
        CHECK(patched.childrenOf(id) == fresh->childrenOf(id));
        CHECK(patched.childCount(id) == fresh->childCount(id));
    }
}

// Удаление поддерева из модели (как removeSubtree в хранилище).
void eraseFrom(Model& model, domain::EntryId id) {
    std::vector<domain::EntryId> stack{id};
    while (!stack.empty()) {
        const auto current = stack.back();
        stack.pop_back();
        model.erase(current);
        for (const auto& [child, e] : model) {
            if (e.parentId == current) {
                stack.push_back(child);
            }
        }
    }
}

} // namespace

TEST_CASE("upsert re-sorts a renamed entry and moves it between parents", "[catalog][patch]") {
    Model model;
    for (auto e :
         {entry(1, std::nullopt, domain::Kind::Folder, "Б"),
          entry(2, std::nullopt, domain::Kind::Folder, "Г"),
          entry(3, 1, domain::Kind::File, "а.txt"), entry(4, 1, domain::Kind::File, "в.txt"),
          entry(5, 1, domain::Kind::Folder, "я")}) {
        model[e.id] = e;
    }
    auto catalog = rebuilt(model);
    CHECK(children(*catalog, 1) == Ids{5, 3, 4});

    model[3].name = "ю.txt"; // переименование: в конец списка
    REQUIRE(catalog->upsert({model[3]}));
    CHECK(children(*catalog, 1) == Ids{5, 4, 3});

    model[5].parentId = 2; // перенос папки с одной на другую
    model[1].name = "Д";   // и в корне сдвигается
    REQUIRE(catalog->upsert({model[5], model[1]}));
    CHECK(children(*catalog, 1) == Ids{4, 3});
    CHECK(children(*catalog, 2) == Ids{5});
    CHECK(children(*catalog, std::nullopt) == Ids{2, 1});
    requireSame(*catalog, model);
}

TEST_CASE("new folder and its files arrive in one batch in any order", "[catalog][patch]") {
    Model model;
    model[1] = entry(1, std::nullopt, domain::Kind::Folder, "корень");
    auto catalog = rebuilt(model);
    model[3] = entry(3, 2, domain::Kind::File, "файл.txt");
    model[2] = entry(2, 1, domain::Kind::Folder, "новая");
    REQUIRE(catalog->upsert({model[3], model[2]})); // ребенок раньше родителя
    CHECK(children(*catalog, 2) == Ids{3});
    requireSame(*catalog, model);
}

TEST_CASE("erase drops the subtree but keeps entries moved out of it", "[catalog][patch]") {
    Model model;
    model[1] = entry(1, std::nullopt, domain::Kind::Folder, "удаляемая");
    model[2] = entry(2, 1, domain::Kind::Folder, "вынесенная");
    model[3] = entry(3, 2, domain::Kind::File, "внутри вынесенной.txt");
    model[4] = entry(4, 1, domain::Kind::File, "уходит.txt");
    model[5] = entry(5, std::nullopt, domain::Kind::Folder, "цель");
    auto catalog = rebuilt(model);

    // Между чтениями: 2 перенесли в 5, потом удалили 1 - изменения пришли одной пачкой.
    model[2].parentId = 5;
    eraseFrom(model, 1);
    const std::vector<domain::EntryId> gone{1};
    catalog->erase(gone, {2});
    REQUIRE(catalog->upsert({model[2]}));
    CHECK(catalog->find(4) == nullptr);
    CHECK(children(*catalog, 5) == Ids{2});
    CHECK(children(*catalog, 2) == Ids{3}); // дети перенесенной папки на месте
    requireSame(*catalog, model);
}

TEST_CASE("upsert refuses cycles and catalogs loaded with them", "[catalog][patch]") {
    Model model;
    model[1] = entry(1, std::nullopt, domain::Kind::Folder, "А");
    model[2] = entry(2, 1, domain::Kind::Folder, "Б");
    auto catalog = rebuilt(model);
    auto looped = model[1];
    looped.parentId = 2;
    CHECK_FALSE(catalog->upsert({looped}));

    Catalog cyclic;
    cyclic.add(entry(3, 4, domain::Kind::Folder, "В"));
    cyclic.add(entry(4, 3, domain::Kind::Folder, "Г"));
    cyclic.finalize();
    CHECK_FALSE(cyclic.upsert({entry(5, std::nullopt, domain::Kind::File, "новый.txt")}));
}

TEST_CASE("clone shares nodes; edits stay in the copy", "[catalog][patch]") {
    Model model;
    model[1] = entry(1, std::nullopt, domain::Kind::Folder, "папка");
    model[2] = entry(2, 1, domain::Kind::File, "старое.txt");
    auto original = rebuilt(model);
    const auto* before = original->find(2);

    auto copy = original->clone();
    CHECK(copy->find(1) == original->find(1)); // тот же узел, не копия строк
    auto renamed = model[2];
    renamed.name = "новое.txt";
    REQUIRE(copy->upsert({renamed}));
    const std::vector<domain::EntryId> gone{1};
    copy->erase(gone, {});

    CHECK(original->find(2) == before);
    CHECK(original->find(2)->entry.name == "старое.txt");
    CHECK(children(*original, 1) == Ids{2});
    CHECK(copy->nodes().empty());
}

TEST_CASE("replaceTags swaps all categories and tags", "[catalog][patch]") {
    Catalog catalog;
    catalog.addCategory({.id = 1, .name = "Люди"});
    catalog.addTag({.id = 10, .categoryId = 1, .name = "Иван"});
    catalog.finalize();
    catalog.replaceTags({{.id = 2, .name = "Места"}},
                        {{.id = 20, .categoryId = 2, .name = "Казань"}});
    CHECK(catalog.findCategory(1) == nullptr);
    CHECK(catalog.findTag(10) == nullptr);
    REQUIRE(catalog.findTag(20) != nullptr);
    CHECK(catalog.findTag(20)->folded == "казань");
    CHECK(catalog.findCategory(2)->folded == "места");
}

TEST_CASE("random edits keep the patched catalog equal to a rebuilt one", "[catalog][patch]") {
    std::mt19937 rng(20261124);
    const auto pick = [&](std::size_t n) {
        return std::uniform_int_distribution<std::size_t>(0, n - 1)(rng);
    };
    static constexpr std::array kNames{"а", "Б", "в", "ё", "е", "Zeta", "alpha", "Ю", "x"};
    Model model;
    domain::EntryId next = 1;
    const auto anyFolder = [&]() -> std::optional<domain::EntryId> {
        std::vector<domain::EntryId> folders;
        for (const auto& [id, e] : model) {
            if (e.isFolder()) {
                folders.push_back(id);
            }
        }
        if (folders.empty() || pick(4) == 0) {
            return std::nullopt;
        }
        return folders[pick(folders.size())];
    };
    const auto anyKind = [&] { return pick(3) == 0 ? domain::Kind::File : domain::Kind::Folder; };
    for (int i = 0; i < 40; ++i) {
        const auto id = next++;
        model[id] = entry(id, anyFolder(), anyKind(), kNames[pick(kNames.size())]);
    }
    auto catalog = rebuilt(model);

    for (int round = 0; round < 200; ++round) {
        // Пачка из нескольких правок между чтениями, как копит сессия.
        std::set<domain::EntryId> touched;
        const auto steps = 1 + pick(4);
        for (std::size_t step = 0; step < steps; ++step) {
            const auto op = pick(4);
            if (op == 0 || model.empty()) { // создание
                const auto id = next++;
                model[id] = entry(id, anyFolder(), anyKind(), kNames[pick(kNames.size())]);
                touched.insert(id);
                continue;
            }
            auto it = model.begin();
            std::advance(it, static_cast<std::ptrdiff_t>(pick(model.size())));
            const auto id = it->first;
            if (op == 1) { // переименование
                it->second.name = kNames[pick(kNames.size())];
            } else if (op == 2) { // перенос, но не в себя и не в своего потомка
                const auto target = anyFolder();
                bool cycle = false;
                for (auto at = target; at && !cycle; at = model.at(*at).parentId) {
                    cycle = *at == id;
                }
                if (!cycle) {
                    it->second.parentId = target;
                }
            } else { // удаление поддерева
                eraseFrom(model, id);
            }
            touched.insert(id);
        }
        std::vector<domain::Entry> present;
        std::vector<domain::EntryId> gone;
        std::unordered_set<domain::EntryId> keep;
        for (const auto id : touched) {
            if (const auto found = model.find(id); found != model.end()) {
                present.push_back(found->second);
                keep.insert(id);
            } else {
                gone.push_back(id);
            }
        }
        catalog->erase(gone, keep);
        REQUIRE(catalog->upsert(std::move(present)));
        INFO("раунд " << round);
        requireSame(*catalog, model);
    }
}

// Сессия: изменения копятся и применяются следующим чтением.

namespace {

struct SessionFixture {
    SessionFixture() : crypto(infra::makeSodiumCrypto()) {
        SessionKeys keys;
        keys.master = crypto->generateKey().value();
        keys.names = crypto->deriveSubkey(keys.master, domain::KeyPurpose::Names).value();
        keys.content = crypto->deriveSubkey(keys.master, domain::KeyPurpose::Content).value();
        keys.thumbnails = crypto->deriveSubkey(keys.master, domain::KeyPurpose::Thumbnails).value();
        session = std::make_unique<VaultSession>(
            1, "test.safebox", domain::SafeMeta{}, std::make_shared<Sealer>(*crypto, keys),
            IdlePolicy(std::chrono::seconds(60), std::chrono::seconds(60), {}));
    }

    domain::Result<VaultSession::CatalogPtr> read() {
        return session->catalog(
            [this]() -> domain::Result<std::shared_ptr<Catalog>> {
                ++loads;
                return rebuilt(model);
            },
            [this](Catalog& catalog, const CatalogChanges& changes) -> domain::Result<bool> {
                ++patches;
                lastPatched = &catalog;
                if (failPatch) {
                    return false;
                }
                std::vector<domain::Entry> present;
                std::vector<domain::EntryId> gone;
                std::unordered_set<domain::EntryId> keep;
                for (const auto id : changes.entries) {
                    if (const auto found = model.find(id); found != model.end()) {
                        present.push_back(found->second);
                        keep.insert(id);
                    } else {
                        gone.push_back(id);
                    }
                }
                catalog.erase(gone, keep);
                return catalog.upsert(std::move(present));
            });
    }

    std::unique_ptr<domain::CryptoSuite> crypto;
    std::unique_ptr<VaultSession> session;
    Model model;
    int loads = 0;
    int patches = 0;
    bool failPatch = false;
    const Catalog* lastPatched = nullptr;
};

} // namespace

TEST_CASE("session patches the cached catalog instead of rebuilding it", "[catalog][session]") {
    SessionFixture f;
    f.model[1] = entry(1, std::nullopt, domain::Kind::Folder, "папка");
    const Catalog* first = nullptr;
    {
        auto cat = f.read();
        REQUIRE(cat.has_value());
        first = cat->get();
    }
    CHECK(f.loads == 1);

    f.model[2] = entry(2, 1, domain::Kind::File, "новый.txt");
    f.session->catalogChanged({.entries = {2}});
    auto cat = f.read();
    REQUIRE(cat.has_value());
    CHECK(f.loads == 1);
    CHECK(f.patches == 1);
    CHECK(cat->get() == first); // никто не держал снимок - правка на месте
    CHECK((*cat)->childCount(1) == 1);

    auto again = f.read(); // без изменений - тот же каталог без правки
    CHECK(f.patches == 1);
    CHECK(again->get() == first);
}

TEST_CASE("a held snapshot is not touched: the patch goes to a copy", "[catalog][session]") {
    SessionFixture f;
    f.model[1] = entry(1, std::nullopt, domain::Kind::Folder, "папка");
    auto held = f.read();
    REQUIRE(held.has_value());

    f.model[2] = entry(2, 1, domain::Kind::File, "новый.txt");
    f.session->catalogChanged({.entries = {2}});
    auto fresh = f.read();
    REQUIRE(fresh.has_value());
    CHECK(fresh->get() != held->get());
    CHECK(f.lastPatched == fresh->get());
    CHECK((*held)->find(2) == nullptr); // у читателя прежний снимок
    CHECK((*fresh)->find(2) != nullptr);
    CHECK(f.loads == 1);
}

TEST_CASE("a failed patch, invalidate and too many changes rebuild the catalog",
          "[catalog][session]") {
    SessionFixture f;
    f.model[1] = entry(1, std::nullopt, domain::Kind::Folder, "папка");
    REQUIRE(f.read().has_value());

    f.failPatch = true;
    f.model[2] = entry(2, 1, domain::Kind::File, "новый.txt");
    f.session->catalogChanged({.entries = {2}});
    auto cat = f.read();
    REQUIRE(cat.has_value());
    CHECK(f.loads == 2);
    CHECK((*cat)->find(2) != nullptr);
    f.failPatch = false;

    f.session->invalidateCatalog();
    REQUIRE(f.read().has_value());
    CHECK(f.loads == 3);

    CatalogChanges many;
    for (domain::EntryId id = 1000; id < 1000 + 20'001; ++id) {
        many.entries.push_back(id);
    }
    f.session->catalogChanged(many);
    const auto patchesBefore = f.patches;
    REQUIRE(f.read().has_value());
    CHECK(f.loads == 4);
    CHECK(f.patches == patchesBefore);
}

TEST_CASE("changes noted while there is no catalog are not replayed onto a fresh load",
          "[catalog][session]") {
    SessionFixture f;
    f.model[1] = entry(1, std::nullopt, domain::Kind::Folder, "папка");
    f.session->catalogChanged({.entries = {1}});
    REQUIRE(f.read().has_value());
    CHECK(f.loads == 1);
    CHECK(f.patches == 0);
    REQUIRE(f.read().has_value());
    CHECK(f.patches == 0);
}
