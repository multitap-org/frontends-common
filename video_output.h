#pragma once

// A VideoTexture on screen: where viewport.h says, through two shader stages
// when there are any (stage 0 = the frontend's pass 0 at the picture's own
// resolution, stage 1 = the chosen preset), else plain with the viewport's
// filter. Drawn through ImGui, so it can sit behind the UI or inside a window.
// Frontends use this instead of the API's own modules, so they don't depend on
// which API the shell runs on. Vulkan: shader_chain.h (librashader, .slangp)
// and, without a chain, picture.h underneath. OpenGL ES: gl_shader_chain.h
// (our own .glpreset) and gl_picture.h. Which presets a stage takes depends
// on the API: the frontend picks the files.
//
// Each frame, in this order:
//   video_output_prepare()   while building the UI (may recreate images,
//                            which waits for the GPU; only when sizes change).
//                            Frames that show nothing call video_output_skip().
//   video_output_draw()      adds the picture to an ImGui draw list
//   video_output_record()    after app_shell_acquire() and the texture's
//                            upload, before app_shell_present()
//
// Stages are loaded and changed between frames.
//
// Screen effects (screen_effect_params.h): the frontend sets this frame's
// knobs with video_output_set_effect() before video_output_prepare(); when
// they aren't at rest the picture goes through the screen effect pass
// (screen_effect.h / gl_screen_effect.h) after the stages, before the UI.
// The same pass makes GPU screenshots: video_output_request_capture(), then
// video_output_take_capture() a few frames later.

#include <cstdint>
#include <string>

#include "gl_picture.h"
#include "gl_shader_chain.h"
#include "picture.h"
#include "screen_effect.h"
#include "screen_effect_params.h"
#include "shader_chain.h"
#include "viewport.h"

struct AppShell;
struct ImDrawList;
struct VideoTexture;

struct VideoOutput
{
    bool shaded = false;   // this frame goes through the stages (set by video_output_prepare)
    bool effect_on = false;   // this frame goes through the screen effect pass (set by video_output_prepare)
    ScreenEffectParams effect = {};   // this frame's knobs (video_output_set_effect)

    // Vulkan, underneath (only video_output.cpp looks inside).
    ShaderChain chain   = {};
    Picture     picture = {};
    ScreenEffect effect_pass = {};
    // OpenGL ES: our own presets (gl_shader_chain.h), else the picture.
    bool          on_gl      = false;   // set by load_stage / prepare from the shell
    GlShaderChain gl_chain   = {};
    GlPicture     gl_picture = {};
};

// Loads `preset_path` into stage `stage` (0 or 1), replacing what it had;
// "" empties the stage. Waits for the GPU. False (logged) if the preset
// fails; the stage is then empty (and its failed preset set). `library` =
// the shader library, loaded or not, or nullptr (without one, presets fail
// and the picture is drawn plain).
bool video_output_load_stage(VideoOutput* output, const ShaderLibrary* library, AppShell* shell, uint32_t stage,
                             const char* preset_path);

// The preset in stage `stage` ("" = none).
const std::string& video_output_stage_preset(const VideoOutput* output, uint32_t stage);

// The last preset that failed to load into stage `stage` ("" = none; emptied
// with the stage), so a frontend doesn't retry it every frame.
const std::string& video_output_stage_failed_preset(const VideoOutput* output, uint32_t stage);

// True when stage `stage` has a preset loaded.
bool video_output_stage_loaded(const VideoOutput* output, uint32_t stage);

// True when any stage has a preset.
bool video_output_shaded_available(const VideoOutput* output);

// Sets a preset parameter (#pragma parameter) of one stage. False if the
// stage is empty or has no parameter by that name.
bool video_output_set_param(VideoOutput* output, const ShaderLibrary* library, AppShell* shell, uint32_t stage,
                            const char* name, float value);

// This frame's screen effect knobs (all at rest: none). Before video_output_prepare().
void video_output_set_effect(VideoOutput* output, const ScreenEffectParams* params);

// Asks for a GPU screenshot: the picture as shown (stages, crop, scale; no
// UI, no effect), read back without stalling the GPU. False if one is
// already on its way or this API can't. Taken from the next frame drawn.
bool video_output_request_capture(VideoOutput* output);

// The screenshot asked for, once it has been read back (the caller frees it
// with video_capture_free). False while it isn't ready.
bool video_output_take_capture(VideoOutput* output, VideoCapture* capture);

// Decides what this frame draws: through the stages when there are any and
// they can run, else plain. False: nothing to draw (no picture yet).
bool video_output_prepare(VideoOutput* output, AppShell* shell, const VideoTexture* source,
                          const ViewportResult* viewport);

// Nothing is shown this frame (records nothing).
void video_output_skip(VideoOutput* output);

// Adds the picture to `draw_list`; the viewport's (0, 0) is at (origin_x, origin_y).
void video_output_draw(const VideoOutput* output, ImDrawList* draw_list, const VideoTexture* source, float origin_x,
                       float origin_y);

// Records this frame's GPU work (copies, shader passes). Only after
// app_shell_acquire() returned true. Does nothing when nothing was prepared.
void video_output_record(VideoOutput* output, const ShaderLibrary* library, AppShell* shell, VideoTexture* source);

// Stage 0's result this frame (the shown part, after the crop, before stage
// 1), once it has been written: for previews of the picture controls. False
// when there is none.
bool video_output_stage0_result(const VideoOutput* output, uint64_t* imgui_id, uint32_t* width, uint32_t* height);

// Frees everything. Waits for the GPU. Safe on an empty output.
void video_output_destroy(VideoOutput* output, const ShaderLibrary* library, AppShell* shell);
