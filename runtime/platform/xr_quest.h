// Meta Quest (Horizon OS): the game inside an OpenXR session (runtime/platform/xr_quest.c).
//
// Started by the "LAS Recomp VR" launcher entry (LasVrActivity, AndroidManifest.xml), which sets LARECOMP_XR=1. The
// game renders as usual into a Vulkan swapchain, but that swapchain is made on an Android Surface the OpenXR runtime
// gives us (XR_KHR_android_surface_swapchain) and shown by the compositor as a panel in front of the player:
//
//   VR_MODE_FLAT      one quad layer with the picture (the port's usual flat screen, in VR)
//   VR_MODE_CURVED    the same picture on a fan of quads wrapping around the player (a curved cinema screen)
//   VR_MODE_STEREO    the two halves of the picture are one eye each, warped per eye by the scene's depth
//                     (runtime/nvn/vulkan/vr_stereo.c) and with their own sub-rect of the panel: a 3D window
//   VR_MODE_DIORAMA   the same stereo pair, tilted back at eye distance like a model on a table
//
// The player's own settings decide all of it (Video tab "VR (Meta Quest)", or larecomp.ini: vr_mode, vr_panel,
// vr_distance, vr_size, vr_curve, vr_tilt, vr_depth, vr_refresh). Everything is logged to logcat with the tag
// LASRecomp-XR; docs/vr-quest.md has the whole picture (building, installing, using).
//
// The Touch controllers are read through OpenXR actions and fed to the game as an SDL virtual gamepad, so the game's
// own button bindings, its prompts and its menus work with them untouched (the mapping is in xr_quest.c; the app's
// menus additionally get a ray pointer from the right controller, which is what menu_vr_pointer feeds).
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ANativeWindow;

// The curved mode's screen is a fan of this many quads along an arc (core OpenXR layers only: no cylinder extension
// to depend on). Six of them over 80 degrees are smooth enough that the seams are not visible.
#define XR_QUEST_ARC_SEGMENTS 6

// LARECOMP_XR=1: the app was started as the VR entry
bool xr_quest_requested(void);
// Starts OpenXR on the first call and returns the Android window the compositor shows as the game's panel, or NULL
// when OpenXR could not start (the caller then uses the activity's own window as usual). w/h: the window the app asked
// for - a VR_PANEL_NATIVE settings value uses those pixels per eye, the fixed presets override them.
struct ANativeWindow *xr_quest_window(int w, int h);
// the panel's size in pixels (false: OpenXR isn't running). Stereo modes: both eyes' halves together.
bool xr_quest_extent(uint32_t *w, uint32_t *h);
bool xr_quest_active(void);
// device.c: the swapchain's real size once it was made (the runtime may have given the surface another size than the
// one asked for; the layers' sub-rects follow this, so the two eyes' halves always land where they should)
void xr_quest_note_panel(uint32_t w, uint32_t h);

// The stereo modes (VR_MODE_STEREO / VR_MODE_DIORAMA) are on: the game's window is the left eye's half of the panel,
// the warping fills both halves (vr_stereo.c), and frame generation stands down (framegen.c cannot make an in-between
// frame for a stereo pair: the two halves have to be the same instant).
bool vr_stereo_on(void);
// Halves w (the stereo modes: the game and the menus are drawn into one eye's half of the panel); device.c calls this
// before the frame's presentation size is worked out. Nothing when the stereo modes are off.
void vr_stereo_split_panel(double *w, double *h);
// The distance / size / tilt / stereo strength that suit a mode, written into g_settings. The menu applies it when the
// mode row is changed; every one of them can then be changed on its own.
void vr_mode_preset(int mode);

#ifdef __cplusplus
}
#endif
