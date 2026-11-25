<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 retrodiv <retrodiv@proton.me> -->

# Building

## Requirements

The Unix build requires C11 and C++11 compilers, an archiver, POSIX threads and
a standard `make` implementation. FFmpeg 8 or later development headers enable
the optional dynamic video provider; `pkg-config` supplies their flags by
default on native builds. Without those headers the core builds with movie
fallback support. The production build does not download dependencies, run
generators, or require a content corpus. Other required library headers and
sources are bundled in this checkout.

The [video dependency guide](../tools/media/README.md) describes the pinned
LGPL-2.1-or-later provider, exact source receipt, runtime dependency check,
and the notices/source required when distributing those libraries. A system
FFmpeg package built with GPL or nonfree components does not satisfy that
reviewed distribution route. `make ffmpeg-check` checks the libraries actually
loaded on a native host; compiling against headers alone is insufficient.

**The compiler must be GCC or Clang.** The runtime uses GNU C extensions that
C11 does not define -- `typeof`, `__builtin_ctzll`, `__attribute__((vector_size))`
and `may_alias` among them -- so MSVC cannot build it. Cross-compilation uses
the usual `CROSS_COMPILE` prefix, except on Apple targets, where the libretro
templates pass `CROSS_COMPILE=1` as a flag and name the target in
`LIBRETRO_APPLE_PLATFORM`; the build recognizes that spelling and turns it into
`-target`/`-isysroot` instead of prefixing the compiler.

**The general test suite is POSIX-only** even though the core cross-builds for Windows:
the harness uses `mkstemp` templates under `/tmp`, and `nm` and `cmp` for the
contract checks. Build for Windows from a POSIX host and run `make check`
there.

The explicit `windows-dll-lifetime-probe` and `windows-dll-lifetime-check` targets
are MinGW x64 exceptions. Cross-build the probe and intentional negative-control
DLL with the same `platform=win`, compiler prefix and private `BUILD_DIR` as the
core. The check requires `WINDOWS_DLL_CORE` and `WINDOWS_DLL_NEGATIVE` as explicit
Windows-visible paths to the unstripped core and generated negative DLL. It
runs both dynamic unload and process-exit modes, requiring the negative fixture
to report a TLS read-after-free and the core to finish cleanly. Every mode runs
in a fresh process with a fifteen-second timeout, no content and no frontend.
Set `WINDOWS_DLL_RUNNER` to an explicit Wine executable when native execution
is unavailable; the DLL arguments must use paths visible to that runner.

The probe resolves the existing COFF emutls symbol without changing exports,
exercises zero-initialized/reused TLS, and instruments only that test process's
loaded image imports. Stripped images or missing instrumentation are failures,
not skipped controls. The negative fixture deliberately includes console-stream
global destruction and must never be linked into a production artifact.

## Core and portable runtime

Build the libretro core sequentially with:

```sh
make -j1
```

Build only the framework-neutral runtime with:

```sh
make -j1 runtime
```

The default Unix outputs are `anygm_libretro.so` and
`build/unix/libanygm_runtime.a`. Set `platform` to select a supported platform
shape (`unix`, `win`, `osx`, `android`, or the Apple `ios`/`tvos` variants), as
the platform-targets section below spells out. `CC`, `CXX`, `AR`,
`CROSS_COMPILE`, `CPPFLAGS`, `CFLAGS`, `CXXFLAGS`, `LDFLAGS`, `LDLIBS`, `STATIC_LINKING`, and
`BUILD_DIR` remain caller-owned build inputs.

`VIDEO_PROVIDER` selects movie support. Its default, `auto`, enables the dynamic
FFmpeg provider when native target headers are discoverable and otherwise builds
a working core whose movies complete immediately. `dynamic` requires FFmpeg 8
target headers through `FFMPEG_CFLAGS`; `none` explicitly builds the fallback
without querying pkg-config or adding FFmpeg SDK flags to compiler commands.
Cross builds never query host pkg-config, so target headers must be selected
explicitly. `FFMPEG_LIBS` is used only by the provider qualification probe: the
core and static runtime resolve the five shared libraries at runtime and retain
no FFmpeg link dependency. `make -s runtime-link-libs` prints the selected
platform loader libraries for C consumers.

At runtime, place compatible FFmpeg 8 shared libraries beside the core or in the
platform's normal library search path. If they are absent or incompatible,
`video_open` logs a bounded warning, reports a closed player and delivers one
`video_end` event so content waiting for a movie can continue. Adding the
libraries later makes the next movie-open attempt use them; the core itself does
not need to be rebuilt.

Core and runtime objects rebuild when the selected compiler commands, compilation
flags or SDK environment change. Linker and archiver settings are tracked separately:
changing only `LDFLAGS` relinks without recompiling. Repeating an unchanged build
retains its objects and library. Dependency files name both absolute and checkout-relative
object paths, so switching the spelling of the same build directory still rebuilds C,
C++ and libretro adapter objects when an included header changes. Use separate
`BUILD_DIR` values for concurrent builds with different settings. If a toolchain or SDK is replaced in place while
its command and settings stay identical, clean that build directory first.

Bundled dependencies compile directly from the source lists below; the core build
does not invoke Autoconf or consume `CONFIGURE_HOST` / `CONFIGURE_BUILD`. The separate
FFmpeg preparation recipe runs upstream's configure before the core build. Set the
actual compiler commands and their flags for cross compilation.

The source lists and their order are defined in `Makefile.common`. A build does
not discover sources by walking the filesystem, which keeps buildbot and local
builds aligned.

## Optional hardware renderer

`HARDWARE_RENDER` selects whether the optional graphics backend is part of the build. It defaults
to `1`.

```sh
make -j1 HARDWARE_RENDER=0
make -j1 HARDWARE_RENDER=1
```

`HARDWARE_RENDER=0` excludes `src/video/renderer/gpu/` and the adapter's hardware bridge from the source
lists, omits the Renderer (`anygm_scene_renderer`) core option, and limits
`Game Shaders (GLSL)` to its two OFF reporting
policies, and produces a binary with no graphics-API symbol in it. Everything else is unchanged:
the same engine, the same recognized software shader families, render-plan software execution,
save states, rewind, and the ordinary CPU video callback. That build is a release gate rather than
an occasional exercise, because it is what keeps the feature removable.

No platform graphics library is added to `LDLIBS` in either mode. The backend resolves every entry
point through the callback the host supplies, so nothing is linked and no cross build probes the
build machine for a graphics SDK.

## Validation targets

```sh
make warnings-check
make architecture-check
make build-config-check
make builtin-registry-check
make api-check
make contract-check
make integration-check
make check
make sanitizer-check
make export-check
```

`warnings-check` builds the production core with `-Wall -Wextra -Werror`.
Warnings owned by vendored libraries are isolated at their object boundary; warnings in AnyGM
sources remain fatal. The full `check` target runs this gate before the synthetic suites and also
treats warnings in the test code as errors.

`builtin-registry-check` proves that every exact builtin implementation branch
has one canonical registry row, stable ID, owner, dispatch stage, and cache
policy, and that the checked-in immutable lookup index is byte-for-byte current.
Run it after changing a language-visible builtin name or owner.

`check` uses only synthetic fixtures. `sanitizer-check` builds the bounded
security and contract suite with AddressSanitizer and UndefinedBehaviorSanitizer
when the selected compiler supports them. `export-check` inspects a linked
shared core against the reviewed entry-point list — the dynamic symbol table of
an ELF library or the export trie of a Mach-O one, recognised by the file's own
magic — and is intended for native Unix and Apple release validation.

Use `make clean` only from the checkout whose resolved build directory should
be removed. The target validates that directory before removing generated
objects and known core artifacts.

## Platform targets

The root `Makefile` selects a platform shape with `platform` and the toolchain
with `CROSS_COMPILE` or with an explicit `CC`/`CXX`/`AR`. The name of the
published library follows the libretro buildbot's per-platform convention.

Default builds disable implicit floating-point contraction in C and C++.
The 32-bit x86 targets use SSE2 scalar arithmetic and require an SSE2-capable
processor. These choices keep multiply/add rounding consistent for a given
operation. Results can still vary across math libraries, host services, or
graphics devices.

```sh
# Linux x86-64 (default) and Linux i686
make -j4 platform=unix core
make -j4 platform=unix CC="gcc -m32" CXX="g++ -m32" BUILD_DIR=build/unix-i686 core

# Linux aarch64, including the Recalbox image
make -j4 platform=unix CROSS_COMPILE=aarch64-linux-gnu- BUILD_DIR=build/unix-aarch64 core

# Linux ARMv7 hard-float (armhf), with a VFPv3-D16 baseline
CFLAGS='-march=armv7-a -mfpu=vfpv3-d16 -mfloat-abi=hard' \
CXXFLAGS='-march=armv7-a -mfpu=vfpv3-d16 -mfloat-abi=hard' \
make -j4 platform=unix CROSS_COMPILE=arm-linux-gnueabihf- BUILD_DIR=build/unix-armhf core

# Windows x86-64 and i686
make -j4 platform=win CROSS_COMPILE=x86_64-w64-mingw32- core
make -j4 platform=win CROSS_COMPILE=i686-w64-mingw32- BUILD_DIR=build/win32 core

# Android arm64-v8a, API 24 or later, with an installed NDK
NDK_BIN=<ndk>/toolchains/llvm/prebuilt/linux-x86_64/bin
make -j4 platform=android BUILD_DIR=build/android-arm64 \
  CC="$NDK_BIN/aarch64-linux-android24-clang" \
  CXX="$NDK_BIN/aarch64-linux-android24-clang++" AR="$NDK_BIN/llvm-ar" core

# Android ARMv7 uses the NDK softfp ABI, distinct from Linux armhf
make -j4 platform=android BUILD_DIR=build/android-armv7 \
  CC="$NDK_BIN/armv7a-linux-androideabi24-clang" \
  CXX="$NDK_BIN/armv7a-linux-androideabi24-clang++" AR="$NDK_BIN/llvm-ar" core

# The buildbot entry point also builds both ABIs into separate directories
<ndk>/ndk-build -j4 -C jni APP_ABI='arm64-v8a armeabi-v7a'

# macOS, natively on either architecture, or with the Apple cross variables the
# libretro osx-arm64 template exports
make -j4 platform=osx CC=clang CXX=clang++ core
make -j4 platform=osx CROSS_COMPILE=1 LIBRETRO_APPLE_PLATFORM=arm64-apple-macos10.15 \
  LIBRETRO_APPLE_ISYSROOT="$(xcodebuild -version -sdk macosx Path)" core
```

Two artifact properties are recorded in the library rather than applied by
whatever loads it. `platform=android` aligns the load segments to 16 KiB,
because a device with 16 KiB pages refuses a library aligned to 4 KiB, and links
the C++ runtime statically so the core depends only on the platform's own
libraries; the resulting `anygm_libretro_android.so` is what `jni/Android.mk`
publishes to the libretro Android runner as `libs/<abi>/libretro.so`, and that
file is also what an Android distribution may name directly. The Windows builds
link the compiler runtime statically for the same reason. Apple targets are
single-architecture, honour `MACOSX_DEPLOYMENT_TARGET`, and export the entry
points through a symbol list generated from the reviewed libretro ABI, because a
Mach-O linker has no version script. The repository's GitHub workflow builds
both Apple targets with warnings as errors, runs `export-check` on each, and
builds the arm64 target a second time through the cross variables above.

## Bounded builds and tests

A translation unit in this tree can reserve several gigabytes, and the kernel
answers an over-commit with a machine-wide stall rather than with a failure of
the command that caused it. `tools/run_guarded.py` puts a command and every
process it starts under an enforced cgroup memory ceiling with swap removed, a
task limit and a wall-clock limit, re-checks those limits from inside the cgroup
before the command starts, and stops the whole tree when the time runs out:

```sh
python3 tools/run_guarded.py --memory-mib 6144 --seconds 1800 -- make -j4
python3 tools/run_guarded.py --memory-mib 1024 --seconds 180 -- make check TEST=content_security
```

Use the build limits for compilation and the suite limits for memory-hungry
tests, keep them separate, and do not run two heavy commands at once. A suite
that must never run unbounded imports `require_limits()` from that file as its
entry check. On a host without a systemd user manager, or without cgroup v2,
the wrapper refuses to run rather than falling back to an unlimited invocation;
arrange an equivalent enforced process-tree limit there.

## Reproducible release builds

Release validation uses one compiler job, a fixed source order, a neutral
locale, deterministic static archives, path-prefix remapping, and a disabled
linker build identifier where supported. MinGW builds also disable the PE
timestamp and link the compiler runtime statically so the core depends only on
Windows system libraries. Record the compiler identity and effective flags with the artifact.
Build twice from clean isolated checkouts with the same toolchain, strip and
package identically, and compare hashes.

Binary equality is expected only for the same platform, toolchain, and build
configuration. A cross-toolchain comparison is not a reproducibility claim.
