# SafeBox v2 - backend

Локальный зашифрованный сейф для фото, видео, файлов и ссылок. Все хранится в одном
файле `*.safebox` (внутри sqlite), имена, содержимое и миниатюры зашифрованы
(Argon2id + XChaCha20-Poly1305).

Собирается в один exe `safeboxd`, который поднимает HTTP-сервер на `127.0.0.1:8900`
(REST API под `/api/v1`) и может раздавать фронт, если его вшить при сборке.

Доки:
- [docs/architecture.md](docs/architecture.md) - архитектура и структура проекта
- [docs/safebox-format.md](docs/safebox-format.md) - формат файла сейфа
- [docs/api.md](docs/api.md) - API для фронта

## Структура

- `src/domain` - модель, правила и интерфейсы (порты), header-only
- `src/application` - сервисы: сейф, записи, импорт/экспорт, поиск
- `src/infrastructure` - реализации портов на libsodium, sqlite, stb, libwebp, miniz
- `src/http` - REST API на cpp-httplib
- `src/daemon` - main, конфиг, вшитый фронт

Зависимости идут только внутрь: application не знает про sqlite/libsodium, http не видит
infra. Это держится на уровне CMake-таргетов, плюс `scripts/ci.sh` проверяет инклюды.

## Сборка

Нужна Windows 10/11 x64 и Visual Studio 2022 (17.7+) с C++ (MSVC, CMake, Ninja, vcpkg).
Зависимости ставятся через vcpkg по `vcpkg.json`. Подойдет и vcpkg из VS, и отдельный
клон в `VCPKG_ROOT`.

Проще всего через скрипт, он сам найдет VS и vcpkg:

```powershell
powershell -File scripts/build.ps1                        # debug + тесты
powershell -File scripts/build.ps1 -Preset msvc-release   # release, один exe без dll
powershell -File scripts/build.ps1 -Preset msvc-asan      # с AddressSanitizer
```

Или руками из Developer PowerShell (CLion / VS Code тоже подхватывают `CMakePresets.json`):

```powershell
cmake --preset msvc-debug
cmake --build --preset msvc-debug
ctest --preset msvc-debug
```

Пресеты:
- `msvc-debug` - Debug, `x64-windows`, dll лежат рядом с exe
- `msvc-release` - Release, `x64-windows-static` + `/MT`, на выходе один `safeboxd.exe`
- `msvc-asan` - RelWithDebInfo + ASan
- `clang-ubsan`, `linux-gcc` - для линукса

exe лежит в `build/<пресет>/src/daemon/safeboxd.exe`.

Чтобы вшить фронт, собрать его (`npm run build`) и передать путь к dist:

```powershell
cmake --preset msvc-release -DSAFEBOX_WEB_DIST=../safebox-frontend/dist
```

## Запуск

```powershell
./build/msvc-release/src/daemon/safeboxd.exe
curl http://127.0.0.1:8900/health
```

Параметры:

| Параметр | По умолчанию | |
|---|---|---|
| `--host` | `127.0.0.1` | `0.0.0.0` для доступа по локалке (без HTTPS) |
| `--port` | `8900` | `0` - любой свободный |
| `--allow-host <host>` | | разрешенный `Host` для доступа по локалке, можно несколько |
| `--allow-origin <origin>` | | доп. Origin, например `http://localhost:5173` для dev |
| `--idle-minutes` | `15` | автоблокировка при бездействии |
| `--presence-seconds` | `120` | блокировка, если вкладку закрыли |
| `--safe-dir` | Документы | папка для относительных путей к сейфу |
| `--quiet` | | не писать лог запросов |

То же самое можно задать через env: `SAFEBOX_HOST`, `SAFEBOX_PORT`, `SAFEBOX_ALLOW_HOSTS`,
`SAFEBOX_ALLOW_ORIGINS`, `SAFEBOX_IDLE_MINUTES`, `SAFEBOX_PRESENCE_SECONDS`, `SAFEBOX_SAFE_DIR`.

При Ctrl+C или закрытии консоли сейф блокируется перед выходом.

Для разработки фронта: запускать `safeboxd --allow-origin http://localhost:5173`, а в Vite
настроить прокси `/api` на `http://127.0.0.1:8900` с `changeOrigin: true` (подробнее в конце
docs/api.md).

## Тесты

Catch2, запуск через `ctest --preset msvc-debug`. Наборы:

- `safebox-app-tests` - сервисы на in-memory хранилище, но с настоящим libsodium
- `safebox-http-tests` - http-слой на фейковых сервисах, без сети
- `safebox-infra-tests` - sqlite, libsodium, stb, webp, zip
- `safebox-integration-tests` - весь стек на случайном порту

## Ограничения

- На FAT32 сейф не может быть больше 4 ГБ, на NTFS/exFAT ограничения нет.
- Режим с доступом по локалке работает без HTTPS, так что только в доверенной сети.
- Один сейф может быть открыт только одним процессом.
