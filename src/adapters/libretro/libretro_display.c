/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "libretro_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

typedef struct LibretroDisplayMode {
  DEVMODEA original;
  bool changed;
} LibretroDisplayMode;

static bool current_mode(DEVMODEA *mode){
  memset(mode,0,sizeof *mode);
  mode->dmSize=sizeof *mode;
  return EnumDisplaySettingsExA(NULL,ENUM_CURRENT_SETTINGS,mode,0)!=0;
}

static AnygmResult native_request(uint32_t hz){
  DEVMODEA mode;
  if(!current_mode(&mode) || mode.dmDisplayFrequency<=1) return ANYGM_ERROR_UNSUPPORTED;
  if(mode.dmDisplayFrequency==hz) return ANYGM_OK;
  DEVMODEA original=mode;
  LibretroDisplayMode *saved=g_libretro.display_mode;
  mode.dmDisplayFrequency=hz;
  mode.dmFields=DM_PELSWIDTH|DM_PELSHEIGHT|DM_BITSPERPEL|DM_DISPLAYFREQUENCY;
  LONG result=ChangeDisplaySettingsExA(NULL,&mode,NULL,CDS_TEST,NULL);
  if(result==DISP_CHANGE_SUCCESSFUL && !saved){
    saved=calloc(1,sizeof *saved);
    if(!saved) return ANYGM_ERROR_OUT_OF_MEMORY;
    saved->original=original;
  }
  if(result==DISP_CHANGE_SUCCESSFUL)
    result=ChangeDisplaySettingsExA(NULL,&mode,NULL,CDS_FULLSCREEN,NULL);
  if(result==DISP_CHANGE_SUCCESSFUL){
    saved->changed=true;
    g_libretro.display_mode=saved;
  } else if(saved!=g_libretro.display_mode) free(saved);
  DEVMODEA observed;
  uint32_t actual=current_mode(&observed)?observed.dmDisplayFrequency:0;
  libretro_log(RETRO_LOG_INFO,"Display native request: hz=%u result=%ld observed=%u\n",
               hz,(long)result,actual);
  return result==DISP_CHANGE_SUCCESSFUL?ANYGM_OK:ANYGM_ERROR_UNSUPPORTED;
}
#endif

uint32_t libretro_display_frequency(void *userdata){
  (void)userdata;
#ifdef _WIN32
  DEVMODEA mode;
  return current_mode(&mode) && mode.dmDisplayFrequency>1?mode.dmDisplayFrequency:0;
#else
  float frequency=0;
  if(!g_libretro.environment ||
     !g_libretro.environment(RETRO_ENVIRONMENT_GET_TARGET_REFRESH_RATE,&frequency) ||
     !isfinite(frequency) || frequency<2 || (double)frequency>UINT32_MAX) return 0;
  /* The frontend may answer with its configured target rather than a physical measurement.
   * Do not replace it with this core's requested cadence. */
  return (uint32_t)floor((double)frequency+0.5);
#endif
}

AnygmResult libretro_display_request(void *userdata,uint32_t hz){
  (void)userdata;
  if(hz<2) return ANYGM_ERROR_INVALID_ARGUMENT;
#ifdef _WIN32
  AnygmResult result=native_request(hz);
  if(result!=ANYGM_OK) return result;
#else
  if(!g_libretro.environment || !libretro_display_frequency(NULL))
    return ANYGM_ERROR_UNSUPPORTED;
#endif
  g_libretro.display_requested_hz=hz;
  g_libretro.display_notification_pending=true;
  libretro_log(RETRO_LOG_INFO,"Display frontend request queued: hz=%u\n",hz);
  return ANYGM_OK;
}

void libretro_display_release(void){
#ifdef _WIN32
  LibretroDisplayMode *saved=g_libretro.display_mode;
  if(saved && saved->changed){
    saved->original.dmFields=DM_PELSWIDTH|DM_PELSHEIGHT|DM_BITSPERPEL|DM_DISPLAYFREQUENCY;
    LONG result=ChangeDisplaySettingsExA(NULL,&saved->original,NULL,0,NULL);
    libretro_log(RETRO_LOG_INFO,"Display native restoration: hz=%lu result=%ld\n",
                 (unsigned long)saved->original.dmDisplayFrequency,(long)result);
  }
  free(saved);
#endif
  g_libretro.display_mode=NULL;
  g_libretro.display_requested_hz=0;
  g_libretro.display_active_hz=0;
  g_libretro.display_source_hz=0;
  g_libretro.display_notification_pending=false;
  g_libretro.display_mouse_x=g_libretro.display_mouse_y=g_libretro.display_wheel=0;
}
