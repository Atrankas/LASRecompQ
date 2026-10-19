#!/system/bin/sh
# tools/android/ondevice/phone_cc_test.sh <dir> [source.c]: run ON the phone (adb shell, /data/local/tmp/<dir>) with
# <dir>/libzig.so (zig, as it would sit in the app's native library folder: nothing else of zig) and <dir>/cckit
# (build_cckit.sh). Compiles the game code headers (cpu.h + simd.h), tools/gamelib_entry.c and, if given, one generated
# file with the game code's flags (tools/android/gamelib_id.sh --flags), timing it.
cd "$1" || exit 1
K=cckit
I="-nostdinc -isystem $K/clang/include -isystem $K/sysroot/usr/include -isystem $K/sysroot/usr/include/aarch64-linux-android -I$K/gamelib-src"
F="${OPT:--O2} -fno-strict-aliasing -fsigned-char -Wno-return-type -Wno-unused-label -fvisibility=hidden -fPIC"
CC="./libzig.so clang -target aarch64-linux-android30"
mkdir -p work
printf '#include "cpu.h"\n#include "simd.h"\n' > work/probe.c
$CC $F $I -c work/probe.c -o work/probe.o && echo "headers: ok"
$CC $F $I '-DGAMELIB_ID="x"' -c $K/gamelib-src/gamelib_entry.c -o work/entry.o && echo "gamelib_entry.c: ok"
if [ -n "$2" ]; then
    time $CC $F $I -c "$2" -o work/one.o && ls -l work/one.o
fi
