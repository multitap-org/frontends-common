// Checks state_delta.h: a delta turns A into B and the same delta turns B
// back into A (random states, small edits, sizes that aren't a multiple of
// 4, nothing changed, everything changed, empty states), it fits
// state_delta_max_size, it's small when little changed, and malformed or
// truncated deltas are refused.
//
// Exit code 0 = all checks passed. Each failure is printed.

#include <cstdio>
#include <cstring>
#include <vector>

#include "state_delta.h"

static int g_failures = 0;

static void check(bool condition, const char* what, size_t size)
{
    if (!condition)
    {
        std::printf("FAIL %s (size %zu)\n", what, size);
        g_failures++;
    }
}

static uint32_t g_random = 12345;

static uint32_t next_random()
{
    g_random = g_random * 1103515245u + 12345u;
    return g_random >> 8;
}

static void fill_random(std::vector<uint8_t>* state, size_t size)
{
    state->resize(size);
    for (size_t i = 0; i < size; i++)
    {
        (*state)[i] = static_cast<uint8_t>(next_random());
    }
}

// Encodes from -> to, checks the size bound, then applies it both ways.
// Returns the delta's size.
static size_t round_trip(const std::vector<uint8_t>* from, const std::vector<uint8_t>* to, const char* what)
{
    size_t size = from->size();
    std::vector<uint8_t> delta(state_delta_max_size(size));
    size_t length = state_delta_encode(from->data(), to->data(), size, delta.data());
    check(length <= delta.size(), what, size);

    std::vector<uint8_t> state = *from;
    check(state_delta_apply(state.data(), size, delta.data(), length), what, size);
    check(state == *to, what, size);
    check(state_delta_apply(state.data(), size, delta.data(), length), what, size);
    check(state == *from, what, size);
    return length;
}

static void check_size(size_t size)
{
    std::vector<uint8_t> a;
    std::vector<uint8_t> b;
    fill_random(&a, size);

    // Nothing changed: a couple of bytes.
    b = a;
    size_t same = round_trip(&a, &b, "unchanged");
    check(same <= 8, "unchanged is tiny", size);

    // Everything changed (every byte differs: the worst case).
    b = a;
    for (size_t i = 0; i < size; i++)
    {
        b[i] = static_cast<uint8_t>(b[i] ^ 0xA5);
    }
    round_trip(&a, &b, "all changed");

    // A few scattered bytes, including the first and the last.
    if (size > 0)
    {
        b = a;
        b[0] = static_cast<uint8_t>(b[0] + 1);
        b[size - 1] = static_cast<uint8_t>(b[size - 1] + 1);
        for (int i = 0; i < 20; i++)
        {
            size_t at = next_random() % size;
            b[at] = static_cast<uint8_t>(b[at] + 1);
        }
        size_t few = round_trip(&a, &b, "few changed");
        if (size >= 4096)
        {
            check(few < size / 20, "few changed is small", size);
        }
    }

    // Alternating changed and unchanged words (runs split at every gap).
    b = a;
    for (size_t i = 0; i < size; i += 8)
    {
        b[i] = static_cast<uint8_t>(b[i] ^ 0xFF);
    }
    round_trip(&a, &b, "alternating");

    // A chain: several steps forward, then back with the same deltas.
    std::vector<std::vector<uint8_t>> states(6, a);
    std::vector<std::vector<uint8_t>> deltas(5);
    std::vector<size_t> lengths(5);
    for (size_t step = 1; step < states.size(); step++)
    {
        states[step] = states[step - 1];
        for (int i = 0; i < 50 && size > 0; i++)
        {
            size_t at = next_random() % size;
            states[step][at] = static_cast<uint8_t>(next_random());
        }
        deltas[step - 1].resize(state_delta_max_size(size));
        lengths[step - 1] = state_delta_encode(states[step - 1].data(), states[step].data(), size, deltas[step - 1].data());
    }
    std::vector<uint8_t> walk = states[5];
    for (size_t step = 5; step > 0; step--)
    {
        check(state_delta_apply(walk.data(), size, deltas[step - 1].data(), lengths[step - 1]), "chain back", size);
    }
    check(walk == states[0], "chain back to the start", size);
    for (size_t step = 0; step < 5; step++)
    {
        check(state_delta_apply(walk.data(), size, deltas[step].data(), lengths[step]), "chain forward", size);
    }
    check(walk == states[5], "chain forward to the end", size);
}

static void check_malformed()
{
    std::vector<uint8_t> a;
    std::vector<uint8_t> b;
    fill_random(&a, 1000);
    fill_random(&b, 1000);
    std::vector<uint8_t> delta(state_delta_max_size(1000));
    size_t length = state_delta_encode(a.data(), b.data(), 1000, delta.data());
    std::vector<uint8_t> state = a;

    // Truncated: refused.
    check(!state_delta_apply(state.data(), 1000, delta.data(), length - 1), "truncated refused", 1000);
    // For a smaller state: refused.
    state = a;
    check(!state_delta_apply(state.data(), 996, delta.data(), length), "wrong size refused", 996);
    // Counts past the end of the state: refused.
    uint8_t too_many[] = {0x00, 0xFF, 0x7F};
    state = a;
    check(!state_delta_apply(state.data(), 1000, too_many, sizeof(too_many)), "count past the end refused", 1000);
    // An endless varint: refused.
    uint8_t endless[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01};
    check(!state_delta_apply(state.data(), 1000, endless, sizeof(endless)), "endless varint refused", 1000);
    // Empty delta for a non-empty state: refused (it doesn't cover the state).
    check(!state_delta_apply(state.data(), 1000, delta.data(), 0), "empty delta refused", 1000);
}

int main()
{
    size_t sizes[] = {0, 1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 63, 64, 65, 1000, 1001, 1002, 1003, 4096, 65537, 300001};
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
    {
        check_size(sizes[i]);
    }
    check_malformed();
    if (g_failures == 0)
    {
        std::printf("state delta: all checks passed\n");
        return 0;
    }
    return 1;
}
