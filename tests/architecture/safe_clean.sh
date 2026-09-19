#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 retrodiv <retrodiv@proton.me>

set -eu

if [ "$#" -lt 2 ]; then
  printf '%s\n' "safe_clean requires a build directory and at least one artifact" >&2
  exit 2
fi

build_dir=$1
shift
case "$build_dir" in
  build|build/*) ;;
  *)
    printf '%s\n' "Refusing to clean outside the checkout build directory: $build_dir" >&2
    exit 2
    ;;
esac
case "/$build_dir/" in
  */../*|*/./*)
    printf '%s\n' "Refusing an unresolved build directory: $build_dir" >&2
    exit 2
    ;;
esac

if [ -L "$build_dir" ]; then
  printf '%s\n' "Refusing to clean a symbolic-link build directory: $build_dir" >&2
  exit 2
fi
if [ -d "$build_dir" ]; then rm -r -- "$build_dir"; fi
for artifact in "$@"; do
  case "$artifact" in
    anygm_libretro.so|anygm_libretro.dll|anygm_libretro.dylib|anygm_libretro.a|anygm_libretro_android.so)
      if [ -f "$artifact" ] || [ -L "$artifact" ]; then rm -f -- "$artifact"; fi
      ;;
    *)
      printf '%s\n' "Refusing an unknown core artifact: $artifact" >&2
      exit 2
      ;;
  esac
done
