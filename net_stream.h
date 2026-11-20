#pragma once

// Remote play's stream (netcode milestone 6, part two), as pure functions:
// piccolo's shown frames (frame_delta.h) and their audio, split into packets
// under the MTU on the stream channel (unreliable, sequenced), and put back
// together on the client.
//
// Stream channel (host -> client), little-endian:
//
//   video  type 1 | frame u32 | flags u8 (bit 0 keyframe) | chunk u16 | chunks u16 | bytes...
//   audio  type 2 | frame u32 | rate mHz u32 | first u16 | count u16 | count x { left i16 | right i16 }
//
// `frame` is piccolo's loop number (with run-ahead the picture is the shown
// one and the audio the real frame's: both carry the loop's number). A coded
// frame goes in chunks of at most NET_STREAM_CHUNK bytes; a frame whose
// chunks don't all arrive is lost, and so is every delta after it until a
// keyframe: the client asks for one (keyframe_request) and drops deltas
// meanwhile. Audio pieces stand alone (each plays as it comes; a lost one is
// a gap): `first` is the piece's first sample in its frame.
//
// Control (STREAM access):
//
//   client -> host  stream_start      flags u8 (bit 0 video, bit 1 audio)
//   client -> host  stream_stop
//   client -> host  keyframe_request
//   host -> client  stream_info       width u16 | height u16 | format u8 | aspect | fps | sample rate
//                                     (the three numbers as text: they're doubles in libretro)
//
// stream_info comes at the start and whenever the picture's size, aspect,
// format or timing changes.
//
//   client -> host  stream_tuning         delay mode u8 | delay ms u8 | run-ahead frames u8
//   host -> client  stream_tuning_result  delay mode u8 | delay ms u8 | run-ahead frames u8 | flags u8
//
// stream_tuning: how the host should run while this client streams, for
// lower latency (Andrés: the client's pacing choice). Only in the host's
// memory, for this stream: its settings aren't changed or saved (a crash
// mustn't leave them changed), nor what AUTO learned for the game locally.
// The result says what's applied now (with AUTO the delay comes down as the
// host learns: sent again whenever it changes); flags bit 0 = the delay
// waits for present (piccolo's frame delay works only that way: turned on in
// memory for the stream if needed). Run-ahead: 0 until the host has it.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "frame_delta.h"

constexpr uint8_t NET_STREAM_START = 70;
constexpr uint8_t NET_STREAM_STOP = 71;
constexpr uint8_t NET_STREAM_KEYFRAME_REQUEST = 72;
constexpr uint8_t NET_STREAM_INFO = 73;
constexpr uint8_t NET_STREAM_TUNING = 74;
constexpr uint8_t NET_STREAM_TUNING_RESULT = 75;
constexpr uint8_t NET_STREAM_MAX_DELAY_MS = 50;
constexpr uint8_t NET_STREAM_MAX_RUN_AHEAD = 4;

constexpr uint8_t NET_STREAM_VIDEO = 1;
constexpr uint8_t NET_STREAM_AUDIO = 2;
constexpr uint8_t NET_STREAM_WANT_VIDEO = 1;
constexpr uint8_t NET_STREAM_WANT_AUDIO = 2;

constexpr size_t   NET_STREAM_CHUNK = 1100;          // payload bytes per packet (under the MTU with every header)
constexpr size_t   NET_STREAM_VIDEO_HEADER = 10;
constexpr size_t   NET_STREAM_AUDIO_HEADER = 13;
constexpr uint32_t NET_STREAM_AUDIO_PIECE = 256;     // stereo samples per audio packet (1024 bytes)
constexpr uint32_t NET_STREAM_MAX_CHUNKS = 4096;     // a frame's chunks at most (4.5 MB)

struct NetStreamInfo
{
    uint32_t         width = 0;
    uint32_t         height = 0;
    FramePixelFormat format = FramePixelFormat::rgb565;
    double           aspect = 0.0;        // as the core's geometry says (0 = width / height)
    double           fps = 0.0;
    double           sample_rate = 0.0;
};

enum class NetStreamDelay : uint8_t
{
    host = 0,     // whatever the host's settings say
    off = 1,
    automatic = 2,   // the host's AUTO search, for this stream (not saved)
    manual = 3,   // `delay_ms`
};
constexpr uint8_t NET_STREAM_DELAY_COUNT = 4;
constexpr uint8_t NET_STREAM_TUNING_WAITS_FOR_PRESENT = 1;

struct NetStreamTuning
{
    NetStreamDelay delay = NetStreamDelay::host;
    uint8_t        delay_ms = 0;     // manual: asked; result: in use now
    uint8_t        run_ahead = 0;    // frames
    uint8_t        flags = 0;        // result only
};

// --- control messages ---------------------------------------------------------

size_t net_stream_write_start(uint8_t* out, size_t capacity, uint8_t flags);
bool   net_stream_read_start(const uint8_t* data, size_t size, uint8_t* flags);
size_t net_stream_write_info(uint8_t* out, size_t capacity, const NetStreamInfo* info);
bool   net_stream_read_info(const uint8_t* data, size_t size, NetStreamInfo* info);
size_t net_stream_write_tuning(uint8_t* out, size_t capacity, uint8_t type, const NetStreamTuning* tuning);
// `type`: NET_STREAM_TUNING or NET_STREAM_TUNING_RESULT. Values come back capped.
bool   net_stream_read_tuning(const uint8_t* data, size_t size, uint8_t type, NetStreamTuning* tuning);
// stream_stop and keyframe_request are their type byte alone.

// --- sending ------------------------------------------------------------------

// How many packets a coded frame of `size` bytes takes.
uint32_t net_stream_chunk_count(size_t size);

// Chunk `chunk` of a coded frame as a stream packet into `out`
// (NET_STREAM_VIDEO_HEADER + NET_STREAM_CHUNK bytes). Returns its size.
size_t net_stream_write_video(uint8_t* out, size_t capacity, uint32_t frame, bool keyframe, const uint8_t* coded,
                              size_t coded_size, uint32_t chunk);

// An audio piece: samples [first, first + count) of a frame's (stereo pairs),
// count at most NET_STREAM_AUDIO_PIECE. Returns its size.
size_t net_stream_write_audio(uint8_t* out, size_t capacity, uint32_t frame, double sample_rate,
                              const int16_t* samples, uint32_t first, uint32_t count);

// --- receiving ----------------------------------------------------------------

struct NetStreamAudio
{
    uint32_t       frame = 0;
    double         sample_rate = 0.0;
    uint32_t       first = 0;
    uint32_t       count = 0;
    const int16_t* samples = nullptr;   // count stereo pairs; valid until the next call
};

// An audio packet. (Samples are copied into `storage`.)
bool net_stream_read_audio(const uint8_t* data, size_t size, NetStreamAudio* audio, std::vector<int16_t>* storage);

struct NetStreamReceiver
{
    FrameDeltaDecoder    decoder;
    // The frame being put together.
    bool                 assembling = false;
    uint32_t             frame = 0;
    bool                 keyframe = false;
    uint32_t             chunks = 0;
    uint32_t             received = 0;
    std::vector<uint8_t> have;          // per chunk: arrived
    std::vector<uint8_t> coded;         // chunks * NET_STREAM_CHUNK
    size_t               coded_size = 0;
    // Since a frame was lost, only a keyframe helps.
    bool                 need_keyframe = true;   // until the first keyframe
    bool                 asked = false;          // a keyframe_request is owed (net_stream_receive set it)
    uint32_t             last_shown = 0;         // the newest frame decoded
    uint32_t             shown = 0;              // frames decoded
    uint32_t             lost = 0;               // frames that didn't all arrive, or were deltas without their frame
};

enum class NetStreamResult
{
    nothing,   // not a video packet, or one that leaves the frame incomplete
    frame,     // a whole frame arrived and decoded: receiver->decoder.frame is the picture
    lost,      // a frame was lost (or a delta was useless): ask for a keyframe (`asked` is set)
};

// A video packet from the stream channel.
NetStreamResult net_stream_receive_video(NetStreamReceiver* receiver, const uint8_t* data, size_t size);
