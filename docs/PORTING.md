<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 retrodiv <retrodiv@proton.me> -->

# Porting AnyGM to another host

This document describes how to embed AnyGM in SDL or another framework without
carrying libretro into the new host.

## Cut line

Keep these directories unchanged:

```text
src/api
src/core
src/compatibility
src/content
src/runtime
src/video
src/audio
src/host
src/generated
src/third_party, except the libretro headers if no longer needed
```

Replace `src/adapters/libretro/` with a new adapter directory. Omit
`ANYGM_LIBRETRO_SOURCES` and `ANYGM_LIBRETRO_INCLUDE_DIRS` from the host build,
then link the new adapter against `libanygm_runtime.a` or the same sources used
by the `runtime` target. The runtime target is the executable proof of this
cut: it neither compiles nor links the libretro adapter.

## Minimum host implementation

1. Include `src/api/anygm.h` only. Do not include internal VM or renderer
   headers from the adapter.
2. Fill `AnygmHostServices`, including `struct_size`, ABI version, userdata,
   diagnostics, clocks, entropy, and the virtual-file operations required by
   the selected content source.
3. Translate framework input into one `AnygmInputFrame` snapshot per frame.
4. Call `anygm_run_frame`, present its borrowed XRGB8888 video view, and queue
   its interleaved signed PCM before the next call on that engine.
5. Translate window or device settings into `AnygmConfigDelta`; do not modify
   renderer internals.
6. Store bytes returned by the public state API if the host offers save states
   or rewind.
7. Call `anygm_unload` before replacing content and destroy the engine before
   destroying anything referenced by host userdata.

The dummy host in `tests/contract/dummy_host.c` is the smallest executable
contract example. `src/host/stdio_vfs.c` is a development and test VFS, not a
requirement for a production host.

## Lifecycle and ownership

```text
created/empty --load--> loaded --run/reset/state/config--> loaded
      ^                    |
      +------unload--------+
```

The first API version requires unload before another load. A failed load from
the empty state remains empty. `AnygmEngine` owns all mutable execution state
and borrows the copied service callbacks and their userdata. Each engine has
thread affinity to the thread that creates it; calls on one engine are not
reentrant. Separate engines may be advanced independently.

Frame output pointers are owned by the engine and remain valid until the next
call that advances, resets, unloads, or destroys that engine. A memory content
source is borrowed until unload or destroy. File and directory handles remain
owned by the host and must be closed through the same service table that
created them.

`file_map` and `file_unmap` are an optional paired optimization for large,
immutable path-backed files. A successful map returns an opaque handle plus a
borrowed byte view; both remain valid until the engine returns them to
`file_unmap` during failed loading, unload, or destroy. A host must provide both
callbacks or neither. Hosts without native mapping support leave both null and
the runtime reads through the ordinary VFS callbacks with identical semantics.

## SDL-shaped loop

The following is pseudocode; framework initialization is intentionally outside
the portable runtime:

```c
AnygmHostServices services = make_host_services(platform_context);
AnygmEngine *engine = NULL;

if (anygm_create(&services, &engine) != ANYGM_OK)
    fail();
if (anygm_load(engine, &source, &config) != ANYGM_OK)
    report(anygm_get_last_error(engine, message, sizeof message));

while (!quit) {
    AnygmInputFrame input = translate_events();
    AnygmFrameOutput output = { .struct_size = sizeof output };
    if (anygm_run_frame(engine, &input, &output) != ANYGM_OK)
        break;
    present_video(output);
    queue_audio(output);
}

anygm_unload(engine);
anygm_destroy(engine);
```

## VFS and paths

Paths passed across the API belong to the host VFS namespace. Portable modules
do not call `fopen`, enumerate ambient operating-system directories, or inspect
process environment variables. A host may map paths to native files, archives,
memory, mobile storage, or another provider as long as callback semantics are
stable.

If a host implements the optional immutable mapping pair, it must interpret
the path in the same namespace as its other file callbacks. Returning no map
for a virtual or otherwise unmappable path selects the regular VFS path; it is
not a load failure by itself.

Directory iteration should be deterministic where the provider has no stable
order. Save and cache roots are explicit fields of `AnygmContentSource`; a host
should expose only the capabilities intended for each namespace. Rename used
for cache publication must be an atomic same-filesystem rename. Missing file
capabilities produce a defined failure and never trigger an ambient filesystem
fallback.

When `save_directory` is present, the runtime places writable content files in
`save_directory/anygm/<sanitized-label>-<path-hash>/`. The label normally comes
from the selected file stem. For generic payload filenames, it comes from the
containing directory instead. The hash is derived from the source identity path,
so equally named content at different paths does not share persistent files. A
host should pass a stable identity path if it expects persistence to survive
restarts or content relocation under its own VFS namespace.

## Timing and optional services

The monotonic callback returns nanoseconds from an arbitrary stable epoch.
Wall time is separate and must not be used for profiling. Logging receives a
complete message and an AnyGM severity. Development settings are optional
host-owned strings and should normally be disabled in production.

If the framework has no rumble, locale formatter, font resolver, native rich
text renderer, or optional capability, leave the callback null. Consult field
comments in `anygm.h` for the fallback. Extend the neutral host contract when a
general service is missing; do not call the framework from a portable module.

## Performance guidance

Do not put a callback in an opcode, pixel, collision, instance, or sample loop.
Translate input once, execute the complete frame internally, and present video
and audio in batches. The facade performs no IPC and does not copy the complete
frame at its boundary. Ordinary function-pointer indirection at file, clock,
diagnostic, and presentation boundaries is not expected to determine frame
time.

## Port acceptance checks

A new adapter is ready when it can:

- build without `src/adapters/libretro/` or `libretro.h`;
- include only the public AnyGM header;
- pass the public dummy-host contract with its service table;
- load path-backed and memory-backed synthetic content where supported;
- run two independent engines in one process;
- save and restore state transactionally;
- report diagnostics only through its host callback; and
- shut down with no open VFS handles or adapter-owned callbacks still in use.
