// Константы формата .safebox, подробно формат описан в Linqtab, «Формат файла *.safebox»
// AAD (все little-endian):
//   конверт       = версия u32 | соль 16 | ops u64 | mem u64
//   кусок         = версия u32 | blob_id i64 | idx u32 | last u8
//   поле записи   = версия u32 | entry_id i64 | тег u8 (name=1, meta=2)
//   имя категории = версия u32 | category_id i64 | 3
//   имя тега      = версия u32 | tag_id i64 | 4 | category_id i64
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "safebox/domain/io.hpp"
#include "safebox/domain/model/entry.hpp"

namespace safebox::domain {

inline constexpr std::uint32_t kFormatVersion = 2;
inline constexpr std::int32_t kApplicationId = 0x53424F58; // 'S' 'B' 'O' 'X'
inline constexpr std::string_view kSafeExtension = ".safebox";

inline constexpr std::uint32_t kDefaultChunkSize = 8u * 1024u * 1024u;
// Границы допустимого размера куска в чужом/поврежденном файле.
inline constexpr std::uint32_t kMinChunkSize = 1024u;
inline constexpr std::uint32_t kMaxChunkSize = 64u * 1024u * 1024u;

inline constexpr std::size_t kSaltSize = 16;  // Argon2id salt
inline constexpr std::size_t kKeySize = 32;   // мастер-ключ, KEK, подключи
inline constexpr std::size_t kNonceSize = 24; // XChaCha20-Poly1305 nonce (префикс шифртекста)
inline constexpr std::size_t kTagSize = 16;   // Poly1305 tag
inline constexpr std::size_t kSealOverhead = kNonceSize + kTagSize;

struct KdfParams {
    std::uint64_t ops = 0; // opslimit (число проходов Argon2id)
    std::uint64_t mem = 0; // memlimit, байт

    friend constexpr bool operator==(const KdfParams&, const KdfParams&) = default;
};

// ~ libsodium MODERATE: ~0,5-1 с и 256 МиБ на вход - защита от перебора
// при неограниченных попытках (ТЗ §2).
inline constexpr KdfParams kDefaultKdf{3, 256ull * 1024 * 1024};
// Минимум Argon2id в libsodium - только для тестов.
inline constexpr KdfParams kMinimalKdf{1, 8ull * 1024};
// Верхние границы для чтения чужого файла: не дать файлу заказать 64 ГБ памяти.
inline constexpr std::uint64_t kMaxKdfOps = 64;
inline constexpr std::uint64_t kMaxKdfMem = 4ull * 1024 * 1024 * 1024;

// Подключи мастер-ключа (crypto_kdf_derive_from_key: subkey_id = значение).
enum class KeyPurpose : std::uint64_t {
    Names = 1,      // enc_name + enc_meta записей, имена категорий и тегов
    Content = 2,    // куски содержимого
    Thumbnails = 3, // куски миниатюр
};
// Контекст crypto_kdf - ровно 8 байт.
inline constexpr std::array<char, 8> kSubkeyContext{'S', 'B', 'O', 'X', 'K', 'E', 'Y', 'S'};

enum class FieldTag : std::uint8_t {
    Name = 1,
    Meta = 2,
    CategoryName = 3,
    TagName = 4,
};

namespace aad {

namespace detail {
template <class T>
inline void putLe(Bytes& out, T value) {
    auto u = static_cast<std::uint64_t>(value);
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        out.push_back(static_cast<std::byte>(u & 0xFFu));
        u >>= 8;
    }
}
} // namespace detail

[[nodiscard]] inline Bytes envelope(std::uint32_t version, std::span<const std::byte> salt,
                                    const KdfParams& kdf) {
    Bytes out;
    out.reserve(4 + salt.size() + 16);
    detail::putLe(out, version);
    out.insert(out.end(), salt.begin(), salt.end());
    detail::putLe(out, kdf.ops);
    detail::putLe(out, kdf.mem);
    return out;
}

[[nodiscard]] inline Bytes chunk(BlobId blob, std::uint32_t index, bool last) {
    Bytes out;
    out.reserve(17);
    detail::putLe(out, kFormatVersion);
    detail::putLe(out, blob);
    detail::putLe(out, index);
    out.push_back(last ? std::byte{1} : std::byte{0});
    return out;
}

[[nodiscard]] inline Bytes field(EntryId entry, FieldTag tag) {
    Bytes out;
    out.reserve(13);
    detail::putLe(out, kFormatVersion);
    detail::putLe(out, entry);
    out.push_back(static_cast<std::byte>(tag));
    return out;
}

// Открытый category_id входит в AAD: подмена категории у тега -> ошибка при открытии.
[[nodiscard]] inline Bytes categoryName(CategoryId category) {
    Bytes out;
    out.reserve(13);
    detail::putLe(out, kFormatVersion);
    detail::putLe(out, category);
    out.push_back(static_cast<std::byte>(FieldTag::CategoryName));
    return out;
}

[[nodiscard]] inline Bytes tagName(TagId tag, CategoryId category) {
    Bytes out;
    out.reserve(21);
    detail::putLe(out, kFormatVersion);
    detail::putLe(out, tag);
    out.push_back(static_cast<std::byte>(FieldTag::TagName));
    detail::putLe(out, category);
    return out;
}

} // namespace aad

// Число кусков блоба размера size: пустой файл - один пустой кусок, чтобы
// "последний" кусок существовал всегда и обрезку было видно.
[[nodiscard]] constexpr std::uint32_t chunkCountFor(std::uint64_t size,
                                                    std::uint32_t chunkSize) noexcept {
    if (size == 0 || chunkSize == 0) {
        return 1;
    }
    return static_cast<std::uint32_t>((size + chunkSize - 1) / chunkSize);
}

} // namespace safebox::domain
