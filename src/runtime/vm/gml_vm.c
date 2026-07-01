/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me> */
/* gml_vm.c — bytecode-14 interpreter. See gml_vm.h. */
#include "gml_vm.h"
#include "gml_render.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static uint32_t u32(const uint8_t *d, uint32_t o){
  return (uint32_t)d[o]|(uint32_t)d[o+1]<<8|(uint32_t)d[o+2]<<16|(uint32_t)d[o+3]<<24;
}
static float f32(const uint8_t *d, uint32_t o){ uint32_t v=u32(d,o); float f; memcpy(&f,&v,4); return f; }

/* ---------------- var map (key = interned name pointer) ---------------- */
/* hash/compare keys by CONTENT (FNV-1a + strcmp), not pointer identity: VM names come from
 * the data.win string table while C-side writers (builtins, room init) use literals — the same
 * variable must hit the same slot regardless of which pointer carries the name. */
static unsigned strhash(const char *p){ unsigned h=2166136261u; while(*p){ h^=(unsigned char)*p++; h*=16777619u; } return h; }
static void varmap_grow(GmlVarMap *m){
  int nc = m->cap? m->cap*2 : 16;
  GmlVarSlot *ns = calloc(nc,sizeof(GmlVarSlot));
  for(int i=0;i<m->cap;i++) if(m->slots[i].key){
    unsigned h=strhash(m->slots[i].key)&(nc-1);
    while(ns[h].key) h=(h+1)&(nc-1);
    ns[h]=m->slots[i];
  }
  free(m->slots); m->slots=ns; m->cap=nc;
}
GmlVal *gml_varmap_get(GmlVarMap *m, const char *key){
  if(!m->cap) return NULL;
  unsigned h=strhash(key)&(m->cap-1);
  while(m->slots[h].key){ if(!strcmp(m->slots[h].key,key)) return &m->slots[h].val; h=(h+1)&(m->cap-1); }
  return NULL;
}
GmlVal *gml_varmap_put(GmlVarMap *m, const char *key){
  if(m->len*4>=m->cap*3) varmap_grow(m);
  unsigned h=strhash(key)&(m->cap-1);
  while(m->slots[h].key){ if(!strcmp(m->slots[h].key,key)) return &m->slots[h].val; h=(h+1)&(m->cap-1); }
  m->slots[h].key=key; m->slots[h].val=vreal(0); m->len++;
  return &m->slots[h].val;
}
/* ---- arrays ---- */
typedef struct { GmlVal *data; int len, cap; } GmlArr;
int gml_val_array_length(GmlVal v){ return (v.t==V_ARR && v.arr)? ((GmlArr*)v.arr)->len : 0; }
static GmlArr *arr_of(GmlVal *slot){
  if(slot->t!=V_ARR){ GmlArr *A=calloc(1,sizeof(GmlArr)); slot->t=V_ARR; slot->arr=A; }
  return (GmlArr*)slot->arr;
}
static void arr_ensure(GmlArr *A, int idx){
  if(idx<0) return;
  if(idx>=A->cap){ int nc=A->cap?A->cap:8; while(nc<=idx) nc*=2;
    A->data=realloc(A->data,nc*sizeof(GmlVal)); for(int i=A->cap;i<nc;i++) A->data[i]=vreal(0); A->cap=nc; }
  if(idx>=A->len) A->len=idx+1;
}
static void varmap_free(GmlVarMap *m){
  for(int i=0;i<m->cap;i++) if(m->slots && m->slots[i].key && m->slots[i].val.t==V_ARR){
    GmlArr *A=m->slots[i].val.arr; if(A){ free(A->data); free(A); } }
  free(m->slots); m->slots=0; m->cap=m->len=0;
}

/* ---------------- helpers ---------------- */
static double asnum(GmlVal v){ return v.t==V_REAL? v.d : (v.s? atof(v.s):0); }
static int    astrue(GmlVal v){ return v.t==V_REAL? (v.d>=0.5) : (v.s&&v.s[0]); } /* GM: real>=0.5 true */

/* ---------------- builtin instance variables ---------------- */
/* returns 1 if name is a builtin and handled */
static int inst_builtin_get(GmlInstance *in, const char *n, GmlVal *out){
  #define B(name,field) if(!strcmp(n,name)){ *out=vreal(in->field); return 1; }
  B("x",x) B("y",y) B("xprevious",xprevious) B("yprevious",yprevious)
  B("xstart",xstart) B("ystart",ystart)
  B("sprite_index",sprite_index) B("image_index",image_index) B("image_speed",image_speed)
  B("image_xscale",image_xscale) B("image_yscale",image_yscale) B("image_angle",image_angle)
  B("image_alpha",image_alpha) B("image_blend",image_blend)
  B("depth",depth) B("visible",visible) B("solid",solid) B("persistent",persistent)
  B("hspeed",hspeed) B("vspeed",vspeed) B("direction",direction) B("speed",speed)
  B("gravity",gravity) B("gravity_direction",gravity_direction) B("friction",friction)
  B("path_index",path_index) B("path_position",path_position) B("path_speed",path_speed)
  B("path_orientation",path_orientation) B("path_scale",path_scale)
  B("path_positionprevious",path_positionprevious) B("path_endaction",path_endaction)
  #undef B
  if(!strcmp(n,"object_index")){ *out=vreal(in->obj); return 1; }
  if(!strcmp(n,"id")){ *out=vreal(in->id); return 1; }
  /* image_single (legacy): -1 while animating, else the frozen sub-image */
  if(!strcmp(n,"image_single")){ *out=vreal(in->image_speed!=0? -1 : in->image_index); return 1; }
  return 0;
}
static int inst_builtin_set(GmlInstance *in, const char *n, GmlVal v){
  double d=asnum(v);
  #define B(name,field) if(!strcmp(n,name)){ in->field=d; return 1; }
  B("x",x) B("y",y) B("xprevious",xprevious) B("yprevious",yprevious)
  B("xstart",xstart) B("ystart",ystart)
  B("sprite_index",sprite_index) B("image_index",image_index) B("image_speed",image_speed)
  B("image_xscale",image_xscale) B("image_yscale",image_yscale) B("image_angle",image_angle)
  B("image_alpha",image_alpha) B("image_blend",image_blend)
  B("depth",depth) B("visible",visible) B("solid",solid) B("persistent",persistent)
  B("gravity",gravity) B("gravity_direction",gravity_direction) B("friction",friction)
  B("path_position",path_position) B("path_speed",path_speed)
  B("path_orientation",path_orientation) B("path_scale",path_scale)
  B("path_positionprevious",path_positionprevious) B("path_endaction",path_endaction)
  #undef B
  if(!strcmp(n,"path_index")){ in->path_index=d; return 1; }   /* set directly = follow that path */
  /* speed/direction/hspeed/vspeed are linked in GM */
  if(!strcmp(n,"hspeed")){ in->hspeed=d; in->speed=hypot(in->hspeed,in->vspeed);
    in->direction=atan2(-in->vspeed,in->hspeed)*180.0/M_PI; return 1; }
  if(!strcmp(n,"vspeed")){ in->vspeed=d; in->speed=hypot(in->hspeed,in->vspeed);
    in->direction=atan2(-in->vspeed,in->hspeed)*180.0/M_PI; return 1; }
  if(!strcmp(n,"direction")){ in->direction=d; in->hspeed=in->speed*cos(d*M_PI/180.0);
    in->vspeed=-in->speed*sin(d*M_PI/180.0); return 1; }
  if(!strcmp(n,"speed")){
    in->speed=d; in->hspeed=in->speed*cos(in->direction*M_PI/180.0);
    in->vspeed=-in->speed*sin(in->direction*M_PI/180.0); return 1; }
  /* image_single (legacy alias): >=0 freezes that sub-image (image_speed 0); -1 resumes */
  if(!strcmp(n,"image_single")){
    if(d>=0){ in->image_index=d; in->image_speed=0; } else in->image_speed=1; return 1; }
  return 0;
}

/* ---------------- variable access by scope ---------------- */
/* resolve an instance-type to the target instance: other, self (negatives), or the
 * first active instance of an object (instance-type >= 0 is an object index). */
static GmlInstance *var_target(GmlVM *vm, int inst){
  if(inst==IT_OTHER) return vm->cur_other;
  if(inst<0)         return vm->cur_self;     /* self, all, noone, etc. -> current self */
  for(int i=0;i<vm->inst_count;i++){ GmlInstance *o=&vm->inst[i];
    if(o->active && !o->marked && gml_object_is(vm,o->obj,inst)) return o; }
  return NULL;
}
/* GM built-in GLOBAL variables: health/lives/score live in global scope even when
 * referenced as self.lives (so an init script' lives=3 is visible to every object). */
static int is_global_builtin(const char *n){
  return !strcmp(n,"health")||!strcmp(n,"lives")||!strcmp(n,"score"); }
static int vm_bbox(GmlVM *vm, GmlInstance *in, double *l, double *t, double *r, double *b);
static GmlVal var_get(GmlVM *vm, int inst, const char *name){
  GmlVal out;
  if(!strcmp(name,"room")) return vreal(vm->room_index);   /* GM built-in: current room index */
  if(!strcmp(name,"keyboard_lastkey")) return vreal(vm->last_key); /* GM: last key pressed */
  if(!strcmp(name,"room_width")||!strcmp(name,"room_height")){   /* GM built-in: current room size */
    GmlRoom r; if(gml_room_get(vm->win,vm->room_index,&r)==0)
      return vreal(name[5]=='w'? (double)r.width : (double)r.height);
    return vreal(0); }
  if(inst==IT_GLOBAL || is_global_builtin(name)){
    GmlVal *p=gml_varmap_get(&vm->globals,name); return p?*p:vreal(0); }
  GmlInstance *self = var_target(vm,inst);
  if(self){
    /* image_number = frame count of the current sprite (needs the renderer) */
    if(!strcmp(name,"image_number")){ GmlRender *R=(GmlRender*)vm->render;
      return vreal(R? gml_sprite_frames(R,(int)self->sprite_index):0); }
    /* bbox_left/right/top/bottom = the instance's collision bounding box (from the sprite mask margins) */
    if(!strncmp(name,"bbox_",5)){ double l,t,r,b;
      if(vm_bbox(vm,self,&l,&t,&r,&b)){
        if(!strcmp(name,"bbox_left"))   return vreal(l);
        if(!strcmp(name,"bbox_right"))  return vreal(r);
        if(!strcmp(name,"bbox_top"))    return vreal(t);
        if(!strcmp(name,"bbox_bottom")) return vreal(b); }
      return vreal(0); }
    if(inst_builtin_get(self,name,&out)) return out;
    GmlVal *p=gml_varmap_get(&self->vars,name); if(p) return *p;
  }
  return vreal(0);
}
static void var_set(GmlVM *vm, int inst, const char *name, GmlVal v){
  if(!strcmp(name,"room")){ vm->pending_room=(int)asnum(v); return; }  /* GM: room=X -> goto room */
  if(inst==IT_GLOBAL || is_global_builtin(name)){ *gml_varmap_put(&vm->globals,name)=v; return; }
  GmlInstance *self = var_target(vm,inst);
  if(self){
    if(inst_builtin_set(self,name,v)) return;
    *gml_varmap_put(&self->vars,name)=v;
  }
}

/* resolve an array/var instance-type to the owning instance. inst_t may be a special
 * scope (self/other), a real instance id (>=100000), or an object index (first instance). */
static GmlInstance *inst_by_id(GmlVM *vm, double idv);   /* fwd */
static GmlInstance *resolve_inst(GmlVM *vm, int inst_t){
  if(inst_t==IT_OTHER) return vm->cur_other;
  if(inst_t<0)         return vm->cur_self;   /* self/all/noone/... */
  return inst_by_id(vm,inst_t);               /* instance id OR object index */
}
/* scope map for array access (global/local/self/other/instance/object) */
static GmlVarMap *scope_map(GmlVM *vm, GmlVarMap *locals, int inst_t){
  if(inst_t==IT_GLOBAL) return &vm->globals;
  if(inst_t==IT_LOCAL)  return locals;
  GmlInstance *s=resolve_inst(vm,inst_t);
  return s? &s->vars : NULL;
}
/* GM built-in room/view ARRAYS (background_index[], view_xview[], ...) are global state,
 * accessed without explicit scope — route them to globals regardless of inst_t. */
static int is_room_global_array(const char *n){
  return !strncmp(n,"background_",11) || !strncmp(n,"view_",5); }
static void array_set(GmlVM *vm, GmlVarMap *locals, int inst_t, const char *nm, int idx, GmlVal v){
  if(!strcmp(nm,"alarm")){ GmlInstance *s=resolve_inst(vm,inst_t);
    if(s && idx>=0 && idx<GML_ALARMS) s->alarm[idx]=v.t==V_REAL?v.d:(v.s?atof(v.s):0); return; }
  GmlVarMap *m=is_room_global_array(nm)? &vm->globals : scope_map(vm,locals,inst_t); if(!m) return;
  GmlVal *slot=gml_varmap_put(m,nm); GmlArr *A=arr_of(slot); arr_ensure(A,idx);
  if(idx>=0 && idx<A->cap) A->data[idx]=v;
}
static GmlVal array_get(GmlVM *vm, GmlVarMap *locals, int inst_t, const char *nm, int idx){
  if(!strcmp(nm,"alarm")){ GmlInstance *s=resolve_inst(vm,inst_t);
    return vreal((s&&idx>=0&&idx<GML_ALARMS)? s->alarm[idx] : -1); }
  GmlVarMap *m=is_room_global_array(nm)? &vm->globals : scope_map(vm,locals,inst_t); if(!m) return vreal(0);
  GmlVal *slot=gml_varmap_get(m,nm); if(!slot||slot->t!=V_ARR) return vreal(0);
  GmlArr *A=slot->arr; return (A && idx>=0 && idx<A->len)? A->data[idx] : vreal(0);
}

/* read/write any var on a specific instance (builtin or custom) */
static GmlVal inst_get_any(GmlInstance *t, const char *nm){
  GmlVal o; if(inst_builtin_get(t,nm,&o)) return o;
  GmlVal *p=gml_varmap_get(&t->vars,nm); return p?*p:vreal(0);
}
static void inst_set_any(GmlInstance *t, const char *nm, GmlVal v){
  if(inst_builtin_set(t,nm,v)) return; *gml_varmap_put(&t->vars,nm)=v;
}
static GmlInstance *inst_by_id(GmlVM *vm, double idv){
  int id=(int)idv;
  for(int i=0;i<vm->inst_count;i++) if(vm->inst[i].active && !vm->inst[i].marked && (int)vm->inst[i].id==id) return &vm->inst[i];
  for(int i=0;i<vm->inst_count;i++) if(vm->inst[i].active && !vm->inst[i].marked && vm->inst[i].obj==id) return &vm->inst[i];
  return NULL;
}
/* public accessor: read a builtin or custom instance variable by name → real value.
 * Returns 0 for absent variables (GM default). */
double gml_inst_var_get(GmlVM *vm, GmlInstance *in, const char *nm){
  if(!in) return 0; GmlVal o; if(inst_builtin_get(in,nm,&o)) return o.t==V_REAL?o.d:0;
  GmlVal *p=gml_varmap_get(&in->vars,nm); return p?(p->t==V_REAL?p->d:0):0;
}

/* ---------------- code lookup ---------------- */
int gml_code_index_by_name(GmlWin *w, const char *name){
  for(int i=0;i<w->n_code;i++) if(!strcmp(w->code[i].name,name)) return i;
  return -1;
}
int gml_code_index_find(GmlWin *w, const char *substr){
  for(int i=0;i<w->n_code;i++) if(strstr(w->code[i].name,substr)) return i;
  return -1;
}

/* ---------------- builtins ---------------- */
static int g_unknown_logged=0;
extern GmlVal gml_builtin_call(GmlVM *vm, const char *name, GmlVal *a, int n);

/* ---------------- interpreter ---------------- */
#define STK 512
GmlVal gml_vm_run_code(GmlVM *vm, int ci, GmlInstance *self, GmlInstance *other,
                       GmlVal *args, int n_args){
  if(ci<0||ci>=vm->win->n_code) return vreal(0);
  GmlWin *w=vm->win; const uint8_t *d=w->data;
  uint32_t start=w->code[ci].start, end=start+w->code[ci].length;
  GmlInstance *save_self=vm->cur_self, *save_other=vm->cur_other;
  vm->cur_self=self; vm->cur_other=other;
  GmlVarMap locals={0};
  for(int i=0;i<n_args && i<16;i++){ char nm[16]; snprintf(nm,sizeof nm,"argument%d",i);
    /* locals keyed by interned pointers won't match transient names; use a tiny array instead */
  }
  GmlVal argv[16]; int argc=n_args<16?n_args:16;
  for(int i=0;i<argc;i++) argv[i]=args[i];

  GmlVal stk[STK]; int sp=0;
  uint32_t pc=start;
  GmlVal ret=vreal(0);
  /* with-statement (pushenv/popenv) loop frames */
  struct { GmlInstance **list; int n, idx; GmlInstance *ss, *so; } withstk[32]; int withsp=0;
  /* string GC: track malloc'd strings so they can be freed at scope exit */
  void **str_gc=NULL; int str_gc_n=0, str_gc_cap=0;
  #define GC_TRACK(ptr) do{ if(str_gc_n>=str_gc_cap){ str_gc_cap=str_gc_cap?str_gc_cap*2:16; str_gc=realloc(str_gc,str_gc_cap*sizeof(void*)); } str_gc[str_gc_n++]=ptr; }while(0)
  int trace = getenv("GML_TRACE") && strstr(w->code[ci].name, getenv("GML_TRACE"));
  while(pc<end){
    GmlInsn in; int sz=gml_decode(d,pc,&in); if(!sz) break;
    uint32_t nextpc=pc+sz;
    if(trace) fprintf(stderr,"  %4u: %-7s t1=%x rt=%02x inst=%d  sp=%d\n",pc-start,gml_op_mnemonic(in.kind),in.type1,in.reftype,in.inst,sp);
    switch(in.kind){
      case OP_PUSH:{
        GmlVal v;
        if(in.type1==DT_INT16) v=vreal(in.sval);
        else if(in.type1==DT_DOUBLE) v=vreal(in.dval);
        else if(in.type1==DT_INT32) v=vreal(in.ival);
        else if(in.type1==DT_INT64) v=vreal((double)in.lval);
        else if(in.type1==DT_STRING) v=vstr(gml_str_by_index(w,in.strindex));
        else if(in.type1==DT_VAR){
          const char *nm=gml_ref_name(w,in.refaddr);
          if(in.reftype==0x00){ /* Array */
            int idx=(int)(sp>0?asnum(stk[--sp]):0); int it=(int)(sp>0?asnum(stk[--sp]):0);
            v=array_get(vm,&locals,it,nm,idx);
          } else if(in.reftype==0x80){ /* StackTop: instance.var */
            GmlVal iv=sp>0?stk[--sp]:vreal(0); GmlInstance *t=inst_by_id(vm,asnum(iv)); v=t?inst_get_any(t,nm):vreal(0);
          } else if(in.inst==IT_LOCAL){
            if(!strncmp(nm,"argument",8)){ int idx=atoi(nm+8); v=(idx>=0&&idx<argc)?argv[idx]:vreal(0); }
            else { GmlVal *pp=gml_varmap_get(&locals,nm); v=pp?*pp:vreal(0); }
          } else v=var_get(vm,in.inst,nm);
        } else v=vreal(0);
        if(sp<STK) stk[sp++]=v;
        break;
      }
      case OP_POP:{
        const char *nm=gml_ref_name(w,in.refaddr);
        if(in.reftype==0x00){ /* Array: value, instancetype, index pushed */
          int idx=(int)(sp>0?asnum(stk[--sp]):0); int it=(int)(sp>0?asnum(stk[--sp]):0);
          GmlVal val=sp>0?stk[--sp]:vreal(0); array_set(vm,&locals,it,nm,idx,val);
        } else if(in.reftype==0x80){ /* StackTop instance.var. GMS quirk: the value/instance push
           * order depends on the value's Type1 — `pop.v.v` (Type1=Variable) pushes [value, instance]
           * (instance on top), every other `pop.<num>.v` pushes [instance, value] (value on top). */
           GmlVal val, iv;
           if(in.type1==DT_VAR){ iv=sp>0?stk[--sp]:vreal(0); val=sp>0?stk[--sp]:vreal(0); }
           else               { val=sp>0?stk[--sp]:vreal(0); iv=sp>0?stk[--sp]:vreal(0); }
           GmlInstance *t=inst_by_id(vm,asnum(iv)); if(t) inst_set_any(t,nm,val);
        } else {
          GmlVal v = sp>0? stk[--sp] : vreal(0);
          if(in.inst==IT_LOCAL){ if(!strncmp(nm,"argument",8)){ int idx=atoi(nm+8); if(idx>=0&&idx<16) argv[idx]=v; }
            else *gml_varmap_put(&locals,nm)=v; }
          else var_set(vm,in.inst,nm,v);
        }
        break;
      }
      case OP_POPZ: if(sp>0) sp--; break;
      case OP_DUP: if(sp>0 && sp<STK){ stk[sp]=stk[sp-1]; sp++; } break;
      case OP_CONV: /* values are dynamically typed; coerce lazily */ break;
      case OP_NEG: if(sp>0) stk[sp-1]=vreal(-asnum(stk[sp-1])); break;
      case OP_NOT: if(sp>0) stk[sp-1]=vreal(!astrue(stk[sp-1])); break;
      case OP_MUL: case OP_DIV: case OP_REM: case OP_MOD: case OP_ADD: case OP_SUB:
      case OP_AND: case OP_OR: case OP_XOR: case OP_SHL: case OP_SHR:{
        if(sp<2) break; GmlVal r=stk[--sp], l=stk[--sp];
        if(in.kind==OP_ADD && l.t==V_STR && r.t==V_STR){
          int la=strlen(l.s), lb=strlen(r.s); char *c=malloc(la+lb+1);
          memcpy(c,l.s,la); memcpy(c+la,r.s,lb+1); stk[sp++]=vstr(c); GC_TRACK(c); break;
        }
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
        stk[sp++]=vreal(o); break;
      }
      case OP_CMP:{
        if(sp<2) break; GmlVal r=stk[--sp], l=stk[--sp]; int res=0;
        if(l.t==V_STR && r.t==V_STR){ int c=strcmp(l.s,r.s);
          switch(in.cmp){case CMP_LT:res=c<0;break;case CMP_LTE:res=c<=0;break;case CMP_EQ:res=c==0;break;
            case CMP_NEQ:res=c!=0;break;case CMP_GTE:res=c>=0;break;case CMP_GT:res=c>0;break;} }
        else { double a=asnum(l), b=asnum(r);
          switch(in.cmp){case CMP_LT:res=a<b;break;case CMP_LTE:res=a<=b;break;case CMP_EQ:res=a==b;break;
            case CMP_NEQ:res=a!=b;break;case CMP_GTE:res=a>=b;break;case CMP_GT:res=a>b;break;} }
        stk[sp++]=vreal(res); break;
      }
      case OP_B:  nextpc = pc + (uint32_t)(in.jump*4); break;
      case OP_BT: { GmlVal v=sp>0?stk[--sp]:vreal(0); if(astrue(v)) nextpc=pc+(uint32_t)(in.jump*4); break; }
      case OP_BF: { GmlVal v=sp>0?stk[--sp]:vreal(0); if(!astrue(v)) nextpc=pc+(uint32_t)(in.jump*4); break; }
      case OP_CALL:{
        const char *nm=gml_ref_name(w,in.refaddr); int na=in.argc;
        GmlVal a[64]; if(na>64) na=64;
        /* GM pushes args in reverse, so arg0 is on top: pop forward -> a[0]=arg0 */
        for(int i=0;i<na;i++) a[i] = sp>0? stk[--sp] : vreal(0);
        GmlVal rv=gml_builtin_call(vm,nm,a,na);
        if(sp<STK) stk[sp++]=rv;
        break;
      }
      case OP_RET: ret = sp>0? stk[--sp]:vreal(0); pc=end; continue;
      case OP_EXIT: ret=vreal(0); pc=end; continue;
      case OP_PUSHENV:{ /* with(target): pop the target, iterate matching instances */
        GmlVal tv = sp>0? stk[--sp] : vreal(0); int T=(int)asnum(tv);
        GmlInstance **list=NULL; int nn=0, capL=0;
        #define WADD(p) do{ if(nn>=capL){ capL=capL?capL*2:8; list=realloc(list,capL*sizeof(void*)); } list[nn++]=(p); }while(0)
        if(T>=100000){ GmlInstance *p=inst_by_id(vm,T); if(p) WADD(p); }
        else if(T==IT_OTHER){ if(vm->cur_other) WADD(vm->cur_other); }
        else if(T==IT_SELF){ if(vm->cur_self) WADD(vm->cur_self); }
        else if(T==IT_ALL){ for(int i=0;i<vm->inst_count;i++) if(vm->inst[i].active&&!vm->inst[i].marked) WADD(&vm->inst[i]); }
        else if(T>=0){ for(int i=0;i<vm->inst_count;i++) if(vm->inst[i].active&&!vm->inst[i].marked&&gml_object_is(vm,vm->inst[i].obj,T)) WADD(&vm->inst[i]); }
        #undef WADD
        if(nn==0 || withsp>=32){ free(list); nextpc = pc + (uint32_t)(in.jump*4); }
        else { withstk[withsp].list=list; withstk[withsp].n=nn; withstk[withsp].idx=0;
          withstk[withsp].ss=vm->cur_self; withstk[withsp].so=vm->cur_other; withsp++;
          vm->cur_other=vm->cur_self; vm->cur_self=list[0]; }
        break; }
      case OP_POPENV:{ /* end of with body: next instance, or restore + fall through */
        if(withsp<=0) break;
        int wi=withsp-1;
        if(++withstk[wi].idx < withstk[wi].n){ vm->cur_self=withstk[wi].list[withstk[wi].idx];
          nextpc = pc + (uint32_t)(in.jump*4); }
        else { vm->cur_self=withstk[wi].ss; vm->cur_other=withstk[wi].so; free(withstk[wi].list); withsp--; }
        break; }
      default: break;
    }
    pc=nextpc;
  }
  while(withsp>0){ withsp--; free(withstk[withsp].list); }  /* free any open with-frames */
  /* string GC: free any concatenated strings still on the stack at scope exit.
   * Strings that were popped and stored into vars/globals (via OP_POP/var_set) have been
   * transferred and must NOT be freed. We only free strings that were never consumed. */
  for(int i=0;i<str_gc_n;i++){
    int still_on_stack=0;
    for(int j=0;j<sp;j++) if(stk[j].t==V_STR && stk[j].s==str_gc[i]) still_on_stack=1;
    if(still_on_stack) free(str_gc[i]);
  }
  free(str_gc);
  #undef GC_TRACK
  varmap_free(&locals);
  vm->cur_self=save_self; vm->cur_other=save_other;
  (void)g_unknown_logged;
  return ret;
}

/* ---------------- path parsing + evaluation (PATH chunk) ---------------- */
static void parse_paths(GmlVM *vm){
  GmlWin *w=vm->win; const GmlChunk *c=gml_chunk(w,"PATH"); if(!c) return;
  const uint8_t *d=w->data; uint32_t base=c->off;
  uint32_t n=u32(d,base); vm->paths=calloc(n>0?n:1,sizeof(GmlPath)); vm->n_paths=n;
  for(uint32_t i=0;i<n;i++){
    uint32_t ep=u32(d,base+4+i*4);
    GmlPath *p=&vm->paths[i];
    /* GMS1.4 PATH entry: [name_ptr:u32][kind:u32(0=straight)][closed:u32][precision:u32]
     * [n_points:u32][points: each x(f32),y(f32),sp(f32)=12B]. The name_ptr must be
     * skipped or the point data is shifted by 8 bytes, reading garbage. */
    p->closed=(int)u32(d,ep+8);
    int npt=(int)u32(d,ep+16);
    p->n=npt; p->pts=calloc(npt>0?npt:1,sizeof(GmlPathPt));
    for(int k=0;k<npt;k++){ uint32_t po=ep+20+k*12;
      p->pts[k].x=f32(d,po); p->pts[k].y=f32(d,po+4); }
    /* cumulative arc length along the polyline (closed paths include the wrap segment) */
    double L=0; p->pts[0].clen=0;
    for(int k=1;k<npt;k++){ double dx=p->pts[k].x-p->pts[k-1].x, dy=p->pts[k].y-p->pts[k-1].y;
      L+=sqrt(dx*dx+dy*dy); p->pts[k].clen=L; }
    if(p->closed && npt>1){ double dx=p->pts[0].x-p->pts[npt-1].x, dy=p->pts[0].y-p->pts[npt-1].y;
      L+=sqrt(dx*dx+dy*dy); }
    p->len=L;
  }
}
/* evaluate a path at position t in [0,1] -> (x,y) in path-local coords */
static void path_eval(GmlPath *p, double t, double *ox, double *oy){
  if(p->n<=0){ *ox=*oy=0; return; }
  if(p->n==1 || p->len<=0){ *ox=p->pts[0].x; *oy=p->pts[0].y; return; }
  if(t<0) t=0; if(t>1) t=1;
  double target=t*p->len;
  int seg=p->closed?p->n:(p->n-1);
  for(int k=0;k<seg;k++){
    int a=k, b=(k+1)%p->n;
    double c0=p->pts[a].clen;
    double c1=(b==0)?p->len:p->pts[b].clen;
    if(target<=c1 || k==seg-1){
      double segl=c1-c0; double f=segl>0?(target-c0)/segl:0;
      *ox=p->pts[a].x+(p->pts[b].x-p->pts[a].x)*f;
      *oy=p->pts[a].y+(p->pts[b].y-p->pts[a].y)*f; return;
    }
  }
  *ox=p->pts[p->n-1].x; *oy=p->pts[p->n-1].y;
}
/* path_start(path,speed,endaction,absolute): begin following a path. */
void gml_path_start(GmlVM *vm, GmlInstance *in, int path, double speed, double endaction, int absolute){
  if(path<0||path>=vm->n_paths) return;
  in->path_index=path; in->path_speed=speed; in->path_endaction=endaction;
  in->path_position=0; in->path_positionprevious=0;
  double px,py; path_eval(&vm->paths[path],0,&px,&py);
  if(absolute){ in->path_xoff=0; in->path_yoff=0; in->x=px; in->y=py; }
  else { in->path_xoff=in->x-px; in->path_yoff=in->y-py; }   /* anchor path to current pos */
}
/* advance every path-following instance one step (called each frame in the movement phase). */
static void run_paths(GmlVM *vm){
  for(int i=0;i<vm->inst_count;i++){ GmlInstance *in=&vm->inst[i];
    if(!in->active||in->marked) continue;
    int pi=(int)in->path_index; if(pi<0||pi>=vm->n_paths) continue;
    GmlPath *p=&vm->paths[pi]; if(p->len<=0){ continue; }
    in->path_positionprevious=in->path_position;
    double scl=in->path_scale!=0?in->path_scale:1;
    in->path_position += (in->path_speed) / (p->len*scl);   /* speed = px/step along the path */
    int ended=0;
    if(in->path_position>=1){
      int ea=(int)in->path_endaction; ended=1;
      if(ea==1||ea==2){ while(in->path_position>=1) in->path_position-=1; }   /* loop / continue */
      else if(ea==3){ in->path_position=1; in->path_speed=-in->path_speed; }  /* reverse */
      else { in->path_position=1; in->path_index=-1; }                        /* 0 = stop at end */
    } else if(in->path_position<0){
      int ea=(int)in->path_endaction; ended=1;
      if(ea==3){ in->path_position=0; in->path_speed=-in->path_speed; }
      else if(ea==1||ea==2){ while(in->path_position<0) in->path_position+=1; }
      else { in->path_position=0; in->path_index=-1; }
    }
    /* snap to the (clamped/wrapped) path position before firing the end event */
    double px,py; path_eval(p,in->path_position,&px,&py);
    in->x=in->path_xoff+px*scl; in->y=in->path_yoff+py*scl;
    /* GM "End of Path" event (Other, subtype 8): fired when the path reaches its end. Many objects
     * chain off this (an intro object flies in on a path, then a later event spawns the enemy). */
    if(ended) gml_run_event(vm,in,"Other_8");
  }
}

/* ---------------- object parsing ---------------- */
static void parse_objects(GmlVM *vm){
  GmlWin *w=vm->win; const GmlChunk *c=gml_chunk(w,"OBJT"); if(!c) return;
  const uint8_t *d=w->data; uint32_t n=u32(d,c->off);
  vm->n_objects=(int)n; vm->objects=calloc(n,sizeof(GmlObject));
  for(uint32_t i=0;i<n;i++){
    uint32_t p=u32(d,c->off+4+i*4);
    GmlObject *o=&vm->objects[i];
    o->name=gml_str_by_ptr(w,u32(d,p));
    o->sprite_index=(int)u32(d,p+4);
    o->visible=(int)u32(d,p+8); o->solid=(int)u32(d,p+12);
    o->depth=(int)u32(d,p+16); o->persistent=(int)u32(d,p+20);
    o->parent=(int)u32(d,p+24);
    /* events parsed lazily via code-name matching for now */
  }
}

/* ---------------- instances / events / rooms ---------------- */
double gml_global_num(GmlVM *vm, const char *name){
  for(int i=0;i<vm->globals.cap;i++){ GmlVarSlot *s=&vm->globals.slots[i];
    if(s->key && !strcmp(s->key,name)) return s->val.t==V_REAL? s->val.d : 0; }
  return 0;
}
double gml_global_arr(GmlVM *vm, const char *name, int idx){
  for(int i=0;i<vm->globals.cap;i++){ GmlVarSlot *s=&vm->globals.slots[i];
    if(s->key && !strcmp(s->key,name) && s->val.t==V_ARR){ GmlArr *A=s->val.arr;
      if(A && idx>=0 && idx<A->len) return A->data[idx].t==V_REAL? A->data[idx].d : 0; } }
  return 0;
}

int gml_object_is(GmlVM *vm, int obj, int target){
  while(obj>=0 && obj<vm->n_objects){ if(obj==target) return 1; obj=vm->objects[obj].parent; }
  return 0;
}
int gml_object_index_by_name(GmlVM *vm, const char *name){
  for(int i=0;i<vm->n_objects;i++) if(vm->objects[i].name && !strcmp(vm->objects[i].name,name)) return i;
  return -1;
}
GmlInstance *gml_find_instance(GmlVM *vm, int obj){
  for(int i=0;i<vm->inst_count;i++) if(vm->inst[i].active && !vm->inst[i].marked && gml_object_is(vm,vm->inst[i].obj,obj)) return &vm->inst[i];
  return NULL;
}
int gml_instance_number(GmlVM *vm, int obj){
  int n=0; for(int i=0;i<vm->inst_count;i++)
    if(vm->inst[i].active && !vm->inst[i].marked && gml_object_is(vm,vm->inst[i].obj,obj)) n++;
  return n;
}
/* find+run `suffix` starting from object level `obj`, walking up the parent chain. Tracks the
 * current event (suffix + object level) so event_inherited can re-dispatch to the parent. */
static int run_event_from(GmlVM *vm, GmlInstance *in, const char *suffix, int obj){
  char name[160];
  while(obj>=0 && obj<vm->n_objects){
    snprintf(name,sizeof name,"gml_Object_%s_%s",vm->objects[obj].name,suffix);
    int ci=gml_code_index_by_name(vm->win,name);
    if(ci>=0){
      const char *pe=vm->cur_event; int peo=vm->cur_event_obj;
      vm->cur_event=suffix; vm->cur_event_obj=obj;
      /* PRESERVE `other`: an event fired from inside another instance's scope (event_user /
       * event_perform / action_inherited, e.g. a projectile's `with(other) event_user(0)`) must see
       * the caller as `other` — else a projectile reads a field as 0 and never
       * dies. For engine-triggered events (Step/Alarm) cur_other is NULL here (saved/restored by
       * gml_vm_run_code), so this is a no-op for them — no regression. */
      gml_vm_run_code(vm,ci,in,vm->cur_other,NULL,0);
      vm->cur_event=pe; vm->cur_event_obj=peo;
      return 1;
    }
    obj=vm->objects[obj].parent;  /* inherited event (automatic) */
  }
  return 0;
}
int gml_run_event(GmlVM *vm, GmlInstance *in, const char *suffix){
  if(!in||in->obj<0||in->obj>=vm->n_objects) return 0;
  return run_event_from(vm,in,suffix,in->obj);
}
/* event_inherited(): from inside a child's event, also run the PARENT's version of that same event. */
int gml_event_inherited(GmlVM *vm){
  if(!vm->cur_self||!vm->cur_event||vm->cur_event_obj<0||vm->cur_event_obj>=vm->n_objects) return 0;
  return run_event_from(vm,vm->cur_self,vm->cur_event,vm->objects[vm->cur_event_obj].parent);
}
static GmlInstance *alloc_inst(GmlVM *vm){
  /* a deactivated instance keeps active=0 but must NOT have its slot reused (it still exists). */
  for(int i=0;i<vm->inst_count;i++) if(!vm->inst[i].active && !vm->inst[i].deactivated){ memset(&vm->inst[i],0,sizeof(GmlInstance)); return &vm->inst[i]; }
  if(vm->inst_count>=vm->inst_cap){
    /* pool full: do NOT realloc — moving the array would dangle every held instance pointer
     * (cur_self/cur_other, with-frames, the room-enter loop) and segfault. Reuse the last slot. */
    static int warned=0; if(!warned){ warned=1;
      fprintf(stderr,"[gml] instance pool full (%d); reusing slots\n",vm->inst_cap); }
    GmlInstance *in=&vm->inst[vm->inst_cap-1]; varmap_free(&in->vars); memset(in,0,sizeof(*in)); return in;
  }
  GmlInstance *in=&vm->inst[vm->inst_count++]; memset(in,0,sizeof(*in)); return in;
}
static void init_inst(GmlVM *vm, GmlInstance *in, double x, double y, int obj){
  in->active=1; in->marked=0; in->obj=obj; in->id=vm->next_id++;
  in->x=in->xstart=x; in->y=in->ystart=y; in->xprevious=x; in->yprevious=y;
  in->image_xscale=in->image_yscale=1; in->image_alpha=1; in->image_speed=1;
  in->image_blend=16777215; in->visible=1; in->depth=0;
  in->gravity_direction=270;   /* GM default: gravity pulls straight down */
  in->path_index=-1; in->path_scale=1; in->path_speed=0; in->path_position=0;
  for(int a=0;a<GML_ALARMS;a++) in->alarm[a]=-1;   /* GM: inactive alarm = -1 */
  if(obj>=0 && obj<vm->n_objects){ GmlObject *o=&vm->objects[obj];
    in->sprite_index=o->sprite_index; in->depth=o->depth;
    in->visible=o->visible; in->solid=o->solid; in->persistent=o->persistent; }
}
GmlInstance *gml_instance_create(GmlVM *vm, double x, double y, int obj){
  GmlInstance *in=alloc_inst(vm); init_inst(vm,in,x,y,obj);
  if(getenv("GML_LOG_CREATE")){ extern long g_vm_frame;
    fprintf(stderr,"[create] f%ld %s @(%.0f,%.0f) spr=%d\n",g_vm_frame,
    (obj>=0&&obj<vm->n_objects)?vm->objects[obj].name:"?",x,y,(int)in->sprite_index); }
  gml_run_event(vm,in,"Create_0");
  return in;
}
void gml_instance_destroy(GmlVM *vm, GmlInstance *in){
  if(!in||!in->active) return;
  gml_run_event(vm,in,"Destroy_0");
  in->marked=1;
}
static void reap(GmlVM *vm){
  for(int i=0;i<vm->inst_count;i++) if(vm->inst[i].active && vm->inst[i].marked){
    varmap_free(&vm->inst[i].vars); vm->inst[i].active=0; vm->inst[i].marked=0; }
}

static void set_global_arr(GmlVM *vm, const char *nm, int idx, double val){
  GmlVal *slot=gml_varmap_put(&vm->globals,nm); GmlArr *A=arr_of(slot); arr_ensure(A,idx);
  if(idx>=0 && idx<A->cap) A->data[idx]=vreal(val);
}
static double get_global_arr_d(GmlVM *vm, const char *nm, int idx){
  GmlVal *slot=gml_varmap_get(&vm->globals,nm); if(!slot||slot->t!=V_ARR) return 0;
  GmlArr *A=slot->arr; return (A && idx>=0 && idx<A->len)? asnum(A->data[idx]) : 0;
}
void gml_room_enter(GmlVM *vm, int room_index){
  /* Fire Room End (Other_5) on active instances before clearing the old room. */
  if(vm->room_index>=0){
    for(int i=0;i<vm->inst_count;i++) if(vm->inst[i].active && !vm->inst[i].marked)
      gml_run_event(vm,&vm->inst[i],"Other_5");
  }
  /* Dispatch destruction and clear non-persistent instances, including deactivated ones. */
  for(int i=0;i<vm->inst_count;i++) if((vm->inst[i].active||vm->inst[i].deactivated) && !vm->inst[i].persistent){
    gml_run_event(vm,&vm->inst[i],"Destroy_0");
    varmap_free(&vm->inst[i].vars); vm->inst[i].active=0; vm->inst[i].deactivated=0; }
  vm->room_index=room_index; vm->pending_room=-1;
  vm->n_tile_mut=0;   /* tile-layer mutations are per-room */
  vm->n_tile_del_at=0; /* tile_layer_delete_at marks are per-room */
  if(getenv("GML_LOG_ROOM")) fprintf(stderr,"[room] enter %d\n",room_index);
  GmlRoom r; if(gml_room_get(vm->win,room_index,&r)!=0) return;
  const uint8_t *d=vm->win->data; uint32_t op=r.obj_ptr, cnt=u32(d,op);
  /* initialise the built-in background_* arrays from the room's background layers — GM does
   * this on room start, and a parallax object reads them to draw the parallax scenery. */
  if(r.bg_ptr){ uint32_t bc=u32(d,r.bg_ptr);
    for(uint32_t i=0;i<bc && i<8;i++){ uint32_t lp=u32(d,r.bg_ptr+4+i*4);
      int en=(int)u32(d,lp), fg=(int)u32(d,lp+4), def=(int)u32(d,lp+8);
      int bx=(int)u32(d,lp+12), by=(int)u32(d,lp+16), htl=(int)u32(d,lp+20), vtl=(int)u32(d,lp+24);
      set_global_arr(vm,"background_visible",i,en?1:0);
      set_global_arr(vm,"background_foreground",i,fg?1:0);
      set_global_arr(vm,"background_index",i,en?def:-1);   /* a parallax object skips layers whose index==-1 */
      set_global_arr(vm,"background_x",i,bx);  set_global_arr(vm,"background_y",i,by);
      set_global_arr(vm,"background_htiled",i,htl?1:0); set_global_arr(vm,"background_vtiled",i,vtl?1:0);
      set_global_arr(vm,"background_alpha",i,1.0);          /* GM default; a parallax object draws *_ext at this alpha */
      if(getenv("GML_LOG_BG")) fprintf(stderr,"[bg-init] layer%u en=%d fg=%d def=%d pos=(%d,%d) htiled=%d vtiled=%d\n",i,en,fg,def,bx,by,htl,vtl);
    } }
  /* view settings → globals (GM auto-follows view_object each step; see gml_vm_step). View record
   * (14 i32): enabled,xv,yv,wv,hv,xport,yport,wport,hport,hborder,vborder,hspeed,vspeed,object. */
  if(r.view_ptr){ uint32_t vc=u32(d,r.view_ptr);
    for(uint32_t i=0;i<vc && i<8;i++){ uint32_t vp=u32(d,r.view_ptr+4+i*4);
      set_global_arr(vm,"view_visible",i,(int)u32(d,vp)?1:0);
      set_global_arr(vm,"view_xview",i,(int32_t)u32(d,vp+4));   set_global_arr(vm,"view_yview",i,(int32_t)u32(d,vp+8));
      set_global_arr(vm,"view_wview",i,(int32_t)u32(d,vp+12));  set_global_arr(vm,"view_hview",i,(int32_t)u32(d,vp+16));
      set_global_arr(vm,"view_xport",i,(int32_t)u32(d,vp+20));  set_global_arr(vm,"view_yport",i,(int32_t)u32(d,vp+24));
      set_global_arr(vm,"view_wport",i,(int32_t)u32(d,vp+28));  set_global_arr(vm,"view_hport",i,(int32_t)u32(d,vp+32));
      set_global_arr(vm,"view_hborder",i,(int32_t)u32(d,vp+36));set_global_arr(vm,"view_vborder",i,(int32_t)u32(d,vp+40));
      set_global_arr(vm,"view_hspeed",i,(int32_t)u32(d,vp+44)); set_global_arr(vm,"view_vspeed",i,(int32_t)u32(d,vp+48));
      set_global_arr(vm,"view_object",i,(int32_t)u32(d,vp+52));
    } }
  /* create instances */
  for(uint32_t i=0;i<cnt;i++){
    uint32_t ip=u32(d,op+4+i*4);
    int32_t x=(int32_t)u32(d,ip), y=(int32_t)u32(d,ip+4); int obj=(int32_t)u32(d,ip+8);
    GmlInstance *in=alloc_inst(vm); init_inst(vm,in,x,y,obj);
    in->id=u32(d,ip+12);  /* room-assigned instance id */
    gml_run_event(vm,in,"Create_0");
    /* per-instance room creation code (ip+16 = CODE index, -1 if none) runs after Create —
     * this is how each room configures its objects (e.g. a background-loader object's backgrounds). */
    int cc=(int32_t)u32(d,ip+16);
    if(cc>=0 && cc<vm->win->n_code) gml_vm_run_code(vm,cc,in,NULL,NULL,0);
  }
  /* Game Start / Room Start fire only on the instances present at room start — SNAPSHOT the
   * count, else an event that creates instances (whose own Other_2/4 also creates) cascades
   * forever (a gate object's Other_4 spawned itself unboundedly → pool overflow). */
  int n0 = vm->inst_count;
  /* Game Start (Other_2), first room only — before room creation code */
  if(!vm->started){ vm->started=1;
    for(int i=0;i<n0;i++) if(vm->inst[i].active && !vm->inst[i].marked)
      gml_run_event(vm,&vm->inst[i],"Other_2"); }
  /* room creation code runs after instances are created (throwaway self in GM) */
  if(r.creation_code>=0 && r.creation_code<vm->win->n_code)
    gml_vm_run_code(vm,r.creation_code,NULL,NULL,NULL,0);
  /* Room Start (Other_4) for the instances that existed at room start */
  n0 = vm->inst_count;
  for(int i=0;i<n0;i++) if(vm->inst[i].active && !vm->inst[i].marked)
    gml_run_event(vm,&vm->inst[i],"Other_4");
  reap(vm);
}
void gml_vm_goto_room_order(GmlVM *vm, int order_index){
  GmlWin *w=vm->win;
  int idx = (order_index>=0 && order_index<w->n_room_order)? (int)w->room_order[order_index] : order_index;
  gml_room_enter(vm,idx);
}

static void run_collisions(GmlVM *vm);
static void run_boundary_events(GmlVM *vm);
long g_vm_frame=0;
void gml_vm_step(GmlVM *vm){
  g_vm_frame++;
  int n=vm->inst_count;
  /* begin step */
  for(int i=0;i<n;i++) if(vm->inst[i].active && !vm->inst[i].marked) gml_run_event(vm,&vm->inst[i],"Step_1");
  /* alarms */
  for(int i=0;i<n;i++){ GmlInstance *in=&vm->inst[i]; if(!in->active||in->marked) continue;
    for(int a=0;a<GML_ALARMS;a++) if(in->alarm[a]>-1){ in->alarm[a]-=1;
      /* fire when the countdown crosses below 0 (not exactly ==-1): GML sometimes sets a FRACTIONAL
       * alarm (e.g. a length-scaled cost for typewriter timing) which would skip the exact -1 and never
       * fire, freezing the cutscene. Set -1 first so the event can re-arm it. */
      if(in->alarm[a]<0){ in->alarm[a]=-1; char s[16]; snprintf(s,sizeof s,"Alarm_%d",a);
        if(getenv("GML_LOG_ALARM")) fprintf(stderr,"[alarm] f%ld %s.%s\n",g_vm_frame,
          (in->obj>=0&&in->obj<vm->n_objects)?vm->objects[in->obj].name:"?",s);
        gml_run_event(vm,in,s); } } }
  /* keyboard events: handled by builtins polling; skip event-based for now */
  /* normal step */
  for(int i=0;i<n;i++) if(vm->inst[i].active && !vm->inst[i].marked) gml_run_event(vm,&vm->inst[i],"Step_0");
  /* movement */
  for(int i=0;i<n;i++){ GmlInstance *in=&vm->inst[i]; if(!in->active||in->marked) continue;
    in->xprevious=in->x; in->yprevious=in->y;
    if(in->gravity!=0){ in->hspeed+=in->gravity*cos(in->gravity_direction*M_PI/180.0);
      in->vspeed-=in->gravity*sin(in->gravity_direction*M_PI/180.0); }
    in->x+=in->hspeed; in->y+=in->vspeed; }
  /* path following (path_start): move instances along their assigned path each step */
  run_paths(vm);
  /* sprite animation + Animation End (Other_7) when the loop wraps past the last frame */
  GmlRender *R=(GmlRender*)vm->render;
  for(int i=0;i<n;i++){ GmlInstance *in=&vm->inst[i]; if(!in->active||in->marked) continue;
    int nf = R? gml_sprite_frames(R,(int)in->sprite_index):0;
    if(nf>1 && in->image_speed!=0){
      double ni=in->image_index+in->image_speed; int wrapped=(ni>=nf)||(ni<0);
      while(ni>=nf) ni-=nf; while(ni<0) ni+=nf; in->image_index=ni;
      /* event may create/destroy instances or stop this anim (image_single); don't touch `in` after */
      if(wrapped){ if(getenv("GML_LOG_ANIM")) fprintf(stderr,"[anim-end] %s (nf=%d)\n",
        (in->obj>=0&&in->obj<vm->n_objects)?vm->objects[in->obj].name:"?",nf);
        gml_run_event(vm,in,"Other_7"); } } }
  /* room/view boundary events (Outside Room/View, Intersect Boundary) — after move */
  run_boundary_events(vm);
  /* collision events (GM order: after move, before end step) */
  run_collisions(vm);
  /* end step */
  for(int i=0;i<n;i++) if(vm->inst[i].active && !vm->inst[i].marked) gml_run_event(vm,&vm->inst[i],"Step_2");
  reap(vm);
  /* Automatic view-follow keeps the selected instance inside the horizontal and
   * vertical border bands, with the view position clamped to the room. */
  if(get_global_arr_d(vm,"view_visible",0)>=0.5){
    int vobj=(int)get_global_arr_d(vm,"view_object",0);
    GmlInstance *fo = vobj>=0 ? gml_find_instance(vm,vobj) : NULL;
    if(fo){
      double vx=get_global_arr_d(vm,"view_xview",0), vy=get_global_arr_d(vm,"view_yview",0);
      double wv=get_global_arr_d(vm,"view_wview",0), hv=get_global_arr_d(vm,"view_hview",0);
      double hb=get_global_arr_d(vm,"view_hborder",0), vb=get_global_arr_d(vm,"view_vborder",0);
      if(fo->x-vx < hb)      vx = fo->x - hb;
      if(fo->x-vx > wv-hb)   vx = fo->x - (wv-hb);
      if(fo->y-vy < vb)      vy = fo->y - vb;
      if(fo->y-vy > hv-vb)   vy = fo->y - (hv-vb);
      GmlRoom rm; if(gml_room_get(vm->win,vm->room_index,&rm)==0){
        double mx=rm.width-wv, my=rm.height-hv;
        if(vx<0)vx=0; if(mx>0&&vx>mx)vx=mx; if(mx<=0)vx=0;
        if(vy<0)vy=0; if(my>0&&vy>my)vy=my; if(my<=0)vy=0;
      }
      set_global_arr(vm,"view_xview",0,vx); set_global_arr(vm,"view_yview",0,vy);
    }
  }
  /* room transition requested during the step */
  if(vm->pending_room>=0){ int t=vm->pending_room; vm->pending_room=-1; gml_room_enter(vm,t); }
  /* Fire Game End (Other_3) on active instances when game_end is set. */
  if(vm->game_end){
    for(int i=0;i<vm->inst_count;i++) if(vm->inst[i].active && !vm->inst[i].marked)
      gml_run_event(vm,&vm->inst[i],"Other_3");
  }
}

/* ---- tile-layer runtime mutations (tile_layer_delete/depth/shift) ----
 * GM tiles live in a tile layer keyed by depth. The room's tiles are read immutably from the
 * data.win each frame, so we keep a small list of per-depth mutations applied at gather time.
 * find-or-create the entry for `depth`. */
static int tile_mut_idx(GmlVM *vm, int depth){
  for(int i=0;i<vm->n_tile_mut;i++) if(vm->tile_mut[i].depth==depth) return i;
  if(vm->n_tile_mut>=64) return -1;
  int i=vm->n_tile_mut++; vm->tile_mut[i].depth=depth; vm->tile_mut[i].deleted=0;
  vm->tile_mut[i].has_remap=0; vm->tile_mut[i].remap=0; vm->tile_mut[i].dx=vm->tile_mut[i].dy=0;
  return i;
}
void gml_tile_layer_delete(GmlVM *vm, int depth){ int i=tile_mut_idx(vm,depth); if(i>=0) vm->tile_mut[i].deleted=1; }
void gml_tile_layer_depth(GmlVM *vm, int depth, int newdepth){ int i=tile_mut_idx(vm,depth);
  if(i>=0){ vm->tile_mut[i].has_remap=1; vm->tile_mut[i].remap=newdepth; } }
void gml_tile_layer_shift(GmlVM *vm, int depth, double dx, double dy){ int i=tile_mut_idx(vm,depth);
  if(i>=0){ vm->tile_mut[i].dx+=dx; vm->tile_mut[i].dy+=dy; } }
void gml_tile_layer_delete_at(GmlVM *vm, int depth, double x, double y){
  if(vm->n_tile_del_at<64){ int i=vm->n_tile_del_at++;
    vm->tile_del_at[i].depth=depth; vm->tile_del_at[i].x=(int)x; vm->tile_del_at[i].y=(int)y; } }
/* apply the mutation for a tile at original `depth`: returns 0 to drop the tile (deleted),
 * else 1 and writes the effective depth + position offset. */
static int tile_apply_mut(GmlVM *vm, int depth, int *eff_depth, double *ox, double *oy){
  *eff_depth=depth; *ox=0; *oy=0;
  for(int i=0;i<vm->n_tile_mut;i++) if(vm->tile_mut[i].depth==depth){
    if(vm->tile_mut[i].deleted) return 0;
    if(vm->tile_mut[i].has_remap) *eff_depth=vm->tile_mut[i].remap;
    *ox=vm->tile_mut[i].dx; *oy=vm->tile_mut[i].dy; return 1;
  }
  return 1;
}

/* Draw instances and room tiles together in descending depth order. */
typedef struct { int x,y,def,sx,sy,w,h; } GmlDrawTile;
typedef struct { double depth; int seq, type, idx; } GmlDrawItem;  /* type: 0=instance, 1=tile */
static int cmp_draw_item(const void *pa, const void *pb){
  const GmlDrawItem *a=pa,*b=pb;
  if(a->depth!=b->depth) return a->depth>b->depth? -1:1;     /* higher depth first (behind) */
  return a->seq<b->seq? -1 : (a->seq>b->seq?1:0);            /* stable tie-break */
}
void gml_vm_draw(GmlVM *vm){
  GmlRender *R=(GmlRender*)vm->render; if(!R) return;
  int n=vm->inst_count;
  /* gather this room's tiles (pointer-list of records: x,y,def,srcx,srcy,w,h,depth,...) */
  GmlDrawTile *tiles=NULL; int nt=0; double *tdepth=NULL;
  GmlRoom rm;
  if(gml_room_get(vm->win,vm->room_index,&rm)==0 && rm.tile_ptr){
    const uint8_t *d=vm->win->data; uint32_t tc=u32(d,rm.tile_ptr);
    if(tc>0 && tc<100000){ tiles=malloc(tc*sizeof(GmlDrawTile)); tdepth=malloc(tc*sizeof(double));
      for(uint32_t i=0;i<tc;i++){ uint32_t p=u32(d,rm.tile_ptr+4+i*4);
        int tx=(int)u32(d,p), ty=(int)u32(d,p+4); int tdep=(int32_t)u32(d,p+28);
        if(vm->n_tile_mut){ int ed; double ox,oy;
          if(!tile_apply_mut(vm,tdep,&ed,&ox,&oy)) continue;   /* deleted layer: drop */
          tdep=ed; tx+=(int)ox; ty+=(int)oy; }
        { int drop=0;
          for(int da=0;da<vm->n_tile_del_at;da++)
            if(vm->tile_del_at[da].depth==tdep && vm->tile_del_at[da].x==tx && vm->tile_del_at[da].y==ty){ drop=1; break; }
          if(drop) continue; }
        tiles[nt].x=tx; tiles[nt].y=ty; tiles[nt].def=(int)u32(d,p+8);
        tiles[nt].sx=(int)u32(d,p+12); tiles[nt].sy=(int)u32(d,p+16);
        tiles[nt].w=(int)u32(d,p+20); tiles[nt].h=(int)u32(d,p+24);
        tdepth[nt]=tdep; nt++; } }
  }
  /* unified depth-sorted draw list of instances + tiles */
  int cap=n+nt; GmlDrawItem *it=malloc((cap>0?cap:1)*sizeof(GmlDrawItem)); int m=0;
  for(int i=0;i<n;i++) if(vm->inst[i].active && !vm->inst[i].marked){
    it[m].depth=vm->inst[i].depth; it[m].type=0; it[m].idx=i; it[m].seq=m; m++; }
  for(int i=0;i<nt;i++){ it[m].depth=tdepth[i]; it[m].type=1; it[m].idx=i; it[m].seq=m; m++; }
  qsort(it,m,sizeof(GmlDrawItem),cmp_draw_item);
  static int dumped=0;
  if(getenv("GML_LOG_INST") && !dumped){ dumped=1;
    fprintf(stderr,"[draw] room=%d, %d instances + %d tiles (back->front):\n",vm->room_index,n,nt);
    for(int k=0;k<m && k<2000;k++){ if(it[k].type){ GmlDrawTile *t=&tiles[it[k].idx];
        fprintf(stderr,"   TILE def=%d depth=%.0f @(%d,%d) %dx%d src=(%d,%d)\n",t->def,it[k].depth,t->x,t->y,t->w,t->h,t->sx,t->sy); }
      else { GmlInstance *in=&vm->inst[it[k].idx];
        fprintf(stderr,"   %-26s spr=%-4d vis=%.0f depth=%.0f @(%.0f,%.0f) ang=%.0f xs=%.1f ys=%.1f a=%.2f\n",
          (in->obj>=0&&in->obj<vm->n_objects)?vm->objects[in->obj].name:"?",
          (int)in->sprite_index,in->visible,in->depth,in->x,in->y,in->image_angle,in->image_xscale,in->image_yscale,in->image_alpha); } } }
  for(int k=0;k<m;k++){
    if(it[k].type==1){ GmlDrawTile *t=&tiles[it[k].idx];
      gml_draw_tile(R,t->def,t->sx,t->sy,t->w,t->h,t->x,t->y); continue; }
    GmlInstance *in=&vm->inst[it[k].idx];
    { const char *sk=getenv("GML_SKIP");                  /* debug: skip drawing a named object */
      if(sk && in->obj>=0 && in->obj<vm->n_objects && vm->objects[in->obj].name
         && strstr(vm->objects[in->obj].name,sk)) continue; }
    if(in->visible<0.5) continue;   /* GM: an invisible instance runs NEITHER its Draw event
                                       nor the automatic sprite draw (a control object hides the HUD
                                       on non-gameplay rooms by setting visible=0 in Room Start). */
    /* Draw event replaces default draw; else draw sprite_index automatically */
    int obj=in->obj, found=0;
    while(obj>=0 && obj<vm->n_objects){ char name[160];
      snprintf(name,sizeof name,"gml_Object_%s_Draw_0",vm->objects[obj].name);
      if(gml_code_index_by_name(vm->win,name)>=0){ found=1; break; }
      obj=vm->objects[obj].parent; }
    if(found) gml_run_event(vm,in,"Draw_0");
    else if(in->sprite_index>=0)
      gml_draw_sprite_ext(R,(int)in->sprite_index,(int)in->image_index,in->x,in->y,
                          in->image_xscale,in->image_yscale,in->image_angle,
                          (uint32_t)in->image_blend,in->image_alpha);
  }
  free(it); free(tiles); free(tdepth);
}

/* Draw GUI pass: execute Draw_64 events in depth order. */
void gml_vm_draw_gui(GmlVM *vm){
  GmlRender *R=(GmlRender*)vm->render; if(!R) return;
  int n=vm->inst_count;
  int *ord=malloc((n>0?n:1)*sizeof(int)); int m=0;
  for(int i=0;i<n;i++){ GmlInstance *in=&vm->inst[i];
    if(!in->active||in->marked||in->visible<0.5) continue;
    int obj=in->obj, found=0;
    while(obj>=0 && obj<vm->n_objects){ char name[160];
      snprintf(name,sizeof name,"gml_Object_%s_Draw_64",vm->objects[obj].name);
      if(gml_code_index_by_name(vm->win,name)>=0){ found=1; break; }
      obj=vm->objects[obj].parent; }
    if(found) ord[m++]=i;
  }
  for(int a=0;a<m;a++) for(int b=a+1;b<m;b++)            /* depth descending (back -> front) */
    if(vm->inst[ord[b]].depth>vm->inst[ord[a]].depth){ int t=ord[a]; ord[a]=ord[b]; ord[b]=t; }
  for(int k=0;k<m;k++) gml_run_event(vm,&vm->inst[ord[k]],"Draw_64");
  free(ord);
}

/* ---- collision events (Collision_<targetobj> handlers) ---- */
static int vm_bbox(GmlVM *vm, GmlInstance *in, double *l, double *t, double *r, double *b){
  GmlRender *R=(GmlRender*)vm->render; if(!R) return 0;
  int si=(int)in->sprite_index; if(si<0||si>=R->n_spr) return 0;
  GmlSprite *s=&R->spr[si]; if(s->mr<s->ml || s->mb<s->mt) return 0;
  *l=in->x-s->originx+s->ml; *r=in->x-s->originx+s->mr;
  *t=in->y-s->originy+s->mt; *b=in->y-s->originy+s->mb; return 1;
}
static int vm_overlap(double l1,double t1,double r1,double b1,double l2,double t2,double r2,double b2){
  return l1<=r2 && l2<=r1 && t1<=b2 && t2<=b1;
}
static int vm_masks_overlap(GmlVM *vm, GmlInstance *a, GmlInstance *b,
                            double l1,double t1,double r1,double b1,
                            double l2,double t2,double r2,double b2){
  GmlRender *R=(GmlRender*)vm->render; if(!R) return 1;
  int as=(int)a->sprite_index, bs=(int)b->sprite_index; if(as<0||bs<0) return 1;
  if(as>=R->n_spr||bs>=R->n_spr) return 1;
  GmlSprite *ap=&R->spr[as], *bp=&R->spr[bs];
  int x0=(int)floor(fmax(l1,l2)), x1=(int)ceil(fmin(r1,r2));
  int y0=(int)floor(fmax(t1,t2)), y1=(int)ceil(fmin(b1,b2));
  if(x1==x0) x1++;
  if(y1==y0) y1++;
  double ax=a->x-ap->originx, ay=a->y-ap->originy;
  double bx=b->x-bp->originx, by=b->y-bp->originy;
  for(int wy=y0; wy<y1; wy++) for(int wx=x0; wx<x1; wx++){
    if(!gml_sprite_collision(R,as,(int)a->image_index,(int)(wx-ax),(int)(wy-ay))) continue;
    if( gml_sprite_collision(R,bs,(int)b->image_index,(int)(wx-bx),(int)(wy-by))) return 1;
  }
  return 0;
}
static void parse_col_events(GmlVM *vm){
  GmlWin *w=vm->win; int cap=0;
  for(int i=0;i<w->n_code;i++) if(strstr(w->code[i].name,"_Collision_")) cap++;
  vm->col_events=calloc(cap>0?cap:1,sizeof(GmlColEvent)); vm->n_col_events=0;
  for(int i=0;i<w->n_code;i++){ const char *nm=w->code[i].name;
    const char *cp=strstr(nm,"_Collision_"); if(!cp || strncmp(nm,"gml_Object_",11)) continue;
    int olen=(int)(cp-(nm+11)); if(olen<=0||olen>=120) continue;
    char obj[128]; memcpy(obj,nm+11,olen); obj[olen]=0;
    int selfobj=gml_object_index_by_name(vm,obj); if(selfobj<0) continue;
    vm->col_events[vm->n_col_events++]=(GmlColEvent){selfobj, atoi(cp+11), i};
  }
}
static void run_collisions(GmlVM *vm){
  if(!vm->render) return;
  for(int e=0;e<vm->n_col_events;e++){ GmlColEvent *ce=&vm->col_events[e];
    for(int i=0;i<vm->inst_count;i++){ GmlInstance *si=&vm->inst[i];
      if(!si->active||si->marked||!gml_object_is(vm,si->obj,ce->self_obj)) continue;
      double l1,t1,r1,b1; if(!vm_bbox(vm,si,&l1,&t1,&r1,&b1)) continue;
      for(int j=0;j<vm->inst_count;j++){ GmlInstance *oi=&vm->inst[j];
        if(oi==si||!oi->active||oi->marked||!gml_object_is(vm,oi->obj,ce->target_obj)) continue;
        double l2,t2,r2,b2; if(!vm_bbox(vm,oi,&l2,&t2,&r2,&b2)) continue;
        if(vm_overlap(l1,t1,r1,b1,l2,t2,r2,b2) && vm_masks_overlap(vm,si,oi,l1,t1,r1,b1,l2,t2,r2,b2)){
          gml_vm_run_code(vm,ce->code,si,oi,NULL,0);
          if(!si->active||si->marked) break;   /* self destroyed by the event */
        }
      }
    }
  }
}
/* precompute which objects have boundary-event handlers (incl. via parent), so run_boundary_events
 * only checks instances that care. bits: 1 OutsideRoom(Other_0) 2 IntersectRoom(Other_1)
 * 4 OutsideView0(Other_40) 8 IntersectView0(Other_50). */
static void parse_boundary_events(GmlVM *vm){
  const struct { int sub, bit; } map[]={{0,1},{1,2},{40,4},{50,8}};
  for(int o=0;o<vm->n_objects;o++){ int bits=0; char name[160];
    for(unsigned m=0;m<sizeof map/sizeof map[0];m++){
      for(int p=o; p>=0 && p<vm->n_objects; p=vm->objects[p].parent){
        snprintf(name,sizeof name,"gml_Object_%s_Other_%d",vm->objects[p].name,map[m].sub);
        if(gml_code_index_by_name(vm->win,name)>=0){ bits|=map[m].bit; break; }
      }
    }
    vm->objects[o].bevents=bits;
  }
}
/* fire the room/view boundary "Other" events for instances whose bbox left the room/view. GM:
 * "Outside" = bbox entirely outside; "Intersect Boundary" = bbox not entirely inside (partly OR fully out). */
static void run_boundary_events(GmlVM *vm){
  if(!vm->render) return;
  GmlRoom rm; int hr=(gml_room_get(vm->win,vm->room_index,&rm)==0);
  double vx=get_global_arr_d(vm,"view_xview",0), vy=get_global_arr_d(vm,"view_yview",0);
  double vw=get_global_arr_d(vm,"view_wview",0), vh=get_global_arr_d(vm,"view_hview",0);
  int has_view=(vw>0 && vh>0);
  int n=vm->inst_count;
  for(int i=0;i<n;i++){ GmlInstance *in=&vm->inst[i];
    if(!in->active||in->marked||in->obj<0||in->obj>=vm->n_objects) continue;
    int bits=vm->objects[in->obj].bevents; if(!bits) continue;
    double l,t,r,b; if(!vm_bbox(vm,in,&l,&t,&r,&b)) continue;
    if((bits&1) && hr && (r<0||l>rm.width||b<0||t>rm.height)) gml_run_event(vm,in,"Other_0");
    if(!in->active||in->marked) continue;
    if((bits&2) && hr && !(l>=0&&r<=rm.width&&t>=0&&b<=rm.height)
       && (r>=0&&l<=rm.width&&b>=0&&t<=rm.height)) gml_run_event(vm,in,"Other_1");
    if(!in->active||in->marked) continue;
    if((bits&4) && has_view && (r<vx||l>vx+vw||b<vy||t>vy+vh)) gml_run_event(vm,in,"Other_40");
    if(!in->active||in->marked) continue;
    if((bits&8) && has_view && !(l>=vx&&r<=vx+vw&&t>=vy&&b<=vy+vh)) gml_run_event(vm,in,"Other_50");
  }
}

/* ---- RNG: WELL512 (Lomont compact form), MSVC-LCG seeding, default seed 0 ----
 * The recurrence follows Chris Lomont's public-domain listing; see LICENSES/WELL512.txt.
 * The seed expansion fills the state using MSVC-LCG arithmetic.
 * random(x) = (next()/2^32)*x. */
void gml_rng_seed(GmlVM *vm, uint32_t seed){
  uint32_t s=seed;
  for(int i=0;i<16;i++){ s=((s*214013u+2531011u)>>16)&0x7fffffffu; vm->rng_well[i]=s; }
  vm->rng_index=0;
}
static uint32_t gml_rng_next(GmlVM *vm){
  uint32_t *st=vm->rng_well; uint32_t idx=vm->rng_index;
  uint32_t a,b,c,d;
  a=st[idx];
  c=st[(idx+13)&15];
  b=a ^ c ^ (a<<16) ^ (c<<15);
  c=st[(idx+9)&15];
  c^=(c>>11);
  st[idx]=a=b^c;
  d=a ^ ((a<<5)&0xDA442D24u);
  idx=(idx+15)&15;
  a=st[idx];
  st[idx]=a=a ^ b ^ d ^ (a<<2) ^ (b<<18) ^ (c<<28);
  vm->rng_index=idx;
  return a;
}
double gml_rng_value(GmlVM *vm){ return (double)gml_rng_next(vm) / 4294967296.0; }  /* [0,1) */

int gml_vm_init(GmlVM *vm, GmlWin *win){
  memset(vm,0,sizeof(*vm));
  vm->win=win; vm->pending_room=-1; vm->room_index=-1; vm->next_id=100000; vm->rng_state=0;
  gml_rng_seed(vm,0);   /* GM default seed */
  parse_objects(vm);
  parse_paths(vm);
  parse_boundary_events(vm);
  parse_col_events(vm);
  vm->inst_cap=16384; vm->inst=calloc(vm->inst_cap,sizeof(GmlInstance)); /* fixed pool: never realloc-move */
  return 0;
}
void gml_vm_free(GmlVM *vm){
  varmap_free(&vm->globals);
  for(int i=0;i<vm->inst_count;i++) varmap_free(&vm->inst[i].vars);
  free(vm->inst);
  for(int i=0;i<vm->n_objects;i++) free(vm->objects[i].events);
  for(int i=0;i<vm->n_paths;i++) free(vm->paths[i].pts);
  free(vm->paths);
  free(vm->objects); free(vm->col_events);
}
