// /api/v1/entries, /api/v1/folders
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
                     auto name = requireString(*body, "name", res);
                     if (!name) {
                         return;
                     }
                     auto entry = ctx.services.entries->rename(*lease, *id, *name);
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
        const auto it = body->find("ids");
        if (it == body->end() || !it->is_array()) {
            sendError(res, 400, "bad_request", "Поле 'ids' должно быть массивом идентификаторов");
            return;
        }
        std::vector<domain::EntryId> ids;
        for (const auto& value : *it) {
            if (!value.is_number_integer() || value.get<std::int64_t>() <= 0) {
                sendError(res, 400, "bad_request", "Некорректный идентификатор объекта");
                return;
            }
            ids.push_back(value.get<domain::EntryId>());
        }
        auto removed = ctx.services.entries->remove(*lease, ids);
        if (!removed) {
            sendError(res, removed.error());
            return;
        }
        sendJson(res, Json{{"removed", *removed}});
    });
}

} // namespace safebox::http
