#pragma once

// rewind_recorder: feeds a rewind_buffer from a worker thread, so encoding a
// state's delta never costs the frame that made it.
//
// The game thread serializes each state straight into a slot from a small
// pool (rewind_recorder_acquire_state / _submit_state): no copy. The worker
// encodes the delta and pushes it; the buffer hands back its previous newest
// state, which becomes a free slot again (pointers are swapped, nothing is
// copied or allocated). An empty pool means the worker is behind: the state
// is skipped and counted, the frame never waits.
//
// Inputs and events are small: they are copied into a queue of jobs reserved
// at create. Jobs (states included) reach the buffer in the order they were
// queued. If the queue is full an input or event is lost, which would break
// replay: the history is restarted right there (logged and counted), so
// nothing recorded later is replayed across the gap.
//
// The buffer: the worker owns it while recording. While paused
// (rewind_recorder_pause: the DVR is open) the caller owns it: cursors,
// fork. Jobs queued while paused wait for rewind_recorder_resume.
//
// Threading: std::thread with a plain function, one std::mutex and two
// condition variables (work queued / worker idle).

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>

#include "rewind_buffer.h"

enum class RewindJobKind : uint8_t
{
    state,
    input,
    event,
};

struct RewindJob
{
    RewindJobKind kind  = RewindJobKind::state;
    uint64_t      frame = 0;
    uint8_t*      state = nullptr;   // state: the slot, given back to the pool once pushed
    uint8_t*      data  = nullptr;   // input / event bytes: max(input_size, max_event_size), reserved at create
    uint32_t      size  = 0;         // event: bytes in data
    uint8_t       event = 0;         // event: the caller's type
    bool          restart_after = false;   // the queue overflowed after this job: clear the history once it's pushed
};

struct RewindRecorderConfig
{
    RewindBufferConfig buffer = {};
    uint32_t slots          = 4;     // state buffers in the pool
    uint32_t queue          = 256;   // jobs (frames of inputs the worker may fall behind); more than slots
    uint32_t max_event_size = 256;   // bytes of one event's data
};

// Numbers for AUTO and the status line. The encode times are the worker's
// (delta encode + push), in nanoseconds.
struct RewindRecorderStats
{
    uint64_t states        = 0;   // states pushed
    uint64_t skipped       = 0;   // states skipped (no free slot)
    uint64_t restarts      = 0;   // history restarted (queue full)
    uint64_t encode_ns_sum = 0;
    uint64_t encode_ns_max = 0;
};

struct RewindRecorder
{
    RewindRecorderConfig config = {};
    RewindBuffer         buffer = {};

    // The pool: free slots as a stack. Every state buffer is in it, in a
    // queued job, out with the caller (acquired), or the buffer's newest.
    uint8_t** free_slots = nullptr;
    uint32_t  free_count = 0;
    uint32_t  out_count  = 0;   // acquired, not submitted or released yet

    // Jobs as a ring: [read, read + queued).
    RewindJob* jobs   = nullptr;
    uint32_t   read   = 0;
    uint32_t   queued = 0;
    bool       busy   = false;   // the worker is pushing the job at `read`

    RewindRecorderStats stats = {};

    std::mutex              lock;
    std::condition_variable work;   // a job was queued, or stop
    std::condition_variable idle;   // the queue went empty
    std::thread             thread;
    bool                    running = false;
    bool                    paused  = false;
};

// Creates the buffer, the pool and the queue, and starts the worker. False
// (logged) if the buffer config can't work or memory runs out.
bool rewind_recorder_create(RewindRecorder* recorder, const RewindRecorderConfig* config);
// Stops the worker (queued jobs are dropped) and frees everything. Every
// acquired slot must have been submitted or released. Safe on a recorder
// that failed to create.
void rewind_recorder_destroy(RewindRecorder* recorder);

// A free slot (state_size bytes) to serialize into, or nullptr when the pool
// is empty (counted as skipped).
uint8_t* rewind_recorder_acquire_state(RewindRecorder* recorder);
// Queues the slot as frame `frame`'s state.
void rewind_recorder_submit_state(RewindRecorder* recorder, uint8_t* slot, uint64_t frame);
// Gives a slot back unused (serialize failed).
void rewind_recorder_release_state(RewindRecorder* recorder, uint8_t* slot);
// Queues a frame's input (input_size bytes, copied).
void rewind_recorder_push_input(RewindRecorder* recorder, const uint8_t* input, uint64_t frame);
// Queues an event (data copied; more than max_event_size bytes is refused, logged).
bool rewind_recorder_push_event(RewindRecorder* recorder, uint8_t type, const uint8_t* data, uint32_t size, uint64_t frame);

// Waits until every queued job is in the buffer (refused, logged, while paused).
void rewind_recorder_drain(RewindRecorder* recorder);
// Drains, then the worker takes no job until resume: the caller owns
// recorder->buffer meanwhile.
void rewind_recorder_pause(RewindRecorder* recorder);
void rewind_recorder_resume(RewindRecorder* recorder);
// Drains, then forgets the history (a new game, a savestate loaded). While
// paused it only forgets (and drops what's queued).
void rewind_recorder_clear(RewindRecorder* recorder);

void rewind_recorder_stats(RewindRecorder* recorder, RewindRecorderStats* stats);
