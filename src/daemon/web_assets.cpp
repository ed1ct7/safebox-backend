#include "web_assets.hpp"

namespace safebox::daemon {

#if !defined(SAFEBOX_HAS_WEB_ASSETS)
std::span<const EmbeddedFile> embeddedFiles() noexcept {
    return {};
}
#endif

WebAssets::WebAssets() {
    for (const auto& file : embeddedFiles()) {
        files_.emplace(file.path, &file);
    }
}

std::optional<http::Asset> WebAssets::find(std::string_view path) const {
    const auto it = files_.find(path);
    if (it == files_.end()) {
        return std::nullopt;
    }
    const auto* file = it->second;
    return http::Asset{file->contentType,
                       std::string_view(reinterpret_cast<const char*>(file->data), file->size)};
}

} // namespace safebox::daemon
