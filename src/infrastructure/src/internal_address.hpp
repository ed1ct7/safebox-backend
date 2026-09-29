// Адрес не из публичного интернета: туда предпросмотр не ходит (иначе ссылка вида
// http://192.168.0.1/ или http://localhost:8080/ превращает сейф в сканер локальной сети).
// Чистая функция над байтами адреса, без сокетов - тестируется без сети
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace safebox::infra {

namespace detail {

[[nodiscard]] constexpr bool isInternalIpv4(std::uint8_t a, std::uint8_t b) noexcept {
    return a == 0                              // 0.0.0.0/8: "этот хост", в том числе 0.0.0.0
           || a == 10                          // 10/8
           || a == 127                         // loopback
           || (a == 100 && (b & 0xC0) == 0x40) // 100.64/10: общее адресное пространство (CGNAT)
           || (a == 169 && b == 254)           // link-local
           || (a == 172 && (b & 0xF0) == 0x10) // 172.16/12
           || (a == 192 && b == 168)           // 192.168/16
           || a >= 224;                        // multicast, зарезервированные, broadcast
}

} // namespace detail

// Четыре байта - IPv4, шестнадцать - IPv6, в сетевом порядке. Loopback, приватные,
// link-local, неопределенные и не-unicast адреса - внутренние; IPv4, вложенный в IPv6
// (::ffff:a.b.c.d, ::a.b.c.d, 64:ff9b::/96), проверяется как IPv4. Любая другая длина
// считается внутренней: сомневаемся - не ходим.
[[nodiscard]] constexpr bool isInternalAddress(std::span<const std::uint8_t> ip) noexcept {
    if (ip.size() == 4) {
        return detail::isInternalIpv4(ip[0], ip[1]);
    }
    if (ip.size() != 16) {
        return true;
    }
    const auto zeros = [&](std::size_t from, std::size_t to) {
        for (std::size_t i = from; i < to; ++i) {
            if (ip[i] != 0) {
                return false;
            }
        }
        return true;
    };
    if (zeros(0, 10) && ((ip[10] == 0xFF && ip[11] == 0xFF) || (ip[10] == 0 && ip[11] == 0))) {
        // ::ffff:a.b.c.d и ::a.b.c.d; сюда же :: и ::1 (первый байт IPv4 нулевой)
        return detail::isInternalIpv4(ip[12], ip[13]);
    }
    if (ip[0] == 0x00 && ip[1] == 0x64 && ip[2] == 0xFF && ip[3] == 0x9B && zeros(4, 12)) {
        return detail::isInternalIpv4(ip[12], ip[13]);
    }
    return (ip[0] & 0xFE) == 0xFC                       // fc00::/7, ULA
           || (ip[0] == 0xFE && (ip[1] & 0xC0) == 0x80) // fe80::/10, link-local
           || (ip[0] == 0xFE && (ip[1] & 0xC0) == 0xC0) // fec0::/10, site-local (устарел)
           || ip[0] == 0xFF;                            // ff00::/8, multicast
}

} // namespace safebox::infra
