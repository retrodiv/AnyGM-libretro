<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 retrodiv <retrodiv@proton.me> -->

# Shared video dependency

The runtime uses FFmpeg's public libavformat, libavcodec, libavutil, libswscale
and libswresample APIs. These libraries remain separate, replaceable shared
libraries; no FFmpeg implementation source is imported into the core.
FFmpeg 8 or later development headers and libraries are required. Formats
available at runtime depend on that provider's enabled decoders and demuxers.

## Pinned provider

`ffmpeg.json` pins an upstream release archive, its detached signature,
release-key fingerprint, and a decode-only shared-library configuration.
The archive checksum was verified against the upstream release signature.
This is a reproducible source selection, not a claim that future security
updates are unnecessary. Review and update the pin when changing the provider.

Download the exact archive and signature from the manifest's official URLs.
Verify the signature using the fingerprint published on
[FFmpeg's download page](https://ffmpeg.org/download.html).
The preparation command itself performs no network access. It requires Python
with `hashlib.file_digest` and `tarfile.data_filter`, an ordinary FFmpeg build
toolchain, and NASM on x86 when using the upstream default assembly support.

Choose a private build directory outside the checkout and set
`ANYGM_VIDEO_DEPS` to its absolute path. It must be absent or empty:

```sh
python3 tools/media/build_ffmpeg.py \
  --source-archive "$ANYGM_FFMPEG_ARCHIVE" \
  --work-root "$ANYGM_VIDEO_DEPS" --jobs 1
```

The script checks the reviewed archive SHA-256, extracts with path filtering,
builds the unmodified sources and installs into `install/` below that directory.
It disables GPL, nonfree, version3, external-library autodetection, encoders,
muxers, programs, devices, filters and protocol/network access. The runtime
provides its own bounded VFS memory input. Built-in decoders and demuxers stay
enabled; this configuration does not silently reduce playback to one test format.

`build-inputs.json`, configure/build/install logs, and `build-receipt.json`
record the source pin, configure arguments, environment and installed-file
digests. `--resume` requires the same recorded inputs and continues the same
build directory. It does not discard a stopped build or choose a different source.

Cross compilation accepts `--cross-prefix`, `--target-os`, `--arch`, `--cc`
and `--cxx`. A cross prefix requires explicit target OS and architecture.
Keep target build directories, headers and shared libraries separate from host
packages. The receipt records configuration and bytes; it does not substitute
for executing the target binary on an appropriate host.

## Building and checking the core

Point `PKG_CONFIG_PATH` at the prepared provider's `install/lib/pkgconfig`
directory. `PKG_CONFIG`, `FFMPEG_CFLAGS` and `FFMPEG_LIBS` may instead select
an equivalent provider or a target sysroot. The ordinary core build does not
run the preparation script, download sources, or invoke FFmpeg's configure.

```sh
PKG_CONFIG_PATH="$ANYGM_VIDEO_DEPS/install/lib/pkgconfig" make -j1
```

The loader must also find those shared libraries when running the core. On
a private Unix installation, set `LD_LIBRARY_PATH` to its `install/lib`
directory; on macOS use the appropriate loader installation/rpath policy;
on Windows install the target FFmpeg DLLs where the normal loader finds them.
Do not embed a developer's absolute prefix in a published core.

```sh
PKG_CONFIG_PATH="$ANYGM_VIDEO_DEPS/install/lib/pkgconfig" \
LD_LIBRARY_PATH="$ANYGM_VIDEO_DEPS/install/lib" make ffmpeg-check
```

The native probe calls the five loaded libraries' version, license and
configuration APIs. It rejects GPL, nonfree and version3 providers for this
reviewed LGPL-2.1-or-later route. It checks actual loaded libraries, because a
link-time prefix alone does not control which library a loader selects.
A cross build cannot establish that runtime result without executing on its target.
`make check` includes this native provider gate.

After qualifying a clean producer, `make video-replay-check` runs the engine's
authored movie diagnostic. It creates each snapshot in that execution, repeats
the public state load and requires exact final bytes. Reported process CPU time
includes the state transaction and decoder reconstruction, not frontend ring
copies, GPU time or wall-clock latency. The short fixture does not establish a
bound for arbitrary movies: decoder replay still grows with playback age.

## Continuous integration

The CI entrypoints explicitly run `prepare_ci.py` before Make or NDK. This
adapter invokes the same pinned builder with one job and a private work root
outside the checkout. Network access requires `--download`; an existing local
archive can instead be selected with `--source-archive`. `--plan` prints the
exact builder arguments without fetching, writing or compiling anything.

The adapter preserves MinGW/MXE compiler prefixes, ARM hard-float flags and
NDK API 24 tool selection. Android requires one `ANDROID_ABI` per invocation.
Apple libraries use `@rpath` install names; native CI loads them from its explicit
provider directory. Environment records select only that target's pkg-config
directory, so a previously selected host provider cannot override a cross build.
The generated shell or GitHub environment supplies the core's five link libraries
and the native loader path; it is a private build output, never committed.

Build hosts need Python 3.11 or later with `tarfile.data_filter`, pkg-config,
Make and the selected compiler toolchain; x86 assembly additionally needs NASM.
The Linux GitLab jobs select Ubuntu 24.04 to provide these prerequisites rather
than inheriting a legacy container's Python. Native Apple and MXE/NDK runner
images must provide those host tools. Configuring a cross job is not evidence
that its core or provider has executed on the target device.

## Distribution

AnyGM's adapters retain MIT terms. The FFmpeg libraries retain their own
LGPL-2.1-or-later terms; they are not relicensed as MIT. The complete LGPL
text is included in the core's notice table and in `LICENSES/`.

When distributing FFmpeg binaries, deliver their exact corresponding sources,
original notices, configure/build instructions and any actual patches through
a route satisfying their license. The pinned archive plus build receipt and
an empty patch list identify this recipe's unmodified source. Keep the archive
available with the library distribution; a moving upstream branch is insufficient.
Preserve the recipient's ability to replace the libraries with compatible
modified versions and do not impose a reverse-engineering prohibition that
conflicts with those rights. A package containing only the core still needs
the external runtime libraries installed to play video.

Consult [FFmpeg's licensing guidance](https://ffmpeg.org/legal.html) and the
operative [LGPL-2.1 text](https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html).
Codec patent licensing is separate from the copyright grant and depends on
the distributed codecs, use and jurisdiction; the recipe does not assert
patent clearance for a particular commercial distribution.
