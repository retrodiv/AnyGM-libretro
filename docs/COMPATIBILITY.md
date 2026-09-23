<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 retrodiv <retrodiv@proton.me> -->

# Compatibility architecture

## One engine, several input generations

AnyGM does not contain a GM8 engine, a Studio 1 engine, and a Studio 2 engine.
It contains one VM, one renderer, one mixer, and one frame lifecycle. External
formats differ in two controlled places:

1. Readers decode their representation into the shared content model.
2. The compatibility resolver converts structural facts into named runtime
   policies.

Everything after those seams is shared unless a reviewed policy expresses a
real semantic difference.

## Normalized gamepad identity

The normalized gamepad interface exposes logical slots, digital buttons and stick
axes, not a physical-device GUID. `gamepad_get_guid` returns the documented string
`none` within the interval exposed by `gamepad_get_device_count`, and
`device index out of range` outside it. A connected logical slot does not imply
that hardware identity is available. Missing and nonfinite indices use the latter
result as a defensive policy; negative fractional indices are also rejected.
The existing description and input mapping are unchanged. This does not add raw-hat
or analogue-button-pressure support. The contract is documented in the
[publisher manual](https://manual.gamemaker.io/lts/en/GameMaker_Language/GML_Reference/Game_Input/GamePad_Input/gamepad_get_guid.htm)
and covered by ordinary, exact cached and prefix-cached dispatch controls.

## Supported matrix

| Input family | Structural selector | Parser path | Runtime path |
| --- | --- | --- | --- |
| Classic | container revisions 600, 701, 702, 800, 810 | `src/content/classic/` | shared engine with classic policies |
| Studio first generation | bytecode 13, 14, 15, 16 | `src/content/datafile/` and `src/content/bytecode/` | shared engine with resolved policies |
| Studio second generation | bytecode 17 | same normalized content model, with the revision-specific reader | shared engine with modern function, struct, and layer policies |

Bytecode 16 reuses the compatible normalized reader path where its serialized
layout permits it, while still receiving its own semantic policy decisions.
Parser reuse is not evidence that two revisions have identical runtime
semantics.

## Resolution flow

```text
raw headers and container metadata
              |
              v
       generation-specific parser
              |
              v
        shared GmlWin model
              |
              v
   AnygmContentFacts (structural only)
              |
              v
 immutable AnygmCompatibilityProfile
              |
              v
 named policy accessors used by shared runtime code
```

The current profile centralizes comparison epsilon, alarm dispatch and trigger
threshold, live versus frame-snapshot instance iteration, solid-collision
coordinates, blend behavior, function/struct/layer semantics, animation
timing, path velocity ownership, transformed collision bounds, creation-event
ordering, frame retention without a background clear, classic presentation and
interpolation, view slots, reported bounding-box far edges, and related format
behavior.

Frame retention is also independently resolved. Studio generations retain the
completed previous frame when a room requests neither the background color nor
the view clear. Classic generations clear the drawing target every frame and use
the room fields only to decide what is painted over that clear, so
half-transparent drawing cannot accumulate towards opacity across frames.

The reported far edges of an instance's bounding box are independently resolved. A recognized later-format marker selects one-past far edges for modern inputs unless the legacy collision option is set. Inputs without that structural marker retain the stored inclusive reading. The collision engine always keeps inclusive bounds; this policy changes only the values exposed to the language.

Alarm dispatch is an independently resolved policy: classic inputs and Studio
bytecode 16 run each alarm subtype by ascending exact object resource and then
by instance insertion order within that resource. Earlier first-generation
Studio inputs and second-generation Studio inputs retain flat instance order.

Automatic motion applies friction before displacement. Live-iteration
generations also include instances created earlier in the frame in that motion
phase before their first draw, while frame-snapshot generations defer those new
instances until the following frame.

## Where a difference belongs

Early Studio background compositing has a narrow policy for bytecode 13 without
room layers. Independent native controls show two successive source-over passes
for each untiled automatic background backed by a content atlas, whether behind
or in front of instances. Horizontal or vertical tiling, runtime-created images,
and explicit background draws use one pass. A black alpha-63 texel over white gives
145 through the automatic path and 192 through the explicit path; at draw opacity
0.5 those values are 197 and 224. Point-sampled content backgrounds truncate draw
opacity to an eight-bit vertex value, round the sampled coverage to a byte, and
round source and destination colour products separately. An opaque 191/200/194
texel over black at opacity 0.5 gives 95/100/97 from content and 96/100/97 from a
runtime-created image. Overlapping red and blue automatic slots give 36/0/146,
which distinguishes repetition of each background from two whole-list passes.
The existing fixed-function blend implements this arithmetic. Controls cover scales one through eight and an
enlarged packed texel. These are measured early-runner results. Keeping later
encodings, filtered sampling, and the separate stretch branch unchanged is a conservative policy
boundary, not a claim that their native behavior has been measured.

Use this decision order:

1. If bytes are laid out differently but mean the same thing, change only the
   relevant reader and normalize the result.
2. If execution semantics differ for a structural generation or option, add a
   named field to `AnygmCompatibilityProfile`, resolve it once, and consume it
   at the smallest shared runtime branch.
3. If behavior is configurable for every generation, use public configuration
   rather than a format policy.
4. If a difference cannot be described from format facts or explicit
   configuration, it is not a compatibility rule and must not enter the core.

A small `if` is appropriate when it consumes a named policy next to the
operation whose semantics differ. A raw expression such as `bytecode >= 17` in
a VM or renderer function is not appropriate: it duplicates classification,
hides intent, and makes future revisions unsafe.

## Adding a revision

To add support for another revision:

1. Extend detection and the narrow parser path, preserving the shared model.
2. Add focused parser fixtures for layout differences.
3. Extend the compatibility resolver and its table-driven unit tests.
4. Add a named policy only for demonstrated semantic divergence.
5. Run the architecture check so raw revision branching does not leak into the
   runtime.
6. Run the same engine-instance, state, renderer, and audio tests; do not create
   a generation-specific engine suite unless the input fixture itself differs.

The profile schema and fingerprint are serialized as part of compatibility
identity. A state produced under one resolved semantic profile is rejected if
loaded under another.
