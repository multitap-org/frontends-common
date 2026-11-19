#pragma once

// RetroArch slang shader presets (.slangp) on the core's picture, through
// librashader (deps/librashader: its MIT C header; the library itself is
// MPL-2.0 / GPL-3.0 and is loaded at run time, never linked). Without the
// library there is no chain and the picture is drawn as before (picture.h).
//
// The library: librashader.so / .dll / .dylib, looked for in the folders the
// frontend gives (e.g. next to the program, the program's data folder), then
// by the system's own search. Only the functions used here are looked up.
//
// Per frame, a chain goes:
//
//   core texture (streaming, bigger than the frame)
//     -> input: a nearest blit of the shown part (crop included), exact size,
//        B8G8R8A8. librashader samples its whole input image, so it must not
//        contain anything outside the picture. Also converts R5G6B5 etc.
//     -> librashader's passes (its own images, recorded into our command buffer)
//     -> output: the picture's size in window pixels, B8G8R8A8
//     -> drawn 1:1 through ImGui, like picture.h (behind the UI or in a window)
//
// Each frame, in this order (same places as picture.h's):
//   shader_chain_prepare()   while building the UI (may recreate the images,
//                            which waits for the GPU; only when sizes change)
//   shader_chain_draw()      adds the output to an ImGui draw list
//   shader_chain_record()    after the texture upload, before rendering starts

#include <cstdint>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include "librashader.h"   // LIBRA_RUNTIME_VULKAN is set by CMake

#include "dynamic_library.h"
#include "viewport.h"
#include "vk_texture.h"

struct ImDrawList;
struct VulkanContext;

// librashader's API version this code is written for (librashader.h:
// LIBRASHADER_CURRENT_VERSION when it was added). The library must have this
// ABI (LIBRASHADER_CURRENT_ABI) and at least this API version.
constexpr LIBRASHADER_API_VERSION SHADER_LIBRARY_API_VERSION = 5;
constexpr LIBRASHADER_ABI_VERSION SHADER_LIBRARY_ABI_VERSION = 2;

struct ShaderLibrary
{
    DynamicLibrary library = {};
    bool           loaded  = false;
    std::string    path;            // the file that was loaded
    LIBRASHADER_API_VERSION api_version = 0;

    PFN_libra_error_write       error_write       = nullptr;
    PFN_libra_error_free_string error_free_string = nullptr;
    PFN_libra_error_free        error_free        = nullptr;
    PFN_libra_preset_create     preset_create     = nullptr;
    PFN_libra_preset_free       preset_free       = nullptr;
    PFN_libra_vk_filter_chain_create    vk_filter_chain_create    = nullptr;
    PFN_libra_vk_filter_chain_frame     vk_filter_chain_frame     = nullptr;
    PFN_libra_vk_filter_chain_set_param vk_filter_chain_set_param = nullptr;
    PFN_libra_vk_filter_chain_free      vk_filter_chain_free      = nullptr;
};

// Tries <folder>/<file name> for each folder, then the bare file name. False
// (logged once, as info: running without it is normal) if none loads or the
// version doesn't fit.
bool shader_library_load(ShaderLibrary* library, const std::vector<std::string>& folders);
void shader_library_unload(ShaderLibrary* library);

// An image the chain owns (its input or its output).
struct ShaderChainImage
{
    VkImage        image        = VK_NULL_HANDLE;
    VkDeviceMemory image_memory = VK_NULL_HANDLE;
    VkImageView    view         = VK_NULL_HANDLE;
    VkImageLayout  layout       = VK_IMAGE_LAYOUT_UNDEFINED;
    uint32_t       width        = 0;
    uint32_t       height       = 0;
    uint64_t       texture_id   = 0;   // ImGui id (the output only)
};

struct ShaderChain
{
    libra_vk_filter_chain_t chain = nullptr;
    std::string preset_path;     // the loaded preset ("" = none)
    std::string failed_path;     // the last preset that failed to load (not retried every frame)

    ShaderChainImage input  = {};
    ShaderChainImage output = {};
    uint64_t frame_count = 0;    // frames rendered through this chain (shaders animate with it)

    // What this frame draws (set by shader_chain_prepare).
    ViewportResult viewport = {};
    bool           ready    = false;
};

// Loads `preset_path` (a .slangp) into a new chain, replacing the current one.
// Waits for the GPU. False (logged) if the library isn't loaded or the preset
// fails; the chain is then empty and the picture is drawn without it.
bool shader_chain_load(ShaderChain* chain, const ShaderLibrary* library, const VulkanContext* context,
                       const char* preset_path);

// Frees everything. Waits for the GPU. Safe on an empty chain.
void shader_chain_destroy(ShaderChain* chain, const ShaderLibrary* library, const VulkanContext* context);

bool shader_chain_active(const ShaderChain* chain);

// Decides what this frame draws. False: draw with picture.h instead (no
// chain, no picture yet, or the images couldn't be made).
bool shader_chain_prepare(ShaderChain* chain, const VulkanContext* context, const StreamingTexture* source,
                          const ViewportResult* viewport);

// Adds the output to `draw_list`; the viewport's (0, 0) is at (origin_x, origin_y).
void shader_chain_draw(const ShaderChain* chain, ImDrawList* draw_list, float origin_x, float origin_y);

// Records the input copy and the chain's passes into `command_buffer`
// (outside a rendering pass, after the source's upload).
void shader_chain_record(ShaderChain* chain, const ShaderLibrary* library, VkCommandBuffer command_buffer,
                         StreamingTexture* source);
