<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 retrodiv <retrodiv@proton.me> -->

# Contributing

Three rules cover almost everything.

**Run `make check` before you open a pull request.** It builds with `-Werror`
and runs the unit, contract, architecture and fuzz tiers. `make sanitizer-check`
runs the same code under ASan, UBSan and LSan and is worth running for anything
that touches memory ownership.

**One logical change per pull request.** A commit message says what the code now
does and what was measured, not what was attempted. If a change is motivated by
a specific piece of content, describe the behaviour, never the content: this
repository names no game, anywhere, for the reasons in `README.md`.

**Do not attach content.** Issues and pull requests must not carry game files,
extracted assets, screenshots of copyrighted material, or anything else you do
not have the right to redistribute. A reproducer is a description of the
behaviour plus, where possible, a synthetic fixture built from first-party
sources.

## Where things live

`docs/CODE-MAP.md` is the map of the tree and is enforced by an architecture
check, so a file added in the wrong place fails the suite rather than drifting.
`docs/PROVENANCE.md` records the stated origin of generated data and selected
non-obvious constants; additions in those categories need an entry.

## Reporting a defect

Say what you did, what you expected, and what happened, and include the core
version string shown by the frontend.
