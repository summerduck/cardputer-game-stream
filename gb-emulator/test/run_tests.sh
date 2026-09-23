#!/bin/zsh
# Builds the portable core with sanitizers and runs the tests; pass a ROM (and cache KB) to also run a game.
#   PAGE_BITS=12 ./run_tests.sh game.gbc 96   tries another page size
set -e
cd "$(dirname "$0")"
mkdir -p out
FLAGS=(-O2 -g -fsanitize=address,undefined -DMINIGB_APU_AUDIO_FORMAT_S16SYS -DEMU_PAGE_BITS=${PAGE_BITS:-11} -w -I../src/core)
clang -c $FLAGS ../src/core/minigb_apu.c -o out/minigb_apu.o
clang++ -std=c++17 $FLAGS host_test.cpp ../src/core/emu.cpp ../src/core/codec.cpp out/minigb_apu.o -o out/host_test
./out/host_test "$@"
