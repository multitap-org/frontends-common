#include "state_delta.h"

#include <cstring>

// A varint: 7 bits per byte, low bits first, the top bit set on every byte
// but the last. Counts of words fit in 5 bytes.
constexpr size_t VARINT_MAX_BYTES = 5;

static size_t write_varint(uint8_t* out, size_t value)
{
    size_t length = 0;
    while (value >= 0x80)
    {
        out[length++] = static_cast<uint8_t>((value & 0x7F) | 0x80);
        value >>= 7;
    }
    out[length++] = static_cast<uint8_t>(value);
    return length;
}

static bool read_varint(const uint8_t* data, size_t size, size_t* pos, size_t* value)
{
    size_t result = 0;
    for (size_t shift = 0; shift < VARINT_MAX_BYTES * 7; shift += 7)
    {
        if (*pos >= size)
        {
            return false;
        }
        uint8_t byte = data[(*pos)++];
        result |= static_cast<size_t>(byte & 0x7F) << shift;
        if ((byte & 0x80) == 0)
        {
            *value = result;
            return true;
        }
    }
    return false;
}

// Word `index` of a state, a last partial word padded with zeros. memcpy:
// states have no alignment promise (compilers turn it into one load).
static uint32_t load_word(const uint8_t* state, size_t size, size_t index)
{
    uint32_t word = 0;
    size_t offset = index * 4;
    size_t bytes = size - offset < 4 ? size - offset : 4;
    std::memcpy(&word, state + offset, bytes);
    return word;
}

static uint32_t xor_word(const uint8_t* from, const uint8_t* to, size_t size, size_t index)
{
    return load_word(from, size, index) ^ load_word(to, size, index);
}

size_t state_delta_max_size(size_t state_size)
{
    size_t words = (state_size + 3) / 4;
    // Every run but the first starts after at least two zero words and holds
    // at least one changed word, so there are at most words / 3 + 1 runs, each
    // with two varints.
    return words * 4 + (words / 3 + 2) * 2 * VARINT_MAX_BYTES;
}

size_t state_delta_encode(const uint8_t* from, const uint8_t* to, size_t size, uint8_t* out)
{
    size_t words = (size + 3) / 4;
    size_t whole_words = size / 4;
    size_t length = 0;
    size_t i = 0;
    while (i < words)
    {
        // Zero words: 8 bytes at a time while both states have them (the
        // common case, most of a state), then word by word.
        size_t zero_start = i;
        while (i + 2 <= whole_words)
        {
            uint64_t a = 0;
            uint64_t b = 0;
            std::memcpy(&a, from + i * 4, 8);
            std::memcpy(&b, to + i * 4, 8);
            if (a != b)
            {
                break;
            }
            i += 2;
        }
        while (i < words && xor_word(from, to, size, i) == 0)
        {
            i++;
        }
        size_t zeros = i - zero_start;

        // Changed words, until two zero words in a row (or the end).
        size_t changed_start = i;
        while (i < words)
        {
            if (xor_word(from, to, size, i) == 0 && (i + 1 == words || xor_word(from, to, size, i + 1) == 0))
            {
                break;
            }
            i++;
        }
        size_t changed = i - changed_start;

        length += write_varint(out + length, zeros);
        length += write_varint(out + length, changed);
        for (size_t w = changed_start; w < i; w++)
        {
            uint32_t value = xor_word(from, to, size, w);
            std::memcpy(out + length, &value, 4);
            length += 4;
        }
    }
    if (words == 0)
    {
        length += write_varint(out + length, 0);
        length += write_varint(out + length, 0);
    }
    return length;
}

bool state_delta_apply(uint8_t* state, size_t size, const uint8_t* delta, size_t delta_size)
{
    size_t words = (size + 3) / 4;
    size_t i = 0;
    size_t pos = 0;
    while (pos < delta_size)
    {
        size_t zeros = 0;
        size_t changed = 0;
        if (!read_varint(delta, delta_size, &pos, &zeros) || !read_varint(delta, delta_size, &pos, &changed))
        {
            return false;
        }
        if (zeros > words - i || changed > words - i - zeros || changed > (delta_size - pos) / 4)
        {
            return false;
        }
        i += zeros;
        for (size_t w = 0; w < changed; w++, i++)
        {
            uint32_t value = 0;
            std::memcpy(&value, delta + pos, 4);
            pos += 4;
            size_t offset = i * 4;
            size_t bytes = size - offset < 4 ? size - offset : 4;
            uint32_t word = 0;
            std::memcpy(&word, state + offset, bytes);
            word ^= value;
            std::memcpy(state + offset, &word, bytes);
        }
    }
    return i == words;
}
