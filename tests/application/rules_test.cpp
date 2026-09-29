// Правила домена v2: названия тегов, адреса ссылок. Отдельного набора для домена нет.
#include <catch2/catch_test_macros.hpp>

#include "safebox/domain/model/rules.hpp"

using namespace safebox;
using Code = domain::Error::Code;

TEST_CASE("validateTagName trims and checks length, colon and control characters",
          "[rules][tags]") {
    auto trimmed = domain::validateTagName("  Иван Петров \t");
    REQUIRE(trimmed.has_value());
    CHECK(*trimmed == "Иван Петров");
    CHECK(domain::validateTagName("🙂 смайл").has_value());

    for (const std::string_view bad : {"", "   ", "\t\n"}) {
        auto result = domain::validateTagName(bad);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == Code::InvalidArgument);
    }
    // длина считается в символах, а не в байтах
    std::string hundred;
    for (int i = 0; i < 100; ++i) {
        hundred += "ж";
    }
    CHECK(domain::validateTagName(hundred).has_value());
    auto tooLong = domain::validateTagName(hundred + "ж");
    REQUIRE_FALSE(tooLong.has_value());
    CHECK(tooLong.error().code == Code::InvalidArgument);
    CHECK(domain::validateTagName(std::string(100, 'a')).has_value());
    CHECK_FALSE(domain::validateTagName(std::string(101, 'a')).has_value());

    const std::string_view forbidden[] = {"Люди:Иван",
                                          ":",
                                          "a\tb",
                                          "a\nb",
                                          "a\x01"
                                          "b",
                                          "a\x7F"
                                          "b",
                                          "a\xC2\x85"
                                          "b",
                                          std::string_view("a\0b", 3)};
    for (const auto bad : forbidden) {
        auto result = domain::validateTagName(bad);
        INFO(bad);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == Code::InvalidArgument);
    }
    CHECK_FALSE(domain::validateTagName("bad\xFF"
                                        "utf8")
                    .has_value());
}

TEST_CASE("isHttpUrl accepts only http(s) addresses with a host", "[rules][url]") {
    CHECK(domain::isHttpUrl("http://example.com"));
    CHECK(domain::isHttpUrl("HTTPS://Example.com/a?b=1#c"));
    CHECK(domain::isHttpUrl("https://пример.рф/путь"));
    for (const std::string_view bad :
         {"", "example.com", "ftp://example.com", "javascript:alert(1)", "file:///etc/passwd",
          "http://", "https:///path", "http://a b.com", "http://a.com/\n", "//example.com"}) {
        INFO(bad);
        CHECK_FALSE(domain::isHttpUrl(bad));
    }
    CHECK_FALSE(domain::isHttpUrl("http://" + std::string(domain::kMaxUrlBytes, 'a')));
}

TEST_CASE("normalizeUrl makes equal links compare equal", "[rules][url]") {
    const auto norm = [](std::string_view url) { return domain::normalizeUrl(url); };

    CHECK(norm("HTTP://Example.COM:80/a/?utm_source=x&id=5#Frag") ==
          std::optional<std::string>("http://example.com/a?id=5#Frag"));
    CHECK(norm("https://a.com:443") == std::optional<std::string>("https://a.com"));
    CHECK(norm("https://a.com:443/") == std::optional<std::string>("https://a.com"));
    CHECK(norm("http://a.com:443/") == std::optional<std::string>("http://a.com:443"));
    CHECK(norm("https://a.com:80/") == std::optional<std::string>("https://a.com:80"));
    CHECK(norm("https://a.com:8443/x/") == std::optional<std::string>("https://a.com:8443/x"));
    CHECK(norm("https://a.com/") == std::optional<std::string>("https://a.com"));
    CHECK(norm("https://a.com") == std::optional<std::string>("https://a.com"));

    // регистр пути и параметров сохраняется, порядок остальных параметров - тоже
    CHECK(norm("https://a.com/Path/File?B=2&utm_medium=m&a=1&UTM_term=t") ==
          std::optional<std::string>("https://a.com/Path/File?B=2&a=1"));
    CHECK(norm("https://a.com/?utm_source=x") == std::optional<std::string>("https://a.com"));
    CHECK(norm("https://a.com/x?utm_a=1&utm_b=2#top") ==
          std::optional<std::string>("https://a.com/x#top"));
    CHECK(norm("https://a.com/#top") == std::optional<std::string>("https://a.com#top"));
    // "utm" без подчеркивания - не метка
    CHECK(norm("https://a.com/?utmost=1") == std::optional<std::string>("https://a.com?utmost=1"));

    CHECK(norm("https://User@Host.com:443/p/") ==
          std::optional<std::string>("https://User@host.com/p"));
    CHECK(norm("http://[::1]:80/x/") == std::optional<std::string>("http://[::1]/x"));
    CHECK(norm("http://[FE80::1]:8080") == std::optional<std::string>("http://[fe80::1]:8080"));

    CHECK(norm("http://a.com/x/") == norm("HTTP://A.com/x?utm_campaign=z"));
    CHECK(norm("http://a.com/x") != norm("https://a.com/x"));
    CHECK(norm("http://a.com/x?a=1&b=2") != norm("http://a.com/x?b=2&a=1"));

    for (const std::string_view bad :
         {"", "example.com", "ftp://a.com", "javascript:alert(1)", "http://", "http://a b"}) {
        INFO(bad);
        CHECK_FALSE(norm(bad).has_value());
    }
}

TEST_CASE("shortcutUrl keeps working through isHttpUrl", "[rules][url]") {
    const auto url = [](std::string_view text) {
        return domain::shortcutUrl(domain::asBytes(text));
    };
    CHECK(url("[InternetShortcut]\r\nURL=https://example.com/a\r\n") ==
          std::optional<std::string>("https://example.com/a"));
    CHECK_FALSE(url("[InternetShortcut]\r\nURL=javascript:alert(1)\r\n").has_value());
    CHECK_FALSE(url("[InternetShortcut]\r\nURL=http://\r\n").has_value());
    CHECK_FALSE(url("[InternetShortcut]\r\nURL=http://a b\r\n").has_value());
    CHECK_FALSE(url("[InternetShortcut]\r\n").has_value());
}

TEST_CASE("a link without content is handed out as a .url shortcut", "[rules][url]") {
    CHECK(domain::shortcutFileName("Мой сайт") == "Мой сайт.url");
    CHECK(domain::shortcutFileName("example.com") ==
          "example.com.url"); // ".com" - не расширение ярлыка
    CHECK(domain::shortcutFileName("site.url") == "site.url");
    CHECK(domain::shortcutFileName("SITE.URL") == "SITE.URL"); // регистр не важен
    CHECK(domain::shortcutFileName("a.url.txt") == "a.url.txt.url");
    CHECK(domain::shortcutFileName(".url") == ".url.url"); // это имя без расширения

    const auto content = domain::shortcutContent("https://example.com/a?b=1");
    CHECK(content == "[InternetShortcut]\r\nURL=https://example.com/a?b=1\r\n");
    // ярлык читается обратно тем же разбором, каким его читает импорт
    const auto back = domain::shortcutUrl(domain::asBytes(content));
    REQUIRE(back.has_value());
    CHECK(*back == "https://example.com/a?b=1");
}
