# Reproducing release `sunshine-e9f1b08`

This directory rebuilds the binary package of release
[`sunshine-e9f1b08`](https://github.com/omodachi/omodachi-sunshine/releases/tag/sunshine-e9f1b08),
`omodachi-sunshine-e9f1b08-x86_64.tar.zst`
(SHA-256 `1d13e3aa16ef7bd68daba216a01701f4f06630135f7ddc40a68f56234c98cef7`),
from public inputs only, and compares the result with the published file byte
for byte.

## What it proves

That the published archive, including the `sunshine` executable in it
(SHA-256 `5198863eea0ce78adbc29d7d235e58a230bb0530597f463a5e9cf4a97f1aed0c`),
is what this repository's commit
`e9f1b089255e235b5e6aa13ed89b7dd3f569de87` (tree
`f0a71093dbe05714722ef8bccb0803cd3af4a0eb`) builds to with that commit's own
`scripts/package_release.sh`, given:

- the toolchain of the original build machine: the 245 Arch Linux packages in
  `packages.txt`, at those exact versions, installed from the
  [Arch Linux Archive](https://archive.archlinux.org/repos/2026/09/28/) as the
  repositories stood on 2026-09-28 (GCC 16.2.1, binutils 2.47, CMake 4.4.3,
  Ninja 1.13.2, GNU tar 1.35, zstd 1.5.7), plus Jinja2 3.1.6, MarkupSafe 3.0.3
  and setuptools 80.10.2 (`python-tools.txt`, pinned by hash);
- the three dependencies that are not in the tree, each checked by SHA-256:
  LizardByte/build-deps `v2026.910.121303` `Linux-x86_64-ffmpeg.tar.gz`,
  `boost-1.89.0-cmake.tar.xz` and nlohmann/json v3.11.3 `json.tar.xz`;
- the flags the release was built with: `-ffile-prefix-map` for the source,
  build and dependency directories, and `-Wl,--strip-debug`.

It does not prove anything about those inputs themselves (the compiler, the
prebuilt FFmpeg); their own provenance is their publishers'.

## Run it

On an x86_64 Linux machine with Docker (or `ENGINE=podman`), `curl` and
network access, from a checkout of this repository:

```sh
reproduce/reproduce.sh
```

It builds the image in `Containerfile`, runs `build.sh` in it, downloads the
published archive from the release, runs `compare.sh` on the two, and leaves
everything in `./reproduce-out`. It exits 0 only when the last line is
`RESULT: MATCH 1d13e3aa…`.

Time: about 10 minutes on 4 cores and 16 GiB of memory (the shape of a
GitHub-hosted `ubuntu-latest` runner): 4 to 5 minutes for the image, most of it
downloading 383 MiB of packages and exporting the image, and 5 to 6 minutes for
the build. With 24 cores the build takes about 2 minutes. It needs under 10 GB of disk.

## Expected output

The one file that depends on the machine is `SOURCE`: its `built_on` line is
`uname -srm` of whoever runs the build. The release was built on
`Linux 6.19.11-zabbly+ x86_64`. On another kernel, `compare.sh` prints, for
example on Ubuntu 24.04's (hashes shortened here):

```
rebuilt    5e0fa7a5a8ffffd7…  8113319 bytes
published  1d13e3aa16ef7bd6…  8113316 bytes
tar headers: DIFFER
7c7
< -rw-r--r-- 0/0             340 2026-09-28 18:11:34 omodachi-sunshine-e9f1b08-x86_64/SOURCE
---
> -rw-r--r-- 0/0             338 2026-09-28 18:11:34 omodachi-sunshine-e9f1b08-x86_64/SOURCE
--- per entry: same|DIFF|ONLY-<side>  <rebuilt sha256 or type/mode>  <path>
same  (directory 755)  ./omodachi-sunshine-e9f1b08-x86_64
same  e3b0c44298fc1c14…  ./omodachi-sunshine-e9f1b08-x86_64/BUNDLED
same  80ee5cdce71d6996…  ./omodachi-sunshine-e9f1b08-x86_64/DEPENDS
same  3972dc9744f6499f…  ./omodachi-sunshine-e9f1b08-x86_64/LICENSE
DIFF  caedd7771a9a99f4… vs 90fe7f94be08e722…  ./omodachi-sunshine-e9f1b08-x86_64/MANIFEST.sha256
same  6e679d2d6fe2c1a7…  ./omodachi-sunshine-e9f1b08-x86_64/NOTICE
DIFF  32bb9ae59b783b93… vs c874398d013532f8…  ./omodachi-sunshine-e9f1b08-x86_64/SOURCE
same  (directory 755)  ./omodachi-sunshine-e9f1b08-x86_64/assets
…                      (the 11 asset files and 4 directories: all same)
same  5198863eea0ce78a…  ./omodachi-sunshine-e9f1b08-x86_64/sunshine
--- 2 entries differ
diff rebuilt published: ./omodachi-sunshine-e9f1b08-x86_64/MANIFEST.sha256
    5c5
    < 32bb9ae59b783b93…  ./SOURCE
    ---
    > c874398d013532f8…  ./SOURCE
diff rebuilt published: ./omodachi-sunshine-e9f1b08-x86_64/SOURCE
    7c7
    < built_on=Linux 6.8.0-147-generic x86_64
    ---
    > built_on=Linux 6.19.11-zabbly+ x86_64
--- with the published SOURCE, the recomputed MANIFEST.sha256 equals the published one
repacked   1d13e3aa16ef7bd68daba216a01701f4f06630135f7ddc40a68f56234c98cef7  8113316 bytes  (mtime @1790619094 = 2026-09-28T18:11:34Z)
RESULT: MATCH 1d13e3aa16ef7bd68daba216a01701f4f06630135f7ddc40a68f56234c98cef7
```

The `sunshine` executable and every other file are identical as built. The
tar headers differ only in the size of `SOURCE`, because the kernel name has a
different length. `MANIFEST.sha256` differs only because it lists the
SHA-256 of `SOURCE`. The repack puts the published `SOURCE` into the rebuilt tree, recomputes the
manifest with `package_release.sh`'s own command and packs the tree with its
own `tar` line; the result is the published file, bit for bit.

To skip the repack, tell the build which kernel name to record:

```sh
reproduce/reproduce.sh --built-on 'Linux 6.19.11-zabbly+ x86_64'
```

Then `uname -srm`, and only that exact call, answers with the given string,
and the rebuilt archive itself has SHA-256 `1d13e3aa…` and `compare.sh`
reports no differing entry.

## Files

- `Containerfile`: the build machine. Its base image is pinned by digest; every
  package is then reinstalled from the Arch Linux Archive, and the image build
  fails unless `pacman -Q` equals `packages.txt`.
- `packages.txt`: `pacman -Q` of the machine that built the release.
- `python-tools.txt`: the build-tools venv, pinned by hash.
- `build.sh`: clones this repository, checks out `e9f1b08` and checks its commit
  and tree ids, downloads and checks the three dependencies, runs
  `scripts/package_release.sh`.
- `compare.sh <rebuilt.tar.zst> <published.tar.zst>`: the comparison above.
- `reproduce.sh`: the one command.

`.github/workflows/reproduce-e9f1b08.yml` runs the same command, with
`--built-on`, on a GitHub-hosted runner and publishes its logs. When the archive
that run built has the published SHA-256, the run attests that file with
`actions/attest-build-provenance` and checks the attestation against the
published file with `gh attestation verify`.
