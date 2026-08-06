/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* gml_vm_rooms.c — path, timeline, and room-phase implementation ownership. */
#include "gml_vm.h"
#include "gml_vm_internal.h"
#include "gml_value_internal.h"
#include "gml_builtin.h"
#include "anygm_compatibility.h"
#include "gml_render.h"
#include "gml_audio.h"
#include "anygm_host.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------- path parsing + evaluation (PATH chunk) ---------------- */
typedef struct { double x,y,sp; } PathCtlPt;

static PathCtlPt path_ctl_at(PathCtlPt *pt, int n, int idx, int closed){
  if(closed){
    idx%=n; if(idx<0) idx+=n;
    return pt[idx];
  }
  if(idx<0) return pt[0];
  if(idx>=n) return pt[n-1];
  return pt[idx];
}

static double path_catmull(double p0, double p1, double p2, double p3, double t){
  double t2=t*t, t3=t2*t;
  return 0.5*((2.0*p1)+(-p0+p2)*t+(2.0*p0-5.0*p1+4.0*p2-p3)*t2+(-p0+3.0*p1-3.0*p2+p3)*t3);
}

static void path_append_sample(GmlPath *p, int *cap, double x, double y, double sp){
  if(p->n>=*cap){
    *cap=*cap?(*cap*2):16;
    p->pts=realloc(p->pts,(size_t)*cap*sizeof(GmlPathPt));
  }
  p->pts[p->n].x=x; p->pts[p->n].y=y; p->pts[p->n].sp=sp; p->pts[p->n].clen=0;
  p->n++;
}

static void path_build_samples(GmlPath *p, PathCtlPt *ctl, int npt){
  int cap=0; p->pts=NULL; p->n=0;
  if(npt<=0) return;
  if(npt==1){
    path_append_sample(p,&cap,ctl[0].x,ctl[0].y,ctl[0].sp);
    return;
  }
  if(p->kind==0){
    for(int k=0;k<npt;k++) path_append_sample(p,&cap,ctl[k].x,ctl[k].y,ctl[k].sp);
    return;
  }
  int subdiv=1 << (p->precision>0?p->precision:1);
  if(subdiv<2) subdiv=2;
  if(subdiv>256) subdiv=256;
  int seg=p->closed?npt:npt-1;
  for(int i=0;i<seg;i++){
    PathCtlPt p0=path_ctl_at(ctl,npt,i-1,p->closed);
    PathCtlPt p1=path_ctl_at(ctl,npt,i,p->closed);
    PathCtlPt p2=path_ctl_at(ctl,npt,i+1,p->closed);
    PathCtlPt p3=path_ctl_at(ctl,npt,i+2,p->closed);
    for(int m=0;m<subdiv;m++){
      double t=(double)m/(double)subdiv;
      double x=path_catmull(p0.x,p1.x,p2.x,p3.x,t);
      double y=path_catmull(p0.y,p1.y,p2.y,p3.y,t);
      double sp=p1.sp+(p2.sp-p1.sp)*t;
      path_append_sample(p,&cap,x,y,sp);
    }
  }
  if(!p->closed) path_append_sample(p,&cap,ctl[npt-1].x,ctl[npt-1].y,ctl[npt-1].sp);
}

static void parse_paths(GmlVM *vm){
  GmlWin *w=vm->win; const GmlChunk *c=gml_chunk(w,"PATH"); if(!c) return;
  const uint8_t *d=w->data; uint32_t base=c->off;
  uint32_t n=gml_vm_read_u32_le(d,base); vm->paths=calloc(n>0?n:1,sizeof(GmlPath)); vm->n_paths=n;
  for(uint32_t i=0;i<n;i++){
    uint32_t ep=gml_vm_read_u32_le(d,base+4+i*4);
    GmlPath *p=&vm->paths[i];
    /* GMS1.4 PATH entry: [name_ptr:u32][kind:u32(0=straight,1=smooth)][closed:u32][precision:u32]
     * [n_points:u32][points: each x(f32),y(f32),speed_factor(f32)=12B]. */
    p->kind=(int)gml_vm_read_u32_le(d,ep+4);
    p->closed=(int)gml_vm_read_u32_le(d,ep+8);
    p->precision=(int)gml_vm_read_u32_le(d,ep+12);
    int npt=(int)gml_vm_read_u32_le(d,ep+16);
    PathCtlPt *ctl=calloc(npt>0?npt:1,sizeof(PathCtlPt));
    for(int k=0;k<npt;k++){ uint32_t po=ep+20+k*12;
      ctl[k].x=gml_vm_read_f32_le(d,po); ctl[k].y=gml_vm_read_f32_le(d,po+4);
      ctl[k].sp=gml_vm_read_f32_le(d,po+8); }
    path_build_samples(p,ctl,npt);
    free(ctl);
    /* cumulative arc length along the polyline (closed paths include the wrap segment) */
    double L=0; if(p->n>0) p->pts[0].clen=0;
    for(int k=1;k<p->n;k++){ double dx=p->pts[k].x-p->pts[k-1].x, dy=p->pts[k].y-p->pts[k-1].y;
      L+=sqrt(dx*dx+dy*dy); p->pts[k].clen=L; }
    if(p->closed && p->n>1){ double dx=p->pts[0].x-p->pts[p->n-1].x, dy=p->pts[0].y-p->pts[p->n-1].y;
      L+=sqrt(dx*dx+dy*dy); }
    p->len=L;
  }
}

static int native_timeline_code(GmlWin *w, const char *timeline_name, int moment, int step,
                                const uint8_t *d, size_t end, uint32_t event_ptr){
  /* Studio gives native timeline moments stable CODE names. Resolve that independent identity
   * first: it avoids depending on the EventAction record layout, which changed between format
   * generations. The action pointer remains a guarded fallback for packages which retained the
   * native TMLN graph but stripped CODE names. */
  if(timeline_name && strcmp(timeline_name,"<@?>")){
    size_t need=strlen(timeline_name)+48;
    char *name=malloc(need);
    if(name){
      snprintf(name,need,"Timeline_%s_%d",timeline_name,moment);
      int code=gml_code_index_by_name(w,name);
      if(code<0){
        snprintf(name,need,"gml_Timeline_%s_%d",timeline_name,moment);
        code=gml_code_index_by_name(w,name);
      }
      /* Some older authoring paths use the authored step in the generated CODE suffix. */
      if(code<0 && step!=moment){
        snprintf(name,need,"Timeline_%s_%d",timeline_name,step);
        code=gml_code_index_by_name(w,name);
      }
      if(code<0 && step!=moment){
        snprintf(name,need,"gml_Timeline_%s_%d",timeline_name,step);
        code=gml_code_index_by_name(w,name);
      }
      free(name);
      if(code>=0) return code;
    }
  }
  /* Native Event list: count followed by absolute pointers to EventAction records. In the
   * bc14-bc17 record shared by native object/timeline events, CodeId is the word at +32. Only
   * accept an in-range CODE index; malformed/foreign layouts leave the moment inert. */
  if((size_t)event_ptr+8<=end){
    uint32_t actions=gml_vm_read_u32_le(d,event_ptr);
    if(actions>0 && actions<=4096 && (size_t)event_ptr+4+(size_t)actions*4<=end){
      for(uint32_t i=0;i<actions;i++){
        uint32_t action_ptr=gml_vm_read_u32_le(d,event_ptr+4+i*4);
        if((size_t)action_ptr+36>end) continue;
        uint32_t code=gml_vm_read_u32_le(d,action_ptr+32);
        if(code<(uint32_t)w->n_code) return (int)code;
      }
    }
  }
  return -1;
}

/* The source compiler writes a deliberately tagged TMLC record. Native Studio packages use
 * [name,count,(step,event-list-pointer)*count]. Both are pointer- and bounds-validated here;
 * native moment code is resolved by its independent CODE name with the event graph as fallback. */
static void parse_timelines(GmlVM *vm){
  GmlWin *w=vm->win; const GmlChunk *c=gml_chunk(w,"TMLN");
  if(!c || c->size<4 || (size_t)c->off+4>w->size) return;
  const uint8_t *d=w->data; uint32_t base=c->off, n=gml_vm_read_u32_le(d,base);
  size_t end=(size_t)c->off+c->size;
  if(n>100000 || (size_t)base+4+(size_t)n*4>end || end>w->size) return;
  int cap=n>0?(int)n:4;
  GmlTimeline *timelines=calloc((size_t)cap,sizeof(*timelines));
  if(!timelines) return;
  for(uint32_t i=0;i<n;i++){
    uint32_t ep=gml_vm_read_u32_le(d,base+4+i*4);
    if((size_t)ep+8>end) continue;
    int tagged=(size_t)ep+12<=end && gml_vm_read_u32_le(d,ep+4)==0x434C4D54u;
    uint32_t count=gml_vm_read_u32_le(d,ep+(tagged?8:4));
    size_t pairs=(size_t)ep+(tagged?12:8);
    if(count>100000 || pairs+(size_t)count*8>end) continue;
    GmlTimeline *t=&timelines[i];
    t->name=gml_str_by_ptr(w,gml_vm_read_u32_le(d,ep));
    t->moments=calloc(count?count:1,sizeof(*t->moments));
    if(!t->moments) continue;
    t->n=(int)count; t->last_step=-1;
    for(uint32_t m=0;m<count;m++){
      uint32_t pair=(uint32_t)(pairs+(size_t)m*8);
      t->moments[m].step=(int32_t)gml_vm_read_u32_le(d,pair);
      t->moments[m].code=tagged ? (int32_t)gml_vm_read_u32_le(d,pair+4) :
        native_timeline_code(w,t->name,(int)m,t->moments[m].step,d,end,
                             gml_vm_read_u32_le(d,pair+4));
      if(t->moments[m].step>t->last_step) t->last_step=t->moments[m].step;
    }
    if(anygm_host_development_setting(vm->host,"GML_LOG_TIMELINE")) anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,
      "[timeline] %u %s format=%s moments=%d last=%d\n",i,t->name?t->name:"?",
      tagged?"tmlc":"native",t->n,t->last_step);
  }
  vm->timelines=timelines; vm->n_timelines=(int)n; vm->cap_timelines=cap;
}

int gml_timeline_add(GmlVM *vm){
  if(!vm) return -1;
  if(vm->n_timelines>=vm->cap_timelines){
    int cap=vm->cap_timelines>0?vm->cap_timelines*2:4;
    if(cap<=vm->n_timelines) cap=vm->n_timelines+1;
    GmlTimeline *grown=realloc(vm->timelines,(size_t)cap*sizeof(*grown));
    if(!grown) return -1;
    memset(grown+vm->cap_timelines,0,(size_t)(cap-vm->cap_timelines)*sizeof(*grown));
    vm->timelines=grown; vm->cap_timelines=cap;
  }
  int index=vm->n_timelines;
  char label[48]; snprintf(label,sizeof(label),"__newtimeline%d",index);
  size_t bytes=strlen(label)+1;
  char *name=malloc(bytes);
  if(!name) return -1;
  memcpy(name,label,bytes);
  GmlTimeline *timeline=&vm->timelines[index];
  memset(timeline,0,sizeof(*timeline));
  timeline->owned_name=name; timeline->name=name; timeline->last_step=-1;
  vm->n_timelines++;
  return index;
}

void gml_timeline_clear(GmlVM *vm, int index){
  if(!vm || index<0 || index>=vm->n_timelines || !vm->timelines[index].name) return;
  GmlTimeline *timeline=&vm->timelines[index];
  /* Keep the old allocation alive: this may be called by a moment that is currently being
   * interpreted. The generation check stops that playback before it can visit another moment. */
  timeline->n=0; timeline->last_step=-1; timeline->generation++;
}
/* evaluate a path at position t in [0,1] -> (x,y) in path-local coords */
static void path_eval_ex(GmlPath *p, double t, double *ox, double *oy, double *osp){
  if(p->n<=0){ *ox=*oy=0; if(osp) *osp=100; return; }
  if(p->n==1 || p->len<=0){ *ox=p->pts[0].x; *oy=p->pts[0].y; if(osp) *osp=p->pts[0].sp; return; }
  if(t<0) t=0;
  if(t>1) t=1;
  double target=t*p->len;
  int seg=p->closed?p->n:(p->n-1);
  for(int k=0;k<seg;k++){
    int a=k, b=(k+1)%p->n;
    double c0=p->pts[a].clen;
    double c1=(b==0)?p->len:p->pts[b].clen;
    if(target<=c1 || k==seg-1){
      double segl=c1-c0; double f=segl>0?(target-c0)/segl:0;
      *ox=p->pts[a].x+(p->pts[b].x-p->pts[a].x)*f;
      *oy=p->pts[a].y+(p->pts[b].y-p->pts[a].y)*f;
      if(osp) *osp=p->pts[a].sp+(p->pts[b].sp-p->pts[a].sp)*f;
      return;
    }
  }
  *ox=p->pts[p->n-1].x; *oy=p->pts[p->n-1].y; if(osp) *osp=p->pts[p->n-1].sp;
}
static void path_eval(GmlPath *p, double t, double *ox, double *oy){
  path_eval_ex(p,t,ox,oy,NULL);
}
void gml_path_eval_public(GmlVM *vm, int pi, double t, double *ox, double *oy){
  if(pi<0||pi>=vm->n_paths){ if(ox)*ox=0; if(oy)*oy=0; return; }
  path_eval(&vm->paths[pi],t,ox,oy);
}
static double path_speed_factor(GmlPath *p, double t){
  double x,y,sp; path_eval_ex(p,t,&x,&y,&sp);
  return isfinite(sp) ? sp : 100.0;
}
static void path_world_xy(GmlInstance *in, double px, double py, double *ox, double *oy){
  double scl=in->path_scale!=0?in->path_scale:1;
  double dx=(px-in->path_origin_x)*scl, dy=(py-in->path_origin_y)*scl;
  double a=in->path_orientation*M_PI/180.0, c=cos(a), s=sin(a);
  *ox=in->path_xoff + dx*c + dy*s;
  *oy=in->path_yoff - dx*s + dy*c;
}
/* path_start(path,speed,endaction,absolute): begin following a path. */
void gml_path_start(GmlVM *vm, GmlInstance *in, int path, double speed, double endaction, int absolute){
  if(path<0||path>=vm->n_paths) return;
  in->path_index=path; in->path_speed=speed; in->path_endaction=endaction;
  /* Each traversal starts from the authored transform. Content code may change scale or
   * orientation afterwards, but completed-traversal values do not leak into path_start.
   * A negative speed begins at the far endpoint and walks the path backwards. */
  in->path_scale=1; in->path_orientation=0;
  double start=speed<0?1.0:0.0;
  in->path_position=start; in->path_positionprevious=start;
  double px,py; path_eval(&vm->paths[path],start,&px,&py);
  if(absolute){
    in->path_xoff=0; in->path_yoff=0; in->path_origin_x=0; in->path_origin_y=0;
  } else {
    in->path_xoff=in->x; in->path_yoff=in->y; in->path_origin_x=px; in->path_origin_y=py;
  }
  path_world_xy(in,px,py,&in->x,&in->y); gml_colgrid_touch(vm,in);
}
static void path_log_step(GmlVM *vm, GmlInstance *in, int pi){
  const char *lp=anygm_host_development_setting(vm->host,"GML_LOG_PATH");
  if(!lp) return;
  const char *on=(in->obj>=0&&in->obj<vm->n_objects)?vm->objects[in->obj].name:"?";
  if(!*lp || strstr(on,lp)){ 
    anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[path] f%ld %s id=%u pi=%d p=%.3f ori=%.1f scl=%.2f @(%.2f,%.2f)\n",
            vm->frame,on,in->id,pi,in->path_position,in->path_orientation,in->path_scale,in->x,in->y);
  }
}

static void path_apply_endaction(GmlVM *vm, GmlInstance *in, int pi, int ea, double next_pos, int hit_end){
  if(pi<0||pi>=vm->n_paths) return;
  GmlPath *p=&vm->paths[pi];
  double pos=next_pos;
  if(ea==1){  /* path_action_restart: wrap to the same placed path */
    while(pos>=1) pos-=1;
    while(pos<0) pos+=1;
    in->path_position=pos;
  } else if(ea==2){  /* path_action_continue: repeat the path starting at the reached endpoint */
    double overflow=hit_end ? (next_pos-1.0) : -next_pos;
    while(overflow>=1.0) overflow-=1.0;
    if(overflow<0) overflow=0;
    double ox,oy;
    path_eval(p,hit_end?0.0:1.0,&ox,&oy);
    in->path_xoff=in->x; in->path_yoff=in->y;
    in->path_origin_x=ox; in->path_origin_y=oy;
    in->path_position=hit_end ? overflow : 1.0-overflow;
  } else if(ea==3){  /* path_action_reverse */
    double overflow=hit_end ? (next_pos-1.0) : -next_pos;
    if(overflow<0) overflow=0;
    in->path_speed=-in->path_speed;
    in->path_position=hit_end ? 1.0-overflow : overflow;
    if(in->path_position<0) in->path_position=0;
    if(in->path_position>1) in->path_position=1;
  } else {           /* path_action_stop */
    in->path_position=hit_end ? 1.0 : 0.0;
    in->path_index=-1;
    return;
  }
  double px,py; path_eval(p,in->path_position,&px,&py);
  path_world_xy(in,px,py,&in->x,&in->y); gml_colgrid_touch(vm,in);
}

/* advance every path-following instance one step (called each frame in the movement phase). */
static void run_paths(GmlVM *vm){
  /* Path interaction with ordinary motion fields differs between format families. Classic and
   * current Studio semantics suspend ordinary velocity while an active path owns movement;
   * Studio 1.x keeps those fields intact and still re-evaluates a zero-speed path after ordinary
   * movement.  Keeping the middle generation explicit is important: treating every non-classic
   * package alike shifts path-driven objects in bytecode-15 content. */
  int path_owns_velocity=anygm_policy_path_motion_owns_velocity(vm->win);
  for(int i=0;i<vm->inst_count;i++){ GmlInstance *in=&vm->inst[i];
    if(!in->active||in->marked) continue;
    int pi=(int)in->path_index; if(pi<0||pi>=vm->n_paths) continue;
    GmlPath *p=&vm->paths[pi];
    if(p->len<=0 || (path_owns_velocity && in->path_speed==0)){ continue; }
    double before_x=in->x, before_y=in->y;
    in->path_positionprevious=in->path_position;
    double scl=in->path_scale!=0?fabs(in->path_scale):1;
    double sf=path_speed_factor(p,in->path_position)/100.0;
    if(sf<0) sf=0;
    double next_pos=in->path_position + (in->path_speed*sf) / (p->len*scl); /* path point speed is a percent */
    int ended=(next_pos>=1.0 || next_pos<0.0);
    if(ended) in->path_position=(next_pos>=1.0)?1.0:0.0;
    else in->path_position=next_pos;
    double px,py,new_x,new_y; path_eval(p,in->path_position,&px,&py);
    path_world_xy(in,px,py,&new_x,&new_y);
    /* A step that advances along the path and changes position owns its motion: derive direction
     * from that displacement and clear ordinary speed components. A stationary path correction
     * retains the existing velocity, so path progress and displacement distinguish the cases. */
    if(in->path_position!=in->path_positionprevious &&
       (before_x!=new_x || before_y!=new_y)){
      in->direction=atan2(before_y-new_y,new_x-before_x)*180.0/M_PI;
      if(in->direction<0) in->direction+=360.0;
      if(in->direction>=360.0) in->direction=fmod(in->direction,360.0);
      in->speed=0; in->hspeed=0; in->vspeed=0;
    }
    in->x=new_x; in->y=new_y; gml_colgrid_touch(vm,in);
    path_log_step(vm,in,pi);
    /* GM End-of-Path event (Other, subtype 8), fired when a path reaches its end. */
    if(ended){
      uint32_t id=in->id;
      int ea=(int)in->path_endaction;
      double boundary=in->path_position;
      int hit_end=next_pos>=1.0;
      gml_run_event(vm,in,"Other_8");
      in=gml_vm_instance_by_id(vm,(double)id);   /* Other_8 may have moved/killed the instance; refetch by id */
      if(!in||!in->active||in->marked) continue;
      if((int)in->path_index==pi && (int)in->path_endaction==ea && fabs(in->path_position-boundary)<1e-9)
        path_apply_endaction(vm,in,pi,ea,next_pos,hit_end);
    }
  }
}

void gml_vm_rooms_init(GmlVM *vm){
  parse_paths(vm);
  parse_timelines(vm);
}

void gml_vm_rooms_step_paths(GmlVM *vm){
  run_paths(vm);
}
/* Resolve a room by name to its index (for a data-driven stage/warp list). -1 if not found. */
int gml_room_index_by_name(GmlWin *win, const char *name){
  if(!win||!name||!*name) return -1;
  int n=gml_room_count(win);
  for(int i=0;i<n;i++){ GmlRoom r; if(gml_room_get(win,i,&r)==0 && r.name && !strcmp(r.name,name)) return i; }
  return -1;
}

static void room_state_key(char *out, size_t cap, int room, const char *field, int index){
  snprintf(out,cap,"__gmlc_room_state_%d_%s_%d",room,field,index);
}

static void room_state_store_number(GmlVM *vm, int room, const char *field, int index, double value){
  char key[128]; room_state_key(key,sizeof(key),room,field,index);
  GmlVal *slot=gml_varmap_get(&vm->globals,key);
  if(!slot){
    char *stable=strdup(key);
    if(!stable) return;
    slot=gml_varmap_put_owned_hashed(&vm->globals,stable,gml_value_name_hash(stable));
  }
  *slot=vreal(value);
}

static double room_state_restore_number(GmlVM *vm, int room, const char *field, int index){
  char key[128]; room_state_key(key,sizeof(key),room,field,index);
  GmlVal *value=gml_varmap_get(&vm->globals,key);
  return value?gml_vm_value_as_number(*value):0.0;
}

int gml_vm_room_get(GmlVM *vm, int room_index, GmlRoom *out){
  if(!vm || gml_room_get(vm->win,room_index,out)!=0) return -1;
  double width=room_state_restore_number(vm,room_index,"width",0);
  double height=room_state_restore_number(vm,room_index,"height",0);
  if(isfinite(width) && width>=1.0 && width<=(double)UINT32_MAX) out->width=(uint32_t)width;
  if(isfinite(height) && height>=1.0 && height<=(double)UINT32_MAX) out->height=(uint32_t)height;
  return 0;
}

int gml_vm_room_set_dimension(GmlVM *vm, int room_index, int height, double value){
  if(!vm || room_index<0 || room_index>=gml_room_count(vm->win) ||
     !isfinite(value) || value<1.0 || value>(double)UINT32_MAX) return 0;
  room_state_store_number(vm,room_index,height?"height":"width",0,
                          (double)(uint32_t)value);
  return 1;
}

static const char *const room_background_fields[]={
  "background_visible","background_foreground","background_index","background_x","background_y",
  "background_htiled","background_vtiled","background_hspeed","background_vspeed","background_stretch",
  "background_alpha","background_blend"
};
static const char *const room_view_fields[]={
  "view_visible","view_xview","view_yview","view_wview","view_hview","view_xport","view_yport",
  "view_wport","view_hport","view_hborder","view_vborder","view_hspeed","view_vspeed","view_object",
  "view_camera"
};

/* Modern room-editor cameras occupy stable handles below the dynamic camera pool. Reserving
 * those handles prevents an early camera_create() from aliasing view_camera[0] before the first
 * room is entered. */
static void room_camera_resources_init(GmlVM *vm,int reset_bindings){
  if(!vm || !vm->win || !anygm_policy_has_modern_layer_semantics(vm->win)) return;
  static const char *const camera_fields[]={
    "__gml_camera_x","__gml_camera_y","__gml_camera_w","__gml_camera_h",
    "__gml_camera_angle","__gml_camera_target","__gml_camera_xspeed",
    "__gml_camera_yspeed","__gml_camera_xborder","__gml_camera_yborder"
  };
  static const char *const view_fields[]={
    "view_xview","view_yview","view_wview","view_hview",NULL,"view_object",
    "view_hspeed","view_vspeed","view_hborder","view_vborder"
  };
  for(int camera=0;camera<GML_ROOM_CAMERA_COUNT;camera++){
    double width=gml_vm_global_array_number(vm,"view_wview",camera);
    double height=gml_vm_global_array_number(vm,"view_hview",camera);
    if(reset_bindings) gml_vm_global_array_set(vm,"view_camera",camera,camera);
    gml_vm_global_array_set(vm,"__gml_camera_live",camera,width>0 && height>0);
    for(size_t field=0;field<sizeof(camera_fields)/sizeof(camera_fields[0]);field++){
      double value=0;
      if(view_fields[field])
        value=gml_vm_global_array_number(vm,view_fields[field],camera);
      gml_vm_global_array_set(vm,camera_fields[field],camera,value);
    }
    gml_vm_global_array_set(vm,"__gml_camera_matrix_eye_x",camera,0);
    gml_vm_global_array_set(vm,"__gml_camera_matrix_eye_y",camera,0);
    gml_vm_global_array_set(vm,"__gml_camera_matrix_eye_valid",camera,0);
  }
}

static void room_runtime_state_store(GmlVM *vm, int room){
  room_state_store_number(vm,room,"present",0,1);
  for(size_t field=0;field<sizeof(room_background_fields)/sizeof(room_background_fields[0]);field++)
    for(int i=0;i<8;i++) room_state_store_number(vm,room,room_background_fields[field],i,
                                                  gml_vm_global_array_number(vm,room_background_fields[field],i));
  for(size_t field=0;field<sizeof(room_view_fields)/sizeof(room_view_fields[0]);field++)
    for(int i=0;i<8;i++) room_state_store_number(vm,room,room_view_fields[field],i,
                                                  gml_vm_global_array_number(vm,room_view_fields[field],i));
  GmlVal *speed=gml_varmap_get(&vm->globals,"room_speed");
  room_state_store_number(vm,room,"room_speed",0,
                          speed?gml_vm_value_as_number(*speed):gml_room_speed(vm));
  GmlVal *view_current=gml_varmap_get(&vm->globals,"view_current");
  room_state_store_number(vm,room,"view_current",0,
                          view_current?gml_vm_value_as_number(*view_current):0);
  GmlVal *view_enabled=gml_varmap_get(&vm->globals,"view_enabled");
  room_state_store_number(vm,room,"view_enabled",0,
                          view_enabled?gml_vm_value_as_number(*view_enabled):0);
  room_state_store_number(vm,room,"tile_mut_count",0,vm->n_tile_mut);
  for(int i=0;i<vm->n_tile_mut;i++){
    room_state_store_number(vm,room,"tile_mut_depth",i,vm->tile_mut[i].depth);
    room_state_store_number(vm,room,"tile_mut_flags",i,vm->tile_mut[i].flags);
    room_state_store_number(vm,room,"tile_mut_has_remap",i,vm->tile_mut[i].has_remap);
    room_state_store_number(vm,room,"tile_mut_remap",i,vm->tile_mut[i].remap);
    room_state_store_number(vm,room,"tile_mut_dx",i,vm->tile_mut[i].dx);
    room_state_store_number(vm,room,"tile_mut_dy",i,vm->tile_mut[i].dy);
  }
  room_state_store_number(vm,room,"tile_del_count",0,vm->n_tile_del_at);
  for(int i=0;i<vm->n_tile_del_at;i++){
    room_state_store_number(vm,room,"tile_del_depth",i,vm->tile_del_at[i].depth);
    room_state_store_number(vm,room,"tile_del_x",i,vm->tile_del_at[i].x);
    room_state_store_number(vm,room,"tile_del_y",i,vm->tile_del_at[i].y);
  }
}

static void room_runtime_state_restore(GmlVM *vm, int room){
  if(room_state_restore_number(vm,room,"present",0)==0) return;
  for(size_t field=0;field<sizeof(room_background_fields)/sizeof(room_background_fields[0]);field++)
    for(int i=0;i<8;i++) gml_vm_global_array_set(vm,room_background_fields[field],i,
                                        room_state_restore_number(vm,room,room_background_fields[field],i));
  for(size_t field=0;field<sizeof(room_view_fields)/sizeof(room_view_fields[0]);field++)
    for(int i=0;i<8;i++) gml_vm_global_array_set(vm,room_view_fields[field],i,
                                        room_state_restore_number(vm,room,room_view_fields[field],i));
  *gml_varmap_put(&vm->globals,"room_speed")=vreal(room_state_restore_number(vm,room,"room_speed",0));
  *gml_varmap_put(&vm->globals,"view_current")=vreal(room_state_restore_number(vm,room,"view_current",0));
  *gml_varmap_put(&vm->globals,"view_enabled")=vreal(room_state_restore_number(vm,room,"view_enabled",0));
  vm->n_tile_mut=(int)room_state_restore_number(vm,room,"tile_mut_count",0);
  if(vm->n_tile_mut<0) vm->n_tile_mut=0;
  if(vm->n_tile_mut>64) vm->n_tile_mut=64;
  for(int i=0;i<vm->n_tile_mut;i++){
    vm->tile_mut[i].depth=(int)room_state_restore_number(vm,room,"tile_mut_depth",i);
    vm->tile_mut[i].flags=(int)room_state_restore_number(vm,room,"tile_mut_flags",i);
    vm->tile_mut[i].has_remap=(int)room_state_restore_number(vm,room,"tile_mut_has_remap",i);
    vm->tile_mut[i].remap=(int)room_state_restore_number(vm,room,"tile_mut_remap",i);
    vm->tile_mut[i].dx=room_state_restore_number(vm,room,"tile_mut_dx",i);
    vm->tile_mut[i].dy=room_state_restore_number(vm,room,"tile_mut_dy",i);
  }
  vm->n_tile_del_at=(int)room_state_restore_number(vm,room,"tile_del_count",0);
  if(vm->n_tile_del_at<0) vm->n_tile_del_at=0;
  if(vm->n_tile_del_at>64) vm->n_tile_del_at=64;
  for(int i=0;i<vm->n_tile_del_at;i++){
    vm->tile_del_at[i].depth=(int)room_state_restore_number(vm,room,"tile_del_depth",i);
    vm->tile_del_at[i].x=(int)room_state_restore_number(vm,room,"tile_del_x",i);
    vm->tile_del_at[i].y=(int)room_state_restore_number(vm,room,"tile_del_y",i);
  }
}

/* GMS2-style ROOM layers are a data-layout feature, not a bytecode-version feature. Early
 * exports can still use bytecode 15, while newer room records inserted an editor scalar before
 * the layer pointer and shifted it from +88 to +92. Detect the slot structurally inside ROOM so
 * both layouts work and classic records cannot be mistaken for a layer list. */
uint32_t gml_vm_rooms_layer_list(GmlVM *vm, int room_index, uint32_t *out_count){
  if(out_count) *out_count=0;
  if(!vm || !vm->win || room_index<0 || room_index>=gml_room_count(vm->win)) return 0;
  const GmlChunk *rc=gml_chunk(vm->win,"ROOM");
  if(!rc) return 0;
  const uint8_t *d=vm->win->data;
  uint64_t rend=(uint64_t)rc->off+rc->size;
  uint64_t slot=(uint64_t)rc->off+4u+(uint64_t)(uint32_t)room_index*4u;
  if(slot+4u>rend) return 0;
  uint32_t rp=gml_vm_read_u32_le(d,(uint32_t)slot);
  if(rp<rc->off || (uint64_t)rp+92u>rend) return 0;
  static const uint8_t layer_slots[]={88,92,96,100,104,108,112,116,120};
  for(size_t s=0;s<sizeof(layer_slots)/sizeof(layer_slots[0]);s++){
    uint32_t lo=layer_slots[s];
    if((uint64_t)rp+lo+4u>rend) break;
    uint32_t lay=gml_vm_read_u32_le(d,rp+lo);
    if(lay<rc->off || (uint64_t)lay+4u>rend) continue;
    uint32_t lcnt=gml_vm_read_u32_le(d,lay);
    if(lcnt==0 || lcnt>=512 || (uint64_t)lay+4u+(uint64_t)lcnt*4u>rend) continue;
    int valid=1;
    for(uint32_t i=0;i<lcnt;i++){
      uint32_t lp=gml_vm_read_u32_le(d,lay+4+i*4);
      if(lp<rc->off || (uint64_t)lp+36u>rend){ valid=0; break; }
      uint32_t type=gml_vm_read_u32_le(d,lp+8);
      if(type<1 || type>8 || gml_vm_read_u32_le(d,lp+32)>1){ valid=0; break; }
      uint32_t name=gml_vm_read_u32_le(d,lp);
      if(name && name>=vm->win->size){ valid=0; break; }
    }
    if(valid){
      if(out_count) *out_count=lcnt;
      return lay;
    }
  }
  return 0;
}

/* GMS2 room-layer type-data offset (+36, or +48 plus 12 bytes per effect property when the
 * extended effectEnabled/effectType/properties[] fields are present), detected once
 * per file by structurally validating background-layer records under BOTH layouts across all
 * rooms and voting. Requiring the first background layer to have an opaque color is invalid
 * because structurally valid content can begin with a transparent layer. */
static int bg_type_data_ok(const GmlWin *w, uint32_t b, int nspr){
  const uint8_t *d=w->data;
  if(b+40>w->size) return 0;
  if(gml_vm_read_u32_le(d,b)>1) return 0;                                   /* visible */
  if(gml_vm_read_u32_le(d,b+4)>1) return 0;                                 /* foreground */
  int32_t spr=(int32_t)gml_vm_read_u32_le(d,b+8);
  if(spr<-1 || spr>=nspr) return 0;                          /* sprite id */
  if(gml_vm_read_u32_le(d,b+12)>1||gml_vm_read_u32_le(d,b+16)>1||gml_vm_read_u32_le(d,b+20)>1) return 0;  /* htiled/vtiled/stretch */
  float ff=gml_vm_read_f32_le(d,b+28);
  if(!(ff>=-1.0f && ff<65536.0f)) return 0;                  /* first frame */
  if(gml_vm_read_u32_le(d,b+36)>1) return 0;                                /* animation speed type */
  return 1;
}
static int layer_effect_fields_ok(const GmlWin *w, uint32_t lp){
  const uint8_t *d=w->data;
  if(lp+48>w->size) return 0;
  if(gml_vm_read_u32_le(d,lp+36)>1) return 0;                               /* effectEnabled */
  uint32_t sp=gml_vm_read_u32_le(d,lp+40);                                  /* effectType: null or strptr */
  if(sp){ if(sp<12 || sp+1>=w->size) return 0;
    uint32_t sl=gml_vm_read_u32_le(d,sp-4); if(sl==0 || sl>256) return 0; }
  if(gml_vm_read_u32_le(d,lp+44)>64) return 0;                              /* effect property count */
  return 1;
}
int gml_room_layer_data_off(GmlVM *vm){
  if(vm->layer_data_off) return vm->layer_data_off;
  vm->layer_data_off=36;
  const GmlChunk *rc=gml_chunk(vm->win,"ROOM");
  const GmlChunk *sc=gml_chunk(vm->win,"SPRT");
  if(!rc||!sc) return vm->layer_data_off;
  const uint8_t *d=vm->win->data;
  int nspr=(int)gml_vm_read_u32_le(d,sc->off);
  int nrooms=gml_room_count(vm->win);
  int v36=0, v48=0, sampled=0;
  for(int ri=0;ri<nrooms && sampled<64;ri++){
    uint32_t lcnt=0;
    uint32_t lay=gml_vm_rooms_layer_list(vm,ri,&lcnt);
    if(!lcnt || lcnt>=512) continue;
    for(uint32_t i=0;i<lcnt && sampled<64;i++){ uint32_t lp=gml_vm_read_u32_le(d,lay+4+i*4);
      if(!lp || lp+96>vm->win->size || gml_vm_read_u32_le(d,lp+8)!=1) continue;
      sampled++;
      if(bg_type_data_ok(vm->win,lp+36,nspr)) v36++;
      if(layer_effect_fields_ok(vm->win,lp)){
        uint32_t pc=gml_vm_read_u32_le(d,lp+44);
        if(bg_type_data_ok(vm->win,lp+48+12*pc,nspr)) v48++;
      }
    }
  }
  if(v48>v36) vm->layer_data_off=48;
  return vm->layer_data_off;
}
/* Absolute start of a layer's type-specific data. Under the extended layout each layer carries
 * an inline, variable-length effect-property list, so the offset is per-layer, not per-file. */
uint32_t gml_room_layer_type_off(GmlVM *vm, uint32_t lp){
  if(gml_room_layer_data_off(vm)==36) return lp+36;
  uint32_t pc=(lp+48<=vm->win->size)?gml_vm_read_u32_le(vm->win->data,lp+44):0;
  if(pc>64) pc=0;
  return lp+48+12*pc;
}

/* ---- GMS2 tile layers (type-4 room layers) for tile-based collision ---- */
static GmlTileMap *gml_tilemap_new(GmlVM *vm){
  if(vm->n_tilemaps>=vm->cap_tilemaps){ int nc=vm->cap_tilemaps?vm->cap_tilemaps*2:8;
    GmlTileMap *nt=realloc(vm->tilemaps,(size_t)nc*sizeof(*nt)); if(!nt) return NULL; vm->tilemaps=nt; vm->cap_tilemaps=nc; }
  GmlTileMap *t=&vm->tilemaps[vm->n_tilemaps++]; memset(t,0,sizeof *t);
  if(vm->next_tilemap_id<2000001) vm->next_tilemap_id=2000001;
  t->id=vm->next_tilemap_id++; t->used=1; t->visible=1;
  return t;
}
void gml_vm_rooms_clear_tilemaps(GmlVM *vm){
  for(int i=0;i<vm->n_tilemaps;i++){
    free(vm->tilemaps[i].owned_tiles);
    free(vm->tilemaps[i].decoded_tiles);
    vm->tilemaps[i].owned_tiles=NULL;
    vm->tilemaps[i].decoded_tiles=NULL;
  }
  vm->n_tilemaps=0;
}

/* Read enough tileset metadata to distinguish the separated-border layout, which also uses
 * compressed room grids, from the older BGND record. */
static int gml_tileset_meta(GmlVM *vm,int tileset,int *tile_count,int *modern){
  if(tile_count) *tile_count=0;
  if(modern) *modern=0;
  const GmlChunk *bc=gml_chunk(vm->win,"BGND");
  if(!bc || tileset<0 || (uint32_t)tileset>=gml_vm_read_u32_le(vm->win->data,bc->off)) return 0;
  const uint8_t *d=vm->win->data;
  uint32_t bp=gml_vm_read_u32_le(d,bc->off+4+(uint32_t)tileset*4u);
  if(!bp || (uint64_t)bp+72u>(uint64_t)bc->off+bc->size) return 0;
  int oi=(int)gml_vm_read_u32_le(d,bp+44), oc=(int)gml_vm_read_u32_le(d,bp+48), ocols=(int)gml_vm_read_u32_le(d,bp+40);
  uint64_t obytes=(uint64_t)(uint32_t)oi*(uint64_t)(uint32_t)oc*4u;
  int old_ok=ocols>0&&ocols<=4096&&oi>0&&oi<=1024&&oc>0&&obytes<=4000000u&&
             (uint64_t)bp+64u+obytes<=(uint64_t)bc->off+bc->size;
  int ni=(int)gml_vm_read_u32_le(d,bp+52), nc=(int)gml_vm_read_u32_le(d,bp+56), ncols=(int)gml_vm_read_u32_le(d,bp+48);
  uint64_t nbytes=(uint64_t)(uint32_t)ni*(uint64_t)(uint32_t)nc*4u;
  int new_ok=gml_vm_read_u32_le(d,bp+32)<=4096&&gml_vm_read_u32_le(d,bp+36)<=4096&&gml_vm_read_u32_le(d,bp+40)<=4096&&gml_vm_read_u32_le(d,bp+44)<=4096&&
             ncols>0&&ncols<=4096&&ni>0&&ni<=1024&&nc>0&&nbytes<=4000000u&&
             (uint64_t)bp+72u+nbytes<=(uint64_t)bc->off+bc->size;
  int is_new=new_ok&&(!old_ok||(nc>oc&&ncols>ocols));
  if(tile_count) *tile_count=is_new?nc:(old_ok?oc:0);
  if(modern) *modern=is_new;
  return is_new||old_ok;
}

static void put_u32le(unsigned char *p,uint32_t v){
  p[0]=(unsigned char)v; p[1]=(unsigned char)(v>>8); p[2]=(unsigned char)(v>>16); p[3]=(unsigned char)(v>>24);
}

/* Decode the byte-run grid format. A high opcode repeats one u32 1..128
 * times; a low opcode copies that many literal u32 values. */
static unsigned char *gml_tile_rle_decode(const uint8_t *src,size_t avail,size_t count,size_t *used){
  if(used) *used=0;
  if(!src || !count || count>16000000u || count>SIZE_MAX/4u) return NULL;
  unsigned char *out=malloc(count*4u); if(!out) return NULL;
  size_t ip=0,op=0;
  while(op<count){
    if(ip>=avail){ free(out); return NULL; }
    unsigned code=src[ip++];
    if(code>=128){
      size_t run=(code&127u)+1u;
      if(ip+4u>avail || run>count-op){ free(out); return NULL; }
      uint32_t v=gml_vm_read_u32_le(src,(uint32_t)ip); ip+=4;
      for(size_t k=0;k<run;k++) put_u32le(out+(op+k)*4u,v);
      op+=run;
    } else {
      size_t run=code;
      if(run==0 || run>count-op || run>(avail-ip)/4u){ free(out); return NULL; }
      memcpy(out+op*4u,src+ip,run*4u); ip+=run*4u; op+=run;
    }
  }
  /* Some GMAC builds append a run of two -1 padding cells when the final two real cells differ. */
  if(count>1 && memcmp(out+(count-1)*4u,out+(count-2)*4u,4)!=0 && ip+5u<=avail &&
     src[ip]==0x81u && gml_vm_read_u32_le(src,(uint32_t)ip+1u)==0xFFFFFFFFu) ip+=5u;
  if(used) *used=ip;
  return out;
}

/* Count the cells of a candidate grid that could have been authored: a cell names a tile inside its
 * tileset and leaves the bits between the index and the mirror/flip/rotate flags clear.
 *
 * The two encodings are not distinguishable from the tileset record. A file pairs either one with
 * either background layout, so the record's shape answers a different question than the one asked
 * here, and reading a run-length stream as one word per cell yields a grid of plausible-looking
 * indices that address the wrong tiles. Scoring both readings separates them: an encoding that
 * matches explains every cell, and one that does not leaves most of them unaddressable. */
static size_t tile_grid_addressable(const unsigned char *grid,size_t cells,int tile_count){
  size_t addressable=0;
  for(size_t cell=0;cell<cells;cell++){
    uint32_t datum=gml_vm_read_u32_le(grid,(uint32_t)(cell*4u));
    if((datum&0x7FFFFu)<=(uint32_t)tile_count && !(datum&0x0FF80000u)) addressable++;
  }
  return addressable;
}
GmlTileMap *gml_tilemap_find(GmlVM *vm, int id){
  for(int i=0;i<vm->n_tilemaps;i++) if(vm->tilemaps[i].used && vm->tilemaps[i].id==id) return &vm->tilemaps[i];
  return NULL;
}
int gml_vm_tilemap_ensure_owned(GmlTileMap *tm){
  if(!tm || tm->cols<=0 || tm->rows<=0 || !tm->tiles) return 0;
  if(tm->owned_tiles) return 1;
  if(!tm->base_tiles) tm->base_tiles=tm->tiles;
  size_t n=(size_t)tm->cols*(size_t)tm->rows*4u;
  if(n==0 || n>64u*1024u*1024u) return 0;
  unsigned char *p=malloc(n);
  if(!p) return 0;
  memcpy(p,tm->tiles,n);
  tm->owned_tiles=p;
  tm->tiles=p;
  return 1;
}
int gml_tilemap_set_cell(GmlTileMap *tm, int cx, int cy, uint32_t datum){
  if(!tm || cx<0 || cy<0 || cx>=tm->cols || cy>=tm->rows) return 0;
  if(!gml_vm_tilemap_ensure_owned(tm)) return 0;
  unsigned char *p=tm->owned_tiles+((size_t)cy*(size_t)tm->cols+(size_t)cx)*4u;
  p[0]=(unsigned char)(datum&0xFFu);
  p[1]=(unsigned char)((datum>>8)&0xFFu);
  p[2]=(unsigned char)((datum>>16)&0xFFu);
  p[3]=(unsigned char)((datum>>24)&0xFFu);
  return 1;
}
/* find the tile layer backing a given layer id OR name (layer_tilemap_get_id accepts either) */
GmlTileMap *gml_tilemap_by_layer(GmlVM *vm, GmlVal v){
  if(v.t==V_STR && v.s){ for(int i=0;i<vm->n_tilemaps;i++) if(vm->tilemaps[i].used && !strcmp(vm->tilemaps[i].name,v.s)) return &vm->tilemaps[i]; return NULL; }
  int lid=(int)gml_vm_value_as_number(v);
  GmlRtLayer *rl=gml_rt_layer_find(vm,lid);   /* a layer id → match tilemap by that layer's name */
  if(rl){ for(int i=0;i<vm->n_tilemaps;i++) if(vm->tilemaps[i].used && !strcmp(vm->tilemaps[i].name,rl->name)) return &vm->tilemaps[i]; }
  return gml_tilemap_find(vm,lid);            /* or it's already a tilemap id */
}
void gml_tilemap_effective(GmlVM *vm, const GmlTileMap *tm,
                           double *x, double *y, double *depth, int *visible){
  double ex=tm?tm->x:0.0, ey=tm?tm->y:0.0, ed=tm?tm->depth:0.0;
  int ev=tm?tm->visible:0;
  if(vm && tm && tm->name[0]){
    GmlRtLayer *rl=gml_rt_layer_find_by_name(vm,tm->name);
    if(rl){
      ev=rl->visible;
      ed=rl->depth;
      if(rl->touched){
        ex=rl->x; ey=rl->y;
      }else{
        
        long fin=vm->frame - vm->room_enter_frame; if(fin<0) fin=0;
        ex=rl->x + rl->hs*fin;
        ey=rl->y + rl->vs*fin;
      }
    }
  }
  if(x) *x=ex;
  if(y) *y=ey;
  if(depth) *depth=ed;
  if(visible) *visible=ev;
}

static int rt_layer_has_sprite_elem(GmlVM *vm, int layer_id, const char *name){
  if(!vm || !name || !*name) return 0;
  for(int i=0;i<vm->n_rte;i++){
    GmlRtElem *e=&vm->rte[i];
    if(e->used && e->type==3 && e->layer==layer_id && !strcmp(e->name,name)) return 1;
  }
  return 0;
}

/* Bind immutable GMS2 room-background records to the runtime element API. Converted projects use
 * layer_get_all_elements/layer_background_* to implement the legacy background[] compatibility
 * functions; registering only the parent layer makes those functions report that the background
 * does not exist even though it is visibly drawn from the ROOM record. */
static void gml_room_bind_backgrounds(GmlVM *vm, int room_index){
  if(!vm || !vm->win || room_index<0) return;
  const uint8_t *rd=vm->win->data;
  uint32_t lcnt=0;
  uint32_t lay=gml_vm_rooms_layer_list(vm,room_index,&lcnt);
  if(!lay || lcnt>=512) return;
  for(uint32_t i=0;i<lcnt;i++){
    uint32_t lp=gml_vm_read_u32_le(rd,lay+4+i*4);
    if(!lp || gml_vm_read_u32_le(rd,lp+8)!=1) continue;
    uint32_t np=gml_vm_read_u32_le(rd,lp);
    const char *lname=(np && np<vm->win->size)?(const char*)(rd+np):"";
    GmlRtLayer *rl=gml_rt_layer_find_by_name(vm,lname);
    if(!rl) continue;
    int exists=0;
    for(int j=0;j<vm->n_rte;j++){
      GmlRtElem *old=&vm->rte[j];
      if(old->used && old->type==1 && old->layer==rl->id){ exists=1; break; }
    }
    if(exists) continue;
    uint32_t b=gml_room_layer_type_off(vm,lp);
    if(b+40>vm->win->size) continue;
    GmlRtElem *e=gml_rt_elem_new(vm);
    if(!e) return;
    e->type=1;
    e->layer=rl->id;
    snprintf(e->name,sizeof e->name,"%s",lname);
    e->visible=gml_vm_read_u32_le(rd,b)?1:0;
    e->sprite=(int32_t)gml_vm_read_u32_le(rd,b+8);
    e->htiled=(int)gml_vm_read_u32_le(rd,b+12);
    e->vtiled=(int)gml_vm_read_u32_le(rd,b+16);
    e->stretch=(int)gml_vm_read_u32_le(rd,b+20);
    uint32_t col=gml_vm_read_u32_le(rd,b+24);
    e->blend=col&0xFFFFFFu;
    e->alpha=((col>>24)&0xFF)/255.0;
    e->image_index=gml_vm_read_f32_le(rd,b+28);
    e->image_speed=gml_vm_read_f32_le(rd,b+32);
    if(anygm_host_development_setting(vm->host,"GML_LOG_RTL")){
      
      anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[rtl] f%ld bind background layer='%s' lid=%d eid=%d sprite=%d\n",
              vm->frame,lname,rl->id,e->id,e->sprite);
    }
  }
}

static void gml_room_bind_asset_sprites(GmlVM *vm, int room_index){
  if(!vm || !vm->win || room_index<0) return;
  const uint8_t *rd=vm->win->data;
  uint32_t lcnt=0;
  uint32_t lay=gml_vm_rooms_layer_list(vm,room_index,&lcnt);
  if(!lcnt || lcnt>=512) return;
  for(uint32_t i=0;i<lcnt;i++){
    uint32_t lp=gml_vm_read_u32_le(rd,lay+4+i*4);
    if(!lp || gml_vm_read_u32_le(rd,lp+8)!=3) continue; /* Assets */
    uint32_t tb=gml_room_layer_type_off(vm,lp);
    if(tb+8>vm->win->size) continue;
    uint32_t np=gml_vm_read_u32_le(rd,lp+0);
    const char *lname=(np&&np<vm->win->size)?(const char*)(rd+np):"";
    GmlRtLayer *rl=gml_rt_layer_find_by_name(vm,lname);
    if(!rl) continue;
    uint32_t sprites=gml_vm_read_u32_le(rd,tb+4);          /* LayerAssetsData.Sprites */
    uint32_t scnt=(sprites && sprites+4<vm->win->size)?gml_vm_read_u32_le(rd,sprites):0;
    if(scnt>100000) continue;
    for(uint32_t k=0;k<scnt;k++){
      uint32_t sprec=gml_vm_read_u32_le(rd,sprites+4+k*4);
      if(!sprec || sprec+44>vm->win->size) continue;
      uint32_t nmp=gml_vm_read_u32_le(rd,sprec+0);
      const char *enm=(nmp&&nmp<vm->win->size)?(const char*)(rd+nmp):"";
      char fallback_name[64];
      if(!*enm){
        snprintf(fallback_name,sizeof fallback_name,"__gms2_asset_%u_%u",i,k);
        enm=fallback_name;
      }
      if(rt_layer_has_sprite_elem(vm,rl->id,enm)) continue;
      GmlRtElem *e=gml_rt_elem_new(vm);
      if(!e) return;
      e->type=3;
      e->layer=rl->id;
      snprintf(e->name,sizeof e->name,"%s",enm);
      e->sprite=(int32_t)gml_vm_read_u32_le(rd,sprec+4);
      e->x=(double)(int32_t)gml_vm_read_u32_le(rd,sprec+8);
      e->y=(double)(int32_t)gml_vm_read_u32_le(rd,sprec+12);
      e->xs=gml_vm_read_f32_le(rd,sprec+16);
      e->ys=gml_vm_read_f32_le(rd,sprec+20);
      uint32_t col=gml_vm_read_u32_le(rd,sprec+24);
      e->blend=col&0xFFFFFFu;
      e->alpha=((col>>24)&0xFF)/255.0;
      e->image_speed=gml_vm_read_f32_le(rd,sprec+28);
      e->image_index=gml_vm_read_f32_le(rd,sprec+36);
      e->image_angle=gml_vm_read_f32_le(rd,sprec+40);
    }
  }
}

/* Rebuild a room's derived GMS2 layer data from immutable ROOM records. Runtime layers/elements are
 * rebuilt on room enter; after modern savestate loads they are preserved and only type-4 tilemap
 * views are rebound to win data, since those grids are not serialized by pointer. */
void gml_vm_room_reload_layers_mode(GmlVM *vm, int room_index, int rebuild_runtime_layers){
  if(rebuild_runtime_layers){
    /* Per-layer shader handles use serialized globals so rewind needs no state-format fork.  A
     * genuine room rebuild owns a new layer set and must not inherit the previous room's slot. */
    for(int i=0;i<vm->n_rtl;i++) gml_set_global_arr(vm,"__gml_layer_shader",i,0);
    vm->n_rtl=0; vm->n_rte=0;
  }
  const uint8_t *rd=vm->win->data;
  uint32_t lcnt=0;
  uint32_t lay=gml_vm_rooms_layer_list(vm,room_index,&lcnt);
  if(!lay) return;
  if(rebuild_runtime_layers && lcnt<512) for(uint32_t i=0;i<lcnt;i++){ uint32_t lp=gml_vm_read_u32_le(rd,lay+4+i*4);
    if(!lp || lp+40>vm->win->size) continue;
    uint32_t np=gml_vm_read_u32_le(rd,lp+0); if(!np || np>=vm->win->size) continue;
    GmlRtLayer *l=gml_rt_layer_new(vm); if(!l) break;
    snprintf(l->name,sizeof l->name,"%s",(const char*)(rd+np));
    l->order=(int)i;
    l->depth=(double)(int32_t)gml_vm_read_u32_le(rd,lp+12);
    l->x=gml_vm_read_f32_le(rd,lp+16); l->y=gml_vm_read_f32_le(rd,lp+20); l->hs=gml_vm_read_f32_le(rd,lp+24); l->vs=gml_vm_read_f32_le(rd,lp+28);
    l->visible=gml_vm_read_u32_le(rd,lp+32)?1:0; l->touched=0;
  }
  if(lcnt<512){
    if(rebuild_runtime_layers)
      for(int ii=0; ii<vm->inst_count; ii++){
        vm->inst[ii].draw_layer_order=-1;
        vm->inst[ii].draw_layer_element_order=-1;
      }
    for(uint32_t i=0;i<lcnt;i++){ uint32_t lp=gml_vm_read_u32_le(rd,lay+4+i*4);
      if(!lp || lp+40>vm->win->size) continue;
      uint32_t np=gml_vm_read_u32_le(rd,lp+0);
      if(np && np<vm->win->size){
        GmlRtLayer *rl=gml_rt_layer_find_by_name(vm,(const char*)(rd+np));
        if(rl) rl->order=(int)i;
      }
    }
  }
  gml_vm_rooms_clear_tilemaps(vm);
  if(lcnt<512) for(uint32_t i=0;i<lcnt;i++){ uint32_t lp=gml_vm_read_u32_le(rd,lay+4+i*4);
    if(!lp || gml_vm_read_u32_le(rd,lp+8)!=2) continue;
    uint32_t tb=gml_room_layer_type_off(vm,lp);
    if(tb+4>vm->win->size) continue;
    uint32_t ic=gml_vm_read_u32_le(rd,tb);
    if(ic>100000) continue;
    int ord=(int)i;
    uint32_t np=gml_vm_read_u32_le(rd,lp+0);
    if(np && np<vm->win->size){
      GmlRtLayer *rl=gml_rt_layer_find_by_name(vm,(const char*)(rd+np));
      if(rl) ord=rl->order;
    }
    for(uint32_t k=0;k<ic;k++){
      uint32_t ip2=tb+4+k*4;
      if(ip2+4>vm->win->size) break;
      uint32_t iid=gml_vm_read_u32_le(rd,ip2);
      for(int ii=0; ii<vm->inst_count; ii++)
        if((vm->inst[ii].active || vm->inst[ii].deactivated) && vm->inst[ii].id==iid){
          vm->inst[ii].draw_layer_order=ord;
          vm->inst[ii].draw_layer_element_order=(int)k;
          break;
        }
    }
  }
  const GmlChunk *bc = gml_chunk(vm->win,"BGND");
  uint32_t bcnt = bc ? gml_vm_read_u32_le(rd,bc->off) : 0;
  if(lcnt<512) for(uint32_t i=0;i<lcnt;i++){ uint32_t lp=gml_vm_read_u32_le(rd,lay+4+i*4);
    if(!lp || gml_vm_read_u32_le(rd,lp+8)!=4) continue;          /* layer type 4 = tile layer */
    uint32_t tb=gml_room_layer_type_off(vm,lp);
    if(tb+12>vm->win->size) continue;
    int tileset=(int32_t)gml_vm_read_u32_le(rd,tb);
    int cols=(int32_t)gml_vm_read_u32_le(rd,tb+4), rows=(int32_t)gml_vm_read_u32_le(rd,tb+8);
    if(cols<=0||rows<=0||cols>8192||rows>8192) continue;
    uint32_t tdata=tb+12;
    size_t cells=(size_t)cols*(size_t)rows;
    int modern_tiles=0,tileset_tiles=0;
    (void)gml_tileset_meta(vm,tileset,&tileset_tiles,&modern_tiles);
    int plain_fits=(uint64_t)tdata + (uint64_t)cells*4u <= vm->win->size;
    unsigned char *decoded=NULL;
    {
      size_t used=0;
      unsigned char *run=gml_tile_rle_decode(rd+tdata,vm->win->size-tdata,cells,&used);
      if(run){
        int keep;
        if(tileset_tiles>0 && plain_fits)
          keep=tile_grid_addressable(run,cells,tileset_tiles)>
               tile_grid_addressable(rd+tdata,cells,tileset_tiles);
        else if(tileset_tiles>0) keep=1;      /* only the run-length reading stays in bounds */
        else keep=modern_tiles;               /* nothing to score against: keep the record's hint */
        if(keep) decoded=run; else free(run);
      }
    }
    if(!decoded && !plain_fits) continue;
    int tw=16,th=16;
    if(bc && tileset>=0 && (uint32_t)tileset<bcnt){ uint32_t bp=gml_vm_read_u32_le(rd,bc->off+4+tileset*4);
      if(bp && bp+32<vm->win->size){ int w=(int32_t)gml_vm_read_u32_le(rd,bp+24),h=(int32_t)gml_vm_read_u32_le(rd,bp+28);
        if(w>0) tw=w;
        if(h>0) th=h; } }
    GmlTileMap *tm=gml_tilemap_new(vm); if(!tm){ free(decoded); break; }
    uint32_t np2=gml_vm_read_u32_le(rd,lp+0);
    snprintf(tm->name,sizeof tm->name,"%s",(np2&&np2<vm->win->size)?(const char*)(rd+np2):"");
    tm->tileset=tileset;
    tm->depth=(double)(int32_t)gml_vm_read_u32_le(rd,lp+12);
    tm->order=(int)i;
    tm->tw=tw; tm->th=th; tm->cols=cols; tm->rows=rows;
    tm->decoded_tiles=decoded;
    tm->tiles=decoded?decoded:rd+tdata;
    tm->base_tiles=tm->tiles;
    tm->x=gml_vm_read_f32_le(rd,lp+16); tm->y=gml_vm_read_f32_le(rd,lp+20); tm->visible=gml_vm_read_u32_le(rd,lp+32)?1:0;
      GmlRtLayer *rl=gml_rt_layer_find_by_name(vm,tm->name);
      if(rl){
        tm->visible=rl->visible;
        tm->depth=rl->depth;
        tm->order=rl->order;
        if(rl->touched){ tm->x=rl->x; tm->y=rl->y; }
      }
	  }
  gml_room_bind_backgrounds(vm, room_index);
  gml_room_bind_asset_sprites(vm, room_index);
  if(anygm_host_development_setting(vm->host,"GML_LOG_ROOM")){ anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[room] reload_layers room=%d: %d layers, %d tilemaps\n",room_index,vm->n_rtl,vm->n_tilemaps);
    for(int t=0;t<vm->n_tilemaps;t++){ GmlTileMap *tm=&vm->tilemaps[t];
      int solid=0; for(int c=0;c<tm->cols*tm->rows;c++){ uint32_t d=gml_vm_read_u32_le(tm->tiles,(uint32_t)c*4); if((d&0x7FFFF)!=0) solid++; }
      anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[tile]  tm[%d] name='%s' cols=%d rows=%d cell=%dx%d pos=%.0f,%.0f solid=%d\n",t,tm->name,tm->cols,tm->rows,tm->tw,tm->th,tm->x,tm->y,solid);
      if(anygm_host_development_setting(vm->host,"GML_DUMP_TILE_VALUES")){
        int shown=0;
        anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[tile-values t%d]",t);
        for(int ry=0;ry<tm->rows && shown<24;ry++) for(int rx=0;rx<tm->cols && shown<24;rx++){
          uint32_t datum=gml_vm_read_u32_le(tm->tiles,(uint32_t)(ry*tm->cols+rx)*4);
          if((datum&0x7FFFFu)!=0){ anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG," %d:%d=%08x",rx,ry,datum); shown++; }
        }
        anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"\n");
      }
      if(anygm_host_development_setting(vm->host,"GML_DUMP_GRID")){ for(int ry=0;ry<tm->rows && ry<40;ry++){ char line[200]; int lp=0;
        for(int rx=0;rx<tm->cols && rx<128;rx++){ uint32_t d=gml_vm_read_u32_le(tm->tiles,(uint32_t)(ry*tm->cols+rx)*4); line[lp++]=((d&0x7FFFF)!=0)?'#':'.'; }
        line[lp]=0; anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[grid t%d r%02d] %s\n",t,ry,line); } } }
  }
}
static void gml_room_reload_layers(GmlVM *vm, int room_index){
  gml_vm_room_reload_layers_mode(vm,room_index,1);
}

/* queue background decodes for every atlas this room's content can touch: instance sprites
 * and masks, GMS2 tile layers, and runtime layer elements. Draws still decode synchronously
 * if a texture arrives before its prefetch finishes, so this only removes stalls. */
void gml_vm_prefetch_room_assets(GmlVM *vm){
  GmlRender *R=(GmlRender*)vm->render; if(!R) return;
  int warm = 1;
  const char *we = anygm_host_development_setting(vm->host,"GML_ATLAS_ROOM_WARM");
  if(we && (!strcmp(we,"0") || !strcmp(we,"off") || !strcmp(we,"false"))) warm = 0;
  for(int i=0;i<vm->inst_count;i++){ GmlInstance *in=&vm->inst[i];
    if(!in->active || in->marked) continue;
    gml_render_prefetch_sprite(R,(int)in->sprite_index);
    if((int)in->mask_index>=0) gml_render_prefetch_sprite(R,(int)in->mask_index);
    if(warm){
      gml_render_warm_sprite(R,(int)in->sprite_index);
      if((int)in->mask_index>=0) gml_render_warm_sprite(R,(int)in->mask_index);
    }
  }
  for(int i=0;i<vm->n_tilemaps;i++){ GmlTileMap *tm=&vm->tilemaps[i];
    if(tm->used){
      gml_render_prefetch_bg(R,tm->tileset);
      if(warm) gml_render_warm_bg(R,tm->tileset);
    } }
  for(int j=0;j<vm->n_rte;j++){ GmlRtElem *e=&vm->rte[j];
    if(!e->used) continue;
    if(e->type==7){
      gml_render_prefetch_bg(R,e->sprite);
      if(warm) gml_render_warm_bg(R,e->sprite);
    } else {
      gml_render_prefetch_sprite(R,e->sprite);
      if(warm) gml_render_warm_sprite(R,e->sprite);
    }
  }
}
static int vm_audio_room_warm_disabled(GmlVM *vm){
  if(vm->diagnostics.audio_room_warm_disabled<0){
    const char *e=anygm_host_development_setting(vm->host,"GML_AUDIO_ROOM_WARM");
    vm->diagnostics.audio_room_warm_disabled=e && (!strcmp(e,"0") || !strcmp(e,"off") || !strcmp(e,"false"));
  }
  return vm->diagnostics.audio_room_warm_disabled;
}
static int vm_audio_room_warm_debug(GmlVM *vm){
  if(vm->diagnostics.audio_room_warm_debug<0)
    vm->diagnostics.audio_room_warm_debug=anygm_host_development_setting(vm->host,"GML_DBG_AUDIO_ROOM_WARM")!=NULL;
  return vm->diagnostics.audio_room_warm_debug;
}
static int insn_push_var_named(const GmlInsn *in, const char *name){
  return in && name && in->kind==OP_PUSH && in->type1==DT_VAR && in->refname &&
         !strcmp(in->refname,name);
}
static int insn_push_int_const(const GmlInsn *in, int *out){
  if(!in || in->kind!=OP_PUSH || !out) return 0;
  double d=0.0;
  if(in->type1==DT_INT16) d=(double)in->sval;
  else if(in->type1==DT_INT32) d=(double)in->ival;
  else if(in->type1==DT_INT64) d=(double)in->lval;
  else if(in->type1==DT_DOUBLE) d=in->dval;
  else if(in->type1==DT_BOOL) d=(double)(in->ival!=0);
  else return 0;
  if(!isfinite(d)) return 0;
  int iv=(int)(d<0.0?d-0.5:d+0.5);
  if(fabs(d-(double)iv)>1e-6) return 0;
  *out=iv;
  return 1;
}
static int insn_audio_call_direct_sound(const GmlInsn *in){
  if(!in || in->kind!=OP_CALL || !in->refname) return 0;
  return !strcmp(in->refname,"audio_play_sound") ||
         !strcmp(in->refname,"sound_play") ||
         !strcmp(in->refname,"audio_play_sound_at");
}
static int code_audio_call_sound_const(const GmlCode *c, int call_i, int *sound){
  if(!c || !c->insn || call_i<=0 || call_i>=(int)c->n_insn ||
     !insn_audio_call_direct_sound(&c->insn[call_i])) return 0;
  for(int j=call_i-1, scanned=0; j>=0 && scanned<16; j--, scanned++){
    const GmlInsn *in=&c->insn[j];
    if(in->kind==OP_CONV) continue;
    return insn_push_int_const(in,sound);
  }
  return 0;
}
static int code_room_eq_at(const GmlCode *c, int cmp_i, int room_index){
  if(!c || !c->insn || cmp_i<2 || cmp_i>=(int)c->n_insn) return 0;
  const GmlInsn *a=&c->insn[cmp_i-2], *b=&c->insn[cmp_i-1], *cmp=&c->insn[cmp_i];
  if(cmp->kind!=OP_CMP || cmp->cmp!=CMP_EQ) return 0;
  int room_const=INT_MIN;
  if(insn_push_var_named(a,"room") && insn_push_int_const(b,&room_const)) return room_const==room_index;
  if(insn_push_int_const(a,&room_const) && insn_push_var_named(b,"room")) return room_const==room_index;
  return 0;
}
static void vm_warm_audio_code_for_room(GmlVM *vm, int ci, int room_index){
  if(!vm || !vm->win || !vm->audio || room_index<0 || ci<0 || ci>=vm->win->n_code) return;
  if(!gml_vm_code_cache_ensure(vm->win,ci)) return;
  GmlCode *c=&vm->win->code[ci];
  if(!c->insn || !c->branch_index) return;
  for(int i=2;i+1<(int)c->n_insn;i++){
    if(!code_room_eq_at(c,i,room_index)) continue;
    if(c->insn[i+1].kind!=OP_BF) continue;
    int end=c->branch_index[i+1];
    if(end<=i+1 || end>(int)c->n_insn) end=(int)c->n_insn;
    for(int j=i+2;j<end;j++){
      int snd=-1;
      if(code_audio_call_sound_const(c,j,&snd)){
        if(vm_audio_room_warm_debug(vm))
          anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[audio-warm-code] room=%d code=%s call=%d sound=%d\n",
                  room_index,c->name?c->name:"?",j,snd);
        gml_audio_warm_sound((GmlAudio*)vm->audio,snd);
      }
    }
  }
}
static int vm_room_order_neighbor(GmlVM *vm, int room_index, int delta){
  if(!vm || !vm->win || !vm->win->room_order || vm->win->n_room_order<=0) return -1;
  for(int i=0;i<vm->win->n_room_order;i++){
    if((int)vm->win->room_order[i]!=room_index) continue;
    int j=i+delta;
    return (j>=0 && j<vm->win->n_room_order) ? (int)vm->win->room_order[j] : -1;
  }
  return -1;
}
static void vm_warm_audio_object_alarm_codes(GmlVM *vm, int obj, int room_index){
  if(!vm || obj<0 || obj>=vm->n_objects) return;
  for(int a=0;a<GML_ALARMS;a++){
    char suffix[16];
    snprintf(suffix,sizeof suffix,"Alarm_%d",a);
    int ci=-1;
    if(gml_vm_instances_event_lookup(vm,suffix,obj,NULL,&ci)) vm_warm_audio_code_for_room(vm,ci,room_index);
  }
}
static void vm_warm_audio_room_placed_alarm_codes(GmlVM *vm, int room_index){
  if(!vm || !vm->win || room_index<0) return;
  GmlRoom r;
  if(gml_vm_room_get(vm,room_index,&r)!=0 || !r.obj_ptr) return;
  const uint8_t *d=vm->win->data;
  if(r.obj_ptr+4>vm->win->size) return;
  uint32_t cnt=gml_vm_read_u32_le(d,r.obj_ptr);
  if(cnt>100000) return;
  for(uint32_t i=0;i<cnt;i++){
    uint32_t ip=gml_vm_read_u32_le(d,r.obj_ptr+4+i*4);
    if(!ip || ip+12>vm->win->size) continue;
    vm_warm_audio_object_alarm_codes(vm,(int32_t)gml_vm_read_u32_le(d,ip+8),room_index);
  }
}
static int vm_audio_room_global_scan_already_done(GmlVM *vm, int room_index){
  if(!vm || !vm->win || room_index<0) return 1;
  int nr=gml_room_count(vm->win);
  if(nr<=0 || room_index>=nr) return 1;
  if(vm->audio_room_warm_scan_n!=nr){
    free(vm->audio_room_warm_scan);
    vm->audio_room_warm_scan=calloc((size_t)nr,1);
    vm->audio_room_warm_scan_n=vm->audio_room_warm_scan?nr:0;
  }
  if(!vm->audio_room_warm_scan) return 0;
  if(vm->audio_room_warm_scan[room_index]) return 1;
  vm->audio_room_warm_scan[room_index]=1;
  return 0;
}
static void vm_warm_audio_global_code_for_room(GmlVM *vm, int room_index){
  if(!vm || !vm->win || room_index<0) return;
  if(vm_audio_room_global_scan_already_done(vm,room_index)) return;
  for(int ci=0;ci<vm->win->n_code;ci++) vm_warm_audio_code_for_room(vm,ci,room_index);
}
void gml_vm_warm_audio_for_room(GmlVM *vm, int room_index){
  if(!vm || !vm->win || !vm->audio || room_index<0 || vm_audio_room_warm_disabled(vm)) return;
  vm_warm_audio_global_code_for_room(vm,room_index);
  for(int i=0;i<vm->inst_count;i++){
    GmlInstance *in=&vm->inst[i];
    if(!in->active || in->marked || in->obj<0 || in->obj>=vm->n_objects) continue;
    vm_warm_audio_object_alarm_codes(vm,in->obj,room_index);
  }
  vm_warm_audio_room_placed_alarm_codes(vm,room_index);
}
void gml_vm_warm_audio_for_room_window(GmlVM *vm){
  if(!vm || vm_audio_room_warm_disabled(vm)) return;
  int rooms[3]={ vm->room_index,
                 vm_room_order_neighbor(vm,vm->room_index,1),
                 vm_room_order_neighbor(vm,vm->room_index,-1) };
  for(int i=0;i<3;i++){
    int room=rooms[i];
    if(room<0) continue;
    int seen=0;
    for(int j=0;j<i;j++) if(rooms[j]==room) seen=1;
    if(!seen) gml_vm_warm_audio_for_room(vm,room);
  }
}
void gml_room_enter(GmlVM *vm, int room_index){
  gml_colgrid_invalidate(vm);
  int prev_room=vm->room_index;
  /* Room lifecycle events are engine-dispatched. Do not let a stale `other` from the GML
   * context that requested the transition leak into room Create/Start events; room-placed
   * instances have no creator/collision partner. */
  vm->cur_self=NULL; vm->cur_other=NULL;
  /* Newer formats execute each GlobalScript root code entry once at startup,
   * BEFORE the first room's instances exist. The root's prologue (function definitions) is a
   * no-op for us — script calls dispatch straight to the gml_Script_ entries — but roots also
   * carry loose top-level statements that initialize compatibility tables. Run them with a
   * throwaway self scope, like room creation code. */
  if(!vm->started && !vm->gs_roots_run && anygm_policy_has_modern_function_values(vm->win)){
    vm->gs_roots_run=1;
    for(int ci=0;ci<vm->win->n_code;ci++){
      const char *cn=vm->win->code[ci].name;
      if(!cn || strncmp(cn,"gml_GlobalScript_",17)) continue;
      GmlInstance gs_scratch;
      memset(&gs_scratch,0,sizeof gs_scratch);
      gs_scratch.active=1; gs_scratch.obj=-1; gs_scratch.id=0;
      gs_scratch.image_xscale=gs_scratch.image_yscale=1; gs_scratch.image_alpha=1;
      gs_scratch.sprite_index=-1; gs_scratch.mask_index=-1; gs_scratch.path_index=-1;
      for(int a2=0;a2<GML_ALARMS;a2++) gs_scratch.alarm[a2]=-1;
      GmlVal _r=gml_vm_run_code(vm,ci,&gs_scratch,NULL,NULL,0);
      if(_r.t==V_STR && _r.d!=0) free((char*)_r.s);
      gml_varmap_free_ex(&gs_scratch.vars,0);
      memset(&gs_scratch.vars,0,sizeof gs_scratch.vars);
    }
  }
  if(anygm_host_development_setting(vm->host,"GML_LOG_ROOMGOTO")){ 
    const char *who=(vm->cur_self && vm->cur_self->obj>=0 && vm->cur_self->obj<vm->n_objects)?vm->objects[vm->cur_self->obj].name:"(none)";
    anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[roomgoto] f%ld %d -> %d  by=%s\n",vm->frame,prev_room,room_index,who); }
  vm->step_alloc_base=0;
  /* Room End (Other_5): fire on all active instances before clearing the old
   * room. Persistent instances survive. */
  if(vm->room_index>=0){
    for(int i=0;i<vm->inst_count;i++) if(vm->inst[i].active && !vm->inst[i].marked)
      gml_run_event(vm,&vm->inst[i],"Other_5");
  }
  int store_previous=0;
  if(prev_room>=0){
    GmlVal *persistent=gml_varmap_get(&vm->globals,"room_persistent");
    if(persistent) store_previous=gml_vm_value_as_number(*persistent)!=0.0;
    else { GmlRoom previous; if(gml_vm_room_get(vm,prev_room,&previous)==0) store_previous=previous.persistent; }
    if(prev_room<vm->room_state_count && vm->room_stored) vm->room_stored[prev_room]=store_previous?1:0;
  }
  if(store_previous) room_runtime_state_store(vm,prev_room);
  if(store_previous){
    for(int i=0;i<vm->inst_count;i++){
      GmlInstance *in=&vm->inst[i];
      if(in->persistent || in->marked || in->room_owner!=prev_room || (!in->active&&!in->deactivated)) continue;
      gml_obj_alive_adjust(vm,in->obj,-1); gml_vm_instances_unlink(vm,in,in->obj);
      in->room_was_deactivated=in->deactivated?1:0;
      in->active=0; in->deactivated=0; in->room_dormant=1;
    }
  }
  /* Clear non-persistent instances, including deactivated ones. GM does not fire Destroy on a room
   * change or restart, only Room End and, in newer versions, Clean Up. Firing Destroy here can
   * spawn a new instance after the disposal pass and let it survive into the fresh room. Room End is where
   * instances put room-transition cleanup; Clean Up frees DS structures. */
  for(int i=0;i<vm->inst_count;i++) if((vm->inst[i].active||vm->inst[i].deactivated) && !vm->inst[i].persistent){
    gml_run_event(vm,&vm->inst[i],"CleanUp_0");   /* GMS2.3: Clean Up fires on room-change disposal */
    /* An aliased GmlArr may be shared with a persistent instance or global that survives the room
     * change. Freeing it here creates a use-after-free. Escaped arrays are
     * released by the deduped full teardown, like everywhere else. */
    gml_obj_alive_adjust(vm,vm->inst[i].obj,-1); gml_vm_instances_unlink(vm,&vm->inst[i],vm->inst[i].obj);
    gml_varmap_free_ex(&vm->inst[i].vars,1); vm->inst[i].active=0; vm->inst[i].deactivated=0; }
  vm->room_index=room_index; vm->pending_room=-1;
  for(int i=0;i<vm->inst_count;i++) if(vm->inst[i].persistent && (vm->inst[i].active||vm->inst[i].deactivated))
    vm->inst[i].room_owner=room_index;
  {  vm->room_enter_frame=vm->frame; }
  vm->n_tile_mut=0;   /* tile-layer mutations are per-room */
  vm->n_tile_del_at=0; /* tile_layer_delete_at marks are per-room */
  gml_builtin_physics_room_reset(vm);
  /* Register this room's GMS2 runtime layers (addressable by name) + type-4 tile-collision maps. */
  gml_room_reload_layers(vm, room_index);
  if(anygm_host_development_setting(vm->host,"GML_LOG_ROOM")) anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[room] enter %d\n",room_index);
  GmlRoom r; if(gml_vm_room_get(vm,room_index,&r)!=0) return;
  *gml_varmap_put(&vm->globals,"room_persistent")=vreal(r.persistent?1.0:0.0);
  *gml_varmap_put(&vm->globals,"view_enabled")=vreal(r.view_enabled?1.0:0.0);
  if(anygm_policy_uses_classic_runtime(vm->win))
    *gml_varmap_put(&vm->globals,"room_speed")=vreal(r.speed>0?r.speed:30);
  if(room_index>=0 && room_index<vm->room_state_count && vm->room_stored && vm->room_stored[room_index]){
    vm->room_stored[room_index]=0;
    *gml_varmap_put(&vm->globals,"room_persistent")=vreal(1);
    room_runtime_state_restore(vm,room_index);
    room_camera_resources_init(vm,0);
    for(int i=0;i<vm->inst_count;i++){
      GmlInstance *in=&vm->inst[i];
      if(!in->room_dormant || in->room_owner!=room_index) continue;
      in->room_dormant=0; in->deactivated=in->room_was_deactivated?1:0;
      in->active=in->deactivated?0:1; in->room_was_deactivated=0;
      gml_obj_alive_adjust(vm,in->obj,1); gml_vm_instances_link(vm,in);
    }
    int n0=vm->inst_count;
    for(int i=0;i<n0;i++) if(vm->inst[i].active && !vm->inst[i].marked)
      gml_run_event(vm,&vm->inst[i],"Other_4");
    gml_fire_gamepad_connected(vm);
    gml_vm_instances_reap(vm); gml_vm_instances_rebase_order(vm);
    gml_vm_warm_audio_for_room(vm,vm->room_index); gml_vm_prefetch_room_assets(vm);
    return;
  }
  const uint8_t *d=vm->win->data; uint32_t op=r.obj_ptr, cnt=gml_vm_read_u32_le(d,op);
  for(int i=0;anygm_policy_uses_classic_runtime(vm->win) && i<8;i++){
    gml_vm_global_array_set(vm,"background_visible",i,0); gml_vm_global_array_set(vm,"background_foreground",i,0);
    gml_vm_global_array_set(vm,"background_index",i,-1); gml_vm_global_array_set(vm,"background_x",i,0);
    gml_vm_global_array_set(vm,"background_y",i,0); gml_vm_global_array_set(vm,"background_htiled",i,0);
    gml_vm_global_array_set(vm,"background_vtiled",i,0); gml_vm_global_array_set(vm,"background_hspeed",i,0);
    gml_vm_global_array_set(vm,"background_vspeed",i,0); gml_vm_global_array_set(vm,"background_stretch",i,0);
    gml_vm_global_array_set(vm,"background_alpha",i,1); gml_vm_global_array_set(vm,"background_blend",i,0xFFFFFF);
    for(size_t field=0;field<sizeof(room_view_fields)/sizeof(room_view_fields[0]);field++)
      gml_vm_global_array_set(vm,room_view_fields[field],i,0);
    gml_vm_global_array_set(vm,"view_object",i,-1);
  }
  if(anygm_policy_has_modern_layer_semantics(vm->win)){
    for(int i=0;i<GML_ROOM_CAMERA_COUNT;i++){
      for(size_t field=0;field<sizeof(room_view_fields)/sizeof(room_view_fields[0]);field++)
        gml_vm_global_array_set(vm,room_view_fields[field],i,0);
      gml_vm_global_array_set(vm,"view_object",i,-1);
    }
  }
  /* Initialise the built-in background_* arrays from the room's background
   * layers. GML draw code can read this state during room startup. */
  if(r.bg_ptr){ uint32_t bc=gml_vm_read_u32_le(d,r.bg_ptr);
    for(uint32_t i=0;i<bc && i<8;i++){ uint32_t lp=gml_vm_read_u32_le(d,r.bg_ptr+4+i*4);
      int en=(int)gml_vm_read_u32_le(d,lp), fg=(int)gml_vm_read_u32_le(d,lp+4), def=(int)gml_vm_read_u32_le(d,lp+8);
      int bx=(int)gml_vm_read_u32_le(d,lp+12), by=(int)gml_vm_read_u32_le(d,lp+16), htl=(int)gml_vm_read_u32_le(d,lp+20), vtl=(int)gml_vm_read_u32_le(d,lp+24);
      int bh=(int32_t)gml_vm_read_u32_le(d,lp+28), bv=(int32_t)gml_vm_read_u32_le(d,lp+32);
      gml_vm_global_array_set(vm,"background_visible",i,en?1:0);
      gml_vm_global_array_set(vm,"background_foreground",i,fg?1:0);
      /* Visibility does not clear the assigned background resource. Classic projects commonly
       * author parallax layers hidden and enable them from room-start code; retaining the index is
       * what makes that later background_visible write meaningful. */
      gml_vm_global_array_set(vm,"background_index",i,def);
      gml_vm_global_array_set(vm,"background_x",i,bx);  gml_vm_global_array_set(vm,"background_y",i,by);
      gml_vm_global_array_set(vm,"background_htiled",i,htl?1:0); gml_vm_global_array_set(vm,"background_vtiled",i,vtl?1:0);
      gml_vm_global_array_set(vm,"background_hspeed",i,bh); gml_vm_global_array_set(vm,"background_vspeed",i,bv);
      gml_vm_global_array_set(vm,"background_stretch",i,(int)gml_vm_read_u32_le(d,lp+36)?1:0);
      gml_vm_global_array_set(vm,"background_alpha",i,1.0);          /* GM default */
      gml_vm_global_array_set(vm,"background_blend",i,0xFFFFFF);
      if(anygm_host_development_setting(vm->host,"GML_LOG_BG")) anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[bg-init] layer%u en=%d fg=%d def=%d pos=(%d,%d) htiled=%d vtiled=%d speed=(%d,%d)\n",i,en,fg,def,bx,by,htl,vtl,bh,bv);
    } }
  /* view settings → globals (GM auto-follows view_object each step; see gml_vm_step). View record
   * (14 i32): enabled,xv,yv,wv,hv,xport,yport,wport,hport,hborder,vborder,hspeed,vspeed,object. */
  if(r.view_ptr){ uint32_t vc=gml_vm_read_u32_le(d,r.view_ptr);
    for(uint32_t i=0;i<vc && i<8;i++){ uint32_t vp=gml_vm_read_u32_le(d,r.view_ptr+4+i*4);
      gml_vm_global_array_set(vm,"view_visible",i,(int)gml_vm_read_u32_le(d,vp)?1:0);
      gml_vm_global_array_set(vm,"view_xview",i,(int32_t)gml_vm_read_u32_le(d,vp+4));   gml_vm_global_array_set(vm,"view_yview",i,(int32_t)gml_vm_read_u32_le(d,vp+8));
      gml_vm_global_array_set(vm,"view_wview",i,(int32_t)gml_vm_read_u32_le(d,vp+12));  gml_vm_global_array_set(vm,"view_hview",i,(int32_t)gml_vm_read_u32_le(d,vp+16));
      gml_vm_global_array_set(vm,"view_xport",i,(int32_t)gml_vm_read_u32_le(d,vp+20));  gml_vm_global_array_set(vm,"view_yport",i,(int32_t)gml_vm_read_u32_le(d,vp+24));
      gml_vm_global_array_set(vm,"view_wport",i,(int32_t)gml_vm_read_u32_le(d,vp+28));  gml_vm_global_array_set(vm,"view_hport",i,(int32_t)gml_vm_read_u32_le(d,vp+32));
      gml_vm_global_array_set(vm,"view_hborder",i,(int32_t)gml_vm_read_u32_le(d,vp+36));gml_vm_global_array_set(vm,"view_vborder",i,(int32_t)gml_vm_read_u32_le(d,vp+40));
      gml_vm_global_array_set(vm,"view_hspeed",i,(int32_t)gml_vm_read_u32_le(d,vp+44)); gml_vm_global_array_set(vm,"view_vspeed",i,(int32_t)gml_vm_read_u32_le(d,vp+48));
      gml_vm_global_array_set(vm,"view_object",i,(int32_t)gml_vm_read_u32_le(d,vp+52));
    } }
  /* Apply runtime room-view resource overrides on entry. */
  if(vm->view_ovr) for(int i=0;i<8;i++){
    int k=room_index*8+i;
    if(k>=0 && k<vm->n_view_ovr){
      struct GmlViewOvr *o=&vm->view_ovr[k];
      if(i==0 && o->room_enabled_set)
        *gml_varmap_put(&vm->globals,"view_enabled")=vreal(o->room_enabled?1.0:0.0);
      if(o->set || o->full){
        gml_vm_global_array_set(vm,"view_visible",i,o->vis);
        gml_vm_global_array_set(vm,"view_xport",i,o->x); gml_vm_global_array_set(vm,"view_yport",i,o->y);
        gml_vm_global_array_set(vm,"view_wport",i,o->w); gml_vm_global_array_set(vm,"view_hport",i,o->h);
      }
      if(o->full){
        gml_vm_global_array_set(vm,"view_xview",i,o->view_x); gml_vm_global_array_set(vm,"view_yview",i,o->view_y);
        gml_vm_global_array_set(vm,"view_wview",i,o->view_w); gml_vm_global_array_set(vm,"view_hview",i,o->view_h);
        gml_vm_global_array_set(vm,"view_hborder",i,o->hborder); gml_vm_global_array_set(vm,"view_vborder",i,o->vborder);
        gml_vm_global_array_set(vm,"view_hspeed",i,o->hspeed); gml_vm_global_array_set(vm,"view_vspeed",i,o->vspeed);
        gml_vm_global_array_set(vm,"view_object",i,o->object);
      }
    } }
  room_camera_resources_init(vm,1);
  if(anygm_host_development_setting(vm->host,"GML_LOG_VIEW")) anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[view] room=%d dim=%ux%u view0 en=%.0f wview=%.0f hview=%.0f wport=%.0f hport=%.0f\n",
    vm->room_index, r.width, r.height, gml_vm_global_array_number(vm,"view_visible",0),
    gml_vm_global_array_number(vm,"view_wview",0), gml_vm_global_array_number(vm,"view_hview",0),
    gml_vm_global_array_number(vm,"view_wport",0), gml_vm_global_array_number(vm,"view_hport",0));
  /* Room instances already have GameMaker ids in the data.win. Dynamic instances created by
   * room-start Create/Other_4 code must start above those ids, or StackTop writes to `nnn.var`
   * can resolve to an unrelated placed instance with the same id. */
  for(uint32_t i=0;i<cnt;i++){
    uint32_t ip=gml_vm_read_u32_le(d,op+4+i*4);
    uint32_t rid=gml_vm_read_u32_le(d,ip+12);
    if(rid>=vm->next_id) vm->next_id=rid+1;
  }
  /* Placed ids are authored resource data, not allocations from the dynamic-id sequence.
   * gml_vm_instances_initialize() still performs all ordinary instance initialization below, so preserve the
   * already-reserved dynamic id here and restore it once every placed slot exists. */
  uint32_t next_dynamic_id=vm->next_id;
  /* Room-placed instances are all present before any Create event runs. Some
   * room setup code relies on seeing other already-placed instances. */
  int *room_inst_idx=malloc((cnt?cnt:1)*sizeof(int));
  for(uint32_t i=0;i<cnt;i++){
    uint32_t ip=gml_vm_read_u32_le(d,op+4+i*4);
    int32_t x=(int32_t)gml_vm_read_u32_le(d,ip), y=(int32_t)gml_vm_read_u32_le(d,ip+4); int obj=(int32_t)gml_vm_read_u32_le(d,ip+8);
    GmlInstance *in=gml_vm_instances_alloc(vm); gml_vm_instances_initialize(vm,in,x,y,obj);
    in->id=gml_vm_read_u32_le(d,ip+12);  /* room-assigned instance id */
    gml_vm_instances_apply_room_transform(vm,in,ip);
    in->room_placed=1;
    room_inst_idx[i]=(int)(in-vm->inst);
  }
  vm->next_id=next_dynamic_id;
  /* GMS2: placed instances take their draw depth from the room LAYER that holds them
   * (type-2 instance layers; the OBJT depth field is 0 in GMS2 exports). Without this every
   * placed instance lands at depth zero and layer ordering collapses. Dynamic instances are
   * unaffected because Studio sets depth at
   * create time (instance_create_depth / compat object_get_depth map). Runs before Create
   * events so Create code reading `depth` sees the room value, like GM. */
  {
    uint32_t lcnt=0;
    uint32_t lay=gml_vm_rooms_layer_list(vm,room_index,&lcnt);
    if(lcnt>0 && lcnt<512){
      for(uint32_t i=0;i<lcnt;i++){ uint32_t lp=gml_vm_read_u32_le(d,lay+4+i*4);
        if(!lp || gml_vm_read_u32_le(d,lp+8)!=2) continue;
        uint32_t tb=gml_room_layer_type_off(vm,lp);
        if(tb+4>vm->win->size) continue;
        double ldep=(double)(int32_t)gml_vm_read_u32_le(d,lp+12);
        int lorder=(int)i;
        uint32_t lnp=gml_vm_read_u32_le(d,lp+0);
        if(lnp && lnp<vm->win->size){
          GmlRtLayer *rl=gml_rt_layer_find_by_name(vm,(const char*)(d+lnp));
          if(rl) lorder=rl->order;
        }
        uint32_t ic=gml_vm_read_u32_le(d,tb);
        if(ic>100000) continue;
        for(uint32_t k=0;k<ic;k++){
          uint32_t ip2=tb+4+k*4;
          if(ip2+4>vm->win->size) break;
          uint32_t iid=gml_vm_read_u32_le(d,ip2);
          for(uint32_t j=0;j<cnt;j++){ int idx=room_inst_idx[j];
            if(idx>=0 && idx<vm->inst_count && vm->inst[idx].id==iid){
              vm->inst[idx].depth=ldep;
              vm->inst[idx].draw_layer_order=lorder;
              vm->inst[idx].draw_layer_element_order=(int)k;
              break;
            } }
        }
      }
    }
  }
  /* Create plus per-instance creation code runs in room order after every placed instance exists.
   * Classic projects serialize which side of Create the authored instance code occupies. Wide
   * bytecode-17 records also carry overrides between the object's PreCreate defaults and Create. */
  for(uint32_t i=0;i<cnt;i++){
    int idx=room_inst_idx[i]; if(idx<0 || idx>=vm->inst_count) continue;
    GmlInstance *in=&vm->inst[idx]; if(!in->active || in->marked) continue;
    uint32_t ip=gml_vm_read_u32_le(d,op+4+i*4);
    int cc=(int32_t)gml_vm_read_u32_le(d,ip+16);
    int pre_cc=gml_room_instance_precreate_code(vm,ip);
    int code_before_create=anygm_policy_creation_code_before_create(vm->win);
    if(code_before_create && cc>=0 && cc<vm->win->n_code){
      GmlVal _r=gml_vm_run_code(vm,cc,in,NULL,NULL,0);
      if(_r.t==V_STR && _r.d!=0) free((char*)_r.s);
    }
    if(in->active && !in->marked){
      gml_run_event(vm,in,"PreCreate_0");   /* GMS2: variable-definitions, before Create */
      if(in->active && !in->marked && pre_cc>=0 && pre_cc<vm->win->n_code){
        GmlVal _r=gml_vm_run_code(vm,pre_cc,in,NULL,NULL,0);
        if(_r.t==V_STR && _r.d!=0) free((char*)_r.s);
      }
      if(in->active && !in->marked) gml_run_event(vm,in,"Create_0");
    }
    if(!code_before_create && in->active && !in->marked && cc>=0 && cc<vm->win->n_code){
      GmlVal _r=gml_vm_run_code(vm,cc,in,NULL,NULL,0);
      if(_r.t==V_STR && _r.d!=0) free((char*)_r.s);
    }
  }
  free(room_inst_idx);
  /* Game Start / Room Start fire only on the instances present at room start.
   * Snapshot the count so instances created by those events do not recursively
   * receive the same startup event in the same dispatch. */
  int n0 = vm->inst_count;
  /* Game Start (Other_2), first room only — before room creation code */
  if(!vm->started){ vm->started=1;
    for(int i=0;i<n0;i++) if(vm->inst[i].active && !vm->inst[i].marked)
      gml_run_event(vm,&vm->inst[i],"Other_2"); }
  /* Room creation code runs after instances are created. GM gives it a throwaway self scope so
   * unqualified writes and reads inside one run round-trip. The scratch instance is not in the
   * pool: it is invisible to with(), instance_number, and draw, then discarded after the run. */
  if(r.creation_code>=0 && r.creation_code<vm->win->n_code){
    GmlInstance cc_scratch;
    memset(&cc_scratch,0,sizeof cc_scratch);
    cc_scratch.active=1; cc_scratch.obj=-1; cc_scratch.id=0;
    cc_scratch.image_xscale=cc_scratch.image_yscale=1; cc_scratch.image_alpha=1;
    cc_scratch.sprite_index=-1; cc_scratch.mask_index=-1; cc_scratch.path_index=-1;
    cc_scratch.timeline_index=-1; cc_scratch.timeline_speed=1;
    for(int a2=0;a2<GML_ALARMS;a2++) cc_scratch.alarm[a2]=-1;
    GmlVal _r=gml_vm_run_code(vm,r.creation_code,&cc_scratch,NULL,NULL,0);
    if(_r.t==V_STR && _r.d!=0) free((char*)_r.s);
    gml_varmap_free_ex(&cc_scratch.vars,0);
    memset(&cc_scratch.vars,0,sizeof cc_scratch.vars);
  }
  /* Room Start (Other_4) for the instances that existed at room start */
  n0 = vm->inst_count;
  for(int i=0;i<n0;i++) if(vm->inst[i].active && !vm->inst[i].marked)
    gml_run_event(vm,&vm->inst[i],"Other_4");
  /* Emit GM's asynchronous gamepad-discovered event so event-driven input can retain a device id. */
  gml_fire_gamepad_connected(vm);
  gml_vm_instances_reap(vm);
  gml_vm_instances_rebase_order(vm);
  gml_vm_warm_audio_for_room(vm,vm->room_index);
  gml_vm_prefetch_room_assets(vm);
}
void gml_vm_goto_room_order(GmlVM *vm, int order_index){
  GmlWin *w=vm->win;
  int idx = (order_index>=0 && order_index<w->n_room_order)? (int)w->room_order[order_index] : order_index;
  gml_room_enter(vm,idx);
}

static int timeline_fire_range(GmlVM *vm, GmlInstance *in, GmlTimeline *timeline,
                               double from, double to, int forward,
                               int expected_index, double expected_position,
                               unsigned expected_generation){
  if(forward){
    for(int m=0;m<timeline->n;m++){
      GmlTimelineMoment *moment=&timeline->moments[m];
      if(moment->step<from || moment->step>=to) continue;
      if(moment->code>=0 && moment->code<vm->win->n_code){
        GmlVal r=gml_vm_run_code(vm,moment->code,in,NULL,NULL,0);
        if(r.t==V_STR && r.d!=0) free((char*)r.s);
      }
      if(!in->active || in->marked || !in->timeline_running ||
         expected_index<0 || expected_index>=vm->n_timelines ||
         vm->timelines[expected_index].generation!=expected_generation ||
         (int)in->timeline_index!=expected_index || in->timeline_position!=expected_position) return 0;
    }
  } else {
    for(int m=timeline->n-1;m>=0;m--){
      GmlTimelineMoment *moment=&timeline->moments[m];
      if(moment->step>from || moment->step<=to) continue;
      if(moment->code>=0 && moment->code<vm->win->n_code){
        GmlVal r=gml_vm_run_code(vm,moment->code,in,NULL,NULL,0);
        if(r.t==V_STR && r.d!=0) free((char*)r.s);
      }
      if(!in->active || in->marked || !in->timeline_running ||
         expected_index<0 || expected_index>=vm->n_timelines ||
         vm->timelines[expected_index].generation!=expected_generation ||
         (int)in->timeline_index!=expected_index || in->timeline_position!=expected_position) return 0;
    }
  }
  return 1;
}

/* Timelines advance between Begin Step and Alarm processing. Each step owns the interval that
 * starts at its old position: forward playback includes the old endpoint and excludes the new
 * one, with the inverse interval for reverse playback. This makes moment zero run when playback
 * first leaves position zero in either direction. Playback mutations take effect immediately. */
void gml_vm_rooms_step_timelines(GmlVM *vm, int count){
  for(int i=0;i<count;i++){
    GmlInstance *in=&vm->inst[i];
    if(!in->active || in->marked || !gml_vm_instances_step_snapshot_member(vm,in) ||
       !in->timeline_running || in->timeline_speed==0) continue;
    int ti=(int)in->timeline_index;
    if(ti<0 || ti>=vm->n_timelines){ in->timeline_running=0; continue; }
    GmlTimeline snapshot=vm->timelines[ti];
    GmlTimeline *timeline=&snapshot;
    if(timeline->n<=0 || timeline->last_step<0){ in->timeline_running=0; continue; }
    double old=in->timeline_position, speed=in->timeline_speed;
    double length=(double)timeline->last_step+1.0;
    if(!in->timeline_loop){
      double raw=old+speed, next=raw;
      int stop=0;
      if(speed>0 && next>timeline->last_step){ next=timeline->last_step; stop=1; }
      if(speed<0 && next<0){ next=0; stop=1; }
      in->timeline_position=next;
      int ok=timeline_fire_range(vm,in,timeline,old,raw,speed>0,ti,next,timeline->generation);
      if(ok && stop && in->active && !in->marked && (int)in->timeline_index==ti &&
         in->timeline_position==next) in->timeline_running=0;
      continue;
    }
    old=fmod(old,length); if(old<0) old+=length;
    double next=fmod(old+speed,length); if(next<0) next+=length;
    in->timeline_position=next;
    double pos=old, remaining=fabs(speed);
    int guard=0, ok=1;
    if(speed>0){
      while(remaining>0 && ok && guard++<4096){
        double distance=length-pos;
        if(remaining<distance){ ok=timeline_fire_range(vm,in,timeline,pos,pos+remaining,1,ti,next,timeline->generation); break; }
        ok=timeline_fire_range(vm,in,timeline,pos,length,1,ti,next,timeline->generation);
        if(!ok) break;
        remaining-=distance;
        pos=0;
      }
    } else {
      while(remaining>0 && ok && guard++<4096){
        double distance=pos+1.0;
        if(remaining<distance){ ok=timeline_fire_range(vm,in,timeline,pos,pos-remaining,0,ti,next,timeline->generation); break; }
        ok=timeline_fire_range(vm,in,timeline,pos,-1,0,ti,next,timeline->generation);
        if(!ok) break;
        remaining-=distance;
        pos=timeline->last_step;
      }
    }
  }
}

/* ---- tile-layer runtime mutations (tile_layer_delete/depth/shift/hide/show) ----
 * GM tiles live in a tile layer keyed by depth. The room's tiles are read immutably from the
 * data.win each frame, so we keep a small list of per-depth mutations applied at gather time.
 * find-or-create the entry for `depth`. */
static int tile_mut_idx(GmlVM *vm, int depth){
  for(int i=0;i<vm->n_tile_mut;i++) if(vm->tile_mut[i].depth==depth) return i;
  if(vm->n_tile_mut>=64) return -1;
  int i=vm->n_tile_mut++; vm->tile_mut[i].depth=depth; vm->tile_mut[i].flags=0;
  vm->tile_mut[i].has_remap=0; vm->tile_mut[i].remap=0; vm->tile_mut[i].dx=vm->tile_mut[i].dy=0;
  return i;
}
void gml_tile_layer_delete(GmlVM *vm, int depth){ int i=tile_mut_idx(vm,depth); if(i>=0) vm->tile_mut[i].flags|=GML_VM_TILE_MUT_DELETED; }
void gml_tile_layer_depth(GmlVM *vm, int depth, int newdepth){ int i=tile_mut_idx(vm,depth);
  if(i>=0){ vm->tile_mut[i].has_remap=1; vm->tile_mut[i].remap=newdepth; } }
void gml_tile_layer_shift(GmlVM *vm, int depth, double dx, double dy){ int i=tile_mut_idx(vm,depth);
  if(i>=0){ vm->tile_mut[i].dx+=dx; vm->tile_mut[i].dy+=dy; } }
void gml_tile_layer_delete_at(GmlVM *vm, int depth, double x, double y){
  if(vm->n_tile_del_at<64){ int i=vm->n_tile_del_at++;
    vm->tile_del_at[i].depth=depth; vm->tile_del_at[i].x=(int)x; vm->tile_del_at[i].y=(int)y; } }
void gml_tile_layer_hide(GmlVM *vm, int depth, int hidden){ int i=tile_mut_idx(vm,depth);
  if(i>=0){ if(hidden) vm->tile_mut[i].flags|=GML_VM_TILE_MUT_HIDDEN; else vm->tile_mut[i].flags&=~GML_VM_TILE_MUT_HIDDEN;
    if(anygm_host_development_setting(vm->host,"GML_LOG_RTL")){ 
      anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[rtl] f%ld tile layer depth=%d hidden=%d flags=%d\n",
              vm->frame,depth,hidden,vm->tile_mut[i].flags); } } }
