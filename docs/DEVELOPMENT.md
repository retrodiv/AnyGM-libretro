<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 retrodiv <retrodiv@proton.me> -->

# Development and validation

## Working method

Migration and refactoring should preserve existing behavior first. Move or
extract whole functions and cohesive data structures, establish the new owner,
then make the minimum edits required by the boundary. Do not redesign a
subsystem merely because its file changes.

Group mechanical edits into coherent batches. Compiling and testing every
single moved line makes the work slower without improving fault isolation.
Instead, inspect the diff, compile the affected ownership layer, run its focused
test, and widen validation at architectural milestones.

## Validation pyramid

| Layer | Command | Purpose |
| --- | --- | --- |
| Architecture | `make architecture-check` | Enforce adapter and compatibility seams |
| Public headers | `make api-check` | Compile the public API from C and C++ |
| Portable build | `make runtime` | Build the engine without libretro |
| Host contract | `make contract-check` | Exercise the public API and synthetic source |
| Instance integration | `make integration-check` | Prove ownership and two-engine isolation |
| Security corpus | `make security-check` | Exercise bounded malformed state, VFS, cache, archive, normalized data, bytecode, and overrides |
| Unit suite | `make check` | Run parser, compatibility, VM, and runtime tests |
| Instrumented suite | `make sanitizer-check` | Run parser and contract gates with ASan/UBSan |
| Core link | `make` | Produce the libretro shared library |

Use the smallest relevant target while developing. Before declaring a release
candidate, run a clean sequential build, all non-diagnostic tests, export
inspection, policy scans, license/provenance scans, and a source-language scan.
Diagnostic tests are opt-in because they are investigation tools rather than
release gates.

## Resource limits

Generated tables and a few large C modules can make parallel compilers consume
substantial memory. Use `make -j1` for release validation and constrained
machines. If a build is interrupted, resume the same target; dependency files
avoid recompiling unchanged objects.

## Buildbot shape

The core target is named `anygm_libretro` and selects the conventional shared
library suffix per platform. `Makefile.common` is the source-of-truth list for
runtime and adapter sources. The top-level `Makefile` accepts the conventional
`platform`, `CROSS_COMPILE`, `CC`, `AR`, `CFLAGS`, `CPPFLAGS`, `LDFLAGS`, and
`STATIC_LINKING` inputs expected by automated core builders.

The CI matrix is a build declaration, not proof that every CI environment is available
locally. Platform-specific failures should be fixed in the adapter or build
selection rather than by adding platform branches to portable runtime code.

## Review checklist

- Does the change keep exactly one engine and instance-owned mutable state?
- Is a format-layout difference confined to a parser?
- Is a semantic difference represented by a named compatibility policy?
- Does portable code use host services rather than operating-system calls?
- Are adapter types absent outside `src/adapters/libretro/`?
- Is state written canonically and loaded transactionally?
- Do new variable-size inputs have explicit limits and corrupt-input tests?
- Is every generated or third-party byte traceable to a notice or generator?
- Are source, comments, diagnostics, tests, and documentation in English?
- Are first-party copyright notices limited to 2026?
- Were tests run once at the correct batch boundary and recorded accurately?
