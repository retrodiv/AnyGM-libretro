/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "content_delta.h"
#include "../../support/anygm_test_runner.h"
#include "../../fixtures/vcdiff/fixtures.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Authored RFC 3284 window: COPY three source bytes, then ADD "def". */
static const uint8_t simple_patch[] = {
  0xd6, 0xc3, 0xc4, 0, 0, 1, 3, 0, 12, 6, 0, 3, 3, 1,
  'd', 'e', 'f', 19, 3, 4, 0
};

static int apply_simple(void)
{
  uint8_t source[] = "abc", *result = NULL;
  char error[256];
  int ok = anygm_content_delta_apply(source, 3, simple_patch, sizeof(simple_patch),
      6, ANYGM_CONTENT_DELTA_MAX_WORKSPACE, &result, error, sizeof(error));
  if (!ok) fprintf(stderr, "%s\n", error);
  ok = ok && result && !memcmp(result, "abcdef", 6) && !strcmp((char *)source, "abc");
  free(result);
  return ok;
}

static int reject_truncated(void)
{
  for (size_t size = 0; size < sizeof(simple_patch); ++size) {
    uint8_t *result = (uint8_t *)(uintptr_t)1;
    char error[256] = {0};
    if (anygm_content_delta_apply((const uint8_t *)"abc", 3, simple_patch, size,
        6, ANYGM_CONTENT_DELTA_MAX_WORKSPACE, &result, error, sizeof(error)) ||
        result || !error[0]) return 0;
  }
  return 1;
}

static int reject_limits(void)
{
  const size_t sizes[] = {0, 5, 7, ANYGM_CONTENT_DELTA_MAX_BYTES + 1};
  uint8_t *result;
  char error[256];
  for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
    if (anygm_content_delta_apply((const uint8_t *)"abc", 3, simple_patch,
        sizeof(simple_patch), sizes[i], ANYGM_CONTENT_DELTA_MAX_WORKSPACE,
        &result, error, sizeof(error)) || result) return 0;
  }
  if (anygm_content_delta_apply((const uint8_t *)"abc", 3, simple_patch,
      sizeof(simple_patch), 6, 1, &result, error, sizeof(error)) || result ||
      !strstr(error, "workspace limit")) return 0;
  return apply_simple(); /* A failed budget must not poison the next decoder. */
}

static void fixture_source(uint8_t source[8192])
{
  for (size_t i = 0; i < 8192; ++i) source[i] = (uint8_t)((i * 13 + i / 97) % 251);
}

static int fixture_result(const uint8_t *result, size_t size, const uint8_t source[8192])
{
  for (size_t i = 0; i < size; ++i) {
    uint8_t expected = i % 997 < 512 ? source[i % 8192] : (uint8_t)(65 + (i / 31) % 23);
    if (result[i] != expected) return 0;
  }
  return 1;
}

static int fixture_apply(const uint8_t *patch, size_t patch_size, size_t target_size)
{
  uint8_t source[8192], *result = NULL;
  char error[256];
  fixture_source(source);
  int ok = anygm_content_delta_apply(source, sizeof(source), patch, patch_size,
      target_size, ANYGM_CONTENT_DELTA_MAX_WORKSPACE, &result, error, sizeof(error));
  if (!ok) fprintf(stderr, "%s\n", error);
  ok = ok && result && fixture_result(result, target_size, source);
  free(result);
  for (size_t i = 0; i < sizeof(source); ++i)
    if (source[i] != (i * 13 + i / 97) % 251) return 0;
  return ok;
}

static int plain(void) { return fixture_apply(vcdiff_plain, sizeof(vcdiff_plain), 16384); }
static int djw(void) { return fixture_apply(vcdiff_djw, sizeof(vcdiff_djw), 16384); }
static int fgk(void) { return fixture_apply(vcdiff_fgk, sizeof(vcdiff_fgk), 16384); }
static int lzma(void) { return fixture_apply(vcdiff_lzma, sizeof(vcdiff_lzma), 16384); }
static int lzma_windows(void)
{
  return fixture_apply(vcdiff_lzma_windows, sizeof(vcdiff_lzma_windows), 8 * 1024 * 1024 + 8192);
}

static int reject_secondary(void)
{
  const struct { const uint8_t *data; size_t size; } fixtures[] = {
    {vcdiff_plain, sizeof(vcdiff_plain)}, {vcdiff_djw, sizeof(vcdiff_djw)},
    {vcdiff_fgk, sizeof(vcdiff_fgk)}, {vcdiff_lzma, sizeof(vcdiff_lzma)},
  };
  uint8_t source[8192];
  fixture_source(source);
  for (size_t f = 0; f < sizeof(fixtures) / sizeof(fixtures[0]); ++f) {
    const uint8_t *patch = fixtures[f].data;
    size_t size = fixtures[f].size;
    for (size_t end = 0; end < size; ++end) {
      uint8_t *result = NULL;
      char error[256] = {0};
      if (anygm_content_delta_apply(source, sizeof(source), patch, end, 16384,
          ANYGM_CONTENT_DELTA_MAX_WORKSPACE, &result, error, sizeof(error)) || result ||
          !error[0]) { free(result); return 0; }
    }
    uint8_t *mutated = (uint8_t *)malloc(size);
    if (!mutated) return 0;
    for (size_t at = 0; at < size; ++at) {
      uint8_t *result = NULL;
      char error[256];
      memcpy(mutated, patch, size);
      mutated[at] ^= (uint8_t)(1u << (at % 8));
      int accepted = anygm_content_delta_apply(source, sizeof(source), mutated, size,
          16384, ANYGM_CONTENT_DELTA_MAX_WORKSPACE, &result, error, sizeof(error));
      /* Metadata mutations can be harmless. An accepted result must still be exact. */
      int ok = accepted ? result && fixture_result(result, 16384, source) : !result;
      free(result);
      if (!ok) { free(mutated); return 0; }
    }
    free(mutated);
    source[0] ^= 1;
    uint8_t *result = NULL;
    char error[256];
    int accepted = anygm_content_delta_apply(source, sizeof(source), patch, size, 16384,
        ANYGM_CONTENT_DELTA_MAX_WORKSPACE, &result, error, sizeof(error));
    source[0] ^= 1;
    if (accepted || result) { free(result); return 0; }
  }
  return 1;
}

static int lzma_budget(void)
{
  uint8_t source[8192], *result = NULL;
  char error[256];
  fixture_source(source);
  if (anygm_content_delta_apply(source, sizeof(source), vcdiff_lzma, sizeof(vcdiff_lzma),
      16384, 65536, &result, error, sizeof(error)) || result ||
      !strstr(error, "workspace limit")) return 0;
  return lzma();
}

int main(int argc, char **argv)
{
  static const AnygmTestCase cases[] = {
    {"copy_and_add", apply_simple},
    {"truncated", reject_truncated},
    {"limits", reject_limits},
    {"plain", plain}, {"djw", djw}, {"fgk", fgk}, {"lzma", lzma},
    {"lzma_windows", lzma_windows}, {"secondary_rejection", reject_secondary},
    {"lzma_budget", lzma_budget},
  };
  const AnygmTestGroup group = {"content.delta", cases, sizeof(cases) / sizeof(cases[0])};
  AnygmTestResult result;
  int ok = anygm_test_run_groups(&group, 1, argc > 1 ? argv[1] : NULL, &result);
  printf("content delta: %d passed, %d failed\n", result.passed, result.failed);
  return ok ? 0 : 1;
}
