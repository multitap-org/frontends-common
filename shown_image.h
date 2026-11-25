#pragma once

// What a GUI draws to show a picture: part of a texture over a rectangle,
// with a filter. The renderer side (picture.h, shader_chain.h,
// screen_effect.h, their GL versions, video_output.h) fills one in and knows
// no GUI; the frontend's game view widget draws it (piccolo: retroui's
// game_view_draw). Another GUI only needs a way to draw a texture.

#include <cstdint>

#include "viewport.h"

enum class ShownImageFilter
{
    nearest,
    linear,
};

struct ShownImage
{
    uint64_t texture = 0;   // the GUI's handle for the texture (ui_texture.h)
    float    x       = 0.0f;   // where, in window pixels
    float    y       = 0.0f;
    float    width   = 0.0f;
    float    height  = 0.0f;
    float    u_min   = 0.0f;   // the part of the texture shown
    float    v_min   = 0.0f;
    float    u_max   = 1.0f;
    float    v_max   = 1.0f;
    ShownImageFilter filter = ShownImageFilter::nearest;
};

// The whole of `texture` over the viewport's rectangle, (0, 0) at (origin_x,
// origin_y), nearest: an image already the picture's size, drawn 1:1.
void shown_image_whole(uint64_t texture, const ViewportResult* viewport, float origin_x, float origin_y,
                       ShownImage* image);

// The viewport's source rectangle of a texture `capacity_width` x
// `capacity_height` big (a streaming texture bigger than the frame), with the
// viewport's filter.
void shown_image_part(uint64_t texture, const ViewportResult* viewport, uint32_t capacity_width,
                      uint32_t capacity_height, float origin_x, float origin_y, ShownImage* image);
