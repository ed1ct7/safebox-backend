// Вместо jpeg возвращает "THUMB:<размер>" - тестам важно только что миниатюра сделана из полного файла
#pragma once

#include <atomic>
#include <string>

#include "safebox/domain/ports/media.hpp"

namespace safebox::test {

class FakeThumbnailer final : public domain::Thumbnailer {
public:
    domain::Result<domain::Bytes> make(std::span<const std::byte> image) override {
        ++calls;
        if (!enabled) {
            return domain::Bytes{};
        }
        return domain::toBytes("THUMB:" + std::to_string(image.size()));
    }

    std::atomic<int> calls{0};
    std::atomic<bool> enabled{true};
};

} // namespace safebox::test
