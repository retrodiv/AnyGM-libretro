/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Content that changes its own speed. Within a room the frontend keeps its clock however long the
 * change lasts: a slowed step holds the picture with sound continuing, slow motion runs on the held
 * clock, and neither reconfigures the frontend, nor does rewinding into it. A room that starts while
 * the content runs at another cadence announces that cadence once, and a speed set by a new room's
 * first step is announced at once, as the room's own. */
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
static void go_to_room(int room){
  GmlVal argument=vreal(room);
  gml_builtin_call(&g_libretro.engine->vm,"room_goto",&argument,1);
}
static long frame_now(void){ return g_libretro.engine->vm.frame; }
static int room_now(void){ return g_libretro.engine->vm.room_index; }
#define REQUIRE(condition,message) do { if(!(condition)){ \
  fprintf(stderr,"content cadence: %s\n",message); ok=0; goto done; \
} } while(0)

int main(void){
  AnygmSyntheticContent fixture={0};
  void *state=NULL;
  int ok=1;
  REQUIRE(anygm_synthetic_tilemap_content_create(&fixture),"create two-room synthetic source");
  retro_set_environment(environment); retro_set_video_refresh(video);
  retro_set_audio_sample_batch(audio); retro_init();
  retro_set_input_state(input_state);
  struct retro_game_info content={fixture.path,NULL,0,NULL};
  REQUIRE(retro_load_game(&content),"load synthetic source");
  struct retro_system_av_info av; retro_get_system_av_info(&av);
  REQUIRE(av.timing.fps==60,"startup cadence");
  for(int i=0;i<4;i++) tick();
  unsigned calls=av_calls; long frame=frame_now();

  /* One step at eight steps per second, as content does to mark an impact. */
  speed(8); samples=0; silent_ticks=0;
  unsigned held=0;
  while(frame_now()==frame && held<20){ tick(); held++; }
  REQUIRE(frame_now()==frame+1,"the slowed step runs");
  REQUIRE(held==7 || held==8,"the picture holds for the step's authored duration");
  speed(60);
  for(int i=0;i<60;i++) tick();
  REQUIRE(av_calls==calls,"an impact pause never reconfigures the frontend");
  REQUIRE(!silent_ticks,"sound continues through every held tick");
  REQUIRE(frame_now()==frame+61,"sixty ticks at the restored cadence");
  /* 7/60 s of held ticks plus 1/8 s for the slowed step's remainder and 60/60 s: the mixer
   * follows the executed steps' total. */
  REQUIRE(samples+735>=44100+5512 && samples<=44100+5512+735,"audio duration follows the steps");

  /* Slow motion lasting three seconds stays on the frontend's clock throughout. */
  speed(30); frame=frame_now(); silent_ticks=0;
  size_t state_size=retro_serialize_size();
  state=malloc(state_size?state_size:1);
  REQUIRE(state!=NULL,"state buffer");
  for(int i=0;i<180;i++){
    tick();
    if(i==90) REQUIRE(retro_serialize(state,state_size),"save during slow motion");
  }
  REQUIRE(av_calls==calls,"slow motion within a room never reconfigures the frontend");
  REQUIRE(frame_now()>=frame+89 && frame_now()<=frame+91,"slow motion runs one step every other tick");
  REQUIRE(!silent_ticks,"sound continues through slow motion");
  /* Rewinding into slow motion resumes the room; it does not start it. */
  REQUIRE(retro_unserialize(state,state_size),"load during slow motion");
  for(int i=0;i<20;i++) tick();
  REQUIRE(av_calls==calls,"rewinding into slow motion never reconfigures the frontend");

  /* A room that starts while the content still runs at another cadence announces it once. */
  int first_room=room_now();
  go_to_room(first_room?0:1);
  tick(); tick();
  REQUIRE(room_now()!=first_room,"the room changes");
  REQUIRE(av_calls==calls+1 && announced_fps==30,"the next room start adopts the content cadence");
  frame=frame_now();
  for(int i=0;i<10;i++) tick();
  REQUIRE(frame_now()==frame+10 && av_calls==calls+1,"after the announcement each tick is one step");
  struct retro_system_av_info again; retro_get_system_av_info(&again);
  REQUIRE(again.timing.fps==30,"a fresh AV answer carries the adopted cadence");

  /* A speed set by a new room's first step is that room's cadence and is announced at once. */
  calls=av_calls;
  go_to_room(first_room);
  while(room_now()!=first_room && av_calls==calls) tick();
  REQUIRE(room_now()==first_room,"the room changes back");
  speed(60);
  tick();
  REQUIRE(av_calls==calls+1 && announced_fps==60,"a room's first-step speed is announced at once");

  /* Later in that room, a change is held again. */
  for(int i=0;i<5;i++) tick();
  calls=av_calls;
  speed(20);
  for(int i=0;i<90;i++) tick();
  REQUIRE(av_calls==calls,"a change later in the room is held");
  REQUIRE(!invalid_callback,"every AV callback occurs inside retro_run");
  printf("content cadence: held=%u videos=%u av_calls=%u\n",held,videos,av_calls);
done:
  free(state);
  retro_unload_game(); retro_deinit(); anygm_synthetic_content_destroy(&fixture);
  return ok?EXIT_SUCCESS:EXIT_FAILURE;
}
