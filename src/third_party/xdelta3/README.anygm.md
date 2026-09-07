<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 retrodiv <retrodiv@proton.me> -->

# xdelta3 decoder import

Source: the official `jmacd/xdelta` **v3.2.0** release,
<https://github.com/jmacd/xdelta/tree/v3.2.0>.
Archive: <https://codeload.github.com/jmacd/xdelta/tar.gz/refs/tags/v3.2.0>.
The Apache-2.0 license is preserved in `LICENSES/xdelta3.txt`; source notices are intact.
Local modifications carry retrodiv's MIT notice within the affected files; redistribution
of each combined file also follows the upstream Apache-2.0 terms.

Only the in-memory decoder is built. The command-line application, encoders, external compression
programs, native file helpers, regression driver and optional command-line armor are excluded.
The first-party `content_delta.c` owner is the only consumer of the raw interface.
Standard VCDIFF and xdelta3's DJW, FGK and LZMA secondary sections are enabled. This is xdelta3's
supported VCDIFF subset, not a claim to implement every optional feature of RFC 3284.

Local modifications preserve the upstream algorithm bodies:

- `xdelta3.h` includes the shared portable decoder configuration and respects an existing
  `_POSIX_SOURCE` definition.
- `xdelta3-fgk.h` excludes complete encoder-only helpers from decoder-only builds.
- `xdelta3-lzma.h` routes allocations through the xdelta stream budget, replaces unlimited
  decoder memory with an explicit limit, excludes encoder-only initialization and rejects a
  truncated section that makes no progress.
- `xdelta3-djw.h` uses a flat array for the prefix decoder's flat traversal and checks symbol,
  repeat-shift and selector-group bounds before indexing or shifting.

`anygm_config.h` is first-party MIT configuration. All decode state and allocator accounting belong
to one invocation. A 64 MiB target-window limit and a caller-selected auxiliary-memory budget capped
at 256 MiB apply independently of the explicit source, patch and output buffers. The library never
disables window checksums. Callers own validation of declared SHA-256 identities.

The build suppresses only imported unused-parameter, unused-function and intentional switch
fallthrough diagnostics. It does not suppress bounds, conversion or owned-wrapper warnings.
