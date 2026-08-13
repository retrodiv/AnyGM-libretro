<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 retrodiv <retrodiv@proton.me> -->

# Supported formats

AnyGM recognizes structural format families and resolves their runtime
semantics through one shared engine. Recognition is not a promise that every
built-in operation present in arbitrary content is implemented.

## Anchor files

An `.anygm` anchor is a one-line text file naming the payload to load,
relative to the anchor's own directory. It provides a stable alias for a
selected payload without renaming that payload. A directly loaded anchor routes its
referenced file as if it had been loaded itself. Inside a ZIP-compatible
archive an anchor member overrides scored payload selection, may name a nested
archive, and a present anchor that cannot be read, parsed, or matched to a
member rejects the archive rather than silently losing to the scores. The
reference uses the same normalization as archive member paths, is confined to
the anchor's subtree, and may not name another anchor.

## Normalized data containers

Studio data containers using bytecode revisions 14, 15, 16, and 17 are
recognized. Revision-specific bytecode readers normalize instructions and
records into the shared content model before execution.

## Classic containers and projects

Classic revisions 600, 701, 702, 800, and 810 are recognized by the classic
reader and normalized into the same content model. Classic origin and bytecode
encoding are represented as separate structural facts; one is not inferred
from the other.

A compiled classic executable stores its resources in a shorter form than the
project it was built from, omitting fields only an editor uses. That difference
belongs to the compiled layout and is not a revision fact: every compiled
revision writes the short form.

Revision 600 compiled executables are parsed as user-supplied content and
normalized through the same classic importer as `.gm6` projects. Native code
and bundled support libraries are not executed.

An editor-standard `.gm6`, `.gmk`, or `.gm81` may have adjacent standard `.gex`
packages, ordinary externally referenced included files (notably GM6), and an optional
`.anygm-classic-fidelity` companion supplied alongside an editor project. The companion is
applied only when its
generation, project length, and embedded SHA-256 match the exact project bytes;
it restores compiled resource details and normalized game-information presence that the editor
format cannot encode. Referenced included files are read through the host VFS and their complete
bytes contribute to the derived-content cache key, so changing one cannot reuse a stale package.
Extension binary calls are imported as self-describing encoded library/symbol
aliases. Runtime support is selected from those names, never from a game title
or hardcoded content hash, and native DLLs are not loaded. Portable Saudio, SGAudio, SuperSound,
BGM, GMFMODSimple, FAudioGMS, and `caster_*` playback reads bounded WAV/OGG/MP3 assets through the host VFS;
the pxwrap adapter renders Pxtone projects in memory, GMXInput and joydll map to the host gamepad,
and nsfs plus color-key operations remain confined to the content/save VFS overlay. The portable
Wwise adapter parses bounded sound banks, reconstructs embedded Wwise Vorbis media in memory, and
supports event play/stop. Random, switch, and music containers choose the first reachable media
branch deterministically; parameter and switch setters are accepted but do not yet alter mixing.
Steam-, GOG-, analytics-, presence-, shell-, window-, and network-service extension calls are
deliberately offline or unavailable no-ops: they neither contact services nor obtain ambient OS
authority, but their absence does not abort a game. Savestates retain
their logical paths and dynamically computed SHA-256 identities so a closed handle can be recreated
without embedding an entire soundtrack or accepting changed bytes.

## Source projects and packages

The content layer can normalize supported project manifests, project archives,
and the checked-in structural package representation used by synthetic tests.
Project parsing and package writing remain part of the portable runtime build,
not the libretro adapter.

## Containers

Path-backed routing recognizes the extensions declared by the core:
`win`, `droid`, `zip`, `port`, `apk`, `yyp`, `yyz`, `gmk`, `gm6`, `gm81`, and
`exe`. Extension recognition selects a parser; magic and structural validation
still determine whether the input is accepted.

ZIP-compatible containers use bounded extraction, reject unsafe paths and
links, and select a normalized payload deterministically. See
`SECURITY_MODEL.md` for exact limits.

## Memory sources

The public API accepts a borrowed in-memory normalized content image. The bytes
and optional identity path remain valid until unload or destroy. Archive and
source-project routing is path-backed because it uses the host VFS and explicit
cache namespace.

## Unsupported input

Unknown revisions, contradictory structural facts, malformed chunks,
unsupported archive features, missing executable code, and invalid references
return a stable error. They never select another engine or enable behavior
based on content identity.
