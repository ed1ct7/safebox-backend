#include "path_folders.hpp"

#include "safebox/domain/model/rules.hpp"

namespace safebox::app {
namespace {

using domain::EntryId;
using domain::Kind;

struct Segments {
    std::vector<std::string> parts;
    bool parent = false; // встретился ".."
};

// Сегменты по "/" и "\": очищаются (sanitizeName), пустые и "." отбрасываются.
[[nodiscard]] Segments segmentsOf(std::string_view path) {
    Segments out;
    std::size_t start = 0;
    while (start <= path.size()) {
        const auto end = path.find_first_of("/\\", start);
        const auto segment = path.substr(
            start, end == std::string_view::npos ? std::string_view::npos : end - start);
        if (segment == "..") {
            out.parent = true;
        } else if (!segment.empty() && segment != ".") {
            out.parts.push_back(domain::sanitizeName(segment));
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    return out;
}

} // namespace

SplitPath splitPath(std::string_view relativePath) {
    SplitPath out;
    auto segments = segmentsOf(relativePath);
    for (const auto& part : segments.parts) {
        out.clean.append(out.clean.empty() ? "" : "/").append(part);
    }
    if (segments.parent || segments.parts.empty()) {
        if (out.clean.empty()) {
            out.clean = domain::sanitizeName(relativePath);
        }
        return out;
    }
    out.name = std::move(segments.parts.back());
    segments.parts.pop_back();
    out.dirs = std::move(segments.parts);
    out.valid = true;
    return out;
}

std::optional<std::vector<std::string>> splitDirectories(std::string_view path) {
    auto segments = segmentsOf(path);
    if (segments.parent) {
        return std::nullopt;
    }
    return std::move(segments.parts);
}

// Siblings

Siblings::Siblings(const Catalog& catalog, std::optional<EntryId> parent) {
    for (const auto child : catalog.childrenOf(parent)) {
        const auto* node = catalog.find(child);
        add(node->folded, Occupant{child, node->entry.isFolder()});
    }
}

const Siblings::Occupant* Siblings::find(const std::string& folded) const {
    const auto it = byName_.find(folded);
    return it == byName_.end() ? nullptr : &it->second;
}

void Siblings::add(std::string folded, Occupant occupant) {
    taken_.insert(folded);
    byName_.try_emplace(std::move(folded), occupant);
}

// PathFolders

PathFolders::PathFolders(Ports ports, VaultSession& session, std::shared_ptr<const Sealer> sealer,
                         std::optional<EntryId> root, VaultSession::CatalogPtr snapshot)
    : ports_(ports), session_(session), sealer_(std::move(sealer)), root_(root),
      snapshot_(std::move(snapshot)) {}

Siblings& PathFolders::siblingsOf(std::optional<EntryId> parent) {
    return siblings_.try_emplace(parent, *snapshot_, parent).first->second;
}

domain::Result<std::optional<EntryId>> PathFolders::resolve(const std::vector<std::string>& dirs,
                                                            domain::UnitOfWork* uow) {
    std::optional<EntryId> parent = root_;
    std::string key;
    for (const auto& dir : dirs) {
        // без учета регистра: "Pict" и "pict" на Windows - одна папка
        const auto foldedDir = foldForSearch(dir);
        key.append(foldedDir).push_back('/');
        if (const auto it = folders_.find(key); it != folders_.end()) {
            parent = it->second;
            continue;
        }
        auto& siblings = siblingsOf(parent);
        const auto* found = siblings.find(foldedDir);
        EntryId folder = 0;
        if (found != nullptr && found->isFolder) {
            folder = found->id;
        } else {
            auto made = uow != nullptr ? createFolder(*uow, parent, dir)
                                       : createFolderCommitted(parent, dir);
            if (!made) {
                return std::unexpected(made.error());
            }
            folder = *made;
            siblings.add(foldedDir, {folder, true});
        }
        folders_.emplace(key, folder);
        parent = folder;
    }
    return parent;
}

domain::Result<EntryId> PathFolders::createFolderCommitted(std::optional<EntryId> parent,
                                                           const std::string& name) {
    auto uow = ports_.store.begin();
    if (!uow) {
        return std::unexpected(uow.error());
    }
    auto id = createFolder(**uow, parent, name);
    if (!id) {
        return id;
    }
    if (auto st = (*uow)->commit(); !st) {
        return std::unexpected(st.error());
    }
    session_.invalidateCatalog();
    return id;
}

domain::Result<EntryId> PathFolders::createFolder(domain::UnitOfWork& uow,
                                                  std::optional<EntryId> parent,
                                                  const std::string& name) {
    domain::EntryRecord record;
    record.parentId = parent;
    record.isFolder = true;
    auto id = uow.entries().insert(record);
    if (!id) {
        return std::unexpected(id.error());
    }
    domain::EntryMeta meta;
    meta.kind = Kind::Folder;
    meta.createdAt = meta.modifiedAt = domain::toUnixMillis(ports_.clock.now());
    auto encName = sealer_->sealName(*id, name);
    if (!encName) {
        return std::unexpected(encName.error());
    }
    auto encMeta = sealer_->sealMeta(*id, meta);
    if (!encMeta) {
        return std::unexpected(encMeta.error());
    }
    if (auto st = uow.entries().updateSealed(*id, *encName, *encMeta); !st) {
        return std::unexpected(st.error());
    }
    return *id;
}

} // namespace safebox::app
