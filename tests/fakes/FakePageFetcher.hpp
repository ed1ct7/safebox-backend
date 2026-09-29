// Страницы из таблицы по адресу (без сети). Неизвестный адрес - ошибка, как недоступный сайт.
// Считает вызовы и запоминает запросы; можно звать из фонового потока
#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "safebox/domain/ports/web.hpp"

namespace safebox::test {

class FakePageFetcher final : public domain::PageFetcher {
public:
    void add(const std::string& url, domain::FetchResponse response) {
        std::scoped_lock lock(mutex_);
        pages_.insert_or_assign(url, std::move(response));
    }

    // HTML с кодом 200; finalUrl пустой - значит редиректа не было.
    void addHtml(const std::string& url, std::string_view html, std::string finalUrl = {}) {
        add(url, {200, finalUrl.empty() ? url : std::move(finalUrl), "text/html; charset=utf-8",
                  domain::toBytes(html)});
    }

    void addImage(const std::string& url, domain::Bytes bytes,
                  std::string contentType = "image/png") {
        add(url, {200, url, std::move(contentType), std::move(bytes)});
    }

    void addError(const std::string& url, std::string message) {
        std::scoped_lock lock(mutex_);
        pages_.insert_or_assign(url,
                                domain::fail(domain::Error::Code::IoError, std::move(message)));
    }

    domain::Result<domain::FetchResponse> fetch(const domain::FetchRequest& request) override {
        {
            std::scoped_lock lock(mutex_);
            ++calls;
            requests_.push_back(request);
        }
        if (onFetch) {
            onFetch(request); // вне мьютекса: обработчик может ждать, пока тест отпустит запрос
        }
        std::scoped_lock lock(mutex_);
        const auto found = pages_.find(request.url);
        if (found == pages_.end()) {
            return domain::fail(domain::Error::Code::IoError, "Сайт недоступен");
        }
        return found->second;
    }

    std::vector<std::string> requested() const {
        std::scoped_lock lock(mutex_);
        std::vector<std::string> urls;
        for (const auto& request : requests_) {
            urls.push_back(request.url);
        }
        return urls;
    }

    std::vector<domain::FetchRequest> requests() const {
        std::scoped_lock lock(mutex_);
        return requests_;
    }

    std::atomic<int> calls{0};
    // Вызывается в начале каждого fetch; задавать до первого запроса.
    std::function<void(const domain::FetchRequest&)> onFetch;

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, domain::Result<domain::FetchResponse>> pages_;
    std::vector<domain::FetchRequest> requests_;
};

} // namespace safebox::test
