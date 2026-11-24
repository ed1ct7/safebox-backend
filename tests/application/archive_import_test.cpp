// Загрузка архива (Ф3): запись создается сразу с описанием, тегами и происхождением из
// manifest; хеш от клиента сверяется; lookup говорит, что уже есть; ссылки - те же поля.
#include <catch2/catch_test_macros.hpp>

#include "app_fixture.hpp"

using namespace safebox;
using namespace safebox::test;
using Code = domain::Error::Code;

namespace {

domain::Sha256 sha256Of(AppFixture& f, std::string_view data) {
    auto hasher = f.crypto->sha256();
    hasher->update(domain::asBytes(data));
    return hasher->finish();
}

domain::TagId makeTag(AppFixture& f, const app::UnlockResult& s, const std::string& name) {
    auto made = f.services.tags->createTag(
        f.lease(s), {.category = "Тема", .name = name, .createCategory = true});
    REQUIRE(made.has_value());
    return made->tag.id;
}

std::vector<domain::TagId> tagIds(const domain::Entry& entry) {
    std::vector<domain::TagId> ids;
    for (const auto& t : entry.meta.tags) {
        CHECK_FALSE(t.inherit);
        ids.push_back(t.tagId);
    }
    return ids;
}

app::ImportFileOptions described(std::vector<domain::TagId> tags) {
    app::ImportFileOptions options;
    options.details.description = "из канала";
    options.details.tags = std::move(tags);
    options.details.source = "  telegram ";
    options.details.sourceUrl = "https://t.me/c/1/2";
    options.details.sourceDate = 1'700'000'000'000;
    return options;
}

} // namespace

TEST_CASE("an imported file gets its description, tags and origin in one go", "[import][archive]") {
    AppFixture f;
    auto s = f.createSafe();
    const auto cat = makeTag(f, s, "кошки");
    const auto dog = makeTag(f, s, "собаки");
    auto options = described({dog, cat, dog});
    const auto small = randomBytes(300, 1);
    const auto big = randomBytes(kTestChunk * 3 + 9, 2); // потоком, своя транзакция
    const auto r = f.importFiles(s, {{"a/small.bin", small, options}, {"a/big.bin", big, options}});
    REQUIRE(r.imported == 2);
    REQUIRE(r.written.size() == 2);
    const auto folder = f.entryNamed(s, "a");
    for (const auto* name : {"small.bin", "big.bin"}) {
        const auto e = f.entryNamed(s, name, folder.id);
        CHECK(e.meta.description == "из канала");
        CHECK(e.meta.descriptionByUser);
        CHECK(e.meta.source == "telegram");
        CHECK(e.meta.sourceUrl == "https://t.me/c/1/2");
        CHECK(e.meta.sourceDate == 1'700'000'000'000);
        CHECK(tagIds(e) == std::vector<domain::TagId>{std::min(cat, dog), std::max(cat, dog)});
    }
    // в порядке записи: пачка пишется после большого файла
    CHECK(r.written[0].path == "a/big.bin");
    CHECK(r.written[0].id == f.entryNamed(s, "big.bin", folder.id).id);
    CHECK(r.written[1].path == "a/small.bin");
    CHECK(r.written[1].id == f.entryNamed(s, "small.bin", folder.id).id);
}

TEST_CASE("a file with bad details fails alone and creates no folders", "[import][archive]") {
    AppFixture f;
    auto s = f.createSafe();
    app::ImportFileOptions unknownTag;
    unknownTag.details.tags = {999};
    app::ImportFileOptions badUrl;
    badUrl.details.sourceUrl = "ftp://x";
    app::ImportFileOptions longSource;
    longSource.details.source = std::string(65, 'x');
    const auto r = f.importFiles(s, {{"t/a.txt", "a", unknownTag},
                                     {"u/b.txt", "b", badUrl},
                                     {"v/c.txt", "c", longSource},
                                     {"ok.txt", "d"}});
    CHECK(r.imported == 1);
    CHECK(r.failed == 3);
    REQUIRE(r.failures.size() == 3);
    CHECK(r.failures[0].message == "Неизвестный тег");
    auto listing = f.services.entries->list(f.lease(s), std::nullopt);
    REQUIRE(listing.has_value());
    REQUIRE(listing->entries.size() == 1);
    CHECK(listing->entries[0].name == "ok.txt");
}

TEST_CASE("the client's sha256 must match what arrived", "[import][archive]") {
    AppFixture f;
    auto s = f.createSafe();
    const auto data = randomBytes(kTestChunk * 2 + 1, 3);
    app::ImportFileOptions right;
    right.sha256 = sha256Of(f, data);
    app::ImportFileOptions wrong;
    wrong.sha256 = sha256Of(f, "something else");
    const auto r = f.importFiles(
        s,
        {{"wrong.bin", data, wrong}, {"small wrong.txt", "x", wrong}, {"right.bin", data, right}});
    CHECK(r.imported == 1);
    CHECK(r.failed == 2);
    CHECK(r.failures[0].message == "Содержимое не совпало с sha256 из manifest");
    CHECK(f.entryNamed(s, "right.bin").meta.sha256 == right.sha256);
    CHECK(f.store.pendingBlobs(f.safePath()) == 0);
    CHECK(f.store.blobCount(f.safePath()) == 1);
}

TEST_CASE("a tag removed while the import runs does not land on the entry", "[import][archive]") {
    AppFixture f;
    auto s = f.createSafe();
    const auto keep = makeTag(f, s, "остается");
    const auto gone = makeTag(f, s, "уходит");
    auto import = f.services.importExport->beginImport(f.lease(s), std::nullopt);
    REQUIRE(import.has_value());
    REQUIRE((*import)->beginFile("a.txt", described({keep, gone})).has_value());
    REQUIRE((*import)->write(domain::asBytes("a")).has_value());
    REQUIRE((*import)->endFile().has_value()); // в пачке
    REQUIRE(f.services.tags->removeTag(f.lease(s), gone).has_value());
    CHECK((*import)->finish().imported == 1);
    import->reset();
    CHECK(tagIds(f.entryNamed(s, "a.txt")) == std::vector<domain::TagId>{keep});
}

TEST_CASE("replace keeps the entry's own fields unless the manifest gives new ones",
          "[import][archive]") {
    AppFixture f;
    auto s = f.createSafe();
    const auto first = makeTag(f, s, "первый");
    const auto second = makeTag(f, s, "второй");
    REQUIRE(f.importFiles(s, {{"a.txt", "old", described({first})}}).imported == 1);
    app::ImportFileOptions replace{.onConflict = app::ConflictPolicy::Replace};
    replace.details.tags = {second};
    replace.details.description = "новое";
    REQUIRE(f.importFiles(s, {{"a.txt", "new", replace}}).replaced == 1);
    const auto e = f.entryNamed(s, "a.txt");
    CHECK(e.meta.description == "новое");
    CHECK(e.meta.source == "telegram");
    CHECK(tagIds(e) ==
          std::vector<domain::TagId>{std::min(first, second), std::max(first, second)});
}

TEST_CASE("lookup tells which contents are already in the safe", "[entries][archive]") {
    AppFixture f;
    auto s = f.createSafe();
    REQUIRE(f.importFiles(s, {{"a.txt", "aaa"}, {"b.txt", "bbb"}, {"e.txt", ""}}).imported == 3);
    const auto a = f.entryNamed(s, "a.txt");
    const auto b = f.entryNamed(s, "b.txt");
    const domain::EntryId trashed[] = {b.id};
    REQUIRE(f.services.entries->remove(f.lease(s), trashed).has_value());

    // пустое содержимое не ищется: импорт пустые файлы повтором не считает
    const domain::Sha256 asked[] = {sha256Of(f, "zzz"), sha256Of(f, "aaa"), sha256Of(f, "bbb"),
                                    sha256Of(f, "")};
    auto found = f.services.entries->lookup(f.lease(s), asked);
    REQUIRE(found.has_value());
    REQUIRE(found->size() == 1); // в корзине - не считается
    CHECK((*found)[0].id == a.id);
    CHECK((*found)[0].sha256 == asked[1]);

    const std::vector<domain::Sha256> tooMany(app::kMaxLookupHashes + 1);
    auto refused = f.services.entries->lookup(f.lease(s), tooMany);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == Code::InvalidArgument);
}

TEST_CASE("an import counts as active until its session is gone", "[import][archive]") {
    AppFixture f;
    auto s = f.createSafe();
    CHECK(f.services.importExport->activeImports() == 0);
    auto import = f.services.importExport->beginImport(f.lease(s), std::nullopt);
    REQUIRE(import.has_value());
    REQUIRE((*import)->beginFile("a.txt", {}).has_value());
    REQUIRE((*import)->write(domain::asBytes("a")).has_value());
    REQUIRE((*import)->endFile().has_value()); // в пачке: дописывается в деструкторе
    CHECK(f.services.importExport->activeImports() == 1);
    import->reset(); // оборванный запрос: пачка дописана, импорт больше не идет
    CHECK(f.services.importExport->activeImports() == 0);
    CHECK(f.entryNamed(s, "a.txt").meta.size == 1);
}

TEST_CASE("links take the same details; a bad one refuses the whole batch", "[links][archive]") {
    AppFixture f;
    REQUIRE(f.services.settings->update({false}).has_value()); // без предпросмотров
    auto s = f.createSafe();
    const auto tag = makeTag(f, s, "видео");
    app::NewLink link{.url = "https://example.com/v/1", .name = std::nullopt, .path = "Видео"};
    link.details = described({tag}).details;
    auto made = f.services.links->create(f.lease(s), {std::nullopt, {link}});
    REQUIRE(made.has_value());
    REQUIRE(made->created.size() == 1);
    const auto e = made->created[0];
    CHECK(e.meta.description == "из канала");
    CHECK(e.meta.descriptionByUser);
    CHECK(e.meta.source == "telegram");
    CHECK(e.meta.sourceDate == 1'700'000'000'000);
    CHECK(tagIds(e) == std::vector<domain::TagId>{tag});

    auto again = f.services.links->create(f.lease(s), {std::nullopt, {link}});
    REQUIRE(again.has_value());
    CHECK(again->created.empty());
    CHECK(again->existing.size() == 1);

    app::NewLink bad{.url = "https://example.com/v/2", .name = std::nullopt, .path = ""};
    bad.details.tags = {12345};
    auto refused = f.services.links->create(f.lease(s), {std::nullopt, {bad}});
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == Code::InvalidArgument);
}
