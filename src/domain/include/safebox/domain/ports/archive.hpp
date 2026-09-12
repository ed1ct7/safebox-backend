// Порт для zip-экспорта. Пишем строго последовательно (в http-ответе seek не сделать),
// поэтому STORE + data descriptor + ZIP64
#pragma once

#include <cstdint>
#include <memory>
#include <string_view>

#include "safebox/domain/io.hpp"
#include "safebox/domain/model/error.hpp"

namespace safebox::domain {

class ZipStream {
public:
    virtual ~ZipStream() = default;

    [[nodiscard]] virtual Status addDirectory(std::string_view path, std::int64_t mtimeMs) = 0;
    // size - сколько байт отдаст source; расхождение -> ошибка (архив был бы битым).
    [[nodiscard]] virtual Status addFile(std::string_view path, std::uint64_t size,
                                         std::int64_t mtimeMs, ByteSource& source) = 0;
    [[nodiscard]] virtual Status finish() = 0;
};

class ZipWriter {
public:
    virtual ~ZipWriter() = default;

    [[nodiscard]] virtual std::unique_ptr<ZipStream> start(ByteSink& out) = 0;
};

} // namespace safebox::domain
