<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 retrodiv <retrodiv@proton.me> -->

# Supported formats

AnyGM recognizes structural format families and resolves their runtime
semantics through one shared engine. Recognition is not a promise that every
built-in operation present in arbitrary content is implemented.

## Anchor files

An `.anygm` anchor is a text file naming the payload to load, relative to the
anchor's own directory. It provides a stable alias for the selected payload
without renaming it. A directly loaded anchor routes its referenced file as if
it had been loaded itself. Inside a ZIP-compatible archive an anchor member
overrides scored payload selection, may name a nested archive, and a present
anchor that cannot be read, parsed, or matched to a member rejects the archive
rather than silently losing to the scores. The reference uses the same
normalization as archive member paths, is confined to the anchor's subtree,
and may not name another anchor. Blank lines and lines whose first significant
character is `#` are comments.

An anchor is an alias for the payload it references: its writable save
namespace follows that payload. Loading through the anchor or loading the
payload directly therefore shares the same profile and settings.

The basic form is one significant line holding the reference. The advanced
form opens with the exact header line `[anygm]`, selects the payload with one
`payload=<reference>` key, and may add an `[overrides]` section:

```
[anygm]
payload=data.win

[overrides]
# freeze a global, patch a forced-aspect view
$lives=99
?aspect view_wport[0]=$forced_w
```

Each `[overrides]` line is one directive in the same grammar the host cheat
interface uses, including development-menu declarations. An
`introskip|1,3-5` directive lists room indices where A, B, or Start advances
to the next room in play order; the `GML_INTROSKIP` development setting, when
present, takes precedence over it. `introauto|1,3-5` takes the same room list
and advances the moment such a room is entered, with no button at all. Both forms
advance the same way; a room listed by `introauto|` advances before
`introskip|` is consulted for it. Directives load into
a channel separate from host cheats: host cheat resets do not clear them, and
the content-override configuration switch disables them without touching host
cheats. Parsing is fail-closed — an unrecognized directive, or the one-shot
`room=` form, rejects the load rather than being dropped. Active directives
join the save-state identity, so a state saved with them loads only while
they are active; content without directives keeps its state identity
unchanged.

`ostype|N` declares which operating system this content is told it is running on, using the same
numbering used by `os_type` (0 Windows, 1 macOS, 6 Linux, and so on). Without it content is
answered `os_windows`, which is the desktop path most content is written for. It is read once the
anchor is installed and before the first frame, because content asks the question in the very first
events it runs, and the `GML_OS_TYPE` development setting still takes precedence over it. Content
that branches on `os_type` accordingly takes the branch written for the declared platform; the
declared value is returned without changing any other condition. It does not establish
permission to run the content or override its license terms.

`call|script_name` invokes the named zero-argument GMS script exactly once,
after the first Step of a fresh content load. The name is the script asset
name, not a CODE entry name: `call|configure_input` resolves only
`gml_Script_configure_input`. It runs with an isolated scratch instance so it
does not borrow an active game's instance. Restoring a state marks the call as
already made: its effects are represented by the restored state and must not
run a second time.

`listset|target|variable[index]|item|value` repairs one numeric item in a DS list. `target` is an
object name for a list held by its first active instance, or the reserved word `global` for a list
held by a global variable. The array index is optional when the variable holds the list handle
directly. It is reapplied after every Step while content overrides are enabled, so a later
configuration load cannot overwrite the repair. This is suitable for replacing invalid persisted
configuration with a stable session policy; in-game changes to that list item take effect only
after the content override is disabled.

A `?gameres` directive applies only while *Render at game resolution* is selected. It declares an inner logical raster for presentation values that the content scales to a separate output raster. Each scoped directive captures the previous value before its first write and restores it when the selection closes. This includes mutable presentation fields such as `@window_w`, `@window_h`, `@application_w`, and `@application_h`, and also permits a scoped instance-variable or surface-resize directive when its target has one meaningful previous value. Room-owned `view_*` and `background_*` arrays are recaptured on room entry, and their directives settle before the frame's output geometry is published.

```
[overrides]
?gameres obj_presenter:canvas_scale=1
?gameres view_wport[0]=320
?gameres view_hport[0]=240
?gameres @window_w=320
?gameres @window_h=240
?gameres @application_w=320
?gameres @application_h=240
# shift the delivered frame from a current global value
?gameres @present_shift_y=$global.vertical_offset/2
```

`@present_shift_x` and `@present_shift_y` move the delivered frame by whole pixels and fill the
edge they leave with black. The shift reaches the copy handed over and never the
completed frame, so it cannot move the bytes a savestate carries. `$global.<name>` reads what a
global holds on the frame the directive is evaluated, and the presentation pass re-evaluates every
`?gameres` directive before geometry is read, so a target declared this way follows the value live
rather than freezing the reading it was written with.

A scope prefix belongs to the directive that carries it, and a `;` chain fans out into one
directive per slot, so each line in a chain that needs the scope has to state it.

`drawhold|<name>|<value>` temporarily writes a global during frame drawing and
restores its pre-draw value afterward. Other phases retain that value.
`?global.<name>` may follow another scope on the same line and enables a
directive only while the named global is non-zero. When it becomes zero,
the scoped destination restores its captured value.

```
[overrides]
# temporarily select the drawing branch
?gameres drawhold|overlay|0
?gameres @window_w=320
?gameres @window_h=240
# enable an offset only while the content global is non-zero
?gameres ?global.overlay @present_shift_y=$global.screen_shake/2
```

`$view_w` and `$view_h` read the current room's authored `view_wview`/`view_hview` live.
Unlike `$base_w`/`$base_h`, they do not refer to the presentation pass's preceding output. Use them
when a room-scoped directive must pin a port to that room's view before geometry is decided:

```
[overrides]
# present each room at its own authored view, whatever its port declares
?gameres view_wport[0]=$view_w
?gameres view_hport[0]=$view_h
```

Content that reads the display once during initialization may declare how its cached presentation
state follows later virtual-monitor changes. `monitorview|H|MIN|MAX` derives a logical height `H`
and a width from the monitor aspect, clamped between the `W:H` ratios `MIN` and `MAX`. A
`?monitor` directive then runs once after each live Monitor width or Monitor height transition; it
does not freeze the assigned value between transitions. The values `$monitor_w`, `$monitor_h`,
`$monitor_view_w`, `$monitor_view_h`, `$monitor_extra_w`, and `$monitor_extra_h` are available to
those expressions. In addition to globals, instance variables, and legacy view arrays, a monitor
program may resize selected live cameras with `camera[LIST]:x|y|width|height=VALUE`, resize a surface
named by an active instance variable with `surface|OBJECT|VARIABLE|WIDTH|HEIGHT`, and resize the
owned application surface through `@application_w` and `@application_h`. Camera lists accept
comma-separated handles and inclusive ranges. These operations affect only runtime resources that
already exist; they do not rerun content initialization or manufacture missing objects, cameras, or
surfaces.

## Normalized data containers

Studio data containers using bytecode revisions 14, 15, 16, and 17 are
recognized. Revision-specific bytecode readers normalize instructions and
records into the shared content model before execution.

## Classic containers and projects

Classic revisions 530, 600, 701, 702, 800, and 810 are recognized by the classic
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

Where a content layout requires an external transform, the core accepts an
explicitly supplied buffer program through `anygm.ini` or an `.anygm` anchor.
The program format and selection rules are in `CONTENT_TRANSFORMS.md`.

The editor-standard revision-530 project is accepted directly with its `.gmd` extension.

An editor-standard `.gmd`, `.gm6`, `.gmk`, or `.gm81` may have adjacent standard `.gex`
packages, ordinary externally referenced included files (notably GM6), and an optional
`.fidelity.patch` companion supplied alongside an editor project. The companion is
applied only when its
generation, project length, and embedded SHA-256 match the exact project bytes;
it restores compiled resource details and normalized game-information presence that the editor
format cannot encode. Referenced included files are read through the host VFS and their complete
bytes contribute to the derived-content cache key, so changing one cannot reuse a stale package.
Adjacent extension packages use their encoded package names; filenames matching the ordinal
placeholder pattern are ignored.

Companion format 2 can restore a resource either from a complete payload or from an exact binary
delta against the editor-project payload. Both sides of a delta carry SHA-256 checks, and a
malformed or mismatched delta fails closed. This changes storage representation only: the normalized compiled manifest presented to the
importer is unchanged.

Extension binary calls are imported as self-describing encoded library/symbol
aliases. Runtime support is selected from those names, never from a game title
or hardcoded content hash, and native DLLs are not loaded. Portable Saudio, SGAudio, SuperSound,
BGM, GMFMODSimple, FAudioGMS, and `caster_*` playback reads bounded WAV/OGG/MP3 assets through the host VFS;
the pxwrap adapter renders Pxtone projects in memory, GMXInput and joydll map to the host gamepads,
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
`win`, `droid`, `zip`, `port`, `apk`, `yyp`, `yyz`, `gmd`, `gmk`, `gm6`, `gm81`, and
`exe`. Extension recognition selects a parser; magic and structural validation
still determine whether the input is accepted.

When a structurally valid PE has no supported internal payload, the core may load the exact
regular `data.win` beside it. The sibling passes through ordinary Studio validation. A
malformed or unsupported internal container is terminal, not a reason to use the sibling.
The executable remains the launch and save identity; content-relative files resolve from
their shared directory. No native machine code is run.
An `exe` may also carry one unfiltered Cabinet in a validated PE raw section. The supported
single-runtime profile uses one Cabinet volume and LZX window 21, contains exactly one normalized
Studio payload, and may contain regular external runtime assets. The core opens only the declared
Cabinet subrange through host VFS callbacks, extracts it into a verified disposable cache, and
passes the selected payload and asset root through the ordinary Studio loader and shared engine.
It never runs the native executable or any executable member.

Structurally complete multipart, continuation, or other compression profiles return
`ANYGM_ERROR_UNSUPPORTED`. False signatures, ambiguity, truncation, malformed tables or blocks,
checksum/decompression failures, unsafe members, and incomplete extraction return
`ANYGM_ERROR_INVALID_CONTENT`.

ZIP-compatible containers use bounded extraction, reject unsafe paths and
links, and select a normalized payload deterministically. See
`SECURITY_MODEL.md` for exact limits.

## Embedded audio

A sound stored in a container is decoded from what its own header declares, not
from its extension. RIFF/WAVE carries 16-bit PCM in place, and 8-bit PCM and
four-bit MS ADPCM are decoded into a bounded per-sound buffer; OGG Vorbis and
MP3 stay compressed until first playback. A WAVE in any other encoding is
silent and reports itself under the audio log rather than being played as if
its bytes were samples.

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
