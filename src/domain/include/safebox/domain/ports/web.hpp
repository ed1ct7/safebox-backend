// Загрузка веб-страниц для предпросмотра ссылок. Любой сбой (сеть, статус не 2xx,
// внутренний адрес) - ошибка с причиной по-русски, наружу код IoError
#pragma once

#include <chrono>
#include <cstddef>
#include <string>

#include "safebox/domain/io.hpp"
#include "safebox/domain/model/error.hpp"

namespace safebox::domain {

struct FetchRequest {
    std::string url;
    std::size_t maxBytes = 1u << 20;
    std::chrono::milliseconds timeout{10000}; // на весь запрос, включая редиректы
};

struct FetchResponse {
    int status = 0;          // всегда 2xx: остальное - ошибка
    std::string finalUrl;    // адрес после редиректов
    std::string contentType; // в нижнем регистре, вместе с параметрами (charset)
    Bytes body;              // не больше maxBytes
};

class PageFetcher {
public:
    virtual ~PageFetcher() = default;

    // Ответ длиннее maxBytes: HTML обрезается и это успех, любой другой тип - ошибка
    // (обрезанная картинка бесполезна).
    [[nodiscard]] virtual Result<FetchResponse> fetch(const FetchRequest& request) = 0;
};

} // namespace safebox::domain
