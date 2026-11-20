#include "gl_context.h"

#include <cstdio>
#include <cstring>

#include <SDL3/SDL_log.h>

// ---------------------------------------------------------------------------
// Functions
// ---------------------------------------------------------------------------

static SDL_FunctionPointer load(const char* name, bool* ok)
{
    SDL_FunctionPointer function = SDL_GL_GetProcAddress(name);
    if (function == nullptr)
    {
        SDL_LogError(SDL_LOG_CATEGORY_GPU, "GL: %s is missing", name);
        *ok = false;
    }
    return function;
}

// Function pointers from SDL's loader: the only reinterpret_casts here
// (STYLE.md: function pointers from loaders).
static bool load_functions(GlFunctions* f)
{
    bool ok = true;
    f->GetString = reinterpret_cast<PFNGLGETSTRINGPROC>(load("glGetString", &ok));
    f->GetIntegerv = reinterpret_cast<PFNGLGETINTEGERVPROC>(load("glGetIntegerv", &ok));
    f->GetError = reinterpret_cast<PFNGLGETERRORPROC>(load("glGetError", &ok));
    f->Viewport = reinterpret_cast<PFNGLVIEWPORTPROC>(load("glViewport", &ok));
    f->ClearColor = reinterpret_cast<PFNGLCLEARCOLORPROC>(load("glClearColor", &ok));
    f->Clear = reinterpret_cast<PFNGLCLEARPROC>(load("glClear", &ok));
    f->Enable = reinterpret_cast<PFNGLENABLEPROC>(load("glEnable", &ok));
    f->Disable = reinterpret_cast<PFNGLDISABLEPROC>(load("glDisable", &ok));
    f->Finish = reinterpret_cast<PFNGLFINISHPROC>(load("glFinish", &ok));
    f->GenTextures = reinterpret_cast<PFNGLGENTEXTURESPROC>(load("glGenTextures", &ok));
    f->DeleteTextures = reinterpret_cast<PFNGLDELETETEXTURESPROC>(load("glDeleteTextures", &ok));
    f->BindTexture = reinterpret_cast<PFNGLBINDTEXTUREPROC>(load("glBindTexture", &ok));
    f->ActiveTexture = reinterpret_cast<PFNGLACTIVETEXTUREPROC>(load("glActiveTexture", &ok));
    f->TexImage2D = reinterpret_cast<PFNGLTEXIMAGE2DPROC>(load("glTexImage2D", &ok));
    f->TexSubImage2D = reinterpret_cast<PFNGLTEXSUBIMAGE2DPROC>(load("glTexSubImage2D", &ok));
    f->TexParameteri = reinterpret_cast<PFNGLTEXPARAMETERIPROC>(load("glTexParameteri", &ok));
    f->PixelStorei = reinterpret_cast<PFNGLPIXELSTOREIPROC>(load("glPixelStorei", &ok));
    f->GenFramebuffers = reinterpret_cast<PFNGLGENFRAMEBUFFERSPROC>(load("glGenFramebuffers", &ok));
    f->DeleteFramebuffers = reinterpret_cast<PFNGLDELETEFRAMEBUFFERSPROC>(load("glDeleteFramebuffers", &ok));
    f->BindFramebuffer = reinterpret_cast<PFNGLBINDFRAMEBUFFERPROC>(load("glBindFramebuffer", &ok));
    f->FramebufferTexture2D = reinterpret_cast<PFNGLFRAMEBUFFERTEXTURE2DPROC>(load("glFramebufferTexture2D", &ok));
    f->CheckFramebufferStatus =
        reinterpret_cast<PFNGLCHECKFRAMEBUFFERSTATUSPROC>(load("glCheckFramebufferStatus", &ok));
    f->CreateShader = reinterpret_cast<PFNGLCREATESHADERPROC>(load("glCreateShader", &ok));
    f->ShaderSource = reinterpret_cast<PFNGLSHADERSOURCEPROC>(load("glShaderSource", &ok));
    f->CompileShader = reinterpret_cast<PFNGLCOMPILESHADERPROC>(load("glCompileShader", &ok));
    f->GetShaderiv = reinterpret_cast<PFNGLGETSHADERIVPROC>(load("glGetShaderiv", &ok));
    f->GetShaderInfoLog = reinterpret_cast<PFNGLGETSHADERINFOLOGPROC>(load("glGetShaderInfoLog", &ok));
    f->DeleteShader = reinterpret_cast<PFNGLDELETESHADERPROC>(load("glDeleteShader", &ok));
    f->CreateProgram = reinterpret_cast<PFNGLCREATEPROGRAMPROC>(load("glCreateProgram", &ok));
    f->AttachShader = reinterpret_cast<PFNGLATTACHSHADERPROC>(load("glAttachShader", &ok));
    f->BindAttribLocation = reinterpret_cast<PFNGLBINDATTRIBLOCATIONPROC>(load("glBindAttribLocation", &ok));
    f->LinkProgram = reinterpret_cast<PFNGLLINKPROGRAMPROC>(load("glLinkProgram", &ok));
    f->GetProgramiv = reinterpret_cast<PFNGLGETPROGRAMIVPROC>(load("glGetProgramiv", &ok));
    f->GetProgramInfoLog = reinterpret_cast<PFNGLGETPROGRAMINFOLOGPROC>(load("glGetProgramInfoLog", &ok));
    f->DeleteProgram = reinterpret_cast<PFNGLDELETEPROGRAMPROC>(load("glDeleteProgram", &ok));
    f->UseProgram = reinterpret_cast<PFNGLUSEPROGRAMPROC>(load("glUseProgram", &ok));
    f->GetUniformLocation = reinterpret_cast<PFNGLGETUNIFORMLOCATIONPROC>(load("glGetUniformLocation", &ok));
    f->Uniform1i = reinterpret_cast<PFNGLUNIFORM1IPROC>(load("glUniform1i", &ok));
    f->Uniform1f = reinterpret_cast<PFNGLUNIFORM1FPROC>(load("glUniform1f", &ok));
    f->Uniform4f = reinterpret_cast<PFNGLUNIFORM4FPROC>(load("glUniform4f", &ok));
    f->GenBuffers = reinterpret_cast<PFNGLGENBUFFERSPROC>(load("glGenBuffers", &ok));
    f->DeleteBuffers = reinterpret_cast<PFNGLDELETEBUFFERSPROC>(load("glDeleteBuffers", &ok));
    f->BindBuffer = reinterpret_cast<PFNGLBINDBUFFERPROC>(load("glBindBuffer", &ok));
    f->BufferData = reinterpret_cast<PFNGLBUFFERDATAPROC>(load("glBufferData", &ok));
    f->VertexAttribPointer = reinterpret_cast<PFNGLVERTEXATTRIBPOINTERPROC>(load("glVertexAttribPointer", &ok));
    f->EnableVertexAttribArray =
        reinterpret_cast<PFNGLENABLEVERTEXATTRIBARRAYPROC>(load("glEnableVertexAttribArray", &ok));
    f->DrawArrays = reinterpret_cast<PFNGLDRAWARRAYSPROC>(load("glDrawArrays", &ok));
    f->ReadPixels = reinterpret_cast<PFNGLREADPIXELSPROC>(load("glReadPixels", &ok));
    return ok;
}

// ---------------------------------------------------------------------------
// The copy program
// ---------------------------------------------------------------------------

// GLSL ES 1.00 (also valid in GLES 3 and WebGL 2). The quad is 0..1 in both
// axes; texture row 0 (the frame's top row, as uploaded) is at t = 0, and it
// lands at the framebuffer's y = 0, so a copy keeps the uploaded orientation
// (ImGui then draws t = 0 at the top).
static const char* COPY_VERTEX_SHADER =
    "attribute vec2 a_position;\n"
    "uniform vec4 u_source_rect;\n"
    "varying vec2 v_coord;\n"
    "void main()\n"
    "{\n"
    "    v_coord = u_source_rect.xy + a_position * u_source_rect.zw;\n"
    "    gl_Position = vec4(a_position * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

// mediump: the Tegra 4 computes highp and mediump alike (fp20) and texture
// coordinates must be mediump there; it holds 0..1 coordinates of textures
// up to ~2048 texels exactly enough for nearest sampling.
static const char* COPY_FRAGMENT_SHADER =
    "precision mediump float;\n"
    "uniform sampler2D u_texture;\n"
    "uniform float u_swap_red_blue;\n"
    "varying vec2 v_coord;\n"
    "void main()\n"
    "{\n"
    "    vec4 color = texture2D(u_texture, v_coord);\n"
    "    gl_FragColor = vec4(mix(color.rgb, color.bgr, u_swap_red_blue), 1.0);\n"
    "}\n";

static GLuint compile_shader(GlContext* context, GLenum type, const char* source)
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
        SDL_LogError(SDL_LOG_CATEGORY_GPU, "GL: %s shader didn't compile: %s",
                     type == GL_VERTEX_SHADER ? "vertex" : "fragment", log);
        gl->DeleteShader(shader);
        return 0;
    }
    return shader;
}

static bool create_copy_program(GlContext* context)
{
    GlFunctions* gl = &context->gl;
    GlCopyProgram* copy = &context->copy;
    GLuint vertex = compile_shader(context, GL_VERTEX_SHADER, COPY_VERTEX_SHADER);
    GLuint fragment = compile_shader(context, GL_FRAGMENT_SHADER, COPY_FRAGMENT_SHADER);
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
    copy->program = gl->CreateProgram();
    gl->AttachShader(copy->program, vertex);
    gl->AttachShader(copy->program, fragment);
    gl->BindAttribLocation(copy->program, 0, "a_position");
    gl->LinkProgram(copy->program);
    gl->DeleteShader(vertex);     // the program keeps them while it lives
    gl->DeleteShader(fragment);
    GLint linked = GL_FALSE;
    gl->GetProgramiv(copy->program, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE)
    {
        char log[1024] = {};
        gl->GetProgramInfoLog(copy->program, sizeof(log), nullptr, log);
        SDL_LogError(SDL_LOG_CATEGORY_GPU, "GL: the copy program didn't link: %s", log);
        return false;
    }
    copy->source_rect = gl->GetUniformLocation(copy->program, "u_source_rect");
    copy->swap_red_blue = gl->GetUniformLocation(copy->program, "u_swap_red_blue");
    copy->texture = gl->GetUniformLocation(copy->program, "u_texture");

    static const GLfloat QUAD[8] = { 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f };
    gl->GenBuffers(1, &copy->quad);
    gl->BindBuffer(GL_ARRAY_BUFFER, copy->quad);
    gl->BufferData(GL_ARRAY_BUFFER, sizeof(QUAD), QUAD, GL_STATIC_DRAW);
    gl->BindBuffer(GL_ARRAY_BUFFER, 0);
    return true;
}

void gl_copy_draw(GlContext* context, GLuint texture, const float source_rect[4], bool swap_red_blue, bool linear)
{
    GlFunctions* gl = &context->gl;
    GlCopyProgram* copy = &context->copy;
    // ImGui sets and restores its own state around its draws; ours runs
    // between them with these off.
    gl->Disable(GL_BLEND);
    gl->Disable(GL_SCISSOR_TEST);
    gl->Disable(GL_DEPTH_TEST);
    gl->Disable(GL_CULL_FACE);

    gl->UseProgram(copy->program);
    gl->ActiveTexture(GL_TEXTURE0);
    gl->BindTexture(GL_TEXTURE_2D, texture);
    GLint filter = linear ? GL_LINEAR : GL_NEAREST;
    gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    gl->Uniform1i(copy->texture, 0);
    gl->Uniform4f(copy->source_rect, source_rect[0], source_rect[1], source_rect[2], source_rect[3]);
    gl->Uniform1f(copy->swap_red_blue, swap_red_blue ? 1.0f : 0.0f);

    gl->BindBuffer(GL_ARRAY_BUFFER, copy->quad);
    gl->VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    gl->EnableVertexAttribArray(0);
    gl->DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    gl->BindBuffer(GL_ARRAY_BUFFER, 0);
    gl->UseProgram(0);
}

// ---------------------------------------------------------------------------
// Context
// ---------------------------------------------------------------------------

static void set_version_attributes(int32_t major)
{
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, major);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
}

void gl_context_set_attributes(bool gles2)
{
    SDL_GL_ResetAttributes();
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    set_version_attributes(gles2 ? 2 : 3);
}

static void copy_string(char* destination, size_t size, const GLubyte* source)
{
    std::snprintf(destination, size, "%s", source != nullptr ? static_cast<const char*>(static_cast<const void*>(source)) : "?");
}

bool gl_context_create(GlContext* context, SDL_Window* window, bool gles2)
{
    *context = GlContext{};
    context->context = SDL_GL_CreateContext(window);
    if (context->context == nullptr)
    {
        SDL_LogError(SDL_LOG_CATEGORY_GPU, "SDL_GL_CreateContext (GLES %d.0) failed: %s", gles2 ? 2 : 3,
                     SDL_GetError());
        return false;
    }
    if (!load_functions(&context->gl))
    {
        return false;
    }
    SDL_GL_GetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, &context->major);
    SDL_GL_GetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, &context->minor);
    GlFunctions* gl = &context->gl;
    copy_string(context->renderer, sizeof(context->renderer), gl->GetString(GL_RENDERER));
    copy_string(context->version, sizeof(context->version), gl->GetString(GL_VERSION));
    gl->GetIntegerv(GL_MAX_TEXTURE_SIZE, &context->max_texture_size);
    SDL_LogInfo(SDL_LOG_CATEGORY_GPU, "OpenGL ES context %d.%d: %s, %s, max texture %d", context->major,
                context->minor, context->renderer, context->version, context->max_texture_size);

    if (!create_copy_program(context))
    {
        return false;
    }
    gl_check_errors(context, "GL context setup");
    return true;
}

void gl_context_destroy(GlContext* context)
{
    if (context->context == nullptr)
    {
        return;
    }
    GlFunctions* gl = &context->gl;
    if (gl->DeleteProgram != nullptr && context->copy.program != 0)
    {
        gl->DeleteProgram(context->copy.program);
    }
    if (gl->DeleteBuffers != nullptr && context->copy.quad != 0)
    {
        gl->DeleteBuffers(1, &context->copy.quad);
    }
    SDL_GL_DestroyContext(context->context);
    *context = GlContext{};
}

const char* gl_context_imgui_glsl_version(const GlContext* context)
{
    return context->major >= 3 ? "#version 300 es" : "#version 100";
}

void gl_check_errors(GlContext* context, const char* where)
{
#if defined(NDEBUG)
    // Release builds don't ask: glGetError can stall the pipeline on some drivers.
    static_cast<void>(context);
    static_cast<void>(where);
#else
    // A few at most: GL keeps one flag per error kind.
    for (uint32_t i = 0; i < 8; i++)
    {
        GLenum error = context->gl.GetError();
        if (error == GL_NO_ERROR)
        {
            return;
        }
        SDL_LogError(SDL_LOG_CATEGORY_GPU, "GL error 0x%04X after %s", static_cast<uint32_t>(error), where);
    }
#endif
}
