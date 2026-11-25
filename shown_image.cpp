// The picture as a texture over a rectangle (shown_image.h).

#include "shown_image.h"

static void place(const ViewportResult* viewport, float origin_x, float origin_y, ShownImage* image)
{
    image->x = origin_x + static_cast<float>(viewport->x);
    image->y = origin_y + static_cast<float>(viewport->y);
    image->width = static_cast<float>(viewport->width);
    image->height = static_cast<float>(viewport->height);
}

void shown_image_whole(uint64_t texture, const ViewportResult* viewport, float origin_x, float origin_y,
                       ShownImage* image)
{
    *image = {};
    image->texture = texture;
    place(viewport, origin_x, origin_y, image);
    image->filter = ShownImageFilter::nearest;
}

void shown_image_part(uint64_t texture, const ViewportResult* viewport, uint32_t capacity_width,
                      uint32_t capacity_height, float origin_x, float origin_y, ShownImage* image)
{
    *image = {};
    image->texture = texture;
    place(viewport, origin_x, origin_y, image);
    float width = static_cast<float>(capacity_width);
    float height = static_cast<float>(capacity_height);
    image->u_min = static_cast<float>(viewport->source_x) / width;
    image->v_min = static_cast<float>(viewport->source_y) / height;
    image->u_max = static_cast<float>(viewport->source_x + viewport->source_width) / width;
    image->v_max = static_cast<float>(viewport->source_y + viewport->source_height) / height;
    image->filter = viewport->filter == ViewportFilter::nearest ? ShownImageFilter::nearest : ShownImageFilter::linear;
}
