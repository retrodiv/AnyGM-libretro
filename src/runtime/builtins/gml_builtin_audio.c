/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Audio builtin argument adaptation, spatial-emitter policy, and ordered dispatch. */
#include "gml_builtin_internal.h"
#include "gml_render.h"
#include "gml_audio.h"
#include "anygm_host.h"
#include "anygm_vfs.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

GmlVal gml_builtin_try_audio(GmlVM *vm, const char *nm, GmlVal *a, int n){
  GmlRender *R=(GmlRender*)vm->render;
  (void)R;
  /* ---- audio ---- */
  { GmlAudio *AU=(GmlAudio*)vm->audio;
    if(!strcmp(nm,"action_sound")){ gml_audio_play(AU,(int)N(a,n,0),(int)N(a,n,1)); return vreal(0); }
    if(!strcmp(nm,"action_end_sound")){ gml_audio_stop(AU,(int)N(a,n,0)); return vreal(0); }
    if(!strcmp(nm,"audio_channel_num")){ gml_audio_channel_num(AU,(int)N(a,n,0)); return vreal(0); }
    if(!strcmp(nm,"sound_add")) return vreal(-1);
    if(!strcmp(nm,"sound_replace")) return vreal(0);
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
    if(!strcmp(nm,"audio_sound_pitch")){ gml_audio_sound_pitch(AU,(int)N(a,n,0),N(a,n,1)); return vreal(0); }
    if(!strcmp(nm,"audio_sound_set_track_position")){ gml_audio_sound_set_track_position(AU,(int)N(a,n,0),N(a,n,1)); return vreal(0); }
    if(!strcmp(nm,"audio_play_sound")||!strcmp(nm,"sound_play")||!strcmp(nm,"sound_loop")){
      int loop=!strcmp(nm,"sound_loop") ? 1 : (int)N(a,n,2);
      int h=gml_audio_play(AU,(int)N(a,n,0),loop);
      if(builtin_setting(vm,"GML_LOG_AUDIO")) anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[audio] play_sound id=%d loop=%d -> handle=%d\n",(int)N(a,n,0),loop,h);
      return vreal(h); }
    if(!strcmp(nm,"audio_play_sound_at")){
      int h=gml_audio_play(AU,(int)N(a,n,0),(int)N(a,n,6));
      if(h>0){
        double dx=N(a,n,1)-vm->builtins->listener_x, dy=N(a,n,2)-vm->builtins->listener_y, dz=N(a,n,3)-vm->builtins->listener_z;
        double dist=sqrt(dx*dx+dy*dy+dz*dz), ref=N(a,n,4), maxd=N(a,n,5), gain=1.0;
        if(vm->builtins->audio_falloff_model && maxd>=ref && ref>0.0){
          double old_ref=vm->builtins->emitter_ref[0], old_max=vm->builtins->emitter_max[0], old_factor=vm->builtins->emitter_factor[0];
          vm->builtins->emitter_ref[0]=ref; vm->builtins->emitter_max[0]=maxd; vm->builtins->emitter_factor[0]=1.0;
          gain=audio_emitter_attenuation(vm,0,dist);
          vm->builtins->emitter_ref[0]=old_ref; vm->builtins->emitter_max[0]=old_max; vm->builtins->emitter_factor[0]=old_factor;
        }
        double pan=dist>0.0?dx/dist:0.0; gml_audio_voice_spatial(AU,h,gain,pan);
      }
      if(builtin_setting(vm,"GML_LOG_AUDIO")) anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[audio] play_sound_at id=%d loop=%d -> handle=%d\n",(int)N(a,n,0),(int)N(a,n,6),h);
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
    if(!strcmp(nm,"audio_is_playing")||!strcmp(nm,"sound_isplaying")) return vreal(gml_audio_is_playing(AU,(int)N(a,n,0)));

    /* ---- caster_* : external Ogg streaming. caster_load("music/X.ogg") resolves the exported
     * "mus_X.ogg" sidecar in the content directory; the resulting sound handle is reused by the
     * regular voice API. */
    if(!strcmp(nm,"caster_load")){
      const char *arg=S(vm,a,n,0);
      const char *base=strrchr(arg,'/'); base=base?base+1:arg;
      char mus[300]; snprintf(mus,sizeof mus,"mus_%s",base);
      char *full=resolve_read_path(vm,mus); int handle=-1;
      uint8_t *data=NULL; size_t size=0;
      if(full && anygm_vfs_read_all(vm->host,full,&data,&size,64u*1024u*1024u) &&
         size>0 && size<=INT_MAX) handle=gml_audio_add_ogg(AU,data,(int)size);
      free(data);
      if(builtin_setting(vm,"GML_LOG_AUDIO")) anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[caster] load %s -> %s handle=%d\n",arg,full?full:"?",handle);
      free(full); return vreal(handle);
    }
    if(!strcmp(nm,"caster_play")||!strcmp(nm,"caster_play_l")||!strcmp(nm,"caster_loop")){
      int h=(int)N(a,n,0); double vol=n>=2?N(a,n,1):1.0, pit=n>=3?N(a,n,2):1.0;
      int loop=strcmp(nm,"caster_play")!=0;   /* caster_loop / caster_play_l repeat */
      gml_audio_sound_gain(AU,h,vol); gml_audio_sound_pitch(AU,h,pit);
      return vreal(gml_audio_play(AU,h,loop));
    }
    if(!strcmp(nm,"caster_stop")){ int h=(int)N(a,n,0); if(h<0) gml_audio_stop_all(AU); else gml_audio_stop(AU,h); return vreal(0); }
    if(!strcmp(nm,"caster_free")){ int h=(int)N(a,n,0); if(h<0) gml_audio_caster_free_all(AU); else gml_audio_caster_free(AU,h); return vreal(0); }
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
