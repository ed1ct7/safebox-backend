// EntriesService: правка полей, вложения у любой записи, перенос с конфликтами имен.
#include <thread>

#include "app_fixture.hpp"

using namespace safebox;
using namespace safebox::test;
using Code = domain::Error::Code;
using app::ConflictPolicy;

namespace {

constexpr std::string_view kShortcut = "[InternetShortcut]\r\nURL=https://example.com/a?b=1\r\n";

using Names = std::vector<std::string>;

Names namesIn(AppFixture& f, const app::UnlockResult& s, std::optional<domain::EntryId> parent) {
    auto listing = f.services.entries->list(f.lease(s), parent);
    REQUIRE(listing.has_value());
    Names out;
    for (const auto& e : listing->entries) {
        out.push_back(e.name);
    }
    return out;
}

Names sorted(Names names) {
    std::sort(names.begin(), names.end());
    return names;
}

app::MoveCmd moveCmd(std::vector<domain::EntryId> ids, std::optional<domain::EntryId> parent,
                     std::unordered_map<domain::EntryId, ConflictPolicy> resolutions = {}) {
    return app::MoveCmd{std::move(ids), parent, std::move(resolutions)};
}

} // namespace

TEST_CASE("update sets fields and the user-set flags, and they persist", "[entries][update]") {
    AppFixture f;
    auto s = f.createSafe();
    REQUIRE(f.importFiles(s, {{"a.txt", "data"}}).imported == 1);
    const auto entry = f.entryNamed(s, "a.txt");
    CHECK_FALSE(entry.meta.nameByUser);
    CHECK_FALSE(entry.meta.descriptionByUser);
    CHECK(entry.meta.description.empty());

    f.clock.advance(std::chrono::seconds(5));
    auto described = f.services.entries->update(f.lease(s), entry.id, {.description = "заметка"});
    REQUIRE(described.has_value());
    CHECK(described->name == "a.txt");
    CHECK(described->meta.description == "заметка");
    CHECK(described->meta.descriptionByUser);
    CHECK_FALSE(described->meta.nameByUser); // описание не трогает флаг имени
    CHECK(described->meta.modifiedAt == entry.meta.modifiedAt + 5000);
    CHECK(described->meta.createdAt == entry.meta.createdAt);
    CHECK(described->meta.blobId == entry.meta.blobId);
    CHECK(described->meta.size == entry.meta.size);

    auto renamed = f.services.entries->update(f.lease(s), entry.id, {.name = "  b.txt "});
    REQUIRE(renamed.has_value());
    CHECK(renamed->name == "b.txt");
    CHECK(renamed->meta.nameByUser);
    CHECK(renamed->meta.descriptionByUser);
    CHECK(renamed->meta.description == "заметка"); // прежнее описание осталось

    auto both =
        f.services.entries->update(f.lease(s), entry.id, {.name = "c.txt", .description = ""});
    REQUIRE(both.has_value());
    CHECK(both->meta.description.empty()); // очистить описание можно, флаг остается
    CHECK(both->meta.descriptionByUser);

    f.services.safe->lock();
    auto again = f.services.safe->unlock({"test", "secret1"});
    REQUIRE(again.has_value());
    const auto reopened = f.entryNamed(*again, "c.txt");
    CHECK(reopened.id == entry.id);
    CHECK(reopened.meta.nameByUser);
    CHECK(reopened.meta.descriptionByUser);
    CHECK(reopened.meta.description.empty());
    CHECK(f.readContent(*again, reopened.id) == "data");
}

TEST_CASE("update changes the url of a link only, http and https only", "[entries][update]") {
    AppFixture f;
    auto s = f.createSafe();
    REQUIRE(
        f.importFiles(
             s, {{"link.url", std::string(kShortcut)}, {"file.txt", "x"}, {"Папка/inner.txt", "y"}})
            .imported == 3);
    const auto link = f.entryNamed(s, "link.url");
    REQUIRE(link.meta.kind == domain::Kind::Link);

    auto changed =
        f.services.entries->update(f.lease(s), link.id, {.url = "  https://other.example/x?y=1  "});
    REQUIRE(changed.has_value());
    CHECK(changed->meta.url == "https://other.example/x?y=1");
    CHECK(changed->meta.blobId == link.meta.blobId); // содержимое .url не пересоздается
    CHECK_FALSE(changed->meta.nameByUser);
    CHECK_FALSE(changed->meta.descriptionByUser);

    for (const char* bad : {"ftp://example.com", "javascript:alert(1)", "example.com", "",
                            "http://", "http://a b.example"}) {
        INFO(bad);
        auto result = f.services.entries->update(f.lease(s), link.id, {.url = bad});
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == Code::InvalidArgument);
    }
    // не ссылка - адреса нет
    for (const auto* name : {"file.txt", "Папка"}) {
        auto result = f.services.entries->update(f.lease(s), f.entryNamed(s, name).id,
                                                 {.url = "https://example.com"});
        INFO(name);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == Code::InvalidArgument);
    }

    // ошибка в одном поле не оставляет следов от другого
    auto atomic =
        f.services.entries->update(f.lease(s), link.id, {.name = "новое", .url = "ftp://nope"});
    REQUIRE_FALSE(atomic.has_value());
    auto unchanged = f.services.entries->get(f.lease(s), link.id);
    REQUIRE(unchanged.has_value());
    CHECK(unchanged->name == "link.url");
    CHECK(unchanged->meta.url == "https://other.example/x?y=1");
    CHECK_FALSE(unchanged->meta.nameByUser);
}

TEST_CASE("update validates name, description size and the request itself", "[entries][update]") {
    AppFixture f;
    auto s = f.createSafe();
    REQUIRE(f.importFiles(s, {{"a.txt", "data"}}).imported == 1);
    const auto id = f.entryNamed(s, "a.txt").id;

    const auto code = [&](const app::UpdateEntryCmd& cmd) {
        auto result = f.services.entries->update(f.lease(s), id, cmd);
        REQUIRE_FALSE(result.has_value());
        return result.error().code;
    };
    CHECK(code({}) == Code::InvalidArgument); // нечего менять
    CHECK(code({.name = "a/b"}) == Code::InvalidArgument);
    CHECK(code({.name = "   "}) == Code::InvalidArgument);

    // предел описания - 64 КиБ в байтах
    const std::string limit(64 * 1024, 'a');
    auto ok = f.services.entries->update(f.lease(s), id, {.description = limit});
    REQUIRE(ok.has_value());
    CHECK(ok->meta.description.size() == 64 * 1024);
    CHECK(code({.description = limit + "a"}) == Code::InvalidArgument);
    std::string wide;
    while (wide.size() <= 64 * 1024) {
        wide += "я"; // 2 байта: символов меньше предела, байтов больше
    }
    CHECK(code({.description = wide}) == Code::InvalidArgument);
    CHECK(code({.description = "bad\xFF"
                               "utf8"}) == Code::InvalidArgument);

    auto missing = f.services.entries->update(f.lease(s), 999, {.name = "x.txt"});
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code == Code::NotFound);

    // большое описание переживает закрытие сейфа
    f.services.safe->lock();
    auto again = f.services.safe->unlock({"test", "secret1"});
    REQUIRE(again.has_value());
    CHECK(f.entryNamed(*again, "a.txt").meta.description == limit);
}

TEST_CASE("any entry can be a parent: attachments, breadcrumbs, folder tree",
          "[entries][attachments]") {
    AppFixture f;
    auto s = f.createSafe();
    REQUIRE(f.importFiles(s, {{"photo.jpg", fakeJpeg(700)},
                              {"Docs/notes.txt", "n"},
                              {"Docs/sub/x.txt", "x"},
                              {"Plain/p.txt", "p"}})
                .imported == 4);
    const auto photo = f.entryNamed(s, "photo.jpg");
    const auto docs = f.entryNamed(s, "Docs");
    CHECK(photo.childCount == 0);
    CHECK(docs.childCount == 2);

    auto moved = f.services.entries->move(f.lease(s), moveCmd({docs.id}, photo.id));
    REQUIRE(moved.has_value());
    CHECK(moved->moved == 1);

    auto attachments = f.services.entries->list(f.lease(s), photo.id);
    REQUIRE(attachments.has_value());
    REQUIRE(attachments->parent.has_value());
    CHECK(attachments->parent->name == "photo.jpg");
    CHECK(attachments->parent->childCount == 1);
    REQUIRE(attachments->path.size() == 1);
    CHECK(attachments->path[0].name == "photo.jpg");
    REQUIRE(attachments->entries.size() == 1);
    CHECK(attachments->entries[0].name == "Docs");
    CHECK(attachments->entries[0].childCount == 2);
    CHECK(attachments->entries[0].parentId == photo.id);

    CHECK(f.entryNamed(s, "photo.jpg").childCount == 1);
    auto got = f.services.entries->get(f.lease(s), photo.id);
    REQUIRE(got.has_value());
    CHECK(got->childCount == 1);
    CHECK(namesIn(f, s, std::nullopt) == Names{"Plain", "photo.jpg"});

    const auto sub = f.entryNamed(s, "sub", docs.id);
    auto deep = f.services.entries->list(f.lease(s), sub.id);
    REQUIRE(deep.has_value());
    REQUIRE(deep->path.size() == 3);
    CHECK(deep->path[0].name == "photo.jpg"); // крошки идут через файл
    CHECK(deep->path[1].name == "Docs");
    CHECK(deep->path[2].name == "sub");

    // дерево слева: папка внутри фото в него не попадает, как и ее подпапки
    auto tree = f.services.entries->folders(f.lease(s));
    REQUIRE(tree.has_value());
    REQUIRE(tree->size() == 1);
    CHECK((*tree)[0].name == "Plain");

    // поиск видит вложенное и показывает путь через фото
    auto hits = f.services.search->search(f.lease(s), "x.txt", 0);
    REQUIRE(hits.has_value());
    REQUIRE(hits->size() == 1);
    REQUIRE((*hits)[0].path.size() == 3);
    CHECK((*hits)[0].path[0].name == "photo.jpg");

    // все выживает после блокировки
    f.services.safe->lock();
    auto again = f.services.safe->unlock({"test", "secret1"});
    REQUIRE(again.has_value());
    CHECK(namesIn(f, *again, photo.id) == Names{"Docs"});

    // запись уносит вложения вместе с собой
    const domain::EntryId selection[] = {photo.id};
    auto removed = f.services.entries->remove(f.lease(*again), selection);
    REQUIRE(removed.has_value());
    CHECK(*removed == 5); // photo, Docs, notes.txt, sub, x.txt
    CHECK(namesIn(f, *again, std::nullopt) == Names{"Plain"});
}

TEST_CASE("move re-parents entries without touching their data", "[entries][move]") {
    AppFixture f;
    auto s = f.createSafe();
    REQUIRE(f.importFiles(s, {{"a.txt", "A"}, {"b.txt", "B"}, {"Dest/x.txt", "X"}}).imported == 3);
    const auto dest = f.entryNamed(s, "Dest");
    const auto a = f.entryNamed(s, "a.txt");
    const auto b = f.entryNamed(s, "b.txt");
    const auto blobs = f.store.blobCount(f.safePath());

    auto moved = f.services.entries->move(f.lease(s), moveCmd({a.id, b.id}, dest.id));
    REQUIRE(moved.has_value());
    CHECK(moved->moved == 2);
    CHECK(moved->replaced == 0);
    CHECK(moved->skipped == 0);
    CHECK(namesIn(f, s, dest.id) == Names{"a.txt", "b.txt", "x.txt"});
    CHECK(namesIn(f, s, std::nullopt) == Names{"Dest"});
    CHECK(f.readContent(s, a.id) == "A");
    CHECK(f.store.blobCount(f.safePath()) == blobs);
    CHECK(f.entryNamed(s, "Dest").childCount == 3);

    // обратно в корень
    auto back = f.services.entries->move(
        f.lease(s), moveCmd({f.entryNamed(s, "x.txt", dest.id).id}, std::nullopt));
    REQUIRE(back.has_value());
    CHECK(back->moved == 1);
    CHECK(namesIn(f, s, std::nullopt) == Names{"Dest", "x.txt"});

    f.services.safe->lock();
    auto again = f.services.safe->unlock({"test", "secret1"});
    REQUIRE(again.has_value());
    CHECK(namesIn(f, *again, dest.id) == Names{"a.txt", "b.txt"});
    CHECK(f.readContent(*again, b.id) == "B");
}

TEST_CASE("move nothing, move into the same parent: no changes, no conflicts", "[entries][move]") {
    AppFixture f;
    auto s = f.createSafe();
    REQUIRE(f.importFiles(s, {{"a.txt", "A"}, {"D/a.txt", "A2"}}).imported == 2);
    const auto dest = f.entryNamed(s, "D");
    const auto inside = f.entryNamed(s, "a.txt", dest.id);
    const auto outside = f.entryNamed(s, "a.txt");

    auto empty = f.services.entries->move(f.lease(s), moveCmd({}, dest.id));
    REQUIRE(empty.has_value());
    CHECK(empty->moved == 0);

    // запись уже у этого родителя: не конфликт со своим же именем и не перенос
    const domain::EntryId same[] = {inside.id};
    auto plan = f.services.entries->planMove(f.lease(s), same, dest.id);
    REQUIRE(plan.has_value());
    CHECK(plan->empty());
    auto result = f.services.entries->move(f.lease(s), moveCmd({inside.id}, dest.id));
    REQUIRE(result.has_value());
    CHECK(result->moved == 0);
    CHECK(result->replaced == 0);
    CHECK(result->skipped == 0);
    CHECK(f.readContent(s, inside.id) == "A2");
    CHECK(namesIn(f, s, dest.id) == Names{"a.txt"});

    // то же в корень для записи из корня; один из двух - все же конфликт
    const domain::EntryId mixed[] = {outside.id, inside.id};
    auto rootPlan = f.services.entries->planMove(f.lease(s), mixed, std::nullopt);
    REQUIRE(rootPlan.has_value());
    REQUIRE(rootPlan->size() == 1);
    CHECK((*rootPlan)[0].id == inside.id);
    CHECK((*rootPlan)[0].existing.id == outside.id);
}

TEST_CASE("moving into itself or into a descendant fails and changes nothing", "[entries][move]") {
    AppFixture f;
    auto s = f.createSafe();
    REQUIRE(f.importFiles(s, {{"F/G/h.txt", "h"}, {"other.txt", "o"}, {"photo.jpg", fakeJpeg(600)}})
                .imported == 3);
    const auto folder = f.entryNamed(s, "F");
    const auto inner = f.entryNamed(s, "G", folder.id);
    const auto other = f.entryNamed(s, "other.txt");
    const auto photo = f.entryNamed(s, "photo.jpg");

    // первая запись валидна, вторая нет - не должно сдвинуться ничего
    auto descendant =
        f.services.entries->move(f.lease(s), moveCmd({other.id, folder.id}, inner.id));
    REQUIRE_FALSE(descendant.has_value());
    CHECK(descendant.error().code == Code::InvalidArgument);
    CHECK(namesIn(f, s, std::nullopt) == Names{"F", "other.txt", "photo.jpg"});
    CHECK(namesIn(f, s, inner.id) == Names{"h.txt"});

    auto self = f.services.entries->move(f.lease(s), moveCmd({folder.id}, folder.id));
    REQUIRE_FALSE(self.has_value());
    CHECK(self.error().code == Code::InvalidArgument);

    const domain::EntryId ids[] = {folder.id};
    auto plan = f.services.entries->planMove(f.lease(s), ids, inner.id);
    REQUIRE_FALSE(plan.has_value());
    CHECK(plan.error().code == Code::InvalidArgument);

    // и через вложения: папка внутри фото не может принять само фото
    REQUIRE(f.services.entries->move(f.lease(s), moveCmd({folder.id}, photo.id)).has_value());
    auto cycle = f.services.entries->move(f.lease(s), moveCmd({photo.id}, inner.id));
    REQUIRE_FALSE(cycle.has_value());
    CHECK(cycle.error().code == Code::InvalidArgument);
    CHECK(namesIn(f, s, photo.id) == Names{"F"});

    // из вложенной папки можно и наружу, и в ее родителя
    auto out = f.services.entries->move(f.lease(s), moveCmd({inner.id}, std::nullopt));
    REQUIRE(out.has_value());
    CHECK(out->moved == 1);
}

TEST_CASE("move refuses unknown ids and unknown parents", "[entries][move]") {
    AppFixture f;
    auto s = f.createSafe();
    REQUIRE(f.importFiles(s, {{"a.txt", "A"}, {"D/x.txt", "X"}}).imported == 2);
    const auto a = f.entryNamed(s, "a.txt");
    const auto dest = f.entryNamed(s, "D");

    auto noParent = f.services.entries->move(f.lease(s), moveCmd({a.id}, 9999));
    REQUIRE_FALSE(noParent.has_value());
    CHECK(noParent.error().code == Code::NotFound);
    auto noEntry = f.services.entries->move(f.lease(s), moveCmd({a.id, 9999}, dest.id));
    REQUIRE_FALSE(noEntry.has_value());
    CHECK(noEntry.error().code == Code::NotFound);
    CHECK(namesIn(f, s, std::nullopt) == Names{"D", "a.txt"}); // a.txt не сдвинулся

    const domain::EntryId ids[] = {9999};
    auto plan = f.services.entries->planMove(f.lease(s), ids, dest.id);
    REQUIRE_FALSE(plan.has_value());
    CHECK(plan.error().code == Code::NotFound);

    // один и тот же id дважды - перенос один раз
    auto twice = f.services.entries->move(f.lease(s), moveCmd({a.id, a.id}, dest.id));
    REQUIRE(twice.has_value());
    CHECK(twice->moved == 1);
    CHECK(namesIn(f, s, dest.id) == Names{"a.txt", "x.txt"});
}

TEST_CASE("planMove finds name clashes at the new parent, ignoring case", "[entries][move]") {
    AppFixture f;
    auto s = f.createSafe();
    REQUIRE(f.importFiles(s, {{"A.TXT", "up"},
                              {"c.txt", "c"},
                              {"D/a.txt", "low"},
                              {"D/b.txt", "b"},
                              {"E/Елка.txt", "e"},
                              {"Ёлка.txt", "yo"}})
                .imported == 6);
    const auto dest = f.entryNamed(s, "D");
    const auto upper = f.entryNamed(s, "A.TXT");
    const auto lower = f.entryNamed(s, "a.txt", dest.id);
    const auto c = f.entryNamed(s, "c.txt");
    const auto yo = f.entryNamed(s, "Ёлка.txt");
    const auto elka = f.entryNamed(s, "Елка.txt", f.entryNamed(s, "E").id);

    const domain::EntryId ids[] = {upper.id, c.id};
    auto plan = f.services.entries->planMove(f.lease(s), ids, dest.id);
    REQUIRE(plan.has_value());
    REQUIRE(plan->size() == 1);
    CHECK((*plan)[0].id == upper.id);
    CHECK((*plan)[0].existing.id == lower.id);
    CHECK((*plan)[0].existing.name == "a.txt");
    CHECK((*plan)[0].existing.meta.size == 3);

    // "ё" = "е" и здесь
    const domain::EntryId yoIds[] = {yo.id};
    auto yoPlan = f.services.entries->planMove(f.lease(s), yoIds, f.entryNamed(s, "E").id);
    REQUIRE(yoPlan.has_value());
    REQUIRE(yoPlan->size() == 1);
    CHECK((*yoPlan)[0].existing.id == elka.id);

    // планирование ничего не меняет
    CHECK(namesIn(f, s, std::nullopt) == Names{"D", "E", "A.TXT", "c.txt", "Ёлка.txt"});

    // без решения по умолчанию - KeepBoth, номер перед расширением
    auto moved = f.services.entries->move(f.lease(s), moveCmd({upper.id, c.id}, dest.id));
    REQUIRE(moved.has_value());
    CHECK(moved->moved == 2);
    CHECK(moved->replaced == 0);
    CHECK(sorted(namesIn(f, s, dest.id)) == Names{"A (2).TXT", "a.txt", "b.txt", "c.txt"});
    CHECK(f.readContent(s, upper.id) == "up");
    CHECK(f.readContent(s, lower.id) == "low");
}

TEST_CASE("move resolves conflicts: keep both, replace, skip", "[entries][move]") {
    AppFixture f;
    auto s = f.createSafe();
    REQUIRE(f.importFiles(s, {{"x.txt", "X1"},
                              {"y.txt", "Y1"},
                              {"z.txt", "Z1"},
                              {"D/x.txt", "X2"},
                              {"D/y.txt", "Y2"},
                              {"D/z.txt", "Z2"}})
                .imported == 6);
    const auto dest = f.entryNamed(s, "D");
    const auto x1 = f.entryNamed(s, "x.txt");
    const auto y1 = f.entryNamed(s, "y.txt");
    const auto z1 = f.entryNamed(s, "z.txt");
    const auto x2 = f.entryNamed(s, "x.txt", dest.id);
    const auto y2 = f.entryNamed(s, "y.txt", dest.id);
    const auto z2 = f.entryNamed(s, "z.txt", dest.id);
    const auto blobs = f.store.blobCount(f.safePath());

    auto moved = f.services.entries->move(
        f.lease(s), moveCmd({x1.id, y1.id, z1.id}, dest.id,
                            {{y1.id, ConflictPolicy::Replace}, {z1.id, ConflictPolicy::Skip}}));
    REQUIRE(moved.has_value());
    CHECK(moved->moved == 2); // x (оба остались) и y (заменил старый)
    CHECK(moved->replaced == 1);
    CHECK(moved->skipped == 1);

    CHECK(sorted(namesIn(f, s, dest.id)) == Names{"x (2).txt", "x.txt", "y.txt", "z.txt"});
    CHECK(namesIn(f, s, std::nullopt) == Names{"D", "z.txt"}); // пропущенный остался на месте
    CHECK(f.readContent(s, x1.id) == "X1");
    CHECK(f.readContent(s, x2.id) == "X2");
    CHECK(f.readContent(s, y1.id) == "Y1");
    CHECK(f.readContent(s, z1.id) == "Z1");
    CHECK(f.readContent(s, z2.id) == "Z2");
    CHECK(f.services.entries->get(f.lease(s), y2.id).error().code == Code::NotFound);
    CHECK(f.entryNamed(s, "y.txt", dest.id).id == y1.id);
    CHECK(f.store.blobCount(f.safePath()) == blobs - 1); // блоб замененной записи ушел
    CHECK(f.store.compactions >= 1);
}

TEST_CASE("replacing a folder removes its whole subtree with the data", "[entries][move]") {
    AppFixture f;
    auto s = f.createSafe();
    REQUIRE(f.importFiles(s, {{"F/inner.txt", "new"},
                              {"F/sub/deep.txt", "deep"},
                              {"Dest/F/old.txt", "old"},
                              {"Dest/F/photo.jpg", fakeJpeg(600)}})
                .imported == 4);
    const auto dest = f.entryNamed(s, "Dest");
    const auto incoming = f.entryNamed(s, "F");
    const auto existing = f.entryNamed(s, "F", dest.id);
    const auto blobsBefore = f.store.blobCount(f.safePath());

    auto moved = f.services.entries->move(
        f.lease(s), moveCmd({incoming.id}, dest.id, {{incoming.id, ConflictPolicy::Replace}}));
    REQUIRE(moved.has_value());
    CHECK(moved->moved == 1);
    CHECK(moved->replaced == 1);
    CHECK(namesIn(f, s, dest.id) == Names{"F"});
    CHECK(f.entryNamed(s, "F", dest.id).id == incoming.id);
    CHECK(sorted(namesIn(f, s, incoming.id)) == Names{"inner.txt", "sub"});
    CHECK(f.services.entries->get(f.lease(s), existing.id).error().code == Code::NotFound);
    CHECK(f.store.blobCount(f.safePath()) == blobsBefore - 3); // old.txt, photo.jpg и его миниатюра
    CHECK(f.store.pendingBlobs(f.safePath()) == 0);
}

TEST_CASE("a moved entry that lay inside the replaced folder survives", "[entries][move]") {
    AppFixture f;
    auto s = f.createSafe();
    REQUIRE(f.importFiles(s, {{"Box/Box", "inner"}, {"Box/other.txt", "other"}}).imported == 2);
    const auto outer = f.entryNamed(s, "Box");
    const auto inner = f.entryNamed(s, "Box", outer.id);
    REQUIRE_FALSE(inner.isFolder());

    const domain::EntryId ids[] = {inner.id};
    auto plan = f.services.entries->planMove(f.lease(s), ids, std::nullopt);
    REQUIRE(plan.has_value());
    REQUIRE(plan->size() == 1);
    CHECK((*plan)[0].existing.id == outer.id);

    auto moved = f.services.entries->move(
        f.lease(s), moveCmd({inner.id}, std::nullopt, {{inner.id, ConflictPolicy::Replace}}));
    REQUIRE(moved.has_value());
    CHECK(moved->moved == 1);
    CHECK(moved->replaced == 1);
    CHECK(namesIn(f, s, std::nullopt) == Names{"Box"});
    const auto survivor = f.entryNamed(s, "Box");
    CHECK(survivor.id == inner.id);
    CHECK(f.readContent(s, survivor.id) == "inner");
    CHECK(f.services.entries->get(f.lease(s), outer.id).error().code == Code::NotFound);
    CHECK(f.store.blobCount(f.safePath()) == 1);
}

TEST_CASE("two entries with one name in a request: the second is kept as a copy",
          "[entries][move]") {
    AppFixture f;
    auto s = f.createSafe();
    REQUIRE(f.importFiles(s, {{"A/n.txt", "first"},
                              {"B/n.txt", "second"},
                              {"C/other.txt", "o"},
                              {"E/n.txt", "old"}})
                .imported == 4);
    const auto first = f.entryNamed(s, "n.txt", f.entryNamed(s, "A").id);
    const auto second = f.entryNamed(s, "n.txt", f.entryNamed(s, "B").id);
    const auto target = f.entryNamed(s, "C");
    const auto occupied = f.entryNamed(s, "E");
    const auto old = f.entryNamed(s, "n.txt", occupied.id);
    const std::unordered_map<domain::EntryId, ConflictPolicy> replaceBoth = {
        {first.id, ConflictPolicy::Replace}, {second.id, ConflictPolicy::Replace}};

    SECTION("nothing occupies the name") {
        const domain::EntryId ids[] = {first.id, second.id};
        auto plan = f.services.entries->planMove(f.lease(s), ids, target.id);
        REQUIRE(plan.has_value());
        CHECK(plan->empty()); // конфликтовать не с чем - вторая просто получит номер

        auto moved = f.services.entries->move(
            f.lease(s), moveCmd({first.id, second.id}, target.id, replaceBoth));
        REQUIRE(moved.has_value());
        CHECK(moved->moved == 2);
        CHECK(moved->replaced == 0);
        CHECK(sorted(namesIn(f, s, target.id)) == Names{"n (2).txt", "n.txt", "other.txt"});
        CHECK(f.readContent(s, first.id) == "first");
        CHECK(f.readContent(s, second.id) == "second");
        CHECK(f.entryNamed(s, "n.txt", target.id).id == first.id); // по порядку в запросе
    }
    SECTION("an existing entry holds the name") {
        const domain::EntryId ids[] = {first.id, second.id};
        auto plan = f.services.entries->planMove(f.lease(s), ids, occupied.id);
        REQUIRE(plan.has_value());
        REQUIRE(plan->size() == 2);
        CHECK((*plan)[0].existing.id == old.id);
        CHECK((*plan)[1].existing.id == old.id);

        auto moved = f.services.entries->move(
            f.lease(s), moveCmd({first.id, second.id}, occupied.id, replaceBoth));
        REQUIRE(moved.has_value());
        CHECK(moved->moved == 2);
        CHECK(moved->replaced == 1); // старую заменила только первая
        CHECK(sorted(namesIn(f, s, occupied.id)) == Names{"n (2).txt", "n.txt"});
        CHECK(f.entryNamed(s, "n.txt", occupied.id).id == first.id);
        CHECK(f.entryNamed(s, "n (2).txt", occupied.id).id == second.id);
        CHECK(f.services.entries->get(f.lease(s), old.id).error().code == Code::NotFound);
    }
}

TEST_CASE("keep both numbers folders without an extension and stays unique", "[entries][move]") {
    AppFixture f;
    auto s = f.createSafe();
    REQUIRE(f.importFiles(s, {{"Отчёты.2024/a.txt", "1"},
                              {"Dest/Отчёты.2024/b.txt", "2"},
                              {"Dest/Отчёты.2024 (2)/c.txt", "3"}})
                .imported == 3);
    const auto dest = f.entryNamed(s, "Dest");
    const auto folder = f.entryNamed(s, "Отчёты.2024");

    auto moved = f.services.entries->move(f.lease(s), moveCmd({folder.id}, dest.id));
    REQUIRE(moved.has_value());
    CHECK(sorted(namesIn(f, s, dest.id)) ==
          Names{"Отчёты.2024", "Отчёты.2024 (2)", "Отчёты.2024 (3)"});
    CHECK(f.entryNamed(s, "Отчёты.2024 (3)", dest.id).id == folder.id);
}

TEST_CASE("a lease can be shared while the session is open", "[session][lease]") {
    AppFixture f;
    auto s = f.createSafe();
    auto lease = f.lease(s);
    auto second = lease.share();
    REQUIRE(second.has_value());
    CHECK_FALSE(second->cancelled());
    CHECK(second->session() == lease.session());
    auto weak = lease.weakSession();
    CHECK_FALSE(weak.expired());
    CHECK(weak.lock().get() == lease.session());

    // lock ждет обе аренды, а пока он ждет, новых аренд уже нет
    std::thread locker([&] { f.services.safe->lock(); });
    for (int i = 0; i < 200 && !lease.cancelled(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(lease.cancelled());
    CHECK_FALSE(lease.share().has_value());
    CHECK_FALSE(second->share().has_value());
    {
        auto drop = std::move(lease);
    }
    {
        auto drop = std::move(*second);
    }
    locker.join();
    CHECK(weak.expired()); // слабая ссылка не держала сессию

    // пустая аренда (тесты транспорта) делится в такую же пустую
    app::Lease empty(nullptr);
    auto emptyShared = empty.share();
    REQUIRE(emptyShared.has_value());
    CHECK(emptyShared->session() == nullptr);
    CHECK(empty.weakSession().expired());
}
