/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "content_patch.h"
#include "content_delta.h"
#include "gml_hash.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct AnygmContentPatch {
  AnygmContentPatchSpec spec;
  size_t references;
  uint8_t *bytes;
  size_t size;
};

static int patch_error(char *error, size_t capacity, const char *message)
{
  if (error && capacity) snprintf(error, capacity, "input patch: %s", message);
  return 0;
}

static int hex_digit(char c)
{
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static int parse_digest(const char *text, size_t size, uint8_t digest[32])
{
  if (size != 64) return 0;
  for (size_t i = 0; i < 32; ++i) {
    int high = hex_digit(text[2 * i]), low = hex_digit(text[2 * i + 1]);
    if (high < 0 || low < 0) return 0;
    digest[i] = (uint8_t)((high << 4) | low);
  }
  return 1;
}

AnygmContentPatch *anygm_content_patch_parse(const char *text, size_t size,
                                           char *error, size_t error_size)
{
  AnygmContentPatchSpec spec = {0};
  const char *parts[6];
  size_t lengths[6], at = 0;
  if (error && error_size) error[0] = 0;
  if (!text || !size || size > 1024 || memchr(text, 0, size)) goto invalid;
  for (size_t part = 0; part < 6; ++part) {
    size_t start = at;
    while (at < size && text[at] != '|') ++at;
    size_t end = at;
    while (start < end && (text[start] == ' ' || text[start] == '\t')) ++start;
    while (end > start && (text[end - 1] == ' ' || text[end - 1] == '\t')) --end;
    if (start == end || (part < 5 ? at == size : at != size)) goto invalid;
    parts[part] = text + start;
    lengths[part] = end - start;
    if (at < size) ++at;
  }
  if (lengths[0] != 6 || memcmp(parts[0], "xdelta", 6) ||
      lengths[1] > ANYGM_CONTENT_MAX_MEMBER_PATH) goto invalid;
  memcpy(spec.path, parts[1], lengths[1]);
  if (!anygm_content_member_normalize(spec.path, lengths[1]) ||
      spec.path[strlen(spec.path) - 1] == '/') goto invalid;
  if (!parse_digest(parts[2], lengths[2], spec.source_digest) ||
      !parse_digest(parts[3], lengths[3], spec.patch_digest) ||
      !parse_digest(parts[4], lengths[4], spec.result_digest)) goto invalid;
  for (size_t i = 0; i < lengths[5]; ++i) {
    char c = parts[5][i];
    if (c < '0' || c > '9' ||
        spec.result_size > (ANYGM_CONTENT_DELTA_MAX_BYTES - (size_t)(c - '0')) / 10)
      goto invalid;
    spec.result_size = spec.result_size * 10 + (size_t)(c - '0');
  }
  AnygmContentPatch *patch = (AnygmContentPatch *)calloc(1, sizeof(*patch));
  if (!patch) {
    patch_error(error, error_size, "declaration allocation failed");
    return NULL;
  }
  patch->spec = spec;
  patch->references = 1;
  return patch;
invalid:
  patch_error(error, error_size, "expected xdelta, confined relative path, three SHA-256 values and bounded result size");
  return NULL;
}

AnygmContentPatch *anygm_content_patch_retain(AnygmContentPatch *patch)
{
  if (!patch || patch->references == SIZE_MAX) return NULL;
  ++patch->references;
  return patch;
}

void anygm_content_patch_release(AnygmContentPatch *patch)
{
  if (!patch || --patch->references) return;
  free(patch->bytes);
  free(patch);
}

void anygm_content_patch_spec(const AnygmContentPatch *patch, AnygmContentPatchSpec *spec)
{
  if (spec) {
    if (patch) *spec = patch->spec;
    else memset(spec, 0, sizeof(*spec));
  }
}

void anygm_content_patch_hash(const AnygmContentPatch *patch, uint8_t digest[32])
{
  GmlSha256 hash;
  gml_sha256_init(&hash);
  if (patch) {
    uint8_t size[8];
    for (unsigned i = 0; i < 8; ++i) size[i] = (uint8_t)((uint64_t)patch->spec.result_size >> (8 * i));
    gml_sha256_update(&hash, "xdelta", 7);
    gml_sha256_update(&hash, patch->spec.path, strlen(patch->spec.path) + 1);
    gml_sha256_update(&hash, patch->spec.source_digest, 32);
    gml_sha256_update(&hash, patch->spec.patch_digest, 32);
    gml_sha256_update(&hash, patch->spec.result_digest, 32);
    gml_sha256_update(&hash, size, sizeof(size));
  }
  gml_sha256_final(&hash, digest);
}

int anygm_content_patch_bind(AnygmContentPatch **patch, uint8_t **bytes, size_t size,
                             char *error, size_t error_size)
{
  uint8_t digest[32];
  if (error && error_size) error[0] = 0;
  if (!patch || !*patch || !bytes || !*bytes || size < 5 ||
      size > ANYGM_CONTENT_DELTA_MAX_BYTES)
    return patch_error(error, error_size, "invalid external byte buffer");
  gml_sha256(*bytes, size, digest);
  if (memcmp(digest, (*patch)->spec.patch_digest, 32))
    return patch_error(error, error_size, "external file SHA-256 mismatch");
  AnygmContentPatch *bound = (AnygmContentPatch *)calloc(1, sizeof(*bound));
  if (!bound) return patch_error(error, error_size, "binding allocation failed");
  bound->spec = (*patch)->spec;
  bound->references = 1;
  bound->bytes = *bytes;
  bound->size = size;
  anygm_content_patch_release(*patch);
  *patch = bound;
  *bytes = NULL;
  return 1;
}

int anygm_content_patch_apply(const AnygmContentPatch *patch,
                              const void *source, size_t source_size,
                              uint8_t **result, size_t *result_size,
                              char *error, size_t error_size)
{
  uint8_t digest[32], *output = NULL;
  if (error && error_size) error[0] = 0;
  if (result) *result = NULL;
  if (result_size) *result_size = 0;
  if (!patch || !patch->bytes || !result || !result_size || (!source && source_size) ||
      source_size > ANYGM_CONTENT_DELTA_MAX_BYTES)
    return patch_error(error, error_size, "missing bound resource or invalid input");
  gml_sha256(source, source_size, digest);
  if (memcmp(digest, patch->spec.source_digest, 32))
    return patch_error(error, error_size, "source SHA-256 mismatch (base or patch order)");
  if (!anygm_content_delta_apply(source, source_size, patch->bytes, patch->size,
      patch->spec.result_size, ANYGM_CONTENT_DELTA_MAX_WORKSPACE, &output, error, error_size))
    return 0;
  gml_sha256(output, patch->spec.result_size, digest);
  if (memcmp(digest, patch->spec.result_digest, 32)) {
    free(output);
    return patch_error(error, error_size, "result SHA-256 mismatch");
  }
  *result = output;
  *result_size = patch->spec.result_size;
  return 1;
}
