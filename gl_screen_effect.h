#pragma once

// screen_effect.h on OpenGL ES: the picture as it would be drawn (the
// chain's output, the picture's copy or the core's texture, sampled the same
// way) drawn again with the effect's knobs into a render target of the
// picture's size on screen, which ImGui draws 1:1 instead. Only on frames
// with an effect or a capture.
//
// A capture (a GPU screenshot) reads that target back:
//   GLES 3      glReadPixels into a pixel-pack buffer plus a fence; the
//               buffer is mapped once the fence has signaled (a frame or
//               two later), so nothing waits for the GPU.
//   GLES 2 and  glReadPixels straight into memory: it waits for the GPU
//   the browser (one frame's stall, only when a screenshot is taken). GLES 2
//               has no pixel-pack buffers; WebGL 2 has no glMapBufferRange.
//
// Each frame, in video_output's order (the same as screen_effect.h's):
//   gl_screen_effect_prepare(), gl_screen_effect_draw(),
//   gl_screen_effect_collect() (every frame), gl_screen_effect_record().

#include <cstdint>

#include "gl_context.h"
#include "screen_effect_params.h"
#include "viewport.h"

struct ImDrawList;

struct GlScreenEffectSource
{
    GLuint texture = 0;
    bool   linear  = false;
    float  rect[4] = {0.0f, 0.0f, 1.0f, 1.0f};   // the picture in it: x, y, width, height (texture coordinates)
};

struct GlScreenEffect
{
    GLuint program       = 0;
    GLint  source        = -1;   // uniform locations
    GLint  source_rect   = -1;
    GLint  knobs         = -1;
    GLint  flash_color   = -1;
    bool   failed        = false;   // the program didn't build: never tried again

    GLuint   texture     = 0;   // the target
    GLuint   framebuffer = 0;
    uint32_t width       = 0;
    uint32_t height      = 0;

    ViewportResult viewport = {};   // what this frame draws (set by prepare)
    bool           ready    = false;

    ScreenEffectCaptureState capture_state = ScreenEffectCaptureState::none;
    GLuint       pack_buffer    = 0;         // GLES 3
    GlSync       fence          = nullptr;   // GLES 3: signaled when the read into pack_buffer is done
    uint32_t     capture_width  = 0;
    uint32_t     capture_height = 0;
    VideoCapture captured       = {};
};

void gl_screen_effect_destroy(GlScreenEffect* effect, GlContext* context);
bool gl_screen_effect_prepare(GlScreenEffect* effect, GlContext* context, const ViewportResult* viewport);
void gl_screen_effect_draw(const GlScreenEffect* effect, ImDrawList* draw_list, float origin_x, float origin_y);
bool gl_screen_effect_request_capture(GlScreenEffect* effect);
void gl_screen_effect_collect(GlScreenEffect* effect, GlContext* context);
void gl_screen_effect_record(GlScreenEffect* effect, GlContext* context, const GlScreenEffectSource* source,
                             const ScreenEffectParams* params);
bool gl_screen_effect_capture_take(GlScreenEffect* effect, VideoCapture* capture);
