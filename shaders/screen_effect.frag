#version 450
// screen_effect.h's pass: the picture with the effect's knobs (all at rest:
// the picture as it is). Keep in step with gl_screen_effect.cpp's copy.

layout(push_constant) uniform Knobs
{
    vec4 source_rect;   // the picture in the source: x, y, width, height (texture coordinates)
    vec4 knobs;         // zoom (>= 1), desaturate (0..1), flash (0..1), bars (0..0.5 of the height)
    vec4 flash_color;   // rgb
} u;

layout(set = 0, binding = 0) uniform sampler2D u_source;

layout(location = 0) in vec2 v_coord;
layout(location = 0) out vec4 out_color;

void main()
{
    // Zoom around the picture's center (never out: it would sample outside it).
    vec2 coord = (v_coord - 0.5) / u.knobs.x + 0.5;
    vec3 color = texture(u_source, u.source_rect.xy + coord * u.source_rect.zw).rgb;
    float luma = dot(color, vec3(0.299, 0.587, 0.114));
    color = mix(color, vec3(luma), u.knobs.y);
    color = mix(color, u.flash_color.rgb, u.knobs.z);
    // Bars closing from the top and the bottom (a shutter).
    float edge = min(v_coord.y, 1.0 - v_coord.y);
    if (edge < u.knobs.w)
    {
        color = vec3(0.0);
    }
    out_color = vec4(color, 1.0);
}
