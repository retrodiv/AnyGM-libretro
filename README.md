<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 retrodiv <retrodiv@proton.me> -->

# AnyGM

AnyGM is a portable runtime and libretro core for
supported GameMaker content formats. It provides one shared execution engine
for classic containers and Studio bytecode; it does not contain separate
runtimes for each format generation.

The repository is self-contained. It includes the runtime, the libretro
adapter, build files, tests, generated data with recorded provenance, and the
third-party components needed to build the core. It does not include content
or proprietary runtime components.

## Supported format families

- Classic container revisions 600, 701, 702, 800, and 810.
- Studio bytecode revisions 14 through 17.
- Path-backed content selected by the libretro extensions declared by the
  core: `win`, `droid`, `zip`, `port`, `apk`, `yyp`, `yyz`, `gmk`, `gm6`,
  `gm81`, and `exe`.
- Memory-backed content through the framework-neutral AnyGM API.

Format recognition does not imply that every built-in operation used by every
piece of content is implemented. Unsupported or malformed input is rejected
with a diagnostic instead of selecting a different engine.

## Build

A release build on a Unix-like host is:

```sh
make -j1
```

The output is `anygm_libretro.so` on Unix, with the platform extension changed
to `dll` or `dylib` where appropriate. A sequential build is recommended on
memory-constrained systems because some generated translation units are large.

Useful targets are:

```sh
make runtime
make architecture-check
make contract-check
make integration-check
make check
```

`runtime` builds the framework-neutral static library without the libretro
adapter. `contract-check` exercises the public API with a dummy host, and
`integration-check` proves that two engine instances can run and serialize
independently. `check` is the broader local suite and should be run after a
coherent batch of changes rather than after each mechanical edit.

Cross-builds may set `platform`, `CC`, `AR`, and `CROSS_COMPILE`. The root
`Makefile` follows libretro buildbot conventions and the included GitLab CI
matrix documents the intended Linux, Windows, and Apple compiler targets.

## Architecture

The public boundary is [`src/api/anygm.h`](src/api/anygm.h). A host supplies
logging, clocks, entropy, virtual file access, locale, font resolution, and
optional platform capabilities through `AnygmHostServices`. Input enters and
video/audio leave once per frame. The VM, renderer, mixer, content loaders,
compatibility policy, and state machinery remain inside `AnygmEngine`.

The dependency direction is:

```text
libretro adapter
      |
      v
framework-neutral AnyGM API
      |
      v
one engine -> normalized content -> compatibility profile
      |              |
      +---- VM ------+---- renderer / audio
      |
      v
host services supplied by the adapter
```

Libretro is therefore an adapter, not an assumption spread through the
runtime. See [Architecture](docs/ARCHITECTURE.md) for ownership rules and
[Porting](docs/PORTING.md) for the exact replacement seam.

## Documentation

- [Architecture](docs/ARCHITECTURE.md)
- [Building](docs/BUILDING.md)
- [Porting](docs/PORTING.md)
- [Compatibility](docs/COMPATIBILITY.md)
- [State format](docs/STATE_FORMAT.md)
- [Supported formats](docs/SUPPORTED_FORMATS.md)
- [Security model](docs/SECURITY_MODEL.md)
- [Development and validation](docs/DEVELOPMENT.md)
- [Code map](docs/CODE-MAP.md)
- [Generated-data provenance](docs/PROVENANCE.md)
- [Security policy](SECURITY.md)
- [Third-party notices](THIRD_PARTY_NOTICES.md)

All first-party source code is available under the MIT License. Bundled
third-party components retain their respective licenses in `LICENSES/` and in
their source notices.
