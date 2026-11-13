/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "gml_video_provider.h"

#if ANYGM_VIDEO_PROVIDER_DYNAMIC

#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
typedef HMODULE GmlVideoModule;
typedef FARPROC GmlVideoSymbol;
#else
#include <dlfcn.h>
typedef void *GmlVideoModule;
typedef void *GmlVideoSymbol;
#endif

enum { GML_VIDEO_MODULE_COUNT=5 };
GmlVideoProviderApi gml_video_provider_api;
static GmlVideoModule provider_modules[GML_VIDEO_MODULE_COUNT];
static unsigned provider_references;
static atomic_flag provider_lock=ATOMIC_FLAG_INIT;
static char provider_error[256]="FFmpeg shared libraries have not been loaded";

static void lock_provider(void){
  while(atomic_flag_test_and_set_explicit(&provider_lock,memory_order_acquire)){}
}
static void unlock_provider(void){
  atomic_flag_clear_explicit(&provider_lock,memory_order_release);
}
static void set_error(const char *message,const char *detail){
  if(detail && detail[0]) snprintf(provider_error,sizeof provider_error,"%s: %s",message,detail);
  else snprintf(provider_error,sizeof provider_error,"%s",message);
}

#ifdef _WIN32
static int provider_directory(wchar_t path[MAX_PATH]){
  HMODULE module=NULL;
  if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                        (const wchar_t *)(uintptr_t)&gml_video_provider_acquire,&module)) return 0;
  DWORD length=GetModuleFileNameW(module,path,MAX_PATH);
  if(!length || length>=MAX_PATH) return 0;
  while(length && path[length-1]!=L'\\' && path[length-1]!=L'/') length--;
  if(!length) return 0;
  path[length]=L'\0';return (int)length;
}
static GmlVideoModule open_named_module(const char *stem,unsigned major){
  wchar_t directory[MAX_PATH],candidate[MAX_PATH];
  int prefix=provider_directory(directory);
  const char *patterns[]={"%s-%u.dll","lib%s-%u.dll","%s.dll","lib%s.dll"};
  for(size_t index=0;index<sizeof patterns/sizeof patterns[0];index++){
    char leaf[96];
    snprintf(leaf,sizeof leaf,patterns[index],stem,major);
    int leaf_length=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,leaf,-1,
                                        candidate+prefix,MAX_PATH-prefix);
    if(prefix>0 && leaf_length>0){
      memcpy(candidate,directory,(size_t)prefix*sizeof *candidate);
      HMODULE module=LoadLibraryExW(candidate,NULL,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
      if(!module) module=LoadLibraryW(candidate);
      if(module) return module;
    }
    wchar_t plain[96];
    if(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,leaf,-1,plain,96)>0){
      HMODULE module=LoadLibraryExW(plain,NULL,LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
      if(module) return module;
    }
  }
  return NULL;
}
static GmlVideoSymbol provider_symbol(GmlVideoModule module,const char *name){
  return GetProcAddress(module,name);
}
static void close_module(GmlVideoModule module){if(module) FreeLibrary(module);}
#else
static GmlVideoModule open_named_module(const char *stem,unsigned major){
  char name[96];
#ifdef __APPLE__
  const char *patterns[]={"lib%s.%u.dylib","lib%s.dylib"};
#else
  const char *patterns[]={"lib%s.so.%u","lib%s.so"};
#endif
  for(size_t index=0;index<sizeof patterns/sizeof patterns[0];index++){
    snprintf(name,sizeof name,patterns[index],stem,major);
    GmlVideoModule module=dlopen(name,RTLD_NOW|RTLD_LOCAL);
    if(module) return module;
  }
  return NULL;
}
static GmlVideoSymbol provider_symbol(GmlVideoModule module,const char *name){
  return dlsym(module,name);
}
static void close_module(GmlVideoModule module){if(module) dlclose(module);}
#endif

static void unload_provider(void){
  memset(&gml_video_provider_api,0,sizeof gml_video_provider_api);
  for(int index=GML_VIDEO_MODULE_COUNT-1;index>=0;index--){
    close_module(provider_modules[index]);provider_modules[index]=NULL;
  }
}

static int load_symbol(GmlVideoModule module,const char *name,void *destination,size_t size){
  GmlVideoSymbol symbol=provider_symbol(module,name);
  if(!symbol){set_error("FFmpeg provider is missing a required symbol",name);return 0;}
  if(size!=sizeof symbol){set_error("FFmpeg provider uses an unsupported function pointer ABI",name);return 0;}
  memcpy(destination,&symbol,size);return 1;
}

#define LOAD(module,name) \
  do{if(!load_symbol((module),#name,&gml_video_provider_api.name, \
                     sizeof gml_video_provider_api.name)) goto fail;}while(0)
static int load_provider(void){
  provider_modules[0]=open_named_module("avutil",LIBAVUTIL_VERSION_MAJOR);
  provider_modules[1]=open_named_module("swresample",LIBSWRESAMPLE_VERSION_MAJOR);
  provider_modules[2]=open_named_module("swscale",LIBSWSCALE_VERSION_MAJOR);
  provider_modules[3]=open_named_module("avcodec",LIBAVCODEC_VERSION_MAJOR);
  provider_modules[4]=open_named_module("avformat",LIBAVFORMAT_VERSION_MAJOR);
  const char *names[]={"avutil","swresample","swscale","avcodec","avformat"};
  for(int index=0;index<GML_VIDEO_MODULE_COUNT;index++) if(!provider_modules[index]){
#ifndef _WIN32
    const char *detail=dlerror();
#else
    const char *detail=NULL;
#endif
    char message[128];snprintf(message,sizeof message,"FFmpeg %s shared library is unavailable",names[index]);
    set_error(message,detail);goto fail;
  }
  GmlVideoModule util=provider_modules[0],resample=provider_modules[1],scale=provider_modules[2];
  GmlVideoModule codec=provider_modules[3],format=provider_modules[4];
  LOAD(util,av_channel_layout_check);LOAD(util,av_channel_layout_compare);
  LOAD(util,av_channel_layout_copy);LOAD(util,av_channel_layout_default);
  LOAD(util,av_channel_layout_uninit);
  LOAD(util,av_dict_free);LOAD(util,av_dict_set);LOAD(util,av_frame_alloc);
  LOAD(util,av_frame_free);LOAD(util,av_frame_get_buffer);LOAD(util,av_frame_unref);
  LOAD(util,av_free);LOAD(util,av_freep);
  LOAD(util,av_get_bytes_per_sample);LOAD(util,av_malloc);LOAD(util,av_realloc);
  LOAD(util,av_rescale_rnd);LOAD(util,avutil_version);
  LOAD(codec,av_packet_alloc);LOAD(codec,av_packet_free);LOAD(codec,av_packet_unref);
  LOAD(codec,avcodec_alloc_context3);LOAD(codec,avcodec_find_decoder);
  LOAD(codec,avcodec_free_context);LOAD(codec,avcodec_open2);
  LOAD(codec,avcodec_parameters_to_context);LOAD(codec,avcodec_receive_frame);
  LOAD(codec,avcodec_send_packet);LOAD(codec,avcodec_version);
  LOAD(format,av_find_best_stream);LOAD(format,av_read_frame);LOAD(format,avformat_alloc_context);
  LOAD(format,avformat_close_input);LOAD(format,avformat_find_stream_info);
  LOAD(format,avformat_open_input);LOAD(format,avformat_version);
  LOAD(format,avio_alloc_context);LOAD(format,avio_context_free);
  LOAD(resample,swresample_version);LOAD(resample,swr_alloc_set_opts2);
  LOAD(resample,swr_convert);LOAD(resample,swr_free);LOAD(resample,swr_get_delay);
  LOAD(resample,swr_init);
  LOAD(scale,swscale_version);LOAD(scale,sws_freeContext);LOAD(scale,sws_getCachedContext);
  LOAD(scale,sws_getCoefficients);LOAD(scale,sws_scale);LOAD(scale,sws_setColorspaceDetails);
  if((gml_video_provider_api.avformat_version()>>16)!=LIBAVFORMAT_VERSION_MAJOR ||
     (gml_video_provider_api.avcodec_version()>>16)!=LIBAVCODEC_VERSION_MAJOR ||
     (gml_video_provider_api.avutil_version()>>16)!=LIBAVUTIL_VERSION_MAJOR ||
     (gml_video_provider_api.swscale_version()>>16)!=LIBSWSCALE_VERSION_MAJOR ||
     (gml_video_provider_api.swresample_version()>>16)!=LIBSWRESAMPLE_VERSION_MAJOR){
    set_error("FFmpeg shared-library majors do not match the build headers",NULL);goto fail;
  }
  provider_error[0]='\0';return 1;
fail:
  unload_provider();return 0;
}
#undef LOAD

int gml_video_provider_acquire(void){
  int result=0;lock_provider();
  if(provider_references || load_provider()){
    provider_references++;result=1;
  }
  unlock_provider();return result;
}
void gml_video_provider_release(void){
  lock_provider();
  if(provider_references && --provider_references==0) unload_provider();
  unlock_provider();
}
int gml_video_provider_available(void){
  int result=gml_video_provider_acquire();
  if(result) gml_video_provider_release();
  return result;
}
const char *gml_video_provider_error(void){return provider_error;}

#else

int gml_video_provider_acquire(void){return 0;}
void gml_video_provider_release(void){}
int gml_video_provider_available(void){return 0;}
const char *gml_video_provider_error(void){return "video provider was disabled at build time";}

#endif
