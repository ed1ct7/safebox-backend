// POST /api/v1/import - multipart читаем потоком и сразу отдаем в ImportSession,
// целиком в память не собираем. filename может быть с относительным путем (webkitRelativePath)
#include "dto.hpp"

namespace safebox::http {

void registerImportApi(httplib::Server& server, ApiContext& ctx) {
    server.Post(std::string(kImportPath), [&ctx](const httplib::Request& req,
                                                 httplib::Response& res,
                                                 const httplib::ContentReader& reader) {
        auto lease = requireApi(ctx, req, res);
        if (!lease) {
            return;
        }
        if (!req.is_multipart_form_data()) {
            sendError(res, 415, "unsupported_media_type", "Ожидается multipart/form-data");
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
        auto session = ctx.services.importExport->beginImport(std::move(*lease), parent);
        if (!session) {
            sendError(res, session.error());
            return;
        }
        app::ImportSession& import = **session;

        std::optional<domain::Error> fatal;
        bool inFile = false;
        const bool read = reader(
            [&](const httplib::FormData& part) {
                if (inFile) {
                    inFile = false;
                    if (auto st = import.endFile(); !st) {
                        fatal = st.error();
                        return false;
                    }
                }
                if (part.filename.empty()) {
                    return true; // текстовое поле формы
                }
                if (auto st = import.beginFile(part.filename); !st) {
                    fatal = st.error();
                    return false;
                }
                inFile = true;
                return true;
            },
            [&](const char* data, std::size_t length) {
                if (!inFile || length == 0) {
                    return true;
                }
                const auto bytes = std::as_bytes(std::span<const char>(data, length));
                if (auto st = import.write(bytes); !st) {
                    fatal = st.error();
                    return false;
                }
                return true;
            });
        if (read && inFile) {
            if (auto st = import.endFile(); !st) {
                fatal = st.error();
            }
        }
        if (fatal) {
            sendError(res, *fatal);
            return;
        }
        if (!read) {
            sendError(res, 400, "bad_request", "Не удалось прочитать тело запроса");
            return;
        }
        sendJson(res, toJson(import.finish()));
    });
}

} // namespace safebox::http
