// Порт криптографии, реализация на libsodium в infra.
// Байты ключей живут только внутри адаптера (sodium_malloc), наружу отдаем KeyHandle.
// seal: XChaCha20-Poly1305, nonce пишется в начало шифртекста
#pragma once
