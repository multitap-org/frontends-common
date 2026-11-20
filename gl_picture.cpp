#include "gl_picture.h"

#include <SDL3/SDL_log.h>

#include "imgui.h"

void gl_picture_destroy(GlPicture* picture, GlContext* context)
{
    gl_render_target_destroy(context, &picture->texture, &picture->framebuffer);
    *picture = GlPicture{};
}

bool gl_picture_prepare(GlPicture* picture, GlContext* context, const GlStreamingTexture* source, bool has_content,
                        const ViewportResult* viewport)
{
    picture->ready = false;
    picture->use_copy = false;
    if (!has_content || source->shown == 0 || viewport->width == 0 || viewport->height == 0 ||
        viewport->source_width == 0 || viewport->source_height == 0)
    {
        return false;
    }
    picture->viewport = *viewport;
    picture->ready = true;
    if (viewport->filter == ViewportFilter::nearest)
    {
        return true;
    }

    // Bilinear: the shown part, same size. Sharp bilinear: scaled up by the
    // whole part of the scale, within the GPU's texture size limit.
    uint32_t prescale_x = viewport->filter == ViewportFilter::sharp_bilinear ? viewport->prescale_x : 1;
    uint32_t prescale_y = viewport->filter == ViewportFilter::sharp_bilinear ? viewport->prescale_y : 1;
    uint64_t limit = static_cast<uint64_t>(context->max_texture_size);
    while (prescale_x > 1 && static_cast<uint64_t>(viewport->source_width) * prescale_x > limit)
    {
        prescale_x--;
    }
    while (prescale_y > 1 && static_cast<uint64_t>(viewport->source_height) * prescale_y > limit)
    {
        prescale_y--;
    }
    uint32_t width = viewport->source_width * prescale_x;
    uint32_t height = viewport->source_height * prescale_y;
    if (picture->texture == 0 || width != picture->width || height != picture->height)
    {
        gl_render_target_destroy(context, &picture->texture, &picture->framebuffer);
        picture->width = 0;
        picture->height = 0;
        if (!gl_render_target_create(context, width, height, &picture->texture, &picture->framebuffer))
        {
            return true;   // drawn straight from the source this frame (bilinear)
        }
        picture->width = width;
        picture->height = height;
        SDL_LogInfo(SDL_LOG_CATEGORY_GPU, "Picture copy (GL): %ux%u (x%u, x%u)", width, height, prescale_x, prescale_y);
    }
    picture->use_copy = true;
    return true;
}

void gl_picture_draw(const GlPicture* picture, ImDrawList* draw_list, const GlStreamingTexture* source,
                     float origin_x, float origin_y)
{
    if (!picture->ready)
    {
        return;
    }
    const ViewportResult* viewport = &picture->viewport;
    ImVec2 top_left(origin_x + static_cast<float>(viewport->x), origin_y + static_cast<float>(viewport->y));
    ImVec2 bottom_right(top_left.x + static_cast<float>(viewport->width),
                        top_left.y + static_cast<float>(viewport->height));
    const ImGuiPlatformIO& platform_io = ImGui::GetPlatformIO();
    // ImTextureID on ImGui's GL backend is the texture name.
    if (picture->use_copy)
    {
        draw_list->AddCallback(platform_io.DrawCallback_SetSamplerLinear, nullptr);
        draw_list->AddImage(ImTextureRef(static_cast<ImTextureID>(picture->texture)), top_left, bottom_right,
                            ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f));
        return;
    }
    float capacity_width = static_cast<float>(source->capacity_width);
    float capacity_height = static_cast<float>(source->capacity_height);
    ImVec2 uv_min(static_cast<float>(viewport->source_x) / capacity_width,
                  static_cast<float>(viewport->source_y) / capacity_height);
    ImVec2 uv_max(static_cast<float>(viewport->source_x + viewport->source_width) / capacity_width,
                  static_cast<float>(viewport->source_y + viewport->source_height) / capacity_height);
    bool nearest = viewport->filter == ViewportFilter::nearest;
    draw_list->AddCallback(nearest ? platform_io.DrawCallback_SetSamplerNearest : platform_io.DrawCallback_SetSamplerLinear,
                           nullptr);
    draw_list->AddImage(ImTextureRef(static_cast<ImTextureID>(source->shown)), top_left, bottom_right, uv_min, uv_max);
    // Back to ImGui's default for whatever this draw list draws next.
    draw_list->AddCallback(platform_io.DrawCallback_SetSamplerLinear, nullptr);
}

void gl_picture_record(GlPicture* picture, GlContext* context, const GlStreamingTexture* source)
{
    if (!picture->ready || !picture->use_copy || picture->framebuffer == 0)
    {
        return;
    }
    const ViewportResult* viewport = &picture->viewport;
    float capacity_width = static_cast<float>(source->capacity_width);
    float capacity_height = static_cast<float>(source->capacity_height);
    float rect[4] = { static_cast<float>(viewport->source_x) / capacity_width,
                      static_cast<float>(viewport->source_y) / capacity_height,
                      static_cast<float>(viewport->source_width) / capacity_width,
                      static_cast<float>(viewport->source_height) / capacity_height };
    GlFunctions* gl = &context->gl;
    gl->BindFramebuffer(GL_FRAMEBUFFER, picture->framebuffer);
    gl->Viewport(0, 0, static_cast<GLsizei>(picture->width), static_cast<GLsizei>(picture->height));
    gl_copy_draw(context, source->shown, rect, false, false);   // nearest: whole-number scale, sharp
    gl->BindFramebuffer(GL_FRAMEBUFFER, 0);
    gl_check_errors(context, "picture copy");
}
