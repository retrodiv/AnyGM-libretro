/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "libretro_internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#if defined(__GLIBC__)
#include <malloc.h>
#endif

#if defined(__linux__)
#include <fcntl.h>
#include <unistd.h>

/* Values from procfs are decimal KiB. Missing or malformed values are not zero. */
static bool meminfo_kib(const char *text,const char *key,uint64_t *value){
  size_t length=strlen(key);
  const char *line=text;
  while(*line){
    if(strncmp(line,key,length)==0){
      const char *digit=line+length;
      while(*digit==' ' || *digit=='\t') digit++;
      if(*digit<'0' || *digit>'9') return false;
      uint64_t number=0;
      do {
        unsigned next=(unsigned)(*digit++-'0');
        if(number>(UINT64_MAX-next)/10) return false;
        number=number*10+next;
      } while(*digit>='0' && *digit<='9');
      while(*digit==' ' || *digit=='\t') digit++;
      if(strncmp(digit,"kB",2)!=0 || (digit[2] && digit[2]!='\n')) return false;
      *value=number;
      return true;
    }
    const char *newline=strchr(line,'\n');
    if(!newline) break;
    line=newline+1;
  }
  return false;
}
#endif

bool libretro_memory_read(const char *path,LibretroMemorySample *sample){
#if defined(__linux__)
  char text[8192];
  int fd=open(path?path:"/proc/meminfo",O_RDONLY|O_CLOEXEC);
  if(fd<0) return false;
  ssize_t length;
  do length=read(fd,text,sizeof text-1); while(length<0 && errno==EINTR);
  close(fd);
  if(length<=0) return false;
  text[length]=0;
  return meminfo_kib(text,"MemAvailable:",&sample->available_kib) &&
         meminfo_kib(text,"SwapFree:",&sample->swap_free_kib);
#else
  (void)path;
  (void)sample;
  return false;
#endif
}

enum LibretroMemoryAction libretro_memory_observe(LibretroMemoryPressure *pressure,
  const LibretroMemorySample *sample,uint64_t now_ns){
  const uint64_t persistence_ns=UINT64_C(2000000000);
  pressure->next_sample_ns=now_ns+UINT64_C(500000000);
  if(!sample){
    pressure->warning_since_ns=0;
    pressure->critical_since_ns=0;
    pressure->emergency_samples=0;
    return LIBRETRO_MEMORY_CONTINUE;
  }

  uint64_t available=sample->available_kib;
  /* Credit swap only up to the threshold. A large swap partition must not
   * disguise the physical pressure used for the warning. */
  uint64_t swap=sample->swap_free_kib;
  uint64_t headroom=available<48u*1024u?
    available+(swap<48u*1024u?swap:48u*1024u):available;
  if(headroom<48u*1024u){
    if(!pressure->critical_since_ns) pressure->critical_since_ns=now_ns;
  } else {
    pressure->critical_since_ns=0;
  }
  pressure->emergency_samples=headroom<32u*1024u?pressure->emergency_samples+1:0;
  if(pressure->emergency_samples>=2 ||
     (pressure->critical_since_ns && now_ns-pressure->critical_since_ns>=persistence_ns))
    return LIBRETRO_MEMORY_STOP;

  if(available>=112u*1024u) pressure->warned=false;
  if(available>=96u*1024u){
    pressure->warning_since_ns=0;
  } else if(!pressure->warned){
    if(!pressure->warning_since_ns) pressure->warning_since_ns=now_ns;
    if(now_ns-pressure->warning_since_ns>=persistence_ns){
      pressure->warned=true;
      return LIBRETRO_MEMORY_WARN;
    }
  }
  return LIBRETRO_MEMORY_CONTINUE;
}

enum { NOTICE_WIDTH=192, NOTICE_HEIGHT=108 };
static uint32_t notice_pixels[NOTICE_WIDTH*NOTICE_HEIGHT];

static const uint8_t notice_font[26][7]={
  ['A'-'A']={14,17,17,31,17,17,17},
  ['B'-'A']={30,17,17,30,17,17,30},
  ['C'-'A']={14,17,16,16,16,17,14},
  ['D'-'A']={30,17,17,17,17,17,30},
  ['E'-'A']={31,16,16,30,16,16,31},
  ['G'-'A']={14,17,16,23,17,17,15},
  ['H'-'A']={17,17,17,31,17,17,17},
  ['L'-'A']={16,16,16,16,16,16,31},
  ['M'-'A']={17,27,21,21,17,17,17},
  ['N'-'A']={17,25,21,19,17,17,17},
  ['O'-'A']={14,17,17,17,17,17,14},
  ['P'-'A']={30,17,17,30,16,16,16},
  ['R'-'A']={30,17,17,30,20,18,17},
  ['S'-'A']={15,16,16,14,1,1,30},
  ['T'-'A']={31,4,4,4,4,4,4},
  ['U'-'A']={17,17,17,17,17,17,14},
  ['W'-'A']={17,17,17,21,21,21,10},
  ['Y'-'A']={17,17,10,4,4,4,4},
};

static void notice_line(const char *text,unsigned top,uint32_t color){
  unsigned x=(NOTICE_WIDTH-(unsigned)strlen(text)*6u+1u)/2u;
  for(;*text;text++,x+=6){
    if(*text<'A' || *text>'Z') continue;
    const uint8_t *rows=notice_font[*text-'A'];
    for(unsigned y=0;y<7;y++)
      for(unsigned col=0;col<5;col++)
        if(rows[y]&(16u>>col)) notice_pixels[(top+y)*NOTICE_WIDTH+x+col]=color;
  }
}

void libretro_memory_reset(void){
  memset(&g_libretro.pressure,0,sizeof g_libretro.pressure);
  /* A deterministic frontend-contract stimulus. Normal runs use the host clock
   * and measurements and never derive memory pressure from emulated frame counts. */
  const char *fixture=getenv("GML_TEST_MEMORY_PRESSURE");
  if(fixture && *fixture>='1' && *fixture<='9'){
    char *end;
    errno=0;
    unsigned long long after=strtoull(fixture,&end,10);
    if(!errno && *end==':' && end[1]>='0' && end[1]<='9'){
      const char *amount=end+1;
      unsigned long long available=strtoull(amount,&end,10);
      if(!errno && !*end && after<=UINT32_MAX && available<=UINT32_MAX){
        g_libretro.pressure.test_after_tick=after;
        g_libretro.pressure.test_available_kib=available*1024u;
      }
    }
  }
  for(size_t i=0;i<sizeof notice_pixels/sizeof *notice_pixels;i++)
    notice_pixels[i]=0x00141820;
  notice_line("NOT ENOUGH MEMORY",30,0x00ffd98d);
  notice_line("THE GAME WAS STOPPED",47,0x00ffffff);
  notice_line("PRESS A BUTTON TO CLOSE",73,0x00c9d3e2);
}

static uint64_t memory_clock(void){
  const LibretroMemoryPressure *pressure=&g_libretro.pressure;
  if(pressure->test_after_tick)
    return UINT64_C(1000000000)+__atomic_load_n(&pressure->tick,__ATOMIC_RELAXED)*UINT64_C(16666667);
  return libretro_host_monotonic_time_ns(NULL);
}

static bool memory_sample(LibretroMemorySample *sample){
  const LibretroMemoryPressure *pressure=&g_libretro.pressure;
  if(pressure->test_after_tick){
    sample->available_kib=__atomic_load_n(&pressure->tick,__ATOMIC_RELAXED)>=pressure->test_after_tick?
      pressure->test_available_kib:512u*1024u;
    sample->swap_free_kib=0;
    return true;
  }
  return libretro_memory_read(NULL,sample);
}

static void memory_notice(const char *text,enum retro_log_level level,unsigned ms){
  libretro_log(level,"%s\n",text);
  if(!g_libretro.environment) return;
  struct retro_message_ext message={
    .msg=text,.duration=ms,.priority=3,.level=level,.target=RETRO_MESSAGE_TARGET_OSD,
    .type=RETRO_MESSAGE_TYPE_NOTIFICATION,.progress=-1
  };
  if(!g_libretro.environment(RETRO_ENVIRONMENT_SET_MESSAGE_EXT,&message)){
    struct retro_message legacy={text,ms*60u/1000u};
    g_libretro.environment(RETRO_ENVIRONMENT_SET_MESSAGE,&legacy);
  }
}

void libretro_memory_load_notice(void){
  memory_notice("AnyGM cannot start: not enough memory.",RETRO_LOG_ERROR,8000);
}

static bool memory_reserve_fits(LibretroMemorySample sample,uint64_t needed_kib){
  return sample.available_kib>=needed_kib ||
         sample.swap_free_kib>=needed_kib-sample.available_kib;
}

uint32_t libretro_memory_permit(void *unused,size_t size){
  (void)unused;
  if(size<8u*1024u*1024u) return 1;
  LibretroMemorySample sample;
  if(!memory_sample(&sample) ||
     memory_reserve_fits(sample,size/1024u+(size%1024u!=0)+32u*1024u)) return 1;
  __atomic_store_n(&g_libretro.pressure.allocation_rejected,true,__ATOMIC_RELAXED);
  return 0;
}

bool libretro_memory_can_load(void){
  LibretroMemorySample sample;
  return !memory_sample(&sample) || memory_reserve_fits(sample,32u*1024u);
}

static void memory_trim_allocator(void){
#if defined(__GLIBC__)
  malloc_trim(0);
#endif
}

bool libretro_memory_allocation_rejected(void){
  return __atomic_load_n(&g_libretro.pressure.allocation_rejected,__ATOMIC_RELAXED);
}

bool libretro_memory_check(void){
  LibretroMemoryPressure *pressure=&g_libretro.pressure;
  if(pressure->stopping || libretro_memory_allocation_rejected()) return false;
  uint64_t now=memory_clock();
  if(!now || now<pressure->next_sample_ns) return true;
  LibretroMemorySample sample;
  enum LibretroMemoryAction action=libretro_memory_observe(pressure,
    memory_sample(&sample)?&sample:NULL,now);
  if(action==LIBRETRO_MEMORY_WARN){
    memory_trim_allocator();
    memory_notice("AnyGM is running low on memory.",RETRO_LOG_WARN,4000);
  }
  if(action==LIBRETRO_MEMORY_STOP){
    __atomic_store_n(&pressure->allocation_rejected,true,__ATOMIC_RELAXED);
    return false;
  }
  return true;
}

void libretro_memory_request_stop(void){
  __atomic_store_n(&g_libretro.pressure.allocation_rejected,true,__ATOMIC_RELAXED);
}

void libretro_memory_stop(void){
  LibretroMemoryPressure *pressure=&g_libretro.pressure;
  if(pressure->stopping) return;
  pressure->stopping=true;
  pressure->hardware_notice=anygm_graphics_present_emergency_frame(
    g_libretro.engine,notice_pixels,NOTICE_WIDTH,NOTICE_HEIGHT)!=0;
  libretro_abort_for_memory();
  memory_trim_allocator();
  pressure->stopped_at_ns=memory_clock();
  memory_notice("Not enough memory. AnyGM stopped the game.",RETRO_LOG_ERROR,8000);
}

static bool notice_button_pressed(void){
  if(!g_libretro.input_state) return false;
  if(g_libretro.input_poll) g_libretro.input_poll();
  if(g_libretro.input_bitmasks)
    return g_libretro.input_state(0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_MASK)!=0;
  for(unsigned id=0;id<=RETRO_DEVICE_ID_JOYPAD_R3;id++)
    if(g_libretro.input_state(0,RETRO_DEVICE_JOYPAD,0,id)) return true;
  return false;
}

bool libretro_memory_run(void){
  LibretroMemoryPressure *pressure=&g_libretro.pressure;
  if(pressure->test_after_tick) __atomic_add_fetch(&pressure->tick,1,__ATOMIC_RELAXED);
  if(!pressure->stopping){
    if(!g_libretro.loaded && !libretro_memory_allocation_rejected()) return false;
    if(libretro_memory_check()) return false;
    libretro_memory_stop();
  }
  bool already_presented=pressure->screen_sent;
  if(g_libretro.video){
    const void *frame=notice_pixels;
    size_t pitch=NOTICE_WIDTH*sizeof(uint32_t);
    if(already_presented && (g_libretro.can_dupe || pressure->hardware_notice)){
      frame=NULL;
      pitch=0;
    } else if(pressure->hardware_notice){
      frame=RETRO_HW_FRAME_BUFFER_VALID;
      pitch=0;
    }
    g_libretro.video(frame,NOTICE_WIDTH,NOTICE_HEIGHT,pitch);
    pressure->screen_sent=true;
  }
  if(pressure->shutdown_sent) return true;
  uint64_t now=memory_clock();
  uint64_t elapsed=now>=pressure->stopped_at_ns?now-pressure->stopped_at_ns:0;
  if((already_presented && pressure->expedite) || !now ||
     elapsed>=UINT64_C(8000000000) ||
     (elapsed>=UINT64_C(1000000000) && notice_button_pressed())){
    pressure->shutdown_sent=true;
    if(g_libretro.environment) g_libretro.environment(RETRO_ENVIRONMENT_SHUTDOWN,NULL);
  }
  return true;
}

bool libretro_memory_refuse_state(void *data,size_t size){
  LibretroMemoryPressure *pressure=&g_libretro.pressure;
  if(!pressure->stopping) return false;
  if(data && size && size<=pressure->last_state_size && !pressure->expedite){
    LibretroMemorySample sample;
    bool available=memory_sample(&sample);
    if((available && !memory_reserve_fits(sample,size/1024u+32u*1024u)) ||
       (!available && size>4u*1024u*1024u))
      pressure->expedite=true;
    else
      memset(data,0,size);
  }
  return true;
}
