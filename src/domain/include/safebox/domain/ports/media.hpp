// Миниатюры. Битая картинка или вообще не картинка -> пустой результат, это не ошибка
#pragma once

#include <cstdint>
#include <span>

#include "safebox/domain/io.hpp"
#include "safebox/domain/model/error.hpp"

namespace safebox::domain {

inline constexpr std::uint32_t kThumbnailMaxSide = 400;             // px, большая сторона
inline constexpr std::uint64_t kThumbnailSourceLimit = 64ull << 20; // фото крупнее - без миниатюры

class Thumbnailer {
public:
    virtual ~Thumbnailer() = default;

    [[nodiscard]] virtual Result<Bytes> make(std::span<const std::byte> image) = 0;
};

} // namespace safebox::domain
