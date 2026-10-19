# Building the game code on the phone

A player installs the APK, picks their own extracted game folder (`exefs/` + `romfs/`), and the app itself copies
it, generates, compiles and links `liblasgame.so`, and starts the game - the way the desktop start screen already
does (`runtime/gamebuild.c` with `zig cc` and the PyInstaller'd generator). A computer (`tools/android/build_game.sh`)
is still the faster way, and makes the same library (same game code id).

## The public APK

    tools/android/ondevice/build_all.sh                       # once, and after a Python/zig/capstone/unicorn change
    cd android && ./gradlew assembleRelease -PonDeviceBuild=true   # (no -PlasGameLib: no game code inside)
    tools/android/ondevice/check_public_apk.sh android/app/build/outputs/apk/release/app-release.apk

- `build_all.sh` (on the computer, NDK 27.2 from `android/local.properties`): `build_python.sh`, `build_zig.sh`,
  `build_cckit.sh`, `make_assets.sh` → `build/ondevice/stage/jniLibs/arm64-v8a/` (libzig.so, liblaspython.so,
  libpython3.14.so, libcapstone.so, libunicorn.so) and `build/ondevice/assets/` (ondevice.zip, licenses/). Downloads
  are checked against published sha256 (python.org's release-file API, PyPI, tools/fetch_zig.sh's pins, the license
  texts' pinned hashes).
- `-PonDeviceBuild=true` (android/app/build.gradle) adds those to the APK, sets `android:extractNativeLibs="true"` and
  `useLegacyPackaging` (the programs must be files to run), and re-runs `build_cckit.sh` + `make_assets.sh` before
  each build so the generator and headers inside are this checkout's (the game code id the app is compiled with is
  made from the same files). Without the property the APK is as before (dev builds stay small; a phone then needs a
  library from a computer).
- `check_public_apk.sh` fails if the APK has `liblasgame.so`, a library defining recompiled game functions
  (`f_70xxxxxxxx`), generated C (also inside ondevice.zip) or game files; it lists the native libraries and licenses.
  Checked: the release APK built this way (76 MB) passes; a personal APK made with `build_game.sh --apk` fails
  (liblasgame.so).
- Licenses: `assets/licenses/` in the APK (README.txt lists each component, version, license and source; unicorn's
  GPL-2.0 `COPYING` and LLVM's license fetched from their repositories at the pinned tags `2.1.4` / `llvmorg-22.1.8`).

## What the app does (first start without the game code)

1. Start screen, setup page (as before): "Game files folder" opens the system folder picker
   (`ACTION_OPEN_DOCUMENT_TREE`), "Set up game files" starts the setup (runtime/ui/launcher.mm `extract_thread`).
2. `LasActivity.importGameFiles` finds exefs/ (main + main.npdm) and romfs/ (region_common) in the picked tree, reads
   `exefs/main`'s build id at 0x40 and refuses anything but v1.0.0 **before** copying ("Wrong version of the game"),
   then copies both into the data folder (`Android/data/com.lasrecomp.android/files/extracted/`; the game reads romfs/
   in place, runtime/services/fs.c), resuming a partial copy.
3. If no matching `liblasgame.so` is installed and the APK carries the builder (`gamebuild_available()`: libzig.so and
   liblaspython.so in the native library folder), `BuildService` (a foreground service, type dataSync) starts: a
   partial wake lock and a notification with the step, percent and time left, so the build continues with the screen
   off or another app in front. `gamebuild_run` (runtime/gamebuild.c, Android branch):
   - `LasActivity.buildTools` unpacks `assets/ondevice.zip` into `files/ondevice/` (once per app version);
   - the build thread (and so every compiler, which inherit it) goes to nice 10 and the faster cores (those whose
     `cpuinfo_max_freq` is above the slowest's: 4 of 8 on a Snapdragon 865); 4 compile jobs;
   - `liblaspython.so files/ondevice/tools/gen.py sdk ...`, then `... main ...` (one after the other: main alone
     peaks at ~1.7 GB), with `gamelib_id.sh --gen-args` (`--simd asm --shift 0x6000000000 --split-dispatch 512`);
     chunks are compiled as soon as they're written;
   - `libzig.so clang -target aarch64-linux-android30 <gamelib flags> -nostdinc -isystem cckit/...`, split-dispatch
     files with `-mllvm -disable-licm-promotion`;
   - `libzig.so ld.lld @cckit/link.rsp ... -L<native lib dir> -lmain ...`, published by the build cache, then copied to
     internal storage (`files/liblasgame.so` + `.id`, where runtime/gamelib.c looks first).
4. The progress overlay shows "Recompiling the game", the step, percent and "about N minutes left" (from the progress
   of the last few minutes); Cancel keeps the finished pieces. When the build ends the service stops and the game
   starts (PLAY as if pressed: the first-launch questions, then the game).
5. A build that was cancelled, failed or killed (the app swiped away, the phone rebooted) resumes: on the next start
   the start screen sees the copied files without game code and starts the build again by itself, reusing every
   finished object (runtime/build_cache.cpp; tested: a build killed partway reused what was done, a second run
   reused all 14 objects).

## Testing it

- `tools/android/ondevice/phone_test.sh [one generated .c]` - Python + modules, the compiler, a link against libmain.so,
  over adb in /data/local/tmp (no app).
- `tools/android/ondevice/phone_gamebuild_test.sh` - **the real `gamebuild_run` from the app's libmain.so, on the
  phone**, without installing the app and without game files: a test program loads libmain.so from a folder laid out
  like the native library folder, `LARECOMP_ONDEVICE_DIR` points at an unpacked ondevice.zip whose gen.py is replaced
  by `fake_gen.py` (synthetic chunks in the generator's layout and progress protocol, one split-dispatch file). Result
  on the S20 FE: Python ran both generators in turn, zig compiled 15 objects and linked them against libmain.so, the
  library was installed and loaded (`id 5510c73a0430eb62, 240 + 240 functions`); killed partway and run again it
  resumed; run again it reused everything.
- The full flow with the real game files needs the APK installed (`adb install`) and the game folder on the phone:
  pick it on the start screen, Set up game files, wait (screen off is fine), the game starts. `lasrecomp.log` (data
  folder) has the `[gamebuild]` lines; `files/ondevice` and `extracted/.gamebuild/*.log` the details of a failure.

## Prototype findings

The rest of this document is the prototype's study: what was measured and why these choices.

## Recommendation

| Piece | Choice | Status |
|---|---|---|
| Python | **CPython 3.14's official Android release** (python.org "Android embeddable package", aarch64) + an 8 KB launcher program (`liblaspython.so`) | **Works on the phone.** capstone, unicorn and lz4 each used once (`selftest.py`), `gen.py --help` runs |
| capstone / unicorn | built from their PyPI sdists with the NDK (cmake), ARM64 / aarch64 only; the pure-Python bindings load them by name through ctypes | **works** |
| lz4 | the python-lz4 C extensions built against the Android libpython with the NDK (4 small `clang -shared` lines) | **works** (no pure-Python fallback needed) |
| Compiler + linker | **zig 0.17.0, aarch64-linux release** (the same pinned version as the desktop builds), run as `zig clang` / `zig ld.lld` | **Works on the phone.** Compiles generated C, links a bionic `.so` with `DT_NEEDED libmain.so`, and bionic loads it and calls into libmain/libc/compiler-rt |
| libc headers, crt, stubs, compiler-rt | a 1.1 MB "cckit" taken from the NDK (`build_cckit.sh`) | **works** |
| Driver | **`runtime/gamebuild.c`** (already in libmain.so, already drives the start screen's progress bar); Java only supplies the paths and keeps the process alive | **implemented** (Android branch: see "What the app does") |
| Packaging | programs and libraries as `lib*.so` in `jniLibs/arm64-v8a`, **`extractNativeLibs=true`** (so the programs exist as files Android lets the app run); stdlib + generator + cckit as one asset archive unpacked into `files/` | **implemented** (`-PonDeviceBuild=true`) |

The big cost is **compile time**: the prototype estimated 1-1.5 hours on this phone; with the generator's split
(below) and 4 jobs on the big cores, expect roughly 40-60 minutes (to be measured with a real build on the phone).
The generator, Python and linking are minor in comparison.

## Measurements

All Mac numbers: Apple M1 (8 cores). Phone: S20 FE; the phone was shared with another session running GPU
benchmarks, so phone numbers are noisy (repeat runs of the same thing varied by up to 2x).

### Sizes

| Item | On disk (uncompressed) | Deflated (≈ APK cost) |
|---|---|---|
| `libzig.so` (zig 0.17.0 aarch64-linux, static, clang/LLD 22.1.8) | 171.7 MB (164 MiB) | 56.3 MB |
| `libpython3.14.so` | 5.6 MB | 1.8 MB |
| `liblaspython.so` (launcher) | 8 KB | - |
| `libcapstone.so` (ARM64 only) | 1.1 MB | |
| `libunicorn.so` (aarch64 only) | 2.3 MB | |
| `pyhome/` (stdlib trimmed + lib-dynload + site-packages, with .pyc) | 30 MB (18 MB without .pyc) | 9.5 MB |
| `cckit/` (44 headers, crt, libc/libm/libdl stubs, compiler-rt builtins, gamelib sources, link.rsp) | 1.1 MB | |
| **Total** | **~211 MB** | **~69 MB** |

So the APK grows by about **48 MB** (measured: the release APK with `-PonDeviceBuild=true` is **76 MB**, a dev APK
without it 29 MB, a personal one with the game library inside 143 MB) and the installed footprint by about **210 MB** (the extracted native libraries + the unpacked asset archive),
plus the build's scratch space: generated C **445 MB** (719 files), objects **122 MB**, the library **~100 MB**. About
**0.7 GB of scratch** during a build; the C and the objects can be deleted afterwards (keeping them makes an update
rebuild incremental, as on the desktop).

zig is almost all of it. A custom clang+lld built with the NDK for an Android host (AArch64 target only, MinSizeRel,
one multicall binary) would likely be 50-70 MB (≈ 20-25 MB deflated) - not built in this prototype (an LLVM build is
1-2 hours on the M1); see "Remaining work".

### Python

| What | Mac (M1, Homebrew Python 3.14) | Phone (official Android 3.14.8) |
|---|---|---|
| `pybench.py` (pure-Python workload shaped like the generator: struct decoding, dict/set, f-strings, regex) | 1.81 s | 6.6-7.1 s (≈ **3.8x** slower); 8.9-10.6 s pinned to cpu7 |
| interpreter start + import capstone, unicorn, lz4 | - | 0.12 s with the shipped .pyc (0.87 s without them) |
| `gen.py --help` | - | 0.25 s |

The generator itself was **not** run on the phone (that would need the game's `exefs/` there). On the Mac with
Python 3.14 (instead of the 3.11 `build_game.sh` uses) it produced **byte-identical** C for both modules (`diff -r`
against `build/android/gen`, same game code id), in 6.6 s (sdk) + 27.4 s (main), **peak RSS 1.66 GB for main**.
Expected on the phone: about 3.8 x 34 s ≈ **2-2.5 minutes**, and it overlaps with compiling (gamebuild.c compiles
each chunk as soon as it is written).

Memory is the generator's real constraint on a 6 GB phone (MemAvailable was ~0.85-0.9 GB while the other session's
game ran): gamebuild.c starts both generators at once (~2.2 GB together). On Android run them **one after the other**
(sdk first: 0.4 GB), with compile jobs filling the gap.

### Compiler

One representative generated file, `main_081.c` (625 KB, the mean file size is ~620 KB), flags from
`tools/android/gamelib_id.sh --flags` (`-O2 ...`):

| Compiler / machine | Time |
|---|---|
| Mac, NDK r27c clang 18 (PGO+LTO-built) | 1.20 s |
| Mac, zig 0.17.0 clang 22 | 1.21 s |
| Phone, zig clang 22, default scheduling (big cores) | 3.0-6.2 s (≈ 2.5-5x the Mac) |
| Phone, pinned to cpu4 (A77) / cpu6 / cpu7 (prime, contended) | 4.3 s / 5.3 s / 9.2 s |
| Phone, pinned to cpu0 (A55 little core) | 16.3 s |
| Phone, 8 copies in parallel | 17.1 s for all 8 (Mac: ~2.5 s) → throughput ≈ **7x** below the M1 |
| Phone, `-O1` instead of `-O2` | 3.7 s vs 4.6 s (same session) |

Peak RSS of one compile: 105 MB. Linking: one chunk 0.03 s on the phone; the full library (719 objects, 100 MB) takes
2.7 s / 350 MB RSS with zig's ld.lld on the Mac, so expect ~10-20 s on the phone.

### Compile time of the whole library

On the Mac with zig and 8 jobs: **12 min 18 s wall, 55 CPU-minutes** (`-O2`); `-O1`: 11 min 6 s, 49 CPU-minutes - the
optimization level barely matters. Scaling by the phone's measured throughput (≈ 7x) gives **~80-90 minutes** on the
phone, before any thermal throttling. (On the Mac, build_game.sh hides this: it runs 8 NDK clangs, and the desktop
builds use `-O1`.)

The cause is a small set of functions. `sdk_055.c` (480 KB, but one function, `f_7004159a00`, with a
158,000-character `switch (c->pc)` that makes every instruction a label - the generator's `br_all_labels` case) takes
**110-250 s** alone on the M1 (`-O2`; the spread is machine load), **~128 s at `-O1`, 1 s at `-O0`**. `-ftime-report`:
**LICM 77%** and LoopLoadElimination 13% of the time (the dispatch switch turns the whole function into one loop, and
LICM's promotion of the `c->x[...]` accesses inside it is quadratic). `-mllvm -disable-licm-promotion` cuts it to
**34 s**. 158 generated files contain such a switch with more than 20,000 characters (both modules). On the phone this
one file alone would take on the order of 10-20 minutes of one big core.

Ways to fix it, best first:
1. **Done:** `gen.py --split-dispatch 512` puts every function with a jump table (all instructions as labels) of 512+
   instructions in a chunk file of its own whose first line is `/* lasgen: split-dispatch */`; build_game.sh and
   gamebuild.c compile those with `-mllvm -disable-licm-promotion` (`gamelib_id.sh --split-flags`), every other file
   with today's flags, so normal code keeps LICM's register promotion. 148 such files (72 sdk, 76 main; 810 chunks
   instead of 719). Mac full build, NDK clang, 8 jobs: **43.8 → 35.5 CPU-minutes** (wall 6m48s → 5m42s on a quiet
   machine); sdk_055's function alone 110-250 s → 34 s. The slowest files are now the split ones themselves (~30-90
   s on the M1 under load; LoopLoadElimination and the rest); `-O1` on top would make them 23 s but changes their
   code more - not done. The bulk of the time is now simply the 445 MB of C. The whole split library links with
   `-z defs` (nothing undefined) and is the same size class (94 MB).
2. A smaller dispatch for those functions (only the addresses an indirect branch can actually reach, when the
   analysis can bound them) - fewer labels, a smaller loop.
3. Ship the profile (`build/android/lasgame.profdata`, clang 18 format; clang 22 reads older formats) and let `-fprofile-use`
   steer the optimizer - optional, it is not a compile-time fix.

With (1), the total should fall well below an hour, but that needs measuring on the phone with a real build.

## Python on Android

**Chosen: the official CPython Android release** (`python-3.14.8-aarch64-linux-android.tar.gz` from python.org,
sha256 `11324313...cf9a33` from python.org's release-file API; PEP 738 made Android a supported platform in 3.13, and
python.org publishes binary packages from 3.14). It is an *embeddable* build: `prefix/lib/libpython3.14.so`, the
standard library and `lib-dynload/` extension modules built for API 24, no `python` program - an app is expected to
embed it.

Alternatives considered:
- **Chaquopy**: a Gradle plugin that embeds Python in the app's own process via Java; it wants to own the app's
  Python setup and its pip wheels repository, its runtime is tied to its plugin version, and it is designed for Python
  called from Java inside the app process. The generator wants a separate process (memory: 1.7 GB peak, freed at exit;
  a crash or the OOM killer then takes out the generator, not the app; and two at once is not possible in one
  interpreter without subinterpreters). Chaquopy is MIT since 2022 - not a licensing problem, just the wrong shape.
- **BeeWare Python-Android-support**: superseded by CPython's own Android support (it is where `Android/android.py`
  came from).
- **Building CPython ourselves with `Android/android.py`**: possible (it downloads its dependencies and NDK 27.3), but
  the release package is exactly that build, signed and published; build it ourselves only if we need a patched
  CPython.

The launcher, `tools/android/ondevice/pylauncher.c` → `liblaspython.so`: `Py_InitializeFromConfig` with an isolated
config, home from `$LASPY_HOME`, `sys.path` = `$LASPY_PATH` (tools/ and site-packages) + the standard library, hash seed
0 (as the desktop helper: `tools/lasgen.spec`), unbuffered stdio (gamebuild.c reads the progress lines from the log as
they come), RUNPATH `$ORIGIN` (it finds `libpython3.14.so` beside it in the native library folder, no
`LD_LIBRARY_PATH`). Command line as python's: `liblaspython.so gen.py sdk <nso> 0x7004000000 <out> --hle ... --simd asm
--shift 0x6000000000`.

Bindings: `sys.platform` is `android`, so unicorn's loader falls back to the plain name `libunicorn.so` and capstone
looks for `libcapstone.so` - both exactly the jniLibs names. Set `LIBCAPSTONE_PATH` / `LIBUNICORN_PATH` to the native
library folder. `lz4` is used only by `tools/nso.py` (`lz4.block.decompress`); its extension modules are built with
the NDK against the Android libpython (`build_python.sh`), so no change to the generator is needed.

Also checked: `/system/bin/linker64 <path>/liblaspython.so ...` runs the launcher, so a dynamically linked PIE program
could even live outside the native library folder (in `files/`). This does **not** work for zig: it is a static
non-PIE executable (`unexpected e_type: 2`), so zig must be in the native library folder.

## The compiler

**Chosen: zig 0.17.0's aarch64-linux release** (`zig-aarch64-linux-0.17.0.tar.xz`, sha256 `9e8d1166...234ff8`, pinned in
`tools/fetch_zig.sh`). It is one statically linked binary (musl inside), so it runs on Android unchanged - checked on
the phone: `zig version`, `zig clang --version` (clang 22.1.8), `zig ld.lld --version` (LLD 22.1.8). Used as
`zig clang` / `zig ld.lld` (the plain clang and lld drivers inside zig) rather than `zig cc`, so zig neither tries to
provide a libc for the Android target nor builds its own compiler_rt: **none of zig's `lib/` folder is needed** - the
phone test ran with nothing but `libzig.so` + cckit.

Compile (as gamebuild.c would spawn it):

    libzig.so clang -target aarch64-linux-android30 <gamelib flags> -nostdinc -isystem cckit/clang/include \
        -isystem cckit/sysroot/usr/include -isystem cckit/sysroot/usr/include/aarch64-linux-android \
        -Icckit/gamelib-src -c <chunk>.c -o <chunk>.o

`-target aarch64-linux-android30` matters beyond headers: it is what keeps `x18` reserved (Android's shadow call stack
register), PIC defaults and the Android ABI's details as the NDK has them.

Link (the line NDK clang runs for build_game.sh's link, from `-###`; flags in `cckit/link.rsp`):

    libzig.so ld.lld @cckit/link.rsp -o liblasgame.so cckit/sysroot/usr/lib/aarch64-linux-android/30/crtbegin_so.o \
        <objects> -L<nativeLibraryDir> -lmain -Lcckit/sysroot/usr/lib/aarch64-linux-android/30 \
        cckit/clang/lib/libclang_rt.builtins-aarch64-android.a -ldl -lc cckit/sysroot/.../30/crtend_so.o

`link.rsp`: `-EL --fix-cortex-a53-843419 -z now -z relro --hash-style=gnu --eh-frame-hdr -m aarch64linux -shared
-z max-page-size=16384 -Bsymbolic -soname liblasgame.so`. The compiler runtime comes from the NDK (clang 18's
builtins archive): it has the outline atomics clang 22 emits for Android (`__aarch64_cas8_acq_rel` etc.); the probe
uses one.

Proof that the result is a proper bionic library: `linkprobe.c` compiled and linked **on the phone** this way, then
`dlopen`ed by a test program built with the NDK, with the app's real `libmain.so` + `libSDL3.so` beside it: it calls
`plat_now_ns` (libmain), `memcpy` / `strlen` (libc) and an outline atomic (compiler-rt) → `linkprobe: counter 42, now
... ns -> ok`. A real generated chunk (`main_081.o`) also links on the phone.

zig's clang (22) is newer than the NDK's (18) - the objects differ from the computer-built library's, but the C is
the same and nothing in it depends on the compiler version. The game's speed with clang 22 code should be checked
on the phone once a full library is built this way (expected: about the same).

Alternatives considered:
- **The NDK's clang**: the NDK ships it only for macOS, Linux and Windows hosts.
- **Termux's clang/lld packages**: built for Android, but dynamically linked against Termux's own prefix
  (`/data/data/com.termux/files/usr/lib`: libLLVM, libclang-cpp, libc++_shared...; ~150 MB+), with paths baked in -
  more files and a relocation problem, no smaller.
- **Our own clang+lld built with the NDK**: the best long-term option for size (one program, AArch64 target only,
  static libc++: estimated 50-70 MB, a third of zig) and it would pin the same clang as the computer builds. Costs an
  LLVM build script and 1-2 hours of build per LLVM update. Recommended as the follow-up once the pipeline works with
  zig.

## Packaging

Android 10+ (`targetSdk >= 29`) does not let an app `execve` files in its data directories; it may run programs from
its **native library folder** (`ApplicationInfo.nativeLibraryDir`), which only exists as real files when native
libraries are extracted at install. So:

- `jniLibs/arm64-v8a/`: `libzig.so`, `liblaspython.so`, `libpython3.14.so`, `libcapstone.so`, `libunicorn.so` (all
  produced by `build_python.sh` / `build_zig.sh` into `build/ondevice/stage/jniLibs/arm64-v8a/`, added to
  `jniLibs.srcDirs` by `-PonDeviceBuild=true`).
- `android:extractNativeLibs="${extractNativeLibs}"` in the manifest and `useLegacyPackaging onDevice` in
  build.gradle: true with the builder (also extracts libmain.so, libSDL3.so, ... ~17 MB more on disk; the libraries
  are stored deflated in the APK), false without it (mapped from the APK, as before).
- One asset archive (`ondevice.zip`, 9.9 MB, 1351 files): `pyhome/` (stdlib + lib-dynload + site-packages), the generator
  (`tools/*.py`, `tools/rando/patch_variants.json`, `runtime/game_addresses/patches_v1_0_0.h`) and `cckit/`. Unpacked
  into `files/ondevice/` on first use (or when the app's version changes), like `runtime/paths.c android_resources`
  does for fonts. `lib-dynload/*.so` and the lz4 extension modules are loaded with `dlopen` from there - allowed (the
  app loads `files/liblasgame.so` the same way today).
- `-lmain` links against `<nativeLibraryDir>/libmain.so`, the very library that will load the result.
- The output goes to the app's internal storage (`files/liblasgame.so` + `.id`), where `runtime/gamelib.c` already looks
  first; the external data folder (where the game files are) is mounted without exec.

## Driver: runtime/gamebuild.c

gamebuild.c already did everything needed - both generators as child processes, their progress from the log,
compiling chunks while the generator writes, the resumable build cache, cancelling, the link, publishing with the
id - and is compiled into libmain.so and wired to the start screen (`runtime/ui/launcher.mm`), so its Android branch
is small (the Java side only unpacks the assets, copies the game files and runs the foreground service):

- `plat_spawn` / `plat_proc_wait` / `plat_proc_kill` (`runtime/platform/android.c`): posix_spawn as on Linux.
- `find_tools_android`: the native library folder from `dladdr` on libmain.so (an APK whose libraries aren't
  extracted - "base.apk!/lib/..." - has no builder: `gamebuild_available()` false); generator = `liblaspython.so` +
  `files/ondevice/tools/gen.py`, compiler = `libzig.so`, headers / entry / HLE list from `files/ondevice/cckit/gamelib-src/`.
- `compiler()`: `clang -target aarch64-linux-android30 -nostdinc -isystem <kit>...`; `add_cflags` drops gamelib_id.sh's
  `-Iruntime`; split-dispatch files (first line) get `GAMELIB_SPLIT_FLAGS`; the generator gets `GAMELIB_GEN_ARGS`
  (both from gamelib_id.sh through CMakeLists, also part of the build cache's spec).
- Generators one after the other (sdk first), progress weighted by their share of the C (main 78%, sdk 22%; also on
  desktop, where they still run together).
- nice 10 + the faster cores for the build thread (the compilers inherit both), `LASPY_*` / `LIB*_PATH` / `TMPDIR`
  in the environment.
- The link with `libzig.so ld.lld @cckit/link.rsp` against `<native lib dir>/libmain.so`; after `build_cache_publish`
  (which now accepts `liblasgame.so`) the library is copied to internal storage and the external copy removed.
- `LARECOMP_ONDEVICE_DIR` (tests) replaces the unpacked assets' folder, so `phone_gamebuild_test.sh` can run it
  without the app's Java side.

## Licenses of everything bundled

| Component | Version | License | Notes |
|---|---|---|---|
| CPython (libpython, stdlib) | 3.14.8 | PSF-2.0 | `LICENSE.python.txt` (from the package) also lists the bundled libraries' licenses |
| - libffi (inside `_ctypes`) | | MIT | |
| - zlib, bzip2, xz/liblzma, zstd, mpdecimal (lib-dynload modules) | | zlib / bzip2 (BSD-like) / 0BSD-PD / BSD-3 / BSD-2 | |
| - OpenSSL, SQLite | | Apache-2.0 / public domain | excluded by `build_python.sh` (`_ssl`, `_hashlib`, `_sqlite3` are not shipped) |
| Capstone (C library + bindings) | 5.0.9 | BSD-3-Clause | `LICENSE.capstone.txt` |
| Unicorn (C library) | 2.1.4 | **GPL-2.0** (QEMU-derived); Python bindings BSD | already the case for the desktop helper (README lists it); ship its license text and offer the source (the PyPI sdist we build from) |
| python-lz4 + LZ4 library | 4.4.5 | BSD-3-Clause / BSD-2-Clause | `LICENSE.python-lz4.txt` |
| zig | 0.17.0 | MIT; its LLVM/clang/lld: Apache-2.0 WITH LLVM-exception; musl (static, inside): MIT | `LICENSE.zig.txt` |
| NDK sysroot pieces (bionic headers, crtbegin/crtend, libc/libm/libdl stubs) | r27c | BSD / Apache-2.0 (AOSP) | |
| compiler-rt builtins (NDK) | clang 18 | Apache-2.0 WITH LLVM-exception | |

## Scripts (tools/android/ondevice/)

Run on the computer, in this order (all downloads go to `build/ondevice/<name>-dl/`, checked against published
sha256: python.org's release-file API, PyPI's JSON, tools/fetch_zig.sh's pins):

1. `build_python.sh` - Python for the phone: fetches the CPython Android package and the capstone / unicorn / lz4
   sdists, builds `libcapstone.so` (ARM64 only) and `libunicorn.so` (aarch64 only) with the NDK's cmake toolchain
   (unicorn's QEMU configure wants pkg-config: `PKG_CONFIG=/usr/bin/false`), the launcher, the lz4 extension modules,
   a trimmed stdlib (no tests, tkinter, idlelib, ensurepip, venv, sqlite3, ssl, ...) with .pyc compiled by the
   computer's python3.14 (`unchecked-hash`, so unpacking doesn't invalidate them). ~20 s.
2. `build_zig.sh` - `libzig.so` (zig 0.17.0 aarch64-linux) and the computer's zig of the same version
   (`build/ondevice/zig-host`).
3. `build_cckit.sh` - the cckit: the exact headers the generated C and gamelib_entry.c include (dependency lists of a
   probe compiled with the host zig, so they are clang 22's own headers), crt + stubs + builtins from the NDK, the
   gamelib sources, link.rsp.
4. `make_assets.sh` - `build/ondevice/assets/ondevice.zip` (deterministic: fixed dates, sorted) and `licenses/`.
   `build_all.sh` runs 1-4.
5. `check_public_apk.sh <apk>`, `phone_gamebuild_test.sh` (above).
6. `phone_test.sh [one generated .c]` - pushes it all to `/data/local/tmp/lasondev` and runs: `selftest.py` +
   `gen.py --help` + `pybench.py`; `phone_cc_test.sh` (headers, gamelib_entry.c, optionally one chunk, timed);
   `linkprobe.c` compiled, linked against libmain.so and loaded. Removes the folder at the end (trap).

Output: `build/ondevice/stage/` (`jniLibs/arm64-v8a/*.so`, `pyhome/`, `cckit/`, license files).

## Remaining work

1. **The on-phone end-to-end test** with the installed APK and real game files (not done here: no APK installs on the
   shared phone yet): the copy, the real generator's time and memory on the phone, the whole build's time and
   thermals with 4 jobs at nice 10, the notification with the screen off, resume after swiping the app away, and the
   game running from the phone-built library (clang 22 code vs the NDK's clang 18).
2. The library from `build_game.sh` with the split (computer) to be checked in the game too (same code id as the
   phone's).
3. Size: our own NDK-built clang+lld instead of zig (≈ 1/3 the size); trim the stdlib further (email, http, xml,
   asyncio, multiprocessing... are unused); ship sourceless .pyc.
4. If the split files stay the slowest: `-O1` for them too (23 s instead of 34 s for the worst on the M1), measured
   against the game's speed in those functions.
