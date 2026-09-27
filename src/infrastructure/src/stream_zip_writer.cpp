// Свой потоковый zip: miniz пишет с seek-ами, а нам надо строго последовательно.
// От miniz берем только mz_crc32.
// STORE, data descriptor, zip64 при >= 4 ГБ, имена в UTF-8
#include <miniz.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "safebox/domain/ports/archive.hpp"
#include "safebox/infra/factories.hpp"

namespace safebox::infra {
namespace {

using domain::Bytes;
using domain::ByteSink;
using domain::ByteSource;
using domain::Error;
using domain::fail;
using domain::Status;

constexpr std::uint32_t kMax32 = 0xFFFFFFFFu;
constexpr std::uint16_t kMax16 = 0xFFFFu;
constexpr std::uint16_t kFlagDescriptor = 0x0008;
constexpr std::uint16_t kFlagUtf8 = 0x0800;
constexpr std::uint16_t kVersion20 = 20;
constexpr std::uint16_t kVersion45 = 45;
constexpr std::size_t kCopyBuffer = 1u << 20;

struct DosTime {
    std::uint16_t time = 0;
    std::uint16_t date = (1 << 5) | 1; // 1980-01-01
};

[[nodiscard]] DosTime toDos(std::int64_t mtimeMs) {
    const std::time_t t = static_cast<std::time_t>(mtimeMs / 1000);
    std::tm local{};
#if defined(_WIN32)
    if (localtime_s(&local, &t) != 0) {
        return {};
    }
#else
    if (localtime_r(&t, &local) == nullptr) {
        return {};
    }
#endif
    if (local.tm_year < 80) {
        return {};
    }
    DosTime dos;
    dos.date = static_cast<std::uint16_t>(((local.tm_year - 80) << 9) | ((local.tm_mon + 1) << 5) |
                                          local.tm_mday);
    dos.time = static_cast<std::uint16_t>((local.tm_hour << 11) | (local.tm_min << 5) |
                                          (local.tm_sec / 2));
    return dos;
}

[[nodiscard]] std::uint32_t unixSeconds32(std::int64_t mtimeMs) {
    const auto s = mtimeMs / 1000;
    return static_cast<std::uint32_t>(
        std::clamp<std::int64_t>(s, 0, std::numeric_limits<std::int32_t>::max()));
}

class Buffer {
public:
    Buffer& u16(std::uint32_t v) {
        put(v, 2);
        return *this;
    }
    Buffer& u32(std::uint64_t v) {
        put(v, 4);
        return *this;
    }
    Buffer& u64(std::uint64_t v) {
        put(v, 8);
        return *this;
    }
    Buffer& u8(std::uint32_t v) {
        put(v, 1);
        return *this;
    }
    Buffer& text(std::string_view s) {
        const auto b = domain::asBytes(s);
        bytes.insert(bytes.end(), b.begin(), b.end());
        return *this;
    }
    Buffer& raw(const Bytes& b) {
        bytes.insert(bytes.end(), b.begin(), b.end());
        return *this;
    }

    Bytes bytes;

private:
    void put(std::uint64_t v, int n) {
        for (int i = 0; i < n; ++i) {
            bytes.push_back(static_cast<std::byte>(v & 0xFF));
            v >>= 8;
        }
    }
};

struct CentralEntry {
    std::string name;
    bool directory = false;
    bool zip64Size = false;
    std::uint32_t crc = 0;
    std::uint64_t size = 0;
    std::uint64_t offset = 0;
    DosTime dos;
    std::uint32_t mtime = 0;
};

class StreamZip final : public domain::ZipStream {
public:
    StreamZip(ByteSink& out, bool forceZip64) : out_(out), forceZip64_(forceZip64) {}

    Status addDirectory(std::string_view path, std::int64_t mtimeMs) override {
        std::string name(path);
        if (name.empty() || name.back() != '/') {
            name.push_back('/');
        }
        if (auto st = checkName(name); !st) {
            return st;
        }
        CentralEntry e{std::move(name),       true, false, 0, 0, offset_, toDos(mtimeMs),
                       unixSeconds32(mtimeMs)};
        Buffer h;
        h.u32(0x04034b50).u16(kVersion20).u16(kFlagUtf8).u16(0).u16(e.dos.time).u16(e.dos.date);
        h.u32(0).u32(0).u32(0).u16(static_cast<std::uint32_t>(e.name.size())).u16(9);
        h.text(e.name);
        timestampExtra(h, e.mtime);
        if (auto st = emit(h.bytes); !st) {
            return st;
        }
        entries_.push_back(std::move(e));
        return {};
    }

    Status addFile(std::string_view path, std::uint64_t size, std::int64_t mtimeMs,
                   ByteSource& source) override {
        std::string name(path);
        if (auto st = checkName(name); !st) {
            return st;
        }
        CentralEntry e{std::move(name), false,          forceZip64_ || size >= kMax32, 0, size,
                       offset_,         toDos(mtimeMs), unixSeconds32(mtimeMs)};

        Buffer h;
        h.u32(0x04034b50)
            .u16(e.zip64Size ? kVersion45 : kVersion20)
            .u16(kFlagDescriptor | kFlagUtf8)
            .u16(0) // STORE
            .u16(e.dos.time)
            .u16(e.dos.date)
            .u32(0) // CRC и размеры - в data descriptor
            .u32(e.zip64Size ? kMax32 : 0)
            .u32(e.zip64Size ? kMax32 : 0)
            .u16(static_cast<std::uint32_t>(e.name.size()))
            .u16(e.zip64Size ? 20 + 9 : 9);
        h.text(e.name);
        if (e.zip64Size) {
            h.u16(0x0001).u16(16).u64(0).u64(0);
        }
        timestampExtra(h, e.mtime);
        if (auto st = emit(h.bytes); !st) {
            return st;
        }

        mz_ulong crc = MZ_CRC32_INIT;
        std::uint64_t copied = 0;
        Bytes buffer(kCopyBuffer);
        for (;;) {
            auto n = source.read(buffer);
            if (!n) {
                return std::unexpected(n.error());
            }
            if (*n == 0) {
                break;
            }
            copied += *n;
            if (copied > size) {
                return fail(Error::Code::IntegrityError, "Размер файла не совпадает с заявленным");
            }
            crc = mz_crc32(crc, reinterpret_cast<const unsigned char*>(buffer.data()), *n);
            if (auto st = emit(std::span<const std::byte>(buffer.data(), *n)); !st) {
                return st;
            }
        }
        if (copied != size) {
            return fail(Error::Code::IntegrityError, "Размер файла не совпадает с заявленным");
        }
        e.crc = static_cast<std::uint32_t>(crc);

        Buffer d;
        d.u32(0x08074b50).u32(e.crc);
        if (e.zip64Size) {
            d.u64(size).u64(size);
        } else {
            d.u32(size).u32(size);
        }
        if (auto st = emit(d.bytes); !st) {
            return st;
        }
        entries_.push_back(std::move(e));
        return {};
    }

    Status finish() override {
        const std::uint64_t cdOffset = offset_;
        for (const auto& e : entries_) {
            const bool bigOffset = forceZip64_ || e.offset >= kMax32;
            // Размеры тоже через zip64, когда смещение 64-битное: часть ридеров
            // (miniz и др.) проверяет "смещение + размер", если размер не
            // 0xFFFFFFFF, и берет для этого заглушку смещения из заголовка.
            const bool bigSize = e.zip64Size || bigOffset;
            Buffer extra;
            if (bigSize || bigOffset) {
                const std::uint32_t len = (bigSize ? 16 : 0) + (bigOffset ? 8 : 0);
                extra.u16(0x0001).u16(len);
                if (bigSize) {
                    extra.u64(e.size).u64(e.size);
                }
                if (bigOffset) {
                    extra.u64(e.offset);
                }
            }
            extra.u16(0x5455).u16(5).u8(1).u32(e.mtime);

            const bool v45 = bigSize || bigOffset;
            Buffer c;
            c.u32(0x02014b50)
                .u16(v45 ? kVersion45 : kVersion20) // made by: MS-DOS, spec 2.0/4.5
                .u16(v45 ? kVersion45 : kVersion20)
                .u16(e.directory ? kFlagUtf8 : (kFlagDescriptor | kFlagUtf8))
                .u16(0)
                .u16(e.dos.time)
                .u16(e.dos.date)
                .u32(e.crc)
                .u32(bigSize ? kMax32 : e.size)
                .u32(bigSize ? kMax32 : e.size)
                .u16(static_cast<std::uint32_t>(e.name.size()))
                .u16(static_cast<std::uint32_t>(extra.bytes.size()))
                .u16(0)                         // комментарий
                .u16(0)                         // диск
                .u16(0)                         // внутренние атрибуты
                .u32(e.directory ? 0x10 : 0x20) // FILE_ATTRIBUTE_DIRECTORY / ARCHIVE
                .u32(bigOffset ? kMax32 : e.offset);
            c.text(e.name).raw(extra.bytes);
            if (auto st = emit(c.bytes); !st) {
                return st;
            }
        }
        const std::uint64_t cdSize = offset_ - cdOffset;
        const std::uint64_t count = entries_.size();
        const bool zip64 = forceZip64_ || count >= kMax16 || cdSize >= kMax32 || cdOffset >= kMax32;

        Buffer end;
        if (zip64) {
            const std::uint64_t recordOffset = offset_;
            end.u32(0x06064b50).u64(44).u16(kVersion45).u16(kVersion45).u32(0).u32(0);
            end.u64(count).u64(count).u64(cdSize).u64(cdOffset);
            end.u32(0x07064b50).u32(0).u64(recordOffset).u32(1);
        }
        end.u32(0x06054b50)
            .u16(0)
            .u16(0)
            .u16(zip64 ? kMax16 : static_cast<std::uint32_t>(count))
            .u16(zip64 ? kMax16 : static_cast<std::uint32_t>(count))
            .u32(zip64 ? kMax32 : cdSize)
            .u32(zip64 ? kMax32 : cdOffset)
            .u16(0);
        return emit(end.bytes);
    }

private:
    static Status checkName(const std::string& name) {
        if (name.empty() || name.size() > kMax16 || name.front() == '/' ||
            name.find('\\') != std::string::npos) {
            return fail(Error::Code::InvalidArgument, "Недопустимый путь в архиве");
        }
        return {};
    }

    static void timestampExtra(Buffer& b, std::uint32_t mtime) {
        b.u16(0x5455).u16(5).u8(1).u32(mtime);
    }

    Status emit(std::span<const std::byte> data) {
        if (data.empty()) {
            return {};
        }
        if (auto st = out_.write(data); !st) {
            return st;
        }
        offset_ += data.size();
        return {};
    }

    ByteSink& out_;
    bool forceZip64_ = false;
    std::uint64_t offset_ = 0;
    std::vector<CentralEntry> entries_;
};

class StreamZipWriter final : public domain::ZipWriter {
public:
    explicit StreamZipWriter(ZipWriterOptions options) : options_(options) {}

    std::unique_ptr<domain::ZipStream> start(ByteSink& out) override {
        return std::make_unique<StreamZip>(out, options_.forceZip64);
    }

private:
    ZipWriterOptions options_;
};

} // namespace

std::unique_ptr<domain::ZipWriter> makeStreamZipWriter(ZipWriterOptions options) {
    return std::make_unique<StreamZipWriter>(options);
}

} // namespace safebox::infra
