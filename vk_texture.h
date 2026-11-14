#pragma once

// A GPU texture that the CPU updates every frame (e.g. a software-rendered
// core's video). By default pixels are B8G8R8A8 in memory, which is what
// host-libretro's converted XRGB8888-with-alpha frames are.
//
// Other formats (streaming_texture_create_format) take a core's frames as the
// core wrote them, so the CPU only copies: R5G6B5 (libretro RGB565),
// A1R5G5B5 (0RGB1555), B8G8R8A8 (XRGB8888). Their bit layouts match the
// libretro formats on little-endian machines, and the texture unit expands
// them to float as it samples, for free. `opaque` makes the image view read
// alpha as 1, so the unused X / top bits never turn into transparency.
//
// Each frame slot (renderer_frame_index) has its own staging buffer: the CPU
// writes slot N while the GPU may still be copying from the other slot.
// The image itself is shared; barriers order the copy after earlier frames'
// reads, because barriers cover everything submitted earlier on the queue.
//
// The texture has a fixed capacity; uploads may be smaller (the core's current
// size). Draw only the uploaded region: see streaming_texture_uv_max().

#include <cstdint>
#include <vulkan/vulkan.h>

#include "renderer.h"   // FRAMES_IN_FLIGHT
#include "vk_context.h"   // VulkanPath

struct VulkanContext;

constexpr VkFormat STREAMING_TEXTURE_FORMAT = VK_FORMAT_B8G8R8A8_UNORM;

struct StreamingTexture
{
    VkFormat       format          = STREAMING_TEXTURE_FORMAT;
    uint32_t       bytes_per_pixel = 4;

    VkImage        image        = VK_NULL_HANDLE;
    VkDeviceMemory image_memory = VK_NULL_HANDLE;
    VkImageView    view         = VK_NULL_HANDLE;
    VkImageLayout  layout       = VK_IMAGE_LAYOUT_UNDEFINED;   // tracked so barriers know the old layout

    VkBuffer       staging[FRAMES_IN_FLIGHT]        = {};
    VkDeviceMemory staging_memory[FRAMES_IN_FLIGHT] = {};
    void*          staging_mapped[FRAMES_IN_FLIGHT] = {};      // persistently mapped, host-coherent

    uint32_t capacity_width  = 0;
    uint32_t capacity_height = 0;

    uint32_t content_width  = 0;   // size of the last upload
    uint32_t content_height = 0;
    bool     has_content    = false;   // false until the first upload: don't draw it before that

    // From the context at creation: how uploads record their barriers (vk_barrier.h).
    VulkanPath                path     = VulkanPath::vulkan_1_3;
    PFN_vkCmdPipelineBarrier2 barrier2 = nullptr;
};

bool streaming_texture_create(StreamingTexture* texture, const VulkanContext* context, uint32_t width, uint32_t height);

// The same in `format` (one of the formats above; others fail). `opaque`:
// the view reads alpha as 1.
bool streaming_texture_create_format(StreamingTexture* texture, const VulkanContext* context, uint32_t width,
                                     uint32_t height, VkFormat format, bool opaque);

// True if this GPU can use `format` for a streaming texture: copy into it,
// sample it, filter it linearly. Required by the Vulkan spec for all of the
// formats above; checked anyway.
bool streaming_texture_format_supported(const VulkanContext* context, VkFormat format);

// The GPU must be done with the texture (call vkDeviceWaitIdle first). Safe on a
// partially created texture.
void streaming_texture_destroy(StreamingTexture* texture, const VulkanContext* context);

// Copies `pixels` (width * height, tightly packed, bytes_per_pixel each) into this frame
// slot's staging buffer and records the copy into `command_buffer`. Must be
// recorded OUTSIDE a rendering pass (before renderer_begin_rendering).
// Fails if the frame is bigger than the capacity.
bool streaming_texture_upload(StreamingTexture* texture, VkCommandBuffer command_buffer, uint32_t frame_index,
                              const void* pixels, uint32_t width, uint32_t height);

// Bottom-right texture coordinate of the uploaded region (top-left is 0,0).
void streaming_texture_uv_max(const StreamingTexture* texture, float* u, float* v);
