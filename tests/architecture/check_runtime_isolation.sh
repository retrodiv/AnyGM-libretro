#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 retrodiv <retrodiv@proton.me>

set -eu

temporary=$(mktemp -d)
cleanup() {
  case "$temporary" in
    /tmp/*|/var/tmp/*) rm -r -- "$temporary" ;;
    *) printf '%s\n' "Refusing to remove unexpected temporary path: $temporary" >&2 ;;
  esac
}
trap cleanup EXIT HUP INT TERM

checkout="$temporary/isolated-core"
mkdir "$checkout"
cp Makefile Makefile.common link.T "$checkout/"
cp -R src tests "$checkout/"
rm -r -- "$checkout/src/adapters/libretro"
rm -r -- "$checkout/src/third_party/libretro"

make -C "$checkout" -j1 BUILD_DIR=build/isolation runtime api-check
printf '%s\n' "isolated portable runtime: ok"
