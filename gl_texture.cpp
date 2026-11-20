#include "gl_texture.h"

#include <SDL3/SDL_log.h>

static void set_clamp(GlFunctions* gl)
{
    gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
}

bool gl_render_target_create(GlContext* context, uint32_t width, uint32_t height, GLuint* texture,
                             GLuint* framebuffer)
{
    GlFunctions* gl = &context->gl;
    gl->GenTextures(1, texture);
    gl->BindTexture(GL_TEXTURE_2D, *texture);
    set_clamp(gl);
    gl->TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, static_cast<GLsizei>(width), static_cast<GLsizei>(height), 0, GL_RGBA,
                   GL_UNSIGNED_BYTE, nullptr);
    gl->BindTexture(GL_TEXTURE_2D, 0);

    gl->GenFramebuffers(1, framebuffer);
    gl->BindFramebuffer(GL_FRAMEBUFFER, *framebuffer);
    gl->FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *texture, 0);
    GLenum status = gl->CheckFramebufferStatus(GL_FRAMEBUFFER);
    gl->BindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE)
    {
        // GLES 2 only promises RGB565 / RGBA4 targets; RGBA8 needs
        // GL_OES_rgb8_rgba8 (the Tegra 4 has it, WebGL 2 always does).
        SDL_LogError(SDL_LOG_CATEGORY_GPU, "GL: can't render into a %ux%u RGBA8 texture (status 0x%04X)", width,
                     height, static_cast<uint32_t>(status));
        gl_render_target_destroy(context, texture, framebuffer);
        return false;
    }
    gl_check_errors(context, "render target");
    return true;
}

void gl_render_target_destroy(GlContext* context, GLuint* texture, GLuint* framebuffer)
{
    GlFunctions* gl = &context->gl;
    if (*framebuffer != 0)
    {
        gl->DeleteFramebuffers(1, framebuffer);
        *framebuffer = 0;
    }
    if (*texture != 0)
    {
        gl->DeleteTextures(1, texture);
        *texture = 0;
    }
}

// GLES's type for each upload (0RGB1555 arrives here converted to RGB565).
static void upload_format(FramePixelFormat format, GLenum* layout, GLenum* type)
{
    if (format == FramePixelFormat::xrgb8888)
    {
        *layout = GL_RGBA;
        *type = GL_UNSIGNED_BYTE;
        return;
    }
    // GL's 5_6_5 packs red in the top bits of a 16-bit value: libretro's RGB565.
    *layout = GL_RGB;
    *type = GL_UNSIGNED_SHORT_5_6_5;
}

bool gl_streaming_texture_create(GlStreamingTexture* texture, GlContext* context, uint32_t width, uint32_t height,
                                 FramePixelFormat format)
{
    gl_streaming_texture_destroy(texture, context);
    if (width == 0 || height == 0 || width > static_cast<uint32_t>(context->max_texture_size) ||
        height > static_cast<uint32_t>(context->max_texture_size))
    {
        SDL_LogError(SDL_LOG_CATEGORY_GPU, "GL: no %ux%u texture (largest %d)", width, height,
                     context->max_texture_size);
        return false;
    }
    texture->format = format;
    texture->capacity_width = width;
    texture->capacity_height = height;
    if (format == FramePixelFormat::xrgb1555)
    {
        texture->converted.assign(static_cast<size_t>(width) * height, 0);
    }

    GlFunctions* gl = &context->gl;
    GLenum layout = GL_RGBA;
    GLenum type = GL_UNSIGNED_BYTE;
    upload_format(format, &layout, &type);
    gl->GenTextures(1, &texture->upload);
    gl->BindTexture(GL_TEXTURE_2D, texture->upload);
    set_clamp(gl);
    gl->TexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(layout), static_cast<GLsizei>(width),
                   static_cast<GLsizei>(height), 0, layout, type, nullptr);
    gl->BindTexture(GL_TEXTURE_2D, 0);

    if (!gl_render_target_create(context, width, height, &texture->shown, &texture->framebuffer))
    {
        gl_streaming_texture_destroy(texture, context);
        return false;
    }
    gl_check_errors(context, "streaming texture");
    return true;
}

void gl_streaming_texture_destroy(GlStreamingTexture* texture, GlContext* context)
{
    if (texture->upload != 0)
    {
        context->gl.DeleteTextures(1, &texture->upload);
    }
    gl_render_target_destroy(context, &texture->shown, &texture->framebuffer);
    *texture = GlStreamingTexture{};
}

// 0RGB1555 -> RGB565: red and blue keep their 5 bits, green's 5 bits become
// 6 (shifted up, the top bit copied into the new low one, so full green stays full).
static void convert_1555(uint16_t* destination, const uint16_t* source, size_t count)
{
    for (size_t i = 0; i < count; i++)
    {
        uint32_t pixel = source[i];
        uint32_t red = (pixel >> 10) & 0x1Fu;
        uint32_t green = (pixel >> 5) & 0x1Fu;
        uint32_t blue = pixel & 0x1Fu;
        uint32_t green6 = (green << 1) | (green >> 4);
        destination[i] = static_cast<uint16_t>((red << 11) | (green6 << 5) | blue);
    }
}

bool gl_streaming_texture_upload(GlStreamingTexture* texture, GlContext* context, const void* pixels, uint32_t width,
                                 uint32_t height)
{
    if (texture->upload == 0 || width == 0 || height == 0 || width > texture->capacity_width ||
        height > texture->capacity_height)
    {
        return false;
    }
    GlFunctions* gl = &context->gl;
    const void* data = pixels;
    if (texture->format == FramePixelFormat::xrgb1555)
    {
        convert_1555(texture->converted.data(), static_cast<const uint16_t*>(pixels),
                     static_cast<size_t>(width) * height);
        data = texture->converted.data();
    }
    GLenum layout = GL_RGBA;
    GLenum type = GL_UNSIGNED_BYTE;
    upload_format(texture->format, &layout, &type);
    // Rows are tightly packed; a 16-bit row of odd width isn't 4-byte aligned.
    gl->PixelStorei(GL_UNPACK_ALIGNMENT, texture->format == FramePixelFormat::xrgb8888 ? 4 : 2);
    gl->BindTexture(GL_TEXTURE_2D, texture->upload);
    gl->TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height), layout, type,
                      data);
    gl->BindTexture(GL_TEXTURE_2D, 0);
    gl->PixelStorei(GL_UNPACK_ALIGNMENT, 4);

    // Into `shown`, same corner, RGB put right.
    gl->BindFramebuffer(GL_FRAMEBUFFER, texture->framebuffer);
    gl->Viewport(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height));
    float rect[4] = { 0.0f, 0.0f, static_cast<float>(width) / static_cast<float>(texture->capacity_width),
                      static_cast<float>(height) / static_cast<float>(texture->capacity_height) };
    gl_copy_draw(context, texture->upload, rect, texture->format == FramePixelFormat::xrgb8888, false);
    gl->BindFramebuffer(GL_FRAMEBUFFER, 0);
    gl_check_errors(context, "streaming texture upload");
    return true;
}
