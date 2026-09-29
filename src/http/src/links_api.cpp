// /api/v1/links: пачка ссылок одним запросом
#include "dto.hpp"

namespace safebox::http {

namespace {

// "parentId": число или null (корень); поле можно не присылать. Иначе 400 уже отправлен.
[[nodiscard]] bool readOptionalParent(const Json& body, std::optional<domain::EntryId>& parent,
                                      httplib::Response& res) {
    const auto it = body.find("parentId");
    if (it == body.end() || it->is_null()) {
        parent.reset();
        return true;
    }
    if (!it->is_number_integer() || it->get<std::int64_t>() <= 0) {
        sendError(res, 400, "bad_request", "Поле 'parentId' должно быть числом или null");
        return false;
    }
    parent = it->get<domain::EntryId>();
    return true;
}

// "links": [{"url": строка, "name"?: строка, "path"?: строка}]; иначе 400 уже отправлен.
[[nodiscard]] bool readLinks(const Json& body, std::vector<app::NewLink>& links,
                             httplib::Response& res) {
    const auto it = body.find("links");
    if (it == body.end() || !it->is_array()) {
        sendError(res, 400, "bad_request", "Поле 'links' должно быть списком ссылок");
        return false;
    }
    links.reserve(it->size());
    for (const auto& item : *it) {
        const auto url = item.is_object() ? item.find("url") : item.end();
        if (url == item.end() || !url->is_string()) {
            sendError(res, 400, "bad_request", "Каждая ссылка - объект с полем 'url' (строка)");
            return false;
        }
        app::NewLink link;
        link.url = url->get<std::string>();
        std::optional<std::string> path;
        if (!readOptionalString(item, "name", link.name, res) ||
            !readOptionalString(item, "path", path, res)) {
            return false;
        }
        link.path = path.value_or(std::string{});
        links.push_back(std::move(link));
    }
    return true;
}

} // namespace

void registerLinksApi(httplib::Server& server, ApiContext& ctx) {
    server.Post(
        std::string(kLinksPath), [&ctx](const httplib::Request& req, httplib::Response& res) {
            auto lease = requireApi(ctx, req, res);
            if (!lease) {
                return;
            }
            auto body = readJsonObject(req, res);
            if (!body) {
                return;
            }
            app::CreateLinksCmd cmd;
            if (!readOptionalParent(*body, cmd.parent, res) || !readLinks(*body, cmd.links, res)) {
                return;
            }
            auto created = ctx.services.links->create(*lease, cmd);
            if (!created) {
                sendError(res, created.error());
                return;
            }
            sendJson(res, toJson(*created), 201);
        });
}

} // namespace safebox::http
