// debug_ui's Vulkan side in a build without Vulkan (MULTITAP_GPU_VULKAN off).
// The shell never sets ImGui up for Vulkan there, so none of this is called.

#include "debug_ui_vulkan.h"

bool debug_ui_init(SDL_Window* window, const VulkanContext* context, const VulkanSwapchain* swapchain)
{
    static_cast<void>(window);
    static_cast<void>(context);
    static_cast<void>(swapchain);
    return false;
}

void debug_ui_vulkan_shutdown()
{
}

void debug_ui_vulkan_new_frame()
{
}

void debug_ui_render(VkCommandBuffer command_buffer)
{
    static_cast<void>(command_buffer);
}

void debug_ui_on_swapchain_recreated(const VulkanSwapchain* swapchain)
{
    static_cast<void>(swapchain);
}

uint64_t ui_texture_add_vulkan(VkImageView view)
{
    static_cast<void>(view);
    return 0;
}

void debug_ui_vulkan_remove_texture(uint64_t texture_id)
{
    static_cast<void>(texture_id);
}
