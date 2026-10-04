// Шифрование полей записи, имен категорий и тегов, кусков.
// Sealer после создания не меняется, так что его можно шарить между потоками
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "safebox/domain/io.hpp"
#include "safebox/domain/model/entry.hpp"
#include "safebox/domain/ports/crypto.hpp"
#include "safebox/domain/ports/storage.hpp"

namespace safebox::app {

struct SessionKeys {
    domain::KeyHandle master;
    domain::KeyHandle names;
    domain::KeyHandle content;
    domain::KeyHandle thumbnails;
};

[[nodiscard]] domain::Bytes encodeMeta(const domain::EntryMeta& meta);
[[nodiscard]] domain::Result<domain::EntryMeta> decodeMeta(std::span<const std::byte> bytes);

class Sealer {
public:
    Sealer(domain::CryptoSuite& crypto, SessionKeys keys) noexcept
        : crypto_(crypto), keys_(std::move(keys)) {}

    [[nodiscard]] const domain::KeyHandle& master() const noexcept { return keys_.master; }

    [[nodiscard]] domain::Result<domain::Bytes> sealName(domain::EntryId id,
                                                         std::string_view name) const;
    [[nodiscard]] domain::Result<domain::Bytes> sealMeta(domain::EntryId id,
                                                         const domain::EntryMeta& meta) const;
    // Имя + мета + сверки (blob_id, thumb_blob_id, is_folder) -> Entry.
    [[nodiscard]] domain::Result<domain::Entry> openEntry(const domain::EntryRecord& record) const;

    [[nodiscard]] domain::Result<domain::Bytes> sealCategoryName(domain::CategoryId id,
                                                                 std::string_view name) const;
    [[nodiscard]] domain::Result<domain::Bytes> sealCategoryNameEn(domain::CategoryId id,
                                                                   std::string_view nameEn) const;
    [[nodiscard]] domain::Result<domain::TagCategory>
    openCategory(const domain::TagCategoryRecord& record) const;
    // category_id входит в AAD: перенос тега в другую категорию = перезапечатать имя.
    [[nodiscard]] domain::Result<domain::Bytes>
    sealTagName(domain::TagId id, domain::CategoryId category, std::string_view name) const;
    [[nodiscard]] domain::Result<domain::Bytes>
    sealTagNameEn(domain::TagId id, domain::CategoryId category, std::string_view nameEn) const;
    [[nodiscard]] domain::Result<domain::Tag> openTag(const domain::TagRecord& record) const;

    [[nodiscard]] domain::Result<domain::Bytes> sealChunk(domain::KeyPurpose purpose,
                                                          domain::BlobId blob, std::uint32_t index,
                                                          bool last,
                                                          std::span<const std::byte> plain) const;
    [[nodiscard]] domain::Result<domain::Bytes> openChunk(domain::KeyPurpose purpose,
                                                          domain::BlobId blob, std::uint32_t index,
                                                          bool last,
                                                          std::span<const std::byte> sealed) const;

private:
    [[nodiscard]] const domain::KeyHandle& keyFor(domain::KeyPurpose purpose) const noexcept;

    domain::CryptoSuite& crypto_;
    SessionKeys keys_;
};

} // namespace safebox::app
