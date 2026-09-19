#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 retrodiv <retrodiv@proton.me>
#
# The whole procedure for incrementing this project's version.
#
# ANYGM_VERSION in src/api/anygm.h is the only place the version is written: an adapter reports it as
# the runtime library version, and every publishable metadata file says "Git" rather than a number,
# so this script has nothing else to synchronise. It rewrites that one line, refuses a value that is
# not a plain three-component version without leading zeros, and prints the old and the new value.
#
#   tools/bump-version.sh [path to the public header]
#
# The optional path exists so tests/architecture/check_version_policy.py can exercise the increment
# and the rejection rules on a copy of the header; it defaults to the in-tree one. The "0.1"
# component is never touched here: it moves only when the project owner asks for it, and by hand.

set -eu

header=${1:-src/api/anygm.h}
if [ ! -f "$header" ]; then
  printf '%s\n' "no such version header: $header" >&2
  exit 2
fi

current=$(sed -n 's/^#define ANYGM_VERSION "\([^"]*\)".*/\1/p' "$header")
if [ -z "$current" ]; then
  printf '%s\n' "no ANYGM_VERSION definition in $header" >&2
  exit 2
fi

# A component is a plain decimal integer: no sign and no leading zero, because a written-out leading
# zero is a different number to whoever parses it next, and to a shell it is octal.
valid_component() {
  case "$1" in
    "" | *[!0-9]*) return 1 ;;
    0) return 0 ;;
    0*) return 1 ;;
  esac
  return 0
}

major= minor= patch= extra=
IFS=. read -r major minor patch extra <<EOF
$current
EOF
if [ -n "$extra" ] || ! valid_component "$major" || ! valid_component "$minor" ||
  ! valid_component "$patch"; then
  printf '%s\n' \
    "ANYGM_VERSION in $header is not a three-component version without leading zeros: $current" >&2
  exit 2
fi

next_patch=$((patch + 1))
next=$major.$minor.$next_patch

temporary=$(mktemp "${TMPDIR:-/tmp}/anygm-bump-version.XXXXXX")
trap 'rm -f -- "$temporary"' EXIT HUP INT TERM
sed "s|^#define ANYGM_VERSION \"[^\"]*\"|#define ANYGM_VERSION \"$next\"|" "$header" >"$temporary"
if [ "$(grep -c -F "#define ANYGM_VERSION \"$next\"" "$temporary")" -ne 1 ] ||
  cmp -s "$header" "$temporary"; then
  printf '%s\n' "could not rewrite the version line in $header" >&2
  exit 1
fi
cat "$temporary" >"$header"
printf 'AnyGM version: %s -> %s\n' "$current" "$next"
