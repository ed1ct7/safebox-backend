// /api/v1/entries (список любой записи, правка, перенос, теги, предпросмотр ссылки, удаление),
// /api/v1/folders
#include "dto.hpp"

namespace safebox::http {

namespace {

[[nodiscard]] std::optional<domain::EntryId> pathId(const httplib::Request& req,
                                                    httplib::Response& res) {
    const auto it = req.path_params.find("id");
    const auto id = it == req.path_params.end() ? std::nullopt : parseId(it->second);
    if (!id) {
        sendError(res, 400, "bad_request", "Некорректный идентификатор объекта");
    }
    return id;
}

// Поле "ids": массив положительных целых; иначе 400 уже отправлен.
[[nodiscard]] std::optional<std::vector<domain::EntryId>> readIds(const Json& body,
                                                                  httplib::Response& res) {
    const auto it = body.find("ids");
    if (it == body.end() || !it->is_array()) {
        sendError(res, 400, "bad_request", "Поле 'ids' должно быть массивом идентификаторов");
        return std::nullopt;
    }
    std::vector<domain::EntryId> ids;
    for (const auto& value : *it) {
        if (!value.is_number_integer() || value.get<std::int64_t>() <= 0) {
            sendError(res, 400, "bad_request", "Некорректный идентификатор объекта");
            return std::nullopt;
        }
        ids.push_back(value.get<domain::EntryId>());
    }
    return ids;
}

// Поле "parentId": число или null (корень), само поле обязательно; иначе 400 уже отправлен.
[[nodiscard]] bool readParent(const Json& body, std::optional<domain::EntryId>& parent,
                              httplib::Response& res) {
    const auto it = body.find("parentId");
    if (it != body.end() && it->is_null()) {
        parent.reset();
        return true;
    }
    if (it == body.end() || !it->is_number_integer() || it->get<std::int64_t>() <= 0) {
        sendError(res, 400, "bad_request", "Поле 'parentId' должно быть числом или null");
        return false;
    }
    parent = it->get<domain::EntryId>();
    return true;
}

// Необязательное "resolutions": {"<id>": "keepBoth"|"replace"|"skip"}; иначе 400 уже отправлен.
[[nodiscard]] bool readResolutions(const Json& body,
                                   std::unordered_map<domain::EntryId, app::ConflictPolicy>& out,
                                   httplib::Response& res) {
    const auto it = body.find("resolutions");
    if (it == body.end()) {
        return true;
    }
    if (!it->is_object()) {
        sendError(res, 400, "bad_request", "Поле 'resolutions' должно быть объектом");
        return false;
    }
    for (const auto& [key, value] : it->items()) {
        const auto id = parseId(key);
        const auto policy =
            value.is_string() ? parseConflictPolicy(value.get<std::string>()) : std::nullopt;
        if (!id || !policy) {
            sendError(res, 400, "bad_request", "Некорректное решение о конфликте имен");
            return false;
        }
        out[*id] = *policy;
    }
    return true;
}

// Необязательное "add": [{"tagId": число, "inherit"?: bool}]; иначе 400 уже отправлен.
[[nodiscard]] bool readAdditions(const Json& body, std::vector<domain::TagAssignment>& out,
                                 httplib::Response& res) {
    const auto it = body.find("add");
    if (it == body.end()) {
        return true;
    }
    const auto invalid = [&] {
        sendError(res, 400, "bad_request",
                  "Поле 'add' должно быть списком объектов {tagId, inherit}");
        return false;
    };
    if (!it->is_array()) {
        return invalid();
    }
    for (const auto& item : *it) {
        const auto id = item.find("tagId"); // не объект - end()
        const auto inherit = item.find("inherit");
        if (id == item.end() || !id->is_number_integer() || id->get<std::int64_t>() <= 0 ||
            (inherit != item.end() && !inherit->is_boolean())) {
            return invalid();
        }
        out.push_back({id->get<domain::TagId>(), inherit != item.end() && inherit->get<bool>()});
    }
    return true;
}

// Необязательное "remove": [числа]; иначе 400 уже отправлен.
[[nodiscard]] bool readRemovals(const Json& body, std::vector<domain::TagId>& out,
                                httplib::Response& res) {
    const auto it = body.find("remove");
    if (it == body.end()) {
        return true;
    }
    if (!it->is_array()) {
        sendError(res, 400, "bad_request", "Поле 'remove' должно быть списком идентификаторов");
        return false;
    }
    for (const auto& value : *it) {
        if (!value.is_number_integer() || value.get<std::int64_t>() <= 0) {
            sendError(res, 400, "bad_request", "Некорректный идентификатор тега");
            return false;
        }
        out.push_back(value.get<domain::TagId>());
    }
    return true;
}

} // namespace

void registerEntriesApi(httplib::Server& server, ApiContext& ctx) {
    server.Get("/api/v1/entries", [&ctx](const httplib::Request& req, httplib::Response& res) {
        auto lease = requireApi(ctx, req, res);
        if (!lease) {
            return;
        }
        std::optional<domain::EntryId> parent;
        if (req.has_param("parentId") && !req.get_param_value("parentId").empty()) {
            parent = parseId(req.get_param_value("parentId"));
            if (!parent) {
                sendError(res, 400, "bad_request", "Некорректный идентификатор папки");
                return;
            }
        }
        app::ListOptions options;
        if (!readListOptions(req, options, res)) {
            return;
        }
        auto listing = ctx.services.entries->list(*lease, parent, options);
        if (!listing) {
            sendError(res, listing.error());
            return;
        }
        sendJson(res, toJson(*listing));
    });

    server.Get("/api/v1/entries/:id", [&ctx](const httplib::Request& req, httplib::Response& res) {
        auto lease = requireApi(ctx, req, res);
        if (!lease) {
            return;
        }
        const auto id = pathId(req, res);
        if (!id) {
            return;
        }
        auto entry = ctx.services.entries->get(*lease, *id);
        if (!entry) {
            sendError(res, entry.error());
            return;
        }
        sendJson(res, toJson(*entry));
    });

    server.Get("/api/v1/folders", [&ctx](const httplib::Request& req, httplib::Response& res) {
        auto lease = requireApi(ctx, req, res);
        if (!lease) {
            return;
        }
        auto folders = ctx.services.entries->folders(*lease);
        if (!folders) {
            sendError(res, folders.error());
            return;
        }
        Json items = Json::array();
        for (const auto& folder : *folders) {
            items.push_back(toJson(folder));
        }
        sendJson(res, Json{{"folders", std::move(items)}});
    });

    // Папки по путям: {"parentId": число|null, "paths": [строка]} -> {"ids": [число|null]}.
    server.Post("/api/v1/folders", [&ctx](const httplib::Request& req, httplib::Response& res) {
        auto lease = requireApi(ctx, req, res);
        if (!lease) {
            return;
        }
        auto body = readJsonObject(req, res);
        if (!body) {
            return;
        }
        std::optional<domain::EntryId> parent;
        if (!readParent(*body, parent, res)) {
            return;
        }
        const auto it = body->find("paths");
        if (it == body->end() || !it->is_array() || it->empty()) {
            sendError(res, 400, "bad_request", "Поле 'paths' должно быть непустым списком строк");
            return;
        }
        std::vector<std::string> paths;
        paths.reserve(it->size());
        for (const auto& path : *it) {
            if (!path.is_string()) {
                sendError(res, 400, "bad_request", "Поле 'paths' должно быть списком строк");
                return;
            }
            paths.push_back(path.get<std::string>());
        }
        auto made = ctx.services.entries->makeFolders(*lease, parent, paths);
        if (!made) {
            sendError(res, made.error());
            return;
        }
        Json ids = Json::array();
        for (const auto& id : *made) {
            ids.push_back(id ? Json(*id) : Json(nullptr));
        }
        sendJson(res, Json{{"ids", std::move(ids)}});
    });

    server.Patch("/api/v1/entries/:id",
                 [&ctx](const httplib::Request& req, httplib::Response& res) {
                     auto lease = requireApi(ctx, req, res);
                     if (!lease) {
                         return;
                     }
                     const auto id = pathId(req, res);
                     if (!id) {
                         return;
                     }
                     auto body = readJsonObject(req, res);
                     if (!body) {
                         return;
                     }
                     app::UpdateEntryCmd cmd;
                     if (!readOptionalString(*body, "name", cmd.name, res) ||
                         !readOptionalString(*body, "description", cmd.description, res) ||
                         !readOptionalString(*body, "url", cmd.url, res) ||
                         !readClearableString(*body, "source", cmd.source, res) ||
                         !readClearableString(*body, "sourceUrl", cmd.sourceUrl, res) ||
                         !readSourceDate(*body, cmd.sourceDate, res)) {
                         return;
                     }
                     if (!cmd.name && !cmd.description && !cmd.url && !cmd.source &&
                         !cmd.sourceUrl && !cmd.sourceDate) {
                         sendError(res, 400, "bad_request",
                                   "Укажите хотя бы одно поле: name, description, url, source, "
                                   "sourceUrl или sourceDate");
                         return;
                     }
                     auto entry = ctx.services.entries->update(*lease, *id, cmd);
                     if (!entry) {
                         sendError(res, entry.error());
                         return;
                     }
                     sendJson(res, toJson(*entry));
                 });

    server.Delete("/api/v1/entries/:id",
                  [&ctx](const httplib::Request& req, httplib::Response& res) {
                      auto lease = requireApi(ctx, req, res);
                      if (!lease) {
                          return;
                      }
                      const auto id = pathId(req, res);
                      if (!id) {
                          return;
                      }
                      const domain::EntryId ids[] = {*id};
                      auto removed = ctx.services.entries->remove(*lease, ids);
                      if (!removed) {
                          sendError(res, removed.error());
                          return;
                      }
                      sendJson(res, Json{{"removed", *removed}});
                  });

    server.Post("/api/v1/entries/delete", [&ctx](const httplib::Request& req,
                                                 httplib::Response& res) {
        auto lease = requireApi(ctx, req, res);
        if (!lease) {
            return;
        }
        auto body = readJsonObject(req, res);
        if (!body) {
            return;
        }
        const auto ids = readIds(*body, res);
        if (!ids) {
            return;
        }
        auto removed = ctx.services.entries->remove(*lease, *ids);
        if (!removed) {
            sendError(res, removed.error());
            return;
        }
        sendJson(res, Json{{"removed", *removed}});
    });

    // {"sha256": [hex]} -> {"found": [{"sha256", "id"}]}: что из архива уже есть в сейфе.
    server.Post(
        std::string(kLookupPath), [&ctx](const httplib::Request& req, httplib::Response& res) {
            auto lease = requireApi(ctx, req, res);
            if (!lease) {
                return;
            }
            auto body = readJsonObject(req, res);
            if (!body) {
                return;
            }
            const auto it = body->find("sha256");
            if (it == body->end() || !it->is_array()) {
                sendError(res, 400, "bad_request", "Поле 'sha256' должно быть списком hex-строк");
                return;
            }
            if (it->size() > app::kMaxLookupHashes) { // до разбора: тело до 32 МиБ
                sendError(res, domain::Error{domain::Error::Code::InvalidArgument,
                                             "Не больше 10 000 хешей за раз"});
                return;
            }
            std::vector<domain::Sha256> hashes;
            hashes.reserve(it->size());
            for (const auto& item : *it) {
                const auto hash =
                    item.is_string() ? parseSha256(item.get<std::string>()) : std::nullopt;
                if (!hash) {
                    sendError(res, 400, "bad_request", "Каждый sha256 - 64 hex-символа");
                    return;
                }
                hashes.push_back(*hash);
            }
            auto found = ctx.services.entries->lookup(*lease, hashes);
            if (!found) {
                sendError(res, found.error());
                return;
            }
            Json items = Json::array();
            for (const auto& match : *found) {
                items.push_back(Json{{"sha256", sha256Hex(match.sha256)}, {"id", match.id}});
            }
            sendJson(res, Json{{"found", std::move(items)}});
        });

    server.Post("/api/v1/entries/move/plan",
                [&ctx](const httplib::Request& req, httplib::Response& res) {
                    auto lease = requireApi(ctx, req, res);
                    if (!lease) {
                        return;
                    }
                    auto body = readJsonObject(req, res);
                    if (!body) {
                        return;
                    }
                    const auto ids = readIds(*body, res);
                    std::optional<domain::EntryId> parent;
                    if (!ids || !readParent(*body, parent, res)) {
                        return;
                    }
                    auto conflicts = ctx.services.entries->planMove(*lease, *ids, parent);
                    if (!conflicts) {
                        sendError(res, conflicts.error());
                        return;
                    }
                    Json items = Json::array();
                    for (const auto& conflict : *conflicts) {
                        items.push_back(toJson(conflict));
                    }
                    sendJson(res, Json{{"conflicts", std::move(items)}});
                });

    server.Post("/api/v1/entries/move",
                [&ctx](const httplib::Request& req, httplib::Response& res) {
                    auto lease = requireApi(ctx, req, res);
                    if (!lease) {
                        return;
                    }
                    auto body = readJsonObject(req, res);
                    if (!body) {
                        return;
                    }
                    app::MoveCmd cmd;
                    auto ids = readIds(*body, res);
                    if (!ids || !readParent(*body, cmd.parent, res) ||
                        !readResolutions(*body, cmd.resolutions, res)) {
                        return;
                    }
                    cmd.ids = std::move(*ids);
                    auto moved = ctx.services.entries->move(*lease, cmd);
                    if (!moved) {
                        sendError(res, moved.error());
                        return;
                    }
                    sendJson(res, toJson(*moved));
                });

    server.Post("/api/v1/entries/:id/preview",
                [&ctx](const httplib::Request& req, httplib::Response& res) {
                    auto lease = requireApi(ctx, req, res);
                    if (!lease) {
                        return;
                    }
                    const auto id = pathId(req, res);
                    if (!id) {
                        return;
                    }
                    auto entry = ctx.services.links->refreshPreview(*lease, *id);
                    if (!entry) {
                        sendError(res, entry.error());
                        return;
                    }
                    sendJson(res, toJson(*entry));
                });

    server.Post("/api/v1/entries/tags", [&ctx](const httplib::Request& req,
                                               httplib::Response& res) {
        auto lease = requireApi(ctx, req, res);
        if (!lease) {
            return;
        }
        auto body = readJsonObject(req, res);
        if (!body) {
            return;
        }
        app::AssignTagsCmd cmd;
        auto ids = readIds(*body, res);
        if (!ids || !readAdditions(*body, cmd.add, res) || !readRemovals(*body, cmd.remove, res)) {
            return;
        }
        if (!body->contains("add") && !body->contains("remove")) {
            sendError(res, 400, "bad_request", "Укажите хотя бы одно поле: add или remove");
            return;
        }
        cmd.ids = std::move(*ids);
        auto updated = ctx.services.tags->assign(*lease, cmd);
        if (!updated) {
            sendError(res, updated.error());
            return;
        }
        sendJson(res, Json{{"updated", *updated}});
    });

    // Корзина: корни удаленных поддеревьев. По умолчанию - недавно удаленные первыми.
    server.Get("/api/v1/trash", [&ctx](const httplib::Request& req, httplib::Response& res) {
        auto lease = requireApi(ctx, req, res);
        if (!lease) {
            return;
        }
        app::ListOptions options;
        if (!req.has_param("sort")) {
            options.sort = app::SortKey::Trashed;
            options.descending = true;
        }
        if (!readListOptions(req, options, res)) {
            return;
        }
        auto listing = ctx.services.trash->list(*lease, options);
        if (!listing) {
            sendError(res, listing.error());
            return;
        }
        sendJson(res, toJson(*listing));
    });

    server.Post("/api/v1/trash/restore",
                [&ctx](const httplib::Request& req, httplib::Response& res) {
                    auto lease = requireApi(ctx, req, res);
                    if (!lease) {
                        return;
                    }
                    auto body = readJsonObject(req, res);
                    if (!body) {
                        return;
                    }
                    const auto ids = readIds(*body, res);
                    if (!ids) {
                        return;
                    }
                    auto restored = ctx.services.trash->restore(*lease, *ids);
                    if (!restored) {
                        sendError(res, restored.error());
                        return;
                    }
                    sendJson(res, toJson(*restored));
                });

    // Навсегда: {"ids": [...]} | {"before": unix-мс} | {"all": true}.
    server.Post("/api/v1/trash/purge", [&ctx](const httplib::Request& req, httplib::Response& res) {
        auto lease = requireApi(ctx, req, res);
        if (!lease) {
            return;
        }
        auto body = readJsonObject(req, res);
        if (!body) {
            return;
        }
        app::PurgeCmd cmd;
        if (body->contains("ids")) {
            auto ids = readIds(*body, res);
            if (!ids) {
                return;
            }
            if (ids->empty()) {
                sendError(res, 400, "bad_request", "Поле 'ids' пустое");
                return;
            }
            cmd.ids = std::move(*ids);
        }
        if (const auto it = body->find("before"); it != body->end()) {
            if (!it->is_number_integer()) {
                sendError(res, 400, "bad_request", "Поле 'before' должно быть временем в мс");
                return;
            }
            cmd.before = it->get<std::int64_t>();
        }
        if (const auto it = body->find("all"); it != body->end()) {
            if (!it->is_boolean() || !it->get<bool>()) {
                sendError(res, 400, "bad_request", "Поле 'all' может быть только true");
                return;
            }
            cmd.all = true;
        }
        auto purged = ctx.services.trash->purge(*lease, cmd);
        if (!purged) {
            sendError(res, purged.error());
            return;
        }
        sendJson(res, Json{{"purged", *purged}});
    });
}

} // namespace safebox::http
