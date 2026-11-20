// signature: a file signed by multitap-org/cores' index_sign (a test key made
// for this, not the real one) verifies; a changed file, a changed signature,
// another key and broken text don't.

#include <cstdio>
#include <cstring>

#include "signature.h"

static int32_t failures = 0;

static void check(bool condition, const char* what)
{
    if (!condition)
    {
        std::printf("FAIL: %s\n", what);
        failures++;
    }
}

// index_sign keygen / sign of "multitap signature test\n".
static const char* const TEST_PUBLIC = "66ed9135cc62c8225c4e459dd0f48665bd73e05e673deaa0c500068a222dd032";
static const char* const TEST_KEY_ID = "72a7c22ac80946a2";
static const char* const TEST_SIGNATURE =
    "key_id=72a7c22ac80946a2\n"
    "signature=a16ceeb74d04875b2817d093b36756217b3103a3a5c65eef03333fabe09df9b0"
    "0458280953749beb60731e8ff58959b1f9bb963f2fafc44e9b4509efdfce410f\n";
// The live server's key (multitap-org/cores keys/current.public).
static const char* const LIVE_PUBLIC = "19c0ec4760edd7a7878b71bfd63d197191077c37193aef0bfa94a41d421eae5b";

static SignatureResult verify(const SignatureKey* keys, int32_t count, const char* message, const char* signature)
{
    return signature_verify(keys, count, reinterpret_cast<const uint8_t*>(message), std::strlen(message),
                            signature, std::strlen(signature));
}

int main()
{
    const char* message = "multitap signature test\n";
    SignatureKey keys[2];
    check(signature_key_from_hex(LIVE_PUBLIC, &keys[0]), "live key parses");
    check(signature_key_from_hex(TEST_PUBLIC, &keys[1]), "test key parses");

    char id[SIGNATURE_KEY_ID_SIZE * 2 + 1];
    signature_key_id_hex(&keys[1], id);
    check(std::strcmp(id, TEST_KEY_ID) == 0, "key_id = index_sign's");
    signature_key_id_hex(&keys[0], id);
    check(std::strcmp(id, "20ad1a6e8a2bfdca") == 0, "live key_id");

    check(verify(keys, 2, message, TEST_SIGNATURE) == SignatureResult::ok, "good signature, second key");
    check(verify(&keys[1], 1, message, TEST_SIGNATURE) == SignatureResult::ok, "good signature, one key");
    check(verify(keys, 1, message, TEST_SIGNATURE) == SignatureResult::unknown_key, "key not trusted");
    check(verify(keys, 2, "multitap signature test!\n", TEST_SIGNATURE) == SignatureResult::bad_signature,
          "changed message");

    char changed[256];
    std::snprintf(changed, sizeof(changed), "%s", TEST_SIGNATURE);
    char* last = std::strrchr(changed, 'f');
    *last = 'e';
    check(verify(keys, 2, message, changed) == SignatureResult::bad_signature, "changed signature");

    // CRLF line ends (a file edited on Windows) still parse.
    const char* crlf =
        "key_id=72a7c22ac80946a2\r\n"
        "signature=a16ceeb74d04875b2817d093b36756217b3103a3a5c65eef03333fabe09df9b0"
        "0458280953749beb60731e8ff58959b1f9bb963f2fafc44e9b4509efdfce410f\r\n";
    check(verify(keys, 2, message, crlf) == SignatureResult::ok, "CRLF");

    check(verify(keys, 2, message, "") == SignatureResult::malformed, "empty");
    check(verify(keys, 2, message, "key_id=72a7c22ac80946a2\n") == SignatureResult::malformed, "no signature line");
    check(verify(keys, 2, message, "key_id=zz\nsignature=00\n") == SignatureResult::malformed, "bad hex");
    check(!signature_key_from_hex("1234", &keys[0]), "short public key refused");
    check(!signature_key_from_hex(nullptr, &keys[0]), "null public key refused");

    if (failures == 0)
    {
        std::printf("signature: all checks passed\n");
        return 0;
    }
    return 1;
}
