#pragma once

// The client's network on its own thread (piccoletto, net_service.h's other
// side): NetClient is polled there about every millisecond, not once per
// frame of the app's loop, so the round trip doesn't carry up to a frame of
// waiting, a keyframe's hundred packets are drained as they come, and input
// the app sends leaves at the next millisecond.
//
// Remote play (net_stream.h) is handled on the thread: video packets go into
// the NetStreamReceiver there (frame_delta's decoding is cheap), and whole
// pictures wait for the app in a short queue (net_client_service_take_frame),
// as deep as the app's pacing says (net_client_service_set_frame_queue):
// 1 = only the newest (the lowest latency: a picture not taken yet is
// replaced); more = taken in order, one per frame of the app, so two arriving
// within one vsync are both shown (smoother, up to that many frames later);
// past that the oldest go. A
// lost frame asks for a keyframe, at most every
// NET_CLIENT_SERVICE_KEYFRAME_SECONDS. Audio pieces are played as they
// come (audio_output.h: a small queue kept at its target by rate control,
// which also absorbs the drift between the host's clock and the device's);
// a lost piece is a gap. Video is never held back for the audio.
//
// Everything else NetClient reports is copied into a queue the app drains
// once per loop (net_client_service_next). Everything else in NetClient
// (connecting, sending, the trust list, stats, unpairing) is used as before,
// between net_client_service_lock and net_client_service_unlock: one mutex
// guards the NetClient and the stream; the thread takes it only for its
// short polls. It's recursive, so an app's functions that call each other
// can each take it.
//
// Threading: std::thread with a plain function, a std::recursive_mutex for
// the client, a std::mutex for the picture handed over.

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#include "audio_output.h"
#include "net_client.h"
#include "net_stream.h"

constexpr uint32_t NET_CLIENT_SERVICE_QUEUE = 256;              // events between two drains
constexpr uint32_t NET_CLIENT_SERVICE_POLL_MS = 1;
constexpr double   NET_CLIENT_SERVICE_KEYFRAME_SECONDS = 0.1;   // between two keyframe requests
constexpr uint32_t NET_CLIENT_SERVICE_FRAMES = 4;               // pictures the queue holds at most
// Lost audio: a gap longer than this isn't filled (the host paused, or the
// stream restarted: there's nothing to keep in step with).
constexpr double   NET_CLIENT_SERVICE_MAX_GAP_SECONDS = 0.1;
constexpr uint32_t NET_CLIENT_SERVICE_AUDIO_HISTORY = 128;      // stereo pairs kept to fill a gap with
// Less missing than this isn't filled (frames hold rate / fps pairs only on
// average: one holds 533, the next 534).
constexpr double   NET_CLIENT_SERVICE_GAP_SLACK = 16.0;

struct NetClientServiceSlot
{
    NetClientEvent       event;
    std::vector<uint8_t> data;   // the event's payload, copied (reused: grows, never shrinks)
};

// What the stream did, for the app's status line (counts since it started).
struct NetClientStreamStats
{
    uint32_t frames = 0;            // whole frames decoded
    uint32_t lost = 0;              // frames lost (a missing chunk, or a delta without its frame)
    uint32_t keyframe_requests = 0;
    uint64_t bytes = 0;             // stream payload received
    uint64_t audio_samples = 0;     // stereo pairs received
    uint32_t audio_queued_ms = 0;   // audio waiting to play now
    uint32_t audio_underruns = 0;
    uint32_t skipped = 0;           // whole frames never shown (the queue was past its depth)
    uint32_t audio_gaps = 0;        // lost audio filled in (pieces that never came)
    uint64_t audio_filled = 0;      // stereo pairs made up for them
};

// A whole picture waiting for the app.
struct NetClientServiceFrame
{
    std::vector<uint8_t> pixels;             // tightly packed (reused: grows, never shrinks)
    uint32_t             width = 0;
    uint32_t             height = 0;
    FramePixelFormat     format = FramePixelFormat::rgb565;
    uint32_t             frame = 0;          // the host's loop number
    NetStreamInfo        info;               // the stream_info in force when it was decoded
};

// Finding lost audio: pieces of one frame must follow each other (first =
// where the last one ended); across frames, the pairs played since the first
// frame must match the frames that went by times samples_per_frame (so
// audio the host sent in one go for two loops isn't taken for a loss). A
// gap longer than max_gap, or more audio than the frames account for by as
// much, starts over (a pause, a fast forward). Pure, for tests.
struct NetAudioGaps
{
    bool     started = false;
    uint32_t base_frame = 0;     // counting from this frame's start
    double   played = 0.0;       // pairs received or filled since then
    uint32_t last_frame = 0;     // the newest piece's frame
    uint32_t last_end = 0;       // where it ended in that frame (first + count)
};

// The pairs missing before this piece (0 = none); counts the piece and the
// fill as played.
uint32_t net_audio_gaps_next(NetAudioGaps* gaps, uint32_t frame, uint32_t first, uint32_t count,
                             double samples_per_frame, uint32_t max_gap);

struct NetClientServiceStream
{
    // Under the service's `lock`.
    uint8_t              flags = 0;            // NET_STREAM_WANT_*, 0 = not streaming
    bool                 play_audio = true;    // false: audio is counted, not played (tests, tools)
    NetStreamReceiver    receiver;
    double               last_request = -1.0;  // when a keyframe was last asked for
    NetStreamInfo        info;
    bool                 has_info = false;
    AudioOutput          audio;
    bool                 audio_open = false;
    bool                 audio_failed = false; // no device: don't try again this stream
    std::vector<int16_t> audio_storage;
    // Lost audio (a piece that never came) is filled in, so the queue keeps
    // its length: otherwise every lost piece shortens it for good, faster
    // than rate control's 0.5% can make up, and the latency target climbs.
    NetAudioGaps         audio_gaps;
    int16_t              audio_history[NET_CLIENT_SERVICE_AUDIO_HISTORY * 2] = {};   // the last pairs played
    uint32_t             audio_history_count = 0;
    std::vector<int16_t> audio_fill;            // scratch for the made-up audio (reserved once)
    NetClientStreamStats stats;
    // The pictures waiting for the app, oldest first, under `frame_lock`.
    NetClientServiceFrame frames[NET_CLIENT_SERVICE_FRAMES];
    uint32_t             first = 0;            // the oldest
    uint32_t             count = 0;
    uint32_t             depth = 1;            // how many the app takes in order (1 = only the newest)
    uint32_t             skipped = 0;          // dropped from the queue (copied into stats)
};

struct NetClientService
{
    NetClient            client;               // guarded by `lock` once started
    std::recursive_mutex lock;
    std::thread          thread;
    bool                 running = false;      // read by the thread under `lock`
    NetClientServiceSlot queue[NET_CLIENT_SERVICE_QUEUE];
    uint32_t             head = 0;             // the oldest queued event
    uint32_t             count = 0;
    uint32_t             dropped = 0;          // events lost to a full queue (logged)
    std::vector<uint8_t> current;              // the payload of the event net_client_service_next last gave
    std::mutex           frame_lock;           // the picture handed over, apart from `lock`
    NetClientServiceStream stream;
};

// Creates the NetClient (as net_client_create, `info` = this device) and
// starts the thread. Builds are checked with net_build_verify (net_build.h).
bool net_client_service_start(NetClientService* service, const NetKeypair* identity, const NetDeviceInfo* info,
                              const char* trust_path);
// Stops the thread, closes the audio and destroys the NetClient. Safe on a
// service that didn't start.
void net_client_service_stop(NetClientService* service);

void net_client_service_lock(NetClientService* service);
void net_client_service_unlock(NetClientService* service);

// Locked: the next queued event, oldest first; its `data` stays valid until
// the next call. False when the queue is empty. Stream packets and
// stream_info never come here (the thread takes them).
bool net_client_service_next(NetClientService* service, NetClientEvent* event);

// --- remote play --------------------------------------------------------------

// Locked: asks the host for the stream (flags NET_STREAM_WANT_*: stream_start)
// or stops it (0: stream_stop); either way the picture, the counts and the
// audio start over. False when the client isn't ready.
bool net_client_service_set_stream(NetClientService* service, uint8_t flags);


// Locked: the stream's info (false until the host sent it) and counts.
bool net_client_service_stream_info(NetClientService* service, NetStreamInfo* info);
void net_client_service_stream_stats(NetClientService* service, NetClientStreamStats* stats);

// Not locked (takes `frame_lock` briefly): how many pictures the app takes in
// order (1..NET_CLIENT_SERVICE_FRAMES; 1 = only the newest, the default).
void net_client_service_set_frame_queue(NetClientService* service, uint32_t depth);

// Not locked (takes `frame_lock` briefly): the next picture if one came since
// the last call: the oldest in the queue once it's down to its depth (the
// ones past it are skipped), so with depth 1 the newest. Its pixels are
// swapped into `pixels` (tightly packed, width x height in `format`; the
// vector given back is reused, so after the first few frames nothing
// allocates). `info`: the stream_info it goes with. False when there's
// nothing new.
bool net_client_service_take_frame(NetClientService* service, std::vector<uint8_t>* pixels, uint32_t* width,
                                   uint32_t* height, FramePixelFormat* format, uint32_t* frame, NetStreamInfo* info);
