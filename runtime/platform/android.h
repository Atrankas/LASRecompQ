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
// Copies exefs/ and romfs/ out of the picked folder into dest (blocking; any thread). NULL = done, else what went
// wrong: "nofolder", "notfound", "unusable", "space:<bytes>", "copy:<details>" (LasActivity.importGameFiles).
const char *android_import_game_files(const char *dest, void (*progress)(uint64_t done, uint64_t total));

// ---- a custom Vulkan driver (Mesa Turnip on Adreno ...: GpuDrivers.java), loaded by runtime/nvn/vulkan/device.c
bool android_gpu_driver_supported(void);  // an Adreno GPU
const char *android_gpu_driver_name(void);  // the installed driver's name, or NULL
bool android_gpu_driver_selected(void);
void android_gpu_driver_select(bool use);  // from the next start on
// Opens the file picker for a driver .zip, installs and selects it; cb (any thread) gets NULL when installed, "" when
// cancelled, else what went wrong
void android_gpu_driver_install(void (*cb)(const char *error));
// The name of a driver that failed at its last start (the system's runs instead), once; else NULL
const char *android_gpu_driver_failed(void);
// The selected driver for this start: false = the system's. Folders end with '/'. probe: a file to remove once the
// driver works (left behind, the next start runs the system driver).
typedef struct { char dir[512], lib[128], hook_dir[512], tmp_dir[512], probe[512]; } AndroidGpuDriver;
bool android_gpu_driver_config(AndroidGpuDriver *out);

#ifdef __cplusplus
}
#endif
