# Omodachi Sunshine fork

A fork of [LizardByte/Sunshine](https://github.com/LizardByte/Sunshine) that adds an
opt-in Unix-socket control plane so the Omodachi desktop app can drive managed pairing, observe
real media sessions, and bind a stream to an owned output — without a Web admin password and
without changing the GameStream wire protocol or its crypto.

## Upstream base

* Upstream commit: `14ffa6fdaa53f7b51512be2b3d24f3939695403c`
  (*build: bump freebsd version to 14.4 and build-deps to v2026.516.30821 (#5129)*).
* The 13 locked submodules are **vendored as plain directories**, exactly as the
  `managed-sunshine-source-v{1..5}.tar.gz` snapshots carry them. There are no git submodules here;
  `.gitmodules` is upstream's file and is inert in this repo. The locked commits are listed in the
  first commit message and in `MANAGED_SOURCE_MANIFEST.json`.
* Verification of commit 1 used **both** methods and they agree byte for byte:
  1. extract `managed-sunshine-source-v5.tar.gz` and reverse-apply `managed-sunshine-v5.patch`
     (root) and `simple-web-server-connection-identity.patch` (`third-party/Simple-Web-Server`);
  2. `git clone --recurse-submodules https://github.com/LizardByte/Sunshine`, checkout `14ffa6fd`.
  All 1630 regular files and 2 symlinks hash-match. The tarball prunes files from the submodule
  checkouts that the selected Linux build does not need; only the pruned set is carried here.

## History

| Commit | Source patch | What it does |
| --- | --- | --- |
| `vendor: import upstream …` | – | pristine upstream `14ffa6fd` + vendored submodules |
| `managed: private Unix JSONL pairing IPC …` | `managed-sunshine-v5.patch` | the IPC listener, peer/permission gates, managed pairing registry, TLS-bound client identity, `nvhttp` hooks |
| `managed: real media-session observer …` | `managed-sunshine-v5.patch` | `media.list` / `media.get`, display epochs, encoder metadata, `stop_complete` separation |
| `managed: owned-output desktop binding …` | `managed-sunshine-v5.patch` | `desktop.*` lease/claim/prepare/session/stop/release, WLR selected-output support |
| `build: register managed sources …` | `managed-sunshine-v5.patch` (+ v3→v4 NVENC, v4→v5 libcap deltas) | cmake wiring, `tools/managed-local/build-linux.sh`, v5 freeze manifest |
| `third-party/Simple-Web-Server: …` | `simple-web-server-connection-identity.patch` | `Request::connection_identity()` for keepalive-stable TLS ownership |
| `res01: enforce the prepared owned-output geometry contract` | `/tmp/{legacy}-res01-geometry.patch` (host) | `managed_desktop_geometry.h`, 409 on reconnect geometry mismatch, stricter teardown fence, capture refuses stale output |
| `res01: scope JSON duplicate-key rejection to each object` | `/tmp/{legacy}-res01-json-object-keys.patch` (host) | `managed_json.h`, per-object key sets |
| `res01: probe the encoder against the owned output` | `/tmp/{legacy}-res01-owned-encoder-probe.patch` (host) | `validate_encoder` uses the managed output |
| `res01: export every GBM plane for wlr dmabuf capture` | `/tmp/{legacy}-res01-dmabuf-all-planes.patch` (host) | multi-plane dmabuf export + one-shot capture metadata log |
| `pointer: anchor absolute input to the real logical desktop origin` | `negative-output-pointer-origin-r1.patch` | `logical_desktop_bounds.h`, `input_desktop_origin_{x,y}`, touch-port origin fix |
| `input: pin the absolute pointer contract with the compositor's own numbers` | INPUT-1 (this repo) | `src/absolute_pointer.h` (the arithmetic, extracted unchanged), `tests/res01/absolute_pointer_port_test.cpp` (host-measured values) |
| `touch: normalize touch and pen to the captured output, not the streamed frame` | REMOTE-2 (this repo) | `absolute::monitor_port()` in `src/absolute_pointer.h`, used by `monitor_touch_port()`; `tests/res01/touch_port_test.cpp` |
| `pairing: revoke an authorized client certificate for real` | PLUG-4 (this repo) | `src/managed_revocation.h`, `pairing.revoke` in `managed_pairing_control`, `registry_t::request_stop_for_client`, `certificate_revocation_supported`; `tests/res01/revocation_fingerprint_test.cpp` |
| `pairing: list the authorized client store by fingerprint` | PLUG-4 follow-up (this repo) | `pairing.clients`, so a caller can find and revoke certificates it has no record of without reading `sunshine_state.json` |
| `desktop: prepare and serve HEVC when the encoder probed it` | STREAM-1b (this repo) | `valid_profile` accepts `codec ∈ {h264,hevc}`, `matches_stream` requires the launch's bitStreamFormat to be the prepared codec, `desktop.status.encoders`; `tests/res01/hevc_profile_test.cpp` |

The v1..v5 fork history exists only as squashed tarball snapshots, so the four `managed:`/`build:`
commits are a **per-file split of the single v5 root patch by subject area**. Those intermediate
trees are not expected to compile on their own; the tree is coherent again from the
`third-party/Simple-Web-Server` commit onward.

`MANAGED_SOURCE_MANIFEST.json` is the v5 freeze manifest. It is correct at the `build:` commit and
is deliberately left stale by the five later commits.

## What the patches add (condensed from `docs/review/managed-sunshine/README.md` and `docs/IPC-v3.md`)

**Activation.** Nothing is enabled unless `SUNSHINE_MANAGED_LOCAL_PAIRING=1` is set in a normal
same-UID user service. The socket is `$XDG_RUNTIME_DIR/omodachi-sunshine/pairing.sock`
(parent dir 0700, socket 0600), peers checked with `SO_PEERCRED`. Root/setuid activation, a
pre-existing endpoint, an unsafe directory and symlink reuse are refused. No TCP listener and no
network admin endpoint is added. Request frames cap at 4096 B, responses at 32768 B, one JSONL
request per connection, 2 s transport deadline, duplicate keys rejected within an object. PINs,
certificates, key material, challenges and raw requests are never logged or returned; the Sunshine
Web admin password is never read.

**Managed pairing.** A pairing attempt gets a random request UUID, the SHA-256 of the canonical
DER client leaf certificate and a 120 s monotonic deadline; at most 8 pending, duplicates rejected
rather than overwritten. Approval requires the exact request ID *and* fingerprint, consumes one
PIN and returns `awaiting_client_proof` — **never** `paired`. Only the unchanged GameStream
AES/challenge/signature `clientpairingsecret` verification authorizes a certificate. A bounded
recent-result queue lets the broker still read paired/failed/cancelled/expired after the session is
dropped. In managed mode the legacy Web `nvhttp::pin(pin,name)` path is rejected so it cannot
approve an arbitrary map entry; PIN-stdin mode and managed IPC are mutually exclusive.
`pairing.cancel` cancels only a temporary attempt — it is **not** a revoke of an authorized client.
Since PLUG-4 `pairing.revoke {client_cert_sha256}` is the persistent one: it drops every authorized
record whose stored PEM has that DER SHA-256, reloads the verification chain from what survives,
cancels a temporary attempt for the same certificate and signals a stop to the live streams that
certificate authorized. `pairing.list`/`pairing.pending` advertise it as
`certificate_revocation_supported:true`, and a caller that does not see the flag must keep its own
revoke unresolved. `pairing.clients` lists that store as `{client_cert_sha256, name, enabled}` so a
caller can find the certificates it has no record of; it returns no certificate, PEM or key material.

**Media observer.** `media.list` / `media.get` register actual stream sessions (null/unregistered
encoder probes excluded) and expose bounded metadata only: the selected WLR display's backend,
stable output name, pixel and logical dimensions and offsets; a display epoch stamped on captured
images by both async and sync producer callbacks so stale frames/encoder generations cannot be
relabeled; AVFrame/NVENC picture dimensions with an explicit source string; `encoder_name`,
`encoder_backend`, canonical `video_codec` beside the compatibility `codec` field. No bitstream
bytes or pixels. `bitstream_dimensions_independently_parsed=false` and `client_presented=null`
remain explicit. `video_ended` is separate from `stop_complete`, which waits for video/audio/control
teardown, the queued key/button release job and (after res01) the final display/platform teardown.

**Owned-output desktop binding.** `desktop.*` speaks `omodachi.sunshine.desktop.v1` with the lease
object `{lease_id, lease_epoch, owner_device_id, client_cert_sha256}`. `desktop.claim` reserves only
the named `OMODACHI-<16 lowercase hex>` output. `desktop.prepare` is accepted only while stopped,
validates the full geometry contract, and sets a runtime output selector that `refresh_displays`
consumes — the WLR backend refuses a missing managed output instead of falling back to monitor 0.
The claimed lease admits only its matching authenticated certificate fingerprint, carried from the
verified TLS connection into launch/resume. `desktop.stop` signals only the bound stream (never
`terminate_sessions()` or a host quit) and returns `stopped` only after the real stop fence;
`session_not_stopped` is a retryable answer. `desktop.release` needs no live session and removes the
override, leaving the user's persisted configuration untouched. No arbitrary shell, persistent
config edit or physical-output change is exposed.

Full operation/response contract: `docs/IPC-v3.md`, `docs/ipc-v3.schema.json`,
`docs/ipc-v3.examples.json` (the examples use synthetic values; they are not runtime receipts).

## Build recipe used on the host

`tools/managed-local/build-linux.sh` is the private recipe (compile and link only — it never runs a
test binary, installs, sets capabilities or starts a daemon). It checks pkg-config for
`libcurl miniupnpc libcap libdrm wayland-client egl gl libevdev libva x11 numa gio-2.0 gio-unix-2.0
opus libpulse`, requires the prepared FFmpeg bundle (`libavcodec.a libavutil.a libswscale.a
libcbs.a`, including the private CBS headers), and uses the private Python venv with `jinja2` and
`setuptools<81` for GLAD.

```
cmake -S <source> -B <build> -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_DOCS=OFF -DBUILD_TESTS=OFF \
  -DCMAKE_INSTALL_PREFIX=<install dir> -DSUNSHINE_ASSETS_DIR=<install dir>/assets \
  -DSUNSHINE_ENABLE_TRAY=OFF -DSUNSHINE_ENABLE_CUDA=OFF -DCUDA_FAIL_ON_MISSING=OFF \
  -DSUNSHINE_ENABLE_DRM=OFF -DSUNSHINE_ENABLE_VULKAN=OFF -DSUNSHINE_ENABLE_X11=OFF \
  -DSUNSHINE_ENABLE_KWIN=OFF -DSUNSHINE_ENABLE_PORTAL=OFF \
  -DSUNSHINE_ENABLE_WAYLAND=ON -DSUNSHINE_ENABLE_VAAPI=ON \
  -DFFMPEG_PREPARED_BINARIES=~/.cache/{legacy}-sunshine-build/deps/ffmpeg-v2026.910.121303/ffmpeg \
  -DFETCHCONTENT_SOURCE_DIR_BOOST=~/.cache/{legacy}-sunshine-build/deps/boost-1.89.0/boost-1.89.0 \
  -DFETCHCONTENT_SOURCE_DIR_JSON=~/.cache/{legacy}-sunshine-build/deps/nlohmann-json-v3.11.3/json \
  -DGLAD_SKIP_PIP_INSTALL=ON -DPython_EXECUTABLE=~/.cache/{legacy}-build-tools/venv/bin/python \
  -DSUNSHINE_PUBLISHER_NAME='Omodachi' \
  -DSUNSHINE_PUBLISHER_ISSUE_URL='https://github.com/omodachi/omodachi-sunshine/issues'
cmake --build <build> --target sunshine --parallel 4
```

### Absolute pointer input (2026-09-19, INPUT-1)

**No behaviour changed here.** The arithmetic moved into `src/absolute_pointer.h` so it can be
unit tested without a session, and `tests/res01/absolute_pointer_port_test.cpp` locks it against
numbers measured on the host.

`client_to_touchport()` maps a client coordinate onto the streaming surface, removes the letterbox
padding, converts to captured pixels (`scalar_inv`) and then to the compositor's logical units
(`scalar_tpcoords`), finally adding the captured output's origin inside the desktop — minus the
desktop's own origin, which is `a41e122`'s contribution. `platf::abs_mouse()` then gets a port the
size of that whole desktop (`env_logical_*`, or `env_*` without logical geometry), and on Linux
`inputtino::Mouse::move_abs()` scales `x / port.width` onto a fixed 19200x12000 absolute range and
adds no offset.

INPUT-1 started from the hypothesis that the port should be the captured output instead, with the
origin subtracted, and that Hyprland's per-device `output` binding would place it. Measured on
omarchy (Hyprland 0.56.2) with a uinput device that copies inputtino's descriptor exactly:

* the desktop-sized port is correct. In takeover (one enabled output, 2560x1850 at scale 2, logical
  1280x925 at x=2304) and in extend (the same output beside a 2304x1440 eDP-1, desktop union
  3584x1440), the five corner and centre coordinates land within **one logical pixel** of their
  target — `2367,46 · 3519,46 · 3519,878 · 2367,878 · 2943,462` against `2368,46 · 3520,46 ·
  3520,879 · 2368,879 · 2944,462`;
* the per-output port is wrong in extend: the same top-left corner lands at `179,71`, i.e. 5% of
  the *layout* box, because nothing downstream adds the output's origin back;
* Hyprland has no per-device `output` binding for a pointer at all —
  `hyprctl getoption "device:mouse-passthrough-(absolute):output"` answers `no such option`, and
  setting it through `hl.device` changes nothing. An absolute pointer is resolved against the
  bounding box of the enabled monitors, which is exactly what this code targets.

Touch and pen are the opposite case and are unchanged: `monitor_touch_port()` divides by the
captured monitor's logical size and subtracts the same origin, and `platf::touch::update()` passes
only those 0..1 coordinates to inputtino, so those devices need the compositor's own output binding
(`omodachi-core` sets it for `touch-passthrough` and `pen-passthrough`).

### Touch and pen normalization (2026-09-19, REMOTE-2)

`monitor_touch_port()` divided a touch coordinate by the **streamed** frame size
converted to logical units, `(config.width * scalar_inv) / scalar_tpcoords`. It
now divides by `absolute::monitor_port()`, which is `output_width()` /
`output_height()`: the same expression minus the letterbox, `2 * client_offset_*`.

`client_to_touchport()` has already taken that padding out of the coordinate
(`x = (x - offsetX) * scalar_inv`), so leaving it in the divisor normalizes the
picture against a box larger than the output on the padded axis. The picture
then covers only `1 - 2*offset/size` of that axis: every touch lands short of its
target, proportionally, and the far edge cannot be reached at all. For a
1920x1080 output streamed as 1920x1200 the factor is exactly 0.9. The unpadded
axis is unaffected, which is why this shows up as an error on one axis only.

**This is not the finding INPUT-2 §5.1 reported**, and that finding does not
survive re-checking. INPUT-2 read the `Touch passthrough` device's `ABS_Y`
against 12000, which is the absolute *mouse*'s range
(`third-party/inputtino/src/uinput/mouse.cpp:23`); the touchscreen declares
`TOUCH_MAX_Y = 10800` (`third-party/inputtino/src/uinput/touchscreen.cpp:20`).
Its own two samples, `y=5399` and `y=533`, are 0.49991 and 0.04935 of 10800
against client fractions of 0.50000 and ~0.0494 — exact to ~0.1%, not 0.9011x.
Those sessions never met the bug fixed here because the host plans the streamed
geometry from the client's viewport, so their stream and their captured output
had the same aspect ratio and `client_offset_*` was zero.

`tests/res01/touch_port_test.cpp` locks both: the aspect-matched geometries
INPUT-1 measured must not move, and a padded geometry must normalize its picture
onto the whole output on both axes.

### HEVC on the owned output (2026-09-23, STREAM-1b)

Until STREAM-1b the owned-output contract was H.264 only in three places even though the host's
`hevc_vaapi` passes Sunshine's startup probe: `valid_profile()` refused any `codec` but `"h264"`,
`matches_stream()` required bitStreamFormat 0, and `desktop.status` did not say what it could
encode. Now a profile may name `"hevc"`, the RTSP launch must request exactly the prepared codec
(0 = H.264, 1 = HEVC; anything else is the same `409 Prepared geometry mismatch` as a wrong size),
and `desktop.status.encoders` is `["h264"]`, or `["h264","hevc"]` when the configured encoder is
available and `video::active_hevc_mode >= 2` after the probe. `omodachi-core` reads only that list
(never Leo's `desktop-runtime.json`) to decide whether HEVC may be planned. AV1 is still never
preparable. `tests/res01/hevc_profile_test.cpp` locks all three.

### The assets directory (2026-09-18, PERF-3)

`SUNSHINE_ASSETS_DIR` is **compiled into the binary** and has no runtime override: upstream's
`cmake/compile_definitions/common.cmake` defaults it to `assets`, `unix.cmake` then prefixes
`${CMAKE_INSTALL_PREFIX}`, and `src/platform/linux/graphics.cpp` derives
`SUNSHINE_SHADERS_DIR = SUNSHINE_ASSETS_DIR "/shaders/opengl"` from it at compile time. Nothing
reads an environment variable or a config key for it; the only upstream alternative is the AppImage
trick in `cmake/compile_definitions/linux.cmake`, which merely rewrites the path to a *relative*
`./usr/local/assets` resolved against the process working directory — useless under systemd.

Every build before this one left the default, so the binary looked for
`/usr/local/assets/shaders/opengl/*`, a directory this recipe never creates. The OpenGL
colour-conversion shaders read back empty, `h264_vaapi` failed to build its pipeline
(`0:1(1): error: syntax error, unexpected end of file` ×5, then `Encoder [vaapi] failed`) and
Sunshine fell back to `libx264 [software]` — PERF-2 §6.

The fix is configure-time only: point both `CMAKE_INSTALL_PREFIX` and `SUNSHINE_ASSETS_DIR` at the
per-commit install directory, and copy the asset tree next to the binary, so nothing is written
under `/usr` or `/usr/local`:

```
~/.local/share/omodachi/sunshine/<short sha>/
├── sunshine
└── assets/
    ├── apps.json  box.png  desktop.png  desktop-alt.png  steam.png
    └── shaders/opengl/{ConvertUV.frag,ConvertUV.vert,ConvertY.frag,Scene.frag,Scene.vert}
```

cmake already stages exactly that tree into `<build>/assets` at configure time
(`cmake/packaging/common.cmake` + `cmake/packaging/linux.cmake`, with `shaders` as a symlink back
into the source), so the install is `cp -RL <build>/assets/. <install>/assets/` plus the binary.
`cmake --install` is deliberately **not** used: it would also try to write udev rules, a systemd
user unit and desktop/metainfo files outside this prefix. `<build>/assets/web` does not exist
because the recipe builds only the `sunshine` target, never the `web-ui` target — the Web admin UI
is unused in managed mode.

`tools/managed-local/build-linux.sh` does this: `OMODACHI_INSTALL_DIR` (default
`~/.local/share/omodachi/sunshine/<short sha of HEAD>`) feeds both cmake flags, and
`OMODACHI_INSTALL=1` performs the copy. Without that variable the script is still compile/link
only, and it never sets capabilities, writes a unit or starts anything.

**Note for `omodachi-core`:** `scripts/install_host.py` was deliberately left alone. When it grows
a Sunshine install step it must place `assets/` (at minimum `assets/shaders/opengl/`) beside the
binary in the same per-sha directory that `ExecStart` points at, and the binary must have been
*configured* for that exact path — moving an already-built binary to a different directory
silently reintroduces the software-encoder fallback.

Throughout this file `{legacy}` stands for the pre-rename spelling of the project name; see the
"2026-09-18 rename" section below. The host's dependency cache, build-tools venv and the v3-era
build directories still carry that spelling and were deliberately left where they are.

On omarchy the incremental rebuilds after v5 used `<build>` =
`~/.cache/{legacy}-sunshine-build/v3/build-managed-v1` (not the script's default
`$build_root/build-managed-v1`, which is a stale v2-era cache with `BUILD_TESTS=ON`); the cached
flags are otherwise identical to the list above. Source dir was
`~/.cache/{legacy}-sunshine-build/sources/v4-8da80f7dc230c6f6/managed-sunshine` — a v4-named
directory that in fact holds the v5 extract plus the res01 and pointer deltas.

The 2026-09-18 rename build used `<source>` = `~/.cache/omodachi-sunshine-build/src` (an rsync of
this branch's worktree, `.git` excluded) and a fresh `<build>` =
`~/.cache/omodachi-sunshine-build/build`, with the dependency paths above still pointing at the
`{legacy}`-spelled cache.

## The binary that is running on omarchy

Since PERF-3 (this commit): `~/.local/share/omodachi/sunshine/<short sha of this commit>/sunshine`
**with its `assets/` directory beside it**, rebuilt from `~/.cache/omodachi-sunshine-build/build`
with `CMAKE_INSTALL_PREFIX` / `SUNSHINE_ASSETS_DIR` set to that directory (see *The assets
directory* above). The exact sha256, timings and the VAAPI log excerpt are in
the Omodachi workspace report PERF-3-vaapi-report, which is not published; the commit id cannot be written here because it is
this commit's own.

Before that, SPEC-B1's binary was `~/.local/share/omodachi/sunshine/fceb3ab/sunshine`,
built 2026-09-18 04:12 local (+10:00) from `~/.cache/omodachi-sunshine-build/build`, sha256
`989516c52751b280e613ec54b1f3d7a9bebf9e5b290543397b1a2e94bda9cc59`, 157994288 bytes. Configure took
4 s and the 266-target compile/link 284 s (`--parallel 4`, GCC 16.2.1), 288 s wall in total. The
rsynced source carries no `.git`, so cmake logs a benign `Got git error while fetching tags: 128`
and the build stamps no commit id into the version string.

Launched by the user unit drop-in
`~/.config/systemd/user/app-dev.lizardbyte.app.Sunshine.service.d/60-omodachi.conf`:

```ini
[Service]
ExecStart=
ExecStart=%h/.local/share/omodachi/sunshine/<short sha>/sunshine capture=wlr encoder=vaapi adapter_name=/dev/dri/renderD129
Environment=SUNSHINE_MANAGED_LOCAL_PAIRING=1
```

The previous drop-in `60-{legacy}-res01.conf` was deleted by the same change.

Before that, the running binary was `~/.local/share/{legacy}/sunshine/pointer-0044a40ad42cf47b/sunshine`,
built 2026-09-17 15:18, md5 `1b5cbd9d3faff11fa90732477c022dca` — identical to
`~/.cache/{legacy}-sunshine-build/v3/build-managed-v1/sunshine`, whose source tree matched this
repo's `main` tip byte for byte (1639 files + 2 symlinks, `diff -rq` clean). That directory and the
four earlier `res01-*` install directories under `~/.local/share/{legacy}/sunshine/` (the
intermediate binaries of the res01 commits, 13:26, 13:58, 14:02, 14:11) are superseded but were
deliberately **not** deleted.

## 2026-09-18 rename

`{legacy}`→`omodachi`. The old spelling is written here only as `{legacy}` because the working tree
must grep clean for it. It now exists only in this repository's history
(commits `81705d2`..`f8bf502` on `main`) and in host-side paths that were deliberately left in
place: the dependency cache, the build-tools venv, the v3 build directories, the old install
directories and the `/tmp/{legacy}-res01-*.patch` files listed in the History table.

What the rename changed, and nothing else — no behaviour was touched:

* IPC desktop protocol string `{legacy}.sunshine.desktop.v1` → `omodachi.sunshine.desktop.v1`.
  The old string now fails the envelope check and is answered
  `{"error":{"code":"binding_mismatch"},"ok":false,"protocol":"omodachi.sunshine.desktop.v1"}`.
* Socket directory `$XDG_RUNTIME_DIR/{legacy}-sunshine/` → `$XDG_RUNTIME_DIR/omodachi-sunshine/`,
  so the endpoint is `/run/user/<uid>/omodachi-sunshine/pairing.sock`.
* Owned-output name pattern `{LEGACY}-[0-9a-f]{16}` → `OMODACHI-[0-9a-f]{16}`, in
  `managed_desktop_control.cpp`, `platform/linux/wlgrab.cpp` and the IPC schema.
* `SUNSHINE_PUBLISHER_NAME='Omodachi'` and
  `SUNSHINE_PUBLISHER_ISSUE_URL='https://github.com/omodachi/omodachi-sunshine/issues'` in
  `tools/managed-local/build-linux.sh`, which also renames its `{LEGACY}_BUILD_*` environment
  overrides to `OMODACHI_BUILD_*`. Its *defaults* now point at `omodachi`-spelled cache paths that
  do not exist on omarchy; the SPEC-B1 build therefore invoked cmake directly with the recipe above
  and the `{legacy}`-spelled dependency paths.
* Comments, log strings and the docs (`docs/IPC-v3.md`, `docs/ipc-v3.schema.json`,
  `docs/ipc-v3.examples.json`, this file). No file or symbol name contained the brand, so none was
  renamed; `managed_*` identifiers are unchanged.

The Omodachi core repository still carries the old constants; they are SPEC-B2's job. Until B2
lands, core's Sunshine socket path and protocol string do not match this binary.

## Validation status (inherited, not re-run here)

Compile/link only. The upstream review notes claim implementer self-tests
(`clang++ -std=c++20 -Wall -Wextra -Werror`) for the socket/peer path, frame bounds, request and
deadline gates, observer probe exclusion, display epochs, stop-state separation and the
claim/prepare/binding/stop/release flow, plus a local NVENC header-order regression. **No
independent re-review, no pairing acceptance test and no stock-client runtime result is claimed.**
Persistent certificate revocation exists since PLUG-4; `tests/res01/revocation_fingerprint_test.cpp`
locks the digest it matches on, and the end-to-end result measured on omarchy is in
the Omodachi workspace report PLUG-4-report, which is not published. This repository does not build, install or publish anything by itself.

## GPL obligations

Sunshine is GPL-3.0-only (see `LICENSE`, `NOTICE`). This fork is a modified version of a GPL work.
If a binary built from this tree is conveyed to anyone else, the corresponding complete source —
this repository, including the vendored third-party trees and `tools/managed-local/build-linux.sh`
— must be offered under the same licence, upstream copyright and licence notices must be retained,
the modifications must be marked with their dates, and the fork must identify its **own** support
channel rather than pointing users at LizardByte's. Since the 2026-09-18 rename the host build sets `SUNSHINE_PUBLISHER_NAME='Omodachi'` and
`SUNSHINE_PUBLISHER_ISSUE_URL='https://github.com/omodachi/omodachi-sunshine/issues'`, so the fork
now names its own support channel instead of LizardByte's.

Conveying a binary built from this tree — including a package attached to this repository's own
Releases — carries the obligation to offer its complete corresponding source. That source is this
repository at the commit the package was built from, and a release has to name that commit.
