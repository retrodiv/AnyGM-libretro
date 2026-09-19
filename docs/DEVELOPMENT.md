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
| Compiler warnings | `make warnings-check` | Build owned sources with `-Wall -Wextra -Werror` |
| Architecture | `make architecture-check` | Enforce adapter and compatibility seams |
| Builtin registry | `make builtin-registry-check` | Prove exact-name/ID/owner/cache coverage and generated-index parity |
| Builtin resource state | `make check TEST=builtin_state` | Prove single-owner lifetime, transient exclusion, and canonical save/load/save bytes |
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

The large runtime characterization binaries expose ownership-level filters:

The builtin dispatch binary also accepts `--case input.gamepad_guid` to run the
string-valued identity capability and argument-boundary contract in isolation.
Without arguments it retains the complete dispatch characterization.

```sh
make -j1 check TEST=d3_state D3_TEST_ARGS='--case raster.blend_and_shader'
make -j1 check TEST=persistent_room PERSISTENT_TEST_ARGS='--case io.save_overlay_sandbox'
```

Each filtered case starts from a clean synthetic fixture. The software-3D test runner
may replay explicitly bounded prerequisite stages; persistent cases do
not inherit mutable state from another registered case.

`make check TEST=renderer_fonts` exercises runtime-font lifetime and renderer-state
restoration through an injected memory VFS. Its binary also accepts `--case`.
`tests/unit/media/font_test_fixture.h` owns the shared synthetic TrueType builder
used by these controls and the existing policy-free font-raster tests.
`test_font_state`, registered in `integration-check`, rejects a late malformed VM
section after removing a live font and checks exact rollback with a missing or
changed source file. It creates every state during its own fresh content load.

The Classic program fixture accepts `font <name> <size> <bold> <italic> <first> <last>`
in manifest generations. It carries at most eight fonts, sizes 1..256, zero/one style
flags and inclusive BMP ranges of at most 256 characters. Font normalization and
invalid declaration bounds are covered by `make check TEST=classic` with
`CLASSIC_TEST_ARGS='--case classic.fonts'`.

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

The version a built core reports belongs to `ANYGM_VERSION` in `src/api/anygm.h` and to nothing
else. The buildbot's metadata declares `Git`, so a nightly identifies itself by the number its own
commit carries rather than by a value written into a metadata file. `tools/bump-version.sh` is the
whole procedure for moving that number; `make architecture-check` proves it is well formed and that
no second copy of it exists in the tree.

## Review checklist

- Does the change keep exactly one engine and instance-owned mutable state?
- Is a format-layout difference confined to a parser?
- Is a semantic difference represented by a named compatibility policy?
- Does portable code use host services rather than operating-system calls?
- Are adapter types absent outside `src/adapters/libretro/`?
- Is state written canonically and loaded transactionally?
- Does `GmlVM` treat builtin resource storage as opaque, with lifecycle and
  staged codecs owned only by `gml_builtin_state.c`?
- Do new variable-size inputs have explicit limits and corrupt-input tests?
- Is every generated or third-party byte traceable to a notice or generator?
- Are source, comments, diagnostics, tests, and documentation in English?
- Are first-party copyright notices limited to 2026?
- Were tests run once at the correct batch boundary and recorded accurately?
