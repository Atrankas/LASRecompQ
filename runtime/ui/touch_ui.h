// Touch screens in the ImGui pages (the start screen, the menu): a finger is the mouse, but a list scrolls under a
// dragged finger as on a phone - a drag past a few pixels scrolls (and clicks nothing), a tap clicks where it lifts,
// and a flick keeps the page going for a moment. ImGui alone would press on touch-down and scroll only by its thin
// scrollbar: long pages (Settings) were out of reach and a drag that started on a row pressed it.
// Driven from the thread that renders the page: event() for each finger event, frame() once before each ImGui frame.
// The drag goes out as one scroll per frame: ImGui takes queued input one wheel event a frame, and a finger reports
// several moves a frame - the page fell further and further behind it.
#pragma once
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_timer.h>
#include <float.h>
#include <math.h>

struct TouchUi {
    SDL_FingerID id = 0;
    bool active = false, drag = false, flinging = false;
    float x0 = 0, y0 = 0, last_x = 0, last_y = 0, pending = 0, vel = 0;  // vel: px / s (the flick)
    uint64_t last_t = 0;

    // pos(x, y), button(down): what the finger means, in window pixels (w x h)
    template <typename Pos, typename Button>
    void event(const SDL_Event *e, float w, float h, Pos pos, Button button) {
        float x = e->tfinger.x * w, y = e->tfinger.y * h;
        uint64_t t = e->tfinger.timestamp;
        if (e->type == SDL_EVENT_FINGER_DOWN) {
            if (active) return;  // (a second finger: ignored)
            id = e->tfinger.fingerID, active = true, drag = false, flinging = false, pending = 0, vel = 0;
            x0 = last_x = x, y0 = last_y = y, last_t = t;
            pos(x, y);
            return;
        }
        if (!active || e->tfinger.fingerID != id) return;
        if (e->type == SDL_EVENT_FINGER_MOTION) {
            float slop = h * 0.015f;  // (~16 px on a 1080-line screen)
            if (!drag && (fabsf(y - y0) > slop || fabsf(x - x0) > slop)) drag = true, last_y = y;
            if (drag) {
                pending += y - last_y;
                if (t > last_t) {
                    float v = (y - last_y) / ((t - last_t) / 1e9f);
                    vel = vel * 0.6f + v * 0.4f;  // (smoothed over the last few moves)
                }
                last_y = y, last_x = x, last_t = t;
                pos(x, y);
            }
            return;
        }
        if (e->type == SDL_EVENT_FINGER_UP && !drag) {  // a tap: a click where it lifted
            pos(x, y);
            button(true);
            button(false);
        }
        active = false;
        // a flick: the page keeps going (the hover stays where the finger was, so the page under it scrolls)
        flinging = drag && e->type == SDL_EVENT_FINGER_UP && fabsf(vel) > 300 && SDL_GetTicksNS() - last_t < 80000000ull;
        if (!flinging) pos(-FLT_MAX, -FLT_MAX);  // (no hover left behind)
    }

    // once a frame: the scroll to apply now (pixels, + = content down), the flick carried on
    template <typename Pos>
    float frame(float dt, Pos pos) {
        float px = pending;
        pending = 0;
        if (flinging) {
            px += vel * dt;
            vel *= expf(-dt * 4.0f);  // (~a quarter of a second to settle)
            if (fabsf(vel) < 60) flinging = false, pos(-FLT_MAX, -FLT_MAX);
        }
        return px;
    }
};
