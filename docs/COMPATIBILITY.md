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

## Supported matrix

| Input family | Structural selector | Parser path | Runtime path |
| --- | --- | --- | --- |
| Classic | container revisions 600, 701, 702, 800, 810 | `src/content/classic/` | shared engine with classic policies |
| Studio first generation | bytecode 14, 15, 16 | `src/content/datafile/` and `src/content/bytecode/` | shared engine with resolved policies |
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
ordering, classic presentation and interpolation, view slots, and related
format behavior.

## Where a difference belongs

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
