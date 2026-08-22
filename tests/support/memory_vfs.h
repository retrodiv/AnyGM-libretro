/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef ANYGM_TEST_MEMORY_VFS_H
#define ANYGM_TEST_MEMORY_VFS_H

#include "anygm.h"

#include <stddef.h>
#include <stdint.h>

typedef struct AnygmMemoryVfsNode {
  char path[1024];
  uint8_t *data;
  size_t size;
  size_t capacity;
  int directory;
} AnygmMemoryVfsNode;

typedef struct AnygmMemoryVfs {
  AnygmMemoryVfsNode nodes[128];
  size_t node_count;
  char guarded_path[1024];
  uint64_t guard_begin;
  uint64_t guard_end;
  size_t max_read_request;
  int guard_active;
  int read_violation;
} AnygmMemoryVfs;

void anygm_memory_vfs_init(AnygmMemoryVfs *memory,AnygmHostServices *services);
void anygm_memory_vfs_destroy(AnygmMemoryVfs *memory);
int anygm_memory_vfs_add_file(AnygmMemoryVfs *memory,const char *path,
                              const void *data,size_t size);
void anygm_memory_vfs_guard_reads(AnygmMemoryVfs *memory,const char *path,
                                  uint64_t begin,uint64_t size);

#endif
