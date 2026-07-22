<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 retrodiv <retrodiv@proton.me> -->

# Generated-data provenance

Generated data checked into the core must be immutable at runtime, legally
redistributable, and traceable without a proprietary content payload. Concrete
artifact digests are intentionally kept in external verification records rather
than in this public repository.

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

## Embedded glyph coverage

`src/generated/gml_default_font_data.h` and
`src/generated/gml_classic_info_font_data.h` contain raster coverage and metrics
derived from Liberation Sans 2.1.5. Their headers identify the source and
rendering conditions, while `LICENSES/font-ofl-1.1.txt` retains the SIL Open Font
License and upstream copyright notices. The font programs themselves are not
bundled.

This revision does not include a font-data generator. The checked-in headers and
license record establish the declared source and terms, but this tree alone does
not provide byte-for-byte regeneration evidence for those two artifacts.

## Publication and validation

Generated data is kept under `src/generated/` and is not regenerated as part of
an ordinary core build. A source release should be checked against its external
verification record, and every binary distribution must carry the applicable
third-party notices and license texts described in
`THIRD_PARTY_NOTICES.md`.
