// AAD: имя/мета = версия|entry_id|тег поля, кусок = версия|blob_id|idx|last.
// Так имена нельзя переставить между записями, а куски между блобами.
// В enc_meta еще лежат копии blob_id/thumb_blob_id, их сверяем с открытыми колонками.
//
// формат enc_meta v1 (little-endian):
//   u8 layout=1 | u8 kind | u64 size | i64 createdAt | i64 modifiedAt |
//   u8 flags (bit0 blob, bit1 thumb) | [i64 blobId] | [i64 thumbBlobId] |
//   u16 len + mime | u32 len + url
