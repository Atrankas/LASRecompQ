// The Quest entry's stereo modes (runtime/nvn/vulkan/vr_stereo.c): one eye per half of the panel.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "vk.h"

// Warps the left eye's half of the panel image into both halves, each eye's copy displaced by the scene's depth
// (the game's view-projection, the depth buffer and the picture before the HUD: the same inputs frame generation
// reprojects with, runtime/nvn/vulkan/framegen.c). Called by device.c's vk_frame_end with the window image it is
// about to present; does nothing when the stereo modes are off (vr_stereo_on, platform/xr_quest.h).
//   rect / crop: where the game's picture lies in one eye's half of the panel (pixels) and what part of it is shown,
//   as device.c worked them out for the halved window.
void vr_stereo_draw(VkCommandBuffer cb, VkImage sc, VkFormat fmt, uint32_t w, uint32_t h, uint64_t frame,
                    const float rect[4], const float crop[4], bool flip_present);
