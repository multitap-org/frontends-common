#include "video_texture.h"

#include "app_shell.h"
#include "debug_ui.h"

// The Vulkan format for each pixel format (vk_texture.h: the bit layouts
// match libretro's on little-endian machines).
static VkFormat vulkan_format(FramePixelFormat format)
{
    switch (format)
    {
        case FramePixelFormat::rgb565:   return VK_FORMAT_R5G6B5_UNORM_PACK16;
        case FramePixelFormat::xrgb1555: return VK_FORMAT_A1R5G5B5_UNORM_PACK16;
        case FramePixelFormat::xrgb8888: return VK_FORMAT_B8G8R8A8_UNORM;
    }
    return VK_FORMAT_B8G8R8A8_UNORM;
}

bool video_texture_format_supported(const AppShell* shell, FramePixelFormat format)
{
    return streaming_texture_format_supported(&shell->vulkan, vulkan_format(format));
}

bool video_texture_create(VideoTexture* texture, AppShell* shell, uint32_t width, uint32_t height,
                          FramePixelFormat format, bool opaque)
{
    video_texture_destroy(texture, shell);
    if (!streaming_texture_create_format(&texture->vulkan, &shell->vulkan, width, height, vulkan_format(format),
                                         opaque))
    {
        *texture = VideoTexture{};
        return false;
    }
    texture->format = format;
    texture->bytes_per_pixel = frame_pixel_bytes(format);
    texture->capacity_width = width;
    texture->capacity_height = height;
    texture->imgui_id = debug_ui_add_texture(texture->vulkan.view);
    return true;
}

bool video_texture_created(const VideoTexture* texture)
{
    return texture->vulkan.image != VK_NULL_HANDLE;
}

void video_texture_destroy(VideoTexture* texture, AppShell* shell)
{
    if (!video_texture_created(texture) && texture->imgui_id == 0)
    {
        return;
    }
    vkDeviceWaitIdle(shell->vulkan.device);   // earlier frames may still be drawing it
    debug_ui_remove_texture(texture->imgui_id);
    streaming_texture_destroy(&texture->vulkan, &shell->vulkan);
    *texture = VideoTexture{};
}

bool video_texture_upload(VideoTexture* texture, AppShell* shell, const void* pixels, uint32_t width,
                          uint32_t height)
{
    if (!video_texture_created(texture) ||
        !streaming_texture_upload(&texture->vulkan, renderer_command_buffer(&shell->renderer),
                                  renderer_frame_index(&shell->renderer), pixels, width, height))
    {
        return false;
    }
    texture->content_width = width;
    texture->content_height = height;
    texture->has_content = true;
    return true;
}

void video_texture_uv_max(const VideoTexture* texture, float* u, float* v)
{
    streaming_texture_uv_max(&texture->vulkan, u, v);
}
