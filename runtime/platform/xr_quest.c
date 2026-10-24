// Meta Quest (Horizon OS): the game inside an OpenXR session. See xr_quest.h.
//
// What this file does (phase A):
//  - OpenXR instance + session on a Vulkan device of its own (XR_KHR_vulkan_enable2: the runtime wants the device it
//    shows images with to come from it; the game keeps its own device, made as on any phone),
//  - an Android-surface swapchain (XR_KHR_android_surface_swapchain): an ordinary Android Surface the game's Vulkan
//    swapchain presents to, shown by the compositor as a quad layer in front of the player (device.c create_surface),
//  - passthrough behind it when the headset offers it (XR_FB_passthrough): mixed reality,
//  - the Touch controllers through OpenXR actions, fed to the game as an SDL virtual gamepad: its menus, its button
//    bindings and its prompts work with it unchanged.
// Everything is logged to logcat with the tag LASRecomp-XR.
#ifdef __ANDROID__
#include "xr_quest.h"

#include <SDL3/SDL.h>
#include <android/log.h>
#include <android/native_window_jni.h>
#include <dlfcn.h>
#include <jni.h>
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

// Where the panel hangs, in metres, in the space the session starts in (LOCAL: the head's place at the start, y up,
// -z ahead). 3.2 m wide at 2.2 m is about 70 degrees of view.
#define PANEL_DISTANCE 2.2f
#define PANEL_WIDTH 3.2f
#define PANEL_HEIGHT_OFFSET -0.1f

static PFN_xrGetVulkanGraphicsRequirements2KHR p_xrGetVulkanGraphicsRequirements2KHR;
static PFN_xrCreateVulkanInstanceKHR p_xrCreateVulkanInstanceKHR;
static PFN_xrGetVulkanGraphicsDevice2KHR p_xrGetVulkanGraphicsDevice2KHR;
static PFN_xrCreateVulkanDeviceKHR p_xrCreateVulkanDeviceKHR;
static PFN_xrCreateSwapchainAndroidSurfaceKHR p_xrCreateSwapchainAndroidSurfaceKHR;
static PFN_xrCreatePassthroughFB p_xrCreatePassthroughFB;
static PFN_xrCreatePassthroughLayerFB p_xrCreatePassthroughLayerFB;

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
    uint32_t w, h;
    void *vklib;
    PFN_vkGetInstanceProcAddr gipa;
    VkInstance vki;
    VkPhysicalDevice vkp;
    VkDevice vkd;
    uint32_t qfam;
    bool has_pt;
    XrPassthroughFB pt;
    XrPassthroughLayerFB ptl;
    XrActionSet aset;
    XrAction a_move, a_look, a_a, a_b, a_x, a_y, a_thumbl, a_thumbr, a_menu, a_trig_l, a_trig_r, a_sq_l, a_sq_r;
    atomic_bool quit, running, focused;
    pthread_t th;
} X = {.lock = PTHREAD_MUTEX_INITIALIZER};

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

    const struct { XrAction a; const char *path; } map[] = {
        {X.a_move, "/user/hand/left/input/thumbstick"},      {X.a_look, "/user/hand/right/input/thumbstick"},
        {X.a_a, "/user/hand/right/input/a/click"},           {X.a_b, "/user/hand/right/input/b/click"},
        {X.a_x, "/user/hand/left/input/x/click"},            {X.a_y, "/user/hand/left/input/y/click"},
        {X.a_thumbl, "/user/hand/left/input/thumbstick/click"}, {X.a_thumbr, "/user/hand/right/input/thumbstick/click"},
        {X.a_menu, "/user/hand/left/input/menu/click"},
        {X.a_trig_l, "/user/hand/left/input/trigger/value"}, {X.a_trig_r, "/user/hand/right/input/trigger/value"},
        {X.a_sq_l, "/user/hand/left/input/squeeze/value"},   {X.a_sq_r, "/user/hand/right/input/squeeze/value"},
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
    return true;
}

// ---- the Touch controllers as an SDL virtual gamepad (standard layout: the game's own mapping, menus and
// rebinding see an ordinary Xbox-style pad; the vendor / product ids are an Xbox One pad's, which input.c ranks above
// the half pad the headset's own Android input layer lists)
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
    for (int i = SDL_GAMEPAD_BUTTON_SOUTH; i <= SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER; i++) d.button_mask |= 1u << i;
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

// Touch layout -> Xbox layout (the face buttons carry the same letters): A south, B east, X west, Y north; the grips
// are the shoulders, the triggers the analog triggers, the left menu button Start, the stick clicks L3 / R3 (the left
// one is Back: the game's Minus; remap in the Controls tab). Sticks: OpenXR's y is up, SDL's down.
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
    bool nb[SDL_GAMEPAD_BUTTON_COUNT] = {0};
    nb[SDL_GAMEPAD_BUTTON_SOUTH] = get_b(X.a_a);
    nb[SDL_GAMEPAD_BUTTON_EAST] = get_b(X.a_b);
    nb[SDL_GAMEPAD_BUTTON_WEST] = get_b(X.a_x);
    nb[SDL_GAMEPAD_BUTTON_NORTH] = get_b(X.a_y);
    nb[SDL_GAMEPAD_BUTTON_START] = get_b(X.a_menu);
    nb[SDL_GAMEPAD_BUTTON_BACK] = get_b(X.a_thumbl);
    nb[SDL_GAMEPAD_BUTTON_RIGHT_STICK] = get_b(X.a_thumbr);
    nb[SDL_GAMEPAD_BUTTON_LEFT_SHOULDER] = grip_l;
    nb[SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER] = grip_r;
    Sint16 na[SDL_GAMEPAD_AXIS_COUNT] = {0};
    float x, y;
    get_v2(X.a_move, &x, &y);
    na[SDL_GAMEPAD_AXIS_LEFTX] = (Sint16)(x * 32767.f), na[SDL_GAMEPAD_AXIS_LEFTY] = (Sint16)(-y * 32767.f);
    get_v2(X.a_look, &x, &y);
    na[SDL_GAMEPAD_AXIS_RIGHTX] = (Sint16)(x * 32767.f), na[SDL_GAMEPAD_AXIS_RIGHTY] = (Sint16)(-y * 32767.f);
    na[SDL_GAMEPAD_AXIS_LEFT_TRIGGER] = (Sint16)(get_f(X.a_trig_l) * 32767.f);
    na[SDL_GAMEPAD_AXIS_RIGHT_TRIGGER] = (Sint16)(get_f(X.a_trig_r) * 32767.f);
    pad_set(nb, na);
}

// ---- the frame loop (its own thread: xrWaitFrame paces it at the headset's refresh rate)
static void push_sdl(SDL_EventType t) {
    SDL_Event e;
    SDL_zero(e);
    e.type = t;
    SDL_PushEvent(&e);
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
                    bool again = atomic_exchange(&X.running, true);
                    (void)again;
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

static void *xr_thread(void *arg) {
    (void)arg;
    JNIEnv *env = NULL;
    (*X.vm)->AttachCurrentThread(X.vm, &env, NULL);
    XLOG("frame loop started");
    while (!atomic_load(&X.quit)) {
        handle_events();
        if (!atomic_load(&X.running)) { usleep(20000); continue; }
        XrFrameState fs = {XR_TYPE_FRAME_STATE};
        if (XR_FAILED(xrWaitFrame(X.sess, NULL, &fs))) { usleep(20000); continue; }
        if (XR_FAILED(xrBeginFrame(X.sess, NULL))) continue;
        if (atomic_load(&X.focused)) pad_update();

        const XrCompositionLayerBaseHeader *layers[2];
        uint32_t nl = 0;
        XrCompositionLayerPassthroughFB pl = {XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB};
        if (X.has_pt) {
            pl.flags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            pl.layerHandle = X.ptl;
            layers[nl++] = (const XrCompositionLayerBaseHeader *)&pl;
        }
        XrCompositionLayerQuad q = {XR_TYPE_COMPOSITION_LAYER_QUAD};
        if (fs.shouldRender) {
            q.layerFlags = 0;
            q.space = X.space;
            q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            q.subImage.swapchain = X.sc;
            q.subImage.imageRect.offset = (XrOffset2Di){0, 0};
            q.subImage.imageRect.extent = (XrExtent2Di){(int32_t)X.w, (int32_t)X.h};
            q.subImage.imageArrayIndex = 0;
            q.pose.orientation = (XrQuaternionf){0.f, 0.f, 0.f, 1.f};
            q.pose.position = (XrVector3f){0.f, PANEL_HEIGHT_OFFSET, -PANEL_DISTANCE};
            q.size = (XrExtent2Df){PANEL_WIDTH, PANEL_WIDTH * (float)X.h / (float)X.w};
            layers[nl++] = (const XrCompositionLayerBaseHeader *)&q;
        }
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
static bool xr_start(int w, int h) {
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
    const char *pt_env = getenv("LARECOMP_XR_PASSTHROUGH");
    bool want_pt = !pt_env || atoi(pt_env) != 0;
    X.has_pt = want_pt && has_ext(ep, ne, XR_FB_PASSTHROUGH_EXTENSION_NAME);
    if (X.has_pt) enabled[nen++] = XR_FB_PASSTHROUGH_EXTENSION_NAME;
    free(ep);
    XLOG("extensions: surface-create-FB %d, passthrough %d", fb_surf, X.has_pt);

    XrInstanceCreateInfoAndroidKHR ai = {XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
    ai.applicationVM = X.vm;
    ai.applicationActivity = X.activity;
    XrInstanceCreateInfo ci = {XR_TYPE_INSTANCE_CREATE_INFO};
    ci.next = &ai;
    strcpy(ci.applicationInfo.applicationName, "LAS Recomp");
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
    vai.pApplicationName = "LAS Recomp (XR session)";
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

    // the game's panel: an Android Surface (width / height in pixels; format, samples, faces, array size, mips: 0)
    X.w = (uint32_t)w, X.h = (uint32_t)h;
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
#endif
