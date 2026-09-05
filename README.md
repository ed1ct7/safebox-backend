# SafeBox v2 - backend

Бэкенд SafeBox: локальный сервер на C++23, сейф хранится в одном зашифрованном файле.
Пока только каркас, логики нет.

## Сборка

Нужны Visual Studio 2022 и vcpkg (`VCPKG_ROOT`).

```
cmake --preset msvc-debug
cmake --build --preset msvc-debug
```
