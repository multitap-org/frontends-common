#pragma once

// rewind_buffer: the history behind the rewind DVR. A ring of records in one
// arena reserved once (nothing allocates while recording):
//
//   state   the XOR delta (state_delta.h) between this state and the previous
//           state record. Every frame (SCRUB) or every few seconds (SNAPSHOTS).
//   input   the XOR delta between this frame's input and the previous one
//           (any fixed-size input struct; replay between sparse states).
//   event   bytes the caller defines (an option changed, a disk swapped...),
//           stored as they are; the buffer only keeps them in order.
//
// The newest state and the newest input are kept whole. XOR deltas work both
// ways, so a cursor walks back from the newest (undoing each record) and
// forward again (applying it): no keyframes. The oldest record is always a
// state record (the cursor's floor); its own delta points at a state that was
// dropped and is never applied.
//
// Old records are dropped when the arena is full, when the record index is
// full, or when the frames kept exceed max_frames. truncate() drops what's
// after a cursor ("play from here").
//
// One thread at a time: the recording worker while recording, the DVR while
// it's open (the caller drains the worker first).

#include <cstddef>
#include <cstdint>

enum class RewindRecordKind : uint8_t
{
    state,
    input,
    event,
};

struct RewindRecord
{
    uint64_t         frame  = 0;   // the caller's frame number
    size_t           offset = 0;   // in the arena
    uint32_t         size   = 0;   // bytes in the arena (always >= 1)
    RewindRecordKind kind   = RewindRecordKind::state;
    uint8_t          event  = 0;   // the caller's event type (kind event)
};

struct RewindBufferConfig
{
    size_t   state_size  = 0;   // bytes of one state (fixed: a new size needs a new buffer)
    size_t   input_size  = 0;   // bytes of one frame's input (0: no input records)
    size_t   arena_bytes = 0;   // the memory for records (MEMORY setting)
    uint32_t max_records = 0;   // the index's size
    uint64_t max_frames  = 0;   // newest frame - oldest frame kept (0: no limit)
};

struct RewindBuffer
{
    RewindBufferConfig config = {};

    uint8_t*      arena   = nullptr;
    RewindRecord* records = nullptr;   // ring of config.max_records
    uint32_t      first   = 0;         // index of the oldest record
    uint32_t      count   = 0;
    uint32_t      states  = 0;         // state records among them

    // The arena as a ring: live bytes are [tail, head) or, once wrapped,
    // [tail, end) + [0, head).
    size_t head    = 0;
    size_t tail    = 0;
    bool   wrapped = false;

    uint8_t* newest_state = nullptr;   // state_size: as of the newest state record
    bool     has_state    = false;     // a state was pushed since create / clear
    uint8_t* newest_input = nullptr;   // input_size: as of the newest input record (zeros before)
    uint8_t* scratch      = nullptr;   // a delta being encoded

    uint64_t dropped = 0;              // records dropped to make room (statistics)
};

// A position in the history: the state and input as of record `position`
// (0 = oldest). Owns its copies; created for one buffer's sizes.
struct RewindCursor
{
    uint32_t position = 0;
    uint8_t* state    = nullptr;   // state_size
    uint8_t* input    = nullptr;   // input_size (nullptr when 0)
    size_t   state_size = 0;
    size_t   input_size = 0;
};

// False (logged) if the config can't work: no state size, an arena smaller
// than one worst-case delta, no records, or out of memory.
bool rewind_buffer_create(RewindBuffer* buffer, const RewindBufferConfig* config);
void rewind_buffer_destroy(RewindBuffer* buffer);
// Forgets every record and the newest state / input (a new game, a reset of the history).
void rewind_buffer_clear(RewindBuffer* buffer);

// Adds a state. `*state` (state_size bytes, allocated with new uint8_t[]) is
// taken as the newest state and swapped with the buffer's previous one, so no
// state is copied: the caller gets a buffer back to serialize the next one
// into. False only if one delta doesn't fit the arena.
bool rewind_buffer_push_state(RewindBuffer* buffer, uint8_t** state, uint64_t frame);
// Adds a frame's input (input_size bytes, copied). Ignored before the first state.
bool rewind_buffer_push_input(RewindBuffer* buffer, const uint8_t* input, uint64_t frame);
// Adds an event: `type` plus `size` bytes of data (may be 0). Ignored before the first state.
bool rewind_buffer_push_event(RewindBuffer* buffer, uint8_t type, const uint8_t* data, uint32_t size, uint64_t frame);

// Record `index` (0 = oldest, count - 1 = newest) and its bytes. For an event
// the bytes are the data (without the type).
const RewindRecord* rewind_buffer_record(const RewindBuffer* buffer, uint32_t index);
const uint8_t*      rewind_buffer_event_data(const RewindBuffer* buffer, uint32_t index, uint32_t* size);
// Bytes of the arena in use (records only, without a wrapped gap).
size_t rewind_buffer_bytes_used(const RewindBuffer* buffer);

bool rewind_cursor_create(RewindCursor* cursor, const RewindBuffer* buffer);
void rewind_cursor_destroy(RewindCursor* cursor);
// At the newest record, with the newest state and input. False if the buffer is empty.
bool rewind_cursor_to_newest(const RewindBuffer* buffer, RewindCursor* cursor);
// One record back / forward. False at the oldest / newest record, or if a
// delta doesn't apply (a damaged buffer).
bool rewind_cursor_back(const RewindBuffer* buffer, RewindCursor* cursor);
bool rewind_cursor_forward(const RewindBuffer* buffer, RewindCursor* cursor);
// Back / forward to the previous / next state record (inputs and events on
// the way are walked too). False if there's none.
bool rewind_cursor_back_to_state(const RewindBuffer* buffer, RewindCursor* cursor);
bool rewind_cursor_forward_to_state(const RewindBuffer* buffer, RewindCursor* cursor);

// "Play from here": drops every record after the cursor; the cursor's state
// and input become the newest. Recording continues from them.
void rewind_buffer_truncate(RewindBuffer* buffer, const RewindCursor* cursor);
