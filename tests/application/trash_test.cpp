// Корзина: удаление ставит trashedAt корню поддерева, записи пропадают из списков, поиска,
// счетчиков тегов и дедупа; восстановление возвращает на место; стирание - навсегда.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>

#include "app_fixture.hpp"

using namespace safebox;
using namespace safebox::test;
using Code = domain::Error::Code;

namespace {

std::vector<std::string> rootNames(AppFixture& f, const app::UnlockResult& s,
                                   std::optional<domain::EntryId> parent = std::nullopt) {
    auto listing = f.services.entries->list(f.lease(s), parent);
    REQUIRE(listing.has_value());
    std::vector<std::string> names;
    for (const auto& e : listing->entries) {
        names.push_back(e.name);
    }
    return names;
}

std::vector<domain::EntryId> trashIds(AppFixture& f, const app::UnlockResult& s,
                                      app::ListOptions options = {}) {
    auto trash = f.services.trash->list(f.lease(s), options);
    REQUIRE(trash.has_value());
    CHECK(trash->total >= trash->entries.size());
    std::vector<domain::EntryId> ids;
    for (const auto& item : trash->entries) {
        CHECK(item.entry.meta.trashedAt.has_value());
        ids.push_back(item.entry.id);
    }
    return ids;
}

std::size_t removeIds(AppFixture& f, const app::UnlockResult& s, std::vector<domain::EntryId> ids) {
    auto removed = f.services.entries->remove(f.lease(s), ids);
    REQUIRE(removed.has_value());
    return *removed;
}

} // namespace

TEST_CASE("trashed entries leave listings, the folder tree, search and tag counts", "[trash]") {
    AppFixture f;
    auto s = f.createSafe();
    REQUIRE(f.importFiles(s, {{"Отпуск/море.txt", "sea"},
                              {"Отпуск/Горы/вершина.txt", "top"},
                              {"море.txt", "другое"},
                              {"keep.txt", "k"}})
                .imported == 4);
    const auto trip = f.entryNamed(s, "Отпуск");
    const auto mountains = f.entryNamed(s, "Горы", trip.id);
    const auto keep = f.entryNamed(s, "keep.txt");
    const auto tag = f.tag(s, "Место", "Сочи");
    f.tagEntries(s, {trip.id, keep.id}, {tag});

    f.clock.advance(std::chrono::seconds(5));
    CHECK(removeIds(f, s, {trip.id}) == 4); // Отпуск, море.txt, Горы, вершина.txt
    CHECK(removeIds(f, s, {trip.id}) == 0); // уже в корзине - не считается

    CHECK(rootNames(f, s) == std::vector<std::string>{"keep.txt", "море.txt"});
    auto trash = f.services.trash->list(f.lease(s), {});
    REQUIRE(trash.has_value());
    REQUIRE(trash->total == 1);
    CHECK(trash->entries[0].entry.id == trip.id);
    CHECK(trash->entries[0].location.empty()); // удалена из корня
    CHECK(trash->entries[0].entry.childCount == 2);
    // внутри папки в корзине можно ходить
    CHECK(rootNames(f, s, trip.id) == std::vector<std::string>{"Горы", "море.txt"});
    auto inside = f.services.entries->get(f.lease(s), mountains.id);
    REQUIRE(inside.has_value());
    CHECK_FALSE(inside->meta.trashedAt.has_value()); // метка - только у корня

    auto folders = f.services.entries->folders(f.lease(s));
    REQUIRE(folders.has_value());
    CHECK(folders->empty());

    auto found = f.services.search->search(f.lease(s), {.text = "море"});
    REQUIRE(found.has_value());
    REQUIRE(found->total == 1);
    CHECK(found->hits[0].entry.name == "море.txt");
    CHECK(found->hits[0].entry.parentId == std::nullopt);
    // поиск внутри папки из корзины - по ее содержимому
    auto within = f.services.search->search(f.lease(s), {.text = "вершина", .within = trip.id});
    REQUIRE(within.has_value());
    CHECK(within->total == 1);

    auto tags = f.services.tags->list(f.lease(s));
    REQUIRE(tags.has_value());
    REQUIRE(tags->size() == 1);
    CHECK((*tags)[0].tags[0].count == 1); // Отпуск в корзине не считается

    // после блокировки (каталог строится заново) - то же самое
    f.services.safe->lock();
    auto again = f.services.safe->unlock({"test", "secret1"});
    REQUIRE(again.has_value());
    CHECK(rootNames(f, *again) == std::vector<std::string>{"keep.txt", "море.txt"});
    CHECK(trashIds(f, *again) == std::vector<domain::EntryId>{trip.id});
}

TEST_CASE("trash lists newest first by default order, with the place each came from", "[trash]") {
    AppFixture f;
    auto s = f.createSafe();
    REQUIRE(f.importFiles(s, {{"A/a.txt", "a"}, {"A/b.txt", "b"}, {"c.txt", "c"}}).imported == 3);
    const auto folder = f.entryNamed(s, "A");
    const auto a = f.entryNamed(s, "a.txt", folder.id);
    const auto b = f.entryNamed(s, "b.txt", folder.id);
    const auto c = f.entryNamed(s, "c.txt");

    CHECK(removeIds(f, s, {b.id}) == 1);
    f.clock.advance(std::chrono::seconds(1));
    CHECK(removeIds(f, s, {c.id, a.id}) == 2);

    const app::ListOptions newest{.sort = app::SortKey::Trashed, .descending = true};
    const auto order = trashIds(f, s, newest);
    REQUIRE(order.size() == 3);
    CHECK(order.back() == b.id);                                             // удалена раньше всех
    CHECK(trashIds(f, s) == std::vector<domain::EntryId>{a.id, b.id, c.id}); // по имени
    CHECK(trashIds(f, s, {.offset = 1, .limit = 1}) == std::vector<domain::EntryId>{b.id});

    auto trash = f.services.trash->list(f.lease(s), {});
    REQUIRE(trash.has_value());
    REQUIRE(trash->entries[0].location.size() == 1);
    CHECK(trash->entries[0].location[0].id == folder.id);
    CHECK(trash->entries[0].location[0].name == "A");
}

TEST_CASE("restore brings entries back to their folder, renaming on a taken name", "[trash]") {
    AppFixture f;
    auto s = f.createSafe();
    REQUIRE(f.importFiles(s, {{"D/x.txt", "old"}, {"D/y.txt", "y"}}).imported == 2);
    const auto dest = f.entryNamed(s, "D");
    const auto x = f.entryNamed(s, "x.txt", dest.id);
    const auto y = f.entryNamed(s, "y.txt", dest.id);
    CHECK(removeIds(f, s, {x.id, y.id}) == 2);
    // на месте удаленной появилась новая с тем же именем - корзина имени не держит
    REQUIRE(f.importFiles(s, {{"X.TXT", "new"}}, dest.id).imported == 1);

    const domain::EntryId ids[] = {x.id, y.id};
    auto restored = f.services.trash->restore(f.lease(s), ids);
    REQUIRE(restored.has_value());
    CHECK(restored->restored == 2);
    REQUIRE(restored->renamed.size() == 1);
    CHECK(restored->renamed[0].id == x.id);
    CHECK(restored->renamed[0].name == "x (2).txt");
    auto names = rootNames(f, s, dest.id);
    std::sort(names.begin(), names.end());
    CHECK(names == std::vector<std::string>{"X.TXT", "x (2).txt", "y.txt"});
    CHECK(f.readContent(s, x.id) == "old");
    CHECK(trashIds(f, s).empty());
    auto back = f.services.entries->get(f.lease(s), x.id);
    REQUIRE(back.has_value());
    CHECK_FALSE(back->meta.trashedAt.has_value());

    // не в корзине -> NotFound, ничего не меняется
    auto again = f.services.trash->restore(f.lease(s), ids);
    REQUIRE_FALSE(again.has_value());
    CHECK(again.error().code == Code::NotFound);
}

TEST_CASE("an entry trashed before its folder waits for the folder to come back", "[trash]") {
    AppFixture f;
    auto s = f.createSafe();
    REQUIRE(f.importFiles(s, {{"F/inner.txt", "i"}, {"F/other.txt", "o"}}).imported == 2);
    const auto folder = f.entryNamed(s, "F");
    const auto inner = f.entryNamed(s, "inner.txt", folder.id);
    CHECK(removeIds(f, s, {inner.id}) == 1);
    CHECK(removeIds(f, s, {folder.id}) == 2); // F и other.txt; inner уже в корзине
    auto both = trashIds(f, s);
    std::sort(both.begin(), both.end());
    CHECK(both == std::vector<domain::EntryId>{std::min(folder.id, inner.id),
                                               std::max(folder.id, inner.id)});

    const domain::EntryId innerOnly[] = {inner.id};
    auto refused = f.services.trash->restore(f.lease(s), innerOnly);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == Code::InvalidArgument);

    const domain::EntryId folderOnly[] = {folder.id};
    REQUIRE(f.services.trash->restore(f.lease(s), folderOnly).has_value());
    CHECK(rootNames(f, s, folder.id) == std::vector<std::string>{"other.txt"});
    REQUIRE(f.services.trash->restore(f.lease(s), innerOnly).has_value());
    CHECK(rootNames(f, s, folder.id) == std::vector<std::string>{"inner.txt", "other.txt"});
}

TEST_CASE("purging a folder takes the entries trashed inside it too", "[trash]") {
    AppFixture f;
    auto s = f.createSafe();
    REQUIRE(f.importFiles(s, {{"F/inner.txt", "i"}, {"F/sub/deep.txt", "d"}, {"keep.txt", "k"}})
                .imported == 3);
    const auto folder = f.entryNamed(s, "F");
    const auto sub = f.entryNamed(s, "sub", folder.id);
    const auto inner = f.entryNamed(s, "inner.txt", folder.id);
    CHECK(removeIds(f, s, {inner.id}) == 1);
    CHECK(removeIds(f, s, {sub.id}) == 2);
    CHECK(removeIds(f, s, {folder.id}) == 1);
    CHECK(trashIds(f, s).size() == 3);

    const auto blobs = f.store.blobCount(f.safePath());
    auto purged = f.services.trash->purge(f.lease(s), {.ids = {folder.id}});
    REQUIRE(purged.has_value());
    CHECK(*purged == 4);           // F, inner.txt, sub, deep.txt
    CHECK(trashIds(f, s).empty()); // каталог правится на месте: вложенные корни ушли с папкой
    CHECK(f.store.blobCount(f.safePath()) == blobs - 2);
    CHECK(f.services.entries->get(f.lease(s), inner.id).error().code == Code::NotFound);
    CHECK(rootNames(f, s) == std::vector<std::string>{"keep.txt"});

    f.services.safe->lock();
    auto again = f.services.safe->unlock({"test", "secret1"});
    REQUIRE(again.has_value());
    CHECK(trashIds(f, *again).empty());
    CHECK(rootNames(f, *again) == std::vector<std::string>{"keep.txt"});
}

TEST_CASE("purge takes ids from the trash, everything older than a moment, or all", "[trash]") {
    AppFixture f;
    auto s = f.createSafe();
    REQUIRE(f.importFiles(s, {{"a.txt", "a"}, {"b.txt", "b"}, {"c.txt", "c"}, {"F/d.txt", "d"}})
                .imported == 4);
    const auto a = f.entryNamed(s, "a.txt");
    const auto b = f.entryNamed(s, "b.txt");
    const auto c = f.entryNamed(s, "c.txt");
    const auto folder = f.entryNamed(s, "F");
    const auto d = f.entryNamed(s, "d.txt", folder.id);

    CHECK(removeIds(f, s, {a.id}) == 1);
    f.clock.advance(std::chrono::seconds(2));
    const auto cut = domain::toUnixMillis(f.clock.now());
    CHECK(removeIds(f, s, {b.id, folder.id}) == 3);

    // не одно из трех, не из корзины - отказ, ничего не стерто
    auto none = f.services.trash->purge(f.lease(s), {});
    REQUIRE_FALSE(none.has_value());
    CHECK(none.error().code == Code::InvalidArgument);
    auto two = f.services.trash->purge(f.lease(s), {.ids = {a.id}, .all = true});
    REQUIRE_FALSE(two.has_value());
    CHECK(two.error().code == Code::InvalidArgument);
    auto live = f.services.trash->purge(f.lease(s), {.ids = {c.id}});
    REQUIRE_FALSE(live.has_value());
    CHECK(live.error().code == Code::NotFound);
    CHECK(trashIds(f, s).size() == 3);

    auto old = f.services.trash->purge(f.lease(s), {.before = cut});
    REQUIRE(old.has_value());
    CHECK(*old == 1);
    CHECK(trashIds(f, s).size() == 2);

    // запись внутри папки в корзине стирается отдельно
    auto one = f.services.trash->purge(f.lease(s), {.ids = {d.id}});
    REQUIRE(one.has_value());
    CHECK(*one == 1);
    CHECK(rootNames(f, s, folder.id).empty());

    auto all = f.services.trash->purge(f.lease(s), {.all = true});
    REQUIRE(all.has_value());
    CHECK(*all == 2); // b.txt, F
    CHECK(trashIds(f, s).empty());
    CHECK(rootNames(f, s) == std::vector<std::string>{"c.txt"});
    CHECK(*f.services.trash->purge(f.lease(s), {.all = true}) == 0);
}

TEST_CASE("nothing new goes into the trash, and trashed entries do not move", "[trash]") {
    AppFixture f;
    auto s = f.createSafe();
    REQUIRE(f.importFiles(s, {{"Old/a.txt", "a"}, {"b.txt", "b"}}).imported == 2);
    const auto old = f.entryNamed(s, "Old");
    const auto b = f.entryNamed(s, "b.txt");
    CHECK(removeIds(f, s, {old.id}) == 2);

    auto import = f.services.importExport->beginImport(f.lease(s), old.id);
    REQUIRE_FALSE(import.has_value());
    CHECK(import.error().code == Code::InvalidArgument);
    auto plan = f.services.importExport->planImport(f.lease(s), old.id, {});
    REQUIRE_FALSE(plan.has_value());
    CHECK(plan.error().code == Code::InvalidArgument);
    auto links = f.services.links->create(
        f.lease(s), {.parent = old.id, .links = {{.url = "https://example.com"}}});
    REQUIRE_FALSE(links.has_value());
    CHECK(links.error().code == Code::InvalidArgument);

    app::MoveCmd into{.ids = {b.id}, .parent = old.id};
    auto movedInto = f.services.entries->move(f.lease(s), into);
    REQUIRE_FALSE(movedInto.has_value());
    CHECK(movedInto.error().code == Code::InvalidArgument);
    app::MoveCmd outOf{.ids = {old.id}, .parent = std::nullopt};
    auto movedOut = f.services.entries->move(f.lease(s), outOf);
    REQUIRE_FALSE(movedOut.has_value());
    CHECK(movedOut.error().code == Code::InvalidArgument);

    // импорт по пути с тем же именем папки создает новую папку, а не пишет в корзину
    REQUIRE(f.importFiles(s, {{"Old/c.txt", "c"}}).imported == 1);
    const auto fresh = f.entryNamed(s, "Old");
    CHECK(fresh.id != old.id);
    CHECK(rootNames(f, s, fresh.id) == std::vector<std::string>{"c.txt"});
}

TEST_CASE("content in the trash does not count as a duplicate", "[trash][dedup]") {
    AppFixture f;
    auto s = f.createSafe();
    REQUIRE(f.importFiles(s, {{"photo.jpg", fakeJpeg(900)}}).imported == 1);
    const auto photo = f.entryNamed(s, "photo.jpg");
    CHECK(removeIds(f, s, {photo.id}) == 1);

    const auto again = f.importFiles(s, {{"photo.jpg", fakeJpeg(900)}});
    CHECK(again.imported == 1);
    CHECK(again.duplicates.empty());
    // и наоборот: живая копия - дубль, даже если в корзине есть запись с меньшим id
    const auto third = f.importFiles(s, {{"copy.jpg", fakeJpeg(900)}});
    CHECK(third.imported == 0);
    REQUIRE(third.duplicates.size() == 1);
    CHECK(third.duplicates[0].existing == f.entryNamed(s, "photo.jpg").id);
}
