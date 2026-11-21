#include "rewind_buffer.h"

#include <cstring>
#include <new>

#include "host_log.h"
#include "state_delta.h"

// The record at ring position `index` from the oldest.
static RewindRecord* record_at(RewindBuffer* buffer, uint32_t index)
{
    return &buffer->records[(buffer->first + index) % buffer->config.max_records];
}

static const RewindRecord* record_at_const(const RewindBuffer* buffer, uint32_t index)
{
    return &buffer->records[(buffer->first + index) % buffer->config.max_records];
}

static void reset_ring(RewindBuffer* buffer)
{
    buffer->first   = 0;
    buffer->count   = 0;
    buffer->states  = 0;
    buffer->head    = 0;
    buffer->tail    = 0;
    buffer->wrapped = false;
}

// Drops the oldest record, then any non-state records that would be left at
// the front: the oldest record is always a state (the cursor's floor).
static void drop_oldest(RewindBuffer* buffer)
{
    bool first_one = true;
    while (buffer->count > 0 && (first_one || record_at(buffer, 0)->kind != RewindRecordKind::state))
    {
        first_one = false;
        if (record_at(buffer, 0)->kind == RewindRecordKind::state)
        {
            buffer->states--;
        }
        buffer->first = (buffer->first + 1) % buffer->config.max_records;
        buffer->count--;
        buffer->dropped++;
        if (buffer->count == 0)
        {
            reset_ring(buffer);
            return;
        }
        // Once the records left are all in the wrapped part (they start
        // below the old tail), the ring isn't wrapped any more.
        size_t offset = record_at(buffer, 0)->offset;
        if (buffer->wrapped && offset < buffer->tail)
        {
            buffer->wrapped = false;
        }
        buffer->tail = offset;
    }
}

// Room for `size` bytes (>= 1) and one more index entry, dropping the oldest
// records as needed. Returns the offset, or false if it can't fit at all.
static bool reserve(RewindBuffer* buffer, size_t size, size_t* offset)
{
    if (size > buffer->config.arena_bytes)
    {
        return false;
    }
    while (true)
    {
        if (buffer->count == buffer->config.max_records)
        {
            drop_oldest(buffer);
            continue;
        }
        if (buffer->count == 0)
        {
            reset_ring(buffer);
        }
        if (!buffer->wrapped)
        {
            if (buffer->config.arena_bytes - buffer->head >= size)
            {
                *offset = buffer->head;
                buffer->head += size;
                return true;
            }
            // Not enough at the end: wrap to the start if the oldest record
            // leaves room there (the end's gap is wasted until it wraps back).
            if (buffer->count > 0 && buffer->tail >= size)
            {
                buffer->wrapped = true;
                *offset = 0;
                buffer->head = size;
                return true;
            }
        }
        else if (buffer->tail - buffer->head >= size)
        {
            *offset = buffer->head;
            buffer->head += size;
            return true;
        }
        drop_oldest(buffer);
    }
}

// Writes a record of `size` bytes from `data` (and `extra` before it, for an
// event's type) at the newest end.
static bool add_record(RewindBuffer* buffer, RewindRecordKind kind, uint8_t event, const uint8_t* data, size_t size,
                       bool with_type, uint64_t frame)
{
    size_t total = size + (with_type ? 1 : 0);
    size_t offset = 0;
    if (!reserve(buffer, total, &offset))
    {
        host_log(LogLevel::warning, "rewind: a record of %zu bytes doesn't fit the history (%zu bytes)", total,
                 buffer->config.arena_bytes);
        return false;
    }
    uint8_t* at = buffer->arena + offset;
    if (with_type)
    {
        at[0] = event;
        at++;
    }
    if (size > 0)
    {
        std::memcpy(at, data, size);
    }
    RewindRecord* record = record_at(buffer, buffer->count);
    record->frame  = frame;
    record->offset = offset;
    record->size   = static_cast<uint32_t>(total);
    record->kind   = kind;
    record->event  = event;
    buffer->count++;
    if (kind == RewindRecordKind::state)
    {
        buffer->states++;
    }
    if (buffer->count == 1)
    {
        buffer->tail = offset;
    }

    // Too long a history: drop from the oldest, but never the last state.
    if (buffer->config.max_frames > 0)
    {
        while (buffer->states > 1 && frame - record_at(buffer, 0)->frame > buffer->config.max_frames)
        {
            drop_oldest(buffer);
        }
    }
    return true;
}

bool rewind_buffer_create(RewindBuffer* buffer, const RewindBufferConfig* config)
{
    size_t largest = config->state_size > config->input_size ? config->state_size : config->input_size;
    if (config->state_size == 0 || config->max_records < 2 ||
        config->arena_bytes < state_delta_max_size(config->state_size))
    {
        host_log(LogLevel::error, "rewind: unusable history (state %zu bytes, arena %zu bytes, %u records)",
                 config->state_size, config->arena_bytes, config->max_records);
        return false;
    }
    buffer->config       = *config;
    buffer->arena        = new (std::nothrow) uint8_t[config->arena_bytes];
    buffer->records      = new (std::nothrow) RewindRecord[config->max_records];
    buffer->newest_state = new (std::nothrow) uint8_t[config->state_size];
    buffer->newest_input = config->input_size > 0 ? new (std::nothrow) uint8_t[config->input_size] : nullptr;
    buffer->scratch      = new (std::nothrow) uint8_t[state_delta_max_size(largest)];
    if (buffer->arena == nullptr || buffer->records == nullptr || buffer->newest_state == nullptr ||
        (config->input_size > 0 && buffer->newest_input == nullptr) || buffer->scratch == nullptr)
    {
        host_log(LogLevel::error, "rewind: out of memory for a %zu bytes history", config->arena_bytes);
        rewind_buffer_destroy(buffer);
        return false;
    }
    rewind_buffer_clear(buffer);
    return true;
}

void rewind_buffer_destroy(RewindBuffer* buffer)
{
    delete[] buffer->arena;
    delete[] buffer->records;
    delete[] buffer->newest_state;
    delete[] buffer->newest_input;
    delete[] buffer->scratch;
    *buffer = RewindBuffer{};
}

void rewind_buffer_clear(RewindBuffer* buffer)
{
    reset_ring(buffer);
    buffer->has_state = false;
    if (buffer->newest_input != nullptr)
    {
        std::memset(buffer->newest_input, 0, buffer->config.input_size);
    }
}

bool rewind_buffer_push_state(RewindBuffer* buffer, uint8_t** state, uint64_t frame)
{
    size_t size = buffer->config.state_size;
    size_t length = 0;
    if (buffer->has_state)
    {
        length = state_delta_encode(buffer->newest_state, *state, size, buffer->scratch);
    }
    else
    {
        // The first state: nothing to take a delta from. Its record is the
        // oldest, whose delta is never applied: one placeholder byte.
        buffer->scratch[0] = 0;
        length = 1;
    }
    if (!add_record(buffer, RewindRecordKind::state, 0, buffer->scratch, length, false, frame))
    {
        return false;
    }
    uint8_t* previous = buffer->newest_state;
    buffer->newest_state = *state;
    *state = previous;
    buffer->has_state = true;
    return true;
}

bool rewind_buffer_push_input(RewindBuffer* buffer, const uint8_t* input, uint64_t frame)
{
    if (!buffer->has_state || buffer->config.input_size == 0)
    {
        return false;
    }
    size_t length = state_delta_encode(buffer->newest_input, input, buffer->config.input_size, buffer->scratch);
    if (!add_record(buffer, RewindRecordKind::input, 0, buffer->scratch, length, false, frame))
    {
        return false;
    }
    std::memcpy(buffer->newest_input, input, buffer->config.input_size);
    return true;
}

bool rewind_buffer_push_event(RewindBuffer* buffer, uint8_t type, const uint8_t* data, uint32_t size, uint64_t frame)
{
    if (!buffer->has_state)
    {
        return false;
    }
    return add_record(buffer, RewindRecordKind::event, type, data, size, true, frame);
}

const RewindRecord* rewind_buffer_record(const RewindBuffer* buffer, uint32_t index)
{
    if (index >= buffer->count)
    {
        return nullptr;
    }
    return record_at_const(buffer, index);
}

const uint8_t* rewind_buffer_event_data(const RewindBuffer* buffer, uint32_t index, uint32_t* size)
{
    const RewindRecord* record = rewind_buffer_record(buffer, index);
    if (record == nullptr || record->kind != RewindRecordKind::event)
    {
        *size = 0;
        return nullptr;
    }
    *size = record->size - 1;
    return buffer->arena + record->offset + 1;
}

size_t rewind_buffer_bytes_used(const RewindBuffer* buffer)
{
    size_t bytes = 0;
    for (uint32_t i = 0; i < buffer->count; i++)
    {
        bytes += record_at_const(buffer, i)->size;
    }
    return bytes;
}

bool rewind_cursor_create(RewindCursor* cursor, const RewindBuffer* buffer)
{
    cursor->state_size = buffer->config.state_size;
    cursor->input_size = buffer->config.input_size;
    cursor->state = new (std::nothrow) uint8_t[cursor->state_size];
    cursor->input = cursor->input_size > 0 ? new (std::nothrow) uint8_t[cursor->input_size] : nullptr;
    if (cursor->state == nullptr || (cursor->input_size > 0 && cursor->input == nullptr))
    {
        rewind_cursor_destroy(cursor);
        return false;
    }
    return true;
}

void rewind_cursor_destroy(RewindCursor* cursor)
{
    delete[] cursor->state;
    delete[] cursor->input;
    *cursor = RewindCursor{};
}

bool rewind_cursor_to_newest(const RewindBuffer* buffer, RewindCursor* cursor)
{
    if (buffer->count == 0)
    {
        return false;
    }
    cursor->position = buffer->count - 1;
    std::memcpy(cursor->state, buffer->newest_state, cursor->state_size);
    if (cursor->input_size > 0)
    {
        std::memcpy(cursor->input, buffer->newest_input, cursor->input_size);
    }
    return true;
}

// XORs record `index`'s delta into the cursor (a state or an input record;
// events change nothing). The same call undoes and redoes it.
static bool toggle(const RewindBuffer* buffer, RewindCursor* cursor, uint32_t index)
{
    const RewindRecord* record = record_at_const(buffer, index);
    const uint8_t* delta = buffer->arena + record->offset;
    if (record->kind == RewindRecordKind::state)
    {
        return state_delta_apply(cursor->state, cursor->state_size, delta, record->size);
    }
    if (record->kind == RewindRecordKind::input)
    {
        return state_delta_apply(cursor->input, cursor->input_size, delta, record->size);
    }
    return true;
}

bool rewind_cursor_back(const RewindBuffer* buffer, RewindCursor* cursor)
{
    // Position 0 is the oldest state: its delta points at a dropped state.
    if (cursor->position == 0 || cursor->position >= buffer->count)
    {
        return false;
    }
    if (!toggle(buffer, cursor, cursor->position))
    {
        return false;
    }
    cursor->position--;
    return true;
}

bool rewind_cursor_forward(const RewindBuffer* buffer, RewindCursor* cursor)
{
    if (cursor->position + 1 >= buffer->count)
    {
        return false;
    }
    if (!toggle(buffer, cursor, cursor->position + 1))
    {
        return false;
    }
    cursor->position++;
    return true;
}

bool rewind_cursor_back_to_state(const RewindBuffer* buffer, RewindCursor* cursor)
{
    uint32_t start = cursor->position;
    // Is there a state record before the cursor at all? (Position 0 always is one.)
    if (start == 0)
    {
        return false;
    }
    while (rewind_cursor_back(buffer, cursor))
    {
        if (record_at_const(buffer, cursor->position)->kind == RewindRecordKind::state)
        {
            return true;
        }
    }
    return false;
}

bool rewind_cursor_forward_to_state(const RewindBuffer* buffer, RewindCursor* cursor)
{
    // Only move if a state lies ahead: otherwise the cursor stays put.
    bool ahead = false;
    for (uint32_t i = cursor->position + 1; i < buffer->count; i++)
    {
        if (record_at_const(buffer, i)->kind == RewindRecordKind::state)
        {
            ahead = true;
            break;
        }
    }
    if (!ahead)
    {
        return false;
    }
    while (rewind_cursor_forward(buffer, cursor))
    {
        if (record_at_const(buffer, cursor->position)->kind == RewindRecordKind::state)
        {
            return true;
        }
    }
    return false;
}

void rewind_buffer_truncate(RewindBuffer* buffer, const RewindCursor* cursor)
{
    if (buffer->count == 0 || cursor->position >= buffer->count)
    {
        return;
    }
    uint32_t keep = cursor->position + 1;
    for (uint32_t i = keep; i < buffer->count; i++)
    {
        if (record_at(buffer, i)->kind == RewindRecordKind::state)
        {
            buffer->states--;
        }
    }
    buffer->count = keep;
    // The arena's newest end moves back to the end of the last record kept.
    // Records are at least one byte, so one in the wrapped part starts below
    // the tail and one in the upper part starts at or above it.
    const RewindRecord* last = record_at(buffer, keep - 1);
    if (buffer->wrapped && last->offset >= buffer->tail)
    {
        buffer->wrapped = false;
    }
    buffer->head = last->offset + last->size;
    std::memcpy(buffer->newest_state, cursor->state, buffer->config.state_size);
    if (buffer->config.input_size > 0)
    {
        std::memcpy(buffer->newest_input, cursor->input, buffer->config.input_size);
    }
}
