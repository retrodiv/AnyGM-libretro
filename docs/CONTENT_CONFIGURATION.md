<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 retrodiv <retrodiv@proton.me> -->

# Content configuration

The optional `AnyGM.ini` in the frontend system directory supplies content
defaults. An advanced `.anygm` anchor adds or replaces declarations. The INI
can also target exact original payload bytes by SHA-256. All identities and
rules come from external configuration; the core ships none of its own.

The canonical filename is `AnyGM.ini`. Lookup follows the host filesystem's case
rules; the core adds no separate capitalization check. Case-sensitive hosts require
that spelling, while case-insensitive hosts also accept differently capitalized names.

## Sections and precedence

Both forms support `[transforms]`, `[pipelines]`, `[patches]` and `[overrides]`. Their languages
are documented in `CONTENT_TRANSFORMS.md` and `SUPPORTED_FORMATS.md`.
An advanced anchor starts with `[anygm]` and `payload=...`; the basic
single-line payload reference remains supported. The INI does not select a
payload.

Priority runs from lowest to highest:

1. Default sections in `AnyGM.ini`.
2. The unique sibling anchor, when sibling discovery applies.
3. Archive anchors, from the innermost to the outermost archive.
4. The explicitly opened anchor.
5. Inherited launch-anchor overrides during an internal content replacement.
6. Matching `[sha256:<digest>.transforms]`, `[sha256:<digest>.pipelines]`,
   `[sha256:<digest>.patches]` and
   `[sha256:<digest>.overrides]` sections in `AnyGM.ini`.

A digest is exactly 64 hexadecimal digits; uppercase and lowercase digits
identify the same bytes. Section names and transform keys are case-sensitive.
Hash selectors belong only in the system INI. Duplicate recognized sections,
including equivalent digest spellings, reject the INI. Multiple sibling
anchors select none; an explicit anchor does not discover another sibling.

A transform, pipeline or patch replaces the complete entry with the same key, including its
parameters, ordered steps or external-resource declaration. These sections share one namespace; duplicate keys in the same
selection scope are rejected. Overrides replace earlier directives with the same destination,
operation kind and conditions. For example, `$counter=5` replaces `$counter=3`,
but `?gameres $counter=5` and `$counter=3` remain distinct scoped operations.
Indexes distinguish array/list items; camera destinations use the normalized
handle mask and field. A surface resize is one operation with two dimensions.
`ostype`, `renderer`, `gamepad`, `introskip`, `introauto`, `monitorview` and the menu declaration are
singletons within their applicable scope. Menu rows replace the same label;
`keybind` declarations replace the same virtual key, with `auto` restoring its default mapping.
`mset` replaces the same menu variable. Repeated calls to the same script and
condition become one call. Different destinations remain available.

Replacement happens before execution or capturing values for restoration.
The winner occupies its last declaration position. Within one section, the
last override for a destination wins; duplicate transform keys remain invalid.

## Original payload identity

The selector hashes complete original bytes before transformation or
compilation. A data image uses its own bytes; a Classic project uses the
project; a self-contained executable uses the executable. A runner delegating
to adjacent `data.win` uses that data image. ZIP-compatible distribution
wrappers use the selected uncompressed member, including nested archives.
Opening an anchor, renaming a file or changing ZIP compression does not change
this identity. Source inputs routed as `.yyp` or `.yyz` use that source file.
Memory content uses the supplied image bytes.

An explicitly selected `input` function or pipeline can normalize the source before routing.
An optional `input.probe` enumerates bounded candidate ranges in that same original source;
the complete structural reader must select exactly one valid normalized result. An optional
`input.final` pipeline then processes the selected representation, including after a probe's
identity or zero-record decline. These entries
follow the ordinary default/anchor/hash layering, with the contract in `CONTENT_TRANSFORMS.md`.
When its path result differs from the original bytes, the original source's digest remains the
selector through all subsequent parsing, including selection inside a resulting ZIP. This lets
one exact wrapped source select both its adapter and its runtime directives. An absent adapter
or byte-identical path result retains ordinary member/adjacent-payload selection. Memory content
always uses the original supplied image's digest. `CONTENT_TRANSFORMS.md` owns the source-preparation
contract and examples.

An internal `data.alternate.win` code companion does not replace the selected
source identity: opening `data.win` still selects its original hash.

This identifies one file, not its external assets, saves or directory. No
path hash, timestamp or derived-cache fingerprint selects these sections.
File hashing streams through the host VFS with bounded buffers and complete
read checks. With an INI present, logs report the selected file and SHA-256,
default and anchor layers, and the effective directive count.

## Example

The digest and global below are illustrative. Replace the digest with the
intended source file's SHA-256. Keep any existing default transform functions
in `[transforms]`.

```ini
[transforms]
# Default source functions, when required by the selected formats.

[overrides]
ostype|0

[sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef.overrides]
$counter=5
```

An anchor beside the data image can contain:

```ini
[anygm]
payload=data.win

[overrides]
ostype|6
$counter=3
introskip|1,3-5
introstep|78|obj_intro|0
```

The operating-system declaration is 6. The global is held at 5 when the hash
matches, and at 3 otherwise. A global override holds a value while enabled;
it is not merely an initial-value declaration.

## Content renderer selection

An enabled load-time declaration overrides the host's Renderer option:

```ini
[anygm]
payload=data.win

[overrides]
renderer|opengl_nofallback
```

`cpu` selects software, `opengl` selects OpenGL with CPU fallback, and
`opengl_nofallback` selects OpenGL (Experimental), including its ordinary omission of unsupported
draws. `auto` withdraws a lower-priority renderer declaration and uses the host option. Normal
INI/anchor/hash precedence applies. The Content override directives switch must be enabled at
load. The effective policy is frozen until unload: changing that switch or the frontend Renderer
option during play does not renegotiate the context. Close/reopen content to select again.

The declaration requests the same policy as the corresponding frontend option. It cannot supply
an unavailable graphics API and does not change existing context-refusal behavior, pixel reads,
state transport or draw coverage. Preparing hosts read the scene bits in `AnygmContentInfo` before
requesting a context; CPU has no scene bit, and the separate content-GLSL candidate bit retains
its independent meaning. No content event runs before this negotiation.

## CRT-dependent settings

`?crt` scopes a runtime directive to effective CRT adjustment: logical-raster rendering and
Adjust for CRT TV are both enabled. It follows live option changes and Content override directives.
For a game whose scalar setting needs a zero-argument recalculation script, use:

```ini
?crt setcall|screen_mode|1|apply_screen_mode
```

`setcall|global_name|finite_number|script_name` holds that global at the literal numeric value.
It calls the named content script only when the stored value changes, including when disabling
the scope restores the captured original value. The script reads the newly written setting.
Stable frames do not rerun it. Select a recalculation script whose side effects are appropriate
for that setting; this does not replay a menu input event. As with other scoped writes, Reset
captures fresh values and state restoration reconciles the target with the live presentation
policy. The actual global and script names come from the game's own settings implementation.

An optional final `|object_name|refresh_script` pair refreshes cached UI after the setting's
application script. It runs on the first active instance of that object (including descendants),
with the real instance as `self`, no arguments and no `other`. A missing instance skips that
refresh. Both callbacks run only when the numeric setting changes, including its restoration.

Some native settings paths also reposition instances or decorative layers outside their common
application script. A `sync|` prefix on a numeric instance override recomputes the expression
while its `?crt` or `?gameres` scope is active and once on withdrawal. It recalculates from the
current geometry instead of restoring a stale coordinate captured under another layout:

```ini
?crt sync|obj_caption:x=$view_w/2
?crt sync|layer_x|obj_options|decoration_layer|$view_w/2-160
```

The `layer_x`/`layer_y` forms use the first active owner's numeric layer-handle variable and the
existing runtime layer setter. They are valid only within a scoped `sync|` policy. Missing owners,
invalid handles and non-finite values do nothing; unchanged offsets avoid another layer mutation.
Declare synchronization after the setting so its new dimensions are available. Withdrawal includes
Content overrides Off, and state restoration reconciles the synchronized coordinates. This prefix
accepts instance coordinates and layer offsets, not scripts, renderer declarations or arbitrary
state mutations.

An optional `?instance.object_name` condition after the presentation/global conditions requires
an active instance of that object (including descendants). Use it when a layout target is shared
by several rooms. A synchronized layout does not execute its withdrawal calculation after that
owner disappears, so a title-only policy cannot reposition a newly entered room's shared objects.

`$port_w` and `$port_h` read the current `view_wport[0]` and `view_hport[0]`. Use these for
port-relative layout rather than `$view_w`/`$view_h`, which read the authored legacy view arrays:
modern content may resize its camera while leaving those legacy view arrays unchanged.

## Measured state reserve in a sibling anchor

A content-specific state reserve belongs beside the payload in its `.anygm` anchor.
For example, the unique anchor in a directory containing `data.win` can contain:

```ini
[anygm]
payload=data.win

[overrides]
statereserve|33554432
```

Opening `data.win` directly discovers that sibling and applies its directives before
first-frame allocation; opening the anchor explicitly also works. Use the actual payload
filename, including its archive extension when the normal input is a ZIP-compatible wrapper.
Keep one anchor in that directory; merge into an existing anchor instead of adding a second.
Content overrides must be enabled. Do not put this reserve in system INI defaults or hash
sections: the measured allocation stays with the content.

The argument is a positive decimal byte count through 1073741824. Enabled declarations
contribute their maximum as an allocation minimum. They can only raise the core's calculation:
a smaller value cannot reduce its current-state, remembered-peak, mutable-resource or default
allowance, and a later smaller declaration cannot cancel an earlier larger one. The directive
adds no state fields and does not change simulation identity. A fixed frontend can allocate its
ring before the first frame, so install or edit the anchor before loading content; the reserve
does not resize an existing ring or promise that arbitrary future growth will fit.

## Lifecycle and bounds

Configuration is read during preparation. Close/reopen content to reload
file edits. Runtime directives respect the content-override switch; transforms
remain independent load requirements. Host runtime cheats and development
settings retain their separate channels and precedence.

Internal `game_change` loads retain launch-anchor overrides while reselecting
defaults and the replacement's SHA-256 sections. The previous payload's
hash-specific directives never migrate. Transform declarations use the new
payload's own anchors. Cross-payload state restore performs the same selection
before accepting the state transactionally.

Effective override text and the launch-anchor envelope contribute to the
active state configuration fingerprint. Replaced default assignments do not.
The allocation-only `statereserve` declaration is excluded: changing a buffer
minimum does not change simulation identity. Its enabled values contribute their
maximum across layers, so a later lower minimum cannot withdraw a larger one.
Programs, parameters, ordered pipelines and patch declarations contribute to derived-content
cache identity; bindings do not change declaration identity. Neither programs, patch bytes nor
machine paths are serialized. Reset retains parsed configuration
and recreates fresh runtime override state.

Limits are 512 KiB per INI, 256 recognized sections and 4 KiB of override text
per section. Anchors retain their 64 KiB file and 4 KiB override-section bounds.
Collected layers fit 32 KiB. Effective overrides and the launch envelope each
fit 4 KiB and 64 directives. Selected functions, pipelines and patches share the 16-entry limit
and the compiler/interpreter limits in `CONTENT_TRANSFORMS.md`.

The INI accepts UTF-8 BOM and LF, CRLF or CR line endings. Blank lines and
whole-line `#` or `;` comments are ignored outside functions; functions use C
comments. Section-looking text inside a function cannot introduce another
layer. Unknown ordinary sections are ignored; malformed `sha256:` sections
reject the INI. Every transform is validated, including unmatched selectors.
Invalid selected runtime directives reject preparation before live content
is replaced. Failure never publishes partial overrides or transforms.

Focused checks are `make check TEST=content_config`,
`make check TEST=content_patch`,
`make check TEST=content_security`, `make check TEST=engine_content_config` and
`make check TEST=engine_content_patch`. Both engine cases are registered in `make integration-check`.
