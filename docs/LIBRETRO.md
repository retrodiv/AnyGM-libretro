<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 retrodiv <retrodiv@proton.me> -->

# The libretro surface, and the decisions behind it

What this core advertises to a frontend, and why. Everything below is a
deliberate choice; the point of writing it down is that a reviewer should not
have to guess which of them are accidents.

## Advertised extensions

```
win | droid | zip | port | apk | yyp | yyz | gmd | gmk | gm6 | gm81 | exe | anygm
```

with `need_fullpath = true` and `block_extract = true`. That pair is not
optional: the core does its own archive and executable parsing, so it needs the
path rather than a buffer, and it must be handed the archive rather than a
frontend's extraction of it.

Two entries have a cost worth stating.

**`zip`** makes every zip in a player's library list this core as a load
candidate, and `block_extract` disables browse-inside-archive for it. That is
accepted because a packaged Studio game legitimately arrives as a zip and
refusing the extension would make the common case unreachable;
other cores also claim `zip` for their own archives.

**`exe`** overlaps with other executable loaders. It is accepted because
classic containers can be embedded in an executable, and declining the
extension would exclude that format family. Unsupported executables receive
a diagnostic rejection rather than being treated as supported content.

An `.info` file submitted to `libretro-super` must carry exactly this list in
`supported_extensions`, and the submission should state the two choices rather
than leave them looking accidental.

## Decisions a reviewer will ask about

**Core option groups are Video, Input, System, and Development, in that order.**
System contains Language followed by Region. Frontends without category support
receive the same options through the flat declaration. The first occurrence of
each category in the option definitions follows the category table's order:
frontends may build their menus from those first occurrences, not from the table.

**Save states are `serialized`, not `deterministic`.** `randomize()` seeds from
host entropy; the seed is part of serialized state, so rewind and
save/load are exact, but two fresh runs of the same
content are not bit-identical. Netplay and run-ahead advertising stays off
until the asynchronous atlas decode is proved timing-independent by a test that
diffs prefetch-on against prefetch-off frame hashes.

**`retro_cheat_set` is a GML expression channel.** A cheat entry is evaluated
by the runtime as a runtime override, which means any entry in a frontend's
cheat database becomes an expression this core runs. That is powerful and
surprising in equal measure; it is documented rather than gated because the
override surface is the same one the diagnostics use, and gating it would take
the feature away from the only people who use it.

**"Clear current game saved data on load" is sticky and destructive.** It defaults
to Off. While it is On, every
load deletes the save namespace of the content being loaded, not just the next
one. The description says so; it is not auto-reset after acting, because a
player who turns it on to escape a corrupted save usually needs it to survive
one more load.

**Cache cleanup has two independent Development options, both On by default.**
They follow the saved-data option: "Clear current game cache on unload", then
"Clear all game caches on load". The first removes the immediate `AnyGM-cache`
children used by the loaded session, after unload or core destruction has closed
the runtime's files and mappings. Warm-cache reads, transformed inputs and
staging renames are tracked as well as newly written caches. The current frontend
value is read on close, so a menu change applies even while content is paused.
Failed preparation also cleans its attempted cache entries when enabled.

The second empties `AnyGM-cache` before any content preparation, including old
versions and caches not used by the incoming game. It retains the root directory;
a deletion failure refuses the new load with a message. Unload deletion failures
are warnings. The existing cache names and layout are unchanged. Unused historical
versions are handled by global cleanup, not guessed from a filename at unload.
Neither option removes the separate saved-data namespace. Cleanup is limited to
the explicit adapter cache root; a frontend supplying no usable writable root
does not grant permission to delete the loader's fallback directories.

Native cache cleanup includes hidden entries and treats symbolic links and Windows
reparse points as leaves. A linked cache root is refused. Native directory deletion
uses host APIs because the frontend VFS lacks a dedicated directory-delete operation;
URI cache roots use frontend VFS operations exclusively and report unsupported removal.
Walks reject path-bearing entry names and stop with failure above 64 levels or
1,048,576 visits. Cache users must be closed before deletion, and hosts sharing a
cache root across concurrent core instances must disable global cleanup.

**Maximum geometry is fixed at 3840x2160.** A frontend may preallocate around
33 MB from that, which is heavy on mobile. It is fixed rather than derived from
content because constant maxima are what make `SET_GEOMETRY` safe to send at
any time: a geometry change can never enlarge the frontend's allocation, so it
can never crash it.

**The VFS interface is required at version 3 or not used at all.** The
directory operations this core needs are v3; taking a v2 interface would mean a
half-working file layer whose failures depend on which operation content
reached first. An older frontend gets the stdio fallback, which is complete.

**Development settings are read from the environment.** Around 150 `GML_*`
names change runtime behaviour when set. Frontends cannot set environment
variables, so this surface is developer-only in practice; `docs/DIAGNOSTICS.md`
describes the tracing subset, and the rest exists for the same reason a
debugger does.

**No memory descriptors.** `retro_get_memory_data` returns NULL: the runtime's
state is a value graph, not an address space, so there is no region a
frontend's cheat search or achievement runtime could scan meaningfully.
