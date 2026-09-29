/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Frontend-sized rewind of moving authored pixels and a playing authored sound. A state load
 * presents its completed picture once before simulation resumes, so replay and continuation are
 * checked separately. The graphics driver is synthetic; real-device pixels have another gate. */
#include "libretro_internal.h"
#include "engine_internal.h"
#include "graphics_driver_fixture.h"
#include "synthetic_content.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { FRAME_WIDTH=64, FRAME_HEIGHT=48, AUDIO_CAPACITY=4096 };

typedef struct Snapshot {
  uint8_t *state;
  size_t capacity;
  uint32_t pixels[FRAME_WIDTH*FRAME_HEIGHT];
  int16_t audio[AUDIO_CAPACITY*2];
  size_t audio_frames;
  long frame;
} Snapshot;

static int variable_response,gpu_requested,loaded,initialized;
static unsigned video_calls,video_hardware;
static size_t captured_audio_frames;
static int16_t captured_audio[AUDIO_CAPACITY*2];
static struct retro_hw_render_callback hardware;

static retro_proc_address_t graphics_proc(const char *name){
  return (retro_proc_address_t)anygm_test_graphics_proc(NULL,name);
}

static uintptr_t graphics_framebuffer(void){
  return anygm_test_graphics_framebuffer(NULL);
}

static bool environment(unsigned command,void *data){
  if(command==RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS){
    if(variable_response>0) *(uint64_t*)data|=RETRO_SERIALIZATION_QUIRK_FRONT_VARIABLE_SIZE;
    return variable_response>=0;
  }
  if(command==RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER){
    if(!gpu_requested) return false;
    *(unsigned*)data=RETRO_HW_CONTEXT_OPENGLES3;
    return true;
  }
  if(command==RETRO_ENVIRONMENT_SET_HW_RENDER){
    struct retro_hw_render_callback *request=data;
    if(!gpu_requested || !request || request->context_type!=RETRO_HW_CONTEXT_OPENGLES3)
      return false;
    request->get_proc_address=graphics_proc;
    request->get_current_framebuffer=graphics_framebuffer;
    hardware=*request;
    return true;
  }
  if(command==RETRO_ENVIRONMENT_GET_VARIABLE){
    struct retro_variable *variable=data;
    variable->value=NULL;
    if(!strcmp(variable->key,"anygm_scene_renderer"))
      variable->value=gpu_requested?"OpenGL":"CPU";
    return variable->value!=NULL;
  }
  if(command==RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE){
    *(bool*)data=false;
    return true;
  }
  return command==RETRO_ENVIRONMENT_SET_PIXEL_FORMAT ||
         command==RETRO_ENVIRONMENT_SET_GEOMETRY ||
         command==RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO;
}

static void video(const void *pixels,unsigned width,unsigned height,size_t pitch){
  (void)pitch;
  if(width==FRAME_WIDTH && height==FRAME_HEIGHT) video_calls++;
  if(pixels==RETRO_HW_FRAME_BUFFER_VALID) video_hardware++;
}

static size_t audio_batch(const int16_t *samples,size_t frames){
  if(frames>AUDIO_CAPACITY-captured_audio_frames) return 0;
  memcpy(captured_audio+captured_audio_frames*2u,samples,frames*2u*sizeof(int16_t));
  captured_audio_frames+=frames;
  return frames;
}

static int state_save(Snapshot *snapshot,size_t initial){
  size_t answer;
  anygm_test_graphics_forbid_calls();
  answer=retro_serialize_size();
  anygm_test_graphics_allow_calls();
  if(!answer || (variable_response<=0 && answer!=initial)) return 0;
  snapshot->state=malloc(answer);
  if(!snapshot->state) return 0;
  snapshot->capacity=answer;
  anygm_test_graphics_forbid_calls();
  int saved=retro_serialize(snapshot->state,answer);
  anygm_test_graphics_allow_calls();
  if(!saved || anygm_test_graphics_calls_after_forget()) return 0;
  AnygmEngine *engine=g_libretro.engine;
  if(!engine || engine->output_width!=FRAME_WIDTH ||
     engine->output_height!=FRAME_HEIGHT || !engine->screen) return 0;
  memcpy(snapshot->pixels,engine->screen,sizeof snapshot->pixels);
  snapshot->audio_frames=captured_audio_frames;
  memcpy(snapshot->audio,captured_audio,captured_audio_frames*2u*sizeof(int16_t));
  snapshot->frame=engine->vm.frame;
  return 1;
}

static int state_load(const Snapshot *snapshot){
  anygm_test_graphics_forbid_calls();
  int loaded_state=retro_unserialize(snapshot->state,snapshot->capacity);
  anygm_test_graphics_allow_calls();
  return loaded_state && anygm_test_graphics_calls_after_forget()==0;
}

static int same_snapshot(const Snapshot *left,const Snapshot *right,int with_audio){
  return left->capacity==right->capacity &&
         !memcmp(left->state,right->state,left->capacity) &&
         left->frame==right->frame &&
         !memcmp(left->pixels,right->pixels,sizeof left->pixels) &&
         (!with_audio || (left->audio_frames==right->audio_frames &&
          !memcmp(left->audio,right->audio,left->audio_frames*2u*sizeof(int16_t))));
}

#define REQUIRE(condition,label) do{ if(!(condition)){ \
  fprintf(stderr,"dynamic rewind (%s, negotiation %d): %s\n", \
          gpu_requested?"GPU":"CPU",variable_response,label); \
  ok=0; goto done; \
} }while(0)

static int run_case(int mode,int response){
  AnygmSyntheticContent fixture={0};
  Snapshot snapshots[6]={0},replay={0},continued={0},repeated={0},after_reset={0};
  GmlGpuCounters before={0},after={0};
  size_t initial=0;
  int deletes_before_load=0;
  int ok=1,changed_images=0,nonzero_audio=0;
  gpu_requested=mode;
  variable_response=response;
  loaded=initialized=0;
  video_calls=video_hardware=0;
  memset(&hardware,0,sizeof hardware);
  anygm_test_graphics_reset();
  REQUIRE(anygm_synthetic_moving_sound_content_create(&fixture),"create moving audio fixture");
  retro_set_environment(environment);
  retro_set_video_refresh(video);
  retro_set_audio_sample_batch(audio_batch);
  retro_init(); initialized=1;
  struct retro_game_info info={fixture.path,NULL,0,NULL};
  REQUIRE(retro_load_game(&info),"load moving audio fixture");
  loaded=1;
  REQUIRE(g_libretro.variable_state_supported==(response>0),"explicit variable-size negotiation");
  if(mode){
    REQUIRE(hardware.context_reset!=NULL,"frontend accepted GLES context request");
    hardware.context_reset();
    REQUIRE(g_libretro.engine->gpu!=NULL,"scene GPU context adopted");
  }
  anygm_test_graphics_forbid_calls();
  initial=retro_serialize_size();
  anygm_test_graphics_allow_calls();
  REQUIRE(initial>0 && initial<=32u*1024u*1024u,"pre-frame ring capacity is bounded");
  REQUIRE(state_save(&snapshots[0],initial),"pre-frame ring push");
  for(int frame=0;frame<5;frame++){
    captured_audio_frames=0;
    retro_run();
    REQUIRE(state_save(&snapshots[frame+1],initial),"post-frame ring push");
    REQUIRE(snapshots[frame+1].audio_frames>0,"authored audio callback emitted samples");
    for(size_t sample=0;sample<snapshots[frame+1].audio_frames*2u;sample++)
      if(snapshots[frame+1].audio[sample]) nonzero_audio++;
    if(frame && memcmp(snapshots[frame].pixels,snapshots[frame+1].pixels,
                       sizeof snapshots[frame].pixels)) changed_images++;
  }
  REQUIRE(video_calls==5 && video_hardware==(mode?5u:0u),"frontend received five expected frames");
  REQUIRE(changed_images>=3,"authored sprite visibly moves across frames");
  REQUIRE(nonzero_audio>0,"authored sound actually plays");
  REQUIRE(memcmp(snapshots[3].audio,snapshots[4].audio,
                 snapshots[3].audio_frames*2u*sizeof(int16_t))!=0,
          "audio changes across adjacent frames");
  if(mode){
    gml_gpu_counters(g_libretro.engine->gpu,&before);
    REQUIRE(before.scene_families[GML_RENDER_SCENE_SPRITE]>=5u,
            "five authored sprites reached the GPU");
  }

  deletes_before_load=anygm_test_graphics_deletes();
  REQUIRE(state_load(&snapshots[3]),"rewind to an earlier frame through frontend");
  REQUIRE(anygm_test_graphics_deletes()==deletes_before_load,
          "rewind defers physical resource deletion without a current context");
  if(mode){
    for(int repeat=0;repeat<4;repeat++)
      REQUIRE(state_load(&snapshots[3]),"repeated loads before context-current reuse");
    REQUIRE(anygm_test_graphics_deletes()==deletes_before_load,
            "repeated loads keep retirement bounded and GL-free");
  }
  captured_audio_frames=0;
  retro_run();
  REQUIRE(captured_audio_frames==0,"first restored call presents without advancing audio");
  REQUIRE(state_save(&replay,initial) && same_snapshot(&replay,&snapshots[3],0),
          "restored completed image and state match saved frame");
  captured_audio_frames=0;
  retro_run();
  REQUIRE(state_save(&continued,initial) && same_snapshot(&continued,&snapshots[4],1),
          "advancing image, audio and state match original future");
  if(mode){
    gml_gpu_counters(g_libretro.engine->gpu,&after);
    REQUIRE(after.scene_families[GML_RENDER_SCENE_SPRITE]>
            before.scene_families[GML_RENDER_SCENE_SPRITE],
            "GPU scene work continues after rewind");
    REQUIRE(anygm_test_graphics_deletes()>deletes_before_load,
            "next current GPU scene reclaims retired resources");
  }

  /* Rewinding the same ring slot again must not accumulate dead device targets or revive the
   * previous timeline. The image and audio still come from the exact recorded continuation. */
  deletes_before_load=anygm_test_graphics_deletes();
  REQUIRE(state_load(&snapshots[3]),"second rewind to the same frontend ring slot");
  REQUIRE(anygm_test_graphics_deletes()==deletes_before_load,
          "second rewind also defers physical deletion");
  captured_audio_frames=0;
  retro_run();
  REQUIRE(captured_audio_frames==0,"second restored call does not advance audio");
  captured_audio_frames=0;
  retro_run();
  REQUIRE(state_save(&repeated,initial) && same_snapshot(&repeated,&snapshots[4],1),
          "second continuation preserves moving image, audio and state");
  if(mode) REQUIRE(anygm_test_graphics_deletes()>deletes_before_load,
                   "second GPU continuation also reclaims retired targets");

  retro_reset();
  captured_audio_frames=0;
  retro_run();
  REQUIRE(state_save(&after_reset,initial),"frontend ring still accepts first Reset frame");
  REQUIRE(after_reset.audio_frames>0 &&
          !memcmp(after_reset.pixels,snapshots[1].pixels,sizeof after_reset.pixels) &&
          after_reset.audio_frames==snapshots[1].audio_frames &&
          !memcmp(after_reset.audio,snapshots[1].audio,
                  after_reset.audio_frames*2u*sizeof(int16_t)),
          "Reset reproduces first moving image and authored audio");
  REQUIRE(anygm_test_graphics_calls_after_forget()==0,"state callbacks made no GL calls");
  printf("dynamic rewind: mode=%s negotiation=%d cold=%zu changed=%d audio_nonzero=%d\n",
         mode?"GPU":"CPU",response,initial,changed_images,nonzero_audio);
done:
  for(size_t i=0;i<6;i++) free(snapshots[i].state);
  free(replay.state);
  free(continued.state);
  free(repeated.state);
  free(after_reset.state);
  if(loaded) retro_unload_game();
  if(initialized) retro_deinit();
  anygm_synthetic_content_destroy(&fixture);
  return ok;
}

int main(void){
  for(int mode=0;mode<=1;mode++)
    for(int response=-1;response<=1;response++)
      if(!run_case(mode,response)) return EXIT_FAILURE;
  return EXIT_SUCCESS;
}
