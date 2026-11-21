// Checks rewind_buffer.h against a plain copy of every state and input
// pushed: a cursor walked back from the newest record to the oldest and
// forward again sees exactly what was pushed at each record, through arena
// wraps and drops (memory, record count, max_frames); the oldest record is
// always a state; truncate() then more pushes keep it right; events keep
// their data; sparse states (snapshots) with an input every frame work.
//
// Exit code 0 = all checks passed. Each failure is printed.

#include <cstdio>
#include <cstring>
#include <vector>

#include "rewind_buffer.h"
#include "state_delta.h"

static int g_failures = 0;

static void check(bool condition, const char* what)
{
    if (!condition)
    {
        std::printf("FAIL %s\n", what);
        g_failures++;
    }
}

static uint32_t g_random = 4242;

static uint32_t next_random()
{
    g_random = g_random * 1103515245u + 12345u;
    return g_random >> 8;
}

constexpr size_t   STATE_SIZE = 1003;   // not a multiple of 4
constexpr size_t   INPUT_SIZE = 12;
constexpr uint32_t FRAMES     = 3000;

// What a game would do: a state that changes a little each frame (sometimes a
// lot), an input that changes now and then.
struct Truth
{
    std::vector<std::vector<uint8_t>> states;   // by frame
    std::vector<std::vector<uint8_t>> inputs;   // by frame
};

static void make_truth(Truth* truth)
{
    std::vector<uint8_t> state(STATE_SIZE);
    std::vector<uint8_t> input(INPUT_SIZE, 0);
    for (size_t i = 0; i < STATE_SIZE; i++)
    {
        state[i] = static_cast<uint8_t>(next_random());
    }
    for (uint32_t frame = 0; frame < FRAMES; frame++)
    {
        uint32_t changes = (next_random() % 50 == 0) ? 600 : 1 + next_random() % 20;
        for (uint32_t c = 0; c < changes; c++)
        {
            state[next_random() % STATE_SIZE] = static_cast<uint8_t>(next_random());
        }
        if (next_random() % 8 == 0)
        {
            input[next_random() % INPUT_SIZE] = static_cast<uint8_t>(next_random());
        }
        truth->states.push_back(state);
        truth->inputs.push_back(input);
    }
}

// The cursor's state / input match what was pushed as of its record.
static bool cursor_matches(const RewindBuffer* buffer, const RewindCursor* cursor, const Truth* truth, bool inputs)
{
    // The state is the one of the last state record at or before the cursor,
    // the input that of its frame (inputs are pushed every frame here).
    uint32_t index = cursor->position;
    while (rewind_buffer_record(buffer, index)->kind != RewindRecordKind::state)
    {
        index--;
    }
    uint64_t state_frame = rewind_buffer_record(buffer, index)->frame;
    if (std::memcmp(cursor->state, truth->states[state_frame].data(), STATE_SIZE) != 0)
    {
        return false;
    }
    if (inputs)
    {
        // The input as of the last input record at or before the cursor (or
        // before the oldest state: the input of that state's frame, since
        // inputs come before the state each frame).
        uint64_t input_frame = state_frame;
        for (uint32_t i = cursor->position + 1; i-- > 0;)
        {
            const RewindRecord* record = rewind_buffer_record(buffer, i);
            if (record->kind == RewindRecordKind::input)
            {
                input_frame = record->frame;
                break;
            }
        }
        if (std::memcmp(cursor->input, truth->inputs[input_frame].data(), INPUT_SIZE) != 0)
        {
            return false;
        }
    }
    return true;
}

// Walks the whole buffer back to the oldest record and forward again.
static void check_walk(const RewindBuffer* buffer, const Truth* truth, bool inputs, const char* what)
{
    check(buffer->count > 0, what);
    check(rewind_buffer_record(buffer, 0)->kind == RewindRecordKind::state, "the oldest record is a state");
    RewindCursor cursor;
    check(rewind_cursor_create(&cursor, buffer), "cursor created");
    check(rewind_cursor_to_newest(buffer, &cursor), what);
    bool good = cursor_matches(buffer, &cursor, truth, inputs);
    uint32_t steps = 0;
    while (rewind_cursor_back(buffer, &cursor))
    {
        good = good && cursor_matches(buffer, &cursor, truth, inputs);
        steps++;
    }
    check(cursor.position == 0 && steps == buffer->count - 1, what);
    while (rewind_cursor_forward(buffer, &cursor))
    {
        good = good && cursor_matches(buffer, &cursor, truth, inputs);
    }
    check(cursor.position == buffer->count - 1, what);
    check(good, what);
    rewind_cursor_destroy(&cursor);
}

// Every frame a state and an input (SCRUB), in an arena that wraps many times.
static void check_scrub(const Truth* truth)
{
    RewindBufferConfig config;
    config.state_size  = STATE_SIZE;
    config.input_size  = INPUT_SIZE;
    config.arena_bytes = 20000;
    config.max_records = 100000;
    RewindBuffer buffer;
    check(rewind_buffer_create(&buffer, &config), "scrub: created");
    uint8_t* slot = new uint8_t[STATE_SIZE];
    for (uint32_t frame = 0; frame < FRAMES; frame++)
    {
        // Before the first state, inputs are ignored.
        bool pushed = rewind_buffer_push_input(&buffer, truth->inputs[frame].data(), frame);
        check(pushed == (frame > 0), "scrub: inputs only after the first state");
        std::memcpy(slot, truth->states[frame].data(), STATE_SIZE);
        check(rewind_buffer_push_state(&buffer, &slot, frame), "scrub: push");
        check(rewind_buffer_bytes_used(&buffer) <= config.arena_bytes, "scrub: within the arena");
        if (frame % 500 == 499)
        {
            check_walk(&buffer, truth, true, "scrub: walk");
        }
    }
    check(buffer.dropped > 0, "scrub: old records dropped");
    check(rewind_buffer_record(&buffer, buffer.count - 1)->frame == FRAMES - 1, "scrub: newest kept");

    // Play from the middle: drop the future, then record a different one.
    RewindCursor cursor;
    rewind_cursor_create(&cursor, &buffer);
    rewind_cursor_to_newest(&buffer, &cursor);
    for (int i = 0; i < 40; i++)
    {
        rewind_cursor_back_to_state(&buffer, &cursor);
    }
    uint64_t from = rewind_buffer_record(&buffer, cursor.position)->frame;
    check(std::memcmp(cursor.state, truth->states[from].data(), STATE_SIZE) == 0, "scrub: back to a state");
    rewind_buffer_truncate(&buffer, &cursor);
    check(rewind_buffer_record(&buffer, buffer.count - 1)->frame == from, "scrub: truncated");
    rewind_cursor_destroy(&cursor);

    // The new future: the old frames again, renumbered after `from`, so a walk
    // checks against "truth" shifted. Simplest: push truth frames from + 1 on
    // again (the same data the cut future had): the walk must still match.
    for (uint64_t frame = from + 1; frame < FRAMES; frame++)
    {
        rewind_buffer_push_input(&buffer, truth->inputs[frame].data(), frame);
        std::memcpy(slot, truth->states[frame].data(), STATE_SIZE);
        rewind_buffer_push_state(&buffer, &slot, frame);
    }
    check_walk(&buffer, truth, true, "scrub: walk after truncate");

    delete[] slot;
    rewind_buffer_destroy(&buffer);
}

// A state every 60 frames, an input every frame, a few events (SNAPSHOTS),
// with a frame limit.
static void check_snapshots(const Truth* truth)
{
    RewindBufferConfig config;
    config.state_size  = STATE_SIZE;
    config.input_size  = INPUT_SIZE;
    config.arena_bytes = 1 << 20;
    config.max_records = 100000;
    config.max_frames  = 600;
    RewindBuffer buffer;
    check(rewind_buffer_create(&buffer, &config), "snapshots: created");
    uint8_t* slot = new uint8_t[STATE_SIZE];
    for (uint32_t frame = 0; frame < FRAMES; frame++)
    {
        rewind_buffer_push_input(&buffer, truth->inputs[frame].data(), frame);
        if (frame % 60 == 0)
        {
            std::memcpy(slot, truth->states[frame].data(), STATE_SIZE);
            check(rewind_buffer_push_state(&buffer, &slot, frame), "snapshots: push");
        }
        if (frame % 250 == 7)
        {
            uint8_t data[3] = {static_cast<uint8_t>(frame), 1, 2};
            check(rewind_buffer_push_event(&buffer, 9, data, sizeof(data), frame), "snapshots: event");
        }
    }
    uint64_t span = rewind_buffer_record(&buffer, buffer.count - 1)->frame - rewind_buffer_record(&buffer, 0)->frame;
    check(span <= config.max_frames, "snapshots: frame limit");
    check(buffer.states >= 10, "snapshots: about ten states kept");
    check_walk(&buffer, truth, true, "snapshots: walk");

    // Events come back with their type and data.
    bool seen = false;
    for (uint32_t i = 0; i < buffer.count; i++)
    {
        const RewindRecord* record = rewind_buffer_record(&buffer, i);
        if (record->kind == RewindRecordKind::event)
        {
            uint32_t size = 0;
            const uint8_t* data = rewind_buffer_event_data(&buffer, i, &size);
            check(record->event == 9 && size == 3 && data[0] == static_cast<uint8_t>(record->frame) && data[2] == 2,
                  "snapshots: event data");
            seen = true;
        }
    }
    check(seen, "snapshots: an event kept");

    // Replay a window: from a state, the inputs of each frame up to the next state.
    RewindCursor cursor;
    rewind_cursor_create(&cursor, &buffer);
    rewind_cursor_to_newest(&buffer, &cursor);
    rewind_cursor_back_to_state(&buffer, &cursor);
    rewind_cursor_back_to_state(&buffer, &cursor);
    uint64_t start = rewind_buffer_record(&buffer, cursor.position)->frame;
    uint32_t inputs_seen = 0;
    while (rewind_cursor_forward(&buffer, &cursor))
    {
        const RewindRecord* record = rewind_buffer_record(&buffer, cursor.position);
        if (record->kind == RewindRecordKind::state)
        {
            break;
        }
        if (record->kind == RewindRecordKind::input)
        {
            check(std::memcmp(cursor.input, truth->inputs[record->frame].data(), INPUT_SIZE) == 0,
                  "snapshots: replayed input");
            inputs_seen++;
        }
    }
    check(inputs_seen == 60, "snapshots: a window's inputs");
    check(std::memcmp(cursor.state, truth->states[start + 60].data(), STATE_SIZE) == 0, "snapshots: next state");
    rewind_cursor_destroy(&cursor);

    // Clear forgets everything.
    rewind_buffer_clear(&buffer);
    check(buffer.count == 0 && !rewind_buffer_push_input(&buffer, truth->inputs[0].data(), 0), "snapshots: cleared");
    delete[] slot;
    rewind_buffer_destroy(&buffer);
}

// A small record index: drops by count, not memory.
static void check_record_limit(const Truth* truth)
{
    RewindBufferConfig config;
    config.state_size  = STATE_SIZE;
    config.arena_bytes = 1 << 20;
    config.max_records = 50;
    RewindBuffer buffer;
    check(rewind_buffer_create(&buffer, &config), "records: created");
    uint8_t* slot = new uint8_t[STATE_SIZE];
    for (uint32_t frame = 0; frame < 500; frame++)
    {
        std::memcpy(slot, truth->states[frame].data(), STATE_SIZE);
        rewind_buffer_push_state(&buffer, &slot, frame);
    }
    check(buffer.count == 50 && rewind_buffer_record(&buffer, 0)->frame == 450, "records: the newest 50");
    check_walk(&buffer, truth, false, "records: walk");
    delete[] slot;
    rewind_buffer_destroy(&buffer);
}

static void check_bad_config()
{
    RewindBufferConfig config;
    config.state_size  = 1000;
    config.arena_bytes = 100;   // smaller than one worst-case delta
    config.max_records = 10;
    RewindBuffer buffer;
    check(!rewind_buffer_create(&buffer, &config), "an arena too small is refused");
    config.arena_bytes = state_delta_max_size(1000);
    config.state_size  = 0;
    check(!rewind_buffer_create(&buffer, &config), "no state size is refused");
}

int main()
{
    Truth truth;
    make_truth(&truth);
    check_scrub(&truth);
    check_snapshots(&truth);
    check_record_limit(&truth);
    check_bad_config();
    if (g_failures == 0)
    {
        std::printf("rewind buffer: all checks passed\n");
        return 0;
    }
    return 1;
}
