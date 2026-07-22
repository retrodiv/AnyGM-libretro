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
2. It supplies an `AnygmContentSource` to `anygm_load`.
3. It queries stable AV properties with `anygm_get_av_info`.
4. Once per frame it calls `anygm_run_frame` with normalized input and consumes
   the returned video and audio views before the next call.
5. It uses `anygm_state_size`, `anygm_state_save`, and `anygm_state_load` for
   save states and rewind.
6. It calls `anygm_unload` and `anygm_destroy` at the corresponding lifecycle
   boundaries.

Host callbacks cover diagnostics, monotonic and wall time, entropy, virtual
files and directories, locale-aware date formatting, rumble, font resolution, development settings,
optional native rich-text rendering, and platform capabilities. Optional callbacks have deterministic
fallbacks or produce an explicit unsupported result.

## Ownership

`AnygmEngine` is the root of all mutable emulation state. It owns or binds the
loaded content model, resolved compatibility profile, VM, renderer, audio
state, particle state, input history, diagnostics, paths, configuration, and
state-serialization scratch data. Each engine retains its own copy of the host
service table and userdata.

No mutable execution cache may be shared implicitly between instances.
Immutable tables and generated constants may be process-wide `const` data.
The integration test interleaves two engines, mutates and serializes them, and
checks that neither instance changes the other.

Frame output buffers remain engine-owned. Their views are valid only for the
documented call interval; the host copies or presents them before calling into
that engine again.

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

## Performance boundary

The host interface does not require a slower runtime. Function-pointer calls
occur at coarse boundaries: opening or reading a file, obtaining a clock value,
logging, resolving a font, or publishing a frame. The engine does not call the
host for each opcode, collision candidate, pixel, or audio sample.

Hot loops therefore remain normal C calls over engine-owned data. A future host
can batch presentation or file operations without changing engine semantics.
Performance-sensitive callbacks should still avoid unnecessary allocations,
and profiling should measure complete frame phases rather than assume that the
API seam is expensive.

## Libretro adapter

`src/adapters/libretro/libretro_entry.c` owns the official `retro_*` entry
points. The other files in that directory translate libretro environment/VFS,
options, and input facilities into the public API. The linked core exports only
the official libretro surface.

Deleting `src/adapters/libretro/` and its entries in `Makefile.common` leaves a
buildable framework-neutral runtime library. No portable module includes
`libretro.h`.

The isolated-runtime acceptance check performs this cut in a temporary copy;
it does not rely only on the source list. See `PORTING.md` for the host-facing
contract and `SECURITY_MODEL.md` for the VFS trust boundary.
