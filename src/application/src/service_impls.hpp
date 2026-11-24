// фабрики реализаций, сами классы в анонимных namespace в .cpp
#pragma once

#include <functional>
#include <memory>

#include "safebox/app/factory.hpp"
#include "safebox/app/services.hpp"

namespace safebox::app {

class VaultSession;

// Сейф открыт (create/unlock): новая сессия. Вызывается под блокировкой жизненного цикла -
// только поставить задание в очередь.
using SessionStarted = std::function<void(std::weak_ptr<VaultSession>)>;

[[nodiscard]] std::shared_ptr<SafeService> makeSafeService(Ports ports, AppConfig config,
                                                           SessionStarted onStarted = {});
[[nodiscard]] std::shared_ptr<EntriesService> makeEntriesService(Ports ports);
[[nodiscard]] std::shared_ptr<ImportExportService> makeImportExportService(Ports ports);
[[nodiscard]] std::shared_ptr<SearchService> makeSearchService(Ports ports);
[[nodiscard]] std::shared_ptr<TagsService> makeTagsService(Ports ports);
[[nodiscard]] std::shared_ptr<SettingsService> makeSettingsService(Ports ports);
// Один фоновый поток на сервис, останавливается в деструкторе.
[[nodiscard]] std::shared_ptr<LinksService>
makeLinksService(Ports ports, std::shared_ptr<SettingsService> settings);

// Досчет хешей: start ставит открытую сессию в очередь своего фонового потока.
class ContentHashRunner : public ContentHashService {
public:
    virtual void start(std::weak_ptr<VaultSession> session) = 0;
};
[[nodiscard]] std::shared_ptr<ContentHashRunner> makeContentHashService(Ports ports);
[[nodiscard]] std::shared_ptr<TrashService> makeTrashService(Ports ports);
// Один фоновый поток на сервис.
[[nodiscard]] std::shared_ptr<BackupService> makeBackupService(Ports ports, AppConfig config);

// Путь к файлу сейфа из запроса: обрезан, UTF-8, относительный - от defaultDirectory,
// нормализован, с именем файла. Расширение не трогает.
[[nodiscard]] Result<std::filesystem::path>
resolveSafePath(std::string_view raw, const std::filesystem::path& defaultDirectory);

} // namespace safebox::app
