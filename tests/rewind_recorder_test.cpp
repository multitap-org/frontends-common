// Checks rewind_recorder.h: what the game thread queues (states in slots,
// inputs, events) reaches the buffer in order and intact; an empty pool skips
// the state; a full queue restarts the history right where an input was
// lost (never replays across the gap); pause gives the buffer to the caller
// (truncate, then recording goes on); a worker left far behind (big states,
// no frame pacing) still leaves a consistent history.
//
// States and inputs are pure functions of the frame number, so any record
// can be checked without keeping copies.
//
// Exit code 0 = all checks passed. Each failure is printed.

#include <cstdio>
#include <cstring>
#include <vector>

#include "rewind_recorder.h"

static int g_failures = 0;

static void check(bool condition, const char* what)
{
    if (!condition)
    {
        std::printf("FAIL %s\n", what);
        g_failures++;
    }
}

constexpr size_t INPUT_SIZE = 12;

// Word i changes every 1 + i % 61 frames: some words every frame, most
// rarely, like a game's RAM. The tail bytes (size not a multiple of 4) too.
static void state_for(uint64_t frame, uint8_t* out, size_t size)
{
    size_t words = size / 4;
    for (size_t i = 0; i < words; i++)
    {
        uint32_t word = static_cast<uint32_t>(i) * 2654435761u + static_cast<uint32_t>(frame / (1 + i % 61));
        std::memcpy(out + i * 4, &word, 4);
    }
    for (size_t i = words * 4; i < size; i++)
        out[i] = static_cast<uint8_t>(frame / 7);
}

static void input_for(uint64_t frame, uint8_t* out)
{
    for (size_t k = 0; k < INPUT_SIZE; k++)
        out[k] = static_cast<uint8_t>(frame / (8 + k));
}

static void record_frame(RewindRecorder* recorder, uint64_t frame)
{
    uint8_t* slot = rewind_recorder_acquire_state(recorder);
    if (slot != nullptr)
    {
        state_for(frame, slot, recorder->config.buffer.state_size);
        rewind_recorder_submit_state(recorder, slot, frame);
    }
    uint8_t input[INPUT_SIZE];
    input_for(frame, input);
    rewind_recorder_push_input(recorder, input, frame);
}

// Walks the (paused) recorder's buffer from the newest record to the oldest:
// every state and input is the one for its frame, inputs have no gaps, the
// oldest is a state. Returns the number of records.
static uint32_t check_history(RewindRecorder* recorder, const char* name)
{
    RewindBuffer* buffer = &recorder->buffer;
    size_t        size   = buffer->config.state_size;
    RewindCursor  cursor = {};
    check(rewind_cursor_create(&cursor, buffer), name);
    std::vector<uint8_t> expected(size);
    uint8_t              input[INPUT_SIZE];
    bool                 states_ok = true;
    bool                 inputs_ok = true;
    bool                 no_gaps   = true;
    uint64_t             last_input_frame = 0;
    bool                 seen_input       = false;

    if (rewind_cursor_to_newest(buffer, &cursor))
    {
        for (;;)
        {
            const RewindRecord* record = rewind_buffer_record(buffer, cursor.position);
            if (record->kind == RewindRecordKind::state)
            {
                state_for(record->frame, expected.data(), size);
                if (std::memcmp(cursor.state, expected.data(), size) != 0)
                    states_ok = false;
            }
            else if (record->kind == RewindRecordKind::input)
            {
                input_for(record->frame, input);
                if (std::memcmp(cursor.input, input, INPUT_SIZE) != 0)
                    inputs_ok = false;
                if (seen_input && record->frame + 1 != last_input_frame)
                    no_gaps = false;
                last_input_frame = record->frame;
                seen_input       = true;
            }
            if (!rewind_cursor_back(buffer, &cursor))
                break;
        }
        check(cursor.position == 0, name);
        check(rewind_buffer_record(buffer, 0)->kind == RewindRecordKind::state, name);
    }
    if (!states_ok || !inputs_ok || !no_gaps)
        std::printf("  %s: states %s, inputs %s, gaps %s\n", name, states_ok ? "ok" : "WRONG",
                    inputs_ok ? "ok" : "WRONG", no_gaps ? "none" : "FOUND");
    check(states_ok && inputs_ok && no_gaps, name);
    rewind_cursor_destroy(&cursor);
    return buffer->count;
}

static RewindRecorderConfig make_config(size_t state_size, size_t arena, uint32_t slots, uint32_t queue)
{
    RewindRecorderConfig config = {};
    config.buffer.state_size  = state_size;
    config.buffer.input_size  = INPUT_SIZE;
    config.buffer.arena_bytes = arena;
    config.buffer.max_records = 16384;
    config.buffer.max_frames  = 0;
    config.slots              = slots;
    config.queue              = queue;
    config.max_event_size     = 64;
    return config;
}

// Every frame a state and an input, events now and then; then pause,
// truncate in the middle and keep recording from there.
static void test_record_and_truncate()
{
    RewindRecorderConfig config   = make_config(40003, 64u << 20, 4, 256);
    RewindRecorder       recorder = {};
    check(rewind_recorder_create(&recorder, &config), "record: create");

    // A drain every few frames stands in for frame pacing (a game makes 60
    // frames a second, not as many as it can): the queue never overflows.
    for (uint64_t frame = 0; frame < 2000; frame++)
    {
        if (frame % 8 == 0)
            rewind_recorder_drain(&recorder);
        record_frame(&recorder, frame);
        if (frame % 500 == 250)
        {
            uint8_t data[3] = { 1, 2, static_cast<uint8_t>(frame) };
            check(rewind_recorder_push_event(&recorder, 7, data, 3, frame), "record: event");
        }
    }
    uint8_t too_big[65] = {};
    check(!rewind_recorder_push_event(&recorder, 7, too_big, 65, 0), "record: oversized event refused");

    rewind_recorder_pause(&recorder);
    RewindRecorderStats stats = {};
    rewind_recorder_stats(&recorder, &stats);
    check(stats.states + stats.skipped == 2000, "record: every state pushed or skipped");
    check(stats.restarts == 0, "record: no restart");
    std::printf("record: %llu states, %llu skipped, encode avg %.3f ms, max %.3f ms\n",
                static_cast<unsigned long long>(stats.states), static_cast<unsigned long long>(stats.skipped),
                stats.states ? static_cast<double>(stats.encode_ns_sum) / static_cast<double>(stats.states) / 1e6 : 0.0,
                static_cast<double>(stats.encode_ns_max) / 1e6);
    uint32_t records = check_history(&recorder, "record: history");
    check(records == stats.states + 2000 + 4, "record: every record kept");

    uint32_t events = 0;
    for (uint32_t i = 0; i < recorder.buffer.count; i++)
    {
        const RewindRecord* record = rewind_buffer_record(&recorder.buffer, i);
        if (record->kind != RewindRecordKind::event)
            continue;
        uint32_t       size = 0;
        const uint8_t* data = rewind_buffer_event_data(&recorder.buffer, i, &size);
        check(record->event == 7 && size == 3 && data[2] == static_cast<uint8_t>(record->frame), "record: event data");
        events++;
    }
    check(events == 4, "record: four events");

    // Back to a state around frame 1000, play from there.
    RewindCursor cursor = {};
    check(rewind_cursor_create(&cursor, &recorder.buffer), "truncate: cursor");
    bool any = rewind_cursor_to_newest(&recorder.buffer, &cursor);
    check(any, "truncate: a history to walk");
    for (; any;)
    {
        const RewindRecord* record = rewind_buffer_record(&recorder.buffer, cursor.position);
        if (record->kind == RewindRecordKind::state && record->frame <= 1000)
            break;
        if (!rewind_cursor_back(&recorder.buffer, &cursor))
            break;
    }
    uint64_t from = rewind_buffer_record(&recorder.buffer, cursor.position)->frame;
    check(from <= 1000 && from > 900, "truncate: a state near frame 1000");
    rewind_buffer_truncate(&recorder.buffer, &cursor);
    rewind_cursor_destroy(&cursor);
    rewind_recorder_resume(&recorder);

    // The game runs frame `from` again from that state (its state is the
    // cursor's, already the newest): only its input, then whole frames.
    uint8_t input[INPUT_SIZE];
    input_for(from, input);
    rewind_recorder_push_input(&recorder, input, from);
    for (uint64_t frame = from + 1; frame < from + 600; frame++)
    {
        if (frame % 8 == 0)
            rewind_recorder_drain(&recorder);
        record_frame(&recorder, frame);
    }
    rewind_recorder_pause(&recorder);
    check_history(&recorder, "truncate: history after playing on");
    check(rewind_buffer_record(&recorder.buffer, recorder.buffer.count - 1)->frame == from + 599,
          "truncate: newest frame");
    rewind_recorder_resume(&recorder);

    rewind_recorder_clear(&recorder);
    check(recorder.buffer.count == 0, "record: clear");
    rewind_recorder_destroy(&recorder);
}

// The pool runs out: the state is skipped and counted, never waited for.
static void test_skip()
{
    RewindRecorderConfig config   = make_config(1000, 1u << 20, 2, 8);
    RewindRecorder       recorder = {};
    check(rewind_recorder_create(&recorder, &config), "skip: create");

    uint8_t* a = rewind_recorder_acquire_state(&recorder);
    uint8_t* b = rewind_recorder_acquire_state(&recorder);
    uint8_t* c = rewind_recorder_acquire_state(&recorder);
    check(a != nullptr && b != nullptr && c == nullptr, "skip: two slots, the third skipped");
    state_for(5, b, 1000);
    rewind_recorder_submit_state(&recorder, b, 5);
    rewind_recorder_release_state(&recorder, a);
    rewind_recorder_drain(&recorder);

    RewindRecorderStats stats = {};
    rewind_recorder_stats(&recorder, &stats);
    check(stats.states == 1 && stats.skipped == 1, "skip: counted");
    check(recorder.free_count == 2, "skip: slots back in the pool");
    rewind_recorder_pause(&recorder);
    check(check_history(&recorder, "skip: history") == 1, "skip: one state");
    rewind_recorder_resume(&recorder);
    rewind_recorder_destroy(&recorder);
}

// The queue fills up while paused: inputs are lost, so the history restarts
// right after the last one queued and only what follows the next state counts.
static void test_restart()
{
    RewindRecorderConfig config   = make_config(1000, 1u << 20, 2, 8);
    RewindRecorder       recorder = {};
    check(rewind_recorder_create(&recorder, &config), "restart: create");

    for (uint64_t frame = 0; frame < 3; frame++)
    {
        record_frame(&recorder, frame);
        rewind_recorder_drain(&recorder);
    }
    rewind_recorder_pause(&recorder);

    uint8_t input[INPUT_SIZE];
    for (uint64_t frame = 3; frame <= 20; frame++)   // 8 fit, 11..20 are lost
    {
        input_for(frame, input);
        rewind_recorder_push_input(&recorder, input, frame);
    }
    RewindRecorderStats stats = {};
    rewind_recorder_stats(&recorder, &stats);
    check(stats.restarts == 1, "restart: counted once");
    rewind_recorder_resume(&recorder);
    rewind_recorder_drain(&recorder);   // or frame 21 would find the queue still full

    for (uint64_t frame = 21; frame <= 30; frame++)
    {
        record_frame(&recorder, frame);
        rewind_recorder_drain(&recorder);
    }
    rewind_recorder_pause(&recorder);
    rewind_recorder_stats(&recorder, &stats);
    check(stats.restarts == 1, "restart: no other restart");
    uint32_t kept = check_history(&recorder, "restart: history");
    if (kept != 20)
        std::printf("  restart: %u records, oldest frame %llu\n", kept,
                    kept ? static_cast<unsigned long long>(rewind_buffer_record(&recorder.buffer, 0)->frame) : 0ull);
    check(kept == 20, "restart: only states 21..30 and their inputs");
    check(recorder.buffer.count > 0 && rewind_buffer_record(&recorder.buffer, 0)->frame == 21,
          "restart: the oldest is state 21");

    // Clear while paused drops what's queued (slots back to the pool).
    record_frame(&recorder, 31);
    rewind_recorder_clear(&recorder);
    check(recorder.queued == 0 && recorder.free_count == 2 && recorder.buffer.count == 0, "restart: clear while paused");
    rewind_recorder_resume(&recorder);
    rewind_recorder_destroy(&recorder);
}

// Big states and no frame pacing: the worker falls behind. Whatever was
// skipped or restarted, the history left is consistent.
static void test_behind()
{
    RewindRecorderConfig config   = make_config(1u << 20, 64u << 20, 4, 32);
    RewindRecorder       recorder = {};
    check(rewind_recorder_create(&recorder, &config), "behind: create");
    for (uint64_t frame = 0; frame < 400; frame++)
        record_frame(&recorder, frame);
    rewind_recorder_pause(&recorder);
    RewindRecorderStats stats = {};
    rewind_recorder_stats(&recorder, &stats);
    std::printf("behind: %llu states, %llu skipped, %llu restarts\n", static_cast<unsigned long long>(stats.states),
                static_cast<unsigned long long>(stats.skipped), static_cast<unsigned long long>(stats.restarts));
    check(stats.states + stats.skipped == 400, "behind: every state pushed or skipped");
    check_history(&recorder, "behind: history");
    rewind_recorder_resume(&recorder);
    // Destroy with jobs still queued.
    for (uint64_t frame = 400; frame < 420; frame++)
        record_frame(&recorder, frame);
    rewind_recorder_destroy(&recorder);
}

static void test_bad_config()
{
    RewindRecorderConfig short_queue = make_config(1000, 1u << 20, 4, 4);
    RewindRecorder       a           = {};
    check(!rewind_recorder_create(&a, &short_queue), "bad: queue not longer than the pool");
    rewind_recorder_destroy(&a);
    RewindRecorderConfig no_state = make_config(0, 1u << 20, 2, 8);
    RewindRecorder       b        = {};
    check(!rewind_recorder_create(&b, &no_state), "bad: no state size");
    rewind_recorder_destroy(&b);
}

int main()
{
    test_record_and_truncate();
    test_skip();
    test_restart();
    test_behind();
    test_bad_config();
    if (g_failures == 0)
        std::printf("rewind recorder: all checks passed\n");
    else
        std::printf("rewind recorder: %d checks failed\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
