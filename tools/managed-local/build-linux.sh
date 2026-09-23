#!/usr/bin/env bash
set -euo pipefail
# Private compile/link recipe. It never runs a test binary, sets capabilities, writes a systemd
# unit or starts a daemon. With OMODACHI_INSTALL=1 it additionally copies the linked binary and
# its asset tree into $install_dir — a plain file copy under $HOME, nothing under /usr or /usr/local.
source_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
build_root=${OMODACHI_BUILD_ROOT:-"$HOME/.cache/omodachi-sunshine-build"}
tools_dir=${OMODACHI_BUILD_TOOLS:-"$HOME/.cache/omodachi-build-tools/venv/bin"}
ffmpeg_dir=${FFMPEG_PREPARED_BINARIES:-"$build_root/deps/ffmpeg-v2026.910.121303/ffmpeg"}
boost_dir=${BOOST_SOURCE_DIR:-"$build_root/deps/boost-1.89.0/boost-1.89.0"}
json_dir=${JSON_SOURCE_DIR:-"$build_root/deps/nlohmann-json-v3.11.3/json"}
# Upstream compiles the asset root into the binary as SUNSHINE_ASSETS_DIR and offers no runtime
# override (src/platform/linux/graphics.cpp derives SUNSHINE_SHADERS_DIR from it at compile time).
# Left at the upstream default it resolves to ${CMAKE_INSTALL_PREFIX}/assets = /usr/local/assets,
# which this recipe never writes: the OpenGL colour-conversion shaders then read back empty, the
# VAAPI encoder fails to build its pipeline and Sunshine falls back to libx264 software encoding.
# So the install directory has to be known at configure time; keep it next to the binary.
install_sha=$(git -C "$source_dir" rev-parse --short HEAD 2>/dev/null || printf 'local')
install_dir=${OMODACHI_INSTALL_DIR:-"$HOME/.local/share/omodachi/sunshine/$install_sha"}
export PATH="$tools_dir:$PATH"
for tool in cmake ninja c++ pkg-config python3; do command -v "$tool" >/dev/null; done
for lib in libcurl miniupnpc libcap libdrm wayland-client egl gl libevdev libva x11 numa gio-2.0 gio-unix-2.0 opus libpulse; do
  pkg-config --exists "$lib" || { printf 'Missing build dependency: %s\n' "$lib" >&2; exit 1; }
done
for file in libavcodec.a libavutil.a libswscale.a libcbs.a; do test -f "$ffmpeg_dir/lib/$file"; done
test -f "$boost_dir/CMakeLists.txt"
python_bin="$tools_dir/python"
"$python_bin" -c 'import jinja2; import pkg_resources' || {
  printf 'Install jinja2 and setuptools<81 in the private build-tools venv, then rerun.\n' >&2
  exit 1
}
build_dir="$build_root/build-managed-v1"
cmake -S "$source_dir" -B "$build_dir" -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_DOCS=OFF -DBUILD_TESTS=OFF \
  -DCMAKE_INSTALL_PREFIX="$install_dir" -DSUNSHINE_ASSETS_DIR="$install_dir/assets" \
  -DSUNSHINE_ENABLE_TRAY=OFF -DSUNSHINE_ENABLE_CUDA=OFF -DCUDA_FAIL_ON_MISSING=OFF \
  -DSUNSHINE_ENABLE_DRM=OFF -DSUNSHINE_ENABLE_VULKAN=OFF -DSUNSHINE_ENABLE_X11=OFF \
  -DSUNSHINE_ENABLE_KWIN=OFF -DSUNSHINE_ENABLE_PORTAL=OFF \
  -DSUNSHINE_ENABLE_WAYLAND=ON -DSUNSHINE_ENABLE_VAAPI=ON \
  -DFFMPEG_PREPARED_BINARIES="$ffmpeg_dir" \
  -DFETCHCONTENT_SOURCE_DIR_BOOST="$boost_dir" \
  -DFETCHCONTENT_SOURCE_DIR_JSON="$json_dir" \
  -DGLAD_SKIP_PIP_INSTALL=ON -DPython_EXECUTABLE="$python_bin" \
  -DSUNSHINE_PUBLISHER_NAME='Omodachi' \
  -DSUNSHINE_PUBLISHER_ISSUE_URL='https://github.com/omodachi/omodachi-sunshine/issues'
cmake --build "$build_dir" --target sunshine --parallel "${OMODACHI_BUILD_JOBS:-4}"
printf 'Compiled and linked private Sunshine under %s; assets root compiled in as %s/assets.\n' \
  "$build_dir" "$install_dir"
if [ "${OMODACHI_INSTALL:-0}" = "1" ]; then
  # cmake staged src_assets/{common,linux}/assets into "$build_dir/assets" at configure time, with
  # shaders/ as a symlink back into the source tree; -L copies the real shader files. `cmake
  # --install` is deliberately not used: it would also try to write udev rules, a systemd unit and
  # desktop/metainfo files outside this prefix.
  test -d "$build_dir/assets/shaders/opengl"
  mkdir -p "$install_dir"
  cp -f "$build_dir/sunshine" "$install_dir/sunshine"
  rm -rf "$install_dir/assets"
  mkdir -p "$install_dir/assets"
  cp -RL "$build_dir/assets/." "$install_dir/assets/"
  printf 'Installed binary and assets under %s; no capabilities set, no unit written, nothing started.\n' \
    "$install_dir"
fi
