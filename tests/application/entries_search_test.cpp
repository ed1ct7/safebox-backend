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

TEST_CASE("remove puts subtrees in the trash, purge deletes them with their data",
          "[entries][trash][UF-9][UF-10]") {
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
    CHECK(*removed == 5);                                  // trip, inner, a.jpg, b.txt, c.txt
    CHECK(f.store.blobCount(f.safePath()) == blobsBefore); // в корзине данные целы
    auto root = f.services.entries->list(f.lease(s), std::nullopt);
    REQUIRE(root.has_value());
    REQUIRE(root->entries.size() == 1);
    CHECK(root->entries[0].name == "keep.txt");
    auto trash = f.services.trash->list(f.lease(s), {});
    REQUIRE(trash.has_value());
    REQUIRE(trash->total == 1); // корень корзины - только папка, файл внутри ушел с ней
    CHECK(trash->entries[0].entry.id == trip.id);
    CHECK(f.readContent(s, a.id).size() == 1500);

    auto purged = f.services.trash->purge(f.lease(s), {.ids = {trip.id}});
    REQUIRE(purged.has_value());
    CHECK(*purged == 5);
    CHECK(f.store.blobCount(f.safePath()) == blobsBefore - 4); // 3 файла + миниатюра a.jpg
    CHECK(f.store.compactions >= 1);
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

    auto hits = f.services.search->search(f.lease(s), {.text = "ёлка на пляже"});
    REQUIRE(hits.has_value());
    REQUIRE(hits->hits.size() == 1);
    CHECK(hits->hits[0].entry.name == "Ёлка на ПЛЯЖЕ.jpg");
    REQUIRE(hits->hits[0].path.size() == 2);
    CHECK(hits->hits[0].path[0].name == "Отпуск");
    CHECK(hits->hits[0].path[1].name == "Море");

    auto yo = f.services.search->search(f.lease(s), {.text = "ЕЛКА"}); // "е" находит "ё"
    REQUIRE(yo.has_value());
    CHECK(yo->hits.size() == 1);

    auto latin = f.services.search->search(f.lease(s), {.text = "report.pdf"});
    REQUIRE(latin.has_value());
    REQUIRE(latin->hits.size() == 1);
    CHECK(latin->hits[0].entry.meta.mime == "application/pdf");

    // латинская: Work, Report.PDF, notes.txt
    auto folders = f.services.search->search(f.lease(s), {.text = "o"});
    REQUIRE(folders.has_value());
    REQUIRE(folders->hits.size() == 3);
    CHECK(folders->hits[0].entry.name == "Work"); // папки первыми

    auto limited = f.services.search->search(f.lease(s), {.text = "t", .page = {.limit = 2}});
    REQUIRE(limited.has_value());
    CHECK(limited->hits.size() == 2);

    auto blank = f.services.search->search(f.lease(s), {.text = "   "});
    REQUIRE(blank.has_value());
    CHECK(blank->hits.empty());

    // кэш имен обновляется после импорта и переименования
    REQUIRE(f.importFiles(s, {{"new.txt", "z"}}).imported == 1);
    auto fresh = f.services.search->search(f.lease(s), {.text = "new"});
    REQUIRE(fresh.has_value());
    CHECK(fresh->hits.size() == 1);
    REQUIRE(f.services.entries
                ->update(f.lease(s), f.entryNamed(s, "new.txt").id, {.name = "renamed.txt"})
                .has_value());
    auto stale = f.services.search->search(f.lease(s), {.text = "new"});
    REQUIRE(stale.has_value());
    CHECK(stale->hits.empty());
}

TEST_CASE("search without limit returns every match", "[search][UF-8]") {
    AppFixture f;
    auto s = f.createSafe();
    // больше старого лимита по умолчанию (200)
    std::vector<ImportFile> files;
    for (int i = 0; i < 250; ++i) {
        files.push_back({.path = "bulk" + std::to_string(i) + ".txt", .data = std::to_string(i)});
    }
    REQUIRE(f.importFiles(s, files).imported == 250);

    auto all = f.services.search->search(f.lease(s), {.text = "bulk"});
    REQUIRE(all.has_value());
    CHECK(all->hits.size() == 250);

    auto trimmed = f.services.search->search(f.lease(s), {.text = "bulk", .page = {.limit = 200}});
    REQUIRE(trimmed.has_value());
    CHECK(trimmed->hits.size() == 200);
    CHECK(trimmed->total == 250);
    CHECK(trimmed->hits[2].entry.name == "bulk2.txt"); // числа в имени - по значению
    CHECK(trimmed->hits[10].entry.name == "bulk10.txt");

    auto tail = f.services.search->search(
        f.lease(s), {.text = "bulk", .page = {.descending = true, .offset = 240, .limit = 20}});
    REQUIRE(tail.has_value());
    CHECK(tail->total == 250);
    REQUIRE(tail->hits.size() == 10);
    CHECK(tail->hits.front().entry.name == "bulk9.txt");
    CHECK(tail->hits.back().entry.name == "bulk0.txt");

    auto past = f.services.search->search(f.lease(s), {.text = "bulk", .page = {.offset = 900}});
    REQUIRE(past.has_value());
    CHECK(past->hits.empty());
    CHECK(past->total == 250);
}

TEST_CASE("list pages and sorts by a key, folders first", "[entries][order]") {
    AppFixture f;
    auto s = f.createSafe();
    auto at = [](std::int64_t ms) { return app::ImportFileOptions{.sourceModifiedAt = ms}; };
    REQUIRE(f.importFiles(s, {{"img10.jpg", "1234", at(3000)},
                              {"img2.jpg", "1", at(1000)},
                              {"img1.jpg", "12", at(2000)},
                              {"Альбом 10/a.txt", "a"},
                              {"Альбом 9/b.txt", "b"}})
                .imported == 5);

    auto names = [](const app::FolderListing& listing) {
        std::vector<std::string> out;
        for (const auto& e : listing.entries) {
            out.push_back(e.name);
        }
        return out;
    };
    using Names = std::vector<std::string>;
    auto list = [&](app::ListOptions options) {
        auto listing = f.services.entries->list(f.lease(s), std::nullopt, options);
        REQUIRE(listing.has_value());
        CHECK(listing->total == 5);
        return names(*listing);
    };
    CHECK(list({}) == Names{"Альбом 9", "Альбом 10", "img1.jpg", "img2.jpg", "img10.jpg"});
    CHECK(list({.descending = true}) ==
          Names{"Альбом 10", "Альбом 9", "img10.jpg", "img2.jpg", "img1.jpg"});
    CHECK(list({.sort = app::SortKey::Size, .descending = true}) ==
          Names{"Альбом 9", "Альбом 10", "img10.jpg", "img1.jpg", "img2.jpg"});
    // дата без sourceDate - время изменения исходного файла (папки - по времени импорта, оно
    // у них почти одно, поэтому смотрим только файлы)
    CHECK(list({.sort = app::SortKey::Date, .offset = 2}) ==
          Names{"img2.jpg", "img1.jpg", "img10.jpg"});
    CHECK(list({.sort = app::SortKey::Date, .offset = 2, .limit = 2}) ==
          Names{"img2.jpg", "img1.jpg"});
    CHECK(list({.offset = 4, .limit = 10}) == Names{"img10.jpg"});
    CHECK(list({.offset = 5}).empty());

    auto unchanged = f.services.entries->list(f.lease(s), std::nullopt); // без окна - все
    REQUIRE(unchanged.has_value());
    CHECK(unchanged->entries.size() == 5);
    CHECK(unchanged->total == 5);
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
