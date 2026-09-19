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

AnyGM runs content authored with these GameMaker generations, in the standard
container each format defines:

- **Game Maker 5.3** — the `.gmd` editor project standard, classic revision 530.
- **Game Maker 6** — the `.gm6` editor project standard, classic revision 600,
  with its adjacent included files and `.gex` extension packages.
- **Game Maker 7** — the `.gmk` editor project standard, classic revisions 701
  and 702.
- **Game Maker 8** — the `.gmk` editor project standard, classic revision 800.
- **Game Maker 8.1** — the `.gm81` editor project standard, classic revision 810.
- **GameMaker Studio 1.x** — the Studio data container (`data.win`,
  `game.droid`, `game.unx`) at bytecode revisions 14, 15, and 16, with classic
  Studio semantics.
- **GameMaker Studio 2.x** — the same data container with modern function,
  struct, and layer semantics. Revision 17 is the common case; an early Studio 2
  package can arrive at bytecode 15, and its generation is decided by the
  container structure rather than by the revision alone.

Two limits apply to every generation:

- **Unprotected content only.** AnyGM loads the format's own container: the
  project, data, or compiled runner file laid out the way that format lays it
  out. A payload that a protection, packing, or obfuscation tool has wrapped,
  packed, re-encoded, or otherwise altered is outside this boundary, however it
  was published or distributed.
- **No native code.** AnyGM executes GML, and it never runs, loads, or links
  machine code. Content compiled with the YYC native compiler carries no GML
  bytecode for the runtime to execute, so it is outside this boundary, and
  neither a native runner nor a native extension library is ever loaded.

### Container forms

- Path-backed content selected by the extensions the core declares to the
  frontend: `win`, `droid`, `unx`, `zip`, `port`, `apk`, `yyp`, `yyz`, `gmd`,
  `gmk`, `gm6`, `gm81`, `exe`, and `anygm`, the anchor that names the payload
  beside it; see [Supported formats](docs/SUPPORTED_FORMATS.md).
- Single-runtime PE executables carrying one embedded, unfiltered LZX-21
  Cabinet with a normalized Studio payload and external runtime assets.
- Executables carrying one unambiguous, structurally valid embedded Studio
  data image.
- PE launchers paired with an adjacent normalized `data.win`.
- Memory-backed content through the framework-neutral AnyGM API.
- Explicit ordered input transformations and external xdelta/VCDIFF patch chains,
  declared in configuration with source, patch and result integrity checks; see
  [content transformation pipelines](docs/CONTENT_TRANSFORMS.md).

Format recognition does not imply that every built-in operation used by every
piece of content is implemented. Unsupported or malformed input is rejected
with a diagnostic instead of selecting a different engine.

## Independence and trademarks

AnyGM is an independent project. It is not affiliated with or endorsed by
YoYo Games Ltd, Opera Norway AS, or the owners of the other interfaces named
in this documentation. "GameMaker", "FMOD", "Wwise", and "Steam" are trademarks
of their respective owners. Their names identify supported formats or
interfaces and imply no sponsorship. This repository contains no proprietary
runtime components or user content. Use only content you are authorized to use.

The core independently implements supported content formats.
[Generated-data provenance](docs/PROVENANCE.md) records
the known sources and verification limits of non-obvious interoperability
constants and generated data used here.

## Build

A release build on a Unix-like host is:

```sh
make -j1
```

The output is `anygm_libretro.so` on Unix, with the platform extension changed
to `dll` or `dylib` where appropriate, and `anygm_libretro_android.so` for the
Android target. A sequential build is recommended on memory-constrained systems
because some generated translation units are large; `tools/run_guarded.py` runs
any command under an enforced memory and time ceiling when the host has a
systemd user manager, and [Building](docs/BUILDING.md) records the limits used
for the cross builds.

Useful targets are:

```sh
make runtime
make architecture-check
make builtin-registry-check
make contract-check
make integration-check
make check TEST=renderer_effects
make check TEST=renderer_postprocess
make check TEST=renderer_surfaces
make check TEST=renderer_tiles
make check TEST=d3_state D3_TEST_ARGS='--case raster.projection'
make check TEST=persistent_room PERSISTENT_TEST_ARGS='--case state.canonical_roundtrip'
make diagnostics-check
make check
```

`runtime` builds the framework-neutral static library without the libretro
adapter. `contract-check` exercises the public API with a dummy host, and
`integration-check` proves that two engine instances can run and serialize
independently. `check TEST=renderer_effects` verifies exact synthetic raster
results for the non-CRT room-layer effects. `check TEST=renderer_postprocess` verifies
the two-sample channel-offset display post-processor against a synthetic
exact-output hash.
`check TEST=renderer_surfaces` verifies deterministic surface allocation,
copy, resize, coverage metadata, and target-stack restoration.
`check TEST=renderer_tiles` verifies modern tileset layout parsing, animation
cadence, source-frame selection, and every mirror/flip/rotate combination.
The D3 and persistent-room binaries are composed from ownership-mirrored
translation units and expose named filters. A filtered software-3D case
replays its bounded prerequisite stages from a clean fixture; persistent
VM, sequence, DS, I/O, audio, timeline, state, and renderer cases are independently
selectable.
`diagnostics-check` builds and tests the separately compiled fine-tracing
instrumentation. A normal build does not contain that instrumentation; see
[`docs/DIAGNOSTICS.md`](docs/DIAGNOSTICS.md) for the diagnostic build and its
strict runtime filters.
`check` is the broader local suite and should be run after a coherent batch of
changes rather than after each mechanical edit.

Cross-builds may set `platform`, `CC`, `AR`, and `CROSS_COMPILE`. The root
`Makefile` follows libretro buildbot conventions and the included GitLab CI
matrix documents the intended Linux, Windows, and Apple compiler targets.
A generic `aarch64-linux-gnu-gcc` distro package can link against a newer
glibc than an older aarch64 target actually has; compare the target's own
`/lib64/libc.so.6` banner against `objdump -T` on the built core before
trusting a fresh cross-build (see `src/host/anygm_libm_compat.c`, which
exists for exactly this reason on one such target).

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

### Core lifecycle and coordination

Within the core, `src/core/engine.c` remains the lifecycle and per-frame
coordinator, `src/core/engine_input.c` owns normalized input and its VM bridge,
`src/core/engine_overrides.c` owns runtime overrides, menu declarations, and
explicit room/intro skip hooks,
`src/core/engine_presentation.c` owns view/aspect/GUI composition, and
`src/core/engine_state.c` owns root state framing and transactional restore.

The engine has one video-owned `GmlSoftware3D` context for fixed-function 3D,
models, and vertex resources; the renderer borrows it and builtins reach it only
through typed subsystem operations.

### Values, VM state, and resources

Language values, arrays, and variable maps use the narrow
`src/runtime/vm/gml_value.h` boundary; consumers that only exchange `GmlVal`
do not need the complete VM state interface. Their one canonical implementation
lives in `src/runtime/vm/gml_value.c`; VM-family representation maintenance is
declared only by `gml_value_internal.h`, while the bounded public
`gml_values_release` operation tears down aliasing value graphs without exposing
that representation. Canonical VM payload sizing, field order, and transactional
restore have one implementation owner in `src/runtime/vm/gml_vm_state.c`.

Builtin-owned INI, file/buffer, asynchronous-request, data-structure,
time-source, spatial-audio, and physics resources live in one opaque
`GmlBuiltinState`; `src/runtime/builtins/gml_builtin_state.c` alone creates,
resets, destroys, visits, and serializes that resource owner. Its four staged
codecs consume the opaque cursor in `gml_vm_state_codec.h` at their established
VM-state positions, so ownership changes do not introduce a second state header.

The compatibility-aware deterministic random
stream has one implementation owner in `src/runtime/vm/gml_vm_rng.c`; room
lookup and entry, persistent-room state, runtime layers and tilemaps, room asset
warming, path and timeline resources and stepping, and tile-layer mutations
have one owner in `src/runtime/vm/gml_vm_rooms.c`; step ordering, draw
scheduling, frame snapshots, event drains, visual-filter decoding, read-only
tile-mutation projection, and frame scratch have one owner in
`src/runtime/vm/gml_vm_frame.c`.

Bytecode dispatch, variable resolution, callable invocation, and code-cache
analysis have one owner in
`src/runtime/vm/gml_vm_exec.c`; object parsing, instance lifetime, event
dispatch, collision, and boundary behavior have one owner in
`src/runtime/vm/gml_vm_instances.c`; private cross-owner VM operations remain
limited to `gml_vm_internal.h`. `src/runtime/vm/gml_vm.c` retains lifecycle,
subsystem wiring, input services, shared comparison/byte utilities, and coarse
control.

### Builtin names

Builtin name resolution is characterized independently by
`make check TEST=builtin_dispatch`; exact and cached builtins, aliases, scripts,
function values, and deliberate fallbacks retain one tested precedence while
the implementation is divided into searchable family owners.

`src/runtime/builtins/gml_builtin_registry.h` is the canonical, searchable
source for every exact builtin name, stable internal ID, family owner, dispatch
stage, and cache policy. `gml_builtin_registry.c` resolves that data through a
deterministically generated immutable index and retains direct integer
execution; `make builtin-registry-check` proves exact coverage against the
family implementations and rejects generated-data drift. Prefix and contextual
fallbacks remain visibly ordered slow paths. In particular,
`gml_builtin_ds.c` owns map/list/grid/priority behavior and DS text envelopes,
while `gml_builtin_json.c` owns the one JSON parser/encoder; their private
boundary exchanges bounded value views and streaming operations rather than
container storage. `make check TEST=builtin_state` proves builtin resource
lifetime, transient-resource exclusion, transactional restore, and exact
save/load/save byte stability.

### Media and renderer

Shared image decode, in-memory PNG encode, inflate, and TrueType raster
implementation live in the path-free `src/media/` leaf; content and video
owners retain host/VFS, atlas-layout, and runtime policy.

The renderer facade owns coarse lifecycle and frame/target coordination,
`gml_render_assets.c` owns asset metadata and runtime-sprite lifetime, and
`gml_render_blit.c` keeps the complete mutually calling sprite, background,
tile, paint, and pixel raster closure. The latter is intentionally cohesive so
hot inner loops do not cross translation-unit boundaries.

The authoritative file-level ownership map is kept in
[`docs/CODE-MAP.md`](docs/CODE-MAP.md).

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
- [Content configuration and SHA-256 selectors](docs/CONTENT_CONFIGURATION.md)
- [Room-layer effects](docs/EFFECT_LAYERS.md)
- [Security model](docs/SECURITY_MODEL.md)
- [Development and validation](docs/DEVELOPMENT.md)
- [Opt-in VM diagnostics](docs/DIAGNOSTICS.md)
- [Code map](docs/CODE-MAP.md)
- [Generated-data provenance](docs/PROVENANCE.md)
- [Security policy](SECURITY.md)
- [Third-party notices](THIRD_PARTY_NOTICES.md)

All first-party source code is available under the MIT License. Bundled
third-party components retain their respective licenses in `LICENSES/` and in
their source notices.
