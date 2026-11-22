#include "video_output.h"

#include <SDL3/SDL_log.h>

#include "app_shell.h"
#include "video_texture.h"

bool video_output_load_stage(VideoOutput* output, const ShaderLibrary* library, AppShell* shell, uint32_t stage,
                             const char* preset_path)
{
    if (shell->api == GpuApi::gl)
    {
        output->on_gl = true;
        return gl_shader_chain_load(&output->gl_chain, &shell->gl, stage, preset_path);
    }
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
    return output->on_gl ? output->gl_chain.stages[stage].preset_path : output->chain.stages[stage].preset_path;
}

const std::string& video_output_stage_failed_preset(const VideoOutput* output, uint32_t stage)
{
    return output->on_gl ? output->gl_chain.stages[stage].failed_path : output->chain.stages[stage].failed_path;
}

bool video_output_stage_loaded(const VideoOutput* output, uint32_t stage)
{
    return output->on_gl ? output->gl_chain.stages[stage].pass_count > 0 : output->chain.stages[stage].chain != nullptr;
}

bool video_output_shaded_available(const VideoOutput* output)
{
    return output->on_gl ? gl_shader_chain_active(&output->gl_chain) : shader_chain_active(&output->chain);
}

bool video_output_set_param(VideoOutput* output, const ShaderLibrary* library, AppShell* shell, uint32_t stage,
                            const char* name, float value)
{
    if (output->on_gl)
    {
        static_cast<void>(library);
        return gl_shader_chain_set_param(&output->gl_chain, &shell->gl, stage, name, value);
    }
    return shader_chain_set_param(&output->chain, library, stage, name, value);
}

void video_output_set_effect(VideoOutput* output, const ScreenEffectParams* params)
{
    output->effect = *params;
}

bool video_output_request_capture(VideoOutput* output)
{
    if (output->on_gl)
    {
        return gl_screen_effect_request_capture(&output->gl_effect);
    }
    return screen_effect_request_capture(&output->effect_pass);
}

bool video_output_take_capture(VideoOutput* output, VideoCapture* capture)
{
    if (output->on_gl)
    {
        return gl_screen_effect_capture_take(&output->gl_effect, capture);
    }
    return screen_effect_capture_take(&output->effect_pass, capture);
}

// After the picture's own decision: the screen effect pass this frame when
// there's an effect or a capture to take (and the pass can run).
static void prepare_effect(VideoOutput* output, AppShell* shell, const ViewportResult* viewport)
{
    output->effect_on = false;
    ScreenEffectCaptureState capture_state =
        output->on_gl ? output->gl_effect.capture_state : output->effect_pass.capture_state;
    bool capture = capture_state == ScreenEffectCaptureState::requested;
    if (screen_effect_params_at_rest(&output->effect) && !capture)
    {
        output->effect_pass.ready = false;
        output->gl_effect.ready = false;
        return;
    }
    if (output->on_gl)
    {
        output->effect_on = gl_screen_effect_prepare(&output->gl_effect, &shell->gl, viewport);
        return;
    }
    output->effect_on = screen_effect_prepare(&output->effect_pass, &shell->vulkan, viewport);
}

static bool prepare_picture(VideoOutput* output, AppShell* shell, const VideoTexture* source,
                            const ViewportResult* viewport)
{
    if (shell->api == GpuApi::gl)
    {
        output->on_gl = true;
        output->shaded = gl_shader_chain_prepare(&output->gl_chain, &shell->gl, &source->gl, source->has_content,
                                                 viewport);
        if (output->shaded)
        {
            output->gl_picture.ready = false;   // the chain draws it
            return true;
        }
        return gl_picture_prepare(&output->gl_picture, &shell->gl, &source->gl, source->has_content, viewport);
    }
    output->shaded = shader_chain_prepare(&output->chain, &shell->vulkan, &source->vulkan, viewport);
    if (output->shaded)
    {
        output->picture.ready = false;   // the chain draws it; the picture records nothing
        return true;
    }
    return picture_prepare(&output->picture, &shell->vulkan, &source->vulkan, viewport);
}

bool video_output_prepare(VideoOutput* output, AppShell* shell, const VideoTexture* source,
                          const ViewportResult* viewport)
{
    if (!prepare_picture(output, shell, source, viewport))
    {
        output->effect_on = false;
        output->effect_pass.ready = false;
        output->gl_effect.ready = false;
        return false;
    }
    prepare_effect(output, shell, viewport);
    return true;
}

void video_output_skip(VideoOutput* output)
{
    output->shaded = false;
    output->effect_on = false;
    output->effect_pass.ready = false;
    output->gl_effect.ready = false;
    output->chain.ready = false;
    output->picture.ready = false;
    output->gl_picture.ready = false;
    output->gl_chain.ready = false;
}

void video_output_draw(const VideoOutput* output, ImDrawList* draw_list, const VideoTexture* source, float origin_x,
                       float origin_y)
{
    if (output->on_gl)
    {
        if (output->effect_on)
        {
            gl_screen_effect_draw(&output->gl_effect, draw_list, origin_x, origin_y);
        }
        else if (output->shaded)
        {
            gl_shader_chain_draw(&output->gl_chain, draw_list, origin_x, origin_y);
        }
        else
        {
            gl_picture_draw(&output->gl_picture, draw_list, &source->gl, origin_x, origin_y);
        }
    }
    else if (output->effect_on)
    {
        screen_effect_draw(&output->effect_pass, draw_list, origin_x, origin_y);
    }
    else if (output->shaded)
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
    if (shell->api == GpuApi::gl)
    {
        // Each does nothing unless its prepare said this frame is its.
        gl_shader_chain_record(&output->gl_chain, &shell->gl, &source->gl);
        gl_picture_record(&output->gl_picture, &shell->gl, &source->gl);
        gl_screen_effect_collect(&output->gl_effect, &shell->gl);
        if (!output->effect_on)
        {
            return;
        }
        // What the picture would have drawn, sampled the same way.
        GlScreenEffectSource gl_source = {};
        if (output->shaded)
        {
            gl_source.texture = gl_shader_chain_output_texture(&output->gl_chain);   // the picture's size
        }
        else if (output->gl_picture.use_copy && output->gl_picture.texture != 0)
        {
            gl_source.texture = output->gl_picture.texture;
            gl_source.linear = true;
        }
        else
        {
            const ViewportResult* viewport = &output->gl_effect.viewport;
            float capacity_width = static_cast<float>(source->gl.capacity_width);
            float capacity_height = static_cast<float>(source->gl.capacity_height);
            gl_source.texture = source->gl.shown;
            gl_source.linear = viewport->filter != ViewportFilter::nearest;
            gl_source.rect[0] = static_cast<float>(viewport->source_x) / capacity_width;
            gl_source.rect[1] = static_cast<float>(viewport->source_y) / capacity_height;
            gl_source.rect[2] = static_cast<float>(viewport->source_width) / capacity_width;
            gl_source.rect[3] = static_cast<float>(viewport->source_height) / capacity_height;
        }
        gl_screen_effect_record(&output->gl_effect, &shell->gl, &gl_source, &output->effect);
        return;
    }
    // Each records nothing unless its prepare said this frame is its.
    VkCommandBuffer command_buffer = renderer_command_buffer(&shell->renderer);
    picture_record(&output->picture, command_buffer, &source->vulkan);
    shader_chain_record(&output->chain, library, command_buffer, &source->vulkan);
    screen_effect_collect(&output->effect_pass, &shell->vulkan);
    if (!output->effect_on)
    {
        return;
    }
    // What the picture would have drawn, sampled the same way. Each one's
    // last barrier left it readable by fragment shaders.
    ScreenEffectSource effect_source = {};
    if (output->shaded && output->chain.output.view != VK_NULL_HANDLE)
    {
        effect_source.view = output->chain.output.view;   // already the picture's size
    }
    else if (!output->shaded && output->picture.use_copy && output->picture.view != VK_NULL_HANDLE)
    {
        effect_source.view = output->picture.view;
        effect_source.linear = true;
    }
    else
    {
        // The shown part of the core's (bigger) texture.
        const ViewportResult* viewport = &output->effect_pass.viewport;
        float capacity_width = static_cast<float>(source->vulkan.capacity_width);
        float capacity_height = static_cast<float>(source->vulkan.capacity_height);
        effect_source.view = source->vulkan.view;
        effect_source.linear = viewport->filter != ViewportFilter::nearest;
        effect_source.rect[0] = static_cast<float>(viewport->source_x) / capacity_width;
        effect_source.rect[1] = static_cast<float>(viewport->source_y) / capacity_height;
        effect_source.rect[2] = static_cast<float>(viewport->source_width) / capacity_width;
        effect_source.rect[3] = static_cast<float>(viewport->source_height) / capacity_height;
    }
    screen_effect_record(&output->effect_pass, &shell->vulkan, command_buffer, renderer_frame_index(&shell->renderer),
                         &effect_source, &output->effect);
}

bool video_output_stage0_result(const VideoOutput* output, uint64_t* imgui_id, uint32_t* width, uint32_t* height)
{
    if (output->on_gl)
    {
        return gl_shader_chain_stage0_result(&output->gl_chain, imgui_id, width, height);
    }
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
    if (shell->api == GpuApi::gl)
    {
        gl_screen_effect_destroy(&output->gl_effect, &shell->gl);
        gl_picture_destroy(&output->gl_picture, &shell->gl);
        gl_shader_chain_destroy(&output->gl_chain, &shell->gl);
        output->chain = ShaderChain{};   // never had a chain or images on GL
        output->effect_pass = ScreenEffect{};
        output->shaded = false;
        return;
    }
    screen_effect_destroy(&output->effect_pass, &shell->vulkan);
    picture_destroy(&output->picture, &shell->vulkan);
    shader_chain_destroy(&output->chain, library, &shell->vulkan);
    output->shaded = false;
}
