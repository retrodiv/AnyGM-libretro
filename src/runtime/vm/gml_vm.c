/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me> */
/* gml_vm.c — normalized GML bytecode interpreter. See gml_vm.h. */
#include "gml_vm.h"
#include "gml_render.h"
#include "gml_particle.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <limits.h>

static uint32_t u32(const uint8_t *d, uint32_t o){
  return (uint32_t)d[o]|(uint32_t)d[o+1]<<8|(uint32_t)d[o+2]<<16|(uint32_t)d[o+3]<<24;
}
static float f32(const uint8_t *d, uint32_t o){ uint32_t v=u32(d,o); float f; memcpy(&f,&v,4); return f; }
static const char *g_cur_code_name;
static GmlVM *g_cur_vm;

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
#define GML_2D_STRIDE 32000
static void arr_row_ensure(GmlArr *A, int row){
  if(!A || row<0) return;
  if(row>=A->row_cap){
    int nc=A->row_cap?A->row_cap:8; while(nc<=row) nc*=2;
    int *nr=realloc(A->row_len,(size_t)nc*sizeof(int)); if(!nr) return;
    A->row_len=nr; for(int i=A->row_cap;i<nc;i++) A->row_len[i]=0; A->row_cap=nc;
  }
  if(row>=A->height2d) A->height2d=row+1;
}
static void arr_note_2d_set(GmlArr *A, int idx){
  if(!A || idx<0) return;
  if(idx>=GML_2D_STRIDE || A->is_2d){
    if(!A->is_2d){
      A->is_2d=1;
      if(A->len>0){
        arr_row_ensure(A,0);
        A->row_len[0]=A->len<GML_2D_STRIDE?A->len:GML_2D_STRIDE;
      }
    }
    int row=idx/GML_2D_STRIDE, col=idx%GML_2D_STRIDE;
    arr_row_ensure(A,row);
    if(A->row_len && col+1>A->row_len[row]) A->row_len[row]=col+1;
  }
}
static int arr_value_nonzero(GmlVal v){ return !(v.t==V_REAL && v.d==0.0); }
static void arr_rebuild_2d_meta(GmlArr *A){
  if(!A || A->len<=GML_2D_STRIDE) return;
  free(A->row_len); A->row_len=NULL; A->row_cap=0; A->height2d=0; A->is_2d=0;
  for(int i=0;i<A->len;i++){
    if(!arr_value_nonzero(A->data[i])) continue;
    int row=i/GML_2D_STRIDE, col=i%GML_2D_STRIDE;
    if(row<=0) continue;
    if(!A->is_2d) A->is_2d=1;
    arr_row_ensure(A,row);
    if(A->row_len && col+1>A->row_len[row]) A->row_len[row]=col+1;
  }
  if(A->is_2d){
    int row0=0;
    int lim=A->len<GML_2D_STRIDE?A->len:GML_2D_STRIDE;
    for(int i=0;i<lim;i++) if(arr_value_nonzero(A->data[i])) row0=i+1;
    arr_row_ensure(A,0);
    if(A->row_len) A->row_len[0]=row0;
  }
}
int gml_val_array_length(GmlVal v){
  if(v.t!=V_ARR || !v.arr) return 0;
  GmlArr *A=(GmlArr*)v.arr;
  return A->is_2d ? A->height2d : A->len;
}
int gml_val_array_height_2d(GmlVal v){
  if(v.t!=V_ARR || !v.arr) return 0;
  GmlArr *A=(GmlArr*)v.arr;
  return A->is_2d ? A->height2d : A->len;
}
int gml_val_array_length_2d(GmlVal v, int row){
  if(v.t!=V_ARR || !v.arr || row<0) return 0;
  GmlArr *A=(GmlArr*)v.arr;
  if(A->is_2d)
    return (row<A->height2d && row<A->row_cap && A->row_len) ? A->row_len[row] : 0;
  if(row<A->len && A->data[row].t==V_ARR) return gml_val_array_length(A->data[row]);
  return row==0 ? A->len : 0;
}
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
/* ---- GMS2.3 array FUNCTIONS (array_create/get/set/push/pop/resize/copy/...). The bytecode's
 * arr[i] read/write uses pushaf/popaf; these are the explicit-function forms that GMS2.3 code
 * (hot GMS2.3 code calls array_set very frequently) also uses. Stored strings are owned COPIES so a
 * caller's GC-tracked temp being freed at scope exit can't dangle the array — the rule ds_ uses. */
GmlVal gml_arr_store_clone(GmlVal v){
  if(v.t==V_STR){ char *c=v.s?strdup(v.s):NULL; return c?vstr_owned(c):vstr(""); }
  if(v.t==V_ARR){ gml_arr_mark_escaped(v); return v; }
  if(v.t==V_UNDEF) return vundef();
  return vreal(v.t==V_REAL?v.d:0.0);
}
#define arr_store_clone gml_arr_store_clone
GmlVal gml_arr_new(int size, GmlVal fill){
  GmlArr *A=calloc(1,sizeof(GmlArr));
  if(size<0) size=0; if(size>16000000) size=16000000;
  if(A && size>0){ A->data=malloc((size_t)size*sizeof(GmlVal));
    if(A->data){ A->cap=size; A->len=size; for(int i=0;i<size;i++) A->data[i]=arr_store_clone(fill); } }
  if(A) A->escaped=1;
  GmlVal v; v.t=V_ARR; v.d=0; v.s=NULL; v.arr=A; return v;
}
void gml_arr_set(GmlVal arr, int idx, GmlVal val){
  if(arr.t!=V_ARR || !arr.arr || idx<0) return;
  GmlArr *A=arr.arr; arr_ensure(A,idx);
  if(idx<A->cap){ A->data[idx]=arr_store_clone(val); if(idx>=A->len) A->len=idx+1; }
}
GmlVal gml_arr_get(GmlVal arr, int idx){
  if(arr.t!=V_ARR || !arr.arr || idx<0) return vreal(0);
  GmlArr *A=arr.arr;
  if(idx<A->len){ GmlVal v=A->data[idx]; if(v.t==V_STR) v.d=0; return v; }   /* d=0: non-owning ref */
  return vreal(0);
}
void gml_arr_push(GmlVal arr, GmlVal val){
  if(arr.t!=V_ARR || !arr.arr) return;
  GmlArr *A=arr.arr; int idx=A->len; arr_ensure(A,idx);
  if(idx<A->cap){ A->data[idx]=arr_store_clone(val); A->len=idx+1; }
}
GmlVal gml_arr_pop(GmlVal arr){   /* remove+return last; transfer ownership of an owned string out */
  if(arr.t!=V_ARR || !arr.arr) return vreal(0);
  GmlArr *A=arr.arr; if(A->len<=0) return vreal(0);
  GmlVal v=A->data[--A->len]; A->data[A->len]=vreal(0); return v;
}
void gml_arr_resize(GmlVal arr, int size){
  if(arr.t!=V_ARR || !arr.arr || size<0 || size>16000000) return;
  GmlArr *A=arr.arr; if(size>0) arr_ensure(A,size-1); if(size<A->len){ for(int i=size;i<A->len;i++) A->data[i]=vreal(0); } A->len=size;
}
void gml_arr_copy(GmlVal dst, int di, GmlVal src, int si, int count){
  if(dst.t!=V_ARR || !dst.arr || src.t!=V_ARR || !src.arr || di<0 || si<0 || count<=0) return;
  GmlArr *D=dst.arr, *S=src.arr;
  for(int k=0;k<count;k++){ if(si+k>=S->len) break; arr_ensure(D,di+k); if(di+k<D->cap){ D->data[di+k]=arr_store_clone(S->data[si+k]); if(di+k>=D->len) D->len=di+k+1; } }
}
/* teardown dedupe: during full teardown (state load / vm free) every alias of a shared
 * array reaches val_free — free each GmlArr exactly once via an open-addressing pointer set. */
static void **g_freeset; static size_t g_freeset_cap, g_freeset_n; static int g_freeset_on, g_free_skip_escaped;
static int freeset_seen(void *p){
  if(!g_freeset_on) return 0;
  if(g_freeset_n*4 >= g_freeset_cap*3){
    size_t nc=g_freeset_cap? g_freeset_cap*2 : 256;
    void **ns=calloc(nc,sizeof(void*));
    if(!ns) return 0;
    for(size_t i=0;i<g_freeset_cap;i++) if(g_freeset[i]){
      size_t h=((uintptr_t)g_freeset[i]>>4)&(nc-1);
      while(ns[h]) h=(h+1)&(nc-1);
      ns[h]=g_freeset[i];
    }
    free(g_freeset); g_freeset=ns; g_freeset_cap=nc;
  }
  size_t h=((uintptr_t)p>>4)&(g_freeset_cap-1);
  while(g_freeset[h]){ if(g_freeset[h]==p) return 1; h=(h+1)&(g_freeset_cap-1); }
  g_freeset[h]=p; g_freeset_n++;
  return 0;
}
static void freeset_begin(void){ g_freeset_on=1; g_freeset_n=0;
  if(g_freeset) memset(g_freeset,0,g_freeset_cap*sizeof(void*)); }
static void freeset_end(void){ g_freeset_on=0; }
static void val_free(GmlVal v){
  if(v.t!=V_ARR || !v.arr) return;
  GmlArr *A=v.arr;
  if(freeset_seen(A)) return;
  /* Escaped arrays are shared reference values. Outside a full teardown sweep,
   * a local/container cleanup must not free a child another live slot still owns. */
  if(A->escaped && (g_free_skip_escaped || !g_freeset_on)) return;
  /* defend against a corrupt GmlArr (garbage len/cap/data from a bad savestate) — free only the
   * struct, never walk a garbage data pointer. */
  if(A->len<0 || A->cap<A->len || A->cap>16000000 || (A->len>0 && !A->data)){
    free(A->row_len); if((uintptr_t)A->data>0x1000 && A->cap>=0 && A->cap<=16000000) free(A->data); free(A); return;
  }
  /* Walk occupied row spans in the flat row*32000+col array layout. */
  if(A->is_2d && A->row_len){
    for(int r=0;r<A->height2d && r<A->row_cap;r++){
      long base=(long)r*GML_2D_STRIDE;
      for(int c=0;c<A->row_len[r] && base+c<A->len;c++) val_free(A->data[base+c]);
    }
  } else {
    for(int i=0;i<A->len;i++) val_free(A->data[i]);
  }
  free(A->row_len);
  free(A->data);
  free(A);
}
/* GmlArr pointers may be shared across scopes. Mark escaped arrays so local cleanup
 * skips them; full teardown handles shared allocations with deduplication. */
static void varmap_free_ex(GmlVarMap *m, int skip_escaped){
  int local_freeset=0, prev_skip=g_free_skip_escaped;
  if(skip_escaped){
    g_free_skip_escaped=1;
    if(!g_freeset_on){ freeset_begin(); local_freeset=1; }
  }
  for(int i=0;i<m->cap;i++) if(m->slots && m->slots[i].key && m->slots[i].val.t==V_ARR){
    val_free(m->slots[i].val); }
  if(skip_escaped){
    g_free_skip_escaped=prev_skip;
    if(local_freeset) freeset_end();
  }
  free(m->slots); m->slots=0; m->cap=m->len=0;
}
static void varmap_free(GmlVarMap *m){ varmap_free_ex(m,0); }
static void arr_mark_escaped_rec(GmlArr *A, int depth){
  if(!A || depth>64) return;
  if(A->escaped) return;
  A->escaped=1;
  if(A->is_2d && A->row_len){
    for(int r=0;r<A->height2d && r<A->row_cap;r++){
      long base=(long)r*GML_2D_STRIDE;
      for(int c=0;c<A->row_len[r] && base+c<A->len;c++){
        GmlVal e=A->data[base+c];
        if(e.t==V_ARR && e.arr) arr_mark_escaped_rec((GmlArr*)e.arr,depth+1);
      }
    }
  } else {
    for(int i=0;i<A->len;i++){
      GmlVal e=A->data[i];
      if(e.t==V_ARR && e.arr) arr_mark_escaped_rec((GmlArr*)e.arr,depth+1);
    }
  }
}
void gml_arr_mark_escaped(GmlVal v){ if(v.t==V_ARR && v.arr) arr_mark_escaped_rec((GmlArr*)v.arr,0); }

/* ---------------- helpers ---------------- */
static double asnum(GmlVal v){ return v.t==V_REAL? v.d : (v.s? atof(v.s):0); }
static int    astrue(GmlVal v){ return v.t==V_REAL? (v.d>=0.5) : (v.s&&v.s[0]); } /* GM: real>=0.5 true */
static const char *asstr_cmp(GmlVal v, char *buf, size_t n){
  if(v.t==V_STR) return v.s?v.s:"";
  if(v.t==V_UNDEF) return "";
  snprintf(buf,n,"%g",v.t==V_REAL?v.d:0.0);
  return buf;
}
static void motion_from_components(GmlInstance *in){
  in->speed=hypot(in->hspeed,in->vspeed);
  in->direction=atan2(-in->vspeed,in->hspeed)*180.0/M_PI;
}
static void motion_from_speed_dir(GmlInstance *in){
  in->hspeed=in->speed*cos(in->direction*M_PI/180.0);
  in->vspeed=-in->speed*sin(in->direction*M_PI/180.0);
}

/* ---------------- builtin instance variables ---------------- */
/* returns 1 if name is a builtin and handled */
static int inst_builtin_get(GmlInstance *in, const char *n, GmlVal *out){
  if(!strcmp(n,"image_single")){ *out=vreal(in->image_speed==0? in->image_index : -1); return 1; }
  #define B(name,field) if(!strcmp(n,name)){ *out=vreal(in->field); return 1; }
  B("x",x) B("y",y) B("xprevious",xprevious) B("yprevious",yprevious)
  B("xstart",xstart) B("ystart",ystart)
  B("sprite_index",sprite_index) B("mask_index",mask_index) B("image_index",image_index) B("image_speed",image_speed)
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
  { static const char *w=NULL; static int w_init=0;
    if(!w_init){ w=getenv("GML_DBG_VARWRITE"); w_init=1; }   /* GML_DBG_VARWRITE=<var>: log builtin writes */
    if(w && !strcmp(n,w)){ extern long g_vm_frame;
      GmlInstance *cs=g_cur_vm?g_cur_vm->cur_self:NULL, *co=g_cur_vm?g_cur_vm->cur_other:NULL;
      fprintf(stderr,"[varwrite] f%ld id=%u obj=%d self=%u other=%u code=%s %s=%g\n",
        g_vm_frame,in->id,in->obj,cs?cs->id:0,co?co->id:0,
        g_cur_code_name?g_cur_code_name:"?",n,d); } }
  #define B(name,field) if(!strcmp(n,name)){ in->field=d; return 1; }
  #define BT(name,field) if(!strcmp(n,name)){ in->field=d; gml_colgrid_touch(in); return 1; }  /* bbox input */
  /* image_single >= 0 selects a subimage and stops animation; -1 resumes animation. */
  if(!strcmp(n,"image_single")){
    if(d>=0){ in->image_index=d; in->image_speed=0; }
    else in->image_speed=1;
    return 1; }
  BT("x",x) BT("y",y) B("xprevious",xprevious) B("yprevious",yprevious)
  B("xstart",xstart) B("ystart",ystart)
  BT("sprite_index",sprite_index) BT("mask_index",mask_index) B("image_index",image_index) B("image_speed",image_speed)
  BT("image_xscale",image_xscale) BT("image_yscale",image_yscale) BT("image_angle",image_angle)
  B("image_alpha",image_alpha) B("image_blend",image_blend)
  #undef BT
  B("depth",depth) B("visible",visible) B("solid",solid) B("persistent",persistent)
  B("gravity",gravity) B("gravity_direction",gravity_direction) B("friction",friction)
  B("path_position",path_position) B("path_speed",path_speed)
  B("path_orientation",path_orientation) B("path_scale",path_scale)
  B("path_positionprevious",path_positionprevious) B("path_endaction",path_endaction)
  #undef B
  if(!strcmp(n,"path_index")){ in->path_index=d; return 1; }   /* set directly = follow that path */
  /* speed/direction/hspeed/vspeed are linked in GM */
  if(!strcmp(n,"hspeed")){ in->hspeed=d; motion_from_components(in); return 1; }
  if(!strcmp(n,"vspeed")){ in->vspeed=d; motion_from_components(in); return 1; }
  if(!strcmp(n,"direction")){ in->direction=d; motion_from_speed_dir(in); return 1; }
  if(!strcmp(n,"speed")){ in->speed=d; motion_from_speed_dir(in); return 1; }
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
/* GM built-in global variables: these names live in global scope even when a
 * bytecode reference uses the current-instance scope. */
static int is_global_builtin(const char *n){
  return !strcmp(n,"health")||!strcmp(n,"lives")||!strcmp(n,"score")||!strcmp(n,"async_load"); }
static int vm_bbox(GmlVM *vm, GmlInstance *in, double *l, double *t, double *r, double *b);
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
  if(idx>=0){ *out=(idx<vm->script_argc)? vm->script_args[idx] : vreal(0); return 1; }
  return 0;
}
static int argument_set(GmlVM *vm, const char *name, GmlVal v){
  int idx=argument_index(name);
  if(idx<0) return 0;
  vm->script_args[idx]=v;
  if(idx>=vm->script_argc) vm->script_argc=idx+1;
  return 1;
}
static double room_speed_value(GmlVM *vm){
  GmlVal *p=gml_varmap_get(&vm->globals,"room_speed");
  double v=p?asnum(*p):30.0;
  return v>0 ? v : 30.0;
}
static int inst_sprite_metric_get(GmlVM *vm, GmlInstance *in, const char *name, GmlVal *out){
  if(!in || !vm || !vm->render) return 0;
  GmlRender *R=(GmlRender*)vm->render;
  int si=(int)in->sprite_index;
  if(si<0 || si>=R->n_spr) return 0;
  GmlSprite *s=&R->spr[si];
  if(!strcmp(name,"sprite_width")){ *out=vreal(s->w); return 1; }
  if(!strcmp(name,"sprite_height")){ *out=vreal(s->h); return 1; }
  if(!strcmp(name,"sprite_xoffset")){ *out=vreal(s->originx); return 1; }
  if(!strcmp(name,"sprite_yoffset")){ *out=vreal(s->originy); return 1; }
  return 0;
}
static GmlVal var_get(GmlVM *vm, int inst, const char *name){
  GmlVal out;
  if(!strcmp(name,"room")) return vreal(vm->room_index);   /* GM built-in: current room index */
  if(!strcmp(name,"keyboard_lastkey")) return vreal(vm->last_key); /* GM: last key pressed */
  if(!strcmp(name,"room_speed")) return vreal(room_speed_value(vm));
  if(!strcmp(name,"working_directory")||!strcmp(name,"program_directory")){
    /* Return the content directory with a trailing slash as a string. */
    static char wd[560]; snprintf(wd,sizeof wd,"%s/",vm->win->content_dir);
    return vstr(wd); }
  if(!strcmp(name,"fps")) return vreal(room_speed_value(vm));
  if(!strcmp(name,"view_current")){ GmlVal *p=gml_varmap_get(&vm->globals,name); return p?*p:vreal(0); }
  if(!strcmp(name,"room_persistent")){ GmlVal *p=gml_varmap_get(&vm->globals,name); return p?*p:vreal(0); }
  if(!strcmp(name,"mouse_x")||!strcmp(name,"mouse_y")) return vreal(0);
  if(!strcmp(name,"current_time")){ extern long g_vm_frame;
    return vreal((double)g_vm_frame * (1000.0 / room_speed_value(vm))); }
  if(argument_get(vm,name,&out)) return out;
  if(!strcmp(name,"room_width")||!strcmp(name,"room_height")){   /* GM built-in: current room size */
    GmlRoom r; if(gml_room_get(vm->win,vm->room_index,&r)==0)
      return vreal(name[5]=='w'? (double)r.width : (double)r.height);
    return vreal(0); }
  if(!strcmp(name,"mouse_x")||!strcmp(name,"mouse_y")){   /* GM built-in: mouse in room coords */
    extern void gml_input_mouse(double*,double*,double*,double*,double*,double*,int*,int*,int*,int*);
    double mx,my; gml_input_mouse(&mx,&my,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL);
    return vreal(name[6]=='x'? mx : my); }
  if(!strcmp(name,"instance_count")){   /* Count active, unmarked instances, matching instance_find enumeration. */
    int c=0; for(int i=0;i<vm->inst_count;i++) if(vm->inst[i].active && !vm->inst[i].marked) c++;
    return vreal(c); }
  if(inst==IT_GLOBAL || is_global_builtin(name)){
    GmlVal *p=gml_varmap_get(&vm->globals,name); return p?*p:vreal(0); }
  if((inst==IT_OTHER && !vm->cur_other) || (inst==IT_SELF && !vm->cur_self)){
    if(!strcmp(name,"id") || !strcmp(name,"object_index")) return vreal(IT_NOONE);
  }
  GmlInstance *self = var_target(vm,inst);
  if(self){
    /* image_number = frame count of the current sprite (needs the renderer) */
    if(!strcmp(name,"image_number")){ GmlRender *R=(GmlRender*)vm->render;
      return vreal(R? gml_sprite_frames(R,(int)self->sprite_index):0); }
    if(inst_sprite_metric_get(vm,self,name,&out)) return out;
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
  gml_arr_mark_escaped(v);   /* target is a global/instance slot: outlives the current scope */
  { const char *dv=getenv("GML_DBG_VARSET");
    if(dv && name && !strcmp(name,dv)){ extern long g_vm_frame;
      fprintf(stderr,"[varset] f%ld inst=%d %s = %s%.2f\n",g_vm_frame,inst,name,
        v.t==V_STR?"str:":"",v.t==V_REAL?v.d:0.0); } }
  if(!strcmp(name,"room")){ vm->pending_room=(int)asnum(v); return; }  /* GM: room=X -> goto room */
  if(argument_set(vm,name,v)) return;
  if(!strcmp(name,"room_speed")||!strcmp(name,"view_current")||!strcmp(name,"room_persistent")){
    *gml_varmap_put(&vm->globals,name)=v;
    return;
  }
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
  if(!strcmp(nm,"argument")){
    if(idx>=0 && idx<16){
      vm->script_args[idx]=v;
      if(idx>=vm->script_argc) vm->script_argc=idx+1;
    }
    return;
  }
  if(!strcmp(nm,"alarm")){ GmlInstance *s=resolve_inst(vm,inst_t);
    if(s && idx>=0 && idx<GML_ALARMS) s->alarm[idx]=v.t==V_REAL?v.d:(v.s?atof(v.s):0); return; }
  GmlVarMap *m=is_room_global_array(nm)? &vm->globals : scope_map(vm,locals,inst_t); if(!m) return;
  GmlVal *slot=gml_varmap_put(m,nm); GmlArr *A=arr_of(slot);
  if(m!=locals || A->escaped) gml_arr_mark_escaped(v);   /* element outlives scope if its owner already does */
  arr_note_2d_set(A,idx); arr_ensure(A,idx);
  if(idx>=0 && idx<A->cap) A->data[idx]=v;
}
static GmlVal array_get(GmlVM *vm, GmlVarMap *locals, int inst_t, const char *nm, int idx){
  if(!strcmp(nm,"argument")) return (idx>=0 && idx<vm->script_argc && idx<16) ? vm->script_args[idx] : vreal(0);
  { int aidx=argument_index(nm);   /* `argumentN[idx]`: index INTO an array-valued argument (distinct from
       `argument[idx]`, the Nth arg). Missing this, serialize's `with(actions[i])` over an array passed as
       argument0 read 0 for every element, so every input binding serialised to "" and lost its default key. */
    if(aidx>=0){ GmlVal av=(aidx<vm->script_argc)? vm->script_args[aidx] : vreal(0);
      if(av.t==V_ARR && av.arr){ GmlArr *A=av.arr; if(idx>=0 && idx<A->len) return A->data[idx]; }
      return vreal(0); } }
  if(!strcmp(nm,"alarm")){ GmlInstance *s=resolve_inst(vm,inst_t);
    return vreal((s&&idx>=0&&idx<GML_ALARMS)? s->alarm[idx] : -1); }
  GmlVarMap *m=is_room_global_array(nm)? &vm->globals : scope_map(vm,locals,inst_t); if(!m) return vreal(0);
  GmlVal *slot=gml_varmap_get(m,nm);
  if(!slot||slot->t!=V_ARR) return vreal(0);
  GmlArr *A=slot->arr;
  /* defend against a corrupt/garbage GmlArr (e.g. a cross-version savestate) — never deref blindly */
  if(!A || !A->data || A->len<0 || A->cap<A->len || A->cap>16000000) return vreal(0);
  return (idx>=0 && idx<A->len)? A->data[idx] : vreal(0);
}
static GmlVal array_get_inst_field(GmlVM *vm, GmlInstance *s, const char *nm, int idx){
  (void)vm;
  if(!s) return vreal(0);
  if(s->obj>=0 && !strcmp(nm,"alarm"))
    return vreal((idx>=0 && idx<GML_ALARMS)? s->alarm[idx] : -1);
  GmlVal *slot=gml_varmap_get(&s->vars,nm);
  if(!slot || slot->t!=V_ARR || !slot->arr) return vreal(0);
  GmlArr *A=slot->arr;
  if(!A || !A->data || A->len<0 || A->cap<A->len || A->cap>16000000) return vreal(0);
  return (idx>=0 && idx<A->len)? A->data[idx] : vreal(0);
}
static void array_set_inst_field(GmlInstance *s, const char *nm, int idx, GmlVal v){
  if(!s) return;
  if(s->obj>=0 && !strcmp(nm,"alarm")){
    if(idx>=0 && idx<GML_ALARMS) s->alarm[idx]=v.t==V_REAL?v.d:(v.s?atof(v.s):0);
    return;
  }
  gml_arr_mark_escaped(v);
  GmlVal *slot=gml_varmap_put(&s->vars,nm);
  GmlArr *A=arr_of(slot);
  arr_note_2d_set(A,idx);
  arr_ensure(A,idx);
  if(idx>=0 && idx<A->cap) A->data[idx]=v;
}

/* read/write any var on a specific instance (builtin or custom) */
static GmlVal inst_get_any(GmlVM *vm, GmlInstance *t, const char *nm){
  GmlVal o; if(inst_builtin_get(t,nm,&o)) return o;
  /* Same sprite-derived builtins var_get resolves for `self.X`, so a REFERENCED instance
   * (`other.image_number`, `foo.bbox_left`) reads them too — not 0. `expr.image_number` returning
   * 0 made an animation-gated cutscene wait forever on `floor(image_index)==image_number-1` (= -1). */
  if(!strcmp(nm,"image_number")){ GmlRender *R=(GmlRender*)vm->render;
    return vreal(R? gml_sprite_frames(R,(int)t->sprite_index):0); }
  if(inst_sprite_metric_get(vm,t,nm,&o)) return o;
  if(!strncmp(nm,"bbox_",5)){ double l,tp,r,b;
    if(vm_bbox(vm,t,&l,&tp,&r,&b)){
      if(!strcmp(nm,"bbox_left"))   return vreal(l);
      if(!strcmp(nm,"bbox_right"))  return vreal(r);
      if(!strcmp(nm,"bbox_top"))    return vreal(tp);
      if(!strcmp(nm,"bbox_bottom")) return vreal(b); }
    return vreal(0); }
  GmlVal *p=gml_varmap_get(&t->vars,nm); return p?*p:vreal(0);
}
static void inst_set_any(GmlInstance *t, const char *nm, GmlVal v){
  gml_arr_mark_escaped(v);   /* instance vars outlive the current scope */
  if(inst_builtin_set(t,nm,v)) return; *gml_varmap_put(&t->vars,nm)=v;
}
static GmlInstance *inst_by_id(GmlVM *vm, double idv){
  int id=(int)idv;
  for(int i=0;i<vm->inst_count;i++) if(vm->inst[i].active && !vm->inst[i].marked && (int)vm->inst[i].id==id) return &vm->inst[i];
  for(int i=0;i<vm->inst_count;i++) if(vm->inst[i].active && !vm->inst[i].marked && vm->inst[i].obj==id) return &vm->inst[i];
  return NULL;
}
/* resolve a StackTop instance reference value: real id / object index / self / other. */
/* GMS2.3 struct pool. A struct is a standalone GmlInstance (varmap of fields, obj=-1) kept OUT of the
 * room instance array so it is never stepped/drawn/counted; field access reaches it by id. */
/* A struct id encodes its slot in the low 20 bits and a 7-bit generation above it, all inside the
 * 0x50000000 struct-id band. gml_struct_find is O(1) (direct slot index) and the generation lets a
 * GC free + reuse slots safely: a dangling id to a recycled slot fails the generation check and reads
 * NULL instead of a different struct. */
#define GML_STRUCT_SLOT_BITS 20
#define GML_STRUCT_SLOT_MAX  (1<<GML_STRUCT_SLOT_BITS)
#define GML_STRUCT_SLOT_MASK (GML_STRUCT_SLOT_MAX-1)
static int struct_ensure_cap(GmlVM *vm, int need){
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
    if(vm->n_structs >= vm->cap_structs && !struct_ensure_cap(vm,vm->n_structs+1)) return NULL;
    slot = vm->n_structs++;
  }
  vm->struct_gen[slot] = (unsigned char)((vm->struct_gen[slot]+1) & 0x7F);   /* bump generation on (re)use */
  GmlInstance *st = calloc(1,sizeof(GmlInstance));
  if(!st) return NULL;
  st->id = GML_STRUCT_ID_BASE + ((unsigned)vm->struct_gen[slot] << GML_STRUCT_SLOT_BITS) + (unsigned)slot;
  st->obj = -1; st->active = 1;
  vm->structs[slot] = st;
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
static void struct_free_push(GmlVM *vm, int slot){
  if(!vm || slot<0) return;
  if(vm->n_struct_free>=vm->cap_struct_free){
    int nc=vm->cap_struct_free?vm->cap_struct_free*2:256;
    int *nf=realloc(vm->struct_free,(size_t)nc*sizeof(int));
    if(nf){ vm->struct_free=nf; vm->cap_struct_free=nc; }
  }
  if(vm->n_struct_free<vm->cap_struct_free) vm->struct_free[vm->n_struct_free++]=slot;
}
/* ---- struct garbage collection (mark-sweep from every GmlVal root, run between frames) ---- */
static void gc_mark_struct(GmlVM *vm, unsigned id, GmlInstance ***wl, int *wn, int *wcap){
  GmlInstance *s = gml_struct_find(vm,id);
  if(s && !s->marked){ s->marked=1;
    if(*wn >= *wcap){ *wcap = *wcap? *wcap*2 : 256; *wl = realloc(*wl,(size_t)*wcap*sizeof(GmlInstance*)); }
    if(*wl) (*wl)[(*wn)++] = s;
  }
}
static void gc_scan_arr(GmlVM *vm, GmlArr *A, GmlInstance ***wl, int *wn, int *wcap, int depth){
  if(!A || depth>64) return;
  for(int i=0;i<A->len;i++){ GmlVal v=A->data[i];
    if(v.t==V_REAL && GML_IS_STRUCT_ID(v.d)) gc_mark_struct(vm,(unsigned)v.d,wl,wn,wcap);
    else if(v.t==V_ARR && v.arr) gc_scan_arr(vm,(GmlArr*)v.arr,wl,wn,wcap,depth+1); }
}
static void gc_scan_val(GmlVM *vm, GmlVal v, GmlInstance ***wl, int *wn, int *wcap){
  if(v.t==V_REAL && GML_IS_STRUCT_ID(v.d)) gc_mark_struct(vm,(unsigned)v.d,wl,wn,wcap);
  else if(v.t==V_ARR && v.arr) gc_scan_arr(vm,(GmlArr*)v.arr,wl,wn,wcap,0);
}
static void gc_scan_vm(GmlVM *vm, GmlVarMap *m, GmlInstance ***wl, int *wn, int *wcap){
  if(!m || !m->slots) return;
  for(int i=0;i<m->cap;i++) if(m->slots[i].key) gc_scan_val(vm,m->slots[i].val,wl,wn,wcap);
}
void gml_struct_gc(GmlVM *vm){
  if(vm->n_structs<=0) return;
  for(int i=0;i<vm->n_structs;i++) if(vm->structs[i]) vm->structs[i]->marked=0;
  GmlInstance **wl=NULL; int wn=0, wcap=0;
  /* roots: globals, every instance (active AND deactivated — the latter can be reactivated),
   * the argument register, and all GmlVal-bearing ds containers (ds_map key+val, ds_list, ds_grid). */
  gc_scan_vm(vm,&vm->globals,&wl,&wn,&wcap);
  for(int i=0;i<vm->inst_count;i++) gc_scan_vm(vm,&vm->inst[i].vars,&wl,&wn,&wcap);
  for(int i=0;i<16;i++) gc_scan_val(vm,vm->script_args[i],&wl,&wn,&wcap);
  for(int i=0;i<GML_DS_MAP_MAX;i++) if(vm->ds_map[i].live){ GmlDSMap *m=&vm->ds_map[i];
    for(int j=0;j<m->len;j++){ gc_scan_val(vm,m->entry[j].key_val,&wl,&wn,&wcap); gc_scan_val(vm,m->entry[j].val,&wl,&wn,&wcap); } }
  for(int i=0;i<GML_DS_LIST_MAX;i++) if(vm->ds_list[i].live){ GmlDSList *l=&vm->ds_list[i];
    for(int j=0;j<l->len;j++) gc_scan_val(vm,l->item[j],&wl,&wn,&wcap); }
  for(int i=0;i<GML_DS_GRID_MAX;i++) if(vm->ds_grid[i].live){ GmlDSGrid *g=&vm->ds_grid[i];
    long cells=(long)g->w*g->h; for(long j=0;j<cells;j++) gc_scan_val(vm,g->cell[j],&wl,&wn,&wcap); }
  /* transitive: a live struct's own fields keep other structs/arrays alive */
  while(wn>0){ GmlInstance *s=wl[--wn]; gc_scan_vm(vm,&s->vars,&wl,&wn,&wcap); }
  /* sweep: free the unmarked, recycle their slots. varmap_free_ex(,1) skips escaped (aliased) arrays,
   * exactly like instance teardown, so a shared GmlArr is never double-freed. */
  for(int i=0;i<vm->n_structs;i++){ GmlInstance *s=vm->structs[i];
    if(s && !s->marked){
      varmap_free_ex(&s->vars,1); free(s); vm->structs[i]=NULL;
      struct_free_push(vm,i);
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
/* public accessor: read a builtin or custom instance variable by name → real value.
 * Returns 0 for absent variables (GM default). */
double gml_inst_var_get(GmlVM *vm, GmlInstance *in, const char *nm){
  if(!in) return 0; GmlVal o; if(inst_builtin_get(in,nm,&o)) return o.t==V_REAL?o.d:0;
  if(inst_sprite_metric_get(vm,in,nm,&o)) return o.t==V_REAL?o.d:0;
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
/* Function-value encoding: a GMS2.3 script/method reference pushed on the value stack (via a
 * `push.i32 <FUNC-ref>` or method()) is represented as a plain real tagged with GML_FUNCVAL_TAG in
 * its high bits and the CODE-entry index in the low 24. OP_CALLV recovers the index and runs it.
 * The tag (0x40000000, ~1.07e9) is far above any real script/asset id or gameplay number, and code
 * indices are < ~1000, so the encoding is unambiguous. */
static int g_run_depth=0;   /* C-recursion guard: gml_vm_run_code re-enters via OP_CALL/OP_CALLV */
static const char *g_recur_chain[300]; static int g_recur_n;   /* names along the active call chain (debug) */

GmlVal gml_vm_run_code(GmlVM *vm, int ci, GmlInstance *self, GmlInstance *other,
                       GmlVal *args, int n_args){
  if(ci<0||ci>=vm->win->n_code) return vreal(0);
  if(g_run_depth>=300){   /* each frame ≈18KB (stk[512]+locals) → 300 levels ≈5.4MB, safely under an
                           * 8MB C-stack, yet high enough not to clip any realistic GM call chain. */
    static int warned=0;
    if(warned<4){ warned++; fprintf(stderr,"[gml] VM recursion too deep in '%s' — aborting run\n", vm->win->code[ci].name); }
    if(getenv("GML_DBG_RECUR")){ static int dumped=0;
      if(!dumped){ dumped=1;
        fprintf(stderr,"[recur] call chain (innermost last):\n");
        for(int i=0;i<g_recur_n && i<300;i++) fprintf(stderr,"  %d: %s\n", i, g_recur_chain[i]?g_recur_chain[i]:"?");
      } }
    return vreal(0);
  }
  if(g_run_depth<300) g_recur_chain[g_run_depth]=vm->win->code[ci].name;
  g_recur_n=g_run_depth+1;
  g_run_depth++;
  GmlWin *w=vm->win; const uint8_t *d=w->data;
  uint32_t start=w->code[ci].start, end=start+w->code[ci].length;
  GmlInstance *save_self=vm->cur_self, *save_other=vm->cur_other;
  const char *save_code_name=g_cur_code_name;
  GmlVM *save_cur_vm=g_cur_vm;
  GmlVal save_args[16]; int save_argc=vm->script_argc;
  for(int i=0;i<16;i++) save_args[i]=vm->script_args[i];
  vm->cur_self=self; vm->cur_other=other;
  g_cur_code_name=vm->win->code[ci].name;
  g_cur_vm=vm;
  GmlVarMap locals={0};
  int argc=n_args<0?0:(n_args<16?n_args:16);
  vm->script_argc=argc;
  for(int i=0;i<16;i++) vm->script_args[i]=(i<argc && args)? args[i] : vreal(0);
  /* Array arguments may alias storage in caller and callee scopes. Mark them escaped
   * so local cleanup leaves shared arrays for deduplicated full teardown. */
  for(int i=0;i<argc;i++) if(vm->script_args[i].t==V_ARR) gml_arr_mark_escaped(vm->script_args[i]);

  GmlVal stk[STK]; int sp=0;
  /* Preserve the array/index reference at savearef for a later popaf store,
   * restoring the stack position below the saved reference. */
  struct { GmlArr *arr; int idx; int base; } aref[16]; int aref_n=0;
  uint32_t pc=start;
  GmlVal ret=vreal(0);
  /* with-statement (pushenv/popenv) loop frames */
  struct { GmlInstance **list; int n, idx; GmlInstance *ss, *so; } withstk[32]; int withsp=0;
  /* string GC: track malloc'd strings so they can be freed at scope exit */
  void **str_gc=NULL; int str_gc_n=0, str_gc_cap=0;
  #define GC_TRACK(ptr) do{ if(str_gc_n>=str_gc_cap){ str_gc_cap=str_gc_cap?str_gc_cap*2:16; str_gc=realloc(str_gc,str_gc_cap*sizeof(void*)); } str_gc[str_gc_n++]=ptr; }while(0)
  /* an owned V_STR (v.d!=0, set by vstr_owned) is a fresh malloc'd temporary the VM must free.
   * Literals/references (data.win STRG, rodata, var pointers via plain vstr) are d==0 and never freed. */
  #define STR_IS_HEAP(vv) ((vv).t==V_STR && (vv).s && (vv).d!=0)
  #define GC_UNTRACK(ss) do{ const void *_p=(const void*)(ss); for(int _i=0;_i<str_gc_n;_i++) if(str_gc[_i]==_p) str_gc[_i]=NULL; }while(0)
  /* For persistent string stores, transfer strings owned by this run, copy strings
   * owned by another scope, and retain stable references with d == 0. */
  #define GC_PERSIST(vv) do{ if((vv).t==V_STR && (vv).s && (vv).d!=0){ int _f=0; const void *_p=(const void*)(vv).s; \
      for(int _i=0;_i<str_gc_n;_i++) if(str_gc[_i]==_p){ str_gc[_i]=NULL; _f=1; } \
      if(!_f){ char *_c=strdup((vv).s); if(_c) (vv)=vstr_owned(_c); } } }while(0)
  int trace = getenv("GML_TRACE") && strstr(w->code[ci].name, getenv("GML_TRACE"));
  /* Watchdog: a single code run should never execute more than a few million instructions. If one
   * blows past a large budget it is a runaway loop (e.g. a control-flow condition corrupted by an
   * unimplemented opcode) — abort the run instead of freezing the whole frontend. Real per-event
   * code, even heavy tile/particle loops, stays orders of magnitude under this. */
  uint64_t watchdog=0;
  const uint64_t WATCHDOG_MAX=64000000ull;
  while(pc<end){
    if(++watchdog>WATCHDOG_MAX){
      static int warned=0;
      if(warned<4){ warned++; extern long g_vm_frame;
        fprintf(stderr,"[gml] f%ld VM watchdog tripped in '%s' at offset %u (runaway loop) — aborting run\n",
          g_vm_frame, w->code[ci].name, pc-start); }
      break;
    }
    GmlInsn in; int sz=gml_decode_bc(d,pc,w->bytecode,&in); if(!sz) break;
    uint32_t nextpc=pc+sz;
    if(trace){
      const char *rn = (in.kind==OP_CALL || in.kind==OP_PUSH || in.kind==OP_POP) ? gml_ref_name(w,in.refaddr) : "";
      fprintf(stderr,"  %4u: %-7s t1=%x rt=%02x inst=%d  sp=%d %s\n",pc-start,gml_op_mnemonic(in.kind),in.type1,in.reftype,in.inst,sp,rn); }
    switch(in.kind){
      case OP_PUSH:{
        GmlVal v;
        if(in.type1==DT_INT16) v=vreal(in.sval);
        else if(in.type1==DT_DOUBLE) v=vreal(in.dval);
        else if(in.type1==DT_INT32){ v=vreal(in.ival);
          /* GMS2.3 function-value: a `push.i32` whose reference word (pc+4) resolves via the FUNC
           * occurrence chain to a script code-entry is pushing that function as a value (later called
           * by OP_CALLV or bound by method()). Tag it so OP_CALLV can dispatch the code entry. */
          if(w->bytecode>=17){ const char *fn=gml_ref_name(w,pc+4);
            if(fn && fn[0]!='?' && !strncmp(fn,"gml_",4)){ int fci=gml_code_index_by_name(w,fn);
              if(fci>=0){ v=vreal((double)(GML_FUNCVAL_TAG|fci));
                if(getenv("GML_DBG_FUNCVAL")) fprintf(stderr,"[funcval] %s pc=%u i32=%d -> %s (ci=%d)\n",
                  w->code[ci].name,pc-start,in.ival,fn,fci); } } } }
        else if(in.type1==DT_INT64) v=vreal((double)in.lval);
        else if(in.type1==DT_STRING) v=vstr(gml_str_by_index(w,in.strindex));
        else if(in.type1==DT_VAR){
          const char *nm=gml_ref_name(w,in.refaddr);
          if(in.reftype==0x00){ /* Array */
            int idx=(int)(sp>0?asnum(stk[--sp]):0);
            GmlVal itv=sp>0?stk[--sp]:vreal(0);
            if(w->bytecode>=17 && sp>0 && itv.t==V_REAL && itv.d==-9.0){
              GmlVal iv=stk[--sp];
              GmlInstance *t=vm_inst_from_ref(vm,iv);
              v=t?array_get_inst_field(vm,t,nm,idx):vreal(0);
            } else {
              int it=(int)asnum(itv);
              v=array_get(vm,&locals,it,nm,idx);
            }
          } else if(in.reftype==0x10 || in.reftype==0x90){
            /* GMS2.3 array-following push (first dimension from a named variable). Stack top->down:
             * index, instance-type. ArrayPushAF(0x10)=read; ArrayPopAF(0x90)=write chain (must yield a
             * live sub-array reference so a following popaf stores into it). */
            int idx=(int)(sp>0?asnum(stk[--sp]):0); int it=(int)(sp>0?asnum(stk[--sp]):IT_SELF);
            GmlVarMap *m=is_room_global_array(nm)? &vm->globals : scope_map(vm,&locals,it);
            v=vreal(0);
            if(m && idx>=0){
              if(in.reftype==0x90){ GmlVal *slot=gml_varmap_put(m,nm); GmlArr *A=arr_of(slot); arr_ensure(A,idx);
                if(idx<A->cap){ if(A->data[idx].t!=V_ARR){ A->data[idx].t=V_ARR; A->data[idx].arr=calloc(1,sizeof(GmlArr)); }
                  v=A->data[idx]; } }
              else { GmlVal *slot=gml_varmap_get(m,nm);
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
            if(w->bytecode>=17 && sp>0 && iv.t==V_REAL && iv.d==-9.0) iv=stk[--sp];
            GmlInstance *t=vm_inst_from_ref(vm,iv); v=t?inst_get_any(vm,t,nm):vreal(0);
          } else if(in.inst==IT_STACK){
            /* GMS2.3 direct StackTop read `push.v stack.var`: the instance is the top of the value
             * stack (no separate -9 marker; the -9 is the instruction's own instance-type). Used for
             * `expr.field` where expr is a temporary — notably struct method dispatch `b.method(...)`,
             * where mis-routing this to var_get(-9) read nobody and every struct method call got 0. */
            GmlVal iv=sp>0?stk[--sp]:vreal(0);
            GmlInstance *t=vm_inst_from_ref(vm,iv); v=t?inst_get_any(vm,t,nm):vreal(0);
          } else if(in.inst==IT_LOCAL){
            if(argument_get(vm,nm,&v)){}
            else { GmlVal *pp=gml_varmap_get(&locals,nm); v=pp?*pp:vreal(0); }
          } else v=var_get(vm,in.inst,nm);
        } else v=vreal(0);
        if(sp<STK) stk[sp++]=v;
        break;
      }
      case OP_POP:{
        const char *nm=gml_ref_name(w,in.refaddr);
        if(in.reftype==0x00){ /* Direct array stores consume value/scope/index; numeric compound stores
           * consume scope/index/value. Select the order from Type1. */
          int idx; GmlVal itv, val, iv=vreal(0); GmlInstance *t=NULL;
          if(in.type1==DT_VAR){
            idx=(int)(sp>0?asnum(stk[--sp]):0);
            itv=sp>0?stk[--sp]:vreal(0);
            if(w->bytecode>=17 && sp>0 && itv.t==V_REAL && itv.d==-9.0){
              iv=stk[--sp];
              t=vm_inst_from_ref(vm,iv);
            }
            val=sp>0?stk[--sp]:vreal(0);
          } else {
            val=sp>0?stk[--sp]:vreal(0);
            idx=(int)(sp>0?asnum(stk[--sp]):0);
            itv=sp>0?stk[--sp]:vreal(0);
            if(w->bytecode>=17 && sp>0 && itv.t==V_REAL && itv.d==-9.0){
              iv=stk[--sp];
              t=vm_inst_from_ref(vm,iv);
            }
          }
          GC_PERSIST(val);
          if(t) array_set_inst_field(t,nm,idx,val);
          else array_set(vm,&locals,(int)asnum(itv),nm,idx,val);
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
             if(w->bytecode>=17 && sp>0 && stk[sp-1].t==V_REAL && stk[sp-1].d==-9.0) --sp;
             iv=sp>0?stk[--sp]:vreal(0); val=sp>0?stk[--sp]:vreal(0);
           } else {                                     /* [instance, (-9), value] — value on top */
             val=sp>0?stk[--sp]:vreal(0);
             if(w->bytecode>=17 && sp>0 && stk[sp-1].t==V_REAL && stk[sp-1].d==-9.0) --sp;
             iv=sp>0?stk[--sp]:vreal(0);
           }
           GC_PERSIST(val);
           GmlInstance *t=vm_inst_from_ref(vm,iv); if(t) inst_set_any(t,nm,val);
        } else {
          GmlVal v = sp>0? stk[--sp] : vreal(0);
          /* locals/arguments die at scope exit, so their owned strings stay tracked and are freed
           * then (str_gc). Only instance/global stores persist beyond the run — untrack those so the
           * var owns the string (else it dangles at scope exit; re-assigning it later leaks it). */
          if(in.inst==IT_LOCAL){ if(!argument_set(vm,nm,v)) *gml_varmap_put(&locals,nm)=v; }
          else if(in.inst==IT_STACK){   /* `pop.v.v stack.var` — write field on the instance under the value */
            GmlVal iv=sp>0?stk[--sp]:vreal(0); GC_PERSIST(v);
            GmlInstance *t=vm_inst_from_ref(vm,iv); if(t) inst_set_any(t,nm,v); }
          else { GC_PERSIST(v); var_set(vm,in.inst,nm,v); }
        }
        break;
      }
      case OP_POPZ: if(sp>0) sp--; break;
      case OP_DUP: {
        /* Use the low-word size parameter to duplicate the top extra+1 stack slots. */
        /* GMS2.3 DUP-SWAP (method call `obj.method(args)`): non-zero HIGH byte in the operand (0x88xx),
         * type Variable. It lifts the instance pushed before the args back to the top so
         * `push.v stack.method` reads obj.method; the instance then sits under the method value for the
         * call self. Swap the top two value slots. */
        if((in.inst & 0xFF00) && in.type1==DT_VAR){
          if(sp>=2){ GmlVal t=stk[sp-1]; stk[sp-1]=stk[sp-2]; stk[sp-2]=t; }
          break; }
        int ncopy = in.inst>0 ? in.inst+1 : 1;
        if(ncopy>8) ncopy=1;
        /* GMS2.3 reference dup for the compound `inst.var op= v` (`push inst; push.e -9; dup;
         * read; op; write`): the operand encodes in.inst=4 -> the formula's 5, which overshoots sp
         * so the dup silently no-op'd and the ref was never duplicated -> the write landed on the
         * bare -9 marker = instance NULL and the store was lost (`inst.x += 3` never moved). A
         * StackTop reference is EXACTLY 2 slots [instance, -9]; detect it by the -9 marker on top
         * (only for reference-dup encodings, in.inst>=2, so value dups and array `arr[i]+=v` — which
         * carries a numeric index on top and is handled via savearef — are left untouched). */
        if(w->bytecode>=17 && in.inst>=2 && sp>=2 && stk[sp-1].t==V_REAL && stk[sp-1].d==-9.0) ncopy=2;
        if(sp>=ncopy && sp+ncopy<=STK){ for(int k=0;k<ncopy;k++) stk[sp+k]=stk[sp-ncopy+k]; sp+=ncopy; }
        break; }
      case OP_CONV: /* values are dynamically typed; coerce lazily */ break;
      case OP_NEG: if(sp>0) stk[sp-1]=vreal(-asnum(stk[sp-1])); break;
      case OP_NOT: if(sp>0) stk[sp-1]=vreal(!astrue(stk[sp-1])); break;
      case OP_MUL: case OP_DIV: case OP_REM: case OP_MOD: case OP_ADD: case OP_SUB:
      case OP_AND: case OP_OR: case OP_XOR: case OP_SHL: case OP_SHR:{
        if(sp<2) break; GmlVal r=stk[--sp], l=stk[--sp];
        if(in.kind==OP_ADD && l.t==V_STR && r.t==V_STR){
          int la=strlen(l.s), lb=strlen(r.s); char *c=malloc(la+lb+1);
          memcpy(c,l.s,la); memcpy(c+la,r.s,lb+1); stk[sp++]=vstr_owned(c); GC_TRACK(c); break;
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
        if(l.t==V_STR || r.t==V_STR){ char lb[64], rb[64];
          int c=strcmp(asstr_cmp(l,lb,sizeof lb),asstr_cmp(r,rb,sizeof rb));
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
        /* track a freshly-malloc'd string result so it's freed (else string builtins leak). Skip
         * arg pass-through (the arg's owner frees it) to avoid double-tracking a var's string. */
        if(STR_IS_HEAP(rv)){ int isarg=0; for(int _k=0;_k<na;_k++) if(a[_k].t==V_STR && a[_k].s==rv.s){isarg=1;break;} if(!isarg) GC_TRACK(rv.s); }
        if(sp<STK) stk[sp++]=rv;
        break;
      }
      case OP_CALLV:{
        /* GMS2.3 call-a-value: a function VALUE sits under the args. GM stack order is
         * func, argN..arg1, arg0 (arg0 on top). Pop args (arg0 first) then the function value.
         * A tagged function-value (from a push.i32 fref or method()) carries the code index. */
        int na=in.argc; GmlVal a[64]; if(na>64) na=64;
        int fci=-1; GmlInstance *call_self=vm->cur_self;
        /* A METHOD call `obj.method(args)` leaves [args.., self, method] — bound method on TOP, accessor
         * instance right under it (via the dup-swap). A plain funcval call leaves the func at the BOTTOM
         * (arg0 on top). Peek: bound-method struct on top => method convention. */
        GmlInstance *bm_top=NULL;
        if(sp>0){ double tv=asnum(stk[sp-1]);
          if(GML_IS_STRUCT_ID(tv)){ GmlInstance *b=gml_struct_find(vm,(unsigned)tv);
            if(b && gml_varmap_get(&b->vars,"__fn")) bm_top=b; } }
        if(bm_top){
          sp--;   /* the method value */
          GmlVal *pf=gml_varmap_get(&bm_top->vars,"__fn"), *ps=gml_varmap_get(&bm_top->vars,"__self");
          if(pf){ int f2=(int)asnum(*pf); if(GML_IS_FUNCVAL(f2)) fci=f2 & 0x00FFFFFF; }
          GmlInstance *bs = ps? vm_inst_from_ref(vm,*ps) : NULL;
          if(ps && sp>0 && asnum(stk[sp-1])==asnum(*ps)) sp--;   /* drop the accessor self (obj. in obj.method) */
          if(bs) call_self=bs;
          for(int i=0;i<na;i++) a[i]= sp>0? stk[--sp] : vreal(0);
        } else {
          for(int i=0;i<na;i++) a[i]= sp>0? stk[--sp] : vreal(0);   /* a[0]=arg0 (top) */
          for(int i=na;i<in.argc;i++) if(sp>0) sp--;                /* drop overflow args */
          GmlVal fv = sp>0? stk[--sp] : vreal(0);
          double fvn=asnum(fv);
          if(GML_IS_STRUCT_ID(fvn)){   /* bound method value called directly */
            GmlInstance *bm=gml_struct_find(vm,(unsigned)fvn);
            if(bm){ GmlVal *pf=gml_varmap_get(&bm->vars,"__fn"), *ps=gml_varmap_get(&bm->vars,"__self");
              if(pf){ int f2=(int)asnum(*pf); if(GML_IS_FUNCVAL(f2)) fci=f2 & 0x00FFFFFF; }
              if(ps){ GmlInstance *bs=vm_inst_from_ref(vm,*ps); if(bs) call_self=bs; } } }
          else if(GML_IS_FUNCVAL((int)fvn)) fci = (int)fvn & 0x00FFFFFF;
        }
        GmlVal rv=vreal(0);
        if(fci>=0 && fci<w->n_code) rv=gml_vm_run_code(vm,fci,call_self,vm->cur_other,a,na);
        /* track a heap-string result so it's freed at scope exit (mirror OP_CALL); args stay owned
         * by this frame's str_gc and are freed there, so don't touch them. */
        if(STR_IS_HEAP(rv)){ int isarg=0; for(int _k=0;_k<na;_k++) if(a[_k].t==V_STR && a[_k].s==rv.s){isarg=1;break;} if(!isarg) GC_TRACK(rv.s); }
        if(sp<STK) stk[sp++]=rv;
        break;
      }
      case OP_RET: ret = sp>0? stk[--sp]:vreal(0); pc=end; continue;
      case OP_EXIT: ret=vreal(0); pc=end; continue;
      case OP_PUSHENV:{ /* with(target): pop the target, iterate matching instances */
        GmlVal tv = sp>0? stk[--sp] : vreal(0);
        /* For the StackTop environment form, consume the -9 sentinel and the target below it. */
        if(w->bytecode>=17 && sp>0 && tv.t==V_REAL && tv.d==-9.0 &&
           pc>=start+4 && u32(d,pc-4)==0x840FFFF7u) tv=stk[--sp];
        int T=(int)asnum(tv);
        GmlInstance **list=NULL; int nn=0, capL=0;
        #define WADD(p) do{ if(nn>=capL){ capL=capL?capL*2:8; list=realloc(list,capL*sizeof(void*)); } list[nn++]=(p); }while(0)
        if(GML_IS_STRUCT_ID((double)T)){ GmlInstance *p=gml_struct_find(vm,(unsigned)T); if(p) WADD(p); }  /* with(struct): GMS2.3 runs the body with self=the struct (e.g. serialize's `with(action){..self.value..}`) */
        else if(T>=100000){ GmlInstance *p=inst_by_id(vm,T); if(p) WADD(p); }
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
      case OP_BREAK:
        /* Dispatch extended break operations with their corresponding stack operands. */
        switch(in.sval){
          case -11: /* pushref: push an asset/function reference encoded as (type<<24 | id). Engine
                     * sprite/script/font indices match GM asset ids, so the low-24 id addresses the
                     * right resource for draw_sprite_ext & OP_CALLV. */
            if(sp<STK) stk[sp++]=vreal((double)(in.ival & 0x00FFFFFF)); break;
          case -2:   /* pushaf: A[idx] where the array value A is on the stack. Stack: idx, A(top->down). */
          case -4:{  /* pushac reads an intermediate array value for a chained access. */
            int idx=(int)(sp>0?asnum(stk[--sp]):0); GmlVal av=sp>0?stk[--sp]:vreal(0);
            GmlVal out=vreal(0);
            if(av.t==V_ARR && av.arr){ GmlArr *A=av.arr; if(idx>=0 && idx<A->len) out=A->data[idx]; }
            if(sp<STK) stk[sp++]=out; break; }
          case -3:{ /* popaf: A[idx] = value. */
            if(aref_n>0){ /* compound-assign write: use the reference saved at savearef; the store
               * value is the stack top. Restore the stack to just below the reference. */
              int a=--aref_n; GmlVal val=sp>0?stk[sp-1]:vreal(0);
              GmlArr *A=aref[a].arr; int idx=aref[a].idx;
              if(A && idx>=0){ arr_ensure(A,idx); if(idx<A->cap){ A->data[idx]=val; if(val.t==V_STR) GC_UNTRACK(val.s); } }
              if(aref[a].base>=0 && aref[a].base<=sp) sp=aref[a].base;
              break; }
            /* plain store. Stack: idx, A, value (top->down). */
            int idx=(int)(sp>0?asnum(stk[--sp]):0); GmlVal av=sp>0?stk[--sp]:vreal(0);
            GmlVal val=sp>0?stk[--sp]:vreal(0);
            if(av.t==V_ARR && av.arr && idx>=0){ GmlArr *A=av.arr; arr_ensure(A,idx);
              if(idx<A->cap){ A->data[idx]=val; if(val.t==V_STR) GC_UNTRACK(val.s); } }
            break; }
          case -5: /* setowner: pop the copy-on-write owner id (COW is not modelled; arrays mutate in place). */
            if(sp>0) sp--; break;
          case -8:{ /* savearef: remember the array element being read so the paired popaf can write
             * it back. Stack top is [idx, array] (top->down); the read (pushaf) consumes them. */
            if(sp>=2 && stk[sp-2].t==V_ARR && stk[sp-2].arr && aref_n<16){
              aref[aref_n].arr=stk[sp-2].arr; aref[aref_n].idx=(int)asnum(stk[sp-1]);
              aref[aref_n].base=sp-2; aref_n++; }
            break; }
          /* chkindex(-1), isstaticok(-6)->treated as false, setstatic(-7), restorearef(-9),
           * chknullish(-10): no net effect on the value stack in our model. */
          default: break;
        }
        break;
      default: break;
    }
    pc=nextpc;
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
  varmap_free_ex(&locals,1);      /* keep escaped arrays alive — persistent slots alias them */
  vm->cur_self=save_self; vm->cur_other=save_other;
  g_cur_code_name=save_code_name;
  g_cur_vm=save_cur_vm;
  vm->script_argc=save_argc;
  for(int i=0;i<16;i++) vm->script_args[i]=save_args[i];
  (void)g_unknown_logged;
  g_run_depth--;
  return ret;
}

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
  uint32_t n=u32(d,base); vm->paths=calloc(n>0?n:1,sizeof(GmlPath)); vm->n_paths=n;
  for(uint32_t i=0;i<n;i++){
    uint32_t ep=u32(d,base+4+i*4);
    GmlPath *p=&vm->paths[i];
    /* GMS1.4 PATH entry: [name_ptr:u32][kind:u32(0=straight,1=smooth)][closed:u32][precision:u32]
     * [n_points:u32][points: each x(f32),y(f32),speed_factor(f32)=12B]. */
    p->kind=(int)u32(d,ep+4);
    p->closed=(int)u32(d,ep+8);
    p->precision=(int)u32(d,ep+12);
    int npt=(int)u32(d,ep+16);
    PathCtlPt *ctl=calloc(npt>0?npt:1,sizeof(PathCtlPt));
    for(int k=0;k<npt;k++){ uint32_t po=ep+20+k*12;
      ctl[k].x=f32(d,po); ctl[k].y=f32(d,po+4); ctl[k].sp=f32(d,po+8); }
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
  in->path_position=0; in->path_positionprevious=0;
  double px,py; path_eval(&vm->paths[path],0,&px,&py);
  if(absolute){
    in->path_xoff=0; in->path_yoff=0; in->path_origin_x=0; in->path_origin_y=0;
  } else {
    in->path_xoff=in->x; in->path_yoff=in->y; in->path_origin_x=px; in->path_origin_y=py;
  }
  path_world_xy(in,px,py,&in->x,&in->y); gml_colgrid_touch(in);
}
static void path_log_step(GmlVM *vm, GmlInstance *in, int pi){
  const char *lp=getenv("GML_LOG_PATH");
  if(!lp) return;
  const char *on=(in->obj>=0&&in->obj<vm->n_objects)?vm->objects[in->obj].name:"?";
  if(!*lp || strstr(on,lp)){ extern long g_vm_frame;
    fprintf(stderr,"[path] f%ld %s id=%u pi=%d p=%.3f ori=%.1f scl=%.2f @(%.2f,%.2f)\n",
            g_vm_frame,on,in->id,pi,in->path_position,in->path_orientation,in->path_scale,in->x,in->y);
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
  path_world_xy(in,px,py,&in->x,&in->y); gml_colgrid_touch(in);
}

/* advance every path-following instance one step (called each frame in the movement phase). */
static void run_paths(GmlVM *vm){
  for(int i=0;i<vm->inst_count;i++){ GmlInstance *in=&vm->inst[i];
    if(!in->active||in->marked) continue;
    int pi=(int)in->path_index; if(pi<0||pi>=vm->n_paths) continue;
    GmlPath *p=&vm->paths[pi]; if(p->len<=0){ continue; }
    in->path_positionprevious=in->path_position;
    double scl=in->path_scale!=0?fabs(in->path_scale):1;
    double sf=path_speed_factor(p,in->path_position)/100.0;
    if(sf<0) sf=0;
    double next_pos=in->path_position + (in->path_speed*sf) / (p->len*scl); /* path point speed is a percent */
    int ended=(next_pos>=1.0 || next_pos<0.0);
    if(ended) in->path_position=(next_pos>=1.0)?1.0:0.0;
    else in->path_position=next_pos;
    double px,py; path_eval(p,in->path_position,&px,&py);
    path_world_xy(in,px,py,&in->x,&in->y); gml_colgrid_touch(in);
    path_log_step(vm,in,pi);
    /* GM "End of Path" event (Other, subtype 8): fired when the path reaches its end. Many objects
     * chain off this (an intro object flies in on a path, then a later event spawns the enemy). */
    if(ended){
      uint32_t id=in->id;
      int ea=(int)in->path_endaction;
      double boundary=in->path_position;
      int hit_end=next_pos>=1.0;
      gml_run_event(vm,in,"Other_8");
      in=inst_by_id(vm,(double)id);
      if(!in||!in->active||in->marked) continue;
      if((int)in->path_index==pi && (int)in->path_endaction==ea && fabs(in->path_position-boundary)<1e-9)
        path_apply_endaction(vm,in,pi,ea,next_pos,hit_end);
    }
  }
}

/* ---------------- object parsing ---------------- */
static void parse_objects(GmlVM *vm){
  GmlWin *w=vm->win; const GmlChunk *c=gml_chunk(w,"OBJT"); if(!c) return;
  const uint8_t *d=w->data; uint32_t n=u32(d,c->off);
  vm->n_objects=(int)n; vm->objects=calloc(n,sizeof(GmlObject));
  /* Select the OBJT parent offset by checking candidate columns for valid object
   * indices and parent sentinels. Account for the optional field after Visible. */
  int poff = 24;
  if(w->bytecode>=17){
    int best=-1;
    for(int cand=28; cand>=24; cand-=4){
      int ok=1, saw_root=0;
      for(uint32_t i=0;i<n;i++){
        uint32_t p=u32(d,c->off+4+i*4);
        int32_t v=(int32_t)u32(d,p+cand);
        if(v==-100){ saw_root=1; continue; }
        if(v<-1 || v>=(int32_t)n || v==(int32_t)i){ ok=0; break; }
      }
      if(ok && saw_root){ best=cand; break; }
    }
    poff = best>=0 ? best : 28;   /* fall back to the newer layout if neither column is clean */
  }
  int shift = poff-24;   /* 4 when the Managed field is present, 0 for the classic layout */
  for(uint32_t i=0;i<n;i++){
    uint32_t p=u32(d,c->off+4+i*4);
    GmlObject *o=&vm->objects[i];
    o->name=gml_str_by_ptr(w,u32(d,p));
    o->sprite_index=(int)u32(d,p+4);
    o->visible=(int)u32(d,p+8); o->solid=(int)u32(d,p+12+shift);
    o->depth=(int)u32(d,p+16+shift); o->persistent=(int)u32(d,p+20+shift);
    o->parent=(int)u32(d,p+poff);
    o->mask_index=(int)u32(d,p+poff+4);
    /* events parsed lazily via code-name matching for now */
  }
  if(getenv("GML_LOG_OBJ")) fprintf(stderr,"[obj] OBJT parent_off=+%d (%s layout)\n", poff,
    shift? "managed/GMS2.3+" : "classic");
  { const char *dbg=getenv("GML_DBG_OBJREC");   /* print one object's parsed record by name */
    if(dbg) for(uint32_t i=0;i<n;i++) if(vm->objects[i].name && !strcmp(vm->objects[i].name,dbg)){
      GmlObject *o=&vm->objects[i];
      fprintf(stderr,"[objrec] %s idx=%u spr=%d vis=%d solid=%d depth=%d pers=%d parent=%d mask=%d\n",
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
  if(getenv("GML_LOG_OBJ")) fprintf(stderr,"[obj] n_objects=%d parent_cycles_cut=%d disp=%ux%u\n", vm->n_objects, n_cut, w->disp_w, w->disp_h);
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
  for(int i=0;i<vm->inst_count;i++)
    if(vm->inst[i].active && !vm->inst[i].marked && gml_object_is(vm,vm->inst[i].obj,target)) n++;
  return n;
}
static int run_event_code_from(GmlVM *vm, GmlInstance *in, GmlInstance *other,
                               const char *suffix, int obj, int ci){
  const char *pe=vm->cur_event; int peo=vm->cur_event_obj;
  vm->cur_event=suffix; vm->cur_event_obj=obj;
  GmlVal _r=gml_vm_run_code(vm,ci,in,other,NULL,0);
  if(_r.t==V_STR && _r.d!=0) free((char*)_r.s);   /* discarded owned return: free it */
  vm->cur_event=pe; vm->cur_event_obj=peo;
  return 1;
}
static uint32_t suffix_hash(const char *s){
  uint32_t h=2166136261u;
  for(const unsigned char *p=(const unsigned char*)s; p && *p; p++) h=(h^*p)*16777619u;
  return h;
}
static int event_lookup_from(GmlVM *vm, const char *suffix, int obj, int *handler_obj, int *code){
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
    char name[160];
    int ho=-1, ci=-1;
    for(int p=obj; p>=0 && p<vm->n_objects; p=vm->objects[p].parent){
      snprintf(name,sizeof name,"gml_Object_%s_%s",vm->objects[p].name,suffix);
      ci=gml_code_index_by_name(vm->win,name);
      if(ci>=0){ ho=p; break; }
    }
    c->valid=1; c->start_obj=obj; c->handler_obj=ho; c->code=ci;
    snprintf(c->suffix,sizeof c->suffix,"%s",suffix);
    if(ci<0) return 0;
    if(handler_obj) *handler_obj=ho;
    if(code) *code=ci;
    return 1;
  }
  char name[160];
  for(int p=obj; p>=0 && p<vm->n_objects; p=vm->objects[p].parent){
    snprintf(name,sizeof name,"gml_Object_%s_%s",vm->objects[p].name,suffix);
    int ci=gml_code_index_by_name(vm->win,name);
    if(ci>=0){
      if(handler_obj) *handler_obj=p;
      if(code) *code=ci;
      return 1;
    }
  }
  return 0;
}
static double vmprof_now(void);
/* find+run `suffix` starting from object level `obj`, walking up the parent chain. Tracks the
 * current event (suffix + object level) so event_inherited can re-dispatch to the parent. */
static int run_event_from(GmlVM *vm, GmlInstance *in, const char *suffix, int obj){
  int handler_obj=-1, ci=-1;
  { static const char *ev=(const char*)-1;   /* GML_LOG_EVENT=<obj_name>: trace every event dispatched to that object */
    if(ev==(const char*)-1) ev=getenv("GML_LOG_EVENT");   /* cached: this ran per dispatch (thousands/frame) */
    extern long g_vm_frame;
    if(ev && in && in->obj>=0 && in->obj<vm->n_objects && vm->objects[in->obj].name &&
       (!strcmp(ev,"*") || !strcmp(vm->objects[in->obj].name,ev)))
      fprintf(stderr,"[event] f%ld %s.%s id=%u other=%u\n",
        g_vm_frame,vm->objects[in->obj].name,suffix,in->id,vm->cur_other?vm->cur_other->id:0); }
  if(!event_lookup_from(vm,suffix,obj,&handler_obj,&ci)) return 0;
  /* GML_DBG_EVTIME accumulates wall-clock milliseconds per handler/event and reports every 300 frames. */
  { static int evt_on=-1; if(evt_on<0) evt_on=getenv("GML_DBG_EVTIME")!=NULL;
    if(evt_on){
      static struct { int obj; char suf[24]; double ms; long runs; } tab[256]; static int ntab=0;
      static long lastf=0, frames=0;
      double t0=vmprof_now();
      int r=run_event_code_from(vm,in,vm->cur_other,suffix,handler_obj,ci);
      double dt=vmprof_now()-t0;
      int k=0; for(;k<ntab;k++) if(tab[k].obj==handler_obj && !strcmp(tab[k].suf,suffix)) break;
      if(k==ntab && ntab<256){ tab[k].obj=handler_obj; snprintf(tab[k].suf,sizeof tab[k].suf,"%s",suffix); tab[k].ms=0; tab[k].runs=0; ntab++; }
      if(k<ntab){ tab[k].ms+=dt; tab[k].runs++; }
      extern long g_vm_frame;
      if(g_vm_frame!=lastf){ lastf=g_vm_frame;
        if(++frames%300==0){
          fprintf(stderr,"[evtime] top por-300f:\n");
          for(int pass=0;pass<12;pass++){ int best=-1; double bm=-1;
            for(int j=0;j<ntab;j++) if(tab[j].ms>bm){ bm=tab[j].ms; best=j; }
            if(best<0 || bm<=0) break;
            fprintf(stderr,"   %8.2fms %6ld runs  %s.%s\n",tab[best].ms,tab[best].runs,
              (tab[best].obj>=0&&tab[best].obj<vm->n_objects)?vm->objects[tab[best].obj].name:"?",tab[best].suf);
            tab[best].ms=-1; }
          ntab=0; } }
      return r;
    } }
  /* Preserve `other`: an event fired from inside another instance's scope
   * (event_user / event_perform / action_inherited) must see the caller as
   * `other`. Engine-triggered events enter with cur_other == NULL, so this is
   * a no-op for Step/Alarm dispatch. */
  return run_event_code_from(vm,in,vm->cur_other,suffix,handler_obj,ci);
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
static void obj_list_link(GmlVM *vm, GmlInstance *in);
static void obj_list_unlink(GmlVM *vm, GmlInstance *in, int obj);
static GmlInstance *alloc_inst(GmlVM *vm){
  /* a deactivated instance keeps active=0 but must NOT have its slot reused (it still exists). */
  int first = vm->step_alloc_base > 0 ? vm->step_alloc_base : 0;
  for(int i=first;i<vm->inst_count;i++) if(!vm->inst[i].active && !vm->inst[i].deactivated){ memset(&vm->inst[i],0,sizeof(GmlInstance)); return &vm->inst[i]; }
  if(vm->inst_count>=vm->inst_cap){
    /* pool full: do NOT realloc — moving the array would dangle every held instance pointer
     * (cur_self/cur_other, with-frames, the room-enter loop) and segfault. Reuse the last slot. */
    static int warned=0; if(!warned){ warned=1; extern long g_vm_frame;
      fprintf(stderr,"[gml] f%ld instance pool full (%d); reusing slots\n",g_vm_frame,vm->inst_cap); }
    GmlInstance *in=&vm->inst[vm->inst_cap-1];
    if(in->active||in->deactivated){ gml_obj_alive_adjust(vm,in->obj,-1); obj_list_unlink(vm,in,in->obj); }
    varmap_free(&in->vars); memset(in,0,sizeof(*in)); return in;
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
/* per-exact-type instance lists (for family-candidate enumeration in run_collisions) */
static void obj_list_link(GmlVM *vm, GmlInstance *in){
  if(!vm->obj_head || in->obj<0 || in->obj>=vm->n_objects) return;
  int i=(int)(in-vm->inst);
  vm->inst_prev[i]=-1; vm->inst_next[i]=vm->obj_head[in->obj];
  if(vm->obj_head[in->obj]>=0) vm->inst_prev[vm->obj_head[in->obj]]=i;
  vm->obj_head[in->obj]=i; vm->obj_list_gen++;
}
static void obj_list_unlink(GmlVM *vm, GmlInstance *in, int obj){
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
      gml_obj_alive_adjust(vm,in->obj,1); obj_list_link(vm,in); } }
  vm->obj_list_gen++;
}
static void init_inst(GmlVM *vm, GmlInstance *in, double x, double y, int obj){
  in->active=1; in->marked=0; in->obj=obj; in->id=vm->next_id++;
  gml_obj_alive_adjust(vm,obj,1); obj_list_link(vm,in);
  in->x=in->xstart=x; in->y=in->ystart=y; in->xprevious=x; in->yprevious=y;
  gml_colgrid_touch(in);   /* fresh instance: unknown to the current grid build */
  in->mask_index=-1;
  in->image_xscale=in->image_yscale=1; in->image_alpha=1; in->image_speed=1;
  in->image_blend=16777215; in->visible=1; in->depth=0;
  in->gravity_direction=270;   /* GM default: gravity pulls straight down */
  in->path_index=-1; in->path_scale=1; in->path_speed=0; in->path_position=0;
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
    const uint8_t *d=vm->win->data; uint32_t n=u32(d,c->off);
    for(uint32_t r=0;r<n;r++){
      uint32_t p=u32(d,c->off+4+r*4); if(!p) continue;
      uint32_t op=u32(d,p+48); if(!op) continue;
      uint32_t cnt=u32(d,op);
      if(cnt>=2){ int s=(int)(u32(d,op+8)-u32(d,op+4)); if(s>=36 && s<=64){ stride=s; break; } }
    }
  }
  vm->room_rec_stride=stride;
  return stride;
}
static void apply_room_instance_transform(GmlVM *vm, GmlInstance *in, uint32_t ip){
  if(!vm || !vm->win || !in || ip > vm->win->size || vm->win->size-ip < 48) return;
  const uint8_t *d=vm->win->data;
  int wide = room_inst_stride(vm)>=48;   /* ImageSpeed/ImageIndex present; color/rot shifted +8 */
  float sx=f32(d,ip+20), sy=f32(d,ip+24), rot=f32(d,ip+(wide?40:32));
  if(!isfinite(sx) || !isfinite(sy) || !isfinite(rot)) return;
  if(fabs((double)sx)<1e-9 || fabs((double)sy)<1e-9) return;
  in->image_xscale=sx;
  in->image_yscale=sy;
  in->image_blend=(double)(u32(d,ip+(wide?36:28))&0xFFFFFFu);
  in->image_angle=rot;
  if(wide){
    float isp=f32(d,ip+28), iix=f32(d,ip+32);
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
GmlInstance *gml_instance_create(GmlVM *vm, double x, double y, int obj){
  GmlInstance *in=alloc_inst(vm); init_inst(vm,in,x,y,obj);
  if(getenv("GML_LOG_CREATE")){ extern long g_vm_frame;
    fprintf(stderr,"[create] f%ld %s @(%.0f,%.0f) spr=%d\n",g_vm_frame,
    (obj>=0&&obj<vm->n_objects)?vm->objects[obj].name:"?",x,y,(int)in->sprite_index); }
  gml_run_event(vm,in,"PreCreate_0");   /* GMS2: runs before Create; sets IDE variable-definitions */
  gml_run_event(vm,in,"Create_0");
  return in;
}
void gml_instance_change(GmlVM *vm, GmlInstance *in, int obj, int perform_events){
  gml_colgrid_touch(in);   /* object swap changes sprite/mask -> bbox */
  if(!vm || !in || !in->active || in->marked || obj<0 || obj>=vm->n_objects) return;
  gml_obj_alive_adjust(vm,in->obj,-1); obj_list_unlink(vm,in,in->obj);
  gml_obj_alive_adjust(vm,obj,1);       /* relinked with the new type after defaults land */
  if(perform_events){
    gml_run_event(vm,in,"Destroy_0");
    if(!in->active || in->marked){ obj_list_link(vm,in); return; }   /* died mid-change: keep lists sane until reap */
  }
  apply_object_defaults(vm,in,obj);
  obj_list_link(vm,in);
  if(perform_events && in->active && !in->marked){ gml_run_event(vm,in,"PreCreate_0"); gml_run_event(vm,in,"Create_0"); }
}
void gml_instance_destroy(GmlVM *vm, GmlInstance *in){
  if(!in||!in->active) return;
  gml_run_event(vm,in,"Destroy_0");
  in->marked=1;
}
static void reap(GmlVM *vm){
  /* Skip escaped arrays during instance reaping; shared arrays remain for deduplicated full teardown. */
  for(int i=0;i<vm->inst_count;i++) if(vm->inst[i].active && vm->inst[i].marked){
    gml_obj_alive_adjust(vm,vm->inst[i].obj,-1); obj_list_unlink(vm,&vm->inst[i],vm->inst[i].obj);
    varmap_free_ex(&vm->inst[i].vars,1); vm->inst[i].active=0; vm->inst[i].marked=0; }
}

static void set_global_arr(GmlVM *vm, const char *nm, int idx, double val){
  GmlVal *slot=gml_varmap_put(&vm->globals,nm); GmlArr *A=arr_of(slot); arr_ensure(A,idx);
  if(idx>=0 && idx<A->cap) A->data[idx]=vreal(val);
}
static double get_global_arr_d(GmlVM *vm, const char *nm, int idx){
  GmlVal *slot=gml_varmap_get(&vm->globals,nm); if(!slot||slot->t!=V_ARR) return 0;
  GmlArr *A=slot->arr; return (A && idx>=0 && idx<A->len)? asnum(A->data[idx]) : 0;
}
void gml_set_global_arr(GmlVM *vm, const char *nm, int idx, double val){ set_global_arr(vm,nm,idx,val); }
/* Detect layer type-data offset 36 or 48 by checking background sprite indices.
 * Share the selected layout between drawing and room entry. */
int gml_room_layer_data_off(GmlVM *vm){
  if(vm->layer_data_off) return vm->layer_data_off;
  vm->layer_data_off=36;
  const GmlChunk *rc=gml_chunk(vm->win,"ROOM");
  const GmlChunk *sc=gml_chunk(vm->win,"SPRT");
  if(!rc||!sc) return vm->layer_data_off;
  const uint8_t *d=vm->win->data;
  int nspr=(int)u32(d,sc->off);
  int nrooms=gml_room_count(vm->win);
  for(int ri=0;ri<nrooms;ri++){
    uint32_t rp=u32(d,rc->off+4+ri*4);
    uint32_t lay=(rp && rp+92<vm->win->size)?u32(d,rp+88):0;
    uint32_t lcnt=(lay && lay+4<vm->win->size)?u32(d,lay):0;
    if(!lcnt || lcnt>=512) continue;
    for(uint32_t i=0;i<lcnt;i++){ uint32_t lp=u32(d,lay+4+i*4);
      if(!lp || lp+96>vm->win->size || u32(d,lp+8)!=1) continue;
      int s48=(int32_t)u32(d,lp+56), v48=(int)u32(d,lp+48);
      if(v48<=1 && s48>=-1 && s48<nspr && (u32(d,lp+72)>>24)>=0x7F) vm->layer_data_off=48;
      return vm->layer_data_off;
    }
  }
  return vm->layer_data_off;
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
static void gml_tilemaps_clear(GmlVM *vm){
  for(int i=0;i<vm->n_tilemaps;i++){
    free(vm->tilemaps[i].owned_tiles);
    vm->tilemaps[i].owned_tiles=NULL;
  }
  vm->n_tilemaps=0;
}
GmlTileMap *gml_tilemap_find(GmlVM *vm, int id){
  for(int i=0;i<vm->n_tilemaps;i++) if(vm->tilemaps[i].used && vm->tilemaps[i].id==id) return &vm->tilemaps[i];
  return NULL;
}
static int gml_tilemap_ensure_owned(GmlTileMap *tm){
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
  if(!gml_tilemap_ensure_owned(tm)) return 0;
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
  int lid=(int)asnum(v);
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
        extern long g_vm_frame;
        long fin=g_vm_frame - vm->room_enter_frame; if(fin<0) fin=0;
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

/* Rebuild a room's derived GMS2 layer data from immutable ROOM records. Runtime layers/elements are
 * rebuilt on room enter; after modern savestate loads they are preserved and only type-4 tilemap
 * views are rebound to win data, since those grids are not serialized by pointer. */
static void gml_room_reload_layers_mode(GmlVM *vm, int room_index, int rebuild_runtime_layers){
  if(rebuild_runtime_layers){ vm->n_rtl=0; vm->n_rte=0; }
  if(vm->win->bytecode<17) return;
  const GmlChunk *rlc=gml_chunk(vm->win,"ROOM"); const uint8_t *rd=vm->win->data;
  uint32_t rp = rlc ? u32(rd,rlc->off+4+room_index*4) : 0;
  uint32_t lay = (rp && rp+92<vm->win->size) ? u32(rd,rp+88) : 0;
  uint32_t lcnt = (lay && lay+4<vm->win->size) ? u32(rd,lay) : 0;
  if(rebuild_runtime_layers && lcnt<512) for(uint32_t i=0;i<lcnt;i++){ uint32_t lp=u32(rd,lay+4+i*4);
    if(!lp || lp+40>vm->win->size) continue;
    uint32_t np=u32(rd,lp+0); if(!np || np>=vm->win->size) continue;
    GmlRtLayer *l=gml_rt_layer_new(vm); if(!l) break;
    snprintf(l->name,sizeof l->name,"%s",(const char*)(rd+np));
    l->depth=(double)(int32_t)u32(rd,lp+12);
    l->x=f32(rd,lp+16); l->y=f32(rd,lp+20); l->hs=f32(rd,lp+24); l->vs=f32(rd,lp+28);
    l->visible=u32(rd,lp+32)?1:0; l->touched=0;
  }
  gml_tilemaps_clear(vm);
  int doff = gml_room_layer_data_off(vm);
  const GmlChunk *bc = gml_chunk(vm->win,"BGND");
  uint32_t bcnt = bc ? u32(rd,bc->off) : 0;
  if(lcnt<512) for(uint32_t i=0;i<lcnt;i++){ uint32_t lp=u32(rd,lay+4+i*4);
    if(!lp || lp+(uint32_t)doff+12>vm->win->size) continue;
    if(u32(rd,lp+8)!=4) continue;                 /* layer type 4 = tile layer */
    int tileset=(int32_t)u32(rd,lp+doff);
    int cols=(int32_t)u32(rd,lp+doff+4), rows=(int32_t)u32(rd,lp+doff+8);
    if(cols<=0||rows<=0||cols>8192||rows>8192) continue;
    uint32_t tdata=lp+(uint32_t)doff+12;
    if((uint64_t)tdata + (uint64_t)cols*rows*4 > vm->win->size) continue;
    int tw=16,th=16;
    if(bc && tileset>=0 && (uint32_t)tileset<bcnt){ uint32_t bp=u32(rd,bc->off+4+tileset*4);
      if(bp && bp+32<vm->win->size){ int w=(int32_t)u32(rd,bp+24),h=(int32_t)u32(rd,bp+28);
        if(w>0)tw=w; if(h>0)th=h; } }
	    GmlTileMap *tm=gml_tilemap_new(vm); if(!tm) break;
	    uint32_t np2=u32(rd,lp+0);
	    snprintf(tm->name,sizeof tm->name,"%s",(np2&&np2<vm->win->size)?(const char*)(rd+np2):"");
	    tm->tileset=tileset;
	    tm->depth=(double)(int32_t)u32(rd,lp+12);
	    tm->tw=tw; tm->th=th; tm->cols=cols; tm->rows=rows; tm->tiles=rd+tdata; tm->base_tiles=rd+tdata;
	    tm->x=f32(rd,lp+16); tm->y=f32(rd,lp+20); tm->visible=u32(rd,lp+32)?1:0;
      GmlRtLayer *rl=gml_rt_layer_find_by_name(vm,tm->name);
      if(rl){
        tm->visible=rl->visible;
        tm->depth=rl->depth;
        if(rl->touched){ tm->x=rl->x; tm->y=rl->y; }
      }
	  }
  if(getenv("GML_LOG_ROOM")){ fprintf(stderr,"[room] reload_layers room=%d: %d layers, %d tilemaps\n",room_index,vm->n_rtl,vm->n_tilemaps);
    for(int t=0;t<vm->n_tilemaps;t++){ GmlTileMap *tm=&vm->tilemaps[t];
      int solid=0; for(int c=0;c<tm->cols*tm->rows;c++){ uint32_t d=u32(tm->tiles,(uint32_t)c*4); if((d&0x7FFFF)!=0) solid++; }
      fprintf(stderr,"[tile]  tm[%d] name='%s' cols=%d rows=%d cell=%dx%d pos=%.0f,%.0f solid=%d\n",t,tm->name,tm->cols,tm->rows,tm->tw,tm->th,tm->x,tm->y,solid);
      if(getenv("GML_DUMP_GRID")){ for(int ry=0;ry<tm->rows && ry<40;ry++){ char line[200]; int lp=0;
        for(int rx=0;rx<tm->cols && rx<128;rx++){ uint32_t d=u32(tm->tiles,(uint32_t)(ry*tm->cols+rx)*4); line[lp++]=((d&0x7FFFF)!=0)?'#':'.'; }
        line[lp]=0; fprintf(stderr,"[grid t%d r%02d] %s\n",t,ry,line); } } }
  }
}
static void gml_room_reload_layers(GmlVM *vm, int room_index){
  gml_room_reload_layers_mode(vm,room_index,1);
}

void gml_room_enter(GmlVM *vm, int room_index){
  gml_colgrid_invalidate(vm);
  int prev_room=vm->room_index;
  /* Room lifecycle events are engine-dispatched. Do not let a stale `other` from the GML
   * context that requested the transition leak into room Create/Start events; room-placed
   * instances have no creator/collision partner. */
  vm->cur_self=NULL; vm->cur_other=NULL;
  /* Run GlobalScript root entries once before the first room's instances,
   * using a temporary self scope for their top-level statements. */
  if(!vm->started && !vm->gs_roots_run && vm->win->bytecode>=17){
    vm->gs_roots_run=1;
    for(int ci=0;ci<vm->win->n_code;ci++){
      const char *cn=vm->win->code[ci].name;
      if(!cn || strncmp(cn,"gml_GlobalScript_",17)) continue;
      static GmlInstance gs_scratch;
      memset(&gs_scratch,0,sizeof gs_scratch);
      gs_scratch.active=1; gs_scratch.obj=-1; gs_scratch.id=0;
      gs_scratch.image_xscale=gs_scratch.image_yscale=1; gs_scratch.image_alpha=1;
      gs_scratch.sprite_index=-1; gs_scratch.mask_index=-1; gs_scratch.path_index=-1;
      for(int a2=0;a2<GML_ALARMS;a2++) gs_scratch.alarm[a2]=-1;
      GmlVal _r=gml_vm_run_code(vm,ci,&gs_scratch,NULL,NULL,0);
      if(_r.t==V_STR && _r.d!=0) free((char*)_r.s);
      varmap_free_ex(&gs_scratch.vars,0);
      memset(&gs_scratch.vars,0,sizeof gs_scratch.vars);
    }
  }
  if(getenv("GML_LOG_ROOMGOTO")){ extern long g_vm_frame;
    const char *who=(vm->cur_self && vm->cur_self->obj>=0 && vm->cur_self->obj<vm->n_objects)?vm->objects[vm->cur_self->obj].name:"(none)";
    fprintf(stderr,"[roomgoto] f%ld %d -> %d  by=%s\n",g_vm_frame,prev_room,room_index,who); }
  vm->step_alloc_base=0;
  /* Room End (Other_5): fire on all active instances before clearing the old
   * room. Persistent instances survive. */
  if(vm->room_index>=0){
    for(int i=0;i<vm->inst_count;i++) if(vm->inst[i].active && !vm->inst[i].marked)
      gml_run_event(vm,&vm->inst[i],"Other_5");
  }
  /* clear non-persistent instances (incl. deactivated ones, which keep active=0).
   * GM fires Destroy_0 when clearing instances during room change. */
  for(int i=0;i<vm->inst_count;i++) if((vm->inst[i].active||vm->inst[i].deactivated) && !vm->inst[i].persistent){
    gml_run_event(vm,&vm->inst[i],"Destroy_0");
    /* Preserve escaped arrays during room cleanup because surviving globals or
     * persistent instances may reference them. Full teardown deduplicates releases. */
    gml_obj_alive_adjust(vm,vm->inst[i].obj,-1); obj_list_unlink(vm,&vm->inst[i],vm->inst[i].obj);
    varmap_free_ex(&vm->inst[i].vars,1); vm->inst[i].active=0; vm->inst[i].deactivated=0; }
  vm->room_index=room_index; vm->pending_room=-1;
  { extern long g_vm_frame; vm->room_enter_frame=g_vm_frame; }
  vm->n_tile_mut=0;   /* tile-layer mutations are per-room */
  vm->n_tile_del_at=0; /* tile_layer_delete_at marks are per-room */
  /* Register this room's GMS2 runtime layers (addressable by name) + type-4 tile-collision maps. */
  gml_room_reload_layers(vm, room_index);
  if(getenv("GML_LOG_ROOM")) fprintf(stderr,"[room] enter %d\n",room_index);
  GmlRoom r; if(gml_room_get(vm->win,room_index,&r)!=0) return;
  const uint8_t *d=vm->win->data; uint32_t op=r.obj_ptr, cnt=u32(d,op);
  /* Initialise the built-in background_* arrays from the room's background
   * layers. GML draw code can read this state during room startup. */
  if(r.bg_ptr){ uint32_t bc=u32(d,r.bg_ptr);
    for(uint32_t i=0;i<bc && i<8;i++){ uint32_t lp=u32(d,r.bg_ptr+4+i*4);
      int en=(int)u32(d,lp), fg=(int)u32(d,lp+4), def=(int)u32(d,lp+8);
      int bx=(int)u32(d,lp+12), by=(int)u32(d,lp+16), htl=(int)u32(d,lp+20), vtl=(int)u32(d,lp+24);
      int bh=(int32_t)u32(d,lp+28), bv=(int32_t)u32(d,lp+32);
      set_global_arr(vm,"background_visible",i,en?1:0);
      set_global_arr(vm,"background_foreground",i,fg?1:0);
      set_global_arr(vm,"background_index",i,en?def:-1);
      set_global_arr(vm,"background_x",i,bx);  set_global_arr(vm,"background_y",i,by);
      set_global_arr(vm,"background_htiled",i,htl?1:0); set_global_arr(vm,"background_vtiled",i,vtl?1:0);
      set_global_arr(vm,"background_hspeed",i,bh); set_global_arr(vm,"background_vspeed",i,bv);
      set_global_arr(vm,"background_alpha",i,1.0);          /* GM default */
      if(getenv("GML_LOG_BG")) fprintf(stderr,"[bg-init] layer%u en=%d fg=%d def=%d pos=(%d,%d) htiled=%d vtiled=%d speed=(%d,%d)\n",i,en,fg,def,bx,by,htl,vtl,bh,bv);
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
  /* room_set_viewport overrides (applied on entry, like GMS: the call edits room config) */
  if(vm->view_ovr) for(int i=0;i<8;i++){
    int k=room_index*8+i;
    if(k>=0 && k<vm->n_view_ovr && vm->view_ovr[k].set){
      struct GmlViewOvr *o=&vm->view_ovr[k];
      set_global_arr(vm,"view_visible",i,o->vis);
      set_global_arr(vm,"view_xport",i,o->x); set_global_arr(vm,"view_yport",i,o->y);
      set_global_arr(vm,"view_wport",i,o->w); set_global_arr(vm,"view_hport",i,o->h);
    } }
  if(getenv("GML_LOG_VIEW")) fprintf(stderr,"[view] room=%d dim=%ux%u view0 en=%.0f wview=%.0f hview=%.0f wport=%.0f hport=%.0f\n",
    vm->room_index, r.width, r.height, get_global_arr_d(vm,"view_visible",0),
    get_global_arr_d(vm,"view_wview",0), get_global_arr_d(vm,"view_hview",0),
    get_global_arr_d(vm,"view_wport",0), get_global_arr_d(vm,"view_hport",0));
  /* Room instances already have GameMaker ids in the data.win. Dynamic instances created by
   * room-start Create/Other_4 code must start above those ids, or StackTop writes to `nnn.var`
   * can resolve to an unrelated placed instance with the same id. */
  for(uint32_t i=0;i<cnt;i++){
    uint32_t ip=u32(d,op+4+i*4);
    uint32_t rid=u32(d,ip+12);
    if(rid>=vm->next_id) vm->next_id=rid+1;
  }
  /* Room-placed instances are all present before any Create event runs. Some
   * room setup code relies on seeing other already-placed instances. */
  int *room_inst_idx=malloc((cnt?cnt:1)*sizeof(int));
  for(uint32_t i=0;i<cnt;i++){
    uint32_t ip=u32(d,op+4+i*4);
    int32_t x=(int32_t)u32(d,ip), y=(int32_t)u32(d,ip+4); int obj=(int32_t)u32(d,ip+8);
    GmlInstance *in=alloc_inst(vm); init_inst(vm,in,x,y,obj);
    in->id=u32(d,ip+12);  /* room-assigned instance id */
    apply_room_instance_transform(vm,in,ip);
    room_inst_idx[i]=(int)(in-vm->inst);
  }
  /* Assign placed-instance depths from their type-2 room layers before Create events. */
  if(vm->win->bytecode>=17){
    const GmlChunk *rc=gml_chunk(vm->win,"ROOM");
    uint32_t rp = rc ? u32(d,rc->off+4+room_index*4) : 0;
    uint32_t lay = (rp && rp+92<vm->win->size) ? u32(d,rp+88) : 0;
    uint32_t lcnt = (lay && lay+4<vm->win->size) ? u32(d,lay) : 0;
    if(lcnt>0 && lcnt<512){
      int doff=gml_room_layer_data_off(vm);
      for(uint32_t i=0;i<lcnt;i++){ uint32_t lp=u32(d,lay+4+i*4);
        if(!lp || lp+(uint32_t)doff+4>vm->win->size || u32(d,lp+8)!=2) continue;
        double ldep=(double)(int32_t)u32(d,lp+12);
        uint32_t ic=u32(d,lp+(uint32_t)doff);
        if(ic>100000) continue;
        for(uint32_t k=0;k<ic;k++){
          uint32_t ip2=lp+(uint32_t)doff+4+k*4;
          if(ip2+4>vm->win->size) break;
          uint32_t iid=u32(d,ip2);
          for(uint32_t j=0;j<cnt;j++){ int idx=room_inst_idx[j];
            if(idx>=0 && idx<vm->inst_count && vm->inst[idx].id==iid){ vm->inst[idx].depth=ldep; break; } }
        }
      }
    }
  }
  /* Create + per-instance creation code, in room order, after every placed instance exists. */
  for(uint32_t i=0;i<cnt;i++){
    int idx=room_inst_idx[i]; if(idx<0 || idx>=vm->inst_count) continue;
    GmlInstance *in=&vm->inst[idx]; if(!in->active || in->marked) continue;
    gml_run_event(vm,in,"PreCreate_0");   /* GMS2: variable-definitions, before Create */
    gml_run_event(vm,in,"Create_0");
    /* Per-instance room creation code (ip+16 = CODE index, -1 if none) runs
     * after Create; this is how room-authored instance setup is applied. */
    uint32_t ip=u32(d,op+4+i*4);
    int cc=(int32_t)u32(d,ip+16);
    if(in->active && !in->marked && cc>=0 && cc<vm->win->n_code){ GmlVal _r=gml_vm_run_code(vm,cc,in,NULL,NULL,0); if(_r.t==V_STR && _r.d!=0) free((char*)_r.s); }
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
  /* Run room creation code with a temporary self scope after instance creation.
   * The scratch instance is outside the instance pool and discarded afterward. */
  if(r.creation_code>=0 && r.creation_code<vm->win->n_code){
    static GmlInstance cc_scratch;
    memset(&cc_scratch,0,sizeof cc_scratch);
    cc_scratch.active=1; cc_scratch.obj=-1; cc_scratch.id=0;
    cc_scratch.image_xscale=cc_scratch.image_yscale=1; cc_scratch.image_alpha=1;
    cc_scratch.sprite_index=-1; cc_scratch.mask_index=-1; cc_scratch.path_index=-1;
    for(int a2=0;a2<GML_ALARMS;a2++) cc_scratch.alarm[a2]=-1;
    GmlVal _r=gml_vm_run_code(vm,r.creation_code,&cc_scratch,NULL,NULL,0);
    if(_r.t==V_STR && _r.d!=0) free((char*)_r.s);
    varmap_free_ex(&cc_scratch.vars,0);
    memset(&cc_scratch.vars,0,sizeof cc_scratch.vars);
  }
  /* Room Start (Other_4) for the instances that existed at room start */
  n0 = vm->inst_count;
  for(int i=0;i<n0;i++) if(vm->inst[i].active && !vm->inst[i].marked)
    gml_run_event(vm,&vm->inst[i],"Other_4");
  /* Dispatch the gamepad-discovered asynchronous event. */
  gml_fire_gamepad_connected(vm);
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
/* GML_PROFILE_VM: per-phase wall time of gml_vm_step, printed every 300 frames (Linux dev aid) */
static struct { double anim,step1,alarms,input,step0,move,coll,step2,rest; long frames; } g_vmprof;
static double vmprof_now(void){
#ifndef _WIN32
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); return ts.tv_sec*1000.0+ts.tv_nsec/1e6;
#else
  return 0;
#endif
}
static int vmprof_on(void){ static int on=-1; if(on<0) on=getenv("GML_PROFILE_VM")!=NULL; return on; }
#define VMPROF_MARK(field) do{ if(pv){ double _t=vmprof_now(); g_vmprof.field += _t-pv_t; pv_t=_t; } }while(0)

void gml_vm_step(GmlVM *vm){
  g_vm_frame++;
  /* GMS2 layers scroll by their hspeed/vspeed each step. Runtime-scripted layers accumulate here;
   * untouched ones are derived on the fly from the room definition (see the draw path). */
  for(int i=0;i<vm->n_rtl;i++) if(vm->rtl[i].used && vm->rtl[i].touched){
    vm->rtl[i].x += vm->rtl[i].hs; vm->rtl[i].y += vm->rtl[i].vs; }
  /* Struct GC between frames (stack/locals empty here). GMS2.3 games mint transient structs every step
   * (a menu returning a fresh palette/description struct ~2-12/frame) that nothing keeps — without this the
   * pool grew ~unboundedly (500MB/h under menu load, cap hit in ~1.5h). No-op for games with no structs. */
  if(vm->n_structs>0 && g_vm_frame - vm->structs_last_gc_frame >= 120){
    vm->structs_last_gc_frame = g_vm_frame; gml_struct_gc(vm); }
  int pv=vmprof_on(); double pv_t=pv?vmprof_now():0;
  int n=vm->inst_count;
  int prev_alloc_base=vm->step_alloc_base;
  vm->step_alloc_base=n;
  /* Advance sprite animation and dispatch Animation End before Begin Step. */
  { GmlRender *R=(GmlRender*)vm->render;
    const char *anim_dbg=getenv("GML_ANIM_OBJ");   /* hoisted out of the loop (diagnostic only) */
    for(int i=0;i<n;i++){ GmlInstance *in=&vm->inst[i]; if(!in->active||in->marked) continue;
      int nf = R? gml_sprite_frames(R,(int)in->sprite_index):0;
      if(anim_dbg){ const char*on=(in->obj>=0&&in->obj<vm->n_objects)?vm->objects[in->obj].name:"";
        if(on && strstr(on,anim_dbg)){
          int si=(int)in->sprite_index; const char*sn=(R&&si>=0&&si<R->n_spr&&R->spr[si].name)?R->spr[si].name:"?";
          fprintf(stderr,"[anim] %s x=%.1f y=%.1f vs=%.1f spr=%d(%s) idx=%.2f nf=%d\n",on,in->x,in->y,in->vspeed,si,sn,in->image_index,nf); } }
      if(nf>0 && in->image_speed!=0){
        double ni=in->image_index+in->image_speed; int wrapped=(ni>=nf)||(ni<0);
        while(ni>=nf) ni-=nf; while(ni<0) ni+=nf; in->image_index=ni;
        /* event may create/destroy instances or stop this anim (image_single); don't touch `in` after */
        if(wrapped){ if(getenv("GML_LOG_ANIM")) fprintf(stderr,"[anim-end] %s (nf=%d)\n",
          (in->obj>=0&&in->obj<vm->n_objects)?vm->objects[in->obj].name:"?",nf);
          gml_run_event(vm,in,"Other_7"); } } } }
  VMPROF_MARK(anim);
  /* begin step */
  for(int i=0;i<n;i++) if(vm->inst[i].active && !vm->inst[i].marked) gml_run_event(vm,&vm->inst[i],"Step_1");
  VMPROF_MARK(step1);
  /* Alarm thresholds depend on bytecode version: below 16, decrement values
   * greater than -1 and fire below zero; later versions decrement positive values
   * and fire at or below zero. Set -1 before dispatch so handlers can re-arm. */
  int alarm_at_zero = vm->win && vm->win->bytecode >= 16;
  for(int i=0;i<n;i++){ GmlInstance *in=&vm->inst[i]; if(!in->active||in->marked) continue;
    for(int a=0;a<GML_ALARMS;a++){
      if(alarm_at_zero){ if(!(in->alarm[a]>0)) continue; } else { if(!(in->alarm[a]>-1)) continue; }
      in->alarm[a]-=1;
      int fire = alarm_at_zero ? (in->alarm[a]<=0) : (in->alarm[a]<0);
      if(fire){ in->alarm[a]=-1; char s[16]; snprintf(s,sizeof s,"Alarm_%d",a);
        if(getenv("GML_LOG_ALARM")) fprintf(stderr,"[alarm] f%ld %s.%s\n",g_vm_frame,
          (in->obj>=0&&in->obj<vm->n_objects)?vm->objects[in->obj].name:"?",s);
        gml_run_event(vm,in,s); } } }
  VMPROF_MARK(alarms);
  /* keyboard events (GM order: after alarms, before the normal step) */
  if(vm->n_key_events){
    extern int gml_input_key(int vk, int edge);
    for(int e=0;e<vm->n_key_events;e++){
      if(!gml_input_key(vm->key_events[e].vk, vm->key_events[e].kind)) continue;
      for(int i=0;i<n;i++) if(vm->inst[i].active && !vm->inst[i].marked)
        gml_run_event(vm,&vm->inst[i],vm->key_events[e].suffix);
    }
  }
  /* instance Mouse_<n> events (with the other input events). Hover = pointer (room coords)
   * inside the instance bbox; enter/leave tracked per instance in mouse_over. Subtypes per GM:
   * 0-2 button held over it, 3 no-button over it, 4-6 pressed, 7-9 released, 10 enter, 11 leave,
   * 50-58 global (no hover), 60/61 wheel. Only games that define Mouse_* events pay any cost. */
  if(vm->n_mouse_events){
    extern void gml_input_mouse(double*,double*,double*,double*,double*,double*,int*,int*,int*,int*);
    double mx,my; int mheld,mpressed,mreleased,mwheel;
    gml_input_mouse(&mx,&my,NULL,NULL,NULL,NULL,&mheld,&mpressed,&mreleased,&mwheel);
    for(int i=0;i<n;i++){
      GmlInstance *in=&vm->inst[i]; if(!in->active||in->marked||in->deactivated) continue;
      double l,t,r2,b; int hov=0;
      if(vm_bbox(vm,in,&l,&t,&r2,&b)) hov = mx>=l && mx<=r2 && my>=t && my<=b;
      unsigned char was=in->mouse_over; in->mouse_over=(unsigned char)hov;
      for(int e=0;e<vm->n_mouse_events;e++){
        int s=vm->mouse_events[e].sub, fire=0;
        switch(s){
          case 0: fire=hov&&(mheld&1); break;      case 1: fire=hov&&(mheld&2); break;
          case 2: fire=hov&&(mheld&4); break;      case 3: fire=hov&&!(mheld&7); break;
          case 4: fire=hov&&(mpressed&1); break;   case 5: fire=hov&&(mpressed&2); break;
          case 6: fire=hov&&(mpressed&4); break;   case 7: fire=hov&&(mreleased&1); break;
          case 8: fire=hov&&(mreleased&2); break;  case 9: fire=hov&&(mreleased&4); break;
          case 10: fire=hov&&!was; break;          case 11: fire=!hov&&was; break;
          case 50: fire=(mheld&1)!=0; break;       case 51: fire=(mheld&2)!=0; break;
          case 52: fire=(mheld&4)!=0; break;       case 53: fire=(mpressed&1)!=0; break;
          case 54: fire=(mpressed&2)!=0; break;    case 55: fire=(mpressed&4)!=0; break;
          case 56: fire=(mreleased&1)!=0; break;   case 57: fire=(mreleased&2)!=0; break;
          case 58: fire=(mreleased&4)!=0; break;
          case 60: fire=mwheel>0; break;           case 61: fire=mwheel<0; break;
          default: break;
        }
        if(fire) gml_run_event(vm,in,vm->mouse_events[e].suffix);
      }
    }
  }
  VMPROF_MARK(input);
  /* normal step */
  for(int i=0;i<n;i++) if(vm->inst[i].active && !vm->inst[i].marked) gml_run_event(vm,&vm->inst[i],"Step_0");
  VMPROF_MARK(step0);
  /* movement */
  for(int i=0;i<n;i++){ GmlInstance *in=&vm->inst[i]; if(!in->active||in->marked) continue;
    in->xprevious=in->x; in->yprevious=in->y;
    if(in->gravity!=0){ in->hspeed+=in->gravity*cos(in->gravity_direction*M_PI/180.0);
      in->vspeed-=in->gravity*sin(in->gravity_direction*M_PI/180.0); motion_from_components(in); }
    if(in->friction!=0 && in->speed!=0){
      if(in->friction>0 && in->speed<=in->friction) in->speed=0;
      else in->speed-=in->friction;
      motion_from_speed_dir(in);
    }
    in->x+=in->hspeed; in->y+=in->vspeed;
    if(in->hspeed!=0||in->vspeed!=0) gml_colgrid_touch(in); }
  /* path following (path_start): move instances along their assigned path each step */
  run_paths(vm);
  /* room/view boundary events (Outside Room/View, Intersect Boundary) — after move */
  run_boundary_events(vm);
  VMPROF_MARK(move);
  /* collision events (GM order: after move, before end step) */
  run_collisions(vm);
  VMPROF_MARK(coll);
  /* end step */
  for(int i=0;i<n;i++) if(vm->inst[i].active && !vm->inst[i].marked) gml_run_event(vm,&vm->inst[i],"Step_2");
  VMPROF_MARK(step2);
  reap(vm);
  { const char *iv=getenv("GML_LOG_INSTVAR");   /* obj_name:var1,var2 — dump instance vars per frame */
    if(iv && *iv){ char buf[256]; snprintf(buf,sizeof buf,"%s",iv);
      char *colon=strchr(buf,':');
      if(colon){ *colon=0; int oi=gml_object_index_by_name(vm,buf);
        GmlInstance *in = oi>=0 ? gml_find_instance(vm,oi) : NULL;
        if(in){ fprintf(stderr,"[ivar] f%ld %s id=%u",g_vm_frame,buf,in->id);
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
              if(have){ fprintf(stderr," %s=%.2f",tok,bv); continue; }
            }
            if(!p) fprintf(stderr," %s=<unset>",tok);
            else if(p->t==V_REAL) fprintf(stderr," %s=%.2f",tok,p->d);
            else if(p->t==V_STR) fprintf(stderr," %s=\"%s\"",tok,p->s?p->s:"");
            else if(p->t==V_ARR && p->arr && ((GmlArr*)p->arr)->is_2d){ GmlArr *A=p->arr;
              fprintf(stderr," %s=2D[h=%d]",tok,A->height2d);
              for(int r=0;r<A->height2d && r<6;r++){
                fprintf(stderr," r%d(",r);
                int rl = (A->row_len && r<A->row_cap)? A->row_len[r] : 0;
                for(int c2=0;c2<rl && c2<4;c2++){
                  long ix=(long)r*32000+c2;
                  if(ix<A->len){ GmlVal *e=&A->data[ix];
                    if(e->t==V_STR) fprintf(stderr,"\"%s\",",e->s?e->s:"");
                    else fprintf(stderr,"%.4g,",e->t==V_REAL?e->d:-1); } }
                fprintf(stderr,")"); } }
            else if(p->t==V_ARR && p->arr){ GmlArr *A=p->arr;
              int stp=A->len>24?A->len/16:1; if(stp<1)stp=1;
              fprintf(stderr," %s=[len=%d stp=%d:",tok,A->len,stp);
              for(int e=0;e<A->len;e+=stp)
                fprintf(stderr,"%s%.1f",e?",":"",A->data[e].t==V_REAL?A->data[e].d:-999);
              fprintf(stderr,"]"); }
            else fprintf(stderr," %s=<t%d>",tok,p->t); }
          fprintf(stderr,"\n"); } } } }
  { const char *gd=getenv("GML_DBG_GLOBDUMP");   /* dump the whole globals varmap once at frame N */
    if(gd && g_vm_frame==atol(gd)){
      fprintf(stderr,"[globdump] f%ld cap=%d:\n",g_vm_frame,vm->globals.cap);
      for(int i=0;i<vm->globals.cap;i++){ GmlVarSlot *s=&vm->globals.slots[i];
        if(!s->key) continue;
        if(s->val.t==V_REAL) fprintf(stderr,"  %s=%.2f\n",s->key,s->val.d);
        else fprintf(stderr,"  %s=<t%d>\n",s->key,s->val.t); } } }
  { const char *gv=getenv("GML_LOG_GLOBALVAR");   /* comma-separated global names, dumped per frame */
    if(gv && *gv){ char buf[256]; snprintf(buf,sizeof buf,"%s",gv);
      fprintf(stderr,"[gvar] f%ld",g_vm_frame);
      for(char *tok=strtok(buf,",");tok;tok=strtok(NULL,",")){
        GmlVal *p=gml_varmap_get(&vm->globals,tok);
        if(!p) fprintf(stderr," %s=<unset>",tok);
        else if(p->t==V_REAL) fprintf(stderr," %s=%.2f",tok,p->d);
        else if(p->t==V_STR) fprintf(stderr," %s=\"%s\"",tok,p->s?p->s:"");
        else fprintf(stderr," %s=<t%d>",tok,p->t); }
      fprintf(stderr,"\n"); } }
  { extern void gml_part_update_all(void); gml_part_update_all(); }   /* advance auto-update particle systems */
  /* Follow the selected instance within the view border bands, clamped to the room. */
  if(getenv("GML_LOG_FOLLOW")){ static int ff=0; if(ff++%60==0){
    int vo=(int)get_global_arr_d(vm,"view_object",0);
    fprintf(stderr,"[follow] vis=%.2f vobj=%d wv=%.0f xv=%.0f yv=%.0f\n",
      get_global_arr_d(vm,"view_visible",0),vo,
      get_global_arr_d(vm,"view_wview",0),
      get_global_arr_d(vm,"view_xview",0),get_global_arr_d(vm,"view_yview",0)); } }
  if(get_global_arr_d(vm,"view_visible",0)>=0.5){
    int vobj=(int)get_global_arr_d(vm,"view_object",0);
    GmlInstance *fo = vobj>=0 ? gml_find_instance(vm,vobj) : NULL;
    if(fo && getenv("GML_LOG_FOLLOW")){ static int f2=0; if(f2++%60==0)
      fprintf(stderr,"[follow-target] obj=%d(%s) id=%u at(%.0f,%.0f) hb=%.0f\n",
        fo->obj,(fo->obj>=0&&fo->obj<vm->n_objects)?vm->objects[fo->obj].name:"?",fo->id,fo->x,fo->y,
        get_global_arr_d(vm,"view_hborder",0)); }
    if(fo){
      double vx=get_global_arr_d(vm,"view_xview",0), vy=get_global_arr_d(vm,"view_yview",0);
      double wv=get_global_arr_d(vm,"view_wview",0), hv=get_global_arr_d(vm,"view_hview",0);
      double hb=get_global_arr_d(vm,"view_hborder",0), vb=get_global_arr_d(vm,"view_vborder",0);
      /* Center the target on axes whose border is at least half the view size. */
      if(2*hb >= wv)         vx = fo->x - wv/2;
      else {
        if(fo->x-vx < hb)    vx = fo->x - hb;
        if(fo->x-vx > wv-hb) vx = fo->x - (wv-hb);
      }
      if(2*vb >= hv)         vy = fo->y - hv/2;
      else {
        if(fo->y-vy < vb)    vy = fo->y - vb;
        if(fo->y-vy > hv-vb) vy = fo->y - (hv-vb);
      }
      GmlRoom rm; if(gml_room_get(vm->win,vm->room_index,&rm)==0){
        double mx=rm.width-wv, my=rm.height-hv;
        if(vx<0)vx=0; if(mx>0&&vx>mx)vx=mx; if(mx<=0)vx=0;
        if(vy<0)vy=0; if(my>0&&vy>my)vy=my; if(my<=0)vy=0;
      }
      set_global_arr(vm,"view_xview",0,vx); set_global_arr(vm,"view_yview",0,vy);
    }
  }
  /* room transition requested during the step */
  for(int i=0;i<8;i++){
    double hx=get_global_arr_d(vm,"background_hspeed",i), vy=get_global_arr_d(vm,"background_vspeed",i);
    if(hx!=0 || vy!=0){
      set_global_arr(vm,"background_x",i,get_global_arr_d(vm,"background_x",i)+hx);
      set_global_arr(vm,"background_y",i,get_global_arr_d(vm,"background_y",i)+vy);
    }
  }
  /* deferred async save/load completion events (Other_72) queued by buffer_*_async this step */
  gml_fire_async_saveload(vm);
  /* room transition requested during the step */
  if(vm->pending_room>=0){ int t=vm->pending_room; vm->pending_room=-1; vm->step_alloc_base=0; gml_room_enter(vm,t); }
  /* Game End (Other_3): fire on all active instances when game_end was set. */
  if(vm->game_end){
    for(int i=0;i<vm->inst_count;i++) if(vm->inst[i].active && !vm->inst[i].marked)
      gml_run_event(vm,&vm->inst[i],"Other_3");
  }
  vm->step_alloc_base=prev_alloc_base;
  if(pv){ VMPROF_MARK(rest);
    if(++g_vmprof.frames % 300 == 0){ double f=300.0;
      fprintf(stderr,"[vmprof] f=%ld avg_ms anim=%.3f step1=%.3f alarms=%.3f input=%.3f step0=%.3f move=%.3f coll=%.3f step2=%.3f rest=%.3f\n",
        g_vmprof.frames, g_vmprof.anim/f, g_vmprof.step1/f, g_vmprof.alarms/f, g_vmprof.input/f,
        g_vmprof.step0/f, g_vmprof.move/f, g_vmprof.coll/f, g_vmprof.step2/f, g_vmprof.rest/f);
      memset(&g_vmprof,0,sizeof g_vmprof); g_vmprof.frames=0; }
  }
}

/* ---- tile-layer runtime mutations (tile_layer_delete/depth/shift/hide/show) ----
 * GM tiles live in a tile layer keyed by depth. The room's tiles are read immutably from the
 * data.win each frame, so we keep a small list of per-depth mutations applied at gather time.
 * find-or-create the entry for `depth`. */
#define TILE_MUT_DELETED 1
#define TILE_MUT_HIDDEN  2
static int tile_mut_idx(GmlVM *vm, int depth){
  for(int i=0;i<vm->n_tile_mut;i++) if(vm->tile_mut[i].depth==depth) return i;
  if(vm->n_tile_mut>=64) return -1;
  int i=vm->n_tile_mut++; vm->tile_mut[i].depth=depth; vm->tile_mut[i].flags=0;
  vm->tile_mut[i].has_remap=0; vm->tile_mut[i].remap=0; vm->tile_mut[i].dx=vm->tile_mut[i].dy=0;
  return i;
}
void gml_tile_layer_delete(GmlVM *vm, int depth){ int i=tile_mut_idx(vm,depth); if(i>=0) vm->tile_mut[i].flags|=TILE_MUT_DELETED; }
void gml_tile_layer_depth(GmlVM *vm, int depth, int newdepth){ int i=tile_mut_idx(vm,depth);
  if(i>=0){ vm->tile_mut[i].has_remap=1; vm->tile_mut[i].remap=newdepth; } }
void gml_tile_layer_shift(GmlVM *vm, int depth, double dx, double dy){ int i=tile_mut_idx(vm,depth);
  if(i>=0){ vm->tile_mut[i].dx+=dx; vm->tile_mut[i].dy+=dy; } }
void gml_tile_layer_delete_at(GmlVM *vm, int depth, double x, double y){
  if(vm->n_tile_del_at<64){ int i=vm->n_tile_del_at++;
    vm->tile_del_at[i].depth=depth; vm->tile_del_at[i].x=(int)x; vm->tile_del_at[i].y=(int)y; } }
void gml_tile_layer_hide(GmlVM *vm, int depth, int hidden){ int i=tile_mut_idx(vm,depth);
  if(i>=0){ if(hidden) vm->tile_mut[i].flags|=TILE_MUT_HIDDEN; else vm->tile_mut[i].flags&=~TILE_MUT_HIDDEN; } }
/* apply the mutation for a tile at original `depth`: returns 0 to drop the tile (deleted/hidden),
 * else 1 and writes the effective depth + position offset. */
static int tile_apply_mut(GmlVM *vm, int depth, int *eff_depth, double *ox, double *oy){
  *eff_depth=depth; *ox=0; *oy=0;
  for(int i=0;i<vm->n_tile_mut;i++) if(vm->tile_mut[i].depth==depth){
    if(vm->tile_mut[i].flags&(TILE_MUT_DELETED|TILE_MUT_HIDDEN)) return 0;
    if(vm->tile_mut[i].has_remap) *eff_depth=vm->tile_mut[i].remap;
    *ox=vm->tile_mut[i].dx; *oy=vm->tile_mut[i].dy; return 1;
  }
  return 1;
}

/* Draw phase: GM draws instances and room tiles interleaved by depth (high
 * depth = behind). Merging them preserves layer ordering between tile layers,
 * scripted background instances, and gameplay instances. */
typedef struct { double x,y,xs,ys; int def,sx,sy,w,h; } GmlDrawTile;
static void draw_tile_add(GmlDrawTile **tiles, double **depth, int *nt, int *cap, GmlDrawTile t, double dep){
  if(*nt>=*cap){
    int nc=*cap?*cap*2:256;
    GmlDrawTile *n_tiles=malloc((size_t)nc*sizeof(**tiles));
    double *n_depth=malloc((size_t)nc*sizeof(**depth));
    if(!n_tiles || !n_depth){ free(n_tiles); free(n_depth); return; }
    if(*nt>0){ memcpy(n_tiles,*tiles,(size_t)*nt*sizeof(**tiles)); memcpy(n_depth,*depth,(size_t)*nt*sizeof(**depth)); }
    free(*tiles); free(*depth);
    *tiles=n_tiles; *depth=n_depth; *cap=nc;
  }
  (*tiles)[*nt]=t; (*depth)[*nt]=dep; (*nt)++;
}
typedef struct { double depth; int seq, type, idx; } GmlDrawItem;  /* type: 0=instance, 1=tile, 2=layer tile, 3=layer bg, 4=particle system */
static int cmp_draw_item(const void *pa, const void *pb){
  const GmlDrawItem *a=pa,*b=pb;
  if(a->depth!=b->depth) return a->depth>b->depth? -1:1;     /* higher depth first (behind) */
  /* At equal depth, order room tiles above instances and later room tiles above
   * earlier tiles. Draw newer instances before older ones. Runtime layer items
   * retain their sequence ordering. */
  int at1=a->type==1, bt1=b->type==1;
  if(at1!=bt1) return at1? 1 : -1;                           /* room tile sorts later (front) */
  if(at1 && bt1) return a->seq<b->seq? -1 : (a->seq>b->seq?1:0);   /* tiles: list order, later on top */
  return a->seq>b->seq? -1 : (a->seq<b->seq?1:0);
}
static int rt_layer_has_background(GmlVM *vm, int layer_id){
  for(int i=0;i<vm->n_rte;i++){
    GmlRtElem *e=&vm->rte[i];
    if(e->used && e->type==1 && e->layer==layer_id) return 1;
  }
  return 0;
}
void gml_vm_draw(GmlVM *vm){
  GmlRender *R=(GmlRender*)vm->render; if(!R) return;
  int n=vm->inst_count;
  /* gather this room's tiles (pointer-list of records: x,y,def,srcx,srcy,w,h,depth,...) */
  GmlDrawTile *tiles=NULL; int nt=0, tcap=0; double *tdepth=NULL;
  GmlRoom rm;
  if(gml_room_get(vm->win,vm->room_index,&rm)==0 && rm.tile_ptr){
    const uint8_t *d=vm->win->data; uint32_t tc=u32(d,rm.tile_ptr);
    if(tc>0 && tc<100000){
      for(uint32_t i=0;i<tc;i++){ uint32_t p=u32(d,rm.tile_ptr+4+i*4);
	int tx=(int)u32(d,p), ty=(int)u32(d,p+4); int tdep=(int32_t)u32(d,p+28);
	if(vm->n_tile_mut){ int ed; double ox,oy;
          if(!tile_apply_mut(vm,tdep,&ed,&ox,&oy)) continue;   /* deleted layer: drop */
          tdep=ed; tx+=(int)ox; ty+=(int)oy; }
        { int drop=0;
          for(int da=0;da<vm->n_tile_del_at;da++)
            if(vm->tile_del_at[da].depth==tdep && vm->tile_del_at[da].x==tx && vm->tile_del_at[da].y==ty){ drop=1; break; }
          if(drop) continue; }
	GmlDrawTile dt;
	dt.x=tx; dt.y=ty; dt.xs=1; dt.ys=1; dt.def=(int)u32(d,p+8);
	dt.sx=(int)u32(d,p+12); dt.sy=(int)u32(d,p+16);
	dt.w=(int)u32(d,p+20); dt.h=(int)u32(d,p+24);
	draw_tile_add(&tiles,&tdepth,&nt,&tcap,dt,tdep); } }
  }
  /* ROOM layer records carry background, instance and asset-tile layers.
   * Use the detected type-data offset, including optional effect fields. */
  struct LayBg { int sprite; int th,tv,stretch; double x,y; uint32_t blend; double alpha; double depth; };
  struct LayTile { int sprite; int sx,sy,w,h; double x,y,xs,ys; uint32_t blend; double alpha; double depth; };
  struct LayBg *lbg=NULL; int nlb=0;
  struct LayTile *ltl=NULL; int nlt=0;
  if(vm->win->bytecode>=17){
    const GmlChunk *rc=gml_chunk(vm->win,"ROOM");
    const uint8_t *d=vm->win->data;
    uint32_t rp = rc ? u32(d,rc->off+4+vm->room_index*4) : 0;
    uint32_t lay = (rp && rp+92<vm->win->size) ? u32(d,rp+88) : 0;
    uint32_t lcnt = (lay && lay+4<vm->win->size) ? u32(d,lay) : 0;
    if(lcnt>0 && lcnt<512){
      int doff=gml_room_layer_data_off(vm);
      long fin = g_vm_frame - vm->room_enter_frame; if(fin<0) fin=0;
      for(uint32_t i=0;i<lcnt;i++){ uint32_t lp=u32(d,lay+4+i*4);
        if(!lp || lp+(uint32_t)doff+40>vm->win->size) continue;
        uint32_t ltype=u32(d,lp+8); double ldep=(double)(int32_t)u32(d,lp+12);
        double lx=f32(d,lp+16), ly=f32(d,lp+20), lhs=f32(d,lp+24), lvs=f32(d,lp+28);
        /* Runtime layer control: if the game moved this layer (layer_x/layer_hspeed by name), use its
         * live accumulated position; otherwise the exact room-def scroll model (byte-identical for
         * games that never script their layers). */
        uint32_t lnp=u32(d,lp+0);
        GmlRtLayer *rl=(lnp && lnp<vm->win->size)? gml_rt_layer_find_by_name(vm,(const char*)(d+lnp)):NULL;
        if(rl) ldep=rl->depth;
        int ltouch = rl && rl->touched;
        double lox = ltouch ? rl->x : lx+lhs*fin;   /* background layer origin (scrolls) */
        double loy = ltouch ? rl->y : ly+lvs*fin;
        double ltx = ltouch ? rl->x : lx;           /* tile layer origin (room-def raw, unchanged) */
        double lty = ltouch ? rl->y : ly;
        if(rl ? !rl->visible : !u32(d,lp+32)) continue;
        if(ltype==1){
          if(rl && rt_layer_has_background(vm,rl->id)) continue;
          uint32_t b=lp+doff;
          if(!u32(d,b)) continue;     /* background not visible */
          int spr=(int32_t)u32(d,b+8);
          if(spr<0) continue;
          lbg=realloc(lbg,(nlb+1)*sizeof(*lbg));
          lbg[nlb].sprite=spr; lbg[nlb].th=(int)u32(d,b+12); lbg[nlb].tv=(int)u32(d,b+16);
          lbg[nlb].stretch=(int)u32(d,b+20);
          uint32_t col=u32(d,b+24);
          lbg[nlb].blend=col&0xFFFFFF; lbg[nlb].alpha=((col>>24)&0xFF)/255.0;
          lbg[nlb].x=lox; lbg[nlb].y=loy; lbg[nlb].depth=ldep;
          nlb++;
        } else if(ltype==3){
          uint32_t tl=u32(d,lp+doff);
          uint32_t tcnt=(tl && tl+4<vm->win->size)?u32(d,tl):0;
          if(tcnt==0 || tcnt>100000) continue;
          for(uint32_t k2=0;k2<tcnt;k2++){ uint32_t tp=u32(d,tl+4+k2*4);
            if(!tp || tp+48>vm->win->size) continue;
            ltl=realloc(ltl,(nlt+1)*sizeof(*ltl));
            ltl[nlt].x=ltx+(int32_t)u32(d,tp); ltl[nlt].y=lty+(int32_t)u32(d,tp+4);
            ltl[nlt].sprite=(int32_t)u32(d,tp+8);
            ltl[nlt].sx=(int32_t)u32(d,tp+12); ltl[nlt].sy=(int32_t)u32(d,tp+16);
            ltl[nlt].w=(int32_t)u32(d,tp+20); ltl[nlt].h=(int32_t)u32(d,tp+24);
            ltl[nlt].depth=ldep;
            ltl[nlt].xs=f32(d,tp+36); ltl[nlt].ys=f32(d,tp+40);
            uint32_t col=u32(d,tp+44);
            ltl[nlt].blend=col&0xFFFFFF; ltl[nlt].alpha=((col>>24)&0xFF)/255.0;
            nlt++; }
        }
      }
    }
  }
  /* GMS2 tilemap layers (type 4): the same grid used by tilemap_get_* for collision is also a
   * visual layer. Without drawing these, games still collide with floors but the floors are
   * invisible. Expand only the camera-visible cells into the existing background-tile draw path. */
  for(int mi=0; mi<vm->n_tilemaps; mi++){
    GmlTileMap *tm=&vm->tilemaps[mi];
    double tmx,tmy,tmdepth; int tmvis;
    gml_tilemap_effective(vm,tm,&tmx,&tmy,&tmdepth,&tmvis);
    if(!tm->used || !tmvis || !tm->tiles || tm->tileset<0 || tm->tw<=0 || tm->th<=0) continue;
    if(tm->tileset>=R->n_bg) continue;
    int bti=R->bg[tm->tileset].tpag;
    if(bti<0 || bti>=R->n_tpag) continue;
    GmlTpag *bt=&R->tpag[bti];
    GmlBg *gb=&R->bg[tm->tileset];
    int tw=gb->tile_w>0?gb->tile_w:tm->tw, th=gb->tile_h>0?gb->tile_h:tm->th;
    int bx=gb->tile_border_x, by=gb->tile_border_y;
    int pitch_x=tw+2*bx, pitch_y=th+2*by;
    int srcw=bt->bw?bt->bw:bt->sw, srch=bt->bh?bt->bh:bt->sh;
    int per_row=gb->tile_columns>0?gb->tile_columns:(pitch_x>0?srcw/pitch_x:0);
    if(srcw<=0 || srch<=0 || tw<=0 || th<=0 || pitch_x<=0 || pitch_y<=0 || per_row<=0) continue;
    int cx0=(int)floor((R->cam_x - tmx) / tm->tw) - 1;
    int cy0=(int)floor((R->cam_y - tmy) / tm->th) - 1;
    int cx1=(int)ceil((R->cam_x + R->fbw - tmx) / tm->tw) + 1;
    int cy1=(int)ceil((R->cam_y + R->fbh - tmy) / tm->th) + 1;
    if(cx0<0) cx0=0; if(cy0<0) cy0=0;
    if(cx1>tm->cols) cx1=tm->cols; if(cy1>tm->rows) cy1=tm->rows;
    for(int cy=cy0; cy<cy1; cy++) for(int cx=cx0; cx<cx1; cx++){
      uint32_t datum=u32(tm->tiles,(uint32_t)((size_t)cy*tm->cols+cx)*4);
      int idx=(int)(datum & 0x7FFFFu);
      if(idx<=0) continue;                  /* GM encodes 0 as empty */
      int src_idx=idx;
      if(gb->tile_ids && gb->tile_items_per_tile>0 && idx<gb->tile_count){
        const uint8_t *idp=gb->tile_ids+(size_t)idx*(size_t)gb->tile_items_per_tile*4u;
        src_idx=(int)((uint32_t)idp[0]|((uint32_t)idp[1]<<8)|((uint32_t)idp[2]<<16)|((uint32_t)idp[3]<<24));
      } else if(!gb->tile_ids) src_idx=idx-1;
      if(src_idx<0) continue;
      int sx=(src_idx%per_row)*pitch_x + bx, sy=(src_idx/per_row)*pitch_y + by;
      if(sx>=srcw || sy>=srch) continue;
      int w=tw, h=th;
      if(sx+w>srcw) w=srcw-sx;
      if(sy+h>srch) h=srch-sy;
      if(w<=0 || h<=0) continue;
      GmlDrawTile dt;
      dt.x=tmx + cx*tm->tw; dt.y=tmy + cy*tm->th; dt.xs=1; dt.ys=1;
      if((datum>>28)&1){ dt.x += tm->tw; dt.xs=-1; }
      if((datum>>29)&1){ dt.y += tm->th; dt.ys=-1; }
      dt.def=tm->tileset; dt.sx=sx; dt.sy=sy; dt.w=w; dt.h=h;
      draw_tile_add(&tiles,&tdepth,&nt,&tcap,dt,tmdepth);
    }
  }
  /* runtime layer elements (layer_tile_create / layer_background_create): converted GM8 games
   * paint terrain through the tile_add compat script, which lands here. Reuse the LayTile/LayBg
   * records so the unified draw list needs no new item types. */
  for(int i=0;i<vm->n_rte;i++){
    GmlRtElem *e=&vm->rte[i];
    if(!e->used || !e->visible || e->sprite<0) continue;
    GmlRtLayer *l=gml_rt_layer_find(vm,e->layer);
    if(!l || !l->visible) continue;
    long fin2 = g_vm_frame - vm->room_enter_frame; if(fin2<0) fin2=0;
    double lx = l->touched ? l->x : l->x+l->hs*fin2;
    double ly = l->touched ? l->y : l->y+l->vs*fin2;
    if(e->type==7){
      ltl=realloc(ltl,(nlt+1)*sizeof(*ltl));
      ltl[nlt].sprite=e->sprite; ltl[nlt].sx=e->sx; ltl[nlt].sy=e->sy; ltl[nlt].w=e->w; ltl[nlt].h=e->h;
      ltl[nlt].x=lx+e->x; ltl[nlt].y=ly+e->y; ltl[nlt].xs=e->xs; ltl[nlt].ys=e->ys;
      ltl[nlt].blend=e->blend; ltl[nlt].alpha=e->alpha; ltl[nlt].depth=l->depth;
      nlt++;
    } else if(e->type==1){
      lbg=realloc(lbg,(nlb+1)*sizeof(*lbg));
      lbg[nlb].sprite=e->sprite; lbg[nlb].th=e->htiled; lbg[nlb].tv=e->vtiled; lbg[nlb].stretch=e->stretch;
      lbg[nlb].x=lx; lbg[nlb].y=ly; lbg[nlb].blend=e->blend; lbg[nlb].alpha=e->alpha; lbg[nlb].depth=l->depth;
      nlb++;
    }
  }
  /* unified depth-sorted draw list of instances + tiles + GMS2 layers + auto-draw particle systems */
  int npart=0; while(gml_part_system_auto_draw_nth(npart,NULL,NULL)) npart++;
  int cap=n+nt+nlb+nlt+npart; GmlDrawItem *it=malloc((cap>0?cap:1)*sizeof(GmlDrawItem)); int m=0;
  for(int i=0;i<n;i++) if(vm->inst[i].active && !vm->inst[i].marked){
    it[m].depth=vm->inst[i].depth; it[m].type=0; it[m].idx=i; it[m].seq=m; m++; }
  for(int i=0;i<nt;i++){ it[m].depth=tdepth[i]; it[m].type=1; it[m].idx=i; it[m].seq=m; m++; }
  for(int i=0;i<nlt;i++){ it[m].depth=ltl[i].depth; it[m].type=2; it[m].idx=i; it[m].seq=m; m++; }
  for(int i=0;i<nlb;i++){ it[m].depth=lbg[i].depth; it[m].type=3; it[m].idx=i; it[m].seq=m; m++; }
  for(int i=0;i<npart;i++){ int pid=0; double dep=0;
    if(gml_part_system_auto_draw_nth(i,&pid,&dep)){ it[m].depth=dep; it[m].type=4; it[m].idx=pid; it[m].seq=m; m++; } }
  qsort(it,m,sizeof(GmlDrawItem),cmp_draw_item);
  static int dumped=0;
  { const char *li=getenv("GML_LOG_INST");
    if(li && atoi(li)>0 && !dumped && g_vm_frame<atoi(li)) goto skip_instdump; }
  if(getenv("GML_LOG_INST") && !dumped){ dumped=1;
    fprintf(stderr,"[draw] room=%d, %d instances + %d tiles (back->front):\n",vm->room_index,n,nt);
    for(int k=0;k<m && k<2000;k++){ if(it[k].type==1){ GmlDrawTile *t=&tiles[it[k].idx];
        fprintf(stderr,"   TILE def=%d depth=%.0f @(%.0f,%.0f) %dx%d src=(%d,%d)\n",t->def,it[k].depth,t->x,t->y,t->w,t->h,t->sx,t->sy); }
      else if(it[k].type==2){ struct LayTile *t=&ltl[it[k].idx];
        fprintf(stderr,"   LTILE spr=%d depth=%.0f @(%.0f,%.0f) %dx%d\n",t->sprite,it[k].depth,t->x,t->y,t->w,t->h); }
      else if(it[k].type==3){ struct LayBg *b=&lbg[it[k].idx];
        fprintf(stderr,"   LBG spr=%d depth=%.0f @(%.0f,%.0f)\n",b->sprite,it[k].depth,b->x,b->y); }
      else if(it[k].type==4){
        fprintf(stderr,"   PARTICLES sys=%d depth=%.0f\n",it[k].idx,it[k].depth); }
      else { GmlInstance *in=&vm->inst[it[k].idx];
        fprintf(stderr,"   %-26s spr=%-4d vis=%.0f depth=%.0f @(%.0f,%.0f) ang=%.0f xs=%.1f ys=%.1f a=%.2f\n",
          (in->obj>=0&&in->obj<vm->n_objects)?vm->objects[in->obj].name:"?",
          (int)in->sprite_index,in->visible,in->depth,in->x,in->y,in->image_angle,in->image_xscale,in->image_yscale,in->image_alpha); } } }
  skip_instdump:
  for(int k=0;k<m;k++){
    if(it[k].type==1){ GmlDrawTile *t=&tiles[it[k].idx];
      gml_draw_background_part_ext(R,t->def,t->sx,t->sy,t->w,t->h,t->x,t->y,t->xs,t->ys,0xFFFFFF,1); continue; }
    if(it[k].type==2){ struct LayTile *t=&ltl[it[k].idx];
      gml_draw_sprite_part_ext(R,t->sprite,0,t->sx,t->sy,t->w,t->h,t->x,t->y,t->xs,t->ys,t->blend,t->alpha);
      continue; }
    if(it[k].type==3){ struct LayBg *b=&lbg[it[k].idx];
      if(b->th || b->tv) gml_draw_sprite_tiled_ext(R,b->sprite,0,b->x,b->y,1,1,b->blend,b->alpha);
      else gml_draw_sprite_ext(R,b->sprite,0,b->x,b->y,1,1,0,b->blend,b->alpha);
      continue; }
    if(it[k].type==4){ gml_part_system_drawit(R,it[k].idx); continue; }
    GmlInstance *in=&vm->inst[it[k].idx];
    if(vm->draw_events_off) continue;   /* draw_enable_drawevent(false): no instance drawing */
    { const char *sk=getenv("GML_SKIP");                  /* debug: skip drawing a named object */
      if(sk && in->obj>=0 && in->obj<vm->n_objects && vm->objects[in->obj].name
         && strstr(vm->objects[in->obj].name,sk)) continue; }
    if(in->visible<0.5) continue;   /* GM: an invisible instance runs neither
                                       its Draw event nor the automatic sprite draw. */
    /* Draw event replaces default draw; else draw sprite_index automatically. */
    if(gml_run_event(vm,in,"Draw_0")) continue;
    if(in->sprite_index>=0){
      double alpha=in->image_alpha;
      if(R && R->alpha<alpha) alpha=R->alpha;
      gml_draw_sprite_ext(R,(int)in->sprite_index,(int)in->image_index,in->x,in->y,
                          in->image_xscale,in->image_yscale,in->image_angle,
                          (uint32_t)in->image_blend,alpha);
    }
  }
  free(it); free(tiles); free(tdepth); free(lbg); free(ltl);
}

/* Dispatch Draw_64 events in depth order. Set view_current to 7 when views
 * are enabled and to 0 otherwise for this pass. */
/* run one draw-stage event pass (Draw_72/73 begin-end, Draw_74/75 pre-post, Draw_65/66 GUI
 * begin-end) over all instances in depth order. Cheap no-op when no object has the event. */
void gml_vm_draw_pass(GmlVM *vm, const char *suffix){
  GmlRender *R=(GmlRender*)vm->render; if(!R) return;
  int n=vm->inst_count;
  int *ord=malloc((n>0?n:1)*sizeof(int)); int m=0;
  for(int i=0;i<n;i++){ GmlInstance *in=&vm->inst[i];
    if(!in->active||in->marked||in->visible<0.5) continue;
    if(event_lookup_from(vm,suffix,in->obj,NULL,NULL)) ord[m++]=i;
  }
  for(int a=0;a<m;a++) for(int b=a+1;b<m;b++)
    if(vm->inst[ord[b]].depth>vm->inst[ord[a]].depth){ int t=ord[a]; ord[a]=ord[b]; ord[b]=t; }
  for(int k=0;k<m;k++) gml_run_event(vm,&vm->inst[ord[k]],suffix);
  free(ord);
}
void gml_vm_draw_gui(GmlVM *vm){
  GmlRender *R=(GmlRender*)vm->render; if(!R) return;
  int n=vm->inst_count;
  int *ord=malloc((n>0?n:1)*sizeof(int)); int m=0;
  for(int i=0;i<n;i++){ GmlInstance *in=&vm->inst[i];
    if(!in->active||in->marked||in->visible<0.5) continue;
    if(event_lookup_from(vm,"Draw_64",in->obj,NULL,NULL)) ord[m++]=i;
  }
  for(int a=0;a<m;a++) for(int b=a+1;b<m;b++)            /* depth descending (back -> front) */
    if(vm->inst[ord[b]].depth>vm->inst[ord[a]].depth){ int t=ord[a]; ord[a]=ord[b]; ord[b]=t; }
  int views_on=0;
  for(int v=0;v<8;v++) if(get_global_arr_d(vm,"view_visible",v)>=0.5){ views_on=1; break; }
  *gml_varmap_put(&vm->globals,"view_current")=vreal(views_on?7:0);
  for(int k=0;k<m;k++) gml_run_event(vm,&vm->inst[ord[k]],"Draw_64");
  *gml_varmap_put(&vm->globals,"view_current")=vreal(0);
  free(ord);
}

/* ---- collision events (Collision_<targetobj> handlers) ---- */
static int inst_mask_sprite_index(GmlInstance *in){
  return in->mask_index>=0 ? (int)in->mask_index : (int)in->sprite_index;
}
static int vm_bbox_at(GmlVM *vm, GmlInstance *in, double atx, double aty,
                      double *l, double *t, double *r, double *b){
  GmlRender *R=(GmlRender*)vm->render; if(!R) return 0;
  int si=inst_mask_sprite_index(in); if(si<0||si>=R->n_spr) return 0;
  GmlSprite *s=&R->spr[si]; if(s->mr<s->ml || s->mb<s->mt) return 0;
  double xs=in->image_xscale, ys=in->image_yscale;
  if(fabs(xs)<1e-9 || fabs(ys)<1e-9) return 0;
  double ang=in->image_angle*M_PI/180.0, c=cos(ang), sn=sin(ang);
  double minx=1e30,miny=1e30,maxx=-1e30,maxy=-1e30;
  double x0=s->ml, x1=s->mr+1.0, y0=s->mt, y1=s->mb+1.0;
  double corners[4][2]={{x0,y0},{x1,y0},{x0,y1},{x1,y1}};
  for(int i=0;i<4;i++){
    double px=(corners[i][0]-s->originx)*xs, py=(corners[i][1]-s->originy)*ys;
    double wx=atx + px*c + py*sn;
    double wy=aty - px*sn + py*c;
    if(wx<minx) minx=wx;
    if(wx>maxx) maxx=wx;
    if(wy<miny) miny=wy;
    if(wy>maxy) maxy=wy;
  }
  *l=floor(minx); *t=floor(miny); *r=ceil(maxx)-1.0; *b=ceil(maxy)-1.0; return 1;
}
static int vm_bbox(GmlVM *vm, GmlInstance *in, double *l, double *t, double *r, double *b){
  return vm_bbox_at(vm,in,in->x,in->y,l,t,r,b);
}
static int vm_overlap(double l1,double t1,double r1,double b1,double l2,double t2,double r2,double b2){
  return l1<=r2 && l2<=r1 && t1<=b2 && t2<=b1;
}
static int vm_mask_hit_world(GmlRender *R, GmlInstance *in, GmlSprite *s, int sprite, int wx, int wy){
  double xs=in->image_xscale, ys=in->image_yscale;
  if(fabs(xs)<1e-9 || fabs(ys)<1e-9) return 0;
  double ang=in->image_angle*M_PI/180.0, c=cos(ang), sn=sin(ang);
  double rx=(double)wx-in->x, ry=(double)wy-in->y;
  double sxr=rx*c - ry*sn, syr=rx*sn + ry*c;
  int lx=(int)floor(sxr/xs + s->originx);
  int ly=(int)floor(syr/ys + s->originy);
  return gml_sprite_collision(R,sprite,(int)in->image_index,lx,ly);
}
static int vm_masks_overlap(GmlVM *vm, GmlInstance *a, GmlInstance *b,
                            double l1,double t1,double r1,double b1,
                            double l2,double t2,double r2,double b2){
  GmlRender *R=(GmlRender*)vm->render; if(!R) return 1;
  int as=inst_mask_sprite_index(a), bs=inst_mask_sprite_index(b); if(as<0||bs<0) return 1;
  if(as>=R->n_spr||bs>=R->n_spr) return 1;
  GmlSprite *ap=&R->spr[as], *bp=&R->spr[bs];
  int x0=(int)floor(fmax(l1,l2)), x1=(int)ceil(fmin(r1,r2)+1.0);
  int y0=(int)floor(fmax(t1,t2)), y1=(int)ceil(fmin(b1,b2)+1.0);
  if(x1==x0) x1++;
  if(y1==y0) y1++;
  for(int wy=y0; wy<y1; wy++) for(int wx=x0; wx<x1; wx++){
    if(!vm_mask_hit_world(R,a,ap,as,wx,wy)) continue;
    if( vm_mask_hit_world(R,b,bp,bs,wx,wy)) return 1;
  }
  return 0;
}
/* Collect unique Keyboard_N, KeyPress_N and KeyRelease_N suffixes from CODE names. */
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
}
/* Collect instance mouse-event subtypes from CODE names for pointer-state dispatch. */
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
    /* Resolve collision subtype suffixes as numeric object indices or object names. */
    const char *tgt=cp+11; int target;
    if(tgt[0]>='0' && tgt[0]<='9') target=atoi(tgt);
    else target=gml_object_index_by_name(vm,tgt);
    if(target<0) continue;
    vm->col_events[vm->n_col_events++]=(GmlColEvent){selfobj, target, i};
  }
  /* Mark objects with Collision handlers, including inherited handlers, for the outer collision filter. */
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
static struct { int obj; long frame, gen; int *slots; int n, cap; } g_colcand[48];
static int colcand_get(GmlVM *vm, int obj, int **out){
  extern long g_vm_frame;
  if(!vm->obj_head || gml_colgrid_mode()==0) return -1;
  int slot=-1;
  for(int k=0;k<48;k++){ if(g_colcand[k].obj==obj){ slot=k; break; } if(slot<0 && g_colcand[k].obj==0 && g_colcand[k].frame==0) slot=k; }
  if(slot<0) slot=obj%48;
  if(g_colcand[slot].obj==obj && g_colcand[slot].frame==g_vm_frame && g_colcand[slot].gen==vm->obj_list_gen){
    *out=g_colcand[slot].slots; return g_colcand[slot].n; }
  /* target roots of this type's chain */
  int roots[16]; int nr=0;
  for(int p=obj; p>=0 && p<vm->n_objects && nr<16; p=vm->objects[p].parent)
    for(int e=0;e<vm->n_col_events && nr<16;e++) if(vm->col_events[e].self_obj==p){
      int t=vm->col_events[e].target_obj, dup=0;
      for(int r=0;r<nr;r++) if(roots[r]==t){ dup=1; break; }
      if(!dup) roots[nr++]=t; }
  static uint64_t *bits=NULL; static int bw=0;
  int words=(vm->inst_count+63)/64;
  if(words>bw){ uint64_t *pp=realloc(bits,(size_t)(words?words:1)*8); if(!pp) return -1; bits=pp; bw=words; }
  memset(bits,0,(size_t)words*8);
  for(int r=0;r<nr;r++) for(int d=0;d<vm->n_objects;d++){
    if(!gml_object_is(vm,d,roots[r])) continue;
    for(int i=vm->obj_head[d]; i>=0; i=vm->inst_next[i])
      if(i<vm->inst_count) bits[i>>6]|=1ull<<(i&63);
  }
  int n=0;
  for(int w=0;w<words;w++){ uint64_t m=bits[w];
    while(m){ int b=__builtin_ctzll(m); m&=m-1; int i=(w<<6)|b;
      if(n>=g_colcand[slot].cap){ int nc=g_colcand[slot].cap? g_colcand[slot].cap*2:64;
        int *pp=realloc(g_colcand[slot].slots,nc*sizeof(int)); if(!pp) return -1;
        g_colcand[slot].slots=pp; g_colcand[slot].cap=nc; }
      g_colcand[slot].slots[n++]=i; } }
  g_colcand[slot].obj=obj; g_colcand[slot].frame=g_vm_frame; g_colcand[slot].gen=vm->obj_list_gen; g_colcand[slot].n=n;
  { static int dbg=-1; if(dbg<0) dbg=getenv("GML_DBG_COLCAND")!=NULL;
    if(dbg){ static long last=0; if(g_vm_frame!=last || 1){ last=g_vm_frame;
      fprintf(stderr,"[colcand] f%ld obj=%d(%s) targets=%d n=%d\n",g_vm_frame,obj,
        (obj>=0&&obj<vm->n_objects)?vm->objects[obj].name:"?",nr,n); } } }
  *out=g_colcand[slot].slots;
  return n;
}
static void run_collisions(GmlVM *vm){
  if(!vm->render) return;
  const char *clog=getenv("GML_LOG_COLLISION");   /* hoisted: this ran PER PAIR (1.3M getenv/frame) */
  int cmode=gml_colgrid_mode();
  for(int i=0;i<vm->inst_count;i++){ GmlInstance *si=&vm->inst[i];
    if(!si->active||si->marked||si->obj<0||si->obj>=vm->n_objects) continue;
    if(!vm->objects[si->obj].colself) continue;   /* no Collision_* handler anywhere in its chain */
    double l1,t1,r1,b1; if(!vm_bbox(vm,si,&l1,&t1,&r1,&b1)) continue;
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
      int handler_obj=-1, target_obj=-1, code=-1;
      if(!col_event_for_pair(vm,si->obj,oi->obj,&handler_obj,&target_obj,&code)) continue;
      if(cmode==2 && vm->obj_head){   /* verify: a firing-capable pair must be in the candidate set */
        int *vc=NULL; int vn=colcand_get(vm,si->obj,&vc), found=0;
        for(int q=0;q<vn;q++) if(vc[q]==j){ found=1; break; }
        if(vn>=0 && !found){ extern long g_vm_frame;
          fprintf(stderr,"[gridcheck] MISMATCH colcand f%ld si_obj=%d oi_obj=%d slot=%d\n",g_vm_frame,si->obj,oi->obj,j); } }
      double l2,t2,r2,b2; if(!vm_bbox(vm,oi,&l2,&t2,&r2,&b2)) continue;
      int bbox_hit=vm_overlap(l1,t1,r1,b1,l2,t2,r2,b2);
      int mask_hit=bbox_hit ? vm_masks_overlap(vm,si,oi,l1,t1,r1,b1,l2,t2,r2,b2) : 0;
      if(clog){
        const char *sn=(si->obj>=0&&si->obj<vm->n_objects)?vm->objects[si->obj].name:"?";
        const char *on=(oi->obj>=0&&oi->obj<vm->n_objects)?vm->objects[oi->obj].name:"?";
        if(!*clog || strstr(sn,clog) || strstr(on,clog))
          fprintf(stderr,"[collision?] f%ld %s id=%u bbox=(%.0f,%.0f,%.0f,%.0f) vs %s id=%u bbox=(%.0f,%.0f,%.0f,%.0f) mask=%d event=Collision_%d\n",
            g_vm_frame,sn,si->id,l1,t1,r1,b1,on,oi->id,l2,t2,r2,b2,mask_hit,target_obj);
      }
      if(bbox_hit && mask_hit){
        if(clog){
          const char *sn=(si->obj>=0&&si->obj<vm->n_objects)?vm->objects[si->obj].name:"?";
          const char *on=(oi->obj>=0&&oi->obj<vm->n_objects)?vm->objects[oi->obj].name:"?";
          if(!*clog || strstr(sn,clog) || strstr(on,clog))
            fprintf(stderr,"[collision] f%ld %s id=%u -> %s id=%u event=Collision_%d\n",
              g_vm_frame,sn,si->id,on,oi->id,target_obj);
        }
        char suffix[32]; snprintf(suffix,sizeof suffix,"Collision_%d",target_obj);
        { static int evt=-1; if(evt<0) evt=getenv("GML_DBG_EVTIME")!=NULL;
          if(evt){ double t0=vmprof_now();
            run_event_code_from(vm,si,oi,suffix,handler_obj,code);
            double dt=vmprof_now()-t0;
            if(dt>0.5) fprintf(stderr,"[evtime] SLOW COLLISION f%ld %s vs %s: %.2fms\n",g_vm_frame,
              (si->obj>=0&&si->obj<vm->n_objects)?vm->objects[si->obj].name:"?",
              (oi->obj>=0&&oi->obj<vm->n_objects)?vm->objects[oi->obj].name:"?",dt);
          } else run_event_code_from(vm,si,oi,suffix,handler_obj,code); }
        if(!si->active||si->marked) break;   /* self destroyed by the event */
        if(!vm_bbox(vm,si,&l1,&t1,&r1,&b1)) break;
        /* the event may have mutated the world (and the shared candidate cache): finish this
         * si with the plain linear scan from the next slot — identical event sequence */
        if(jlin<0 && fcn>=0){ jlin=j; fcn=-1; }
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

/* WELL512 follows the Lomont recurrence credited in LICENSES/WELL512.txt.
 * Expand seeds using MSVC-LCG arithmetic; random(x) scales the next value by x/2^32. */
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
  vm->cg_built_frame=-1;   /* memset leaves 0, which would collide with g_vm_frame==0 at boot */
  { extern void gml_part_reset_all(void); gml_part_reset_all(); }   /* fresh particle pools per game */
  vm->win=win; vm->pending_room=-1; vm->room_index=-1; vm->next_id=100000; vm->rng_state=0;
  vm->next_buffer_id=1;
  vm->next_ds_id=1;
  vm->ds_list_compat_repair=0;
  gml_rng_seed(vm,0);   /* GM default seed */
  parse_objects(vm);
  parse_paths(vm);
  parse_boundary_events(vm);
  free(vm->obj_alive); vm->obj_alive=calloc(vm->n_objects?vm->n_objects:1,sizeof(int));
  free(vm->obj_head); vm->obj_head=malloc((vm->n_objects?vm->n_objects:1)*sizeof(int));
  if(vm->obj_head) for(int o=0;o<vm->n_objects;o++) vm->obj_head[o]=-1;
  free(vm->inst_next); free(vm->inst_prev);
  { int pc=vm->inst_cap>0?vm->inst_cap:16384;
    vm->inst_next=malloc(pc*sizeof(int)); vm->inst_prev=malloc(pc*sizeof(int));
    if(vm->inst_next&&vm->inst_prev) for(int i=0;i<pc;i++){ vm->inst_next[i]=vm->inst_prev[i]=-1; }
    else { free(vm->obj_head); vm->obj_head=NULL; } }
  parse_col_events(vm);
  parse_key_events(vm);
  parse_mouse_events(vm);
  vm->col_pair_cache_cap=16384;
  vm->col_pair_cache=calloc((size_t)vm->col_pair_cache_cap,sizeof(GmlColPairCache));
  vm->event_cache_cap=32768;
  vm->event_cache=calloc((size_t)vm->event_cache_cap,sizeof(GmlEventCache));
  vm->inst_cap=16384; vm->inst=calloc(vm->inst_cap,sizeof(GmlInstance)); /* fixed pool: never realloc-move */
  return 0;
}
static void ini_free(GmlVM *vm){
  for(int i=0;i<vm->ini_n;i++){ free(vm->ini_kv[i].section); free(vm->ini_kv[i].key); free(vm->ini_kv[i].sval); }
  memset(vm->ini_kv,0,sizeof(vm->ini_kv)); vm->ini_n=0; vm->ini_open=0; vm->ini_path[0]=0;
}
static void io_free(GmlVM *vm){
  for(int i=0;i<16;i++){
    if(vm->bin_file[i]){ fclose((FILE*)vm->bin_file[i]); vm->bin_file[i]=NULL; }
    free(vm->buffer[i].data);
    memset(&vm->buffer[i],0,sizeof(vm->buffer[i]));
  }
  vm->next_buffer_id=1;
  vm->n_async_sl=0;
  vm->async_group_active=0;
  vm->async_group_id=0;
  vm->async_group_status=1;
  vm->async_group_count=0;
}
static void ds_maps_free(GmlVM *vm){
  for(int i=0;i<GML_DS_MAP_MAX;i++){
    GmlDSMap *m=&vm->ds_map[i];
    for(int j=0;j<m->len;j++) free(m->entry[j].key);   /* owned val/key_val strings leak by design: d=0 refs escape via ds_ret */
    free(m->entry);
    free(m->hidx);
    memset(m,0,sizeof(*m));
  }
  for(int i=0;i<GML_DS_LIST_MAX;i++){
    free(vm->ds_list[i].item);
    memset(&vm->ds_list[i],0,sizeof(vm->ds_list[i]));
  }
  for(int i=0;i<GML_DS_GRID_MAX;i++){ free(vm->ds_grid[i].cell); memset(&vm->ds_grid[i],0,sizeof(vm->ds_grid[i])); }
  vm->next_ds_id=1;
  vm->ds_list_compat_repair=0;
}
void gml_vm_free(GmlVM *vm){
  gml_colgrid_invalidate(vm);
  free(vm->obj_alive); vm->obj_alive=NULL;
  free(vm->obj_head); vm->obj_head=NULL; free(vm->inst_next); vm->inst_next=NULL; free(vm->inst_prev); vm->inst_prev=NULL;
  free(vm->cg_off); vm->cg_off=NULL; free(vm->cg_items); vm->cg_items=NULL; vm->cg_items_cap=0;
  free(vm->cg_overlay); vm->cg_overlay=NULL; vm->cg_overlay_cap=vm->cg_overlay_n=0;
  freeset_begin();
  varmap_free(&vm->globals);
  for(int i=0;i<vm->inst_count;i++) varmap_free(&vm->inst[i].vars);
  for(int i=0;i<vm->n_structs;i++) if(vm->structs[i]) varmap_free(&vm->structs[i]->vars);
  freeset_end();
  for(int i=0;i<vm->n_structs;i++) free(vm->structs[i]);
  free(vm->structs); free(vm->struct_gen); free(vm->struct_free);
  ini_free(vm);
  io_free(vm);
  ds_maps_free(vm);
  free(vm->inst);
  for(int i=0;i<vm->n_objects;i++) free(vm->objects[i].events);
  for(int i=0;i<vm->n_paths;i++) free(vm->paths[i].pts);
  free(vm->paths);
  free(vm->objects); free(vm->col_events); free(vm->col_pair_cache); free(vm->event_cache);
  gml_tilemaps_clear(vm);
  free(vm->rtl); free(vm->rte); free(vm->view_ovr); free(vm->tilemaps);
}

/* ---------------- save-state runtime serialization ---------------- */
typedef struct { uint8_t *data; size_t cap, pos; int ok; GmlVM *vm; int compact_strings, array_meta; } StateW;
typedef struct { const uint8_t *data; size_t cap, pos; int ok; GmlVM *vm; int compact_strings, array_meta, v6, v7, v8, v9, v10, v11, v12, v13; } StateR;

static int state_debug_enabled(void){ return getenv("GML_STATE_DEBUG")!=NULL; }
static void state_debug(const char *msg, size_t pos, uint32_t v){
  if(state_debug_enabled()) fprintf(stderr,"[state] %s pos=%llu v=%u\n",msg,(unsigned long long)pos,v);
}

static void sw_raw(StateW *s, const void *p, size_t n){
  if(s->data){
    if(s->pos+n<=s->cap) memcpy(s->data+s->pos,p,n);
    else s->ok=0;
  }
  s->pos+=n;
}
static void sr_raw(StateR *s, void *p, size_t n){
  if(s->pos+n<=s->cap) memcpy(p,s->data+s->pos,n);
  else { memset(p,0,n); s->ok=0; }
  s->pos+=n;
}
static void sw_u32(StateW *s, uint32_t v){ sw_raw(s,&v,sizeof(v)); }
static void sw_i32(StateW *s, int v){ int32_t x=(int32_t)v; sw_raw(s,&x,sizeof(x)); }
static void sw_i64(StateW *s, int64_t v){ sw_raw(s,&v,sizeof(v)); }
static void sw_d(StateW *s, double v){ sw_raw(s,&v,sizeof(v)); }
static uint32_t sr_u32(StateR *s){ uint32_t v=0; sr_raw(s,&v,sizeof(v)); return v; }
static int sr_i32(StateR *s){ int32_t v=0; sr_raw(s,&v,sizeof(v)); return (int)v; }
static int64_t sr_i64(StateR *s){ int64_t v=0; sr_raw(s,&v,sizeof(v)); return v; }
static double sr_d(StateR *s){ double v=0; sr_raw(s,&v,sizeof(v)); return v; }
static void sw_particle_state(StateW *s){
  size_t pn=gml_part_state_size();
  if(pn>UINT32_MAX){ s->ok=0; pn=0; }
  sw_u32(s,(uint32_t)pn);
  if(s->data){
    if(s->pos+pn<=s->cap){
      size_t wr=0;
      if(!gml_part_state_save(s->data+s->pos,pn,&wr) || wr!=pn) s->ok=0;
    } else {
      s->ok=0;
    }
  }
  s->pos+=pn;
}
static int state_str_index_by_ptr(GmlVM *vm, const char *p){
  if(!vm || !vm->win || !p) return -1;
  const uint8_t *base=vm->win->data;
  const uint8_t *q=(const uint8_t*)p;
  if(q < base || q >= base + vm->win->size) return -1;
  uint32_t off=(uint32_t)(q-base);
  int lo=0, hi=vm->win->n_strs-1;
  while(lo<=hi){
    int m=(lo+hi)/2;
    if(vm->win->str_charoff[m]==off) return m;
    if(vm->win->str_charoff[m]<off) lo=m+1; else hi=m-1;
  }
  return -1;
}
static void sw_str(StateW *s, const char *p){
  if(!p) p="";
  if(s->compact_strings){
    int idx=state_str_index_by_ptr(s->vm,p);
    if(idx>=0){ sw_u32(s,0x80000000u | (uint32_t)idx); return; }
  }
  size_t n=strlen(p); if(n>UINT32_MAX) n=UINT32_MAX;
  sw_u32(s,(uint32_t)n); sw_raw(s,p,n);
}
static char *sr_str_dup(StateR *s){
  uint32_t n=sr_u32(s);
  if(s->compact_strings && (n&0x80000000u)){
    uint32_t idx=n&0x7fffffffu;
    const char *p=(s->vm && s->vm->win && idx<(uint32_t)s->vm->win->n_strs) ? s->vm->win->strs[idx] : "";
    return strdup(p?p:"");
  }
  if(n>1024*1024){ state_debug("string too large",s->pos,n); s->ok=0; return strdup(""); }
  char *p=malloc((size_t)n+1);
  if(!p){ s->ok=0; return NULL; }
  sr_raw(s,p,n); p[n]=0; return p;
}
static const char *state_intern(GmlVM *vm, char *owned){
  if(!owned) return "";
  const char *hit=gml_win_intern_lookup(vm->win,owned);   /* Look up an existing interned STRG string. */
  if(hit){ free(owned); return hit; }
  return owned; /* intentionally kept alive like other runtime strings in this VM */
}
static int state_val_is_default_zero(GmlVal v){
  return v.t==V_REAL && v.d==0.0;
}
static void sw_val(StateW *s, GmlVal v, int depth);
static int state_array_row_width(GmlArr *A, int row){
  if(!A || row<0 || row>=A->height2d || row>=A->row_cap || !A->row_len) return -1;
  int w=A->row_len[row];
  if(w<0) w=0;
  if(w>GML_2D_STRIDE) w=GML_2D_STRIDE;
  int base=row*GML_2D_STRIDE;
  if(base>=A->len) return 0;
  if(w>A->len-base) w=A->len-base;
  return w;
}
static void sw_array_sparse_entries(StateW *s, GmlArr *A, int depth, uint32_t *count){
  if(!A || !count) return;
  if(A->is_2d && A->row_len && A->height2d>0){
    for(int row=0; row<A->height2d; row++){
      int w=state_array_row_width(A,row);
      if(w<0) break;
      int base=row*GML_2D_STRIDE;
      for(int col=0; col<w; col++){
        int idx=base+col;
        if(idx<0 || idx>=A->len) break;
        if(state_val_is_default_zero(A->data[idx])) continue;
        sw_u32(s,(uint32_t)idx);
        sw_val(s,A->data[idx],depth+1);
        (*count)++;
      }
    }
    return;
  }
  for(int i=0;i<A->len;i++) if(!state_val_is_default_zero(A->data[i])){
    sw_u32(s,(uint32_t)i);
    sw_val(s,A->data[i],depth+1);
    (*count)++;
  }
}
static void sw_val(StateW *s, GmlVal v, int depth){
  if(v.t==V_ARR && depth>=8) v=vreal(0);
  sw_u32(s,(uint32_t)v.t);
  if(v.t==V_REAL){ sw_d(s,v.d); return; }
  if(v.t==V_STR){ sw_str(s,v.s); return; }
  if(v.t==V_UNDEF) return;
  if(v.t==V_ARR && v.arr && depth<8){
    GmlArr *A=(GmlArr*)v.arr;
    if(s->compact_strings && A->len>=0 && A->len<0x7fffffff){
      sw_u32(s,0x80000000u | (uint32_t)A->len);
      if(s->array_meta){
        uint32_t h=(A->is_2d && A->height2d>0) ? (uint32_t)A->height2d : 0;
        sw_u32(s,h);
        for(uint32_t i=0;i<h;i++){
          uint32_t w=(A->row_len && i<(uint32_t)A->row_cap && A->row_len[i]>0) ? (uint32_t)A->row_len[i] : 0;
          sw_u32(s,w);
        }
      }
      size_t count_pos=s->pos;
      sw_u32(s,0);
      uint32_t count=0;
      sw_array_sparse_entries(s,A,depth,&count);
      if(s->data && count_pos+sizeof(uint32_t)<=s->cap)
        memcpy(s->data+count_pos,&count,sizeof(count));
      return;
    }
    sw_u32(s,(uint32_t)A->len);
    if(s->array_meta){
      uint32_t h=(A->is_2d && A->height2d>0) ? (uint32_t)A->height2d : 0;
      sw_u32(s,h);
      for(uint32_t i=0;i<h;i++){
        uint32_t w=(A->row_len && i<(uint32_t)A->row_cap && A->row_len[i]>0) ? (uint32_t)A->row_len[i] : 0;
        sw_u32(s,w);
      }
    }
    for(int i=0;i<A->len;i++) sw_val(s,A->data[i],depth+1);
    return;
  }
  sw_u32(s,0);
}
static GmlVal sr_val(GmlVM *vm, StateR *s, int depth){
  uint32_t t=sr_u32(s);
  if(t==V_REAL) return vreal(sr_d(s));
  if(t==V_STR){ char *p=sr_str_dup(s); return vstr(state_intern(vm,p)); }
  if(t==V_UNDEF) return vundef();
  if(t==V_ARR && depth<8){
    uint32_t raw_len=sr_u32(s);
    int sparse = s->compact_strings && (raw_len&0x80000000u);
    uint32_t len = raw_len&0x7fffffffu;
    size_t remain = s->pos <= s->cap ? s->cap - s->pos : 0;
    if(len>1000000 || (!sparse && len>remain/4)){
      state_debug("array too large",s->pos,len);
      s->ok=0; return vreal(0);
    }
    GmlArr *A=calloc(1,sizeof(GmlArr));
    if(!A){ s->ok=0; return vreal(0); }
    if(len>32000 && getenv("GML_DBG_STATE_TIME")){ static int ap=0;
      if(ap++<20) fprintf(stderr,"[vmload]   big array len=%u sparse=%d\n",len,sparse); }
    A->len=A->cap=(int)len;
    A->data=calloc(len?len:1,sizeof(GmlVal));
    if(!A->data){ free(A); s->ok=0; return vreal(0); }
    /* calloc IS the fill: vreal(0) = {V_REAL=0, 0.0, NULL, NULL} = all-zero bytes. sparse 2D
     * arrays have logical lengths in the hundreds of thousands (row*32000 stride); explicitly
     * storing vreal(0) into every slot touched a large amount of fresh pages per state load, which made
     * retro_unserialize slowly and RetroArch's rewind a slideshow. Only the sparse entries below
     * touch memory now. */
    if(s->array_meta){
      uint32_t h=sr_u32(s);
      if(h>100000){ state_debug("array meta too large",s->pos,h); s->ok=0; h=0; }
      if(h>0){
        A->is_2d=1;
        arr_row_ensure(A,(int)h-1);
        for(uint32_t i=0;i<h;i++){
          uint32_t w=sr_u32(s);
          if(w>GML_2D_STRIDE){ state_debug("array row too large",s->pos,w); s->ok=0; w=GML_2D_STRIDE; }
          if(A->row_len && i<(uint32_t)A->row_cap) A->row_len[i]=(int)w;
        }
      }
    }
    if(sparse){
      uint32_t count=sr_u32(s);
      remain = s->pos <= s->cap ? s->cap - s->pos : 0;
      if(count>len || count>remain/8){ state_debug("sparse array too large",s->pos,count); s->ok=0; count=0; }
      for(uint32_t n=0;n<count;n++){
        uint32_t idx=sr_u32(s);
        GmlVal elem=sr_val(vm,s,depth+1);
        if(idx<len) A->data[idx]=elem;
        else s->ok=0;
      }
    } else {
      for(uint32_t i=0;i<len;i++) A->data[i]=sr_val(vm,s,depth+1);
    }
    if(!s->array_meta) arr_rebuild_2d_meta(A);
    GmlVal v=vreal(0); v.t=V_ARR; v.arr=A; return v;
  }
  state_debug("bad value type",s->pos,t);
  s->ok=0; return vreal(0);
}
static void sw_varmap(StateW *s, GmlVarMap *m){
  static int dbg=-1; if(dbg<0) dbg=getenv("GML_DBG_STATEVAR")!=NULL;
  sw_u32(s,(uint32_t)m->len);
  for(int i=0;i<m->cap;i++) if(m->slots[i].key){
    if(dbg) fprintf(stderr,"[svar] %s t=%d\n",m->slots[i].key,m->slots[i].val.t);
    sw_str(s,m->slots[i].key);
    sw_val(s,m->slots[i].val,0);
  }
}
static int sr_varmap(GmlVM *vm, StateR *s, GmlVarMap *m){
  uint32_t n=sr_u32(s);
  if(n>100000){ state_debug("varmap too large",s->pos,n); s->ok=0; return 0; }
  for(uint32_t i=0;i<n;i++){
    char *k=sr_str_dup(s); const char *key=state_intern(vm,k);
    GmlVal v=sr_val(vm,s,0);
    gml_arr_mark_escaped(v);
    *gml_varmap_put(m,key)=v;
  }
  return s->ok;
}
/* GMV6 per-instance layout (LOSSLESS compaction; a state is written every frame under rewind):
 *  flags byte (active|marked<<1|deactivated<<2), id u32, obj i32,
 *  field mask u32 (bit set = value differs from its default and follows as a double, in bit
 *  order), x/y/xprev/yprev/xstart/ystart always as doubles, alarm mask u16 (bit = alarm != -1,
 *  set ones follow), path block (11 doubles) only when mask bit 20 is set, then the varmap.
 *  Defaults mirror init_inst; equality is exact, so untouched fields round-trip bit-perfectly
 *  and anything else is written verbatim. Typical terrain instance: 308 -> ~80 bytes. */
#define GMV6_NOPT 20
static const double gmv6_def[GMV6_NOPT]={-1,-1,0,1,1,1,0,1,16777215,0,1,0,0,0,0,0,0,0,270,0};
#define GMV6_FIELDS(X) \
  X(0,sprite_index) X(1,mask_index) X(2,image_index) X(3,image_speed) X(4,image_xscale) \
  X(5,image_yscale) X(6,image_angle) X(7,image_alpha) X(8,image_blend) X(9,depth) \
  X(10,visible) X(11,solid) X(12,persistent) X(13,hspeed) X(14,vspeed) X(15,direction) \
  X(16,speed) X(17,gravity) X(18,gravity_direction) X(19,friction)
static int gmv6_path_present(GmlInstance *in){
  return in->path_index!=-1 || in->path_position!=0 || in->path_positionprevious!=0 ||
         in->path_speed!=0 || in->path_orientation!=0 || in->path_scale!=1 ||
         in->path_endaction!=0 || in->path_xoff!=0 || in->path_yoff!=0 ||
         in->path_origin_x!=0 || in->path_origin_y!=0;
}
static void sw_instance(StateW *s, GmlInstance *in){
  uint8_t fl=(in->active?1:0)|(in->marked?2:0)|(in->deactivated?4:0);
  sw_raw(s,&fl,1);
  sw_u32(s,in->id); sw_i32(s,in->obj);
  uint32_t fm=0;
  #define FCHK(bit,field) if(in->field!=gmv6_def[bit]) fm|=1u<<(bit);
  GMV6_FIELDS(FCHK)
  #undef FCHK
  if(gmv6_path_present(in)) fm|=1u<<20;
  sw_u32(s,fm);
  sw_d(s,in->x); sw_d(s,in->y); sw_d(s,in->xprevious); sw_d(s,in->yprevious);
  sw_d(s,in->xstart); sw_d(s,in->ystart);
  #define FWR(bit,field) if(fm&(1u<<(bit))) sw_d(s,in->field);
  GMV6_FIELDS(FWR)
  #undef FWR
  { uint16_t am=0;
    for(int i=0;i<GML_ALARMS;i++) if(in->alarm[i]!=-1) am|=(uint16_t)(1u<<i);
    sw_raw(s,&am,2);
    for(int i=0;i<GML_ALARMS;i++) if(am&(1u<<i)) sw_d(s,in->alarm[i]); }
  if(fm&(1u<<20)){
    sw_d(s,in->path_index); sw_d(s,in->path_position); sw_d(s,in->path_positionprevious);
    sw_d(s,in->path_speed); sw_d(s,in->path_orientation); sw_d(s,in->path_scale);
    sw_d(s,in->path_endaction); sw_d(s,in->path_xoff); sw_d(s,in->path_yoff);
    sw_d(s,in->path_origin_x); sw_d(s,in->path_origin_y);
  }
  sw_varmap(s,&in->vars);
}
static void vm_state_profile_globals(GmlVM *vm){
  static size_t last_total = 0;
  if(!getenv("GML_VM_STATE_PROFILE") || !vm) return;
  typedef struct { const char *key; size_t bytes; } Top;
  Top top[10]; memset(top,0,sizeof(top));
  size_t total=0;
  for(int i=0;i<vm->globals.cap;i++) if(vm->globals.slots[i].key){
    StateW ts={0}; ts.ok=1; ts.vm=vm; ts.compact_strings=1;
    sw_str(&ts,vm->globals.slots[i].key);
    sw_val(&ts,vm->globals.slots[i].val,0);
    total += ts.pos;
    for(int k=0;k<10;k++) if(ts.pos > top[k].bytes){
      memmove(&top[k+1],&top[k],(size_t)(9-k)*sizeof(top[0]));
      top[k]=(Top){vm->globals.slots[i].key,ts.pos};
      break;
    }
  }
  if(total <= last_total) return;
  last_total = total;
  fprintf(stderr,"[vm-state-profile] globals_total=%llu globals=%d\n",
          (unsigned long long)total, vm->globals.len);
  for(int i=0;i<10 && top[i].key;i++)
    fprintf(stderr,"[vm-state-profile] global[%d] %s=%llu\n",
            i, top[i].key, (unsigned long long)top[i].bytes);
  Top inst_top[10]; memset(inst_top,0,sizeof(inst_top));
  size_t inst_total=0;
  for(int i=0;i<vm->inst_count;i++){
    StateW ts={0}; ts.ok=1; ts.vm=vm; ts.compact_strings=1; ts.array_meta=1;
    sw_instance(&ts,&vm->inst[i]);
    inst_total += ts.pos;
    const char *name="?";
    int obj=vm->inst[i].obj;
    if(obj>=0 && obj<vm->n_objects && vm->objects[obj].name) name=vm->objects[obj].name;
    for(int k=0;k<10;k++) if(ts.pos > inst_top[k].bytes){
      memmove(&inst_top[k+1],&inst_top[k],(size_t)(9-k)*sizeof(inst_top[0]));
      inst_top[k]=(Top){name,ts.pos};
      break;
    }
  }
  fprintf(stderr,"[vm-state-profile] instances_total=%llu inst_count=%d\n",
          (unsigned long long)inst_total, vm->inst_count);
  for(int i=0;i<10 && inst_top[i].key;i++)
    fprintf(stderr,"[vm-state-profile] inst[%d] %s=%llu\n",
            i, inst_top[i].key, (unsigned long long)inst_top[i].bytes);
  Top struct_top[10]; memset(struct_top,0,sizeof(struct_top));
  size_t struct_total=0; int struct_live=0;
  for(int i=0;i<vm->n_structs;i++) if(vm->structs[i]){
    struct_live++;
    StateW ts={0}; ts.ok=1; ts.vm=vm; ts.compact_strings=1; ts.array_meta=1;
    sw_instance(&ts,vm->structs[i]);
    struct_total += ts.pos;
    const char *name="?";
    GmlVal *nm=gml_varmap_get(&vm->structs[i]->vars,"__name");
    if(nm && nm->t==V_STR && nm->s) name=nm->s;
    for(int k=0;k<10;k++) if(ts.pos > struct_top[k].bytes){
      memmove(&struct_top[k+1],&struct_top[k],(size_t)(9-k)*sizeof(struct_top[0]));
      struct_top[k]=(Top){name,ts.pos};
      break;
    }
  }
  fprintf(stderr,"[vm-state-profile] structs_total=%llu structs=%d/%d\n",
          (unsigned long long)struct_total, struct_live, vm->n_structs);
  for(int i=0;i<10 && struct_top[i].key;i++)
    fprintf(stderr,"[vm-state-profile] struct[%d] %s=%llu\n",
            i, struct_top[i].key, (unsigned long long)struct_top[i].bytes);
  size_t ds_map_total=0, ds_list_total=0, ds_grid_total=0;
  int ds_map_live=0, ds_list_live=0, ds_grid_live=0;
  for(int i=0;i<GML_DS_MAP_MAX;i++) if(vm->ds_map[i].live){
    GmlDSMap *m=&vm->ds_map[i]; ds_map_live++;
    StateW ts={0}; ts.ok=1; ts.vm=vm; ts.compact_strings=1; ts.array_meta=1;
    sw_u32(&ts,m->id); sw_i32(&ts,m->len);
    for(int j=0;j<m->len;j++){ sw_str(&ts,m->entry[j].key); sw_val(&ts,m->entry[j].key_val,0); sw_val(&ts,m->entry[j].val,0); }
    ds_map_total += ts.pos;
  }
  for(int i=0;i<GML_DS_LIST_MAX;i++) if(vm->ds_list[i].live){
    GmlDSList *l=&vm->ds_list[i]; ds_list_live++;
    StateW ts={0}; ts.ok=1; ts.vm=vm; ts.compact_strings=1; ts.array_meta=1;
    sw_u32(&ts,l->id); sw_i32(&ts,l->len);
    for(int j=0;j<l->len;j++) sw_val(&ts,l->item[j],0);
    ds_list_total += ts.pos;
  }
  for(int i=0;i<GML_DS_GRID_MAX;i++) if(vm->ds_grid[i].live){
    GmlDSGrid *g=&vm->ds_grid[i]; ds_grid_live++;
    StateW ts={0}; ts.ok=1; ts.vm=vm; ts.compact_strings=1; ts.array_meta=1;
    sw_u32(&ts,g->id); sw_i32(&ts,g->w); sw_i32(&ts,g->h);
    long long cells=(long long)g->w*g->h;
    for(long long j=0;j<cells;j++) sw_val(&ts,g->cell[j],0);
    ds_grid_total += ts.pos;
  }
  fprintf(stderr,"[vm-state-profile] ds_map=%d/%llu ds_list=%d/%llu ds_grid=%d/%llu\n",
          ds_map_live,(unsigned long long)ds_map_total,
          ds_list_live,(unsigned long long)ds_list_total,
          ds_grid_live,(unsigned long long)ds_grid_total);
}
static void sr_instance(GmlVM *vm, StateR *s, GmlInstance *in){
  if(s->v6){
    uint8_t fl=0; sr_raw(s,&fl,1);
    in->active=fl&1; in->marked=(fl>>1)&1; in->deactivated=(fl>>2)&1;
    in->id=sr_u32(s); in->obj=sr_i32(s);
    uint32_t fm=sr_u32(s);
    in->x=sr_d(s); in->y=sr_d(s); in->xprevious=sr_d(s); in->yprevious=sr_d(s);
    in->xstart=sr_d(s); in->ystart=sr_d(s);
    #define FRD(bit,field) in->field = (fm&(1u<<(bit))) ? sr_d(s) : gmv6_def[bit];
    GMV6_FIELDS(FRD)
    #undef FRD
    { uint16_t am=0; sr_raw(s,&am,2);
      for(int i=0;i<GML_ALARMS;i++) in->alarm[i] = (am&(1u<<i)) ? sr_d(s) : -1; }
    if(fm&(1u<<20)){
      in->path_index=sr_d(s); in->path_position=sr_d(s); in->path_positionprevious=sr_d(s);
      in->path_speed=sr_d(s); in->path_orientation=sr_d(s); in->path_scale=sr_d(s);
      in->path_endaction=sr_d(s); in->path_xoff=sr_d(s); in->path_yoff=sr_d(s);
      in->path_origin_x=sr_d(s); in->path_origin_y=sr_d(s);
    } else {
      in->path_index=-1; in->path_position=0; in->path_positionprevious=0;
      in->path_speed=0; in->path_orientation=0; in->path_scale=1;
      in->path_endaction=0; in->path_xoff=0; in->path_yoff=0;
      in->path_origin_x=0; in->path_origin_y=0;
    }
    sr_varmap(vm,s,&in->vars);
    return;
  }
  in->active=sr_i32(s); in->marked=sr_i32(s); in->deactivated=sr_i32(s);
  in->id=sr_u32(s); in->obj=sr_i32(s);
  in->x=sr_d(s); in->y=sr_d(s); in->xprevious=sr_d(s); in->yprevious=sr_d(s);
  in->xstart=sr_d(s); in->ystart=sr_d(s);
  in->sprite_index=sr_d(s); in->mask_index=sr_d(s); in->image_index=sr_d(s); in->image_speed=sr_d(s);
  in->image_xscale=sr_d(s); in->image_yscale=sr_d(s); in->image_angle=sr_d(s);
  in->image_alpha=sr_d(s); in->image_blend=sr_d(s);
  in->depth=sr_d(s); in->visible=sr_d(s); in->solid=sr_d(s); in->persistent=sr_d(s);
  in->hspeed=sr_d(s); in->vspeed=sr_d(s); in->direction=sr_d(s); in->speed=sr_d(s);
  in->gravity=sr_d(s); in->gravity_direction=sr_d(s); in->friction=sr_d(s);
  for(int i=0;i<GML_ALARMS;i++) in->alarm[i]=sr_d(s);
  in->path_index=sr_d(s); in->path_position=sr_d(s); in->path_positionprevious=sr_d(s);
  in->path_speed=sr_d(s); in->path_orientation=sr_d(s); in->path_scale=sr_d(s);
  in->path_endaction=sr_d(s); in->path_xoff=sr_d(s); in->path_yoff=sr_d(s);
  in->path_origin_x=sr_d(s); in->path_origin_y=sr_d(s);
  sr_varmap(vm,s,&in->vars);
}
static void runtime_clear(GmlVM *vm){
  freeset_begin();
  varmap_free(&vm->globals);
  for(int i=0;i<vm->inst_count;i++) varmap_free(&vm->inst[i].vars);
  for(int i=0;i<vm->n_structs;i++) if(vm->structs[i]) varmap_free(&vm->structs[i]->vars);
  freeset_end();
  for(int i=0;i<vm->n_structs;i++){ free(vm->structs[i]); vm->structs[i]=NULL; }
  if(vm->struct_gen && vm->cap_structs>0) memset(vm->struct_gen,0,(size_t)vm->cap_structs);
  vm->n_structs=0; vm->n_struct_free=0; vm->structs_last_gc_frame=0;
  if(vm->inst && vm->inst_cap>0) memset(vm->inst,0,(size_t)vm->inst_cap*sizeof(GmlInstance));
  ini_free(vm);
  io_free(vm);
  ds_maps_free(vm);
  vm->inst_count=0; vm->cur_self=vm->cur_other=NULL; vm->cur_event=NULL; vm->cur_event_obj=0;
  vm->step_alloc_base=0; vm->action_relative=0;
  vm->window_x=0; vm->window_y=0; vm->window_cursor=0;
}
static int tilemap_diff_count(const GmlTileMap *tm){
  if(!tm || !tm->owned_tiles || !tm->base_tiles || tm->cols<=0 || tm->rows<=0) return 0;
  int cells=tm->cols*tm->rows, n=0;
  for(int c=0;c<cells;c++){
    uint32_t off=(uint32_t)c*4u;
    if(u32(tm->owned_tiles,off)!=u32(tm->base_tiles,off)) n++;
  }
  return n;
}
static void sw_vm(StateW *s, GmlVM *vm){
  s->vm=vm; s->compact_strings=1; s->array_meta=1;
  sw_u32(s,0x3D564D47u); /* GMV13: GMV12 plus room layer scroll age for rewind/load */
  sw_i32(s,vm->inst_count); sw_u32(s,vm->next_id);
  sw_i32(s,vm->room_index); sw_i32(s,vm->pending_room); sw_i32(s,vm->game_end);
  sw_i32(s,vm->started); sw_d(s,vm->last_key); sw_d(s,vm->window_fullscreen);
  { extern long g_vm_frame;
    long age=g_vm_frame - vm->room_enter_frame;
    if(age<0) age=0;
    sw_i64(s,(int64_t)age);
  }
  sw_i32(s,vm->action_relative);
  sw_i32(s,vm->script_argc); for(int i=0;i<16;i++) sw_val(s,vm->script_args[i],0);
  for(int i=0;i<16;i++) sw_u32(s,vm->rng_well[i]);
  sw_i32(s,vm->rng_index); sw_u32(s,vm->rng_state);
  sw_i32(s,vm->n_tile_mut); sw_raw(s,vm->tile_mut,sizeof(vm->tile_mut));
  sw_i32(s,vm->n_tile_del_at); sw_raw(s,vm->tile_del_at,sizeof(vm->tile_del_at));
  int mut_tm=0;
  for(int i=0;i<vm->n_tilemaps;i++)
    if(vm->tilemaps[i].used && vm->tilemaps[i].owned_tiles && tilemap_diff_count(&vm->tilemaps[i])>0) mut_tm++;
  sw_i32(s,mut_tm);
  for(int i=0;i<vm->n_tilemaps;i++) if(vm->tilemaps[i].used && vm->tilemaps[i].owned_tiles){
    GmlTileMap *tm=&vm->tilemaps[i];
    int diffs=tilemap_diff_count(tm);
    if(diffs<=0) continue;
    sw_i32(s,i);
    sw_i32(s,tm->cols);
    sw_i32(s,tm->rows);
    sw_i32(s,diffs);
    int cells=tm->cols*tm->rows;
    for(int c=0;c<cells;c++){
      uint32_t off=(uint32_t)c*4u;
      uint32_t datum=u32(tm->owned_tiles,off);
      if(datum==u32(tm->base_tiles,off)) continue;
      sw_i32(s,c);
      sw_u32(s,datum);
    }
  }
  sw_i32(s,vm->ini_n); sw_i32(s,vm->ini_open); sw_str(s,vm->ini_path);
  for(int i=0;i<vm->ini_n;i++){
    sw_str(s,vm->ini_kv[i].section); sw_str(s,vm->ini_kv[i].key);
    sw_i32(s,vm->ini_kv[i].is_str); sw_d(s,vm->ini_kv[i].val); sw_str(s,vm->ini_kv[i].sval);
  }
  sw_i32(s,vm->next_ds_id);
  int ds_live=0; for(int i=0;i<GML_DS_MAP_MAX;i++) if(vm->ds_map[i].live) ds_live++;
  sw_i32(s,ds_live);
  for(int i=0;i<GML_DS_MAP_MAX;i++) if(vm->ds_map[i].live){
    GmlDSMap *m=&vm->ds_map[i];
    sw_u32(s,m->id); sw_i32(s,m->len);
    for(int j=0;j<m->len;j++){
      sw_str(s,m->entry[j].key);
      sw_val(s,m->entry[j].key_val,0);
      sw_val(s,m->entry[j].val,0);
    }
  }
  int dl_live=0; for(int i=0;i<GML_DS_LIST_MAX;i++) if(vm->ds_list[i].live) dl_live++;
  sw_i32(s,dl_live);
  for(int i=0;i<GML_DS_LIST_MAX;i++) if(vm->ds_list[i].live){
    GmlDSList *l=&vm->ds_list[i];
    sw_u32(s,l->id); sw_i32(s,l->len);
    for(int j=0;j<l->len;j++) sw_val(s,l->item[j],0);
  }
  int dg_live=0; for(int i=0;i<GML_DS_GRID_MAX;i++) if(vm->ds_grid[i].live) dg_live++;
  sw_i32(s,dg_live);
  for(int i=0;i<GML_DS_GRID_MAX;i++) if(vm->ds_grid[i].live){
    GmlDSGrid *g=&vm->ds_grid[i];
    sw_u32(s,g->id); sw_i32(s,g->w); sw_i32(s,g->h);
    long long cells=(long long)g->w*g->h;
    for(long long j=0;j<cells;j++) sw_val(s,g->cell[j],0);
  }
  sw_varmap(s,&vm->globals);
  for(int i=0;i<vm->inst_count;i++) sw_instance(s,&vm->inst[i]);
  sw_i32(s,vm->n_structs);
  for(int i=0;i<vm->n_structs;i++){
    sw_i32(s,vm->struct_gen ? (int)(vm->struct_gen[i]&0x7F) : 0);
    sw_i32(s,vm->structs[i]!=NULL);
    if(vm->structs[i]) sw_instance(s,vm->structs[i]);
  }
  sw_d(s,vm->window_x); sw_d(s,vm->window_y);
  /* GMV5: runtime layers/elements (tile_add-painted terrain must survive rewind/loadstate) */
  sw_i32(s,vm->rt_next_id);
  int live_l=0; for(int i=0;i<vm->n_rtl;i++) if(vm->rtl[i].used) live_l++;
  sw_i32(s,live_l);
  for(int i=0;i<vm->n_rtl;i++) if(vm->rtl[i].used){ GmlRtLayer *l=&vm->rtl[i];
    sw_i32(s,l->id); sw_i32(s,l->visible); sw_d(s,l->depth);
    sw_d(s,l->x); sw_d(s,l->y); sw_d(s,l->hs); sw_d(s,l->vs);
    sw_raw(s,l->name,sizeof(l->name)); sw_i32(s,l->touched); }
  int live_e=0; for(int i=0;i<vm->n_rte;i++) if(vm->rte[i].used) live_e++;
  sw_i32(s,live_e);
  for(int i=0;i<vm->n_rte;i++) if(vm->rte[i].used){ GmlRtElem *e=&vm->rte[i];
    sw_i32(s,e->id); sw_i32(s,e->layer); sw_i32(s,e->type); sw_i32(s,e->sprite);
    sw_d(s,e->x); sw_d(s,e->y);
    sw_i32(s,e->sx); sw_i32(s,e->sy); sw_i32(s,e->w); sw_i32(s,e->h);
    sw_d(s,e->xs); sw_d(s,e->ys); sw_d(s,e->alpha);
    sw_i32(s,e->visible); sw_u32(s,e->blend);
    sw_i32(s,e->htiled); sw_i32(s,e->vtiled); sw_i32(s,e->stretch); }
  sw_i32(s,vm->window_cursor);
  sw_particle_state(s);
  vm_state_profile_globals(vm);
}
size_t gml_vm_state_size(GmlVM *vm){
  extern long g_vm_frame;
  if(vm && vm->n_structs>0 && vm->structs_last_gc_frame!=g_vm_frame){
    vm->structs_last_gc_frame=g_vm_frame;
    gml_struct_gc(vm);
  }
  StateW s={0}; s.ok=1; s.vm=vm; s.compact_strings=1; sw_vm(&s,vm); return s.pos;
}
int gml_vm_state_save(GmlVM *vm, void *data, size_t len, size_t *written){
  extern long g_vm_frame;
  if(vm && vm->n_structs>0 && vm->structs_last_gc_frame!=g_vm_frame){
    vm->structs_last_gc_frame=g_vm_frame;
    gml_struct_gc(vm);
  }
  StateW s={.data=(uint8_t*)data,.cap=len,.pos=0,.ok=1,.vm=vm,.compact_strings=1};
  sw_vm(&s,vm); if(written) *written=s.pos; return s.ok && s.pos<=len;
}
int gml_vm_state_load(GmlVM *vm, const void *data, size_t len, size_t *used){
  gml_colgrid_invalidate(vm);   /* wholesale: every instance is about to be rewritten */
  StateR s={.data=(const uint8_t*)data,.cap=len,.pos=0,.ok=1,.vm=vm};
  uint32_t magic=sr_u32(&s);
  if((magic!=0x31564D47u && magic!=0x32564D47u && magic!=0x33564D47u && magic!=0x34564D47u
      && magic!=0x35564D47u && magic!=0x36564D47u && magic!=0x37564D47u && magic!=0x38564D47u
      && magic!=0x39564D47u && magic!=0x3A564D47u && magic!=0x3B564D47u && magic!=0x3C564D47u
      && magic!=0x3D564D47u) || !s.ok){ state_debug("bad vm magic",s.pos,magic); return 0; }
  s.compact_strings = magic>=0x32564D47u;
  s.array_meta = magic>=0x34564D47u;
  s.v6 = magic>=0x36564D47u;
  s.v7 = magic>=0x37564D47u;
  s.v8 = magic>=0x38564D47u;
  s.v9 = magic>=0x39564D47u;
  s.v10 = magic>=0x3A564D47u;
  s.v11 = magic>=0x3B564D47u;
  s.v12 = magic>=0x3C564D47u;
  s.v13 = magic>=0x3D564D47u;
  void *render=vm->render, *audio=vm->audio;
  runtime_clear(vm);
  vm->ds_list_compat_repair = !s.v8;
  int inst_count=sr_i32(&s); if(inst_count<0 || inst_count>vm->inst_cap) s.ok=0;
  if(!s.ok) state_debug("bad inst_count",s.pos,(uint32_t)inst_count);
  vm->next_id=sr_u32(&s); vm->room_index=sr_i32(&s); vm->pending_room=sr_i32(&s);
  vm->game_end=sr_i32(&s); vm->started=sr_i32(&s);
  vm->last_key=sr_d(&s); vm->window_fullscreen=sr_d(&s);
  int64_t room_age=0;
  if(s.v13){
    room_age=sr_i64(&s);
    if(room_age<0) room_age=0;
    if(room_age>(int64_t)LONG_MAX) room_age=(int64_t)LONG_MAX;
  }
  { extern long g_vm_frame;
    vm->room_enter_frame = g_vm_frame - (long)room_age;
  }
  vm->action_relative=sr_i32(&s);
  vm->script_argc=sr_i32(&s); for(int i=0;i<16;i++){ vm->script_args[i]=sr_val(vm,&s,0); gml_arr_mark_escaped(vm->script_args[i]); }
  for(int i=0;i<16;i++) vm->rng_well[i]=sr_u32(&s);
  vm->rng_index=sr_i32(&s); vm->rng_state=sr_u32(&s);
  vm->n_tile_mut=sr_i32(&s); sr_raw(&s,vm->tile_mut,sizeof(vm->tile_mut));
  vm->n_tile_del_at=sr_i32(&s); sr_raw(&s,vm->tile_del_at,sizeof(vm->tile_del_at));
  if(vm->n_tile_mut<0 || vm->n_tile_mut>64 || vm->n_tile_del_at<0 || vm->n_tile_del_at>64){
    state_debug("bad tile mutation counts",s.pos,(uint32_t)vm->n_tile_mut);
    s.ok=0;
  }
  typedef struct { int index, cols, rows, n; unsigned char *data; int *cell; uint32_t *datum; } TileMapState;
  TileMapState *tm_state=NULL;
  int tm_state_n=0;
  if(s.v11 && s.ok){
    tm_state_n=sr_i32(&s);
    if(tm_state_n<0 || tm_state_n>512){ state_debug("bad tilemap state count",s.pos,(uint32_t)tm_state_n); s.ok=0; tm_state_n=0; }
    tm_state=tm_state_n?calloc((size_t)tm_state_n,sizeof(*tm_state)):NULL;
    if(tm_state_n && !tm_state) s.ok=0;
    for(int i=0;i<tm_state_n;i++){
      tm_state[i].index=sr_i32(&s);
      tm_state[i].cols=sr_i32(&s);
      tm_state[i].rows=sr_i32(&s);
      if(tm_state[i].cols<=0 || tm_state[i].rows<=0 || tm_state[i].cols>8192 || tm_state[i].rows>8192){
        state_debug("bad tilemap state dims",s.pos,(uint32_t)tm_state[i].cols);
        s.ok=0;
        tm_state[i].cols=tm_state[i].rows=0;
      }
      if(s.v12){
        int maxcells=tm_state[i].cols*tm_state[i].rows;
        tm_state[i].n=sr_i32(&s);
        if(tm_state[i].n<0 || tm_state[i].n>maxcells){
          state_debug("bad tilemap sparse count",s.pos,(uint32_t)tm_state[i].n);
          s.ok=0;
          tm_state[i].n=0;
        }
        if(tm_state[i].n>0){
          tm_state[i].cell=malloc((size_t)tm_state[i].n*sizeof(int));
          tm_state[i].datum=malloc((size_t)tm_state[i].n*sizeof(uint32_t));
          if(!tm_state[i].cell || !tm_state[i].datum){ s.ok=0; tm_state[i].n=0; }
        }
        for(int j=0;j<tm_state[i].n;j++){
          tm_state[i].cell[j]=sr_i32(&s);
          tm_state[i].datum[j]=sr_u32(&s);
          if(tm_state[i].cell[j]<0 || tm_state[i].cell[j]>=maxcells) s.ok=0;
        }
      } else {
        size_t bytes=(size_t)tm_state[i].cols*(size_t)tm_state[i].rows*4u;
        if(bytes>64u*1024u*1024u || s.pos>s.cap || bytes>s.cap-s.pos){
          state_debug("bad tilemap state bytes",s.pos,(uint32_t)bytes);
          s.ok=0;
          bytes=0;
        }
        if(bytes){
          tm_state[i].data=malloc(bytes);
          if(!tm_state[i].data) s.ok=0;
          else sr_raw(&s,tm_state[i].data,bytes);
        }
      }
    }
  }
  vm->ini_n=sr_i32(&s); vm->ini_open=sr_i32(&s);
  char *path=sr_str_dup(&s); snprintf(vm->ini_path,sizeof(vm->ini_path),"%s",path?path:""); free(path);
  if(vm->ini_n<0 || vm->ini_n>256){ state_debug("bad ini count",s.pos,(uint32_t)vm->ini_n); s.ok=0; }
  int ini_n=vm->ini_n; vm->ini_n=0;
  for(int i=0;i<ini_n && i<256;i++){
    vm->ini_kv[i].section=sr_str_dup(&s);
    vm->ini_kv[i].key=sr_str_dup(&s);
    vm->ini_kv[i].is_str=sr_i32(&s);
    vm->ini_kv[i].val=sr_d(&s);
    vm->ini_kv[i].sval=sr_str_dup(&s);
    vm->ini_n++;
  }
  vm->next_ds_id=sr_i32(&s);
  int ds_live=sr_i32(&s);
  if(ds_live<0 || ds_live>GML_DS_MAP_MAX){ state_debug("bad ds_map count",s.pos,(uint32_t)ds_live); s.ok=0; ds_live=0; }
  for(int mi=0;mi<ds_live;mi++){
    int slot=-1; for(int i=0;i<GML_DS_MAP_MAX;i++) if(!vm->ds_map[i].live){ slot=i; break; }
    if(slot<0){ s.ok=0; break; }
    GmlDSMap *m=&vm->ds_map[slot];
    m->live=1; m->id=sr_u32(&s); m->len=sr_i32(&s);
    if(m->len<0 || m->len>100000){ state_debug("bad ds_map len",s.pos,(uint32_t)m->len); s.ok=0; m->len=0; }
    m->cap=m->len; m->entry=m->cap?calloc((size_t)m->cap,sizeof(GmlDSMapEntry)):NULL;
    for(int j=0;j<m->len;j++){
      m->entry[j].key=sr_str_dup(&s);
      m->entry[j].key_val=sr_val(vm,&s,0);
      m->entry[j].val=sr_val(vm,&s,0);
      gml_arr_mark_escaped(m->entry[j].key_val);
      gml_arr_mark_escaped(m->entry[j].val);
    }
  }
  if(s.v8 && s.ok){
    int dl_live=sr_i32(&s);
    if(dl_live<0 || dl_live>GML_DS_LIST_MAX){ state_debug("bad ds_list count",s.pos,(uint32_t)dl_live); s.ok=0; dl_live=0; }
    for(int li=0;li<dl_live;li++){
      int slot=-1; for(int i=0;i<GML_DS_LIST_MAX;i++) if(!vm->ds_list[i].live){ slot=i; break; }
      if(slot<0){ s.ok=0; break; }
      GmlDSList *l=&vm->ds_list[slot];
      l->live=1; l->id=sr_u32(&s); l->len=sr_i32(&s);
      if(l->len<0 || l->len>100000){ state_debug("bad ds_list len",s.pos,(uint32_t)l->len); s.ok=0; l->len=0; }
      l->cap=l->len;
      l->item=l->cap?calloc((size_t)l->cap,sizeof(GmlVal)):NULL;
      if(l->cap && !l->item){ s.ok=0; l->len=l->cap=0; break; }
      for(int j=0;j<l->len;j++){
        l->item[j]=sr_val(vm,&s,0);
        gml_arr_mark_escaped(l->item[j]);
      }
    }
    int dg_live=sr_i32(&s);
    if(dg_live<0 || dg_live>GML_DS_GRID_MAX){ state_debug("bad ds_grid count",s.pos,(uint32_t)dg_live); s.ok=0; dg_live=0; }
    for(int gi=0;gi<dg_live;gi++){
      int slot=-1; for(int i=0;i<GML_DS_GRID_MAX;i++) if(!vm->ds_grid[i].live){ slot=i; break; }
      if(slot<0){ s.ok=0; break; }
      GmlDSGrid *g=&vm->ds_grid[slot];
      g->live=1; g->id=sr_u32(&s); g->w=sr_i32(&s); g->h=sr_i32(&s);
      long long cells=(long long)g->w*g->h;
      if(g->w<0 || g->h<0 || cells>8000000){ state_debug("bad ds_grid size",s.pos,(uint32_t)cells); s.ok=0; g->w=g->h=0; cells=0; }
      g->cell=cells?calloc((size_t)cells,sizeof(GmlVal)):NULL;
      if(cells && !g->cell){ s.ok=0; g->w=g->h=0; break; }
      for(long long j=0;j<cells;j++){
        g->cell[j]=sr_val(vm,&s,0);
        gml_arr_mark_escaped(g->cell[j]);
      }
    }
  }
  if(!s.ok) state_debug("before globals failed",s.pos,0);
#ifdef _WIN32
  /* GML_DBG_STATE_TIME is a Linux-side profiling aid; mingw lacks clock_gettime here */
  struct { long tv_sec, tv_nsec; } vt0={0,0},vt1={0,0},vt2={0,0}; int vdbg=0;
  #define clock_gettime(c,t) ((void)0)
#else
  struct timespec vt0,vt1,vt2; int vdbg=getenv("GML_DBG_STATE_TIME")!=NULL;
#endif
  if(vdbg) clock_gettime(CLOCK_MONOTONIC,&vt0);
  sr_varmap(vm,&s,&vm->globals);
  if(vdbg) clock_gettime(CLOCK_MONOTONIC,&vt1);
  if(!s.ok) state_debug("globals failed",s.pos,0);
  vm->inst_count=s.ok?inst_count:0;
  for(int i=0;i<vm->inst_count;i++){
    sr_instance(vm,&s,&vm->inst[i]);
    if(!s.ok){ state_debug("instance failed",s.pos,(uint32_t)i); break; }
  }
  if(s.v7 && s.ok){
    int ns=sr_i32(&s);
    if(ns<0 || ns>GML_STRUCT_SLOT_MAX){ state_debug("bad struct count",s.pos,(uint32_t)ns); s.ok=0; ns=0; }
    if(s.ok && !struct_ensure_cap(vm,ns)){ state_debug("struct alloc failed",s.pos,(uint32_t)ns); s.ok=0; ns=0; }
    vm->n_structs=ns; vm->n_struct_free=0;
    for(int i=0;i<ns && s.ok;i++){
      int gen=sr_i32(&s) & 0x7F;
      int live=sr_i32(&s);
      vm->struct_gen[i]=(unsigned char)gen;
      vm->structs[i]=NULL;
      if(live){
        GmlInstance *st=calloc(1,sizeof(GmlInstance));
        if(!st){ s.ok=0; break; }
        sr_instance(vm,&s,st);
        st->id = GML_STRUCT_ID_BASE + ((unsigned)gen << GML_STRUCT_SLOT_BITS) + (unsigned)i;
        st->obj = -1; st->active = 1;
        vm->structs[i]=st;
      } else {
        struct_free_push(vm,i);
      }
    }
  }
  if(vdbg){ clock_gettime(CLOCK_MONOTONIC,&vt2);
    static int vp=0; if(vp++<8) fprintf(stderr,"[vmload] globals=%.2fms inst(%d)=%.2fms\n",
      ((vt1.tv_sec-vt0.tv_sec)*1e3+(vt1.tv_nsec-vt0.tv_nsec)/1e6),
      vm->inst_count,
      ((vt2.tv_sec-vt1.tv_sec)*1e3+(vt2.tv_nsec-vt1.tv_nsec)/1e6)); }
  if(s.ok && s.pos + sizeof(double)*2 <= s.cap){
    vm->window_x=sr_d(&s); vm->window_y=sr_d(&s);
  }
  /* GMV5: runtime layers/elements */
  vm->n_rtl=0; vm->n_rte=0;
  if(magic>=0x35564D47u && s.ok){
    vm->rt_next_id=sr_i32(&s);
    int nl=sr_i32(&s);
    if(nl<0 || nl>4096){ state_debug("bad rt layer count",s.pos,(uint32_t)nl); s.ok=0; nl=0; }
    for(int i=0;i<nl && s.ok;i++){
      GmlRtLayer *l=gml_rt_layer_new(vm);
      if(!l){ s.ok=0; break; }
      int id=sr_i32(&s);
      l->visible=sr_i32(&s); l->depth=sr_d(&s);
      l->x=sr_d(&s); l->y=sr_d(&s); l->hs=sr_d(&s); l->vs=sr_d(&s);
      sr_raw(&s,l->name,sizeof(l->name)); l->name[sizeof(l->name)-1]=0;
      l->touched=sr_i32(&s);
      l->id=id;
      if(getenv("GML_LOG_RTL")){
        fprintf(stderr,"[rtl-state] load layer id=%d name=\"%s\" vis=%d depth=%.0f pos=(%.2f,%.2f) speed=(%.2f,%.2f) touched=%d\n",
                l->id,l->name,l->visible,l->depth,l->x,l->y,l->hs,l->vs,l->touched);
      }
    }
    int ne=sr_i32(&s);
    if(ne<0 || ne>1000000){ state_debug("bad rt elem count",s.pos,(uint32_t)ne); s.ok=0; ne=0; }
    for(int i=0;i<ne && s.ok;i++){
      GmlRtElem *e=gml_rt_elem_new(vm);
      if(!e){ s.ok=0; break; }
      int id=sr_i32(&s);
      e->layer=sr_i32(&s); e->type=sr_i32(&s); e->sprite=sr_i32(&s);
      e->x=sr_d(&s); e->y=sr_d(&s);
      e->sx=sr_i32(&s); e->sy=sr_i32(&s); e->w=sr_i32(&s); e->h=sr_i32(&s);
      e->xs=sr_d(&s); e->ys=sr_d(&s); e->alpha=sr_d(&s);
      e->visible=sr_i32(&s); e->blend=sr_u32(&s);
      e->htiled=sr_i32(&s); e->vtiled=sr_i32(&s); e->stretch=sr_i32(&s);
      e->id=id;
    }
  }
  if(s.v9 && s.ok) vm->window_cursor=sr_i32(&s);
  if(s.v10 && s.ok){
    uint32_t pn=sr_u32(&s);
    if(s.pos>s.cap || pn>s.cap-s.pos){ state_debug("bad particle state size",s.pos,pn); s.ok=0; }
    else {
      size_t pu=0;
      if(!gml_part_state_load(s.data+s.pos,pn,&pu) || pu>pn){ state_debug("particle state failed",s.pos,pn); s.ok=0; }
      s.pos+=pn;
    }
  } else {
    gml_part_reset_all();
  }
  vm->cur_self=vm->cur_other=NULL; vm->cur_event=NULL; vm->cur_event_obj=0;
  vm->render=render; vm->audio=audio;
  gml_obj_alive_recount(vm);   /* family live counts rebuilt from the restored pool */
  /* Rebuild tile-collision maps, which point into win data and are not serialized. GMV5+
   * states do serialize runtime layer/element state, so keep it intact; older states need the
   * room-definition runtime layers rebuilt as a compatibility fallback. */
  if(vm->win && vm->room_index>=0) gml_room_reload_layers_mode(vm, vm->room_index, magic<0x35564D47u);
  for(int i=0;i<tm_state_n;i++){
    TileMapState *ts=&tm_state[i];
    if(ts->index>=0 && ts->index<vm->n_tilemaps){
      GmlTileMap *tm=&vm->tilemaps[ts->index];
      if(tm->cols==ts->cols && tm->rows==ts->rows && (ts->data || ts->n>0) && gml_tilemap_ensure_owned(tm)){
        if(ts->data){
        memcpy(tm->owned_tiles,ts->data,(size_t)ts->cols*(size_t)ts->rows*4u);
        } else {
          for(int j=0;j<ts->n;j++){
            int c=ts->cell[j];
            if(c<0 || c>=ts->cols*ts->rows) continue;
            unsigned char *p=tm->owned_tiles+(size_t)c*4u;
            uint32_t datum=ts->datum[j];
            p[0]=(unsigned char)(datum&0xFFu);
            p[1]=(unsigned char)((datum>>8)&0xFFu);
            p[2]=(unsigned char)((datum>>16)&0xFFu);
            p[3]=(unsigned char)((datum>>24)&0xFFu);
          }
        }
      }
    }
    free(ts->data);
    free(ts->cell);
    free(ts->datum);
  }
  free(tm_state);
  if(used) *used=s.pos;
  return s.ok && s.pos<=len;
}
