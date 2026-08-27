/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* A replay may pin the civil clock without changing the framework-neutral host contract. */
#include "libretro_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LibretroAdapter g_libretro;

const char *libretro_options_value(const char *key){ (void)key; return NULL; }
void libretro_vfs_services_init(AnygmHostServices *services){ (void)services; }
void libretro_log(enum retro_log_level level,const char *format,...){
  (void)level; (void)format;
}

int main(void){
  AnygmHostServices services={0};
  AnygmWallTime wall={0};
  wall.struct_size=sizeof wall;
  memset(&g_libretro,0,sizeof g_libretro);
  setenv("GML_WALL_TIME","4294967297,-210",1);
  libretro_host_services_init(&services);
  if(!services.wall_time || services.wall_time(services.userdata,&wall)!=ANYGM_OK ||
     wall.unix_seconds!=INT64_C(4294967297) || wall.utc_offset_minutes!=-210 ||
     !(wall.flags&ANYGM_WALL_TIME_OFFSET_VALID)){
    printf("fixed wall time was not transported: seconds=%lld offset=%d flags=%u\n",
      (long long)wall.unix_seconds,wall.utc_offset_minutes,wall.flags);
    return 1;
  }
  printf("libretro wall time: ok\n");
  return 0;
}
