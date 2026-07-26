/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef GMLC_CLASSIC_IMPORT_INTERNAL_H
#define GMLC_CLASSIC_IMPORT_INTERNAL_H

#include "gmlc_classic_import.h"

typedef struct {
  const uint8_t *data;
  size_t size, pos;
  char *err;
  size_t errcap;
} ImportReader;

typedef struct {
  char *data;
  size_t length, capacity;
} ImportText;

/* Cross-owner classic import operations. */
char *import_inflate_owned(const uint8_t *encoded,size_t encoded_size,
                                  int *decoded_size);
uint32_t import_u32_at(const uint8_t *p);
int import_u32(ImportReader *r, uint32_t *value, const char *what);
int import_skip_words(ImportReader *r,uint64_t count,const char *what);
int import_blob(ImportReader *r, const uint8_t **data, uint32_t *size, const char *what);
int import_skip_string(ImportReader *r, const uint8_t **text, uint32_t *length, const char *what);
int import_copy_string(ImportReader *r, char **text, const char *what);
int import_double(ImportReader *r, double *value, const char *what);
char *copy_string(const char *text);
char *cache_path(const char *dir, const char *leaf);
char *import_source_path(GmlcProject *project, const char *cache_dir,
                                const char *leaf, const char *source,
                                char *err, size_t errcap);
int text_append(ImportText *text, const char *value);
int import_actions(ImportReader *r, ImportText *text);

/* GMLC_CLASSIC_IMPORT_PRIVATE_OPERATIONS */

#endif
