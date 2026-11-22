#version 450
// screen_effect.h's pass: one triangle that covers the target; v_coord is
// 0..1 over it, row 0 (v_coord.y = 0) at the top, like the images' memory.

layout(location = 0) out vec2 v_coord;

void main()
{
    vec2 corner = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    v_coord = corner;
    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
