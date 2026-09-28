// Интерфейсы сервисов, которые дергает http-слой. Запросы/ответы как в docs/api.md
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "safebox/app/session.hpp"
#include "safebox/domain/io.hpp"
#include "safebox/domain/model/entry.hpp"
#include "safebox/domain/model/error.hpp"

namespace safebox::app {

using domain::Result;
using domain::Status;

// SafeService

struct CreateSafeCmd {
    std::string path; // полный или относительно defaultDirectory; .safebox добавится
    std::string password;
    std::string confirm;
};

struct UnlockCmd {
    std::string path;
    std::string password;
};

struct ChangePasswordCmd {
    std::string oldPassword;
    std::string newPassword;
    std::string confirm;
};

struct SafeInfo {
    std::string path;                  // полный путь открытого сейфа (UTF-8)
    std::uint64_t entryCount = 0;      // объектов всего: файлы, фото, видео, ссылки, папки
    std::int64_t idleRemainingSec = 0; // до автоблокировки
};

struct PublicStatus {
    bool unlocked = false;
    std::optional<std::string> lastPath; // последний открытый/созданный сейф
    std::string defaultDirectory;        // база относительных путей
};

struct UnlockResult {
    SessionTokens tokens;
    SafeInfo safe;
};

struct HeartbeatResult {
    std::int64_t idleRemainingSec = 0;
};

class SafeService {
public:
    virtual ~SafeService() = default;

    [[nodiscard]] virtual Result<UnlockResult> create(const CreateSafeCmd& cmd) = 0;
    [[nodiscard]] virtual Result<UnlockResult> unlock(const UnlockCmd& cmd) = 0;
    virtual void lock() noexcept = 0;
    [[nodiscard]] virtual PublicStatus publicStatus() = 0;
    [[nodiscard]] virtual Result<SafeInfo> info(const Lease& lease) = 0;
    [[nodiscard]] virtual Status changePassword(const Lease& lease,
                                                const ChangePasswordCmd& cmd) = 0;
    [[nodiscard]] virtual Result<Lease> authorize(std::string_view token, TokenScope scope) = 0;
    [[nodiscard]] virtual Result<HeartbeatResult> heartbeat(const Lease& lease, bool active) = 0;
    virtual void tick() noexcept = 0;
};

// EntriesService

struct FolderListing {
    std::optional<domain::Entry> folder; // nullopt - корень "Все объекты"
    std::vector<domain::PathItem> path;  // крошки от корня до папки включительно
    std::vector<domain::Entry> entries;  // папки первыми, затем по имени
};

struct FolderNode {
    domain::EntryId id = 0;
    std::optional<domain::EntryId> parentId;
    std::string name;
};

class EntriesService {
public:
    virtual ~EntriesService() = default;

    [[nodiscard]] virtual Result<FolderListing> list(const Lease& lease,
                                                     std::optional<domain::EntryId> folder) = 0;
    [[nodiscard]] virtual Result<domain::Entry> get(const Lease& lease, domain::EntryId id) = 0;
    // Все папки сейфа: родители раньше детей, соседи по имени (дерево слева).
    [[nodiscard]] virtual Result<std::vector<FolderNode>> folders(const Lease& lease) = 0;
    [[nodiscard]] virtual Result<domain::Entry> rename(const Lease& lease, domain::EntryId id,
                                                       std::string_view name) = 0;
    // Записи вместе с поддеревьями одной транзакцией; -> сколько удалено всего.
    [[nodiscard]] virtual Result<std::size_t> remove(const Lease& lease,
                                                     std::span<const domain::EntryId> ids) = 0;
};

// ImportExportService

// Потоковый импорт одного запроса: http проталкивает части multipart по
// мере чтения сокета. Ошибка отдельного файла не прерывает импорт - она
// попадает в итог; наружу (Status) выходит только фатальное: отмена lock'ом.
class ImportSession {
public:
    virtual ~ImportSession() = default; // незавершенный файл -> его pending удаляется

    // relativePath - "Папка/Подпапка/файл.jpg" (структура папок сохраняется)
    [[nodiscard]] virtual Status beginFile(std::string_view relativePath) = 0;
    [[nodiscard]] virtual Status write(std::span<const std::byte> data) = 0;
    [[nodiscard]] virtual Status endFile() = 0;
    [[nodiscard]] virtual domain::ImportResult finish() = 0;
};

// Расшифрованное содержимое записи с произвольным доступом (Range).
class ContentStream {
public:
    virtual ~ContentStream() = default;

    [[nodiscard]] virtual std::uint64_t size() const noexcept = 0;
    // Пишет ровно length байт с offset; расшифровывает только нужные куски,
    // последний расшифрованный кусок кэшируется на время жизни потока.
    [[nodiscard]] virtual Status read(std::uint64_t offset, std::uint64_t length,
                                      domain::ByteSink& out) = 0;
};

enum class ContentVariant {
    Original,
    Thumbnail,
};

struct OpenedContent {
    domain::Entry entry;
    std::string mime;
    std::unique_ptr<ContentStream> stream; // держит аренду, пока жив
};

class ImportExportService {
public:
    virtual ~ImportExportService() = default;

    [[nodiscard]] virtual Result<std::unique_ptr<ImportSession>>
    beginImport(Lease lease, std::optional<domain::EntryId> parent) = 0;
    [[nodiscard]] virtual Result<OpenedContent> openContent(Lease lease, domain::EntryId id,
                                                            ContentVariant variant) = 0;
    // папка -> потоковый zip (STORE) в out; синхронно, под арендой вызывающего.
    [[nodiscard]] virtual Status exportZip(const Lease& lease, domain::EntryId folder,
                                           domain::ByteSink& out) = 0;
};

// SearchService

inline constexpr std::size_t kDefaultSearchLimit = 200;

class SearchService {
public:
    virtual ~SearchService() = default;

    // Без учёта регистра (кириллица включительно, "ё" = "е"); пустой запрос -> [].
    [[nodiscard]] virtual Result<std::vector<domain::SearchHit>>
    search(const Lease& lease, std::string_view query, std::size_t limit) = 0;
};

// Набор для транспорта

struct Services {
    std::shared_ptr<SafeService> safe;
    std::shared_ptr<EntriesService> entries;
    std::shared_ptr<ImportExportService> importExport;
    std::shared_ptr<SearchService> search;
};

} // namespace safebox::app
