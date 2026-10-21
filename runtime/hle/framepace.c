// Frame pacing: the game's speed doesn't depend on the frame rate.
// The game advances its world each frame by its frame period (16.67 ms) times the swap interval its frame timer
// (f_700000c910) reads from the system object - normally 1. When a frame takes longer than a vsync (GPU-bound at
// 32:9, a hitch), the next frame starts a vsync later but still advanced only 16.67 ms: slow motion (30 fps = half
// speed). tools/patches.py FRAME_STEPS_SITE routes that read here: we return how many vsyncs really passed since
// the last frame, so the world advances by the time that passed. The game's own waiting is unaffected (it reads
// the interval nowhere else). Shared by the macOS and Windows builds.
// FINDING (2026-10-09): forcing 2 steps did NOT change Link's walking speed - gameplay is a true fixed step per
// frame; this timer feeds something else. Kept for research, off by default. (2026-10-18: the read is the game's
// dynamic resolution's frame budget - tools/patches.py GAME_DYNRES_SITE, now off, so it no longer runs at all.)
//   LARECOMP_FRAMEPACE=1     hand the timer the vsyncs elapsed (default: the game's own value)
//   LARECOMP_FRAME_STEPS_MAX maximum steps per frame (default 4: after a long stall the game doesn't jump ahead)
//   LARECOMP_FORCE_STEPS=<n> (test) always n
#include <math.h>
#include <stdlib.h>

#include "../hle.h"
#include "../platform.h"
#include "../settings.h"

extern volatile uint64_t g_vsync_count;  // runtime/hle/vi.c
uint64_t g_frame_steps_late;             // (stats) frames that advanced more than one step
double g_game_speed = 1.0;               // the game's speed over the last 2 s (1: real time; rt_frame_steps)
uint32_t g_game_interval = 1;            // the game's frame interval now (1: its 60 fps mode, 2: its 30)

uint32_t rt_frame_steps(uint32_t interval) {
    static int on = -1, max_steps, forced;
    if (on < 0) {
        const char *e = ENV("LARECOMP_FRAMEPACE");
        on = e ? atoi(e) != 0 : 0;  // off by default: the world (Link's movement) doesn't use this step (tested)
        e = ENV("LARECOMP_FRAME_STEPS_MAX");
        max_steps = e ? atoi(e) : 4;
        if (max_steps < 1) max_steps = 1;
        e = ENV("LARECOMP_FORCE_STEPS");
        forced = e ? atoi(e) : 0;
    }
    {   // Game speed: each frame should take its interval (the game's mode: 1 = its 60 fps mode, 2 = its 30) times a
        // vsync of real time; the world steps per frame are fixed, so speed = that time / the time frames really took.
        // Logged every 2 s ("[speed]"); g_game_speed for the fps line.
        static uint64_t t_last, t_start, frames;
        static double want_ns;
        uint64_t t = plat_now_ns();
        if (t_last && t - t_last < 250000000ull) want_ns += interval * 16666667.0, frames++;  // (a load / stall: not counted)
        else if (t_last) t_start += t - t_last;  // (its time left out too)
        if (!t_start) t_start = t;
        t_last = t;
        if (t - t_start >= 2000000000ull && frames) {
            g_game_speed = want_ns / (double)(t - t_start);
            if (ENV("LARECOMP_LOG_SPEED")) LOG("[speed] %.1f%% (interval %u, %llu frames)\n", g_game_speed * 100, interval, (unsigned long long)frames);
            t_start = t, want_ns = 0, frames = 0;
        }
        g_game_interval = interval;
    }
    static int ncall;
    if (ENV("LARECOMP_LOG_FRAMEPACE") && ncall++ % 120 == 0) LOG("[framepace] call %d interval %u vsync %llu\n", ncall, interval, (unsigned long long)g_vsync_count);
    if (forced > 0) return (uint32_t)forced;
    static uint64_t last;
    uint64_t now = __atomic_load_n(&g_vsync_count, __ATOMIC_RELAXED);
    uint64_t steps = last ? now - last : 1;
    last = now;
    if (!on) return interval;
    if (steps < interval) steps = interval;  // (the game's own 30 fps mode: it waits that many vsyncs anyway)
    if (steps < 1) steps = 1;
    if (steps > (uint64_t)max_steps) steps = (uint64_t)max_steps;
    if (steps > 1) g_frame_steps_late++;
    return (uint32_t)steps;
}

// The game's next swap interval (less one: 0 = its 60 fps mode), as its governor chose it (tools/patches.py
// INTERVAL_CHOICE_SITE: from the vsyncs the last frame took, one fewer when its CPU and GPU times were short).
// g_pace_slow: the renderer's pacer found our frames don't fit 60 (nvn/pace.c) - then its tries at 60 are
// declined: each would be a frame of slow motion (one world step in two vsyncs).
// LARECOMP_LOG_INTERVAL=1: each change, with its inputs.
volatile bool g_pace_slow;
uint32_t rt_interval_choice(uint32_t v, uint64_t obj, uint64_t gpu, uint64_t cpu) {
    static int log = -1;
    if (log < 0) log = ENV("LARECOMP_LOG_INTERVAL") != NULL;
    uint32_t lo = LD8(obj + 0x4e2), hi = LD8(obj + 0x4e3), was = LD8(obj + 0x4e1), out = v;
    if (g_pace_slow && v == 0 && hi >= 1) out = 1;
    // (the Frame rate setting: its 60 fps mode whenever its range has it, or its 30)
    if (g_settings.frame_rate == FRAME_RATE_NATIVE_60 && lo == 0) out = 0;
    else if (g_settings.frame_rate == FRAME_RATE_FIXED_30 && hi >= 1 && out == 0) out = 1;
    // (Never past its range: Link walks 3 steps a frame at interval 3, but other things don't take it - Madam
    // MeowMeow's little chain chomps' chains flew apart. Below 30 fps the resolution gives way instead.)
    static int force = -1;  // LARECOMP_FORCE_INTERVAL=<n> (test): always n
    if (force < 0) force = ENV("LARECOMP_FORCE_INTERVAL") ? atoi(ENV("LARECOMP_FORCE_INTERVAL")) : 0;
    if (force > 0) out = (uint32_t)force - 1;
    static int linklog = -1;  // LARECOMP_LOG_LINKSPEED=1 (test): Link's speed, units per real second, each second
    if (linklog < 0) linklog = ENV("LARECOMP_LOG_LINKSPEED") != NULL;
    if (linklog) {
        static uint64_t t0, frames, fr;
        static float x0, z0;
        extern uint64_t doorjump_player(uint64_t frame);
        uint64_t pc = doorjump_player(++fr), t = plat_now_ns();
        if (pc) {
            const float *pos = (const float *)(uintptr_t)(pc + 0x80);
            frames++;
            if (!t0) t0 = t, x0 = pos[0], z0 = pos[2], frames = 0;
            else if (t - t0 >= 1000000000ull) {
                float dx = pos[0] - x0, dz = pos[2] - z0;
                LOG("[linkspeed] %.3f units/s (%llu frames, interval %u)\n", sqrtf(dx * dx + dz * dz) / ((t - t0) / 1e9),
                    (unsigned long long)frames, out + 1);
                t0 = t, x0 = pos[0], z0 = pos[2], frames = 0;
            }
        }
    }
    if (log && out != was)
        LOG("[interval] %u -> %u (game %u, range %u-%u, steps max %u, gpu %llu, cpu %llu, vsync %llu)\n", was + 1, out + 1, v + 1,
            lo + 1, hi + 1, (unsigned)LD8(obj + 0x649), (unsigned long long)gpu, (unsigned long long)cpu, (unsigned long long)g_vsync_count);
    return out;
}
