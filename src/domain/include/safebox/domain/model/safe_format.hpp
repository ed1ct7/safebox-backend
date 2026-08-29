// Константы формата .safebox, подробно формат описан в docs/safebox-format.md
// AAD (все little-endian):
//   конверт     = версия u32 | соль 16 | ops u64 | mem u64
//   кусок       = версия u32 | blob_id i64 | idx u32 | last u8
//   поле записи = версия u32 | entry_id i64 | тег u8 (name=1, meta=2)
#pragma once
