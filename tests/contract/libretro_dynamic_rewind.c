/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Frontend-sized rewind of moving authored pixels and a playing authored sound. A state load
 * presents its completed picture once before simulation resumes, so replay and continuation are
 * checked separately. The graphics driver is synthetic; real-device pixels have another gate. */
#include "libretro_internal.h"
#include "engine_internal.h"
#include "gml_render_resources.h"
#include "graphics_driver_fixture.h"
#include "synthetic_content.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
static int monitor_output,options_changed;
static unsigned video_calls,video_hardware;
static unsigned delivered_width,delivered_height;
static size_t captured_audio_frames;
static int16_t captured_audio[AUDIO_CAPACITY*2];
static struct retro_hw_render_callback hardware;

typedef struct GrowthSnapshot {
  uint8_t *state;
  size_t capacity;
  uint32_t *pixels;
  unsigned width,height;
  unsigned host_width,host_height;
  int16_t audio[AUDIO_CAPACITY*2];
  size_t audio_frames;
  long frame;
} GrowthSnapshot;

typedef struct RewindCost {
  double size_total,size_max,save_total,save_max,copy_total,copy_max;
  double load_total,load_max;
  size_t copy_bytes,peak_slot;
  unsigned pushes,loads;
} RewindCost;

static RewindCost growth_cost;

static double elapsed_ms(void){
  struct timespec now;
  if(clock_gettime(CLOCK_MONOTONIC,&now)!=0) return 0.0;
  return (double)now.tv_sec*1000.0+(double)now.tv_nsec/1000000.0;
}

static void cost_add(double duration,double *total,double *maximum){
  *total+=duration;
  if(duration>*maximum) *maximum=duration;
}

/* The frontend can query size repeatedly before every push. Measure the first CPU observation
 * separately from repeated queries/saves of one unchanged GPU-produced frame: only the first may
 * materialise it, and no query may borrow a GL context that the frontend has not lent. */
static int observation_cost(AnygmEngine *engine,const char *phase,size_t ring_capacity){
  GmlGpuCounters before_gpu={0},after_gpu={0};
  GmlRenderJournalMetrics journal={0};
  uint8_t *first=NULL,*again=NULL;
  size_t bytes=0,written=0;
  unsigned before_materializations,after_first;
  int before_forbidden,ok=1;
  double first_query=0.0,repeat_queries=0.0,repeat_max=0.0;
  double saves=0.0,save_max=0.0;
  if(!engine) return 0;
  before_materializations=engine->frame_materializations;
  before_forbidden=anygm_test_graphics_calls_after_forget();
  if(engine->gpu) gml_gpu_counters(engine->gpu,&before_gpu);
  anygm_test_graphics_forbid_calls();
  double start=elapsed_ms();
  bytes=anygm_state_size(engine);
  first_query=elapsed_ms()-start;
  after_first=engine->frame_materializations;
  ok=bytes>0 && after_first>=before_materializations &&
     after_first-before_materializations<=1u;
  for(int repeat=0;repeat<3 && ok;repeat++){
    start=elapsed_ms();
    size_t measured=anygm_state_size(engine);
    cost_add(elapsed_ms()-start,&repeat_queries,&repeat_max);
    ok=measured==bytes && engine->frame_materializations==after_first;
  }
  if(ok){
    first=malloc(bytes);
    again=malloc(bytes);
    ok=first && again;
  }
  for(int repeat=0;repeat<4 && ok;repeat++){
    uint8_t *destination=repeat?again:first;
    start=elapsed_ms();
    ok=anygm_state_save(engine,destination,bytes,&written)==ANYGM_OK && written==bytes;
    cost_add(elapsed_ms()-start,&saves,&save_max);
    ok=ok && engine->frame_materializations==after_first &&
       (!repeat || !memcmp(first,again,bytes));
  }
  ok=ok && anygm_test_graphics_calls_after_forget()==before_forbidden;
  anygm_test_graphics_allow_calls();
  if(engine->gpu) gml_gpu_counters(engine->gpu,&after_gpu);
  ok=ok && after_gpu.scene_readbacks==before_gpu.scene_readbacks &&
     after_gpu.scene_readback_bytes==before_gpu.scene_readback_bytes;
  ok=ok && (!engine->gpu || engine->render.app_surface_resource!=NULL);
  if(engine->render.app_surface_resource)
    gml_render_resource_journal_metrics(engine->render.app_surface_resource,&journal);
  ok=ok && journal.journal_bytes<=8u*1024u*1024u &&
     journal.retained_mutable_bytes<=64u*1024u*1024u;
  printf("state observation cost: mode=%s phase=%s frame=%ld state=%zu screen=%zu "
         "ring_initial=%zu materializations=%u first_query=%.3f repeated_query_avg_max=%.3f/%.3f "
         "save_avg_max=%.3f/%.3f ms journal=%zu/%u retained=%zu/%u "
         "replay=%llu checkpoints=%llu readback_delta=%llu/%llu\n",
         gpu_requested?"GPU":"CPU",phase,engine->vm.frame,bytes,
         (size_t)engine->output_width*engine->output_height*4u,ring_capacity,
         after_first-before_materializations,first_query,repeat_queries/3.0,repeat_max,
         saves/4.0,save_max,journal.journal_bytes,journal.journal_operations,
         journal.retained_mutable_bytes,journal.retained_versions,
         (unsigned long long)journal.cpu_replays,
         (unsigned long long)journal.device_checkpoints,
         (unsigned long long)(after_gpu.scene_readbacks-before_gpu.scene_readbacks),
         (unsigned long long)(after_gpu.scene_readback_bytes-before_gpu.scene_readback_bytes));
  if(!ok) fprintf(stderr,"state observation failed: mode=%s phase=%s bytes=%zu\n",
                  gpu_requested?"GPU":"CPU",phase,bytes);
  free(again);
  free(first);
  return ok;
}

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
    if(!strcmp(variable->key,"anygm_render_game_resolution"))
      variable->value=monitor_output?"Off":"On";
    if(!strcmp(variable->key,"anygm_width_resolution")) variable->value="1280";
    if(!strcmp(variable->key,"anygm_height_resolution")) variable->value="960";
    if(!strcmp(variable->key,"anygm_adjust_crt_tv")) variable->value="Off";
    return variable->value!=NULL;
  }
  if(command==RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE){
    *(bool*)data=options_changed!=0;
    options_changed=0;
    return true;
  }
  return command==RETRO_ENVIRONMENT_SET_PIXEL_FORMAT ||
         command==RETRO_ENVIRONMENT_SET_GEOMETRY ||
         command==RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO;
}

static void video(const void *pixels,unsigned width,unsigned height,size_t pitch){
  (void)pitch;
  delivered_width=width;
  delivered_height=height;
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

/* A frontend tests its advertised capacity before it lends a rewind slot. Keep the picture
 * separately so fixed compact and acknowledged-variable policies can be compared fairly. */
static int growth_snapshot(GrowthSnapshot *snapshot,size_t cold_capacity){
  AnygmEngine *engine=g_libretro.engine;
  size_t answer,count;
  double start=elapsed_ms();
  anygm_test_graphics_forbid_calls();
  answer=retro_serialize_size();
  anygm_test_graphics_allow_calls();
  double size_ms=elapsed_ms()-start;
  if(!answer || (variable_response<=0 && answer!=cold_capacity) || !engine ||
     !engine->screen || !engine->output_width || !engine->output_height) return 0;
  count=(size_t)engine->output_width*engine->output_height;
  snapshot->state=malloc(answer);
  snapshot->pixels=malloc(count*sizeof(uint32_t));
  if(!snapshot->state || !snapshot->pixels) return 0;
  snapshot->capacity=answer;
  snapshot->width=engine->output_width;
  snapshot->height=engine->output_height;
  snapshot->host_width=engine->host_output_width;
  snapshot->host_height=engine->host_output_height;
  start=elapsed_ms();
  anygm_test_graphics_forbid_calls();
  int saved=retro_serialize(snapshot->state,answer);
  anygm_test_graphics_allow_calls();
  double save_ms=elapsed_ms()-start;
  if(!saved || anygm_test_graphics_calls_after_forget()) return 0;
  uint8_t *ring_copy=malloc(answer);
  if(!ring_copy) return 0;
  start=elapsed_ms();
  memcpy(ring_copy,snapshot->state,answer);
  volatile uint8_t copied_tail=ring_copy[answer-1u];
  (void)copied_tail;
  double copy_ms=elapsed_ms()-start;
  free(ring_copy);
  memcpy(snapshot->pixels,engine->screen,count*sizeof(uint32_t));
  snapshot->audio_frames=captured_audio_frames;
  memcpy(snapshot->audio,captured_audio,captured_audio_frames*2u*sizeof(int16_t));
  snapshot->frame=engine->vm.frame;
  cost_add(size_ms,&growth_cost.size_total,&growth_cost.size_max);
  cost_add(save_ms,&growth_cost.save_total,&growth_cost.save_max);
  cost_add(copy_ms,&growth_cost.copy_total,&growth_cost.copy_max);
  growth_cost.copy_bytes+=answer;
  if(answer>growth_cost.peak_slot) growth_cost.peak_slot=answer;
  growth_cost.pushes++;
  return 1;
}

static int same_growth(const GrowthSnapshot *left,const GrowthSnapshot *right,int with_audio){
  size_t count=(size_t)left->width*left->height;
  return left->capacity==right->capacity && left->width==right->width &&
         left->height==right->height && left->host_width==right->host_width &&
         left->host_height==right->host_height && left->frame==right->frame &&
         !memcmp(left->state,right->state,left->capacity) &&
         !memcmp(left->pixels,right->pixels,count*sizeof(uint32_t)) &&
         (!with_audio || (left->audio_frames==right->audio_frames &&
          !memcmp(left->audio,right->audio,left->audio_frames*2u*sizeof(int16_t))));
}

static void growth_dispose(GrowthSnapshot *snapshot){
  free(snapshot->state);
  free(snapshot->pixels);
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
  monitor_output=options_changed=0;
  loaded=initialized=0;
  video_calls=video_hardware=0;
  delivered_width=delivered_height=0;
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

/* Keep a real GPU scene prefix while a live virtual-monitor change makes the completed image
 * much larger than the pre-frame ring. A high-entropy completed image separates compact fixed
 * slots from full variable slots without changing authored sprite/audio simulation. */
static int monitor_growth_case(int mode,int response){
  AnygmSyntheticContent fixture={0};
  GrowthSnapshot cold={0},native={0},large={0},future={0},later={0};
  GrowthSnapshot replay={0},continued={0};
  GrowthSnapshot reset_cold={0},reset_frame={0};
  GmlGpuCounters counters={0};
  size_t initial=0,count=0,complete_bytes=0,resume_bytes=0;
  int ok=1;
  gpu_requested=mode;
  variable_response=response;
  monitor_output=options_changed=0;
  memset(&growth_cost,0,sizeof growth_cost);
  loaded=initialized=0;
  video_calls=video_hardware=0;
  delivered_width=delivered_height=0;
  memset(&hardware,0,sizeof hardware);
  anygm_test_graphics_reset();
  REQUIRE(anygm_synthetic_large_moving_sound_content_create(&fixture),
          "create large monitor growth fixture");
  retro_set_environment(environment);
  retro_set_video_refresh(video);
  retro_set_audio_sample_batch(audio_batch);
  retro_init(); initialized=1;
  struct retro_game_info info={fixture.path,NULL,0,NULL};
  REQUIRE(retro_load_game(&info),"load monitor growth fixture");
  loaded=1;
  if(mode){
    REQUIRE(hardware.context_reset!=NULL,"monitor frontend accepted GLES context");
    hardware.context_reset();
    REQUIRE(g_libretro.engine->gpu!=NULL,"monitor scene context adopted");
  }
  anygm_test_graphics_forbid_calls();
  initial=retro_serialize_size();
  anygm_test_graphics_allow_calls();
  REQUIRE(initial>0 && initial<=32u*1024u*1024u,"monitor ring allocated before first frame");
  REQUIRE(growth_snapshot(&cold,initial),"monitor cold ring push");
  captured_audio_frames=0;
  retro_run();
  if(response>0)
    REQUIRE(observation_cost(g_libretro.engine,"moving",initial),
            "repeated native-frame state observations are bounded and GL-free");
  REQUIRE(growth_snapshot(&native,initial),"native frame ring push");
  REQUIRE(native.width==1920u && native.height==1080u,
          "native picture precedes monitor change");
  monitor_output=options_changed=1;
  captured_audio_frames=0;
  retro_run();
  REQUIRE(g_libretro.engine->host_output_width==1280u &&
          g_libretro.engine->host_output_height==960u &&
          delivered_width==1280u && delivered_height==960u,
          "live monitor changes frontend presentation geometry");
  REQUIRE(engine_materialize_completed_frame(g_libretro.engine),
          "complete GPU scene before stressing frame capacity");
  count=(size_t)g_libretro.engine->output_width*g_libretro.engine->output_height;
  for(size_t index=0;index<count;index++)
    g_libretro.engine->screen[index]=UINT32_C(0xFF000000)|
      (uint32_t)((index*UINT32_C(2654435761))&UINT32_C(0x00FFFFFF));
  if(response>0)
    REQUIRE(observation_cost(g_libretro.engine,"detailed",initial),
            "repeated detailed-frame state observations are bounded and GL-free");
  complete_bytes=anygm_state_size(g_libretro.engine);
  resume_bytes=anygm_state_resume_size(g_libretro.engine);
  REQUIRE(complete_bytes>initial && resume_bytes<initial,
          "detailed picture exceeds the fixed slot but required resume bytes fit");
  REQUIRE(growth_snapshot(&large,initial),"grown monitor ring push");
  REQUIRE(large.width==1920u && large.height==1080u,
          "authored state raster stays distinct from virtual monitor");
  REQUIRE(response>0?large.capacity>initial:large.capacity==initial,
          "only acknowledged variable ring grows for a detailed frame");
  captured_audio_frames=0;
  retro_run();
  REQUIRE(growth_snapshot(&future,initial),"post-growth moving frame ring push");
  REQUIRE(future.frame>large.frame && future.audio_frames>0,
          "monitor route continues authored image and audio");
  captured_audio_frames=0;
  retro_run();
  REQUIRE(growth_snapshot(&later,initial),"second post-growth moving frame ring push");
  REQUIRE(later.frame>future.frame && later.audio_frames>0,
          "second monitor frame advances authored image and audio");
  if(mode){
    gml_gpu_counters(g_libretro.engine->gpu,&counters);
    REQUIRE(counters.scene_families[GML_RENDER_SCENE_SPRITE]>=4u,
            "GPU scene sprites execute across monitor growth");
  }
  double load_start=elapsed_ms();
  anygm_test_graphics_forbid_calls();
  int loaded_state=retro_unserialize(large.state,large.capacity);
  anygm_test_graphics_allow_calls();
  cost_add(elapsed_ms()-load_start,&growth_cost.load_total,&growth_cost.load_max);
  growth_cost.loads++;
  REQUIRE(loaded_state && !anygm_test_graphics_calls_after_forget(),
          "grown ring restore uses no current graphics context");
  captured_audio_frames=0;
  retro_run();
  REQUIRE(growth_snapshot(&replay,initial),"first restored monitor picture push");
  /* A full variable slot replays its picture without simulation or sound. A fixed slot cannot
   * contain the high-entropy picture; its frame-free resume reconstructs the next authored
   * picture/audio exactly, which is the established compact-ring policy. */
  REQUIRE(response>0?(same_growth(&large,&replay,0) && captured_audio_frames==0):
                     same_growth(&future,&replay,1),
          "restored slot follows its complete or compact presentation policy");
  captured_audio_frames=0;
  retro_run();
  REQUIRE(growth_snapshot(&continued,initial) &&
          (response>0?same_growth(&future,&continued,1):same_growth(&later,&continued,1)),
          "monitor rewind continues exact image audio and state");
  monitor_output=0; options_changed=1;
  captured_audio_frames=0;
  retro_run();
  REQUIRE(g_libretro.engine->host_output_width==1920u &&
          g_libretro.engine->host_output_height==1080u &&
          delivered_width==1920u && delivered_height==1080u &&
          growth_snapshot(&reset_cold,initial),"ring survives return to native monitor");
  retro_reset();
  REQUIRE(growth_snapshot(&reset_frame,initial),"old frontend ring accepts Reset cold state");
  printf("monitor rewind: mode=%s negotiation=%d cold=%zu grown=%zu complete=%zu "
         "resume=%zu native=%ux%u monitor=%ux%u\n",
         mode?"GPU":"CPU",response,initial,large.capacity,complete_bytes,resume_bytes,
         native.width,native.height,large.host_width,large.host_height);
  printf("monitor rewind cost: mode=%s negotiation=%d pushes=%u loads=%u slot_peak=%zu "
         "ring_copy_bytes=%zu size_avg_max=%.3f/%.3f save_avg_max=%.3f/%.3f "
         "copy_avg_max=%.3f/%.3f restore_avg_max=%.3f/%.3f ms\n",
         mode?"GPU":"CPU",response,growth_cost.pushes,growth_cost.loads,
         growth_cost.peak_slot,growth_cost.copy_bytes,
         growth_cost.size_total/growth_cost.pushes,growth_cost.size_max,
         growth_cost.save_total/growth_cost.pushes,growth_cost.save_max,
         growth_cost.copy_total/growth_cost.pushes,growth_cost.copy_max,
         growth_cost.load_total/growth_cost.loads,growth_cost.load_max);
done:
  growth_dispose(&cold); growth_dispose(&native); growth_dispose(&large);
  growth_dispose(&future); growth_dispose(&later);
  growth_dispose(&replay); growth_dispose(&continued);
  growth_dispose(&reset_cold); growth_dispose(&reset_frame);
  if(loaded) retro_unload_game();
  if(initialized) retro_deinit();
  anygm_synthetic_content_destroy(&fixture);
  return ok;
}

int main(void){
  for(int mode=0;mode<=1;mode++)
    for(int response=-1;response<=1;response++)
      if(!run_case(mode,response) || !monitor_growth_case(mode,response)) return EXIT_FAILURE;
  return EXIT_SUCCESS;
}
