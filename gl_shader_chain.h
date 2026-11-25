#pragma once

// Shader presets on OpenGL ES (piccolo's docs/gl.md): our own small format,
// the GL path's counterpart of shader_chain.h (librashader, Vulkan). For old
// devices and the browser: a short list of simple shaders, no parameters
// (frontends set their own uniforms, e.g. pass 0's picture controls, with
// gl_shader_chain_set_param).
//
// A preset (*.glpreset, settings_file's format, paths relative to it):
//
//   passes = 1                       1 to GL_SHADER_MAX_PASSES
//   pass0 = crt-mask.glsl            the fragment shader of each pass
//   pass0_filter = nearest           how the pass samples its input: nearest / linear
//   pass0_scale = viewport           its output: source (the input's size) or
//                                    viewport (the picture's size on screen)
//   lut = masks/aperture-2x2.png     optional texture, sampled nearest
//
// A pass is a GLSL ES 1.00 fragment shader body. The runner puts this before it:
//
//   precision mediump float;
//   uniform sampler2D u_source;       the input (the whole texture is the picture)
//   uniform vec4 u_source_size;       w, h, 1/w, 1/h
//   uniform vec4 u_output_size;       w, h, 1/w, 1/h
//   uniform float u_frame_count;
//   uniform sampler2D u_lut;          the preset's lut (if any)
//   uniform vec4 u_lut_size;          w, h, 1/w, 1/h
//   varying vec2 v_coord;             0..1 over the input; row 0 at v_coord.y = 0
//
// and the shader writes gl_FragColor. mediump: the Tegra 4 computes in fp20
// whatever the shader asks for.
//
// A chain has two stages, as shader_chain.h: stage 0 (the frontend's pass 0,
// at the picture's own size) and stage 1 (the chosen preset). Per frame:
//
//   the source texture's shown part -> input (exact size, nearest copy)
//     -> stage 0's passes -> stage 1's passes
//     -> the last pass at the picture's size on screen -> drawn 1:1 by the GUI.

#include <cstdint>
#include <string>

#include "gl_context.h"
#include "gl_texture.h"
#include "shown_image.h"
#include "viewport.h"


constexpr uint32_t GL_SHADER_MAX_PASSES = 4;
constexpr uint32_t GL_SHADER_STAGES = 2;

struct GlShaderPass
{
    GLuint program        = 0;
    GLint  source         = -1;   // uniform locations (-1: the shader doesn't use it)
    GLint  source_size    = -1;
    GLint  output_size    = -1;
    GLint  frame_count    = -1;
    GLint  lut            = -1;
    GLint  lut_size       = -1;
    bool   linear         = false;   // samples its input linear
    bool   viewport_scale = false;   // output at the picture's size on screen

    // Its output (made by gl_shader_chain_prepare).
    GLuint   texture     = 0;
    GLuint   framebuffer = 0;
    uint32_t width       = 0;
    uint32_t height      = 0;
};

struct GlShaderStage
{
    GlShaderPass passes[GL_SHADER_MAX_PASSES] = {};
    uint32_t     pass_count  = 0;
    GLuint       lut         = 0;
    uint32_t     lut_width   = 0;
    uint32_t     lut_height  = 0;
    std::string  preset_path;    // "" = empty
    std::string  failed_path;    // the last preset that failed (not retried every frame)
    uint64_t     frame_count = 0;
};

struct GlShaderChain
{
    GlShaderStage stages[GL_SHADER_STAGES] = {};

    // The shown part of the source, exact size.
    GLuint   input_texture     = 0;
    GLuint   input_framebuffer = 0;
    uint32_t input_width       = 0;
    uint32_t input_height      = 0;

    ViewportResult viewport = {};   // what this frame draws (set by prepare)
    bool           ready    = false;
    bool           written  = false;   // the pass outputs hold a frame (false after they're remade)
};

// Loads a preset into stage `stage`, replacing what it had ("" empties it).
// False (logged) if it fails; the stage is then empty with failed_path set.
bool gl_shader_chain_load(GlShaderChain* chain, GlContext* context, uint32_t stage, const char* preset_path);
bool gl_shader_chain_active(const GlShaderChain* chain);

// Sets a float uniform called `name` in every pass of the stage that has one.
// False if none has it.
bool gl_shader_chain_set_param(GlShaderChain* chain, GlContext* context, uint32_t stage, const char* name,
                               float value);

// Decides what this frame draws (makes the pass outputs when sizes change).
// False: no chain, or no picture.
bool gl_shader_chain_prepare(GlShaderChain* chain, GlContext* context, const GlStreamingTexture* source,
                             bool has_content, const ViewportResult* viewport);

bool gl_shader_chain_shown_image(const GlShaderChain* chain, float origin_x, float origin_y, ShownImage* image);

// The last pass's output (the picture's size on screen), or 0 without passes.
GLuint gl_shader_chain_output_texture(const GlShaderChain* chain);

// Runs the passes (after the source's upload this frame).
void gl_shader_chain_record(GlShaderChain* chain, GlContext* context, const GlStreamingTexture* source);

// Stage 0's output when stage 1 follows it (the picture with its colors
// corrected, for previews). False when there's none.
bool gl_shader_chain_stage0_result(const GlShaderChain* chain, uint64_t* ui_texture, uint32_t* width,
                                   uint32_t* height);

void gl_shader_chain_destroy(GlShaderChain* chain, GlContext* context);
