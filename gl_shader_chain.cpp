#include "gl_shader_chain.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <SDL3/SDL.h>

#include "imgui.h"

#include "host_fs.h"
#include "settings_file.h"

// ---------------------------------------------------------------------------
// Programs
// ---------------------------------------------------------------------------

// Every pass draws the same quad over its whole output; v_coord is 0..1 over
// the input (inputs are always exactly the picture, never a corner of a
// bigger texture).
static const char* PASS_VERTEX_SHADER =
    "attribute vec2 a_position;\n"
    "varying vec2 v_coord;\n"
    "void main()\n"
    "{\n"
    "    v_coord = a_position;\n"
    "    gl_Position = vec4(a_position * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

// What every pass gets before its own text (gl_shader_chain.h). highp where
// the GPU has it in fragment shaders: the presets work out where they are
// within a source pixel (fract(v_coord * u_source_size)), and mediump is
// really 16-bit on some GPUs (Mali with panfrost, phones): at 224 lines
// that's off by a tenth of a line, so scanlines come out uneven. The Tegra 4
// has no highp there (fp20 for both), so it keeps mediump.
static const char* PASS_FRAGMENT_HEADER =
    "#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
    "precision highp float;\n"
    "#else\n"
    "precision mediump float;\n"
    "#endif\n"
    "uniform sampler2D u_source;\n"
    "uniform vec4 u_source_size;\n"
    "uniform vec4 u_output_size;\n"
    "uniform float u_frame_count;\n"
    "uniform sampler2D u_lut;\n"
    "uniform vec4 u_lut_size;\n"
    "varying vec2 v_coord;\n"
    "#line 1\n";

static GLuint compile(GlContext* context, GLenum type, const char* header, const char* text, const char* path)
{
    GlFunctions* gl = &context->gl;
    GLuint shader = gl->CreateShader(type);
    const char* sources[2] = { header, text };
    gl->ShaderSource(shader, 2, sources, nullptr);
    gl->CompileShader(shader);
    GLint compiled = GL_FALSE;
    gl->GetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled != GL_TRUE)
    {
        char log[2048] = {};
        gl->GetShaderInfoLog(shader, sizeof(log), nullptr, log);
        SDL_LogError(SDL_LOG_CATEGORY_GPU, "GL shader %s didn't compile:\n%s", path, log);
        gl->DeleteShader(shader);
        return 0;
    }
    return shader;
}

static bool create_pass_program(GlShaderPass* pass, GlContext* context, const char* text, const char* path)
{
    GlFunctions* gl = &context->gl;
    GLuint vertex = compile(context, GL_VERTEX_SHADER, "", PASS_VERTEX_SHADER, "(vertex)");
    GLuint fragment = compile(context, GL_FRAGMENT_SHADER, PASS_FRAGMENT_HEADER, text, path);
    if (vertex == 0 || fragment == 0)
    {
        if (vertex != 0)
        {
            gl->DeleteShader(vertex);
        }
        if (fragment != 0)
        {
            gl->DeleteShader(fragment);
        }
        return false;
    }
    pass->program = gl->CreateProgram();
    gl->AttachShader(pass->program, vertex);
    gl->AttachShader(pass->program, fragment);
    gl->BindAttribLocation(pass->program, 0, "a_position");
    gl->LinkProgram(pass->program);
    gl->DeleteShader(vertex);   // the program keeps them while it lives
    gl->DeleteShader(fragment);
    GLint linked = GL_FALSE;
    gl->GetProgramiv(pass->program, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE)
    {
        char log[2048] = {};
        gl->GetProgramInfoLog(pass->program, sizeof(log), nullptr, log);
        SDL_LogError(SDL_LOG_CATEGORY_GPU, "GL shader %s didn't link:\n%s", path, log);
        return false;
    }
    pass->source = gl->GetUniformLocation(pass->program, "u_source");
    pass->source_size = gl->GetUniformLocation(pass->program, "u_source_size");
    pass->output_size = gl->GetUniformLocation(pass->program, "u_output_size");
    pass->frame_count = gl->GetUniformLocation(pass->program, "u_frame_count");
    pass->lut = gl->GetUniformLocation(pass->program, "u_lut");
    pass->lut_size = gl->GetUniformLocation(pass->program, "u_lut_size");
    return true;
}

// ---------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------

static void free_outputs(GlShaderChain* chain, GlContext* context)
{
    for (uint32_t s = 0; s < GL_SHADER_STAGES; s++)
    {
        for (uint32_t i = 0; i < chain->stages[s].pass_count; i++)
        {
            GlShaderPass* pass = &chain->stages[s].passes[i];
            gl_render_target_destroy(context, &pass->texture, &pass->framebuffer);
            pass->width = 0;
            pass->height = 0;
        }
    }
    chain->written = false;
}

static void clear_stage(GlShaderChain* chain, GlContext* context, uint32_t stage)
{
    GlShaderStage* target = &chain->stages[stage];
    for (uint32_t i = 0; i < target->pass_count; i++)
    {
        GlShaderPass* pass = &target->passes[i];
        gl_render_target_destroy(context, &pass->texture, &pass->framebuffer);
        if (pass->program != 0)
        {
            context->gl.DeleteProgram(pass->program);
        }
    }
    if (target->lut != 0)
    {
        context->gl.DeleteTextures(1, &target->lut);
    }
    *target = GlShaderStage{};
    // The other stage's outputs may change size (the last pass is the picture's size).
    free_outputs(chain, context);
}

// `path` relative to the preset's folder (absolute paths stay as they are).
static std::string relative_to(const char* preset_path, const char* path)
{
    if (path[0] == '/' || (path[0] != '\0' && path[1] == ':'))
    {
        return path;
    }
    std::string folder = preset_path;
    size_t slash = folder.find_last_of("/\\");
    folder = slash == std::string::npos ? std::string() : folder.substr(0, slash + 1);
    return folder + path;
}

static bool read_text(const std::string& path, std::string* text)
{
    uint8_t* data = nullptr;
    size_t size = 0;
    if (!host_fs_read_file(path.c_str(), &data, &size))
    {
        return false;   // logged by host_fs
    }
    text->assign(static_cast<const char*>(static_cast<const void*>(data)), size);
    delete[] data;
    return true;
}

static bool load_lut(GlShaderStage* stage, GlContext* context, const std::string& path)
{
    SDL_Surface* png = SDL_LoadPNG(path.c_str());
    // RGBA32: bytes R, G, B, A in memory, GL_RGBA / GL_UNSIGNED_BYTE's order.
    SDL_Surface* rgba = png != nullptr ? SDL_ConvertSurface(png, SDL_PIXELFORMAT_RGBA32) : nullptr;
    bool ok = rgba != nullptr && rgba->pitch == rgba->w * 4;
    if (ok)
    {
        GlFunctions* gl = &context->gl;
        gl->GenTextures(1, &stage->lut);
        gl->BindTexture(GL_TEXTURE_2D, stage->lut);
        // Clamp + nearest: shaders tile it themselves with fract() (GLES 2
        // only repeats power-of-two textures).
        gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        gl->PixelStorei(GL_UNPACK_ALIGNMENT, 4);
        gl->TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, rgba->w, rgba->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba->pixels);
        gl->BindTexture(GL_TEXTURE_2D, 0);
        stage->lut_width = static_cast<uint32_t>(rgba->w);
        stage->lut_height = static_cast<uint32_t>(rgba->h);
    }
    else
    {
        SDL_LogError(SDL_LOG_CATEGORY_GPU, "GL shader texture %s: %s", path.c_str(),
                     png == nullptr ? SDL_GetError() : "couldn't convert it");
    }
    SDL_DestroySurface(rgba);
    SDL_DestroySurface(png);
    return ok;
}

static bool load_stage(GlShaderStage* stage, GlContext* context, const char* preset_path)
{
    SettingsFile preset = {};
    if (!settings_file_load(&preset, preset_path))
    {
        SDL_LogError(SDL_LOG_CATEGORY_GPU, "GL shader preset %s: can't read it", preset_path);
        return false;
    }
    const char* passes = settings_file_get(&preset, "", "passes");
    long count = passes != nullptr ? std::strtol(passes, nullptr, 10) : 0;
    if (count < 1 || count > static_cast<long>(GL_SHADER_MAX_PASSES))
    {
        SDL_LogError(SDL_LOG_CATEGORY_GPU, "GL shader preset %s: passes must be 1 to %u", preset_path,
                     GL_SHADER_MAX_PASSES);
        return false;
    }
    for (uint32_t i = 0; i < static_cast<uint32_t>(count); i++)
    {
        char key[32] = {};
        std::snprintf(key, sizeof(key), "pass%u", i);
        const char* file = settings_file_get(&preset, "", key);
        std::string text;
        std::string path = file != nullptr ? relative_to(preset_path, file) : std::string();
        if (file == nullptr || !read_text(path, &text))
        {
            SDL_LogError(SDL_LOG_CATEGORY_GPU, "GL shader preset %s: no shader for %s", preset_path, key);
            return false;
        }
        GlShaderPass* pass = &stage->passes[i];
        stage->pass_count = i + 1;   // so a failure frees what was made
        if (!create_pass_program(pass, context, text.c_str(), path.c_str()))
        {
            return false;
        }
        std::snprintf(key, sizeof(key), "pass%u_filter", i);
        const char* filter = settings_file_get(&preset, "", key);
        pass->linear = filter != nullptr && std::strcmp(filter, "linear") == 0;
        std::snprintf(key, sizeof(key), "pass%u_scale", i);
        const char* scale = settings_file_get(&preset, "", key);
        pass->viewport_scale = scale != nullptr && std::strcmp(scale, "viewport") == 0;
    }
    const char* lut = settings_file_get(&preset, "", "lut");
    if (lut != nullptr && lut[0] != '\0' && !load_lut(stage, context, relative_to(preset_path, lut)))
    {
        return false;
    }
    return true;
}

bool gl_shader_chain_load(GlShaderChain* chain, GlContext* context, uint32_t stage, const char* preset_path)
{
    clear_stage(chain, context, stage);
    if (preset_path[0] == '\0')
    {
        return true;
    }
    GlShaderStage* target = &chain->stages[stage];
    if (!load_stage(target, context, preset_path))
    {
        clear_stage(chain, context, stage);
        target->failed_path = preset_path;
        return false;
    }
    target->preset_path = preset_path;
    gl_check_errors(context, "GL shader preset");
    SDL_LogInfo(SDL_LOG_CATEGORY_GPU, "GL shader stage %u: %s (%u passes)", stage, preset_path, target->pass_count);
    return true;
}

bool gl_shader_chain_active(const GlShaderChain* chain)
{
    return chain->stages[0].pass_count > 0 || chain->stages[1].pass_count > 0;
}

bool gl_shader_chain_set_param(GlShaderChain* chain, GlContext* context, uint32_t stage, const char* name,
                               float value)
{
    GlFunctions* gl = &context->gl;
    GlShaderStage* target = &chain->stages[stage];
    bool found = false;
    for (uint32_t i = 0; i < target->pass_count; i++)
    {
        GLint location = gl->GetUniformLocation(target->passes[i].program, name);
        if (location < 0)
        {
            continue;
        }
        // A program keeps its uniforms' values: set once, used every frame.
        gl->UseProgram(target->passes[i].program);
        gl->Uniform1f(location, value);
        found = true;
    }
    gl->UseProgram(0);
    return found;
}

// ---------------------------------------------------------------------------
// Per frame
// ---------------------------------------------------------------------------

static GlShaderPass* last_pass(GlShaderChain* chain)
{
    for (uint32_t s = GL_SHADER_STAGES; s > 0; s--)
    {
        GlShaderStage* stage = &chain->stages[s - 1];
        if (stage->pass_count > 0)
        {
            return &stage->passes[stage->pass_count - 1];
        }
    }
    return nullptr;
}

static bool ensure_target(GlContext* context, uint32_t width, uint32_t height, GLuint* texture, GLuint* framebuffer,
                          uint32_t* current_width, uint32_t* current_height)
{
    if (*texture != 0 && *current_width == width && *current_height == height)
    {
        return true;
    }
    gl_render_target_destroy(context, texture, framebuffer);
    *current_width = 0;
    *current_height = 0;
    if (!gl_render_target_create(context, width, height, texture, framebuffer))
    {
        return false;
    }
    *current_width = width;
    *current_height = height;
    return true;
}

bool gl_shader_chain_prepare(GlShaderChain* chain, GlContext* context, const GlStreamingTexture* source,
                             bool has_content, const ViewportResult* viewport)
{
    chain->ready = false;
    GlShaderPass* final_pass = last_pass(chain);
    if (final_pass == nullptr || !has_content || source->shown == 0 || viewport->width == 0 ||
        viewport->height == 0 || viewport->source_width == 0 || viewport->source_height == 0)
    {
        return false;
    }
    uint32_t limit = static_cast<uint32_t>(context->max_texture_size);
    if (viewport->width > limit || viewport->height > limit)
    {
        return false;
    }
    bool remade = false;
    if (chain->input_width != viewport->source_width || chain->input_height != viewport->source_height)
    {
        remade = true;
    }
    if (!ensure_target(context, viewport->source_width, viewport->source_height, &chain->input_texture,
                       &chain->input_framebuffer, &chain->input_width, &chain->input_height))
    {
        return false;
    }
    // Each pass's output: its input's size, or the picture's size on screen;
    // the last one always the picture's size (drawn 1:1).
    uint32_t width = viewport->source_width;
    uint32_t height = viewport->source_height;
    for (uint32_t s = 0; s < GL_SHADER_STAGES; s++)
    {
        GlShaderStage* stage = &chain->stages[s];
        for (uint32_t i = 0; i < stage->pass_count; i++)
        {
            GlShaderPass* pass = &stage->passes[i];
            if (pass->viewport_scale || pass == final_pass)
            {
                width = viewport->width;
                height = viewport->height;
            }
            if (pass->width != width || pass->height != height)
            {
                remade = true;
            }
            if (!ensure_target(context, width, height, &pass->texture, &pass->framebuffer, &pass->width,
                               &pass->height))
            {
                return false;
            }
        }
    }
    if (remade)
    {
        chain->written = false;
    }
    chain->viewport = *viewport;
    chain->ready = true;
    return true;
}

GLuint gl_shader_chain_output_texture(const GlShaderChain* chain)
{
    for (uint32_t s = GL_SHADER_STAGES; s > 0; s--)
    {
        const GlShaderStage* stage = &chain->stages[s - 1];
        if (stage->pass_count > 0)
        {
            return stage->passes[stage->pass_count - 1].texture;
        }
    }
    return 0;
}

void gl_shader_chain_draw(const GlShaderChain* chain, ImDrawList* draw_list, float origin_x, float origin_y)
{
    if (!chain->ready)
    {
        return;
    }
    const ViewportResult* viewport = &chain->viewport;
    ImVec2 top_left(origin_x + static_cast<float>(viewport->x), origin_y + static_cast<float>(viewport->y));
    ImVec2 bottom_right(top_left.x + static_cast<float>(viewport->width),
                        top_left.y + static_cast<float>(viewport->height));
    const ImGuiPlatformIO& platform_io = ImGui::GetPlatformIO();
    // Already the picture's size: 1:1, nearest.
    draw_list->AddCallback(platform_io.DrawCallback_SetSamplerNearest, nullptr);
    GLuint output = gl_shader_chain_output_texture(chain);
    draw_list->AddImage(ImTextureRef(static_cast<ImTextureID>(output)), top_left, bottom_right,
                        ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f));
    draw_list->AddCallback(platform_io.DrawCallback_SetSamplerLinear, nullptr);
}

static void set_size(GlFunctions* gl, GLint location, uint32_t width, uint32_t height)
{
    if (location < 0)
    {
        return;
    }
    float w = static_cast<float>(width);
    float h = static_cast<float>(height);
    gl->Uniform4f(location, w, h, 1.0f / w, 1.0f / h);
}

static void run_pass(GlShaderPass* pass, GlShaderStage* stage, GlContext* context, GLuint input,
                     uint32_t input_width, uint32_t input_height)
{
    GlFunctions* gl = &context->gl;
    gl->BindFramebuffer(GL_FRAMEBUFFER, pass->framebuffer);
    gl->Viewport(0, 0, static_cast<GLsizei>(pass->width), static_cast<GLsizei>(pass->height));
    gl->UseProgram(pass->program);

    gl->ActiveTexture(GL_TEXTURE0);
    gl->BindTexture(GL_TEXTURE_2D, input);
    GLint filter = pass->linear ? GL_LINEAR : GL_NEAREST;
    gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    if (pass->source >= 0)
    {
        gl->Uniform1i(pass->source, 0);
    }
    if (stage->lut != 0)
    {
        gl->ActiveTexture(GL_TEXTURE1);
        gl->BindTexture(GL_TEXTURE_2D, stage->lut);
        if (pass->lut >= 0)
        {
            gl->Uniform1i(pass->lut, 1);
        }
        set_size(gl, pass->lut_size, stage->lut_width, stage->lut_height);
        gl->ActiveTexture(GL_TEXTURE0);
    }
    set_size(gl, pass->source_size, input_width, input_height);
    set_size(gl, pass->output_size, pass->width, pass->height);
    if (pass->frame_count >= 0)
    {
        // fp20 on the Tegra 4: wrap early so the count stays exact.
        gl->Uniform1f(pass->frame_count, static_cast<float>(stage->frame_count % 4096));
    }

    gl->BindBuffer(GL_ARRAY_BUFFER, context->copy.quad);
    gl->VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    gl->EnableVertexAttribArray(0);
    gl->DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    gl->BindBuffer(GL_ARRAY_BUFFER, 0);
}

void gl_shader_chain_record(GlShaderChain* chain, GlContext* context, const GlStreamingTexture* source)
{
    if (!chain->ready)
    {
        return;
    }
    GlFunctions* gl = &context->gl;
    const ViewportResult* viewport = &chain->viewport;

    // The shown part (crop included) into the input, exact size.
    float capacity_width = static_cast<float>(source->capacity_width);
    float capacity_height = static_cast<float>(source->capacity_height);
    float rect[4] = { static_cast<float>(viewport->source_x) / capacity_width,
                      static_cast<float>(viewport->source_y) / capacity_height,
                      static_cast<float>(viewport->source_width) / capacity_width,
                      static_cast<float>(viewport->source_height) / capacity_height };
    gl->BindFramebuffer(GL_FRAMEBUFFER, chain->input_framebuffer);
    gl->Viewport(0, 0, static_cast<GLsizei>(chain->input_width), static_cast<GLsizei>(chain->input_height));
    gl_copy_draw(context, source->shown, rect, false, false);

    GLuint input = chain->input_texture;
    uint32_t input_width = chain->input_width;
    uint32_t input_height = chain->input_height;
    for (uint32_t s = 0; s < GL_SHADER_STAGES; s++)
    {
        GlShaderStage* stage = &chain->stages[s];
        for (uint32_t i = 0; i < stage->pass_count; i++)
        {
            GlShaderPass* pass = &stage->passes[i];
            run_pass(pass, stage, context, input, input_width, input_height);
            input = pass->texture;
            input_width = pass->width;
            input_height = pass->height;
        }
        if (stage->pass_count > 0)
        {
            stage->frame_count++;
        }
    }
    gl->UseProgram(0);
    gl->BindFramebuffer(GL_FRAMEBUFFER, 0);
    chain->written = true;
    gl_check_errors(context, "GL shader chain");
}

bool gl_shader_chain_stage0_result(const GlShaderChain* chain, uint64_t* imgui_id, uint32_t* width,
                                   uint32_t* height)
{
    const GlShaderStage* stage0 = &chain->stages[0];
    if (!chain->ready || !chain->written || stage0->pass_count == 0 || chain->stages[1].pass_count == 0)
    {
        return false;
    }
    const GlShaderPass* pass = &stage0->passes[stage0->pass_count - 1];
    *imgui_id = pass->texture;
    *width = pass->width;
    *height = pass->height;
    return true;
}

void gl_shader_chain_destroy(GlShaderChain* chain, GlContext* context)
{
    for (uint32_t s = 0; s < GL_SHADER_STAGES; s++)
    {
        clear_stage(chain, context, s);
    }
    gl_render_target_destroy(context, &chain->input_texture, &chain->input_framebuffer);
    *chain = GlShaderChain{};
}
