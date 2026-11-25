#pragma once

// Textures the GUI draws: a core's picture, the shader chain's output, a
// preview, a thumbnail. The renderer makes the texture and registers it here,
// getting back the GUI's handle for it, which it passes on (in a ShownImage,
// a VideoTexture) without knowing what it is. 0 is never a handle.
//
// Only the GUI's backend knows what a handle is. With ImGui (debug_ui.cpp,
// debug_ui_vulkan.cpp): on Vulkan a VkDescriptorSet made for the view, on GL
// the texture's own name (nothing to register). A frontend on another toolkit
// implements these three for it; the renderer doesn't change.

#include <cstdint>
#include <vulkan/vulkan.h>

// Vulkan: the view must be in SHADER_READ_ONLY_OPTIMAL layout whenever the GUI
// draws it. 0 if the GUI isn't set up for Vulkan.
uint64_t ui_texture_add_vulkan(VkImageView view);

// GL: a texture name (a GLuint).
uint64_t ui_texture_from_gl(uint32_t texture);

// Frees a handle from ui_texture_add_vulkan / ui_texture_from_gl. 0: nothing.
void ui_texture_remove(uint64_t texture);
