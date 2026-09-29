// Настройки в памяти. Считает сохранения; failSave / failLoad проваливают операцию
#pragma once

#include <mutex>

#include "safebox/domain/ports/settings.hpp"

namespace safebox::test {

class FakeSettingsStore final : public domain::SettingsStore {
public:
    domain::Result<domain::AppSettings> load() override {
        std::scoped_lock lock(mutex_);
        if (failLoad) {
            return domain::fail(domain::Error::Code::IoError, "Не удалось прочитать файл настроек");
        }
        return saved;
    }

    domain::Status save(const domain::AppSettings& settings) override {
        std::scoped_lock lock(mutex_);
        if (failSave) {
            return domain::fail(domain::Error::Code::IoError, "Не удалось сохранить настройки");
        }
        saved = settings;
        ++saves;
        return {};
    }

    domain::AppSettings saved; // что "лежит в файле"
    int saves = 0;
    bool failLoad = false;
    bool failSave = false;

private:
    std::mutex mutex_;
};

} // namespace safebox::test
