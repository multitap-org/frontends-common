#include "net_service.h"

#include <chrono>
#include <cstring>

#include "host_log.h"

// One poll: everything NetAccess has for us, copied into the queue.
static void poll_once(NetService* service)
{
    NetAccessEvent event;
    while (net_access_poll(&service->access, &event))
    {
        if (service->count == NET_SERVICE_QUEUE)
        {
            // The frontend hasn't drained for a long while (a stall): the
            // newest is dropped; pads repeat their last states anyway.
            if (service->dropped++ == 0)
            {
                host_log(LogLevel::warning, "Network: event queue full, events dropped");
            }
            continue;
        }
        NetServiceSlot* slot = &service->queue[(service->head + service->count) % NET_SERVICE_QUEUE];
        slot->event = event;
        slot->data.resize(event.size);
        if (event.size > 0)
        {
            std::memcpy(slot->data.data(), event.data, event.size);
        }
        slot->event.data = nullptr;   // set again when it's handed out
        service->count++;
    }
}

static bool same_info(const NetStreamInfo* a, const NetStreamInfo* b)
{
    return a->width == b->width && a->height == b->height && a->format == b->format && a->aspect == b->aspect &&
           a->fps == b->fps && a->sample_rate == b->sample_rate;
}

// Remote play: takes what the frontend handed over, codes it (without the
// lock: only the thread uses the encoders), then sends it to the peers
// streaming (with the lock: NetAccess). Called after each poll.
static void stream_once(NetService* service)
{
    NetServiceStream* stream = &service->stream;
    bool video = false;
    uint32_t width = 0;
    uint32_t height = 0;
    FramePixelFormat format = FramePixelFormat::rgb565;
    uint32_t frame = 0;
    uint32_t audio_frame = 0;
    NetStreamInfo info;
    service->frame_lock.lock();
    if (stream->video_ready)
    {
        stream->pixels.swap(stream->work_pixels);
        width = stream->width;
        height = stream->height;
        format = stream->format;
        frame = stream->frame;
        info = stream->info;
        stream->video_ready = false;
        video = true;
    }
    stream->work_audio.clear();
    stream->work_audio.swap(stream->audio);
    audio_frame = stream->audio_frame;
    service->frame_lock.unlock();
    if (!video && stream->work_audio.empty())
    {
        return;
    }

    service->lock.lock();
    bool any_key = false;
    bool any_video = false;
    for (uint32_t peer = 0; peer < NET_MAX_PEERS; peer++)
    {
        bool wants_video = (stream->flags[peer] & NET_STREAM_WANT_VIDEO) != 0;
        any_video = any_video || wants_video;
        any_key = any_key || (wants_video && stream->keyframe_wanted[peer]);
    }
    service->lock.unlock();

    size_t coded_size = 0;
    size_t key_size = 0;
    if (video && any_video)
    {
        size_t most = frame_delta_max_size(width, height, format);
        stream->coded.resize(most);
        coded_size = frame_delta_encode(&stream->encoder, stream->work_pixels.data(), width, height, format, false,
                                        stream->coded.data());
        if (any_key)
        {
            stream->key_coded.resize(most);
            key_size = frame_delta_encode(&stream->key_encoder, stream->work_pixels.data(), width, height, format,
                                          true, stream->key_coded.data());
        }
    }
    bool keyframe_delta = frame_delta_is_keyframe(stream->coded.data(), coded_size);   // the first, or a new size

    stream->packet.resize(NET_STREAM_VIDEO_HEADER + NET_STREAM_CHUNK + NET_STREAM_AUDIO_HEADER +
                          NET_STREAM_AUDIO_PIECE * 4);
    uint8_t* packet = stream->packet.data();
    service->lock.lock();
    bool info_changed = video && !same_info(&info, &stream->sent_info);
    for (uint32_t peer = 0; peer < NET_MAX_PEERS; peer++)
    {
        uint8_t flags = stream->flags[peer];
        if (flags == 0)
        {
            continue;
        }
        if (video && (info_changed || stream->info_wanted[peer]))
        {
            size_t size = net_stream_write_info(packet, stream->packet.size(), &info);
            net_access_send(&service->access, peer, NetChannel::control, packet, size);
            stream->info_wanted[peer] = false;
        }
        if (coded_size > 0 && (flags & NET_STREAM_WANT_VIDEO) != 0)
        {
            bool key = stream->keyframe_wanted[peer] && key_size > 0 && !keyframe_delta;
            const uint8_t* coded = key ? stream->key_coded.data() : stream->coded.data();
            size_t size = key ? key_size : coded_size;
            uint32_t chunks = net_stream_chunk_count(size);
            for (uint32_t chunk = 0; chunk < chunks; chunk++)
            {
                size_t packet_size =
                    net_stream_write_video(packet, stream->packet.size(), frame, key || keyframe_delta, coded, size, chunk);
                net_access_send(&service->access, peer, NetChannel::stream, packet, packet_size);
            }
            if (key || keyframe_delta)
            {
                stream->keyframe_wanted[peer] = false;
                stream->keyframes_sent++;
            }
            stream->frames_sent++;
        }
        if (!stream->work_audio.empty() && (flags & NET_STREAM_WANT_AUDIO) != 0)
        {
            uint32_t total = static_cast<uint32_t>(stream->work_audio.size() / 2);
            for (uint32_t first = 0; first < total && first <= 0xFFFF; first += NET_STREAM_AUDIO_PIECE)
            {
                uint32_t count = total - first < NET_STREAM_AUDIO_PIECE ? total - first : NET_STREAM_AUDIO_PIECE;
                size_t size = net_stream_write_audio(packet, stream->packet.size(), audio_frame, info.sample_rate,
                                                     stream->work_audio.data(), first, count);
                net_access_send(&service->access, peer, NetChannel::stream, packet, size);
            }
        }
    }
    if (info_changed)
    {
        stream->sent_info = info;
    }
    service->lock.unlock();
}

static void run(NetService* service)
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
        if (running)
        {
            stream_once(service);
        }
        if (!running)
        {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(NET_SERVICE_POLL_MS));
    }
}

static void resolve_beacon(NetService* service, uint32_t generation, std::string text)
{
    NetAddress addresses[NET_BEACON_MAX_SERVERS];
    uint32_t count = text.empty() ? 0 : net_address_resolve(text.c_str(), NET_BEACON_DEFAULT_PORT, addresses);
    service->lock.lock();
    if (generation == service->beacon_generation)
    {
        net_access_set_beacon(&service->access, addresses, count);
        if (!text.empty())
        {
            host_log(count > 0 ? LogLevel::info : LogLevel::warning, "Network: beacon %s: %u address(es)",
                     text.c_str(), static_cast<unsigned>(count));
        }
    }
    service->lock.unlock();
}

void net_service_set_beacon(NetService* service, const char* server)
{
    if (service->resolver.joinable())
    {
        service->resolver.join();
    }
    service->lock.lock();
    service->beacon_generation++;
    uint32_t generation = service->beacon_generation;
    service->beacon_text = server != nullptr ? server : "";
    std::string text = service->beacon_text;
    // Browsers through the same beacon (WebRTC signaling; needs no lookup
    // here: the socket resolves its own name). Nothing in builds without WebRTC.
    if (text.empty())
    {
        net_access_set_signal(&service->access, "", nullptr);
    }
    else
    {
        net_access_set_signal(&service->access, net_signal_url(text.c_str()).c_str(), net_signal_stun(text.c_str()).c_str());
    }
    service->lock.unlock();
    service->resolver = std::thread(resolve_beacon, service, generation, text);
}

bool net_service_start(NetService* service, uint16_t port, const char* name, const NetKeypair* identity,
                       const char* trust_path)
{
    service->head = 0;
    service->count = 0;
    service->dropped = 0;
    for (uint32_t peer = 0; peer < NET_MAX_PEERS; peer++)
    {
        service->stream.flags[peer] = 0;
    }
    service->stream.encoder = FrameDeltaEncoder{};
    service->stream.key_encoder = FrameDeltaEncoder{};
    service->stream.sent_info = NetStreamInfo{};
    if (!net_access_create(&service->access, port, name, identity, trust_path))
    {
        net_access_destroy(&service->access);
        return false;
    }
    service->running = true;
    service->thread = std::thread(run, service);
    return true;
}

void net_service_stop(NetService* service)
{
    if (service->resolver.joinable())
    {
        service->resolver.join();
    }
    if (service->thread.joinable())
    {
        service->lock.lock();
        service->running = false;
        service->lock.unlock();
        service->thread.join();
    }
    net_access_destroy(&service->access);
    service->count = 0;
}

void net_service_lock(NetService* service)
{
    service->lock.lock();
}

void net_service_unlock(NetService* service)
{
    service->lock.unlock();
}

bool net_service_next(NetService* service, NetAccessEvent* event)
{
    if (service->count == 0)
    {
        return false;
    }
    NetServiceSlot* slot = &service->queue[service->head];
    service->head = (service->head + 1) % NET_SERVICE_QUEUE;
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

void net_service_set_stream(NetService* service, uint32_t peer, uint8_t flags)
{
    if (peer >= NET_MAX_PEERS)
    {
        return;
    }
    bool starting = service->stream.flags[peer] == 0 && flags != 0;
    service->stream.flags[peer] = flags;
    if (starting)
    {
        service->stream.keyframe_wanted[peer] = true;
        service->stream.info_wanted[peer] = true;
    }
}

void net_service_request_keyframe(NetService* service, uint32_t peer)
{
    if (peer < NET_MAX_PEERS && service->stream.flags[peer] != 0)
    {
        service->stream.keyframe_wanted[peer] = true;
    }
}

bool net_service_streaming(const NetService* service)
{
    for (uint32_t peer = 0; peer < NET_MAX_PEERS; peer++)
    {
        if (service->stream.flags[peer] != 0)
        {
            return true;
        }
    }
    return false;
}

void net_service_submit_frame(NetService* service, const uint8_t* pixels, uint32_t width, uint32_t height,
                              FramePixelFormat format, uint32_t frame, const NetStreamInfo* info)
{
    size_t bytes = static_cast<size_t>(width) * height * frame_pixel_bytes(format);
    service->frame_lock.lock();
    NetServiceStream* stream = &service->stream;
    stream->pixels.resize(bytes);   // grows to the largest picture once, then stays
    std::memcpy(stream->pixels.data(), pixels, bytes);
    stream->width = width;
    stream->height = height;
    stream->format = format;
    stream->frame = frame;
    stream->info = *info;
    stream->video_ready = true;
    service->frame_lock.unlock();
}

void net_service_submit_audio(NetService* service, const int16_t* samples, size_t count, uint32_t frame)
{
    service->frame_lock.lock();
    NetServiceStream* stream = &service->stream;
    stream->audio.insert(stream->audio.end(), samples, samples + count * 2);
    stream->audio_frame = frame;
    service->frame_lock.unlock();
}
