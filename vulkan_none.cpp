// The Vulkan modules' entry points that the API-neutral code calls
// (video_texture, video_output, frontends' librashader loading), in a build
// without Vulkan (MULTITAP_GPU_VULKAN off: the Android 5 APK, where nothing
// may link libvulkan.so). The shell never runs on Vulkan in such a build, so
// none of these is reached except shader_library_load, which says there's
// no library (librashader's runtime here is Vulkan's).

#include <SDL3/SDL_log.h>

#include "picture.h"
#include "renderer.h"
#include "screen_effect.h"
#include "shader_chain.h"
#include "vk_context.h"
#include "vk_texture.h"

bool shader_library_load(ShaderLibrary* library, const std::vector<std::string>& folders)
{
    static_cast<void>(folders);
    *library = ShaderLibrary{};
    SDL_LogInfo(SDL_LOG_CATEGORY_GPU, "This build has no Vulkan: no librashader (.slangp presets)");
    return false;
}

void shader_library_unload(ShaderLibrary* library)
{
    *library = ShaderLibrary{};
}

void vulkan_context_wait_idle(const VulkanContext* context)
{
    static_cast<void>(context);
}

bool streaming_texture_format_supported(const VulkanContext* context, VkFormat format)
{
    static_cast<void>(context);
    static_cast<void>(format);
    return false;
}

bool streaming_texture_create_format(StreamingTexture* texture, const VulkanContext* context, uint32_t width,
                                     uint32_t height, VkFormat format, bool opaque)
{
    static_cast<void>(texture);
    static_cast<void>(context);
    static_cast<void>(width);
    static_cast<void>(height);
    static_cast<void>(format);
    static_cast<void>(opaque);
    return false;
}

void streaming_texture_destroy(StreamingTexture* texture, const VulkanContext* context)
{
    static_cast<void>(texture);
    static_cast<void>(context);
}

bool streaming_texture_upload(StreamingTexture* texture, VkCommandBuffer command_buffer, uint32_t frame_index,
                              const void* pixels, uint32_t width, uint32_t height)
{
    static_cast<void>(texture);
    static_cast<void>(command_buffer);
    static_cast<void>(frame_index);
    static_cast<void>(pixels);
    static_cast<void>(width);
    static_cast<void>(height);
    return false;
}

VkCommandBuffer renderer_command_buffer(const Renderer* renderer)
{
    static_cast<void>(renderer);
    return VK_NULL_HANDLE;
}

uint32_t renderer_frame_index(const Renderer* renderer)
{
    static_cast<void>(renderer);
    return 0;
}

void picture_destroy(Picture* picture, const VulkanContext* context)
{
    static_cast<void>(picture);
    static_cast<void>(context);
}

bool picture_prepare(Picture* picture, const VulkanContext* context, const StreamingTexture* source,
                     const ViewportResult* viewport)
{
    static_cast<void>(picture);
    static_cast<void>(context);
    static_cast<void>(source);
    static_cast<void>(viewport);
    return false;
}

void picture_draw(const Picture* picture, ImDrawList* draw_list, const StreamingTexture* source,
                  uint64_t source_texture_id, float origin_x, float origin_y)
{
    static_cast<void>(picture);
    static_cast<void>(draw_list);
    static_cast<void>(source);
    static_cast<void>(source_texture_id);
    static_cast<void>(origin_x);
    static_cast<void>(origin_y);
}

void picture_record(Picture* picture, VkCommandBuffer command_buffer, StreamingTexture* source)
{
    static_cast<void>(picture);
    static_cast<void>(command_buffer);
    static_cast<void>(source);
}

bool shader_chain_load(ShaderChain* chain, const ShaderLibrary* library, const VulkanContext* context, uint32_t stage,
                       const char* preset_path)
{
    static_cast<void>(chain);
    static_cast<void>(library);
    static_cast<void>(context);
    static_cast<void>(stage);
    static_cast<void>(preset_path);
    return false;
}

void shader_chain_clear(ShaderChain* chain, const ShaderLibrary* library, const VulkanContext* context, uint32_t stage)
{
    static_cast<void>(library);
    static_cast<void>(context);
    chain->stages[stage] = ShaderStage{};
}

void shader_chain_destroy(ShaderChain* chain, const ShaderLibrary* library, const VulkanContext* context)
{
    static_cast<void>(library);
    static_cast<void>(context);
    *chain = ShaderChain{};
}

bool shader_chain_active(const ShaderChain* chain)
{
    static_cast<void>(chain);
    return false;
}

bool shader_chain_set_param(ShaderChain* chain, const ShaderLibrary* library, uint32_t stage, const char* name,
                            float value)
{
    static_cast<void>(chain);
    static_cast<void>(library);
    static_cast<void>(stage);
    static_cast<void>(name);
    static_cast<void>(value);
    return false;
}

bool shader_chain_prepare(ShaderChain* chain, const VulkanContext* context, const StreamingTexture* source,
                          const ViewportResult* viewport)
{
    static_cast<void>(chain);
    static_cast<void>(context);
    static_cast<void>(source);
    static_cast<void>(viewport);
    return false;
}

void shader_chain_draw(const ShaderChain* chain, ImDrawList* draw_list, float origin_x, float origin_y)
{
    static_cast<void>(chain);
    static_cast<void>(draw_list);
    static_cast<void>(origin_x);
    static_cast<void>(origin_y);
}

void shader_chain_record(ShaderChain* chain, const ShaderLibrary* library, VkCommandBuffer command_buffer,
                         StreamingTexture* source)
{
    static_cast<void>(chain);
    static_cast<void>(library);
    static_cast<void>(command_buffer);
    static_cast<void>(source);
}

void screen_effect_destroy(ScreenEffect* effect, const VulkanContext* context)
{
    static_cast<void>(context);
    *effect = ScreenEffect{};
}

bool screen_effect_prepare(ScreenEffect* effect, const VulkanContext* context, const ViewportResult* viewport)
{
    static_cast<void>(effect);
    static_cast<void>(context);
    static_cast<void>(viewport);
    return false;
}

void screen_effect_draw(const ScreenEffect* effect, ImDrawList* draw_list, float origin_x, float origin_y)
{
    static_cast<void>(effect);
    static_cast<void>(draw_list);
    static_cast<void>(origin_x);
    static_cast<void>(origin_y);
}

bool screen_effect_request_capture(ScreenEffect* effect)
{
    static_cast<void>(effect);
    return false;
}

void screen_effect_collect(ScreenEffect* effect, const VulkanContext* context)
{
    static_cast<void>(effect);
    static_cast<void>(context);
}

void screen_effect_record(ScreenEffect* effect, const VulkanContext* context, VkCommandBuffer command_buffer,
                          uint32_t frame_index, const ScreenEffectSource* source, const ScreenEffectParams* params)
{
    static_cast<void>(effect);
    static_cast<void>(context);
    static_cast<void>(command_buffer);
    static_cast<void>(frame_index);
    static_cast<void>(source);
    static_cast<void>(params);
}

bool screen_effect_capture_take(ScreenEffect* effect, VideoCapture* capture)
{
    static_cast<void>(effect);
    static_cast<void>(capture);
    return false;
}
