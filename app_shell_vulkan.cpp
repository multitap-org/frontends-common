// app_shell's Vulkan side (app_shell_vulkan.h). Built when MULTITAP_GPU_VULKAN
// is on; app_shell_vulkan_none.cpp stands in for it when it's off.

#include "app_shell_vulkan.h"

#include <cstdio>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include "debug_ui.h"

// ---------------------------------------------------------------------------
// Create / destroy
// ---------------------------------------------------------------------------

static bool recreate_swapchain(AppShell* shell)
{
    uint32_t width = 0;
    uint32_t height = 0;
    if (!app_shell_window_size_in_pixels(shell->window, &width, &height))
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

bool app_shell_vulkan_create(AppShell* shell, const AppShellConfig* config)
{
    if (!app_shell_create_window(shell, config, SDL_WINDOW_VULKAN))
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
    if (!app_shell_window_size_in_pixels(shell->window, &width, &height))
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

void app_shell_vulkan_destroy(AppShell* shell)
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
    if (!app_shell_window_size_in_pixels(shell->window, &width, &height) ||
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

void app_shell_vulkan_process_event(AppShell* shell, const SDL_Event* event)
{
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
}

bool app_shell_vulkan_begin_frame(AppShell* shell)
{
    // Background (Android/iOS): no surface to draw on until we're back.
    if (shell->in_background || (shell->surface_released && !restore_surface(shell)))
    {
        SDL_Delay(10);
        return false;
    }

    // Window resized since the swapchain was built?
    uint32_t width = 0;
    uint32_t height = 0;
    if (app_shell_window_size_in_pixels(shell->window, &width, &height) &&
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
    return true;
}

RenderFrameResult app_shell_vulkan_acquire(AppShell* shell)
{
    return renderer_begin_frame(&shell->renderer, &shell->vulkan, &shell->swapchain);
}

bool app_shell_vulkan_present(AppShell* shell, ClearColor clear_color, bool acquired)
{
    RenderFrameResult result = RenderFrameResult::error;
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

bool app_shell_vulkan_set_vsync(AppShell* shell)
{
    return recreate_swapchain(shell);
}

bool app_shell_vulkan_vsync_forced(const AppShell* shell)
{
    return !shell->swapchain.vsync && shell->swapchain.present_mode == VK_PRESENT_MODE_FIFO_KHR;
}

void app_shell_vulkan_set_frames_in_flight(AppShell* shell, uint32_t count)
{
    renderer_set_frames_in_flight(&shell->renderer, &shell->vulkan, count);
}

bool app_shell_vulkan_waits_for_screen(const AppShell* shell)
{
    return shell->vulkan.present_wait_version > 0;
}

void app_shell_vulkan_gpu_info(const AppShell* shell, AppShellGpuInfo* info)
{
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
