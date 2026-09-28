# Формат .safebox (версия 1)

Константы лежат в `src/domain/include/safebox/domain/model/safe_format.hpp`, схема в
`src/infrastructure/src/sqlite_schema.cpp`, шифрование полей в
`src/application/src/sealing.cpp`.

Сейф это обычный файл SQLite 3. Открыто хранятся только id, структура папок и
служебные поля. Имена, ссылки, содержимое, миниатюры и мастер-ключ зашифрованы
XChaCha20-Poly1305.

## Как понять, что это сейф

Смотрим заголовок SQLite, не открывая базу:
- 0: `SQLite format 3\0`
- 68: `application_id` = `0x53424F58` ("SBOX"), big-endian
- 60: `user_version` = 1

Если не совпало - `NotASafe`. Если `user_version` больше нашего - файл от более новой
версии. Миграций пока нет.

## Pragma

При создании (потом без полного VACUUM не поменять):
- `page_size = 65536` - кусок 8 МиБ занимает ~128 страниц, а не 2048
- `auto_vacuum = INCREMENTAL` - после удаления делаем `incremental_vacuum` и файл уменьшается

На каждом соединении:
- `journal_mode = DELETE` - рядом нет -wal/-shm, файл всегда можно просто скопировать
- `locking_mode = EXCLUSIVE` - сейф открыт только одним процессом, блокировка берется сразу
- `synchronous = FULL`, `foreign_keys = ON`, `cell_size_check = ON`

Файл могут подсунуть, поэтому открываем с `SQLITE_DBCONFIG_DEFENSIVE`,
`trusted_schema = OFF`, без триггеров, view и расширений. Если в файле есть триггеры
или view или не хватает таблиц - `NotASafe`.

В EXCLUSIVE режиме sqlite не удаляет журнал, а обнуляет заголовок. Поэтому после
закрытия пустой `-journal` удаляем сами (горячий не трогаем).

## Схема

```sql
CREATE TABLE meta (                       -- одна строка
    id             INTEGER PRIMARY KEY CHECK (id = 1),
    format_version INTEGER NOT NULL,      -- 1
    kdf_ops        INTEGER NOT NULL,      -- Argon2id opslimit
    kdf_mem        INTEGER NOT NULL,      -- Argon2id memlimit, байт
    salt           BLOB    NOT NULL,      -- 16 байт
    chunk_size     INTEGER NOT NULL,      -- 8388608
    envelope       BLOB    NOT NULL       -- зашифрованный мастер-ключ, 72 байта
) STRICT;

CREATE TABLE blobs (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    status      INTEGER NOT NULL DEFAULT 0 CHECK (status IN (0, 1)), -- 0 pending, 1 ready
    size        INTEGER NOT NULL DEFAULT 0,
    chunk_count INTEGER NOT NULL DEFAULT 0
) STRICT;

CREATE TABLE chunks (
    blob_id INTEGER NOT NULL REFERENCES blobs(id) ON DELETE CASCADE,
    idx     INTEGER NOT NULL,
    data    BLOB    NOT NULL,             -- nonce | шифртекст | tag
    PRIMARY KEY (blob_id, idx)
) STRICT;

CREATE TABLE entries (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    parent_id     INTEGER REFERENCES entries(id) ON DELETE CASCADE, -- NULL = корень
    is_folder     INTEGER NOT NULL CHECK (is_folder IN (0, 1)),
    blob_id       INTEGER REFERENCES blobs(id),
    thumb_blob_id INTEGER REFERENCES blobs(id),
    enc_name      BLOB    NOT NULL,
    enc_meta      BLOB    NOT NULL
) STRICT;

CREATE INDEX entries_parent ON entries(parent_id);
```

id сделаны AUTOINCREMENT специально: они входят в AAD, и если бы id переиспользовался,
AAD удаленной записи совпал бы с новой.

## Ключи

1. KEK = Argon2id(пароль, соль, kdf_ops, kdf_mem), 32 байта. По умолчанию ops = 3,
   mem = 256 МиБ (примерно MODERATE из libsodium). Для чужого файла параметры
   ограничены: ops от 1 до 64, mem от 8 КиБ до 4 ГиБ.
2. Мастер-ключ - 32 случайных байта, лежит только в `meta.envelope` зашифрованным на
   KEK: 24 (nonce) + 32 + 16 (tag) = 72 байта. Не расшифровался - неверный пароль.
3. Подключи через `crypto_kdf_derive_from_key(32, id, "SBOXKEYS", мастер-ключ)`:
   1 - имена и мета, 2 - содержимое, 3 - миниатюры.

Смена пароля: новая соль, новый KEK, перезаписываем `salt` и `envelope` одной
транзакцией. Сами данные не перешифровываются.

В памяти ключи лежат в `sodium_malloc` и стираются при освобождении.

## Шифрование и AAD

XChaCha20-Poly1305 IETF, результат = `nonce(24) | шифртекст | tag(16)`, nonce
случайный. Числа в AAD little-endian:

- конверт: `format_version u32 | salt[16] | kdf_ops u64 | kdf_mem u64`
- кусок: `1 u32 | blob_id i64 | idx u32 | last u8`
- поле записи: `1 u32 | entry_id i64 | tag u8` (1 - имя, 2 - мета)

За счет этого `IntegrityError` будет, если переставить куски, перенести кусок в чужой
блоб, отрезать хвост (флаг last), перенести имя или мету в другую запись, подменить
соль или параметры Argon2id.

## Поля записи

`enc_name` - имя в UTF-8 на подключе 1 с тегом 1. `enc_meta` - мета на подключе 1 с
тегом 2, формат (little-endian):

```
u8  layout = 1
u8  kind          0 folder, 1 file, 2 photo, 3 video, 4 link
u64 size          размер исходного файла
i64 createdAt     unix-мс
i64 modifiedAt    unix-мс
u8  flags         bit0 - есть blobId, bit1 - есть thumbBlobId
i64 blobId        если bit0
i64 thumbBlobId   если bit1
u16 len + mime
u32 len + url     только у ссылок
```

При чтении blobId, thumbBlobId и признак папки сверяются с открытыми колонками, так
что подменить содержимое между записями не получится.

## Блобы

- Файл режется на куски по `chunk_size`, кусков `max(1, ceil(size / chunk_size))`. У
  пустого файла один пустой кусок, чтобы последний кусок был всегда. `last` стоит у
  `idx == chunk_count - 1`.
- При чтении проверяем: блоб ready, `chunk_count` соответствует `size`, `size` совпадает
  с тем что в мете, каждый кусок кроме последнего ровно `chunk_size` байт.
- Миниатюра - отдельный блоб (jpeg до 400px) на подключе 3.
- Импорт: блоб создается со status 0, каждый кусок пишется своей транзакцией, в конце
  одной транзакцией создается запись и status = 1. Блобы, оставшиеся в 0, удаляются
  при блокировке и при следующем открытии.
- Удаление: поддерево записей (рекурсивный CTE), их блобы и куски одной транзакцией,
  потом `incremental_vacuum`.

## Что не защищено

- `parent_id` открыт и не аутентифицирован, то есть записи можно перекинуть между
  папками (прочитать или подменить содержимое нельзя). Циклы в `parent_id` сервер не
  ломают, такие записи показываются в корне.
- Удаление записей и откат файла к старой копии не заметить.
- Без пароля видно число записей, размеры и число кусков.
- На FAT32 файл не может быть больше 4 ГБ.
