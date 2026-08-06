/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "libretro_internal.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <limits.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <richedit.h>
#endif

#ifdef _WIN32
typedef struct LibretroRichTextStream {
  const uint8_t *data;
  size_t size,position;
} LibretroRichTextStream;

static DWORD CALLBACK host_rich_text_stream(DWORD_PTR cookie,LPBYTE output,
                                             LONG requested,LONG *written){
  LibretroRichTextStream *stream=(LibretroRichTextStream*)cookie;
  size_t remaining=stream->size-stream->position;
  size_t amount=(size_t)requested<remaining?(size_t)requested:remaining;
  if(amount) memcpy(output,stream->data+stream->position,amount);
  stream->position+=amount;
  *written=(LONG)amount;
  return 0;
}

static AnygmResult host_rich_text_render(void *userdata,const void *rtf,size_t rtf_size,
                                         uint32_t background_xrgb,void *pixels,
                                         uint32_t width,uint32_t height,size_t pitch){
  (void)userdata;
  if(!rtf||!rtf_size||!pixels||!width||!height||pitch!=(size_t)width*4u||
     width>INT_MAX||height>INT_MAX) return ANYGM_ERROR_INVALID_ARGUMENT;
  typedef HANDLE (WINAPI *SetThreadDpiAwarenessContextFn)(HANDLE);
  HMODULE user32=GetModuleHandleA("user32.dll");
  SetThreadDpiAwarenessContextFn set_thread_dpi=user32?
    (SetThreadDpiAwarenessContextFn)(void*)GetProcAddress(
      user32,"SetThreadDpiAwarenessContext"):NULL;
  HANDLE previous_dpi=set_thread_dpi?set_thread_dpi((HANDLE)(intptr_t)-1):NULL;
  HMODULE rich_edit=LoadLibraryA("riched20.dll");
  if(!rich_edit){
    if(set_thread_dpi&&previous_dpi) set_thread_dpi(previous_dpi);
    return ANYGM_ERROR_UNSUPPORTED;
  }
  int w=(int)width,h=(int)height;
  HINSTANCE instance=GetModuleHandleA(NULL);
  HWND window=CreateWindowExA(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,"STATIC","",
    WS_POPUP,0,0,w,h,NULL,NULL,instance,NULL);
  HWND edit=window?CreateWindowExA(WS_EX_CLIENTEDGE,RICHEDIT_CLASSA,"",
    WS_CHILD|WS_VISIBLE|ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL,
    0,0,w,h,window,NULL,instance,NULL):NULL;
  int ok=window&&edit;
  if(ok){
    COLORREF background=RGB((background_xrgb>>16)&255u,(background_xrgb>>8)&255u,
                            background_xrgb&255u);
    SendMessageA(edit,EM_SETBKGNDCOLOR,0,(LPARAM)background);
    SendMessageA(edit,EM_SETMARGINS,EC_LEFTMARGIN|EC_RIGHTMARGIN,MAKELPARAM(1,1));
    LibretroRichTextStream stream={(const uint8_t*)rtf,rtf_size,0};
    EDITSTREAM edit_stream={(DWORD_PTR)&stream,0,host_rich_text_stream};
    SendMessageA(edit,EM_STREAMIN,SF_RTF,(LPARAM)&edit_stream);
    if(edit_stream.dwError) ok=0;
  }
  if(ok){
    SetWindowPos(window,HWND_BOTTOM,0,0,w,h,SWP_NOACTIVATE|SWP_SHOWWINDOW);
    RedrawWindow(window,NULL,NULL,RDW_INVALIDATE|RDW_UPDATENOW|RDW_ALLCHILDREN);
    HDC screen=GetDC(window);
    HDC copy=screen?CreateCompatibleDC(screen):NULL;
    HBITMAP bitmap=screen?CreateCompatibleBitmap(screen,w,h):NULL;
    if(!screen||!copy||!bitmap) ok=0;
    HGDIOBJ previous=NULL;
    if(ok){
      previous=SelectObject(copy,bitmap);
      ok=PrintWindow(window,copy,PW_CLIENTONLY)!=0;
      SelectObject(copy,previous);
    }
    if(ok){
      BITMAPINFO information;
      memset(&information,0,sizeof information);
      information.bmiHeader.biSize=sizeof information.bmiHeader;
      information.bmiHeader.biWidth=w;
      information.bmiHeader.biHeight=-h;
      information.bmiHeader.biPlanes=1;
      information.bmiHeader.biBitCount=32;
      information.bmiHeader.biCompression=BI_RGB;
      ok=GetDIBits(copy,bitmap,0,(UINT)h,pixels,&information,DIB_RGB_COLORS)!=0;
      if(ok){
        size_t count=(size_t)width*height;
        uint32_t *output=(uint32_t*)pixels;
        for(size_t i=0;i<count;i++) output[i]|=0xFF000000u;
      }
    }
    if(bitmap) DeleteObject(bitmap);
    if(copy) DeleteDC(copy);
    if(screen) ReleaseDC(window,screen);
  }
  if(window) DestroyWindow(window);
  FreeLibrary(rich_edit);
  if(set_thread_dpi&&previous_dpi) set_thread_dpi(previous_dpi);
  return ok?ANYGM_OK:ANYGM_ERROR_UNSUPPORTED;
}
#endif

static void host_log(void *userdata,AnygmLogLevel level,const char *message){
  (void)userdata;
  enum retro_log_level mapped=level==ANYGM_LOG_ERROR?RETRO_LOG_ERROR:
                              level==ANYGM_LOG_WARN?RETRO_LOG_WARN:
                              level==ANYGM_LOG_DEBUG?RETRO_LOG_DEBUG:RETRO_LOG_INFO;
  libretro_log(mapped,"%s",message?message:"");
}

static uint64_t host_monotonic_time_ns(void *userdata){
  (void)userdata;
#ifdef _WIN32
  LARGE_INTEGER frequency,now;
  QueryPerformanceFrequency(&frequency);
  QueryPerformanceCounter(&now);
  if(!frequency.QuadPart) return 0;
  uint64_t seconds=(uint64_t)(now.QuadPart/frequency.QuadPart);
  uint64_t remainder=(uint64_t)(now.QuadPart%frequency.QuadPart);
  return seconds*UINT64_C(1000000000)+remainder*UINT64_C(1000000000)/(uint64_t)frequency.QuadPart;
#else
  struct timespec now;
  if(clock_gettime(CLOCK_MONOTONIC,&now)!=0) return 0;
  return (uint64_t)now.tv_sec*UINT64_C(1000000000)+(uint64_t)now.tv_nsec;
#endif
}

static AnygmResult host_wall_time(void *userdata,AnygmWallTime *wall){
  (void)userdata;
  if(!wall || wall->struct_size<sizeof *wall) return ANYGM_ERROR_INVALID_ARGUMENT;
  time_t now=time(NULL);
  if(now==(time_t)-1) return ANYGM_ERROR_UNSUPPORTED;
  struct tm local_value,utc_value;
#ifdef _WIN32
  if(localtime_s(&local_value,&now)!=0 || gmtime_s(&utc_value,&now)!=0)
    return ANYGM_ERROR_UNSUPPORTED;
#else
  if(!localtime_r(&now,&local_value) || !gmtime_r(&now,&utc_value))
    return ANYGM_ERROR_UNSUPPORTED;
#endif
  utc_value.tm_isdst=local_value.tm_isdst;
  time_t local_epoch=mktime(&local_value);
  time_t utc_as_local=mktime(&utc_value);
  double offset=difftime(local_epoch,utc_as_local)/60.0;
  if(offset<-1440.0 || offset>1440.0) offset=0.0;
  wall->flags=ANYGM_WALL_TIME_OFFSET_VALID;
  wall->unix_seconds=(int64_t)now;
  wall->utc_offset_minutes=(int32_t)offset;
  wall->reserved=0;
  return ANYGM_OK;
}

static uint64_t host_random_seed(void *userdata){
  (void)userdata;
  AnygmWallTime wall={0};
  wall.struct_size=sizeof wall;
  host_wall_time(NULL,&wall);
  uint64_t value=host_monotonic_time_ns(NULL)^(uint64_t)wall.unix_seconds;
  value^=++g_libretro.seed_counter*UINT64_C(0x9e3779b97f4a7c15);
  value=(value^(value>>30))*UINT64_C(0xbf58476d1ce4e5b9);
  value=(value^(value>>27))*UINT64_C(0x94d049bb133111eb);
  return value^(value>>31);
}

/* Development settings are consulted from per-item hot paths — the VM asks
 * per script call, the blitter per sprite — thousands of times a frame, and
 * getenv walks the whole environment block per call; the Windows C runtimes
 * additionally pay a locale-aware comparison per entry on that walk. The
 * environment cannot change while the frontend runs the core, so the first
 * answer per name is the answer. Missing names are memoised too: almost every
 * hot lookup is a miss. The cache resets on retro_load_game. */
static const char *host_development_setting(void *userdata,const char *name){
  (void)userdata;
  if(!name || !name[0]) return NULL;
  /* Legacy content without locale builtins uses the locale environment variables. A selected
   * language overrides the host environment; Auto returns the host value when present and falls
   * back to the resolved locale otherwise. Handle these names before the cache so changes remain
   * visible during the session. */
  if(!strcmp(name,"LANG")||!strcmp(name,"LC_ALL")||!strcmp(name,"LANGUAGE")){
    const char *chosen=libretro_options_value("anygm_language");
    const char *host=getenv(name);
    if((!chosen || !chosen[0] || !strcmp(chosen,"Auto")) && host && host[0]) return host;
    if(!g_libretro.language[0]) return host;
    if(!strcmp(name,"LANGUAGE")) return g_libretro.language;
    snprintf(g_libretro.locale_variable,sizeof g_libretro.locale_variable,"%s_%s",
             g_libretro.language,g_libretro.region[0]?g_libretro.region:"US");
    return g_libretro.locale_variable;
  }
  uint32_t hash=2166136261u;
  size_t len=0;
  for(;name[len];len++) hash=(hash^(unsigned char)name[len])*16777619u;
  if(len>=sizeof g_libretro.setting_cache[0].name) return getenv(name);
  enum { CACHE_SLOTS=sizeof g_libretro.setting_cache/sizeof g_libretro.setting_cache[0] };
  uint32_t slot=hash&(CACHE_SLOTS-1);
  while(g_libretro.setting_cache[slot].name[0]){
    if(g_libretro.setting_cache[slot].hash==hash &&
       !strcmp(g_libretro.setting_cache[slot].name,name))
      return g_libretro.setting_cache[slot].value;
    slot=(slot+1)&(CACHE_SLOTS-1);
  }
  const char *value=getenv(name);
  if(g_libretro.setting_cache_count<CACHE_SLOTS-16){   /* keep probes short */
    g_libretro.setting_cache[slot].hash=hash;
    g_libretro.setting_cache[slot].value=value;
    memcpy(g_libretro.setting_cache[slot].name,name,len+1);
    g_libretro.setting_cache_count++;
  }
  return value;
}

static void host_rumble(void *userdata,uint32_t port,uint16_t strong,uint16_t weak){
  (void)userdata;
  if(!g_libretro.rumble_available || !g_libretro.rumble.set_rumble_state) return;
  g_libretro.rumble.set_rumble_state(port,RETRO_RUMBLE_STRONG,strong);
  g_libretro.rumble.set_rumble_state(port,RETRO_RUMBLE_WEAK,weak);
}

static AnygmResult host_locale(void *userdata,char *language,size_t language_size,
                               char *region,size_t region_size,char *tag,size_t tag_size){
  (void)userdata;
  if(language&&language_size) snprintf(language,language_size,"%s",g_libretro.language);
  if(region&&region_size) snprintf(region,region_size,"%s",g_libretro.region);
  if(tag&&tag_size) snprintf(tag,tag_size,"%s",g_libretro.language_tag);
  return ANYGM_OK;
}

static AnygmResult host_date_time_format(void *userdata,const AnygmCalendarTime *calendar,
                                         uint32_t style,char *text,size_t text_size){
  (void)userdata;
  if(!calendar||calendar->struct_size<sizeof *calendar||!text||!text_size)
    return ANYGM_ERROR_INVALID_ARGUMENT;
  struct tm value;
  memset(&value,0,sizeof value);
  value.tm_year=calendar->year-1900;
  value.tm_mon=calendar->month-1;
  value.tm_mday=calendar->day;
  value.tm_wday=calendar->weekday;
  value.tm_hour=calendar->hour;
  value.tm_min=calendar->minute;
  value.tm_sec=calendar->second;
  value.tm_isdst=-1;
  const char *format=style==ANYGM_DATE_FORMAT_DATE?"%x":
                     style==ANYGM_DATE_FORMAT_TIME?"%X":"%x %X";
  if(!strftime(text,text_size,format,&value)){
    text[0]=0;
    return ANYGM_ERROR_UNSUPPORTED;
  }
  return ANYGM_OK;
}

void libretro_host_services_init(AnygmHostServices *services){
  if(!services) return;
  memset(services,0,sizeof *services);
  services->struct_size=sizeof *services;
  services->abi_version=ANYGM_HOST_SERVICES_VERSION;
  services->log=host_log;
  services->monotonic_time_ns=host_monotonic_time_ns;
  services->wall_time=host_wall_time;
  services->random_seed=host_random_seed;
  services->development_setting=host_development_setting;
  libretro_vfs_services_init(services);
  services->locale=host_locale;
  services->date_time_format=host_date_time_format;
  services->rumble=host_rumble;
#ifdef _WIN32
  services->rich_text_render=host_rich_text_render;
#endif
}
