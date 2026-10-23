#!/usr/bin/env bash
# Download and verify the sources listed in deps.lock, then unpack each into
# <dest>/<name>/ (top-level archive folder stripped). Runs on the host (needs
# network); the container build itself stays --network none.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
dest=${1:?usage: fetch_deps.sh <dest-dir>}
mkdir -p "$dest/_archives"
while read -r name url sha _; do
  case "$name" in ''|'#'*) continue ;; esac
  archive="$dest/_archives/$name-$(basename "$url")"
  if [ ! -f "$archive" ] || ! echo "$sha  $archive" | sha256sum -c --quiet - 2>/dev/null; then
    curl -sSfL --retry 3 -o "$archive.part" "$url"
    mv "$archive.part" "$archive"
  fi
  echo "$sha  $archive" | sha256sum -c --quiet - || { echo "hash mismatch: $name" >&2; exit 1; }
  # The stamp covers the archive and our patches (patches/deps/<name>/*.patch),
  # so editing a patch re-unpacks and re-patches that dependency.
  patches=("$root"/patches/deps/"$name"/*.patch)
  [ -e "${patches[0]}" ] || patches=()
  want=$( { echo "$sha"; [ ${#patches[@]} -eq 0 ] || cat "${patches[@]}"; } | sha256sum | cut -d' ' -f1)
  stamp="$dest/$name/.stamp"
  if [ -f "$stamp" ] && [ "$(cat "$stamp")" = "$want" ]; then continue; fi
  rm -rf "$dest/$name" "$dest/_unpack"; mkdir -p "$dest/_unpack"
  case "$archive" in
    *.zip) unzip -q "$archive" -d "$dest/_unpack" ;;
    *) tar -xf "$archive" -C "$dest/_unpack" ;;
  esac
  # Strip a single top-level folder when the archive has one (miniz has none).
  set -- "$dest/_unpack"/*
  if [ $# -eq 1 ] && [ -d "$1" ]; then mv "$1" "$dest/$name"; else mv "$dest/_unpack" "$dest/$name"; fi
  rm -rf "$dest/_unpack"
  for p in "${patches[@]}"; do patch -s -p1 --no-backup-if-mismatch -d "$dest/$name" < "$p"; done
  echo "$want" > "$stamp"
  echo "fetched $name (${#patches[@]} patches)"
done < "$root/deps.lock"
