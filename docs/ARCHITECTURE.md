<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 retrodiv <retrodiv@proton.me> -->

# Architecture

## Design objective

AnyGM separates two concerns that happen to be delivered together: a portable
GameMaker execution engine and a libretro frontend adapter. The seam is a C API
with host-owned services. Replacing libretro must not require edits to the VM,
content readers, renderer, mixer, compatibility resolver, or state format.

There is one engine implementation. Input generations differ at parsing and at
named compatibility decisions, then converge on the same runtime objects and
execution loop.

## Dependency rule

```text
src/adapters/libretro/*
        |
        | translates lifecycle, input, VFS, options, video, and audio
        v
src/api/anygm.h
        |
        v
src/core/engine.c
   |       |          |            |
   v       v          v            v
content  runtime     video        audio
   |
   v
compatibility profile

portable code ----calls----> AnygmHostServices ----implemented by----> host
```

Dependencies may point downward in this diagram. Portable code must not reach
back into an adapter. An adapter may translate its own framework types into
public AnyGM types, but those framework types never cross `src/api/anygm.h`.

## Public seam

The lifecycle in `src/api/anygm.h` is deliberately small:

1. The host fills `AnygmHostServices` and calls `anygm_create`.
2. It either supplies an `AnygmContentSource` to the one-shot `anygm_load`, or calls
   `anygm_load_prepare`, inspects `AnygmContentInfo`, settles any session-long graphics context and
   policy, then calls `anygm_load_start`. Preparation parses and initializes content resources but
   runs no extension, room, or instance event.
3. It queries stable AV properties with `anygm_get_av_info` after startup.
4. Once per frame it calls `anygm_run_frame` with normalized input and consumes
   the returned video and audio views before the next call.
5. It uses `anygm_state_size`, `anygm_state_save`, and `anygm_state_load` for complete save states.
   A high-frequency in-memory resume ring may instead size and write the explicit frame-free form
   with `anygm_state_resume_size` and `anygm_state_save_for_resume`; both forms load through the
   same transactional reader. A host that knows its next call is a save, as a rewind ring
   recording every Nth frame does, may call `anygm_state_prepare` as that frame ends: the engine
   reads what the save needs from the device and writes the runtime section, the picture's
   encoding and the renderer section on workers while the host presents, and the save completes
   from them with the bytes it would have written alone. Any other call discards that work first.
6. It calls `anygm_unload` and `anygm_destroy` at the corresponding lifecycle
   boundaries.

Host callbacks cover diagnostics, monotonic and wall time, entropy, virtual
files and directories, optional immutable file mapping, locale-aware date
formatting, rumble, font resolution, development settings, optional native
rich-text rendering, and platform capabilities. Optional callbacks have
deterministic fallbacks or produce an explicit unsupported result.

## Ownership

[Code map](CODE-MAP.md) is the file-level ownership index. Each row identifies
its authorized dependencies, exclusions, state responsibilities, and focused
validation. [Renderer backends](RENDERER_BACKENDS.md) owns drawing-operation
correspondence, coverage, and fallback policy. [State and cache formats](STATE_FORMAT.md)
owns wire layouts and restore requirements.

| Subsystem | Invariant |
| --- | --- |
| Engine | One `AnygmEngine` owns the mutable execution state and its host service table. Frame views are borrowed for the documented API interval. |
| Content and compatibility | Readers normalize into one content model. Named compatibility policies select semantics inside the shared engine. |
| VM | Execution, frame ordering, room lifecycle, instance lifecycle, RNG, and canonical state each have a single implementation owner. Code caches are derived from content. |
| Values | `gml_value.h` exposes tagged values and bounded operations. Array representation, hashed internals, and teardown stay inside the VM family. State codecs encode fields and references independently of the host layout. |
| Builtins | The registry owns exact names, stable IDs, owner, and cache policy. Exact and ordered dispatch share implementation bodies and preserve script precedence. Family files adapt language values to subsystem operations. |
| Builtin resources | One opaque builtin state owns resources and their staged codecs. It exchanges cursors with the VM state owner at the canonical positions. JSON and DS exchange typed views and operations. |
| Video | The renderer owns draw preparation and target coordination. CPU kernels stay together at pixel granularity. Shared geometry and resource contracts feed the optional graphics executor. |
| Software 3D | One engine-owned opaque context is borrowed by the renderer. Builtins use typed operations, and software 3D reaches the renderer through its backend leaf interface. |
| Graphics | The backend derives device objects from engine-owned state and borrows its context from the host. Device handles, transient plans, and recoverable program caches stay outside serialized state. |
| Host | The API carries host services, normalized input, borrowed frame output, and capability negotiation. Framework types stay in the adapter. |

The VM and builtin private headers declare only the operations with callers
across translation units within their own family. Hot interpreter and raster
operations remain direct calls or header-local leaves. Keep the draw and facade
objects adjacent in the builtin source list to preserve the measured link-layout
constraint. The architecture checks enforce these boundaries and the code-map
inventory. The integration suite interleaves two engines to check isolation.

Language-triggered content replacement prepares and compatibility-resolves the
target before releasing the live runtime. It bounds the replacement chain and
confines the target path to the current content directory. A successful
replacement fires Game End, keeps the writable namespace and program directory,
and starts the new content through the ordinary cold-boot path in the same
engine. Failed preparation leaves the live runtime intact.

A mapped content view borrows immutable host bytes. The content owner returns
its opaque handle once on rejection, unload, or destruction. Ordinary VFS
reads use the same parser when mapping is unavailable.

## Content normalization

`src/content/container/` routes a source to the appropriate reader. The
generation-specific readers in `datafile/`, `bytecode/`, `project/`, and
`classic/` decode external representation into the shared `GmlWin` content
model. Parser-level differences stay in those readers.

After parsing, `src/compatibility/` derives immutable `AnygmContentFacts` and
one `AnygmCompatibilityProfile`. Runtime modules ask named questions such as
whether an alarm triggers at zero or whether instance iteration uses a frame
snapshot. They do not infer a generation repeatedly from raw revision numbers.

This gives format differences a single reviewable home without multiplying
the VM, renderer, or frame loop. See `COMPATIBILITY.md` for placement rules.

`src/media/` is a shared leaf below content and video. It owns vendored image
decode, PNG encode-to-memory, inflate, TrueType raster implementation and
standard-default imohash recognition, but
it does not know paths, host services, content formats, atlas layout, or
runtime policy. Callers perform VFS I/O through their own host boundary and
exchange only explicit byte ranges, owned buffers, opaque font-face lifetimes,
value-copy metrics and bounded read-at callbacks with the media leaf.

## Performance boundary

The host interface does not require a slower runtime. Function-pointer calls
occur at coarse boundaries: opening, reading, or mapping a file, obtaining a
clock value, logging, resolving a font, or publishing a frame. The engine does
not call the host for each opcode, collision candidate, pixel, or audio sample.

Hot loops therefore remain normal C calls over engine-owned data. A future host
can batch presentation or file operations without changing engine semantics.
Performance-sensitive callbacks should still avoid unnecessary allocations,
and profiling should measure complete frame phases rather than assume that the
API seam is expensive.

## Libretro adapter

`src/adapters/libretro/libretro_entry.c` owns the official `retro_*` entry
points. At load it freezes both graphics settings, prepares content, and requests a context when
hybrid presentation is enabled or when an enabled content-GLSL policy finds a catalogue candidate.
`libretro_hw_render.c` negotiates that optional frontend context and forwards its lifecycle through
the public graphics seam; after reset succeeds or fails, it finalizes the policies and starts the
prepared runtime before authored events run. It holds the one ABI-level record the adapter owns and
names no graphics API type. The other files in that directory translate libretro environment/VFS,
options, and input facilities into the public API. The linked core exports only
the official libretro surface.

`libretro_cache.c` owns the adapter's two cache-cleanup policies and a bounded,
session-local list of immediate cache-root children observed by its VFS callbacks.
The VFS owner performs confined cache deletion after all runtime handles are
released, or before any incoming content preparation for global cleanup. Existing
content hashes, directory names and runtime state remain owned by their original
modules; the adapter does not reproduce cache naming or parse cache markers.

Deleting `src/adapters/libretro/` and its entries in `Makefile.common` leaves a
buildable framework-neutral runtime library. No portable module includes
`libretro.h`.

The isolated-runtime acceptance check performs this cut in a temporary copy;
it does not rely only on the source list. See `PORTING.md` for the host-facing
contract and `SECURITY_MODEL.md` for the VFS trust boundary.
