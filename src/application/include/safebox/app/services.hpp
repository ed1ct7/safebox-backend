// Интерфейсы сервисов, которые дергает http-слой. Запросы/ответы как в контракте REST API (Linqtab)
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "safebox/app/session.hpp"
#include "safebox/domain/io.hpp"
#include "safebox/domain/model/entry.hpp"
#include "safebox/domain/model/error.hpp"
#include "safebox/domain/model/settings.hpp"

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
    std::optional<domain::Entry> parent; // nullopt - корень "Все объекты"; иначе любая запись
    std::vector<domain::PathItem> path;  // крошки от корня до parent включительно
    std::vector<domain::Entry> entries;  // папки первыми, затем по имени
};

struct FolderNode {
    domain::EntryId id = 0;
    std::optional<domain::EntryId> parentId;
    std::string name;
};

// Что изменить у записи; заданное поле подменяется целиком.
struct UpdateEntryCmd {
    std::optional<std::string> name;        // ставит nameByUser
    std::optional<std::string> description; // <= 64 КиБ, ставит descriptionByUser
    std::optional<std::string> url;         // только у ссылки, http/https
};

enum class ConflictPolicy {
    KeepBoth,
    Replace,
    Skip,
};

// У нового родителя уже есть запись с тем же именем (без учета регистра).
struct MoveConflict {
    domain::EntryId id = 0; // перемещаемая запись
    domain::Entry existing; // запись, занявшая имя
};

struct MoveCmd {
    std::vector<domain::EntryId> ids;
    std::optional<domain::EntryId> parent;                           // nullopt - корень
    std::unordered_map<domain::EntryId, ConflictPolicy> resolutions; // по умолчанию KeepBoth
};

// moved - записи, сменившие родителя (вместе с заменившими и переименованными), replaced -
// удаленные ими записи, skipped - оставленные на месте по Skip. Запись, уже лежащая у
// нужного родителя, нигде не учитывается.
struct MoveResult {
    std::size_t moved = 0;
    std::size_t replaced = 0;
    std::size_t skipped = 0;
};

class EntriesService {
public:
    virtual ~EntriesService() = default;

    // parent - любая запись (у файла бывают вложения); нет такой -> NotFound.
    [[nodiscard]] virtual Result<FolderListing> list(const Lease& lease,
                                                     std::optional<domain::EntryId> parent) = 0;
    [[nodiscard]] virtual Result<domain::Entry> get(const Lease& lease, domain::EntryId id) = 0;
    // Папки, у которых все предки тоже папки: родители раньше детей, соседи по имени (дерево
    // слева).
    [[nodiscard]] virtual Result<std::vector<FolderNode>> folders(const Lease& lease) = 0;
    [[nodiscard]] virtual Result<domain::Entry> update(const Lease& lease, domain::EntryId id,
                                                       const UpdateEntryCmd& cmd) = 0;
    // Конфликты имен, которые возникнут при переносе ids к parent; ничего не меняет.
    [[nodiscard]] virtual Result<std::vector<MoveConflict>>
    planMove(const Lease& lease, std::span<const domain::EntryId> ids,
             std::optional<domain::EntryId> parent) = 0;
    // Перенос одной транзакцией; в себя или своего потомка нельзя (ничего не меняется).
    [[nodiscard]] virtual Result<MoveResult> move(const Lease& lease, const MoveCmd& cmd) = 0;
    // Записи вместе с поддеревьями одной транзакцией; -> сколько удалено всего.
    [[nodiscard]] virtual Result<std::size_t> remove(const Lease& lease,
                                                     std::span<const domain::EntryId> ids) = 0;
};

// ImportExportService

// Что известно о файле кроме пути: дата исходного файла и как поступить, если имя занято.
struct ImportFileOptions {
    std::optional<std::int64_t> sourceModifiedAt; // unix-мс изменения файла на диске
    ConflictPolicy onConflict = ConflictPolicy::KeepBoth;
};

// Потоковый импорт одного запроса: http проталкивает части multipart по
// мере чтения сокета. Ошибка отдельного файла не прерывает импорт - она
// попадает в итог; наружу (Status) выходит только фатальное: отмена lock'ом.
class ImportSession {
public:
    virtual ~ImportSession() = default; // незавершенный файл -> его pending удаляется

    // relativePath - "Папка/Подпапка/файл.jpg" (структура папок сохраняется). Skip - байты
    // следующих write игнорируются, в хранилище не пишется ничего.
    [[nodiscard]] virtual Status beginFile(std::string_view relativePath,
                                           const ImportFileOptions& options) = 0;
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

struct ImportPlanFile {
    std::string path; // как в beginFile
    std::uint64_t size = 0;
};

// Файл плана, чье имя уже занято у целевого родителя.
struct ImportConflict {
    std::string path;       // как в ImportPlanFile
    domain::Entry existing; // запись, занявшая имя
};

struct ImportPlan {
    std::vector<ImportConflict> conflicts;
    // Файлы без конфликта; недопустимый путь тоже здесь: он упадет уже при импорте.
    std::size_t newFiles = 0;
};

class ImportExportService {
public:
    virtual ~ImportExportService() = default;

    // parent - любая запись (импорт во вложения); нет такой -> NotFound.
    [[nodiscard]] virtual Result<std::unique_ptr<ImportSession>>
    beginImport(Lease lease, std::optional<domain::EntryId> parent) = 0;
    // Какие файлы столкнутся по имени с записями сейфа, если импортировать их в parent; ничего
    // не создает (несуществующие папки пути - значит конфликтов в них нет).
    [[nodiscard]] virtual Result<ImportPlan> planImport(const Lease& lease,
                                                        std::optional<domain::EntryId> parent,
                                                        std::span<const ImportPlanFile> files) = 0;
    [[nodiscard]] virtual Result<OpenedContent> openContent(Lease lease, domain::EntryId id,
                                                            ContentVariant variant) = 0;
    // Папка или запись с вложениями -> потоковый zip (STORE) в out; синхронно, под арендой
    // вызывающего. Запись без вложений и не папка -> InvalidArgument.
    [[nodiscard]] virtual Status exportZip(const Lease& lease, domain::EntryId id,
                                           domain::ByteSink& out) = 0;
};

// TagsService

struct TagWithCount {
    domain::Tag tag;
    std::size_t count = 0; // записей, на которых тег стоит напрямую
};

struct CategoryWithTags {
    domain::TagCategory category;
    std::vector<TagWithCount> tags; // по имени
};

struct CreateTagCmd {
    std::string category; // имя категории, не id
    std::string name;
    std::string nameEn;          // вторая локализация; пусто - не задана
    bool createCategory = false; // нет такой категории: создать вместо NotFound
};

struct CreatedTag {
    domain::Tag tag;
    bool created = false; // false - такой тег уже был, вернулся он
};

struct UpdateTagCmd {
    std::optional<std::string> name;
    std::optional<std::string> nameEn; // вторая локализация; "" - очистить, nullopt - не менять
    std::optional<domain::CategoryId> categoryId; // перенос в другую категорию
};

// Переименование категории: оба поля опциональны; nameEn "" - очистить, nullopt - не менять.
struct RenameCategoryCmd {
    std::optional<std::string> name;
    std::optional<std::string> nameEn;
};

struct RemovedTags {
    std::size_t removedTags = 0;
    std::size_t affectedEntries = 0; // записи, с которых теги сняты
};

struct AssignTagsCmd {
    std::vector<domain::EntryId> ids;
    std::vector<domain::TagAssignment> add; // тег уже стоит - обновляется inherit
    std::vector<domain::TagId> remove;      // снимается раньше, чем ставится add
};

// Имена сравниваются без учета регистра ("ё" = "е"): "Eris" и "eris" в одной категории - один тег.
// Любая правка тегов, которая касается записей, переписывает их одной транзакцией с самим тегом.
class TagsService {
public:
    virtual ~TagsService() = default;

    // Категории и теги в них по имени.
    [[nodiscard]] virtual Result<std::vector<CategoryWithTags>> list(const Lease& lease) = 0;
    // Дубль имени (по любой локализации) -> AlreadyExists, плохое имя -> InvalidArgument.
    [[nodiscard]] virtual Result<domain::TagCategory>
    createCategory(const Lease& lease, std::string_view name, std::string_view nameEn = {}) = 0;
    [[nodiscard]] virtual Result<CategoryWithTags>
    renameCategory(const Lease& lease, domain::CategoryId id, const RenameCategoryCmd& cmd) = 0;
    // Категория и ее теги; теги снимаются со всех записей.
    [[nodiscard]] virtual Result<RemovedTags> removeCategory(const Lease& lease,
                                                             domain::CategoryId id) = 0;
    // Нет категории и не createCategory -> NotFound; тег уже есть -> он же и created = false.
    [[nodiscard]] virtual Result<CreatedTag> createTag(const Lease& lease,
                                                       const CreateTagCmd& cmd) = 0;
    // Переименование и/или перенос в другую категорию; такое имя уже есть там -> AlreadyExists.
    [[nodiscard]] virtual Result<domain::Tag> updateTag(const Lease& lease, domain::TagId id,
                                                        const UpdateTagCmd& cmd) = 0;
    // Все присвоения from переходят на into (inherit = a || b), from удаляется -> сколько
    // записей затронуто.
    [[nodiscard]] virtual Result<std::size_t> mergeTag(const Lease& lease, domain::TagId from,
                                                       domain::TagId into) = 0;
    // -> сколько записей потеряло тег.
    [[nodiscard]] virtual Result<std::size_t> removeTag(const Lease& lease, domain::TagId id) = 0;
    // Одной транзакцией; неизвестный тег -> InvalidArgument, неизвестная запись -> NotFound,
    // больше 1000 тегов на записи -> InvalidArgument, и ничего не меняется. -> записей изменено.
    [[nodiscard]] virtual Result<std::size_t> assign(const Lease& lease,
                                                     const AssignTagsCmd& cmd) = 0;
};

// SearchService

// Как сочетаются выбранные теги.
enum class TagMatch {
    Categories, // И между категориями, ИЛИ внутри категории
    All,        // все выбранные
    Any,        // хотя бы один
};

struct SearchQuery {
    std::string text; // может быть пустым, если заданы теги
    std::vector<domain::TagId> tags;
    TagMatch match = TagMatch::Categories;
    std::optional<domain::EntryId> within; // только потомки этой записи, ее самой в выдаче нет
    std::size_t limit = 0;                 // 0 - без ограничения
};

class SearchService {
public:
    virtual ~SearchService() = default;

    // Текст - без учета регистра (кириллица включительно, "ё" = "е") в имени, иначе в описании;
    // теги - по прямым и унаследованным. Пусто и то и другое -> []. limit = 0 - выдать все
    // совпадения. Неизвестный тег -> InvalidArgument, неизвестный within -> NotFound.
    [[nodiscard]] virtual Result<std::vector<domain::SearchHit>>
    search(const Lease& lease, const SearchQuery& query) = 0;
};

// LinksService

inline constexpr std::size_t kMaxLinksPerRequest = 10'000;

struct NewLink {
    std::string url;                 // как ввел пользователь; пробелы по краям срезаются
    std::optional<std::string> name; // не задано - хост адреса
    std::string path;                // "Закладки/Работа": папки внутри parent, пусто - сам parent
};

struct CreateLinksCmd {
    std::optional<domain::EntryId> parent; // nullopt - корень; иначе любая запись
    std::vector<NewLink> links;
};

// Ссылка с таким адресом (без учета схемы/хоста в регистре, порта по умолчанию, "/" в конце и
// utm_-параметров) уже лежит у того же родителя.
struct ExistingLink {
    std::string url;
    domain::EntryId entryId = 0;
};

struct CreateLinksResult {
    std::vector<domain::Entry> created;
    std::vector<ExistingLink> existing;
    std::vector<std::string> invalid; // не http(s)-адрес или недопустимый путь папок
};

class LinksService {
public:
    virtual ~LinksService() = default;

    // Ссылки без содержимого одной транзакцией: всё или ничего. Больше kMaxLinksPerRequest ->
    // InvalidArgument, нет parent -> NotFound. Если в настройках включен предпросмотр, каждая
    // созданная ссылка становится в очередь фонового воркера (previewPending).
    [[nodiscard]] virtual Result<CreateLinksResult> create(const Lease& lease,
                                                           const CreateLinksCmd& cmd) = 0;
    // Синхронно (до 15 с), независимо от настройки: нет записи -> NotFound, не ссылка ->
    // InvalidArgument, страница не загрузилась -> PreviewFailed.
    [[nodiscard]] virtual Result<domain::Entry> refreshPreview(const Lease& lease,
                                                               domain::EntryId id) = 0;
    // Запись стоит в очереди на предпросмотр (или обрабатывается сейчас).
    [[nodiscard]] virtual bool previewPending(const Lease& lease, domain::EntryId id) const = 0;
    // Ждет, пока очередь опустеет и текущее задание закончится. Для тестов: в работе никто не
    // ждет предпросмотры.
    virtual void drain() = 0;
};

// SettingsService

// Настройки не зависят от сейфа: доступны и без аренды, а транспорт отдает их только
// авторизованному.
class SettingsService {
public:
    virtual ~SettingsService() = default;

    [[nodiscard]] virtual domain::AppSettings get() = 0;
    // Сохраняет и применяет сразу; не сохранилось -> ошибка, прежние настройки остаются.
    [[nodiscard]] virtual Result<domain::AppSettings>
    update(const domain::AppSettings& settings) = 0;
};

// Набор для транспорта

struct Services {
    std::shared_ptr<SafeService> safe;
    std::shared_ptr<EntriesService> entries;
    std::shared_ptr<ImportExportService> importExport;
    std::shared_ptr<SearchService> search;
    std::shared_ptr<TagsService> tags;
    std::shared_ptr<LinksService> links;
    std::shared_ptr<SettingsService> settings;
};

} // namespace safebox::app
