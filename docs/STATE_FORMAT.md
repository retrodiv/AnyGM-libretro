<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 retrodiv <retrodiv@proton.me> -->

# State and cache formats

Save states and caches are independent formats with independent schema
numbers. Neither number is part of a filename, type name, directory name, or
marketing version. It is a numeric field in a validated binary header.

## Save-state schema

The current AnyGM save-state schema is `8`. It is the format transported by
libretro frontends for manual save states, automatic state slots, and rewind
snapshots. Those features remain supported.

Schema `8` adds the logical paths and SHA-256 identities of loaded portable
Wwise banks to the canonical VM audio stage. Schema `7` adds portable external-audio asset identities plus the Saudio string-ID registry to the
canonical VM audio stage. Every loose dynamic sound records its logical source path and dynamically
computed source SHA-256; Saudio additionally records the extension-visible string ID. Schema
`6` preserves the frame-redraw state needed after restore. Schema `4` preserves the semantic
distinction between ordinary nested arrays and indexed
two-dimensional arrays in the canonical VM payload. No reader for an earlier internal layout
exists. The canonical header records
magic, schema, header size, binary
encoding, total and section sizes, content identity, compatibility identity,
stateful configuration identity, and a payload checksum.

Loading validates the complete framing and checksum before invoking section
readers. Section readers validate their own counts and references. If any
reader rejects the input after decoding begins, the engine restores an exact
snapshot of its prior state before returning an error.

If a future release deliberately breaks state compatibility, increment the
schema to `9`, then `10`, and so on. Supporting an older schema requires an
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
- Active content-override directives join the configuration fingerprint, so a state saved with
  them loads only while they are active. Content without directives keeps the configuration
  encoding it always had.
- Presentation-only settings such as output resolution, aspect ratio, and CRT effects are not
  state identity; the host's current presentation settings remain active when a state is loaded.
- Diagnostics, host handles, and disposable caches are excluded.

`anygm_state_size` returns the exact size of the current canonical state.
Repeated serialization without an intervening mutation produces identical
bytes.

## VM payload ownership

`src/core/engine_state.c` owns the root framing and section transaction.
Within the VM section, `src/runtime/vm/gml_vm_state.c` alone owns schema `5`,
field order, value-graph encoding, sizing, and restore scratch.

Builtin resources have a separate storage and lifetime owner, not a separate
format. `src/runtime/builtins/gml_builtin_state.c` owns INI and data-structure
storage, physics fixtures and joints, spatial-audio emitters/listener/falloff,
and language-visible time sources. It reads and writes those resources at the
four positions established by the VM schema. The VM state owner passes
only opaque cursors declared by `gml_vm_state_codec.h`; only those two
translation units may consume that boundary.

There is no nested builtin header, checksum, schema number, or independent load
transaction. Moving the four existing resource stages to their storage owner
therefore adds no independent state framing or schema.
Transient host file handles, binary buffers, asynchronous request queues,
search cursors, and derived caches are reset rather than serialized. INI
values, maps/lists/grids, physics resources, spatial-audio state, external-audio source identities,
Saudio IDs, loaded Wwise-bank identities, and time sources remain serialized.

External-audio states do not embed potentially large MP3/OGG/WAV files. If a dynamic handle loaded
through Saudio, SGAudio, SuperSound, or `caster_*` has been closed, restoration reads its bounded
logical VFS path and recreates that exact handle only when the current file matches the SHA-256
stored by the state. A missing or changed file rejects the state and the root state transaction
restores the previous live engine.

Wwise bank state follows the same rule: it stores bounded logical paths and
SHA-256 identities, not bank or media bytes. Restore reparses each bank through
the host VFS and rejects the complete state transaction if a bank is absent or
has changed.

`make check TEST=builtin_state` constructs every resource family, proves reset
and transient exclusion, restores from a fresh VM, and requires the second
serialization to match the first byte for byte.

## Variable state size

The state can grow as language-level containers grow. The portable API reports
the exact current size and writes into a caller-owned buffer.

The libretro adapter advertises the variable-size serialization quirk and records whether the
frontend acknowledges it. Its transport size is a conservative capacity that remains stable until
the logical state exceeds it. An acknowledging frontend can then query a larger monotonic capacity
and retry. For a frontend that does not acknowledge variable states, the initial fallback capacity
remains session-stable so fixed-slot rewind storage cannot silently change size. This capacity
policy does not change the exact logical size stored in the canonical header or the portable state
format.

`make contract-check` exercises acknowledged and unacknowledged frontends, capacity growth and
fixed-capacity rejection, unload reset, exact roundtrips, and the full transport blocks used by
rewind-capable frontends.

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
