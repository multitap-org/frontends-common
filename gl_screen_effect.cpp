#include "gl_screen_effect.h"

#include <cstring>

#include <SDL3/SDL_log.h>

#include "imgui.h"

#include "gl_texture.h"

// GLSL ES 1.00, the same quad as the copy program (gl_context.cpp): row 0 of
// the source (the picture's top) lands at the target's y = 0, so the target
// keeps the uploaded orientation and ImGui draws it the right way up.
static const char* EFFECT_VERTEX_SHADER =
    "attribute vec2 a_position;\n"
    "varying vec2 v_coord;\n"
    "void main()\n"
    "{\n"
    "    v_coord = a_position;\n"
    "    gl_Position = vec4(a_position * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

// shaders/screen_effect.frag, in GLSL ES 1.00 (keep both in step). highp
// where there is some, for the same reason as the copy program's.
static const char* EFFECT_FRAGMENT_SHADER =
    "#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
    "precision highp float;\n"
    "#else\n"
    "precision mediump float;\n"
    "#endif\n"
    "uniform sampler2D u_source;\n"
    "uniform vec4 u_source_rect;\n"   // x, y, width, height
    "uniform vec4 u_knobs;\n"         // zoom, desaturate, flash, bars
    "uniform vec4 u_flash_color;\n"
    "varying vec2 v_coord;\n"
    "void main()\n"
    "{\n"
    "    vec2 coord = (v_coord - 0.5) / u_knobs.x + 0.5;\n"
    "    vec3 color = texture2D(u_source, u_source_rect.xy + coord * u_source_rect.zw).rgb;\n"
    "    float luma = dot(color, vec3(0.299, 0.587, 0.114));\n"
    "    color = mix(color, vec3(luma), u_knobs.y);\n"
    "    color = mix(color, u_flash_color.rgb, u_knobs.z);\n"
    "    float edge = min(v_coord.y, 1.0 - v_coord.y);\n"
    "    if (edge < u_knobs.w)\n"
    "    {\n"
    "        color = vec3(0.0);\n"
    "    }\n"
    "    gl_FragColor = vec4(color, 1.0);\n"
    "}\n";

static GLuint compile(GlContext* context, GLenum type, const char* source)
{
    GlFunctions* gl = &context->gl;
    GLuint shader = gl->CreateShader(type);
    gl->ShaderSource(shader, 1, &source, nullptr);
    gl->CompileShader(shader);
    GLint compiled = GL_FALSE;
    gl->GetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled != GL_TRUE)
    {
        char log[1024] = {};
        gl->GetShaderInfoLog(shader, sizeof(log), nullptr, log);
        SDL_LogError(SDL_LOG_CATEGORY_GPU, "GL: the screen effect's %s shader didn't compile: %s",
                     type == GL_VERTEX_SHADER ? "vertex" : "fragment", log);
        gl->DeleteShader(shader);
        return 0;
    }
    return shader;
}

static bool create_program(GlScreenEffect* effect, GlContext* context)
{
    GlFunctions* gl = &context->gl;
    GLuint vertex = compile(context, GL_VERTEX_SHADER, EFFECT_VERTEX_SHADER);
    GLuint fragment = compile(context, GL_FRAGMENT_SHADER, EFFECT_FRAGMENT_SHADER);
    bool ok = vertex != 0 && fragment != 0;
    if (ok)
    {
        effect->program = gl->CreateProgram();
        gl->AttachShader(effect->program, vertex);
        gl->AttachShader(effect->program, fragment);
        gl->BindAttribLocation(effect->program, 0, "a_position");
        gl->LinkProgram(effect->program);
        GLint linked = GL_FALSE;
        gl->GetProgramiv(effect->program, GL_LINK_STATUS, &linked);
        if (linked != GL_TRUE)
        {
            char log[1024] = {};
            gl->GetProgramInfoLog(effect->program, sizeof(log), nullptr, log);
            SDL_LogError(SDL_LOG_CATEGORY_GPU, "GL: the screen effect program didn't link: %s", log);
            ok = false;
        }
    }
    if (vertex != 0)
    {
        gl->DeleteShader(vertex);
    }
    if (fragment != 0)
    {
        gl->DeleteShader(fragment);
    }
    if (!ok)
    {
        return false;
    }
    effect->source = gl->GetUniformLocation(effect->program, "u_source");
    effect->source_rect = gl->GetUniformLocation(effect->program, "u_source_rect");
    effect->knobs = gl->GetUniformLocation(effect->program, "u_knobs");
    effect->flash_color = gl->GetUniformLocation(effect->program, "u_flash_color");
    return true;
}

static void delete_capture_objects(GlScreenEffect* effect, GlContext* context)
{
    GlFunctions* gl = &context->gl;
    if (effect->fence != nullptr && gl->DeleteSync != nullptr)
    {
        gl->DeleteSync(effect->fence);
    }
    if (effect->pack_buffer != 0)
    {
        gl->DeleteBuffers(1, &effect->pack_buffer);
    }
    effect->fence = nullptr;
    effect->pack_buffer = 0;
}

void gl_screen_effect_destroy(GlScreenEffect* effect, GlContext* context)
{
    if (context->context != nullptr)
    {
        gl_render_target_destroy(context, &effect->texture, &effect->framebuffer);
        delete_capture_objects(effect, context);
        if (effect->program != 0)
        {
            context->gl.DeleteProgram(effect->program);
        }
    }
    video_capture_free(&effect->captured);
    *effect = GlScreenEffect{};
}

bool gl_screen_effect_prepare(GlScreenEffect* effect, GlContext* context, const ViewportResult* viewport)
{
    effect->ready = false;
    if (effect->failed || viewport->width == 0 || viewport->height == 0)
    {
        return false;
    }
    uint64_t limit = static_cast<uint64_t>(context->max_texture_size);
    if (viewport->width > limit || viewport->height > limit)
    {
        return false;
    }
    if (effect->program == 0 && !create_program(effect, context))
    {
        SDL_LogError(SDL_LOG_CATEGORY_GPU, "GL: no screen effects");
        gl_screen_effect_destroy(effect, context);
        effect->failed = true;
        return false;
    }
    if (effect->texture == 0 || effect->width != viewport->width || effect->height != viewport->height)
    {
        gl_render_target_destroy(context, &effect->texture, &effect->framebuffer);
        effect->width = 0;
        effect->height = 0;
        if (!gl_render_target_create(context, viewport->width, viewport->height, &effect->texture, &effect->framebuffer))
        {
            return false;
        }
        effect->width = viewport->width;
        effect->height = viewport->height;
        SDL_LogInfo(SDL_LOG_CATEGORY_GPU, "Screen effect target (GL): %ux%u", effect->width, effect->height);
    }
    effect->viewport = *viewport;
    effect->ready = true;
    return true;
}

void gl_screen_effect_draw(const GlScreenEffect* effect, ImDrawList* draw_list, float origin_x, float origin_y)
{
    if (!effect->ready)
    {
        return;
    }
    const ViewportResult* viewport = &effect->viewport;
    ImVec2 top_left(origin_x + static_cast<float>(viewport->x), origin_y + static_cast<float>(viewport->y));
    ImVec2 bottom_right(top_left.x + static_cast<float>(viewport->width),
                        top_left.y + static_cast<float>(viewport->height));
    const ImGuiPlatformIO& platform_io = ImGui::GetPlatformIO();
    // ImTextureID on ImGui's GL backend is the texture name. 1:1, nearest.
    draw_list->AddCallback(platform_io.DrawCallback_SetSamplerNearest, nullptr);
    draw_list->AddImage(ImTextureRef(static_cast<ImTextureID>(effect->texture)), top_left, bottom_right,
                        ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f));
    draw_list->AddCallback(platform_io.DrawCallback_SetSamplerLinear, nullptr);
}

bool gl_screen_effect_request_capture(GlScreenEffect* effect)
{
    if (effect->capture_state != ScreenEffectCaptureState::none)
    {
        return false;
    }
    effect->capture_state = ScreenEffectCaptureState::requested;
    return true;
}

static void start_capture(GlScreenEffect* effect, uint32_t width, uint32_t height)
{
    video_capture_free(&effect->captured);
    effect->captured.width = width;
    effect->captured.height = height;
    effect->captured.pitch = width * 4;
    effect->captured.format = VideoCaptureFormat::rgba8;
    effect->captured.pixels = new uint8_t[static_cast<size_t>(width) * height * 4];
}

void gl_screen_effect_collect(GlScreenEffect* effect, GlContext* context)
{
    if (effect->capture_state != ScreenEffectCaptureState::recorded || effect->fence == nullptr)
    {
        return;
    }
    GlFunctions* gl = &context->gl;
    // A zero timeout only asks: never waits here.
    GLenum status = gl->ClientWaitSync(effect->fence, 0, 0);
    if (status != GLES3_ALREADY_SIGNALED && status != GLES3_CONDITION_SATISFIED)
    {
        if (status == GLES3_WAIT_FAILED)
        {
            SDL_LogWarn(SDL_LOG_CATEGORY_GPU, "GL: a screen capture's fence failed; capture dropped");
            delete_capture_objects(effect, context);
            effect->capture_state = ScreenEffectCaptureState::none;
        }
        return;
    }
    gl->DeleteSync(effect->fence);
    effect->fence = nullptr;
    size_t size = static_cast<size_t>(effect->capture_width) * effect->capture_height * 4;
    gl->BindBuffer(GLES3_PIXEL_PACK_BUFFER, effect->pack_buffer);
    void* mapped = gl->MapBufferRange(GLES3_PIXEL_PACK_BUFFER, 0, static_cast<GLsizeiptr>(size), GLES3_MAP_READ_BIT);
    bool ok = mapped != nullptr;
    if (ok)
    {
        start_capture(effect, effect->capture_width, effect->capture_height);
        std::memcpy(effect->captured.pixels, mapped, size);
        gl->UnmapBuffer(GLES3_PIXEL_PACK_BUFFER);
    }
    gl->BindBuffer(GLES3_PIXEL_PACK_BUFFER, 0);
    if (!ok)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_GPU, "GL: a screen capture couldn't be mapped; capture dropped");
        effect->capture_state = ScreenEffectCaptureState::none;
        return;
    }
    effect->capture_state = ScreenEffectCaptureState::ready;
}

// Reads the target back (bound as the framebuffer).
static void record_capture(GlScreenEffect* effect, GlContext* context)
{
    GlFunctions* gl = &context->gl;
    GLsizei width = static_cast<GLsizei>(effect->width);
    GLsizei height = static_cast<GLsizei>(effect->height);
    gl->PixelStorei(GL_PACK_ALIGNMENT, 4);   // rows of 4-byte pixels: always packed
    if (gl_context_has_async_read(context))
    {
        size_t size = static_cast<size_t>(effect->width) * effect->height * 4;
        if (effect->pack_buffer == 0)
        {
            gl->GenBuffers(1, &effect->pack_buffer);
        }
        gl->BindBuffer(GLES3_PIXEL_PACK_BUFFER, effect->pack_buffer);
        gl->BufferData(GLES3_PIXEL_PACK_BUFFER, static_cast<GLsizeiptr>(size), nullptr, GLES3_STREAM_READ);
        // With a pack buffer bound the "pointer" is an offset into it, and
        // the call returns at once.
        gl->ReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        gl->BindBuffer(GLES3_PIXEL_PACK_BUFFER, 0);
        effect->fence = gl->FenceSync(GLES3_SYNC_GPU_COMMANDS_COMPLETE, 0);
        effect->capture_width = effect->width;
        effect->capture_height = effect->height;
        effect->capture_state = effect->fence != nullptr ? ScreenEffectCaptureState::recorded
                                                         : ScreenEffectCaptureState::none;
        return;
    }
    // GLES 2 / the browser: waits for the GPU (see the top of the header).
    start_capture(effect, effect->width, effect->height);
    gl->ReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, effect->captured.pixels);
    effect->capture_state = ScreenEffectCaptureState::ready;
}

void gl_screen_effect_record(GlScreenEffect* effect, GlContext* context, const GlScreenEffectSource* source,
                             const ScreenEffectParams* params)
{
    if (!effect->ready || effect->framebuffer == 0 || source->texture == 0)
    {
        return;
    }
    GlFunctions* gl = &context->gl;
    bool capture = effect->capture_state == ScreenEffectCaptureState::requested;
    ScreenEffectParams rest = {};
    const ScreenEffectParams* knobs = capture ? &rest : params;   // a capture shows the picture itself

    gl->BindFramebuffer(GL_FRAMEBUFFER, effect->framebuffer);
    gl->Viewport(0, 0, static_cast<GLsizei>(effect->width), static_cast<GLsizei>(effect->height));
    // ImGui sets and restores its own state around its draws; ours runs
    // between them with these off.
    gl->Disable(GL_BLEND);
    gl->Disable(GL_SCISSOR_TEST);
    gl->Disable(GL_DEPTH_TEST);
    gl->Disable(GL_CULL_FACE);
    gl->UseProgram(effect->program);
    gl->ActiveTexture(GL_TEXTURE0);
    gl->BindTexture(GL_TEXTURE_2D, source->texture);
    GLint filter = source->linear ? GL_LINEAR : GL_NEAREST;   // ImGui changes it too: set on every use
    gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    gl->Uniform1i(effect->source, 0);
    gl->Uniform4f(effect->source_rect, source->rect[0], source->rect[1], source->rect[2], source->rect[3]);
    gl->Uniform4f(effect->knobs, knobs->zoom < 1.0f ? 1.0f : knobs->zoom, knobs->desaturate, knobs->flash, knobs->bars);
    gl->Uniform4f(effect->flash_color, knobs->flash_red, knobs->flash_green, knobs->flash_blue, 1.0f);
    gl->BindBuffer(GL_ARRAY_BUFFER, context->copy.quad);
    gl->VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    gl->EnableVertexAttribArray(0);
    gl->DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    gl->BindBuffer(GL_ARRAY_BUFFER, 0);
    gl->UseProgram(0);
    if (capture)
    {
        record_capture(effect, context);
    }
    gl->BindFramebuffer(GL_FRAMEBUFFER, 0);
    gl_check_errors(context, "screen effect");
}

bool gl_screen_effect_capture_take(GlScreenEffect* effect, VideoCapture* capture)
{
    if (effect->capture_state != ScreenEffectCaptureState::ready)
    {
        return false;
    }
    *capture = effect->captured;
    effect->captured = VideoCapture{};
    effect->capture_state = ScreenEffectCaptureState::none;
    return true;
}
