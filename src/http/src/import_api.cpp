// POST /api/v1/import - multipart читаем потоком и сразу отдаем в ImportSession,
// целиком в память не собираем. filename может быть с относительным путем (webkitRelativePath).
// Первая часть name="manifest" (без filename) - JSON с датой и решением по каждому файлу.
// POST /api/v1/import/plan - какие файлы столкнутся по имени с записями сейфа
#include <cmath>
#include <unordered_map>
#include <utility>

#include "dto.hpp"

namespace safebox::http {

namespace {

constexpr std::size_t kMaxPlanFiles = 100'000;

using Manifest = std::unordered_map<std::string, app::ImportFileOptions>; // по filename

// Необязательный query-параметр parentId: пусто - корень; иначе 400 уже отправлен.
[[nodiscard]] bool readParentParam(const httplib::Request& req,
                                   std::optional<domain::EntryId>& parent, httplib::Response& res) {
    if (!req.has_param("parentId") || req.get_param_value("parentId").empty()) {
        return true;
    }
    parent = parseId(req.get_param_value("parentId"));
    if (!parent) {
        sendError(res, 400, "bad_request", "Некорректный идентификатор объекта");
        return false;
    }
    return true;
}

// {"files": {"<путь>": {"lastModified"?: мс, "onConflict"?: "keepBoth"|"replace"|"skip"}}};
// nullopt - тело не такое.
[[nodiscard]] std::optional<Manifest> parseManifest(std::string_view text) {
    const Json root = Json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (!root.is_object()) {
        return std::nullopt;
    }
    Manifest manifest;
    const auto files = root.find("files");
    if (files == root.end()) {
        return manifest;
    }
    if (!files->is_object()) {
        return std::nullopt;
    }
    for (const auto& [path, item] : files->items()) {
        if (!item.is_object()) {
            return std::nullopt;
        }
        app::ImportFileOptions options;
        if (const auto it = item.find("lastModified"); it != item.end() && !it->is_null()) {
            if (!it->is_number()) {
                return std::nullopt;
            }
            const auto ms = it->get<double>();
            if (!(std::fabs(ms) < 9e15)) { // за пределами точности double
                return std::nullopt;
            }
            options.sourceModifiedAt = static_cast<std::int64_t>(std::floor(ms));
        }
        if (const auto it = item.find("onConflict"); it != item.end() && !it->is_null()) {
            const auto policy =
                it->is_string() ? parseConflictPolicy(it->get<std::string>()) : std::nullopt;
            if (!policy) {
                return std::nullopt;
            }
            options.onConflict = *policy;
        }
        manifest.emplace(path, options);
    }
    return manifest;
}

// Отказ самого http-слоя: разбор тела, а не сервис.
struct Rejection {
    int status = 400;
    std::string code = "bad_request";
    std::string message;
};

} // namespace

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
        if (!readParentParam(req, parent, res)) {
            return;
        }
        auto session = ctx.services.importExport->beginImport(std::move(*lease), parent);
        if (!session) {
            sendError(res, session.error());
            return;
        }
        app::ImportSession& import = **session;

        std::optional<domain::Error> fatal;
        std::optional<Rejection> rejection;
        enum class Part { Other, Manifest, File };
        Part part = Part::Other;
        std::size_t partCount = 0;
        std::string manifestText;
        Manifest manifest;

        // Часть закончилась: файл дописывается, manifest разбирается.
        const auto closePart = [&] {
            const auto closed = std::exchange(part, Part::Other);
            if (closed == Part::File) {
                if (auto st = import.endFile(); !st) {
                    fatal = st.error();
                    return false;
                }
            } else if (closed == Part::Manifest) {
                auto parsed = parseManifest(manifestText);
                manifestText = {};
                if (!parsed) {
                    rejection = Rejection{400, "bad_request", "Некорректный manifest импорта"};
                    return false;
                }
                manifest = std::move(*parsed);
            }
            return true;
        };

        const bool read = reader(
            [&](const httplib::FormData& form) {
                if (!closePart()) {
                    return false;
                }
                const bool first = partCount++ == 0;
                if (form.filename.empty() && form.name == "manifest") {
                    if (!first) {
                        rejection = Rejection{400, "bad_request",
                                              "manifest должен быть первой частью запроса"};
                        return false;
                    }
                    part = Part::Manifest;
                    return true;
                }
                if (form.filename.empty()) {
                    return true; // текстовое поле формы
                }
                app::ImportFileOptions options;
                if (const auto it = manifest.find(form.filename); it != manifest.end()) {
                    options = it->second;
                }
                if (auto st = import.beginFile(form.filename, options); !st) {
                    fatal = st.error();
                    return false;
                }
                part = Part::File;
                return true;
            },
            [&](const char* data, std::size_t length) {
                if (part == Part::Manifest) {
                    if (manifestText.size() + length > kMaxImportJsonBytes) {
                        rejection =
                            Rejection{413, "payload_too_large", "Слишком большой manifest импорта"};
                        return false;
                    }
                    manifestText.append(data, length);
                    return true;
                }
                if (part != Part::File || length == 0) {
                    return true;
                }
                const auto bytes = std::as_bytes(std::span<const char>(data, length));
                if (auto st = import.write(bytes); !st) {
                    fatal = st.error();
                    return false;
                }
                return true;
            });
        if (read) {
            (void)closePart(); // последняя часть; причина отказа уже в fatal/rejection
        }
        if (fatal) {
            sendError(res, *fatal);
            return;
        }
        if (rejection) {
            sendError(res, rejection->status, rejection->code, rejection->message);
            return;
        }
        if (!read) {
            sendError(res, 400, "bad_request", "Не удалось прочитать тело запроса");
            return;
        }
        sendJson(res, toJson(import.finish()));
    });

    server.Post(std::string(kImportPlanPath), [&ctx](const httplib::Request& req,
                                                     httplib::Response& res) {
        auto lease = requireApi(ctx, req, res);
        if (!lease) {
            return;
        }
        std::optional<domain::EntryId> parent;
        if (!readParentParam(req, parent, res)) {
            return;
        }
        auto body = readJsonObject(req, res);
        if (!body) {
            return;
        }
        const auto list = body->find("files");
        if (list == body->end() || !list->is_array()) {
            sendError(res, 400, "bad_request", "Поле 'files' должно быть массивом");
            return;
        }
        if (list->size() > kMaxPlanFiles) {
            sendError(res, 422, "invalid_argument", "В плане не больше 100 000 файлов");
            return;
        }
        std::vector<app::ImportPlanFile> files;
        files.reserve(list->size());
        for (const auto& item : *list) {
            const auto path = item.is_object() ? item.find("path") : item.end();
            const auto size = item.is_object() ? item.find("size") : item.end();
            if (path == item.end() || !path->is_string() ||
                (size != item.end() && !size->is_number_unsigned())) {
                sendError(res, 400, "bad_request",
                          "Каждый файл плана - объект с путем 'path' и размером 'size'");
                return;
            }
            files.push_back({path->get<std::string>(),
                             size == item.end() ? std::uint64_t{0} : size->get<std::uint64_t>()});
        }
        auto plan = ctx.services.importExport->planImport(*lease, parent, files);
        if (!plan) {
            sendError(res, plan.error());
            return;
        }
        sendJson(res, toJson(*plan));
    });
}

} // namespace safebox::http
