#!/bin/bash
# tools/android/build_game.sh <game folder> [--apk] [--push]: the game code library for the Android app, from your own
# game files (v1.0.0; the folder holding exefs/, or exefs/ itself). On this computer: the generator (tools/gen.py,
# with the Android address shift) turns exefs/main and exefs/sdk into C, the NDK compiles that into liblasgame.so
# (build/android/gamelib/arm64-v8a/), linked against the app's own libmain.so (build the app first:
# cd android && ./gradlew assembleDebug).
#   --apk   also build a personal APK with it inside (android/app/build/outputs/apk/debug/app-debug.apk)
#   --push  copy it into the app on the phone connected over adb (debug builds: run-as)
# The result holds code made from the game: keep it to yourself, never share it.
set -euo pipefail
cd "$(dirname "$0")/../.."
ROOT=$PWD
GAME=${1:?usage: tools/android/build_game.sh <game folder> [--apk] [--push]}
shift
APK=0 PUSH=0
for a in "$@"; do case $a in --apk) APK=1 ;; --push) PUSH=1 ;; *) echo "unknown option $a"; exit 1 ;; esac; done

EXEFS=$GAME
[ -f "$EXEFS/main" ] || EXEFS=$GAME/exefs
[ -f "$EXEFS/main" ] && [ -f "$EXEFS/sdk" ] || { echo "no exefs/main and exefs/sdk in $GAME"; exit 1; }
EXEFS=$(cd "$EXEFS" && pwd)  # (the generator runs in tools/)
# v1.0.0 only (the build id at 0x40, as the start screen checks: runtime/ui/launcher.mm BUILD_ID)
WANT=$(sed -n 's/.*BUILD_ID\[8\] = {\([^}]*\)}.*/\1/p' runtime/ui/launcher.mm | tr -d ' ,' | sed 's/0x//g' | tr 'A-F' 'a-f')
HAVE=$(dd if="$EXEFS/main" bs=1 skip=64 count=8 2>/dev/null | xxd -p)
[ -z "$WANT" ] || [ "$WANT" = "$HAVE" ] || { echo "exefs/main is not version 1.0.0 (build id $HAVE)"; exit 1; }

SDK=${ANDROID_HOME:-$(sed -n 's/^sdk.dir=//p' android/local.properties 2>/dev/null || true)}
SDK=${SDK:-/opt/homebrew/share/android-commandlinetools}
NDK=${ANDROID_NDK_HOME:-$SDK/ndk/27.2.12479018}
case "$(uname -s)" in Darwin) HOST=darwin-x86_64; JOBS=$(sysctl -n hw.ncpu) ;; *) HOST=linux-x86_64; JOBS=$(nproc) ;; esac
CC="$NDK/toolchains/llvm/prebuilt/$HOST/bin/aarch64-linux-android30-clang"
[ -x "$CC" ] || { echo "Android NDK 27.2 not found ($NDK)"; exit 1; }
MAINLIB=$(ls -t android/app/build/intermediates/cxx/*/*/obj/arm64-v8a/libmain.so 2>/dev/null | head -1 || true)
[ -n "$MAINLIB" ] || { echo "build the app first: cd android && ./gradlew assembleDebug"; exit 1; }

# the generator's Python (capstone, unicorn, lz4)
VENV=build/android/venv
if [ ! -x "$VENV/bin/python" ]; then
    "$(command -v python3.12 || command -v python3.11 || command -v python3)" -m venv "$VENV"
    "$VENV/bin/pip" install -q capstone==5.0.9 unicorn==2.1.4 lz4==4.4.5
fi

ID=$(tools/android/gamelib_id.sh)
FLAGS=$(tools/android/gamelib_id.sh --flags)
GEN=build/android/gen
OBJ=build/android/gen-obj
OUT=build/android/gamelib/arm64-v8a
mkdir -p "$GEN" "$OBJ" "$OUT"

echo "== generating C (game code id $ID)"
# (unchanged chunks keep their files: a rebuild after a change compiles only what it touched)
(cd tools && "$ROOT/$VENV/bin/python" gen.py sdk "$EXEFS/sdk" 0x7004000000 "$ROOT/$GEN/sdk" --hle ../runtime/hle/hle_symbols.txt \
    --simd asm --shift 0x6000000000)
(cd tools && "$ROOT/$VENV/bin/python" gen.py main "$EXEFS/main" 0x7000000000 "$ROOT/$GEN/main" --simd asm --shift 0x6000000000)

echo "== compiling ($JOBS jobs)"
compile() {  # <source> <object>
    [ "$2" -nt "$1" ] && [ "$2" -nt runtime/cpu.h ] && [ "$2" -nt tools/android/gamelib_id.sh ] && return 0
    "$CC" $FLAGS -c "$1" -o "$2.tmp" && mv "$2.tmp" "$2"
}
export -f compile
export CC FLAGS
ls "$GEN"/sdk/*.c "$GEN"/main/*.c | while read -r c; do echo "$c $OBJ/$(basename "$(dirname "$c")")_$(basename "$c" .c).o"; done |
    xargs -P "$JOBS" -n 2 bash -c 'compile "$0" "$1" || exit 255'
"$CC" $FLAGS -DGAMELIB_ID="\"$ID\"" -c tools/gamelib_entry.c -o "$OBJ/gamelib_entry.o"

echo "== linking"
# DT_NEEDED libmain.so: the runtime functions the game code calls are the app's own (it is loaded first)
"$CC" -shared -fuse-ld=lld -Wl,-z,max-page-size=16384 -Wl,-Bsymbolic -Wl,-soname,liblasgame.so \
    -o "$OUT/liblasgame.so.tmp" "$OBJ"/*.o -L"$(dirname "$MAINLIB")" -lmain
mv "$OUT/liblasgame.so.tmp" "$OUT/liblasgame.so"
echo "$ID" > "$OUT/liblasgame.so.id"
ls -la "$OUT/liblasgame.so"

if [ $APK = 1 ]; then
    echo "== personal APK"
    (cd android && ./gradlew assembleDebug -q -PlasGameLib="$ROOT/build/android/gamelib")
    ls -la android/app/build/outputs/apk/debug/app-debug.apk
fi
if [ $PUSH = 1 ]; then
    echo "== into the app on the phone"
    PKG=com.lasrecomp.android
    adb push "$OUT/liblasgame.so" "$OUT/liblasgame.so.id" /data/local/tmp/
    adb shell "run-as $PKG cp /data/local/tmp/liblasgame.so /data/local/tmp/liblasgame.so.id files/ &&
               rm /data/local/tmp/liblasgame.so /data/local/tmp/liblasgame.so.id"
    adb shell "run-as $PKG ls -la files/liblasgame.so files/liblasgame.so.id"
fi
