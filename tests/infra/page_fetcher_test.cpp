// Сетевых запросов здесь нет: проверяется классификация адресов и отказы,
// которые случаются до любого соединения
#include <catch2/catch_test_macros.hpp>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#endif

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "internal_address.hpp"
#include "safebox/infra/factories.hpp"

using namespace safebox;

namespace {

// Текст адреса -> байты в сетевом порядке (4 или 16), пусто - не адрес.
std::vector<std::uint8_t> bytesOf(const char* text) {
    std::array<std::uint8_t, 16> v6{};
    if (inet_pton(AF_INET6, text, v6.data()) == 1) {
        return {v6.begin(), v6.end()};
    }
    std::array<std::uint8_t, 4> v4{};
    if (inet_pton(AF_INET, text, v4.data()) == 1) {
        return {v4.begin(), v4.end()};
    }
    return {};
}

bool internal(const char* text) {
    const auto bytes = bytesOf(text);
    REQUIRE_FALSE(bytes.empty());
    return infra::isInternalAddress(bytes);
}

} // namespace

TEST_CASE("internal IPv4 addresses are recognised", "[page_fetcher][address]") {
    for (const auto* ip :
         {"127.0.0.1", "127.255.255.254", "10.0.0.1", "10.255.255.255", "172.16.0.1",
          "172.31.255.255", "192.168.0.1", "192.168.255.255", "169.254.169.254", "0.0.0.0",
          "0.1.2.3", "100.64.0.1", "100.127.255.255", "224.0.0.1", "255.255.255.255"}) {
        INFO("address: " << ip);
        CHECK(internal(ip));
    }
}

TEST_CASE("public IPv4 addresses are not internal", "[page_fetcher][address]") {
    for (const auto* ip :
         {"8.8.8.8", "1.1.1.1", "93.184.216.34", "11.0.0.1", "9.255.255.255", "172.15.255.255",
          "172.32.0.0", "192.167.255.255", "192.169.0.0", "169.253.255.255", "169.255.0.0",
          "100.63.255.255", "100.128.0.0", "126.255.255.255", "128.0.0.1", "223.255.255.255"}) {
        INFO("address: " << ip);
        CHECK_FALSE(internal(ip));
    }
}

TEST_CASE("internal IPv6 addresses are recognised", "[page_fetcher][address]") {
    for (const auto* ip : {"::", "::1", "fe80::1", "fe80::", "febf::1", "fec0::1",
                           "fc00::", "fd12:3456:789a::1", "fdff:ffff::", "ff02::1", "ff00::"}) {
        INFO("address: " << ip);
        CHECK(internal(ip));
    }
}

TEST_CASE("public IPv6 addresses are not internal", "[page_fetcher][address]") {
    for (const auto* ip : {"2606:4700::", "2606:4700:4700::1111", "2001:4860:4860::8888",
                           "2a00:1450:4010:c05::64", "fbff::1", "fe00::1", "fe7f::1", "2000::"}) {
        INFO("address: " << ip);
        CHECK_FALSE(internal(ip));
    }
}

TEST_CASE("IPv4 inside IPv6 is judged as IPv4", "[page_fetcher][address]") {
    for (const auto* ip : {"::ffff:127.0.0.1", "::ffff:10.1.2.3", "::ffff:192.168.0.1",
                           "::ffff:169.254.1.1", "::ffff:0.0.0.0", "::127.0.0.1", "::10.0.0.1",
                           "64:ff9b::7f00:1", "64:ff9b::a00:1", "64:ff9b::c0a8:1"}) {
        INFO("address: " << ip);
        CHECK(internal(ip));
    }
    for (const auto* ip : {"::ffff:8.8.8.8", "::ffff:93.184.216.34", "64:ff9b::808:808"}) {
        INFO("address: " << ip);
        CHECK_FALSE(internal(ip));
    }
}

TEST_CASE("an address of unexpected length is treated as internal", "[page_fetcher][address]") {
    CHECK(infra::isInternalAddress({}));
    const std::array<std::uint8_t, 8> eight{8, 8, 8, 8, 8, 8, 8, 8};
    CHECK(infra::isInternalAddress(eight));
    const std::array<std::uint8_t, 5> five{8, 8, 8, 8, 8};
    CHECK(infra::isInternalAddress(five));
}

TEST_CASE("the page fetcher refuses without touching the network", "[page_fetcher]") {
    auto fetcher = infra::makePageFetcher();
    REQUIRE(fetcher != nullptr);

    // адреса-литералы и localhost не требуют ни DNS-сервера, ни соединения
    for (const auto* url :
         {"http://127.0.0.1/", "http://127.0.0.1:8080/status", "https://10.0.0.5/admin",
          "http://192.168.1.1/", "http://169.254.169.254/latest/meta-data/", "http://0.0.0.0/",
          "http://[::1]/", "http://[fe80::1]/", "http://[::ffff:127.0.0.1]/", "http://localhost/",
          "http://LOCALHOST:3000/", "http://user:pass@127.0.0.1/"}) {
        INFO("url: " << url);
        const auto result = fetcher->fetch(domain::FetchRequest{.url = url});
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == domain::Error::Code::IoError);
#if defined(_WIN32)
        CHECK(result.error().message == "Адрес во внутренней сети - предпросмотр не загружается");
#endif
    }

    // не http(s) и мусор - отказ, а не попытка соединиться
    for (const auto* url : {"", "ftp://example.com/", "file:///C:/Windows/win.ini", "javascript:1",
                            "example.com", "http://", "http:///path", "http://host:99999/",
                            "http://host:0/", "http://host:abc/", "http://[::1/"}) {
        INFO("url: " << url);
        const auto result = fetcher->fetch(domain::FetchRequest{.url = url});
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == domain::Error::Code::IoError);
        CHECK_FALSE(result.error().message.empty());
    }
}
