/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* gml_vm_exec.c — bytecode execution, variables, and code-cache ownership. */
#include "gml_vm.h"
#include "gml_vm_internal.h"
#include "gml_value_internal.h"
#include "anygm_compatibility.h"
#include "gml_builtin.h"
#include "gml_render.h"
#include "gml_audio.h"
#include "gml_particle.h"
#include "anygm_host.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int inst_is_struct_ref(const GmlInstance *in);
static void method_cache_invalidate(GmlInstance *in, const char *nm);
static GmlVal *struct_field_get_h(GmlVM *vm, GmlInstance *in, const char *nm, uint32_t nh);
enum {
  GML_HASH_METHOD_FN   = 0x35836763u,
  GML_HASH_METHOD_SELF = 0x61c0232du,
  GML_HASH_CONSTRUCTOR = 0x44f7d3e1u
};

/* ---------------- helpers ---------------- */
static double asnum(GmlVal v){ return v.t==V_REAL? v.d : (v.s? atof(v.s):0); }
static int    astrue(GmlVal v){ return v.t==V_REAL? (v.d>=0.5) : (v.s&&v.s[0]); } /* GM: real>=0.5 true */
double gml_vm_value_as_number(GmlVal value){ return asnum(value); }
int gml_vm_value_is_true(GmlVal value){ return astrue(value); }
static const char *asstr_cmp(GmlVal v, char *buf, size_t n){
  if(v.t==V_STR) return v.s?v.s:"";
  if(v.t==V_UNDEF) return "";
  snprintf(buf,n,"%g",v.t==V_REAL?v.d:0.0);
  return buf;
}
static void log_val_simple(GmlVM *vm,GmlVal v){
  if(v.t==V_STR){
    const char *s=v.s?v.s:"";
    anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"\"");
    char excerpt[97];
    int length=0;
    while(s[length] && length<96){
      excerpt[length]=(s[length]=='\n'||s[length]=='\r')?' ':s[length];
      length++;
    }
    excerpt[length]=0;
    anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"%s",excerpt);
    if(strlen(s)>96) anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"...");
    anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"\"");
  } else if(v.t==V_ARR){
    GmlArr *A=(GmlArr*)v.arr;
    anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"<array len=%d",A?A->len:0);
    if(A){
      int n=A->len<5?A->len:5;
      for(int i=0;i<n;i++){ anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG," "); log_val_simple(vm,A->data[i]); }
      if(A->len>n) anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG," ...");
    }
    anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,">");
  }
  else if(v.t==V_UNDEF) anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"undefined");
  else anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"%.17g",v.t==V_REAL?v.d:0.0);
}
static void motion_from_components(GmlVM *vm, GmlInstance *in){
  in->speed=hypot(in->hspeed,in->vspeed);
  in->direction=atan2(-in->vspeed,in->hspeed)*180.0/M_PI;
  if(vm && vm->win && anygm_policy_uses_classic_runtime(vm->win)){
    in->direction=fmod(in->direction,360.0);
    if(in->direction<0) in->direction+=360.0;
    double rounded=round(in->direction);
    if(fabs(rounded-in->direction)<0.0001) in->direction=rounded;
    if(in->direction>=360.0) in->direction-=360.0;
  }
}
static void motion_from_speed_dir(GmlVM *vm, GmlInstance *in){
  if(vm && vm->win && anygm_policy_uses_classic_runtime(vm->win)){
    in->direction=fmod(in->direction,360.0);
    if(in->direction<0) in->direction+=360.0;
  }
  in->hspeed=in->speed*cos(in->direction*M_PI/180.0);
  in->vspeed=-in->speed*sin(in->direction*M_PI/180.0);
  if(vm && vm->win && anygm_policy_uses_classic_runtime(vm->win)){
    double rounded=round(in->hspeed);
    if(fabs(rounded-in->hspeed)<0.0001) in->hspeed=rounded;
    rounded=round(in->vspeed);
    if(fabs(rounded-in->vspeed)<0.0001) in->vspeed=rounded;
  }
}
void gml_vm_motion_from_components(GmlVM *vm, GmlInstance *instance){
  motion_from_components(vm,instance);
}
void gml_vm_motion_from_speed_direction(GmlVM *vm, GmlInstance *instance){
  motion_from_speed_dir(vm,instance);
}

/* ---------------- builtin instance variables ---------------- */
/* returns 1 if name is a builtin and handled */
static int inst_builtin_get(GmlVM *vm, GmlInstance *in, const char *n, GmlVal *out){
  if(!strcmp(n,"image_single")){ *out=vreal(in->image_speed==0? in->image_index : -1); return 1; }
  if(!strcmp(n,"layer")){
    if(vm && in->draw_layer_order>=0){
      for(int i=0;i<vm->n_rtl;i++){
        GmlRtLayer *layer=&vm->rtl[i];
        if(layer->used && layer->order==in->draw_layer_order){ *out=vreal(layer->id); return 1; }
      }
    }
    *out=vreal(-1); return 1;
  }
  #define B(name,field) if(!strcmp(n,name)){ *out=vreal(in->field); return 1; }
  B("x",x) B("y",y) B("xprevious",xprevious) B("yprevious",yprevious)
  B("phy_position_x",x) B("phy_position_y",y)
  B("xstart",xstart) B("ystart",ystart)
  B("sprite_index",sprite_index) B("mask_index",mask_index) B("image_index",image_index) B("image_speed",image_speed)
  B("image_xscale",image_xscale) B("image_yscale",image_yscale) B("image_angle",image_angle)
  B("image_alpha",image_alpha) B("image_blend",image_blend)
  B("depth",depth) B("visible",visible) B("solid",solid) B("persistent",persistent)
  if(!strcmp(n,"hspeed") || !strcmp(n,"vspeed")){
    double d=!strcmp(n,"hspeed")?in->hspeed:in->vspeed;
    if(vm && vm->win && anygm_policy_uses_classic_runtime(vm->win) && fabs(d)<1e-12) d=0;
    *out=vreal(d); return 1;
  }
  B("direction",direction) B("speed",speed)
  B("gravity",gravity) B("gravity_direction",gravity_direction) B("friction",friction)
  B("path_index",path_index) B("path_position",path_position) B("path_speed",path_speed)
  B("path_orientation",path_orientation) B("path_scale",path_scale)
  B("path_positionprevious",path_positionprevious) B("path_endaction",path_endaction)
  B("timeline_index",timeline_index) B("timeline_position",timeline_position)
  B("timeline_speed",timeline_speed) B("timeline_running",timeline_running) B("timeline_loop",timeline_loop)
  #undef B
  if(!strcmp(n,"object_index")){ *out=vreal(in->obj); return 1; }
  if(!strcmp(n,"id")){ *out=vreal(in->id); return 1; }
  return 0;
}
static int inst_builtin_set(GmlVM *vm, GmlInstance *in, const char *n, GmlVal v){
  double d=asnum(v);
  #define B(name,field) if(!strcmp(n,name)){ in->field=d; return 1; }
  #define BT(name,field) if(!strcmp(n,name)){ in->field=d; gml_colgrid_touch(vm,in); return 1; }  /* bbox input */
  /* legacy GM7/8 image_single (GMS1.4 compat): >=0 locks the shown subimage (index=v, speed=0);
   * -1 resumes animation. */
  if(!strcmp(n,"image_single")){
    if(d>=0){ in->image_index=d; in->image_speed=0; }
    else in->image_speed=1;
    return 1; }
  BT("x",x) BT("y",y) BT("phy_position_x",x) BT("phy_position_y",y)
  B("xprevious",xprevious) B("yprevious",yprevious)
  B("xstart",xstart) B("ystart",ystart)
  if(!strcmp(n,"sprite_index")){
    in->sprite_index=d; gml_colgrid_touch(vm,in);
    /* Queue the new sprite atlas for background decoding because a swap can be followed by a draw
     * from a not-yet-decoded page in the same frame. */
    if(vm && vm->render && (int)d>=0)
      gml_render_prefetch_sprite((GmlRender*)vm->render,(int)d);
    return 1; }
  BT("mask_index",mask_index) B("image_index",image_index) B("image_speed",image_speed)
  BT("image_xscale",image_xscale) BT("image_yscale",image_yscale) BT("image_angle",image_angle)
  B("image_alpha",image_alpha) B("image_blend",image_blend)
  #undef BT
  B("depth",depth) B("visible",visible) B("solid",solid) B("persistent",persistent)
  B("gravity",gravity) B("gravity_direction",gravity_direction) B("friction",friction)
  B("path_position",path_position) B("path_speed",path_speed)
  B("path_orientation",path_orientation) B("path_scale",path_scale)
  B("path_positionprevious",path_positionprevious) B("path_endaction",path_endaction)
  B("timeline_position",timeline_position) B("timeline_speed",timeline_speed)
  B("timeline_running",timeline_running) B("timeline_loop",timeline_loop)
  #undef B
  if(!strcmp(n,"path_index")){ in->path_index=d; return 1; }   /* set directly = follow that path */
  if(!strcmp(n,"timeline_index")){ in->timeline_index=d; return 1; }
  /* speed/direction/hspeed/vspeed are linked in GM */
  if(!strcmp(n,"hspeed")){ in->hspeed=d; motion_from_components(vm,in); return 1; }
  if(!strcmp(n,"vspeed")){ in->vspeed=d; motion_from_components(vm,in); return 1; }
  if(!strcmp(n,"direction")){ in->direction=d; motion_from_speed_dir(vm,in); return 1; }
  if(!strcmp(n,"speed")){ in->speed=d; motion_from_speed_dir(vm,in); return 1; }
  return 0;
}
int gml_vm_instance_builtin_set(GmlVM *vm, GmlInstance *instance,
                                const char *name, GmlVal value){
  return inst_builtin_set(vm,instance,name,value);
}

/* ---------------- variable access by scope ---------------- */
static GmlInstance *first_active_instance(GmlVM *vm){
  if(!vm) return NULL;
  for(int i=0;i<vm->inst_count;i++){
    GmlInstance *o=&vm->inst[i];
    if(o->active && !o->marked) return o;
  }
  return NULL;
}
/* Resolve an instance-type to the target instance. GM reads `all.variable` from the first
 * active instance (writes are handled as a fan-out below); treating every negative selector as
 * self made `all.variable` accidentally private to the caller. */
static GmlInstance *var_target(GmlVM *vm, int inst){
  if(inst==IT_OTHER) return vm->cur_other;
  if(inst==IT_ALL)   return first_active_instance(vm);
  if(inst<0)         return vm->cur_self;
  for(int i=0;i<vm->inst_count;i++){ GmlInstance *o=&vm->inst[i];
    if(o->active && !o->marked && gml_object_is(vm,o->obj,inst)) return o; }
  return NULL;
}
/* GM built-in global variables: these names live in global scope even when a
 * bytecode reference uses the current-instance scope. */
static int is_global_builtin(const char *n){
  return !strcmp(n,"health")||!strcmp(n,"lives")||!strcmp(n,"score")||!strcmp(n,"async_load")||
         !strcmp(n,"view_enabled")||!strcmp(n,"cursor_sprite"); }
static int is_classic_transition_builtin(GmlVM *vm,const char *n){
  return vm && vm->win && anygm_policy_uses_classic_runtime(vm->win) &&
         (!strcmp(n,"transition_kind") || !strcmp(n,"transition_steps"));
}
#define GML_DYNAMIC_GLOBAL_PREFIX "\001globalvar:"
static int dynamic_global_marker_name(const char *name,char *marker,size_t marker_size){
  if(!name || !*name || !marker || marker_size<=sizeof(GML_DYNAMIC_GLOBAL_PREFIX)) return 0;
  size_t prefix_size=sizeof(GML_DYNAMIC_GLOBAL_PREFIX)-1;
  size_t name_size=strlen(name);
  if(name_size>127 || prefix_size+name_size+1>marker_size) return 0;
  memcpy(marker,GML_DYNAMIC_GLOBAL_PREFIX,prefix_size);
  memcpy(marker+prefix_size,name,name_size+1);
  return 1;
}
int gml_vm_declare_globalvar(GmlVM *vm,const char *name){
  if(!vm || !vm->win || !anygm_policy_uses_classic_runtime(vm->win) || !name) return 0;
  size_t name_size=strlen(name);
  if(!name_size || name_size>127 ||
     !(name[0]=='_' || (name[0]>='A'&&name[0]<='Z') ||
       (name[0]>='a'&&name[0]<='z'))) return 0;
  for(size_t index=1;index<name_size;index++)
    if(!(name[index]=='_' || (name[index]>='A'&&name[index]<='Z') ||
         (name[index]>='a'&&name[index]<='z') ||
         (name[index]>='0'&&name[index]<='9'))) return 0;
  char marker[160];
  if(!dynamic_global_marker_name(name,marker,sizeof marker)) return 0;
  char *owned_name=strdup(name);
  char *owned_marker=strdup(marker);
  if(!owned_name || !owned_marker){
    free(owned_name);
    free(owned_marker);
    return 0;
  }
  uint32_t name_hash=gml_value_name_hash(owned_name);
  uint32_t marker_hash=gml_value_name_hash(owned_marker);
  (void)gml_varmap_put_owned_hashed(&vm->globals,owned_name,name_hash);
  *gml_varmap_put_owned_hashed(&vm->globals,owned_marker,marker_hash)=vreal(1);
  return 1;
}
static int is_dynamic_globalvar(GmlVM *vm,int inst,const char *name,uint32_t name_hash){
  if(!strcmp(name,"background_color") || !strcmp(name,"background_colour")) return 0;
  if(inst!=IT_SELF || !vm || !vm->win ||
     !anygm_policy_uses_classic_runtime(vm->win) ||
     !gml_varmap_get_hashed(&vm->globals,name,name_hash)) return 0;
  char marker[160];
  if(!dynamic_global_marker_name(name,marker,sizeof marker)) return 0;
  GmlVal *declared=gml_varmap_get_hashed(
      &vm->globals,marker,gml_value_name_hash(marker));
  return declared && (declared->t!=V_REAL || declared->d!=0.0);
}
static int argument_index(const char *name){
  if(strncmp(name,"argument",8)) return -1;
  const char *p=name+8;
  if(*p<'0' || *p>'9') return -1;
  int idx=0;
  while(*p>='0' && *p<='9'){
    idx=idx*10 + (*p-'0');
    if(idx>=16) return -1;
    p++;
  }
  return *p? -1 : idx;
}
static int argument_get(GmlVM *vm, const char *name, GmlVal *out){
  if(!strcmp(name,"argument_count")){ *out=vreal(vm->script_argc); return 1; }
  int idx=argument_index(name);
  if(idx>=0){ *out=(idx<vm->script_argc)? vm->script_args[idx] : vundef(); return 1; }
  return 0;
}
static int argument_set(GmlVM *vm, const char *name, GmlVal v){
  int idx=argument_index(name);
  if(idx<0) return 0;
  vm->script_args[idx]=v;
  if(idx>=vm->script_argc) vm->script_argc=idx+1;
  return 1;
}
double gml_room_speed(GmlVM *vm){
  /* game_set_speed() is the authoritative Studio cadence once it has been called. The
   * host uses the same slot when scheduling anygm_run_frame/audio, so VM clocks and delta_time must
   * not continue advancing at the room resource's older speed. */
  GmlVal *runtime=vm?gml_varmap_get(&vm->globals,"__game_speed_fps"):NULL;
  double runtime_fps=runtime?asnum(*runtime):0.0;
  if(runtime_fps>0) return runtime_fps;
  GmlVal *p=gml_varmap_get(&vm->globals,"room_speed");
  double v=p?asnum(*p):0.0;
  if(v>0) return v;
  GmlRoom room;
  if(vm && vm->win && vm->room_index>=0 && gml_vm_room_get(vm,vm->room_index,&room)==0 && room.speed>0)
    return room.speed;
  if(vm && vm->win && vm->win->game_speed>0) return vm->win->game_speed;
  return 30.0;
}
uint32_t gml_vm_room_background_argb(GmlVM *vm){
  GmlVal *runtime=vm?gml_varmap_get(&vm->globals,"background_color"):NULL;
  if(runtime) return 0xFF000000u|((uint32_t)asnum(*runtime)&0xFFFFFFu);
  GmlRoom room;
  if(vm && vm->win && vm->room_index>=0 && gml_vm_room_get(vm,vm->room_index,&room)==0)
    return room.bgcolor;
  return 0xFF000000u;
}
uint64_t gml_host_monotonic_time_ns(GmlVM *vm){
  if(vm && vm->host && vm->host->monotonic_time_ns)
    return vm->host->monotonic_time_ns(vm->host->userdata);
  if(!vm) return 0;
  vm->fallback_monotonic_ns+=UINT64_C(1000000);
  return vm->fallback_monotonic_ns;
}

AnygmResult gml_host_wall_time(GmlVM *vm,AnygmWallTime *wall){
  if(!wall || wall->struct_size<sizeof *wall) return ANYGM_ERROR_INVALID_ARGUMENT;
  if(vm && vm->host && vm->host->wall_time)
    return vm->host->wall_time(vm->host->userdata,wall);
  wall->flags=ANYGM_WALL_TIME_OFFSET_VALID;
  wall->unix_seconds=0;
  wall->utc_offset_minutes=0;
  wall->reserved=0;
  return ANYGM_OK;
}

uint64_t gml_host_random_seed(GmlVM *vm){
  if(vm && vm->host && vm->host->random_seed)
    return vm->host->random_seed(vm->host->userdata);
  uint64_t frame=vm&&vm->frame>0?(uint64_t)vm->frame:0;
  uint64_t previous=vm?(uint64_t)vm->rng_state:0;
  return UINT64_C(0x9e3779b97f4a7c15)^(frame<<32)^previous;
}

/* A read inside one frame still has to move, because content implements waiting by spinning on this
 * value until it advances. What it must not do is move by how long this machine took: a clock that
 * depends on host load makes the same run answer differently every time, and a run that cannot
 * repeat itself cannot be restored either — a state saved mid-stage resumes into a different
 * continuation, which is what a savestate and a rewind both are. So the intra-frame part counts
 * reads rather than milliseconds, bounded below one frame so it can never overtake the next. */
static double intra_frame_ms(GmlVM *vm){
  double step=1000.0 / gml_room_speed(vm);
  if(vm->time_sample_frame!=vm->frame){
    vm->time_sample_frame=vm->frame;
    vm->time_sample_cpu_ms=0.0;
    vm->time_sample_draw_ms=0.0;
  }
  /* Drawing may read the clock, and a wait loop written in a Draw event needs it to advance or it
   * never ends. But what a frame draws has to be a function of the state alone: a state restored
   * and redrawn must reproduce the frame it was saved from, and the redraw runs without the step
   * that preceded it. So the advance made while drawing lands in a shadow that starts from the
   * step's value and is dropped at the end of the phase. */
  double *slot=vm->draw_phase?&vm->time_sample_draw_ms:&vm->time_sample_cpu_ms;
  double intra=*slot;
  double limit=step*0.999;
  if(intra<limit) *slot=intra+1.0;
  return intra<limit?intra:limit;
}
static double current_time_value(GmlVM *vm){
  return (double)vm->frame * (1000.0 / gml_room_speed(vm)) + intra_frame_ms(vm);
}
static int current_calendar_value(GmlVM *vm,const char *name,GmlVal *out){
  if(strncmp(name,"current_",8) || !strcmp(name,"current_time")) return 0;
  AnygmWallTime wall={0};
  wall.struct_size=sizeof wall;
  if(gml_host_wall_time(vm,&wall)!=ANYGM_OK) return 0;
  int64_t seconds=wall.unix_seconds;
  if(wall.flags&ANYGM_WALL_TIME_OFFSET_VALID)
    seconds+=(int64_t)wall.utc_offset_minutes*60;
  AnygmCalendarTime value;
  if(!anygm_calendar_from_unix_seconds(seconds,&value)) return 0;
  if(!strcmp(name,"current_second")) *out=vreal(value.second);
  else if(!strcmp(name,"current_minute")) *out=vreal(value.minute);
  else if(!strcmp(name,"current_hour")) *out=vreal(value.hour);
  else if(!strcmp(name,"current_day")) *out=vreal(value.day);
  else if(!strcmp(name,"current_weekday")) *out=vreal(value.weekday); /* Sunday=0 */
  else if(!strcmp(name,"current_month")) *out=vreal(value.month);
  else if(!strcmp(name,"current_year")) *out=vreal(value.year);
  else return 0;
  return 1;
}
/* get_timer (µs): frame-locked base plus intra-frame CPU advance, matching current_time.
 * The base advances exactly 1/fps per frame and shares the bounded intra-frame advance, so
 * busy-wait loops still exit and the answer stays repeatable. */
double gml_vm_get_timer_us(GmlVM *vm){
  return (double)vm->frame * (1000000.0 / gml_room_speed(vm)) + intra_frame_ms(vm)*1000.0;
}
static int inst_sprite_metric_get(GmlVM *vm, GmlInstance *in, const char *name, GmlVal *out){
  if(!in || !vm || !vm->render) return 0;
  GmlRender *R=(GmlRender*)vm->render;
  int si=(int)in->sprite_index;
  GmlRenderSpriteMetrics sprite;
  if(!gml_render_sprite_metrics(R,si,&sprite)) return 0;
  if(!strcmp(name,"sprite_width")){
    *out=vreal(sprite.width*fabs(in->image_xscale));
    return 1;
  }
  if(!strcmp(name,"sprite_height")){
    *out=vreal(sprite.height*fabs(in->image_yscale));
    return 1;
  }
  if(!strcmp(name,"sprite_xoffset")){ *out=vreal(sprite.origin_x); return 1; }
  if(!strcmp(name,"sprite_yoffset")){ *out=vreal(sprite.origin_y); return 1; }
  return 0;
}
/* hash gate for the special-variable chains in var_get_h/var_set_h: almost every variable
 * access is a plain instance/global var, which otherwise pays the full strcmp chain on every
 * read and write. Hash-hit => run the original chain (its strcmps confirm; collisions are
 * safe); miss => the name is provably not special, go straight to the varmap. */
static const char *const g_special_var_names[]={
  "undefined","infinity","room","room_first","room_last","keyboard_lastkey","room_speed","working_directory","program_directory",
  "fps","delta_time","view_current","view_enabled","room_persistent","background_color","background_colour",
  "event_type","event_number","mouse_x","mouse_y",
  "current_time","current_second","current_minute","current_hour","current_day","current_weekday",
  "current_month","current_year","os_type","os_windows","os_uwp","os_xboxone","os_ps3","os_ps4","os_psvita",
  "os_macosx","os_linux","os_ios","os_android","os_unknown","os_switch_operating_system","room_width",
  "time_source_global","time_source_game","time_source_units_seconds","time_source_units_frames",
  "time_source_expire_nearest","time_source_expire_after","time_source_state_initial",
  "time_source_state_active","time_source_state_paused","time_source_state_stopped",
  "room_height","instance_count","health","lives","score","async_load","id","object_index",
  "cursor_sprite",
  "image_number","sprite_width","sprite_height","sprite_xoffset","sprite_yoffset","image_single",
  "x","y","xprevious","yprevious","xstart","ystart","sprite_index","mask_index","image_index",
  "image_speed","image_xscale","image_yscale","image_angle","image_alpha","image_blend",
  "depth","visible","solid","persistent","hspeed","vspeed","direction","speed",
  "layer",
  "phy_position_x","phy_position_y",
  "gravity","gravity_direction","friction","path_index","path_position","path_speed",
  "path_orientation","path_scale","path_positionprevious","path_endaction",
  "timeline_index","timeline_position","timeline_speed","timeline_running","timeline_loop",
  "transition_kind","transition_steps",
};
#define N_SPECIAL_VAR (int)(sizeof g_special_var_names/sizeof *g_special_var_names)
/* Open-addressed set of the special-name hashes.  A 64-bit bloom cannot gate 109 names — it
 * saturates, so nearly every access paid the full linear scan the gate exists to avoid.  Probing
 * a power-of-two table keeps the miss path at one or two loads.  Empty slots are 0, so a name
 * hashing to 0 is stored as 1; that can only produce a false positive, and the chains behind
 * this gate confirm with their own comparisons. */
#define SPECIAL_VAR_SLOTS 256u
#define SPECIAL_VAR_MASK (SPECIAL_VAR_SLOTS-1u)
static int var_name_maybe_special(GmlVM *vm,const char *name,uint32_t nh){
  if(vm && !vm->special_var_hash){
    vm->special_var_hash=calloc(SPECIAL_VAR_SLOTS,sizeof(*vm->special_var_hash));
    if(!vm->special_var_hash) return 1;
    for(int i=0;i<N_SPECIAL_VAR;i++){
      uint32_t h=gml_value_name_hash(g_special_var_names[i]);
      uint32_t stored=h?h:1u;
      uint32_t slot=h&SPECIAL_VAR_MASK;
      while(vm->special_var_hash[slot] && vm->special_var_hash[slot]!=stored)
        slot=(slot+1u)&SPECIAL_VAR_MASK;
      vm->special_var_hash[slot]=stored;
      vm->special_var_bloom |= 1ull<<(h&63);
    }
  }
  if(!vm) return 1;
  if(vm->special_var_bloom & (1ull<<(nh&63))){
    uint32_t want=nh?nh:1u;
    for(uint32_t slot=nh&SPECIAL_VAR_MASK;vm->special_var_hash[slot];slot=(slot+1u)&SPECIAL_VAR_MASK)
      if(vm->special_var_hash[slot]==want) return 1;
  }
  /* prefix-matched specials (argumentN / argument_count / bbox_*) */
  if(name[0]=='a' && !strncmp(name,"argument",8)) return 1;
  if(name[0]=='b' && !strncmp(name,"bbox_",5)) return 1;
  return 0;
}
int gml_vm_variable_name_maybe_special(GmlVM *vm, const char *name,
                                       uint32_t name_hash){
  return var_name_maybe_special(vm,name,name_hash);
}
static int is_room_global_array(const char *n);
static int background_dimension_get(GmlVM *vm,const char *name,int index,GmlVal *out);
static GmlVal var_get_h(GmlVM *vm, int inst, const char *name, uint32_t nh){
  GmlVal out;
  if(inst==IT_STATIC){
    int ci=vm?vm->cur_code_index:-1;
    if(ci>=0 && ci<vm->code_static_count && vm->code_static){
      GmlVal *p=gml_varmap_get_hashed(&vm->code_static[ci],name,nh);
      return p?*p:vundef();
    }
    return vundef();
  }
  if(is_dynamic_globalvar(vm,inst,name,nh)){
    GmlVal *slot=gml_varmap_get_hashed(&vm->globals,name,nh);
    return slot?*slot:vreal(0);
  }
  if(vm->win && anygm_policy_uses_classic_runtime(vm->win) &&
     background_dimension_get(vm,name,0,&out)) return out;
  /* GM6/7/8 variables retain their old scalar-at-index-zero behaviour even when the same
   * built-in also exposes indexed view/background slots. Classic source commonly reads
   * `view_wview` with no brackets; returning the array value coerces to zero and can pin every
   * moving instance to the left edge. Keep this compatibility local to classic containers so
   * Studio arrays continue to use normal value semantics. */
  if(vm->win && anygm_policy_uses_classic_runtime(vm->win) && strcmp(name,"view_current") &&
     is_room_global_array(name)){
    GmlVal *slot=gml_varmap_get_hashed(&vm->globals,name,nh);
    if(!slot) return vreal(0);
    if(slot->t!=V_ARR || !slot->arr) return *slot;
    GmlArr *a=slot->arr;
    return (a->data && a->len>0)?a->data[0]:vreal(0);
  }
  if(!var_name_maybe_special(vm,name,nh)){
    if(inst==IT_GLOBAL){ GmlVal *p=gml_varmap_get_hashed(&vm->globals,name,nh); return p?*p:vreal(0); }
    GmlInstance *self=var_target(vm,inst);
    if(self){
      GmlVal *p=inst_is_struct_ref(self)?struct_field_get_h(vm,self,name,nh)
                                             :gml_varmap_get_hashed(&self->vars,name,nh);
      if(p) return *p;
    }
    return vreal(0);
  }
  if(!strcmp(name,"undefined")) return vundef();   /* GMS2.3 builtin literal used by optional-arg prologues */
  if(!strcmp(name,"infinity")) return vreal(INFINITY); /* unbounded numeric literal */
  if(!strcmp(name,"room")) return vreal(vm->room_index);   /* GM built-in: current room index */
  if(!strcmp(name,"room_first") || !strcmp(name,"room_last")){
    if(!vm->win || !vm->win->room_order || vm->win->n_room_order<=0) return vreal(-1);
    int order_index=!strcmp(name,"room_first")?0:vm->win->n_room_order-1;
    return vreal((double)vm->win->room_order[order_index]);
  }
  if(!strcmp(name,"keyboard_lastkey")) return vreal(vm->last_key); /* GM: last key pressed */
  if(!strcmp(name,"room_speed")) return vreal(gml_room_speed(vm));
  if(!strcmp(name,"working_directory")){
    /* This is the installed file-bundle root. The file API overlays save_dir on reads and
     * redirects writes there, including absolute paths formed by concatenating this value. */
    return vstr(vm->working_directory); }
  if(!strcmp(name,"program_directory")){
    return vstr(vm->program_directory); }
  if(!strcmp(name,"fps")) return vreal(gml_room_speed(vm));
  /* Studio exposes the previous frame duration in microseconds. A host frame is scheduled at
   * the declared cadence, so a fixed deterministic interval is both the closest steady-run
   * value and keeps time-based gameplay reproducible across host load and fast-forward. */
  if(!strcmp(name,"delta_time")) return vreal(1000000.0/gml_room_speed(vm));
  /* GameMaker OS identity: the runtime executes Windows-built data.win files, so os_type reports
   * os_windows and content takes its desktop path. The os_* constants are also provided; GMS bakes some as literals
   * but references others as runtime constants; without them a `case os_windows` read undefined and
   * never matched). */
  if(!strcmp(name,"os_type")){ const char *e=anygm_host_development_setting(vm->host,"GML_OS_TYPE"); return vreal(e?atof(e):0 /*os_windows*/); }
  if(!strcmp(name,"os_windows")) return vreal(0);
  if(!strcmp(name,"os_macosx")) return vreal(1);
  if(!strcmp(name,"os_ios")) return vreal(3);
  if(!strcmp(name,"os_android")) return vreal(4);
  if(!strcmp(name,"os_linux")) return vreal(6);
  if(!strcmp(name,"os_psvita")) return vreal(12);
  if(!strcmp(name,"os_ps4")) return vreal(14);
  if(!strcmp(name,"os_xboxone")) return vreal(15);
  if(!strcmp(name,"os_ps3")) return vreal(16);
  if(!strcmp(name,"os_uwp")) return vreal(18);
  if(!strcmp(name,"os_unknown")) return vreal(-1);
  if(!strcmp(name,"time_source_global") || !strcmp(name,"time_source_units_seconds") ||
     !strcmp(name,"time_source_expire_nearest") || !strcmp(name,"time_source_state_initial")) return vreal(0);
  if(!strcmp(name,"time_source_game") || !strcmp(name,"time_source_units_frames") ||
     !strcmp(name,"time_source_expire_after") || !strcmp(name,"time_source_state_active")) return vreal(1);
  if(!strcmp(name,"time_source_state_paused")) return vreal(2);
  if(!strcmp(name,"time_source_state_stopped")) return vreal(3);
  if(!strcmp(name,"view_current")){ GmlVal *p=gml_varmap_get(&vm->globals,name); return p?*p:vreal(0); }
  if(!strcmp(name,"room_persistent")){ GmlVal *p=gml_varmap_get(&vm->globals,name); return p?*p:vreal(0); }
  if(!strcmp(name,"background_color")||!strcmp(name,"background_colour")){
    return vreal((double)(gml_vm_room_background_argb(vm)&0xFFFFFFu));
  }
  if(!strcmp(name,"event_type")) return vreal(vm->event_type);
  if(!strcmp(name,"event_number")) return vreal(vm->event_number);
  if(!strcmp(name,"current_time")) return vreal(current_time_value(vm));
  if(current_calendar_value(vm,name,&out)) return out;
  if(argument_get(vm,name,&out)) return out;
  if(!strcmp(name,"room_width")||!strcmp(name,"room_height")){   /* GM built-in: current room size */
    GmlRoom r; if(gml_vm_room_get(vm,vm->room_index,&r)==0)
      return vreal(name[5]=='w'? (double)r.width : (double)r.height);
    return vreal(0); }
  if(!strcmp(name,"mouse_x")||!strcmp(name,"mouse_y")){   /* GM built-in: mouse in room coords */
    double mx,my; gml_input_mouse(vm,&mx,&my,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL);
    return vreal(name[6]=='x'? mx : my); }
  if(!strcmp(name,"instance_count")){   /* Count active, unmarked instances, matching instance_find enumeration. */
    int c=0; for(int i=0;i<vm->inst_count;i++) if(vm->inst[i].active && !vm->inst[i].marked) c++;
    return vreal(c); }
  if(is_classic_transition_builtin(vm,name) || inst==IT_GLOBAL || is_global_builtin(name)){
    GmlVal *p=gml_varmap_get_hashed(&vm->globals,name,nh); return p?*p:vreal(0); }
  if((inst==IT_OTHER && !vm->cur_other) || (inst==IT_SELF && !vm->cur_self)){
    if(!strcmp(name,"id") || !strcmp(name,"object_index")) return vreal(IT_NOONE);
  }
  GmlInstance *self = var_target(vm,inst);
  if(self){
    if(inst_is_struct_ref(self)){
      GmlVal *p=struct_field_get_h(vm,self,name,nh);
      return p?*p:vreal(0);
    }
    /* image_number = frame count of the current sprite (needs the renderer) */
    if(!strcmp(name,"image_number")){ GmlRender *R=(GmlRender*)vm->render;
      return vreal(R? gml_sprite_frames(R,(int)self->sprite_index):0); }
    if(inst_sprite_metric_get(vm,self,name,&out)) return out;
    /* bbox_left/right/top/bottom = the instance's collision bounding box (from the sprite mask margins) */
    if(!strncmp(name,"bbox_",5)){ double l,t,r,b;
      if(gml_vm_instance_bbox(vm,self,&l,&t,&r,&b)){
        /* Modern special variables expose far edges as exclusive coordinates while the collision
         * engine keeps inclusive pixel bounds internally. Classic formats expose inclusive edges. */
        if(!vm->win || !anygm_policy_uses_classic_runtime(vm->win)){ r+=1.0; b+=1.0; }
        if(!strcmp(name,"bbox_left"))   return vreal(l);
        if(!strcmp(name,"bbox_right"))  return vreal(r);
        if(!strcmp(name,"bbox_top"))    return vreal(t);
        if(!strcmp(name,"bbox_bottom")) return vreal(b); }
      return vreal(0); }
    if(inst_builtin_get(vm,self,name,&out)) return out;
    GmlVal *p=gml_varmap_get_hashed(&self->vars,name,nh); if(p) return *p;
  }
  return vreal(0);
}
GmlVal gml_vm_variable_get_h(GmlVM *vm, int instance,
                             const char *name, uint32_t name_hash){
  return var_get_h(vm,instance,name,name_hash);
}
GmlVal gml_vm_identifier_get(GmlVM *vm,const char *name){
  if(!vm || !name || !*name) return vundef();
  return var_get_h(vm,IT_SELF,name,gml_value_name_hash(name));
}
/* GM: writing OBJECT.variable = value assigns to EVERY instance of that object (reading
 * returns only the first). inst_t in [0,n_objects) is an object index; a real instance id
 * is >=100000, so it never collides. Fans a write out to all instances of the object. */
static int is_object_scope(GmlVM *vm, int inst_t){ return inst_t>=0 && inst_t<vm->n_objects; }
static void var_set_h(GmlVM *vm, int inst, const char *name, uint32_t nh, GmlVal v){
  GML_VM_DIAGNOSTIC_VARIABLE_SCOPE(vm,inst,name,-1,v);
  gml_arr_mark_escaped(v);   /* target is a global/instance slot: outlives the current scope */
  if(inst==IT_STATIC){
    int ci=vm?vm->cur_code_index:-1;
    if(ci>=0 && ci<vm->code_static_count && vm->code_static)
      *gml_varmap_put_hashed(&vm->code_static[ci],name,nh)=v;
    return;
  }
  if(is_dynamic_globalvar(vm,inst,name,nh)){
    *gml_varmap_put_hashed(&vm->globals,name,nh)=v;
    return;
  }
  if(vm->win && anygm_policy_uses_classic_runtime(vm->win) && strcmp(name,"view_current") &&
     is_room_global_array(name)){
    GmlVal *slot=gml_varmap_put_hashed(&vm->globals,name,nh);
    GmlArr *a=gml_arr_slot_ensure(slot);
    a->escaped=1;
    gml_arr_index_ensure(a,0);
    if(a && a->cap>0) a->data[0]=v;
    return;
  }
  if(!var_name_maybe_special(vm,name,nh)){
    if(inst==IT_GLOBAL){ *gml_varmap_put_hashed(&vm->globals,name,nh)=v; return; }
    if(inst==IT_ALL){
      for(int i=0;i<vm->inst_count;i++){ GmlInstance *o=&vm->inst[i];
        if(o->active && !o->marked) *gml_varmap_put_hashed(&o->vars,name,nh)=v; }
      return;
    }
    if(is_object_scope(vm,inst)){   /* object.var = v -> all instances */
      for(int i=0;i<vm->inst_count;i++){ GmlInstance *o=&vm->inst[i];
        if(o->active && !o->marked && gml_object_is(vm,o->obj,inst)) *gml_varmap_put_hashed(&o->vars,name,nh)=v; }
      return;
    }
    GmlInstance *self=var_target(vm,inst);
    if(self){
      if(inst_is_struct_ref(self)) method_cache_invalidate(self,name);
      *gml_varmap_put_hashed(&self->vars,name,nh)=v;
    }
    return;
  }
  if(!strcmp(name,"room")){
    int target=(int)asnum(v);
    gml_vm_warm_audio_for_room(vm,target);
    vm->pending_room=target;
    return; }  /* GM: room=X -> goto room */
  if(argument_set(vm,name,v)) return;
  if(!strcmp(name,"room_speed")||!strcmp(name,"view_current")||!strcmp(name,"room_persistent")){
    *gml_varmap_put_hashed(&vm->globals,name,nh)=v;
    return;
  }
  if(!strcmp(name,"background_color")||!strcmp(name,"background_colour")){
    *gml_varmap_put(&vm->globals,"background_color")=v;
    return;
  }
  if(is_classic_transition_builtin(vm,name) || inst==IT_GLOBAL || is_global_builtin(name)){
    *gml_varmap_put_hashed(&vm->globals,name,nh)=v; return; }
  if(inst==IT_ALL){
    for(int i=0;i<vm->inst_count;i++){ GmlInstance *o=&vm->inst[i];
      if(o->active && !o->marked && !inst_builtin_set(vm,o,name,v))
        *gml_varmap_put_hashed(&o->vars,name,nh)=v; }
    return;
  }
  if(is_object_scope(vm,inst)){   /* object.builtin = v (x, hspeed, visible, ...) -> all instances */
    for(int i=0;i<vm->inst_count;i++){ GmlInstance *o=&vm->inst[i];
      if(o->active && !o->marked && gml_object_is(vm,o->obj,inst)){
        if(!inst_builtin_set(vm,o,name,v)) *gml_varmap_put_hashed(&o->vars,name,nh)=v; } }
    return;
  }
  GmlInstance *self = var_target(vm,inst);
  if(self){
    if(inst_is_struct_ref(self)){
      method_cache_invalidate(self,name);
      *gml_varmap_put_hashed(&self->vars,name,nh)=v;
      return;
    }
    if(inst_builtin_set(vm,self,name,v)) return;
    *gml_varmap_put_hashed(&self->vars,name,nh)=v;
  }
}
/* resolve an array/var instance-type to the owning instance. inst_t may be a special
 * scope (self/other), a real instance id (>=100000), or an object index (first instance). */
static GmlInstance *inst_by_id(GmlVM *vm, double idv);   /* fwd */
static GmlInstance *resolve_inst(GmlVM *vm, int inst_t){
  if(inst_t==IT_OTHER) return vm->cur_other;
  if(inst_t==IT_ALL)   return first_active_instance(vm);
  if(inst_t<0)         return vm->cur_self;
  return inst_by_id(vm,inst_t);               /* instance id OR object index */
}
/* scope map for array access (global/local/self/other/instance/object) */
static GmlVarMap *scope_map(GmlVM *vm, GmlVarMap *locals, int inst_t){
  if(inst_t==IT_GLOBAL) return &vm->globals;
  if(inst_t==IT_LOCAL)  return locals;
  if(inst_t==IT_STATIC){
    int ci=vm?vm->cur_code_index:-1;
    return (ci>=0 && ci<vm->code_static_count && vm->code_static)?&vm->code_static[ci]:NULL;
  }
  GmlInstance *s=resolve_inst(vm,inst_t);
  return s? &s->vars : NULL;
}
/* GM built-in room/view ARRAYS (background_index[], view_xview[], ...) are global state,
 * accessed without explicit scope — route them to globals regardless of inst_t. */
static int is_room_global_array(const char *n){
  if(!strncmp(n,"background_",11)){
    const char *s=n+11;
    return !strcmp(s,"visible") || !strcmp(s,"foreground") || !strcmp(s,"index") ||
           !strcmp(s,"x") || !strcmp(s,"y") || !strcmp(s,"htiled") || !strcmp(s,"vtiled") ||
           !strcmp(s,"hspeed") || !strcmp(s,"vspeed") || !strcmp(s,"stretch") ||
           !strcmp(s,"alpha") || !strcmp(s,"blend");
  }
  if(!strncmp(n,"view_",5)){
    const char *s=n+5;
    return !strcmp(s,"visible") || !strcmp(s,"xview") || !strcmp(s,"yview") ||
           !strcmp(s,"wview") || !strcmp(s,"hview") || !strcmp(s,"xport") ||
           !strcmp(s,"yport") || !strcmp(s,"wport") || !strcmp(s,"hport") ||
           !strcmp(s,"hborder") || !strcmp(s,"vborder") || !strcmp(s,"hspeed") ||
           !strcmp(s,"vspeed") || !strcmp(s,"object") || !strcmp(s,"camera") ||
           !strcmp(s,"surface_id");
  }
  return 0;
}
static int background_dimension_get(GmlVM *vm,const char *name,int index,GmlVal *out){
  int width=!strcmp(name,"background_width");
  if(!width && strcmp(name,"background_height")) return 0;
  double value=0;
  if(vm && index>=0){
    int background=(int)gml_vm_global_array_number(vm,"background_index",index);
    GmlRenderBackgroundMetrics metrics;
    if(gml_render_background_metrics((GmlRender*)vm->render,background,&metrics))
      value=width?metrics.logical_width:metrics.logical_height;
  }
  if(out) *out=vreal(value);
  return 1;
}
static double alarm_store_value(GmlVM *vm,GmlVal v){
  double value=v.t==V_REAL?v.d:(v.s?atof(v.s):0);
  /* GM6/7/8 stores alarms as integers. Delphi's Math.Round uses ties-to-even; nearbyint
   * supplies the same result under the process' default IEEE rounding mode. Studio semantics
   * retain fractional alarms, which several typewriter effects deliberately use. */
  return vm && vm->win && anygm_policy_uses_classic_runtime(vm->win) ? nearbyint(value) : value;
}
/* The environment cannot change while content runs, so resolve each of these once and keep the
 * per-write path free of host lookups. */
static const char *vm_arrayset_filter(GmlVM *vm){
  if(!vm->diagnostics.arrayset_filter_initialized){
    vm->diagnostics.arrayset_filter=anygm_host_development_setting(vm->host,"GML_DBG_ARRAYSET");
    vm->diagnostics.arrayset_filter_initialized=1;
  }
  return vm->diagnostics.arrayset_filter;
}
static const char *vm_view_log(GmlVM *vm){
  if(!vm->diagnostics.view_log_initialized){
    vm->diagnostics.view_log=anygm_host_development_setting(vm->host,"GML_LOG_VIEW");
    vm->diagnostics.view_log_initialized=1;
  }
  return vm->diagnostics.view_log;
}
static const char *vm_trace_filter(GmlVM *vm){
  if(!vm->diagnostics.trace_filter_initialized){
    vm->diagnostics.trace_filter=anygm_host_development_setting(vm->host,"GML_TRACE");
    vm->diagnostics.trace_filter_initialized=1;
  }
  return vm->diagnostics.trace_filter;
}
/* Consulted once per opcode dispatch and again on every call opcode, so this one dominated
 * every other host lookup in the runtime combined. */
static const char *vm_trace_call_filter(GmlVM *vm){
  if(!vm->diagnostics.trace_call_initialized){
    vm->diagnostics.trace_call=anygm_host_development_setting(vm->host,"GML_TRACE_CALL");
    vm->diagnostics.trace_call_initialized=1;
  }
  return vm->diagnostics.trace_call;
}
static void array_set_h(GmlVM *vm, GmlVarMap *locals, int inst_t, const char *nm, uint32_t nh, int idx, GmlVal v){
  GML_VM_DIAGNOSTIC_VARIABLE_SCOPE(vm,inst_t,nm,idx,v);
  { const char *debug_name=vm_arrayset_filter(vm);
    if(debug_name && nm && !strcmp(debug_name,nm)){
      
      anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[arrayset] f%ld scope=%d %s[%d] type=%d value=%.17g\n",
              vm->frame,inst_t,nm,idx,v.t,v.t==V_REAL?v.d:0.0);
    }
  }
  if(vm_view_log(vm) && !strcmp(nm,"view_camera")){
    
    anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[camera] bind f%ld view=%d value=%.0f scope=%d global=%d\n",
            vm->frame,idx,asnum(v),inst_t,is_room_global_array(nm));
  }
  if(!strcmp(nm,"view_enabled")){
    /* Current bytecode represents this scalar built-in through its array-access opcode:
     * writes carry accessor index 0 while reads carry index 1.  The index is metadata, not a pair
     * of independent GML cells, so both forms address the same global scalar. */
    *gml_varmap_put_hashed(&vm->globals,nm,nh)=v;
    return;
  }
  if(!strcmp(nm,"argument")){
    if(idx>=0 && idx<16){
      vm->script_args[idx]=v;
      if(idx>=vm->script_argc) vm->script_argc=idx+1;
    }
    return;
  }
  if(is_dynamic_globalvar(vm,inst_t,nm,nh)){
    GmlVal *slot=gml_varmap_put_hashed(&vm->globals,nm,nh);
    GmlArr *array=gml_arr_slot_ensure(slot);
    array->escaped=1;
    if(gml_arr_nested_set_flat(*slot,idx,v)) return;
    gml_arr_mark_escaped(v);
    gml_arr_note_legacy_2d_set(array,idx);
    gml_arr_index_ensure(array,idx);
    if(idx>=0 && idx<array->cap) array->data[idx]=v;
    return;
  }
  if(!strcmp(nm,"alarm")){
    double alv=alarm_store_value(vm,v);
    /* Writing OBJECT.alarm[i]=v applies to every instance of that object. An object index is in
     * [0,n_objects), while a real instance id is at least 100000. Reading returns the first
     * instance, but a write fans out. */
    if(inst_t==IT_ALL){
      if(idx>=0 && idx<GML_ALARMS)
        for(int i=0;i<vm->inst_count;i++){ GmlInstance *o=&vm->inst[i];
          if(o->active && !o->marked) o->alarm[idx]=alv; }
      return;
    }
    if(is_object_scope(vm,inst_t)){
      if(idx>=0 && idx<GML_ALARMS)
        for(int i=0;i<vm->inst_count;i++){ GmlInstance *o=&vm->inst[i];
          if(o->active && !o->marked && gml_object_is(vm,o->obj,inst_t)) o->alarm[idx]=alv; }
      return;
    }
    GmlInstance *s=resolve_inst(vm,inst_t);
    if(s && idx>=0 && idx<GML_ALARMS) s->alarm[idx]=alv;
    return; }
  /* In classic GML every variable accessor carries an optional array index, including scalar
   * built-ins.  For scalar instance variables that index is ignored: `image_single[0]=7` is the
   * same built-in write as `image_single=7`, while alarm[] remains genuinely indexed above.
   * Letting the generic array path create a user field instead left the displayed sub-image at
   * frame zero in projects that use the old indexed spelling. */
  if(var_name_maybe_special(vm,nm,nh) && !is_room_global_array(nm) &&
     inst_t!=IT_GLOBAL && inst_t!=IT_LOCAL){
    int handled=0;
    if(inst_t==IT_ALL){
      for(int i=0;i<vm->inst_count;i++){ GmlInstance *o=&vm->inst[i];
        if(o->active && !o->marked && !inst_is_struct_ref(o))
          handled |= inst_builtin_set(vm,o,nm,v); }
      if(handled) return;
    } else if(is_object_scope(vm,inst_t)){
      for(int i=0;i<vm->inst_count;i++){ GmlInstance *o=&vm->inst[i];
        if(o->active && !o->marked && gml_object_is(vm,o->obj,inst_t) && !inst_is_struct_ref(o))
          handled |= inst_builtin_set(vm,o,nm,v); }
      if(handled) return;
    } else {
      GmlInstance *s=resolve_inst(vm,inst_t);
      if(s && !inst_is_struct_ref(s) && inst_builtin_set(vm,s,nm,v)) return;
    }
  }
  if(inst_t==IT_ALL && !is_room_global_array(nm)){
    for(int i=0;i<vm->inst_count;i++){ GmlInstance *o=&vm->inst[i];
      if(!o->active || o->marked) continue;
      GmlVal *slot=gml_varmap_put_hashed(&o->vars,nm,nh); GmlArr *A=gml_arr_slot_ensure(slot);
      A->escaped=1;
      if(gml_arr_nested_set_flat(*slot,idx,v)) continue;
      gml_arr_mark_escaped(v); gml_arr_note_legacy_2d_set(A,idx); gml_arr_index_ensure(A,idx);
      if(idx>=0 && idx<A->cap) A->data[idx]=v; }
    return;
  }
  /* Non-alarm array write to OBJECT scope also fans out to all instances. */
  if(is_object_scope(vm,inst_t) && !is_room_global_array(nm)){
    for(int i=0;i<vm->inst_count;i++){ GmlInstance *o=&vm->inst[i];
      if(!o->active || o->marked || !gml_object_is(vm,o->obj,inst_t)) continue;
      GmlVal *slot=gml_varmap_put_hashed(&o->vars,nm,nh); GmlArr *A=gml_arr_slot_ensure(slot);
      A->escaped=1;
      if(gml_arr_nested_set_flat(*slot,idx,v)) continue;
      gml_arr_mark_escaped(v); gml_arr_note_legacy_2d_set(A,idx); gml_arr_index_ensure(A,idx);
      if(idx>=0 && idx<A->cap) A->data[idx]=v; }
    return;
  }
  GmlVarMap *m=is_room_global_array(nm)? &vm->globals : scope_map(vm,locals,inst_t); if(!m) return;
  GmlVal *slot=gml_varmap_put_hashed(m,nm,nh); GmlArr *A=gml_arr_slot_ensure(slot);
  /* The array container itself lives in a global/instance map. A later `var alias = field`
   * only borrows that same pointer in this VM; mark the owner before the local scope is cleaned,
   * otherwise the alias frees the persistent array and the next event reads dangling memory. */
  if(m!=locals) A->escaped=1;
  if(gml_arr_nested_set_flat(*slot,idx,v)) return;
  if(m!=locals || A->escaped) gml_arr_mark_escaped(v);   /* element outlives scope if its owner already does */
  gml_arr_note_legacy_2d_set(A,idx); gml_arr_index_ensure(A,idx);
  if(idx>=0 && idx<A->cap) A->data[idx]=v;
}
static GmlVal array_get_h(
    GmlVM *vm,GmlVarMap *locals,int inst_t,
    const char *nm,uint32_t nh,int idx){
  GmlVal dimension;
  if(background_dimension_get(vm,nm,idx,&dimension)) return dimension;
  if(!strcmp(nm,"view_enabled")){
    GmlVal *slot=gml_varmap_get_hashed(&vm->globals,nm,nh);
    return slot?*slot:vreal(0);
  }
  if(!strcmp(nm,"argument")) return (idx>=0 && idx<vm->script_argc && idx<16) ? vm->script_args[idx] : vundef();
  { int aidx=argument_index(nm);   /* `argumentN[idx]`: index INTO an array-valued argument (distinct from
       `argument[idx]`, the Nth arg). Missing this, serialize's `with(actions[i])` over an array passed as
       argument0 read 0 for every element, so every input binding serialised to "" and lost its default key. */
    if(aidx>=0){ GmlVal av=(aidx<vm->script_argc)? vm->script_args[aidx] : vundef();
      if(av.t==V_ARR && av.arr){ GmlVal nested; if(gml_arr_nested_get_flat(av,idx,&nested)) return nested;
        GmlArr *A=av.arr; if(idx>=0 && idx<A->len) return A->data[idx]; }
      return vreal(0); } }
  if(is_dynamic_globalvar(vm,inst_t,nm,nh)){
    GmlVal *slot=gml_varmap_get_hashed(&vm->globals,nm,nh);
    if(!slot || slot->t!=V_ARR || !slot->arr) return vreal(0);
    GmlArr *array=slot->arr;
    if(!array->data || array->len<0 || array->cap<array->len ||
       array->cap>16000000) return vreal(0);
    GmlVal nested;
    if(gml_arr_nested_get_flat(*slot,idx,&nested)) return nested;
    return (idx>=0 && idx<array->len)?array->data[idx]:vreal(0);
  }
  if(!strcmp(nm,"alarm")){ GmlInstance *s=resolve_inst(vm,inst_t);
    return vreal((s&&idx>=0&&idx<GML_ALARMS)? s->alarm[idx] : -1); }
  int room_global=is_room_global_array(nm);
  if(var_name_maybe_special(vm,nm,nh) && !room_global &&
     inst_t!=IT_GLOBAL && inst_t!=IT_LOCAL){
    GmlInstance *s=resolve_inst(vm,inst_t); GmlVal out;
    if(s && !inst_is_struct_ref(s) && inst_builtin_get(vm,s,nm,&out)) return out;
  }
  GmlVarMap *m=room_global? &vm->globals : scope_map(vm,locals,inst_t); if(!m) return vreal(0);
  GmlVal *slot=gml_varmap_get_hashed(m,nm,nh);
  if(!slot && !room_global && inst_t!=IT_GLOBAL && inst_t!=IT_LOCAL && inst_t!=IT_STATIC){
    GmlInstance *owner=resolve_inst(vm,inst_t);
    if(inst_is_struct_ref(owner)) slot=struct_field_get_h(vm,owner,nm,nh);
  }
  if(!slot||slot->t!=V_ARR) return vreal(0);
  GmlArr *A=slot->arr;
  /* defend against a corrupt/garbage GmlArr (e.g. a cross-version savestate) — never deref blindly */
  if(!A || !A->data || A->len<0 || A->cap<A->len || A->cap>16000000) return vreal(0);
  { GmlVal nested; if(gml_arr_nested_get_flat(*slot,idx,&nested)) return nested; }
  return (idx>=0 && idx<A->len)? A->data[idx] : vreal(0);
}
static GmlVal array_get_inst_field_h(GmlVM *vm, GmlInstance *s, const char *nm, uint32_t nh, int idx){
  if(!s) return vreal(0);
  if(s->obj>=0 && !strcmp(nm,"alarm"))
    return vreal((idx>=0 && idx<GML_ALARMS)? s->alarm[idx] : -1);
  GmlVal out;
  if(!inst_is_struct_ref(s) && inst_builtin_get(vm,s,nm,&out)) return out;
  GmlVal *slot=inst_is_struct_ref(s)?struct_field_get_h(vm,s,nm,nh)
                                         :gml_varmap_get_hashed(&s->vars,nm,nh);
  if(!slot || slot->t!=V_ARR || !slot->arr) return vreal(0);
  GmlArr *A=slot->arr;
  if(!A || !A->data || A->len<0 || A->cap<A->len || A->cap>16000000) return vreal(0);
  { GmlVal nested; if(gml_arr_nested_get_flat(*slot,idx,&nested)) return nested; }
  return (idx>=0 && idx<A->len)? A->data[idx] : vreal(0);
}
static void array_set_inst_field_h(GmlVM *vm,GmlInstance *s,const char *nm,uint32_t nh,int idx,GmlVal v){
  if(!s) return;
  GML_VM_DIAGNOSTIC_VARIABLE_INSTANCE(vm,s,nm,idx,v);
  { const char *debug_name=vm_arrayset_filter(vm);
    if(debug_name && nm && !strcmp(debug_name,nm)){
      
      anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[arrayset] f%ld instance=%u object=%d %s[%d] type=%d value=%.17g\n",
              vm->frame,s->id,s->obj,nm,idx,v.t,v.t==V_REAL?v.d:0.0);
    }
  }
  if(s->obj>=0 && !strcmp(nm,"alarm")){
    if(idx>=0 && idx<GML_ALARMS) s->alarm[idx]=alarm_store_value(vm,v);
    return;
  }
  if(!inst_is_struct_ref(s) && inst_builtin_set(vm,s,nm,v)) return;
  gml_arr_mark_escaped(v);
  GmlVal *slot=gml_varmap_put_hashed(&s->vars,nm,nh);
  GmlArr *A=gml_arr_slot_ensure(slot);
  A->escaped=1;
  if(gml_arr_nested_set_flat(*slot,idx,v)) return;
  gml_arr_note_legacy_2d_set(A,idx);
  gml_arr_index_ensure(A,idx);
  if(idx>=0 && idx<A->cap) A->data[idx]=v;
}
static int inst_is_struct_ref(const GmlInstance *in){
  return in && GML_IS_STRUCT_ID((double)in->id);
}
/* Constructor statics form the shared prototype of every struct produced by that constructor.
 * Instance fields shadow them; a constructor-less data struct has no prototype fallback. */
static GmlVal *struct_field_get_h(GmlVM *vm, GmlInstance *in, const char *nm, uint32_t nh){
  if(!vm || !inst_is_struct_ref(in) || !nm) return NULL;
  GmlVal *own=gml_varmap_get_hashed(&in->vars,nm,nh);
  if(own) return own;
  GmlVal *constructor=gml_varmap_get_hashed(&in->vars,"__ctor",GML_HASH_CONSTRUCTOR);
  int ci=(constructor && constructor->t==V_REAL)?(int)constructor->d:-1;
  if(ci<0 || ci>=vm->code_static_count || !vm->code_static) return NULL;
  return gml_varmap_get_hashed(&vm->code_static[ci],nm,nh);
}
static int method_cache_key(const char *nm){
  return nm && (!strcmp(nm,"__fn") || !strcmp(nm,"__self"));
}
static void method_cache_invalidate(GmlInstance *in, const char *nm){
  if(inst_is_struct_ref(in) && method_cache_key(nm)) in->method_bound=0;
}
static int method_struct_info(GmlInstance *bm, int *fci, GmlVal *selfv, int *have_self){
  if(fci) *fci=-1;
  if(selfv) *selfv=vundef();
  if(have_self) *have_self=0;
  if(!bm) return 0;
  if(bm->method_bound){
    if(fci) *fci=bm->method_fci;
    if(selfv) *selfv=bm->method_self;
    if(have_self) *have_self=1;
    return 1;
  }
  GmlVal *pf=gml_varmap_get_hashed(&bm->vars,"__fn",GML_HASH_METHOD_FN);
  if(!pf) return 0;
  GmlVal *ps=gml_varmap_get_hashed(&bm->vars,"__self",GML_HASH_METHOD_SELF);
  int ci=-1;
  int f=(int)asnum(*pf);
  if(GML_IS_FUNCVAL(f)) ci=f & 0x00FFFFFF;
  if(ci>=0 && ps){
    bm->method_bound=1;
    bm->method_fci=ci;
    bm->method_self=*ps;
  }
  if(fci) *fci=ci;
  if(ps){
    if(selfv) *selfv=*ps;
    if(have_self) *have_self=1;
  }
  return 1;
}

/* read/write any var on a specific instance (builtin or custom) */
static GmlVal inst_get_any_h(GmlVM *vm, GmlInstance *t, const char *nm, uint32_t nh){
  if(inst_is_struct_ref(t)){
    GmlVal *p=struct_field_get_h(vm,t,nm,nh);
    return p?*p:vreal(0);
  }
  GmlVal o; if(inst_builtin_get(vm,t,nm,&o)) return o;
  /* Same sprite-derived builtins var_get resolves for `self.X`, so a REFERENCED instance
   * (`other.image_number`, `foo.bbox_left`) reads them too — not 0. `expr.image_number` returning
   * 0 made an animation-gated cutscene wait forever on `floor(image_index)==image_number-1` (= -1). */
  if(!strcmp(nm,"image_number")){ GmlRender *R=(GmlRender*)vm->render;
    return vreal(R? gml_sprite_frames(R,(int)t->sprite_index):0); }
  if(inst_sprite_metric_get(vm,t,nm,&o)) return o;
  if(!strncmp(nm,"bbox_",5)){ double l,tp,r,b;
    if(gml_vm_instance_bbox(vm,t,&l,&tp,&r,&b)){
      if(!vm->win || !anygm_policy_uses_classic_runtime(vm->win)){ r+=1.0; b+=1.0; }
      if(!strcmp(nm,"bbox_left"))   return vreal(l);
      if(!strcmp(nm,"bbox_right"))  return vreal(r);
      if(!strcmp(nm,"bbox_top"))    return vreal(tp);
      if(!strcmp(nm,"bbox_bottom")) return vreal(b); }
    return vreal(0); }
  GmlVal *p=gml_varmap_get_hashed(&t->vars,nm,nh); return p?*p:vreal(0);
}
static void inst_set_any_h(GmlVM *vm, GmlInstance *t, const char *nm, uint32_t nh, GmlVal v){
  GML_VM_DIAGNOSTIC_VARIABLE_INSTANCE(vm,t,nm,-1,v);
  gml_arr_mark_escaped(v);   /* instance vars outlive the current scope */
  if(inst_is_struct_ref(t)){ method_cache_invalidate(t,nm); *gml_varmap_put_hashed(&t->vars,nm,nh)=v; return; }
  if(inst_builtin_set(vm,t,nm,v)) return;
  *gml_varmap_put_hashed(&t->vars,nm,nh)=v;
}
static GmlInstance *inst_by_id(GmlVM *vm, double idv){
  int id=(int)idv;
  /* Resource indices and live instance ids occupy disjoint ranges. Resolve an object index
   * directly instead of first scanning every room instance for an impossible low instance id;
   * array-heavy draw loops can perform thousands of these object-scoped reads per frame. */
  if(id>=0 && id<vm->n_objects){
    for(int i=0;i<vm->inst_count;i++)
      if(vm->inst[i].active && !vm->inst[i].marked && vm->inst[i].obj==id)
        return &vm->inst[i];
    return NULL;
  }
  /* Deactivation removes an instance from object-scoped lookup and scheduling, but an explicit
   * saved id remains a valid variable receiver. Region-culling controllers commonly deactivate
   * their marker and immediately read its geometry to decide which instances to reactivate. */
  for(int i=0;i<vm->inst_count;i++)
    if((vm->inst[i].active || vm->inst[i].deactivated) &&
       !vm->inst[i].marked && (int)vm->inst[i].id==id) return &vm->inst[i];
  return NULL;
}
GmlInstance *gml_vm_instance_by_id(GmlVM *vm, double id){
  return inst_by_id(vm,id);
}
/* resolve a StackTop instance reference value: real id / object index / self / other. */
/* GMS2.3 struct pool. A struct is a standalone GmlInstance (varmap of fields, obj=-1) kept OUT of the
 * room instance array so it is never stepped/drawn/counted; field access reaches it by id. */
/* A struct id encodes its slot in the low 20 bits and a 7-bit generation above it, all inside the
 * 0x50000000 struct-id band. gml_struct_find is O(1) (direct slot index) and the generation lets a
 * GC free + reuse slots safely: a dangling id to a recycled slot fails the generation check and reads
 * NULL instead of a different struct. */
int gml_vm_struct_ensure_capacity(GmlVM *vm, int need){
  if(!vm || need<0 || need>GML_STRUCT_SLOT_MAX) return 0;
  if(need<=vm->cap_structs) return 1;
  int nc=vm->cap_structs?vm->cap_structs:64; while(nc<need) nc*=2;
  GmlInstance **ns = realloc(vm->structs, (size_t)nc*sizeof(GmlInstance*));
  if(!ns) return 0;
  vm->structs=ns;
  unsigned char *ng = realloc(vm->struct_gen, (size_t)nc*sizeof(unsigned char));
  if(!ng) return 0;
  vm->struct_gen=ng;
  for(int i=vm->cap_structs;i<nc;i++){ vm->structs[i]=NULL; vm->struct_gen[i]=0; }
  vm->cap_structs=nc;
  return 1;
}
GmlInstance *gml_struct_new(GmlVM *vm){
  int slot;
  if(vm->n_struct_free > 0){ slot = vm->struct_free[--vm->n_struct_free]; }   /* reuse a GC'd slot */
  else {
    if(vm->n_structs >= GML_STRUCT_SLOT_MAX) return NULL;   /* 1M live slots — far past any real set */
    if(vm->n_structs >= vm->cap_structs && !gml_vm_struct_ensure_capacity(vm,vm->n_structs+1)) return NULL;
    slot = vm->n_structs++;
  }
  vm->struct_gen[slot] = (unsigned char)((vm->struct_gen[slot]+1) & 0x7F);   /* bump generation on (re)use */
  GmlInstance *st = calloc(1,sizeof(GmlInstance));
  if(!st) return NULL;
  st->id = GML_STRUCT_ID_BASE + ((unsigned)vm->struct_gen[slot] << GML_STRUCT_SLOT_BITS) + (unsigned)slot;
  st->obj = -1; st->active = 1;
  vm->structs[slot] = st;
  /* A periodic collection can run at the beginning of a frame before that frame creates temporary
   * method/struct values. Mark that collection stale so a later canonical state query in the same
   * frame sees the post-step graph and does not serialize newly unreachable temporaries. */
  if(vm->structs_last_gc_frame==vm->frame)
    vm->structs_last_gc_frame=vm->frame-1;
  return st;
}
GmlInstance *gml_struct_find(GmlVM *vm, unsigned id){
  if(id < GML_STRUCT_ID_BASE) return NULL;
  unsigned idx = id - GML_STRUCT_ID_BASE;
  unsigned slot = idx & GML_STRUCT_SLOT_MASK;
  unsigned gen  = (idx >> GML_STRUCT_SLOT_BITS) & 0x7F;
  if((int)slot < vm->n_structs && vm->structs[slot] && (vm->struct_gen[slot] & 0x7F) == gen)
    return vm->structs[slot];
  return NULL;
}
void gml_vm_struct_free_slot_push(GmlVM *vm, int slot){
  if(!vm || slot<0) return;
  if(vm->n_struct_free>=vm->cap_struct_free){
    int nc=vm->cap_struct_free?vm->cap_struct_free*2:256;
    int *nf=realloc(vm->struct_free,(size_t)nc*sizeof(int));
    if(nf){ vm->struct_free=nf; vm->cap_struct_free=nc; }
  }
  if(vm->n_struct_free<vm->cap_struct_free) vm->struct_free[vm->n_struct_free++]=slot;
}
/* ---- struct garbage collection (mark-sweep from every GmlVal root, run between frames) ---- */
/* Array deduplication bounds repeated visits; retain an independent recursion limit to protect
 * the C stack on deeply nested arrays. */
#define GML_GC_MAX_ARRAY_DEPTH 4096
static void gc_mark_struct(GmlVM *vm, unsigned id, GmlInstance ***wl, int *wn, int *wcap){
  GmlInstance *s = gml_struct_find(vm,id);
  if(s && !s->marked){ s->marked=1;
    if(*wn >= *wcap){ *wcap = *wcap? *wcap*2 : 256; *wl = realloc(*wl,(size_t)*wcap*sizeof(GmlInstance*)); }
    if(*wl) (*wl)[(*wn)++] = s;
  }
}
/* Stamp arrays per collection so shared children are visited once rather than once per path.
 * A recursion limit alone bounds stack use, not the number of walks, and can leave reachable
 * objects unmarked. The epoch also terminates cycles without revisiting their nodes. */
static void gc_scan_arr(GmlVM *vm, GmlArr *A, GmlInstance ***wl, int *wn, int *wcap, int depth){
  if(!A || A->gc_epoch==vm->gc_epoch || depth>GML_GC_MAX_ARRAY_DEPTH) return;
  A->gc_epoch=vm->gc_epoch;
  for(int i=0;i<A->len;i++){ GmlVal v=A->data[i];
    if(v.t==V_REAL && GML_IS_STRUCT_ID(v.d)) gc_mark_struct(vm,(unsigned)v.d,wl,wn,wcap);
    else if(v.t==V_ARR && v.arr) gc_scan_arr(vm,(GmlArr*)v.arr,wl,wn,wcap,depth+1); }
}
static void gc_scan_val(GmlVM *vm, GmlVal v, GmlInstance ***wl, int *wn, int *wcap){
  if(v.t==V_REAL && GML_IS_STRUCT_ID(v.d)) gc_mark_struct(vm,(unsigned)v.d,wl,wn,wcap);
  else if(v.t==V_ARR && v.arr) gc_scan_arr(vm,(GmlArr*)v.arr,wl,wn,wcap,0);
}
typedef struct {
  GmlVM *vm;
  GmlInstance ***worklist;
  int *count;
  int *capacity;
} GcBuiltinRoots;
static void gc_scan_builtin_value(void *userdata,GmlVal value){
  GcBuiltinRoots *roots=userdata;
  gc_scan_val(roots->vm,value,roots->worklist,roots->count,roots->capacity);
}
static void gc_scan_vm(GmlVM *vm, GmlVarMap *m, GmlInstance ***wl, int *wn, int *wcap){
  if(!m || !m->slots) return;
  for(int i=0;i<m->cap;i++) if(m->slots[i].key) gc_scan_val(vm,m->slots[i].val,wl,wn,wcap);
}
void gml_struct_gc(GmlVM *vm){
  if(vm->n_structs<=0) return;
  /* A fresh array carries epoch 0, so a pass never claims one it has not reached. Skipping 0 on
   * wrap keeps that true for as long as the process runs. */
  if(++vm->gc_epoch==0) vm->gc_epoch=1;
  for(int i=0;i<vm->n_structs;i++) if(vm->structs[i]) vm->structs[i]->marked=0;
  GmlInstance **wl=NULL; int wn=0, wcap=0;
  /* roots: globals, function statics, every instance (active AND deactivated — the latter can be
   * reactivated), the argument register, and all GmlVal-bearing ds containers. */
  gc_scan_vm(vm,&vm->globals,&wl,&wn,&wcap);
  for(int i=0;i<vm->code_static_count;i++) gc_scan_vm(vm,&vm->code_static[i],&wl,&wn,&wcap);
  for(int i=0;i<vm->inst_count;i++) gc_scan_vm(vm,&vm->inst[i].vars,&wl,&wn,&wcap);
  for(int i=0;i<16;i++) gc_scan_val(vm,vm->script_args[i],&wl,&wn,&wcap);
  GcBuiltinRoots builtin_roots={vm,&wl,&wn,&wcap};
  gml_builtin_state_visit_values(vm->builtins,
                                 gc_scan_builtin_value,
                                 &builtin_roots);
  /* transitive: a live struct's own fields keep other structs/arrays alive */
  while(wn>0){ GmlInstance *s=wl[--wn]; gc_scan_vm(vm,&s->vars,&wl,&wn,&wcap); }
  /* sweep: free the unmarked, recycle their slots. gml_varmap_free_ex(,1) skips escaped (aliased) arrays,
   * exactly like instance teardown, so a shared GmlArr is never double-freed. */
  for(int i=0;i<vm->n_structs;i++){ GmlInstance *s=vm->structs[i];
    if(s && !s->marked){
      gml_varmap_free_ex(&s->vars,1); free(s); vm->structs[i]=NULL;
      gml_vm_struct_free_slot_push(vm,i);
    }
  }
  free(wl);
}
static GmlInstance *vm_inst_from_ref(GmlVM *vm, GmlVal iv){
  double v=asnum(iv);
  if(v==-1.0) return vm->cur_self;
  if(v==-2.0) return vm->cur_other;
  if(v<0) return NULL;
  if(GML_IS_STRUCT_ID(v)) return gml_struct_find(vm,(unsigned)v);   /* GMS2.3 struct field access */
  /* Values below 100000 select object types, including descendants; instance IDs use the higher range. */
  if(v<100000.0) return gml_find_instance(vm,(int)v);
  return inst_by_id(vm,v);
}
static int inst_has_any_h(GmlVM *vm, GmlInstance *t, const char *nm, uint32_t nh){
  if(!t || !nm) return 0;
  if(inst_is_struct_ref(t)) return struct_field_get_h(vm,t,nm,nh)!=NULL;
  GmlVal o;
  if(inst_builtin_get(vm,t,nm,&o)) return 1;
  if(!strcmp(nm,"image_number")) return 1;
  if(inst_sprite_metric_get(vm,t,nm,&o)) return 1;
  if(!strcmp(nm,"bbox_left")||!strcmp(nm,"bbox_right")||
     !strcmp(nm,"bbox_top") ||!strcmp(nm,"bbox_bottom")) return 1;
  return gml_varmap_get_hashed(&t->vars,nm,nh)!=NULL;
}
int gml_inst_var_exists(GmlVM *vm, GmlVal ref, const char *name){
  if(!vm || !name) return 0;
  GmlInstance *t=vm_inst_from_ref(vm,ref);
  return inst_has_any_h(vm,t,name,gml_value_name_hash(name));
}
GmlVal gml_inst_var_get_val(GmlVM *vm, GmlVal ref, const char *name, int *ok){
  if(ok) *ok=0;
  if(!vm || !name) return vundef();
  uint32_t nh=gml_value_name_hash(name);
  GmlInstance *t=vm_inst_from_ref(vm,ref);
  if(!inst_has_any_h(vm,t,name,nh)) return vundef();
  if(ok) *ok=1;
  GmlVal out=inst_get_any_h(vm,t,name,nh);
  if(out.t==V_STR) out.d=0;
  return out;
}
int gml_inst_var_set_val(GmlVM *vm, GmlVal ref, const char *name, GmlVal v){
  if(!vm || !name) return 0;
  GmlInstance *t=vm_inst_from_ref(vm,ref);
  if(!t) return 0;
  gml_arr_mark_escaped(v);
  uint32_t nh=gml_value_name_hash(name);
  if(inst_is_struct_ref(t)){
    method_cache_invalidate(t,name);
    GmlVal *p=gml_varmap_get_hashed(&t->vars,name,nh);
    if(p) *p=v;
    else {
      char *owned=strdup(name);
      if(!owned) return 0;
      *gml_varmap_put_owned_hashed(&t->vars,owned,gml_value_name_hash(owned))=v;
    }
    return 1;
  }
  if(inst_builtin_set(vm,t,name,v)) return 1;
  GmlVal *p=gml_varmap_get_hashed(&t->vars,name,nh);
  if(p) *p=v;
  else {
    char *owned=strdup(name);
    if(!owned) return 0;
    *gml_varmap_put_owned_hashed(&t->vars,owned,gml_value_name_hash(owned))=v;
  }
  return 1;
}
/* public accessor: read a builtin or custom instance variable by name → real value.
 * Returns 0 for absent variables (GM default). */
double gml_inst_var_get(GmlVM *vm, GmlInstance *in, const char *nm){
  if(!in) return 0;
  if(inst_is_struct_ref(in)){ GmlVal *p=gml_varmap_get(&in->vars,nm); return p?(p->t==V_REAL?p->d:0):0; }
  GmlVal o; if(inst_builtin_get(vm,in,nm,&o)) return o.t==V_REAL?o.d:0;
  if(inst_sprite_metric_get(vm,in,nm,&o)) return o.t==V_REAL?o.d:0;
  GmlVal *p=gml_varmap_get(&in->vars,nm); return p?(p->t==V_REAL?p->d:0):0;
}

/* ---------------- code lookup ---------------- */
int gml_code_index_by_name(GmlWin *w, const char *name){
  if(!w || !name) return -1;
  if(!w->code_hix && w->n_code>0){
    uint32_t cap=1;
    while(cap < (uint32_t)w->n_code*2u) cap<<=1;
    w->code_hix=malloc((size_t)cap*sizeof(int32_t));
    if(w->code_hix){
      for(uint32_t i=0;i<cap;i++) w->code_hix[i]=-1;
      w->code_hix_cap=cap;
      for(int i=0;i<w->n_code;i++){
        const char *nm=w->code[i].name;
        if(!nm) continue;
        uint32_t h=gml_value_name_hash(nm)&(cap-1);
        while(w->code_hix[h]>=0){
          if(!strcmp(w->code[w->code_hix[h]].name,nm)) break; /* preserve first duplicate */
          h=(h+1)&(cap-1);
        }
        if(w->code_hix[h]<0) w->code_hix[h]=i;
      }
    }
  }
  if(w->code_hix && w->code_hix_cap){
    uint32_t h=gml_value_name_hash(name)&(w->code_hix_cap-1);
    for(uint32_t probe=0; probe<w->code_hix_cap; probe++){
      int32_t i=w->code_hix[h];
      if(i<0) return -1;
      if(i<w->n_code && w->code[i].name && !strcmp(w->code[i].name,name)) return i;
      h=(h+1)&(w->code_hix_cap-1);
    }
    return -1;
  }
  for(int i=0;i<w->n_code;i++) if(w->code[i].name && !strcmp(w->code[i].name,name)) return i;
  return -1;
}
int gml_code_index_find(GmlWin *w, const char *substr){
  for(int i=0;i<w->n_code;i++) if(strstr(w->code[i].name,substr)) return i;
  return -1;
}

static int code_cache_branch_op(uint8_t kind){
  return kind==OP_B || kind==OP_BT || kind==OP_BF || kind==OP_PUSHENV || kind==OP_POPENV;
}
static int code_cache_find_pc(const GmlCode *c, uint32_t pc){
  int lo=0, hi=(int)c->n_insn-1;
  while(lo<=hi){
    int mid=lo+((hi-lo)>>1);
    uint32_t m=c->insn_pc[mid];
    if(m==pc) return mid;
    if(m<pc) lo=mid+1; else hi=mid-1;
  }
  return -1;
}
static void code_cache_free(GmlCode *c){
  if(!c) return;
  free(c->insn); free(c->insn_pc); free(c->branch_index);
  c->insn=NULL; c->insn_pc=NULL; c->branch_index=NULL;
  c->n_insn=0;
  c->micro_kind=0; c->micro_name=NULL; c->micro_hash=0;
}
enum {
  GML_MICRO_NONE=0,
  GML_MICRO_DS_MAP_GLOBAL_ARG0=1,
  GML_MICRO_APPROACH3=2,
  GML_MICRO_CALL_GLOBAL_ARG0=3,
  GML_MICRO_DS_MAP_METHOD_LOOP1=4,
  GML_MICRO_DS_MAP_NESTED_FALLBACK=5,
  GML_MICRO_ARRAY_METHOD_FLAGS=6,
  GML_MICRO_INPUT_ACTION_UPDATE=7
};
static int insn_arg_ref(const GmlInsn *in, int arg){
  if(!in || !in->refname || in->inst!=IT_ARG) return 0;
  if(arg<0 || arg>9) return 0;
  return !strncmp(in->refname,"argument",8) && in->refname[8]==(char)('0'+arg) && in->refname[9]==0;
}
static int insn_push_arg(const GmlInsn *in, int arg){
  return in && in->kind==OP_PUSH && in->type1==DT_VAR && insn_arg_ref(in,arg);
}
static int insn_pop_arg(const GmlInsn *in, int arg){
  return in && in->kind==OP_POP && in->type1==DT_VAR && insn_arg_ref(in,arg);
}
static int insn_push_builtin_name(const GmlInsn *in, const char *name){
  return in && name && in->kind==OP_PUSH && in->type1==DT_VAR && in->inst==IT_BUILTIN &&
         in->refname && !strcmp(in->refname,name);
}
static int insn_push_global_var(const GmlInsn *in){
  return in && in->kind==OP_PUSH && in->type1==DT_VAR && in->inst==IT_GLOBAL && in->refname;
}
static int insn_push_string(const GmlInsn *in){
  return in && in->kind==OP_PUSH && in->type1==DT_STRING;
}
static int insn_pop_local_var(const GmlInsn *in){
  return in && in->kind==OP_POP && in->type1==DT_VAR && in->inst==IT_LOCAL && in->refname;
}
static int insn_push_self_var(const GmlInsn *in){
  return in && in->kind==OP_PUSH && in->type1==DT_VAR && in->inst==IT_SELF && in->refname;
}
static int insn_pop_self_var(const GmlInsn *in){
  return in && in->kind==OP_POP && in->type1==DT_VAR && in->inst==IT_SELF && in->refname;
}
static int insn_push_stack_var(const GmlInsn *in){
  return in && in->kind==OP_PUSH && in->type1==DT_VAR && in->inst==IT_STACK && in->refname;
}
static int insn_push_stacktop_var(const GmlInsn *in){
  return in && in->kind==OP_PUSH && in->type1==DT_VAR && in->reftype==0x80 && in->refname;
}
static int insn_same_ref(const GmlInsn *a, const GmlInsn *b){
  if(!a || !b || !a->refname || !b->refname || a->inst!=b->inst) return 0;
  if(a->refhash && b->refhash && a->refhash!=b->refhash) return 0;
  return !strcmp(a->refname,b->refname);
}
static int insn_same_name(const GmlInsn *a, const GmlInsn *b){
  if(!a || !b || !a->refname || !b->refname) return 0;
  if(a->refhash && b->refhash && a->refhash!=b->refhash) return 0;
  return !strcmp(a->refname,b->refname);
}
static int insn_push_same_ref(const GmlInsn *push, const GmlInsn *ref){
  return push && push->kind==OP_PUSH && push->type1==DT_VAR && insn_same_ref(push,ref);
}
static int insn_pop_same_ref(const GmlInsn *pop, const GmlInsn *ref){
  return pop && pop->kind==OP_POP && pop->type1==DT_VAR && insn_same_ref(pop,ref);
}
static int insn_push_num(const GmlInsn *in, double v){
  if(!in || in->kind!=OP_PUSH) return 0;
  double d=0.0;
  if(in->type1==DT_INT16) d=(double)in->sval;
  else if(in->type1==DT_INT32) d=(double)in->ival;
  else if(in->type1==DT_INT64) d=(double)in->lval;
  else if(in->type1==DT_DOUBLE) d=in->dval;
  else return 0;
  return fabs(d-v)<1e-9;
}
static int insn_call_name(const GmlInsn *in, const char *name, int argc){
  return in && name && in->kind==OP_CALL && in->argc==argc &&
         in->refname && !strcmp(in->refname,name);
}
static const char *micro_debug_filter(const GmlWin *win){
  const char *filter=anygm_host_development_setting(win?win->host:NULL,"GML_DBG_MICRO");
  return filter?filter:"";
}
static void micro_debug(const GmlWin *win,const GmlCode *c,const char *where){
  const char *f=micro_debug_filter(win);
  if(f && *f && c && c->name && strstr(c->name,f))
    anygm_host_logf(win ? win->host : NULL,ANYGM_LOG_DEBUG,"[micro] %s kind=%u n=%u %s\n",where?where:"?",(unsigned)c->micro_kind,(unsigned)c->n_insn,c->name);
}
static void micro_debug_pc(const GmlWin *win,const GmlCode *c,const char *where,
                           uint32_t pc,uint32_t aux){
  const char *f=micro_debug_filter(win);
  if(f && *f && c && c->name && strstr(c->name,f))
    anygm_host_logf(win ? win->host : NULL,ANYGM_LOG_DEBUG,"[micro] %s pc=%u aux=%u n=%u %s\n",where?where:"?",pc,aux,(unsigned)c->n_insn,c->name);
}
static void code_cache_analyze_micro(GmlWin *w,GmlCode *c){
  if(!c) return;
  c->micro_kind=0; c->micro_name=NULL; c->micro_hash=0;
  if(c->n_insn<4 || !c->insn){ micro_debug(w,c,"short"); return; }
  GmlInsn *in=c->insn;
  if(in[0].kind==OP_PUSH && in[0].type1==DT_VAR && in[0].inst==IT_ARG &&
     in[0].refname && !strcmp(in[0].refname,"argument0") &&
     in[1].kind==OP_PUSH && in[1].type1==DT_VAR && in[1].inst==IT_GLOBAL &&
     in[1].refname &&
     in[2].kind==OP_CALL && in[2].argc==2 &&
     in[2].refname && !strcmp(in[2].refname,"ds_map_find_value") &&
     in[3].kind==OP_RET){
    c->micro_kind=GML_MICRO_DS_MAP_GLOBAL_ARG0;
    c->micro_name=in[1].refname;
    c->micro_hash=in[1].refhash?in[1].refhash:gml_value_name_hash(in[1].refname);
    micro_debug(w,c,"ds-map-global-arg0");
    return;
  }
  if(c->n_insn>=28 && c->branch_index &&
     insn_push_arg(&in[0],0) &&
     insn_push_arg(&in[1],1) &&
     in[2].kind==OP_CMP && in[2].cmp==CMP_LT &&
     in[3].kind==OP_BF && c->branch_index[3]==15 &&
     insn_push_arg(&in[4],0) &&
     insn_push_arg(&in[5],2) &&
     in[6].kind==OP_ADD &&
     insn_pop_arg(&in[7],0) &&
     insn_push_arg(&in[8],0) &&
     insn_push_arg(&in[9],1) &&
     in[10].kind==OP_CMP && in[10].cmp==CMP_GT &&
     in[11].kind==OP_BF && c->branch_index[11]==14 &&
     insn_push_arg(&in[12],1) &&
     in[13].kind==OP_RET &&
     in[14].kind==OP_B && c->branch_index[14]==25 &&
     insn_push_arg(&in[15],0) &&
     insn_push_arg(&in[16],2) &&
     in[17].kind==OP_SUB &&
     insn_pop_arg(&in[18],0) &&
     insn_push_arg(&in[19],0) &&
     insn_push_arg(&in[20],1) &&
     in[21].kind==OP_CMP && in[21].cmp==CMP_LT &&
     in[22].kind==OP_BF && c->branch_index[22]==25 &&
     insn_push_arg(&in[23],1) &&
     in[24].kind==OP_RET &&
     insn_push_arg(&in[25],0) &&
     in[26].kind==OP_RET){
    c->micro_kind=GML_MICRO_APPROACH3;
    micro_debug(w,c,"approach3");
    return;
  }
  if(c->n_insn>=4 &&
     insn_push_arg(&in[0],0) &&
     in[1].kind==OP_PUSH && in[1].type1==DT_VAR && in[1].inst==IT_GLOBAL && in[1].refname &&
     in[2].kind==OP_CALL && in[2].argc==2 && in[2].refname &&
     in[3].kind==OP_RET){
    c->micro_kind=GML_MICRO_CALL_GLOBAL_ARG0;
    c->micro_name=in[1].refname;
    c->micro_hash=in[1].refhash?in[1].refhash:gml_value_name_hash(in[1].refname);
    micro_debug(w,c,"call-global-arg0");
    return;
  }
  if(c->n_insn>=41 && c->branch_index &&
     insn_push_arg(&in[0],0) &&
     insn_push_builtin_name(&in[1],"undefined") &&
     in[2].kind==OP_CMP && in[2].cmp==CMP_EQ &&
     in[3].kind==OP_BF && c->branch_index[3]==6 &&
     insn_push_num(&in[4],-1.0) &&
     insn_pop_arg(&in[5],0) &&
     insn_push_global_var(&in[6]) &&
     insn_push_arg(&in[7],0) &&
     insn_call_name(&in[8],"gamepad_set_axis_deadzone",2) &&
     in[9].kind==OP_POPZ &&
     insn_push_global_var(&in[10]) &&
     insn_call_name(&in[11],"ds_map_find_first",1) &&
     insn_pop_local_var(&in[12]) &&
     insn_push_same_ref(&in[13],&in[10]) &&
     insn_call_name(&in[14],"ds_map_size",1) &&
     insn_pop_local_var(&in[15]) &&
     insn_push_num(&in[16],0.0) &&
     insn_pop_local_var(&in[17]) &&
     insn_push_same_ref(&in[18],&in[17]) &&
     insn_push_same_ref(&in[19],&in[15]) &&
     in[20].kind==OP_CMP && in[20].cmp==CMP_LT &&
     in[21].kind==OP_BF && c->branch_index[21]==40 &&
     insn_push_same_ref(&in[22],&in[12]) &&
     insn_push_same_ref(&in[23],&in[10]) &&
     insn_call_name(&in[24],"ds_map_find_value",2) &&
     insn_push_arg(&in[25],0) &&
     in[26].kind==OP_DUP &&
     in[27].kind==OP_DUP &&
     in[28].kind==OP_PUSH && in[28].type1==DT_VAR && in[28].inst==IT_STACK && in[28].refname &&
     in[29].kind==OP_CALLV && in[29].argc==1 &&
     in[30].kind==OP_POPZ &&
     insn_push_same_ref(&in[31],&in[12]) &&
     insn_push_same_ref(&in[32],&in[10]) &&
     insn_call_name(&in[33],"ds_map_find_next",2) &&
     insn_pop_same_ref(&in[34],&in[12]) &&
     insn_push_same_ref(&in[35],&in[17]) &&
     insn_push_num(&in[36],1.0) &&
     in[37].kind==OP_ADD &&
     insn_pop_same_ref(&in[38],&in[17]) &&
     in[39].kind==OP_B && c->branch_index[39]==18 &&
     in[40].kind==OP_EXIT){
    c->micro_kind=GML_MICRO_DS_MAP_METHOD_LOOP1;
    c->micro_name=in[10].refname;
    c->micro_hash=in[10].refhash?in[10].refhash:gml_value_name_hash(in[10].refname);
    micro_debug(w,c,"ds-map-method-loop1");
    return;
  }
  if(c->n_insn>=67 && c->branch_index &&
     insn_push_arg(&in[0],0) &&
     insn_push_builtin_name(&in[1],"undefined") &&
     in[2].kind==OP_CMP && in[2].cmp==CMP_EQ &&
     in[3].kind==OP_BF && c->branch_index[3]==6 &&
     insn_push_num(&in[4],-1.0) &&
     insn_pop_arg(&in[5],0) &&
     insn_push_num(&in[6],0.0) && insn_pop_self_var(&in[7]) &&
     insn_push_num(&in[8],0.0) && insn_pop_self_var(&in[9]) &&
     insn_push_num(&in[10],0.0) && insn_pop_self_var(&in[11]) &&
     insn_push_num(&in[12],0.0) && insn_pop_self_var(&in[13]) &&
     insn_push_num(&in[14],0.0) && insn_pop_local_var(&in[15]) &&
     insn_push_same_ref(&in[16],&in[15]) &&
     insn_push_self_var(&in[17]) &&
     insn_call_name(&in[18],"array_length",1) &&
     in[19].kind==OP_CMP && in[19].cmp==CMP_LT &&
     in[20].kind==OP_BF && c->branch_index[20]==66 &&
     insn_push_num(&in[21],-1.0) &&
     insn_push_same_ref(&in[22],&in[15]) &&
     in[23].kind==OP_CONV &&
     in[24].kind==OP_PUSH && in[24].type1==DT_VAR && in[24].reftype==0x00 &&
     insn_same_name(&in[24],&in[17]) &&
     insn_pop_local_var(&in[25]) &&
     insn_push_same_ref(&in[26],&in[25]) &&
     insn_push_arg(&in[27],0) &&
     in[28].kind==OP_DUP &&
     in[29].kind==OP_DUP &&
     insn_push_stack_var(&in[30]) &&
     in[31].kind==OP_CALLV && in[31].argc==1 &&
     in[32].kind==OP_POPZ &&
     insn_push_same_ref(&in[33],&in[25]) &&
     insn_push_num(&in[34],-9.0) &&
     insn_push_stacktop_var(&in[35]) &&
     in[36].kind==OP_CONV &&
     in[37].kind==OP_BF && c->branch_index[37]==40 &&
     insn_push_num(&in[38],1.0) &&
     insn_pop_self_var(&in[39]) && insn_same_name(&in[39],&in[35]) &&
     insn_push_same_ref(&in[40],&in[25]) &&
     insn_push_num(&in[41],-9.0) &&
     insn_push_stacktop_var(&in[42]) &&
     in[43].kind==OP_CONV &&
     in[44].kind==OP_BF && c->branch_index[44]==47 &&
     insn_push_num(&in[45],1.0) &&
     insn_pop_self_var(&in[46]) && insn_same_name(&in[46],&in[42]) &&
     insn_push_same_ref(&in[47],&in[25]) &&
     insn_push_num(&in[48],-9.0) &&
     insn_push_stacktop_var(&in[49]) &&
     in[50].kind==OP_CONV &&
     in[51].kind==OP_BF && c->branch_index[51]==54 &&
     insn_push_num(&in[52],1.0) &&
     insn_pop_self_var(&in[53]) && insn_same_name(&in[53],&in[49]) &&
     in[54].kind==OP_PUSH && in[54].type1==DT_INT32 &&
     in[55].kind==OP_BREAK &&
     insn_push_same_ref(&in[56],&in[25]) &&
     insn_push_num(&in[57],-1.0) &&
     insn_push_same_ref(&in[58],&in[15]) &&
     in[59].kind==OP_CONV &&
     in[60].kind==OP_POP && in[60].type1==DT_VAR && in[60].reftype==0x00 &&
     insn_same_name(&in[60],&in[17]) &&
     insn_push_same_ref(&in[61],&in[15]) &&
     insn_push_num(&in[62],1.0) &&
     in[63].kind==OP_ADD &&
     insn_pop_same_ref(&in[64],&in[15]) &&
     in[65].kind==OP_B && c->branch_index[65]==16 &&
     in[66].kind==OP_EXIT){
    c->micro_kind=GML_MICRO_ARRAY_METHOD_FLAGS;
    c->micro_name=in[17].refname;
    c->micro_hash=in[17].refhash?in[17].refhash:gml_value_name_hash(in[17].refname);
    micro_debug(w,c,"array-method-flags");
    return;
  }
  if(c->n_insn>=347 && c->branch_index &&
     insn_push_arg(&in[0],0) &&
     insn_push_builtin_name(&in[1],"undefined") &&
     in[2].kind==OP_CMP && in[2].cmp==CMP_EQ &&
     in[3].kind==OP_BF && c->branch_index[3]==6 &&
     insn_push_num(&in[4],-1.0) &&
     insn_pop_arg(&in[5],0) &&
     insn_push_self_var(&in[6]) &&
     in[7].kind==OP_DUP &&
     insn_push_num(&in[8],0.0) &&
     in[9].kind==OP_CMP && in[9].cmp==CMP_EQ &&
     in[10].kind==OP_BT && c->branch_index[10]==20 &&
     in[11].kind==OP_DUP &&
     insn_push_num(&in[12],1.0) &&
     in[13].kind==OP_CMP && in[13].cmp==CMP_EQ &&
     in[14].kind==OP_BT && c->branch_index[14]==72 &&
     in[15].kind==OP_DUP &&
     insn_push_num(&in[16],2.0) &&
     in[17].kind==OP_CMP && in[17].cmp==CMP_EQ &&
     in[18].kind==OP_BT && c->branch_index[18]==131 &&
     in[19].kind==OP_B && c->branch_index[19]==345 &&
     insn_push_self_var(&in[20]) &&
     insn_call_name(&in[21],"is_array",1) &&
     in[22].kind==OP_CONV &&
     in[23].kind==OP_BF && c->branch_index[23]==62 &&
     insn_push_self_var(&in[24]) &&
     insn_pop_local_var(&in[25]) &&
     insn_push_num(&in[26],-1.0) && insn_push_num(&in[27],1.0) &&
     in[28].kind==OP_PUSH && in[28].type1==DT_VAR && in[28].reftype==0x00 && insn_same_name(&in[28],&in[20]) &&
     insn_call_name(&in[29],"keyboard_check",1) &&
     insn_push_num(&in[30],-1.0) && insn_push_num(&in[31],0.0) &&
     in[32].kind==OP_PUSH && in[32].type1==DT_VAR && in[32].reftype==0x00 && insn_same_name(&in[32],&in[20]) &&
     insn_call_name(&in[33],"keyboard_check",1) &&
     in[34].kind==OP_SUB &&
     insn_pop_self_var(&in[35]) &&
     insn_push_num(&in[36],-1.0) && insn_push_num(&in[37],1.0) &&
     in[38].kind==OP_PUSH && in[38].type1==DT_VAR && in[38].reftype==0x00 && insn_same_name(&in[38],&in[20]) &&
     insn_call_name(&in[39],"keyboard_check_pressed",1) &&
     insn_push_num(&in[40],-1.0) && insn_push_num(&in[41],0.0) &&
     in[42].kind==OP_PUSH && in[42].type1==DT_VAR && in[42].reftype==0x00 && insn_same_name(&in[42],&in[20]) &&
     insn_call_name(&in[43],"keyboard_check_pressed",1) &&
     in[44].kind==OP_SUB &&
     insn_push_num(&in[45],0.0) &&
     in[46].kind==OP_CMP && in[46].cmp==CMP_NEQ &&
     insn_pop_self_var(&in[47]) &&
     insn_push_self_var(&in[48]) && insn_same_name(&in[48],&in[35]) &&
     insn_push_num(&in[49],0.0) &&
     in[50].kind==OP_CMP && in[50].cmp==CMP_NEQ &&
     insn_pop_self_var(&in[51]) && insn_same_name(&in[51],&in[24]) &&
     insn_push_self_var(&in[52]) && insn_same_name(&in[52],&in[35]) &&
     insn_push_num(&in[53],0.0) &&
     in[54].kind==OP_CMP && in[54].cmp==CMP_EQ &&
     in[55].kind==OP_BF && c->branch_index[55]==59 &&
     insn_push_same_ref(&in[56],&in[25]) &&
     in[57].kind==OP_CONV &&
     in[58].kind==OP_B && c->branch_index[58]==60 &&
     insn_push_num(&in[59],0.0) &&
     insn_pop_self_var(&in[60]) &&
     in[61].kind==OP_B && c->branch_index[61]==71 &&
     insn_push_self_var(&in[62]) && insn_same_name(&in[62],&in[20]) &&
     insn_call_name(&in[63],"keyboard_check_pressed",1) &&
     insn_pop_self_var(&in[64]) && insn_same_name(&in[64],&in[47]) &&
     insn_push_self_var(&in[65]) && insn_same_name(&in[65],&in[20]) &&
     insn_call_name(&in[66],"keyboard_check",1) &&
     insn_pop_self_var(&in[67]) && insn_same_name(&in[67],&in[24]) &&
     insn_push_self_var(&in[68]) && insn_same_name(&in[68],&in[20]) &&
     insn_call_name(&in[69],"keyboard_check_released",1) &&
     insn_pop_self_var(&in[70]) && insn_same_name(&in[70],&in[60]) &&
     in[71].kind==OP_B && c->branch_index[71]==345 &&
     insn_push_self_var(&in[72]) && insn_same_name(&in[72],&in[20]) &&
     insn_call_name(&in[73],"is_array",1) &&
     in[74].kind==OP_CONV &&
     in[75].kind==OP_BF && c->branch_index[75]==118 &&
     insn_push_self_var(&in[76]) && insn_same_name(&in[76],&in[24]) &&
     insn_pop_local_var(&in[77]) &&
     insn_push_num(&in[78],-1.0) && insn_push_num(&in[79],1.0) &&
     in[80].kind==OP_PUSH && in[80].type1==DT_VAR && in[80].reftype==0x00 && insn_same_name(&in[80],&in[20]) &&
     insn_push_arg(&in[81],0) &&
     insn_call_name(&in[82],"gamepad_button_check",2) &&
     insn_push_num(&in[83],-1.0) && insn_push_num(&in[84],0.0) &&
     in[85].kind==OP_PUSH && in[85].type1==DT_VAR && in[85].reftype==0x00 && insn_same_name(&in[85],&in[20]) &&
     insn_push_arg(&in[86],0) &&
     insn_call_name(&in[87],"gamepad_button_check",2) &&
     in[88].kind==OP_SUB &&
     insn_pop_self_var(&in[89]) && insn_same_name(&in[89],&in[35]) &&
     insn_push_num(&in[90],-1.0) && insn_push_num(&in[91],1.0) &&
     in[92].kind==OP_PUSH && in[92].type1==DT_VAR && in[92].reftype==0x00 && insn_same_name(&in[92],&in[20]) &&
     insn_push_arg(&in[93],0) &&
     insn_call_name(&in[94],"gamepad_button_check_pressed",2) &&
     insn_push_num(&in[95],-1.0) && insn_push_num(&in[96],0.0) &&
     in[97].kind==OP_PUSH && in[97].type1==DT_VAR && in[97].reftype==0x00 && insn_same_name(&in[97],&in[20]) &&
     insn_push_arg(&in[98],0) &&
     insn_call_name(&in[99],"gamepad_button_check_pressed",2) &&
     in[100].kind==OP_SUB &&
     insn_push_num(&in[101],0.0) &&
     in[102].kind==OP_CMP && in[102].cmp==CMP_NEQ &&
     insn_pop_self_var(&in[103]) && insn_same_name(&in[103],&in[47]) &&
     insn_push_self_var(&in[104]) && insn_same_name(&in[104],&in[35]) &&
     insn_push_num(&in[105],0.0) &&
     in[106].kind==OP_CMP && in[106].cmp==CMP_NEQ &&
     insn_pop_self_var(&in[107]) && insn_same_name(&in[107],&in[24]) &&
     insn_push_self_var(&in[108]) && insn_same_name(&in[108],&in[35]) &&
     insn_push_num(&in[109],0.0) &&
     in[110].kind==OP_CMP && in[110].cmp==CMP_EQ &&
     in[111].kind==OP_BF && c->branch_index[111]==115 &&
     insn_push_same_ref(&in[112],&in[77]) &&
     in[113].kind==OP_CONV &&
     in[114].kind==OP_B && c->branch_index[114]==116 &&
     insn_push_num(&in[115],0.0) &&
     insn_pop_self_var(&in[116]) && insn_same_name(&in[116],&in[60]) &&
     in[117].kind==OP_B && c->branch_index[117]==130 &&
     insn_push_self_var(&in[118]) && insn_same_name(&in[118],&in[20]) &&
     insn_push_arg(&in[119],0) &&
     insn_call_name(&in[120],"gamepad_button_check_pressed",2) &&
     insn_pop_self_var(&in[121]) && insn_same_name(&in[121],&in[47]) &&
     insn_push_self_var(&in[122]) && insn_same_name(&in[122],&in[20]) &&
     insn_push_arg(&in[123],0) &&
     insn_call_name(&in[124],"gamepad_button_check",2) &&
     insn_pop_self_var(&in[125]) && insn_same_name(&in[125],&in[24]) &&
     insn_push_self_var(&in[126]) && insn_same_name(&in[126],&in[20]) &&
     insn_push_arg(&in[127],0) &&
     insn_call_name(&in[128],"gamepad_button_check_released",2) &&
     insn_pop_self_var(&in[129]) && insn_same_name(&in[129],&in[60]) &&
     in[130].kind==OP_B && c->branch_index[130]==345 &&
     in[345].kind==OP_POPZ &&
     in[346].kind==OP_EXIT){
    c->micro_kind=GML_MICRO_INPUT_ACTION_UPDATE;
    c->micro_name=in[6].refname;
    c->micro_hash=in[6].refhash?in[6].refhash:gml_value_name_hash(in[6].refname);
    micro_debug(w,c,"input-action-update");
    return;
  }
  if(c->n_insn>=23 && c->branch_index &&
     insn_push_arg(&in[0],1) &&
     insn_push_arg(&in[1],0) &&
     insn_push_global_var(&in[2]) &&
     insn_call_name(&in[3],"ds_map_find_value",2) &&
     insn_call_name(&in[4],"ds_map_find_value",2) &&
     insn_pop_local_var(&in[5]) &&
     insn_push_same_ref(&in[6],&in[5]) &&
     insn_call_name(&in[7],"is_undefined",1) &&
     in[8].kind==OP_CONV &&
     in[9].kind==OP_BF && c->branch_index[9]==17 &&
     insn_push_arg(&in[10],1) &&
     insn_push_string(&in[11]) &&
     in[12].kind==OP_CONV &&
     insn_push_same_ref(&in[13],&in[2]) &&
     insn_call_name(&in[14],"ds_map_find_value",2) &&
     insn_call_name(&in[15],"ds_map_find_value",2) &&
     insn_pop_same_ref(&in[16],&in[5]) &&
     insn_push_same_ref(&in[17],&in[5]) &&
     insn_call_name(&in[18],"is_undefined",1) &&
     in[19].kind==OP_CONV &&
     in[20].kind==OP_BF){
    int ri=c->branch_index[20];
    if(ri>=0 && ri+1<(int)c->n_insn &&
       insn_push_same_ref(&in[ri],&in[5]) &&
       in[ri+1].kind==OP_RET){
      c->micro_kind=GML_MICRO_DS_MAP_NESTED_FALLBACK;
      c->micro_name=in[2].refname;
      c->micro_hash=in[2].refhash?in[2].refhash:gml_value_name_hash(in[2].refname);
      micro_debug(w,c,"ds-map-nested-fallback");
      return;
    }
  }
  micro_debug(w,c,"none");
}
static int code_cache_ensure(GmlWin *w, int ci){
  if(!w || ci<0 || ci>=w->n_code) return 0;
  GmlCode *c=&w->code[ci];
  if(c->cache_bad) return 0;
  if(c->insn && c->insn_pc && c->branch_index) return 1;
  if(c->length==0){ c->n_insn=0; return 1; }
  if(c->start>w->size || c->length>w->size-c->start){ c->cache_bad=1; return 0; }
  uint32_t max=c->length/4u + 1u;
  GmlInsn *ins=calloc(max?max:1,sizeof(*ins));
  uint32_t *pcs=calloc(max?max:1,sizeof(*pcs));
  int32_t *br=calloc(max?max:1,sizeof(*br));
  if(!ins || !pcs || !br){ free(ins); free(pcs); free(br); return 0; }
  for(uint32_t i=0;i<max;i++) br[i]=-1;
  uint32_t pc=c->start, end=c->start+c->length, n=0;
  while(pc<end){
    if(n>=max || pc>w->size || 4u>w->size-pc){ c->cache_bad=1; micro_debug_pc(w,c,"cache-decode-bounds",pc,n); goto fail; }
    GmlInsn in; int sz=gml_decode_bc_bounded(w->data,w->size,pc,w->bytecode,&in);
    if(!sz){ c->cache_bad=1; micro_debug_pc(w,c,"cache-decode-fail",pc,0); goto fail; }
    if((uint32_t)sz>end-pc){
      /* Some GMS2 parent entries end their recorded range in the middle of an embedded child body.
       * Normal execution exits or branches away before that tail; treat it as the cached exit edge. */
      micro_debug_pc(w,c,"cache-truncated-tail",pc,(uint32_t)sz);
      break;
    }
    in.funcval_ci=-1;
    in.builtin_id=0;
    if((in.kind==OP_CALL || in.kind==OP_PUSH || in.kind==OP_POP ||
        (in.kind==OP_BREAK && in.sval==-11)) && in.refaddr){
      in.refname=gml_ref_name(w,in.refaddr);
      if(in.refname) in.refhash=gml_value_name_hash(in.refname);
    }
    if(in.kind==OP_PUSH && in.type1==DT_INT32){
      /* A VARI occurrence on push.i32 names a stable member hash. Its stored
       * operand is an occurrence-chain link, not the value to expose. */
      if(gml_ref_kind(w,pc+4)==GML_REF_VARIABLE){
        in.refname=gml_ref_name(w,pc+4);
        in.refhash=gml_value_name_hash(in.refname);
      }
      /* Resolve function values once at decode: -2 = checked, NOT a function-value. Leaving it -1
       * made the interpreter redo the ref-chain walk + name lookup on every plain push.i32. */
      in.funcval_ci=-2;
      if(!in.refname && anygm_policy_has_modern_function_values(w)){
        const char *fn=gml_ref_name(w,pc+4);
        if(fn && fn[0]!='?' && !strncmp(fn,"gml_",4)){
          int fci=gml_code_index_by_name(w,fn);
          if(fci>=0) in.funcval_ci=fci;
        }
      }
    }
    if(in.kind==OP_BREAK && in.sval==-11 && anygm_policy_has_modern_function_values(w)){
      /* pushref uses the same FUNC occurrence table as push.i32 function values, although its
       * payload also serves as an untagged resource id. Resolve only an actual CODE name. */
      in.funcval_ci=-2;
      const char *fn=in.refname;
      if(fn && fn[0]!='?' && !strncmp(fn,"gml_",4)){
        int fci=gml_code_index_by_name(w,fn);
        if(fci>=0) in.funcval_ci=fci;
      }
    }
    ins[n]=in; pcs[n]=pc; n++;
    pc+=(uint32_t)sz;
  }
  c->insn=ins; c->insn_pc=pcs; c->branch_index=br; c->n_insn=n;
  for(uint32_t i=0;i<n;i++){
    if(!code_cache_branch_op(ins[i].kind)) continue;
    int64_t target64=(int64_t)pcs[i] + (int64_t)ins[i].jump*4;
    if(target64>=(int64_t)end){ br[i]=(int32_t)n; continue; }
    if(target64<(int64_t)c->start){ c->cache_bad=1; micro_debug_pc(w,c,"cache-branch-before",pcs[i],(uint32_t)i); goto fail_live; }
    int ti=code_cache_find_pc(c,(uint32_t)target64);
    if(ti<0){ c->cache_bad=1; micro_debug_pc(w,c,"cache-branch-miss",pcs[i],(uint32_t)target64); goto fail_live; }
    br[i]=ti;
  }
  code_cache_analyze_micro(w,c);
  return 1;
fail_live:
  code_cache_free(c);
  return 0;
fail:
  free(ins); free(pcs); free(br);
  return 0;
}
int gml_vm_code_cache_ensure(GmlWin *win, int code_index){
  return code_cache_ensure(win,code_index);
}

/* Optional per-code wall time and invocation counts, owned by the VM. */
static int codeprof_on(GmlVM *vm){
  if(!vm) return 0;
  if(vm->diagnostics.code_profile<0){
    const char *v=anygm_host_development_setting(vm->host,"GML_PROFILE_CODE");
    vm->diagnostics.code_profile=(v && *v)?1:0;
  }
  return vm->diagnostics.code_profile;
}
static double codeprof_now_ms(GmlVM *vm){
  /* Obtain time through host services rather than directly from the operating system. */
  return vm ? (double)anygm_host_monotonic_time_ns(vm->host)/1000000.0 : 0.0;
}
static void codeprof_add(GmlVM *vm, int ci, double ms, uint64_t insn){
  (void)insn;
  if(!vm || !vm->win || ci<0 || ci>=vm->win->n_code) return;
  if(!vm->diagnostics.code_profile_ms){
    vm->diagnostics.code_profile_ms=calloc((size_t)vm->win->n_code,sizeof(double));
    vm->diagnostics.code_profile_hits=calloc((size_t)vm->win->n_code,sizeof(uint64_t));
    if(!vm->diagnostics.code_profile_ms || !vm->diagnostics.code_profile_hits) return;
  }
  vm->diagnostics.code_profile_ms[ci]+=ms;
  vm->diagnostics.code_profile_hits[ci]++;
}
void gml_vm_code_profile_report(GmlVM *vm){
  if(!vm || !vm->win || !vm->diagnostics.code_profile_ms) return;
  int n=vm->win->n_code, shown=0;
  for(int round=0;round<12;round++){
    int best=-1;
    for(int i=0;i<n;i++){
      if(vm->diagnostics.code_profile_ms[i]<=0) continue;
      if(best<0 || vm->diagnostics.code_profile_ms[i]>vm->diagnostics.code_profile_ms[best]) best=i;
    }
    if(best<0) break;
    anygm_host_logf(vm->host,ANYGM_LOG_DEBUG,"[codeprof] %10.1f ms  %8llu calls  %s\n",
      vm->diagnostics.code_profile_ms[best],
      (unsigned long long)vm->diagnostics.code_profile_hits[best],
      vm->win->code[best].name?vm->win->code[best].name:"");
    vm->diagnostics.code_profile_ms[best]=-1;   /* consumed, so the next round finds the next */
    shown++;
  }
  (void)shown;
}

/* ---------------- builtins ---------------- */
int gml_input_key(GmlVM *vm,int key,int edge){
  return vm&&vm->input.key?vm->input.key(vm->input.userdata,key,edge):0;
}
void gml_input_key_clear(GmlVM *vm,int key){
  if(vm&&vm->input.key_clear) vm->input.key_clear(vm->input.userdata,key);
}
void gml_input_key_press(GmlVM *vm,int key){
  if(vm&&vm->input.key_press) vm->input.key_press(vm->input.userdata,key);
}
void gml_input_key_release(GmlVM *vm,int key){
  if(vm&&vm->input.key_release) vm->input.key_release(vm->input.userdata,key);
}
int gml_input_gamepad(GmlVM *vm,int button,int edge){
  return vm&&vm->input.gamepad?vm->input.gamepad(vm->input.userdata,button,edge):0;
}
int gml_input_gamepad_connected(GmlVM *vm,int device){
  return vm&&vm->input.gamepad_connected?
    vm->input.gamepad_connected(vm->input.userdata,device):0;
}
int gml_input_gamepad_device_count(GmlVM *vm){
  return vm&&vm->input.gamepad_device_count?
    vm->input.gamepad_device_count(vm->input.userdata):0;
}
double gml_input_gamepad_axis(GmlVM *vm,int device,int axis){
  return vm&&vm->input.gamepad_axis?
    vm->input.gamepad_axis(vm->input.userdata,device,axis):0.0;
}
void gml_input_gamepad_set_vibration(GmlVM *vm,int device,double low,double high){
  if(vm&&vm->input.gamepad_vibration)
    vm->input.gamepad_vibration(vm->input.userdata,device,low,high);
}
void gml_input_mouse(GmlVM *vm,double *room_x,double *room_y,double *gui_x,double *gui_y,
                     double *window_x,double *window_y,int *held,int *pressed,int *released,int *wheel){
  if(vm&&vm->input.mouse){
    vm->input.mouse(vm->input.userdata,room_x,room_y,gui_x,gui_y,window_x,window_y,
                    held,pressed,released,wheel);
    return;
  }
  if(room_x) *room_x=0;
  if(room_y) *room_y=0;
  if(gui_x) *gui_x=0;
  if(gui_y) *gui_y=0;
  if(window_x) *window_x=0;
  if(window_y) *window_y=0;
  if(held) *held=0;
  if(pressed) *pressed=0;
  if(released) *released=0;
  if(wheel) *wheel=0;
}
void gml_input_mouse_set(GmlVM *vm,double x,double y){
  if(vm&&vm->input.mouse_set) vm->input.mouse_set(vm->input.userdata,x,y);
}
static int code_micro_maybe(GmlVM *vm, int ci, GmlVal *args, int n_args, GmlVal *out);
static inline int builtin_hotprof_on(GmlVM *vm){
  if(vm->diagnostics.hot_builtin_profile<0)
    vm->diagnostics.hot_builtin_profile=anygm_host_development_setting(vm->host,"GML_PROFILE_HOTBUILTIN")!=NULL;
  return vm->diagnostics.hot_builtin_profile;
}
static int vm_heap_string(GmlVal v){
  return v.t==V_STR && v.s && v.d!=0;
}
static void micro_set_bool_fields(GmlVM *vm, GmlInstance *self,
                                  const GmlInsn *pressed_in, const GmlInsn *held_in, const GmlInsn *released_in,
                                  int pressed, int held, int released){
  inst_set_any_h(vm,self,pressed_in->refname,pressed_in->refhash?pressed_in->refhash:gml_value_name_hash(pressed_in->refname),vreal(pressed?1:0));
  inst_set_any_h(vm,self,held_in->refname,held_in->refhash?held_in->refhash:gml_value_name_hash(held_in->refname),vreal(held?1:0));
  inst_set_any_h(vm,self,released_in->refname,released_in->refhash?released_in->refhash:gml_value_name_hash(released_in->refname),vreal(released?1:0));
}
static int micro_input_edge(GmlVM *vm, int type, int key, int edge){
  return type==0 ? gml_keyboard_check(vm,key,edge) : gml_input_gamepad(vm,key,edge);
}
static void micro_call_method_field1(GmlVM *vm, GmlVal targetv, const char *field, uint32_t hash, GmlVal arg0){
  if(!vm || !field) return;
  GmlInstance *target=vm_inst_from_ref(vm,targetv);
  if(!target) return;
  GmlVal mv=inst_get_any_h(vm,target,field,hash);
  GmlVal a[1]={arg0};
  GmlVal rv=gml_vm_call_member_callable(vm,targetv,mv,a,1);
  if(vm_heap_string(rv) && !(arg0.t==V_STR && arg0.s==rv.s)) free((char*)rv.s);
}
static int code_micro_try(GmlVM *vm, int ci, GmlVal *args, int n_args, GmlVal *out){
  if(!vm || !vm->win || ci<0 || ci>=vm->win->n_code || !out) return 0;
  GmlCode *c=&vm->win->code[ci];
  const char *trace=vm_trace_filter(vm);
  if(trace && *trace && c->name && strstr(c->name,trace)) return 0;
  if(GML_VM_DIAGNOSTIC_OPCODE_ENABLED(vm,c->name)) return 0;
  if(!code_cache_ensure(vm->win,ci)) return 0;
  if(c->micro_kind==GML_MICRO_NONE) return 0;
  if(builtin_hotprof_on(vm)) return 0;
  const char *arglog=anygm_host_development_setting(vm->host,"GML_LOG_CODE_ARGS");
  if(arglog && *arglog && c->name && strstr(c->name,arglog)) return 0;
  double t0=codeprof_on(vm)?codeprof_now_ms(vm):0.0;
  if(c->micro_kind==GML_MICRO_DS_MAP_GLOBAL_ARG0 && c->micro_name){
    GmlVal *map=gml_varmap_get_hashed(&vm->globals,c->micro_name,c->micro_hash);
    *out = gml_ds_map_find_value_direct(vm,(int)(map?asnum(*map):0.0),
      (args && n_args>0)?args[0]:vundef(), args && n_args>0);
    gml_arr_mark_escaped(*out);
    if(codeprof_on(vm)) codeprof_add(vm,ci,codeprof_now_ms(vm)-t0,4);
    return 1;
  }
  if(c->micro_kind==GML_MICRO_APPROACH3){
    double cur=(args && n_args>0)?asnum(args[0]):0.0;
    double target=(args && n_args>1)?asnum(args[1]):0.0;
    double step=(args && n_args>2)?asnum(args[2]):0.0;
    if(cur<target){
      cur+=step;
      if(cur>target) cur=target;
    } else {
      cur-=step;
      if(cur<target) cur=target;
    }
    *out=vreal(cur);
    if(codeprof_on(vm)) codeprof_add(vm,ci,codeprof_now_ms(vm)-t0,28);
    return 1;
  }
  if(c->micro_kind==GML_MICRO_CALL_GLOBAL_ARG0 && c->micro_name && c->insn){
    int fci=c->insn[2].funcval_ci;
    if(fci==-1){
      fci=gml_code_index_by_name(vm->win,c->insn[2].refname);
      c->insn[2].funcval_ci=(fci>=0)?fci:-2;
    }
    if(fci>=0 && fci<vm->win->n_code){
      GmlVal *gv=gml_varmap_get_hashed(&vm->globals,c->micro_name,c->micro_hash);
      GmlVal a[2]={ gv?*gv:vreal(0), (args && n_args>0)?args[0]:vundef() };
      *out=gml_vm_run_code(vm,fci,vm->cur_self,vm->cur_other,a,2);
      if(codeprof_on(vm)) codeprof_add(vm,ci,codeprof_now_ms(vm)-t0,4);
      return 1;
    }
  }
  if(c->micro_kind==GML_MICRO_DS_MAP_METHOD_LOOP1 && c->micro_name && c->insn){
    GmlVal arg0=(args && n_args>0)?args[0]:vundef();
    if(arg0.t==V_UNDEF) arg0=vreal(-1);
    GmlInsn *in=c->insn;
    GmlVal *dz=gml_varmap_get_hashed(&vm->globals,in[6].refname,in[6].refhash?in[6].refhash:gml_value_name_hash(in[6].refname));
    gml_gamepad_set_axis_deadzone_direct(vm,(int)asnum(arg0),dz?asnum(*dz):0.0);
    GmlVal *mapv=gml_varmap_get_hashed(&vm->globals,c->micro_name,c->micro_hash);
    int mapid=(int)(mapv?asnum(*mapv):0.0);
    GmlVal key=gml_ds_map_find_first_direct(vm,mapid);
    int num=gml_ds_map_size_direct(vm,mapid);
    const char *method=in[28].refname;
    uint32_t method_hash=in[28].refhash?in[28].refhash:gml_value_name_hash(method);
    for(int i=0;i<num;i++){
      GmlVal target=gml_ds_map_find_value_direct(vm,mapid,key,1);
      micro_call_method_field1(vm,target,method,method_hash,arg0);
      key=gml_ds_map_find_next_direct(vm,mapid,key,1);
    }
    *out=vreal(0);
    if(codeprof_on(vm)) codeprof_add(vm,ci,codeprof_now_ms(vm)-t0,41);
    return 1;
  }
  if(c->micro_kind==GML_MICRO_DS_MAP_NESTED_FALLBACK && c->micro_name && c->insn){
    if(anygm_host_development_setting(vm->host,"GML_LOG_DS")) return 0;
    GmlVal key0=(args && n_args>0)?args[0]:vundef();
    GmlVal key1=(args && n_args>1)?args[1]:vundef();
    GmlVal *rootv=gml_varmap_get_hashed(&vm->globals,c->micro_name,c->micro_hash);
    int root=(int)(rootv?asnum(*rootv):0.0);
    GmlVal inner=gml_ds_map_find_value_direct(vm,root,key0,1);
    if(inner.t!=V_UNDEF){
      GmlVal val=gml_ds_map_find_value_direct(vm,(int)asnum(inner),key1,1);
      if(val.t!=V_UNDEF){
        *out=val;
        gml_arr_mark_escaped(*out);
        if(codeprof_on(vm)) codeprof_add(vm,ci,codeprof_now_ms(vm)-t0,16);
        return 1;
      }
    }
    const char *fb=gml_str_by_index(vm->win,c->insn[11].strindex);
    inner=gml_ds_map_find_value_direct(vm,root,vstr(fb?fb:""),1);
    if(inner.t!=V_UNDEF){
      GmlVal val=gml_ds_map_find_value_direct(vm,(int)asnum(inner),key1,1);
      if(val.t!=V_UNDEF){
        *out=val;
        gml_arr_mark_escaped(*out);
        if(codeprof_on(vm)) codeprof_add(vm,ci,codeprof_now_ms(vm)-t0,16);
        return 1;
      }
    }
    return 0;
  }
  if(c->micro_kind==GML_MICRO_ARRAY_METHOD_FLAGS && c->micro_name && c->insn){
    GmlInstance *self=vm->cur_self;
    if(!self) return 0;
    GmlInsn *in=c->insn;
    GmlVal arg0=(args && n_args>0)?args[0]:vundef();
    if(arg0.t==V_UNDEF) arg0=vreal(-1);
    const char *arr_name=in[17].refname;
    uint32_t arr_hash=c->micro_hash;
    const char *method=in[30].refname;
    uint32_t method_hash=in[30].refhash?in[30].refhash:gml_value_name_hash(method);
    const char *flag0=in[7].refname, *flag1=in[9].refname, *flag2=in[11].refname, *field3=in[13].refname;
    uint32_t flag0_hash=in[7].refhash?in[7].refhash:gml_value_name_hash(flag0);
    uint32_t flag1_hash=in[9].refhash?in[9].refhash:gml_value_name_hash(flag1);
    uint32_t flag2_hash=in[11].refhash?in[11].refhash:gml_value_name_hash(flag2);
    uint32_t field3_hash=in[13].refhash?in[13].refhash:gml_value_name_hash(field3);
    inst_set_any_h(vm,self,flag0,flag0_hash,vreal(0));
    inst_set_any_h(vm,self,flag1,flag1_hash,vreal(0));
    inst_set_any_h(vm,self,flag2,flag2_hash,vreal(0));
    inst_set_any_h(vm,self,field3,field3_hash,vreal(0));
    GmlVal arr=inst_get_any_h(vm,self,arr_name,arr_hash);
    if(arr.t==V_ARR && arr.arr){
      GmlArr *A=(GmlArr*)arr.arr;
      int len=A->len;
      if(len<0 || A->cap<len || A->cap>16000000 || !A->data) return 0;
      for(int i=0;i<len;i++){
        GmlVal item=A->data[i];
        micro_call_method_field1(vm,item,method,method_hash,arg0);
        GmlInstance *st=vm_inst_from_ref(vm,item);
        if(st){
          if(astrue(inst_get_any_h(vm,st,flag0,flag0_hash))) inst_set_any_h(vm,self,flag0,flag0_hash,vreal(1));
          if(astrue(inst_get_any_h(vm,st,flag1,flag1_hash))) inst_set_any_h(vm,self,flag1,flag1_hash,vreal(1));
          if(astrue(inst_get_any_h(vm,st,flag2,flag2_hash))) inst_set_any_h(vm,self,flag2,flag2_hash,vreal(1));
        }
        array_set_inst_field_h(vm,self,arr_name,arr_hash,i,item);
      }
    }
    *out=vreal(0);
    if(codeprof_on(vm)) codeprof_add(vm,ci,codeprof_now_ms(vm)-t0,67);
    return 1;
  }
  if(c->micro_kind==GML_MICRO_INPUT_ACTION_UPDATE && c->insn){
    GmlInstance *self=vm->cur_self;
    if(!self) return 0;
    GmlInsn *in=c->insn;
    GmlVal typev=inst_get_any_h(vm,self,in[6].refname,in[6].refhash?in[6].refhash:gml_value_name_hash(in[6].refname));
    int type=(int)asnum(typev);
    if(type!=0 && type!=1) return 0;
    if(type==1 && anygm_host_development_setting(vm->host,"GML_DBG_GP")) return 0;
    GmlVal value=inst_get_any_h(vm,self,in[20].refname,in[20].refhash?in[20].refhash:gml_value_name_hash(in[20].refname));
    if(value.t==V_ARR && value.arr){
      int a0=(int)asnum(gml_arr_get(value,0));
      int a1=(int)asnum(gml_arr_get(value,1));
      int axis=micro_input_edge(vm,type,a1,0)-micro_input_edge(vm,type,a0,0);
      int pressed=(micro_input_edge(vm,type,a1,1)-micro_input_edge(vm,type,a0,1))!=0;
      int held=axis!=0;
      int released=(axis==0) && astrue(inst_get_any_h(vm,self,in[24].refname,in[24].refhash?in[24].refhash:gml_value_name_hash(in[24].refname)));
      inst_set_any_h(vm,self,in[35].refname,in[35].refhash?in[35].refhash:gml_value_name_hash(in[35].refname),vreal(axis));
      micro_set_bool_fields(vm,self,&in[47],&in[24],&in[60],pressed,held,released);
    } else {
      int key=(int)asnum(value);
      int pressed=micro_input_edge(vm,type,key,1);
      int held=micro_input_edge(vm,type,key,0);
      int released=micro_input_edge(vm,type,key,2);
      micro_set_bool_fields(vm,self,&in[47],&in[24],&in[60],pressed,held,released);
    }
    *out=vreal(0);
    if(codeprof_on(vm)) codeprof_add(vm,ci,codeprof_now_ms(vm)-t0,42);
    return 1;
  }
  return 0;
}
static int code_micro_maybe(GmlVM *vm, int ci, GmlVal *args, int n_args, GmlVal *out){
  if(!vm || !vm->win || ci<0 || ci>=vm->win->n_code) return 0;
  GmlCode *c=&vm->win->code[ci];
  if(c->cache_bad || (c->insn && c->micro_kind==GML_MICRO_NONE)) return 0;
  return code_micro_try(vm,ci,args,n_args,out);
}

/* Some classic extension packages exported small helpers under these names.  The software
 * runtime supplies portable fallbacks for projects whose extension code is unavailable, but a
 * project may also contain a real script with the same name.  That script is the authoritative
 * implementation; only fall back to the compatibility builtin when no exact script resource
 * exists.  Keep this deliberately limited to names invented by our legacy-compat layer -- native
 * builtins retain their normal dispatch precedence. */
static int classic_extension_script_code(GmlVM *vm,const char *name){
  if(!vm || !vm->win || !anygm_policy_uses_classic_runtime(vm->win) || !name) return -1;
  if(strcmp(name,"crear") && strcmp(name,"depthy") && strcmp(name,"move_rpg") &&
     strcmp(name,"direction_rpg") && strcmp(name,"friction_platform") &&
     strcmp(name,"destruir") &&
     strcmp(name,"draw_full_sprite") && strcmp(name,"draw_shadow") &&
     strcmp(name,"draw_shadow_ext")) return -1;
  char code_name[192];
  snprintf(code_name,sizeof code_name,"gml_Script_%s",name);
  return gml_code_index_by_name(vm->win,code_name);
}

typedef enum {
  GML_DRAW_LOOP_CONSTANT,
  GML_DRAW_LOOP_SCALAR,
  GML_DRAW_LOOP_COUNTER,
  GML_DRAW_LOOP_ARRAY
} GmlDrawLoopArgumentKind;

typedef struct {
  GmlDrawLoopArgumentKind kind;
  GmlVal value;
  GmlArr *array;
} GmlDrawLoopArgument;

static int draw_loop_numeric_push(const GmlInsn *instruction,double *value){
  if(!instruction || !value || instruction->kind!=OP_PUSH) return 0;
  switch(instruction->type1){
    case DT_INT16: *value=(double)instruction->sval; return 1;
    case DT_INT32: *value=(double)instruction->ival; return 1;
    case DT_INT64: *value=(double)instruction->lval; return 1;
    case DT_DOUBLE: *value=instruction->dval; return 1;
    default: return 0;
  }
}

static int draw_loop_normal_variable_push(const GmlInsn *instruction){
  return instruction && instruction->kind==OP_PUSH &&
         instruction->type1==DT_VAR && instruction->reftype==0xA0 &&
         instruction->inst!=IT_STACK && instruction->refname;
}

static int draw_loop_variable_value(
    GmlVM *vm,GmlVarMap *locals,const GmlInsn *instruction,GmlVal *value){
  if(!draw_loop_normal_variable_push(instruction) || !value) return 0;
  const char *name=instruction->refname;
  uint32_t hash=instruction->refhash
    ? instruction->refhash : gml_value_name_hash(name);
  if(instruction->inst==IT_LOCAL){
    if(argument_get(vm,name,value)) return 1;
    GmlVal *slot=gml_varmap_get_hashed(locals,name,hash);
    if(!slot) return 0;
    *value=*slot;
    return 1;
  }
  *value=var_get_h(vm,instruction->inst,name,hash);
  return 1;
}

static int draw_loop_parse_argument(
    GmlVM *vm,GmlVarMap *locals,GmlInsn *instructions,
    uint32_t *cursor,uint32_t end,const GmlInsn *counter,
    GmlDrawLoopArgument *argument){
  if(!vm || !locals || !instructions || !cursor || !counter || !argument ||
     *cursor>=end) return 0;
  uint32_t current=*cursor;
  GmlInsn *first=&instructions[current];
  double number=0.0;
  memset(argument,0,sizeof *argument);

  if(first->kind==OP_BREAK && first->sval==-11 && first->funcval_ci<0){
    argument->kind=GML_DRAW_LOOP_CONSTANT;
    argument->value=vreal((double)(first->ival&0x00FFFFFF));
    *cursor=current+1;
    return 1;
  }

  if(draw_loop_numeric_push(first,&number)){
    uint32_t index=current+1;
    if(index<end && insn_push_same_ref(&instructions[index],counter)){
      index++;
      if(index<end && instructions[index].kind==OP_CONV) index++;
      if(index>=end) return 0;
      GmlInsn *array_push=&instructions[index];
      if(array_push->kind!=OP_PUSH || array_push->type1!=DT_VAR ||
         array_push->reftype!=0x00 || !array_push->refname ||
         number!=(double)(int)number || number<INT_MIN || number>INT_MAX)
        return 0;
      const char *name=array_push->refname;
      uint32_t hash=array_push->refhash
        ? array_push->refhash : gml_value_name_hash(name);
      if(var_name_maybe_special(vm,name,hash)) return 0;
      GmlVarMap *owner=is_room_global_array(name)
        ? &vm->globals : scope_map(vm,locals,(int)number);
      GmlVal *slot=owner?gml_varmap_get_hashed(owner,name,hash):NULL;
      if(!slot || slot->t!=V_ARR || !slot->arr) return 0;
      GmlArr *array=slot->arr;
      if(array->nested_2d || !array->data || array->len<0 ||
         array->cap<array->len || array->cap>16000000) return 0;
      argument->kind=GML_DRAW_LOOP_ARRAY;
      argument->array=array;
      *cursor=index+1;
      return 1;
    }
    argument->kind=GML_DRAW_LOOP_CONSTANT;
    argument->value=vreal(number);
    current++;
    if(current<end && instructions[current].kind==OP_CONV) current++;
    *cursor=current;
    return 1;
  }

  if(draw_loop_normal_variable_push(first)){
    if(insn_same_ref(first,counter)){
      argument->kind=GML_DRAW_LOOP_COUNTER;
    } else {
      argument->kind=GML_DRAW_LOOP_SCALAR;
      if(!draw_loop_variable_value(vm,locals,first,&argument->value)) return 0;
    }
    current++;
    if(current<end && instructions[current].kind==OP_CONV) current++;
    *cursor=current;
    return 1;
  }
  return 0;
}

/* A common generated draw loop consists only of a local counter, immutable scalar/array reads,
 * and draw_sprite_ext. Recognize the complete control-flow shape before doing any work, then
 * execute the same builtin calls without rebuilding the VM operand stack for every element.
 * Every unsupported expression falls back to the ordinary interpreter. */
static int vm_try_array_draw_loop(
    GmlVM *vm,GmlVarMap *locals,GmlInsn *instructions,int32_t *branches,
    uint32_t count,uint32_t header,uint64_t watchdog,uint64_t watchdog_max,
    uint32_t *exit_out,uint64_t *instruction_count_out){
  if(!vm || !locals || !instructions || !branches || !exit_out ||
     !instruction_count_out || header+4>count) return 0;
  GmlInsn *counter=&instructions[header];
  GmlInsn *limit=&instructions[header+1];
  GmlInsn *compare=&instructions[header+2];
  GmlInsn *branch_false=&instructions[header+3];
  if(!draw_loop_normal_variable_push(counter) || counter->inst!=IT_LOCAL ||
     compare->kind!=OP_CMP || compare->cmp!=CMP_LT ||
     branch_false->kind!=OP_BF) return 0;
  int exit=branches[header+3];
  if(exit<0 || (uint32_t)exit>count || (uint32_t)exit<header+11) return 0;
  uint32_t footer=(uint32_t)exit-7;
  GmlInsn *call=&instructions[footer];
  if(!insn_call_name(call,"draw_sprite_ext",9) ||
     instructions[footer+1].kind!=OP_POPZ ||
     !insn_push_same_ref(&instructions[footer+2],counter) ||
     !insn_push_num(&instructions[footer+3],1.0) ||
     instructions[footer+4].kind!=OP_ADD ||
     !insn_pop_same_ref(&instructions[footer+5],counter) ||
     instructions[footer+6].kind!=OP_B ||
     branches[footer+6]!=(int32_t)header)
    return 0;

  uint32_t counter_hash=counter->refhash
    ? counter->refhash : gml_value_name_hash(counter->refname);
  GmlVal *counter_slot=gml_varmap_get_hashed(locals,counter->refname,counter_hash);
  GmlVal limit_value;
  if(!counter_slot || counter_slot->t!=V_REAL ||
     !draw_loop_variable_value(vm,locals,limit,&limit_value) ||
     limit_value.t!=V_REAL || !isfinite(counter_slot->d) ||
     !isfinite(limit_value.d)) return 0;

  GmlDrawLoopArgument arguments[9];
  uint32_t cursor=header+4;
  int argument_count=0;
  while(cursor<footer && argument_count<9){
    if(!draw_loop_parse_argument(
         vm,locals,instructions,&cursor,footer,counter,
         &arguments[argument_count])) return 0;
    argument_count++;
  }
  if(cursor!=footer || argument_count!=9) return 0;

  int builtin_id=call->builtin_id;
  if(builtin_id==0){
    builtin_id=gml_builtin_fast_id(vm,call->refname);
    call->builtin_id=(int16_t)builtin_id;
  }
  if(builtin_id<=0 || classic_extension_script_code(vm,call->refname)>=0) return 0;

  double epsilon=anygm_policy_exact_comparisons(vm->win)?0.0:vm->math_epsilon;
  double probe=counter_slot->d;
  uint64_t iterations=0;
  uint64_t loop_instructions=(uint64_t)exit-header;
  while(gml_real_compare_epsilon(probe,limit_value.d,CMP_LT,epsilon)){
    if(iterations>=watchdog_max/loop_instructions) return 0;
    iterations++;
    probe+=1.0;
    if(!isfinite(probe)) return 0;
  }
  uint64_t consumed=iterations*loop_instructions+4;
  if(consumed==0 || watchdog>watchdog_max ||
     consumed-1>watchdog_max-watchdog) return 0;

  double counter_value=counter_slot->d;
  for(uint64_t iteration=0;iteration<iterations;iteration++){
    int index=(int)counter_value;
    GmlVal call_arguments[9];
    for(int pushed=0;pushed<9;pushed++){
      GmlDrawLoopArgument *argument=&arguments[pushed];
      GmlVal value=vreal(0);
      if(argument->kind==GML_DRAW_LOOP_ARRAY){
        GmlArr *array=argument->array;
        if(index>=0 && index<array->len) value=array->data[index];
      } else if(argument->kind==GML_DRAW_LOOP_COUNTER){
        value=vreal(counter_value);
      } else {
        value=argument->value;
      }
      call_arguments[8-pushed]=value;
    }
    (void)gml_builtin_call_fast_id(
      vm,builtin_id,call->refname,call_arguments,9);
    counter_value+=1.0;
    counter_slot->t=V_REAL;
    counter_slot->d=counter_value;
    counter_slot->s=NULL;
    counter_slot->arr=NULL;
  }
  *exit_out=(uint32_t)exit;
  *instruction_count_out=consumed;
  return 1;
}

/* ---------------- interpreter ---------------- */
#define STK 512
/* The encoded operand stack is byte-sized even though this interpreter stores every logical
 * value in one GmlVal slot. DUP operands use units of their encoded data type, so retain that
 * type beside each slot to duplicate mixed-width references correctly. */
static int vm_stack_type_size(uint8_t type){
  switch(type){
    case DT_DOUBLE: case DT_INT64: return 8;
    case DT_VAR: return 16;
    case DT_FLOAT: case DT_INT32: case DT_BOOL: case DT_STRING: case DT_INT16: return 4;
    default: return 4;
  }
}

static int vm_stack_type_bias(uint8_t type){
  if(type==DT_VAR) return 2;
  if(type==DT_DOUBLE || type==DT_FLOAT || type==DT_INT64) return 1;
  return 0;
}

static uint8_t vm_math_result_type(uint8_t left, uint8_t right){
  int lb=vm_stack_type_bias(left), rb=vm_stack_type_bias(right);
  if(lb!=rb) return lb>rb?left:right;
  return left<right?left:right;
}

static void vm_stack_reverse(GmlVal *values, uint8_t *types, int first, int last){
  for(last--; first<last; first++,last--){
    GmlVal value=values[first]; values[first]=values[last]; values[last]=value;
    uint8_t type=types[first]; types[first]=types[last]; types[last]=type;
  }
}

/* Function-value encoding: a GMS2.3 script/method reference pushed on the value stack (via a
 * `push.i32 <FUNC-ref>` or method()) is represented as a plain real tagged with GML_FUNCVAL_TAG in
 * its high bits and the CODE-entry index in the low 24. OP_CALLV recovers the index and runs it.
 * The tag (0x40000000, ~1.07e9) is far above any real script/asset id or gameplay number, and code
 * indices are < ~1000, so the encoding is unambiguous. */
static int with_newest_first_cmp(const void *aa, const void *bb){
  const GmlInstance *a=*(GmlInstance *const *)aa;
  const GmlInstance *b=*(GmlInstance *const *)bb;
  if(a->creation_seq<b->creation_seq) return 1;
  if(a->creation_seq>b->creation_seq) return -1;
  return a->id<b->id ? 1 : a->id>b->id ? -1 : 0;
}

GmlVal gml_vm_run_code(GmlVM *vm, int ci, GmlInstance *self, GmlInstance *other,
                       GmlVal *args, int n_args){
  if(ci<0||ci>=vm->win->n_code) return vreal(0);
  /* Each frame is roughly 18 KiB; 300 levels remain below a typical 8 MiB C stack. */
  if(vm->execution_depth>=300) return vreal(0);
  vm->execution_depth++;
  GmlWin *w=vm->win; const uint8_t *d=w->data;
  uint32_t start=w->code[ci].start, end=start+w->code[ci].length;
  GmlInstance *save_self=vm->cur_self, *save_other=vm->cur_other;
  int save_code_index=vm->cur_code_index;
  int save_caller_index=vm->caller_code_index;
  unsigned save_call_seq=vm->call_seq;
  GmlVal save_args[16]; int save_argc=vm->script_argc;
  for(int i=0;i<16;i++) save_args[i]=vm->script_args[i];
  vm->cur_self=self; vm->cur_other=other;
  vm->cur_code_index=ci;
  vm->caller_code_index=save_code_index;
  /* VM-owned sequence distinguishes separate code invocations. */
  vm->call_seq=++vm->call_seq_next;
  if(vm->code_depth<16) vm->code_stack[vm->code_depth]=ci;
  vm->code_depth++;
  GmlVarMap locals={0};
  int argc=n_args<0?0:(n_args<16?n_args:16);
  vm->script_argc=argc;
  for(int i=0;i<16;i++) vm->script_args[i]=(i<argc && args)? args[i] : vundef();
  const char *arglog=anygm_host_development_setting(vm->host,"GML_LOG_CODE_ARGS");
  if(arglog && *arglog && strstr(w->code[ci].name,arglog)){
    
    anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[args] f%ld %s argc=%d self=%u obj=%d:",vm->frame,w->code[ci].name,argc,self?self->id:0,self?self->obj:-1);
    for(int i=0;i<argc && i<16;i++){ anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG," a%d=",i); log_val_simple(vm,vm->script_args[i]); }
    anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"\n");
  }
  /* Array arguments may alias storage in caller and callee scopes. Mark them escaped
   * so local cleanup leaves shared arrays for deduplicated full teardown. */
  for(int i=0;i<argc;i++) if(vm->script_args[i].t==V_ARR) gml_arr_mark_escaped(vm->script_args[i]);

  GmlVal stk[STK]; uint8_t stkt[STK]; int sp=0;
  /* Newer array compound assignment, including nested a[i][j] op= v, emits
   * savearef(-8) before the read and restorearef(-9)+popaf(-3) for the write. The reference is
   * duplicated by a dup that, for nested arrays, fails here (its ncopy encoding is misread), so
   * pushaf(-2) consumed the array and index and popaf had nothing to write back. Save the pair at
   * savearef and use it at popaf, restoring the stack to just below the reference. */
  struct { GmlArr *arr; int idx; int base; } aref[16]; int aref_n=0;
  uint32_t pc=start;
  GmlVal ret=vreal(0);
  int classic_implicit_return=anygm_policy_uses_classic_runtime(w) && w->code[ci].name &&
    !strncmp(w->code[ci].name,"gml_Script_",11);
  /* with-statement (pushenv/popenv) loop frames */
  struct { GmlInstance **list; int n, idx; GmlInstance *ss, *so; } withstk[32]; int withsp=0;
  /* string GC: track malloc'd strings so they can be freed at scope exit */
  void **str_gc=NULL; int str_gc_n=0, str_gc_cap=0;
  #define GC_TRACK(ptr) do{ if(str_gc_n>=str_gc_cap){ str_gc_cap=str_gc_cap?str_gc_cap*2:16; str_gc=realloc(str_gc,str_gc_cap*sizeof(void*)); } str_gc[str_gc_n++]=(void *)(ptr); }while(0)
  /* an owned V_STR (v.d!=0, set by vstr_owned) is a fresh malloc'd temporary the VM must free.
   * Literals/references (data.win STRG, rodata, var pointers via plain vstr) are d==0 and never freed. */
  #define STR_IS_HEAP(vv) ((vv).t==V_STR && (vv).s && (vv).d!=0)
  #define GC_UNTRACK(ss) do{ const void *_p=(const void*)(ss); for(int _i=0;_i<str_gc_n;_i++) if(str_gc[_i]==_p) str_gc[_i]=NULL; }while(0)
  /* Persisting a V_STR into an instance/global variable transfers it if this run owns it.
   * An owned string outside this run's tracker belongs to a caller scope and dies when that caller
   * returns, so store a copy.
   * d==0 strings are stable refs (STRG/rodata) and need neither. */
  #define GC_PERSIST(vv) do{ if((vv).t==V_STR && (vv).s && (vv).d!=0){ int _f=0; const void *_p=(const void*)(vv).s; \
      for(int _i=0;_i<str_gc_n;_i++) if(str_gc[_i]==_p){ str_gc[_i]=NULL; _f=1; } \
      if(!_f){ char *_c=strdup((vv).s); if(_c) (vv)=vstr_owned(_c); } } }while(0)
  const char *trace_filter = vm_trace_filter(vm);
  int trace = trace_filter && strstr(w->code[ci].name, trace_filter);
  int use_cache = !trace && code_cache_ensure(w,ci);
  GmlInsn *cached_ins = use_cache ? w->code[ci].insn : NULL;
  uint32_t *cached_pc = use_cache ? w->code[ci].insn_pc : NULL;
  int32_t *cached_branch = use_cache ? w->code[ci].branch_index : NULL;
  uint32_t cached_n = use_cache ? w->code[ci].n_insn : 0;
  int cp = codeprof_on(vm);
  int hp_builtin = builtin_hotprof_on(vm);
  double cp_t0 = cp ? codeprof_now_ms(vm) : 0.0;
  /* Watchdog: a single code run should never execute more than a few million instructions. If one
   * blows past a large budget it is a runaway loop (e.g. a control-flow condition corrupted by an
   * unimplemented opcode) — abort the run instead of freezing the whole host. Real per-event
   * code, even heavy tile/particle loops, stays orders of magnitude under this. */
  uint64_t watchdog=0;
  const uint64_t WATCHDOG_MAX=64000000ull;
  uint32_t ip=0;
  while(use_cache ? ip<cached_n : pc<end){
    if(++watchdog>WATCHDOG_MAX){
      if(vm->diagnostics.watchdog_warnings<4){ vm->diagnostics.watchdog_warnings++;
        anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[gml] f%ld VM watchdog tripped in '%s' at offset %u (runaway loop) — aborting run\n",
          vm->frame, w->code[ci].name, pc-start); }
      break;
    }
    GmlInsn in; GmlInsn *pin=NULL; uint32_t nextpc; uint32_t nextip=ip+1;
    if(use_cache){
      pin=&cached_ins[ip];
      in=*pin;
      pc=cached_pc[ip];
      nextpc=pc+in.size;
    } else {
      int sz=gml_decode_bc_bounded(d,w->size,pc,w->bytecode,&in); if(!sz) break;
      nextpc=pc+(uint32_t)sz;
    }
    int prev_conv_v_i32=0;
    if(use_cache){
      if(ip>0){
        GmlInsn *prev=&cached_ins[ip-1];
        prev_conv_v_i32 = prev->kind==OP_CONV && prev->type1==DT_VAR && prev->type2==DT_INT32;
      }
    } else if(pc>=start+4){
      GmlInsn prev;
      if(gml_decode_bc_bounded(d,w->size,pc-4,w->bytecode,&prev)==4)
        prev_conv_v_i32 = prev.kind==OP_CONV && prev.type1==DT_VAR && prev.type2==DT_INT32;
    }
    /* Include the numeric top-of-stack value in the optional opcode trace. */
    GML_VM_DIAGNOSTIC_OPCODE(vm,w->code[ci].name,pc-start,
                             gml_op_mnemonic(in.kind),sp,
                             sp>0?asnum(stk[sp-1]):0.0);
    if(trace){
      const char *rn = (in.kind==OP_CALL || in.kind==OP_PUSH || in.kind==OP_POP) ? (in.refname?in.refname:gml_ref_name(w,in.refaddr)) : "";
      anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"  %4u: %-7s t1=%x rt=%02x inst=%d  sp=%d %s\n",pc-start,gml_op_mnemonic(in.kind),in.type1,in.reftype,in.inst,sp,rn); }
    {
      if(!vm->diagnostics.pc_initialized){
        const char *e=anygm_host_development_setting(vm->host,"GML_LOG_PC"); vm->diagnostics.pc_initialized=1;
        if(e && *e){
          const char *colon=strrchr(e,':'); size_t n=colon?(size_t)(colon-e):strlen(e);
          if(n>=sizeof vm->diagnostics.pc_name) n=sizeof vm->diagnostics.pc_name-1;
          memcpy(vm->diagnostics.pc_name,e,n); vm->diagnostics.pc_name[n]=0;
          vm->diagnostics.pc_offset=colon?strtol(colon+1,NULL,0):-1;
          const char *m=anygm_host_development_setting(vm->host,"GML_LOG_PC_MAX"); vm->diagnostics.pc_max_logs=m?atoi(m):200;
          if(vm->diagnostics.pc_max_logs<=0) vm->diagnostics.pc_max_logs=200;
        }
      }
      if(vm->diagnostics.pc_name[0] && vm->diagnostics.pc_log_count<vm->diagnostics.pc_max_logs &&
         strstr(w->code[ci].name,vm->diagnostics.pc_name) &&
         (vm->diagnostics.pc_offset<0 || (long)(pc-start)==vm->diagnostics.pc_offset)){
        
        GmlInstance *dbg_self=vm->cur_self, *dbg_other=vm->cur_other;
        const char *dbg_self_name=(dbg_self && dbg_self->obj>=0 && dbg_self->obj<vm->n_objects)
          ? vm->objects[dbg_self->obj].name : "?";
        const char *dbg_other_name=(dbg_other && dbg_other->obj>=0 && dbg_other->obj<vm->n_objects)
          ? vm->objects[dbg_other->obj].name : "?";
        anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[pc] f%ld %s+%u sp=%d self=%s/%u(hsp=%.3f,xs=%.1f) other=%s/%u(hsp=%.3f,xs=%.1f) top=",
          vm->frame,w->code[ci].name,pc-start,sp,
          dbg_self_name?dbg_self_name:"?",dbg_self?dbg_self->id:0,
          dbg_self?dbg_self->hspeed:0.0,dbg_self?dbg_self->image_xscale:0.0,
          dbg_other_name?dbg_other_name:"?",dbg_other?dbg_other->id:0,
          dbg_other?dbg_other->hspeed:0.0,dbg_other?dbg_other->image_xscale:0.0);
        if(sp>0) log_val_simple(vm,stk[sp-1]); else anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"<empty>");
        anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG," stack[");
        int stack_first=sp>8?sp-8:0;
        for(int dbg_i=stack_first;dbg_i<sp;dbg_i++){
          if(dbg_i>stack_first) anygm_host_logf(vm->host,ANYGM_LOG_DEBUG,",");
          log_val_simple(vm,stk[dbg_i]);
        }
        anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"]");
        anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"\n");
        vm->diagnostics.pc_log_count++;
      } }
    if(use_cache && !hp_builtin && sp==0 && !vm->diagnostics.pc_name[0] &&
       !GML_VM_DIAGNOSTIC_OPCODE_ENABLED(vm,w->code[ci].name) &&
       !vm_trace_call_filter(vm)){
      uint32_t loop_exit=0;
      uint64_t loop_instructions=0;
      if(vm_try_array_draw_loop(
           vm,&locals,cached_ins,cached_branch,cached_n,ip,
           watchdog,WATCHDOG_MAX,&loop_exit,&loop_instructions)){
        watchdog+=loop_instructions-1;
        ip=loop_exit;
        continue;
      }
    }
    switch(in.kind){
      case OP_PUSH:{
        GmlVal v;
        if(in.type1==DT_INT16) v=vreal(in.sval);
        else if(in.type1==DT_DOUBLE) v=vreal(in.dval);
        else if(in.type1==DT_INT32){
          const char *hash_name=use_cache ? in.refname :
            (gml_ref_kind(w,pc+4)==GML_REF_VARIABLE?gml_ref_name(w,pc+4):NULL);
          v=vreal(hash_name ? (double)(use_cache?in.refhash:gml_value_name_hash(hash_name))
                            : (double)in.ival);
          /* GMS2.3 function-value: a `push.i32` whose reference word (pc+4) resolves via the FUNC
           * occurrence chain to a script code-entry is pushing that function as a value (later called
           * by OP_CALLV or bound by method()). Tag it so OP_CALLV can dispatch the code entry. */
          if(!hash_name && anygm_policy_has_modern_function_values(w)){
            int fci=use_cache ? in.funcval_ci : -1;
            const char *fn=NULL;
            if(fci==-1){   /* -2 = decode already determined it is not a function-value */
              fn=gml_ref_name(w,pc+4);
              if(fn && fn[0]!='?' && !strncmp(fn,"gml_",4)) fci=gml_code_index_by_name(w,fn);
            }
            if(fci>=0){ v=vreal((double)(GML_FUNCVAL_TAG|fci));
              if(vm->diagnostics.function_value_debug<0)
                vm->diagnostics.function_value_debug=anygm_host_development_setting(vm->host,"GML_DBG_FUNCVAL")!=NULL;
              if(vm->diagnostics.function_value_debug) anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[funcval] %s pc=%u i32=%d -> %s (ci=%d)\n",
                w->code[ci].name,pc-start,in.ival,fn?fn:(w->code[fci].name?w->code[fci].name:"?"),fci); } } }
        else if(in.type1==DT_INT64) v=vreal((double)in.lval);
        else if(in.type1==DT_STRING) v=vstr(gml_str_by_index(w,in.strindex));
        else if(in.type1==DT_VAR){
          const char *nm=in.refname?in.refname:gml_ref_name(w,in.refaddr);
          uint32_t nh=in.refhash?in.refhash:gml_value_name_hash(nm);
          if(in.reftype==0x00){ /* Array */
            int idx=(int)(sp>0?asnum(stk[--sp]):0);
            GmlVal itv=sp>0?stk[--sp]:vreal(0);
            if(anygm_policy_has_modern_function_values(w) && sp>0 && itv.t==V_REAL && itv.d==-9.0){
              GmlVal iv=stk[--sp];
              GmlInstance *t=vm_inst_from_ref(vm,iv);
              v=t?array_get_inst_field_h(vm,t,nm,nh,idx):
                  array_get_h(vm,&locals,(int)asnum(iv),nm,nh,idx);
            } else {
              int it=(int)asnum(itv);
              v=array_get_h(vm,&locals,it,nm,nh,idx);
            }
          } else if(in.reftype==0x10 || in.reftype==0x90){
            /* GMS2.3 array-following push (first dimension from a named variable). Stack top->down:
             * index, instance-type. ArrayPushAF(0x10)=read; ArrayPopAF(0x90)=write chain (must yield a
             * live sub-array reference so a following popaf stores into it). An expression receiver
             * (`global.a[i][j]`, `obj.a[i][j]`) is encoded below a StackTop -9 marker, just like a
             * normal StackTop field access. Consume that marker and resolve the actual receiver;
             * otherwise a global nested store is silently redirected into self.a. */
            int idx=(int)(sp>0?asnum(stk[--sp]):0);
            GmlVal iv=sp>0?stk[--sp]:vreal(IT_SELF);
            if(anygm_policy_has_modern_function_values(w) && sp>0 && iv.t==V_REAL && iv.d==-9.0) iv=stk[--sp];
            GmlVarMap *m=NULL;
            int it=(int)asnum(iv);
            if(is_room_global_array(nm)) m=&vm->globals;
            else if(iv.t==V_REAL && !GML_IS_STRUCT_ID(iv.d) && it<100000)
              m=scope_map(vm,&locals,it);
            else {
              GmlInstance *t=vm_inst_from_ref(vm,iv);
              if(t) m=&t->vars;
            }
            v=vreal(0);
            if(m && idx>=0){
              if(in.reftype==0x90){ GmlVal *slot=gml_varmap_put_hashed(m,nm,nh); GmlArr *A=gml_arr_slot_ensure(slot);
                if(m!=&locals) A->escaped=1;
                gml_arr_index_ensure(A,idx);
                if(idx<A->cap){ if(A->data[idx].t!=V_ARR){ A->data[idx].t=V_ARR; A->data[idx].arr=calloc(1,sizeof(GmlArr));
                    if(A->escaped && A->data[idx].arr) ((GmlArr*)A->data[idx].arr)->escaped=1; }
                  v=A->data[idx]; } }
              else { GmlVal *slot=gml_varmap_get_hashed(m,nm,nh);
                if(slot && slot->t==V_ARR){ GmlArr *A=slot->arr; if(idx<A->len) v=A->data[idx]; } }
            }
          } else if(in.reftype==0x80){ /* StackTop: instance.var */
            /* GMS2.3 emits `push.e -9` (InstanceType.StackTop sentinel) between the instance
             * expression and the variable access: stack is [instance, -9] with the marker on top.
             * -9 is never a valid instance reference (real ids are >=100000, the special scopes are
             * -1/-2/-5), so a -9 on top of a StackTop access is ALWAYS that marker — consume it and
             * take the instance below. Keying off the stack value (not the preceding opcode) is what
             * makes the compound `inst.var op= v` form work: there `dup` sits between the marker and
             * the read, so the old "previous instruction was push.e -9" heuristic failed and the read
             * resolved instance -9 = NULL = 0, freezing e.g. `inst.x += 14` at a constant 14. */
            GmlVal iv=sp>0?stk[--sp]:vreal(0);
            if(anygm_policy_has_modern_function_values(w) && sp>0 && iv.t==V_REAL && iv.d==-9.0) iv=stk[--sp];
            int it=(int)asnum(iv);
            if(iv.t==V_REAL && !GML_IS_STRUCT_ID(iv.d) && it<100000)
              v=var_get_h(vm,it,nm,nh);
            else {
              GmlInstance *t=vm_inst_from_ref(vm,iv);
              v=t?inst_get_any_h(vm,t,nm,nh):vreal(0);
            }
          } else if(in.inst==IT_STACK){
            /* GMS2.3 direct StackTop read `push.v stack.var`: the instance is the top of the value
             * stack (no separate -9 marker; the -9 is the instruction's own instance-type). Used for
             * `expr.field` where expr is a temporary — notably struct method dispatch `b.method(...)`,
             * where mis-routing this to var_get(-9) read nobody and every struct method call got 0. */
            GmlVal iv=sp>0?stk[--sp]:vreal(0);
            GmlInstance *t=vm_inst_from_ref(vm,iv);
            if(t) v=inst_get_any_h(vm,t,nm,nh);
            /* A direct StackTop owner can be a numeric scope sentinel. Resolve it through the
             * scope reader only when no instance was found. Struct identifiers retain the
             * instance path and IT_STACK remains a marker rather than an owner scope. */
            else if(iv.t==V_REAL && !GML_IS_STRUCT_ID(iv.d) &&
                    gml_it_is_scope_to_read((int)iv.d))
              v=var_get_h(vm,(int)iv.d,nm,nh);
            else v=vreal(0);
          } else if(in.inst==0 && in.reftype==0xA0 && prev_conv_v_i32){
            GmlVal iv=sp>0?stk[--sp]:vreal(0);
            int it=(int)asnum(iv);
            if(iv.t==V_REAL && !GML_IS_STRUCT_ID(iv.d) && it<100000)
              v=var_get_h(vm,it,nm,nh);
            else {
              GmlInstance *t=vm_inst_from_ref(vm,iv);
              v=t?inst_get_any_h(vm,t,nm,nh):vreal(0);
            }
          } else if(in.inst==IT_LOCAL){
            if(argument_get(vm,nm,&v)){}
            else { GmlVal *pp=gml_varmap_get_hashed(&locals,nm,nh);
              v=pp?*pp:vreal(0); }
          } else v=var_get_h(vm,in.inst,nm,nh);
        } else v=vreal(0);
        if(sp<STK){ stk[sp]=v; stkt[sp]=in.type1; sp++; }
        break;
      }
      case OP_POP:{
        const char *nm=in.refname?in.refname:gml_ref_name(w,in.refaddr);
        uint32_t nh=in.refhash?in.refhash:gml_value_name_hash(nm);
        if(in.reftype==0x00){ /* Array store. Same Type1 quirk as StackTop: a direct `arr[i] = v`
           * (pop.v.v) pushes [value, insttype, index] (index on top), but a GMS2.3 compound
           * `arr[i] += v` (pop.<num>.v after dup(1)) leaves [insttype, index, value] with the
           * value on top. Reading it with the direct order writes an invalid value and index. */
          int idx; GmlVal itv, val, iv=vreal(0), scopev; GmlInstance *t=NULL;
          if(in.type1==DT_VAR){
            idx=(int)(sp>0?asnum(stk[--sp]):0);
            itv=sp>0?stk[--sp]:vreal(0);
            scopev=itv;
            if(anygm_policy_has_modern_function_values(w) && sp>0 && itv.t==V_REAL && itv.d==-9.0){
              iv=stk[--sp];
              scopev=iv;
              t=vm_inst_from_ref(vm,iv);
            }
            val=sp>0?stk[--sp]:vreal(0);
          } else {
            val=sp>0?stk[--sp]:vreal(0);
            idx=(int)(sp>0?asnum(stk[--sp]):0);
            itv=sp>0?stk[--sp]:vreal(0);
            scopev=itv;
            if(anygm_policy_has_modern_function_values(w) && sp>0 && itv.t==V_REAL && itv.d==-9.0){
              iv=stk[--sp];
              scopev=iv;
              t=vm_inst_from_ref(vm,iv);
            }
          }
          GC_PERSIST(val);
          if(classic_implicit_return) ret=val;
          if(t) array_set_inst_field_h(vm,t,nm,nh,idx,val);
          else array_set_h(vm,&locals,(int)asnum(scopev),nm,nh,idx,val);
        } else if(in.reftype==0x80){ /* StackTop instance.var. GMS quirk: the value/instance push
           * order depends on the value's Type1 — `pop.v.*` (Type1=Variable) pushes the value FIRST
           * then [instance, -9] (marker on top); every other `pop.<num>.v` (the compound `inst.var
           * op= v` form, emitted after a dup of the [instance,-9] ref) pushes [instance, -9] then the
           * value, so the value is on top and the -9 marker sits UNDER it. -9 is never a valid
           * instance reference, so wherever it lands it is the StackTop marker and we drop it. Keying
           * off the actual stack (not the preceding opcode) is what makes the compound form store
           * back into the instance instead of writing to instance -9 = nobody (which left the read's
           * result — e.g. `inst.x += 14` — computed but never committed). */
           GmlVal val, iv;
           if(in.type1==DT_VAR){                        /* [value, instance, (-9)] — marker on top */
             if(anygm_policy_has_modern_function_values(w) && sp>0 && stk[sp-1].t==V_REAL && stk[sp-1].d==-9.0) --sp;
             iv=sp>0?stk[--sp]:vreal(0); val=sp>0?stk[--sp]:vreal(0);
           } else {                                     /* [instance, (-9), value] — value on top */
             val=sp>0?stk[--sp]:vreal(0);
             if(anygm_policy_has_modern_function_values(w) && sp>0 && stk[sp-1].t==V_REAL && stk[sp-1].d==-9.0) --sp;
             iv=sp>0?stk[--sp]:vreal(0);
           }
           GC_PERSIST(val);
           if(classic_implicit_return) ret=val;
           int it=(int)asnum(iv);
           if(iv.t==V_REAL && !GML_IS_STRUCT_ID(iv.d) && it<100000)
             var_set_h(vm,it,nm,nh,val);
           else {
             GmlInstance *t=vm_inst_from_ref(vm,iv); if(t) inst_set_any_h(vm,t,nm,nh,val);
           }
        } else {
          if(in.inst==0 && in.reftype==0xA0 && prev_conv_v_i32){
            GmlVal iv=sp>0?stk[--sp]:vreal(0);
            GmlVal v=sp>0?stk[--sp]:vreal(0);
            GC_PERSIST(v);
            if(classic_implicit_return) ret=v;
            int it=(int)asnum(iv);
            if(iv.t==V_REAL && !GML_IS_STRUCT_ID(iv.d) && it<100000)
              var_set_h(vm,it,nm,nh,v);
            else {
              GmlInstance *t=vm_inst_from_ref(vm,iv); if(t) inst_set_any_h(vm,t,nm,nh,v);
            }
            break;
          }
          GmlVal v = sp>0? stk[--sp] : vreal(0);
          /* locals/arguments die at scope exit, so their owned strings stay tracked and are freed
           * then (str_gc). Only instance/global stores persist beyond the run — untrack those so the
           * var owns the string (else it dangles at scope exit; re-assigning it later leaks it). */
          if(in.inst==IT_LOCAL){ if(!argument_set(vm,nm,v)) *gml_varmap_put_hashed(&locals,nm,nh)=v; }
          else if(in.inst==IT_STACK){   /* `pop.v.v stack.var` — write field on the instance under the value */
            GmlVal iv=sp>0?stk[--sp]:vreal(0); GC_PERSIST(v);
            GmlInstance *t=vm_inst_from_ref(vm,iv); if(t) inst_set_any_h(vm,t,nm,nh,v); }
          else { GC_PERSIST(v); var_set_h(vm,in.inst,nm,nh,v); }
          if(classic_implicit_return) ret=v;
        }
        break;
      }
      case OP_POPZ:
        if(sp>0){
          if(classic_implicit_return) ret=stk[sp-1];
          sp--;
        }
        break;
      case OP_DUP: {
        /* The low byte is a count in units of Type1, not a count of logical values. The next byte
         * optionally describes an adjacent lower block to swap with the top block. Values retain
         * their own encoded widths in stkt[], so mixed-width references are handled without
         * recognising any particular source expression. */
        uint16_t raw=(uint16_t)in.inst;
        int top_units=raw&0xFFu;
        int bottom_units=((raw>>8)&0x7Fu)>>3;
        int unit_size=vm_stack_type_size(in.type1);
        if(in.type1==DT_INT16){
          /* Newer bytecode reverses the two block counts and measures them in variable slots. */
          int swap=top_units; top_units=bottom_units; bottom_units=swap;
          unit_size=vm_stack_type_size(DT_VAR);
        }
        if(bottom_units>0){
          /* A full-width receiver needs no extra copy in the zero-argument
           * member-call shuffle. A narrower receiver retains one logical
           * value before the following VAR-width duplication. */
          if(in.type1==DT_VAR && top_units==0){
            if(sp>0 && vm_stack_type_size(stkt[sp-1])<unit_size && sp<STK){
              stk[sp]=stk[sp-1]; stkt[sp]=stkt[sp-1]; sp++;
            }
            break;
          }
          int top_bytes=top_units*unit_size, bottom_bytes=bottom_units*unit_size;
          int mid=sp, first=sp, bytes=0;
          while(mid>0 && bytes<top_bytes) bytes+=vm_stack_type_size(stkt[--mid]);
          if(bytes!=top_bytes) break;
          bytes=0; first=mid;
          while(first>0 && bytes<bottom_bytes) bytes+=vm_stack_type_size(stkt[--first]);
          if(bytes!=bottom_bytes) break;
          vm_stack_reverse(stk,stkt,first,mid);
          vm_stack_reverse(stk,stkt,mid,sp);
          vm_stack_reverse(stk,stkt,first,sp);
          break;
        }
        int wanted=(top_units+1)*unit_size;
        int first=sp, bytes=0;
        while(first>0 && bytes<wanted) bytes+=vm_stack_type_size(stkt[--first]);
        if(bytes==wanted){
          int count=sp-first;
          if(sp+count<=STK){
            memcpy(stk+sp,stk+first,(size_t)count*sizeof(*stk));
            memcpy(stkt+sp,stkt+first,(size_t)count*sizeof(*stkt));
            sp+=count;
          }
        }
        break; }
      case OP_CONV: /* values are dynamically typed; retain the encoded stack width. */
        if(sp>0) stkt[sp-1]=in.type2;
        break;
      case OP_NEG: if(sp>0){ stk[sp-1]=(stk[sp-1].t==V_UNDEF)?vundef():vreal(-asnum(stk[sp-1])); stkt[sp-1]=in.type1; } break;
      case OP_NOT: if(sp>0){ stk[sp-1]=vreal(!astrue(stk[sp-1])); stkt[sp-1]=in.type1==DT_BOOL?DT_BOOL:in.type1; } break;
      case OP_MUL: case OP_DIV: case OP_REM: case OP_MOD: case OP_ADD: case OP_SUB:
      case OP_AND: case OP_OR: case OP_XOR: case OP_SHL: case OP_SHR:{
        if(sp<2) break;
        GmlVal r=stk[--sp], l=stk[--sp];
        uint8_t result_type=vm_math_result_type(in.type2,in.type1);
        if(in.kind==OP_ADD && l.t==V_STR && r.t==V_STR){
          int la=strlen(l.s), lb=strlen(r.s); char *c=malloc(la+lb+1);
          memcpy(c,l.s,la); memcpy(c+la,r.s,lb+1); stk[sp]=vstr_owned(c); stkt[sp++]=result_type; GC_TRACK(c); break;
        }
        if(l.t==V_UNDEF || r.t==V_UNDEF){ stk[sp]=vundef(); stkt[sp++]=result_type; break; }
        double a=asnum(l), b=asnum(r), o=0;
        switch(in.kind){
          case OP_MUL:o=a*b;break; case OP_DIV:o=b!=0?a/b:0;break;
          case OP_REM:o=b!=0?trunc(a/b):0;break;        /* GM 'div' */
          case OP_MOD:o=b!=0?fmod(a,b):0;break;
          case OP_ADD:o=a+b;break; case OP_SUB:o=a-b;break;
          case OP_AND:o=(double)((long)a&(long)b);break; case OP_OR:o=(double)((long)a|(long)b);break;
          case OP_XOR:o=(double)((long)a^(long)b);break;
          case OP_SHL:o=(double)((long)a<<(long)b);break; case OP_SHR:o=(double)((long)a>>(long)b);break;
        }
        stk[sp]=vreal(o); stkt[sp++]=result_type; break;
      }
      case OP_CMP:{
        if(sp<2) break;
        GmlVal r=stk[--sp], l=stk[--sp]; int res=0;
        if(l.t==V_UNDEF || r.t==V_UNDEF){
          int both=(l.t==V_UNDEF && r.t==V_UNDEF);
          switch(in.cmp){
            case CMP_EQ: res=both; break;
            case CMP_NEQ: res=!both; break;
            default: res=0; break;
          }
        } else if(l.t==V_STR || r.t==V_STR){ char lb[64], rb[64];
          int c=strcmp(asstr_cmp(l,lb,sizeof lb),asstr_cmp(r,rb,sizeof rb));
          switch(in.cmp){case CMP_LT:res=c<0;break;case CMP_LTE:res=c<=0;break;case CMP_EQ:res=c==0;break;
            case CMP_NEQ:res=c!=0;break;case CMP_GTE:res=c>=0;break;case CMP_GT:res=c>0;break;} }
        else {
          /* Apply the selected compatibility comparison policy to real expressions. This matters
           * for ordered comparisons too: repeated decimal steps can otherwise cross zero through a
           * tiny floating-point residue instead of settling at the epsilon bound. */
          double epsilon=anygm_policy_exact_comparisons(w)?0.0:vm->math_epsilon;
          res=gml_real_compare_epsilon(asnum(l),asnum(r),in.cmp,epsilon);
        }
        stk[sp]=vreal(res); stkt[sp++]=DT_BOOL; break;
      }
      case OP_B:
        nextpc = pc + (uint32_t)(in.jump*4);
        if(use_cache) nextip=(uint32_t)cached_branch[ip];
        break;
      case OP_BT: {
        GmlVal v=sp>0?stk[--sp]:vreal(0);
        if(astrue(v)){ nextpc=pc+(uint32_t)(in.jump*4); if(use_cache) nextip=(uint32_t)cached_branch[ip]; }
        break; }
      case OP_BF: {
        GmlVal v=sp>0?stk[--sp]:vreal(0);
        if(!astrue(v)){ nextpc=pc+(uint32_t)(in.jump*4); if(use_cache) nextip=(uint32_t)cached_branch[ip]; }
        break; }
      case OP_CALL:{
        const char *nm=in.refname?in.refname:gml_ref_name(w,in.refaddr); int na=in.argc;
        {
          const char *match=vm_trace_call_filter(vm);
          if(match && (!*match || (nm && strstr(nm,match)))){
            
            anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[call] f%ld code=%s pc=%u name=%s argc=%d\n",
              vm->frame,w->code[ci].name?w->code[ci].name:"?",pc,
              nm?nm:"?",na);
          }
        }
        GmlVal a[64]; if(na>64) na=64;
        /* GM pushes args in reverse, so arg0 is on top: pop forward -> a[0]=arg0 */
        for(int i=0;i<na;i++) a[i] = sp>0? stk[--sp] : vreal(0);
        int sci = pin ? pin->funcval_ci : -1;
        if(sci<0){
          sci=classic_extension_script_code(vm,nm);
          if(pin && sci>=0) pin->funcval_ci=sci;
        }
        int bid = pin ? pin->builtin_id : -1;
        if(pin && bid==0){
          bid=gml_builtin_fast_id(vm,nm);
          pin->builtin_id=(int16_t)bid;
        }
        /* per-call-site script cache: once the generic dispatch resolves this name to a user
         * script, remember its code index (in funcval_ci, unused on OP_CALL) and run it directly —
         * otherwise repeated script calls walk the whole builtin name chain before
         * reaching the script fallback. */
        GmlVal rv;
        if(sci>=0 && !hp_builtin){
          if(!code_micro_maybe(vm,sci,a,na,&rv)) rv=gml_vm_run_code(vm,sci,vm->cur_self,vm->cur_other,a,na);
        }
        else if(bid>0 && !hp_builtin) rv=gml_builtin_call_fast_id(vm,bid,nm,a,na);
        else {
          vm->call_script_ci=-1;
          rv=gml_builtin_call(vm,nm,a,na);
          if(pin && vm->call_script_ci>=0) pin->funcval_ci=vm->call_script_ci;
          vm->call_script_ci=-1;
        }
        /* track a freshly-malloc'd string result so it's freed (else string builtins leak). Skip
         * arg pass-through (the arg's owner frees it) to avoid double-tracking a var's string. */
        if(STR_IS_HEAP(rv)){ int isarg=0; for(int _k=0;_k<na;_k++) if(a[_k].t==V_STR && a[_k].s==rv.s){isarg=1;break;} if(!isarg) GC_TRACK(rv.s); }
        if(sp<STK){ stk[sp]=rv; stkt[sp]=DT_VAR; sp++; }
        break;
      }
      case OP_CALLV:{
        int callv_sp_in=sp; (void)callv_sp_in;   /* optional diagnostic input; unused when disabled */
        /* GMS2.3 call-a-value: a function VALUE sits under the args. GM stack order is
         * func, argN..arg1, arg0 (arg0 on top). Pop args (arg0 first) then the function value.
         * A tagged function-value (from a push.i32 fref or method()) carries the code index. */
        int na=in.argc; GmlVal a[64]; if(na>64) na=64;
        int fci=-1; GmlInstance *call_self=vm->cur_self;
        /* A field call `receiver.callback(args)` is emitted as a StackTop field push immediately
         * before callv. Its final stack shape is [args..., receiver, callback], with callback on
         * top. A plain function value stored in that field is still invoked with receiver as self;
         * only recognising explicitly-bound method structs lost that receiver and ran callbacks
         * against the surrounding event instance. */
        int member_value_call=0;
        if(use_cache && ip>0){
          GmlInsn *prev=&cached_ins[ip-1];
          member_value_call=prev->kind==OP_PUSH && prev->type1==DT_VAR &&
                            (prev->inst==IT_STACK || prev->reftype==0x80);
          if(!member_value_call && ip>1 && prev->kind==OP_PUSH && prev->type1==DT_VAR){
            GmlInsn *scope=&cached_ins[ip-2];
            const char *sn=scope->refname?scope->refname:gml_ref_name(w,scope->refaddr);
            member_value_call=scope->kind==OP_CALL && sn &&
                              (!strcmp(sn,"@@This@@") || !strcmp(sn,"@@Other@@"));
          }
          if(!member_value_call && ip>1 && prev->kind==OP_BREAK && prev->sval==-11){
            /* `self.method(args)` where the callee is a pushref rather than a field read:
             * call @@This@@(0); break -11 ref=...; callv. The reference is pushed last, so the
             * function value is on TOP and the args are under it — the same shape as a field call
             * and the opposite of the plain funcval convention the fallback assumes. Read as a
             * plain call it pops the reference as an argument, finds an instance id where the
             * function should be, and dispatches nothing at all, silently. */
            GmlInsn *scope=&cached_ins[ip-2];
            const char *sn=scope->refname?scope->refname:gml_ref_name(w,scope->refaddr);
            member_value_call=scope->kind==OP_CALL && sn &&
                              (!strcmp(sn,"@@This@@") || !strcmp(sn,"@@Other@@"));
          }
        } else if(!use_cache && pc>=start+8){
          GmlInsn prev;
          int psz=gml_decode_bc_bounded(d,w->size,pc-8,w->bytecode,&prev);
          member_value_call=psz==8 && prev.kind==OP_PUSH && prev.type1==DT_VAR &&
                            (prev.inst==IT_STACK || prev.reftype==0x80);
          if(!member_value_call && psz==8 && prev.kind==OP_PUSH && prev.type1==DT_VAR &&
             pc>=start+16){
            GmlInsn scope;
            int ssz=gml_decode_bc_bounded(d,w->size,pc-16,w->bytecode,&scope);
            const char *sn=ssz==8?(scope.refname?scope.refname:gml_ref_name(w,scope.refaddr)):NULL;
            member_value_call=ssz==8 && scope.kind==OP_CALL && sn &&
                              (!strcmp(sn,"@@This@@") || !strcmp(sn,"@@Other@@"));
          }
          if(!member_value_call && psz==8 && prev.kind==OP_BREAK && prev.sval==-11 &&
             pc>=start+16){
            GmlInsn scope;
            int ssz=gml_decode_bc_bounded(d,w->size,pc-16,w->bytecode,&scope);
            const char *sn=ssz==8?(scope.refname?scope.refname:gml_ref_name(w,scope.refaddr)):NULL;
            member_value_call=ssz==8 && scope.kind==OP_CALL && sn &&
                              (!strcmp(sn,"@@This@@") || !strcmp(sn,"@@Other@@"));
          }
        }
        /* A METHOD call `obj.method(args)` leaves [args.., self, method] — bound method on TOP, accessor
         * instance right under it (via the dup-swap). A plain funcval call leaves the func at the BOTTOM
         * (arg0 on top). Peek: bound-method struct on top => method convention. */
        GmlInstance *bm_top=NULL;
        if(sp>0){ double tv=asnum(stk[sp-1]);
          if(GML_IS_STRUCT_ID(tv)){ GmlInstance *b=gml_struct_find(vm,(unsigned)tv);
            if(method_struct_info(b,NULL,NULL,NULL)) bm_top=b; } }
        if(bm_top){
          sp--;   /* the method value */
          GmlVal selfv; int have_self=0;
          method_struct_info(bm_top,&fci,&selfv,&have_self);
          GmlInstance *bs = have_self? vm_inst_from_ref(vm,selfv) : NULL;
          if(member_value_call && sp>0){
            GmlVal receiver=stk[--sp];   /* drop the accessor receiver (obj. in obj.method) */
            /* A constructor-static method is shared, but dot invocation still executes against
             * the struct through which it was accessed. The -16 binding marks that late receiver. */
            if(have_self && selfv.t==V_REAL && selfv.d==-16.0){
              GmlInstance *rs=vm_inst_from_ref(vm,receiver);
              if(rs) bs=rs;
            }
          }
          else if(have_self && sp>0 && asnum(stk[sp-1])==asnum(selfv)) sp--;
          if(bs) call_self=bs;
          for(int i=0;i<na;i++) a[i]= sp>0? stk[--sp] : vreal(0);
        } else if(member_value_call && sp>=2){
          GmlVal fv=stk[--sp];
          GmlVal receiver=stk[--sp];
          double fvn=asnum(fv);
          if(GML_IS_FUNCVAL((int)fvn)) fci=(int)fvn & 0x00FFFFFF;
          else if(GML_IS_STRUCT_ID(fvn)){
            GmlInstance *bm=gml_struct_find(vm,(unsigned)fvn);
            if(bm){
              GmlVal selfv; int have_self=0;
              method_struct_info(bm,&fci,&selfv,&have_self);
              if(have_self){ GmlInstance *bs=vm_inst_from_ref(vm,selfv); if(bs) call_self=bs; }
            }
          }
          if(fci>=0){
            GmlInstance *rs=vm_inst_from_ref(vm,receiver);
            if(rs) call_self=rs;
          }
          for(int i=0;i<na;i++) a[i]=sp>0?stk[--sp]:vreal(0);
        } else {
          for(int i=0;i<na;i++) a[i]= sp>0? stk[--sp] : vreal(0);   /* a[0]=arg0 (top) */
          for(int i=na;i<in.argc;i++) if(sp>0) sp--;                /* drop overflow args */
          GmlVal fv = sp>0? stk[--sp] : vreal(0);
          double fvn=asnum(fv);
          if(GML_IS_STRUCT_ID(fvn)){   /* bound method value called directly */
            GmlInstance *bm=gml_struct_find(vm,(unsigned)fvn);
            if(bm){ GmlVal selfv; int have_self=0;
              method_struct_info(bm,&fci,&selfv,&have_self);
              if(have_self){ GmlInstance *bs=vm_inst_from_ref(vm,selfv); if(bs) call_self=bs; } } }
          else if(GML_IS_FUNCVAL((int)fvn)) fci = (int)fvn & 0x00FFFFFF;
        }
        GmlVal rv=vreal(0);
        if(fci>=0 && fci<w->n_code){
          GmlInstance *old_self=vm->cur_self;
          vm->cur_self=call_self;
          int micro_ok=code_micro_maybe(vm,fci,a,na,&rv);
          vm->cur_self=old_self;
          if(!micro_ok) rv=gml_vm_run_code(vm,fci,call_self,vm->cur_other,a,na);
        }
        /* track a heap-string result so it's freed at scope exit (mirror OP_CALL); args stay owned
         * by this frame's str_gc and are freed there, so don't touch them. */
        if(STR_IS_HEAP(rv)){ int isarg=0; for(int _k=0;_k<na;_k++) if(a[_k].t==V_STR && a[_k].s==rv.s){isarg=1;break;} if(!isarg) GC_TRACK(rv.s); }
        if(sp<STK){ stk[sp]=rv; stkt[sp]=DT_VAR; sp++; }
        GML_VM_DIAGNOSTIC_CALLV_STACK(vm,na,callv_sp_in,sp);
        if(fci<0 && anygm_host_development_setting(vm->host,"GML_LOG_CALLV_MISS")){
          /* Report unresolved dynamic calls when the optional setting is present. */
          double top_=callv_sp_in>0?asnum(stk[callv_sp_in-1]):0.0;
          double under_=callv_sp_in>1?asnum(stk[callv_sp_in-2]):0.0;
          /* Include the offset within a code entry so an unresolved call can be localized even
           * when several call sites share the same entry name. */
          anygm_host_logf(vm->host,ANYGM_LOG_DEBUG,
            "[callv-miss] argc=%d top=%.17g under=%.17g struct_top=%d funcval_top=%d in %s+%u\n",na,
            top_,under_,GML_IS_STRUCT_ID(top_)?1:0,GML_IS_FUNCVAL((int)top_)?1:0,
            (vm->win && vm->cur_code_index>=0 && vm->cur_code_index<vm->win->n_code &&
             vm->win->code[vm->cur_code_index].name)?vm->win->code[vm->cur_code_index].name:"",
            (unsigned)(pc-start));
        }
        break;
      }
      case OP_RET: ret = sp>0? stk[--sp]:vreal(0); if(use_cache) ip=cached_n; else pc=end; continue;
      case OP_EXIT:
        if(!classic_implicit_return) ret=vreal(0);
        if(use_cache) ip=cached_n; else pc=end;
        continue;
      case OP_PUSHENV:{ /* with(target): pop the target, iterate matching instances */
        GmlVal tv = sp>0? stk[--sp] : vreal(0);
        /* GMS2.3 `with(<expr>)` pushes the -9 StackTop sentinel above the target (same convention
         * as its variable accesses). Consuming only one value made with(instance_create(...)) run on
         * target -9 = nothing AND leak the id on the stack — burying enclosing repeat/for counters
         * and can bury enclosing repeat/for counters. */
        if(anygm_policy_has_modern_function_values(w) && sp>0 && tv.t==V_REAL && tv.d==-9.0 &&
           pc>=start+4 && gml_vm_read_u32_le(d,pc-4)==0x840FFFF7u) tv=stk[--sp];
        int T=(int)asnum(tv);
        GmlInstance **list=NULL; int nn=0, capL=0;
        int family_target=0;
        #define WADD(p) do{ if(nn>=capL){ capL=capL?capL*2:8; list=realloc(list,capL*sizeof(void*)); } list[nn++]=(p); }while(0)
        if(GML_IS_STRUCT_ID((double)T)){ GmlInstance *p=gml_struct_find(vm,(unsigned)T); if(p) WADD(p); }  /* with(struct): GMS2.3 runs the body with self=the struct (e.g. serialize's `with(action){..self.value..}`) */
        else if(T>=100000){ GmlInstance *p=inst_by_id(vm,T); if(p) WADD(p); }
        else if(T==IT_OTHER){ if(vm->cur_other) WADD(vm->cur_other); }
        else if(T==IT_SELF){ if(vm->cur_self) WADD(vm->cur_self); }
        else if(T==IT_ALL){ family_target=1; for(int i=0;i<vm->inst_count;i++) if(vm->inst[i].active&&!vm->inst[i].marked) WADD(&vm->inst[i]); }
        else if(T>=0){ family_target=1; for(int i=0;i<vm->inst_count;i++) if(vm->inst[i].active&&!vm->inst[i].marked&&gml_object_is(vm,vm->inst[i].obj,T)) WADD(&vm->inst[i]); }
        #undef WADD
        /* The modern instance-family chain is newest-first. This is observable when a parent target
         * includes both room-placed descendants and instances created by room code: the dynamic
         * instances must run before the older placed ones. Keep classic's established order, and
         * use creation sequence rather than slot order so recycled slots do not perturb a with(). */
        if(family_target && nn>1 && !anygm_policy_uses_classic_runtime(w))
          qsort(list,(size_t)nn,sizeof(*list),with_newest_first_cmp);
        if(nn==0){
          /* PUSHENV branches to its matching POPENV when the target set is empty. Keep an
           * empty frame on the environment stack so that POPENV consumes this scope instead
           * of accidentally closing an enclosing with-block. */
          int pushed_empty=withsp<32;
          if(pushed_empty){
            withstk[withsp].list=list; withstk[withsp].n=0; withstk[withsp].idx=0;
            withstk[withsp].ss=vm->cur_self; withstk[withsp].so=vm->cur_other; withsp++;
          } else free(list);
          nextpc = pc + (uint32_t)(in.jump*4);
          if(use_cache){
            int target=cached_branch[ip];
            nextip=(uint32_t)(pushed_empty?target:(target>=0?target+1:target));
          } else if(!pushed_empty){
            GmlInsn endenv;
            int size=gml_decode_bc_bounded(d,w->size,nextpc,w->bytecode,&endenv);
            if(size>0 && endenv.kind==OP_POPENV) nextpc+=(uint32_t)size;
          }
        }
        else if(withsp>=32){
          free(list);
          nextpc = pc + (uint32_t)(in.jump*4);
          if(use_cache){ int target=cached_branch[ip]; nextip=(uint32_t)(target>=0?target+1:target); }
          else { GmlInsn endenv; int size=gml_decode_bc_bounded(d,w->size,nextpc,w->bytecode,&endenv);
            if(size>0 && endenv.kind==OP_POPENV) nextpc+=(uint32_t)size; }
        }
        else { withstk[withsp].list=list; withstk[withsp].n=nn; withstk[withsp].idx=0;
          withstk[withsp].ss=vm->cur_self; withstk[withsp].so=vm->cur_other; withsp++;
          vm->cur_other=vm->cur_self; vm->cur_self=list[0]; }
        break; }
      case OP_POPENV:{ /* end of with body: next instance, or restore + fall through */
        if(withsp<=0) break;
        int wi=withsp-1;
        int next=withstk[wi].idx+1;
        /* The target set is snapshotted, but an earlier body may destroy a later member. Skip
         * that member before iteration reaches its retained diagnostic slot. */
        while(next<withstk[wi].n &&
              (!withstk[wi].list[next]->active || withstk[wi].list[next]->marked)) next++;
        if(next < withstk[wi].n){ withstk[wi].idx=next; vm->cur_self=withstk[wi].list[next];
          nextpc = pc + (uint32_t)(in.jump*4); if(use_cache) nextip=(uint32_t)cached_branch[ip]; }
        else { vm->cur_self=withstk[wi].ss; vm->cur_other=withstk[wi].so; free(withstk[wi].list); withsp--; }
        break; }
      case OP_BREAK:
        /* Newer extended ("break") operations keep the value stack balanced. Incorrect operand
         * counts corrupt control flow and can hang execution. */
        switch(in.sval){
          case -11:{ /* pushref: resource id OR function reference, selected by its FUNC occurrence.
                      * Treating every payload as a low-24 resource id left nested callbacks as plain
                      * integers, so a later callv silently had nothing callable to dispatch. */
            int fci=use_cache ? in.funcval_ci : -1;
            if(fci==-1){
              const char *fn=gml_ref_name(w,in.refaddr?in.refaddr:pc+4);
              if(fn && fn[0]!='?' && !strncmp(fn,"gml_",4)) fci=gml_code_index_by_name(w,fn);
            }
            {
              if(vm->diagnostics.push_reference_debug<0)
                vm->diagnostics.push_reference_debug=anygm_host_development_setting(vm->host,"GML_DBG_FUNCVAL")!=NULL;
              if(vm->diagnostics.push_reference_debug){
                const char *dbgfn=gml_ref_name(w,in.refaddr?in.refaddr:pc+4);
                if((dbgfn && !strncmp(dbgfn,"gml_",4)) || fci>=0) anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,
                  "[funcval] %s pc=%u pushref=%d name=%s -> ci=%d\n",
                  w->code[ci].name,pc-start,in.ival,dbgfn?dbgfn:"?",fci);
              } }
            GmlVal ref = fci>=0
              ? vreal((double)(GML_FUNCVAL_TAG|fci))
              : vreal((double)(in.ival & 0x00FFFFFF));
            if(sp<STK){ stk[sp]=ref; stkt[sp]=DT_VAR; sp++; }
            break; }
          case -2:   /* pushaf: A[idx] where the array value A is on the stack. Stack: idx, A(top->down). */
          case -4:{  /* pushac: non-terminal component of a chained read/write. A chained store
                      * contains ArrayPopAF for a[i], pushac for [j], then popaf for [k]. The
                      * intermediate zero must become a live sub-array or the final store has no
                      * receiver. A later terminal pushaf on the empty sub-array still yields zero. */
            int idx=(int)(sp>0?asnum(stk[--sp]):0); GmlVal av=sp>0?stk[--sp]:vreal(0);
            GmlVal out=vreal(0);
            if(in.sval==-4) out=gml_arr_chain_ensure(av,idx);
            else if(av.t==V_ARR && av.arr){ GmlArr *A=av.arr; if(idx>=0 && idx<A->len) out=A->data[idx]; }
            if(sp<STK){ stk[sp]=out; stkt[sp]=DT_VAR; sp++; } break; }
          case -3:{ /* popaf: A[idx] = value. */
            if(aref_n>0){ /* compound-assign write: use the reference saved at savearef; the store
               * may either retain a correctly reordered [value,array,index] triple or only the
               * computed value when the reference DUP cannot be represented. */
              int a=--aref_n;
              GmlArr *A=aref[a].arr; int idx=aref[a].idx;
              GmlVal val=sp>0?stk[sp-1]:vreal(0);
              int base=aref[a].base;
              if(sp>=3 && stk[sp-2].t==V_ARR && stk[sp-2].arr==A &&
                 (int)asnum(stk[sp-1])==idx){
                val=stk[sp-3];
                base=sp-3;
              }
              if(A && idx>=0){ gml_arr_index_ensure(A,idx); if(idx<A->cap){ A->data[idx]=val; if(val.t==V_STR) GC_UNTRACK(val.s); } }
              if(base>=0 && base<=sp) sp=base;
              break; }
            /* plain store. Stack: idx, A, value (top->down). */
            int idx=(int)(sp>0?asnum(stk[--sp]):0); GmlVal av=sp>0?stk[--sp]:vreal(0);
            GmlVal val=sp>0?stk[--sp]:vreal(0);
            if(av.t==V_ARR && av.arr && idx>=0){ GmlArr *A=av.arr; gml_arr_index_ensure(A,idx);
              if(idx<A->cap){ A->data[idx]=val; if(val.t==V_STR) GC_UNTRACK(val.s); } }
            break; }
          case -5: /* setowner: pop the copy-on-write owner id (COW is not modelled; arrays mutate in place). */
            if(sp>0) sp--;
            break;
          case -8:{ /* savearef: remember the array element being read so the paired popaf can write
             * it back. Stack top is [idx, array] (top->down); the read (pushaf) consumes them. */
            if(sp>=2 && stk[sp-2].t==V_ARR && stk[sp-2].arr && aref_n<16){
              aref[aref_n].arr=stk[sp-2].arr; aref[aref_n].idx=(int)asnum(stk[sp-1]);
              aref[aref_n].base=sp-2; aref_n++; }
            break; }
          case -6:{ /* isstaticok: push whether this function's static initializer has run. */
            int initialized=(ci>=0 && ci<vm->code_static_count && vm->code_static_init)
              ? vm->code_static_init[ci]!=0 : 0;
            if(sp<STK){ stk[sp]=vreal(initialized); stkt[sp]=DT_BOOL; sp++; }
            break; }
          case -7:  /* setstatic: latch BEFORE assignments, preventing recursive re-entry. */
            if(ci>=0 && ci<vm->code_static_count && vm->code_static_init)
              vm->code_static_init[ci]=1;
            break;
          /* chkindex(-1), restorearef(-9), chknullish(-10): no net stack effect here. */
          default: break;
        }
        break;
      default: break;
    }
    /* Record post-instruction stack depth in the optional opcode trace. */
    GML_VM_DIAGNOSTIC_OPCODE_AFTER(vm,w->code[ci].name,pc-start,sp);
    if(use_cache) ip=nextip;
    else pc=nextpc;
  }
  while(withsp>0){ withsp--; free(withstk[withsp].list); }  /* free any open with-frames */
  /* string GC: temporaries (builtin results / concatenations) are owned heap strings. Free every one
   * that wasn't transferred to a var (those were GC_UNTRACK'd on store, the var owns them now). The
   * return value is dup'd first so the CALLER owns an independent copy — GM strings are values, and
   * this also prevents freeing a var's string that a script returned (e.g. `return global.text`). */
  if(STR_IS_HEAP(ret)){
    int tracked=0; for(int i=0;i<str_gc_n;i++) if(str_gc[i]==ret.s){tracked=1;break;}
    if(tracked) GC_UNTRACK(ret.s);   /* live temp: hand ownership to the caller, no copy */
    else { char *rc=strdup(ret.s); ret = rc? vstr_owned(rc) : vstr(""); }  /* a var's string: copy it */
  }
  for(int i=0;i<str_gc_n;i++){
    void *p=str_gc[i]; if(!p) continue;
    free(p);
    for(int j=i+1;j<str_gc_n;j++) if(str_gc[j]==p) str_gc[j]=NULL;   /* de-dup: never double-free */
  }
  free(str_gc);
  #undef GC_TRACK
  #undef STR_IS_HEAP
  #undef GC_UNTRACK
  gml_arr_mark_escaped(ret);      /* a returned array escapes to the caller */
  gml_varmap_free_ex(&locals,1);      /* keep escaped arrays alive — persistent slots alias them */
  vm->cur_self=save_self; vm->cur_other=save_other;
  vm->cur_code_index=save_code_index;
  vm->caller_code_index=save_caller_index;
  vm->call_seq=save_call_seq;
  if(vm->code_depth>0) vm->code_depth--;
  vm->script_argc=save_argc;
  for(int i=0;i<16;i++) vm->script_args[i]=save_args[i];
  if(cp) codeprof_add(vm,ci,codeprof_now_ms(vm)-cp_t0,watchdog);
  vm->execution_depth--;
  return ret;
}

GmlVal gml_vm_call_callable(GmlVM *vm, GmlVal callable, GmlVal *args, int n_args){
  if(!vm || !vm->win || callable.t!=V_REAL) return vundef();
  int fci=-1;
  GmlInstance *call_self=vm->cur_self;
  double raw=callable.d;
  if(GML_IS_STRUCT_ID(raw)){
    GmlInstance *method=gml_struct_find(vm,(unsigned)raw);
    if(method){
      GmlVal selfv=vundef();
      int have_self=0;
      method_struct_info(method,&fci,&selfv,&have_self);
      if(have_self){
        GmlInstance *bound=vm_inst_from_ref(vm,selfv);
        if(bound) call_self=bound;
      }
    }
  } else if(GML_IS_FUNCVAL((int)raw)) {
    fci=(int)raw & 0x00FFFFFF;
  }
  if(fci<0 || fci>=vm->win->n_code) return vundef();
  return gml_vm_run_code(vm,fci,call_self,vm->cur_other,args,n_args);
}

GmlVal gml_vm_call_member_callable(GmlVM *vm, GmlVal receiver,
                                   GmlVal callable, GmlVal *args,
                                   int n_args){
  if(!vm || !vm->win || callable.t!=V_REAL) return vundef();
  GmlInstance *call_self=vm_inst_from_ref(vm,receiver);
  if(!call_self) return vundef();
  int fci=-1;
  double raw=callable.d;
  if(GML_IS_STRUCT_ID(raw)){
    GmlInstance *method=gml_struct_find(vm,(unsigned)raw);
    if(method){
      GmlVal selfv=vundef();
      int have_self=0;
      method_struct_info(method,&fci,&selfv,&have_self);
      /* Constructor-static methods carry a late-bound receiver marker. Other
       * method values retain the scope captured by method(). */
      if(have_self && !(selfv.t==V_REAL && selfv.d==-16.0)){
        GmlInstance *bound=vm_inst_from_ref(vm,selfv);
        if(bound) call_self=bound;
      }
    }
  } else if(GML_IS_FUNCVAL((int)raw)){
    fci=(int)raw & 0x00FFFFFF;
  }
  if(fci<0 || fci>=vm->win->n_code) return vundef();
  GmlVal result=vreal(0);
  GmlInstance *old_self=vm->cur_self;
  vm->cur_self=call_self;
  int micro_ok=code_micro_maybe(vm,fci,args,n_args,&result);
  vm->cur_self=old_self;
  if(!micro_ok)
    result=gml_vm_run_code(vm,fci,call_self,vm->cur_other,args,n_args);
  return result;
}
