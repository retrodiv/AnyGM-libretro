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
static size_t stub_hint_bytes;
static size_t last_load_bytes;
static int variable_frontend;
/* A resume-scoped save leaves the completed frame out; the stub stands in for that by writing a
 * distinctly shorter state, so the adapter's choice of entry point is observable from the bytes as
 * well as from the counters. */
static size_t stub_resume_state_bytes;
static unsigned complete_saves;
static unsigned resume_saves;
static int serialization_query_seen;
static int development_setting_seen;

static bool environment_callback(unsigned command,void *data){
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
AnygmResult anygm_load(AnygmEngine *engine,const AnygmContentSource *source,
                       const AnygmLoadConfig *config){
  (void)engine; (void)source; (void)config; return ANYGM_OK;
}
void anygm_unload(AnygmEngine *engine){ (void)engine; }
AnygmResult anygm_reset(AnygmEngine *engine){ (void)engine; return ANYGM_OK; }
AnygmResult anygm_get_av_info(const AnygmEngine *engine,AnygmAvInfo *info){
  (void)engine; (void)info; return ANYGM_OK;
}
AnygmResult anygm_run_frame(AnygmEngine *engine,const AnygmInputFrame *input,
                            AnygmFrameOutput *output){
  (void)engine; (void)input; (void)output; return ANYGM_OK;
}
AnygmResult anygm_set_config(AnygmEngine *engine,const AnygmConfigDelta *delta){
  (void)engine; (void)delta; return ANYGM_OK;
}
AnygmResult anygm_set_runtime_override(AnygmEngine *engine,uint32_t slot,uint32_t enabled,
                                       const char *expression){
  (void)engine; (void)slot; (void)enabled; (void)expression; return ANYGM_OK;
}
size_t anygm_state_size(AnygmEngine *engine){ (void)engine; return stub_state_bytes; }
size_t anygm_state_capacity_hint(const AnygmEngine *engine){ (void)engine; return stub_hint_bytes; }
AnygmResult anygm_state_save(AnygmEngine *engine,void *data,size_t capacity,size_t *written){
  (void)engine;
  if(written) *written=0;
  if(!data || !written || capacity<stub_state_bytes) return ANYGM_ERROR_INVALID_ARGUMENT;
  memset(data,0x4D,stub_state_bytes);
  *written=stub_state_bytes;
  complete_saves++;
  return ANYGM_OK;
}
AnygmResult anygm_state_save_for_resume(AnygmEngine *engine,void *data,size_t capacity,
                                        size_t *written){
  (void)engine;
  if(written) *written=0;
  if(!data || !written || capacity<stub_resume_state_bytes) return ANYGM_ERROR_INVALID_ARGUMENT;
  memset(data,0x4D,stub_resume_state_bytes);
  *written=stub_resume_state_bytes;
  resume_saves++;
  return ANYGM_OK;
}
AnygmResult anygm_state_load(AnygmEngine *engine,const void *data,size_t size){
  (void)engine;
  if(!data || size<stub_state_bytes) return ANYGM_ERROR_INVALID_STATE;
  const uint8_t *bytes=(const uint8_t *)data;
  for(size_t i=0;i<stub_state_bytes;i++) if(bytes[i]!=0x4D) return ANYGM_ERROR_INVALID_STATE;
  for(size_t i=stub_state_bytes;i<size;i++) if(bytes[i]!=0) return ANYGM_ERROR_INVALID_STATE;
  last_load_bytes=size;
  return ANYGM_OK;
}
size_t anygm_get_last_error(const AnygmEngine *engine,char *message,size_t capacity){
  (void)engine;
  if(message && capacity) message[0]=0;
  return 0;
}

void libretro_vfs_request(void){}
void libretro_vfs_services_init(AnygmHostServices *services){ (void)services; }
static unsigned options_registered;
void libretro_options_register(void){ options_registered++; }
static unsigned options_applied;
void libretro_options_apply(bool all_fields){ (void)all_fields; options_applied++; }
/* Unset, as a frontend answers before the player touches anything: the locale stays on Auto. The
 * one setting these cases drive is whether a state carries its picture. */
static const char *exact_frame_setting;
const char *libretro_options_value(const char *key){
  if(key && !strcmp(key,"anygm_state_exact_frame")) return exact_frame_setting;
  return NULL;
}
void libretro_options_publish_rooms(void){}
void libretro_options_release(void){}
void libretro_input_register(void){}
void libretro_input_snapshot(AnygmInputFrame *input,uint32_t width,uint32_t height){
  (void)width; (void)height;
  memset(input,0,sizeof *input);
  input->struct_size=sizeof *input;
}

static int begin_frontend(int variable_support){
  memset(&g_libretro,0,sizeof g_libretro);
  variable_frontend=variable_support;
  exact_frame_setting=NULL;
  serialization_query_seen=0;
  development_setting_seen=0;
  last_load_bytes=0;
  stub_hint_bytes=0;
  if(setenv("ANYGM_TEST_SETTING","visible",1)!=0) return 0;
  retro_set_environment(environment_callback);
  retro_init();
  if(!serialization_query_seen || !development_setting_seen || !g_libretro.engine) return 0;
  g_libretro.loaded=true;
  return 1;
}

static int stable_transport(int negotiation_result){
  stub_state_bytes=113u;
  if(!begin_frontend(negotiation_result)) return 0;
  exact_frame_setting="On";
  const size_t expected_capacity=113u*2u+512u*1024u;
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

/* The size answer grows past its previous value whatever the frontend answered during quirk
 * negotiation. RetroArch stores the declared core-variable-size quirk without acknowledging it and
 * still re-queries the size on every save; when the answer stayed frozen at the boot-time
 * capacity, every mid-session save of content that had allocated failed. A serialize into a stale
 * older capacity — a rewind ring sized at load — must still fail cleanly rather than truncate. */
static int growing_transport(int negotiation_result){
  stub_state_bytes=113u;
  if(!begin_frontend(negotiation_result)) return 0;
  /* A complete state is what has to refuse to truncate; one written for a resumed run leaves the
   * picture out and legitimately fits capacities a complete one cannot. */
  exact_frame_setting="On";
  const size_t initial_capacity=retro_serialize_size();
  stub_state_bytes=initial_capacity+1u;
  const size_t grown_capacity=retro_serialize_size();
  if(grown_capacity<=initial_capacity || retro_serialize_size()!=grown_capacity) return 0;
  uint8_t *state=(uint8_t *)malloc(grown_capacity);
  if(!state) return 0;
  int ok=retro_serialize(state,grown_capacity);
  if(ok){
    /* Sized again, because what is being asserted is that a complete state refuses to truncate
     * into a capacity that has gone stale. A snapshot the frontend did not size is a different
     * question - it leaves the completed frame out, so it is far smaller and fits capacities a
     * complete one cannot, which is the whole point of the scope. */
    uint8_t *stale=(uint8_t *)malloc(initial_capacity);
    ok=stale && retro_serialize_size()==grown_capacity && !retro_serialize(stale,initial_capacity);
    free(stale);
  }
  free(state);
  retro_unload_game();
  retro_deinit();
  return ok;
}

/* A remembered peak from an earlier session raises the first answer of a fresh load, so a
 * frontend that sizes a rewind ring once, at load, covers the gameplay the content is known to
 * reach instead of only its boot state. */
static int remembered_peak_covers_first_answer(void){
  stub_state_bytes=113u;
  if(!begin_frontend(0)) return 0;
  stub_hint_bytes=6u*1024u*1024u;
  const size_t capacity=retro_serialize_size();
  int ok=capacity>=stub_hint_bytes;
  stub_state_bytes=197u;
  if(retro_serialize_size()!=capacity) ok=0;
  retro_unload_game();
  retro_deinit();
  return ok;
}

void retro_run(void);

/* Whether a state carries the picture that was on screen follows the player's setting, and
 * nothing else. Two earlier attempts inferred it from what the frontend did - the cadence of the
 * calls, then whether the size was asked for first - and both were wrong, because rewind_granularity
 * is a preference that says nothing and because RetroArch's rewind path asks for the size before
 * every snapshot exactly as a save does. So this asserts the answer is the same for every snapshot
 * however the frontend behaves: sized or not, at any cadence, in any order. */
static int snapshot_scope_follows_the_setting(void){
  stub_state_bytes=4096u;
  stub_resume_state_bytes=64u;
  if(!begin_frontend(0)) return 0;
  const size_t capacity=retro_serialize_size();
  uint8_t *state=(uint8_t *)malloc(capacity);
  if(!state) return 0;
  int ok=1;

  /* Unset, so the shipped answer applies: the picture stays out, and a rewind keeps its length. */
  exact_frame_setting=NULL;
  complete_saves=resume_saves=0;
  if(!retro_serialize(state,capacity) || resume_saves!=1u || complete_saves!=0u){
    fprintf(stderr,"the shipped setting did not leave the frame out\n");
    ok=0;
  }

  /* However the frontend arranges its calls. Sized first or not, and at any granularity, because
   * neither is a statement about what the snapshot is for. */
  static const unsigned granularities[]={1u,2u,10u,60u,240u};
  for(size_t g=0;ok && g<sizeof granularities/sizeof *granularities;g++){
    complete_saves=resume_saves=0;
    for(unsigned snapshot=0;ok && snapshot<4u;snapshot++){
      for(unsigned f=0;f<granularities[g];f++) retro_run();
      if(snapshot&1u) (void)retro_serialize_size();
      if(!retro_serialize(state,capacity)) ok=0;
    }
    if(ok && (resume_saves!=4u || complete_saves!=0u)){
      fprintf(stderr,"at a granularity of %u the answer moved (%u of 4 carried the frame)\n",
              granularities[g],complete_saves);
      ok=0;
    }
  }

  /* Turned on, every snapshot carries it, just as consistently. */
  exact_frame_setting="On";
  complete_saves=resume_saves=0;
  for(unsigned snapshot=0;ok && snapshot<3u;snapshot++){
    retro_run();
    if(!retro_serialize(state,capacity)) ok=0;
  }
  if(ok && (complete_saves!=3u || resume_saves!=0u)){
    fprintf(stderr,"with the setting on, %u of 3 snapshots still left the frame out\n",resume_saves);
    ok=0;
  }

  free(state);
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

int main(void){
  uint8_t byte=0;
  memset(&g_libretro,0,sizeof g_libretro);
  if(retro_serialize_size()!=0 || retro_serialize(&byte,1) || retro_unserialize(&byte,1) ||
     !stable_transport(1) || !stable_transport(0) || !stable_transport(-1) ||
     !growing_transport(1) || !growing_transport(0) || !growing_transport(-1) ||
     !remembered_peak_covers_first_answer() ||
     !snapshot_scope_follows_the_setting() ||
     !restart_rereads_settings() || !starting_declares_the_settings()){
    fprintf(stderr,"libretro state transport contract failed\n");
    return 1;
  }
  puts("libretro state transport: ok");
  return 0;
}
