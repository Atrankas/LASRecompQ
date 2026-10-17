// On-screen controls for touch screens (phones, tablets): a floating left stick, A / B / X / Y, L / R / ZL / ZR, Plus /
// Minus, and a button for the overlay menu. Shown while no controller is connected (Settings > Controls > Touch
// controls: Auto / Always / Off). SDL finger events come in from the window loop (runtime/hle/input.c), the state goes
// to the game with the keyboard's and the controller's (input.c fill_state), and the controls are drawn over the game
// by the overlay menu's ImGui frame (runtime/ui/menu.mm). Layout in units of the screen height, for landscape.
#include <SDL3/SDL.h>
#include <math.h>
#include <atomic>
#include <mutex>
#include <string.h>

#include "imgui.h"

extern "C" {
#include "settings.h"
bool input_gamepad_connected(void);  // runtime/hle/input.c
bool menu_input_captured(void);      // runtime/ui/menu.mm
void menu_toggle(void);
}

enum { BTN_A = 0, BTN_B, BTN_X, BTN_Y, BTN_L = 6, BTN_R, BTN_ZL, BTN_ZR, BTN_PLUS, BTN_MINUS };  // (input.c's bits)
enum { C_STICK = 0, C_A, C_B, C_X, C_Y, C_L, C_R, C_ZL, C_ZR, C_PLUS, C_MINUS, C_MENU, C_COUNT };

typedef struct {
    float x, y, r;       // centre and radius (round) / half height (shoulder pills: width 2.2 * r)
    int bit;             // button bit, -1 none
    const char *label;
    bool pill;
} Control;

static Control g_ctl[C_COUNT];
static float g_w, g_h, g_size;             // the screen and size setting the layout was made for (pixels)
static std::mutex g_lock;                  // layout and fingers: the window loop's events, the render thread's drawing
static std::atomic<uint64_t> g_buttons{0};
static std::atomic<int32_t> g_lx{0}, g_ly{0};
static float g_stick_cx, g_stick_cy;       // where the stick's base is now (it follows the thumb's first touch)
static float g_knob_x, g_knob_y;
static bool g_menu_down;
static double g_last_touch;                // (SDL_GetTicks seconds) for Auto: a touch shows them again
#define MAX_FINGERS 10
static struct { SDL_FingerID id; int ctl; } g_fingers[MAX_FINGERS];  // ctl -1 = free slot

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

static void layout(float w, float h) {
    g_w = w, g_h = h, g_size = g_settings.touch_size;
    float s = h * clampf(g_settings.touch_size, 0.6f, 1.6f);
    float br = 0.068f * s, gap = 0.118f * s;  // face buttons
    float fx = w - 0.25f * s, fy = h - 0.30f * s;
    g_ctl[C_A] = {fx + gap, fy, br, BTN_A, "A", false};
    g_ctl[C_B] = {fx, fy + gap, br, BTN_B, "B", false};
    g_ctl[C_X] = {fx, fy - gap, br, BTN_X, "X", false};
    g_ctl[C_Y] = {fx - gap, fy, br, BTN_Y, "Y", false};
    float pr = 0.045f * s;  // shoulders: pills in the top corners, ZL / ZR outermost
    g_ctl[C_ZL] = {0.10f * s, 0.075f * s, pr, BTN_ZL, "ZL", true};
    g_ctl[C_L] = {0.10f * s, 0.19f * s, pr, BTN_L, "L", true};
    g_ctl[C_ZR] = {w - 0.10f * s, 0.075f * s, pr, BTN_ZR, "ZR", true};
    g_ctl[C_R] = {w - 0.10f * s, 0.19f * s, pr, BTN_R, "R", true};
    float sr = 0.042f * s;
    g_ctl[C_MINUS] = {w * 0.5f - 0.13f * s, 0.06f * s, sr, BTN_MINUS, "-", false};
    g_ctl[C_PLUS] = {w * 0.5f + 0.13f * s, 0.06f * s, sr, BTN_PLUS, "+", false};
    g_ctl[C_MENU] = {w * 0.5f, 0.06f * s, sr, -1, "", false};
    g_ctl[C_STICK] = {0.24f * s, h - 0.27f * s, 0.13f * s, -1, "", false};
    g_stick_cx = g_ctl[C_STICK].x, g_stick_cy = g_ctl[C_STICK].y;
    g_knob_x = g_stick_cx, g_knob_y = g_stick_cy;
}

extern "C" bool touch_visible(void) {
    if (g_settings.touch_mode == 2) return false;
    if (g_settings.touch_mode == 1) return true;
    // Auto: on a touch screen, while there is no controller - or a touch came in the last 30 s
    static int touchscreen = -1;
    if (touchscreen < 0) {
        int n = 0;
        SDL_free(SDL_GetTouchDevices(&n));
        touchscreen = n > 0;
    }
    if (!touchscreen) return false;
    return !input_gamepad_connected() || (g_last_touch > 0 && SDL_GetTicks() / 1000.0 - g_last_touch < 30);
}

static bool inside(const Control &c, float x, float y, float slack) {
    if (c.pill) return fabsf(x - c.x) <= c.r * 2.2f * slack && fabsf(y - c.y) <= c.r * 1.2f * slack;
    float dx = x - c.x, dy = y - c.y, r = c.r * slack;
    return dx * dx + dy * dy <= r * r;
}

// The control under (x, y): buttons get a generous slack (thumbs miss), the stick takes the lower left quarter
static int hit(float x, float y) {
    for (int i = C_A; i < C_COUNT; i++)
        if (inside(g_ctl[i], x, y, 1.35f)) return i;
    if (x < g_w * 0.42f && y > g_h * 0.30f) return C_STICK;
    return -1;
}

static void recompute(void) {
    uint64_t b = 0;
    for (int i = 0; i < MAX_FINGERS; i++) {
        int c = g_fingers[i].ctl;
        if (c > C_STICK && g_ctl[c].bit >= 0) b |= 1ull << g_ctl[c].bit;
    }
    g_buttons = b;
}

static void stick_to(float x, float y) {
    float r = g_ctl[C_STICK].r, dx = x - g_stick_cx, dy = y - g_stick_cy, d = sqrtf(dx * dx + dy * dy);
    if (d > r) dx *= r / d, dy *= r / d;
    g_knob_x = g_stick_cx + dx, g_knob_y = g_stick_cy + dy;
    float nx = dx / r, ny = -dy / r, m = sqrtf(nx * nx + ny * ny);
    if (m < 0.12f) nx = ny = 0;  // dead zone
    g_lx = (int32_t)lrintf(clampf(nx, -1, 1) * 32767), g_ly = (int32_t)lrintf(clampf(ny, -1, 1) * 32767);
}

static void release_all(void) {
    for (int i = 0; i < MAX_FINGERS; i++) g_fingers[i].ctl = -1;
    g_buttons = 0, g_lx = 0, g_ly = 0;
    g_knob_x = g_stick_cx = g_ctl[C_STICK].x, g_knob_y = g_stick_cy = g_ctl[C_STICK].y;
    g_menu_down = false;
}

// Everything let go (the overlay menu opened: it takes the touches, the lifts included)
extern "C" void touch_reset(void) {
    std::lock_guard<std::mutex> l(g_lock);
    release_all();
}

static bool handle(const SDL_Event *e, bool *toggle_menu);
extern "C" bool touch_handle_event(const SDL_Event *e) {
    if (e->type != SDL_EVENT_FINGER_DOWN && e->type != SDL_EVENT_FINGER_MOTION && e->type != SDL_EVENT_FINGER_UP &&
        e->type != SDL_EVENT_FINGER_CANCELED)
        return false;
    bool toggle = false, r;
    {
        std::lock_guard<std::mutex> l(g_lock);
        r = handle(e, &toggle);
    }
    if (toggle) menu_toggle();  // (outside the lock: opening the menu lets every touch go, touch_reset)
    return r;
}

static bool handle(const SDL_Event *e, bool *toggle_menu) {
    static bool init;
    if (!init) { init = true; for (int i = 0; i < MAX_FINGERS; i++) g_fingers[i].ctl = -1; }
    g_last_touch = SDL_GetTicks() / 1000.0;
    if (!g_w || menu_input_captured() || !touch_visible()) { release_all(); return false; }  // (the menu takes touches)
    float x = e->tfinger.x * g_w, y = e->tfinger.y * g_h;
    int slot = -1;
    for (int i = 0; i < MAX_FINGERS; i++)
        if (g_fingers[i].ctl >= 0 && g_fingers[i].id == e->tfinger.fingerID) slot = i;
    if (e->type == SDL_EVENT_FINGER_DOWN) {
        int c = hit(x, y);
        if (c < 0) return true;
        if (slot < 0) for (int i = 0; i < MAX_FINGERS && slot < 0; i++) if (g_fingers[i].ctl < 0) slot = i;
        if (slot < 0) return true;
        g_fingers[slot].id = e->tfinger.fingerID, g_fingers[slot].ctl = c;
        if (c == C_STICK) {  // the base comes to the thumb (kept on screen)
            float r = g_ctl[C_STICK].r;
            g_stick_cx = clampf(x, r, g_w * 0.42f), g_stick_cy = clampf(y, g_h * 0.30f + r * 0.5f, g_h - r);
            stick_to(x, y);
        } else if (c == C_MENU) {
            g_menu_down = true;
        }
        recompute();
        return true;
    }
    if (slot < 0) return true;
    int c = g_fingers[slot].ctl;
    if (e->type == SDL_EVENT_FINGER_MOTION) {
        if (c == C_STICK) stick_to(x, y);
        else if (c >= C_A && c <= C_Y) {  // sliding across the face buttons presses the one under the thumb
            int n = hit(x, y);
            if (n >= C_A && n <= C_Y) g_fingers[slot].ctl = n;
        }
        recompute();
        return true;
    }
    // up / cancelled
    g_fingers[slot].ctl = -1;
    if (c == C_STICK) {
        g_lx = 0, g_ly = 0;
        g_knob_x = g_stick_cx = g_ctl[C_STICK].x, g_knob_y = g_stick_cy = g_ctl[C_STICK].y;
    }
    if (c == C_MENU && g_menu_down) {
        g_menu_down = false;
        if (e->type == SDL_EVENT_FINGER_UP) *toggle_menu = true;
    }
    recompute();
    return true;
}

extern "C" uint64_t touch_buttons(void) { return g_buttons; }
extern "C" void touch_stick(int32_t *lx, int32_t *ly) { *lx = g_lx, *ly = g_ly; }

// Drawn over the game (the overlay menu's frame, before its windows)
extern "C" void touch_draw(ImDrawList *dl, float w, float h, ImFont *font) {
    std::lock_guard<std::mutex> l(g_lock);
    if (w != g_w || h != g_h || g_size != g_settings.touch_size) {  // (a new screen, or the Size setting changed)
        release_all();
        layout(w, h);
    }
    float a = clampf(g_settings.touch_opacity, 0.15f, 1.0f);
    uint64_t b = g_buttons;
    ImU32 fill = IM_COL32(20, 20, 24, (int)(110 * a)), fill_on = IM_COL32(236, 194, 84, (int)(200 * a));
    ImU32 ring = IM_COL32(255, 255, 255, (int)(150 * a)), text = IM_COL32(255, 255, 255, (int)(230 * a));
    float th = h / 1080.0f;
    for (int i = C_A; i < C_COUNT; i++) {
        const Control &c = g_ctl[i];
        bool on = (c.bit >= 0 && (b >> c.bit & 1)) || (i == C_MENU && g_menu_down);
        if (c.pill) {
            ImVec2 p0(c.x - c.r * 2.2f, c.y - c.r * 1.2f), p1(c.x + c.r * 2.2f, c.y + c.r * 1.2f);
            dl->AddRectFilled(p0, p1, on ? fill_on : fill, c.r * 1.2f);
            dl->AddRect(p0, p1, ring, c.r * 1.2f, 0, 2.5f * th);
        } else {
            dl->AddCircleFilled(ImVec2(c.x, c.y), c.r, on ? fill_on : fill, 40);
            dl->AddCircle(ImVec2(c.x, c.y), c.r, ring, 40, 2.5f * th);
        }
        if (i == C_MENU) {  // three bars
            float u = c.r * 0.45f;
            for (int k = -1; k <= 1; k++)
                dl->AddLine(ImVec2(c.x - u, c.y + k * u * 0.6f), ImVec2(c.x + u, c.y + k * u * 0.6f), text, 3 * th);
            continue;
        }
        float fs = c.r * (c.pill ? 0.95f : strlen(c.label) == 1 && (c.label[0] == '+' || c.label[0] == '-') ? 1.5f : 1.05f);
        ImVec2 ts = font->CalcTextSizeA(fs, 1e9f, 0, c.label);
        dl->AddText(font, fs, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), text, c.label);
    }
    // the stick: base and knob
    const Control &s = g_ctl[C_STICK];
    dl->AddCircleFilled(ImVec2(g_stick_cx, g_stick_cy), s.r, IM_COL32(20, 20, 24, (int)(80 * a)), 48);
    dl->AddCircle(ImVec2(g_stick_cx, g_stick_cy), s.r, ring, 48, 2.5f * th);
    dl->AddCircleFilled(ImVec2(g_knob_x, g_knob_y), s.r * 0.45f, IM_COL32(230, 230, 235, (int)(140 * a)), 40);
}
