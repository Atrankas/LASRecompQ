// Output resolution, shared by the Metal and Vulkan backends (each calls output_scale() once per presented frame).
// Render targets the game sizes for the Switch's 1920x1080 screen are allocated at (sx, sy) times their size; the
// image is then drawn into (vx, vy, vw, vh) of the window. Video tab "Internal resolution" (g_settings.res_mode):
//   Match display  render at the window's own pixel size (default)
//   1x .. 4x       N times the Switch's docked image: 1080 lines x N (wide windows keep their shape: 32:9 at 2x =
//                  7680 x 2160); capped to the GPU's 16384-pixel texture limit
//   .75x / .50x / .25x  of Match display (scaled up to the window - with MetalFX on macOS when chosen)
//   Dynamic        Match display, lowered in steps while the GPU can't hold 60 fps and raised again when it can
//                  (each change reallocates the render targets: a short hitch, so it changes rarely, with hysteresis)
// Test overrides: LARECOMP_SCALE=<f> (render height = 1080 x f), LARECOMP_WIDE_TEST=<f> (forced shape).
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "../hle.h"
#include "../settings.h"
#include "output_scale.h"

#define MAX_TEX 16384.0

// Dynamic resolution: a factor on the Match display scale, 0.5 .. 1 in 0.05 steps, held at 1 (the game's own look)
// whenever the GPU allows. Phones aim at the game's 30 fps (a phone GPU can't hold 60 in its heavy areas: an S20 FE's
// overworld at native resolution takes 24 ms of GPU time a frame cool, 35 once its thermal limits set in) and judge
// by the GPU's own time when the backend measures it (Vulkan with fences: the done watcher), raising again only with
// clear room.
#ifdef __ANDROID__
// Phones go up to the 3D at the window's own lines (1.5: the game's 720p 3D at 1080p) where the GPU has room - houses,
// dungeons - and down to half of 1 where it hasn't. With frame generation on, the in-between frames' ~3 ms come out
// of the budget so that it keeps running (vulkan/framegen.c pauses it over 36.5 ms a frame).
#define DYN_MAX 1.5
#define DYN_SLOW_MS 34.5   // (a 30 fps frame is 33.3 ms; one late frame is 50)
#define DYN_SLOWER_MS 40.0
#define DYN_GPU_SLOW (fg_on() ? 27.0 : 30.0)
#define DYN_GPU_ROOM (fg_on() ? 23.5 : 26.0)  // (+0.05 near 1: ~10% more pixels, ~1 ms in the Field)
extern bool fg_enabled(void);
static bool fg_on(void) { return fg_enabled(); }
#else
#define DYN_MAX 1.0
#define DYN_SLOW_MS 17.4
#define DYN_SLOWER_MS 22.0
#define DYN_GPU_SLOW 14.5
#define DYN_GPU_ROOM 10.5
#endif
static double g_dyn = 1.0;  // (from the game's own size: up or down from there)
static double g_dyn_gpu_sum, g_dyn_dt_sum, g_last_t;
static int g_dyn_n, g_dyn_gpu_n, g_dyn_good;
static double g_dyn_cool_until, g_dyn_probe_wait = 5.0, g_dyn_probe_at;
static double g_dyn_last_raise = -100;
// A raise taken back soon after: that factor is out of reach for a while (30 s, doubling to 5 min) - walking in the
// Field it went 0.65 -> 0.70 -> 0.65 every few seconds, each change a render-target reallocation (a hitch)
static double g_dyn_ceiling = 99, g_dyn_ceiling_until, g_dyn_ceiling_wait = 30;

// The factor each level settled at: back in a level, start from it (the Field after a house began at the house's
// factor and spent seconds at 20 fps coming down). This session only.
static struct { char name[32]; double f; } g_dyn_levels[48];
static void dyn_level_check(double now) {
    extern const char *game_level_name(void);
    static char cur[32];
    const char *lv = game_level_name();
    if (!lv || !*lv || !strncmp(lv, cur, sizeof cur - 1)) return;
    int i, freei = -1;
    for (i = 0; i < 48 && g_dyn_levels[i].name[0]; i++)
        if (!strcmp(g_dyn_levels[i].name, cur)) break;
    if (cur[0] && i < 48) snprintf(g_dyn_levels[i].name, sizeof g_dyn_levels[i].name, "%s", cur), g_dyn_levels[i].f = g_dyn;
    snprintf(cur, sizeof cur, "%s", lv);
    g_dyn_ceiling = 99, g_dyn_ceiling_until = 0, g_dyn_ceiling_wait = 30;  // (another place: its own limits)
    for (i = 0; i < 48 && g_dyn_levels[i].name[0]; i++)
        if (!strcmp(g_dyn_levels[i].name, cur)) { g_dyn = g_dyn_levels[i].f; freei = i; break; }
    if (freei >= 0) {
        g_dyn_dt_sum = g_dyn_gpu_sum = 0, g_dyn_n = g_dyn_gpu_n = 0, g_dyn_good = 0;
        g_dyn_cool_until = now + 1.0;
        LOG("[dynres] back in %s: %.2f\n", cur, g_dyn);
    }
}

static void dyn_update(double now, double gpu_ms) {
    dyn_level_check(now);
    double dt = g_last_t > 0 ? (now - g_last_t) * 1000.0 : 16.7;
    g_last_t = now;
    if (dt > 250) return;  // (loading, menus, a stall: not the GPU's steady state)
    g_dyn_dt_sum += dt;
    g_dyn_n++;
    if (gpu_ms >= 0) { g_dyn_gpu_sum += gpu_ms; g_dyn_gpu_n++; }
    if (g_dyn_n < 30) return;  // decide every ~half second
    double avg_dt = g_dyn_dt_sum / g_dyn_n, avg_gpu = g_dyn_gpu_n ? g_dyn_gpu_sum / g_dyn_gpu_n : -1;
    g_dyn_dt_sum = g_dyn_gpu_sum = 0;
    g_dyn_n = g_dyn_gpu_n = 0;
    if (now < g_dyn_cool_until) return;
    double prev = g_dyn;
    // (frames late with the GPU far under its budget: the CPU's doing - fewer pixels wouldn't help; Madam MeowMeow's
    // house went from 1.2 to 0.6 for 1 ms)
    bool slow = (avg_dt > DYN_SLOW_MS && (avg_gpu < 0 || avg_gpu >= DYN_GPU_ROOM)) || (avg_gpu >= 0 && avg_gpu > DYN_GPU_SLOW);
    if (slow) {
        // the step by how far over (GPU time ~ the square of the factor): entering the Field from a house it took
        // ten seconds at 20 fps, 0.05 at a time, to get down to where it fits
        double down = avg_dt > DYN_SLOWER_MS ? 0.1 : 0.05;
        if (avg_gpu > DYN_GPU_SLOW) down = fmax(down, fmin(0.3, g_dyn * (1 - sqrt((DYN_GPU_SLOW - 2) / avg_gpu))));
        if (now - g_dyn_last_raise < 20.0) {
            if (now < g_dyn_ceiling_until + 60.0 && fabs(g_dyn - g_dyn_ceiling) < 0.01) g_dyn_ceiling_wait = fmin(g_dyn_ceiling_wait * 2, 300.0);
            else g_dyn_ceiling_wait = 30;
            g_dyn_ceiling = g_dyn, g_dyn_ceiling_until = now + g_dyn_ceiling_wait;
        }
        g_dyn = fmax(0.5, g_dyn - down);
        g_dyn_good = 0;
        if (now - g_dyn_last_raise < 3.0) g_dyn_probe_wait = fmin(g_dyn_probe_wait * 2, 60.0);  // raised too eagerly
    } else if (g_dyn < DYN_MAX) {
        g_dyn_good++;
        // with GPU timing: raise when there is clear headroom; without (Vulkan): probe upwards now and then
        bool room = avg_gpu >= 0 ? avg_gpu < DYN_GPU_ROOM && g_dyn_good >= 4 : now >= g_dyn_probe_at && g_dyn_good >= 2 * (int)g_dyn_probe_wait;
        // the step by the room (GPU time grows with the pixels, the square of the factor): far under the budget, up
        // to 0.2 at once - a house after the Field went from half to full in a minute 0.05 at a time
        double step = 0.05;
        if (avg_gpu > 0) step = fmax(0.05, fmin(0.2, g_dyn * (sqrt(DYN_GPU_ROOM / avg_gpu) - 1)));
        if (room && now < g_dyn_ceiling_until && g_dyn + step > g_dyn_ceiling - 0.01) {
            step = round((g_dyn_ceiling - 0.05 - g_dyn) * 20) / 20;  // (up to just under it)
            room = step > 0.01;
        }
        if (room) { g_dyn = fmin(DYN_MAX, g_dyn + step); g_dyn_good = 0; g_dyn_last_raise = now; g_dyn_probe_at = now + g_dyn_probe_wait; }
    }
    g_dyn = round(g_dyn * 20) / 20;
    if (g_dyn != prev) {
        g_dyn_cool_until = now + 1.5;
        LOG("[dynres] %.2f -> %.2f (frame %.1f ms, gpu %.1f ms)\n", prev, g_dyn, avg_dt, avg_gpu);
    }
}

double output_scale_dynamic_factor(void) { return g_dyn; }

void output_scale(double dw, double dh, bool reshape_allowed, double now, double gpu_ms, OutScale *o) {
    double fit = fmin(dw / 1920.0, dh / 1080.0);
    static double forced = -1, wide_test = -1;
    if (forced < 0) forced = getenv("LARECOMP_SCALE") ? atof(getenv("LARECOMP_SCALE")) : 0;
    if (wide_test < 0) wide_test = getenv("LARECOMP_WIDE_TEST") ? atof(getenv("LARECOMP_WIDE_TEST")) : 0;
    double shape = wide_test > 0 ? wide_test : (dw / dh) / (16.0 / 9.0);
    bool reshape = reshape_allowed && fabs(shape - 1.0) > 0.005;
    if (!reshape) shape = 1.0;
    // render height in units of the Switch's 1080 lines
    double h;
    int mode = g_settings.res_mode;
    if (forced > 0) h = forced;
    else if (mode >= 1 && mode <= 8) h = mode;
    else {
        h = reshape ? dh / 1080.0 : fit;  // Match display
        if (mode == RES_DYNAMIC) { dyn_update(now, gpu_ms); h *= g_dyn; }
        else if (mode == RES_Q75) h *= 0.75;
        else if (mode == RES_Q25) h *= 0.25;
        else if (mode == RES_Q50) h *= 0.5;
    }
    h = fmax(h, 0.1);  // (.25x of a small window)
    double sx = h * shape, sy = h;
    double cap = fmin(MAX_TEX / (1920.0 * sx), MAX_TEX / (1080.0 * sy));
    if (cap < 1.0) { sx *= cap; sy *= cap; }
    o->sx = sx; o->sy = sy; o->shape = shape;
    // where the image goes in the window: fills it when reshaped, else a fitted 16:9 box
    if (reshape) { o->vw = dw; o->vh = dh; }
    else { o->vw = 1920.0 * fit; o->vh = 1080.0 * fit; }
    if (wide_test > 0) {
        double f2 = fmin(dw / (1920.0 * wide_test), dh / 1080.0);
        o->vw = 1920.0 * wide_test * f2; o->vh = 1080.0 * f2;
    }
    o->vx = (dw - o->vw) / 2; o->vy = (dh - o->vh) / 2;
}
