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

// Имя, не занятое среди соседей: "имя (2).ext", "имя (3).ext"... taken - folded-имена соседей
// (результат туда не добавляется). Для папок расширения нет. resume - для тех, кто подбирает много
// имен подряд от одной основы при растущем taken: номер, с которого продолжить (меньше 2 - с
// двойки), после вызова там выбранный номер; без него каждый вызов пробует все номера с двойки.
[[nodiscard]] std::string uniqueName(std::string_view name, bool isFolder,
                                     const std::unordered_set<std::string>& taken,
                                     int* resume = nullptr);

class Catalog {
public:
    struct Node {
        domain::Entry entry; // без childCount и inheritedTags - их выдает describe()
        std::string folded;  // foldForSearch(entry.name)
        std::string foldedDescription;
        std::size_t childCount = 0; // прямых детей
    };
    struct CategoryNode {
        domain::TagCategory category;
        std::string folded;
    };
    struct TagNode {
        domain::Tag tag;
        std::string folded;
    };

    Catalog() = default;
    Catalog(const Catalog&) = delete;
    Catalog& operator=(const Catalog&) = delete;
    ~Catalog(); // имена стираются из памяти

    void add(domain::Entry entry);
    void addCategory(domain::TagCategory category);
    void addTag(domain::Tag tag);
    // Списки детей (у любой записи, не только у папки): папки первыми, дальше по имени.
    // Сироты (родитель не найден) и циклы показываются в корне, а не теряются.
    void finalize();

    [[nodiscard]] const Node* find(domain::EntryId id) const;
    [[nodiscard]] const std::vector<domain::EntryId>&
    childrenOf(std::optional<domain::EntryId> parent) const;
    // Путь от корня до id включительно; устойчив к циклам в parent_id.
    [[nodiscard]] std::vector<domain::PathItem> pathTo(domain::EntryId id) const;
    [[nodiscard]] const std::unordered_map<domain::EntryId, Node>& nodes() const noexcept {
        return nodes_;
    }

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
    std::unordered_map<domain::EntryId, Node> nodes_;
    std::unordered_map<domain::CategoryId, CategoryNode> categories_;
    std::unordered_map<domain::TagId, TagNode> tags_;
    std::unordered_map<domain::EntryId, std::vector<domain::EntryId>> children_;
    std::vector<domain::EntryId> roots_;
};

class VaultSession {
public:
    using MonoTime = domain::Clock::MonoTime;
    using CatalogPtr = std::shared_ptr<const Catalog>;
    using Loader = std::function<domain::Result<CatalogPtr>()>;

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

    // кэш расшифрованных имен
    [[nodiscard]] domain::Result<CatalogPtr> catalog(const Loader& load);
    void invalidateCatalog() noexcept;

    // Ссылки, ждущие предпросмотра: очередь и ее задания у LinksService, а признак для ответов
    // хранит сессия - с блокировкой он исчезает вместе с ней.
    void setPreviewPending(domain::EntryId id, bool pending);
    [[nodiscard]] bool previewPending(domain::EntryId id) const;

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
    CatalogPtr catalog_;
    std::uint64_t catalogGeneration_ = 0;
    std::unordered_set<domain::EntryId> previewPending_;

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
[[nodiscard]] domain::Result<VaultSession::CatalogPtr> loadCatalog(domain::VaultStore& store,
                                                                   const Sealer& sealer);
[[nodiscard]] domain::Result<VaultSession::CatalogPtr> catalogOf(const OperationContext& ctx,
                                                                 domain::VaultStore& store);
// Небольшой блоб целиком из памяти (миниатюра): pending, пока запись не сделает promote.
[[nodiscard]] domain::Result<domain::BlobId>
writeBlob(domain::BlobStore& blobs, const Sealer& sealer, std::uint32_t chunkSize,
          domain::KeyPurpose purpose, std::span<const std::byte> data);
// Запись для ответа: Catalog::describe() и признак очереди предпросмотра из сессии.
[[nodiscard]] domain::Entry describeEntry(const VaultSession& session, const Catalog& catalog,
                                          const Catalog::Node& node);

} // namespace safebox::app
