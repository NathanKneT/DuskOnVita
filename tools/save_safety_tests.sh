#!/usr/bin/env bash
# Save safety host tests (docs/ARCHITECTURE.md), run by tools/host_tests.sh.
#
#   tools/save_safety_tests.sh [output-dir]
#
# 1. platform/vita/aurora/save_safety.cpp on real folders (tests/save_safety_test.cpp), with the
#    frame limit the Vita build puts on it (its backup hook runs on a 32 KiB thread stack).
# 2. The real card: Aurora's lib/card, lib/io.cpp and lib/dolphin/card.cpp with patches/aurora/0005
#    and 0011 applied, on a real folder through SDL calls backed by stdio (tests/fake_sdl_fs), driven
#    through the CARD API the game uses (tests/card_save_safety_test.cpp). Built twice, without the
#    port's hooks (the card as before 0011) and with them; what the game reads back must match.
set -euo pipefail
repo=$(cd "$(dirname "$0")/.." && pwd)
work=${TWV_WORK:-$HOME/twilight-vita-work}
out=${1:-}
if [[ -z "$out" ]]; then out=$(mktemp -d); trap 'rm -rf "$out"' EXIT; fi
mkdir -p "$out"
aurora="$repo/external/dusklight/extern/aurora"
sanitize=(-fsanitize=address,undefined -fno-omit-frame-pointer)

${CXX:-g++} -std=c++20 -O1 -g -Wall -Wextra -Werror "${sanitize[@]}" -I "$repo/platform/vita" \
  "$repo/tests/save_safety_test.cpp" "$repo/platform/vita/aurora/save_safety.cpp" -o "$out/save_safety_test"
"$out/save_safety_test" "$out/save-safety-work"
# Same frame limit as cmake/PlatformLibs.cmake (64-bit host frames are the larger ones).
${CXX:-g++} -std=c++20 -O2 -Werror=frame-larger-than=1024 -I "$repo/platform/vita" \
  -c "$repo/platform/vita/aurora/save_safety.cpp" -o "$out/save_safety_frame.o"
echo "save_safety frame check: every function under 1 KiB"

card="$out/aurora-card"
rm -rf "$card"; mkdir -p "$card/lib/card" "$card/lib/dolphin"
cp "$aurora/lib/io.cpp" "$aurora/lib/io.hpp" "$aurora/lib/internal.hpp" "$aurora/lib/logging.hpp" "$card/lib/"
cp "$aurora/lib/card/"*.cpp "$aurora/lib/card/"*.hpp "$aurora/lib/card/"*.h "$card/lib/card/"
cp "$aurora/lib/dolphin/card.cpp" "$card/lib/dolphin/"
patch -s -p1 --no-backup-if-mismatch -d "$card" < "$repo/patches/aurora/0005-vita-copy-stream-stack.patch"
patch -s -p1 --no-backup-if-mismatch -d "$card" < "$repo/patches/aurora/0011-vita-save-safety-hooks.patch"
common=(-std=c++20 -O1 -g "${sanitize[@]}" -DTARGET_PC=1 -DAURORA -DFMT_HEADER_ONLY
        -I "$repo/tests/fake_sdl_fs" -I "$aurora/include" -I "$work/deps/fmt/include" -I "$repo/platform/vita")
mkdir -p "$out/card-objects"
objects=()
for source in "$card"/lib/io.cpp "$card"/lib/card/*.cpp "$card"/lib/dolphin/card.cpp "$repo/tests/fake_sdl_fs/fake_sdl_fs.cpp"; do
  object="$out/card-objects/$(basename "$(dirname "$source")")_$(basename "$source" .cpp).o"
  # Aurora's own sources keep their own warnings; the port and test code below use -Werror.
  ${CXX:-g++} "${common[@]}" -w -c "$source" -o "$object"
  objects+=("$object")
done
for hooks in 0 1; do
  extra=()
  if ((hooks)); then
    ${CXX:-g++} "${common[@]}" -Wall -Wextra -Werror -c "$repo/platform/vita/aurora/save_safety.cpp" \
      -o "$out/card-objects/save_safety.o"
    extra=("$out/card-objects/save_safety.o")
  fi
  ${CXX:-g++} "${common[@]}" -Wall -Wextra -Werror -DTWV_TEST_HOOKS=$hooks \
    "$repo/tests/card_save_safety_test.cpp" "${objects[@]}" "${extra[@]}" -o "$out/card_save_safety_$hooks"
  for mode in saves import import-off drop unfinished recover-zero recover-header keep-zero keep-header; do
    "$out/card_save_safety_$hooks" "$mode" "$out/card-work-$hooks-$mode" "$out/transcript-$hooks.bin" > "$out/card-$hooks-$mode.log" \
      || { cat "$out/card-$hooks-$mode.log"; exit 1; }
    tail -n 1 "$out/card-$hooks-$mode.log"
  done
done
cmp "$out/transcript-0.bin" "$out/transcript-1.bin"
echo "card_save_safety: the game reads the same card with and without the hooks ($(stat -c %s "$out/transcript-1.bin") bytes)"
