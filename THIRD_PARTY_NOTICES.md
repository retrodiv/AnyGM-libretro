<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 retrodiv <retrodiv@proton.me> -->

# Third-party notices

AnyGM first-party code is licensed under the repository MIT License. Bundled
third-party components and derived data retain their own terms and notices,
including their original copyright years.

| Component | Location | Terms |
| --- | --- | --- |
| bzip2 | `src/third_party/bzip2/` | bzip2 license in `LICENSES/bzip2.txt` |
| libretro API header | `src/third_party/libretro/include/libretro.h` | MIT terms in `LICENSES/libretro.txt` |
| minimp3 | `src/third_party/minimp3/` | CC0 terms in `LICENSES/minimp3.txt` |
| stb image, image write, TrueType, and Vorbis | `src/third_party/stb/` | MIT option in `LICENSES/stb.txt` |
| Liberation Sans-derived glyph coverage | `src/generated/gml_default_font_data.h`, `src/generated/gml_classic_info_font_data.h` | SIL Open Font License in `LICENSES/font-ofl-1.1.txt` |
| Xiph libvorbis-derived Vorbis setup packets | `src/generated/audio_setup_data.h` | BSD terms in `LICENSES/libvorbis-1.3.7.txt` |
| Xiph libogg used by the optional audio-data generator | `tools/audiogen/` build-time dependency, not linked into the core | BSD terms in `LICENSES/libogg.txt` |
| FMOD Studio bank metadata reader | `src/audio/banks/gml_fmod.c` | Adapted portions retain Apache-2.0 terms; see `NOTICE`, `LICENSES/fmodbankparser.txt`, and `LICENSES/fmodbankparser-NOTICE.txt` |

The Liberation-derived headers contain raster coverage and metrics, not the
font programs. Their source and rendering conditions are recorded in their
headers and in [Generated-data provenance](docs/PROVENANCE.md).

The Vorbis setup packets are generated from licensed Xiph sources. The optional
generator accepts verification values from a required external manifest and
does not use an existing setup table, a sound bank, middleware binary, or
decoded audio as a generation input. Runtime lookup identifiers are calculated
from the generated packet bytes and are not stored as a concrete fingerprint
catalog in this repository.

The bzip2 source headers refer to material in the upstream distribution. The
complete license is retained locally, and the repository's `NOTICE` identifies
the upstream release and source location.

This revision does not embed the notice bundle into the compiled core. A binary
distributed without this source repository must therefore be accompanied by
`THIRD_PARTY_NOTICES.md`, `NOTICE`, and the applicable files under `LICENSES/`.
