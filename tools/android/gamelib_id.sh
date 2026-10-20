#!/bin/bash
# tools/android/gamelib_id.sh [--flags]: the Android game code library's id (runtime/gamelib.h GAMELIB_ID), the way
# the Makefile makes it: a hash of the generator, the headers the code is compiled against and the flags. The app
# (android/app/CMakeLists.txt) and the library (tools/android/build_game.sh) both take it from here, so they agree.
# --flags prints the flags instead (what build_game.sh compiles the generated code with), --split-flags the ones
# added for the generator's split-dispatch files, --gen-args the generator options (both ways of building the library,
# on a computer and on the phone, use these: the same code, the same id), --cc22-flags compile-time-only options of
# newer clangs (the phone's zig clang 22; build_game.sh adds them when its compiler knows them).
set -euo pipefail
cd "$(dirname "$0")/../.."
# -O2 (the desktop builds use -O1 for build time; phones get the faster code), -fsigned-char: plain char signed as on
# the systems the code is tested on (unsigned in the arm64 Linux / Android ABI)
# -dag-maps-huge-region=500 (default 1000): the instruction schedulers' dependency graph of a huge block (the
# generator's long straight runs of stores, e.g. object constructors) is built with coarser memory chains from 500
# memory operations on - that was quadratic (main_012.c: 4.3 s -> 0.7 s, the same code); 787 of the 812 objects are
# byte-identical, the other 25 differ by an instruction order inside such blocks (32 bytes in all)
GENFLAGS="-O2 -fno-strict-aliasing -fsigned-char -Wno-return-type -Wno-unused-label -fvisibility=hidden -fPIC -Iruntime -mllvm -dag-maps-huge-region=500"
# the generator's huge jump-table functions in files of their own, compiled without LICM promotion (tools/gen.py
# --split-dispatch: minutes per file otherwise, an hour+ on a phone)
SPLITFLAGS="-mllvm -disable-licm-promotion"
GENARGS="--simd asm --shift 0x6000000000 --split-dispatch 512"
# Compile time only, the same code (checked: byte-identical objects for a 59-file sample and the whole library with
# zig clang 22; clang 18, the NDK's, has neither option): no LoopLoadElimination (it finds nothing to forward in this
# code, but builds loop-simplified form and memory dependences for the dispatch loops: 50-60% of a split-dispatch
# file's time) and a shorter search in the instruction selector's predecessor checks (the pre/post-indexed load/store
# combines walk the thousands of uses of the Cpu pointer in a block: 8192 steps by default; 2048 still finds every
# combine). -O2 is otherwise unchanged.
CC22FLAGS="-mllvm -opt-disable=loop-load-elim -mllvm -has-predecessor-max-steps=2048"
case "${1:-}" in
    --flags) echo "$GENFLAGS"; exit 0 ;;
    --split-flags) echo "$SPLITFLAGS"; exit 0 ;;
    --gen-args) echo "$GENARGS"; exit 0 ;;
    --cc22-flags) echo "$CC22FLAGS"; exit 0 ;;
esac
SRCS="tools/game_addresses.py runtime/game_addresses/patches_v1_0_0.h tools/gen.py tools/lifter.py tools/analyze.py tools/nso.py tools/passes.py tools/patches.py tools/simd_c.py
      tools/rando/patch_variants.json tools/gamelib_entry.c runtime/hle/hle_symbols.txt runtime/cpu.h runtime/simd.h
      runtime/simd_fp.h runtime/gamelib.h"
# (+ the shift: the same sources make another library on Android, runtime/cpu.h GUEST_SHIFT)
(cat $SRCS; echo "$GENFLAGS android shift=0x6000000000"; echo "split $SPLITFLAGS gen $GENARGS cc22 $CC22FLAGS") | (shasum -a 256 2>/dev/null || sha256sum) | cut -c1-16
