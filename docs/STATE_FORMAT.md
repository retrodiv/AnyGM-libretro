<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 retrodiv <retrodiv@proton.me> -->

# State and cache formats

Save states and caches are independent formats with independent schema
numbers. Neither number is part of a filename, type name, directory name, or
marketing version. It is a numeric field in a validated binary header.

## Save-state schema

The current AnyGM save-state schema is `2`. It is the format transported by
libretro frontends for manual save states, automatic state slots, and rewind
snapshots. Those features remain supported.

Schema `2` starts a new compatibility line and includes the runtime GUI transform. No reader for
an earlier internal layout exists. The canonical header records magic, schema, header size, binary
encoding, total and section sizes, content identity, compatibility identity,
stateful configuration identity, and a payload checksum.

Loading validates the complete framing and checksum before invoking section
readers. Section readers validate their own counts and references. If any
reader rejects the input after decoding begins, the engine restores an exact
snapshot of its prior state before returning an error.

If a future release deliberately breaks state compatibility, increment the
schema to `3`, then `4`, and so on. Supporting an older schema requires an
explicit compatibility reader. A refactor does not require a bump when the
canonical bytes and semantics remain unchanged.

## Canonical encoding

- Integers are written little-endian at explicit widths.
- Floating-point values use their IEEE 754 bit representation at an explicit
  width.
- Native structs, padding, pointers, `long`, and `size_t` are never serialized.
- Maps and other unordered collections are written in deterministic order.
- Every variable section is length-delimited and must be consumed exactly.
- Content, compatibility, and stateful configuration fingerprints are distinct.
- Presentation-only settings such as output resolution, aspect ratio, and CRT effects are not
  state identity; the host's current presentation settings remain active when a state is loaded.
- Diagnostics, host handles, and disposable caches are excluded.

`anygm_state_size` returns the exact size of the current canonical state.
Repeated serialization without an intervening mutation produces identical
bytes.

## Variable state size

The state can grow as language-level containers grow. The portable API reports
the exact current size and writes into a caller-owned buffer.

The libretro adapter advertises the variable-size serialization quirk and records whether the
frontend acknowledges it. Its transport size is nevertheless a conservative session-stable
capacity: rewind implementations allocate fixed ring slots from an early size query, so allowing
that value to grow would silently stop later snapshots. This capacity policy does not change the
exact logical size stored in the canonical header or the portable state format.

`make contract-check` exercises acknowledged and unacknowledged frontends, changing state sizes,
unload reset, exact roundtrips, and the full stable-capacity blocks used by rewind-capable frontends.

## Cache schema

The generated-content cache schema is independently `1`. Equal state and cache
numbers are coincidental. A cache header records its own magic, schema,
producer recipe identity, source identity, output identity, and bounded
metadata.

A missing, truncated, corrupt, or incompatible cache is ignored and rebuilt
from the original source. A temporary file is published with one atomic rename
only after it is complete. Failed publication leaves the previous marker
untouched. Cache rejection never makes otherwise valid source content
unsupported.

A breaking cache-layout change increments only the cache schema. A state
schema change does not invalidate cache data unless the cached representation
also changed.

## Review rules

- Validate sizes, counts, and arithmetic before allocation or pointer access.
- Treat every incoming byte as untrusted.
- Do not alter the resolved compatibility profile while loading state.
- Keep state failure transactional and cache failure disposable.
- Extend deterministic corruption tests whenever a section is added.
- Keep frontend-specific capacity policy out of the portable serializer.
