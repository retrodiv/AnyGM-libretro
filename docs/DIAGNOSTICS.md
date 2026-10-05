<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 retrodiv <retrodiv@proton.me> -->

# Opt-in VM diagnostics

## Resolved draw dispatch accounting

`gml_render_dispatch_diagnostics_enable` opts one renderer into transient, metadata-only
accounting. `gml_render_dispatch_metrics` does not flush work, read pixels or invoke a
device callback. Diagnostics are disabled by default and never enter canonical state.

The engine's `GML_RENDER_DISPATCH_STATS` development setting pins accounting for one complete
content load, before Create executes. Reset, authored restart and content replacement retire
the old renderer's completed counters without resetting session totals. A private trial boot
for transactional state restoration neither publishes a second footer nor copies an earlier
diagnostic history into the replacement. On acceptance, the live session's retired history is
transferred once; restoring state never restores counters. A new content load starts a new
session. Unload emits `[graphics-dispatch]` and seven bounded
`[graphics-dispatch-family]` lines after the existing renderer teardown completes queued work.
This report exists even in CPU-only builds or when no GPU scene was admitted.

Completed scene submissions are counted by their resolved family. Queued work is not counted
until successful submission; CPU recipe execution is a fallback, not GPU work. Failed batches
and unclassified families remain separate disqualifiers. Saturating counters flag overflow
instead of wrapping to zero. The existing device partial-prefix report continues to describe
physical writes from an unsuccessful batch.

Actual retained-history and nested source-recipe execution reports `cpu_replay` after the
existing replay executor returns. Reconstruction is counted even when the original submission
ran on the device. Newly rejected operations are credited by their submitting facade, not twice
by the replay helper. An opaque borrowed renderer owner is rebound when a runtime moves;
immutable recipes never retain that pointer. Repeated CPU-current reads add no drawing.

All six engine-owned coarse GPU plan execution edges report their returned result, including
mid-frame content programs, terminal programs/readback, nearest presentation, host-canvas
composition and CPU-frame transport. Clears are primitives, image passes are surfaces and
content programs are effects. Reporting reads bounded operation metadata only. A CPU-frame
transport remains transport, not proof of scene acceleration; its preceding CPU draws remain
in the ordinary CPU counters and the existing device report names transport separately.

Software primitive scopes include complete CPU-only passes and every subsequent CPU draw.
The first covered span or pixel marks the operation; the owner counts it once after any row
workers join. Clipped, zero-alpha and degenerate no-ops remain uncounted. The pre-barrier reason
is retained, rather than relabelling the first fallback as merely an already inactive scene.
Successful software surface copies and classic subpixel glyphs are also counted; observations,
empty copies and fully transparent glyphs are not drawings.
Normal, filtered, mirrored and rotated software surface kernels mark their actual writes,
including parallel row bands. Layer filters and the recognized two-sample post-process group
their software fragments into one effect operation; noise and tint retain their no-op guards.

Software-3D triangle, line and point kernels mark surviving color writes through a borrowed,
renderer-owned atomic coverage flag. Depth-tested draws use the separate
`exceptions[family][GML_RENDER_DISPATCH_EXCEPTION_DEPTH]` counter; a merely active 3D pipeline
with hidden-surface testing disabled remains an ordinary CPU fallback. Procedural particle
shapes, including auxiliary interpolation planes, count once per covered particle and are not
exempted by a 3D-active flag. A particle square routed through the software-3D bridge instead
uses its actual triangle/depth classification. Diagnostics introduce no pixel observations or
additional allocation sites.

Runtime RGBA sprite kernels now mark surviving nearest, axis-scaled, filtered, rotated,
region and positioned/general-color writes. Their scopes close only after row workers finish;
nested region helpers do not count the same sprite twice. Axis-cache build/hit copies count as
draws, not as GPU execution; source-cache construction itself is not a target draw. Existing
alpha quantization, clipping, shader discard and zero-coverage guards still exclude no-ops.
Destination-dependent and subtract fragments that actually write with zero alpha remain counted.

Synchronous atlas point-sampled, filtered, stretched and cardinal-tile kernels likewise mark
actual target writes, including the warmed filtered draw cache and real row workers. Source-cache
construction alone is not counted. Background and tile-part entry points retain their resolved
families through nested sprite helpers. Classic auxiliary sampling planes belong to the same
draw, while replacement writes to authored transparent margins remain counted.

Rotated atlas execution retains the existing queue and painter order. Each queued operation owns
an atomic coverage flag in the existing queue allocation. Worker contexts borrow those flags only
after the queue stops growing, and the owner records each covered operation after all workers
join. Empty/discarded operations, enqueue and repeated empty flushes add nothing. Family and
pre-barrier reason are copied as values from the enclosing dispatch; a pending underlay temporarily
suspends and restores those values. A flush inside another draw does not merge its operations
with that draw. Commands queued before diagnostics were enabled remain unclassified if executed
under accounting, rather than silently qualifying a partially observed route. Immediate filtered,
special-blend and classic auxiliary-plane draws retain their own scopes and actual no-op guards.

Deferred clears count at CPU materialization, not at recording or cancellation. Immediate
classic auxiliary-plane clear writes count independently of a still-deferred base clear.
Recorded CPU presentations count their optional clear and actual image copy separately, even
when a later ordinary draw triggers the flush. Discarded records, empty copies and repeated
flushes add nothing. A non-deferred automatic CPU underlay also keeps its independent draw
scope when materialized inside the following draw's preparation. Valid pooled CPU host-canvas
plans count each executed operation after its
existing kernel completes; a plan rejected before execution adds no completed draw. Source-free
scene clears are classified as primitives without changing retained operation or state bytes.

Shared ordinary atlas, runtime, rotated, filtered and distance-field glyphs retain the TEXT
family at their actual CPU leaf. String measurement and unpainted sprite-font spaces add no draw.
Successful depth bridges keep their own software-3D classification, outside these ordinary scopes.
Classic repeated backgrounds and public/room tiles retain BACKGROUND/TILE instead of inheriting
the sprite helper's family. Each actually covered skeletal attachment is counted independently;
a handled return without color writes is not completed drawing.
Position-dependent text programs retain the CPU glyph gathering and final composition as
separate executed stages. Cached shader source construction is not another draw; a rejected
host shader still counts the existing plain composition. These stages do not imply completed
GPU execution merely because a host shader callback ran.

Engine-owned view copies/scales/crops, multiview seeding, GUI copies/scales, presentation
shifts, narrow-room black spans, owned application clears and classic transition blending
report `cpu_composition` after their existing nonempty writes. Failed/empty/clipped copies,
inactive transitions and unchanged shifts add nothing. Native cached information pages count
one TEXT copy; the software page counts its primitive page/row backgrounds, surviving
underlines and actual glyph stages.
Building the native page cache or capturing an immutable transition source is not a target draw.

The standalone renderer snapshot always reports `complete = 0`: it cannot certify that
accounting started before content executed. The engine session report supplies `complete = 1`
only for a pinned, enabled load with at least one recorded renderer epoch and no unknown work,
accounting gap or overflow. Temporarily disabling an enabled renderer leaves a sticky unknown
marker. Failed execution remains a separate disqualifier, even when coverage is complete.
A zero ordinary CPU total qualifies only with complete route execution, a valid session footer,
no failures/unknowns/overflow and a witnessed active GPU producer on every selected route;
device-only transport totals cannot substitute for this check. Pixel queries, source/cache
construction, immutable captures and state decoding are not drawings. Neither a 3D-active
flag nor a serialization call implicitly exempts an ordinary raster draw. Driver qualification
and complete route selection/provenance remain measurement prerequisites, not counter values.

Focused controls: `make check TEST=renderer_primitives` and `make check TEST=render_resources`.
The primitive controls compare diagnostics on/off pixels, metadata-only state bytes, nested
scopes and coverage marks from four actual row-band slots. Surface-copy controls include
clipping, empty input, observation and overlapping self-copy.
Also run `make check TEST=renderer_surfaces`, `make check TEST=renderer_effects` and
`make check TEST=renderer_postprocess` for visible, discarded, clipped and no-op composition.
`make check TEST=d3_state D3_TEST_ARGS='--case boundary.dispatch_accounting'` compares
diagnostics on/off for triangle/line/point draws, fourteen procedural particle shapes and
active-3D particles with and without depth testing, including clipped and alpha-zero no-ops.
`make check TEST=render_plan` also checks deferred/cancelled clears, auxiliary-plane clear
writes, discarded/repeated/nested presentation flushes, fill-only presentation and rejected plans,
with complete diagnostics on/off pixel comparisons for presentation and host-plan execution.
The effect controls also compare twenty-five complete RGBA texture diagnostics-off/on pairs,
including actual row workers and warmed axis-cache copies, and twenty-five atlas pairs covering
family attribution, discarded draws, auxiliary planes, stretched and warmed filtered-cache
execution. They retain all prior effect hashes.
Twenty rotated-queue pairs check cached/exact/mask, vector/scalar and real-worker execution;
forty-command queue growth; nested flushes; family/reason retention across underlay preparation;
late diagnostic enable; discarded coverage; and immediate classic/filtered/blend paths.
The font controls add twenty-four diagnostics-off/on glyph pairs, including metadata-only
measurement, actual synthetic shader callbacks/cache/refusal and complete base/phase RGBA.
The tile controls add eighteen public/background/room/repeated-cell pairs. Nine skeletal pairs
distinguish actual attachment writes from clipped, transparent and missing-source handled returns.
The skeletal alpha-test-state control observes the existing writer; it does not change its policy
or assert original-runner correctness.
Engine controls add fourteen presentation pairs, five narrow-room pairs and three complete
load/session pairs with full target and canonical-state comparisons. They cover prepared
accounting, host-setting pinning, Reset, authored restart, content change, accepted state-runtime
replacement, retired totals, footer publication, reloading the same engine and accounting gaps.
Eight information page pairs compare software/underline/native/cached/refused/invalid/empty/resized
execution. Five resource pairs cover retained history, CPU fallback, nested recipes, repeated
reads and app/base/numbered owner rebinding, with complete pixels and forbidden device callbacks.
Plan controls
also cover completion metadata, malformed/failed reports and saturating epoch accumulation;
these bookkeeping controls are not qualification of a real graphics driver.

## Fine VM trace

The fine VM trace is development instrumentation, not release functionality.
It is absent from a normal build, including its parser, strings, state pointer,
and hot-loop calls. Build it explicitly:

```sh
make -j1 DIAGNOSTICS=1 BUILD_DIR=build/diagnostics
make diagnostics-check
```

`DEBUG=1` controls compiler optimization and debug symbols independently. It
does not enable the trace. Conversely, `DIAGNOSTICS=1` does not require
unoptimized code or debug symbols.

The diagnostic core reads filters through the ordinary host development
settings and writes one JSON object per host log callback. It never opens a
trace file, changes the public API, or changes canonical state. A host may
capture or route its own log stream.

## Required settings

Tracing is disabled unless `GML_DIAGNOSTICS` is present. Its value is a
comma-separated list with no spaces, empty tokens, or duplicates. The
permitted tokens are:

- `opcode`: each interpreted bytecode operation;
- `event`: each event handler that is about to be dispatched;
- `collision`: each handler-capable collision pair after bounding-box and
  mask evaluation;
- `variable`: selected scalar and array writes performed by the interpreter.

Every enabled trace requires both of these settings:

- `GML_DIAGNOSTICS_FRAMES=FIRST:LAST`, an inclusive nonnegative decimal frame
  range;
- `GML_DIAGNOSTICS_LIMIT=COUNT`, an integer from 1 through 1,000,000.

The limit is the maximum number of JSON records. The implementation reserves
the last available record for a `limit` marker when another matching event
would overflow the stream. It then disables the trace for the rest of the
content session. Thus a limit of one can emit only the marker, while a limit
of 1,000 can emit at most 999 data records plus the marker.

Malformed or incomplete settings emit one `config_error` JSON record and
disable this trace. They do not fail content loading or alter execution.

## Narrow selectors

Selectors are intersected: when several are supplied, a record must satisfy
all applicable selectors.

- `GML_DIAGNOSTICS_CODE=TEXT` selects code entries containing `TEXT`. It is
  mandatory for `opcode` and optional for the other trace kinds.
- `GML_DIAGNOSTICS_OBJECT=NAME` selects an exact object name.
- `GML_DIAGNOSTICS_INSTANCE=ID` selects an exact unsigned decimal instance
  ID. `global` is accepted instead of an ID only for `variable`.
- `GML_DIAGNOSTICS_VARIABLE=NAME` selects one exact variable name and is
  mandatory for `variable`.

`event` and `collision` require an object or numeric instance selector.
`variable` requires a variable selector plus an object, numeric instance, or
`global` selector. Selector text is limited to 127 printable ASCII bytes.
These requirements deliberately prevent an accidental unfiltered
per-opcode, per-pair, or whole-variable trace.

Examples:

```sh
GML_DIAGNOSTICS=opcode \
GML_DIAGNOSTICS_FRAMES=240:245 \
GML_DIAGNOSTICS_LIMIT=2000 \
GML_DIAGNOSTICS_CODE=movement \
frontend diagnostic_core content

GML_DIAGNOSTICS=event,collision \
GML_DIAGNOSTICS_FRAMES=600:620 \
GML_DIAGNOSTICS_LIMIT=500 \
GML_DIAGNOSTICS_INSTANCE=100137 \
frontend diagnostic_core content

GML_DIAGNOSTICS=variable \
GML_DIAGNOSTICS_FRAMES=30:90 \
GML_DIAGNOSTICS_LIMIT=128 \
GML_DIAGNOSTICS_INSTANCE=global \
GML_DIAGNOSTICS_VARIABLE=phase \
frontend diagnostic_core content
```

## Record contract

Every line has `"schema":"anygm.vm.trace"`, `"version":1`, a zero-based
`sequence`, a `kind`, and the current simulation `frame`. Resource text is
JSON-escaped and bounded. Real values use exact IEEE-754 bits instead of
locale-sensitive decimal text. String values are bounded and carry a
`truncated` flag; array writes report their logical length.

Opcode records include code, byte offset, mnemonic, stack depth, the numeric
top-of-stack value, and current self ID. Event records include instance,
object, event, and resolved code.
Collision records include both instances and objects, the mask-qualified hit
result, and resolved handler code. Variable records include scope, target,
name, optional array index, current code, and the written value.

The variable hook covers interpreter-owned scalar and array stores. Mutations
performed wholly inside an opaque builtin-owned container are outside the VM
variable trace and remain the responsibility of that subsystem's focused
diagnostics.

## Route coverage

`GML_LOG_OBJ=TEXT` lists active instances whose object names contain `TEXT`
after each frame (`*` selects every object). With the additional setting
`GML_LOG_OBJ_DEACTIVATED=1`, the same read-only listing includes temporarily
deactivated instances. It does not activate them, execute their events or
change canonical state. This exposes retained positions outside an active
view without confusing temporary deactivation with destruction. Consumers
that request it must interpret object counts as including those instances.

`GML_LOG_COVERAGE=1` reports, once when content is unloaded, how much of the payload's own code a
run ever entered:

```text
[coverage] code=<entered>/<total> objects=<entered>/<total>
```

`code` counts CODE entries the VM began executing, each once however often it ran; `objects` counts
objects at least one of whose event entries did. It answers the question a room count cannot: a
route may enter several rooms while executing only a small fraction of its code entries.

Kept apart from `GML_PROFILE_CODE`, which is beside it in the same structure and answers a different
question. That one accumulates wall time and reads a clock twice per call, so it cannot be left on;
this stores one byte per invocation when enabled. It does not change frame output.

Off unless asked for, and it allocates nothing until the first entry runs.

## Shader accounting

`GML_SHADER_ACCOUNT` reports, once when content is unloaded, one line per content program that was
drawn with:

```text
[shader-account] shader=N executed=N plain=N recognized=N procedural=N device=N surface=N/N sprite=N/N ...
```

`executed` counts draws that carried the device's answer — including those composed from a kept
shaded frame, which carried it as much as the draw that produced it — and `plain` counts draws that
fell back to the software renderer. The per-shape figures say which draw shapes those were, so a
gap names itself. A shader whose `recognized` is 1 belongs to a family implemented in
software; that flag alone does not establish output fidelity. State-setting builtins
whose names begin with `draw_` are not counted as draws.

## Graphics-device counters

`GML_GRAPHICS_DEVICE_STATS` reports the bounded render-interval lines when the graphics context is
released or content is unloaded. On release these precede the required persistence observation,
which has a separate delta line. It covers the shared context used by the independent content-GLSL
and hybrid presentation policies:

```text
[graphics-device] frames=N accepted=N replayed=N transport=N draws=N uploads=N bytes=N resets=N
             destroys=N losses=N program_failures=N fallback_unsupported=N fallback_box=N
             fallback_context=N fallback_upload=N fallback_overflow=N fallback_shader=N
             materializations=N screen_passes=N canvas_passes=N readback_passes=N
             readback_ms_per_frame=N readback_pipelined=N readback_refused=N last_error="..."
[graphics-scene] offered=N accepted=N refused=N partial_prefix=N texture=N primitive=N
             surface=N effect=N presentation=N shader=N software3d=N uploads=N
             upload_bytes=N readbacks=N readback_bytes=N
[graphics-scene-families] sprite=N background=N tile=N surface=N text=N unclassified=N
[graphics-release-observation] context_current=N access_witness=N attempts=N observed=N
             elapsed_ns=N timing_available=N materializations=N scene_reads=N scene_read_bytes=N
             checkpoint_masks=N checkpoint_mask_bytes=N rgb_queries=N rgb_query_refusals=N
             completed_reads=N completed_read_bytes=N completed_copies=N completed_copy_bytes=N
             scene_uploads=N scene_upload_bytes=N screen_cpu_reads=N screen_cpu_materializations=N
```

The scene line counts the resident scene service separately from hybrid plan transport.
The release line records only persistence-and-detach deltas before device destruction, not a
frame or object-deletion time. Add its observations to the preceding render report for whole-context
totals; no counters are cleared. `observed=1` means an attempted witnessed preservation succeeded;
zero attempts do not prove that volatile GPU images survived a lost/unavailable context. Explicit
state/content observations before release remain in the render report and are not reclassified.
Operation-family counts include only complete accepted batches; `partial_prefix` records how many
operations a failed batch reported as already accepted and is never treated as completed work.
The `text` scene family counts accepted packaged sprite-font glyph quads, not layout calls or
glyphs painted later by the ordered CPU fallback. Older logs without `text` predate this counter.
Transfer counts and bytes include only successful explicit scene-service uploads/readbacks and use
the transferred region's area, not its enclosing texture's area. A scene line of all zeros means
the host did not submit resident scene work, even if the older hybrid counters are nonzero.

`fallback_shader` counts passes refused because the content's own program did not compile or
link on the device; the first refusal retires that program for the session, so the count stays at
the number of frames that reached the device before the retirement took effect. `last_error` is
the backend's most recent diagnostic, verbatim, and empty when nothing failed. `readback_passes`
counts content programs executed off-screen in the middle of a frame and read back for the
software renderer to compose; each is one device round trip. `readback_ms_per_frame` is what
those passes cost the frames that paid for them, and `readback_refused` is 1 when that average was
over the per-frame budget, after which such draws proceed plain for the session; the refusal is
also logged once, with the measured cost, as a warning. The budget is per frame rather than per
pass because what a frame can afford is a frame's time: three passes of 2.5 milliseconds fit and
six do not. `readback_pipelined` is 1 while the measured policy is taking the previous pass's answer rather
than waiting for the device — correct, one frame late, and what the measured policy does from its
first pass, because waiting is most of what a read-back costs and none of what it produces.
`Game shaders (GLSL)` presents these policies as Performance (measured, pipelined), Exact (waits), and
its two OFF reporting variants. Terminal GLSL is exact in both active modes; the OFF variants do
not execute it.

`accepted` counts passes the device executed and `replayed` counts passes the software executor had
to take back. `transport` is the subset of accepted passes that merely carried a complete software
frame to the device, which is not acceleration: a session whose `accepted` equals its `transport`
never accelerated anything, and a diagnostic that blurred the two would report it as a success.
`bytes` distinguishes the two just as plainly — an accelerated presentation uploads its small
source, while transport uploads the whole host-sized frame. `screen_passes` includes terminal GLSL
and eligible hybrid presentation; `canvas_passes` is the hybrid fit of a completed frame into a
larger host framebuffer. `materializations` counts the times something needed the processor's copy
after all and it had to be rebuilt. A content-GLSL-only session can therefore have device shader
passes and transport without enabling generic hybrid final-pass acceleration.

A normal build pays nothing for this when the setting is absent: the counters are increments at
pass granularity and no line is formatted.
