// Настройки приложения. Не относятся к сейфу: лежат рядом с программой и действуют до входа в сейф
#pragma once

#include <string_view>

namespace safebox::domain {

// Какую локализацию имен тегов и категорий показывать (у тега два имени, тег один).
enum class TagLanguage { Ru, En };

[[nodiscard]] inline std::string_view tagLanguageName(TagLanguage lang) {
    return lang == TagLanguage::En ? "en" : "ru";
}

[[nodiscard]] inline TagLanguage tagLanguage(std::string_view name) {
    return name == "en" ? TagLanguage::En : TagLanguage::Ru;
}

struct AppSettings {
    bool linkPreviews =
        true; // после добавления ссылки сама ходит на страницу за названием и картинкой
    TagLanguage tagLanguage = TagLanguage::Ru;

    friend bool operator==(const AppSettings&, const AppSettings&) = default;
};

} // namespace safebox::domain
