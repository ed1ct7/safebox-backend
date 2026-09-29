// Пути и папки для импорта файлов и добавления ссылок: разбор пути, занятые имена у родителя,
// поиск и создание папок по пути. Папки по пути сливаются с существующими папками по имени без
// учета регистра; одноименная не-папка не мешает - рядом создается папка
#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "safebox/app/factory.hpp"
#include "vault_session.hpp"

namespace safebox::app {

// Путь файла из запроса: сегменты очищены, ".." или пустой путь - недопустим.
struct SplitPath {
    std::string clean; // очищенный путь целиком: для отчета об ошибке
    std::vector<std::string> dirs;
    std::string name;
    bool valid = false;
};

[[nodiscard]] SplitPath splitPath(std::string_view relativePath);

// Путь, все сегменты которого - папки (у ссылки нет имени файла в пути): очищенные сегменты,
// пустой путь - без папок; ".." - nullopt.
[[nodiscard]] std::optional<std::vector<std::string>> splitDirectories(std::string_view path);

// Занятые имена у одного родителя: записи любого вида по folded-имени. С файлом конфликтует
// первая запись с таким именем; папки в каталоге идут первыми.
class Siblings {
public:
    struct Occupant {
        domain::EntryId id = 0;
        bool isFolder = false;
    };

    Siblings(const Catalog& catalog, std::optional<domain::EntryId> parent);

    [[nodiscard]] const Occupant* find(const std::string& folded) const;
    [[nodiscard]] const std::unordered_set<std::string>& taken() const noexcept { return taken_; }

    void add(std::string folded, Occupant occupant);

private:
    std::unordered_map<std::string, Occupant> byName_;
    std::unordered_set<std::string> taken_;
};

class PathFolders {
public:
    // root - папка (или любая запись), от которой считаются пути; snapshot - каталог на начало
    // работы: то, что создано позже, объект помнит сам.
    PathFolders(Ports ports, VaultSession& session, std::shared_ptr<const Sealer> sealer,
                std::optional<domain::EntryId> root, VaultSession::CatalogPtr snapshot);

    // Папка dirs внутри root (nullopt - сам root, если он корень); недостающие создаются.
    // uow - транзакция вызывающего: коммит и invalidateCatalog на нем. nullptr - каждая новая
    // папка сохраняется своей транзакцией (импорт пишет файлы отдельно от нее).
    [[nodiscard]] domain::Result<std::optional<domain::EntryId>>
    resolve(const std::vector<std::string>& dirs, domain::UnitOfWork* uow = nullptr);

    // Занятые имена у parent: снимок плюс созданное через этот объект и добавленное add().
    [[nodiscard]] Siblings& siblingsOf(std::optional<domain::EntryId> parent);

private:
    // Своя транзакция на одну папку.
    [[nodiscard]] domain::Result<domain::EntryId>
    createFolderCommitted(std::optional<domain::EntryId> parent, const std::string& name);
    [[nodiscard]] domain::Result<domain::EntryId>
    createFolder(domain::UnitOfWork& uow, std::optional<domain::EntryId> parent,
                 const std::string& name);

    Ports ports_;
    VaultSession& session_;
    std::shared_ptr<const Sealer> sealer_;
    std::optional<domain::EntryId> root_;
    VaultSession::CatalogPtr snapshot_;
    std::map<std::string, domain::EntryId> folders_; // "a/b/" (folded) -> найденная или созданная
    std::map<std::optional<domain::EntryId>, Siblings> siblings_; // родитель -> занятые имена
};

} // namespace safebox::app
