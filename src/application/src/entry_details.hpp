// Поля записи из manifest импорта и пачки ссылок (EntryDetails): проверка и запись в мету.
#pragma once

#include <unordered_set>

#include "safebox/app/services.hpp"
#include "vault_session.hpp"

namespace safebox::app {

using TagIds = std::unordered_set<domain::TagId>;

// Проверка и нормализация: обрезка source/sourceUrl, теги без повторов и известные снимку.
[[nodiscard]] Status checkDetails(EntryDetails& details, const Catalog& catalog);

// Теги, которые есть в хранилище сейчас: внутри транзакции записи, чтобы удаленный за время
// импорта тег не попал в запись.
[[nodiscard]] Result<TagIds> liveTags(domain::UnitOfWork& uow);

// Заданное заменяет поле меты; теги добавляются к прямым (кроме исчезнувших из live).
void applyDetails(domain::EntryMeta& meta, const EntryDetails& details, const TagIds& live);

} // namespace safebox::app
