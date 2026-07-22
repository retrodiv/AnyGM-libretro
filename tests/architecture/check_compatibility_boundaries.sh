#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 retrodiv <retrodiv@proton.me>

set -eu

fail_matches() {
  message=$1
  matches=$2
  if [ -n "$matches" ]; then
    printf '%s\n' "$message" >&2
    printf '%s\n' "$matches" >&2
    exit 1
  fi
}

portable_product_files() {
  find src/api src/core src/compatibility src/content src/runtime src/video src/audio src/host \
    -type f \( -name '*.c' -o -name '*.h' \) \
    -not -path '*/third_party/*' -not -path '*/generated/*'
}

runtime_files() {
  find src/core src/runtime src/video src/audio -type f \
    \( -name '*.c' -o -name '*.h' \)
}

raw_revision_pattern='classic_version|bytecode[[:space:]]*(==|!=|<=|>=|<|>)'
matches=$(runtime_files | xargs grep -n -E "$raw_revision_pattern" 2>/dev/null || true)
fail_matches "Runtime code must consume named compatibility policies:" "$matches"

frontend_coupling=$(portable_product_files | xargs grep -n -E \
  'libretro|(^|[^[:alnum:]_])retro_[[:alnum:]_]+|RETRO_[[:alnum:]_]+' 2>/dev/null || true)
fail_matches "Portable product code must not depend on the libretro adapter:" "$frontend_coupling"

public_includes=$(grep -R -n -E '^[[:space:]]*#include' src/api --include='*.h' | \
  grep -v -E '#include[[:space:]]+[<"](stddef|stdint)\.h[>"]' || true)
fail_matches "Public headers may include only standard fixed-width and size headers:" "$public_includes"

public_internals=$(grep -R -n -E 'Gml|gml_|libretro|retro_|RETRO_|engine_internal|content_router' \
  src/api --include='*.h' || true)
fail_matches "Public headers must not expose internal or adapter types:" "$public_internals"

adapter_private_includes=$(grep -R -n -E '^[[:space:]]*#include[[:space:]]+"' \
  src/adapters/libretro --include='*.c' --include='*.h' | \
  grep -v -E '"(libretro_internal|anygm|libretro)\.h"' || true)
fail_matches "The libretro adapter may include only its boundary header and public API:" \
  "$adapter_private_includes"

direct_diagnostics=$(portable_product_files | xargs grep -n -E \
  '(^|[^[:alnum:]_])(fprintf|printf|puts|putchar|perror|fputc|fputs)[[:space:]]*\(' \
  2>/dev/null | grep -v 'src/host/stdio_vfs.c:' || true)
fail_matches "Portable diagnostics must use the host log service:" "$direct_diagnostics"

direct_system=$(portable_product_files | xargs grep -n -E \
  '(^|[^[:alnum:]_])(getenv|fopen|freopen|fread|fwrite|fclose|fflush|fseek|ftell|remove|rename|opendir|readdir|closedir|mkdir|rmdir|stat|lstat|clock_gettime|gettimeofday|localtime|localtime_r|gmtime|gmtime_r|CreateWindowExA|LoadLibraryA|GetModuleHandleA|GetProcAddress|PrintWindow)[[:space:]]*\(' \
  2>/dev/null | grep -v -E 'src/host/(stdio_vfs\.c|gml_thread\.h):' || true)
fail_matches "Portable modules must use host services instead of operating-system APIs:" \
  "$direct_system"

active_content=$(portable_product_files | xargs grep -n -E \
  '(^|[^[:alnum:]_])(system|popen|fork|execl|execv|posix_spawn|dlopen|LoadLibrary|socket|connect|curl_easy_)[[:space:]]*\(' \
  2>/dev/null | grep -v -E ':[[:space:]]*(/\*|\*|//)' || true)
fail_matches "Content and runtime paths must not execute processes, load plugins, or open network sockets:" \
  "$active_content"

mutable_local_static=$(portable_product_files | xargs grep -n -E \
  '^[[:space:]]+static[[:space:]]+' 2>/dev/null | \
  grep -v -E 'static[[:space:]]+const[[:space:]]' || true)
fail_matches "Function-local static state must be immutable or owned by an engine context:" \
  "$mutable_local_static"

unbounded_decode=$(find src -type f -name '*.c' \
  -not -path 'src/content/bytecode/gml_bytecode.c' \
  -exec grep -n -H -E 'gml_decode_bc[[:space:]]*\(' {} + 2>/dev/null || true)
fail_matches "Content-derived bytecode must use the bounded decoder entry point:" \
  "$unbounded_decode"

printf '%s\n' "architecture boundaries: ok"
