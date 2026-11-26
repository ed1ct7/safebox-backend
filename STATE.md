# STATE — SafeBox: подготовка к большому архиву
Обновлено: 2026-11-25. Ф1-Ф3 в main и origin (Ф3 = 0c4a86c, Ф2 = b5c122b..c3523f0); REST в Linqtab = Ф3
(`docs-draft\make_rest_f3.py`). Отчёт и план: `AUDIT.md` (рядом, не в git). Ф2 закрыта: big-f2 вход 372 мс, листинг 60k 0,77 с.

## Сделано
- Ф1 (в main): enc_meta layout 3 = layout 2 + `u16 extFlags` и source, sourceUrl, sourceDate,
  sha256, dimensions, durationMs, trashedAt (`sealing.cpp`); схема 4, миграции 2->3->4 одной
  транзакцией (`sqlite_schema.cpp`); корзина - только `trashedAt` у корня поддерева, parent_id
  не меняется. Golden, миграции, откат; копии tiny = 2.1; миграция после пароля (7deb420); Linqtab = схема 4.

## Боевой сейф — aaa.safebox (содержимое не показывать; все сейфы - в `C:\Secret\SafeBox\vaults\`)
- `C:\Secret\SafeBox\vaults\aaa.safebox` (схема 3, 3,14 ГБ). Копии на стенде `aaa\aaa-{a,b,t}.safebox`;
  миграция 3->4 на копиях OK (`jet\jet_check.py`: записи, теги, файлы, миниатюры; вход 0,41 с).
- jet (формат прототипа) перенесён в aaa 2026-11-24 (`jet\jet_transfer.py`): 2785/2785 по sha256,
  2843 записи в «Перенос из jet»; бэкап до переноса `backup\SafeBox-backup-2026-11-24\aaa.safebox.bak`.

## Ф2 (f2-catalog) - что есть
- Ф2.1 каталог: узлы общие, `catalogChanged` -> правка только изменённых строк; правка ->
  чтение 0,6 мс (было 1,5 с). Ф2.2 пачка 256 файлов / 64 МиБ одной транзакцией, миниатюры до 8
  потоков, дедуп по sha256 (`duplicates`, manifest `onDuplicate`), досчёт хешей в фоне
  (`hashes` в статусе): 223 фото/с (было 43,6). Ограничение: папки пути создаются до хеша.
- Ф2.3 `sort/order/offset/limit` + `total` в `/entries` и `/search`: страница 100 из 60k 4,9 мс.
- Ф2.4 `POST /safe/backup` (SQLite backup API, `.part`): 3,65 ГБ за 6 с. Ф2.5 корзина: `/trash`,
  `/trash/restore`, `/trash/purge {ids|before|all}`; в корзину 44 мс, стереть 307 мс (папка 60k).
- Ф2.6 `PUT /media/:id/thumbnail` (до 64 МиБ): JPEG 1080p 32 мс. Ф2.7 PATCH source*, `/sources`. Формат не тронут.
- Отложено: пик памяти миниатюр (8 потоков) на огромных фото (замерить на архиве); backup при
  BUSY без паузы; дедуп не видит стёртое во время импорта; ошибка копии не видна после входа;
  отброшенный повтор X теряется, если X заменён в том же импорте. REST Ф2 в Linqtab.

## Ф3 (f3-import) - загрузка архива
- Бэкенд: manifest импорта - `sha256` (сверка) и поля записи `description/tags/source/sourceUrl/
  sourceDate` (`entry_details.cpp`: проверка до папок пути, теги - живые в транзакции записи);
  ответ импорта `importedFiles [{path,id}]`; `POST /entries/lookup` (до 10k, вне корзины);
  `/links` - те же поля (ошибка - 422 на пачку); `imports.active` в статусе (импорт считается
  до конца деструктора сессии); lookup не ищет пустой хеш. Формат не тронут. Тесты `[archive]`.
- `C:\Secret\SafeBox\app\safebox-tagger\import_manifest.py` (+ lookup/import_files/create_links/entry/
  patch_entry в `safebox_api.py`, бэкап `safebox-bench\safebox_api.py.bak-f3`): JSONL, sha256
  локально (кэш по размеру+mtime), ждет `hashes.pending` и `imports.active`, lookup, теги,
  multipart потоком (`filename*=UTF-8''`), путь чистится как на сервере, значения проверяются
  до отправки (`invalid`), журнал sqlite + блокировка `.lock`; поля строк с уже имевшимся
  содержимым дописываются (теги + пустые поля). Сухой прогон по умолчанию.
- Ревью агентом: 11 находок; исправлены 1-5, 7-11; 6 (папки при несовпавшем sha256) - как у
  повтора, в документе. `f3_import_e2e.py`: 226 проверок, 4 обрыва подряд без паузы и второй
  запуск на журнале; `f3_race_probe.py`: дописывание после обрыва 0,11 с, копий 0.
  Загрузка 57-79 МиБ/с (большие файлы), 3000 фото ~30 КБ ~1000/с; хеш локально ~560 МиБ/с.
  lookup 10k хешей на big-f2 (250k) - 16 мс. REST-черновик: `docs-draft\make_rest_f3.py`.

## Решено / ждём
- 2026-11-25 (AUDIT §5): архив - свой новый сейф, не aaa; личное видео внутри, крупное (>~10 ГБ
  или >половины) - отдельный сейф; публичное видео - ссылками; локальных ссылок нет.
  Данные готовит сессия «Архив личных сохранёнок» (C:\Secret\SafeBox\archive-migration): уникальных
  1828 файлов / 45,2 ГБ (видео 347 / 17,9 ГБ; решение: всё в ОДНОМ новом сейфе, видео тоже,
  отдельного под видео нет; путь `C:\Secret\SafeBox\vaults\archive.safebox`), ссылок ~12,2k (YouTube 11,6k, шортсы - ссылками). Manifest
  `out\safebox_manifest.jsonl` - после одобрения правил. Предпросмотры ссылок выключены: сервер
  со своим APPDATA, linkPreviews=0; загрузчик с включёнными отказывает (`--allow-link-previews`,
  `f3_previews_guard.py`); включение потом старые не догружает (только POST /entries/:id/preview).
- Загрузчик: поле `thumbnail` (+width/height/durationMs) -> PUT /media/:id/thumbnail; чужой
  записи - только без миниатюры; журнал `thumbed`. `f3_thumbs.py` 21/21, e2e 226/226.
- Ф4: макет `mockup\` (не в git); этап 2 (клиент 411c549, + таблицы ac8071b; main, запушено; релиз `safeboxd-f4c.exe`): аудио (oga/flac/wav/mp3/m4a/aac играют с octet-stream, сервер не трогали) и текст (2 МиБ, UTF-8/16/1251) в просмотре, .md - react-markdown (lazy, без HTML/картинок); ревью 8 - исправлены; vitest 702.

## Ф4 этап 1 (клиент 0865ba5 в main и f4-client, бэкенд CSP 69abba8; запушено)
- Модель B; стрелки 2D (колонки по offsetTop), Shift+стрелки; Del - сразу в корзину, тост
  «Отменить» и Ctrl+Z (один раз на пачку, и из просмотра); экран «Корзина» под деревом
  (восстановить, навсегда, очистить - с подтверждением). `MediaViewer` вместо Lightbox/VideoModal:
  фото+видео+ролики YouTube одной лентой, Del -> следующий; `lib/youtube.ts` (watch/youtu.be/
  shorts/live/embed, t=, теги-запреты, без сети - обложка). Бэкенд: SPA CSP `frame-src
  youtube-nocookie` (+тест), под CSP+no-referrer плеер грузится. Ревью: 8 находок исправлены
  (автоповтор Del/пробела, Ctrl+Z после ошибки, индекс просмотра по id, корзина и поиск/вставка).
  vitest 646/646 (код 0), tsc, vite build; ctest 4/4.

## К2 виртуализация (тот же коммит 0865ba5; релиз `safebox-bench\safeboxd-f4.exe`)
- `/entries` и `/search` страницами (200, дальше по 2000; `useInfiniteQuery`, `useAllPages`), в строке
  состояния «загружается N из M» / «загрузка прервана»; папка поменялась между страницами - `stale`,
  перечитать (до 3 раз). Сетка `Gallery` виртуальная (`lib/gridLayout.ts`: видимые строки +3, высота
  на total, заглушки, строки фиксированной высоты), стрелки/Home/End/рамка по геометрии (`GalleryHandle`),
  выделение по id; предпросмотры - точечно `GET /entries/:id` + `patchCachedEntries` (и имя/свойства).
- big-f2: Saved Messages 47k - первые карточки 0,5 с, всё 1,5 с, 532 DOM-узла, 46 МБ (было 15,4 с,
  519k, 391 МБ); Watch later 60k - 0,29 с / 2,3 с, 502 узла, 73 МБ. Ревью агентом: 10 находок, 9 исправлено
  (6 - правка кэша во время догрузки теряется - лечится перечитыванием). vitest 663/663, tsc, build.
- Ограничения: Ctrl+A до конца загрузки - подсказка; любое изменение = перечитывание всех страниц
  (60k ~2 с, данные на экране); >~150k карточек в одну колонку - предел высоты элемента браузера.

## Окружение
- 2026-11-25 всё в `C:\Secret\SafeBox\`: `app\` (backend, frontend+tables, tagger, bench), `vaults\`, `archive-migration\`. Кэш CMake в build - старого пути: перед сборкой удалить `build\<preset>`, затем build.ps1.
- Стенд `app\safebox-bench`: `safeboxd-{2.1,v4,f4c,f5}.exe`, свой APPDATA (`appdata\SafeBox\settings.ini`: linkPreviews=0).
  - `big.safebox` (`bench-password-1`): ~250k записей, 3,66 ГБ; `big-f2` - его копия схемы 4;
    `tiny.safebox` (4,3k, тот же пароль); `walk.safebox` (`walk-pass-1`); `drytest\`.
  - Скрипты: `bench_gen.py`, `fill_fast.py`, `tags_light.py`, `measure.py`, `walk.mjs`,
    `probe-big.mjs`, `jet\jet_check.py`, `import_bench.py`, `measure_{backfill,page,backup,trash,thumb,source}.py`.
  - Python: `C:\Secret\SafeBox\app\safebox-tagger\.venv\Scripts\python.exe`. Linqtab: продукт Safebox; креды — из чата,
    не на диске; PATCH — сырым UTF-8 (иначе 413).
## Загрузка архива (`safebox-bench/archive_*.py`, сервер :8920 launch `safeboxd-archive`)
- 1б пересоздан (`archive_verify.py --totals`); 1б2 (`archive_1b2.py`+`_fix.py`): 117 реклам и 7 старых заметок в
  корзину, строка 5224 удалена пользователем; 14146/2106/12040, лишних 0; копия `backup/
  archive-1b-2026-11-25.safebox`. Урок: lookup не видит корзину (13 файлов вернулись) -> `--update` их не грузит.
- `import_manifest.py --update [--prune]`+`manifest_update.py`: sha256/normalizeUrl -> перенос/имя/адрес/теги/поля;
  журнал: given (правки клиента целы), pruned (возврат цепочкой из корзины), хеши по sha1 пути+индекс; ревью 11
  + 10 + 6 - исправлены; бэкенд 6c11403 (не запушен): `POST /folders`, «a.com (2)»; e2e 79/79. Копия: 100 переносов+77
  тегов+5 в корзину за 4,8 с (было ~18 мин). Рабочий archive 17:56 (`archive_update_real.py`): -1 тег у 27 ссылок, сверка OK; копии ДО/ПОСЛЕ.
- Backup 40 ГБ: status ждал до 25 с -> 343924c (`lockConn` считает ждущих, уступка до 250 мс): 2,6 с; f4b.
