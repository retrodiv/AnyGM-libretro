#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 retrodiv <retrodiv@proton.me>
#
# The hybrid GPU path is bounded by construction, not by intention. These rules exist so that its
# weight can grow without the software renderer becoming entangled with it: removing the GPU
# directory and the hardware bridge must stay a deletion rather than a reversal.

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

# Every first-party source outside the GL owner and the libretro adapter.
portable_outside_gpu() {
  find src/api src/core src/compatibility src/content src/runtime src/audio src/host \
    src/video/renderer src/video/software3d \
    -type f \( -name '*.c' -o -name '*.h' \) \
    -not -path '*/third_party/*' -not -path '*/generated/*' 2>/dev/null
}

gl_tokens='(^|[^[:alnum:]_])(GLuint|GLint|GLenum|GLsizei|GLbitfield|GLfloat|GLchar|GLboolean|glGen[[:alnum:]_]*|glBind[[:alnum:]_]*|glDraw[[:alnum:]_]*|glTex[[:alnum:]_]*|glUniform[[:alnum:]_]*|GL_[A-Z0-9_]+|EGL[A-Z][[:alnum:]_]*|egl[A-Z][[:alnum:]_]*)([^[:alnum:]_]|$)'

gl_outside_owner=$(portable_outside_gpu | xargs grep -n -H -E "$gl_tokens" 2>/dev/null || true)
fail_matches "Only the private GL backend may name OpenGL or EGL types, constants and entry points:" \
  "$gl_outside_owner"

# The neutral seams. A GL or libretro type in either of these headers would make the plan and the
# RHI facade describe one backend instead of describing the work.
for header in src/video/renderer/gml_render_plan.h src/video/gpu/gml_gpu.h; do
  [ -f "$header" ] || continue
  neutral=$(grep -n -H -E "$gl_tokens|retro_[[:alnum:]_]+|RETRO_[A-Z0-9_]+" "$header" 2>/dev/null || true)
  fail_matches "The neutral graphics seam must contain no GL or libretro type: $header" "$neutral"
done

# Nothing outside the GPU directory and its own tests may include its private headers.
if [ -d src/video/gpu ]; then
  gpu_private=$(portable_outside_gpu | xargs grep -n -H -E \
    '#[[:space:]]*include[[:space:]]*"gml_gpu_(gl|internal)[[:alnum:]_]*\.h"' 2>/dev/null || true)
  fail_matches "GPU-private headers belong to the GPU owner:" "$gpu_private"
fi

# The adapter translates; it does not reach into the renderer or the GPU implementation.
adapter_internals=$(find src/adapters -type f \( -name '*.c' -o -name '*.h' \) -exec grep -n -H -E \
  '#[[:space:]]*include[[:space:]]*"(gml_render|gml_gpu|engine_internal|gml_vm|gml_win)[[:alnum:]_]*\.h"' \
  {} + 2>/dev/null || true)
fail_matches "The libretro adapter must consume only the public AnyGM surface:" "$adapter_internals"

# One dispatch table per engine. A process-global one would be shared by every core instance in a
# frontend that loads more than one, and the second instance would call into the first's context.
if [ -d src/video/gpu ]; then
  global_dispatch=$(find src/video/gpu -type f -name '*.c' -exec grep -n -H -E \
    '^[[:space:]]*(static[[:space:]]+)?[[:alnum:]_]+[[:space:]]+g_[[:alnum:]_]*(gl|gpu|proc)[[:alnum:]_]*[[:space:]]*(=|;|\[)' \
    {} + 2>/dev/null || true)
  fail_matches "GPU dispatch and resource state must be engine-owned, never process-global:" \
    "$global_dispatch"
fi

# The proc loader the host supplied is the only way in.
if [ -d src/video/gpu ]; then
  ambient_loader=$(find src/video/gpu -type f \( -name '*.c' -o -name '*.h' \) -exec grep -n -H -E \
    '(dlsym|wglGetProcAddress|eglGetProcAddress|glXGetProcAddress|GetProcAddress)[[:space:]]*\(' \
    {} + 2>/dev/null || true)
  fail_matches "GL entry points come from the host-provided callback only:" "$ambient_loader"
fi

printf '%s\n' "graphics boundaries: ok"
