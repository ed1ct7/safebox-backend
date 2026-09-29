// Хранилище настроек приложения (файл рядом с программой в infra, память в тестах)
#pragma once

#include "safebox/domain/model/error.hpp"
#include "safebox/domain/model/settings.hpp"

namespace safebox::domain {

class SettingsStore {
public:
    virtual ~SettingsStore() = default;

    // Нет сохраненных настроек - значения по умолчанию; ошибка - только если прочитать не удалось.
    [[nodiscard]] virtual Result<AppSettings> load() = 0;
    [[nodiscard]] virtual Status save(const AppSettings& settings) = 0;
};

} // namespace safebox::domain
