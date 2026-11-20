#include "debug_ui.h"

#include <cstdlib>
#include <cstring>

#include <SDL3/SDL.h>

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_opengl3.h"

#include "debug_ui_vulkan.h"
#include "text_input.h"

// Which renderer backend ImGui was set up with (ImGui itself is one global
// context; this goes with it).
static DebugUiRenderer g_renderer = DebugUiRenderer::none;

void debug_ui_set_renderer(DebugUiRenderer renderer)
{
    g_renderer = renderer;
}

void debug_ui_create_context(SDL_Window* window)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    // No gamepad navigation yet: SDL takes some devices for gamepads that
    // aren't (a keyboard's thumbstick receiver reads as a stick held all the
    // way up), and ImGui then scrolls the focused window to the top every
    // frame. To come back with the input work, ignoring such devices.

    // Scale for high-DPI displays (e.g. 150% on Windows, Retina on macOS).
    float display_scale = SDL_GetWindowDisplayScale(window);
    if (display_scale <= 0.0f)
    {
        display_scale = 1.0f;
    }

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.FrameRounding = 3.0f;
    style.GrabRounding  = 3.0f;
    style.ScaleAllSizes(display_scale);
    style.FontScaleDpi = display_scale;
}

bool debug_ui_init_gl(SDL_Window* window, SDL_GLContext context, const char* glsl_version)
{
    debug_ui_create_context(window);
    if (!ImGui_ImplSDL3_InitForOpenGL(window, context))
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "ImGui_ImplSDL3_InitForOpenGL failed");
        return false;
    }
    if (!ImGui_ImplOpenGL3_Init(glsl_version))
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "ImGui_ImplOpenGL3_Init failed");
        return false;
    }
    g_renderer = DebugUiRenderer::gl;
    return true;
}

void debug_ui_shutdown()
{
    if (ImGui::GetCurrentContext() == nullptr)
    {
        return;
    }
    ImGuiIO& io = ImGui::GetIO();
    if (io.BackendRendererUserData != nullptr)
    {
        if (g_renderer == DebugUiRenderer::gl)
        {
            ImGui_ImplOpenGL3_Shutdown();
        }
        else
        {
            debug_ui_vulkan_shutdown();
        }
    }
    if (io.BackendPlatformUserData != nullptr)
    {
        ImGui_ImplSDL3_Shutdown();
    }
    ImGui::DestroyContext();
    g_renderer = DebugUiRenderer::none;
}

// ImGui calls this (during EndFrame) when its text input state changes: a
// field got or lost focus, or its caret moved.
static void imgui_set_ime_data(ImGuiContext*, ImGuiViewport*, ImGuiPlatformImeData* data)
{
    TextInput* input = static_cast<TextInput*>(ImGui::GetPlatformIO().Platform_ImeUserData);
    if (input == nullptr)
    {
        return;
    }
    TextInputRect area = {};
    area.x = static_cast<int32_t>(data->InputPos.x);
    area.y = static_cast<int32_t>(data->InputPos.y);
    area.w = 1;
    area.h = static_cast<int32_t>(data->InputLineHeight);
    text_input_set_ui_field(input, data->WantVisible || data->WantTextInput, &area);
}

void debug_ui_route_text_input(TextInput* input)
{
    // Replaces the SDL3 backend's handler (set by ImGui_ImplSDL3_Init); the
    // backend's per-frame IME update then has nothing to do.
    ImGuiPlatformIO& platform_io = ImGui::GetPlatformIO();
    platform_io.Platform_SetImeDataFn = imgui_set_ime_data;
    platform_io.Platform_ImeUserData = input;
}

bool debug_ui_process_event(const SDL_Event* event)
{
    return ImGui_ImplSDL3_ProcessEvent(event);
}

void debug_ui_begin_frame()
{
    if (g_renderer == DebugUiRenderer::gl)
    {
        ImGui_ImplOpenGL3_NewFrame();
    }
    else
    {
        debug_ui_vulkan_new_frame();
    }
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
}

static DebugUiDrawDataHook g_draw_data_hook = nullptr;
static void*               g_draw_data_hook_user = nullptr;

void debug_ui_set_draw_data_hook(DebugUiDrawDataHook hook, void* user)
{
    g_draw_data_hook = hook;
    g_draw_data_hook_user = user;
}

ImDrawData* debug_ui_finish_frame()
{
    ImGui::Render();
    if (g_draw_data_hook != nullptr)
    {
        g_draw_data_hook(ImGui::GetDrawData(), g_draw_data_hook_user);
    }
    return ImGui::GetDrawData();
}

void debug_ui_render_gl()
{
    ImGui_ImplOpenGL3_RenderDrawData(debug_ui_finish_frame());
}

void debug_ui_end_frame_without_render()
{
    ImGui::EndFrame();
}

// ---------------------------------------------------------------------------
// Textures
// ---------------------------------------------------------------------------

void debug_ui_remove_texture(uint64_t texture_id)
{
    // On GL an ImGui id is the texture's own name: nothing was registered.
    if (g_renderer == DebugUiRenderer::vulkan)
    {
        debug_ui_vulkan_remove_texture(texture_id);
    }
}

void debug_ui_image_nearest(uint64_t texture_id, float width, float height, float uv_max_x, float uv_max_y)
{
    // Draw callbacks switch the backend's sampler for the draw commands between them.
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    const ImGuiPlatformIO& platform_io = ImGui::GetPlatformIO();

    draw_list->AddCallback(platform_io.DrawCallback_SetSamplerNearest, nullptr);
    ImGui::Image(ImTextureRef(static_cast<ImTextureID>(texture_id)), ImVec2(width, height), ImVec2(0.0f, 0.0f),
                 ImVec2(uv_max_x, uv_max_y));
    draw_list->AddCallback(platform_io.DrawCallback_SetSamplerLinear, nullptr);
}
