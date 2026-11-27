#include "net_service.h"

#include <chrono>
#include <cstring>

#include "host_log.h"
#include "net_build.h"
#include "net_rtc.h"

static void resolve_pick_up(NetService* service);   // (the beacon lookup, further down)

// One poll: everything NetAccess has for us, copied into the queue.
static void poll_once(NetService* service)
{
    NetAccessEvent event;
    while (net_access_poll(&service->access, &event))
    {
        if (net_share_host_handle(&service->share, &service->access, &event))
        {
            continue;   // piccolo-share's, handled here
        }
        if (event.type == NetAccessEventType::closed)
        {
            net_share_host_peer_closed(&service->share, event.peer);
        }
        // The last NET_SERVICE_QUEUE_RESERVE slots are for everything but
        // pad messages: a flood of pad packets must not push out `closed`
        // (the frontend's per-device cleanup hangs off it), connects or
        // pairing.
        bool input_message = event.type == NetAccessEventType::message && event.channel == NetChannel::input;
        if (input_message && service->count >= NET_SERVICE_QUEUE - NET_SERVICE_QUEUE_RESERVE)
        {
            // The frontend hasn't drained for a long while (a stall): the
            // newest pad message is dropped; pads repeat their last states anyway.
            if (service->dropped++ == 0)
            {
                host_log(LogLevel::warning, "Network: event queue full, input events dropped");
            }
            continue;
        }
        if (service->count == NET_SERVICE_QUEUE)
        {
            // Even the reserve is full (a stall of minutes): something that
            // matters is lost. Logged once, like the warning above.
            if (service->lost_lifecycle++ == 0)
            {
                host_log(LogLevel::error, "Network: event queue full, lifecycle events lost (connections, pairing)");
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
            resolve_pick_up(service);
            poll_once(service);
        }
        service->lock.unlock();
        if (running)
        {
            stream_once(service);
            // Copies after the stream: they wait while anyone plays remotely.
            service->lock.lock();
            net_share_host_pump(&service->share, &service->access, net_service_streaming(service));
            service->lock.unlock();
        }
        if (!running)
        {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(NET_SERVICE_POLL_MS));
    }
}

// One beacon lookup's mailbox (heap, made by net_service_set_beacon). The
// lookup thread (detached: a blocking getaddrinfo can't be cancelled, so
// nobody waits for it) touches only this, never the NetService, which may be
// gone by the time it finishes.
//
// Ownership: exactly one side deletes it.
// - The thread, when it finds `abandoned` after resolving (the service no
//   longer wants the answer and has let go of the pointer).
// - The service in every other case: it sees `done`, or it abandons a
//   mailbox that is already `done` (the thread has finished and won't touch
//   it again).
// `lock` makes the decision atomic: the thread sets `done` or reads
// `abandoned` in one locked step, the service the reverse. `text` and
// `generation` are written before the thread starts and never change.
// Lock order: the service's `lock`, then the mailbox's (the thread takes
// only the mailbox's). `NetService::resolve` is guarded by the service's lock.
struct NetServiceResolve
{
    std::mutex lock;
    bool       abandoned = false;   // the service let go: the thread deletes
    bool       done = false;        // the addresses are in: the service deletes
    std::string text;
    uint32_t   generation = 0;
    NetAddress addresses[NET_BEACON_MAX_SERVERS];
    uint32_t   count = 0;
};

static void resolve_thread(NetServiceResolve* mailbox)
{
    NetAddress addresses[NET_BEACON_MAX_SERVERS];
    uint32_t count = mailbox->text.empty() ? 0 : net_address_resolve(mailbox->text.c_str(), NET_BEACON_DEFAULT_PORT, addresses);
    mailbox->lock.lock();
    if (mailbox->abandoned)
    {
        mailbox->lock.unlock();
        delete mailbox;   // the last owner
        return;
    }
    for (uint32_t i = 0; i < count; i++)
    {
        mailbox->addresses[i] = addresses[i];
    }
    mailbox->count = count;
    mailbox->done = true;
    mailbox->lock.unlock();   // the service owns it from here
}

// With the service's `lock` held: lets go of the pending lookup, if any,
// without waiting for it.
static void resolve_abandon(NetService* service)
{
    NetServiceResolve* mailbox = service->resolve;
    if (mailbox == nullptr)
    {
        return;
    }
    service->resolve = nullptr;
    mailbox->lock.lock();
    if (mailbox->done)
    {
        mailbox->lock.unlock();
        delete mailbox;   // the thread is finished with it
        return;
    }
    mailbox->abandoned = true;
    mailbox->lock.unlock();   // the thread deletes it when it finishes
}

// With the service's `lock` held, from the service's thread (`run`): applies
// a finished lookup. Not before net_service_start or after net_service_stop
// (no thread then): the mailbox just waits, and stop / the next set_beacon
// abandons it.
static void resolve_pick_up(NetService* service)
{
    NetServiceResolve* mailbox = service->resolve;
    if (mailbox == nullptr)
    {
        return;
    }
    mailbox->lock.lock();
    if (!mailbox->done)
    {
        mailbox->lock.unlock();
        return;
    }
    NetAddress addresses[NET_BEACON_MAX_SERVERS];
    uint32_t count = mailbox->count;
    for (uint32_t i = 0; i < count; i++)
    {
        addresses[i] = mailbox->addresses[i];
    }
    std::string text = mailbox->text;
    uint32_t generation = mailbox->generation;
    mailbox->lock.unlock();
    delete mailbox;
    service->resolve = nullptr;
    if (generation == service->beacon_generation)
    {
        net_access_set_beacon(&service->access, addresses, count);
        if (!text.empty())
        {
            host_log(count > 0 ? LogLevel::info : LogLevel::warning, "Network: beacon %s: %u address(es)",
                     text.c_str(), static_cast<unsigned>(count));
        }
    }
}

void net_service_set_beacon(NetService* service, const char* server)
{
    service->lock.lock();
    resolve_abandon(service);   // never waits: the old lookup finishes on its own
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
    if (net_rtc_only())
    {
        service->lock.unlock();
        return;   // the browser: no UDP, so no beacon registration to look up (signaling only)
    }
    NetServiceResolve* mailbox = new NetServiceResolve();
    mailbox->text = text;
    mailbox->generation = generation;
    service->resolve = mailbox;
    // Detached at once: the thread owns nothing but the mailbox.
    std::thread(resolve_thread, mailbox).detach();
    service->lock.unlock();
}

bool net_service_start(NetService* service, uint16_t port, const NetDeviceInfo* info, const NetKeypair* identity,
                       const char* trust_path)
{
    service->head = 0;
    service->count = 0;
    service->dropped = 0;
    service->lost_lifecycle = 0;
    for (uint32_t peer = 0; peer < NET_MAX_PEERS; peer++)
    {
        service->stream.flags[peer] = 0;
    }
    service->stream.encoder = FrameDeltaEncoder{};
    service->stream.key_encoder = FrameDeltaEncoder{};
    service->stream.sent_info = NetStreamInfo{};
    if (!net_access_create(&service->access, port, info, identity, trust_path))
    {
        net_access_destroy(&service->access);
        return false;
    }
    net_access_set_verify_build(&service->access, net_build_verify, nullptr);
    net_share_host_start(&service->share);
    service->running = true;
    service->thread = std::thread(run, service);
    return true;
}

void net_service_stop(NetService* service)
{
    if (service->thread.joinable())
    {
        service->lock.lock();
        service->running = false;
        service->lock.unlock();
        service->thread.join();
    }
    // After the thread is gone: nobody picks the lookup up any more.
    service->lock.lock();
    resolve_abandon(service);
    service->lock.unlock();
    net_share_host_stop(&service->share);
    net_access_destroy(&service->access);
    service->count = 0;
}

void net_service_set_share_folders(NetService* service, const NetShareFolderPaths* paths)
{
    net_share_host_set_folders(&service->share, paths);
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
