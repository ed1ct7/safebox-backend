// Модель записи (уже расшифрованная). Зашифрованная строка таблицы - EntryRecord в ports/storage.hpp
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace safebox::domain {

using EntryId = std::int64_t;
using BlobId = std::int64_t;
using TagId = std::int64_t;
using CategoryId = std::int64_t;

// Значения - часть формата файла (лежат в enc_meta), менять нельзя.
enum class Kind : std::uint8_t {
    Folder = 0,
    File = 1,
    Photo = 2,
    Video = 3,
    Link = 4,
};

struct TagAssignment {
    TagId tagId = 0;
    bool inherit = false; // действует на всё поддерево записи

    friend bool operator==(const TagAssignment&, const TagAssignment&) = default;
};

using Sha256 = std::array<std::byte, 32>;

struct Dimensions {
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    friend bool operator==(const Dimensions&, const Dimensions&) = default;
};

struct EntryMeta {
    Kind kind = Kind::File;
    std::string mime;             // тип содержимого (для photo/video - то, что отдается inline)
    std::uint64_t size = 0;       // байт исходного файла; у папки 0
    std::string url;              // только у ссылок (http/https)
    std::int64_t createdAt = 0;   // unix-время, мс (импорт)
    std::int64_t modifiedAt = 0;  // unix-время, мс (импорт/переименование)
    std::optional<BlobId> blobId; // копия открытой колонки entries.blob_id
    std::optional<BlobId> thumbBlobId; // копия открытой колонки entries.thumb_blob_id
    std::optional<std::int64_t> sourceModifiedAt; // unix-мс изменения исходного файла на диске
    std::string description;
    bool nameByUser = false; // имя задано пользователем - предпросмотр его не трогает
    bool descriptionByUser = false;
    std::vector<TagAssignment> tags; // прямые присвоения, по возрастанию tagId, без дублей

    // Дальше - поля enc_meta layout 3 (схема 4). У записей layout 2 все пустые.
    std::string source;                      // откуда запись ("telegram", "youtube"); пусто - нет
    std::string sourceUrl;                   // адрес оригинала (пост, сообщение); пусто - нет
    std::optional<std::int64_t> sourceDate;  // unix-мс публикации/сохранения в источнике
    std::optional<Sha256> sha256;            // хеш исходного содержимого (открытого текста)
    std::optional<Dimensions> dimensions;    // фото и видео: размер кадра в пикселях
    std::optional<std::uint64_t> durationMs; // видео и аудио
    // Запись лежит в корзине с этого момента (unix-мс). Ставится только корню удаленного
    // поддерева: его потомки в корзине вместе с ним, parent_id не меняется.
    std::optional<std::int64_t> trashedAt;
};

// Второе имя (nameEn) - английская локализация; пустая строка - не задано, показывается name.
struct TagCategory {
    CategoryId id = 0;
    std::string name;
    std::string nameEn;

    friend bool operator==(const TagCategory&, const TagCategory&) = default;
};

struct Tag {
    TagId id = 0;
    CategoryId categoryId = 0;
    std::string name;
    std::string nameEn;

    friend bool operator==(const Tag&, const Tag&) = default;
};

// Тег, действующий на запись через предка с inherit.
struct InheritedTag {
    TagId tagId = 0;
    EntryId fromId = 0; // предок, у которого он задан

    friend bool operator==(const InheritedTag&, const InheritedTag&) = default;
};

struct Entry {
    EntryId id = 0;
    std::optional<EntryId> parentId; // nullopt - корень ("Все объекты")
    std::string name;
    EntryMeta meta;
    // Считает каталог, в файле не лежит.
    std::size_t childCount = 0;              // прямых детей
    std::vector<InheritedTag> inheritedTags; // от предков с inherit, по возрастанию tagId
    bool previewPending = false;             // ссылка ждет загрузки предпросмотра (у остальных нет)

    [[nodiscard]] bool isFolder() const noexcept { return meta.kind == Kind::Folder; }
    [[nodiscard]] bool hasThumbnail() const noexcept { return meta.thumbBlobId.has_value(); }
};

struct PathItem {
    EntryId id = 0;
    std::string name;
};

enum class MatchedIn : std::uint8_t {
    None, // поиска по тексту не было (только теги)
    Name,
    Description,
};

struct SearchHit {
    Entry entry;
    std::vector<PathItem> path; // папки от корня до родителя записи
    MatchedIn matchedIn = MatchedIn::None;
};

struct ImportFailure {
    std::string path;    // относительный путь файла, как прислал клиент
    std::string message; // причина для пользователя
};

// Файл с тем же содержимым (sha256) уже есть: в сейф не записан.
struct ImportDuplicate {
    std::string path;     // относительный путь файла, как прислал клиент
    EntryId existing = 0; // запись с тем же содержимым
};

struct ImportResult {
    std::size_t imported = 0;
    std::size_t replaced = 0; // файлы, записанные поверх существующей записи с тем же именем
    std::size_t skipped = 0;  // имя было занято и выбрано "пропустить": в сейф не записаны
    std::size_t failed = 0;
    std::vector<ImportFailure> failures;
    std::vector<ImportDuplicate> duplicates; // повторы содержимого: в сейф не записаны
};

} // namespace safebox::domain
