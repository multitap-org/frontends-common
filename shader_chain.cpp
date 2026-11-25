#include "shader_chain.h"

#include <SDL3/SDL_log.h>


#include "debug_ui.h"
#include "host_fs.h"
#include "renderer.h"
#include "vk_barrier.h"
#include "vk_context.h"

// librashader.so / librashader.dll / librashader.dylib: set by CMake from the
// platform's shared library suffix.
constexpr const char* SHADER_LIBRARY_FILE = MULTITAP_LIBRASHADER_FILE;

// Our images are always this format: B8G8R8A8_UNORM must support sampling,
// linear filtering, blit destination and color attachment on every Vulkan
// device (the spec's mandatory format table).
constexpr VkFormat SHADER_CHAIN_FORMAT = VK_FORMAT_B8G8R8A8_UNORM;

// ---------------------------------------------------------------------------
// The library
// ---------------------------------------------------------------------------

static GenericFunction find_function(const ShaderLibrary* library, const char* name, bool* all_found)
{
    GenericFunction function = dynamic_library_symbol(&library->library, name);
    if (function == nullptr)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_GPU, "librashader: %s is missing", name);
        *all_found = false;
    }
    return function;
}

static bool open_and_check(ShaderLibrary* library, const char* path)
{
    if (!dynamic_library_open(&library->library, path))
    {
        return false;
    }
    bool all_found = true;
    PFN_libra_instance_abi_version abi_version = reinterpret_cast<PFN_libra_instance_abi_version>(
        find_function(library, "libra_instance_abi_version", &all_found));
    PFN_libra_instance_api_version api_version = reinterpret_cast<PFN_libra_instance_api_version>(
        find_function(library, "libra_instance_api_version", &all_found));
    if (!all_found)
    {
        dynamic_library_close(&library->library);
        return false;
    }
    // The ABI is the layout of the structs we pass; the API version only
    // grows (new fields are read only when we pass a higher version), so a
    // newer one is fine.
    LIBRASHADER_ABI_VERSION abi = abi_version();
    LIBRASHADER_API_VERSION api = api_version();
    if (abi != SHADER_LIBRARY_ABI_VERSION || api < SHADER_LIBRARY_API_VERSION)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_GPU, "librashader at %s: ABI %u API %u, needs ABI %u API %u or newer", path,
                    static_cast<unsigned>(abi), static_cast<unsigned>(api),
                    static_cast<unsigned>(SHADER_LIBRARY_ABI_VERSION), static_cast<unsigned>(SHADER_LIBRARY_API_VERSION));
        dynamic_library_close(&library->library);
        return false;
    }

    library->error_write = reinterpret_cast<PFN_libra_error_write>(find_function(library, "libra_error_write", &all_found));
    library->error_free_string =
        reinterpret_cast<PFN_libra_error_free_string>(find_function(library, "libra_error_free_string", &all_found));
    library->error_free = reinterpret_cast<PFN_libra_error_free>(find_function(library, "libra_error_free", &all_found));
    library->preset_create =
        reinterpret_cast<PFN_libra_preset_create>(find_function(library, "libra_preset_create", &all_found));
    library->preset_free = reinterpret_cast<PFN_libra_preset_free>(find_function(library, "libra_preset_free", &all_found));
    library->vk_filter_chain_create = reinterpret_cast<PFN_libra_vk_filter_chain_create>(
        find_function(library, "libra_vk_filter_chain_create", &all_found));
    library->vk_filter_chain_frame = reinterpret_cast<PFN_libra_vk_filter_chain_frame>(
        find_function(library, "libra_vk_filter_chain_frame", &all_found));
    library->vk_filter_chain_set_param = reinterpret_cast<PFN_libra_vk_filter_chain_set_param>(
        find_function(library, "libra_vk_filter_chain_set_param", &all_found));
    library->vk_filter_chain_free = reinterpret_cast<PFN_libra_vk_filter_chain_free>(
        find_function(library, "libra_vk_filter_chain_free", &all_found));
    if (!all_found)
    {
        // A build without the Vulkan runtime, most likely.
        SDL_LogWarn(SDL_LOG_CATEGORY_GPU, "librashader at %s lacks functions this program needs", path);
        dynamic_library_close(&library->library);
        return false;
    }
    library->api_version = api;
    return true;
}

bool shader_library_load(ShaderLibrary* library, const std::vector<std::string>& folders)
{
    shader_library_unload(library);
    std::vector<std::string> candidates;
    for (size_t i = 0; i < folders.size(); i++)
    {
        if (folders[i].empty())
        {
            continue;
        }
        std::string folder = folders[i];
        if (folder.back() != '/' && folder.back() != '\\')
        {
            folder += "/";
        }
        // Only files that are there: a missing one would log a dlopen error.
        std::string candidate = folder + SHADER_LIBRARY_FILE;
        if (host_fs_exists(candidate.c_str()))
        {
            candidates.push_back(candidate);
        }
    }
    candidates.push_back(SHADER_LIBRARY_FILE);   // the system's search (LD_LIBRARY_PATH, /usr/lib, PATH...)

    for (size_t i = 0; i < candidates.size(); i++)
    {
        if (open_and_check(library, candidates[i].c_str()))
        {
            library->loaded = true;
            library->path = candidates[i];
            SDL_LogInfo(SDL_LOG_CATEGORY_GPU, "librashader: %s (API %u)", library->path.c_str(),
                        static_cast<unsigned>(library->api_version));
            return true;
        }
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_GPU, "librashader not found: shader presets are off");
    return false;
}

void shader_library_unload(ShaderLibrary* library)
{
    dynamic_library_close(&library->library);
    *library = ShaderLibrary{};
}

static void log_error(const ShaderLibrary* library, libra_error_t error, const char* what)
{
    char* message = nullptr;
    if (library->error_write(error, &message) == 0 && message != nullptr)
    {
        SDL_LogError(SDL_LOG_CATEGORY_GPU, "librashader: %s: %s", what, message);
        library->error_free_string(&message);
    }
    else
    {
        SDL_LogError(SDL_LOG_CATEGORY_GPU, "librashader: %s failed", what);
    }
    library->error_free(&error);
}

// ---------------------------------------------------------------------------
// Images
// ---------------------------------------------------------------------------

static void destroy_image(ShaderChainImage* image, const VulkanContext* context)
{
    debug_ui_remove_texture(image->texture_id);
    if (image->view != VK_NULL_HANDLE)
    {
        vkDestroyImageView(context->device, image->view, nullptr);
    }
    if (image->image != VK_NULL_HANDLE)
    {
        vkDestroyImage(context->device, image->image, nullptr);
    }
    if (image->image_memory != VK_NULL_HANDLE)
    {
        vkFreeMemory(context->device, image->image_memory, nullptr);
    }
    *image = ShaderChainImage{};
}

// `for_imgui`: also a view (alpha read as 1) and an ImGui texture id.
static bool create_image(ShaderChainImage* image, const VulkanContext* context, uint32_t width, uint32_t height,
                         VkImageUsageFlags usage, bool for_imgui, const char* name)
{
    VkImageCreateInfo image_info = {
        .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType     = VK_IMAGE_TYPE_2D,
        .format        = SHADER_CHAIN_FORMAT,
        .extent        = { width, height, 1 },
        .mipLevels     = 1,
        .arrayLayers   = 1,
        .samples       = VK_SAMPLE_COUNT_1_BIT,
        .tiling        = VK_IMAGE_TILING_OPTIMAL,
        .usage         = usage,
        .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (!vk_succeeded(vkCreateImage(context->device, &image_info, nullptr, &image->image), name))
    {
        return false;
    }
    VkMemoryRequirements requirements = {};
    vkGetImageMemoryRequirements(context->device, image->image, &requirements);
    uint32_t type_index = 0;
    if (!vulkan_find_memory_type(context, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type_index))
    {
        SDL_LogError(SDL_LOG_CATEGORY_GPU, "No device-local memory for the %s", name);
        return false;
    }
    VkMemoryAllocateInfo allocate_info = {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize  = requirements.size,
        .memoryTypeIndex = type_index,
    };
    if (!vk_succeeded(vkAllocateMemory(context->device, &allocate_info, nullptr, &image->image_memory), name) ||
        !vk_succeeded(vkBindImageMemory(context->device, image->image, image->image_memory, 0), name))
    {
        return false;
    }
    image->width = width;
    image->height = height;
    image->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!for_imgui)
    {
        return true;
    }

    VkImageViewCreateInfo view_info = {
        .sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image    = image->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format   = SHADER_CHAIN_FORMAT,
        // Alpha read as 1: ImGui draws with blending on, and a shader's alpha
        // (or a core's unused X bits carried through) must never make the
        // picture see-through.
        .components = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                        VK_COMPONENT_SWIZZLE_ONE },
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    if (!vk_succeeded(vkCreateImageView(context->device, &view_info, nullptr, &image->view), name))
    {
        return false;
    }
    image->texture_id = debug_ui_add_texture(image->view);
    return true;
}

// ---------------------------------------------------------------------------
// The stages
// ---------------------------------------------------------------------------

bool shader_chain_active(const ShaderChain* chain)
{
    for (uint32_t i = 0; i < SHADER_CHAIN_STAGES; i++)
    {
        if (chain->stages[i].chain != nullptr)
        {
            return true;
        }
    }
    return false;
}

void shader_chain_clear(ShaderChain* chain, const ShaderLibrary* library, const VulkanContext* context, uint32_t stage)
{
    ShaderStage* target = &chain->stages[stage];
    if (target->chain != nullptr)
    {
        vkDeviceWaitIdle(context->device);   // earlier frames may still be running its passes
        if (library->loaded)
        {
            library->vk_filter_chain_free(&target->chain);
        }
    }
    *target = ShaderStage{};
}

void shader_chain_destroy(ShaderChain* chain, const ShaderLibrary* library, const VulkanContext* context)
{
    if (shader_chain_active(chain) || chain->input.image != VK_NULL_HANDLE || chain->middle.image != VK_NULL_HANDLE ||
        chain->output.image != VK_NULL_HANDLE)
    {
        vkDeviceWaitIdle(context->device);   // earlier frames may still be using them
    }
    for (uint32_t i = 0; i < SHADER_CHAIN_STAGES; i++)
    {
        if (chain->stages[i].chain != nullptr && library->loaded)
        {
            library->vk_filter_chain_free(&chain->stages[i].chain);
        }
    }
    destroy_image(&chain->input, context);
    destroy_image(&chain->middle, context);
    destroy_image(&chain->output, context);
    *chain = ShaderChain{};
}

bool shader_chain_load(ShaderChain* chain, const ShaderLibrary* library, const VulkanContext* context, uint32_t stage,
                       const char* preset_path)
{
    shader_chain_clear(chain, library, context, stage);
    if (!library->loaded)
    {
        return false;
    }
    ShaderStage* target = &chain->stages[stage];
    target->failed_path = preset_path;   // until it works

    libra_shader_preset_t preset = nullptr;
    libra_error_t error = library->preset_create(preset_path, &preset);
    if (error != nullptr)
    {
        log_error(library, error, preset_path);
        return false;
    }

    libra_device_vk_t device = {
        .physical_device = context->physical_device,
        .instance        = context->instance,
        .device          = context->device,
        // Ours. Creation submits uploads (lookup textures) on it; we call
        // this between frames on the main thread, so no submit of ours races it.
        .queue           = context->queue,
        .entry           = vkGetInstanceProcAddr,
    };
    filter_chain_vk_opt_t options = {};
    options.version = SHADER_LIBRARY_API_VERSION;
    // Its per-frame resources are reused this many frames later: at least
    // as many as we can have in flight.
    options.frames_in_flight = FRAMES_IN_FLIGHT;
    // Dynamic rendering on the 1.3 path; on the 1.1 one (older phones: the
    // SHIELD's Tegra X1) librashader makes its own render passes.
    options.use_dynamic_rendering = context->path == VulkanPath::vulkan_1_3;

    // The call uses up the preset when it succeeds (sets it to null). Free it
    // only when it's still there: libra_preset_free on a used-up preset
    // panics inside librashader (0.12.1, presets.rs: unwrap on None).
    libra_vk_filter_chain_t created = nullptr;
    error = library->vk_filter_chain_create(&preset, device, &options, &created);
    if (preset != nullptr)
    {
        library->preset_free(&preset);
    }
    if (error != nullptr)
    {
        log_error(library, error, preset_path);
        return false;
    }
    target->chain = created;
    target->preset_path = preset_path;
    target->failed_path.clear();
    SDL_LogInfo(SDL_LOG_CATEGORY_GPU, "Shader stage %u: %s", static_cast<unsigned>(stage), preset_path);
    return true;
}

bool shader_chain_set_param(ShaderChain* chain, const ShaderLibrary* library, uint32_t stage, const char* name,
                            float value)
{
    ShaderStage* target = &chain->stages[stage];
    if (target->chain == nullptr || !library->loaded)
    {
        return false;
    }
    libra_error_t error = library->vk_filter_chain_set_param(&target->chain, name, value);
    if (error != nullptr)
    {
        log_error(library, error, name);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Per frame
// ---------------------------------------------------------------------------

// (Re)creates `image` when its size isn't `width` x `height`. Waits for the
// GPU only then (a resize, a crop change, a core changing resolution).
static bool ensure_image(ShaderChainImage* image, const VulkanContext* context, uint32_t width, uint32_t height,
                         VkImageUsageFlags usage, bool for_imgui, const char* name)
{
    if (image->image != VK_NULL_HANDLE && image->width == width && image->height == height)
    {
        return true;
    }
    vkDeviceWaitIdle(context->device);
    destroy_image(image, context);
    if (!create_image(image, context, width, height, usage, for_imgui, name))
    {
        destroy_image(image, context);
        return false;
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_GPU, "%s: %ux%u", name, width, height);
    return true;
}

bool shader_chain_prepare(ShaderChain* chain, const VulkanContext* context, const StreamingTexture* source,
                          const ViewportResult* viewport)
{
    chain->ready = false;
    chain->path = context->path;
    chain->barrier2 = context->cmd_pipeline_barrier2;
    if (!shader_chain_active(chain) || !source->has_content || viewport->width == 0 || viewport->height == 0 ||
        viewport->source_width == 0 || viewport->source_height == 0)
    {
        return false;
    }
    // The output is the picture's whole rectangle, even a part off the
    // window (a picture bigger than the window): within the GPU's limit, or
    // the picture is drawn without the chain.
    uint32_t limit = context->physical_device_properties.limits.maxImageDimension2D;
    if (viewport->width > limit || viewport->height > limit)
    {
        return false;
    }

    // The source's format (a core's native one) must blit into ours.
    VkFormatProperties source_properties = {};
    vkGetPhysicalDeviceFormatProperties(context->physical_device, source->format, &source_properties);
    if ((source_properties.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) == 0)
    {
        return false;
    }

    // TRANSFER_SRC on the images a stage reads: librashader copies its input
    // for presets that read earlier frames (OriginalHistory) and for mipmaps.
    VkImageUsageFlags input_usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                    VK_IMAGE_USAGE_SAMPLED_BIT;
    if (!ensure_image(&chain->input, context, viewport->source_width, viewport->source_height, input_usage, false,
                      "Shader chain input"))
    {
        return false;
    }
    bool both = chain->stages[0].chain != nullptr && chain->stages[1].chain != nullptr;
    if (both)
    {
        VkImageUsageFlags middle_usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                         VK_IMAGE_USAGE_SAMPLED_BIT;
        // An ImGui texture too: a frontend may show it (piccolo: IMAGE
        // SETTINGS' preview, with pass 0's effect).
        if (!ensure_image(&chain->middle, context, viewport->source_width, viewport->source_height, middle_usage,
                          true, "Shader chain middle"))
        {
            return false;
        }
    }
    VkImageUsageFlags output_usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (!ensure_image(&chain->output, context, viewport->width, viewport->height, output_usage, true,
                      "Shader chain output"))
    {
        return false;
    }
    chain->viewport = *viewport;
    chain->ready = true;
    return true;
}

bool shader_chain_shown_image(const ShaderChain* chain, float origin_x, float origin_y, ShownImage* image)
{
    if (!chain->ready)
    {
        return false;
    }
    // The output is already the picture's size: drawn 1:1 with nearest, so
    // nothing blurs it again.
    shown_image_whole(chain->output.texture_id, &chain->viewport, origin_x, origin_y, image);
    return true;
}

// One image into the layout a stage writes: COLOR_ATTACHMENT_OPTIMAL
// (librashader's requirement for its output). Earlier frames may still be
// sampling it (ImGui's draw of the output, stage 1 reading the middle) or
// copying it (librashader's history of the middle): reads only, so an
// execution dependency is enough. It's fully redrawn: old contents discarded.
static VkImageMemoryBarrier2 to_render_target(ShaderChainImage* image)
{
    bool first_use = image->layout == VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageMemoryBarrier2 barrier = {
        .sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask  = first_use ? VK_PIPELINE_STAGE_2_NONE
                                   : (VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT),
        .srcAccessMask = VK_ACCESS_2_NONE,
        .dstStageMask  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT,
        .oldLayout     = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image         = image->image,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    image->layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    return barrier;
}

// A stage's written image back to sampled, for whoever reads it next: stage 1
// (fragment shaders; librashader may also copy it: transfer) or ImGui.
static void render_target_to_sampled(const ShaderChain* chain, VkCommandBuffer command_buffer, ShaderChainImage* image)
{
    VkImageMemoryBarrier2 barrier = {
        .sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        .dstStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT,
        .oldLayout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image         = image->image,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    vk_cmd_image_barriers(command_buffer, chain->path, chain->barrier2, &barrier, 1);
    image->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

// Records one stage's passes from `from` (sampled) into `to` (render target).
static void run_stage(ShaderStage* stage, const ShaderLibrary* library, VkCommandBuffer command_buffer,
                      const ShaderChainImage* from, const ShaderChainImage* to)
{
    libra_image_vk_t input = {
        .handle = from->image,
        .format = SHADER_CHAIN_FORMAT,
        .width  = from->width,
        .height = from->height,
    };
    libra_image_vk_t output = {
        .handle = to->image,
        .format = SHADER_CHAIN_FORMAT,
        .width  = to->width,
        .height = to->height,
    };
    frame_vk_opt_t frame_options = {};
    frame_options.version = SHADER_LIBRARY_API_VERSION;
    frame_options.frame_direction = 1;
    frame_options.total_subframes = 1;
    frame_options.current_subframe = 1;
    libra_error_t error = library->vk_filter_chain_frame(&stage->chain, command_buffer, stage->frame_count, input,
                                                         output, nullptr, nullptr, &frame_options);
    if (error != nullptr)
    {
        // Logged and dropped: the image keeps old contents for this frame.
        log_error(library, error, "frame");
    }
    stage->frame_count++;
}

void shader_chain_record(ShaderChain* chain, const ShaderLibrary* library, VkCommandBuffer command_buffer,
                         StreamingTexture* source)
{
    if (!chain->ready || !shader_chain_active(chain) || !source->has_content ||
        source->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
    {
        return;
    }
    const ViewportResult* viewport = &chain->viewport;
    ShaderStage* first = chain->stages[0].chain != nullptr ? &chain->stages[0] : &chain->stages[1];
    ShaderStage* second = chain->stages[0].chain != nullptr && chain->stages[1].chain != nullptr ? &chain->stages[1]
                                                                                                 : nullptr;

    // 1. Copy the shown part of the source into the input.
    // Source: its last writer was this or an earlier frame's upload copy; its
    // last readers, earlier frames' fragment shaders.
    // Input: earlier frames' passes may still be sampling it (fragment) or
    // copying it (librashader's history / mipmaps: transfer). Reads only, so
    // an execution dependency is enough; it's fully overwritten, so its old
    // contents are discarded (UNDEFINED).
    bool input_first_use = chain->input.layout == VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageMemoryBarrier2 before_copy[2] = {
        {
            .sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask  = VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStageMask  = VK_PIPELINE_STAGE_2_BLIT_BIT,
            .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
            .oldLayout     = source->layout,
            .newLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image         = source->image,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        },
        {
            .sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask  = input_first_use ? VK_PIPELINE_STAGE_2_NONE
                                             : (VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT),
            .srcAccessMask = VK_ACCESS_2_NONE,
            .dstStageMask  = VK_PIPELINE_STAGE_2_BLIT_BIT,
            .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .oldLayout     = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image         = chain->input.image,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        },
    };
    vk_cmd_image_barriers(command_buffer, chain->path, chain->barrier2, before_copy, 2);

    // A blit, not a copy: same size (so nearest is exact), but it converts a
    // core's native format (R5G6B5...) into ours.
    VkImageBlit region = {
        .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .srcOffsets = {
            { static_cast<int32_t>(viewport->source_x), static_cast<int32_t>(viewport->source_y), 0 },
            { static_cast<int32_t>(viewport->source_x + viewport->source_width),
              static_cast<int32_t>(viewport->source_y + viewport->source_height), 1 },
        },
        .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .dstOffsets = {
            { 0, 0, 0 },
            { static_cast<int32_t>(chain->input.width), static_cast<int32_t>(chain->input.height), 1 },
        },
    };
    vkCmdBlitImage(command_buffer, source->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, chain->input.image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_NEAREST);

    // 2. Source back to sampled (the next upload waits on the fragment stage,
    // which this chains after the blit). Input readable by the first stage
    // (its passes sample it; it may copy it). The first stage's target
    // writable.
    ShaderChainImage* first_target = second != nullptr ? &chain->middle : &chain->output;
    VkImageMemoryBarrier2 before_first[3] = {
        {
            .sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask  = VK_PIPELINE_STAGE_2_BLIT_BIT,
            .srcAccessMask = VK_ACCESS_2_NONE,
            .dstStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            .oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image         = source->image,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        },
        {
            .sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask  = VK_PIPELINE_STAGE_2_BLIT_BIT,
            .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
            .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT,
            .oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image         = chain->input.image,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        },
        to_render_target(first_target),
    };
    vk_cmd_image_barriers(command_buffer, chain->path, chain->barrier2, before_first, 3);
    source->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    chain->input.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    // 3. The stages. librashader orders its own intermediate images; it
    // leaves its target in COLOR_ATTACHMENT_OPTIMAL with no barrier after.
    run_stage(first, library, command_buffer, &chain->input, first_target);
    render_target_to_sampled(chain, command_buffer, first_target);
    if (second != nullptr)
    {
        VkImageMemoryBarrier2 before_second = to_render_target(&chain->output);
        vk_cmd_image_barriers(command_buffer, chain->path, chain->barrier2, &before_second, 1);
        run_stage(second, library, command_buffer, &chain->middle, &chain->output);
        render_target_to_sampled(chain, command_buffer, &chain->output);
    }
}
