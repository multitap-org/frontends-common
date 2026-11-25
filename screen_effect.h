#pragma once

// The screen effect pass on Vulkan (video_output.h uses it; frontends set the
// knobs through video_output_set_effect): the picture as it would be drawn
// (the shader chain's output, the picture's copy or the core's texture, with
// the same sampling) drawn again through our own small shader
// (shaders/screen_effect.*) into an image of the picture's size on screen,
// which the GUI then draws 1:1 instead. After the shader chain, before the UI:
// the user's preset is untouched and the UI never gets the effect.
//
// It only runs on frames with an effect (knobs not at rest) or a capture,
// so it costs nothing the rest of the time.
//
// A capture (a GPU screenshot) copies that image into a host-visible buffer
// in the same command buffer. It is read once the frame's fence has been
// waited by the renderer (two frames later at most), so the GPU never stalls
// for it.
//
// Each frame, in video_output's order:
//   screen_effect_prepare()   while building the UI (may recreate the image,
//                             which waits for the GPU; only when its size changes)
//   screen_effect_shown_image()  what the GUI draws (shown_image.h)
//   screen_effect_collect()   after app_shell_acquire, every frame: a capture whose frame is done
//   screen_effect_record()    after the picture's own GPU work, before rendering starts

#include <cstdint>

#include <vulkan/vulkan.h>

#include "renderer.h"   // FRAMES_IN_FLIGHT
#include "screen_effect_params.h"
#include "shown_image.h"
#include "viewport.h"
#include "vk_context.h"


// What the pass reads: an image in SHADER_READ_ONLY_OPTIMAL whose writes this
// frame are already visible to fragment shaders (the picture's, the chain's
// and the upload's last barriers all end that way).
struct ScreenEffectSource
{
    VkImageView view   = VK_NULL_HANDLE;
    bool        linear = false;                       // bilinear sampling (else nearest)
    float       rect[4] = {0.0f, 0.0f, 1.0f, 1.0f};   // the picture in it: x, y, width, height (texture coordinates)
};

struct ScreenEffect
{
    // Made once (the first prepare).
    VkDescriptorSetLayout set_layout      = VK_NULL_HANDLE;
    VkPipelineLayout      pipeline_layout = VK_NULL_HANDLE;
    VkRenderPass          render_pass     = VK_NULL_HANDLE;   // the 1.1 path only
    VkPipeline            pipeline        = VK_NULL_HANDLE;
    VkSampler             sampler_nearest = VK_NULL_HANDLE;
    VkSampler             sampler_linear  = VK_NULL_HANDLE;
    VkDescriptorPool      descriptor_pool = VK_NULL_HANDLE;
    VkDescriptorSet       sets[FRAMES_IN_FLIGHT] = {};   // one per frame slot (rewritten when the slot is free)
    bool                  failed          = false;   // the pipeline couldn't be made: never tried again

    // The target: the picture's size on screen.
    VkImage        image        = VK_NULL_HANDLE;
    VkDeviceMemory image_memory = VK_NULL_HANDLE;
    VkImageView    view         = VK_NULL_HANDLE;
    VkFramebuffer  framebuffer  = VK_NULL_HANDLE;   // the 1.1 path only
    VkImageLayout  layout       = VK_IMAGE_LAYOUT_UNDEFINED;
    uint32_t       width        = 0;
    uint32_t       height       = 0;
    uint64_t       texture_id   = 0;   // the UI's handle of the target (debug_ui_add_texture)

    // What this frame draws (set by screen_effect_prepare).
    ViewportResult viewport = {};
    bool           ready    = false;

    // The capture.
    ScreenEffectCaptureState capture_state = ScreenEffectCaptureState::none;
    VkBuffer       capture_buffer   = VK_NULL_HANDLE;
    VkDeviceMemory capture_memory   = VK_NULL_HANDLE;
    void*          capture_mapped   = nullptr;
    bool           capture_coherent = false;
    uint32_t       capture_width    = 0;   // what the buffer holds
    uint32_t       capture_height   = 0;
    VkDeviceSize   capture_size     = 0;   // the buffer's size
    uint64_t       capture_serial   = 0;   // the frame it was recorded in
    uint64_t       serial           = 0;   // frames collected (one per acquired frame)
    VideoCapture   captured         = {};  // ready: waiting for screen_effect_capture_take

    // From the context (screen_effect_prepare).
    VulkanPath                path            = VulkanPath::vulkan_1_3;
    PFN_vkCmdPipelineBarrier2 barrier2        = nullptr;
    PFN_vkCmdBeginRendering   begin_rendering = nullptr;
    PFN_vkCmdEndRendering     end_rendering   = nullptr;
};

// Frees everything. Waits for the GPU. Safe on an empty ScreenEffect.
void screen_effect_destroy(ScreenEffect* effect, const VulkanContext* context);

// Makes the target for `viewport` (and the pipeline the first time). False:
// draw the picture without the pass this frame (logged once if it can't be made).
bool screen_effect_prepare(ScreenEffect* effect, const VulkanContext* context, const ViewportResult* viewport);

// Adds the target to `draw_list`; the viewport's (0, 0) is at (origin_x, origin_y).
bool screen_effect_shown_image(const ScreenEffect* effect, float origin_x, float origin_y, ShownImage* image);

// Copies the target into the capture buffer on the next record (makes the
// buffer when needed). False if a capture is already on its way.
bool screen_effect_request_capture(ScreenEffect* effect);

// Every acquired frame (after app_shell_acquire), recording or not: reads a
// capture whose frame the GPU has finished.
void screen_effect_collect(ScreenEffect* effect, const VulkanContext* context);

// Records the pass (and a requested capture) into `command_buffer`, outside
// a rendering pass. `frame_index` = renderer_frame_index().
void screen_effect_record(ScreenEffect* effect, const VulkanContext* context, VkCommandBuffer command_buffer,
                          uint32_t frame_index, const ScreenEffectSource* source, const ScreenEffectParams* params);

// The finished capture, handed over (the caller frees it). False if none is ready.
bool screen_effect_capture_take(ScreenEffect* effect, VideoCapture* capture);
