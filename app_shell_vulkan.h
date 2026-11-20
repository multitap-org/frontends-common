#pragma once

// app_shell's Vulkan side, inside frontends-common only (frontends use
// app_shell.h). app_shell_vulkan.cpp when the build has Vulkan
// (MULTITAP_GPU_VULKAN), app_shell_vulkan_none.cpp when it doesn't (the
// Android 5 APK: Android 5 has no libvulkan.so, so nothing may link it).

#include <cstdint>

#include "app_shell.h"

// app_shell.cpp's helpers, shared with this side.
bool app_shell_window_size_in_pixels(SDL_Window* window, uint32_t* width, uint32_t* height);
bool app_shell_create_window(AppShell* shell, const AppShellConfig* config, SDL_WindowFlags api_flag);

// Window, instance, surface, device, swapchain, renderer, ImGui. False
// (logged) when Vulkan isn't usable; then app_shell_vulkan_destroy() cleans
// up so OpenGL ES can start over with a new window.
bool app_shell_vulkan_create(AppShell* shell, const AppShellConfig* config);
void app_shell_vulkan_destroy(AppShell* shell);

void              app_shell_vulkan_process_event(AppShell* shell, const SDL_Event* event);
bool              app_shell_vulkan_begin_frame(AppShell* shell);   // false: skip this frame
RenderFrameResult app_shell_vulkan_acquire(AppShell* shell);
bool              app_shell_vulkan_present(AppShell* shell, ClearColor clear_color, bool acquired);
bool              app_shell_vulkan_set_vsync(AppShell* shell);     // swapchain.vsync already set
bool              app_shell_vulkan_vsync_forced(const AppShell* shell);
void              app_shell_vulkan_set_frames_in_flight(AppShell* shell, uint32_t count);
bool              app_shell_vulkan_waits_for_screen(const AppShell* shell);
void              app_shell_vulkan_gpu_info(const AppShell* shell, AppShellGpuInfo* info);
