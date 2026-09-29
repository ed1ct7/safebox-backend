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

// Значения - часть формата файла (лежат в enc_meta), менять нельзя.
enum class Kind : std::uint8_t {
    Folder = 0,
    File = 1,
    Photo = 2,
    Video = 3,
    Link = 4,
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
};

struct Entry {
    EntryId id = 0;
    std::optional<EntryId> parentId; // nullopt - корень ("Все объекты")
    std::string name;
    EntryMeta meta;

    [[nodiscard]] bool isFolder() const noexcept { return meta.kind == Kind::Folder; }
    [[nodiscard]] bool hasThumbnail() const noexcept { return meta.thumbBlobId.has_value(); }
};

struct PathItem {
    EntryId id = 0;
    std::string name;
};

struct SearchHit {
    Entry entry;
    std::vector<PathItem> path; // папки от корня до родителя записи
};

struct ImportFailure {
    std::string path;    // относительный путь файла, как прислал клиент
    std::string message; // причина для пользователя
};

struct ImportResult {
    std::size_t imported = 0;
    std::size_t failed = 0;
    std::size_t skipped = 0; // уже были в папке: то же имя без учета регистра и те же байты
    std::vector<ImportFailure> failures;
};

} // namespace safebox::domain
