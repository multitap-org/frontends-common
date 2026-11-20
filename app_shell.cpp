#include "app_shell.h"

#include <cstdio>
#include <cstring>

#include <SDL3/SDL.h>

#include "app_shell_vulkan.h"
#include "debug_ui.h"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

bool app_shell_window_size_in_pixels(SDL_Window* window, uint32_t* width, uint32_t* height)
{
    int32_t w = 0;
    int32_t h = 0;
    if (!SDL_GetWindowSizeInPixels(window, &w, &h) || w <= 0 || h <= 0)
    {
        return false;
    }
    *width  = static_cast<uint32_t>(w);
    *height = static_cast<uint32_t>(h);
    return true;
}

// Sets the window icon from the config's PNGs. A failure only costs the icon.
static void set_window_icon(SDL_Window* window, const AppShellConfig* config)
{
    if (config->icons == nullptr || config->icon_count <= 0)
    {
        return;
    }

    SDL_Surface* icon = nullptr;
    for (int32_t i = 0; i < config->icon_count; i++)
    {
        SDL_IOStream* io = SDL_IOFromConstMem(config->icons[i].png, config->icons[i].size);
        SDL_Surface* image = nullptr;
        if (io != nullptr)
        {
            image = SDL_LoadPNG_IO(io, true);
        }
        if (image == nullptr)
        {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Window icon %d: %s", i, SDL_GetError());
            continue;
        }
        if (icon == nullptr)
        {
            icon = image;
        }
        else
        {
            SDL_AddSurfaceAlternateImage(icon, image);
            SDL_DestroySurface(image);   // the base surface keeps its own reference
        }
    }

    if (icon != nullptr)
    {
        if (!SDL_SetWindowIcon(window, icon))
        {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "SDL_SetWindowIcon failed: %s", SDL_GetError());
        }
        SDL_DestroySurface(icon);
    }
}

bool app_shell_create_window(AppShell* shell, const AppShellConfig* config, SDL_WindowFlags api_flag)
{
    int32_t width  = config->window_width;
    int32_t height = config->window_height;

    // Don't open a window bigger than the screen (laptops, small panels, macOS points).
    SDL_Rect usable = {};
    if (SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &usable))
    {
        if (width > usable.w)
        {
            width = usable.w;
        }
        if (height > usable.h)
        {
            height = usable.h;
        }
    }

    SDL_WindowFlags flags = api_flag | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN;
    shell->window = SDL_CreateWindow(config->name, width, height, flags);
    if (shell->window == nullptr)
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL_CreateWindow failed: %s", SDL_GetError());
        return false;
    }
    set_window_icon(shell->window, config);
    return true;
}


// A build whose ImGui GL backend is GLES 2's (the Android 5 APK, or a Linux
// build that tests like it) asks for GLES 2.0; others for 3.0.
#if defined(MULTITAP_GL_ES2)
constexpr bool GL_ES2_BUILD = true;
#else
constexpr bool GL_ES2_BUILD = false;
#endif

// OpenGL ES: window -> context -> ImGui.
static bool create_gl(AppShell* shell, const AppShellConfig* config)
{
    shell->api = GpuApi::gl;
    gl_context_set_attributes(GL_ES2_BUILD);
    if (!app_shell_create_window(shell, config, SDL_WINDOW_OPENGL))
    {
        return false;
    }
    if (!gl_context_create(&shell->gl, shell->window, GL_ES2_BUILD))
    {
        return false;
    }
    // Vsync on until the frontend says otherwise (as the Vulkan swapchain starts with FIFO).
    shell->swapchain.vsync = true;
    shell->gl.swap_interval = SDL_GL_SetSwapInterval(1) ? 1 : 0;
    if (!debug_ui_init_gl(shell->window, shell->gl.context, gl_context_imgui_glsl_version(&shell->gl)))
    {
        return false;
    }
    shell->debug_ui_ready = true;
    return true;
}

bool app_shell_create(AppShell* shell, const AppShellConfig* config)
{
    SDL_SetAppMetadata(config->name, config->version, config->identifier);

    // SDL hides everything below ERROR for most categories by default, which
    // would also hide Vulkan validation warnings (logged as SDL_LOG_CATEGORY_GPU).
    SDL_SetLogPriorities(SDL_LOG_PRIORITY_INFO);

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD))
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL_Init failed: %s", SDL_GetError());
        return false;
    }
    shell->sdl_ready = true;

    // The highest API that works, unless MULTITAP_GPU names one.
    const char* forced = SDL_getenv("MULTITAP_GPU");
    bool force_gl = forced != nullptr && std::strcmp(forced, "gl") == 0;
    bool force_vulkan = forced != nullptr && std::strcmp(forced, "vulkan") == 0;
    bool ready = false;
    if (!force_gl)
    {
        shell->api = GpuApi::vulkan;
        ready = app_shell_vulkan_create(shell, config);
        if (!ready && !force_vulkan)
        {
            SDL_LogInfo(SDL_LOG_CATEGORY_GPU, "No usable Vulkan: trying OpenGL ES");
            app_shell_vulkan_destroy(shell);
        }
    }
    if (!ready && !force_vulkan)
    {
        ready = create_gl(shell, config);
    }
    if (!ready)
    {
        return false;
    }

    SDL_SetWindowPosition(shell->window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    SDL_ShowWindow(shell->window);

    uint32_t width = 0;
    uint32_t height = 0;
    app_shell_window_size_in_pixels(shell->window, &width, &height);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Window %ux%u pixels, display scale %.2f, %s",
                width, height, static_cast<double>(SDL_GetWindowDisplayScale(shell->window)),
                shell->api == GpuApi::gl ? "OpenGL ES" : "Vulkan");
    return true;
}

void app_shell_destroy(AppShell* shell)
{
    if (shell->api == GpuApi::gl)
    {
        if (shell->debug_ui_ready)
        {
            debug_ui_shutdown();
            shell->debug_ui_ready = false;
        }
        gl_context_destroy(&shell->gl);
        if (shell->window != nullptr)
        {
            SDL_DestroyWindow(shell->window);
            shell->window = nullptr;
        }
    }
    else
    {
        app_shell_vulkan_destroy(shell);
    }

    if (shell->sdl_ready)
    {
        SDL_Quit();
        shell->sdl_ready = false;
    }
}

// ---------------------------------------------------------------------------
// Per frame
// ---------------------------------------------------------------------------


bool app_shell_process_event(AppShell* shell, const SDL_Event* event)
{
    if (shell->api == GpuApi::gl)
    {
        // SDL keeps the EGL surface in step with the window itself.
        if (event->type == SDL_EVENT_DID_ENTER_BACKGROUND)
        {
            shell->in_background = true;
        }
        else if (event->type == SDL_EVENT_DID_ENTER_FOREGROUND)
        {
            shell->in_background = false;
        }
        return debug_ui_process_event(event);
    }
    app_shell_vulkan_process_event(shell, event);
    return debug_ui_process_event(event);
}

bool app_shell_begin_frame(AppShell* shell)
{
    shell->frame_started = false;
    shell->acquire_tried = false;

    if (SDL_GetWindowFlags(shell->window) & SDL_WINDOW_MINIMIZED)
    {
        SDL_Delay(10);
        return false;
    }

    if (shell->api == GpuApi::gl)
    {
        if (shell->in_background)
        {
            SDL_Delay(10);
            return false;
        }
        debug_ui_begin_frame();
        shell->frame_started = true;
        return true;
    }

    if (!app_shell_vulkan_begin_frame(shell))
    {
        return false;
    }

    debug_ui_begin_frame();
    shell->frame_started = true;
    return true;
}

bool app_shell_acquire(AppShell* shell)
{
    if (!shell->frame_started)
    {
        return false;
    }
    if (!shell->acquire_tried)
    {
        shell->acquire_tried = true;
        // GL: nothing to acquire; the window's framebuffer is always there.
        shell->acquire_result = shell->api == GpuApi::gl
                                    ? RenderFrameResult::ok
                                    : app_shell_vulkan_acquire(shell);
    }
    return shell->acquire_result == RenderFrameResult::ok;
}

// GL: clear, ImGui, swap. Uploads and picture copies already ran (GL calls
// run as they're made, in the order made).
static void present_gl(AppShell* shell, ClearColor clear_color)
{
    GlFunctions* gl = &shell->gl.gl;
    uint32_t width = 0;
    uint32_t height = 0;
    app_shell_window_size_in_pixels(shell->window, &width, &height);
    gl->BindFramebuffer(GL_FRAMEBUFFER, 0);
    gl->Viewport(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height));
    gl->Disable(GL_SCISSOR_TEST);
    gl->ClearColor(clear_color.r, clear_color.g, clear_color.b, clear_color.a);
    gl->Clear(GL_COLOR_BUFFER_BIT);
    uint64_t started = SDL_GetTicksNS();
    debug_ui_render_gl();
    uint64_t drawn = SDL_GetTicksNS();
    if (!SDL_GL_SwapWindow(shell->window))
    {
        SDL_LogError(SDL_LOG_CATEGORY_GPU, "SDL_GL_SwapWindow failed: %s", SDL_GetError());
    }
    uint64_t swapped = SDL_GetTicksNS();
    // Low latency: wait for the GPU to finish this frame (GL can't wait for
    // the screen), so the next frame starts as late as possible.
    if (shell->renderer.wait_for_present)
    {
        gl->Finish();
    }
    uint64_t finished = SDL_GetTicksNS();
    // A present that takes a quarter of a second is a stall (a driver or a
    // surface problem): said in the log, at most once a second.
    constexpr uint64_t STALL_NS = 250000000;
    if ((swapped - drawn > STALL_NS || finished - swapped > STALL_NS || drawn - started > STALL_NS) &&
        finished - shell->gl.stall_logged_ns > 1000000000)
    {
        shell->gl.stall_logged_ns = finished;
        SDL_LogWarn(SDL_LOG_CATEGORY_GPU, "GL present stalled: ImGui %.0f ms, swap %.0f ms, finish %.0f ms",
                    static_cast<double>(drawn - started) / 1e6, static_cast<double>(swapped - drawn) / 1e6,
                    static_cast<double>(finished - swapped) / 1e6);
    }
    gl_check_errors(&shell->gl, "present");
}

bool app_shell_present(AppShell* shell, ClearColor clear_color)
{
    if (!shell->frame_started)
    {
        return true;
    }

    // Acquire BEFORE marking the frame finished: app_shell_acquire() refuses to
    // run outside a frame. (Getting this order wrong leaves an acquired frame
    // whose fence was reset but never submitted: the next frame waits forever.)
    bool acquired = app_shell_acquire(shell);
    shell->frame_started = false;

    if (shell->api == GpuApi::gl)
    {
        present_gl(shell, clear_color);
        return true;
    }

    return app_shell_vulkan_present(shell, clear_color, acquired);
}

bool app_shell_set_vsync(AppShell* shell, bool vsync)
{
    if (shell->swapchain.vsync == vsync)
    {
        return true;
    }
    shell->swapchain.vsync = vsync;
    if (shell->api == GpuApi::gl)
    {
        // Off may be refused (some drivers, every browser): then it stays on.
        if (SDL_GL_SetSwapInterval(vsync ? 1 : 0))
        {
            shell->gl.swap_interval = vsync ? 1 : 0;
        }
        return true;
    }
    return app_shell_vulkan_set_vsync(shell);
}

double app_shell_display_refresh_hz(const AppShell* shell)
{
    const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(shell->window));
    if (mode == nullptr)
    {
        return 0.0;
    }
    // The exact ratio when the driver gives one (59.94 = 60000/1001); else the rounded float.
    if (mode->refresh_rate_numerator > 0 && mode->refresh_rate_denominator > 0)
    {
        return static_cast<double>(mode->refresh_rate_numerator) / static_cast<double>(mode->refresh_rate_denominator);
    }
    return static_cast<double>(mode->refresh_rate);
}

bool app_shell_vsync_forced(const AppShell* shell)
{
    if (shell->api == GpuApi::gl)
    {
        return !shell->swapchain.vsync && shell->gl.swap_interval != 0;
    }
    return app_shell_vulkan_vsync_forced(shell);
}

void app_shell_set_frames_in_flight(AppShell* shell, uint32_t count)
{
    if (shell->api == GpuApi::gl)
    {
        // The driver decides how far ahead GL runs; kept for the getter.
        shell->renderer.frames_in_flight = count < 1 ? 1 : (count > FRAMES_IN_FLIGHT ? FRAMES_IN_FLIGHT : count);
        return;
    }
    app_shell_vulkan_set_frames_in_flight(shell, count);
}

uint32_t app_shell_frames_in_flight(const AppShell* shell)
{
    return shell->renderer.frames_in_flight;
}

void app_shell_set_wait_for_present(AppShell* shell, bool wait)
{
    shell->renderer.wait_for_present = wait;
}

bool app_shell_wait_for_present(const AppShell* shell)
{
    return shell->renderer.wait_for_present;
}

bool app_shell_waits_for_screen(const AppShell* shell)
{
    return shell->api == GpuApi::vulkan && app_shell_vulkan_waits_for_screen(shell);
}

void app_shell_gpu_info(const AppShell* shell, AppShellGpuInfo* info)
{
    if (shell->api == GpuApi::gl)
    {
        uint32_t width = 0;
        uint32_t height = 0;
        app_shell_window_size_in_pixels(shell->window, &width, &height);
        std::snprintf(info->device, sizeof(info->device), "%s", shell->gl.renderer);
        std::snprintf(info->api, sizeof(info->api), "%s", shell->gl.version);
        std::snprintf(info->swapchain, sizeof(info->swapchain), "%ux%u, swap interval %d", width, height,
                      shell->gl.swap_interval);
#if defined(NDEBUG)
        std::snprintf(info->validation, sizeof(info->validation), "GL errors: not checked");
#else
        std::snprintf(info->validation, sizeof(info->validation), "GL errors: logged");
#endif
        return;
    }
    app_shell_vulkan_gpu_info(shell, info);
}
