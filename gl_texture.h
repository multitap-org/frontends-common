#pragma once

// video_texture.h on OpenGL ES. Two textures:
//
//   upload   the frame as it comes: RGB565 as is; 0RGB1555 converted to
//            RGB565 on the CPU (GLES has no 0RGB1555 layout; 5-bit green to 6
//            bits is exact); XRGB8888 as RGBA bytes (B, G, R, X in memory).
//   shown    RGBA8, the same size, written from `upload` by the copy program
//            right after each upload (red and blue swapped back for XRGB8888,
//            alpha 1). This is the one ImGui draws (and the picture copies
//            from), so every user sees correct colors whatever the format.
//
// Both are the texture's capacity; uploads fill the top-left corner. GLES 2
// allows non-power-of-two sizes with clamp-to-edge and no mipmaps, which is
// all this uses.

#include <cstdint>
#include <vector>

#include "frame_pixel_format.h"
#include "gl_context.h"

struct GlStreamingTexture
{
    GLuint upload      = 0;
    GLuint shown       = 0;
    GLuint framebuffer = 0;   // renders into `shown`
    FramePixelFormat format = FramePixelFormat::xrgb8888;
    uint32_t capacity_width  = 0;
    uint32_t capacity_height = 0;
    std::vector<uint16_t> converted;   // 0RGB1555 -> RGB565, capacity-sized (reserved at creation)
};

bool gl_streaming_texture_create(GlStreamingTexture* texture, GlContext* context, uint32_t width, uint32_t height,
                                 FramePixelFormat format);
void gl_streaming_texture_destroy(GlStreamingTexture* texture, GlContext* context);

// Uploads and converts. Leaves framebuffer 0 bound with the viewport unset
// (the frame's own drawing sets it). False if the frame is bigger than the capacity.
bool gl_streaming_texture_upload(GlStreamingTexture* texture, GlContext* context, const void* pixels, uint32_t width,
                                 uint32_t height);

// A texture with a framebuffer: RGBA8, clamp-to-edge. False (logged) if the
// GPU can't render into it.
bool gl_render_target_create(GlContext* context, uint32_t width, uint32_t height, GLuint* texture,
                             GLuint* framebuffer);
void gl_render_target_destroy(GlContext* context, GLuint* texture, GLuint* framebuffer);
