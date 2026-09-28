// Раздача вшитого фронта. Без SAFEBOX_WEB_DIST таблица пустая, в dev фронт идет через vite proxy
#pragma once

#include <cstddef>
#include <span>
#include <string_view>
#include <unordered_map>

#include "safebox/http/server.hpp"

namespace safebox::daemon {

struct EmbeddedFile {
    std::string_view path; // "/index.html", "/assets/app-3f2a.js"
    std::string_view contentType;
    const unsigned char* data;
    std::size_t size;
};

// Определяется сгенерированным web_assets_data.cpp или пустой заглушкой.
[[nodiscard]] std::span<const EmbeddedFile> embeddedFiles() noexcept;

class WebAssets final : public http::AssetProvider {
public:
    WebAssets();

    [[nodiscard]] std::optional<http::Asset> find(std::string_view path) const override;
    [[nodiscard]] bool empty() const override { return files_.empty(); }

private:
    std::unordered_map<std::string_view, const EmbeddedFile*> files_;
};

} // namespace safebox::daemon
