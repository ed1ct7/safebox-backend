#include "safebox/app/factory.hpp"

#include "service_impls.hpp"

namespace safebox::app {

Services makeServices(Ports ports, AppConfig config) {
    Services services;
    auto hashes = makeContentHashService(ports);
    services.safe = makeSafeService(
        ports, config,
        [weak = std::weak_ptr<ContentHashRunner>(hashes)](std::weak_ptr<VaultSession> session) {
            if (auto runner = weak.lock()) {
                runner->start(std::move(session));
            }
        });
    services.entries = makeEntriesService(ports);
    services.importExport = makeImportExportService(ports);
    services.search = makeSearchService(ports);
    services.tags = makeTagsService(ports);
    services.settings = makeSettingsService(ports);
    services.links = makeLinksService(ports, services.settings);
    services.backup = makeBackupService(ports, config);
    services.hashes = std::move(hashes);
    return services;
}

} // namespace safebox::app
