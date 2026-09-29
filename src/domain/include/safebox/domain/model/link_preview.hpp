// Разбор HTML страницы для предпросмотра ссылки: заголовок, описание, адреса картинок.
// Чистые функции: сеть и картинки - забота вызывающего
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "safebox/domain/model/rules.hpp"

namespace safebox::domain {

// Дальше этого в HTML не смотрим (и раньше, если встретился </head> или <body>).
inline constexpr std::size_t kPreviewScanBytes = 512 * 1024;
// Сколько адресов картинок возвращаем (и собираем на каждый приоритет): пробовать десятки
// кандидатов незачем.
inline constexpr std::size_t kMaxPreviewImages = 8;

struct LinkPreview {
    std::string title;
    std::string description;
    // Абсолютные http(s)-адреса без повторов: og:image, twitter:image, apple-touch-icon, icon.
    std::vector<std::string> imageUrls;
};

namespace detail {

struct NamedEntity {
    std::string_view name;
    std::uint32_t codePoint;
};

// nbsp сразу пробел: так его ждут в заголовках.
inline constexpr std::array<NamedEntity, 21> kNamedEntities{{
    {"amp", '&'},     {"lt", '<'},       {"gt", '>'},       {"quot", '"'},      {"apos", '\''},
    {"nbsp", ' '},    {"mdash", 0x2014}, {"ndash", 0x2013}, {"hellip", 0x2026}, {"laquo", 0xAB},
    {"raquo", 0xBB},  {"ldquo", 0x201C}, {"rdquo", 0x201D}, {"lsquo", 0x2018},  {"rsquo", 0x2019},
    {"copy", 0xA9},   {"reg", 0xAE},     {"trade", 0x2122}, {"bull", 0x2022},   {"middot", 0xB7},
    {"euro", 0x20AC},
}};
// Самая длинная сущность между '&' и ';': "#x10FFFF".
inline constexpr std::size_t kMaxEntityLength = 8;

[[nodiscard]] inline std::string toLowerAscii(std::string_view s) {
    std::string out(s);
    for (auto& c : out) {
        c = asciiLower(c);
    }
    return out;
}

[[nodiscard]] constexpr bool isAsciiLetter(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

[[nodiscard]] constexpr bool isAsciiDigit(char c) noexcept {
    return c >= '0' && c <= '9';
}

[[nodiscard]] constexpr bool isHtmlNameChar(char c) noexcept {
    return isAsciiLetter(c) || isAsciiDigit(c) || c == '-' || c == ':' || c == '_';
}

inline void appendUtf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// "123" или "x1F" (без "&#" и ";") -> code point; нуль, суррогаты и > U+10FFFF - ошибка.
[[nodiscard]] inline std::optional<std::uint32_t> parseNumericEntity(std::string_view digits) {
    const bool hex = !digits.empty() && (digits.front() == 'x' || digits.front() == 'X');
    if (hex) {
        digits.remove_prefix(1);
    }
    if (digits.empty() || digits.size() > (hex ? 6u : 7u)) {
        return std::nullopt;
    }
    std::uint32_t value = 0;
    for (const char c : digits) {
        std::uint32_t digit = 0;
        if (isAsciiDigit(c)) {
            digit = static_cast<std::uint32_t>(c - '0');
        } else if (hex && c >= 'a' && c <= 'f') {
            digit = static_cast<std::uint32_t>(c - 'a' + 10);
        } else if (hex && c >= 'A' && c <= 'F') {
            digit = static_cast<std::uint32_t>(c - 'A' + 10);
        } else {
            return std::nullopt;
        }
        value = value * (hex ? 16u : 10u) + digit;
    }
    if (value == 0 || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF)) {
        return std::nullopt;
    }
    return value;
}

// Битые и неизвестные сущности остаются как есть.
[[nodiscard]] inline std::string decodeHtmlEntities(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') {
            out.push_back(s[i]);
            continue;
        }
        // окно ограничено, иначе строка из одних '&' даст квадратичное время
        const auto semi = s.substr(i + 1, kMaxEntityLength + 1).find(';');
        if (semi != std::string_view::npos) {
            const auto body = s.substr(i + 1, semi);
            std::optional<std::uint32_t> cp;
            if (body.starts_with('#')) {
                cp = parseNumericEntity(body.substr(1));
            } else {
                for (const auto& entity : kNamedEntities) {
                    if (entity.name == body) {
                        cp = entity.codePoint;
                        break;
                    }
                }
            }
            if (cp) {
                appendUtf8(out, *cp);
                i += semi + 1;
                continue;
            }
        }
        out.push_back('&');
    }
    return out;
}

[[nodiscard]] inline std::string replaceInvalidUtf8(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size();) {
        const auto len = utf8SequenceLength(s, i);
        if (len == 0) {
            out.push_back('?');
            ++i;
        } else {
            out.append(s.substr(i, len));
            i += len;
        }
    }
    return out;
}

// Пробельные (в том числе U+00A0) подряд -> один пробел, по краям срезать,
// управляющие символы выбросить. Вход - корректный UTF-8.
[[nodiscard]] inline std::string collapseSpaces(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    bool pendingSpace = false;
    for (std::size_t i = 0, len = 1; i < s.size(); i += len) {
        len = std::max<std::size_t>(utf8SequenceLength(s, i), 1);
        const auto ch = s.substr(i, len);
        if (ch == "\xC2\xA0" || (len == 1 && isSpace(ch[0]))) {
            pendingSpace = !out.empty();
            continue;
        }
        if (len == 1 && (static_cast<unsigned char>(ch[0]) < 0x20 || ch[0] == 0x7F)) {
            continue;
        }
        if (pendingSpace) {
            out.push_back(' ');
            pendingSpace = false;
        }
        out.append(ch);
    }
    return out;
}

// Текст элемента или значение атрибута -> строка для показа.
[[nodiscard]] inline std::string cleanHtmlText(std::string_view raw) {
    return collapseSpaces(decodeHtmlEntities(replaceInvalidUtf8(raw)));
}

inline void truncateUtf8(std::string& s, std::size_t maxBytes) {
    if (s.size() <= maxBytes) {
        return;
    }
    std::size_t cut = maxBytes;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) {
        --cut;
    }
    s.resize(cut);
    while (!s.empty() && s.back() == ' ') {
        s.pop_back();
    }
}


// "mailto:x", "data:...", "C:\x": начало похоже на схему URL.
[[nodiscard]] inline bool startsWithUrlScheme(std::string_view s) {
    if (s.empty() || !isAsciiLetter(s.front())) {
        return false;
    }
    for (const char c : s.substr(1)) {
        if (c == ':') {
            return true;
        }
        if (!isAsciiLetter(c) && !isAsciiDigit(c) && c != '+' && c != '-' && c != '.') {
            return false;
        }
    }
    return false;
}

// RFC 3986, 5.2.4. path начинается с '/'.
[[nodiscard]] inline std::string removeDotSegments(std::string_view path) {
    std::vector<std::string_view> kept;
    bool endsWithSlash = path.size() == 1;
    for (std::size_t i = 1; i < path.size();) {
        const auto slash = path.find('/', i);
        const auto end = slash == std::string_view::npos ? path.size() : slash;
        const auto segment = path.substr(i, end - i);
        const bool last = slash == std::string_view::npos;
        endsWithSlash = !last && slash + 1 == path.size();
        if (segment == "..") {
            if (!kept.empty()) {
                kept.pop_back();
            }
            endsWithSlash = endsWithSlash || last;
        } else if (segment == ".") {
            endsWithSlash = endsWithSlash || last;
        } else {
            kept.push_back(segment);
        }
        i = last ? path.size() : slash + 1;
    }
    std::string out;
    for (const auto segment : kept) {
        out.push_back('/');
        out.append(segment);
    }
    if (kept.empty() || endsWithSlash) {
        out.push_back('/');
    }
    return out;
}

// url - абсолютный http(s)
[[nodiscard]] inline std::string normalizeUrlPath(std::string_view url) {
    const auto authorityEnd = url.find_first_of("/?#", url.find("://") + 3);
    if (authorityEnd == std::string_view::npos || url[authorityEnd] != '/') {
        return std::string(url);
    }
    const auto pathEnd = std::min(url.find_first_of("?#", authorityEnd), url.size());
    std::string out(url.substr(0, authorityEnd));
    out += removeDotSegments(url.substr(authorityEnd, pathEnd - authorityEnd));
    out.append(url.substr(pathEnd));
    return out;
}

// Абсолютный http(s)-адрес для ссылки reference (`//host/x`, `/x`, `x`, `../x`, `?q`, `#f`)
// на странице baseUrl; пусто, если получился не http(s) или baseUrl не годится.
// Значение уже без HTML-сущностей.
[[nodiscard]] inline std::string resolveUrl(std::string_view baseUrl, std::string_view reference) {
    std::string cleaned; // табы и переводы строк внутри адреса браузеры выбрасывают
    for (const char c : reference) {
        if (c != '\t' && c != '\r' && c != '\n') {
            cleaned.push_back(c);
        }
    }
    const auto ref = trim(cleaned);
    std::string url;
    if (istartsWith(ref, "http://") || istartsWith(ref, "https://")) {
        url = ref;
    } else if (ref.empty() || startsWithUrlScheme(ref) || !isHttpUrl(baseUrl)) {
        return {};
    } else {
        const auto base = baseUrl.substr(0, baseUrl.find('#'));
        const auto authorityEnd =
            std::min(base.find_first_of("/?", base.find("://") + 3), base.size());
        const auto origin = base.substr(0, authorityEnd);
        const auto path = base.substr(
            authorityEnd, std::min(base.find('?', authorityEnd), base.size()) - authorityEnd);
        if (ref.starts_with("//")) {
            url = std::string(base.substr(0, base.find(':') + 1)) + std::string(ref);
        } else if (ref.front() == '/') {
            url = std::string(origin) + std::string(ref);
        } else if (ref.front() == '?') {
            url = std::string(origin) + std::string(path) + std::string(ref);
        } else if (ref.front() == '#') {
            url = std::string(base) + std::string(ref);
        } else {
            const auto directory = path.substr(0, path.rfind('/') + 1);
            url = std::string(origin) + (directory.empty() ? "/" : std::string(directory)) +
                  std::string(ref);
        }
    }
    url = normalizeUrlPath(url);
    return isHttpUrl(url) ? url : std::string{};
}

struct HtmlAttribute {
    std::string name; // в нижнем регистре
    std::string_view value;
};

struct HtmlTag {
    std::string name; // в нижнем регистре
    std::vector<HtmlAttribute> attributes;
    std::size_t end = 0; // сразу за '>'
    bool closed = false; // false: '>' нет или не закрыта кавычка

    [[nodiscard]] std::optional<std::string_view> attribute(std::string_view wanted) const {
        for (const auto& attr : attributes) {
            if (attr.name == wanted) {
                return attr.value;
            }
        }
        return std::nullopt;
    }
};

// html[lt] == '<', дальше буква. Значения - срезы html, без декодирования.
[[nodiscard]] inline HtmlTag parseHtmlTag(std::string_view html, std::size_t lt) {
    HtmlTag tag;
    std::size_t i = lt + 1;
    while (i < html.size() && !isSpace(html[i]) && html[i] != '/' && html[i] != '>') {
        tag.name.push_back(asciiLower(html[i++]));
    }
    const auto skipSpaces = [&] {
        while (i < html.size() && isSpace(html[i])) {
            ++i;
        }
    };
    while (i < html.size()) {
        while (i < html.size() && (isSpace(html[i]) || html[i] == '/')) {
            ++i;
        }
        if (i >= html.size()) {
            break;
        }
        if (html[i] == '>') {
            tag.end = i + 1;
            tag.closed = true;
            return tag;
        }
        const auto nameStart = i++; // первый символ - в имя всегда, иначе можно не сдвинуться
        while (i < html.size() && !isSpace(html[i]) && html[i] != '=' && html[i] != '>' &&
               html[i] != '/') {
            ++i;
        }
        HtmlAttribute attr{toLowerAscii(html.substr(nameStart, i - nameStart)), {}};
        skipSpaces();
        if (i < html.size() && html[i] == '=') {
            ++i;
            skipSpaces();
            if (i < html.size() && (html[i] == '"' || html[i] == '\'')) {
                const char quote = html[i++];
                const auto close = html.find(quote, i);
                if (close == std::string_view::npos) {
                    break;
                }
                attr.value = html.substr(i, close - i);
                i = close + 1;
            } else {
                const auto valueStart = i;
                while (i < html.size() && !isSpace(html[i]) && html[i] != '>') {
                    ++i;
                }
                attr.value = html.substr(valueStart, i - valueStart);
            }
        }
        tag.attributes.push_back(std::move(attr));
    }
    tag.end = html.size();
    return tag;
}

// Позиция "</name" без учета регистра.
[[nodiscard]] inline std::size_t findClosingTag(std::string_view html, std::size_t from,
                                                std::string_view name) {
    for (auto i = html.find("</", from); i != std::string_view::npos; i = html.find("</", i + 2)) {
        if (istartsWith(html.substr(i + 2), name)) {
            return i;
        }
    }
    return std::string_view::npos;
}

// "</name" на позиции at (сразу за "</") и это целое имя, а не "</header" для "head".
[[nodiscard]] inline bool closesElement(std::string_view html, std::size_t at,
                                        std::string_view name) {
    const auto rest = html.substr(at);
    return istartsWith(rest, name) &&
           (rest.size() == name.size() || !isHtmlNameChar(rest[name.size()]));
}

} // namespace detail

// Заголовок, описание и адреса картинок из <head> страницы. baseUrl - адрес, с которого
// страница получена (после редиректов): от него считаются относительные адреса.
[[nodiscard]] inline LinkPreview parseLinkPreview(std::string_view html, std::string_view baseUrl) {
    html = html.substr(0, kPreviewScanBytes);

    std::string pageTitle, ogTitle, twitterTitle;
    std::string metaDescription, ogDescription, twitterDescription;
    std::array<std::vector<std::string>, 4> images; // по приоритету, как в LinkPreview
    bool titleClosable = true;                      // false: дальше </title> нет

    const auto keep = [](std::string& slot, std::string_view raw) {
        if (slot.empty()) {
            slot = detail::cleanHtmlText(raw);
        }
    };
    const auto addImage = [&](std::size_t rank, std::string_view raw) {
        if (images[rank].size() >= kMaxPreviewImages) {
            return;
        }
        auto url = detail::resolveUrl(baseUrl, detail::decodeHtmlEntities(raw));
        if (!url.empty()) {
            images[rank].push_back(std::move(url));
        }
    };
    const auto applyMeta = [&](std::string_view rawKey, std::string_view content) {
        const auto key = detail::toLowerAscii(detail::trim(rawKey));
        if (key == "og:title") {
            keep(ogTitle, content);
        } else if (key == "twitter:title") {
            keep(twitterTitle, content);
        } else if (key == "og:description") {
            keep(ogDescription, content);
        } else if (key == "twitter:description") {
            keep(twitterDescription, content);
        } else if (key == "description") {
            keep(metaDescription, content);
        } else if (key == "og:image") {
            addImage(0, content);
        } else if (key == "twitter:image") {
            addImage(1, content);
        }
    };

    std::size_t pos = 0;
    while (pos < html.size()) {
        const auto lt = html.find('<', pos);
        if (lt == std::string_view::npos || lt + 1 >= html.size()) {
            break;
        }
        const char next = html[lt + 1];
        if (html.substr(lt).starts_with("<!--")) {
            const auto close = html.find("-->", lt + 4);
            if (close == std::string_view::npos) {
                break;
            }
            pos = close + 3;
        } else if (next == '!' || next == '?') {
            const auto close = html.find('>', lt + 2);
            if (close == std::string_view::npos) {
                break;
            }
            pos = close + 1;
        } else if (next == '/') {
            if (detail::closesElement(html, lt + 2, "head")) {
                break;
            }
            pos = lt + 2;
        } else if (detail::isAsciiLetter(next)) {
            const auto tag = detail::parseHtmlTag(html, lt);
            if (!tag.closed || tag.name == "body") {
                break;
            }
            pos = tag.end;
            if (tag.name == "script" || tag.name == "style") {
                const auto close = detail::findClosingTag(html, pos, tag.name);
                if (close == std::string_view::npos) {
                    break;
                }
                pos = close;
            } else if (tag.name == "title" && pageTitle.empty()) {
                auto close = titleClosable ? detail::findClosingTag(html, pos, "title")
                                           : std::string_view::npos;
                if (close == std::string_view::npos) {
                    titleClosable = false;
                    close = std::min(html.find('<', pos), html.size());
                }
                pageTitle = detail::cleanHtmlText(html.substr(pos, close - pos));
                pos = close;
            } else if (tag.name == "meta") {
                if (const auto content = tag.attribute("content")) {
                    for (const auto key : {tag.attribute("property"), tag.attribute("name")}) {
                        if (key) {
                            applyMeta(*key, *content);
                        }
                    }
                }
            } else if (tag.name == "link") {
                const auto rel = tag.attribute("rel");
                const auto href = tag.attribute("href");
                if (!rel || !href) {
                    continue;
                }
                bool apple = false;
                bool icon = false;
                for (auto tokens = *rel; !tokens.empty();) {
                    tokens = detail::trim(tokens);
                    const auto end = tokens.find_first_of(" \t\r\n\f\v");
                    const auto token = detail::toLowerAscii(tokens.substr(0, end));
                    apple = apple || token.starts_with("apple-touch-icon");
                    icon = icon || token == "icon";
                    tokens =
                        end == std::string_view::npos ? std::string_view{} : tokens.substr(end);
                }
                if (apple) {
                    addImage(2, *href);
                } else if (icon) {
                    addImage(3, *href);
                }
            }
        } else {
            pos = lt + 1;
        }
    }

    LinkPreview preview;
    for (auto* title : {&ogTitle, &twitterTitle, &pageTitle}) {
        if (!title->empty()) {
            preview.title = std::move(*title);
            break;
        }
    }
    for (auto* description : {&ogDescription, &twitterDescription, &metaDescription}) {
        if (!description->empty()) {
            preview.description = std::move(*description);
            break;
        }
    }
    detail::truncateUtf8(preview.description, kMaxDescriptionBytes);
    for (auto& group : images) {
        for (auto& url : group) {
            if (preview.imageUrls.size() < kMaxPreviewImages &&
                std::ranges::find(preview.imageUrls, url) == preview.imageUrls.end()) {
                preview.imageUrls.push_back(std::move(url));
            }
        }
    }
    return preview;
}

} // namespace safebox::domain
