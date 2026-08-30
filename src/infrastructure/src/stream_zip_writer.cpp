// Свой потоковый zip: miniz пишет с seek-ами, а нам надо строго последовательно.
// От miniz берем только mz_crc32.
// STORE, data descriptor, zip64 при >= 4 ГБ, имена в UTF-8
