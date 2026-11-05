<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 retrodiv <retrodiv@proton.me> -->

# State and cache formats

Save states and caches are independent formats with independent schema
numbers. Neither number is part of a filename, type name, directory name, or
marketing version. It is a numeric field in a validated binary header.

## Save-state schema

Incremental device checkpoints do not change state bytes or capacity policy.
Retained frame-target staging names an explicit successful baseline; the device
may download only exact changed regions, but the resource publishes them only
after success and keeps a complete current CPU-canonical image before frontend
return. Baseline textures, change masks and staging versions are disposable
context/resource metadata, never serialized state. Cold or invalid baselines
use full reads. CPU writes and restoration cannot inherit stale baseline data;
failed partial reads leave the canonical image unpublished and invalid as a
current resource. See R03-P9-INCREMENTAL-CHECKPOINT in
[the renderer map](RENDERER_BACKENDS.md) for owners and controls.

The current AnyGM save-state schema is `39`. It is the format transported by
libretro frontends for manual save states, automatic state slots, and rewind
snapshots. Those features remain supported.

Schema `39` introduces VM schema `21`. Boxed object values retain their distinct
category through variables, arrays and builtin containers. Wire value tag `5`
stores their stable unsigned 32-bit VM handle, never a host pointer. Numeric
values retain their existing encoding and array-reference tag `4` remains
reserved for array identities. Older root and VM schemas reject transactionally
without a legacy reader.

Schema `38` introduces VM schema `20`. Every live structure record starts with a
one-byte layout tag. Tag zero uses the established full instance record. Tag one
stores the lifecycle flags and variable map, with the handle reconstructed from
the separately recorded slot and generation. It applies only when all omitted
instance fields have their exact zero bits, the object sentinel is negative one,
and the handle agrees with its allocator slot. Nonzero instance fields and signed
zero coordinates retain the full record. Ordinary instances, array identities,
free-list ordering and collection timing retain their existing rules. Older root
and VM schemas reject transactionally; no legacy reader is provided.

Schema `37` adds two little-endian doubles after the root audio accumulator:
presentation phase in `[0,1)`, then presentation Hz (zero for an unused clock,
otherwise finite and at least two). They retain the fractional relationship
between logical simulation steps and independent frontend presentation ticks.
The fixed addition is 16 bytes, including before the first frame. The most recent
host-accepted display request uses the existing VM global-value codec. VM schema
`19` and audio schema `3` retain their layouts. Native mode handles and frontend
callback acknowledgements remain host state, not emulated state. Older root
schemas and invalid clock values reject transactionally without legacy readers.

Schema `36` introduces audio schema `3`; VM schema `19` is unchanged. After
the established mixer parameters, audio states carry handle-ordered PCM queues:
format, source rate, channel mode, then buffer handles and bounded little-endian
signed PCM blocks. Voice positions retain fractional resampling progress. A
FIFO of undelivered playback notices preserves queue/buffer identities and the
shutdown flag. Queue PCM is limited to 16 MiB per mixer and all queued blocks,
reserved shutdown notices and pending events share a 65536-node limit. Queue
data is decoded in staging storage before publication; rejection leaves the
live mixer untouched. No host pointer or persisted external state is involved.
Older root/audio states reject without legacy readers.

Schema `35` introduces VM schema `19`. Live GML buffers are mutable runtime
memory, no longer discarded host I/O. After the buffer and asynchronous identity
cursors, a signed count precedes handle-ordered records: unsigned 32-bit ID,
signed 32-bit logical length and position, then exactly that many raw bytes.
Unused slots and allocation capacity are absent. Counts and lengths are bounded
by remaining payload bytes; IDs must be unique, positive and below the next
buffer ID; position must be within the logical buffer. Root restoration remains
transactional. Older root/VM states reject without a legacy reader.

`game_save_buffer` retains its destination handle as an empty buffer in the
embedded snapshot, preventing recursive inclusion of prior snapshot bytes.
Other buffers are saved normally; restoring that embedded snapshot replaces
the destination with its empty record. Ordinary frontend snapshots include
every live buffer's logical bytes. Fixed frontend capacity policy and compact
completed-frame handling remain unchanged: added live bytes have real memory
and copy cost, while merely growing the tables adds no reserved-slot bytes.

Schema `34` introduces compatibility schema `2`. The profile fingerprint now
includes early Studio background compositing. The automatic pass count and
packed-image blend policy affect the reconstructed image; older semantic
profiles reject transactionally. Payload layouts, VM schema `18`, and audio
schema `2` remain unchanged.

The renderer's authored-sprite boundary may legitimately be zero. In that case
every generated sprite is a runtime record, including ID zero: deletion, Reset
cleanup and state-load replacement must not reinterpret the current runtime
sprite count as an authored prefix. This is an ownership correction within the
existing renderer payload, not a change to its bytes or schema `34`.

Schema `33` introduces VM schema `18`. Legacy room background scale arrays now belong
to the room's canonical globals and persistent-room records. Fresh rooms initialize
both axes to one; state restoration retains the active and dormant rooms' scales.
The existing named-value codec carries these fields. Older states may instead contain
instance-local scale writes or omit the defaults, so they reject transactionally.
Renderer and audio layouts are unchanged.

Schema `32` appends base and auxiliary sampler filtering to the renderer payload, followed by
its GPU-state stack depth and the active stack records. Each filter is a validated little-endian
32-bit boolean. An empty stack adds nine words in total; each active entry adds fifteen words,
including its seven auxiliary filters. The public schema owns this renderer layout; VM schema
`17` and audio schema `2` are unchanged. Older states reject transactionally.

Schema `31` introduces VM schema `17`: physics body coordinates and the visual-origin offset
are canonical independently of instance coordinates. Per-fixture translation offsets are removed;
solver state remains reconstructible from the canonical body and attachment records.

Schema `30` introduces VM schema `16`. Physics fixture records append two doubles
for the local binding offset and two signed 32-bit words for sensor and collision
group before the point arrays. Joint records append five doubles (both local
anchors and the reference angle) and a signed 32-bit initialized flag before the
parameter array. Template and bound fixture handles remain distinct. Body rotation,
velocities, accumulated forces and mass overrides use the existing instance variable
codec. Native solver objects and caches are transient and rebuilt deterministically.
Earlier public and VM schemas reject transactionally. Renderer encoding and audio
schema `2` are unchanged.

Schema `29` introduces VM schema `15`. Two signed little-endian 32-bit words
prefix the existing builtin INI/data-structure section: the next buffer search
cursor (1 through 16), then the last issued asynchronous save/load request ID
(0 through INT_MAX). They retain the identities of future operations after a
restore. Exhausted asynchronous IDs return failure instead of overflowing.
Buffer contents, open host handles and pending completion queues remain transient;
these counters do not replay external I/O. Reset clears both cursors. The fixed
eight-byte addition is present before the first frame. Earlier public and VM
schemas reject transactionally, without a legacy reader. Renderer encoding and audio
schema `2` retain their layouts.

Schema `28` appends the language-visible asset name to each renderer runtime-sprite
record, after its collision mask. An unsigned little-endian 32-bit byte count includes
the terminating NUL; zero means a null name. Non-null names contain no earlier NUL
and at most 16 MiB of text. Generated, copied and file-backed identities survive both
pixel reconstruction and compressed-cache reuse, independently of source-path relocation.
The renderer has no independent schema header. VM schema `14` and audio schema `2`
retain their layouts. Earlier public schemas reject transactionally.

Schema `27` adds two fixed 256-byte arrays after the root keyboard snapshots:
the carried simulated-press event flags (zero or one), then deferred release
phases (zero through two). A press/release pair issued within one frame owes
input work to later frames, so restoring its held bit without these arrays
would latch the key and lose its event. The existing input poll consumes the
restored queues; Reset clears them. Invalid values and earlier public schemas
reject transactionally. VM schema `14`, renderer encoding and audio schema `2`
retain their layouts. The 512-byte addition is present before the first frame.

Schema `26` adds renderer runtime-sprite mode `2`: a little-endian unsigned
32-bit encoded length followed by a zlib-compressed RGBA plane. Dimensions and
frame count determine the exact decoded byte count. Compressed planes share a
256-MiB decoded budget per renderer payload. The writer retains raw mode `0`
for planes below 4 KiB, incompressible planes and planes exceeding the remaining
budget; file-backed mode `1` is unchanged. Compression uses the existing image
codec and is cached until the sprite's pixels change. Repeated size queries and
snapshots neither recompress nor read a mutable source file. A reader may reuse
live pixels only when the complete compressed bytes match its own canonical
cache and the sprite geometry matches. Other streams are decoded with an exact
output bound. Cached bytes are derived data, never a separate state dependency.
Previous public schemas reject transactionally. VM schema `14` and audio schema
`2` retain their byte layouts; the renderer has no independent schema header.

Schema `25` introduces VM schema `14`. Time-source records retain their field
order and widths, but parent `2` now identifies hidden, self-cleaning delayed
calls. They cannot be parents or targets of ordinary time-source operations.
Their remaining time, callback reference, repetition state and handle survive
restoration through the existing builtin-state stage. Hidden records have no
argument array, use after-deadline expiry and retain a positive period. The
reader rejects duplicate or non-advancing handles, invalid hidden records and
ordinary children of hidden sources inside the existing transaction. Older
public and VM schemas reject cleanly; audio schema `2` is unchanged.

Schema `24` introduces VM schema `13`. Runtime layer and element tables preserve
their slot extents instead of compacting surviving handles. Each slot starts with
a signed 32-bit used flag; an unused slot has no further fields. Used records
retain the preceding field order, with one additional signed 32-bit parent-order
word at the end of each layer. Shader globals therefore keep addressing the same
slots, and subsequent allocation reuses the same holes.

After the active layer/element tables, a signed 32-bit count introduces dormant
room visuals in strictly increasing room-index order. Each room has a signed
32-bit index, signed 64-bit nonnegative room-relative age, the same layer and
element tables, one shader-binding double per layer slot, and a signed 32-bit map
count followed by the same map metadata and sparse cells as the active room.
Global next-handle counters are written only once. Dormant rooms must be flagged
stored, belong to the loaded content and differ from the current room. Grids are
rebound by the existing room owner without executing room entry or mutating room
globals. Layer, element and map handles are checked for global uniqueness in
their respective namespaces before publication. Deleted map records remain
retained, and ordinary room recreation remains distinct from persistent return.
Previous public and VM schemas reject cleanly; audio schema `2` is unchanged.

Schema `23` introduced VM schema `12`. The tilemap section records every authored
map, including maps with no edited cells. A signed 32-bit map count and next-map
identity precede the records. Each record holds seven signed 32-bit words
(identity, used, visible, parent order, tileset, columns, rows), three doubles
(local x, local y, fallback depth), and a signed 32-bit sparse-cell count followed
by that many (cell index, unsigned datum) word pairs. Indices are strictly
increasing. Parent-layer position remains in its existing runtime-layer record.

The fixed metadata is present before any setter call. It does not grow when a
map moves or changes its tileset; edited cells retain their sparse representation.
Restoration rebinds grids from immutable room content, checks map count, order and
dimensions, and restores the original language handles and next identity. Counts,
remaining byte spans, duplicate identities, flags and finite coordinates are
validated before publication. Previous public and VM schemas reject without a
legacy reader. Audio schema `2` and root input transport retain their layouts.

Schema `22` adds one little-endian unsigned 32-bit mouse held-suppression mask
after the root keyboard snapshots and before the optional completed frame.
Only bits zero through two are valid, selecting left, right and middle buttons.
The mask records buttons cleared while held; clearing an already released button
only removes its transient edge observation and does not suppress its next press.
Raw mouse input and cleared edge observations remain poll-local, not state bytes.
After restoration, the mask survives while the host still holds the button and
is removed when the normalized pointer/mouse union releases it. The existing
first-advancing-poll continuity policy still prevents synthetic press edges.
Reset discards suppression. The reader rejects invalid masks transactionally;
previous public schemas reject without legacy readers. VM schema `11` and audio
schema `2` retain their existing byte layouts. The fixed four-byte addition is
present even before the first frame and does not introduce state-size growth.

Schema `21` introduces VM schema `11`. The VM payload ends with a signed 32-bit
object count followed by seven signed 32-bit words per loaded object: sprite,
mask, parent, depth, visible, solid and persistent. Its fixed extent is present
before the first frame and never grows when a creation default changes. Names,
event definitions and physics metadata remain immutable content, not state bytes.
The count must equal the loaded object count; the bounded reader validates the
complete parent forest before publishing any property, then rebuilds hierarchy
caches through their existing owner. Authored scalar words and negative root
sentinels are preserved. Previous public and VM schemas reject without a legacy
reader. A later section rejection uses the ordinary root transaction to restore
the previous object properties and hierarchy as well as the instance state.

Schema `20` retains VM schema `10` and extends each renderer font-pool record with a 32-bit
runtime-file flag. A live file font adds its portable source root/path, SHA-256, raster pixel
height, initial character range, 32-bit bold/italic booleans and ordered materialized character
list. Authored styles remain immutable content metadata; sprite fonts have neither style.
Paths are bounded to
4,095 bytes and the list to 65,536 unique BMP codepoints. Font-file reads retain their 32-MiB
bound. Glyph coordinates and pixels are rebuilt through the existing loader and lazy-glyph
operation; the file bytes and raster atlas are not repeated in every rewind snapshot. Deletion
and restoration reuse font-owned atlas slots, without changing authored texture pages.

A matching live font can be reused without reopening its source. Reconstruction requires the
current VFS file to match its saved digest; a missing or changed source rejects the state.
The root transaction retains an in-memory font checkpoint until every section and the
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
  The keyed map/list/grid resource records are ordered by public resource ID,
  independently of private allocator holes. Element order inside each container
  is preserved. Restore may compact the private pools without changing the bytes
  produced after an identical sequence of later allocations.
- Every variable section is length-delimited and must be consumed exactly.
- DS map/list/grid pools grow on demand and retain container addresses and public handles.
  Their live handle indexes are maintained in canonical order; snapshot traversal
  neither sorts them nor allocates capacity-sized scratch. Unused pool slots and
  reserved capacity are excluded from bytes. This storage change retains schema
  `34` and VM schema `18`, because existing record layouts and meaning are unchanged.
  Restore bounds counts against the remaining section before allocation and
  rejects duplicate or non-advancing handles within each pool. Runtime growth is
  bounded by allocation success and the positive signed handle namespace; the
  existing per-container state element and string bounds still apply.
  Grid records retain their ID, width, height and tagged-cell encoding. The live
  grid count is bounded by the remaining record bytes rather than a fixed private
  slot table. Grid identities must be positive, unique and below the restored
  allocation cursor. Individual grids retain the eight-million-cell limit.
- Content, compatibility, and stateful configuration fingerprints are distinct.
- Active content-override directives join the configuration fingerprint, so a state saved with
  them loads only while they are active. Content without directives keeps the configuration
  encoding it always had.
- Presentation-only settings such as virtual-monitor dimensions and aspect ratio are not
  state identity; the host's current presentation settings remain active when a state is loaded.
  If the stored completed frame has another extent, the load redraws once without advancing the
  simulation instead of publishing that stale extent and changing geometry on the following frame.
- CRT output adaptation is also excluded from state identity and bytes. The completed image stays
  at its content or forced-aspect extent; native-pixel padding and the filtered 640x480 host frame
  are derived output.
  Restoring a state applies the current CRT setting when presenting it, without changing the
  frontend's serialization capacity policy or adding another frame to the state.
- Diagnostics, host handles, and disposable caches are excluded.
- Graphics-target resources and host execution policies are excluded under every schema. Textures,
  programs, framebuffer identifiers, entry-point tables, upload buffers, residency records,
  render plans and timing queries are derived caches, while content-GLSL and hybrid-presentation
  selection belong to the host session. They are rebuilt or reapplied rather than restored, are
  absent from the configuration fingerprint, and a state saved under one graphics policy loads
  under another.

`anygm_state_size` returns the buffer requirement for the current complete canonical state.
It remains exact in CPU mode. With active scene GPU execution, it counts metadata-only ceilings:
raw completed-frame RGBA, at most one surface RLE run per pixel, and raw inline sprite pixels.
Neither complete nor frame-free queries materialise pixels, replay GPU work, read a device or
prepare pixel-compression caches. The bound is stable across pixel changes at fixed geometry;
language allocations and resource metadata may still change it. The actual writer retains the
existing lossless encodings and returns the canonical payload length in `written`, which can be
smaller than the GPU buffer requirement. Load and transactional rollback consume that length,
not uninitialised capacity bytes. State bytes and schema are unchanged by this sizing policy.
When a scene GPU host supplies a positive owning-context proof, the actual writer
first observes dirty numbered, application and owned SCREEN resources through
`gml_render_observe_device_resources`. Observation preserves the device RGBA,
even when a CPU replay recipe exists. Failed reads do not publish partial pixels
or discard that recipe; a retry can still observe the resident image. An absent
witness retains the established resource writer. A negative installed witness
rejects an unobserved resource before the renderer writer can replay it, including
frame-free states and restore rollback; already observed CPU images remain usable.
The observation
callback and device services are transport metadata, not serialized fields.
Witnessed scene passes retain pending persistent device images across Draw and frame end;
automatic all-resource seals remain for CPU scenes and hosts without the callback. An explicit
save observes the pending GPU version, not an earlier CPU plane. This changes observation timing,
not state bytes, capacity, schemas or transactional restore. CPU content readers retain their
resource barriers, and availability of the host's actual save-time context remains required.
An eligible completed scene presentation with positive context proof is copied
GPU-to-GPU into separate completed-frame storage before the frontend can reuse
its target. The actual complete writer observes those exact RGBA bytes through
`gml_gpu_read_completed_frame`; ordinary shader readback targets cannot overwrite
them. A failed read leaves the CPU frame unpublished and can retry. Successful
observation retires the pre-presentation SCREEN recipe instead of replaying it
over the observed pixels. Controlled current-context destruction observes both
persistent resources and this completed frame before deleting GPU storage.
An installed negative witness forbids graphics calls even after a controlled notice.
Destruction retries one transient observation failure. Persistent failure reports
loss and invalidates only still-unobserved volatile pixels instead of publishing
CPU recipes as actual GPU bytes; valid CPU images and saved states remain intact.
No host handle, snapshot generation or context callback enters the state layout.
Unmaterialised device-only completed pixels cannot be saved with a temporarily
unbound context; rejection is not substituted CPU reconstruction. Hosts without
context proof retain the old path. Qualification of frontend access transitions
and removal of ordinary checkpoints remains separate work. Genuine scene-context
loss discards device-only volatile user surfaces and unmaterialised completed
images instead of replaying them. Current CPU images and valid saved bytes are
not invalidated; a saved image restores exactly onto a replacement context.
Managed application storage is recreated separately. This lifecycle policy
changes neither the state layout nor the default CPU recovery behavior.
`anygm_state_resume_size` returns the corresponding frame-free buffer requirement, and
`anygm_state_save_for_resume` writes that form explicitly. Repeated serialization through the
same form without an intervening mutation produces identical bytes.

## VM payload ownership

`src/core/engine_state.c` owns the root framing and section transaction.
Within the VM section, `src/runtime/vm/gml_vm_state.c` alone owns schema `21`,
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
Transient host file handles, asynchronous request queues,
search cursors, and derived caches are reset rather than serialized. INI
values, live binary buffers, maps/lists/grids, physics resources, spatial-audio state, external-audio source identities,
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
the current buffer requirement and writes into a caller-owned buffer.

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

The root writer may stop counting completed-picture runs when both the raw encoding and the
already-proven row-RLE lower bound exceed the offered buffer's remaining capacity. It then uses
the same frame-free writer; it does not assume that another picture of the same dimensions will
also overflow. Large repeated pictures retain their complete encoding whenever it fits. Exact
CPU size queries still count the full selected encoding; scene GPU queries use the metadata
ceiling described above. If the compact attempt fails too, the
original complete-plus-compact attempts are replayed to retain failed-save size and buffer
behavior. This changes neither state bytes nor slot capacity, materialization, schema or restore
semantics. `tests/integration/test_graphics_state.c::completed_frame_capacity_case` compares raw,
row-RLE and adjacent-row repetition at exact, near-fit, compact and failed capacities against
the ordinary complete and explicit resume bytes, with canaries and unchanged live-state checks.

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
render and language-level tables populated by gameplay. References to grid/list/map creation or
explicit array allocation/resizing select a 32-MiB ordinary reserve: deferred container
initialization can exceed the four-MiB reserve before
the first gameplay snapshot. These are conservative reference flags and a selected growth policy,
not a proof that arbitrary language allocations fit. Both established compact-raster policies
remain unchanged. The compact large-frame path instead
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
