#include <thread>

#include "app_fixture.hpp"

using namespace safebox;
using namespace safebox::test;
using Code = domain::Error::Code;

TEST_CASE("list sorts folders first and builds breadcrumbs", "[entries][UF-3]") {
    AppFixture f;
    auto s = f.createSafe();
    REQUIRE(
        f.importFiles(
             s, {{"b.txt", "b"}, {"Zeta/x.txt", "x"}, {"alpha/Beta/deep.txt", "d"}, {"A.txt", "a"}})
            .imported == 4);

    auto root = f.services.entries->list(f.lease(s), std::nullopt);
    REQUIRE(root.has_value());
    CHECK_FALSE(root->parent.has_value());
    std::vector<std::string> names;
    for (const auto& e : root->entries) {
        names.push_back(e.name);
    }
    CHECK(names == std::vector<std::string>{"alpha", "Zeta", "A.txt", "b.txt"});

    const auto alpha = f.entryNamed(s, "alpha");
    const auto beta = f.entryNamed(s, "Beta", alpha.id);
    auto deep = f.services.entries->list(f.lease(s), beta.id);
    REQUIRE(deep.has_value());
    REQUIRE(deep->parent.has_value());
    CHECK(deep->parent->name == "Beta");
    REQUIRE(deep->path.size() == 2);
    CHECK(deep->path[0].name == "alpha");
    CHECK(deep->path[1].name == "Beta");

    auto ofFile = f.services.entries->list(f.lease(s), f.entryNamed(s, "b.txt").id);
    REQUIRE(ofFile.has_value()); // родитель - любая запись, у файла просто нет вложений
    CHECK(ofFile->parent->name == "b.txt");
    CHECK(ofFile->entries.empty());
    auto missing = f.services.entries->list(f.lease(s), domain::EntryId{12345});
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code == Code::NotFound);

    auto tree = f.services.entries->folders(f.lease(s));
    REQUIRE(tree.has_value());
    std::vector<std::string> folderNames;
    for (const auto& node : *tree) {
        folderNames.push_back(node.name);
    }
    CHECK(folderNames ==
          std::vector<std::string>{"alpha", "Beta", "Zeta"}); // родители раньше детей
}

TEST_CASE("update re-seals the name and survives lock/unlock", "[entries][UF-10]") {
    AppFixture f;
    auto s = f.createSafe();
    REQUIRE(f.importFiles(s, {{"old.txt", "data"}}).imported == 1);
    const auto entry = f.entryNamed(s, "old.txt");

    auto renamed = f.services.entries->update(f.lease(s), entry.id, {.name = "  Новое имя.txt "});
    REQUIRE(renamed.has_value());
    CHECK(renamed->name == "Новое имя.txt");
    CHECK(renamed->meta.blobId == entry.meta.blobId);
    CHECK(f.entryNamed(s, "Новое имя.txt").id == entry.id);

    auto bad = f.services.entries->update(f.lease(s), entry.id, {.name = "a/b"});
    REQUIRE_FALSE(bad.has_value());
    CHECK(bad.error().code == Code::InvalidArgument);
    auto empty = f.services.entries->update(f.lease(s), entry.id, {.name = "   "});
    REQUIRE_FALSE(empty.has_value());
    CHECK(empty.error().code == Code::InvalidArgument);
    auto missing = f.services.entries->update(f.lease(s), 999, {.name = "x.txt"});
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code == Code::NotFound);

    f.services.safe->lock();
    auto again = f.services.safe->unlock({"test", "secret1"});
    REQUIRE(again.has_value());
    const auto reopened = f.entryNamed(*again, "Новое имя.txt");
    CHECK(f.readContent(*again, reopened.id) == "data");
}

TEST_CASE("remove deletes subtrees with their data", "[entries][UF-9][UF-10]") {
    AppFixture f;
    auto s = f.createSafe();
    REQUIRE(f.importFiles(s, {{"keep.txt", "k"},
                              {"trip/a.jpg", fakeJpeg(1500)},
                              {"trip/inner/b.txt", "b"},
                              {"trip/c.txt", "c"}})
                .imported == 4);
    const auto trip = f.entryNamed(s, "trip");
    const auto a = f.entryNamed(s, "a.jpg", trip.id);
    const auto blobsBefore = f.store.blobCount(f.safePath());

    const domain::EntryId selection[] = {a.id, trip.id}; // файл внутри выделенной папки
    auto removed = f.services.entries->remove(f.lease(s), selection);
    REQUIRE(removed.has_value());
    CHECK(*removed == 5);                                      // trip, inner, a.jpg, b.txt, c.txt
    CHECK(f.store.blobCount(f.safePath()) == blobsBefore - 4); // 3 файла + миниатюра a.jpg
    CHECK(f.store.compactions >= 1);

    auto root = f.services.entries->list(f.lease(s), std::nullopt);
    REQUIRE(root.has_value());
    REQUIRE(root->entries.size() == 1);
    CHECK(root->entries[0].name == "keep.txt");
    auto gone = f.services.entries->get(f.lease(s), a.id);
    REQUIRE_FALSE(gone.has_value());
    CHECK(gone.error().code == Code::NotFound);

    const domain::EntryId unknown[] = {424242};
    auto missing = f.services.entries->remove(f.lease(s), unknown);
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code == Code::NotFound);
}

TEST_CASE("search finds names anywhere, case-insensitively", "[search][UF-8]") {
    AppFixture f;
    auto s = f.createSafe();
    REQUIRE(f.importFiles(s, {{"Отпуск/Море/Ёлка на ПЛЯЖЕ.jpg", fakeJpeg(600)},
                              {"Отпуск/план.txt", "p"},
                              {"Work/Report.PDF", "%PDF-1.4"},
                              {"notes.txt", "n"}})
                .imported == 4);

    auto hits = f.services.search->search(f.lease(s), "ёлка на пляже", 0);
    REQUIRE(hits.has_value());
    REQUIRE(hits->size() == 1);
    CHECK((*hits)[0].entry.name == "Ёлка на ПЛЯЖЕ.jpg");
    REQUIRE((*hits)[0].path.size() == 2);
    CHECK((*hits)[0].path[0].name == "Отпуск");
    CHECK((*hits)[0].path[1].name == "Море");

    auto yo = f.services.search->search(f.lease(s), "ЕЛКА", 0); // "е" находит "ё"
    REQUIRE(yo.has_value());
    CHECK(yo->size() == 1);

    auto latin = f.services.search->search(f.lease(s), "report.pdf", 0);
    REQUIRE(latin.has_value());
    REQUIRE(latin->size() == 1);
    CHECK((*latin)[0].entry.meta.mime == "application/pdf");

    auto folders =
        f.services.search->search(f.lease(s), "o", 0); // латинская: Work, Report.PDF, notes.txt
    REQUIRE(folders.has_value());
    REQUIRE(folders->size() == 3);
    CHECK((*folders)[0].entry.name == "Work"); // папки первыми

    auto limited = f.services.search->search(f.lease(s), "t", 2);
    REQUIRE(limited.has_value());
    CHECK(limited->size() == 2);

    auto blank = f.services.search->search(f.lease(s), "   ", 0);
    REQUIRE(blank.has_value());
    CHECK(blank->empty());

    // кэш имен обновляется после импорта и переименования
    REQUIRE(f.importFiles(s, {{"new.txt", "z"}}).imported == 1);
    auto fresh = f.services.search->search(f.lease(s), "new", 0);
    REQUIRE(fresh.has_value());
    CHECK(fresh->size() == 1);
    REQUIRE(f.services.entries
                ->update(f.lease(s), f.entryNamed(s, "new.txt").id, {.name = "renamed.txt"})
                .has_value());
    auto stale = f.services.search->search(f.lease(s), "new", 0);
    REQUIRE(stale.has_value());
    CHECK(stale->empty());
}

TEST_CASE("services refuse to work after lock", "[entries][UF-13]") {
    AppFixture f;
    auto s = f.createSafe();
    REQUIRE(f.importFiles(s, {{"a.txt", "a"}}).imported == 1);
    auto held = f.lease(s);
    std::thread locker([&] { f.services.safe->lock(); });
    for (int i = 0; i < 200 && !held.cancelled(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    auto listing = f.services.entries->list(held, std::nullopt);
    REQUIRE_FALSE(listing.has_value());
    CHECK(listing.error().code == Code::Locked);
    {
        auto release = std::move(held);
    }
    locker.join();
    CHECK_FALSE(f.store.isOpen());
}
