// CryptoSuite на libsodium. Ключи в sodium_malloc, при освобождении sodium_free (он же зануляет)
#include <sodium.h>

#include <array>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>

#include "safebox/domain/ports/crypto.hpp"
#include "safebox/infra/factories.hpp"

namespace safebox::infra {
namespace {

using domain::Bytes;
using domain::Error;
using domain::fail;
using domain::KdfParams;
using domain::KeyHandle;
using domain::KeyPurpose;
using domain::Result;

static_assert(domain::kNonceSize == crypto_aead_xchacha20poly1305_ietf_NPUBBYTES);
static_assert(domain::kTagSize == crypto_aead_xchacha20poly1305_ietf_ABYTES);
static_assert(domain::kKeySize == crypto_aead_xchacha20poly1305_ietf_KEYBYTES);
static_assert(domain::kKeySize == crypto_kdf_KEYBYTES);
static_assert(domain::kSaltSize == crypto_pwhash_SALTBYTES);
static_assert(domain::kSubkeyContext.size() == crypto_kdf_CONTEXTBYTES);

class SodiumKey final : public domain::SecretKey {
public:
    SodiumKey() : bytes_(static_cast<unsigned char*>(sodium_malloc(domain::kKeySize))) {
        if (bytes_ == nullptr) {
            throw std::bad_alloc();
        }
    }
    ~SodiumKey() override { sodium_free(bytes_); }

    [[nodiscard]] unsigned char* data() const noexcept { return bytes_; }

private:
    unsigned char* bytes_;
};

[[nodiscard]] const unsigned char* u8(std::span<const std::byte> s) noexcept {
    return reinterpret_cast<const unsigned char*>(s.data());
}

[[nodiscard]] Result<const SodiumKey*> keyOf(const KeyHandle& handle) {
    const auto* key = dynamic_cast<const SodiumKey*>(handle.get());
    if (key == nullptr) {
        return fail(Error::Code::Internal, "Ключ не принадлежит адаптеру SodiumCrypto");
    }
    return key;
}

class SodiumCrypto final : public domain::CryptoSuite {
public:
    Result<KeyHandle> deriveKek(std::string_view password, std::span<const std::byte> salt,
                                const KdfParams& params) override {
        if (salt.size() != crypto_pwhash_SALTBYTES) {
            return fail(Error::Code::IntegrityError, "Повреждены параметры ключа (соль)");
        }
        if (params.ops < crypto_pwhash_argon2id_OPSLIMIT_MIN || params.ops > domain::kMaxKdfOps ||
            params.mem < crypto_pwhash_argon2id_MEMLIMIT_MIN || params.mem > domain::kMaxKdfMem) {
            return fail(Error::Code::IntegrityError, "Повреждены параметры ключа (Argon2id)");
        }
        auto key = std::make_shared<SodiumKey>();
        if (crypto_pwhash(key->data(), domain::kKeySize, password.data(), password.size(), u8(salt),
                          params.ops, static_cast<std::size_t>(params.mem),
                          crypto_pwhash_ALG_ARGON2ID13) != 0) {
            return fail(Error::Code::Internal,
                        "Недостаточно памяти для вычисления ключа (Argon2id)");
        }
        return KeyHandle(std::move(key));
    }

    Result<KeyHandle> generateKey() override {
        auto key = std::make_shared<SodiumKey>();
        crypto_aead_xchacha20poly1305_ietf_keygen(key->data());
        return KeyHandle(std::move(key));
    }

    Result<KeyHandle> deriveSubkey(const KeyHandle& master, KeyPurpose purpose) override {
        auto parent = keyOf(master);
        if (!parent) {
            return std::unexpected(parent.error());
        }
        auto key = std::make_shared<SodiumKey>();
        if (crypto_kdf_derive_from_key(key->data(), domain::kKeySize,
                                       static_cast<std::uint64_t>(purpose),
                                       domain::kSubkeyContext.data(), (*parent)->data()) != 0) {
            return fail(Error::Code::Internal, "Не удалось вывести подключ");
        }
        return KeyHandle(std::move(key));
    }

    Result<Bytes> seal(const KeyHandle& key, std::span<const std::byte> plaintext,
                       std::span<const std::byte> aad) override {
        auto k = keyOf(key);
        if (!k) {
            return std::unexpected(k.error());
        }
        return sealRaw((*k)->data(), plaintext, aad);
    }

    Result<Bytes> open(const KeyHandle& key, std::span<const std::byte> sealed,
                       std::span<const std::byte> aad) override {
        auto k = keyOf(key);
        if (!k) {
            return std::unexpected(k.error());
        }
        if (sealed.size() < domain::kSealOverhead) {
            return integrityError();
        }
        Bytes out(sealed.size() - domain::kSealOverhead);
        unsigned char dummy = 0;
        auto* dst = out.empty() ? &dummy : reinterpret_cast<unsigned char*>(out.data());
        unsigned long long len = 0;
        if (crypto_aead_xchacha20poly1305_ietf_decrypt(dst, &len, nullptr,
                                                       u8(sealed) + domain::kNonceSize,
                                                       sealed.size() - domain::kNonceSize, u8(aad),
                                                       aad.size(), u8(sealed), (*k)->data()) != 0) {
            return integrityError();
        }
        return out;
    }

    Result<Bytes> wrapKey(const KeyHandle& kek, const KeyHandle& key,
                          std::span<const std::byte> aad) override {
        auto wrapping = keyOf(kek);
        auto wrapped = keyOf(key);
        if (!wrapping || !wrapped) {
            return fail(Error::Code::Internal, "Ключ не принадлежит адаптеру SodiumCrypto");
        }
        const auto raw = std::as_bytes(std::span((*wrapped)->data(), domain::kKeySize));
        return sealRaw((*wrapping)->data(), raw, aad);
    }

    Result<KeyHandle> unwrapKey(const KeyHandle& kek, std::span<const std::byte> envelope,
                                std::span<const std::byte> aad) override {
        auto wrapping = keyOf(kek);
        if (!wrapping) {
            return std::unexpected(wrapping.error());
        }
        if (envelope.size() != domain::kSealOverhead + domain::kKeySize) {
            return integrityError();
        }
        auto key = std::make_shared<SodiumKey>();
        unsigned long long len = 0;
        if (crypto_aead_xchacha20poly1305_ietf_decrypt(
                key->data(), &len, nullptr, u8(envelope) + domain::kNonceSize,
                envelope.size() - domain::kNonceSize, u8(aad), aad.size(), u8(envelope),
                (*wrapping)->data()) != 0 ||
            len != domain::kKeySize) {
            return integrityError();
        }
        return KeyHandle(std::move(key));
    }

    Bytes generateSalt() override {
        Bytes salt(domain::kSaltSize);
        randombytes_buf(salt.data(), salt.size());
        return salt;
    }

    std::string generateToken() override {
        std::array<unsigned char, 32> raw{};
        randombytes_buf(raw.data(), raw.size());
        constexpr int variant = sodium_base64_VARIANT_URLSAFE_NO_PADDING;
        std::array<char, sodium_base64_ENCODED_LEN(32, variant)> text{};
        sodium_bin2base64(text.data(), text.size(), raw.data(), raw.size(), variant);
        sodium_memzero(raw.data(), raw.size());
        return std::string(text.data());
    }

    bool equalConstantTime(std::string_view a, std::string_view b) override {
        // длина токена не секрет (фиксирована), сравнивается только содержимое
        if (a.size() != b.size()) {
            return false;
        }
        return a.empty() || sodium_memcmp(a.data(), b.data(), a.size()) == 0;
    }

private:
    static std::unexpected<Error> integrityError() {
        return fail(Error::Code::IntegrityError, "Данные сейфа повреждены или подменены");
    }

    static Result<Bytes> sealRaw(const unsigned char* key, std::span<const std::byte> plaintext,
                                 std::span<const std::byte> aad) {
        Bytes out(domain::kSealOverhead + plaintext.size());
        auto* nonce = reinterpret_cast<unsigned char*>(out.data());
        randombytes_buf(nonce, domain::kNonceSize);
        unsigned long long len = 0;
        if (crypto_aead_xchacha20poly1305_ietf_encrypt(nonce + domain::kNonceSize, &len,
                                                       u8(plaintext), plaintext.size(), u8(aad),
                                                       aad.size(), nullptr, nonce, key) != 0) {
            return fail(Error::Code::Internal, "Ошибка шифрования");
        }
        return out;
    }
};

} // namespace

std::unique_ptr<domain::CryptoSuite> makeSodiumCrypto() {
    if (sodium_init() < 0) {
        throw std::runtime_error("libsodium: sodium_init() не удался");
    }
    return std::make_unique<SodiumCrypto>();
}

} // namespace safebox::infra
