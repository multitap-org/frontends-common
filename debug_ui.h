#pragma once

// Dear ImGui setup for a frontend's development/debug UI.
// Thin wrapper over the official imgui SDL3 + Vulkan / OpenGL backends (deps/imgui/backends).

#include <cstdint>
#include <SDL3/SDL_video.h>   // SDL_GLContext
#include <vulkan/vulkan.h>

struct SDL_Window;
union SDL_Event;
struct VulkanContext;
struct VulkanSwapchain;

bool debug_ui_init(SDL_Window* window, const VulkanContext* context, const VulkanSwapchain* swapchain);
// The same on an OpenGL (ES) context: ImGui's GL backend, `glsl_version` its
// shaders' version line (gl_context_imgui_glsl_version).
bool debug_ui_init_gl(SDL_Window* window, SDL_GLContext context, const char* glsl_version);
void debug_ui_shutdown();

// Routes ImGui's text fields to `input` (text_input.h) instead of the SDL3
// backend's own SDL_StartTextInput calls: one owner for the window's text input.
struct TextInput;
void debug_ui_route_text_input(TextInput* input);

// Forward every SDL event here. Returns true if imgui used it.
bool debug_ui_process_event(const SDL_Event* event);

// Call once per frame before any ImGui:: widget calls.
void debug_ui_begin_frame();

// Finishes the imgui frame and records its draw commands. Must be called inside
// the active rendering pass (between renderer_begin_frame and renderer_end_frame).
void debug_ui_render(VkCommandBuffer command_buffer);
// GL: draws into the bound framebuffer (the window's), right away.
void debug_ui_render_gl();

// Called with ImGui's draw data between finishing the frame and drawing it,
// to change what gets drawn (e.g. add text shadows). nullptr: none.
struct ImDrawData;
typedef void (*DebugUiDrawDataHook)(ImDrawData* draw_data, void* user);
void debug_ui_set_draw_data_hook(DebugUiDrawDataHook hook, void* user);

// Finishes the imgui frame without drawing (e.g. the frame was skipped).
void debug_ui_end_frame_without_render();

// Call after the swapchain was recreated (image count may have changed).
void debug_ui_on_swapchain_recreated(const VulkanSwapchain* swapchain);

// Textures ImGui draws: debug_ui implements ui_texture.h (ui_texture_add_vulkan,
// ui_texture_from_gl, ui_texture_remove) for ImGui.
