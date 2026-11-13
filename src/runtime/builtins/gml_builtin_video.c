/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "gml_builtin_internal.h"
#include "gml_video_player.h"
#include "anygm_vfs.h"
#include "anygm_host.h"
#include "gml_hash.h"

#include <string.h>

struct GmlBuiltinVideoSource {
  uint8_t *bytes;
  size_t size,references;
  char *path;
  uint8_t digest[32];
};
typedef struct {
  GmlVideoPlayer *player;
  GmlBuiltinVideoSource *source;
  int skip_pending;
} GmlBuiltinVideo;

static void video_notify(GmlVM *vm,const char *type);

void gml_builtin_video_source_release(GmlBuiltinVideoSource *source){
  if(source && --source->references==0){free(source->bytes);free(source->path);free(source);}
}
GmlBuiltinVideoSource *gml_builtin_video_checkpoint(GmlVM *vm){
  GmlBuiltinVideo *video=vm && vm->builtins?vm->builtins->video:NULL;
  if(!video || !video->source) return NULL;
  video->source->references++;return video->source;
}
void gml_builtin_video_rebind(GmlVM *vm){
  GmlBuiltinVideo *video=vm && vm->builtins?vm->builtins->video:NULL;
  if(video) gml_video_player_rebind(video->player,vm->render,vm->audio);
}
int gml_builtin_video_finish_restore(GmlVM *vm){
  GmlBuiltinVideo *video=vm && vm->builtins?vm->builtins->video:NULL;
  return !video || gml_video_player_state_adopt(video->player);
}
size_t gml_builtin_video_state_capacity_hint(const GmlVM *vm){
  const GmlBuiltinVideo *video=vm && vm->builtins?vm->builtins->video:NULL;
  return video?gml_video_player_state_capacity_hint(video->player):0;
}

void gml_builtin_video_close(GmlVM *vm){
  GmlBuiltinState *state=vm?vm->builtins:NULL;
  GmlBuiltinVideo *video=state?state->video:NULL;
  if(!video) return;
  state->video=NULL;state->video_generation++;
  gml_video_player_close(video->player);
  gml_builtin_video_source_release(video->source);free(video);
}
static GmlBuiltinVideoSource *source_open(GmlVM *vm,const char *path,const uint8_t *expected){
  if(!path || !*path || strlen(path)>4096u) return NULL;
  GmlBuiltinVideoSource *cached=vm->video_restore_source;
  if(expected && cached && !strcmp(cached->path,path) && !memcmp(cached->digest,expected,32)){
    cached->references++;return cached;
  }
  char *resolved=resolve_read_path(vm,path);
  GmlBuiltinVideoSource *source=calloc(1,sizeof *source);
  if(!source){free(resolved);return NULL;}
  source->references=1;
  int loaded=resolved && anygm_vfs_read_all(vm->host,resolved,&source->bytes,&source->size,
                                           256u*1024u*1024u);
  free(resolved);
  if(loaded && source->size) source->path=strdup(path);
  if(!source->path){gml_builtin_video_source_release(source);return NULL;}
  gml_sha256(source->bytes,source->size,source->digest);
  if(expected && memcmp(source->digest,expected,32)){
    gml_builtin_video_source_release(source);return NULL;
  }
  return source;
}
static void video_skip(GmlVM *vm,const char *reason){
  GmlBuiltinVideo *video=calloc(1,sizeof *video);
  if(!video) return;
  video->skip_pending=1;
  vm->builtins->video=video;vm->builtins->video_generation++;
  if(vm->builtins->video_skip_log_count++<4)
    anygm_host_logf(vm->host,ANYGM_LOG_WARN,
      "Video playback unavailable; skipping the requested movie and completing it: %s\n",
      reason&&reason[0]?reason:"unknown provider failure");
}
static void video_open(GmlVM *vm,const char *path){
  gml_builtin_video_close(vm);
  if(!vm->render) return;
  if(!gml_video_codec_available()){
    video_skip(vm,gml_video_codec_provider_error());return;
  }
  GmlBuiltinVideoSource *source=source_open(vm,path,NULL);
  if(!source){video_skip(vm,"the media source could not be opened");return;}
  GmlBuiltinVideo *video=calloc(1,sizeof *video);
  if(!video){gml_builtin_video_source_release(source);return;}
  video->source=source;
  video->player=gml_video_player_open(source->bytes,source->size,vm->render,vm->audio);
  if(!video->player){
    gml_builtin_video_source_release(source);free(video);
    video_skip(vm,"the media stream could not be decoded");return;
  }
  vm->builtins->video=video;vm->builtins->video_generation++;
}
GmlVal gml_builtin_try_video(GmlVM *vm,const char *name,GmlVal *args,int count){
  GmlBuiltinVideo *video=vm && vm->builtins?vm->builtins->video:NULL;
  GmlVideoPlayer *player=video?video->player:NULL;
  if(!strcmp(name,"video_open")){
    video_open(vm,count>0 && args[0].t==V_STR?args[0].s:NULL);return vundef();
  }
  if(!strcmp(name,"video_close")){gml_builtin_video_close(vm);return vundef();}
  if(!strcmp(name,"video_pause")){gml_video_player_pause(player,1);return vundef();}
  if(!strcmp(name,"video_resume")){gml_video_player_pause(player,0);return vundef();}
  if(!strcmp(name,"video_set_volume")){
    if(count>0) gml_video_player_volume(player,N(args,count,0));
    return vundef();
  }
  if(!strcmp(name,"video_enable_loop")){
    if(count>0) gml_video_player_loop(player,N(args,count,0)!=0);
    return vundef();
  }
  if(!strcmp(name,"video_get_status")) return vreal(gml_video_player_status(player));
  if(!strcmp(name,"video_get_format")) return vreal(GML_VIDEO_FORMAT_RGBA);
  if(!strcmp(name,"video_get_duration")) return vreal(gml_video_player_duration(player));
  if(!strcmp(name,"video_get_position")) return vreal(gml_video_player_position(player));
  if(!strcmp(name,"video_draw")){
    int surface=gml_video_player_surface(player);
    GmlVal result=gml_arr_new(2,vreal(-1));
    gml_arr_set(result,0,vreal(surface>=0?0:surface));
    if(surface>=0) gml_arr_set(result,1,vreal(surface));
    return result;
  }
  return gml_builtin_try_instances_paths(vm,name,args,count);
}
static void video_notify(GmlVM *vm,const char *type){
  int map=ds_map_create_id(vm);
  if(map<0) return;
  if(!ds_map_put_static_string(vm,map,"type",type) ||
     !ds_map_put_static_string(vm,map,"event_type",type) ||
     !ds_map_put_static_key(vm,map,"status",vreal(0),1)){
    ds_map_destroy_id(vm,map);return;
  }
  builtin_fire_async_map(vm,map,"Other_70");
}
void gml_builtin_video_tick(GmlVM *vm){
  GmlBuiltinState *state=vm?vm->builtins:NULL;
  GmlBuiltinVideo *video=state?state->video:NULL;
  if(!video) return;
  if(video->skip_pending){
    /* Remove the transient slot before dispatch. A Social handler may sleep and expose a public
     * frame boundary; that state must contain the callback payload, not an unserializable player. */
    state->video=NULL;state->video_generation++;
    free(video);video_notify(vm,"video_end");return;
  }
  gml_video_player_tick(video->player,1000.0/gml_room_speed(vm));
  unsigned notices=gml_video_player_notifications(video->player);
  uint64_t generation=state->video_generation;
  if(notices&GML_VIDEO_NOTIFY_START) video_notify(vm,"video_start");
  if(state->video_generation==generation && (notices&GML_VIDEO_NOTIFY_END))
    video_notify(vm,"video_end");
}

int gml_builtin_video_state_get(const GmlBuiltinState *state,const char **path,
                                const uint8_t **digest,GmlVideoPlayerState *saved){
  const GmlBuiltinVideo *video=state?state->video:NULL;
  if(!video || !video->source || !video->player || !path || !digest || !saved) return 0;
  *path=video->source->path;*digest=video->source->digest;
  return gml_video_player_state_get(video->player,saved);
}
int gml_builtin_video_state_restore(GmlBuiltinState *state,const char *path,
                                    const uint8_t digest[32],const GmlVideoPlayerState *saved){
  GmlBuiltinVideoSource *source=source_open(state->vm,path,digest);
  GmlBuiltinVideo *video=NULL;
  if(source){
    video=calloc(1,sizeof *video);
    if(video){
      video->source=source;
      video->player=gml_video_player_state_restore(source->bytes,source->size,
                                                  state->vm->render,state->vm->audio,saved);
    }
  }
  if(!video || !video->player){
    if(video) gml_video_player_close(video->player);
    free(video);gml_builtin_video_source_release(source);return 0;
  }
  state->video=video;state->video_generation++;return 1;
}
