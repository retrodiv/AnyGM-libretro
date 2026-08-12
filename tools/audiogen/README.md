<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 retrodiv <retrodiv@proton.me> -->

# Audio configuration generation

The helpers generate Vorbis setup packets and packed codebooks from Xiph's
libogg and libvorbis reference sources. They do not read the checked-in tables
as generation inputs. Xiph data retains its BSD license; original helper and
wrapper contributions are MIT-licensed. See the corresponding notices in
`LICENSES/`.

Run `python3 tools/audiogen/generate.py --help` for arguments. Supply a new
external directory through `--work-dir` and a trusted external JSON file through
`--verification-manifest`. Neither may resolve inside this source directory.
`--source-archives` selects an offline archive directory; otherwise the tool
downloads the three reference releases. `--sanitize` instruments the reference
encoder and helpers. The tool needs Python, CMake and a C compiler.

The verification manifest has `schema` set to 1 and these fields:

- `archives`: expected SHA-256 values keyed by `libogg-1.3.5`,
  `libvorbis-1.2.0` and `libvorbis-1.3.7`.
- `profiles`: 161 objects in recipe order, each containing an `id` with the
  expected eight-digit CRC-32 and a `sha256` with the expected packet digest.
- `packed_sha256`: the expected digest of the packed codebook library.

Obtain these expectations from a trusted source. A manifest is supplied
verification data, not proof of its own authenticity. Concrete digests are
not stored in this source directory. The public catalog retains the encoder
parameters and reference symbol order.

Generation produces all 161 setup packets and 598 packed codebooks. Candidate
headers are written to the external work directory. The default mode compares
them against the checked-in headers and reports drift; `--write` replaces both
headers after reproduction succeeds. A report is written to the external work
directory only after comparison or replacement succeeds. Generated identifiers
use ordinal indices, not fingerprints.

At this historical revision, both generated tables are checked in. The setup
table contains all 161 packets in ordinal recipe order, and the packed
codebook table contains 598 entries. A source-to-output reproduction establishes
the numeric bodies, but the default whole-header comparison can still report
an attribution-only drift against the previously adapted setup-table header.
Treat that result as a failed whole-header comparison and retain its exact
diagnostic in external verification evidence. Generation and replacement do
not by themselves establish runtime compatibility.
