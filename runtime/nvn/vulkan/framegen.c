// 60 fps (generated): the game runs at its own 30 frames a second, and after each real frame an in-between one is
// made on the GPU and shown one refresh later (the approach of the WWHD port's runtime/src/vk/reproj.cpp).
// The in-between frame is the real one reprojected with the scene's depth to the camera moved on by half its last
// step (the camera's view-projection, each frame: runtime/hle/widescreen.c). The HUD stays where it is: at the
// scene's composite into the presented picture (exec.c, as mirror mode finds it) the picture is copied aside, and
// pixels where the finished picture differs from that copy are HUD; the window's own overlays (touch controls,
// menu) differ from it too. The window picture (with the upscaler's output and the overlays) is copied before it is
// presented, the in-between one is drawn from it into the next swapchain image and presented right after: FIFO
// shows it one refresh later. Characters' own movement isn't reprojected (it stays at 30 a second).
// Settings: Video > Frame generation; LARECOMP_FRAMEGEN=0/1 overrides.
#include <SDL3/SDL_timer.h>
#include <math.h>
#include <string.h>

#include "../../rt.h"
#include "../../settings.h"
#include "vk.h"

// ---- the frame's inputs (render thread)
static TexBackend g_scene;        // the presented picture right after the scene's composite (no HUD yet)
static bool g_scene_ok;           // captured this frame
static NvnTexture *g_depth;       // the scene's depth (the main pass's)
static float g_depth_scale[2];    // its content's share of the depth image (the game's dynamic resolution)
static float g_vp[16], g_vp_prev[16];
static bool g_have_vp, g_have_prev;
static uint64_t g_vp_frame;
static TexBackend g_comp;         // the window picture of the real frame
static bool g_comp_ok;

static VkDescriptorSetLayout g_dsl;
static VkPipelineLayout g_layout;
static VkPipeline g_pipe;
static VkFormat g_pipe_fmt;

static const char kWarp[] =
    "#version 450\n"
    "layout(set = 0, binding = 0) uniform sampler2D comp;\n"   // the window picture
    "layout(set = 0, binding = 1) uniform sampler2D scene;\n"  // the presented picture before the HUD
    "layout(set = 0, binding = 2) uniform sampler2D fin;\n"    // the presented picture (with the HUD)
    // the scene's view distance (1 / (1 - depth)) averaged over 8x8 pixels, bilinear (kDepthDown): a smooth movement -
    // the exact depth tore thin layered things (tall grass, plants) into rows when the camera moved
    "layout(set = 0, binding = 3) uniform sampler2D depthT;\n"
    "layout(location = 0) in vec2 uv;\n"
    "layout(location = 0) out vec4 o;\n"
    // m0..m3: rows of (generated view-projection) x inverse(this frame's); rect: the game picture in the window (uv);
    // crop: the part of the presented picture shown there; a: y flip (game NDC up = image top), depth in [-1, 1],
    // present flip, mode; b: objects' own movement on (x), the depth's content scale (zw)
    "layout(push_constant) uniform P { vec4 m0; vec4 m1; vec4 m2; vec4 m3; vec4 rect; vec4 crop; vec4 a; vec4 b; } p;\n"
    "vec2 toGame(vec2 w) { vec2 q = (w - p.rect.xy) / p.rect.zw; if (p.a.z > 0.5) q.y = 1.0 - q.y; return p.crop.xy + q * p.crop.zw; }\n"
    "vec2 toWin(vec2 g) { vec2 q = (g - p.crop.xy) / p.crop.zw; if (p.a.z > 0.5) q.y = 1.0 - q.y; return p.rect.xy + q * p.rect.zw; }\n"
    "layout(set = 0, binding = 4) uniform sampler2D hudMask;\n"  // where the HUD is: a quarter of the presented picture
    // (bilinear: also a texel around it)
    "bool hudAt(vec2 g) { return textureLod(hudMask, g, 0.0).r > 0.01; }\n"
    // where the scene point at g lands in the generated picture
    "vec2 moved(vec2 g) {\n"
    "  vec2 n = g;\n"
    "  float d = 1.0 - 1.0 / max(textureLod(depthT, n, 0.0).r, 1.0);\n"
    "  vec4 c = vec4(n.x * 2.0 - 1.0, p.a.x > 0.5 ? 1.0 - 2.0 * n.y : 2.0 * n.y - 1.0, p.a.y > 0.5 ? d * 2.0 - 1.0 : d, 1.0);\n"
    "  vec4 h = vec4(dot(p.m0, c), dot(p.m1, c), dot(p.m2, c), dot(p.m3, c));\n"
    "  if (h.w <= 1e-5) return g;\n"
    "  vec2 e = h.xy / h.w;\n"
    "  vec2 n2 = vec2(e.x * 0.5 + 0.5, p.a.x > 0.5 ? 0.5 - e.y * 0.5 : e.y * 0.5 + 0.5);\n"
    "  return n2;\n"
    "}\n"
    "layout(set = 0, binding = 5) uniform sampler2D flowT;\n"  // objects' own movement per 16x16 block (kFlow)
    // half the movement of the object at g beyond the camera's, where g's depth is its block's (characters walking:
    // with the camera's movement alone they stood still in the in-between frame, then jumped - and Link, whom the
    // camera follows, was moved with the background and shook)
    "vec2 objMove(vec2 g) {\n"
    "  if (p.b.x < 0.5) return vec2(0.0);\n"
    "  vec4 f = textureLod(flowT, g, 0.0);\n"
    "  if (f.w < 0.5) return vec2(0.0);\n"
    "  float z = textureLod(depthT, g, 0.0).r;\n"
    "  return abs(z - f.z) > 0.15 * f.z ? vec2(0.0) : f.xy * 0.5;\n"
    "}\n"
    "void main() {\n"
    "  if (p.a.w > 1.5) { vec2 n = toGame(uv); float d = 1.0 - 1.0 / max(textureLod(depthT, n, 0.0).r, 1.0); o = vec4(vec3(clamp((1.0 - d) * 20.0, 0.0, 1.0)), 1.0); return; }\n"  // (LARECOMP_FRAMEGEN_DEBUG=depth)
    "  if (p.a.w > 0.5) { o = textureLod(comp, uv, 0.0); return; }\n"  // (LARECOMP_FRAMEGEN_PLAIN: the cost without the warp)
    "  if (any(lessThan(uv, p.rect.xy)) || any(greaterThan(uv, p.rect.xy + p.rect.zw))) { o = textureLod(comp, uv, 0.0); return; }\n"
    "  vec2 g = toGame(uv);\n"
    "  if (hudAt(g)) { o = textureLod(comp, uv, 0.0); return; }\n"
    "  vec2 lo = p.crop.xy, hi = p.crop.xy + p.crop.zw;\n"
    "  vec2 s = g;\n"
    "  for (int i = 0; i < 2; i++) s = clamp(g - (moved(s) - s) - objMove(s), lo, hi);\n"  // (the source pixel that lands here)
    "  o = hudAt(s) ? vec4(textureLod(scene, s, 0.0).rgb, 1.0) : textureLod(comp, toWin(s), 0.0);\n"
    "}\n";

// the HUD mask: each texel covers 4x4 pixels of the presented picture, set where it differs from the scene's copy
static const char kMask[] =
    "#version 450\n"
    "layout(set = 0, binding = 0) uniform sampler2D scene;\n"
    "layout(set = 0, binding = 1) uniform sampler2D fin;\n"
    "layout(location = 0) in vec2 uv;\n"
    "layout(location = 0) out vec4 o;\n"
    "void main() {\n"
    "  vec2 t = 1.0 / vec2(textureSize(fin, 0));\n"
    "  float m = 0.0;\n"
    "  for (int i = 0; i < 4; i++) {\n"  // (bilinear taps between the 2x2 quads of the block: all 16 pixels)
    "    vec2 q = uv + t * vec2((i & 1) == 0 ? -1.0 : 1.0, (i & 2) == 0 ? -1.0 : 1.0);\n"
    "    vec3 d = abs(textureLod(fin, q, 0.0).rgb - textureLod(scene, q, 0.0).rgb);\n"
    "    m = max(m, step(0.01, max(d.r, max(d.g, d.b))));\n"
    "  }\n"
    "  o = vec4(m);\n"
    "}\n";
// a quarter-size brightness picture of the scene (each texel the 4x4 pixels' average), for kFlow
static const char kDown[] =
    "#version 450\n"
    "layout(set = 0, binding = 0) uniform sampler2D scene;\n"
    "layout(location = 0) in vec2 uv;\n"
    "layout(location = 0) out vec4 o;\n"
    "void main() {\n"
    "  vec2 t = 1.0 / vec2(textureSize(scene, 0));\n"
    "  vec3 c = vec3(0.0);\n"
    "  for (int i = 0; i < 4; i++) c += textureLod(scene, uv + t * vec2((i & 1) == 0 ? -1.0 : 1.0, (i & 2) == 0 ? -1.0 : 1.0), 0.0).rgb;\n"
    "  o = vec4(dot(c * 0.25, vec3(0.299, 0.587, 0.114)));\n"
    "}\n";
// the scene's view distance (1 / (1 - depth): up to a factor) averaged over each 8x8 pixels: 4 depth samples. (No
// textureGather on the depth buffer, here or in kFlow: with it the S20 FE froze solid - a hard reset, nothing logged -
// within minutes of play; plain fetches ran a day without one.)
static const char kDepthDown[] =
    "#version 450\n"
    "layout(set = 0, binding = 0) uniform sampler2D depthT;\n"
    "layout(location = 0) in vec2 uv;\n"
    "layout(location = 0) out vec4 o;\n"
    "layout(push_constant) uniform P { vec4 b; } p;\n"  // b.zw: the depth's content scale
    "void main() {\n"
    "  vec2 t = 1.0 / vec2(textureSize(depthT, 0)), h = 0.25 / p.b.xy;\n"  // (b.xy: this image's size)
    "  float z = 0.0;\n"
    "  for (int i = 0; i < 4; i++) {\n"
    "    float d = textureLod(depthT, (uv + h * vec2((i & 1) == 0 ? -1.0 : 1.0, (i & 2) == 0 ? -1.0 : 1.0)) * p.b.zw, 0.0).r;\n"
    "    z += 1.0 / max(1.0 - d, 1e-6);\n"
    "  }\n"
    "  o = vec4(min(z * 0.25, 60000.0));\n"
    "}\n";
// Objects' own movement, one value per 16x16 block of the scene (from the WWHD port's reproj.cpp): the block of this
// frame's quarter picture is looked for in the previous one around where the camera's movement alone puts it, and
// around the same place on the screen (what the camera follows: Link). Out: the movement beyond the camera's,
// previous -> this frame (uv), the block's nearest depth (1 / (1 - d): view distance up to a factor), valid.
// m0..m3: rows of previous view-projection x inverse(this one's); a: y flip, depth in [-1, 1]; b.zw: depth scale
static const char kFlow[] =
    "#version 450\n"
    "layout(set = 0, binding = 0) uniform sampler2D curImg;\n"
    "layout(set = 0, binding = 1) uniform sampler2D prevImg;\n"
    "layout(set = 0, binding = 2) uniform sampler2D depthT;\n"
    "layout(location = 0) in vec2 uv;\n"
    "layout(location = 0) out vec4 o;\n"
    "layout(push_constant) uniform P { vec4 m0; vec4 m1; vec4 m2; vec4 m3; vec4 a; vec4 b; } p;\n"
    "void block(sampler2D img, vec2 at, vec2 ts, out vec4 b[4]) {\n"  // 4x4 quarter-size texels as four gathers
    "  b[0] = textureGather(img, at + vec2(-1.0, -1.0) * ts, 0); b[1] = textureGather(img, at + vec2(1.0, -1.0) * ts, 0);\n"
    "  b[2] = textureGather(img, at + vec2(-1.0, 1.0) * ts, 0); b[3] = textureGather(img, at + vec2(1.0, 1.0) * ts, 0);\n"
    "}\n"
    "float sad(vec4 a[4], vec4 b[4]) { vec4 d = abs(a[0] - b[0]) + abs(a[1] - b[1]) + abs(a[2] - b[2]) + abs(a[3] - b[3]); return d.x + d.y + d.z + d.w; }\n"
    "void main() {\n"
    "  o = vec4(0.0);\n"
    "  vec2 ts = 1.0 / vec2(textureSize(curImg, 0));\n"
    "  vec4 cur[4]; block(curImg, uv, ts, cur);\n"
    "  vec4 m4 = (cur[0] + cur[1] + cur[2] + cur[3]) * 0.25; float mean = (m4.x + m4.y + m4.z + m4.w) * 0.25;\n"
    "  vec4 dv = abs(cur[0] - mean) + abs(cur[1] - mean) + abs(cur[2] - mean) + abs(cur[3] - mean);\n"
    "  vec2 dq = uv * p.b.zw, dt = 1.0 / vec2(textureSize(depthT, 0));\n"  // (the block's nearest depth)
    "  float d = min(min(textureLod(depthT, dq + vec2(-4.0, -4.0) * dt, 0.0).r, textureLod(depthT, dq + vec2(4.0, -4.0) * dt, 0.0).r),\n"
    "                min(textureLod(depthT, dq + vec2(-4.0, 4.0) * dt, 0.0).r, textureLod(depthT, dq + vec2(4.0, 4.0) * dt, 0.0).r));\n"
    "  o.z = min(1.0 / max(1.0 - d, 1e-6), 60000.0);\n"
    "  if (dv.x + dv.y + dv.z + dv.w < 0.12) return;\n"  // (little detail: no reliable match)
    "  vec4 c = vec4(uv.x * 2.0 - 1.0, p.a.x > 0.5 ? 1.0 - 2.0 * uv.y : 2.0 * uv.y - 1.0, p.a.y > 0.5 ? d * 2.0 - 1.0 : d, 1.0);\n"
    "  vec4 h = vec4(dot(p.m0, c), dot(p.m1, c), dot(p.m2, c), dot(p.m3, c));\n"
    "  bool camOk = h.w > 1e-5;\n"
    "  vec2 e = camOk ? h.xy / h.w : vec2(0.0);\n"
    "  vec2 pp = camOk ? vec2(e.x * 0.5 + 0.5, p.a.x > 0.5 ? 0.5 - e.y * 0.5 : e.y * 0.5 + 0.5) : uv;\n"
    "  camOk = camOk && all(greaterThan(pp, vec2(0.01))) && all(lessThan(pp, vec2(0.99)));\n"
    "  float best = 1e9, atCam = 1e9; vec2 bp = uv; vec4 b[4];\n"
    "  for (int k = 0; k < 2; k++) {\n"  // (around pp, and around the same place on the screen; +-6 texels, then refined)
    "    vec2 c0 = k == 0 ? pp : uv;\n"
    "    if (k == 0 && !camOk) continue;\n"
    "    for (int oy = -6; oy <= 6; oy += 2) for (int ox = -6; ox <= 6; ox += 2) {\n"
    "      vec2 at = c0 + vec2(ox, oy) * ts;\n"
    "      if (any(lessThan(at, vec2(0.01))) || any(greaterThan(at, vec2(0.99)))) continue;\n"
    "      block(prevImg, at, ts, b); float sd = sad(cur, b);\n"
    "      if (k == 0 && ox == 0 && oy == 0) atCam = sd;\n"
    "      if (sd < best) { best = sd; bp = at; }\n"
    "    }\n"
    "  }\n"
    "  vec2 c1 = bp;\n"
    "  for (int oy = -1; oy <= 1; oy++) for (int ox = -1; ox <= 1; ox++) {\n"
    "    if (ox == 0 && oy == 0) continue;\n"
    "    vec2 at = c1 + vec2(ox, oy) * ts;\n"
    "    block(prevImg, at, ts, b); float sd = sad(cur, b);\n"
    "    if (sd < best) { best = sd; bp = at; }\n"
    "  }\n"
    "  if (best > 1.2 || atCam <= best * 1.5 + 0.05) return;\n"  // (no clear match, or the camera explains it)
    "  o = vec4(pp - bp, o.z, 1.0);\n"
    "}\n";
// a block keeps its movement only when at least 2 of its 8 neighbours moved the same way (their average); lone
// blocks (water, flicker) are dropped
static const char kFilter[] =
    "#version 450\n"
    "layout(set = 0, binding = 0) uniform sampler2D flowIn;\n"
    "layout(location = 0) in vec2 uv;\n"
    "layout(location = 0) out vec4 o;\n"
    "void main() {\n"
    "  vec4 c = textureLod(flowIn, uv, 0.0);\n"
    "  o = vec4(0.0, 0.0, c.z, 0.0);\n"
    "  if (c.w < 0.5) return;\n"
    "  vec2 ts = 1.0 / vec2(textureSize(flowIn, 0));\n"
    "  vec2 sum = c.xy; float n = 0.0;\n"
    "  for (int j = -1; j <= 1; j++) for (int i = -1; i <= 1; i++) {\n"
    "    if (i == 0 && j == 0) continue;\n"
    "    vec4 q = textureLod(flowIn, uv + vec2(i, j) * ts, 0.0);\n"
    "    if (q.w > 0.5 && all(lessThan(abs(q.xy - c.xy), ts * 0.25))) { sum += q.xy; n += 1.0; }\n"
    "  }\n"
    "  if (n >= 2.0) o = vec4(sum / (n + 1.0), c.z, 1.0);\n"
    "}\n";
static TexBackend g_zc;  // kDepthDown's image (this frame's)
static bool g_zc_ok;
static TexBackend g_q[2], g_flow_raw, g_flow;  // quarter pictures (this frame's: g_q[g_qi]), block movement
static int g_qi;
static bool g_q_prev, g_flow_ok;  // the previous frame's quarter picture is there; this frame's movement is
static TexBackend g_mask;
static TexBackend *g_fin;
static bool build_mask(VkCommandBuffer cb);
static void build_flow(VkCommandBuffer cb);
static VkDescriptorSetLayout g_mdsl;
static VkPipelineLayout g_mlayout;
static VkPipeline g_mpipe;

static int fg_setting(void) {
    static int env = -2;
    if (env == -2) env = getenv("LARECOMP_FRAMEGEN") ? atoi(getenv("LARECOMP_FRAMEGEN")) : -1;
    return env >= 0 ? env : g_settings.frame_gen;
}

bool fg_enabled(void) { return fg_setting() != 0; }

static bool make_image(TexBackend *t, VkFormat f, uint32_t w, uint32_t h, VkImageUsageFlags usage) {
    if (t->image && t->width == w && t->height == h && t->format == f) return true;
    if (t->image) vk_views_forget(t), vk_defer_image(t->image, t->mem);
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f,
                            .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    *t = (TexBackend){0};
    if (vmaCreateImage(g_vk.vma, &ci, &ai, &t->image, &t->mem, NULL) != VK_SUCCESS) { t->image = VK_NULL_HANDLE; return false; }
    vk_image_init_layout(vk_upload_cmd(), t->image, VK_IMAGE_ASPECT_COLOR_BIT);
    t->format = f, t->width = w, t->height = h, t->depth = 1, t->levels = t->layers = t->samples = 1;
    t->aspect = VK_IMAGE_ASPECT_COLOR_BIT, t->base_type = VK_IMAGE_VIEW_TYPE_2D, t->gpu_owned = true;
    return true;
}

// exec.c draw_: a draw writing depth. The scene's depth is the one most draws of the frame write (the main pass's;
// not the shadow map's, a square one, nor the HUD's)
static struct { NvnTexture *t; int n; float sx, sy; } g_dc[8];
void fg_note_draw(NvnTexture *depth, uint32_t pass_w, uint32_t pass_h) {
    if (!depth || depth->width == depth->height) return;
    int i = 0;
    while (i < 8 && g_dc[i].t && g_dc[i].t != depth) i++;
    if (i == 8) return;
    if (!g_dc[i].t) {
        TexBackend *b = depth->backend;
        g_dc[i].t = depth;
        g_dc[i].sx = b && b->width ? (float)pass_w / b->width : 1;
        g_dc[i].sy = b && b->height ? (float)pass_h / b->height : 1;
    }
    g_dc[i].n++;
}
static void pick_depth(void) {
    int best = -1;
    for (int i = 0; i < 8 && g_dc[i].t; i++) if (best < 0 || g_dc[i].n > g_dc[best].n) best = i;
    g_depth = best >= 0 ? g_dc[best].t : NULL;
    if (best >= 0) g_depth_scale[0] = g_dc[best].sx, g_depth_scale[1] = g_dc[best].sy;
    memset(g_dc, 0, sizeof g_dc);
}

// exec.c draw_, right after the scene's composite into the presented picture (outside any pass)
void fg_scene_composited(VkCommandBuffer cb, TexBackend *rt) {
    if (!fg_enabled() || !rt || !rt->image || rt->samples != 1) return;
    if (!make_image(&g_scene, rt->format, rt->width, rt->height, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) return;
    vk_full_barrier(cb);
    VkImageCopy c = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                     .extent = {rt->width, rt->height, 1}};
    vkCmdCopyImage(cb, rt->image, VK_IMAGE_LAYOUT_GENERAL, g_scene.image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    vk_full_barrier(cb);
    g_scene_ok = true;
}

static void mat_mul(const float *a, const float *b, float *o) {  // rows: o = a b
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += a[r * 4 + k] * b[k * 4 + c];
            o[r * 4 + c] = s;
        }
}

static bool mat_inv(const float *m, float *o) {
    float inv[16], det;
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (fabsf(det) < 1e-20f) return false;
    for (int i = 0; i < 16; i++) o[i] = inv[i] / det;
    return true;
}

// device.c: whether this frame gets an in-between one (asked before the real frame is presented)
bool fg_frame_wanted(uint64_t frame, TexBackend *fin) {
    extern uint32_t g_game_interval;   // runtime/hle/framepace.c: the game's swap interval (2: its 30 fps mode)
    extern bool menu_is_open(void);
    extern bool widescreen_camera(uint64_t frame, float vp[16]);
    if (!fg_enabled()) return false;
    pick_depth();
    float vp[16];
    bool have = widescreen_camera(frame, vp);
    if (have) {
        if (g_have_vp) memcpy(g_vp_prev, g_vp, sizeof g_vp), g_have_prev = true;
        memcpy(g_vp, vp, sizeof vp), g_have_vp = true, g_vp_frame = frame;
    } else {
        g_have_vp = g_have_prev = false;
    }
    // only while the game holds its 30: real frames late (the GPU full) get no in-between ones - their GPU time goes
    // back to the game until it has room again (a period average under 34.5 ms for 1 s). The average moves slowly
    // (~1 s): a single late frame (a 50 ms one now and then in Madam MeowMeow's house) paused it for seconds before
    static double last, avg = 33.3, room_since = -1;
    static bool paused;
    double now = SDL_GetTicksNS() / 1e6;
    if (last > 0 && now - last < 200) avg += (now - last - avg) * 0.05;
    last = now;
    if (!paused && avg > 36.5) paused = true, room_since = -1;
    else if (paused && avg < 34.5) {
        if (room_since < 0) room_since = now;
        else if (now - room_since > 1000) paused = false;
    } else if (paused) {
        room_since = -1;
    }
    bool ok = have && g_have_prev && g_scene_ok && g_depth && g_depth->backend && g_game_interval == 2 && !menu_is_open() && !paused;
    static int logwhy = -1;  // LARECOMP_LOG_FRAMEGEN=1: why frames went without an in-between one, every 2 s
    if (logwhy < 0) logwhy = getenv("LARECOMP_LOG_FRAMEGEN") != NULL;
    if (logwhy) {
        static int n, why[6];
        static double at;
        n++;
        why[0] += !have || !g_have_prev, why[1] += !g_scene_ok, why[2] += !g_depth || !g_depth->backend;
        why[3] += g_game_interval != 2, why[4] += menu_is_open(), why[5] += paused;
        if (now - at > 2000) {
            if (why[0] + why[1] + why[2] + why[3] + why[5])
                LOG("[framegen] of %d frames without: camera %d, scene %d, depth %d, interval %d, menu %d, paused %d (avg %.1f ms)\n", n,
                    why[0], why[1], why[2], why[3], why[4], why[5], avg);
            n = 0, memset(why, 0, sizeof why), at = now;
        }
    }
    if (!ok) g_scene_ok = false, g_depth = NULL;
    g_fin = fin;
    return ok;
}

// device.c vk_frame_end, outside rendering: the real window picture, before it goes to the presentation engine
void fg_copy_window(VkCommandBuffer cb, VkImage sc, VkFormat fmt, uint32_t w, uint32_t h) {
    g_comp_ok = false;
    if (!make_image(&g_comp, fmt, w, h, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) return;
    VkImageMemoryBarrier2 b[2] = {
        {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
         .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
         .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT, .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
         .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, .image = sc, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}},
        {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
         .srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
         .dstAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_GENERAL, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
         .image = g_comp.image, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}}};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 2, .pImageMemoryBarriers = b};
    vkCmdPipelineBarrier2(cb, &di);
    VkImageCopy c = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                     .extent = {w, h, 1}};
    vkCmdCopyImage(cb, sc, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_comp.image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    b[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b[0].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b[0].srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT, b[0].dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b[1].srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT, b[1].dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT;
    vkCmdPipelineBarrier2(cb, &di);
    g_comp_ok = build_mask(cb);
    if (g_comp_ok) build_flow(cb);
}

static VkPipeline fs_pipeline(VkShaderModule fs, VkFormat fmt, VkPipelineLayout layout) {
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vk_builtin_shader("nv12.vert"), .pName = "main"},  // (a fullscreen triangle, uv = position)
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

static VkPipeline warp_pipeline(VkFormat fmt) {
    if (g_pipe && g_pipe_fmt == fmt) return g_pipe;
    if (!g_layout) g_layout = set_layout(6, 128, &g_dsl);
    static VkShaderModule fs;
    if (!fs) fs = vk_compile_glsl(kWarp, STAGE_FRAGMENT, "framegen.frag");
    VkPipeline p = fs ? fs_pipeline(fs, fmt, g_layout) : VK_NULL_HANDLE;
    if (!p) return VK_NULL_HANDLE;
    g_pipe = p, g_pipe_fmt = fmt;
    LOG("[framegen] 60 fps (generated): in-between frames by reprojection\n");
    return p;
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
    vkUpdateDescriptorSets(g_vk.dev, (uint32_t)n, w, 0, NULL);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, NULL);
}

static VkImageView color_view(TexBackend *t) {
    VkComponentMapping ident = {0};
    return vk_image_view(t, VK_IMAGE_VIEW_TYPE_2D, t->format, 0, 1, 0, 1, ident, VK_IMAGE_ASPECT_COLOR_BIT);
}

// the HUD mask (outside any rendering: fg_copy_window)
static bool build_mask(VkCommandBuffer cb) {
    TexBackend *fin = g_fin;
    if (!fin || !fin->image || !g_scene.image) return false;
    uint32_t mw = (fin->width + 3) / 4, mh = (fin->height + 3) / 4;
    if (!make_image(&g_mask, VK_FORMAT_R8_UNORM, mw, mh, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) return false;
    if (!g_mlayout) g_mlayout = set_layout(2, 0, &g_mdsl);
    if (!g_mpipe) {
        VkShaderModule fs = vk_compile_glsl(kMask, STAGE_FRAGMENT, "framegen_mask.frag");
        g_mpipe = fs ? fs_pipeline(fs, VK_FORMAT_R8_UNORM, g_mlayout) : VK_NULL_HANDLE;
        if (!g_mpipe) return false;
    }
    VkImageView mv = color_view(&g_mask), v[2] = {color_view(&g_scene), color_view(fin)};
    if (!mv || !v[0] || !v[1]) return false;
    vk_full_barrier(cb);
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = mv, .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {mw, mh}}, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &ca};
    vkCmdBeginRendering(cb, &ri);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_mpipe);
    static const bool lin[2] = {true, true};
    bind_images(cb, g_mlayout, g_mdsl, 2, v, lin);
    VkViewport vp = {0, 0, (float)mw, (float)mh, 0, 1};
    VkRect2D sc = {{0, 0}, {mw, mh}};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRendering(cb);
    vk_full_barrier(cb);
    return true;
}

// a fullscreen pass of fs into target (outside any rendering); pc: push constants (size 0: none)
static bool run_pass(VkCommandBuffer cb, TexBackend *target, VkPipeline pipe, VkPipelineLayout layout, VkDescriptorSetLayout dsl,
                     int n, VkImageView *v, const bool *linear, const void *pc, uint32_t pcsize) {
    VkImageView tv = color_view(target);
    if (!tv || !pipe) return false;
    for (int i = 0; i < n; i++) if (!v[i]) return false;
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = tv, .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {target->width, target->height}}, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &ca};
    vkCmdBeginRendering(cb, &ri);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    bind_images(cb, layout, dsl, n, v, linear);
    if (pcsize) vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, pcsize, pc);
    VkViewport vp = {0, 0, (float)target->width, (float)target->height, 0, 1};
    VkRect2D sc = {{0, 0}, {target->width, target->height}};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRendering(cb);
    vk_full_barrier(cb);
    return true;
}

// objects' own movement (fg_copy_window, after the HUD mask): this frame's quarter picture, then - with the previous
// frame's there - the block search and its neighbour filter. LARECOMP_FRAMEGEN_FLOW=0: off
static void build_flow(VkCommandBuffer cb) {
    static int on = -1;
    if (on < 0) on = !getenv("LARECOMP_FRAMEGEN_FLOW") || atoi(getenv("LARECOMP_FRAMEGEN_FLOW"));
    g_flow_ok = false, g_zc_ok = false;
    NvnTexture *dt = g_depth;
    if (!g_scene.image || !dt || !dt->backend) { g_q_prev = false; return; }
    {   // the smooth view distance for the warp
        static VkDescriptorSetLayout zdsl;
        static VkPipelineLayout zl;
        static VkPipeline zp;
        if (!zl) zl = set_layout(1, 16, &zdsl);
        if (!zp) {
            VkShaderModule m = vk_compile_glsl(kDepthDown, STAGE_FRAGMENT, "framegen_depth.frag");
            zp = m ? fs_pipeline(m, VK_FORMAT_R16_SFLOAT, zl) : VK_NULL_HANDLE;
        }
        uint32_t zw = (g_scene.width + 7) / 8, zh = (g_scene.height + 7) / 8;
        TexBackend *db = dt->backend;
        VkComponentMapping ident = {0};
        VkImageView dv = vk_image_view(db, VK_IMAGE_VIEW_TYPE_2D, db->format, 0, 1, 0, 1, ident, VK_IMAGE_ASPECT_DEPTH_BIT);
        float zpc[4] = {(float)zw, (float)zh, g_depth_scale[0], g_depth_scale[1]};
        static const bool nn[1] = {false};
        g_zc_ok = zp && make_image(&g_zc, VK_FORMAT_R16_SFLOAT, zw, zh, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) &&
                  run_pass(cb, &g_zc, zp, zl, zdsl, 1, &dv, nn, zpc, sizeof zpc);
    }
    if (!on) { g_q_prev = false; return; }
    static VkDescriptorSetLayout dsl1, dsl3;
    static VkPipelineLayout l1, l3;
    static VkPipeline pdown, pflow, pfilt;
    if (!l1) l1 = set_layout(1, 0, &dsl1), l3 = set_layout(3, 96, &dsl3);
    if (!pdown) {
        VkShaderModule a = vk_compile_glsl(kDown, STAGE_FRAGMENT, "framegen_down.frag"), b = vk_compile_glsl(kFlow, STAGE_FRAGMENT, "framegen_flow.frag"),
                       c = vk_compile_glsl(kFilter, STAGE_FRAGMENT, "framegen_filter.frag");
        if (!a || !b || !c) { on = 0; return; }
        pdown = fs_pipeline(a, VK_FORMAT_R8_UNORM, l1);
        pflow = fs_pipeline(b, VK_FORMAT_R16G16B16A16_SFLOAT, l3);
        pfilt = fs_pipeline(c, VK_FORMAT_R16G16B16A16_SFLOAT, l1);
        if (!pdown || !pflow || !pfilt) { on = 0; return; }
    }
    uint32_t qw = (g_scene.width + 3) / 4, qh = (g_scene.height + 3) / 4, fw = (qw + 3) / 4, fh = (qh + 3) / 4;
    VkImageUsageFlags use = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    g_qi ^= 1;
    TexBackend *cur = &g_q[g_qi], *prev = &g_q[g_qi ^ 1];
    if (prev->width != qw || prev->height != qh) g_q_prev = false;
    if (!make_image(cur, VK_FORMAT_R8_UNORM, qw, qh, use) || !make_image(&g_flow_raw, VK_FORMAT_R16G16B16A16_SFLOAT, fw, fh, use) ||
        !make_image(&g_flow, VK_FORMAT_R16G16B16A16_SFLOAT, fw, fh, use)) { g_q_prev = false; return; }
    static const bool lin1[1] = {true}, nearest1[1] = {false}, lin3[3] = {false, false, false};
    VkImageView sv = color_view(&g_scene);
    bool made = run_pass(cb, cur, pdown, l1, dsl1, 1, &sv, lin1, NULL, 0);
    if (made && g_q_prev && g_have_prev) {
        float inv[16], mp[16];
        if (mat_inv(g_vp, inv)) {
            mat_mul(g_vp_prev, inv, mp);
            extern uint32_t nvn_window_origin(void);
            extern uint32_t nvn_depth_mode(void);
            TexBackend *db = dt->backend;
            VkComponentMapping ident = {0};
            struct { float m[16], a[4], b[4]; } pc = {{0}};
            memcpy(pc.m, mp, sizeof mp);
            pc.a[0] = nvn_window_origin() == 0 ? 1.0f : 0.0f;
            pc.a[1] = nvn_depth_mode() == 0 ? 1.0f : 0.0f;
            pc.b[2] = g_depth_scale[0], pc.b[3] = g_depth_scale[1];
            VkImageView v[3] = {color_view(cur), color_view(prev),
                                vk_image_view(db, VK_IMAGE_VIEW_TYPE_2D, db->format, 0, 1, 0, 1, ident, VK_IMAGE_ASPECT_DEPTH_BIT)};
            VkImageView fv = color_view(&g_flow_raw);
            g_flow_ok = run_pass(cb, &g_flow_raw, pflow, l3, dsl3, 3, v, lin3, &pc, sizeof pc) &&
                        run_pass(cb, &g_flow, pfilt, l1, dsl1, 1, &fv, nearest1, NULL, 0);
        }
    }
    g_q_prev = made;
}

// device.c, inside the in-between frame's rendering into the swapchain image: draws it. fin: the presented picture;
// rect: where it lies in the window (pixels), crop: the part of it shown; flip_present: lower-left origin
bool fg_draw(VkCommandBuffer cb, VkFormat fmt, uint32_t ww, uint32_t wh, TexBackend *fin, const float rect[4], const float crop[4],
             bool flip_present) {
    extern uint32_t nvn_window_origin(void);
    extern uint32_t nvn_depth_mode(void);
    g_scene_ok = false;
    NvnTexture *dt = g_depth;
    g_depth = NULL;
    if (!g_comp_ok || !fin || !fin->image || !dt || !dt->backend) return false;
    TexBackend *db = dt->backend;
    VkPipeline pipe = warp_pipeline(fmt);
    if (!pipe) return false;
    // generated camera: half a step on (rows); M = VPg x inverse(VP)
    float vpg[16], inv[16], m[16];
    static float step = -1;  // LARECOMP_FRAMEGEN_T=<t>: how far on (tests: 1 = where the next real frame should be)
    if (step < 0) step = getenv("LARECOMP_FRAMEGEN_T") ? (float)atof(getenv("LARECOMP_FRAMEGEN_T")) : 0.5f;
    for (int i = 0; i < 16; i++) vpg[i] = g_vp[i] + step * (g_vp[i] - g_vp_prev[i]);
    if (!mat_inv(g_vp, inv)) return false;
    mat_mul(vpg, inv, m);
    VkComponentMapping ident = {0};
    if (!g_zc_ok) return false;
    (void)ident;
    VkImageView v[6] = {color_view(&g_comp), color_view(&g_scene), color_view(fin), color_view(&g_zc), color_view(&g_mask),
                        g_flow_ok ? color_view(&g_flow) : color_view(&g_mask)};
    for (int i = 0; i < 6; i++) if (!v[i]) return false;
    struct { float m[16], rect[4], crop[4], a[4], b[4]; } pc;
    memcpy(pc.m, m, sizeof m);
    pc.rect[0] = rect[0] / ww, pc.rect[1] = rect[1] / wh, pc.rect[2] = rect[2] / ww, pc.rect[3] = rect[3] / wh;
    memcpy(pc.crop, crop, sizeof pc.crop);
    pc.a[0] = nvn_window_origin() == 0 ? 1.0f : 0.0f;
    pc.a[1] = nvn_depth_mode() == 0 ? 1.0f : 0.0f;
    pc.a[2] = flip_present ? 1.0f : 0.0f;
    // a camera that didn't move: the real picture again, without the per-pixel reprojection
    float still = 0;
    for (int i = 0; i < 16; i++) still = fmaxf(still, fabsf(g_vp[i] - g_vp_prev[i]) / (fabsf(g_vp[i]) + 1e-3f));
    pc.a[3] = getenv("LARECOMP_FRAMEGEN_DEBUG") ? 2.0f : getenv("LARECOMP_FRAMEGEN_PLAIN") || still < 1e-5f ? 1.0f : 0.0f;
    static int nlog;
    if (getenv("LARECOMP_FRAMEGEN_DEBUG") && nlog++ < 3)
        LOG("[framegen] depth %ux%u scale %.3f %.3f, picture %ux%u, rect %.0f %.0f %.0f %.0f crop %.3f %.3f %.3f %.3f\n", db->width, db->height,
            g_depth_scale[0], g_depth_scale[1], fin->width, fin->height, rect[0], rect[1], rect[2], rect[3], crop[0], crop[1], crop[2], crop[3]);
    pc.b[0] = g_flow_ok ? 1.0f : 0.0f, pc.b[1] = 0;
    pc.b[2] = g_depth_scale[0], pc.b[3] = g_depth_scale[1];
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    static const bool lin[6] = {true, true, true, true, true, false};
    bind_images(cb, g_layout, g_dsl, 6, v, lin);
    vkCmdPushConstants(cb, g_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
    VkViewport vp = {0, 0, (float)ww, (float)wh, 0, 1};
    VkRect2D sc = {{0, 0}, {ww, wh}};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdDraw(cb, 3, 1, 0, 0);
    return true;
}
