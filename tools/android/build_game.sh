#!/bin/bash
# tools/android/build_game.sh <game folder> [--apk] [--push]: the game code library for the Android app, from your own
# game files (v1.0.0; the folder holding exefs/, or exefs/ itself). On this computer: the generator (tools/gen.py,
# with the Android address shift) turns exefs/main and exefs/sdk into C, the NDK compiles that into liblasgame.so
# (build/android/gamelib/arm64-v8a/), linked against the app's own libmain.so (build the app first:
# cd android && ./gradlew assembleDebug).
#   --apk   also build a personal APK with it inside (android/app/build/outputs/apk/debug/app-debug.apk)
#   --push  copy it into the app on the phone connected over adb (debug builds: run-as)
#   --pgo-gen        an instrumented build (profile-guided optimisation, step 1): play with LARECOMP_PGO_DUMP_EVERY=600
#                    in env.txt, then pull build/pgo/lasgame.profraw from the app's data folder
#   --pgo-use=<file> the optimised build from that profile (llvm-profdata merge -o <file> lasgame.profraw)
# The result holds code made from the game: keep it to yourself, never share it.
set -euo pipefail
cd "$(dirname "$0")/../.."
ROOT=$PWD
GAME=${1:?usage: tools/android/build_game.sh <game folder> [--apk] [--push]}
shift
APK=0 PUSH=0 PGO=
for a in "$@"; do case $a in --apk) APK=1 ;; --push) PUSH=1 ;; --pgo-gen) PGO=gen ;; --pgo-use=*) PGO=${a#--pgo-use=} ;;
                              *) echo "unknown option $a"; exit 1 ;; esac; done

EXEFS=$GAME
[ -f "$EXEFS/main" ] || EXEFS=$GAME/exefs
[ -f "$EXEFS/main" ] && [ -f "$EXEFS/sdk" ] || { echo "no exefs/main and exefs/sdk in $GAME"; exit 1; }
EXEFS=$(cd "$EXEFS" && pwd)  # (the generator runs in tools/)
# (the game version: tools/gen.py picks its address table by the main program's build ID, at 0x40, and stops on an
# unknown one; the library's id ends in its first 8 bytes, as runtime/gamelib.h gamelib_full_id makes it)
BUILD8=$(dd if="$EXEFS/main" bs=1 skip=64 count=8 2>/dev/null | xxd -p | tr 'a-f' 'A-F')

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

ID=$(tools/android/gamelib_id.sh)-$BUILD8
FLAGS=$(tools/android/gamelib_id.sh --flags)
SPLITFLAGS=$(tools/android/gamelib_id.sh --split-flags)
GENARGS=$(tools/android/gamelib_id.sh --gen-args)
# (compile-time-only options of newer clangs, the same code: the phone's zig clang 22 has them, the NDK's clang 18 not)
CC22=$(tools/android/gamelib_id.sh --cc22-flags)
# (clang 18 prints "Unknown command line argument" for them - and may still exit 0 - so: no output at all)
PROBE=$(mktemp -t lasprobe.XXXXXX)
[ -z "$(echo 'int x;' | "$CC" $CC22 -x c -c - -o "$PROBE" 2>&1)" ] && FLAGS="$FLAGS $CC22"
rm -f "$PROBE"
GEN=build/android/gen
OBJ=build/android/gen-obj
LINKPGO=
if [ "$PGO" = gen ]; then
    FLAGS="$FLAGS -fprofile-generate -DLASGAME_PGO" LINKPGO=-fprofile-generate OBJ=build/android/gen-obj-pgo-gen
elif [ -n "$PGO" ]; then
    PGO=$(cd "$(dirname "$PGO")" && pwd)/$(basename "$PGO")
    FLAGS="$FLAGS -fprofile-use=$PGO -Wno-profile-instr-out-of-date -Wno-profile-instr-unprofiled -Wno-backend-plugin"
    OBJ=build/android/gen-obj-pgo-use
fi
OUT=build/android/gamelib/arm64-v8a
mkdir -p "$GEN" "$OBJ" "$OUT"

echo "== generating C (game code id $ID)"
# (unchanged chunks keep their files: a rebuild after a change compiles only what it touched)
(cd tools && "$ROOT/$VENV/bin/python" gen.py sdk "$EXEFS/sdk" 0x7004000000 "$ROOT/$GEN/sdk" --hle ../runtime/hle/hle_symbols.txt \
    $GENARGS)
(cd tools && "$ROOT/$VENV/bin/python" gen.py main "$EXEFS/main" 0x7000000000 "$ROOT/$GEN/main" $GENARGS)

echo "== compiling ($JOBS jobs)"
compile() {  # <source> <object>
    [ "$2" -nt "$1" ] && [ "$2" -nt runtime/cpu.h ] && [ "$2" -nt tools/android/gamelib_id.sh ] && { [ -z "${PGOFILE:-}" ] || [ "$2" -nt "$PGOFILE" ]; } && return 0
    local x= f=$FLAGS  # (the generator's split-dispatch files: their first line says so, tools/gen.py SPLIT_MARK;
    head -c 40 "$1" | grep -q 'lasgen: split-dispatch' && x=$SPLITFLAGS  # cold functions' files (COLD_MARK): -O0)
    head -c 40 "$1" | grep -q 'lasgen: cold' && f=${FLAGS/-O2/-O0}
    "$CC" $f $x -c "$1" -o "$2.tmp" && mv "$2.tmp" "$2"
}
export -f compile
PGOFILE=$([ -n "$PGO" ] && [ "$PGO" != gen ] && echo "$PGO" || true)
export CC FLAGS SPLITFLAGS PGOFILE
# the slowest first (no long file left alone at the end): the split-dispatch files (~8x the time per byte), then by
# size, the cold functions' files (-O0) last
# (grep -L's exit status differs between versions: || true)
{ { grep -l 'lasgen: split-dispatch' "$GEN"/sdk/*.c "$GEN"/main/*.c || true; } | xargs ls -S
  { grep -L 'lasgen: cold' "$GEN"/sdk/*.c "$GEN"/main/*.c || true; } | xargs ls -S
  ls -S "$GEN"/sdk/*.c "$GEN"/main/*.c; } | awk '!seen[$0]++' |
    while read -r c; do echo "$c $OBJ/$(basename "$(dirname "$c")")_$(basename "$c" .c).o"; done |
    xargs -P "$JOBS" -n 2 bash -c 'compile "$0" "$1" || exit 255'
"$CC" $FLAGS -DGAMELIB_ID="\"$ID\"" -c tools/gamelib_entry.c -o "$OBJ/gamelib_entry.o"

# (objects of chunks the generator no longer writes - another split into files - would define functions twice)
for o in "$OBJ"/main_main_*.o "$OBJ"/sdk_sdk_*.o; do
    b=$(basename "$o" .o); m=${b%%_*}
    [ -e "$o" ] && [ ! -e "$GEN/$m/${b#*_}.c" ] && rm -f "$o"
done
echo "== linking"
# DT_NEEDED libmain.so: the runtime functions the game code calls are the app's own (it is loaded first)
"$CC" -shared -fuse-ld=lld $LINKPGO -Wl,-z,max-page-size=16384 -Wl,-Bsymbolic -Wl,-soname,liblasgame.so \
    -o "$OUT/liblasgame.so.tmp" "$OBJ"/*.o -L"$(dirname "$MAINLIB")" -lmain
mv "$OUT/liblasgame.so.tmp" "$OUT/liblasgame.so"
echo "$ID" > "$OUT/liblasgame.so.id"
ls -la "$OUT/liblasgame.so"

if [ $APK = 1 ]; then
    echo "== personal APK"
    # (your own copy of the Brazilian Portuguese fan translation, when present: inside this personal APK only)
    TR=; [ -d translations/pt-BR/romfs ] && TR="-PlasTranslation=$ROOT/translations/pt-BR"
    (cd android && ./gradlew assembleDebug -q -PlasGameLib="$ROOT/build/android/gamelib" $TR)
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
