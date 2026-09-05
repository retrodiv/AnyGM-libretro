<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 retrodiv <retrodiv@proton.me> -->

# External content transforms

The bounded transform interpreter operates only on supplied byte buffers. It contains no
format-specific program, native-code loader, filesystem instruction, network instruction,
or implicit fallback.

## Configuration representation

A `[transforms]` section contains `name=program-hex:parameter-hex` declarations. Names are
case-sensitive ASCII lowercase letters, digits, dots, underscores, and hyphens, with a maximum
of 63 bytes. Hexadecimal is case-insensitive. The colon is mandatory; empty parameters are
allowed. Spaces around the name and value are ignored, but not within hexadecimal data.
Blank lines and whole-line `#` or `;` comments are ignored. Other sections are not interpreted
by the transform parser. Duplicate names or duplicate transform sections in one document fail.

The frontend's system directory optionally supplies `anygm.ini`; no working-directory or
content-directory file of that name is guessed. Libretro obtains this directory through
`RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY`; portable hosts set `AnygmContentSource.system_directory`.
No file means no default programs. A present, readable but malformed file rejects the load.
Hosts whose stat service cannot distinguish an absent file from an inaccessible one supply no
defaults in either case; content that requires a program still fails without one.

An advanced `.anygm` anchor may carry the same `[transforms]` section after its `[anygm]` payload
selection. Each declared entry replaces the lower-priority entry completely, including parameters;
undeclared entries remain available. Precedence is an explicitly opened anchor, then outer-to-inner
archive anchors, then the lone sibling anchor, then `anygm.ini`. Multiple sibling anchors select
none. Transform requirements do not depend on the runtime-overrides option. An unreadable selected
sibling anchor rejects resolution rather than silently selecting another program.

A malformed layer changes nothing. Program and parameter identities both contribute to a
canonical, order-independent SHA-256 digest and the normalized-content cache key. Changing either
invalidates that cached representation. Programs and host system paths are not serialized into
savestates. Internal replacement loads retain the system directory but select the replacement's
own anchors; a launch anchor's transform declarations are not implicitly inherited by another
payload.

The following synthetic program returns the supplied buffer unchanged:

```ini
[transforms]
identity=00001f000000000000000000:
```

Limits are 512 KiB per configuration, 16 programs, 12 KiB per program, and 4 KiB of parameters
per program. These are limits, not allocations derived from untrusted length words.

## Instruction format

Each instruction is exactly twelve bytes: `opcode, d, a, b, immediate[8]`. The immediate is
unsigned little-endian. There are 32 unsigned 64-bit registers. Initially `r0` is the input
length, `r1` is the parameter length, and all other registers are zero. Arithmetic wraps at
64 bits; programs requiring narrower words must mask explicitly. Comparisons are unsigned.

| Opcode | Operation |
| --- | --- |
| 0 | Return work-buffer slice at `r[a]`, length `r[b]`; `d=0` |
| 1 | Reject input; `d=a=b=0` |
| 2 | `r[d]=immediate`; `a=b=0` |
| 3 | `r[d]=r[a]`; `b=0` |
| 4–15 | `r[d]=r[a] OP r[b]`, respectively add, subtract, multiply, divide, remainder, AND, OR, XOR, left shift, right shift, equality, unsigned less-than |
| 16–18 | Load 1, 4, or 8 little-endian bytes into `r[d]` at `r[a]+immediate` from memory space `b` |
| 19–21 | Store the low 1, 4, or 8 bytes of `r[d]` at `r[a]+immediate` in memory space `b` |
| 22 | Jump to instruction `immediate`; `d=a=b=0` |
| 23–24 | Jump to instruction `immediate` when `r[a]` is zero/nonzero; `d=b=0` |

Immediates must be zero outside constant, memory, and jump instructions. All register fields
must be below 32 even when unused. The four memory spaces are input (0, read-only), work (1,
input-sized private copy), parameters (2, read-only), and zero-initialized 64 KiB scratch (3).
Only work and scratch permit stores. Output must fit entirely inside work; no instruction
grows a buffer. Returned bytes are owned by the caller.

## Rejection and execution bounds

The complete program is structurally validated before any instruction runs, including
unreachable instructions. Unknown operations, invalid registers, invalid memory spaces, stores
to read-only spaces, misaligned program lengths, and out-of-range jump targets fail closed.
Every load/store checks address overflow and the complete access width. Division by zero,
shifts of 64 or more, an invalid output slice, falling off the program, and exhausting the
instruction budget reject the entire result. Input and parameters are unchanged on success
and failure, and failure never returns partial output.

Inputs are bounded to 1 GiB. Execution allows at most `1,000,000 + 128 * input_bytes`
instructions; a caller may lower but cannot raise that budget. The absolute ceiling follows
from the input-size limit rather than truncating valid linear work on large buffers. Allocation
is limited to one input-sized work copy and the fixed scratch buffer. These interpreter bounds
bound work but do not promise a short wall-clock deadline. They do not make the rest of the runtime
a process sandbox; hosts still own process isolation and
scheduling as described in `SECURITY_MODEL.md`.

## Parser boundaries

The revision-selected adaptation interface is omitted from this unpublished
history. Earlier snapshots with omitted implementations are not supported builds.

`make check TEST=content_transform` exercises synthetic arithmetic, buffer isolation, malformed
programs, execution exhaustion, missing programs, transactional overrides, and identity changes.
