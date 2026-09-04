<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 retrodiv <retrodiv@proton.me> -->

# Building

## Requirements

The Unix build requires a C11-capable C compiler, an archiver, POSIX threads,
and a standard `make` implementation. The production build does not download
dependencies, run generators, or require a content corpus. All required
headers and third-party sources are present in this checkout.

**The compiler must be GCC or Clang.** The runtime uses GNU C extensions that
C11 does not define -- `typeof`, `__builtin_ctzll`, `__attribute__((vector_size))`
and `may_alias` among them -- so MSVC cannot build it. Cross-compilation uses
the usual `CROSS_COMPILE` prefix, except on Apple targets, where the libretro
templates pass `CROSS_COMPILE=1` as a flag and name the target in
`LIBRETRO_APPLE_PLATFORM`; the build recognizes that spelling and turns it into
`-target`/`-isysroot` instead of prefixing the compiler.

**The test suite is POSIX-only** even though the core cross-builds for Windows:
the harness uses `mkstemp` templates under `/tmp`, and `nm` and `cmp` for the
contract checks. Build for Windows from a POSIX host and run `make check`
there.

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
shape. `CC`, `CXX`, `AR`, `CROSS_COMPILE`, `CPPFLAGS`, `CFLAGS`, `LDFLAGS`,
`LDLIBS`, `STATIC_LINKING`, and `BUILD_DIR` remain caller-owned build inputs.

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

`HARDWARE_RENDER=0` excludes `src/video/gpu/` and the adapter's hardware bridge from the source
lists, omits the `anygm_hybrid_gpu` core option, limits `Shaders (GLSL)` to its two OFF reporting
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
shared core and is intended for native Unix release validation.

Use `make clean` only from the checkout whose resolved build directory should
be removed. The target validates that directory before removing generated
objects and known core artifacts.

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
