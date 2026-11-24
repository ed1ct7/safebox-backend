// Формат: enc_meta layout 2 (чтение) и 3 (чтение и запись), AAD категорий и тегов, загрузка
// каталога вместе с тегами, контракт InMemoryVaultStore (тот же, что проверяется на sqlite в
// tests/infra).
#include <catch2/catch_test_macros.hpp>

#include <string_view>

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

// Поля layout 2.
domain::EntryMeta baseMeta() {
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

// Все поля, включая хвост layout 3.
domain::EntryMeta fullMeta() {
    auto meta = baseMeta();
    meta.source = "telegram";
    meta.sourceUrl = "https://t.me/c/123/456";
    meta.sourceDate = 1'600'000'000'000;
    domain::Sha256 hash{};
    for (std::size_t i = 0; i < hash.size(); ++i) {
        hash[i] = static_cast<std::byte>(0xA0 + i);
    }
    meta.sha256 = hash;
    meta.dimensions = domain::Dimensions{3840, 2160};
    meta.durationMs = 5'400'000;
    meta.trashedAt = 1'800'000'000'000;
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
    CHECK(a.source == b.source);
    CHECK(a.sourceUrl == b.sourceUrl);
    CHECK(a.sourceDate == b.sourceDate);
    CHECK(a.sha256 == b.sha256);
    CHECK(a.dimensions == b.dimensions);
    CHECK(a.durationMs == b.durationMs);
    CHECK(a.trashedAt == b.trashedAt);
}

void checkNoLayout3Fields(const domain::EntryMeta& meta) {
    CHECK(meta.source.empty());
    CHECK(meta.sourceUrl.empty());
    CHECK_FALSE(meta.sourceDate.has_value());
    CHECK_FALSE(meta.sha256.has_value());
    CHECK_FALSE(meta.dimensions.has_value());
    CHECK_FALSE(meta.durationMs.has_value());
    CHECK_FALSE(meta.trashedAt.has_value());
}

constexpr std::size_t kLayoutAt = 0;
constexpr std::size_t kKindAt = 1;
constexpr std::size_t kFlagsAt = 26; // u8 layout, u8 kind, u64 size, i64 created, i64 modified
// Хвост пустой меты: 27 байт фиксированной части + mime 2 + url 4 + описание 4 + теги 2.
constexpr std::size_t kBlankExtAt = 39;

domain::Bytes fromHex(std::string_view hex) {
    domain::Bytes out;
    auto nibble = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
    for (std::size_t i = 0; i < hex.size();) {
        if (hex[i] == ' ') {
            ++i;
            continue;
        }
        out.push_back(static_cast<std::byte>(nibble(hex[i]) * 16 + nibble(hex[i + 1])));
        i += 2;
    }
    return out;
}

// Пример из документа о формате: ссылка layout 2 (так ее записала версия 2.x), 117 байт.
domain::Bytes layout2Example() {
    return fromHex(
        "02 04 00 00 00 00 00 00 00 00 00 68 e5 cf 8b 01 00 00 60 52 e6 cf 8b 01 00 00 02 07 00 "
        "00 00 00 00 00 00 1d 00 61 70 70 6c 69 63 61 74 69 6f 6e 2f 69 6e 74 65 72 6e 65 74 2d "
        "73 68 6f 72 74 63 75 74 14 00 00 00 68 74 74 70 73 3a 2f 2f 65 78 61 6d 70 6c 65 2e 63 "
        "6f 6d 2f 0c 00 00 00 d0 9f d1 80 d0 b8 d0 bc d0 b5 d1 80 01 00 03 00 00 00 00 00 00 00 "
        "01");
}

// Та же ссылка в layout 3 с источником "telegram" и датой в источнике (второй пример).
domain::Bytes layout3Example() {
    auto bytes = layout2Example();
    bytes[kLayoutAt] = std::byte{3};
    const auto tail = fromHex("05 00 08 00 74 65 6c 65 67 72 61 6d c0 25 d6 cf 8b 01 00 00");
    bytes.insert(bytes.end(), tail.begin(), tail.end());
    return bytes;
}

domain::EntryMeta exampleMeta() {
    domain::EntryMeta meta;
    meta.kind = domain::Kind::Link;
    meta.createdAt = 1'700'000'000'000;
    meta.modifiedAt = 1'700'000'060'000;
    meta.thumbBlobId = 7;
    meta.mime = "application/internet-shortcut";
    meta.url = "https://example.com/";
    meta.description = "Пример";
    meta.tags = {{3, true}};
    return meta;
}

void expectCorrupted(const domain::Bytes& bytes) {
    auto decoded = decodeMeta(bytes);
    REQUIRE_FALSE(decoded.has_value());
    CHECK(decoded.error().code == Code::IntegrityError);
}

} // namespace

TEST_CASE("enc_meta layout 3 round-trips every field", "[format][meta]") {
    const auto meta = fullMeta();
    const auto bytes = encodeMeta(meta);
    CHECK(static_cast<int>(bytes[kLayoutAt]) == 3);
    CHECK(static_cast<int>(bytes[kFlagsAt]) == 0x1F); // все пять флагов
    auto decoded = decodeMeta(bytes);
    REQUIRE(decoded.has_value());
    checkSame(*decoded, meta);

    // пустая мета: необязательных полей нет, флаги нулевые, хвост - два нулевых байта
    const domain::EntryMeta blank;
    const auto blankBytes = encodeMeta(blank);
    CHECK(static_cast<int>(blankBytes[kFlagsAt]) == 0);
    REQUIRE(blankBytes.size() == kBlankExtAt + 2);
    CHECK(blankBytes[kBlankExtAt] == std::byte{0});
    CHECK(blankBytes[kBlankExtAt + 1] == std::byte{0});
    auto decodedBlank = decodeMeta(blankBytes);
    REQUIRE(decodedBlank.has_value());
    checkSame(*decodedBlank, blank);
    CHECK(decodedBlank->tags.empty());
    CHECK_FALSE(decodedBlank->sourceModifiedAt.has_value());
    checkNoLayout3Fields(*decodedBlank);

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

TEST_CASE("every layout 3 field lives on its own bit", "[format][meta]") {
    struct Case {
        const char* name;
        void (*set)(domain::EntryMeta&);
        int bit;
    };
    const Case cases[] = {
        {"source", [](domain::EntryMeta& m) { m.source = "youtube"; }, 0x01},
        {"sourceUrl", [](domain::EntryMeta& m) { m.sourceUrl = "https://youtu.be/x"; }, 0x02},
        {"sourceDate", [](domain::EntryMeta& m) { m.sourceDate = 0; }, 0x04},
        {"sha256", [](domain::EntryMeta& m) { m.sha256 = domain::Sha256{}; }, 0x08},
        {"dimensions", [](domain::EntryMeta& m) { m.dimensions = domain::Dimensions{}; }, 0x10},
        {"durationMs", [](domain::EntryMeta& m) { m.durationMs = 0; }, 0x20},
        {"trashedAt", [](domain::EntryMeta& m) { m.trashedAt = -1; }, 0x40},
    };
    for (const auto& c : cases) {
        INFO(c.name);
        domain::EntryMeta meta;
        c.set(meta);
        const auto bytes = encodeMeta(meta);
        REQUIRE(bytes.size() > kBlankExtAt + 2);
        CHECK(static_cast<int>(bytes[kBlankExtAt]) == c.bit);
        CHECK(static_cast<int>(bytes[kBlankExtAt + 1]) == 0);
        auto decoded = decodeMeta(bytes);
        REQUIRE(decoded.has_value());
        checkSame(*decoded, meta);
    }
}

TEST_CASE("enc_meta bytes match the examples in the format document", "[format][meta]") {
    // layout 2, записанный версией 2.x, читается; новых полей у такой записи нет
    auto old = decodeMeta(layout2Example());
    REQUIRE(old.has_value());
    checkSame(*old, exampleMeta());
    checkNoLayout3Fields(*old);

    // пишется уже layout 3: тот же префикс и хвост extFlags
    auto withSource = exampleMeta();
    withSource.source = "telegram";
    withSource.sourceDate = 1'699'999'000'000;
    CHECK(encodeMeta(withSource) == layout3Example());
    auto decoded = decodeMeta(layout3Example());
    REQUIRE(decoded.has_value());
    checkSame(*decoded, withSource);

    // без новых полей layout 3 = layout 2 с другим байтом layout и двумя нулями в конце
    auto rewritten = layout2Example();
    rewritten[kLayoutAt] = std::byte{3};
    rewritten.push_back(std::byte{0});
    rewritten.push_back(std::byte{0});
    CHECK(encodeMeta(*old) == rewritten);
}

TEST_CASE("any layout 2 meta decodes and is rewritten as layout 3 without loss", "[format][meta]") {
    // layout 2 получаем из layout 3 без хвоста: префиксы раскладок совпадают
    for (const auto& meta : {baseMeta(), domain::EntryMeta{}}) {
        auto bytes = encodeMeta(meta);
        REQUIRE(bytes.size() >= 2);
        bytes.resize(bytes.size() - 2);
        bytes[kLayoutAt] = std::byte{2};
        auto decoded = decodeMeta(bytes);
        REQUIRE(decoded.has_value());
        checkSame(*decoded, meta);
        checkNoLayout3Fields(*decoded);
        CHECK(encodeMeta(*decoded) == encodeMeta(meta));
    }
}

TEST_CASE("large descriptions and many tags survive the round trip", "[format][meta]") {
    auto meta = fullMeta();
    meta.description.assign(domain::kMaxDescriptionBytes, 'z');
    meta.tags.clear();
    for (domain::TagId id = 1; id <= static_cast<domain::TagId>(domain::kMaxTagsPerEntry); ++id) {
        meta.tags.push_back({id * 3, id % 2 == 0});
    }
    meta.sourceUrl.assign(domain::kMaxUrlBytes, 'u');
    auto decoded = decodeMeta(encodeMeta(meta));
    REQUIRE(decoded.has_value());
    checkSame(*decoded, meta);
}

TEST_CASE("enc_meta decoding refuses anything but layouts 2 and 3", "[format][meta]") {
    const auto good = encodeMeta(fullMeta());

    SECTION("old layout and unknown layouts") {
        for (const int layout : {0, 1, 4, 255}) {
            auto bytes = good;
            bytes[kLayoutAt] = static_cast<std::byte>(layout);
            expectCorrupted(bytes);
        }
    }
    SECTION("layout 2 with a layout 3 tail and layout 3 without one") {
        auto bytes = good;
        bytes[kLayoutAt] = std::byte{2};
        expectCorrupted(bytes); // хвост для layout 2 - лишние байты
        auto noTail = layout2Example();
        noTail[kLayoutAt] = std::byte{3};
        expectCorrupted(noTail);
    }
    SECTION("unknown flag bits") {
        for (const int bit : {0x20, 0x40, 0x80}) {
            auto bytes = good;
            bytes[kFlagsAt] = static_cast<std::byte>(static_cast<int>(bytes[kFlagsAt]) | bit);
            expectCorrupted(bytes);
        }
    }
    SECTION("unknown extFlags bits") {
        // у меты без новых полей хвост - последние два байта
        const auto plain = encodeMeta(baseMeta());
        for (const int bit : {0x0080, 0x0100, 0x8000}) {
            auto bytes = plain;
            bytes[bytes.size() - 2] = static_cast<std::byte>(bit & 0xFF);
            bytes[bytes.size() - 1] = static_cast<std::byte>(bit >> 8);
            expectCorrupted(bytes);
        }
    }
    SECTION("empty source or sourceUrl under a set bit") {
        // правильная запись пустые строки не пишет, поэтому это подмена
        auto source = encodeMeta(domain::EntryMeta{});
        source[kBlankExtAt] = std::byte{0x01};
        source.insert(source.end(), 2, std::byte{0}); // u16 len = 0
        expectCorrupted(source);
        auto url = encodeMeta(domain::EntryMeta{});
        url[kBlankExtAt] = std::byte{0x02};
        url.insert(url.end(), 4, std::byte{0}); // u32 len = 0
        expectCorrupted(url);
    }
    SECTION("unknown tag flag bits") {
        auto bytes = encodeMeta(baseMeta());
        // флаги последнего присвоения стоят перед двумя байтами extFlags: допустим только bit0
        bytes[bytes.size() - 3] = std::byte{0x02};
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

TEST_CASE("AAD of sealed fields", "[format][seal]") {
    CHECK(domain::kSealVersion == 2); // в AAD - версия раскладки шифра, не схемы
    CHECK(domain::kFormatVersion == 4);
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
        REQUIRE(tags.updateCategory(people, *f.sealer.sealCategoryName(people, "Люди"), {})
                    .has_value());
        REQUIRE(tags.updateCategory(places, *f.sealer.sealCategoryName(places, "Места"), {})
                    .has_value());
        ivan = tags.insertTag(people).value();
        kazan = tags.insertTag(places).value();
        REQUIRE(tags.updateTag(ivan, people, *f.sealer.sealTagName(ivan, people, "Иван"), {})
                    .has_value());
        REQUIRE(tags.updateTag(kazan, places, *f.sealer.sealTagName(kazan, places, "Казань"), {})
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
