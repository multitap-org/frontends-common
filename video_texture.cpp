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
    if (shell->api == GpuApi::gl)
    {
        return true;   // all three (gl_texture.h: 0RGB1555 is converted on upload)
    }
    return streaming_texture_format_supported(&shell->vulkan, vulkan_format(format));
}

bool video_texture_create(VideoTexture* texture, AppShell* shell, uint32_t width, uint32_t height,
                          FramePixelFormat format, bool opaque)
{
    video_texture_destroy(texture, shell);
    if (shell->api == GpuApi::gl)
    {
        // Always opaque on GL: the shown texture is written with alpha 1.
        if (!gl_streaming_texture_create(&texture->gl, &shell->gl, width, height, format))
        {
            *texture = VideoTexture{};
            return false;
        }
        texture->imgui_id = texture->gl.shown;   // ImGui's GL backend: the texture name
    }
    else
    {
        if (!streaming_texture_create_format(&texture->vulkan, &shell->vulkan, width, height, vulkan_format(format),
                                             opaque))
        {
            *texture = VideoTexture{};
            return false;
        }
        texture->imgui_id = debug_ui_add_texture(texture->vulkan.view);
    }
    texture->format = format;
    texture->bytes_per_pixel = frame_pixel_bytes(format);
    texture->capacity_width = width;
    texture->capacity_height = height;
    return true;
}

bool video_texture_created(const VideoTexture* texture)
{
    return texture->vulkan.image != VK_NULL_HANDLE || texture->gl.upload != 0;
}

void video_texture_destroy(VideoTexture* texture, AppShell* shell)
{
    if (!video_texture_created(texture) && texture->imgui_id == 0)
    {
        return;
    }
    if (shell->api == GpuApi::gl)
    {
        // GL keeps a texture alive until the commands that use it are done.
        gl_streaming_texture_destroy(&texture->gl, &shell->gl);
    }
    else
    {
        vulkan_context_wait_idle(&shell->vulkan);   // earlier frames may still be drawing it
        debug_ui_remove_texture(texture->imgui_id);
        streaming_texture_destroy(&texture->vulkan, &shell->vulkan);
    }
    *texture = VideoTexture{};
}

bool video_texture_upload(VideoTexture* texture, AppShell* shell, const void* pixels, uint32_t width,
                          uint32_t height)
{
    if (!video_texture_created(texture))
    {
        return false;
    }
    bool uploaded = shell->api == GpuApi::gl
                        ? gl_streaming_texture_upload(&texture->gl, &shell->gl, pixels, width, height)
                        : streaming_texture_upload(&texture->vulkan, renderer_command_buffer(&shell->renderer),
                                                   renderer_frame_index(&shell->renderer), pixels, width, height);
    if (!uploaded)
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
    *u = texture->capacity_width > 0
             ? static_cast<float>(texture->content_width) / static_cast<float>(texture->capacity_width)
             : 0.0f;
    *v = texture->capacity_height > 0
             ? static_cast<float>(texture->content_height) / static_cast<float>(texture->capacity_height)
             : 0.0f;
}
