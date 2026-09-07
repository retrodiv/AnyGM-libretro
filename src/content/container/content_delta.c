/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "content_delta.h"
#include "../../third_party/xdelta3/xdelta3.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  size_t used;
  size_t limit;
  int refused;
} DeltaBudget;

/* Prepending an aligned allocation record lets free return its exact charge
 * without a side table, mutable globals, or exposing a vendor allocator. */
typedef union {
  max_align_t alignment;
  size_t charge;
} DeltaAllocation;

static void *delta_alloc(void *opaque, size_t count, usize_t width)
{
  DeltaBudget *budget = (DeltaBudget *)opaque;
  DeltaAllocation *allocation;
  size_t bytes;
  if (width && count > (SIZE_MAX - sizeof(*allocation)) / width) {
    budget->refused = 1;
    return NULL;
  }
  bytes = count * width + sizeof(*allocation);
  if (bytes > budget->limit - budget->used) {
    budget->refused = 1;
    return NULL;
  }
  allocation = (DeltaAllocation *)malloc(bytes);
  if (!allocation) return NULL;
  allocation->charge = bytes;
  budget->used += bytes;
  return allocation + 1;
}

static void delta_free(void *opaque, void *address)
{
  DeltaBudget *budget = (DeltaBudget *)opaque;
  DeltaAllocation *allocation;
  if (!address) return;
  allocation = (DeltaAllocation *)address - 1;
  budget->used -= allocation->charge;
  free(allocation);
}

static int delta_error(char *error, size_t size, const char *message)
{
  if (error && size) snprintf(error, size, "input patch: %s", message);
  return 0;
}

int anygm_content_delta_apply(const uint8_t *source, size_t source_size,
                              const uint8_t *patch, size_t patch_size,
                              size_t result_size, size_t workspace_limit,
                              uint8_t **result, char *error, size_t error_size)
{
  DeltaBudget budget = {0, workspace_limit, 0};
  xd3_stream stream;
  xd3_config config;
  xd3_source base;
  uint8_t *output;
  usize_t written = 0;
  int status;
  if (error && error_size) error[0] = 0;
  if (result) *result = NULL;
  if (!result || (!source && source_size) || !patch || patch_size < 5 ||
      source_size > ANYGM_CONTENT_DELTA_MAX_BYTES ||
      patch_size > ANYGM_CONTENT_DELTA_MAX_BYTES ||
      result_size > ANYGM_CONTENT_DELTA_MAX_BYTES ||
      !workspace_limit || workspace_limit > ANYGM_CONTENT_DELTA_MAX_WORKSPACE)
    return delta_error(error, error_size, "invalid byte range or resource limit");
  if (memcmp(patch, "\xd6\xc3\xc4\0", 4))
    return delta_error(error, error_size, "not an xdelta3 VCDIFF stream");

  output = (uint8_t *)malloc(result_size ? result_size : 1);
  if (!output) return delta_error(error, error_size, "result allocation failed");
  memset(&stream, 0, sizeof(stream));
  memset(&config, 0, sizeof(config));
  memset(&base, 0, sizeof(base));
  config.alloc = delta_alloc;
  config.freef = delta_free;
  config.opaque = &budget;
  status = xd3_config_stream(&stream, &config);
  if (!status && source_size) {
    /* The complete bounded source is one immutable block. No source callback
     * can reopen a file or silently supply a different version of the bytes. */
    base.blksize = (usize_t)source_size;
    base.onblk = (usize_t)source_size;
    base.curblk = source;
    base.max_winsize = (xoff_t)source_size;
    status = xd3_set_source_and_size(&stream, &base, (xoff_t)source_size);
  }
  if (!status)
    status = xd3_decode_stream(&stream, patch, (usize_t)patch_size, output,
                               &written, (usize_t)result_size);
  if (status || written != result_size) {
    delta_error(error, error_size, budget.refused ? "decoder workspace limit exceeded" :
                status ? (stream.msg ? stream.msg : "invalid VCDIFF stream") :
                "result size does not match the declaration");
    xd3_free_stream(&stream);
    free(output);
    return 0;
  }
  xd3_free_stream(&stream);
  *result = output;
  return 1;
}
