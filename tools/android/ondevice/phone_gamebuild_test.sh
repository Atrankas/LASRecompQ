#!/bin/bash
# tools/android/ondevice/phone_gamebuild_test.sh: runtime/gamebuild.c's Android branch, end to end ON the phone but
# without installing the app and without game files (adb, /data/local/tmp/lasondev only, removed at the end):
#   lib/       the app's libmain.so + libSDL3.so (android/app/build, any variant) and build_all.sh's programs
#   ondevice/  assets/ondevice.zip unpacked, with tools/gen.py replaced by fake_gen.py (synthetic chunks, one split)
#   game/      exefs/main and exefs/sdk: random bytes (fake_gen.py only checks they exist)
# gamebuild_test.c calls libmain.so's gamebuild_run (LARECOMP_ONDEVICE_DIR = ondevice/): Python runs the generator,
# zig compiles (the split chunk with the split flags) and links against libmain.so, the result is installed next to
# ondevice/ and loaded; then a build killed partway and run again (resumes), and a third run (all reused).
set -euo pipefail
cd "$(dirname "$0")/../../.."
D=/data/local/tmp/lasondev
MAIN=$(ls -t android/app/build/intermediates/cxx/*/*/obj/arm64-v8a/libmain.so | head -1)
SDK=${ANDROID_HOME:-$(sed -n 's/^sdk.dir=//p' android/local.properties)}
CC=$(ls "$SDK"/ndk/27.2.12479018/toolchains/llvm/prebuilt/*/bin/aarch64-linux-android30-clang)
W=build/ondevice/obj/gbtest
rm -rf $W
mkdir -p $W/ondevice $W/game/exefs
"$CC" -O2 tools/android/ondevice/gamebuild_test.c -o $W/gamebuild_test
unzip -q build/ondevice/assets/ondevice.zip -d $W/ondevice
cp tools/android/ondevice/fake_gen.py $W/ondevice/tools/gen.py
head -c 4096 /dev/urandom > $W/game/exefs/main
head -c 4096 /dev/urandom > $W/game/exefs/sdk
trap 'adb shell rm -rf $D' EXIT
adb shell "rm -rf $D && mkdir -p $D/lib"
adb push -q build/ondevice/stage/jniLibs/arm64-v8a/. $D/lib/
adb push -q "$MAIN" "$(dirname "$MAIN")/libSDL3.so" $D/lib/
adb push -q $W/ondevice $W/game $W/gamebuild_test $D/
RUN="cd $D && LARECOMP_ONDEVICE_DIR=$D/ondevice LD_LIBRARY_PATH=$D/lib exec ./gamebuild_test $D/lib/libmain.so $D/game $D/liblasgame.so"
adb shell "($RUN); echo exit \$?"
echo "== killed mid-build (generator running), then again: resumes"
# (only that process and its children, by pid: the phone is shared)
adb shell "rm -rf $D/game/.gamebuild $D/liblasgame.so*; sh -c '$RUN' > /dev/null 2>&1 & p=\$!; sleep 2; kill -9 -\$p 2>/dev/null || kill -9 \$p; sleep 1;
           ($RUN) | grep -E 'reused|done|failed|gamebuild_run|functions'"
echo "== once more: everything reused"
adb shell "($RUN) | grep -E 'reused|done|gamebuild_run'"
adb shell "ls -l $D/liblasgame.so $D/liblasgame.so.id; cat $D/liblasgame.so.id"
