// Порядок выдачи записей: список детей (каталог, EntriesService) и поиск. Один порядок на всех,
// чтобы клиент видел одно и то же в папке и в результатах поиска.
#pragma once

#include <string_view>
#include <utility>

#include "safebox/app/services.hpp"
#include "vault_session.hpp"

namespace safebox::app {

// Сравнение имен (уже folded): серии цифр - по значению (ведущие нули не в счет: "2" < "10"),
// остальное - побайтно. <0, 0 или >0; 0 - только у одинаковых строк.
[[nodiscard]] int compareNames(std::string_view a, std::string_view b) noexcept;

// Строгий порядок ListOptions: папки первыми, ключ (descending - по убыванию), имя по
// возрастанию, id.
[[nodiscard]] bool listedBefore(const Catalog::Node& a, const Catalog::Node& b,
                                const ListOptions& options) noexcept;

// Окно [offset, offset + limit) из total элементов; limit = 0 - до конца.
[[nodiscard]] std::pair<std::size_t, std::size_t> windowOf(std::size_t total,
                                                           const ListOptions& options) noexcept;

} // namespace safebox::app
