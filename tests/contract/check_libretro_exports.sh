#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 retrodiv <retrodiv@proton.me>

set -eu

binary=${1:-}
if [ -z "$binary" ] || [ ! -f "$binary" ]; then
  printf '%s\n' "A linked shared core is required" >&2
  exit 2
fi

temporary=$(mktemp)
expected=$(mktemp)
trap 'rm -f -- "$temporary" "$expected"' EXIT HUP INT TERM

# Every defined symbol, not only the retro_ ones. Filtering to the prefix first would have let a
# leaked vendored symbol - the stbi_* interposition link.T exists to prevent - pass this check
# unnoticed, which is the exact failure this file is here to catch. A Mach-O core is recognised by
# its magic and read through the Mach-O spelling of the same request, so one expectation file
# covers both formats and neither can drift from the other.
magic=$(od -An -tx1 -N4 "$binary" | tr -d '[:space:]')
case "$magic" in
  cffaedfe|feedfacf|cefaedfe|feedface)
    # The leading underscore is the Mach-O namespace prefix, and the two linker-generated names
    # below mark the image header and the dso handle; neither is part of the core's own surface.
    if ! symbols=$(nm -gU "$binary" 2>/dev/null); then
      printf '%s\n' "Cannot read the Mach-O export table; a Mach-O-capable nm is required" >&2
      exit 2
    fi
    printf '%s\n' "$symbols" | awk '{ print $NF }' | sed 's/^_//' | \
      grep -v -x -e '_mh_dylib_header' -e '__dso_handle' | LC_ALL=C sort -u >"$temporary"
    ;;
  *)
    if ! symbols=$(nm -D --defined-only "$binary" 2>/dev/null); then
      printf '%s\n' "Cannot read the ELF dynamic symbol table" >&2
      exit 2
    fi
    printf '%s\n' "$symbols" | awk '$2 ~ /^[TWDBR]$/ { print $3 }' | \
      LC_ALL=C sort -u >"$temporary"
    ;;
esac

sed '/^[[:space:]]*#/d;/^[[:space:]]*$/d' tests/contract/libretro_exports.txt >"$expected"
if ! cmp -s "$expected" "$temporary"; then
  printf '%s\n' "The shared core exports do not match the reviewed libretro ABI:" >&2
  diff -u "$expected" "$temporary" >&2 || true
  exit 1
fi
printf '%s\n' "libretro exports: ok"
