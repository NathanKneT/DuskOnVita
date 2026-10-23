#!/usr/bin/env bash
# Cross-build twilight-vita in the digest-pinned VitaSDK image.
#
#   tools/vita_build.sh [build-dir] [extra cmake -D args...]
#
# Runs on Linux or WSL with Docker. The repository (usually on /mnt/c) is
# mirrored into a native work tree ($TWV_WORK, default ~/twilight-vita-work)
# because compiling ~2000 TUs through the WSL 9P mount is several times slower.
# patches/<module>/*.patch are applied to the mirror only; files whose patched
# content did not change keep their mtime so incremental builds stay incremental.
# Dependencies come from deps.lock (fetched here, on the host); the container
# runs with --network none. Logs and artifacts are copied back to <repo>/<build-dir>/.
set -euo pipefail
repo=$(cd "$(dirname "$0")/.." && pwd)
work=${TWV_WORK:-$HOME/twilight-vita-work}
image=vitasdk/vitasdk@sha256:8227da1b596d981f4513cd9b36bda1e43857f6dae5bcf4312c1cd40167f7d9bb
build=${1:-build-vita}; shift || true
jobs=${TWV_JOBS:-$(nproc)}

declare -A module_dir=(
  [dusklight]=external/dusklight
  [aurora]=external/dusklight/extern/aurora
  [borealis]=external/dusklight/extern/borealis
)

mkdir -p "$work/src" "$work/$build" "$repo/$build/logs"
exclude=$(mktemp); trap 'rm -f "$exclude"' EXIT
printf '%s\n' '.git' '/build*' '/external/_deps' > "$exclude"
patched=()
for p in "$repo"/patches/*/*.patch; do
  [ -e "$p" ] || continue
  mod=$(basename "$(dirname "$p")"); dir=${module_dir[$mod]:?unknown patch module $mod}
  while read -r f; do patched+=("$dir/$f"); echo "/$dir/$f" >> "$exclude"; done \
    < <(sed -n 's#^+++ b/\([^[:space:]]*\).*#\1#p' "$p")
done
rsync -a --delete --exclude-from="$exclude" "$repo/" "$work/src/"

stage="$work/patch-stage"; rm -rf "$stage"; mkdir -p "$stage"
for f in "${patched[@]}"; do
  mkdir -p "$stage/$(dirname "$f")"
  [ -f "$repo/$f" ] && cp -p "$repo/$f" "$stage/$f"
done
for p in "$repo"/patches/*/*.patch; do
  [ -e "$p" ] || continue
  mod=$(basename "$(dirname "$p")")
  patch -s -p1 --no-backup-if-mismatch -d "$stage/${module_dir[$mod]}" < "$p"
done
# -c without -t: identical content is left alone (mtime kept), changed files are rewritten.
[ -n "$(ls -A "$stage")" ] && rsync -rlc "$stage/" "$work/src/"

"$repo/tools/fetch_deps.sh" "$work/deps"

log="$repo/$build/logs/build.log"
set +e
docker run --rm --network none --user "$(id -u):$(id -g)" -v "$work:/w" -w /w "$image" \
  sh -c 'b=$1; j=$2; shift 2
         cmake -S /w/src -B "/w/$b" -G Ninja \
           -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake" \
           -DCMAKE_BUILD_TYPE=RelWithDebInfo -DTWV_DEPS_DIR=/w/deps "$@" &&
         cmake --build "/w/$b" -j "$j" -- -k 0' sh "$build" "$jobs" "$@" > "$log" 2>&1
code=$?
set -e
echo "$code" > "$repo/$build/logs/build.exit"
# Copy back whatever was produced (reports, ELF/VPK), never the object files.
for f in twilight_vita.vpk twilight_vita_audio_probe.vpk eboot.bin twilight_vita.elf link_report.txt compile_census.txt sdl3_abi_audit.txt libstdcxx_abi_audit.txt; do
  [ -f "$work/$build/$f" ] && cp -p "$work/$build/$f" "$repo/$build/"
done
tail -n 5 "$log"
echo "exit $code (log: $build/logs/build.log)"
exit "$code"
