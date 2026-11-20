#include "net_client_service.h"

#include <chrono>
#include <cstring>

#include "host_log.h"

// --- remote play, on the thread (under `lock`) --------------------------------

// Audio as it comes: the device opens on the first piece (at the stream's
// rate), the rate follows the pieces.
static void stream_audio(NetClientServiceStream* stream, const NetStreamAudio* audio)
{
    stream->stats.audio_samples += audio->count;
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
    }
    audio_output_set_sample_rate(&stream->audio, audio->sample_rate);
    audio_output_push(&stream->audio, audio->samples, audio->count);
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
    stream->fresh = false;
    service->frame_lock.unlock();
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
    stream->pixels.resize(decoder->frame.size());   // grows to the largest picture once, then stays
    std::memcpy(stream->pixels.data(), decoder->frame.data(), decoder->frame.size());
    stream->width = decoder->width;
    stream->height = decoder->height;
    stream->format = decoder->format;
    stream->frame = stream->receiver.frame;
    stream->frame_info = stream->info;
    stream->fresh = true;
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
    bool fresh = stream->fresh;
    if (fresh)
    {
        pixels->swap(stream->pixels);
        *width = stream->width;
        *height = stream->height;
        *format = stream->format;
        *frame = stream->frame;
        *info = stream->frame_info;
        stream->fresh = false;
    }
    service->frame_lock.unlock();
    return fresh;
}
