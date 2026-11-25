/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Content that changes its own speed. Within a room the frontend keeps its clock however long the
 * change lasts: a slowed step holds the picture with sound continuing, slow motion runs on the held
 * clock, and neither reconfigures the frontend, nor does rewinding into it. A room that starts while
 * the content runs at another cadence announces that cadence once, and a speed set by a new room's
 * first step is announced at once, as the room's own. A declared CRT mode holds one padded raster
 * and refresh across such changes, and its room cadence never enters content state. */
#include "libretro_internal.h"
#include "engine_internal.h"
#include "gml_builtin.h"
#include "synthetic_content.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool running,invalid_callback,refuse_av;
static unsigned videos,av_calls,silent_ticks;
static size_t samples,tick_samples;
static double announced_fps;
static unsigned announced_width,geometry_calls,video_width,video_height;
static int crt_on,options_changed,overrides_on=1,negotiation,mouse_delta;

static bool environment(unsigned command,void *data){
  if(command==RETRO_ENVIRONMENT_SET_PIXEL_FORMAT) return true;
  if(command==RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS){
    if(negotiation>0) *(uint64_t*)data|=RETRO_SERIALIZATION_QUIRK_FRONT_VARIABLE_SIZE;
    return negotiation>=0;
  }
  if(command==RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO){
    const struct retro_system_av_info *info=data;
    av_calls++;
    if(!running) invalid_callback=true;
    announced_fps=info->timing.fps;
    announced_width=info->geometry.base_width;
    return !refuse_av;
  }
  if(command==RETRO_ENVIRONMENT_SET_GEOMETRY){ geometry_calls++; return true; }
  if(command==RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE){
    *(bool*)data=options_changed!=0; options_changed=0; return true;
  }
  if(command==RETRO_ENVIRONMENT_GET_VARIABLE){
    struct retro_variable *variable=data;
    variable->value=NULL;
    if(!strcmp(variable->key,"anygm_scene_renderer")) variable->value="CPU";
    if(!strcmp(variable->key,"anygm_adjust_crt_tv")) variable->value=crt_on?"On":"Off";
    if(!strcmp(variable->key,"anygm_render_game_resolution")) variable->value="On";
    if(!strcmp(variable->key,"anygm_content_overrides")) variable->value=overrides_on?"On":"Off";
    return variable->value!=NULL;
  }
  return false;
}
static void video(const void *data,unsigned width,unsigned height,size_t pitch){
  (void)data; (void)pitch; videos++; video_width=width; video_height=height;
}
static size_t audio(const int16_t *data,size_t count){
  (void)data; samples+=count; tick_samples+=count; return count;
}
static int16_t input_state(unsigned port,unsigned device,unsigned index,unsigned id){
  (void)port; (void)index;
  return device==RETRO_DEVICE_MOUSE && id==RETRO_DEVICE_ID_MOUSE_X?mouse_delta:0;
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

static int held_cadence_case(void){
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
  return ok;
}

static void options(int crt,int overrides){
  crt_on=crt; overrides_on=overrides; options_changed=1;
}
static int enter_room(int room){
  go_to_room(room);
  for(int i=0;i<4 && room_now()!=room;i++) tick();
  return room_now()==room;
}
static int snapshot(void *ring,size_t capacity){
  size_t answer=retro_serialize_size();
  return answer && answer<=capacity && retro_serialize(ring,capacity);
}

static int automatic_crt_case(int boot_speed,int refresh){
  AnygmSyntheticContent fixture={0};
  int ok=1;
  negotiation=0; crt_on=1; overrides_on=1; options_changed=0;
  invalid_callback=false;
  REQUIRE(anygm_synthetic_cold_room_content_create(&fixture,boot_speed),"create automatic CRT source");
  retro_set_environment(environment); retro_set_video_refresh(video);
  retro_set_audio_sample_batch(audio); retro_init();
  struct retro_game_info content={fixture.path,NULL,0,NULL};
  REQUIRE(retro_load_game(&content),"load without a declared mode");
  struct retro_system_av_info av; retro_get_system_av_info(&av);
  REQUIRE(g_libretro.av.frames_per_second==boot_speed,"the boot room keeps its authored speed");
  REQUIRE(av.timing.fps==refresh,"automatic CRT startup reports the expected refresh");

  unsigned calls=av_calls;
  for(int i=0;i<4;i++) tick();
  long frame=frame_now();
  samples=0; silent_ticks=0;
  for(int i=0;i<refresh;i++) tick();
  REQUIRE(frame_now()==frame+boot_speed && av_calls==calls,
          "one second of refreshes executes one second of logical steps");
  printf("automatic CRT: %d steps per second at %d Hz, %zu audio frames\n",
         boot_speed,refresh,samples);
  REQUIRE(!silent_ticks && samples==44100,"audio crosses every refresh and keeps its duration");

  bool multiplied=refresh!=boot_speed;
  options(0,1);
  tick();
  REQUIRE(av_calls==calls+multiplied && (!multiplied || announced_fps==boot_speed),
          "turning CRT adjustment off restores the authored clock when needed");
  frame=frame_now();
  for(int i=0;i<8;i++) tick();
  REQUIRE(frame_now()==frame+8,"ordinary display clocks advance at one step per tick");

  options(1,1);
  tick();
  REQUIRE(av_calls==calls+2*multiplied && (!multiplied || announced_fps==refresh),
          "turning CRT adjustment back on restores the repeat cadence when needed");
  if(boot_speed==30){
    calls=av_calls;
    speed(60);
    REQUIRE(enter_room(1),"enter a full-rate room with the same CRT refresh");
    frame=frame_now();
    for(int i=0;i<8;i++) tick();
    REQUIRE(frame_now()==frame+8 && av_calls==calls,
            "a new room keeping the same CRT refresh does not rebuild the frontend");
    retro_reset();
    for(int i=0;i<4;i++) tick();
    REQUIRE(av_calls==calls && g_libretro.av.frames_per_second==30,
            "Reset returns to half-speed logical steps on the same CRT refresh");
    options(0,1);
    tick();
    calls=av_calls;
    refuse_av=true;
    options(1,1);
    tick();
    frame=frame_now();
    for(int i=0;i<8;i++) tick();
    REQUIRE(av_calls==calls+1 && frame_now()==frame+8,
            "a refused CRT transition schedules against the preceding frontend clock");
  }
  REQUIRE(!invalid_callback,"automatic CRT updates run inside retro_run");
done:
  refuse_av=false;
  retro_unload_game(); retro_deinit(); anygm_synthetic_content_destroy(&fixture);
  return ok;
}

static int declared_mode_case(int response,const char *earlier){
  AnygmSyntheticContent fixture={0};
  void *ring=NULL,*plain=NULL,*in_mode=NULL;
  FILE *file=NULL;
  int ok=1;
  negotiation=response; crt_on=0; overrides_on=1; options_changed=0; mouse_delta=0;
  invalid_callback=false;
  REQUIRE(anygm_synthetic_cold_room_content_create(&fixture,60),"create declared-mode source");
  retro_set_environment(environment); retro_set_video_refresh(video);
  retro_set_audio_sample_batch(audio); retro_init();
  retro_set_input_state(input_state);

  /* The same anchor before it gained the declarations writes the state loaded at the end. */
  char anchor[256];
  snprintf(anchor,sizeof anchor,"%s/mode.anygm",fixture.directory);
  file=fopen(anchor,"wb");
  REQUIRE(file && fprintf(file,"[anygm]\npayload=data.win\n[overrides]\n%s",earlier)>=0,
          "write the earlier anchor");
  fclose(file); file=NULL;
  struct retro_game_info content={anchor,NULL,0,NULL};
  REQUIRE(retro_load_game(&content),"load undeclared source");
  for(int i=0;i<4;i++) tick();
  size_t plain_size=retro_serialize_size();
  plain=malloc(plain_size?plain_size:1);
  REQUIRE(plain && retro_serialize(plain,plain_size),"save without declarations");
  retro_unload_game();

  file=fopen(anchor,"wb");
  REQUIRE(file && fprintf(file,"[anygm]\npayload=data.win\n[overrides]\n%s"
                               "?crt crtmode|320|240|60\n"
                               "?crt roomspeed|1|48\n",earlier)>=0,"write declarations");
  fclose(file); file=NULL;
  crt_on=1; av_calls=0; geometry_calls=0;
  REQUIRE(retro_load_game(&content),"load declared mode");
  struct retro_system_av_info av; retro_get_system_av_info(&av);
  REQUIRE(av.timing.fps==60 && av.geometry.base_width==320 && av.geometry.base_height==240,
          "the startup answer already carries the mode");
  size_t capacity=retro_serialize_size();
  ring=malloc(capacity?capacity:1); in_mode=malloc(capacity?capacity:1);
  REQUIRE(ring && in_mode && snapshot(ring,capacity),"reserve the ring before the first frame");
  tick();
  REQUIRE(!av_calls && video_width==320 && video_height==240,"the first frame keeps that mode");
  AnygmEngine *engine=g_libretro.engine;
  REQUIRE(engine->output_width==64 && engine->output_height==48,"content keeps its own raster");
  REQUIRE(engine->host_canvas_x==128 && engine->host_canvas_y==96 &&
          engine->host_canvas_width==64 && engine->host_canvas_height==48,
          "padding grows both axes around an unscaled image");
  for(unsigned y=0;y<48;y++) for(unsigned x=0;x<64;x++)
    engine->screen[y*64+x]=0xFF000000u|((x*73u+y*251u+1u)&0xFFFFFFu);
  const uint32_t *pixels=NULL; unsigned width=0,height=0;
  REQUIRE(resolve_host_frame(engine,&pixels,&width,&height) && width==320 && height==240,
          "resolve the padded frame");
  for(unsigned y=0;y<height;y++) for(unsigned x=0;x<width;x++){
    uint32_t wanted=x>=128 && x<192 && y>=96 && y<144?
      engine->screen[(y-96)*64+x-128]&0xFFFFFFu:0u;
    REQUIRE((pixels[y*width+x]&0xFFFFFFu)==wanted,"native pixels and black margins");
  }

  speed(48);
  for(int i=0;i<4;i++) tick();
  long frame=frame_now(); samples=0; silent_ticks=0;
  for(int i=0;i<60;i++) tick();
  REQUIRE(frame_now()==frame+48 && !av_calls,"forty-eight steps on sixty presentations");
  REQUIRE(!silent_ticks && samples+919>=44100 && samples<=44100+919,"one second of sound");
  /* Centre the pointer's travel away from the raster edge. */
  mouse_delta=1; tick(); mouse_delta=0;
  for(int i=0;i<4;i++) tick();
  double pointer=engine->mouse_pixel_x;
  mouse_delta=1;
  for(int i=0;i<8;i++) tick();
  mouse_delta=0;
  for(int i=0;i<4;i++) tick();
  REQUIRE(engine->mouse_pixel_x==pointer+8,"relative motion survives a held tick");

  speed(120);
  for(int i=0;i<4;i++) tick();
  frame=frame_now();
  for(int i=0;i<30;i++) tick();
  REQUIRE(frame_now()==frame+60 && !av_calls,"a faster game runs two steps on each refresh");
  speed(36);
  REQUIRE(enter_room(1),"enter the declared room");
  for(int i=0;i<4;i++) tick();
  frame=frame_now();
  for(int i=0;i<60;i++) tick();
  REQUIRE(frame_now()==frame+48,"the declared room replaces the content's cadence");
  REQUIRE(gml_room_speed(&engine->vm)==48 && gml_room_speed_authored(&engine->vm)==36,
          "content queries agree while its own value stays untouched");
  REQUIRE(!av_calls && !geometry_calls,"a room change never rebuilds the mode");
  REQUIRE(snapshot(ring,capacity) && retro_serialize(in_mode,capacity),"save inside the mode");
  long saved=frame_now();
  for(int i=0;i<7;i++) tick();
  long replay=frame_now();
  REQUIRE(retro_unserialize(in_mode,capacity) && frame_now()==saved,"rewind inside the mode");
  for(int i=0;i<8;i++) tick();   /* the first tick only presents the restored picture */
  REQUIRE(frame_now()==replay && !av_calls,"rewind repeats the same cadence");

  options(0,1);
  for(int i=0;i<4 && !av_calls;i++) tick();
  REQUIRE(av_calls==1 && announced_fps==36 && announced_width==64,
          "withdrawing the mode announces the content's own cadence and raster once");
  REQUIRE(gml_room_speed(&engine->vm)==36,"withdrawal leaves no forced value behind");
  REQUIRE(snapshot(ring,capacity),"ring after withdrawal");
  REQUIRE(retro_unserialize(in_mode,capacity),"a state made in the mode loads outside it");
  for(int i=0;i<4;i++) tick();
  REQUIRE(gml_room_speed(&engine->vm)==36 && video_width==64,"such a state imports no mode");

  options(1,1);
  for(int i=0;i<4 && av_calls<2;i++) tick();
  REQUIRE(av_calls==2 && announced_fps==60 && announced_width==320,"selecting it announces once");
  options(1,0);
  for(int i=0;i<4 && av_calls<3;i++) tick();
  REQUIRE(av_calls==3 && announced_fps==36 && gml_room_speed(&engine->vm)==36,
          "Content overrides Off withdraws both declarations");
  options(1,1);
  for(int i=0;i<4 && av_calls<4;i++) tick();
  REQUIRE(av_calls==4 && announced_fps==60,"Content overrides On restores the mode");
  REQUIRE(snapshot(ring,capacity),"ring after option changes");

  REQUIRE(enter_room(0),"leave the declared room");
  tick();
  REQUIRE(gml_room_speed(&engine->vm)==36 && av_calls==4,"other rooms keep their own cadence");
  running=true; retro_reset(); running=false;
  REQUIRE(snapshot(ring,capacity),"ring accepts the Reset snapshot");
  for(int i=0;i<4;i++) tick();
  REQUIRE(av_calls==4 && video_width==320 && video_height==240,"Reset keeps the mode");
  REQUIRE(retro_unserialize(plain,plain_size),"declarations are not state identity");
  for(int i=0;i<4;i++) tick();
  REQUIRE(g_libretro.engine==engine && engine->crt_mode_hz==60 &&
          strstr(engine->content_overrides_text,"crtmode|") && video_width==320 && av_calls==4,
          "an earlier state resumes under the live mode");

  REQUIRE(anygm_set_runtime_override(engine,0,1,"?crt crtmode|512|240|60")==ANYGM_OK,
          "wide declaration");
  for(int i=0;i<4;i++) tick();
  REQUIRE(video_width==512 && video_height==240 && av_calls==4,
          "a declared extent is exempt from the automatic limits");
  REQUIRE(anygm_set_runtime_override(engine,0,1,"?crt crtmode|32|32|60")==ANYGM_OK,
          "small declaration");
  for(int i=0;i<4;i++) tick();
  REQUIRE(video_width==64 && video_height==48,"a picture that cannot fit keeps automatic policy");

  REQUIRE(anygm_set_runtime_override(engine,0,1,"?crt crtmode|window_w|window_h|60")==ANYGM_OK,
          "follow both raster dimensions");
  for(int i=0;i<4;i++) tick();
  REQUIRE(video_width==64 && video_height==48 && av_calls==4,"raster starts at the game's size");
  unsigned prior_geometry=geometry_calls;
  GmlVal window_size[]={vreal(96),vreal(48)};
  gml_builtin_call(&engine->vm,"window_set_size",window_size,2);
  gml_builtin_call(&engine->vm,"display_set_gui_size",window_size,2);
  for(int i=0;i<4;i++) tick();
  retro_get_system_av_info(&av);
  REQUIRE(video_width==96 && video_height==48 && av.geometry.base_width==96 &&
          av.geometry.base_height==48 && geometry_calls>prior_geometry &&
          av.timing.fps==60 && av_calls==4,"the live raster changes while the refresh stays held");
  REQUIRE(snapshot(ring,capacity),"ring accepts the new live raster");
  REQUIRE(retro_unserialize(in_mode,capacity),"rewind to the smaller live raster");
  tick();
  REQUIRE(video_width==64 && video_height==48 && av_calls==4,
          "the restored frame uses its own dimensions at the same refresh");

  REQUIRE(anygm_set_runtime_override(engine,0,1,"?crt crtmode|320|240|window_fps")==ANYGM_OK,
          "fixed raster with game cadence");
  for(int i=0;i<4;i++) tick();
  REQUIRE(video_width==320 && video_height==240,"the dynamic cadence keeps fixed geometry");
  unsigned prior_av=av_calls;
  REQUIRE(enter_room(0),"return to the room without a cadence override");
  for(int i=0;i<4;i++) tick();
  REQUIRE(av_calls==prior_av+1 && announced_fps==36 && announced_width==320,
          "the fixed CRT canvas follows the game-set speed outside overridden rooms");
  REQUIRE(enter_room(1),"enter the overridden room with a dynamic CRT cadence");
  for(int i=0;i<4;i++) tick();
  REQUIRE(av_calls==prior_av+2 && announced_fps==48 && announced_width==320,
          "the fixed CRT canvas follows the effective room speed");
  REQUIRE(!invalid_callback,"every AV callback occurs inside retro_run");
  printf("declared CRT mode: negotiation=%d capacity=%zu\n",response,capacity);
done:
  if(file) fclose(file);
  free(ring); free(plain); free(in_mode);
  retro_unload_game(); retro_deinit(); anygm_synthetic_content_destroy(&fixture);
  return ok;
}

int main(void){
  if(!held_cadence_case()) return EXIT_FAILURE;
  if(!automatic_crt_case(25,50) || !automatic_crt_case(30,60) ||
     !automatic_crt_case(48,48) || !automatic_crt_case(60,60)) return EXIT_FAILURE;
  for(int response=-1;response<=1;response++)
    if(!declared_mode_case(response,"") || !declared_mode_case(response,"$fixture_marker=7\n"))
      return EXIT_FAILURE;
  return EXIT_SUCCESS;
}
