// /api/v1/entries (список любой записи, правка, перенос, удаление), /api/v1/folders
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

// Необязательное строковое поле; неверный тип -> 400 уже отправлен.
[[nodiscard]] bool readOptionalString(const Json& body, const char* field,
                                      std::optional<std::string>& value, httplib::Response& res) {
    const auto it = body.find(field);
    if (it == body.end()) {
        return true;
    }
    if (!it->is_string()) {
        sendError(res, 400, "bad_request", std::string("Поле '") + field + "' должно быть строкой");
        return false;
    }
    value = it->get<std::string>();
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
        auto listing = ctx.services.entries->list(*lease, parent);
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
                         !readOptionalString(*body, "url", cmd.url, res)) {
                         return;
                     }
                     if (!cmd.name && !cmd.description && !cmd.url) {
                         sendError(res, 400, "bad_request",
                                   "Укажите хотя бы одно поле: name, description или url");
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
}

} // namespace safebox::http
