// SearchService: текст в имени и описании, фильтр по тегам (прямым и унаследованным), режимы
// сочетания, область within.
#include "app_fixture.hpp"

using namespace safebox;
using namespace safebox::test;
using Code = domain::Error::Code;
using app::TagMatch;
using domain::MatchedIn;
using Names = std::vector<std::string>;

namespace {

// Отпуск/ (Крым для всех вложенных), в нем Море/ с двумя фото и план.txt; Работа/отчёт.pdf;
// заметки.txt в корне.
struct FilterVault {
    FilterVault() {
        REQUIRE(f.importFiles(s, {{"Отпуск/Море/Ёлка на пляже.jpg", fakeJpeg(600)},
                                  {"Отпуск/Море/закат.jpg", fakeJpeg(700, 8)},
                                  {"Отпуск/план.txt", "p"},
                                  {"Работа/отчёт.pdf", "%PDF-1.4"},
                                  {"заметки.txt", "n"}})
                    .imported == 5);
        trip = f.entryNamed(s, "Отпуск").id;
        sea = f.entryNamed(s, "Море", trip).id;
        elka = f.entryNamed(s, "Ёлка на пляже.jpg", sea).id;
        sunset = f.entryNamed(s, "закат.jpg", sea).id;
        plan = f.entryNamed(s, "план.txt", trip).id;
        work = f.entryNamed(s, "Работа").id;
        report = f.entryNamed(s, "отчёт.pdf", work).id;
        notes = f.entryNamed(s, "заметки.txt").id;
        anna = f.tag(s, "Люди", "Анна");
        boris = f.tag(s, "Люди", "Борис");
        crimea = f.tag(s, "Место", "Крым");
        sochi = f.tag(s, "Место", "Сочи");
        ru = f.tag(s, "Язык", "ru");
        f.tagEntries(s, {trip}, {crimea}, true); // достается и вложенным
        f.tagEntries(s, {elka, plan}, {anna});
        f.tagEntries(s, {sunset}, {boris, ru});
        f.tagEntries(s, {plan}, {ru});
        f.tagEntries(s, {report}, {sochi, boris});
        REQUIRE(f.services.entries->update(f.lease(s), notes, {.description = "про Анну и Крым"})
                    .has_value());
        REQUIRE(f.services.entries->update(f.lease(s), report, {.description = "план на квартал"})
                    .has_value());
    }

    std::vector<domain::SearchHit> find(const app::SearchQuery& query) {
        auto hits = f.services.search->search(f.lease(s), query);
        REQUIRE(hits.has_value());
        return *hits;
    }

    static Names namesOf(const std::vector<domain::SearchHit>& hits) {
        Names out;
        for (const auto& hit : hits) {
            out.push_back(hit.entry.name);
        }
        return out;
    }

    Names byTags(std::vector<domain::TagId> tags, TagMatch match = TagMatch::Categories) {
        return namesOf(find({.tags = std::move(tags), .match = match}));
    }

    AppFixture f;
    app::UnlockResult s = f.createSafe();
    domain::EntryId trip = 0, sea = 0, elka = 0, sunset = 0, plan = 0, work = 0, report = 0,
                    notes = 0;
    domain::TagId anna = 0, boris = 0, crimea = 0, sochi = 0, ru = 0;
};

} // namespace

TEST_CASE("tags filter by direct and inherited tags", "[search][tags][UF-18]") {
    FilterVault v;
    // Крым стоит на "Отпуск" с inherit: он сам и все внутри; порядок - папки, затем по имени
    CHECK(v.byTags({v.crimea}) ==
          Names{"Море", "Отпуск", "Ёлка на пляже.jpg", "закат.jpg", "план.txt"});
    CHECK(v.byTags({v.sochi}) == Names{"отчёт.pdf"});
    CHECK(v.byTags({v.ru}) == Names{"закат.jpg", "план.txt"});

    auto hits = v.find({.tags = {v.crimea}});
    for (const auto& hit : hits) {
        CHECK(hit.matchedIn == MatchedIn::None); // текста не было
    }
    const auto& sunset = *std::find_if(hits.begin(), hits.end(), [](const domain::SearchHit& hit) {
        return hit.entry.name == "закат.jpg";
    });
    REQUIRE(sunset.path.size() == 2);
    CHECK(sunset.path[0].name == "Отпуск");
    CHECK(sunset.path[1].name == "Море");
    CHECK(sunset.entry.inheritedTags == std::vector<domain::InheritedTag>{{v.crimea, v.trip}});

    // снятый тег перестает находить; удаленный - неизвестен
    REQUIRE(v.f.services.tags->assign(v.f.lease(v.s), {{v.plan}, {}, {v.ru}}).has_value());
    CHECK(v.byTags({v.ru}) == Names{"закат.jpg"});
    REQUIRE(v.f.services.tags->removeTag(v.f.lease(v.s), v.ru).has_value());
    auto gone = v.f.services.search->search(v.f.lease(v.s), {.tags = {v.ru}});
    REQUIRE_FALSE(gone.has_value());
    CHECK(gone.error().code == Code::InvalidArgument);
}

TEST_CASE("match modes: categories, all and any", "[search][tags][UF-18]") {
    FilterVault v;
    const std::vector<domain::TagId> picked{v.anna, v.boris, v.ru};

    // (Анна или Борис) и ru
    CHECK(v.byTags(picked, TagMatch::Categories) == Names{"закат.jpg", "план.txt"});
    CHECK(v.byTags(picked) == Names{"закат.jpg", "план.txt"}); // по умолчанию - он же
    CHECK(app::SearchQuery{}.match == TagMatch::Categories);
    // все три сразу - таких записей нет
    CHECK(v.byTags(picked, TagMatch::All).empty());
    // хотя бы один из трех
    CHECK(v.byTags(picked, TagMatch::Any) ==
          Names{"Ёлка на пляже.jpg", "закат.jpg", "отчёт.pdf", "план.txt"});

    // внутри одной категории Categories - это "или", All - "и"
    CHECK(v.byTags({v.anna, v.boris}, TagMatch::Categories) ==
          Names{"Ёлка на пляже.jpg", "закат.jpg", "отчёт.pdf", "план.txt"});
    CHECK(v.byTags({v.anna, v.boris}, TagMatch::All).empty());
    CHECK(v.byTags({v.boris, v.ru}, TagMatch::All) == Names{"закат.jpg"});
    // "Анна" и "Крым": Крым записи получили от папки
    CHECK(v.byTags({v.anna, v.crimea}, TagMatch::Categories) ==
          Names{"Ёлка на пляже.jpg", "план.txt"});
    CHECK(v.byTags({v.anna, v.crimea}, TagMatch::All) == Names{"Ёлка на пляже.jpg", "план.txt"});
    CHECK(v.byTags({v.crimea, v.sochi}, TagMatch::Categories) ==
          Names{"Море", "Отпуск", "Ёлка на пляже.jpg", "закат.jpg", "отчёт.pdf", "план.txt"});
    CHECK(v.byTags({v.crimea, v.sochi}, TagMatch::All).empty());

    // повтор тега в запросе ничего не меняет
    CHECK(v.byTags({v.anna, v.anna}, TagMatch::All) == v.byTags({v.anna}, TagMatch::All));
}

TEST_CASE("text matches the name first, then the description", "[search][UF-8][UF-18]") {
    FilterVault v;
    auto plan = v.find({.text = "ПЛАН"});
    REQUIRE(plan.size() == 2);
    CHECK(plan[0].entry.name == "отчёт.pdf"); // "отчет.pdf" < "план.txt"
    CHECK(plan[0].matchedIn == MatchedIn::Description);
    CHECK(plan[1].entry.name == "план.txt");
    CHECK(plan[1].matchedIn == MatchedIn::Name);

    // совпало и там и там - считается имя
    REQUIRE(v.f.services.entries->update(v.f.lease(v.s), v.plan, {.description = "мой план"})
                .has_value());
    CHECK(v.find({.text = "план"})[1].matchedIn == MatchedIn::Name);

    auto description = v.find({.text = "анну"}); // в описании, регистр и "ё" не важны
    REQUIRE(description.size() == 1);
    CHECK(description[0].entry.name == "заметки.txt");
    CHECK(description[0].matchedIn == MatchedIn::Description);
    CHECK(v.find({.text = "  КРЫМ "}).size() == 1);

    // текст и теги вместе: обе половины должны совпасть
    auto both = v.find({.text = "ёлка", .tags = {v.crimea}});
    REQUIRE(both.size() == 1);
    CHECK(both[0].entry.name == "Ёлка на пляже.jpg");
    CHECK(both[0].matchedIn == MatchedIn::Name);
    auto inDescription = v.find({.text = "план", .tags = {v.sochi}});
    REQUIRE(inDescription.size() == 1);
    CHECK(inDescription[0].entry.name == "отчёт.pdf"); // план.txt без Сочи отпал
    CHECK(inDescription[0].matchedIn == MatchedIn::Description);
    CHECK(v.find({.text = "ёлка", .tags = {v.sochi}}).empty());
}

TEST_CASE("within limits the search to the descendants", "[search][tags][UF-18]") {
    FilterVault v;
    CHECK(v.namesOf(v.find({.tags = {v.anna}, .within = v.trip})) ==
          Names{"Ёлка на пляже.jpg", "план.txt"});
    CHECK(v.namesOf(v.find({.tags = {v.anna}, .within = v.work})).empty());
    // рекурсивно и с унаследованными тегами
    CHECK(v.namesOf(v.find({.tags = {v.crimea}, .within = v.sea})) ==
          Names{"Ёлка на пляже.jpg", "закат.jpg"});
    CHECK(v.namesOf(v.find({.tags = {v.boris}, .within = v.work})) == Names{"отчёт.pdf"});
    CHECK(v.namesOf(v.find({.text = "jpg", .within = v.trip})) ==
          Names{"Ёлка на пляже.jpg", "закат.jpg"});

    // самой записи в выдаче нет, лист ничего не содержит
    CHECK(v.namesOf(v.find({.text = "отпуск"})) == Names{"Отпуск"});
    CHECK(v.find({.text = "отпуск", .within = v.trip}).empty());
    CHECK(v.find({.tags = {v.crimea}, .within = v.plan}).empty());
    // пустой запрос пуст и с областью
    CHECK(v.find({.within = v.trip}).empty());

    auto missing = v.f.services.search->search(v.f.lease(v.s), {.text = "a", .within = 424242});
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code == Code::NotFound);
}

TEST_CASE("an empty query finds nothing; an unknown tag is refused", "[search][tags]") {
    FilterVault v;
    CHECK(v.find({}).empty());
    CHECK(v.find({.text = "   "}).empty());
    CHECK(v.find({.text = "", .match = TagMatch::Any}).empty());

    for (const auto match : {TagMatch::Categories, TagMatch::All, TagMatch::Any}) {
        auto unknown = v.f.services.search->search(
            v.f.lease(v.s), {.text = "план", .tags = {v.anna, 999}, .match = match});
        REQUIRE_FALSE(unknown.has_value());
        CHECK(unknown.error().code == Code::InvalidArgument);
    }
}

TEST_CASE("the limit applies after sorting", "[search][tags]") {
    FilterVault v;
    auto two = v.find({.tags = {v.crimea}, .limit = 2});
    CHECK(v.namesOf(two) == Names{"Море", "Отпуск"});            // папки первыми
    CHECK(v.find({.tags = {v.crimea}, .limit = 0}).size() == 5); // 0 - по умолчанию
    CHECK(v.find({.tags = {v.crimea}, .limit = 5000}).size() == 5);
}

TEST_CASE("the filter sees the tags after the safe is reopened", "[search][tags][UF-18]") {
    FilterVault v;
    v.f.services.safe->lock();
    auto again = v.f.services.safe->unlock({"test", "secret1"});
    REQUIRE(again.has_value());
    auto hits = v.f.services.search->search(v.f.lease(*again),
                                            {.tags = {v.anna, v.ru}, .match = TagMatch::All});
    REQUIRE(hits.has_value());
    REQUIRE(hits->size() == 1);
    CHECK((*hits)[0].entry.name == "план.txt");
}
