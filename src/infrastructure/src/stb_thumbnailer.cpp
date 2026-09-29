// Миниатюры через stb (webp - через libwebp, stb его не декодирует):
// decode -> resize -> поворот по EXIF -> jpeg q=82.
// Прозрачность заливаем темным фоном. Слишком большие картинки (> kMaxPixels) не декодим
#if defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wpedantic"
#endif
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_ONLY_BMP
#define STBI_ONLY_GIF
#define STBI_NO_STDIO
#define STBI_MAX_DIMENSIONS (1 << 15)
#include <stb_image.h>
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include <stb_image_resize2.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include <stb_image_write.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <webp/decode.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

#include "safebox/domain/ports/media.hpp"
#include "safebox/infra/factories.hpp"

namespace safebox::infra {
namespace {

using domain::Bytes;
using domain::Result;

constexpr std::uint64_t kMaxPixels = 100'000'000; // ~400 МБ RGBA при декодировании
constexpr int kJpegQuality = 82;
constexpr unsigned char kBackground[3] = {32, 32, 36};

[[nodiscard]] std::uint8_t at(std::span<const std::byte> d, std::size_t i) {
    return static_cast<std::uint8_t>(d[i]);
}

// EXIF Orientation (1..8) из APP1 JPEG; 1 - если нет или не разобрать.
[[nodiscard]] int jpegOrientation(std::span<const std::byte> d) {
    if (d.size() < 4 || at(d, 0) != 0xFF || at(d, 1) != 0xD8) {
        return 1;
    }
    std::size_t i = 2;
    while (i + 4 <= d.size()) {
        if (at(d, i) != 0xFF) {
            return 1;
        }
        const auto marker = at(d, i + 1);
        if (marker == 0xFF) {
            ++i;
            continue;
        }
        if (marker == 0xD9 || marker == 0xDA) {
            return 1; // EOI / начало скана - EXIF уже не встретится
        }
        const std::size_t len = (std::size_t{at(d, i + 2)} << 8) | at(d, i + 3);
        if (len < 2 || i + 2 + len > d.size()) {
            return 1;
        }
        if (marker == 0xE1 && len >= 8 + 6 && at(d, i + 4) == 'E' && at(d, i + 5) == 'x' &&
            at(d, i + 6) == 'i' && at(d, i + 7) == 'f' && at(d, i + 8) == 0 && at(d, i + 9) == 0) {
            const auto tiff = d.subspan(i + 10, len - 8);
            if (tiff.size() < 8) {
                return 1;
            }
            const bool le = at(tiff, 0) == 'I' && at(tiff, 1) == 'I';
            const bool be = at(tiff, 0) == 'M' && at(tiff, 1) == 'M';
            if (!le && !be) {
                return 1;
            }
            auto u16 = [&](std::size_t p) -> std::uint32_t {
                return le ? (std::uint32_t{at(tiff, p)} | (std::uint32_t{at(tiff, p + 1)} << 8))
                          : ((std::uint32_t{at(tiff, p)} << 8) | std::uint32_t{at(tiff, p + 1)});
            };
            auto u32 = [&](std::size_t p) -> std::uint32_t {
                return le ? (u16(p) | (u16(p + 2) << 16)) : ((u16(p) << 16) | u16(p + 2));
            };
            const std::size_t ifd = u32(4);
            if (ifd + 2 > tiff.size()) {
                return 1;
            }
            const std::size_t count = u16(ifd);
            for (std::size_t k = 0; k < count; ++k) {
                const std::size_t entry = ifd + 2 + k * 12;
                if (entry + 12 > tiff.size()) {
                    return 1;
                }
                if (u16(entry) == 0x0112 && u16(entry + 2) == 3) { // Orientation, SHORT
                    const auto value = static_cast<int>(u16(entry + 8));
                    return value >= 1 && value <= 8 ? value : 1;
                }
            }
            return 1;
        }
        i += 2 + len;
    }
    return 1;
}

struct Image {
    int width = 0;
    int height = 0;
    std::vector<unsigned char> rgb; // width * height * 3
};

// Отображаемый пиксель (dx, dy) -> исходный (sx, sy) для ориентаций EXIF.
[[nodiscard]] Image orient(const Image& src, int orientation) {
    if (orientation == 1) {
        return src;
    }
    const bool swap = orientation >= 5;
    Image out;
    out.width = swap ? src.height : src.width;
    out.height = swap ? src.width : src.height;
    out.rgb.resize(src.rgb.size());
    const int w = src.width;
    const int h = src.height;
    for (int dy = 0; dy < out.height; ++dy) {
        for (int dx = 0; dx < out.width; ++dx) {
            int sx = dx;
            int sy = dy;
            switch (orientation) {
            case 2:
                sx = w - 1 - dx;
                sy = dy;
                break;
            case 3:
                sx = w - 1 - dx;
                sy = h - 1 - dy;
                break;
            case 4:
                sx = dx;
                sy = h - 1 - dy;
                break;
            case 5:
                sx = dy;
                sy = dx;
                break;
            case 6:
                sx = dy;
                sy = h - 1 - dx;
                break;
            case 7:
                sx = w - 1 - dy;
                sy = h - 1 - dx;
                break;
            case 8:
                sx = w - 1 - dy;
                sy = dx;
                break;
            default:
                break;
            }
            const auto s = (static_cast<std::size_t>(sy) * w + sx) * 3;
            const auto t = (static_cast<std::size_t>(dy) * out.width + dx) * 3;
            std::copy_n(src.rgb.begin() + static_cast<std::ptrdiff_t>(s), 3,
                        out.rgb.begin() + static_cast<std::ptrdiff_t>(t));
        }
    }
    return out;
}

// RIFF....WEBP - magic webp, правила считают его Photo.
[[nodiscard]] bool isWebp(std::span<const std::byte> d) {
    return d.size() >= 12 && std::memcmp(d.data(), "RIFF", 4) == 0 &&
           std::memcmp(d.data() + 8, "WEBP", 4) == 0;
}

// Общий хвост: RGBA-пиксели -> фон под альфой -> ресайз -> поворот EXIF -> jpeg.
[[nodiscard]] Bytes rgbaToThumbnail(const unsigned char* rgba, int w, int h, int orientation) {
    // Прозрачность -> темный фон: в jpeg альфы нет.
    const auto pixels = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    Image full{w, h, std::vector<unsigned char>(pixels * 3)};
    for (std::size_t p = 0; p < pixels; ++p) {
        const unsigned a = rgba[p * 4 + 3];
        for (std::size_t c = 0; c < 3; ++c) {
            const unsigned v = rgba[p * 4 + c];
            full.rgb[p * 3 + c] =
                static_cast<unsigned char>((v * a + kBackground[c] * (255 - a) + 127) / 255);
        }
    }

    const double scale = std::min(1.0, static_cast<double>(domain::kThumbnailMaxSide) /
                                           static_cast<double>(std::max(w, h)));
    Image small = std::move(full);
    if (scale < 1.0) {
        Image resized;
        resized.width = std::max(1, static_cast<int>(w * scale + 0.5));
        resized.height = std::max(1, static_cast<int>(h * scale + 0.5));
        resized.rgb.resize(static_cast<std::size_t>(resized.width) * resized.height * 3);
        if (stbir_resize_uint8_linear(small.rgb.data(), w, h, 0, resized.rgb.data(),
                                      resized.width, resized.height, 0, STBIR_RGB) == nullptr) {
            return {};
        }
        small = std::move(resized);
    }
    const Image oriented = orient(small, orientation);

    Bytes jpeg;
    auto write = [](void* ctx, void* bytes, int size) {
        auto* out = static_cast<Bytes*>(ctx);
        const auto* p = static_cast<const std::byte*>(bytes);
        out->insert(out->end(), p, p + size);
    };
    if (stbi_write_jpg_to_func(write, &jpeg, oriented.width, oriented.height, 3,
                               oriented.rgb.data(), kJpegQuality) == 0) {
        return {};
    }
    return jpeg;
}

class StbThumbnailer final : public domain::Thumbnailer {
public:
    Result<Bytes> make(std::span<const std::byte> image) override {
        if (image.empty() ||
            image.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            return Bytes{};
        }
        const auto* data = reinterpret_cast<const unsigned char*>(image.data());
        const int len = static_cast<int>(image.size());
        int w = 0;
        int h = 0;

        if (isWebp(image)) {
            WebPBitstreamFeatures features{};
            if (WebPGetFeatures(data, len, &features) != VP8_STATUS_OK) {
                return Bytes{};
            }
            // анимацию WebPDecode не декодирует (нужен demux) - будет заглушка
            if (features.has_animation != 0 || features.width <= 0 || features.height <= 0 ||
                static_cast<std::uint64_t>(features.width) *
                        static_cast<std::uint64_t>(features.height) >
                    kMaxPixels) {
                return Bytes{};
            }
            std::unique_ptr<unsigned char, decltype(&WebPFree)> rgba(
                WebPDecodeRGBA(data, len, &w, &h), &WebPFree);
            if (rgba == nullptr || w <= 0 || h <= 0) {
                return Bytes{};
            }
            // EXIF-чанк webp не разбираем: ориентация как есть
            return rgbaToThumbnail(rgba.get(), w, h, 1);
        }

        int channels = 0;
        if (stbi_info_from_memory(data, len, &w, &h, &channels) == 0 || w <= 0 || h <= 0 ||
            static_cast<std::uint64_t>(w) * static_cast<std::uint64_t>(h) > kMaxPixels) {
            return Bytes{};
        }
        std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> rgba(
            stbi_load_from_memory(data, len, &w, &h, &channels, 4), &stbi_image_free);
        if (!rgba) {
            return Bytes{};
        }
        return rgbaToThumbnail(rgba.get(), w, h, jpegOrientation(image));
    }
};

} // namespace

std::unique_ptr<domain::Thumbnailer> makeStbThumbnailer() {
    return std::make_unique<StbThumbnailer>();
}

} // namespace safebox::infra
