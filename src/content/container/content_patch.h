/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef ANYGM_CONTENT_PATCH_H
#define ANYGM_CONTENT_PATCH_H

#include "content_path.h"
#include <stdint.h>

typedef struct AnygmContentPatch AnygmContentPatch;

typedef struct {
  char path[ANYGM_CONTENT_MAX_MEMBER_PATH + 1u];
  uint8_t source_digest[32];
  uint8_t patch_digest[32];
  uint8_t result_digest[32];
  size_t result_size;
} AnygmContentPatchSpec;

/* xdelta | relative path | source SHA-256 | patch SHA-256 | result SHA-256 | size */
AnygmContentPatch *anygm_content_patch_parse(const char *text, size_t size,
                                           char *error, size_t error_size);
/* Immutable declarations and bound bytes may be shared by configuration copies.
 * References belong to one engine's serial configuration/lifecycle owner. */
AnygmContentPatch *anygm_content_patch_retain(AnygmContentPatch *patch);
void anygm_content_patch_release(AnygmContentPatch *patch);
void anygm_content_patch_spec(const AnygmContentPatch *patch, AnygmContentPatchSpec *spec);
void anygm_content_patch_hash(const AnygmContentPatch *patch, uint8_t digest[32]);
/* Replace only this reference, after checking the patch identity. Success takes
 * the malloc-owned byte buffer and clears *bytes; failure changes neither input.
 * The caller relinquishes all aliases to transferred bytes. No I/O occurs here. */
int anygm_content_patch_bind(AnygmContentPatch **patch, uint8_t **bytes, size_t size,
                             char *error, size_t error_size);
int anygm_content_patch_apply(const AnygmContentPatch *patch,
                              const void *source, size_t source_size,
                              uint8_t **result, size_t *result_size,
                              char *error, size_t error_size);

#endif
