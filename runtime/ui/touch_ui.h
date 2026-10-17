// Touch screens in the ImGui pages (the start screen, the menu): a finger is the mouse, but a list scrolls under a
// dragged finger as on a phone - a drag past a few pixels scrolls (and clicks nothing), a tap clicks where it lifts.
// ImGui alone would press on touch-down and scroll only by its thin scrollbar: long pages (Settings) were out of
// reach and a drag that started on a row pressed it.
#pragma once
#include <SDL3/SDL_events.h>
#include <float.h>
#include <math.h>

struct TouchUi {
    SDL_FingerID id = 0;
    bool active = false, drag = false;
    float x0 = 0, y0 = 0, last_y = 0;

    // pos(x, y), button(down), scroll(pixels): what the finger means, in window pixels (w x h)
    template <typename Pos, typename Button, typename Scroll>
    void event(const SDL_Event *e, float w, float h, Pos pos, Button button, Scroll scroll) {
        float x = e->tfinger.x * w, y = e->tfinger.y * h;
        if (e->type == SDL_EVENT_FINGER_DOWN) {
            if (active) return;  // (a second finger: ignored)
            id = e->tfinger.fingerID, active = true, drag = false, x0 = x, y0 = y, last_y = y;
            pos(x, y);
            return;
        }
        if (!active || e->tfinger.fingerID != id) return;
        if (e->type == SDL_EVENT_FINGER_MOTION) {
            float slop = h * 0.015f;  // (~16 px on a 1080-line screen)
            if (!drag && (fabsf(y - y0) > slop || fabsf(x - x0) > slop)) drag = true;
            if (drag) {
                pos(x, y);
                scroll(y - last_y);
                last_y = y;
            }
            return;
        }
        if (e->type == SDL_EVENT_FINGER_UP && !drag) {  // a tap: a click where it lifted
            pos(x, y);
            button(true);
            button(false);
        }
        pos(-FLT_MAX, -FLT_MAX);  // (no hover left behind)
        active = false;
    }
};
