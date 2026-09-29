// Модель записи (уже расшифрованная). Зашифрованная строка таблицы - EntryRecord в ports/storage.hpp
#pragma once

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
};

struct TagCategory {
    CategoryId id = 0;
    std::string name;
};

struct Tag {
    TagId id = 0;
    CategoryId categoryId = 0;
    std::string name;
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

struct ImportResult {
    std::size_t imported = 0;
    std::size_t replaced = 0; // файлы, записанные поверх существующей записи с тем же именем
    std::size_t skipped = 0;  // имя было занято и выбрано "пропустить": в сейф не записаны
    std::size_t failed = 0;
    std::vector<ImportFailure> failures;
};

} // namespace safebox::domain
