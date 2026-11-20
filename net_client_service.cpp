#include "net_client_service.h"

#include <chrono>
#include <cstring>

#include <SDL3/SDL_thread.h>

#include "host_log.h"

// --- remote play, on the thread (under `lock`) --------------------------------

uint32_t net_audio_gaps_next(NetAudioGaps* gaps, uint32_t frame, uint32_t first, uint32_t count,
                             double samples_per_frame, uint32_t max_gap)
{
    if (!gaps->started)
    {
        gaps->started = true;
        gaps->base_frame = frame;
        gaps->played = static_cast<double>(first + count);   // (what came before it in this frame isn't ours to fill)
        gaps->last_frame = frame;
        gaps->last_end = first + count;
        return 0;
    }
    if (frame < gaps->last_frame || (frame == gaps->last_frame && first < gaps->last_end))
    {
        return 0;   // older than what was played (can't be put back): only counted
    }
    double missing = 0.0;
    if (frame == gaps->last_frame)
    {
        missing = static_cast<double>(first - gaps->last_end);
    }
    else
    {
        // Everything up to the end of the last frame should have come by
        // now; and this frame's own start.
        double due = static_cast<double>(gaps->last_frame - gaps->base_frame + 1) * samples_per_frame;
        double behind = due - gaps->played;
        double frames_by = static_cast<double>(frame - gaps->last_frame - 1);
        if (behind < -static_cast<double>(max_gap) || frames_by * samples_per_frame > static_cast<double>(max_gap))
        {
            missing = -1.0;   // far more than the frames account for, or frames without audio (a pause): start over
        }
        else
        {
            missing = (behind > NET_CLIENT_SERVICE_GAP_SLACK ? behind : 0.0) + static_cast<double>(first);
        }
    }
    if (missing < 0.0 || missing > static_cast<double>(max_gap))
    {
        // A pause, a fast forward, a restart: count from here.
        gaps->base_frame = frame;
        gaps->played = static_cast<double>(first + count);   // (what came before it in this frame isn't ours to fill)
        gaps->last_frame = frame;
        gaps->last_end = first + count;
        return 0;
    }
    uint32_t gap = static_cast<uint32_t>(missing + 0.5);
    gaps->played += static_cast<double>(gap) + static_cast<double>(count);
    gaps->last_frame = frame;
    gaps->last_end = first + count;
    return gap;
}

// Made-up audio for `gap` missing pairs: the last pairs played, repeated and
// fading to silence (a hole of silence would click; a hold of the last value
// would hum).
static void fill_gap(NetClientServiceStream* stream, uint32_t gap)
{
    stream->audio_fill.resize(static_cast<size_t>(gap) * 2);   // reserved at the first stream: doesn't allocate after
    uint32_t history = stream->audio_history_count;
    for (uint32_t i = 0; i < gap; i++)
    {
        float gain = 1.0f - static_cast<float>(i + 1) / static_cast<float>(gap + 1);
        int16_t left = 0;
        int16_t right = 0;
        if (history > 0)
        {
            uint32_t from = NET_CLIENT_SERVICE_AUDIO_HISTORY - history + i % history;
            left = stream->audio_history[from * 2];
            right = stream->audio_history[from * 2 + 1];
        }
        stream->audio_fill[i * 2] = static_cast<int16_t>(static_cast<float>(left) * gain);
        stream->audio_fill[i * 2 + 1] = static_cast<int16_t>(static_cast<float>(right) * gain);
    }
}

// Keeps the newest pairs played (what a gap is filled with).
static void remember_audio(NetClientServiceStream* stream, const int16_t* samples, uint32_t count)
{
    const uint32_t keep = NET_CLIENT_SERVICE_AUDIO_HISTORY;
    if (count >= keep)
    {
        std::memcpy(stream->audio_history, samples + (count - keep) * 2, keep * 2 * sizeof(int16_t));
        stream->audio_history_count = keep;
        return;
    }
    std::memmove(stream->audio_history, stream->audio_history + count * 2, (keep - count) * 2 * sizeof(int16_t));
    std::memcpy(stream->audio_history + (keep - count) * 2, samples, count * 2 * sizeof(int16_t));
    uint32_t total = stream->audio_history_count + count;
    stream->audio_history_count = total > keep ? keep : total;
}

// Audio as it comes: the device opens on the first piece (at the stream's
// rate), the rate follows the pieces.
static void stream_audio(NetClientServiceStream* stream, const NetStreamAudio* audio)
{
    stream->stats.audio_samples += audio->count;
    // What went missing before this piece.
    uint32_t gap = 0;
    if (audio->sample_rate > 0.0)
    {
        double fps = stream->has_info && stream->info.fps > 0.0 ? stream->info.fps : 60.0;
        uint32_t max_gap = static_cast<uint32_t>(audio->sample_rate * NET_CLIENT_SERVICE_MAX_GAP_SECONDS);
        gap = net_audio_gaps_next(&stream->audio_gaps, audio->frame, audio->first, audio->count,
                                  audio->sample_rate / fps, max_gap);
    }
    if (gap > 0)
    {
        stream->stats.audio_gaps++;
        stream->stats.audio_filled += gap;
    }
    if (!stream->play_audio || stream->audio_failed || audio->sample_rate <= 0.0)
    {
        return;
    }
    if (!stream->audio_open)
    {
        AudioOutputConfig config;
        if (!audio_output_create(&stream->audio, audio->sample_rate, &config))
        {
            audio_output_destroy(&stream->audio);
            stream->audio = AudioOutput{};
            stream->audio_failed = true;   // logged by audio_output; the picture goes on without sound
            return;
        }
        // The host runs the core at its own fps: its audio plays at its own
        // rate, rate control absorbs the clocks' drift.
        audio_output_set_speed(&stream->audio, 1.0, true);
        stream->audio_open = true;
        // The most a gap can make up, so filling one never allocates.
        stream->audio_fill.reserve(static_cast<size_t>(audio->sample_rate * NET_CLIENT_SERVICE_MAX_GAP_SECONDS + 1.0) * 2);
    }
    audio_output_set_sample_rate(&stream->audio, audio->sample_rate);
    if (gap > 0)
    {
        fill_gap(stream, gap);
        audio_output_push(&stream->audio, stream->audio_fill.data(), gap);
    }
    audio_output_push(&stream->audio, audio->samples, audio->count);
    remember_audio(stream, audio->samples, audio->count);
    stream->stats.audio_queued_ms = audio_output_queued_ms(&stream->audio);
    stream->stats.audio_underruns = static_cast<uint32_t>(stream->audio.underruns);
}

static void stream_close_audio(NetClientServiceStream* stream)
{
    if (stream->audio_open)
    {
        audio_output_destroy(&stream->audio);
        stream->audio = AudioOutput{};
        stream->audio_open = false;
    }
    stream->audio_failed = false;
    stream->audio_gaps = NetAudioGaps{};
    stream->audio_history_count = 0;
}

// The picture and the counts start over (a new stream, or none).
static void stream_reset(NetClientService* service)
{
    NetClientServiceStream* stream = &service->stream;
    stream->receiver.assembling = false;
    stream->receiver.need_keyframe = true;
    stream->receiver.asked = false;
    stream->receiver.decoder.has_frame = false;
    stream->last_request = -1.0;
    stream->has_info = false;
    stream->stats = NetClientStreamStats{};
    stream_close_audio(stream);
    service->frame_lock.lock();
    stream->first = 0;
    stream->count = 0;
    stream->skipped = 0;
    service->frame_lock.unlock();
}

// Drops the oldest queued picture (under `frame_lock`).
static void drop_oldest(NetClientServiceStream* stream)
{
    stream->first = (stream->first + 1) % NET_CLIENT_SERVICE_FRAMES;
    stream->count--;
    stream->skipped++;
}

static void stream_packet(NetClientService* service, const uint8_t* data, size_t size)
{
    NetClientServiceStream* stream = &service->stream;
    if (stream->flags == 0)
    {
        return;   // stopped: what's still on the way
    }
    stream->stats.bytes += size;
    NetStreamAudio audio;
    if (net_stream_read_audio(data, size, &audio, &stream->audio_storage))
    {
        stream_audio(stream, &audio);
        return;
    }
    NetStreamResult result = net_stream_receive_video(&stream->receiver, data, size);
    stream->stats.lost = stream->receiver.lost;
    if (result != NetStreamResult::frame)
    {
        return;
    }
    stream->stats.frames++;
    const FrameDeltaDecoder* decoder = &stream->receiver.decoder;
    service->frame_lock.lock();
    if (stream->count == NET_CLIENT_SERVICE_FRAMES)
    {
        drop_oldest(stream);   // the app isn't taking them (a stall)
    }
    NetClientServiceFrame* slot = &stream->frames[(stream->first + stream->count) % NET_CLIENT_SERVICE_FRAMES];
    slot->pixels.resize(decoder->frame.size());   // grows to the largest picture once, then stays
    std::memcpy(slot->pixels.data(), decoder->frame.data(), decoder->frame.size());
    slot->width = decoder->width;
    slot->height = decoder->height;
    slot->format = decoder->format;
    slot->frame = stream->receiver.frame;
    slot->info = stream->info;
    stream->count++;
    stream->stats.skipped = stream->skipped;
    service->frame_lock.unlock();
}

// A lost frame: ask for a keyframe, at most every NET_CLIENT_SERVICE_KEYFRAME_SECONDS
// (the receiver drops deltas meanwhile; the request stays owed until then).
static void stream_ask_keyframe(NetClientService* service)
{
    NetClientServiceStream* stream = &service->stream;
    if (stream->flags == 0 || !stream->receiver.asked)
    {
        return;
    }
    double now = net_seconds();
    if (stream->last_request >= 0.0 && now - stream->last_request < NET_CLIENT_SERVICE_KEYFRAME_SECONDS)
    {
        return;
    }
    uint8_t request = NET_STREAM_KEYFRAME_REQUEST;
    if (net_client_send(&service->client, NetChannel::control, &request, 1))
    {
        stream->receiver.asked = false;
        stream->last_request = now;
        stream->stats.keyframe_requests++;
    }
}

// --- the thread ---------------------------------------------------------------

static void queue_event(NetClientService* service, const NetClientEvent* event)
{
    if (service->count == NET_CLIENT_SERVICE_QUEUE)
    {
        // The app hasn't drained for a long while (a stall): the newest is dropped.
        if (service->dropped++ == 0)
        {
            host_log(LogLevel::warning, "Network: event queue full, events dropped");
        }
        return;
    }
    NetClientServiceSlot* slot = &service->queue[(service->head + service->count) % NET_CLIENT_SERVICE_QUEUE];
    slot->event = *event;
    slot->data.resize(event->size);
    if (event->size > 0)
    {
        std::memcpy(slot->data.data(), event->data, event->size);
    }
    slot->event.data = nullptr;   // set again when it's handed out
    service->count++;
}

// One poll: the stream taken here, everything else into the queue.
static void poll_once(NetClientService* service)
{
    NetClientServiceStream* stream = &service->stream;
    NetClientEvent event;
    while (net_client_poll(&service->client, &event))
    {
        if (event.type == NetClientEventType::message && event.channel == NetChannel::stream)
        {
            stream_packet(service, event.data, event.size);
            continue;
        }
        if (event.type == NetClientEventType::message && event.channel == NetChannel::control && event.size > 0 &&
            event.data[0] == NET_STREAM_INFO)
        {
            NetStreamInfo info;
            if (net_stream_read_info(event.data, event.size, &info))
            {
                stream->info = info;
                stream->has_info = true;
            }
            continue;
        }
        if (event.type == NetClientEventType::closed || event.type == NetClientEventType::refused)
        {
            stream->flags = 0;
            stream_reset(service);
        }
        queue_event(service, &event);
    }
    stream_ask_keyframe(service);
    if (stream->audio_open)
    {
        stream->stats.audio_queued_ms = audio_output_queued_ms(&stream->audio);
    }
}

static void run(NetClientService* service)
{
    for (;;)
    {
        service->lock.lock();
        bool running = service->running;
        if (running)
        {
            poll_once(service);
        }
        service->lock.unlock();
        if (!running)
        {
            // The audio uses SDL on this thread, which SDL didn't start: its
            // per-thread data (the error buffer) is freed only when asked.
            SDL_CleanupTLS();
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(NET_CLIENT_SERVICE_POLL_MS));
    }
}

bool net_client_service_start(NetClientService* service, const NetKeypair* identity, const char* name,
                              const char* trust_path)
{
    service->head = 0;
    service->count = 0;
    service->dropped = 0;
    service->stream.flags = 0;
    if (!net_client_create(&service->client, identity, name, trust_path))
    {
        net_client_destroy(&service->client);
        return false;
    }
    service->running = true;
    service->thread = std::thread(run, service);
    return true;
}

void net_client_service_stop(NetClientService* service)
{
    if (service->thread.joinable())
    {
        service->lock.lock();
        service->running = false;
        service->lock.unlock();
        service->thread.join();
    }
    stream_close_audio(&service->stream);
    net_client_destroy(&service->client);
    service->count = 0;
}

void net_client_service_lock(NetClientService* service)
{
    service->lock.lock();
}

void net_client_service_unlock(NetClientService* service)
{
    service->lock.unlock();
}

bool net_client_service_next(NetClientService* service, NetClientEvent* event)
{
    if (service->count == 0)
    {
        return false;
    }
    NetClientServiceSlot* slot = &service->queue[service->head];
    service->head = (service->head + 1) % NET_CLIENT_SERVICE_QUEUE;
    service->count--;
    // The payload moves to `current` (no copy): the slot gets `current`'s
    // old buffer to reuse.
    service->current.swap(slot->data);
    *event = slot->event;
    event->data = service->current.empty() ? nullptr : service->current.data();
    return true;
}

// ---------------------------------------------------------------------------
// Remote play
// ---------------------------------------------------------------------------

bool net_client_service_set_stream(NetClientService* service, uint8_t flags)
{
    stream_reset(service);
    service->stream.flags = 0;
    if (service->client.state != NetClientState::ready)
    {
        return false;
    }
    uint8_t message[8];
    size_t size = 0;
    if (flags != 0)
    {
        size = net_stream_write_start(message, sizeof(message), flags);
    }
    else
    {
        message[0] = NET_STREAM_STOP;
        size = 1;
    }
    if (size == 0 || !net_client_send(&service->client, NetChannel::control, message, size))
    {
        return false;
    }
    service->stream.flags = flags;
    return true;
}

bool net_client_service_stream_info(NetClientService* service, NetStreamInfo* info)
{
    *info = service->stream.info;
    return service->stream.has_info;
}

void net_client_service_stream_stats(NetClientService* service, NetClientStreamStats* stats)
{
    *stats = service->stream.stats;
}

bool net_client_service_take_frame(NetClientService* service, std::vector<uint8_t>* pixels, uint32_t* width,
                                   uint32_t* height, FramePixelFormat* format, uint32_t* frame, NetStreamInfo* info)
{
    service->frame_lock.lock();
    NetClientServiceStream* stream = &service->stream;
    while (stream->count > stream->depth)
    {
        drop_oldest(stream);
    }
    bool fresh = stream->count > 0;
    if (fresh)
    {
        NetClientServiceFrame* slot = &stream->frames[stream->first];
        pixels->swap(slot->pixels);
        *width = slot->width;
        *height = slot->height;
        *format = slot->format;
        *frame = slot->frame;
        *info = slot->info;
        stream->first = (stream->first + 1) % NET_CLIENT_SERVICE_FRAMES;
        stream->count--;
    }
    service->frame_lock.unlock();
    return fresh;
}

void net_client_service_set_frame_queue(NetClientService* service, uint32_t depth)
{
    depth = depth < 1 ? 1 : (depth > NET_CLIENT_SERVICE_FRAMES ? NET_CLIENT_SERVICE_FRAMES : depth);
    service->frame_lock.lock();
    service->stream.depth = depth;
    service->frame_lock.unlock();
}
