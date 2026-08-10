/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* gml_vm_frame.c — VM step, draw, and per-frame resource coordination. */
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

double gml_legacy_view_follow_axis(double current, double target, double extent,
                                   double border, double speed){
  if(2*border>=extent) return target-extent/2;
  if(target-border<current){
    double wanted=target-border;
    return speed<0?wanted:current-fmin(current-wanted,fmax(speed,0));
  }
  if(target+border>current+extent){
    double wanted=target+border-extent;
    return speed<0?wanted:current+fmin(wanted-current,fmax(speed,0));
  }
  return current;
}

void gml_vm_instances_run_collisions(GmlVM *vm);
void gml_vm_instances_run_boundary_events(GmlVM *vm);
int gml_vm_instances_step_snapshot_member(GmlVM *vm, const GmlInstance *in);
#define VMPROF_MARK(field) do{ (void)sizeof(#field); }while(0)

static const char *instance_skeleton_attachment(void *context,const char *slot){
  GmlInstance *instance=(GmlInstance*)context;
  char key[384];
  int length=snprintf(key,sizeof(key),"__skel_attachment:%s",slot?slot:"");
  if(length<0 || (size_t)length>=sizeof(key)) return NULL;
  GmlVal *value=gml_varmap_get(&instance->vars,key);
  return value && value->t==V_STR ? (value->s?value->s:"") : NULL;
}

static int instance_skeleton_bone(void *context,const char *kind,
                                  const char *bone,const char *field,
                                  double *value){
  GmlInstance *instance=(GmlInstance*)context;
  char key[384];
  int length=snprintf(key,sizeof(key),"__skel_%s:%s:%s",
                      kind?kind:"",bone?bone:"",field?field:"");
  if(length<0 || (size_t)length>=sizeof(key)) return 0;
  GmlVal *stored=gml_varmap_get(&instance->vars,key);
  if(!stored || stored->t==V_UNDEF || stored->t==V_STR) return 0;
  *value=stored->d;
  return 1;
}

static GmlVal *instance_skeleton_value(GmlInstance *instance,const char *name){
  return instance?gml_varmap_get(&instance->vars,name):NULL;
}

static double instance_skeleton_time(GmlInstance *instance){
  GmlVal *value=instance_skeleton_value(instance,"__skel_time");
  return value && value->t==V_REAL ? value->d : 0;
}

static void instance_skeleton_time_set(GmlInstance *instance,double time){
  GmlVal *value=instance_skeleton_value(instance,"__skel_time");
  if(!value) value=gml_varmap_put(&instance->vars,"__skel_time");
  if(value) *value=vreal(time);
}

void gml_vm_draw_instance_sprite(GmlVM *vm,GmlInstance *instance,double alpha){
  GmlRender *render=vm?(GmlRender*)vm->render:NULL;
  if(!render || !instance || instance->sprite_index<0) return;
  if(gml_render_sprite_is_skeleton(render,(int)instance->sprite_index)){
    GmlVal *animation=instance_skeleton_value(instance,"__skel_animation");
    GmlVal *skin=instance_skeleton_value(instance,"__skel_skin");
    GmlRenderSkeletonState state;
    memset(&state,0,sizeof(state));
    state.context=instance;
    state.animation=(animation&&animation->t==V_STR)?animation->s:NULL;
    state.skin=(skin&&skin->t==V_STR)?skin->s:NULL;
    state.time=instance_skeleton_time(instance);
    state.attachment=instance_skeleton_attachment;
    state.bone=instance_skeleton_bone;
    gml_render_skeleton_state_set(render,&state);
  }
  gml_draw_sprite_ext(render,(int)instance->sprite_index,(int)instance->image_index,
                      instance->x,instance->y,
                      instance->image_xscale,instance->image_yscale,
                      instance->image_angle,(uint32_t)instance->image_blend,alpha);
  gml_render_skeleton_state_set(render,NULL);
}

static void advance_instance_animation(GmlVM *vm,GmlInstance *in,
                                       GmlRender *render,const char *anim_dbg){
  int nf=render?gml_sprite_frames(render,(int)in->sprite_index):0;
  if(anim_dbg){ const char *on=(in->obj>=0&&in->obj<vm->n_objects)?vm->objects[in->obj].name:"";
    if(on && strstr(on,anim_dbg)){
      int si=(int)in->sprite_index;
      GmlRenderSpriteMetrics sprite;
      const char *sn=(render&&gml_render_sprite_metrics(render,si,&sprite)&&sprite.name)?sprite.name:"?";
      anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[anim] %s x=%.1f y=%.1f vs=%.1f spr=%d(%s) idx=%.2f nf=%d\n",
        on,in->x,in->y,in->vspeed,si,sn,in->image_index,nf);
    }
  }
  if(render && gml_render_sprite_is_skeleton(render,(int)in->sprite_index)){
    if(in->image_speed!=0){
      GmlVal *animation=instance_skeleton_value(in,"__skel_animation");
      const char *name=(animation&&animation->t==V_STR)?animation->s:NULL;
      double duration=gml_render_skeleton_animation_duration(
        render,(int)in->sprite_index,name);
      double time=instance_skeleton_time(in)+
        in->image_speed/fmax(gml_room_speed(vm),1);
      int wrapped=duration>0 && (time>=duration || time<0);
      if(duration>0){
        while(time>=duration) time-=duration;
        while(time<0) time+=duration;
      }
      instance_skeleton_time_set(in,time);
      if(wrapped) gml_run_event(vm,in,"Other_7");
    }
    return;
  }
  /* An instance without sprite frames still advances image_index. Frames are
   * required for wrapping, and Animation End remains tied to that boundary
   * except for the existing classic-runtime policy. */
  int classic_runtime=vm->win && anygm_policy_uses_classic_runtime(vm->win);
  if(in->image_speed!=0){
    double step=(vm->win && !classic_runtime && nf>0)
      ? gml_sprite_animation_delta(render,(int)in->sprite_index,in->image_speed,gml_room_speed(vm))
      : in->image_speed;
    double ni=in->image_index+step;
    int wrapped=nf>0 && ((ni>=nf)||(ni<0));
    if(nf>0){ while(ni>=nf) ni-=nf; while(ni<0) ni+=nf; }
    in->image_index=ni;
    if(wrapped || (nf<=0 && ni>=0 && classic_runtime)){
      if(anygm_host_development_setting(vm->host,"GML_LOG_ANIM")) anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[anim-end] %s (nf=%d)\n",
        (in->obj>=0&&in->obj<vm->n_objects)?vm->objects[in->obj].name:"?",nf);
      gml_run_event(vm,in,"Other_7");
    }
  }
}

static void advance_instance_animations(GmlVM *vm){
  GmlRender *render=(GmlRender*)vm->render;
  const char *anim_dbg=anygm_host_development_setting(vm->host,"GML_ANIM_OBJ");
  int snapshot=anygm_policy_snapshot_instance_iteration(vm->win);
  int extent=(snapshot && vm->step_active)?vm->step_alloc_base:vm->inst_count;
  for(int i=0;i<extent;i++){
    GmlInstance *in=&vm->inst[i];
    if(!in->active||in->marked||
       (snapshot && !gml_vm_instances_step_snapshot_member(vm,in))) continue;
    advance_instance_animation(vm,in,render,anim_dbg);
  }
}

void gml_vm_post_draw(GmlVM *vm){
  /* The advance is owed here but taken at the start of the next step, which is the same point in a
   * continuous run and the other side of the frame boundary. States are serialized on that
   * boundary: taking it here would leave every state holding an animation one step past the frame
   * it was saved from, so restoring it and redrawing would not reproduce that frame. */
  if(vm && anygm_policy_animation_before_step(vm->win)) vm->animation_due=1;
}

static int classic_joystick_event_fires(GmlVM *vm,int s){
  int device=-1, control=-1;
  if(s>=16 && s<=19){
    static const int directions[4]={32783,32784,32781,32782};
    device=0; control=directions[s-16];
  } else if(s>=21 && s<=28){
    device=0; control=32768+(s-20);
  } else if(s>=31 && s<=34){
    static const int directions[4]={32783,32784,32781,32782};
    device=1; control=directions[s-31];
  } else if(s>=36 && s<=43){
    device=1; control=32768+(s-35);
  }
  return device==0 && gml_input_gamepad(vm,control,0);
}
static int mouse_event_fires(GmlVM *vm,int s,int hov,int was,int held,int pressed,int released,int wheel){
  switch(s){
    case 0: return hov&&(held&1);       case 1: return hov&&(held&2);
    case 2: return hov&&(held&4);       case 3: return hov&&!(held&7);
    case 4: return hov&&(pressed&1);    case 5: return hov&&(pressed&2);
    case 6: return hov&&(pressed&4);    case 7: return hov&&(released&1);
    case 8: return hov&&(released&2);   case 9: return hov&&(released&4);
    case 10:return hov&&!was;           case 11:return !hov&&was;
    case 50:return (held&1)!=0;         case 51:return (held&2)!=0;
    case 52:return (held&4)!=0;         case 53:return (pressed&1)!=0;
    case 54:return (pressed&2)!=0;      case 55:return (pressed&4)!=0;
    case 56:return (released&1)!=0;     case 57:return (released&2)!=0;
    case 58:return (released&4)!=0;     case 60:return wheel>0;
    case 61:return wheel<0;             default:return classic_joystick_event_fires(vm,s);
  }
}
static int room_element_animation_speed_type(GmlVM *vm, const GmlRtElem *element,
                                             int *speed_type){
  if(!vm || !vm->win || !element || vm->room_index<0) return 0;
  GmlRtLayer *runtime_layer=gml_rt_layer_find(vm,element->layer);
  if(!runtime_layer || !runtime_layer->name[0]) return 0;
  const uint8_t *data=vm->win->data;
  uint32_t count=0;
  uint32_t layers=gml_vm_rooms_layer_list(vm,vm->room_index,&count);
  if(!layers || count>=512) return 0;
  for(uint32_t index=0;index<count;index++){
    uint32_t layer=gml_vm_read_u32_le(data,layers+4+index*4);
    if(!layer || layer+12>vm->win->size) continue;
    uint32_t name=gml_vm_read_u32_le(data,layer);
    if(!name || name>=vm->win->size ||
       strcmp((const char*)(data+name),runtime_layer->name)) continue;
    uint32_t type=gml_vm_read_u32_le(data,layer+8);
    uint32_t type_data=gml_room_layer_type_off(vm,layer);
    if(element->type==1 && type==1 && type_data+40<=vm->win->size){
      if(speed_type) *speed_type=(int)gml_vm_read_u32_le(data,type_data+36);
      return 1;
    }
    if(element->type==3 && type==3 && type_data+8<=vm->win->size){
      uint32_t sprites=gml_vm_read_u32_le(data,type_data+4);
      uint32_t sprite_count=(sprites && sprites+4<=vm->win->size)
        ? gml_vm_read_u32_le(data,sprites):0;
      if(sprite_count>100000 ||
         (uint64_t)sprites+4+(uint64_t)sprite_count*4>vm->win->size) return 0;
      for(uint32_t sprite_index=0;sprite_index<sprite_count;sprite_index++){
        uint32_t record=gml_vm_read_u32_le(data,sprites+4+sprite_index*4);
        if(!record || record+44>vm->win->size) continue;
        uint32_t element_name=gml_vm_read_u32_le(data,record);
        if(element_name && element_name<vm->win->size &&
           !strcmp((const char*)(data+element_name),element->name)){
          if(speed_type) *speed_type=(int)gml_vm_read_u32_le(data,record+32);
          return 1;
        }
      }
    }
  }
  return 0;
}

void gml_vm_frame_advance_layers(GmlVM *vm){
  if(!vm) return;
  /* GMS2 layers scroll by their hspeed/vspeed each step. Runtime-scripted layers accumulate here;
   * untouched ones are derived on the fly from the room definition (see the draw path). */
  for(int i=0;i<vm->n_rtl;i++) if(vm->rtl[i].used && vm->rtl[i].touched){
    vm->rtl[i].x += vm->rtl[i].hs; vm->rtl[i].y += vm->rtl[i].vs; }
  { GmlRender *R=(GmlRender*)vm->render;
    for(int i=0;i<vm->n_rte;i++){
      GmlRtElem *e=&vm->rte[i];
      if(!e->used || (e->type!=1 && e->type!=3) || e->image_speed==0) continue;
      int speed_type=0;
      int native_element=room_element_animation_speed_type(vm,e,&speed_type);
      /* Native room layers are a structural GMS2 feature and can occur with early bytecode.
       * Classic backgrounds and non-layer compatibility records keep their separate cadence. */
      if(e->type==1 &&
         (!vm->win || anygm_policy_uses_classic_runtime(vm->win) ||
          (!anygm_policy_has_modern_function_values(vm->win) &&
           !native_element))) continue;
      int nf=R?gml_sprite_frames(R,e->sprite):0;
      double delta;
      if(native_element){
        delta=e->image_speed;
        if(speed_type==0) delta/=fmax(gml_room_speed(vm),1);
      } else
        delta=gml_sprite_animation_delta(R,e->sprite,e->image_speed,gml_room_speed(vm));
      e->image_index += delta;
      if(nf>0){
        while(e->image_index>=nf) e->image_index-=nf;
        while(e->image_index<0) e->image_index+=nf;
      }
    }
  }
}

static void gml_vm_apply_pending_room(GmlVM *vm){
  int target=vm->pending_room;
  int previous_count=vm->inst_count;
  int advance_entered=vm->step_active && vm->win &&
    !anygm_policy_animation_before_step(vm->win);
  uint32_t *previous_active_ids=NULL;
  if(advance_entered && previous_count>0){
    previous_active_ids=calloc((size_t)previous_count,sizeof(*previous_active_ids));
    if(previous_active_ids){
      for(int i=0;i<previous_count;i++)
        if(vm->inst[i].active && !vm->inst[i].marked)
          previous_active_ids[i]=vm->inst[i].id;
    } else anygm_host_logf(vm->host,ANYGM_LOG_WARN,
      "[gml] room transition animation snapshot unavailable\n");
  }
  vm->pending_room=-1;
  vm->step_alloc_base=0;
  gml_room_enter(vm,target);
  if(advance_entered){
    /* Newly bound layer assets receive the first animation tick at the same room-entry step
     * boundary as new instances. Scrolling still derives from room_enter_frame, so only stateful
     * layer elements advance here. */
    if(anygm_policy_has_modern_layer_semantics(vm->win))
      gml_vm_frame_advance_layers(vm);
    GmlRender *render=(GmlRender*)vm->render;
    const char *anim_dbg=anygm_host_development_setting(vm->host,"GML_ANIM_OBJ");
    for(int i=0;i<vm->inst_count;i++){
      GmlInstance *in=&vm->inst[i];
      if(!in->active || in->marked) continue;
      if(previous_active_ids && i<previous_count && previous_active_ids[i]==in->id) continue;
      /* Only persistent instances can survive a room change. If the optional identity snapshot
       * could not be allocated, avoid advancing those survivors twice; ordinary entered and
       * reactivated room instances still retain the first-draw phase. */
      if(!previous_active_ids && previous_count>0 && in->persistent) continue;
      advance_instance_animation(vm,in,render,anim_dbg);
    }
  }
  free(previous_active_ids);
}

static void gml_vm_finish_step(GmlVM *vm,int previous_alloc_base){
  vm->step_alloc_base=previous_alloc_base;
  vm->step_active=0;
  vm->step_free_n=vm->step_free_pos=0;
  gml_vm_instances_trim_pool_tail(vm);
}

static int gml_vm_finish_classic_room_request(GmlVM *vm,int previous_alloc_base){
  if(vm->pending_room<0 || !vm->win || !anygm_policy_uses_classic_runtime(vm->win)) return 0;
  gml_vm_apply_pending_room(vm);
  gml_vm_finish_step(vm,previous_alloc_base);
  return 1;
}

void gml_vm_step(GmlVM *vm){
  if(vm && vm->classic_info_active){
    if(gml_keyboard_check(vm,1,1)) vm->classic_info_active=0;
    return;
  }
  /* The classic animation advance owed by the previous frame's draw (see gml_vm_post_draw). */
  if(vm->animation_due){ vm->animation_due=0; advance_instance_animations(vm); }
  vm->frame++;
  if(vm->render) gml_render_set_frame((GmlRender*)vm->render,vm->frame);
  gml_vm_frame_advance_layers(vm);
  /* Collect transient structures between frames while stacks and locals are empty. Without this,
   * unreferenced per-step structures accumulate indefinitely. */
  if(vm->n_structs>0 && vm->frame - vm->structs_last_gc_frame >= 120){
    vm->structs_last_gc_frame = vm->frame; gml_struct_gc(vm); }
  int n=vm->inst_count;
  int prev_alloc_base=vm->step_alloc_base;
  vm->step_active=1;
  vm->step_first_id=vm->next_id;
  gml_vm_instances_prepare_step(vm,n);
  vm->step_alloc_base=n;
  /* xprevious/yprevious describe the position at the start of this step. This
   * must precede user Step code: classic content commonly moves by assigning x/y
   * directly, and solid collision rollback needs the position from before
   * those assignments. */
  for(int i=0;i<n;i++) if(vm->inst[i].active && !vm->inst[i].marked){
    vm->inst[i].xprevious=vm->inst[i].x;
    vm->inst[i].yprevious=vm->inst[i].y;
  }
  /* Studio advances animation before Step. GM6-8 advances it after the draw phase; the host
   * calls gml_vm_post_draw() once the complete classic frame has been rendered. */
  if(!anygm_policy_animation_before_step(vm->win)) advance_instance_animations(vm);
  VMPROF_MARK(anim);
  /* GML_GOD: development capture aid for content that exposes a cooldown through an alarm. */
  {
    if(!vm->diagnostics.god_initialized){
      const char *g=anygm_host_development_setting(vm->host,"GML_GOD");
      const char *o=anygm_host_development_setting(vm->host,"GML_GOD_OBJ");
      vm->diagnostics.god_enabled=g!=NULL;
      if((!o || !*o) && g && *g && strcmp(g,"1") && strcmp(g,"on") && strcmp(g,"true")) o=g;
      snprintf(vm->diagnostics.god_object,sizeof vm->diagnostics.god_object,"%s",(o&&*o)?o:"");
      vm->diagnostics.god_alarm=0;
      vm->diagnostics.god_value=120;
      const char *a=anygm_host_development_setting(vm->host,"GML_GOD_ALARM"); if(a && *a) vm->diagnostics.god_alarm=atoi(a);
      if(vm->diagnostics.god_alarm<0 || vm->diagnostics.god_alarm>=GML_ALARMS) vm->diagnostics.god_alarm=0;
      const char *v=anygm_host_development_setting(vm->host,"GML_GOD_VALUE"); if(v && *v) vm->diagnostics.god_value=atoi(v);
      if(vm->diagnostics.god_value<0) vm->diagnostics.god_value=0;
      vm->diagnostics.god_initialized=1;
    }
    if((vm->diagnostics.god_enabled || vm->god_mode) && vm->diagnostics.god_object[0]){
      int po=gml_object_index_by_name(vm,vm->diagnostics.god_object);
      GmlInstance *pl = po>=0 ? gml_find_instance(vm,po) : NULL;
      if(pl) pl->alarm[vm->diagnostics.god_alarm]=vm->diagnostics.god_value;
    } }
  /* begin step */
  gml_vm_instances_run_classic_triggers(vm,1);
  if(vm->win && anygm_policy_uses_classic_runtime(vm->win)) gml_vm_instances_run_classic_event(vm,"Step_1");
  else for(int i=0;i<n;i++) if(vm->inst[i].active && !vm->inst[i].marked &&
      gml_vm_instances_step_snapshot_member(vm,&vm->inst[i])){
    gml_run_event(vm,&vm->inst[i],"Step_1");
    if(vm->pending_room>=0){
      /* Commit a Begin Step room request after the requesting event. The target room skips Begin
       * Step but participates in every remaining phase of this same frame. */
      gml_vm_apply_pending_room(vm);
      n=vm->inst_count;
      gml_vm_instances_prepare_step(vm,n);
      vm->step_alloc_base=n;
      vm->step_first_id=vm->next_id;
      break;
    }
  }
  VMPROF_MARK(step1);
  if(gml_vm_finish_classic_room_request(vm,prev_alloc_base)){
    VMPROF_MARK(rest);
    return;
  }
  /* GMS2 ticks both built-in time-source trees after every Begin Step and before the remaining
   * Step phases. Sources created by a callback join on the next tick via the scheduler snapshot. */
  gml_time_sources_tick(vm);
  gml_vm_rooms_step_timelines(vm,n);
  /* Alarm firing thresholds and dispatch order depend on the compatibility generation:
   *  Bytecode 14/15: decrement any alarm > -1, fire when the result crosses BELOW 0. An integer
   *    alarm[i]=N fires N+1 steps later.
   *  Bytecode 16 and later: decrement only alarms > 0, fire when the result reaches <= 0. An integer
   *    alarm[i]=N fires N steps later.
   *  Fractional alarms (e.g. a length-scaled cost) fire on the SAME tick under both rules (first
   *  decrement that lands <= 0), so cutscene typewriters are unaffected. Set -1 before running
   *  the event so the handler can re-arm. Classic and bytecode-16 policies dispatch each
   *  alarm subtype by ascending exact object resource, then insertion order within that object. */
  int resource_major_alarm_order=anygm_policy_resource_major_alarm_dispatch(vm->win);
  int alarm_at_zero = anygm_policy_alarm_at_zero(vm->win);
  if(resource_major_alarm_order){
    for(int a=0;a<GML_ALARMS;a++){
      char s[16]; snprintf(s,sizeof s,"Alarm_%d",a);
      int object_count=0;
      const int *objects=gml_vm_instances_event_objects(vm,s,&object_count);
      int extent=objects?object_count:vm->n_objects;
      for(int oi=0;oi<extent;oi++){
        int object=objects?objects[oi]:oi;
        int declared_code=-1;
        int native_declared=gml_vm_instances_native_event_declared(
          vm,2,a,object,NULL,&declared_code);
        if(!native_declared && !gml_vm_instances_event_lookup(vm,s,object,NULL,NULL)) continue;
        int count=gml_vm_instances_collect_object_slots(vm,object);
        if(count<0) continue;
        for(int k=count-1;k>=0;k--){ int i=vm->event_ord[k];
          if(i>=vm->inst_count) continue;
          GmlInstance *in=&vm->inst[i];
          if(!in->active||in->marked||in->obj!=object||!(in->alarm[a]>0)) continue;
          in->alarm[a]-=1;
          if(in->alarm[a]<=0){ in->alarm[a]=-1;
            if(anygm_host_development_setting(vm->host,"GML_LOG_ALARM")) anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[alarm] f%ld %s.%s\n",vm->frame,
              vm->objects[in->obj].name,s);
            if(!native_declared || declared_code>=0) gml_run_event(vm,in,s); }
        }
      }
    }
  } else for(int i=0;i<n;i++){ GmlInstance *in=&vm->inst[i];
    if(!in->active||in->marked||!gml_vm_instances_step_snapshot_member(vm,in)) continue;
    for(int a=0;a<GML_ALARMS;a++){
      if(alarm_at_zero){ if(!(in->alarm[a]>0)) continue; } else { if(!(in->alarm[a]>-1)) continue; }
      char s[16]; snprintf(s,sizeof s,"Alarm_%d",a);
      int declared_code=-1;
      int native_declared=gml_vm_instances_native_event_declared(
          vm,2,a,in->obj,NULL,&declared_code);
      /* Alarms count down only when this object family declares the matching event. Native
       * records matter here because an empty event has no CODE name but still owns the countdown.
       * A genuinely unused alarm slot remains an ordinary writable value. */
      if(!native_declared && !gml_vm_instances_event_lookup(vm,s,in->obj,NULL,NULL)) continue;
      in->alarm[a]-=1;
      int fire = alarm_at_zero ? (in->alarm[a]<=0) : (in->alarm[a]<0);
      if(fire){ in->alarm[a]=-1;
        if(anygm_host_development_setting(vm->host,"GML_LOG_ALARM")) anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[alarm] f%ld %s.%s\n",vm->frame,
          (in->obj>=0&&in->obj<vm->n_objects)?vm->objects[in->obj].name:"?",s);
        /* An explicitly empty declaration completes at -1 without inheriting or executing code. */
        if(!native_declared || declared_code>=0) gml_run_event(vm,in,s); } } }
  VMPROF_MARK(alarms);
  /* Classic compatibility commits a room change requested by an Alarm before normal Step. The
   * requesting event still completes, so assignments after room_goto remain visible, but a
   * newly created persistent instance must not run Step against the old room dimensions. */
  if(vm->pending_room>=0 && vm->win && anygm_policy_uses_classic_runtime(vm->win)){
    gml_vm_apply_pending_room(vm);
    gml_vm_finish_step(vm,prev_alloc_base);
    VMPROF_MARK(rest);
    return;
  }
  /* keyboard events (GM order: after alarms, before the normal step) */
  if(vm->n_key_events){
    for(int e=0;e<vm->n_key_events;e++){
      if(!gml_keyboard_check(vm,vm->key_events[e].vk,vm->key_events[e].kind)) continue;
      if(vm->win && anygm_policy_uses_classic_runtime(vm->win))
        gml_vm_instances_run_classic_event(vm,vm->key_events[e].suffix);
      else for(int i=0;i<n;i++){
        if(vm->inst[i].active && !vm->inst[i].marked &&
           gml_vm_instances_step_snapshot_member(vm,&vm->inst[i]))
          gml_run_event(vm,&vm->inst[i],vm->key_events[e].suffix); }
      if(gml_vm_finish_classic_room_request(vm,prev_alloc_base)){
        VMPROF_MARK(rest);
        return;
      }
    }
  }
  /* instance Mouse_<n> events (with the other input events). Hover = pointer (room coords)
   * inside the instance bbox; enter/leave tracked per instance in mouse_over. Subtypes per GM:
   * 0-2 button held over it, 3 no-button over it, 4-6 pressed, 7-9 released, 10 enter, 11 leave,
   * 16-28/31-43 joystick 1/2, 50-58 global (no hover), 60/61 wheel. Only games that define
   * Mouse_* events pay any cost. */
  if(vm->n_mouse_events){
    double mx,my; int mheld,mpressed,mreleased,mwheel;
    gml_input_mouse(vm,&mx,&my,NULL,NULL,NULL,NULL,&mheld,&mpressed,&mreleased,&mwheel);
    if(vm->win && anygm_policy_uses_classic_runtime(vm->win)){
      for(int e=0;e<vm->n_mouse_events;e++){
        int s=vm->mouse_events[e].sub;
        int object_count=0;
        const int *objects=gml_vm_instances_event_objects(vm,vm->mouse_events[e].suffix,&object_count);
        int extent=objects?object_count:vm->n_objects;
        for(int oi=0;oi<extent;oi++){
          int object=objects?objects[oi]:oi;
          if(!objects && !gml_vm_instances_event_lookup(vm,vm->mouse_events[e].suffix,object,NULL,NULL)) continue;
          int count=gml_vm_instances_collect_object_slots(vm,object); if(count<0) continue;
          for(int k=count-1;k>=0;k--){ int i=vm->event_ord[k];
            if(i>=vm->inst_count) continue;
            GmlInstance *in=&vm->inst[i];
            if(!in->active||in->marked||in->deactivated||in->obj!=object) continue;
            double l=0,t=0,r2=0,b=0; int hov=0;
            int have_bbox=gml_vm_instances_bbox(vm,in,&l,&t,&r2,&b);
            if(have_bbox) hov=mx>=l&&mx<=r2&&my>=t&&my<=b;
            if(anygm_host_development_setting(vm->host,"GML_LOG_MOUSE_HIT") && mpressed)
              anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[mouse-hit] %s at=%.1f,%.1f bbox=%d:%.1f,%.1f..%.1f,%.1f hover=%d event=%s\n",
                (in->obj>=0&&in->obj<vm->n_objects)?vm->objects[in->obj].name:"?",
                mx,my,have_bbox,l,t,r2,b,hov,vm->mouse_events[e].suffix);
            if(mouse_event_fires(vm,s,hov,in->mouse_over,mheld,mpressed,mreleased,mwheel))
              gml_run_event(vm,in,vm->mouse_events[e].suffix);
          }
        }
      }
      for(int i=0;i<vm->inst_count;i++){ GmlInstance *in=&vm->inst[i];
        if(!in->active||in->marked||in->deactivated) continue;
        double l,t,r2,b; int hov=0;
        if(gml_vm_instances_bbox(vm,in,&l,&t,&r2,&b)) hov=mx>=l&&mx<=r2&&my>=t&&my<=b;
        in->mouse_over=(unsigned char)hov;
      }
    } else for(int i=0;i<n;i++){
      GmlInstance *in=&vm->inst[i];
      if(!in->active||in->marked||in->deactivated||!gml_vm_instances_step_snapshot_member(vm,in)) continue;
      double l,t,r2,b; int hov=0;
      if(gml_vm_instances_bbox(vm,in,&l,&t,&r2,&b)) hov=mx>=l&&mx<=r2&&my>=t&&my<=b;
      unsigned char was=in->mouse_over; in->mouse_over=(unsigned char)hov;
      for(int e=0;e<vm->n_mouse_events;e++) if(mouse_event_fires(vm,vm->mouse_events[e].sub,hov,was,mheld,mpressed,mreleased,mwheel))
        gml_run_event(vm,in,vm->mouse_events[e].suffix);
    }
  }
  VMPROF_MARK(input);
  /* Normal Step in classic formats is grouped by ascending object resource, with insertion order
   * inside each exact object. Each object takes its extent when that group begins: an earlier
   * object can create an instance whose later object group still sees it, while a same-object
   * creation waits until the next Step. First-generation policy refreshes its flat phase extent
   * after Alarm and input, so instances created earlier in the frame join normal Step. The later
   * policy retains the frame-start snapshot. */
  gml_vm_instances_run_classic_triggers(vm,0);
  if(vm->win && anygm_policy_uses_classic_runtime(vm->win)) gml_vm_instances_run_classic_event(vm,"Step_0");
  else {
    int step_count=anygm_policy_snapshot_instance_iteration(vm->win)?n:vm->inst_count;
    for(int i=0;i<step_count;i++) if(vm->inst[i].active && !vm->inst[i].marked &&
        gml_vm_instances_step_snapshot_member(vm,&vm->inst[i])){
      gml_run_event(vm,&vm->inst[i],"Step_0");
      /* Commit a room request after the requesting event completes. Do not let later members
       * of the old room's Step snapshot run before the room lifecycle boundary. */
      if(vm->pending_room>=0){
        gml_vm_apply_pending_room(vm);
        gml_vm_finish_step(vm,prev_alloc_base);
        VMPROF_MARK(rest);
        return;
      }
    }
  }
  VMPROF_MARK(step0);
  if(gml_vm_finish_classic_room_request(vm,prev_alloc_base)){
    VMPROF_MARK(rest);
    return;
  }
  /* Live-iteration generations include an instance created earlier in the frame in automatic
   * movement before its first draw. Snapshot iteration retains the frame-start extent. */
  int movement_count=anygm_policy_snapshot_instance_iteration(vm->win)?n:vm->inst_count;
  for(int i=0;i<movement_count;i++){ GmlInstance *in=&vm->inst[i];
    if(!in->active||in->marked||!gml_vm_instances_step_snapshot_member(vm,in)) continue;
    if(in->gravity!=0){ in->hspeed+=in->gravity*cos(in->gravity_direction*M_PI/180.0);
      in->vspeed-=in->gravity*sin(in->gravity_direction*M_PI/180.0);
      gml_vm_motion_from_components(vm,in);
    }
    if(in->friction!=0 && in->speed!=0){
      if(in->speed>0){
        if(in->friction>in->speed) in->speed=0;
        else in->speed-=in->friction;
      } else {
        if(in->friction>-in->speed) in->speed=0;
        else in->speed+=in->friction;
      }
      gml_vm_motion_from_speed_direction(vm,in);
    }
    in->x+=in->hspeed; in->y+=in->vspeed;
    if(in->hspeed!=0||in->vspeed!=0) gml_colgrid_touch(vm,in); }
  /* path following (path_start): move instances along their assigned path each step */
  gml_vm_rooms_step_paths(vm);
  /* room/view boundary events (Outside Room/View, Intersect Boundary) — after move */
  gml_vm_instances_run_boundary_events(vm);
  VMPROF_MARK(move);
  /* collision events (GM order: after move, before end step) */
  gml_vm_instances_run_collisions(vm);
  VMPROF_MARK(coll);
  /* end step */
  gml_vm_instances_run_classic_triggers(vm,2);
  if(vm->win && anygm_policy_uses_classic_runtime(vm->win)) gml_vm_instances_run_classic_event(vm,"Step_2");
  else for(int i=0;i<n;i++) if(vm->inst[i].active && !vm->inst[i].marked &&
      gml_vm_instances_step_snapshot_member(vm,&vm->inst[i]))
    gml_run_event(vm,&vm->inst[i],"Step_2");
  VMPROF_MARK(step2);
  if(gml_vm_finish_classic_room_request(vm,prev_alloc_base)){
    VMPROF_MARK(rest);
    return;
  }
  gml_vm_instances_reap(vm);
  { const char *iv=anygm_host_development_setting(vm->host,"GML_LOG_INSTVAR");   /* obj_name[@id]:var1,var2 — dump instance vars per frame */
    if(iv && *iv){ char buf[256]; snprintf(buf,sizeof buf,"%s",iv);
      char *colon=strchr(buf,':');
      if(colon){ *colon=0; unsigned wanted_id=0; char *at=strchr(buf,'@');
        if(at){ *at=0; wanted_id=(unsigned)strtoul(at+1,NULL,10); }
        int oi=gml_object_index_by_name(vm,buf); GmlInstance *in=NULL;
        if(wanted_id){ for(int i=0;i<vm->inst_count;i++) if(vm->inst[i].active &&
            !vm->inst[i].marked && vm->inst[i].id==wanted_id){ in=&vm->inst[i]; break; } }
        else if(oi>=0) in=gml_find_instance(vm,oi);
        if(in){ anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[ivar] f%ld %s id=%u",vm->frame,buf,in->id);
          for(char *tok=strtok(colon+1,",");tok;tok=strtok(NULL,",")){
            GmlVal *p=gml_varmap_get(&in->vars,tok);
            if(!p){   /* built-in struct fields aren't in the varmap — resolve the common ones */
              double bv; int have=1;
              if(!strcmp(tok,"x")) bv=in->x; else if(!strcmp(tok,"y")) bv=in->y;
              else if(!strcmp(tok,"hspeed")) bv=in->hspeed; else if(!strcmp(tok,"vspeed")) bv=in->vspeed;
              else if(!strcmp(tok,"speed")) bv=in->speed; else if(!strcmp(tok,"direction")) bv=in->direction;
              else if(!strcmp(tok,"sprite_index")) bv=in->sprite_index; else if(!strcmp(tok,"image_index")) bv=in->image_index;
              else if(!strcmp(tok,"image_speed")) bv=in->image_speed; else if(!strcmp(tok,"visible")) bv=in->visible;
              else if(!strcmp(tok,"depth")) bv=in->depth; else have=0;
              if(have){ anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG," %s=%.2f",tok,bv); continue; }
            }
            if(!p) anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG," %s=<unset>",tok);
            else if(p->t==V_REAL) anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG," %s=%.2f",tok,p->d);
            else if(p->t==V_STR) anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG," %s=\"%s\"",tok,p->s?p->s:"");
            else if(p->t==V_ARR && p->arr && ((GmlArr*)p->arr)->is_2d){ GmlArr *A=p->arr;
              anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG," %s=2D[h=%d]",tok,A->height2d);
              for(int r=0;r<A->height2d && r<6;r++){
                anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG," r%d(",r);
                int rl = (A->row_len && r<A->row_cap)? A->row_len[r] : 0;
                for(int c2=0;c2<rl && c2<4;c2++){
                  long ix=(long)r*32000+c2;
                  if(ix<A->len){ GmlVal *e=&A->data[ix];
                    if(e->t==V_STR) anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"\"%s\",",e->s?e->s:"");
                    else anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"%.4g,",e->t==V_REAL?e->d:-1); } }
                anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,")"); } }
            else if(p->t==V_ARR && p->arr){ GmlArr *A=p->arr;
              int stp=A->len>24?A->len/16:1; if(stp<1)stp=1;
              anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG," %s=[len=%d stp=%d:",tok,A->len,stp);
              for(int e=0;e<A->len;e+=stp)
                anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"%s%.1f",e?",":"",A->data[e].t==V_REAL?A->data[e].d:-999);
              anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"]"); }
            else anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG," %s=<t%d>",tok,p->t); }
          anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"\n"); } } } }
  { const char *gd=anygm_host_development_setting(vm->host,"GML_DBG_GLOBDUMP");   /* dump the whole globals varmap once at frame N */
    if(gd && vm->frame==atol(gd)){
      anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[globdump] f%ld cap=%d:\n",vm->frame,vm->globals.cap);
      for(int i=0;i<vm->globals.cap;i++){ GmlVarSlot *s=&vm->globals.slots[i];
        if(!s->key) continue;
        if(s->val.t==V_REAL) anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"  %s=%.2f\n",s->key,s->val.d);
        else anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"  %s=<t%d>\n",s->key,s->val.t); } } }
  { const char *gv=anygm_host_development_setting(vm->host,"GML_LOG_GLOBALVAR");   /* comma-separated global names, dumped per frame */
    if(gv && *gv){ char buf[256]; snprintf(buf,sizeof buf,"%s",gv);
      anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[gvar] f%ld",vm->frame);
      for(char *tok=strtok(buf,",");tok;tok=strtok(NULL,",")){
        GmlVal *p=gml_varmap_get(&vm->globals,tok);
        if(!p) anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG," %s=<unset>",tok);
        else if(p->t==V_REAL) anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG," %s=%.2f",tok,p->d);
        else if(p->t==V_STR) anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG," %s=\"%s\"",tok,p->s?p->s:"");
        else anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG," %s=<t%d>",tok,p->t); }
      anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"\n"); } }
  gml_part_update_all(vm->particles);   /* advance auto-update particle systems */
  /* Legacy automatic view-follow keeps its target inside the border band and honors per-axis
   * speed in Classic and bytecode-15 modes. A negative speed snaps and zero holds that axis.
   * Modern camera resources are advanced separately below. */
  if(anygm_host_development_setting(vm->host,"GML_LOG_FOLLOW")){ if(vm->diagnostics.follow_log_count++%60==0){
    int vo=(int)gml_vm_global_array_number(vm,"view_object",0);
    anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[follow] vis=%.2f vobj=%d wv=%.0f xv=%.0f yv=%.0f\n",
      gml_vm_global_array_number(vm,"view_visible",0),vo,
      gml_vm_global_array_number(vm,"view_wview",0),
      gml_vm_global_array_number(vm,"view_xview",0),gml_vm_global_array_number(vm,"view_yview",0)); } }
  int follow_views=(int)anygm_policy_legacy_view_slots(vm->win);
  for(int view=0;view<follow_views;view++) if(gml_vm_global_array_number(vm,"view_visible",view)>=0.5){
    int vobj=(int)gml_vm_global_array_number(vm,"view_object",view);
    GmlInstance *fo = vobj>=0 ? gml_find_instance(vm,vobj) : NULL;
    if(fo && view==0 && anygm_host_development_setting(vm->host,"GML_LOG_FOLLOW")){ if(vm->diagnostics.follow_target_log_count++%60==0)
      anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[follow-target] obj=%d(%s) id=%u at(%.0f,%.0f) hb=%.0f\n",
        fo->obj,(fo->obj>=0&&fo->obj<vm->n_objects)?vm->objects[fo->obj].name:"?",fo->id,fo->x,fo->y,
        gml_vm_global_array_number(vm,"view_hborder",view)); }
    if(fo){
      double vx=gml_vm_global_array_number(vm,"view_xview",view), vy=gml_vm_global_array_number(vm,"view_yview",view);
      double wv=gml_vm_global_array_number(vm,"view_wview",view), hv=gml_vm_global_array_number(vm,"view_hview",view);
      double hb=gml_vm_global_array_number(vm,"view_hborder",view), vb=gml_vm_global_array_number(vm,"view_vborder",view);
      double tx=fo->x, ty=fo->y;
      int classic=vm->win && anygm_policy_uses_classic_runtime(vm->win);
      if(classic){ tx=gml_vm_classic_round_even(tx); ty=gml_vm_classic_round_even(ty); }
      vx=gml_legacy_view_follow_axis(vx,tx,wv,hb,
        gml_vm_global_array_number(vm,"view_hspeed",view));
      vy=gml_legacy_view_follow_axis(vy,ty,hv,vb,
        gml_vm_global_array_number(vm,"view_vspeed",view));
      GmlRoom rm; if(gml_vm_room_get(vm,vm->room_index,&rm)==0){
        double mx=rm.width-wv, my=rm.height-hv;
        if(vx<0) vx=0;
        if(mx>0&&vx>mx) vx=mx;
        if(mx<=0) vx=0;
        if(vy<0) vy=0;
        if(my>0&&vy>my) vy=my;
        if(my<=0) vy=0;
      }
      gml_vm_global_array_set(vm,"view_xview",view,vx); gml_vm_global_array_set(vm,"view_yview",view,vy);
    }
  }
  /* Studio camera resources have their own target, border and speed state. Binding a camera
   * through view_camera[] does not copy those fields into the legacy view_* arrays: live camera
   * resources advance after Step and the renderer later resolves the bound handle. Consequently,
   * camera_create_view(..., target, speed, border) follows its target without an explicit
   * camera_set_view_pos call. */
  for(int camera=0;camera<GML_CAMERA_LIMIT;camera++){
    if(gml_vm_global_array_number(vm,"__gml_camera_live",camera)<0.5) continue;
    int target=(int)gml_vm_global_array_number(vm,"__gml_camera_target",camera);
    GmlInstance *fo=target>=0?gml_find_instance(vm,target):NULL;
    if(!fo) continue;
    double vx=gml_vm_global_array_number(vm,"__gml_camera_x",camera);
    double vy=gml_vm_global_array_number(vm,"__gml_camera_y",camera);
    double wv=gml_vm_global_array_number(vm,"__gml_camera_w",camera);
    double hv=gml_vm_global_array_number(vm,"__gml_camera_h",camera);
    if(wv<=0 || hv<=0) continue;
    double hb=gml_vm_global_array_number(vm,"__gml_camera_xborder",camera);
    double vb=gml_vm_global_array_number(vm,"__gml_camera_yborder",camera);
    double hs=gml_vm_global_array_number(vm,"__gml_camera_xspeed",camera);
    double vs=gml_vm_global_array_number(vm,"__gml_camera_yspeed",camera);
    double wanted_x=vx, wanted_y=vy;
    if(hb<0) hb=0;
    if(vb<0) vb=0;
    if(2*hb>=wv) wanted_x=fo->x-wv/2;
    else if(fo->x-hb<vx) wanted_x=fo->x-hb;
    else if(fo->x+hb>vx+wv) wanted_x=fo->x+hb-wv;
    if(2*vb>=hv) wanted_y=fo->y-hv/2;
    else if(fo->y-vb<vy) wanted_y=fo->y-vb;
    else if(fo->y+vb>vy+hv) wanted_y=fo->y+vb-hv;
    if(hs<0) vx=wanted_x;
    else if(hs>0){
      double delta=wanted_x-vx;
      if(delta>hs) delta=hs; else if(delta< -hs) delta= -hs;
      vx+=delta;
    }
    if(vs<0) vy=wanted_y;
    else if(vs>0){
      double delta=wanted_y-vy;
      if(delta>vs) delta=vs; else if(delta< -vs) delta= -vs;
      vy+=delta;
    }
    GmlRoom rm;
    if(gml_vm_room_get(vm,vm->room_index,&rm)==0){
      double mx=(double)rm.width-wv, my=(double)rm.height-hv;
      if(vx<0) vx=0;
      if(mx>0 && vx>mx) vx=mx;
      if(mx<=0) vx=0;
      if(vy<0) vy=0;
      if(my>0 && vy>my) vy=my;
      if(my<=0) vy=0;
    }
    gml_vm_global_array_set(vm,"__gml_camera_x",camera,vx);
    gml_vm_global_array_set(vm,"__gml_camera_y",camera,vy);
    if(camera==0 && anygm_host_development_setting(vm->host,"GML_LOG_FOLLOW")){ if(vm->diagnostics.camera_follow_log_count++%60==0)
      anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[camera-follow] cam=%d target=%d(%s) at=(%.0f,%.0f) view=(%.0f,%.0f %.0fx%.0f) border=(%.0f,%.0f) speed=(%.0f,%.0f)\n",
        camera,target,(fo->obj>=0&&fo->obj<vm->n_objects)?vm->objects[fo->obj].name:"?",
        fo->x,fo->y,vx,vy,wv,hv,hb,vb,hs,vs); }
  }
  /* room transition requested during the step */
  /* Classic room backgrounds start at their authored position for the first rendered frame;
   * their automatic speed is applied between frames. The step precedes drawing in this runtime,
   * so skip the first room step to preserve that phase. */
  if(!vm->win || !anygm_policy_uses_classic_runtime(vm->win) || vm->frame-vm->room_enter_frame>1)
    for(int i=0;i<8;i++){
      double hx=gml_vm_global_array_number(vm,"background_hspeed",i), vy=gml_vm_global_array_number(vm,"background_vspeed",i);
      if(hx!=0 || vy!=0){
        gml_vm_global_array_set(vm,"background_x",i,gml_vm_global_array_number(vm,"background_x",i)+hx);
        gml_vm_global_array_set(vm,"background_y",i,gml_vm_global_array_number(vm,"background_y",i)+vy);
      }
    }
  /* deferred async save/load completion events (Other_72) queued by buffer_*_async this step */
  gml_fire_async_saveload(vm);
  /* deferred async HTTP failure events (Other_62) queued by http_* this step (offline core) */
  gml_fire_async_http(vm);
  /* room transition requested during the step */
  if(vm->pending_room>=0) gml_vm_apply_pending_room(vm);
  /* Game End (Other_3): fire on all active instances when game_end was set. */
  if(vm->game_end) gml_vm_fire_game_end(vm);
  gml_vm_finish_step(vm,prev_alloc_base);
  VMPROF_MARK(rest);
}

void gml_vm_fire_game_end(GmlVM *vm){
  if(!vm) return;
  for(int i=0;i<vm->inst_count;i++) if(vm->inst[i].active && !vm->inst[i].marked)
    gml_run_event(vm,&vm->inst[i],"Other_3");
}

/* Draw phase: GM draws instances and room tiles interleaved by depth (high
 * depth = behind). Merging them preserves layer ordering between tile layers,
 * scripted background instances, and gameplay instances. */
typedef struct {
  double x,y,xs,ys;
  int def,sx,sy,w,h,mirror,flip,rotate,order;
} GmlDrawTile;
static void draw_tile_add(GmlDrawTile **tiles, double **depth, int *nt, int *cap, GmlDrawTile t, double dep, int order){
  if(*nt>=*cap){
    int nc=*cap?*cap*2:256;
    GmlDrawTile *n_tiles=malloc((size_t)nc*sizeof(**tiles));
    double *n_depth=malloc((size_t)nc*sizeof(**depth));
    if(!n_tiles || !n_depth){ free(n_tiles); free(n_depth); return; }
    if(*nt>0){ memcpy(n_tiles,*tiles,(size_t)*nt*sizeof(**tiles)); memcpy(n_depth,*depth,(size_t)*nt*sizeof(**depth)); }
    free(*tiles); free(*depth);
    *tiles=n_tiles; *depth=n_depth; *cap=nc;
  }
  t.order=order;
  (*tiles)[*nt]=t; (*depth)[*nt]=dep; (*nt)++;
}
/* GmlDrawItem lives in gml_vm_internal.h so the sort guard in the unit suite
 * can drive the comparator and sort below over synthetic lists. */
int gml_vm_draw_item_cmp(const void *pa, const void *pb){
  const GmlDrawItem *a=pa,*b=pb;
  if(a->depth!=b->depth) return a->depth>b->depth? -1:1;     /* higher depth first (behind) */
  if(a->order>=0 && b->order>=0 && a->order!=b->order)
    return a->order>b->order? -1:1;                           /* GMS2 layer list: later/back layers first */
/* Equal-depth ordering:
   *  - ROOM-chunk tiles draw on top of same-depth instances.
   *  - ROOM-chunk tiles retain list order, so later tiles draw on top.
   *  - Studio dynamic-depth layers draw in front of same-depth named instance layers. Peers on
   *    the same layer retain reverse instance-chain order (newer first, older on top).
   *  - runtime layer items (types 2/3) keep the seq rule (below instances). */
  int at1=a->type==1, bt1=b->type==1;
  if(at1!=bt1) return at1? 1 : -1;                           /* room tile sorts later (front) */
  if(at1 && bt1) return a->seq<b->seq? -1 : (a->seq>b->seq?1:0);   /* tiles: list order, later on top */
  /* Classic background slots are painted in array order. They deliberately share one
   * synthetic depth, so keep the lower slot behind and let later slots overlay it. The
   * generic instance tie-break below does the opposite and would place slot zero last,
   * allowing an opaque sky layer to hide every following scenery layer. */
  if(a->type==6 && b->type==6)
    return a->seq<b->seq? -1 : (a->seq>b->seq?1:0);
  /* Classic room actors use the resource grouping encoded by the room, while
   * runtime-created peers retain insertion order and overlay room content. */
  if(a->type==0 && b->type==0 && a->classic && b->classic && a->placed!=b->placed)
    return a->placed? -1:1;
  /* Editor-project containers do not retain the complete serialized room chain and are grouped
   * by object resource. Embedded-layout rooms retain that chain, so their placed peers preserve
   * insertion order like runtime-created peers. */
  if(a->type==0 && b->type==0 && a->classic==1 && b->classic==1 &&
     a->placed && a->obj!=b->obj)
    return a->obj>b->obj? -1:1;
  if(a->type==0 && b->type==0 && a->classic && b->classic)
    return a->seq<b->seq? -1 : (a->seq>b->seq?1:0);
  if(a->type==0 && b->type==0 && !a->classic && ((a->order<0)!=(b->order<0)))
    return a->order<0? 1:-1;
  /* ROOM uses a global instance list for creation/event order and a separate element list
   * for every instance layer. Authored element order is the back-to-front tie-break inside that
   * layer. Keeping the two orders separate matters when a full-layer overlay and decorative peers
   * share one depth; using reverse creation order paints the overlay last. */
  if(a->type==0 && b->type==0 && !a->classic && a->order>=0 && a->order==b->order &&
     a->element_order>=0 && b->element_order>=0 && a->element_order!=b->element_order)
    return a->element_order<b->element_order? -1:1;
  /* Asset-layer sprites retain their authored sequence rather than the reverse instance-chain
   * ordering, so later elements composite over earlier elements in the same layer. */
  if(a->type==5 && b->type==5 && a->order>=0 && a->order==b->order)
    return a->seq<b->seq? -1 : (a->seq>b->seq?1:0);
  return a->seq>b->seq? -1 : (a->seq<b->seq?1:0);
}
/* Natural-run bottom-up merge sort for the draw list.
 *
 * The assembler above emits the list in a few stretches that are already
 * internally ordered — instances in slot order, tiles and layer records in
 * list order — and depth sorts descending, so most stretches arrive either
 * sorted or exactly reversed. Detecting maximal runs, reversing descending
 * ones, and merging pairwise sorts a real scene in a fraction of the
 * comparisons a general qsort spends, calls the comparator directly instead
 * of through a function pointer, and gives every platform the same ordering:
 * the C runtimes' qsort implementations call the comparator in different
 * sequences, which msvcrt paid for in both time and, where the comparator is
 * not transitive (see header note), in a different picture.
 *
 * Reversing a descending run cannot reorder equal elements because distinct
 * items never compare equal: every comparator path above ends in a seq
 * tie-break and seq is unique per list. `aux` needs room for n items and
 * `run_starts` for n+1 ints; both are caller-owned scratch. */
void gml_vm_draw_items_sort(GmlDrawItem *items, GmlDrawItem *aux, int *run_starts, int n){
  if(n<2) return;
  int nr=0, i=0;
  while(i<n){
    int start=i;
    if(i==n-1){ run_starts[nr++]=start; i++; break; }
    if(gml_vm_draw_item_cmp(&items[i],&items[i+1])>0){
      while(i<n-1 && gml_vm_draw_item_cmp(&items[i],&items[i+1])>0) i++;
      for(int lo=start,hi=i;lo<hi;lo++,hi--){ GmlDrawItem t=items[lo]; items[lo]=items[hi]; items[hi]=t; }
    }else{
      while(i<n-1 && gml_vm_draw_item_cmp(&items[i],&items[i+1])<=0) i++;
    }
    run_starts[nr++]=start;
    i++;
  }
  run_starts[nr]=n;
  while(nr>1){
    int w=0;
    for(int r=0;r+1<nr;r+=2){
      int lo=run_starts[r], mid=run_starts[r+1], hi=run_starts[r+2];
      if(gml_vm_draw_item_cmp(&items[mid-1],&items[mid])>0){
        memcpy(aux+lo,items+lo,(size_t)(mid-lo)*sizeof *items);
        int a=lo,b=mid,k=lo;
        while(a<mid && b<hi)
          items[k++]= gml_vm_draw_item_cmp(&aux[a],&items[b])<=0 ? aux[a++] : items[b++];
        while(a<mid) items[k++]=aux[a++];
      }
      run_starts[w++]=lo;
    }
    if(nr%2) run_starts[w++]=run_starts[nr-1];
    run_starts[w]=n;
    nr=w;
  }
}
static int rt_layer_has_background(GmlVM *vm, int layer_id){
  for(int i=0;i<vm->n_rte;i++){
    GmlRtElem *e=&vm->rte[i];
    if(e->used && e->type==1 && e->layer==layer_id) return 1;
  }
  return 0;
}
static GmlRtLayer *rt_layer_by_order(GmlVM *vm, int order){
  if(!vm || order<0) return NULL;
  for(int i=0;i<vm->n_rtl;i++) if(vm->rtl[i].used && vm->rtl[i].order==order) return &vm->rtl[i];
  return NULL;
}
/* An instance remains active when its GMS2 instance layer is hidden: Step and collision events
 * still run, but draw-stage events and the automatic sprite draw are suppressed by the layer.
 * The instance's own `visible` field is independent and cannot represent this state. */
static int instance_draw_layer_visible(GmlVM *vm, const GmlInstance *in){
  if(!vm || !in || in->draw_layer_order<0) return 1;
  GmlRtLayer *layer=rt_layer_by_order(vm,in->draw_layer_order);
  return !layer || layer->visible;
}
static void gml_run_layer_script(GmlVM *vm, int ci){
  if(!vm || !vm->win || ci<0 || ci>=vm->win->n_code) return;
  GmlInstance layer_scratch;
  int pet=vm->event_type, pen=vm->event_number;
  memset(&layer_scratch,0,sizeof layer_scratch);
  layer_scratch.active=1; layer_scratch.obj=-1; layer_scratch.id=0;
  layer_scratch.image_xscale=layer_scratch.image_yscale=1; layer_scratch.image_alpha=1;
  layer_scratch.sprite_index=-1; layer_scratch.mask_index=-1; layer_scratch.path_index=-1;
  for(int a2=0;a2<GML_ALARMS;a2++) layer_scratch.alarm[a2]=-1;
  vm->event_type=8; vm->event_number=0;
  GmlVal _r=gml_vm_run_code(vm,ci,&layer_scratch,NULL,NULL,0);
  if(_r.t==V_STR && _r.d!=0) free((char*)_r.s);
  gml_varmap_free_ex(&layer_scratch.vars,0);
  memset(&layer_scratch.vars,0,sizeof layer_scratch.vars);
  vm->event_type=pet; vm->event_number=pen;
}
static int *vm_draw_order_scratch(GmlVM *vm, int need){
  if(!vm || need<=0) return NULL;
  if(need>vm->draw_ord_cap){
    int nc=vm->draw_ord_cap?vm->draw_ord_cap:64;
    while(nc<need) nc*=2;
    int *p=realloc(vm->draw_ord,(size_t)nc*sizeof(int));
    if(!p) return NULL;
    vm->draw_ord=p;
    vm->draw_ord_cap=nc;
  }
  return vm->draw_ord;
}
/* GMS2 runtime-layer draw records. File scope so the per-frame scratch buffers below can persist
 * across frames (reused, grown by doubling) instead of malloc/free + realloc(n+1) every frame. */
struct LayBg { int sprite,subimg; int th,tv,stretch,order,native; double x,y,xs,ys; uint32_t blend; double alpha; double depth; };
struct LayTile { int sprite; int sx,sy,w,h,order; double x,y,xs,ys; uint32_t blend; double alpha; double depth; };
struct LaySprite { int sprite, subimg,order; double x,y,xs,ys,angle; uint32_t blend; double alpha; double depth; };
struct LayEffect { GmlLayerFilter effect; int order; double depth; };
struct LayAttachedFilter { GmlLayerFilter effect; int order; };
struct ClassicBg { int def,th,tv,stretch; double x,y; uint32_t blend; double alpha,depth; };
static void draw_event_hook(GmlVM *vm, GmlInstance *in, const char *suffix, int begin){
  if(vm && vm->draw_event_hook) vm->draw_event_hook(vm,in,suffix,begin,vm->draw_event_hook_user);
}
/* Persistent per-frame draw scratch (gml_vm_draw is single-threaded, once per frame). Reset counts
 * to 0 each frame; the allocations survive so a steady room does zero malloc/free in its draw. */
typedef struct GmlDrawScratch {
  struct LayBg *layer_background; int layer_background_capacity;
  struct LayTile *layer_tile; int layer_tile_capacity;
  struct LaySprite *layer_sprite; int layer_sprite_capacity;
  struct LayEffect *layer_effect; int layer_effect_capacity;
  struct LayAttachedFilter *layer_attached_filter; int layer_attached_filter_capacity;
  GmlDrawItem *item; int item_capacity;
  GmlDrawItem *sort_aux; int sort_aux_capacity;
  int *sort_runs; int sort_runs_capacity;
  GmlDrawTile *tile; double *tile_depth; int tile_capacity;
} GmlDrawScratch;
static GmlDrawScratch *draw_scratch_get(GmlVM *vm){
  if(!vm) return NULL;
  if(!vm->draw_scratch) vm->draw_scratch=calloc(1,sizeof(*vm->draw_scratch));
  return vm->draw_scratch;
}
static void draw_scratch_destroy(GmlDrawScratch *scratch){
  if(!scratch) return;
  free(scratch->layer_background); free(scratch->layer_tile); free(scratch->layer_sprite);
  free(scratch->layer_effect); free(scratch->layer_attached_filter); free(scratch->item);
  free(scratch->sort_aux); free(scratch->sort_runs);
  free(scratch->tile); free(scratch->tile_depth); free(scratch);
}
void gml_vm_frame_cleanup(GmlVM *vm){
  if(!vm) return;
  draw_scratch_destroy(vm->draw_scratch);
  vm->draw_scratch=NULL;
}
/* grow *pp (element size esz) to hold at least `need` elements, doubling capacity. Returns 1 on ok. */
static int dl_grow(void **pp, int *cap, int need, size_t esz){
  if(need<=*cap) return 1;
  int nc = *cap>0 ? *cap : 64;
  while(nc<need) nc*=2;
  void *p=realloc(*pp,(size_t)nc*esz);
  if(!p) return 0;
  *pp=p; *cap=nc; return 1;
}

static const char *layer_effect_string(const GmlWin *w, uint32_t ptr){
  if(!w || !ptr || ptr>=w->size) return "";
  const char *s=(const char*)w->data+ptr;
  return memchr(s,0,w->size-ptr)?s:"";
}
/* Modern EMBI records associate the sampler resource name used by an effect definition with a
 * TPAG record. Resolve that indirection from the user-supplied package instead of embedding any
 * seed image in the core. */
static uint32_t layer_effect_sampler_tpag(const GmlWin *w, const char *name){
  const GmlChunk *c=gml_chunk(w,"EMBI");
  if(!c || !name || !*name || c->size<8) return 0;
  const uint8_t *d=w->data;
  uint32_t version=gml_vm_read_u32_le(d,c->off), count=gml_vm_read_u32_le(d,c->off+4);
  if(version!=1 || count>4096 || 8u+(uint64_t)count*8u>c->size) return 0;
  for(uint32_t i=0;i<count;i++){
    uint32_t p=c->off+8+i*8, np=gml_vm_read_u32_le(d,p), tp=gml_vm_read_u32_le(d,p+4);
    if(!strcmp(layer_effect_string(w,np),name)) return tp;
  }
  return 0;
}
static uint32_t layer_effect_colour(const char *value, uint32_t fallback){
  if(!value || value[0]!='#') return fallback;
  char *end=NULL;
  unsigned long colour=strtoul(value+1,&end,16);
  if(!(end && *end==0 && end==value+9)) return fallback;
  /* Effect JSON/ROOM colours are serialized as AABBGGRR. The software renderer stores ARGB. */
  uint32_t abgr=(uint32_t)colour;
  return (abgr&0xFF00FF00u)|((abgr&0x00FF0000u)>>16)|((abgr&0x000000FFu)<<16);
}
/* Decode the standard effect-layer descriptor stored inline in extended ROOM records. These are
 * engine filter identifiers/properties, so every package using the stock filters follows the same
 * path; unknown/custom filters remain a conservative no-op. */
static int gml_room_layer_effect(GmlVM *vm, uint32_t lp, GmlLayerFilter *out){
  memset(out,0,sizeof(*out));
  out->sampler_sprite=-1;
  if(!vm || !vm->win || gml_room_layer_data_off(vm)!=48 || lp+48>vm->win->size) return 0;
  const uint8_t *d=vm->win->data;
  if(!gml_vm_read_u32_le(d,lp+36)) return 0;
  const char *type=layer_effect_string(vm->win,gml_vm_read_u32_le(d,lp+40));
  if(!strcmp(type,"_filter_rgbnoise")){ out->kind=GML_LAYER_FILTER_RGB_NOISE; out->u.noise.colour=0xFFFFFFFFu; }
  else if(!strcmp(type,"_filter_tintfilter")){ out->kind=GML_LAYER_FILTER_TINT; out->u.tint.colour=0xFFFFFFFFu; }
  else if(!strcmp(type,"_filter_clouds")){
    out->kind=GML_LAYER_FILTER_CLOUDS;
    out->u.clouds.light_colour=out->u.clouds.shade_colour=0xFFFFFFFFu;
  }
  else if(!strcmp(type,"_effect_glow")){
    /* Attached effects receive only the pixels authored on their own layer. Full-screen
     * effects use an explicit layer element when they need deeper-layer coverage. */
    out->kind=GML_LAYER_FILTER_GLOW; out->u.glow.alpha=1;
  }
  else if(!strcmp(type,"_filter_underwater")){
    out->kind=GML_LAYER_FILTER_UNDERWATER;
    out->u.underwater.glint_colour=out->u.underwater.tint_colour=0xFFFFFFFFu;
    out->u.underwater.add_colour=0xFF000000u;
  }
  else if(!strcmp(type,"_filter_zoom_blur")) out->kind=GML_LAYER_FILTER_ZOOM_BLUR;
  else if(!strcmp(type,"_filter_large_blur")) out->kind=GML_LAYER_FILTER_LARGE_BLUR;
  else if(!strcmp(type,"_filter_boxes")) out->kind=GML_LAYER_FILTER_BOXES;
  else if(!strcmp(type,"_filter_colourise")){
    out->kind=GML_LAYER_FILTER_COLOURISE; out->u.colourise.tint_colour=0xFFFFFFFFu;
  }
  else return 0;
  uint32_t count=gml_vm_read_u32_le(d,lp+44);
  if(count>64 || lp+48+(uint64_t)count*12u>vm->win->size) return 0;
  const char *sampler="";
  int velocity=0,shape=0,shade_offset=0,distort1_scale=0,distort2_scale=0;
  int zoom_centre=0,box_size=0,box_rotation=0;
  for(uint32_t i=0;i<count;i++){
    uint32_t p=lp+48+i*12;
    const char *name=layer_effect_string(vm->win,gml_vm_read_u32_le(d,p+4));
    const char *value=layer_effect_string(vm->win,gml_vm_read_u32_le(d,p+8));
    double number=strtod(value,NULL);
    if(out->kind==GML_LAYER_FILTER_RGB_NOISE){
      if(strstr(name,"Intensity")) out->u.noise.intensity=number;
      else if(strstr(name,"Animation")) out->u.noise.animation=number;
      else if(strstr(name,"Colour")||strstr(name,"Color")) out->u.noise.colour=layer_effect_colour(value,out->u.noise.colour);
      else if((int32_t)gml_vm_read_u32_le(d,p)==2 || strstr(name,"Texture")) sampler=value;
    } else if(out->kind==GML_LAYER_FILTER_TINT){
      if(strstr(name,"TintCol")) out->u.tint.colour=layer_effect_colour(value,out->u.tint.colour);
    } else if(out->kind==GML_LAYER_FILTER_CLOUDS){
      if(!strcmp(name,"g_CloudScale")) out->u.clouds.scale=number;
      else if(!strcmp(name,"g_CloudVelocity") && velocity<2) out->u.clouds.velocity[velocity++]=number;
      else if(!strcmp(name,"g_CloudTurbulence")) out->u.clouds.turbulence=number;
      else if(!strcmp(name,"g_CloudLevel")) out->u.clouds.level=number;
      else if(!strcmp(name,"g_CloudWaves")) out->u.clouds.waves=number;
      else if(!strcmp(name,"g_CloudShape") && shape<2) out->u.clouds.shape[shape++]=number;
      else if(!strcmp(name,"g_CloudDensity")) out->u.clouds.density=number;
      else if(!strcmp(name,"g_CloudFade")) out->u.clouds.fade=number;
      else if(!strcmp(name,"g_CloudColour1")) out->u.clouds.light_colour=layer_effect_colour(value,out->u.clouds.light_colour);
      else if(!strcmp(name,"g_CloudColour2")) out->u.clouds.shade_colour=layer_effect_colour(value,out->u.clouds.shade_colour);
      else if(!strcmp(name,"g_CloudShadeOffset") && shade_offset<2) out->u.clouds.shade_offset[shade_offset++]=number;
      else if(!strcmp(name,"g_CloudShadeFade")) out->u.clouds.shade_fade=number;
      else if(strstr(name,"Texture")) sampler=value;
    } else if(out->kind==GML_LAYER_FILTER_GLOW){
      if(!strcmp(name,"g_GlowRadius")) out->u.glow.radius=number;
      else if(!strcmp(name,"g_GlowQuality")) out->u.glow.quality=number;
      else if(!strcmp(name,"g_GlowIntensity")) out->u.glow.intensity=number;
      else if(!strcmp(name,"g_GlowGamma")) out->u.glow.gamma=number;
      else if(!strcmp(name,"g_GlowAlpha")) out->u.glow.alpha=number;
    } else if(out->kind==GML_LAYER_FILTER_UNDERWATER){
      if(!strcmp(name,"g_Distort1Speed")) out->u.underwater.speed[0]=number;
      else if(!strcmp(name,"g_Distort2Speed")) out->u.underwater.speed[1]=number;
      else if(!strcmp(name,"g_Distort1Scale") && distort1_scale<2) out->u.underwater.scale[0][distort1_scale++]=number;
      else if(!strcmp(name,"g_Distort2Scale") && distort2_scale<2) out->u.underwater.scale[1][distort2_scale++]=number;
      else if(!strcmp(name,"g_Distort1Amount")) out->u.underwater.amount[0]=number;
      else if(!strcmp(name,"g_Distort2Amount")) out->u.underwater.amount[1]=number;
      else if(!strcmp(name,"g_ChromaSpreadAmount")) out->u.underwater.chroma=number;
      else if(!strcmp(name,"g_CamOffsetScale")) out->u.underwater.camera_scale=number;
      else if(!strcmp(name,"g_GlintCol")) out->u.underwater.glint_colour=layer_effect_colour(value,out->u.underwater.glint_colour);
      else if(!strcmp(name,"g_TintCol")) out->u.underwater.tint_colour=layer_effect_colour(value,out->u.underwater.tint_colour);
      else if(!strcmp(name,"g_AddCol")) out->u.underwater.add_colour=layer_effect_colour(value,out->u.underwater.add_colour);
      else if(strstr(name,"Texture")) sampler=value;
    } else if(out->kind==GML_LAYER_FILTER_ZOOM_BLUR){
      if(!strcmp(name,"g_ZoomBlurCenter") && zoom_centre<2) out->u.zoom_blur.centre[zoom_centre++]=number;
      else if(!strcmp(name,"g_ZoomBlurIntensity")) out->u.zoom_blur.intensity=number;
      else if(!strcmp(name,"g_ZoomBlurFocusRadius")) out->u.zoom_blur.focus_radius=number;
      else if(strstr(name,"Texture")) sampler=value;
    } else if(out->kind==GML_LAYER_FILTER_LARGE_BLUR){
      if(!strcmp(name,"g_Radius")) out->u.large_blur.radius=number;
      else if(strstr(name,"Texture")) sampler=value;
    } else if(out->kind==GML_LAYER_FILTER_BOXES){
      if(!strcmp(name,"g_BoxesScale")) out->u.boxes.scale=number;
      else if(!strcmp(name,"g_BoxesSize") && box_size<2) out->u.boxes.size[box_size++]=number;
      else if(!strcmp(name,"g_BoxesDisplacement")) out->u.boxes.displacement=number;
      else if(!strcmp(name,"g_BoxesSpeed")) out->u.boxes.speed=number;
      else if(!strcmp(name,"g_BoxesAngle")) out->u.boxes.angle=number;
      else if(!strcmp(name,"g_BoxesRotation") && box_rotation<2) out->u.boxes.rotation[box_rotation++]=number;
      else if(!strcmp(name,"g_BoxesRoundness")) out->u.boxes.roundness=number;
      else if(!strcmp(name,"g_BoxesColourSpeed")) out->u.boxes.colour_speed=number;
      else if(!strcmp(name,"g_BoxesColours")) out->u.boxes.colours=number;
      else if(!strcmp(name,"g_BoxesSharpness")) out->u.boxes.sharpness=number;
      else if(strstr(name,"Palette")||strstr(name,"Texture")) sampler=value;
    } else if(out->kind==GML_LAYER_FILTER_COLOURISE){
      if(!strcmp(name,"g_Intensity")) out->u.colourise.intensity=number;
      else if(!strcmp(name,"g_TintCol")) out->u.colourise.tint_colour=layer_effect_colour(value,out->u.colourise.tint_colour);
    }
  }
  GmlRender *render=(GmlRender*)vm->render;
  out->sampler_sprite=gml_render_named_sprite(render,sampler);
  if(out->kind==GML_LAYER_FILTER_RGB_NOISE){
    out->u.noise.sampler_tpag_ptr=gml_render_named_tpag_ptr(render,sampler);
    if(!out->u.noise.sampler_tpag_ptr) out->u.noise.sampler_tpag_ptr=layer_effect_sampler_tpag(vm->win,sampler);
    if(!out->u.noise.sampler_tpag_ptr || out->u.noise.intensity<=0.0) return 0;
  }
  if((out->kind==GML_LAYER_FILTER_CLOUDS || out->kind==GML_LAYER_FILTER_UNDERWATER ||
      out->kind==GML_LAYER_FILTER_ZOOM_BLUR || out->kind==GML_LAYER_FILTER_LARGE_BLUR ||
      out->kind==GML_LAYER_FILTER_BOXES) && out->sampler_sprite<0) return 0;
  return 1;
}
/* apply the mutation for a tile at original `depth`: returns 0 to drop the tile (deleted/hidden),
 * else 1 and writes the effective depth + position offset. */
int gml_vm_frame_apply_tile_mutation(GmlVM *vm, int depth, int *eff_depth,
                                     double *ox, double *oy){
  *eff_depth=depth; *ox=0; *oy=0;
  for(int i=0;i<vm->n_tile_mut;i++) if(vm->tile_mut[i].depth==depth){
    if(vm->tile_mut[i].flags&
       (GML_VM_TILE_MUT_DELETED|GML_VM_TILE_MUT_HIDDEN)) return 0;
    if(vm->tile_mut[i].has_remap) *eff_depth=vm->tile_mut[i].remap;
    *ox=vm->tile_mut[i].dx; *oy=vm->tile_mut[i].dy; return 1;
  }
  return 1;
}
void gml_vm_draw(GmlVM *vm){
  GmlRender *R=(GmlRender*)vm->render; if(!R) return;
  gml_render_set_frame(R,vm->frame);
  GmlDrawScratch *scratch=draw_scratch_get(vm); if(!scratch) return;
  int n=vm->inst_count;
  /* gather this room's tiles (pointer-list of records: x,y,def,srcx,srcy,w,h,depth,...).
   * Persistent scratch: draw_tile_add grows g_dl_tiles/g_dl_tdepth by doubling and they survive
   * across frames, so a many-tile room does no per-frame tile malloc. */
  GmlDrawTile *tiles=scratch->tile; int nt=0, tcap=scratch->tile_capacity;
  double *tdepth=scratch->tile_depth;
  GmlRoom rm;
  if(gml_vm_room_get(vm,vm->room_index,&rm)==0 && rm.tile_ptr){
    const uint8_t *d=vm->win->data; uint32_t tc=gml_vm_read_u32_le(d,rm.tile_ptr);
    if(tc>0 && tc<100000){
      for(uint32_t i=0;i<tc;i++){ uint32_t p=gml_vm_read_u32_le(d,rm.tile_ptr+4+i*4);
	int tx=(int)gml_vm_read_u32_le(d,p), ty=(int)gml_vm_read_u32_le(d,p+4); int tdep=(int32_t)gml_vm_read_u32_le(d,p+28);
	if(vm->n_tile_mut){ int ed; double ox,oy;
          if(!gml_vm_frame_apply_tile_mutation(vm,tdep,&ed,&ox,&oy)) continue;   /* deleted layer: drop */
          tdep=ed; tx+=(int)ox; ty+=(int)oy; }
        { int drop=0;
          for(int da=0;da<vm->n_tile_del_at;da++)
            if(vm->tile_del_at[da].depth==tdep && vm->tile_del_at[da].x==tx && vm->tile_del_at[da].y==ty){ drop=1; break; }
          if(drop) continue; }
	GmlDrawTile dt;
	dt.x=tx; dt.y=ty; dt.xs=1; dt.ys=1; dt.def=(int)gml_vm_read_u32_le(d,p+8);
	dt.sx=(int)gml_vm_read_u32_le(d,p+12); dt.sy=(int)gml_vm_read_u32_le(d,p+16);
	dt.w=(int)gml_vm_read_u32_le(d,p+20); dt.h=(int)gml_vm_read_u32_le(d,p+24);
	dt.mirror=dt.flip=dt.rotate=0;
	draw_tile_add(&tiles,&tdepth,&nt,&tcap,dt,tdep,-1); } }
  }
  /* ---- GMS2 room layers (ROOM record +88): Background (type 1) and Asset-tile (type 3)
   * layers hold the scenery that legacy background/tile sections carry in bc14-16 (those are
   * empty in GMS2 data — without layers, menu/level scenery renders black). Instance layers
   * (type 2) duplicate the legacy instance list already handled. Type-specific data begins at
   * +36, or +48 when the extended per-layer effect fields are present — detected once per file
   * by validating a background layer's sprite id under each layout. */
  struct LayBg *lbg=scratch->layer_background; int nlb=0;
  struct LayTile *ltl=scratch->layer_tile; int nlt=0;
  struct LaySprite *lsp=scratch->layer_sprite; int nls=0;
  struct LayEffect *lfx=scratch->layer_effect; int nlf=0;
  struct LayAttachedFilter *laf=scratch->layer_attached_filter; int naf=0;
  struct ClassicBg cbg[9]; int ncb=0;
  if(rm.draw_bg){
    uint32_t room_color=gml_vm_room_background_argb(vm);
    cbg[ncb].def=-1; cbg[ncb].th=cbg[ncb].tv=cbg[ncb].stretch=0;
    cbg[ncb].x=cbg[ncb].y=0; cbg[ncb].blend=room_color&0xFFFFFFu;
    cbg[ncb].alpha=((room_color>>24)&0xFF)/255.0; cbg[ncb].depth=1.1e300; ncb++;
  }
  for(int i=0;i<8;i++){
    if(gml_vm_global_array_number(vm,"background_visible",i)<0.5) continue;
    int def=(int)gml_vm_global_array_number(vm,"background_index",i);
    if(def<0 || !gml_render_background_metrics(R,def,NULL)) continue;
    struct ClassicBg *bg=&cbg[ncb++];
    bg->def=def;
    bg->x=gml_vm_global_array_number(vm,"background_x",i); bg->y=gml_vm_global_array_number(vm,"background_y",i);
    bg->th=gml_vm_global_array_number(vm,"background_htiled",i)>=0.5;
    bg->tv=gml_vm_global_array_number(vm,"background_vtiled",i)>=0.5;
    bg->stretch=gml_vm_global_array_number(vm,"background_stretch",i)>=0.5;
    bg->blend=(uint32_t)gml_vm_global_array_number(vm,"background_blend",i);
    bg->alpha=gml_vm_global_array_number(vm,"background_alpha",i);
    bg->depth=gml_vm_global_array_number(vm,"background_foreground",i)>=0.5 ? -1.0e300 : 1.0e300;
  }
  {
    const uint8_t *d=vm->win->data;
    uint32_t lcnt=0;
    uint32_t lay=gml_vm_rooms_layer_list(vm,vm->room_index,&lcnt);
    if(lcnt>0 && lcnt<512){
      long fin = vm->frame - vm->room_enter_frame; if(fin<0) fin=0;
      for(uint32_t i=0;i<lcnt;i++){ uint32_t lp=gml_vm_read_u32_le(d,lay+4+i*4);
        if(!lp || lp+44>vm->win->size) continue;
        uint32_t ltype=gml_vm_read_u32_le(d,lp+8); double ldep=(double)(int32_t)gml_vm_read_u32_le(d,lp+12);
        double lx=gml_vm_read_f32_le(d,lp+16), ly=gml_vm_read_f32_le(d,lp+20), lhs=gml_vm_read_f32_le(d,lp+24), lvs=gml_vm_read_f32_le(d,lp+28);
        /* Runtime layer control: if the game moved this layer (layer_x/layer_hspeed by name), use its
         * live accumulated position; otherwise the exact room-def scroll model (byte-identical for
         * games that never script their layers). */
        uint32_t lnp=gml_vm_read_u32_le(d,lp+0);
        GmlRtLayer *rl=(lnp && lnp<vm->win->size)? gml_rt_layer_find_by_name(vm,(const char*)(d+lnp)):NULL;
        if(rl) ldep=rl->depth;
        int lorder = rl ? rl->order : (int)i;
        int ltouch = rl && rl->touched;
        double lox = ltouch ? rl->x : lx+lhs*fin;   /* background layer origin (scrolls) */
        double loy = ltouch ? rl->y : ly+lvs*fin;
        double ltx = ltouch ? rl->x : lx;           /* tile layer origin (room-def raw, unchanged) */
        double lty = ltouch ? rl->y : ly;
        if(rl ? !rl->visible : !gml_vm_read_u32_le(d,lp+32)) continue;
        GmlLayerFilter layer_filter;
        int has_layer_filter=gml_room_layer_effect(vm,lp,&layer_filter);
        if(has_layer_filter && anygm_host_development_setting(vm->host,"GML_LOG_LAYER_EFFECT") &&
           (vm->frame<4 || (vm->frame%60)==0))
          anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[layer-effect] f%ld order=%d type=%u depth=%.0f kind=%d sampler-sprite=%d\n",
                  vm->frame,lorder,ltype,ldep,layer_filter.kind,layer_filter.sampler_sprite);
        /* An effect attached to an ordinary room layer transforms just that layer. The sorted draw
         * loop below isolates the corresponding runtime order before applying the software filter.
         * Type-6 records remain standalone effect draw items. */
        if(has_layer_filter && ltype!=6){
          if(dl_grow((void**)&scratch->layer_attached_filter,
                     &scratch->layer_attached_filter_capacity,naf+1,sizeof(*laf))){
            laf=scratch->layer_attached_filter;
            laf[naf].effect=layer_filter; laf[naf].order=lorder; naf++;
          }
        }
        if(ltype==1){
          if(rl && rt_layer_has_background(vm,rl->id)) continue;
          uint32_t b=gml_room_layer_type_off(vm,lp);
          if(b+28>vm->win->size) continue;
          if(!gml_vm_read_u32_le(d,b)) continue;     /* background not visible */
          int spr=(int32_t)gml_vm_read_u32_le(d,b+8);
          uint32_t col=gml_vm_read_u32_le(d,b+24);
          /* A sprite-less background layer fills the screen with its nontransparent color. */
          if(spr<0 && !(col>>24)) continue;
          if(!dl_grow((void**)&scratch->layer_background,
                      &scratch->layer_background_capacity,nlb+1,sizeof(*lbg))) continue;
          lbg=scratch->layer_background;
          lbg[nlb].sprite=spr; lbg[nlb].subimg=0; lbg[nlb].native=1;
          lbg[nlb].th=(int)gml_vm_read_u32_le(d,b+12); lbg[nlb].tv=(int)gml_vm_read_u32_le(d,b+16);
          lbg[nlb].stretch=(int)gml_vm_read_u32_le(d,b+20);
          lbg[nlb].xs=lbg[nlb].ys=1;
          lbg[nlb].blend=col&0xFFFFFF; lbg[nlb].alpha=((col>>24)&0xFF)/255.0;
          lbg[nlb].x=lox; lbg[nlb].y=loy; lbg[nlb].depth=ldep; lbg[nlb].order=lorder;
          nlb++;
        } else if(ltype==3){
          uint32_t tb3=gml_room_layer_type_off(vm,lp);
          uint32_t tl=(tb3+4<=vm->win->size)?gml_vm_read_u32_le(d,tb3):0;
          uint32_t tcnt=(tl && tl+4<vm->win->size)?gml_vm_read_u32_le(d,tl):0;
          if(tcnt==0 || tcnt>100000) continue;
          for(uint32_t k2=0;k2<tcnt;k2++){ uint32_t tp=gml_vm_read_u32_le(d,tl+4+k2*4);
            if(!tp || tp+48>vm->win->size) continue;
            if(!dl_grow((void**)&scratch->layer_tile,
                        &scratch->layer_tile_capacity,nlt+1,sizeof(*ltl))) continue;
            ltl=scratch->layer_tile;
            ltl[nlt].x=ltx+(int32_t)gml_vm_read_u32_le(d,tp); ltl[nlt].y=lty+(int32_t)gml_vm_read_u32_le(d,tp+4);
            ltl[nlt].sprite=(int32_t)gml_vm_read_u32_le(d,tp+8);
            ltl[nlt].sx=(int32_t)gml_vm_read_u32_le(d,tp+12); ltl[nlt].sy=(int32_t)gml_vm_read_u32_le(d,tp+16);
            ltl[nlt].w=(int32_t)gml_vm_read_u32_le(d,tp+20); ltl[nlt].h=(int32_t)gml_vm_read_u32_le(d,tp+24);
            ltl[nlt].depth=ldep;
            ltl[nlt].order=lorder;
            ltl[nlt].xs=gml_vm_read_f32_le(d,tp+36); ltl[nlt].ys=gml_vm_read_f32_le(d,tp+40);
            uint32_t col=gml_vm_read_u32_le(d,tp+44);
            ltl[nlt].blend=col&0xFFFFFF; ltl[nlt].alpha=((col>>24)&0xFF)/255.0;
            nlt++; }
        } else if(ltype==6){
          if(!has_layer_filter) continue;
          if(!dl_grow((void**)&scratch->layer_effect,
                      &scratch->layer_effect_capacity,nlf+1,sizeof(*lfx))) continue;
          lfx=scratch->layer_effect;
          lfx[nlf].effect=layer_filter; lfx[nlf].depth=ldep; lfx[nlf].order=lorder;
          nlf++;
        }
      }
    }
  }
  /* GMS2 tilemap layers (type 4): the same grid used by tilemap_get_* for collision is also a
   * visual layer. Without drawing these, games still collide with floors but the floors are
   * invisible. Expand only the camera-visible cells into the existing background-tile draw path. */
  /* The grid is authored in world coordinates, so the visible rectangle has to be asked for in
   * the same units. Target metrics are target pixels; dividing those by a cell size selects a
   * region scaled by whatever the view-to-target transform is, which is off-screen entirely once
   * the two differ. */
  double view_x=0,view_y=0,view_w=0,view_h=0;
  if(!gml_render_world_view(R,&view_x,&view_y,&view_w,&view_h)){
    view_x=view_y=0; view_w=view_h=0;
  }
  for(int mi=0; mi<vm->n_tilemaps; mi++){
    GmlTileMap *tm=&vm->tilemaps[mi];
    double tmx,tmy,tmdepth; int tmvis;
    gml_tilemap_effective(vm,tm,&tmx,&tmy,&tmdepth,&tmvis);
    if(!tm->used || !tmvis || !tm->tiles || tm->tileset<0 || tm->tw<=0 || tm->th<=0) continue;
    GmlRenderBackgroundMetrics background;
    if(!gml_render_background_metrics(R,tm->tileset,&background) ||
       background.texture_page<0) continue;
    int tw=background.tile_width>0?background.tile_width:tm->tw;
    int th=background.tile_height>0?background.tile_height:tm->th;
    int bx=background.tile_border_x,by=background.tile_border_y;
    int pitch_x=tw+2*bx+background.tile_separation_x;
    int pitch_y=th+2*by+background.tile_separation_y;
    int srcw=background.logical_width,srch=background.logical_height;
    int per_row=background.tile_columns>0?background.tile_columns:(pitch_x>0?srcw/pitch_x:0);
    if(srcw<=0 || srch<=0 || tw<=0 || th<=0 || pitch_x<=0 || pitch_y<=0 || per_row<=0) continue;
    double speed=gml_room_speed(vm);
    double elapsed_seconds=speed>0.0?vm->frame/speed:0.0;
    int animation_frame=
      gml_render_background_tile_animation_frame(R,tm->tileset,elapsed_seconds);
    int cx0=(int)floor((view_x-tmx)/tm->tw)-1;
    int cy0=(int)floor((view_y-tmy)/tm->th)-1;
    int cx1=(int)ceil((view_x+view_w-tmx)/tm->tw)+1;
    int cy1=(int)ceil((view_y+view_h-tmy)/tm->th)+1;
    if(cx0<0) cx0=0;
    if(cy0<0) cy0=0;
    if(cx1>tm->cols) cx1=tm->cols;
    if(cy1>tm->rows) cy1=tm->rows;
    for(int cy=cy0; cy<cy1; cy++) for(int cx=cx0; cx<cx1; cx++){
      uint32_t datum=gml_vm_read_u32_le(tm->tiles,(uint32_t)((size_t)cy*tm->cols+cx)*4);
      int idx=(int)(datum & 0x7FFFFu);
      if(idx<=0) continue;                  /* GM encodes 0 as empty */
      int src_idx=
        gml_render_background_tile_source_index(R,tm->tileset,idx,animation_frame);
      if(src_idx<0) continue;
      int sx=(src_idx%per_row)*pitch_x + bx, sy=(src_idx/per_row)*pitch_y + by;
      if(sx>=srcw || sy>=srch) continue;
      int w=tw, h=th;
      if(sx+w>srcw) w=srcw-sx;
      if(sy+h>srch) h=srch-sy;
      if(w<=0 || h<=0) continue;
      GmlDrawTile dt;
      dt.x=tmx + cx*tm->tw; dt.y=tmy + cy*tm->th; dt.xs=1; dt.ys=1;
      dt.mirror=(int)((datum>>28)&1);
      dt.flip=(int)((datum>>29)&1);
      dt.rotate=(int)((datum>>30)&1);
      dt.def=tm->tileset; dt.sx=sx; dt.sy=sy; dt.w=w; dt.h=h;
      draw_tile_add(&tiles,&tdepth,&nt,&tcap,dt,tmdepth,tm->order);
    }
  }
  /* runtime layer elements (layer_tile_create / layer_background_create): converted GM8 games
   * paint terrain through the tile_add compat script, which lands here. Reuse the LayTile/LayBg
   * records so the unified draw list needs no new item types. */
  for(int i=0;i<vm->n_rte;i++){
    GmlRtElem *e=&vm->rte[i];
    if(!e->used || !e->visible || (e->sprite<0 && e->type!=1)) continue;
    GmlRtLayer *l=gml_rt_layer_find(vm,e->layer);
    if(!l || !l->visible) continue;
    long fin2 = vm->frame - vm->room_enter_frame; if(fin2<0) fin2=0;
    double lx = l->touched ? l->x : l->x+l->hs*fin2;
    double ly = l->touched ? l->y : l->y+l->vs*fin2;
    double ldepth=l->depth;
    if(e->type==7){
      /* Legacy tile_layer_* operations address every tile at an authored depth, including
       * tiles created at runtime.  Static ROOM tiles already pass through gml_vm_frame_apply_tile_mutation above;
       * apply the same visibility/depth/shift transform to layer-backed dynamic tiles. */
      if(vm->n_tile_mut){ int ed; double ox,oy;
        if(!gml_vm_frame_apply_tile_mutation(vm,(int)l->depth,&ed,&ox,&oy)) continue;
        ldepth=ed; lx+=ox; ly+=oy;
      }
      if(!dl_grow((void**)&scratch->layer_tile,
                  &scratch->layer_tile_capacity,nlt+1,sizeof(*ltl))) continue;
      ltl=scratch->layer_tile;
      ltl[nlt].sprite=e->sprite; ltl[nlt].sx=e->sx; ltl[nlt].sy=e->sy; ltl[nlt].w=e->w; ltl[nlt].h=e->h;
      ltl[nlt].x=lx+e->x; ltl[nlt].y=ly+e->y; ltl[nlt].xs=e->xs; ltl[nlt].ys=e->ys;
      ltl[nlt].blend=e->blend; ltl[nlt].alpha=e->alpha; ltl[nlt].depth=ldepth; ltl[nlt].order=l->order;
      nlt++;
    } else if(e->type==1){
      if(!dl_grow((void**)&scratch->layer_background,
                  &scratch->layer_background_capacity,nlb+1,sizeof(*lbg))) continue;
      lbg=scratch->layer_background;
      lbg[nlb].sprite=e->sprite; lbg[nlb].subimg=(int)floor(e->image_index);
      lbg[nlb].native=anygm_policy_has_modern_function_values(vm->win) ||
                      room_element_animation_speed_type(vm,e,NULL);
      lbg[nlb].th=e->htiled; lbg[nlb].tv=e->vtiled; lbg[nlb].stretch=e->stretch;
      lbg[nlb].xs=e->xs; lbg[nlb].ys=e->ys;
      lbg[nlb].x=lx; lbg[nlb].y=ly; lbg[nlb].blend=e->blend; lbg[nlb].alpha=e->alpha; lbg[nlb].depth=l->depth; lbg[nlb].order=l->order;
      nlb++;
    } else if(e->type==3){
      if(!dl_grow((void**)&scratch->layer_sprite,
                  &scratch->layer_sprite_capacity,nls+1,sizeof(*lsp))) continue;
      lsp=scratch->layer_sprite;
      lsp[nls].sprite=e->sprite; lsp[nls].subimg=(int)e->image_index;
      lsp[nls].x=lx+e->x; lsp[nls].y=ly+e->y; lsp[nls].xs=e->xs; lsp[nls].ys=e->ys;
      lsp[nls].angle=e->image_angle; lsp[nls].blend=e->blend; lsp[nls].alpha=e->alpha; lsp[nls].depth=l->depth; lsp[nls].order=l->order;
      nls++;
    }
  }
  /* unified depth-sorted draw list of instances + tiles + GMS2 layers + auto-draw particle systems */
  int npart=0; while(gml_part_system_auto_draw_nth(vm->particles,npart,NULL,NULL)) npart++;
  int cap=n+nt+nlb+nlt+nls+nlf+ncb+npart;
  if(!dl_grow((void**)&scratch->item,&scratch->item_capacity,
              cap>0?cap:1,sizeof(GmlDrawItem))){
    scratch->tile=tiles; scratch->tile_depth=tdepth; scratch->tile_capacity=tcap; return;
  }
  GmlDrawItem *it=scratch->item; int m=0;
  int *inst_ord=vm_draw_order_scratch(vm,n>0?n:1); int inst_n=0;
  if(inst_ord){
    for(int i=0;i<n;i++) if(vm->inst[i].active && !vm->inst[i].marked) inst_ord[inst_n++]=i;
    if(inst_n>1) gml_vm_instances_sort_slots(vm,inst_ord,inst_n);
  }
  for(int k=0;k<inst_n;k++){ int i=inst_ord[k]; if(instance_draw_layer_visible(vm,&vm->inst[i])){
    it[m].depth=vm->inst[i].depth; it[m].type=0; it[m].idx=i; it[m].seq=m; it[m].order=vm->inst[i].draw_layer_order;
    it[m].element_order=vm->inst[i].draw_layer_element_order;
    it[m].classic=(vm->win&&anygm_policy_uses_classic_runtime(vm->win))?
      (vm->win->classic_executable_layout?2:1):0; it[m].obj=vm->inst[i].obj;
    it[m].placed=it[m].classic && vm->inst[i].room_placed; m++; } }
  for(int i=0;i<nt;i++){ it[m].depth=tdepth[i]; it[m].type=1; it[m].idx=i; it[m].seq=m; it[m].order=tiles[i].order; it[m].classic=0; it[m].obj=-1; m++; }
  for(int i=0;i<nlt;i++){ it[m].depth=ltl[i].depth; it[m].type=2; it[m].idx=i; it[m].seq=m; it[m].order=ltl[i].order; it[m].classic=0; it[m].obj=-1; m++; }
  for(int i=0;i<nlb;i++){ it[m].depth=lbg[i].depth; it[m].type=3; it[m].idx=i; it[m].seq=m; it[m].order=lbg[i].order; it[m].classic=0; it[m].obj=-1; m++; }
  for(int i=0;i<nls;i++){ it[m].depth=lsp[i].depth; it[m].type=5; it[m].idx=i; it[m].seq=m; it[m].order=lsp[i].order; it[m].classic=0; it[m].obj=-1; m++; }
  for(int i=0;i<nlf;i++){ it[m].depth=lfx[i].depth; it[m].type=7; it[m].idx=i; it[m].seq=m; it[m].order=lfx[i].order; it[m].classic=0; it[m].obj=-1; m++; }
  for(int i=0;i<ncb;i++){ it[m].depth=cbg[i].depth; it[m].type=6; it[m].idx=i; it[m].seq=m; it[m].order=-1; it[m].classic=0; it[m].obj=-1; m++; }
  for(int i=0;i<npart;i++){ int pid=0; double dep=0;
    if(gml_part_system_auto_draw_nth(vm->particles,i,&pid,&dep)){ it[m].depth=dep; it[m].type=4; it[m].idx=pid; it[m].seq=m; it[m].order=-1; it[m].classic=0; it[m].obj=-1; m++; } }
  if(dl_grow((void**)&scratch->sort_aux,&scratch->sort_aux_capacity,m,sizeof(GmlDrawItem)) &&
     dl_grow((void**)&scratch->sort_runs,&scratch->sort_runs_capacity,m+1,sizeof(int)))
    gml_vm_draw_items_sort(it,scratch->sort_aux,scratch->sort_runs,m);
  else
    qsort(it,m,sizeof(GmlDrawItem),gml_vm_draw_item_cmp);
  /* A frame number dumps every draw of that frame rather than the first one past it: a rewind
   * redraws a frame the forward run already drew, and the two dumps are only comparable when both
   * are emitted. */
  int inst_dump=0;
  { const char *li=anygm_host_development_setting(vm->host,"GML_LOG_INST");
    if(li && atoi(li)>0) inst_dump=(vm->frame==atoi(li));
    else if(li && !vm->diagnostics.instance_draw_dumped) inst_dump=1;
    if(!inst_dump) goto skip_instdump; }
  { vm->diagnostics.instance_draw_dumped=1;
    anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[draw] room=%d, %d instances + %d tiles, draw_events_off=%d (back->front):\n",
            vm->room_index,n,nt,vm->draw_events_off);
    for(int k=0;k<m && k<2000;k++){ if(it[k].type==1){ GmlDrawTile *t=&tiles[it[k].idx];
        anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"   TILE def=%d depth=%.0f @(%.0f,%.0f) %dx%d src=(%d,%d)",t->def,it[k].depth,t->x,t->y,t->w,t->h,t->sx,t->sy);
        GmlRenderBackgroundMetrics background;
        if(t->def>=0 && gml_render_background_metrics(R,t->def,&background) &&
           background.texture_page>=0)
          anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,
            " tpag=%d atlas=%d packed=(%d,%d %dx%d) trim=(%d,%d) logical=%dx%d",
            background.texture_page,background.atlas,background.packed_x,background.packed_y,
            background.packed_width,background.packed_height,background.trim_x,background.trim_y,
            background.declared_width,background.declared_height);
        anygm_host_logf(vm->host,ANYGM_LOG_DEBUG,"\n"); }
      else if(it[k].type==2){ struct LayTile *t=&ltl[it[k].idx];
        anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"   LTILE spr=%d depth=%.0f @(%.0f,%.0f) %dx%d\n",t->sprite,it[k].depth,t->x,t->y,t->w,t->h); }
      else if(it[k].type==3){ struct LayBg *b=&lbg[it[k].idx];
        anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"   LBG spr=%d sub=%d depth=%.0f @(%.3f,%.3f) scale=%.3f/%.3f tiled=%d/%d stretch=%d colour=%06x alpha=%.3f\n",
          b->sprite,b->subimg,it[k].depth,b->x,b->y,b->xs,b->ys,b->th,b->tv,b->stretch,b->blend&0xFFFFFFu,b->alpha); }
      else if(it[k].type==4){
        anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"   PARTICLES sys=%d depth=%.0f\n",it[k].idx,it[k].depth); }
      else if(it[k].type==5){ struct LaySprite *s=&lsp[it[k].idx];
        anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"   LSPR spr=%d depth=%.0f @(%.0f,%.0f) idx=%d ang=%.0f xs=%.1f ys=%.1f a=%.2f\n",
          s->sprite,it[k].depth,s->x,s->y,s->subimg,s->angle,s->xs,s->ys,s->alpha); }
      else if(it[k].type==6){ struct ClassicBg *b=&cbg[it[k].idx];
        anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"   CBG def=%d depth=%.0f @(%.0f,%.0f) tiled=%d/%d stretch=%d colour=%06x alpha=%.3f\n",
          b->def,it[k].depth,b->x,b->y,b->th,b->tv,b->stretch,b->blend&0xFFFFFFu,b->alpha); }
      else if(it[k].type==7){ struct LayEffect *f=&lfx[it[k].idx];
        anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"   LEFFECT kind=%d depth=%.0f sampler-sprite=%d\n",
          f->effect.kind,it[k].depth,f->effect.sampler_sprite); }
      else { GmlInstance *in=&vm->inst[it[k].idx];
        anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"   %-26s id=%u spr=%-4d vis=%.0f depth=%.0f ord=%d elem=%d @(%.0f,%.0f) ang=%.0f xs=%.1f ys=%.1f a=%.2f ii=%.4f is=%.3f\n",
          (in->obj>=0&&in->obj<vm->n_objects)?vm->objects[in->obj].name:"?",in->id,
          (int)in->sprite_index,in->visible,in->depth,in->draw_layer_order,in->draw_layer_element_order,
          in->x,in->y,in->image_angle,in->image_xscale,in->image_yscale,in->image_alpha,in->image_index,in->image_speed); } } }
  skip_instdump:
  int active_layer_order=-1;
  GmlRtLayer *active_layer=NULL;
  GmlLayerFilter *active_filter=NULL;
  int active_filter_started=0;
  double effect_time=vm->frame/gml_room_speed(vm);
  for(int k=0;k<m;k++){
    gml_d3_set_draw_depth((GmlRender*)vm->render,it[k].depth);
    if(it[k].order!=active_layer_order){
      if(active_layer) gml_run_layer_script(vm,active_layer->script_end);
      if(active_filter_started) gml_render_layer_filter_end(R,active_filter,effect_time);
      gml_render_shader_set_current(R,-1);
      active_layer_order=it[k].order;
      active_layer=rt_layer_by_order(vm,active_layer_order);
      active_filter=NULL; active_filter_started=0;
      for(int fi=0;fi<naf;fi++) if(laf[fi].order==active_layer_order){
        active_filter=&laf[fi].effect;
        active_filter_started=gml_render_layer_filter_begin(R,active_filter);
        break;
      }
      if(active_layer){
        int slot=(int)(active_layer-vm->rtl);
        double encoded=gml_global_arr(vm,"__gml_layer_shader",slot);
        if(encoded!=0) gml_render_shader_set_current(R,(int)encoded-1);
        gml_run_layer_script(vm,active_layer->script_begin);
      }
    }
    if(it[k].type==1){ GmlDrawTile *t=&tiles[it[k].idx];
      gml_draw_background_tile(R,t->def,t->sx,t->sy,t->w,t->h,
                               t->x,t->y,t->xs,t->ys,
                               t->mirror,t->flip,t->rotate,0xFFFFFF,1);
      continue; }
    if(it[k].type==2){ struct LayTile *t=&ltl[it[k].idx];
      if(vm->win && anygm_policy_uses_classic_runtime(vm->win))
        gml_draw_background_part_ext(R,t->sprite,t->sx,t->sy,t->w,t->h,t->x,t->y,t->xs,t->ys,t->blend,t->alpha);
      else
        gml_draw_sprite_part_ext(R,t->sprite,0,t->sx,t->sy,t->w,t->h,t->x,t->y,t->xs,t->ys,t->blend,t->alpha);
      continue; }
    if(it[k].type==3){ struct LayBg *b=&lbg[it[k].idx];
      if(b->sprite<0) gml_draw_layer_color_fill(R,b->blend,b->alpha);
      else if(!b->native){
        /* Compatibility layer records produced by older formats keep the legacy sprite-instance
         * origin and combined tiling behavior. The distinct-axis/top-left semantics below belong
         * to native GMS2 background layers. */
        if(b->th || b->tv) gml_draw_sprite_tiled_ext(R,b->sprite,0,b->x,b->y,1,1,b->blend,b->alpha);
        else gml_draw_sprite_ext(R,b->sprite,0,b->x,b->y,1,1,0,b->blend,b->alpha);
      }
      else {
        GmlRenderSpriteMetrics sprite;
        if(b->stretch && gml_render_sprite_metrics(R,b->sprite,&sprite) &&
           sprite.width>0 && sprite.height>0){
          double xs=(double)rm.width/sprite.width;
          double ys=(double)rm.height/sprite.height;
          gml_draw_sprite_ext(R,b->sprite,b->subimg,
            b->x+sprite.origin_x*xs,b->y+sprite.origin_y*ys,
            xs,ys,0,b->blend,b->alpha);
        } else gml_draw_layer_background_sprite(R,b->sprite,b->subimg,b->x,b->y,
                                                  b->xs,b->ys,b->blend,b->alpha,b->th,b->tv);
      }
      continue; }
    if(it[k].type==4){ gml_part_system_drawit(vm->particles,R,it[k].idx); continue; }
    if(it[k].type==5){ struct LaySprite *s=&lsp[it[k].idx];
      gml_draw_sprite_ext(R,s->sprite,s->subimg,s->x,s->y,s->xs,s->ys,s->angle,s->blend,s->alpha);
      continue; }
    if(it[k].type==6){ struct ClassicBg *b=&cbg[it[k].idx];
      if(b->def<0) gml_draw_layer_color_fill(R,b->blend,b->alpha);
      else if(b->stretch) gml_draw_background_stretched(R,b->def,b->x,b->y,rm.width,rm.height,b->blend,b->alpha);
      else if(b->th || b->tv) gml_draw_background_tiled_ext(R,b->def,b->x,b->y,1,1,b->blend,b->alpha,b->th,b->tv);
      else gml_draw_background_ext(R,b->def,b->x,b->y,1,1,b->blend,b->alpha);
      continue; }
    if(it[k].type==7){ struct LayEffect *f=&lfx[it[k].idx];
      if(f->effect.kind==GML_LAYER_FILTER_RGB_NOISE)
        gml_render_layer_rgb_noise(R,f->effect.u.noise.sampler_tpag_ptr,
                                   f->effect.u.noise.intensity,f->effect.u.noise.animation,
                                   f->effect.u.noise.colour&0xFFFFFFu);
      else if(f->effect.kind==GML_LAYER_FILTER_TINT)
        gml_render_layer_tint(R,f->effect.u.tint.colour);
      continue; }
    GmlInstance *in=&vm->inst[it[k].idx];
    if(vm->draw_events_off) continue;   /* draw_enable_drawevent(false): no instance drawing */
    { const char *sk=anygm_host_development_setting(vm->host,"GML_SKIP");                  /* debug: skip drawing a named object */
      if(sk && in->obj>=0 && in->obj<vm->n_objects && vm->objects[in->obj].name
         && strstr(vm->objects[in->obj].name,sk)) continue; }
    if(in->visible<0.5) continue;   /* GM: an invisible instance runs neither
                                       its Draw event nor the automatic sprite draw. */
    /* Draw event replaces default draw; else draw sprite_index automatically. */
    if(gml_vm_instances_event_lookup(vm,"Draw_0",in->obj,NULL,NULL)){
      draw_event_hook(vm,in,"Draw_0",1);
      int drew=gml_run_event(vm,in,"Draw_0");
      draw_event_hook(vm,in,"Draw_0",0);
      if(drew) continue;
    }
    if(in->sprite_index>=0)
      gml_vm_draw_instance_sprite(vm,in,in->image_alpha);
  }
  if(active_layer) gml_run_layer_script(vm,active_layer->script_end);
  if(active_filter_started) gml_render_layer_filter_end(R,active_filter,effect_time);
  gml_render_shader_set_current(R,-1);
  /* All draw scratch (it/lbg/ltl/lsp/tiles/tdepth) is persistent (g_dl_*) — write the possibly-grown
   * tile buffers back and keep everything allocated for next frame; nothing is freed here. */
  scratch->tile=tiles; scratch->tile_depth=tdepth; scratch->tile_capacity=tcap;
}

/* Draw GUI pass: run Draw GUI events (Draw_64) in depth order. GUI code can
 * composite the application surface and overlays through ordinary GML events.
 * view_current during the GUI pass: the desktop GMS2 runtime iterates all 8 view slots
 * (visible or not) in its per-view draw loop, so after the loop view_current is left at 7,
 * and the GUI event observes that leftover. Converted content can depend on this desktop-runtime
 * behavior. With views disabled the
 * loop never runs and view_current stays 0. */
/* Run one draw-stage event pass (Draw_72/73 begin/end, Draw_74/75 and Draw_65/66 GUI stages,
 * Draw_76/77 pre/post) over all instances in depth order. Cheap no-op when no object has it. */
int gml_vm_draw_pass_active(GmlVM *vm, const char *suffix){
  if(!vm || !suffix || !vm->render) return 0;
  for(int i=0;i<vm->inst_count;i++){
    GmlInstance *in=&vm->inst[i];
    if(!in->active||in->marked||in->visible<0.5||!instance_draw_layer_visible(vm,in)) continue;
    if(gml_vm_instances_event_lookup(vm,suffix,in->obj,NULL,NULL)) return 1;
  }
  return 0;
}
void gml_vm_draw_pass(GmlVM *vm, const char *suffix){
  GmlRender *R=(GmlRender*)vm->render; if(!R) return;
  gml_render_set_frame(R,vm->frame);
  int n=vm->inst_count;
  int *ord=vm_draw_order_scratch(vm,n>0?n:1); if(!ord) return;
  int m=0;
  for(int i=0;i<n;i++){ GmlInstance *in=&vm->inst[i];
    if(!in->active||in->marked||in->visible<0.5||!instance_draw_layer_visible(vm,in)) continue;
    if(gml_vm_instances_event_lookup(vm,suffix,in->obj,NULL,NULL)) ord[m++]=i;
  }
  if(m>1) gml_vm_instances_sort_slots(vm,ord,m);
  for(int a=0;a<m;a++) for(int b=a+1;b<m;b++)
    if(vm->inst[ord[b]].depth>vm->inst[ord[a]].depth){ int t=ord[a]; ord[a]=ord[b]; ord[b]=t; }
  for(int k=0;k<m;k++){
    GmlInstance *in=&vm->inst[ord[k]];
    if(anygm_host_development_setting(vm->host,"GML_LOG_DRAW_PASS")){
      const char *on=(in->obj>=0&&in->obj<vm->n_objects&&vm->objects[in->obj].name)?vm->objects[in->obj].name:"?";
      int ci=-1; gml_vm_instances_event_lookup(vm,suffix,in->obj,NULL,&ci);
      const char *cn=(ci>=0&&vm->win&&ci<vm->win->n_code)?vm->win->code[ci].name:"?";
      anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[drawpass] f%ld %s obj=%s id=%d depth=%.0f code=%d:%s\n",
              vm->frame,suffix,on,in->id,in->depth,ci,cn);
    }
    draw_event_hook(vm,in,suffix,1);
    gml_run_event(vm,in,suffix);
    draw_event_hook(vm,in,suffix,0);
  }
}
void gml_vm_draw_gui(GmlVM *vm){
  GmlRender *R=(GmlRender*)vm->render; if(!R) return;
  gml_render_set_frame(R,vm->frame);
  int n=vm->inst_count;
  int *ord=vm_draw_order_scratch(vm,n>0?n:1); if(!ord) return;
  int m=0;
  for(int i=0;i<n;i++){ GmlInstance *in=&vm->inst[i];
    if(!in->active||in->marked||in->visible<0.5||!instance_draw_layer_visible(vm,in)) continue;
    if(gml_vm_instances_event_lookup(vm,"Draw_64",in->obj,NULL,NULL)) ord[m++]=i;
  }
  if(m>1) gml_vm_instances_sort_slots(vm,ord,m);
  for(int a=0;a<m;a++) for(int b=a+1;b<m;b++)            /* depth descending (back -> front) */
    if(vm->inst[ord[b]].depth>vm->inst[ord[a]].depth){ int t=ord[a]; ord[a]=ord[b]; ord[b]=t; }
  int views_on=0;
  for(int v=0;v<8;v++) if(gml_vm_global_array_number(vm,"view_visible",v)>=0.5){ views_on=1; break; }
  *gml_varmap_put(&vm->globals,"view_current")=vreal(views_on?7:0);
  for(int k=0;k<m;k++){
    GmlInstance *in=&vm->inst[ord[k]];
    if(anygm_host_development_setting(vm->host,"GML_LOG_DRAW_PASS")){
      const char *on=(in->obj>=0&&in->obj<vm->n_objects&&vm->objects[in->obj].name)?vm->objects[in->obj].name:"?";
      int ci=-1; gml_vm_instances_event_lookup(vm,"Draw_64",in->obj,NULL,&ci);
      const char *cn=(ci>=0&&vm->win&&ci<vm->win->n_code)?vm->win->code[ci].name:"?";
      anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[drawpass] f%ld Draw_64 obj=%s id=%d depth=%.0f code=%d:%s\n",
              vm->frame,on,in->id,in->depth,ci,cn);
    }
    draw_event_hook(vm,in,"Draw_64",1);
    gml_run_event(vm,in,"Draw_64");
    draw_event_hook(vm,in,"Draw_64",0);
  }
  *gml_varmap_put(&vm->globals,"view_current")=vreal(0);
}
