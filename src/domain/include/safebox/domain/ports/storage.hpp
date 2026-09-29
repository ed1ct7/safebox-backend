// Порт хранилища сейфа (sqlite в infra, InMemoryVaultStore в тестах).
// UnitOfWork - транзакция, откатывается в деструкторе если не было commit.
// BlobStore пишет каждый кусок отдельной короткой транзакцией, чтобы импорт не держал базу.
// Из потока, который держит UnitOfWork, blobs() не звать - это разные транзакции
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "safebox/domain/io.hpp"
#include "safebox/domain/model/entry.hpp"
#include "safebox/domain/model/error.hpp"
#include "safebox/domain/model/safe_format.hpp"

namespace safebox::domain {

// Строка таблицы meta.
struct SafeMeta {
    std::uint32_t formatVersion = kFormatVersion;
    KdfParams kdf;
    Bytes salt;
    std::uint32_t chunkSize = kDefaultChunkSize;
    Bytes envelope; // мастер-ключ, запечатанный KEK (nonce | ct | tag)
};

enum class BlobStatus : std::uint8_t {
    Pending = 0,
    Ready = 1,
};

struct BlobInfo {
    BlobId id = 0;
    BlobStatus status = BlobStatus::Pending;
    std::uint64_t size = 0; // байт открытого содержимого
    std::uint32_t chunkCount = 0;
};

// Строка таблицы entries: открытые колонки + зашифрованные поля.
struct EntryRecord {
    EntryId id = 0;
    std::optional<EntryId> parentId;
    bool isFolder = false;
    std::optional<BlobId> blobId;
    std::optional<BlobId> thumbBlobId;
    Bytes encName;
    Bytes encMeta;
};

struct RemovedEntries {
    std::vector<EntryId> entries;
    std::vector<BlobId> blobs;
};

class EntryRepository {
public:
    virtual ~EntryRepository() = default;

    // record.id игнорируется; id выдается хранилищем и не переиспользуется.
    // enc_name/enc_meta можно вставить пустыми и запечатать по полученному id.
    [[nodiscard]] virtual Result<EntryId> insert(const EntryRecord& record) = 0;
    [[nodiscard]] virtual Status updateSealed(EntryId id, std::span<const std::byte> encName,
                                              std::span<const std::byte> encMeta) = 0;
    // Все колонки записи по record.id (перенос, смена блобов); NotFound - нет записи или
    // нового родителя.
    [[nodiscard]] virtual Status update(const EntryRecord& record) = 0;
    [[nodiscard]] virtual Result<EntryRecord> get(EntryId id) = 0; // NotFound
    [[nodiscard]] virtual Result<std::vector<EntryRecord>>
    children(std::optional<EntryId> parent) = 0;
    [[nodiscard]] virtual Result<std::vector<EntryRecord>> all() = 0;
    [[nodiscard]] virtual Result<std::uint64_t> count() = 0;
    // Запись, ее потомки и все их блобы (с кусками) - в текущей транзакции.
    [[nodiscard]] virtual Result<RemovedEntries> removeSubtree(EntryId id) = 0;
};

class BlobRepository {
public:
    virtual ~BlobRepository() = default;

    // pending -> ready; блоб должен существовать и быть pending.
    [[nodiscard]] virtual Status promote(BlobId id) = 0;
    // Блоб и его куски (ready или pending); нет такого - не ошибка. На блоб не должна
    // ссылаться запись.
    [[nodiscard]] virtual Status remove(BlobId id) = 0;
};

// Зашифрованные строки таблиц tag_categories и tags. category_id открыт и входит в AAD имени тега.
struct TagCategoryRecord {
    CategoryId id = 0;
    Bytes encName;
};

struct TagRecord {
    TagId id = 0;
    CategoryId categoryId = 0;
    Bytes encName;
};

class TagRepository {
public:
    virtual ~TagRepository() = default;

    // Имя вставляется пустым и запечатывается по полученному id (updateCategory/updateTag).
    [[nodiscard]] virtual Result<CategoryId> insertCategory() = 0;
    [[nodiscard]] virtual Status updateCategory(CategoryId id,
                                                std::span<const std::byte> encName) = 0;
    // Категория и каскадом ее теги; NotFound - нет такой.
    [[nodiscard]] virtual Status removeCategory(CategoryId id) = 0;
    [[nodiscard]] virtual Result<std::vector<TagCategoryRecord>> categories() = 0;
    // NotFound - нет категории.
    [[nodiscard]] virtual Result<TagId> insertTag(CategoryId category) = 0;
    // Перенос тега в другую категорию = смена category_id вместе с перезапечатанным именем.
    [[nodiscard]] virtual Status updateTag(TagId id, CategoryId category,
                                           std::span<const std::byte> encName) = 0;
    [[nodiscard]] virtual Status removeTag(TagId id) = 0; // NotFound - нет такого
    [[nodiscard]] virtual Result<std::vector<TagRecord>> tags() = 0;
};

class UnitOfWork {
public:
    virtual ~UnitOfWork() = default; // без commit - откат

    [[nodiscard]] virtual EntryRepository& entries() = 0;
    [[nodiscard]] virtual BlobRepository& blobs() = 0;
    [[nodiscard]] virtual TagRepository& tags() = 0;
    [[nodiscard]] virtual Status saveMeta(const SafeMeta& meta) = 0;
    [[nodiscard]] virtual Status commit() = 0;
};

// Запись одного блоба кусками; каждый append - отдельная короткая транзакция.
class BlobWriter {
public:
    virtual ~BlobWriter() = default;

    [[nodiscard]] virtual BlobId id() const = 0;
    // Кусок с индексом "число предыдущих append" (уже запечатанный).
    [[nodiscard]] virtual Status append(std::span<const std::byte> sealedChunk) = 0;
    // Фиксирует размер и число кусков; блоб остается pending до promote.
    [[nodiscard]] virtual Status finish(std::uint64_t plainSize) = 0;
};

class BlobStore {
public:
    virtual ~BlobStore() = default;

    [[nodiscard]] virtual Result<std::unique_ptr<BlobWriter>> create() = 0;
    [[nodiscard]] virtual Result<BlobInfo> info(BlobId id) = 0;
    [[nodiscard]] virtual Result<Bytes> readChunk(BlobId id, std::uint32_t index) = 0;
    // Удалить свой pending-блоб (ошибка импорта одного файла).
    [[nodiscard]] virtual Status discard(BlobId id) = 0;
    // Удалить все pending-блобы (lock, открытие после сбоя). -> сколько удалено.
    [[nodiscard]] virtual Result<std::size_t> gcPending() = 0;
};

class VaultStore {
public:
    virtual ~VaultStore() = default;

    [[nodiscard]] virtual Status create(const std::filesystem::path& path,
                                        const SafeMeta& meta) = 0;
    [[nodiscard]] virtual Status open(const std::filesystem::path& path) = 0;
    // Только чтение: NotFound / NotASafe / IoError или meta файла. Открытый
    // сейф (если есть) не закрывается.
    [[nodiscard]] virtual Result<SafeMeta> inspect(const std::filesystem::path& path) = 0;
    virtual void close() noexcept = 0;
    [[nodiscard]] virtual bool isOpen() const = 0;

    [[nodiscard]] virtual Result<SafeMeta> meta() = 0;
    [[nodiscard]] virtual Result<std::unique_ptr<UnitOfWork>> begin() = 0;
    [[nodiscard]] virtual BlobStore& blobs() = 0;
    [[nodiscard]] virtual Status compact() = 0;
};

} // namespace safebox::domain
