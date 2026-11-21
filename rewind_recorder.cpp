#include "rewind_recorder.h"

#include <chrono>
#include <cstring>
#include <new>

#include "host_log.h"

static uint64_t now_ns()
{
    std::chrono::steady_clock::duration since = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(since).count());
}

// The worker: takes the job at `read`, pushes it into the buffer without the
// lock (only the worker touches the buffer while jobs are queued), then
// retires it. The job's bytes stay valid meanwhile: the game thread never
// reuses a job before `read` moves past it.
static void run(RewindRecorder* recorder)
{
    std::unique_lock<std::mutex> guard(recorder->lock);
    for (;;)
    {
        while (recorder->running && (recorder->queued == 0 || recorder->paused))
            recorder->work.wait(guard);
        if (!recorder->running)
            break;

        RewindJob* job = &recorder->jobs[recorder->read];
        recorder->busy = true;
        guard.unlock();

        uint8_t* given_back = nullptr;
        uint64_t encode_ns  = 0;
        if (job->kind == RewindJobKind::state)
        {
            // push_state swaps: the slot becomes the newest state and the
            // previous newest comes back as a free slot.
            given_back = job->state;
            uint64_t start = now_ns();
            rewind_buffer_push_state(&recorder->buffer, &given_back, job->frame);
            encode_ns = now_ns() - start;
        }
        else if (job->kind == RewindJobKind::input)
        {
            rewind_buffer_push_input(&recorder->buffer, job->data, job->frame);
        }
        else
        {
            rewind_buffer_push_event(&recorder->buffer, job->event, job->data, job->size, job->frame);
        }

        guard.lock();
        if (job->restart_after)
        {
            // An input or event after this job was lost: nothing recorded
            // from here on may be replayed from what's before.
            rewind_buffer_clear(&recorder->buffer);
            job->restart_after = false;
        }
        if (job->kind == RewindJobKind::state)
        {
            recorder->free_slots[recorder->free_count] = given_back;
            recorder->free_count++;
            job->state = nullptr;
            recorder->stats.states++;
            recorder->stats.encode_ns_sum += encode_ns;
            if (encode_ns > recorder->stats.encode_ns_max)
                recorder->stats.encode_ns_max = encode_ns;
        }
        recorder->read = (recorder->read + 1) % recorder->config.queue;
        recorder->queued--;
        recorder->busy = false;
        if (recorder->queued == 0)
            recorder->idle.notify_all();
    }
}

// Drops the queued jobs (under the lock, worker paused or idle): their slots
// go back to the pool.
static void drop_queued(RewindRecorder* recorder)
{
    for (uint32_t i = 0; i < recorder->queued; i++)
    {
        RewindJob* job = &recorder->jobs[(recorder->read + i) % recorder->config.queue];
        if (job->kind == RewindJobKind::state && job->state != nullptr)
        {
            recorder->free_slots[recorder->free_count] = job->state;
            recorder->free_count++;
            job->state = nullptr;
        }
        job->restart_after = false;
    }
    recorder->read   = (recorder->read + recorder->queued) % recorder->config.queue;
    recorder->queued = 0;
}

static uint32_t job_data_size(const RewindRecorderConfig* config)
{
    size_t input = config->buffer.input_size;
    return input > config->max_event_size ? static_cast<uint32_t>(input) : config->max_event_size;
}

bool rewind_recorder_create(RewindRecorder* recorder, const RewindRecorderConfig* config)
{
    recorder->config = *config;
    if (config->slots == 0 || config->queue <= config->slots)
    {
        host_log(LogLevel::error, "rewind: a recorder needs slots (%u) and a longer queue (%u)", config->slots,
                 config->queue);
        return false;
    }
    if (!rewind_buffer_create(&recorder->buffer, &config->buffer))
        return false;

    size_t   state_size = config->buffer.state_size;
    uint32_t data_size  = job_data_size(config);
    recorder->free_slots = new (std::nothrow) uint8_t*[config->slots]();
    recorder->jobs       = new (std::nothrow) RewindJob[config->queue];
    bool ok = recorder->free_slots != nullptr && recorder->jobs != nullptr;
    for (uint32_t i = 0; ok && i < config->slots; i++)
    {
        uint8_t* slot = new (std::nothrow) uint8_t[state_size];
        if (slot == nullptr)
        {
            ok = false;
            break;
        }
        recorder->free_slots[recorder->free_count] = slot;
        recorder->free_count++;
    }
    for (uint32_t i = 0; ok && i < config->queue; i++)
    {
        recorder->jobs[i].data = new (std::nothrow) uint8_t[data_size];
        if (recorder->jobs[i].data == nullptr)
            ok = false;
    }
    if (!ok)
    {
        host_log(LogLevel::error, "rewind: out of memory for the recorder (%u slots of %zu bytes)", config->slots,
                 state_size);
        rewind_recorder_destroy(recorder);
        return false;
    }

    recorder->running = true;
    recorder->thread  = std::thread(run, recorder);
    return true;
}

void rewind_recorder_destroy(RewindRecorder* recorder)
{
    if (recorder->thread.joinable())
    {
        {
            std::lock_guard<std::mutex> guard(recorder->lock);
            recorder->running = false;
        }
        recorder->work.notify_all();
        recorder->thread.join();
    }

    if (recorder->out_count != 0)
        host_log(LogLevel::error, "rewind: %u state slots still out at destroy (leaked)", recorder->out_count);

    if (recorder->jobs != nullptr)
    {
        // Slots still queued go back to the pool to be freed with it.
        drop_queued(recorder);
        for (uint32_t i = 0; i < recorder->config.queue; i++)
            delete[] recorder->jobs[i].data;
        delete[] recorder->jobs;
    }
    if (recorder->free_slots != nullptr)
    {
        for (uint32_t i = 0; i < recorder->free_count; i++)
            delete[] recorder->free_slots[i];
        delete[] recorder->free_slots;
    }
    rewind_buffer_destroy(&recorder->buffer);

    recorder->free_slots = nullptr;
    recorder->free_count = 0;
    recorder->out_count  = 0;
    recorder->jobs       = nullptr;
    recorder->read       = 0;
    recorder->queued     = 0;
    recorder->busy       = false;
    recorder->running    = false;
    recorder->paused     = false;
    recorder->stats      = {};
}

uint8_t* rewind_recorder_acquire_state(RewindRecorder* recorder)
{
    std::lock_guard<std::mutex> guard(recorder->lock);
    // A state also needs a job; both run out when the worker is behind.
    if (recorder->free_count == 0 || recorder->queued + recorder->out_count >= recorder->config.queue)
    {
        recorder->stats.skipped++;
        return nullptr;
    }
    recorder->free_count--;
    recorder->out_count++;
    return recorder->free_slots[recorder->free_count];
}

// The next free job (under the lock), or nullptr when the queue is full
// (keeping `reserve` jobs free): then the history is restarted after the
// newest queued job. queue > slots (create), so a full queue has one.
static RewindJob* queue_job(RewindRecorder* recorder, uint32_t reserve)
{
    if (recorder->queued + reserve >= recorder->config.queue)
    {
        RewindJob* newest = &recorder->jobs[(recorder->read + recorder->queued - 1) % recorder->config.queue];
        if (!newest->restart_after)
        {
            newest->restart_after = true;
            recorder->stats.restarts++;
            host_log(LogLevel::warning, "rewind: the recorder fell %u jobs behind, history restarted",
                     recorder->config.queue);
        }
        return nullptr;
    }
    RewindJob* job = &recorder->jobs[(recorder->read + recorder->queued) % recorder->config.queue];
    job->restart_after = false;
    return job;
}

static void commit_job(RewindRecorder* recorder)
{
    recorder->queued++;
    recorder->work.notify_one();
}

void rewind_recorder_submit_state(RewindRecorder* recorder, uint8_t* slot, uint64_t frame)
{
    std::lock_guard<std::mutex> guard(recorder->lock);
    recorder->out_count--;
    // acquire_state kept a job free for every slot out.
    RewindJob* job = queue_job(recorder, 0);
    job->kind  = RewindJobKind::state;
    job->frame = frame;
    job->state = slot;
    commit_job(recorder);
}

void rewind_recorder_release_state(RewindRecorder* recorder, uint8_t* slot)
{
    std::lock_guard<std::mutex> guard(recorder->lock);
    recorder->out_count--;
    recorder->free_slots[recorder->free_count] = slot;
    recorder->free_count++;
}

void rewind_recorder_push_input(RewindRecorder* recorder, const uint8_t* input, uint64_t frame)
{
    if (recorder->config.buffer.input_size == 0)
        return;
    std::lock_guard<std::mutex> guard(recorder->lock);
    // Keep a job for every slot out, or submit_state couldn't queue it.
    RewindJob* job = queue_job(recorder, recorder->out_count);
    if (job == nullptr)
        return;
    job->kind  = RewindJobKind::input;
    job->frame = frame;
    std::memcpy(job->data, input, recorder->config.buffer.input_size);
    commit_job(recorder);
}

bool rewind_recorder_push_event(RewindRecorder* recorder, uint8_t type, const uint8_t* data, uint32_t size,
                                uint64_t frame)
{
    if (size > recorder->config.max_event_size)
    {
        host_log(LogLevel::warning, "rewind: an event of %u bytes is over the limit (%u)", size,
                 recorder->config.max_event_size);
        return false;
    }
    std::lock_guard<std::mutex> guard(recorder->lock);
    RewindJob* job = queue_job(recorder, recorder->out_count);
    if (job == nullptr)
        return false;
    job->kind  = RewindJobKind::event;
    job->frame = frame;
    job->event = type;
    job->size  = size;
    if (size > 0)
        std::memcpy(job->data, data, size);
    commit_job(recorder);
    return true;
}

void rewind_recorder_drain(RewindRecorder* recorder)
{
    std::unique_lock<std::mutex> guard(recorder->lock);
    if (recorder->paused)
    {
        host_log(LogLevel::error, "rewind: drain while paused (it would never end)");
        return;
    }
    while (recorder->queued > 0)
        recorder->idle.wait(guard);
}

void rewind_recorder_pause(RewindRecorder* recorder)
{
    std::unique_lock<std::mutex> guard(recorder->lock);
    while (recorder->queued > 0)
        recorder->idle.wait(guard);
    recorder->paused = true;
}

void rewind_recorder_resume(RewindRecorder* recorder)
{
    {
        std::lock_guard<std::mutex> guard(recorder->lock);
        recorder->paused = false;
    }
    recorder->work.notify_one();
}

void rewind_recorder_clear(RewindRecorder* recorder)
{
    std::unique_lock<std::mutex> guard(recorder->lock);
    if (recorder->paused)
        drop_queued(recorder);
    while (recorder->queued > 0)
        recorder->idle.wait(guard);
    rewind_buffer_clear(&recorder->buffer);
}

void rewind_recorder_stats(RewindRecorder* recorder, RewindRecorderStats* stats)
{
    std::lock_guard<std::mutex> guard(recorder->lock);
    *stats = recorder->stats;
}
