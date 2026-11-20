// The OpenGL ES path's picture: each libretro pixel format uploaded through
// a VideoTexture comes out with the right colors (red and blue swapped back
// for XRGB8888, 0RGB1555 converted, alpha 1), and the sharp bilinear copy
// scales it up with nearest. Reads the pixels back from the GPU. Needs a
// display: without one (or without GLES) it says SKIP and exits 77.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <SDL3/SDL.h>

#include "app_shell.h"
#include "gl_picture.h"
#include "gl_shader_chain.h"
#include "host_fs.h"
#include "video_texture.h"

static int g_failures = 0;

// Every pixel of a 2x2 picture: red, green / blue, white (row 0 first).
static const uint8_t EXPECTED[4][3] = { { 255, 0, 0 }, { 0, 255, 0 }, { 0, 0, 255 }, { 255, 255, 255 } };

static bool close_to(uint8_t value, uint8_t expected)
{
    int32_t difference = static_cast<int32_t>(value) - static_cast<int32_t>(expected);
    return difference >= -2 && difference <= 2;
}

// Reads `width` x `height` RGBA pixels of `framebuffer` and checks them
// against EXPECTED, each picture pixel `scale` x `scale` big.
static void check_pixels(AppShell* shell, GLuint framebuffer, uint32_t width, uint32_t height, uint32_t scale,
                         const char* what)
{
    GlFunctions* gl = &shell->gl.gl;
    uint8_t pixels[16 * 16 * 4] = {};
    gl->BindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    gl->ReadPixels(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height), GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    gl->BindFramebuffer(GL_FRAMEBUFFER, 0);
    for (uint32_t y = 0; y < height; y++)
    {
        for (uint32_t x = 0; x < width; x++)
        {
            const uint8_t* pixel = &pixels[(y * width + x) * 4];
            const uint8_t* expected = EXPECTED[(y / scale) * 2 + (x / scale)];
            if (!close_to(pixel[0], expected[0]) || !close_to(pixel[1], expected[1]) ||
                !close_to(pixel[2], expected[2]) || pixel[3] != 255)
            {
                std::printf("FAIL %s: pixel %u,%u is %u %u %u %u, expected %u %u %u 255\n", what, x, y, pixel[0],
                            pixel[1], pixel[2], pixel[3], expected[0], expected[1], expected[2]);
                g_failures++;
                return;
            }
        }
    }
    std::printf("ok %s\n", what);
}

static void check_format(AppShell* shell, FramePixelFormat format, const void* pixels, const char* name)
{
    VideoTexture texture = {};
    // A capacity bigger than the frame, odd-sized: uploads go in a corner.
    if (!video_texture_create(&texture, shell, 5, 3, format, true))
    {
        std::printf("FAIL %s: no texture\n", name);
        g_failures++;
        return;
    }
    if (!app_shell_begin_frame(shell) || !app_shell_acquire(shell))
    {
        std::printf("FAIL %s: no frame\n", name);
        g_failures++;
        video_texture_destroy(&texture, shell);
        return;
    }
    if (!video_texture_upload(&texture, shell, pixels, 2, 2))
    {
        std::printf("FAIL %s: upload\n", name);
        g_failures++;
    }
    check_pixels(shell, texture.gl.framebuffer, 2, 2, 1, name);

    // Sharp bilinear at x3: the copy is the picture scaled up by 3, nearest.
    ViewportResult viewport = {};
    viewport.width = 6;
    viewport.height = 6;
    viewport.source_width = 2;
    viewport.source_height = 2;
    viewport.filter = ViewportFilter::sharp_bilinear;
    viewport.prescale_x = 3;
    viewport.prescale_y = 3;
    GlPicture picture = {};
    if (!gl_picture_prepare(&picture, &shell->gl, &texture.gl, texture.has_content, &viewport) || !picture.use_copy)
    {
        std::printf("FAIL %s: no picture copy\n", name);
        g_failures++;
    }
    else
    {
        gl_picture_record(&picture, &shell->gl, &texture.gl);
        char what[64] = {};
        std::snprintf(what, sizeof(what), "%s, sharp bilinear copy", name);
        check_pixels(shell, picture.framebuffer, 6, 6, 3, what);
    }
    gl_picture_destroy(&picture, &shell->gl);
    app_shell_present(shell, ClearColor{ 0.0f, 0.0f, 0.0f, 1.0f });
    video_texture_destroy(&texture, shell);
}

static bool write_text(const std::string& path, const char* text)
{
    return host_fs_write_file(path.c_str(), text, std::strlen(text));
}

// Inverts the picture times u_gain (set through set_param).
static const char* INVERT_SHADER =
    "uniform float u_gain;\n"
    "void main()\n"
    "{\n"
    "    gl_FragColor = vec4((1.0 - texture2D(u_source, v_coord).rgb) * u_gain, 1.0);\n"
    "}\n";

// A two-stage chain from preset files: stage 0 inverts at the source's size,
// stage 1 inverts again at the picture's size (x3, nearest): the picture
// comes back as it was, three times bigger. Then u_gain 0.5 in stage 1.
static void check_chain(AppShell* shell, const char* folder)
{
    std::string base = folder;
    if (!host_fs_make_directories(folder) || !write_text(base + "/invert.glsl", INVERT_SHADER) ||
        !write_text(base + "/stage0.glpreset", "passes = 1\npass0 = invert.glsl\npass0_scale = source\n") ||
        !write_text(base + "/stage1.glpreset", "passes = 1\npass0 = invert.glsl\npass0_scale = viewport\n"))
    {
        std::printf("FAIL chain: can't write the presets in %s\n", folder);
        g_failures++;
        return;
    }
    GlShaderChain chain = {};
    if (!gl_shader_chain_load(&chain, &shell->gl, 0, (base + "/stage0.glpreset").c_str()) ||
        !gl_shader_chain_load(&chain, &shell->gl, 1, (base + "/stage1.glpreset").c_str()))
    {
        std::printf("FAIL chain: presets didn't load\n");
        g_failures++;
        gl_shader_chain_destroy(&chain, &shell->gl);
        return;
    }
    gl_shader_chain_set_param(&chain, &shell->gl, 0, "u_gain", 1.0f);
    gl_shader_chain_set_param(&chain, &shell->gl, 1, "u_gain", 1.0f);

    VideoTexture texture = {};
    const uint16_t rgb565[4] = { 0xF800, 0x07E0, 0x001F, 0xFFFF };
    ViewportResult viewport = {};
    viewport.width = 6;
    viewport.height = 6;
    viewport.source_width = 2;
    viewport.source_height = 2;
    if (!video_texture_create(&texture, shell, 4, 4, FramePixelFormat::rgb565, true) ||
        !app_shell_begin_frame(shell) || !app_shell_acquire(shell) ||
        !video_texture_upload(&texture, shell, rgb565, 2, 2) ||
        !gl_shader_chain_prepare(&chain, &shell->gl, &texture.gl, texture.has_content, &viewport))
    {
        std::printf("FAIL chain: no frame\n");
        g_failures++;
    }
    else
    {
        gl_shader_chain_record(&chain, &shell->gl, &texture.gl);
        check_pixels(shell, chain.stages[1].passes[0].framebuffer, 6, 6, 3, "chain, two stages");
        uint64_t middle = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        if (!gl_shader_chain_stage0_result(&chain, &middle, &width, &height) || width != 2 || height != 2)
        {
            std::printf("FAIL chain: stage 0's result\n");
            g_failures++;
        }
        // Gain 0 in stage 1: black everywhere.
        if (!gl_shader_chain_set_param(&chain, &shell->gl, 1, "u_gain", 0.0f))
        {
            std::printf("FAIL chain: set_param found no u_gain\n");
            g_failures++;
        }
        gl_shader_chain_record(&chain, &shell->gl, &texture.gl);
        uint8_t pixel[4] = {};
        shell->gl.gl.BindFramebuffer(GL_FRAMEBUFFER, chain.stages[1].passes[0].framebuffer);
        shell->gl.gl.ReadPixels(1, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
        shell->gl.gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
        if (pixel[0] != 0 || pixel[1] != 0 || pixel[2] != 0)
        {
            std::printf("FAIL chain: gain 0 left %u %u %u\n", pixel[0], pixel[1], pixel[2]);
            g_failures++;
        }
        else
        {
            std::printf("ok chain, parameter\n");
        }
        app_shell_present(shell, ClearColor{ 0.0f, 0.0f, 0.0f, 1.0f });
    }
    gl_shader_chain_destroy(&chain, &shell->gl);
    video_texture_destroy(&texture, shell);
}

int main(int argc, char** argv)
{
    SDL_setenv_unsafe("MULTITAP_GPU", "gl", 1);
    AppShell shell = {};
    AppShellConfig config = {};
    config.name = "gl_video_test";
    config.window_width = 64;
    config.window_height = 64;
    if (!app_shell_create(&shell, &config))
    {
        std::printf("SKIP: no display or no OpenGL ES\n");
        app_shell_destroy(&shell);
        return 77;
    }

    const uint16_t rgb565[4] = { 0xF800, 0x07E0, 0x001F, 0xFFFF };
    check_format(&shell, FramePixelFormat::rgb565, rgb565, "RGB565");
    // The top bit is unused: set on two pixels, which must not matter.
    const uint16_t xrgb1555[4] = { 0x7C00, 0x83E0, 0x001F, 0xFFFF };
    check_format(&shell, FramePixelFormat::xrgb1555, xrgb1555, "0RGB1555");
    // X = 0 on purpose: alpha must still come out 1.
    const uint32_t xrgb8888[4] = { 0x00FF0000u, 0x0000FF00u, 0x000000FFu, 0x00FFFFFFu };
    check_format(&shell, FramePixelFormat::xrgb8888, xrgb8888, "XRGB8888");
    // argv[1]: a scratch folder for the chain's preset files.
    check_chain(&shell, argc > 1 ? argv[1] : "gl_video_test");

    gl_check_errors(&shell.gl, "gl video test");
    app_shell_destroy(&shell);
    if (g_failures != 0)
    {
        std::printf("FAIL: %d checks failed\n", g_failures);
        return 1;
    }
    std::printf("gl video: all checks passed\n");
    return 0;
}
