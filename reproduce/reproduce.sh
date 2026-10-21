#!/bin/sh
# The one command: rebuild release sunshine-e9f1b08 and compare it with the
# published archive. Needs docker (or ENGINE=podman), curl and network access.
#
#   reproduce/reproduce.sh [--built-on 'Linux 6.19.11-zabbly+ x86_64']
#
# Writes to ./reproduce-out: the rebuilt archive, the published one,
# build.log and compare.log. Exits 0 only on `RESULT: MATCH`.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
engine=${ENGINE:-docker}
image=omodachi-sunshine-reproduce:e9f1b08
name=omodachi-sunshine-e9f1b08-x86_64.tar.zst
release=https://github.com/omodachi/omodachi-sunshine/releases/download/sunshine-e9f1b08
out=$PWD/reproduce-out
container=omodachi-sunshine-reproduce-$$
mkdir -p "$out"

"$engine" build -t "$image" -f "$here/Containerfile" "$here"
"$engine" run --name "$container" "$image" /reproduce/build.sh "$@" > "$out/build.log" 2>&1 \
  || { tail -n 40 "$out/build.log"; exit 1; }
tail -n 4 "$out/build.log"
"$engine" cp "$container:/build/out/dist/$name" "$out/rebuilt-$name"
"$engine" rm "$container" >/dev/null

curl -fsSL -o "$out/published-$name" "$release/$name"
"$engine" run --rm -v "$out:/in:ro" "$image" \
  /reproduce/compare.sh "/in/rebuilt-$name" "/in/published-$name" > "$out/compare.log" 2>&1 || true
cat "$out/compare.log"
tail -n 1 "$out/compare.log" | grep -q '^RESULT: MATCH '
