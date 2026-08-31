<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 retrodiv <retrodiv@proton.me> -->

# Opt-in VM diagnostics

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

## Hybrid GPU counters

`GML_HYBRID_GPU_STATS` reports one bounded line when the graphics context is released or content is
unloaded:

```text
[hybrid-gpu] frames=N accepted=N replayed=N transport=N draws=N uploads=N bytes=N resets=N
             destroys=N losses=N program_failures=N fallback_unsupported=N fallback_box=N
             fallback_context=N fallback_upload=N fallback_overflow=N fallback_shader=N
             materializations=N screen_passes=N canvas_passes=N readback_passes=N
             readback_ms_per_frame=N readback_pipelined=N readback_refused=N last_error="..."
```

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
six do not. `readback_pipelined` is 1 once the measured policy stopped waiting for the device and began taking
the previous pass's answer instead — correct, one frame late, and it is tried before a draw is
given up. The `anygm_content_shader_readback` option selects the policy — measured, always
(exact, waits), or never. The terminal presentation is not affected by any of it.

`accepted` counts passes the device executed and `replayed` counts passes the software executor had
to take back. `transport` is the subset of accepted passes that merely carried a complete software
frame to the device, which is not acceleration: a session whose `accepted` equals its `transport`
never accelerated anything, and a diagnostic that blurred the two would report it as a success.
`bytes` distinguishes the two just as plainly — an accelerated presentation uploads its small
source, while transport uploads the whole host-sized frame. `screen_passes` and `canvas_passes`
separate the two accelerated shapes, because they remove different work: the first is the frame's
last operation performed on the device so the processor's copy is never written, and the second is
the fit of a completed frame into a larger host framebuffer. `materializations` counts the times
something needed the processor's copy after all and it had to be rebuilt.

A normal build pays nothing for this when the setting is absent: the counters are increments at
pass granularity and no line is formatted.
