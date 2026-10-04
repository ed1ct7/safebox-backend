// SafeService: create/unlock/lock, смена пароля, authorize.
// мьютексы: lifecycle - create/unlock/lock по очереди (argon2 по 256 МБ параллельно
// гонять незачем), state - короткий доступ к current_/epoch_, password - смена пароля.
// authorize берет только state
#include <mutex>
#include <system_error>

#include "safebox/domain/model/rules.hpp"
#include "safebox/domain/model/safe_format.hpp"
#include "service_impls.hpp"
#include "vault_session.hpp"

namespace safebox::app {
namespace {

using domain::Error;
using domain::fail;
using domain::KeyHandle;
using domain::SafeMeta;

[[nodiscard]] Status validateMeta(const SafeMeta& meta) {
    if (meta.formatVersion != domain::kSealVersion || meta.salt.size() != domain::kSaltSize ||
        meta.chunkSize < domain::kMinChunkSize || meta.chunkSize > domain::kMaxChunkSize ||
        meta.envelope.size() != domain::kSealOverhead + domain::kKeySize) {
        return fail(Error::Code::NotASafe, "Файл сейфа повреждён");
    }
    return {};
}

[[nodiscard]] bool sameEnvelope(const SafeMeta& a, const SafeMeta& b) {
    return a.formatVersion == b.formatVersion && a.kdf == b.kdf && a.salt == b.salt &&
           a.envelope == b.envelope;
}

[[nodiscard]] bool hasSafeExtension(const std::filesystem::path& path) {
    return domain::detail::iequals(domain::pathToUtf8(path.extension()), domain::kSafeExtension);
}

// Тот же файл? На диске - equivalent (регистр, 8.3, ссылки); фейковому
// хранилищу тестов хватает сравнения нормализованных путей.
[[nodiscard]] bool sameSafe(const std::filesystem::path& open, const std::filesystem::path& asked) {
    const auto withExt = domain::withSafeExtension(asked);
    std::error_code ec;
    if (std::filesystem::equivalent(open, asked, ec) ||
        std::filesystem::equivalent(open, withExt, ec)) {
        return true;
    }
    const auto normal = open.lexically_normal();
    return normal == asked.lexically_normal() || normal == withExt.lexically_normal();
}

class SafeServiceImpl final : public SafeService {
public:
    SafeServiceImpl(Ports ports, AppConfig config) : ports_(ports), config_(std::move(config)) {}
    ~SafeServiceImpl() override { lock(); }

    Result<UnlockResult> create(const CreateSafeCmd& cmd) override {
        if (auto st = domain::validateNewPassword(cmd.password, cmd.confirm); !st) {
            return std::unexpected(st.error());
        }
        auto resolved = resolvePath(cmd.path);
        if (!resolved) {
            return std::unexpected(resolved.error());
        }
        const auto path = domain::withSafeExtension(*resolved);
        std::error_code ec;
        if (std::filesystem::exists(path, ec)) {
            // до lock текущего сейфа: неудачная попытка не должна выкидывать сессию
            return fail(Error::Code::AlreadyExists, "Файл уже существует");
        }

        std::scoped_lock lifecycle(lifecycleMutex_);
        lockLocked();

        SafeMeta meta;
        meta.formatVersion = domain::kSealVersion;
        meta.kdf = config_.kdf;
        meta.salt = ports_.crypto.generateSalt();
        meta.chunkSize = config_.chunkSize;
        auto master = ports_.crypto.generateKey();
        if (!master) {
            return std::unexpected(master.error());
        }
        auto kek = ports_.crypto.deriveKek(cmd.password, meta.salt, meta.kdf);
        if (!kek) {
            return std::unexpected(kek.error());
        }
        auto envelope = ports_.crypto.wrapKey(
            *kek, *master, domain::aad::envelope(meta.formatVersion, meta.salt, meta.kdf));
        if (!envelope) {
            return std::unexpected(envelope.error());
        }
        meta.envelope = std::move(*envelope);
        if (auto st = ports_.store.create(path, meta); !st) {
            return std::unexpected(st.error());
        }
        auto result = startSession(path, std::move(meta), std::move(*master));
        if (!result) {
            ports_.store.close();
        }
        return result;
    }

    Result<UnlockResult> unlock(const UnlockCmd& cmd) override {
        auto resolved = resolvePath(cmd.path);
        if (!resolved) {
            return std::unexpected(resolved.error());
        }
        if (cmd.password.empty()) {
            return fail(Error::Code::InvalidArgument, "Введите пароль");
        }

        std::scoped_lock lifecycle(lifecycleMutex_);
        auto path = *resolved;
        std::optional<std::pair<SafeMeta, KeyHandle>> prechecked;
        if (auto session = current()) {
            if (sameSafe(session->path(), path)) {
                // тот же сейф из другой вкладки: пароль проверяется так же честно
                if (auto master = unwrapMaster(cmd.password, session->meta()); !master) {
                    return std::unexpected(master.error());
                }
                session->heartbeat(ports_.clock.monotonic(), true);
                auto tokens = issueTokens(*session);
                auto info = describe(*session);
                if (!info) {
                    return std::unexpected(info.error());
                }
                return UnlockResult{std::move(tokens), std::move(*info)};
            }
            // Другой сейф: файл и пароль проверяются до блокировки открытого -
            // опечатка в пути или пароле не выбрасывает текущую сессию.
            auto inspected = ports_.store.inspect(path);
            if (!inspected && inspected.error().code == Error::Code::NotFound &&
                !hasSafeExtension(path)) {
                const auto alt = domain::withSafeExtension(path);
                if (auto second = ports_.store.inspect(alt);
                    second || second.error().code != Error::Code::NotFound) {
                    inspected = std::move(second);
                    path = alt;
                }
            }
            if (inspected) {
                if (auto st = validateMeta(*inspected); !st) {
                    return std::unexpected(st.error());
                }
                auto master = unwrapMaster(cmd.password, *inspected);
                if (!master) {
                    return std::unexpected(master.error());
                }
                prechecked.emplace(std::move(*inspected), std::move(*master));
            } else if (inspected.error().code != Error::Code::IoError) {
                return std::unexpected(inspected.error());
            } // IoError (например, журнал после сбоя) - решит обычное открытие ниже
            lockLocked();
        }

        auto opened = ports_.store.open(path);
        if (!opened && opened.error().code == Error::Code::NotFound && !hasSafeExtension(path)) {
            const auto alt = domain::withSafeExtension(path);
            if (auto second = ports_.store.open(alt);
                second || second.error().code != Error::Code::NotFound) {
                opened = second;
                path = alt;
            }
        }
        if (!opened) {
            return std::unexpected(opened.error());
        }

        const auto closeWith = [this](Error error) -> Result<UnlockResult> {
            ports_.store.close();
            return std::unexpected(std::move(error));
        };
        auto meta = ports_.store.meta();
        if (!meta) {
            return closeWith(meta.error());
        }
        if (auto st = validateMeta(*meta); !st) {
            return closeWith(st.error());
        }
        // Файл не изменился после проверки - второй Argon2id не нужен.
        auto master = prechecked && sameEnvelope(prechecked->first, *meta)
                          ? Result<KeyHandle>(prechecked->second)
                          : unwrapMaster(cmd.password, *meta);
        if (!master) {
            return closeWith(master.error());
        }
        (void)ports_.store.blobs().gcPending(); // хвосты импорта, прерванного сбоем
        auto result = startSession(path, std::move(*meta), std::move(*master));
        if (!result) {
            ports_.store.close();
        }
        return result;
    }

    void lock() noexcept override {
        std::scoped_lock lifecycle(lifecycleMutex_);
        lockLocked();
    }

    PublicStatus publicStatus() override {
        std::scoped_lock state(stateMutex_);
        return PublicStatus{current_ != nullptr, lastPath_,
                            domain::pathToUtf8(config_.defaultDirectory)};
    }

    Result<SafeInfo> info(const Lease& lease) override {
        auto ctx = contextOf(lease);
        if (!ctx) {
            return std::unexpected(ctx.error());
        }
        return describe(ctx->session);
    }

    Status changePassword(const Lease& lease, const ChangePasswordCmd& cmd) override {
        if (cmd.oldPassword.empty()) {
            return fail(Error::Code::InvalidArgument, "Введите текущий пароль");
        }
        if (auto st = domain::validateNewPassword(cmd.newPassword, cmd.confirm); !st) {
            return st;
        }
        auto ctx = contextOf(lease);
        if (!ctx) {
            return std::unexpected(ctx.error());
        }
        std::scoped_lock password(passwordMutex_);
        auto meta = ctx->session.meta();
        if (auto old = unwrapMaster(cmd.oldPassword, meta); !old) {
            return std::unexpected(old.error()); // данные не тронуты
        }
        SafeMeta next = meta;
        next.salt = ports_.crypto.generateSalt();
        auto kek = ports_.crypto.deriveKek(cmd.newPassword, next.salt, next.kdf);
        if (!kek) {
            return std::unexpected(kek.error());
        }
        auto envelope =
            ports_.crypto.wrapKey(*kek, ctx->sealer->master(),
                                  domain::aad::envelope(next.formatVersion, next.salt, next.kdf));
        if (!envelope) {
            return std::unexpected(envelope.error());
        }
        next.envelope = std::move(*envelope);
        {
            auto uow = ports_.store.begin();
            if (!uow) {
                return std::unexpected(uow.error());
            }
            if (auto st = (*uow)->saveMeta(next); !st) {
                return st;
            }
            if (auto st = (*uow)->commit(); !st) {
                return st;
            }
        }
        ctx->session.setMeta(std::move(next));
        ctx->session.heartbeat(ports_.clock.monotonic(), true);
        return {};
    }

    Result<Lease> authorize(std::string_view token, TokenScope scope) override {
        if (token.empty()) {
            return fail(Error::Code::Locked, "Требуется вход в сейф");
        }
        std::shared_ptr<VaultSession> session;
        std::uint64_t epoch = 0;
        {
            std::scoped_lock state(stateMutex_);
            session = current_;
            epoch = epoch_;
        }
        if (!session || session->epoch() != epoch) {
            return fail(Error::Code::Locked, "Сейф заблокирован");
        }
        if (!session->hasToken(ports_.crypto, token, scope)) {
            return fail(Error::Code::Locked, "Сессия недействительна - войдите заново");
        }
        if (session->idleExpired(ports_.clock.monotonic())) {
            session.reset();
            lockIfCurrent(epoch);
            return fail(Error::Code::Locked, "Сейф заблокирован из-за неактивности");
        }
        if (!session->tryAcquire()) {
            return fail(Error::Code::Locked, "Сейф заблокирован");
        }
        return Lease(std::move(session));
    }

    Result<HeartbeatResult> heartbeat(const Lease& lease, bool active) override {
        VaultSession* session = lease.session();
        if (session == nullptr) {
            return fail(Error::Code::Internal, "Операция без аренды сессии");
        }
        if (session->cancelled()) {
            return fail(Error::Code::Locked, "Сейф заблокирован");
        }
        const auto now = ports_.clock.monotonic();
        session->heartbeat(now, active);
        return HeartbeatResult{session->idleRemaining(now).count()};
    }

    void tick() noexcept override {
        auto session = current();
        if (session && session->idleExpired(ports_.clock.monotonic())) {
            const auto epoch = session->epoch();
            session.reset();
            lockIfCurrent(epoch);
        }
    }

private:
    [[nodiscard]] std::shared_ptr<VaultSession> current() const {
        std::scoped_lock state(stateMutex_);
        return current_;
    }

    [[nodiscard]] Result<std::filesystem::path> resolvePath(std::string_view raw) const {
        const auto text = domain::detail::trim(raw);
        if (text.empty()) {
            return fail(Error::Code::InvalidArgument, "Укажите путь к файлу сейфа");
        }
        if (!domain::isValidUtf8(text) || text.find('\0') != std::string_view::npos) {
            return fail(Error::Code::InvalidArgument, "Некорректный путь к файлу сейфа");
        }
        auto path = domain::pathFromUtf8(text);
        if (path.is_relative()) {
            if (config_.defaultDirectory.empty()) {
                return fail(Error::Code::InvalidArgument, "Укажите полный путь к файлу сейфа");
            }
            path = config_.defaultDirectory / path;
        }
        path = path.lexically_normal();
        if (!path.has_filename()) {
            return fail(Error::Code::InvalidArgument, "Укажите имя файла сейфа");
        }
        return path;
    }

    [[nodiscard]] Result<KeyHandle> unwrapMaster(std::string_view password, const SafeMeta& meta) {
        auto kek = ports_.crypto.deriveKek(password, meta.salt, meta.kdf);
        if (!kek) {
            return std::unexpected(kek.error());
        }
        auto master = ports_.crypto.unwrapKey(
            *kek, meta.envelope, domain::aad::envelope(meta.formatVersion, meta.salt, meta.kdf));
        if (!master) {
            return fail(Error::Code::WrongPassword, "Неверный пароль");
        }
        return master;
    }

    [[nodiscard]] Result<UnlockResult> startSession(std::filesystem::path path, SafeMeta meta,
                                                    KeyHandle master) {
        SessionKeys keys;
        keys.master = master;
        for (auto [purpose, slot] : {std::pair{domain::KeyPurpose::Names, &keys.names},
                                     std::pair{domain::KeyPurpose::Content, &keys.content},
                                     std::pair{domain::KeyPurpose::Thumbnails, &keys.thumbnails}}) {
            auto subkey = ports_.crypto.deriveSubkey(master, purpose);
            if (!subkey) {
                return std::unexpected(subkey.error());
            }
            *slot = std::move(*subkey);
        }
        std::uint64_t epoch = 0;
        {
            std::scoped_lock state(stateMutex_);
            epoch = epoch_;
        }
        auto session = std::make_shared<VaultSession>(
            epoch, std::move(path), std::move(meta),
            std::make_shared<const Sealer>(ports_.crypto, std::move(keys)),
            IdlePolicy(config_.idleTimeout, config_.presenceTimeout, ports_.clock.monotonic()));
        auto tokens = issueTokens(*session);
        auto info = describe(*session);
        if (!info) {
            session->wipe();
            return std::unexpected(info.error());
        }
        {
            std::scoped_lock state(stateMutex_);
            current_ = session;
            lastPath_ = domain::pathToUtf8(session->path());
        }
        return UnlockResult{std::move(tokens), std::move(*info)};
    }

    [[nodiscard]] SessionTokens issueTokens(VaultSession& session) {
        SessionTokens tokens{ports_.crypto.generateToken(), ports_.crypto.generateToken()};
        session.addTokens(tokens);
        return tokens;
    }

    [[nodiscard]] Result<SafeInfo> describe(VaultSession& session) {
        std::uint64_t count = 0;
        {
            auto uow = ports_.store.begin();
            if (!uow) {
                return std::unexpected(uow.error());
            }
            auto n = (*uow)->entries().count();
            if (!n) {
                return std::unexpected(n.error());
            }
            count = *n;
        }
        return SafeInfo{domain::pathToUtf8(session.path()), count,
                        session.idleRemaining(ports_.clock.monotonic()).count()};
    }

    // Автоблокировка из tick/authorize: не заблокировать сессию, открытую
    // заново, пока мы шли к мьютексу.
    void lockIfCurrent(std::uint64_t epoch) noexcept {
        std::scoped_lock lifecycle(lifecycleMutex_);
        if (auto session = current(); session && session->epoch() == epoch) {
            session.reset();
            lockLocked();
        }
    }

    // lifecycleMutex_ уже взят.
    void lockLocked() noexcept {
        std::shared_ptr<VaultSession> session;
        {
            std::scoped_lock state(stateMutex_);
            session = std::move(current_);
            ++epoch_; // старые токены и аренды больше не проходят authorize
        }
        if (session) {
            session->beginClose();                                   // 2. отмена
            (void)session->waitForLeases(config_.leaseDrainTimeout); // 3. аренды
            session->wipe();                                         // 4. ключи, токены, имена
        }
        if (ports_.store.isOpen()) {
            (void)ports_.store.blobs().gcPending(); // 5. незавершенные куски
            ports_.store.close();
        }
    }

    Ports ports_;
    AppConfig config_;
    std::mutex lifecycleMutex_;
    std::mutex passwordMutex_;
    mutable std::mutex stateMutex_;
    std::shared_ptr<VaultSession> current_;
    std::uint64_t epoch_ = 1;
    std::optional<std::string> lastPath_;
};

} // namespace

std::shared_ptr<SafeService> makeSafeService(Ports ports, AppConfig config) {
    return std::make_shared<SafeServiceImpl>(ports, std::move(config));
}

} // namespace safebox::app
