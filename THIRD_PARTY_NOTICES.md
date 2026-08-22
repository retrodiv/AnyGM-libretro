<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 retrodiv <retrodiv@proton.me> -->

# Third-party notices

AnyGM first-party code is licensed under the repository MIT License. Bundled
third-party components and derived data retain their own terms and notices,
including their original copyright years.

| Component | Location | Terms |
| --- | --- | --- |
| libarchive Cabinet reader closure | `src/third_party/libarchive/` | Upstream terms in `LICENSES/libarchive.txt`; retained file-level notices, including the UC Regents notice in `archive_entry.c` |
| bzip2 | `src/third_party/bzip2/` | bzip2 license in `LICENSES/bzip2.txt` |
| libretro API header | `src/third_party/libretro/include/libretro.h` | MIT, reproduced in `LICENSES/libretro.txt` |
| minimp3 | `src/third_party/minimp3/` | CC0, reproduced in `LICENSES/minimp3.txt` |
| Pxtone playback sources and fork contributions | `src/third_party/pxtone/` | Studio Pixel's playback-source terms in `LICENSES/pxtone.txt` and `src/third_party/pxtone/LICENSE.txt`; source and contributions credited below |
| stb image, image write, TrueType, and Vorbis | `src/third_party/stb/` | MIT option, reproduced in `LICENSES/stb.txt` |
| ww2ogg Wwise Vorbis reconstruction, adapted to read from memory (see [Provenance](docs/PROVENANCE.md)) | `src/third_party/ww2ogg/` | BSD-3-Clause-style terms, reproduced in `LICENSES/ww2ogg.txt` |
| Liberation Sans-derived glyph coverage | `src/generated/gml_default_font_data.h`, `src/generated/gml_classic_info_font_data.h` | SIL Open Font License in `LICENSES/font-ofl-1.1.txt` |
| Roboto Mono-derived glyph coverage | `src/generated/gml_studio_default_font_data.h` | SIL Open Font License in `LICENSES/font-roboto-mono-ofl-1.1.txt` |
| Xiph libvorbis-derived Vorbis setup packets | `src/generated/audio_setup_data.h` | BSD terms in `LICENSES/libvorbis-1.3.7.txt` |
| Xiph libvorbis-derived packed Wwise codebooks | `src/generated/wwise_codebooks.h` | BSD terms in `LICENSES/libvorbis-1.2.0.txt` |
| Xiph libogg used by the optional audio-data generator | `tools/audiogen/` build-time dependency, not linked into the core | BSD terms in `LICENSES/libogg.txt` |
| FMOD Studio bank metadata reader | `src/audio/banks/gml_fmod.c` | Adapted portions retain Apache-2.0 terms; see `NOTICE`, `LICENSES/fmodbankparser.txt`, and `LICENSES/fmodbankparser-NOTICE.txt` |

The Pxtone playback sources are selected from the public cross-platform fork
maintained by Ewan Green. Studio Pixel authored the original playback sources.
The fork also records contributions by Clownacy, syanodev, OPNA2608 and
Christoph Neidahl, among others. Preserve the contributor edit markers in the
vendored code. The Japanese grant is authoritative; its English rendering is
unofficial. Pxtone is not relicensed as first-party MIT code.

The Liberation-derived headers contain raster coverage and metrics, not the
font programs. Their source and rendering conditions are recorded in their
headers and in [Generated-data provenance](docs/PROVENANCE.md). The
Roboto Mono-derived fallback likewise contains coverage and metrics, with
its source and OFL terms recorded in those locations.

The Vorbis setup packets and packed codebooks are generated from licensed Xiph
sources. The optional generator accepts verification values from a required
external manifest and does not use an existing setup table, a sound bank,
middleware binary, or decoded audio as a generation input. Runtime lookup
identifiers are the CRC-32 of the generated setup packets; each is stored
with its packet and verified against the packet bytes. The ww2ogg notice
separately covers the in-memory reconstruction implementation.

The bzip2 source headers refer to material in the upstream distribution. The
complete license is retained locally, and the repository's `NOTICE` identifies
the upstream release and source location.

This revision does not embed the notice bundle into the compiled core. A binary
distributed without this source repository must therefore be accompanied by
`THIRD_PARTY_NOTICES.md`, `NOTICE`, and the applicable files under `LICENSES/`.
