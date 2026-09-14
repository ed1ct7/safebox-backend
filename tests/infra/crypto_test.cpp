#include <catch2/catch_test_macros.hpp>

#include <set>

#include "safebox/domain/model/safe_format.hpp"
#include "safebox/infra/factories.hpp"

using namespace safebox;
using Code = domain::Error::Code;

namespace {

domain::Bytes aad(std::string_view s) {
    return domain::toBytes(s);
}

bool opens(domain::CryptoSuite& crypto, const domain::KeyHandle& key, const domain::Bytes& sealed,
           const domain::Bytes& ad) {
    return crypto.open(key, sealed, ad).has_value();
}

} // namespace

TEST_CASE("AEAD seal/open round trip and tamper detection", "[crypto]") {
    auto crypto = infra::makeSodiumCrypto();
    auto key = crypto->generateKey().value();
    const auto plain = domain::toBytes("секретные данные");

    auto sealed = crypto->seal(key, plain, aad("ctx")).value();
    CHECK(sealed.size() == plain.size() + domain::kSealOverhead);
    CHECK(crypto->open(key, sealed, aad("ctx")).value() == plain);
    CHECK(crypto->seal(key, plain, aad("ctx")).value() != sealed); // новый nonce

    CHECK(crypto->open(key, sealed, aad("other")).error().code == Code::IntegrityError);
    auto flipped = sealed;
    flipped[domain::kNonceSize + 3] ^= std::byte{0x80};
    CHECK_FALSE(opens(*crypto, key, flipped, aad("ctx")));
    auto truncated = sealed;
    truncated.pop_back();
    CHECK_FALSE(opens(*crypto, key, truncated, aad("ctx")));
    CHECK_FALSE(opens(*crypto, key, domain::Bytes(10), aad("ctx")));
    auto otherKey = crypto->generateKey().value();
    CHECK_FALSE(opens(*crypto, otherKey, sealed, aad("ctx")));

    auto empty = crypto->seal(key, {}, aad("")).value();
    CHECK(empty.size() == domain::kSealOverhead);
    CHECK(crypto->open(key, empty, aad("")).value().empty());
}

TEST_CASE("Argon2id derives the same KEK only for the same inputs", "[crypto][kdf]") {
    auto crypto = infra::makeSodiumCrypto();
    const auto salt = crypto->generateSalt();
    CHECK(salt.size() == domain::kSaltSize);
    auto kek1 = crypto->deriveKek("пароль-1", salt, domain::kMinimalKdf).value();
    auto kek2 = crypto->deriveKek("пароль-1", salt, domain::kMinimalKdf).value();
    auto sealed = crypto->seal(kek1, domain::toBytes("x"), {}).value();
    CHECK(opens(*crypto, kek2, sealed, {}));

    auto otherPassword = crypto->deriveKek("пароль-2", salt, domain::kMinimalKdf).value();
    CHECK_FALSE(opens(*crypto, otherPassword, sealed, {}));
    auto otherSalt =
        crypto->deriveKek("пароль-1", crypto->generateSalt(), domain::kMinimalKdf).value();
    CHECK_FALSE(opens(*crypto, otherSalt, sealed, {}));
    auto otherParams = crypto->deriveKek("пароль-1", salt, domain::KdfParams{2, 16 * 1024}).value();
    CHECK_FALSE(opens(*crypto, otherParams, sealed, {}));

    CHECK(crypto->deriveKek("p", domain::Bytes(8), domain::kMinimalKdf).error().code ==
          Code::IntegrityError);
    CHECK(crypto->deriveKek("p", salt, domain::KdfParams{0, 8192}).error().code ==
          Code::IntegrityError);
    CHECK(crypto->deriveKek("p", salt, domain::KdfParams{1, domain::kMaxKdfMem + 1}).error().code ==
          Code::IntegrityError);
}

TEST_CASE("envelope wraps the master key without exposing it", "[crypto][envelope]") {
    auto crypto = infra::makeSodiumCrypto();
    const auto salt = crypto->generateSalt();
    auto kek = crypto->deriveKek("correct", salt, domain::kMinimalKdf).value();
    auto master = crypto->generateKey().value();
    const auto envAad = domain::aad::envelope(domain::kFormatVersion, salt, domain::kMinimalKdf);
    auto envelope = crypto->wrapKey(kek, master, envAad).value();
    CHECK(envelope.size() == domain::kSealOverhead + domain::kKeySize);

    auto unwrapped = crypto->unwrapKey(kek, envelope, envAad).value();
    const auto data = crypto->seal(master, domain::toBytes("payload"), aad("a")).value();
    CHECK(opens(*crypto, unwrapped, data, aad("a"))); // тот же мастер-ключ

    auto wrongKek = crypto->deriveKek("wrong", salt, domain::kMinimalKdf).value();
    CHECK(crypto->unwrapKey(wrongKek, envelope, envAad).error().code == Code::IntegrityError);
    const auto otherAad =
        domain::aad::envelope(domain::kFormatVersion, salt, domain::KdfParams{2, 8192});
    CHECK(crypto->unwrapKey(kek, envelope, otherAad).error().code == Code::IntegrityError);
    auto shortEnvelope = envelope;
    shortEnvelope.pop_back();
    CHECK(crypto->unwrapKey(kek, shortEnvelope, envAad).error().code == Code::IntegrityError);
}

TEST_CASE("subkeys are separated by purpose and deterministic", "[crypto][subkeys]") {
    auto crypto = infra::makeSodiumCrypto();
    auto master = crypto->generateKey().value();
    auto names = crypto->deriveSubkey(master, domain::KeyPurpose::Names).value();
    auto namesAgain = crypto->deriveSubkey(master, domain::KeyPurpose::Names).value();
    auto content = crypto->deriveSubkey(master, domain::KeyPurpose::Content).value();
    auto thumbs = crypto->deriveSubkey(master, domain::KeyPurpose::Thumbnails).value();

    const auto sealed = crypto->seal(names, domain::toBytes("имя"), {}).value();
    CHECK(opens(*crypto, namesAgain, sealed, {}));
    CHECK_FALSE(opens(*crypto, content, sealed, {}));
    CHECK_FALSE(opens(*crypto, thumbs, sealed, {}));
    CHECK_FALSE(opens(*crypto, master, sealed, {}));
}

TEST_CASE("chunk AAD binds blob, index and the last flag", "[crypto][aad]") {
    auto crypto = infra::makeSodiumCrypto();
    auto key = crypto->generateKey().value();
    const auto sealed =
        crypto->seal(key, domain::toBytes("chunk"), domain::aad::chunk(5, 1, false)).value();
    CHECK(opens(*crypto, key, sealed, domain::aad::chunk(5, 1, false)));
    CHECK_FALSE(opens(*crypto, key, sealed, domain::aad::chunk(5, 2, false))); // перестановка
    CHECK_FALSE(opens(*crypto, key, sealed, domain::aad::chunk(6, 1, false))); // чужой блоб
    CHECK_FALSE(opens(*crypto, key, sealed, domain::aad::chunk(5, 1, true)));  // обрезка
    const auto field =
        crypto->seal(key, domain::toBytes("n"), domain::aad::field(7, domain::FieldTag::Name))
            .value();
    CHECK_FALSE(opens(*crypto, key, field, domain::aad::field(8, domain::FieldTag::Name)));
    CHECK_FALSE(opens(*crypto, key, field, domain::aad::field(7, domain::FieldTag::Meta)));
}

TEST_CASE("session tokens are random base64url strings", "[crypto][tokens]") {
    auto crypto = infra::makeSodiumCrypto();
    std::set<std::string> tokens;
    for (int i = 0; i < 100; ++i) {
        const auto token = crypto->generateToken();
        CHECK(token.size() == 43);
        CHECK(token.find_first_not_of(
                  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_") ==
              std::string::npos);
        tokens.insert(token);
    }
    CHECK(tokens.size() == 100);
    CHECK(crypto->equalConstantTime("abc", "abc"));
    CHECK_FALSE(crypto->equalConstantTime("abc", "abd"));
    CHECK_FALSE(crypto->equalConstantTime("abc", "abcd"));
}
