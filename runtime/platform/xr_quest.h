// Meta Quest (Horizon OS): the game inside an OpenXR session (runtime/platform/xr_quest.c).
// Started by the "LAS Recomp VR" launcher entry (LasVrActivity, AndroidManifest.xml), which sets LARECOMP_XR=1.
// Phase A: the game's picture is a panel floating in front of the player (over passthrough), and the Touch controllers
// are a virtual gamepad the game reads like any other. Everywhere else (phones, the flat Quest entry) none of this runs.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ANativeWindow;

// LARECOMP_XR=1: the app was started as the VR entry
bool xr_quest_requested(void);
// Starts OpenXR on the first call and returns the Android window the compositor shows as the game's panel (w x h
// pixels), or NULL when OpenXR could not start (the caller then uses the activity's own window as usual)
struct ANativeWindow *xr_quest_window(int w, int h);
// the panel's size in pixels (false: OpenXR isn't running)
bool xr_quest_extent(uint32_t *w, uint32_t *h);
bool xr_quest_active(void);

#ifdef __cplusplus
}
#endif
