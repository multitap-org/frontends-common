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
    buffer->lane    = RewindLane::a;
    buffer->kept    = 0;
    buffer->dead    = 0;
}

RewindLane rewind_lane_other(RewindLane lane)
{
    return lane == RewindLane::a ? RewindLane::b : RewindLane::a;
}

RewindLane rewind_buffer_kept_lane(const RewindBuffer* buffer)
{
    return rewind_lane_other(buffer->lane);
}

static bool lane_has(const RewindRecord* record, RewindLane lane)
{
    return record->lane == RewindLane::shared || record->lane == lane;
}

bool rewind_buffer_on_lane(const RewindBuffer* buffer, uint32_t index, RewindLane lane)
{
    return index < buffer->count && lane_has(record_at_const(buffer, index), lane);
}

// The recorded lane's state records (shared included): after a fork, which
// moves records between lanes.
static void count_states(RewindBuffer* buffer)
{
    buffer->states = 0;
    for (uint32_t i = 0; i < buffer->count; i++)
    {
        const RewindRecord* record = record_at(buffer, i);
        if (record->kind == RewindRecordKind::state && lane_has(record, buffer->lane))
        {
            buffer->states++;
        }
    }
}

// Removes the dead records, and the kept lane's too with `with_kept`: every
// record after the first one removed moves down (bytes and index entry) to
// close the gaps, in order. Packed the way reserve() packs them, each lands
// at or before where it was (counting a wrap as one more lap), so a memmove
// per record never overwrites one not moved yet. The oldest record is live
// (a state on the recorded lane, or shared), so something stays before them.
static void compact(RewindBuffer* buffer, bool with_kept)
{
    RewindLane kept_lane = rewind_buffer_kept_lane(buffer);
    uint32_t first_gone = buffer->count;
    for (uint32_t i = 1; i < buffer->count; i++)
    {
        RewindLane lane = record_at(buffer, i)->lane;
        if (lane == RewindLane::dead || (with_kept && lane == kept_lane))
        {
            first_gone = i;
            break;
        }
    }
    if (first_gone < buffer->count)
    {
        const RewindRecord* before = record_at(buffer, first_gone - 1);
        size_t   to    = before->offset + before->size;
        bool     lap   = buffer->wrapped && before->offset < buffer->tail;   // `to` is in the wrapped part
        uint32_t write = first_gone;
        for (uint32_t i = first_gone; i < buffer->count; i++)
        {
            RewindRecord record = *record_at(buffer, i);
            if (record.lane == RewindLane::dead || (with_kept && record.lane == kept_lane))
            {
                continue;
            }
            if (!lap && buffer->config.arena_bytes - to < record.size)
            {
                to  = 0;
                lap = true;
            }
            if (to != record.offset)
            {
                std::memmove(buffer->arena + to, buffer->arena + record.offset, record.size);
                record.offset = to;
            }
            to += record.size;
            *record_at(buffer, write) = record;
            write++;
        }
        buffer->count   = write;
        buffer->head    = to;
        buffer->wrapped = lap;
    }
    buffer->dead = 0;
    if (with_kept)
    {
        buffer->kept = 0;
    }
}

// Whether a shared record is left once the front one goes (dead ones skipped).
static bool shared_after_front(const RewindBuffer* buffer)
{
    for (uint32_t i = 1; i < buffer->count; i++)
    {
        RewindLane lane = record_at_const(buffer, i)->lane;
        if (lane != RewindLane::dead)
        {
            return lane == RewindLane::shared;
        }
    }
    return false;
}

// Drops the oldest record, then any records that would be left at the front
// that aren't a state on the recorded lane: the oldest record is always one
// (the cursor's floor). The kept lane hangs off the shared records: before
// the last of them goes, so does the kept lane.
static void drop_oldest(RewindBuffer* buffer)
{
    bool first_one = true;
    while (buffer->count > 0)
    {
        const RewindRecord* front = record_at(buffer, 0);
        bool live_state = front->kind == RewindRecordKind::state && lane_has(front, buffer->lane);
        if (!first_one && live_state)
        {
            break;
        }
        first_one = false;
        if (buffer->kept > 0 && !shared_after_front(buffer))
        {
            compact(buffer, true);
            front = record_at(buffer, 0);
            live_state = front->kind == RewindRecordKind::state && lane_has(front, buffer->lane);
        }
        if (live_state)
        {
            buffer->states--;
        }
        if (front->lane == RewindLane::dead)
        {
            buffer->dead--;
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

// Frees some room: dead records first, then the kept lane, and only then
// the oldest record (Andrés: the kept lane goes first when the buffer wraps).
static void make_room(RewindBuffer* buffer)
{
    if (buffer->dead > 0)
    {
        compact(buffer, false);
    }
    else if (buffer->kept > 0)
    {
        compact(buffer, true);
    }
    else
    {
        drop_oldest(buffer);
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
            make_room(buffer);
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
        make_room(buffer);
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
    record->lane   = buffer->lane;
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
    buffer->kept_state   = new (std::nothrow) uint8_t[config->state_size];
    buffer->kept_input   = config->input_size > 0 ? new (std::nothrow) uint8_t[config->input_size] : nullptr;
    if (buffer->arena == nullptr || buffer->records == nullptr || buffer->newest_state == nullptr ||
        (config->input_size > 0 && buffer->newest_input == nullptr) || buffer->scratch == nullptr ||
        buffer->kept_state == nullptr || (config->input_size > 0 && buffer->kept_input == nullptr))
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
    delete[] buffer->kept_state;
    delete[] buffer->kept_input;
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

// The previous / next record on `lane` from `index` (dead and the other
// lane's records skipped).
static bool previous_on_lane(const RewindBuffer* buffer, uint32_t index, RewindLane lane, uint32_t* found)
{
    for (uint32_t i = index; i > 0; i--)
    {
        if (lane_has(record_at_const(buffer, i - 1), lane))
        {
            *found = i - 1;
            return true;
        }
    }
    return false;
}

static bool next_on_lane(const RewindBuffer* buffer, uint32_t index, RewindLane lane, uint32_t* found)
{
    for (uint32_t i = index + 1; i < buffer->count; i++)
    {
        if (lane_has(record_at_const(buffer, i), lane))
        {
            *found = i;
            return true;
        }
    }
    return false;
}

bool rewind_buffer_lane_newest(const RewindBuffer* buffer, RewindLane lane, uint32_t* index)
{
    return previous_on_lane(buffer, buffer->count, lane, index);
}

bool rewind_buffer_fork_point(const RewindBuffer* buffer, uint32_t* index)
{
    if (buffer->kept == 0)
    {
        return false;
    }
    // The shared records are the oldest ones, all together.
    uint32_t last = 0;
    for (uint32_t i = 0; i < buffer->count; i++)
    {
        RewindLane lane = record_at_const(buffer, i)->lane;
        if (lane == RewindLane::shared)
        {
            last = i;
        }
        else if (lane != RewindLane::dead)
        {
            break;
        }
    }
    *index = last;
    return true;
}

bool rewind_cursor_to_newest(const RewindBuffer* buffer, RewindCursor* cursor)
{
    uint32_t newest = 0;
    if (!rewind_buffer_lane_newest(buffer, buffer->lane, &newest))
    {
        return false;
    }
    cursor->position = newest;
    cursor->lane     = buffer->lane;
    std::memcpy(cursor->state, buffer->newest_state, cursor->state_size);
    if (cursor->input_size > 0)
    {
        std::memcpy(cursor->input, buffer->newest_input, cursor->input_size);
    }
    return true;
}

bool rewind_cursor_to_kept_newest(const RewindBuffer* buffer, RewindCursor* cursor)
{
    RewindLane kept = rewind_buffer_kept_lane(buffer);
    uint32_t newest = 0;
    if (buffer->kept == 0 || !rewind_buffer_lane_newest(buffer, kept, &newest))
    {
        return false;
    }
    cursor->position = newest;
    cursor->lane     = kept;
    std::memcpy(cursor->state, buffer->kept_state, cursor->state_size);
    if (cursor->input_size > 0)
    {
        std::memcpy(cursor->input, buffer->kept_input, cursor->input_size);
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
    uint32_t previous = 0;
    if (cursor->position == 0 || cursor->position >= buffer->count ||
        !previous_on_lane(buffer, cursor->position, cursor->lane, &previous))
    {
        return false;
    }
    if (!toggle(buffer, cursor, cursor->position))
    {
        return false;
    }
    cursor->position = previous;
    return true;
}

bool rewind_cursor_forward(const RewindBuffer* buffer, RewindCursor* cursor)
{
    uint32_t next = 0;
    if (!next_on_lane(buffer, cursor->position, cursor->lane, &next))
    {
        return false;
    }
    if (!toggle(buffer, cursor, next))
    {
        return false;
    }
    cursor->position = next;
    return true;
}

bool rewind_cursor_back_to_state(const RewindBuffer* buffer, RewindCursor* cursor)
{
    // Is there a state record before the cursor at all? (Position 0 always is one.)
    if (cursor->position == 0)
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
    // Only move if a state lies ahead on the lane: otherwise the cursor stays put.
    bool ahead = false;
    for (uint32_t i = cursor->position + 1; i < buffer->count; i++)
    {
        const RewindRecord* record = record_at_const(buffer, i);
        if (record->kind == RewindRecordKind::state && lane_has(record, cursor->lane))
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

// Records on `lane` in (from, to], to count a walk's length.
static uint32_t records_between(const RewindBuffer* buffer, uint32_t from, uint32_t to, RewindLane lane)
{
    uint32_t n = 0;
    for (uint32_t i = from + 1; i <= to && i < buffer->count; i++)
    {
        if (lane_has(record_at_const(buffer, i), lane))
        {
            n++;
        }
    }
    return n;
}

// Walks the cursor along its lane to record `target` (on that lane).
static bool walk_to(const RewindBuffer* buffer, RewindCursor* cursor, uint32_t target)
{
    while (cursor->position > target)
    {
        if (!rewind_cursor_back(buffer, cursor))
        {
            return false;
        }
    }
    while (cursor->position < target)
    {
        if (!rewind_cursor_forward(buffer, cursor))
        {
            return false;
        }
    }
    return cursor->position == target;
}

bool rewind_cursor_switch_lane(const RewindBuffer* buffer, RewindCursor* cursor)
{
    uint32_t fork = 0;
    if (cursor->position >= buffer->count || !rewind_buffer_fork_point(buffer, &fork))
    {
        return false;
    }
    RewindLane other = rewind_lane_other(cursor->lane);
    if (record_at_const(buffer, cursor->position)->lane == RewindLane::shared)
    {
        cursor->lane = other;   // the same record on both lanes
        return true;
    }
    // The other lane's newest state at or before the cursor's frame (the
    // shared ones are on it: at worst the fork's).
    uint64_t frame  = record_at_const(buffer, cursor->position)->frame;
    uint32_t target = 0;
    for (uint32_t i = 0; i < buffer->count; i++)
    {
        const RewindRecord* record = record_at_const(buffer, i);
        if (record->kind == RewindRecordKind::state && lane_has(record, other) && record->frame <= frame)
        {
            target = i;
        }
    }
    uint32_t other_newest = 0;
    rewind_buffer_lane_newest(buffer, other, &other_newest);
    // Back to the fork and along the other lane, or down from its newest.
    uint32_t to_fork = records_between(buffer, fork, cursor->position, cursor->lane);
    uint32_t along   = target > fork ? records_between(buffer, fork, target, other)
                                     : records_between(buffer, target, fork, other);
    uint32_t down    = records_between(buffer, target, other_newest, other);
    if (to_fork + along <= down)
    {
        if (!walk_to(buffer, cursor, fork))
        {
            return false;
        }
        cursor->lane = other;
        return walk_to(buffer, cursor, target);
    }
    bool newest = other == buffer->lane ? rewind_cursor_to_newest(buffer, cursor)
                                        : rewind_cursor_to_kept_newest(buffer, cursor);
    return newest && walk_to(buffer, cursor, target);
}

void rewind_buffer_fork(RewindBuffer* buffer, const RewindCursor* cursor)
{
    if (buffer->count == 0 || cursor->position >= buffer->count)
    {
        return;
    }
    uint32_t   here     = cursor->position;
    RewindLane recorded = buffer->lane;
    RewindLane kept     = rewind_buffer_kept_lane(buffer);
    bool       shared   = record_at(buffer, here)->lane == RewindLane::shared;

    if (cursor->lane == recorded || shared)
    {
        // From the recorded lane (or before the fork): what it has after the
        // cursor becomes the kept lane, the old kept lane goes. A future
        // without a state (the inputs after the newest one) isn't worth a
        // lane: it goes, and the kept lane stays.
        bool had_kept  = buffer->kept > 0;
        bool abandoned = false;
        for (uint32_t i = here + 1; i < buffer->count && !abandoned; i++)
        {
            const RewindRecord* record = record_at(buffer, i);
            abandoned = record->kind == RewindRecordKind::state && lane_has(record, recorded);
        }
        for (uint32_t i = 0; i < buffer->count; i++)
        {
            RewindRecord* record = record_at(buffer, i);
            if (!abandoned)
            {
                if (i > here && record->lane == recorded)
                {
                    record->lane = RewindLane::dead;
                    buffer->dead++;
                }
                else if (i > here && record->lane == RewindLane::shared && had_kept)
                {
                    record->lane = kept;   // before the kept lane parted: now only its
                    buffer->kept++;
                }
                else if (i > here && record->lane == RewindLane::shared)
                {
                    record->lane = RewindLane::dead;   // no kept lane: shared is just the recorded one
                    buffer->dead++;
                }
            }
            else if (record->lane == kept)
            {
                record->lane = RewindLane::dead;
                buffer->dead++;
            }
            else if (i > here && lane_has(record, recorded))
            {
                record->lane = kept;
            }
            else if (i <= here && record->lane == recorded)
            {
                record->lane = RewindLane::shared;
            }
        }
        if (!abandoned)
        {
            std::memcpy(buffer->newest_state, cursor->state, buffer->config.state_size);
            if (buffer->config.input_size > 0)
            {
                std::memcpy(buffer->newest_input, cursor->input, buffer->config.input_size);
            }
            count_states(buffer);
            return;
        }
        buffer->kept = 0;
        for (uint32_t i = 0; i < buffer->count; i++)
        {
            buffer->kept += record_at(buffer, i)->lane == kept ? 1u : 0u;
        }
    }
    else
    {
        // From the kept lane: recording goes on there, the recorded lane is
        // kept, and what the kept lane had after the cursor goes.
        buffer->kept = 0;
        for (uint32_t i = 0; i < buffer->count; i++)
        {
            RewindRecord* record = record_at(buffer, i);
            if (i > here && record->lane == kept)
            {
                record->lane = RewindLane::dead;
                buffer->dead++;
            }
            else if (record->lane == recorded)
            {
                buffer->kept++;
            }
        }
        buffer->lane = kept;
    }
    // The newest of the lane left behind is the kept lane's newest; the
    // cursor's state and input are the recorded lane's from now on.
    std::memcpy(buffer->kept_state, buffer->newest_state, buffer->config.state_size);
    std::memcpy(buffer->newest_state, cursor->state, buffer->config.state_size);
    if (buffer->config.input_size > 0)
    {
        std::memcpy(buffer->kept_input, buffer->newest_input, buffer->config.input_size);
        std::memcpy(buffer->newest_input, cursor->input, buffer->config.input_size);
    }
    count_states(buffer);
}
