#include "screen_effect.h"

#include <cstring>

#include <SDL3/SDL_log.h>


#include "ui_texture.h"
#include "screen_effect_spirv.h"
#include "vk_barrier.h"

// The target's format: the same as the chain's output and the picture's copy.
constexpr VkFormat SCREEN_EFFECT_FORMAT = VK_FORMAT_B8G8R8A8_UNORM;

// shaders/screen_effect.frag's push constants.
struct ScreenEffectPush
{
    float source_rect[4];
    float knobs[4];         // zoom, desaturate, flash, bars
    float flash_color[4];
};

// ---------------------------------------------------------------------------
// Making and freeing
// ---------------------------------------------------------------------------

static void destroy_target(ScreenEffect* effect, const VulkanContext* context)
{
    if (effect->image != VK_NULL_HANDLE || effect->texture_id != 0)
    {
        vkDeviceWaitIdle(context->device);   // earlier frames may still be drawing or copying it
    }
    ui_texture_remove(effect->texture_id);
    if (effect->framebuffer != VK_NULL_HANDLE)
    {
        vkDestroyFramebuffer(context->device, effect->framebuffer, nullptr);
    }
    if (effect->view != VK_NULL_HANDLE)
    {
        vkDestroyImageView(context->device, effect->view, nullptr);
    }
    if (effect->image != VK_NULL_HANDLE)
    {
        vkDestroyImage(context->device, effect->image, nullptr);
    }
    if (effect->image_memory != VK_NULL_HANDLE)
    {
        vkFreeMemory(context->device, effect->image_memory, nullptr);
    }
    effect->texture_id = 0;
    effect->framebuffer = VK_NULL_HANDLE;
    effect->view = VK_NULL_HANDLE;
    effect->image = VK_NULL_HANDLE;
    effect->image_memory = VK_NULL_HANDLE;
    effect->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    effect->width = 0;
    effect->height = 0;
}

static void destroy_capture_buffer(ScreenEffect* effect, const VulkanContext* context)
{
    if (effect->capture_memory != VK_NULL_HANDLE && effect->capture_mapped != nullptr)
    {
        vkUnmapMemory(context->device, effect->capture_memory);
    }
    if (effect->capture_buffer != VK_NULL_HANDLE)
    {
        vkDestroyBuffer(context->device, effect->capture_buffer, nullptr);
    }
    if (effect->capture_memory != VK_NULL_HANDLE)
    {
        vkFreeMemory(context->device, effect->capture_memory, nullptr);
    }
    effect->capture_buffer = VK_NULL_HANDLE;
    effect->capture_memory = VK_NULL_HANDLE;
    effect->capture_mapped = nullptr;
    effect->capture_size = 0;
}

void screen_effect_destroy(ScreenEffect* effect, const VulkanContext* context)
{
    if (effect->pipeline != VK_NULL_HANDLE || effect->capture_buffer != VK_NULL_HANDLE)
    {
        vkDeviceWaitIdle(context->device);
    }
    destroy_target(effect, context);
    destroy_capture_buffer(effect, context);
    video_capture_free(&effect->captured);
    if (effect->descriptor_pool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(context->device, effect->descriptor_pool, nullptr);   // frees the sets
    }
    if (effect->sampler_linear != VK_NULL_HANDLE)
    {
        vkDestroySampler(context->device, effect->sampler_linear, nullptr);
    }
    if (effect->sampler_nearest != VK_NULL_HANDLE)
    {
        vkDestroySampler(context->device, effect->sampler_nearest, nullptr);
    }
    if (effect->pipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(context->device, effect->pipeline, nullptr);
    }
    if (effect->render_pass != VK_NULL_HANDLE)
    {
        vkDestroyRenderPass(context->device, effect->render_pass, nullptr);
    }
    if (effect->pipeline_layout != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(context->device, effect->pipeline_layout, nullptr);
    }
    if (effect->set_layout != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(context->device, effect->set_layout, nullptr);
    }
    *effect = ScreenEffect{};
}

static bool create_shader_module(const VulkanContext* context, const uint32_t* code, size_t size, VkShaderModule* module)
{
    VkShaderModuleCreateInfo info = {
        .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = size,
        .pCode    = code,
    };
    return vk_succeeded(vkCreateShaderModule(context->device, &info, nullptr, module), "vkCreateShaderModule (screen effect)");
}

static bool create_sampler(const VulkanContext* context, VkFilter filter, VkSampler* sampler)
{
    // Clamped: the picture's rectangle may touch the texture's edge, and the
    // zoom never samples outside the rectangle.
    VkSamplerCreateInfo info = {
        .sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter    = filter,
        .minFilter    = filter,
        .mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .maxLod       = 0.0f,
    };
    return vk_succeeded(vkCreateSampler(context->device, &info, nullptr, sampler), "vkCreateSampler (screen effect)");
}

// The 1.1 path's render pass. The image's layout is handled by our own
// barriers around it (as on the 1.3 path), so it starts and ends in
// COLOR_ATTACHMENT_OPTIMAL. Its old contents are never needed: every pixel
// is drawn (DONT_CARE).
static bool create_render_pass(ScreenEffect* effect, const VulkanContext* context)
{
    VkAttachmentDescription attachment = {
        .format         = SCREEN_EFFECT_FORMAT,
        .samples        = VK_SAMPLE_COUNT_1_BIT,
        .loadOp         = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .storeOp        = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout  = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .finalLayout    = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    };
    VkAttachmentReference color = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription subpass = {
        .pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments    = &color,
    };
    VkRenderPassCreateInfo info = {
        .sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments    = &attachment,
        .subpassCount    = 1,
        .pSubpasses      = &subpass,
    };
    return vk_succeeded(vkCreateRenderPass(context->device, &info, nullptr, &effect->render_pass),
                        "vkCreateRenderPass (screen effect)");
}

static bool create_pipeline(ScreenEffect* effect, const VulkanContext* context)
{
    VkDescriptorSetLayoutBinding binding = {
        .binding         = 0,
        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
        .stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT,
    };
    VkDescriptorSetLayoutCreateInfo set_layout_info = {
        .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1,
        .pBindings    = &binding,
    };
    if (!vk_succeeded(vkCreateDescriptorSetLayout(context->device, &set_layout_info, nullptr, &effect->set_layout),
                      "vkCreateDescriptorSetLayout (screen effect)"))
    {
        return false;
    }
    VkPushConstantRange push_range = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ScreenEffectPush) };
    VkPipelineLayoutCreateInfo layout_info = {
        .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount         = 1,
        .pSetLayouts            = &effect->set_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges    = &push_range,
    };
    if (!vk_succeeded(vkCreatePipelineLayout(context->device, &layout_info, nullptr, &effect->pipeline_layout),
                      "vkCreatePipelineLayout (screen effect)"))
    {
        return false;
    }
    if (context->path == VulkanPath::vulkan_1_1 && !create_render_pass(effect, context))
    {
        return false;
    }

    VkShaderModule vertex = VK_NULL_HANDLE;
    VkShaderModule fragment = VK_NULL_HANDLE;
    bool ok = create_shader_module(context, SCREEN_EFFECT_VERT_SPIRV, sizeof(SCREEN_EFFECT_VERT_SPIRV), &vertex) &&
              create_shader_module(context, SCREEN_EFFECT_FRAG_SPIRV, sizeof(SCREEN_EFFECT_FRAG_SPIRV), &fragment);
    if (ok)
    {
        VkPipelineShaderStageCreateInfo stages[2] = {
            { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
              .module = vertex, .pName = "main" },
            { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
              .module = fragment, .pName = "main" },
        };
        VkPipelineVertexInputStateCreateInfo vertex_input = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,   // the vertex shader makes its triangle
        };
        VkPipelineInputAssemblyStateCreateInfo input_assembly = {
            .sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        };
        VkPipelineViewportStateCreateInfo viewport_state = {
            .sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            .viewportCount = 1,
            .scissorCount  = 1,
        };
        VkPipelineRasterizationStateCreateInfo rasterization = {
            .sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
            .polygonMode = VK_POLYGON_MODE_FILL,
            .cullMode    = VK_CULL_MODE_NONE,
            .frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE,
            .lineWidth   = 1.0f,
        };
        VkPipelineMultisampleStateCreateInfo multisample = {
            .sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
        };
        VkPipelineColorBlendAttachmentState blend_attachment = {
            .blendEnable    = VK_FALSE,
            .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                              VK_COLOR_COMPONENT_A_BIT,
        };
        VkPipelineColorBlendStateCreateInfo blend = {
            .sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .attachmentCount = 1,
            .pAttachments    = &blend_attachment,
        };
        VkDynamicState dynamic_states[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dynamic = {
            .sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
            .dynamicStateCount = 2,
            .pDynamicStates    = dynamic_states,
        };
        // 1.3: dynamic rendering, so the pipeline names the target's format
        // instead of a render pass.
        VkFormat format = SCREEN_EFFECT_FORMAT;
        VkPipelineRenderingCreateInfo rendering = {
            .sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
            .colorAttachmentCount    = 1,
            .pColorAttachmentFormats = &format,
        };
        VkGraphicsPipelineCreateInfo info = {
            .sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .pNext               = context->path == VulkanPath::vulkan_1_3 ? &rendering : nullptr,
            .stageCount          = 2,
            .pStages             = stages,
            .pVertexInputState   = &vertex_input,
            .pInputAssemblyState = &input_assembly,
            .pViewportState      = &viewport_state,
            .pRasterizationState = &rasterization,
            .pMultisampleState   = &multisample,
            .pColorBlendState    = &blend,
            .pDynamicState       = &dynamic,
            .layout              = effect->pipeline_layout,
            .renderPass          = effect->render_pass,   // VK_NULL_HANDLE on the 1.3 path
            .subpass             = 0,
        };
        ok = vk_succeeded(vkCreateGraphicsPipelines(context->device, VK_NULL_HANDLE, 1, &info, nullptr, &effect->pipeline),
                          "vkCreateGraphicsPipelines (screen effect)");
    }
    if (fragment != VK_NULL_HANDLE)
    {
        vkDestroyShaderModule(context->device, fragment, nullptr);
    }
    if (vertex != VK_NULL_HANDLE)
    {
        vkDestroyShaderModule(context->device, vertex, nullptr);
    }
    if (!ok)
    {
        return false;
    }

    if (!create_sampler(context, VK_FILTER_NEAREST, &effect->sampler_nearest) ||
        !create_sampler(context, VK_FILTER_LINEAR, &effect->sampler_linear))
    {
        return false;
    }
    VkDescriptorPoolSize pool_size = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, FRAMES_IN_FLIGHT };
    VkDescriptorPoolCreateInfo pool_info = {
        .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets       = FRAMES_IN_FLIGHT,
        .poolSizeCount = 1,
        .pPoolSizes    = &pool_size,
    };
    if (!vk_succeeded(vkCreateDescriptorPool(context->device, &pool_info, nullptr, &effect->descriptor_pool),
                      "vkCreateDescriptorPool (screen effect)"))
    {
        return false;
    }
    VkDescriptorSetLayout layouts[FRAMES_IN_FLIGHT] = {};
    for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; i++)
    {
        layouts[i] = effect->set_layout;
    }
    VkDescriptorSetAllocateInfo allocate_info = {
        .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool     = effect->descriptor_pool,
        .descriptorSetCount = FRAMES_IN_FLIGHT,
        .pSetLayouts        = layouts,
    };
    return vk_succeeded(vkAllocateDescriptorSets(context->device, &allocate_info, effect->sets),
                        "vkAllocateDescriptorSets (screen effect)");
}

static bool create_target(ScreenEffect* effect, const VulkanContext* context, uint32_t width, uint32_t height)
{
    VkImageCreateInfo image_info = {
        .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType     = VK_IMAGE_TYPE_2D,
        .format        = SCREEN_EFFECT_FORMAT,
        .extent        = { width, height, 1 },
        .mipLevels     = 1,
        .arrayLayers   = 1,
        .samples       = VK_SAMPLE_COUNT_1_BIT,
        .tiling        = VK_IMAGE_TILING_OPTIMAL,
        .usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (!vk_succeeded(vkCreateImage(context->device, &image_info, nullptr, &effect->image), "vkCreateImage (screen effect)"))
    {
        return false;
    }
    VkMemoryRequirements requirements = {};
    vkGetImageMemoryRequirements(context->device, effect->image, &requirements);
    uint32_t type_index = 0;
    if (!vulkan_find_memory_type(context, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type_index))
    {
        SDL_LogError(SDL_LOG_CATEGORY_GPU, "No device-local memory for the screen effect");
        return false;
    }
    VkMemoryAllocateInfo allocate_info = {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize  = requirements.size,
        .memoryTypeIndex = type_index,
    };
    if (!vk_succeeded(vkAllocateMemory(context->device, &allocate_info, nullptr, &effect->image_memory),
                      "vkAllocateMemory (screen effect)") ||
        !vk_succeeded(vkBindImageMemory(context->device, effect->image, effect->image_memory, 0),
                      "vkBindImageMemory (screen effect)"))
    {
        return false;
    }
    VkImageViewCreateInfo view_info = {
        .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image            = effect->image,
        .viewType         = VK_IMAGE_VIEW_TYPE_2D,
        .format           = SCREEN_EFFECT_FORMAT,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    if (!vk_succeeded(vkCreateImageView(context->device, &view_info, nullptr, &effect->view),
                      "vkCreateImageView (screen effect)"))
    {
        return false;
    }
    if (effect->render_pass != VK_NULL_HANDLE)
    {
        VkFramebufferCreateInfo framebuffer_info = {
            .sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass      = effect->render_pass,
            .attachmentCount = 1,
            .pAttachments    = &effect->view,
            .width           = width,
            .height          = height,
            .layers          = 1,
        };
        if (!vk_succeeded(vkCreateFramebuffer(context->device, &framebuffer_info, nullptr, &effect->framebuffer),
                          "vkCreateFramebuffer (screen effect)"))
        {
            return false;
        }
    }
    effect->texture_id = ui_texture_add_vulkan(effect->view);
    effect->width = width;
    effect->height = height;
    effect->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    return true;
}

// A host-visible buffer the target is copied into. Cached memory when there
// is some: the CPU reads it back, and uncached memory is very slow to read.
static bool create_capture_buffer(ScreenEffect* effect, const VulkanContext* context, VkDeviceSize size)
{
    VkBufferCreateInfo buffer_info = {
        .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size        = size,
        .usage       = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    if (!vk_succeeded(vkCreateBuffer(context->device, &buffer_info, nullptr, &effect->capture_buffer),
                      "vkCreateBuffer (screen capture)"))
    {
        return false;
    }
    VkMemoryRequirements requirements = {};
    vkGetBufferMemoryRequirements(context->device, effect->capture_buffer, &requirements);
    uint32_t type_index = 0;
    VkMemoryPropertyFlags cached = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    VkMemoryPropertyFlags coherent = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    VkMemoryPropertyFlags chosen = cached;
    if (!vulkan_find_memory_type(context, requirements.memoryTypeBits, cached, &type_index))
    {
        chosen = coherent;
        if (!vulkan_find_memory_type(context, requirements.memoryTypeBits, coherent, &type_index))
        {
            SDL_LogError(SDL_LOG_CATEGORY_GPU, "No host-visible memory for a screen capture");
            return false;
        }
    }
    VkPhysicalDeviceMemoryProperties memory_properties = {};
    vkGetPhysicalDeviceMemoryProperties(context->physical_device, &memory_properties);
    effect->capture_coherent =
        (memory_properties.memoryTypes[type_index].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    VkMemoryAllocateInfo allocate_info = {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize  = requirements.size,
        .memoryTypeIndex = type_index,
    };
    if (!vk_succeeded(vkAllocateMemory(context->device, &allocate_info, nullptr, &effect->capture_memory),
                      "vkAllocateMemory (screen capture)") ||
        !vk_succeeded(vkBindBufferMemory(context->device, effect->capture_buffer, effect->capture_memory, 0),
                      "vkBindBufferMemory (screen capture)") ||
        !vk_succeeded(vkMapMemory(context->device, effect->capture_memory, 0, VK_WHOLE_SIZE, 0, &effect->capture_mapped),
                      "vkMapMemory (screen capture)"))
    {
        return false;
    }
    effect->capture_size = size;
    SDL_LogInfo(SDL_LOG_CATEGORY_GPU, "Screen capture buffer: %llu bytes (%s)", static_cast<unsigned long long>(size),
                chosen == cached ? "cached" : "coherent, uncached");
    return true;
}

// ---------------------------------------------------------------------------
// Per frame
// ---------------------------------------------------------------------------

bool screen_effect_prepare(ScreenEffect* effect, const VulkanContext* context, const ViewportResult* viewport)
{
    effect->ready = false;
    effect->path = context->path;
    effect->barrier2 = context->cmd_pipeline_barrier2;
    effect->begin_rendering = context->cmd_begin_rendering;
    effect->end_rendering = context->cmd_end_rendering;
    if (effect->failed || viewport->width == 0 || viewport->height == 0)
    {
        return false;
    }
    uint32_t limit = context->physical_device_properties.limits.maxImageDimension2D;
    if (viewport->width > limit || viewport->height > limit)
    {
        return false;   // a picture bigger than the GPU's images (zoomed far off screen): no effect
    }
    if (effect->pipeline == VK_NULL_HANDLE && !create_pipeline(effect, context))
    {
        SDL_LogError(SDL_LOG_CATEGORY_GPU, "Screen effect: the pipeline couldn't be made; no effects");
        screen_effect_destroy(effect, context);
        effect->failed = true;
        return false;
    }
    if (effect->image == VK_NULL_HANDLE || effect->width != viewport->width || effect->height != viewport->height)
    {
        destroy_target(effect, context);
        if (!create_target(effect, context, viewport->width, viewport->height))
        {
            destroy_target(effect, context);
            return false;
        }
        SDL_LogInfo(SDL_LOG_CATEGORY_GPU, "Screen effect target: %ux%u", viewport->width, viewport->height);
    }
    effect->viewport = *viewport;
    effect->ready = true;
    return true;
}

bool screen_effect_shown_image(const ScreenEffect* effect, float origin_x, float origin_y, ShownImage* image)
{
    if (!effect->ready)
    {
        return false;
    }
    // The target is the picture's size: 1:1 with nearest, so nothing blurs it again.
    shown_image_whole(effect->texture_id, &effect->viewport, origin_x, origin_y, image);
    return true;
}

bool screen_effect_request_capture(ScreenEffect* effect)
{
    if (effect->capture_state != ScreenEffectCaptureState::none)
    {
        return false;
    }
    effect->capture_state = ScreenEffectCaptureState::requested;
    return true;
}

void screen_effect_collect(ScreenEffect* effect, const VulkanContext* context)
{
    effect->serial++;
    if (effect->capture_state != ScreenEffectCaptureState::recorded)
    {
        return;
    }
    // renderer_begin_frame waits for the fence of the slot it reuses. At most
    // FRAMES_IN_FLIGHT slots take turns, so once that many frames have been
    // acquired since the copy, its slot's fence has been waited: the GPU is
    // done with it. (Changing the number of frames in flight waits for the
    // whole GPU, which is also fine.)
    if (effect->serial - effect->capture_serial < FRAMES_IN_FLIGHT)
    {
        return;
    }
    uint32_t pitch = effect->capture_width * 4;
    size_t size = static_cast<size_t>(pitch) * effect->capture_height;
    if (!effect->capture_coherent)
    {
        // Cached memory: the CPU's caches may hold old lines of it.
        VkMappedMemoryRange range = {
            .sType  = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
            .memory = effect->capture_memory,
            .offset = 0,
            .size   = VK_WHOLE_SIZE,
        };
        vk_succeeded(vkInvalidateMappedMemoryRanges(context->device, 1, &range), "vkInvalidateMappedMemoryRanges");
    }
    // Copied out so the buffer is free again (one copy, only when a
    // screenshot is taken).
    video_capture_free(&effect->captured);
    effect->captured.pixels = new uint8_t[size];
    std::memcpy(effect->captured.pixels, effect->capture_mapped, size);
    effect->captured.width = effect->capture_width;
    effect->captured.height = effect->capture_height;
    effect->captured.pitch = pitch;
    effect->captured.format = VideoCaptureFormat::bgra8;
    effect->capture_state = ScreenEffectCaptureState::ready;
}

bool screen_effect_capture_take(ScreenEffect* effect, VideoCapture* capture)
{
    if (effect->capture_state != ScreenEffectCaptureState::ready)
    {
        return false;
    }
    *capture = effect->captured;
    effect->captured = VideoCapture{};
    effect->capture_state = ScreenEffectCaptureState::none;
    return true;
}

// Copies the target (just drawn, still COLOR_ATTACHMENT_OPTIMAL) into the
// capture buffer and leaves the target ready to be sampled.
static void record_capture(ScreenEffect* effect, const VulkanContext* context, VkCommandBuffer command_buffer)
{
    VkDeviceSize size = static_cast<VkDeviceSize>(effect->width) * effect->height * 4;
    if (effect->capture_size != size)
    {
        // Only here: when a capture is taken at a new size. No GPU wait: the
        // old buffer isn't used by any frame (a capture is collected before
        // the next one is recorded).
        destroy_capture_buffer(effect, context);
        if (!create_capture_buffer(effect, context, size))
        {
            destroy_capture_buffer(effect, context);
            effect->capture_state = ScreenEffectCaptureState::none;
            return;
        }
    }
    // The pass's color writes -> the copy reads them.
    VkImageMemoryBarrier2 to_copy = {
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask        = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .srcAccessMask       = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        .dstStageMask        = VK_PIPELINE_STAGE_2_COPY_BIT,
        .dstAccessMask       = VK_ACCESS_2_TRANSFER_READ_BIT,
        .oldLayout           = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image               = effect->image,
        .subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    vk_cmd_image_barriers(command_buffer, effect->path, effect->barrier2, &to_copy, 1);

    VkBufferImageCopy region = {
        .bufferOffset      = 0,
        .bufferRowLength   = 0,   // tightly packed
        .bufferImageHeight = 0,
        .imageSubresource  = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageOffset       = { 0, 0, 0 },
        .imageExtent       = { effect->width, effect->height, 1 },
    };
    vkCmdCopyImageToBuffer(command_buffer, effect->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, effect->capture_buffer,
                           1, &region);

    // The target back to being sampled (the GUI's draw this frame), after the
    // copy has read it: an execution dependency plus the layout change.
    VkImageMemoryBarrier2 to_sampled = {
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask        = VK_PIPELINE_STAGE_2_COPY_BIT,
        .srcAccessMask       = VK_ACCESS_2_NONE,
        .dstStageMask        = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        .dstAccessMask       = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        .oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image               = effect->image,
        .subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    vk_cmd_image_barriers(command_buffer, effect->path, effect->barrier2, &to_sampled, 1);

    // The copy's writes made available to the host: the CPU reads the buffer
    // after waiting for this frame's fence (screen_effect_collect).
    VkMemoryBarrier2 to_host = {
        .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask  = VK_PIPELINE_STAGE_2_COPY_BIT,
        .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .dstStageMask  = VK_PIPELINE_STAGE_2_HOST_BIT,
        .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT,
    };
    vk_cmd_memory_barrier(command_buffer, effect->path, effect->barrier2, &to_host);

    effect->capture_width = effect->width;
    effect->capture_height = effect->height;
    effect->capture_serial = effect->serial;
    effect->capture_state = ScreenEffectCaptureState::recorded;
}

void screen_effect_record(ScreenEffect* effect, const VulkanContext* context, VkCommandBuffer command_buffer,
                          uint32_t frame_index, const ScreenEffectSource* source, const ScreenEffectParams* params)
{
    if (!effect->ready || effect->pipeline == VK_NULL_HANDLE || source->view == VK_NULL_HANDLE ||
        frame_index >= FRAMES_IN_FLIGHT)
    {
        return;
    }

    // This slot's set was last used by the frame that had this slot before;
    // renderer_begin_frame has waited for its fence, so it can be rewritten.
    VkDescriptorImageInfo image_info = {
        .sampler     = source->linear ? effect->sampler_linear : effect->sampler_nearest,
        .imageView   = source->view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };
    VkWriteDescriptorSet write = {
        .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet          = effect->sets[frame_index],
        .dstBinding      = 0,
        .descriptorCount = 1,
        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo      = &image_info,
    };
    vkUpdateDescriptorSets(context->device, 1, &write, 0, nullptr);

    // The target becomes a color attachment. Earlier frames may still be
    // sampling it (the GUI) or copying it (a capture): reads only, so an
    // execution dependency is enough. Every pixel is redrawn: old contents
    // discarded (UNDEFINED).
    VkImageMemoryBarrier2 to_attachment = {
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask        = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT,
        .srcAccessMask       = VK_ACCESS_2_NONE,
        .dstStageMask        = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstAccessMask       = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        .oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout           = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image               = effect->image,
        .subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    vk_cmd_image_barriers(command_buffer, effect->path, effect->barrier2, &to_attachment, 1);

    VkRect2D area = { { 0, 0 }, { effect->width, effect->height } };
    if (effect->path == VulkanPath::vulkan_1_3)
    {
        VkRenderingAttachmentInfo attachment = {
            .sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView   = effect->view,
            .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .loadOp      = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .storeOp     = VK_ATTACHMENT_STORE_OP_STORE,
        };
        VkRenderingInfo rendering = {
            .sType                = VK_STRUCTURE_TYPE_RENDERING_INFO,
            .renderArea           = area,
            .layerCount           = 1,
            .colorAttachmentCount = 1,
            .pColorAttachments    = &attachment,
        };
        effect->begin_rendering(command_buffer, &rendering);
    }
    else
    {
        VkRenderPassBeginInfo begin = {
            .sType       = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass  = effect->render_pass,
            .framebuffer = effect->framebuffer,
            .renderArea  = area,
        };
        vkCmdBeginRenderPass(command_buffer, &begin, VK_SUBPASS_CONTENTS_INLINE);
    }

    VkViewport viewport = { 0.0f, 0.0f, static_cast<float>(effect->width), static_cast<float>(effect->height), 0.0f, 1.0f };
    vkCmdSetViewport(command_buffer, 0, 1, &viewport);
    vkCmdSetScissor(command_buffer, 0, 1, &area);
    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, effect->pipeline);
    vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, effect->pipeline_layout, 0, 1,
                            &effect->sets[frame_index], 0, nullptr);
    // A capture shows the picture itself: the knobs at rest.
    bool capture = effect->capture_state == ScreenEffectCaptureState::requested;
    ScreenEffectParams rest = {};
    const ScreenEffectParams* knobs = capture ? &rest : params;
    ScreenEffectPush push = {
        .source_rect = { source->rect[0], source->rect[1], source->rect[2], source->rect[3] },
        .knobs       = { knobs->zoom < 1.0f ? 1.0f : knobs->zoom, knobs->desaturate, knobs->flash, knobs->bars },
        .flash_color = { knobs->flash_red, knobs->flash_green, knobs->flash_blue, 1.0f },
    };
    vkCmdPushConstants(command_buffer, effect->pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    vkCmdDraw(command_buffer, 3, 1, 0, 0);

    if (effect->path == VulkanPath::vulkan_1_3)
    {
        effect->end_rendering(command_buffer);
    }
    else
    {
        vkCmdEndRenderPass(command_buffer);
    }

    if (capture)
    {
        record_capture(effect, context, command_buffer);
        if (effect->capture_state == ScreenEffectCaptureState::recorded)
        {
            effect->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            return;
        }
    }
    // The pass's writes -> the GUI samples the target this frame.
    VkImageMemoryBarrier2 to_sampled = {
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask        = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .srcAccessMask       = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        .dstStageMask        = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        .dstAccessMask       = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        .oldLayout           = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image               = effect->image,
        .subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    vk_cmd_image_barriers(command_buffer, effect->path, effect->barrier2, &to_sampled, 1);
    effect->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}
