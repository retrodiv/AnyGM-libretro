<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 retrodiv <retrodiv@proton.me> -->

# State and cache formats

Save states and caches are independent formats with independent schema
numbers. Neither number is part of a filename, type name, directory name, or
marketing version. It is a numeric field in a validated binary header.

## Save-state schema

The current AnyGM save-state schema is `20`. It is the format transported by
libretro frontends for manual save states, automatic state slots, and rewind
snapshots. Those features remain supported.

Schema `20` retains VM schema `10` and extends each renderer font-pool record with a 32-bit
runtime-file flag. A live file font adds its portable source root/path, SHA-256, raster pixel
height, initial character range and ordered materialized character list. Paths are bounded to
4,095 bytes and the list to 65,536 unique BMP codepoints. Font-file reads retain their 32-MiB
bound. Glyph coordinates and pixels are rebuilt through the existing loader and lazy-glyph
operation; the file bytes and raster atlas are not repeated in every rewind snapshot. Deletion
and restoration reuse font-owned atlas slots, without changing authored texture pages.

A matching live font can be reused without reopening its source. Reconstruction requires the
current VFS file to match its saved digest; a missing or changed source rejects the state.
The root transaction must retain an in-memory font checkpoint until every section and the
reapply allocation have accepted the candidate: rollback cannot depend on reopening a file
after its previous face has been destroyed. The checkpoint copies mutable glyph/atlas storage
and retains the immutable raster face; it is not part of the wire format or save-size queries.
Previous public schemas reject cleanly, with no legacy reader.

Schema `19` introduced VM schema `10`: each edited or runtime-created path retains its defining
controls as well as its existing execution samples and deleted-resource flag. Each defining point
is three doubles (x, y, speed), preceded by a signed 32-bit count. The sampled geometry remains
byte-exact across restoration rather than being refitted. Counts are bounded to 4,194,304 points
per representation and 65,536 resource slots. Names are immutable resource identity: authored
names are re-read from content and runtime names follow the append-only creation ordinal, so no
mutable name or separate naming counter is serialized. An instance's optional path block adds one
relative-mode byte; a resource translation moves a relative follower's pivot with the controls.
Deleted IDs are not reused. Previous root and VM schemas reject cleanly.

Schema `18` carried VM schema `9`: arrays retain identity across every VM value root, including
builtin containers, static variables, arguments and parked frames. The first encounter defines an
array and implicitly assigns the next one-based ID before its elements. Wire value tag `4` carries
a little-endian array ID; zero denotes a null array and every other reference must already exist.
This preserves shared and cyclic graphs without merging distinct equal arrays. Definitions are
bounded to one million arrays and 64 nested definitions; back references do not add depth. A graph
beyond the bounds makes saving fail, never silently replaces a value. Sparse indices are strictly
increasing. Identity tables are operation-local scratch, not serialized addresses. Previous public
and VM schemas are rejected without a legacy reader.

Schema `17` binds state configuration to the effective content override program and the
launch-anchor envelope governing later internal content replacements. Defaults overridden by
another layer are excluded. Root framing and embedded codecs retain their layouts; earlier
schemas are rejected cleanly because they do not establish this configuration contract.

Schema `16` selects the smaller of two lossless completed-frame encodings. Repeated-row RLE remains
the compact form for integer-scaled canvases and letterboxes, while a frame whose run stream would
be larger is stored as raw little-endian RGBA. The latter avoids scanning and expanding almost one
run per pixel for detailed authored rasters. The encoding selector changes the root section layout,
so schema `15` states are rejected cleanly rather than interpreted through a legacy reader.

Schema `15` carries the runtime motion-planning state: the motion-planning grids a run created,
and every path it added or edited. Both come into existence after content load, and an instance's
`path_index` names one of them, so a state without them restores a run that has forgotten where
anything walks. A room that builds its grid from placed blockers commonly destroys the instance
that did it, which leaves nothing to rebuild the grid afterwards; the state is the only thing that
can put it back. Authored paths are not written - the content holds them, and restore re-reads the
authored table before applying the records above it, so a path the run had edited is authored again
when the state says it was untouched.

Schema `14` includes the independently authored collision plane of a runtime sprite in the
renderer payload. Sprite assignment and duplication copy that plane separately from visible RGBA,
so restore must retain both or precise collisions change after save, rewind, or run-ahead.

Schema `13` widens the serialized pad state from one row to one per supported pad port. The held
and previous button rows are what edge detection resumes from, so a state written when a single row
existed describes a different shape and is refused rather than read as the first player's. Nothing
else in the section moves, and no legacy reader is added.

Schema `12` prefixes the root core section with two fixed-size, zero-padded launch fields: the
portable path of the active internal replacement relative to the launched distribution, and its
launch parameters. An empty path names the frontend-selected payload. When a state names another
internal payload, restore prepares and validates that content in a separate engine transaction,
loads the complete state there, and replaces the running content only after every section accepts
it. Absolute machine paths are never serialized. Fixed field capacities keep this metadata from
growing when content changes, including for a frontend whose rewind slot size was fixed before the
first frame. Schema `11` carries no new section. The completed-frame section remains optional: ordinary save
states carry it so the first frontend frame after a load is exact, while a host may deliberately
write the frame-free form for a high-frequency in-memory resume point. Both forms use the same
schema and restore the same canonical post-frame simulation state. Schema `11` marks the
compatibility profile growing the policy that
decides what GML reads for an instance's far bounding-box edges, which every state's compatibility
fingerprint covers: a state written before it describes a run under a different reading of that
policy, and is refused rather than resumed under this one. Schema `10` re-encodes the completed frame as a row table plus run-length encoded literal rows,
so the repetition an upscaled presentation canvas manufactures - integer scales repeat whole rows,
letterboxes repeat black ones - no longer reaches the stored bytes, and the slot costs about the
source raster whatever the monitor. It also carries the struct allocator's free list in its exact
order, because the slot an allocation mints becomes part of every stored handle, and restores
input-edge continuity: the first advancing frame after a load treats a key held now as held, not
newly pressed. Schema `9` added a run-length encoded copy of the completed application frame to
the root section. The first frontend frame after a load presents those pixels without running game
code; this keeps transient Draw output exact while simulation resumes from the canonical
post-frame state on the following call. Schema `8` adds the logical paths and SHA-256 identities of loaded portable
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
public schema and every affected embedded schema, and reject earlier states
without legacy readers. A refactor does not require a bump when the
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
- Presentation-only settings such as virtual-monitor dimensions and aspect ratio are not
  state identity; the host's current presentation settings remain active when a state is loaded.
  If the stored completed frame has another extent, the load redraws once without advancing the
  simulation instead of publishing that stale extent and changing geometry on the following frame.
- Diagnostics, host handles, and disposable caches are excluded.
- Graphics-target resources and host execution policies are excluded under every schema. Textures,
  programs, framebuffer identifiers, entry-point tables, upload buffers, residency records,
  render plans and timing queries are derived caches, while content-GLSL and hybrid-presentation
  selection belong to the host session. They are rebuilt or reapplied rather than restored, are
  absent from the configuration fingerprint, and a state saved under one graphics policy loads
  under another.

`anygm_state_size` returns the exact size of the current complete canonical state.
`anygm_state_resume_size` returns its frame-free size, and
`anygm_state_save_for_resume` writes that form explicitly. Repeated serialization through the
same form without an intervening mutation produces identical bytes.

## VM payload ownership

`src/core/engine_state.c` owns the root framing and section transaction.
Within the VM section, `src/runtime/vm/gml_vm_state.c` alone owns schema `10`,
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
frontend acknowledges it. Requesting the quirk is not sufficient: a declined or unsupported
request has the baseline fixed-size contract. Its first answer covers the remembered frame-free
peak and mutable render allocations already known at load. When the conservative completed-frame
ceiling is modest it is covered too. When that ceiling would dominate every rewind slot, the first
answer instead selects one compact capacity that remains fixed for the loaded session. Later
queries return it without traversing the runtime state again. Every save at that declared capacity
first attempts the complete encoded picture and falls back to the explicit frame-free form only
when the picture does not fit. The representation cannot depend on whether the caller is a rewind
push or a manual save because the libretro ABI does not identify those consumers.

A frontend that explicitly acknowledges variable sizes uses the same compact pre-frame policy.
A one-MiB ring is also sufficient when the complete-frame ceiling plus twice the known required
state fits within it. This keeps the bounded raw-picture cost separate from the simulation growth
allowance instead of doubling both and then applying the ordinary four-MiB floor. A missing frame
ceiling, a larger remembered required peak, or a larger mutable render allocation does not qualify.
Content that references surface creation/resizing, dynamic source execution or payload replacement
retains the ordinary
reserve even if no surface exists at load. Those references make the cold allocation count an
incomplete estimate of later render storage. The complete-first writer retains the exact encoded
picture whenever it fits; later geometry
growth may use the frame-free form in the same slot. This is an advisory reserve, not a proof that
arbitrary future language allocations are bounded by the cold state.

Otherwise the compact path begins when omitting the conservative completed-frame ceiling saves at
least the ordinary four-MiB session reserve. Moderate pictures remain in the ordinary capacity,
retaining its larger allowance for required simulation state that grows after a cold load. A compact
startup capacity is one MiB or larger and covers the predicted required state. Later ordinary size
queries grow monotonically to the conservative complete-state capacity,
so newly sized saves retain the exact completed picture while older compact ring slots remain
loadable.

An ordinary complete-capacity session reserves at least 4 MiB because a cold load can precede the
render and language-level tables populated by gameplay. The compact large-frame path instead
reserves at least 1 MiB; its frame-free capacity hint describes the startup ring while that separate
floor covers bounded cold-to-gameplay growth. This adapter capacity policy changes neither
the logical size in the canonical header nor the portable state format. Explicit portable hosts
may still request the frame-free resume representation directly.

`make contract-check` exercises acknowledged growth, fixed declined and unsupported frontends,
compact startup rings, exact-when-fitting and frame-free fallback representations, Reset under all
three negotiations, unload reset, exact roundtrips, and the transport blocks used by
rewind-capable frontends. The graphics-state
integration case separately proves that an explicit frame-free state roundtrips and resumes while
the complete form remains available.

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
