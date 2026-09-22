// AAD: имя/мета = версия|entry_id|тег поля, кусок = версия|blob_id|idx|last.
// Так имена нельзя переставить между записями, а куски между блобами.
// В enc_meta еще лежат копии blob_id/thumb_blob_id, их сверяем с открытыми колонками.
//
// формат enc_meta v1 (little-endian):
//   u8 layout=1 | u8 kind | u64 size | i64 createdAt | i64 modifiedAt |
//   u8 flags (bit0 blob, bit1 thumb) | [i64 blobId] | [i64 thumbBlobId] |
//   u16 len + mime | u32 len + url
#include "sealing.hpp"

#include <algorithm>
#include <limits>

#include "safebox/domain/model/rules.hpp"
#include "safebox/domain/model/safe_format.hpp"

namespace safebox::app {

using domain::Bytes;
using domain::Error;
using domain::fail;
using domain::Result;

namespace {

constexpr std::uint8_t kMetaLayout = 1;
constexpr std::uint8_t kHasBlob = 0x01;
constexpr std::uint8_t kHasThumb = 0x02;

class Writer {
public:
    template <class T>
    void put(T value) {
        auto u = static_cast<std::uint64_t>(value);
        for (std::size_t i = 0; i < sizeof(T); ++i) {
            out.push_back(static_cast<std::byte>(u & 0xFFu));
            u >>= 8;
        }
    }
    void text(std::string_view s) {
        const auto b = domain::asBytes(s);
        out.insert(out.end(), b.begin(), b.end());
    }

    Bytes out;
};

class Reader {
public:
    explicit Reader(std::span<const std::byte> in) noexcept : in_(in) {}

    template <class T>
    [[nodiscard]] bool get(T& value) {
        if (in_.size() - pos_ < sizeof(T)) {
            return false;
        }
        std::uint64_t u = 0;
        for (std::size_t i = 0; i < sizeof(T); ++i) {
            u |= static_cast<std::uint64_t>(in_[pos_ + i]) << (8 * i);
        }
        pos_ += sizeof(T);
        value = static_cast<T>(u);
        return true;
    }
    [[nodiscard]] bool text(std::size_t len, std::string& value) {
        if (in_.size() - pos_ < len) {
            return false;
        }
        value.assign(domain::asChars(in_.subspan(pos_, len)));
        pos_ += len;
        return true;
    }
    [[nodiscard]] bool done() const noexcept { return pos_ == in_.size(); }

private:
    std::span<const std::byte> in_;
    std::size_t pos_ = 0;
};

[[nodiscard]] std::unexpected<Error> corrupted() {
    return fail(Error::Code::IntegrityError, "Данные записи повреждены или подменены");
}

} // namespace

Bytes encodeMeta(const domain::EntryMeta& meta) {
    Writer w;
    w.put(kMetaLayout);
    w.put(static_cast<std::uint8_t>(meta.kind));
    w.put(meta.size);
    w.put(meta.createdAt);
    w.put(meta.modifiedAt);
    w.put(static_cast<std::uint8_t>((meta.blobId ? kHasBlob : 0) |
                                    (meta.thumbBlobId ? kHasThumb : 0)));
    if (meta.blobId) {
        w.put(*meta.blobId);
    }
    if (meta.thumbBlobId) {
        w.put(*meta.thumbBlobId);
    }
    const auto mimeLen =
        std::min<std::size_t>(meta.mime.size(), std::numeric_limits<std::uint16_t>::max());
    w.put(static_cast<std::uint16_t>(mimeLen));
    w.text(std::string_view(meta.mime).substr(0, mimeLen));
    w.put(static_cast<std::uint32_t>(meta.url.size()));
    w.text(meta.url);
    return std::move(w.out);
}

Result<domain::EntryMeta> decodeMeta(std::span<const std::byte> bytes) {
    Reader r(bytes);
    std::uint8_t layout = 0;
    std::uint8_t kind = 0;
    std::uint8_t flags = 0;
    domain::EntryMeta meta;
    if (!r.get(layout) || layout != kMetaLayout || !r.get(kind) ||
        kind > static_cast<std::uint8_t>(domain::Kind::Link) || !r.get(meta.size) ||
        !r.get(meta.createdAt) || !r.get(meta.modifiedAt) || !r.get(flags) ||
        (flags & ~(kHasBlob | kHasThumb)) != 0) {
        return corrupted();
    }
    meta.kind = static_cast<domain::Kind>(kind);
    if ((flags & kHasBlob) != 0) {
        domain::BlobId id = 0;
        if (!r.get(id)) {
            return corrupted();
        }
        meta.blobId = id;
    }
    if ((flags & kHasThumb) != 0) {
        domain::BlobId id = 0;
        if (!r.get(id)) {
            return corrupted();
        }
        meta.thumbBlobId = id;
    }
    std::uint16_t mimeLen = 0;
    std::uint32_t urlLen = 0;
    if (!r.get(mimeLen) || !r.text(mimeLen, meta.mime) || !r.get(urlLen) ||
        !r.text(urlLen, meta.url) || !r.done()) {
        return corrupted();
    }
    return meta;
}

const domain::KeyHandle& Sealer::keyFor(domain::KeyPurpose purpose) const noexcept {
    switch (purpose) {
    case domain::KeyPurpose::Names:
        return keys_.names;
    case domain::KeyPurpose::Content:
        return keys_.content;
    case domain::KeyPurpose::Thumbnails:
        return keys_.thumbnails;
    }
    return keys_.content;
}

Result<Bytes> Sealer::sealName(domain::EntryId id, std::string_view name) const {
    return crypto_.seal(keys_.names, domain::asBytes(name),
                        domain::aad::field(id, domain::FieldTag::Name));
}

Result<Bytes> Sealer::sealMeta(domain::EntryId id, const domain::EntryMeta& meta) const {
    auto plain = encodeMeta(meta);
    auto sealed = crypto_.seal(keys_.names, plain, domain::aad::field(id, domain::FieldTag::Meta));
    domain::secureWipe(plain);
    return sealed;
}

Result<domain::Entry> Sealer::openEntry(const domain::EntryRecord& record) const {
    auto name = crypto_.open(keys_.names, record.encName,
                             domain::aad::field(record.id, domain::FieldTag::Name));
    if (!name) {
        return corrupted();
    }
    auto plainMeta = crypto_.open(keys_.names, record.encMeta,
                                  domain::aad::field(record.id, domain::FieldTag::Meta));
    if (!plainMeta) {
        domain::secureWipe(*name);
        return corrupted();
    }
    auto meta = decodeMeta(*plainMeta);
    domain::secureWipe(*plainMeta);
    if (!meta || meta->blobId != record.blobId || meta->thumbBlobId != record.thumbBlobId ||
        (meta->kind == domain::Kind::Folder) != record.isFolder) {
        domain::secureWipe(*name);
        return corrupted();
    }
    domain::Entry entry;
    entry.id = record.id;
    entry.parentId = record.parentId;
    entry.name.assign(domain::asChars(*name));
    entry.meta = std::move(*meta);
    domain::secureWipe(*name);
    return entry;
}

Result<Bytes> Sealer::sealChunk(domain::KeyPurpose purpose, domain::BlobId blob,
                                std::uint32_t index, bool last,
                                std::span<const std::byte> plain) const {
    return crypto_.seal(keyFor(purpose), plain, domain::aad::chunk(blob, index, last));
}

Result<Bytes> Sealer::openChunk(domain::KeyPurpose purpose, domain::BlobId blob,
                                std::uint32_t index, bool last,
                                std::span<const std::byte> sealed) const {
    auto plain = crypto_.open(keyFor(purpose), sealed, domain::aad::chunk(blob, index, last));
    if (!plain) {
        return fail(Error::Code::IntegrityError, "Данные файла повреждены или подменены");
    }
    return plain;
}

} // namespace safebox::app
