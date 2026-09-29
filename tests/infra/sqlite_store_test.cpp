#include <catch2/catch_test_macros.hpp>

#include "safebox/domain/model/safe_format.hpp"
#include "safebox/infra/factories.hpp"
#include "store_contract.hpp"
#include "temp_dir.hpp"

using namespace safebox;
using namespace safebox::test;
using Code = domain::Error::Code;

namespace {

domain::SafeMeta sampleMeta() {
    domain::SafeMeta meta;
    meta.kdf = domain::kMinimalKdf;
    meta.salt = domain::Bytes(domain::kSaltSize, std::byte{7});
    meta.chunkSize = 1024 * 1024;
    meta.envelope = domain::Bytes(domain::kSealOverhead + domain::kKeySize, std::byte{9});
    return meta;
}

std::uint32_t be32(const std::string& h, std::size_t at) {
    return (std::uint32_t(static_cast<unsigned char>(h[at])) << 24) |
           (std::uint32_t(static_cast<unsigned char>(h[at + 1])) << 16) |
           (std::uint32_t(static_cast<unsigned char>(h[at + 2])) << 8) |
           std::uint32_t(static_cast<unsigned char>(h[at + 3]));
}

domain::BlobId writeBlob(domain::VaultStore& store, std::size_t chunks, std::size_t chunkBytes) {
    auto writer = store.blobs().create();
    REQUIRE(writer.has_value());
    for (std::size_t i = 0; i < chunks; ++i) {
        REQUIRE((*writer)->append(domain::Bytes(chunkBytes, std::byte(i + 1))).has_value());
    }
    REQUIRE((*writer)->finish(chunks * chunkBytes).has_value());
    return (*writer)->id();
}

domain::EntryId insertEntry(domain::VaultStore& store, std::optional<domain::EntryId> parent,
                            bool folder, std::optional<domain::BlobId> blob = std::nullopt) {
    auto uow = store.begin();
    REQUIRE(uow.has_value());
    domain::EntryRecord record;
    record.parentId = parent;
    record.isFolder = folder;
    record.blobId = blob;
    record.encName = domain::toBytes("name");
    record.encMeta = domain::toBytes("meta");
    auto id = (*uow)->entries().insert(record);
    REQUIRE(id.has_value());
    if (blob) {
        REQUIRE((*uow)->blobs().promote(*blob).has_value());
    }
    REQUIRE((*uow)->commit().has_value());
    return *id;
}

} // namespace

TEST_CASE("created file carries the SafeBox pragmas and no side files after close",
          "[sqlite][format]") {
    TempDir dir;
    const auto path = dir / u8"Мой сейф.safebox"; // u8: на Windows узкий литерал - ANSI
    auto store = infra::makeSqliteVaultStore();
    REQUIRE(store->create(path, sampleMeta()).has_value());
    CHECK(store->isOpen());

    auto meta = store->meta();
    REQUIRE(meta.has_value());
    CHECK(meta->kdf == domain::kMinimalKdf);
    CHECK(meta->chunkSize == 1024 * 1024);
    CHECK(meta->salt == sampleMeta().salt);
    CHECK(meta->envelope == sampleMeta().envelope);

    store->close();
    CHECK_FALSE(store->isOpen());
    const auto header = readFile(path).substr(0, 100);
    REQUIRE(header.size() == 100);
    CHECK(header.substr(0, 16) == std::string("SQLite format 3\0", 16));
    CHECK(be32(header, 68) == static_cast<std::uint32_t>(domain::kApplicationId));
    CHECK(be32(header, 60) == domain::kFormatVersion);
    CHECK(static_cast<unsigned char>(header[16]) == 0); // page_size 1 = 65536
    CHECK(static_cast<unsigned char>(header[17]) == 1);
    CHECK(be32(header, 52) != 0); // auto_vacuum включен
    CHECK(be32(header, 64) != 0); // ...в режиме INCREMENTAL
    for (const char* suffix : {"-journal", "-wal", "-shm"}) {
        auto side = path;
        side += suffix;
        CHECK_FALSE(std::filesystem::exists(side));
    }
}

TEST_CASE("create refuses existing files and missing folders", "[sqlite]") {
    TempDir dir;
    auto store = infra::makeSqliteVaultStore();
    writeFile(dir / "taken.safebox", "x");
    auto taken = store->create(dir / "taken.safebox", sampleMeta());
    REQUIRE_FALSE(taken.has_value());
    CHECK(taken.error().code == Code::AlreadyExists);
    auto noDir = store->create(dir / "missing" / "a.safebox", sampleMeta());
    REQUIRE_FALSE(noDir.has_value());
    CHECK(noDir.error().code == Code::InvalidArgument);
    CHECK_FALSE(store->isOpen());
}

TEST_CASE("open recognises foreign files before any password", "[sqlite][UF-2]") {
    TempDir dir;
    auto store = infra::makeSqliteVaultStore();

    auto missing = store->open(dir / "nope.safebox");
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code == Code::NotFound);

    writeFile(dir / "photo.jpg", std::string(4096, '\xAB'));
    auto random = store->open(dir / "photo.jpg");
    REQUIRE_FALSE(random.has_value());
    CHECK(random.error().code == Code::NotASafe);

    writeFile(dir / "empty.safebox", "");
    CHECK(store->open(dir / "empty.safebox").error().code == Code::NotASafe);

    // настоящая SQLite-база, но с чужим application_id
    auto sqliteHeader = std::string("SQLite format 3\0", 16) + std::string(84, '\0');
    sqliteHeader[16] = 0x10; // page size 4096
    writeFile(dir / "other.db", sqliteHeader + std::string(4096 - 100, '\0'));
    CHECK(store->open(dir / "other.db").error().code == Code::NotASafe);

    std::filesystem::create_directories(dir / "folder.safebox");
    CHECK(store->open(dir / "folder.safebox").error().code == Code::NotASafe);

    auto inspect = store->inspect(dir / "photo.jpg");
    REQUIRE_FALSE(inspect.has_value());
    CHECK(inspect.error().code == Code::NotASafe);
    CHECK_FALSE(store->isOpen());
}

TEST_CASE("exclusive locking: one process per safe; inspect reads without opening",
          "[sqlite][locking]") {
    TempDir dir;
    const auto path = dir / "vault.safebox";
    auto first = infra::makeSqliteVaultStore();
    REQUIRE(first->create(path, sampleMeta()).has_value());
    first->close();

    auto viewer = infra::makeSqliteVaultStore();
    auto inspected = viewer->inspect(path);
    REQUIRE(inspected.has_value());
    CHECK(inspected->envelope == sampleMeta().envelope);
    CHECK_FALSE(viewer->isOpen());

    REQUIRE(first->open(path).has_value());
    auto second = infra::makeSqliteVaultStore();
    auto busy = second->open(path);
    REQUIRE_FALSE(busy.has_value());
    CHECK(busy.error().code == Code::IoError);

    first->close();
    CHECK(second->open(path).has_value());
}

TEST_CASE("unit of work rolls back unless committed", "[sqlite][uow]") {
    TempDir dir;
    auto store = infra::makeSqliteVaultStore();
    REQUIRE(store->create(dir / "v.safebox", sampleMeta()).has_value());
    {
        auto uow = store->begin();
        REQUIRE(uow.has_value());
        domain::EntryRecord record;
        record.isFolder = true;
        REQUIRE((*uow)->entries().insert(record).has_value());
        auto nested = store->blobs().info(1); // тот же поток внутри транзакции - ошибка, не дедлок
        REQUIRE_FALSE(nested.has_value());
        CHECK(nested.error().code == Code::Internal);
    } // без commit
    auto uow = store->begin();
    REQUIRE(uow.has_value());
    CHECK((*uow)->entries().count().value() == 0);
    uow->reset();

    const auto first = insertEntry(*store, std::nullopt, true);
    const auto second = insertEntry(*store, std::nullopt, true);
    CHECK(second > first);
    auto tx = store->begin();
    REQUIRE((*tx)->entries().removeSubtree(second).has_value());
    REQUIRE((*tx)->commit().has_value());
    tx->reset();
    CHECK(insertEntry(*store, std::nullopt, true) > second); // id не переиспользуются

    auto bad = store->begin();
    domain::EntryRecord orphan;
    orphan.parentId = 999;
    auto inserted = (*bad)->entries().insert(orphan);
    REQUIRE_FALSE(inserted.has_value());
    CHECK(inserted.error().code == Code::NotFound);
}

TEST_CASE("pending blobs become ready only via promote; gc removes orphans", "[sqlite][blobs]") {
    TempDir dir;
    const auto path = dir / "v.safebox";
    auto store = infra::makeSqliteVaultStore();
    REQUIRE(store->create(path, sampleMeta()).has_value());

    const auto kept = writeBlob(*store, 2, 1000);
    CHECK(store->blobs().info(kept)->status == domain::BlobStatus::Pending);
    CHECK(store->blobs().info(kept)->chunkCount == 2);
    CHECK(store->blobs().info(kept)->size == 2000);
    insertEntry(*store, std::nullopt, false, kept);
    CHECK(store->blobs().info(kept)->status == domain::BlobStatus::Ready);

    auto chunk = store->blobs().readChunk(kept, 1);
    REQUIRE(chunk.has_value());
    CHECK(*chunk == domain::Bytes(1000, std::byte{2}));
    CHECK(store->blobs().readChunk(kept, 2).error().code == Code::NotFound);

    const auto dropped = writeBlob(*store, 1, 10);
    REQUIRE(store->blobs().discard(dropped).has_value());
    CHECK(store->blobs().info(dropped).error().code == Code::NotFound);

    // "обрыв посреди импорта": pending остается в файле после закрытия
    const auto crashed = writeBlob(*store, 3, 5000);
    store->close();
    REQUIRE(store->open(path).has_value());
    CHECK(store->blobs().info(crashed)->status == domain::BlobStatus::Pending);
    CHECK(store->blobs().gcPending().value() == 1);
    CHECK(store->blobs().info(crashed).error().code == Code::NotFound);
    CHECK(store->blobs().info(kept)->status == domain::BlobStatus::Ready);
    CHECK(store->blobs().gcPending().value() == 0);
}

TEST_CASE("removeSubtree cascades and compact shrinks the file", "[sqlite][cascade]") {
    TempDir dir;
    const auto path = dir / "v.safebox";
    auto store = infra::makeSqliteVaultStore();
    REQUIRE(store->create(path, sampleMeta()).has_value());

    const auto keep = insertEntry(*store, std::nullopt, false, writeBlob(*store, 1, 100));
    const auto folder = insertEntry(*store, std::nullopt, true);
    const auto sub = insertEntry(*store, folder, true);
    const auto bigA = writeBlob(*store, 3, 1024 * 1024);
    const auto bigB = writeBlob(*store, 3, 1024 * 1024);
    insertEntry(*store, folder, false, bigA);
    insertEntry(*store, sub, false, bigB);
    store->close();
    const auto sizeBefore = std::filesystem::file_size(path);
    REQUIRE(store->open(path).has_value());

    {
        auto uow = store->begin();
        auto removed = (*uow)->entries().removeSubtree(folder);
        REQUIRE(removed.has_value());
        CHECK(removed->entries.size() == 4);
        CHECK(removed->blobs.size() == 2);
        REQUIRE((*uow)->commit().has_value());
    }
    CHECK(store->blobs().info(bigA).error().code == Code::NotFound);
    CHECK(store->blobs().readChunk(bigB, 0).error().code == Code::NotFound);
    REQUIRE(store->compact().has_value());
    {
        auto uow = store->begin();
        CHECK((*uow)->entries().count().value() == 1);
        CHECK((*uow)->entries().get(keep).has_value());
        CHECK((*uow)->entries().get(sub).error().code == Code::NotFound);
    }
    store->close();
    const auto sizeAfter = std::filesystem::file_size(path);
    CHECK(sizeAfter + 5 * 1024 * 1024 < sizeBefore);
}

TEST_CASE("meta can be re-saved in a transaction (password change)", "[sqlite][UF-12]") {
    TempDir dir;
    const auto path = dir / "v.safebox";
    auto store = infra::makeSqliteVaultStore();
    REQUIRE(store->create(path, sampleMeta()).has_value());
    auto next = sampleMeta();
    next.salt = domain::Bytes(domain::kSaltSize, std::byte{1});
    next.envelope = domain::Bytes(domain::kSealOverhead + domain::kKeySize, std::byte{2});
    {
        auto uow = store->begin();
        REQUIRE((*uow)->saveMeta(next).has_value());
        // без commit - откат
    }
    CHECK(store->meta()->salt == sampleMeta().salt);
    {
        auto uow = store->begin();
        REQUIRE((*uow)->saveMeta(next).has_value());
        REQUIRE((*uow)->commit().has_value());
    }
    store->close();
    REQUIRE(store->open(path).has_value());
    CHECK(store->meta()->salt == next.salt);
    CHECK(store->meta()->envelope == next.envelope);
}

TEST_CASE("sqlite store passes the shared storage contract", "[sqlite][contract]") {
    TempDir dir;
    auto store = infra::makeSqliteVaultStore();
    REQUIRE(store->create(dir / "v.safebox", sampleMeta()).has_value());
    SECTION("entry update") {
        checkEntryUpdate(*store);
    }
    SECTION("blob remove") {
        checkBlobRemove(*store);
    }
    SECTION("tag repository") {
        checkTagRepository(*store);
    }
}

TEST_CASE("categories and tags persist; removing a category cascades on disk",
          "[sqlite][tags][format]") {
    TempDir dir;
    const auto path = dir / "v.safebox";
    auto store = infra::makeSqliteVaultStore();
    REQUIRE(store->create(path, sampleMeta()).has_value());

    domain::CategoryId doomed = 0;
    domain::CategoryId kept = 0;
    {
        auto uow = store->begin();
        auto& tags = (*uow)->tags();
        doomed = tags.insertCategory().value();
        kept = tags.insertCategory().value();
        REQUIRE(tags.updateCategory(doomed, domain::toBytes("doomed")).has_value());
        REQUIRE(tags.updateCategory(kept, domain::toBytes("kept")).has_value());
        for (const auto category : {doomed, kept, doomed}) {
            const auto id = tags.insertTag(category).value();
            REQUIRE(tags.updateTag(id, category, domain::toBytes("tag-" + std::to_string(id)))
                        .has_value());
        }
        REQUIRE((*uow)->commit().has_value());
    }
    store->close();
    REQUIRE(store->open(path).has_value());
    {
        auto uow = store->begin();
        auto& tags = (*uow)->tags();
        REQUIRE(tags.categories()->size() == 2);
        REQUIRE(tags.tags()->size() == 3);
        REQUIRE(tags.removeCategory(doomed).has_value());
        REQUIRE((*uow)->commit().has_value());
    }
    store->close();
    REQUIRE(store->open(path).has_value());
    auto uow = store->begin();
    auto categories = (*uow)->tags().categories();
    REQUIRE(categories->size() == 1);
    CHECK((*categories)[0].id == kept);
    CHECK((*categories)[0].encName == domain::toBytes("kept"));
    auto remaining = (*uow)->tags().tags();
    REQUIRE(remaining->size() == 1);
    CHECK((*remaining)[0].categoryId == kept);
    CHECK((*remaining)[0].encName == domain::toBytes("tag-2"));
}

TEST_CASE("a file with user_version 1 is refused with a clear message", "[sqlite][format]") {
    CHECK(domain::kFormatVersion == 2);
    TempDir dir;
    const auto path = dir / "old.safebox";
    {
        auto store = infra::makeSqliteVaultStore();
        REQUIRE(store->create(path, sampleMeta()).has_value());
    }
    auto bytes = readFile(path);
    REQUIRE(bytes.size() >= 100);
    REQUIRE(be32(bytes, 60) == 2);

    const auto withVersion = [&](char version) {
        auto patched = bytes;
        patched[60] = patched[61] = patched[62] = '\0';
        patched[63] = version;
        writeFile(path, patched);
    };

    withVersion(1);
    auto store = infra::makeSqliteVaultStore();
    auto opened = store->open(path);
    REQUIRE_FALSE(opened.has_value());
    CHECK(opened.error().code == Code::NotASafe);
    CHECK(opened.error().message ==
          "Сейф в старом формате (версия 1) не поддерживается - создайте новый сейф");
    CHECK_FALSE(store->isOpen());
    auto inspected = store->inspect(path);
    REQUIRE_FALSE(inspected.has_value());
    CHECK(inspected.error().code == Code::NotASafe);
    CHECK(inspected.error().message == opened.error().message);

    // файл новее - другое сообщение, как и раньше
    withVersion(3);
    auto newer = store->open(path);
    REQUIRE_FALSE(newer.has_value());
    CHECK(newer.error().code == Code::NotASafe);
    CHECK(newer.error().message.find("новой версией") != std::string::npos);

    withVersion(0);
    CHECK(store->open(path).error().message == "Файл не является сейфом SafeBox");

    withVersion(2); // и без подмены файл открывается
    REQUIRE(store->open(path).has_value());
}
