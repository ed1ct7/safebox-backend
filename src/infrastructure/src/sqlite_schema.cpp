// Схема файла, подробнее в docs/safebox-format.md
// При создании: page_size=65536, auto_vacuum=INCREMENTAL (потом без VACUUM не поменять).
// На каждом соединении: journal_mode=DELETE (чтобы рядом не было -wal), locking_mode=EXCLUSIVE.
// id сделаны AUTOINCREMENT, чтобы не переиспользовались - они входят в AAD
