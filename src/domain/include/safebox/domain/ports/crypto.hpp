// Порт криптографии, реализация на libsodium в infra.
// Байты ключей живут только внутри адаптера (sodium_malloc), наружу отдаем KeyHandle.
// seal: XChaCha20-Poly1305, nonce пишется в начало шифртекста
#pragma once

#include <memory>
#include <span>
#include <string>
#include <string_view>

#include "safebox/domain/io.hpp"
#include "safebox/domain/model/error.hpp"
#include "safebox/domain/model/safe_format.hpp"

namespace safebox::domain {

class SecretKey {
public:
    virtual ~SecretKey() = default;
    SecretKey(const SecretKey&) = delete;
    SecretKey& operator=(const SecretKey&) = delete;

protected:
    SecretKey() = default;
};

using KeyHandle = std::shared_ptr<const SecretKey>;

class CryptoSuite {
public:
    virtual ~CryptoSuite() = default;

    [[nodiscard]] virtual Result<KeyHandle> deriveKek(std::string_view password,
                                                      std::span<const std::byte> salt,
                                                      const KdfParams& params) = 0;

    [[nodiscard]] virtual Result<KeyHandle> generateKey() = 0;

    [[nodiscard]] virtual Result<KeyHandle> deriveSubkey(const KeyHandle& master,
                                                         KeyPurpose purpose) = 0;

    // nonce(24) | ciphertext | tag(16)
    [[nodiscard]] virtual Result<Bytes> seal(const KeyHandle& key,
                                             std::span<const std::byte> plaintext,
                                             std::span<const std::byte> aad) = 0;

    // Подмена байтов, чужой ключ или чужой AAD -> IntegrityError.
    [[nodiscard]] virtual Result<Bytes> open(const KeyHandle& key,
                                             std::span<const std::byte> sealed,
                                             std::span<const std::byte> aad) = 0;

    [[nodiscard]] virtual Result<Bytes> wrapKey(const KeyHandle& kek, const KeyHandle& key,
                                                std::span<const std::byte> aad) = 0;

    // Не вскрылся (неверный пароль => чужой KEK, или подмена) -> IntegrityError.
    [[nodiscard]] virtual Result<KeyHandle> unwrapKey(const KeyHandle& kek,
                                                      std::span<const std::byte> envelope,
                                                      std::span<const std::byte> aad) = 0;

    [[nodiscard]] virtual Bytes generateSalt() = 0;

    // Случайный токен сессии (256 бит), пригодный для заголовка и cookie.
    [[nodiscard]] virtual std::string generateToken() = 0;

    [[nodiscard]] virtual bool equalConstantTime(std::string_view a, std::string_view b) = 0;
};

} // namespace safebox::domain
