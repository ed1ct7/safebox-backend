#include "safebox/app/factory.hpp"

#include "service_impls.hpp"

namespace safebox::app {

Services makeServices(Ports ports, AppConfig config) {
    Services services;
    services.safe = makeSafeService(ports, std::move(config));
    services.entries = makeEntriesService(ports);
    services.importExport = makeImportExportService(ports);
    services.search = makeSearchService(ports);
    services.tags = makeTagsService(ports);
    return services;
}

} // namespace safebox::app
