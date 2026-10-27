# Meta Quest (Horizon OS): the game in VR

The app has a second launcher entry, **LAS Recomp VR**, that runs the game inside an OpenXR session instead of a flat
window: the game's picture becomes a panel floating in front of you, the Touch controllers act as a gamepad, and the
panel can be a cinema screen or a stereo window with depth. The flat entry (**LAS Recomp**) is unchanged and every
setting here is ignored outside it.

| file | what it is |
| --- | --- |
| `runtime/platform/xr_quest.c` | the OpenXR instance, session, the panel's swapchain (an Android Surface), the layers, the Touch controllers, the menus' ray pointer |
| `runtime/nvn/vulkan/vr_stereo.c` | the stereo modes: each eye's half of the panel, warped by the scene's depth |
| `android/app/src/main/AndroidManifest.xml` | the `LasVrActivity` alias (the VR launcher entry, VR intent filters) |
| `android/app/src/main/java/com/lasrecomp/android/LasActivity.java` | sets `LARECOMP_XR=1` when started through that entry |
| `runtime/settings.{c,h}`, `runtime/ui/menu.mm` | the settings and their rows (Video tab, "VR (Meta Quest)") |

## Modes

Video > **VR mode** picks what the picture is. Changing it also sets the distance, size, tilt and stereo depth to values
that suit it; each can then be changed on its own.

| mode | what it does | cost |
| --- | --- | --- |
| Cinema (flat) | one quad layer with the picture, at the configured distance and size | the cheapest |
| Cinema (curved) | the same picture on a fan of quads wrapping around you (`XR_QUEST_ARC_SEGMENTS`, core OpenXR layers: no cylinder extension to depend on) | the same |
| Window (3D depth) | each eye gets its own half of the panel, displaced per pixel by the scene's depth: the picture reads as a window with real depth | + one full-screen pass per eye |
| Diorama (table) | the same stereo pair, leaned back and hung lower: the world reads as a model on a table | as Window |

The stereo modes work on the picture the game already drew, not by moving the game's camera: the game's own
view-projection, the depth buffer and the picture before the HUD are the inputs (the same ones frame generation
reprojects with), and each eye's pixels are resampled from them. Consequences worth knowing:

- Characters keep their 2D shapes as the source, so depth is a per-pixel displacement, not a new render: it looks right
  for the scenery, walls and platforms, and softer on sprites.
- **Frame generation is off while a stereo mode is on** (`fg_enabled()`): the two halves have to be the same instant.
- No mode moves the game's camera (the game's projection lives in the recompiled code). The diorama is a stereo window
  tilted back and hung low - an orthographic or isometric camera would need a re-implementation of the game's own
  projection, which this runtime does not do.
- A negative **VR depth** swaps the eyes: use it if the scenery reads as a hole instead of a model.

## Resolution

Video > **VR panel resolution** is the panel's pixels per eye, and the resolution the game renders at (keep Internal
resolution at Match display). Used from the next start of the app.

| setting | per eye | stereo modes' panel |
| --- | --- | --- |
| 720p | 1280x720 | 2560x720 |
| 1080p (default) | 1920x1080 | 3840x1080 |
| 1440p | 2560x1440 | 5120x1440 |
| Native | the window's own pixels, capped at 3840x2160 | 2x that wide |

Before this, the panel was the headset's own window (a Quest 3 reports a window wider than 4000 px), and the game
rendered at that size twice over in the stereo modes. 1080p per eye is the default: it fits the headset's GPU with room
for 60 frames a second.

## Touch controllers

Read through OpenXR actions (`/interaction_profiles/oculus/touch_controller`) and fed to the game as an **SDL virtual
gamepad** (an Xbox One pad's ids, which the runtime's input layer ranks above the headset's own half pad). The game's
own bindings, its prompts and its menus therefore work with them unchanged - rebind them in Controls like any pad.

| controller | the game |
| --- | --- |
| left stick | movement |
| right stick | the d-pad (the game's menus, and left / right / up / down in the app's own menus) |
| A | A (confirm, talk, lift) |
| B | B (sword) |
| X / Y | X / Y |
| left grip / right grip | L / R (shield, Pegasus Boots) |
| left trigger / right trigger | ZL / ZR (analog) |
| left stick click | Minus (map) |
| menu button | Plus (items / pause) |
| right stick click | the app's settings menu |
| menu + left stick click | the app's settings menu too |
| right trigger (ray at the panel) | click in the app's menus, while they are open |

The right controller also aims a ray at the panel: while a menu is open (`menu_input_captured`), the ray's hit becomes
the pointer and the right trigger is the click. The game gets no input while a menu is open, so the same trigger cannot
do two things at once.

## Building and installing

Needs the Android SDK (platform 36), NDK 27.2 and JDK 17+. On Ubuntu / WSL:

    # once
    tools/android/build_deps.sh                       # FFmpeg, shaderc, Vulkan headers, VMA, volk, SDL3
    tools/android/ondevice/build_all.sh               # the on-phone builder (release APK: -PonDeviceBuild=true)

    # every build: the app (the runtime, SDL3, Vulkan, OpenXR loader)
    cd android && ./gradlew assembleRelease -PonDeviceBuild=true

    # or a personal APK with the game code inside (needs the game's exefs)
    tools/android/build_game.sh <game folder> --apk

Release builds signed with your own key (so a new APK replaces the installed app instead of being refused):

    # once: android/keystore.properties (gitignored) + the key it names
    keytool -genkeypair -v -keystore android/lasrelease.jks -alias lasrecomp \
            -keyalg RSA -keysize 4096 -validity 10000 -storetype PKCS12

    # android/keystore.properties
    storeFile=../lasrelease.jks
    storePassword=<the password>
    keyAlias=lasrecomp
    keyPassword=<the password>

`android/app/build.gradle` reads that file in its `signingConfigs` block: with it, `assembleRelease` is signed with your
key; without it, release builds use the local debug key (fine for your own device as long as you always use the same
one - a change of key means uninstalling the app, and the app's data folder goes with it).

Install over USB with developer mode on (Settings > System > Developer in the headset):

    adb install -r android/app/build/outputs/apk/release/app-release.apk

Then start **LAS Recomp VR** from *Unknown Sources* / the app library. Without the library entry:

    adb shell am start -n com.lasrecomp.android/.LasVrActivity

## Settings file

`Video` tab rows, or `larecomp.ini` in the app's data folder (`Android/data/com.lasrecomp.android/files/`):

    vr_mode     = 0        # 0 flat, 1 curved, 2 window (3D depth), 3 diorama
    vr_panel    = 1        # 0 720p, 1 1080p, 2 1440p, 3 native (per eye)
    vr_refresh  = 0        # 0 the headset's own, else 72 / 90 / 120 Hz (XR_FB_display_refresh_rate)
    vr_distance = 2.20     # metres to the panel
    vr_size     = 3.20     # the panel's width in metres
    vr_curve    = 80.0     # the curved mode's arc, degrees
    vr_tilt     = 0.0      # degrees leaned back (the diorama preset uses 42)
    vr_depth    = 100      # stereo eye separation, percent (negative swaps the eyes)

## Logging and environment

Everything the VR side does goes to logcat with the tag `LASRecomp-XR`:

    adb logcat -s LASRecomp-XR

| variable | effect |
| --- | --- |
| `LARECOMP_XR=1` | start in OpenXR (what the `LasVrActivity` entry sets) |
| `LARECOMP_XR_PASSTHROUGH=0` | never create the passthrough layer (black background instead of your room) |

Put them in `env.txt` in the app's data folder (one `NAME=value` per line), as the other `LARECOMP_*` variables.

The stereo warping logs its inputs once in a while (`[vr] stereo: P00 ..., HUD mask ...`) and says why it fell back to
both halves showing the same picture (`[vr] no stereo this frame (picture ..., depth ..., camera ...)`): a frame with no
scene, no depth or no camera keeps the panel flat rather than showing a broken one.
