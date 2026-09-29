#include <catch2/catch_test_macros.hpp>

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <stb_image.h>
#include <stb_image_write.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <miniz.h>
#include <webp/encode.h>

#include <cstdint>
#include <cstring>
#include <random>

#include "safebox/infra/factories.hpp"

using namespace safebox;

namespace {

domain::Bytes encode(int w, int h, int channels, bool jpeg) {
    std::vector<unsigned char> pixels(static_cast<std::size_t>(w * h * channels));
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            auto* p = &pixels[static_cast<std::size_t>((y * w + x) * channels)];
            p[0] = static_cast<unsigned char>(x);
            p[1] = static_cast<unsigned char>(y);
            p[2] = 128;
            if (channels == 4) {
                p[3] = static_cast<unsigned char>(x % 256);
            }
        }
    }
    domain::Bytes out;
    auto write = [](void* ctx, void* data, int size) {
        auto* bytes = static_cast<domain::Bytes*>(ctx);
        const auto* p = static_cast<const std::byte*>(data);
        bytes->insert(bytes->end(), p, p + size);
    };
    if (jpeg) {
        stbi_write_jpg_to_func(write, &out, w, h, channels, pixels.data(), 90);
    } else {
        stbi_write_png_to_func(write, &out, w, h, channels, pixels.data(), w * channels);
    }
    return out;
}

std::pair<int, int> dimensions(const domain::Bytes& image) {
    int w = 0;
    int h = 0;
    int c = 0;
    REQUIRE(stbi_info_from_memory(reinterpret_cast<const stbi_uc*>(image.data()),
                                  static_cast<int>(image.size()), &w, &h, &c) == 1);
    return {w, h};
}

domain::Bytes encodeWebp(int w, int h, bool lossless) {
    std::vector<unsigned char> pixels(static_cast<std::size_t>(w * h * 4));
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            auto* p = &pixels[static_cast<std::size_t>((y * w + x) * 4)];
            p[0] = static_cast<unsigned char>(x);
            p[1] = static_cast<unsigned char>(y);
            p[2] = 128;
            p[3] = 255;
        }
    }
    std::uint8_t* encoded = nullptr;
    const std::size_t size = lossless ? WebPEncodeLosslessRGBA(pixels.data(), w, h, w * 4, &encoded)
                                      : WebPEncodeRGBA(pixels.data(), w, h, w * 4, 80.f, &encoded);
    REQUIRE(encoded != nullptr);
    REQUIRE(size > 0);
    domain::Bytes out(size);
    std::memcpy(out.data(), encoded, size);
    WebPFree(encoded);
    return out;
}

// APP1 Exif с одной записью Orientation сразу после SOI.
domain::Bytes withOrientation(const domain::Bytes& jpeg, std::uint8_t orientation) {
    const unsigned char exif[] = {
        0xFF, 0xE1,        0x00, 0x22, 'E',  'x',  'i',  'f',  0, 0, // APP1, длина 34
        'M',  'M',         0x00, 0x2A, 0x00, 0x00, 0x00, 0x08,       // TIFF BE, IFD0 @8
        0x00, 0x01,                                                  // 1 запись
        0x01, 0x12,        0x00, 0x03, 0x00, 0x00, 0x00, 0x01,       // Orientation, SHORT, 1
        0x00, orientation, 0x00, 0x00,                               // значение
        0x00, 0x00,        0x00, 0x00};                              // next IFD
    domain::Bytes out(jpeg.begin(), jpeg.begin() + 2);
    for (const auto b : exif) {
        out.push_back(static_cast<std::byte>(b));
    }
    out.insert(out.end(), jpeg.begin() + 2, jpeg.end());
    return out;
}

struct ZipFile {
    std::string name;
    std::string data;
    bool directory = false;
};

std::vector<ZipFile> unzipWithMiniz(const domain::Bytes& archive) {
    mz_zip_archive zip{};
    REQUIRE(mz_zip_reader_init_mem(&zip, archive.data(), archive.size(), 0));
    std::vector<ZipFile> files;
    const auto count = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat stat{};
        REQUIRE(mz_zip_reader_file_stat(&zip, i, &stat));
        ZipFile file{stat.m_filename, {}, stat.m_is_directory != 0};
        if (!file.directory) {
            std::size_t size = 0;
            void* data = mz_zip_reader_extract_to_heap(&zip, i, &size, 0); // проверяет CRC
            REQUIRE(data != nullptr);
            file.data.assign(static_cast<const char*>(data), size);
            mz_free(data);
        }
        files.push_back(std::move(file));
    }
    mz_zip_reader_end(&zip);
    return files;
}

domain::Bytes makeArchive(bool forceZip64, const std::string& bigData) {
    auto writer = infra::makeStreamZipWriter({forceZip64});
    domain::MemorySink sink;
    auto zip = writer->start(sink);
    REQUIRE(zip->addDirectory("Отпуск", 1'790'000'000'000).has_value());
    domain::MemorySource big(domain::asBytes(bigData));
    REQUIRE(zip->addFile("Отпуск/море.bin", bigData.size(), 1'790'000'000'000, big).has_value());
    domain::MemorySource empty(domain::asBytes(""));
    REQUIRE(zip->addFile("Отпуск/пусто.txt", 0, 1'790'000'000'000, empty).has_value());
    domain::MemorySource small(domain::asBytes("hello"));
    REQUIRE(zip->addFile("readme.txt", 5, 0, small).has_value());
    REQUIRE(zip->finish().has_value());
    return sink.bytes;
}

} // namespace

TEST_CASE("thumbnailer scales images and ignores non-images", "[media]") {
    auto thumbnailer = infra::makeStbThumbnailer();
    auto thumb = thumbnailer->make(encode(800, 600, 3, false));
    REQUIRE(thumb.has_value());
    REQUIRE_FALSE(thumb->empty());
    CHECK(static_cast<unsigned char>((*thumb)[0]) == 0xFF); // jpeg
    CHECK(dimensions(*thumb) == std::pair{400, 300});

    auto small = thumbnailer->make(encode(120, 80, 3, true));
    REQUIRE(small.has_value());
    CHECK(dimensions(*small) == std::pair{120, 80}); // маленькие не увеличиваются

    auto transparent = thumbnailer->make(encode(300, 300, 4, false));
    REQUIRE(transparent.has_value());
    CHECK(dimensions(*transparent) == std::pair{300, 300});

    auto garbage = thumbnailer->make(domain::toBytes("definitely not an image"));
    REQUIRE(garbage.has_value());
    CHECK(garbage->empty());
    auto nothing = thumbnailer->make({});
    REQUIRE(nothing.has_value());
    CHECK(nothing->empty());
}

TEST_CASE("thumbnailer decodes webp (lossy and lossless)", "[media][webp]") {
    auto thumbnailer = infra::makeStbThumbnailer();

    auto thumb = thumbnailer->make(encodeWebp(800, 600, false));
    REQUIRE(thumb.has_value());
    REQUIRE_FALSE(thumb->empty());
    CHECK(static_cast<unsigned char>((*thumb)[0]) == 0xFF); // jpeg
    CHECK(dimensions(*thumb) == std::pair{400, 300});

    auto lossless = thumbnailer->make(encodeWebp(120, 80, true));
    REQUIRE(lossless.has_value());
    CHECK(dimensions(*lossless) == std::pair{120, 80}); // маленькие не увеличиваются

    // webp-заголовок с битым полезным грузом - пустая миниатюра, не ошибка
    auto broken = encodeWebp(300, 200, false);
    broken.resize(broken.size() / 2);
    auto brokenThumb = thumbnailer->make(broken);
    REQUIRE(brokenThumb.has_value());
    CHECK(brokenThumb->empty());
}

TEST_CASE("thumbnail honours EXIF orientation like the browser does", "[media][exif]") {
    auto thumbnailer = infra::makeStbThumbnailer();
    const auto landscape = encode(800, 400, 3, true);
    auto upright = thumbnailer->make(withOrientation(landscape, 1));
    REQUIRE(upright.has_value());
    CHECK(dimensions(*upright) == std::pair{400, 200});
    auto rotated = thumbnailer->make(withOrientation(landscape, 6));
    REQUIRE(rotated.has_value());
    CHECK(dimensions(*rotated) == std::pair{200, 400});
}

TEST_CASE("streamed zip is readable by a third-party reader", "[zip][UF-11]") {
    std::mt19937 rng(1);
    std::string bigData(300'000, '\0');
    for (auto& c : bigData) {
        c = static_cast<char>(rng());
    }
    for (const bool zip64 : {false, true}) {
        CAPTURE(zip64);
        const auto archive = makeArchive(zip64, bigData);
        const auto files = unzipWithMiniz(archive);
        REQUIRE(files.size() == 4);
        CHECK(files[0].name == "Отпуск/");
        CHECK(files[0].directory);
        CHECK(files[1].name == "Отпуск/море.bin");
        CHECK(files[1].data == bigData);
        CHECK(files[2].name == "Отпуск/пусто.txt");
        CHECK(files[2].data.empty());
        CHECK(files[3].name == "readme.txt");
        CHECK(files[3].data == "hello");
    }
}

TEST_CASE("zip refuses a source that lies about its size", "[zip]") {
    auto writer = infra::makeStreamZipWriter();
    domain::MemorySink sink;
    auto zip = writer->start(sink);
    domain::MemorySource shortSource(domain::asBytes("abc"));
    auto st = zip->addFile("a.txt", 10, 0, shortSource);
    REQUIRE_FALSE(st.has_value());
    CHECK(st.error().code == domain::Error::Code::IntegrityError);
    domain::MemorySource longSource(domain::asBytes("abcdef"));
    CHECK_FALSE(zip->addFile("b.txt", 2, 0, longSource).has_value());
    CHECK_FALSE(zip->addFile("/abs.txt", 0, 0, longSource).has_value());
}
