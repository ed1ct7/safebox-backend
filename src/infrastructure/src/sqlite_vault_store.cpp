// VaultStore на sqlite.
// removeSubtree через рекурсивный CTE с UNION, чтобы не зациклиться на битом parent_id.
// close(): в EXCLUSIVE режиме sqlite не удаляет -journal, а только обнуляет, так что удаляем сами
