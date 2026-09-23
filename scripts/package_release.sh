#!/usr/bin/env bash
set -euo pipefail
#
# Build this fork on an Omarchy host and pack the result into one relocatable
# archive an installer on another machine can unpack and run.
#
#   scripts/package_release.sh
#
# It produces, under $OMODACHI_PACKAGE_OUT (default
# "$OMODACHI_BUILD_ROOT/dist"):
#
#   omodachi-sunshine-<short sha>-x86_64.tar.zst
#   omodachi-sunshine-<short sha>-x86_64.tar.zst.sha256
#
# This script compiles and links. It never installs anything outside
# $OMODACHI_BUILD_ROOT, never sets capabilities, never writes a systemd unit
# and never starts a daemon. Writing the unit is the installer's job, because
# only the installer knows which directory the archive was unpacked into.
#
# --- why the archive is relocatable, and why that needed a build change ------
#
# Upstream compiles the asset root into the binary as SUNSHINE_ASSETS_DIR and
# offers no runtime override: src/platform/linux/graphics.cpp derives
# SUNSHINE_SHADERS_DIR from it at compile time. tools/managed-local/build-linux.sh
# sets it to an absolute per-commit directory under the *building user's* home
# ($HOME/.local/share/omodachi/sunshine/<sha>). A binary built that way only
# finds its shaders on a machine where that exact absolute path exists - i.e.
# only for a user with the same name as whoever built it. Everywhere else the
# OpenGL colour-conversion shaders read back empty, the VAAPI encoder fails to
# build its pipeline and the fork silently falls back to libx264 software
# encoding (FORK.md, "The assets directory", PERF-3).
#
# So a redistributable build cannot use an absolute assets path at all. cmake
# already has the seam: cmake/compile_definitions/common.cmake takes the
# compiled-in value from SUNSHINE_ASSETS_DIR_DEF when that is set, and only
# falls back to the absolute SUNSHINE_ASSETS_DIR when it is not. This build
# sets SUNSHINE_ASSETS_DIR_DEF=assets, so every asset lookup is resolved
# against the process working directory, and the unit the installer writes
# carries WorkingDirectory=<the directory the archive was unpacked into>.
# CMAKE_INSTALL_PREFIX / SUNSHINE_ASSETS_DIR are still set (cmake stages the
# asset tree using them) but nothing absolute reaches the binary.
#
# --- GPL-3.0 --------------------------------------------------------------
#
# Sunshine is GPL-3.0-only and this is a modified version of it. An archive
# built here is a conveyed binary, so it carries LICENSE and NOTICE verbatim
# and a SOURCE file naming this repository and the exact full commit it was
# built from. Do not publish an archive built from a tree with uncommitted
# changes: the sha in SOURCE would not describe the binary. The script refuses
# to, unless OMODACHI_PACKAGE_ALLOW_DIRTY=1 (which stamps the sha as
# "<sha>-dirty" and is for local testing only).

source_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
build_root=${OMODACHI_BUILD_ROOT:-"$HOME/.cache/omodachi-sunshine-build"}
tools_dir=${OMODACHI_BUILD_TOOLS:-"$HOME/.cache/omodachi-build-tools/venv/bin"}
ffmpeg_dir=${FFMPEG_PREPARED_BINARIES:-"$build_root/deps/ffmpeg-v2026.910.121303/ffmpeg"}
boost_dir=${BOOST_SOURCE_DIR:-"$build_root/deps/boost-1.89.0/boost-1.89.0"}
json_dir=${JSON_SOURCE_DIR:-"$build_root/deps/nlohmann-json-v3.11.3/json"}
out_dir=${OMODACHI_PACKAGE_OUT:-"$build_root/dist"}
build_dir=${OMODACHI_PACKAGE_BUILD:-"$build_root/build-release"}
stage_root=${OMODACHI_PACKAGE_STAGE:-"$build_root/stage"}
# Every public URL in this project hangs off one owner, so moving the project
# between accounts is a single edit rather than a grep across four repositories.
github_owner=${OMODACHI_GITHUB_OWNER:-omodachi}
source_url=${OMODACHI_SOURCE_URL:-"https://github.com/$github_owner/omodachi-sunshine"}

say() { printf '==> %s\n' "$*"; }

# --- 1. identify the exact tree ---------------------------------------------
full_sha=$(git -C "$source_dir" rev-parse HEAD 2>/dev/null || true)
if [ -z "$full_sha" ]; then
  printf 'not a git checkout: %s\n' "$source_dir" >&2
  exit 2
fi
short_sha=${full_sha:0:7}
if ! git -C "$source_dir" diff --quiet HEAD -- || [ -n "$(git -C "$source_dir" ls-files --others --exclude-standard)" ]; then
  if [ "${OMODACHI_PACKAGE_ALLOW_DIRTY:-0}" != "1" ]; then
    printf 'working tree is not clean; a published archive must name the commit it was built from.\n' >&2
    printf 'commit first, or set OMODACHI_PACKAGE_ALLOW_DIRTY=1 for a local test build.\n' >&2
    exit 2
  fi
  short_sha="$short_sha-dirty"
fi
name="omodachi-sunshine-$short_sha-x86_64"
stage="$stage_root/$name"
say "packaging $name from $full_sha"

# --- 2. the same dependency gate the private recipe uses --------------------
export PATH="$tools_dir:$PATH"
for tool in cmake ninja c++ pkg-config python3 tar zstd sha256sum ldd; do
  command -v "$tool" >/dev/null || { printf 'missing build tool: %s\n' "$tool" >&2; exit 2; }
done
for lib in libcurl miniupnpc libcap libdrm wayland-client egl gl libevdev libva x11 numa \
           gio-2.0 gio-unix-2.0 opus libpulse; do
  pkg-config --exists "$lib" || { printf 'Missing build dependency: %s\n' "$lib" >&2; exit 2; }
done
for file in libavcodec.a libavutil.a libswscale.a libcbs.a; do
  test -f "$ffmpeg_dir/lib/$file" || { printf 'Missing prepared FFmpeg: %s\n' "$file" >&2; exit 2; }
done
test -f "$boost_dir/CMakeLists.txt"
python_bin="$tools_dir/python"
"$python_bin" -c 'import jinja2; import pkg_resources' >/dev/null 2>&1 || {
  printf 'Install jinja2 and setuptools<81 in the private build-tools venv, then rerun.\n' >&2
  exit 2
}

# --- 3. configure and build -------------------------------------------------
rm -rf "$stage"
mkdir -p "$stage" "$out_dir"
say "configuring $build_dir"
cmake -S "$source_dir" -B "$build_dir" -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_DOCS=OFF -DBUILD_TESTS=OFF \
  -DCMAKE_INSTALL_PREFIX="$stage" -DSUNSHINE_ASSETS_DIR="$stage/assets" \
  -DSUNSHINE_ASSETS_DIR_DEF="assets" \
  -DSUNSHINE_ENABLE_TRAY=OFF -DSUNSHINE_ENABLE_CUDA=OFF -DCUDA_FAIL_ON_MISSING=OFF \
  -DSUNSHINE_ENABLE_DRM=OFF -DSUNSHINE_ENABLE_VULKAN=OFF -DSUNSHINE_ENABLE_X11=OFF \
  -DSUNSHINE_ENABLE_KWIN=OFF -DSUNSHINE_ENABLE_PORTAL=OFF \
  -DSUNSHINE_ENABLE_WAYLAND=ON -DSUNSHINE_ENABLE_VAAPI=ON \
  -DFFMPEG_PREPARED_BINARIES="$ffmpeg_dir" \
  -DFETCHCONTENT_SOURCE_DIR_BOOST="$boost_dir" \
  -DFETCHCONTENT_SOURCE_DIR_JSON="$json_dir" \
  -DGLAD_SKIP_PIP_INSTALL=ON -DPython_EXECUTABLE="$python_bin" \
  -DSUNSHINE_PUBLISHER_NAME='Omodachi' \
  -DSUNSHINE_PUBLISHER_ISSUE_URL="$source_url/issues" >/dev/null
say "building"
cmake --build "$build_dir" --target sunshine --parallel "${OMODACHI_BUILD_JOBS:-4}"

# The compiled-in assets path must be relative or the archive is not portable.
# Read it back out of the binary rather than trusting the flag went through.
if strings "$build_dir/sunshine" 2>/dev/null | grep -qx "$HOME/.local/share/omodachi/sunshine/.*"; then
  printf 'the binary still carries an absolute assets path; refusing to package it\n' >&2
  exit 3
fi

# --- 4. stage the payload ---------------------------------------------------
# cmake staged src_assets/{common,linux}/assets into "$build_dir/assets" at
# configure time, with shaders/ as a symlink back into the source tree; -L
# copies the real shader files. `cmake --install` is deliberately not used: it
# would also write udev rules, a systemd unit and desktop/metainfo files.
test -d "$build_dir/assets/shaders/opengl"
cp -f "$build_dir/sunshine" "$stage/sunshine"
chmod 0755 "$stage/sunshine"
mkdir -p "$stage/assets"
cp -RL "$build_dir/assets/." "$stage/assets/"
cp -f "$source_dir/LICENSE" "$stage/LICENSE"
cp -f "$source_dir/NOTICE" "$stage/NOTICE"

{
  printf 'name=%s\n' "$name"
  printf 'commit=%s\n' "$full_sha"
  printf 'repository=%s\n' "$source_url"
  printf 'upstream=https://github.com/LizardByte/Sunshine\n'
  printf 'licence=GPL-3.0-only\n'
  printf 'architecture=x86_64\n'
  printf 'built_on=%s\n' "$(uname -srm)"
  printf 'assets_dir=assets (relative to the working directory the unit sets)\n'
} >"$stage/SOURCE"

# --- 5. dependencies: what the host must already have, and what we carry ----
# Every shared object the linked binary needs is classified by asking pacman
# who owns it. Owned -> the package name goes in DEPENDS and the installer
# makes the host install it. Unowned -> it came from outside the distribution
# and travels in lib/ with the binary, listed in BUNDLED. The prepared FFmpeg
# is static (.a), so in practice nothing lands in lib/; the branch exists so a
# future non-distribution dependency cannot leave silently.
mkdir -p "$stage/lib"
: >"$stage/DEPENDS"
: >"$stage/BUNDLED"
while read -r lib_path; do
  [ -n "$lib_path" ] || continue
  if owner=$(pacman -Qoq "$lib_path" 2>/dev/null) && [ -n "$owner" ]; then
    printf '%s\n' "$owner" >>"$stage/DEPENDS"
  else
    cp -L "$lib_path" "$stage/lib/"
    printf '%s %s\n' "$(basename "$lib_path")" "$lib_path" >>"$stage/BUNDLED"
  fi
done < <(ldd "$stage/sunshine" | awk '{for (i = 1; i <= NF; i++) if ($i ~ /^\//) { print $i; break }}' | sort -u)
sort -u -o "$stage/DEPENDS" "$stage/DEPENDS"
rmdir "$stage/lib" 2>/dev/null || true
say "$(wc -l <"$stage/DEPENDS") system packages in DEPENDS, $(wc -l <"$stage/BUNDLED") bundled libraries"

# The fork also needs these at *runtime* rather than through the dynamic
# linker: the desktop entry it publishes is launched through them, and the
# managed capture path needs a VAAPI driver present. They cannot be discovered
# with ldd, so they are named here.
for extra in libva-utils; do
  pacman -Qq "$extra" >/dev/null 2>&1 && printf '%s\n' "$extra" >>"$stage/DEPENDS"
done
sort -u -o "$stage/DEPENDS" "$stage/DEPENDS"

# --- 6. manifest, archive, checksum -----------------------------------------
( cd "$stage" && find . -type f ! -name MANIFEST.sha256 -print0 \
    | sort -z | xargs -0 sha256sum >MANIFEST.sha256 )
archive="$out_dir/$name.tar.zst"
rm -f "$archive" "$archive.sha256"
tar --sort=name --owner=0 --group=0 --numeric-owner \
    --mtime="@$(git -C "$source_dir" show -s --format=%ct HEAD)" \
    -C "$stage_root" -I 'zstd -19 -T0' -cf "$archive" "$name"
( cd "$out_dir" && sha256sum "$name.tar.zst" >"$name.tar.zst.sha256" )
say "wrote $archive"
cat "$archive.sha256"
printf 'size=%s bytes\n' "$(stat -c %s "$archive")"
