/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "anygm.h"
#include "stdio_vfs.h"
#include "synthetic_content.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct DummyHost {
  unsigned log_calls;
  unsigned locale_calls;
  unsigned monotonic_calls;
  unsigned wall_calls;
  unsigned seed_calls;
} DummyHost;

static AnygmResult unsupported_locale(void *userdata,char *language,size_t language_size,
                                      char *region,size_t region_size,char *tag,size_t tag_size){
  DummyHost *host=userdata;
  (void)language;
  (void)language_size;
  (void)region;
  (void)region_size;
  (void)tag;
  (void)tag_size;
  host->locale_calls++;
  return ANYGM_ERROR_UNSUPPORTED;
}

static AnygmResult unsupported_wall_time(void *userdata,AnygmWallTime *wall){
  DummyHost *host=userdata;
  (void)wall;
  host->wall_calls++;
  return ANYGM_ERROR_UNSUPPORTED;
}

static void *unavailable_file_open(void *userdata,const char *path,AnygmFileMode mode){
  DummyHost *host=userdata;
  (void)path;
  (void)mode;
  host->log_calls++;
  return NULL;
}

static AnygmResult unavailable_file_stat(void *userdata,const char *path,AnygmFileInfo *info){
  DummyHost *host=userdata;
  (void)path;
  (void)info;
  host->log_calls++;
  return ANYGM_ERROR_IO;
}

static size_t unavailable_file_read(void *userdata,void *file,void *data,size_t size){
  (void)userdata;
  (void)file;
  (void)data;
  (void)size;
  return 0;
}

static void unavailable_file_close(void *userdata,void *file){
  (void)userdata;
  (void)file;
}

static void dummy_log(void *userdata,AnygmLogLevel level,const char *message){
  DummyHost *host=userdata;
  (void)level;
  (void)message;
  host->log_calls++;
}

static AnygmResult dummy_locale(void *userdata,char *language,size_t language_size,
                                char *region,size_t region_size,char *tag,size_t tag_size){
  DummyHost *host=userdata;
  host->locale_calls++;
  if(language&&language_size) snprintf(language,language_size,"en");
  if(region&&region_size) snprintf(region,region_size,"gb");
  if(tag&&tag_size) snprintf(tag,tag_size,"en-GB");
  return ANYGM_OK;
}

static uint64_t dummy_monotonic_time_ns(void *userdata){
  DummyHost *host=userdata;
  return (uint64_t)++host->monotonic_calls*UINT64_C(1000000);
}

static AnygmResult dummy_wall_time(void *userdata,AnygmWallTime *wall){
  DummyHost *host=userdata;
  if(!wall || wall->struct_size<sizeof *wall) return ANYGM_ERROR_INVALID_ARGUMENT;
  host->wall_calls++;
  wall->flags=ANYGM_WALL_TIME_OFFSET_VALID;
  wall->unix_seconds=0;
  wall->utc_offset_minutes=0;
  wall->reserved=0;
  return ANYGM_OK;
}

static uint64_t dummy_random_seed(void *userdata){
  DummyHost *host=userdata;
  host->seed_calls++;
  return UINT64_C(0x123456789abcdef0);
}

static int fail(const char *message){
  fprintf(stderr,"dummy host: %s\n",message);
  return 1;
}

static int exercise_optional_service_fallbacks(const AnygmContentSource *source){
  DummyHost dummy={0};
  AnygmHostServices services={0};
  services.struct_size=sizeof services;
  services.abi_version=ANYGM_HOST_SERVICES_VERSION;
  services.userdata=&dummy;
  services.locale=unsupported_locale;
  services.wall_time=unsupported_wall_time;

  AnygmEngine *engine=NULL;
  if(anygm_create(&services,&engine)!=ANYGM_OK || !engine)
    return fail("fallback host creation failed");
  if(anygm_load(engine,source,NULL)!=ANYGM_OK)
    return fail("optional-service fallback load failed");
  AnygmFrameOutput output={0};
  output.struct_size=sizeof output;
  if(anygm_run_frame(engine,NULL,&output)!=ANYGM_OK || !output.pixels || !output.audio)
    return fail("optional-service fallback frame failed");
  anygm_destroy(engine);
  if(dummy.locale_calls!=1 || !dummy.wall_calls)
    return fail("optional-service failure paths were not exercised");
  return 0;
}

static int exercise_unavailable_path_service(void){
  DummyHost dummy={0};
  AnygmHostServices services={0};
  services.struct_size=sizeof services;
  services.abi_version=ANYGM_HOST_SERVICES_VERSION;
  services.userdata=&dummy;
  services.file_open=unavailable_file_open;
  services.file_read=unavailable_file_read;
  services.file_close=unavailable_file_close;
  services.file_stat=unavailable_file_stat;

  AnygmEngine *engine=NULL;
  if(anygm_create(&services,&engine)!=ANYGM_OK || !engine)
    return fail("unavailable-path host creation failed");
  AnygmContentSource source={0};
  source.struct_size=sizeof source;
  source.kind=ANYGM_CONTENT_PATH;
  source.path="unavailable-content.win";
  if(anygm_load(engine,&source,NULL)!=ANYGM_ERROR_INVALID_CONTENT){
    anygm_destroy(engine);
    return fail("unavailable host path was accepted");
  }
  if(anygm_state_size(engine)!=0){
    anygm_destroy(engine);
    return fail("failed path load changed lifecycle state");
  }
  anygm_destroy(engine);
  if(!dummy.log_calls) return fail("unavailable file service was not called");
  return 0;
}

int main(void){
  AnygmSyntheticContent fixture;
  if(!anygm_synthetic_content_create(&fixture)) return fail("could not create synthetic content");
  uint8_t *content=NULL;
  size_t content_size=0;
  if(!anygm_synthetic_content_read(&fixture,&content,&content_size))
    return fail("could not read synthetic content");
  const char *content_output=getenv("ANYGM_TEST_CONTENT_OUTPUT");
  if(content_output && *content_output){
    FILE *file=fopen(content_output,"wb");
    int ok=file && fwrite(content,1,content_size,file)==content_size;
    if(file && fclose(file)!=0) ok=0;
    if(!ok) return fail("could not export synthetic content");
  }

  DummyHost dummy={0};
  AnygmHostServices services={0};
  services.struct_size=sizeof services;
  services.abi_version=ANYGM_HOST_SERVICES_VERSION;
  services.userdata=&dummy;
  services.log=dummy_log;
  services.monotonic_time_ns=dummy_monotonic_time_ns;
  services.wall_time=dummy_wall_time;
  services.random_seed=dummy_random_seed;
  anygm_stdio_vfs_services_init(&services);
  services.locale=dummy_locale;

  if(anygm_api_version()!=ANYGM_API_VERSION) return fail("API version mismatch");
  if(anygm_create(&services,NULL)!=ANYGM_ERROR_INVALID_ARGUMENT)
    return fail("create accepted a null output");
  AnygmHostServices incompatible=services;
  incompatible.abi_version=ANYGM_HOST_SERVICES_VERSION+1;
  AnygmEngine *rejected=(AnygmEngine *)(uintptr_t)1;
  if(anygm_create(&incompatible,&rejected)!=ANYGM_ERROR_INCOMPATIBLE_ABI || rejected)
    return fail("create accepted an incompatible host version");
  incompatible=services;
  incompatible.struct_size=sizeof incompatible-1;
  rejected=(AnygmEngine *)(uintptr_t)1;
  if(anygm_create(&incompatible,&rejected)!=ANYGM_ERROR_INCOMPATIBLE_ABI || rejected)
    return fail("create accepted a short host structure");

  AnygmEngine *engine=NULL;
  if(anygm_create(&services,&engine)!=ANYGM_OK || !engine) return fail("create failed");

  anygm_unload(engine);
  if(anygm_reset(engine)!=ANYGM_ERROR_INVALID_STATE || anygm_state_size(engine)!=0 ||
     anygm_set_runtime_override(engine,0,1,"global.flag=1")!=ANYGM_ERROR_INVALID_STATE)
    return fail("empty-engine lifecycle operation was accepted");
  uint8_t one_byte=0;
  size_t empty_written=123;
  if(anygm_state_save(engine,&one_byte,sizeof one_byte,&empty_written)!=ANYGM_ERROR_INVALID_STATE ||
     empty_written!=0 ||
     anygm_state_load(engine,&one_byte,sizeof one_byte)!=ANYGM_ERROR_INVALID_STATE)
    return fail("empty-engine state operation was accepted");

  AnygmConfigDelta delta={0};
  delta.struct_size=sizeof delta;
  delta.values.struct_size=sizeof delta.values;
  delta.fields=ANYGM_CONFIG_FAST_ALPHA_CULL;
  delta.values.fast_alpha_cull=16;
  if(anygm_set_config(engine,&delta)!=ANYGM_OK)
    return fail("empty-engine configuration was rejected");
  AnygmConfigDelta short_delta=delta;
  short_delta.struct_size=sizeof short_delta-1;
  if(anygm_set_config(engine,&short_delta)!=ANYGM_ERROR_INCOMPATIBLE_ABI)
    return fail("short configuration structure was accepted");

  AnygmInputFrame input={0};
  input.struct_size=sizeof input;
  input.pointer_x=input.pointer_y=-1;
  AnygmFrameOutput output={0};
  output.struct_size=sizeof output;
  if(anygm_run_frame(engine,&input,&output)!=ANYGM_ERROR_INVALID_STATE)
    return fail("run before load was accepted");

  uint8_t invalid_image[4]={0};
  AnygmContentSource invalid_source={0};
  invalid_source.struct_size=sizeof invalid_source;
  invalid_source.kind=ANYGM_CONTENT_MEMORY;
  invalid_source.path="invalid-content.win";
  invalid_source.data=invalid_image;
  invalid_source.size=sizeof invalid_image;
  AnygmLoadConfig explicit_locale={0};
  explicit_locale.struct_size=sizeof explicit_locale;
  explicit_locale.language="en";
  explicit_locale.region="us";
  explicit_locale.language_tag="en-US";
  if(anygm_load(engine,&invalid_source,&explicit_locale)!=ANYGM_ERROR_INVALID_CONTENT ||
     anygm_state_size(engine)!=0)
    return fail("invalid content changed lifecycle state");

  AnygmContentSource short_source={0};
  short_source.struct_size=sizeof short_source-1;
  if(anygm_load(engine,&short_source,NULL)!=ANYGM_ERROR_INVALID_ARGUMENT)
    return fail("short content structure was accepted");
  AnygmLoadConfig short_load_config=explicit_locale;
  short_load_config.struct_size=sizeof short_load_config-1;

  AnygmContentSource source={0};
  source.struct_size=sizeof source;
  source.kind=ANYGM_CONTENT_MEMORY;
  source.path="synthetic-content.win";
  source.data=content;
  source.size=content_size;
  if(anygm_load(engine,&source,&short_load_config)!=ANYGM_ERROR_INCOMPATIBLE_ABI)
    return fail("short load configuration was accepted");
  if(anygm_load(engine,&source,NULL)!=ANYGM_OK) return fail("memory load failed");
  if(dummy.locale_calls!=1) return fail("locale was not captured exactly once at load");

  if(anygm_load(engine,&source,NULL)!=ANYGM_ERROR_INVALID_STATE)
    return fail("second load was accepted");

  AnygmAvInfo av={0};
  av.struct_size=sizeof av;
  if(anygm_get_av_info(engine,&av)!=ANYGM_OK || !av.base_width || !av.base_height)
    return fail("A/V description failed");
  AnygmAvInfo short_av={0};
  short_av.struct_size=sizeof short_av-1;
  if(anygm_get_av_info(engine,&short_av)!=ANYGM_ERROR_INCOMPATIBLE_ABI)
    return fail("short A/V structure was accepted");
  AnygmInputFrame short_input=input;
  short_input.struct_size=sizeof short_input-1;
  if(anygm_run_frame(engine,&short_input,&output)!=ANYGM_ERROR_INCOMPATIBLE_ABI)
    return fail("short input structure was accepted");
  AnygmFrameOutput short_output=output;
  short_output.struct_size=sizeof short_output-1;
  if(anygm_run_frame(engine,&input,&short_output)!=ANYGM_ERROR_INCOMPATIBLE_ABI)
    return fail("short output structure was accepted");
  if(anygm_run_frame(engine,&input,&output)!=ANYGM_OK || !output.pixels || !output.audio ||
     !output.width || !output.height || output.audio_rate!=44100)
    return fail("frame pull failed");

  size_t capacity=anygm_state_size(engine),written=0;
  uint8_t *baseline=malloc(capacity?capacity:1);
  uint8_t *restored=malloc(capacity?capacity:1);
  if(!baseline || !restored ||
     anygm_state_save(engine,baseline,capacity,&written)!=ANYGM_OK || !written)
    return fail("state save failed");
  size_t baseline_size=written;
  written=123;
  if(anygm_state_save(engine,restored,1,&written)!=ANYGM_ERROR_OUT_OF_MEMORY ||
     written<=1)
    return fail("short state destination was accepted");
  if(anygm_state_load(engine,&one_byte,sizeof one_byte)!=ANYGM_ERROR_STATE_MISMATCH)
    return fail("invalid state image was accepted");
  AnygmConfigDelta presentation_delta={0};
  presentation_delta.struct_size=sizeof presentation_delta;
  presentation_delta.values.struct_size=sizeof presentation_delta.values;
  presentation_delta.fields=ANYGM_CONFIG_PRESENT_WIDTH|ANYGM_CONFIG_PRESENT_HEIGHT|
                            ANYGM_CONFIG_ASPECT_MODE;
  presentation_delta.values.present_width=640;
  presentation_delta.values.present_height=360;
  presentation_delta.values.aspect_mode=2;
  if(anygm_set_config(engine,&presentation_delta)!=ANYGM_OK ||
     anygm_state_load(engine,baseline,baseline_size)!=ANYGM_OK)
    return fail("presentation configuration prevented state loading");
  presentation_delta.values.present_width=0;
  presentation_delta.values.present_height=0;
  presentation_delta.values.aspect_mode=0;
  if(anygm_set_config(engine,&presentation_delta)!=ANYGM_OK)
    return fail("presentation configuration could not be restored");
  AnygmConfigDelta stateful_delta={0};
  stateful_delta.struct_size=sizeof stateful_delta;
  stateful_delta.values.struct_size=sizeof stateful_delta.values;
  stateful_delta.fields=ANYGM_CONFIG_GOD_MODE;
  stateful_delta.values.god_mode=1;
  if(anygm_set_config(engine,&stateful_delta)!=ANYGM_OK ||
     anygm_state_load(engine,baseline,baseline_size)!=ANYGM_ERROR_STATE_MISMATCH)
    return fail("stateful configuration mismatch was accepted");
  stateful_delta.values.god_mode=0;
  if(anygm_set_config(engine,&stateful_delta)!=ANYGM_OK)
    return fail("stateful configuration could not be restored");
  output.struct_size=sizeof output;
  if(anygm_run_frame(engine,&input,&output)!=ANYGM_OK ||
     anygm_state_load(engine,baseline,baseline_size)!=ANYGM_OK ||
     anygm_state_save(engine,restored,capacity,&written)!=ANYGM_OK ||
     written!=baseline_size || memcmp(baseline,restored,baseline_size))
    return fail("state roundtrip changed the engine");

  if(anygm_reset(engine)!=ANYGM_OK || anygm_run_frame(engine,NULL,&output)!=ANYGM_OK)
    return fail("reset did not produce a runnable loaded engine");

  free(restored);
  free(baseline);
  anygm_unload(engine);
  if(anygm_get_av_info(engine,&av)!=ANYGM_ERROR_INVALID_STATE)
    return fail("query after unload was accepted");
  anygm_unload(engine);
  if(anygm_load(engine,&source,&explicit_locale)!=ANYGM_OK)
    return fail("reload after unload failed");
  anygm_destroy(engine);

  if(exercise_optional_service_fallbacks(&source) || exercise_unavailable_path_service())
    return 1;
  free(content);
  anygm_synthetic_content_destroy(&fixture);
  if(!dummy.log_calls) return fail("host logger was never used");
  if(!dummy.monotonic_calls || dummy.wall_calls!=3 || dummy.seed_calls!=3){
    char message[160];
    snprintf(message,sizeof message,
             "host service counts differ: monotonic=%u wall=%u seed=%u",
             dummy.monotonic_calls,dummy.wall_calls,dummy.seed_calls);
    return fail(message);
  }
  puts("dummy host contract: ok");
  return 0;
}
