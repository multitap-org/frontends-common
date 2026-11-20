#include "video_output.h"

#include "app_shell.h"
#include "video_texture.h"

bool video_output_load_stage(VideoOutput* output, const ShaderLibrary* library, AppShell* shell, uint32_t stage,
                             const char* preset_path)
{
    if (preset_path[0] == '\0')
    {
        shader_chain_clear(&output->chain, library, &shell->vulkan, stage);
        return true;
    }
    if (library == nullptr)
    {
        return false;
    }
    return shader_chain_load(&output->chain, library, &shell->vulkan, stage, preset_path);
}

const std::string& video_output_stage_preset(const VideoOutput* output, uint32_t stage)
{
    return output->chain.stages[stage].preset_path;
}

const std::string& video_output_stage_failed_preset(const VideoOutput* output, uint32_t stage)
{
    return output->chain.stages[stage].failed_path;
}

bool video_output_stage_loaded(const VideoOutput* output, uint32_t stage)
{
    return output->chain.stages[stage].chain != nullptr;
}

bool video_output_shaded_available(const VideoOutput* output)
{
    return shader_chain_active(&output->chain);
}

bool video_output_set_param(VideoOutput* output, const ShaderLibrary* library, uint32_t stage, const char* name,
                            float value)
{
    return shader_chain_set_param(&output->chain, library, stage, name, value);
}

bool video_output_prepare(VideoOutput* output, AppShell* shell, const VideoTexture* source,
                          const ViewportResult* viewport)
{
    output->shaded = shader_chain_prepare(&output->chain, &shell->vulkan, &source->vulkan, viewport);
    if (output->shaded)
    {
        output->picture.ready = false;   // the chain draws it; the picture records nothing
        return true;
    }
    return picture_prepare(&output->picture, &shell->vulkan, &source->vulkan, viewport);
}

void video_output_skip(VideoOutput* output)
{
    output->shaded = false;
    output->chain.ready = false;
    output->picture.ready = false;
}

void video_output_draw(const VideoOutput* output, ImDrawList* draw_list, const VideoTexture* source, float origin_x,
                       float origin_y)
{
    if (output->shaded)
    {
        shader_chain_draw(&output->chain, draw_list, origin_x, origin_y);
    }
    else
    {
        picture_draw(&output->picture, draw_list, &source->vulkan, source->imgui_id, origin_x, origin_y);
    }
}

void video_output_record(VideoOutput* output, const ShaderLibrary* library, AppShell* shell, VideoTexture* source)
{
    // Each records nothing unless its prepare said this frame is its.
    VkCommandBuffer command_buffer = renderer_command_buffer(&shell->renderer);
    picture_record(&output->picture, command_buffer, &source->vulkan);
    shader_chain_record(&output->chain, library, command_buffer, &source->vulkan);
}

bool video_output_stage0_result(const VideoOutput* output, uint64_t* imgui_id, uint32_t* width, uint32_t* height)
{
    // Only once the chain has written it: a new image is still UNDEFINED
    // while the UI is built.
    const ShaderChain* chain = &output->chain;
    if (!chain->ready || chain->stages[0].chain == nullptr || chain->middle.texture_id == 0 ||
        chain->middle.layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
    {
        return false;
    }
    *imgui_id = chain->middle.texture_id;
    *width = chain->middle.width;
    *height = chain->middle.height;
    return true;
}

void video_output_destroy(VideoOutput* output, const ShaderLibrary* library, AppShell* shell)
{
    picture_destroy(&output->picture, &shell->vulkan);
    shader_chain_destroy(&output->chain, library, &shell->vulkan);
    output->shaded = false;
}
