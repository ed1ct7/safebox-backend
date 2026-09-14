// Фабрики адаптеров. Сюда не инклюдить sqlite3.h/sodium.h - этот хедер видит daemon
#pragma once

#include <memory>

#include "safebox/domain/ports/archive.hpp"
#include "safebox/domain/ports/clock.hpp"
#include "safebox/domain/ports/crypto.hpp"
#include "safebox/domain/ports/media.hpp"
#include "safebox/domain/ports/storage.hpp"

namespace safebox::infra {

struct ZipWriterOptions {
    // Всегда писать ZIP64-структуры - только чтобы тесты проверяли эту ветку
    // без архивов > 4 ГБ.
    bool forceZip64 = false;
};

[[nodiscard]] std::unique_ptr<domain::CryptoSuite> makeSodiumCrypto();
[[nodiscard]] std::unique_ptr<domain::VaultStore> makeSqliteVaultStore();
[[nodiscard]] std::unique_ptr<domain::Thumbnailer> makeStbThumbnailer();
[[nodiscard]] std::unique_ptr<domain::ZipWriter> makeStreamZipWriter(ZipWriterOptions options = {});
[[nodiscard]] std::unique_ptr<domain::Clock> makeSystemClock();

} // namespace safebox::infra
