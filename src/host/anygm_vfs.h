/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef ANYGM_VFS_H
#define ANYGM_VFS_H

#include "anygm.h"

int anygm_vfs_can_read(const AnygmHostServices *host);
int anygm_vfs_can_write(const AnygmHostServices *host);
int anygm_vfs_read_all(const AnygmHostServices *host,const char *path,
                       uint8_t **data,size_t *size,size_t limit);
int anygm_vfs_read_prefix(const AnygmHostServices *host,const char *path,
                          void *data,size_t capacity,size_t *size);
int anygm_vfs_write_all(const AnygmHostServices *host,const char *path,
                        const void *data,size_t size);
int anygm_vfs_copy(const AnygmHostServices *host,const char *source,const char *destination);
int anygm_vfs_stat(const AnygmHostServices *host,const char *path,AnygmFileInfo *info);
int anygm_vfs_mkdirs(const AnygmHostServices *host,const char *path);
int anygm_vfs_publish(const AnygmHostServices *host,const char *temporary,const char *destination);
void anygm_vfs_remove(const AnygmHostServices *host,const char *path);
/* Recover the on-disk spelling of a path whose components differ only in case, which is what a
 * bundle authored for a case-insensitive filesystem asks for when it is read from a
 * case-sensitive one. Writes the exact path unchanged when it already resolves, and otherwise the
 * closest spelling the host can enumerate, component by component. Returns 0 only when nothing
 * could be written. */
int anygm_vfs_resolve_casefold(const AnygmHostServices *host,const char *path,
                               char *out,size_t capacity);

#endif
