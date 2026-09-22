// makeServices - собирает сервисы из портов, вызывается из daemon/main.cpp и из тестов.
// kdf и chunkSize в AppConfig нужны только при создании нового сейфа, у существующего они лежат в файле
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>

#include "safebox/app/services.hpp"
#include "safebox/domain/model/safe_format.hpp"
#include "safebox/domain/ports/archive.hpp"
#include "safebox/domain/ports/clock.hpp"
#include "safebox/domain/ports/crypto.hpp"
#include "safebox/domain/ports/media.hpp"
#include "safebox/domain/ports/storage.hpp"

namespace safebox::app {

struct Ports {
    domain::CryptoSuite& crypto;
    domain::VaultStore& store;
    domain::Thumbnailer& thumbnailer;
    domain::ZipWriter& zip;
    domain::Clock& clock;
};

struct AppConfig {
    domain::KdfParams kdf = domain::kDefaultKdf;
    std::uint32_t chunkSize = domain::kDefaultChunkSize;
    std::chrono::seconds idleTimeout{15 * 60};    // 15 мин без активности
    std::chrono::seconds presenceTimeout{2 * 60}; // окно закрыто: 2 мин без heartbeat
    std::chrono::milliseconds leaseDrainTimeout{10'000};
    std::filesystem::path defaultDirectory; // база относительных путей (обычно "Документы")
};

[[nodiscard]] Services makeServices(Ports ports, AppConfig config);

} // namespace safebox::app
