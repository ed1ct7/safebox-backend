// Одни и те же проверки для настоящего SqliteVaultStore (tests/infra) и InMemoryVaultStore
// (tests/application): фейк обязан вести себя как sqlite. store - уже открытое хранилище.
#pragma once

#include <catch2/catch_test_macros.hpp>

#include "safebox/domain/ports/storage.hpp"

namespace safebox::test {

inline domain::EntryId storeInsert(domain::VaultStore& store, std::optional<domain::EntryId> parent,
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

inline domain::BlobId storeWriteBlob(domain::VaultStore& store, std::size_t bytes = 10) {
    auto writer = store.blobs().create();
    REQUIRE(writer.has_value());
    REQUIRE((*writer)->append(domain::Bytes(bytes, std::byte{5})).has_value());
    REQUIRE((*writer)->finish(bytes).has_value());
    return (*writer)->id();
}

inline void checkEntryUpdate(domain::VaultStore& store) {
    using Code = domain::Error::Code;
    const auto folder = storeInsert(store, std::nullopt, true);
    const auto file = storeInsert(store, std::nullopt, false);

    auto uow = store.begin();
    REQUIRE(uow.has_value());
    auto& entries = (*uow)->entries();

    // все колонки по id: перенос в папку и новые зашифрованные поля
    auto record = entries.get(file);
    REQUIRE(record.has_value());
    record->parentId = folder;
    record->encName = domain::toBytes("renamed");
    record->encMeta = domain::toBytes("meta2");
    REQUIRE(entries.update(*record).has_value());
    auto reread = entries.get(file);
    REQUIRE(reread.has_value());
    CHECK(reread->parentId == folder);
    CHECK(reread->encName == domain::toBytes("renamed"));
    CHECK(reread->encMeta == domain::toBytes("meta2"));

    // родитель - любая запись, не только папка: у файла бывают вложения
    domain::EntryRecord attachment;
    attachment.parentId = file;
    auto attachmentId = entries.insert(attachment);
    REQUIRE(attachmentId.has_value());
    CHECK(entries.children(file)->size() == 1);
    auto moved = entries.get(*attachmentId);
    moved->parentId = std::nullopt; // и обратно в корень
    REQUIRE(entries.update(*moved).has_value());
    CHECK(entries.children(file)->empty());

    auto missing = *record;
    missing.id = 9999;
    CHECK(entries.update(missing).error().code == Code::NotFound);
    auto orphan = *record;
    orphan.parentId = 9999;
    CHECK(entries.update(orphan).error().code == Code::NotFound);
    domain::EntryRecord noParent;
    noParent.parentId = 9999;
    CHECK(entries.insert(noParent).error().code == Code::NotFound);

    // без commit - откат
    uow->reset();
    auto check = store.begin();
    CHECK((*check)->entries().get(file)->parentId == std::nullopt);
    CHECK((*check)->entries().get(file)->encName == domain::toBytes("name"));
}

inline void checkBlobRemove(domain::VaultStore& store) {
    using Code = domain::Error::Code;
    const auto first = storeWriteBlob(store);
    const auto entry = storeInsert(store, std::nullopt, false, first);

    {
        auto uow = store.begin();
        // блоб, на который ссылается запись, не удалить (как внешний ключ в sqlite)
        auto busy = (*uow)->blobs().remove(first);
        REQUIRE_FALSE(busy.has_value());
        CHECK(busy.error().code == Code::IntegrityError);
    }
    CHECK(store.blobs().info(first)->status == domain::BlobStatus::Ready);

    // запись переехала на другой блоб - старый удаляется вместе с кусками
    const auto second = storeWriteBlob(store, 20);
    {
        auto uow = store.begin();
        auto record = (*uow)->entries().get(entry);
        record->blobId = second;
        REQUIRE((*uow)->entries().update(*record).has_value());
        REQUIRE((*uow)->blobs().promote(second).has_value());
        REQUIRE((*uow)->blobs().remove(first).has_value());
        REQUIRE((*uow)->commit().has_value());
    }
    CHECK(store.blobs().info(first).error().code == Code::NotFound);
    CHECK(store.blobs().readChunk(first, 0).error().code == Code::NotFound);
    CHECK(store.blobs().info(second)->status == domain::BlobStatus::Ready);

    // pending тоже удаляется; несуществующий блоб - не ошибка
    const auto pending = storeWriteBlob(store);
    {
        auto uow = store.begin();
        REQUIRE((*uow)->blobs().remove(pending).has_value());
        REQUIRE((*uow)->blobs().remove(424242).has_value());
        REQUIRE((*uow)->commit().has_value());
    }
    CHECK(store.blobs().info(pending).error().code == Code::NotFound);
}

inline void checkTagRepository(domain::VaultStore& store) {
    using Code = domain::Error::Code;
    domain::CategoryId cat1 = 0;
    domain::CategoryId cat2 = 0;
    domain::TagId tag1 = 0;
    domain::TagId tag2 = 0;
    domain::TagId tag3 = 0;
    {
        auto uow = store.begin();
        REQUIRE(uow.has_value());
        auto& tags = (*uow)->tags();
        cat1 = tags.insertCategory().value();
        cat2 = tags.insertCategory().value();
        CHECK(cat2 > cat1);
        REQUIRE(tags.updateCategory(cat1, domain::toBytes("cat-one"), {}).has_value());
        REQUIRE(tags.updateCategory(cat2, domain::toBytes("cat-two"), {}).has_value());
        tag1 = tags.insertTag(cat1).value();
        tag2 = tags.insertTag(cat1).value();
        tag3 = tags.insertTag(cat2).value();
        REQUIRE(tags.updateTag(tag1, cat1, domain::toBytes("t1"), {}).has_value());
        REQUIRE(tags.updateTag(tag2, cat1, domain::toBytes("t2"), {}).has_value());
        REQUIRE(tags.updateTag(tag3, cat2, domain::toBytes("t3"), {}).has_value());
        REQUIRE((*uow)->commit().has_value());
    }
    {
        auto uow = store.begin();
        auto& tags = (*uow)->tags();
        auto categories = tags.categories();
        REQUIRE(categories.has_value());
        REQUIRE(categories->size() == 2);
        CHECK((*categories)[0].id == cat1);
        CHECK((*categories)[0].encName == domain::toBytes("cat-one"));
        CHECK((*categories)[1].encName == domain::toBytes("cat-two"));
        auto all = tags.tags();
        REQUIRE(all.has_value());
        REQUIRE(all->size() == 3);
        CHECK((*all)[0].id == tag1);
        CHECK((*all)[0].categoryId == cat1);
        CHECK((*all)[0].encName == domain::toBytes("t1"));
        CHECK((*all)[2].categoryId == cat2);

        // нет такой категории или тега
        CHECK(tags.insertTag(9999).error().code == Code::NotFound);
        CHECK(tags.updateTag(tag1, 9999, domain::toBytes("x"), {}).error().code == Code::NotFound);
        CHECK(tags.updateTag(9999, cat1, domain::toBytes("x"), {}).error().code == Code::NotFound);
        CHECK(tags.updateCategory(9999, domain::toBytes("x"), {}).error().code == Code::NotFound);
        CHECK(tags.removeTag(9999).error().code == Code::NotFound);
        CHECK(tags.removeCategory(9999).error().code == Code::NotFound);

        // перенос тега в другую категорию и удаление - без commit откатываются
        REQUIRE(tags.updateTag(tag2, cat2, domain::toBytes("t2-moved"), {}).has_value());
        REQUIRE(tags.removeTag(tag3).has_value());
        CHECK(tags.tags()->size() == 2);
    }
    {
        auto uow = store.begin();
        auto& tags = (*uow)->tags();
        auto all = tags.tags();
        REQUIRE(all->size() == 3);
        CHECK((*all)[1].categoryId == cat1);
        CHECK((*all)[1].encName == domain::toBytes("t2"));

        // удаление категории уносит ее теги, чужие не трогает
        REQUIRE(tags.updateTag(tag2, cat2, domain::toBytes("t2-moved"), {}).has_value());
        REQUIRE(tags.removeCategory(cat1).has_value());
        REQUIRE((*uow)->commit().has_value());
    }
    {
        auto uow = store.begin();
        auto& tags = (*uow)->tags();
        auto categories = tags.categories();
        REQUIRE(categories->size() == 1);
        CHECK((*categories)[0].id == cat2);
        auto all = tags.tags();
        REQUIRE(all->size() == 2);
        CHECK((*all)[0].id == tag2);
        CHECK((*all)[0].categoryId == cat2);
        CHECK((*all)[0].encName == domain::toBytes("t2-moved"));
        CHECK((*all)[1].id == tag3);

        // id не переиспользуются
        CHECK(tags.insertCategory().value() > cat2);
        CHECK(tags.insertTag(cat2).value() > tag3);
    }
}

} // namespace safebox::test
