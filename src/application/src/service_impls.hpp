// фабрики реализаций, сами классы в анонимных namespace в .cpp
#pragma once

#include <memory>

#include "safebox/app/factory.hpp"
#include "safebox/app/services.hpp"

namespace safebox::app {

[[nodiscard]] std::shared_ptr<SafeService> makeSafeService(Ports ports, AppConfig config);
[[nodiscard]] std::shared_ptr<EntriesService> makeEntriesService(Ports ports);
[[nodiscard]] std::shared_ptr<ImportExportService> makeImportExportService(Ports ports);
[[nodiscard]] std::shared_ptr<SearchService> makeSearchService(Ports ports);

} // namespace safebox::app
