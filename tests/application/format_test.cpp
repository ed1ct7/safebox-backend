// Формат v2: enc_meta layout 2, AAD категорий и тегов, загрузка каталога вместе с тегами,
// контракт InMemoryVaultStore (тот же, что проверяется на sqlite в tests/infra).
#include <catch2/catch_test_macros.hpp>

#include "InMemoryVaultStore.hpp"
#include "safebox/domain/model/rules.hpp"
#include "safebox/domain/model/safe_format.hpp"
#include "safebox/infra/factories.hpp"
#include "sealing.hpp"
#include "store_contract.hpp"
#include "vault_session.hpp"

using namespace safebox;
using namespace safebox::app;
using namespace safebox::test;
using Code = domain::Error::Code;

namespace {

SessionKeys makeKeys(domain::CryptoSuite& crypto) {
    SessionKeys keys;
    auto master = crypto.generateKey();
    REQUIRE(master.has_value());
    keys.master = *master;
    keys.names = crypto.deriveSubkey(keys.master, domain::KeyPurpose::Names).value();
    keys.content = crypto.deriveSubkey(keys.master, domain::KeyPurpose::Content).value();
    keys.thumbnails = crypto.deriveSubkey(keys.master, domain::KeyPurpose::Thumbnails).value();
    return keys;
}

struct SealerFixture {
    SealerFixture() : crypto(infra::makeSodiumCrypto()), sealer(*crypto, makeKeys(*crypto)) {}

    std::unique_ptr<domain::CryptoSuite> crypto;
    Sealer sealer;
};

domain::EntryMeta fullMeta() {
    domain::EntryMeta meta;
    meta.kind = domain::Kind::Link;
    meta.mime = "text/html";
    meta.size = 123'456'789'012ull;
    meta.url = "https://example.com/a?b=1";
    meta.createdAt = 1'790'000'000'123;
    meta.modifiedAt = 1'790'000'999'456;
    meta.blobId = 17;
    meta.thumbBlobId = 18;
    meta.sourceModifiedAt = 1'700'000'000'000;
    meta.description = "Описание\nв две строки";
    meta.nameByUser = true;
    meta.descriptionByUser = true;
    meta.tags = {{3, true}, {8, false}, {1'000'000, true}};
    return meta;
}

void checkSame(const domain::EntryMeta& a, const domain::EntryMeta& b) {
    CHECK(a.kind == b.kind);
    CHECK(a.mime == b.mime);
    CHECK(a.size == b.size);
    CHECK(a.url == b.url);
    CHECK(a.createdAt == b.createdAt);
    CHECK(a.modifiedAt == b.modifiedAt);
    CHECK(a.blobId == b.blobId);
    CHECK(a.thumbBlobId == b.thumbBlobId);
    CHECK(a.sourceModifiedAt == b.sourceModifiedAt);
    CHECK(a.description == b.description);
    CHECK(a.nameByUser == b.nameByUser);
    CHECK(a.descriptionByUser == b.descriptionByUser);
    CHECK(a.tags == b.tags);
}

constexpr std::size_t kLayoutAt = 0;
constexpr std::size_t kKindAt = 1;
constexpr std::size_t kFlagsAt = 26; // u8 layout, u8 kind, u64 size, i64 created, i64 modified

void expectCorrupted(const domain::Bytes& bytes) {
    auto decoded = decodeMeta(bytes);
    REQUIRE_FALSE(decoded.has_value());
    CHECK(decoded.error().code == Code::IntegrityError);
}

} // namespace

TEST_CASE("enc_meta layout 2 round-trips every field", "[format][meta]") {
    const auto meta = fullMeta();
    const auto bytes = encodeMeta(meta);
    CHECK(static_cast<int>(bytes[kLayoutAt]) == 2);
    CHECK(static_cast<int>(bytes[kFlagsAt]) == 0x1F); // все пять флагов
    auto decoded = decodeMeta(bytes);
    REQUIRE(decoded.has_value());
    checkSame(*decoded, meta);

    // пустая мета: необязательных полей нет, флаги нулевые
    const domain::EntryMeta blank;
    const auto blankBytes = encodeMeta(blank);
    CHECK(static_cast<int>(blankBytes[kFlagsAt]) == 0);
    auto decodedBlank = decodeMeta(blankBytes);
    REQUIRE(decodedBlank.has_value());
    checkSame(*decodedBlank, blank);
    CHECK(decodedBlank->tags.empty());
    CHECK_FALSE(decodedBlank->sourceModifiedAt.has_value());

    // каждый флаг живет сам по себе
    domain::EntryMeta onlyName;
    onlyName.nameByUser = true;
    CHECK(static_cast<int>(encodeMeta(onlyName)[kFlagsAt]) == 0x08);
    domain::EntryMeta onlyDescription;
    onlyDescription.descriptionByUser = true;
    CHECK(static_cast<int>(encodeMeta(onlyDescription)[kFlagsAt]) == 0x10);
    domain::EntryMeta onlySource;
    onlySource.sourceModifiedAt = -5; // до 1970 - тоже значение
    auto decodedSource = decodeMeta(encodeMeta(onlySource));
    REQUIRE(decodedSource.has_value());
    CHECK(decodedSource->sourceModifiedAt == -5);
}

TEST_CASE("large descriptions and many tags survive the round trip", "[format][meta]") {
    domain::EntryMeta meta;
    meta.description.assign(domain::kMaxDescriptionBytes, 'z');
    for (domain::TagId id = 1; id <= static_cast<domain::TagId>(domain::kMaxTagsPerEntry); ++id) {
        meta.tags.push_back({id * 3, id % 2 == 0});
    }
    auto decoded = decodeMeta(encodeMeta(meta));
    REQUIRE(decoded.has_value());
    checkSame(*decoded, meta);
}

TEST_CASE("enc_meta decoding refuses anything but layout 2", "[format][meta]") {
    const auto good = encodeMeta(fullMeta());

    SECTION("old layout and unknown layouts") {
        for (const int layout : {0, 1, 3, 255}) {
            auto bytes = good;
            bytes[kLayoutAt] = static_cast<std::byte>(layout);
            expectCorrupted(bytes);
        }
    }
    SECTION("unknown flag bits") {
        for (const int bit : {0x20, 0x40, 0x80}) {
            auto bytes = good;
            bytes[kFlagsAt] = static_cast<std::byte>(static_cast<int>(bytes[kFlagsAt]) | bit);
            expectCorrupted(bytes);
        }
    }
    SECTION("unknown tag flag bits") {
        auto bytes = good;
        bytes.back() = std::byte{0x02}; // флаги последнего присвоения: допустим только bit0
        expectCorrupted(bytes);
    }
    SECTION("unknown kind") {
        auto bytes = good;
        bytes[kKindAt] = std::byte{5};
        expectCorrupted(bytes);
    }
    SECTION("trailing bytes") {
        auto bytes = good;
        bytes.push_back(std::byte{0});
        expectCorrupted(bytes);
    }
    SECTION("every truncation") {
        for (std::size_t n = 0; n < good.size(); ++n) {
            expectCorrupted(domain::Bytes(good.begin(), good.begin() + static_cast<long>(n)));
        }
    }
}

TEST_CASE("category and tag names are sealed under their own ids", "[format][seal]") {
    SealerFixture f;
    auto sealedCategory = f.sealer.sealCategoryName(2, "Люди");
    REQUIRE(sealedCategory.has_value());
    auto category = f.sealer.openCategory({2, *sealedCategory});
    REQUIRE(category.has_value());
    CHECK(category->id == 2);
    CHECK(category->name == "Люди");
    // имя не переставить в другую категорию
    CHECK(f.sealer.openCategory({3, *sealedCategory}).error().code == Code::IntegrityError);

    auto sealedTag = f.sealer.sealTagName(5, 2, "Иван");
    REQUIRE(sealedTag.has_value());
    auto tag = f.sealer.openTag({5, 2, *sealedTag});
    REQUIRE(tag.has_value());
    CHECK(tag->id == 5);
    CHECK(tag->categoryId == 2);
    CHECK(tag->name == "Иван");
    // открытый category_id входит в AAD: другая категория или другой тег - ошибка
    CHECK(f.sealer.openTag({5, 3, *sealedTag}).error().code == Code::IntegrityError);
    CHECK(f.sealer.openTag({6, 2, *sealedTag}).error().code == Code::IntegrityError);
    // и теги полей различают: имя категории не открывается как имя тега и наоборот
    CHECK(f.sealer.openTag({2, 0, *sealedCategory}).error().code == Code::IntegrityError);
    CHECK(f.sealer.openCategory({5, *sealedTag}).error().code == Code::IntegrityError);
    // и не открывается как имя записи
    CHECK(f.sealer.openEntry({.id = 2, .encName = *sealedCategory, .encMeta = *sealedCategory})
              .error()
              .code == Code::IntegrityError);
}

TEST_CASE("AAD of v2 fields", "[format][seal]") {
    CHECK(domain::kFormatVersion == 2);
    const auto category = domain::aad::categoryName(0x0102030405060708);
    REQUIRE(category.size() == 13);
    CHECK(static_cast<int>(category[0]) == 2); // версия u32 little-endian
    CHECK(static_cast<int>(category[4]) == 0x08);
    CHECK(static_cast<int>(category[12]) == 3);
    const auto tag = domain::aad::tagName(7, 9);
    REQUIRE(tag.size() == 21);
    CHECK(static_cast<int>(tag[4]) == 7);
    CHECK(static_cast<int>(tag[12]) == 4);
    CHECK(static_cast<int>(tag[13]) == 9); // category_id после тега поля
    CHECK(domain::aad::field(7, domain::FieldTag::Name) !=
          domain::aad::field(7, domain::FieldTag::Meta));
}

TEST_CASE("loadCatalog reads entries, categories and tags and checks their ids",
          "[format][catalog]") {
    SealerFixture f;
    InMemoryVaultStore store;
    const std::filesystem::path path = "catalog.safebox";
    REQUIRE(store.create(path, domain::SafeMeta{}).has_value());

    domain::CategoryId people = 0;
    domain::CategoryId places = 0;
    domain::TagId ivan = 0;
    domain::TagId kazan = 0;
    {
        auto uow = store.begin();
        REQUIRE(uow.has_value());
        domain::EntryRecord record;
        record.isFolder = true;
        const auto folder = (*uow)->entries().insert(record).value();
        domain::EntryMeta meta;
        meta.kind = domain::Kind::Folder;
        meta.description = "Летом";
        meta.tags = {{1, true}};
        REQUIRE((*uow)
                    ->entries()
                    .updateSealed(folder, *f.sealer.sealName(folder, "Поездки"),
                                  *f.sealer.sealMeta(folder, meta))
                    .has_value());

        auto& tags = (*uow)->tags();
        people = tags.insertCategory().value();
        places = tags.insertCategory().value();
        REQUIRE(
            tags.updateCategory(people, *f.sealer.sealCategoryName(people, "Люди")).has_value());
        REQUIRE(
            tags.updateCategory(places, *f.sealer.sealCategoryName(places, "Места")).has_value());
        ivan = tags.insertTag(people).value();
        kazan = tags.insertTag(places).value();
        REQUIRE(
            tags.updateTag(ivan, people, *f.sealer.sealTagName(ivan, people, "Иван")).has_value());
        REQUIRE(tags.updateTag(kazan, places, *f.sealer.sealTagName(kazan, places, "Казань"))
                    .has_value());
        REQUIRE((*uow)->commit().has_value());
    }

    auto loaded = loadCatalog(store, f.sealer);
    REQUIRE(loaded.has_value());
    const Catalog& catalog = **loaded;
    REQUIRE(catalog.nodes().size() == 1);
    const auto& folder = catalog.nodes().begin()->second;
    CHECK(folder.entry.name == "Поездки");
    CHECK(folder.foldedDescription == "летом");
    CHECK(catalog.categories().size() == 2);
    REQUIRE(catalog.findCategory(people) != nullptr);
    CHECK(catalog.findCategory(people)->category.name == "Люди");
    CHECK(catalog.findCategory(places)->folded == "места");
    REQUIRE(catalog.findTag(kazan) != nullptr);
    CHECK(catalog.findTag(kazan)->tag.name == "Казань");
    CHECK(catalog.findTag(kazan)->tag.categoryId == places);
    CHECK(catalog.findTag(ivan)->folded == "иван");
    CHECK(catalog.findTag(9999) == nullptr);
    CHECK(catalog.findCategory(9999) == nullptr);

    SECTION("a tag moved to another category by editing the open column") {
        store.database(path)->tags.at(ivan).categoryId = places;
        auto tampered = loadCatalog(store, f.sealer);
        REQUIRE_FALSE(tampered.has_value());
        CHECK(tampered.error().code == Code::IntegrityError);
    }
    SECTION("a category name swapped with another category's") {
        auto& db = *store.database(path);
        std::swap(db.categories.at(people).encName, db.categories.at(places).encName);
        auto tampered = loadCatalog(store, f.sealer);
        REQUIRE_FALSE(tampered.has_value());
        CHECK(tampered.error().code == Code::IntegrityError);
    }
    SECTION("a tag renamed to a foreign ciphertext") {
        auto& db = *store.database(path);
        db.tags.at(ivan).encName = db.tags.at(kazan).encName;
        auto tampered = loadCatalog(store, f.sealer);
        REQUIRE_FALSE(tampered.has_value());
        CHECK(tampered.error().code == Code::IntegrityError);
    }
}

TEST_CASE("InMemoryVaultStore behaves like the sqlite store", "[store][fake]") {
    const std::filesystem::path path = "contract.safebox";
    SECTION("entry update") {
        InMemoryVaultStore store;
        REQUIRE(store.create(path, domain::SafeMeta{}).has_value());
        checkEntryUpdate(store);
    }
    SECTION("blob remove") {
        InMemoryVaultStore store;
        REQUIRE(store.create(path, domain::SafeMeta{}).has_value());
        checkBlobRemove(store);
    }
    SECTION("tag repository") {
        InMemoryVaultStore store;
        REQUIRE(store.create(path, domain::SafeMeta{}).has_value());
        checkTagRepository(store);
    }
}
