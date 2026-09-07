<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 retrodiv <retrodiv@proton.me> -->

# Authored VCDIFF fixtures

These patches describe only algorithmically authored byte sequences, never an external payload.
`generate.py` owns the exact source and expected-result recipe. The unit test reconstructs both
without invoking an encoder. The handwritten COPY/ADD window in the test is an additional
independent RFC 3284 exercise.

Regeneration is optional and explicit. Use the unmodified xdelta3 v3.2.0 archive pinned in
`src/third_party/xdelta3/README.anygm.md`, with a separately installed liblzma 5.8.3 development
library. On a 64-bit Linux development host, from the upstream `xdelta3/` directory:

```sh
cc -std=c11 -O2 -fPIC -shared -DXD3_MAIN=0 -DXD3_ENCODER=1 \
  -DSECONDARY_DJW=1 -DSECONDARY_FGK=1 -DSECONDARY_LZMA=1 \
  -DSIZEOF_SIZE_T=8 -DSIZEOF_UNSIGNED_LONG=8 -DSIZEOF_UNSIGNED_LONG_LONG=8 \
  xdelta3.c -llzma -o encoder.so
```

Then invoke `generate.py --encoder-library <explicit-path-to-encoder.so>` from this checkout.
The production configuration is decoder-only and cannot regenerate the fixtures. No build or
test downloads, compiles or launches an encoder. The generated header records each patch digest;
the generator also reports target digests. The secondary stream IDs are checked during generation.
The LZMA multi-window fixture crosses the upstream default 8 MiB target-window boundary.
