// VaultSession - один открытый сейф (внутренний класс application, http его не видит).
// Держит ключи (Sealer), токены, IdlePolicy, кеш расшифрованных имен и счетчик аренд
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "idle_policy.hpp"
#include "safebox/app/session.hpp"
#include "safebox/domain/model/entry.hpp"
#include "safebox/domain/ports/crypto.hpp"
#include "safebox/domain/ports/storage.hpp"
#include "sealing.hpp"

namespace safebox::app {

// Регистронезависимая форма для поиска: латиница (с Latin-1/Ext-A), греческий,
// кириллица; "ё" приравнена к "е".
[[nodiscard]] std::string foldForSearch(std::string_view text);

// Номер в имени ставится в конец, а не перед расширением: папка и ссылка ("example.com").
[[nodiscard]] inline bool wholeName(const domain::Entry& entry) {
    return entry.isFolder() || entry.meta.kind == domain::Kind::Link;
}

// Имя, не занятое среди соседей: "имя (2).ext", "имя (3).ext"... taken - folded-имена соседей
// (результат туда не добавляется). isFolder - расширения нет (wholeName): "a.com (2)". resume -
// для тех, кто подбирает много имен подряд от одной основы при растущем taken: номер, с которого
// продолжить (меньше 2 - с двойки), после вызова там выбранный номер; без него каждый вызов
// пробует все номера с двойки.
[[nodiscard]] std::string uniqueName(std::string_view name, bool isFolder,
                                     const std::unordered_set<std::string>& taken,
                                     int* resume = nullptr);

// Ключ хеш-таблиц по sha256 содержимого.
struct Sha256Hash {
    [[nodiscard]] std::size_t operator()(const domain::Sha256& hash) const noexcept;
};

class Catalog {
public:
    // Узел не меняется после создания и делится между копиями каталога (clone): правка записи
    // заменяет узел целиком. Имена стирает последний владелец.
    struct Node {
        domain::Entry entry; // без childCount и inheritedTags - их выдает describe()
        std::string folded;  // foldForSearch(entry.name)
        std::string foldedDescription;
        ~Node();
    };
    using NodePtr = std::shared_ptr<const Node>;
    struct CategoryNode {
        domain::TagCategory category;
        std::string folded;   // foldForSearch(name)
        std::string foldedEn; // foldForSearch(nameEn); пуст, если второе имя не задано
    };
    struct TagNode {
        domain::Tag tag;
        std::string folded;
        std::string foldedEn;
    };

    Catalog() = default;
    Catalog& operator=(const Catalog&) = delete;
    ~Catalog(); // имена категорий и тегов стираются из памяти (записей - в ~Node)

    void add(domain::Entry entry);
    void addCategory(domain::TagCategory category);
    void addTag(domain::Tag tag);
    // Списки детей (у любой записи, не только у папки): папки первыми, дальше по имени.
    // Сироты (родитель не найден) и циклы показываются в корне, а не теряются. Корни корзины
    // (meta.trashedAt) - не в списке родителя, а в trash(); их потомки - в своих списках.
    void finalize();

    // Правки готового каталога (после finalize). Копия делит узлы с оригиналом.
    [[nodiscard]] std::shared_ptr<Catalog> clone() const;
    // Записи, которых больше нет в сейфе, уходят с потомками (и с корнями корзины внутри);
    // спуск останавливается на keep - записях, которые есть в сейфе (их могли перенести из
    // удаленной папки, и детей с ними).
    void erase(std::span<const domain::EntryId> ids,
               const std::unordered_set<domain::EntryId>& keep);
    // Новые и измененные записи встают на места по своим parentId. false - правку этим путем
    // не сделать (цикл в каталоге или в правке): каталог испорчен, его надо построить заново.
    [[nodiscard]] bool upsert(std::vector<domain::Entry> entries);
    // Все категории и теги заново.
    void replaceTags(std::vector<domain::TagCategory> categories, std::vector<domain::Tag> tags);

    [[nodiscard]] const Node* find(domain::EntryId id) const;
    // Дети без корней корзины (у записи в корзине - ее дети, они тоже в корзине).
    [[nodiscard]] const std::vector<domain::EntryId>&
    childrenOf(std::optional<domain::EntryId> parent) const;
    // Корни корзины: записи с meta.trashedAt, по имени.
    [[nodiscard]] const std::vector<domain::EntryId>& trash() const noexcept { return trash_; }
    // Запись или один из ее предков в корзине.
    [[nodiscard]] bool inTrash(domain::EntryId id) const;
    // Все записи в корзине (корни с потомками).
    [[nodiscard]] std::unordered_set<domain::EntryId> trashedIds() const;
    // Путь от корня до id включительно; устойчив к циклам в parent_id.
    [[nodiscard]] std::vector<domain::PathItem> pathTo(domain::EntryId id) const;
    [[nodiscard]] const std::unordered_map<domain::EntryId, NodePtr>& nodes() const noexcept {
        return nodes_;
    }
    [[nodiscard]] std::size_t childCount(domain::EntryId id) const; // прямых детей
    // Запись с таким содержимым вне корзины (с меньшим id, если их несколько); nullptr - нет.
    [[nodiscard]] const Node* findBySha256(const domain::Sha256& hash) const;

    // Теги с inherit у предков (устойчиво к циклам): ближайший предок побеждает, теги, уже
    // прямые у самой записи, не повторяются. По возрастанию tagId.
    [[nodiscard]] std::vector<domain::InheritedTag> inheritedTags(domain::EntryId id) const;
    // Прямые и унаследованные теги записи, по возрастанию, без повторов (для фильтра).
    [[nodiscard]] std::vector<domain::TagId> effectiveTags(domain::EntryId id) const;
    // ancestor - сама запись id или один из ее предков.
    [[nodiscard]] bool isAncestorOrSelf(domain::EntryId ancestor, domain::EntryId id) const;
    // Запись для ответа: с childCount и inheritedTags.
    [[nodiscard]] domain::Entry describe(const Node& node) const;

    [[nodiscard]] const std::unordered_map<domain::CategoryId, CategoryNode>&
    categories() const noexcept {
        return categories_;
    }
    [[nodiscard]] const std::unordered_map<domain::TagId, TagNode>& tags() const noexcept {
        return tags_;
    }
    [[nodiscard]] const CategoryNode* findCategory(domain::CategoryId id) const;
    [[nodiscard]] const TagNode* findTag(domain::TagId id) const;

private:
    Catalog(const Catalog&) = default; // для clone: узлы общие, категории и теги - копии

    [[nodiscard]] bool before(domain::EntryId a, domain::EntryId b) const;
    // Список, в котором стоит запись: корзина, дети родителя или корень (родителя нет в
    // каталоге); nullptr - у родителя нет списка детей (create = false).
    [[nodiscard]] std::vector<domain::EntryId>* placeOf(const Node& node, bool create);
    void unlink(domain::EntryId id); // из списка по текущему узлу; нет там - ничего
    // Запись с потомками (кроме keep) - из узлов и списков; ушедшие - в erased.
    void eraseSubtree(domain::EntryId id, const std::unordered_set<domain::EntryId>& keep,
                      std::unordered_set<domain::EntryId>& erased);
    void wipeTags() noexcept;
    void index(const Node& node);   // в bySha256_, если хеш известен
    void unindex(const Node& node); // из bySha256_

    std::unordered_map<domain::EntryId, NodePtr> nodes_;
    std::unordered_map<domain::CategoryId, CategoryNode> categories_;
    std::unordered_map<domain::TagId, TagNode> tags_;
    std::unordered_map<domain::EntryId, std::vector<domain::EntryId>> children_;
    std::vector<domain::EntryId> roots_;
    std::vector<domain::EntryId> trash_;
    std::unordered_multimap<domain::Sha256, domain::EntryId, Sha256Hash> bySha256_;
    bool hasCycles_ = false; // в файле цикл parent_id: правки только полной перестройкой
};

// Что изменилось в сейфе после коммита: каталог перечитает только это.
struct CatalogChanges {
    std::vector<domain::EntryId> entries; // созданные, измененные и удаленные (с поддеревом)
    bool tags = false;                    // категории и теги
};

class VaultSession {
public:
    using MonoTime = domain::Clock::MonoTime;
    using CatalogPtr = std::shared_ptr<const Catalog>;
    using Loader = std::function<domain::Result<std::shared_ptr<Catalog>>()>;
    // Правит каталог по накопленным изменениям; false или ошибка - построить заново.
    using Patcher = std::function<domain::Result<bool>(Catalog&, const CatalogChanges&)>;

    VaultSession(std::uint64_t epoch, std::filesystem::path path, domain::SafeMeta meta,
                 std::shared_ptr<const Sealer> sealer, IdlePolicy idle);
    VaultSession(const VaultSession&) = delete;
    VaultSession& operator=(const VaultSession&) = delete;
    ~VaultSession();

    [[nodiscard]] std::uint64_t epoch() const noexcept { return epoch_; }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
    [[nodiscard]] std::uint32_t chunkSize() const noexcept { return chunkSize_; }
    [[nodiscard]] domain::SafeMeta meta() const;
    void setMeta(domain::SafeMeta meta);

    // аренды
    [[nodiscard]] bool tryAcquire() noexcept; // false - сессия уже закрывается
    void release() noexcept;
    void beginClose() noexcept; // отмена операций + запрет новых аренд
    [[nodiscard]] bool cancelled() const noexcept { return closing_.load(); }
    [[nodiscard]] bool waitForLeases(std::chrono::milliseconds timeout);

    // ключи и токены
    [[nodiscard]] std::shared_ptr<const Sealer> sealer() const; // nullptr после wipe
    void addTokens(const SessionTokens& tokens);
    [[nodiscard]] bool hasToken(domain::CryptoSuite& crypto, std::string_view token,
                                TokenScope scope) const;
    void wipe() noexcept; // ключи, токены, кэш имен

    // присутствие
    void heartbeat(MonoTime now, bool active);
    [[nodiscard]] bool idleExpired(MonoTime now) const;
    [[nodiscard]] std::chrono::seconds idleRemaining(MonoTime now) const;

    // кэш расшифрованных имен: строится целиком при первом чтении, дальше правится по
    // изменениям, отмеченным catalogChanged() (их применяет следующее чтение).
    [[nodiscard]] domain::Result<CatalogPtr> catalog(const Loader& load, const Patcher& patch);
    void catalogChanged(const CatalogChanges& changes) noexcept; // после коммита
    void invalidateCatalog() noexcept;                           // следующее чтение - с нуля

    // Ссылки, ждущие предпросмотра: очередь и ее задания у LinksService, а признак для ответов
    // хранит сессия - с блокировкой он исчезает вместе с ней.
    void setPreviewPending(domain::EntryId id, bool pending);
    [[nodiscard]] bool previewPending(domain::EntryId id) const;

    // Досчет sha256 старых записей (ContentHashService): сколько осталось и не прочиталось.
    void setHashProgress(std::size_t pending, std::size_t failed) noexcept {
        hashesPending_.store(pending);
        hashesFailed_.store(failed);
    }
    [[nodiscard]] std::size_t hashesPending() const noexcept { return hashesPending_.load(); }
    [[nodiscard]] std::size_t hashesFailed() const noexcept { return hashesFailed_.load(); }

private:
    struct Token {
        std::string value;
        TokenScope scope;
    };

    const std::uint64_t epoch_;
    const std::filesystem::path path_;
    const std::uint32_t chunkSize_;

    mutable std::mutex mutex_; // meta, sealer, tokens, idle, catalog
    domain::SafeMeta meta_;
    std::shared_ptr<const Sealer> sealer_;
    std::vector<Token> tokens_;
    IdlePolicy idle_;
    std::shared_ptr<Catalog> catalog_;
    std::uint64_t catalogGeneration_ = 0;
    std::unordered_set<domain::EntryId> pendingEntries_; // изменения после построения catalog_
    bool pendingTags_ = false;
    std::unordered_set<domain::EntryId> previewPending_;
    std::atomic<std::size_t> hashesPending_{0};
    std::atomic<std::size_t> hashesFailed_{0};

    std::mutex catalogBuildMutex_; // один построитель кэша за раз

    std::mutex leasesMutex_;
    std::condition_variable leasesCv_;
    std::size_t leases_ = 0;
    std::atomic<bool> closing_{false};
};

// общие помощники сервисов

// Еще одна аренда сессии, за которой следят по weak_ptr (фоновая работа держит ее только пока
// пишет); nullopt - сессии уже нет или она закрывается.
[[nodiscard]] std::optional<Lease> leaseFrom(const std::weak_ptr<VaultSession>& session);

struct OperationContext {
    VaultSession& session;
    std::shared_ptr<const Sealer> sealer; // держит ключи живыми до конца операции
};

// Аренда -> сессия и ключи; закрывающийся/стертый сейф -> Locked.
[[nodiscard]] domain::Result<OperationContext> contextOf(const Lease& lease);
// Записи, категории и теги одной транзакцией -> расшифровка вне мьютекса хранилища.
[[nodiscard]] domain::Result<std::shared_ptr<Catalog>> loadCatalog(domain::VaultStore& store,
                                                                   const Sealer& sealer);
// Перечитывает изменившиеся строки одной транзакцией и правит ими каталог.
[[nodiscard]] domain::Result<bool> patchCatalog(Catalog& catalog, const CatalogChanges& changes,
                                                domain::VaultStore& store, const Sealer& sealer);
[[nodiscard]] domain::Result<VaultSession::CatalogPtr> catalogOf(const OperationContext& ctx,
                                                                 domain::VaultStore& store);
// Небольшой блоб целиком из памяти (миниатюра): pending, пока запись не сделает promote.
[[nodiscard]] domain::Result<domain::BlobId>
writeBlob(domain::BlobStore& blobs, const Sealer& sealer, std::uint32_t chunkSize,
          domain::KeyPurpose purpose, std::span<const std::byte> data);
// Родитель для новых и перенесенных записей: nullopt (корень) или запись вне корзины.
// Нет записи -> NotFound, в корзине -> InvalidArgument.
[[nodiscard]] domain::Status requireLive(const Catalog& catalog, std::optional<domain::EntryId> id);
// В текущей транзакции: meta.trashedAt = at (nullopt - из корзины), rename - новое имя.
[[nodiscard]] domain::Status markTrashed(domain::EntryRepository& entries, const Sealer& sealer,
                                         domain::EntryId id, std::optional<std::int64_t> at,
                                         const std::string* rename = nullptr);
// Запись для ответа: Catalog::describe() и признак очереди предпросмотра из сессии.
[[nodiscard]] domain::Entry describeEntry(const VaultSession& session, const Catalog& catalog,
                                          const Catalog::Node& node);

} // namespace safebox::app
