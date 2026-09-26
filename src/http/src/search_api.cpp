// GET /api/v1/search?q=&limit=
#include <charconv>

#include "dto.hpp"

namespace safebox::http {

void registerSearchApi(httplib::Server& server, ApiContext& ctx) {
    server.Get("/api/v1/search", [&ctx](const httplib::Request& req, httplib::Response& res) {
        auto lease = requireApi(ctx, req, res);
        if (!lease) {
            return;
        }
        const auto query = req.get_param_value("q");
        std::size_t limit = app::kDefaultSearchLimit;
        if (const auto text = req.get_param_value("limit"); !text.empty()) {
            const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), limit);
            if (ec != std::errc{} || ptr != text.data() + text.size() || limit == 0) {
                sendError(res, 400, "bad_request", "Некорректный параметр limit");
                return;
            }
        }
        auto hits = ctx.services.search->search(*lease, query, limit);
        if (!hits) {
            sendError(res, hits.error());
            return;
        }
        Json results = Json::array();
        for (const auto& hit : *hits) {
            results.push_back(toJson(hit));
        }
        sendJson(res, Json{{"query", query}, {"results", std::move(results)}});
    });
}

} // namespace safebox::http
