/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me> */
#ifndef GMLC_JSON_H
#define GMLC_JSON_H

#include <stddef.h>

typedef enum {
  GMLC_JSON_NULL,
  GMLC_JSON_BOOL,
  GMLC_JSON_NUMBER,
  GMLC_JSON_STRING,
  GMLC_JSON_ARRAY,
  GMLC_JSON_OBJECT
} GmlcJsonType;

typedef struct GmlcJson {
  GmlcJsonType type;
  char *name;
  char *s;
  double n;
  int b;
  struct GmlcJson *child;
  struct GmlcJson *next;
} GmlcJson;

GmlcJson *gmlc_json_parse_file(const char *path, char *err, size_t errcap);
GmlcJson *gmlc_json_parse_text(const char *text, const char *label, char *err, size_t errcap);
void gmlc_json_free(GmlcJson *v);

const GmlcJson *gmlc_json_obj(const GmlcJson *v, const char *key);
const GmlcJson *gmlc_json_index(const GmlcJson *v, int index);
int gmlc_json_len(const GmlcJson *v);
const char *gmlc_json_str(const GmlcJson *v, const char *fallback);
double gmlc_json_num(const GmlcJson *v, double fallback);
int gmlc_json_int(const GmlcJson *v, int fallback);
int gmlc_json_bool(const GmlcJson *v, int fallback);

#endif
