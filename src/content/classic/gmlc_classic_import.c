/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "gmlc_classic_import.h"
#include "gmlc_classic_import_internal.h"
#include "anygm_host.h"
#include "anygm_vfs.h"
#include "gml_font_raster.h"
#include "gml_image_codec.h"

#include "gml_default_font_data.h"

#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *import_inflate_owned(const uint8_t *encoded,size_t encoded_size,
                                  int *decoded_size){
  GmlMediaBuffer decoded={0};
  if(decoded_size) *decoded_size=0;
  if(!decoded_size ||
     !gml_deflate_decode(encoded,encoded_size,GML_DEFLATE_ZLIB,&decoded) ||
     decoded.size>(size_t)INT_MAX){
    gml_media_buffer_release(&decoded);
    return NULL;
  }
  *decoded_size=(int)decoded.size;
  return (char*)decoded.data;
}

uint32_t import_u32_at(const uint8_t *p){
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

int import_u32(ImportReader *r, uint32_t *value, const char *what){
  if(r->pos > r->size || r->size - r->pos < 4){
    if(r->err && r->errcap) snprintf(r->err, r->errcap, "classic import: truncated %s", what);
    return 0;
  }
  *value = import_u32_at(r->data + r->pos);
  r->pos += 4;
  return 1;
}

int import_skip_words(ImportReader *r,uint64_t count,const char *what){
  if(r->pos>r->size || count>(r->size-r->pos)/4u){
    if(r->err&&r->errcap) snprintf(r->err,r->errcap,"classic import: truncated %s",what);
    return 0;
  }
  r->pos+=(size_t)count*4u;
  return 1;
}

int import_blob(ImportReader *r, const uint8_t **data, uint32_t *size, const char *what){
  if(!import_u32(r, size, what)) return 0;
  if(r->pos > r->size || *size > r->size - r->pos){
    if(r->err && r->errcap) snprintf(r->err, r->errcap, "classic import: truncated %s data", what);
    return 0;
  }
  *data = r->data + r->pos;
  r->pos += *size;
  return 1;
}

int import_skip_string(ImportReader *r, const uint8_t **text, uint32_t *length, const char *what){
  if(!import_u32(r, length, what)) return 0;
  if(r->pos > r->size || *length > r->size - r->pos){
    if(r->err && r->errcap) snprintf(r->err, r->errcap, "classic import: truncated %s", what);
    return 0;
  }
  if(text) *text = r->data + r->pos;
  r->pos += *length;
  return 1;
}

int import_copy_string(ImportReader *r, char **text, const char *what){
  const uint8_t *bytes;
  uint32_t length;
  *text = NULL;
  if(!import_skip_string(r, &bytes, &length, what)) return 0;
  char *copy = (char*)malloc((size_t)length + 1);
  if(!copy){
    if(r->err && r->errcap) snprintf(r->err, r->errcap, "classic import: out of memory reading %s", what);
    return 0;
  }
  memcpy(copy, bytes, length);
  copy[length] = '\0';
  *text = copy;
  return 1;
}

int import_double(ImportReader *r, double *value, const char *what){
  if(r->pos > r->size || r->size - r->pos < 8){
    if(r->err && r->errcap) snprintf(r->err, r->errcap, "classic import: truncated %s", what);
    return 0;
  }
  uint64_t bits = (uint64_t)r->data[r->pos] | (uint64_t)r->data[r->pos + 1] << 8 |
                  (uint64_t)r->data[r->pos + 2] << 16 | (uint64_t)r->data[r->pos + 3] << 24 |
                  (uint64_t)r->data[r->pos + 4] << 32 | (uint64_t)r->data[r->pos + 5] << 40 |
                  (uint64_t)r->data[r->pos + 6] << 48 | (uint64_t)r->data[r->pos + 7] << 56;
  memcpy(value, &bits, sizeof(bits));
  r->pos += 8;
  return 1;
}

char *copy_string(const char *text){
  size_t length = text ? strlen(text) : 0;
  char *copy = (char*)malloc(length + 1);
  if(!copy) return NULL;
  if(length) memcpy(copy, text, length);
  copy[length] = '\0';
  return copy;
}

char *cache_path(const char *dir, const char *leaf){
  size_t nd = strlen(dir), nl = strlen(leaf);
  int separator = nd && dir[nd - 1] != '/' && dir[nd - 1] != '\\';
  char *path = (char*)malloc(nd + (size_t)separator + nl + 1);
  if(!path) return NULL;
  memcpy(path, dir, nd);
  size_t at = nd;
  if(separator) path[at++] = '/';
  memcpy(path + at, leaf, nl + 1);
  return path;
}

static int write_source(const AnygmHostServices *host,const char *path,const char *source,
                        char *err,size_t errcap){
  size_t length = source ? strlen(source) : 0;
  int ok=anygm_vfs_write_all(host,path,source?source:"",length);
  if(!ok && err && errcap) snprintf(err, errcap, "classic import: cannot write %s", path);
  return ok;
}

char *import_source_path(GmlcProject *project, const char *cache_dir,
                                const char *leaf, const char *source,
                                char *err, size_t errcap){
  if(project->prefer_memory_files){
    const char *text=source?source:"";
    char *path=gmlc_project_add_memory_file(project,leaf,GMLC_MEMORY_TEXT,
                                            text,strlen(text),0,0);
    if(!path && err && errcap)
      snprintf(err,errcap,"classic import: out of memory retaining %s",leaf);
    return path;
  }
  char *path=cache_path(cache_dir,leaf);
  if(!path || !write_source(project->host,path,source,err,errcap)){
    free(path);
    return NULL;
  }
  return path;
}
