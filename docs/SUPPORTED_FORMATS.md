<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 retrodiv <retrodiv@proton.me> -->

# Supported formats

AnyGM recognizes structural format families and resolves their runtime
semantics through one shared engine. Recognition is not a promise that every
built-in operation present in arbitrary content is implemented.

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
