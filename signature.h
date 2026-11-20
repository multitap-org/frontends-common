#pragma once

// Checks files signed with multitap's index_sign tool (multitap-org/cores,
// tools/index_sign.cpp): the core downloader's indexes, later anything else
// we publish. Monocypher's EdDSA (crypto_eddsa_check: Ed25519's curve with
// BLAKE2b), not standard Ed25519.
//
// A signature file is text:
//   key_id=<16 hex>       the first 8 bytes of BLAKE2b(public key)
//   signature=<128 hex>   over the exact bytes of the signed file
// The frontend carries a few trusted public keys (the current one and the
// next, so a leaked key can be retired by an update) and the signature's
// key_id picks one.

#include <cstddef>
#include <cstdint>

constexpr size_t SIGNATURE_KEY_ID_SIZE = 8;
constexpr size_t SIGNATURE_PUBLIC_SIZE = 32;

struct SignatureKey
{
    uint8_t key_id[SIGNATURE_KEY_ID_SIZE]     = {};
    uint8_t public_key[SIGNATURE_PUBLIC_SIZE] = {};
};

enum class SignatureResult
{
    ok,
    malformed,       // the signature text isn't key_id=... / signature=...
    unknown_key,     // signed with a key we don't trust
    bad_signature,   // a trusted key, but the file was changed (or the signature)
};

// A trusted key from its public key in hex (64 characters, as in index_sign's
// public key file). False if it isn't 32 bytes of hex.
bool signature_key_from_hex(const char* public_hex, SignatureKey* key);

// Checks `message` against the signature text with any of `keys`.
SignatureResult signature_verify(const SignatureKey* keys, int32_t key_count,
                                 const uint8_t* message, size_t message_size,
                                 const char* signature_text, size_t signature_size);

// "ok", "malformed", ... for logs.
const char* signature_result_name(SignatureResult result);

// The key_id as 16 hex characters + NUL (for logs and screens).
void signature_key_id_hex(const SignatureKey* key, char out[SIGNATURE_KEY_ID_SIZE * 2 + 1]);
