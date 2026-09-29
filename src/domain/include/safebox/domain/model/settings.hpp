// Настройки приложения. Не относятся к сейфу: лежат рядом с программой и действуют до входа в сейф
#pragma once

namespace safebox::domain {

struct AppSettings {
    bool linkPreviews =
        true; // после добавления ссылки сама ходит на страницу за названием и картинкой

    friend bool operator==(const AppSettings&, const AppSettings&) = default;
};

} // namespace safebox::domain
