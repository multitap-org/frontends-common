#pragma once

// Everything a frontend needs before it draws anything: SDL, the window, Vulkan
// (instance, device, surface, swapchain, renderer) and the Dear ImGui debug UI.
// Every frontend (piccolo, the host-libretro test frontend) uses this instead of
// its own copy.
//
// Each frame:
//
//   if (app_shell_begin_frame(&shell))        // false = minimized, skip drawing
//   {
//       ... build ImGui UI ...
//       if (app_shell_acquire(&shell))       // a swapchain image is ours, recording started
//       {
//           ... optional uploads into renderer_command_buffer(&shell.renderer) ...
//       }
//       if (!app_shell_present(&shell, clear)) { fatal error }
//   }

#include <cstdint>

#include <vulkan/vulkan.h>

#include "gl_context.h"
#include "renderer.h"
#include "text_input.h"
#include "vk_context.h"
#include "vk_swapchain.h"

struct SDL_Window;
union SDL_Event;

// One size of the window icon, as the bytes of a PNG file.
struct AppShellIcon
{
    const uint8_t* png;
    uint32_t       size;
};

struct AppShellConfig
{
    const char* name           = "frontend";   // window title, Vulkan application name
    const char* version        = "0.1.0";
    const char* identifier     = "org.multitap.frontend";
    int32_t     window_width   = 1920;          // clamped to the screen's usable area
    int32_t     window_height  = 1080;
    // The window icon: every size the frontend has, the first one is the base
    // image and the rest go to the window manager as alternates. None = the
    // system's default icon.
    const AppShellIcon* icons      = nullptr;
    int32_t             icon_count = 0;

    // Use the Vulkan 1.1 path (render passes) even on a 1.3 GPU. Also turned
    // on by the environment variable MULTITAP_VULKAN_1_1=1, to test that path
    // on a desktop GPU without changing the program.
    bool                force_vulkan_1_1 = false;
};

// The graphics API the shell runs on, chosen at startup: the highest that
// works (Vulkan, else OpenGL ES; piccolo's docs/gl.md). MULTITAP_GPU=gl (or
// =vulkan) forces one, to test a path.
enum class GpuApi
{
    vulkan,
    gl,
};

// Zero/null-initialized, so app_shell_destroy() is safe at any point during startup.
struct AppShell
{
    GpuApi          api            = GpuApi::vulkan;
    SDL_Window*     window         = nullptr;
    GlContext       gl             = {};      // api == gl
    VulkanContext   vulkan         = {};
    VkSurfaceKHR    surface        = VK_NULL_HANDLE;
    VulkanSwapchain swapchain      = {};
    Renderer        renderer       = {};
    bool            sdl_ready      = false;
    bool            debug_ui_ready = false;

    // The window's SDL text input (UI fields, a core's keyboard request, the
    // game's characters): applied at each begin_frame.
    TextInput       text_input     = {};

    // Android/iOS: in the background the window's surface is destroyed. The
    // swapchain and VkSurfaceKHR are released then and created again when the
    // app is back (app_shell_process_event handles both events).
    bool            in_background    = false;
    bool            surface_released = false;

    // Per-frame state between begin_frame / acquire / present.
    bool              frame_started  = false;
    bool              acquire_tried  = false;
    RenderFrameResult acquire_result = RenderFrameResult::error;
};

bool app_shell_create(AppShell* shell, const AppShellConfig* config);
void app_shell_destroy(AppShell* shell);

// Forward every SDL event (feeds ImGui; on Android/iOS also releases and
// restores the Vulkan surface around the background). Returns true if ImGui used it.
bool app_shell_process_event(AppShell* shell, const SDL_Event* event);

// Recreates the swapchain if the window was resized and starts the ImGui frame.
// Returns false when the window is minimized (nothing to draw; it sleeps briefly).
bool app_shell_begin_frame(AppShell* shell);

// Acquires the next swapchain image and starts command recording. Returns true
// if recording started; callers may then record uploads (outside the rendering pass).
bool app_shell_acquire(AppShell* shell);

// Turns vsync on (FIFO) or off (MAILBOX, else IMMEDIATE), recreating the
// swapchain if it changes. Returns false only on a fatal error.
bool app_shell_set_vsync(AppShell* shell, bool vsync);

// True when vsync was asked to be off but the swapchain is still on FIFO
// (the driver offers neither MAILBOX nor IMMEDIATE): every present waits for
// vsync anyway.
bool app_shell_vsync_forced(const AppShell* shell);

// The refresh rate of the display the window is on, in Hz (e.g. 59.91), or 0
// if SDL doesn't know it.
double app_shell_display_refresh_hz(const AppShell* shell);

// 1 or 2 frames the CPU may prepare ahead of the GPU (renderer.h: 1 is lower
// latency, 2 smoother). Changing it waits for the GPU; call between frames.
void app_shell_set_frames_in_flight(AppShell* shell, uint32_t count);
uint32_t app_shell_frames_in_flight(const AppShell* shell);

// Low latency: after presenting, wait until the frame is on screen (or else
// until the GPU finished it), so the next frame starts as late as possible.
void app_shell_set_wait_for_present(AppShell* shell, bool wait);
bool app_shell_wait_for_present(const AppShell* shell);

// True when the shell can wait until a frame is on the screen (Vulkan's
// present wait); false when the lowest-latency wait is for the GPU to finish.
bool app_shell_waits_for_screen(const AppShell* shell);

// What the shell runs on, for debug windows: one line each.
struct AppShellGpuInfo
{
    char device[256];      // "AMD Radeon RX 6600"
    char api[160];         // "Vulkan 1.3.280", "OpenGL ES 3.1 Mesa 25.2"
    char swapchain[128];   // "1920x1080, 3 images, FIFO"
    char validation[64];   // "Validation: on"
};
void app_shell_gpu_info(const AppShell* shell, AppShellGpuInfo* info);

// Draws the ImGui UI and presents (acquiring first if the caller didn't).
// Returns false only on a fatal Vulkan error.
bool app_shell_present(AppShell* shell, ClearColor clear_color);
