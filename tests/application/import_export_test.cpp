#include <thread>

#include "app_fixture.hpp"
#include "safebox/domain/model/safe_format.hpp"
#include "sealing.hpp"

using namespace safebox;
using namespace safebox::test;
using Code = domain::Error::Code;
using app::ConflictPolicy;

namespace {

constexpr std::string_view kShortcut = "[InternetShortcut]\r\nURL=https://example.com/a?b=1\r\n";

constexpr app::ImportFileOptions kSkip{.onConflict = ConflictPolicy::Skip};
constexpr app::ImportFileOptions kReplace{.onConflict = ConflictPolicy::Replace};

using Names = std::vector<std::string>;

domain::BlobId blobOf(const domain::Entry& e) {
    REQUIRE(e.meta.blobId.has_value());
    return *e.meta.blobId;
}

Names namesIn(AppFixture& f, const app::UnlockResult& s, std::optional<domain::EntryId> parent) {
    auto listing = f.services.entries->list(f.lease(s), parent);
    REQUIRE(listing.has_value());
    Names out;
    for (const auto& e : listing->entries) {
        out.push_back(e.name);
    }
    std::sort(out.begin(), out.end());
    return out;
}

// Правка записей "мимо" сервисов: тестам нужно то, чего в интерфейсах пока нет (теги, дубли
// имен, ссылка без блоба). Ключи выводятся из пароля так же, как при входе.
class RawVault {
public:
    explicit RawVault(AppFixture& f) : f_(f), db_(f.store.database(f.safePath())) {
        const auto& meta = db_->meta;
        auto kek = f.crypto->deriveKek("secret1", meta.salt, meta.kdf);
        REQUIRE(kek.has_value());
        auto master = f.crypto->unwrapKey(
            *kek, meta.envelope, domain::aad::envelope(meta.formatVersion, meta.salt, meta.kdf));
        REQUIRE(master.has_value());
        app::SessionKeys keys;
        keys.master = *master;
        keys.names = f.crypto->deriveSubkey(*master, domain::KeyPurpose::Names).value();
        keys.content = f.crypto->deriveSubkey(*master, domain::KeyPurpose::Content).value();
        keys.thumbnails = f.crypto->deriveSubkey(*master, domain::KeyPurpose::Thumbnails).value();
        sealer_.emplace(*f.crypto, std::move(keys));
    }

    template <class Change>
    void editMeta(domain::EntryId id, Change change) {
        auto& record = db_->entries.at(id);
        auto entry = sealer_->openEntry(record);
        REQUIRE(entry.has_value());
        change(entry->meta);
        auto sealed = sealer_->sealMeta(id, entry->meta);
        REQUIRE(sealed.has_value());
        record.encMeta = std::move(*sealed);
    }

    void rename(domain::EntryId id, const std::string& name) {
        auto sealed = sealer_->sealName(id, name);
        REQUIRE(sealed.has_value());
        db_->entries.at(id).encName = std::move(*sealed);
    }

    // Ссылка без блоба, какой ее создаст сервис ссылок.
    domain::EntryId addLink(std::optional<domain::EntryId> parent, const std::string& name,
                            const std::string& url) {
        auto uow = f_.store.begin();
        REQUIRE(uow.has_value());
        domain::EntryRecord record;
        record.parentId = parent;
        auto id = (*uow)->entries().insert(record);
        REQUIRE(id.has_value());
        domain::EntryMeta meta;
        meta.kind = domain::Kind::Link;
        meta.url = url;
        meta.createdAt = meta.modifiedAt = 1'790'000'000'000;
        auto encName = sealer_->sealName(*id, name);
        auto encMeta = sealer_->sealMeta(*id, meta);
        REQUIRE(encName.has_value());
        REQUIRE(encMeta.has_value());
        REQUIRE((*uow)->entries().updateSealed(*id, *encName, *encMeta).has_value());
        REQUIRE((*uow)->commit().has_value());
        return *id;
    }

private:
    AppFixture& f_;
    std::shared_ptr<InMemoryVaultStore::Db> db_;
    std::optional<app::Sealer> sealer_;
};

// Каталог сессии кэшируется: после правок мимо сервисов заходим заново.
app::UnlockResult reopen(AppFixture& f) {
    f.services.safe->lock();
    auto again = f.services.safe->unlock({"test", "secret1"});
    REQUIRE(again.has_value());
    return *again;
}

} // namespace

TEST_CASE("import keeps folder structure, detects kinds and makes thumbnails", "[import][UF-7]") {
    AppFixture f;
    auto s = f.createSafe();
    const auto photo1 = fakeJpeg(3000, 1);
    const auto photo2 = fakeJpeg(500, 2);
    auto result = f.importFiles(s, {{"Отпуск/Море/photo1.jpg", photo1},
                                    {"Отпуск/photo2.jpg", photo2},
                                    {"Отпуск\\Море\\notes.txt", "заметки"},
                                    {"link.url", std::string(kShortcut)},
                                    {"empty.bin", ""}});
    CHECK(result.imported == 5);
    CHECK(result.failed == 0);

    auto root = f.services.entries->list(f.lease(s), std::nullopt);
    REQUIRE(root.has_value());
    REQUIRE(root->entries.size() == 3);
    CHECK(root->entries[0].name == "Отпуск"); // папки первыми
    CHECK(root->entries[0].isFolder());

    const auto trip = root->entries[0];
    const auto sea = f.entryNamed(s, "Море", trip.id);
    CHECK(sea.isFolder());
    auto tripListing = f.services.entries->list(f.lease(s), trip.id);
    REQUIRE(tripListing.has_value());
    CHECK(tripListing->entries.size() == 2); // "Море" не задвоилась
    CHECK(tripListing->path.size() == 1);

    const auto p1 = f.entryNamed(s, "photo1.jpg", sea.id);
    CHECK(p1.meta.kind == domain::Kind::Photo);
    CHECK(p1.meta.mime == "image/jpeg");
    CHECK(p1.meta.size == 3000);
    REQUIRE(p1.hasThumbnail());
    // миниатюра построена по всем байтам фото, хотя кусок - 1 КиБ
    CHECK(f.readContent(s, p1.id, app::ContentVariant::Thumbnail) == "THUMB:3000");
    CHECK(f.readContent(s, p1.id) == photo1);

    const auto notes = f.entryNamed(s, "notes.txt", sea.id);
    CHECK(notes.meta.kind == domain::Kind::File);
    CHECK_FALSE(notes.hasThumbnail());
    auto noThumb =
        f.services.importExport->openContent(f.lease(s), notes.id, app::ContentVariant::Thumbnail);
    REQUIRE_FALSE(noThumb.has_value());
    CHECK(noThumb.error().code == Code::NotFound);

    const auto link = f.entryNamed(s, "link.url");
    CHECK(link.meta.kind == domain::Kind::Link);
    CHECK(link.meta.url == "https://example.com/a?b=1");
    CHECK(f.readContent(s, link.id) == kShortcut); // экспорт отдает исходный .url

    const auto empty = f.entryNamed(s, "empty.bin");
    CHECK(empty.meta.size == 0);
    CHECK(f.readContent(s, empty.id).empty());

    CHECK(f.thumbnailer.calls == 2);
    CHECK(f.store.pendingBlobs(f.safePath()) == 0);
}

TEST_CASE("round trip is byte exact across chunk boundaries", "[import][UF-11]") {
    AppFixture f;
    auto s = f.createSafe();
    const std::size_t sizes[] = {1, 1023, 1024, 1025, 3 * 1024, 10 * 1024 + 123};
    const std::size_t pieces[] = {1, 333, 4096};
    int n = 0;
    for (const auto size : sizes) {
        for (const auto piece : pieces) {
            const auto name = "f" + std::to_string(n++) + ".bin";
            const auto data = randomBytes(size, static_cast<std::uint32_t>(n));
            REQUIRE(f.importFiles(s, {{name, data}}, std::nullopt, piece).imported == 1);
            const auto entry = f.entryNamed(s, name);
            CHECK(entry.meta.size == size);
            CHECK(f.readContent(s, entry.id) == data);
            const auto& blob = f.store.database(f.safePath())->blobs.at(blobOf(entry));
            CHECK(blob.chunkCount == domain::chunkCountFor(size, kTestChunk));
        }
    }
}

TEST_CASE("range reads decrypt only the chunks they need", "[import][UF-5]") {
    AppFixture f;
    auto s = f.createSafe();
    const auto data = randomBytes(10 * 1024 + 123);
    REQUIRE(f.importFiles(s, {{"video.bin", data}}).imported == 1);
    const auto entry = f.entryNamed(s, "video.bin");

    auto opened =
        f.services.importExport->openContent(f.lease(s), entry.id, app::ContentVariant::Original);
    REQUIRE(opened.has_value());
    auto& stream = *opened->stream;
    CHECK(stream.size() == data.size());

    const auto before = f.store.chunkReads.load();
    domain::MemorySink part;
    REQUIRE(stream.read(1500, 1000, part).has_value()); // куски 1 и 2
    CHECK(domain::asChars(part.bytes) == std::string_view(data).substr(1500, 1000));
    CHECK(f.store.chunkReads.load() - before == 2);

    domain::MemorySink cached;
    REQUIRE(stream.read(2400, 50, cached).has_value()); // тот же последний кусок - из кэша
    CHECK(domain::asChars(cached.bytes) == std::string_view(data).substr(2400, 50));
    CHECK(f.store.chunkReads.load() - before == 2);

    domain::MemorySink tail;
    REQUIRE(stream.read(data.size() - 10, 10, tail).has_value());
    CHECK(domain::asChars(tail.bytes) == std::string_view(data).substr(data.size() - 10));

    domain::MemorySink outside;
    auto bad = stream.read(data.size() - 5, 10, outside);
    REQUIRE_FALSE(bad.has_value());
    CHECK(bad.error().code == Code::InvalidArgument);
}

TEST_CASE("one bad file does not stop the import and leaves no pending data", "[import][UF-7]") {
    AppFixture f;
    auto s = f.createSafe();
    auto result =
        f.importFiles(s, {{"good1.txt", "one"}, {"../escape.txt", "evil"}, {"good2.txt", "two"}});
    CHECK(result.imported == 2);
    CHECK(result.failed == 1);
    REQUIRE(result.failures.size() == 1);
    CHECK(result.failures[0].path.find("escape") != std::string::npos);
    CHECK(f.store.pendingBlobs(f.safePath()) == 0);

    auto missing = f.services.importExport->beginImport(f.lease(s), domain::EntryId{9999});
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code == Code::NotFound);
}

TEST_CASE("names from disk are sanitized instead of rejected", "[import][UF-7]") {
    AppFixture f;
    auto s = f.createSafe();
    auto result = f.importFiles(s, {{"dir/ we:ird?.txt ", "x"}});
    CHECK(result.imported == 1);
    const auto dir = f.entryNamed(s, "dir");
    CHECK(f.entryNamed(s, "we_ird_.txt", dir.id).meta.size == 1);
}

TEST_CASE("any entry takes an import into its attachments", "[import][UF-14]") {
    AppFixture f;
    auto s = f.createSafe();
    REQUIRE(f.importFiles(s, {{"снимок.jpg", fakeJpeg(700)}, {"site.url", std::string(kShortcut)}})
                .imported == 2);
    const auto photo = f.entryNamed(s, "снимок.jpg");
    const auto link = f.entryNamed(s, "site.url");

    CHECK(f.importFiles(s, {{"Заметки/a.txt", "1"}, {"b.txt", "2"}}, photo.id).imported == 2);
    CHECK(namesIn(f, s, photo.id) == Names{"b.txt", "Заметки"});
    CHECK(f.entryNamed(s, "снимок.jpg").childCount == 2);
    CHECK(f.entryNamed(s, "снимок.jpg").meta.kind == domain::Kind::Photo); // остается собой
    auto listing = f.services.entries->list(f.lease(s), photo.id);
    REQUIRE(listing.has_value());
    CHECK(listing->path.size() == 1); // крошки идут через запись

    // имена сверяются у вложений, а не в корне; папки сливаются
    const auto again = f.importFiles(s, {{"b.txt", "3"}, {"ЗАМЕТКИ/z.txt", "4"}}, photo.id);
    CHECK(again.imported == 2);
    CHECK(namesIn(f, s, photo.id) == Names{"b (2).txt", "b.txt", "Заметки"});
    CHECK(namesIn(f, s, f.entryNamed(s, "Заметки", photo.id).id) == Names{"a.txt", "z.txt"});
    CHECK(namesIn(f, s, std::nullopt) == Names{"site.url", "снимок.jpg"});

    CHECK(f.importFiles(s, {{"c.txt", "x"}}, link.id).imported == 1);
    CHECK(f.entryNamed(s, "site.url").childCount == 1);
    CHECK(f.entryNamed(s, "site.url").meta.kind == domain::Kind::Link);
    CHECK(f.store.pendingBlobs(f.safePath()) == 0);
}

TEST_CASE("the source modification time is kept in the meta", "[import][UF-15]") {
    AppFixture f;
    auto s = f.createSafe();
    REQUIRE(f.importFiles(s, {{"D/a.txt", "1", {.sourceModifiedAt = 1'700'000'000'123}},
                              {"D/b.txt", "2"},
                              {"D/old.txt", "3", {.sourceModifiedAt = -86'400'000}}})
                .imported == 3);
    const auto dir = f.entryNamed(s, "D");
    CHECK(f.entryNamed(s, "a.txt", dir.id).meta.sourceModifiedAt == 1'700'000'000'123);
    CHECK_FALSE(f.entryNamed(s, "b.txt", dir.id).meta.sourceModifiedAt.has_value());
    CHECK(f.entryNamed(s, "old.txt", dir.id).meta.sourceModifiedAt == -86'400'000);
    CHECK_FALSE(dir.meta.sourceModifiedAt.has_value()); // у папки даты исходника нет

    // дата лежит в зашифрованной мете и переживает блокировку
    const auto again = reopen(f);
    CHECK(f.entryNamed(again, "a.txt", dir.id).meta.sourceModifiedAt == 1'700'000'000'123);
}

TEST_CASE("a taken name keeps both by default", "[import][UF-15]") {
    AppFixture f;
    auto s = f.createSafe();
    REQUIRE(f.importFiles(s, {{"pict/a.txt", "AAA"}}).imported == 1);

    const auto second = f.importFiles(s, {{"pict/a.txt", "BBB"}, {"pict/A.TXT", "CCC"}});
    CHECK(second.imported == 2);
    CHECK(second.replaced == 0);
    CHECK(second.skipped == 0);
    const auto pict = f.entryNamed(s, "pict");
    CHECK(namesIn(f, s, pict.id) == Names{"A (3).TXT", "a (2).txt", "a.txt"});
    CHECK(f.readContent(s, f.entryNamed(s, "a.txt", pict.id).id) == "AAA");
    CHECK(f.readContent(s, f.entryNamed(s, "a (2).txt", pict.id).id) == "BBB");
    CHECK(f.readContent(s, f.entryNamed(s, "A (3).TXT", pict.id).id) == "CCC");
    CHECK(f.store.pendingBlobs(f.safePath()) == 0);

    SECTION("folders are merged regardless of case, created ones too") {
        const auto third =
            f.importFiles(s, {{"PICT/d.txt", "D"}, {"New/a.txt", "1"}, {"NEW/b.txt", "2"}});
        CHECK(third.imported == 3);
        CHECK(namesIn(f, s, std::nullopt) == Names{"New", "pict"});
        CHECK(namesIn(f, s, pict.id).size() == 4);
        CHECK(namesIn(f, s, f.entryNamed(s, "New").id) == Names{"a.txt", "b.txt"});
    }

    SECTION("a file does not stop a folder of the same name from being created") {
        const auto r = f.importFiles(s, {{"pict/a.txt/x.txt", "X"}});
        CHECK(r.imported == 1);
        CHECK(f.entryNamed(s, "x.txt", f.entryNamed(s, "a.txt", pict.id).id).meta.size == 1);
    }

    SECTION("a file named like a folder gets a number") {
        const auto r = f.importFiles(s, {{"pict", "file named pict"}});
        CHECK(r.imported == 1);
        CHECK(namesIn(f, s, std::nullopt) == Names{"pict", "pict (2)"});
    }
}

TEST_CASE("skip leaves the safe untouched and writes not a byte", "[import][UF-15]") {
    AppFixture f;
    auto s = f.createSafe();
    REQUIRE(f.importFiles(s, {{"pict/a.txt", "AAA"}, {"pict/b.jpg", fakeJpeg(3000)}}).imported ==
            2);
    auto db = f.store.database(f.safePath());
    const auto blobs = f.store.blobCount(f.safePath());
    const auto entries = db->entries.size();
    const auto nextBlob = db->nextBlob;
    const auto thumbs = f.thumbnailer.calls.load();

    const auto r = f.importFiles(s, {{"pict/a.txt", randomBytes(5000), kSkip},
                                     {"PICT/B.JPG", fakeJpeg(3000, 9), kSkip},
                                     {"pict/a.txt", "again", kSkip}});
    CHECK(r.imported == 0);
    CHECK(r.skipped == 3);
    CHECK(r.failed == 0);
    CHECK(db->entries.size() == entries);
    CHECK(f.store.blobCount(f.safePath()) == blobs);
    CHECK(db->nextBlob == nextBlob); // блоб не заводился даже временно
    CHECK(f.thumbnailer.calls.load() == thumbs);
    CHECK(f.readContent(s, f.entryNamed(s, "a.txt", f.entryNamed(s, "pict").id).id) == "AAA");

    // новый файл с Skip - не конфликт, он импортируется
    const auto mixed = f.importFiles(s, {{"pict/a.txt", "x", kSkip}, {"pict/new.txt", "N", kSkip}});
    CHECK(mixed.imported == 1);
    CHECK(mixed.skipped == 1);
    CHECK(f.store.blobCount(f.safePath()) == blobs + 1);
}

TEST_CASE("replace keeps the record and swaps only the content", "[import][UF-15]") {
    AppFixture f;
    auto s = f.createSafe();
    REQUIRE(f.importFiles(s, {{"pict/a.jpg", fakeJpeg(3000, 1), {.sourceModifiedAt = 1'000}},
                              {"pict/keep.txt", "K"}})
                .imported == 2);
    const auto pict = f.entryNamed(s, "pict");
    const auto original = f.entryNamed(s, "a.jpg", pict.id);
    REQUIRE(original.hasThumbnail());
    REQUIRE(f.importFiles(s, {{"att.txt", "attached"}}, original.id).imported == 1);
    REQUIRE(f.services.entries->update(f.lease(s), original.id, {.description = "мое описание"})
                .has_value());
    RawVault(f).editMeta(original.id,
                         [](domain::EntryMeta& meta) { meta.tags = {{7, true}, {9, false}}; });
    const auto before = f.entryNamed(s, "a.jpg", pict.id);
    REQUIRE(before.meta.tags.size() == 2);
    const auto oldBlob = blobOf(before);
    const auto oldThumb = *before.meta.thumbBlobId;

    f.clock.advance(std::chrono::seconds(5));
    const auto data = randomBytes(2500, 5); // не фото: миниатюры больше нет
    const auto r = f.importFiles(
        s,
        {{"PICT/A.JPG", data, {.sourceModifiedAt = 2'000, .onConflict = ConflictPolicy::Replace}}});
    CHECK(r.imported == 0);
    CHECK(r.replaced == 1);
    CHECK(r.skipped == 0);
    CHECK(r.failed == 0);

    CHECK(namesIn(f, s, pict.id) == Names{"a.jpg", "keep.txt"}); // ни "A.JPG", ни "a (2).jpg"
    const auto after = f.entryNamed(s, "a.jpg", pict.id);
    CHECK(after.id == before.id);
    CHECK(after.meta.createdAt == before.meta.createdAt);
    CHECK(after.meta.modifiedAt == before.meta.modifiedAt + 5000);
    CHECK(after.meta.description == "мое описание");
    CHECK(after.meta.descriptionByUser);
    CHECK(after.meta.tags == before.meta.tags);
    CHECK(after.childCount == 1);
    CHECK(f.readContent(s, f.entryNamed(s, "att.txt", after.id).id) == "attached");
    CHECK(after.meta.kind == domain::Kind::File);
    CHECK(after.meta.mime == "application/octet-stream");
    CHECK(after.meta.size == data.size());
    CHECK(after.meta.sourceModifiedAt == 2'000);
    CHECK_FALSE(after.hasThumbnail());
    CHECK(f.readContent(s, after.id) == data);

    // старые блоб и миниатюра ушли, мусора нет
    auto db = f.store.database(f.safePath());
    CHECK_FALSE(db->blobs.contains(oldBlob));
    CHECK_FALSE(db->blobs.contains(oldThumb));
    CHECK(f.store.blobCount(f.safePath()) == 3); // a.jpg, keep.txt, att.txt
    CHECK(f.store.pendingBlobs(f.safePath()) == 0);
    CHECK(f.store.compactions >= 1);

    SECTION("a photo over a photo gets a new thumbnail") {
        const auto photo = fakeJpeg(1500, 7);
        const auto again = f.importFiles(s, {{"pict/a.jpg", photo, kReplace}});
        CHECK(again.replaced == 1);
        const auto swapped = f.entryNamed(s, "a.jpg", pict.id);
        CHECK(swapped.id == before.id);
        CHECK(swapped.meta.kind == domain::Kind::Photo);
        REQUIRE(swapped.hasThumbnail());
        CHECK(f.readContent(s, swapped.id, app::ContentVariant::Thumbnail) == "THUMB:1500");
        CHECK_FALSE(
            swapped.meta.sourceModifiedAt.has_value()); // дата прежнего содержимого не нужна
        CHECK(swapped.meta.description == "мое описание");
        CHECK(f.store.blobCount(f.safePath()) == 4); // + миниатюра
        CHECK(f.store.pendingBlobs(f.safePath()) == 0);
    }
}

TEST_CASE("replace works across chunk boundaries", "[import][UF-15]") {
    AppFixture f;
    auto s = f.createSafe();
    REQUIRE(f.importFiles(s, {{"big.bin", randomBytes(kTestChunk * 3 + 17, 1)}}).imported == 1);
    const auto before = f.entryNamed(s, "big.bin");
    const auto data = randomBytes(kTestChunk * 5 + 3, 2);
    REQUIRE(f.importFiles(s, {{"big.bin", data, kReplace}}, std::nullopt, 500).replaced == 1);
    const auto after = f.entryNamed(s, "big.bin");
    CHECK(after.id == before.id);
    CHECK(after.meta.size == data.size());
    CHECK(f.readContent(s, after.id) == data);
    CHECK(f.store.blobCount(f.safePath()) == 1);
    CHECK(f.store.pendingBlobs(f.safePath()) == 0);
}

TEST_CASE("replace of a folder keeps both", "[import][UF-15]") {
    AppFixture f;
    auto s = f.createSafe();
    REQUIRE(f.importFiles(s, {{"docs/x.txt", "1"}}).imported == 1);
    const auto r = f.importFiles(s, {{"Docs", "a file named like the folder", kReplace}});
    CHECK(r.imported == 1);
    CHECK(r.replaced == 0);
    CHECK(namesIn(f, s, std::nullopt) == Names{"Docs (2)", "docs"});
    const auto folder = f.entryNamed(s, "docs");
    CHECK(folder.isFolder());
    CHECK(namesIn(f, s, folder.id) == Names{"x.txt"});
}

TEST_CASE("one path twice in a batch is resolved by the second file's policy", "[import][UF-15]") {
    AppFixture f;
    auto s = f.createSafe();

    SECTION("keep both") {
        const auto r = f.importFiles(s, {{"x.txt", "first"}, {"x.txt", "second"}});
        CHECK(r.imported == 2);
        CHECK(namesIn(f, s, std::nullopt) == Names{"x (2).txt", "x.txt"});
        CHECK(f.readContent(s, f.entryNamed(s, "x.txt").id) == "first");
        CHECK(f.readContent(s, f.entryNamed(s, "x (2).txt").id) == "second");
    }
    SECTION("skip") {
        const auto r = f.importFiles(s, {{"x.txt", "first"}, {"x.txt", "second", kSkip}});
        CHECK(r.imported == 1);
        CHECK(r.skipped == 1);
        CHECK(namesIn(f, s, std::nullopt) == Names{"x.txt"});
        CHECK(f.readContent(s, f.entryNamed(s, "x.txt").id) == "first");
    }
    SECTION("replace") {
        const auto r = f.importFiles(s, {{"x.txt", "first"}, {"x.txt", "second", kReplace}});
        CHECK(r.imported == 1); // запись создана первым, вторым - заменена
        CHECK(r.replaced == 1);
        CHECK(namesIn(f, s, std::nullopt) == Names{"x.txt"});
        CHECK(f.readContent(s, f.entryNamed(s, "x.txt").id) == "second");
        CHECK(f.store.blobCount(f.safePath()) == 1);
    }
    CHECK(f.store.pendingBlobs(f.safePath()) == 0);
}

TEST_CASE("each file of a batch follows its own decision", "[import][UF-15]") {
    AppFixture f;
    auto s = f.createSafe();
    REQUIRE(f.importFiles(s, {{"pict/a.txt", "A0"}, {"pict/b.txt", "B0"}, {"pict/c.txt", "C0"}})
                .imported == 3);
    const auto r = f.importFiles(s, {{"pict/a.txt", "A1", kReplace},
                                     {"pict/b.txt", "B1", kSkip},
                                     {"pict/c.txt", "C1"},
                                     {"pict/d.txt", "D1", kReplace}});
    CHECK(r.imported == 2); // c (2).txt и d.txt: замена без совпадения - обычный импорт
    CHECK(r.replaced == 1);
    CHECK(r.skipped == 1);
    const auto pict = f.entryNamed(s, "pict");
    CHECK(namesIn(f, s, pict.id) == Names{"a.txt", "b.txt", "c (2).txt", "c.txt", "d.txt"});
    CHECK(f.readContent(s, f.entryNamed(s, "a.txt", pict.id).id) == "A1");
    CHECK(f.readContent(s, f.entryNamed(s, "b.txt", pict.id).id) == "B0");
    CHECK(f.readContent(s, f.entryNamed(s, "c.txt", pict.id).id) == "C0");
}

TEST_CASE("planImport reports the name clashes and creates nothing", "[import][plan][UF-15]") {
    AppFixture f;
    auto s = f.createSafe();
    REQUIRE(f.importFiles(s, {{"pict/a.txt", "AAA"},
                              {"pict/b.txt", "BBBB"},
                              {"pict/sub/c.txt", "C"},
                              {"top.txt", "T"}})
                .imported == 4);
    auto db = f.store.database(f.safePath());
    const auto entries = db->entries.size();
    const auto nextBlob = db->nextBlob;
    const auto pict = f.entryNamed(s, "pict");

    const std::vector<app::ImportPlanFile> files = {
        {"pict/a.txt", 3},
        {"PICT/B.TXT", 4},        // регистр не важен
        {"pict/new.txt", 5},      // нет такого имени
        {"pict/sub/c.txt", 1},    // глубже
        {"pict/newdir/a.txt", 1}, // папки нет - конфликтовать не с чем
        {"other/top.txt", 1},     // и корневого top.txt в другой папке нет
        {"top.txt", 1},
        {"pict/a.txt/x.txt", 1}, // a.txt - файл, а не папка: рядом появится папка
        {"../evil.txt", 1},      // недопустимый путь упадет при импорте, конфликтов не дает
    };
    auto plan = f.services.importExport->planImport(f.lease(s), std::nullopt, files);
    REQUIRE(plan.has_value());
    REQUIRE(plan->conflicts.size() == 4);
    CHECK(plan->conflicts[0].path == "pict/a.txt");
    CHECK(plan->conflicts[0].existing.name == "a.txt");
    CHECK(plan->conflicts[0].existing.meta.size == 3);
    CHECK(plan->conflicts[0].existing.parentId == pict.id);
    CHECK(plan->conflicts[1].path == "PICT/B.TXT"); // путь как прислал клиент
    CHECK(plan->conflicts[1].existing.name == "b.txt");
    CHECK(plan->conflicts[1].existing.meta.size == 4);
    CHECK(plan->conflicts[2].path == "pict/sub/c.txt");
    CHECK(plan->conflicts[3].path == "top.txt");
    CHECK(plan->newFiles == 5);
    CHECK(plan->conflicts.size() + plan->newFiles == files.size());

    // ничего не создано
    CHECK(db->entries.size() == entries);
    CHECK(db->nextBlob == nextBlob);
    CHECK(f.store.pendingBlobs(f.safePath()) == 0);

    SECTION("relative to a folder") {
        const std::vector<app::ImportPlanFile> inside = {
            {"a.txt", 1}, {"sub/c.txt", 1}, {"z.txt", 1}, {"pict/a.txt", 1}};
        auto relative = f.services.importExport->planImport(f.lease(s), pict.id, inside);
        REQUIRE(relative.has_value());
        REQUIRE(relative->conflicts.size() == 2);
        CHECK(relative->conflicts[0].path == "a.txt");
        CHECK(relative->conflicts[1].path == "sub/c.txt");
        CHECK(relative->newFiles == 2);
    }

    SECTION("relative to the attachments of a file") {
        const auto a = f.entryNamed(s, "a.txt", pict.id);
        REQUIRE(f.importFiles(s, {{"n.txt", "N"}}, a.id).imported == 1);
        const std::vector<app::ImportPlanFile> inside = {{"n.txt", 1}, {"a.txt", 1}};
        auto relative = f.services.importExport->planImport(f.lease(s), a.id, inside);
        REQUIRE(relative.has_value());
        REQUIRE(relative->conflicts.size() == 1);
        CHECK(relative->conflicts[0].path == "n.txt");
        CHECK(relative->newFiles == 1);
    }

    SECTION("the same path twice in a plan clashes only with the safe") {
        const std::vector<app::ImportPlanFile> twice = {
            {"fresh.txt", 1}, {"fresh.txt", 1}, {"top.txt", 1}, {"top.txt", 1}};
        auto duplicated = f.services.importExport->planImport(f.lease(s), std::nullopt, twice);
        REQUIRE(duplicated.has_value());
        CHECK(duplicated->conflicts.size() == 2);
        CHECK(duplicated->newFiles == 2);
    }

    SECTION("a folder occupying the name is a clash too") {
        const std::vector<app::ImportPlanFile> named = {{"PICT", 1}};
        auto folderClash = f.services.importExport->planImport(f.lease(s), std::nullopt, named);
        REQUIRE(folderClash.has_value());
        REQUIRE(folderClash->conflicts.size() == 1);
        CHECK(folderClash->conflicts[0].existing.id == pict.id);
        CHECK(folderClash->conflicts[0].existing.isFolder());
        CHECK(folderClash->conflicts[0].existing.childCount == 3);
    }

    SECTION("an empty plan and a missing parent") {
        auto empty = f.services.importExport->planImport(f.lease(s), std::nullopt, {});
        REQUIRE(empty.has_value());
        CHECK(empty->conflicts.empty());
        CHECK(empty->newFiles == 0);
        auto missing =
            f.services.importExport->planImport(f.lease(s), domain::EntryId{9999}, files);
        REQUIRE_FALSE(missing.has_value());
        CHECK(missing.error().code == Code::NotFound);
    }
}

TEST_CASE("the plan and the import agree about what clashes", "[import][plan][UF-15]") {
    AppFixture f;
    auto s = f.createSafe();
    REQUIRE(f.importFiles(s, {{"pict/a.txt", "AAA"}, {"pict/sub/c.txt", "C"}, {"top.txt", "T"}})
                .imported == 3);

    const std::vector<std::string> paths = {"pict/a.txt",     "PICT/A2.txt", "Pict/SUB/c.TXT",
                                            "pict/sub/d.txt", "top.txt",     "new/top.txt",
                                            "TOP.txt"};
    std::vector<app::ImportPlanFile> planned;
    std::vector<ImportFile> batch;
    for (const auto& path : paths) {
        planned.push_back({path, 1});
        batch.push_back({path, "x", kSkip});
    }
    auto plan = f.services.importExport->planImport(f.lease(s), std::nullopt, planned);
    REQUIRE(plan.has_value());
    const auto r = f.importFiles(s, batch);
    CHECK(r.skipped == plan->conflicts.size());
    CHECK(r.imported == plan->newFiles);
    CHECK(r.skipped == 4);
}

TEST_CASE("lock during import cancels it and leaves no garbage", "[import][UF-13][concurrency]") {
    AppFixture f;
    auto s = f.createSafe();
    auto import = f.services.importExport->beginImport(f.lease(s), std::nullopt);
    REQUIRE(import.has_value());
    REQUIRE((*import)->beginFile("done.txt", {}).has_value());
    REQUIRE((*import)->write(domain::asBytes("finished before lock")).has_value());
    REQUIRE((*import)->endFile().has_value());

    REQUIRE((*import)->beginFile("big.bin", {}).has_value());
    REQUIRE((*import)->write(domain::asBytes(randomBytes(5000))).has_value());
    CHECK(f.store.pendingBlobs(f.safePath()) == 1); // куски уже на "диске", запись - нет

    std::thread locker([&] { f.services.safe->lock(); });
    domain::Status st;
    for (int i = 0; i < 400 && st.has_value(); ++i) {
        st = (*import)->write(domain::asBytes("more"));
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE_FALSE(st.has_value());
    CHECK(st.error().code == Code::Cancelled);
    CHECK(f.store.isOpen()); // lock ждет, пока импорт отпустит аренду

    import->reset(); // http уничтожает сессию: pending удаляется, аренда отпускается
    locker.join();
    CHECK_FALSE(f.store.isOpen());

    auto again = f.services.safe->unlock({"test", "secret1"});
    REQUIRE(again.has_value());
    CHECK(f.store.pendingBlobs(f.safePath()) == 0);
    auto root = f.services.entries->list(f.lease(*again), std::nullopt);
    REQUIRE(root.has_value());
    REQUIRE(root->entries.size() == 1);
    CHECK(root->entries[0].name == "done.txt");
}

TEST_CASE("lock during a replace keeps the old content", "[import][UF-13][concurrency]") {
    AppFixture f;
    auto s = f.createSafe();
    const auto old = randomBytes(3000, 1);
    REQUIRE(f.importFiles(s, {{"a.bin", old}}).imported == 1);
    const auto before = f.entryNamed(s, "a.bin");

    auto import = f.services.importExport->beginImport(f.lease(s), std::nullopt);
    REQUIRE(import.has_value());
    REQUIRE((*import)->beginFile("A.BIN", kReplace).has_value());
    REQUIRE((*import)->write(domain::asBytes(randomBytes(5000, 2))).has_value());
    CHECK(f.store.pendingBlobs(f.safePath()) == 1);

    std::thread locker([&] { f.services.safe->lock(); });
    domain::Status st;
    for (int i = 0; i < 400 && st.has_value(); ++i) {
        st = (*import)->write(domain::asBytes("more"));
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE_FALSE(st.has_value());
    import->reset();
    locker.join();

    auto again = f.services.safe->unlock({"test", "secret1"});
    REQUIRE(again.has_value());
    CHECK(f.store.pendingBlobs(f.safePath()) == 0);
    const auto after = f.entryNamed(*again, "a.bin");
    CHECK(after.id == before.id);
    CHECK(blobOf(after) == blobOf(before));
    CHECK(f.readContent(*again, after.id) == old);
    CHECK(f.store.blobCount(f.safePath()) == 1);
}

TEST_CASE("tampering with the safe file is detected", "[import][integrity]") {
    AppFixture f;
    auto s = f.createSafe();
    const auto data = randomBytes(3 * 1024 + 100);
    REQUIRE(f.importFiles(s, {{"a.bin", data}, {"b.bin", "short file"}}).imported == 2);
    const auto a = f.entryNamed(s, "a.bin");
    const auto b = f.entryNamed(s, "b.bin");
    auto db = f.store.database(f.safePath());

    const auto expectIntegrityError = [&](domain::EntryId id) {
        auto opened =
            f.services.importExport->openContent(f.lease(s), id, app::ContentVariant::Original);
        if (!opened) {
            CHECK(opened.error().code == Code::IntegrityError);
            return;
        }
        domain::MemorySink sink;
        auto read = opened->stream->read(0, opened->stream->size(), sink);
        REQUIRE_FALSE(read.has_value());
        CHECK(read.error().code == Code::IntegrityError);
    };

    SECTION("swapped chunks") {
        auto& chunks = db->blobs.at(blobOf(a)).chunks;
        std::swap(chunks[0], chunks[1]);
        expectIntegrityError(a.id);
    }
    SECTION("flipped byte") {
        auto& chunk = db->blobs.at(blobOf(a)).chunks[2];
        auto copy = *chunk;
        copy[30] ^= std::byte{0x01};
        chunk = std::make_shared<const domain::Bytes>(copy);
        expectIntegrityError(a.id);
    }
    SECTION("truncated: last chunk dropped") {
        db->blobs.at(blobOf(a)).chunks.pop_back();
        expectIntegrityError(a.id);
    }
    SECTION("truncated consistently: size and count rewritten") {
        auto& blob = db->blobs.at(blobOf(a));
        blob.chunks.pop_back();
        blob.chunkCount = 3;
        blob.size = 3 * 1024;
        expectIntegrityError(a.id); // размер в зашифрованной мете не сходится
    }
    SECTION("names swapped between entries") {
        std::swap(db->entries.at(a.id).encName, db->entries.at(b.id).encName);
        f.services.safe->lock();
        auto again = f.services.safe->unlock({"test", "secret1"});
        REQUIRE(again.has_value());
        auto listing = f.services.entries->list(f.lease(*again), std::nullopt);
        REQUIRE_FALSE(listing.has_value());
        CHECK(listing.error().code == Code::IntegrityError);
    }
    SECTION("content swapped between entries via blob_id columns") {
        std::swap(db->entries.at(a.id).blobId, db->entries.at(b.id).blobId);
        f.services.safe->lock();
        auto again = f.services.safe->unlock({"test", "secret1"});
        REQUIRE(again.has_value());
        auto listing = f.services.entries->list(f.lease(*again), std::nullopt);
        REQUIRE_FALSE(listing.has_value());
        CHECK(listing.error().code == Code::IntegrityError);
    }
}

TEST_CASE("zip export streams the whole folder", "[export][UF-11]") {
    AppFixture f;
    auto s = f.createSafe();
    const auto big = randomBytes(2500);
    REQUIRE(f.importFiles(s, {{"Папка/a.txt", "1"},
                              {"Папка/A.txt", "2"}, // при импорте станет "A (2).txt"
                              {"Папка/sub/b.bin", big},
                              {"Папка/sub/b.bin", "dup"}})
                .imported == 4);
    const auto folder = f.entryNamed(s, "Папка");

    domain::MemorySink sink;
    REQUIRE(f.services.importExport->exportZip(f.lease(s), folder.id, sink).has_value());
    REQUIRE(f.zip.archives.size() == 1);
    const auto& archive = f.zip.archives.front();
    CHECK(archive.finished);
    CHECK(archive.directories == std::set<std::string>{"Папка/", "Папка/sub/"});
    REQUIRE(archive.files.size() == 4);
    CHECK(domain::asChars(archive.files.at("Папка/a.txt")) == "1");
    CHECK(domain::asChars(archive.files.at("Папка/A (2).txt")) == "2");
    CHECK(domain::asChars(archive.files.at("Папка/sub/b.bin")) == big);
    CHECK(domain::asChars(archive.files.at("Папка/sub/b (2).bin")) == "dup");
    CHECK(archive.order.front() == "Папка/");
    CHECK(sink.bytes.size() == 1 + 1 + big.size() + 3); // данные текли потоком

    auto notFolder = f.services.importExport->exportZip(
        f.lease(s), f.entryNamed(s, "a.txt", folder.id).id, sink);
    REQUIRE_FALSE(notFolder.has_value()); // не папка и без вложений
    CHECK(notFolder.error().code == Code::InvalidArgument);
    auto missing = f.services.importExport->exportZip(f.lease(s), domain::EntryId{9999}, sink);
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code == Code::NotFound);

    auto asFile =
        f.services.importExport->openContent(f.lease(s), folder.id, app::ContentVariant::Original);
    REQUIRE_FALSE(asFile.has_value());
    CHECK(asFile.error().code == Code::InvalidArgument);
}

TEST_CASE("zip export makes names unique even if the safe holds equal ones", "[export][UF-11]") {
    AppFixture f;
    auto s = f.createSafe();
    REQUIRE(
        f.importFiles(s, {{"Папка/one.txt", "1"}, {"Папка/two.txt", "2"}, {"Папка/tri.txt", "3"}})
            .imported == 3);
    const auto folder = f.entryNamed(s, "Папка");
    RawVault raw(f);
    raw.rename(f.entryNamed(s, "two.txt", folder.id).id, "ONE.TXT"); // то же имя без учета регистра
    raw.rename(f.entryNamed(s, "tri.txt", folder.id).id, "one.txt");
    s = reopen(f);

    domain::MemorySink sink;
    REQUIRE(f.services.importExport->exportZip(f.lease(s), folder.id, sink).has_value());
    const auto& archive = f.zip.archives.front();
    REQUIRE(archive.files.size() == 3);
    CHECK(archive.files.contains("Папка/one.txt")); // по порядку id: первым идет исходный
    CHECK(archive.files.contains("Папка/ONE (2).TXT"));
    CHECK(archive.files.contains("Папка/one (3).txt"));
}

TEST_CASE("zip of attachments: any entry with children, structure and names",
          "[export][UF-11][UF-14]") {
    AppFixture f;
    auto s = f.createSafe();
    REQUIRE(
        f.importFiles(s, {{"снимок.jpg", fakeJpeg(700)}, {"plain.txt", "нет вложений"}}).imported ==
        2);
    const auto photo = f.entryNamed(s, "снимок.jpg");
    REQUIRE(f.importFiles(s,
                          {{"Цель/n.txt", "из цели"},
                           {"Цель/глубже/m.txt", "глубоко"},
                           {"doc.txt", "документ"},
                           {"doc.txt (вложения)/x.txt", "чужая папка"}}, // занимает имя каталога
                          photo.id)
                .imported == 4);
    const auto doc = f.entryNamed(s, "doc.txt", photo.id);
    REQUIRE(f.importFiles(s, {{"q.txt", "вложение вложения"}}, doc.id).imported == 1);

    domain::MemorySink sink;
    REQUIRE(f.services.importExport->exportZip(f.lease(s), photo.id, sink).has_value());
    const auto& archive = f.zip.archives.front();
    CHECK(archive.finished);
    // каталог вложений doc.txt занят настоящей папкой - он получает номер, папка остается
    CHECK(archive.directories ==
          std::set<std::string>{"снимок.jpg (вложения)/", "снимок.jpg (вложения)/Цель/",
                                "снимок.jpg (вложения)/Цель/глубже/",
                                "снимок.jpg (вложения)/doc.txt (вложения)/",
                                "снимок.jpg (вложения)/doc.txt (вложения) (2)/"});
    REQUIRE(archive.files.size() == 5);
    CHECK(domain::asChars(archive.files.at("снимок.jpg (вложения)/Цель/n.txt")) == "из цели");
    CHECK(domain::asChars(archive.files.at("снимок.jpg (вложения)/Цель/глубже/m.txt")) ==
          "глубоко");
    CHECK(domain::asChars(archive.files.at("снимок.jpg (вложения)/doc.txt")) == "документ");
    CHECK(domain::asChars(archive.files.at("снимок.jpg (вложения)/doc.txt (вложения)/x.txt")) ==
          "чужая папка");
    CHECK(domain::asChars(archive.files.at("снимок.jpg (вложения)/doc.txt (вложения) (2)/q.txt")) ==
          "вложение вложения");
    CHECK(archive.order.front() == "снимок.jpg (вложения)/");
    // сам снимок в архив вложений не входит
    CHECK_FALSE(archive.files.contains("снимок.jpg"));

    SECTION("a folder with an attachment-bearing file inside") {
        REQUIRE(f.importFiles(s, {{"Альбом/one.txt", "1"}}).imported == 1);
        const auto one = f.entryNamed(s, "one.txt", f.entryNamed(s, "Альбом").id);
        REQUIRE(f.importFiles(s, {{"p.txt", "P"}}, one.id).imported == 1);
        domain::MemorySink out;
        REQUIRE(f.services.importExport->exportZip(f.lease(s), f.entryNamed(s, "Альбом").id, out)
                    .has_value());
        const auto& album = f.zip.archives.back();
        CHECK(album.directories == std::set<std::string>{"Альбом/", "Альбом/one.txt (вложения)/"});
        CHECK(domain::asChars(album.files.at("Альбом/one.txt")) == "1");
        CHECK(domain::asChars(album.files.at("Альбом/one.txt (вложения)/p.txt")) == "P");
    }

    SECTION("an entry without children is refused") {
        auto refused =
            f.services.importExport->exportZip(f.lease(s), f.entryNamed(s, "plain.txt").id, sink);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().code == Code::InvalidArgument);
    }
}

TEST_CASE("a link without a blob goes into the zip as an .url shortcut", "[export][UF-11][UF-14]") {
    AppFixture f;
    auto s = f.createSafe();
    REQUIRE(f.importFiles(s, {{"Закладки/a.txt", "a"}}).imported == 1);
    const auto folder = f.entryNamed(s, "Закладки");
    RawVault raw(f);
    const auto site = raw.addLink(folder.id, "example.com", "https://example.com/page?x=1");
    raw.addLink(folder.id, "Сайт.URL", "https://example.org/");
    s = reopen(f);
    REQUIRE(f.importFiles(s, {{"заметка.txt", "к ссылке"}}, site).imported == 1);

    domain::MemorySink sink;
    REQUIRE(f.services.importExport->exportZip(f.lease(s), folder.id, sink).has_value());
    const auto& archive = f.zip.archives.front();
    CHECK(archive.directories ==
          std::set<std::string>{"Закладки/", "Закладки/example.com.url (вложения)/"});
    REQUIRE(archive.files.size() == 4);
    CHECK(domain::asChars(archive.files.at("Закладки/example.com.url")) ==
          "[InternetShortcut]\r\nURL=https://example.com/page?x=1\r\n");
    CHECK(domain::asChars(archive.files.at("Закладки/Сайт.URL")) ==
          "[InternetShortcut]\r\nURL=https://example.org/\r\n"); // расширение не дублируется
    CHECK(domain::asChars(archive.files.at("Закладки/example.com.url (вложения)/заметка.txt")) ==
          "к ссылке");
    CHECK(archive.files.contains("Закладки/a.txt"));

    // и сама ссылка с вложениями скачивается архивом вложений
    domain::MemorySink out;
    REQUIRE(f.services.importExport->exportZip(f.lease(s), site, out).has_value());
    const auto& attachments = f.zip.archives.back();
    CHECK(attachments.directories == std::set<std::string>{"example.com (вложения)/"});
    CHECK(attachments.files.size() == 1);
    CHECK(attachments.files.contains("example.com (вложения)/заметка.txt"));
}
