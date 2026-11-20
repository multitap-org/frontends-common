#include "frame_delta.h"

#include <cstring>

constexpr uint8_t OP_INDEX = 0x00;
constexpr uint8_t OP_RUN = 0x40;
constexpr uint8_t OP_SAME = 0x80;
constexpr uint8_t OP_RAW = 0xC0;
constexpr uint8_t OP_MASK = 0xC0;
constexpr uint32_t OP_MAX_COUNT = 64;
constexpr uint32_t TABLE_SIZE = 64;

uint32_t frame_pixel_bytes(FramePixelFormat format)
{
    return format == FramePixelFormat::xrgb8888 ? 4u : 2u;
}

static uint32_t tiles_across(uint32_t side)
{
    return (side + FRAME_DELTA_TILE - 1) / FRAME_DELTA_TILE;
}

size_t frame_delta_max_size(uint32_t width, uint32_t height, FramePixelFormat format)
{
    size_t tiles = static_cast<size_t>(tiles_across(width)) * tiles_across(height);
    size_t pixels = static_cast<size_t>(width) * height;
    return FRAME_DELTA_HEADER_SIZE + (tiles + 7) / 8 + pixels * (1 + frame_pixel_bytes(format));
}

static uint32_t read_pixel(const uint8_t* at, uint32_t bytes)
{
    uint32_t value = static_cast<uint32_t>(at[0]) | (static_cast<uint32_t>(at[1]) << 8);
    if (bytes == 4)
    {
        value |= (static_cast<uint32_t>(at[2]) << 16) | (static_cast<uint32_t>(at[3]) << 24);
    }
    return value;
}

static void write_pixel(uint8_t* at, uint32_t bytes, uint32_t value)
{
    at[0] = static_cast<uint8_t>(value);
    at[1] = static_cast<uint8_t>(value >> 8);
    if (bytes == 4)
    {
        at[2] = static_cast<uint8_t>(value >> 16);
        at[3] = static_cast<uint8_t>(value >> 24);
    }
}

// The recent-colors slot of a pixel.
static uint32_t slot_of(uint32_t pixel)
{
    return (pixel * 2654435761u) >> 26;
}

static bool tile_changed(const uint8_t* now, const uint8_t* before, size_t row_bytes, uint32_t x, uint32_t y,
                         uint32_t tile_width, uint32_t tile_height, uint32_t bytes)
{
    for (uint32_t row = 0; row < tile_height; row++)
    {
        size_t offset = (static_cast<size_t>(y) + row) * row_bytes + static_cast<size_t>(x) * bytes;
        if (std::memcmp(now + offset, before + offset, static_cast<size_t>(tile_width) * bytes) != 0)
        {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Encoding
// ---------------------------------------------------------------------------

enum class PendingOp
{
    none,
    run,
    same,
};

struct Coder
{
    uint8_t*  out = nullptr;
    size_t    at = 0;
    uint32_t  bytes = 2;
    uint32_t  table[TABLE_SIZE] = {};
    uint32_t  last = 0;
    PendingOp pending = PendingOp::none;
    uint32_t  count = 0;
};

static void flush(Coder* coder)
{
    if (coder->pending == PendingOp::run)
    {
        coder->out[coder->at++] = static_cast<uint8_t>(OP_RUN | (coder->count - 1));
    }
    else if (coder->pending == PendingOp::same)
    {
        coder->out[coder->at++] = static_cast<uint8_t>(OP_SAME | (coder->count - 1));
    }
    coder->pending = PendingOp::none;
    coder->count = 0;
}

static void extend(Coder* coder, PendingOp op)
{
    if (coder->pending != op || coder->count == OP_MAX_COUNT)
    {
        flush(coder);
        coder->pending = op;
    }
    coder->count++;
}

// One pixel; `same` = it's what the previous frame had there (deltas only).
static void code_pixel(Coder* coder, uint32_t pixel, bool same)
{
    bool run = pixel == coder->last;
    // Keep going with the op under way when the pixel allows it.
    if (coder->pending == PendingOp::same && same)
    {
        extend(coder, PendingOp::same);
        coder->table[slot_of(pixel)] = pixel;
    }
    else if (coder->pending == PendingOp::run && run)
    {
        extend(coder, PendingOp::run);
    }
    else if (same)
    {
        extend(coder, PendingOp::same);
        coder->table[slot_of(pixel)] = pixel;
    }
    else if (run)
    {
        extend(coder, PendingOp::run);
    }
    else
    {
        flush(coder);
        uint32_t slot = slot_of(pixel);
        if (coder->table[slot] == pixel)
        {
            coder->out[coder->at++] = static_cast<uint8_t>(OP_INDEX | slot);
        }
        else
        {
            coder->out[coder->at++] = OP_RAW;
            write_pixel(coder->out + coder->at, coder->bytes, pixel);
            coder->at += coder->bytes;
            coder->table[slot] = pixel;
        }
    }
    coder->last = pixel;
}

size_t frame_delta_encode(FrameDeltaEncoder* encoder, const uint8_t* pixels, uint32_t width, uint32_t height,
                          FramePixelFormat format, bool keyframe, uint8_t* out)
{
    if (width == 0 || height == 0 || width > FRAME_DELTA_MAX_SIDE || height > FRAME_DELTA_MAX_SIDE ||
        static_cast<uint8_t>(format) >= FRAME_PIXEL_FORMAT_COUNT)
    {
        return 0;
    }
    uint32_t bytes = frame_pixel_bytes(format);
    bool key = keyframe || !encoder->has_previous || width != encoder->width || height != encoder->height ||
               format != encoder->format;
    out[0] = key ? 1 : 0;
    out[1] = static_cast<uint8_t>(format);
    out[2] = static_cast<uint8_t>(width);
    out[3] = static_cast<uint8_t>(width >> 8);
    out[4] = static_cast<uint8_t>(height);
    out[5] = static_cast<uint8_t>(height >> 8);

    uint32_t tiles_x = tiles_across(width);
    uint32_t tiles_y = tiles_across(height);
    size_t map_size = key ? 0 : (static_cast<size_t>(tiles_x) * tiles_y + 7) / 8;
    uint8_t* map = out + FRAME_DELTA_HEADER_SIZE;
    std::memset(map, 0, map_size);

    Coder coder;
    coder.out = out;
    coder.at = FRAME_DELTA_HEADER_SIZE + map_size;
    coder.bytes = bytes;
    size_t row_bytes = static_cast<size_t>(width) * bytes;
    const uint8_t* before = key ? nullptr : encoder->previous.data();
    uint32_t tile = 0;
    for (uint32_t ty = 0; ty < tiles_y; ty++)
    {
        for (uint32_t tx = 0; tx < tiles_x; tx++, tile++)
        {
            uint32_t x = tx * FRAME_DELTA_TILE;
            uint32_t y = ty * FRAME_DELTA_TILE;
            uint32_t tile_width = width - x < FRAME_DELTA_TILE ? width - x : FRAME_DELTA_TILE;
            uint32_t tile_height = height - y < FRAME_DELTA_TILE ? height - y : FRAME_DELTA_TILE;
            if (!key)
            {
                if (!tile_changed(pixels, before, row_bytes, x, y, tile_width, tile_height, bytes))
                {
                    continue;
                }
                map[tile / 8] = static_cast<uint8_t>(map[tile / 8] | (1u << (tile % 8)));
            }
            for (uint32_t row = 0; row < tile_height; row++)
            {
                size_t offset = (static_cast<size_t>(y) + row) * row_bytes + static_cast<size_t>(x) * bytes;
                for (uint32_t column = 0; column < tile_width; column++)
                {
                    size_t at = offset + static_cast<size_t>(column) * bytes;
                    uint32_t pixel = read_pixel(pixels + at, bytes);
                    bool same = !key && read_pixel(before + at, bytes) == pixel;
                    code_pixel(&coder, pixel, same);
                }
            }
        }
    }
    flush(&coder);

    // This frame is what the next delta is made against.
    size_t frame_bytes = row_bytes * height;
    encoder->previous.resize(frame_bytes);
    std::memcpy(encoder->previous.data(), pixels, frame_bytes);
    encoder->width = width;
    encoder->height = height;
    encoder->format = format;
    encoder->has_previous = true;
    return coder.at;
}

// ---------------------------------------------------------------------------
// Decoding
// ---------------------------------------------------------------------------

bool frame_delta_is_keyframe(const uint8_t* data, size_t size)
{
    return size >= FRAME_DELTA_HEADER_SIZE && (data[0] & 1) != 0;
}

bool frame_delta_decode(FrameDeltaDecoder* decoder, const uint8_t* data, size_t size)
{
    if (size < FRAME_DELTA_HEADER_SIZE || data[0] > 1 || data[1] >= FRAME_PIXEL_FORMAT_COUNT)
    {
        return false;
    }
    bool key = data[0] == 1;
    FramePixelFormat format = static_cast<FramePixelFormat>(data[1]);
    uint32_t width = static_cast<uint32_t>(data[2]) | (static_cast<uint32_t>(data[3]) << 8);
    uint32_t height = static_cast<uint32_t>(data[4]) | (static_cast<uint32_t>(data[5]) << 8);
    if (width == 0 || height == 0 || width > FRAME_DELTA_MAX_SIDE || height > FRAME_DELTA_MAX_SIDE)
    {
        return false;
    }
    if (!key && (!decoder->has_frame || width != decoder->width || height != decoder->height ||
                 format != decoder->format))
    {
        return false;   // a delta against a frame we don't have: a keyframe is needed
    }
    uint32_t bytes = frame_pixel_bytes(format);
    size_t row_bytes = static_cast<size_t>(width) * bytes;
    if (key)
    {
        decoder->frame.resize(row_bytes * height);
        decoder->width = width;
        decoder->height = height;
        decoder->format = format;
        decoder->has_frame = true;
    }
    uint32_t tiles_x = tiles_across(width);
    uint32_t tiles_y = tiles_across(height);
    size_t map_size = key ? 0 : (static_cast<size_t>(tiles_x) * tiles_y + 7) / 8;
    if (size < FRAME_DELTA_HEADER_SIZE + map_size)
    {
        return false;
    }
    const uint8_t* map = data + FRAME_DELTA_HEADER_SIZE;
    size_t at = FRAME_DELTA_HEADER_SIZE + map_size;
    uint8_t* frame = decoder->frame.data();
    uint32_t table[TABLE_SIZE] = {};
    uint32_t last = 0;
    PendingOp pending = PendingOp::none;
    uint32_t remaining = 0;
    uint32_t tile = 0;
    for (uint32_t ty = 0; ty < tiles_y; ty++)
    {
        for (uint32_t tx = 0; tx < tiles_x; tx++, tile++)
        {
            if (!key && (map[tile / 8] & (1u << (tile % 8))) == 0)
            {
                continue;
            }
            uint32_t x = tx * FRAME_DELTA_TILE;
            uint32_t y = ty * FRAME_DELTA_TILE;
            uint32_t tile_width = width - x < FRAME_DELTA_TILE ? width - x : FRAME_DELTA_TILE;
            uint32_t tile_height = height - y < FRAME_DELTA_TILE ? height - y : FRAME_DELTA_TILE;
            for (uint32_t row = 0; row < tile_height; row++)
            {
                size_t offset = (static_cast<size_t>(y) + row) * row_bytes + static_cast<size_t>(x) * bytes;
                for (uint32_t column = 0; column < tile_width; column++)
                {
                    uint8_t* pixel_at = frame + offset + static_cast<size_t>(column) * bytes;
                    if (remaining == 0)
                    {
                        if (at >= size)
                        {
                            return false;
                        }
                        uint8_t op = data[at++];
                        if ((op & OP_MASK) == OP_INDEX)
                        {
                            pending = PendingOp::none;
                            last = table[op & 0x3F];
                            write_pixel(pixel_at, bytes, last);
                            continue;
                        }
                        if (op == OP_RAW)
                        {
                            if (size - at < bytes)
                            {
                                return false;
                            }
                            pending = PendingOp::none;
                            last = read_pixel(data + at, bytes);
                            at += bytes;
                            table[slot_of(last)] = last;
                            write_pixel(pixel_at, bytes, last);
                            continue;
                        }
                        if ((op & OP_MASK) == OP_RUN)
                        {
                            pending = PendingOp::run;
                        }
                        else if ((op & OP_MASK) == OP_SAME && !key)
                        {
                            pending = PendingOp::same;
                        }
                        else
                        {
                            return false;   // SAME in a keyframe, or a reserved op
                        }
                        remaining = (op & 0x3Fu) + 1;
                    }
                    if (pending == PendingOp::run)
                    {
                        write_pixel(pixel_at, bytes, last);
                    }
                    else
                    {
                        last = read_pixel(pixel_at, bytes);   // as it was: left in place
                        table[slot_of(last)] = last;
                    }
                    remaining--;
                }
            }
        }
    }
    return remaining == 0 && at == size;
}
