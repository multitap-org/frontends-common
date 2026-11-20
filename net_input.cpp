#include "net_input.h"

static void write_u32(uint8_t* out, uint32_t value)
{
    for (uint32_t i = 0; i < 4; i++)
    {
        out[i] = static_cast<uint8_t>((value >> (8 * i)) & 0xFF);
    }
}

static uint32_t read_u32(const uint8_t* data)
{
    return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
           (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
}

static void write_i16(uint8_t* out, int16_t value)
{
    uint16_t bits = static_cast<uint16_t>(value);
    out[0] = static_cast<uint8_t>(bits & 0xFF);
    out[1] = static_cast<uint8_t>(bits >> 8);
}

static int16_t read_i16(const uint8_t* data)
{
    return static_cast<int16_t>(static_cast<uint16_t>(data[0] | (data[1] << 8)));
}

size_t net_input_push(NetPadSender* sender, uint8_t pad, const NetPadState* state, uint8_t* out, size_t capacity)
{
    // Newest first: move the history down one.
    for (uint32_t i = NET_INPUT_HISTORY - 1; i > 0; i--)
    {
        sender->history[i] = sender->history[i - 1];
    }
    sender->history[0] = *state;
    sender->history[0].frame = sender->next_frame;
    sender->next_frame++;
    if (sender->count < NET_INPUT_HISTORY)
    {
        sender->count++;
    }

    size_t size = 3 + sender->count * NET_INPUT_STATE_SIZE;
    if (capacity < size)
    {
        return 0;
    }
    out[0] = NET_INPUT_TYPE_PAD;
    out[1] = pad;
    out[2] = static_cast<uint8_t>(sender->count);
    uint8_t* at = out + 3;
    for (uint32_t i = 0; i < sender->count; i++)
    {
        const NetPadState* entry = &sender->history[i];
        write_u32(at, entry->frame);
        write_u32(at + 4, entry->buttons);
        for (uint32_t a = 0; a < NET_INPUT_AXES; a++)
        {
            write_i16(at + 8 + 2 * a, entry->axes[a]);
        }
        at += NET_INPUT_STATE_SIZE;
    }
    return size;
}

bool net_input_read(const uint8_t* data, size_t size, uint8_t* pad, NetPadState* states, uint32_t* count)
{
    if (size < 3 || data[0] != NET_INPUT_TYPE_PAD || data[1] >= NET_INPUT_MAX_PADS || data[2] == 0 ||
        data[2] > NET_INPUT_HISTORY || size != 3 + data[2] * NET_INPUT_STATE_SIZE)
    {
        return false;
    }
    *pad = data[1];
    *count = data[2];
    // The packet is newest first; hand them out oldest first.
    for (uint32_t i = 0; i < *count; i++)
    {
        const uint8_t* at = data + 3 + (*count - 1 - i) * NET_INPUT_STATE_SIZE;
        NetPadState* state = &states[i];
        state->frame = read_u32(at);
        state->buttons = read_u32(at + 4);
        for (uint32_t a = 0; a < NET_INPUT_AXES; a++)
        {
            state->axes[a] = read_i16(at + 8 + 2 * a);
        }
    }
    return true;
}

static void write_u16(uint8_t* out, uint16_t value)
{
    out[0] = static_cast<uint8_t>(value & 0xFF);
    out[1] = static_cast<uint8_t>(value >> 8);
}

static uint16_t read_u16(const uint8_t* data)
{
    return static_cast<uint16_t>(data[0] | (data[1] << 8));
}

size_t net_input_push_retropad(NetRetroPadSender* sender, uint8_t pad, const NetRetroPadState* state, uint8_t* out,
                               size_t capacity)
{
    for (uint32_t i = NET_INPUT_HISTORY - 1; i > 0; i--)
    {
        sender->history[i] = sender->history[i - 1];
    }
    sender->history[0] = *state;
    sender->history[0].frame = sender->next_frame;
    sender->next_frame++;
    if (sender->count < NET_INPUT_HISTORY)
    {
        sender->count++;
    }
    size_t size = 3 + sender->count * NET_RETROPAD_STATE_SIZE;
    if (capacity < size)
    {
        return 0;
    }
    out[0] = NET_INPUT_TYPE_RETROPAD;
    out[1] = pad;
    out[2] = static_cast<uint8_t>(sender->count);
    uint8_t* at = out + 3;
    for (uint32_t i = 0; i < sender->count; i++)
    {
        const NetRetroPadState* entry = &sender->history[i];
        write_u32(at, entry->frame);
        write_u16(at + 4, entry->buttons);
        write_i16(at + 6, entry->analog[0][0]);
        write_i16(at + 8, entry->analog[0][1]);
        write_i16(at + 10, entry->analog[1][0]);
        write_i16(at + 12, entry->analog[1][1]);
        at += NET_RETROPAD_STATE_SIZE;
    }
    return size;
}

bool net_input_read_retropad(const uint8_t* data, size_t size, uint8_t* pad, NetRetroPadState* states,
                             uint32_t* count)
{
    if (size < 3 || data[0] != NET_INPUT_TYPE_RETROPAD || data[1] >= NET_INPUT_MAX_PADS || data[2] == 0 ||
        data[2] > NET_INPUT_HISTORY || size != 3 + data[2] * NET_RETROPAD_STATE_SIZE)
    {
        return false;
    }
    *pad = data[1];
    *count = data[2];
    for (uint32_t i = 0; i < *count; i++)
    {
        const uint8_t* at = data + 3 + (*count - 1 - i) * NET_RETROPAD_STATE_SIZE;
        NetRetroPadState* state = &states[i];
        state->frame = read_u32(at);
        state->buttons = read_u16(at + 4);
        state->analog[0][0] = read_i16(at + 6);
        state->analog[0][1] = read_i16(at + 8);
        state->analog[1][0] = read_i16(at + 10);
        state->analog[1][1] = read_i16(at + 12);
    }
    return true;
}
