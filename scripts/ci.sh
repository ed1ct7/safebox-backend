#!/usr/bin/env bash
# проверка include-ов по слоям + сборка и тесты на пресете (по умолчанию clang-ubsan)
#   scripts/ci.sh              - все
#   scripts/ci.sh --hygiene    - только проверка include-ов
set -euo pipefail
cd "$(dirname "$0")/.."

fail() { echo "CI FAIL: $1" >&2; exit 1; }

echo "==> header hygiene"

# a) сторонние заголовки адаптеров - только внутри infra
viol=$(grep -rlE --include='*.cpp' --include='*.hpp' \
       '#include *[<"](sqlite3\.h|sodium\.h|stb_[a-z_0-9]+\.h|miniz\.h)' src \
       | grep -v '^src/infrastructure/src/' || true)
[ -z "$viol" ] || fail "sqlite3/sodium/stb/miniz вне src/infrastructure/src: $viol"

# b) витрина infra (factories.hpp) - только infra и daemon (корень композиции)
viol=$(grep -rl --include='*.cpp' --include='*.hpp' 'safebox/infra/' src \
       | grep -vE '^src/(infrastructure|daemon)/' || true)
[ -z "$viol" ] || fail "safebox/infra/* вне infrastructure/daemon: $viol"

# c) транспортные библиотеки - только http-слой
viol=$(grep -rlE --include='*.cpp' --include='*.hpp' '#include *[<"](httplib\.h|nlohmann/)' src \
       | grep -v '^src/http/' || true)
[ -z "$viol" ] || fail "httplib/nlohmann вне src/http: $viol"

# d) единственная точка входа
viol=$(grep -rlE --include='*.cpp' 'int main\s*\(' src \
       | grep -v '^src/daemon/main\.cpp$' || true)
[ -z "$viol" ] || fail "main() вне src/daemon/main.cpp: $viol"

# e) домен и application не знают ни ОС-API, ни транспорта
viol=$(grep -rlE --include='*.cpp' --include='*.hpp' '#include *[<"](windows\.h|unistd\.h)' \
       src/domain src/application || true)
[ -z "$viol" ] || fail "ОС-заголовки в domain/application: $viol"

echo "    границы слоёв чисты"

if [ "${1:-}" = "--hygiene" ]; then
    echo "==> CI OK (только гигиена)"
    exit 0
fi

PRESET="${PRESET:-clang-ubsan}"
echo "==> configure/build/test: $PRESET"
cmake --preset "$PRESET"
cmake --build --preset "$PRESET"
ctest --preset "$PRESET"

echo "==> CI OK"
