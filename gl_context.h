#pragma once

// OpenGL ES for the window, when Vulkan isn't there (piccolo's docs/gl.md):
// the context SDL makes, the GL functions we use (looked up at run time, so
// nothing links against a GL library), and the one shader program the GL
// path needs for itself (copy).
//
// One code path for GLES 2.0 (Tegra 4), GLES 3.x (phones, Mali, Adreno,
// Mesa) and WebGL 2: only GLES 2.0 calls and GLSL ES 1.00 shaders (GLES 3 and
// WebGL 2 accept both). Desktop GL (3.3+, with librashader) comes later.
//
// The copy program draws a rectangle of a texture into whatever framebuffer
// is bound, with nearest or linear sampling, optionally swapping red and
// blue: a core's XRGB8888 frame is uploaded as RGBA bytes (B, G, R, X in
// memory) because GLES has no BGRA upload everywhere (WebGL never), so the
// copy turns it into RGB.

#include <cstdint>

#include <SDL3/SDL_opengles2.h>
#include <SDL3/SDL_video.h>

struct GlFunctions
{
    PFNGLGETSTRINGPROC               GetString               = nullptr;
    PFNGLGETINTEGERVPROC             GetIntegerv             = nullptr;
    PFNGLGETERRORPROC                GetError                = nullptr;
    PFNGLVIEWPORTPROC                Viewport                = nullptr;
    PFNGLCLEARCOLORPROC              ClearColor              = nullptr;
    PFNGLCLEARPROC                   Clear                   = nullptr;
    PFNGLENABLEPROC                  Enable                  = nullptr;
    PFNGLDISABLEPROC                 Disable                 = nullptr;
    PFNGLFINISHPROC                  Finish                  = nullptr;
    PFNGLGENTEXTURESPROC             GenTextures             = nullptr;
    PFNGLDELETETEXTURESPROC          DeleteTextures          = nullptr;
    PFNGLBINDTEXTUREPROC             BindTexture             = nullptr;
    PFNGLACTIVETEXTUREPROC           ActiveTexture           = nullptr;
    PFNGLTEXIMAGE2DPROC              TexImage2D              = nullptr;
    PFNGLTEXSUBIMAGE2DPROC           TexSubImage2D           = nullptr;
    PFNGLTEXPARAMETERIPROC           TexParameteri           = nullptr;
    PFNGLPIXELSTOREIPROC             PixelStorei             = nullptr;
    PFNGLGENFRAMEBUFFERSPROC         GenFramebuffers         = nullptr;
    PFNGLDELETEFRAMEBUFFERSPROC      DeleteFramebuffers      = nullptr;
    PFNGLBINDFRAMEBUFFERPROC         BindFramebuffer         = nullptr;
    PFNGLFRAMEBUFFERTEXTURE2DPROC    FramebufferTexture2D    = nullptr;
    PFNGLCHECKFRAMEBUFFERSTATUSPROC  CheckFramebufferStatus  = nullptr;
    PFNGLCREATESHADERPROC            CreateShader            = nullptr;
    PFNGLSHADERSOURCEPROC            ShaderSource            = nullptr;
    PFNGLCOMPILESHADERPROC           CompileShader           = nullptr;
    PFNGLGETSHADERIVPROC             GetShaderiv             = nullptr;
    PFNGLGETSHADERINFOLOGPROC        GetShaderInfoLog        = nullptr;
    PFNGLDELETESHADERPROC            DeleteShader            = nullptr;
    PFNGLCREATEPROGRAMPROC           CreateProgram           = nullptr;
    PFNGLATTACHSHADERPROC            AttachShader            = nullptr;
    PFNGLBINDATTRIBLOCATIONPROC      BindAttribLocation      = nullptr;
    PFNGLLINKPROGRAMPROC             LinkProgram             = nullptr;
    PFNGLGETPROGRAMIVPROC            GetProgramiv            = nullptr;
    PFNGLGETPROGRAMINFOLOGPROC       GetProgramInfoLog       = nullptr;
    PFNGLDELETEPROGRAMPROC           DeleteProgram           = nullptr;
    PFNGLUSEPROGRAMPROC              UseProgram              = nullptr;
    PFNGLGETUNIFORMLOCATIONPROC      GetUniformLocation      = nullptr;
    PFNGLUNIFORM1IPROC               Uniform1i               = nullptr;
    PFNGLUNIFORM1FPROC               Uniform1f               = nullptr;
    PFNGLUNIFORM4FPROC               Uniform4f               = nullptr;
    PFNGLGENBUFFERSPROC              GenBuffers              = nullptr;
    PFNGLDELETEBUFFERSPROC           DeleteBuffers           = nullptr;
    PFNGLBINDBUFFERPROC              BindBuffer              = nullptr;
    PFNGLBUFFERDATAPROC              BufferData              = nullptr;
    PFNGLVERTEXATTRIBPOINTERPROC     VertexAttribPointer     = nullptr;
    PFNGLENABLEVERTEXATTRIBARRAYPROC EnableVertexAttribArray = nullptr;
    PFNGLDRAWARRAYSPROC              DrawArrays              = nullptr;
    PFNGLREADPIXELSPROC              ReadPixels              = nullptr;   // tests
};

// The copy program (see the top of this file).
struct GlCopyProgram
{
    GLuint program        = 0;
    GLuint quad           = 0;    // vertex buffer: a 0..1 square as a triangle strip
    GLint  source_rect    = -1;   // uniform vec4: x, y, width, height in texture coordinates
    GLint  swap_red_blue  = -1;   // uniform float: 1 = the texture holds B, G, R
    GLint  texture        = -1;   // uniform sampler2D
};

struct GlContext
{
    SDL_GLContext context = nullptr;
    GlFunctions   gl      = {};
    int32_t       major   = 0;     // the context's GLES version (2 or 3)
    int32_t       minor   = 0;
    int32_t       max_texture_size = 0;
    char          renderer[128] = {};   // GL_RENDERER
    char          version[128]  = {};   // GL_VERSION
    GlCopyProgram copy    = {};
    int32_t       swap_interval = 1;    // what SDL accepted (0 = no vsync)
};

// Asks SDL for a GLES context on `window` (made with SDL_WINDOW_OPENGL):
// 2.0 when `gles2` (a build whose ImGui backend is GLES 2's:
// MULTITAP_GL_ES2), else 3.0 (ImGui's backend in its desktop mode handles
// GLES 3 but not 2). Call gl_context_set_attributes() before the window is
// created. False (logged) on failure; gl_context_destroy() cleans up.
void gl_context_set_attributes(bool gles2);
bool gl_context_create(GlContext* context, SDL_Window* window, bool gles2);
void gl_context_destroy(GlContext* context);

// The GLSL version line for ImGui's GL backend on this context.
const char* gl_context_imgui_glsl_version(const GlContext* context);

// Draws `source_rect` (texture coordinates: x, y, width, height) of `texture`
// over the whole viewport of the bound framebuffer. `linear` picks the
// sampling (it sets the texture's filter: ImGui changes it too, so it's set
// on every copy).
void gl_copy_draw(GlContext* context, GLuint texture, const float source_rect[4], bool swap_red_blue, bool linear);

// Logs and clears GL errors (debug builds call it after each step; `where` names the step).
void gl_check_errors(GlContext* context, const char* where);
