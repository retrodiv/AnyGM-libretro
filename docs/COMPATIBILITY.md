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

## Array-copy selection

The resolved profile distinguishes Classic independent value arrays, Studio
copy-on-write arrays and later reference arrays. For modern bytecode, the OPTN
copy-on-write bit or a decoded `setowner` operation selects copy-on-write;
without either, reference semantics apply. The decoder walks merged CODE spans
once, including embedded functions, and never treats an opcode-shaped integer
operand as an operation. Earlier Studio bytecode retains copy-on-write policy.
The policy is part of the compatibility fingerprint transported by save states.

Source projects read the boolean `option_copy_on_write_enabled` from their main
options resource (`Options` or a `GMMainOptions` resource). If it is not declared,
the reader also checks `options/main/options_main.yy`. A missing file or setting
retains the existing transport-derived default; it does not infer an editor release
from a project format version. Declared missing files, malformed documents,
nonboolean values and duplicate settings fail loading.

An explicit setting is preserved in the first-party normalized `ACPY` chunk: an
eight-byte little-endian body with version 1 and mode 1 (copy on write) or 2
(reference). This selects only array copying, independently of the compiler's
bytecode 15 transport. Absent settings emit no chunk. Other sizes, versions or
modes, duplicate chunks, Classic provenance, or reference mode combined with
native COW flags/owner operations are rejected. The resolved policy already enters
the state compatibility fingerprint; this input fact adds no state bytes.

## Instance-copy ownership

`instance_copy` copies direct methods bound to the source into new method wrappers
bound to the new identity, preserving aliases between those direct fields. It retains
ordinary struct/array references and methods bound elsewhere. This selects the
publisher's [verified method correction](https://github.com/YoYoGames/GameMaker-Bugs/issues/10494),
rather than reproducing that issue's earlier release bug. Nested methods inside a
shared array or struct are not rebound by mutating that shared object.
Source member calls retain their accessor receiver beneath the callable, with
arguments below both values. Otherwise unresolved named calls check the current
instance's callable fields on every invocation, without caching a method receiver.

The [beta instance-copy contract](https://manual.gamemaker.io/beta/en/GameMaker_Language/GML_Reference/Asset_Management/Instances/instance_copy.htm)
specifies a nonphysical copy without source physics values. The shared physics owner
suppresses automatic object-template materialization for that copy; an explicit
fixture binding can initialize a new body later. Other instance values and unrecognized
user keys retain ordinary copying. This is a selected documented policy, not a
measurement across all native runner releases; the monthly manual omits the physics note.

Layer membership uses the copied layer order and the existing layer owner. Element
enumeration allocates a distinct element identity for the new instance; it never
copies a source element's runtime handle or cached instance pointer.

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

## Legacy music controls

The shared audio owner supports one legacy music voice independently of ordinary effects,
including effects playing the same sound resource. The early Studio profile exposes the legacy
music controls; later Studio profiles retain obsolete play/stop/gain no-ops. Classic source
carriers can exercise these controls as a runtime compatibility extension, without claiming
historical editor API exposure. Music volume retains a pre-play value, and its linear sample-clock
fade continues across silence and replacement while global audio pause freezes it. These lifetime
choices are selected compatibility behavior, not a claim of measured native-runner equivalence.
Music voice identity and fade progress participate in the current transactional audio state.

## Emitter pitch and velocity

Emitter voices retain independent asset, voice and emitter pitch multipliers, with a default voice
multiplier of one. Emitter pitch applies to active and future voices, and changing the asset
multiplier retains each emitter voice's own multiplier.
Velocity changes the source's radial Doppler factor; position changes remain explicit, and the
single listener's velocity defaults to zero. Raw velocity units use a propagation scale of 343.3
and Doppler factor 1. These scales and the positive saturation policy are selected compatibility
behavior, not a native-runner measurement. Coincident source/listener positions retain a unit
Doppler factor. The resulting emitter-source pitch is bounded to 1/256..256 for PCM and
1/256..4 for compressed sources. Ordinary voices retain their established pitch behavior.
Freeing an emitter stops its attached voices through the existing stop path, so reusing its
identifier does not retune old sounds. Emitter and derived voice state participate in the current
transactional format; this does not add the separate Audio Playback Ended callback API.

## Extended blend queries

The extended blend getters return independently owned arrays of RGB source/destination
factors, followed by alpha source/destination factors in the four-element form. The matching
setters accept either scalar factors or those arrays. Existing preset raster bodies remain;
other valid tuples use the shared CPU factor executor, including during ordinary counted
fallback in the OpenGL fallback renderer. Device execution of those tuples remains unqualified.
State push/pop and the current save-state format retain the active tuple and equations.
Invalid, nonintegral or nonfinite factors and short arrays leave the selected mode unchanged;
this invalid-input policy is not a measured native-runner exception contract.

## Gamepad button threshold configuration

`gamepad_get_button_threshold` reads the selected logical device's configuration,
initially `0.5`. The matching setter retains independent values in `0..1`; Reset
restores defaults and current save states retain all sixteen slots. Out-of-range
finite thresholds clamp; missing/nonfinite setter arguments leave state unchanged.
Invalid getter slots return the default. These invalid-input policies are not a
measured native-runner exception contract. The normalized frontend currently supplies
digital button values, so this configuration adds no analogue button transport and
does not change the existing digital held/pressed/released or raw-value paths.
