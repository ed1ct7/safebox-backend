// Обвязка для тестов application. Кусок 1 КиБ, чтобы многокусковые случаи проверялись на мелких данных.
// Порядок полей важен: services должны умереть первыми
#pragma once

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "FakeClock.hpp"
#include "FakeThumbnailer.hpp"
#include "InMemoryVaultStore.hpp"
#include "MemZipWriter.hpp"
#include "safebox/app/factory.hpp"
#include "safebox/infra/factories.hpp"

namespace safebox::test {

inline constexpr std::uint32_t kTestChunk = 1024;

// Абсолютный путь, которого точно нет на диске: InMemoryVaultStore хранит
// "файлы" в памяти, а предпроверка create смотрит на настоящий диск.
inline std::filesystem::path testSafeDir() {
    return std::filesystem::temp_directory_path() / "safebox-app-tests-in-memory";
}

inline std::string randomBytes(std::size_t n, std::uint32_t seed = 42) {
    std::mt19937 rng(seed);
    std::string out(n, '\0');
    for (auto& c : out) {
        c = static_cast<char>(rng() & 0xFF);
    }
    return out;
}

inline std::string fakeJpeg(std::size_t n, std::uint32_t seed = 7) {
    auto data = randomBytes(n, seed);
    data.replace(0, 4, "\xFF\xD8\xFF\xE0", 4);
    return data;
}

struct AppFixture {
    explicit AppFixture(std::uint32_t chunkSize = kTestChunk) {
        config.kdf = domain::kMinimalKdf;
        config.chunkSize = chunkSize;
        config.defaultDirectory = testSafeDir();
        config.leaseDrainTimeout = std::chrono::seconds(5);
        services = app::makeServices({*crypto, store, thumbnailer, zip, clock}, config);
    }

    app::UnlockResult createSafe(const std::string& name = "test",
                                 const std::string& password = "secret1") {
        auto result = services.safe->create({name, password, password});
        REQUIRE(result.has_value());
        return *result;
    }

    app::Lease lease(const app::UnlockResult& session) {
        auto l = services.safe->authorize(session.tokens.api, app::TokenScope::Api);
        REQUIRE(l.has_value());
        return std::move(*l);
    }

    [[nodiscard]] std::filesystem::path safePath(const std::string& name = "test") const {
        return (testSafeDir() / domain::pathFromUtf8(name + ".safebox")).lexically_normal();
    }

    domain::ImportResult importFiles(const app::UnlockResult& session,
                                     const std::vector<std::pair<std::string, std::string>>& files,
                                     std::optional<domain::EntryId> parent = std::nullopt,
                                     std::size_t piece = 333) {
        auto import = services.importExport->beginImport(lease(session), parent);
        REQUIRE(import.has_value());
        for (const auto& [path, data] : files) {
            REQUIRE((*import)->beginFile(path).has_value());
            for (std::size_t off = 0; off < data.size(); off += piece) {
                const auto part = std::string_view(data).substr(off, piece);
                REQUIRE((*import)->write(domain::asBytes(part)).has_value());
            }
            REQUIRE((*import)->endFile().has_value());
        }
        return (*import)->finish();
    }

    std::string readContent(const app::UnlockResult& session, domain::EntryId id,
                            app::ContentVariant variant = app::ContentVariant::Original) {
        auto opened = services.importExport->openContent(lease(session), id, variant);
        REQUIRE(opened.has_value());
        domain::MemorySink sink;
        REQUIRE(opened->stream->read(0, opened->stream->size(), sink).has_value());
        return std::string(domain::asChars(sink.bytes));
    }

    // Запись по имени в папке (nullopt - корень).
    domain::Entry entryNamed(const app::UnlockResult& session, const std::string& name,
                             std::optional<domain::EntryId> folder = std::nullopt) {
        auto listing = services.entries->list(lease(session), folder);
        REQUIRE(listing.has_value());
        const auto it = std::find_if(listing->entries.begin(), listing->entries.end(),
                                     [&](const domain::Entry& e) { return e.name == name; });
        INFO("запись " << name);
        REQUIRE(it != listing->entries.end());
        return *it;
    }

    std::unique_ptr<domain::CryptoSuite> crypto = infra::makeSodiumCrypto();
    InMemoryVaultStore store;
    FakeThumbnailer thumbnailer;
    MemZipWriter zip;
    FakeClock clock;
    app::AppConfig config;
    app::Services services; // последним: уничтожается первым
};

} // namespace safebox::test
