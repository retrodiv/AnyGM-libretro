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
| Embedded-Cabinet cache marker serialization budget | 20 MiB |

External content transforms compile bounded C-like source into buffer programs with no host capabilities. Their format,
selection, memory limits and proportional instruction budget are documented in
`CONTENT_TRANSFORMS.md`. A work bound is not a wall-clock deadline; a frontend accepting untrusted
content and programs should still enforce process resource and time limits.

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
The revision-selected adaptation interface is omitted from this unpublished
history. Earlier snapshots with omitted implementations are not supported builds. Game Maker 6 archive entries, padding counts,
compressed lengths, settings blobs, resource counts, and decoded integrity
words are validated before the resource stream is accepted.
Runtime bytecode decoding uses a bounded entry point. It retains the direct
decoder when the maximum operand window is available and uses a zero-padded
local window only at an input boundary, so the normal cached decode path does
not gain a per-instruction copy.

Project, bytecode, image, audio, and state readers validate their own sizes and
counts before allocation or pointer arithmetic. A new variable-length field
must introduce a matching limit and a synthetic boundary test in its owning
module.

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

The VM state reader exposes an opaque bounded cursor to the builtin resource
owner at four established field positions. That owner validates INI,
map/list/grid, physics, emitter, external-audio handle/path/hash, Saudio ID, and time-source counts
and dimensions before allocation or indexed access. It cannot change the root framing, advance
outside the VM section, or publish an independently restored object. Decoding
occurs in the engine's scratch transaction, so any resource-stage failure
discards the candidate and preserves the prior live engine exactly.
The list/queue/stack pool accepts at most 1,024 simultaneously live containers; this remains a
state-reader bound as well as a runtime resource bound.

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
