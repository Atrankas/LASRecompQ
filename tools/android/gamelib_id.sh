#!/bin/bash
# tools/android/gamelib_id.sh [--flags]: the Android game code library's id (runtime/gamelib.h GAMELIB_ID), the way
# the Makefile makes it: a hash of the generator, the headers the code is compiled against and the flags. The app
# (android/app/CMakeLists.txt) and the library (tools/android/build_game.sh) both take it from here, so they agree.
# --flags prints the flags instead (what build_game.sh compiles the generated code with), --split-flags the ones
# added for the generator's split-dispatch files, --gen-args the generator options (both ways of building the library,
# on a computer and on the phone, use these: the same code, the same id).
set -euo pipefail
cd "$(dirname "$0")/../.."
# -O2 (the desktop builds use -O1 for build time; phones get the faster code), -fsigned-char: plain char signed as on
# the systems the code is tested on (unsigned in the arm64 Linux / Android ABI)
GENFLAGS="-O2 -fno-strict-aliasing -fsigned-char -Wno-return-type -Wno-unused-label -fvisibility=hidden -fPIC -Iruntime"
# the generator's huge jump-table functions in files of their own, compiled without LICM promotion (tools/gen.py
# --split-dispatch: minutes per file otherwise, an hour+ on a phone)
SPLITFLAGS="-mllvm -disable-licm-promotion"
GENARGS="--simd asm --shift 0x6000000000 --split-dispatch 512"
case "${1:-}" in
    --flags) echo "$GENFLAGS"; exit 0 ;;
    --split-flags) echo "$SPLITFLAGS"; exit 0 ;;
    --gen-args) echo "$GENARGS"; exit 0 ;;
esac
SRCS="tools/game_addresses.py runtime/game_addresses/patches_v1_0_0.h tools/gen.py tools/lifter.py tools/analyze.py tools/nso.py tools/passes.py tools/patches.py tools/simd_c.py
      tools/rando/patch_variants.json tools/gamelib_entry.c runtime/hle/hle_symbols.txt runtime/cpu.h runtime/simd.h
      runtime/simd_fp.h runtime/gamelib.h"
# (+ the shift: the same sources make another library on Android, runtime/cpu.h GUEST_SHIFT)
(cat $SRCS; echo "$GENFLAGS android shift=0x6000000000"; echo "split $SPLITFLAGS gen $GENARGS") | (shasum -a 256 2>/dev/null || sha256sum) | cut -c1-16
