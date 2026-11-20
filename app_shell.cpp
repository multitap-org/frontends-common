#include "app_shell.h"

#include <cstdio>
#include <cstring>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include "debug_ui.h"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static bool window_size_in_pixels(SDL_Window* window, uint32_t* width, uint32_t* height)
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

static bool create_window(AppShell* shell, const AppShellConfig* config, SDL_WindowFlags api_flag)
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

static bool recreate_swapchain(AppShell* shell)
{
    uint32_t width = 0;
    uint32_t height = 0;
    if (!window_size_in_pixels(shell->window, &width, &height))
    {
        return true;   // minimized / zero-sized: try again later
    }
    if (!vulkan_swapchain_recreate(&shell->swapchain, &shell->vulkan, shell->surface, width, height))
    {
        return false;
    }
    debug_ui_on_swapchain_recreated(&shell->swapchain);
    return true;
}

// ---------------------------------------------------------------------------
// Create / destroy
// ---------------------------------------------------------------------------

// Vulkan: instance -> surface -> device -> swapchain -> renderer -> ImGui.
static bool create_vulkan(AppShell* shell, const AppShellConfig* config)
{
    if (!create_window(shell, config, SDL_WINDOW_VULKAN))
    {
        return false;
    }

    // Instance -> surface -> device (the device choice depends on the surface).
    uint32_t sdl_extension_count = 0;
    const char* const* sdl_extensions = SDL_Vulkan_GetInstanceExtensions(&sdl_extension_count);
    if (sdl_extensions == nullptr)
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL_Vulkan_GetInstanceExtensions failed: %s", SDL_GetError());
        return false;
    }
    if (!vulkan_context_create_instance(&shell->vulkan, config->name, sdl_extensions, sdl_extension_count))
    {
        return false;
    }

    if (!SDL_Vulkan_CreateSurface(shell->window, shell->vulkan.instance, nullptr, &shell->surface))
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL_Vulkan_CreateSurface failed: %s", SDL_GetError());
        return false;
    }

    const char* force_1_1 = SDL_getenv("MULTITAP_VULKAN_1_1");
    shell->vulkan.force_vulkan_1_1 =
        config->force_vulkan_1_1 || (force_1_1 != nullptr && force_1_1[0] != '\0' && force_1_1[0] != '0');
    if (!vulkan_context_create_device(&shell->vulkan, shell->surface))
    {
        return false;
    }

    uint32_t width = 0;
    uint32_t height = 0;
    if (!window_size_in_pixels(shell->window, &width, &height))
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Could not read window size: %s", SDL_GetError());
        return false;
    }
    if (!vulkan_swapchain_create(&shell->swapchain, &shell->vulkan, shell->surface, width, height))
    {
        return false;
    }

    if (!renderer_create(&shell->renderer, &shell->vulkan))
    {
        return false;
    }

    if (!debug_ui_init(shell->window, &shell->vulkan, &shell->swapchain))
    {
        return false;
    }
    shell->debug_ui_ready = true;
    return true;
}

// Everything create_vulkan made, so OpenGL can start over with a new window.
static void destroy_vulkan(AppShell* shell)
{
    if (shell->vulkan.device != VK_NULL_HANDLE)
    {
        vkDeviceWaitIdle(shell->vulkan.device);
    }
    if (shell->debug_ui_ready)
    {
        debug_ui_shutdown();
        shell->debug_ui_ready = false;
    }
    renderer_destroy(&shell->renderer, &shell->vulkan);
    vulkan_swapchain_destroy(&shell->swapchain, &shell->vulkan);
    if (shell->surface != VK_NULL_HANDLE)
    {
        SDL_Vulkan_DestroySurface(shell->vulkan.instance, shell->surface, nullptr);
        shell->surface = VK_NULL_HANDLE;
    }
    vulkan_context_destroy(&shell->vulkan);
    if (shell->window != nullptr)
    {
        SDL_DestroyWindow(shell->window);
        shell->window = nullptr;
    }
    // Back to the defaults: GL keeps its vsync / frame settings in these too.
    shell->renderer = Renderer{};
    shell->swapchain = VulkanSwapchain{};
    shell->vulkan = VulkanContext{};
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
    if (!create_window(shell, config, SDL_WINDOW_OPENGL))
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
        ready = create_vulkan(shell, config);
        if (!ready && !force_vulkan)
        {
            SDL_LogInfo(SDL_LOG_CATEGORY_GPU, "No usable Vulkan: trying OpenGL ES");
            destroy_vulkan(shell);
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
    window_size_in_pixels(shell->window, &width, &height);
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
        destroy_vulkan(shell);
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

// Android, going to the background: the surface may already be gone (or its
// window handed over) when the swapchain is recreated.
static bool swapchain_failed_for_lost_surface(const VulkanSwapchain* swapchain)
{
    return swapchain->last_create_result == VK_ERROR_SURFACE_LOST_KHR ||
           swapchain->last_create_result == VK_ERROR_NATIVE_WINDOW_IN_USE_KHR;
}

// The app goes to the background (Android/iOS): the window's surface is about
// to be destroyed (Android does it right after this event, on its UI thread),
// so nothing may use it any more. SDL sends this event on our thread before
// it blocks the event loop until the app comes back.
static void release_surface(AppShell* shell)
{
    if (shell->surface_released)
    {
        return;
    }
    vulkan_swapchain_release(&shell->swapchain, &shell->vulkan);   // waits for the GPU first
    if (shell->surface != VK_NULL_HANDLE)
    {
        SDL_Vulkan_DestroySurface(shell->vulkan.instance, shell->surface, nullptr);
        shell->surface = VK_NULL_HANDLE;
    }
    shell->surface_released = true;
    SDL_LogInfo(SDL_LOG_CATEGORY_GPU, "In the background: swapchain and surface released");
}

// Back from the background: a new surface for the window's new native window.
// May fail while Android hasn't handed SDL the new window yet; begin_frame
// tries again every frame.
static bool restore_surface(AppShell* shell)
{
    if (!SDL_Vulkan_CreateSurface(shell->window, shell->vulkan.instance, nullptr, &shell->surface))
    {
        shell->surface = VK_NULL_HANDLE;
        return false;
    }

    // The queue family was chosen for the old surface; the spec wants the
    // question asked again for this one (same window, so expected yes).
    VkBool32 can_present = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(shell->vulkan.physical_device, shell->vulkan.queue_family, shell->surface,
                                         &can_present);
    if (can_present != VK_TRUE)
    {
        SDL_LogError(SDL_LOG_CATEGORY_GPU, "The new surface can't be presented from queue family %u",
                     shell->vulkan.queue_family);
        SDL_Vulkan_DestroySurface(shell->vulkan.instance, shell->surface, nullptr);
        shell->surface = VK_NULL_HANDLE;
        return false;
    }

    uint32_t width = 0;
    uint32_t height = 0;
    if (!window_size_in_pixels(shell->window, &width, &height) ||
        !vulkan_swapchain_create(&shell->swapchain, &shell->vulkan, shell->surface, width, height))
    {
        SDL_Vulkan_DestroySurface(shell->vulkan.instance, shell->surface, nullptr);
        shell->surface = VK_NULL_HANDLE;
        return false;
    }
    debug_ui_on_swapchain_recreated(&shell->swapchain);
    shell->surface_released = false;
    SDL_LogInfo(SDL_LOG_CATEGORY_GPU, "Back in the foreground: surface and swapchain created again");
    return true;
}

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
    if (event->type == SDL_EVENT_DID_ENTER_BACKGROUND)
    {
        release_surface(shell);
        shell->in_background = true;
    }
    else if (event->type == SDL_EVENT_DID_ENTER_FOREGROUND)
    {
        shell->in_background = false;
        if (shell->surface_released)
        {
            restore_surface(shell);   // if it fails, begin_frame tries again
        }
    }
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

    // Background (Android/iOS): no surface to draw on until we're back.
    if (shell->in_background || (shell->surface_released && !restore_surface(shell)))
    {
        SDL_Delay(10);
        return false;
    }

    // Window resized since the swapchain was built?
    uint32_t width = 0;
    uint32_t height = 0;
    if (window_size_in_pixels(shell->window, &width, &height) &&
        (width != shell->swapchain.extent.width || height != shell->swapchain.extent.height))
    {
        if (!recreate_swapchain(shell))
        {
            if (swapchain_failed_for_lost_surface(&shell->swapchain))
            {
                // The surface went away mid-resize (Android, background).
                release_surface(shell);
                SDL_Delay(10);
                return false;
            }
            SDL_LogError(SDL_LOG_CATEGORY_GPU, "Swapchain recreation failed");
        }
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
                                    : renderer_begin_frame(&shell->renderer, &shell->vulkan, &shell->swapchain);
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
    window_size_in_pixels(shell->window, &width, &height);
    gl->BindFramebuffer(GL_FRAMEBUFFER, 0);
    gl->Viewport(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height));
    gl->Disable(GL_SCISSOR_TEST);
    gl->ClearColor(clear_color.r, clear_color.g, clear_color.b, clear_color.a);
    gl->Clear(GL_COLOR_BUFFER_BIT);
    debug_ui_render_gl();
    SDL_GL_SwapWindow(shell->window);
    // Low latency: wait for the GPU to finish this frame (GL can't wait for
    // the screen), so the next frame starts as late as possible.
    if (shell->renderer.wait_for_present)
    {
        gl->Finish();
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
    RenderFrameResult result = RenderFrameResult::error;
    bool acquired = app_shell_acquire(shell);
    shell->frame_started = false;

    if (shell->api == GpuApi::gl)
    {
        present_gl(shell, clear_color);
        return true;
    }

    if (acquired)
    {
        renderer_begin_rendering(&shell->renderer, &shell->swapchain, clear_color);
        debug_ui_render(renderer_command_buffer(&shell->renderer));
        result = renderer_end_frame(&shell->renderer, &shell->vulkan, &shell->swapchain);
    }
    else
    {
        debug_ui_end_frame_without_render();
        result = shell->acquire_result;
    }

    if (result == RenderFrameResult::swapchain_needs_recreate)
    {
        if (recreate_swapchain(shell))
        {
            return true;
        }
        // The driver may say "out of date" first when the surface is really
        // gone (Android, going to the background): then it's not fatal.
        if (!swapchain_failed_for_lost_surface(&shell->swapchain))
        {
            return false;
        }
        result = RenderFrameResult::surface_lost;
    }
    if (result == RenderFrameResult::surface_lost)
    {
        // Gone before the background event reached us: same as that event.
        // begin_frame creates it again once a new window is there.
        release_surface(shell);
        return true;
    }
    return result == RenderFrameResult::ok;
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
    return recreate_swapchain(shell);
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
    return !shell->swapchain.vsync && shell->swapchain.present_mode == VK_PRESENT_MODE_FIFO_KHR;
}

void app_shell_set_frames_in_flight(AppShell* shell, uint32_t count)
{
    if (shell->api == GpuApi::gl)
    {
        // The driver decides how far ahead GL runs; kept for the getter.
        shell->renderer.frames_in_flight = count < 1 ? 1 : (count > FRAMES_IN_FLIGHT ? FRAMES_IN_FLIGHT : count);
        return;
    }
    renderer_set_frames_in_flight(&shell->renderer, &shell->vulkan, count);
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
    return shell->api == GpuApi::vulkan && shell->vulkan.present_wait_version > 0;
}

void app_shell_gpu_info(const AppShell* shell, AppShellGpuInfo* info)
{
    if (shell->api == GpuApi::gl)
    {
        uint32_t width = 0;
        uint32_t height = 0;
        window_size_in_pixels(shell->window, &width, &height);
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
    const VkPhysicalDeviceProperties* gpu = &shell->vulkan.physical_device_properties;
    const VulkanSwapchain* swapchain = &shell->swapchain;
    std::snprintf(info->device, sizeof(info->device), "%s", gpu->deviceName);
    std::snprintf(info->api, sizeof(info->api), "Vulkan %u.%u.%u", VK_API_VERSION_MAJOR(gpu->apiVersion),
                  VK_API_VERSION_MINOR(gpu->apiVersion), VK_API_VERSION_PATCH(gpu->apiVersion));
    std::snprintf(info->swapchain, sizeof(info->swapchain), "%ux%u, %u images, %s", swapchain->extent.width,
                  swapchain->extent.height, swapchain->image_count, vk_present_mode_to_string(swapchain->present_mode));
    std::snprintf(info->validation, sizeof(info->validation), "Validation: %s",
                  shell->vulkan.validation_enabled ? "on" : "off");
}
