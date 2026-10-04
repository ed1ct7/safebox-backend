// Аренды: счетчик + condvar. beginClose() запрещает новые и ставит флаг отмены,
// waitForLeases() ждет ноль.
// Кеш имен строится лениво, generation не дает сохранить устаревший снимок
#include "vault_session.hpp"

#include <algorithm>
#include <unordered_set>

#include "safebox/domain/model/rules.hpp"

namespace safebox::app {

using domain::EntryId;
using domain::Error;
using domain::fail;
using domain::Result;

// регистронезависимый поиск

namespace {

[[nodiscard]] char32_t foldCodePoint(char32_t c) noexcept {
    if (c < 0x80) {
        return (c >= U'A' && c <= U'Z') ? c + 32 : c;
    }
    char32_t lower = c;
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7) {
        lower = c + 32; // Latin-1
    } else if ((c >= 0x100 && c <= 0x137) || (c >= 0x14A && c <= 0x177)) {
        lower = c | 1u; // Latin Extended-A: пары "четный -> нечетный"
    } else if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) {
        lower = (c % 2 == 1) ? c + 1 : c;
    } else if (c == 0x178) {
        lower = 0xFF;
    } else if (c >= 0x391 && c <= 0x3A9 && c != 0x3A2) {
        lower = c + 32; // греческие заглавные
    } else if (c == 0x3C2) {
        lower = 0x3C3; // конечная сигма
    } else if (c >= 0x410 && c <= 0x42F) {
        lower = c + 32; // А-Я
    } else if (c >= 0x400 && c <= 0x40F) {
        lower = c + 80; // Ѐ-Џ (включая Ё)
    } else if ((c >= 0x460 && c <= 0x481) || (c >= 0x48A && c <= 0x4BF) ||
               (c >= 0x4D0 && c <= 0x52F)) {
        lower = c | 1u;
    } else if (c >= 0x4C1 && c <= 0x4CE) {
        lower = (c % 2 == 1) ? c + 1 : c;
    }
    return lower == 0x451 ? char32_t{0x435} : lower; // ё -> е
}

void appendUtf8(std::string& out, char32_t c) {
    if (c < 0x80) {
        out.push_back(static_cast<char>(c));
    } else if (c < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (c >> 6)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    } else if (c < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (c >> 12)));
        out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (c >> 18)));
        out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    }
}

} // namespace

std::string foldForSearch(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        const auto len = domain::detail::utf8SequenceLength(text, i);
        if (len == 0) { // битый байт - как есть
            out.push_back(text[i]);
            ++i;
            continue;
        }
        const auto b0 = static_cast<unsigned char>(text[i]);
        char32_t c =
            len == 1 ? b0 : (len == 2 ? (b0 & 0x1Fu) : (len == 3 ? (b0 & 0x0Fu) : (b0 & 0x07u)));
        for (std::size_t k = 1; k < len; ++k) {
            c = (c << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3Fu);
        }
        appendUtf8(out, foldCodePoint(c));
        i += len;
    }
    return out;
}

std::string uniqueName(std::string_view name, bool isFolder,
                       const std::unordered_set<std::string>& taken, int* resume) {
    if (!taken.contains(foldForSearch(name))) {
        return std::string(name);
    }
    const auto dot = isFolder ? std::string_view::npos : name.rfind('.');
    const bool hasExt = dot != std::string_view::npos && dot > 0;
    const auto stem = hasExt ? name.substr(0, dot) : name;
    const auto ext = hasExt ? name.substr(dot) : std::string_view{};
    for (int n = resume != nullptr ? std::max(*resume, 2) : 2;; ++n) {
        const auto suffix = " (" + std::to_string(n) + ")";
        // имя остается в пределах kMaxNameBytes: лишнее срезается с основы по границе символа
        const auto fixed = suffix.size() + ext.size();
        auto keep = stem;
        if (fixed < domain::kMaxNameBytes && keep.size() > domain::kMaxNameBytes - fixed) {
            auto cut = domain::kMaxNameBytes - fixed;
            while (cut > 0 && (static_cast<unsigned char>(keep[cut]) & 0xC0) == 0x80) {
                --cut;
            }
            keep = keep.substr(0, cut);
        }
        auto candidate = std::string(keep).append(suffix).append(ext);
        if (!taken.contains(foldForSearch(candidate))) {
            if (resume != nullptr) {
                *resume = n;
            }
            return candidate;
        }
    }
}

// Catalog

Catalog::~Catalog() {
    for (auto& [id, node] : nodes_) {
        domain::secureWipe(node.entry.name);
        domain::secureWipe(node.folded);
        domain::secureWipe(node.foldedDescription);
        domain::secureWipe(node.entry.meta.url);
        domain::secureWipe(node.entry.meta.description);
    }
    for (auto& [id, node] : categories_) {
        domain::secureWipe(node.category.name);
        domain::secureWipe(node.category.nameEn);
        domain::secureWipe(node.folded);
        domain::secureWipe(node.foldedEn);
    }
    for (auto& [id, node] : tags_) {
        domain::secureWipe(node.tag.name);
        domain::secureWipe(node.tag.nameEn);
        domain::secureWipe(node.folded);
        domain::secureWipe(node.foldedEn);
    }
}

void Catalog::add(domain::Entry entry) {
    auto folded = foldForSearch(entry.name);
    auto foldedDescription = foldForSearch(entry.meta.description);
    const auto id = entry.id;
    nodes_.insert_or_assign(
        id, Node{std::move(entry), std::move(folded), std::move(foldedDescription), 0});
}

void Catalog::addCategory(domain::TagCategory category) {
    auto folded = foldForSearch(category.name);
    auto foldedEn = foldForSearch(category.nameEn);
    const auto id = category.id;
    categories_.insert_or_assign(
        id, CategoryNode{std::move(category), std::move(folded), std::move(foldedEn)});
}

void Catalog::addTag(domain::Tag tag) {
    auto folded = foldForSearch(tag.name);
    auto foldedEn = foldForSearch(tag.nameEn);
    const auto id = tag.id;
    tags_.insert_or_assign(id, TagNode{std::move(tag), std::move(folded), std::move(foldedEn)});
}

void Catalog::finalize() {
    children_.clear();
    roots_.clear();
    for (auto& [id, node] : nodes_) {
        node.childCount = 0;
        const auto& parent = node.entry.parentId;
        if (parent && *parent != id && nodes_.contains(*parent)) {
            children_[*parent].push_back(id);
        } else {
            roots_.push_back(id);
        }
    }
    // Узлы, недостижимые из корня (цикл в подмененном parent_id), - в корень.
    std::unordered_set<EntryId> reachable;
    std::vector<EntryId> stack(roots_.begin(), roots_.end());
    while (!stack.empty()) {
        const auto id = stack.back();
        stack.pop_back();
        if (!reachable.insert(id).second) {
            continue;
        }
        if (auto it = children_.find(id); it != children_.end()) {
            stack.insert(stack.end(), it->second.begin(), it->second.end());
        }
    }
    for (const auto& [id, node] : nodes_) {
        if (!reachable.contains(id)) {
            roots_.push_back(id);
        }
    }
    const auto less = [this](EntryId a, EntryId b) {
        const auto& na = nodes_.at(a);
        const auto& nb = nodes_.at(b);
        if (na.entry.isFolder() != nb.entry.isFolder()) {
            return na.entry.isFolder();
        }
        if (na.folded != nb.folded) {
            return na.folded < nb.folded;
        }
        return a < b;
    };
    std::sort(roots_.begin(), roots_.end(), less);
    for (auto& [parent, list] : children_) {
        std::sort(list.begin(), list.end(), less);
        nodes_.at(parent).childCount = list.size();
    }
}

const Catalog::Node* Catalog::find(EntryId id) const {
    const auto it = nodes_.find(id);
    return it == nodes_.end() ? nullptr : &it->second;
}

const std::vector<EntryId>& Catalog::childrenOf(std::optional<EntryId> parent) const {
    static const std::vector<EntryId> empty;
    if (!parent) {
        return roots_;
    }
    const auto it = children_.find(*parent);
    return it == children_.end() ? empty : it->second;
}

std::vector<domain::InheritedTag> Catalog::inheritedTags(EntryId id) const {
    std::vector<domain::InheritedTag> out;
    const Node* self = find(id);
    if (self == nullptr) {
        return out;
    }
    std::unordered_set<domain::TagId> known; // прямые и уже найденные у более близких предков
    for (const auto& tag : self->entry.meta.tags) {
        known.insert(tag.tagId);
    }
    std::unordered_set<EntryId> seen{id};
    std::optional<EntryId> current = self->entry.parentId;
    while (current && seen.insert(*current).second) {
        const Node* node = find(*current);
        if (node == nullptr) {
            break;
        }
        for (const auto& tag : node->entry.meta.tags) {
            if (tag.inherit && known.insert(tag.tagId).second) {
                out.push_back({tag.tagId, node->entry.id});
            }
        }
        current = node->entry.parentId;
    }
    std::sort(out.begin(), out.end(),
              [](const auto& a, const auto& b) { return a.tagId < b.tagId; });
    return out;
}

std::vector<domain::TagId> Catalog::effectiveTags(EntryId id) const {
    std::vector<domain::TagId> out;
    const Node* self = find(id);
    if (self == nullptr) {
        return out;
    }
    for (const auto& tag : self->entry.meta.tags) {
        out.push_back(tag.tagId);
    }
    for (const auto& inherited : inheritedTags(id)) {
        out.push_back(inherited.tagId);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

bool Catalog::isAncestorOrSelf(EntryId ancestor, EntryId id) const {
    std::unordered_set<EntryId> seen;
    std::optional<EntryId> current = id;
    while (current && seen.insert(*current).second) {
        if (*current == ancestor) {
            return true;
        }
        const Node* node = find(*current);
        if (node == nullptr) {
            break;
        }
        current = node->entry.parentId;
    }
    return false;
}

domain::Entry Catalog::describe(const Node& node) const {
    auto entry = node.entry;
    entry.childCount = node.childCount;
    entry.inheritedTags = inheritedTags(entry.id);
    return entry;
}

const Catalog::CategoryNode* Catalog::findCategory(domain::CategoryId id) const {
    const auto it = categories_.find(id);
    return it == categories_.end() ? nullptr : &it->second;
}

const Catalog::TagNode* Catalog::findTag(domain::TagId id) const {
    const auto it = tags_.find(id);
    return it == tags_.end() ? nullptr : &it->second;
}

std::vector<domain::PathItem> Catalog::pathTo(EntryId id) const {
    std::vector<domain::PathItem> path;
    std::unordered_set<EntryId> seen;
    std::optional<EntryId> current = id;
    while (current && seen.insert(*current).second) {
        const Node* node = find(*current);
        if (node == nullptr) {
            break;
        }
        path.push_back({node->entry.id, node->entry.name});
        current = node->entry.parentId;
    }
    std::reverse(path.begin(), path.end());
    return path;
}

// VaultSession

namespace {
constexpr std::size_t kMaxTokens = 64; // 32 входа (вкладки/перезагрузки) на сессию
}

VaultSession::VaultSession(std::uint64_t epoch, std::filesystem::path path, domain::SafeMeta meta,
                           std::shared_ptr<const Sealer> sealer, IdlePolicy idle)
    : epoch_(epoch), path_(std::move(path)), chunkSize_(meta.chunkSize), meta_(std::move(meta)),
      sealer_(std::move(sealer)), idle_(idle) {}

VaultSession::~VaultSession() {
    wipe();
}

domain::SafeMeta VaultSession::meta() const {
    std::scoped_lock lock(mutex_);
    return meta_;
}

void VaultSession::setMeta(domain::SafeMeta meta) {
    std::scoped_lock lock(mutex_);
    meta_ = std::move(meta);
}

bool VaultSession::tryAcquire() noexcept {
    std::scoped_lock lock(leasesMutex_);
    if (closing_.load()) {
        return false;
    }
    ++leases_;
    return true;
}

void VaultSession::release() noexcept {
    {
        std::scoped_lock lock(leasesMutex_);
        if (leases_ > 0) {
            --leases_;
        }
    }
    leasesCv_.notify_all();
}

void VaultSession::beginClose() noexcept {
    {
        std::scoped_lock lock(leasesMutex_);
        closing_.store(true);
    }
    leasesCv_.notify_all();
}

bool VaultSession::waitForLeases(std::chrono::milliseconds timeout) {
    std::unique_lock lock(leasesMutex_);
    return leasesCv_.wait_for(lock, timeout, [this] { return leases_ == 0; });
}

std::shared_ptr<const Sealer> VaultSession::sealer() const {
    std::scoped_lock lock(mutex_);
    return sealer_;
}

void VaultSession::addTokens(const SessionTokens& tokens) {
    std::scoped_lock lock(mutex_);
    while (tokens_.size() + 2 > kMaxTokens) {
        domain::secureWipe(tokens_.front().value);
        tokens_.erase(tokens_.begin());
    }
    tokens_.push_back({tokens.api, TokenScope::Api});
    tokens_.push_back({tokens.media, TokenScope::Media});
}

bool VaultSession::hasToken(domain::CryptoSuite& crypto, std::string_view token,
                            TokenScope scope) const {
    std::scoped_lock lock(mutex_);
    bool found = false;
    for (const auto& t : tokens_) {
        // без раннего выхода: время проверки не зависит от позиции токена
        found = (crypto.equalConstantTime(t.value, token) && t.scope == scope) || found;
    }
    return found;
}

void VaultSession::wipe() noexcept {
    std::shared_ptr<const Sealer> sealer;
    CatalogPtr catalog;
    {
        std::scoped_lock lock(mutex_);
        sealer = std::move(sealer_);
        catalog = std::move(catalog_);
        ++catalogGeneration_;
        previewPending_.clear();
        for (auto& t : tokens_) {
            domain::secureWipe(t.value);
        }
        tokens_.clear();
    }
    // Отпускаем вне мьютекса: если это последние владельцы, ключи стираются в
    // адаптере (sodium_free), а имена - в ~Catalog.
    catalog.reset();
    sealer.reset();
}

void VaultSession::heartbeat(MonoTime now, bool active) {
    std::scoped_lock lock(mutex_);
    idle_.heartbeat(now, active);
}

bool VaultSession::idleExpired(MonoTime now) const {
    std::scoped_lock lock(mutex_);
    return idle_.expired(now);
}

std::chrono::seconds VaultSession::idleRemaining(MonoTime now) const {
    std::scoped_lock lock(mutex_);
    return idle_.idleRemaining(now);
}

Result<VaultSession::CatalogPtr> VaultSession::catalog(const Loader& load) {
    std::scoped_lock build(catalogBuildMutex_);
    std::uint64_t generation = 0;
    {
        std::scoped_lock lock(mutex_);
        if (catalog_) {
            return catalog_;
        }
        if (!sealer_) {
            return fail(Error::Code::Locked, "Сейф заблокирован");
        }
        generation = catalogGeneration_;
    }
    auto built = load();
    if (!built) {
        return std::unexpected(built.error());
    }
    std::scoped_lock lock(mutex_);
    if (generation == catalogGeneration_) {
        catalog_ = *built;
    }
    return *built;
}

void VaultSession::invalidateCatalog() noexcept {
    std::scoped_lock lock(mutex_);
    ++catalogGeneration_;
    catalog_.reset();
}

void VaultSession::setPreviewPending(domain::EntryId id, bool pending) {
    std::scoped_lock lock(mutex_);
    if (pending) {
        previewPending_.insert(id);
    } else {
        previewPending_.erase(id);
    }
}

bool VaultSession::previewPending(domain::EntryId id) const {
    std::scoped_lock lock(mutex_);
    return previewPending_.contains(id);
}

// Lease

Lease::Lease(std::shared_ptr<VaultSession> session) noexcept : session_(std::move(session)) {}

Lease::Lease(Lease&& other) noexcept : session_(std::move(other.session_)) {}

Lease& Lease::operator=(Lease&& other) noexcept {
    if (this != &other) {
        release();
        session_ = std::move(other.session_);
    }
    return *this;
}

Lease::~Lease() {
    release();
}

void Lease::release() noexcept {
    if (session_) {
        session_->release();
        session_.reset();
    }
}

bool Lease::cancelled() const noexcept {
    return session_ && session_->cancelled();
}

std::optional<Lease> Lease::share() const {
    if (!session_) {
        return Lease(nullptr); // пустая аренда (тесты транспорта)
    }
    if (!session_->tryAcquire()) {
        return std::nullopt;
    }
    return Lease(session_);
}

// помощники сервисов

std::optional<Lease> leaseFrom(const std::weak_ptr<VaultSession>& weak) {
    auto session = weak.lock();
    if (!session || !session->tryAcquire()) {
        return std::nullopt;
    }
    return Lease(std::move(session));
}

Result<OperationContext> contextOf(const Lease& lease) {
    VaultSession* session = lease.session();
    if (session == nullptr) {
        return fail(Error::Code::Internal, "Операция без аренды сессии");
    }
    if (session->cancelled()) {
        return fail(Error::Code::Locked, "Сейф заблокирован");
    }
    auto sealer = session->sealer();
    if (!sealer) {
        return fail(Error::Code::Locked, "Сейф заблокирован");
    }
    return OperationContext{*session, std::move(sealer)};
}

Result<VaultSession::CatalogPtr> loadCatalog(domain::VaultStore& store, const Sealer& sealer) {
    std::vector<domain::EntryRecord> rows;
    std::vector<domain::TagCategoryRecord> categoryRows;
    std::vector<domain::TagRecord> tagRows;
    {
        auto uow = store.begin();
        if (!uow) {
            return std::unexpected(uow.error());
        }
        auto all = (*uow)->entries().all();
        if (!all) {
            return std::unexpected(all.error());
        }
        auto categories = (*uow)->tags().categories();
        if (!categories) {
            return std::unexpected(categories.error());
        }
        auto tags = (*uow)->tags().tags();
        if (!tags) {
            return std::unexpected(tags.error());
        }
        rows = std::move(*all);
        categoryRows = std::move(*categories);
        tagRows = std::move(*tags);
    } // только чтение: откат, мьютекс хранилища свободен до расшифровки
    auto catalog = std::make_shared<Catalog>();
    for (const auto& row : rows) {
        auto entry = sealer.openEntry(row);
        if (!entry) {
            return std::unexpected(entry.error());
        }
        catalog->add(std::move(*entry));
    }
    for (const auto& row : categoryRows) {
        auto category = sealer.openCategory(row);
        if (!category) {
            return std::unexpected(category.error());
        }
        catalog->addCategory(std::move(*category));
    }
    for (const auto& row : tagRows) {
        auto tag = sealer.openTag(row);
        if (!tag) {
            return std::unexpected(tag.error());
        }
        catalog->addTag(std::move(*tag));
    }
    catalog->finalize();
    return VaultSession::CatalogPtr(std::move(catalog));
}

Result<VaultSession::CatalogPtr> catalogOf(const OperationContext& ctx, domain::VaultStore& store) {
    const auto& sealer = *ctx.sealer;
    return ctx.session.catalog([&store, &sealer] { return loadCatalog(store, sealer); });
}

domain::Result<domain::BlobId> writeBlob(domain::BlobStore& blobs, const Sealer& sealer,
                                         std::uint32_t chunkSize, domain::KeyPurpose purpose,
                                         std::span<const std::byte> data) {
    auto writer = blobs.create();
    if (!writer) {
        return std::unexpected(writer.error());
    }
    const auto id = (*writer)->id();
    const auto discard = [&](const domain::Error& e) -> domain::Result<domain::BlobId> {
        (void)blobs.discard(id);
        return std::unexpected(e);
    };
    const auto count = domain::chunkCountFor(data.size(), chunkSize);
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto from = std::size_t{i} * chunkSize;
        const auto part = data.subspan(from, std::min<std::size_t>(chunkSize, data.size() - from));
        auto sealed = sealer.sealChunk(purpose, id, i, i + 1 == count, part);
        if (!sealed) {
            return discard(sealed.error());
        }
        if (auto st = (*writer)->append(*sealed); !st) {
            return discard(st.error());
        }
    }
    if (auto st = (*writer)->finish(data.size()); !st) {
        return discard(st.error());
    }
    return id;
}

domain::Entry describeEntry(const VaultSession& session, const Catalog& catalog,
                            const Catalog::Node& node) {
    auto entry = catalog.describe(node);
    entry.previewPending =
        entry.meta.kind == domain::Kind::Link && session.previewPending(entry.id);
    return entry;
}

} // namespace safebox::app
