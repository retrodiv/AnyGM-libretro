/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include <windows.h>
#include <stdio.h>

static DWORD frequency=120;
static unsigned tests,applies,restores;
static int query_available=1,apply_refused,invalid_arguments;

static BOOL WINAPI fixture_enum(LPCSTR device,DWORD index,LPDEVMODEA mode,DWORD flags){
  if(device || index!=ENUM_CURRENT_SETTINGS || flags || mode->dmSize!=sizeof *mode)
    invalid_arguments=1;
  if(!query_available) return FALSE;
  mode->dmPelsWidth=1280; mode->dmPelsHeight=720; mode->dmBitsPerPel=32;
  mode->dmDisplayFrequency=frequency;
  return TRUE;
}
static LONG WINAPI fixture_change(LPCSTR device,LPDEVMODEA mode,HWND window,
                                  DWORD flags,LPVOID parameter){
  if(device || window || parameter || mode->dmPelsWidth!=1280 || mode->dmPelsHeight!=720 ||
     mode->dmBitsPerPel!=32 || mode->dmFields!=
       (DM_PELSWIDTH|DM_PELSHEIGHT|DM_BITSPERPEL|DM_DISPLAYFREQUENCY))
    invalid_arguments=1;
  if(flags==CDS_TEST){ tests++; return mode->dmDisplayFrequency==53?DISP_CHANGE_BADMODE:0; }
  if(flags==CDS_FULLSCREEN){ applies++; if(apply_refused) return DISP_CHANGE_FAILED; }
  else if(!flags) restores++;
  else { invalid_arguments=1; return DISP_CHANGE_BADFLAGS; }
  frequency=mode->dmDisplayFrequency;
  return DISP_CHANGE_SUCCESSFUL;
}

/* Compile the production adapter body against controlled Win32 responses. No real mode is
 * selected, and no separate implementation of the mode-selection algorithm is tested. */
#define EnumDisplaySettingsExA fixture_enum
#define ChangeDisplaySettingsExA fixture_change
#include "../../src/adapters/libretro/libretro_display.c"
#undef EnumDisplaySettingsExA
#undef ChangeDisplaySettingsExA

LibretroAdapter g_libretro;
void libretro_log(enum retro_log_level level,const char *format,...){
  (void)level; (void)format;
}
#define REQUIRE(condition) do { if(!(condition)){ \
  fprintf(stderr,"Windows display contract failed at line %d\n",__LINE__); return 1; \
} } while(0)

int main(void){
  REQUIRE(libretro_display_frequency(NULL)==120);
  query_available=0;
  REQUIRE(libretro_display_frequency(NULL)==0);
  REQUIRE(libretro_display_request(NULL,50)==ANYGM_ERROR_UNSUPPORTED && tests==0);
  query_available=1;
  REQUIRE(libretro_display_request(NULL,53)==ANYGM_ERROR_UNSUPPORTED);
  REQUIRE(tests==1 && applies==0 && !g_libretro.display_mode && frequency==120);
  frequency=144; apply_refused=1;
  REQUIRE(libretro_display_request(NULL,50)==ANYGM_ERROR_UNSUPPORTED);
  REQUIRE(!g_libretro.display_mode && frequency==144);
  apply_refused=0;
  REQUIRE(libretro_display_request(NULL,50)==ANYGM_OK);
  REQUIRE(libretro_display_frequency(NULL)==50 && g_libretro.display_requested_hz==50);
  REQUIRE(g_libretro.display_notification_pending && g_libretro.display_mode);
  unsigned previous_tests=tests,previous_applies=applies;
  REQUIRE(libretro_display_request(NULL,50)==ANYGM_OK);
  REQUIRE(tests==previous_tests && applies==previous_applies);
  REQUIRE(libretro_display_request(NULL,53)==ANYGM_ERROR_UNSUPPORTED);
  REQUIRE(frequency==50 && g_libretro.display_requested_hz==50);
  REQUIRE(libretro_display_request(NULL,60)==ANYGM_OK && frequency==60);
  libretro_display_release();
  REQUIRE(frequency==144 && restores==1 && !g_libretro.display_mode);
  REQUIRE(!g_libretro.display_requested_hz && !g_libretro.display_active_hz);
  libretro_display_release();
  REQUIRE(restores==1 && !invalid_arguments);
  puts("Windows display modes: ok");
  return 0;
}
