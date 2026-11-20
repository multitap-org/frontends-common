// app_shell's Vulkan side in a build without Vulkan (MULTITAP_GPU_VULKAN off:
// the Android 5 APK). Creating says no, so the shell goes on to OpenGL ES;
// the rest is never called.

#include "app_shell_vulkan.h"

#include <SDL3/SDL_log.h>

bool app_shell_vulkan_create(AppShell* shell, const AppShellConfig* config)
{
    static_cast<void>(shell);
    static_cast<void>(config);
    SDL_LogInfo(SDL_LOG_CATEGORY_GPU, "This build has no Vulkan");
    return false;
}

void app_shell_vulkan_destroy(AppShell* shell)
{
    static_cast<void>(shell);
}

void app_shell_vulkan_process_event(AppShell* shell, const SDL_Event* event)
{
    static_cast<void>(shell);
    static_cast<void>(event);
}

bool app_shell_vulkan_begin_frame(AppShell* shell)
{
    static_cast<void>(shell);
    return false;
}

RenderFrameResult app_shell_vulkan_acquire(AppShell* shell)
{
    static_cast<void>(shell);
    return RenderFrameResult::error;
}

bool app_shell_vulkan_present(AppShell* shell, ClearColor clear_color, bool acquired)
{
    static_cast<void>(shell);
    static_cast<void>(clear_color);
    static_cast<void>(acquired);
    return false;
}

bool app_shell_vulkan_set_vsync(AppShell* shell)
{
    static_cast<void>(shell);
    return false;
}

bool app_shell_vulkan_vsync_forced(const AppShell* shell)
{
    static_cast<void>(shell);
    return false;
}

void app_shell_vulkan_set_frames_in_flight(AppShell* shell, uint32_t count)
{
    static_cast<void>(shell);
    static_cast<void>(count);
}

bool app_shell_vulkan_waits_for_screen(const AppShell* shell)
{
    static_cast<void>(shell);
    return false;
}

void app_shell_vulkan_gpu_info(const AppShell* shell, AppShellGpuInfo* info)
{
    static_cast<void>(shell);
    static_cast<void>(info);
}
