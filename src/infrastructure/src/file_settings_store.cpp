// Настройки в текстовом файле: строки key=value ("linkPreviews=1"). Нет файла - значения по
// умолчанию; пустые строки, "#"-комментарии, неизвестные ключи и непонятные значения пропускаются.
// Запись через временный файл и rename: оборванная запись не оставит полфайла
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

#include "safebox/domain/model/rules.hpp"
#include "safebox/infra/factories.hpp"

namespace safebox::infra {
namespace {

using domain::AppSettings;
using domain::Error;
using domain::fail;
using domain::Result;
using domain::Status;

// "1"/"true" и "0"/"false" (без учета регистра); остальное - nullopt.
[[nodiscard]] std::optional<bool> parseBool(std::string_view text) {
    if (text == "1" || domain::detail::iequals(text, "true")) {
        return true;
    }
    if (text == "0" || domain::detail::iequals(text, "false")) {
        return false;
    }
    return std::nullopt;
}

class FileSettingsStore final : public domain::SettingsStore {
public:
    explicit FileSettingsStore(std::filesystem::path file) : file_(std::move(file)) {}

    Result<AppSettings> load() override {
        AppSettings settings;
        std::error_code ec;
        if (!std::filesystem::exists(file_, ec)) {
            return settings;
        }
        std::ifstream in(file_, std::ios::binary);
        if (!in) {
            return fail(Error::Code::IoError, "Не удалось прочитать файл настроек");
        }
        std::string line;
        while (std::getline(in, line)) {
            const auto text = domain::detail::trim(line);
            const auto eq = text.find('=');
            if (text.starts_with('#') || eq == std::string_view::npos) {
                continue;
            }
            const auto key = domain::detail::trim(text.substr(0, eq));
            if (key == "linkPreviews") {
                if (const auto value = parseBool(domain::detail::trim(text.substr(eq + 1)))) {
                    settings.linkPreviews = *value;
                }
            }
        }
        return settings;
    }

    Status save(const AppSettings& settings) override {
        std::error_code ec;
        if (file_.has_parent_path()) {
            std::filesystem::create_directories(file_.parent_path(), ec);
            if (ec) {
                return failSave();
            }
        }
        auto temp = file_;
        temp += ".tmp";
        {
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            out << "linkPreviews=" << (settings.linkPreviews ? 1 : 0) << '\n';
            out.flush();
            if (!out) {
                out.close();
                std::filesystem::remove(temp, ec);
                return failSave();
            }
        }
        std::filesystem::rename(temp, file_, ec);
        if (ec) {
            std::filesystem::remove(temp, ec);
            return failSave();
        }
        return {};
    }

private:
    [[nodiscard]] static std::unexpected<Error> failSave() {
        return fail(Error::Code::IoError, "Не удалось сохранить настройки");
    }

    std::filesystem::path file_;
};

} // namespace

std::unique_ptr<domain::SettingsStore> makeFileSettingsStore(std::filesystem::path file) {
    return std::make_unique<FileSettingsStore>(std::move(file));
}

} // namespace safebox::infra
