#pragma once

// The knobs of the screen effect pass (screen_effect.h on Vulkan,
// gl_screen_effect.h on OpenGL ES): what a frontend sets each frame to play an
// effect on the game picture. All at rest (the defaults): no pass at all.
// A new effect adds a knob here and to both shaders.

#include <cstdint>

struct ScreenEffectParams
{
    float zoom        = 1.0f;   // >= 1: the picture zoomed in around its center
    float desaturate  = 0.0f;   // 0..1: 1 = grey
    float flash       = 0.0f;   // 0..1: how much of flash_* covers the picture
    float flash_red   = 1.0f;
    float flash_green = 1.0f;
    float flash_blue  = 1.0f;
    float bars        = 0.0f;   // 0..0.5: black bars from the top and the bottom, each this much of the height
};

// True when the knobs change nothing (the pass can be skipped).
bool screen_effect_params_at_rest(const ScreenEffectParams* params);

// A picture read back from the GPU (a GPU screenshot): rows top first, 4
// bytes a pixel. Owned: free with video_capture_free().
enum class VideoCaptureFormat
{
    bgra8,   // B, G, R, A bytes (Vulkan's B8G8R8A8)
    rgba8,   // R, G, B, A bytes (OpenGL ES)
};

struct VideoCapture
{
    uint8_t*           pixels = nullptr;
    uint32_t           width  = 0;
    uint32_t           height = 0;
    uint32_t           pitch  = 0;   // bytes a row
    VideoCaptureFormat format = VideoCaptureFormat::rgba8;
};

void video_capture_free(VideoCapture* capture);

// Where a capture is (both APIs).
enum class ScreenEffectCaptureState
{
    none,
    requested,   // the next record copies the image
    recorded,    // copied in frame `capture_serial`; waiting for that frame's fence
    ready,       // read: screen_effect_capture_take hands it out
};

