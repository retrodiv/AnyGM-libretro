#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 retrodiv <retrodiv@proton.me>
#
# Runtime values reach fixed-width builtin arguments as doubles, and content passes negative ones as
# a matter of course (-1 is the ordinary "no tint" colour). Converting a negative or out-of-range
# double straight to an unsigned type is undefined in C, and the targets this runtime ships to
# disagree: one wraps modulo 2^32 while another saturates to zero, so an untinted draw multiplies by
# white on one host and by black on the other.
#
# The defect cannot reproduce on a host that happens to wrap, so no executed unit case can guard it.
# This check enforces the shape instead: argument readers must go through the wrapping conversion.

set -eu

matches=$(grep -rn '(uint32_t)N(' src/runtime/builtins src/adapters 2>/dev/null || true)
if [ -n "$matches" ]; then
  printf '%s\n' "Convert double arguments with NU32/U32, not a direct unsigned cast:" >&2
  printf '%s\n' "A negative or out-of-range double converts differently per target architecture." >&2
  printf '%s\n' "$matches" >&2
  exit 1
fi

printf '%s\n' "numeric argument conversions: ok"
