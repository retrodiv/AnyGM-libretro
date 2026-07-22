<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 retrodiv <retrodiv@proton.me> -->

# Building

## Requirements

The Unix build requires a C11-capable C compiler, an archiver, POSIX threads,
and a standard `make` implementation. The production build does not download
dependencies, run generators, or require a content corpus. All required
headers and third-party sources are present in this checkout.

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

## Validation targets

```sh
make architecture-check
make api-check
make contract-check
make integration-check
make check
make sanitizer-check
make export-check
```

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
