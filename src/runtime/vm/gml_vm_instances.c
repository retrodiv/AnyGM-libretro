/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* gml_vm_instances.c — object, instance, event, and collision ownership. */
#include "gml_vm.h"
#include "gml_vm_internal.h"
#include "gml_value_internal.h"
#include "anygm_compatibility.h"
#include "gml_render.h"
#include "anygm_host.h"

#include <math.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

double gml_vm_classic_round_even(double x){
  double f=floor(x), diff=x-f;
  if(diff<0.5) return f;
  if(diff>0.5) return f+1.0;
  return fmod(f,2.0)==0.0 ? f : f+1.0;
}


int gml_vm_instances_bbox(GmlVM *vm, GmlInstance *in,
                          double *l, double *t, double *r, double *b);

/* ---------------- object parsing ---------------- */
static void parse_native_object_events(GmlVM *vm, GmlObject *object, uint32_t table,
                                       size_t end){
  const uint8_t *data=vm->win->data;
  if((size_t)table+4>end) return;
  uint32_t types=gml_vm_read_u32_le(data,table);
  if(types>32 || (size_t)table+4+(size_t)types*4>end) return;
  int total=0;
  for(uint32_t type=0;type<types;type++){
    uint32_t list=gml_vm_read_u32_le(data,table+4+type*4);
    if((size_t)list+4>end) return;
    uint32_t count=gml_vm_read_u32_le(data,list);
    if(count>100000 || total>(int)(100000-count) || (size_t)list+4+(size_t)count*4>end) return;
    total+=(int)count;
  }
  if(!total) return;
  object->events=calloc((size_t)total,sizeof(*object->events));
  if(!object->events) return;
  for(uint32_t type=0;type<types;type++){
    uint32_t list=gml_vm_read_u32_le(data,table+4+type*4), count=gml_vm_read_u32_le(data,list);
    for(uint32_t event_index=0;event_index<count;event_index++){
      uint32_t event=gml_vm_read_u32_le(data,list+4+event_index*4);
      if((size_t)event+8>end) continue;
      int subtype=(int32_t)gml_vm_read_u32_le(data,event);
      uint32_t actions=gml_vm_read_u32_le(data,event+4);
      if(actions>4096 || (size_t)event+8+(size_t)actions*4>end) continue;
      int code=-1;
      for(uint32_t action_index=0;action_index<actions;action_index++){
        uint32_t action=gml_vm_read_u32_le(data,event+8+action_index*4);
        if((size_t)action+36>end) continue;
        uint32_t candidate=gml_vm_read_u32_le(data,action+32);
        if(candidate<(uint32_t)vm->win->n_code){ code=(int)candidate; break; }
      }
      /* Keep the declaration even when the event has no executable action.  Studio serialises
       * empty Alarm events with a null CODE id; their presence still makes that alarm count down,
       * while a genuinely undeclared alarm remains a user-controlled value. */
      object->events[object->n_events].evtype=(int)type;
      object->events[object->n_events].subtype=subtype;
      object->events[object->n_events].code=code;
      object->n_events++;
    }
  }
}

static void parse_objects(GmlVM *vm){
  GmlWin *w=vm->win; const GmlChunk *c=gml_chunk(w,"OBJT"); if(!c) return;
  const uint8_t *d=w->data; uint32_t n=gml_vm_read_u32_le(d,c->off);
  vm->n_objects=(int)n; vm->objects=calloc(n,sizeof(GmlObject));
  /* OBJT layout is not uniform across bytecode-17 files, matching the split in FUNC references.
   * Newer exports insert a Managed flag after Visible and shift the remaining fields by four bytes;
   * older exports retain the bc14-16 layout. Reading the wrong slots corrupts persistence and object
   * inheritance. Detect the parent offset per file: the parent column
   * is the one whose values all fall in {-100 (no parent), -1, 0..n_objects-1} and that CONTAINS the
   * -100 sentinel (root objects always have it; a mask/sprite column never does). */
  int poff = 24;
  if(anygm_policy_has_modern_function_values(w)){
    int best=-1;
    for(int cand=28; cand>=24; cand-=4){
      int ok=1, saw_root=0;
      for(uint32_t i=0;i<n;i++){
        uint32_t p=gml_vm_read_u32_le(d,c->off+4+i*4);
        int32_t v=(int32_t)gml_vm_read_u32_le(d,p+cand);
        if(v==-100){ saw_root=1; continue; }
        if(v<-1 || v>=(int32_t)n || v==(int32_t)i){ ok=0; break; }
      }
      if(ok && saw_root){ best=cand; break; }
    }
    poff = best>=0 ? best : 28;   /* fall back to the newer layout if neither column is clean */
  }
  int shift = poff-24;   /* 4 when the Managed field is present, 0 for the classic layout */
  for(uint32_t i=0;i<n;i++){
    uint32_t p=gml_vm_read_u32_le(d,c->off+4+i*4);
    GmlObject *o=&vm->objects[i];
    o->name=gml_str_by_ptr(w,gml_vm_read_u32_le(d,p));
    o->sprite_index=(int)gml_vm_read_u32_le(d,p+4);
    o->visible=(int)gml_vm_read_u32_le(d,p+8); o->solid=(int)gml_vm_read_u32_le(d,p+12+shift);
    o->depth=(int)gml_vm_read_u32_le(d,p+16+shift); o->persistent=(int)gml_vm_read_u32_le(d,p+20+shift);
    o->parent=(int)gml_vm_read_u32_le(d,p+poff);
    o->mask_index=(int)gml_vm_read_u32_le(d,p+poff+4);
    /* Physics metadata follows parent/mask in both classic and managed GMS2 OBJT layouts. Retain
     * only the mass inputs needed by the lightweight backend, after validating the variable-length
     * vertex list entirely inside OBJT. Coordinates are fixture-local pixels. */
    uint32_t q=p+(uint32_t)poff+8u;
    uint64_t cend=(uint64_t)c->off+c->size;
    uint32_t nvert=UINT32_MAX;
    if((uint64_t)q+48u<=cend && (uint64_t)q+48u<=w->size){
      uint32_t enabled=gml_vm_read_u32_le(d,q), kinematic=gml_vm_read_u32_le(d,q+44);
      nvert=gml_vm_read_u32_le(d,q+32);
      double density=gml_vm_read_f32_le(d,q+12);
      uint64_t vend=(uint64_t)q+48u+(uint64_t)nvert*8u;
      uint32_t sensor=gml_vm_read_u32_le(d,q+4), awake=gml_vm_read_u32_le(d,q+40);
      int32_t shape=(int32_t)gml_vm_read_u32_le(d,q+8);
      int32_t group=(int32_t)gml_vm_read_u32_le(d,q+20);
      if(enabled<=1 && sensor<=1 && awake<=1 && kinematic<=1 && shape>=0 && shape<=2 &&
         isfinite(density) && density>=0.0 && density<=1000000.0 && nvert<=128 &&
         vend<=cend && vend<=w->size){
        o->physics_enabled=(int)enabled;
        o->physics_sensor=(int)sensor;
        o->physics_shape=(int)shape;
        o->physics_group=(int)group;
        o->physics_awake=(int)awake;
        o->physics_kinematic=(int)kinematic;
        o->physics_density=density;
        if(nvert<=GML_OBJECT_PHYSICS_POINT_MAX){
          int points_valid=1;
          for(uint32_t vi=0;vi<nvert;vi++){
            o->physics_point[vi][0]=gml_vm_read_f32_le(d,q+48u+vi*8u);
            o->physics_point[vi][1]=gml_vm_read_f32_le(d,q+52u+vi*8u);
            if(!isfinite(o->physics_point[vi][0]) || !isfinite(o->physics_point[vi][1]) ||
               fabs(o->physics_point[vi][0])>1000000.0f ||
               fabs(o->physics_point[vi][1])>1000000.0f) points_valid=0;
          }
          if(points_valid) o->physics_point_count=(int)nvert;
        }
        if(o->physics_point_count>=3){
          double twice_area=0.0;
          for(uint32_t vi=0;vi<nvert;vi++){
            uint32_t vj=(vi+1u)%nvert;
            double xi=o->physics_point[vi][0], yi=o->physics_point[vi][1];
            double xj=o->physics_point[vj][0], yj=o->physics_point[vj][1];
            twice_area+=xi*yj-xj*yi;
          }
          o->physics_area_px=fabs(twice_area)*0.5;
        }
      }
    }
    if(nvert<=128){
      uint64_t event_table=(uint64_t)q+48u+(uint64_t)nvert*8u;
      if(event_table+4u<=cend && event_table+4u<=w->size)
        parse_native_object_events(vm,o,(uint32_t)event_table,(size_t)cend);
    }
  }
  if(anygm_host_development_setting(vm->host,"GML_LOG_OBJ")) anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[obj] OBJT parent_off=+%d (%s layout)\n", poff,
    shift? "managed/GMS2.3+" : "classic");
  { const char *dbg=anygm_host_development_setting(vm->host,"GML_DBG_OBJREC");   /* print one object's parsed record by name */
    if(dbg) for(uint32_t i=0;i<n;i++) if(vm->objects[i].name && !strcmp(vm->objects[i].name,dbg)){
      GmlObject *o=&vm->objects[i];
      anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[objrec] %s idx=%u spr=%d vis=%d solid=%d depth=%d pers=%d parent=%d mask=%d\n",
        dbg,i,o->sprite_index,o->visible,o->solid,o->depth,o->persistent,o->parent,o->mask_index); } }
  /* Defensive: cut parent cycles / self-parents so every parent-chain walk (events, collisions,
   * inheritance) terminates. Those walks only bounds-check `p`, not revisits, so a malformed or
   * unexpectedly-laid-out OBJT parent (seen in some bytecode-17 data) would otherwise hang forever. */
  int n_cut=0;
  for(int i=0;i<vm->n_objects;i++){
    if(vm->objects[i].parent==i){ vm->objects[i].parent=-1; n_cut++; continue; }
    int slow=i, fast=i;
    for(;;){
      if(fast<0||fast>=vm->n_objects) break;
      fast=vm->objects[fast].parent; if(fast<0||fast>=vm->n_objects) break;
      fast=vm->objects[fast].parent; slow=vm->objects[slow].parent;
      if(slow<0||slow>=vm->n_objects) break;
      if(slow==fast){ vm->objects[i].parent=-1; n_cut++; break; }  /* cycle reached from i: cut its link */
    }
  }
  if(anygm_host_development_setting(vm->host,"GML_LOG_OBJ")) anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[obj] n_objects=%d parent_cycles_cut=%d disp=%ux%u\n", vm->n_objects, n_cut, w->disp_w, w->disp_h);
}

/* ---------------- instances / events / rooms ---------------- */
double gml_global_num(GmlVM *vm, const char *name){
  if(vm && name && gml_vm_variable_name_maybe_special(vm,name,gml_value_name_hash(name))){
    GmlVal value=gml_vm_variable_get_h(vm,IT_GLOBAL,name,gml_value_name_hash(name));
    if(value.t==V_REAL) return value.d;
  }
  { GmlVal *slot=gml_varmap_get_hashed(&vm->globals,name,gml_value_name_hash(name));
    if(slot) return slot->t==V_REAL? slot->d : 0; }
  return 0;
}
/* Globals are a hashed varmap with unique keys, so resolve through the hash rather than
 * comparing every occupied slot: the view/camera reads below run several times per frame and
 * the linear form made whole-table strcmp sweeps one of the hottest paths in the runtime. */
double gml_global_arr(GmlVM *vm, const char *name, int idx){
  if(!vm || !name) return 0;
  GmlVal *slot=gml_varmap_get_hashed(&vm->globals,name,gml_value_name_hash(name));
  if(slot && slot->t==V_ARR){ GmlArr *A=slot->arr;
    if(A && idx>=0 && idx<A->len) return A->data[idx].t==V_REAL? A->data[idx].d : 0; }
  return 0;
}

int gml_object_is(GmlVM *vm, int obj, int target){
  while(obj>=0 && obj<vm->n_objects){ if(obj==target) return 1; obj=vm->objects[obj].parent; }
  return 0;
}
void gml_alarm_pause_reset(GmlVM *vm){ if(vm) vm->alarm_pause_count=0; }
int gml_alarm_pause_add(GmlVM *vm, const char *objname, int alarm_index){
  if(!vm || !objname || alarm_index<0 || alarm_index>=GML_ALARMS) return 0;
  int obj=gml_object_index_by_name(vm,objname); if(obj<0) return 0;
  for(int i=0;i<vm->alarm_pause_count;i++)
    if(vm->alarm_pause[i].obj==obj && vm->alarm_pause[i].alarm==alarm_index) return 1;
  if(vm->alarm_pause_count>=GML_ALARM_PAUSES) return 0;
  vm->alarm_pause[vm->alarm_pause_count].obj=obj;
  vm->alarm_pause[vm->alarm_pause_count].alarm=alarm_index;
  vm->alarm_pause_count++;
  return 1;
}
/* Descendants pause with their declared family, matching how an instance freeze selects targets. */
int gml_alarm_paused(GmlVM *vm, int obj, int alarm_index){
  if(!vm || vm->alarm_pause_count<=0) return 0;
  for(int i=0;i<vm->alarm_pause_count;i++)
    if(vm->alarm_pause[i].alarm==alarm_index && gml_object_is(vm,obj,vm->alarm_pause[i].obj)) return 1;
  return 0;
}
int gml_object_index_by_name(GmlVM *vm, const char *name){
  for(int i=0;i<vm->n_objects;i++) if(vm->objects[i].name && !strcmp(vm->objects[i].name,name)) return i;
  return -1;
}
GmlInstance *gml_find_instance(GmlVM *vm, int obj){
  if(obj>=100000){
    for(int i=0;i<vm->inst_count;i++)
      if(vm->inst[i].active && !vm->inst[i].marked && (int)vm->inst[i].id==obj) return &vm->inst[i];
    return NULL;
  }
  for(int i=0;i<vm->inst_count;i++) if(vm->inst[i].active && !vm->inst[i].marked && gml_object_is(vm,vm->inst[i].obj,obj)) return &vm->inst[i];
  return NULL;
}
int gml_instance_number(GmlVM *vm, int target){
  int n=0;
  if(target==IT_ALL){
    for(int i=0;i<vm->inst_count;i++)
      if(vm->inst[i].active && !vm->inst[i].marked) n++;
    return n;
  }
  if(target==IT_SELF)  return (vm->cur_self  && vm->cur_self->active  && !vm->cur_self->marked)  ? 1 : 0;
  if(target==IT_OTHER) return (vm->cur_other && vm->cur_other->active && !vm->cur_other->marked) ? 1 : 0;
  if(target>=100000){
    for(int i=0;i<vm->inst_count;i++)
      if(vm->inst[i].active && !vm->inst[i].marked && (int)vm->inst[i].id==target) return 1;
    return 0;
  }
  /* Family counts include deactivated instances, so only zero is an exact shortcut here. */
  if(target>=0 && target<vm->n_objects && vm->obj_alive && vm->obj_alive[target]==0) return 0;
  for(int i=0;i<vm->inst_count;i++)
    if(vm->inst[i].active && !vm->inst[i].marked && gml_object_is(vm,vm->inst[i].obj,target)) n++;
  return n;
}
static int run_event_code_from(GmlVM *vm, GmlInstance *in, GmlInstance *other,
                               const char *suffix, int obj, int ci){
  const char *pe=vm->cur_event; int peo=vm->cur_event_obj;
  int pet=vm->event_type, pen=vm->event_number;
  int et=0, en=0;
  if(suffix){
    if(!strncmp(suffix,"Create_",7)){ et=0; en=atoi(suffix+7); }
    else if(!strncmp(suffix,"Destroy_",8)){ et=1; en=atoi(suffix+8); }
    else if(!strncmp(suffix,"Alarm_",6)){ et=2; en=atoi(suffix+6); }
    else if(!strncmp(suffix,"Step_",5)){ et=3; en=atoi(suffix+5); }
    else if(!strncmp(suffix,"Collision_",10)){ et=4; en=atoi(suffix+10); }
    else if(!strncmp(suffix,"Other_",6)){ et=7; en=atoi(suffix+6); }
    else if(!strncmp(suffix,"Draw_",5)){ et=8; en=atoi(suffix+5); }
    else if(!strncmp(suffix,"KeyPress_",9)){ et=9; en=atoi(suffix+9); }
    else if(!strncmp(suffix,"KeyRelease_",11)){ et=10; en=atoi(suffix+11); }
    else if(!strncmp(suffix,"CleanUp_",8)){ et=12; en=atoi(suffix+8); }
  }
  vm->cur_event=suffix; vm->cur_event_obj=obj;
  vm->event_type=et; vm->event_number=en;
  GmlVal _r=gml_vm_run_code(vm,ci,in,other,NULL,0);
  if(_r.t==V_STR && _r.d!=0) free((char*)_r.s);   /* discarded owned return: free it */
  vm->cur_event=pe; vm->cur_event_obj=peo;
  vm->event_type=pet; vm->event_number=pen;
  return 1;
}
static uint32_t suffix_hash(const char *s){
  uint32_t h=2166136261u;
  for(const unsigned char *p=(const unsigned char*)s; p && *p; p++) h=(h^*p)*16777619u;
  return h;
}
/* Resolve an event suffix at one object level. For numeric collision targets,
 * try the target object's CODE spelling and declared OBJT entry after the
 * literal suffix. Keep inherited lookup consistent with collision registration. */
static int event_code_at_level(GmlVM *vm, int level, const char *suffix){
  char name[160];
  snprintf(name,sizeof name,"gml_Object_%s_%s",vm->objects[level].name,suffix);
  int ci=gml_code_index_by_name(vm->win,name);
  if(ci>=0) return ci;
  if(!strncmp(suffix,"Collision_",10) && suffix[10]>='0' && suffix[10]<='9'){
    int target=atoi(suffix+10);
    if(target>=0 && target<vm->n_objects){
      snprintf(name,sizeof name,"gml_Object_%s_Collision_%s",
               vm->objects[level].name,vm->objects[target].name);
      ci=gml_code_index_by_name(vm->win,name);
      if(ci>=0) return ci;
      GmlObject *object=&vm->objects[level];
      for(int e=0;e<object->n_events;e++)
        if(object->events[e].evtype==4 && object->events[e].subtype==target &&
           object->events[e].code>=0 && object->events[e].code<vm->win->n_code)
          return object->events[e].code;
    }
  }
  return -1;
}
int gml_vm_instances_event_lookup(GmlVM *vm, const char *suffix, int obj, int *handler_obj, int *code){
  if(!suffix || obj<0 || obj>=vm->n_objects) return 0;
  if(vm->event_cache && vm->event_cache_cap>0){
    uint32_t h=((uint32_t)obj*2654435761u) ^ suffix_hash(suffix);
    GmlEventCache *c=&vm->event_cache[h & (uint32_t)(vm->event_cache_cap-1)];
    if(c->valid && c->start_obj==obj && !strcmp(c->suffix,suffix)){
      if(c->code<0) return 0;
      if(handler_obj) *handler_obj=c->handler_obj;
      if(code) *code=c->code;
      return 1;
    }
    int ho=-1, ci=-1;
    for(int p=obj; p>=0 && p<vm->n_objects; p=vm->objects[p].parent){
      ci=event_code_at_level(vm,p,suffix);
      if(ci>=0){ ho=p; break; }
    }
    c->valid=1; c->start_obj=obj; c->handler_obj=ho; c->code=ci;
    snprintf(c->suffix,sizeof c->suffix,"%s",suffix);
    if(ci<0) return 0;
    if(handler_obj) *handler_obj=ho;
    if(code) *code=ci;
    return 1;
  }
  for(int p=obj; p>=0 && p<vm->n_objects; p=vm->objects[p].parent){
    int ci=event_code_at_level(vm,p,suffix);
    if(ci>=0){
      if(handler_obj) *handler_obj=p;
      if(code) *code=ci;
      return 1;
    }
  }
  return 0;
}

/* Native OBJT records retain declarations that have no CODE entry.  Walk the same inheritance
 * chain as event dispatch and stop at the first matching declaration: an empty child event is
 * still a declaration (and therefore must not inherit a parent's handler implicitly). */
int gml_vm_instances_native_event_declared(
    GmlVM *vm, int evtype, int subtype, int obj,
    int *handler_obj, int *code){
  if(!vm || obj<0 || obj>=vm->n_objects) return 0;
  for(int p=obj; p>=0 && p<vm->n_objects; p=vm->objects[p].parent){
    GmlObject *object=&vm->objects[p];
    for(int e=0;e<object->n_events;e++){
      if(object->events[e].evtype!=evtype || object->events[e].subtype!=subtype) continue;
      if(handler_obj) *handler_obj=p;
      if(code) *code=object->events[e].code;
      return 1;
    }
  }
  return 0;
}
static double gml_vm_instances_profile_now(GmlVM *vm);
/* find+run `suffix` starting from object level `obj`, walking up the parent chain. Tracks the
 * current event (suffix + object level) so event_inherited can re-dispatch to the parent. */
static int run_event_from(GmlVM *vm, GmlInstance *in, const char *suffix, int obj){
  int handler_obj=-1, ci=-1;
  { /* GML_LOG_EVENT=<object name>: trace every event dispatched to that object. */
    if(!vm->diagnostics.event_filter_initialized){
      const char *filter=anygm_host_development_setting(vm->host,"GML_LOG_EVENT");
      snprintf(vm->diagnostics.event_filter,sizeof vm->diagnostics.event_filter,"%s",filter?filter:"");
      vm->diagnostics.event_filter_initialized=1;
    }
    const char *filter=vm->diagnostics.event_filter;
    if(filter[0] && in && in->obj>=0 && in->obj<vm->n_objects && vm->objects[in->obj].name &&
       (!strcmp(filter,"*") || !strcmp(vm->objects[in->obj].name,filter)))
      anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[event] f%ld %s.%s id=%u other=%u\n",
        vm->frame,vm->objects[in->obj].name,suffix,in->id,vm->cur_other?vm->cur_other->id:0); }
  if(!gml_vm_instances_event_lookup(vm,suffix,obj,&handler_obj,&ci)) return 0;
  GML_VM_DIAGNOSTIC_EVENT(vm,in,suffix,ci);
  /* Preserve `other`: an event fired from inside another instance's scope
   * (event_user / event_perform / action_inherited) must see the caller as
   * `other`. For an engine-triggered event there is no separate caller; GM
   * exposes the event instance itself as both `self` and `other`. This matters
   * for ordinary Step/Create code that deliberately enters `with(other.id)`. */
  GmlInstance *other=vm->cur_other?vm->cur_other:in;
  /* GML_DBG_EVTIME accumulates wall time per handler object and event, then emits a periodic
   * development report. */
  { if(vm->diagnostics.event_time_enabled<0)
      vm->diagnostics.event_time_enabled=anygm_host_development_setting(vm->host,"GML_DBG_EVTIME")!=NULL;
    if(vm->diagnostics.event_time_enabled){
      GmlVmEventProfileEntry *tab=vm->diagnostics.event_profile;
      int *ntab=&vm->diagnostics.event_profile_count;
      double t0=gml_vm_instances_profile_now(vm);
      int r=run_event_code_from(vm,in,other,suffix,handler_obj,ci);
      double dt=gml_vm_instances_profile_now(vm)-t0;
      int k=0; for(;k<*ntab;k++) if(tab[k].object==handler_obj && !strcmp(tab[k].suffix,suffix)) break;
      if(k==*ntab && *ntab<256){
        tab[k].object=handler_obj; snprintf(tab[k].suffix,sizeof tab[k].suffix,"%s",suffix);
        tab[k].milliseconds=0; tab[k].runs=0; (*ntab)++;
      }
      if(k<*ntab){ tab[k].milliseconds+=dt; tab[k].runs++; }
      if(vm->frame!=vm->diagnostics.event_profile_last_frame){
        vm->diagnostics.event_profile_last_frame=vm->frame;
        if(++vm->diagnostics.event_profile_frames%300==0){
          anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[event-time] top over 300 frames:\n");
          for(int pass=0;pass<12;pass++){ int best=-1; double bm=-1;
            for(int j=0;j<*ntab;j++) if(tab[j].milliseconds>bm){ bm=tab[j].milliseconds; best=j; }
            if(best<0 || bm<=0) break;
            anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"   %8.2fms %6ld runs  %s.%s\n",tab[best].milliseconds,tab[best].runs,
              (tab[best].object>=0&&tab[best].object<vm->n_objects)?vm->objects[tab[best].object].name:"?",tab[best].suffix);
            tab[best].milliseconds=-1; }
          *ntab=0; } }
      return r;
    } }
  return run_event_code_from(vm,in,other,suffix,handler_obj,ci);
}
int gml_run_event(GmlVM *vm, GmlInstance *in, const char *suffix){
  if(!in||in->obj<0||in->obj>=vm->n_objects) return 0;
  return run_event_from(vm,in,suffix,in->obj);
}
/* Classic all-instance events are dispatched by ascending exact object resource and insertion
 * order within that object. Snapshot one object group at a time: creations from an earlier object
 * can join a later group, but a same-object creation waits until the next dispatch. The linked
 * lists are newest-first, so reverse the collected slots into insertion order. */
int gml_vm_instances_collect_object_slots(GmlVM *vm, int object){
  if(vm->inst_count>vm->event_ord_cap){
    int nc=vm->event_ord_cap?vm->event_ord_cap:64; while(nc<vm->inst_count) nc*=2;
    int *np=realloc(vm->event_ord,(size_t)nc*sizeof(*np));
    if(np){ vm->event_ord=np; vm->event_ord_cap=nc; }
  }
  if(!vm->event_ord || !vm->obj_head) return -1;
  int count=0;
  for(int i=vm->obj_head[object];i>=0;i=vm->inst_next[i])
    if(vm->inst[i].active && !vm->inst[i].marked && count<vm->event_ord_cap) vm->event_ord[count++]=i;
  return count;
}
/* Sparse classic projects retain authored resource ids, so the object array can contain very
 * large gaps. Cache the ascending object ids that declare each event suffix, including inherited
 * handlers and native empty alarms. This preserves resource-major order without rescanning every
 * empty slot on every frame. Runtime hierarchy mutation resets this cache before the next
 * dispatch. */
typedef struct GmlClassicDispatchCache {
  char suffix[32];
  int *objects, n, cap, used;
} ClassicDispatchCache;
#define CLASSIC_DISPATCH_CACHE_MAX 128
static void classic_dispatch_cache_reset(GmlVM *vm){
  if(!vm || !vm->classic_dispatch) return;
  for(int i=0;i<CLASSIC_DISPATCH_CACHE_MAX;i++) free(vm->classic_dispatch[i].objects);
  free(vm->classic_dispatch);
  vm->classic_dispatch=NULL;
}
const int *gml_vm_instances_event_objects(GmlVM *vm, const char *suffix, int *count){
  *count=0;
  if(!vm || !suffix || strlen(suffix)>=sizeof(((ClassicDispatchCache*)0)->suffix)) return NULL;
  if(!vm->classic_dispatch){
    vm->classic_dispatch=calloc(CLASSIC_DISPATCH_CACHE_MAX,sizeof(*vm->classic_dispatch));
    if(!vm->classic_dispatch) return NULL;
  }
  int slot=-1;
  for(int i=0;i<CLASSIC_DISPATCH_CACHE_MAX;i++){
    if(vm->classic_dispatch[i].used && !strcmp(vm->classic_dispatch[i].suffix,suffix)){
      *count=vm->classic_dispatch[i].n;
      return vm->classic_dispatch[i].n ? vm->classic_dispatch[i].objects : &vm->classic_dispatch_empty;
    }
    if(slot<0 && !vm->classic_dispatch[i].used) slot=i;
  }
  if(slot<0) return NULL;
  ClassicDispatchCache *cache=&vm->classic_dispatch[slot];
  cache->used=1;
  snprintf(cache->suffix,sizeof(cache->suffix),"%s",suffix);
  int alarm_subtype=!strncmp(suffix,"Alarm_",6)?atoi(suffix+6):-1;
  for(int object=0;object<vm->n_objects;object++){
    int native_declared=alarm_subtype>=0 &&
      gml_vm_instances_native_event_declared(vm,2,alarm_subtype,object,NULL,NULL);
    if(!native_declared && !gml_vm_instances_event_lookup(vm,suffix,object,NULL,NULL)) continue;
    if(cache->n>=cache->cap){
      int nc=cache->cap?cache->cap*2:16;
      int *objects=realloc(cache->objects,(size_t)nc*sizeof(*objects));
      if(!objects){ free(cache->objects); memset(cache,0,sizeof(*cache)); return NULL; }
      cache->objects=objects; cache->cap=nc;
    }
    cache->objects[cache->n++]=object;
  }
  *count=cache->n;
  if(anygm_host_development_setting(vm->host,"GML_LOG_CLASSIC_DISPATCH"))
    anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[classic-dispatch] %s objects=%d/%d\n",suffix,cache->n,vm->n_objects);
  return cache->n ? cache->objects : &vm->classic_dispatch_empty;
}
void gml_vm_instances_run_classic_event(GmlVM *vm, const char *suffix){
  if(vm->pending_room>=0) return;
  int object_count=0;
  const int *objects=gml_vm_instances_event_objects(vm,suffix,&object_count);
  int extent=objects?object_count:vm->n_objects;
  for(int oi=0;oi<extent;oi++){
    int object=objects?objects[oi]:oi;
    if(!objects && !gml_vm_instances_event_lookup(vm,suffix,object,NULL,NULL)) continue;
    int count=gml_vm_instances_collect_object_slots(vm,object);
    if(count>=0){
      for(int k=count-1;k>=0;k--){ int i=vm->event_ord[k];
        if(i<vm->inst_count && vm->inst[i].active && !vm->inst[i].marked && vm->inst[i].obj==object){
          gml_run_event(vm,&vm->inst[i],suffix);
          /* A classic room request is committed after the requesting event completes.  Code
           * following room_goto inside that handler has already run, but no later instance from
           * the old room may receive the same engine-dispatched event. */
          if(vm->pending_room>=0) return;
        } }
    } else {
      int extent=vm->inst_count;
      for(int i=0;i<extent;i++) if(vm->inst[i].active && !vm->inst[i].marked && vm->inst[i].obj==object){
        gml_run_event(vm,&vm->inst[i],suffix);
        if(vm->pending_room>=0) return;
      }
    }
  }
}
/* event_inherited(): from inside a child's event, also run the PARENT's version of that same event. */
int gml_event_inherited(GmlVM *vm){
  if(!vm->cur_self||!vm->cur_event||vm->cur_event_obj<0||vm->cur_event_obj>=vm->n_objects) return 0;
  return run_event_from(vm,vm->cur_self,vm->cur_event,vm->objects[vm->cur_event_obj].parent);
}
void gml_vm_instances_link(GmlVM *vm, GmlInstance *in);
void gml_vm_instances_unlink(GmlVM *vm, GmlInstance *in, int obj);
void gml_vm_instances_sort_slots(GmlVM *vm, int *slot, int count){
  /* Portable in-place Shell sort: unlike qsort_r it builds on mingw too, and unlike a global
   * comparator it remains safe if diagnostics ever host more than one VM. */
  for(int gap=count/2;gap>0;gap/=2){
    for(int i=gap;i<count;i++){
      int value=slot[i], j=i;
      uint64_t seq=vm->inst[value].creation_seq;
      while(j>=gap && vm->inst[slot[j-gap]].creation_seq>seq){ slot[j]=slot[j-gap]; j-=gap; }
      slot[j]=value;
    }
  }
}
void gml_vm_instances_prepare_step(GmlVM *vm, int extent){
  vm->step_free_n=vm->step_free_pos=0;
  if(extent<=0) return;
  if(extent>vm->step_free_cap){
    int nc=vm->step_free_cap?vm->step_free_cap:64; while(nc<extent) nc*=2;
    int *p=realloc(vm->step_free,(size_t)nc*sizeof(*p));
    if(!p) return;
    vm->step_free=p; vm->step_free_cap=nc;
  }
  for(int i=0;i<extent;i++){
    GmlInstance *in=&vm->inst[i];
    if(!in->active && !in->deactivated && !in->room_dormant)
      vm->step_free[vm->step_free_n++]=i;
  }
}
GmlInstance *gml_vm_instances_alloc(GmlVM *vm){
  /* A deactivated/dormant instance keeps active=0 but still exists.  During a step, reuse only
   * holes that were already free at frame start: a slot destroyed earlier in this same step can
   * still be referenced by the fixed Studio event snapshot.  The old allocator reserved the
   * ENTIRE prefix instead, so one short-lived effect per frame made the pool (and every scan)
   * grow forever even though almost every slot was dead. */
  if(vm->step_active && vm->step_alloc_base>0){
    while(vm->step_free_pos<vm->step_free_n){
      int i=vm->step_free[vm->step_free_pos++];
      if(i>=0 && i<vm->inst_count && !vm->inst[i].active &&
         !vm->inst[i].deactivated && !vm->inst[i].room_dormant){
        memset(&vm->inst[i],0,sizeof(GmlInstance)); return &vm->inst[i];
      }
    }
  } else {
    for(int i=0;i<vm->inst_count;i++) if(!vm->inst[i].active &&
        !vm->inst[i].deactivated && !vm->inst[i].room_dormant){
      memset(&vm->inst[i],0,sizeof(GmlInstance)); return &vm->inst[i]; }
  }
  if(vm->inst_count>=vm->inst_cap){
    /* pool full: do NOT realloc — moving the array would dangle every held instance pointer
     * (cur_self/cur_other, with-frames, the room-enter loop) and segfault. Reuse the last slot. */
    if(!vm->diagnostics.instance_pool_warned){ vm->diagnostics.instance_pool_warned=1;
      anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[gml] f%ld instance pool full (%d); reusing slots\n",vm->frame,vm->inst_cap); }
    GmlInstance *in=&vm->inst[vm->inst_cap-1];
    if(in->active||in->deactivated){ gml_obj_alive_adjust(vm,in->obj,-1); gml_vm_instances_unlink(vm,in,in->obj); }
    gml_varmap_free(&in->vars); memset(in,0,sizeof(*in)); return in;
  }
  GmlInstance *in=&vm->inst[vm->inst_count++]; memset(in,0,sizeof(*in)); return in;
}
/* Family live counts include an object's descendants and allow skipping queries
 * with no live candidates. Deactivated instances remain counted; exact scans still
 * decide collisions. Rebuild the counts after state load. */
void gml_obj_alive_adjust(GmlVM *vm, int obj, int delta){
  if(!vm->obj_alive) return;
  for(int p=obj; p>=0 && p<vm->n_objects; p=vm->objects[p].parent) vm->obj_alive[p]+=delta;
}
/* per-exact-type instance lists (for family-candidate enumeration in gml_vm_instances_run_collisions) */
void gml_vm_instances_link(GmlVM *vm, GmlInstance *in){
  if(!vm->obj_head || in->obj<0 || in->obj>=vm->n_objects) return;
  int i=(int)(in-vm->inst);
  vm->inst_prev[i]=-1; vm->inst_next[i]=vm->obj_head[in->obj];
  if(vm->obj_head[in->obj]>=0) vm->inst_prev[vm->obj_head[in->obj]]=i;
  vm->obj_head[in->obj]=i; vm->obj_list_gen++;
}
void gml_vm_instances_unlink(GmlVM *vm, GmlInstance *in, int obj){
  if(!vm->obj_head || obj<0 || obj>=vm->n_objects) return;
  int i=(int)(in-vm->inst);
  if(vm->inst_prev[i]>=0) vm->inst_next[vm->inst_prev[i]]=vm->inst_next[i];
  else if(vm->obj_head[obj]==i) vm->obj_head[obj]=vm->inst_next[i];
  else return;   /* not linked (defensive) */
  if(vm->inst_next[i]>=0) vm->inst_prev[vm->inst_next[i]]=vm->inst_prev[i];
  vm->inst_next[i]=vm->inst_prev[i]=-1; vm->obj_list_gen++;
}
void gml_obj_alive_recount(GmlVM *vm){
  if(!vm->obj_alive) return;
  memset(vm->obj_alive,0,(size_t)vm->n_objects*sizeof(int));
  if(vm->obj_head){ for(int o=0;o<vm->n_objects;o++) vm->obj_head[o]=-1;
    for(int i=0;i<vm->inst_cap;i++){ vm->inst_next[i]=vm->inst_prev[i]=-1; } }
  for(int i=0;i<vm->inst_count;i++){ GmlInstance *in=&vm->inst[i];
    if((in->active||in->deactivated) && in->obj>=0 && in->obj<vm->n_objects){
      gml_obj_alive_adjust(vm,in->obj,1); gml_vm_instances_link(vm,in); } }
  vm->obj_list_gen++;
}
void gml_vm_instances_trim_pool_tail(GmlVM *vm){
  /* Pointer-stable compaction: only discard unused slots at the end.  Active instances never
   * move, which is a required property of the fixed pool and of the C API's instance pointers. */
  while(vm->inst_count>0){
    GmlInstance *in=&vm->inst[vm->inst_count-1];
    if(in->active || in->deactivated || in->room_dormant) break;
    vm->inst_count--;
  }
}
void gml_vm_instances_rebase_order(GmlVM *vm){
  /* Room entry is the one place the historical pool deliberately recycled arbitrary holes.
   * Its flat slot order therefore became the format-visible iteration order. Preserve that
   * boundary behavior, then let in-step recycling append logically via creation_seq. */
  uint64_t next=1;
  for(int i=0;i<vm->inst_count;i++){
    GmlInstance *in=&vm->inst[i];
    if(in->active||in->deactivated||in->room_dormant) in->creation_seq=next++;
    else in->creation_seq=0;
  }
  vm->next_creation_seq=next;
}
void gml_vm_instances_initialize(GmlVM *vm, GmlInstance *in, double x, double y, int obj){
  in->active=1; in->marked=0; in->obj=obj; in->id=vm->next_id++;
  if(vm->step_active && vm->step_alloc_base>0){
    if(vm->next_creation_seq==0) vm->next_creation_seq=1;
    in->creation_seq=vm->next_creation_seq++;
  } else {
    uint64_t seq=(uint64_t)(in-vm->inst)+1;
    in->creation_seq=seq;
    if(vm->next_creation_seq<=seq) vm->next_creation_seq=seq+1;
  }
  in->room_owner=vm->room_index;
  gml_obj_alive_adjust(vm,obj,1); gml_vm_instances_link(vm,in);
  in->x=in->xstart=x; in->y=in->ystart=y; in->xprevious=x; in->yprevious=y;
  gml_colgrid_touch(vm,in);   /* fresh instance: unknown to the current grid build */
  in->mask_index=-1;
  in->image_xscale=in->image_yscale=1; in->image_alpha=1; in->image_speed=1;
  in->image_blend=16777215; in->visible=1; in->depth=0;
  in->gravity_direction=270;   /* GM default: gravity pulls straight down */
  in->draw_layer_order=-1;
  in->draw_layer_element_order=-1;
  in->room_placed=0;
  in->path_index=-1; in->path_scale=1; in->path_speed=0; in->path_position=0;
  in->timeline_index=-1; in->timeline_position=0; in->timeline_speed=1;
  in->timeline_running=0; in->timeline_loop=0;
  for(int a=0;a<GML_ALARMS;a++) in->alarm[a]=-1;   /* GM: inactive alarm = -1 */
  if(obj>=0 && obj<vm->n_objects){ GmlObject *o=&vm->objects[obj];
    in->sprite_index=o->sprite_index; in->mask_index=o->mask_index; in->depth=o->depth;
    in->visible=o->visible; in->solid=o->solid; in->persistent=o->persistent; }
}
/* Room instance record stride: 36 (GMS1: ...scale,color,rot), 40 (adds a field), or 48
 * (GMS 2.2.2+: float ImageSpeed/ImageIndex inserted BEFORE color). Reading color at the fixed
 * +28 on 48-byte records grabs ImageSpeed=1.0f (0x3F800000), whose low 24 bits are 0x800000 —
 * a phantom dark-blue tint on every placed instance. Detect the stride once
 * from any room with two consecutive records. */
static int room_inst_stride(GmlVM *vm){
  if(vm->room_rec_stride) return vm->room_rec_stride;
  int stride=36;
  const GmlChunk *c=gml_chunk(vm->win,"ROOM");
  if(c){
    const uint8_t *d=vm->win->data; uint32_t n=gml_vm_read_u32_le(d,c->off);
    for(uint32_t r=0;r<n;r++){
      uint32_t p=gml_vm_read_u32_le(d,c->off+4+r*4); if(!p) continue;
      uint32_t op=gml_vm_read_u32_le(d,p+48); if(!op) continue;
      uint32_t cnt=gml_vm_read_u32_le(d,op);
      if(cnt>=2){ int s=(int)(gml_vm_read_u32_le(d,op+8)-gml_vm_read_u32_le(d,op+4)); if(s>=36 && s<=64){ stride=s; break; } }
    }
  }
  vm->room_rec_stride=stride;
  return stride;
}
int gml_room_instance_precreate_code(GmlVM *vm, uint32_t ip){
  /* Wide bytecode-17 instance records append a placement-variable override body after the
   * transform fields. It is distinct from ordinary instance creation code at +16: object defaults
   * run first, placement overrides run next, then Create observes the resulting values. Earlier
   * record layouts end before this field. */
  if(!vm || !vm->win || anygm_policy_uses_classic_runtime(vm->win) || !anygm_policy_has_modern_function_values(vm->win) ||
     room_inst_stride(vm)<48 || ip>vm->win->size || vm->win->size-ip<48) return -1;
  return (int32_t)gml_vm_read_u32_le(vm->win->data,ip+44);
}
void gml_vm_instances_apply_room_transform(
    GmlVM *vm, GmlInstance *in, uint32_t ip){
  if(!vm || !vm->win || !in) return;
  int wide = room_inst_stride(vm)>=48;   /* ImageSpeed/ImageIndex present; color/rot shifted +8 */
  size_t needed=wide?44u:36u;
  if(ip > vm->win->size || vm->win->size-ip < needed) return;
  const uint8_t *d=vm->win->data;
  float sx=gml_vm_read_f32_le(d,ip+20), sy=gml_vm_read_f32_le(d,ip+24), rot=gml_vm_read_f32_le(d,ip+(wide?40:32));
  if(!isfinite(sx) || !isfinite(sy) || !isfinite(rot)) return;
  if(fabs((double)sx)<1e-9 || fabs((double)sy)<1e-9) return;
  in->image_xscale=sx;
  in->image_yscale=sy;
  in->image_blend=(double)(gml_vm_read_u32_le(d,ip+(wide?36:28))&0xFFFFFFu);
  in->image_angle=rot;
  if(wide){
    float isp=gml_vm_read_f32_le(d,ip+28), iix=gml_vm_read_f32_le(d,ip+32);
    if(isfinite(isp) && isp>=0 && isp<100) in->image_speed=isp;
    if(isfinite(iix) && iix>=0 && iix<10000) in->image_index=iix;
  }
}
static void apply_object_defaults(GmlVM *vm, GmlInstance *in, int obj){
  if(!in || obj<0 || obj>=vm->n_objects) return;
  GmlObject *o=&vm->objects[obj];
  in->obj=obj;
  in->sprite_index=o->sprite_index;
  in->mask_index=o->mask_index;
  in->depth=o->depth;
  in->visible=o->visible;
  in->solid=o->solid;
  in->persistent=o->persistent;
  in->image_index=0;
  in->image_speed=1;
  in->image_xscale=1;
  in->image_yscale=1;
  in->image_angle=0;
  in->image_alpha=1;
  in->image_blend=16777215;
}
static GmlInstance *instance_create_configured(GmlVM *vm, double x, double y, int obj,
                                               int have_depth, double depth, int layer_order){
  GmlInstance *in=gml_vm_instances_alloc(vm); gml_vm_instances_initialize(vm,in,x,y,obj);
  /* GM sets create depth before Create runs, so a Create event that assigns depth wins.
   * instance_create_depth must not overwrite the event's result afterward. */
  if(have_depth) in->depth=depth;
  if(layer_order>=0) in->draw_layer_order=layer_order;
  if(vm->render && (int)in->sprite_index>=0)   /* head start for the background decoder before first draw */
    gml_render_prefetch_sprite((GmlRender*)vm->render,(int)in->sprite_index);
  if(anygm_host_development_setting(vm->host,"GML_LOG_CREATE")){ 
    GmlInstance *caller=vm->cur_self;
    const char *caller_name=(caller&&caller->obj>=0&&caller->obj<vm->n_objects)
      ?vm->objects[caller->obj].name:"?";
    anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[create] f%ld %s id=%u @(%.0f,%.0f) spr=%d caller=%s/%u event=%s\n",vm->frame,
      (obj>=0&&obj<vm->n_objects)?vm->objects[obj].name:"?",in->id,x,y,(int)in->sprite_index,
      caller_name?caller_name:"?",caller?caller->id:0,vm->cur_event?vm->cur_event:"?"); }
  gml_run_event(vm,in,"PreCreate_0");   /* GMS2: runs before Create; sets IDE variable-definitions */
  gml_run_event(vm,in,"Create_0");
  return in;
}
GmlInstance *gml_instance_create_depth(GmlVM *vm, double x, double y, int obj, int have_depth, double depth){
  return instance_create_configured(vm,x,y,obj,have_depth,depth,-1);
}
GmlInstance *gml_instance_create(GmlVM *vm, double x, double y, int obj){
  return gml_instance_create_depth(vm,x,y,obj,0,0);
}
GmlInstance *gml_instance_create_layer(GmlVM *vm, double x, double y, int obj, int layer_id){
  GmlRtLayer *layer=gml_rt_layer_find(vm,layer_id);
  if(!layer) return instance_create_configured(vm,x,y,obj,0,0,-1);
  return instance_create_configured(vm,x,y,obj,1,layer->depth,layer->order);
}
void gml_instance_change(GmlVM *vm, GmlInstance *in, int obj, int perform_events){
  gml_colgrid_touch(vm,in);   /* object swap changes sprite/mask -> bbox */
  if(!vm || !in || !in->active || in->marked || obj<0 || obj>=vm->n_objects) return;
  gml_obj_alive_adjust(vm,in->obj,-1); gml_vm_instances_unlink(vm,in,in->obj);
  gml_obj_alive_adjust(vm,obj,1);       /* relinked with the new type after defaults land */
  if(perform_events){
    gml_run_event(vm,in,"Destroy_0");
    if(!in->active || in->marked){ gml_vm_instances_link(vm,in); return; }   /* died mid-change: keep lists sane until gml_vm_instances_reap */
  }
  apply_object_defaults(vm,in,obj);
  gml_vm_instances_link(vm,in);
  if(perform_events && in->active && !in->marked){ gml_run_event(vm,in,"PreCreate_0"); gml_run_event(vm,in,"Create_0"); }
}
void gml_instance_destroy(GmlVM *vm, GmlInstance *in){
  /* An instance stops being a live destruction target before its Destroy event runs.  Destroy
   * handlers are allowed to call instance_destroy() (directly or through a cleanup script);
   * leaving the pending flag until after the callback re-entered the same handler forever and
   * eventually overflowed the host thread's stack.  Event dispatch itself deliberately does
   * not reject marked instances, so the one required Destroy/CleanUp pair still executes. */
  if(!in||!in->active||in->marked) return;
  in->marked=1;
  gml_run_event(vm,in,"Destroy_0");
  /* Newer Clean Up events fire immediately after Destroy when an instance is disposed of.
   * Per-instance resources are commonly released there. */
  gml_run_event(vm,in,"CleanUp_0");
}
void gml_vm_instances_reap(GmlVM *vm){
  /* skip ESCAPED arrays: `other.arr = my.arr` aliases one GmlArr between instances — freeing
   * it when the owner dies corrupts the survivor (heap-use-after-free -> host hang).
   * Full teardowns (state load / vm free) still free them, deduped. */
  for(int i=0;i<vm->inst_count;i++) if(vm->inst[i].active && vm->inst[i].marked){
    gml_obj_alive_adjust(vm,vm->inst[i].obj,-1); gml_vm_instances_unlink(vm,&vm->inst[i],vm->inst[i].obj);
    gml_varmap_free_ex(&vm->inst[i].vars,1); vm->inst[i].active=0; vm->inst[i].marked=0; }
}

void gml_vm_global_array_set(GmlVM *vm, const char *nm, int idx, double val){
  GmlVal *slot=gml_varmap_put(&vm->globals,nm); GmlArr *A=gml_arr_slot_ensure(slot); gml_arr_index_ensure(A,idx);
  A->escaped=1;
  if(idx>=0 && idx<A->cap) A->data[idx]=vreal(val);
}
double gml_vm_global_array_number(GmlVM *vm, const char *nm, int idx){
  GmlVal *slot=gml_varmap_get(&vm->globals,nm); if(!slot||slot->t!=V_ARR) return 0;
  GmlArr *A=slot->arr;
  return (A && idx>=0 && idx<A->len)?gml_vm_value_as_number(A->data[idx]):0;
}
void gml_set_global_arr(GmlVM *vm, const char *nm, int idx, double val){ gml_vm_global_array_set(vm,nm,idx,val); }
/* Write a global as a plain SCALAR (V_REAL), not as an array. This is the counterpart the generic
 * cheat interface needs for GameMaker Studio games: they read most globals as scalars, and a value
 * left as V_ARR (what gml_vm_global_array_set produces) coerces to 0 in numeric/boolean context (asnum),
 * so an array-write to a scalar-read global silently does nothing. Overwrites the slot in place. */
void gml_set_global_scalar(GmlVM *vm, const char *nm, double val){
  if(!vm||!nm) return;
  GmlVal *slot=gml_varmap_put(&vm->globals,nm); if(slot) *slot=vreal(val);
}
/* Set a numeric instance variable on every active instance of an object or its descendants,
 * resolved by caller-supplied object and variable names. Returns the number of writes. */
int gml_set_inst_var_all(GmlVM *vm, const char *objname, const char *var, double val){
  if(!vm||!objname||!var) return 0;
  int obj=gml_object_index_by_name(vm,objname); if(obj<0) return 0;
  int n=0;
  for(int i=0;i<vm->inst_count;i++){
    GmlInstance *in=&vm->inst[i];
    if(!in->active || in->marked) continue;
    if(!gml_object_is(vm,in->obj,obj)) continue;
    GmlVal v=vreal(val);
    if(gml_vm_instance_builtin_set(vm,in,var,v)){ n++; continue; }
    GmlVal *slot=gml_varmap_put(&in->vars,var); if(slot){ *slot=v; n++; }
  }
  return n;
}
int gml_resize_inst_surface_all(GmlVM *vm,const char *objname,const char *var,int width,int height){
  if(!vm || !vm->render || !objname || !var || width<=0 || height<=0) return 0;
  int obj=gml_object_index_by_name(vm,objname); if(obj<0) return 0;
  int n=0;
  for(int i=0;i<vm->inst_count;i++){
    GmlInstance *in=&vm->inst[i];
    if(!in->active || in->marked || !gml_object_is(vm,in->obj,obj)) continue;
    GmlVal *value=gml_varmap_get(&in->vars,var);
    if(!value || value->t!=V_REAL || !isfinite(value->d) || value->d<0 || value->d>INT_MAX)
      continue;
    int surface=(int)value->d;
    if(surface<0 || !gml_surface_exists((GmlRender*)vm->render,surface)) continue;
    gml_surface_resize((GmlRender*)vm->render,surface,width,height);
    n++;
  }
  return n;
}
/* --- generic pause-menu injection helpers --- */
int gml_inst_get_num(const GmlInstance *in, const char *var){
  if(!in||!var) return 0;
  GmlVal *v=gml_varmap_get((GmlVarMap*)&in->vars,var);
  return (v && v->t==V_REAL) ? (int)v->d : 0;
}
/* Number of existing items in a one- or two-dimensional menu array. */
int gml_inst_array_count(const GmlInstance *in, const char *var){
  if(!in||!var) return 0;
  GmlVal *slot=gml_varmap_get((GmlVarMap*)&in->vars,var);
  if(!slot || slot->t!=V_ARR || !slot->arr) return 0;
  return gml_val_array_length(*slot);
}
/* Set instance-array element [row] (1D, col=0) or [row,col] (2D), reusing the interpreter's own
 * flat 2D layout + metadata path. Frees any prior owned string in the slot so repeated label
 * rewrites don't leak. */
static void inst_array_set(GmlInstance *in, const char *var, int idx, GmlVal v){
  GmlVal *slot=gml_varmap_put(&in->vars,var); if(!slot) return;
  GmlArr *A=gml_arr_slot_ensure(slot);
  A->escaped=1;
  gml_arr_note_legacy_2d_set(A,idx); gml_arr_index_ensure(A,idx);
  if(idx>=0 && idx<A->cap){
    if(A->data[idx].t==V_STR && A->data[idx].d && A->data[idx].s) free((char*)A->data[idx].s);
    A->data[idx]=v;
  }
}
/* Flat index: 1D arrays index by row; 2D arrays use the interpreter's row*GML_2D_STRIDE+col layout. */
static int inst_array_idx(int is2d, int row, int col){ return is2d ? row*GML_2D_STRIDE + col : row; }
void gml_inst_array_set_str(GmlInstance *in, const char *var, int is2d, int row, int col, const char *str){
  if(!in||!var||!str) return;
  char *c=strdup(str);
  inst_array_set(in,var,inst_array_idx(is2d,row,col), c?vstr_owned(c):vstr(""));
}
void gml_inst_array_set_num(GmlInstance *in, const char *var, int is2d, int row, int col, double num){
  if(!in||!var) return;
  inst_array_set(in,var,inst_array_idx(is2d,row,col), vreal(num));
}



void gml_vm_instances_run_classic_triggers(GmlVM *vm, int moment){
  const GmlChunk *chunk=vm && vm->win ? gml_chunk(vm->win,"TRIG") : NULL;
  if(!chunk || chunk->size<4) return;
  const uint8_t *data=vm->win->data;
  uint32_t count=gml_vm_read_u32_le(data,chunk->off);
  if(count>(chunk->size-4)/12) return;
  for(uint32_t i=0;i<count;i++){
    uint32_t entry=chunk->off+4+i*12;
    int trigger_id=(int32_t)gml_vm_read_u32_le(data,entry);
    int trigger_moment=(int32_t)gml_vm_read_u32_le(data,entry+4);
    int code_index=(int32_t)gml_vm_read_u32_le(data,entry+8);
    if(trigger_moment!=moment || code_index<0 || code_index>=vm->win->n_code) continue;
    char suffix[32]; snprintf(suffix,sizeof(suffix),"Trigger_%d",trigger_id);
    if(vm->win && anygm_policy_uses_classic_runtime(vm->win)){
      int object_count=0;
      const int *objects=gml_vm_instances_event_objects(vm,suffix,&object_count);
      int extent=objects?object_count:vm->n_objects;
      for(int oi=0;oi<extent;oi++){
        int object=objects?objects[oi]:oi;
        if(!objects && !gml_vm_instances_event_lookup(vm,suffix,object,NULL,NULL)) continue;
        int count=gml_vm_instances_collect_object_slots(vm,object); if(count<0) continue;
        for(int k=count-1;k>=0;k--){ int slot=vm->event_ord[k];
          if(slot>=vm->inst_count) continue;
          GmlInstance *in=&vm->inst[slot];
          if(!in->active||in->marked||in->obj!=object) continue;
          int old_type=vm->event_type, old_number=vm->event_number;
          vm->event_type=11; vm->event_number=trigger_id;
          GmlVal result=gml_vm_run_code(vm,code_index,in,NULL,NULL,0);
          vm->event_type=old_type; vm->event_number=old_number;
          int fire=gml_vm_value_is_true(result);
          if(result.t==V_STR && result.d!=0) free((char*)result.s);
          if(fire && in->active && !in->marked) gml_run_event(vm,in,suffix);
        }
      }
      continue;
    }
    GmlInstance scratch;
    memset(&scratch,0,sizeof(scratch));
    scratch.active=1; scratch.obj=-1; scratch.id=0;
    scratch.image_xscale=scratch.image_yscale=1; scratch.image_alpha=1;
    scratch.sprite_index=-1; scratch.mask_index=-1; scratch.path_index=-1;
    scratch.timeline_index=-1; scratch.timeline_speed=1;
    for(int a=0;a<GML_ALARMS;a++) scratch.alarm[a]=-1;
    GmlVal result=gml_vm_run_code(vm,code_index,&scratch,NULL,NULL,0);
    int fire=gml_vm_value_is_true(result);
    if(result.t==V_STR && result.d!=0) free((char*)result.s);
    gml_varmap_free_ex(&scratch.vars,0);
    if(!fire) continue;
    int n=vm->inst_count;
    for(int instance=0;instance<n;instance++)
      if(vm->inst[instance].active && !vm->inst[instance].marked)
        gml_run_event(vm,&vm->inst[instance],suffix);
  }
}

int gml_vm_instances_step_snapshot_member(GmlVM *vm, const GmlInstance *in){
  return !vm->step_active || !vm->win || !anygm_policy_snapshot_instance_iteration(vm->win) ||
         in->id < vm->step_first_id;
}
static double gml_vm_instances_profile_now(GmlVM *vm){
  return (double)anygm_host_monotonic_time_ns(vm?vm->host:NULL)/1000000.0;
}



/* ---- collision events (Collision_<targetobj> handlers) ---- */
static int inst_mask_sprite_index(GmlInstance *in){
  return in->mask_index>=0 ? (int)in->mask_index : (int)in->sprite_index;
}
static int vm_room_has_physics(GmlVM *vm){
  if(!vm || !vm->win || vm->room_index<0) return 0;
  GmlVal *dynamic_room=gml_varmap_get(&vm->globals,"__physics_world_scale_room");
  if(dynamic_room && dynamic_room->t==V_REAL && (int)dynamic_room->d==vm->room_index)
    return 1;
  const GmlChunk *room=gml_chunk(vm->win,"ROOM");
  if(!room) return 0;
  uint64_t end=(uint64_t)room->off+room->size;
  uint64_t slot=(uint64_t)room->off+4u+(uint64_t)(uint32_t)vm->room_index*4u;
  if(slot+4u>end) return 0;
  uint32_t record=gml_vm_read_u32_le(vm->win->data,(uint32_t)slot);
  return record>=room->off && (uint64_t)record+60u<=end &&
         gml_vm_read_u32_le(vm->win->data,record+56u)==1;
}
static int vm_fixture_active(GmlVM *vm,GmlInstance *in){
  if(!vm || !in || in->obj<0 || in->obj>=vm->n_objects || !vm_room_has_physics(vm))
    return 0;
  GmlObject *object=&vm->objects[in->obj];
  return object->physics_enabled && object->physics_shape>=1 &&
         object->physics_shape<=2 && object->physics_point_count>=3;
}
static void vm_fixture_vertex(const GmlObject *object,const GmlInstance *in,int index,
                              double *x,double *y){
  double px=object->physics_point[index][0], py=object->physics_point[index][1];
  double angle=in->image_angle*M_PI/180.0, c=cos(angle), s=sin(angle);
  *x=in->x+px*c+py*s;
  *y=in->y-px*s+py*c;
}
static int vm_fixture_bbox(GmlVM *vm,GmlInstance *in,
                           double *left,double *top,double *right,double *bottom){
  if(!vm_fixture_active(vm,in)) return 0;
  GmlObject *object=&vm->objects[in->obj];
  double l=1e30,t=1e30,r=-1e30,b=-1e30;
  for(int point=0;point<object->physics_point_count;point++){
    double x,y; vm_fixture_vertex(object,in,point,&x,&y);
    if(x<l) l=x;
    if(x>r) r=x;
    if(y<t) t=y;
    if(y>b) b=y;
  }
  *left=l; *top=t; *right=r; *bottom=b;
  return l<=r && t<=b;
}
static int vm_fixture_axis_contact(const GmlObject *first,const GmlInstance *a,
                                   const GmlObject *second,const GmlInstance *b,
                                   double axis_x,double axis_y,
                                   double *least_overlap,double *normal_x,double *normal_y){
  double length=hypot(axis_x,axis_y);
  if(length<1e-12) return 1;
  axis_x/=length; axis_y/=length;
  double first_min=1e30,first_max=-1e30,second_min=1e30,second_max=-1e30;
  for(int point=0;point<first->physics_point_count;point++){
    double x,y; vm_fixture_vertex(first,a,point,&x,&y);
    double projection=x*axis_x+y*axis_y;
    if(projection<first_min) first_min=projection;
    if(projection>first_max) first_max=projection;
  }
  for(int point=0;point<second->physics_point_count;point++){
    double x,y; vm_fixture_vertex(second,b,point,&x,&y);
    double projection=x*axis_x+y*axis_y;
    if(projection<second_min) second_min=projection;
    if(projection>second_max) second_max=projection;
  }
  double overlap=fmin(first_max,second_max)-fmax(first_min,second_min);
  if(overlap < -1e-9) return 0;
  if(overlap<*least_overlap){
    double first_centre=(first_min+first_max)*0.5;
    double second_centre=(second_min+second_max)*0.5;
    if(second_centre<first_centre){ axis_x=-axis_x; axis_y=-axis_y; }
    *least_overlap=overlap;
    *normal_x=axis_x;
    *normal_y=axis_y;
  }
  return 1;
}
static int vm_fixture_contact(GmlVM *vm,GmlInstance *a,GmlInstance *b,
                              double *normal_x,double *normal_y,double *penetration){
  if(!vm_fixture_active(vm,a) || !vm_fixture_active(vm,b)) return 0;
  GmlObject *first=&vm->objects[a->obj], *second=&vm->objects[b->obj];
  if(first->physics_group<0 && first->physics_group==second->physics_group) return 0;
  double least_overlap=1e30,nx=0.0,ny=0.0;
  const GmlObject *objects[2]={first,second};
  const GmlInstance *instances[2]={a,b};
  for(int polygon=0;polygon<2;polygon++){
    const GmlObject *object=objects[polygon];
    const GmlInstance *instance=instances[polygon];
    for(int point=0;point<object->physics_point_count;point++){
      int next=(point+1)%object->physics_point_count;
      double x0,y0,x1,y1;
      vm_fixture_vertex(object,instance,point,&x0,&y0);
      vm_fixture_vertex(object,instance,next,&x1,&y1);
      double axis_x=-(y1-y0),axis_y=x1-x0;
      if(fabs(axis_x)+fabs(axis_y)<1e-12) continue;
      if(!vm_fixture_axis_contact(first,a,second,b,axis_x,axis_y,
                                  &least_overlap,&nx,&ny)) return 0;
    }
  }
  if(least_overlap==1e30) return 0;
  if(normal_x) *normal_x=nx;
  if(normal_y) *normal_y=ny;
  if(penetration) *penetration=least_overlap>0.0?least_overlap:0.0;
  return 1;
}
static void vm_fixture_resolve(GmlVM *vm,GmlInstance *a,GmlInstance *b,
                               double normal_x,double normal_y,double penetration){
  if(!vm || !a || !b || penetration<=0.0) return;
  GmlObject *first=&vm->objects[a->obj], *second=&vm->objects[b->obj];
  if(first->physics_sensor || second->physics_sensor) return;
  double first_mass=(!first->physics_kinematic && first->physics_density>0.0)
    ? first->physics_density*first->physics_area_px : 0.0;
  double second_mass=(!second->physics_kinematic && second->physics_density>0.0)
    ? second->physics_density*second->physics_area_px : 0.0;
  double first_inverse=first_mass>0.0?1.0/first_mass:0.0;
  double second_inverse=second_mass>0.0?1.0/second_mass:0.0;
  double inverse_sum=first_inverse+second_inverse;
  if(inverse_sum<=0.0) return;
  /* Apply one minimum-translation correction with a small contact slop.
   * Mass weighting handles static and unequal-density contacts. */
  double correction=fmax(0.0,penetration-0.01);
  double first_share=first_inverse/inverse_sum;
  double second_share=second_inverse/inverse_sum;
  a->x-=normal_x*correction*first_share;
  a->y-=normal_y*correction*first_share;
  b->x+=normal_x*correction*second_share;
  b->y+=normal_y*correction*second_share;
  gml_colgrid_touch(vm,a);
  gml_colgrid_touch(vm,b);
}
static int vm_bbox_at(GmlVM *vm, GmlInstance *in, double atx, double aty,
                      double *l, double *t, double *r, double *b){
  GmlRender *R=(GmlRender*)vm->render; if(!R) return 0;
  int si=inst_mask_sprite_index(in);
  GmlRenderSpriteMetrics sprite;
  if(si<0 || !gml_render_sprite_metrics(R,si,&sprite) ||
     sprite.collision_right<sprite.collision_left ||
     sprite.collision_bottom<sprite.collision_top) return 0;
  double xs=in->image_xscale, ys=in->image_yscale;
  if(fabs(xs)<1e-9 || fabs(ys)<1e-9) return 0;
  double ang=in->image_angle*M_PI/180.0, c=cos(ang), sn=sin(ang);
  double minx=1e30,miny=1e30,maxx=-1e30,maxy=-1e30;
  double x0, x1, y0, y1;
  int round_bounds=anygm_policy_round_collision_bounds(vm->win);
  if(round_bounds){
    x0=(sprite.collision_left-sprite.origin_x)*xs;
    y0=(sprite.collision_top-sprite.origin_y)*ys;
    x1=x0+(sprite.collision_right+1.0-sprite.collision_left)*xs-1.0;
    y1=y0+(sprite.collision_bottom+1.0-sprite.collision_top)*ys-1.0;
  } else {
    x0=sprite.collision_left; x1=sprite.collision_right+1.0;
    y0=sprite.collision_top; y1=sprite.collision_bottom+1.0;
  }
  double corners[4][2]={{x0,y0},{x1,y0},{x0,y1},{x1,y1}};
  for(int i=0;i<4;i++){
    double px, py;
    if(round_bounds){ px=corners[i][0]; py=corners[i][1]; }
    else { px=(corners[i][0]-sprite.origin_x)*xs; py=(corners[i][1]-sprite.origin_y)*ys; }
    double wx=atx + px*c + py*sn;
    double wy=aty - px*sn + py*c;
    if(wx<minx) minx=wx;
    if(wx>maxx) maxx=wx;
    if(wy<miny) miny=wy;
    if(wy>maxy) maxy=wy;
  }
  if(round_bounds){
    *l=gml_vm_classic_round_even(minx); *t=gml_vm_classic_round_even(miny);
    *r=gml_vm_classic_round_even(maxx); *b=gml_vm_classic_round_even(maxy);
  } else {
    *l=floor(minx); *t=floor(miny); *r=ceil(maxx)-1.0; *b=ceil(maxy)-1.0;
  }
  return 1;
}
int gml_vm_instances_bbox(GmlVM *vm, GmlInstance *in,
                          double *l, double *t, double *r, double *b){
  return vm_bbox_at(vm,in,in->x,in->y,l,t,r,b);
}
int gml_vm_instance_bbox(GmlVM *vm, GmlInstance *instance,
                         double *left, double *top, double *right, double *bottom){
  return gml_vm_instances_bbox(vm,instance,left,top,right,bottom);
}
static int vm_overlap(double l1,double t1,double r1,double b1,double l2,double t2,double r2,double b2){
  return l1<=r2 && l2<=r1 && t1<=b2 && t2<=b1;
}
static int vm_mask_hit_world(GmlVM *vm,GmlRender *R,GmlInstance *in,
                             const GmlRenderSpriteMetrics *metrics,
                             int sprite_index,int wx,int wy){
  double xs=in->image_xscale, ys=in->image_yscale;
  if(fabs(xs)<1e-9 || fabs(ys)<1e-9) return 0;
  double ang=in->image_angle*M_PI/180.0, c=cos(ang), sn=sin(ang);
  double ox=in->x, oy=in->y;
  if(vm->win && anygm_policy_uses_classic_runtime(vm->win)){
    ox=gml_vm_classic_round_even(ox); oy=gml_vm_classic_round_even(oy);
  }
  double rx=(double)wx-ox, ry=(double)wy-oy;
  double sxr=rx*c - ry*sn, syr=rx*sn + ry*c;
  int lx=(int)floor(sxr/xs + metrics->origin_x);
  int ly=(int)floor(syr/ys + metrics->origin_y);
  return gml_sprite_collision(R,sprite_index,(int)in->image_index,lx,ly);
}
static int vm_masks_overlap(GmlVM *vm, GmlInstance *a, GmlInstance *b,
                            double l1,double t1,double r1,double b1,
                            double l2,double t2,double r2,double b2){
  GmlRender *R=(GmlRender*)vm->render; if(!R) return 1;
  int as=inst_mask_sprite_index(a), bs=inst_mask_sprite_index(b); if(as<0||bs<0) return 1;
  GmlRenderSpriteMetrics ap,bp;
  if(!gml_render_sprite_metrics(R,as,&ap) || !gml_render_sprite_metrics(R,bs,&bp)) return 1;
  int x0=(int)floor(fmax(l1,l2)), x1=(int)ceil(fmin(r1,r2)+1.0);
  int y0=(int)floor(fmax(t1,t2)), y1=(int)ceil(fmin(b1,b2)+1.0);
  if(x1==x0) x1++;
  if(y1==y0) y1++;
  for(int wy=y0; wy<y1; wy++) for(int wx=x0; wx<x1; wx++){
    if(!vm_mask_hit_world(vm,R,a,&ap,as,wx,wy)) continue;
    if( vm_mask_hit_world(vm,R,b,&bp,bs,wx,wy)) return 1;
  }
  return 0;
}
/* Keyboard events: collect the unique Keyboard_N, KeyPress_N, and KeyRelease_N suffixes present
 * in CODE names. Fire them per step for event-driven input that does not poll keyboard_check. */
static void parse_key_events(GmlVM *vm){
  GmlWin *w=vm->win; vm->n_key_events=0;
  static const struct { const char *tag; int kind; } K[3]=
    {{"_Keyboard_",0},{"_KeyPress_",1},{"_KeyRelease_",2}};
  for(int i=0;i<w->n_code && vm->n_key_events<64;i++){
    const char *nm=w->code[i].name;
    if(strncmp(nm,"gml_Object_",11)) continue;
    for(int k=0;k<3;k++){
      const char *cp=strstr(nm,K[k].tag); if(!cp) continue;
      const char *num=cp+strlen(K[k].tag);
      if(num[0]<'0'||num[0]>'9') continue;
      char suffix[24]; snprintf(suffix,sizeof suffix,"%s%s",K[k].tag+1,num);
      int dup=0;
      for(int e=0;e<vm->n_key_events;e++) if(!strcmp(vm->key_events[e].suffix,suffix)){ dup=1; break; }
      if(!dup){
        vm->key_events[vm->n_key_events].vk=atoi(num);
        vm->key_events[vm->n_key_events].kind=K[k].kind;
        snprintf(vm->key_events[vm->n_key_events].suffix,24,"%s",suffix);
        vm->n_key_events++;
      }
      break;
    }
  }
  if(vm->win && anygm_policy_uses_classic_runtime(vm->win)){
    for(int i=0;i<vm->n_key_events;i++) for(int j=i+1;j<vm->n_key_events;j++){
      int swap=vm->key_events[j].kind<vm->key_events[i].kind ||
        (vm->key_events[j].kind==vm->key_events[i].kind && vm->key_events[j].vk<vm->key_events[i].vk);
      if(swap){ typeof(vm->key_events[0]) t=vm->key_events[i]; vm->key_events[i]=vm->key_events[j]; vm->key_events[j]=t; }
    }
  }
}
/* Instance Mouse_<n> events: collect the unique subtypes present in CODE names. GM fires them
 * per step from pointer state, including per-instance press, idle, and leave events. */
static void parse_mouse_events(GmlVM *vm){
  GmlWin *w=vm->win; vm->n_mouse_events=0;
  for(int i=0;i<w->n_code && vm->n_mouse_events<32;i++){
    const char *nm=w->code[i].name;
    if(strncmp(nm,"gml_Object_",11)) continue;
    const char *cp=strstr(nm,"_Mouse_"); if(!cp) continue;
    const char *num=cp+7;
    if(num[0]<'0'||num[0]>'9') continue;
    char suffix[20]; snprintf(suffix,sizeof suffix,"Mouse_%s",num);
    int dup=0;
    for(int e=0;e<vm->n_mouse_events;e++) if(!strcmp(vm->mouse_events[e].suffix,suffix)){ dup=1; break; }
    if(!dup){
      vm->mouse_events[vm->n_mouse_events].sub=atoi(num);
      snprintf(vm->mouse_events[vm->n_mouse_events].suffix,20,"%s",suffix);
      vm->n_mouse_events++;
    }
  }
  if(vm->win && anygm_policy_uses_classic_runtime(vm->win))
    for(int i=0;i<vm->n_mouse_events;i++) for(int j=i+1;j<vm->n_mouse_events;j++)
      if(vm->mouse_events[j].sub<vm->mouse_events[i].sub){
        typeof(vm->mouse_events[0]) t=vm->mouse_events[i]; vm->mouse_events[i]=vm->mouse_events[j]; vm->mouse_events[j]=t;
      }
}
static void parse_col_events(GmlVM *vm){
  GmlWin *w=vm->win; int cap=0;
  for(int i=0;i<w->n_code;i++) if(strstr(w->code[i].name,"_Collision_")) cap++;
  for(int object=0;object<vm->n_objects;object++)
    for(int event=0;event<vm->objects[object].n_events;event++)
      if(vm->objects[object].events[event].evtype==4) cap++;
  vm->col_events=calloc(cap>0?cap:1,sizeof(GmlColEvent)); vm->n_col_events=0;
  /* Native OBJT event records retain the numeric collision subtype even when Studio emits a
   * GUID in the CODE name. This is the authoritative mapping for those packages. */
  for(int object=0;object<vm->n_objects;object++){
    GmlObject *source=&vm->objects[object];
    for(int event=0;event<source->n_events;event++){
      if(source->events[event].evtype!=4) continue;
      int target=source->events[event].subtype, code=source->events[event].code;
      if(target<0 || target>=vm->n_objects || code<0 || code>=w->n_code) continue;
      vm->col_events[vm->n_col_events++]=(GmlColEvent){object,target,code};
    }
  }
  for(int i=0;i<w->n_code;i++){ const char *nm=w->code[i].name;
    const char *cp=strstr(nm,"_Collision_"); if(!cp || strncmp(nm,"gml_Object_",11)) continue;
    int olen=(int)(cp-(nm+11)); if(olen<=0||olen>=120) continue;
    char obj[128]; memcpy(obj,nm+11,olen); obj[olen]=0;
    int selfobj=gml_object_index_by_name(vm,obj); if(selfobj<0) continue;
    /* The collision subtype is a target-object index in bc14-16 and a target-object name in newer
     * exports. Parsing a name as an integer silently binds the event to object zero. */
    const char *tgt=cp+11; int target;
    if(tgt[0]>='0' && tgt[0]<='9') target=atoi(tgt);
    else target=gml_object_index_by_name(vm,tgt);
    if(target<0) continue;
    int duplicate=0;
    for(int event=0;event<vm->n_col_events;event++)
      if(vm->col_events[event].self_obj==selfobj &&
         vm->col_events[event].target_obj==target){ duplicate=1; break; }
    if(duplicate) continue;
    vm->col_events[vm->n_col_events++]=(GmlColEvent){selfobj, target, i};
  }
  /* Per-object outer filter for gml_vm_instances_run_collisions: only instances whose object or ancestor owns a
   * Collision_* handler can fire one. Without the flag, the quadratic pair loop examines large
   * groups of non-colliding instances every frame. */
  for(int o=0;o<vm->n_objects;o++){ vm->objects[o].colself=0;
    for(int p=o; p>=0 && p<vm->n_objects; p=vm->objects[p].parent){
      for(int e=0;e<vm->n_col_events;e++) if(vm->col_events[e].self_obj==p){ vm->objects[o].colself=1; break; }
      if(vm->objects[o].colself) break;
    }
  }
}
static int col_event_exact(GmlVM *vm, int self_obj, int target_obj){
  for(int e=0;e<vm->n_col_events;e++){
    GmlColEvent *ce=&vm->col_events[e];
    if(ce->self_obj==self_obj && ce->target_obj==target_obj) return ce->code;
  }
  return -1;
}
static int col_event_for_pair(GmlVM *vm, int self_obj, int other_obj,
                              int *handler_obj, int *target_obj, int *code){
  if(vm->col_pair_cache && vm->col_pair_cache_cap>0){
    uint32_t h=(uint32_t)self_obj*1103515245u ^ (uint32_t)other_obj*2654435761u;
    GmlColPairCache *c=&vm->col_pair_cache[h & (uint32_t)(vm->col_pair_cache_cap-1)];
    if(c->valid && c->self_obj==self_obj && c->other_obj==other_obj){
      if(c->code<0) return 0;
      if(handler_obj) *handler_obj=c->handler_obj;
      if(target_obj) *target_obj=c->target_obj;
      if(code) *code=c->code;
      return 1;
    }
    int ho=-1, to=-1, ci=-1, found=0;
    for(int so=self_obj; so>=0 && so<vm->n_objects && !found; so=vm->objects[so].parent){
      for(int ot=other_obj; ot>=0 && ot<vm->n_objects; ot=vm->objects[ot].parent){
        ci=col_event_exact(vm,so,ot);
        if(ci>=0){ ho=so; to=ot; found=1; break; }
      }
    }
    c->valid=1; c->self_obj=self_obj; c->other_obj=other_obj;
    c->handler_obj=ho; c->target_obj=to; c->code=found?ci:-1;
    if(!found) return 0;
    if(handler_obj) *handler_obj=ho;
    if(target_obj) *target_obj=to;
    if(code) *code=ci;
    return 1;
  }
  for(int so=self_obj; so>=0 && so<vm->n_objects; so=vm->objects[so].parent){
    for(int to=other_obj; to>=0 && to<vm->n_objects; to=vm->objects[to].parent){
      int ci=col_event_exact(vm,so,to);
      if(ci>=0){
        if(handler_obj) *handler_obj=so;
        if(target_obj) *target_obj=to;
        if(code) *code=ci;
        return 1;
      }
    }
  }
  return 0;
}
/* Per-frame, per-object-type collision candidates: the union of live instances that IS-A any
 * TARGET of any Collision_* handler on the type's parent chain (exactly the pairs
 * col_event_for_pair can ever accept). A bomb blast spawns ~150 debris of the SAME type whose
 * handlers target tiny families (small target families) — the full-pool inner scan burned ~1.5ms a
 * frame; the shared candidate list makes it proportional to the actual target population.
 * Cached per (type, frame, obj_list_gen); any create/destroy/change bumps the gen. */
static void colcand_reset(GmlVM *vm){
  if(!vm) return;
  for(int i=0;i<48;i++){
    free(vm->collision_candidate[i].slots);
    memset(&vm->collision_candidate[i],0,sizeof(vm->collision_candidate[i]));
  }
  free(vm->collision_candidate_bits);
  vm->collision_candidate_bits=NULL;
  vm->collision_candidate_bits_words=0;
}
void gml_vm_instances_reset_caches(GmlVM *vm){
  classic_dispatch_cache_reset(vm);
  colcand_reset(vm);
}
/* lazy per-object descendant list: "every object that is_a(target)" is computed once per
 * target instead of scanning all objects. Runtime hierarchy mutation drops these lists and
 * bumps the instance-list generation before the next collision candidate build. */
static const int *object_descendants(GmlVM *vm, int target, int *count){
  *count=0;
  if(target<0 || target>=vm->n_objects) return NULL;
  if(!vm->obj_desc){
    vm->obj_desc=calloc((size_t)vm->n_objects,sizeof(int*));
    vm->obj_desc_n=calloc((size_t)vm->n_objects,sizeof(int));
    if(!vm->obj_desc || !vm->obj_desc_n){ free(vm->obj_desc); free(vm->obj_desc_n); vm->obj_desc=NULL; vm->obj_desc_n=NULL; return NULL; }
  }
  if(!vm->obj_desc[target]){
    int n=0;
    for(int d=0;d<vm->n_objects;d++) if(gml_object_is(vm,d,target)) n++;
    int *lst=malloc((size_t)(n?n:1)*sizeof(int));
    if(!lst) return NULL;
    n=0;
    for(int d=0;d<vm->n_objects;d++) if(gml_object_is(vm,d,target)) lst[n++]=d;
    vm->obj_desc[target]=lst; vm->obj_desc_n[target]=n;
  }
  *count=vm->obj_desc_n[target];
  return vm->obj_desc[target];
}
static int colcand_get(GmlVM *vm, int obj, int **out){
  
  if(!vm->obj_head || gml_colgrid_mode(vm)==0) return -1;
  int slot=-1;
  for(int k=0;k<48;k++){
    GmlCollisionCandidateCache *cache=&vm->collision_candidate[k];
    if(cache->obj==obj){ slot=k; break; }
    if(slot<0 && cache->obj==0 && cache->frame==0) slot=k;
  }
  if(slot<0) slot=obj%48;
  GmlCollisionCandidateCache *cache=&vm->collision_candidate[slot];
  if(cache->obj==obj && cache->frame==vm->frame && cache->generation==vm->obj_list_gen){
    *out=cache->slots; return cache->count; }
  /* target roots of this type's chain */
  int roots[16]; int nr=0;
  for(int p=obj; p>=0 && p<vm->n_objects && nr<16; p=vm->objects[p].parent)
    for(int e=0;e<vm->n_col_events && nr<16;e++) if(vm->col_events[e].self_obj==p){
      int t=vm->col_events[e].target_obj, dup=0;
      for(int r=0;r<nr;r++) if(roots[r]==t){ dup=1; break; }
      if(!dup) roots[nr++]=t; }
  int words=(vm->inst_count+63)/64;
  if(words>vm->collision_candidate_bits_words){
    uint64_t *bits=realloc(vm->collision_candidate_bits,
                           (size_t)(words?words:1)*sizeof(*bits));
    if(!bits) return -1;
    vm->collision_candidate_bits=bits;
    vm->collision_candidate_bits_words=words;
  }
  memset(vm->collision_candidate_bits,0,(size_t)words*sizeof(*vm->collision_candidate_bits));
  for(int r=0;r<nr;r++){
    int nd=0; const int *dl=object_descendants(vm,roots[r],&nd);
    for(int k=0;k<nd;k++){ int d=dl[k];
      for(int i=vm->obj_head[d]; i>=0; i=vm->inst_next[i])
        if(i<vm->inst_count) vm->collision_candidate_bits[i>>6]|=1ull<<(i&63);
    }
  }
  int n=0;
  for(int w=0;w<words;w++){ uint64_t m=vm->collision_candidate_bits[w];
    while(m){ int b=__builtin_ctzll(m); m&=m-1; int i=(w<<6)|b;
      if(n>=cache->capacity){ int nc=cache->capacity?cache->capacity*2:64;
        int *slots=realloc(cache->slots,(size_t)nc*sizeof(*slots)); if(!slots) return -1;
        cache->slots=slots; cache->capacity=nc; }
      cache->slots[n++]=i; } }
  cache->obj=obj; cache->frame=vm->frame; cache->generation=vm->obj_list_gen; cache->count=n;
  { if(vm->diagnostics.collision_candidate_debug<0)
      vm->diagnostics.collision_candidate_debug=anygm_host_development_setting(vm->host,"GML_DBG_COLCAND")!=NULL;
    if(vm->diagnostics.collision_candidate_debug){
      if(vm->frame!=vm->diagnostics.collision_candidate_last_frame || 1){
        vm->diagnostics.collision_candidate_last_frame=vm->frame;
      anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[colcand] f%ld obj=%d(%s) targets=%d n=%d\n",vm->frame,obj,
        (obj>=0&&obj<vm->n_objects)?vm->objects[obj].name:"?",nr,n); } } }
  *out=cache->slots;
  return n;
}
void gml_vm_instances_run_collisions(GmlVM *vm){
  if(!vm->render) return;
  const char *clog=anygm_host_development_setting(vm->host,"GML_LOG_COLLISION");   /* hoisted: this ran PER PAIR (1.3M getenv/frame) */
  int cmode=gml_colgrid_mode(vm);
  uint64_t *collision_done=NULL;
  int collision_done_n=0, collision_done_cap=0;
  for(int i=0;i<vm->inst_count;i++){ GmlInstance *si=&vm->inst[i];
    if(!si->active||si->marked||si->obj<0||si->obj>=vm->n_objects) continue;
    if(!vm->objects[si->obj].colself) continue;   /* no Collision_* handler anywhere in its chain */
    double l1,t1,r1,b1;
    if(!gml_vm_instances_bbox(vm,si,&l1,&t1,&r1,&b1) &&
       !vm_fixture_bbox(vm,si,&l1,&t1,&r1,&b1)) continue;
    int *fcand=NULL; int fcn=-1, fpos=0; long jlin=-1;
    if(cmode==1) fcn=colcand_get(vm,si->obj,&fcand);
    for(int j=0;j<vm->inst_count;j++){
      if(jlin<0 && fcn>=0){
        if(fpos>=fcn) break;
        j=fcand[fpos++];
        if(j<0||j>=vm->inst_count) continue;
      }
      GmlInstance *oi=&vm->inst[j];
      if(oi==si||!oi->active||oi->marked||oi->obj<0||oi->obj>=vm->n_objects) continue;
      int solid_pair=si->solid || oi->solid;
      int classic_pair=solid_pair && vm->win && anygm_policy_uses_classic_runtime(vm->win);
      int fixture_pair=vm_fixture_active(vm,si) && vm_fixture_active(vm,oi);
      int rollback_pair=solid_pair && anygm_policy_previous_solid_coordinates(vm->win);
      uint64_t pair_key=((uint64_t)(unsigned)(i<j?i:j)<<32)|(unsigned)(i<j?j:i);
      int pair_done=0;
      if(classic_pair || fixture_pair)
        for(int p=0;p<collision_done_n;p++) if(collision_done[p]==pair_key){ pair_done=1; break; }
      if(pair_done) continue;
      int handler_obj=-1, target_obj=-1, code=-1;
      if(!col_event_for_pair(vm,si->obj,oi->obj,&handler_obj,&target_obj,&code)) continue;
      if(cmode==2 && vm->obj_head){   /* verify: a firing-capable pair must be in the candidate set */
        int *vc=NULL; int vn=colcand_get(vm,si->obj,&vc), found=0;
        for(int q=0;q<vn;q++) if(vc[q]==j){ found=1; break; }
        if(vn>=0 && !found){ 
          anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[gridcheck] MISMATCH colcand f%ld si_obj=%d oi_obj=%d slot=%d\n",vm->frame,si->obj,oi->obj,j); } }
      double l2,t2,r2,b2;
      if(fixture_pair){
        if(!vm_fixture_bbox(vm,si,&l1,&t1,&r1,&b1) ||
           !vm_fixture_bbox(vm,oi,&l2,&t2,&r2,&b2)) continue;
      } else {
        if(!gml_vm_instances_bbox(vm,si,&l1,&t1,&r1,&b1) ||
           !gml_vm_instances_bbox(vm,oi,&l2,&t2,&r2,&b2)) continue;
      }
      int bbox_hit=vm_overlap(l1,t1,r1,b1,l2,t2,r2,b2);
      double fixture_normal_x=0.0,fixture_normal_y=0.0,fixture_penetration=0.0;
      int mask_hit=bbox_hit ? (fixture_pair?
        vm_fixture_contact(vm,si,oi,&fixture_normal_x,&fixture_normal_y,&fixture_penetration):
        vm_masks_overlap(vm,si,oi,l1,t1,r1,b1,l2,t2,r2,b2)) : 0;
      GML_VM_DIAGNOSTIC_COLLISION(vm,si,oi,code,bbox_hit&&mask_hit);
      if(clog){
        const char *sn=(si->obj>=0&&si->obj<vm->n_objects)?vm->objects[si->obj].name:"?";
        const char *on=(oi->obj>=0&&oi->obj<vm->n_objects)?vm->objects[oi->obj].name:"?";
        if(!*clog || strstr(sn,clog) || strstr(on,clog))
          anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[collision?] f%ld %s id=%u bbox=(%.0f,%.0f,%.0f,%.0f) vs %s id=%u bbox=(%.0f,%.0f,%.0f,%.0f) mask=%d event=Collision_%d\n",
            vm->frame,sn,si->id,l1,t1,r1,b1,on,oi->id,l2,t2,r2,b2,mask_hit,target_obj);
      }
      if(bbox_hit && mask_hit){
        if(fixture_pair)
          vm_fixture_resolve(vm,si,oi,fixture_normal_x,fixture_normal_y,fixture_penetration);
        if(clog){
          const char *sn=(si->obj>=0&&si->obj<vm->n_objects)?vm->objects[si->obj].name:"?";
          const char *on=(oi->obj>=0&&oi->obj<vm->n_objects)?vm->objects[oi->obj].name:"?";
          if(!*clog || strstr(sn,clog) || strstr(on,clog))
            anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[collision] f%ld %s id=%u -> %s id=%u event=Collision_%d\n",
              vm->frame,sn,si->id,on,oi->id,target_obj);
        }
        /* Remember which participant entered the contact during this step. The
         * Collision code gets the first opportunity to resolve the overlap. */
        int oi_moved=oi->x!=oi->xprevious || oi->y!=oi->yprevious;
        int oi_kinematic=oi->hspeed!=0.0 || oi->vspeed!=0.0;
        int si_moved=si->x!=si->xprevious || si->y!=si->yprevious;
        int si_kinematic=si->hspeed!=0.0 || si->vspeed!=0.0;
        /* Transactional solid contacts present both participants at their pre-movement
         * positions. Event code may then resolve the contact explicitly. */
        if(rollback_pair){
          si->x=si->xprevious; si->y=si->yprevious;
          oi->x=oi->xprevious; oi->y=oi->yprevious;
          si->path_position=si->path_positionprevious;
          oi->path_position=oi->path_positionprevious;
          gml_colgrid_touch(vm,si); gml_colgrid_touch(vm,oi);
        }
        char suffix[32]; snprintf(suffix,sizeof suffix,"Collision_%d",target_obj);
        { if(vm->diagnostics.collision_event_time_enabled<0)
            vm->diagnostics.collision_event_time_enabled=anygm_host_development_setting(vm->host,"GML_DBG_EVTIME")!=NULL;
          if(vm->diagnostics.collision_event_time_enabled){ double t0=gml_vm_instances_profile_now(vm);
            run_event_code_from(vm,si,oi,suffix,handler_obj,code);
            double dt=gml_vm_instances_profile_now(vm)-t0;
            if(dt>0.5) anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[event-time] slow collision f%ld %s vs %s: %.2fms\n",vm->frame,
              (si->obj>=0&&si->obj<vm->n_objects)?vm->objects[si->obj].name:"?",
              (oi->obj>=0&&oi->obj<vm->n_objects)?vm->objects[oi->obj].name:"?",dt);
          } else run_event_code_from(vm,si,oi,suffix,handler_obj,code); }
        if(classic_pair || fixture_pair){
          /* Classic solid collisions and physics contacts are pair transactions. The contact is
           * calculated once, then both directed handlers observe that same resolved contact. */
          if(si->active && !si->marked && oi->active && !oi->marked){
            int reverse_handler=-1, reverse_target=-1, reverse_code=-1;
            if(col_event_for_pair(vm,oi->obj,si->obj,&reverse_handler,&reverse_target,&reverse_code)){
              char reverse_suffix[32]; snprintf(reverse_suffix,sizeof reverse_suffix,"Collision_%d",reverse_target);
              run_event_code_from(vm,oi,si,reverse_suffix,reverse_handler,reverse_code);
            }
          }
          if(classic_pair && si->active && !si->marked){ si->x+=si->hspeed; si->y+=si->vspeed; gml_colgrid_touch(vm,si); }
          if(classic_pair && oi->active && !oi->marked){ oi->x+=oi->hspeed; oi->y+=oi->vspeed; gml_colgrid_touch(vm,oi); }
          if(classic_pair && si->active && !si->marked && oi->active && !oi->marked){
            double cl1,ct1,cr1,cb1,cl2,ct2,cr2,cb2;
            int still_hit=gml_vm_instances_bbox(vm,si,&cl1,&ct1,&cr1,&cb1) &&
                          gml_vm_instances_bbox(vm,oi,&cl2,&ct2,&cr2,&cb2) &&
                          vm_overlap(cl1,ct1,cr1,cb1,cl2,ct2,cr2,cb2) &&
                          vm_masks_overlap(vm,si,oi,cl1,ct1,cr1,cb1,cl2,ct2,cr2,cb2);
            if(still_hit){
              si->x=si->xprevious; si->y=si->yprevious; si->path_position=si->path_positionprevious;
              oi->x=oi->xprevious; oi->y=oi->yprevious; oi->path_position=oi->path_positionprevious;
              gml_colgrid_touch(vm,si); gml_colgrid_touch(vm,oi);
            }
          }
          if(collision_done_n>=collision_done_cap){
            int nc=collision_done_cap?collision_done_cap*2:16;
            uint64_t *np=realloc(collision_done,(size_t)nc*sizeof(*np));
            if(np){ collision_done=np; collision_done_cap=nc; }
          }
          if(collision_done_n<collision_done_cap) collision_done[collision_done_n++]=pair_key;
        }
        /* If Collision code left the pair intersecting, apply the solid fallback
         * to the participant that entered the contact. Classic actions that stop
         * an incoming motion restore the pre-contact coordinate too; motion that
         * remains active stays under the event's explicit contact resolution. */
        int oi_stopped=oi->hspeed==0.0 && oi->vspeed==0.0;
        if(!fixture_pair && !rollback_pair && si->active && !si->marked &&
           oi->active && !oi->marked &&
           si->solid && oi_moved && (!oi_kinematic ||
             (vm->win && anygm_policy_uses_classic_runtime(vm->win) && oi_stopped))){
          double pl1,pt1,pr1,pb1,pl2,pt2,pr2,pb2;
          int post_hit=gml_vm_instances_bbox(vm,si,&pl1,&pt1,&pr1,&pb1) &&
                       gml_vm_instances_bbox(vm,oi,&pl2,&pt2,&pr2,&pb2) &&
                       vm_overlap(pl1,pt1,pr1,pb1,pl2,pt2,pr2,pb2) &&
                       vm_masks_overlap(vm,si,oi,pl1,pt1,pr1,pb1,pl2,pt2,pr2,pb2);
          if(post_hit){
            oi->x=oi->xprevious; oi->y=oi->yprevious; gml_colgrid_touch(vm,oi);
          }
        }
        /* If a stopped non-classic handler remains inside a stationary solid after its
         * Collision event, restore its frame-start position even without prior motion.
         * Classic retains the moved-into-contact condition; active post-event motion
         * retains the event's explicit resolution. The stationary classic case remains
         * outside this selected policy. */
        int si_stopped=si->hspeed==0.0 && si->vspeed==0.0;
        int si_restorable=(vm->win && anygm_policy_uses_classic_runtime(vm->win))
          ? (si_moved && (!si_kinematic || si_stopped)) : si_stopped;
        if(!fixture_pair && !rollback_pair && si->active && !si->marked &&
           oi->active && !oi->marked &&
           oi->solid && !si->solid && si_restorable){
          double pl1,pt1,pr1,pb1,pl2,pt2,pr2,pb2;
          int post_hit=gml_vm_instances_bbox(vm,si,&pl1,&pt1,&pr1,&pb1) &&
                       gml_vm_instances_bbox(vm,oi,&pl2,&pt2,&pr2,&pb2) &&
                       vm_overlap(pl1,pt1,pr1,pb1,pl2,pt2,pr2,pb2) &&
                       vm_masks_overlap(vm,si,oi,pl1,pt1,pr1,pb1,pl2,pt2,pr2,pb2);
          if(post_hit){
            si->x=si->xprevious; si->y=si->yprevious; gml_colgrid_touch(vm,si);
          }
        }
        if(!si->active||si->marked) break;   /* self destroyed by the event */
        /* the event may have mutated the world (and the shared candidate cache): finish this
         * si with the plain linear scan from the next slot — identical event sequence */
        if(jlin<0 && fcn>=0){ jlin=j; fcn=-1; }
      }
    }
  }
  free(collision_done);
}
/* Precompute boundary handlers (including inherited ones). Bits 0..1 are room events,
 * 2..9 are Outside View 0..7 and 10..17 are Intersect View 0..7. */
static void parse_boundary_events(GmlVM *vm){
  for(int o=0;o<vm->n_objects;o++){ int bits=0; char name[160];
    for(int phase=0;phase<18;phase++){
      int sub=phase<2?phase:(phase<10?38+phase:40+phase);
      for(int p=o; p>=0 && p<vm->n_objects; p=vm->objects[p].parent){
        snprintf(name,sizeof name,"gml_Object_%s_Other_%d",vm->objects[p].name,sub);
        if(gml_code_index_by_name(vm->win,name)>=0){ bits|=1<<phase; break; }
      }
    }
    vm->objects[o].bevents=bits;
  }
}
void gml_vm_instances_parse_objects(GmlVM *vm){
  parse_objects(vm);
}
void gml_vm_instances_parse_boundary_events(GmlVM *vm){
  parse_boundary_events(vm);
}
void gml_vm_instances_parse_dispatch_events(GmlVM *vm){
  parse_col_events(vm);
  parse_key_events(vm);
  parse_mouse_events(vm);
}

int gml_object_set_parent(GmlVM *vm, int object, int parent){
  if(!vm || object<0 || object>=vm->n_objects) return 0;
  if(parent<0 || parent>=vm->n_objects) parent=-1;
  if(parent==object) return 0;
  for(int p=parent, guard=0; p>=0 && p<vm->n_objects && guard++<vm->n_objects;
      p=vm->objects[p].parent)
    if(p==object) return 0;
  if(vm->objects[object].parent==parent) return 1;
  vm->objects[object].parent=parent;

  classic_dispatch_cache_reset(vm);
  if(vm->event_cache) memset(vm->event_cache,0,(size_t)vm->event_cache_cap*sizeof(*vm->event_cache));
  if(vm->col_pair_cache) memset(vm->col_pair_cache,0,(size_t)vm->col_pair_cache_cap*sizeof(*vm->col_pair_cache));
  if(vm->obj_desc){
    for(int i=0;i<vm->n_objects;i++){ free(vm->obj_desc[i]); vm->obj_desc[i]=NULL; }
    if(vm->obj_desc_n) memset(vm->obj_desc_n,0,(size_t)vm->n_objects*sizeof(*vm->obj_desc_n));
  }
  for(int o=0;o<vm->n_objects;o++){
    vm->objects[o].colself=0;
    for(int p=o; p>=0 && p<vm->n_objects; p=vm->objects[p].parent){
      for(int e=0;e<vm->n_col_events;e++) if(vm->col_events[e].self_obj==p){ vm->objects[o].colself=1; break; }
      if(vm->objects[o].colself) break;
    }
  }
  parse_boundary_events(vm);
  gml_obj_alive_recount(vm);
  colcand_reset(vm);
  gml_colgrid_invalidate(vm);
  return 1;
}
/* fire the room/view boundary "Other" events for instances whose bbox left the room/view. GM:
 * "Outside" = bbox entirely outside; "Intersect Boundary" = bbox not entirely inside (partly OR fully out). */
static void classic_boundary_box(GmlVM *vm, GmlInstance *in,
                                 double *l, double *t, double *r, double *b){
  if(gml_vm_instances_bbox(vm,in,l,t,r,b)) return;
  /* Sprite-less classic instances use their point, with the same directed rounding as the
   * historical rectangle test. Boundary events are commonly used by invisible controllers. */
  *l=floor(in->x); *t=floor(in->y); *r=ceil(in->x); *b=ceil(in->y);
}
/* Resolve the world rectangle of a Studio view from a live bound camera first,
 * then fall back to the legacy view arrays. Boundary checks and rendering
 * use the same camera coordinates. */
static void studio_view_world_rect(GmlVM *vm,int view,
                                   double *x,double *y,double *w,double *h){
  int camera=(int)gml_vm_global_array_number(vm,"view_camera",view);
  if(camera>=0 && camera<GML_CAMERA_LIMIT &&
     gml_vm_global_array_number(vm,"__gml_camera_live",camera)>=0.5){
    double camera_w=gml_vm_global_array_number(vm,"__gml_camera_w",camera);
    double camera_h=gml_vm_global_array_number(vm,"__gml_camera_h",camera);
    if(camera_w>0 && camera_h>0){
      *x=gml_vm_global_array_number(vm,"__gml_camera_x",camera);
      *y=gml_vm_global_array_number(vm,"__gml_camera_y",camera);
      *w=camera_w;
      *h=camera_h;
      return;
    }
  }
  *x=gml_vm_global_array_number(vm,"view_xview",view);
  *y=gml_vm_global_array_number(vm,"view_yview",view);
  *w=gml_vm_global_array_number(vm,"view_wview",view);
  *h=gml_vm_global_array_number(vm,"view_hview",view);
}
void gml_vm_instances_run_boundary_events(GmlVM *vm){
  if(!vm->render) return;
  GmlRoom rm; int hr=(gml_vm_room_get(vm,vm->room_index,&rm)==0);
  double vx,vy,vw,vh;
  studio_view_world_rect(vm,0,&vx,&vy,&vw,&vh);
  int has_view=(vw>0 && vh>0);
  /* Classic dispatch completes one boundary subtype at a time, grouped by exact object
   * resource inside that subtype. Keep Studio's instance-major dispatch below unchanged. */
  if(vm->win && anygm_policy_uses_classic_runtime(vm->win)){
    int view_count=0;
    if(hr && rm.view_ptr){ view_count=(int)gml_vm_read_u32_le(vm->win->data,rm.view_ptr); if(view_count>8)view_count=8; }
    for(int phase=0;phase<18;phase++){
      int bit=1<<phase, sub=phase<2?phase:(phase<10?38+phase:40+phase);
      int view=phase<2?-1:(phase<10?phase-2:phase-10);
      if((view<0 && !hr) || (view>=0 && view>=view_count)) continue;
      double x1=0,y1=0,x2=rm.width,y2=rm.height;
      if(view>=0){
        x1=gml_vm_global_array_number(vm,"view_xview",view); y1=gml_vm_global_array_number(vm,"view_yview",view);
        x2=x1+gml_vm_global_array_number(vm,"view_wview",view);
        y2=y1+gml_vm_global_array_number(vm,"view_hview",view);
      }
      char suffix[16]; snprintf(suffix,sizeof suffix,"Other_%d",sub);
      for(int object=0;object<vm->n_objects;object++){
        if(!(vm->objects[object].bevents&bit)) continue;
        int count=gml_vm_instances_collect_object_slots(vm,object); if(count<0) continue;
        for(int k=count-1;k>=0;k--){ int i=vm->event_ord[k];
          if(i>=vm->inst_count) continue;
          GmlInstance *in=&vm->inst[i];
          if(!in->active||in->marked||in->obj!=object) continue;
          double l,t,r,b; classic_boundary_box(vm,in,&l,&t,&r,&b);
          int outside=phase==0 || (phase>=2 && phase<10);
          int fire=outside ? (r<x1||l>x2||b<y1||t>y2) :
                             !(l>=x1&&r<=x2&&t>=y1&&b<=y2);
          if(fire) gml_run_event(vm,in,suffix);
        }
      }
    }
    return;
  }
  int n=vm->inst_count;
  for(int i=0;i<n;i++){ GmlInstance *in=&vm->inst[i];
    if(!in->active||in->marked||in->obj<0||in->obj>=vm->n_objects) continue;
    int bits=vm->objects[in->obj].bevents; if(!bits) continue;
    double l,t,r,b; if(!gml_vm_instances_bbox(vm,in,&l,&t,&r,&b)) continue;
    if((bits&1) && hr && (r<0||l>rm.width||b<0||t>rm.height)) gml_run_event(vm,in,"Other_0");
    if(!in->active||in->marked) continue;
    if((bits&2) && hr && !(l>=0&&r<=rm.width&&t>=0&&b<=rm.height)
       && (r>=0&&l<=rm.width&&b>=0&&t<=rm.height)) gml_run_event(vm,in,"Other_1");
    if(!in->active||in->marked) continue;
    if((bits&4) && has_view && (r<vx||l>vx+vw||b<vy||t>vy+vh)){
      /* Log the bbox and view rectangle used in this boundary decision. */
      if(anygm_host_development_setting(vm->host,"GML_LOG_EVENT"))
        anygm_host_logf(vm->host,ANYGM_LOG_DEBUG,
          "[event] f%ld boundary Other_40: id=%u bbox=(%.1f,%.1f)-(%.1f,%.1f) view0=(%.1f,%.1f %.1fx%.1f)\n",
          vm->frame,in->id,l,t,r,b,vx,vy,vw,vh);
      gml_run_event(vm,in,"Other_40");
    }
    if(!in->active||in->marked) continue;
    if((bits&(1<<10)) && has_view && !(l>=vx&&r<=vx+vw&&t>=vy&&b<=vy+vh)) gml_run_event(vm,in,"Other_50");
  }
}
