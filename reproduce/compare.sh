#!/bin/bash
# compare.sh <rebuilt.tar.zst> <published.tar.zst>
#
# Unpacks both archives and compares every entry: type, mode and bytes, one
# line per path. Shows what differs. Then packs the rebuilt tree again with the
# published SOURCE (and MANIFEST.sha256 recomputed by package_release.sh's own
# command), using package_release.sh's exact tar command, and compares that
# archive's SHA-256 with the published one.
# Exit 0 only if that SHA-256 is equal; the last line is then `RESULT: MATCH <sha256>`.
set -euo pipefail
rebuilt=$(realpath "$1"); published=$(realpath "$2")
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
umask 022

sha() { sha256sum "$1" | cut -c1-64; }
echo "rebuilt    $(sha "$rebuilt")  $(stat -c %s "$rebuilt") bytes"
echo "published  $(sha "$published")  $(stat -c %s "$published") bytes"

mkdir "$work/r" "$work/p"
tar -I zstd -xpf "$rebuilt" -C "$work/r"
tar -I zstd -xpf "$published" -C "$work/p"
# Tar metadata (names, modes, owners, mtimes, order), compared as listings.
for x in r p; do
  tar -I zstd --full-time -tvf "$([ $x = r ] && echo "$rebuilt" || echo "$published")" > "$work/$x.list"
done
if cmp -s "$work/r.list" "$work/p.list"; then
  echo "tar headers: same ($(wc -l < "$work/r.list") entries, modes, owners, mtimes, order)"
else
  echo "tar headers: DIFFER"; diff "$work/r.list" "$work/p.list" || true
fi

echo "--- per entry: same|DIFF|ONLY-<side>  <rebuilt sha256 or type/mode>  <path>"
( cd "$work/r" && find . -mindepth 1 ; cd "$work/p" && find . -mindepth 1 ) | sort -u > "$work/paths"
differ=()
while IFS= read -r path; do
  a="$work/r/$path" b="$work/p/$path"
  if [ ! -e "$a" ] && [ ! -L "$a" ]; then echo "ONLY-PUBLISHED  $path"; differ+=("$path"); continue; fi
  if [ ! -e "$b" ] && [ ! -L "$b" ]; then echo "ONLY-REBUILT    $path"; differ+=("$path"); continue; fi
  ma=$(stat -c '%F %a' "$a") mb=$(stat -c '%F %a' "$b")
  if [ "$ma" != "$mb" ]; then echo "DIFF mode $ma vs $mb  $path"; differ+=("$path"); continue; fi
  if [ -f "$a" ]; then
    if cmp -s "$a" "$b"; then echo "same  $(sha "$a")  $path"
    else echo "DIFF  $(sha "$a") vs $(sha "$b")  $path"; differ+=("$path"); fi
  else
    echo "same  ($ma)  $path"
  fi
done < "$work/paths"

echo "--- ${#differ[@]} entries differ"
for path in "${differ[@]}"; do
  [ -f "$work/r/$path" ] && [ -f "$work/p/$path" ] || continue
  echo "diff rebuilt published: $path"
  diff "$work/r/$path" "$work/p/$path" | sed 's/^/    /' || true
done

name=$(cd "$work/p" && ls)
if [ "${#differ[@]}" -eq 0 ]; then
  echo "--- the rebuilt archive's contents are identical to the published one's"
fi
# Repack: the rebuilt tree, with the published SOURCE, the manifest recomputed
# exactly as package_release.sh step 6 does, and package_release.sh's tar line.
# The mtime is the one every entry of the rebuilt archive carries (the commit time).
mtime=$(stat -c %Y "$work/r/$name")
cp -p "$work/p/$name/SOURCE" "$work/r/$name/SOURCE"
( cd "$work/r/$name" && find . -type f ! -name MANIFEST.sha256 -print0 \
    | sort -z | xargs -0 sha256sum > MANIFEST.sha256 )
if cmp -s "$work/r/$name/MANIFEST.sha256" "$work/p/$name/MANIFEST.sha256"; then
  echo "--- with the published SOURCE, the recomputed MANIFEST.sha256 equals the published one"
else
  echo "--- with the published SOURCE, MANIFEST.sha256 still differs:"
  diff "$work/r/$name/MANIFEST.sha256" "$work/p/$name/MANIFEST.sha256" | sed 's/^/    /' || true
fi
tar --sort=name --owner=0 --group=0 --numeric-owner \
    --mtime="@$mtime" \
    -C "$work/r" -I 'zstd -19 -T0' -cf "$work/repacked.tar.zst" "$name"
echo "repacked   $(sha "$work/repacked.tar.zst")  $(stat -c %s "$work/repacked.tar.zst") bytes  (mtime @$mtime = $(date -u -d "@$mtime" +%FT%TZ))"
if [ "$(sha "$work/repacked.tar.zst")" = "$(sha "$published")" ]; then
  echo "RESULT: MATCH $(sha "$published")"
else
  echo "RESULT: MISMATCH"; exit 1
fi
