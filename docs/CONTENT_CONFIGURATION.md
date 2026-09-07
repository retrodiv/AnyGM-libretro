<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 retrodiv <retrodiv@proton.me> -->

# Content configuration

The optional `anygm.ini` in the frontend system directory supplies content
defaults. An advanced `.anygm` anchor adds or replaces declarations. The INI
can also target exact original payload bytes by SHA-256. All identities and
rules come from external configuration; the core ships none of its own.

## Sections and precedence

Both forms support `[transforms]`, `[pipelines]` and `[overrides]`. Their languages
are documented in `CONTENT_TRANSFORMS.md` and `SUPPORTED_FORMATS.md`.
An advanced anchor starts with `[anygm]` and `payload=...`; the basic
single-line payload reference remains supported. The INI does not select a
payload.

Priority runs from lowest to highest:

1. Default sections in `anygm.ini`.
2. The unique sibling anchor, when sibling discovery applies.
3. Archive anchors, from the innermost to the outermost archive.
4. The explicitly opened anchor.
5. Inherited launch-anchor overrides during an internal content replacement.
6. Matching `[sha256:<digest>.transforms]`, `[sha256:<digest>.pipelines]` and
   `[sha256:<digest>.overrides]` sections in `anygm.ini`.

A digest is exactly 64 hexadecimal digits; uppercase and lowercase digits
identify the same bytes. Section names and transform keys are case-sensitive.
Hash selectors belong only in the system INI. Duplicate recognized sections,
including equivalent digest spellings, reject the INI. Multiple sibling
anchors select none; an explicit anchor does not discover another sibling.

A transform or pipeline replaces the complete entry with the same key, including its
parameters or ordered steps. Both sections share one namespace; duplicate keys in the same
selection scope are rejected. Overrides replace earlier directives with the same destination,
operation kind and conditions. For example, `$counter=5` replaces `$counter=3`,
but `?gameres $counter=5` and `$counter=3` remain distinct scoped operations.
Indexes distinguish array/list items; camera destinations use the normalized
handle mask and field. A surface resize is one operation with two dimensions.
`ostype`, `introskip`, `introauto`, `monitorview` and the menu declaration are
singletons within their applicable scope. Menu rows replace the same label;
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
the complete structural reader must select exactly one valid normalized result. Both entries
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
```

The operating-system declaration is 6. The global is held at 5 when the hash
matches, and at 3 otherwise. A global override holds a value while enabled;
it is not merely an initial-value declaration.

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
Programs and parameters contribute to derived-content cache identity; neither
programs nor machine paths are serialized. Reset retains parsed configuration
and recreates fresh runtime override state.

Limits are 512 KiB per INI, 256 recognized sections and 4 KiB of override text
per section. Anchors retain their 64 KiB file and 4 KiB override-section bounds.
Collected layers fit 32 KiB. Effective overrides and the launch envelope each
fit 4 KiB and 64 directives. Selected functions and pipelines share the 16-entry limit
and the compiler/interpreter limits in `CONTENT_TRANSFORMS.md`.

The INI accepts UTF-8 BOM and LF, CRLF or CR line endings. Blank lines and
whole-line `#` or `;` comments are ignored outside functions; functions use C
comments. Section-looking text inside a function cannot introduce another
layer. Unknown ordinary sections are ignored; malformed `sha256:` sections
reject the INI. Every transform is validated, including unmatched selectors.
Invalid selected runtime directives reject preparation before live content
is replaced. Failure never publishes partial overrides or transforms.

Focused checks are `make check TEST=content_config`,
`make check TEST=content_security` and `make check TEST=engine_content_config`.
The engine case is also registered in `make integration-check`.
