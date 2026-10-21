#!/bin/bash
# Rebuild release sunshine-e9f1b08 inside the Containerfile's image.
#
#   build.sh [--built-on 'Linux 6.19.11-zabbly+ x86_64']
#
# Clones the public repository, checks out commit e9f1b08 and checks its commit
# and tree ids, downloads the three prebuilt or packaged dependencies and checks
# their SHA-256, then runs that commit's own scripts/package_release.sh with the
# flags the release was built with. The archive lands in /build/out/dist.
#
# The package's SOURCE file records `uname -srm` as built_on, the kernel of
# whatever machine runs the build; that line is the one thing the toolchain
# cannot pin. compare.sh accounts for it. --built-on instead makes `uname -srm`
# (and only that exact call) answer with the given string, so the archive comes
# out byte-identical by itself; the release was built on 'Linux 6.19.11-zabbly+ x86_64'.
set -euo pipefail

commit=e9f1b089255e235b5e6aa13ed89b7dd3f569de87
tree=f0a71093dbe05714722ef8bccb0803cd3af4a0eb
repo=https://github.com/omodachi/omodachi-sunshine

if [ "${1:-}" = --built-on ]; then
  mkdir -p /build/shim
  printf '%s\n' '#!/bin/sh' \
    'if [ "$*" = "-srm" ]; then printf "%s\n" "$BUILT_ON"; else exec /usr/bin/uname "$@"; fi' \
    > /build/shim/uname
  chmod 0755 /build/shim/uname
  export BUILT_ON=${2:?--built-on needs a value} PATH=/build/shim:$PATH
fi

umask 022
export LANG=C.UTF-8 TZ=UTC
start=$(date -u +%s)

echo "==> source: $repo at $commit"
rm -rf /build/src && git clone -q "$repo" /build/src
git -C /build/src -c advice.detachedHead=false checkout -q --detach "$commit"
test "$(git -C /build/src rev-parse HEAD)" = "$commit"
test "$(git -C /build/src rev-parse 'HEAD^{tree}')" = "$tree"
echo "    commit $(git -C /build/src rev-parse HEAD) tree $(git -C /build/src rev-parse 'HEAD^{tree}')"

echo "==> dependencies"
mkdir -p /build/deps/dl && cd /build/deps/dl
curl -fsSLO https://github.com/LizardByte/build-deps/releases/download/v2026.910.121303/Linux-x86_64-ffmpeg.tar.gz
curl -fsSLO https://github.com/boostorg/boost/releases/download/boost-1.89.0/boost-1.89.0-cmake.tar.xz
curl -fsSLO https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
sha256sum -c - <<'EOF'
496d2bbb674d01e6033e31b9dfc15cbc9dc1494e882a4505f6ab1e03f75b385c  Linux-x86_64-ffmpeg.tar.gz
67acec02d0d118b5de9eb441f5fb707b3a1cdd884be00ca24b9a73c995511f74  boost-1.89.0-cmake.tar.xz
d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d  json.tar.xz
EOF
mkdir -p /build/deps/ffmpeg-v2026.910.121303 /build/deps/boost-1.89.0 /build/deps/nlohmann-json-v3.11.3
tar -xzf Linux-x86_64-ffmpeg.tar.gz -C /build/deps/ffmpeg-v2026.910.121303
tar -xJf boost-1.89.0-cmake.tar.xz -C /build/deps/boost-1.89.0
tar -xJf json.tar.xz -C /build/deps/nlohmann-json-v3.11.3

echo "==> package_release.sh with $(nproc) jobs"
map="-ffile-prefix-map=/build/src=. -ffile-prefix-map=/build/out=out -ffile-prefix-map=/build/deps=deps"
export CFLAGS="$map" CXXFLAGS="$map" LDFLAGS="-Wl,--strip-debug"
export OMODACHI_BUILD_ROOT=/build/out OMODACHI_BUILD_TOOLS=/build/tools/venv/bin
export FFMPEG_PREPARED_BINARIES=/build/deps/ffmpeg-v2026.910.121303/ffmpeg
export BOOST_SOURCE_DIR=/build/deps/boost-1.89.0/boost-1.89.0
export JSON_SOURCE_DIR=/build/deps/nlohmann-json-v3.11.3/json
export OMODACHI_PACKAGE_BUILD=/build/out/build-release OMODACHI_PACKAGE_STAGE=/build/out/stage
export OMODACHI_PACKAGE_OUT=/build/out/dist OMODACHI_BUILD_JOBS=$(nproc)
rm -rf /build/out/build-release /build/out/stage /build/out/dist
cd /build/src
bash scripts/package_release.sh
echo "==> built in $(( $(date -u +%s) - start )) s"
