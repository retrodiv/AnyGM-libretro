<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 retrodiv <retrodiv@proton.me> -->

# liblzma decoder import

Source: XZ Utils **5.8.3**, <https://github.com/tukaani-project/xz/releases/tag/v5.8.3>.
Archive: <https://github.com/tukaani-project/xz/releases/download/v5.8.3/xz-5.8.3.tar.gz>.
The library's 0BSD terms are preserved verbatim in `LICENSES/liblzma.txt`.

Retained sources and headers come from `src/liblzma/` and `src/common/`; the latter's bounded
portability headers live in `common_support/`. No command-line utility, getopt implementation,
build system, translation or unrelated package component is imported. Required upstream notices
remain intact. `anygm_config.h` is first-party MIT configuration, not an upstream file.

`ANYGM_LZMA_SOURCES` selects the in-memory stream decoder and its dependencies. It supports the
LZMA1/LZMA2 filters used by xdelta's LZMA secondary streams and CRC32, CRC64 and SHA-256 checks.
Other XZ filters and general archive loading are outside this boundary. No encoder, thread, host
memory probe, runtime CPU selection, assembly implementation or mutable CRC table is compiled.
The selected C implementation uses immutable upstream CRC tables and caller-owned decoder state.

Only the vendored xdelta LZMA adapter calls this private library. The first-party content boundary
does not expose liblzma types. xdelta forwards its bounded allocator to all LZMA allocations.
The core has no dynamic liblzma dependency and no build-time download step.
