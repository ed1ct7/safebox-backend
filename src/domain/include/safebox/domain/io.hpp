// ByteSource/ByteSink - чтобы большие файлы шли кусками, а не целиком через память.
// Плюс мелкие утилиты: Bytes, secureWipe, конвертация UTF-8 <-> path
#pragma once

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "safebox/domain/model/error.hpp"

namespace safebox::domain {

using Bytes = std::vector<std::byte>;

class ByteSource {
public:
    virtual ~ByteSource() = default;

    // Читает до buffer.size() байт; 0 - конец потока.
    [[nodiscard]] virtual Result<std::size_t> read(std::span<std::byte> buffer) = 0;
};

class ByteSink {
public:
    virtual ~ByteSink() = default;

    [[nodiscard]] virtual Status write(std::span<const std::byte> data) = 0;
};

// Источник поверх готового буфера (тесты, миниатюры, мелкие данные).
class MemorySource final : public ByteSource {
public:
    explicit MemorySource(std::span<const std::byte> data) noexcept : data_(data) {}

    [[nodiscard]] Result<std::size_t> read(std::span<std::byte> buffer) override {
        const auto n = std::min(buffer.size(), data_.size() - pos_);
        std::copy_n(data_.begin() + static_cast<std::ptrdiff_t>(pos_), n, buffer.begin());
        pos_ += n;
        return n;
    }

private:
    std::span<const std::byte> data_;
    std::size_t pos_ = 0;
};

// Приемник в память (тесты, миниатюры).
class MemorySink final : public ByteSink {
public:
    [[nodiscard]] Status write(std::span<const std::byte> data) override {
        bytes.insert(bytes.end(), data.begin(), data.end());
        return {};
    }

    Bytes bytes;
};

[[nodiscard]] inline std::span<const std::byte> asBytes(std::string_view s) noexcept {
    return std::as_bytes(std::span<const char>(s.data(), s.size()));
}

[[nodiscard]] inline std::string_view asChars(std::span<const std::byte> b) noexcept {
    return {reinterpret_cast<const char*>(b.data()), b.size()};
}

[[nodiscard]] inline Bytes toBytes(std::string_view s) {
    const auto b = asBytes(s);
    return Bytes(b.begin(), b.end());
}

// Затирание, которое компилятор не выкинет как "мертвую запись" (volatile).
// Для расшифрованных имен/кусков: ключи затирает адаптер (sodium_free).
inline void secureWipe(std::span<std::byte> data) noexcept {
    volatile std::byte* p = data.data();
    for (std::size_t i = 0; i < data.size(); ++i) {
        p[i] = std::byte{0};
    }
}

inline void secureWipe(Bytes& data) noexcept {
    secureWipe(std::span<std::byte>(data));
    data.clear();
}

inline void secureWipe(std::string& data) noexcept {
    volatile char* p = data.data();
    for (std::size_t i = 0; i < data.size(); ++i) {
        p[i] = '\0';
    }
    data.clear();
}

// Пути в проводе и в application - UTF-8 строки. На Windows конструктор
// path(std::string) трактует байты в ANSI-кодировке, поэтому только через u8.
[[nodiscard]] inline std::filesystem::path pathFromUtf8(std::string_view utf8) {
    return std::filesystem::path(
        std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

[[nodiscard]] inline std::string pathToUtf8(const std::filesystem::path& path) {
    const auto u8 = path.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

} // namespace safebox::domain
