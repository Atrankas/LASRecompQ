// What only the Android app has (runtime/platform/android_jni.c, with LasActivity.java): the system's folder picker and
// the copy of the player's game files out of the picked folder.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Opens the folder picker; cb gets the picked folder's name (shown on the start screen), or NULL if none, on the UI
// thread.
void android_pick_game_folder(void (*cb)(const char *name));
// The folder picked in an earlier session (still readable), or NULL
const char *android_saved_game_folder(void);
// Copies exefs/ and romfs/ out of the picked folder into dest (blocking; any thread). build_id: exefs/main's at 0x40
// (16 hex digits) - another version is refused before anything is copied. NULL = done, else what went wrong:
// "nofolder", "notfound", "unusable", "version", "space:<bytes>", "copy:<details>" (LasActivity.importGameFiles).
const char *android_import_game_files(const char *dest, const char *build_id,
                                      void (*progress)(uint64_t done, uint64_t total));

// ---- building the game code on the phone (runtime/gamebuild.c; docs/android-ondevice-build.md)
// The generator, its Python and the compiler's NDK pieces unpacked from the APK's assets into the app's internal
// storage (once per app version; blocking): that folder, or NULL (an APK without them, or no space)
const char *android_build_tools(void);
// The build's foreground service (keeps the app running with the screen off: wake lock, a notification); on = false
// ends it. text: what the notification says (any thread).
void android_build_service(bool on, const char *title);
void android_build_progress(int percent, const char *text);

// ---- a custom Vulkan driver (Mesa Turnip on Adreno ...: GpuDrivers.java), loaded by runtime/nvn/vulkan/device.c
bool android_gpu_driver_supported(void);  // an Adreno GPU
const char *android_gpu_driver_name(void);  // the installed driver's name, or NULL
bool android_gpu_driver_selected(void);
void android_gpu_driver_select(bool use);  // from the next start on
// Opens the file picker for a driver .zip, installs and selects it; cb (any thread) gets NULL when installed, "" when
// cancelled, else what went wrong
void android_gpu_driver_install(void (*cb)(const char *error));
// The folder picker for a fan translation as extracted; its romfs/ (the folder holding regionUS/) is copied into dest
// on a thread of the app's, then cb (on that thread): NULL = done, else "nofolder", "notfound" or "copy:<details>".
void android_import_translation(const char *dest, void (*cb)(const char *error));
// The name of a driver that failed at its last start (the system's runs instead), once; else NULL
const char *android_gpu_driver_failed(void);
// The selected driver for this start: false = the system's. Folders end with '/'. probe: a file to remove once the
// driver works (left behind, the next start runs the system driver).
typedef struct { char dir[512], lib[128], hook_dir[512], tmp_dir[512], probe[512]; } AndroidGpuDriver;
bool android_gpu_driver_config(AndroidGpuDriver *out);

#ifdef __cplusplus
}
#endif
