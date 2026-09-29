// SettingsService: настройки читаются из хранилища один раз при создании и держатся в памяти.
// Не читается (порча, права) - значения по умолчанию: приложение должно запускаться и так
#include <mutex>

#include "service_impls.hpp"

namespace safebox::app {
namespace {

class SettingsServiceImpl final : public SettingsService {
public:
    explicit SettingsServiceImpl(Ports ports) : store_(ports.settings) {
        if (auto loaded = store_.load()) {
            current_ = *loaded;
        }
    }

    domain::AppSettings get() override {
        std::scoped_lock lock(mutex_);
        return current_;
    }

    Result<domain::AppSettings> update(const domain::AppSettings& settings) override {
        std::scoped_lock lock(mutex_);
        if (auto st = store_.save(settings); !st) {
            return std::unexpected(st.error());
        }
        current_ = settings;
        return current_;
    }

private:
    domain::SettingsStore& store_;
    std::mutex mutex_; // save может ждать диск: два update не должны перемешаться
    domain::AppSettings current_;
};

} // namespace

std::shared_ptr<SettingsService> makeSettingsService(Ports ports) {
    return std::make_shared<SettingsServiceImpl>(ports);
}

} // namespace safebox::app
