#pragma once

// picture.h on OpenGL ES: the core's picture where viewport.h says, as a
// ShownImage for the GUI, with the viewport's filter.
//
//   nearest         the texture's shown part as is, nearest sampling.
//   bilinear        a copy of the shown part at its exact size, then bilinear
//                   (so the edges never blend in pixels outside the picture).
//   sharp bilinear  the copy scaled up by the whole part of the scale with
//                   nearest, then bilinear for the rest.
//
// The copy is made in gl_picture_record() (the copy program into a render
// target), after the frame's upload; the GUI draws it when the frame is drawn.

#include <cstdint>

#include "gl_context.h"
#include "gl_texture.h"
#include "shown_image.h"
#include "viewport.h"


struct GlPicture
{
    GLuint   texture     = 0;   // the copy (bilinear / sharp bilinear)
    GLuint   framebuffer = 0;
    uint32_t width       = 0;
    uint32_t height      = 0;

    ViewportResult viewport = {};   // what this frame draws (set by gl_picture_prepare)
    bool           use_copy = false;
    bool           ready    = false;
};

void gl_picture_destroy(GlPicture* picture, GlContext* context);

// Decides what this frame draws (may recreate the copy when its size changes).
// False: nothing to draw.
bool gl_picture_prepare(GlPicture* picture, GlContext* context, const GlStreamingTexture* source, bool has_content,
                        const ViewportResult* viewport);

bool gl_picture_shown_image(const GlPicture* picture, const GlStreamingTexture* source, float origin_x,
                            float origin_y, ShownImage* image);

// Makes the copy (after the source's upload this frame). Does nothing for nearest.
void gl_picture_record(GlPicture* picture, GlContext* context, const GlStreamingTexture* source);
