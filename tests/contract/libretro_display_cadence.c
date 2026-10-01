/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "libretro_internal.h"
#include "engine_internal.h"
#include "gml_builtin.h"
#include "synthetic_content.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int negotiation;
static bool running,invalid_callback;
static unsigned videos,av_calls;
static size_t samples;
static uint32_t target_hz,mode_pending;
static double announced_hz;
static int16_t input_state(unsigned port,unsigned device,unsigned index,unsigned id){
  (void)port; (void)index;
  return device==RETRO_DEVICE_MOUSE && id==RETRO_DEVICE_ID_MOUSE_X?1:0;
}
static bool environment(unsigned command,void *data){
  if(command==RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS){
    if(negotiation>0) *(uint64_t*)data|=RETRO_SERIALIZATION_QUIRK_FRONT_VARIABLE_SIZE;
    return negotiation>=0;
  }
  if(command==RETRO_ENVIRONMENT_SET_PIXEL_FORMAT) return true;
  if(command==RETRO_ENVIRONMENT_GET_TARGET_REFRESH_RATE){
    *(float*)data=(float)target_hz; return true;
  }
  if(command==RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO){
    av_calls++;
    if(!running) invalid_callback=true;
    double hz=((const struct retro_system_av_info*)data)->timing.fps;
    if(hz!=50 && hz!=60) return false;
    announced_hz=hz; mode_pending=(uint32_t)hz; return true;
  }
  if(command==RETRO_ENVIRONMENT_SET_GEOMETRY) return true;
  if(command==RETRO_ENVIRONMENT_GET_VARIABLE){
    struct retro_variable *variable=data;
    variable->value=NULL;
    if(!strcmp(variable->key,"anygm_render_game_resolution")) variable->value="On";
    if(!strcmp(variable->key,"anygm_scene_renderer")) variable->value="CPU";
    if(!strcmp(variable->key,"anygm_hybrid_gpu")) variable->value="None";
    return variable->value!=NULL;
  }
  return false;
}
static void video(const void *data,unsigned width,unsigned height,size_t pitch){
  (void)data; (void)width; (void)height; (void)pitch; videos++;
  /* Apply the accepted mode on video submission, not in the AV callback. */
  if(mode_pending){ target_hz=mode_pending; mode_pending=0; }
}
static size_t audio(const int16_t *data,size_t count){
  (void)data; samples+=count; return count;
}
static void tick(void){ running=true; retro_run(); running=false; }
static double call(const char *name,double value){
  GmlVal argument=vreal(value);
  return gml_builtin_call(&g_libretro.engine->vm,name,&argument,1).d;
}
static void speed(double fps){
  GmlVal arguments[]={vreal(fps),vreal(0)};
  gml_builtin_call(&g_libretro.engine->vm,"game_set_speed",arguments,2);
}
static int snapshot(uint8_t **bytes,size_t *capacity,size_t initial){
  size_t answer=retro_serialize_size();
  if(!answer || (negotiation<=0 && answer!=initial)) return 0;
  if(answer>*capacity){
    if(negotiation<=0) return 0;
    uint8_t *next=realloc(*bytes,answer);
    if(!next) return 0;
    *bytes=next; *capacity=answer;
  }
  return retro_serialize(*bytes,*capacity);
}
#define REQUIRE(condition,message) do { if(!(condition)){ \
  fprintf(stderr,"display cadence (%d): %s\n",negotiation,message); ok=0; goto done; \
} } while(0)
static int run_case(int response){
  AnygmSyntheticContent fixture={0};
  uint8_t *ring=NULL,*saved=NULL,*before=NULL,*after=NULL;
  size_t capacity=0,initial=0,saved_capacity=0;
  int ok=1;
  negotiation=response; running=invalid_callback=false;
  videos=av_calls=0; samples=0; target_hz=60; mode_pending=0; announced_hz=60;
  REQUIRE(anygm_synthetic_display_cadence_content_create(&fixture),"create synthetic source");
  retro_set_environment(environment); retro_set_video_refresh(video);
  retro_set_audio_sample_batch(audio); retro_init();
  retro_set_input_state(input_state);
  struct retro_game_info content={fixture.path,NULL,0,NULL};
  REQUIRE(retro_load_game(&content),"load synthetic source");
  const GmlVal *created=gml_varmap_get(&g_libretro.engine->vm.globals,"fixture_display_result");
  REQUIRE(created && created->d==1,"authored Create enqueues cold request");
  REQUIRE(av_calls==0 && call("display_get_frequency",0)==60,
          "no AV callback outside retro_run and no synthetic getter success");
  struct retro_system_av_info av; retro_get_system_av_info(&av);
  REQUIRE(av.timing.fps==50,"initial AV carries requested presentation cadence");
  capacity=initial=retro_serialize_size();
  REQUIRE(capacity>0,"reserve frontend ring before first frame");
  ring=malloc(capacity);
  REQUIRE(ring && snapshot(&ring,&capacity,initial),"cold frontend snapshot");
  long frame=g_libretro.engine->vm.frame;
  int mouse_x=g_libretro.engine->mouse_pixel_x<0?
    (int)g_libretro.engine->output_width/2:(int)g_libretro.engine->mouse_pixel_x;
  for(int i=0;i<10;i++) tick();
  REQUIRE(videos==10 && g_libretro.engine->vm.frame-frame==6,
          "ten presentation ticks retain six logical steps at thirty FPS");
  REQUIRE(samples==8820,"thirty-FPS mixer advances six times, not ten");
  REQUIRE(g_libretro.engine->mouse_pixel_x==mouse_x+10,
          "relative input survives zero-step presentation ticks");
  REQUIRE(!invalid_callback && call("display_get_frequency",0)==50,
          "target observation changes only after frontend submission");
  REQUIRE(snapshot(&ring,&capacity,initial),"ring after completed frames");
  speed(60); frame=g_libretro.engine->vm.frame; samples=0;
  mouse_x=g_libretro.engine->mouse_pixel_x;
  for(int i=0;i<10;i++) tick();
  REQUIRE(g_libretro.engine->vm.frame-frame==12 && samples==8820,
          "multiple logical steps preserve sixty FPS and audio duration");
  REQUIRE(g_libretro.engine->mouse_pixel_x==mouse_x+10,
          "multiple logical steps consume relative input only once");
  REQUIRE(snapshot(&ring,&capacity,initial),"ring after logical speed change");
  REQUIRE(call("display_set_frequency",60)==1,"later display request"); tick();
  REQUIRE(g_libretro.display_active_hz==60 && announced_hz==60,"later request accepted");
  REQUIRE(call("display_set_frequency",53)==1,"queue differs from frontend acceptance"); tick();
  REQUIRE(g_libretro.display_active_hz==60 && announced_hz==60 && target_hz==60,
          "frontend refusal preserves cadence and observed target");
  REQUIRE(call("display_set_frequency",50)==1,"subsequent valid request");
  speed(30); tick();
  REQUIRE(snapshot(&ring,&capacity,initial),"ring after display changes");
  saved_capacity=capacity; saved=malloc(saved_capacity);
  REQUIRE(saved,"allocate same-run restore source"); memcpy(saved,ring,saved_capacity);
  double phase=g_libretro.engine->presentation_phase;
  long saved_frame=g_libretro.engine->vm.frame;
  for(int i=0;i<7;i++) tick();
  long expected=g_libretro.engine->vm.frame;
  REQUIRE(retro_unserialize(saved,saved_capacity),"same-run clock restore");
  REQUIRE(g_libretro.engine->presentation_phase==phase &&
          g_libretro.engine->vm.frame==saved_frame,"restore fractional clock and logical state");
  tick();
  REQUIRE(g_libretro.engine->presentation_phase==phase &&
          g_libretro.engine->vm.frame==saved_frame,"restore picture does not advance either clock");
  for(int i=0;i<7;i++) tick();
  REQUIRE(g_libretro.engine->vm.frame==expected,"rewind repeats logical cadence exactly");
  REQUIRE(snapshot(&ring,&capacity,initial),"ring after restoration");
  before=malloc(capacity); after=malloc(capacity);
  REQUIRE(before && after && retro_serialize(before,capacity),"capture canonical bytes");
  saved[4]=(uint8_t)(ANYGM_STATE_SCHEMA-1);
  REQUIRE(!retro_unserialize(saved,saved_capacity),"older root schema rejected");
  REQUIRE(retro_serialize(after,capacity) && !memcmp(before,after,capacity),
          "schema rejection leaves engine transactionally unchanged");
  /* A restored VM with no display request returns to logical cadence. Here the frontend
   * refuses that thirty-Hz cadence; neither the active clock nor its observation may change. */
  *gml_varmap_put(&g_libretro.engine->vm.globals,"__display_request_hz")=vreal(0);
  speed(30); tick();
  REQUIRE(g_libretro.display_requested_hz==0 && g_libretro.display_active_hz==50 &&
          announced_hz==50 && target_hz==50,"refused restoration keeps accepted cadence");
  REQUIRE(call("display_set_frequency",50)==1,"request after refused restoration"); tick();
  retro_reset();
  REQUIRE(g_libretro.av.display_frequency_request_hz==50,
          "Reset retains session mode when Create observes the requested frequency");
  REQUIRE(g_libretro.engine->presentation_phase==0 && g_libretro.engine->presentation_hz==0,
          "Reset clears presentation clock");
  REQUIRE(snapshot(&ring,&capacity,initial),"old ring accepts Reset cold snapshot"); tick();
  REQUIRE(g_libretro.display_active_hz==50,"Reset reapplies authored Create request");
  REQUIRE(snapshot(&ring,&capacity,initial),"ring accepts first reset frame");
  REQUIRE(!invalid_callback,"every AV callback occurs inside retro_run");
  printf("display cadence: negotiation=%d initial=%zu final=%zu videos=%u\n",
         response,initial,capacity,videos);
done:
  free(ring); free(saved); free(before); free(after);
  retro_deinit(); anygm_synthetic_content_destroy(&fixture); return ok;
}
int main(void){
  for(int response=-1;response<=1;response++) if(!run_case(response)) return EXIT_FAILURE;
  return EXIT_SUCCESS;
}
