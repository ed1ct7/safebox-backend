// Интерфейсы сервисов, которые дергает http-слой. Запросы/ответы как в контракте REST API (Linqtab)
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
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

// Порядок и окно выдачи (список записи, поиск)

// Name - имя без учета регистра, числа в имени - по значению ("2" раньше "10");
// Added - createdAt; Modified - modifiedAt; Date - sourceDate, иначе sourceModifiedAt, иначе
// createdAt; Size - size; Trashed - trashedAt (вне корзины - 0).
enum class SortKey {
    Name,
    Added,
    Modified,
    Date,
    Size,
    Trashed,
};

// Папки всегда первыми; descending переворачивает только ключ, равные по ключу - по имени
// по возрастанию, затем по id. Окно [offset, offset + limit); limit = 0 - до конца.
struct ListOptions {
    SortKey sort = SortKey::Name;
    bool descending = false;
    std::size_t offset = 0;
    std::size_t limit = 0;
};

// EntriesService

struct FolderListing {
    std::optional<domain::Entry> parent; // nullopt - корень "Все объекты"; иначе любая запись
    std::vector<domain::PathItem> path;  // крошки от корня до parent включительно
    std::vector<domain::Entry> entries;  // окно выдачи в порядке ListOptions
    std::size_t total = 0;               // детей всего, без учета окна
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
    // Происхождение: пустая строка / nullopt внутри - снять.
    std::optional<std::string> source;                     // validateSource
    std::optional<std::string> sourceUrl;                  // http/https
    std::optional<std::optional<std::int64_t>> sourceDate; // unix-мс
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
// записи, ушедшие от них в корзину, skipped - оставленные на месте по Skip. Запись, уже
// лежащая у нужного родителя, нигде не учитывается.
struct MoveResult {
    std::size_t moved = 0;
    std::size_t replaced = 0;
    std::size_t skipped = 0;
};

// Содержимое с этим хешем уже есть в сейфе (вне корзины): запись с наименьшим id.
struct HashMatch {
    domain::Sha256 sha256{};
    domain::EntryId id = 0;
};

inline constexpr std::size_t kMaxLookupHashes = 10'000;
inline constexpr std::size_t kMaxFolderPaths = 10'000;

class EntriesService {
public:
    virtual ~EntriesService() = default;

    // parent - любая запись (у файла бывают вложения, у записи в корзине - ее дети в корзине);
    // нет такой -> NotFound. Корни корзины в списках не показываются (их выдает TrashService).
    [[nodiscard]] virtual Result<FolderListing>
    list(const Lease& lease, std::optional<domain::EntryId> parent, const ListOptions& options) = 0;
    // Все дети, папки первыми, затем по имени.
    [[nodiscard]] Result<FolderListing> list(const Lease& lease,
                                             std::optional<domain::EntryId> parent) {
        return list(lease, parent, ListOptions{});
    }
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
    // Перенос одной транзакцией; в себя или своего потомка нельзя (ничего не меняется). Ни
    // переносимые, ни родитель не могут быть в корзине (InvalidArgument).
    [[nodiscard]] virtual Result<MoveResult> move(const Lease& lease, const MoveCmd& cmd) = 0;
    // Папки по путям внутри parent (nullopt - корень), недостающие создаются; одна транзакция,
    // ошибка -> ничего не создано. Путь чистится и сливается с папками как у импорта и ссылок;
    // пустой путь - сам parent, ".." -> InvalidArgument. Не больше kMaxFolderPaths путей.
    // -> id папки на каждый путь, по порядку (nullopt - корень).
    [[nodiscard]] virtual Result<std::vector<std::optional<domain::EntryId>>>
    makeFolders(const Lease& lease, std::optional<domain::EntryId> parent,
                std::span<const std::string> paths) = 0;
    // В корзину одной транзакцией: trashedAt ставится только корням выделения, потомки уходят с
    // ними. Уже лежащие в корзине пропускаются. -> сколько записей ушло всего (с потомками).
    [[nodiscard]] virtual Result<std::size_t> remove(const Lease& lease,
                                                     std::span<const domain::EntryId> ids) = 0;
    // Какие хеши содержимого уже есть: загрузчик архива не передает их. Не больше
    // kMaxLookupHashes; у записей, чей хеш еще досчитывается (HashProgress), совпадения нет.
    // Пустое содержимое не ищется: импорт пустые файлы повтором не считает.
    [[nodiscard]] virtual Result<std::vector<HashMatch>>
    lookup(const Lease& lease, std::span<const domain::Sha256> hashes) = 0;
};

// TrashService - корзина: корни удаленных поддеревьев (meta.trashedAt), parent_id у них прежний.

struct TrashedEntry {
    domain::Entry entry;                    // meta.trashedAt задан
    std::vector<domain::PathItem> location; // откуда удалена: крошки до родителя; пусто - корень
};

struct TrashListing {
    std::vector<TrashedEntry> entries; // окно в порядке ListOptions
    std::size_t total = 0;             // корней в корзине
};

struct RenamedEntry {
    domain::EntryId id = 0;
    std::string name; // новое имя
};

struct RestoreResult {
    std::size_t restored = 0;
    std::vector<RenamedEntry> renamed; // имя на месте уже было занято -> "имя (2)"
};

// Что стереть навсегда - одно из трех.
struct PurgeCmd {
    std::vector<domain::EntryId> ids;   // записи в корзине (корни или внутри них)
    std::optional<std::int64_t> before; // корни, удаленные раньше этого момента (unix-мс)
    bool all = false;                   // вся корзина
};

class TrashService {
public:
    virtual ~TrashService() = default;

    [[nodiscard]] virtual Result<TrashListing> list(const Lease& lease,
                                                    const ListOptions& options) = 0;
    // Корни корзины - обратно к родителю одной транзакцией. Не корень корзины -> NotFound,
    // родитель сам в корзине -> InvalidArgument (сначала восстановить его).
    [[nodiscard]] virtual Result<RestoreResult> restore(const Lease& lease,
                                                        std::span<const domain::EntryId> ids) = 0;
    // Навсегда, с поддеревьями, одной транзакцией; -> сколько записей стерто всего.
    [[nodiscard]] virtual Result<std::size_t> purge(const Lease& lease, const PurgeCmd& cmd) = 0;
};

// ImportExportService

// Что клиент знает о кадре: размер в пикселях и длительность (видео, аудио).
struct MediaInfo {
    std::optional<domain::Dimensions> dimensions; // обе стороны > 0
    std::optional<std::uint64_t> durationMs;
};

// Файл с тем же содержимым (sha256) уже есть в сейфе или раньше в этом же импорте.
enum class DuplicatePolicy {
    Skip,   // не записывать, сообщить в ImportResult::duplicates
    Import, // записать еще раз
};

// Что клиент знает о записи при создании (manifest импорта, пачка ссылок): запись создается
// сразу целиком, без второго запроса. Незаданное не трогается; у замены теги добавляются.
struct EntryDetails {
    std::optional<std::string> description;
    std::vector<domain::TagId> tags; // прямые присвоения, без наследования
    std::optional<std::string> source;
    std::optional<std::string> sourceUrl;
    std::optional<std::int64_t> sourceDate;
};

// Что известно о файле кроме пути: дата исходного файла и как поступить, если имя занято или
// такое содержимое уже есть.
struct ImportFileOptions {
    std::optional<std::int64_t> sourceModifiedAt; // unix-мс изменения файла на диске
    ConflictPolicy onConflict = ConflictPolicy::KeepBoth;
    DuplicatePolicy onDuplicate = DuplicatePolicy::Skip;
    EntryDetails details;
    // Хеш, посчитанный клиентом: не совпал с принятым содержимым - файл не записывается.
    std::optional<domain::Sha256> sha256;
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
    // Миниатюра от клиента (кадр видео, снимок AVIF/HEIC, который сервер не читает): картинка
    // JPEG/PNG/WebP/GIF/BMP до kThumbnailSourceLimit перекодируется, как при импорте (JPEG,
    // <= kThumbnailMaxSide), старая миниатюра стирается. Заданные поля info пишутся в мету.
    // Папка, не картинка, кривые info -> InvalidArgument. modifiedAt не меняется.
    [[nodiscard]] virtual Result<domain::Entry> setThumbnail(const Lease& lease, domain::EntryId id,
                                                             std::span<const std::byte> image,
                                                             const MediaInfo& info) = 0;
    // Сколько импортов еще идет, включая дописывание пачки после оборванного запроса: пока не
    // 0, lookup может не видеть часть принятого (загрузчик архива ждет 0).
    [[nodiscard]] virtual std::size_t activeImports() const = 0;
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
    std::string source; // только с этим источником (без учета регистра); пусто - любые
    ListOptions page;   // по умолчанию - все совпадения по имени
};

struct SourceCount {
    std::string source; // как записан (из разных написаний - первое по алфавиту)
    std::size_t count = 0;
};

struct SearchResults {
    std::vector<domain::SearchHit> hits; // окно выдачи
    std::size_t total = 0;               // совпадений всего, без учета окна
};

class SearchService {
public:
    virtual ~SearchService() = default;

    // Текст - без учета регистра (кириллица включительно, "ё" = "е") в имени, иначе в описании;
    // теги - по прямым и унаследованным; источник - точное совпадение без учета регистра.
    // Ничего не задано -> []. Неизвестный тег -> InvalidArgument, неизвестный within -> NotFound.
    [[nodiscard]] virtual Result<SearchResults> search(const Lease& lease,
                                                       const SearchQuery& query) = 0;
    // Источники записей вне корзины, по убыванию числа записей.
    [[nodiscard]] virtual Result<std::vector<SourceCount>> sources(const Lease& lease) = 0;
};

// LinksService

inline constexpr std::size_t kMaxLinksPerRequest = 10'000;

struct NewLink {
    std::string url;                 // как ввел пользователь; пробелы по краям срезаются
    std::optional<std::string> name; // не задано - хост адреса
    std::string path;                // "Закладки/Работа": папки внутри parent, пусто - сам parent
    EntryDetails details;            // у уже существующей ссылки не применяется
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

// Досчет sha256 содержимого у записей, сохраненных до того, как импорт стал его считать (поиск
// повторов). Идет сам в фоне после открытия сейфа, пачками; блокировку не задерживает.
struct HashProgress {
    std::size_t pending = 0; // записей с содержимым, еще ждущих хеша в этом открытии
    std::size_t failed = 0;  // содержимое не прочиталось (повреждено) - пропущены
};

class ContentHashService {
public:
    virtual ~ContentHashService() = default;

    [[nodiscard]] virtual HashProgress progress(const Lease& lease) const = 0;
    virtual void drain() = 0; // дождаться, пока очередь опустеет (тесты)
};

// Копия открытого сейфа в другой файл: постранично, содержимое не расшифровывается, пароль у
// копии тот же. Идет в фоне шагами (16 МиБ), запросы между шагами не ждут; правки во время
// копии в нее попадают. Блокировка обрывает копию, незаконченный файл удаляется.
enum class BackupState {
    None, // в этом открытии копий не было
    Running,
    Done,
    Failed,
};

struct BackupProgress {
    BackupState state = BackupState::None;
    std::filesystem::path path; // куда
    std::uint64_t bytesDone = 0;
    std::uint64_t bytesTotal = 0;
    std::string error; // Failed: причина
};

class BackupService {
public:
    virtual ~BackupService() = default;

    // path - как у create: относительный - от папки по умолчанию, ".safebox" дописывается.
    // Файл уже есть или копия уже идет -> AlreadyExists, нет папки -> InvalidArgument.
    [[nodiscard]] virtual Result<BackupProgress> start(const Lease& lease,
                                                       std::string_view path) = 0;
    // Последняя копия этого открытия сейфа.
    [[nodiscard]] virtual BackupProgress progress(const Lease& lease) const = 0;
    virtual void drain() = 0; // дождаться конца идущей копии (тесты)
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
    std::shared_ptr<TrashService> trash;
    std::shared_ptr<BackupService> backup;
    std::shared_ptr<ContentHashService> hashes; // последним: его поток останавливается первым
};

} // namespace safebox::app
