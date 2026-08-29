// Порт для zip-экспорта. Пишем строго последовательно (в http-ответе seek не сделать),
// поэтому STORE + data descriptor + ZIP64
#pragma once
