<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 retrodiv <retrodiv@proton.me> -->

# Content configuration

The optional `AnyGM.ini` in the frontend system directory supplies content
defaults. An advanced `.anygm` anchor adds or replaces declarations for its
content. The INI supplies global defaults and optional fast payload selectors.
Content-local declarations can also live in `.anygm` anchors.

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
6. Matching fast payload sections in `AnyGM.ini`.

Section names and transform keys are case-sensitive. Duplicate recognized sections
reject the INI. Unknown selector prefixes also reject the INI rather
than silently losing a content-specific policy. Multiple sibling
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

## Preparation and cache identity

Complete-file SHA-256 selectors remain unsupported. Their full input reads were
removed from ordinary startup. Fast recognition supports one section prefix,
followed by `.overrides`, `.transforms`, `.pipelines` or `.patches`:

`imohash:<size>:<digest>` identifies a path-backed payload using standard-default
[imohash](https://github.com/kalafut/imohash). The digest is 32 hexadecimal digits. Sizes are positive
decimal integers without leading zeroes; hexadecimal digits are case insensitive.

For Studio, selection follows the actual data image, including an adjacent or
extracted embedded image reached through a launcher. Classic projects and
executables identify their original input before conversion. Loose and archived
copies of the same payload share one key and one INI rule. Archive payloads are
sampled from their extracted files, including with a warm cache. The archive itself,
its compression and unrelated members do not contribute to the key. Nested archives
select the final payload member. No archive-specific selector exists.

Selection first filters declared candidates by size; with no candidate, no hash
is calculated. Standard imohash reads the entire payload below 131072 bytes.
For larger payloads it reads 16384 bytes at offsets 0, floor(size/2) and
size-16384, in that order: 48 KiB total. MurmurHash3 x64_128 with seed zero
hashes the selected bytes. Each 64-bit output lane is written big-endian;
the unsigned LEB128 payload size overwrites the start of the 16-byte digest.
There is no custom hashing domain or configurable sampling variant.
Memory-only loads retain defaults; these selectors are path-backed recognition.
See [provenance and licenses](PROVENANCE.md#imohash-recognition).

These are recognition hints, not proof of complete-file equality. Different
payloads can share them. Duplicate equivalent sections reject the INI, and every
section is validated even when unmatched. No content database is bundled.
Archive extraction CRC checks and exact patch source/result SHA-256 validation
remain independent of recognition.

The archive-extraction, embedded Studio-image, Classic conversion and source-project
caches use the same default imohash for source and cached-payload identity.
Schema-2 markers retain all 128 bits. Directory names retain their input stem
and cache-kind suffix, with an eight-digit lowercase hexadecimal locator between
them, for example `Example-01234567-anygm-archive`. Classic caches also use one
eight-digit locator, rather than separate source and dependency fields.
Prepared inputs use `input-01234567`.

Locators hash the complete fingerprint, size, kind and dependency context with
standard imohash and take the final four digest bytes; they do not truncate the
encoded-size prefix of the original fingerprint. The complete marker identity
is checked before selecting an occupied directory. A different identity or an
unreadable marker selects a deterministic alternative eight-digit locator, with
a limit of 64 candidates, without overwriting that directory. Invalid unowned
entries remain disposable and are removed by the normal cache-clearing controls.
The short locator is never an INI selector or an integrity check.

Sizes, producer identity and the applicable transformation/dependency signatures
also participate in reuse. Loose
source projects are rebuilt because their external dependencies can change.
Prepared input-adapter images likewise use imohash identities after preparation
and patch validation have completed. Small metadata and dependency signatures
retain their existing algorithms; this does not replace every hash in the loader.

These disposable caches use sampled checks, not complete-byte integrity proofs:
a same-size modification outside the sampled regions may go undetected. Clear the
content cache if its contents are suspect. Older cache formats are regenerated on
the first load. Cabinet installation manifests retain their separate verification
contract. Reading or converting content may still require the complete input even
though computing its cache identity takes only the samples described above.

Payload selectors cannot declare pre-routing `input`, `input.probe` or `input.final`
adapters: those must be available before payload selection and belong in defaults
or anchors. Other selected transforms and patches retain their ordinary contracts.

An illustrative sampled key can select runtime directives:

```ini
[imohash:123456:0123456789abcdef0123456789abcdef.overrides]
ostype|0
```

Compute the key from the actual payload; a size alone is never a selector.

### Obtain a selector without additional tools

The following standalone recipe requires only Python 3 and its standard library.
Save it as `content_selector.py`. It reads the selected input and prints a complete
`[...overrides]` section header; it does not modify the input or `AnyGM.ini`.

```python
# imohash adaptation: Jim Kalafut (MIT); license follows.
# The MIT License (MIT)
#
# Copyright (c) 2015 Jim Kalafut
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.
# MurmurHash3 was written by Austin Appleby, and is placed in the public
# domain. The author hereby disclaims copyright to this source code.
# C port and general tidying by Peter Scott; adapted here to Python.

import argparse
import zipfile
from pathlib import Path
from typing import BinaryIO


def _murmur128(data: bytes) -> bytes:
    """MurmurHash3 x64_128, seed zero, with imohash's big-endian lanes.

    Adapted from Austin Appleby's public-domain algorithm and Peter Scott's C
    port. See THIRD_PARTY_NOTICES.md for source pins and the original notice.
    """
    mask = (1 << 64) - 1

    def rotate(value, count):
        return ((value << count) | (value >> (64 - count))) & mask

    def mix(value):
        value = ((value ^ (value >> 33)) * 0xff51afd7ed558ccd) & mask
        value = ((value ^ (value >> 33)) * 0xc4ceb9fe1a85ec53) & mask
        return value ^ (value >> 33)

    c1, c2 = 0x87c37b91114253d5, 0x4cf5ad432745937f
    h1 = h2 = 0
    end = len(data) // 16 * 16
    for offset in range(0, end, 16):
        k1 = int.from_bytes(data[offset:offset + 8], "little")
        k2 = int.from_bytes(data[offset + 8:offset + 16], "little")
        h1 ^= (rotate(k1 * c1 & mask, 31) * c2) & mask
        h1 = ((rotate(h1, 27) + h2) * 5 + 0x52dce729) & mask
        h2 ^= (rotate(k2 * c2 & mask, 33) * c1) & mask
        h2 = ((rotate(h2, 31) + h1) * 5 + 0x38495ab5) & mask
    tail = data[end:]
    if len(tail) > 8:
        k2 = int.from_bytes(tail[8:], "little")
        h2 ^= (rotate(k2 * c2 & mask, 33) * c1) & mask
    if tail:
        k1 = int.from_bytes(tail[:8], "little")
        h1 ^= (rotate(k1 * c1 & mask, 31) * c2) & mask
    h1 ^= len(data)
    h2 ^= len(data)
    h1 = (h1 + h2) & mask
    h2 = (h2 + h1) & mask
    h1, h2 = mix(h1), mix(h2)
    h1 = (h1 + h2) & mask
    h2 = (h2 + h1) & mask
    return h1.to_bytes(8, "big") + h2.to_bytes(8, "big")


def imohash(source: BinaryIO, size: int) -> str:
    """Standard default imohash; see Jim Kalafut's MIT grant in LICENSES/."""
    if not 0 <= size <= (1 << 64) - 1:
        raise ValueError("payload size is outside the unsigned 64-bit range")
    count = size if size < 131072 else 16384
    offsets = (0,) if size < 131072 else (0, size // 2, size - count)
    samples = []
    for offset in offsets:
        source.seek(offset)
        sample = source.read(count)
        if len(sample) != count:
            raise ValueError("payload changed or a sample could not be read")
        samples.append(sample)
    digest = bytearray(_murmur128(b"".join(samples)))
    remaining, index = size, 0
    while True:
        digest[index] = (remaining & 127) | (128 if remaining >= 128 else 0)
        remaining >>= 7
        index += 1
        if not remaining:
            return digest.hex()


def _sample_key(source: BinaryIO, size: int) -> str:
    if not size:
        raise ValueError("payload is empty")
    return f"imohash:{size}:{imohash(source, size)}"


def make_key(path: Path, member: str | None = None) -> str:
    if member is not None:
        with zipfile.ZipFile(path) as archive:
            matches = [entry for entry in archive.infolist() if entry.filename == member]
            if len(matches) != 1 or matches[0].is_dir() or matches[0].file_size == 0:
                raise ValueError("select exactly one nonempty payload member")
            entry = matches[0]
            with archive.open(entry) as source:
                return _sample_key(source, entry.file_size)
    if zipfile.is_zipfile(path):
        raise ValueError("archives require --member naming the actual payload")
    with path.open("rb") as source:
        source.seek(0, 2)
        size = source.tell()
        key = _sample_key(source, size)
        source.seek(0, 2)
        if source.tell() != size:
            raise ValueError("payload size changed during sampling")
    return key


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Print an imohash payload INI section")
    parser.add_argument("path", type=Path)
    parser.add_argument("--member", help="Exact payload member path inside a ZIP")
    args = parser.parse_args()
    try:
        print(f"[{make_key(args.path, args.member)}.overrides]")
    except (OSError, ValueError, zipfile.BadZipFile) as error:
        parser.error(str(error))
```

For ZIP-compatible content, first list the member paths and then name the actual
payload, preserving its spelling and any directory prefix:

```sh
python3 -m zipfile -l "Content.zip"
python3 content_selector.py "Content.zip" --member "data.win"
python3 content_selector.py "Content.apk" --member "assets/game.droid"
```

This mode samples the selected member's uncompressed bytes and produces the same
`imohash` key as the loose payload. Python may need to decompress that member to
reach the samples; this is a one-time authoring operation, not an extra core load
pass. Other members are not decompressed. For nested archives, run it on the
innermost archive that contains the selected payload. Do not hash the outer
archive or an executable that merely launches a Studio data image.

For loose content, select the actual Studio data image or the original Classic
project/executable:

```sh
python3 content_selector.py "data.win"
python3 content_selector.py "game.unx"
python3 content_selector.py "game.droid"
python3 content_selector.py "Project.gm81"
python3 content_selector.py "Classic.exe"
```

On Windows, replace `python3` with `py -3` if using the Python launcher. When a
Studio executable delegates to adjacent data, pass that data file. For an embedded
Studio image, use the extracted data image identified by the core's cache/load
logs. A Classic executable is sampled directly. An ordinary whole-file SHA-256
command does not produce an `imohash` key.

Copy the printed header into the frontend system directory's `AnyGM.ini`, then add
the desired directives below it. For example, an illustrative output and directive:

```ini
[imohash:123456:0123456789abcdef0123456789abcdef.overrides]
ostype|0
```

Use the header printed for your own payload, rather than these illustrative
numbers. The same section works for the loose payload and any archive carrying
those same payload bytes. Recompute the
key when updating the content. With a matching rule present, load diagnostics show
`configuration: recognition ... (matched)` and the selected payload path. These
keys retain the recognition limitations described above.

### Source preparation

An explicitly selected `input` function or pipeline can normalize the source before routing.
An optional `input.probe` enumerates bounded candidate ranges in that same original source;
the complete structural reader must select exactly one valid normalized result. An optional
`input.final` pipeline then processes the selected representation, including after a probe's
identity or zero-record decline. These entries
follow the ordinary default/anchor layering, with the contract in `CONTENT_TRANSFORMS.md`.
An absent adapter or byte-identical path result retains ordinary member/adjacent-payload
selection. A changed path result retains original-byte identity only for its cache key,
not for selecting runtime directives. `CONTENT_TRANSFORMS.md` owns that preparation
contract. Logs report default and anchor layers and the effective directive count.

## Example

Keep any required default transform functions in `[transforms]`.

```ini
[transforms]
# Default source functions, when required by the selected formats.

[overrides]
ostype|0
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

The operating-system declaration is 6 and the anchor holds the global at 3,
overriding the INI default of 5. A global override holds a value while enabled;
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
INI/anchor precedence applies. The Content override directives switch must be enabled at
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

Internal `game_change` loads retain launch-anchor overrides while collecting
defaults and the replacement's own anchors. Transform declarations use the new
payload's own anchors. Cross-payload state restore performs the same collection
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
layer. Unknown ordinary sections are ignored; all removed `sha256:` sections
reject the INI. Every declared transform is validated.
Invalid selected runtime directives reject preparation before live content
is replaced. Failure never publishes partial overrides or transforms.

Focused checks are `make check TEST=content_config`,
`make check TEST=content_patch`,
`make check TEST=content_security`, `make check TEST=engine_content_config` and
`make check TEST=engine_content_patch`. Both engine cases are registered in `make integration-check`.
