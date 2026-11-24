// /api/v1/media/:id/{thumbnail,content,download,zip}; zip - папка или запись с вложениями
// inline отдаем только фото/видео, остальное attachment, чтобы html/svg из сейфа не исполнялся.
// Ссылка без блоба скачивается ярлыком "имя.url"; content у нее 404 (отдавать нечего)
#include <algorithm>
#include <array>
#include <charconv>

#include "dto.hpp"
#include "safebox/domain/model/rules.hpp"

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

// Необязательное число из запроса; пусто -> nullopt, мусор -> false.
[[nodiscard]] bool queryNumber(const httplib::Request& req, const char* name,
                               std::optional<std::uint64_t>& out) {
    const auto text = req.get_param_value(name);
    if (text.empty()) {
        return true;
    }
    std::uint64_t value = 0;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || ptr != text.data() + text.size()) {
        return false;
    }
    out = value;
    return true;
}

// ?width=&height=&durationMs= к PUT миниатюры. Ширина без высоты - ошибка.
[[nodiscard]] std::optional<app::MediaInfo> mediaInfo(const httplib::Request& req,
                                                      httplib::Response& res) {
    std::optional<std::uint64_t> width;
    std::optional<std::uint64_t> height;
    std::optional<std::uint64_t> duration;
    if (!queryNumber(req, "width", width) || !queryNumber(req, "height", height) ||
        !queryNumber(req, "durationMs", duration) || width.has_value() != height.has_value() ||
        (width && (*width > UINT32_MAX || *height > UINT32_MAX))) {
        sendError(res, 400, "bad_request",
                  "Параметры width и height - оба целые, durationMs - целое");
        return std::nullopt;
    }
    app::MediaInfo info;
    if (width) {
        info.dimensions = domain::Dimensions{static_cast<std::uint32_t>(*width),
                                             static_cast<std::uint32_t>(*height)};
    }
    info.durationMs = duration;
    return info;
}

[[nodiscard]] bool isInlineMime(std::string_view mime) {
    static constexpr std::array<std::string_view, 11> kSafe = {
        "image/jpeg",      "image/png",        "image/gif", "image/webp",
        "image/bmp",       "image/avif",       "video/mp4", "video/webm",
        "video/quicktime", "video/x-matroska", "video/3gpp"};
    return std::find(kSafe.begin(), kSafe.end(), mime) != kSafe.end();
}

// Содержимое с известной длиной: httplib сам разбирает Range и отдает 206
// (статус не трогаем: -1 -> 200 или 206).
void serveStream(httplib::Response& res, app::OpenedContent opened, std::string_view disposition,
                 std::string mime) {
    std::shared_ptr<app::ContentStream> stream(std::move(opened.stream));
    replaceHeader(res, "Content-Disposition", contentDisposition(disposition, opened.entry.name));
    replaceHeader(res, "Accept-Ranges", "bytes");
    const auto size = stream->size();
    if (size == 0) {
        res.status = 200;
        res.set_content(std::string{}, mime);
        return;
    }
    res.set_content_provider(
        static_cast<std::size_t>(size), mime,
        [stream](std::size_t offset, std::size_t length, httplib::DataSink& sink) {
            DataSinkAdapter out(sink);
            return stream->read(offset, length, out).has_value();
        });
}

// Ярлык вместо файла: содержимого у ссылки нет, только адрес.
void serveShortcut(httplib::Response& res, const domain::Entry& entry) {
    replaceHeader(res, "Content-Disposition",
                  contentDisposition("attachment", domain::shortcutFileName(entry.name)));
    res.status = 200;
    res.set_content(domain::shortcutContent(entry.meta.url), "application/octet-stream");
}

// Архив папки называется "имя.zip", архив вложений записи - "имя (вложения).zip".
void serveZip(ApiContext& ctx, httplib::Response& res, app::Lease lease,
              const domain::Entry& entry) {
    auto shared = std::make_shared<app::Lease>(std::move(lease));
    const auto archive = entry.isFolder() ? entry.name + ".zip" : entry.name + " (вложения).zip";
    replaceHeader(res, "Content-Disposition", contentDisposition("attachment", archive));
    res.status = 200; // явно: chunked-ответ не бывает 206
    res.set_chunked_content_provider(
        "application/zip",
        [&ctx, shared, id = entry.id](std::size_t /*offset*/, httplib::DataSink& sink) {
            DataSinkAdapter out(sink);
            if (!ctx.services.importExport->exportZip(*shared, id, out)) {
                return false; // заголовки уже ушли: обрыв соединения = сбой загрузки
            }
            sink.done();
            return true;
        });
}

} // namespace

void registerMediaApi(httplib::Server& server, ApiContext& ctx) {
    server.Get("/api/v1/media/:id/thumbnail",
               [&ctx](const httplib::Request& req, httplib::Response& res) {
                   auto lease = requireMedia(ctx, req, res);
                   if (!lease) {
                       return;
                   }
                   const auto id = pathId(req, res);
                   if (!id) {
                       return;
                   }
                   auto opened = ctx.services.importExport->openContent(
                       std::move(*lease), *id, app::ContentVariant::Thumbnail);
                   if (!opened) {
                       sendError(res, opened.error());
                       return;
                   }
                   domain::MemorySink sink;
                   if (auto st = opened->stream->read(0, opened->stream->size(), sink); !st) {
                       sendError(res, st.error());
                       return;
                   }
                   res.status = 200;
                   res.set_content(std::string(domain::asChars(sink.bytes)), "image/jpeg");
               });

    // Миниатюра от клиента: тело - JPEG/PNG/WebP, сервер перекодирует; ответ - запись.
    server.Put("/api/v1/media/:id/thumbnail", [&ctx](const httplib::Request& req,
                                                     httplib::Response& res) {
        auto lease = requireApi(ctx, req, res);
        if (!lease) {
            return;
        }
        const auto id = pathId(req, res);
        if (!id) {
            return;
        }
        const auto info = mediaInfo(req, res);
        if (!info) {
            return;
        }
        if (req.body.empty()) {
            sendError(res, 400, "bad_request", "Пустое тело: нужна картинка");
            return;
        }
        const auto* data = reinterpret_cast<const std::byte*>(req.body.data());
        auto entry =
            ctx.services.importExport->setThumbnail(*lease, *id, {data, req.body.size()}, *info);
        if (!entry) {
            sendError(res, entry.error());
            return;
        }
        sendJson(res, toJson(*entry));
    });

    server.Get("/api/v1/media/:id/content", [&ctx](const httplib::Request& req,
                                                   httplib::Response& res) {
        auto lease = requireMedia(ctx, req, res);
        if (!lease) {
            return;
        }
        const auto id = pathId(req, res);
        if (!id) {
            return;
        }
        auto opened = ctx.services.importExport->openContent(std::move(*lease), *id,
                                                             app::ContentVariant::Original);
        if (!opened) {
            sendError(res, opened.error());
            return;
        }
        const auto kind = opened->entry.meta.kind;
        const bool inlineOk = (kind == domain::Kind::Photo || kind == domain::Kind::Video) &&
                              isInlineMime(opened->mime);
        auto mime = inlineOk ? opened->mime : std::string("application/octet-stream");
        serveStream(res, std::move(*opened), inlineOk ? "inline" : "attachment", std::move(mime));
    });

    server.Get("/api/v1/media/:id/download",
               [&ctx](const httplib::Request& req, httplib::Response& res) {
                   auto lease = requireMedia(ctx, req, res);
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
                   if (entry->isFolder()) {
                       serveZip(ctx, res, std::move(*lease), *entry);
                       return;
                   }
                   if (entry->meta.kind == domain::Kind::Link && !entry->meta.blobId) {
                       serveShortcut(res, *entry);
                       return;
                   }
                   auto opened = ctx.services.importExport->openContent(
                       std::move(*lease), *id, app::ContentVariant::Original);
                   if (!opened) {
                       sendError(res, opened.error());
                       return;
                   }
                   serveStream(res, std::move(*opened), "attachment", "application/octet-stream");
               });

    server.Get("/api/v1/media/:id/zip",
               [&ctx](const httplib::Request& req, httplib::Response& res) {
                   auto lease = requireMedia(ctx, req, res);
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
                   if (!entry->isFolder() && entry->childCount == 0) {
                       sendError(res, 422, "invalid_argument",
                                 "Архивом скачивается папка или запись с вложениями");
                       return;
                   }
                   serveZip(ctx, res, std::move(*lease), *entry);
               });
}

} // namespace safebox::http
