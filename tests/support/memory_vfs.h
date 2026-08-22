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
  char replacement_path[1024];
  const uint8_t *replacement_data;
  size_t replacement_size;
  unsigned replacement_open;
  unsigned replacement_read_opens;
  int replacement_active;
  int replacement_complete;
  char snapshot_path[1024];
  const uint8_t *snapshot_data;
  size_t snapshot_size;
  unsigned snapshot_open;
  unsigned snapshot_read_opens;
  int snapshot_active;
  int snapshot_complete;
} AnygmMemoryVfs;

void anygm_memory_vfs_init(AnygmMemoryVfs *memory,AnygmHostServices *services);
void anygm_memory_vfs_destroy(AnygmMemoryVfs *memory);
int anygm_memory_vfs_add_file(AnygmMemoryVfs *memory,const char *path,
                              const void *data,size_t size);
int anygm_memory_vfs_xor_byte(AnygmMemoryVfs *memory,const char *path,
                              size_t offset,uint8_t mask);
void anygm_memory_vfs_guard_reads(AnygmMemoryVfs *memory,const char *path,
                                  uint64_t begin,uint64_t size);
void anygm_memory_vfs_replace_on_read_open(AnygmMemoryVfs *memory,const char *path,
                                           unsigned open_number,const void *data,size_t size);
void anygm_memory_vfs_snapshot_on_read_open(AnygmMemoryVfs *memory,const char *path,
                                            unsigned open_number,const void *data,size_t size);

#endif
