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

## The version

`src/api/anygm.h` holds the only copy of this project's version, as `ANYGM_VERSION`. An adapter
reports it as the runtime library version, and every publishable metadata file declares
`display_version = "Git"` instead of a number, so no copy can promise a value that has gone stale.

Every commit increments it, in one step:

```sh
tools/bump-version.sh      # increments the patch component, printing the old and the new value
```

The patch component moves. The `0.1` component changes only when the project owner asks for it, and
then by hand. `make architecture-check` proves the value is well formed, exercises the increment and
its rejections on a copy of the header, and refuses a second copy of the series anywhere else in the
tree.

The repository also ships the hook that enforces the increment at commit time. Enable it once per
clone:

```sh
git config core.hooksPath .githooks
```

It refuses a commit that would keep the previous version and names the increment step in the
refusal. An amend, a merge, a rebase replay or a cherry-pick passes `git commit --no-verify`: those
rewrite or carry commits that already bumped rather than adding a new change.

## Reporting a defect

Say what you did, what you expected, and what happened, and include the core
version string shown by the frontend.
