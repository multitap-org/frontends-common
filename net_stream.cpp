#include "net_stream.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "net_wire.h"

// ---------------------------------------------------------------------------
// Control
// ---------------------------------------------------------------------------

size_t net_stream_write_start(uint8_t* out, size_t capacity, uint8_t flags)
{
    if (capacity < 2)
    {
        return 0;
    }
    out[0] = NET_STREAM_START;
    out[1] = flags;
    return 2;
}

bool net_stream_read_start(const uint8_t* data, size_t size, uint8_t* flags)
{
    if (size != 2 || data[0] != NET_STREAM_START || data[1] > 3)
    {
        return false;
    }
    *flags = data[1];
    return true;
}

static void put_double(NetWriter* writer, double value)
{
    char text[40];
    std::snprintf(text, sizeof(text), "%.9g", value);
    net_wire_put_text(writer, text, std::strlen(text));
}

static double get_double(NetReader* reader)
{
    std::string text;
    net_wire_get_text(reader, &text);
    return std::strtod(text.c_str(), nullptr);
}

size_t net_stream_write_info(uint8_t* out, size_t capacity, const NetStreamInfo* info)
{
    NetWriter writer = {out, capacity, 0, true};
    net_wire_put_u8(&writer, NET_STREAM_INFO);
    net_wire_put_u16(&writer, static_cast<uint16_t>(info->width));
    net_wire_put_u16(&writer, static_cast<uint16_t>(info->height));
    net_wire_put_u8(&writer, static_cast<uint8_t>(info->format));
    put_double(&writer, info->aspect);
    put_double(&writer, info->fps);
    put_double(&writer, info->sample_rate);
    return writer.ok ? writer.size : 0;
}

bool net_stream_read_info(const uint8_t* data, size_t size, NetStreamInfo* info)
{
    NetReader reader;
    reader.data = data;
    reader.size = size;
    if (net_wire_get_u8(&reader) != NET_STREAM_INFO)
    {
        return false;
    }
    info->width = net_wire_get_u16(&reader);
    info->height = net_wire_get_u16(&reader);
    uint8_t format = net_wire_get_u8(&reader);
    info->aspect = get_double(&reader);
    info->fps = get_double(&reader);
    info->sample_rate = get_double(&reader);
    if (!net_wire_done(&reader) || format >= FRAME_PIXEL_FORMAT_COUNT)
    {
        return false;
    }
    info->format = static_cast<FramePixelFormat>(format);
    return true;
}

size_t net_stream_write_tuning(uint8_t* out, size_t capacity, uint8_t type, const NetStreamTuning* tuning)
{
    if (capacity < 5)
    {
        return 0;
    }
    out[0] = type;
    out[1] = static_cast<uint8_t>(tuning->delay);
    out[2] = tuning->delay_ms;
    out[3] = tuning->run_ahead;
    out[4] = tuning->flags;
    return type == NET_STREAM_TUNING ? 4 : 5;
}

bool net_stream_read_tuning(const uint8_t* data, size_t size, uint8_t type, NetStreamTuning* tuning)
{
    size_t expected = type == NET_STREAM_TUNING ? 4 : 5;
    if (size != expected || data[0] != type || data[1] >= NET_STREAM_DELAY_COUNT)
    {
        return false;
    }
    tuning->delay = static_cast<NetStreamDelay>(data[1]);
    tuning->delay_ms = data[2] < NET_STREAM_MAX_DELAY_MS ? data[2] : NET_STREAM_MAX_DELAY_MS;
    tuning->run_ahead = data[3] < NET_STREAM_MAX_RUN_AHEAD ? data[3] : NET_STREAM_MAX_RUN_AHEAD;
    tuning->flags = type == NET_STREAM_TUNING ? 0 : data[4];
    return true;
}

// ---------------------------------------------------------------------------
// Sending
// ---------------------------------------------------------------------------

static void put32(uint8_t* out, uint32_t value)
{
    for (uint32_t i = 0; i < 4; i++)
    {
        out[i] = static_cast<uint8_t>(value >> (8 * i));
    }
}

static void put16(uint8_t* out, uint32_t value)
{
    out[0] = static_cast<uint8_t>(value);
    out[1] = static_cast<uint8_t>(value >> 8);
}

static uint32_t get32(const uint8_t* data)
{
    return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
           (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
}

static uint32_t get16(const uint8_t* data)
{
    return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8);
}

uint32_t net_stream_chunk_count(size_t size)
{
    return size == 0 ? 1u : static_cast<uint32_t>((size + NET_STREAM_CHUNK - 1) / NET_STREAM_CHUNK);
}

size_t net_stream_write_video(uint8_t* out, size_t capacity, uint32_t frame, bool keyframe, const uint8_t* coded,
                              size_t coded_size, uint32_t chunk)
{
    uint32_t chunks = net_stream_chunk_count(coded_size);
    if (chunk >= chunks || chunks > NET_STREAM_MAX_CHUNKS)
    {
        return 0;
    }
    size_t offset = static_cast<size_t>(chunk) * NET_STREAM_CHUNK;
    size_t length = coded_size - offset < NET_STREAM_CHUNK ? coded_size - offset : NET_STREAM_CHUNK;
    if (capacity < NET_STREAM_VIDEO_HEADER + length)
    {
        return 0;
    }
    out[0] = NET_STREAM_VIDEO;
    put32(out + 1, frame);
    out[5] = keyframe ? 1 : 0;
    put16(out + 6, chunk);
    put16(out + 8, chunks);
    std::memcpy(out + NET_STREAM_VIDEO_HEADER, coded + offset, length);
    return NET_STREAM_VIDEO_HEADER + length;
}

size_t net_stream_write_audio(uint8_t* out, size_t capacity, uint32_t frame, double sample_rate,
                              const int16_t* samples, uint32_t first, uint32_t count)
{
    size_t size = NET_STREAM_AUDIO_HEADER + static_cast<size_t>(count) * 4;
    if (count == 0 || count > NET_STREAM_AUDIO_PIECE || first > 0xFFFF || capacity < size)
    {
        return 0;
    }
    out[0] = NET_STREAM_AUDIO;
    put32(out + 1, frame);
    put32(out + 5, static_cast<uint32_t>(sample_rate * 1000.0 + 0.5));
    put16(out + 9, first);
    put16(out + 11, count);
    uint8_t* at = out + NET_STREAM_AUDIO_HEADER;
    for (uint32_t i = 0; i < count * 2; i++)
    {
        put16(at + 2 * i, static_cast<uint16_t>(samples[static_cast<size_t>(first) * 2 + i]));
    }
    return size;
}

// ---------------------------------------------------------------------------
// Receiving
// ---------------------------------------------------------------------------

bool net_stream_read_audio(const uint8_t* data, size_t size, NetStreamAudio* audio, std::vector<int16_t>* storage)
{
    if (size < NET_STREAM_AUDIO_HEADER || data[0] != NET_STREAM_AUDIO)
    {
        return false;
    }
    uint32_t count = get16(data + 11);
    if (count == 0 || count > NET_STREAM_AUDIO_PIECE || size != NET_STREAM_AUDIO_HEADER + static_cast<size_t>(count) * 4)
    {
        return false;
    }
    audio->frame = get32(data + 1);
    audio->sample_rate = static_cast<double>(get32(data + 5)) / 1000.0;
    audio->first = get16(data + 9);
    audio->count = count;
    storage->resize(static_cast<size_t>(count) * 2);
    for (uint32_t i = 0; i < count * 2; i++)
    {
        (*storage)[i] = static_cast<int16_t>(get16(data + NET_STREAM_AUDIO_HEADER + 2 * i));
    }
    audio->samples = storage->data();
    return true;
}

// Is frame number `a` after `b`? (They wrap after 2^32.)
static bool newer(uint32_t a, uint32_t b)
{
    return static_cast<int32_t>(a - b) > 0;
}

static NetStreamResult lose(NetStreamReceiver* receiver)
{
    receiver->lost++;
    receiver->need_keyframe = true;
    receiver->asked = true;
    return NetStreamResult::lost;
}

NetStreamResult net_stream_receive_video(NetStreamReceiver* receiver, const uint8_t* data, size_t size)
{
    if (size < NET_STREAM_VIDEO_HEADER || data[0] != NET_STREAM_VIDEO || data[5] > 1)
    {
        return NetStreamResult::nothing;
    }
    uint32_t frame = get32(data + 1);
    bool keyframe = data[5] == 1;
    uint32_t chunk = get16(data + 6);
    uint32_t chunks = get16(data + 8);
    size_t length = size - NET_STREAM_VIDEO_HEADER;
    if (chunks == 0 || chunks > NET_STREAM_MAX_CHUNKS || chunk >= chunks || length > NET_STREAM_CHUNK ||
        (chunk + 1 < chunks && length != NET_STREAM_CHUNK))
    {
        return NetStreamResult::nothing;
    }
    NetStreamResult result = NetStreamResult::nothing;
    if (receiver->assembling && frame != receiver->frame)
    {
        if (!newer(frame, receiver->frame))
        {
            return NetStreamResult::nothing;   // a late piece of a frame already given up
        }
        receiver->assembling = false;          // the one being put together won't finish
        result = lose(receiver);
    }
    if (!receiver->assembling)
    {
        if (receiver->shown > 0 && !newer(frame, receiver->last_shown))
        {
            return result;   // a late piece of a frame already shown
        }
        receiver->assembling = true;
        receiver->frame = frame;
        receiver->keyframe = keyframe;
        receiver->chunks = chunks;
        receiver->received = 0;
        receiver->have.assign(chunks, 0);
        receiver->coded.resize(static_cast<size_t>(chunks) * NET_STREAM_CHUNK);
        receiver->coded_size = 0;
    }
    if (chunks != receiver->chunks || receiver->have[chunk] != 0)
    {
        return result;   // inconsistent, or a duplicate
    }
    std::memcpy(receiver->coded.data() + static_cast<size_t>(chunk) * NET_STREAM_CHUNK, data + NET_STREAM_VIDEO_HEADER,
                length);
    receiver->have[chunk] = 1;
    receiver->received++;
    if (chunk + 1 == chunks)
    {
        receiver->coded_size = static_cast<size_t>(chunk) * NET_STREAM_CHUNK + length;
    }
    if (receiver->received < chunks)
    {
        return result;
    }
    receiver->assembling = false;
    if (receiver->need_keyframe && !receiver->keyframe)
    {
        return lose(receiver);   // a delta against a frame we don't have
    }
    if (!frame_delta_decode(&receiver->decoder, receiver->coded.data(), receiver->coded_size))
    {
        return lose(receiver);
    }
    receiver->need_keyframe = false;
    receiver->shown++;
    receiver->last_shown = frame;
    return NetStreamResult::frame;
}
