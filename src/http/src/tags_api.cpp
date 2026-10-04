// /api/v1/tags: категории и теги, слияние. Присвоение тегов записям - POST /api/v1/entries/tags
// (entries_api.cpp)
#include "dto.hpp"

namespace safebox::http {

namespace {

// Идентификатор из адреса; иначе 400 уже отправлен.
[[nodiscard]] std::optional<std::int64_t> pathId(const httplib::Request& req,
                                                 httplib::Response& res, const char* what) {
    const auto it = req.path_params.find("id");
    const auto id = it == req.path_params.end() ? std::nullopt : parseId(it->second);
    if (!id) {
        sendError(res, 400, "bad_request", std::string("Некорректный идентификатор ") + what);
    }
    return id;
}

// Идентификатор в теле - положительное целое.
[[nodiscard]] bool isId(const Json& value) {
    return value.is_number_integer() && value.get<std::int64_t>() > 0;
}

// Необязательное числовое поле-идентификатор; иначе 400 уже отправлен.
[[nodiscard]] bool readOptionalId(const Json& body, const char* field,
                                  std::optional<std::int64_t>& value, httplib::Response& res) {
    const auto it = body.find(field);
    if (it == body.end()) {
        return true;
    }
    if (!isId(*it)) {
        sendError(res, 400, "bad_request", std::string("Поле '") + field + "' должно быть числом");
        return false;
    }
    value = it->get<std::int64_t>();
    return true;
}

} // namespace

void registerTagsApi(httplib::Server& server, ApiContext& ctx) {
    server.Get("/api/v1/tags", [&ctx](const httplib::Request& req, httplib::Response& res) {
        auto lease = requireApi(ctx, req, res);
        if (!lease) {
            return;
        }
        auto categories = ctx.services.tags->list(*lease);
        if (!categories) {
            sendError(res, categories.error());
            return;
        }
        Json items = Json::array();
        for (const auto& category : *categories) {
            items.push_back(toJson(category));
        }
        sendJson(res, Json{{"categories", std::move(items)}});
    });

    server.Post(
        "/api/v1/tags/categories", [&ctx](const httplib::Request& req, httplib::Response& res) {
            auto lease = requireApi(ctx, req, res);
            if (!lease) {
                return;
            }
            auto body = readJsonObject(req, res);
            if (!body) {
                return;
            }
            const auto name = requireString(*body, "name", res);
            if (!name) {
                return;
            }
            std::optional<std::string> nameEn;
            if (!readOptionalString(*body, "nameEn", nameEn, res)) {
                return;
            }
            auto category = ctx.services.tags->createCategory(*lease, *name, nameEn.value_or(""));
            if (!category) {
                sendError(res, category.error());
                return;
            }
            sendJson(res, toJson(app::CategoryWithTags{*category, {}}), 201);
        });

    server.Patch(
        "/api/v1/tags/categories/:id", [&ctx](const httplib::Request& req, httplib::Response& res) {
            auto lease = requireApi(ctx, req, res);
            if (!lease) {
                return;
            }
            const auto id = pathId(req, res, "категории");
            if (!id) {
                return;
            }
            auto body = readJsonObject(req, res);
            if (!body) {
                return;
            }
            app::RenameCategoryCmd cmd;
            if (!readOptionalString(*body, "name", cmd.name, res) ||
                !readOptionalString(*body, "nameEn", cmd.nameEn, res)) {
                return;
            }
            if (!cmd.name && !cmd.nameEn) {
                sendError(res, 400, "bad_request", "Укажите хотя бы одно поле: name или nameEn");
                return;
            }
            auto category = ctx.services.tags->renameCategory(*lease, *id, cmd);
            if (!category) {
                sendError(res, category.error());
                return;
            }
            sendJson(res, toJson(*category));
        });

    server.Delete("/api/v1/tags/categories/:id",
                  [&ctx](const httplib::Request& req, httplib::Response& res) {
                      auto lease = requireApi(ctx, req, res);
                      if (!lease) {
                          return;
                      }
                      const auto id = pathId(req, res, "категории");
                      if (!id) {
                          return;
                      }
                      auto removed = ctx.services.tags->removeCategory(*lease, *id);
                      if (!removed) {
                          sendError(res, removed.error());
                          return;
                      }
                      sendJson(res, toJson(*removed));
                  });

    server.Post("/api/v1/tags", [&ctx](const httplib::Request& req, httplib::Response& res) {
        auto lease = requireApi(ctx, req, res);
        if (!lease) {
            return;
        }
        auto body = readJsonObject(req, res);
        if (!body) {
            return;
        }
        app::CreateTagCmd cmd;
        const auto category = requireString(*body, "category", res);
        const auto name = category ? requireString(*body, "name", res) : std::nullopt;
        if (!category || !name) {
            return;
        }
        std::optional<std::string> nameEn;
        if (!readOptionalString(*body, "nameEn", nameEn, res)) {
            return;
        }
        if (nameEn) {
            cmd.nameEn = std::move(*nameEn);
        }
        if (const auto it = body->find("createCategory"); it != body->end()) {
            if (!it->is_boolean()) {
                sendError(res, 400, "bad_request", "Поле 'createCategory' должно быть булевым");
                return;
            }
            cmd.createCategory = it->get<bool>();
        }
        cmd.category = *category;
        cmd.name = *name;
        auto created = ctx.services.tags->createTag(*lease, cmd);
        if (!created) {
            sendError(res, created.error());
            return;
        }
        sendJson(res, toJson(created->tag), created->created ? 201 : 200);
    });

    server.Patch("/api/v1/tags/:id", [&ctx](const httplib::Request& req, httplib::Response& res) {
        auto lease = requireApi(ctx, req, res);
        if (!lease) {
            return;
        }
        const auto id = pathId(req, res, "тега");
        if (!id) {
            return;
        }
        auto body = readJsonObject(req, res);
        if (!body) {
            return;
        }
        app::UpdateTagCmd cmd;
        if (!readOptionalString(*body, "name", cmd.name, res) ||
            !readOptionalString(*body, "nameEn", cmd.nameEn, res) ||
            !readOptionalId(*body, "categoryId", cmd.categoryId, res)) {
            return;
        }
        if (!cmd.name && !cmd.nameEn && !cmd.categoryId) {
            sendError(res, 400, "bad_request",
                      "Укажите хотя бы одно поле: name, nameEn или categoryId");
            return;
        }
        auto tag = ctx.services.tags->updateTag(*lease, *id, cmd);
        if (!tag) {
            sendError(res, tag.error());
            return;
        }
        sendJson(res, toJson(*tag));
    });

    server.Post("/api/v1/tags/:id/merge",
                [&ctx](const httplib::Request& req, httplib::Response& res) {
                    auto lease = requireApi(ctx, req, res);
                    if (!lease) {
                        return;
                    }
                    const auto id = pathId(req, res, "тега");
                    if (!id) {
                        return;
                    }
                    auto body = readJsonObject(req, res);
                    if (!body) {
                        return;
                    }
                    std::optional<std::int64_t> into;
                    if (!readOptionalId(*body, "into", into, res)) {
                        return;
                    }
                    if (!into) {
                        sendError(res, 400, "bad_request", "Поле 'into' должно быть числом");
                        return;
                    }
                    auto affected = ctx.services.tags->mergeTag(*lease, *id, *into);
                    if (!affected) {
                        sendError(res, affected.error());
                        return;
                    }
                    sendJson(res, Json{{"affectedEntries", *affected}});
                });

    server.Delete("/api/v1/tags/:id", [&ctx](const httplib::Request& req, httplib::Response& res) {
        auto lease = requireApi(ctx, req, res);
        if (!lease) {
            return;
        }
        const auto id = pathId(req, res, "тега");
        if (!id) {
            return;
        }
        auto affected = ctx.services.tags->removeTag(*lease, *id);
        if (!affected) {
            sendError(res, affected.error());
            return;
        }
        sendJson(res, Json{{"affectedEntries", *affected}});
    });
}

} // namespace safebox::http
