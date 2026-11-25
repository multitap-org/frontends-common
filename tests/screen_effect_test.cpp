// The screen effect pass (screen_effect.h, gl_screen_effect.h) through
// video_output, on a real window: GPU captures of a known 2x2 picture come
// out exact (nearest from the core's texture, and from the picture's sharp
// bilinear copy), a new picture size remakes the target, frames with every
// knob turned run, and on GLES the knobs' colors are right (read from the
// target). Any Vulkan validation warning or error fails it.
//
//   screen_effect_test vulkan | vulkan11 | gl
//
// Needs a display: without one (or without that API) it says SKIP and exits 77.

#include <cstdint>
#include <cstdio>
#include <cstring>

#include <SDL3/SDL.h>

#include "imgui.h"

#include "app_shell.h"
#include "video_output.h"
#include "video_texture.h"

static int g_failures = 0;
static int g_validation_messages = 0;
static SDL_LogOutputFunction g_default_log = nullptr;
static void* g_default_log_data = nullptr;

// Counts the validation layer's warnings and errors ("[vulkan] ..." from
// vk_context's messenger), and still prints everything.
static void log_output(void* data, int category, SDL_LogPriority priority, const char* message)
{
    if (priority >= SDL_LOG_PRIORITY_WARN && std::strncmp(message, "[vulkan]", 8) == 0)
    {
        g_validation_messages++;
    }
    g_default_log(data == nullptr ? g_default_log_data : data, category, priority, message);
}

// Every pixel of the 2x2 picture: red, green / blue, white (row 0 first).
static const uint8_t EXPECTED[4][3] = { { 255, 0, 0 }, { 0, 255, 0 }, { 0, 0, 255 }, { 255, 255, 255 } };

static bool close_to(uint8_t value, uint8_t expected)
{
    int32_t difference = static_cast<int32_t>(value) - static_cast<int32_t>(expected);
    return difference >= -2 && difference <= 2;
}

static void check_capture(const VideoCapture* capture, uint32_t size, const char* what)
{
    if (capture->width != size || capture->height != size || capture->pixels == nullptr)
    {
        std::printf("FAIL %s: capture is %ux%u, expected %ux%u\n", what, capture->width, capture->height, size, size);
        g_failures++;
        return;
    }
    uint32_t scale = size / 2;
    for (uint32_t y = 0; y < size; y++)
    {
        for (uint32_t x = 0; x < size; x++)
        {
            const uint8_t* pixel = &capture->pixels[y * capture->pitch + x * 4];
            uint8_t red = capture->format == VideoCaptureFormat::bgra8 ? pixel[2] : pixel[0];
            uint8_t blue = capture->format == VideoCaptureFormat::bgra8 ? pixel[0] : pixel[2];
            const uint8_t* expected = EXPECTED[(y / scale) * 2 + (x / scale)];
            if (!close_to(red, expected[0]) || !close_to(pixel[1], expected[1]) || !close_to(blue, expected[2]) ||
                pixel[3] != 255)
            {
                std::printf("FAIL %s: pixel %u,%u is %u %u %u %u\n", what, x, y, red, pixel[1], blue, pixel[3]);
                g_failures++;
                return;
            }
        }
    }
    std::printf("ok %s\n", what);
}

static ViewportResult picture_viewport(uint32_t size, ViewportFilter filter)
{
    ViewportResult viewport = {};
    viewport.width = size;
    viewport.height = size;
    viewport.source_width = 2;
    viewport.source_height = 2;
    viewport.filter = filter;
    viewport.prescale_x = size / 2;
    viewport.prescale_y = size / 2;
    return viewport;
}

// What a frontend's game view widget does with the ShownImage (piccolo:
// retro_ui_draw_shown_image), with ImGui as the shell's UI.
static void draw_shown_image(const ShownImage* image)
{
    const ImGuiPlatformIO& platform_io = ImGui::GetPlatformIO();
    ImDrawList* draw_list = ImGui::GetBackgroundDrawList();
    bool nearest = image->filter == ShownImageFilter::nearest;
    draw_list->AddCallback(nearest ? platform_io.DrawCallback_SetSamplerNearest : platform_io.DrawCallback_SetSamplerLinear,
                           nullptr);
    draw_list->AddImage(ImTextureRef(static_cast<ImTextureID>(image->texture)), ImVec2(image->x, image->y),
                        ImVec2(image->x + image->width, image->y + image->height), ImVec2(image->u_min, image->v_min),
                        ImVec2(image->u_max, image->v_max));
    draw_list->AddCallback(platform_io.DrawCallback_SetSamplerLinear, nullptr);
}

// One frame of the loop, as a frontend runs it. False if the shell couldn't make the frame.
static bool run_frame(AppShell* shell, VideoOutput* output, VideoTexture* texture, const void* pixels,
                      const ViewportResult* viewport, const ScreenEffectParams* params)
{
    if (!app_shell_begin_frame(shell))
    {
        return false;
    }
    video_output_set_effect(output, params);
    if (video_output_prepare(output, shell, texture, viewport))
    {
        ShownImage image = {};
        if (video_output_shown_image(output, texture, 0.0f, 0.0f, &image))
        {
            draw_shown_image(&image);
        }
    }
    if (!app_shell_acquire(shell))
    {
        return false;
    }
    if (!video_texture_upload(texture, shell, pixels, 2, 2))
    {
        return false;
    }
    video_output_record(output, nullptr, shell, texture);
    return app_shell_present(shell, ClearColor{ 0.0f, 0.0f, 0.0f, 1.0f });
}

// Asks for a capture and runs frames until it comes back.
static void capture_and_check(AppShell* shell, VideoOutput* output, VideoTexture* texture, const void* pixels,
                              const ViewportResult* viewport, const ScreenEffectParams* params, const char* what)
{
    if (!video_output_request_capture(output))
    {
        std::printf("FAIL %s: capture refused\n", what);
        g_failures++;
        return;
    }
    if (video_output_request_capture(output))
    {
        std::printf("FAIL %s: a second capture accepted while one is on its way\n", what);
        g_failures++;
    }
    VideoCapture capture = {};
    uint32_t frames = 0;
    bool taken = false;
    while (frames < 10 && !taken)
    {
        if (!run_frame(shell, output, texture, pixels, viewport, params))
        {
            std::printf("FAIL %s: no frame\n", what);
            g_failures++;
            return;
        }
        frames++;
        taken = video_output_take_capture(output, &capture);
    }
    if (!taken)
    {
        std::printf("FAIL %s: no capture after %u frames\n", what, frames);
        g_failures++;
        return;
    }
    std::printf("   (%s: back after %u frame(s))\n", what, frames);
    check_capture(&capture, viewport->width, what);
    video_capture_free(&capture);
}

// GLES: the target's own pixels, with knobs that make known colors.
static void check_gl_knobs(AppShell* shell, VideoOutput* output, VideoTexture* texture, const void* pixels)
{
    ViewportResult viewport = picture_viewport(6, ViewportFilter::nearest);
    GlFunctions* gl = &shell->gl.gl;
    uint8_t read[6 * 6 * 4] = {};

    ScreenEffectParams grey = {};
    grey.desaturate = 1.0f;
    if (!run_frame(shell, output, texture, pixels, &viewport, &grey))
    {
        std::printf("FAIL knobs: no frame\n");
        g_failures++;
        return;
    }
    gl->BindFramebuffer(GL_FRAMEBUFFER, output->gl_effect.framebuffer);
    gl->ReadPixels(0, 0, 6, 6, GL_RGBA, GL_UNSIGNED_BYTE, read);
    gl->BindFramebuffer(GL_FRAMEBUFFER, 0);
    // Red's luma: 0.299 * 255 = 76.
    bool ok = close_to(read[0], 76) && close_to(read[1], 76) && close_to(read[2], 76);
    std::printf("%s knobs: desaturate (%u %u %u)\n", ok ? "ok" : "FAIL", read[0], read[1], read[2]);
    g_failures += ok ? 0 : 1;

    ScreenEffectParams shutter = {};
    shutter.bars = 0.5f;
    shutter.flash = 1.0f;   // the bars win over the flash
    run_frame(shell, output, texture, pixels, &viewport, &shutter);
    gl->BindFramebuffer(GL_FRAMEBUFFER, output->gl_effect.framebuffer);
    gl->ReadPixels(0, 0, 6, 6, GL_RGBA, GL_UNSIGNED_BYTE, read);
    gl->BindFramebuffer(GL_FRAMEBUFFER, 0);
    ok = read[0] == 0 && read[1] == 0 && read[2] == 0 && read[3] == 255;
    std::printf("%s knobs: shutter closed (%u %u %u)\n", ok ? "ok" : "FAIL", read[0], read[1], read[2]);
    g_failures += ok ? 0 : 1;

    ScreenEffectParams flash = {};
    flash.flash = 1.0f;
    flash.flash_red = 0.0f;
    flash.flash_green = 0.0f;
    flash.flash_blue = 1.0f;
    run_frame(shell, output, texture, pixels, &viewport, &flash);
    gl->BindFramebuffer(GL_FRAMEBUFFER, output->gl_effect.framebuffer);
    gl->ReadPixels(0, 0, 6, 6, GL_RGBA, GL_UNSIGNED_BYTE, read);
    gl->BindFramebuffer(GL_FRAMEBUFFER, 0);
    ok = close_to(read[0], 0) && close_to(read[1], 0) && close_to(read[2], 255);
    std::printf("%s knobs: flash color (%u %u %u)\n", ok ? "ok" : "FAIL", read[0], read[1], read[2]);
    g_failures += ok ? 0 : 1;
}

int main(int argc, char** argv)
{
    const char* api = argc > 1 ? argv[1] : "vulkan";
    bool want_gl = std::strcmp(api, "gl") == 0;
    SDL_setenv_unsafe("MULTITAP_GPU", want_gl ? "gl" : "vulkan", 1);
    SDL_setenv_unsafe("MULTITAP_VULKAN_1_1", std::strcmp(api, "vulkan11") == 0 ? "1" : "0", 1);
    SDL_SetLogPriorities(SDL_LOG_PRIORITY_INFO);
    SDL_GetLogOutputFunction(&g_default_log, &g_default_log_data);
    SDL_SetLogOutputFunction(log_output, nullptr);

    AppShell shell = {};
    AppShellConfig config = {};
    config.name = "screen_effect_test";
    config.window_width = 64;
    config.window_height = 64;
    if (!app_shell_create(&shell, &config) || (shell.api == GpuApi::gl) != want_gl)
    {
        std::printf("SKIP: no display, or no %s\n", api);
        app_shell_destroy(&shell);
        return 77;
    }
    if (!want_gl)
    {
        // (Not vulkan_path_to_string: a build without Vulkan has no such function.)
        std::printf("Vulkan path: %s\n", shell.vulkan.path == VulkanPath::vulkan_1_3 ? "1.3" : "1.1");
    }

    const uint16_t rgb565[4] = { 0xF800, 0x07E0, 0x001F, 0xFFFF };
    VideoTexture texture = {};
    VideoOutput output = {};
    if (!video_texture_create(&texture, &shell, 5, 3, FramePixelFormat::rgb565, true))
    {
        std::printf("FAIL: no texture\n");
        g_failures++;
    }
    else
    {
        ScreenEffectParams rest = {};
        ViewportResult nearest = picture_viewport(6, ViewportFilter::nearest);
        run_frame(&shell, &output, &texture, rgb565, &nearest, &rest);   // a frame with no pass first
        capture_and_check(&shell, &output, &texture, rgb565, &nearest, &rest, "capture, nearest x3");

        // Every knob at once for a few frames (the capture still shows the picture itself).
        ScreenEffectParams all = {};
        all.zoom = 1.05f;
        all.desaturate = 0.5f;
        all.flash = 0.25f;
        all.bars = 0.1f;
        for (uint32_t i = 0; i < 4; i++)
        {
            run_frame(&shell, &output, &texture, rgb565, &nearest, &all);
        }
        capture_and_check(&shell, &output, &texture, rgb565, &nearest, &all, "capture during an effect");

        ViewportResult copy = picture_viewport(8, ViewportFilter::sharp_bilinear);
        capture_and_check(&shell, &output, &texture, rgb565, &copy, &rest, "capture, sharp bilinear copy x4 (new size)");

        if (want_gl)
        {
            check_gl_knobs(&shell, &output, &texture, rgb565);
        }
        else
        {
            vulkan_context_wait_idle(&shell.vulkan);
        }
    }
    video_output_destroy(&output, nullptr, &shell);
    video_texture_destroy(&texture, &shell);
    app_shell_destroy(&shell);

    if (g_validation_messages != 0)
    {
        std::printf("FAIL: %d Vulkan validation message(s)\n", g_validation_messages);
        g_failures++;
    }
    if (g_failures != 0)
    {
        std::printf("FAIL: %d checks failed\n", g_failures);
        return 1;
    }
    std::printf("screen effect (%s): all checks passed\n", api);
    return 0;
}
