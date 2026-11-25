#pragma once

// debug_ui's Vulkan side, inside frontends-common only (frontends use
// debug_ui.h). debug_ui_vulkan.cpp when the build has Vulkan
// (MULTITAP_GPU_VULKAN), debug_ui_vulkan_none.cpp when it doesn't.
// debug_ui_init(), debug_ui_render(), debug_ui_on_swapchain_recreated()
// (debug_ui.h) and ui_texture_add_vulkan() (ui_texture.h) live there too.

#include <cstdint>

#include "debug_ui.h"
#include "ui_texture.h"

struct ImDrawData;

enum class DebugUiRenderer
{
    none,
    vulkan,
    gl,
};

// debug_ui.cpp's, shared with the Vulkan side.
void        debug_ui_create_context(SDL_Window* window);
void        debug_ui_set_renderer(DebugUiRenderer renderer);
ImDrawData* debug_ui_finish_frame();   // ImGui::Render + the draw data hook

void debug_ui_vulkan_shutdown();
void debug_ui_vulkan_new_frame();
void debug_ui_vulkan_remove_texture(uint64_t texture_id);
