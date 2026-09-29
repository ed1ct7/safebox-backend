// TagsService: категории, теги, присвоения записям, слияние и удаление.
#include <thread>

#include "app_fixture.hpp"
#include "safebox/domain/model/rules.hpp"
#include "vault_session.hpp"

using namespace safebox;
using namespace safebox::test;
using Code = domain::Error::Code;
using Assigned = std::vector<domain::TagAssignment>;
using Names = std::vector<std::string>;

namespace {

std::vector<app::CategoryWithTags> listAll(AppFixture& f, const app::UnlockResult& s) {
    auto all = f.services.tags->list(f.lease(s));
    REQUIRE(all.has_value());
    return *all;
}

Names categoryNames(const std::vector<app::CategoryWithTags>& all) {
    Names out;
    for (const auto& category : all) {
        out.push_back(category.category.name);
    }
    return out;
}

Names tagNames(const app::CategoryWithTags& category) {
    Names out;
    for (const auto& item : category.tags) {
        out.push_back(item.tag.name);
    }
    return out;
}

// Сколько записей держат тег; nullopt - такого тега в списке нет.
std::optional<std::size_t> countOf(const std::vector<app::CategoryWithTags>& all,
                                   domain::TagId id) {
    for (const auto& category : all) {
        for (const auto& item : category.tags) {
            if (item.tag.id == id) {
                return item.count;
            }
        }
    }
    return std::nullopt;
}

domain::Entry entryOf(AppFixture& f, const app::UnlockResult& s, domain::EntryId id) {
    auto entry = f.services.entries->get(f.lease(s), id);
    REQUIRE(entry.has_value());
    return *entry;
}

Assigned tagsOf(AppFixture& f, const app::UnlockResult& s, domain::EntryId id) {
    return entryOf(f, s, id).meta.tags;
}

// Тысячи тегов через сервис создаются долго (после каждого пересобирается каталог), поэтому кладем
// их в хранилище сразу, запечатав ключами открытого сейфа. Все в одной категории "Все".
std::vector<domain::TagId> bulkTags(AppFixture& f, const app::UnlockResult& s, std::size_t count) {
    auto lease = f.lease(s);
    const auto sealer = lease.session()->sealer();
    REQUIRE(sealer != nullptr);
    std::vector<domain::TagId> ids;
    {
        auto uow = f.store.begin();
        REQUIRE(uow.has_value());
        auto& repo = (*uow)->tags();
        const auto category = repo.insertCategory().value();
        REQUIRE(
            repo.updateCategory(category, *sealer->sealCategoryName(category, "Все")).has_value());
        for (std::size_t i = 0; i < count; ++i) {
            const auto id = repo.insertTag(category).value();
            const auto name = "тег " + std::to_string(i);
            REQUIRE(
                repo.updateTag(id, category, *sealer->sealTagName(id, category, name)).has_value());
            ids.push_back(id);
        }
        REQUIRE((*uow)->commit().has_value());
    }
    lease.session()->invalidateCatalog();
    return ids;
}

// Закрыть сейф и открыть заново: все, что осталось, лежит в файле.
app::UnlockResult reopen(AppFixture& f) {
    f.services.safe->lock();
    auto again = f.services.safe->unlock({"test", "secret1"});
    REQUIRE(again.has_value());
    return *again;
}

} // namespace

TEST_CASE("categories are trimmed, unique without case and listed by folded name",
          "[tags][UF-17]") {
    AppFixture f;
    auto s = f.createSafe();
    auto& tags = *f.services.tags;

    SECTION("names") {
        auto made = tags.createCategory(f.lease(s), "  Люди ");
        REQUIRE(made.has_value());
        CHECK(made->name == "Люди");
        for (const char* dup : {"люди", "ЛЮДИ", " Люди"}) {
            INFO(dup);
            auto again = tags.createCategory(f.lease(s), dup);
            REQUIRE_FALSE(again.has_value());
            CHECK(again.error().code == Code::AlreadyExists);
        }
        REQUIRE(tags.createCategory(f.lease(s), "Ёлки").has_value());
        auto yo = tags.createCategory(f.lease(s), "елки"); // "ё" = "е"
        REQUIRE_FALSE(yo.has_value());
        CHECK(yo.error().code == Code::AlreadyExists);

        for (const auto& bad : Names{"", "   ", "a:b", "tab\there", std::string(101, 'x')}) {
            INFO(bad.size());
            auto refused = tags.createCategory(f.lease(s), bad);
            REQUIRE_FALSE(refused.has_value());
            CHECK(refused.error().code == Code::InvalidArgument);
        }
        std::string hundred; // 100 символов, а не 100 байт
        for (int i = 0; i < 100; ++i) {
            hundred += "ж";
        }
        CHECK(tags.createCategory(f.lease(s), hundred).has_value());
        CHECK(categoryNames(listAll(f, s)).size() == 3);
    }

    SECTION("rename") {
        auto people = tags.createCategory(f.lease(s), "Люди");
        auto places = tags.createCategory(f.lease(s), "Места");
        REQUIRE(people.has_value());
        REQUIRE(places.has_value());
        const auto anna = f.tag(s, "Люди", "Анна");
        REQUIRE(f.importFiles(s, {{"a.txt", "a"}}).imported == 1);
        REQUIRE(f.tagEntries(s, {f.entryNamed(s, "a.txt").id}, {anna}) == 1);

        auto renamed = tags.renameCategory(f.lease(s), people->id, " Персонажи ");
        REQUIRE(renamed.has_value());
        CHECK(renamed->category.id == people->id);
        CHECK(renamed->category.name == "Персонажи");
        REQUIRE(renamed->tags.size() == 1); // теги и счетчики едут с ответом
        CHECK(renamed->tags[0].tag.name == "Анна");
        CHECK(renamed->tags[0].count == 1);

        auto taken = tags.renameCategory(f.lease(s), people->id, "места");
        REQUIRE_FALSE(taken.has_value());
        CHECK(taken.error().code == Code::AlreadyExists);
        auto recased =
            tags.renameCategory(f.lease(s), people->id, "ПЕРСОНАЖИ"); // сама себе не дубль
        REQUIRE(recased.has_value());
        CHECK(recased->category.name == "ПЕРСОНАЖИ");
        auto same = tags.renameCategory(f.lease(s), places->id, "Места"); // ничего не меняется
        REQUIRE(same.has_value());
        CHECK(same->category.name == "Места");

        auto bad = tags.renameCategory(f.lease(s), people->id, "a:b");
        REQUIRE_FALSE(bad.has_value());
        CHECK(bad.error().code == Code::InvalidArgument);
        auto missing = tags.renameCategory(f.lease(s), 999, "Другая");
        REQUIRE_FALSE(missing.has_value());
        CHECK(missing.error().code == Code::NotFound);

        auto again = reopen(f);
        CHECK(categoryNames(listAll(f, again)) == Names{"Места", "ПЕРСОНАЖИ"});
    }

    SECTION("list order and persistence") {
        CHECK(listAll(f, s).empty());
        for (const char* name : {"Язык", "Люди", "Ёлки", "Аниме"}) {
            REQUIRE(tags.createCategory(f.lease(s), name).has_value());
        }
        // по folded-имени: "ё" сортируется как "е"
        CHECK(categoryNames(listAll(f, s)) == Names{"Аниме", "Ёлки", "Люди", "Язык"});
        for (const auto& category : listAll(f, s)) {
            CHECK(category.tags.empty());
        }
        auto again = reopen(f);
        CHECK(categoryNames(listAll(f, again)) == Names{"Аниме", "Ёлки", "Люди", "Язык"});
    }
}

TEST_CASE("createTag finds an existing tag, creates a category only on request", "[tags][UF-16]") {
    AppFixture f;
    auto s = f.createSafe();
    auto& tags = *f.services.tags;

    auto noCategory = tags.createTag(f.lease(s), {"Персонажи", "Eris", false});
    REQUIRE_FALSE(noCategory.has_value());
    CHECK(noCategory.error().code == Code::NotFound);
    CHECK(listAll(f, s).empty()); // отказ ничего не создал

    auto first = tags.createTag(f.lease(s), {" Персонажи ", " Eris Greyrat ", true});
    REQUIRE(first.has_value());
    CHECK(first->created);
    CHECK(first->tag.name == "Eris Greyrat");
    auto all = listAll(f, s);
    REQUIRE(all.size() == 1);
    CHECK(all[0].category.name == "Персонажи");
    CHECK(first->tag.categoryId == all[0].category.id);

    // категория уже есть: createCategory не нужен; тот же тег без учета регистра - он же
    auto other = tags.createTag(f.lease(s), {"персонажи", "Roxy", false});
    REQUIRE(other.has_value());
    CHECK(other->created);
    auto again = tags.createTag(f.lease(s), {"ПЕРСОНАЖИ", "ERIS GREYRAT", true});
    REQUIRE(again.has_value());
    CHECK_FALSE(again->created);
    CHECK(again->tag.id == first->tag.id);
    CHECK(again->tag.name == "Eris Greyrat"); // остается как записано
    CHECK(listAll(f, s).size() == 1);

    // то же имя в другой категории - другой тег
    auto elsewhere = tags.createTag(f.lease(s), {"Язык", "Roxy", true});
    REQUIRE(elsewhere.has_value());
    CHECK(elsewhere->created);
    CHECK(elsewhere->tag.id != other->tag.id);

    for (const auto& [category, name] :
         std::vector<std::pair<std::string, std::string>>{{"Персонажи", ""},
                                                          {"Персонажи", "a:b"},
                                                          {"Персонажи", std::string(101, 'x')},
                                                          {"", "Eris"},
                                                          {"Пер:сонажи", "Eris"},
                                                          {"Новая", "a:b"}}) {
        INFO(category << " / " << name.size());
        auto refused = tags.createTag(f.lease(s), {category, name, true});
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().code == Code::InvalidArgument);
    }
    CHECK(categoryNames(listAll(f, s)) == Names{"Персонажи", "Язык"}); // "Новая" не появилась

    auto reopened = reopen(f);
    auto stored = listAll(f, reopened);
    REQUIRE(stored.size() == 2);
    CHECK(tagNames(stored[0]) == Names{"Eris Greyrat", "Roxy"});
    CHECK(tagNames(stored[1]) == Names{"Roxy"});
}

TEST_CASE("updateTag renames and moves a tag, refusing duplicates", "[tags][UF-17]") {
    AppFixture f;
    auto s = f.createSafe();
    auto& tags = *f.services.tags;
    const auto eris = f.tag(s, "Персонажи", "Eris");
    const auto roxy = f.tag(s, "Персонажи", "Roxy");
    const auto ru = f.tag(s, "Язык", "Eris"); // то же имя в другой категории
    const auto all = listAll(f, s);
    const auto characters = all[0].category.id; // "Персонажи" < "Язык"
    const auto language = all[1].category.id;

    auto taken = tags.updateTag(f.lease(s), eris, {.name = "roxy"});
    REQUIRE_FALSE(taken.has_value());
    CHECK(taken.error().code == Code::AlreadyExists);
    auto recased = tags.updateTag(f.lease(s), eris, {.name = " ERIS "}); // сам себе не дубль
    REQUIRE(recased.has_value());
    CHECK(recased->name == "ERIS");
    CHECK(recased->categoryId == characters);
    auto same = tags.updateTag(f.lease(s), roxy, {.name = "Roxy", .categoryId = characters});
    REQUIRE(same.has_value()); // ничего не меняется
    CHECK(same->name == "Roxy");

    auto clash = tags.updateTag(f.lease(s), ru, {.categoryId = characters}); // "Eris" уже там
    REQUIRE_FALSE(clash.has_value());
    CHECK(clash.error().code == Code::AlreadyExists);
    auto moved = tags.updateTag(f.lease(s), roxy, {.categoryId = language});
    REQUIRE(moved.has_value());
    CHECK(moved->categoryId == language);
    CHECK(moved->name == "Roxy");
    auto both = tags.updateTag(f.lease(s), eris, {.name = "Zed", .categoryId = language});
    REQUIRE(both.has_value());
    CHECK(both->name == "Zed");

    auto after = listAll(f, s);
    CHECK(tagNames(after[0]).empty());
    CHECK(tagNames(after[1]) == Names{"Eris", "Roxy", "Zed"});

    auto nothing = tags.updateTag(f.lease(s), eris, {});
    REQUIRE_FALSE(nothing.has_value());
    CHECK(nothing.error().code == Code::InvalidArgument);
    auto badName = tags.updateTag(f.lease(s), eris, {.name = "a:b"});
    REQUIRE_FALSE(badName.has_value());
    CHECK(badName.error().code == Code::InvalidArgument);
    auto noTag = tags.updateTag(f.lease(s), 999, {.name = "x"});
    REQUIRE_FALSE(noTag.has_value());
    CHECK(noTag.error().code == Code::NotFound);
    auto noCategory = tags.updateTag(f.lease(s), eris, {.categoryId = 999});
    REQUIRE_FALSE(noCategory.has_value());
    CHECK(noCategory.error().code == Code::NotFound);

    // имя запечатано вместе с категорией: после переноса сейф открывается и читается
    auto reopened = reopen(f);
    auto stored = listAll(f, reopened);
    REQUIRE(stored.size() == 2);
    CHECK(tagNames(stored[1]) == Names{"Eris", "Roxy", "Zed"});
    CHECK(stored[1].tags[1].tag.id == roxy);
}

TEST_CASE("assign adds, updates inherit and removes tags on several entries", "[tags][UF-16]") {
    AppFixture f;
    auto s = f.createSafe();
    REQUIRE(f.importFiles(s, {{"a.txt", "a"}, {"b.txt", "b"}, {"c.txt", "c"}}).imported == 3);
    const auto a = f.entryNamed(s, "a.txt");
    const auto b = f.entryNamed(s, "b.txt");
    const auto c = f.entryNamed(s, "c.txt");
    const auto anna = f.tag(s, "Люди", "Анна");
    const auto boris = f.tag(s, "Люди", "Борис");
    const auto crimea = f.tag(s, "Место", "Крым");
    auto& tags = *f.services.tags;
    f.clock.advance(std::chrono::seconds(5));

    CHECK(f.tagEntries(s, {a.id, b.id, a.id}, {anna, crimea}) == 2); // a дважды - одна запись
    CHECK(tagsOf(f, s, a.id) == Assigned{{anna, false}, {crimea, false}});
    CHECK(tagsOf(f, s, b.id) == Assigned{{anna, false}, {crimea, false}});
    CHECK(tagsOf(f, s, c.id).empty());
    CHECK(entryOf(f, s, a.id).meta.modifiedAt == a.meta.modifiedAt); // тег - не содержимое
    CHECK(f.tagEntries(s, {a.id, b.id}, {anna, crimea}) == 0);       // уже стоят

    CHECK(f.tagEntries(s, {a.id, c.id}, {anna}, true) == 2); // a: обновился inherit, c: новый
    CHECK(tagsOf(f, s, a.id) == Assigned{{anna, true}, {crimea, false}});
    CHECK(tagsOf(f, s, c.id) == Assigned{{anna, true}});
    CHECK(tagsOf(f, s, b.id) == Assigned{{anna, false}, {crimea, false}});

    auto counts = listAll(f, s);
    CHECK(countOf(counts, anna) == 3);
    CHECK(countOf(counts, crimea) == 2);
    CHECK(countOf(counts, boris) == 0);

    // снять и поставить в одном запросе: сначала remove, потом add
    auto swapped =
        tags.assign(f.lease(s), {{a.id, b.id}, {{boris, false}, {crimea, true}}, {anna, crimea}});
    REQUIRE(swapped.has_value());
    CHECK(*swapped == 2);
    CHECK(tagsOf(f, s, a.id) == Assigned{{boris, false}, {crimea, true}});
    CHECK(tagsOf(f, s, b.id) == Assigned{{boris, false}, {crimea, true}});

    auto removed = tags.assign(f.lease(s), {{a.id, b.id, c.id}, {}, {boris}});
    REQUIRE(removed.has_value());
    CHECK(*removed == 2);
    CHECK(tagsOf(f, s, a.id) == Assigned{{crimea, true}});
    CHECK(tagsOf(f, s, c.id) == Assigned{{anna, true}});

    // пустое - ничего не меняет
    auto none = tags.assign(f.lease(s), {{a.id}, {}, {}});
    REQUIRE(none.has_value());
    CHECK(*none == 0);
    auto noEntries = tags.assign(f.lease(s), {{}, {{anna, false}}, {}});
    REQUIRE(noEntries.has_value());
    CHECK(*noEntries == 0);

    auto again = reopen(f);
    CHECK(tagsOf(f, again, a.id) == Assigned{{crimea, true}});
    CHECK(tagsOf(f, again, b.id) == Assigned{{crimea, true}});
    CHECK(tagsOf(f, again, c.id) == Assigned{{anna, true}});
}

TEST_CASE("assign refuses unknown tags and entries and changes nothing", "[tags][UF-16]") {
    AppFixture f;
    auto s = f.createSafe();
    REQUIRE(f.importFiles(s, {{"a.txt", "a"}, {"b.txt", "b"}}).imported == 2);
    const auto a = f.entryNamed(s, "a.txt");
    const auto b = f.entryNamed(s, "b.txt");
    const auto anna = f.tag(s, "Люди", "Анна");
    auto& tags = *f.services.tags;
    REQUIRE(f.tagEntries(s, {a.id}, {anna}) == 1);

    auto unknownAdd = tags.assign(f.lease(s), {{a.id, b.id}, {{anna, true}, {999, false}}, {}});
    REQUIRE_FALSE(unknownAdd.has_value());
    CHECK(unknownAdd.error().code == Code::InvalidArgument);
    auto unknownRemove = tags.assign(f.lease(s), {{a.id}, {}, {999}});
    REQUIRE_FALSE(unknownRemove.has_value());
    CHECK(unknownRemove.error().code == Code::InvalidArgument);
    auto unknownEntry = tags.assign(f.lease(s), {{b.id, 424242}, {{anna, true}}, {}});
    REQUIRE_FALSE(unknownEntry.has_value());
    CHECK(unknownEntry.error().code == Code::NotFound);

    CHECK(tagsOf(f, s, a.id) == Assigned{{anna, false}}); // inherit не обновился
    CHECK(tagsOf(f, s, b.id).empty());
}

TEST_CASE("an entry holds at most 1000 tags", "[tags][UF-16]") {
    AppFixture f;
    auto s = f.createSafe();
    REQUIRE(f.importFiles(s, {{"a.txt", "a"}, {"b.txt", "b"}}).imported == 2);
    const auto a = f.entryNamed(s, "a.txt");
    const auto b = f.entryNamed(s, "b.txt");
    const auto ids = bulkTags(f, s, domain::kMaxTagsPerEntry + 1);
    const std::vector<domain::TagId> first(ids.begin(), ids.begin() + 1000);

    auto& tags = *f.services.tags;
    app::AssignTagsCmd tooMany;
    tooMany.ids = {a.id};
    for (const auto id : ids) {
        tooMany.add.push_back({id, false});
    }
    auto refused = tags.assign(f.lease(s), tooMany);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == Code::InvalidArgument);
    CHECK(tagsOf(f, s, a.id).empty());

    CHECK(f.tagEntries(s, {a.id}, first) == 1); // ровно 1000 - можно
    CHECK(tagsOf(f, s, a.id).size() == 1000);
    auto over = tags.assign(f.lease(s), {{b.id, a.id}, {{ids.back(), false}}, {}});
    REQUIRE_FALSE(over.has_value()); // на a уже 1000: отказ откатывает и правку b
    CHECK(over.error().code == Code::InvalidArgument);
    CHECK(tagsOf(f, s, a.id).size() == 1000);
    CHECK(tagsOf(f, s, b.id).empty());

    // обновить inherit у уже стоящих можно: их число не растет
    CHECK(f.tagEntries(s, {a.id}, first, true) == 1);
    // и заменить один тег другим
    auto swap = tags.assign(f.lease(s), {{a.id}, {{ids.back(), false}}, {first.front()}});
    REQUIRE(swap.has_value());
    CHECK(*swap == 1);
    CHECK(tagsOf(f, s, a.id).size() == 1000);
}

TEST_CASE("removeTag takes the tag off every entry, inherited or not", "[tags][UF-17]") {
    AppFixture f;
    auto s = f.createSafe();
    REQUIRE(f.importFiles(s, {{"Папка/x.txt", "x"}, {"a.txt", "a"}, {"b.txt", "b"}}).imported == 3);
    const auto folder = f.entryNamed(s, "Папка");
    const auto x = f.entryNamed(s, "x.txt", folder.id);
    const auto a = f.entryNamed(s, "a.txt");
    const auto b = f.entryNamed(s, "b.txt");
    const auto doomed = f.tag(s, "Люди", "Анна");
    const auto kept = f.tag(s, "Люди", "Борис");
    REQUIRE(f.tagEntries(s, {folder.id}, {doomed}, true) == 1);
    REQUIRE(f.tagEntries(s, {a.id, b.id}, {doomed}) == 2);
    REQUIRE(f.tagEntries(s, {a.id}, {kept}) == 1);
    CHECK(entryOf(f, s, x.id).inheritedTags.size() == 1);
    auto& tags = *f.services.tags;

    auto removed = tags.removeTag(f.lease(s), doomed);
    REQUIRE(removed.has_value());
    CHECK(*removed == 3); // папка, a и b
    CHECK(tagsOf(f, s, folder.id).empty());
    CHECK(tagsOf(f, s, a.id) == Assigned{{kept, false}});
    CHECK(tagsOf(f, s, b.id).empty());
    CHECK(entryOf(f, s, x.id).inheritedTags.empty());
    auto all = listAll(f, s);
    CHECK_FALSE(countOf(all, doomed).has_value());
    CHECK(countOf(all, kept) == 1);
    CHECK(entryOf(f, s, a.id).meta.modifiedAt == a.meta.modifiedAt);

    auto twice = tags.removeTag(f.lease(s), doomed);
    REQUIRE_FALSE(twice.has_value());
    CHECK(twice.error().code == Code::NotFound);
    // тег без записей тоже удаляется
    auto unused = tags.removeTag(f.lease(s), kept);
    REQUIRE(unused.has_value());
    CHECK(*unused == 1);
    auto lonely = tags.createTag(f.lease(s), {"Люди", "Вера", false});
    REQUIRE(lonely.has_value());
    auto none = tags.removeTag(f.lease(s), lonely->tag.id);
    REQUIRE(none.has_value());
    CHECK(*none == 0);

    auto again = reopen(f);
    CHECK(tagsOf(f, again, a.id).empty());
    CHECK(listAll(f, again)[0].tags.empty());
}

TEST_CASE("removeCategory removes its tags from the entries", "[tags][UF-17]") {
    AppFixture f;
    auto s = f.createSafe();
    REQUIRE(f.importFiles(s, {{"a.txt", "a"}, {"b.txt", "b"}, {"c.txt", "c"}}).imported == 3);
    const auto a = f.entryNamed(s, "a.txt");
    const auto b = f.entryNamed(s, "b.txt");
    const auto c = f.entryNamed(s, "c.txt");
    const auto anna = f.tag(s, "Люди", "Анна");
    const auto boris = f.tag(s, "Люди", "Борис");
    const auto crimea = f.tag(s, "Место", "Крым");
    REQUIRE(f.tagEntries(s, {a.id}, {anna, crimea}) == 1);
    REQUIRE(f.tagEntries(s, {b.id}, {boris}, true) == 1);
    REQUIRE(f.tagEntries(s, {c.id}, {crimea}) == 1);
    auto& tags = *f.services.tags;
    const auto before = listAll(f, s);
    REQUIRE(categoryNames(before) == Names{"Люди", "Место"});
    const auto peopleId = before[0].category.id;

    auto removed = tags.removeCategory(f.lease(s), peopleId);
    REQUIRE(removed.has_value());
    CHECK(removed->removedTags == 2);
    CHECK(removed->affectedEntries == 2); // a и b
    CHECK(tagsOf(f, s, a.id) == Assigned{{crimea, false}});
    CHECK(tagsOf(f, s, b.id).empty());
    CHECK(tagsOf(f, s, c.id) == Assigned{{crimea, false}});
    auto all = listAll(f, s);
    REQUIRE(all.size() == 1);
    CHECK(all[0].category.name == "Место");
    CHECK(countOf(all, crimea) == 2);

    auto twice = tags.removeCategory(f.lease(s), peopleId);
    REQUIRE_FALSE(twice.has_value());
    CHECK(twice.error().code == Code::NotFound);

    // пустая категория
    auto empty = tags.createCategory(f.lease(s), "Пусто");
    REQUIRE(empty.has_value());
    auto none = tags.removeCategory(f.lease(s), empty->id);
    REQUIRE(none.has_value());
    CHECK(none->removedTags == 0);
    CHECK(none->affectedEntries == 0);

    auto again = reopen(f); // каталог читается: ни висячих тегов, ни висячих присвоений
    CHECK(tagsOf(f, again, a.id) == Assigned{{crimea, false}});
    CHECK(categoryNames(listAll(f, again)) == Names{"Место"});
}

TEST_CASE("mergeTag moves the assignments over with inherit = a || b", "[tags][UF-17]") {
    AppFixture f;
    auto s = f.createSafe();
    REQUIRE(
        f.importFiles(
             s,
             {{"e1.txt", "1"}, {"e2.txt", "2"}, {"e3.txt", "3"}, {"e4.txt", "4"}, {"e5.txt", "5"}})
            .imported == 5);
    const auto e1 = f.entryNamed(s, "e1.txt").id;
    const auto e2 = f.entryNamed(s, "e2.txt").id;
    const auto e3 = f.entryNamed(s, "e3.txt").id;
    const auto e4 = f.entryNamed(s, "e4.txt").id;
    const auto e5 = f.entryNamed(s, "e5.txt").id;
    const auto from = f.tag(s, "Люди", "Ирис");
    const auto into = f.tag(s, "Персонажи", "Eris"); // другая категория - можно
    auto& tags = *f.services.tags;

    REQUIRE(f.tagEntries(s, {e1, e3}, {from}, true) == 2);
    REQUIRE(f.tagEntries(s, {e2, e4}, {from}) == 2);
    REQUIRE(f.tagEntries(s, {e2, e5}, {into}, true) == 2); // e2 - оба, e5 - только into
    REQUIRE(f.tagEntries(s, {e3, e4}, {into}) == 2);

    auto merged = tags.mergeTag(f.lease(s), from, into);
    REQUIRE(merged.has_value());
    CHECK(*merged == 4);                                // e5 не в счет: у нее from не было
    CHECK(tagsOf(f, s, e1) == Assigned{{into, true}});  // только from (inherit)
    CHECK(tagsOf(f, s, e2) == Assigned{{into, true}});  // оба: false || true
    CHECK(tagsOf(f, s, e3) == Assigned{{into, true}});  // from inherit + into без: true
    CHECK(tagsOf(f, s, e4) == Assigned{{into, false}}); // false || false
    CHECK(tagsOf(f, s, e5) == Assigned{{into, true}});  // не тронута
    auto all = listAll(f, s);
    CHECK_FALSE(countOf(all, from).has_value());
    CHECK(countOf(all, into) == 5);

    auto self = tags.mergeTag(f.lease(s), into, into);
    REQUIRE_FALSE(self.has_value());
    CHECK(self.error().code == Code::InvalidArgument);
    auto noFrom = tags.mergeTag(f.lease(s), from, into); // from уже нет
    REQUIRE_FALSE(noFrom.has_value());
    CHECK(noFrom.error().code == Code::NotFound);
    auto noInto = tags.mergeTag(f.lease(s), into, 999);
    REQUIRE_FALSE(noInto.has_value());
    CHECK(noInto.error().code == Code::NotFound);
    CHECK(tagsOf(f, s, e1) == Assigned{{into, true}}); // отказы ничего не тронули

    auto again = reopen(f);
    CHECK(tagsOf(f, again, e2) == Assigned{{into, true}});
    CHECK(tagsOf(f, again, e4) == Assigned{{into, false}});
    CHECK(countOf(listAll(f, again), into) == 5);
}

TEST_CASE("inherited tags show up on descendants and follow moves", "[tags][UF-16]") {
    AppFixture f;
    auto s = f.createSafe();
    REQUIRE(f.importFiles(s, {{"Папка/Вложенная/x.txt", "x"}, {"y.txt", "y"}}).imported == 2);
    const auto folder = f.entryNamed(s, "Папка");
    const auto inner = f.entryNamed(s, "Вложенная", folder.id);
    const auto x = f.entryNamed(s, "x.txt", inner.id);
    const auto y = f.entryNamed(s, "y.txt");
    const auto crimea = f.tag(s, "Место", "Крым");
    const auto quiet = f.tag(s, "Место", "Тихо");
    REQUIRE(f.tagEntries(s, {folder.id}, {crimea}, true) == 1);
    REQUIRE(f.tagEntries(s, {folder.id}, {quiet}, false) == 1); // без inherit вниз не идет

    CHECK(entryOf(f, s, x.id).inheritedTags ==
          std::vector<domain::InheritedTag>{{crimea, folder.id}});
    CHECK(entryOf(f, s, y.id).inheritedTags.empty());
    auto counts = listAll(f, s);
    CHECK(countOf(counts, crimea) == 1); // счетчик - только прямые присвоения

    // перенос в папку с тегом - унаследует, из нее - потеряет
    REQUIRE(f.services.entries->move(f.lease(s), {{y.id}, folder.id, {}}).has_value());
    CHECK(entryOf(f, s, y.id).inheritedTags ==
          std::vector<domain::InheritedTag>{{crimea, folder.id}});
    REQUIRE(f.services.entries->move(f.lease(s), {{y.id}, std::nullopt, {}}).has_value());
    CHECK(entryOf(f, s, y.id).inheritedTags.empty());
}

TEST_CASE("tag operations refuse to work after lock", "[tags][UF-13]") {
    AppFixture f;
    auto s = f.createSafe();
    REQUIRE(f.importFiles(s, {{"a.txt", "a"}}).imported == 1);
    const auto a = f.entryNamed(s, "a.txt");
    const auto anna = f.tag(s, "Люди", "Анна");
    auto held = f.lease(s);
    std::thread locker([&] { f.services.safe->lock(); });
    for (int i = 0; i < 200 && !held.cancelled(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    auto& tags = *f.services.tags;
    auto listed = tags.list(held);
    REQUIRE_FALSE(listed.has_value());
    CHECK(listed.error().code == Code::Locked);
    auto made = tags.createCategory(held, "Новая");
    REQUIRE_FALSE(made.has_value());
    CHECK(made.error().code == Code::Locked);
    auto assigned = tags.assign(held, {{a.id}, {{anna, false}}, {}});
    REQUIRE_FALSE(assigned.has_value());
    CHECK(assigned.error().code == Code::Locked);
    auto found = f.services.search->search(held, {.tags = {anna}});
    REQUIRE_FALSE(found.has_value());
    CHECK(found.error().code == Code::Locked);
    {
        auto release = std::move(held);
    }
    locker.join();
    CHECK_FALSE(f.store.isOpen());
}
