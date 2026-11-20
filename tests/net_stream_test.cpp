// Checks net_stream.h: coded frames go out in chunks and come back whole (in
// order or shuffled within a frame), a lost chunk loses its frame and the
// deltas after it until a keyframe (asked for), audio pieces and stream_info
// read back.
//
// Exit code 0 = all checks passed. Each failure is printed.

#include <cstdio>
#include <cstring>
#include <vector>

#include "net_stream.h"

static int g_failures = 0;

static void check(bool condition, const char* what)
{
    if (!condition)
    {
        std::printf("FAIL %s\n", what);
        g_failures++;
    }
}

static uint32_t g_random = 777;

static uint32_t next_random()
{
    g_random = g_random * 1103515245u + 12345u;
    return g_random >> 8;
}

// A noisy 256x224 RGB565 picture, so frames take many chunks.
static void picture(std::vector<uint8_t>* pixels, uint32_t seed)
{
    pixels->resize(256 * 224 * 2);
    for (size_t i = 0; i < pixels->size(); i++)
    {
        (*pixels)[i] = static_cast<uint8_t>((i / 64 + seed) * 37 + (next_random() % 4 == 0 ? next_random() : 0));
    }
}

// Sends one frame's chunks (all, or all but `drop`; shuffled if asked) and
// returns the last result.
static NetStreamResult send(NetStreamReceiver* receiver, uint32_t frame, const std::vector<uint8_t>* coded,
                            size_t coded_size, bool keyframe, int32_t drop, bool shuffle)
{
    uint32_t chunks = net_stream_chunk_count(coded_size);
    std::vector<uint32_t> order;
    for (uint32_t c = 0; c < chunks; c++)
    {
        order.push_back(c);
    }
    if (shuffle)
    {
        for (uint32_t c = chunks; c > 1; c--)
        {
            uint32_t other = next_random() % c;
            uint32_t keep = order[c - 1];
            order[c - 1] = order[other];
            order[other] = keep;
        }
    }
    NetStreamResult last = NetStreamResult::nothing;
    uint8_t packet[NET_STREAM_VIDEO_HEADER + NET_STREAM_CHUNK];
    for (uint32_t i = 0; i < chunks; i++)
    {
        if (static_cast<int32_t>(order[i]) == drop)
        {
            continue;
        }
        size_t size = net_stream_write_video(packet, sizeof(packet), frame, keyframe, coded->data(), coded_size, order[i]);
        NetStreamResult result = net_stream_receive_video(receiver, packet, size);
        if (result != NetStreamResult::nothing)
        {
            last = result;
        }
    }
    return last;
}

int main()
{
    FrameDeltaEncoder encoder;
    NetStreamReceiver receiver;
    std::vector<uint8_t> pixels;
    std::vector<uint8_t> coded(frame_delta_max_size(256, 224, FramePixelFormat::rgb565));

    picture(&pixels, 0);
    size_t size = frame_delta_encode(&encoder, pixels.data(), 256, 224, FramePixelFormat::rgb565, false, coded.data());
    check(net_stream_chunk_count(size) > 10, "a noisy keyframe takes many chunks");
    check(send(&receiver, 1, &coded, size, true, -1, false) == NetStreamResult::frame && receiver.decoder.frame == pixels,
          "a keyframe arrives whole");

    picture(&pixels, 1);
    size = frame_delta_encode(&encoder, pixels.data(), 256, 224, FramePixelFormat::rgb565, false, coded.data());
    check(send(&receiver, 2, &coded, size, false, -1, true) == NetStreamResult::frame && receiver.decoder.frame == pixels,
          "a delta's chunks shuffled: still whole");

    // Frame 3 loses a chunk; frame 4 (a delta) starts arriving: 3 is lost.
    picture(&pixels, 2);
    size = frame_delta_encode(&encoder, pixels.data(), 256, 224, FramePixelFormat::rgb565, false, coded.data());
    send(&receiver, 3, &coded, size, false, 2, false);
    picture(&pixels, 3);
    size = frame_delta_encode(&encoder, pixels.data(), 256, 224, FramePixelFormat::rgb565, false, coded.data());
    receiver.asked = false;
    NetStreamResult result = send(&receiver, 4, &coded, size, false, -1, false);
    check(result == NetStreamResult::lost && receiver.asked && receiver.need_keyframe,
          "a lost chunk loses the frame, and the delta after it can't be used: keyframe asked");
    check(receiver.lost >= 2, "both counted");

    // The host sends a keyframe: back on track.
    picture(&pixels, 4);
    size = frame_delta_encode(&encoder, pixels.data(), 256, 224, FramePixelFormat::rgb565, true, coded.data());
    check(send(&receiver, 5, &coded, size, true, -1, false) == NetStreamResult::frame && receiver.decoder.frame == pixels &&
              !receiver.need_keyframe,
          "a keyframe recovers");
    // A late chunk of an old frame changes nothing.
    uint8_t late[NET_STREAM_VIDEO_HEADER + NET_STREAM_CHUNK];
    size_t late_size = net_stream_write_video(late, sizeof(late), 3, false, coded.data(), size, 0);
    check(net_stream_receive_video(&receiver, late, late_size) == NetStreamResult::nothing, "a late chunk is ignored");

    // Audio: a frame's samples in pieces, each standing alone.
    std::vector<int16_t> samples(534 * 2);
    for (size_t i = 0; i < samples.size(); i++)
    {
        samples[i] = static_cast<int16_t>(i * 13 - 3000);
    }
    std::vector<int16_t> back(samples.size());
    std::vector<int16_t> storage;
    uint8_t audio_packet[NET_STREAM_AUDIO_HEADER + NET_STREAM_AUDIO_PIECE * 4];
    bool audio_ok = true;
    for (uint32_t first = 0; first < 534; first += NET_STREAM_AUDIO_PIECE)
    {
        uint32_t count = 534 - first < NET_STREAM_AUDIO_PIECE ? 534 - first : NET_STREAM_AUDIO_PIECE;
        size_t audio_size =
            net_stream_write_audio(audio_packet, sizeof(audio_packet), 9, 32040.5, samples.data(), first, count);
        NetStreamAudio audio;
        audio_ok = audio_ok && net_stream_read_audio(audio_packet, audio_size, &audio, &storage) && audio.frame == 9 &&
                   audio.first == first && audio.count == count && audio.sample_rate > 32040.4 &&
                   audio.sample_rate < 32040.6;
        if (audio_ok)
        {
            std::memcpy(back.data() + first * 2, audio.samples, count * 4);
        }
    }
    check(audio_ok && back == samples, "audio pieces read back");
    check(net_stream_receive_video(&receiver, audio_packet, NET_STREAM_AUDIO_HEADER + 4) == NetStreamResult::nothing,
          "an audio packet isn't video");

    NetStreamInfo info;
    info.width = 256;
    info.height = 224;
    info.format = FramePixelFormat::rgb565;
    info.aspect = 4.0 / 3.0;
    info.fps = 60.098811862348;
    info.sample_rate = 32040.5;
    uint8_t out[128];
    NetStreamInfo read;
    size_t info_size = net_stream_write_info(out, sizeof(out), &info);
    check(net_stream_read_info(out, info_size, &read) && read.width == 256 && read.height == 224 &&
              read.aspect > 1.3333 && read.aspect < 1.3334 && read.fps > 60.0988 && read.fps < 60.0989,
          "stream_info read");
    uint8_t flags = 0;
    size_t start_size = net_stream_write_start(out, sizeof(out), NET_STREAM_WANT_VIDEO | NET_STREAM_WANT_AUDIO);
    check(net_stream_read_start(out, start_size, &flags) && flags == 3, "stream_start read");

    if (g_failures == 0)
    {
        std::printf("net stream: all checks passed\n");
        return 0;
    }
    std::printf("%d check(s) failed\n", g_failures);
    return 1;
}
