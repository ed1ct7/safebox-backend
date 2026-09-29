#include <thread>

#include "app_fixture.hpp"

using namespace safebox;
using namespace safebox::test;
using Code = domain::Error::Code;

namespace {

constexpr std::string_view kShortcut = "[InternetShortcut]\r\nURL=https://example.com/a?b=1\r\n";

domain::BlobId blobOf(const domain::Entry& e) {
    REQUIRE(e.meta.blobId.has_value());
    return *e.meta.blobId;
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
    auto intoFile =
        f.services.importExport->beginImport(f.lease(s), f.entryNamed(s, "good1.txt").id);
    REQUIRE_FALSE(intoFile.has_value());
    CHECK(intoFile.error().code == Code::InvalidArgument);
}

TEST_CASE("names from disk are sanitized instead of rejected", "[import][UF-7]") {
    AppFixture f;
    auto s = f.createSafe();
    auto result = f.importFiles(s, {{"dir/ we:ird?.txt ", "x"}});
    CHECK(result.imported == 1);
    const auto dir = f.entryNamed(s, "dir");
    CHECK(f.entryNamed(s, "we_ird_.txt", dir.id).meta.size == 1);
}

TEST_CASE("re-import merges folders and skips files already there", "[import][UF-7]") {
    AppFixture f;
    auto s = f.createSafe();

    REQUIRE(f.importFiles(s, {{"pict/a.txt", "AAA"}, {"pict/b.txt", "BBB"}}).imported == 2);

    // на диске появился новый файл, папку импортируют еще раз целиком
    const auto second =
        f.importFiles(s, {{"pict/a.txt", "AAA"}, {"pict/b.txt", "BBB"}, {"pict/c.txt", "CCC"}});
    CHECK(second.imported == 1); // только новый
    CHECK(second.skipped == 2);  // уже были
    CHECK(second.failed == 0);

    auto root = f.services.entries->list(f.lease(s), std::nullopt);
    REQUIRE(root.has_value());
    REQUIRE(root->entries.size() == 1); // pict не задвоилась
    auto files = f.services.entries->list(f.lease(s), root->entries[0].id);
    REQUIRE(files.has_value());
    REQUIRE(files->entries.size() == 3);
    CHECK(files->entries[0].name == "a.txt");
    CHECK(files->entries[0].meta.size == 3);
    CHECK(f.readContent(s, files->entries[0].id) == "AAA"); // содержимое прежнее
    CHECK(f.store.pendingBlobs(f.safePath()) == 0);         // дубликаты не оставляют мусора

    SECTION("case does not matter: PICT is the same folder") {
        const auto third = f.importFiles(s, {{"PICT/d.txt", "DDDD"}});
        CHECK(third.imported == 1);
        auto after = f.services.entries->list(f.lease(s), std::nullopt);
        REQUIRE(after.has_value());
        REQUIRE(after->entries.size() == 1); // вторая папка не создана
        CHECK(f.entryNamed(s, "d.txt", root->entries[0].id).meta.size == 4);
    }

    SECTION("same name with different size is not a duplicate") {
        const auto r = f.importFiles(s, {{"pict/a.txt", "AAAA"}});
        CHECK(r.imported == 1);
        CHECK(r.skipped == 0);

        // теперь в папке два a.txt: совпадение с любым из них - дубликат
        const auto again = f.importFiles(s, {{"pict/a.txt", "AAAA"}, {"pict/A.TXT", "AAA"}});
        CHECK(again.imported == 0);
        CHECK(again.skipped == 2);
    }

    SECTION("same name and size but other bytes is a new version, not a duplicate") {
        const auto r = f.importFiles(s, {{"pict/a.txt", "AAB"}});
        CHECK(r.imported == 1);
        CHECK(r.skipped == 0);
    }

    SECTION("duplicate inside one batch is skipped too") {
        const auto r = f.importFiles(s, {{"x.txt", "same"}, {"x.txt", "same"}});
        CHECK(r.imported == 1);
        CHECK(r.skipped == 1);
    }

    SECTION("folders created by this import are merged regardless of case") {
        const auto r = f.importFiles(s, {{"New/a.txt", "1"}, {"NEW/b.txt", "2"}});
        CHECK(r.imported == 2);
        auto after = f.services.entries->list(f.lease(s), std::nullopt);
        REQUIRE(after.has_value());
        CHECK(after->entries.size() == 2); // pict и New
    }
}

TEST_CASE("duplicates are compared byte by byte across chunks", "[import][UF-7]") {
    AppFixture f;
    auto s = f.createSafe();
    const auto big = randomBytes(kTestChunk * 3 + 17);
    REQUIRE(f.importFiles(s, {{"big.bin", big}}).imported == 1);

    CHECK(f.importFiles(s, {{"big.bin", big}}).skipped == 1);

    // отличие только в последнем куске
    auto tail = big;
    tail.back() = static_cast<char>(tail.back() ^ 1);
    // отличие только в первом куске
    auto head = big;
    head.front() = static_cast<char>(head.front() ^ 1);
    // тот же префикс, но короче / длиннее
    const auto shorter = big.substr(0, kTestChunk * 2);
    const auto longer = big + "x";
    const auto r = f.importFiles(
        s, {{"big.bin", tail}, {"big.bin", head}, {"big.bin", shorter}, {"big.bin", longer}});
    CHECK(r.imported == 4);
    CHECK(r.skipped == 0);
    CHECK(f.store.pendingBlobs(f.safePath()) == 0);
}

TEST_CASE("lock during import cancels it and leaves no garbage", "[import][UF-13][concurrency]") {
    AppFixture f;
    auto s = f.createSafe();
    auto import = f.services.importExport->beginImport(f.lease(s), std::nullopt);
    REQUIRE(import.has_value());
    REQUIRE((*import)->beginFile("done.txt").has_value());
    REQUIRE((*import)->write(domain::asBytes("finished before lock")).has_value());
    REQUIRE((*import)->endFile().has_value());

    REQUIRE((*import)->beginFile("big.bin").has_value());
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

TEST_CASE("zip export streams the whole folder with unique names", "[export][UF-11]") {
    AppFixture f;
    auto s = f.createSafe();
    const auto big = randomBytes(2500);
    REQUIRE(f.importFiles(s, {{"Папка/a.txt", "1"},
                              {"Папка/A.txt", "2"},
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
    REQUIRE_FALSE(notFolder.has_value());
    CHECK(notFolder.error().code == Code::InvalidArgument);

    auto asFile =
        f.services.importExport->openContent(f.lease(s), folder.id, app::ContentVariant::Original);
    REQUIRE_FALSE(asFile.has_value());
    CHECK(asFile.error().code == Code::InvalidArgument);
}
