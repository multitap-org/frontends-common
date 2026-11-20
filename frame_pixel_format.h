#pragma once

// The pixel formats a core's picture comes in (libretro's three), named once
// for everything that carries or shows one: the remote play codec
// (frame_delta.h), the textures that show it (video_texture.h). The memory
// layouts are libretro's, little-endian.

#include <cstdint>

enum class FramePixelFormat : uint8_t
{
    rgb565 = 0,     // 2 bytes
    xrgb1555 = 1,   // 2 bytes (libretro's 0RGB1555)
    xrgb8888 = 2,   // 4 bytes
};
constexpr uint8_t FRAME_PIXEL_FORMAT_COUNT = 3;

uint32_t frame_pixel_bytes(FramePixelFormat format);
