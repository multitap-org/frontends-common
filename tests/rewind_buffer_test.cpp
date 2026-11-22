// Checks rewind_buffer.h against a plain copy of every state and input
// pushed: a cursor walked back from the newest record to the oldest and
// forward again sees exactly what was pushed at each record, through arena
// wraps and drops (memory, record count, max_frames); the oldest record is
// always a state; fork() then more pushes keep it right; events keep
// their data; sparse states (snapshots) with an input every frame work.
// Lanes: after a fork each lane walks exactly its own history, switching
// lands on the other lane's state at the same frame, a fork from the kept
// lane swaps them, and room is made from dead records, then the kept lane,
// before the oldest record is dropped (through arena wraps).
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
    truth->states.clear();
    truth->inputs.clear();
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
    while (rewind_buffer_record(buffer, index)->kind != RewindRecordKind::state ||
           !rewind_buffer_on_lane(buffer, index, cursor->lane))
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
            if (record->kind == RewindRecordKind::input && rewind_buffer_on_lane(buffer, i, cursor->lane))
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

// Walks a lane (the recorded one, or the kept one) back to the oldest record
// and forward again.
static void check_lane_walk(const RewindBuffer* buffer, const Truth* truth, bool inputs, bool kept, const char* what)
{
    check(buffer->count > 0, what);
    check(rewind_buffer_record(buffer, 0)->kind == RewindRecordKind::state, "the oldest record is a state");
    RewindLane lane = kept ? rewind_buffer_kept_lane(buffer) : buffer->lane;
    uint32_t on_lane = 0;
    for (uint32_t i = 0; i < buffer->count; i++)
    {
        on_lane += rewind_buffer_on_lane(buffer, i, lane) ? 1u : 0u;
    }
    uint32_t newest = 0;
    rewind_buffer_lane_newest(buffer, lane, &newest);
    RewindCursor cursor;
    check(rewind_cursor_create(&cursor, buffer), "cursor created");
    check(kept ? rewind_cursor_to_kept_newest(buffer, &cursor) : rewind_cursor_to_newest(buffer, &cursor), what);
    bool good = cursor_matches(buffer, &cursor, truth, inputs);
    uint32_t steps = 0;
    while (rewind_cursor_back(buffer, &cursor))
    {
        good = good && cursor_matches(buffer, &cursor, truth, inputs);
        steps++;
    }
    check(cursor.position == 0 && steps == on_lane - 1, what);
    while (rewind_cursor_forward(buffer, &cursor))
    {
        good = good && cursor_matches(buffer, &cursor, truth, inputs);
    }
    check(cursor.position == newest, what);
    check(good, what);
    rewind_cursor_destroy(&cursor);
}

static void check_walk(const RewindBuffer* buffer, const Truth* truth, bool inputs, const char* what)
{
    check_lane_walk(buffer, truth, inputs, false, what);
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
    rewind_buffer_fork(&buffer, &cursor);
    uint32_t newest = 0;
    check(rewind_buffer_lane_newest(&buffer, buffer.lane, &newest) &&
              rewind_buffer_record(&buffer, newest)->frame == from && buffer.kept > 0,
          "scrub: forked, the future kept");
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
    check_walk(&buffer, truth, true, "scrub: walk after the fork");

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

// A truth that is `base` up to frame `from` and `other` after it (a game
// played differently from there).
static void splice(Truth* out, const Truth* base, const Truth* other, uint64_t from)
{
    out->states.clear();
    out->inputs.clear();
    for (uint32_t frame = 0; frame < FRAMES; frame++)
    {
        const Truth* source = frame <= from ? base : other;
        out->states.push_back(source->states[frame]);
        out->inputs.push_back(source->inputs[frame]);
    }
}

// SCRUB's way: an input and a state every frame, frames [first, last).
static void push_frames(RewindBuffer* buffer, uint8_t** slot, const Truth* truth, uint64_t first, uint64_t last)
{
    for (uint64_t frame = first; frame < last; frame++)
    {
        rewind_buffer_push_input(buffer, truth->inputs[frame].data(), frame);
        std::memcpy(*slot, truth->states[frame].data(), STATE_SIZE);
        rewind_buffer_push_state(buffer, slot, frame);
    }
}

// The cursor to the state record of `frame` on its lane (walking back).
static bool cursor_back_to(const RewindBuffer* buffer, RewindCursor* cursor, uint64_t frame)
{
    while (rewind_buffer_record(buffer, cursor->position)->kind != RewindRecordKind::state ||
           rewind_buffer_record(buffer, cursor->position)->frame > frame)
    {
        if (!rewind_cursor_back(buffer, cursor))
        {
            return false;
        }
    }
    return rewind_buffer_record(buffer, cursor->position)->frame == frame;
}

static bool state_is(const RewindCursor* cursor, const Truth* truth, uint64_t frame)
{
    return std::memcmp(cursor->state, truth->states[frame].data(), STATE_SIZE) == 0;
}

// Fork, switch, fork from the kept lane, without memory pressure.
static void check_lanes(const Truth* a, const Truth* other)
{
    Truth b;
    splice(&b, a, other, 600);
    RewindBufferConfig config;
    config.state_size  = STATE_SIZE;
    config.input_size  = INPUT_SIZE;
    config.arena_bytes = 16 << 20;
    config.max_records = 100000;
    RewindBuffer buffer;
    check(rewind_buffer_create(&buffer, &config), "lanes: created");
    uint8_t* slot = new uint8_t[STATE_SIZE];
    push_frames(&buffer, &slot, a, 0, 1000);

    RewindCursor cursor;
    rewind_cursor_create(&cursor, &buffer);
    rewind_cursor_to_newest(&buffer, &cursor);
    check(!rewind_cursor_switch_lane(&buffer, &cursor), "lanes: none to switch to before a fork");
    rewind_buffer_fork(&buffer, &cursor);
    check(buffer.kept == 0, "lanes: a fork at the newest record keeps nothing");

    // Fork at 600: 601..999 become the kept lane; B is played from there.
    check(cursor_back_to(&buffer, &cursor, 600), "lanes: back to 600");
    RewindLane first_lane = buffer.lane;
    rewind_buffer_fork(&buffer, &cursor);
    check(buffer.kept == 2 * 399 && buffer.lane == first_lane, "lanes: the future after 600 is the kept lane");
    push_frames(&buffer, &slot, &b, 601, 900);
    check(buffer.states == 900, "lanes: the recorded lane's states");
    check_lane_walk(&buffer, &b, true, false, "lanes: the recorded lane walks B");
    check_lane_walk(&buffer, a, true, true, "lanes: the kept lane walks A");
    uint32_t fork = 0;
    check(rewind_buffer_fork_point(&buffer, &fork) && rewind_buffer_record(&buffer, fork)->frame == 600,
          "lanes: they part at 600");

    // Switch at 700: the kept lane's 700 (A); back: B's 700. At 300 (shared): only the lane changes.
    rewind_cursor_to_newest(&buffer, &cursor);
    check(cursor_back_to(&buffer, &cursor, 700) && state_is(&cursor, &b, 700), "lanes: B at 700");
    check(rewind_cursor_switch_lane(&buffer, &cursor) && cursor.lane != buffer.lane &&
              rewind_buffer_record(&buffer, cursor.position)->frame == 700 && state_is(&cursor, a, 700),
          "lanes: switched to A at 700");
    check(rewind_cursor_switch_lane(&buffer, &cursor) && cursor.lane == buffer.lane && state_is(&cursor, &b, 700),
          "lanes: switched back to B at 700");
    // B is shorter (899): from A's 950, B's newest state.
    rewind_cursor_to_kept_newest(&buffer, &cursor);
    check(cursor_back_to(&buffer, &cursor, 950) && rewind_cursor_switch_lane(&buffer, &cursor) &&
              rewind_buffer_record(&buffer, cursor.position)->frame == 899 && state_is(&cursor, &b, 899),
          "lanes: past the other lane's end, its newest");
    check(cursor_back_to(&buffer, &cursor, 300) && rewind_cursor_switch_lane(&buffer, &cursor) &&
              rewind_buffer_record(&buffer, cursor.position)->frame == 300 && state_is(&cursor, a, 300),
          "lanes: a shared frame switches in place");

    // PLAY FROM HERE on the kept lane (A at 800): A is recorded on, B is kept,
    // A's 801..999 go.
    rewind_cursor_to_kept_newest(&buffer, &cursor);
    check(cursor_back_to(&buffer, &cursor, 800), "lanes: A at 800");
    rewind_buffer_fork(&buffer, &cursor);
    check(buffer.lane != first_lane && buffer.kept == 2 * 299 && buffer.dead == 2 * 199,
          "lanes: the kept lane recorded on, the other one kept");
    push_frames(&buffer, &slot, a, 801, 990);
    check_lane_walk(&buffer, a, true, false, "lanes: A recorded on");
    check_lane_walk(&buffer, &b, true, true, "lanes: B kept");

    // PLAY FROM HERE on the recorded lane before the fork (500): A's future
    // is kept, B goes (only the last abandoned future stays).
    rewind_cursor_to_newest(&buffer, &cursor);
    check(cursor_back_to(&buffer, &cursor, 500), "lanes: A at 500");
    rewind_buffer_fork(&buffer, &cursor);
    check(buffer.kept == 2 * 489 && buffer.dead == 2 * 199 + 2 * 299, "lanes: A's future kept, B dead");
    check_lane_walk(&buffer, a, true, true, "lanes: A kept from 500");
    Truth d;
    splice(&d, a, other, 500);
    push_frames(&buffer, &slot, &d, 501, 520);
    check_walk(&buffer, &d, true, "lanes: the new lane from 500");

    rewind_cursor_destroy(&cursor);
    delete[] slot;
    rewind_buffer_destroy(&buffer);
}

// A small arena: room comes from dead records, then the kept lane, and only
// then the oldest record; once it has wrapped, the kept lane goes the same way.
static void check_lane_room(const Truth* a, const Truth* other, const Truth* third)
{
    RewindBufferConfig config;
    config.state_size  = STATE_SIZE;
    config.input_size  = INPUT_SIZE;
    config.arena_bytes = 60000;
    config.max_records = 100000;
    RewindBuffer buffer;
    check(rewind_buffer_create(&buffer, &config), "room: created");
    uint8_t* slot = new uint8_t[STATE_SIZE];
    push_frames(&buffer, &slot, a, 0, 150);
    check(buffer.dropped == 0, "room: not full yet");

    // Fork at 120: the kept lane. Then 20 frames of B and a fork at 130: the
    // first kept lane is dead, B's 131..140 kept.
    RewindCursor cursor;
    rewind_cursor_create(&cursor, &buffer);
    rewind_cursor_to_newest(&buffer, &cursor);
    check(cursor_back_to(&buffer, &cursor, 120), "room: back to 120");
    rewind_buffer_fork(&buffer, &cursor);
    Truth b;
    splice(&b, a, other, 120);
    push_frames(&buffer, &slot, &b, 121, 141);
    rewind_cursor_to_newest(&buffer, &cursor);
    check(cursor_back_to(&buffer, &cursor, 130), "room: back to 130");
    rewind_buffer_fork(&buffer, &cursor);
    check(buffer.kept == 20 && buffer.dead == 58, "room: one lane dead, one kept");
    Truth c;
    splice(&c, &b, third, 130);

    uint64_t frame = 131;
    bool dead_first = false;
    while (buffer.kept > 0 && frame < FRAMES)
    {
        push_frames(&buffer, &slot, &c, frame, frame + 1);
        frame++;
        dead_first = dead_first || (buffer.dead == 0 && buffer.kept > 0);
        check(buffer.dropped == 0, "room: nothing old dropped while there's a dead record or a kept lane");
        if (buffer.kept > 0 && frame % 25 == 0)
        {
            check_lane_walk(&buffer, &c, true, false, "room: the recorded lane through compaction");
            check_lane_walk(&buffer, &b, true, true, "room: the kept lane while it lasts");
        }
    }
    check(dead_first && buffer.kept == 0, "room: dead records, then the kept lane");
    check_walk(&buffer, &c, true, "room: walk after the kept lane went");

    // Wrapped: a fork 40 frames back, the kept lane compacted away inside the
    // wrapped ring before any old record goes.
    push_frames(&buffer, &slot, &c, frame, frame + 400);
    frame += 400;
    check(buffer.dropped > 0 && buffer.wrapped, "room: the arena wrapped");
    rewind_cursor_to_newest(&buffer, &cursor);
    check(cursor_back_to(&buffer, &cursor, frame - 41), "room: back 40 frames");
    rewind_buffer_fork(&buffer, &cursor);
    check(buffer.kept == 80, "room: kept in the wrapped ring");
    check_lane_walk(&buffer, &c, true, true, "room: the kept lane, wrapped");
    Truth d;
    splice(&d, &c, a, frame - 41);
    uint64_t dropped = buffer.dropped;
    push_frames(&buffer, &slot, &d, frame - 40, frame - 39);
    check(buffer.kept == 0 && buffer.dropped == dropped, "room: wrapped, the kept lane goes before the oldest");
    check_walk(&buffer, &d, true, "room: walk after compacting a wrapped ring");
    push_frames(&buffer, &slot, &d, frame - 39, frame + 300);
    check(rewind_buffer_bytes_used(&buffer) <= config.arena_bytes, "room: within the arena");
    check_walk(&buffer, &d, true, "room: walk after more wraps");

    rewind_cursor_destroy(&cursor);
    delete[] slot;
    rewind_buffer_destroy(&buffer);
}

// The frame limit drops shared records: the kept lane goes before the last
// of them.
static void check_lane_frames(const Truth* a, const Truth* other)
{
    RewindBufferConfig config;
    config.state_size  = STATE_SIZE;
    config.input_size  = INPUT_SIZE;
    config.arena_bytes = 16 << 20;
    config.max_records = 100000;
    config.max_frames  = 300;
    RewindBuffer buffer;
    check(rewind_buffer_create(&buffer, &config), "frames: created");
    uint8_t* slot = new uint8_t[STATE_SIZE];
    push_frames(&buffer, &slot, a, 0, 1000);
    RewindCursor cursor;
    rewind_cursor_create(&cursor, &buffer);
    rewind_cursor_to_newest(&buffer, &cursor);
    check(cursor_back_to(&buffer, &cursor, 900), "frames: back to 900");
    rewind_buffer_fork(&buffer, &cursor);
    Truth b;
    splice(&b, a, other, 900);
    push_frames(&buffer, &slot, &b, 901, 1150);
    check(buffer.kept > 0, "frames: kept while shared records last");
    check_lane_walk(&buffer, a, true, true, "frames: the kept lane");
    push_frames(&buffer, &slot, &b, 1150, 1250);
    check(buffer.kept == 0, "frames: the kept lane went with the shared records");
    check_walk(&buffer, &b, true, "frames: walk after");
    rewind_cursor_destroy(&cursor);
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
    Truth other;
    Truth third;
    make_truth(&other);
    make_truth(&third);
    check_lanes(&truth, &other);
    check_lane_room(&truth, &other, &third);
    check_lane_frames(&truth, &other);
    check_bad_config();
    if (g_failures == 0)
    {
        std::printf("rewind buffer: all checks passed\n");
        return 0;
    }
    return 1;
}
