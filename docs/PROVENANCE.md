<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 retrodiv <retrodiv@proton.me> -->

# Generated-data provenance

## Removed libarchive Cabinet read closure

Earlier revisions retained a bounded CAB/LZX reader from the official libarchive
3.8.9 release. This revision removes every retained libarchive source and its
license file, and no longer compiles that closure. Its notices remain in the
earlier source revisions. The exact release and file comparisons are retained
in private verification evidence rather than embedded as artifact fingerprints.

The checked-in LZX-21 Cabinet fixture contains only a first-party synthetic
normalized payload and a neutral text asset. Its generation and comparison
evidence remain in private records.

Generated data checked into the core must be immutable at runtime, legally
redistributable, and traceable without a proprietary content payload. Concrete
artifact digests remain outside this public repository.

## Exact builtin lookup index

`src/generated/gml_builtin_registry_index.h` is a first-party, data-only
open-addressed index derived exclusively from the canonical rows in
`src/runtime/builtins/gml_builtin_registry.h`. It contains no external content
and has no license dependency beyond the repository's MIT-licensed source.

Regenerate it deterministically with:

```sh
python3 tests/architecture/check_builtin_registry.py generate
```

`make builtin-registry-check` independently reconstructs the index, compares
it byte-for-byte, and also proves that the canonical exact-name set matches the
implementation branches. A production build consumes the immutable header and
does not run the generator. Concrete verification values belong in external
release evidence rather than this repository.

## Audio setup packets

`src/generated/audio_setup_data.h` contains 161 Vorbis setup packets generated
from the official Xiph libvorbis 1.3.7 source release. The corresponding BSD
terms and source codebook notices are retained in
`LICENSES/libvorbis-1.3.7.txt`.

The packets total 619,163 bytes. `tools/audiogen/recipes.json` records the
encoder configuration, block sizes, packet sizes, and source-symbol order
without embedding artifact fingerprints. The generator accepts expected
archive, packet, and packed-library values through an external verification
manifest. It builds from the Xiph libogg and libvorbis releases, writes
candidates outside the source tree, and compares complete generated output
before an optional replacement.

The setup arrays use ordinal identifiers. Each table entry stores the CRC-32
of its generated setup packet for runtime lookup; the catalog check verifies
the key against the packet bytes. No encoder is linked into the core.

## Wwise Vorbis codebooks

`src/generated/wwise_codebooks.h` is generated from named `static_codebook`
declarations in the official Xiph libvorbis 1.2.0 source release. The source
codebook notices and BSD license are retained in
`LICENSES/libvorbis-1.2.0.txt`. The recipe associates 598 wire identifiers
with named source declarations; `emit_books.c` reads those declarations and
`packed_books.py` encodes the compact fields and offset table. The resulting
74,164-byte catalog is immutable and is consumed in memory. Its data license
is Xiph's, separate from the MIT-licensed first-party generator and wrapper.
The source-to-output byte comparison and concrete verification values are
kept in external evidence, not this repository.

## ww2ogg adaptation

The in-memory Wwise Vorbis reconstruction in `src/third_party/ww2ogg/` is
adapted from [ww2ogg](https://github.com/hcs64/ww2ogg) under its retained
BSD-style terms in `LICENSES/ww2ogg.txt`. The `codebook_library` constructor
takes a byte range, `Wwise_RIFF_Vorbis` reads RIFF and catalog bytes from
memory, and `generate_ogg` writes to an output stream. The CRC files are
unmodified upstream copies; `Bit_stream.h` and `errors.h` differ only in
trailing whitespace. The upstream revision and file-by-file comparison values
are recorded in external verification evidence.

## Vendored third-party sources

The repository includes these source components with their upstream notices:

- bzip2 1.0.8 under `src/third_party/bzip2/`;
- stb image, image write, TrueType, and Vorbis single-file libraries under
  `src/third_party/stb/`;
- minimp3 headers under `src/third_party/minimp3/`; and
- the libretro API header under `src/third_party/libretro/`.

The applicable terms are preserved in the files themselves and in `LICENSES/`.
The bzip2 files are recorded as an unmodified copy of release 1.0.8. Version
labels and license texts identify the remaining vendored inputs; concrete
verification values belong in external release evidence.

The vendored `minimp3_ex.h` carries a marked local change in
`mp3dec_detect_cb`: it initializes the free-format frame-size pair before
`mp3d_find_frame` reads it. The mark preserves the modification's provenance
for reviewers; the byte comparison belongs in external verification evidence.

## Embedded glyph coverage

`src/generated/gml_default_font_data.h` and
`src/generated/gml_classic_info_font_data.h` contain raster coverage and metrics
derived from Liberation Sans 2.1.5. Their headers identify the source and
rendering conditions, while `LICENSES/font-ofl-1.1.txt` retains the SIL Open Font
License and upstream copyright notices. The font programs themselves are not
bundled.

`src/generated/gml_studio_default_font_data.h` contains fallback glyph
coverage and metrics derived from Roboto Mono Medium in the
[`v3.001` upstream release](https://github.com/googlefonts/RobotoMono/tree/v3.001).
The first-party generator `tools/fontgen/gen_studio_font.c` uses the vendored
stb TrueType rasterizer with a fixed nine-pixel advance, 20-row line box,
baseline at row 16, and code points 32 through 127. The source font program
is not bundled. Its derived coverage remains under the SIL Open Font
License in `LICENSES/font-roboto-mono-ofl-1.1.txt`; the first-party
generator and C declarations retain their separate MIT terms. Concrete
input and output verification values belong in external evidence, not in
this repository.

This revision still does not include generators for the two Liberation
headers. Their checked-in declarations and licence record identify the
declared source and terms, but this tree alone does not provide
byte-for-byte regeneration evidence for those two artifacts.

## Interoperability constants

A small number of literal constants in first-party source are facts of the
formats being read rather than derived data. They are recorded here so their
origin is documented deliberately instead of discovered.

- **FMOD Studio bank layout** (`src/audio/banks/gml_fmod.c`). The FEV metadata
  reader is adapted from the public
  [FModBankParser](https://github.com/Masusder/FModBankParser) project. Its
  reading order, element-list encodings and node kinds inform the C adaptation;
  the upstream Apache-2.0 grant and notice remain in `LICENSES/` and `NOTICE`.
  Project-original contributions retain their separate MIT terms.
- **GLSL effect-recognition patterns** (`src/video/renderer/`). Renderer
  policy records describe narrow shader families through structural
  operations and parameterized identifiers. The repository does not bundle
  content shader programs. This historical revision does not yet provide a
  complete recognition implementation.
- **GMS2 shader preambles**
  (`src/content/project/gmlc_package_chunks.c`). The gm_* uniform names,
  MATRIX_*/MAX_VS_LIGHTS macros, helper-function signatures and the
  `_YY_GLSLES_`/`_YY_GLSL_` dialect markers form an interoperability interface
  for authored shaders. The preamble function bodies and formatting here are
  first-party implementations; no proprietary shader program is bundled.

## Graphics entry-point declarations

`src/video/gpu/gml_gpu_gl_api.h` is a first-party declaration set for the
OpenGL entry points the optional graphics backend uses. It names the required
type widths, enumerants, and function signatures without incorporating a
platform loader. The host supplies a callback that resolves each entry point
for its current context; the core itself links no graphics library. Keeping
the declaration set in one owner also makes the graphics boundary check
enforceable. These values describe the Khronos interface, not a copied
implementation.

`src/video/gpu/gml_gpu_gl_shaders.h` contains the two first-party shader
bodies compiled by that backend. They are checked in so an ordinary
production build neither generates them nor accesses a network.

## Publication and validation

Generated data is kept under `src/generated/` and is not regenerated as part of
an ordinary core build. A source release should be checked against its external
verification record, and every binary distribution must carry the applicable
third-party notices and license texts described in
`THIRD_PARTY_NOTICES.md`.
