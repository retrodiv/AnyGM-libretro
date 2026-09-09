/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Audio builtin argument adaptation, spatial-emitter policy, and ordered dispatch. */
#include "gml_builtin_internal.h"
#include "gml_render.h"
#include "gml_audio.h"
#include "gml_pxtone.h"
#include "gml_tracker.h"
#include "gml_wwise.h"
#include "gml_hash.h"
#include "anygm_host.h"
#include "anygm_vfs.h"

#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  uint32_t id;
  uint32_t offset;
  uint32_t size;
  int sound;
} GmlWwiseMedia;

typedef struct {
  uint32_t id;
  uint8_t type;
  const uint8_t *body;
  uint32_t size;
} GmlWwiseObject;

typedef struct {
  char *path;
  uint8_t *data;
  size_t size;
  uint8_t content_sha256[32];
  const uint8_t *media_data;
  size_t media_data_size;
  GmlWwiseMedia *media;
  uint32_t media_count;
  GmlWwiseObject *objects;
  uint32_t object_count;
} GmlWwiseBank;

typedef struct {
  char base_path[768];
  GmlWwiseBank banks[16];
  uint32_t bank_count;
} GmlWwiseState;

static uint32_t wwise_u32(const uint8_t *bytes){
  return (uint32_t)bytes[0]|((uint32_t)bytes[1]<<8)|
         ((uint32_t)bytes[2]<<16)|((uint32_t)bytes[3]<<24);
}

static void wwise_bank_free(GmlWwiseBank *bank){
  if(!bank) return;
  free(bank->path);
  free(bank->media);
  free(bank->objects);
  free(bank->data);
  memset(bank,0,sizeof *bank);
}

void builtin_wwise_state_free(GmlBuiltinState *state){
  GmlWwiseState *wwise=state?(GmlWwiseState *)state->wwise:NULL;
  if(!wwise) return;
  for(uint32_t index=0;index<wwise->bank_count;index++)
    wwise_bank_free(&wwise->banks[index]);
  free(wwise);
  state->wwise=NULL;
}

static GmlWwiseState *wwise_state(GmlVM *vm){
  GmlBuiltinState *state=builtin_state_ensure(vm);
  if(!state) return NULL;
  if(!state->wwise) state->wwise=calloc(1,sizeof(GmlWwiseState));
  return (GmlWwiseState *)state->wwise;
}

static int wwise_bank_parse(GmlWwiseBank *bank){
  const uint8_t *didx=NULL,*hirc=NULL;
  size_t didx_size=0,hirc_size=0;
  for(size_t offset=0;offset+8u<=bank->size;){
    uint32_t chunk_size=wwise_u32(bank->data+offset+4u);
    if((size_t)chunk_size>bank->size-offset-8u) return 0;
    const uint8_t *chunk=bank->data+offset+8u;
    if(!memcmp(bank->data+offset,"DIDX",4)){ didx=chunk; didx_size=chunk_size; }
    else if(!memcmp(bank->data+offset,"DATA",4)){
      bank->media_data=chunk; bank->media_data_size=chunk_size;
    } else if(!memcmp(bank->data+offset,"HIRC",4)){ hirc=chunk; hirc_size=chunk_size; }
    offset+=8u+(size_t)chunk_size;
  }
  if(didx){
    if(didx_size%12u || didx_size/12u>65536u || !bank->media_data) return 0;
    bank->media_count=(uint32_t)(didx_size/12u);
    bank->media=calloc(bank->media_count?bank->media_count:1u,sizeof *bank->media);
    if(!bank->media) return 0;
    for(uint32_t index=0;index<bank->media_count;index++){
      const uint8_t *entry=didx+(size_t)index*12u;
      bank->media[index]=(GmlWwiseMedia){
        wwise_u32(entry),wwise_u32(entry+4),wwise_u32(entry+8),-1
      };
      if((uint64_t)bank->media[index].offset+bank->media[index].size>
         bank->media_data_size) return 0;
    }
  }
  if(hirc){
    if(hirc_size<4u) return 0;
    uint32_t count=wwise_u32(hirc);
    if(count>65536u) return 0;
    bank->objects=calloc(count?count:1u,sizeof *bank->objects);
    if(!bank->objects) return 0;
    size_t offset=4u;
    for(uint32_t index=0;index<count;index++){
      if(offset+9u>hirc_size) return 0;
      uint8_t type=hirc[offset];
      uint32_t size=wwise_u32(hirc+offset+1u);
      if(size<4u || (size_t)size>hirc_size-offset-5u) return 0;
      bank->objects[index]=(GmlWwiseObject){
        wwise_u32(hirc+offset+5u),type,hirc+offset+9u,size-4u
      };
      offset+=5u+(size_t)size;
    }
    bank->object_count=count;
  }
  return bank->media_count || bank->object_count;
}

static GmlWwiseObject *wwise_object(GmlWwiseState *state,uint32_t id){
  if(!state) return NULL;
  for(uint32_t bank=0;bank<state->bank_count;bank++)
    for(uint32_t index=0;index<state->banks[bank].object_count;index++)
      if(state->banks[bank].objects[index].id==id) return &state->banks[bank].objects[index];
  return NULL;
}

static GmlWwiseMedia *wwise_media(GmlWwiseState *state,uint32_t id,GmlWwiseBank **owner){
  if(owner) *owner=NULL;
  if(!state) return NULL;
  for(uint32_t bank=0;bank<state->bank_count;bank++)
    for(uint32_t index=0;index<state->banks[bank].media_count;index++)
      if(state->banks[bank].media[index].id==id){
        if(owner) *owner=&state->banks[bank];
        return &state->banks[bank].media[index];
      }
  return NULL;
}

static int wwise_play_media(GmlVM *vm,GmlWwiseState *state,uint32_t id){
  GmlWwiseBank *bank=NULL;
  GmlWwiseMedia *media=wwise_media(state,id,&bank);
  GmlAudio *audio=vm?(GmlAudio *)vm->audio:NULL;
  if(!media || !bank || !audio) return 0;
  if(media->sound<0){
    uint8_t *ogg=NULL; size_t ogg_size=0;
    if(!gml_wwise_wem_to_ogg(bank->media_data+media->offset,media->size,&ogg,&ogg_size))
      return 0;
    media->sound=gml_audio_add_encoded(audio,ogg,ogg_size);
    free(ogg);
    if(media->sound<0) return 0;
  }
  return gml_audio_play(audio,media->sound,0)>=0;
}

static int wwise_play_object(GmlVM *vm,GmlWwiseState *state,uint32_t id,
                             uint32_t *visited,int visited_count,int depth){
  if(depth>16 || visited_count>=64) return 0;
  for(int index=0;index<visited_count;index++) if(visited[index]==id) return 0;
  GmlWwiseObject *object=wwise_object(state,id);
  if(!object) return 0;
  visited[visited_count++]=id;
  if(object->type==2 && object->size>=9u)
    return wwise_play_media(vm,state,wwise_u32(object->body+5u));
  int played=0;
  for(uint32_t offset=0;offset+4u<=object->size;offset++){
    uint32_t child=wwise_u32(object->body+offset);
    if(!wwise_object(state,child)) continue;
    played+=wwise_play_object(vm,state,child,visited,visited_count,depth+1);
    if(played && (object->type==5 || object->type==6 ||
                  object->type==12 || object->type==13)) break;
  }
  return played;
}

static uint32_t wwise_string_id(const char *value){
  uint32_t hash=2166136261u;
  if(!value) return 0;
  for(;*value;value++) hash=hash*16777619u^(uint8_t)tolower((unsigned char)*value);
  return hash;
}

static uint32_t wwise_argument_id(GmlVM *vm,GmlVal *args,int count,int index){
  if(index<0 || index>=count) return 0;
  return args[index].t==V_STR?wwise_string_id(S(vm,args,count,index)):
    (uint32_t)(uint64_t)N(args,count,index);
}

static int wwise_load_bank(GmlVM *vm,const char *name){
  GmlWwiseState *state=wwise_state(vm);
  if(!state || !name || !name[0] || state->bank_count>=16u) return 0;
  char relative[1536];
  if(state->base_path[0])
    snprintf(relative,sizeof relative,"%s/%s",state->base_path,name);
  else snprintf(relative,sizeof relative,"%s",name);
  for(uint32_t index=0;index<state->bank_count;index++)
    if(state->banks[index].path && !strcmp(state->banks[index].path,relative)) return 1;
  char *path=resolve_read_path(vm,relative);
  uint8_t *data=NULL; size_t size=0;
  if(!path || !anygm_vfs_read_all(vm->host,path,&data,&size,256u*1024u*1024u)){
    free(path); free(data); return 0;
  }
  GmlWwiseBank bank={0};
  bank.path=strdup(relative); bank.data=data; bank.size=size;
  gml_sha256(data,size,bank.content_sha256);
  int ok=bank.path && wwise_bank_parse(&bank);
  free(path);
  if(!ok){ wwise_bank_free(&bank); return 0; }
  state->banks[state->bank_count++]=bank;
  return 1;
}

static int wwise_post_event(GmlVM *vm,uint32_t event_id){
  GmlWwiseState *state=wwise_state(vm);
  GmlWwiseObject *event=wwise_object(state,event_id);
  if(!event || event->type!=4 || event->size<4u) return 0;
  uint32_t count=wwise_u32(event->body);
  if(count>(event->size-4u)/4u) return 0;
  int played=0;
  for(uint32_t index=0;index<count;index++){
    GmlWwiseObject *action=wwise_object(state,wwise_u32(event->body+4u+index*4u));
    if(!action || action->type!=3 || action->size<6u) continue;
    uint8_t action_type=action->body[1];
    if(action_type==4){
      uint32_t visited[64]={0};
      played+=wwise_play_object(vm,state,wwise_u32(action->body+2u),visited,0,0);
    } else if(action_type==1 && vm && vm->audio){
      for(uint32_t bank=0;bank<state->bank_count;bank++)
        for(uint32_t media=0;media<state->banks[bank].media_count;media++)
          if(state->banks[bank].media[media].sound>=0)
            gml_audio_stop((GmlAudio *)vm->audio,state->banks[bank].media[media].sound);
    }
  }
  return played;
}

uint32_t builtin_wwise_bank_count(const GmlBuiltinState *state){
  const GmlWwiseState *wwise=state?(const GmlWwiseState *)state->wwise:NULL;
  return wwise?wwise->bank_count:0;
}

const char *builtin_wwise_base_path(const GmlBuiltinState *state){
  const GmlWwiseState *wwise=state?(const GmlWwiseState *)state->wwise:NULL;
  return wwise?wwise->base_path:"";
}

const char *builtin_wwise_bank_path(const GmlBuiltinState *state,uint32_t index){
  const GmlWwiseState *wwise=state?(const GmlWwiseState *)state->wwise:NULL;
  return wwise && index<wwise->bank_count && wwise->banks[index].path?
    wwise->banks[index].path:"";
}

const uint8_t *builtin_wwise_bank_digest(const GmlBuiltinState *state,uint32_t index){
  const GmlWwiseState *wwise=state?(const GmlWwiseState *)state->wwise:NULL;
  return wwise && index<wwise->bank_count?wwise->banks[index].content_sha256:NULL;
}

int builtin_wwise_state_restore(GmlBuiltinState *state,const char *base,
                                const char *const *paths,const uint8_t digests[][32],
                                uint32_t count){
  if(!state || !base || strlen(base)>=sizeof(((GmlWwiseState *)0)->base_path) ||
     count>16u) return 0;
  GmlWwiseState *wwise=wwise_state(state->vm);
  if(!wwise) return 0;
  wwise->base_path[0]=0;
  for(uint32_t index=0;index<count;index++)
    if(!paths || !paths[index] || !paths[index][0] || strlen(paths[index])>=1536u ||
       !digests || !wwise_load_bank(state->vm,paths[index]) ||
       memcmp(wwise->banks[index].content_sha256,digests[index],32u)) return 0;
  snprintf(wwise->base_path,sizeof wwise->base_path,"%s",base);
  return 1;
}
static double audio_emitter_attenuation(GmlVM *vm, int e, double distance){
  if(!vm || e<0 || e>=GML_MAX_EMITTERS || vm->builtins->audio_falloff_model==0) return 1.0;
  double ref=vm->builtins->emitter_ref[e], maxd=vm->builtins->emitter_max[e], factor=vm->builtins->emitter_factor[e];
  if(ref<=0.0) ref=1.0;
  if(maxd<ref) maxd=ref;
  if(factor<0.0) factor=0.0;
  int model=vm->builtins->audio_falloff_model, clamped=(model==2 || model==4 || model==6);
  double d=distance;
  if(clamped){ if(d<ref) d=ref; if(d>maxd) d=maxd; }
  double gain=1.0;
  if(model==1 || model==2) gain=ref/(ref+factor*(d-ref));
  else if(model==3 || model==4) gain=maxd>ref?1.0-factor*(d-ref)/(maxd-ref):1.0;
  else if(model==5 || model==6) gain=pow(d>0.0?d/ref:1.0,-factor);
  if(!isfinite(gain) || gain<0.0) gain=0.0;
  return gain;
}
static void audio_refresh_emitter(GmlVM *vm, GmlAudio *au, int e){
  if(!vm || !au || e<0 || e>=GML_MAX_EMITTERS || !vm->builtins->emitter_live[e]) return;
  double dx=vm->builtins->emitter_x[e]-vm->builtins->listener_x, dy=vm->builtins->emitter_y[e]-vm->builtins->listener_y;
  double dz=vm->builtins->emitter_z[e]-vm->builtins->listener_z, dist=sqrt(dx*dx+dy*dy+dz*dz);
  double rx=vm->builtins->listener_forward_y*vm->builtins->listener_up_z-vm->builtins->listener_forward_z*vm->builtins->listener_up_y;
  double ry=vm->builtins->listener_forward_z*vm->builtins->listener_up_x-vm->builtins->listener_forward_x*vm->builtins->listener_up_z;
  double rz=vm->builtins->listener_forward_x*vm->builtins->listener_up_y-vm->builtins->listener_forward_y*vm->builtins->listener_up_x;
  double rl=sqrt(rx*rx+ry*ry+rz*rz), pan=0.0;
  if(dist>0.0 && rl>0.0) pan=(dx*rx+dy*ry+dz*rz)/(dist*rl);
  if(pan<-1.0) pan=-1.0; else if(pan>1.0) pan=1.0;
  gml_audio_emitter_mix(au,e,vm->builtins->emitter_gain[e]*audio_emitter_attenuation(vm,e,dist),pan);
}
static void audio_refresh_emitters(GmlVM *vm, GmlAudio *au){
  if(!vm || !au) return;
  for(int e=0;e<GML_MAX_EMITTERS;e++) if(vm->builtins->emitter_live[e]) audio_refresh_emitter(vm,au,e);
}
static uint32_t external_audio_type(double value){
  if(!isfinite(value)) return 0;
  double wrapped=fmod(trunc(value),4294967296.0);
  if(wrapped<0.0) wrapped+=4294967296.0;
  return (uint32_t)wrapped;
}

enum {
  GML_EXTERNAL_AUDIO_HANDLE_BASE=0x4A100000,
  GML_EXTERNAL_AUDIO_INIT=1,
  GML_EXTERNAL_AUDIO_FREE,
  GML_EXTERNAL_AUDIO_SET_LISTENER_POSITION,
  GML_EXTERNAL_AUDIO_SET_LISTENER_DIRECTION,
  GML_EXTERNAL_AUDIO_SET_GROUP_VOLUME,
  GML_EXTERNAL_AUDIO_GET_GROUP_VOLUME,
  GML_EXTERNAL_AUDIO_CREATE_EMITTER,
  GML_EXTERNAL_AUDIO_DESTROY_EMITTER,
  GML_EXTERNAL_AUDIO_PLAY,
  GML_EXTERNAL_AUDIO_PAUSE,
  GML_EXTERNAL_AUDIO_STOP,
  GML_EXTERNAL_AUDIO_REWIND,
  GML_EXTERNAL_AUDIO_IS_PLAYING,
  GML_EXTERNAL_AUDIO_SET_VOLUME,
  GML_EXTERNAL_AUDIO_GET_VOLUME,
  GML_EXTERNAL_AUDIO_FADE_VOLUME,
  GML_EXTERNAL_AUDIO_SET_TYPE,
  GML_EXTERNAL_AUDIO_GET_TYPE,
  GML_EXTERNAL_AUDIO_SET_LOOPING,
  GML_EXTERNAL_AUDIO_SET_3D_MODE,
  GML_EXTERNAL_AUDIO_SET_DISTANCE_FACTOR,
  GML_EXTERNAL_AUDIO_SET_POSITION,
  GML_EXTERNAL_AUDIO_TRACK_PLAY,
  /* Sound-handle operations use the identifier returned by a load. */
  GML_EXTERNAL_AUDIO_SS_LOAD,
  GML_EXTERNAL_AUDIO_SS_PLAY,
  GML_EXTERNAL_AUDIO_SS_LOOP,
  GML_EXTERNAL_AUDIO_SS_RESUME,
  GML_EXTERNAL_AUDIO_SS_FREE_SOUND,
  GML_EXTERNAL_AUDIO_SS_SET_VOLUME,
  GML_EXTERNAL_AUDIO_SS_GET_VOLUME,
  GML_EXTERNAL_AUDIO_SS_SET_FREQ,
  GML_EXTERNAL_AUDIO_SS_GET_FREQ,
  GML_EXTERNAL_AUDIO_SS_SET_PAN,
  GML_EXTERNAL_AUDIO_SS_GET_PAN,
  GML_EXTERNAL_AUDIO_SS_SET_POSITION,
  GML_EXTERNAL_AUDIO_SS_GET_POSITION,
  GML_EXTERNAL_AUDIO_SS_IS_PAUSED,
  GML_EXTERNAL_AUDIO_SS_IS_LOOPING,
  GML_EXTERNAL_AUDIO_SS_GET_LENGTH,
  GML_EXTERNAL_AUDIO_SS_GET_BYTES_PER_SECOND,
  GML_EXTERNAL_AUDIO_SS_IS_HANDLE_VALID,
  /* Saudio uses caller-selected string IDs rather than numeric sound handles. */
  GML_EXTERNAL_AUDIO_SA_OPEN,
  GML_EXTERNAL_AUDIO_SA_PLAY,
  GML_EXTERNAL_AUDIO_SA_LOOP,
  GML_EXTERNAL_AUDIO_SA_STOP,
  GML_EXTERNAL_AUDIO_SA_STOP_ALL,
  GML_EXTERNAL_AUDIO_SA_PAUSE,
  GML_EXTERNAL_AUDIO_SA_RESUME,
  GML_EXTERNAL_AUDIO_SA_POSITION,
  GML_EXTERNAL_AUDIO_SA_LENGTH,
  GML_EXTERNAL_AUDIO_SA_SEEK,
  GML_EXTERNAL_AUDIO_SA_STATUS,
  GML_EXTERNAL_AUDIO_SA_CHANNELS,
  GML_EXTERNAL_AUDIO_SA_BYTES_PER_SECOND,
  GML_EXTERNAL_AUDIO_SA_CAN_PLAY,
  GML_EXTERNAL_AUDIO_SA_OPEN_RECORD,
  GML_EXTERNAL_AUDIO_SA_RECORD,
  GML_EXTERNAL_AUDIO_SA_SAVE_RECORD,
  GML_EXTERNAL_AUDIO_SA_CLOSE,
  GML_EXTERNAL_AUDIO_SA_CLOSE_ALL,
  /* BGM.dll: local sampled-file playback through AnyGM's portable mixer. */
  GML_EXTERNAL_AUDIO_BGM_INIT,
  GML_EXTERNAL_AUDIO_BGM_CLOSE,
  GML_EXTERNAL_AUDIO_BGM_LOAD,
  GML_EXTERNAL_AUDIO_BGM_UNLOAD_ID,
  GML_EXTERNAL_AUDIO_BGM_UNLOAD_NAME,
  GML_EXTERNAL_AUDIO_BGM_LOADED_ID,
  GML_EXTERNAL_AUDIO_BGM_LOADED_NAME,
  GML_EXTERNAL_AUDIO_BGM_PLAY_ID,
  GML_EXTERNAL_AUDIO_BGM_PLAY_NAME,
  GML_EXTERNAL_AUDIO_BGM_STOP_ID,
  GML_EXTERNAL_AUDIO_BGM_STOP_NAME,
  GML_EXTERNAL_AUDIO_BGM_PAUSE_ID,
  GML_EXTERNAL_AUDIO_BGM_PAUSE_NAME,
  GML_EXTERNAL_AUDIO_BGM_RESUME_ID,
  GML_EXTERNAL_AUDIO_BGM_RESUME_NAME,
  GML_EXTERNAL_AUDIO_BGM_PLAYING_ID,
  GML_EXTERNAL_AUDIO_BGM_PLAYING_NAME,
  GML_EXTERNAL_AUDIO_BGM_LENGTH_ID,
  GML_EXTERNAL_AUDIO_BGM_LENGTH_NAME,
  GML_EXTERNAL_AUDIO_BGM_POSITION_ID,
  GML_EXTERNAL_AUDIO_BGM_POSITION_NAME,
  GML_EXTERNAL_AUDIO_BGM_GET_ATTR_ID,
  GML_EXTERNAL_AUDIO_BGM_GET_ATTR_NAME,
  GML_EXTERNAL_AUDIO_BGM_SET_ATTR_ID,
  GML_EXTERNAL_AUDIO_BGM_SET_ATTR_NAME,
  GML_EXTERNAL_AUDIO_BGM_FADE_ID,
  GML_EXTERNAL_AUDIO_BGM_FADE_NAME,
  GML_EXTERNAL_AUDIO_BGM_ERROR,
  GML_EXTERNAL_AUDIO_BGM_NOOP,
  /* GMFMODSimple.dll: the subset with portable mixer semantics plus successful no-ops for
   * optional DSP/occlusion/spectrum controls unsupported by the software mixer. */
  GML_EXTERNAL_AUDIO_FMOD_INIT,
  GML_EXTERNAL_AUDIO_FMOD_FREE,
  GML_EXTERNAL_AUDIO_FMOD_SOUND_ADD,
  GML_EXTERNAL_AUDIO_FMOD_SOUND_GROUP,
  GML_EXTERNAL_AUDIO_FMOD_GROUP_VOLUME,
  GML_EXTERNAL_AUDIO_FMOD_SOUND_VOLUME,
  GML_EXTERNAL_AUDIO_FMOD_SOUND_PLAY,
  GML_EXTERNAL_AUDIO_FMOD_SOUND_LOOP,
  GML_EXTERNAL_AUDIO_FMOD_SOUND_FREE,
  GML_EXTERNAL_AUDIO_FMOD_GROUP_STOP,
  GML_EXTERNAL_AUDIO_FMOD_ALL_STOP,
  GML_EXTERNAL_AUDIO_FMOD_INSTANCE_STOP,
  GML_EXTERNAL_AUDIO_FMOD_INSTANCE_PLAYING,
  GML_EXTERNAL_AUDIO_FMOD_INSTANCE_PAUSED,
  GML_EXTERNAL_AUDIO_FMOD_INSTANCE_GET_PAUSED,
  GML_EXTERNAL_AUDIO_FMOD_INSTANCE_SOUND,
  GML_EXTERNAL_AUDIO_FMOD_INSTANCE_POSITION,
  GML_EXTERNAL_AUDIO_FMOD_INSTANCE_VOLUME,
  GML_EXTERNAL_AUDIO_FMOD_INSTANCE_GET_VOLUME,
  GML_EXTERNAL_AUDIO_FMOD_INSTANCE_PAN,
  GML_EXTERNAL_AUDIO_FMOD_INSTANCE_GET_PAN,
  GML_EXTERNAL_AUDIO_FMOD_INSTANCE_FREQUENCY,
  GML_EXTERNAL_AUDIO_FMOD_INSTANCE_GET_FREQUENCY,
  GML_EXTERNAL_AUDIO_FMOD_INSTANCE_GET_POSITION,
  GML_EXTERNAL_AUDIO_FMOD_INSTANCE_SOUND_LENGTH,
  GML_EXTERNAL_AUDIO_FMOD_GROUP_GET_VOLUME,
  GML_EXTERNAL_AUDIO_FMOD_MASTER_VOLUME,
  GML_EXTERNAL_AUDIO_FMOD_SOUND_LENGTH,
  GML_EXTERNAL_AUDIO_FMOD_SOUND_MAX_VOLUME,
  GML_EXTERNAL_AUDIO_FMOD_SOUND_READY,
  GML_EXTERNAL_AUDIO_FMOD_SOUND_PLAY_3D,
  GML_EXTERNAL_AUDIO_FMOD_SOUND_LOOP_3D,
  GML_EXTERNAL_AUDIO_FMOD_INSTANCE_POSITION_3D,
  GML_EXTERNAL_AUDIO_FMOD_LISTENER_POSITION_3D,
  GML_EXTERNAL_AUDIO_FMOD_UPDATE,
  GML_EXTERNAL_AUDIO_FMOD_LAST_ERROR,
  GML_EXTERNAL_AUDIO_FMOD_NOOP,
  GML_EXTERNAL_AUDIO_PXTONE_INIT,
  GML_EXTERNAL_AUDIO_PXTONE_LOAD,
  GML_EXTERNAL_AUDIO_PXTONE_PLAY,
  GML_EXTERNAL_AUDIO_PXTONE_VOLUME,
  GML_EXTERNAL_AUDIO_PXTONE_FADE,
  GML_EXTERNAL_AUDIO_PXTONE_LOOP,
  GML_EXTERNAL_AUDIO_PXTONE_STOP,
  GML_EXTERNAL_AUDIO_PXTONE_RELEASE,
  GML_EXTERNAL_AUDIO_PXTONE_SHUTDOWN,
  /* Tracker-music extension operations. Spectrum requests use the first band argument
   * and ignore an optional second argument. */
  GML_EXTERNAL_AUDIO_JB_INIT,
  GML_EXTERNAL_AUDIO_JB_CLOSE,
  GML_EXTERNAL_AUDIO_JB_LOAD_SONG,
  GML_EXTERNAL_AUDIO_JB_LOAD_SONG_PACK,
  GML_EXTERNAL_AUDIO_JB_PLAY_SONG,
  GML_EXTERNAL_AUDIO_JB_STOP_SONG,
  GML_EXTERNAL_AUDIO_JB_SET_LOOPING,
  GML_EXTERNAL_AUDIO_JB_SET_MASTER_VOLUME,
  GML_EXTERNAL_AUDIO_JB_GET_MASTER_VOLUME,
  GML_EXTERNAL_AUDIO_JB_SET_ORDER,
  GML_EXTERNAL_AUDIO_JB_SET_PAN_SEPARATION,
  GML_EXTERNAL_AUDIO_JB_SET_PAUSED,
  GML_EXTERNAL_AUDIO_JB_GET_PAUSED,
  GML_EXTERNAL_AUDIO_JB_IS_PLAYING,
  GML_EXTERNAL_AUDIO_JB_IS_FINISHED,
  GML_EXTERNAL_AUDIO_JB_GET_ORDER,
  GML_EXTERNAL_AUDIO_JB_GET_PATTERN,
  GML_EXTERNAL_AUDIO_JB_GET_PATTERN_LENGTH,
  GML_EXTERNAL_AUDIO_JB_GET_ROW,
  GML_EXTERNAL_AUDIO_JB_GET_SPEED,
  GML_EXTERNAL_AUDIO_JB_GET_BPM,
  GML_EXTERNAL_AUDIO_JB_GET_TIME,
  GML_EXTERNAL_AUDIO_JB_GET_TIME_FORMAT,
  GML_EXTERNAL_AUDIO_JB_GET_CURRENT_SONG,
  GML_EXTERNAL_AUDIO_JB_GET_SONG_EXISTS,
  GML_EXTERNAL_AUDIO_JB_GET_NAME,
  GML_EXTERNAL_AUDIO_JB_GET_TYPE,
  GML_EXTERNAL_AUDIO_JB_GET_NUM_CHANNELS,
  GML_EXTERNAL_AUDIO_JB_GET_NUM_INSTRUMENTS,
  GML_EXTERNAL_AUDIO_JB_GET_NUM_ORDERS,
  GML_EXTERNAL_AUDIO_JB_GET_NUM_PATTERNS,
  GML_EXTERNAL_AUDIO_JB_GET_NUM_SAMPLES,
  GML_EXTERNAL_AUDIO_JB_GET_CHANNELS_PLAYING,
  GML_EXTERNAL_AUDIO_JB_GET_CPU_USAGE,
  GML_EXTERNAL_AUDIO_JB_GET_FREQUENCY,
  GML_EXTERNAL_AUDIO_JB_GET_ERROR_CODE,
  GML_EXTERNAL_AUDIO_JB_GET_ERROR_MESSAGE,
  GML_EXTERNAL_AUDIO_JB_GET_JB_INFO,
  GML_EXTERNAL_AUDIO_JB_GET_PACK_INFO,
  GML_EXTERNAL_AUDIO_JB_GET_INSTRUMENT_PLAYED,
  GML_EXTERNAL_AUDIO_JB_GET_INSTRUMENT_PLAYED_ONCE,
  GML_EXTERNAL_AUDIO_JB_GET_ZXX,
  GML_EXTERNAL_AUDIO_JB_GET_ZXX_ONCE,
  GML_EXTERNAL_AUDIO_JB_GET_ZXX_PLAYED,
  GML_EXTERNAL_AUDIO_JB_GET_ZXX_PLAYED_ONCE,
  GML_EXTERNAL_AUDIO_JB_INIT_SPECTRUM,
  GML_EXTERNAL_AUDIO_JB_CLOSE_SPECTRUM,
  GML_EXTERNAL_AUDIO_JB_GET_SPECTRUM,
  GML_EXTERNAL_AUDIO_OPERATION_LIMIT
};
typedef struct {
  const char *symbol;
  int operation;
} GmlExternalAudioSymbol;
static const GmlExternalAudioSymbol external_audio_symbols[]={
  {"sga_Init",GML_EXTERNAL_AUDIO_INIT},
  {"sga_Free",GML_EXTERNAL_AUDIO_FREE},
  {"sga_SetListenerPosition",GML_EXTERNAL_AUDIO_SET_LISTENER_POSITION},
  {"sga_SetListenerDirection",GML_EXTERNAL_AUDIO_SET_LISTENER_DIRECTION},
  {"sga_SetGroupVolume",GML_EXTERNAL_AUDIO_SET_GROUP_VOLUME},
  {"sga_GetGroupVolume",GML_EXTERNAL_AUDIO_GET_GROUP_VOLUME},
  {"sga_CreateEmitter",GML_EXTERNAL_AUDIO_CREATE_EMITTER},
  {"sga_DestroyEmitter",GML_EXTERNAL_AUDIO_DESTROY_EMITTER},
  {"sga_Play",GML_EXTERNAL_AUDIO_PLAY},
  {"sga_Pause",GML_EXTERNAL_AUDIO_PAUSE},
  {"sga_Stop",GML_EXTERNAL_AUDIO_STOP},
  {"sga_Rewind",GML_EXTERNAL_AUDIO_REWIND},
  {"sga_IsPlaying",GML_EXTERNAL_AUDIO_IS_PLAYING},
  {"sga_SetVolume",GML_EXTERNAL_AUDIO_SET_VOLUME},
  {"sga_GetVolume",GML_EXTERNAL_AUDIO_GET_VOLUME},
  {"sga_FadeVolume",GML_EXTERNAL_AUDIO_FADE_VOLUME},
  {"sga_SetType",GML_EXTERNAL_AUDIO_SET_TYPE},
  {"sga_GetType",GML_EXTERNAL_AUDIO_GET_TYPE},
  {"sga_SetLooping",GML_EXTERNAL_AUDIO_SET_LOOPING},
  {"sga_Set3DMode",GML_EXTERNAL_AUDIO_SET_3D_MODE},
  {"sga_SetDistFactor",GML_EXTERNAL_AUDIO_SET_DISTANCE_FACTOR},
  {"sga_SetPosition",GML_EXTERNAL_AUDIO_SET_POSITION},
  {"sga_TrackPlay",GML_EXTERNAL_AUDIO_TRACK_PLAY},
};
/* External sound-handle API symbols accepted by the audio adapter. */
static const GmlExternalAudioSymbol external_supersound_symbols[]={
  {"SS_Init",GML_EXTERNAL_AUDIO_INIT},
  {"SS_Unload",GML_EXTERNAL_AUDIO_FREE},
  {"SS_LoadSound",GML_EXTERNAL_AUDIO_SS_LOAD},
  {"SS_PlaySound",GML_EXTERNAL_AUDIO_SS_PLAY},
  {"SS_LoopSound",GML_EXTERNAL_AUDIO_SS_LOOP},
  {"SS_StopSound",GML_EXTERNAL_AUDIO_STOP},
  {"SS_PauseSound",GML_EXTERNAL_AUDIO_PAUSE},
  {"SS_ResumeSound",GML_EXTERNAL_AUDIO_SS_RESUME},
  {"SS_FreeSound",GML_EXTERNAL_AUDIO_SS_FREE_SOUND},
  {"SS_IsSoundPlaying",GML_EXTERNAL_AUDIO_IS_PLAYING},
  {"SS_IsSoundPaused",GML_EXTERNAL_AUDIO_SS_IS_PAUSED},
  {"SS_IsSoundLooping",GML_EXTERNAL_AUDIO_SS_IS_LOOPING},
  {"SS_IsHandleValid",GML_EXTERNAL_AUDIO_SS_IS_HANDLE_VALID},
  {"SS_SetSoundVol",GML_EXTERNAL_AUDIO_SS_SET_VOLUME},
  {"SS_GetSoundVol",GML_EXTERNAL_AUDIO_SS_GET_VOLUME},
  {"SS_SetSoundFreq",GML_EXTERNAL_AUDIO_SS_SET_FREQ},
  {"SS_GetSoundFreq",GML_EXTERNAL_AUDIO_SS_GET_FREQ},
  {"SS_SetSoundPan",GML_EXTERNAL_AUDIO_SS_SET_PAN},
  {"SS_GetSoundPan",GML_EXTERNAL_AUDIO_SS_GET_PAN},
  {"SS_SetSoundPosition",GML_EXTERNAL_AUDIO_SS_SET_POSITION},
  {"SS_GetSoundPosition",GML_EXTERNAL_AUDIO_SS_GET_POSITION},
  {"SS_GetSoundLength",GML_EXTERNAL_AUDIO_SS_GET_LENGTH},
  {"SS_GetSoundBytesPerSecond",GML_EXTERNAL_AUDIO_SS_GET_BYTES_PER_SECOND},
};
static const GmlExternalAudioSymbol external_saudio_symbols[]={
  {"open",GML_EXTERNAL_AUDIO_SA_OPEN},
  {"play",GML_EXTERNAL_AUDIO_SA_PLAY},
  {"loop",GML_EXTERNAL_AUDIO_SA_LOOP},
  {"stop",GML_EXTERNAL_AUDIO_SA_STOP},
  {"stop_all",GML_EXTERNAL_AUDIO_SA_STOP_ALL},
  {"pause",GML_EXTERNAL_AUDIO_SA_PAUSE},
  {"resume",GML_EXTERNAL_AUDIO_SA_RESUME},
  {"position",GML_EXTERNAL_AUDIO_SA_POSITION},
  {"length",GML_EXTERNAL_AUDIO_SA_LENGTH},
  {"seek",GML_EXTERNAL_AUDIO_SA_SEEK},
  {"status",GML_EXTERNAL_AUDIO_SA_STATUS},
  {"channels",GML_EXTERNAL_AUDIO_SA_CHANNELS},
  {"bytespersec",GML_EXTERNAL_AUDIO_SA_BYTES_PER_SECOND},
  {"canplay",GML_EXTERNAL_AUDIO_SA_CAN_PLAY},
  {"open_record",GML_EXTERNAL_AUDIO_SA_OPEN_RECORD},
  {"record",GML_EXTERNAL_AUDIO_SA_RECORD},
  {"save_record",GML_EXTERNAL_AUDIO_SA_SAVE_RECORD},
  {"close",GML_EXTERNAL_AUDIO_SA_CLOSE},
  {"close_all",GML_EXTERNAL_AUDIO_SA_CLOSE_ALL},
};
static const GmlExternalAudioSymbol external_bgm_symbols[]={
  {"bgm_Init",GML_EXTERNAL_AUDIO_BGM_INIT},
  {"bgm_Close",GML_EXTERNAL_AUDIO_BGM_CLOSE},
  {"bgm_Load",GML_EXTERNAL_AUDIO_BGM_LOAD},
  {"bgm_LoadMod",GML_EXTERNAL_AUDIO_BGM_LOAD},
  {"bgm_LoadSample",GML_EXTERNAL_AUDIO_BGM_LOAD},
  {"bgm_LoadStream",GML_EXTERNAL_AUDIO_BGM_LOAD},
  {"bgm_LoadNetStream",GML_EXTERNAL_AUDIO_BGM_LOAD},
  {"bgm_UnloadById",GML_EXTERNAL_AUDIO_BGM_UNLOAD_ID},
  {"bgm_UnloadByFname",GML_EXTERNAL_AUDIO_BGM_UNLOAD_NAME},
  {"bgm_IsLoadedById",GML_EXTERNAL_AUDIO_BGM_LOADED_ID},
  {"bgm_IsLoadedByFname",GML_EXTERNAL_AUDIO_BGM_LOADED_NAME},
  {"bgm_PlayById",GML_EXTERNAL_AUDIO_BGM_PLAY_ID},
  {"bgm_PlayByFname",GML_EXTERNAL_AUDIO_BGM_PLAY_NAME},
  {"bgm_StopById",GML_EXTERNAL_AUDIO_BGM_STOP_ID},
  {"bgm_StopByFname",GML_EXTERNAL_AUDIO_BGM_STOP_NAME},
  {"bgm_PauseById",GML_EXTERNAL_AUDIO_BGM_PAUSE_ID},
  {"bgm_PauseByFname",GML_EXTERNAL_AUDIO_BGM_PAUSE_NAME},
  {"bgm_UnpauseById",GML_EXTERNAL_AUDIO_BGM_RESUME_ID},
  {"bgm_UnpauseByFname",GML_EXTERNAL_AUDIO_BGM_RESUME_NAME},
  {"bgm_IsPlayingById",GML_EXTERNAL_AUDIO_BGM_PLAYING_ID},
  {"bgm_IsPlayingByFname",GML_EXTERNAL_AUDIO_BGM_PLAYING_NAME},
  {"bgm_GetLenById",GML_EXTERNAL_AUDIO_BGM_LENGTH_ID},
  {"bgm_GetLenByFname",GML_EXTERNAL_AUDIO_BGM_LENGTH_NAME},
  {"bgm_GetPosById",GML_EXTERNAL_AUDIO_BGM_POSITION_ID},
  {"bgm_GetPosByFname",GML_EXTERNAL_AUDIO_BGM_POSITION_NAME},
  {"bgm_GetAttrById",GML_EXTERNAL_AUDIO_BGM_GET_ATTR_ID},
  {"bgm_GetAttrByFname",GML_EXTERNAL_AUDIO_BGM_GET_ATTR_NAME},
  {"bgm_SetAttrById",GML_EXTERNAL_AUDIO_BGM_SET_ATTR_ID},
  {"bgm_SetAttrByFname",GML_EXTERNAL_AUDIO_BGM_SET_ATTR_NAME},
  {"bgm_FadeVolById",GML_EXTERNAL_AUDIO_BGM_FADE_ID},
  {"bgm_FadeVolByFname",GML_EXTERNAL_AUDIO_BGM_FADE_NAME},
  {"bgm_Error",GML_EXTERNAL_AUDIO_BGM_ERROR},
};
static const GmlExternalAudioSymbol external_fmod_symbols[]={
  {"FMODinit",GML_EXTERNAL_AUDIO_FMOD_INIT},
  {"FMODfree",GML_EXTERNAL_AUDIO_FMOD_FREE},
  {"FMODSoundAdd",GML_EXTERNAL_AUDIO_FMOD_SOUND_ADD},
  {"FMODSoundSetGroup",GML_EXTERNAL_AUDIO_FMOD_SOUND_GROUP},
  {"FMODGroupSetVolume",GML_EXTERNAL_AUDIO_FMOD_GROUP_VOLUME},
  {"FMODSoundSetMaxVolume",GML_EXTERNAL_AUDIO_FMOD_SOUND_VOLUME},
  {"FMODSoundPlay",GML_EXTERNAL_AUDIO_FMOD_SOUND_PLAY},
  {"FMODSoundLoop",GML_EXTERNAL_AUDIO_FMOD_SOUND_LOOP},
  {"FMODSoundFree",GML_EXTERNAL_AUDIO_FMOD_SOUND_FREE},
  {"FMODGroupStop",GML_EXTERNAL_AUDIO_FMOD_GROUP_STOP},
  {"FMODAllStop",GML_EXTERNAL_AUDIO_FMOD_ALL_STOP},
  {"FMODInstanceStop",GML_EXTERNAL_AUDIO_FMOD_INSTANCE_STOP},
  {"FMODInstanceIsPlaying",GML_EXTERNAL_AUDIO_FMOD_INSTANCE_PLAYING},
  {"FMODInstanceSetPaused",GML_EXTERNAL_AUDIO_FMOD_INSTANCE_PAUSED},
  {"FMODInstanceGetPaused",GML_EXTERNAL_AUDIO_FMOD_INSTANCE_GET_PAUSED},
  {"FMODInstanceGetSound",GML_EXTERNAL_AUDIO_FMOD_INSTANCE_SOUND},
  {"FMODInstanceSetPosition",GML_EXTERNAL_AUDIO_FMOD_INSTANCE_POSITION},
  {"FMODInstanceSetVolume",GML_EXTERNAL_AUDIO_FMOD_INSTANCE_VOLUME},
  {"FMODInstanceGetVolume",GML_EXTERNAL_AUDIO_FMOD_INSTANCE_GET_VOLUME},
  {"FMODInstanceSetPan",GML_EXTERNAL_AUDIO_FMOD_INSTANCE_PAN},
  {"FMODInstanceGetPan",GML_EXTERNAL_AUDIO_FMOD_INSTANCE_GET_PAN},
  {"FMODInstanceSetFrequency",GML_EXTERNAL_AUDIO_FMOD_INSTANCE_FREQUENCY},
  {"FMODInstanceGetFrequency",GML_EXTERNAL_AUDIO_FMOD_INSTANCE_GET_FREQUENCY},
  {"FMODInstanceGetPosition",GML_EXTERNAL_AUDIO_FMOD_INSTANCE_GET_POSITION},
  {"FMODInstanceSoundGetLength",GML_EXTERNAL_AUDIO_FMOD_INSTANCE_SOUND_LENGTH},
  {"FMODGroupGetVolume",GML_EXTERNAL_AUDIO_FMOD_GROUP_GET_VOLUME},
  {"FMODMasterSetVolume",GML_EXTERNAL_AUDIO_FMOD_MASTER_VOLUME},
  {"FMODSoundGetLength",GML_EXTERNAL_AUDIO_FMOD_SOUND_LENGTH},
  {"FMODSoundGetMaxVolume",GML_EXTERNAL_AUDIO_FMOD_SOUND_MAX_VOLUME},
  {"FMODSoundAsyncReady",GML_EXTERNAL_AUDIO_FMOD_SOUND_READY},
  {"FMODInstanceAsyncOK",GML_EXTERNAL_AUDIO_FMOD_SOUND_READY},
  {"FMODSoundPlay3d",GML_EXTERNAL_AUDIO_FMOD_SOUND_PLAY_3D},
  {"FMODSoundLoop3d",GML_EXTERNAL_AUDIO_FMOD_SOUND_LOOP_3D},
  {"FMODInstanceSet3dPosition",GML_EXTERNAL_AUDIO_FMOD_INSTANCE_POSITION_3D},
  {"FMODListenerSet3dPosition",GML_EXTERNAL_AUDIO_FMOD_LISTENER_POSITION_3D},
  {"FMODUpdate",GML_EXTERNAL_AUDIO_FMOD_UPDATE},
  {"FMODGetLastError",GML_EXTERNAL_AUDIO_FMOD_LAST_ERROR},
};
static const GmlExternalAudioSymbol external_pxtone_symbols[]={
  {"pxtone_init",GML_EXTERNAL_AUDIO_PXTONE_INIT},
  {"pxtone_load",GML_EXTERNAL_AUDIO_PXTONE_LOAD},
  {"pxtone_play",GML_EXTERNAL_AUDIO_PXTONE_PLAY},
  {"pxtone_volume",GML_EXTERNAL_AUDIO_PXTONE_VOLUME},
  {"pxtone_fadeout",GML_EXTERNAL_AUDIO_PXTONE_FADE},
  {"pxtone_setloop",GML_EXTERNAL_AUDIO_PXTONE_LOOP},
  {"pxtone_stop",GML_EXTERNAL_AUDIO_PXTONE_STOP},
  {"pxtone_release",GML_EXTERNAL_AUDIO_PXTONE_RELEASE},
  {"pxtone_shutdown",GML_EXTERNAL_AUDIO_PXTONE_SHUTDOWN},
};
static int external_ascii_equal(const char *left,const char *right){
  if(!left || !right) return 0;
  while(*left && *right){
    if(tolower((unsigned char)*left)!=tolower((unsigned char)*right)) return 0;
    left++;
    right++;
  }
  return *left==0 && *right==0;
}
/* jbfmod.dll extension names. Both spectrum wrappers map to the same one-band operation. */
static const GmlExternalAudioSymbol external_jbfmod_symbols[]={
  {"Init",GML_EXTERNAL_AUDIO_JB_INIT},
  {"Close",GML_EXTERNAL_AUDIO_JB_CLOSE},
  {"LoadSong",GML_EXTERNAL_AUDIO_JB_LOAD_SONG},
  {"LoadSongPack",GML_EXTERNAL_AUDIO_JB_LOAD_SONG_PACK},
  {"PlaySong",GML_EXTERNAL_AUDIO_JB_PLAY_SONG},
  {"StopSong",GML_EXTERNAL_AUDIO_JB_STOP_SONG},
  {"SetLooping",GML_EXTERNAL_AUDIO_JB_SET_LOOPING},
  {"SetMasterVolume",GML_EXTERNAL_AUDIO_JB_SET_MASTER_VOLUME},
  {"GetMasterVolume",GML_EXTERNAL_AUDIO_JB_GET_MASTER_VOLUME},
  {"SetOrder",GML_EXTERNAL_AUDIO_JB_SET_ORDER},
  {"SetPanSeperation",GML_EXTERNAL_AUDIO_JB_SET_PAN_SEPARATION},
  {"SetPaused",GML_EXTERNAL_AUDIO_JB_SET_PAUSED},
  {"GetPaused",GML_EXTERNAL_AUDIO_JB_GET_PAUSED},
  {"IsPlaying",GML_EXTERNAL_AUDIO_JB_IS_PLAYING},
  {"IsFinished",GML_EXTERNAL_AUDIO_JB_IS_FINISHED},
  {"GetOrder",GML_EXTERNAL_AUDIO_JB_GET_ORDER},
  {"GetPattern",GML_EXTERNAL_AUDIO_JB_GET_PATTERN},
  {"GetPatternLength",GML_EXTERNAL_AUDIO_JB_GET_PATTERN_LENGTH},
  {"GetRow",GML_EXTERNAL_AUDIO_JB_GET_ROW},
  {"GetSpeed",GML_EXTERNAL_AUDIO_JB_GET_SPEED},
  {"GetBPM",GML_EXTERNAL_AUDIO_JB_GET_BPM},
  {"GetTime",GML_EXTERNAL_AUDIO_JB_GET_TIME},
  {"GetTimeFormat",GML_EXTERNAL_AUDIO_JB_GET_TIME_FORMAT},
  {"GetCurrentSong",GML_EXTERNAL_AUDIO_JB_GET_CURRENT_SONG},
  {"GetSongExists",GML_EXTERNAL_AUDIO_JB_GET_SONG_EXISTS},
  {"GetName",GML_EXTERNAL_AUDIO_JB_GET_NAME},
  {"GetType",GML_EXTERNAL_AUDIO_JB_GET_TYPE},
  {"GetNumChannels",GML_EXTERNAL_AUDIO_JB_GET_NUM_CHANNELS},
  {"GetNumInstruments",GML_EXTERNAL_AUDIO_JB_GET_NUM_INSTRUMENTS},
  {"GetNumOrders",GML_EXTERNAL_AUDIO_JB_GET_NUM_ORDERS},
  {"GetNumPatterns",GML_EXTERNAL_AUDIO_JB_GET_NUM_PATTERNS},
  {"GetNumSamples",GML_EXTERNAL_AUDIO_JB_GET_NUM_SAMPLES},
  {"GetChannelsPlaying",GML_EXTERNAL_AUDIO_JB_GET_CHANNELS_PLAYING},
  {"GetCPUUsage",GML_EXTERNAL_AUDIO_JB_GET_CPU_USAGE},
  {"GetFrequency",GML_EXTERNAL_AUDIO_JB_GET_FREQUENCY},
  {"GetErrorCode",GML_EXTERNAL_AUDIO_JB_GET_ERROR_CODE},
  {"GetErrorMessage",GML_EXTERNAL_AUDIO_JB_GET_ERROR_MESSAGE},
  {"GetJBInfo",GML_EXTERNAL_AUDIO_JB_GET_JB_INFO},
  {"GetPackInfo",GML_EXTERNAL_AUDIO_JB_GET_PACK_INFO},
  {"GetInstrumentPlayed",GML_EXTERNAL_AUDIO_JB_GET_INSTRUMENT_PLAYED},
  {"GetInstrumentPlayedOnce",GML_EXTERNAL_AUDIO_JB_GET_INSTRUMENT_PLAYED_ONCE},
  {"GetZxx",GML_EXTERNAL_AUDIO_JB_GET_ZXX},
  {"GetZxxOnce",GML_EXTERNAL_AUDIO_JB_GET_ZXX_ONCE},
  {"GetZxxPlayed",GML_EXTERNAL_AUDIO_JB_GET_ZXX_PLAYED},
  {"GetZxxPlayedOnce",GML_EXTERNAL_AUDIO_JB_GET_ZXX_PLAYED_ONCE},
  {"InitSpectrum",GML_EXTERNAL_AUDIO_JB_INIT_SPECTRUM},
  {"CloseSpectrum",GML_EXTERNAL_AUDIO_JB_CLOSE_SPECTRUM},
  {"GetSpectrum",GML_EXTERNAL_AUDIO_JB_GET_SPECTRUM},
};

int builtin_external_audio_define(const char *library,const char *symbol){
  if(!library || !symbol) return 0;
  const char *base=library;
  for(const char *cursor=library;*cursor;cursor++)
    if(*cursor=='/' || *cursor=='\\') base=cursor+1;
  const GmlExternalAudioSymbol *table=NULL;
  size_t entries=0;
  if(external_ascii_equal(base,"SGAudio.dll")){
    table=external_audio_symbols;
    entries=sizeof(external_audio_symbols)/sizeof(external_audio_symbols[0]);
  } else if(external_ascii_equal(base,"supersound.dll")){
    table=external_supersound_symbols;
    entries=sizeof(external_supersound_symbols)/sizeof(external_supersound_symbols[0]);
  } else if(external_ascii_equal(base,"saudio.dll")){
    table=external_saudio_symbols;
    entries=sizeof(external_saudio_symbols)/sizeof(external_saudio_symbols[0]);
  } else if(external_ascii_equal(base,"bgm.dll")){
    table=external_bgm_symbols;
    entries=sizeof(external_bgm_symbols)/sizeof(external_bgm_symbols[0]);
  } else if(external_ascii_equal(base,"GMFMODSimple.dll")){
    table=external_fmod_symbols;
    entries=sizeof(external_fmod_symbols)/sizeof(external_fmod_symbols[0]);
  } else if(external_ascii_equal(base,"pxwrap.dll")){
    table=external_pxtone_symbols;
    entries=sizeof(external_pxtone_symbols)/sizeof(external_pxtone_symbols[0]);
  } else if(external_ascii_equal(base,"jbfmod.dll")){
    table=external_jbfmod_symbols;
    entries=sizeof(external_jbfmod_symbols)/sizeof(external_jbfmod_symbols[0]);
  }
  for(size_t index=0;table && index<entries;index++)
    if(!strcmp(symbol,table[index].symbol))
      return GML_EXTERNAL_AUDIO_HANDLE_BASE+table[index].operation;
  if(external_ascii_equal(base,"bgm.dll") && !strncmp(symbol,"bgm_",4))
    return GML_EXTERNAL_AUDIO_HANDLE_BASE+GML_EXTERNAL_AUDIO_BGM_NOOP;
  if(external_ascii_equal(base,"GMFMODSimple.dll") && !strncmp(symbol,"FMOD",4))
    return GML_EXTERNAL_AUDIO_HANDLE_BASE+GML_EXTERNAL_AUDIO_FMOD_NOOP;
  return 0;
}

static void external_audio_free(GmlVM *vm,int sound){
  GmlBuiltinState *state=vm?builtin_state_ensure(vm):NULL;
  builtin_state_external_audio_remove(state,sound);
  gml_audio_caster_free(vm?(GmlAudio*)vm->audio:NULL,sound);
}

static void external_audio_free_all(GmlVM *vm){
  GmlBuiltinState *state=vm?builtin_state_ensure(vm):NULL;
  /* The legacy unload/free-all entry points share one dynamic-sound pool. Keeping API-level
   * records after that pool is released would leave valid-looking IDs pointing at recycled
   * handles, so clear every view of the pool together. */
  builtin_state_saudio_clear(state);
  builtin_state_external_audio_clear(state);
  gml_audio_caster_free_all(vm?(GmlAudio*)vm->audio:NULL);
}

static int external_audio_load_hashed_mode(GmlVM *vm,const char *relative,
                                           uint8_t digest[32],int eager_ogg){
  if(digest) memset(digest,0,32);
  if(!vm || !relative || !*relative || strlen(relative)>4096u) return -1;
  char *path=resolve_read_path(vm,relative);
  uint8_t *encoded=NULL;
  size_t size=0;
  int handle=-1;
  if(path && anygm_vfs_read_all(vm->host,path,&encoded,&size,64u*1024u*1024u) &&
     size>0 && size<=INT_MAX)
    handle=eager_ogg
      ? gml_audio_add_ogg((GmlAudio*)vm->audio,encoded,(int)size)
      : gml_audio_add_encoded((GmlAudio*)vm->audio,encoded,(int)size);
  uint8_t identity[32];
  if(handle>=0 &&
     (!gml_audio_sound_content_hash((GmlAudio*)vm->audio,handle,identity) ||
      !builtin_state_external_audio_store(
        builtin_state_ensure(vm),handle,relative,identity))){
    external_audio_free(vm,handle);
    handle=-1;
  }
  if(handle>=0 && digest) memcpy(digest,identity,sizeof(identity));
  if(builtin_setting(vm,"GML_LOG_AUDIO"))
    anygm_host_logf(vm->host,ANYGM_LOG_DEBUG,
                    "[external-audio] load %s handle=%d\n",
                    path?path:"",handle);
  free(encoded);
  free(path);
  return handle;
}

static int external_audio_load_hashed(GmlVM *vm,const char *relative,uint8_t digest[32]){
  return external_audio_load_hashed_mode(vm,relative,digest,0);
}

static int external_audio_load(GmlVM *vm,const char *relative){
  return external_audio_load_hashed(vm,relative,NULL);
}

static int external_audio_pxtone_path(const char *path){
  if(!path) return 0;
  size_t size=strlen(path);
  return (size>=6u && external_ascii_equal(path+size-6u,".ptcop")) ||
    (size>=7u && external_ascii_equal(path+size-7u,".pttune"));
}

static int external_pxtone_current(GmlVM *vm){
  GmlBuiltinState *state=builtin_state_ensure(vm);
  if(!state) return -1;
  for(uint32_t i=0;i<state->external_audio_asset_count;i++)
    if(external_audio_pxtone_path(state->external_audio_assets[i].path))
      return state->external_audio_assets[i].sound;
  return -1;
}

static int external_pxtone_load(GmlVM *vm,const char *relative){
  if(!vm || !relative || !*relative || strlen(relative)>4096u) return -1;
  int previous=external_pxtone_current(vm);
  if(previous>=0) external_audio_free(vm,previous);
  char *path=resolve_read_path(vm,relative);
  uint8_t *encoded=NULL; size_t size=0;
  int16_t *pcm=NULL; uint32_t frames=0,loop_start=0;
  int sound=-1;
  if(path && anygm_vfs_read_all(vm->host,path,&encoded,&size,64u*1024u*1024u) && size &&
     gml_pxtone_render(encoded,size,&pcm,&frames,&loop_start)){
    sound=gml_audio_add_pcm16((GmlAudio*)vm->audio,pcm,frames,2,44100,encoded,size);
    if(sound>=0){
      uint8_t identity[32];
      if(!gml_audio_sound_content_hash((GmlAudio*)vm->audio,sound,identity) ||
         !builtin_state_external_audio_store(
           builtin_state_ensure(vm),sound,relative,identity)){
        external_audio_free(vm,sound); sound=-1;
      } else gml_audio_sound_loop_start((GmlAudio*)vm->audio,sound,
                                        (double)loop_start/44100.0);
    }
  }
  free(pcm); free(encoded); free(path);
  return sound;
}

static int external_tracker_path(const char *path);

int builtin_external_audio_restore(GmlVM *vm,const char *relative,int sound,
                                   const uint8_t expected_sha256[32]){
  if(!vm || !vm->audio || !relative || !relative[0] || strlen(relative)>4096u ||
     !expected_sha256) return 0;
  uint8_t current[32];
  if(gml_audio_sound_content_hash((GmlAudio*)vm->audio,sound,current) &&
     !memcmp(current,expected_sha256,sizeof(current))) return 1;
  char *path=resolve_read_path(vm,relative);
  uint8_t *encoded=NULL;
  size_t size=0;
  int ok=path && anygm_vfs_read_all(vm->host,path,&encoded,&size,64u*1024u*1024u) &&
    size>0 && size<=INT_MAX &&
    gml_audio_restore_encoded((GmlAudio*)vm->audio,sound,encoded,(int)size,
                              expected_sha256);
  if(!ok && encoded && size && external_audio_pxtone_path(relative)){
    int16_t *pcm=NULL; uint32_t frames=0,loop_start=0;
    if(gml_pxtone_render(encoded,size,&pcm,&frames,&loop_start)){
      ok=gml_audio_restore_pcm16((GmlAudio*)vm->audio,sound,pcm,frames,2,44100,
                                 encoded,size,expected_sha256);
      if(ok) gml_audio_sound_loop_start((GmlAudio*)vm->audio,sound,
                                        (double)loop_start/44100.0);
    }
    free(pcm);
  }
  if(!ok && encoded && size && external_tracker_path(relative)){
    /* A tracker module restores the way it loaded: re-render the source bytes the state named.
     * The render is deterministic, so the voice resumes over identical samples. */
    GmlTrackerModule *module=gml_tracker_load(encoded,size,NULL,0);
    if(module){
      int16_t *pcm=NULL; uint32_t frames=0,loop_start=0;
      if(gml_tracker_render(module,44100,128,&pcm,&frames,&loop_start)){
        ok=gml_audio_restore_pcm16((GmlAudio*)vm->audio,sound,pcm,frames,2,44100,
                                   encoded,size,expected_sha256);
        if(ok) gml_audio_sound_loop_start((GmlAudio*)vm->audio,sound,
                                          (double)loop_start/44100.0);
      }
      free(pcm);
      gml_tracker_free(module);
    }
  }
  free(encoded);
  free(path);
  return ok;
}

static GmlVal external_saudio_number_string(GmlVM *vm,double value){
  GmlBuiltinState *state=builtin_state_ensure(vm);
  if(!state || !isfinite(value) || value<0.0) return vstr("0");
  char *buffer=state->string_ring[state->string_ring_index++&7u];
  snprintf(buffer,sizeof(state->string_ring[0]),"%.0f",value);
  return vstr(buffer);
}

static int external_saudio_close(GmlVM *vm,const char *id){
  GmlBuiltinState *state=builtin_state_ensure(vm);
  GmlSaudioEntry *entry=builtin_state_saudio_find(state,id);
  if(!entry) return 0;
  if(entry->sound>=0)
    external_audio_free(vm,entry->sound);
  builtin_state_saudio_remove(state,id);
  return 1;
}

static void external_saudio_close_all(GmlVM *vm){
  GmlBuiltinState *state=builtin_state_ensure(vm);
  if(!state) return;
  for(uint32_t i=0;i<state->saudio_count;i++)
    if(state->saudio_entries[i].sound>=0)
      external_audio_free(vm,state->saudio_entries[i].sound);
  builtin_state_saudio_clear(state);
}

static GmlVal external_saudio_call(GmlVM *vm,int operation,
                                   GmlVal *args,int count){
  GmlBuiltinState *state=builtin_state_ensure(vm);
  GmlAudio *audio=vm?(GmlAudio*)vm->audio:NULL;
  if(!state || !audio) return vreal(1);
  if(operation==GML_EXTERNAL_AUDIO_SA_OPEN){
    const char *path=S(vm,args,count,0);
    const char *id=S(vm,args,count,1);
    if(!id[0] || strlen(id)>4096u || !path[0] || strlen(path)>4096u) return vreal(1);
    (void)external_saudio_close(vm,id);
    uint8_t digest[32];
    int sound=external_audio_load_hashed(vm,path,digest);
    if(sound<0) return vreal(1);
    if(!builtin_state_saudio_store(state,id,path,digest,sound,0)){
      external_audio_free(vm,sound);
      return vreal(1);
    }
    return vreal(0);
  }
  if(operation==GML_EXTERNAL_AUDIO_SA_STOP_ALL){
    for(uint32_t i=0;i<state->saudio_count;i++){
      GmlSaudioEntry *entry=&state->saudio_entries[i];
      if(entry->sound<0) continue;
      entry->position_ms=gml_audio_sound_get_track_position(audio,entry->sound)*1000.0;
      gml_audio_stop(audio,entry->sound);
    }
    return vreal(0);
  }
  if(operation==GML_EXTERNAL_AUDIO_SA_CLOSE_ALL){
    external_saudio_close_all(vm);
    return vreal(0);
  }
  const char *id=operation==GML_EXTERNAL_AUDIO_SA_SEEK
    ? S(vm,args,count,1) : S(vm,args,count,0);
  GmlSaudioEntry *entry=builtin_state_saudio_find(state,id);
  if(operation==GML_EXTERNAL_AUDIO_SA_STATUS){
    if(!entry) return vstr("");
    if(entry->recording) return vstr(entry->record_active?"recording":"stopped");
    if(!gml_audio_is_playing(audio,entry->sound)) return vstr("stopped");
    return vstr(gml_audio_voice_paused(audio,entry->sound)?"paused":"playing");
  }
  if(operation==GML_EXTERNAL_AUDIO_SA_CAN_PLAY)
    return vstr(entry && !entry->recording && gml_audio_exists(audio,entry->sound)
      ? "true" : "false");
  if(operation==GML_EXTERNAL_AUDIO_SA_POSITION){
    double position=entry?entry->position_ms:0.0;
    if(entry && !entry->recording && gml_audio_is_playing(audio,entry->sound)){
      position=gml_audio_sound_get_track_position(audio,entry->sound)*1000.0;
      entry->position_ms=position;
    }
    return external_saudio_number_string(vm,position);
  }
  if(operation==GML_EXTERNAL_AUDIO_SA_LENGTH)
    return external_saudio_number_string(vm,
      entry && !entry->recording?gml_audio_sound_length(audio,entry->sound)*1000.0:0.0);
  if(operation==GML_EXTERNAL_AUDIO_SA_CHANNELS ||
     operation==GML_EXTERNAL_AUDIO_SA_BYTES_PER_SECOND){
    int channels=0,sample_rate=0,bytes_per_second=0;
    if(entry && !entry->recording)
      (void)gml_audio_sound_format(audio,entry->sound,&channels,&sample_rate,
                                   &bytes_per_second);
    (void)sample_rate;
    return external_saudio_number_string(vm,
      operation==GML_EXTERNAL_AUDIO_SA_CHANNELS?channels:bytes_per_second);
  }
  if(operation==GML_EXTERNAL_AUDIO_SA_OPEN_RECORD ||
     operation==GML_EXTERNAL_AUDIO_SA_RECORD ||
     operation==GML_EXTERNAL_AUDIO_SA_SAVE_RECORD){
    /* Libretro exposes no microphone callback. Return MCI's nonzero error convention instead of
     * fabricating a recording; ordinary playback remains entirely portable. */
    return vreal(1);
  }
  if(!entry || entry->recording || entry->sound<0) return vreal(1);
  if(operation==GML_EXTERNAL_AUDIO_SA_PLAY || operation==GML_EXTERNAL_AUDIO_SA_LOOP){
    int loop=operation==GML_EXTERNAL_AUDIO_SA_LOOP;
    gml_audio_stop(audio,entry->sound);
    entry->position_ms=0.0;
    gml_audio_sound_set_default_loop(audio,entry->sound,loop);
    return vreal(gml_audio_play(audio,entry->sound,loop)>=0?0:1);
  }
  if(operation==GML_EXTERNAL_AUDIO_SA_STOP){
    entry->position_ms=gml_audio_sound_get_track_position(audio,entry->sound)*1000.0;
    gml_audio_stop(audio,entry->sound);
    return vreal(0);
  }
  if(operation==GML_EXTERNAL_AUDIO_SA_PAUSE){
    entry->position_ms=gml_audio_sound_get_track_position(audio,entry->sound)*1000.0;
    gml_audio_pause_sound(audio,entry->sound,1);
    return vreal(0);
  }
  if(operation==GML_EXTERNAL_AUDIO_SA_RESUME){
    gml_audio_pause_sound(audio,entry->sound,0);
    return vreal(0);
  }
  if(operation==GML_EXTERNAL_AUDIO_SA_SEEK){
    char *end=NULL;
    double position=strtod(S(vm,args,count,0),&end);
    if(!end || *end || !isfinite(position) || position<0.0) return vreal(1);
    entry->position_ms=position;
    gml_audio_sound_set_track_position(audio,entry->sound,position/1000.0);
    return vreal(0);
  }
  if(operation==GML_EXTERNAL_AUDIO_SA_CLOSE){
    (void)external_saudio_close(vm,id);
    return vreal(0);
  }
  return vreal(1);
}

static GmlExternalAudioAsset *external_audio_asset_by_path(GmlBuiltinState *state,
                                                           const char *path){
  if(!state || !path) return NULL;
  for(uint32_t i=0;i<state->external_audio_asset_count;i++)
    if(state->external_audio_assets[i].path &&
       external_ascii_equal(state->external_audio_assets[i].path,path))
      return &state->external_audio_assets[i];
  return NULL;
}

static int external_bgm_sound(GmlVM *vm,int operation,GmlVal *args,int count){
  if(operation==GML_EXTERNAL_AUDIO_BGM_UNLOAD_NAME ||
     operation==GML_EXTERNAL_AUDIO_BGM_LOADED_NAME ||
     operation==GML_EXTERNAL_AUDIO_BGM_PLAY_NAME ||
     operation==GML_EXTERNAL_AUDIO_BGM_STOP_NAME ||
     operation==GML_EXTERNAL_AUDIO_BGM_PAUSE_NAME ||
     operation==GML_EXTERNAL_AUDIO_BGM_RESUME_NAME ||
     operation==GML_EXTERNAL_AUDIO_BGM_PLAYING_NAME ||
     operation==GML_EXTERNAL_AUDIO_BGM_LENGTH_NAME ||
     operation==GML_EXTERNAL_AUDIO_BGM_POSITION_NAME ||
     operation==GML_EXTERNAL_AUDIO_BGM_GET_ATTR_NAME ||
     operation==GML_EXTERNAL_AUDIO_BGM_SET_ATTR_NAME ||
     operation==GML_EXTERNAL_AUDIO_BGM_FADE_NAME){
    GmlExternalAudioAsset *asset=external_audio_asset_by_path(
      builtin_state_ensure(vm),S(vm,args,count,0));
    return asset?asset->sound:-1;
  }
  return (int)N(args,count,0);
}

static GmlVal external_bgm_call(GmlVM *vm,int operation,GmlVal *args,int count){
  GmlAudio *audio=vm?(GmlAudio*)vm->audio:NULL;
  if(!audio) return vreal(0);
  if(operation==GML_EXTERNAL_AUDIO_BGM_INIT) return vreal(1);
  if(operation==GML_EXTERNAL_AUDIO_BGM_CLOSE){ external_audio_free_all(vm); return vreal(1); }
  if(operation==GML_EXTERNAL_AUDIO_BGM_LOAD){
    int sound=external_audio_load(vm,S(vm,args,count,0));
    return vreal(sound>=0?sound:0);
  }
  if(operation==GML_EXTERNAL_AUDIO_BGM_ERROR) return vstr("");
  if(operation==GML_EXTERNAL_AUDIO_BGM_NOOP) return vreal(1);
  int sound=external_bgm_sound(vm,operation,args,count);
  if(operation==GML_EXTERNAL_AUDIO_BGM_LOADED_ID ||
     operation==GML_EXTERNAL_AUDIO_BGM_LOADED_NAME)
    return vreal(gml_audio_exists(audio,sound));
  if(sound<0 || !gml_audio_exists(audio,sound)) return vreal(0);
  if(operation==GML_EXTERNAL_AUDIO_BGM_UNLOAD_ID ||
     operation==GML_EXTERNAL_AUDIO_BGM_UNLOAD_NAME){ external_audio_free(vm,sound); return vreal(1); }
  if(operation==GML_EXTERNAL_AUDIO_BGM_PLAY_ID ||
     operation==GML_EXTERNAL_AUDIO_BGM_PLAY_NAME){
    int loop=N(args,count,1)!=0.0;
    gml_audio_sound_set_default_loop(audio,sound,loop);
    return vreal(gml_audio_play(audio,sound,loop)>=0);
  }
  if(operation==GML_EXTERNAL_AUDIO_BGM_STOP_ID ||
     operation==GML_EXTERNAL_AUDIO_BGM_STOP_NAME){ gml_audio_stop(audio,sound); return vreal(1); }
  if(operation==GML_EXTERNAL_AUDIO_BGM_PAUSE_ID ||
     operation==GML_EXTERNAL_AUDIO_BGM_PAUSE_NAME){ gml_audio_pause_sound(audio,sound,1); return vreal(1); }
  if(operation==GML_EXTERNAL_AUDIO_BGM_RESUME_ID ||
     operation==GML_EXTERNAL_AUDIO_BGM_RESUME_NAME){ gml_audio_pause_sound(audio,sound,0); return vreal(1); }
  if(operation==GML_EXTERNAL_AUDIO_BGM_PLAYING_ID ||
     operation==GML_EXTERNAL_AUDIO_BGM_PLAYING_NAME)
    return vreal(gml_audio_is_playing(audio,sound));
  if(operation==GML_EXTERNAL_AUDIO_BGM_LENGTH_ID ||
     operation==GML_EXTERNAL_AUDIO_BGM_LENGTH_NAME)
    return vreal(gml_audio_sound_length(audio,sound));
  if(operation==GML_EXTERNAL_AUDIO_BGM_POSITION_ID ||
     operation==GML_EXTERNAL_AUDIO_BGM_POSITION_NAME)
    return vreal(gml_audio_sound_get_track_position(audio,sound));
  if(operation==GML_EXTERNAL_AUDIO_BGM_GET_ATTR_ID ||
     operation==GML_EXTERNAL_AUDIO_BGM_GET_ATTR_NAME){
    const char *attribute=S(vm,args,count,1);
    if(external_ascii_equal(attribute,"cVolume")){
      GmlBuiltinState *state=builtin_state_ensure(vm);
      char *buffer=state->string_ring[state->string_ring_index++&7u];
      snprintf(buffer,sizeof(state->string_ring[0]),"%.6g",
               gml_audio_sound_get_gain(audio,sound)*100.0);
      return vstr(buffer);
    }
    return vstr("");
  }
  if(operation==GML_EXTERNAL_AUDIO_BGM_SET_ATTR_ID ||
     operation==GML_EXTERNAL_AUDIO_BGM_SET_ATTR_NAME){
    const char *attribute=S(vm,args,count,1);
    if(external_ascii_equal(attribute,"cVolume")){
      char *end=NULL; double volume=strtod(S(vm,args,count,2),&end);
      if(end && !*end){
        if(volume<0.0) volume=0.0; else if(volume>100.0) volume=100.0;
        gml_audio_sound_gain(audio,sound,volume/100.0);
      }
    }
    return vreal(1);
  }
  if(operation==GML_EXTERNAL_AUDIO_BGM_FADE_ID ||
     operation==GML_EXTERNAL_AUDIO_BGM_FADE_NAME){
    double volume=N(args,count,1);
    if(volume>1.0) volume/=100.0;
    int duration=(int)N(args,count,2);
    gml_audio_sound_gain_fade(audio,sound,volume,duration);
    return vreal(1);
  }
  return vreal(1);
}

static GmlVal external_fmod_call(GmlVM *vm,int operation,GmlVal *args,int count){
  GmlAudio *audio=vm?(GmlAudio*)vm->audio:NULL;
  if(!audio) return vreal(0);
  if(operation==GML_EXTERNAL_AUDIO_FMOD_INIT) return vreal(1);
  if(operation==GML_EXTERNAL_AUDIO_FMOD_FREE){ external_audio_free_all(vm); return vreal(1); }
  if(operation==GML_EXTERNAL_AUDIO_FMOD_SOUND_ADD)
    return vreal(external_audio_load(vm,S(vm,args,count,0)));
  if(operation==GML_EXTERNAL_AUDIO_FMOD_SOUND_GROUP){
    gml_audio_sound_set_external_group(audio,(int)N(args,count,0),(int)N(args,count,1));
    return vreal(1);
  }
  if(operation==GML_EXTERNAL_AUDIO_FMOD_GROUP_VOLUME){
    gml_audio_group_gain(audio,(int)N(args,count,0),N(args,count,1),0); return vreal(1);
  }
  if(operation==GML_EXTERNAL_AUDIO_FMOD_SOUND_VOLUME){
    gml_audio_sound_gain(audio,(int)N(args,count,0),N(args,count,1)); return vreal(1);
  }
  if(operation==GML_EXTERNAL_AUDIO_FMOD_SOUND_PLAY ||
     operation==GML_EXTERNAL_AUDIO_FMOD_SOUND_LOOP){
    int sound=(int)N(args,count,0);
    int loop=operation==GML_EXTERNAL_AUDIO_FMOD_SOUND_LOOP;
    gml_audio_sound_set_default_loop(audio,sound,loop);
    int voice=gml_audio_play(audio,sound,loop);
    if(voice>=0 && N(args,count,1)!=0.0) gml_audio_pause_sound(audio,voice,1);
    return vreal(voice);
  }
  if(operation==GML_EXTERNAL_AUDIO_FMOD_SOUND_PLAY_3D ||
     operation==GML_EXTERNAL_AUDIO_FMOD_SOUND_LOOP_3D){
    int sound=(int)N(args,count,0);
    int loop=operation==GML_EXTERNAL_AUDIO_FMOD_SOUND_LOOP_3D;
    int voice=gml_audio_play(audio,sound,loop);
    if(voice>=0){
      double dx=N(args,count,1)-vm->builtins->listener_x;
      double dy=N(args,count,2)-vm->builtins->listener_y;
      double distance=sqrt(dx*dx+dy*dy);
      double pan=distance>0.0?dx/distance:0.0;
      gml_audio_voice_spatial(audio,voice,1.0,pan);
      if(N(args,count,4)!=0.0) gml_audio_pause_sound(audio,voice,1);
    }
    return vreal(voice);
  }
  if(operation==GML_EXTERNAL_AUDIO_FMOD_SOUND_FREE){
    external_audio_free(vm,(int)N(args,count,0)); return vreal(1);
  }
  if(operation==GML_EXTERNAL_AUDIO_FMOD_GROUP_STOP){
    gml_audio_group_stop_all(audio,(int)N(args,count,0)); return vreal(1);
  }
  if(operation==GML_EXTERNAL_AUDIO_FMOD_ALL_STOP){ gml_audio_stop_all(audio); return vreal(1); }
  if(operation==GML_EXTERNAL_AUDIO_FMOD_INSTANCE_STOP){
    gml_audio_stop(audio,(int)N(args,count,0)); return vreal(1);
  }
  if(operation==GML_EXTERNAL_AUDIO_FMOD_INSTANCE_PLAYING)
    return vreal(gml_audio_is_playing(audio,(int)N(args,count,0)));
  if(operation==GML_EXTERNAL_AUDIO_FMOD_INSTANCE_PAUSED){
    gml_audio_pause_sound(audio,(int)N(args,count,0),N(args,count,1)!=0.0); return vreal(1);
  }
  if(operation==GML_EXTERNAL_AUDIO_FMOD_INSTANCE_GET_PAUSED)
    return vreal(gml_audio_voice_paused(audio,(int)N(args,count,0)));
  if(operation==GML_EXTERNAL_AUDIO_FMOD_INSTANCE_SOUND)
    return vreal(gml_audio_voice_sound(audio,(int)N(args,count,0)));
  if(operation==GML_EXTERNAL_AUDIO_FMOD_INSTANCE_POSITION){
    int voice=(int)N(args,count,0);
    int sound=gml_audio_voice_sound(audio,voice);
    double length=gml_audio_sound_length(audio,sound);
    double position=N(args,count,1);
    if(position<0.0) position=0.0; else if(position>1.0) position=1.0;
    gml_audio_sound_set_track_position(audio,voice,position*length);
    return vreal(1);
  }
  if(operation==GML_EXTERNAL_AUDIO_FMOD_INSTANCE_VOLUME){
    gml_audio_sound_gain(audio,(int)N(args,count,0),N(args,count,1)); return vreal(1);
  }
  if(operation==GML_EXTERNAL_AUDIO_FMOD_INSTANCE_GET_VOLUME)
    return vreal(gml_audio_sound_get_gain(audio,(int)N(args,count,0)));
  if(operation==GML_EXTERNAL_AUDIO_FMOD_INSTANCE_PAN){
    gml_audio_voice_spatial(audio,(int)N(args,count,0),1.0,N(args,count,1)); return vreal(1);
  }
  if(operation==GML_EXTERNAL_AUDIO_FMOD_INSTANCE_GET_PAN)
    return vreal(gml_audio_voice_pan(audio,(int)N(args,count,0)));
  if(operation==GML_EXTERNAL_AUDIO_FMOD_INSTANCE_FREQUENCY){
    int voice=(int)N(args,count,0),sound=gml_audio_voice_sound(audio,voice);
    int sample_rate=44100;
    (void)gml_audio_sound_format(audio,sound,NULL,&sample_rate,NULL);
    gml_audio_sound_pitch(audio,voice,N(args,count,1)/(double)sample_rate);
    return vreal(1);
  }
  if(operation==GML_EXTERNAL_AUDIO_FMOD_INSTANCE_GET_FREQUENCY){
    int voice=(int)N(args,count,0),sound=gml_audio_voice_sound(audio,voice);
    int sample_rate=44100;
    (void)gml_audio_sound_format(audio,sound,NULL,&sample_rate,NULL);
    return vreal(gml_audio_sound_get_pitch(audio,voice)*(double)sample_rate);
  }
  if(operation==GML_EXTERNAL_AUDIO_FMOD_INSTANCE_GET_POSITION){
    int voice=(int)N(args,count,0);
    int sound=gml_audio_voice_sound(audio,voice);
    double length=gml_audio_sound_length(audio,sound);
    return vreal(length>0.0?gml_audio_sound_get_track_position(audio,voice)/length:0.0);
  }
  if(operation==GML_EXTERNAL_AUDIO_FMOD_INSTANCE_SOUND_LENGTH){
    int sound=gml_audio_voice_sound(audio,(int)N(args,count,0));
    return vreal(gml_audio_sound_length(audio,sound)*1000.0);
  }
  if(operation==GML_EXTERNAL_AUDIO_FMOD_GROUP_GET_VOLUME)
    return vreal(gml_audio_group_get_gain(audio,(int)N(args,count,0)));
  if(operation==GML_EXTERNAL_AUDIO_FMOD_MASTER_VOLUME){
    gml_audio_set_master_gain(audio,N(args,count,0)); return vreal(1);
  }
  if(operation==GML_EXTERNAL_AUDIO_FMOD_SOUND_LENGTH)
    return vreal(gml_audio_sound_length(audio,(int)N(args,count,0))*1000.0);
  if(operation==GML_EXTERNAL_AUDIO_FMOD_SOUND_MAX_VOLUME)
    return vreal(gml_audio_sound_get_gain(audio,(int)N(args,count,0)));
  if(operation==GML_EXTERNAL_AUDIO_FMOD_SOUND_READY)
    return vreal(gml_audio_exists(audio,(int)N(args,count,0)));
  if(operation==GML_EXTERNAL_AUDIO_FMOD_INSTANCE_POSITION_3D){
    double dx=N(args,count,1)-vm->builtins->listener_x;
    double dy=N(args,count,2)-vm->builtins->listener_y;
    double distance=sqrt(dx*dx+dy*dy);
    gml_audio_voice_spatial(audio,(int)N(args,count,0),1.0,
                            distance>0.0?dx/distance:0.0);
    return vreal(1);
  }
  if(operation==GML_EXTERNAL_AUDIO_FMOD_LISTENER_POSITION_3D){
    vm->builtins->listener_x=N(args,count,1);
    vm->builtins->listener_y=N(args,count,2);
    vm->builtins->listener_z=N(args,count,3);
    return vreal(1);
  }
  if(operation==GML_EXTERNAL_AUDIO_FMOD_LAST_ERROR) return vreal(0);
  if(operation==GML_EXTERNAL_AUDIO_FMOD_UPDATE ||
     operation==GML_EXTERNAL_AUDIO_FMOD_NOOP) return vreal(1);
  return vreal(1);
}

static GmlVal external_pxtone_call(GmlVM *vm,int operation,GmlVal *args,int count){
  GmlAudio *audio=vm?(GmlAudio*)vm->audio:NULL;
  if(!audio) return vreal(0);
  if(operation==GML_EXTERNAL_AUDIO_PXTONE_INIT) return vreal(1);
  if(operation==GML_EXTERNAL_AUDIO_PXTONE_LOAD)
    return vreal(external_pxtone_load(vm,S(vm,args,count,0))>=0);
  int sound=external_pxtone_current(vm);
  if(operation==GML_EXTERNAL_AUDIO_PXTONE_SHUTDOWN ||
     operation==GML_EXTERNAL_AUDIO_PXTONE_RELEASE){
    if(sound>=0) external_audio_free(vm,sound);
    return vreal(1);
  }
  if(sound<0) return vreal(0);
  if(operation==GML_EXTERNAL_AUDIO_PXTONE_PLAY){
    int loop=N(args,count,2)!=0.0;
    gml_audio_sound_set_default_loop(audio,sound,loop);
    gml_audio_sound_gain(audio,sound,1.0);
    gml_audio_sound_set_track_position(audio,sound,0.0);
    return vreal(gml_audio_play(audio,sound,loop)>=0);
  }
  if(operation==GML_EXTERNAL_AUDIO_PXTONE_VOLUME){
    double volume=N(args,count,0);
    if(volume<0.0) volume=0.0; else if(volume>1.0) volume=1.0;
    gml_audio_sound_gain(audio,sound,volume); return vreal(1);
  }
  if(operation==GML_EXTERNAL_AUDIO_PXTONE_FADE){
    int milliseconds=(int)N(args,count,0);
    gml_audio_sound_gain_fade(audio,sound,0.0,milliseconds); return vreal(1);
  }
  if(operation==GML_EXTERNAL_AUDIO_PXTONE_LOOP){
    gml_audio_sound_set_default_loop(audio,sound,N(args,count,0)!=0.0); return vreal(1);
  }
  if(operation==GML_EXTERNAL_AUDIO_PXTONE_STOP){
    gml_audio_stop(audio,sound); return vreal(1);
  }
  return vreal(1);
}


/* ---------------------------------------------------------------- jbfmod.dll */

/* Module playback rides the ordinary mixer exactly as pxtone does: LoadSong renders the module
 * once at 44100 and registers the PCM as a dynamic sound whose identity is the source file's
 * bytes, so savestates restore it by re-reading the module and re-rendering — both deterministic.
 * Slot bookkeeping is persisted through jbfmod-prefixed Saudio entries, which already serialize;
 * the in-memory cache below only holds what a restore can re-derive (the parsed module and its
 * timeline), and is rebuilt lazily when a getter first asks after a state load. */
enum { JB_SLOTS = 64, JB_SPECTRUM_BANDS = 512 };
typedef struct {
  GmlTrackerModule *module;
  int sound;
  uint32_t loop_frame;
  uint32_t once_cursor[128];       /* GetInstrumentPlayedOnce per-instrument consumption */
} JbSlot;
typedef struct {
  JbSlot slot[JB_SLOTS];
  int current;
  int master_volume;               /* 0..256 extension volume scale */
  int pan_separation;              /* 0..128; applies to renders made after it is set */
  int looping;
  int spectrum_on;
  int error_code;
  char error_message[96];
} JbState;

static int external_tracker_path(const char *path){
  if(!path) return 0;
  size_t size=strlen(path);
  return (size>=4u && external_ascii_equal(path+size-4u,".mod")) ||
         (size>=3u && external_ascii_equal(path+size-3u,".xm"));
}

static JbState *jbfmod_state(GmlVM *vm){
  GmlBuiltinState *state=vm?builtin_state_ensure(vm):NULL;
  if(!state) return NULL;
  if(!state->jbfmod){
    JbState *jb=(JbState*)calloc(1,sizeof *jb);
    if(!jb) return NULL;
    jb->current=-1;
    jb->master_volume=256;
    jb->pan_separation=128;
    jb->looping=1;
    for(int i=0;i<JB_SLOTS;i++) jb->slot[i].sound=-1;
    state->jbfmod=jb;
  }
  return (JbState*)state->jbfmod;
}

void builtin_jbfmod_state_free(GmlBuiltinState *state){
  if(!state || !state->jbfmod) return;
  JbState *jb=(JbState*)state->jbfmod;
  for(int i=0;i<JB_SLOTS;i++) gml_tracker_free(jb->slot[i].module);
  free(jb);
  state->jbfmod=NULL;
}

static void jb_error(JbState *jb,int code,const char *message){
  if(!jb) return;
  jb->error_code=code;
  snprintf(jb->error_message,sizeof(jb->error_message),"%s",message?message:"");
}

static void jb_saudio_id(int slot,char out[24]){ snprintf(out,24,"jbfmod:%02d",slot); }

static void jb_drop_slot(GmlVM *vm,JbState *jb,int slot){
  JbSlot *s=&jb->slot[slot];
  if(s->sound>=0) external_audio_free(vm,s->sound);
  gml_tracker_free(s->module);
  char id[24]; jb_saudio_id(slot,id);
  builtin_state_saudio_remove(builtin_state_ensure(vm),id);
  memset(s,0,sizeof *s);
  s->sound=-1;
  if(jb->current==slot) jb->current=-1;
}

static int jb_load_slot(GmlVM *vm,JbState *jb,int slot,const char *relative){
  if(!vm || !relative || !*relative || strlen(relative)>4096u){
    jb_error(jb,1,"bad song path"); return 0;
  }
  jb_drop_slot(vm,jb,slot);
  char *path=resolve_read_path(vm,relative);
  uint8_t *encoded=NULL; size_t size=0;
  int16_t *pcm=NULL; uint32_t frames=0,loop_frame=0;
  char reason[96]={0};
  GmlTrackerModule *module=NULL;
  int sound=-1;
  if(path && anygm_vfs_read_all(vm->host,path,&encoded,&size,16u*1024u*1024u) && size)
    module=gml_tracker_load(encoded,size,reason,sizeof(reason));
  if(module && gml_tracker_render(module,44100,jb->pan_separation,&pcm,&frames,&loop_frame)){
    sound=gml_audio_add_pcm16((GmlAudio*)vm->audio,pcm,frames,2,44100,encoded,size);
    if(sound>=0){
      uint8_t identity[32];
      char id[24]; jb_saudio_id(slot,id);
      GmlBuiltinState *state=builtin_state_ensure(vm);
      if(!gml_audio_sound_content_hash((GmlAudio*)vm->audio,sound,identity) ||
         !builtin_state_external_audio_store(state,sound,relative,identity) ||
         !builtin_state_saudio_store(state,id,relative,identity,sound,0)){
        external_audio_free(vm,sound); sound=-1;
      } else {
        gml_audio_sound_loop_start((GmlAudio*)vm->audio,sound,(double)loop_frame/44100.0);
        gml_audio_sound_set_default_loop((GmlAudio*)vm->audio,sound,jb->looping);
        gml_audio_sound_gain((GmlAudio*)vm->audio,sound,jb->master_volume/256.0);
      }
    }
  }
  free(pcm); free(encoded); free(path);
  if(sound<0){
    gml_tracker_free(module);
    jb_error(jb,1,reason[0]?reason:"could not load the module");
    return 0;
  }
  jb->slot[slot].module=module;
  jb->slot[slot].sound=sound;
  jb->slot[slot].loop_frame=loop_frame;
  jb_error(jb,0,"");
  return 1;
}

/* After a state load the mixer already holds the song (asset restore re-rendered it); the module
 * metadata is re-derived here from the Saudio record the state carried. */
static JbSlot *jb_slot_get(GmlVM *vm,JbState *jb,int slot){
  if(slot<0 || slot>=JB_SLOTS) return NULL;
  JbSlot *s=&jb->slot[slot];
  if(s->module) return s;
  char id[24]; jb_saudio_id(slot,id);
  GmlSaudioEntry *entry=builtin_state_saudio_find(builtin_state_ensure(vm),id);
  if(!entry || !entry->path) return NULL;
  char *path=resolve_read_path(vm,entry->path);
  uint8_t *encoded=NULL; size_t size=0;
  if(path && anygm_vfs_read_all(vm->host,path,&encoded,&size,16u*1024u*1024u) && size){
    GmlTrackerModule *module=gml_tracker_load(encoded,size,NULL,0);
    if(module){
      int16_t *pcm=NULL; uint32_t frames=0,loop_frame=0;
      if(gml_tracker_render(module,44100,jb->pan_separation,&pcm,&frames,&loop_frame)){
        s->module=module;
        s->sound=entry->sound;
        s->loop_frame=loop_frame;
      } else gml_tracker_free(module);
      free(pcm);
    }
  }
  free(encoded); free(path);
  return s->module?s:NULL;
}

static uint32_t jb_position_frames(GmlVM *vm,const JbSlot *s){
  double seconds=gml_audio_sound_get_track_position((GmlAudio*)vm->audio,s->sound);
  if(seconds<0.0) seconds=0.0;
  return (uint32_t)(seconds*44100.0);
}

static const GmlTrackerRowMark *jb_mark_at(const JbSlot *s,uint32_t frame){
  uint32_t count=0;
  const GmlTrackerRowMark *marks=gml_tracker_timeline(s->module,&count);
  if(!marks || !count) return NULL;
  uint32_t low=0,high=count;
  while(low+1<high){
    uint32_t mid=(low+high)/2;
    if(marks[mid].frame<=frame) low=mid; else high=mid;
  }
  return &marks[low];
}

static GmlVal external_jbfmod_call(GmlVM *vm,int operation,GmlVal *args,int count){
  JbState *jb=jbfmod_state(vm);
  GmlAudio *audio=vm?(GmlAudio*)vm->audio:NULL;
  GmlBuiltinState *state=vm?builtin_state_ensure(vm):NULL;
  if(!jb || !audio || !state) return vreal(0);
  JbSlot *cur=jb->current>=0?jb_slot_get(vm,jb,jb->current):NULL;
  switch(operation){
    case GML_EXTERNAL_AUDIO_JB_INIT: jb_error(jb,0,""); return vreal(1);
    case GML_EXTERNAL_AUDIO_JB_CLOSE:
      for(int i=0;i<JB_SLOTS;i++) if(jb->slot[i].sound>=0 || jb->slot[i].module) jb_drop_slot(vm,jb,i);
      jb->current=-1;
      return vreal(1);
    case GML_EXTERNAL_AUDIO_JB_LOAD_SONG: {
      int slot=(int)N(args,count,0);
      if(slot<0 || slot>=JB_SLOTS){ jb_error(jb,1,"song id out of range"); return vreal(0); }
      return vreal(jb_load_slot(vm,jb,slot,S(vm,args,count,1)));
    }
    case GML_EXTERNAL_AUDIO_JB_LOAD_SONG_PACK:
      /* Song-pack decoding is unsupported; refuse instead of reporting a successful load. */
      jb_error(jb,1,"song packs are not supported");
      return vreal(0);
    case GML_EXTERNAL_AUDIO_JB_PLAY_SONG: {
      int slot=(int)N(args,count,0);
      JbSlot *s=jb_slot_get(vm,jb,slot);
      if(!s){ jb_error(jb,1,"song is not loaded"); return vreal(0); }
      if(cur && cur->sound>=0) gml_audio_stop(audio,cur->sound);
      gml_audio_sound_set_default_loop(audio,s->sound,jb->looping);
      gml_audio_play(audio,s->sound,jb->looping);
      jb->current=slot;
      return vreal(1);
    }
    case GML_EXTERNAL_AUDIO_JB_STOP_SONG:
      if(cur && cur->sound>=0) gml_audio_stop(audio,cur->sound);
      jb->current=-1;
      return vreal(1);
    case GML_EXTERNAL_AUDIO_JB_SET_LOOPING:
      jb->looping=N(args,count,0)!=0.0;
      for(int i=0;i<JB_SLOTS;i++)
        if(jb->slot[i].sound>=0) gml_audio_sound_set_default_loop(audio,jb->slot[i].sound,jb->looping);
      return vreal(1);
    case GML_EXTERNAL_AUDIO_JB_SET_MASTER_VOLUME: {
      int volume=(int)N(args,count,0);
      if(volume<0) volume=0;
      if(volume>256) volume=256;
      jb->master_volume=volume;
      for(int i=0;i<JB_SLOTS;i++)
        if(jb->slot[i].sound>=0) gml_audio_sound_gain(audio,jb->slot[i].sound,volume/256.0);
      return vreal(1);
    }
    case GML_EXTERNAL_AUDIO_JB_GET_MASTER_VOLUME: return vreal(jb->master_volume);
    case GML_EXTERNAL_AUDIO_JB_SET_ORDER: {
      if(!cur) return vreal(0);
      int order=(int)N(args,count,0);
      uint32_t mark_count=0;
      const GmlTrackerRowMark *marks=gml_tracker_timeline(cur->module,&mark_count);
      for(uint32_t i=0;i<mark_count;i++)
        if(marks[i].order==order){
          gml_audio_sound_set_track_position(audio,cur->sound,(double)marks[i].frame/44100.0);
          return vreal(1);
        }
      return vreal(0);
    }
    case GML_EXTERNAL_AUDIO_JB_SET_PAN_SEPARATION: {
      int separation=(int)N(args,count,0);
      if(separation<0) separation=0;
      if(separation>128) separation=128;
      /* Applies to songs loaded from here on: panning is baked into the render, and re-rendering
       * a playing song would tear the voice under it. */
      jb->pan_separation=separation;
      return vreal(1);
    }
    case GML_EXTERNAL_AUDIO_JB_SET_PAUSED:
      if(cur && cur->sound>=0) gml_audio_pause_sound(audio,cur->sound,N(args,count,0)!=0.0);
      return vreal(1);
    case GML_EXTERNAL_AUDIO_JB_GET_PAUSED:
      return vreal(cur && cur->sound>=0 && gml_audio_voice_paused(audio,cur->sound));
    case GML_EXTERNAL_AUDIO_JB_IS_PLAYING:
      return vreal(cur && cur->sound>=0 && gml_audio_is_playing(audio,cur->sound));
    case GML_EXTERNAL_AUDIO_JB_IS_FINISHED:
      return vreal(cur && cur->sound>=0 && !gml_audio_is_playing(audio,cur->sound));
    case GML_EXTERNAL_AUDIO_JB_GET_ORDER: case GML_EXTERNAL_AUDIO_JB_GET_PATTERN:
    case GML_EXTERNAL_AUDIO_JB_GET_ROW: case GML_EXTERNAL_AUDIO_JB_GET_SPEED:
    case GML_EXTERNAL_AUDIO_JB_GET_BPM: case GML_EXTERNAL_AUDIO_JB_GET_PATTERN_LENGTH: {
      if(!cur) return vreal(0);
      const GmlTrackerRowMark *mark=jb_mark_at(cur,jb_position_frames(vm,cur));
      if(!mark) return vreal(0);
      switch(operation){
        case GML_EXTERNAL_AUDIO_JB_GET_ORDER: return vreal(mark->order);
        case GML_EXTERNAL_AUDIO_JB_GET_PATTERN: return vreal(mark->pattern);
        case GML_EXTERNAL_AUDIO_JB_GET_ROW: return vreal(mark->row);
        case GML_EXTERNAL_AUDIO_JB_GET_SPEED: return vreal(mark->speed);
        case GML_EXTERNAL_AUDIO_JB_GET_BPM: return vreal(mark->bpm);
        default: return vreal(gml_tracker_pattern_rows(cur->module,mark->pattern));
      }
    }
    case GML_EXTERNAL_AUDIO_JB_GET_TIME:
      return vreal(cur?gml_audio_sound_get_track_position(audio,cur->sound)*1000.0:0.0);
    case GML_EXTERNAL_AUDIO_JB_GET_TIME_FORMAT: {
      double seconds=cur?gml_audio_sound_get_track_position(audio,cur->sound):0.0;
      if(seconds<0.0) seconds=0.0;
      char *buffer=state->string_ring[state->string_ring_index++&7u];
      snprintf(buffer,sizeof(state->string_ring[0]),"%02d:%02d:%02d",
               (int)(seconds/60.0),(int)seconds%60,((int)(seconds*100.0))%100);
      return vstr(buffer);
    }
    case GML_EXTERNAL_AUDIO_JB_GET_CURRENT_SONG: return vreal(jb->current<0?0:jb->current);
    case GML_EXTERNAL_AUDIO_JB_GET_SONG_EXISTS: {
      int slot=(int)N(args,count,0);
      return vreal(jb_slot_get(vm,jb,slot)!=NULL);
    }
    case GML_EXTERNAL_AUDIO_JB_GET_NAME: case GML_EXTERNAL_AUDIO_JB_GET_TYPE:
    case GML_EXTERNAL_AUDIO_JB_GET_NUM_CHANNELS: case GML_EXTERNAL_AUDIO_JB_GET_NUM_INSTRUMENTS:
    case GML_EXTERNAL_AUDIO_JB_GET_NUM_ORDERS: case GML_EXTERNAL_AUDIO_JB_GET_NUM_PATTERNS:
    case GML_EXTERNAL_AUDIO_JB_GET_NUM_SAMPLES: {
      JbSlot *s=jb_slot_get(vm,jb,(int)N(args,count,0));
      if(!s){
        return operation==GML_EXTERNAL_AUDIO_JB_GET_NAME?vstr(""):vreal(0);
      }
      switch(operation){
        case GML_EXTERNAL_AUDIO_JB_GET_NAME: {
          char *buffer=state->string_ring[state->string_ring_index++&7u];
          snprintf(buffer,sizeof(state->string_ring[0]),"%s",gml_tracker_name(s->module));
          return vstr(buffer);
        }
        /* FMOD's own type codes: 1 is MOD, 3 is XM. */
        case GML_EXTERNAL_AUDIO_JB_GET_TYPE:
          return vreal(gml_tracker_type(s->module)[0]=='X'?3:1);
        case GML_EXTERNAL_AUDIO_JB_GET_NUM_CHANNELS:
          return vreal(gml_tracker_num_channels(s->module));
        case GML_EXTERNAL_AUDIO_JB_GET_NUM_INSTRUMENTS:
          /* This interface reports no instrument count for MOD modules. */
          return vreal(gml_tracker_type(s->module)[0]=='X'
                         ?gml_tracker_num_instruments(s->module):0);
        case GML_EXTERNAL_AUDIO_JB_GET_NUM_ORDERS:
          return vreal(gml_tracker_num_orders(s->module));
        case GML_EXTERNAL_AUDIO_JB_GET_NUM_PATTERNS:
          return vreal(gml_tracker_num_patterns(s->module));
        default:
          /* MOD modules expose 31 fixed sample slots, including empty slots. */
          return vreal(gml_tracker_type(s->module)[0]=='X'
                         ?gml_tracker_num_samples(s->module):31);
      }
    }
    case GML_EXTERNAL_AUDIO_JB_GET_CHANNELS_PLAYING:
      return vreal(cur && gml_audio_is_playing(audio,cur->sound)
                     ?gml_tracker_num_channels(cur->module):0);
    case GML_EXTERNAL_AUDIO_JB_GET_CPU_USAGE: return vreal(0);
    case GML_EXTERNAL_AUDIO_JB_GET_FREQUENCY: return vreal(0);
    case GML_EXTERNAL_AUDIO_JB_GET_ERROR_CODE: return vreal(jb->error_code);
    case GML_EXTERNAL_AUDIO_JB_GET_ERROR_MESSAGE: {
      char *buffer=state->string_ring[state->string_ring_index++&7u];
      snprintf(buffer,sizeof(state->string_ring[0]),"%.*s",
               (int)sizeof(state->string_ring[0])-1,jb->error_message);
      return vstr(buffer);
    }
    case GML_EXTERNAL_AUDIO_JB_GET_JB_INFO: return vstr("");
    case GML_EXTERNAL_AUDIO_JB_GET_PACK_INFO: return vstr("");
    case GML_EXTERNAL_AUDIO_JB_GET_INSTRUMENT_PLAYED:
    case GML_EXTERNAL_AUDIO_JB_GET_INSTRUMENT_PLAYED_ONCE: {
      if(!cur) return vreal(0);
      int instrument=(int)N(args,count,0);
      if(instrument<0 || instrument>=128) return vreal(0);
      uint32_t position=jb_position_frames(vm,cur);
      uint32_t event_count=0;
      const GmlTrackerNoteEvent *events=gml_tracker_events(cur->module,&event_count);
      if(operation==GML_EXTERNAL_AUDIO_JB_GET_INSTRUMENT_PLAYED){
        uint32_t played=0;
        for(uint32_t i=0;i<event_count && events[i].frame<=position;i++)
          if(events[i].instrument==(uint16_t)instrument) played++;
        return vreal(played);
      }
      uint32_t since=cur->once_cursor[instrument];
      for(uint32_t i=0;i<event_count && events[i].frame<=position;i++)
        if(events[i].frame>since && events[i].instrument==(uint16_t)instrument){
          cur->once_cursor[instrument]=position;
          return vreal(1);
        }
      return vreal(0);
    }
    /* Neither MOD nor XM has a Zxx effect — it is an Impulse Tracker MIDI macro — so there is
     * nothing these can ever report for the formats this engine plays. */
    case GML_EXTERNAL_AUDIO_JB_GET_ZXX: case GML_EXTERNAL_AUDIO_JB_GET_ZXX_ONCE:
    case GML_EXTERNAL_AUDIO_JB_GET_ZXX_PLAYED: case GML_EXTERNAL_AUDIO_JB_GET_ZXX_PLAYED_ONCE:
      return vreal(0);
    case GML_EXTERNAL_AUDIO_JB_INIT_SPECTRUM: jb->spectrum_on=1; return vreal(1);
    case GML_EXTERNAL_AUDIO_JB_CLOSE_SPECTRUM: jb->spectrum_on=0; return vreal(1);
    case GML_EXTERNAL_AUDIO_JB_GET_SPECTRUM: {
      if(!jb->spectrum_on || !cur || cur->sound<0) return vreal(0);
      uint32_t frames=0; int channels=0;
      const int16_t *pcm=gml_audio_sound_pcm16(audio,cur->sound,&frames,&channels);
      if(!pcm || channels!=2) return vreal(0);
      uint32_t position=jb_position_frames(vm,cur);
      /* Spectrum uses the first band argument; an optional second argument is ignored. */
      int band=(int)N(args,count,0);
      if(band<0 || band>=JB_SPECTRUM_BANDS) return vreal(0);
      return vreal((double)gml_tracker_spectrum(pcm,frames,position,band,JB_SPECTRUM_BANDS)/65536.0);
    }
    default: return vreal(0);
  }
}

GmlVal builtin_external_audio_call(GmlVM *vm,int handle,
                                   GmlVal *args,int count,int *handled){
  if(handled) *handled=0;
  int operation=handle-GML_EXTERNAL_AUDIO_HANDLE_BASE;
  if(operation<=0 || operation>=GML_EXTERNAL_AUDIO_OPERATION_LIMIT)
    return vreal(0);
  if(handled) *handled=1;
  GmlAudio *audio=vm?(GmlAudio*)vm->audio:NULL;
  if(operation>=GML_EXTERNAL_AUDIO_SA_OPEN &&
     operation<=GML_EXTERNAL_AUDIO_SA_CLOSE_ALL)
    return external_saudio_call(vm,operation,args,count);
  if(operation>=GML_EXTERNAL_AUDIO_BGM_INIT &&
     operation<=GML_EXTERNAL_AUDIO_BGM_NOOP)
    return external_bgm_call(vm,operation,args,count);
  if(operation>=GML_EXTERNAL_AUDIO_FMOD_INIT &&
     operation<=GML_EXTERNAL_AUDIO_FMOD_NOOP)
    return external_fmod_call(vm,operation,args,count);
  if(operation>=GML_EXTERNAL_AUDIO_PXTONE_INIT &&
     operation<=GML_EXTERNAL_AUDIO_PXTONE_SHUTDOWN)
    return external_pxtone_call(vm,operation,args,count);
  if(operation>=GML_EXTERNAL_AUDIO_JB_INIT &&
     operation<=GML_EXTERNAL_AUDIO_JB_GET_SPECTRUM)
    return external_jbfmod_call(vm,operation,args,count);
  if(operation==GML_EXTERNAL_AUDIO_INIT) return vreal(1);
  if(operation==GML_EXTERNAL_AUDIO_FREE){
    external_audio_free_all(vm);
    return vreal(0);
  }
  if(operation==GML_EXTERNAL_AUDIO_SET_LISTENER_POSITION){
    if(vm && vm->builtins){
      vm->builtins->listener_x=N(args,count,0);
      vm->builtins->listener_y=N(args,count,1);
      vm->builtins->listener_z=N(args,count,2);
      audio_refresh_emitters(vm,audio);
    }
    return vreal(0);
  }
  if(operation==GML_EXTERNAL_AUDIO_SET_LISTENER_DIRECTION){
    if(vm && vm->builtins){
      vm->builtins->listener_forward_x=N(args,count,0);
      vm->builtins->listener_forward_y=N(args,count,1);
      vm->builtins->listener_forward_z=N(args,count,2);
      vm->builtins->listener_up_x=N(args,count,3);
      vm->builtins->listener_up_y=N(args,count,4);
      vm->builtins->listener_up_z=N(args,count,5);
      audio_refresh_emitters(vm,audio);
    }
    return vreal(0);
  }
  if(operation==GML_EXTERNAL_AUDIO_SET_GROUP_VOLUME){
    gml_audio_group_gain(audio,(int)N(args,count,0),N(args,count,1),0);
    return vreal(0);
  }
  if(operation==GML_EXTERNAL_AUDIO_GET_GROUP_VOLUME)
    return vreal(gml_audio_group_get_gain(audio,(int)N(args,count,0)));
  if(operation==GML_EXTERNAL_AUDIO_CREATE_EMITTER)
    return vreal(external_audio_load(vm,S(vm,args,count,0)));
  if(operation==GML_EXTERNAL_AUDIO_DESTROY_EMITTER){
    external_audio_free(vm,(int)N(args,count,0));
    return vreal(0);
  }
  if(operation==GML_EXTERNAL_AUDIO_PLAY){
    int sound=(int)N(args,count,0);
    int loop=gml_audio_sound_get_default_loop(audio,sound);
    int voice=gml_audio_play(audio,sound,loop);
    if(vm && builtin_setting(vm,"GML_LOG_AUDIO"))
      anygm_host_logf(vm->host,ANYGM_LOG_DEBUG,
                      "[external-audio] play sound=%d loop=%d voice=%d\n",
                      sound,loop,voice);
    return vreal(0);
  }
  if(operation==GML_EXTERNAL_AUDIO_PAUSE){
    gml_audio_pause_sound(audio,(int)N(args,count,0),1);
    return vreal(0);
  }
  if(operation==GML_EXTERNAL_AUDIO_STOP){
    gml_audio_stop(audio,(int)N(args,count,0));
    return vreal(0);
  }
  if(operation==GML_EXTERNAL_AUDIO_REWIND){
    gml_audio_sound_set_track_position(audio,(int)N(args,count,0),0.0);
    return vreal(0);
  }
  if(operation==GML_EXTERNAL_AUDIO_IS_PLAYING)
    return vreal(gml_audio_is_playing(audio,(int)N(args,count,0)));
  if(operation==GML_EXTERNAL_AUDIO_SET_VOLUME){
    int sound=(int)N(args,count,0);
    double gain=N(args,count,1);
    gml_audio_sound_gain(audio,sound,gain);
    if(vm && builtin_setting(vm,"GML_LOG_AUDIO"))
      anygm_host_logf(vm->host,ANYGM_LOG_DEBUG,
                      "[external-audio] gain sound=%d value=%.6f\n",
                      sound,gain);
    return vreal(0);
  }
  if(operation==GML_EXTERNAL_AUDIO_GET_VOLUME)
    return vreal(gml_audio_sound_get_gain(audio,(int)N(args,count,0)));
  if(operation==GML_EXTERNAL_AUDIO_FADE_VOLUME){
    int sound=(int)N(args,count,0);
    double gain=N(args,count,1);
    double seconds=N(args,count,2);
    double milliseconds=seconds>0.0?seconds*1000.0:0.0;
    int duration=milliseconds>(double)INT_MAX?INT_MAX:(int)milliseconds;
    gml_audio_sound_gain_fade(audio,sound,gain,duration);
    if(vm && builtin_setting(vm,"GML_LOG_AUDIO"))
      anygm_host_logf(vm->host,ANYGM_LOG_DEBUG,
                      "[external-audio] fade sound=%d target=%.6f ms=%d\n",
                      sound,gain,duration);
    return vreal(0);
  }
  if(operation==GML_EXTERNAL_AUDIO_SET_TYPE){
    gml_audio_sound_set_external_type(
        audio,(int)N(args,count,0),external_audio_type(N(args,count,1)));
    return vreal(0);
  }
  if(operation==GML_EXTERNAL_AUDIO_GET_TYPE)
    return vreal((double)gml_audio_sound_get_external_type(
        audio,(int)N(args,count,0)));
  if(operation==GML_EXTERNAL_AUDIO_SET_LOOPING){
    gml_audio_sound_set_default_loop(
        audio,(int)N(args,count,0),N(args,count,1)!=0.0);
    return vreal(0);
  }
  /* Sound handles map onto mixer sound IDs. Volume is 0..10000 and
   * frequency is absolute hertz, so convert them to gain and pitch. */
  if(operation==GML_EXTERNAL_AUDIO_SS_LOAD){
    /* Return the load handle as decimal text; accessors parse it numerically.
     * The failure result is represented by the string "0". */
    int loaded=external_audio_load(vm,S(vm,args,count,0));
    char handle_text[32];
    snprintf(handle_text,sizeof handle_text,"%d",loaded<0?0:loaded);
    return vstr(handle_text);
  }
  if(operation==GML_EXTERNAL_AUDIO_SS_PLAY || operation==GML_EXTERNAL_AUDIO_SS_LOOP ||
     operation==GML_EXTERNAL_AUDIO_SS_RESUME){
    int sound=(int)N(args,count,0);
    int loop=operation==GML_EXTERNAL_AUDIO_SS_LOOP
               ? 1 : gml_audio_sound_get_default_loop(audio,sound);
    if(operation==GML_EXTERNAL_AUDIO_SS_LOOP)
      gml_audio_sound_set_default_loop(audio,sound,1);
    int voice=gml_audio_play(audio,sound,loop);
    if(vm && builtin_setting(vm,"GML_LOG_AUDIO"))
      anygm_host_logf(vm->host,ANYGM_LOG_DEBUG,
                      "[external-audio] supersound play sound=%d loop=%d voice=%d\n",
                      sound,loop,voice);
    return vreal(0);
  }
  if(operation==GML_EXTERNAL_AUDIO_SS_FREE_SOUND){
    external_audio_free(vm,(int)N(args,count,0));
    return vreal(0);
  }
  if(operation==GML_EXTERNAL_AUDIO_SS_SET_VOLUME){
    double level=N(args,count,1)/10000.0;
    if(level<0.0) level=0.0; else if(level>1.0) level=1.0;
    gml_audio_sound_gain(audio,(int)N(args,count,0),level);
    return vreal(0);
  }
  if(operation==GML_EXTERNAL_AUDIO_SS_GET_VOLUME)
    return vreal(gml_audio_sound_get_gain(audio,(int)N(args,count,0))*10000.0);
  if(operation==GML_EXTERNAL_AUDIO_SS_SET_FREQ){
    double hz=N(args,count,1);
    if(hz>0.0) gml_audio_sound_pitch(audio,(int)N(args,count,0),hz/44100.0);
    return vreal(0);
  }
  if(operation==GML_EXTERNAL_AUDIO_SS_GET_FREQ)
    return vreal(gml_audio_sound_get_pitch(audio,(int)N(args,count,0))*44100.0);
  if(operation==GML_EXTERNAL_AUDIO_SS_SET_PAN || operation==GML_EXTERNAL_AUDIO_SS_GET_PAN)
    /* Per-sound pan is not implemented by the mixer; report center. */
    return vreal(0);
  if(operation==GML_EXTERNAL_AUDIO_SS_SET_POSITION){
    gml_audio_sound_set_track_position(audio,(int)N(args,count,0),N(args,count,1));
    return vreal(0);
  }
  if(operation==GML_EXTERNAL_AUDIO_SS_GET_POSITION)
    return vreal(gml_audio_sound_get_track_position(audio,(int)N(args,count,0)));
  if(operation==GML_EXTERNAL_AUDIO_SS_IS_PAUSED)
    return vreal(!gml_audio_is_playing(audio,(int)N(args,count,0)));
  if(operation==GML_EXTERNAL_AUDIO_SS_IS_LOOPING)
    return vreal(gml_audio_sound_get_default_loop(audio,(int)N(args,count,0)));
  if(operation==GML_EXTERNAL_AUDIO_SS_GET_LENGTH)
    return vreal(gml_audio_sound_length(audio,(int)N(args,count,0)));
  if(operation==GML_EXTERNAL_AUDIO_SS_GET_BYTES_PER_SECOND)
    /* Sixteen-bit stereo at the mixer's rate. */
    return vreal(44100.0*2.0*2.0);
  if(operation==GML_EXTERNAL_AUDIO_SS_IS_HANDLE_VALID)
    return vreal((int)N(args,count,0)>=0);
  if(operation==GML_EXTERNAL_AUDIO_TRACK_PLAY){
    int prior=(int)N(args,count,0);
    if(prior>=0) external_audio_free(vm,prior);
    int sound=external_audio_load(vm,S(vm,args,count,1));
    if(sound>=0){
      gml_audio_sound_set_default_loop(audio,sound,N(args,count,2)!=0.0);
      gml_audio_sound_set_external_type(
          audio,sound,external_audio_type(N(args,count,3)));
      gml_audio_play(audio,sound,gml_audio_sound_get_default_loop(audio,sound));
    }
    return vreal(sound);
  }
  /* The software mixer is non-spatial for this legacy extension. Its 2D/3D mode, distance
   * factor, and position controls remain deterministic no-ops until the shared emitter model
   * gains a sound-index binding. */
  return vreal(0);
}

static GmlVal builtin_faudio_gms(GmlVM *vm,const char *name,GmlVal *args,int count){
  GmlAudio *audio=vm?(GmlAudio*)vm->audio:NULL;
  if(!audio || !name) return vreal(0);
  if(!strcmp(name,"FAudioGMS_Init")) return vreal(1);
  if(!strcmp(name,"FAudioGMS_Destroy")){
    external_audio_free_all(vm); return vreal(0);
  }
  if(!strcmp(name,"FAudioGMS_Update")) return vreal(0);
  if(!strcmp(name,"FAudioGMS_StaticSound_LoadWAV")||
     !strcmp(name,"FAudioGMS_StreamingSound_LoadOGG"))
    return vreal(external_audio_load(vm,S(vm,args,count,0)));
  if(!strcmp(name,"FAudioGMS_StaticSound_CreateSoundInstance")){
    GmlBuiltinState *state=builtin_state_ensure(vm);
    GmlExternalAudioAsset *asset=state?
      builtin_state_external_audio_find(state,(int)N(args,count,0)):NULL;
    return vreal(asset&&asset->path?external_audio_load(vm,asset->path):-1);
  }
  if(!strcmp(name,"FAudioGMS_StaticSound_Destroy")){
    external_audio_free(vm,(int)N(args,count,0)); return vreal(0);
  }
  if(!strcmp(name,"FAudioGMS_SoundInstance_Play")){
    int sound=(int)N(args,count,0);
    gml_audio_stop(audio,sound);
    (void)gml_audio_play(audio,sound,gml_audio_sound_get_default_loop(audio,sound));
    return vreal(0);
  }
  if(!strcmp(name,"FAudioGMS_SoundInstance_Stop")){
    gml_audio_stop(audio,(int)N(args,count,0)); return vreal(0);
  }
  if(!strcmp(name,"FAudioGMS_SoundInstance_Pause")){
    gml_audio_pause_sound(audio,(int)N(args,count,0),1); return vreal(0);
  }
  if(!strcmp(name,"FAudioGMS_SoundInstance_Destroy")){
    external_audio_free(vm,(int)N(args,count,0)); return vreal(0);
  }
  if(!strcmp(name,"FAudioGMS_SoundInstance_DestroyWhenFinished")) return vreal(0);
  if(!strcmp(name,"FAudioGMS_SoundInstance_SetLoop")){
    gml_audio_sound_set_default_loop(audio,(int)N(args,count,0),N(args,count,1)!=0.0);
    return vreal(0);
  }
  if(!strcmp(name,"FAudioGMS_SoundInstance_SetPlayRegion")){
    int sound=(int)N(args,count,0);
    double start=N(args,count,1)/1000.0;
    gml_audio_sound_loop_start(audio,sound,start);
    gml_audio_sound_set_track_position(audio,sound,start);
    return vreal(0);
  }
  if(!strcmp(name,"FAudioGMS_SoundInstance_SetVolume")){
    gml_audio_sound_gain(audio,(int)N(args,count,0),N(args,count,1)); return vreal(0);
  }
  if(!strcmp(name,"FAudioGMS_SoundInstance_GetVolume"))
    return vreal(gml_audio_sound_get_gain(audio,(int)N(args,count,0)));
  if(!strcmp(name,"FAudioGMS_SoundInstance_SetVolumeOverTime")){
    gml_audio_sound_gain_fade(audio,(int)N(args,count,0),N(args,count,1),
                              (int)(N(args,count,2)*1000.0));
    return vreal(0);
  }
  if(!strcmp(name,"FAudioGMS_SoundInstance_SetPitch")){
    gml_audio_sound_pitch(audio,(int)N(args,count,0),N(args,count,1)); return vreal(0);
  }
  if(!strcmp(name,"FAudioGMS_SoundInstance_GetPitch"))
    return vreal(gml_audio_sound_get_pitch(audio,(int)N(args,count,0)));
  if(!strcmp(name,"FAudioGMS_SoundInstance_SetTrackPositionInSeconds")){
    gml_audio_sound_set_track_position(audio,(int)N(args,count,0),N(args,count,1));
    return vreal(0);
  }
  if(!strcmp(name,"FAudioGMS_SoundInstance_GetTrackPositionInSeconds"))
    return vreal(gml_audio_sound_get_track_position(audio,(int)N(args,count,0)));
  if(!strcmp(name,"FAudioGMS_SoundInstance_GetTrackLengthInSeconds"))
    return vreal(gml_audio_sound_length(audio,(int)N(args,count,0)));
  if(!strcmp(name,"FAudioGMS_SoundInstance_SetPan")){
    int sound=(int)N(args,count,0);
    gml_audio_voice_spatial(audio,sound,gml_audio_sound_get_gain(audio,sound),N(args,count,1));
    return vreal(0);
  }
  if(!strcmp(name,"FAudioGMS_SoundInstance_Set3DPosition")){
    int sound=(int)N(args,count,0);
    double dx=N(args,count,1)-vm->builtins->listener_x;
    double dy=N(args,count,2)-vm->builtins->listener_y;
    double dz=N(args,count,3)-vm->builtins->listener_z;
    double distance=sqrt(dx*dx+dy*dy+dz*dz);
    gml_audio_voice_spatial(audio,sound,gml_audio_sound_get_gain(audio,sound),
                            distance>0.0?dx/distance:0.0);
    return vreal(0);
  }
  if(!strcmp(name,"FAudioGMS_SetListenerPosition")){
    vm->builtins->listener_x=N(args,count,0);
    vm->builtins->listener_y=N(args,count,1);
    vm->builtins->listener_z=N(args,count,2);
    return vreal(0);
  }
  if(!strcmp(name,"FAudioGMS_PauseAll")){
    gml_audio_pause_all(audio,1); return vreal(0);
  }
  if(!strcmp(name,"FAudioGMS_ResumeAll")){
    gml_audio_pause_all(audio,0); return vreal(0);
  }
  if(!strcmp(name,"FAudioGMS_StopAll")){
    gml_audio_stop_all(audio); return vreal(0);
  }
  /* Effect chains, filters, velocity/orientation and queued synchronization have no exact mixer
   * equivalent yet. They retain valid deterministic control flow while the decoded sound and its
   * principal playback, gain, pitch, loop, position and pan controls remain functional. */
  if(!strncmp(name,"FAudioGMS_",10)) return vreal(0);
  return vundef();
}

static GmlVal builtin_gmwwise(GmlVM *vm,const char *name,GmlVal *args,int count){
  GmlWwiseState *state=wwise_state(vm);
  if(!state || !name) return vreal(0);
  if(!strcmp(name,"gmwInit")) return vreal(1);
  if(!strcmp(name,"gmwSetBasePath")){
    snprintf(state->base_path,sizeof state->base_path,"%s",S(vm,args,count,0));
    for(char *cursor=state->base_path;*cursor;cursor++) if(*cursor=='\\') *cursor='/';
    size_t length=strlen(state->base_path);
    while(length && state->base_path[length-1]=='/') state->base_path[--length]=0;
    return vreal(1);
  }
  if(!strcmp(name,"gmwLoadBank")) return vreal(wwise_load_bank(vm,S(vm,args,count,0)));
  if(!strcmp(name,"gmwUnloadBank")){
    const char *requested=S(vm,args,count,0);
    for(uint32_t index=0;index<state->bank_count;index++){
      const char *path=state->banks[index].path?state->banks[index].path:"";
      const char *base=strrchr(path,'/');
      if(strcmp(path,requested) && strcmp(base?base+1:path,requested)) continue;
      wwise_bank_free(&state->banks[index]);
      if(index+1u<state->bank_count)
        memmove(&state->banks[index],&state->banks[index+1u],
                (size_t)(state->bank_count-index-1u)*sizeof(state->banks[0]));
      state->bank_count--;
      memset(&state->banks[state->bank_count],0,sizeof(state->banks[0]));
      return vreal(1);
    }
    return vreal(0);
  }
  if(!strcmp(name,"gmwPostEvent"))
    return vreal(wwise_post_event(vm,wwise_argument_id(vm,args,count,0)));
  if(!strcmp(name,"gmwStop")||!strcmp(name,"gmwStopAll")){
    GmlAudio *audio=vm?(GmlAudio *)vm->audio:NULL;
    if(audio) for(uint32_t bank=0;bank<state->bank_count;bank++)
      for(uint32_t media=0;media<state->banks[bank].media_count;media++)
        if(state->banks[bank].media[media].sound>=0)
          gml_audio_stop(audio,state->banks[bank].media[media].sound);
    return vreal(1);
  }
  if(!strcmp(name,"gmwGetError")) return vstr("");
  if(!strcmp(name,"gmwGetParameter")) return vreal(0);
  /* These calls mutate Wwise's object/spatial/RTPC routing, for which AnyGM currently has no
   * equivalent graph. Accepting them is deliberate: the event/media graph remains playable and
   * the success result is stable, while no unavailable native service is implied. */
  if(!strcmp(name,"gmwRegisterObject")||!strcmp(name,"gmwUnregisterObject")||
     !strcmp(name,"gmwRegisterGroup")||!strcmp(name,"gmwUnregisterGroup")||
     !strcmp(name,"gmwSetParameter")||!strcmp(name,"gmwSetGlobalParameter")||
     !strcmp(name,"gmwSetSwitch")||!strcmp(name,"gmwSetState")||
     !strcmp(name,"gmwSet2DListenerPosition")||!strcmp(name,"gmwSet2DPosition")||
     !strcmp(name,"gmwSet3DListenerPosition")||!strcmp(name,"gmwSet3DPosition")||
     !strcmp(name,"gmwSetActiveListeners")||!strcmp(name,"gmwPostTrigger")||
     !strcmp(name,"gmwProcess")) return vreal(1);
  if(!strncmp(name,"gmw",3)) return vreal(0);
  return vundef();
}

GmlVal gml_builtin_try_audio(GmlVM *vm, const char *nm, GmlVal *a, int n){
  GmlRender *R=(GmlRender*)vm->render;
  (void)R;
  if(!strncmp(nm,"gmw",3)) return builtin_gmwwise(vm,nm,a,n);
  if(!strncmp(nm,"FAudioGMS_",10)) return builtin_faudio_gms(vm,nm,a,n);
  /* ---- audio ---- */
  { GmlAudio *AU=(GmlAudio*)vm->audio;
    if(!strcmp(nm,"action_sound")){ gml_audio_play(AU,(int)N(a,n,0),(int)N(a,n,1)); return vreal(0); }
    if(!strcmp(nm,"action_end_sound")){ gml_audio_stop(AU,(int)N(a,n,0)); return vreal(0); }
    if(!strcmp(nm,"audio_channel_num")){ gml_audio_channel_num(AU,(int)N(a,n,0)); return vreal(0); }
    if(!strcmp(nm,"sound_add")){
      const char *path=S(vm,a,n,0);
      return vreal(path&&*path?external_audio_load(vm,path):-1);
    }
    if(!strcmp(nm,"sound_delete")){
      external_audio_free(vm,(int)N(a,n,0));
      return vreal(0);
    }
    if(!strcmp(nm,"sound_discard")) return vreal(0);
    if(!strcmp(nm,"sound_exists"))
      return vreal(gml_audio_exists(AU,(int)N(a,n,0)));
    if(!strcmp(nm,"sound_restore")){
      (void)gml_audio_warm_sound(AU,(int)N(a,n,0));
      return vreal(0);
    }
    /* sound_replace(index, path, kind, loadonuse) reads encoded bytes through
     * the host VFS and records their identity against the requested sound slot
     * so state restoration retains the replacement. */
    if(!strcmp(nm,"sound_replace")){
      int sound=(int)N(a,n,0);
      const char *relative=S(vm,a,n,1);
      char *path=resolve_read_path(vm,relative);
      uint8_t *encoded=NULL; size_t size=0; uint8_t identity[32];
      int ok=sound>=0 && path &&
             anygm_vfs_read_all(vm->host,path,&encoded,&size,64u*1024u*1024u) &&
             size>0 && size<=INT_MAX &&
             gml_audio_replace_encoded(AU,sound,encoded,(int)size) &&
             gml_audio_sound_content_hash(AU,sound,identity) &&
             builtin_state_external_audio_store(builtin_state_ensure(vm),sound,relative,identity);
      if(builtin_setting(vm,"GML_LOG_AUDIO"))
        anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,
                        "[sound-replace] sound=%d ok=%d asked='%s' path='%s' bytes=%zu\n",
                        sound,ok,relative?relative:"",path?path:"",size);
      free(encoded); free(path);
      return vreal(ok);
    }
    if(!strcmp(nm,"audio_get_master_gain")) return vreal(gml_audio_get_master_gain(AU));
    if(!strcmp(nm,"audio_sound_get_gain")) return vreal(gml_audio_sound_get_gain(AU,(int)N(a,n,0)));
    if(!strcmp(nm,"audio_sound_get_pitch")) return vreal(gml_audio_sound_get_pitch(AU,(int)N(a,n,0)));
    if(!strcmp(nm,"audio_sound_length")||!strcmp(nm,"sound_get_length"))
      return vreal(gml_audio_sound_length(AU,(int)N(a,n,0)));
    if(!strcmp(nm,"audio_sound_get_track_position")) return vreal(gml_audio_sound_get_track_position(AU,(int)N(a,n,0)));
    if(!strcmp(nm,"audio_exists")) return vreal(gml_audio_exists(AU,(int)N(a,n,0)));
    if(!strcmp(nm,"audio_set_master_gain")){ gml_audio_set_master_gain(AU,n>=2?N(a,n,1):N(a,n,0)); return vreal(0); }
    if(!strcmp(nm,"audio_sound_gain")){ gml_audio_sound_gain(AU,(int)N(a,n,0),N(a,n,1)); return vreal(0); }
    if(!strcmp(nm,"sound_volume")){ gml_audio_sound_gain(AU,(int)N(a,n,0),N(a,n,1)); return vreal(0); }
    if(!strcmp(nm,"sound_fade")){
      double sound=N(a,n,0), gain=N(a,n,1), milliseconds=N(a,n,2);
      if(n<3 || !isfinite(sound) || sound<0 || sound>INT_MAX ||
         !isfinite(gain) || !isfinite(milliseconds) || milliseconds>INT_MAX)
        return vreal(0);
      if(!gml_audio_exists(AU,(int)sound) ||
         gml_audio_voice_sound(AU,(int)sound)>=0) return vreal(0);
      if(gain<0) gain=0;
      if(gain>1) gain=1;
      gml_audio_sound_gain_fade(AU,(int)sound,gain,
                                milliseconds>0?(int)milliseconds:0);
      return vreal(0);
    }
    /* The classic name uses the mixer's master gain over the 0..1 control range. */
    if(!strcmp(nm,"sound_global_volume")){ gml_audio_set_master_gain(AU,N(a,n,0)); return vreal(0); }
    if(!strcmp(nm,"audio_sound_pitch")){ gml_audio_sound_pitch(AU,(int)N(a,n,0),N(a,n,1)); return vreal(0); }
    if(!strcmp(nm,"audio_sound_set_track_position")){ gml_audio_sound_set_track_position(AU,(int)N(a,n,0),N(a,n,1)); return vreal(0); }
    if(!strcmp(nm,"audio_play_sound")||!strcmp(nm,"sound_play")||!strcmp(nm,"sound_loop")){
      int loop=!strcmp(nm,"sound_loop") ? 1 : (int)N(a,n,2);
      int h=gml_audio_play(AU,(int)N(a,n,0),loop);
      if(builtin_setting(vm,"GML_LOG_AUDIO")) anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[audio] play_sound id=%d loop=%d -> handle=%d\n",(int)N(a,n,0),loop,h);
      return vreal(h); }
    if(!strcmp(nm,"audio_play_sound_at")){
      /* (soundid, x, y, z, falloff_ref, falloff_max, falloff_factor, loop, priority[, listener])
       * The three distance parameters sit between the position and the loop flag, so the flag is
       * the eighth argument. Reading the factor in its place makes a one-shot repeat for as long
       * as the room lasts, because a falloff factor is normally one. */
      double falloff_factor=N(a,n,6);
      int h=gml_audio_play(AU,(int)N(a,n,0),(int)N(a,n,7));
      if(h>0){
        double dx=N(a,n,1)-vm->builtins->listener_x, dy=N(a,n,2)-vm->builtins->listener_y, dz=N(a,n,3)-vm->builtins->listener_z;
        double dist=sqrt(dx*dx+dy*dy+dz*dz), ref=N(a,n,4), maxd=N(a,n,5), gain=1.0;
        if(vm->builtins->audio_falloff_model && maxd>=ref && ref>0.0){
          double old_ref=vm->builtins->emitter_ref[0], old_max=vm->builtins->emitter_max[0], old_factor=vm->builtins->emitter_factor[0];
          vm->builtins->emitter_ref[0]=ref; vm->builtins->emitter_max[0]=maxd;
          vm->builtins->emitter_factor[0]=falloff_factor>0.0?falloff_factor:1.0;
          gain=audio_emitter_attenuation(vm,0,dist);
          vm->builtins->emitter_ref[0]=old_ref; vm->builtins->emitter_max[0]=old_max; vm->builtins->emitter_factor[0]=old_factor;
        }
        double pan=dist>0.0?dx/dist:0.0; gml_audio_voice_spatial(AU,h,gain,pan);
      }
      if(builtin_setting(vm,"GML_LOG_AUDIO")) anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[audio] play_sound_at id=%d loop=%d -> handle=%d\n",(int)N(a,n,0),(int)N(a,n,7),h);
      return vreal(h); }
    if(!strcmp(nm,"audio_sound_loop_start")){ gml_audio_sound_loop_start(AU,(int)N(a,n,0),N(a,n,1)); return vreal(0); }
    if(!strcmp(nm,"audio_listener_position")){
      vm->builtins->listener_x=N(a,n,0); vm->builtins->listener_y=N(a,n,1); vm->builtins->listener_z=N(a,n,2);
      audio_refresh_emitters(vm,AU); return vreal(0); }
    if(!strcmp(nm,"audio_listener_orientation")){
      vm->builtins->listener_forward_x=N(a,n,0); vm->builtins->listener_forward_y=N(a,n,1); vm->builtins->listener_forward_z=N(a,n,2);
      vm->builtins->listener_up_x=N(a,n,3); vm->builtins->listener_up_y=N(a,n,4); vm->builtins->listener_up_z=N(a,n,5);
      audio_refresh_emitters(vm,AU); return vreal(0); }
    if(!strcmp(nm,"audio_falloff_set_model")){
      int model=(int)N(a,n,0); vm->builtins->audio_falloff_model=(model>=0&&model<=6)?model:0;
      audio_refresh_emitters(vm,AU); return vreal(0); }
    if(!strcmp(nm,"audio_get_listener_count")) return vreal(1);
    if(!strcmp(nm,"audio_get_listener_info")) return arr8(0,0,0,0,0,1,0,1);
    /* Audio group sidecars are loaded with content, so report each available group as complete. */
    if(!strcmp(nm,"audio_group_is_loaded")) return vreal(1);
    if(!strcmp(nm,"audio_group_load_progress")) return vreal(1.0);
    if(!strcmp(nm,"audio_group_load")||!strcmp(nm,"audio_group_unload")) return vreal(1);
    if(!strcmp(nm,"audio_group_stop_all")){ gml_audio_group_stop_all(AU,(int)N(a,n,0)); return vreal(0); }
    if(!strcmp(nm,"audio_stop_sound")||!strcmp(nm,"sound_stop")){ gml_audio_stop(AU,(int)N(a,n,0)); return vreal(0); }
    if(!strcmp(nm,"audio_stop_all")||!strcmp(nm,"sound_stop_all")){ gml_audio_stop_all(AU); return vreal(0); }
    if(!strcmp(nm,"audio_pause_sound")){ gml_audio_pause_sound(AU,(int)N(a,n,0),1); return vreal(0); }
    if(!strcmp(nm,"audio_resume_sound")){ gml_audio_pause_sound(AU,(int)N(a,n,0),0); return vreal(0); }
    if(!strcmp(nm,"audio_pause_all")){ gml_audio_pause_all(AU,1); return vreal(0); }
    if(!strcmp(nm,"audio_resume_all")){ gml_audio_pause_all(AU,0); return vreal(0); }
    if(!strcmp(nm,"audio_is_playing")||!strcmp(nm,"sound_isplaying")||
       !strcmp(nm,"action_if_sound"))
      return vreal(gml_audio_is_playing(AU,(int)N(a,n,0)));

    /* Streams use the existing external-audio loader. An unavailable stream returns -1 so it
     * cannot alias the valid sound index zero. */
    if(!strcmp(nm,"audio_create_stream")){
      const char *path=S(vm,a,n,0);
      int handle=path&&*path ? external_audio_load(vm,path) : -1;
      if(builtin_setting(vm,"GML_LOG_AUDIO"))
        anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,
                        "[audio] create_stream %s handle=%d\n",path?path:"",handle);
      return vreal(handle);
    }
    if(!strcmp(nm,"audio_destroy_stream")){
      int handle=(int)N(a,n,0);
      if(handle>=0) external_audio_free(vm,handle);
      return vreal(0);
    }

    /* ---- caster_* : external Ogg streaming. caster_load("music/X.ogg") resolves the exported
     * "mus_X.ogg" sidecar in the content directory; the resulting sound handle is reused by the
     * regular voice API. */
    if(!strcmp(nm,"caster_load")){
      const char *arg=S(vm,a,n,0);
      const char *base=strrchr(arg,'/'); base=base?base+1:arg;
      char mus[4097];
      int written=snprintf(mus,sizeof mus,"mus_%s",base);
      int handle=written>0 && (size_t)written<sizeof(mus)
        ? external_audio_load_hashed_mode(vm,mus,NULL,1) : -1;
      if(builtin_setting(vm,"GML_LOG_AUDIO"))
        anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,
                        "[caster] load %s -> %s handle=%d\n",
                        arg,written>0 && (size_t)written<sizeof(mus)?mus:"?",handle);
      return vreal(handle);
    }
    if(!strcmp(nm,"caster_play")||!strcmp(nm,"caster_play_l")||!strcmp(nm,"caster_loop")){
      int h=(int)N(a,n,0); double vol=n>=2?N(a,n,1):1.0, pit=n>=3?N(a,n,2):1.0;
      int loop=strcmp(nm,"caster_play")!=0;   /* caster_loop / caster_play_l repeat */
      gml_audio_sound_gain(AU,h,vol); gml_audio_sound_pitch(AU,h,pit);
      return vreal(gml_audio_play(AU,h,loop));
    }
    if(!strcmp(nm,"caster_stop")){ int h=(int)N(a,n,0); if(h<0) gml_audio_stop_all(AU); else gml_audio_stop(AU,h); return vreal(0); }
    if(!strcmp(nm,"caster_free")){ int h=(int)N(a,n,0); if(h<0) external_audio_free_all(vm); else external_audio_free(vm,h); return vreal(0); }
    if(!strcmp(nm,"caster_set_volume")){ gml_audio_sound_gain(AU,(int)N(a,n,0),N(a,n,1)); return vreal(0); }
    if(!strcmp(nm,"caster_get_volume")) return vreal(gml_audio_sound_get_gain(AU,(int)N(a,n,0)));
    if(!strcmp(nm,"caster_set_pitch")){ gml_audio_sound_pitch(AU,(int)N(a,n,0),N(a,n,1)); return vreal(0); }
    if(!strcmp(nm,"caster_get_pitch")) return vreal(gml_audio_sound_get_pitch(AU,(int)N(a,n,0)));
    if(!strcmp(nm,"caster_pause")){ int h=(int)N(a,n,0); if(h<0) gml_audio_pause_all(AU,1); else gml_audio_pause_sound(AU,h,1); return vreal(0); }
    if(!strcmp(nm,"caster_resume")){ int h=(int)N(a,n,0); if(h<0) gml_audio_pause_all(AU,0); else gml_audio_pause_sound(AU,h,0); return vreal(0); }
    if(!strcmp(nm,"caster_is_playing")) return vreal(gml_audio_is_playing(AU,(int)N(a,n,0)));
    if(!strncmp(nm,"caster_",7)) return vreal(0);   /* set_panning / get_length / etc.: graceful no-op */
    /* Audio emitters are modeled as gain cells with create, gain, query, free, and existence
     * operations. Retaining the gain value supports runtime ducking envelopes. */
    if(!strcmp(nm,"audio_emitter_create")){
      for(int i=0;i<GML_MAX_EMITTERS;i++) if(!vm->builtins->emitter_live[i]){ vm->builtins->emitter_live[i]=1; vm->builtins->emitter_gain[i]=1.0;
        vm->builtins->emitter_x[i]=vm->builtins->emitter_y[i]=vm->builtins->emitter_z[i]=0.0;
        vm->builtins->emitter_ref[i]=100.0; vm->builtins->emitter_max[i]=100000.0; vm->builtins->emitter_factor[i]=1.0;
        return vreal(3000000+i); }
      return vreal(-1); }
    if(!strcmp(nm,"audio_emitter_gain")){ int e=(int)N(a,n,0)-3000000;
      if(e>=0&&e<GML_MAX_EMITTERS&&vm->builtins->emitter_live[e]){ double g=N(a,n,1); vm->builtins->emitter_gain[e]=g<0?0:g; audio_refresh_emitter(vm,AU,e); } return vreal(0); }
    if(!strcmp(nm,"audio_emitter_get_gain")){ int e=(int)N(a,n,0)-3000000;
      return vreal((e>=0&&e<GML_MAX_EMITTERS&&vm->builtins->emitter_live[e])?vm->builtins->emitter_gain[e]:1.0); }
    if(!strcmp(nm,"audio_emitter_free")){ int e=(int)N(a,n,0)-3000000;
      if(e>=0&&e<GML_MAX_EMITTERS) vm->builtins->emitter_live[e]=0;
      return vreal(0); }
    if(!strcmp(nm,"audio_emitter_exists")){ int e=(int)N(a,n,0)-3000000;
      return vreal(e>=0&&e<GML_MAX_EMITTERS&&vm->builtins->emitter_live[e]); }
    if(!strcmp(nm,"audio_emitter_position")){ int e=(int)N(a,n,0)-3000000;
      if(e>=0&&e<GML_MAX_EMITTERS&&vm->builtins->emitter_live[e]){ vm->builtins->emitter_x[e]=N(a,n,1); vm->builtins->emitter_y[e]=N(a,n,2);
        vm->builtins->emitter_z[e]=N(a,n,3); audio_refresh_emitter(vm,AU,e); } return vreal(0); }
    if(!strcmp(nm,"audio_emitter_falloff")){ int e=(int)N(a,n,0)-3000000;
      if(e>=0&&e<GML_MAX_EMITTERS&&vm->builtins->emitter_live[e]){ vm->builtins->emitter_ref[e]=N(a,n,1); vm->builtins->emitter_max[e]=N(a,n,2);
        vm->builtins->emitter_factor[e]=N(a,n,3); audio_refresh_emitter(vm,AU,e); } return vreal(0); }
    if(!strcmp(nm,"audio_play_sound_on")){   /* (emitter, snd, loop, priority) -> play scaled by emitter gain */
      int e=(int)N(a,n,0)-3000000; int h=gml_audio_play_on(AU,(int)N(a,n,1),(int)N(a,n,2),e);
      if(h>0&&e>=0&&e<GML_MAX_EMITTERS&&vm->builtins->emitter_live[e]) audio_refresh_emitter(vm,AU,e);
      return vreal(h); }
    if(!strcmp(nm,"audio_is_paused")){
      /* paused state of a sound/voice: true only if some matching voice exists and is paused */
      return vreal(gml_audio_voice_paused(AU,(int)N(a,n,0))); }
    if(!strcmp(nm,"audio_get_type")) return vreal(0);          /* 0 = in-memory sample (all ours are) */
    if(!strcmp(nm,"audio_get_name")) return vstr("");
    if(!strcmp(nm,"audio_master_gain")){ gml_audio_set_master_gain(AU,N(a,n,0)); return vreal(0); }
    if(!strcmp(nm,"audio_group_get_gain")) return vreal(gml_audio_group_get_gain(AU,(int)N(a,n,0)));
    if(!strcmp(nm,"audio_group_set_gain")){
      gml_audio_group_gain(AU,(int)N(a,n,0),N(a,n,1),(int)N(a,n,2)); return vreal(0); }
  }

  return gml_builtin_try_instances_paths(vm,nm,a,n);
}
