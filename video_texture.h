#pragma once

// A texture the CPU fills every frame and the GUI can draw: a core's picture,
// a remote play stream, a savestate thumbnail. Frontends use this instead of
// the graphics API's own texture, so they don't depend on which API the shell
// runs on: vk_texture.h's StreamingTexture on Vulkan, gl_texture.h's
// GlStreamingTexture on OpenGL ES (piccolo's docs/gl.md).
//
// The texture has a fixed capacity; uploads may be smaller (the current
// frame's size). Only the uploaded region is drawn: video_texture_uv_max().
//
// Each frame, uploads go after app_shell_acquire() succeeded and before
// app_shell_present() (they are recorded before the frame's drawing).

#include <cstdint>

#include "frame_pixel_format.h"
#include "gl_texture.h"
#include "vk_texture.h"

struct AppShell;

struct VideoTexture
{
    FramePixelFormat format          = FramePixelFormat::xrgb8888;
    uint32_t         bytes_per_pixel = 4;
    uint32_t         capacity_width  = 0;
    uint32_t         capacity_height = 0;
    uint32_t         content_width   = 0;       // size of the last upload
    uint32_t         content_height  = 0;
    bool             has_content     = false;   // false until the first upload: don't draw it before that
    uint64_t         ui_texture      = 0;       // the GUI's handle (ui_texture.h), 0 = not created

    // The API's texture underneath, the one the shell runs on (only
    // video_texture.cpp and the video modules look inside).
    StreamingTexture   vulkan = {};
    GlStreamingTexture gl     = {};
};

// True if this GPU can take `format` as it comes (no conversion on the CPU).
bool video_texture_format_supported(const AppShell* shell, FramePixelFormat format);

// Creates the texture and its GUI handle. `opaque`: alpha reads as 1 (a core's
// picture: the unused X / top bits never turn into transparency). False
// (logged) on failure; the texture is then empty.
bool video_texture_create(VideoTexture* texture, AppShell* shell, uint32_t width, uint32_t height,
                          FramePixelFormat format, bool opaque);

// True once created (and until destroyed).
bool video_texture_created(const VideoTexture* texture);

// Frees it. Waits for the GPU first (earlier frames may still draw it). Safe
// on an empty texture.
void video_texture_destroy(VideoTexture* texture, AppShell* shell);

// Copies `pixels` (width * height, tightly packed, in the texture's format)
// in. Only after app_shell_acquire() returned true this frame. False if the
// frame is bigger than the capacity.
bool video_texture_upload(VideoTexture* texture, AppShell* shell, const void* pixels, uint32_t width,
                          uint32_t height);

// Bottom-right texture coordinate of the uploaded region (top-left is 0,0).
void video_texture_uv_max(const VideoTexture* texture, float* u, float* v);
