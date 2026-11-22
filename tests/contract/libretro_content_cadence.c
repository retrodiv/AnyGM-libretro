/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Content that changes its own speed within a room. A single slowed step holds the picture on the
 * frontend's existing clock, with sound continuing, and never reconfigures the frontend; a lasting
 * change is announced once, after one second of presented ticks. */
#include "libretro_internal.h"
#include "engine_internal.h"
#include "gml_builtin.h"
#include "synthetic_content.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool running,invalid_callback;
static unsigned videos,av_calls,silent_ticks;
static size_t samples,tick_samples;
static double announced_fps;

static bool environment(unsigned command,void *data){
  if(command==RETRO_ENVIRONMENT_SET_PIXEL_FORMAT) return true;
  if(command==RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO){
    av_calls++;
    if(!running) invalid_callback=true;
    announced_fps=((const struct retro_system_av_info*)data)->timing.fps;
    return true;
  }
  if(command==RETRO_ENVIRONMENT_SET_GEOMETRY) return true;
  if(command==RETRO_ENVIRONMENT_GET_VARIABLE){
    struct retro_variable *variable=data;
    variable->value=NULL;
    if(!strcmp(variable->key,"anygm_scene_renderer")) variable->value="CPU";
    return variable->value!=NULL;
  }
  return false;
}
static void video(const void *data,unsigned width,unsigned height,size_t pitch){
  (void)data; (void)width; (void)height; (void)pitch; videos++;
}
static size_t audio(const int16_t *data,size_t count){
  (void)data; samples+=count; tick_samples+=count; return count;
}
static int16_t input_state(unsigned port,unsigned device,unsigned index,unsigned id){
  (void)port; (void)device; (void)index; (void)id; return 0;
}
static void tick(void){
  tick_samples=0;
  running=true; retro_run(); running=false;
  if(!tick_samples) silent_ticks++;
}
static void speed(double fps){
  GmlVal arguments[]={vreal(fps),vreal(0)};
  gml_builtin_call(&g_libretro.engine->vm,"game_set_speed",arguments,2);
}
#define REQUIRE(condition,message) do { if(!(condition)){ \
  fprintf(stderr,"content cadence: %s\n",message); ok=0; goto done; \
} } while(0)

int main(void){
  AnygmSyntheticContent fixture={0};
  int ok=1;
  REQUIRE(anygm_synthetic_content_create(&fixture),"create synthetic source");
  retro_set_environment(environment); retro_set_video_refresh(video);
  retro_set_audio_sample_batch(audio); retro_init();
  retro_set_input_state(input_state);
  struct retro_game_info content={fixture.path,NULL,0,NULL};
  REQUIRE(retro_load_game(&content),"load synthetic source");
  struct retro_system_av_info av; retro_get_system_av_info(&av);
  REQUIRE(av.timing.fps==60,"startup cadence");
  for(int i=0;i<4;i++) tick();
  unsigned calls=av_calls; long frame=g_libretro.engine->vm.frame;

  /* One step at eight steps per second, as content does to mark an impact. */
  speed(8); samples=0; silent_ticks=0;
  unsigned held=0;
  while(g_libretro.engine->vm.frame==frame && held<20){ tick(); held++; }
  REQUIRE(g_libretro.engine->vm.frame==frame+1,"the slowed step runs");
  REQUIRE(held==7 || held==8,"the picture holds for the step's authored duration");
  speed(60);
  for(int i=0;i<60;i++) tick();
  REQUIRE(av_calls==calls,"a momentary cadence never reconfigures the frontend");
  REQUIRE(announced_fps==0 || announced_fps==60,"no other cadence was announced");
  REQUIRE(!silent_ticks,"sound continues through every held tick");
  REQUIRE(g_libretro.engine->vm.frame==frame+61,"sixty ticks at the restored cadence");
  /* 7/60 s of held ticks plus 1/8 s for the slowed step's remainder and 60/60 s: the mixer
   * follows the executed steps' total. */
  REQUIRE(samples+735>=44100+5512 && samples<=44100+5512+735,"audio duration follows the steps");

  /* A lasting change reaches the frontend once, after one second of presented ticks. */
  speed(30); calls=av_calls;
  for(int i=0;i<59;i++) tick();
  REQUIRE(av_calls==calls,"a new cadence waits one second");
  /* The second tick runs the first step at the new cadence, which is when the host observes it. */
  tick(); tick();
  REQUIRE(av_calls==calls+1 && announced_fps==30,"a lasting cadence is announced once");
  frame=g_libretro.engine->vm.frame;
  for(int i=0;i<10;i++) tick();
  REQUIRE(g_libretro.engine->vm.frame==frame+10 && av_calls==calls+1,
          "after the announcement each tick is one step");
  struct retro_system_av_info again; retro_get_system_av_info(&again);
  REQUIRE(again.timing.fps==30,"a fresh AV answer carries the held cadence");
  REQUIRE(!invalid_callback,"every AV callback occurs inside retro_run");
  printf("content cadence: held=%u videos=%u av_calls=%u\n",held,videos,av_calls);
done:
  retro_unload_game(); retro_deinit(); anygm_synthetic_content_destroy(&fixture);
  return ok?EXIT_SUCCESS:EXIT_FAILURE;
}
