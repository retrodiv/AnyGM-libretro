/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "libretro_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void retro_set_environment(retro_environment_t callback);
void retro_init(void);
void retro_deinit(void);
void retro_unload_game(void);
size_t retro_serialize_size(void);
bool retro_serialize(void *data,size_t size);
bool retro_unserialize(const void *data,size_t size);

static size_t stub_state_bytes;
static size_t stub_resume_state_bytes;
static size_t stub_hint_bytes;
static size_t stub_resume_hint_bytes;
static uint32_t stub_capacity_flags;
static size_t stub_capacity_reserve;
static unsigned snapshot_failure_messages;
static size_t last_load_bytes;
static unsigned complete_saves;
static unsigned complete_save_attempts;
static unsigned resume_saves;
static unsigned resume_size_queries;
static unsigned complete_hint_queries;
static unsigned resume_hint_queries;
static int variable_frontend;
static int serialization_query_seen;
static int development_setting_seen;
static AnygmResult stub_frame_result;
static int stub_text_consumed;
static unsigned text_acknowledgements;

static bool environment_callback(unsigned command,void *data){
  if(command==RETRO_ENVIRONMENT_SET_MESSAGE){
    const struct retro_message *message=(const struct retro_message *)data;
    if(message && message->msg && strstr(message->msg,"snapshot could not be recorded"))
      snapshot_failure_messages++;
    return true;
  }
  if(command==RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS){
    uint64_t *quirks=(uint64_t *)data;
    serialization_query_seen=quirks &&
      (*quirks&RETRO_SERIALIZATION_QUIRK_CORE_VARIABLE_SIZE)!=0;
    if(variable_frontend>0 && quirks)
      *quirks|=RETRO_SERIALIZATION_QUIRK_FRONT_VARIABLE_SIZE;
    return variable_frontend>=0;
  }
  if(command==RETRO_ENVIRONMENT_SET_PIXEL_FORMAT) return true;
  return false;
}

AnygmResult anygm_create(const AnygmHostServices *services,AnygmEngine **engine){
  if(!services || !engine) return ANYGM_ERROR_INVALID_ARGUMENT;
  const char *setting=services->development_setting?
    services->development_setting(services->userdata,"ANYGM_TEST_SETTING"):NULL;
  development_setting_seen=setting && !strcmp(setting,"visible");
  *engine=(AnygmEngine *)(uintptr_t)1u;
  return ANYGM_OK;
}

void anygm_destroy(AnygmEngine *engine){ (void)engine; }
AnygmResult anygm_load_prepare(AnygmEngine *engine,const AnygmContentSource *source,
                               const AnygmLoadConfig *config,AnygmContentInfo *info){
  (void)engine; (void)source; (void)config;
  if(info) info->flags=0;
  return ANYGM_OK;
}
AnygmResult anygm_load_start(AnygmEngine *engine){ (void)engine; return ANYGM_OK; }
AnygmResult anygm_load(AnygmEngine *engine,const AnygmContentSource *source,
                       const AnygmLoadConfig *config){
  return anygm_load_prepare(engine,source,config,NULL);
}
void anygm_unload_for_memory_pressure(AnygmEngine *engine){ (void)engine; }
void anygm_unload(AnygmEngine *engine){ (void)engine; }
AnygmResult anygm_reset(AnygmEngine *engine){ (void)engine; return ANYGM_OK; }
AnygmResult anygm_get_av_info(const AnygmEngine *engine,AnygmAvInfo *info){
  (void)engine; (void)info; return ANYGM_OK;
}
AnygmResult anygm_run_frame(AnygmEngine *engine,const AnygmInputFrame *input,
                            AnygmFrameOutput *output){
  (void)engine; (void)input;
  output->flags=stub_text_consumed?ANYGM_FRAME_TEXT_INPUT_CONSUMED:0;
  return stub_frame_result;
}
AnygmResult anygm_schedule_frame(AnygmEngine *engine,double hz,uint32_t *steps){
  (void)engine; (void)hz; *steps=1; return ANYGM_OK;
}
AnygmResult anygm_repeat_frame(AnygmEngine *engine,AnygmFrameOutput *output){
  return anygm_run_frame(engine,NULL,output);
}
AnygmResult anygm_set_config(AnygmEngine *engine,const AnygmConfigDelta *delta){
  (void)engine; (void)delta; return ANYGM_OK;
}
AnygmResult anygm_set_runtime_override(AnygmEngine *engine,uint32_t slot,uint32_t enabled,
                                       const char *expression){
  (void)engine; (void)slot; (void)enabled; (void)expression; return ANYGM_OK;
}
size_t anygm_state_size(AnygmEngine *engine){ (void)engine; return stub_state_bytes; }
size_t anygm_state_resume_size(AnygmEngine *engine){
  (void)engine;
  resume_size_queries++;
  return stub_resume_state_bytes?stub_resume_state_bytes:stub_state_bytes;
}
size_t anygm_state_capacity_hint(const AnygmEngine *engine){
  (void)engine;
  complete_hint_queries++;
  return stub_hint_bytes;
}
size_t anygm_state_resume_capacity_hint(const AnygmEngine *engine){
  (void)engine;
  resume_hint_queries++;
  return stub_resume_hint_bytes;
}
uint32_t anygm_state_capacity_flags(const AnygmEngine *engine){
  (void)engine;
  return stub_capacity_flags;
}
size_t anygm_state_capacity_reserve(const AnygmEngine *engine){
  (void)engine;
  return stub_capacity_reserve;
}
AnygmResult anygm_state_save(AnygmEngine *engine,void *data,size_t capacity,size_t *written){
  (void)engine;
  complete_save_attempts++;
  if(written) *written=0;
  if(!data || !written || capacity<stub_state_bytes) return ANYGM_ERROR_INVALID_ARGUMENT;
  memset(data,0x4D,stub_state_bytes);
  *written=stub_state_bytes;
  complete_saves++;
  return ANYGM_OK;
}
static unsigned stub_prepare_calls,stub_video_calls,stub_prepare_after_video;
void anygm_state_prepare(AnygmEngine *engine){
  (void)engine; stub_prepare_calls++;
  stub_prepare_after_video=stub_video_calls;
}
static void stub_video(const void *data,unsigned width,unsigned height,size_t pitch){
  (void)data;(void)width;(void)height;(void)pitch; stub_video_calls++;
}
AnygmResult anygm_state_save_for_resume(AnygmEngine *engine,void *data,size_t capacity,
                                        size_t *written){
  (void)engine;
  size_t required=stub_resume_state_bytes?stub_resume_state_bytes:stub_state_bytes;
  if(written) *written=0;
  if(!data || !written || capacity<required) return ANYGM_ERROR_INVALID_ARGUMENT;
  memset(data,0x52,required);
  *written=required;
  resume_saves++;
  return ANYGM_OK;
}
AnygmResult anygm_state_load(AnygmEngine *engine,const void *data,size_t size){
  (void)engine;
  if(!data || !size) return ANYGM_ERROR_INVALID_STATE;
  const uint8_t *bytes=(const uint8_t *)data;
  size_t logical=bytes[0]==0x52?stub_resume_state_bytes:stub_state_bytes;
  uint8_t marker=bytes[0]==0x52?0x52:0x4D;
  if(size<logical) return ANYGM_ERROR_INVALID_STATE;
  for(size_t i=0;i<logical;i++) if(bytes[i]!=marker) return ANYGM_ERROR_INVALID_STATE;
  for(size_t i=logical;i<size;i++) if(bytes[i]!=0) return ANYGM_ERROR_INVALID_STATE;
  last_load_bytes=size;
  return ANYGM_OK;
}
size_t anygm_get_last_error(const AnygmEngine *engine,char *message,size_t capacity){
  (void)engine;
  if(message && capacity) message[0]=0;
  return 0;
}

/* The hardware bridge is part of the adapter's lifecycle, so its entry points exist here too.
 * This scenario never turns the setting on, so they do nothing. */
#if ANYGM_HARDWARE_RENDER
bool libretro_hw_render_request(bool needed){ (void)needed; return false; }
uint32_t anygm_graphics_present_emergency_frame(AnygmEngine *engine,const uint32_t *pixels,
                                                uint32_t width,uint32_t height){
  (void)engine;
  (void)pixels;
  (void)width;
  (void)height;
  return 0;
}

void libretro_hw_render_release(void){}
bool libretro_hw_render_requested(void){ return false; }
#endif
void libretro_vfs_request(void){}
void libretro_vfs_services_init(AnygmHostServices *services){ (void)services; }
bool libretro_vfs_clear_cache(const char *path,bool keep_root){
  (void)path; (void)keep_root; return true;
}
static unsigned options_registered;
void libretro_options_register(void){ options_registered++; }
static unsigned options_applied;
void libretro_options_apply(bool all_fields){ (void)all_fields; options_applied++; }
void libretro_options_finalize_graphics(bool available){ (void)available; }
/* Unset, as a frontend answers before the player touches anything: the locale stays on Auto. */
const char *libretro_options_value(const char *key){ (void)key; return NULL; }
void libretro_options_publish_rooms(void){}
void libretro_options_release(void){}
void libretro_input_register(void){}
void libretro_input_snapshot(AnygmInputFrame *input,uint32_t width,uint32_t height){
  (void)width; (void)height;
  memset(input,0,sizeof *input);
  input->struct_size=sizeof *input;
  input->text_event_count=g_libretro.keyboard_text_count;
}
void libretro_input_text_acknowledge(uint32_t count){
  text_acknowledgements++;
  if(count>g_libretro.keyboard_text_count) count=g_libretro.keyboard_text_count;
  g_libretro.keyboard_text_count-=count;
}

static int begin_frontend(int variable_support){
  memset(&g_libretro,0,sizeof g_libretro);
  variable_frontend=variable_support;
  serialization_query_seen=0;
  development_setting_seen=0;
  stub_frame_result=ANYGM_OK; stub_text_consumed=0; text_acknowledgements=0;
  last_load_bytes=0;
  stub_hint_bytes=0;
  stub_resume_hint_bytes=0;
  stub_capacity_flags=0;
  stub_capacity_reserve=0;
  snapshot_failure_messages=0;
  stub_resume_state_bytes=0;
  complete_saves=0;
  complete_save_attempts=0;
  resume_saves=0;
  resume_size_queries=0;
  complete_hint_queries=0;
  resume_hint_queries=0;
  if(setenv("ANYGM_TEST_SETTING","visible",1)!=0) return 0;
  retro_set_environment(environment_callback);
  retro_init();
  if(!serialization_query_seen || !development_setting_seen || !g_libretro.engine ||
     g_libretro.variable_state_supported!=(variable_support>0)) return 0;
  g_libretro.loaded=true;
  return 1;
}

/* Once a fixed-size frontend has allocated its session ring, another size query must be a cheap
 * read of that capacity. RetroArch makes this query before every rewind push; walking the runtime
 * graph here would duplicate the traversal that retro_serialize performs immediately afterward. */
static int fixed_frontend_reuses_the_measured_capacity(int negotiation_result){
  stub_state_bytes=113u;
  if(!begin_frontend(negotiation_result)) return 0;
  size_t capacity=retro_serialize_size();
  unsigned resume_sizes=resume_size_queries;
  unsigned complete_hints=complete_hint_queries;
  unsigned resume_hints=resume_hint_queries;
  if(!capacity || !resume_sizes || !complete_hints || !resume_hints) return 0;
  stub_state_bytes=capacity+1u;
  if(retro_serialize_size()!=capacity || resume_size_queries!=resume_sizes ||
     complete_hint_queries!=complete_hints || resume_hint_queries!=resume_hints) return 0;
  retro_run();
  if(retro_serialize_size()!=capacity || resume_size_queries!=resume_sizes ||
     complete_hint_queries!=complete_hints || resume_hint_queries!=resume_hints) return 0;
  retro_unload_game();
  retro_deinit();
  return 1;
}

/* A measured deferred graph can exceed the ordinary reserve. Its explicit minimum must
 * exist before that growth, including when variable sizing is unsupported. */
static int measured_growth_keeps_a_larger_fixed_reserve(int negotiation_result,int compact){
  stub_state_bytes=320u*1024u;
  if(!begin_frontend(negotiation_result)) return 0;
  stub_capacity_flags=ANYGM_STATE_CAPACITY_DYNAMIC_CONTAINERS;
  stub_capacity_reserve=32u*1024u*1024u;
  stub_hint_bytes=compact?16u*1024u*1024u:512u*1024u;
  const size_t capacity=retro_serialize_size();
  int ok=capacity==32u*1024u*1024u;
  retro_run();
  stub_state_bytes=24u*1024u*1024u;
  stub_resume_state_bytes=stub_state_bytes;
  uint8_t *state=malloc(capacity);
  if(!state) ok=0;
  if(ok) ok=retro_serialize_size()==capacity && retro_serialize(state,capacity) &&
            complete_saves==1u && resume_saves==0u && retro_unserialize(state,capacity);
  retro_reset();
  if(ok) ok=retro_serialize_size()==capacity;
  retro_run();
  if(ok) ok=retro_serialize(state,capacity) && retro_unserialize(state,capacity);
  free(state);
  retro_unload_game();
  retro_deinit();
  return ok;
}

static int container_mentions_keep_declared_capacity(int negotiation_result,int compact){
  stub_state_bytes=64u*1024u;
  if(!begin_frontend(negotiation_result)) return 0;
  stub_capacity_flags=ANYGM_STATE_CAPACITY_DYNAMIC_CONTAINERS;
  stub_capacity_reserve=(compact?1024u:4608u)*1024u;
  stub_hint_bytes=(compact?16u:2u)*1024u*1024u;
  size_t expected=(compact?1024u:4608u)*1024u;
  int ok=retro_serialize_size()==expected;
  retro_unload_game();retro_deinit();
  return ok;
}

/* Unexpected fixed-session growth must be an observable rejection, never a false
 * successful push or a larger size answer that the frontend silently refuses first. */
static int rejected_snapshot_is_reported_once(int negotiation_result){
  stub_state_bytes=64u*1024u;
  if(!begin_frontend(negotiation_result)) return 0;
  stub_hint_bytes=16u*1024u*1024u;
  size_t capacity=retro_serialize_size();
  if(capacity!=1024u*1024u) return 0;
  retro_run();
  stub_state_bytes=stub_resume_state_bytes=2u*1024u*1024u;
  uint8_t *state=malloc(capacity);
  if(!state) return 0;
  int ok=retro_serialize_size()==capacity && !retro_serialize(state,capacity) &&
         !retro_serialize(state,capacity) && snapshot_failure_messages==1 &&
         g_libretro.state_snapshot_failure_reported && !complete_saves && !resume_saves;
  free(state);retro_unload_game();
  ok=ok && !g_libretro.state_snapshot_failure_reported;
  retro_deinit();return ok;
}

static int stable_transport(int negotiation_result){
  stub_state_bytes=113u;
  if(!begin_frontend(negotiation_result)) return 0;
  stub_capacity_reserve=4u*1024u*1024u;
  const size_t expected_capacity=4u*1024u*1024u;
  if(retro_serialize_size()!=expected_capacity ||
     retro_serialize_size()!=expected_capacity) return 0;
  stub_state_bytes=197u;
  if(retro_serialize_size()!=expected_capacity) return 0;
  uint8_t *state=(uint8_t *)malloc(expected_capacity);
  if(!state) return 0;
  memset(state,0xA5,expected_capacity);
  int ok=retro_serialize(state,expected_capacity);
  for(size_t i=0;ok && i<stub_state_bytes;i++) if(state[i]!=0x4D) ok=0;
  for(size_t i=stub_state_bytes;ok && i<expected_capacity;i++) if(state[i]!=0) ok=0;
  if(ok) ok=retro_unserialize(state,expected_capacity) &&
            last_load_bytes==expected_capacity;
  free(state);
  retro_unload_game();
  if(g_libretro.fixed_state_capacity!=0) ok=0;
  retro_deinit();
  return ok;
}

/* A rewind ring saves every Nth tick. Once one interval has repeated, the tick that completes the
 * next announces the save to the engine once its frame is handed over, so the frame is presented
 * on time while the save is prepared; a restored state forgets the interval, and only an interval
 * seen twice predicts. */
static int repeated_save_interval_announces_the_next_save(void){
  stub_state_bytes=113u;
  if(!begin_frontend(0)) return 0;
  g_libretro.video=stub_video;
  g_libretro.video_enabled=true;
  stub_video_calls=stub_prepare_after_video=0;
  stub_capacity_reserve=4u*1024u*1024u;
  const size_t capacity=retro_serialize_size();
  uint8_t *state=(uint8_t *)malloc(capacity);
  int ok=state!=NULL;
  unsigned announced[4]={0};
  for(int save=0;ok && save<4;save++){
    unsigned before=stub_prepare_calls;
    for(int tick=0;tick<3;tick++) retro_run();
    announced[save]=stub_prepare_calls-before;
    ok=retro_serialize(state,capacity);
  }
  /* The second save repeats the first interval of three ticks, so the third and fourth saves are
   * announced, each once, on the tick that completes the interval. */
  if(!ok || announced[0] || announced[1] || announced[2]!=1 || announced[3]!=1)
    fprintf(stderr,"save announcements: %u %u %u %u\n",announced[0],announced[1],announced[2],
            announced[3]);
  if(ok) ok=!announced[0] && !announced[1] && announced[2]==1 && announced[3]==1;
  /* The announcing tick hands its frame over first: the last announcement saw every frame. */
  if(ok && stub_prepare_after_video!=stub_video_calls){
    fprintf(stderr,"save announced before its frame: %u of %u frames\n",stub_prepare_after_video,
            stub_video_calls);
    ok=0;
  }
  if(ok) ok=retro_unserialize(state,capacity);
  unsigned before=stub_prepare_calls;
  for(int tick=0;ok && tick<3;tick++) retro_run();
  if(ok) ok=stub_prepare_calls==before;
  free(state);
  retro_unload_game();
  retro_deinit();
  return ok;
}

/* Content can populate render and language-level tables after the first size answer. Its declared
 * budget must cover that measured transition without enlarging a fixed frontend's allocation. */
static int fixed_ordinary_ring_covers_expected_runtime_growth(int negotiation_result){
  stub_state_bytes=113u;
  if(!begin_frontend(negotiation_result)) return 0;
  stub_capacity_reserve=4u*1024u*1024u;
  const size_t capacity=retro_serialize_size();
  if(capacity!=4u*1024u*1024u) return 0;
  stub_state_bytes=3u*1024u*1024u;
  if(retro_serialize_size()!=capacity) return 0;
  uint8_t *state=(uint8_t *)malloc(capacity);
  if(!state) return 0;
  int ok=retro_serialize(state,capacity) && complete_saves==1u && resume_saves==0u;
  free(state);
  retro_unload_game();
  retro_deinit();
  return ok;
}

/* Acknowledged variable-size frontends receive a larger answer when required. An unacknowledged
 * frontend retains its first answer, as baseline libretro requires; RetroArch fixes its rewind
 * allocation from that answer and refuses snapshots itself if a later query is larger. */
static int growth_follows_frontend_acknowledgement(int negotiation_result){
  stub_state_bytes=113u;
  if(!begin_frontend(negotiation_result)) return 0;
  const size_t initial_capacity=retro_serialize_size();
  stub_state_bytes=initial_capacity+1u;
  const size_t current_capacity=retro_serialize_size();
  if(negotiation_result>0){
    if(current_capacity<=initial_capacity || retro_serialize_size()!=current_capacity) return 0;
  } else if(current_capacity!=initial_capacity || retro_serialize_size()!=initial_capacity)
    return 0;
  uint8_t *state=(uint8_t *)malloc(current_capacity);
  if(!state) return 0;
  int ok=retro_serialize(state,current_capacity)==(negotiation_result>0);
  if(ok && negotiation_result>0){
    uint8_t *stale=(uint8_t *)malloc(initial_capacity);
    ok=stale && !retro_serialize(stale,initial_capacity);
    free(stale);
  }
  free(state);
  retro_unload_game();
  retro_deinit();
  return ok;
}

/* Content declares a budget covering its recorded required peak before the first fixed allocation,
 * without also charging the optional monitor-sized picture to every slot. */
static int declared_budget_covers_recorded_peak_from_first_answer(void){
  stub_state_bytes=113u;
  if(!begin_frontend(0)) return 0;
  stub_capacity_reserve=8u*1024u*1024u;
  stub_resume_hint_bytes=6u*1024u*1024u;
  const size_t capacity=retro_serialize_size();
  int ok=capacity>=stub_resume_hint_bytes;
  stub_state_bytes=197u;
  if(retro_serialize_size()!=capacity) ok=0;
  retro_unload_game();
  retro_deinit();
  return ok;
}

/* A frontend that does not acknowledge variable sizes sees one compact capacity for the complete
 * loaded session. This is the RetroArch contract: its wrapper compares every current size query
 * with the load-time rewind allocation before calling retro_serialize. The representation first
 * attempts the complete picture, then falls back within that same stable capacity. */
static int fixed_compact_ring_survives_frontend_size_checks(void){
  stub_state_bytes=9u*1024u*1024u;
  if(!begin_frontend(0)) return 0;
  stub_resume_state_bytes=96u*1024u;
  stub_resume_hint_bytes=128u*1024u;
  stub_hint_bytes=16u*1024u*1024u;
  size_t ring_capacity=retro_serialize_size();
  size_t expected_ring=1u*1024u*1024u;
  if(ring_capacity!=expected_ring || retro_serialize_size()!=ring_capacity) return 0;
  retro_run();
  if(retro_serialize_size()!=ring_capacity) return 0;
  uint8_t *ring=malloc(ring_capacity);
  if(!ring) return 0;
  memset(ring,0xA5,ring_capacity);
  int ok=retro_serialize(ring,ring_capacity) && resume_saves==1u && complete_saves==0u;
  for(size_t i=0;ok && i<stub_resume_state_bytes;i++) if(ring[i]!=0x52) ok=0;
  for(size_t i=stub_resume_state_bytes;ok && i<ring_capacity;i++) if(ring[i]) ok=0;
  if(ok) ok=retro_unserialize(ring,ring_capacity) && last_load_bytes==ring_capacity;
  free(ring);
  retro_unload_game();
  retro_deinit();
  return ok;
}

/* A content budget covers known required render growth even when the cold serialized form is
 * tiny. The resulting compact ring still preserves every required section after that growth. */
static int compact_ring_covers_known_required_growth(void){
  stub_state_bytes=32u*1024u;
  if(!begin_frontend(0)) return 0;
  stub_capacity_reserve=2u*1024u*1024u;
  stub_resume_hint_bytes=1280u*1024u;
  stub_hint_bytes=8u*1024u*1024u;
  size_t ring_capacity=retro_serialize_size();
  if(ring_capacity<stub_resume_hint_bytes || !g_libretro.startup_ring_compact){
    fprintf(stderr,"known-growth ring setup failed: capacity=%zu required=%zu compact=%d\n",
            ring_capacity,stub_resume_hint_bytes,g_libretro.startup_ring_compact);
    return 0;
  }
  retro_run();
  stub_state_bytes=1650u*1024u;
  stub_resume_state_bytes=stub_resume_hint_bytes;
  uint8_t *ring=malloc(ring_capacity);
  if(!ring) return 0;
  int ok=retro_serialize_size()==ring_capacity && retro_serialize(ring,ring_capacity) &&
         complete_saves+resume_saves==1u && retro_unserialize(ring,ring_capacity);
  if(!ok)
    fprintf(stderr,"known-growth ring failed: capacity=%zu state=%zu resume=%zu saves=%u\n",
            ring_capacity,stub_state_bytes,stub_resume_state_bytes,resume_saves);
  free(ring);
  retro_unload_game();
  retro_deinit();
  return ok;
}

/* A moderate lossless picture fits below the declared four-MiB budget. Keep the complete form
 * through cold-to-gameplay growth: a fixed frontend cannot distinguish manual saves from
 * rewind pushes, and presenting a frame-free restored slot would make visible rewind inexact. */
static int moderate_frame_keeps_complete_growth_headroom(void){
  stub_state_bytes=75u*1024u;
  if(!begin_frontend(0)) return 0;
  stub_capacity_reserve=4u*1024u*1024u;
  stub_resume_hint_bytes=75u*1024u;
  stub_hint_bytes=1600u*1024u;
  const size_t ring_capacity=retro_serialize_size();
  const size_t expected_capacity=4u*1024u*1024u;
  if(ring_capacity!=expected_capacity || g_libretro.startup_ring_compact){
    fprintf(stderr,"moderate-frame ring setup failed: capacity=%zu expected=%zu compact=%d\n",
            ring_capacity,expected_capacity,g_libretro.startup_ring_compact);
    return 0;
  }
  retro_run();
  stub_state_bytes=1750u*1024u;
  uint8_t *ring=malloc(ring_capacity);
  if(!ring) return 0;
  int ok=retro_serialize_size()==ring_capacity && retro_serialize(ring,ring_capacity) &&
         complete_saves==1u && resume_saves==0u && retro_unserialize(ring,ring_capacity);
  if(!ok)
    fprintf(stderr,"moderate-frame ring failed: capacity=%zu state=%zu complete=%u resume=%u\n",
            ring_capacity,stub_state_bytes,complete_saves,resume_saves);
  free(ring);
  retro_unload_game();
  retro_deinit();
  return ok;
}

/* A frontend that explicitly accepts variable state sizes can grow past its compact startup ring.
 * An old ring slot attempts a complete state first and falls back when it cannot fit, while a
 * newly sized ordinary save carries its frame directly. */
static int variable_frontend_keeps_compact_ring_and_complete_save(void){
  stub_state_bytes=9u*1024u*1024u;
  if(!begin_frontend(1)) return 0;
  stub_resume_state_bytes=96u*1024u;
  stub_resume_hint_bytes=128u*1024u;
  stub_hint_bytes=16u*1024u*1024u;
  size_t ring_capacity=retro_serialize_size();
  retro_run();
  size_t save_capacity=retro_serialize_size();
  if(save_capacity<=ring_capacity || save_capacity<stub_hint_bytes) return 0;
  uint8_t *ring=malloc(ring_capacity),*save=malloc(save_capacity);
  if(!ring || !save){ free(ring); free(save); return 0; }
  int ok=retro_serialize(ring,ring_capacity) && resume_saves==1u && complete_saves==0u &&
         complete_save_attempts==1u && g_libretro.startup_complete_frame_refused;
  if(ok) ok=retro_serialize(ring,ring_capacity) && resume_saves==2u &&
            complete_save_attempts==1u;
  if(ok) ok=retro_serialize(save,save_capacity) && complete_saves==1u && resume_saves==2u &&
            complete_save_attempts==2u;
  free(ring);
  free(save);
  retro_unload_game();
  retro_deinit();
  return ok;
}

/* A modest completed frame stays in rewind. The compact form is a targeted escape from a picture
 * large enough to dominate the fixed ring, not a global trade of exact rewind pictures for
 * smaller slots. */
static int ordinary_raster_ring_keeps_the_complete_frame(void){
  stub_state_bytes=256u*1024u;
  if(!begin_frontend(0)) return 0;
  stub_resume_state_bytes=96u*1024u;
  stub_resume_hint_bytes=128u*1024u;
  stub_hint_bytes=768u*1024u;
  size_t ring_capacity=retro_serialize_size();
  if(ring_capacity<stub_hint_bytes) return 0;
  retro_run();
  uint8_t *ring=malloc(ring_capacity);
  if(!ring) return 0;
  int ok=retro_serialize(ring,ring_capacity) && complete_saves==1u && resume_saves==0u;
  free(ring);
  retro_unload_game();
  retro_deinit();
  return ok;
}

/* A raw completed-picture ceiling and twice the known required state can fit in one small slot.
 * Query before the first frame, grow the simulation, change presentation, and cross Reset using
 * a fixed frontend's size check before each push. The variable branch also probes the codec's old
 * compact slot directly; a wrapper which refuses a growing answer cannot use that direct path. */
static int small_complete_ring_covers_growth_and_reset(int negotiation_result){
  stub_state_bytes=43u*1024u;
  if(!begin_frontend(negotiation_result)) return 0;
  stub_resume_state_bytes=stub_state_bytes;
  stub_resume_hint_bytes=stub_state_bytes;
  stub_hint_bytes=900u*1024u;
  const size_t capacity=retro_serialize_size();
  if(capacity!=1024u*1024u) return 0;
  uint8_t *state=malloc(capacity);
  if(!state) return 0;
  int ok=retro_serialize_size()<=capacity && retro_serialize(state,capacity);
  retro_run();
  stub_resume_state_bytes=86u*1024u;
  stub_state_bytes=stub_hint_bytes+stub_resume_state_bytes;
  if(negotiation_result<=0 && retro_serialize_size()!=capacity) ok=0;
  if(ok) ok=retro_serialize(state,capacity) && complete_saves==2u && resume_saves==0u &&
            retro_unserialize(state,capacity);
  if(negotiation_result<=0 && retro_serialize_size()!=capacity) ok=0;
  /* A larger live monitor can exceed the old picture's ceiling without growing required state. */
  stub_hint_bytes=8u*1024u*1024u;
  stub_state_bytes=stub_hint_bytes+stub_resume_state_bytes;
  size_t current=retro_serialize_size();
  if(negotiation_result<=0 && current!=capacity) ok=0;
  if(negotiation_result>0 && current<=capacity) ok=0;
  if(ok) ok=retro_serialize(state,capacity) && resume_saves==1u &&
            retro_unserialize(state,capacity);
  retro_reset();
  if(ok) ok=!retro_unserialize(state,capacity);
  retro_run();
  if(ok) ok=retro_unserialize(state,capacity);
  free(state);
  retro_unload_game();
  retro_deinit();
  return ok;
}

/* The complete picture and required bytes fit together even when duplicating the required bytes
 * would not. Required-state growth has its own allowance; keep an exact picture in this slot. */
static int small_complete_ring_counts_required_bytes_once(int negotiation_result){
  stub_state_bytes=80u*1024u;
  if(!begin_frontend(negotiation_result)) return 0;
  stub_resume_state_bytes=stub_state_bytes;
  stub_resume_hint_bytes=stub_state_bytes;
  stub_hint_bytes=900u*1024u;
  stub_capacity_flags=ANYGM_STATE_CAPACITY_DYNAMIC_CONTAINERS;
  const size_t capacity=retro_serialize_size();
  int ok=capacity==1024u*1024u;
  uint8_t *state=ok?malloc(capacity):NULL;
  if(!state) ok=0;
  if(ok) ok=retro_serialize(state,capacity);
  if(ok){
    retro_run();
    stub_resume_state_bytes=96u*1024u;
    stub_state_bytes=stub_hint_bytes+stub_resume_state_bytes;
    ok=retro_serialize_size()==capacity && retro_serialize(state,capacity) &&
       complete_saves==2u && resume_saves==0u && retro_unserialize(state,capacity);
  }
  if(!ok) fprintf(stderr,"small complete ring duplicated required bytes: capacity=%zu\n",capacity);
  free(state);
  stub_capacity_flags=0;
  retro_unload_game();
  retro_deinit();
  return ok;
}

/* Content with measured later surface growth declares its budget before those surfaces exist;
 * a fixed frontend cannot grow its ring afterwards. */
static int deferred_surfaces_keep_the_declared_reserve(int negotiation_result){
  stub_state_bytes=64u*1024u;
  if(!begin_frontend(negotiation_result)) return 0;
  stub_capacity_reserve=4u*1024u*1024u;
  stub_resume_state_bytes=stub_state_bytes;
  stub_resume_hint_bytes=0;
  stub_hint_bytes=320u*1024u;
  stub_capacity_flags=ANYGM_STATE_CAPACITY_DYNAMIC_SURFACES;
  size_t capacity=retro_serialize_size();
  int ok=capacity==4u*1024u*1024u;
  retro_run();
  stub_state_bytes=3400u*1024u;
  stub_resume_state_bytes=3100u*1024u;
  uint8_t *state=malloc(capacity);
  if(!state) ok=0;
  if(ok) ok=retro_serialize_size()==capacity && retro_serialize(state,capacity) &&
            complete_saves==1u && resume_saves==0u && retro_unserialize(state,capacity);
  free(state);
  stub_capacity_flags=0;
  retro_unload_game();
  retro_deinit();
  return ok;
}

/* The declared budget covers this two-MiB picture and later required growth. Retain the complete
 * form so both the picture and the growing simulation remain serializable. */
static int authored_raster_with_declared_reserve_keeps_complete_form(void){
  stub_state_bytes=768u*1024u;
  if(!begin_frontend(0)) return 0;
  stub_capacity_reserve=4608u*1024u;
  stub_resume_state_bytes=96u*1024u;
  stub_resume_hint_bytes=128u*1024u;
  stub_hint_bytes=2u*1024u*1024u;
  size_t ring_capacity=retro_serialize_size();
  if(ring_capacity!=4608u*1024u || g_libretro.startup_ring_compact) return 0;
  retro_run();
  stub_state_bytes=2200u*1024u;
  stub_resume_state_bytes=1200u*1024u;
  uint8_t *ring=malloc(ring_capacity);
  if(!ring) return 0;
  int ok=retro_serialize(ring,ring_capacity) && complete_saves==1u && resume_saves==0u;
  free(ring);
  retro_unload_game();
  retro_deinit();
  return ok;
}

/* RetroArch retains its rewind manager across a core reset. If the rewind chord used while leaving
 * its menu is still active, the frontend pops the old ring before it lets the newly reset core run
 * even one frame. The one ambiguous operation is refused: a compact-capacity state cannot replace
 * a reset until its first frame has run. When variable sizing provides one, a separately sized
 * complete manual state remains loadable; the ring state becomes loadable after that frame,
 * preserving deliberate rewind. */
static int restart_rejects_only_an_immediate_old_ring_state(int negotiation_result){
  stub_state_bytes=9u*1024u*1024u;
  if(!begin_frontend(negotiation_result)) return 0;
  stub_resume_state_bytes=96u*1024u;
  stub_resume_hint_bytes=128u*1024u;
  stub_hint_bytes=16u*1024u*1024u;
  size_t ring_capacity=retro_serialize_size();
  retro_run();
  size_t save_capacity=retro_serialize_size();
  uint8_t *ring=malloc(ring_capacity);
  uint8_t *save=negotiation_result>0?malloc(save_capacity):NULL;
  if(!ring || (negotiation_result>0 && !save)){ free(ring); free(save); return 0; }
  int ok=retro_serialize(ring,ring_capacity);
  if(ok && save) ok=retro_serialize(save,save_capacity);
  if(ok){
    retro_reset();
    ok=!retro_unserialize(ring,ring_capacity);
    if(ok && save) ok=retro_unserialize(save,save_capacity);
  }
  if(ok){
    retro_reset();
    ok=!retro_unserialize(ring,ring_capacity);
  }
  if(ok){
    retro_run();
    ok=retro_unserialize(ring,ring_capacity);
  }
  if(!ok) fprintf(stderr,"restart accepted an old rewind state before its first frame\n");
  free(ring);
  free(save);
  retro_unload_game();
  retro_deinit();
  return ok;
}

void retro_reset(void);

/* A player changes a setting in the host's menu and restarts from that same menu, so the core
 * never runs a frame in between; anything read only from inside the frame loop would apply to the
 * boot after the one the player asked for. Settings read at boot are read on restart. */
/* Starting the core declares its settings, whether or not the host hands its callback over again:
 * tearing down releases what the declaration holds, and a start that skipped it would leave the
 * host with nothing to show. */
static int starting_declares_the_settings(void){
  if(!begin_frontend(1)) return 0;
  unsigned before=options_registered;
  retro_init();
  int ok=options_registered>before;
  if(!ok) fprintf(stderr,"a started core declared no settings\n");
  retro_unload_game();
  retro_deinit();
  return ok;
}

static int restart_rereads_settings(void){
  if(!begin_frontend(1)) return 0;
  unsigned before=options_applied;
  retro_reset();
  int ok=options_applied>before;
  if(!ok) fprintf(stderr,"restart booted without re-reading the settings\n");
  retro_unload_game();
  retro_deinit();
  return ok;
}

static int frame_failure_acknowledges_only_consumed_text(void){
  if(!begin_frontend(1)) return 0;
  g_libretro.keyboard_text_count=1;
  stub_frame_result=ANYGM_ERROR_INTERNAL;
  retro_run();
  int ok=g_libretro.keyboard_text_count==1 && text_acknowledgements==0;
  stub_text_consumed=1;
  retro_run();
  ok=ok && !g_libretro.keyboard_text_count && text_acknowledgements==1;
  if(!ok) fputs("failed frame lost unconsumed text or retained consumed text\n",stderr);
  retro_unload_game(); retro_deinit(); return ok;
}

/* Known required storage receives automatic headroom before all three frontend negotiations
 * fix their first allocation. Constructor mentions alone do not supply a size estimate. */
static int cold_known_storage_sizes_the_default_budget(int negotiation_result){
  stub_state_bytes=64u*1024u;
  if(!begin_frontend(negotiation_result)) return 0;
  stub_hint_bytes=2u*1024u*1024u;
  stub_capacity_flags=ANYGM_STATE_CAPACITY_DYNAMIC_CONTAINERS |
                      ANYGM_STATE_CAPACITY_DYNAMIC_SURFACES;
  const size_t capacity=retro_serialize_size();
  if(capacity!=2u*1024u*1024u){
    fprintf(stderr,"known cold storage returned %zu bytes\n",capacity);
    return 0;
  }
  /* The completed frame resolves to the small representation; acknowledged variable sizing
   * remains free to grow when a later real representation actually requires it. */
  stub_hint_bytes=stub_state_bytes;
  retro_run();
  uint8_t *state=malloc(capacity);
  if(!state) return 0;
  int ok=retro_serialize_size()==capacity && retro_serialize(state,capacity) &&
         complete_saves==1u && resume_saves==0u && retro_unserialize(state,capacity);
  free(state);
  retro_unload_game();retro_deinit();
  return ok;
}

/* A measured larger requirement is declared before the first query. The frontend allocates that
 * capacity before any frame and the exact same slot accepts the later complete representation. */
static int explicit_budget_sizes_the_cold_fixed_ring(int negotiation_result){
  stub_state_bytes=64u*1024u;
  if(!begin_frontend(negotiation_result)) return 0;
  stub_capacity_reserve=2u*1024u*1024u;
  stub_hint_bytes=2u*1024u*1024u;
  const size_t capacity=retro_serialize_size();
  if(capacity!=2u*1024u*1024u){
    fprintf(stderr,"declared content reserve returned %zu bytes\n",capacity);
    return 0;
  }
  retro_run();
  stub_state_bytes=1400u*1024u;
  uint8_t *state=malloc(capacity);
  if(!state) return 0;
  int ok=retro_serialize_size()==capacity && retro_serialize(state,capacity) &&
         complete_saves==1u && resume_saves==0u && retro_unserialize(state,capacity);
  free(state);
  retro_unload_game();retro_deinit();
  return ok;
}

/* The first query can occur after gameplay. Every frontend needs a usable first allocation;
 * acknowledgement controls subsequent growth rather than this initial measurement. */
static int first_size_query_after_gameplay_respects_negotiation(int negotiation_result){
  stub_state_bytes=64u*1024u;
  if(!begin_frontend(negotiation_result)) return 0;
  retro_run();
  stub_state_bytes=1500u*1024u;
  stub_resume_state_bytes=stub_state_bytes;
  stub_resume_hint_bytes=stub_state_bytes;
  stub_hint_bytes=stub_state_bytes;
  size_t capacity=retro_serialize_size();
  uint8_t *state=malloc(capacity?capacity:1u);
  if(!state) return 0;
  int ok=capacity>=stub_state_bytes && retro_serialize(state,capacity) &&
         retro_serialize_size()==capacity && complete_saves==1u && resume_saves==0u &&
         retro_unserialize(state,capacity);
  if(!ok)
    fprintf(stderr,"late first state query failed: negotiation=%d capacity=%zu required=%zu\n",
            negotiation_result,capacity,stub_state_bytes);
  free(state);
  retro_unload_game();retro_deinit();
  return ok;
}

/* A cold frontend must receive enough space for the already-known required state without
 * requiring the content author to predict a buffer budget. Later fixed queries remain stable. */
static int cold_required_state_fits_without_a_declaration(int negotiation_result){
  stub_state_bytes=64u*1024u;
  if(!begin_frontend(negotiation_result)) return 0;
  stub_state_bytes=1500u*1024u;
  stub_resume_state_bytes=stub_state_bytes;
  stub_resume_hint_bytes=stub_state_bytes;
  stub_hint_bytes=stub_state_bytes;
  size_t capacity=retro_serialize_size();
  uint8_t *state=malloc(capacity?capacity:1u);
  if(!state) return 0;
  int ok=capacity>=stub_state_bytes && retro_serialize(state,capacity) &&
         retro_serialize_size()==capacity && retro_unserialize(state,capacity) &&
         snapshot_failure_messages==0u;
  if(!ok) fprintf(stderr,"cold required state rejected: negotiation=%d capacity=%zu required=%zu\n",
                  negotiation_result,capacity,stub_state_bytes);
  free(state);
  retro_unload_game();retro_deinit();
  return ok;
}

/* Advisory capacities already describe their whole storage. Cold sizing must fit the known
 * sections and their later growth without doubling an existing near-full capacity estimate. */
static int cold_capacity_hint_avoids_duplicate_growth(int negotiation_result,int declared){
  stub_state_bytes=6u*1024u*1024u;
  if(!begin_frontend(negotiation_result)) return 0;
  stub_resume_hint_bytes=stub_state_bytes;
  stub_hint_bytes=15u*1024u*1024u/2u;
  stub_capacity_reserve=declared?8u*1024u*1024u:0;
  size_t capacity=retro_serialize_size();
  if(capacity<stub_hint_bytes || capacity>8u*1024u*1024u){
    fprintf(stderr,"cold capacity duplicated its hint: capacity=%zu hint=%zu\n",capacity,stub_hint_bytes);
    return 0;
  }
  retro_run();
  stub_state_bytes=7u*1024u*1024u;
  uint8_t *state=malloc(capacity);
  if(!state) return 0;
  int ok=retro_serialize_size()==capacity && retro_serialize(state,capacity) &&
         retro_unserialize(state,capacity) && !snapshot_failure_messages;
  free(state);retro_unload_game();retro_deinit();
  return ok;
}

int main(void){
  uint8_t byte=0;
  memset(&g_libretro,0,sizeof g_libretro);
  for(int negotiation=-1;negotiation<=1;negotiation++)
    for(int declared=0;declared<2;declared++)
      if(!cold_capacity_hint_avoids_duplicate_growth(negotiation,declared)) return 1;
  if(!cold_required_state_fits_without_a_declaration(0) ||
     !cold_required_state_fits_without_a_declaration(-1) ||
     !cold_required_state_fits_without_a_declaration(1)) return 1;
  if(retro_serialize_size()!=0 || retro_serialize(&byte,1) || retro_unserialize(&byte,1) ||
     !stable_transport(1) || !stable_transport(0) || !stable_transport(-1) ||
     !repeated_save_interval_announces_the_next_save() ||
     !first_size_query_after_gameplay_respects_negotiation(1) ||
     !first_size_query_after_gameplay_respects_negotiation(0) ||
     !first_size_query_after_gameplay_respects_negotiation(-1) ||
     !fixed_ordinary_ring_covers_expected_runtime_growth(0) ||
     !fixed_ordinary_ring_covers_expected_runtime_growth(-1) ||
     !measured_growth_keeps_a_larger_fixed_reserve(1,0) ||
     !measured_growth_keeps_a_larger_fixed_reserve(0,0) ||
     !measured_growth_keeps_a_larger_fixed_reserve(-1,0) ||
     !measured_growth_keeps_a_larger_fixed_reserve(1,1) ||
     !measured_growth_keeps_a_larger_fixed_reserve(0,1) ||
     !measured_growth_keeps_a_larger_fixed_reserve(-1,1) ||
     !container_mentions_keep_declared_capacity(1,0) ||
     !container_mentions_keep_declared_capacity(0,0) ||
     !container_mentions_keep_declared_capacity(-1,0) ||
     !container_mentions_keep_declared_capacity(1,1) ||
     !container_mentions_keep_declared_capacity(0,1) ||
     !container_mentions_keep_declared_capacity(-1,1) ||
     !rejected_snapshot_is_reported_once(0) ||
     !rejected_snapshot_is_reported_once(-1) ||
     !growth_follows_frontend_acknowledgement(1) ||
     !growth_follows_frontend_acknowledgement(0) ||
     !growth_follows_frontend_acknowledgement(-1) ||
     !fixed_frontend_reuses_the_measured_capacity(0) ||
     !fixed_frontend_reuses_the_measured_capacity(-1) ||
     !declared_budget_covers_recorded_peak_from_first_answer() ||
     !fixed_compact_ring_survives_frontend_size_checks() ||
     !compact_ring_covers_known_required_growth() ||
     !moderate_frame_keeps_complete_growth_headroom() ||
     !variable_frontend_keeps_compact_ring_and_complete_save() ||
     !ordinary_raster_ring_keeps_the_complete_frame() ||
     !small_complete_ring_covers_growth_and_reset(1) ||
     !small_complete_ring_covers_growth_and_reset(0) ||
     !small_complete_ring_covers_growth_and_reset(-1) ||
     !small_complete_ring_counts_required_bytes_once(1) ||
     !small_complete_ring_counts_required_bytes_once(0) ||
     !small_complete_ring_counts_required_bytes_once(-1) ||
     !deferred_surfaces_keep_the_declared_reserve(0) ||
     !deferred_surfaces_keep_the_declared_reserve(-1) ||
     !authored_raster_with_declared_reserve_keeps_complete_form() ||
     !restart_rejects_only_an_immediate_old_ring_state(1) ||
     !restart_rejects_only_an_immediate_old_ring_state(0) ||
     !restart_rejects_only_an_immediate_old_ring_state(-1) ||
     !restart_rereads_settings() || !starting_declares_the_settings() ||
     !frame_failure_acknowledges_only_consumed_text() ||
     !cold_known_storage_sizes_the_default_budget(1) ||
     !cold_known_storage_sizes_the_default_budget(0) ||
     !cold_known_storage_sizes_the_default_budget(-1) ||
     !explicit_budget_sizes_the_cold_fixed_ring(1) ||
     !explicit_budget_sizes_the_cold_fixed_ring(0) ||
     !explicit_budget_sizes_the_cold_fixed_ring(-1)){
    fprintf(stderr,"libretro state transport contract failed\n");
    return 1;
  }
  puts("libretro state transport: ok");
  return 0;
}
