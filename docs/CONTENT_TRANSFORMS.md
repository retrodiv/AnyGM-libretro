<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 retrodiv <retrodiv@proton.me> -->

# External content transforms

The bounded transform interpreter operates only on supplied byte buffers. It contains no
format-specific program, native-code loader, filesystem instruction, network instruction,
or implicit fallback.

## Configuration representation

A `[transforms]` section contains `name = buffer function_name() { ... }` declarations.
The function may span multiple lines and is compiled at configuration load into the existing
bounded instruction interpreter. The INI key selects the transform; the descriptive function
name does not affect dispatch. Keys are case-sensitive ASCII lowercase letters, digits, dots,
underscores, and hyphens, with a maximum of 63 bytes.

Blank lines and whole-line `#` or `;` comments outside functions are ignored. Inside a function,
use C-style `//` or `/* ... */` comments. The closing brace must end its declaration line,
apart from spaces or tabs. Other INI sections are not interpreted by the transform parser.
Duplicate keys or duplicate transform sections in one document fail. UTF-8 BOM and LF, CRLF,
and CR line endings are accepted. The former hexadecimal declaration syntax is rejected;
configuration files must contain source functions.

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

The following synthetic function returns the supplied buffer unchanged:

```ini
[transforms]
identity = buffer copy_input() {
    return slice(0, input_size);
}
```

An optional first statement supplies read-only parameter bytes as a string. This synthetic
example edits each work byte with a repeating parameter, keeping input unchanged:

```ini
[transforms]
example = buffer edit_bytes() {
    parameters("ABC");
    for (uint64_t offset = 0; offset < input_size; offset++) {
        uint64_t value = read8(input, offset);
        uint64_t parameter = read8(parameters, offset % parameter_size);
        write8(work, offset, value ^ parameter);
    }
    return slice(0, input_size);
}
```

## Source language

Each entry defines one `buffer name()` function with no arguments. Available statements are
initialized `uint64_t` local declarations, assignments, `if`/`else`, `while`, `for`, `break`,
`continue`, braced blocks, `reject()`, memory writes, and `return slice(offset, length)`.
Local scope follows blocks and loop declarations; shadowing a visible name is rejected.
There are no uninitialized declarations. Falling through the function rejects the input.

Values are unsigned 64-bit integers, including literals and character literals. Arithmetic
wraps at 64 bits; narrower arithmetic needs an explicit mask. Decimal and hexadecimal literals
are accepted without suffixes; octal and floating-point literals are rejected. `true` and `false`
are 1 and 0. Operators use C precedence and associativity: unary `+ - ! ~`, arithmetic
`+ - * / %`, shifts `<< >>`, comparisons `== != < <= > >=`, bitwise `& ^ |`, and logical
`&& ||`. Logical operations normalize to 0 or 1 and short-circuit. Comparisons are unsigned.
Assignments support `= += -= *= /= %= &= |= ^=`; statement updates also support `name++`
and `name--`. In a `for`, initialization and update each accept one assignment or update;
initialization may instead declare a local. Empty clauses are allowed.

`input_size` and `parameter_size` are read-only byte counts. `read8`, `read32`, and `read64`
take a memory name and byte offset; `write8`, `write32`, and `write64` also take a value.
Multi-byte accesses are little-endian and need no alignment. Memory names are `input`
(read-only), `work` (an input-sized private copy), `parameters` (read-only), and `scratch`
(zero-initialized, 64 KiB). Writes store the low bits of the value. `slice` returns a byte
range from work, with ownership transferred to the caller only on success.

`parameters("...");` is optional and allowed only before executable statements in the
function. Strings and character literals accept printable ASCII and the escapes `\n`, `\r`,
`\t`, `\0`, `\\`, `\"`, `\'`, and `\xHH` (exactly two hexadecimal digits). There is no
implicit terminating zero byte. Parameters are part of the complete entry, so overriding a
function also replaces its parameters. User-function calls, pointers, arrays, casts, includes,
allocation, native functions and host capabilities are unavailable.

Limits are 512 KiB per configuration, 16 programs, 64 KiB of source per function, 12 KiB of
compiled instructions, and 4 KiB of parameters per program. The compiler bounds syntax nesting
to 64 levels and `break` sites to 128 per loop. Its 32 registers hold the two read-only sizes,
live locals, and expression temporaries; overly complex expressions or too many live variables
are rejected. Compilation validates all source, including unreachable code, before selecting
an entry. Diagnostics identify the transform key and line/column relative to its function value.
Whitespace, comments and local/function names do not affect the compiled program identity.

## Internal instruction format

Instructions are an internal compilation result, not INI syntax. The interpreter and direct
execution contract remain available for synthetic low-level validation.

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

`make check TEST=content_transform` exercises source compilation, scopes, short-circuit control
flow, diagnostics, arithmetic, buffer isolation, malformed source/instructions, execution
exhaustion, missing programs, transactional overrides, and identity changes. Content-security
tests also prove that section-shaped text inside source comments cannot become anchor overrides.
