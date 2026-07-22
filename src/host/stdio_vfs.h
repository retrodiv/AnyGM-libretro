/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef ANYGM_STDIO_VFS_H
#define ANYGM_STDIO_VFS_H

#include "anygm.h"

void anygm_stdio_vfs_services_init(AnygmHostServices *services);
struct GmlWin;
int anygm_stdio_load_win(struct GmlWin *win,const char *path);

#endif
