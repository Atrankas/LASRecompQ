// Meta Quest (Horizon OS): the game inside an OpenXR session (runtime/platform/xr_quest.c).
// Started by the "LAS Recomp VR" launcher entry (LasVrActivity, AndroidManifest.xml), or on a headset by itself
// (LasActivity.java sets LARECOMP_XR=1). Everywhere else (phones, the flat Quest entry) none of this runs.
//
// The game's picture is a panel floating in front of the player, in the space the session starts in. Its size and
// place are live: the game's own menu ("Headset panel", runtime/ui/menu.mm) and the Touch controllers themselves
// (with that section's "Adjust with the controllers" on: the sticks move and resize it) change them while you play.
// The Touch controllers are an SDL virtual gamepad the game reads like any other - its menus, its bindings and its
// rumble work with it unchanged - laid out like a Switch's own controller (see pad_update).
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ANativeWindow;

// The panel's size and place, in metres, in the space the session starts in (LOCAL: the head's place at the start, y
// up, -z ahead): the picture's width, how far ahead of the player its centre hangs, and that centre's offsets to the
// right and up from straight ahead. The game's window is scaled to the panel, so a wider panel is a bigger picture,
// never a different shape.
typedef struct {
    float width, distance, off_x, off_y;
} XrQuestPanel;

// What a fresh install shows: 3.2 m wide, 2.2 m ahead (about 70 degrees of view), 10 cm below eye level.
void xr_quest_panel_defaults(XrQuestPanel *out);
// The ranges the menu's rows (and xr_quest_panel_set) clamp to
#define XR_PANEL_MIN_WIDTH 0.6f
#define XR_PANEL_MAX_WIDTH 20.f
#define XR_PANEL_MIN_DISTANCE 0.4f
#define XR_PANEL_MAX_DISTANCE 12.f
#define XR_PANEL_MAX_OFF 6.f

// LARECOMP_XR=1: the app was started as the VR entry
bool xr_quest_requested(void);
// Starts OpenXR on the first call and returns the Android window the compositor shows as the game's panel (w x h
// pixels), or NULL when OpenXR could not start (the caller then uses the activity's own window as usual)
struct ANativeWindow *xr_quest_window(int w, int h);
// the panel's size in pixels (false: OpenXR isn't running)
bool xr_quest_extent(uint32_t *w, uint32_t *h);
bool xr_quest_active(void);
// device.c lifecycle_watch: an OpenXR panel is the picture, so the app's Android window going away (an immersive
// activity may have no surface of its own) must not stop the game - only the session ending does. The frame loop sets
// this before it pushes the background event (STOPPING / EXITING / LOSS_PENDING) and clears it when a session begins.
bool xr_quest_wants_pause(void);
// The panel's size and place as they are now (false: OpenXR isn't running). Read it every frame - the controllers
// move the panel too, and this is where the menu's rows take their value from.
bool xr_quest_panel(XrQuestPanel *out);
// Sets them (clamped to the ranges above). Applied to the next composed frame; stored whether OpenXR is running yet
// or not (the settings are loaded before the session starts).
void xr_quest_panel_set(const XrQuestPanel *p);
// "Adjust with the controllers": while on, the two sticks move the panel (right stick: across and up, left stick:
// closer / further and bigger / smaller) and the game's own sticks stay centred, so Link holds still. The buttons
// keep working (the game's menu, to turn it off again, is Minus + Plus as always).
bool xr_quest_adjusting(void);
void xr_quest_adjust_set(bool on);

#ifdef __cplusplus
}
#endif
