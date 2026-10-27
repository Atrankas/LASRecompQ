// Meta Quest (Horizon OS): the game inside an OpenXR session. See xr_quest.h for the whole picture.
//
// What this file does:
//  - OpenXR instance + session on a Vulkan device of its own (XR_KHR_vulkan_enable2: the runtime wants the device it
//    shows images with to come from it; the game keeps its own device, made as on any phone),
//  - an Android-surface swapchain (XR_KHR_android_surface_swapchain): an ordinary Android Surface the game's Vulkan
//    swapchain presents to, shown by the compositor as a panel in front of the player (device.c create_surface). The
//    panel's pixels per eye come from the VR settings (vr_panel; 1080p by default instead of the window's own size,
//    which on a headset is large and wastes the whole GPU on a picture nobody sees that sharply),
//  - the panel's presentation, per VR mode (vr_mode):
//      FLAT     one quad layer,
//      CURVED   a fan of quads around the player (a curved cinema screen; core OpenXR layers only, so it works on any
//               runtime - it needs no cylinder extension),
//      STEREO   the panel's two halves are the two eyes, each warped by the scene's depth (vr_stereo.c) and shown to
//               its own eye with its own sub-rect: a window with real depth,
//      DIORAMA  the same stereo pair, tilted back and hung low: a model on a virtual table,
//  - the headset's refresh rate (XR_FB_display_refresh_rate) when the settings ask for one,
//  - passthrough behind the panel when the headset offers it (XR_FB_passthrough): mixed reality,
//  - the Touch controllers through OpenXR actions, fed to the game as an SDL virtual gamepad: its menus, its button
//    bindings and its prompts work with them unchanged. The right controller additionally aims a ray at the panel
//    which moves and clicks the app's own menus (menu_vr_pointer, runtime/ui/menu.mm).
// Everything is logged to logcat with the tag LASRecomp-XR.
#ifdef __ANDROID__
#include "xr_quest.h"

#include "../settings.h"
#include <SDL3/SDL.h>
#include <android/log.h>
#include <android/native_window_jni.h>
#include <dlfcn.h>
#include <jni.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <vulkan/vulkan.h>
#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#define XLOG(...) __android_log_print(ANDROID_LOG_INFO, "LASRecomp-XR", __VA_ARGS__)

// ---- the app's menus (runtime/ui/menu.mm): the ray pointer, and whether they are taking input at all
extern bool menu_input_captured(void);
extern void menu_vr_pointer(float u, float v, bool down);  // u/v: the place on the panel (0..1, 0 = left / top)

// ---- poses (a screen or a hand): a position and an orientation, both in the session's LOCAL space (its origin is
// where the player's head was when the session started, +x right, +y up, -z ahead)
typedef struct {
    float x, y, z;
} V3;
typedef struct {
    float x, y, z, w;
} Q4;

static Q4 q_axis(float ax, float ay, float az, float deg) {  // rotation of `deg` degrees about (ax, ay, az)
    float r = deg * (float)M_PI / 360.0f;                    // (half the angle: the quaternion's own units)
    float s = sinf(r);
    return (Q4){ax * s, ay * s, az * s, cosf(r)};
}
static V3 q_rot(Q4 q, V3 v) {  // v turned by q
    V3 t = {2.0f * (q.y * v.z - q.z * v.y), 2.0f * (q.z * v.x - q.x * v.z), 2.0f * (q.x * v.y - q.y * v.x)};
    return (V3){v.x + q.w * t.x + (q.y * t.z - q.z * t.y), v.y + q.w * t.y + (q.z * t.x - q.x * t.z),
                v.z + q.w * t.z + (q.x * t.y - q.y * t.x)};
}
static V3 q_unrot(Q4 q, V3 v) { return q_rot((Q4){-q.x, -q.y, -q.z, q.w}, v); }  // v turned back (the inverse)

// ---- the panel's pixels (per eye): the presets, or the window's own size for VR_PANEL_NATIVE
static void panel_pixels(uint32_t *ew, uint32_t *eh, int win_w, int win_h) {
    switch (g_settings.vr_panel) {
    case VR_PANEL_720: *ew = 1280, *eh = 720; break;
    case VR_PANEL_1440: *ew = 2560, *eh = 1440; break;
    case VR_PANEL_NATIVE:
        *ew = (uint32_t)(win_w > 0 ? win_w : 1920), *eh = (uint32_t)(win_h > 0 ? win_h : 1080);
        if (*ew > 3840) *ew = 3840;  // (a headset window can be 4K wide and portrait: the panel never goes past this)
        if (*eh > 2160) *eh = 2160;
        break;
    default: *ew = 1920, *eh = 1080; break;
    }
    if (*ew < 320) *ew = 320;
    if (*eh < 240) *eh = 240;
}

// The panel's place and size for a mode, in metres (the layer's pose and size; the stereo modes' halves use the same
// pose, each with its own half of the picture). y0 sinks the panel as it leans back, so a leaning diorama hangs low
// like a table instead of in front of the player's face. arc: the curved mode's (radius, segment angle in radians).
typedef struct {
    V3 pos;
    Q4 rot;
    float w, h;    // the size of one layer, metres
    float radius;  // curved: the cylinder's radius; 0 otherwise
    float step;    // curved: the angle of one segment, radians
} Panel;

static Panel panel_place(bool stereo, float aspect) {
    float dist = g_settings.vr_distance > 0.2f ? g_settings.vr_distance : 2.2f;
    float width = g_settings.vr_size > 0.2f ? g_settings.vr_size : 3.2f;
    float tilt = g_settings.vr_tilt;
    if (tilt > 89.0f) tilt = 89.0f;
    if (tilt < 0.0f) tilt = 0.0f;
    Panel p = {0};
    p.rot = q_axis(1, 0, 0, -tilt);  // lean back: the panel's top edge goes away from the player (about +x)
    p.pos.y = -0.02f - 0.42f * sinf(tilt * (float)M_PI / 180.0f);
    if (g_settings.vr_mode == VR_MODE_CURVED && !stereo) {
        // a cylinder around the player: the angle is the arc it covers, its radius follows from the arc's length
        float deg = g_settings.vr_curve;
        if (deg < 20.0f) deg = 20.0f;
        if (deg > 160.0f) deg = 160.0f;
        float arc = deg * (float)M_PI / 180.0f;
        p.radius = width / arc;
        p.step = arc / (float)XR_QUEST_ARC_SEGMENTS;
        p.pos.z = 0.0f;  // (the axis sits at the player: the surface is `radius` ahead of them)
        p.w = 2.0f * p.radius * sinf(p.step * 0.5f);  // the chord of one segment
        p.h = p.w * aspect;
    } else {
        p.pos.z = -dist;
        p.w = width;
        p.h = width * aspect;
    }
    return p;
}

// ---- the session
static PFN_xrGetVulkanGraphicsRequirements2KHR p_xrGetVulkanGraphicsRequirements2KHR;
static PFN_xrCreateVulkanInstanceKHR p_xrCreateVulkanInstanceKHR;
static PFN_xrGetVulkanGraphicsDevice2KHR p_xrGetVulkanGraphicsDevice2KHR;
static PFN_xrCreateVulkanDeviceKHR p_xrCreateVulkanDeviceKHR;
static PFN_xrCreateSwapchainAndroidSurfaceKHR p_xrCreateSwapchainAndroidSurfaceKHR;
static PFN_xrCreatePassthroughFB p_xrCreatePassthroughFB;
static PFN_xrCreatePassthroughLayerFB p_xrCreatePassthroughLayerFB;
static PFN_xrEnumerateDisplayRefreshRatesFB p_xrEnumerateDisplayRefreshRatesFB;
static PFN_xrRequestDisplayRefreshRateFB p_xrRequestDisplayRefreshRateFB;

static struct {
    pthread_mutex_t lock;
    bool tried, ok;
    JavaVM *vm;
    jobject activity;
    XrInstance inst;
    XrSystemId sys;
    XrSession sess;
    XrSpace space;
    XrSwapchain sc;
    jobject surf;
    struct ANativeWindow *win;
    uint32_t w, h;    // the panel the swapchain was made with (both eyes' halves together)
    uint32_t ew, eh;  // one eye's half
    bool stereo;
    uint32_t pw, ph;  // the size the swapchain really got (device.c xr_quest_note_panel); 0: as asked for
    void *vklib;
    PFN_vkGetInstanceProcAddr gipa;
    VkInstance vki;
    VkPhysicalDevice vkp;
    VkDevice vkd;
    uint32_t qfam;
    bool has_pt;
    XrPassthroughFB pt;
    XrPassthroughLayerFB ptl;
    bool rr;
    XrActionSet aset;
    XrAction a_move, a_look, a_a, a_b, a_x, a_y, a_thumbl, a_thumbr, a_menu, a_trig_l, a_trig_r, a_sq_l, a_sq_r, a_aim;
    XrSpace aim_space;  // (an action space is an XrSpace: xrCreateActionSpace, OpenXR 1.0)
    atomic_bool quit, running, focused;
    pthread_t th;
} X = {.lock = PTHREAD_MUTEX_INITIALIZER};

// ---- what the rest of the app asks about the VR side (runtime/nvn/vulkan/vr_stereo.c, framegen.c, device.c,
//      runtime/ui/menu.mm). X is declared above: these have to follow it.
bool vr_stereo_on(void) {
    if (!xr_quest_requested() || !X.ok) return false;  // (no OpenXR: the picture is an ordinary window, one eye only)
    return g_settings.vr_mode == VR_MODE_STEREO || g_settings.vr_mode == VR_MODE_DIORAMA;
}

void vr_stereo_split_panel(double *w, double *h) {
    (void)h;
    if (vr_stereo_on() && *w > 8.0) *w *= 0.5;
}

void vr_mode_preset(int mode) {
    switch (mode) {
    case VR_MODE_CURVED: g_settings.vr_distance = 2.4f, g_settings.vr_size = 3.6f, g_settings.vr_curve = 80.0f,
                         g_settings.vr_tilt = 0.0f, g_settings.vr_depth = 100.0f; break;
    case VR_MODE_STEREO: g_settings.vr_distance = 1.7f, g_settings.vr_size = 2.0f, g_settings.vr_tilt = 0.0f,
                         g_settings.vr_depth = 100.0f; break;
    case VR_MODE_DIORAMA: g_settings.vr_distance = 1.5f, g_settings.vr_size = 1.8f, g_settings.vr_tilt = 42.0f,
                          g_settings.vr_depth = 160.0f; break;
    default: g_settings.vr_distance = 2.2f, g_settings.vr_size = 3.2f, g_settings.vr_tilt = 0.0f,
             g_settings.vr_depth = 100.0f; break;
    }
    XLOG("mode %d: %.2f m away, %.2f m wide, tilt %.0f, stereo %+.0f%%", mode, g_settings.vr_distance,
         g_settings.vr_size, g_settings.vr_tilt, g_settings.vr_depth);
}

bool xr_quest_requested(void) {
    const char *e = getenv("LARECOMP_XR");
    return e && atoi(e) != 0;
}
bool xr_quest_active(void) { return X.ok; }
bool xr_quest_extent(uint32_t *w, uint32_t *h) {
    if (!X.ok) return false;
    *w = X.w, *h = X.h;
    return true;
}
void xr_quest_note_panel(uint32_t w, uint32_t h) {
    if (!w || !h) return;
    if (w != X.pw || h != X.ph) XLOG("panel: the swapchain got %ux%u (asked for %ux%u)", w, h, X.w, X.h);
    X.pw = w, X.ph = h;
}

#define TRY(call, what) \
    do { \
        XrResult r_ = (call); \
        if (XR_FAILED(r_)) { XLOG("%s failed: %d", what, (int)r_); return false; } \
    } while (0)

static bool has_ext(const XrExtensionProperties *p, uint32_t n, const char *name) {
    for (uint32_t i = 0; i < n; i++)
        if (!strcmp(p[i].extensionName, name)) return true;
    return false;
}

static XrPath path_of(const char *s) {
    XrPath p = XR_NULL_PATH;
    xrStringToPath(X.inst, s, &p);
    return p;
}

static XrAction make_action(const char *name, XrActionType type) {
    XrActionCreateInfo ai = {XR_TYPE_ACTION_CREATE_INFO};
    ai.actionType = type;
    strncpy(ai.actionName, name, XR_MAX_ACTION_NAME_SIZE - 1);
    strncpy(ai.localizedActionName, name, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    XrAction a = XR_NULL_HANDLE;
    if (XR_FAILED(xrCreateAction(X.aset, &ai, &a))) XLOG("xrCreateAction %s failed", name);
    return a;
}

// ---- the Touch controllers' actions: one action set, one suggested binding each. The mapping onto the game's own
// controls is in pad_update below.
static bool make_actions(void) {
    XrActionSetCreateInfo si = {XR_TYPE_ACTION_SET_CREATE_INFO};
    strcpy(si.actionSetName, "gameplay");
    strcpy(si.localizedActionSetName, "Gameplay");
    TRY(xrCreateActionSet(X.inst, &si, &X.aset), "xrCreateActionSet");
    X.a_move = make_action("move", XR_ACTION_TYPE_VECTOR2F_INPUT);
    X.a_look = make_action("look", XR_ACTION_TYPE_VECTOR2F_INPUT);
    X.a_a = make_action("button_a", XR_ACTION_TYPE_BOOLEAN_INPUT);
    X.a_b = make_action("button_b", XR_ACTION_TYPE_BOOLEAN_INPUT);
    X.a_x = make_action("button_x", XR_ACTION_TYPE_BOOLEAN_INPUT);
    X.a_y = make_action("button_y", XR_ACTION_TYPE_BOOLEAN_INPUT);
    X.a_thumbl = make_action("thumb_left", XR_ACTION_TYPE_BOOLEAN_INPUT);
    X.a_thumbr = make_action("thumb_right", XR_ACTION_TYPE_BOOLEAN_INPUT);
    X.a_menu = make_action("menu", XR_ACTION_TYPE_BOOLEAN_INPUT);
    X.a_trig_l = make_action("trigger_left", XR_ACTION_TYPE_FLOAT_INPUT);
    X.a_trig_r = make_action("trigger_right", XR_ACTION_TYPE_FLOAT_INPUT);
    X.a_sq_l = make_action("grip_left", XR_ACTION_TYPE_FLOAT_INPUT);
    X.a_sq_r = make_action("grip_right", XR_ACTION_TYPE_FLOAT_INPUT);
    X.a_aim = make_action("aim", XR_ACTION_TYPE_POSE_INPUT);

    const struct { XrAction a; const char *path; } map[] = {
        {X.a_move, "/user/hand/left/input/thumbstick"},         {X.a_look, "/user/hand/right/input/thumbstick"},
        {X.a_a, "/user/hand/right/input/a/click"},              {X.a_b, "/user/hand/right/input/b/click"},
        {X.a_x, "/user/hand/left/input/x/click"},               {X.a_y, "/user/hand/left/input/y/click"},
        {X.a_thumbl, "/user/hand/left/input/thumbstick/click"}, {X.a_thumbr, "/user/hand/right/input/thumbstick/click"},
        {X.a_menu, "/user/hand/left/input/menu/click"},
        {X.a_trig_l, "/user/hand/left/input/trigger/value"},    {X.a_trig_r, "/user/hand/right/input/trigger/value"},
        {X.a_sq_l, "/user/hand/left/input/squeeze/value"},      {X.a_sq_r, "/user/hand/right/input/squeeze/value"},
        {X.a_aim, "/user/hand/right/input/aim/pose"},
    };
    XrActionSuggestedBinding b[sizeof map / sizeof map[0]];
    uint32_t n = 0;
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
        if (map[i].a) b[n].action = map[i].a, b[n].binding = path_of(map[i].path), n++;
    XrInteractionProfileSuggestedBinding sb = {XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    sb.interactionProfile = path_of("/interaction_profiles/oculus/touch_controller");
    sb.countSuggestedBindings = n;
    sb.suggestedBindings = b;
    TRY(xrSuggestInteractionProfileBindings(X.inst, &sb), "xrSuggestInteractionProfileBindings");
    XrSessionActionSetsAttachInfo ai = {XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    ai.countActionSets = 1;
    ai.actionSets = &X.aset;
    TRY(xrAttachSessionActionSets(X.sess, &ai), "xrAttachSessionActionSets");

    if (X.a_aim) {  // the right controller's ray, for the app's menus
        XrActionSpaceCreateInfo asci = {XR_TYPE_ACTION_SPACE_CREATE_INFO};
        asci.action = X.a_aim;
        asci.subactionPath = path_of("/user/hand/right");
        asci.poseInActionSpace.orientation.w = 1.f;
        if (XR_FAILED(xrCreateActionSpace(X.sess, &asci, &X.aim_space))) XLOG("xrCreateActionSpace (aim) failed");
    }
    return true;
}

// ---- the Touch controllers as an SDL virtual gamepad. Standard layout: the game's own mapping, menus and rebinding
// see an ordinary Xbox-style pad (the vendor / product ids are an Xbox One pad's, which input.c ranks above the half
// pad the headset's own Android input layer lists).
//
//   left stick    movement (L)                  A             the game's A (bottom: confirm / talk / lift)
//   right stick   the game's d-pad (also the app's menu up / down / left / right)
//   grips (L/R)   the game's L / R  (shield, Pegasus Boots)
//   triggers      the game's ZL / ZR (analog)   B / X / Y     the game's B (right: sword), X, Y
//   left stick click   the game's Minus (-): map      right stick click   the app's settings menu
//   menu button        the game's Plus (+): items     menu + left stick click   the app's settings menu too
//   The right trigger is also the menus' click (the ray pointer below), which is only felt while a menu is open (the
//   game gets no input then: runtime/ui/menu.mm menu_input_captured).
static SDL_Joystick *g_vj;

static void pad_ensure(void) {
    if (g_vj) return;
    if (!(SDL_WasInit(0) & SDL_INIT_JOYSTICK)) return;  // (the game starts SDL a little after us)
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");  // (the VR shell, not the app's window, has the focus)
    SDL_VirtualJoystickDesc d;
    SDL_INIT_INTERFACE(&d);
    d.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    d.vendor_id = 0x045e;
    d.product_id = 0x02ea;
    d.naxes = SDL_GAMEPAD_AXIS_COUNT;
    d.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    d.name = "Meta Quest Touch controllers";
    d.button_mask = 0;
    for (int i = SDL_GAMEPAD_BUTTON_SOUTH; i <= SDL_GAMEPAD_BUTTON_DPAD_RIGHT; i++) d.button_mask |= 1u << i;
    d.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1;
    SDL_JoystickID id = SDL_AttachVirtualJoystick(&d);
    if (!id) { XLOG("virtual gamepad: %s", SDL_GetError()); return; }
    g_vj = SDL_OpenJoystick(id);
    XLOG("virtual gamepad %u: %s (gamepad: %d)", (unsigned)id, g_vj ? "open" : SDL_GetError(), SDL_IsGamepad(id));
}

static float get_f(XrAction a) {
    XrActionStateGetInfo gi = {XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = a;
    XrActionStateFloat s = {XR_TYPE_ACTION_STATE_FLOAT};
    return a && XR_SUCCEEDED(xrGetActionStateFloat(X.sess, &gi, &s)) && s.isActive ? s.currentState : 0.f;
}
static bool get_b(XrAction a) {
    XrActionStateGetInfo gi = {XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = a;
    XrActionStateBoolean s = {XR_TYPE_ACTION_STATE_BOOLEAN};
    return a && XR_SUCCEEDED(xrGetActionStateBoolean(X.sess, &gi, &s)) && s.isActive && s.currentState;
}
static void get_v2(XrAction a, float *x, float *y) {
    XrActionStateGetInfo gi = {XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = a;
    XrActionStateVector2f s = {XR_TYPE_ACTION_STATE_VECTOR2F};
    *x = *y = 0.f;
    if (a && XR_SUCCEEDED(xrGetActionStateVector2f(X.sess, &gi, &s)) && s.isActive) *x = s.currentState.x, *y = s.currentState.y;
}

// A stick's deadzone, radial (the sticks rest a little off centre; SDL leaves a virtual joystick's values alone)
static void deadzone(float *x, float *y) {
    const float dz = 0.15f, rest = 0.94f;
    float m = sqrtf(*x * *x + *y * *y);
    if (m <= dz) { *x = *y = 0.f; return; }
    float k = fminf(1.f, (m - dz) / (rest * (1.f - dz)));
    *x = *x / m * k, *y = *y / m * k;
}

static bool g_pb[SDL_GAMEPAD_BUTTON_COUNT];
static Sint16 g_pa[SDL_GAMEPAD_AXIS_COUNT];

static void pad_set(const bool *nb, const Sint16 *na) {
    for (int i = 0; i < SDL_GAMEPAD_BUTTON_COUNT; i++)
        if (nb[i] != g_pb[i]) g_pb[i] = nb[i], SDL_SetJoystickVirtualButton(g_vj, i, nb[i]);
    for (int i = 0; i < SDL_GAMEPAD_AXIS_COUNT; i++)
        if (na[i] != g_pa[i]) g_pa[i] = na[i], SDL_SetJoystickVirtualAxis(g_vj, i, na[i]);
}

static void pad_release(void) {
    if (!g_vj) return;
    bool nb[SDL_GAMEPAD_BUTTON_COUNT] = {0};
    Sint16 na[SDL_GAMEPAD_AXIS_COUNT] = {0};
    pad_set(nb, na);
}

static float g_trig_r;  // (the menus' click: pad_update notes it)

static void pad_update(void) {
    pad_ensure();
    if (!g_vj) return;
    XrActiveActionSet as = {X.aset, XR_NULL_PATH};
    XrActionsSyncInfo si = {XR_TYPE_ACTIONS_SYNC_INFO};
    si.countActiveActionSets = 1;
    si.activeActionSets = &as;
    if (XR_FAILED(xrSyncActions(X.sess, &si))) return;
    static bool grip_l, grip_r;
    float v = get_f(X.a_sq_l);
    if (v > 0.7f) grip_l = true; else if (v < 0.4f) grip_l = false;
    v = get_f(X.a_sq_r);
    if (v > 0.7f) grip_r = true; else if (v < 0.4f) grip_r = false;
    float x, y, rx, ry;
    get_v2(X.a_move, &x, &y);
    deadzone(&x, &y);
    get_v2(X.a_look, &rx, &ry);
    deadzone(&rx, &ry);
    bool nb[SDL_GAMEPAD_BUTTON_COUNT] = {0};
    nb[SDL_GAMEPAD_BUTTON_SOUTH] = get_b(X.a_a);
    nb[SDL_GAMEPAD_BUTTON_EAST] = get_b(X.a_b);
    nb[SDL_GAMEPAD_BUTTON_WEST] = get_b(X.a_x);
    nb[SDL_GAMEPAD_BUTTON_NORTH] = get_b(X.a_y);
    nb[SDL_GAMEPAD_BUTTON_START] = get_b(X.a_menu);       // the game's Plus (the pause / items menu)
    nb[SDL_GAMEPAD_BUTTON_BACK] = get_b(X.a_thumbl);      // the game's Minus (the map)
    nb[SDL_GAMEPAD_BUTTON_GUIDE] = get_b(X.a_thumbr);     // our own settings menu (runtime/ui/menu.mm)
    nb[SDL_GAMEPAD_BUTTON_LEFT_SHOULDER] = grip_l;        // the game's L
    nb[SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER] = grip_r;       // the game's R
    // the right stick is the d-pad: the game's menus (file select, item boxes) and our own menu rows both navigate
    // with it, and no game control of this one is lost (the d-pad moves Link only with the dpad_move setting)
    nb[SDL_GAMEPAD_BUTTON_DPAD_LEFT] = rx < -0.45f;
    nb[SDL_GAMEPAD_BUTTON_DPAD_RIGHT] = rx > 0.45f;
    nb[SDL_GAMEPAD_BUTTON_DPAD_UP] = ry > 0.45f;
    nb[SDL_GAMEPAD_BUTTON_DPAD_DOWN] = ry < -0.45f;
    Sint16 na[SDL_GAMEPAD_AXIS_COUNT] = {0};
    na[SDL_GAMEPAD_AXIS_LEFTX] = (Sint16)(x * 32767.f), na[SDL_GAMEPAD_AXIS_LEFTY] = (Sint16)(-y * 32767.f);
    na[SDL_GAMEPAD_AXIS_RIGHTX] = (Sint16)(rx * 32767.f), na[SDL_GAMEPAD_AXIS_RIGHTY] = (Sint16)(-ry * 32767.f);
    float tl = get_f(X.a_trig_l), tr = get_f(X.a_trig_r);
    na[SDL_GAMEPAD_AXIS_LEFT_TRIGGER] = (Sint16)(tl * 32767.f);
    na[SDL_GAMEPAD_AXIS_RIGHT_TRIGGER] = (Sint16)(tr * 32767.f);
    g_trig_r = tr;
    pad_set(nb, na);
}

// ---- the ray pointer (the app's menus): the right controller's aim ray against the panel's plane, in the panel's
// pixels. Only while a menu takes input (menu_input_captured): the events would otherwise pile up in the menu's queue,
// and the game's own input must not be disturbed. Click = the right trigger (the menus take the game's input away
// while they are open, so the same trigger cannot do two things at once).
static void pointer_update(XrTime time) {
    static bool down, have;
    static float last[2];
    if (!X.aim_space || !X.a_aim) return;
    float tr = g_trig_r;
    bool inside = false;
    float px = 0, py = 0;
    if (menu_input_captured()) {
        XrSpaceLocation loc = {XR_TYPE_SPACE_LOCATION};
        if (XR_SUCCEEDED(xrLocateSpace(X.aim_space, X.space, time, &loc))) {
            const uint32_t ok = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
            if ((loc.locationFlags & ok) == ok) {
                const XrPosef *q = &loc.pose;
                V3 o = {q->position.x, q->position.y, q->position.z};
                V3 d = q_rot((Q4){q->orientation.x, q->orientation.y, q->orientation.z, q->orientation.w}, (V3){0, 0, -1});
                // the panel's plane (the curved mode's fan is approximated by its middle segment's plane: fine at its
                // centre, a few centimetres out at its edges)
                float aspect = (float)X.eh / (float)(X.ew ? X.ew : 1);
                Panel p = panel_place(X.stereo, aspect);
                V3 n = q_rot(p.rot, (V3){0, 0, 1});
                float den = n.x * d.x + n.y * d.y + n.z * d.z;
                if (fabsf(den) > 1e-4f) {
                    float t = (n.x * (p.pos.x - o.x) + n.y * (p.pos.y - o.y) + n.z * (p.pos.z - o.z)) / den;
                    if (t > 0.1f && t < 30.f) {
                        V3 hit = {o.x + d.x * t, o.y + d.y * t, o.z + d.z * t};
                        V3 l = q_unrot(p.rot, (V3){hit.x - p.pos.x, hit.y - p.pos.y, hit.z - p.pos.z});
                        float u = 0.5f + l.x / p.w, v = 0.5f - l.y / p.h;
                        inside = u > -0.02f && u < 1.02f && v > -0.02f && v < 1.02f;
                        // the menus live on one eye's half of the picture: a place on either half is the same pixel
                        float half = X.stereo ? (u < 0.5f ? u * 2.f : (u - 0.5f) * 2.f) : u;
                        float pww = (float)(X.pw ? X.pw : X.w), phh = (float)(X.ph ? X.ph : X.h);
                        px = fminf(fmaxf(half, 0.f), 1.f) * (X.stereo ? pww * 0.5f : pww);
                        py = fminf(fmaxf(v, 0.f), 1.f) * phh;
                    }
                }
            }
        }
    }
    if (inside) have = true, last[0] = px, last[1] = py;
    bool want = inside && (down ? tr > 0.4f : tr > 0.6f);  // (press over 0.6, release under 0.4: no flutter)
    if (!menu_input_captured()) {
        if (down) down = false, menu_vr_pointer(last[0], last[1], false);
        have = false;
        return;
    }
    down = want;
    if (have) menu_vr_pointer(last[0], last[1], down);
}

// ---- the frame loop (its own thread: xrWaitFrame paces it at the headset's refresh rate)
static void push_sdl(SDL_EventType t) {
    SDL_Event e;
    SDL_zero(e);
    e.type = t;
    SDL_PushEvent(&e);
}

static void request_refresh_rate(void) {
    int want = g_settings.vr_refresh;
    if (want <= 0 || !X.rr) return;
    uint32_t n = 0;
    if (XR_FAILED(p_xrEnumerateDisplayRefreshRatesFB(X.sess, 0, &n, NULL)) || !n) return;
    float rates[16];
    if (n > 16) n = 16;
    if (XR_FAILED(p_xrEnumerateDisplayRefreshRatesFB(X.sess, n, &n, rates))) return;
    float best = rates[0];
    for (uint32_t i = 1; i < n; i++)
        if (fabsf(rates[i] - (float)want) < fabsf(best - (float)want)) best = rates[i];
    XrResult r = p_xrRequestDisplayRefreshRateFB(X.sess, best);
    XLOG("%d Hz asked for: %.0f Hz %s (%u offered)", want, best, XR_SUCCEEDED(r) ? "set" : "refused", n);
}

static void handle_events(void) {
    XrEventDataBuffer ev = {XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(X.inst, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const XrEventDataSessionStateChanged *s = (const XrEventDataSessionStateChanged *)&ev;
            XLOG("session state %d", (int)s->state);
            switch (s->state) {
            case XR_SESSION_STATE_READY: {
                XrSessionBeginInfo bi = {XR_TYPE_SESSION_BEGIN_INFO};
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                if (XR_SUCCEEDED(xrBeginSession(X.sess, &bi))) {
                    atomic_store(&X.running, true);
                    request_refresh_rate();
                    push_sdl(SDL_EVENT_DID_ENTER_FOREGROUND);  // (device.c: a new surface after a pause)
                } else XLOG("xrBeginSession failed");
                break;
            }
            case XR_SESSION_STATE_STOPPING:
                // the game stops presenting (device.c lifecycle_watch) before the surface's session ends
                push_sdl(SDL_EVENT_WILL_ENTER_BACKGROUND);
                usleep(150000);
                atomic_store(&X.running, false);
                xrEndSession(X.sess);
                pad_release();
                break;
            case XR_SESSION_STATE_FOCUSED: atomic_store(&X.focused, true); break;
            case XR_SESSION_STATE_VISIBLE:
            case XR_SESSION_STATE_SYNCHRONIZED:
                atomic_store(&X.focused, false);
                pad_release();
                break;
            case XR_SESSION_STATE_EXITING:
            case XR_SESSION_STATE_LOSS_PENDING:
                atomic_store(&X.quit, true);
                push_sdl(SDL_EVENT_QUIT);
                break;
            default: break;
            }
        }
        ev = (XrEventDataBuffer){XR_TYPE_EVENT_DATA_BUFFER};
    }
}

// The layers of one frame. One quad for FLAT, one quad per segment for CURVED, and one per eye (each half of the
// picture, its own eye) for the stereo modes. out[]: the layer headers, in back to front order.
static void build_layers(const XrFrameState *fs, const XrCompositionLayerBaseHeader **out, uint32_t *nout,
                         XrCompositionLayerQuad *quads, XrCompositionLayerPassthroughFB *pt) {
    uint32_t n = 0;
    if (X.has_pt) {
        *pt = (XrCompositionLayerPassthroughFB){XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB};
        pt->flags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        pt->layerHandle = X.ptl;
        out[n++] = (const XrCompositionLayerBaseHeader *)pt;
    }
    if (!fs->shouldRender) { *nout = n; return; }
    // the panel's real pixels (device.c may have got another size than the one asked for)
    uint32_t pw = X.pw ? X.pw : X.w, ph = X.ph ? X.ph : X.h;
    bool stereo = X.stereo;
    float aspect = stereo ? (float)ph / (float)(pw / 2) : (float)ph / (float)pw;  // (h/w: one layer's own shape)
    Panel p = panel_place(stereo, aspect);
    uint32_t nseg = (!stereo && g_settings.vr_mode == VR_MODE_CURVED) ? XR_QUEST_ARC_SEGMENTS : 1;
    if (nseg > XR_QUEST_ARC_SEGMENTS) nseg = XR_QUEST_ARC_SEGMENTS;
    for (uint32_t i = 0; i < nseg && n < 8; i++) {
        XrCompositionLayerQuad *q = &quads[i];
        *q = (XrCompositionLayerQuad){XR_TYPE_COMPOSITION_LAYER_QUAD};
        q->space = X.space;
        q->eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        q->subImage.swapchain = X.sc;
        q->subImage.imageArrayIndex = 0;
        if (stereo) {
            // one eye per half: the left half to the left eye, the right one to the right eye. Each keeps the same
            // pose and size (each half has the other's aspect: the picture is not squeezed).
            uint32_t hw = pw / 2;
            q->subImage.imageRect.offset = (XrOffset2Di){i ? (int32_t)hw : 0, 0};
            q->subImage.imageRect.extent = (XrExtent2Di){i ? (int32_t)(pw - hw) : (int32_t)hw, (int32_t)ph};
            q->eyeVisibility = i ? XR_EYE_VISIBILITY_RIGHT : XR_EYE_VISIBILITY_LEFT;
            q->pose.orientation = (XrQuaternionf){p.rot.x, p.rot.y, p.rot.z, p.rot.w};
            q->pose.position = (XrVector3f){p.pos.x, p.pos.y, p.pos.z};
            q->size = (XrExtent2Df){p.w, p.h};
        } else if (nseg > 1) {
            // the curved screen: one segment of the arc each, its own slice of the picture
            float a = -0.5f * p.step * (float)nseg + p.step * ((float)i + 0.5f);  // this segment's middle angle
            float u0 = (float)i / (float)nseg;
            q->subImage.imageRect.offset = (XrOffset2Di){(int32_t)(u0 * (float)pw), 0};
            q->subImage.imageRect.extent = (XrExtent2Di){(int32_t)((float)pw / (float)nseg), (int32_t)ph};
            q->pose.orientation = (XrQuaternionf){0.f, sinf(-a * 0.5f), 0.f, cosf(-a * 0.5f)};  // about +y by -a
            q->pose.position = (XrVector3f){p.radius * sinf(a), p.pos.y, -p.radius * cosf(a)};
            q->size = (XrExtent2Df){p.w, p.h};
        } else {
            q->subImage.imageRect.offset = (XrOffset2Di){0, 0};
            q->subImage.imageRect.extent = (XrExtent2Di){(int32_t)pw, (int32_t)ph};
            q->pose.orientation = (XrQuaternionf){p.rot.x, p.rot.y, p.rot.z, p.rot.w};
            q->pose.position = (XrVector3f){p.pos.x, p.pos.y, p.pos.z};
            q->size = (XrExtent2Df){p.w, p.h};
        }
        out[n++] = (const XrCompositionLayerBaseHeader *)q;
    }
    *nout = n;
}

static void *xr_thread(void *arg) {
    (void)arg;
    JNIEnv *env = NULL;
    (*X.vm)->AttachCurrentThread(X.vm, (void **)&env, NULL);
    XLOG("frame loop started");
    static XrCompositionLayerQuad quads[XR_QUEST_ARC_SEGMENTS];
    static XrCompositionLayerPassthroughFB ptl;
    while (!atomic_load(&X.quit)) {
        handle_events();
        if (!atomic_load(&X.running)) { usleep(20000); continue; }
        XrFrameState fs = {XR_TYPE_FRAME_STATE};
        if (XR_FAILED(xrWaitFrame(X.sess, NULL, &fs))) { usleep(20000); continue; }
        if (XR_FAILED(xrBeginFrame(X.sess, NULL))) continue;
        bool focused = atomic_load(&X.focused);
        if (focused) pad_update();
        pointer_update(fs.predictedDisplayTime);

        const XrCompositionLayerBaseHeader *layers[8];
        uint32_t nl = 0;
        build_layers(&fs, layers, &nl, quads, &ptl);
        XrFrameEndInfo ei = {XR_TYPE_FRAME_END_INFO};
        ei.displayTime = fs.predictedDisplayTime;
        ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        ei.layerCount = nl;
        ei.layers = layers;
        xrEndFrame(X.sess, &ei);
    }
    (*X.vm)->DetachCurrentThread(X.vm);
    return NULL;
}

// ---- start-up
static bool xr_start(int win_w, int win_h) {
    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    jobject act = (jobject)SDL_GetAndroidActivity();
    if (!env || !act) { XLOG("no JNI environment / activity"); return false; }
    (*env)->GetJavaVM(env, &X.vm);
    X.activity = (*env)->NewGlobalRef(env, act);
    (*env)->DeleteLocalRef(env, act);

    PFN_xrInitializeLoaderKHR init_loader = NULL;
    TRY(xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR", (PFN_xrVoidFunction *)&init_loader), "xrInitializeLoaderKHR lookup");
    XrLoaderInitInfoAndroidKHR li = {XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
    li.applicationVM = X.vm;
    li.applicationContext = X.activity;
    TRY(init_loader((const XrLoaderInitInfoBaseHeaderKHR *)&li), "xrInitializeLoaderKHR");

    uint32_t ne = 0;
    TRY(xrEnumerateInstanceExtensionProperties(NULL, 0, &ne, NULL), "extension count");
    XrExtensionProperties *ep = calloc(ne ? ne : 1, sizeof *ep);
    for (uint32_t i = 0; i < ne; i++) ep[i].type = XR_TYPE_EXTENSION_PROPERTIES;
    xrEnumerateInstanceExtensionProperties(NULL, ne, &ne, ep);
    const char *need[] = {XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME,
                          XR_KHR_ANDROID_SURFACE_SWAPCHAIN_EXTENSION_NAME};
    const char *enabled[8];
    uint32_t nen = 0;
    for (size_t i = 0; i < sizeof need / sizeof need[0]; i++) {
        if (!has_ext(ep, ne, need[i])) { XLOG("the runtime lacks %s", need[i]); free(ep); return false; }
        enabled[nen++] = need[i];
    }
    bool fb_surf = has_ext(ep, ne, XR_FB_ANDROID_SURFACE_SWAPCHAIN_CREATE_EXTENSION_NAME);
    if (fb_surf) enabled[nen++] = XR_FB_ANDROID_SURFACE_SWAPCHAIN_CREATE_EXTENSION_NAME;
    X.rr = has_ext(ep, ne, XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
    if (X.rr) enabled[nen++] = XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME;
    const char *pt_env = getenv("LARECOMP_XR_PASSTHROUGH");
    bool want_pt = !pt_env || atoi(pt_env) != 0;
    X.has_pt = want_pt && has_ext(ep, ne, XR_FB_PASSTHROUGH_EXTENSION_NAME);
    if (X.has_pt) enabled[nen++] = XR_FB_PASSTHROUGH_EXTENSION_NAME;
    free(ep);
    XLOG("extensions: surface-create-FB %d, passthrough %d, refresh rate %d", fb_surf, X.has_pt, X.rr);

    XrInstanceCreateInfoAndroidKHR ai = {XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
    ai.applicationVM = X.vm;
    ai.applicationActivity = X.activity;
    XrInstanceCreateInfo ci = {XR_TYPE_INSTANCE_CREATE_INFO};
    ci.next = &ai;
    strcpy(ci.applicationInfo.applicationName, "LAS Recomp VR");
    ci.applicationInfo.applicationVersion = 1;
    strcpy(ci.applicationInfo.engineName, "LASRecomp");
    ci.applicationInfo.engineVersion = 1;
    ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ci.enabledExtensionCount = nen;
    ci.enabledExtensionNames = enabled;
    TRY(xrCreateInstance(&ci, &X.inst), "xrCreateInstance");

#define LOADFN(n) xrGetInstanceProcAddr(X.inst, #n, (PFN_xrVoidFunction *)&p_##n)
    LOADFN(xrGetVulkanGraphicsRequirements2KHR);
    LOADFN(xrCreateVulkanInstanceKHR);
    LOADFN(xrGetVulkanGraphicsDevice2KHR);
    LOADFN(xrCreateVulkanDeviceKHR);
    LOADFN(xrCreateSwapchainAndroidSurfaceKHR);
    if (X.has_pt) {
        LOADFN(xrCreatePassthroughFB);
        LOADFN(xrCreatePassthroughLayerFB);
        if (!p_xrCreatePassthroughFB || !p_xrCreatePassthroughLayerFB) X.has_pt = false;
    }
    if (X.rr) {
        LOADFN(xrEnumerateDisplayRefreshRatesFB);
        LOADFN(xrRequestDisplayRefreshRateFB);
        if (!p_xrEnumerateDisplayRefreshRatesFB || !p_xrRequestDisplayRefreshRateFB) X.rr = false;
    }
    if (!p_xrGetVulkanGraphicsRequirements2KHR || !p_xrCreateVulkanInstanceKHR || !p_xrGetVulkanGraphicsDevice2KHR ||
        !p_xrCreateVulkanDeviceKHR || !p_xrCreateSwapchainAndroidSurfaceKHR) {
        XLOG("OpenXR functions missing");
        return false;
    }

    XrSystemGetInfo sg = {XR_TYPE_SYSTEM_GET_INFO};
    sg.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    TRY(xrGetSystem(X.inst, &sg, &X.sys), "xrGetSystem");

    // Vulkan, through the runtime's own create functions (it adds the extensions it needs)
    XrGraphicsRequirementsVulkan2KHR req = {XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
    TRY(p_xrGetVulkanGraphicsRequirements2KHR(X.inst, X.sys, &req), "xrGetVulkanGraphicsRequirements2KHR");
    X.vklib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    X.gipa = X.vklib ? (PFN_vkGetInstanceProcAddr)dlsym(X.vklib, "vkGetInstanceProcAddr") : NULL;
    if (!X.gipa) { XLOG("no libvulkan"); return false; }
    uint32_t maj = XR_VERSION_MAJOR(req.minApiVersionSupported), min = XR_VERSION_MINOR(req.minApiVersionSupported);
    if (maj < 1 || (maj == 1 && min < 1)) maj = 1, min = 1;
    VkApplicationInfo vai = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    vai.pApplicationName = "LAS Recomp VR (XR session)";
    vai.apiVersion = VK_MAKE_API_VERSION(0, maj, min, 0);
    VkInstanceCreateInfo vci = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    vci.pApplicationInfo = &vai;
    XrVulkanInstanceCreateInfoKHR xvi = {XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
    xvi.systemId = X.sys;
    xvi.pfnGetInstanceProcAddr = X.gipa;
    xvi.vulkanCreateInfo = &vci;
    VkResult vr = VK_SUCCESS;
    TRY(p_xrCreateVulkanInstanceKHR(X.inst, &xvi, &X.vki, &vr), "xrCreateVulkanInstanceKHR");
    if (vr != VK_SUCCESS) { XLOG("vkCreateInstance (by OpenXR): %d", (int)vr); return false; }
    XrVulkanGraphicsDeviceGetInfoKHR gdi = {XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
    gdi.systemId = X.sys;
    gdi.vulkanInstance = X.vki;
    TRY(p_xrGetVulkanGraphicsDevice2KHR(X.inst, &gdi, &X.vkp), "xrGetVulkanGraphicsDevice2KHR");
    PFN_vkGetPhysicalDeviceQueueFamilyProperties gqf =
        (PFN_vkGetPhysicalDeviceQueueFamilyProperties)X.gipa(X.vki, "vkGetPhysicalDeviceQueueFamilyProperties");
    if (!gqf) { XLOG("vkGetPhysicalDeviceQueueFamilyProperties missing"); return false; }
    VkQueueFamilyProperties qp[16];
    uint32_t nq = 16;
    gqf(X.vkp, &nq, qp);
    bool found = false;
    for (uint32_t i = 0; i < nq && !found; i++)
        if (qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) X.qfam = i, found = true;
    if (!found) { XLOG("no graphics queue"); return false; }
    float prio = 1.f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = X.qfam;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    XrVulkanDeviceCreateInfoKHR xvd = {XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
    xvd.systemId = X.sys;
    xvd.pfnGetInstanceProcAddr = X.gipa;
    xvd.vulkanPhysicalDevice = X.vkp;
    xvd.vulkanCreateInfo = &dci;
    TRY(p_xrCreateVulkanDeviceKHR(X.inst, &xvd, &X.vkd, &vr), "xrCreateVulkanDeviceKHR");
    if (vr != VK_SUCCESS) { XLOG("vkCreateDevice (by OpenXR): %d", (int)vr); return false; }

    XrGraphicsBindingVulkan2KHR gb = {XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR};
    gb.instance = X.vki;
    gb.physicalDevice = X.vkp;
    gb.device = X.vkd;
    gb.queueFamilyIndex = X.qfam;
    gb.queueIndex = 0;
    XrSessionCreateInfo sci = {XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &gb;
    sci.systemId = X.sys;
    TRY(xrCreateSession(X.inst, &sci, &X.sess), "xrCreateSession");

    XrReferenceSpaceCreateInfo rsi = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rsi.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rsi.poseInReferenceSpace.orientation.w = 1.f;
    TRY(xrCreateReferenceSpace(X.sess, &rsi, &X.space), "xrCreateReferenceSpace");
    if (!make_actions()) return false;

    // the game's panel: an Android Surface (width / height in pixels; format, samples, faces, array size, mips: 0).
    // The size is the settings': a headset's own window is far bigger than the panel needs to be, and the game then
    // renders at that size (Video > Internal resolution: Match display) - a 4K-wide picture per frame for nothing.
    uint32_t ew, eh;
    panel_pixels(&ew, &eh, win_w, win_h);
    X.stereo = vr_stereo_on();
    X.ew = ew, X.eh = eh;
    X.w = X.stereo ? ew * 2 : ew, X.h = eh;
    XLOG("panel %ux%u (%s, %u x %u per eye)", X.w, X.h, X.stereo ? "stereo" : "one picture",
         X.stereo ? X.ew : X.w, X.h);
    XrAndroidSurfaceSwapchainCreateInfoFB sfi = {XR_TYPE_ANDROID_SURFACE_SWAPCHAIN_CREATE_INFO_FB};
    XrSwapchainCreateInfo sc = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    sc.next = fb_surf ? &sfi : NULL;
    sc.width = X.w;
    sc.height = X.h;
    XrResult sr = XR_ERROR_RUNTIME_FAILURE;
    jobject jsurf = NULL;
    for (int attempt = 0; attempt < 2 && XR_FAILED(sr); attempt++) {
        sc.usageFlags = attempt == 0 ? (XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) : 0;
        sr = p_xrCreateSwapchainAndroidSurfaceKHR(X.sess, &sc, &X.sc, &jsurf);
        if (XR_FAILED(sr)) XLOG("xrCreateSwapchainAndroidSurfaceKHR (usage %#llx): %d", (unsigned long long)sc.usageFlags, (int)sr);
    }
    if (XR_FAILED(sr) || !jsurf) return false;
    X.surf = (*env)->NewGlobalRef(env, jsurf);
    X.win = ANativeWindow_fromSurface(env, X.surf);
    if (!X.win) { XLOG("ANativeWindow_fromSurface failed"); return false; }
    ANativeWindow_setBuffersGeometry(X.win, (int32_t)X.w, (int32_t)X.h, 0);  // (0: the surface picks the format)

    if (X.has_pt) {
        XrPassthroughCreateInfoFB pci = {XR_TYPE_PASSTHROUGH_CREATE_INFO_FB};
        pci.flags = XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB;
        XrPassthroughLayerCreateInfoFB lci = {XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB};
        if (XR_SUCCEEDED(p_xrCreatePassthroughFB(X.sess, &pci, &X.pt))) {
            lci.passthrough = X.pt;
            lci.flags = XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB;
            lci.purpose = XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB;
            if (XR_FAILED(p_xrCreatePassthroughLayerFB(X.sess, &lci, &X.ptl))) X.has_pt = false;
        } else X.has_pt = false;
        XLOG("passthrough %s", X.has_pt ? "on" : "not available (black background)");
    }

    if (pthread_create(&X.th, NULL, xr_thread, NULL) != 0) { XLOG("thread"); return false; }
    pthread_detach(X.th);
    XLOG("OpenXR ready: panel %ux%u", X.w, X.h);
    return true;
}

struct ANativeWindow *xr_quest_window(int w, int h) {
    if (!xr_quest_requested()) return NULL;
    pthread_mutex_lock(&X.lock);
    if (!X.tried) {
        X.tried = true;
        X.ok = xr_start(w, h);
        if (!X.ok) XLOG("OpenXR did not start: the game uses the activity's own window");
    }
    struct ANativeWindow *r = X.ok ? X.win : NULL;
    pthread_mutex_unlock(&X.lock);
    return r;
}

#else  // not Android: nothing
#include "xr_quest.h"
bool xr_quest_requested(void) { return false; }
struct ANativeWindow *xr_quest_window(int w, int h) { (void)w; (void)h; return 0; }
bool xr_quest_extent(uint32_t *w, uint32_t *h) { (void)w; (void)h; return false; }
bool xr_quest_active(void) { return false; }
void xr_quest_note_panel(uint32_t w, uint32_t h) { (void)w; (void)h; }
bool vr_stereo_on(void) { return false; }
void vr_stereo_split_panel(double *w, double *h) { (void)w; (void)h; }
void vr_mode_preset(int mode) { (void)mode; }
#endif
