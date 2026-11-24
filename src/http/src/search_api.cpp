// GET /api/v1/search?q=&tags=1,2,3&match=categories|all|any&within=<id>&sort=&order=&offset=&limit=
// Без limit выдаются все совпадения; total - сколько их всего.
#include "dto.hpp"

namespace safebox::http {

namespace {

// "1,2,3"; пустая строка - без тегов. Иначе 400 уже отправлен.
[[nodiscard]] bool readTags(std::string_view text, std::vector<domain::TagId>& tags,
                            httplib::Response& res) {
    if (text.empty()) {
        return true;
    }
    for (;;) {
        const auto comma = text.find(',');
        const auto id = parseId(text.substr(0, comma)); // пустой кусок ("1,,2", "1,") - ошибка
        if (!id) {
            sendError(res, 400, "bad_request", "Некорректный параметр tags");
            return false;
        }
        tags.push_back(*id);
        if (comma == std::string_view::npos) {
            return true;
        }
        text.remove_prefix(comma + 1);
    }
}

} // namespace

void registerSearchApi(httplib::Server& server, ApiContext& ctx) {
    server.Get("/api/v1/search", [&ctx](const httplib::Request& req, httplib::Response& res) {
        auto lease = requireApi(ctx, req, res);
        if (!lease) {
            return;
        }
        app::SearchQuery query;
        query.text = req.get_param_value("q");
        if (!readListOptions(req, query.page, res)) {
            return;
        }
        if (!readTags(req.get_param_value("tags"), query.tags, res)) {
            return;
        }
        if (const auto text = req.get_param_value("match"); !text.empty()) {
            if (text == "categories") {
                query.match = app::TagMatch::Categories;
            } else if (text == "all") {
                query.match = app::TagMatch::All;
            } else if (text == "any") {
                query.match = app::TagMatch::Any;
            } else {
                sendError(res, 400, "bad_request", "Некорректный параметр match");
                return;
            }
        }
        if (const auto text = req.get_param_value("within"); !text.empty()) {
            query.within = parseId(text);
            if (!query.within) {
                sendError(res, 400, "bad_request", "Некорректный параметр within");
                return;
            }
        }
        auto found = ctx.services.search->search(*lease, query);
        if (!found) {
            sendError(res, found.error());
            return;
        }
        Json results = Json::array();
        for (const auto& hit : found->hits) {
            results.push_back(toJson(hit));
        }
        sendJson(
            res,
            Json{{"query", query.text}, {"results", std::move(results)}, {"total", found->total}});
    });
}

} // namespace safebox::http
