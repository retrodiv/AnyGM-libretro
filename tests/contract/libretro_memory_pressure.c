/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "libretro_internal.h"

#include <stdio.h>
#include <string.h>

LibretroAdapter g_libretro;
static unsigned stops,notices,retired;

uint64_t libretro_host_monotonic_time_ns(void *unused){
  (void)unused;
  return 0;
}

uint32_t anygm_graphics_present_emergency_frame(AnygmEngine *engine,const uint32_t *pixels,
                                                uint32_t width,uint32_t height){
  (void)engine;
  (void)pixels;
  (void)width;
  (void)height;
  return 0;
}

void libretro_abort_for_memory(void){
  retired++;
  g_libretro.loaded=false;
}

void libretro_log(enum retro_log_level level,const char *format,...){
  (void)level;
  (void)format;
}

static bool environment(unsigned command,void *data){
  (void)data;
  if(command==RETRO_ENVIRONMENT_SHUTDOWN) stops++;
  return false; /* The raster must work even without frontend notifications. */
}

static void video(const void *data,unsigned width,unsigned height,size_t pitch){
  if(!data || width!=192 || height!=108 || pitch!=width*sizeof(uint32_t)) return;
  const uint32_t *pixels=data;
  unsigned ink=0;
  for(unsigned i=0;i<width*height;i++) ink+=pixels[i]!=pixels[0];
  if(ink>100) notices++;
}

static int check_policy(void){
  LibretroMemoryPressure pressure={0};
  LibretroMemorySample sample={.available_kib=85u*1024u};
  uint64_t time=UINT64_C(1000000000);
  for(unsigned i=0;i<4;i++,time+=UINT64_C(500000000))
    if(libretro_memory_observe(&pressure,&sample,time)!=LIBRETRO_MEMORY_CONTINUE) return 0;
  if(libretro_memory_observe(&pressure,&sample,time)!=LIBRETRO_MEMORY_WARN) return 0;
  sample.available_kib=105u*1024u;
  if(libretro_memory_observe(&pressure,&sample,time+=UINT64_C(500000000))!=LIBRETRO_MEMORY_CONTINUE ||
     !pressure.warned) return 0;
  sample.available_kib=120u*1024u;
  if(libretro_memory_observe(&pressure,&sample,time+=UINT64_C(500000000))!=LIBRETRO_MEMORY_CONTINUE ||
     pressure.warned) return 0;
  sample.available_kib=25u*1024u;
  sample.swap_free_kib=64u*1024u;
  for(unsigned i=0;i<7;i++)
    if(libretro_memory_observe(&pressure,&sample,time+=UINT64_C(500000000))==LIBRETRO_MEMORY_STOP)
      return 0;
  sample.swap_free_kib=0;
  if(libretro_memory_observe(&pressure,&sample,time+=UINT64_C(500000000))==LIBRETRO_MEMORY_STOP)
    return 0;
  return libretro_memory_observe(&pressure,&sample,time+=UINT64_C(500000000))==LIBRETRO_MEMORY_STOP;
}

static int check_frontend(void){
  libretro_memory_reset();
  g_libretro.environment=environment;
  g_libretro.video=video;
  g_libretro.can_dupe=true;
  g_libretro.loaded=true;
  g_libretro.pressure.test_after_tick=10;
  g_libretro.pressure.test_available_kib=40u*1024u;
  g_libretro.pressure.last_state_size=16;
  while(!g_libretro.pressure.stopping && g_libretro.pressure.tick<200)
    libretro_memory_run();
  if(!g_libretro.pressure.stopping || g_libretro.loaded || retired!=1 ||
     notices!=1 || stops!=0 || g_libretro.pressure.last_state_size!=16) return 0;
  uint8_t buffer[16];
  memset(buffer,0x5f,sizeof buffer);
  if(!libretro_memory_refuse_state(buffer,sizeof buffer)) return 0;
  for(unsigned i=0;i<sizeof buffer;i++) if(buffer[i]) return 0;
  for(unsigned i=0;i<500;i++) if(!libretro_memory_run()) return 0;
  return stops==1 && notices==1 && retired==1;
}

int main(void){
  if(!check_policy() || !check_frontend()){
    fputs("libretro memory pressure contract failed\n",stderr);
    return 1;
  }
  puts("libretro memory pressure: warning, recovery, terminal raster and shutdown passed");
  return 0;
}
