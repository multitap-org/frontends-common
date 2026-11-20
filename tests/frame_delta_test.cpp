// Checks frame_delta.h: every frame decodes to exactly what was coded
// (each pixel format, sizes that aren't multiples of the tile, random
// pictures and pictures that change a little), deltas are small when little
// changed, and damaged or out-of-order frames are refused.
//
// Exit code 0 = all checks passed. Each failure is printed.

#include <cstdio>
#include <cstring>
#include <vector>

#include "frame_delta.h"

static int g_failures = 0;

static void check(bool condition, const char* what)
{
    if (!condition)
    {
        std::printf("FAIL %s\n", what);
        g_failures++;
    }
}

static uint32_t g_random = 12345;

static uint32_t next_random()
{
    g_random = g_random * 1103515245u + 12345u;
    return g_random >> 8;
}

// A "retro" picture: a few colors in blocks, plus some noise.
static void make_picture(std::vector<uint8_t>* pixels, uint32_t width, uint32_t height, uint32_t bytes, uint32_t seed,
                         bool noisy)
{
    pixels->resize(static_cast<size_t>(width) * height * bytes);
    uint32_t palette[6] = {0x0000, 0xF800, 0x07E0, 0x001F, 0xFFFF, 0x8410};
    for (uint32_t y = 0; y < height; y++)
    {
        for (uint32_t x = 0; x < width; x++)
        {
            uint32_t value = palette[((x / 5 + y / 3 + seed) % 6)];
            if (bytes == 4)
            {
                value = value * 0x10001u;
            }
            if (noisy && next_random() % 7 == 0)
            {
                value = next_random();
            }
            uint8_t* at = pixels->data() + (static_cast<size_t>(y) * width + x) * bytes;
            for (uint32_t b = 0; b < bytes; b++)
            {
                at[b] = static_cast<uint8_t>(value >> (8 * b));
            }
        }
    }
}

static bool round_trip(FrameDeltaEncoder* encoder, FrameDeltaDecoder* decoder, const std::vector<uint8_t>* pixels,
                       uint32_t width, uint32_t height, FramePixelFormat format, bool keyframe, size_t* coded_size)
{
    std::vector<uint8_t> coded(frame_delta_max_size(width, height, format));
    size_t size = frame_delta_encode(encoder, pixels->data(), width, height, format, keyframe, coded.data());
    *coded_size = size;
    return size > 0 && frame_delta_decode(decoder, coded.data(), size) && decoder->frame == *pixels;
}

int main()
{
    const FramePixelFormat formats[3] = {FramePixelFormat::rgb565, FramePixelFormat::xrgb1555,
                                         FramePixelFormat::xrgb8888};
    const uint32_t sizes[4][2] = {{256, 224}, {257, 223}, {7, 5}, {320, 240}};
    for (uint32_t f = 0; f < 3; f++)
    {
        uint32_t bytes = frame_pixel_bytes(formats[f]);
        for (uint32_t s = 0; s < 4; s++)
        {
            uint32_t width = sizes[s][0];
            uint32_t height = sizes[s][1];
            FrameDeltaEncoder encoder;
            FrameDeltaDecoder decoder;
            std::vector<uint8_t> picture;
            size_t size = 0;
            make_picture(&picture, width, height, bytes, 0, true);
            bool ok = round_trip(&encoder, &decoder, &picture, width, height, formats[f], false, &size);
            // A few frames that change: a moving block, then noise.
            for (uint32_t frame = 1; frame < 20 && ok; frame++)
            {
                std::vector<uint8_t> next = picture;
                for (uint32_t y = frame % height; y < height && y < frame % height + 4; y++)
                {
                    for (uint32_t x = 0; x < width && x < 10; x++)
                    {
                        next[(static_cast<size_t>(y) * width + x) * bytes] ^= 0x5A;
                    }
                }
                if (frame % 7 == 0)
                {
                    make_picture(&next, width, height, bytes, frame, true);
                }
                ok = round_trip(&encoder, &decoder, &next, width, height, formats[f], frame % 11 == 0, &size);
                picture = next;
            }
            check(ok, "frames decode exactly (each format and size)");
        }
    }

    // Small when little changed; nothing changed = header + map only.
    FrameDeltaEncoder encoder;
    FrameDeltaDecoder decoder;
    std::vector<uint8_t> picture;
    make_picture(&picture, 256, 224, 2, 0, false);
    size_t key_size = 0;
    size_t size = 0;
    check(round_trip(&encoder, &decoder, &picture, 256, 224, FramePixelFormat::rgb565, false, &key_size), "keyframe");
    // Diagonal stripes changing every 5 pixels: harder than most games.
    check(key_size < 256 * 224 * 2 / 3, "a blocky picture codes well below its raw size");
    check(round_trip(&encoder, &decoder, &picture, 256, 224, FramePixelFormat::rgb565, false, &size) &&
              size == FRAME_DELTA_HEADER_SIZE + (32 * 28 + 7) / 8,
          "an unchanged frame: header and map only");
    std::vector<uint8_t> sprite = picture;
    for (uint32_t y = 100; y < 116; y++)
    {
        for (uint32_t x = 120; x < 136; x++)
        {
            sprite[(static_cast<size_t>(y) * 256 + x) * 2] = 0x42;
        }
    }
    // Over six tiles (it isn't aligned to them): their other pixels go as SAME.
    check(round_trip(&encoder, &decoder, &sprite, 256, 224, FramePixelFormat::rgb565, false, &size) && size < 400,
          "a 16x16 sprite moving: a small delta");

    // A delta needs the frame it was made against.
    std::vector<uint8_t> coded(frame_delta_max_size(256, 224, FramePixelFormat::rgb565));
    size = frame_delta_encode(&encoder, picture.data(), 256, 224, FramePixelFormat::rgb565, false, coded.data());
    FrameDeltaDecoder fresh;
    check(!frame_delta_is_keyframe(coded.data(), size) && !frame_delta_decode(&fresh, coded.data(), size),
          "a delta without its frame is refused");
    check(!frame_delta_decode(&decoder, coded.data(), size - 1), "a cut frame is refused");
    // Another size: a keyframe whatever was asked.
    std::vector<uint8_t> wide;
    make_picture(&wide, 512, 224, 2, 3, false);
    std::vector<uint8_t> coded_wide(frame_delta_max_size(512, 224, FramePixelFormat::rgb565));
    size = frame_delta_encode(&encoder, wide.data(), 512, 224, FramePixelFormat::rgb565, false, coded_wide.data());
    check(frame_delta_is_keyframe(coded_wide.data(), size), "a size change makes a keyframe");
    check(frame_delta_decode(&fresh, coded_wide.data(), size) && fresh.frame == wide, "which any decoder takes");
    check(frame_delta_encode(&encoder, wide.data(), 0, 224, FramePixelFormat::rgb565, false, coded_wide.data()) == 0,
          "width 0 refused");

    if (g_failures == 0)
    {
        std::printf("frame delta: all checks passed\n");
        return 0;
    }
    std::printf("%d check(s) failed\n", g_failures);
    return 1;
}
