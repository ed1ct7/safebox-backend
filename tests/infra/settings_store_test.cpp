#include <catch2/catch_test_macros.hpp>

#include <filesystem>

#include "safebox/infra/factories.hpp"
#include "temp_dir.hpp"

using namespace safebox;
using domain::AppSettings;

TEST_CASE("no settings file means the defaults", "[settings]") {
    test::TempDir dir;
    auto store = infra::makeFileSettingsStore(dir / "settings.ini");
    auto loaded = store->load();
    REQUIRE(loaded.has_value());
    CHECK(loaded->linkPreviews); // предпросмотр по умолчанию включен
    CHECK(loaded->tagLanguage == domain::TagLanguage::Ru);
    CHECK_FALSE(std::filesystem::exists(dir / "settings.ini")); // чтение файл не создает
}

TEST_CASE("saved settings are read back, also by a new store", "[settings]") {
    test::TempDir dir;
    const auto file = dir / "settings.ini";
    auto store = infra::makeFileSettingsStore(file);

    REQUIRE(store->save(AppSettings{false}).has_value());
    CHECK(test::readFile(file) == "linkPreviews=0\ntagLanguage=ru\n");
    CHECK_FALSE(store->load()->linkPreviews);
    CHECK_FALSE(infra::makeFileSettingsStore(file)->load()->linkPreviews);

    REQUIRE(store->save(AppSettings{true}).has_value());
    CHECK(test::readFile(file) == "linkPreviews=1\ntagLanguage=ru\n");
    CHECK(infra::makeFileSettingsStore(file)->load()->linkPreviews);
}

TEST_CASE("saving creates the folder and leaves no temporary file behind", "[settings]") {
    test::TempDir dir;
    const auto file = dir / "SafeBox" / "nested" / "settings.ini";
    REQUIRE(infra::makeFileSettingsStore(file)->save(AppSettings{false}).has_value());
    CHECK(std::filesystem::exists(file));

    std::size_t files = 0;
    for (const auto& item : std::filesystem::directory_iterator(file.parent_path())) {
        CHECK(item.path() == file); // ни settings.ini.tmp, ни чего-то еще
        ++files;
    }
    CHECK(files == 1);
}

TEST_CASE("a save replaces the previous file completely", "[settings]") {
    test::TempDir dir;
    const auto file = dir / "settings.ini";
    test::writeFile(file, "linkPreviews=1\nстарая строка, которая не должна остаться\n");
    REQUIRE(infra::makeFileSettingsStore(file)->save(AppSettings{false}).has_value());
    CHECK(test::readFile(file) == "linkPreviews=0\ntagLanguage=ru\n");
}

TEST_CASE("the file format tolerates comments, spaces, unknown keys and bad values", "[settings]") {
    test::TempDir dir;
    const auto file = dir / "settings.ini";
    const auto read = [&](std::string_view text) {
        test::writeFile(file, text);
        auto loaded = infra::makeFileSettingsStore(file)->load();
        REQUIRE(loaded.has_value());
        return loaded->linkPreviews;
    };

    CHECK_FALSE(read("linkPreviews=0\n"));
    CHECK_FALSE(read("linkPreviews=false\n"));
    CHECK_FALSE(read("  linkPreviews  =  FALSE  \r\n")); // пробелы, CRLF, регистр
    CHECK(read("linkPreviews=true\n"));
    CHECK_FALSE(read("# комментарий\nother=1\n\nlinkPreviews=0\nbroken line\n"));
    CHECK(read("linkPreviews=maybe\n"));                   // непонятное значение - по умолчанию
    CHECK(read("linkpreviews=0\n"));                       // ключи различают регистр
    CHECK(read("# linkPreviews=0\n"));                     // закомментировано
    CHECK(read(""));                                       // пустой файл
    CHECK_FALSE(read("linkPreviews=1\nlinkPreviews=0\n")); // побеждает последняя строка
}

TEST_CASE("an unwritable location is an error, not an exception", "[settings]") {
    test::TempDir dir;
    test::writeFile(dir / "occupied", "файл на месте каталога");
    // родитель - обычный файл: ни создать каталог, ни записать нельзя
    auto store = infra::makeFileSettingsStore(dir / "occupied" / "settings.ini");
    auto saved = store->save(AppSettings{false});
    REQUIRE_FALSE(saved.has_value());
    CHECK(saved.error().code == domain::Error::Code::IoError);
    CHECK_FALSE(saved.error().message.empty());
    CHECK(store->load().has_value()); // читать нечего - значения по умолчанию
}
