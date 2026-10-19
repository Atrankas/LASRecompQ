#!/bin/bash
# tools/android/ondevice/phone_test.sh [generated .c]: the on-phone build pieces tried on the phone over adb, in
# /data/local/tmp/lasondev only (no app, no root), then removed (docs/android-ondevice-build.md):
#   1. Python: the generator's modules each used once (selftest.py), `gen.py --help`
#   2. zig + cckit: the game code headers, tools/gamelib_entry.c and (if given: your own, kept to yourself) one
#      generated file compiled with the game code's flags
#   3. link: linkprobe.c compiled and linked on the phone against the app's libmain.so (+ libSDL3.so), loaded by bionic
# Needs build_python.sh, build_zig.sh, build_cckit.sh (HOSTZIG=...) first and the app built (libmain.so).
set -euo pipefail
cd "$(dirname "$0")/../../.."
S=build/ondevice/stage D=/data/local/tmp/lasondev
MAINLIB=$(ls -t android/app/build/intermediates/cxx/*/*/obj/arm64-v8a/libmain.so 2>/dev/null | head -1 || true)
[ -n "$MAINLIB" ] || MAINLIB=$(ls -t "$(git rev-parse --git-common-dir)"/../android/app/build/intermediates/cxx/*/*/obj/arm64-v8a/libmain.so | head -1)
MAINDIR=$(dirname "$MAINLIB")
NDKCC=$(ls "${ANDROID_NDK_HOME:-$(sed -n 's/^sdk.dir=//p' "$(git rev-parse --git-common-dir)/../android/local.properties")/ndk/27.2.12479018}"/toolchains/llvm/prebuilt/*/bin/aarch64-linux-android30-clang)
mkdir -p build/ondevice/obj
"$NDKCC" -DLOADER -O2 -s tools/android/ondevice/linkprobe.c -o build/ondevice/obj/linkprobe_loader
trap 'adb shell rm -rf $D' EXIT
adb shell "rm -rf $D && mkdir -p $D/tools/rando $D/runtime/game_addresses $D/work"
adb push -q $S/jniLibs/arm64-v8a $D/lib
adb push -q $S/pyhome $D/pyhome
adb push -q $S/cckit $D/cckit
adb push -q tools/gen.py tools/lifter.py tools/analyze.py tools/nso.py tools/passes.py tools/patches.py tools/simd_c.py \
    tools/game_addresses.py $D/tools/
adb push -q tools/rando/patch_variants.json $D/tools/rando/
adb push -q runtime/game_addresses/patches_v1_0_0.h $D/runtime/game_addresses/
adb push -q tools/android/ondevice/selftest.py tools/android/ondevice/pybench.py tools/android/ondevice/phone_cc_test.sh $D/
adb push -q tools/android/ondevice/linkprobe.c build/ondevice/obj/linkprobe_loader "$MAINDIR/libmain.so" "$MAINDIR/libSDL3.so" $D/work/
[ -z "${1:-}" ] || adb push -q "$1" $D/work/one.c

echo "== 1. python"
PYENV="LASPY_HOME=$D/pyhome LASPY_PATH=$D/tools:$D/pyhome/lib/python3.14/site-packages LIBCAPSTONE_PATH=$D/lib LIBUNICORN_PATH=$D/lib"
adb shell "cd $D/tools && export $PYENV && ../lib/liblaspython.so ../selftest.py && ../lib/liblaspython.so gen.py --help | head -1 && ../lib/liblaspython.so ../pybench.py"
echo "== 2. compiler"
adb shell "cp $D/lib/libzig.so $D/libzig.so && sh $D/phone_cc_test.sh $D ${1:+work/one.c}"
echo "== 3. link + load"
adb shell "cd $D && K=cckit S=cckit/sysroot/usr/lib/aarch64-linux-android/30 &&
    ./libzig.so clang -target aarch64-linux-android30 -O2 -fPIC -fvisibility=hidden -nostdinc -isystem \$K/clang/include \
        -isystem \$K/sysroot/usr/include -isystem \$K/sysroot/usr/include/aarch64-linux-android -c work/linkprobe.c -o work/linkprobe.o &&
    ./libzig.so ld.lld @\$K/link.rsp -o work/liblinkprobe.so \$S/crtbegin_so.o work/linkprobe.o -Lwork -lmain -L\$S \
        \$K/clang/lib/libclang_rt.builtins-aarch64-android.a -ldl -lc \$S/crtend_so.o &&
    cd work && LD_LIBRARY_PATH=. ./linkprobe_loader ./liblinkprobe.so"
