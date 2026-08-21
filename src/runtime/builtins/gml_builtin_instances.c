/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Instance, room, event, path, timeline, and time-source builtin adapters. */
#include "gml_builtin_internal.h"
#include "anygm_compatibility.h"
#include "gml_render.h"
#include "anygm_host.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static GmlVal gml_builtin_try_instances_events(GmlVM *vm, const char *nm, GmlVal *a, int n);
static GmlVal gml_builtin_try_instances_timelines(GmlVM *vm, const char *nm, GmlVal *a, int n);

static struct GmlViewOvr *room_view_override(GmlVM *vm,int room,int view,int create){
  int nrooms=vm&&vm->win?gml_room_count(vm->win):0;
  if(!vm || room<0 || room>=nrooms || view<0 || view>=8) return NULL;
  if(!vm->view_ovr && create){
    vm->view_ovr=calloc((size_t)nrooms*8,sizeof(*vm->view_ovr));
    if(!vm->view_ovr) return NULL;
    vm->n_view_ovr=nrooms*8;
  }
  int slot=room*8+view;
  return vm->view_ovr && slot<vm->n_view_ovr ? &vm->view_ovr[slot] : NULL;
}

static int time_source_parent_exists(GmlVM *vm, int parent){
  return parent==0 || parent==1 || time_source_find(vm,parent)!=NULL;
}

static double time_source_period(double period, int units){
  if(!isfinite(period)) return period>0.0?DBL_MAX:1.0;
  if(units==1){
    if(period<1.0) return 1.0;
    return floor(period);
  }
  return period>0.0?period:0.0;
}

static int time_source_repetitions(double value){
  if(value<0.0) return -1;
  if(!isfinite(value)) return value>0.0?INT_MAX:1;
  int repetitions=(int)floor(value);
  return repetitions>0?repetitions:1;
}

static void time_source_configure(GmlTimeSource *source, double period, int units,
                                  GmlVal callback, GmlVal args, int repetitions,
                                  int expiry_type){
  if(!source) return;
  source->units=units==1?1:0;
  source->period=time_source_period(period,source->units);
  source->remaining=source->period;
  source->callback=gml_arr_store_clone(callback);
  source->args=args.t==V_ARR?gml_arr_store_clone(args):vundef();
  source->repetitions=repetitions;
  source->reps_remaining=repetitions;
  source->reps_completed=0;
  source->expiry_type=expiry_type==0?0:1;
  source->state=0;
}

static GmlVal time_source_create_builtin(GmlVM *vm, GmlVal *args, int count){
  if(!vm || count<4) return vundef();
  int parent=(int)N(args,count,0);
  if(!time_source_parent_exists(vm,parent)) return vundef();
  int slot=-1;
  for(int i=0;i<GML_TIME_SOURCE_MAX;i++) if(!vm->builtins->time_source[i].live){ slot=i; break; }
  if(slot<0) return vundef();
  if(vm->builtins->next_time_source_id<GML_TIME_SOURCE_ID_BASE)
    vm->builtins->next_time_source_id=GML_TIME_SOURCE_ID_BASE;
  GmlTimeSource *source=&vm->builtins->time_source[slot];
  memset(source,0,sizeof(*source));
  source->live=1;
  source->id=vm->builtins->next_time_source_id++;
  if(vm->builtins->next_time_source_id<GML_TIME_SOURCE_ID_BASE)
    vm->builtins->next_time_source_id=GML_TIME_SOURCE_ID_BASE;
  source->parent=parent;
  int repetitions=count>5?time_source_repetitions(N(args,count,5)):1;
  int expiry_type=count>6?(int)N(args,count,6):1;
  time_source_configure(source,N(args,count,1),(int)N(args,count,2),args[3],
                        count>4?args[4]:vundef(),repetitions,expiry_type);
  return vreal((double)source->id);
}

static int time_source_has_child(GmlVM *vm, int parent){
  for(int i=0;i<GML_TIME_SOURCE_MAX;i++)
    if(vm->builtins->time_source[i].live && vm->builtins->time_source[i].parent==parent) return 1;
  return 0;
}

static GmlVal time_source_builtin(GmlVM *vm, const char *name, GmlVal *args, int count){
  if(!strcmp(name,"time_source_create")) return time_source_create_builtin(vm,args,count);
  int id=count>0?(int)N(args,count,0):-1;
  GmlTimeSource *source=time_source_find(vm,id);
  if(!strcmp(name,"time_source_exists")) return vreal(id==0 || id==1 || source!=NULL);
  if(!strcmp(name,"time_source_start")){
    if(source){ source->remaining=source->period; source->reps_remaining=source->repetitions;
      source->reps_completed=0; source->state=1; }
    else if(id==1) vm->builtins->time_source_game_state=1;
    return vreal(0);
  }
  if(!strcmp(name,"time_source_stop")){
    if(source && (source->state==1 || source->state==2)){
      source->remaining=source->period; source->state=3;
    } else if(id==1 && vm->builtins->time_source_game_state==1) vm->builtins->time_source_game_state=3;
    return vreal(0);
  }
  if(!strcmp(name,"time_source_pause")){
    if(source && source->state==1) source->state=2;
    else if(id==1 && vm->builtins->time_source_game_state==1) vm->builtins->time_source_game_state=2;
    return vreal(0);
  }
  if(!strcmp(name,"time_source_resume")){
    if(source && source->state==2) source->state=1;
    else if(id==1 && vm->builtins->time_source_game_state==2) vm->builtins->time_source_game_state=1;
    return vreal(0);
  }
  if(!strcmp(name,"time_source_reset")){
    if(source){ source->remaining=source->period; source->reps_remaining=source->repetitions;
      source->reps_completed=0; source->state=0; }
    return vreal(0);
  }
  if(!strcmp(name,"time_source_destroy")){
    if(source && !time_source_has_child(vm,id)) memset(source,0,sizeof(*source));
    return vreal(0);
  }
  if(!strcmp(name,"time_source_reconfigure")){
    if(source && count>=4){
      int repetitions=count>5?time_source_repetitions(N(args,count,5)):1;
      int expiry_type=count>6?(int)N(args,count,6):1;
      time_source_configure(source,N(args,count,1),(int)N(args,count,2),args[3],
                            count>4?args[4]:vundef(),repetitions,expiry_type);
    }
    return vreal(0);
  }
  if(!strcmp(name,"time_source_get_children")){
    if(id!=0 && id!=1 && !source) return vundef();
    int children=0;
    for(int i=0;i<GML_TIME_SOURCE_MAX;i++)
      if(vm->builtins->time_source[i].live && vm->builtins->time_source[i].parent==id) children++;
    GmlVal result=gml_arr_new(children,vreal(0)); int index=0;
    for(int i=0;i<GML_TIME_SOURCE_MAX;i++)
      if(vm->builtins->time_source[i].live && vm->builtins->time_source[i].parent==id)
        gml_arr_set(result,index++,vreal((double)vm->builtins->time_source[i].id));
    return result;
  }
  if(!source){
    if(!strcmp(name,"time_source_get_state") && id==1) return vreal(vm->builtins->time_source_game_state);
    return vundef();
  }
  if(!strcmp(name,"time_source_get_parent")) return vreal(source->parent);
  if(!strcmp(name,"time_source_get_period")) return vreal(source->period);
  if(!strcmp(name,"time_source_get_reps_completed")) return vreal(source->reps_completed);
  if(!strcmp(name,"time_source_get_reps_remaining")) return vreal(source->reps_remaining);
  if(!strcmp(name,"time_source_get_state")) return vreal(source->state);
  if(!strcmp(name,"time_source_get_time_remaining")) return vreal(source->remaining);
  if(!strcmp(name,"time_source_get_units")) return vreal(source->units);
  return vundef();
}

GmlVal gml_builtin_try_instances_rooms(GmlVM *vm, const char *nm, GmlVal *a, int n){
  GmlRender *R=(GmlRender*)vm->render;
  (void)R;
  /* ---- rooms / flow ---- */
  if(!strcmp(nm,"room_get_name")){ GmlRoom rr; int ri=(int)N(a,n,0);
    return vstr(gml_room_get(vm->win,ri,&rr)==0 && rr.name ? rr.name : ""); }
  if(!strcmp(nm,"room_exists")){ int ri=(int)N(a,n,0); return vreal(ri>=0 && ri<gml_room_count(vm->win)); }
  if(!strcmp(nm,"room_set_width")){
    if(n>=2) (void)gml_vm_room_set_dimension(vm,(int)N(a,n,0),0,N(a,n,1));
    return vreal(0);
  }
  if(!strcmp(nm,"room_set_height")){
    if(n>=2) (void)gml_vm_room_set_dimension(vm,(int)N(a,n,0),1,N(a,n,1));
    return vreal(0);
  }
  if(!strcmp(nm,"room_set_view_enabled")){
    int rm=(int)N(a,n,0);
    struct GmlViewOvr *o=room_view_override(vm,rm,0,1);
    if(!o) return vreal(-1);
    o->room_enabled_set=1;
    o->room_enabled=N(a,n,1)>=0.5;
    if(builtin_setting(vm,"GML_LOG_VIEW"))
      anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[view] room_set_view_enabled rm=%d enabled=%d\n",rm,o->room_enabled);
    return vreal(0);
  }
  if(!strcmp(nm,"room_set_view")){
    /* Classic room_set_view edits the complete indexed view record for a future room entry:
     * room, index, visible, view rect, port rect, borders, follow speed, followed object. */
    int rm=(int)N(a,n,0), vind=(int)N(a,n,1);
    struct GmlViewOvr *o=room_view_override(vm,rm,vind,1);
    if(!o) return vreal(-1);
    o->full=1; o->vis=N(a,n,2)>=0.5;
    o->view_x=(int)N(a,n,3); o->view_y=(int)N(a,n,4);
    o->view_w=(int)N(a,n,5); o->view_h=(int)N(a,n,6);
    o->x=(int)N(a,n,7); o->y=(int)N(a,n,8);
    o->w=(int)N(a,n,9); o->h=(int)N(a,n,10);
    o->hborder=(int)N(a,n,11); o->vborder=(int)N(a,n,12);
    o->hspeed=(int)N(a,n,13); o->vspeed=(int)N(a,n,14);
    o->object=(int)N(a,n,15);
    if(builtin_setting(vm,"GML_LOG_VIEW"))
      anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[view] room_set_view rm=%d v=%d vis=%d view=(%d,%d,%d,%d) port=(%d,%d,%d,%d) follow=%d\n",
        rm,vind,o->vis,o->view_x,o->view_y,o->view_w,o->view_h,
        o->x,o->y,o->w,o->h,o->object);
    return vreal(0);
  }
  if(!strcmp(nm,"room_set_viewport")){
    /* room_set_viewport(rm, vind, visible, xport, yport, wport, hport): like GMS, edits the
     * room's viewport config; applied when that room is entered (gml_room_enter). */
    int rm=(int)N(a,n,0), vind=(int)N(a,n,1);
    struct GmlViewOvr *o=room_view_override(vm,rm,vind,1);
    if(!o) return vreal(-1);
    o->set=1; o->vis=N(a,n,2)>=0.5;
    o->x=(int)N(a,n,3); o->y=(int)N(a,n,4); o->w=(int)N(a,n,5); o->h=(int)N(a,n,6);
    if(builtin_setting(vm,"GML_LOG_VIEW")) anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[view] room_set_viewport rm=%d v=%d vis=%d port=(%d,%d,%d,%d)\n",
      rm,vind,o->vis,o->x,o->y,o->w,o->h);
    return vreal(0);
  }
  if(!strcmp(nm,"room_get_viewport")){
    int rm=(int)N(a,n,0), vind=(int)N(a,n,1);
    struct GmlViewOvr *o=room_view_override(vm,rm,vind,0);
    if(o && (o->set || o->full)){
      GmlVal v=arr_newv(5); if(v.t!=V_ARR) return v;
      GmlArr *A=(GmlArr*)v.arr;
      A->data[0]=vreal(o->vis); A->data[1]=vreal(o->x); A->data[2]=vreal(o->y);
      A->data[3]=vreal(o->w);   A->data[4]=vreal(o->h);
      return v;
    }
    return arr_newv(5);
  }
  if(!strcmp(nm,"room_goto")||!strcmp(nm,"room_restart")||!strcmp(nm,"room_goto_next")){
    if(builtin_setting(vm,"GML_LOG_ROOMGOTO")){ const char*w=(vm->cur_self&&vm->cur_self->obj>=0&&vm->cur_self->obj<vm->n_objects)?vm->objects[vm->cur_self->obj].name:"?";
      /* Include the active code entry alongside the object for calls issued through struct methods. */
      const char *in=(vm->win && vm->cur_code_index>=0 && vm->cur_code_index<vm->win->n_code &&
                      vm->win->code[vm->cur_code_index].name)?vm->win->code[vm->cur_code_index].name:"?";
      /* Log the requested target separately from the current room. */
      anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[rg] %s by=%s from=%d to=%d in=%s\n",
        nm,w,vm->room_index,(n>0)?(int)N(a,n,0):-1,in); } }
  if(!strcmp(nm,"room_goto")){
    int target=(int)N(a,n,0);
    gml_vm_warm_audio_for_room(vm,target);
    vm->pending_room=target;
    return vreal(0); }
  if(!strcmp(nm,"room_goto_next")||!strcmp(nm,"room_next")){
    int cur = !strcmp(nm,"room_next")? (int)N(a,n,0) : vm->room_index;
    int pos=order_pos(vm,cur);
    int nxt = (pos>=0 && pos+1<vm->win->n_room_order)? (int)vm->win->room_order[pos+1] : -1;
    if(!strcmp(nm,"room_next")) return vreal(nxt);
    if(nxt>=0){ gml_vm_warm_audio_for_room(vm,nxt); vm->pending_room=nxt; }
    return vreal(0); }
  if(!strcmp(nm,"room_goto_previous")||!strcmp(nm,"room_previous")){
    int cur = !strcmp(nm,"room_previous")? (int)N(a,n,0) : vm->room_index;
    int pos=order_pos(vm,cur);
    int prv = (pos>0)? (int)vm->win->room_order[pos-1] : -1;
    if(!strcmp(nm,"room_previous")) return vreal(prv);
    if(prv>=0){ gml_vm_warm_audio_for_room(vm,prv); vm->pending_room=prv; }
    return vreal(0); }
  if(!strcmp(nm,"room_restart")){
    gml_vm_warm_audio_for_room(vm,vm->room_index);
    vm->pending_room=vm->room_index;
    return vreal(0); }
  if(!strcmp(nm,"room_set_persistent")) return vreal(0);
  if(!strcmp(nm,"game_end") || !strcmp(nm,"action_end_game")){
    vm->game_end=1; return vreal(0); }
  if(!strcmp(nm,"game_restart") || !strcmp(nm,"action_restart_game")){
    vm->game_end=2; return vreal(0); }
  if(!strcmp(nm,"game_change")){
    const char *directory=S(vm,a,n,0);
    const char *parameters=S(vm,a,n,1);
    int directory_size=snprintf(vm->game_change_directory,
      sizeof vm->game_change_directory,"%s",directory);
    int parameters_size=snprintf(vm->game_change_parameters,
      sizeof vm->game_change_parameters,"%s",parameters);
    if(directory_size<0 || (size_t)directory_size>=sizeof vm->game_change_directory ||
       parameters_size<0 || (size_t)parameters_size>=sizeof vm->game_change_parameters){
      vm->game_change_directory[0]='\0';
      vm->game_change_parameters[0]='\0';
    }
    vm->game_change_pending=(directory_size<0 ||
      (size_t)directory_size>=sizeof vm->game_change_directory || parameters_size<0 ||
      (size_t)parameters_size>=sizeof vm->game_change_parameters)?-1:1;
    return vreal(0);
  }

  return gml_builtin_try_actions(vm,nm,a,n);
}

GmlVal gml_builtin_try_instances(GmlVM *vm, const char *nm, GmlVal *a, int n){
  GmlRender *R=(GmlRender*)vm->render;
  (void)R;
  /* ---- instances ---- */
  if(!strcmp(nm,"instance_create")){ GmlInstance*in=gml_instance_create(vm,N(a,n,0),N(a,n,1),(int)N(a,n,2));
    return vreal(in?(double)in->id:-4); }
  /* GMS2 creation: instance_create_depth(x,y,depth,obj) / instance_create_layer(x,y,layer,obj). The
   * object is the 4th arg (not the 3rd); without these, GMS2 games spawn nothing dynamically. */
  if(!strcmp(nm,"instance_create_depth")){ GmlInstance*in=gml_instance_create_depth(vm,N(a,n,0),N(a,n,1),(int)N(a,n,3),1,N(a,n,2));
    return vreal(in?(double)in->id:-4); }
  if(!strcmp(nm,"instance_create_layer")){
    GmlRtLayer *layer=(n>2 && a[2].t==V_STR && a[2].s)
      ?gml_rt_layer_find_by_name(vm,a[2].s):gml_rt_layer_find(vm,(int)N(a,n,2));
    if(builtin_setting(vm,"GML_LOG_RTL")){ 
      anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[rtl] f%ld instance_create_layer arg=%s/%g resolved=%s id=%d order=%d depth=%.0f\n",
        vm->frame,(n>2&&a[2].t==V_STR&&a[2].s)?a[2].s:"#",N(a,n,2),
        layer?layer->name:"(none)",layer?layer->id:-1,layer?layer->order:-1,layer?layer->depth:0); }
    GmlInstance*in=gml_instance_create_layer(vm,N(a,n,0),N(a,n,1),(int)N(a,n,3),layer?layer->id:-1);
    return vreal(in?(double)in->id:-4); }
  return gml_builtin_try_values_language(vm,nm,a,n);
}

GmlVal gml_builtin_try_instances_scripts(GmlVM *vm, const char *nm, GmlVal *a, int n){
  GmlRender *R=(GmlRender*)vm->render;
  (void)R;
  /* ---- script_execute(scriptid, args...) ---- */
  if(!strcmp(nm,"script_execute") || !strcmp(nm,"action_execute_script")){ int sid=(int)N(a,n,0);
    /* the argument may be a classic SCPT index or a GMS2.3 function value (tagged CODE index) */
    int ci = n>0 ? script_ref_code_of(vm,a[0]) : -1;
    if(builtin_setting(vm,"GML_DBG_SCRIPTX")){ 
      anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[scriptx] f%ld sid=%d -> ci=%d (%s)\n",vm->frame,sid,ci,
        (ci>=0&&ci<vm->win->n_code)?vm->win->code[ci].name:"?"); }
    return gml_vm_run_code(vm,ci,vm->cur_self,vm->cur_other,a+1,n-1); }
  if(!strcmp(nm,"script_exists")){ int sid=(int)N(a,n,0);
    if(GML_IS_FUNCVAL(sid)) return vreal(1);
    return vreal(script_code_of(vm,sid)>=0); }
  if(!strcmp(nm,"script_get_name")){
    int sid=(int)N(a,n,0);
    int ci = GML_IS_FUNCVAL(sid) ? (sid & 0x00FFFFFF) : script_code_of(vm,sid);
    return vstr((vm->win&&ci>=0&&ci<vm->win->n_code&&vm->win->code[ci].name)?vm->win->code[ci].name:"");
  }

  return gml_builtin_try_instances_events(vm,nm,a,n);
}

static GmlVal gml_builtin_try_instances_events(GmlVM *vm, const char *nm, GmlVal *a, int n){
  GmlRender *R=(GmlRender*)vm->render;
  (void)R;
  /* ---- event dispatch ---- */
  if(!strcmp(nm,"event_inherited")||!strcmp(nm,"action_inherited")){
    gml_event_inherited(vm); return vreal(0); }
  if(!strcmp(nm,"event_user")){ char s[24]; snprintf(s,sizeof s,"Other_%d",10+(int)N(a,n,0));
    if(vm->cur_self) gml_run_event(vm,vm->cur_self,s);
    return vreal(0); }
  /* event_perform(type,numb): manually run one of THIS instance's events. GM event types:
   * 0 Create, 1 Destroy, 2 Alarm, 3 Step, 4 Collision, 6 Mouse, 7 Other, 8 Draw
   * (numb = subtype/alarm/other obj). */
  if(!strcmp(nm,"event_perform")){
    if(vm->cur_self){ int ty=(int)N(a,n,0), nb=(int)N(a,n,1); const char *pre=0;
      switch(ty){ case 0:pre="Create";nb=0;break; case 1:pre="Destroy";nb=0;break; case 2:pre="Alarm";break;
        case 3:pre="Step";break; case 4:pre="Collision";break; case 6:pre="Mouse";break;
        case 7:pre="Other";break; case 8:pre="Draw";break; }
      if(pre){
        char s[24];
        snprintf(s,sizeof s,"%s_%d",pre,nb);
        /* A collision handler registered for a parent serves its descendants. When the requested
         * subtype names a child, walk its ancestry until this instance's event table has a match. */
        if(ty==4 && nb>=0 && nb<vm->n_objects && vm->cur_self->obj>=0){
          for(int object=nb; object>=0 && object<vm->n_objects;
              object=vm->objects[object].parent){
            char candidate[24];
            snprintf(candidate,sizeof candidate,"Collision_%d",object);
            if(gml_vm_instances_event_lookup(vm,candidate,vm->cur_self->obj,NULL,NULL)){
              snprintf(s,sizeof s,"%s",candidate);
              break;
            }
          }
        }
        gml_run_event(vm,vm->cur_self,s);
      } }
    return vreal(0); }
  if(!strcmp(nm,"event_perform_object")) return vreal(0);

  return gml_builtin_try_audio(vm,nm,a,n);
}

GmlVal gml_builtin_try_instances_paths(GmlVM *vm, const char *nm, GmlVal *a, int n){
  GmlRender *R=(GmlRender*)vm->render;
  (void)R;
  /* ---- paths (path_start / path_end): instance follows a PATH each step ---- */
  if(!strcmp(nm,"path_start")){ if(vm->cur_self)
      gml_path_start(vm,vm->cur_self,(int)N(a,n,0),N(a,n,1),N(a,n,2),(int)N(a,n,3));
    return vreal(0); }
  if(!strcmp(nm,"action_path")){ if(vm->cur_self)
      gml_path_start(vm,vm->cur_self,(int)N(a,n,0),N(a,n,1),N(a,n,2),(int)N(a,n,3));
    return vreal(0); }
  if(!strcmp(nm,"path_end") || !strcmp(nm,"action_path_end")){
    if(vm->cur_self) vm->cur_self->path_index=-1;
    return vreal(0);
  }
  if(!strcmp(nm,"action_path_speed")){
    if(vm->cur_self) vm->cur_self->path_speed=N(a,n,0);
    return vreal(0);
  }

  return gml_builtin_try_instances_timelines(vm,nm,a,n);
}

static GmlVal gml_builtin_try_instances_timelines(GmlVM *vm, const char *nm, GmlVal *a, int n){
  GmlRender *R=(GmlRender*)vm->render;
  (void)R;
  /* ---- classic timelines ---- */
  if(!strcmp(nm,"timeline_exists")){ int ti=(int)N(a,n,0);
    return vreal(ti>=0 && ti<vm->n_timelines && vm->timelines[ti].name!=NULL); }
  if(!strcmp(nm,"timeline_get_name")){ int ti=(int)N(a,n,0);
    return vstr((ti>=0 && ti<vm->n_timelines && vm->timelines[ti].name)?vm->timelines[ti].name:""); }
  if(!strcmp(nm,"timeline_add")) return vreal(gml_timeline_add(vm));
  if(!strcmp(nm,"timeline_clear")){ gml_timeline_clear(vm,(int)N(a,n,0)); return vreal(0); }
  if(!strcmp(nm,"action_set_timeline") || !strcmp(nm,"action_timeline_set")){ if(vm->cur_self){
      vm->cur_self->timeline_index=N(a,n,0); vm->cur_self->timeline_position=N(a,n,1);
      vm->cur_self->timeline_running=N(a,n,2)!=0; vm->cur_self->timeline_loop=N(a,n,3)!=0; }
    return vreal(0); }
  if(!strcmp(nm,"action_set_timeline_position")){ if(vm->cur_self) vm->cur_self->timeline_position=N(a,n,0); return vreal(0); }
  if(!strcmp(nm,"action_set_timeline_speed")){ if(vm->cur_self) vm->cur_self->timeline_speed=N(a,n,0); return vreal(0); }
  if(!strcmp(nm,"action_timeline_start")){ if(vm->cur_self) vm->cur_self->timeline_running=1; return vreal(0); }
  if(!strcmp(nm,"action_timeline_pause")){ if(vm->cur_self) vm->cur_self->timeline_running=0; return vreal(0); }
  if(!strcmp(nm,"action_timeline_stop")){ if(vm->cur_self){ vm->cur_self->timeline_running=0; vm->cur_self->timeline_position=0; } return vreal(0); }

  if(!strncmp(nm,"time_source_",12)) return time_source_builtin(vm,nm,a,n);
  if(!strcmp(nm,"time_seconds_to_bpm")){ double seconds=N(a,n,0); return vreal(seconds!=0.0?60.0/seconds:INFINITY); }
  if(!strcmp(nm,"time_bpm_to_seconds")){ double bpm=N(a,n,0); return vreal(bpm!=0.0?60.0/bpm:INFINITY); }

  return gml_builtin_try_layers_early(vm,nm,a,n);
}

GmlVal gml_builtin_try_instances_destroy(GmlVM *vm, const char *nm, GmlVal *a, int n){
  if(!strcmp(nm,"instance_destroy")){
    /* instance_destroy([id_or_obj, execute_event]): no argument selects self; an explicit
     * target selects one instance or every instance in the matching object family. A false second
     * argument suppresses Destroy while Clean Up still runs. */
    int perform_destroy_event=n<2 || N(a,n,1)!=0;
    if(n>=1){ int target=(int)N(a,n,0);
      for(int i=0;i<vm->inst_count;i++){ GmlInstance *o=&vm->inst[i];
        if(target_matches_instance(vm,vm->cur_self,o,target))
          gml_instance_destroy_with_event(vm,o,perform_destroy_event); } }
    else if(vm->cur_self) gml_instance_destroy_with_event(vm,vm->cur_self,perform_destroy_event);
    return vreal(0); }
  return gml_builtin_try_actions_legacy(vm,nm,a,n);
}

GmlVal gml_builtin_try_instances_queries(GmlVM *vm, const char *nm, GmlVal *a, int n){
  if(!strcmp(nm,"instance_number")) return vreal(gml_instance_number(vm,(int)N(a,n,0)));
  if(!strcmp(nm,"instance_exists")||!strcmp(nm,"existe"))
    return vreal(gml_instance_number(vm,(int)N(a,n,0))>0);
  if(!strcmp(nm,"instance_find")){
    int obj=(int)N(a,n,0), nth=(int)N(a,n,1), seen=0;
    if(nth<0) return vreal(-4);
    for(int i=0;i<vm->inst_count;i++){ GmlInstance *o=&vm->inst[i];
      if(!target_matches_instance(vm,vm->cur_self,o,obj)) continue;
      if(seen++==nth) return vreal(o->id);
    }
    return vreal(-4);
  }
  /* instance (de)activation supports off-screen culling and pause behavior.
   * Deactivated instances keep active=0 so step/draw/query guards skip them. */
  if(!strcmp(nm,"instance_deactivate_object")){ int obj=(int)N(a,n,0);
    for(int i=0;i<vm->inst_count;i++){ GmlInstance*o=&vm->inst[i];
      if(target_matches_instance(vm,vm->cur_self,o,obj)){ o->active=0; o->deactivated=1; } } return vreal(0); }
  if(!strcmp(nm,"instance_deactivate_all")){ int notme=(int)N(a,n,0);
    for(int i=0;i<vm->inst_count;i++){ GmlInstance*o=&vm->inst[i];
      if(o->active&&!o->marked&&!(notme&&o==vm->cur_self)){ o->active=0; o->deactivated=1; } } return vreal(0); }
  if(!strcmp(nm,"instance_activate_object")){ int obj=(int)N(a,n,0);
    for(int i=0;i<vm->inst_count;i++){ GmlInstance*o=&vm->inst[i];
      if(o->deactivated && !o->marked && (obj==IT_ALL || obj==IT_SELF || obj==IT_OTHER ||
          (obj>=100000 && (int)o->id==obj) || (obj>=0 && gml_object_is(vm,o->obj,obj)))){
        o->active=1; o->deactivated=0;
      } } return vreal(0); }
  if(!strcmp(nm,"instance_activate_all")){
    for(int i=0;i<vm->inst_count;i++){ GmlInstance*o=&vm->inst[i];
      if(o->deactivated){ o->active=1; o->deactivated=0; } } return vreal(0); }
  if(!strcmp(nm,"instance_activate_region")||!strcmp(nm,"instance_deactivate_region")){
    double rx=N(a,n,0),ry=N(a,n,1),rw=N(a,n,2),rh=N(a,n,3); int inside=(int)N(a,n,4);
    int act=!strcmp(nm,"instance_activate_region");
    for(int i=0;i<vm->inst_count;i++){ GmlInstance*o=&vm->inst[i];
      if(act ? !o->deactivated : (!o->active||o->marked)) continue;
      if(!act && n>=6 && N(a,n,5)!=0 && o==vm->cur_self) continue;
      int inreg=instance_region_hit(vm,o,rx,ry,rw,rh);
      if(inreg==(inside!=0)){ if(act){ o->active=1; o->deactivated=0; } else { o->active=0; o->deactivated=1; } } }
    return vreal(0); }
  if(!strcmp(nm,"instance_change") || !strcmp(nm,"action_change_object")){
    if(vm->cur_self) gml_instance_change(vm,vm->cur_self,(int)N(a,n,0),N(a,n,1)>=0.5);
    return vreal(0);
  }
  /* instance_position(x,y,obj): id of the instance of obj whose mask covers (x,y), else noone(-4). */
  if(!strcmp(nm,"instance_position")){ GmlInstance *o=instance_at_point(vm,N(a,n,0),N(a,n,1),(int)N(a,n,2));
    return vreal(o? (double)o->id : -4); }
  /* instance_place(x,y,obj): id of the first instance self would collide with at (x,y), else noone. */
  if(!strcmp(nm,"instance_place")){ GmlInstance *o=collision_instance_at(vm,N(a,n,0),N(a,n,1),(int)N(a,n,2),0);
    return vreal(o? (double)o->id : -4); }
  if(!strcmp(nm,"instance_place_list")){
    GmlDSList *l=ds_list_slot_repair(vm,(int)N(a,n,3));
    int r=collision_instance_list_at(vm,N(a,n,0),N(a,n,1),(int)N(a,n,2),l,N(a,n,4)>=0.5);
    if(log_col_on(vm)){ GmlInstance*cs=vm->cur_self; const char*cnm=(cs&&cs->obj>=0&&cs->obj<vm->n_objects)?vm->objects[cs->obj].name:"?";
      const char*tn=((int)N(a,n,2)>=0&&(int)N(a,n,2)<vm->n_objects)?vm->objects[(int)N(a,n,2)].name:"?";
      if(log_col_match(vm,cnm)) anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[col] %s instance_place_list(%.0f,%.0f,%s)=%d\n",cnm,N(a,n,0),N(a,n,1),tn,r); }
    return vreal(r); }
  if(!strcmp(nm,"collision_line")){
    double x1=N(a,n,0), y1=N(a,n,1), x2=N(a,n,2), y2=N(a,n,3);
    int obj=(int)N(a,n,4), precise=N(a,n,5)>=0.5, notme=(int)N(a,n,6);
    if(!vm->diagnostics.collision_line_setting_initialized){
      const char *setting=builtin_setting(vm,"GML_DBG_COLLINE");
      snprintf(vm->diagnostics.collision_line_setting,
               sizeof vm->diagnostics.collision_line_setting,"%s",setting?setting:"");
      vm->diagnostics.collision_line_setting_initialized=1;
    }
    const char *colline_dbg=vm->diagnostics.collision_line_setting[0]?
                            vm->diagnostics.collision_line_setting:NULL;
    GmlInstance *hit=collision_line_query(vm,x1,y1,x2,y2,obj,precise,notme);
    if(colline_dbg && !hit){ 
      anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[colline] f%ld (%.2f,%.2f)-(%.2f,%.2f) obj=%d prec=%d notme=%d MISS\n",
        vm->frame,x1,y1,x2,y2,obj,precise,notme);
      if(!strcmp(colline_dbg,"boxes")){
        for(int i=0;i<vm->inst_count;i++){ GmlInstance *o=&vm->inst[i]; double l,t,r,b;
          if(!target_matches_instance(vm,vm->cur_self,o,obj) ||
             !inst_bbox(vm,o,o->x,o->y,&l,&t,&r,&b)) continue;
          anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[colline-box] %s id=%u at(%.2f,%.2f) bbox=(%.2f,%.2f)-(%.2f,%.2f)\n",
            (o->obj>=0&&o->obj<vm->n_objects)?vm->objects[o->obj].name:"?",o->id,o->x,o->y,l,t,r,b);
        }
      }
    }
    if(hit){ if(colline_dbg){ 
        anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[colline] f%ld (%.0f,%.0f)-(%.0f,%.0f) obj=%d HIT %s id=%u at(%.1f,%.1f)\n",
          vm->frame,x1,y1,x2,y2,obj,
          (hit->obj>=0&&hit->obj<vm->n_objects)?vm->objects[hit->obj].name:"?",hit->id,hit->x,hit->y); }
      return vreal(hit->id); }
    return vreal(-4);
  }
  if(!strcmp(nm,"collision_line_list")){
    GmlDSList *l=ds_list_slot_repair(vm,(int)N(a,n,7));
    int r=collision_line_list_query(vm,N(a,n,0),N(a,n,1),N(a,n,2),N(a,n,3),
                                    (int)N(a,n,4),N(a,n,5)>=0.5,(int)N(a,n,6),
                                    l,N(a,n,8)>=0.5);
    return vreal(r);
  }
  /* Step an overlapping instance along `direction` until it clears either solids or every
   * instance, subject to maxdist. Both variants use the same mask-precise collision path. */
  if(!strcmp(nm,"move_outside_solid")||!strcmp(nm,"move_outside_all")){ GmlInstance *s=vm->cur_self; if(s){
      int all=!strcmp(nm,"move_outside_all");
      double dir=N(a,n,0), md=N(a,n,1); if(md<=0) md=1000;
      double dx=cos(dir*M_PI/180.0), dy=-sin(dir*M_PI/180.0);
      for(int k=0;k<(int)md;k++){
        if(!collision_at(vm,s->x,s->y,all?IT_ALL:0,!all)) break;
        s->x+=dx; s->y+=dy;
      }
      gml_colgrid_touch(vm,s); }
    return vreal(0); }
  if(!strcmp(nm,"move_bounce_solid")||!strcmp(nm,"move_bounce_all")){ GmlInstance *s=vm->cur_self; if(s){
      int all=!strcmp(nm,"move_bounce_all");
      classic_move_bounce(vm,s,all,N(a,n,0)!=0.0); }
    return vreal(0); }
  if(!strcmp(nm,"move_wrap")){ GmlInstance *s=vm->cur_self; GmlRoom room;
    if(s && gml_vm_room_get(vm,vm->room_index,&room)==0){
      int horizontal=N(a,n,0)!=0, vertical=N(a,n,1)!=0; double margin=fabs(N(a,n,2));
      if(horizontal){ if(s->x < -margin) s->x=room.width+margin; else if(s->x>room.width+margin) s->x=-margin; }
      if(vertical){ if(s->y < -margin) s->y=room.height+margin; else if(s->y>room.height+margin) s->y=-margin; }
      gml_colgrid_touch(vm,s);
    }
    return vreal(0);
  }
  /* action_snap aliases the move_snap grid operation. Both forms use absolute grid spacing;
   * the action-relative flag is irrelevant. */
  if(!strcmp(nm,"move_snap")||!strcmp(nm,"action_snap")){ GmlInstance *s=vm->cur_self; if(s){
      double xs=N(a,n,0), ys=N(a,n,1); if(xs>0) s->x=round(s->x/xs)*xs; if(ys>0) s->y=round(s->y/ys)*ys; gml_colgrid_touch(vm,s); }
    return vreal(0); }
  /* action_set_alarm(value,index): D&D Set Alarm → self.alarm[index] = value. */
  if(!strcmp(nm,"action_set_alarm")){ GmlInstance *s=vm->cur_self; int idx=(int)N(a,n,1);
    double value=N(a,n,0);
    if(vm->win && anygm_policy_uses_classic_runtime(vm->win)) value=nearbyint(value);
    if(s && idx>=0 && idx<GML_ALARMS) s->alarm[idx]=value;
    return vreal(0); }
  /* Studio 2's accessor uses index,value order, unlike the legacy D&D action above. */
  if(!strcmp(nm,"alarm_set")){ GmlInstance *s=vm->cur_self; int idx=(int)N(a,n,0);
    double value=N(a,n,1);
    if(vm->win && anygm_policy_uses_classic_runtime(vm->win)) value=nearbyint(value);
    if(s && idx>=0 && idx<GML_ALARMS) s->alarm[idx]=value;
    return vreal(0); }
  if(!strcmp(nm,"alarm_get")){ GmlInstance *s=vm->cur_self; int idx=(int)N(a,n,0);
    return vreal(s && idx>=0 && idx<GML_ALARMS ? s->alarm[idx] : -1); }
  /* action_bounce(advanced,against): D&D Bounce. against 0=solid, 1=all. */
  if(!strcmp(nm,"action_bounce")){ GmlInstance *s=vm->cur_self; if(s){
      int advanced=N(a,n,0)!=0, all=(int)N(a,n,1)!=0;
      classic_move_bounce(vm,s,all,advanced); }
    return vreal(0); }

  return gml_builtin_try_particles(vm,nm,a,n);
}
