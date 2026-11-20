#include "signature.h"

#include <cstring>

#include "monocypher.h"

constexpr size_t SIGNATURE_SIZE = 64;

static int32_t hex_digit(char c)
{
    if (c >= '0' && c <= '9')
    {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f')
    {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F')
    {
        return c - 'A' + 10;
    }
    return -1;
}

// Exactly `size` bytes of hex in text[0 .. text_size).
static bool hex_decode(const char* text, size_t text_size, uint8_t* out, size_t size)
{
    if (text_size != size * 2)
    {
        return false;
    }
    for (size_t i = 0; i < size; i++)
    {
        int32_t high = hex_digit(text[i * 2]);
        int32_t low  = hex_digit(text[i * 2 + 1]);
        if (high < 0 || low < 0)
        {
            return false;
        }
        out[i] = static_cast<uint8_t>(high * 16 + low);
    }
    return true;
}

// Finds "key=value" at the start of a line in text[0 .. size); the value runs
// to the end of the line (a trailing '\r' is dropped). False if absent.
static bool field_find(const char* text, size_t size, const char* key, const char** value, size_t* value_size)
{
    size_t key_size = std::strlen(key);
    size_t line = 0;
    while (line < size)
    {
        size_t end = line;
        while (end < size && text[end] != '\n')
        {
            end++;
        }
        if (end - line > key_size && std::memcmp(text + line, key, key_size) == 0 && text[line + key_size] == '=')
        {
            size_t value_end = end;
            if (value_end > line && text[value_end - 1] == '\r')
            {
                value_end--;
            }
            *value      = text + line + key_size + 1;
            *value_size = value_end - (line + key_size + 1);
            return true;
        }
        line = end + 1;
    }
    return false;
}

bool signature_key_from_hex(const char* public_hex, SignatureKey* key)
{
    *key = {};
    if (public_hex == nullptr || !hex_decode(public_hex, std::strlen(public_hex), key->public_key, SIGNATURE_PUBLIC_SIZE))
    {
        return false;
    }
    crypto_blake2b(key->key_id, SIGNATURE_KEY_ID_SIZE, key->public_key, SIGNATURE_PUBLIC_SIZE);
    return true;
}

SignatureResult signature_verify(const SignatureKey* keys, int32_t key_count,
                                 const uint8_t* message, size_t message_size,
                                 const char* signature_text, size_t signature_size)
{
    const char* id_text = nullptr;
    size_t id_size = 0;
    const char* sig_text = nullptr;
    size_t sig_size = 0;
    uint8_t key_id[SIGNATURE_KEY_ID_SIZE];
    uint8_t signature[SIGNATURE_SIZE];
    if (!field_find(signature_text, signature_size, "key_id", &id_text, &id_size) ||
        !field_find(signature_text, signature_size, "signature", &sig_text, &sig_size) ||
        !hex_decode(id_text, id_size, key_id, sizeof(key_id)) ||
        !hex_decode(sig_text, sig_size, signature, sizeof(signature)))
    {
        return SignatureResult::malformed;
    }
    for (int32_t i = 0; i < key_count; i++)
    {
        if (std::memcmp(keys[i].key_id, key_id, SIGNATURE_KEY_ID_SIZE) != 0)
        {
            continue;
        }
        if (crypto_eddsa_check(signature, keys[i].public_key, message, message_size) == 0)
        {
            return SignatureResult::ok;
        }
        return SignatureResult::bad_signature;
    }
    return SignatureResult::unknown_key;
}

const char* signature_result_name(SignatureResult result)
{
    switch (result)
    {
        case SignatureResult::ok:            return "ok";
        case SignatureResult::malformed:     return "malformed";
        case SignatureResult::unknown_key:   return "unknown key";
        case SignatureResult::bad_signature: return "bad signature";
    }
    return "?";
}

void signature_key_id_hex(const SignatureKey* key, char out[SIGNATURE_KEY_ID_SIZE * 2 + 1])
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < SIGNATURE_KEY_ID_SIZE; i++)
    {
        out[i * 2]     = digits[key->key_id[i] >> 4];
        out[i * 2 + 1] = digits[key->key_id[i] & 0x0f];
    }
    out[SIGNATURE_KEY_ID_SIZE * 2] = '\0';
}
