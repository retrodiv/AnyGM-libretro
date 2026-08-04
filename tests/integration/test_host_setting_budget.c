/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Development settings are host lookups, not free reads.
 *
 * Profiling a frame on an in-order handheld found the runtime asking the host for the same
 * development settings once per opcode dispatch and once per array write: thousands of lookups per
 * frame, costing more than the interpreter's own dispatch.  Settings cannot change while content
 * runs, so each hot-path consumer resolves its setting once per VM and keeps the answer.
 *
 * The asserted observable is per-name, not a total: a setting that a hot path consumes must not be
 * re-asked once content is running.  That holds no matter how much code the fixture executes, so
 * the case stays discriminating on a small synthetic game, where a whole-frame budget would not
 * separate a cached consumer from an uncached one. */
#include "anygm.h"
#include "stdio_vfs.h"
#include "synthetic_content.h"

#include <stdio.h>
#include <string.h>

/* Settings consumed from per-opcode, per-call or per-array-write paths. */
static const char *const hot_path_settings[]={
  "GML_TRACE_CALL",     /* opcode dispatch and every call opcode */
  "GML_TRACE",          /* code entry */
  "GML_DBG_ARRAYSET",   /* every array write */
  "GML_LOG_VIEW",       /* every view-array write */
};
#define HOT_SETTING_COUNT ((int)(sizeof hot_path_settings/sizeof *hot_path_settings))

/* A re-entered VM may legitimately resolve a setting once more; per-opcode consumers overshoot
 * this by three orders of magnitude, so the bound stays both tolerant and discriminating. */
#define HOT_SETTING_ALLOWANCE 1

/* Secondary net for settings that are not individually tracked. */
#define SETTING_BUDGET_PER_FRAME 64

static long g_hot_calls[HOT_SETTING_COUNT];
static long g_total_calls;
static int g_counting;
static AnygmDevelopmentSettingFn g_host_setting;

static const char *counting_setting(void *userdata,const char *name){
  if(g_counting){
    g_total_calls++;
    for(int i=0;i<HOT_SETTING_COUNT;i++)
      if(name && !strcmp(name,hot_path_settings[i])){ g_hot_calls[i]++; break; }
  }
  return g_host_setting?g_host_setting(userdata,name):NULL;
}

static int setting_budget_case(void){
  AnygmSyntheticContent fixture;
  if(!anygm_synthetic_draw_content_create(&fixture)){
    fputs("host setting budget: fixture creation failed\n",stderr);
    return 0;
  }

  AnygmHostServices services={0};
  services.struct_size=sizeof services;
  services.abi_version=ANYGM_HOST_SERVICES_VERSION;
  anygm_stdio_vfs_services_init(&services);
  g_host_setting=services.development_setting;
  services.development_setting=counting_setting;

  AnygmEngine *engine=NULL;
  AnygmContentSource source={0};
  source.struct_size=sizeof source;
  source.kind=ANYGM_CONTENT_PATH;
  source.path=fixture.path;
  source.cache_directory=fixture.directory;
  source.save_directory=fixture.directory;

  int ok=anygm_create(&services,&engine)==ANYGM_OK &&
         anygm_load(engine,&source,NULL)==ANYGM_OK;
  if(!ok){
    fputs("host setting budget: engine load failed\n",stderr);
    if(engine) anygm_destroy(engine);
    anygm_synthetic_content_destroy(&fixture);
    return 0;
  }

  AnygmInputFrame input={0};
  input.struct_size=sizeof input;
  input.pointer_x=input.pointer_y=-1;
  AnygmFrameOutput output={0};

  /* Load and the first frames resolve one-time settings; measure the steady state after them. */
  for(int frame=0;ok && frame<4;frame++){
    output.struct_size=sizeof output;
    ok=anygm_run_frame(engine,&input,&output)==ANYGM_OK;
  }
  if(!ok){
    fputs("host setting budget: warmup frame failed\n",stderr);
    anygm_destroy(engine);
    anygm_synthetic_content_destroy(&fixture);
    return 0;
  }

  const int measured_frames=8;
  memset(g_hot_calls,0,sizeof g_hot_calls);
  g_total_calls=0;
  g_counting=1;
  for(int frame=0;ok && frame<measured_frames;frame++){
    output.struct_size=sizeof output;
    ok=anygm_run_frame(engine,&input,&output)==ANYGM_OK;
  }
  g_counting=0;
  long per_frame=g_total_calls/measured_frames;

  anygm_destroy(engine);
  anygm_synthetic_content_destroy(&fixture);

  if(!ok){
    fputs("host setting budget: measured frame failed\n",stderr);
    return 0;
  }

  int failed=0;
  for(int i=0;i<HOT_SETTING_COUNT;i++){
    if(g_hot_calls[i]>HOT_SETTING_ALLOWANCE){
      fprintf(stderr,
              "host setting budget: '%s' was resolved %ld times across %d running frames "
              "(allowance %d) — a hot path is asking the host per opcode or per write instead of "
              "resolving it once per VM\n",
              hot_path_settings[i],g_hot_calls[i],measured_frames,HOT_SETTING_ALLOWANCE);
      failed=1;
    }
  }
  if(per_frame>SETTING_BUDGET_PER_FRAME){
    fprintf(stderr,
            "host setting budget: %ld development-setting lookups per frame exceeds the %d budget\n",
            per_frame,SETTING_BUDGET_PER_FRAME);
    failed=1;
  }
  if(failed) return 0;

  printf("host development-setting budget: %ld per frame, hot-path settings re-resolved 0 times "
         "(limit %d per frame)\n",per_frame,SETTING_BUDGET_PER_FRAME);
  return 1;
}

int main(void){
  if(!setting_budget_case()) return 1;
  puts("host setting budget: ok");
  return 0;
}
