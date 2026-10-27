// Meta Quest, VR_MODE_STEREO / VR_MODE_DIORAMA: the game renders into the left eye's half of the panel (device.c
// halves the window for those modes: vr_stereo_split_panel), and this file fills the right eye's half - and re-does
// the left one - so each eye gets its own picture of the same instant, displaced per pixel by the scene's depth.
//
// The inputs are the ones frame generation reprojects with (framegen.c): the presented picture, the same picture
// before the HUD (so the hearts and the item icons stay on the panel's plane instead of swimming with the scenery),
// the scene's depth and the game's view-projection for the frame. Frame generation and these modes never run
// together (fg_enabled() stands down while a stereo mode is on: the two halves have to be the same instant).
//
// The maths: an eye is the game's camera moved sideways by half the eye separation. For a scene point whose clip
// position is (x, y, z, w), w being its distance along the view axis, a camera moved by d in view space changes the
// clip x by the projection's x scale times -d: this eye's x is (x - P00*d)/w. P00 is |row 0 of the view-projection|
// (its x row times a unit view row), which is all a horizontal eye offset needs: the shader rebuilds the point from
// the depth buffer, applies that, and samples the picture at the pixel that lands on the output one.
#include <SDL3/SDL_timer.h>
#include <math.h>
#include <string.h>

#include "../../platform/xr_quest.h"
#include "../../rt.h"
#include "../../settings.h"
#include "record.h"
#include "vk.h"

// ---- the frame's data, from framegen.c (it owns the depth pick, the camera and the picture before the HUD)
extern NvnTexture *fg_vr_depth_take(float *sx, float *sy);  // the scene's depth (NULL: none this frame)
extern bool fg_vr_scene_take(TexBackend **scene);           // the picture before the HUD (false: not captured)
extern bool fg_vr_scene_srgb(void);                         // ...whose colours are sRGB-encoded (see the flags)
extern void fg_vr_scene_done(void);
extern bool widescreen_camera(uint64_t frame, float vp[16]);  // the game's view-projection this frame (runtime/hle/widescreen.c)
extern int vk_sc_quarter(void);
extern uint32_t nvn_window_origin(void);
extern uint32_t nvn_depth_mode(void);

// How far one game world unit is, in metres, and the eye separation asked for: the game's world is a grid of 15-unit
// cells, one of them roughly 1.6 m across (Link is a boy fitting in one). The exact number only scales the effect -
// Video > VR > Depth moves it either way, and a negative value swaps the eyes.
#define VR_WORLD_UNIT_M 0.107f
#define VR_EYE_SEPARATION_M 0.063f

typedef struct {
    VkImage image;
    VmaAllocation mem;
    VkImageView view;
    VkFormat format;
    uint32_t width, height;
} VrImage;

static VrImage g_src;   // one eye's half of the presented picture, copied aside (the warping's source)
static VrImage g_mask;  // where the HUD is (a quarter of the size)
static uint32_t g_ew, g_eh;

static VkDescriptorSetLayout g_dsl;
static VkPipelineLayout g_layout;
static VkPipeline g_pipe;
static VkFormat g_pipe_fmt;

static const char kWarp[] =
    "#version 450\n"
    "layout(set = 0, binding = 0) uniform sampler2D src;\n"     // one eye's half of the picture (with the HUD)
    "layout(set = 0, binding = 1) uniform sampler2D scene;\n"   // the picture before the HUD (framegen's copy)
    "layout(set = 0, binding = 2) uniform sampler2D depthT;\n"  // the scene's depth
    "layout(set = 0, binding = 3) uniform sampler2D mask;\n"    // where the HUD is (over the picture, quarter size)
    "layout(location = 0) in vec2 uv;\n"
    "layout(location = 0) out vec4 o;\n"
    // rect: where the game's picture lies in one eye's half (uv), crop: the part of it shown;
    // r0 / r3: rows 0 and 3 of the game's view-projection (row 3 is the clip w: the distance along the view axis);
    // a: x flags (1 the game's origin flip, 2 depth in [-1, 1], 4 the present flip, 16 sRGB-encoded picture),
    //    y this eye's clip x shift (its half of the separation), z 0 (the warp), w 1 (the HUD stays on the plane);
    // b: zw the depth image's content share (the game's own dynamic resolution)
    "layout(push_constant) uniform P { vec4 rect; vec4 crop; vec4 r0; vec4 r3; vec4 a; vec4 b; } p;\n"
    "bool flag(uint f) { return (uint(p.a.x) & f) != 0u; }\n"
    "vec2 toGame(vec2 w) { vec2 q = (w - p.rect.xy) / p.rect.zw; if (flag(4u)) q.y = 1.0 - q.y; return p.crop.xy + q * p.crop.zw; }\n"
    "vec2 toWin(vec2 g) { vec2 q = (g - p.crop.xy) / p.crop.zw; if (flag(4u)) q.y = 1.0 - q.y; return p.rect.xy + q * p.rect.zw; }\n"
    "vec4 srcAt(vec2 g) { return textureLod(src, toWin(g), 0.0); }\n"
    // the picture before the HUD is the scene's linear composite, the presented picture sRGB-encoded
    "vec3 sceneAt(vec2 g) {\n"
    "  vec3 c = clamp(textureLod(scene, g, 0.0).rgb, 0.0, 1.0);\n"
    "  if (!flag(16u)) return c;\n"
    "  return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c));\n"
    "}\n"
    // how much HUD there is around a place on the picture (the mask a quarter of the size: its neighbours count too,
    // the faint edge of the hearts' shadow is under its threshold)
    "float hudAt(vec2 g) {\n"
    "  vec2 t = 1.0 / vec2(textureSize(mask, 0));\n"
    "  vec2 q = toWin(g);\n"
    "  float w = textureLod(mask, q, 0.0).r;\n"
    "  for (int i = 0; i < 4; i++) w = max(w, textureLod(mask, q + t * vec2((i & 1) == 0 ? -1.0 : 1.0, (i & 2) == 0 ? -1.0 : 1.0), 0.0).r);\n"
    "  return clamp(w * 2.0, 0.0, 1.0);\n"
    "}\n"
    // where the scene point under the game uv g lands in this eye's picture (game uv)
    "vec2 moved(vec2 g) {\n"
    "  vec2 n = g;\n"
    // the depth averaged over ~9x9 pixels (five fetches): the exact depth tore thin things (tall grass, plants) apart
    "  vec2 off = 4.0 / vec2(textureSize(depthT, 0));\n"
    "  float z = 0.0;\n"
    "  for (int i = 0; i < 5; i++) {\n"
    "    vec2 q = n * p.b.zw + off * (i == 0 ? vec2(0.0) : vec2((i & 1) == 1 ? -1.0 : 1.0, (i & 2) == 2 ? -1.0 : 1.0));\n"
    "    z += 1.0 / max(1.0 - textureLod(depthT, q, 0.0).r, 1e-6);\n"
    "  }\n"
    "  float d = 1.0 - 5.0 / z;\n"
    "  vec2 ndc = vec2(n.x * 2.0 - 1.0, flag(1u) ? 1.0 - 2.0 * n.y : 2.0 * n.y - 1.0);\n"
    "  vec4 c = vec4(ndc, flag(2u) ? d * 2.0 - 1.0 : d, 1.0);\n"
    "  float w = dot(p.r3, c);\n"
    "  if (w <= 1e-5) return g;\n"
    "  float x = (dot(p.r0, c) + p.a.y) / w;\n"  // this eye's clip x (p.a.y: its half of the separation)
    "  float y = ndc.y;\n"                       // the eyes are apart horizontally: the vertical stays where it was
    "  return vec2(x * 0.5 + 0.5, flag(1u) ? 0.5 - y * 0.5 : y * 0.5 + 0.5);\n"
    "}\n"
    "void main() {\n"
    "  vec2 g = toGame(uv);\n"
    "  if (any(lessThan(uv, p.rect.xy)) || any(greaterThan(uv, p.rect.xy + p.rect.zw))) { o = srcAt(g); return; }\n"  // the bars
    "  vec2 lo = p.crop.xy, hi = p.crop.xy + p.crop.zw;\n"
    "  vec2 s = g;\n"
    "  if (p.a.z < 0.5) for (int i = 0; i < 2; i++) s = clamp(g - (moved(s) - s), lo, hi);\n"  // (the source pixel landing here)
    "  float w = p.a.w > 0.5 ? hudAt(g) : 0.0;\n"
    "  vec3 bg = mix(srcAt(s).rgb, sceneAt(s), hudAt(s));\n"  // where the source was a HUD pixel: the scene before it
    "  o = vec4(mix(bg, srcAt(g).rgb, w), 1.0);\n"
    "}\n";

// The HUD mask: each texel covers 4x4 pixels of the eye's picture, set where the picture differs from the scene before
// the HUD (framegen.c builds the same one for its in-between frames).
static const char kMask[] =
    "#version 450\n"
    "layout(set = 0, binding = 0) uniform sampler2D fin;\n"
    "layout(set = 0, binding = 1) uniform sampler2D scene;\n"
    "layout(location = 0) in vec2 uv;\n"
    "layout(location = 0) out vec4 o;\n"
    "layout(push_constant) uniform P { vec4 rect; vec4 crop; vec4 r0; vec4 r3; vec4 a; vec4 b; } p;\n"
    "bool flag(uint f) { return (uint(p.a.x) & f) != 0u; }\n"
    "vec2 toGame(vec2 w) { vec2 q = (w - p.rect.xy) / p.rect.zw; if (flag(4u)) q.y = 1.0 - q.y; return p.crop.xy + q * p.crop.zw; }\n"
    "void main() {\n"
    "  vec2 t = 1.0 / vec2(textureSize(fin, 0));\n"
    "  vec3 f = vec3(0.0);\n"
    "  for (int i = 0; i < 4; i++) f += textureLod(fin, uv + t * vec2((i & 1) == 0 ? -1.0 : 1.0, (i & 2) == 0 ? -1.0 : 1.0), 0.0).rgb;\n"
    "  vec3 c = clamp(textureLod(scene, toGame(uv), 0.0).rgb, 0.0, 1.0);\n"
    "  if (flag(16u)) c = mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c));\n"
    "  vec3 d = abs(f * 0.25 - c);\n"
    // (0.05: the two sides round differently - 8-bit copies against filtered taps - by up to a few levels)
    "  o = vec4(step(0.05, max(d.r, max(d.g, d.b))));\n"
    "}\n";

static VkPipeline fs_pipeline_v(VkShaderModule fs, VkFormat fmt, VkPipelineLayout layout, const char *vert) {
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vk_builtin_shader(vert), .pName = "main"},  // (a fullscreen triangle, uv = position)
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xF};
    VkPipelineColorBlendStateCreateInfo cbs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1,
                                               .pAttachments = &cba};
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2,
                                           .pDynamicStates = dyn};
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1,
                                        .pColorAttachmentFormats = &fmt};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri, .stageCount = 2,
                                       .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vs,
                                       .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds,
                                       .pColorBlendState = &cbs, .pDynamicState = &dy, .layout = layout};
    VkPipeline p;
    return vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &pi, NULL, &p) == VK_SUCCESS ? p : VK_NULL_HANDLE;
}

static VkPipelineLayout set_layout(int nbind, uint32_t push, VkDescriptorSetLayout *dsl) {
    VkDescriptorSetLayoutBinding bd[8];
    for (int i = 0; i < nbind; i++)
        bd[i] = (VkDescriptorSetLayoutBinding){(uint32_t)i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = (uint32_t)nbind,
                                          .pBindings = bd};
    VK_CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &li, NULL, dsl));
    VkPushConstantRange r = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, push};
    VkPipelineLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = dsl,
                                     .pushConstantRangeCount = push ? 1 : 0, .pPushConstantRanges = push ? &r : NULL};
    VkPipelineLayout l;
    VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &ci, NULL, &l));
    return l;
}

static void bind_images(VkCommandBuffer cb, VkPipelineLayout layout, VkDescriptorSetLayout dsl, int n, VkImageView *v, const bool *linear) {
    VkDescriptorSet set = vk_alloc_set(dsl);
    VkDescriptorImageInfo ii[8];
    VkWriteDescriptorSet w[8];
    for (int i = 0; i < n; i++) {
        ii[i] = (VkDescriptorImageInfo){vk_default_sampler(linear[i]), v[i], VK_IMAGE_LAYOUT_GENERAL};
        w[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = (uint32_t)i,
                                      .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii[i]};
    }
    rec_UpdateDescriptorSets((uint32_t)n, w);
    rec_CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, NULL);
}

static bool make_image(VrImage *t, VkFormat f, uint32_t w, uint32_t h) {
    if (t->image && t->width == w && t->height == h && t->format == f) return true;
    if (t->image) vk_defer_view(t->view), vk_defer_image(t->image, t->mem);
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f,
                            .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    *t = (VrImage){0};
    if (vmaCreateImage(g_vk.vma, &ci, &ai, &t->image, &t->mem, NULL) != VK_SUCCESS) { t->image = VK_NULL_HANDLE; return false; }
    vk_image_init_layout(vk_upload_cmd(), t->image, VK_IMAGE_ASPECT_COLOR_BIT);
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = t->image, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                .format = f, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (vkCreateImageView(g_vk.dev, &vi, NULL, &t->view) != VK_SUCCESS) { t->view = VK_NULL_HANDLE; return false; }
    t->format = f, t->width = w, t->height = h;
    return true;
}

// The swapchain image is a different one each frame: a view of each is kept (the swapchain has a handful of them) and
// handed over to the deferred list when the slot is reused (its image may have been replaced by a resize by then).
static VkImageView sc_view(VkImage img, VkFormat fmt) {
    static struct { VkImage img; VkFormat fmt; VkImageView view; } c[8];
    static int next;
    for (int i = 0; i < 8; i++)
        if (c[i].view && c[i].img == img && c[i].fmt == fmt) return c[i].view;
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                .format = fmt, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkImageView v = VK_NULL_HANDLE;
    if (vkCreateImageView(g_vk.dev, &vi, NULL, &v) != VK_SUCCESS) return VK_NULL_HANDLE;
    if (c[next].view) vk_defer_view(c[next].view);
    c[next].img = img, c[next].fmt = fmt, c[next].view = v;
    next = (next + 1) % 8;
    return v;
}

// The swapchain's bytes as the compositor scans them out: a copy of an sRGB image has to be made in the matching UNORM
// format, else sampling it would linearise them and both eyes would come out washed out.
static VkFormat raw_fmt(VkFormat f) {
    switch (f) {
    case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
    case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
    default: return f;
    }
}

static void barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                               .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                               .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                               .oldLayout = from, .newLayout = to, .image = img,
                               .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    rec_CmdPipelineBarrier2(cb, &di);
}

// The HUD mask over the eye's picture (needs the picture before the HUD; without one nothing is masked)
static bool build_mask(VkCommandBuffer cb, TexBackend *scene, int flags, const float rect[4], const float crop[4]) {
    static VkDescriptorSetLayout mdsl;
    static VkPipelineLayout mlayout;
    static VkPipeline mpipe;
    uint32_t mw = (g_ew + 3) / 4, mh = (g_eh + 3) / 4;
    if (!scene || !scene->image || !g_src.view || !make_image(&g_mask, VK_FORMAT_R8_UNORM, mw, mh)) return false;
    VkImageView sv = vk_image_view(scene, VK_IMAGE_VIEW_TYPE_2D, scene->format, 0, 1, 0, 1, (VkComponentMapping){0}, VK_IMAGE_ASPECT_COLOR_BIT);
    if (!sv) return false;
    if (!mlayout) mlayout = set_layout(2, 96, &mdsl);
    if (!mpipe) {
        VkShaderModule fs = vk_compile_glsl(kMask, STAGE_FRAGMENT, "vr_stereo_mask.frag");
        mpipe = fs ? fs_pipeline_v(fs, VK_FORMAT_R8_UNORM, mlayout, "nv12.vert") : VK_NULL_HANDLE;
        if (!mpipe) return false;
    }
    struct { float rect[4], crop[4], r0[4], r3[4], a[4], b[4]; } pc = {0};
    pc.rect[0] = rect[0] / g_ew, pc.rect[1] = rect[1] / g_eh, pc.rect[2] = rect[2] / g_ew, pc.rect[3] = rect[3] / g_eh;
    memcpy(pc.crop, crop, sizeof pc.crop);
    pc.a[0] = (float)flags;
    vk_full_barrier(cb);
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = g_mask.view,
                                    .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                    .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {mw, mh}}, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &ca};
    rec_CmdBeginRendering(cb, &ri);
    rec_CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, mpipe);
    VkImageView v2[2] = {g_src.view, sv};
    static const bool lin[2] = {true, true};
    bind_images(cb, mlayout, mdsl, 2, v2, lin);
    rec_CmdPushConstants(cb, mlayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
    VkViewport vp = {0, 0, (float)mw, (float)mh, 0, 1};
    VkRect2D sc = {{0, 0}, {mw, mh}};
    rec_CmdSetViewport(cb, 0, 1, &vp);
    rec_CmdSetScissor(cb, 0, 1, &sc);
    rec_CmdDraw(cb, 3, 1, 0, 0);
    rec_CmdEndRendering(cb);
    vk_full_barrier(cb);
    return true;
}

static VkPipeline warp_pipeline(VkFormat fmt) {
    if (g_pipe && g_pipe_fmt == fmt) return g_pipe;
    if (!g_layout) g_layout = set_layout(4, 96, &g_dsl);
    VkShaderModule fs = vk_compile_glsl(kWarp, STAGE_FRAGMENT, "vr_stereo.frag");
    VkPipeline p = fs ? fs_pipeline_v(fs, fmt, g_layout, "nv12.vert") : VK_NULL_HANDLE;
    if (!p) return VK_NULL_HANDLE;
    g_pipe = p, g_pipe_fmt = fmt;
    LOG("[vr] stereo: one eye per half of the panel, warped by the scene's depth\n");
    return p;
}

// One eye's half copied into the other (both eyes the same picture: a flat panel - what a frame without a depth
// buffer or without a camera gets, so the player never sees a broken one)
static void duplicate_halves(VkCommandBuffer cb, VkImage sc) {
    barrier(cb, sc, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy c = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                     .extent = {g_ew, g_eh, 1}};
    c.dstOffset = (VkOffset3D){(int32_t)g_ew, 0, 0};
    rec_CmdCopyImage(cb, g_src.image, VK_IMAGE_LAYOUT_GENERAL, sc, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    barrier(cb, sc, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
}

void vr_stereo_draw(VkCommandBuffer cb, VkImage sc, VkFormat fmt, uint32_t w, uint32_t h, uint64_t frame,
                    const float rect[4], const float crop[4], bool flip_present) {
    if (!vr_stereo_on() || !sc || !w || !h || (w & 1) || w < 64) return;
    if (vk_sc_quarter()) return;  // (a pre-rotated window: its halves aren't the eyes there - never on a headset)
    g_ew = w / 2, g_eh = h;
    if (!make_image(&g_src, raw_fmt(fmt), g_ew, g_eh) || !g_src.view) return;

    // ---- one eye's half out of the panel image (both eyes are built from it below)
    barrier(cb, sc, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkImageCopy c = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                     .extent = {g_ew, g_eh, 1}};
    rec_CmdCopyImage(cb, sc, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_src.image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    barrier(cb, sc, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    // ---- the frame's data
    float dsx = 1, dsy = 1;
    NvnTexture *dt = fg_vr_depth_take(&dsx, &dsy);
    TexBackend *db = dt ? dt->backend : NULL;  // (NvnTexture::backend is a void *: framegen.c keeps one too)
    TexBackend *scene = NULL;
    bool have_scene = fg_vr_scene_take(&scene);
    bool srgb = fg_vr_scene_srgb();
    float vp[16];
    bool cam = widescreen_camera(frame, vp);
    VkPipeline pipe = warp_pipeline(fmt);
    if (!pipe || !db || !dt || !cam) {
        static uint64_t last;
        if (frame < 3 || frame - last > 600) {
            last = frame;
            LOG("[vr] no stereo this frame (picture %s, depth %s, camera %s): both halves the same picture\n",
                pipe ? "ok" : "no pipeline", db ? "ok" : "none", cam ? "ok" : "none");
        }
        duplicate_halves(cb, sc);
        fg_vr_scene_done();
        return;
    }
    // the eye's half of the separation, in clip units: P00 (the projection's x scale) times the world-space offset
    int flags = (nvn_window_origin() != 0 ? 1 : 0) | (nvn_depth_mode() == 0 ? 2 : 0) | (flip_present ? 4 : 0) | (srgb ? 16 : 0);
    bool have_mask = have_scene && build_mask(cb, scene, flags, rect, crop);
    float p00 = sqrtf(vp[0] * vp[0] + vp[1] * vp[1] + vp[2] * vp[2] + vp[3] * vp[3]);
    float shift = p00 * (VR_EYE_SEPARATION_M * 0.5f / VR_WORLD_UNIT_M) * (g_settings.vr_depth / 100.0f);
    static uint64_t last_log;
    if (frame < 3 || frame - last_log > 600) {
        last_log = frame;
        LOG("[vr] stereo: P00 %.3f, clip shift %.4f (depth %+.0f%%), depth %ux%u scale %.2f %.2f, HUD mask %s\n", p00, shift,
            g_settings.vr_depth, db->width, db->height, dsx, dsy, have_mask ? "yes" : "no");
    }
    VkImageView sv = vk_image_view(scene, VK_IMAGE_VIEW_TYPE_2D, scene->format, 0, 1, 0, 1, (VkComponentMapping){0}, VK_IMAGE_ASPECT_COLOR_BIT);
    VkImageView dv = vk_image_view(db, VK_IMAGE_VIEW_TYPE_2D, db->format, 0, 1, 0, 1, (VkComponentMapping){0}, VK_IMAGE_ASPECT_DEPTH_BIT);
    VkImageView cv = sc_view(sc, fmt);
    VkImageView v4[4] = {g_src.view, sv, dv, have_mask ? g_mask.view : g_src.view};
    if (!sv || !dv || !cv || !v4[3]) { duplicate_halves(cb, sc); fg_vr_scene_done(); return; }

    struct { float rect[4], crop[4], r0[4], r3[4], a[4], b[4]; } pc = {0};
    pc.rect[0] = rect[0] / g_ew, pc.rect[1] = rect[1] / g_eh, pc.rect[2] = rect[2] / g_ew, pc.rect[3] = rect[3] / g_eh;
    memcpy(pc.crop, crop, sizeof pc.crop);
    memcpy(pc.r0, vp, sizeof pc.r0);
    memcpy(pc.r3, vp + 12, sizeof pc.r3);
    pc.a[0] = (float)flags;
    pc.a[3] = have_mask ? 1.0f : 0.0f;
    pc.b[2] = dsx, pc.b[3] = dsy;

    vk_full_barrier(cb);
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = cv,
                                    .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                                    .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {w, h}}, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &ca};
    rec_CmdBeginRendering(cb, &ri);
    rec_CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    static const bool lin[4] = {true, true, false, true};  // (the depth slot: a plain sampler, as framegen's)
    bind_images(cb, g_layout, g_dsl, 4, v4, lin);
    for (int eye = 0; eye < 2; eye++) {
        // the eyes are apart horizontally: the left one looks from the left. A negative Depth swaps them, for a game
        // world whose right is the other way round.
        pc.a[1] = eye ? -shift : shift;
        rec_CmdPushConstants(cb, g_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
        VkViewport vpf = {(float)(eye ? g_ew : 0), 0, (float)g_ew, (float)g_eh, 0, 1};
        VkRect2D r2 = {{(int32_t)(eye ? g_ew : 0), 0}, {g_ew, g_eh}};
        rec_CmdSetViewport(cb, 0, 1, &vpf);
        rec_CmdSetScissor(cb, 0, 1, &r2);
        rec_CmdDraw(cb, 3, 1, 0, 0);
    }
    rec_CmdEndRendering(cb);
    vk_full_barrier(cb);
    fg_vr_scene_done();
}
