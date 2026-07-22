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
trap 'rm -f -- "$temporary"' EXIT HUP INT TERM
nm -D --defined-only "$binary" | awk '$2 ~ /^[TW]$/ && $3 ~ /^retro_/ { print $3 }' | \
  LC_ALL=C sort -u >"$temporary"
expected=$(mktemp)
trap 'rm -f -- "$temporary" "$expected"' EXIT HUP INT TERM
sed '/^[[:space:]]*#/d;/^[[:space:]]*$/d' tests/contract/libretro_exports.txt >"$expected"
if ! cmp -s "$expected" "$temporary"; then
  printf '%s\n' "The shared core exports do not match the reviewed libretro ABI:" >&2
  diff -u "$expected" "$temporary" >&2 || true
  exit 1
fi
printf '%s\n' "libretro exports: ok"
