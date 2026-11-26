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

An advanced anchor may also contain `[transforms]`, `[pipelines]` and `[patches]`.
Named external xdelta3/VCDIFF patches can be applied in an explicit pipeline order, including an
`input.final` chain after source selection. Original files remain unchanged; source, patch and
result SHA-256 values and exact result sizes are checked. The generic declaration, resource-path
and decoder limits are owned by `CONTENT_TRANSFORMS.md`, not by any particular format family.

Each `[overrides]` line declares a content rule. System `AnyGM.ini` defaults and anchor
declarations combine before execution. See [Content configuration](CONTENT_CONFIGURATION.md)
for syntax and precedence. A frontend cheat reset leaves content declarations in place.
The Content override directives option disables them. Unknown directives reject content
loading. Enabled declarations participate in save-state identity.

### Room actions and triggers

Room actions share the order `action|rooms|trigger|...`. List source room indices
from 0 to 1023, with commas and ascending inclusive ranges such as `1,3-5`.

```ini
roomskip|1,3-5|pad1:a,pad1:b,pad1:start|next
roomskip|8|key:escape|12
roomskip|2|room-enter|next
roomalarm|4|pad2:a,key:enter|obj_panel|0
roomcounter|4|pad1:b,key:space|obj_panel|phase=2|elapsed=120
```

Input triggers are comma-separated alternatives without spaces. Each `pad1:` through
`pad4:` prefix names a RetroPad port. The button names match `keybind`. Each `key:`
prefix names a physical keyboard key independently of RetroPad emulation and simulated
content keypresses. Supported names are lowercase `a` through `z`, `0` through `9`,
`f1` through `f12`, `escape`, `enter`, `space`, `backspace`, `tab`, `delete`,
`insert`, `home`, `end`, `pageup`, `pagedown`, `up`, `down`, `left`, `right`,
`lshift`, `rshift`, `lctrl`, `rctrl`, `lalt` and `ralt`. A fresh press triggers
the rule once. The trigger does not consume the input seen by the game. A held button
or a press on the frame that enters a source room does not repeat or carry over.

- `roomskip` transitions to `next` in authored play order or to the explicit room index.
  A valid explicit destination stops current audio before entering it. A rule whose
  destination is unavailable does nothing. `room-enter` selects an automatic skip
  when that room is entered. It is exclusive to `roomskip` and is written by itself.
  An explicit automatic destination must lie outside its own source list.
- `roomalarm` sets the numbered alarm (0 through 11) to one step on each active
  instance of the named object or its descendants. The ordinary alarm event runs
  the authored transition on the following step.
- `roomcounter` sets an existing numeric instance counter before Step when the
  named object or descendant has the specified numeric guard value. Both values
  are signed 32-bit integers. Choose a value that satisfies the authored completion
  condition. Missing or nonnumeric variables are ignored. A press that enters the
  guarded phase cannot also advance that phase.

Room rules layer by action, normalized source set, normalized trigger set, and
the addressed alarm or guarded counter when applicable. A replacement occupies
its last declaration position. At most one room jump runs per frame. An automatic
entry rule gets priority, then the last matching input rule with a valid destination.
A room change already requested by content gets priority over these rules.

For local development, `GML_ROOMSKIP` can hold one complete input-driven
`roomskip|...` directive and replace anchor input-driven jumps. Automatic
entry rules and timer rules retain their ordinary behavior.

The former `introskip`, `introauto`, `introstep` and `introcount` spellings
must be migrated. To retain the previous input, use `pad1:a,pad1:b,pad1:start`.
Use `roomskip` with `next` or the old explicit destination, `roomskip` with
`room-enter` for automatic advances, `roomalarm` for alarms and `roomcounter`
with `guard=value|counter=value` for counters. Changes to an anchor's rule text
change the identity of savestates created with that anchor enabled.

`ostype|N` declares which operating system this content is told it is running on, using the same
numbering used by `os_type` (0 Windows, 1 macOS, 6 Linux, and so on). Without it content is
answered `os_windows`, which is the desktop path most content is written for. It is read once the
anchor is installed and before the first frame, because content asks the question in the very first
events it runs, and the `GML_OS_TYPE` development setting still takes precedence over it. Content
that branches on `os_type` accordingly takes the branch written for the declared platform; the
declared value is returned without changing any other condition. It does not establish
permission to run the content or override its license terms.

`statereserve|33554432` declares a measured minimum state-buffer allocation in bytes.
Use it in the content's `.anygm` anchor when deferred initialization needs more simulation
state than the cold load contains. Loading the payload directly discovers its unique sibling
anchor; keep the measured reserve with the content rather than in a system INI. Values are positive
decimal integers through 1073741824. All enabled declarations contribute their maximum;
a lower declaration cannot reduce the normal allocation or an earlier higher minimum.
Content overrides must be enabled. This declaration does not change the state format, promise
arbitrary future growth, or make a fixed frontend resize an already allocated ring.
It affects allocation only, so changing it does not invalidate a state's simulation identity.

`emptyscreen|keep` declares that content which composes its own screen leaves an uncoloured
frame as it is: the core skips its end-of-frame empty-screen check and application-surface
fallback. See [Keeping an empty screen](CONTENT_CONFIGURATION.md#keeping-an-empty-screen).
It is not part of state identity.

`gamepad|keyboard` selects keyboard emulation for this content even when it contains optional
joystick or gamepad functions. RetroPad directions and action buttons use the existing keyboard
mapping; native pad connection, button and axis reads return inactive values. This is useful for
content whose default or menu controls require a keyboard despite containing pad references.
`gamepad|single` exposes only native pad index zero. Other indices report disconnected,
with inactive button and axis reads; the device-count query retains its slot capacity. Both
sticks and native buttons on the first pad remain intact. The frontend ownership option still
applies. This supports single-player content that otherwise retains the last exposed device.
`gamepad|auto` withdraws an earlier declaration and leaves the frontend's RetroPad behavior option
in charge. These singleton declarations follow ordinary content-override layering, disabling and
save-state identity. They do not edit the frontend option or the content's own controller settings.

`keybind|2|87|up` assigns Up on RetroPad port 2 to virtual key 87 during keyboard emulation.
The port is a decimal number from 1 to 4. Use decimal Win32 key numbers from 2 to 255.
Both the key and button fields accept a single value or a comma-separated list of distinct
values without spaces.
For example, `keybind|1|90|b,r2` maps both B and the right trigger on port 1 to virtual key 90.
With `keybind|1|65,83|b,r2`, either button holds both keys in the same frame.
The key is held while any listed button is down. Switching between buttons while the key stays
held does not produce another press or release. Button names are `b`, `a`, `y`, `x`,
`start`, `select`, `up`, `down`, `left`, `right`, `l`, `r`, `l2`, `r2`, `l3` and `r3`.
Only the named key changes; other default aliases and physical keyboard input remain intact.
Each key names one source port. Buttons on other ports do not activate that
key; physical keyboard input still reaches it independently. Only the declared buttons on
additional ports contribute to the keyboard's any-key check. Bindings do not select
keyboard emulation or affect native pad input. Declare
`gamepad|keyboard` as well when content contains native pad references. Declarations layer
by virtual key. A later declaration replaces the entire button list and port for each key it
names. Use `auto` alone in the button field, as in `keybind|1|65,83|auto`, to restore the
default mapping on port 1 for those keys. Each key in a list counts as one effective directive.
The port is explicit in every declaration; the old three-field syntax
is rejected. Update content declarations when updating a core that used that syntax.
These content-only declarations share ordinary override disabling and save-state identity.

Append `|turbo` to repeat a binding while any listed button is held. For example,
`keybind|1|65,83|b,r2|turbo` repeats both virtual keys together from either button.
The game sees three simulation frames pressed followed by three released, repeating
on the simulation clock. This gives ten presses per second at 60 Hz. The first pulse
can wait up to three simulation frames, and releasing all mapped buttons stops the
input immediately. The saved simulation clock preserves the repeat phase on restore;
physical keyboard input and native pad reads retain their ordinary behavior. A
later declaration replaces the turbo setting along with the button list. `auto`
is a complete binding value and cannot take the `|turbo` suffix.

Load-time `renderer|...` declarations and reversible `?crt setcall|...` settings are documented
in [Content configuration](CONTENT_CONFIGURATION.md#content-renderer-selection).

`call|script_name` invokes the named zero-argument GMS script exactly once,
after the first Step of a fresh content load. The name is the script asset
name, not a CODE entry name: `call|configure_input` resolves only
`gml_Script_configure_input`. It runs with an isolated scratch instance so it
does not borrow an active game's instance. Restoring a state marks the call as
already made: its effects are represented by the restored state and must not
run a second time.

`afterdraw|object_name|script_name|value` appends one named content script to each
completed ordinary Draw event (`Draw_0`) of that exact object. The script receives
the event's live instance as `self`, no `other`, and one finite literal numeric
argument. It runs in the event's current render target and projection before any
event-local view policy is withdrawn. It does not reorder existing events, create
objects or invent a cursor. Missing objects/scripts do nothing. An invisible,
inactive or destroyed receiver receives no extra draw. Inherited events may run
on the exact declared object, but descendants must opt in separately.

The declaration respects Content overrides, `?gameres`, `?aspect` and `?global`
activation; `?monitor` is not valid because this is a draw-time operation, not a
monitor-transition write. Withdrawal stops the appended calls without undoing
authored script side effects. Nested `screen_redraw` does not append the script
recursively. Reset retains the parsed declaration, and restored frames do not
execute it until the next ordinary draw. Use a drawing-only authored helper;
this is not permission to suppress a menu or to modify payload bytes.

`listset|target|variable[index]|item|value` repairs one numeric item in a DS list. `target` is an
object name for a list held by its first active instance, or the reserved word `global` for a list
held by a global variable. The array index is optional when the variable holds the list handle
directly. It is reapplied after every Step while content overrides are enabled, so a later
configuration load cannot overwrite the repair. This is suitable for replacing invalid persisted
configuration with a stable session policy; in-game changes to that list item take effect only
after the content override is disabled.

A `?gameres` directive applies only while *Render at game resolution* is selected. It declares an inner logical raster for presentation values that the content scales to a separate output raster. Each scoped directive captures the previous value before its first write and restores it when the selection closes. This includes mutable presentation fields such as `@window_w`, `@window_h`, `@application_w`, `@application_h`, and `@application_hold`, and also permits a scoped instance-variable or surface-resize directive when its target has one meaningful previous value. Room-owned `view_*` and `background_*` arrays are recaptured on room entry, and their directives settle before the frame's output geometry is published.

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

`@application_hold=1` keeps the application surface at its current extent while content asks to
resize it. A content `surface_resize(application_surface, w, h)` is recorded instead of applied,
and releasing the hold applies the latest recorded request. Content that resizes the application
surface on every Draw would otherwise draw at its own extent before `@application_w` and
`@application_h` set the native one. Declare the hold together with those two fields:

```
[overrides]
?gameres @application_hold=1
?gameres view_wport[0]=$view_w
?gameres view_hport[0]=$view_h
?gameres @application_w=$view_w
?gameres @application_h=$view_h
```

`@present_shift_x` and `@present_shift_y` move the delivered frame by whole pixels and fill the
edge they leave with black. The shift reaches the copy handed over and never the
completed frame, so it cannot move the bytes a savestate carries. `$global.<name>` reads what a
global holds on the frame the directive is evaluated, and the presentation pass re-evaluates every
`?gameres` directive before geometry is read, so a target declared this way follows the value live
rather than freezing the reading it was written with.

`@present_crop_left`, `@present_crop_top`, `@present_crop_right`, and
`@present_crop_bottom` remove whole-pixel margins from the completed composition before it is
scaled or handed to the host. They retain the exact pixels inside the selected rectangle: a
content-owned decoration drawn across the complete frame is cropped with the picture instead of
being disabled or resampled. The compositor and the completed frame carried by a savestate keep
their full raster. Negative margins, or margins which leave an empty axis, disable the complete
crop rather than publishing a partial or invalid rectangle. Pointer input maps back through the
crop origin into the compositor's coordinate system.

```
[overrides]
?gameres @present_crop_left=16
?gameres @present_crop_top=12
?gameres @present_crop_right=16
?gameres @present_crop_bottom=12
```

`?crt crtmode|width|height|hz` and `?crt roomspeed|rooms|fps` declare a CRT mode and the cadence
of chosen rooms under effective CRT adjustment. The `crtmode` fields can use `window_w`,
`window_h` and `window_fps` to follow the game's raster and room cadence. See
[Content configuration](CONTENT_CONFIGURATION.md#declared-crt-mode).

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

Camera configuration selectors address handles 0 through 63. This 64-bit selector is independent
of the runtime camera pool, which supports up to 1024 simultaneous handles, including reserved
room-view handles. A camera outside the selector range remains usable through ordinary GML camera
and room-binding functions.

### Fixed-coordinate surface compositors

An aspect-scoped `surface_canvas|OBJECT|VARIABLE|W|H` declares the native canvas
of one persistent presentation object. `W` and `H` are literal integer dimensions
between 16 and the framebuffer limits. The object variable must name a live
user surface. The declaration affects only forced aspects; disabling the mode
restores the surface's original dimensions.

```
?aspect surface_canvas|obj_presenter|canvas|640|480
```

The final aspect is calculated from this complete canvas, independently of a
shorter gameplay camera. The surface widens to the forced output and retains
its declared height. Its ordinary drawing coordinates are horizontally centred
without scaling. A complete native-width `application_surface` draw starting at
x=0 spans the widened surface, retaining its authored vertical offset and height.
Narrow rooms clip that application image to their bounds. The content's clear
fills the entire surface, including the expanded HUD background. Presenting the
complete surface at (0,0) uses the final output extent even when the GUI declares
the original canvas size. Offset, partial, rotated and preview draws retain
their ordinary semantics.

This opt-in policy describes a self-compositor that redraws its singleton
surface each frame, with an unscaled HUD beside an application image. It is not
a general transformation for arbitrary cached surfaces or multiple views.

### Event-local visible-view layout

An aspect draw route can give one object's event a different layout rectangle:

```
?aspect obj_notice@Draw_0->visible_view
```

Like `full_view`, this exposes the forced gameplay origin and extent through
view zero's `view_xview`, `view_yview`, view size and port size during that
event only. `visible_view` intersects that rectangle with the current room's
bounds first. A right- or bottom-anchored notice therefore stays inside a
room narrower or shorter than the forced viewport, instead of landing in a
clipped side band. An empty intersection has zero extent. It does not add
padding, move geometry, change renderer projection or enlarge a literal fill.
The event must calculate its own placement from those view values; cached
coordinates require their own declaration. Nested events restore the prior
values, and the ordinary HUD and gameplay camera remain unchanged. Routes
apply only while the forced frame is wider than its native view.

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

Revision 600 compiled executables are parsed as user-supplied content. Their
archive entry is inflated; an explicitly configured optional record pipeline
can prepare another representation before the same classic importer as `.gm6`
projects. Otherwise the normalized record is consumed directly. Native code
and bundled support libraries are not executed.

Where a content layout requires an external transform, the core accepts an
explicitly supplied buffer program through `AnyGM.ini` or an `.anygm` anchor.
The program format and selection rules are in `CONTENT_TRANSFORMS.md`.

The editor-standard revision-530 project is accepted directly with its `.gmd` extension
and inside an inert executable wrapper when its normalized header and complete structure
validate. The executable reader does not choose a source operation from a revision;
configured input pipelines and candidate selection prepare other source representations
before the same structural reader. Native code is never executed.

Adjacent `.gex` packages share the optional record boundary: their normalized
representation retains the ordinary common header followed by package data.
The reader does not supply a decoder for other representations. See
`CONTENT_TRANSFORMS.md` for the structural context contract.

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
authority, but their absence does not abort a game. Built-in Steam capability queries likewise
report an unavailable service: initialization, file existence, cloud writes and stats readiness
return false, while cloud reads and the persona-name query return empty strings. Cloud writes do
not create local save files or completion events. Savestates retain
their logical paths and dynamically computed SHA-256 identities so a closed handle can be recreated
without embedding an entire soundtrack or accepting changed bytes.


Legacy `action_if_question` calls use the same portable dialog policy as `show_question`:
the native dialog is unavailable and the result is false.

`physics_debug` controls per-VM host error reporting, disabled by default. Rejected
fixture and joint handles, fixture capacity, world scale and chain geometry report
through the host logger when enabled. The switch is transient diagnostic verbosity;
it does not alter the simulation, drawing configuration or game-state bytes.

Compiled bytecode exception hooks preserve pending messages across nested calls
and `finally` dispatch. A `catch` receives a struct with a `message` field.
Handlers and deferred messages survive supported suspended direct-call chains
and current-format save states. Unhandled exceptions end their invocation with
an error diagnostic. This is a bytecode contract; source-project exception syntax
and additional native exception metadata are not established here.

## Source projects and packages

The content layer can normalize supported project manifests, project archives,
and the checked-in structural package representation used by synthetic tests.
Project parsing and package writing remain part of the portable runtime build,
not the libretro adapter.

## Containers

Path-backed routing recognizes the extensions declared by the core:
`win`, `droid`, `unx`, `zip`, `port`, `apk`, `yyp`, `yyz`, `gmd`, `gmk`, `gm6`,
`gm81`, and `exe`. Extension recognition selects a parser; magic and structural
validation still determine whether the input is accepted.

An exporter names the Studio data container after the platform it was exported
for: `data.win` on Windows, `game.droid` on Android, `game.unx` on Linux, and
`game.ios` in Apple exports. These names select the same structural container reader.
They are one container under platform-specific names, so any of them is accepted as the
directly loaded payload. Archives and self-extracting packages select these canonical names.
Automatic sibling lookup beside a launcher searches `data.win`, `game.droid`, and `game.unx`
in its existing locations; load an Apple payload directly or name it in an explicit anchor.

When a structurally valid PE has no supported internal payload, the core may load the exact
regular `data.win` beside it. The sibling passes through ordinary Studio validation. A
malformed or unsupported internal container is terminal, not a reason to use the sibling.
The executable remains the launch and save identity; content-relative files resolve from
their shared directory. No native machine code is run.
An `exe` may also carry one unambiguous normalized Studio `FORM` data image.
The bounded reader validates it before extracting it to a disposable cache.
An embedded image with no VM code defers to an adjacent image only when both
declare the same project identity and the adjacent image has VM code.
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
MP3 stay compressed until first playback. An Ogg entry that its sound record
declares compressed is then decoded as its voices consume it, in bounded windows,
as a loose Ogg file is; any other Ogg entry is decoded completely on its first
play. Both produce the same samples. A WAVE in any other encoding is
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

## Stored room information

`room_get_info` returns a detached snapshot of stored room metadata, with optional view,
placement, layer, element and tile-cell collections. It reads the same editable room template,
placement layout and decoded tile grids that ordinary room loading uses, without entering the
queried room. Changing returned strings, structs or cells does not change the source resource.
Empty collections use zero-length arrays. Layer records follow increasing depth, and tilemap
cells preserve their unsigned packed values. The later optional live-room argument is not
implemented; this API reports stored data.
