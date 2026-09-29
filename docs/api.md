# API `/api/v1`

По этому файлу пишется `frontend/src/api/types.ts`. Код - `src/http/src/*_api.cpp`,
DTO - `src/http/src/dto.hpp`.

## Общее

- Сервер `http://127.0.0.1:8900`. Тела запросов и ответов JSON (UTF-8), кроме импорта
  (multipart) и медиа.
- После `create`/`unlock` приходит `token`, дальше он шлется в каждом запросе:
  `Authorization: Bearer <token>`. На фронте удобно хранить в `sessionStorage`.
- Там же сервер ставит cookie `sbx_media` (`Path=/api/v1/media; HttpOnly; SameSite=Strict`).
  Она нужна для `<img src>`, `<video src>` и скачивания, и работает только на GET
  медиа-эндпоинтах. Остальные по cookie отвечают 401.
- 401 значит, что сессии нет (нет токена, сейф заблокирован, операцию прервал lock).
  На любой 401 фронт показывает экран входа. Неверный пароль - это 403 `wrong_password`.
- До любой логики сервер проверяет:
  - `Host` - `127.0.0.1:<port>`, `localhost:<port>`, `[::1]:<port>` или из `--allow-host`, иначе 403
  - на POST/PATCH/PUT/DELETE: `Origin` (если есть) должен быть разрешенным или из
    `--allow-origin`, `Sec-Fetch-Site: cross-site` запрещен, иначе 403
  - тело (кроме импорта) только `application/json` (иначе 415), до 64 КиБ (иначе 413),
    без chunked (411)
- Время в unix-миллисекундах, размеры в байтах, id - целые > 0.

## Ошибки

Тело ошибки: `{"error": {"code": "...", "message": "..."}}`. `message` на русском, можно
показывать как есть.

| HTTP | code | когда |
|---|---|---|
| 400 | `bad_request` | битый JSON, нет поля, кривой id |
| 401 | `unauthorized` | нет сессии |
| 403 | `wrong_password` | неверный пароль |
| 403 | `forbidden` | чужой Host / Origin |
| 404 | `not_found` | нет записи, файла или маршрута |
| 409 | `already_exists` | файл сейфа уже есть |
| 411, 413, 415 | `length_required`, `payload_too_large`, `unsupported_media_type` | не то тело |
| 416 | `range_not_satisfiable` | Range за концом файла |
| 422 | `invalid_argument` | короткий пароль, пароли не совпали, плохое имя или путь |
| 422 | `not_a_safe` | файл не сейф (проверяется до пароля) |
| 500 | `io_error`, `integrity_error`, `internal` | диск, порча данных, прочее |

## Эндпоинты

| | путь | авторизация |
|---|---|---|
| GET | `/health` | нет |
| GET | `/api/v1/safe/status` | не обязательна |
| POST | `/api/v1/safe/create` | нет |
| POST | `/api/v1/safe/unlock` | нет |
| POST | `/api/v1/safe/lock` | Bearer |
| POST | `/api/v1/safe/password` | Bearer |
| POST | `/api/v1/safe/heartbeat` | Bearer |
| GET | `/api/v1/entries[?parentId=]` | Bearer |
| GET, PATCH, DELETE | `/api/v1/entries/:id` | Bearer |
| POST | `/api/v1/entries/delete` | Bearer |
| GET | `/api/v1/folders` | Bearer |
| POST | `/api/v1/import[?parentId=]` | Bearer |
| GET | `/api/v1/search?q=&limit=` | Bearer |
| GET | `/api/v1/media/:id/thumbnail`, `content`, `download`, `zip` | Bearer или cookie |
| GET | все остальное вне `/api/` | фронт |

## Типы

```ts
export type EntryKind = 'folder' | 'file' | 'photo' | 'video' | 'link';

export interface Entry {
  id: number;
  parentId: number | null;   // null - корень
  kind: EntryKind;
  name: string;
  size: number;              // у папки 0
  mime: string;              // у папки ''
  hasThumbnail: boolean;
  createdAt: number;
  modifiedAt: number;
  url?: string;              // только у link
  domain?: string;           // только у link
}

export interface PathItem { id: number; name: string; }

export interface Listing {
  folder: Entry | null;      // null - корень
  path: PathItem[];          // от корня до папки включительно
  entries: Entry[];          // сначала папки, потом по имени
}

export interface FolderNode { id: number; parentId: number | null; name: string; }

export interface SafeInfo {
  path: string;
  entryCount: number;
  idleRemainingSec: number;
}

export interface SafeStatus {
  unlocked: boolean;         // открыт ли вообще какой-то сейф
  authorized: boolean;       // жив ли переданный токен
  lastPath: string | null;   // последний открытый сейф, для формы входа
  defaultDirectory: string;  // от нее считаются относительные пути
  safe?: SafeInfo;           // если authorized
}

export interface Session { token: string; safe: SafeInfo; }

export interface SearchHit { entry: Entry; path: PathItem[]; }

export interface ImportResult {
  imported: number;
  failed: number;
  skipped: number;            // уже были в папке (то же имя и те же байты)
  failures: { path: string; message: string }[];
}

export interface ApiError { error: { code: string; message: string } }
```

## Сейф

`GET /health` -> `{"status":"ok","version":"2.0.0"}`

`GET /api/v1/safe/status` -> `SafeStatus`. Первый запрос фронта при загрузке.

`POST /api/v1/safe/create`
```json
{ "path": "Мой сейф", "password": "...", "confirm": "..." }
```
Путь полный или относительно `defaultDirectory`, `.safebox` допишется сам.
Ответ `201 Session` + cookie. Ошибки: 409 если файл есть, 422 если пароль короче 6
символов, пароли не совпали, пустой путь или нет папки. Если был открыт другой сейф,
он блокируется.

`POST /api/v1/safe/unlock`
```json
{ "path": "C:\\Users\\me\\Documents\\Мой сейф.safebox", "password": "..." }
```
`.safebox` можно не писать. Ответ `200 Session` + cookie. Ошибки: 403 `wrong_password`,
404, 422 `not_a_safe`, 500 (например сейф уже открыт другим процессом). Если открыт
тот же сейф (вторая вкладка) - выдается новый токен в той же сессии. Если другой -
сначала проверяется пароль, и только потом текущий блокируется, так что опечатка его
не закроет.

`POST /api/v1/safe/lock` - без тела, `204`, cookie стирается. Все токены умирают.

`POST /api/v1/safe/password`
```json
{ "oldPassword": "...", "newPassword": "...", "confirm": "..." }
```
`204`. 403 если старый неверный, 422 если новый не подходит. Перешифровывается только
конверт ключа, так что это быстро, и сессия продолжает работать.

`POST /api/v1/safe/heartbeat`
```json
{ "active": true }
```
Слать раз в 20с. `active` - была ли активность пользователя с прошлого раза (мышь,
клавиатура, переходы; идущий импорт или видео тоже можно считать). Ответ
`{"idleRemainingSec": 612}`. 15 минут без active - автоблокировка, 2 минуты без
heartbeat - тоже. Когда `idleRemainingSec <= 60`, стоит показать предупреждение.

## Записи

`GET /api/v1/entries[?parentId=<id>]` -> `Listing`. Без parentId - корень. 404 если нет
папки, 422 если это не папка.

`GET /api/v1/entries/:id` -> `Entry`

`GET /api/v1/folders` -> `{"folders": FolderNode[]}`, все папки для дерева, родители
раньше детей.

`PATCH /api/v1/entries/:id` с `{"name": "..."}` -> `Entry`. Нельзя пустое имя, `.`, `..`,
символы `/ \ : * ? " < > |` и длиннее 255 байт - 422.

`DELETE /api/v1/entries/:id` -> `{"removed": n}` вместе с содержимым папки.

`POST /api/v1/entries/delete` с `{"ids": [1, 2, 3]}` -> `{"removed": n}`, все одной
транзакцией.

## Импорт

`POST /api/v1/import[?parentId=<id>]`, `multipart/form-data`. Сервер читает поток и
ничего не держит в памяти, так что размер файлов любой. Каждая часть с `filename` это
файл, в `filename` может быть относительный путь - папки создадутся:

```ts
const fd = new FormData();
for (const file of files) fd.append('file', file, file.webkitRelativePath || file.name);
await fetch(`/api/v1/import?parentId=${folderId}`, { method: 'POST', body: fd, headers: { Authorization } });
```

Ответ `ImportResult`. Если какой-то файл не импортировался, он попадает в `failures`,
остальные импортируются. Если в целевой папке уже есть файл с тем же именем (без учета
регистра) и побайтно тем же содержимым, второй не вставляется и считается в `skipped`:
повторный импорт папки докачивает только новое. Тот же размер при другом содержимом -
новый файл рядом. Папки по пути сливаются с существующими по имени без учета регистра.
Тип определяется по содержимому: фото (jpeg, png, gif, webp, bmp - с миниатюрой, avif и
анимированный webp - без нее), видео (mp4, mov, webm, mkv, 3gp), ссылка (`.url` ярлык),
остальное файл. Если во время импорта сейф заблокировался - 401, то что успело
импортироваться остается.

## Поиск

`GET /api/v1/search?q=<текст>&limit=<n>` -> `{"query": "...", "results": SearchHit[]}`.
По именам во всем сейфе, без учета регистра, "ё" = "е". Пустой q - пустой список.
limit по умолчанию 200, максимум 1000.

## Медиа

Можно по Bearer или по cookie.

- `GET /media/:id/thumbnail` - jpeg до 400px, 404 если миниатюры нет
- `GET /media/:id/content` - содержимое, поддерживает Range (206). Фото и видео отдаются
  inline, все остальное как `attachment` + `application/octet-stream`, чтобы html/svg
  из сейфа не исполнялись в браузере
- `GET /media/:id/download` - файл с оригинальным именем, Range тоже работает. Для папки
  то же что `/zip`
- `GET /media/:id/zip` - папка zip-ом (chunked). Одинаковые имена переименуются в
  `имя (2).ext`. Не папка - 422

У медиа `Cache-Control: no-store`, чтобы расшифрованное не оседало в кеше браузера.

## Фронт

Все, что не `/api/`, отдается из вшитого `frontend/dist` (`-DSAFEBOX_WEB_DIST=...`).
Неизвестный путь без расширения отдает `index.html`, `/assets/*` кешируются навсегда.
Если фронт не вшит, на `/` будет заглушка.

Для разработки запускать `safeboxd --allow-origin http://localhost:5173` и в vite
настроить прокси (changeOrigin нужен, сервер пускает только Host 127.0.0.1:8900):

```ts
server: { proxy: { '/api': { target: 'http://127.0.0.1:8900', changeOrigin: true } } }
```
