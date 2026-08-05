/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef ANYGM_CONTENT_ROUTER_H
#define ANYGM_CONTENT_ROUTER_H

#include <stddef.h>
#include <stdint.h>
#include "gml_win.h"

#define ANYGM_CONTENT_MAX_ARCHIVE_BYTES UINT64_C(1073741824)
#define ANYGM_CONTENT_MAX_ARCHIVE_ENTRIES 32768u
#define ANYGM_CONTENT_MAX_MEMBER_PATH 511u
#define ANYGM_CONTENT_MAX_MEMBER_BYTES UINT64_C(1073741824)
#define ANYGM_CONTENT_MAX_EXTRACTED_BYTES UINT64_C(4294967296)
#define ANYGM_CONTENT_MAX_EXPANSION_RATIO 1000u
#define ANYGM_CONTENT_EXPANSION_ALLOWANCE UINT64_C(16777216)
#define ANYGM_CONTENT_MAX_NESTING_LEVELS 4u

typedef void (*AnygmContentLogFn)(void *userdata,int level,const char *message);

typedef struct AnygmContentRouter {
  const struct AnygmHostServices *host;
  const char *cache_directory;
  AnygmContentLogFn log;
  void *log_userdata;
} AnygmContentRouter;

enum {
  ANYGM_CONTENT_LOG_INFO=1,
  ANYGM_CONTENT_LOG_WARN=2,
  ANYGM_CONTENT_LOG_ERROR=3
};

/* resolved_path receives the payload to load. asset_root, when supplied, receives the directory
 * holding the runtime assets the content opens by path, and is emptied whenever that is the
 * payload's own directory. An archive carrying a source project rather than a compiled payload
 * separates the two: the payload is generated into the cache while the assets stay extracted. */
int anygm_content_resolve_path(const AnygmContentRouter *router,const char *input_path,
                               char *resolved_path,size_t resolved_path_size,
                               char *asset_root,size_t asset_root_size);
int anygm_content_load_win(const AnygmContentRouter *router,GmlWin *win,const char *path,char *loaded_path,
                           size_t loaded_path_size);
void anygm_content_path_stem(const char *path,char *output,size_t output_size);
void anygm_content_path_parent(const char *path,char *output,size_t output_size);
void anygm_content_save_label(const char *path,char *output,size_t output_size);
unsigned anygm_content_path_hash(const char *path);
int anygm_content_directory_create(const struct AnygmHostServices *host,const char *path);
/* Delete a generated namespace and its contents. Returns non-zero when the path is gone. */
int anygm_content_directory_remove(const struct AnygmHostServices *host,const char *path);

#endif
