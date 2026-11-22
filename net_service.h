#pragma once

// The host's network on its own thread (netcode milestone 6): ENet is polled
// there about every millisecond, not once per frame of the frontend's loop,
// so the round trip doesn't carry up to a frame of waiting, high rates get
// drained in time, and (later) frames are encoded and sent there without
// touching the frontend's loop: AUTO frame delay and run-ahead keep their
// time.
//
// What net_access_poll reports is copied into a queue the frontend drains
// once per loop (net_service_next). Everything else in NetAccess (sending,
// pairing, the device list, levels) is used as before, between
// net_service_lock and net_service_unlock: one mutex guards the whole
// NetAccess, the thread takes it only for its short polls.
//
// Threading: std::thread with a plain function, one std::mutex. The thread
// never calls the frontend or the core.

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "net_access.h"
#include "net_share_host.h"
#include "net_stream.h"

constexpr uint32_t NET_SERVICE_QUEUE = 1024;     // events between two drains (input packets included)
constexpr uint32_t NET_SERVICE_POLL_MS = 1;

struct NetServiceSlot
{
    NetAccessEvent       event;
    std::vector<uint8_t> data;   // the event's payload, copied (reused: grows, never shrinks)
};

// Remote play (net_stream.h), done on the thread: the frontend hands over
// each frame it shows and the audio of the frames it ran; the thread codes
// the frame once (frame_delta.h: one delta for every device; a keyframe only
// for the ones that need it) and sends both to the devices streaming.
struct NetServiceStream
{
    // Handed over by the frontend (under `frame_lock`).
    std::vector<uint8_t> pixels;
    uint32_t             width = 0;
    uint32_t             height = 0;
    FramePixelFormat     format = FramePixelFormat::rgb565;
    uint32_t             frame = 0;          // the frontend's loop number
    bool                 video_ready = false;
    std::vector<int16_t> audio;              // stereo pairs since the thread last took them
    uint32_t             audio_frame = 0;
    NetStreamInfo        info;
    // The thread's own.
    std::vector<uint8_t> work_pixels;
    std::vector<int16_t> work_audio;
    FrameDeltaEncoder    encoder;            // the deltas everyone gets
    FrameDeltaEncoder    key_encoder;        // keyframes, for who asks
    std::vector<uint8_t> coded;
    std::vector<uint8_t> key_coded;
    std::vector<uint8_t> packet;
    NetStreamInfo        sent_info;          // the last stream_info sent
    // Per peer, guarded by the service's `lock` (set by the frontend from
    // stream_start / stream_stop / keyframe_request).
    uint8_t              flags[NET_MAX_PEERS] = {};      // NET_STREAM_WANT_*, 0 = not streaming
    bool                 keyframe_wanted[NET_MAX_PEERS] = {};
    bool                 info_wanted[NET_MAX_PEERS] = {};
    uint32_t             frames_sent = 0;
    uint32_t             keyframes_sent = 0;
};

struct NetService
{
    NetAccess      access;               // guarded by `lock` once started
    std::mutex     lock;
    std::thread    thread;
    bool           running = false;      // read by the thread under `lock`
    NetServiceSlot queue[NET_SERVICE_QUEUE];
    uint32_t       head = 0;             // the oldest queued event
    uint32_t       count = 0;
    uint32_t       dropped = 0;          // events lost to a full queue (logged)
    std::vector<uint8_t> current;        // the payload of the event net_service_next last gave
    std::mutex     frame_lock;           // the frame and audio handed over (stream), apart from `lock`
    NetServiceStream stream;
    // The beacon (across the internet): its name is resolved on a thread of
    // its own (DNS may take seconds; neither the frontend nor the network
    // thread should wait for it).
    std::thread    resolver;
    std::string    beacon_text;          // guarded by `lock`
    // piccolo-share: SHARE devices' listings and copies, handled on the
    // thread (never queued for the frontend); guarded by `lock`.
    NetShareHost   share;
    uint32_t       beacon_generation = 0;   // guarded by `lock`: a newer set_beacon wins
};

// Creates the NetAccess (as net_access_create) and starts the thread.
bool net_service_start(NetService* service, uint16_t port, const char* name, const NetKeypair* identity,
                       const char* trust_path);
// Stops the thread and destroys the NetAccess. Safe on a service that
// didn't start.
void net_service_stop(NetService* service);

void net_service_lock(NetService* service);
void net_service_unlock(NetService* service);

// Not locked: across the internet through the beacon `server` ("name",
// "name:port", "[v6]:port"), or off with nullptr / "". Resolved on its own
// thread; the access gets the addresses when it's done (net_access_beacon_status
// says how it goes). Waits for a lookup still running from an earlier call.
// The same beacon's WebRTC signaling (browsers) starts at once, where the
// build has WebRTC (net_access_signal_status).
void net_service_set_beacon(NetService* service, const char* server);

// Locked: the next queued event, oldest first; its `data` stays valid until
// the next call. False when the queue is empty.
bool net_service_next(NetService* service, NetAccessEvent* event);

// Locked: the shared folders for SHARE devices (piccolo-share): the game
// folders, then SAVES and STATES ("" = not shared).
void net_service_set_share_folders(NetService* service, const std::vector<std::string>* game_paths,
                                   const char* saves_path, const char* states_path);

// --- remote play --------------------------------------------------------------

// Locked: a peer starts (flags NET_STREAM_WANT_*) or stops (0) streaming. A
// start gets stream_info and a keyframe first.
void net_service_set_stream(NetService* service, uint32_t peer, uint8_t flags);
// Locked: the peer lost a frame and needs a keyframe.
void net_service_request_keyframe(NetService* service, uint32_t peer);
// Locked: is any peer streaming? (The frontend hands frames over only then.)
bool net_service_streaming(const NetService* service);

// Not locked (takes `frame_lock` briefly): the frame the frontend shows now,
// tightly packed in `format`, with its loop number and the stream's info. A
// frame not taken yet is replaced (the thread codes the newest).
void net_service_submit_frame(NetService* service, const uint8_t* pixels, uint32_t width, uint32_t height,
                              FramePixelFormat format, uint32_t frame, const NetStreamInfo* info);
// Not locked: the audio of the frames run in this loop (stereo pairs), added
// to what the thread hasn't sent yet.
void net_service_submit_audio(NetService* service, const int16_t* samples, size_t count, uint32_t frame);
