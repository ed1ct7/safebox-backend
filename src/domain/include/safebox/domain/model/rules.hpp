// Правила домена: проверка паролей, имен и тегов, тип файла по сигнатуре, разбор .url ярлыков,
// адреса ссылок
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "safebox/domain/io.hpp"
#include "safebox/domain/model/entry.hpp"
#include "safebox/domain/model/error.hpp"
#include "safebox/domain/model/safe_format.hpp"

namespace safebox::domain {

inline constexpr std::size_t kMinPasswordLength = 6;
inline constexpr std::size_t kMaxPasswordBytes = 1024;
inline constexpr std::size_t kMaxNameBytes = 255;
inline constexpr std::size_t kMaxUrlBytes = 8192;
inline constexpr std::size_t kMaxTagNameChars = 100;
inline constexpr std::size_t kMaxDescriptionBytes = 64 * 1024;
inline constexpr std::size_t kMaxTagsPerEntry = 1000;
// Сколько первых байтов нужно detectKind (сигнатуры + ярлык .url).
inline constexpr std::size_t kSniffBytes = 64 * 1024;

namespace detail {

[[nodiscard]] constexpr char asciiLower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

[[nodiscard]] constexpr bool iequals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (asciiLower(a[i]) != asciiLower(b[i])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] constexpr bool istartsWith(std::string_view s, std::string_view prefix) noexcept {
    return s.size() >= prefix.size() && iequals(s.substr(0, prefix.size()), prefix);
}

[[nodiscard]] constexpr bool isSpace(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

[[nodiscard]] constexpr std::string_view trim(std::string_view s) noexcept {
    while (!s.empty() && isSpace(s.front())) {
        s.remove_prefix(1);
    }
    while (!s.empty() && isSpace(s.back())) {
        s.remove_suffix(1);
    }
    return s;
}

// Длина корректной UTF-8 последовательности, начинающейся в s[i]; 0 - ошибка.
[[nodiscard]] constexpr std::size_t utf8SequenceLength(std::string_view s, std::size_t i) noexcept {
    const auto b0 = static_cast<unsigned char>(s[i]);
    if (b0 < 0x80) {
        return 1;
    }
    std::size_t len = 0;
    std::uint32_t cp = 0;
    if ((b0 & 0xE0) == 0xC0) {
        len = 2;
        cp = b0 & 0x1Fu;
    } else if ((b0 & 0xF0) == 0xE0) {
        len = 3;
        cp = b0 & 0x0Fu;
    } else if ((b0 & 0xF8) == 0xF0) {
        len = 4;
        cp = b0 & 0x07u;
    } else {
        return 0;
    }
    if (i + len > s.size()) {
        return 0;
    }
    for (std::size_t k = 1; k < len; ++k) {
        const auto b = static_cast<unsigned char>(s[i + k]);
        if ((b & 0xC0) != 0x80) {
            return 0;
        }
        cp = (cp << 6) | (b & 0x3Fu);
    }
    // overlong, суррогаты, > U+10FFFF
    if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000) ||
        (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) {
        return 0;
    }
    return len;
}

[[nodiscard]] constexpr bool isForbiddenNameChar(char c) noexcept {
    const auto u = static_cast<unsigned char>(c);
    if (u < 0x20 || u == 0x7F) {
        return true;
    }
    switch (c) {
    case '/':
    case '\\':
    case ':':
    case '*':
    case '?':
    case '"':
    case '<':
    case '>':
    case '|':
        return true;
    default:
        return false;
    }
}

[[nodiscard]] inline bool startsWithBytes(std::span<const std::byte> data, std::size_t offset,
                                          std::string_view sig) noexcept {
    if (data.size() < offset + sig.size()) {
        return false;
    }
    for (std::size_t i = 0; i < sig.size(); ++i) {
        if (data[offset + i] != static_cast<std::byte>(sig[i])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline std::string_view extensionOf(std::string_view name) noexcept {
    const auto dot = name.rfind('.');
    if (dot == std::string_view::npos || dot == 0) {
        return {};
    }
    return name.substr(dot);
}

} // namespace detail

[[nodiscard]] constexpr bool isValidUtf8(std::string_view s) noexcept {
    for (std::size_t i = 0; i < s.size();) {
        const auto len = detail::utf8SequenceLength(s, i);
        if (len == 0) {
            return false;
        }
        i += len;
    }
    return true;
}

// Число символов (code points) корректной UTF-8 строки.
[[nodiscard]] constexpr std::size_t utf8Length(std::string_view s) noexcept {
    std::size_t n = 0;
    for (const char c : s) {
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) {
            ++n;
        }
    }
    return n;
}

[[nodiscard]] inline Status validatePassword(std::string_view password) {
    if (password.size() > kMaxPasswordBytes) {
        return fail(Error::Code::InvalidArgument, "Слишком длинный пароль");
    }
    if (!isValidUtf8(password)) {
        return fail(Error::Code::InvalidArgument, "Пароль содержит некорректные символы");
    }
    if (utf8Length(password) < kMinPasswordLength) {
        return fail(Error::Code::InvalidArgument, "Пароль должен содержать не меньше 6 символов");
    }
    return {};
}

[[nodiscard]] inline Status validateNewPassword(std::string_view password,
                                                std::string_view repeat) {
    if (auto st = validatePassword(password); !st) {
        return st;
    }
    if (password != repeat) {
        return fail(Error::Code::InvalidArgument, "Пароли не совпадают");
    }
    return {};
}

[[nodiscard]] inline Status validateName(std::string_view name) {
    if (name.empty()) {
        return fail(Error::Code::InvalidArgument, "Имя не может быть пустым");
    }
    if (name.size() > kMaxNameBytes) {
        return fail(Error::Code::InvalidArgument, "Слишком длинное имя");
    }
    if (!isValidUtf8(name)) {
        return fail(Error::Code::InvalidArgument, "Имя содержит некорректные символы");
    }
    if (detail::isSpace(name.front()) || detail::isSpace(name.back())) {
        return fail(Error::Code::InvalidArgument,
                    "Имя не может начинаться или заканчиваться пробелом");
    }
    if (name == "." || name == "..") {
        return fail(Error::Code::InvalidArgument, "Недопустимое имя");
    }
    for (const char c : name) {
        if (detail::isForbiddenNameChar(c)) {
            return fail(Error::Code::InvalidArgument,
                        "Имя не может содержать символы / \\ : * ? \" < > |");
        }
    }
    return {};
}

// Название категории или тега: пробелы по краям срезаются, результат - очищенное имя.
// ":" запрещен - по нему фронтенд разделяет "категория:тег".
[[nodiscard]] inline Result<std::string> validateTagName(std::string_view name) {
    const auto clean = detail::trim(name);
    if (clean.empty()) {
        return fail(Error::Code::InvalidArgument, "Название не может быть пустым");
    }
    if (!isValidUtf8(clean)) {
        return fail(Error::Code::InvalidArgument, "Название содержит некорректные символы");
    }
    if (utf8Length(clean) > kMaxTagNameChars) {
        return fail(Error::Code::InvalidArgument, "Название длиннее 100 символов");
    }
    for (std::size_t i = 0; i < clean.size(); ++i) {
        const auto u = static_cast<unsigned char>(clean[i]);
        const bool c1 = u == 0xC2 && i + 1 < clean.size() &&
                        static_cast<unsigned char>(clean[i + 1]) >= 0x80 &&
                        static_cast<unsigned char>(clean[i + 1]) <= 0x9F;
        if (u < 0x20 || u == 0x7F || clean[i] == ':' || c1) {
            return fail(Error::Code::InvalidArgument,
                        "Название не может содержать двоеточие и управляющие символы");
        }
    }
    return std::string(clean);
}

// Имя из импорта: запрещенные символы и битый UTF-8 -> '_', пробелы по краям
// срезаются, длина режется по границе символа. Результат всегда проходит
// validateName.
[[nodiscard]] inline std::string sanitizeName(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());
    for (std::size_t i = 0; i < raw.size();) {
        const auto len = detail::utf8SequenceLength(raw, i);
        if (len == 0) {
            out.push_back('_');
            ++i;
            continue;
        }
        if (len == 1 && detail::isForbiddenNameChar(raw[i])) {
            out.push_back('_');
        } else {
            out.append(raw.substr(i, len));
        }
        i += len;
    }
    std::string_view view = detail::trim(out);
    if (view.size() > kMaxNameBytes) {
        std::size_t cut = kMaxNameBytes;
        while (cut > 0 && (static_cast<unsigned char>(view[cut]) & 0xC0) == 0x80) {
            --cut;
        }
        view = detail::trim(view.substr(0, cut));
    }
    if (view.empty() || view == "." || view == "..") {
        return "без имени";
    }
    return std::string(view);
}

struct DetectedKind {
    Kind kind = Kind::File;
    std::string_view mime = "application/octet-stream";
};

// Адрес ссылки: http/https, без пробелов и управляющих символов, с хостом.
[[nodiscard]] inline bool isHttpUrl(std::string_view url) {
    if (url.empty() || url.size() > kMaxUrlBytes || !isValidUtf8(url)) {
        return false;
    }
    for (const char c : url) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7F || c == ' ') {
            return false;
        }
    }
    std::string_view rest;
    if (detail::istartsWith(url, "http://")) {
        rest = url.substr(7);
    } else if (detail::istartsWith(url, "https://")) {
        rest = url.substr(8);
    } else {
        return false;
    }
    return !rest.empty() && rest.front() != '/' && rest.front() != '?' && rest.front() != '#';
}

// URL из ярлыка Windows (.url): секция [InternetShortcut], строка URL=...
// Только http/https - иначе это не ссылка (javascript:, file: и т.п.).
[[nodiscard]] inline std::optional<std::string> shortcutUrl(std::span<const std::byte> content) {
    std::string_view text = asChars(content);
    if (text.starts_with("\xEF\xBB\xBF")) {
        text.remove_prefix(3);
    }
    if (!detail::istartsWith(detail::trim(text), "[InternetShortcut]")) {
        return std::nullopt;
    }
    bool inSection = false;
    while (!text.empty()) {
        const auto eol = text.find('\n');
        auto line = detail::trim(text.substr(0, eol));
        text = eol == std::string_view::npos ? std::string_view{} : text.substr(eol + 1);
        if (line.starts_with('[')) {
            inSection = detail::iequals(line, "[InternetShortcut]");
            continue;
        }
        if (!inSection || !detail::istartsWith(line, "URL=")) {
            continue;
        }
        const auto url = detail::trim(line.substr(4));
        if (!isHttpUrl(url)) {
            return std::nullopt;
        }
        return std::string(url);
    }
    return std::nullopt;
}

// Ссылка без блоба отдается ярлыком, как его записал бы проводник: файл "имя.url" (расширение не
// дублируется) с секцией [InternetShortcut].
[[nodiscard]] inline std::string shortcutFileName(std::string_view name) {
    std::string out(name);
    if (!detail::iequals(detail::extensionOf(name), ".url")) {
        out += ".url";
    }
    return out;
}

[[nodiscard]] inline std::string shortcutContent(std::string_view url) {
    return "[InternetShortcut]\r\nURL=" + std::string(url) + "\r\n";
}

// Хост из http(s)-URL в нижнем регистре, без userinfo и порта.
[[nodiscard]] inline std::string urlHost(std::string_view url) {
    const auto scheme = url.find("://");
    if (scheme == std::string_view::npos) {
        return {};
    }
    auto rest = url.substr(scheme + 3);
    rest = rest.substr(0, rest.find_first_of("/?#"));
    if (const auto at = rest.rfind('@'); at != std::string_view::npos) {
        rest = rest.substr(at + 1);
    }
    if (rest.starts_with('[')) {
        rest = rest.substr(0, rest.find(']') + 1);
    } else {
        rest = rest.substr(0, rest.find(':'));
    }
    std::string host(rest);
    for (auto& c : host) {
        c = detail::asciiLower(c);
    }
    return host;
}

// Вид адреса для сравнения ссылок на дубли: схема и хост в нижнем регистре, порт по
// умолчанию, завершающий "/" пути и utm_-параметры убраны, остальное как есть. nullopt - не
// http(s).
[[nodiscard]] inline std::optional<std::string> normalizeUrl(std::string_view url) {
    if (!isHttpUrl(url)) {
        return std::nullopt;
    }
    const auto schemeEnd = url.find("://");
    std::string scheme(url.substr(0, schemeEnd));
    for (auto& c : scheme) {
        c = detail::asciiLower(c);
    }
    auto rest = url.substr(schemeEnd + 3);

    std::string_view fragment;
    if (const auto hash = rest.find('#'); hash != std::string_view::npos) {
        fragment = rest.substr(hash);
        rest = rest.substr(0, hash);
    }
    std::string_view query;
    if (const auto mark = rest.find('?'); mark != std::string_view::npos) {
        query = rest.substr(mark + 1);
        rest = rest.substr(0, mark);
    }
    auto hostPort = rest.substr(0, rest.find('/'));
    auto path = rest.substr(hostPort.size());
    std::string_view userinfo;
    if (const auto at = hostPort.rfind('@'); at != std::string_view::npos) {
        userinfo = hostPort.substr(0, at + 1);
        hostPort = hostPort.substr(at + 1);
    }
    std::string_view host = hostPort;
    std::string_view port;
    const auto colon =
        hostPort.starts_with('[') ? hostPort.find(':', hostPort.find(']')) : hostPort.find(':');
    if (colon != std::string_view::npos) {
        host = hostPort.substr(0, colon);
        port = hostPort.substr(colon + 1);
    }
    if (port == (scheme == "https" ? "443" : "80")) {
        port = {};
    }
    if (path.ends_with('/')) {
        path.remove_suffix(1);
    }

    std::string out = scheme + "://";
    out.append(userinfo);
    for (const char c : host) {
        out.push_back(detail::asciiLower(c));
    }
    if (!port.empty()) {
        out.push_back(':');
        out.append(port);
    }
    out.append(path);
    std::string kept;
    while (!query.empty()) {
        const auto amp = query.find('&');
        const auto param = query.substr(0, amp);
        query = amp == std::string_view::npos ? std::string_view{} : query.substr(amp + 1);
        if (!detail::istartsWith(param, "utm_")) {
            kept.append(kept.empty() ? "" : "&").append(param);
        }
    }
    if (!kept.empty()) {
        out.append("?").append(kept);
    }
    out.append(fragment);
    return out;
}

[[nodiscard]] inline DetectedKind detectKind(std::span<const std::byte> head,
                                             std::string_view name) {
    using detail::startsWithBytes;
    const auto ext = detail::extensionOf(name);
    const auto hasExt = [&](std::string_view e) { return detail::iequals(ext, e); };

    // изображения, которые браузер покажет в лайтбоксе
    if (startsWithBytes(head, 0, "\xFF\xD8\xFF")) {
        return {Kind::Photo, "image/jpeg"};
    }
    if (startsWithBytes(head, 0, "\x89PNG\r\n\x1A\n")) {
        return {Kind::Photo, "image/png"};
    }
    if (startsWithBytes(head, 0, "GIF87a") || startsWithBytes(head, 0, "GIF89a")) {
        return {Kind::Photo, "image/gif"};
    }
    if (startsWithBytes(head, 0, "RIFF") && startsWithBytes(head, 8, "WEBP")) {
        return {Kind::Photo, "image/webp"};
    }
    if (startsWithBytes(head, 0, "BM") && head.size() >= 18) {
        // заголовок BMP слабый ("BM"), подтверждаем размером DIB-заголовка
        const auto dib = static_cast<unsigned>(head[14]) | (static_cast<unsigned>(head[15]) << 8);
        if (dib == 12 || dib == 40 || dib == 52 || dib == 56 || dib == 108 || dib == 124) {
            return {Kind::Photo, "image/bmp"};
        }
    }

    // ISO BMFF (mp4/mov/avif/heic): 'ftyp' со смещения 4
    if (startsWithBytes(head, 4, "ftyp") && head.size() >= 12) {
        const auto brand = asChars(head.subspan(8, 4));
        if (brand == "avif" || brand == "avis") {
            return {Kind::Photo, "image/avif"};
        }
        if (brand == "heic" || brand == "heix" || brand == "hevc" || brand == "hevx" ||
            brand == "heim" || brand == "heis" || brand == "mif1" || brand == "msf1") {
            return {Kind::File, "image/heic"}; // браузеры не показывают HEIC
        }
        if (brand == "M4A " || brand == "M4B " || brand == "M4P ") {
            return {Kind::File, "audio/mp4"};
        }
        if (brand == "qt  ") {
            return {Kind::Video, "video/quicktime"};
        }
        if (brand.starts_with("3gp") || brand.starts_with("3g2")) {
            return {Kind::Video, "video/3gpp"};
        }
        return {Kind::Video, "video/mp4"};
    }
    // Matroska / WebM
    if (startsWithBytes(head, 0, "\x1A\x45\xDF\xA3")) {
        const auto sniff = asChars(head.first(std::min<std::size_t>(head.size(), 4096)));
        if (sniff.find("webm") != std::string_view::npos) {
            return {Kind::Video, "video/webm"};
        }
        return {Kind::Video, "video/x-matroska"};
    }

    // ссылки: ярлык Windows .url с http(s)-адресом
    if (shortcutUrl(head).has_value()) {
        return {Kind::Link, "application/internet-shortcut"};
    }

    // прочие файлы: mime только информационный (отдаются как attachment)
    if (startsWithBytes(head, 0, "%PDF-")) {
        return {Kind::File, "application/pdf"};
    }
    if (startsWithBytes(head, 0, "PK\x03\x04")) {
        return {Kind::File, "application/zip"};
    }
    if (hasExt(".txt") || hasExt(".md") || hasExt(".log") || hasExt(".csv")) {
        return {Kind::File, "text/plain"};
    }
    if (hasExt(".mp3")) {
        return {Kind::File, "audio/mpeg"};
    }
    return {Kind::File, "application/octet-stream"};
}

// "расширение .safebox добавляется автоматически".
[[nodiscard]] inline std::filesystem::path withSafeExtension(std::filesystem::path path) {
    const auto ext = pathToUtf8(path.extension());
    if (!detail::iequals(ext, kSafeExtension)) {
        path += pathFromUtf8(kSafeExtension);
    }
    return path;
}

} // namespace safebox::domain
