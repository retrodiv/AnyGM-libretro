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
5. It uses `anygm_state_size`, `anygm_state_save`, and `anygm_state_load` for complete save states.
   A high-frequency in-memory resume ring may instead size and write the explicit frame-free form
   with `anygm_state_resume_size` and `anygm_state_save_for_resume`; both forms load through the
   same transactional reader.
6. It calls `anygm_unload` and `anygm_destroy` at the corresponding lifecycle
   boundaries.

Host callbacks cover diagnostics, monotonic and wall time, entropy, virtual
files and directories, optional immutable file mapping, locale-aware date
formatting, rumble, font resolution, development settings, optional native
rich-text rendering, and platform capabilities. Optional callbacks have
deterministic fallbacks or produce an explicit unsupported result.

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

Language-triggered content replacement is a core lifecycle transaction over the same
`AnygmEngine`. The target is prepared and compatibility-resolved before the live VM, renderer,
audio context, or content model is released. A successful replacement fires Game End, preserves
the original writable save namespace and program directory, installs the child content, exposes
its working directory and launch parameters to the new VM, then cold-boots through the ordinary
runtime path. Failed preparation leaves the live runtime intact. Target paths are bounded below
the current content directory, reject parent traversal and drive-qualified segments, and retain a
finite replacement-chain depth. This is not a second engine or a compatibility-selected runtime.

Language values form a narrow runtime boundary. `gml_value.h` is the sole
definition owner for `GmlVal`, `GmlArr`, and `GmlVarMap`, their inline
constructors, and the public array and variable-map operations. `gml_vm.h`
includes that value boundary, but value-only consumers do not need the complete
VM state interface. Array ownership, escape marking, and variable-map lifetime
have one canonical implementation in `gml_value.c`; the split does not
introduce another value model.
`gml_value_internal.h` is the VM-family-only boundary for hashed map access,
array representation maintenance, and deduplicated teardown. Builtins exchange
values through `gml_value.h`; they do not include the private header or depend
on array storage mechanics. Cross-unit private symbols retain direct ordinary C
calls and use owner-prefixed names.
`gml_vm_internal.h` is the corresponding VM-family-only operation boundary
between execution, lifecycle, room, and canonical-state implementation owners.
It declares narrow owner-prefixed operations, not another VM record or public
API. Only implementation files directly under `src/runtime/vm/` may include it;
builtins and other subsystems continue to use the coarse `gml_vm.h` interface.
Interpreter extraction uses direct value coercion, instance-field mutation,
hashed global lookup, code-cache preparation, motion synchronization, and
instance-bounds operations. These remain ordinary C calls with no dispatch
table, allocation, or exposure of the instance pool.
The room owner uses direct little-endian readers, opaque instance operations,
typed renderer metadata, audio warming, and ordered initialization. It does
not expose the instance pool, content-reader helpers, runtime-layer storage, or
tilemap storage. `gml_vm_rooms.c` is the sole implementation owner behind that
seam. It keeps room lookup and entry, persistent-room state, runtime layers and
tilemaps, asset warming, path and timeline parsing and stepping, end-event
handling, and tile-layer mutation state and public operations together.
`gml_vm_frame.c` is the sole frame-phase implementation owner. It keeps step
ordering, draw scheduling, asynchronous input/event drains, frame snapshots,
draw scratch lifetime, room-layer visual-filter decoding, and read-only tile
mutation projection together. The compiler can therefore keep complete hot
draw loops and their static helper clusters in one translation unit.
Its cross-owner seam is explicit:
instance event/collision phases, room layer queries and mutations, frame
scratch cleanup, and global-array access use direct VM-family-private calls.
These calls neither allocate merely to cross the boundary nor introduce
callbacks into iteration, collision, or draw hot paths.
`gml_vm_instances.c` is the sole object, instance, event, and collision
implementation owner. It keeps parsing, allocation and destruction order,
event lookup and dispatch, collision geometry and dispatch, and boundary
events together. Frame and room coordinators use its direct owner-prefixed
operations; they do not duplicate pool traversal or collision policy.
`gml_vm_exec.c` is the sole bytecode-execution implementation owner. It keeps
variable and scope resolution, instance-field access, code-cache analysis,
microcode recognition, opcode dispatch, and callable invocation together so
their static dependency graph and allocation-free hot paths remain local.
`gml_vm.c` retains VM lifecycle, subsystem wiring, input services, shared
comparison and byte utilities, and coarse host control.
The private header also owns the one struct-slot ID bit layout shared by
allocation and canonical reconstruction; those constants must not be repeated
in either implementation owner.
`gml_vm_state.c` is the sole canonical VM payload implementation owner. It
retains the existing schema, exact field order, value-graph encoding, and
transactional restore behavior; the root state owner continues to frame that
payload. State-only readers, writers, sort scratch, and restore cleanup remain
with this owner rather than becoming a second VM context. At the four existing
builtin-resource positions it passes an opaque reader or writer cursor to
`gml_builtin_state.c`; only those two owners may include
`gml_vm_state_codec.h`. This delegation adds no independent header or schema and
does not introduce independent state framing.
`gml_vm_rng.c` is the sole implementation owner for the one VM random stream.
It keeps compatibility-policy selection, seed expansion, random advancement,
and optional diagnostics together while the canonical state owner continues
to serialize the same fields in the same order.

Builtin resolution has one canonical exact-name model.
`gml_builtin_registry.h` is the sole source of truth for every exact spelling,
stable append-only ID, family owner, dispatch stage, and call-site cache policy.
`gml_builtin_registry.c` expands that model into metadata, resolves names with
the immutable open-addressed index in
`src/generated/gml_builtin_registry_index.h`, and executes cached IDs with a
direct integer switch and ordinary C calls. IDs whose established bodies were
already in that switch remain there; all other cacheable IDs enter their
declared owning stage directly instead of scanning unrelated families. There
is no function-pointer call per VM operation.

The cache policy is part of observable precedence, not an optimization guess.
Context-sensitive exact operations, exact names that must remain after script
lookup, and the DS diagnostic exception deliberately return no cached ID and
follow the ordered slow path. Dynamic `fmod_` and `gamepad_` prefixes also
remain explicit outside the exact table. The facade preserves compatibility
shadows and script lookup before broad or unknown fallbacks.
`make builtin-registry-check` tokenizes every established family
implementation, proves exact set equality with the canonical registry, checks
stable IDs and stage cases, and regenerates the index byte-for-byte.
`make check TEST=builtin_dispatch` is the focused behavioral oracle for all
exact rows, aliases, cache policies, scripts, function values, dynamic
prefixes, and fallbacks.

`gml_builtin_internal.h` is the private builtin-family operation boundary. It
declares only operations that cross a translation-unit boundary and keeps the
`N` and little-endian `u32` leaves
header-local so argument decoding and immutable content reads do not gain hot
external calls. The same header owns the byte-preserved private
`GmlBuiltinState` layout needed by physical family owners.
`gml_builtin_state.c` is the sole create, reset, destroy, lazy-allocation,
value-root traversal, resource-default, and staged canonical-codec owner.
`GmlVM` retains only an opaque pointer to this object; VM implementation owners
never dereference it. Family files acquire that one engine-owned object through
`gml_builtin_state_ensure` and may touch only their fields. The narrow
`builtin_setting` and `motion_from_components`
operations preserve one shared implementation when a family crosses that
boundary. The legacy-action helper declarations likewise preserve the same
implementations for exact-ID and ordered dispatch, while the actions and
instances stage declarations keep their direct tail edge across physical
owners. The input helper declarations retain one keyboard/gamepad and mouse
implementation for the exact-ID and ordered paths; the input and I/O stage
declarations preserve their direct slow-path edge. One coarse classic
display-coordinate operation keeps presentation size policy in the facade
instead of widening that policy into the input owner. The I/O boundary
declares one find-state reset, two file-slot
operations, and four exact text/INI adapters that remain shared with builtin
state lifecycle or the exact-ID switch. Their bodies remain single and the
boundary exposes neither host-file storage nor an alternate I/O path. The DS
boundary keeps the exact-ID switch on named lookup, iteration, existence, size,
and list operations instead of exposing map hash indices, temporary key
storage, or entry arrays. JSON receives bounded map/list item views and uses
declared creation, mutation, and destruction operations; DS text serialization
uses the reciprocal streaming JSON writer. These are subsystem-private
operations over the one builtin-state-owned DS pools and one JSON
implementation, not
alternate APIs or state owners. Shared string-argument, fixed-array, and
content-relative read-path operations retain one implementation across their
current callers; the audio and
instance-path stage declarations preserve their direct tail edge. Shared array
allocation, time-source lookup, room-order lookup, and script-reference
decoding likewise remain single operations across their current callers.
Room, script, value-language, and early-layer stage declarations
preserve only the direct edges that cross physical owners. The layer boundary
also declares its shared four-value result, layer lookup/touch helpers, and
platform/layer/tail stages so the later physical owner keeps the existing
ordered slow-path calls while the exact-ID facade still reuses one
implementation. This
is not a second builtin API or a place for family implementation bodies.
`gml_builtin_io.c` is the physical owner for wildcard/path normalization,
VFS-backed text and binary files, INI parsing, VM buffer adaptation, MD5/SHA-1
and base64 helpers, asynchronous save/load request queuing, and both established
ordered I/O stages. It uses injected host/VFS services and the existing VM
resource operations; storage lives in the one builtin state. It does not own
host implementations, ambient filesystem
access, DS/JSON conversion, or a second persistence layer. Exact-ID file and
INI calls retain their direct switch cases and cross only through the prepared
private declarations. I/O retains the host-file close implementation, while
the builtin-state owner coordinates reset/destruction and the one shared
file-find reset operation.

`gml_builtin_ds.c` owns map, list, grid, and priority-queue adaptation,
container lifecycle helpers, DS text serialization, and the ordered DS stage.
Map/list/grid storage remains in the one builtin state and retains its existing
positions in canonical VM state bytes. Exact-ID dispatch reaches named DS
operations; JSON sees only bounded
item views and ID-based mutation, never container arrays, hash indices, or
temporary key records. The DS stage delegates misses directly to
`gml_builtin_try_json`.

`gml_builtin_json.c` owns JSON parsing, encoding, the private streaming writer,
and the ordered JSON stage. It converts DS values through typed item views and
mutation operations and does not own DS storage. DS serialization uses the same
writer and decoder through the private boundary, so there is one parser and one
encoder. The JSON stage delegates misses directly to
`gml_builtin_try_values_variables`.

`gml_builtin_draw.c` owns draw, camera, vertex, model, matrix, shader-texture,
and fixed-function language adaptation behind the two existing ordered draw
stages. It preserves direct delegation from the particle stage to draw, from
draw to input, from variable adaptation to D3, and from D3 to animation.
Keep its object immediately after the facade object in `ANYGM_BUILTIN_SOURCES`.
That adjacency preserves the characterized pre-extraction link layout for the
remaining builtin owners; the architecture gate enforces it because moving the
object to the end produced a reproducible frame-time regression.
The registry object follows the draw object, so that physical ownership does
not disturb the characterized facade/draw adjacency.
Rasterization, video resources, and canonical software-3D state stay
video-owned. Draw crossing declarations cover only facade definitions that
still have callers in more than one family, and private forward declarations
retain the same linkage as their definitions. The one-line
`graphics_state_for_render` operation
is header-local and `static inline`, preserving the pre-extraction D3 dispatch
cost for both physical owners. `GML_GRAPHICS` is the only shared header-local
graphics alias and resolves that opaque engine-owned software-3D context; the
former private-state and storage aliases are retired. The boundary exposes
operations and bounded value-copy records, not renderer or software-3D
storage. Crossings with no remaining external caller are removed rather than
kept as a convenience API.

`gml_builtin_animation.c` owns animation-curve chunk lookup, skeleton instance
value helpers and language-visible skeleton adaptation, plus the ordered
animation, GC, shader-fallback, and platform-service fallback stage. Skeleton
adaptation reads DS string keys through the one shared facade lookup; instance
lifetime and value storage remain with the VM. Its final direct delegation to
the physics stage preserves the characterized resolution order while the
families remain an explicit slow-path chain.

`gml_builtin_physics.c` owns the language-visible fixture and joint resource
adapters, room-scale and instance-mass lookup, and the ordered physics stage.
The pools remain fields of the one builtin state and retain their existing
serializer positions through the state owner; the file neither owns a second
physics simulation nor private VM lifecycle.
Its final direct delegation to the platform-extensions stage preserves the
characterized resolution order.

`gml_builtin_actions.c` owns both established legacy action-function
positions: the early portable-extension stage and the later drag-and-drop
action continuation. The later stage keeps its three drawing helpers local and
delegates directly to the instance-query continuation. Helpers still consumed
by exact-ID dispatch remain single facade implementations rather than being
copied. Both positions use ordinary calls and preserve the characterized
resolution order without a function-pointer hop.

`gml_builtin_input.c` owns keyboard-map, gamepad, pointer, and joystick
argument adaptation plus the ordered input stage. The exact-ID path and the
ordered stage retain one shared implementation of their keyboard/gamepad and
mouse-button helpers. Classic display-coordinate conversion remains one coarse
facade operation so presentation-size policy is not duplicated or widened.
The stage delegates unrecognized names directly to the I/O stage, preserving
precedence without indirect dispatch.

`gml_builtin_audio.c` owns audio argument adaptation, spatial-emitter
attenuation and refresh policy, external audio-sidecar loading, the portable
Saudio/SGAudio/SuperSound/BGM/GMFMODSimple/pxwrap adapters, and the ordered audio
stage. `gml_builtin_platform.c` owns generic encoded external-call decoding and
the deterministic no-op policy for offline-only platform libraries; the input
owner maps GMXInput to the normalized host gamepads. Every gamepad and joystick reader takes
an explicit device, and the host contract carries one row per supported port. The adapter presents
every supported port because frontend signals establish capacity and declaration, not occupancy,
while idle and absent input are indistinguishable. The RetroPad behavior option determines
ownership: keyboard emulation reports no pad rows and maps RetroPad input to the keyboard. Shared string conversion, fixed-array construction, and
content-relative path resolution remain single facade operations while exact
and ordered lookup coexist. The stage delegates unrecognized names directly to
the instance-path stage, preserving precedence without indirect dispatch.
Emitter, listener, and falloff storage and their established canonical stage
remain with the builtin-state owner.

`gml_builtin_instances.c` owns language-visible instance creation and
destruction, activation, queries, collision-facing lookup, movement and alarm
adaptation, room flow and view overrides, script and event invocation, path
and timeline adapters, and time-source operations. Instance execution, event
ordering, collision implementation, room lifetime, path stepping, and timeline
scheduling remain with the one VM implementation; this file only adapts
language arguments to those operations. Time-source storage, owned value roots,
defaults, and canonical codec remain with the builtin-state owner. The
value-language stage delegates to
the instance-destruction continuation, which delegates through the intervening
legacy-action owner and returns to the instance-query continuation before
drawing. Instance queries first enter the particle adapter and then the draw
adapter, preserving the established nested particle-before-draw precedence. The
other instance stages retain their established direct edges, including the
final transition to early layers. Shared array allocation,
time-source lookup, room-order lookup, and script-reference decoding remain
single facade operations while exact and ordered lookup coexist.

`gml_builtin_particles.c` owns language-visible particle type, system,
particle, and emitter argument adaptation. Particle simulation, procedural
shape masks, resource pools, draw kernels, and canonical particle state remain
in `src/runtime/particles/`; the builtin owner only normalizes arguments and
calls that subsystem's typed operations. Its ordered stage delegates misses
directly to drawing with an ordinary call.

`gml_builtin_layers.c` owns the exact-name layer adapter, early tile-layer
mutation, and the complete language-visible runtime layer, element, and tilemap
adapters at both ordered positions. Runtime-layer and tilemap lifetime remain
with the one VM; the builtin owner only resolves arguments and mutates that
established storage. Its three entry paths share one four-value constructor and
one set of layer lookup/touch helpers. Early and late stages delegate directly
to the platform and platform-tail stages, preserving both positions in the
established chain.

`gml_builtin_platform.c` owns deterministic platform and offline-capability
adaptation, host-backed wall-time and date formatting, bounded classic
assignment execution, network capability policy, modal and file-dialog
fallbacks, extension/offline-service policy, explicit host/software-renderer
noops, and all three established platform positions. It obtains capabilities
and wall time only through injected host services and performs no ambient
network or operating-system access. Target and presentation metrics, draw-state
snapshots and updates, DS-map insertion, buffer-slot lookup, raw buffer writes,
and the registry-owned script fallback remain single implementations behind
the private builtin boundary while their other callers still reside there. The
primary stage delegates directly to `gml_builtin_try_ds`; the DS owner then
delegates to JSON. The extension stage calls the animation-owned
skeleton and layer-owned exact adapters before the explicit-noop stage, whose
final edge reaches the script fallback. Those ordinary calls preserve all
three precedence positions without indirect dispatch.

`gml_builtin_collision.c` owns language-visible collision geometry, line and
shape queries, contact resolution, the lazily built candidate index, motion,
path and grid planning, nearest/furthest selection, and both established
collision ordered stages. Instance lifetime and collision-event scheduling
remain in the VM. Logging policy, list access, rounding, and motion
synchronization retain one implementation while exact-ID and value consumers
still reside in the facade. Only operations with a current cross-file caller
are declared on the private builtin boundary. The cached grid-mode policy and
motion-planning grids remain in the shared private builtin state. The collision
stage delegates directly to value-math; planning misses return through one
direct values-string continuation on the deliberate slow path, preserving
established precedence without indirect dispatch.

`gml_builtin_values.c` owns the ordered adapter stages for language-visible
math, color construction and inspection, pure geometry predicates, arrays,
strings, string iteration, Unicode conversion, direct string hashing adapters,
variable and struct reflection, method classification, language-level method
binding and struct construction, and recursive value cloning. Its
byte-identical point-in-polygon, string/Unicode, struct-name, and clone-graph
helpers stay translation-unit local beside their only callers. The math stage
delegates to collision planning, the string stage delegates to the INI stage,
the language stage delegates to instance destruction, and the variable stage
delegates to draw/3D, preserving the established order. A same-process paired
performance gate compares the actual baseline and candidate objects for an
early math name and the geometry path; moving the complete predicate kernel
satisfies the fixed tolerance. Value comparison, sorting, array callbacks,
small math helpers, and hash/encoding helpers with exact-ID, DS, JSON, or I/O
facade callers remain in the facade. The value-storage clone used by both
variables and JSON has one implementation in the values owner and one private
declaration. Shared helpers remain private crossings only while they have
callers in multiple physical owners. DS equality, deterministic offline HTTP
behavior, collision distance/motion operations, and
builtin-state lifecycle retain their existing owners; no alternate value
representation or dispatch path is introduced.

The engine also owns exactly one `GmlSoftware3D` fixed-function context through
the VM lifetime. The renderer borrows that context through an explicit binding;
it does not allocate or release it. Language-visible D3 and vertex builtins are
argument adapters over the opaque `gml_software3d.h` operation boundary, not
owners of raster, resource, model, or serialized state. They take one
value-copy active-status snapshot per dispatch branch and use typed masked
control, matrix/stack, resource, primitive, and model operations. Model and
vertex serialization adapters inspect only bounded value-copy records.
High-level renderer paths may enter software 3D, while software 3D may call
back only through the non-reentrant renderer backend leaf interface. This
one-way boundary prevents the former renderer/builtin cycle without creating
a second renderer or exposing video storage.

Renderer implementation files are divided by durable ownership seams.
`gml_render.c` is the coarse renderer facade: it owns lifecycle, frame and
target coordination, draw preparation, presentation, shader controls, and the
non-reentrant target-view backend leaves. `gml_render_assets.c` owns renderer
asset metadata plus the complete runtime-sprite record lifecycle.
`gml_render_blit.c` owns the complete sprite, background, room-tile, paint,
axis-cache, and low-level pixel raster closure. Keeping those mutually calling
hot kernels in one translation unit avoids introducing per-pixel indirection
or cross-unit calls. Its size is therefore an intentional exception to the
soft source-size trigger, supported by the frame-boundary allocation and
performance characterization; new non-raster policy does not belong there.

The coarse presentation passes are additionally described by a framework-neutral render plan.
`gml_render_plan.c` owns the plan's lifetime, bounded recording, validation, the two characterized
axis rules and their composition, and the eligibility facts that decide whether a pass may leave the
software executor. `gml_render_plan_software.c` is that executor and remains the canonical
implementation: an alternative execution is accepted only when it reproduces these pixels exactly,
and any pass that cannot be is replayed here in full. The plan carries value records only — no
graphics API type, no host callback, no function pointer — and is never serialized under any schema.

`gml_render_surfaces.c` owns surface lookup and coverage metadata, allocation,
copy, resize, free, target-stack operations, and the complete normal,
interpolated, part, stretched, and rotated surface composition kernels. The
facade may request one coarse surface composition while flushing a deferred
application-surface underlay; composition may request coarse draw preparation
and recognized post-process owners. No per-pixel call crosses those
translation-unit seams.

`gml_render_effects.c` owns bounded structural recognition of the supported
software shader families plus non-CRT room-layer capture, filtering,
composition, and complete effect pixel kernels. Recognition produces
renderer-owned policy records during initialization; it does not interpret
arbitrary shader code at draw time. The effects owner may use the
renderer-private row-band executor only at pass granularity. Inner pixel loops
and the main sprite/surface mappers remain direct and do not cross an indirect
dispatch boundary.

`gml_render_postprocess.c` owns the recognized display post-process kept in
software, the two-sample channel offset, with its complete sampler, row-band
context, fast/general paths, and pixel kernel. A recognized family is an
operation whose parameters are read from the content; a kernel that would
reproduce a content shader's own expression is not a software family and is
left to the shader executing on the host's graphics context. That execution is the content-program
path: `gml_render.c` keeps, per shader the renderer recognizes no family in, the values and
samplers the content sets by name; `gml_render_presentation.c` records a frame the content drew
through such a shader as a deferred presentation that names the program; `engine_graphics.c` turns
that record into a `SHADER_DRAW` plan operation carrying the program's lent sources, values and
sampler pictures; and `gml_gpu_gl.c` compiles the program once per context, respells it into the
dialect the context accepts, and draws the frame through it. A program the device refuses is
retired by the renderer for the session, so the frame is presented unshaded and `shader_is_compiled`
answers no for it, as it would under a driver that refused it. A surface drawn through such a
program anywhere else in the frame goes through the renderer's executor hook instead:
`gml_render_surfaces.c` asks the host to run the program over the surface at the destination's
size, `engine_graphics.c` executes that as a plan whose target is read back, and the answer is
composed in software as a surface of that size with the draw's own blend and alpha; the software
renderer remains the complete implementation, and a host without a context, or a program the
device refuses, draws plain. A read-back stalls the device, and its cost is the device's: the
engine times what those passes cost the frames that paid for them and, on a device where that
exceeds the per-frame budget, stops waiting: the pass takes what the previous pass of the same
program left, one frame old, and starts its own without waiting, which is what the stall actually
costs. Only if that is still too slow are the mid-frame draws given up for the session; the terminal presentation stays on the device, and the policy is a host option so a
comparison against the original can ask for every shader however slow.

A fragment that samples no picture is a case of its own. It derives every pixel from coordinates,
time and its own uniforms, so leaving it unrun paints the primitive flat in a colour the shader was
going to discard — an unrelated picture rather than a weaker one. That is why such a shader answers
that it did not compile, and why the answer is now conditional: when the session requested a
graphics device it is executed and the answer is yes, otherwise it is still no. An early query can occur before the frontend has adopted the context, so the
answer is taken from whether a device was requested (a stable session fact) rather than from
whether the context is ready this instant. A filled rectangle or a sprite drawn through such a
program reaches the same executor as a surface does. A program applied to a sprite transforms that
sprite's texels, so its answer depends on the frame, the program and the values set on it rather
than on where the frame is drawn: the answer is evaluated once and kept, and every later draw of
the same frame composes from it. This replaces repeated device evaluations with one evaluation per distinct
frame. Complete sprite and surface kernels share the
canonical header-local recognized-shader sampling operations in
`gml_render_sampling_internal.h`; the general surface mapper and post-process
kernels additionally share the two final-pixel operations in
`gml_render_pixel_internal.h`. Sprite and surface composition also share the
canonical header-local cardinal-anchor, opacity-run, fast-alpha, and
bilinear-band helpers in `gml_render_blit_internal.h`. These private definitions
remain direct and header-local, so coarse draw preparation, opacity queries,
profiling hooks, and row-band dispatch may cross renderer implementation units
but no sampling, blending, or final-pixel call does.

An optional graphics target sits beside that plan rather than inside the renderer.
`src/video/gpu/` owns the context lifetime, the capability and resource generations, and one direct
OpenGL / OpenGL ES backend; `src/core/engine_graphics.c` is the only core file that knows it exists,
and the whole feature reaches the rest of the engine through one pointer on `AnygmEngine`. Eligible
plans execute there; everything else keeps the software executor. A `GmlGpu` is a derived cache with
a lifecycle rather than state: it is never serialized, it is absent from the state configuration
fingerprint, and losing all of it costs a rebuild rather than a wrong frame. `HARDWARE_RENDER=0`
removes the directory and the adapter bridge from the source lists and leaves the same engine, the
same renderer, the same state format and the ordinary CPU video callback.

A mapped content view remains host-owned and immutable. Its opaque mapping
handle is retained by the owning content model and returned exactly once on
failed parsing, unload, or destroy. Mapping is a coarse load-time optimization,
not a different content path: hosts that omit it use the same parser through
ordinary VFS reads.

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
decode, PNG encode-to-memory, inflate, and TrueType raster implementation, but
it does not know paths, host services, content formats, atlas layout, or
runtime policy. Callers perform VFS I/O through their own host boundary and
exchange only explicit byte ranges, owned buffers, opaque font-face lifetimes,
and value-copy metrics with the media leaf.

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
points. `libretro_hw_render.c` negotiates the optional frontend graphics context and forwards its
lifecycle through the public graphics seam; it holds the one ABI-level record the adapter owns and
names no graphics API type. The other files in that directory translate libretro environment/VFS,
options, and input facilities into the public API. The linked core exports only
the official libretro surface.

Deleting `src/adapters/libretro/` and its entries in `Makefile.common` leaves a
buildable framework-neutral runtime library. No portable module includes
`libretro.h`.

The isolated-runtime acceptance check performs this cut in a temporary copy;
it does not rely only on the source list. See `PORTING.md` for the host-facing
contract and `SECURITY_MODEL.md` for the VFS trust boundary.
