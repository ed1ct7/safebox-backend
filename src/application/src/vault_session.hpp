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
#include <string>
#include <string_view>
#include <unordered_map>
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

class Catalog {
public:
    struct Node {
        domain::Entry entry;
        std::string folded; // foldForSearch(entry.name)
    };

    Catalog() = default;
    Catalog(const Catalog&) = delete;
    Catalog& operator=(const Catalog&) = delete;
    ~Catalog(); // имена стираются из памяти

    void add(domain::Entry entry);
    // Списки детей: папки первыми, дальше по имени. Сироты (родитель не найден
    // или не папка) показываются в корне, а не теряются.
    void finalize();

    [[nodiscard]] const Node* find(domain::EntryId id) const;
    [[nodiscard]] const std::vector<domain::EntryId>&
    childrenOf(std::optional<domain::EntryId> parent) const;
    // Путь от корня до id включительно; устойчив к циклам в parent_id.
    [[nodiscard]] std::vector<domain::PathItem> pathTo(domain::EntryId id) const;
    [[nodiscard]] const std::unordered_map<domain::EntryId, Node>& nodes() const noexcept {
        return nodes_;
    }

private:
    std::unordered_map<domain::EntryId, Node> nodes_;
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

    std::mutex catalogBuildMutex_; // один построитель кэша за раз

    std::mutex leasesMutex_;
    std::condition_variable leasesCv_;
    std::size_t leases_ = 0;
    std::atomic<bool> closing_{false};
};

// общие помощники сервисов

struct OperationContext {
    VaultSession& session;
    std::shared_ptr<const Sealer> sealer; // держит ключи живыми до конца операции
};

// Аренда -> сессия и ключи; закрывающийся/стертый сейф -> Locked.
[[nodiscard]] domain::Result<OperationContext> contextOf(const Lease& lease);
// Все записи одной транзакцией -> расшифровка вне мьютекса хранилища.
[[nodiscard]] domain::Result<VaultSession::CatalogPtr> loadCatalog(domain::VaultStore& store,
                                                                   const Sealer& sealer);
[[nodiscard]] domain::Result<VaultSession::CatalogPtr> catalogOf(const OperationContext& ctx,
                                                                 domain::VaultStore& store);

} // namespace safebox::app
