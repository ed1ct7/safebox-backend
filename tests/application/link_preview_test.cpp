#include <catch2/catch_test_macros.hpp>

#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "FakePageFetcher.hpp"
#include "safebox/domain/model/link_preview.hpp"

using namespace safebox;
using domain::parseLinkPreview;
using Urls = std::vector<std::string>;

namespace {

constexpr std::string_view kBase = "https://example.com/blog/post.html";

std::string page(std::string_view head) {
    std::string html = "<!doctype html>\n<html>\n<head>\n<meta charset=\"utf-8\">\n";
    html.append(head);
    html.append("\n</head>\n<body><h1>Body</h1></body></html>");
    return html;
}

domain::LinkPreview parseHead(std::string_view head, std::string_view base = kBase) {
    return parseLinkPreview(page(head), base);
}

// Первая картинка, которую предпросмотр нашел бы по og:image со ссылкой ref.
std::string resolved(std::string_view base, std::string_view ref) {
    std::string tag = "<meta property=\"og:image\" content=\"";
    tag.append(ref);
    tag.append("\">");
    const auto preview = parseHead(tag, base);
    return preview.imageUrls.empty() ? std::string{} : preview.imageUrls.front();
}

} // namespace

TEST_CASE("og tags beat twitter tags, which beat plain title and description",
          "[link_preview][priority]") {
    const std::string plain = R"html(
        <title>Plain title</title>
        <meta name="description" content="Plain description">)html";
    const std::string twitter = R"html(
        <meta name="twitter:title" content="Twitter title">
        <meta name="twitter:description" content="Twitter description">)html";
    const std::string og = R"html(
        <meta property="og:title" content="OG title">
        <meta property="og:description" content="OG description">)html";

    // порядок в документе не важен
    auto all = parseHead(plain + og + twitter);
    CHECK(all.title == "OG title");
    CHECK(all.description == "OG description");

    auto noOg = parseHead(twitter + plain);
    CHECK(noOg.title == "Twitter title");
    CHECK(noOg.description == "Twitter description");

    auto onlyPlain = parseHead(plain);
    CHECK(onlyPlain.title == "Plain title");
    CHECK(onlyPlain.description == "Plain description");

    auto nothing = parseHead("");
    CHECK(nothing.title.empty());
    CHECK(nothing.description.empty());
    CHECK(nothing.imageUrls.empty());
}

TEST_CASE("blank values fall through to the next source", "[link_preview][priority]") {
    auto preview = parseHead(R"html(
        <meta property="og:title" content="   ">
        <meta property="og:description" content="">
        <title>Fallback</title>
        <meta name="description" content="Fallback description">)html");
    CHECK(preview.title == "Fallback");
    CHECK(preview.description == "Fallback description");

    auto second = parseHead(R"html(
        <meta property="og:title" content=" ">
        <meta property="og:title" content="Second">
        <title>  </title><title>Real title</title>)html");
    CHECK(second.title == "Second");
    CHECK(parseHead("<title></title><title> Real title </title>").title == "Real title");
    CHECK(parseHead("<title>First</title><title>Second</title>").title == "First");
    // og:title без content - пропускается
    CHECK(parseHead(R"html(<meta property="og:title"><title>T</title>)html").title == "T");
}

TEST_CASE("tag and attribute names ignore case, values keep it", "[link_preview]") {
    auto preview = parseHead(R"html(
        <META PROPERTY="OG:TITLE" CONTENT="Shouting Title">
        <Meta Name="Description" Content="Mixed Description">
        <LINK REL="Icon" HREF="/Favicon.PNG">)html");
    CHECK(preview.title == "Shouting Title");
    CHECK(preview.description == "Mixed Description");
    CHECK(preview.imageUrls == Urls{"https://example.com/Favicon.PNG"});

    CHECK(parseHead("<TITLE>Upper</TITLE>").title == "Upper");
    CHECK(parseHead("<tItLe>Mixed</TiTlE>").title == "Mixed");
    // og-свойство можно задать и через name
    CHECK(parseHead(R"html(<meta name="og:title" content="By name">)html").title == "By name");
}

TEST_CASE("attribute values: double, single and no quotes", "[link_preview][attributes]") {
    CHECK(parseHead(R"html(<meta property='og:title' content='Single "quoted"'>)html").title ==
          "Single \"quoted\"");
    CHECK(parseHead(R"html(<meta property="og:title" content="It's double">)html").title ==
          "It's double");
    CHECK(parseHead("<meta property=og:title content=Bare>").title == "Bare");
    CHECK(parseHead(R"html(<meta property = "og:title"   content =   'Spaced' >)html").title ==
          "Spaced");
    CHECK(parseHead(R"html(<meta content="Reversed" property="og:title">)html").title ==
          "Reversed");
    CHECK(parseHead(R"html(<meta property="og:title" content="a > b < c">)html").title ==
          "a > b < c");
    CHECK(parseHead(R"html(<meta property="og:title" content="A" />)html").title == "A");
    CHECK(parseHead("<meta\n  property=\"og:title\"\n  content=\"Line\n   break\"\n>").title ==
          "Line break");
    // повторный атрибут: как в браузере, работает первый
    CHECK(parseHead(R"html(<meta property="og:title" content="One" content="Two">)html").title ==
          "One");
    // булев атрибут без значения не сбивает разбор
    CHECK(parseHead(R"html(<meta data-x property="og:title" content="After">)html").title ==
          "After");
}

TEST_CASE("HTML entities are decoded", "[link_preview][entities]") {
    auto title = [](std::string_view text) {
        std::string tag = "<title>";
        tag.append(text);
        tag.append("</title>");
        return parseHead(tag).title;
    };
    CHECK(title("Tom &amp; Jerry &lt;3 &gt; &quot;q&quot; &#39;s&#39; &apos;x&apos;&nbsp;y") ==
          "Tom & Jerry <3 > \"q\" 's' 'x' y");
    CHECK(title("&#1055;&#x440;&#x438;&#1074;&#x435;&#X442;") == "Привет");
    CHECK(title("&laquo;Название&raquo; &mdash; &hellip;") ==
          "«Название» \xE2\x80\x94 \xE2\x80\xA6"); // тире и многоточие
    CHECK(title("smile &#x1F600;!") == "smile \xF0\x9F\x98\x80!");
    // это не двойное декодирование
    CHECK(title("&amp;lt;") == "&lt;");

    // битые и неизвестные сущности остаются как есть
    const std::string broken = "a &amp b &#xZZ; c &#99999999; d &bogus; e & f &#0; g AT&T &; &#;";
    CHECK(title(broken) == broken);
    CHECK(title("&#xD800; &#x110000;") == "&#xD800; &#x110000;");
    CHECK(title("x&#7;y") == "xy"); // управляющий символ выброшен

    // и в описании, и в адресе картинки
    auto preview = parseHead(R"html(
        <meta name="description" content="Fish &amp; chips">
        <meta property="og:image" content="/i.png?a=1&amp;b=2">)html");
    CHECK(preview.description == "Fish & chips");
    CHECK(preview.imageUrls == Urls{"https://example.com/i.png?a=1&b=2"});
}

TEST_CASE("whitespace is collapsed and trimmed", "[link_preview][entities]") {
    CHECK(parseHead("<title>\n   Two \t\r\n  lines   </title>").title == "Two lines");
    CHECK(parseHead("<title>A\xC2\xA0\xC2\xA0"
                    "B&nbsp;&nbsp;C&#160;D</title>")
              .title == "A B C D");
    CHECK(parseHead("<title>&nbsp; padded &#32;</title>").title == "padded");
    CHECK(parseHead("<title> \n\t </title>").title.empty());
}

TEST_CASE("relative image URLs become absolute", "[link_preview][urls]") {
    constexpr std::string_view base = "https://example.com/blog/2024/post.html?x=1#top";
    CHECK(resolved(base, "https://cdn.test/a.png") == "https://cdn.test/a.png");
    CHECK(resolved(base, "http://other.test/x.png") == "http://other.test/x.png");
    CHECK(resolved(base, "//cdn.example.org/a.png") == "https://cdn.example.org/a.png");
    CHECK(resolved(base, "/img/a.png") == "https://example.com/img/a.png");
    CHECK(resolved(base, "a.png") == "https://example.com/blog/2024/a.png");
    CHECK(resolved(base, "./a.png") == "https://example.com/blog/2024/a.png");
    CHECK(resolved(base, "../a.png") == "https://example.com/blog/a.png");
    CHECK(resolved(base, "../../a.png") == "https://example.com/a.png");
    CHECK(resolved(base, "../../../../a.png") == "https://example.com/a.png");
    CHECK(resolved(base, "sub/dir/../x.png") == "https://example.com/blog/2024/sub/x.png");
    CHECK(resolved(base, "/a/b/../../c.png") == "https://example.com/c.png");
    CHECK(resolved(base, "?v=2") == "https://example.com/blog/2024/post.html?v=2");
    CHECK(resolved(base, "  /trimmed.png\n") == "https://example.com/trimmed.png");
    CHECK(resolved(base, "/tab\tbreak.png") == "https://example.com/tabbreak.png");
    CHECK(resolved(base, "/cyr/картинка.png") == "https://example.com/cyr/картинка.png");
    CHECK(resolved(base, "/a.png#frag") == "https://example.com/a.png#frag");
    CHECK(resolved(base, "/a/../b.png?u=../x") == "https://example.com/b.png?u=../x");

    // разные виды базового адреса
    CHECK(resolved("https://example.com", "a.png") == "https://example.com/a.png");
    CHECK(resolved("https://example.com?x=1", "a.png") == "https://example.com/a.png");
    CHECK(resolved("https://example.com/dir/", "a.png") == "https://example.com/dir/a.png");
    CHECK(resolved("http://localhost:8080/a/b", "c.png") == "http://localhost:8080/a/c.png");
    CHECK(resolved("http://user:pw@host.test/p/q", "/z.png") == "http://user:pw@host.test/z.png");
    CHECK(resolved("http://h.test/a/b", "//other.test:81/x.png") == "http://other.test:81/x.png");

    // без годного базового адреса работают только абсолютные
    CHECK(resolved("", "https://x.test/a.png") == "https://x.test/a.png");
    CHECK(resolved("", "a.png").empty());
    CHECK(resolved("", "/a.png").empty());
    CHECK(resolved("", "//x.test/a.png").empty());
    CHECK(resolved("not a url", "/a.png").empty());
    CHECK(resolved("ftp://x.test/dir/", "a.png").empty());
}

TEST_CASE("images that are not http(s) URLs are dropped", "[link_preview][urls]") {
    for (const auto* ref :
         {"data:image/png;base64,iVBORw0KGgo=", "javascript:alert(1)", "JAVASCRIPT:alert(1)",
          "ftp://x.test/a.png", "mailto:a@b.test", "blob:https://x.test/uuid", "file:///C:/a.png",
          "about:blank", "C:\\images\\a.png", "http:relative.png", "", "   ", "http://",
          "https:///a.png", "https://?x=1", "http://x.test/a b.png"}) {
        INFO("ref: " << ref);
        CHECK(resolved(kBase, ref).empty());
    }
    auto preview = parseHead(R"html(
        <link rel="icon" href="data:image/x-icon;base64,AAAA">
        <link rel="apple-touch-icon" href="javascript:void(0)">
        <meta name="twitter:image" content="ftp://x.test/a.png">)html");
    CHECK(preview.imageUrls.empty());
    // мусор не мешает найти нормальную
    auto mixed = parseHead(R"html(
        <meta property="og:image" content="data:image/png;base64,AAAA">
        <meta property="og:image" content="/real.png">)html");
    CHECK(mixed.imageUrls == Urls{"https://example.com/real.png"});
}

TEST_CASE("images come in priority order: og, twitter, apple-touch-icon, icon",
          "[link_preview][priority]") {
    auto preview = parseHead(R"html(
        <link rel="icon" href="/favicon.png">
        <link rel="apple-touch-icon" href="/apple.png">
        <meta name="twitter:image" content="/tw.png">
        <meta property="og:image" content="/og1.png">
        <meta property="og:image" content="/og2.png">)html");
    CHECK(preview.imageUrls == Urls{"https://example.com/og1.png", "https://example.com/og2.png",
                                    "https://example.com/tw.png", "https://example.com/apple.png",
                                    "https://example.com/favicon.png"});

    // apple-touch-icon после og:image, но раньше обычной иконки, что бы ни было в документе
    auto icons = parseHead(R"html(
        <link rel="shortcut icon" href="/a.ico">
        <link rel="apple-touch-icon-precomposed" href="/pre.png">
        <link rel="ICON" href="/b.png">)html");
    CHECK(icons.imageUrls == Urls{"https://example.com/pre.png", "https://example.com/a.ico",
                                  "https://example.com/b.png"});
}

TEST_CASE("only real icon links count, duplicates are dropped", "[link_preview][priority]") {
    auto preview = parseHead(R"html(
        <link rel="stylesheet" href="/site.css">
        <link rel="mask-icon" href="/mask.svg">
        <link rel="preload" as="image" href="/hero.png">
        <link rel="canonical" href="https://example.com/post">
        <link rel="icon">
        <link href="/no-rel.png">
        <link rel="  shortcut   icon  " href="  /spaced.png ">
        <meta property="og:image">
        <meta property="og:image" content="/dup.png">
        <meta property="og:image" content="/dup.png">
        <link rel="icon" href="/dup.png">)html");
    CHECK(preview.imageUrls ==
          Urls{"https://example.com/dup.png", "https://example.com/spaced.png"});
}

TEST_CASE("the number of candidate images is limited", "[link_preview][priority]") {
    std::string head;
    for (int i = 0; i < 40; ++i) {
        head += "<meta property=\"og:image\" content=\"/og" + std::to_string(i) + ".png\">\n";
        head += "<link rel=\"icon\" href=\"/icon" + std::to_string(i) + ".png\">\n";
    }
    auto preview = parseHead(head);
    REQUIRE(preview.imageUrls.size() == domain::kMaxPreviewImages);
    CHECK(preview.imageUrls.front() == "https://example.com/og0.png");
    CHECK(preview.imageUrls.back() == "https://example.com/og7.png");
}

TEST_CASE("only the head is inspected", "[link_preview][head]") {
    // после </head> (любой регистр) ничего не берется
    auto afterHead =
        parseLinkPreview("<html><head><title>Head</title></HEAD><body>"
                         "<meta property=\"og:title\" content=\"Body\">"
                         "<meta property=\"og:image\" content=\"/body.png\"></body></html>",
                         kBase);
    CHECK(afterHead.title == "Head");
    CHECK(afterHead.imageUrls.empty());

    // без </head> голова кончается на <body>
    auto noHeadEnd = parseLinkPreview("<title>Top</title><body><title>Inline svg</title>"
                                      "<meta property=\"og:title\" content=\"Body\">",
                                      kBase);
    CHECK(noHeadEnd.title == "Top");

    // <header> и <head-подобные> теги голову не закрывают
    CHECK(parseLinkPreview("<head></header><meta property=\"og:title\" content=\"Still head\">",
                           kBase)
              .title == "Still head");
    // страница без <head> вообще
    CHECK(parseLinkPreview("<title>No head</title><p>text", kBase).title == "No head");
}

TEST_CASE("comments, scripts and styles are skipped", "[link_preview][head]") {
    auto preview = parseHead(R"html(
        <!-- <meta property="og:title" content="Commented"> -->
        <!-- </head> -->
        <script>
            var s = '</head><meta property="og:title" content="InScript">';
            document.write("<title>ScriptTitle</title>");
        </script>
        <style>/* <meta property="og:description" content="InStyle"> */</style>
        <!DOCTYPE junk>
        <?xml version="1.0"?>
        <meta property="og:title" content="Real">
        <meta property="og:description" content="Real description">)html");
    CHECK(preview.title == "Real");
    CHECK(preview.description == "Real description");

    // незакрытый <script> съедает остаток
    CHECK(parseHead("<script>var x;").title.empty());
    CHECK(parseLinkPreview("<title>T</title><script>never closed <meta property=\"og:title\" "
                           "content=\"Hidden\">",
                           kBase)
              .title == "T");
}

TEST_CASE("only the first 512 KiB are inspected", "[link_preview][head]") {
    std::string html = "<head><meta property=\"og:title\" content=\"Early\">";
    html.append(domain::kPreviewScanBytes, ' ');
    html.append("<meta property=\"og:description\" content=\"Late\"></head>");
    auto preview = parseLinkPreview(html, kBase);
    CHECK(preview.title == "Early");
    CHECK(preview.description.empty());

    // тег, разрезанный границей, не считается
    std::string cut = "<head>";
    cut.append(domain::kPreviewScanBytes - cut.size() - 20, ' ');
    cut.append("<meta property=\"og:title\" content=\"Cut\">");
    CHECK(parseLinkPreview(cut, kBase).title.empty());
}

TEST_CASE("<title> without an end tag ends at the next tag", "[link_preview][head]") {
    CHECK(parseHead("<title>No end").title == "No end");
    CHECK(parseLinkPreview("<title>Tail", kBase).title == "Tail");
    CHECK(parseLinkPreview("<title><title>Second", kBase).title == "Second");
}

TEST_CASE("garbage and empty input do not fail", "[link_preview][robustness]") {
    const std::string nul("a\0b<\0>", 6);
    for (const auto html : {std::string_view(""),
                            std::string_view("<"),
                            std::string_view("<<<<"),
                            std::string_view(">>>>"),
                            std::string_view("</"),
                            std::string_view("</>"),
                            std::string_view("<meta"),
                            std::string_view("<meta property="),
                            std::string_view("<meta property=\"og:title\" content=\"cut"),
                            std::string_view("<meta property=og:title content="),
                            std::string_view("<!--"),
                            std::string_view("<!-- x"),
                            std::string_view("<!"),
                            std::string_view("<?"),
                            std::string_view("<title>"),
                            std::string_view("<script>"),
                            std::string_view("<link rel"),
                            std::string_view("<a<b<c"),
                            std::string_view("&&&&&&&&&&&&"),
                            std::string_view("=\"\"'' ///"),
                            std::string_view(nul)}) {
        INFO("html size " << html.size());
        const auto preview = parseLinkPreview(html, kBase);
        CHECK(domain::isValidUtf8(preview.title));
        CHECK(domain::isValidUtf8(preview.description));
    }
    CHECK(parseLinkPreview("", "").title.empty());
    CHECK(parseLinkPreview("<title>x</title>", "").title == "x");
    CHECK(parseHead("<meta property=\"og:title\" content=\"cut").title.empty());
}

TEST_CASE("random bytes do not fail and yield valid UTF-8", "[link_preview][robustness]") {
    std::mt19937 rng(7);
    const std::string_view alphabet = "<>/=\"' &;#xmetalinkproperty:ogtitleicon\xC3\xFF\x80\n";
    for (int round = 0; round < 50; ++round) {
        std::string junk(4096, '\0');
        for (auto& c : junk) {
            c = alphabet[rng() % alphabet.size()];
        }
        const auto preview = parseLinkPreview(junk, kBase);
        CHECK(domain::isValidUtf8(preview.title));
        CHECK(domain::isValidUtf8(preview.description));
        for (const auto& url : preview.imageUrls) {
            CHECK(domain::isValidUtf8(url));
        }
    }
    std::string binary(64 * 1024, '\0');
    for (auto& c : binary) {
        c = static_cast<char>(rng());
    }
    CHECK(domain::isValidUtf8(parseLinkPreview(binary, kBase).title));
}

TEST_CASE("pathological input stays linear", "[link_preview][robustness]") {
    std::string titles;
    while (titles.size() < domain::kPreviewScanBytes) {
        titles += "<title>";
    }
    CHECK(parseLinkPreview(titles, kBase).title.size() < 10);

    std::string ampersands = "<meta property=\"og:title\" content=\"";
    ampersands.append(400 * 1024, '&');
    ampersands.append("\">");
    CHECK(parseLinkPreview(ampersands, kBase).title.size() == 400 * 1024);

    std::string tags;
    while (tags.size() < domain::kPreviewScanBytes) {
        tags += "<a<a<a<a x='";
    }
    CHECK(parseLinkPreview(tags, kBase).title.empty());
}

TEST_CASE("invalid UTF-8 is replaced with question marks", "[link_preview][utf8]") {
    auto preview = parseHead("<meta property=\"og:title\" content=\"bad \xFF\xFE bytes \xC3\">");
    CHECK(preview.title == "bad ?? bytes ?");
    CHECK(parseHead("<title>over\xC0\x80long</title>").title == "over??long");
    CHECK(parseHead("<title>surrogate \xED\xA0\x80!</title>").title == "surrogate ???!");
    CHECK(parseHead("<meta name=\"description\" content=\"\xF0\x9F\">").description == "??");

    // корректные многобайтные последовательности не трогаем
    auto valid = parseHead("<title>Привет, мир! \xE2\x82\xAC \xF0\x9F\x98\x80</title>");
    CHECK(valid.title == "Привет, мир! € \xF0\x9F\x98\x80");

    // адрес с битыми байтами - не адрес
    CHECK(parseHead("<meta property=\"og:image\" content=\"/a\xFF.png\">").imageUrls.empty());
}

TEST_CASE("a very long description is cut on a character boundary", "[link_preview][utf8]") {
    std::string head = "<meta name=\"description\" content=\"";
    for (int i = 0; i < 70'000; ++i) {
        head += "я";
    }
    head += "\">";
    const auto preview = parseHead(head);
    CHECK(preview.description.size() == domain::kMaxDescriptionBytes);
    CHECK(domain::isValidUtf8(preview.description));
    CHECK(preview.description.starts_with("яя"));
}

TEST_CASE("the final URL of a fetch is the base for relative addresses",
          "[link_preview][fetcher]") {
    test::FakePageFetcher fetcher;
    fetcher.addHtml(
        "https://short.test/x",
        R"html(<head><title>T</title><meta property="og:image" content="img/cover.png"></head>)html",
        "https://example.com/articles/2024/full.html");
    fetcher.addImage("https://example.com/articles/2024/img/cover.png", domain::toBytes("PNG"));
    fetcher.addError("https://down.test/", "Не удалось найти сайт");

    const auto response = fetcher.fetch(domain::FetchRequest{.url = "https://short.test/x"});
    REQUIRE(response.has_value());
    CHECK(response->status == 200);
    CHECK(response->contentType == "text/html; charset=utf-8");
    const auto preview = parseLinkPreview(domain::asChars(response->body), response->finalUrl);
    CHECK(preview.title == "T");
    REQUIRE(preview.imageUrls == Urls{"https://example.com/articles/2024/img/cover.png"});

    const auto image = fetcher.fetch(domain::FetchRequest{.url = preview.imageUrls.front()});
    REQUIRE(image.has_value());
    CHECK(image->contentType == "image/png");
    CHECK(image->body == domain::toBytes("PNG"));

    const auto down = fetcher.fetch(domain::FetchRequest{.url = "https://down.test/"});
    REQUIRE_FALSE(down.has_value());
    CHECK(down.error().code == domain::Error::Code::IoError);
    CHECK(down.error().message == "Не удалось найти сайт");
    const auto unknown = fetcher.fetch(domain::FetchRequest{.url = "https://unknown.test/"});
    REQUIRE_FALSE(unknown.has_value());
    CHECK(unknown.error().code == domain::Error::Code::IoError);

    CHECK(fetcher.calls.load() == 4);
    CHECK(fetcher.requested() == Urls{"https://short.test/x",
                                      "https://example.com/articles/2024/img/cover.png",
                                      "https://down.test/", "https://unknown.test/"});
}
