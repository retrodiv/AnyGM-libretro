<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 retrodiv <retrodiv@proton.me> -->

# Security model

## Scope

AnyGM parses and executes untrusted content inside the host process. It is not
a sandbox. The goal of this model is memory-safe rejection, bounded resource
use at parser boundaries, deterministic behavior, and confinement to the VFS
namespaces and capabilities explicitly supplied by the host.

The host remains responsible for process isolation, operating-system access
control, scheduling limits, and deciding which files a VFS path can name.

## Trust boundaries

The following inputs are untrusted:

- content images, chunks, bytecode, strings, records, and resource counts;
- classic containers, source projects, JSON, generated packages, and archives;
- state buffers and disposable cache files;
- paths, locale data, clocks, entropy, and other values returned by host
  services;
- runtime override expressions and public configuration values.

The compiled core, its immutable tables, and the `AnygmHostServices` function
table after ABI validation are trusted code. Individual callback operations
may still fail and every failure must propagate without switching to an
ambient process service.

## Content and archive limits

ZIP-compatible and embedded-Cabinet routing enforce these compile-time limits:

| Resource | Limit |
| --- | ---: |
| Container bytes read into memory | 1 GiB |
| Native executable scanned for embedded content | 1 GiB |
| Central-directory entries | 32,768 |
| Normalized member path | 511 bytes |
| One extracted member | 1 GiB |
| Total extracted bytes per level | 4 GiB |
| Folder/archive expansion ratio after a 16 MiB allowance | 1,000:1 |
| Nested archive levels | 4 |
| Anchor (`.anygm`) file bytes | 64 KiB |
| Runtime-override text within an anchor | 4 KiB |
| External transform configuration | 512 KiB |
| Recognized content-configuration sections | 256 |
| Collected override layers before resolution | 32 KiB |
| Effective overrides / launch-anchor envelope | 4 KiB / 64 directives each |
| One transform source function | 64 KiB |
| Transform syntax nesting | 64 levels |
| Source-candidate records | 64 records, 80 bytes each |
| Transform caller metadata | 256 read-only bytes per execution |
| All effective external patch files | 1 GiB combined per preparation |
| One patch result | 1 GiB, exact size declared by configuration |
| VCDIFF target window / auxiliary decoder memory | 64 MiB / 256 MiB |
| Embedded-Cabinet cache marker serialization budget | 20 MiB |

External content transforms compile bounded C-like source into buffer programs with no host capabilities. Their format,
selection, memory limits and proportional instruction budget are documented in
`CONTENT_TRANSFORMS.md`. A work bound is not a wall-clock deadline; a frontend accepting untrusted
content and programs should still enforce process resource and time limits.

Named VCDIFF patches bind only explicitly declared files beneath the original source directory,
using the same member-name policy as anchors and archives. The router alone reads those files
through the host VFS; memory-only loading has no patch-resource root. Every effective patch file
is SHA-256 checked before input preparation. Every applied stage verifies its current source and
exact result identity, so wrong bases or ordering reject without publishing intermediate bytes.
Decoder allocations, including LZMA, share a per-call auxiliary budget. The configuration owner
shares only immutable bound resources between copies; it serializes no patch bytes or pointers.
These are in-memory data operations, not native execution or a filesystem capability for the
buffer interpreter. `CONTENT_TRANSFORMS.md` owns the complete declaration and lifecycle contract.

Member paths are normalized before selection. Absolute paths, drive or stream
syntax, empty and dot segments, parent traversal, embedded control bytes,
ASCII case-folded duplicates, encrypted members, and Unix symbolic links are not
extracted. Local-header bounds, method, compressed and uncompressed sizes, and
CRC are validated before a member is accepted. Unsupported ZIP64 input is
rejected.

An `.anygm` anchor is parsed from a bounded file; its payload reference passes
the same member-path normalization, may not name another anchor, and is
resolved only against the anchor's own directory. Anchor override directives
are bounded in count and per-line length, validated fail-closed against the
override grammar before any live engine state is torn down, and confer no
authority beyond what the loaded content's own code already has. An archive's
anchor is staged into the disposable extraction cache and reparsed as
untrusted input on every load. A present anchor that fails any of these
checks rejects its archive; a directly loaded anchor that fails them loads
nothing.

The normalized FORM reader enforces a separate set of compile-time limits:

| Resource | Limit |
| --- | ---: |
| Normalized image bytes | 2 GiB |
| Chunks | 40 |
| Strings | 8,388,608 |
| Bytes in one string | 16 MiB |
| Code entries | 1,048,576 |
| Reference occurrences | 16,777,216 |
| Rooms | 1,048,576 |
| Room-order entries | 1,048,576 |
| Embedded classic information | 8 MiB |
| Texture pages returned by one named-group query | 65,536 |

The reader validates the exact FORM extent, unique and complete chunks,
record tables, string termination, code spans, room records, room order, and
reference chains before returning a live object. Failed parsing releases all
partial indexes and leaves ownership of the supplied memory with the caller.
A PE launcher without an internal payload may select only the exact adjacent regular `data.win`,
after its PE envelope is structurally bounded. The sibling must have `FORM` magic and is
parsed as ordinary untrusted Studio content through the host VFS; no native code is run.
The embedded-Cabinet route accepts a signature only inside a bounded PE raw section and only after
validating its complete header extent, folder and file tables, and CFDATA block bounds. Overlapping
PE section ranges are merged before searching, so no executable byte is searched more than once.
Each file range must be contiguous, non-overlapping, and covered by its folder's declared expanded
extent. The supported single-volume LZX-21 profile is then read through callbacks whose logical
zero and EOF are the validated Cabinet range; reads are at most 64 KiB and cannot reach adjacent PE
bytes. Multipart, continuation, and other compression profiles are classified before extraction.
Classic readers consume normalized structural representations. Optional configured source and
record operations return bounded owned buffers; they do not modify the supplied image or confer
host capabilities. The readers validate the complete returned structure. Archive entries,
padding counts, compressed lengths, settings blobs, resource counts and integrity words remain
subject to their existing checks before acceptance.
Runtime bytecode decoding uses a bounded entry point. It retains the direct
decoder when the maximum operand window is available and uses a zero-padded
local window only at an input boundary, so the normal cached decode path does
not gain a per-instruction copy.

Project, bytecode, image, audio, and state readers validate their own sizes and
counts before allocation or pointer arithmetic. A new variable-length field
must introduce a matching limit and a synthetic boundary test in its owning
module.

Compressed runtime-sprite state planes share a 256-MiB decoded-byte budget per
renderer section. Every dimensions/frame product is checked before allocation;
encoded lengths must fit the remaining section and be smaller than the decoded
plane. The bounded existing zlib decoder must produce exactly that plane's size.
Raw planes remain bounded by their complete bytes in the section. This budget
is a selected resource policy, not an observed format limit; larger live planes
remain writable through the raw representation.

Runtime-sprite state names use the content reader's 16-MiB per-string limit, plus
one terminating NUL. The complete byte span and termination are checked before
allocation; embedded NUL bytes reject. Names confer no VFS access and remain distinct
from the separately confined source path. Whole-engine rejection restores their
previous identities along with the other renderer state.

Runtime-font state records limit paths to 4,095 bytes, pixel height to 4 through 256,
and materialized glyphs to 65,536 unique BMP codepoints. Their initial range is bounded to
the same character domain. Reconstruction hashes the same bounded VFS read passed to the
existing font parser, before accepting its face, and never trusts state-provided font bytes.
The ordinary runtime-font atlas remains bounded to 128 MiB. An in-memory rollback checkpoint
retains immutable faces and copies mutable glyph/atlas storage, independently of source-file
availability; it does not enlarge serialized states.

Classic binary-extension library and symbol names are limited to 4,096 bytes before their
self-describing aliases are created or decoded. Portable external audio reads at most 64 MiB per
loose asset. External-audio state records limit logical paths (and Saudio IDs) to 4,096 bytes and
cap the dynamic sound table at 65,536 entries. Every restored handle is bound to the SHA-256
computed from its original bytes; rehydration rejects a missing, oversized, malformed, or changed
VFS asset.

Portable Wwise parsing accepts at most 16 loaded banks, reads at most 256 MiB
per bank, and caps both embedded-media and hierarchy-object tables at 65,536
records. Every chunk, media slice, hierarchy object, and recursive event walk
is bounded before use. Savestates retain only bank paths and SHA-256 identities;
changed or missing banks reject restoration transactionally.

FSB5 sample-header and metadata chains are bounded by their declared header
region before any field is read. The declared name and data regions must fit the
containing buffer or bank chunk, each subsound offset must fit the data region,
and malformed input is rejected before it can publish a sample descriptor.

Classic fidelity companions are untrusted inputs. Their fixed header, project
generation, exact byte length, SHA-256, record counts, resource identities,
compressed and expanded lengths, duplicates, and trailing bytes are validated
before restored data is published. Adjacent extension packages use independent
size/count limits and rollback any aliases appended before a later malformed
record. Externally referenced Classic included files use the same 1 GiB per-file bound and their
complete paths, lengths, and contents contribute to the cache dependency hash. Neither format may
select behavior by title or content hash.

## VFS confinement

All production file access crosses `AnygmHostServices`. A path is a name in the
host's VFS namespace, not permission to open the process filesystem. Content,
cache, and save roots are passed explicitly in `AnygmContentSource`; the core
does not infer sibling repositories, a home directory, or a process working
directory.

Writable content files are confined below the explicit save root in
`anygm/<sanitized-label>-<path-hash>/`. The human-readable label contains only
ASCII letters, digits, dots, hyphens, and underscores. It is descriptive only;
the path hash keeps separate source identities from colliding by label alone.

The portable nsfs adapter applies an additional content/save-root check before
using the ordinary overlay resolver. Absolute names outside those roots and any
parent-traversal component are rejected. Recursive directory copy/delete is
bounded to 32 levels and 32,768 entries; copy additionally accepts at most 4 GiB
of regular-file data. It cannot change the host process working directory.

The optional file-mapping callbacks use that same namespace and borrow only
immutable bytes. The runtime validates mapped size and content exactly as it
does VFS-read bytes, retains the opaque host handle while the view is live, and
returns it exactly once. A host that cannot map a path returns no handle and
the runtime falls back to its explicit VFS callbacks.

Archive extraction joins only validated relative member paths below the
explicit cache root. Cache markers are written completely to a temporary path
and published with an atomic same-filesystem rename. The host should expose
separate read and write capabilities where stronger confinement is required.
Missing callbacks fail explicitly and never activate the test-only stdio VFS.

Cabinet extraction additionally rejects native executable members, every non-regular entry,
hardlinks, symlinks, special files, and exact or ASCII case-folded path collisions. Its cache key
uses a streamed source-content hash. Warm reuse verifies schema and producer, source size/hash,
Cabinet range/profile, selected payload, and the complete relative-path/size/content-hash manifest;
missing, extra, stale, or corrupt files force regeneration. Members and the marker are flushed in
an unpublished staging directory before one directory rename publishes them. Write, flush,
decompression, marker, or rename failure removes staging and cannot publish a valid cache.
Cold extraction, marker parsing, and warm tree validation share one folded-name index with two
slots per accepted entry and a ceiling of 64 probes for every insertion or lookup. An input whose
collisions exceed that ceiling is rejected, so predictable hash collisions cannot recover an
all-pairs manifest scan. Duplicate rejection remains ASCII case-folded while warm-cache lookup
still requires the exact serialized spelling and case.
Their shared member-path validator also rejects components ending in a dot or space and ASCII
case-insensitive DOS device basenames, with or without an extension. This keeps cold writes, marker
identity, and warm tree verification consistent on Windows-backed VFS implementations.
The marker writer and reader share the serialization budget above; an over-budget manifest is
rejected before publication, so every successfully published marker remains representable to the
warm verifier.

## State and cache

State and cache use different magic values and independent schema fields. A
state load validates header encoding, exact section arithmetic, total size,
checksum, content fingerprint, compatibility fingerprint, and stateful config
fingerprint before section decoding. A failure during decoding restores an
exact pre-load snapshot.

The root input section accepts only three bits in its unsigned mouse
held-suppression word. Invalid bits reject the state, including when the
outer payload checksum is otherwise valid.

The VM state reader exposes an opaque bounded cursor to the builtin resource
owner at four established field positions. That owner validates INI,
map/list/grid, physics, emitter, external-audio handle/path/hash, Saudio ID, and time-source counts
and dimensions before allocation or indexed access. It cannot change the root framing, advance
outside the VM section, or publish an independently restored object. Decoding
occurs in the engine's scratch transaction, so any resource-stage failure
discards the candidate and preserves the prior live engine exactly.
The list/queue/stack pool accepts at most 1,024 simultaneously live containers; this remains a
state-reader bound as well as a runtime resource bound.
Queue/stack text codecs accept at most 64 MiB of decoded bytes, one million
value nodes (including synthesized legacy rows), and 64 value-tree levels.
Writers apply the same budgets and reject cycles. Readers validate the exact
extent and stage all decoded values before replacing a live sequence. Unsupported
records, non-exact integer conversions and embedded NUL string bytes reject
without changing the destination; old resource values retain their established
escaped-reference lifetime policy. This external text format is not a savestate.
Paths have at most 65,536 resource slots and 4,194,304 points in either their retained control
array or sampled polyline. Smoothing checks the expanded sample count before allocation.
Runtime edits publish only after the new controls and finite sampled geometry are ready;
state records check counts against their remaining byte span before allocating either array.

Object-property state has exactly one seven-word record per loaded object. The
reader checks that count against both the existing content table and its remaining
byte span before allocating scratch. Parent edges must stay within that table or
be negative root sentinels, and a linear forest validation rejects self references
and longer cycles before publishing any object properties. Derived event,
collision and family caches are rebuilt by the instance owner after publication.

Tilemap state has at most 512 records, each with a fixed 56-byte metadata prefix
and ordered eight-byte cell deltas. Each grid dimension is at most 8,192. The
reader checks the remaining byte extent before allocating metadata or delta
arrays, rejects duplicate handles and repeated or unordered cells, and binds
counts, dimensions and parent order to the room's reconstructed maps. Used and
visible flags are boolean; local positions and fallback depths must be finite.
The next handle is nonnegative and strictly greater than every retained handle.
Any rejection remains inside the existing root transaction.

Dormant visual state is bounded by the serialized room-flag count and loaded
room table. Room indices are strictly increasing, cannot name the active room,
and must have a stored flag. Each room uses the same bounded map codec and
immutable-content binding as the active room. Layer and element slot extents
are limited to 4,096 and 1,000,000 respectively and checked against remaining
used-marker bytes before allocation; each used marker is zero or one. Dormant
shader arrays have exactly the layer-slot extent and finite, nonnegative
encoded bindings no larger than one beyond the signed integer maximum.
Room-relative age must fit a nonnegative host long. Global handle validation
sorts bounded operation-local scratch and rejects duplicate or non-advancing
identities across active and dormant rooms. Failed reconstruction releases
all dormant owned grids, arrays and shader storage through the room owner.

Cache data is never authoritative. A schema, producer, source, output, size,
or checksum mismatch discards the cache marker and regenerates from the source.
No reader for an earlier unpublished state or cache layout is present.

## Runtime overrides

Runtime overrides are bounded by the public slot count and fixed parser storage.
An enabled slot requires a nonempty expression. Parsing never invokes a shell,
loads a module, accesses the network, or opens a file. Invalid or truncated
expressions fail within the slot and cannot change the compatibility profile.

## Deliberately unavailable operations

Content paths do not invoke a shell or child process, execute extracted
payloads, load dynamic plugins, or make network requests. Network-related
language operations receive deterministic unavailable behavior through the
runtime. Native rich-text rendering, fonts, rumble, clocks, and similar
facilities are explicit optional host callbacks.

## Failure guarantees

- Failed `load` from an empty engine leaves it empty and destroyable.
- Failed state load leaves the prior serialized state unchanged.
- Failed cache validation leaves original content authoritative.
- Failed extraction does not publish a valid cache marker.
- Unsupported input returns a stable result and an engine-owned diagnostic.
- Unload and destroy close or release every resource owned by the engine.

## Verification

`make architecture-check` enforces dependency and direct-system-call rules.
`make contract-check` exercises lifecycle and host failures.
`make check TEST=builtin_state` exercises resource reset, transient exclusion,
and canonical resource restoration, while `make check TEST=state_security`
exercises bounded corrupt-state rejection.
`make integration-check` proves deterministic state and engine isolation.
`make security-check` runs bounded state, VFS, archive, cache, and override
corpora. `make sanitizer-check` rebuilds those paths with address and undefined
behavior instrumentation.
