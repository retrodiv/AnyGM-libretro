/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me> */
/* gml_vm.c — normalized GML bytecode interpreter. See gml_vm.h. */
#include "gml_vm.h"
#include "gml_render.h"
#include "gml_audio.h"
#include "gml_particle.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <limits.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

static double vm_profile_now_ms(void){
#ifdef _WIN32
  static LARGE_INTEGER frequency;
  static int ready;
  LARGE_INTEGER now;
  if(!ready){ QueryPerformanceFrequency(&frequency); ready=1; }
  QueryPerformanceCounter(&now);
  return frequency.QuadPart ? (double)now.QuadPart*1000.0/(double)frequency.QuadPart : 0.0;
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC,&ts);
  return ts.tv_sec*1000.0+ts.tv_nsec/1e6;
#endif
}

static uint32_t u32(const uint8_t *d, uint32_t o){
  return (uint32_t)d[o]|(uint32_t)d[o+1]<<8|(uint32_t)d[o+2]<<16|(uint32_t)d[o+3]<<24;
}
static float f32(const uint8_t *d, uint32_t o){ uint32_t v=u32(d,o); float f; memcpy(&f,&v,4); return f; }
static double classic_round_even(double x){
  double f=floor(x), diff=x-f;
  if(diff<0.5) return f;
  if(diff>0.5) return f+1.0;
  return fmod(f,2.0)==0.0 ? f : f+1.0;
}
int gml_real_compare_epsilon(double lhs, double rhs, int cmp, double epsilon){
  int order;
  if(isnan(lhs) || isnan(rhs)) return cmp==CMP_NEQ;
  if(epsilon<0.0) epsilon=0.0;
  double delta=lhs-rhs;
  order=fabs(delta)<=epsilon ? 0 : (delta<0.0 ? -1 : 1);
  switch(cmp){
    case CMP_LT: return order<0;
    case CMP_LTE: return order<=0;
    case CMP_EQ: return order==0;
    case CMP_NEQ: return order!=0;
    case CMP_GTE: return order>=0;
    case CMP_GT: return order>0;
    default: return 0;
  }
}
int gml_real_compare(double lhs, double rhs, int cmp, int classic){
  return gml_real_compare_epsilon(lhs,rhs,cmp,classic?1e-13:1e-5);
}
static const char *g_cur_code_name;
static GmlVM *g_cur_vm;
static int inst_is_struct_ref(const GmlInstance *in);
static void method_cache_invalidate(GmlInstance *in, const char *nm);
static GmlVal *struct_field_get_h(GmlVM *vm, GmlInstance *in, const char *nm, uint32_t nh);

/* ---------------- var map (key = interned name pointer) ---------------- */
/* hash/compare keys by CONTENT (FNV-1a + strcmp), not pointer identity: VM names come from
 * the data.win string table while C-side writers (builtins, room init) use literals — the same
 * variable must hit the same slot regardless of which pointer carries the name. */
static unsigned strhash(const char *p){ unsigned h=2166136261u; if(!p) p=""; while(*p){ h^=(unsigned char)*p++; h*=16777619u; } return h; }
enum {
  GML_HASH_METHOD_FN   = 0x35836763u,
  GML_HASH_METHOD_SELF = 0x61c0232du,
  GML_HASH_CONSTRUCTOR = 0x44f7d3e1u
};
static void varmap_grow(GmlVarMap *m){
  int nc = m->cap? m->cap*2 : 16;
  GmlVarSlot *ns = calloc(nc,sizeof(GmlVarSlot));
  for(int i=0;i<m->cap;i++) if(m->slots[i].key){
    if(!m->slots[i].hash) m->slots[i].hash=strhash(m->slots[i].key);
    unsigned h=m->slots[i].hash&(nc-1);
    while(ns[h].key) h=(h+1)&(nc-1);
    ns[h]=m->slots[i];
  }
  free(m->slots); m->slots=ns; m->cap=nc;
}
static GmlVal *gml_varmap_get_h(GmlVarMap *m, const char *key, uint32_t kh){
  if(!m->cap) return NULL;
  unsigned h=kh&(m->cap-1);
  while(m->slots[h].key){
    if(m->slots[h].hash==kh && (m->slots[h].key==key || !strcmp(m->slots[h].key,key))) return &m->slots[h].val;
    h=(h+1)&(m->cap-1);
  }
  return NULL;
}
GmlVal *gml_varmap_get(GmlVarMap *m, const char *key){
  return key?gml_varmap_get_h(m,key,strhash(key)):NULL;
}
static GmlVarSlot *gml_varmap_put_slot_h(GmlVarMap *m, const char *key, uint32_t kh,
                                         int key_owned){
  if(!key){ key=""; kh=strhash(key); key_owned=0; }
  if(m->len*4>=m->cap*3) varmap_grow(m);
  unsigned h=kh&(m->cap-1);
  while(m->slots[h].key){
    if(m->slots[h].hash==kh && (m->slots[h].key==key || !strcmp(m->slots[h].key,key))){
      if(key_owned) free((char*)key);
      return &m->slots[h];
    }
    h=(h+1)&(m->cap-1);
  }
  m->slots[h].key=key; m->slots[h].hash=kh; m->slots[h].val=vreal(0);
  m->slots[h].key_owned=(unsigned char)(key_owned!=0); m->len++;
  return &m->slots[h];
}
static GmlVal *gml_varmap_put_h(GmlVarMap *m, const char *key, uint32_t kh){
  return &gml_varmap_put_slot_h(m,key,kh,0)->val;
}
static GmlVal *gml_varmap_put_owned_h(GmlVarMap *m, char *key, uint32_t kh){
  return &gml_varmap_put_slot_h(m,key,kh,1)->val;
}
GmlVal *gml_varmap_put(GmlVarMap *m, const char *key){
  return gml_varmap_put_h(m,key,key?strhash(key):strhash(""));
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
/* A non-terminal pushac in a chained array assignment requires an array container rather
 * than the numeric zero returned by a terminal read. Materialize that container lazily; existing
 * scalar and string values remain observable so malformed chains do not replace user data. */
GmlVal gml_arr_chain_ensure(GmlVal arr, int idx){
  if(arr.t!=V_ARR || !arr.arr || idx<0) return vreal(0);
  GmlArr *A=arr.arr;
  GmlVal value=idx<A->len?A->data[idx]:vreal(0);
  if(value.t==V_REAL && value.d==0.0){
    arr_ensure(A,idx);
    if(idx<A->cap){
      A->data[idx]=gml_arr_new(0,vreal(0));
      value=A->data[idx];
    }
  }
  if(value.t==V_STR) value.d=0;
  return value;
}
void gml_arr_set_2d(GmlVal arr, int row, int column, GmlVal val){
  if(arr.t!=V_ARR || !arr.arr || row<0 || column<0 || column>=GML_2D_STRIDE) return;
  GmlArr *A=arr.arr;
  /* Preserve an already-flat legacy array, but create new function-form 2D
   * arrays as rows of arrays. This avoids allocating row*32000 holes for the
   * Studio array_set_2D compatibility function. */
  if(A->is_2d){
    long flat=(long)row*GML_2D_STRIDE+column;
    if(flat<0 || flat>=16000000) return;
    int idx=(int)flat; arr_note_2d_set(A,idx); arr_ensure(A,idx);
    if(idx<A->cap) A->data[idx]=arr_store_clone(val);
    return;
  }
  arr_ensure(A,row); if(row>=A->cap) return;
  if(A->data[row].t!=V_ARR || !A->data[row].arr)
    A->data[row]=gml_arr_new(0,vreal(0));
  A->nested_2d=1;
  gml_arr_set(A->data[row],column,val);
}
GmlVal gml_arr_get_2d(GmlVal arr, int row, int column){
  if(arr.t!=V_ARR || !arr.arr || row<0 || column<0 || column>=GML_2D_STRIDE) return vreal(0);
  GmlArr *A=arr.arr;
  /* Modern nested arrays and legacy flat 2D arrays can both reach these
   * compatibility functions, so preserve either representation. */
  if((A->nested_2d || !A->is_2d) && row<A->len && A->data[row].t==V_ARR)
    return gml_arr_get(A->data[row],column);
  long flat=(long)row*GML_2D_STRIDE+column;
  if(flat<0 || flat>=A->len) return vreal(0);
  GmlVal value=A->data[flat]; if(value.t==V_STR) value.d=0;
  return value;
}
static int arr_nested_set_flat(GmlVal arr,int flat,GmlVal value){
  if(arr.t!=V_ARR || !arr.arr || !((GmlArr*)arr.arr)->nested_2d) return 0;
  if(flat>=0) gml_arr_set_2d(arr,flat/GML_2D_STRIDE,flat%GML_2D_STRIDE,value);
  return 1;
}
static int arr_nested_get_flat(GmlVal arr,int flat,GmlVal *out){
  if(arr.t!=V_ARR || !arr.arr || !((GmlArr*)arr.arr)->nested_2d) return 0;
  *out=flat>=0?gml_arr_get_2d(arr,flat/GML_2D_STRIDE,flat%GML_2D_STRIDE):vreal(0);
  return 1;
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
void gml_arr_insert(GmlVal arr, int index, GmlVal *values, int count){
  if(arr.t!=V_ARR || !arr.arr || !values || count<=0) return;
  GmlArr *A=arr.arr;
  int old_len=A->len;
  if(index<0) index+=old_len;       /* -1 addresses the current last element. */
  if(index<0) index=0;
  if(index>16000000-count) return;
  if(index>old_len){
    gml_arr_resize(arr,index+count); /* arr_ensure zero-fills the gap. */
  } else {
    gml_arr_resize(arr,old_len+count);
    if(A->len!=old_len+count) return;
    memmove(&A->data[index+count],&A->data[index],
            (size_t)(old_len-index)*sizeof(*A->data));
  }
  for(int i=0;i<count;i++) A->data[index+i]=arr_store_clone(values[i]);
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
  for(int i=0;i<m->cap;i++) if(m->slots && m->slots[i].key_owned){
    free((char*)m->slots[i].key); }
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
static void log_val_simple(GmlVal v){
  if(v.t==V_STR){
    const char *s=v.s?v.s:"";
    fprintf(stderr,"\"");
    for(int i=0;s[i] && i<96;i++) fputc((s[i]=='\n'||s[i]=='\r')?' ':s[i],stderr);
    if(strlen(s)>96) fprintf(stderr,"...");
    fprintf(stderr,"\"");
  } else if(v.t==V_ARR){
    GmlArr *A=(GmlArr*)v.arr;
    fprintf(stderr,"<array len=%d",A?A->len:0);
    if(A){
      int n=A->len<5?A->len:5;
      for(int i=0;i<n;i++){ fprintf(stderr," "); log_val_simple(A->data[i]); }
      if(A->len>n) fprintf(stderr," ...");
    }
    fprintf(stderr,">");
  }
  else if(v.t==V_UNDEF) fprintf(stderr,"undefined");
  else fprintf(stderr,"%.17g",v.t==V_REAL?v.d:0.0);
}
static void motion_from_components(GmlInstance *in){
  in->speed=hypot(in->hspeed,in->vspeed);
  in->direction=atan2(-in->vspeed,in->hspeed)*180.0/M_PI;
  if(g_cur_vm && g_cur_vm->win && g_cur_vm->win->classic_version){
    in->direction=fmod(in->direction,360.0);
    if(in->direction<0) in->direction+=360.0;
    double rounded=round(in->direction);
    if(fabs(rounded-in->direction)<0.0001) in->direction=rounded;
    if(in->direction>=360.0) in->direction-=360.0;
  }
}
static void motion_from_speed_dir(GmlInstance *in){
  if(g_cur_vm && g_cur_vm->win && g_cur_vm->win->classic_version){
    in->direction=fmod(in->direction,360.0);
    if(in->direction<0) in->direction+=360.0;
  }
  in->hspeed=in->speed*cos(in->direction*M_PI/180.0);
  in->vspeed=-in->speed*sin(in->direction*M_PI/180.0);
  if(g_cur_vm && g_cur_vm->win && g_cur_vm->win->classic_version){
    double rounded=round(in->hspeed);
    if(fabs(rounded-in->hspeed)<0.0001) in->hspeed=rounded;
    rounded=round(in->vspeed);
    if(fabs(rounded-in->vspeed)<0.0001) in->vspeed=rounded;
  }
}

/* ---------------- builtin instance variables ---------------- */
/* returns 1 if name is a builtin and handled */
static int inst_builtin_get(GmlInstance *in, const char *n, GmlVal *out){
  if(!strcmp(n,"image_single")){ *out=vreal(in->image_speed==0? in->image_index : -1); return 1; }
  if(!strcmp(n,"layer")){
    if(g_cur_vm && in->draw_layer_order>=0){
      for(int i=0;i<g_cur_vm->n_rtl;i++){
        GmlRtLayer *layer=&g_cur_vm->rtl[i];
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
    if(g_cur_vm && g_cur_vm->win && g_cur_vm->win->classic_version && fabs(d)<1e-12) d=0;
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
  BT("x",x) BT("y",y) BT("phy_position_x",x) BT("phy_position_y",y)
  B("xprevious",xprevious) B("yprevious",yprevious)
  B("xstart",xstart) B("ystart",ystart)
  if(!strcmp(n,"sprite_index")){
    in->sprite_index=d; gml_colgrid_touch(in);
    /* queue the new sprite's atlas for the background decoder: sprite swaps often jump to a
     * not-yet-decoded page (transformations/bosses) and the draw follows within the same frame */
    if(g_cur_vm && g_cur_vm->render && (int)d>=0)
      gml_render_prefetch_sprite((GmlRender*)g_cur_vm->render,(int)d);
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
  if(!strcmp(n,"hspeed")){ in->hspeed=d; motion_from_components(in); return 1; }
  if(!strcmp(n,"vspeed")){ in->vspeed=d; motion_from_components(in); return 1; }
  if(!strcmp(n,"direction")){ in->direction=d; motion_from_speed_dir(in); return 1; }
  if(!strcmp(n,"speed")){ in->speed=d; motion_from_speed_dir(in); return 1; }
  return 0;
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
         !strcmp(n,"view_enabled"); }
static int is_classic_transition_builtin(GmlVM *vm,const char *n){
  return vm && vm->win && vm->win->classic_version &&
         (!strcmp(n,"transition_kind") || !strcmp(n,"transition_steps"));
}
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
  /* game_set_speed() is the authoritative Studio cadence once content selects one. The
   * frontend uses the same slot when scheduling retro_run/audio, so VM clocks and delta_time must
   * not continue advancing at the room resource's older speed. */
  GmlVal *runtime=vm?gml_varmap_get(&vm->globals,"__game_speed_fps"):NULL;
  double runtime_fps=runtime?asnum(*runtime):0.0;
  if(runtime_fps>0) return runtime_fps;
  GmlVal *p=gml_varmap_get(&vm->globals,"room_speed");
  double v=p?asnum(*p):0.0;
  if(v>0) return v;
  GmlRoom room;
  if(vm && vm->win && vm->room_index>=0 && gml_room_get(vm->win,vm->room_index,&room)==0 && room.speed>0)
    return room.speed;
  if(vm && vm->win && vm->win->game_speed>0) return vm->win->game_speed;
  return 30.0;
}
static double cpu_clock_ms(void){
  clock_t c=clock();
  if(c==(clock_t)-1) return 0.0;
  return (double)c * (1000.0 / CLOCKS_PER_SEC);
}
static double current_time_value(GmlVM *vm){
  extern long g_vm_frame;
  static long frame=-1;
  static double frame_cpu_ms=0.0;
  double now=cpu_clock_ms();
  if(frame!=g_vm_frame){
    frame=g_vm_frame;
    frame_cpu_ms=now;
  }
  double intra=now-frame_cpu_ms;
  if(intra<0.0) intra=0.0;
  /* Keep the old frame-clock base for gameplay timers, but let the value advance while GML is
   * executing. Some GM scripts implement sleep by spinning on current_time inside one frame. */
  return (double)g_vm_frame * (1000.0 / gml_room_speed(vm)) + intra;
}
static int current_calendar_value(const char *name,GmlVal *out){
  if(strncmp(name,"current_",8) || !strcmp(name,"current_time")) return 0;
  time_t now=time(NULL); struct tm value;
#ifdef _WIN32
  if(localtime_s(&value,&now)!=0) memset(&value,0,sizeof value);
#else
  if(!localtime_r(&now,&value)) memset(&value,0,sizeof value);
#endif
  if(!strcmp(name,"current_second")) *out=vreal(value.tm_sec);
  else if(!strcmp(name,"current_minute")) *out=vreal(value.tm_min);
  else if(!strcmp(name,"current_hour")) *out=vreal(value.tm_hour);
  else if(!strcmp(name,"current_day")) *out=vreal(value.tm_mday);
  else if(!strcmp(name,"current_weekday")) *out=vreal(value.tm_wday); /* Sunday=0 */
  else if(!strcmp(name,"current_month")) *out=vreal(value.tm_mon+1);
  else if(!strcmp(name,"current_year")) *out=vreal(value.tm_year+1900);
  else return 0;
  return 1;
}
/* Return a frame-based microsecond timer with intra-frame CPU-time advance. */
double gml_vm_get_timer_us(GmlVM *vm){
  extern long g_vm_frame;
  static long frame=-1;
  static double frame_cpu_ms=0.0;
  double now=cpu_clock_ms();
  if(frame!=g_vm_frame){ frame=g_vm_frame; frame_cpu_ms=now; }
  double intra_ms=now-frame_cpu_ms;
  if(intra_ms<0.0) intra_ms=0.0;
  return (double)g_vm_frame * (1000000.0 / gml_room_speed(vm)) + intra_ms*1000.0;
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
/* hash gate for the special-variable chains in var_get_h/var_set_h: almost every variable
 * access is a plain instance/global var, which otherwise pays the full strcmp chain on every
 * read and write. Hash-hit => run the original chain (its strcmps confirm; collisions are
 * safe); miss => the name is provably not special, go straight to the varmap. */
static const char *const g_special_var_names[]={
  "undefined","room","keyboard_lastkey","room_speed","working_directory","program_directory",
  "fps","delta_time","view_current","view_enabled","room_persistent","event_type","event_number","mouse_x","mouse_y",
  "current_time","current_second","current_minute","current_hour","current_day","current_weekday",
  "current_month","current_year","os_type","os_windows","os_uwp","os_xboxone","os_ps3","os_ps4","os_psvita",
  "os_macosx","os_linux","os_ios","os_android","os_unknown","os_switch_operating_system","room_width",
  "time_source_global","time_source_game","time_source_units_seconds","time_source_units_frames",
  "time_source_expire_nearest","time_source_expire_after","time_source_state_initial",
  "time_source_state_active","time_source_state_paused","time_source_state_stopped",
  "room_height","instance_count","health","lives","score","async_load","id","object_index",
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
static uint32_t g_special_var_hash[N_SPECIAL_VAR];
static uint64_t g_special_var_bloom;
static int g_special_var_built;
static int var_name_maybe_special(const char *name, uint32_t nh){
  if(!g_special_var_built){
    for(int i=0;i<N_SPECIAL_VAR;i++){
      uint32_t h=strhash(g_special_var_names[i]);
      g_special_var_hash[i]=h;
      g_special_var_bloom |= 1ull<<(h&63);
    }
    g_special_var_built=1;
  }
  if(g_special_var_bloom & (1ull<<(nh&63))){
    for(int i=0;i<N_SPECIAL_VAR;i++) if(g_special_var_hash[i]==nh) return 1;
  }
  /* prefix-matched specials (argumentN / argument_count / bbox_*) */
  if(name[0]=='a' && !strncmp(name,"argument",8)) return 1;
  if(name[0]=='b' && !strncmp(name,"bbox_",5)) return 1;
  return 0;
}
static int is_room_global_array(const char *n);
static GmlVal var_get_h(GmlVM *vm, int inst, const char *name, uint32_t nh){
  GmlVal out;
  if(inst==IT_STATIC){
    int ci=vm?vm->cur_code_index:-1;
    if(ci>=0 && ci<vm->code_static_count && vm->code_static){
      GmlVal *p=gml_varmap_get_h(&vm->code_static[ci],name,nh);
      return p?*p:vundef();
    }
    return vundef();
  }
  /* GM6/7/8 variables retain their old scalar-at-index-zero behaviour even when the same
   * built-in also exposes indexed view/background slots. Classic source commonly reads
   * `view_wview` with no brackets; returning the array value coerces to zero and can pin every
   * moving instance to the left edge. Keep this compatibility local to classic containers so
   * Studio arrays continue to use normal value semantics. */
  if(vm->win && vm->win->classic_version && strcmp(name,"view_current") &&
     is_room_global_array(name)){
    GmlVal *slot=gml_varmap_get_h(&vm->globals,name,nh);
    if(!slot) return vreal(0);
    if(slot->t!=V_ARR || !slot->arr) return *slot;
    GmlArr *a=slot->arr;
    return (a->data && a->len>0)?a->data[0]:vreal(0);
  }
  if(!var_name_maybe_special(name,nh)){
    if(inst==IT_GLOBAL){ GmlVal *p=gml_varmap_get_h(&vm->globals,name,nh); return p?*p:vreal(0); }
    GmlInstance *self=var_target(vm,inst);
    if(self){
      GmlVal *p=inst_is_struct_ref(self)?struct_field_get_h(vm,self,name,nh)
                                             :gml_varmap_get_h(&self->vars,name,nh);
      if(p) return *p;
    }
    return vreal(0);
  }
  if(!strcmp(name,"undefined")) return vundef();   /* GMS2.3 builtin literal used by optional-arg prologues */
  if(!strcmp(name,"room")) return vreal(vm->room_index);   /* GM built-in: current room index */
  if(!strcmp(name,"keyboard_lastkey")) return vreal(vm->last_key); /* GM: last key pressed */
  if(!strcmp(name,"room_speed")) return vreal(gml_room_speed(vm));
  if(!strcmp(name,"working_directory")){
    /* This is the installed file-bundle root. The file API overlays save_dir on reads and
     * redirects writes there, including absolute paths formed by concatenating this value. */
    static char wd[560];
    snprintf(wd,sizeof wd,"%s/",vm->win->content_dir);
    return vstr(wd); }
  if(!strcmp(name,"program_directory")){
    static char pd[560]; snprintf(pd,sizeof pd,"%s/",vm->win->content_dir);
    return vstr(pd); }
  if(!strcmp(name,"fps")) return vreal(gml_room_speed(vm));
  /* Studio exposes the previous frame duration in microseconds. A libretro frame is scheduled at
   * the declared cadence, so a fixed deterministic interval is the closest steady-run value and
   * keeps time-based behavior reproducible across host load and fast-forward. */
  if(!strcmp(name,"delta_time")) return vreal(1000000.0/gml_room_speed(vm));
  /* Report os_windows by default; GML_OS_TYPE overrides the platform value for testing. Provide
   * the os_* constants for runtime lookups as well as compiled literal values. */
  if(!strcmp(name,"os_type")){ const char *e=getenv("GML_OS_TYPE"); return vreal(e?atof(e):0 /*os_windows*/); }
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
  if(!strcmp(name,"event_type")) return vreal(vm->event_type);
  if(!strcmp(name,"event_number")) return vreal(vm->event_number);
  if(!strcmp(name,"current_time")) return vreal(current_time_value(vm));
  if(current_calendar_value(name,&out)) return out;
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
  if(is_classic_transition_builtin(vm,name) || inst==IT_GLOBAL || is_global_builtin(name)){
    GmlVal *p=gml_varmap_get_h(&vm->globals,name,nh); return p?*p:vreal(0); }
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
      if(vm_bbox(vm,self,&l,&t,&r,&b)){
        /* Studio exposes the far edges as exclusive coordinates; this is why its usual line and
         * rectangle probes use bbox_right-1 while a grounded probe starts at bbox_bottom.  The
         * collision engine itself keeps inclusive pixel bounds, and classic formats also expose
         * inclusive right/bottom, so translate only the Studio-facing special variables here. */
        if(!vm->win || !vm->win->classic_version){ r+=1.0; b+=1.0; }
        if(!strcmp(name,"bbox_left"))   return vreal(l);
        if(!strcmp(name,"bbox_right"))  return vreal(r);
        if(!strcmp(name,"bbox_top"))    return vreal(t);
        if(!strcmp(name,"bbox_bottom")) return vreal(b); }
      return vreal(0); }
    if(inst_builtin_get(self,name,&out)) return out;
    GmlVal *p=gml_varmap_get_h(&self->vars,name,nh); if(p) return *p;
  }
  return vreal(0);
}
/* GM: writing OBJECT.variable = value assigns to EVERY instance of that object (reading
 * returns only the first). inst_t in [0,n_objects) is an object index; a real instance id
 * is >=100000, so it never collides. Fans a write out to all instances of the object. */
static int is_object_scope(GmlVM *vm, int inst_t){ return inst_t>=0 && inst_t<vm->n_objects; }
static void var_set_h(GmlVM *vm, int inst, const char *name, uint32_t nh, GmlVal v){
  gml_arr_mark_escaped(v);   /* target is a global/instance slot: outlives the current scope */
  if(inst==IT_STATIC){
    int ci=vm?vm->cur_code_index:-1;
    if(ci>=0 && ci<vm->code_static_count && vm->code_static)
      *gml_varmap_put_h(&vm->code_static[ci],name,nh)=v;
    return;
  }
  { static const char *dv=NULL; static int dv_init=0;
    if(!dv_init){ dv=getenv("GML_DBG_VARSET"); dv_init=1; }
    if(dv && name && !strcmp(name,dv)){ extern long g_vm_frame;
      fprintf(stderr,"[varset] f%ld code=%s inst=%d %s type=%d value=%s%.2f\n",
        g_vm_frame,g_cur_code_name?g_cur_code_name:"?",inst,name,v.t,
        v.t==V_STR?"str:":"",v.t==V_REAL?v.d:0.0); } }
  if(vm->win && vm->win->classic_version && strcmp(name,"view_current") &&
     is_room_global_array(name)){
    GmlVal *slot=gml_varmap_put_h(&vm->globals,name,nh);
    GmlArr *a=arr_of(slot);
    a->escaped=1;
    arr_ensure(a,0);
    if(a && a->cap>0) a->data[0]=v;
    return;
  }
  if(!var_name_maybe_special(name,nh)){
    if(inst==IT_GLOBAL){ *gml_varmap_put_h(&vm->globals,name,nh)=v; return; }
    if(inst==IT_ALL){
      for(int i=0;i<vm->inst_count;i++){ GmlInstance *o=&vm->inst[i];
        if(o->active && !o->marked) *gml_varmap_put_h(&o->vars,name,nh)=v; }
      return;
    }
    if(is_object_scope(vm,inst)){   /* object.var = v -> all instances */
      for(int i=0;i<vm->inst_count;i++){ GmlInstance *o=&vm->inst[i];
        if(o->active && !o->marked && gml_object_is(vm,o->obj,inst)) *gml_varmap_put_h(&o->vars,name,nh)=v; }
      return;
    }
    GmlInstance *self=var_target(vm,inst);
    if(self){
      if(inst_is_struct_ref(self)) method_cache_invalidate(self,name);
      *gml_varmap_put_h(&self->vars,name,nh)=v;
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
    *gml_varmap_put_h(&vm->globals,name,nh)=v;
    return;
  }
  if(is_classic_transition_builtin(vm,name) || inst==IT_GLOBAL || is_global_builtin(name)){
    *gml_varmap_put_h(&vm->globals,name,nh)=v; return; }
  if(inst==IT_ALL){
    for(int i=0;i<vm->inst_count;i++){ GmlInstance *o=&vm->inst[i];
      if(o->active && !o->marked && !inst_builtin_set(o,name,v))
        *gml_varmap_put_h(&o->vars,name,nh)=v; }
    return;
  }
  if(is_object_scope(vm,inst)){   /* object.builtin = v (x, hspeed, visible, ...) -> all instances */
    for(int i=0;i<vm->inst_count;i++){ GmlInstance *o=&vm->inst[i];
      if(o->active && !o->marked && gml_object_is(vm,o->obj,inst)){
        if(!inst_builtin_set(o,name,v)) *gml_varmap_put_h(&o->vars,name,nh)=v; } }
    return;
  }
  GmlInstance *self = var_target(vm,inst);
  if(self){
    if(inst_is_struct_ref(self)){
      method_cache_invalidate(self,name);
      *gml_varmap_put_h(&self->vars,name,nh)=v;
      return;
    }
    if(inst_builtin_set(self,name,v)) return;
    *gml_varmap_put_h(&self->vars,name,nh)=v;
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
           !strcmp(s,"vspeed") || !strcmp(s,"object") || !strcmp(s,"camera");
  }
  return 0;
}
static double alarm_store_value(GmlVM *vm,GmlVal v){
  double value=v.t==V_REAL?v.d:(v.s?atof(v.s):0);
  /* GM6/7/8 stores alarms as integers. Delphi's Math.Round uses ties-to-even; nearbyint
   * supplies the same result under the process' default IEEE rounding mode. Studio semantics
   * retain fractional alarms, which several typewriter effects deliberately use. */
  return vm && vm->win && vm->win->classic_version ? nearbyint(value) : value;
}
static void array_set_h(GmlVM *vm, GmlVarMap *locals, int inst_t, const char *nm, uint32_t nh, int idx, GmlVal v){
  { static const char *debug_name=(const char*)-1;
    if(debug_name==(const char*)-1) debug_name=getenv("GML_DBG_ARRAYSET");
    if(debug_name && nm && !strcmp(debug_name,nm)){
      extern long g_vm_frame;
      fprintf(stderr,"[arrayset] f%ld scope=%d %s[%d] type=%d value=%.17g\n",
              g_vm_frame,inst_t,nm,idx,v.t,v.t==V_REAL?v.d:0.0);
    }
  }
  if(getenv("GML_LOG_VIEW") && !strcmp(nm,"view_camera")){
    extern long g_vm_frame;
    fprintf(stderr,"[camera] bind f%ld view=%d value=%.0f scope=%d global=%d\n",
            g_vm_frame,idx,asnum(v),inst_t,is_room_global_array(nm));
  }
  if(!strcmp(nm,"view_enabled")){
    /* Current bytecode represents this scalar built-in through its array-access opcode:
     * writes carry accessor index 0 while reads carry index 1.  The index is metadata, not a pair
     * of independent GML cells, so both forms address the same global scalar. */
    *gml_varmap_put_h(&vm->globals,nm,nh)=v;
    return;
  }
  if(!strcmp(nm,"argument")){
    if(idx>=0 && idx<16){
      vm->script_args[idx]=v;
      if(idx>=vm->script_argc) vm->script_argc=idx+1;
    }
    return;
  }
  if(!strcmp(nm,"alarm")){
    double alv=alarm_store_value(vm,v);
    /* Object-scope alarm writes apply to every active, unmarked matching instance. */
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
    if(s && idx>=0 && idx<GML_ALARMS) s->alarm[idx]=alv; return; }
  /* In classic GML every variable accessor carries an optional array index, including scalar
   * built-ins.  For scalar instance variables that index is ignored: `image_single[0]=7` is the
   * same built-in write as `image_single=7`, while alarm[] remains genuinely indexed above.
   * Letting the generic array path create a user field instead left the displayed sub-image at
   * frame zero in projects that use the old indexed spelling. */
  if(var_name_maybe_special(nm,nh) && !is_room_global_array(nm) &&
     inst_t!=IT_GLOBAL && inst_t!=IT_LOCAL){
    int handled=0;
    if(inst_t==IT_ALL){
      for(int i=0;i<vm->inst_count;i++){ GmlInstance *o=&vm->inst[i];
        if(o->active && !o->marked && !inst_is_struct_ref(o))
          handled |= inst_builtin_set(o,nm,v); }
      if(handled) return;
    } else if(is_object_scope(vm,inst_t)){
      for(int i=0;i<vm->inst_count;i++){ GmlInstance *o=&vm->inst[i];
        if(o->active && !o->marked && gml_object_is(vm,o->obj,inst_t) && !inst_is_struct_ref(o))
          handled |= inst_builtin_set(o,nm,v); }
      if(handled) return;
    } else {
      GmlInstance *s=resolve_inst(vm,inst_t);
      if(s && !inst_is_struct_ref(s) && inst_builtin_set(s,nm,v)) return;
    }
  }
  if(inst_t==IT_ALL && !is_room_global_array(nm)){
    for(int i=0;i<vm->inst_count;i++){ GmlInstance *o=&vm->inst[i];
      if(!o->active || o->marked) continue;
      GmlVal *slot=gml_varmap_put_h(&o->vars,nm,nh); GmlArr *A=arr_of(slot);
      A->escaped=1;
      if(arr_nested_set_flat(*slot,idx,v)) continue;
      gml_arr_mark_escaped(v); arr_note_2d_set(A,idx); arr_ensure(A,idx);
      if(idx>=0 && idx<A->cap) A->data[idx]=v; }
    return;
  }
  /* Non-alarm array write to OBJECT scope also fans out to all instances. */
  if(is_object_scope(vm,inst_t) && !is_room_global_array(nm)){
    for(int i=0;i<vm->inst_count;i++){ GmlInstance *o=&vm->inst[i];
      if(!o->active || o->marked || !gml_object_is(vm,o->obj,inst_t)) continue;
      GmlVal *slot=gml_varmap_put_h(&o->vars,nm,nh); GmlArr *A=arr_of(slot);
      A->escaped=1;
      if(arr_nested_set_flat(*slot,idx,v)) continue;
      gml_arr_mark_escaped(v); arr_note_2d_set(A,idx); arr_ensure(A,idx);
      if(idx>=0 && idx<A->cap) A->data[idx]=v; }
    return;
  }
  GmlVarMap *m=is_room_global_array(nm)? &vm->globals : scope_map(vm,locals,inst_t); if(!m) return;
  GmlVal *slot=gml_varmap_put_h(m,nm,nh); GmlArr *A=arr_of(slot);
  /* The array container itself lives in a global/instance map. A later `var alias = field`
   * only borrows that same pointer in this VM; mark the owner before the local scope is cleaned,
   * otherwise the alias frees the persistent array and the next event reads dangling memory. */
  if(m!=locals) A->escaped=1;
  if(arr_nested_set_flat(*slot,idx,v)) return;
  if(m!=locals || A->escaped) gml_arr_mark_escaped(v);   /* element outlives scope if its owner already does */
  arr_note_2d_set(A,idx); arr_ensure(A,idx);
  if(idx>=0 && idx<A->cap) A->data[idx]=v;
}
static GmlVal array_get_h(GmlVM *vm, GmlVarMap *locals, int inst_t, const char *nm, uint32_t nh, int idx){
  if(!strcmp(nm,"view_enabled")){
    GmlVal *slot=gml_varmap_get_h(&vm->globals,nm,nh);
    return slot?*slot:vreal(0);
  }
  if(!strcmp(nm,"argument")) return (idx>=0 && idx<vm->script_argc && idx<16) ? vm->script_args[idx] : vundef();
  { int aidx=argument_index(nm);   /* `argumentN[idx]`: index INTO an array-valued argument (distinct from
       `argument[idx]`, the Nth arg). Missing this, serialize's `with(actions[i])` over an array passed as
       argument0 read 0 for every element, so every input binding serialised to "" and lost its default key. */
    if(aidx>=0){ GmlVal av=(aidx<vm->script_argc)? vm->script_args[aidx] : vundef();
      if(av.t==V_ARR && av.arr){ GmlVal nested; if(arr_nested_get_flat(av,idx,&nested)) return nested;
        GmlArr *A=av.arr; if(idx>=0 && idx<A->len) return A->data[idx]; }
      return vreal(0); } }
  if(!strcmp(nm,"alarm")){ GmlInstance *s=resolve_inst(vm,inst_t);
    return vreal((s&&idx>=0&&idx<GML_ALARMS)? s->alarm[idx] : -1); }
  if(var_name_maybe_special(nm,nh) && !is_room_global_array(nm) &&
     inst_t!=IT_GLOBAL && inst_t!=IT_LOCAL){
    GmlInstance *s=resolve_inst(vm,inst_t); GmlVal out;
    if(s && !inst_is_struct_ref(s) && inst_builtin_get(s,nm,&out)) return out;
  }
  GmlVarMap *m=is_room_global_array(nm)? &vm->globals : scope_map(vm,locals,inst_t); if(!m) return vreal(0);
  GmlVal *slot=gml_varmap_get_h(m,nm,nh);
  if(!slot && !is_room_global_array(nm) && inst_t!=IT_GLOBAL && inst_t!=IT_LOCAL && inst_t!=IT_STATIC){
    GmlInstance *owner=resolve_inst(vm,inst_t);
    if(inst_is_struct_ref(owner)) slot=struct_field_get_h(vm,owner,nm,nh);
  }
  if(!slot||slot->t!=V_ARR) return vreal(0);
  GmlArr *A=slot->arr;
  /* defend against a corrupt/garbage GmlArr (e.g. a cross-version savestate) — never deref blindly */
  if(!A || !A->data || A->len<0 || A->cap<A->len || A->cap>16000000) return vreal(0);
  { GmlVal nested; if(arr_nested_get_flat(*slot,idx,&nested)) return nested; }
  return (idx>=0 && idx<A->len)? A->data[idx] : vreal(0);
}
static GmlVal array_get_inst_field_h(GmlVM *vm, GmlInstance *s, const char *nm, uint32_t nh, int idx){
  if(!s) return vreal(0);
  if(s->obj>=0 && !strcmp(nm,"alarm"))
    return vreal((idx>=0 && idx<GML_ALARMS)? s->alarm[idx] : -1);
  GmlVal out;
  if(!inst_is_struct_ref(s) && inst_builtin_get(s,nm,&out)) return out;
  GmlVal *slot=inst_is_struct_ref(s)?struct_field_get_h(vm,s,nm,nh)
                                         :gml_varmap_get_h(&s->vars,nm,nh);
  if(!slot || slot->t!=V_ARR || !slot->arr) return vreal(0);
  GmlArr *A=slot->arr;
  if(!A || !A->data || A->len<0 || A->cap<A->len || A->cap>16000000) return vreal(0);
  { GmlVal nested; if(arr_nested_get_flat(*slot,idx,&nested)) return nested; }
  return (idx>=0 && idx<A->len)? A->data[idx] : vreal(0);
}
static void array_set_inst_field_h(GmlVM *vm,GmlInstance *s,const char *nm,uint32_t nh,int idx,GmlVal v){
  if(!s) return;
  { static const char *debug_name=(const char*)-1;
    if(debug_name==(const char*)-1) debug_name=getenv("GML_DBG_ARRAYSET");
    if(debug_name && nm && !strcmp(debug_name,nm)){
      extern long g_vm_frame;
      fprintf(stderr,"[arrayset] f%ld instance=%u object=%d %s[%d] type=%d value=%.17g\n",
              g_vm_frame,s->id,s->obj,nm,idx,v.t,v.t==V_REAL?v.d:0.0);
    }
  }
  if(s->obj>=0 && !strcmp(nm,"alarm")){
    if(idx>=0 && idx<GML_ALARMS) s->alarm[idx]=alarm_store_value(vm,v);
    return;
  }
  if(!inst_is_struct_ref(s) && inst_builtin_set(s,nm,v)) return;
  gml_arr_mark_escaped(v);
  GmlVal *slot=gml_varmap_put_h(&s->vars,nm,nh);
  GmlArr *A=arr_of(slot);
  A->escaped=1;
  if(arr_nested_set_flat(*slot,idx,v)) return;
  arr_note_2d_set(A,idx);
  arr_ensure(A,idx);
  if(idx>=0 && idx<A->cap) A->data[idx]=v;
}
static int inst_is_struct_ref(const GmlInstance *in){
  return in && GML_IS_STRUCT_ID((double)in->id);
}
/* Constructor statics form the shared prototype of every struct produced by that constructor.
 * Instance fields shadow them; a constructor-less data struct has no prototype fallback. */
static GmlVal *struct_field_get_h(GmlVM *vm, GmlInstance *in, const char *nm, uint32_t nh){
  if(!vm || !inst_is_struct_ref(in) || !nm) return NULL;
  GmlVal *own=gml_varmap_get_h(&in->vars,nm,nh);
  if(own) return own;
  GmlVal *constructor=gml_varmap_get_h(&in->vars,"__ctor",GML_HASH_CONSTRUCTOR);
  int ci=(constructor && constructor->t==V_REAL)?(int)constructor->d:-1;
  if(ci<0 || ci>=vm->code_static_count || !vm->code_static) return NULL;
  return gml_varmap_get_h(&vm->code_static[ci],nm,nh);
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
  GmlVal *pf=gml_varmap_get_h(&bm->vars,"__fn",GML_HASH_METHOD_FN);
  if(!pf) return 0;
  GmlVal *ps=gml_varmap_get_h(&bm->vars,"__self",GML_HASH_METHOD_SELF);
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
  GmlVal o; if(inst_builtin_get(t,nm,&o)) return o;
  /* Same sprite-derived builtins var_get resolves for `self.X`, so a REFERENCED instance
   * (`other.image_number`, `foo.bbox_left`) reads them too — not 0. `expr.image_number` returning
   * 0 made an animation-gated cutscene wait forever on `floor(image_index)==image_number-1` (= -1). */
  if(!strcmp(nm,"image_number")){ GmlRender *R=(GmlRender*)vm->render;
    return vreal(R? gml_sprite_frames(R,(int)t->sprite_index):0); }
  if(inst_sprite_metric_get(vm,t,nm,&o)) return o;
  if(!strncmp(nm,"bbox_",5)){ double l,tp,r,b;
    if(vm_bbox(vm,t,&l,&tp,&r,&b)){
      if(!vm->win || !vm->win->classic_version){ r+=1.0; b+=1.0; }
      if(!strcmp(nm,"bbox_left"))   return vreal(l);
      if(!strcmp(nm,"bbox_right"))  return vreal(r);
      if(!strcmp(nm,"bbox_top"))    return vreal(tp);
      if(!strcmp(nm,"bbox_bottom")) return vreal(b); }
    return vreal(0); }
  GmlVal *p=gml_varmap_get_h(&t->vars,nm,nh); return p?*p:vreal(0);
}
static void inst_set_any_h(GmlInstance *t, const char *nm, uint32_t nh, GmlVal v){
  gml_arr_mark_escaped(v);   /* instance vars outlive the current scope */
  if(inst_is_struct_ref(t)){ method_cache_invalidate(t,nm); *gml_varmap_put_h(&t->vars,nm,nh)=v; return; }
  if(inst_builtin_set(t,nm,v)) return;
  *gml_varmap_put_h(&t->vars,nm,nh)=v;
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
  /* roots: globals, function statics, every instance (active AND deactivated — the latter can be
   * reactivated), the argument register, and all GmlVal-bearing ds containers. */
  gc_scan_vm(vm,&vm->globals,&wl,&wn,&wcap);
  for(int i=0;i<vm->code_static_count;i++) gc_scan_vm(vm,&vm->code_static[i],&wl,&wn,&wcap);
  for(int i=0;i<vm->inst_count;i++) gc_scan_vm(vm,&vm->inst[i].vars,&wl,&wn,&wcap);
  for(int i=0;i<16;i++) gc_scan_val(vm,vm->script_args[i],&wl,&wn,&wcap);
  for(int i=0;i<GML_DS_MAP_MAX;i++) if(vm->ds_map[i].live){ GmlDSMap *m=&vm->ds_map[i];
    for(int j=0;j<m->len;j++){ gc_scan_val(vm,m->entry[j].key_val,&wl,&wn,&wcap); gc_scan_val(vm,m->entry[j].val,&wl,&wn,&wcap); } }
  for(int i=0;i<GML_DS_LIST_MAX;i++) if(vm->ds_list[i].live){ GmlDSList *l=&vm->ds_list[i];
    for(int j=0;j<l->len;j++) gc_scan_val(vm,l->item[j],&wl,&wn,&wcap); }
  for(int i=0;i<GML_DS_GRID_MAX;i++) if(vm->ds_grid[i].live){ GmlDSGrid *g=&vm->ds_grid[i];
    long cells=(long)g->w*g->h; for(long j=0;j<cells;j++) gc_scan_val(vm,g->cell[j],&wl,&wn,&wcap); }
  for(int i=0;i<GML_TIME_SOURCE_MAX;i++) if(vm->time_source[i].live){
    gc_scan_val(vm,vm->time_source[i].callback,&wl,&wn,&wcap);
    gc_scan_val(vm,vm->time_source[i].args,&wl,&wn,&wcap);
  }
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
static int inst_has_any_h(GmlVM *vm, GmlInstance *t, const char *nm, uint32_t nh){
  if(!t || !nm) return 0;
  if(inst_is_struct_ref(t)) return struct_field_get_h(vm,t,nm,nh)!=NULL;
  GmlVal o;
  if(inst_builtin_get(t,nm,&o)) return 1;
  if(!strcmp(nm,"image_number")) return 1;
  if(inst_sprite_metric_get(vm,t,nm,&o)) return 1;
  if(!strcmp(nm,"bbox_left")||!strcmp(nm,"bbox_right")||
     !strcmp(nm,"bbox_top") ||!strcmp(nm,"bbox_bottom")) return 1;
  return gml_varmap_get_h(&t->vars,nm,nh)!=NULL;
}
int gml_inst_var_exists(GmlVM *vm, GmlVal ref, const char *name){
  if(!vm || !name) return 0;
  GmlInstance *t=vm_inst_from_ref(vm,ref);
  return inst_has_any_h(vm,t,name,strhash(name));
}
GmlVal gml_inst_var_get_val(GmlVM *vm, GmlVal ref, const char *name, int *ok){
  if(ok) *ok=0;
  if(!vm || !name) return vundef();
  uint32_t nh=strhash(name);
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
  uint32_t nh=strhash(name);
  if(inst_is_struct_ref(t)){
    method_cache_invalidate(t,name);
    GmlVal *p=gml_varmap_get_h(&t->vars,name,nh);
    if(p) *p=v;
    else {
      char *owned=strdup(name);
      if(!owned) return 0;
      *gml_varmap_put_owned_h(&t->vars,owned,strhash(owned))=v;
    }
    return 1;
  }
  if(inst_builtin_set(t,name,v)) return 1;
  GmlVal *p=gml_varmap_get_h(&t->vars,name,nh);
  if(p) *p=v;
  else {
    char *owned=strdup(name);
    if(!owned) return 0;
    *gml_varmap_put_owned_h(&t->vars,owned,strhash(owned))=v;
  }
  return 1;
}
/* public accessor: read a builtin or custom instance variable by name → real value.
 * Returns 0 for absent variables (GM default). */
double gml_inst_var_get(GmlVM *vm, GmlInstance *in, const char *nm){
  if(!in) return 0;
  if(inst_is_struct_ref(in)){ GmlVal *p=gml_varmap_get(&in->vars,nm); return p?(p->t==V_REAL?p->d:0):0; }
  GmlVal o; if(inst_builtin_get(in,nm,&o)) return o.t==V_REAL?o.d:0;
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
        uint32_t h=strhash(nm)&(cap-1);
        while(w->code_hix[h]>=0){
          if(!strcmp(w->code[w->code_hix[h]].name,nm)) break; /* preserve first duplicate */
          h=(h+1)&(cap-1);
        }
        if(w->code_hix[h]<0) w->code_hix[h]=i;
      }
    }
  }
  if(w->code_hix && w->code_hix_cap){
    uint32_t h=strhash(name)&(w->code_hix_cap-1);
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
static const char *micro_debug_filter(void){
  static const char *f=(const char*)-1;
  if(f==(const char*)-1){ f=getenv("GML_DBG_MICRO"); if(!f) f=""; }
  return f;
}
static void micro_debug(const GmlCode *c, const char *where){
  const char *f=micro_debug_filter();
  if(f && *f && c && c->name && strstr(c->name,f))
    fprintf(stderr,"[micro] %s kind=%u n=%u %s\n",where?where:"?",(unsigned)c->micro_kind,(unsigned)c->n_insn,c->name);
}
static void micro_debug_pc(const GmlCode *c, const char *where, uint32_t pc, uint32_t aux){
  const char *f=micro_debug_filter();
  if(f && *f && c && c->name && strstr(c->name,f))
    fprintf(stderr,"[micro] %s pc=%u aux=%u n=%u %s\n",where?where:"?",pc,aux,(unsigned)c->n_insn,c->name);
}
static void code_cache_analyze_micro(GmlCode *c){
  if(!c) return;
  c->micro_kind=0; c->micro_name=NULL; c->micro_hash=0;
  if(c->n_insn<4 || !c->insn){ micro_debug(c,"short"); return; }
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
    c->micro_hash=in[1].refhash?in[1].refhash:strhash(in[1].refname);
    micro_debug(c,"ds-map-global-arg0");
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
    micro_debug(c,"approach3");
    return;
  }
  if(c->n_insn>=4 &&
     insn_push_arg(&in[0],0) &&
     in[1].kind==OP_PUSH && in[1].type1==DT_VAR && in[1].inst==IT_GLOBAL && in[1].refname &&
     in[2].kind==OP_CALL && in[2].argc==2 && in[2].refname &&
     in[3].kind==OP_RET){
    c->micro_kind=GML_MICRO_CALL_GLOBAL_ARG0;
    c->micro_name=in[1].refname;
    c->micro_hash=in[1].refhash?in[1].refhash:strhash(in[1].refname);
    micro_debug(c,"call-global-arg0");
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
    c->micro_hash=in[10].refhash?in[10].refhash:strhash(in[10].refname);
    micro_debug(c,"ds-map-method-loop1");
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
    c->micro_hash=in[17].refhash?in[17].refhash:strhash(in[17].refname);
    micro_debug(c,"array-method-flags");
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
    c->micro_hash=in[6].refhash?in[6].refhash:strhash(in[6].refname);
    micro_debug(c,"input-action-update");
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
      c->micro_hash=in[2].refhash?in[2].refhash:strhash(in[2].refname);
      micro_debug(c,"ds-map-nested-fallback");
      return;
    }
  }
  micro_debug(c,"none");
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
    if(n>=max || pc>w->size || 4u>w->size-pc){ c->cache_bad=1; micro_debug_pc(c,"cache-decode-bounds",pc,n); goto fail; }
    GmlInsn in; int sz=gml_decode_bc(w->data,pc,w->bytecode,&in);
    if(!sz){ c->cache_bad=1; micro_debug_pc(c,"cache-decode-fail",pc,0); goto fail; }
    if((uint32_t)sz>end-pc){
      /* Some GMS2 parent entries end their recorded range in the middle of an embedded child body.
       * Normal execution exits or branches away before that tail; treat it as the cached exit edge. */
      micro_debug_pc(c,"cache-truncated-tail",pc,(uint32_t)sz);
      break;
    }
    in.funcval_ci=-1;
    in.builtin_id=0;
    if((in.kind==OP_CALL || in.kind==OP_PUSH || in.kind==OP_POP ||
        (in.kind==OP_BREAK && in.sval==-11)) && in.refaddr){
      in.refname=gml_ref_name(w,in.refaddr);
      if(in.refname) in.refhash=strhash(in.refname);
    }
    if(in.kind==OP_PUSH && in.type1==DT_INT32 && w->bytecode>=17){
      /* resolve once at decode: -2 = checked, NOT a function-value. Leaving it -1 made the
       * interpreter redo the ref-chain walk + name lookup on every plain push.i32 execution. */
      in.funcval_ci=-2;
      const char *fn=gml_ref_name(w,pc+4);
      if(fn && fn[0]!='?' && !strncmp(fn,"gml_",4)){
        int fci=gml_code_index_by_name(w,fn);
        if(fci>=0) in.funcval_ci=fci;
      }
    }
    if(in.kind==OP_BREAK && in.sval==-11 && w->bytecode>=17){
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
    if(target64<(int64_t)c->start){ c->cache_bad=1; micro_debug_pc(c,"cache-branch-before",pcs[i],(uint32_t)i); goto fail_live; }
    int ti=code_cache_find_pc(c,(uint32_t)target64);
    if(ti<0){ c->cache_bad=1; micro_debug_pc(c,"cache-branch-miss",pcs[i],(uint32_t)target64); goto fail_live; }
    br[i]=ti;
  }
  code_cache_analyze_micro(c);
  return 1;
fail_live:
  code_cache_free(c);
  return 0;
fail:
  free(ins); free(pcs); free(br);
  return 0;
}

typedef struct { int ci; const char *name; long calls; uint64_t insn; double ms; } GmlCodeProfSlot;
#define CODEPROF_MAX 192
static GmlCodeProfSlot g_codeprof[CODEPROF_MAX];
static int g_codeprof_n;
static long g_codeprof_last_frame=-1;
static int codeprof_on(void){ static int on=-1; if(on<0) on=getenv("GML_PROFILE_CODE")!=NULL; return on; }
static double codeprof_now_ms(void){
  return vm_profile_now_ms();
}
static void codeprof_add(GmlWin *w, int ci, double ms, uint64_t insn){
  if(!codeprof_on() || !w || ci<0 || ci>=w->n_code) return;
  int slot=-1;
  for(int i=0;i<g_codeprof_n;i++) if(g_codeprof[i].ci==ci){ slot=i; break; }
  if(slot<0){
    slot = g_codeprof_n<CODEPROF_MAX ? g_codeprof_n++ : CODEPROF_MAX-1;
    g_codeprof[slot]=(GmlCodeProfSlot){ci,w->code[ci].name,0,0,0};
  }
  g_codeprof[slot].calls++;
  g_codeprof[slot].insn += insn;
  g_codeprof[slot].ms += ms;
  extern long g_vm_frame;
  if(g_vm_frame<=0 || g_vm_frame==g_codeprof_last_frame || g_vm_frame%300) return;
  g_codeprof_last_frame=g_vm_frame;
  fprintf(stderr,"[codeprof] f=%ld top:\n",g_vm_frame);
  int used[20]; for(int i=0;i<20;i++) used[i]=-1;
  for(int rank=0; rank<20; rank++){
    int best=-1;
    for(int i=0;i<g_codeprof_n;i++){
      int seen=0; for(int j=0;j<rank;j++) if(used[j]==i){ seen=1; break; }
      if(!seen && (best<0 || g_codeprof[i].ms>g_codeprof[best].ms)) best=i;
    }
    if(best<0 || g_codeprof[best].ms<=0) break;
    used[rank]=best;
    fprintf(stderr,"[codeprof]   %8.2fms %7ld calls %10llu insn %s\n",
            g_codeprof[best].ms,g_codeprof[best].calls,
            (unsigned long long)g_codeprof[best].insn,
            g_codeprof[best].name?g_codeprof[best].name:"?");
  }
  memset(g_codeprof,0,sizeof g_codeprof);
  g_codeprof_n=0;
}

/* ---------------- builtins ---------------- */
static int g_unknown_logged=0;
extern GmlVal gml_builtin_call(GmlVM *vm, const char *name, GmlVal *a, int n);
extern int gml_builtin_fast_id(const char *name);
extern GmlVal gml_builtin_call_fast_id(GmlVM *vm, int id, const char *nm, GmlVal *a, int n);
extern int gml_input_key(int vk, int edge);
extern int gml_input_gamepad(int button, int edge);
extern void gml_input_key_clear(int vk);
static int code_micro_maybe(GmlVM *vm, int ci, GmlVal *args, int n_args, GmlVal *out);
static inline int builtin_hotprof_on(void){
  static int on=-1;
  if(on<0) on=getenv("GML_PROFILE_HOTBUILTIN")!=NULL;
  return on;
}
static int vm_heap_string(GmlVal v){
  return v.t==V_STR && v.s && v.d!=0;
}
static void micro_set_bool_fields(GmlInstance *self,
                                  const GmlInsn *pressed_in, const GmlInsn *held_in, const GmlInsn *released_in,
                                  int pressed, int held, int released){
  inst_set_any_h(self,pressed_in->refname,pressed_in->refhash?pressed_in->refhash:strhash(pressed_in->refname),vreal(pressed?1:0));
  inst_set_any_h(self,held_in->refname,held_in->refhash?held_in->refhash:strhash(held_in->refname),vreal(held?1:0));
  inst_set_any_h(self,released_in->refname,released_in->refhash?released_in->refhash:strhash(released_in->refname),vreal(released?1:0));
}
static int micro_input_edge(GmlVM *vm, int type, int key, int edge){
  return type==0 ? gml_keyboard_check(vm,key,edge) : gml_input_gamepad(key,edge);
}
static void micro_call_method_field1(GmlVM *vm, GmlVal targetv, const char *field, uint32_t hash, GmlVal arg0){
  if(!vm || !field) return;
  GmlInstance *target=vm_inst_from_ref(vm,targetv);
  if(!target) return;
  GmlVal mv=inst_get_any_h(vm,target,field,hash);
  int fci=-1;
  GmlInstance *call_self=vm->cur_self;
  double fn=asnum(mv);
  if(GML_IS_STRUCT_ID(fn)){
    GmlInstance *bm=gml_struct_find(vm,(unsigned)fn);
    if(bm){
      GmlVal selfv; int have_self=0;
      method_struct_info(bm,&fci,&selfv,&have_self);
      if(have_self){
        GmlInstance *bs=vm_inst_from_ref(vm,selfv);
        if(bs) call_self=bs;
      }
    }
  } else if(GML_IS_FUNCVAL((int)fn)){
    fci=(int)fn & 0x00FFFFFF;
  }
  if(fci<0 || !vm->win || fci>=vm->win->n_code) return;
  GmlVal a[1]={arg0};
  GmlVal rv;
  GmlInstance *old_self=vm->cur_self;
  vm->cur_self=call_self;
  int micro_ok=code_micro_maybe(vm,fci,a,1,&rv);
  vm->cur_self=old_self;
  if(!micro_ok) rv=gml_vm_run_code(vm,fci,call_self,vm->cur_other,a,1);
  if(vm_heap_string(rv) && !(arg0.t==V_STR && arg0.s==rv.s)) free((char*)rv.s);
}
static int code_micro_try(GmlVM *vm, int ci, GmlVal *args, int n_args, GmlVal *out){
  if(!vm || !vm->win || ci<0 || ci>=vm->win->n_code || !out) return 0;
  GmlCode *c=&vm->win->code[ci];
  const char *trace=getenv("GML_TRACE");
  if(trace && *trace && c->name && strstr(c->name,trace)) return 0;
  if(!code_cache_ensure(vm->win,ci)) return 0;
  if(c->micro_kind==GML_MICRO_NONE) return 0;
  if(builtin_hotprof_on()) return 0;
  const char *arglog=getenv("GML_LOG_CODE_ARGS");
  if(arglog && *arglog && c->name && strstr(c->name,arglog)) return 0;
  double t0=codeprof_on()?codeprof_now_ms():0.0;
  if(c->micro_kind==GML_MICRO_DS_MAP_GLOBAL_ARG0 && c->micro_name){
    GmlVal *map=gml_varmap_get_h(&vm->globals,c->micro_name,c->micro_hash);
    *out = gml_ds_map_find_value_direct(vm,(int)(map?asnum(*map):0.0),
      (args && n_args>0)?args[0]:vundef(), args && n_args>0);
    gml_arr_mark_escaped(*out);
    if(codeprof_on()) codeprof_add(vm->win,ci,codeprof_now_ms()-t0,4);
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
    if(codeprof_on()) codeprof_add(vm->win,ci,codeprof_now_ms()-t0,28);
    return 1;
  }
  if(c->micro_kind==GML_MICRO_CALL_GLOBAL_ARG0 && c->micro_name && c->insn){
    int fci=c->insn[2].funcval_ci;
    if(fci==-1){
      fci=gml_code_index_by_name(vm->win,c->insn[2].refname);
      c->insn[2].funcval_ci=(fci>=0)?fci:-2;
    }
    if(fci>=0 && fci<vm->win->n_code){
      GmlVal *gv=gml_varmap_get_h(&vm->globals,c->micro_name,c->micro_hash);
      GmlVal a[2]={ gv?*gv:vreal(0), (args && n_args>0)?args[0]:vundef() };
      *out=gml_vm_run_code(vm,fci,vm->cur_self,vm->cur_other,a,2);
      if(codeprof_on()) codeprof_add(vm->win,ci,codeprof_now_ms()-t0,4);
      return 1;
    }
  }
  if(c->micro_kind==GML_MICRO_DS_MAP_METHOD_LOOP1 && c->micro_name && c->insn){
    GmlVal arg0=(args && n_args>0)?args[0]:vundef();
    if(arg0.t==V_UNDEF) arg0=vreal(-1);
    GmlInsn *in=c->insn;
    GmlVal *dz=gml_varmap_get_h(&vm->globals,in[6].refname,in[6].refhash?in[6].refhash:strhash(in[6].refname));
    gml_gamepad_set_axis_deadzone_direct((int)asnum(arg0),dz?asnum(*dz):0.0);
    GmlVal *mapv=gml_varmap_get_h(&vm->globals,c->micro_name,c->micro_hash);
    int mapid=(int)(mapv?asnum(*mapv):0.0);
    GmlVal key=gml_ds_map_find_first_direct(vm,mapid);
    int num=gml_ds_map_size_direct(vm,mapid);
    const char *method=in[28].refname;
    uint32_t method_hash=in[28].refhash?in[28].refhash:strhash(method);
    for(int i=0;i<num;i++){
      GmlVal target=gml_ds_map_find_value_direct(vm,mapid,key,1);
      micro_call_method_field1(vm,target,method,method_hash,arg0);
      key=gml_ds_map_find_next_direct(vm,mapid,key,1);
    }
    *out=vreal(0);
    if(codeprof_on()) codeprof_add(vm->win,ci,codeprof_now_ms()-t0,41);
    return 1;
  }
  if(c->micro_kind==GML_MICRO_DS_MAP_NESTED_FALLBACK && c->micro_name && c->insn){
    if(getenv("GML_LOG_DS")) return 0;
    GmlVal key0=(args && n_args>0)?args[0]:vundef();
    GmlVal key1=(args && n_args>1)?args[1]:vundef();
    GmlVal *rootv=gml_varmap_get_h(&vm->globals,c->micro_name,c->micro_hash);
    int root=(int)(rootv?asnum(*rootv):0.0);
    GmlVal inner=gml_ds_map_find_value_direct(vm,root,key0,1);
    if(inner.t!=V_UNDEF){
      GmlVal val=gml_ds_map_find_value_direct(vm,(int)asnum(inner),key1,1);
      if(val.t!=V_UNDEF){
        *out=val;
        gml_arr_mark_escaped(*out);
        if(codeprof_on()) codeprof_add(vm->win,ci,codeprof_now_ms()-t0,16);
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
        if(codeprof_on()) codeprof_add(vm->win,ci,codeprof_now_ms()-t0,16);
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
    uint32_t method_hash=in[30].refhash?in[30].refhash:strhash(method);
    const char *flag0=in[7].refname, *flag1=in[9].refname, *flag2=in[11].refname, *field3=in[13].refname;
    uint32_t flag0_hash=in[7].refhash?in[7].refhash:strhash(flag0);
    uint32_t flag1_hash=in[9].refhash?in[9].refhash:strhash(flag1);
    uint32_t flag2_hash=in[11].refhash?in[11].refhash:strhash(flag2);
    uint32_t field3_hash=in[13].refhash?in[13].refhash:strhash(field3);
    inst_set_any_h(self,flag0,flag0_hash,vreal(0));
    inst_set_any_h(self,flag1,flag1_hash,vreal(0));
    inst_set_any_h(self,flag2,flag2_hash,vreal(0));
    inst_set_any_h(self,field3,field3_hash,vreal(0));
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
          if(astrue(inst_get_any_h(vm,st,flag0,flag0_hash))) inst_set_any_h(self,flag0,flag0_hash,vreal(1));
          if(astrue(inst_get_any_h(vm,st,flag1,flag1_hash))) inst_set_any_h(self,flag1,flag1_hash,vreal(1));
          if(astrue(inst_get_any_h(vm,st,flag2,flag2_hash))) inst_set_any_h(self,flag2,flag2_hash,vreal(1));
        }
        array_set_inst_field_h(vm,self,arr_name,arr_hash,i,item);
      }
    }
    *out=vreal(0);
    if(codeprof_on()) codeprof_add(vm->win,ci,codeprof_now_ms()-t0,67);
    return 1;
  }
  if(c->micro_kind==GML_MICRO_INPUT_ACTION_UPDATE && c->insn){
    GmlInstance *self=vm->cur_self;
    if(!self) return 0;
    GmlInsn *in=c->insn;
    GmlVal typev=inst_get_any_h(vm,self,in[6].refname,in[6].refhash?in[6].refhash:strhash(in[6].refname));
    int type=(int)asnum(typev);
    if(type!=0 && type!=1) return 0;
    if(type==1 && getenv("GML_DBG_GP")) return 0;
    GmlVal value=inst_get_any_h(vm,self,in[20].refname,in[20].refhash?in[20].refhash:strhash(in[20].refname));
    if(value.t==V_ARR && value.arr){
      int a0=(int)asnum(gml_arr_get(value,0));
      int a1=(int)asnum(gml_arr_get(value,1));
      int axis=micro_input_edge(vm,type,a1,0)-micro_input_edge(vm,type,a0,0);
      int pressed=(micro_input_edge(vm,type,a1,1)-micro_input_edge(vm,type,a0,1))!=0;
      int held=axis!=0;
      int released=(axis==0) && astrue(inst_get_any_h(vm,self,in[24].refname,in[24].refhash?in[24].refhash:strhash(in[24].refname)));
      inst_set_any_h(self,in[35].refname,in[35].refhash?in[35].refhash:strhash(in[35].refname),vreal(axis));
      micro_set_bool_fields(self,&in[47],&in[24],&in[60],pressed,held,released);
    } else {
      int key=(int)asnum(value);
      int pressed=micro_input_edge(vm,type,key,1);
      int held=micro_input_edge(vm,type,key,0);
      int released=micro_input_edge(vm,type,key,2);
      micro_set_bool_fields(self,&in[47],&in[24],&in[60],pressed,held,released);
    }
    *out=vreal(0);
    if(codeprof_on()) codeprof_add(vm->win,ci,codeprof_now_ms()-t0,42);
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
  if(!vm || !vm->win || !vm->win->classic_version || !name) return -1;
  if(strcmp(name,"crear") && strcmp(name,"depthy") && strcmp(name,"move_rpg") &&
     strcmp(name,"direction_rpg") && strcmp(name,"friction_platform") &&
     strcmp(name,"destruir") &&
     strcmp(name,"draw_full_sprite") && strcmp(name,"draw_shadow") &&
     strcmp(name,"draw_shadow_ext")) return -1;
  char code_name[192];
  snprintf(code_name,sizeof code_name,"gml_Script_%s",name);
  return gml_code_index_by_name(vm->win,code_name);
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
static int g_run_depth=0;   /* C-recursion guard: gml_vm_run_code re-enters via OP_CALL/OP_CALLV */
static const char *g_recur_chain[300]; static int g_recur_n;   /* names along the active call chain (debug) */

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
  int save_code_index=vm->cur_code_index;
  const char *save_code_name=g_cur_code_name;
  GmlVM *save_cur_vm=g_cur_vm;
  GmlVal save_args[16]; int save_argc=vm->script_argc;
  for(int i=0;i<16;i++) save_args[i]=vm->script_args[i];
  vm->cur_self=self; vm->cur_other=other;
  vm->cur_code_index=ci;
  g_cur_code_name=vm->win->code[ci].name;
  g_cur_vm=vm;
  GmlVarMap locals={0};
  int argc=n_args<0?0:(n_args<16?n_args:16);
  vm->script_argc=argc;
  for(int i=0;i<16;i++) vm->script_args[i]=(i<argc && args)? args[i] : vundef();
  const char *arglog=getenv("GML_LOG_CODE_ARGS");
  if(arglog && *arglog && strstr(w->code[ci].name,arglog)){
    extern long g_vm_frame;
    fprintf(stderr,"[args] f%ld %s argc=%d self=%u obj=%d:",g_vm_frame,w->code[ci].name,argc,self?self->id:0,self?self->obj:-1);
    for(int i=0;i<argc && i<16;i++){ fprintf(stderr," a%d=",i); log_val_simple(vm->script_args[i]); }
    fprintf(stderr,"\n");
  }
  /* Array arguments may alias storage in caller and callee scopes. Mark them escaped
   * so local cleanup leaves shared arrays for deduplicated full teardown. */
  for(int i=0;i<argc;i++) if(vm->script_args[i].t==V_ARR) gml_arr_mark_escaped(vm->script_args[i]);

  GmlVal stk[STK]; uint8_t stkt[STK]; int sp=0;
  /* Preserve the array/index reference at savearef for a later popaf store,
   * restoring the stack position below the saved reference. */
  struct { GmlArr *arr; int idx; int base; } aref[16]; int aref_n=0;
  uint32_t pc=start;
  GmlVal ret=vreal(0);
  int classic_implicit_return=w->classic_version && w->code[ci].name &&
    !strncmp(w->code[ci].name,"gml_Script_",11);
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
  int use_cache = !trace && code_cache_ensure(w,ci);
  GmlInsn *cached_ins = use_cache ? w->code[ci].insn : NULL;
  uint32_t *cached_pc = use_cache ? w->code[ci].insn_pc : NULL;
  int32_t *cached_branch = use_cache ? w->code[ci].branch_index : NULL;
  uint32_t cached_n = use_cache ? w->code[ci].n_insn : 0;
  int cp = codeprof_on();
  int hp_builtin = builtin_hotprof_on();
  double cp_t0 = cp ? codeprof_now_ms() : 0.0;
  /* Watchdog: a single code run should never execute more than a few million instructions. If one
   * blows past a large budget it is a runaway loop (e.g. a control-flow condition corrupted by an
   * unimplemented opcode) — abort the run instead of freezing the whole frontend. Real per-event
   * code, even heavy tile/particle loops, stays orders of magnitude under this. */
  uint64_t watchdog=0;
  const uint64_t WATCHDOG_MAX=64000000ull;
  uint32_t ip=0;
  while(use_cache ? ip<cached_n : pc<end){
    if(++watchdog>WATCHDOG_MAX){
      static int warned=0;
      if(warned<4){ warned++; extern long g_vm_frame;
        fprintf(stderr,"[gml] f%ld VM watchdog tripped in '%s' at offset %u (runaway loop) — aborting run\n",
          g_vm_frame, w->code[ci].name, pc-start); }
      break;
    }
    GmlInsn in; GmlInsn *pin=NULL; uint32_t nextpc; uint32_t nextip=ip+1;
    if(use_cache){
      pin=&cached_ins[ip];
      in=*pin;
      pc=cached_pc[ip];
      nextpc=pc+in.size;
    } else {
      int sz=gml_decode_bc(d,pc,w->bytecode,&in); if(!sz) break;
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
      if(gml_decode_bc(d,pc-4,w->bytecode,&prev)==4)
        prev_conv_v_i32 = prev.kind==OP_CONV && prev.type1==DT_VAR && prev.type2==DT_INT32;
    }
    if(trace){
      const char *rn = (in.kind==OP_CALL || in.kind==OP_PUSH || in.kind==OP_POP) ? (in.refname?in.refname:gml_ref_name(w,in.refaddr)) : "";
      fprintf(stderr,"  %4u: %-7s t1=%x rt=%02x inst=%d  sp=%d %s\n",pc-start,gml_op_mnemonic(in.kind),in.type1,in.reftype,in.inst,sp,rn); }
    { static int init=0; static char namebuf[128]; static long off=-1; static int maxlog=0, count=0;
      if(!init){
        const char *e=getenv("GML_LOG_PC"); init=1;
        if(e && *e){
          const char *colon=strrchr(e,':'); size_t n=colon?(size_t)(colon-e):strlen(e);
          if(n>=sizeof namebuf) n=sizeof namebuf-1;
          memcpy(namebuf,e,n); namebuf[n]=0; off=colon?strtol(colon+1,NULL,0):-1;
          const char *m=getenv("GML_LOG_PC_MAX"); maxlog=m?atoi(m):200;
          if(maxlog<=0) maxlog=200;
        }
      }
      if(namebuf[0] && count<maxlog && strstr(w->code[ci].name,namebuf) && (off<0 || (long)(pc-start)==off)){
        extern long g_vm_frame;
        GmlInstance *dbg_self=vm->cur_self, *dbg_other=vm->cur_other;
        const char *dbg_self_name=(dbg_self && dbg_self->obj>=0 && dbg_self->obj<vm->n_objects)
          ? vm->objects[dbg_self->obj].name : "?";
        const char *dbg_other_name=(dbg_other && dbg_other->obj>=0 && dbg_other->obj<vm->n_objects)
          ? vm->objects[dbg_other->obj].name : "?";
        fprintf(stderr,"[pc] f%ld %s+%u sp=%d self=%s/%u(hsp=%.3f,xs=%.1f) other=%s/%u(hsp=%.3f,xs=%.1f) top=",
          g_vm_frame,w->code[ci].name,pc-start,sp,
          dbg_self_name?dbg_self_name:"?",dbg_self?dbg_self->id:0,
          dbg_self?dbg_self->hspeed:0.0,dbg_self?dbg_self->image_xscale:0.0,
          dbg_other_name?dbg_other_name:"?",dbg_other?dbg_other->id:0,
          dbg_other?dbg_other->hspeed:0.0,dbg_other?dbg_other->image_xscale:0.0);
        if(sp>0) log_val_simple(stk[sp-1]); else fprintf(stderr,"<empty>");
        fprintf(stderr," stack[");
        int stack_first=sp>8?sp-8:0;
        for(int dbg_i=stack_first;dbg_i<sp;dbg_i++){
          if(dbg_i>stack_first) fputc(',',stderr);
          log_val_simple(stk[dbg_i]);
        }
        fprintf(stderr,"]");
        fprintf(stderr,"\n");
        count++;
      } }
    switch(in.kind){
      case OP_PUSH:{
        GmlVal v;
        if(in.type1==DT_INT16) v=vreal(in.sval);
        else if(in.type1==DT_DOUBLE) v=vreal(in.dval);
        else if(in.type1==DT_INT32){ v=vreal(in.ival);
          /* GMS2.3 function-value: a `push.i32` whose reference word (pc+4) resolves via the FUNC
           * occurrence chain to a script code-entry is pushing that function as a value (later called
           * by OP_CALLV or bound by method()). Tag it so OP_CALLV can dispatch the code entry. */
          if(w->bytecode>=17){
            int fci=use_cache ? in.funcval_ci : -1;
            const char *fn=NULL;
            if(fci==-1){   /* -2 = decode already determined it is not a function-value */
              fn=gml_ref_name(w,pc+4);
              if(fn && fn[0]!='?' && !strncmp(fn,"gml_",4)) fci=gml_code_index_by_name(w,fn);
            }
            if(fci>=0){ v=vreal((double)(GML_FUNCVAL_TAG|fci));
              static int dbg_funcval=-1; if(dbg_funcval<0) dbg_funcval=getenv("GML_DBG_FUNCVAL")!=NULL;
              if(dbg_funcval) fprintf(stderr,"[funcval] %s pc=%u i32=%d -> %s (ci=%d)\n",
                w->code[ci].name,pc-start,in.ival,fn?fn:(w->code[fci].name?w->code[fci].name:"?"),fci); } } }
        else if(in.type1==DT_INT64) v=vreal((double)in.lval);
        else if(in.type1==DT_STRING) v=vstr(gml_str_by_index(w,in.strindex));
        else if(in.type1==DT_VAR){
          const char *nm=in.refname?in.refname:gml_ref_name(w,in.refaddr);
          uint32_t nh=in.refhash?in.refhash:strhash(nm);
          if(in.reftype==0x00){ /* Array */
            int idx=(int)(sp>0?asnum(stk[--sp]):0);
            GmlVal itv=sp>0?stk[--sp]:vreal(0);
            if(w->bytecode>=17 && sp>0 && itv.t==V_REAL && itv.d==-9.0){
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
            if(w->bytecode>=17 && sp>0 && iv.t==V_REAL && iv.d==-9.0) iv=stk[--sp];
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
              if(in.reftype==0x90){ GmlVal *slot=gml_varmap_put_h(m,nm,nh); GmlArr *A=arr_of(slot);
                if(m!=&locals) A->escaped=1;
                arr_ensure(A,idx);
                if(idx<A->cap){ if(A->data[idx].t!=V_ARR){ A->data[idx].t=V_ARR; A->data[idx].arr=calloc(1,sizeof(GmlArr));
                    if(A->escaped && A->data[idx].arr) ((GmlArr*)A->data[idx].arr)->escaped=1; }
                  v=A->data[idx]; } }
              else { GmlVal *slot=gml_varmap_get_h(m,nm,nh);
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
            GmlInstance *t=vm_inst_from_ref(vm,iv); v=t?inst_get_any_h(vm,t,nm,nh):vreal(0);
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
            else { GmlVal *pp=gml_varmap_get_h(&locals,nm,nh); v=pp?*pp:vreal(0); }
          } else v=var_get_h(vm,in.inst,nm,nh);
        } else v=vreal(0);
        if(sp<STK){ stk[sp]=v; stkt[sp]=in.type1; sp++; }
        break;
      }
      case OP_POP:{
        const char *nm=in.refname?in.refname:gml_ref_name(w,in.refaddr);
        uint32_t nh=in.refhash?in.refhash:strhash(nm);
        if(in.reftype==0x00){ /* Direct array stores consume value/scope/index; numeric compound stores
           * consume scope/index/value. Select the order from Type1. */
          int idx; GmlVal itv, val, iv=vreal(0), scopev; GmlInstance *t=NULL;
          if(in.type1==DT_VAR){
            idx=(int)(sp>0?asnum(stk[--sp]):0);
            itv=sp>0?stk[--sp]:vreal(0);
            scopev=itv;
            if(w->bytecode>=17 && sp>0 && itv.t==V_REAL && itv.d==-9.0){
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
            if(w->bytecode>=17 && sp>0 && itv.t==V_REAL && itv.d==-9.0){
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
             if(w->bytecode>=17 && sp>0 && stk[sp-1].t==V_REAL && stk[sp-1].d==-9.0) --sp;
             iv=sp>0?stk[--sp]:vreal(0); val=sp>0?stk[--sp]:vreal(0);
           } else {                                     /* [instance, (-9), value] — value on top */
             val=sp>0?stk[--sp]:vreal(0);
             if(w->bytecode>=17 && sp>0 && stk[sp-1].t==V_REAL && stk[sp-1].d==-9.0) --sp;
             iv=sp>0?stk[--sp]:vreal(0);
           }
           GC_PERSIST(val);
           if(classic_implicit_return) ret=val;
           int it=(int)asnum(iv);
           if(iv.t==V_REAL && !GML_IS_STRUCT_ID(iv.d) && it<100000)
             var_set_h(vm,it,nm,nh,val);
           else {
             GmlInstance *t=vm_inst_from_ref(vm,iv); if(t) inst_set_any_h(t,nm,nh,val);
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
              GmlInstance *t=vm_inst_from_ref(vm,iv); if(t) inst_set_any_h(t,nm,nh,v);
            }
            break;
          }
          GmlVal v = sp>0? stk[--sp] : vreal(0);
          /* locals/arguments die at scope exit, so their owned strings stay tracked and are freed
           * then (str_gc). Only instance/global stores persist beyond the run — untrack those so the
           * var owns the string (else it dangles at scope exit; re-assigning it later leaks it). */
          if(in.inst==IT_LOCAL){ if(!argument_set(vm,nm,v)) *gml_varmap_put_h(&locals,nm,nh)=v; }
          else if(in.inst==IT_STACK){   /* `pop.v.v stack.var` — write field on the instance under the value */
            GmlVal iv=sp>0?stk[--sp]:vreal(0); GC_PERSIST(v);
            GmlInstance *t=vm_inst_from_ref(vm,iv); if(t) inst_set_any_h(t,nm,nh,v); }
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
          if(in.type1==DT_VAR && top_units==0) break;
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
        if(sp<2) break; GmlVal r=stk[--sp], l=stk[--sp];
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
        if(sp<2) break; GmlVal r=stk[--sp], l=stk[--sp]; int res=0;
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
          /* Studio 1 bytecode compares reals exactly. The configurable epsilon belongs to
           * expression comparisons in the current format family; applying its 1e-5 default
           * retroactively makes long-running bytecode-15 state machines cross thresholds early. */
          double epsilon=(!w->classic_version && w->bytecode<17)?0.0:vm->math_epsilon;
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
          const char *match=getenv("GML_TRACE_CALL");
          if(match && (!*match || (nm && strstr(nm,match)))){
            extern long g_vm_frame;
            fprintf(stderr,"[call] f%ld code=%s pc=%u name=%s argc=%d\n",
              g_vm_frame,w->code[ci].name?w->code[ci].name:"?",pc,
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
          bid=gml_builtin_fast_id(nm);
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
        } else if(!use_cache && pc>=start+8){
          GmlInsn prev;
          int psz=gml_decode_bc(d,pc-8,w->bytecode,&prev);
          member_value_call=psz==8 && prev.kind==OP_PUSH && prev.type1==DT_VAR &&
                            (prev.inst==IT_STACK || prev.reftype==0x80);
          if(!member_value_call && psz==8 && prev.kind==OP_PUSH && prev.type1==DT_VAR &&
             pc>=start+16){
            GmlInsn scope;
            int ssz=gml_decode_bc(d,pc-16,w->bytecode,&scope);
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
        break;
      }
      case OP_RET: ret = sp>0? stk[--sp]:vreal(0); if(use_cache) ip=cached_n; else pc=end; continue;
      case OP_EXIT:
        if(!classic_implicit_return) ret=vreal(0);
        if(use_cache) ip=cached_n; else pc=end;
        continue;
      case OP_PUSHENV:{ /* with(target): pop the target, iterate matching instances */
        GmlVal tv = sp>0? stk[--sp] : vreal(0);
        /* For the StackTop environment form, consume the -9 sentinel and the target below it. */
        if(w->bytecode>=17 && sp>0 && tv.t==V_REAL && tv.d==-9.0 &&
           pc>=start+4 && u32(d,pc-4)==0x840FFFF7u) tv=stk[--sp];
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
        if(family_target && nn>1 && !w->classic_version)
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
            int size=gml_decode_bc(d,nextpc,w->bytecode,&endenv);
            if(size>0 && endenv.kind==OP_POPENV) nextpc+=(uint32_t)size;
          }
        }
        else if(withsp>=32){
          free(list);
          nextpc = pc + (uint32_t)(in.jump*4);
          if(use_cache){ int target=cached_branch[ip]; nextip=(uint32_t)(target>=0?target+1:target); }
          else { GmlInsn endenv; int size=gml_decode_bc(d,nextpc,w->bytecode,&endenv);
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
        /* Dispatch extended break operations with their corresponding stack operands. */
        switch(in.sval){
          case -11:{ /* pushref: resource id OR function reference, selected by its FUNC occurrence.
                      * Treating every payload as a low-24 resource id left nested callbacks as plain
                      * integers, so a later callv silently had nothing callable to dispatch. */
            int fci=use_cache ? in.funcval_ci : -1;
            if(fci==-1){
              const char *fn=gml_ref_name(w,in.refaddr?in.refaddr:pc+4);
              if(fn && fn[0]!='?' && !strncmp(fn,"gml_",4)) fci=gml_code_index_by_name(w,fn);
            }
            { static int dbg_pushref=-1;
              if(dbg_pushref<0) dbg_pushref=getenv("GML_DBG_FUNCVAL")!=NULL;
              if(dbg_pushref){
                const char *dbgfn=gml_ref_name(w,in.refaddr?in.refaddr:pc+4);
                if((dbgfn && !strncmp(dbgfn,"gml_",4)) || fci>=0) fprintf(stderr,
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
  varmap_free_ex(&locals,1);      /* keep escaped arrays alive — persistent slots alias them */
  vm->cur_self=save_self; vm->cur_other=save_other;
  vm->cur_code_index=save_code_index;
  g_cur_code_name=save_code_name;
  g_cur_vm=save_cur_vm;
  vm->script_argc=save_argc;
  for(int i=0;i<16;i++) vm->script_args[i]=save_args[i];
  (void)g_unknown_logged;
  if(cp) codeprof_add(w,ci,codeprof_now_ms()-cp_t0,watchdog);
  g_run_depth--;
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
    uint32_t actions=u32(d,event_ptr);
    if(actions>0 && actions<=4096 && (size_t)event_ptr+4+(size_t)actions*4<=end){
      for(uint32_t i=0;i<actions;i++){
        uint32_t action_ptr=u32(d,event_ptr+4+i*4);
        if((size_t)action_ptr+36>end) continue;
        uint32_t code=u32(d,action_ptr+32);
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
  const uint8_t *d=w->data; uint32_t base=c->off, n=u32(d,base);
  size_t end=(size_t)c->off+c->size;
  if(n>100000 || (size_t)base+4+(size_t)n*4>end || end>w->size) return;
  int cap=n>0?(int)n:4;
  GmlTimeline *timelines=calloc((size_t)cap,sizeof(*timelines));
  if(!timelines) return;
  for(uint32_t i=0;i<n;i++){
    uint32_t ep=u32(d,base+4+i*4);
    if((size_t)ep+8>end) continue;
    int tagged=(size_t)ep+12<=end && u32(d,ep+4)==0x434C4D54u;
    uint32_t count=u32(d,ep+(tagged?8:4));
    size_t pairs=(size_t)ep+(tagged?12:8);
    if(count>100000 || pairs+(size_t)count*8>end) continue;
    GmlTimeline *t=&timelines[i];
    t->name=gml_str_by_ptr(w,u32(d,ep));
    t->moments=calloc(count?count:1,sizeof(*t->moments));
    if(!t->moments) continue;
    t->n=(int)count; t->last_step=-1;
    for(uint32_t m=0;m<count;m++){
      uint32_t pair=(uint32_t)(pairs+(size_t)m*8);
      t->moments[m].step=(int32_t)u32(d,pair);
      t->moments[m].code=tagged ? (int32_t)u32(d,pair+4) :
        native_timeline_code(w,t->name,(int)m,t->moments[m].step,d,end,u32(d,pair+4));
      if(t->moments[m].step>t->last_step) t->last_step=t->moments[m].step;
    }
    if(getenv("GML_LOG_TIMELINE")) fprintf(stderr,
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
  /* Path interaction with ordinary motion fields differs between format families. Classic and
   * current Studio semantics suspend ordinary velocity while an active path owns movement;
   * Studio 1.x keeps those fields intact and still re-evaluates a zero-speed path after ordinary
   * movement.  Keeping the middle generation explicit is important: treating every non-classic
   * package alike shifts path-driven objects in bytecode-15 content. */
  int path_owns_velocity=vm->win &&
    (vm->win->classic_version || vm->win->bytecode>=17);
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
    /* Path movement owns speed/direction for this step. GameMaker derives direction from the
     * path displacement, then clears the ordinary speed components so a pre-path hspeed is not
     * observable as stale motion (or applied again on the following step). */
    if(path_owns_velocity){
      in->direction=atan2(before_y-new_y,new_x-before_x)*180.0/M_PI;
      if(in->direction<0) in->direction+=360.0;
      if(in->direction>=360.0) in->direction=fmod(in->direction,360.0);
      in->speed=0; in->hspeed=0; in->vspeed=0;
    }
    in->x=new_x; in->y=new_y; gml_colgrid_touch(in);
    path_log_step(vm,in,pi);
    /* GM "End of Path" event (Other, subtype 8): fired when the path reaches its end. Many objects
     * chain off this (an intro object flies in on a path, then a later event spawns the enemy). */
    if(ended){
      uint32_t id=in->id;
      int ea=(int)in->path_endaction;
      double boundary=in->path_position;
      int hit_end=next_pos>=1.0;
      gml_run_event(vm,in,"Other_8");
      in=inst_by_id(vm,(double)id);   /* Other_8 may have moved/killed the instance; refetch by id */
      if(!in||!in->active||in->marked) continue;
      if((int)in->path_index==pi && (int)in->path_endaction==ea && fabs(in->path_position-boundary)<1e-9)
        path_apply_endaction(vm,in,pi,ea,next_pos,hit_end);
    }
  }
}

/* ---------------- object parsing ---------------- */
static void parse_native_object_events(GmlVM *vm, GmlObject *object, uint32_t table,
                                       size_t end){
  const uint8_t *data=vm->win->data;
  if((size_t)table+4>end) return;
  uint32_t types=u32(data,table);
  if(types>32 || (size_t)table+4+(size_t)types*4>end) return;
  int total=0;
  for(uint32_t type=0;type<types;type++){
    uint32_t list=u32(data,table+4+type*4);
    if((size_t)list+4>end) return;
    uint32_t count=u32(data,list);
    if(count>100000 || total>(int)(100000-count) || (size_t)list+4+(size_t)count*4>end) return;
    total+=(int)count;
  }
  if(!total) return;
  object->events=calloc((size_t)total,sizeof(*object->events));
  if(!object->events) return;
  for(uint32_t type=0;type<types;type++){
    uint32_t list=u32(data,table+4+type*4), count=u32(data,list);
    for(uint32_t event_index=0;event_index<count;event_index++){
      uint32_t event=u32(data,list+4+event_index*4);
      if((size_t)event+8>end) continue;
      int subtype=(int32_t)u32(data,event);
      uint32_t actions=u32(data,event+4);
      if(actions>4096 || (size_t)event+8+(size_t)actions*4>end) continue;
      int code=-1;
      for(uint32_t action_index=0;action_index<actions;action_index++){
        uint32_t action=u32(data,event+8+action_index*4);
        if((size_t)action+36>end) continue;
        uint32_t candidate=u32(data,action+32);
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
    /* Physics metadata follows parent/mask in both classic and managed GMS2 OBJT layouts. Retain
     * only the mass inputs needed by the lightweight backend, after validating the variable-length
     * vertex list entirely inside OBJT. Coordinates are fixture-local pixels. */
    uint32_t q=p+(uint32_t)poff+8u;
    uint64_t cend=(uint64_t)c->off+c->size;
    uint32_t nvert=UINT32_MAX;
    if((uint64_t)q+48u<=cend && (uint64_t)q+48u<=w->size){
      uint32_t enabled=u32(d,q), kinematic=u32(d,q+44);
      nvert=u32(d,q+32);
      double density=f32(d,q+12);
      uint64_t vend=(uint64_t)q+48u+(uint64_t)nvert*8u;
      if(enabled<=1 && kinematic<=1 && isfinite(density) && density>=0.0 && density<=1000000.0 &&
         nvert<=128 && vend<=cend && vend<=w->size){
        o->physics_enabled=(int)enabled;
        o->physics_kinematic=(int)kinematic;
        o->physics_density=density;
        if(nvert>=3){
          double twice_area=0.0;
          int valid=1;
          for(uint32_t vi=0;vi<nvert;vi++){
            uint32_t vj=(vi+1u)%nvert;
            double xi=f32(d,q+48u+vi*8u), yi=f32(d,q+52u+vi*8u);
            double xj=f32(d,q+48u+vj*8u), yj=f32(d,q+52u+vj*8u);
            if(!isfinite(xi)||!isfinite(yi)||!isfinite(xj)||!isfinite(yj) ||
               fabs(xi)>1000000.0||fabs(yi)>1000000.0||fabs(xj)>1000000.0||fabs(yj)>1000000.0){
              valid=0; break;
            }
            twice_area+=xi*yj-xj*yi;
          }
          if(valid) o->physics_area_px=fabs(twice_area)*0.5;
        }
      }
    }
    if(nvert<=128){
      uint64_t event_table=(uint64_t)q+48u+(uint64_t)nvert*8u;
      if(event_table+4u<=cend && event_table+4u<=w->size)
        parse_native_object_events(vm,o,(uint32_t)event_table,(size_t)cend);
    }
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
  if(vm && name && var_name_maybe_special(name,strhash(name))){
    GmlVal value=var_get_h(vm,IT_GLOBAL,name,strhash(name));
    if(value.t==V_REAL) return value.d;
  }
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

/* Native OBJT records retain declarations that have no CODE entry.  Walk the same inheritance
 * chain as event dispatch and stop at the first matching declaration: an empty child event is
 * still a declaration (and therefore must not inherit a parent's handler implicitly). */
static int native_event_declared_from(GmlVM *vm, int evtype, int subtype, int obj,
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
  /* Preserve `other`: an event fired from inside another instance's scope
   * (event_user / event_perform / action_inherited) must see the caller as
   * `other`. For an engine-triggered event there is no separate caller; GM
   * exposes the event instance itself as both `self` and `other`. This matters
   * for ordinary Step/Create code that deliberately enters `with(other.id)`. */
  GmlInstance *other=vm->cur_other?vm->cur_other:in;
  /* GML_DBG_EVTIME accumulates wall-clock milliseconds per handler and event, reporting every
   * 300 frames. */
  { static int evt_on=-1; if(evt_on<0) evt_on=getenv("GML_DBG_EVTIME")!=NULL;
    if(evt_on){
      static struct { int obj; char suf[24]; double ms; long runs; } tab[256]; static int ntab=0;
      static long lastf=0, frames=0;
      double t0=vmprof_now();
      int r=run_event_code_from(vm,in,other,suffix,handler_obj,ci);
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
static int classic_collect_object_slots(GmlVM *vm, int object){
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
 * large gaps. Cache the ascending object ids that resolve each event suffix (including inherited
 * handlers): this preserves object-major event order without rescanning every empty slot on
 * every frame. Runtime hierarchy mutation resets this cache before the next dispatch. */
typedef struct { char suffix[32]; int *objects, n, cap, used; } ClassicDispatchCache;
#define CLASSIC_DISPATCH_CACHE_MAX 128
static ClassicDispatchCache g_classic_dispatch[CLASSIC_DISPATCH_CACHE_MAX];
static int g_classic_dispatch_empty;
static void classic_dispatch_cache_reset(void){
  for(int i=0;i<CLASSIC_DISPATCH_CACHE_MAX;i++) free(g_classic_dispatch[i].objects);
  memset(g_classic_dispatch,0,sizeof(g_classic_dispatch));
}
static const int *classic_event_objects(GmlVM *vm, const char *suffix, int *count){
  *count=0;
  if(!vm || !suffix || strlen(suffix)>=sizeof(g_classic_dispatch[0].suffix)) return NULL;
  int slot=-1;
  for(int i=0;i<CLASSIC_DISPATCH_CACHE_MAX;i++){
    if(g_classic_dispatch[i].used && !strcmp(g_classic_dispatch[i].suffix,suffix)){
      *count=g_classic_dispatch[i].n;
      return g_classic_dispatch[i].n ? g_classic_dispatch[i].objects : &g_classic_dispatch_empty;
    }
    if(slot<0 && !g_classic_dispatch[i].used) slot=i;
  }
  if(slot<0) return NULL;
  ClassicDispatchCache *cache=&g_classic_dispatch[slot];
  cache->used=1;
  snprintf(cache->suffix,sizeof(cache->suffix),"%s",suffix);
  for(int object=0;object<vm->n_objects;object++){
    if(!event_lookup_from(vm,suffix,object,NULL,NULL)) continue;
    if(cache->n>=cache->cap){
      int nc=cache->cap?cache->cap*2:16;
      int *objects=realloc(cache->objects,(size_t)nc*sizeof(*objects));
      if(!objects){ free(cache->objects); memset(cache,0,sizeof(*cache)); return NULL; }
      cache->objects=objects; cache->cap=nc;
    }
    cache->objects[cache->n++]=object;
  }
  *count=cache->n;
  if(getenv("GML_LOG_CLASSIC_DISPATCH"))
    fprintf(stderr,"[classic-dispatch] %s objects=%d/%d\n",suffix,cache->n,vm->n_objects);
  return cache->n ? cache->objects : &g_classic_dispatch_empty;
}
static void run_classic_object_event(GmlVM *vm, const char *suffix){
  int object_count=0;
  const int *objects=classic_event_objects(vm,suffix,&object_count);
  int extent=objects?object_count:vm->n_objects;
  for(int oi=0;oi<extent;oi++){
    int object=objects?objects[oi]:oi;
    if(!objects && !event_lookup_from(vm,suffix,object,NULL,NULL)) continue;
    int count=classic_collect_object_slots(vm,object);
    if(count>=0){
      for(int k=count-1;k>=0;k--){ int i=vm->event_ord[k];
        if(i<vm->inst_count && vm->inst[i].active && !vm->inst[i].marked && vm->inst[i].obj==object)
          gml_run_event(vm,&vm->inst[i],suffix); }
    } else {
      int extent=vm->inst_count;
      for(int i=0;i<extent;i++) if(vm->inst[i].active && !vm->inst[i].marked && vm->inst[i].obj==object)
        gml_run_event(vm,&vm->inst[i],suffix);
    }
  }
}
/* event_inherited(): from inside a child's event, also run the PARENT's version of that same event. */
int gml_event_inherited(GmlVM *vm){
  if(!vm->cur_self||!vm->cur_event||vm->cur_event_obj<0||vm->cur_event_obj>=vm->n_objects) return 0;
  return run_event_from(vm,vm->cur_self,vm->cur_event,vm->objects[vm->cur_event_obj].parent);
}
static void obj_list_link(GmlVM *vm, GmlInstance *in);
static void obj_list_unlink(GmlVM *vm, GmlInstance *in, int obj);
static void sort_slots_by_creation(GmlVM *vm, int *slot, int count){
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
static void prepare_step_free_slots(GmlVM *vm, int extent){
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
static GmlInstance *alloc_inst(GmlVM *vm){
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
static void trim_instance_pool_tail(GmlVM *vm){
  /* Pointer-stable compaction: only discard unused slots at the end.  Active instances never
   * move, which is a required property of the fixed pool and of the C API's instance pointers. */
  while(vm->inst_count>0){
    GmlInstance *in=&vm->inst[vm->inst_count-1];
    if(in->active || in->deactivated || in->room_dormant) break;
    vm->inst_count--;
  }
}
static void rebase_instance_order_to_slots(GmlVM *vm){
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
static void init_inst(GmlVM *vm, GmlInstance *in, double x, double y, int obj){
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
  gml_obj_alive_adjust(vm,obj,1); obj_list_link(vm,in);
  in->x=in->xstart=x; in->y=in->ystart=y; in->xprevious=x; in->yprevious=y;
  gml_colgrid_touch(in);   /* fresh instance: unknown to the current grid build */
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
static GmlInstance *instance_create_configured(GmlVM *vm, double x, double y, int obj,
                                               int have_depth, double depth, int layer_order){
  GmlInstance *in=alloc_inst(vm); init_inst(vm,in,x,y,obj);
  /* Set the supplied depth before running Create so that assignments in the event take precedence. */
  if(have_depth) in->depth=depth;
  if(layer_order>=0) in->draw_layer_order=layer_order;
  if(vm->render && (int)in->sprite_index>=0)   /* head start for the background decoder before first draw */
    gml_render_prefetch_sprite((GmlRender*)vm->render,(int)in->sprite_index);
  if(getenv("GML_LOG_CREATE")){ extern long g_vm_frame;
    GmlInstance *caller=vm->cur_self;
    const char *caller_name=(caller&&caller->obj>=0&&caller->obj<vm->n_objects)
      ?vm->objects[caller->obj].name:"?";
    fprintf(stderr,"[create] f%ld %s id=%u @(%.0f,%.0f) spr=%d caller=%s/%u event=%s\n",g_vm_frame,
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
  /* An instance stops being a live destruction target before its Destroy event runs.  Destroy
   * handlers are allowed to call instance_destroy() (directly or through a cleanup script);
   * leaving the pending flag until after the callback re-entered the same handler forever and
   * eventually overflowed the frontend thread's stack.  Event dispatch itself deliberately does
   * not reject marked instances, so the one required Destroy/CleanUp pair still executes. */
  if(!in||!in->active||in->marked) return;
  in->marked=1;
  gml_run_event(vm,in,"Destroy_0");
  /* Dispatch Clean Up after Destroy so disposal handlers can release instance resources. */
  gml_run_event(vm,in,"CleanUp_0");
}
static void reap(GmlVM *vm){
  /* Skip escaped arrays during instance reaping; shared arrays remain for deduplicated full teardown. */
  for(int i=0;i<vm->inst_count;i++) if(vm->inst[i].active && vm->inst[i].marked){
    gml_obj_alive_adjust(vm,vm->inst[i].obj,-1); obj_list_unlink(vm,&vm->inst[i],vm->inst[i].obj);
    varmap_free_ex(&vm->inst[i].vars,1); vm->inst[i].active=0; vm->inst[i].marked=0; }
}

static void set_global_arr(GmlVM *vm, const char *nm, int idx, double val){
  GmlVal *slot=gml_varmap_put(&vm->globals,nm); GmlArr *A=arr_of(slot); arr_ensure(A,idx);
  A->escaped=1;
  if(idx>=0 && idx<A->cap) A->data[idx]=vreal(val);
}
static double get_global_arr_d(GmlVM *vm, const char *nm, int idx){
  GmlVal *slot=gml_varmap_get(&vm->globals,nm); if(!slot||slot->t!=V_ARR) return 0;
  GmlArr *A=slot->arr; return (A && idx>=0 && idx<A->len)? asnum(A->data[idx]) : 0;
}
void gml_set_global_arr(GmlVM *vm, const char *nm, int idx, double val){ set_global_arr(vm,nm,idx,val); }
/* Write a global as a plain SCALAR (V_REAL), not as an array. This is the counterpart the generic
 * cheat interface needs for GameMaker Studio games: they read most globals as scalars, and a value
 * left as V_ARR (what set_global_arr produces) coerces to 0 in numeric/boolean context (asnum),
 * so an array-write to a scalar-read global silently does nothing. Overwrites the slot in place. */
void gml_set_global_scalar(GmlVM *vm, const char *nm, double val){
  if(!vm||!nm) return;
  GmlVal *slot=gml_varmap_put(&vm->globals,nm); if(slot) *slot=vreal(val);
}
/* Freeze/set a numeric instance variable on EVERY active instance of an object (or its descendants),
 * resolved by name. Returns how many instances were written. No game-specific knowledge: the object
 * name and variable name are supplied by the caller (a cheat line), never baked in. */
int gml_set_inst_var_all(GmlVM *vm, const char *objname, const char *var, double val){
  if(!vm||!objname||!var) return 0;
  int obj=gml_object_index_by_name(vm,objname); if(obj<0) return 0;
  int n=0;
  for(int i=0;i<vm->inst_count;i++){
    GmlInstance *in=&vm->inst[i];
    if(!in->active || in->marked) continue;
    if(!gml_object_is(vm,in->obj,obj)) continue;
    GmlVal v=vreal(val);
    if(inst_builtin_set(in,var,v)){ n++; continue; }
    GmlVal *slot=gml_varmap_put(&in->vars,var); if(slot){ *slot=v; n++; }
  }
  return n;
}
/* --- generic pause-menu injection helpers (used by the frontend menu editor; no game specifics) --- */
int gml_inst_get_num(const GmlInstance *in, const char *var){
  if(!in||!var) return 0;
  GmlVal *v=gml_varmap_get((GmlVarMap*)&in->vars,var);
  return (v && v->t==V_REAL) ? (int)v->d : 0;
}
/* Number of items the game itself placed in a 1D or 2D menu array (height2d for 2D, len for 1D). */
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
  GmlArr *A=arr_of(slot);
  A->escaped=1;
  arr_note_2d_set(A,idx); arr_ensure(A,idx);
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
    slot=gml_varmap_put_owned_h(&vm->globals,stable,strhash(stable));
  }
  *slot=vreal(value);
}

static double room_state_restore_number(GmlVM *vm, int room, const char *field, int index){
  char key[128]; room_state_key(key,sizeof(key),room,field,index);
  GmlVal *value=gml_varmap_get(&vm->globals,key);
  return value?asnum(*value):0.0;
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

static void room_runtime_state_store(GmlVM *vm, int room){
  room_state_store_number(vm,room,"present",0,1);
  for(size_t field=0;field<sizeof(room_background_fields)/sizeof(room_background_fields[0]);field++)
    for(int i=0;i<8;i++) room_state_store_number(vm,room,room_background_fields[field],i,
                                                  get_global_arr_d(vm,room_background_fields[field],i));
  for(size_t field=0;field<sizeof(room_view_fields)/sizeof(room_view_fields[0]);field++)
    for(int i=0;i<8;i++) room_state_store_number(vm,room,room_view_fields[field],i,
                                                  get_global_arr_d(vm,room_view_fields[field],i));
  GmlVal *speed=gml_varmap_get(&vm->globals,"room_speed");
  room_state_store_number(vm,room,"room_speed",0,speed?asnum(*speed):gml_room_speed(vm));
  GmlVal *view_current=gml_varmap_get(&vm->globals,"view_current");
  room_state_store_number(vm,room,"view_current",0,view_current?asnum(*view_current):0);
  GmlVal *view_enabled=gml_varmap_get(&vm->globals,"view_enabled");
  room_state_store_number(vm,room,"view_enabled",0,view_enabled?asnum(*view_enabled):0);
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
    for(int i=0;i<8;i++) set_global_arr(vm,room_background_fields[field],i,
                                        room_state_restore_number(vm,room,room_background_fields[field],i));
  for(size_t field=0;field<sizeof(room_view_fields)/sizeof(room_view_fields[0]);field++)
    for(int i=0;i<8;i++) set_global_arr(vm,room_view_fields[field],i,
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
static uint32_t gml_room_layer_list(GmlVM *vm, int room_index, uint32_t *out_count){
  if(out_count) *out_count=0;
  if(!vm || !vm->win || room_index<0 || room_index>=gml_room_count(vm->win)) return 0;
  const GmlChunk *rc=gml_chunk(vm->win,"ROOM");
  if(!rc) return 0;
  const uint8_t *d=vm->win->data;
  uint64_t rend=(uint64_t)rc->off+rc->size;
  uint64_t slot=(uint64_t)rc->off+4u+(uint64_t)(uint32_t)room_index*4u;
  if(slot+4u>rend) return 0;
  uint32_t rp=u32(d,(uint32_t)slot);
  if(rp<rc->off || (uint64_t)rp+92u>rend) return 0;
  static const uint8_t layer_slots[]={88,92,96,100,104,108,112,116,120};
  for(size_t s=0;s<sizeof(layer_slots)/sizeof(layer_slots[0]);s++){
    uint32_t lo=layer_slots[s];
    if((uint64_t)rp+lo+4u>rend) break;
    uint32_t lay=u32(d,rp+lo);
    if(lay<rc->off || (uint64_t)lay+4u>rend) continue;
    uint32_t lcnt=u32(d,lay);
    if(lcnt==0 || lcnt>=512 || (uint64_t)lay+4u+(uint64_t)lcnt*4u>rend) continue;
    int valid=1;
    for(uint32_t i=0;i<lcnt;i++){
      uint32_t lp=u32(d,lay+4+i*4);
      if(lp<rc->off || (uint64_t)lp+36u>rend){ valid=0; break; }
      uint32_t type=u32(d,lp+8);
      if(type<1 || type>8 || u32(d,lp+32)>1){ valid=0; break; }
      uint32_t name=u32(d,lp);
      if(name && name>=vm->win->size){ valid=0; break; }
    }
    if(valid){
      if(out_count) *out_count=lcnt;
      return lay;
    }
  }
  return 0;
}

/* Select the base or effect-field layer layout by structural voting across background records.
 * Effect-field layers include a variable-length property list before their type data. */
static int bg_type_data_ok(const GmlWin *w, uint32_t b, int nspr){
  const uint8_t *d=w->data;
  if(b+40>w->size) return 0;
  if(u32(d,b)>1) return 0;                                   /* visible */
  if(u32(d,b+4)>1) return 0;                                 /* foreground */
  int32_t spr=(int32_t)u32(d,b+8);
  if(spr<-1 || spr>=nspr) return 0;                          /* sprite id */
  if(u32(d,b+12)>1||u32(d,b+16)>1||u32(d,b+20)>1) return 0;  /* htiled/vtiled/stretch */
  float ff=f32(d,b+28);
  if(!(ff>=-1.0f && ff<65536.0f)) return 0;                  /* first frame */
  if(u32(d,b+36)>1) return 0;                                /* animation speed type */
  return 1;
}
static int layer_effect_fields_ok(const GmlWin *w, uint32_t lp){
  const uint8_t *d=w->data;
  if(lp+48>w->size) return 0;
  if(u32(d,lp+36)>1) return 0;                               /* effectEnabled */
  uint32_t sp=u32(d,lp+40);                                  /* effectType: null or strptr */
  if(sp){ if(sp<12 || sp+1>=w->size) return 0;
    uint32_t sl=u32(d,sp-4); if(sl==0 || sl>256) return 0; }
  if(u32(d,lp+44)>64) return 0;                              /* effect property count */
  return 1;
}
int gml_room_layer_data_off(GmlVM *vm){
  if(vm->layer_data_off) return vm->layer_data_off;
  vm->layer_data_off=36;
  const GmlChunk *rc=gml_chunk(vm->win,"ROOM");
  const GmlChunk *sc=gml_chunk(vm->win,"SPRT");
  if(!rc||!sc) return vm->layer_data_off;
  const uint8_t *d=vm->win->data;
  int nspr=(int)u32(d,sc->off);
  int nrooms=gml_room_count(vm->win);
  int v36=0, v48=0, sampled=0;
  for(int ri=0;ri<nrooms && sampled<64;ri++){
    uint32_t lcnt=0;
    uint32_t lay=gml_room_layer_list(vm,ri,&lcnt);
    if(!lcnt || lcnt>=512) continue;
    for(uint32_t i=0;i<lcnt && sampled<64;i++){ uint32_t lp=u32(d,lay+4+i*4);
      if(!lp || lp+96>vm->win->size || u32(d,lp+8)!=1) continue;
      sampled++;
      if(bg_type_data_ok(vm->win,lp+36,nspr)) v36++;
      if(layer_effect_fields_ok(vm->win,lp)){
        uint32_t pc=u32(d,lp+44);
        if(bg_type_data_ok(vm->win,lp+48+12*pc,nspr)) v48++;
      }
    }
  }
  if(v48>v36) vm->layer_data_off=48;
  return vm->layer_data_off;
}
/* Resolve each layer's type-data start after its optional effect-property list. */
uint32_t gml_room_layer_type_off(GmlVM *vm, uint32_t lp){
  if(gml_room_layer_data_off(vm)==36) return lp+36;
  uint32_t pc=(lp+48<=vm->win->size)?u32(vm->win->data,lp+44):0;
  if(pc>64) pc=0;
  return lp+48+12*pc;
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
  uint32_t version=u32(d,c->off), count=u32(d,c->off+4);
  if(version!=1 || count>4096 || 8u+(uint64_t)count*8u>c->size) return 0;
  for(uint32_t i=0;i<count;i++){
    uint32_t p=c->off+8+i*8, np=u32(d,p), tp=u32(d,p+4);
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
/* Decode the standard effect-layer descriptor stored inline in later ROOM records. These are
 * engine filter identifiers/properties, so every package using the stock filters follows the same
 * path; unknown/custom filters remain a conservative no-op. */
static int gml_room_layer_effect(GmlVM *vm, uint32_t lp, GmlLayerFilter *out){
  memset(out,0,sizeof(*out));
  out->sampler_sprite=-1;
  if(!vm || !vm->win || gml_room_layer_data_off(vm)!=48 || lp+48>vm->win->size) return 0;
  const uint8_t *d=vm->win->data;
  if(!u32(d,lp+36)) return 0;
  const char *type=layer_effect_string(vm->win,u32(d,lp+40));
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
  uint32_t count=u32(d,lp+44);
  if(count>64 || lp+48+(uint64_t)count*12u>vm->win->size) return 0;
  const char *sampler="";
  int velocity=0,shape=0,shade_offset=0,distort1_scale=0,distort2_scale=0;
  int zoom_centre=0,box_size=0,box_rotation=0;
  for(uint32_t i=0;i<count;i++){
    uint32_t p=lp+48+i*12;
    const char *name=layer_effect_string(vm->win,u32(d,p+4));
    const char *value=layer_effect_string(vm->win,u32(d,p+8));
    double number=strtod(value,NULL);
    if(out->kind==GML_LAYER_FILTER_RGB_NOISE){
      if(strstr(name,"Intensity")) out->u.noise.intensity=number;
      else if(strstr(name,"Animation")) out->u.noise.animation=number;
      else if(strstr(name,"Colour")||strstr(name,"Color")) out->u.noise.colour=layer_effect_colour(value,out->u.noise.colour);
      else if((int32_t)u32(d,p)==2 || strstr(name,"Texture")) sampler=value;
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
    free(vm->tilemaps[i].decoded_tiles);
    vm->tilemaps[i].owned_tiles=NULL;
    vm->tilemaps[i].decoded_tiles=NULL;
  }
  vm->n_tilemaps=0;
}

/* Read enough tileset metadata to distinguish the extended layout, which also uses compressed
 * room grids, from the older BGND record. */
static int gml_tileset_meta(GmlVM *vm,int tileset,int *tile_count,int *modern){
  if(tile_count) *tile_count=0;
  if(modern) *modern=0;
  const GmlChunk *bc=gml_chunk(vm->win,"BGND");
  if(!bc || tileset<0 || (uint32_t)tileset>=u32(vm->win->data,bc->off)) return 0;
  const uint8_t *d=vm->win->data;
  uint32_t bp=u32(d,bc->off+4+(uint32_t)tileset*4u);
  if(!bp || (uint64_t)bp+72u>(uint64_t)bc->off+bc->size) return 0;
  int oi=(int)u32(d,bp+44), oc=(int)u32(d,bp+48), ocols=(int)u32(d,bp+40);
  uint64_t obytes=(uint64_t)(uint32_t)oi*(uint64_t)(uint32_t)oc*4u;
  int old_ok=ocols>0&&ocols<=4096&&oi>0&&oi<=1024&&oc>0&&obytes<=4000000u&&
             (uint64_t)bp+64u+obytes<=(uint64_t)bc->off+bc->size;
  int ni=(int)u32(d,bp+52), nc=(int)u32(d,bp+56), ncols=(int)u32(d,bp+48);
  uint64_t nbytes=(uint64_t)(uint32_t)ni*(uint64_t)(uint32_t)nc*4u;
  int new_ok=u32(d,bp+32)<=4096&&u32(d,bp+36)<=4096&&u32(d,bp+40)<=4096&&u32(d,bp+44)<=4096&&
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

/* Decode compressed room-grid byte runs. A high opcode repeats one u32 1..128 times; a low
 * opcode copies that many literal u32 values. */
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
      uint32_t v=u32(src,(uint32_t)ip); ip+=4;
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
     src[ip]==0x81u && u32(src,(uint32_t)ip+1u)==0xFFFFFFFFu) ip+=5u;
  if(used) *used=ip;
  return out;
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
  uint32_t lay=gml_room_layer_list(vm,room_index,&lcnt);
  if(!lay || lcnt>=512) return;
  for(uint32_t i=0;i<lcnt;i++){
    uint32_t lp=u32(rd,lay+4+i*4);
    if(!lp || u32(rd,lp+8)!=1) continue;
    uint32_t np=u32(rd,lp);
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
    e->visible=u32(rd,b)?1:0;
    e->sprite=(int32_t)u32(rd,b+8);
    e->htiled=(int)u32(rd,b+12);
    e->vtiled=(int)u32(rd,b+16);
    e->stretch=(int)u32(rd,b+20);
    uint32_t col=u32(rd,b+24);
    e->blend=col&0xFFFFFFu;
    e->alpha=((col>>24)&0xFF)/255.0;
    e->image_index=f32(rd,b+28);
    e->image_speed=f32(rd,b+32);
    if(getenv("GML_LOG_RTL")){
      extern long g_vm_frame;
      fprintf(stderr,"[rtl] f%ld bind background layer='%s' lid=%d eid=%d sprite=%d\n",
              g_vm_frame,lname,rl->id,e->id,e->sprite);
    }
  }
}

static void gml_room_bind_asset_sprites(GmlVM *vm, int room_index){
  if(!vm || !vm->win || room_index<0) return;
  const uint8_t *rd=vm->win->data;
  uint32_t lcnt=0;
  uint32_t lay=gml_room_layer_list(vm,room_index,&lcnt);
  if(!lcnt || lcnt>=512) return;
  for(uint32_t i=0;i<lcnt;i++){
    uint32_t lp=u32(rd,lay+4+i*4);
    if(!lp || u32(rd,lp+8)!=3) continue; /* Assets */
    uint32_t tb=gml_room_layer_type_off(vm,lp);
    if(tb+8>vm->win->size) continue;
    uint32_t np=u32(rd,lp+0);
    const char *lname=(np&&np<vm->win->size)?(const char*)(rd+np):"";
    GmlRtLayer *rl=gml_rt_layer_find_by_name(vm,lname);
    if(!rl) continue;
    uint32_t sprites=u32(rd,tb+4);          /* LayerAssetsData.Sprites */
    uint32_t scnt=(sprites && sprites+4<vm->win->size)?u32(rd,sprites):0;
    if(scnt>100000) continue;
    for(uint32_t k=0;k<scnt;k++){
      uint32_t sprec=u32(rd,sprites+4+k*4);
      if(!sprec || sprec+44>vm->win->size) continue;
      uint32_t nmp=u32(rd,sprec+0);
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
      e->sprite=(int32_t)u32(rd,sprec+4);
      e->x=(double)(int32_t)u32(rd,sprec+8);
      e->y=(double)(int32_t)u32(rd,sprec+12);
      e->xs=f32(rd,sprec+16);
      e->ys=f32(rd,sprec+20);
      uint32_t col=u32(rd,sprec+24);
      e->blend=col&0xFFFFFFu;
      e->alpha=((col>>24)&0xFF)/255.0;
      e->image_speed=f32(rd,sprec+28);
      e->image_index=f32(rd,sprec+36);
      e->image_angle=f32(rd,sprec+40);
    }
  }
}

/* Rebuild a room's derived GMS2 layer data from immutable ROOM records. Runtime layers/elements are
 * rebuilt on room enter; after modern savestate loads they are preserved and only type-4 tilemap
 * views are rebound to win data, since those grids are not serialized by pointer. */
static void gml_room_reload_layers_mode(GmlVM *vm, int room_index, int rebuild_runtime_layers){
  if(rebuild_runtime_layers){
    /* Per-layer shader handles use serialized globals so rewind needs no state-format fork.  A
     * genuine room rebuild owns a new layer set and must not inherit the previous room's slot. */
    for(int i=0;i<vm->n_rtl;i++) gml_set_global_arr(vm,"__gml_layer_shader",i,0);
    vm->n_rtl=0; vm->n_rte=0;
  }
  const uint8_t *rd=vm->win->data;
  uint32_t lcnt=0;
  uint32_t lay=gml_room_layer_list(vm,room_index,&lcnt);
  if(!lay) return;
  if(rebuild_runtime_layers && lcnt<512) for(uint32_t i=0;i<lcnt;i++){ uint32_t lp=u32(rd,lay+4+i*4);
    if(!lp || lp+40>vm->win->size) continue;
    uint32_t np=u32(rd,lp+0); if(!np || np>=vm->win->size) continue;
    GmlRtLayer *l=gml_rt_layer_new(vm); if(!l) break;
    snprintf(l->name,sizeof l->name,"%s",(const char*)(rd+np));
    l->order=(int)i;
    l->depth=(double)(int32_t)u32(rd,lp+12);
    l->x=f32(rd,lp+16); l->y=f32(rd,lp+20); l->hs=f32(rd,lp+24); l->vs=f32(rd,lp+28);
    l->visible=u32(rd,lp+32)?1:0; l->touched=0;
  }
  if(lcnt<512){
    if(rebuild_runtime_layers)
      for(int ii=0; ii<vm->inst_count; ii++){
        vm->inst[ii].draw_layer_order=-1;
        vm->inst[ii].draw_layer_element_order=-1;
      }
    for(uint32_t i=0;i<lcnt;i++){ uint32_t lp=u32(rd,lay+4+i*4);
      if(!lp || lp+40>vm->win->size) continue;
      uint32_t np=u32(rd,lp+0);
      if(np && np<vm->win->size){
        GmlRtLayer *rl=gml_rt_layer_find_by_name(vm,(const char*)(rd+np));
        if(rl) rl->order=(int)i;
      }
    }
  }
  gml_tilemaps_clear(vm);
  if(lcnt<512) for(uint32_t i=0;i<lcnt;i++){ uint32_t lp=u32(rd,lay+4+i*4);
    if(!lp || u32(rd,lp+8)!=2) continue;
    uint32_t tb=gml_room_layer_type_off(vm,lp);
    if(tb+4>vm->win->size) continue;
    uint32_t ic=u32(rd,tb);
    if(ic>100000) continue;
    int ord=(int)i;
    uint32_t np=u32(rd,lp+0);
    if(np && np<vm->win->size){
      GmlRtLayer *rl=gml_rt_layer_find_by_name(vm,(const char*)(rd+np));
      if(rl) ord=rl->order;
    }
    for(uint32_t k=0;k<ic;k++){
      uint32_t ip2=tb+4+k*4;
      if(ip2+4>vm->win->size) break;
      uint32_t iid=u32(rd,ip2);
      for(int ii=0; ii<vm->inst_count; ii++)
        if((vm->inst[ii].active || vm->inst[ii].deactivated) && vm->inst[ii].id==iid){
          vm->inst[ii].draw_layer_order=ord;
          vm->inst[ii].draw_layer_element_order=(int)k;
          break;
        }
    }
  }
  const GmlChunk *bc = gml_chunk(vm->win,"BGND");
  uint32_t bcnt = bc ? u32(rd,bc->off) : 0;
  if(lcnt<512) for(uint32_t i=0;i<lcnt;i++){ uint32_t lp=u32(rd,lay+4+i*4);
    if(!lp || u32(rd,lp+8)!=4) continue;          /* layer type 4 = tile layer */
    uint32_t tb=gml_room_layer_type_off(vm,lp);
    if(tb+12>vm->win->size) continue;
    int tileset=(int32_t)u32(rd,tb);
    int cols=(int32_t)u32(rd,tb+4), rows=(int32_t)u32(rd,tb+8);
    if(cols<=0||rows<=0||cols>8192||rows>8192) continue;
    uint32_t tdata=tb+12;
    size_t cells=(size_t)cols*(size_t)rows;
    int modern_tiles=0;
    (void)gml_tileset_meta(vm,tileset,NULL,&modern_tiles);
    unsigned char *decoded=NULL;
    if(modern_tiles){
      size_t used=0;
      decoded=gml_tile_rle_decode(rd+tdata,vm->win->size-tdata,cells,&used);
      if(!decoded) continue;
    } else if((uint64_t)tdata + (uint64_t)cells*4u > vm->win->size) continue;
    int tw=16,th=16;
    if(bc && tileset>=0 && (uint32_t)tileset<bcnt){ uint32_t bp=u32(rd,bc->off+4+tileset*4);
      if(bp && bp+32<vm->win->size){ int w=(int32_t)u32(rd,bp+24),h=(int32_t)u32(rd,bp+28);
        if(w>0)tw=w; if(h>0)th=h; } }
	    GmlTileMap *tm=gml_tilemap_new(vm); if(!tm){ free(decoded); break; }
	    uint32_t np2=u32(rd,lp+0);
	    snprintf(tm->name,sizeof tm->name,"%s",(np2&&np2<vm->win->size)?(const char*)(rd+np2):"");
	    tm->tileset=tileset;
	    tm->depth=(double)(int32_t)u32(rd,lp+12);
	    tm->order=(int)i;
	    tm->tw=tw; tm->th=th; tm->cols=cols; tm->rows=rows;
        tm->decoded_tiles=decoded;
        tm->tiles=decoded?decoded:rd+tdata;
        tm->base_tiles=tm->tiles;
	    tm->x=f32(rd,lp+16); tm->y=f32(rd,lp+20); tm->visible=u32(rd,lp+32)?1:0;
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
  if(getenv("GML_LOG_ROOM")){ fprintf(stderr,"[room] reload_layers room=%d: %d layers, %d tilemaps\n",room_index,vm->n_rtl,vm->n_tilemaps);
    for(int t=0;t<vm->n_tilemaps;t++){ GmlTileMap *tm=&vm->tilemaps[t];
      int solid=0; for(int c=0;c<tm->cols*tm->rows;c++){ uint32_t d=u32(tm->tiles,(uint32_t)c*4); if((d&0x7FFFF)!=0) solid++; }
      fprintf(stderr,"[tile]  tm[%d] name='%s' cols=%d rows=%d cell=%dx%d pos=%.0f,%.0f solid=%d\n",t,tm->name,tm->cols,tm->rows,tm->tw,tm->th,tm->x,tm->y,solid);
      if(getenv("GML_DUMP_TILE_VALUES")){
        int shown=0;
        fprintf(stderr,"[tile-values t%d]",t);
        for(int ry=0;ry<tm->rows && shown<24;ry++) for(int rx=0;rx<tm->cols && shown<24;rx++){
          uint32_t datum=u32(tm->tiles,(uint32_t)(ry*tm->cols+rx)*4);
          if((datum&0x7FFFFu)!=0){ fprintf(stderr," %d:%d=%08x",rx,ry,datum); shown++; }
        }
        fprintf(stderr,"\n");
      }
      if(getenv("GML_DUMP_GRID")){ for(int ry=0;ry<tm->rows && ry<40;ry++){ char line[200]; int lp=0;
        for(int rx=0;rx<tm->cols && rx<128;rx++){ uint32_t d=u32(tm->tiles,(uint32_t)(ry*tm->cols+rx)*4); line[lp++]=((d&0x7FFFF)!=0)?'#':'.'; }
        line[lp]=0; fprintf(stderr,"[grid t%d r%02d] %s\n",t,ry,line); } } }
  }
}
static void gml_room_reload_layers(GmlVM *vm, int room_index){
  gml_room_reload_layers_mode(vm,room_index,1);
}

/* queue background decodes for every atlas this room's content can touch: instance sprites
 * and masks, GMS2 tile layers, and runtime layer elements. Draws still decode synchronously
 * if a texture arrives before its prefetch finishes, so this only removes stalls. */
static void vm_prefetch_room_assets(GmlVM *vm){
  GmlRender *R=(GmlRender*)vm->render; if(!R) return;
  int warm = 1;
  const char *we = getenv("GML_ATLAS_ROOM_WARM");
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
static int vm_audio_room_warm_disabled(void){
  static int disabled=-1;
  if(disabled<0){
    const char *e=getenv("GML_AUDIO_ROOM_WARM");
    disabled = e && (!strcmp(e,"0") || !strcmp(e,"off") || !strcmp(e,"false"));
  }
  return disabled;
}
static int vm_audio_room_warm_debug(void){
  static int enabled=-1;
  if(enabled<0) enabled=getenv("GML_DBG_AUDIO_ROOM_WARM")!=NULL;
  return enabled;
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
  if(!code_cache_ensure(vm->win,ci)) return;
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
        if(vm_audio_room_warm_debug())
          fprintf(stderr,"[audio-warm-code] room=%d code=%s call=%d sound=%d\n",
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
    if(event_lookup_from(vm,suffix,obj,NULL,&ci)) vm_warm_audio_code_for_room(vm,ci,room_index);
  }
}
static void vm_warm_audio_room_placed_alarm_codes(GmlVM *vm, int room_index){
  if(!vm || !vm->win || room_index<0) return;
  GmlRoom r;
  if(gml_room_get(vm->win,room_index,&r)!=0 || !r.obj_ptr) return;
  const uint8_t *d=vm->win->data;
  if(r.obj_ptr+4>vm->win->size) return;
  uint32_t cnt=u32(d,r.obj_ptr);
  if(cnt>100000) return;
  for(uint32_t i=0;i<cnt;i++){
    uint32_t ip=u32(d,r.obj_ptr+4+i*4);
    if(!ip || ip+12>vm->win->size) continue;
    vm_warm_audio_object_alarm_codes(vm,(int32_t)u32(d,ip+8),room_index);
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
  if(!vm || !vm->win || !vm->audio || room_index<0 || vm_audio_room_warm_disabled()) return;
  vm_warm_audio_global_code_for_room(vm,room_index);
  for(int i=0;i<vm->inst_count;i++){
    GmlInstance *in=&vm->inst[i];
    if(!in->active || in->marked || in->obj<0 || in->obj>=vm->n_objects) continue;
    vm_warm_audio_object_alarm_codes(vm,in->obj,room_index);
  }
  vm_warm_audio_room_placed_alarm_codes(vm,room_index);
}
static void vm_warm_audio_for_room_window(GmlVM *vm){
  if(!vm || vm_audio_room_warm_disabled()) return;
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
  int store_previous=0;
  if(prev_room>=0){
    GmlVal *persistent=gml_varmap_get(&vm->globals,"room_persistent");
    if(persistent) store_previous=asnum(*persistent)!=0.0;
    else { GmlRoom previous; if(gml_room_get(vm->win,prev_room,&previous)==0) store_previous=previous.persistent; }
    if(prev_room<vm->room_state_count && vm->room_stored) vm->room_stored[prev_room]=store_previous?1:0;
  }
  if(store_previous) room_runtime_state_store(vm,prev_room);
  if(store_previous){
    for(int i=0;i<vm->inst_count;i++){
      GmlInstance *in=&vm->inst[i];
      if(in->persistent || in->marked || in->room_owner!=prev_room || (!in->active&&!in->deactivated)) continue;
      gml_obj_alive_adjust(vm,in->obj,-1); obj_list_unlink(vm,in,in->obj);
      in->room_was_deactivated=in->deactivated?1:0;
      in->active=0; in->deactivated=0; in->room_dormant=1;
    }
  }
  /* clear non-persistent instances (incl. deactivated ones, which keep active=0).
   * Room disposal runs Clean Up after Room End, without invoking Destroy. */
  for(int i=0;i<vm->inst_count;i++) if((vm->inst[i].active||vm->inst[i].deactivated) && !vm->inst[i].persistent){
    gml_run_event(vm,&vm->inst[i],"CleanUp_0");   /* GMS2.3: Clean Up fires on room-change disposal */
    /* Preserve escaped arrays during room cleanup because surviving globals or
     * persistent instances may reference them. Full teardown deduplicates releases. */
    gml_obj_alive_adjust(vm,vm->inst[i].obj,-1); obj_list_unlink(vm,&vm->inst[i],vm->inst[i].obj);
    varmap_free_ex(&vm->inst[i].vars,1); vm->inst[i].active=0; vm->inst[i].deactivated=0; }
  vm->room_index=room_index; vm->pending_room=-1;
  for(int i=0;i<vm->inst_count;i++) if(vm->inst[i].persistent && (vm->inst[i].active||vm->inst[i].deactivated))
    vm->inst[i].room_owner=room_index;
  { extern long g_vm_frame; vm->room_enter_frame=g_vm_frame; }
  vm->n_tile_mut=0;   /* tile-layer mutations are per-room */
  vm->n_tile_del_at=0; /* tile_layer_delete_at marks are per-room */
  memset(vm->phys_fixture,0,sizeof(vm->phys_fixture));
  memset(vm->phys_joint,0,sizeof(vm->phys_joint));
  vm->phys_next_id=0; vm->phys_gravity_x=0; vm->phys_gravity_y=0;
  vm->phys_update_speed=0; vm->phys_update_iterations=0; vm->phys_paused=0; vm->phys_debug_draw=0;
  /* Register this room's GMS2 runtime layers (addressable by name) + type-4 tile-collision maps. */
  gml_room_reload_layers(vm, room_index);
  if(getenv("GML_LOG_ROOM")) fprintf(stderr,"[room] enter %d\n",room_index);
  GmlRoom r; if(gml_room_get(vm->win,room_index,&r)!=0) return;
  *gml_varmap_put(&vm->globals,"room_persistent")=vreal(r.persistent?1.0:0.0);
  *gml_varmap_put(&vm->globals,"view_enabled")=vreal(r.view_enabled?1.0:0.0);
  if(vm->win->classic_version)
    *gml_varmap_put(&vm->globals,"room_speed")=vreal(r.speed>0?r.speed:30);
  if(room_index>=0 && room_index<vm->room_state_count && vm->room_stored && vm->room_stored[room_index]){
    vm->room_stored[room_index]=0;
    *gml_varmap_put(&vm->globals,"room_persistent")=vreal(1);
    room_runtime_state_restore(vm,room_index);
    for(int i=0;i<vm->inst_count;i++){
      GmlInstance *in=&vm->inst[i];
      if(!in->room_dormant || in->room_owner!=room_index) continue;
      in->room_dormant=0; in->deactivated=in->room_was_deactivated?1:0;
      in->active=in->deactivated?0:1; in->room_was_deactivated=0;
      gml_obj_alive_adjust(vm,in->obj,1); obj_list_link(vm,in);
    }
    int n0=vm->inst_count;
    for(int i=0;i<n0;i++) if(vm->inst[i].active && !vm->inst[i].marked)
      gml_run_event(vm,&vm->inst[i],"Other_4");
    gml_fire_gamepad_connected(vm);
    reap(vm); rebase_instance_order_to_slots(vm);
    gml_vm_warm_audio_for_room(vm,vm->room_index); vm_prefetch_room_assets(vm);
    return;
  }
  const uint8_t *d=vm->win->data; uint32_t op=r.obj_ptr, cnt=u32(d,op);
  for(int i=0;vm->win->classic_version && i<8;i++){
    set_global_arr(vm,"background_visible",i,0); set_global_arr(vm,"background_foreground",i,0);
    set_global_arr(vm,"background_index",i,-1); set_global_arr(vm,"background_x",i,0);
    set_global_arr(vm,"background_y",i,0); set_global_arr(vm,"background_htiled",i,0);
    set_global_arr(vm,"background_vtiled",i,0); set_global_arr(vm,"background_hspeed",i,0);
    set_global_arr(vm,"background_vspeed",i,0); set_global_arr(vm,"background_stretch",i,0);
    set_global_arr(vm,"background_alpha",i,1); set_global_arr(vm,"background_blend",i,0xFFFFFF);
    for(size_t field=0;field<sizeof(room_view_fields)/sizeof(room_view_fields[0]);field++)
      set_global_arr(vm,room_view_fields[field],i,0);
    set_global_arr(vm,"view_object",i,-1);
  }
  /* Initialise the built-in background_* arrays from the room's background
   * layers. GML draw code can read this state during room startup. */
  if(r.bg_ptr){ uint32_t bc=u32(d,r.bg_ptr);
    for(uint32_t i=0;i<bc && i<8;i++){ uint32_t lp=u32(d,r.bg_ptr+4+i*4);
      int en=(int)u32(d,lp), fg=(int)u32(d,lp+4), def=(int)u32(d,lp+8);
      int bx=(int)u32(d,lp+12), by=(int)u32(d,lp+16), htl=(int)u32(d,lp+20), vtl=(int)u32(d,lp+24);
      int bh=(int32_t)u32(d,lp+28), bv=(int32_t)u32(d,lp+32);
      set_global_arr(vm,"background_visible",i,en?1:0);
      set_global_arr(vm,"background_foreground",i,fg?1:0);
      /* Visibility does not clear the assigned background resource. Classic projects commonly
       * author parallax layers hidden and enable them from room-start code; retaining the index is
       * what makes that later background_visible write meaningful. */
      set_global_arr(vm,"background_index",i,def);
      set_global_arr(vm,"background_x",i,bx);  set_global_arr(vm,"background_y",i,by);
      set_global_arr(vm,"background_htiled",i,htl?1:0); set_global_arr(vm,"background_vtiled",i,vtl?1:0);
      set_global_arr(vm,"background_hspeed",i,bh); set_global_arr(vm,"background_vspeed",i,bv);
      set_global_arr(vm,"background_stretch",i,(int)u32(d,lp+36)?1:0);
      set_global_arr(vm,"background_alpha",i,1.0);          /* GM default */
      set_global_arr(vm,"background_blend",i,0xFFFFFF);
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
  /* Apply runtime room-view resource overrides on entry. */
  if(vm->view_ovr) for(int i=0;i<8;i++){
    int k=room_index*8+i;
    if(k>=0 && k<vm->n_view_ovr){
      struct GmlViewOvr *o=&vm->view_ovr[k];
      if(i==0 && o->room_enabled_set)
        *gml_varmap_put(&vm->globals,"view_enabled")=vreal(o->room_enabled?1.0:0.0);
      if(o->set || o->full){
        set_global_arr(vm,"view_visible",i,o->vis);
        set_global_arr(vm,"view_xport",i,o->x); set_global_arr(vm,"view_yport",i,o->y);
        set_global_arr(vm,"view_wport",i,o->w); set_global_arr(vm,"view_hport",i,o->h);
      }
      if(o->full){
        set_global_arr(vm,"view_xview",i,o->view_x); set_global_arr(vm,"view_yview",i,o->view_y);
        set_global_arr(vm,"view_wview",i,o->view_w); set_global_arr(vm,"view_hview",i,o->view_h);
        set_global_arr(vm,"view_hborder",i,o->hborder); set_global_arr(vm,"view_vborder",i,o->vborder);
        set_global_arr(vm,"view_hspeed",i,o->hspeed); set_global_arr(vm,"view_vspeed",i,o->vspeed);
        set_global_arr(vm,"view_object",i,o->object);
      }
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
  /* Placed ids are authored resource data, not allocations from the dynamic-id sequence.
   * init_inst() still performs all ordinary instance initialization below, so preserve the
   * already-reserved dynamic id here and restore it once every placed slot exists. */
  uint32_t next_dynamic_id=vm->next_id;
  /* Room-placed instances are all present before any Create event runs. Some
   * room setup code relies on seeing other already-placed instances. */
  int *room_inst_idx=malloc((cnt?cnt:1)*sizeof(int));
  for(uint32_t i=0;i<cnt;i++){
    uint32_t ip=u32(d,op+4+i*4);
    int32_t x=(int32_t)u32(d,ip), y=(int32_t)u32(d,ip+4); int obj=(int32_t)u32(d,ip+8);
    GmlInstance *in=alloc_inst(vm); init_inst(vm,in,x,y,obj);
    in->id=u32(d,ip+12);  /* room-assigned instance id */
    apply_room_instance_transform(vm,in,ip);
    in->room_placed=1;
    room_inst_idx[i]=(int)(in-vm->inst);
  }
  vm->next_id=next_dynamic_id;
  /* Assign placed-instance depths from their type-2 room layers before Create events. */
  {
    uint32_t lcnt=0;
    uint32_t lay=gml_room_layer_list(vm,room_index,&lcnt);
    if(lcnt>0 && lcnt<512){
      for(uint32_t i=0;i<lcnt;i++){ uint32_t lp=u32(d,lay+4+i*4);
        if(!lp || u32(d,lp+8)!=2) continue;
        uint32_t tb=gml_room_layer_type_off(vm,lp);
        if(tb+4>vm->win->size) continue;
        double ldep=(double)(int32_t)u32(d,lp+12);
        int lorder=(int)i;
        uint32_t lnp=u32(d,lp+0);
        if(lnp && lnp<vm->win->size){
          GmlRtLayer *rl=gml_rt_layer_find_by_name(vm,(const char*)(d+lnp));
          if(rl) lorder=rl->order;
        }
        uint32_t ic=u32(d,tb);
        if(ic>100000) continue;
        for(uint32_t k=0;k<ic;k++){
          uint32_t ip2=tb+4+k*4;
          if(ip2+4>vm->win->size) break;
          uint32_t iid=u32(d,ip2);
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
  /* Create + per-instance creation code, in room order, after every placed instance exists.
   * Classic projects serialize which side of Create the room-authored instance code occupies. */
  for(uint32_t i=0;i<cnt;i++){
    int idx=room_inst_idx[i]; if(idx<0 || idx>=vm->inst_count) continue;
    GmlInstance *in=&vm->inst[idx]; if(!in->active || in->marked) continue;
    uint32_t ip=u32(d,op+4+i*4);
    int cc=(int32_t)u32(d,ip+16);
    int code_before_create=vm->win->classic_version && !vm->win->classic_swap_creation_events;
    if(code_before_create && cc>=0 && cc<vm->win->n_code){
      GmlVal _r=gml_vm_run_code(vm,cc,in,NULL,NULL,0);
      if(_r.t==V_STR && _r.d!=0) free((char*)_r.s);
    }
    if(in->active && !in->marked){
      gml_run_event(vm,in,"PreCreate_0");   /* GMS2: variable-definitions, before Create */
      gml_run_event(vm,in,"Create_0");
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
  /* Run room creation code with a temporary self scope after instance creation.
   * The scratch instance is outside the instance pool and discarded afterward. */
  if(r.creation_code>=0 && r.creation_code<vm->win->n_code){
    static GmlInstance cc_scratch;
    memset(&cc_scratch,0,sizeof cc_scratch);
    cc_scratch.active=1; cc_scratch.obj=-1; cc_scratch.id=0;
    cc_scratch.image_xscale=cc_scratch.image_yscale=1; cc_scratch.image_alpha=1;
    cc_scratch.sprite_index=-1; cc_scratch.mask_index=-1; cc_scratch.path_index=-1;
    cc_scratch.timeline_index=-1; cc_scratch.timeline_speed=1;
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
  rebase_instance_order_to_slots(vm);
  gml_vm_warm_audio_for_room(vm,vm->room_index);
  vm_prefetch_room_assets(vm);
}
void gml_vm_goto_room_order(GmlVM *vm, int order_index){
  GmlWin *w=vm->win;
  int idx = (order_index>=0 && order_index<w->n_room_order)? (int)w->room_order[order_index] : order_index;
  gml_room_enter(vm,idx);
}

/* Apply one numeric assignment from a caller-supplied string.
 * room=N queues a room transition; name=V and name[i]=V write a global array slot.
 * Return 1 for a global write that the caller may repeat each frame, or 0 for a
 * room transition or parse failure. */
#define GML_CHEAT_NAMECH(c) (((c)>='a'&&(c)<='z')||((c)>='A'&&(c)<='Z')||((c)>='0'&&(c)<='9')||(c)=='_')
int gml_cheat_apply(GmlVM *vm, const char *code){
  if(!vm || !code) return 0;
  while(*code==' '||*code=='\t') code++;
  const char *n=code; while(*code && GML_CHEAT_NAMECH(*code)) code++;
  int nlen=(int)(code-n);
  if(nlen<=0 || nlen>=64) return 0;
  char name[64]; memcpy(name,n,(size_t)nlen); name[nlen]=0;
  int idx=0;
  if(*code=='['){ code++; idx=atoi(code); while(*code && *code!=']') code++; if(*code==']') code++; }
  while(*code==' ') code++;
  if(*code!='=') return 0;
  code++; while(*code==' ') code++;
  double val=atof(code);
  if(!strcmp(name,"room")){
    int target=(int)val;
    gml_vm_warm_audio_for_room(vm,target);
    vm->pending_room=target;
    return 0; }   /* one-shot warp (room index) */
  if(idx<0) idx=0;
  gml_set_global_arr(vm,name,idx,val);
  return 1;   /* sticky: re-apply each frame to freeze */
}

static void run_collisions(GmlVM *vm);
static void run_boundary_events(GmlVM *vm);
static int studio_step_snapshot_member(GmlVM *vm, const GmlInstance *in);

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
static void run_timelines(GmlVM *vm, int count){
  for(int i=0;i<count;i++){
    GmlInstance *in=&vm->inst[i];
    if(!in->active || in->marked || !studio_step_snapshot_member(vm,in) ||
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

static void run_classic_triggers(GmlVM *vm, int moment){
  const GmlChunk *chunk=vm && vm->win ? gml_chunk(vm->win,"TRIG") : NULL;
  if(!chunk || chunk->size<4) return;
  const uint8_t *data=vm->win->data;
  uint32_t count=u32(data,chunk->off);
  if(count>(chunk->size-4)/12) return;
  for(uint32_t i=0;i<count;i++){
    uint32_t entry=chunk->off+4+i*12;
    int trigger_id=(int32_t)u32(data,entry);
    int trigger_moment=(int32_t)u32(data,entry+4);
    int code_index=(int32_t)u32(data,entry+8);
    if(trigger_moment!=moment || code_index<0 || code_index>=vm->win->n_code) continue;
    char suffix[32]; snprintf(suffix,sizeof(suffix),"Trigger_%d",trigger_id);
    if(vm->win && vm->win->classic_version){
      int object_count=0;
      const int *objects=classic_event_objects(vm,suffix,&object_count);
      int extent=objects?object_count:vm->n_objects;
      for(int oi=0;oi<extent;oi++){
        int object=objects?objects[oi]:oi;
        if(!objects && !event_lookup_from(vm,suffix,object,NULL,NULL)) continue;
        int count=classic_collect_object_slots(vm,object); if(count<0) continue;
        for(int k=count-1;k>=0;k--){ int slot=vm->event_ord[k];
          if(slot>=vm->inst_count) continue;
          GmlInstance *in=&vm->inst[slot];
          if(!in->active||in->marked||in->obj!=object) continue;
          int old_type=vm->event_type, old_number=vm->event_number;
          vm->event_type=11; vm->event_number=trigger_id;
          GmlVal result=gml_vm_run_code(vm,code_index,in,NULL,NULL,0);
          vm->event_type=old_type; vm->event_number=old_number;
          int fire=astrue(result);
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
    int fire=astrue(result);
    if(result.t==V_STR && result.d!=0) free((char*)result.s);
    varmap_free_ex(&scratch.vars,0);
    if(!fire) continue;
    int n=vm->inst_count;
    for(int instance=0;instance<n;instance++)
      if(vm->inst[instance].active && !vm->inst[instance].marked)
        gml_run_event(vm,&vm->inst[instance],suffix);
  }
}

long g_vm_frame=0;
static int studio_step_snapshot_member(GmlVM *vm, const GmlInstance *in){
  return !vm->step_active || !vm->win || vm->win->classic_version ||
         in->id < vm->step_first_id;
}
/* GML_PROFILE_VM: per-phase wall time of gml_vm_step, printed every 300 frames (Linux dev aid) */
static struct { double anim,step1,alarms,input,step0,move,coll,step2,rest; long frames; } g_vmprof;
static double vmprof_now(void){
  return vm_profile_now_ms();
}
static int vmprof_on(void){ static int on=-1; if(on<0) on=getenv("GML_PROFILE_VM")!=NULL; return on; }
#define VMPROF_MARK(field) do{ if(pv){ double _t=vmprof_now(); g_vmprof.field += _t-pv_t; pv_t=_t; } }while(0)

static void advance_instance_animations(GmlVM *vm){
  GmlRender *R=(GmlRender*)vm->render;
  const char *anim_dbg=getenv("GML_ANIM_OBJ");
  int snapshot=vm->win && !vm->win->classic_version && vm->win->bytecode>=17;
  int extent=(snapshot && vm->step_active)?vm->step_alloc_base:vm->inst_count;
  for(int i=0;i<extent;i++){
    GmlInstance *in=&vm->inst[i];
    if(!in->active||in->marked||
       (snapshot && !studio_step_snapshot_member(vm,in))) continue;
    int nf=R?gml_sprite_frames(R,(int)in->sprite_index):0;
    if(anim_dbg){ const char *on=(in->obj>=0&&in->obj<vm->n_objects)?vm->objects[in->obj].name:"";
      if(on && strstr(on,anim_dbg)){
        int si=(int)in->sprite_index;
        const char *sn=(R&&si>=0&&si<R->n_spr&&R->spr[si].name)?R->spr[si].name:"?";
        fprintf(stderr,"[anim] %s x=%.1f y=%.1f vs=%.1f spr=%d(%s) idx=%.2f nf=%d\n",
          on,in->x,in->y,in->vspeed,si,sn,in->image_index,nf);
      }
    }
    if(in->image_speed!=0 && (nf>0 || (vm->win && vm->win->classic_version))){
      double step=(vm->win && !vm->win->classic_version)
        ? gml_sprite_animation_delta(R,(int)in->sprite_index,in->image_speed,gml_room_speed(vm))
        : in->image_speed;
      double ni=in->image_index+step;
      int wrapped=nf>0 && ((ni>=nf)||(ni<0));
      if(nf>0){ while(ni>=nf) ni-=nf; while(ni<0) ni+=nf; }
      in->image_index=ni;
      if(wrapped || (nf<=0 && ni>=0)){
        if(getenv("GML_LOG_ANIM")) fprintf(stderr,"[anim-end] %s (nf=%d)\n",
          (in->obj>=0&&in->obj<vm->n_objects)?vm->objects[in->obj].name:"?",nf);
        gml_run_event(vm,in,"Other_7");
      }
    }
  }
}

void gml_vm_post_draw(GmlVM *vm){
  if(vm && vm->win && vm->win->classic_version) advance_instance_animations(vm);
}

static int classic_joystick_event_fires(int s){
  extern int gml_input_gamepad(int button,int edge);
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
  return device==0 && gml_input_gamepad(control,0);
}
static int mouse_event_fires(int s,int hov,int was,int held,int pressed,int released,int wheel){
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
    case 61:return wheel<0;             default:return classic_joystick_event_fires(s);
  }
}
void gml_vm_step(GmlVM *vm){
  if(vm && vm->classic_info_active){
    if(gml_keyboard_check(vm,1,1)) vm->classic_info_active=0;
    return;
  }
  g_vm_frame++;
  /* GMS2 layers scroll by their hspeed/vspeed each step. Runtime-scripted layers accumulate here;
   * untouched ones are derived on the fly from the room definition (see the draw path). */
  for(int i=0;i<vm->n_rtl;i++) if(vm->rtl[i].used && vm->rtl[i].touched){
    vm->rtl[i].x += vm->rtl[i].hs; vm->rtl[i].y += vm->rtl[i].vs; }
  { GmlRender *R=(GmlRender*)vm->render;
    for(int i=0;i<vm->n_rte;i++){
      GmlRtElem *e=&vm->rte[i];
      if(!e->used || (e->type!=1 && e->type!=3) || e->image_speed==0) continue;
      /* GMS2 background layers own an animated sprite subimage. Classic and Studio 1 room
       * backgrounds have a different cadence path; compatibility records must not advance as
       * modern layer elements. */
      if(e->type==1 && (!vm->win || vm->win->bytecode<17)) continue;
      int nf=R?gml_sprite_frames(R,e->sprite):0;
      e->image_index += gml_sprite_animation_delta(R,e->sprite,e->image_speed,gml_room_speed(vm));
      if(nf>0){
        while(e->image_index>=nf) e->image_index-=nf;
        while(e->image_index<0) e->image_index+=nf;
      }
    }
  }
  /* Struct GC between frames (stack/locals empty here). GMS2.3 games mint transient structs every step
   * (a menu returning a fresh palette/description struct ~2-12/frame) that nothing keeps — without this the
   * pool grew ~unboundedly (500MB/h under menu load, cap hit in ~1.5h). No-op for games with no structs. */
  if(vm->n_structs>0 && g_vm_frame - vm->structs_last_gc_frame >= 120){
    vm->structs_last_gc_frame = g_vm_frame; gml_struct_gc(vm); }
  int pv=vmprof_on(); double pv_t=pv?vmprof_now():0;
  int n=vm->inst_count;
  int prev_alloc_base=vm->step_alloc_base;
  vm->step_active=1;
  vm->step_first_id=vm->next_id;
  prepare_step_free_slots(vm,n);
  vm->step_alloc_base=n;
  /* xprevious/yprevious describe the position at the start of this step. This
   * must precede user Step code: classic games commonly move by assigning x/y
   * directly, and solid collision rollback needs the position from before
   * those assignments. */
  for(int i=0;i<n;i++) if(vm->inst[i].active && !vm->inst[i].marked){
    vm->inst[i].xprevious=vm->inst[i].x;
    vm->inst[i].yprevious=vm->inst[i].y;
  }
  /* Studio advances animation before Step. GM6-8 advances it after the draw phase; the frontend
   * calls gml_vm_post_draw() once the complete classic frame has been rendered. */
  if(!vm->win || !vm->win->classic_version) advance_instance_animations(vm);
  VMPROF_MARK(anim);
  /* Override an explicitly selected object family alarm using debug parameters. */
  { static int god_env=-1, god_alarm=0, god_value=120; static const char *god_obj=(const char*)-1;
    if(god_env<0){
      const char *g=getenv("GML_GOD");
      const char *o=getenv("GML_GOD_OBJ");
      god_env=g!=NULL;
      if((!o || !*o) && g && *g && strcmp(g,"1") && strcmp(g,"on") && strcmp(g,"true")) o=g;
      god_obj=(o && *o)?o:NULL;
      const char *a=getenv("GML_GOD_ALARM"); if(a && *a) god_alarm=atoi(a);
      if(god_alarm<0 || god_alarm>=GML_ALARMS) god_alarm=0;
      const char *v=getenv("GML_GOD_VALUE"); if(v && *v) god_value=atoi(v);
      if(god_value<0) god_value=0;
    }
    if((god_env || vm->god_mode) && god_obj){
      int po=gml_object_index_by_name(vm,god_obj);
      GmlInstance *pl = po>=0 ? gml_find_instance(vm,po) : NULL;
      if(pl) pl->alarm[god_alarm]=god_value;
    } }
  /* begin step */
  run_classic_triggers(vm,1);
  if(vm->win && vm->win->classic_version) run_classic_object_event(vm,"Step_1");
  else for(int i=0;i<n;i++) if(vm->inst[i].active && !vm->inst[i].marked &&
      studio_step_snapshot_member(vm,&vm->inst[i]))
    gml_run_event(vm,&vm->inst[i],"Step_1");
  VMPROF_MARK(step1);
  /* Tick both built-in time-source trees after every Begin Step and before the remaining
   * Step phases. Sources created by a callback join on the next tick via the scheduler snapshot. */
  gml_time_sources_tick(vm);
  run_timelines(vm,n);
  /* Alarm thresholds depend on bytecode version: below 16, decrement values
   * greater than -1 and fire below zero; later versions decrement positive values
   * and fire at or below zero. Set -1 before dispatch so handlers can re-arm. */
  int classic_alarm_order=vm->win && vm->win->classic_version;
  int alarm_at_zero = vm->win && (classic_alarm_order || vm->win->bytecode >= 16);
  if(classic_alarm_order){
    for(int a=0;a<GML_ALARMS;a++){
      char s[16]; snprintf(s,sizeof s,"Alarm_%d",a);
      int object_count=0;
      const int *objects=classic_event_objects(vm,s,&object_count);
      int extent=objects?object_count:vm->n_objects;
      for(int oi=0;oi<extent;oi++){
        int object=objects?objects[oi]:oi;
        if(!objects && !event_lookup_from(vm,s,object,NULL,NULL)) continue;
        int count=classic_collect_object_slots(vm,object);
        if(count<0) continue;
        for(int k=count-1;k>=0;k--){ int i=vm->event_ord[k];
          if(i>=vm->inst_count) continue;
          GmlInstance *in=&vm->inst[i];
          if(!in->active||in->marked||in->obj!=object||!(in->alarm[a]>0)) continue;
          in->alarm[a]-=1;
          if(in->alarm[a]<=0){ in->alarm[a]=-1;
            if(getenv("GML_LOG_ALARM")) fprintf(stderr,"[alarm] f%ld %s.%s\n",g_vm_frame,
              vm->objects[in->obj].name,s);
            gml_run_event(vm,in,s); }
        }
      }
    }
  } else for(int i=0;i<n;i++){ GmlInstance *in=&vm->inst[i];
    if(!in->active||in->marked||!studio_step_snapshot_member(vm,in)) continue;
    for(int a=0;a<GML_ALARMS;a++){
      if(alarm_at_zero){ if(!(in->alarm[a]>0)) continue; } else { if(!(in->alarm[a]>-1)) continue; }
      char s[16]; snprintf(s,sizeof s,"Alarm_%d",a);
      int declared_code=-1;
      int native_declared=native_event_declared_from(vm,2,a,in->obj,NULL,&declared_code);
      /* Alarms count down only when this object family declares the matching event. Native
       * records matter here because an empty event has no CODE name but still owns the countdown.
       * A genuinely unused alarm slot remains an ordinary writable value. */
      if(!native_declared && !event_lookup_from(vm,s,in->obj,NULL,NULL)) continue;
      in->alarm[a]-=1;
      int fire = alarm_at_zero ? (in->alarm[a]<=0) : (in->alarm[a]<0);
      if(fire){ in->alarm[a]=-1;
        if(getenv("GML_LOG_ALARM")) fprintf(stderr,"[alarm] f%ld %s.%s\n",g_vm_frame,
          (in->obj>=0&&in->obj<vm->n_objects)?vm->objects[in->obj].name:"?",s);
        /* An explicitly empty declaration completes at -1 without inheriting or executing code. */
        if(!native_declared || declared_code>=0) gml_run_event(vm,in,s); } } }
  VMPROF_MARK(alarms);
  /* keyboard events (GM order: after alarms, before the normal step) */
  if(vm->n_key_events){
    for(int e=0;e<vm->n_key_events;e++){
      if(!gml_keyboard_check(vm,vm->key_events[e].vk,vm->key_events[e].kind)) continue;
      if(vm->win && vm->win->classic_version)
        run_classic_object_event(vm,vm->key_events[e].suffix);
      else for(int i=0;i<n;i++){
        if(vm->inst[i].active && !vm->inst[i].marked &&
           studio_step_snapshot_member(vm,&vm->inst[i]))
          gml_run_event(vm,&vm->inst[i],vm->key_events[e].suffix); }
    }
  }
  /* instance Mouse_<n> events (with the other input events). Hover = pointer (room coords)
   * inside the instance bbox; enter/leave tracked per instance in mouse_over. Subtypes per GM:
   * 0-2 button held over it, 3 no-button over it, 4-6 pressed, 7-9 released, 10 enter, 11 leave,
   * 16-28/31-43 joystick 1/2, 50-58 global (no hover), 60/61 wheel. Only games that define
   * Mouse_* events pay any cost. */
  if(vm->n_mouse_events){
    extern void gml_input_mouse(double*,double*,double*,double*,double*,double*,int*,int*,int*,int*);
    double mx,my; int mheld,mpressed,mreleased,mwheel;
    gml_input_mouse(&mx,&my,NULL,NULL,NULL,NULL,&mheld,&mpressed,&mreleased,&mwheel);
    if(vm->win && vm->win->classic_version){
      for(int e=0;e<vm->n_mouse_events;e++){
        int s=vm->mouse_events[e].sub;
        int object_count=0;
        const int *objects=classic_event_objects(vm,vm->mouse_events[e].suffix,&object_count);
        int extent=objects?object_count:vm->n_objects;
        for(int oi=0;oi<extent;oi++){
          int object=objects?objects[oi]:oi;
          if(!objects && !event_lookup_from(vm,vm->mouse_events[e].suffix,object,NULL,NULL)) continue;
          int count=classic_collect_object_slots(vm,object); if(count<0) continue;
          for(int k=count-1;k>=0;k--){ int i=vm->event_ord[k];
            if(i>=vm->inst_count) continue;
            GmlInstance *in=&vm->inst[i];
            if(!in->active||in->marked||in->deactivated||in->obj!=object) continue;
            double l=0,t=0,r2=0,b=0; int hov=0;
            int have_bbox=vm_bbox(vm,in,&l,&t,&r2,&b);
            if(have_bbox) hov=mx>=l&&mx<=r2&&my>=t&&my<=b;
            if(getenv("GML_LOG_MOUSE_HIT") && mpressed)
              fprintf(stderr,"[mouse-hit] %s at=%.1f,%.1f bbox=%d:%.1f,%.1f..%.1f,%.1f hover=%d event=%s\n",
                (in->obj>=0&&in->obj<vm->n_objects)?vm->objects[in->obj].name:"?",
                mx,my,have_bbox,l,t,r2,b,hov,vm->mouse_events[e].suffix);
            if(mouse_event_fires(s,hov,in->mouse_over,mheld,mpressed,mreleased,mwheel))
              gml_run_event(vm,in,vm->mouse_events[e].suffix);
          }
        }
      }
      for(int i=0;i<vm->inst_count;i++){ GmlInstance *in=&vm->inst[i];
        if(!in->active||in->marked||in->deactivated) continue;
        double l,t,r2,b; int hov=0;
        if(vm_bbox(vm,in,&l,&t,&r2,&b)) hov=mx>=l&&mx<=r2&&my>=t&&my<=b;
        in->mouse_over=(unsigned char)hov;
      }
    } else for(int i=0;i<n;i++){
      GmlInstance *in=&vm->inst[i];
      if(!in->active||in->marked||in->deactivated||!studio_step_snapshot_member(vm,in)) continue;
      double l,t,r2,b; int hov=0;
      if(vm_bbox(vm,in,&l,&t,&r2,&b)) hov=mx>=l&&mx<=r2&&my>=t&&my<=b;
      unsigned char was=in->mouse_over; in->mouse_over=(unsigned char)hov;
      for(int e=0;e<vm->n_mouse_events;e++) if(mouse_event_fires(vm->mouse_events[e].sub,hov,was,mheld,mpressed,mreleased,mwheel))
        gml_run_event(vm,in,vm->mouse_events[e].suffix);
    }
  }
  VMPROF_MARK(input);
  /* Normal Step in classic formats is grouped by ascending object resource, with insertion order
   * inside each exact object. Each object takes its extent when that group begins: an earlier
   * object can create an instance whose later object group still sees it, while a same-object
   * creation waits until the next Step. Studio retains its verified flat snapshot. */
  run_classic_triggers(vm,0);
  if(vm->win && vm->win->classic_version) run_classic_object_event(vm,"Step_0");
  else {
    for(int i=0;i<n;i++) if(vm->inst[i].active && !vm->inst[i].marked &&
        studio_step_snapshot_member(vm,&vm->inst[i]))
      gml_run_event(vm,&vm->inst[i],"Step_0");
  }
  VMPROF_MARK(step0);
  /* Classic object iteration remains live through automatic movement. An instance
   * created by an earlier normal-Step handler already participates in that Step
   * phase, and GM6-8 also applies its freshly assigned speed/gravity before the
   * first draw. Studio keeps the frame-start snapshot verified by its fixtures. */
  int movement_count=(vm->win && vm->win->classic_version)?vm->inst_count:n;
  for(int i=0;i<movement_count;i++){ GmlInstance *in=&vm->inst[i];
    if(!in->active||in->marked||!studio_step_snapshot_member(vm,in)) continue;
    if(in->gravity!=0){ in->hspeed+=in->gravity*cos(in->gravity_direction*M_PI/180.0);
      in->vspeed-=in->gravity*sin(in->gravity_direction*M_PI/180.0); motion_from_components(in); }
    if(in->friction!=0 && in->speed!=0){
      if(in->speed>0){
        if(in->friction>in->speed) in->speed=0;
        else in->speed-=in->friction;
      } else {
        if(in->friction>-in->speed) in->speed=0;
        else in->speed+=in->friction;
      }
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
  run_classic_triggers(vm,2);
  if(vm->win && vm->win->classic_version) run_classic_object_event(vm,"Step_2");
  else for(int i=0;i<n;i++) if(vm->inst[i].active && !vm->inst[i].marked &&
      studio_step_snapshot_member(vm,&vm->inst[i]))
    gml_run_event(vm,&vm->inst[i],"Step_2");
  VMPROF_MARK(step2);
  reap(vm);
  { const char *iv=getenv("GML_LOG_INSTVAR");   /* obj_name[@id]:var1,var2 — dump instance vars per frame */
    if(iv && *iv){ char buf[256]; snprintf(buf,sizeof buf,"%s",iv);
      char *colon=strchr(buf,':');
      if(colon){ *colon=0; unsigned wanted_id=0; char *at=strchr(buf,'@');
        if(at){ *at=0; wanted_id=(unsigned)strtoul(at+1,NULL,10); }
        int oi=gml_object_index_by_name(vm,buf); GmlInstance *in=NULL;
        if(wanted_id){ for(int i=0;i<vm->inst_count;i++) if(vm->inst[i].active &&
            !vm->inst[i].marked && vm->inst[i].id==wanted_id){ in=&vm->inst[i]; break; } }
        else if(oi>=0) in=gml_find_instance(vm,oi);
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
  int follow_views=(vm->win && vm->win->classic_version)?8:1;
  for(int view=0;view<follow_views;view++) if(get_global_arr_d(vm,"view_visible",view)>=0.5){
    int vobj=(int)get_global_arr_d(vm,"view_object",view);
    GmlInstance *fo = vobj>=0 ? gml_find_instance(vm,vobj) : NULL;
    if(fo && view==0 && getenv("GML_LOG_FOLLOW")){ static int f2=0; if(f2++%60==0)
      fprintf(stderr,"[follow-target] obj=%d(%s) id=%u at(%.0f,%.0f) hb=%.0f\n",
        fo->obj,(fo->obj>=0&&fo->obj<vm->n_objects)?vm->objects[fo->obj].name:"?",fo->id,fo->x,fo->y,
        get_global_arr_d(vm,"view_hborder",view)); }
    if(fo){
      double vx=get_global_arr_d(vm,"view_xview",view), vy=get_global_arr_d(vm,"view_yview",view);
      double wv=get_global_arr_d(vm,"view_wview",view), hv=get_global_arr_d(vm,"view_hview",view);
      double hb=get_global_arr_d(vm,"view_hborder",view), vb=get_global_arr_d(vm,"view_vborder",view);
      double tx=fo->x, ty=fo->y;
      int classic=vm->win && vm->win->classic_version;
      if(classic){ tx=classic_round_even(tx); ty=classic_round_even(ty); }
      /* A border of at least half the view centers the target. Otherwise a classic positive
       * speed caps the correction per step; zero holds the view and a negative value snaps. */
      if(2*hb >= wv) vx=tx-wv/2;
      else if(tx-hb < vx){
        double wanted=tx-hb;
        if(classic){ double speed=get_global_arr_d(vm,"view_hspeed",view);
          vx=speed<0?wanted:vx-fmin(vx-wanted,fmax(speed,0)); }
        else vx=wanted;
      } else if(tx+hb > vx+wv){
        double wanted=tx+hb-wv;
        if(classic){ double speed=get_global_arr_d(vm,"view_hspeed",view);
          vx=speed<0?wanted:vx+fmin(wanted-vx,fmax(speed,0)); }
        else vx=wanted;
      }
      if(2*vb >= hv) vy=ty-hv/2;
      else if(ty-vb < vy){
        double wanted=ty-vb;
        if(classic){ double speed=get_global_arr_d(vm,"view_vspeed",view);
          vy=speed<0?wanted:vy-fmin(vy-wanted,fmax(speed,0)); }
        else vy=wanted;
      } else if(ty+vb > vy+hv){
        double wanted=ty+vb-hv;
        if(classic){ double speed=get_global_arr_d(vm,"view_vspeed",view);
          vy=speed<0?wanted:vy+fmin(wanted-vy,fmax(speed,0)); }
        else vy=wanted;
      }
      GmlRoom rm; if(gml_room_get(vm->win,vm->room_index,&rm)==0){
        double mx=rm.width-wv, my=rm.height-hv;
        if(vx<0)vx=0; if(mx>0&&vx>mx)vx=mx; if(mx<=0)vx=0;
        if(vy<0)vy=0; if(my>0&&vy>my)vy=my; if(my<=0)vy=0;
      }
      set_global_arr(vm,"view_xview",view,vx); set_global_arr(vm,"view_yview",view,vy);
    }
  }
  /* Studio camera resources have their own target, border and speed state. Binding a camera
   * through view_camera[] does not copy those fields into the legacy view_* arrays: live camera
   * resources advance after Step and the renderer later resolves the bound handle. Consequently,
   * camera_create_view(..., target, speed, border) follows its target without an explicit
   * camera_set_view_pos call. */
  for(int camera=0;camera<64;camera++){
    if(get_global_arr_d(vm,"__gml_camera_live",camera)<0.5) continue;
    int target=(int)get_global_arr_d(vm,"__gml_camera_target",camera);
    GmlInstance *fo=target>=0?gml_find_instance(vm,target):NULL;
    if(!fo) continue;
    double vx=get_global_arr_d(vm,"__gml_camera_x",camera);
    double vy=get_global_arr_d(vm,"__gml_camera_y",camera);
    double wv=get_global_arr_d(vm,"__gml_camera_w",camera);
    double hv=get_global_arr_d(vm,"__gml_camera_h",camera);
    if(wv<=0 || hv<=0) continue;
    double hb=get_global_arr_d(vm,"__gml_camera_xborder",camera);
    double vb=get_global_arr_d(vm,"__gml_camera_yborder",camera);
    double hs=get_global_arr_d(vm,"__gml_camera_xspeed",camera);
    double vs=get_global_arr_d(vm,"__gml_camera_yspeed",camera);
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
    if(gml_room_get(vm->win,vm->room_index,&rm)==0){
      double mx=(double)rm.width-wv, my=(double)rm.height-hv;
      if(vx<0) vx=0; if(mx>0 && vx>mx) vx=mx; if(mx<=0) vx=0;
      if(vy<0) vy=0; if(my>0 && vy>my) vy=my; if(my<=0) vy=0;
    }
    set_global_arr(vm,"__gml_camera_x",camera,vx);
    set_global_arr(vm,"__gml_camera_y",camera,vy);
    if(camera==0 && getenv("GML_LOG_FOLLOW")){ static int cf=0; if(cf++%60==0)
      fprintf(stderr,"[camera-follow] cam=%d target=%d(%s) at=(%.0f,%.0f) view=(%.0f,%.0f %.0fx%.0f) border=(%.0f,%.0f) speed=(%.0f,%.0f)\n",
        camera,target,(fo->obj>=0&&fo->obj<vm->n_objects)?vm->objects[fo->obj].name:"?",
        fo->x,fo->y,vx,vy,wv,hv,hb,vb,hs,vs); }
  }
  /* room transition requested during the step */
  /* Classic room backgrounds start at their authored position for the first rendered frame;
   * their automatic speed is applied between frames. The step precedes drawing in this runtime,
   * so skip the first room step to preserve that phase. */
  if(!vm->win || !vm->win->classic_version || g_vm_frame-vm->room_enter_frame>1)
    for(int i=0;i<8;i++){
      double hx=get_global_arr_d(vm,"background_hspeed",i), vy=get_global_arr_d(vm,"background_vspeed",i);
      if(hx!=0 || vy!=0){
        set_global_arr(vm,"background_x",i,get_global_arr_d(vm,"background_x",i)+hx);
        set_global_arr(vm,"background_y",i,get_global_arr_d(vm,"background_y",i)+vy);
      }
    }
  /* deferred async save/load completion events (Other_72) queued by buffer_*_async this step */
  gml_fire_async_saveload(vm);
  /* deferred async HTTP failure events (Other_62) queued by http_* this step (offline core) */
  gml_fire_async_http(vm);
  /* room transition requested during the step */
  if(vm->pending_room>=0){
    int t=vm->pending_room;
    vm->pending_room=-1;
    vm->step_alloc_base=0;
    gml_room_enter(vm,t);
  }
  /* Game End (Other_3): fire on all active instances when game_end was set. */
  if(vm->game_end){
    for(int i=0;i<vm->inst_count;i++) if(vm->inst[i].active && !vm->inst[i].marked)
      gml_run_event(vm,&vm->inst[i],"Other_3");
  }
  vm->step_alloc_base=prev_alloc_base;
  vm->step_active=0;
  vm->step_free_n=vm->step_free_pos=0;
  trim_instance_pool_tail(vm);
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
  if(i>=0){ if(hidden) vm->tile_mut[i].flags|=TILE_MUT_HIDDEN; else vm->tile_mut[i].flags&=~TILE_MUT_HIDDEN;
    if(getenv("GML_LOG_RTL")){ extern long g_vm_frame;
      fprintf(stderr,"[rtl] f%ld tile layer depth=%d hidden=%d flags=%d\n",
              g_vm_frame,depth,hidden,vm->tile_mut[i].flags); } } }
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
typedef struct { double x,y,xs,ys; int def,sx,sy,w,h,order; } GmlDrawTile;
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
typedef struct { double depth; int seq, type, idx, order, element_order, classic, obj, placed; } GmlDrawItem;  /* type: 0=instance, 1=tile, 2=layer tile, 3=layer bg, 4=particle system, 5=layer sprite, 6=classic bg, 7=layer effect */
static int cmp_draw_item(const void *pa, const void *pb){
  const GmlDrawItem *a=pa,*b=pb;
  if(a->depth!=b->depth) return a->depth>b->depth? -1:1;     /* higher depth first (behind) */
  if(a->order>=0 && b->order>=0 && a->order!=b->order)
    return a->order>b->order? -1:1;                           /* GMS2 layer list: later/back layers first */
  /* At equal depth, room tiles draw above instances and retain room-list order. Dynamic depth
   * layers precede named instance layers, peers use reverse creation order, and runtime layer
   * items retain the sequence fallback below instances. */
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
  if(a->type==0 && b->type==0 && a->classic && b->classic && a->placed && a->obj!=b->obj)
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
  return a->seq>b->seq? -1 : (a->seq<b->seq?1:0);
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
  static GmlInstance layer_scratch;
  int pet=vm->event_type, pen=vm->event_number;
  memset(&layer_scratch,0,sizeof layer_scratch);
  layer_scratch.active=1; layer_scratch.obj=-1; layer_scratch.id=0;
  layer_scratch.image_xscale=layer_scratch.image_yscale=1; layer_scratch.image_alpha=1;
  layer_scratch.sprite_index=-1; layer_scratch.mask_index=-1; layer_scratch.path_index=-1;
  for(int a2=0;a2<GML_ALARMS;a2++) layer_scratch.alarm[a2]=-1;
  vm->event_type=8; vm->event_number=0;
  GmlVal _r=gml_vm_run_code(vm,ci,&layer_scratch,NULL,NULL,0);
  if(_r.t==V_STR && _r.d!=0) free((char*)_r.s);
  varmap_free_ex(&layer_scratch.vars,0);
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
struct LayBg { int sprite,subimg; int th,tv,stretch,order; double x,y,xs,ys; uint32_t blend; double alpha; double depth; };
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
static struct LayBg    *g_dl_lbg;   static int g_dl_lbg_cap;
static struct LayTile  *g_dl_ltl;   static int g_dl_ltl_cap;
static struct LaySprite*g_dl_lsp;   static int g_dl_lsp_cap;
static struct LayEffect*g_dl_lfx;   static int g_dl_lfx_cap;
static struct LayAttachedFilter *g_dl_laf; static int g_dl_laf_cap;
static GmlDrawItem     *g_dl_it;    static int g_dl_it_cap;
static GmlDrawTile     *g_dl_tiles; static double *g_dl_tdepth; static int g_dl_tiles_cap;
/* grow *pp (element size esz) to hold at least `need` elements, doubling capacity. Returns 1 on ok. */
static int dl_grow(void **pp, int *cap, int need, size_t esz){
  if(need<=*cap) return 1;
  int nc = *cap>0 ? *cap : 64;
  while(nc<need) nc*=2;
  void *p=realloc(*pp,(size_t)nc*esz);
  if(!p) return 0;
  *pp=p; *cap=nc; return 1;
}
void gml_vm_draw(GmlVM *vm){
  GmlRender *R=(GmlRender*)vm->render; if(!R) return;
  int n=vm->inst_count;
  /* gather this room's tiles (pointer-list of records: x,y,def,srcx,srcy,w,h,depth,...).
   * Persistent scratch: draw_tile_add grows g_dl_tiles/g_dl_tdepth by doubling and they survive
   * across frames, avoiding tile-buffer allocation while the existing capacity suffices. */
  GmlDrawTile *tiles=g_dl_tiles; int nt=0, tcap=g_dl_tiles_cap; double *tdepth=g_dl_tdepth;
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
	draw_tile_add(&tiles,&tdepth,&nt,&tcap,dt,tdep,-1); } }
  }
  /* ROOM layer records carry background, instance and asset-tile layers.
   * Use the detected type-data offset, including optional effect fields. */
  struct LayBg *lbg=g_dl_lbg; int nlb=0;
  struct LayTile *ltl=g_dl_ltl; int nlt=0;
  struct LaySprite *lsp=g_dl_lsp; int nls=0;
  struct LayEffect *lfx=g_dl_lfx; int nlf=0;
  struct LayAttachedFilter *laf=g_dl_laf; int naf=0;
  struct ClassicBg cbg[9]; int ncb=0;
  if(rm.draw_bg){
    cbg[ncb].def=-1; cbg[ncb].th=cbg[ncb].tv=cbg[ncb].stretch=0;
    cbg[ncb].x=cbg[ncb].y=0; cbg[ncb].blend=rm.bgcolor&0xFFFFFFu;
    cbg[ncb].alpha=((rm.bgcolor>>24)&0xFF)/255.0; cbg[ncb].depth=1.1e300; ncb++;
  }
  for(int i=0;i<8;i++){
    if(get_global_arr_d(vm,"background_visible",i)<0.5) continue;
    int def=(int)get_global_arr_d(vm,"background_index",i);
    if(def<0 || def>=R->n_bg) continue;
    struct ClassicBg *bg=&cbg[ncb++];
    bg->def=def;
    bg->x=get_global_arr_d(vm,"background_x",i); bg->y=get_global_arr_d(vm,"background_y",i);
    bg->th=get_global_arr_d(vm,"background_htiled",i)>=0.5;
    bg->tv=get_global_arr_d(vm,"background_vtiled",i)>=0.5;
    bg->stretch=get_global_arr_d(vm,"background_stretch",i)>=0.5;
    bg->blend=(uint32_t)get_global_arr_d(vm,"background_blend",i);
    bg->alpha=get_global_arr_d(vm,"background_alpha",i);
    bg->depth=get_global_arr_d(vm,"background_foreground",i)>=0.5 ? -1.0e300 : 1.0e300;
  }
  {
    const uint8_t *d=vm->win->data;
    uint32_t lcnt=0;
    uint32_t lay=gml_room_layer_list(vm,vm->room_index,&lcnt);
    if(lcnt>0 && lcnt<512){
      long fin = g_vm_frame - vm->room_enter_frame; if(fin<0) fin=0;
      for(uint32_t i=0;i<lcnt;i++){ uint32_t lp=u32(d,lay+4+i*4);
        if(!lp || lp+44>vm->win->size) continue;
        uint32_t ltype=u32(d,lp+8); double ldep=(double)(int32_t)u32(d,lp+12);
        double lx=f32(d,lp+16), ly=f32(d,lp+20), lhs=f32(d,lp+24), lvs=f32(d,lp+28);
        /* Runtime layer control: if the game moved this layer (layer_x/layer_hspeed by name), use its
         * live accumulated position; otherwise the exact room-def scroll model (byte-identical for
         * games that never script their layers). */
        uint32_t lnp=u32(d,lp+0);
        GmlRtLayer *rl=(lnp && lnp<vm->win->size)? gml_rt_layer_find_by_name(vm,(const char*)(d+lnp)):NULL;
        if(rl) ldep=rl->depth;
        int lorder = rl ? rl->order : (int)i;
        int ltouch = rl && rl->touched;
        double lox = ltouch ? rl->x : lx+lhs*fin;   /* background layer origin (scrolls) */
        double loy = ltouch ? rl->y : ly+lvs*fin;
        double ltx = ltouch ? rl->x : lx;           /* tile layer origin (room-def raw, unchanged) */
        double lty = ltouch ? rl->y : ly;
        if(rl ? !rl->visible : !u32(d,lp+32)) continue;
        GmlLayerFilter layer_filter;
        int has_layer_filter=gml_room_layer_effect(vm,lp,&layer_filter);
        if(has_layer_filter && getenv("GML_LOG_LAYER_EFFECT") &&
           (g_vm_frame<4 || (g_vm_frame%60)==0))
          fprintf(stderr,"[layer-effect] f%ld order=%d type=%u depth=%.0f kind=%d sampler-sprite=%d\n",
                  g_vm_frame,lorder,ltype,ldep,layer_filter.kind,layer_filter.sampler_sprite);
        /* An effect attached to an ordinary room layer transforms just that layer. The sorted draw
         * loop below isolates the corresponding runtime order before applying the software filter.
         * Type-6 records remain standalone effect draw items. */
        if(has_layer_filter && ltype!=6){
          if(dl_grow((void**)&g_dl_laf,&g_dl_laf_cap,naf+1,sizeof(*laf))){
            laf=g_dl_laf; laf[naf].effect=layer_filter; laf[naf].order=lorder; naf++;
          }
        }
        if(ltype==1){
          if(rl && rt_layer_has_background(vm,rl->id)) continue;
          uint32_t b=gml_room_layer_type_off(vm,lp);
          if(b+28>vm->win->size) continue;
          if(!u32(d,b)) continue;     /* background not visible */
          int spr=(int32_t)u32(d,b+8);
          uint32_t col=u32(d,b+24);
          /* Use the layer color when no sprite is assigned; skip fully transparent color. */
          if(spr<0 && !(col>>24)) continue;
          if(!dl_grow((void**)&g_dl_lbg,&g_dl_lbg_cap,nlb+1,sizeof(*lbg))) continue; lbg=g_dl_lbg;
          lbg[nlb].sprite=spr; lbg[nlb].subimg=0;
          lbg[nlb].th=(int)u32(d,b+12); lbg[nlb].tv=(int)u32(d,b+16);
          lbg[nlb].stretch=(int)u32(d,b+20);
          lbg[nlb].xs=lbg[nlb].ys=1;
          lbg[nlb].blend=col&0xFFFFFF; lbg[nlb].alpha=((col>>24)&0xFF)/255.0;
          lbg[nlb].x=lox; lbg[nlb].y=loy; lbg[nlb].depth=ldep; lbg[nlb].order=lorder;
          nlb++;
        } else if(ltype==3){
          uint32_t tb3=gml_room_layer_type_off(vm,lp);
          uint32_t tl=(tb3+4<=vm->win->size)?u32(d,tb3):0;
          uint32_t tcnt=(tl && tl+4<vm->win->size)?u32(d,tl):0;
          if(tcnt==0 || tcnt>100000) continue;
          for(uint32_t k2=0;k2<tcnt;k2++){ uint32_t tp=u32(d,tl+4+k2*4);
            if(!tp || tp+48>vm->win->size) continue;
            if(!dl_grow((void**)&g_dl_ltl,&g_dl_ltl_cap,nlt+1,sizeof(*ltl))) continue; ltl=g_dl_ltl;
            ltl[nlt].x=ltx+(int32_t)u32(d,tp); ltl[nlt].y=lty+(int32_t)u32(d,tp+4);
            ltl[nlt].sprite=(int32_t)u32(d,tp+8);
            ltl[nlt].sx=(int32_t)u32(d,tp+12); ltl[nlt].sy=(int32_t)u32(d,tp+16);
            ltl[nlt].w=(int32_t)u32(d,tp+20); ltl[nlt].h=(int32_t)u32(d,tp+24);
            ltl[nlt].depth=ldep;
            ltl[nlt].order=lorder;
            ltl[nlt].xs=f32(d,tp+36); ltl[nlt].ys=f32(d,tp+40);
            uint32_t col=u32(d,tp+44);
            ltl[nlt].blend=col&0xFFFFFF; ltl[nlt].alpha=((col>>24)&0xFF)/255.0;
            nlt++; }
        } else if(ltype==6){
          if(!has_layer_filter) continue;
          if(!dl_grow((void**)&g_dl_lfx,&g_dl_lfx_cap,nlf+1,sizeof(*lfx))) continue; lfx=g_dl_lfx;
          lfx[nlf].effect=layer_filter; lfx[nlf].depth=ldep; lfx[nlf].order=lorder;
          nlf++;
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
    int pitch_x=tw+2*bx+gb->tile_separation_x;
    int pitch_y=th+2*by+gb->tile_separation_y;
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
    long fin2 = g_vm_frame - vm->room_enter_frame; if(fin2<0) fin2=0;
    double lx = l->touched ? l->x : l->x+l->hs*fin2;
    double ly = l->touched ? l->y : l->y+l->vs*fin2;
    double ldepth=l->depth;
    if(e->type==7){
      /* Legacy tile_layer_* operations address every tile at an authored depth, including
       * tiles created at runtime.  Static ROOM tiles already pass through tile_apply_mut above;
       * apply the same visibility/depth/shift transform to layer-backed dynamic tiles. */
      if(vm->n_tile_mut){ int ed; double ox,oy;
        if(!tile_apply_mut(vm,(int)l->depth,&ed,&ox,&oy)) continue;
        ldepth=ed; lx+=ox; ly+=oy;
      }
      if(!dl_grow((void**)&g_dl_ltl,&g_dl_ltl_cap,nlt+1,sizeof(*ltl))) continue; ltl=g_dl_ltl;
      ltl[nlt].sprite=e->sprite; ltl[nlt].sx=e->sx; ltl[nlt].sy=e->sy; ltl[nlt].w=e->w; ltl[nlt].h=e->h;
      ltl[nlt].x=lx+e->x; ltl[nlt].y=ly+e->y; ltl[nlt].xs=e->xs; ltl[nlt].ys=e->ys;
      ltl[nlt].blend=e->blend; ltl[nlt].alpha=e->alpha; ltl[nlt].depth=ldepth; ltl[nlt].order=l->order;
      nlt++;
    } else if(e->type==1){
      if(!dl_grow((void**)&g_dl_lbg,&g_dl_lbg_cap,nlb+1,sizeof(*lbg))) continue; lbg=g_dl_lbg;
      lbg[nlb].sprite=e->sprite; lbg[nlb].subimg=(int)floor(e->image_index);
      lbg[nlb].th=e->htiled; lbg[nlb].tv=e->vtiled; lbg[nlb].stretch=e->stretch;
      lbg[nlb].xs=e->xs; lbg[nlb].ys=e->ys;
      lbg[nlb].x=lx; lbg[nlb].y=ly; lbg[nlb].blend=e->blend; lbg[nlb].alpha=e->alpha; lbg[nlb].depth=l->depth; lbg[nlb].order=l->order;
      nlb++;
    } else if(e->type==3){
      if(!dl_grow((void**)&g_dl_lsp,&g_dl_lsp_cap,nls+1,sizeof(*lsp))) continue; lsp=g_dl_lsp;
      lsp[nls].sprite=e->sprite; lsp[nls].subimg=(int)e->image_index;
      lsp[nls].x=lx+e->x; lsp[nls].y=ly+e->y; lsp[nls].xs=e->xs; lsp[nls].ys=e->ys;
      lsp[nls].angle=e->image_angle; lsp[nls].blend=e->blend; lsp[nls].alpha=e->alpha; lsp[nls].depth=l->depth; lsp[nls].order=l->order;
      nls++;
    }
  }
  /* unified depth-sorted draw list of instances + tiles + GMS2 layers + auto-draw particle systems */
  int npart=0; while(gml_part_system_auto_draw_nth(npart,NULL,NULL)) npart++;
  int cap=n+nt+nlb+nlt+nls+nlf+ncb+npart; if(!dl_grow((void**)&g_dl_it,&g_dl_it_cap,cap>0?cap:1,sizeof(GmlDrawItem))){ g_dl_tiles=tiles; g_dl_tdepth=tdepth; g_dl_tiles_cap=tcap; return; }
  GmlDrawItem *it=g_dl_it; int m=0;
  int *inst_ord=vm_draw_order_scratch(vm,n>0?n:1); int inst_n=0;
  if(inst_ord){
    for(int i=0;i<n;i++) if(vm->inst[i].active && !vm->inst[i].marked) inst_ord[inst_n++]=i;
    if(inst_n>1) sort_slots_by_creation(vm,inst_ord,inst_n);
  }
  for(int k=0;k<inst_n;k++){ int i=inst_ord[k]; if(instance_draw_layer_visible(vm,&vm->inst[i])){
    it[m].depth=vm->inst[i].depth; it[m].type=0; it[m].idx=i; it[m].seq=m; it[m].order=vm->inst[i].draw_layer_order;
    it[m].element_order=vm->inst[i].draw_layer_element_order;
    it[m].classic=vm->win&&vm->win->classic_version; it[m].obj=vm->inst[i].obj;
    it[m].placed=it[m].classic && vm->inst[i].room_placed; m++; } }
  for(int i=0;i<nt;i++){ it[m].depth=tdepth[i]; it[m].type=1; it[m].idx=i; it[m].seq=m; it[m].order=tiles[i].order; it[m].classic=0; it[m].obj=-1; m++; }
  for(int i=0;i<nlt;i++){ it[m].depth=ltl[i].depth; it[m].type=2; it[m].idx=i; it[m].seq=m; it[m].order=ltl[i].order; it[m].classic=0; it[m].obj=-1; m++; }
  for(int i=0;i<nlb;i++){ it[m].depth=lbg[i].depth; it[m].type=3; it[m].idx=i; it[m].seq=m; it[m].order=lbg[i].order; it[m].classic=0; it[m].obj=-1; m++; }
  for(int i=0;i<nls;i++){ it[m].depth=lsp[i].depth; it[m].type=5; it[m].idx=i; it[m].seq=m; it[m].order=lsp[i].order; it[m].classic=0; it[m].obj=-1; m++; }
  for(int i=0;i<nlf;i++){ it[m].depth=lfx[i].depth; it[m].type=7; it[m].idx=i; it[m].seq=m; it[m].order=lfx[i].order; it[m].classic=0; it[m].obj=-1; m++; }
  for(int i=0;i<ncb;i++){ it[m].depth=cbg[i].depth; it[m].type=6; it[m].idx=i; it[m].seq=m; it[m].order=-1; it[m].classic=0; it[m].obj=-1; m++; }
  for(int i=0;i<npart;i++){ int pid=0; double dep=0;
    if(gml_part_system_auto_draw_nth(i,&pid,&dep)){ it[m].depth=dep; it[m].type=4; it[m].idx=pid; it[m].seq=m; it[m].order=-1; it[m].classic=0; it[m].obj=-1; m++; } }
  qsort(it,m,sizeof(GmlDrawItem),cmp_draw_item);
  static int dumped=0;
  { const char *li=getenv("GML_LOG_INST");
    if(li && atoi(li)>0 && !dumped && g_vm_frame<atoi(li)) goto skip_instdump; }
  if(getenv("GML_LOG_INST") && !dumped){ dumped=1;
    fprintf(stderr,"[draw] room=%d, %d instances + %d tiles, draw_events_off=%d (back->front):\n",
            vm->room_index,n,nt,vm->draw_events_off);
    for(int k=0;k<m && k<2000;k++){ if(it[k].type==1){ GmlDrawTile *t=&tiles[it[k].idx];
        fprintf(stderr,"   TILE def=%d depth=%.0f @(%.0f,%.0f) %dx%d src=(%d,%d)",t->def,it[k].depth,t->x,t->y,t->w,t->h,t->sx,t->sy);
        if(t->def>=0 && t->def<R->n_bg){ int ti=R->bg[t->def].tpag;
          if(ti>=0 && ti<R->n_tpag){ GmlTpag *p=&R->tpag[ti];
            fprintf(stderr," tpag=%d atlas=%d packed=(%d,%d %dx%d) trim=(%d,%d) logical=%dx%d",ti,p->atlas,p->sx,p->sy,p->sw,p->sh,p->tx,p->ty,p->bw,p->bh); } }
        fputc('\n',stderr); }
      else if(it[k].type==2){ struct LayTile *t=&ltl[it[k].idx];
        fprintf(stderr,"   LTILE spr=%d depth=%.0f @(%.0f,%.0f) %dx%d\n",t->sprite,it[k].depth,t->x,t->y,t->w,t->h); }
      else if(it[k].type==3){ struct LayBg *b=&lbg[it[k].idx];
        fprintf(stderr,"   LBG spr=%d sub=%d depth=%.0f @(%.3f,%.3f) scale=%.3f/%.3f tiled=%d/%d stretch=%d colour=%06x alpha=%.3f\n",
          b->sprite,b->subimg,it[k].depth,b->x,b->y,b->xs,b->ys,b->th,b->tv,b->stretch,b->blend&0xFFFFFFu,b->alpha); }
      else if(it[k].type==4){
        fprintf(stderr,"   PARTICLES sys=%d depth=%.0f\n",it[k].idx,it[k].depth); }
      else if(it[k].type==5){ struct LaySprite *s=&lsp[it[k].idx];
        fprintf(stderr,"   LSPR spr=%d depth=%.0f @(%.0f,%.0f) idx=%d ang=%.0f xs=%.1f ys=%.1f a=%.2f\n",
          s->sprite,it[k].depth,s->x,s->y,s->subimg,s->angle,s->xs,s->ys,s->alpha); }
      else if(it[k].type==6){ struct ClassicBg *b=&cbg[it[k].idx];
        fprintf(stderr,"   CBG def=%d depth=%.0f @(%.0f,%.0f) tiled=%d/%d stretch=%d colour=%06x alpha=%.3f\n",
          b->def,it[k].depth,b->x,b->y,b->th,b->tv,b->stretch,b->blend&0xFFFFFFu,b->alpha); }
      else if(it[k].type==7){ struct LayEffect *f=&lfx[it[k].idx];
        fprintf(stderr,"   LEFFECT kind=%d depth=%.0f sampler-sprite=%d\n",
          f->effect.kind,it[k].depth,f->effect.sampler_sprite); }
      else { GmlInstance *in=&vm->inst[it[k].idx];
        fprintf(stderr,"   %-26s id=%u spr=%-4d vis=%.0f depth=%.0f ord=%d elem=%d @(%.0f,%.0f) ang=%.0f xs=%.1f ys=%.1f a=%.2f ii=%.4f is=%.3f\n",
          (in->obj>=0&&in->obj<vm->n_objects)?vm->objects[in->obj].name:"?",in->id,
          (int)in->sprite_index,in->visible,in->depth,in->draw_layer_order,in->draw_layer_element_order,
          in->x,in->y,in->image_angle,in->image_xscale,in->image_yscale,in->image_alpha,in->image_index,in->image_speed); } } }
  skip_instdump:
  int active_layer_order=-1;
  GmlRtLayer *active_layer=NULL;
  GmlLayerFilter *active_filter=NULL;
  int active_filter_started=0;
  double effect_time=g_vm_frame/gml_room_speed(vm);
  for(int k=0;k<m;k++){
    gml_d3_set_draw_depth(it[k].depth);
    if(it[k].order!=active_layer_order){
      if(active_layer) gml_run_layer_script(vm,active_layer->script_end);
      if(active_filter_started) gml_render_layer_filter_end(R,active_filter,effect_time);
      R->active_shader=-1;
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
        if(encoded!=0) R->active_shader=(int)encoded-1;
        gml_run_layer_script(vm,active_layer->script_begin);
      }
    }
    if(it[k].type==1){ GmlDrawTile *t=&tiles[it[k].idx];
      gml_draw_background_part_ext(R,t->def,t->sx,t->sy,t->w,t->h,t->x,t->y,t->xs,t->ys,0xFFFFFF,1); continue; }
    if(it[k].type==2){ struct LayTile *t=&ltl[it[k].idx];
      if(vm->win && vm->win->classic_version)
        gml_draw_background_part_ext(R,t->sprite,t->sx,t->sy,t->w,t->h,t->x,t->y,t->xs,t->ys,t->blend,t->alpha);
      else
        gml_draw_sprite_part_ext(R,t->sprite,0,t->sx,t->sy,t->w,t->h,t->x,t->y,t->xs,t->ys,t->blend,t->alpha);
      continue; }
    if(it[k].type==3){ struct LayBg *b=&lbg[it[k].idx];
      if(b->sprite<0) gml_draw_layer_color_fill(R,b->blend,b->alpha);
      else if(!vm->win || vm->win->bytecode<17){
        /* Compatibility layer records from older formats keep the legacy sprite-instance
         * origin and combined tiling behavior. The distinct-axis/top-left semantics below belong
         * to native GMS2 background layers. */
        if(b->th || b->tv) gml_draw_sprite_tiled_ext(R,b->sprite,0,b->x,b->y,1,1,b->blend,b->alpha);
        else gml_draw_sprite_ext(R,b->sprite,0,b->x,b->y,1,1,0,b->blend,b->alpha);
      }
      else if(b->stretch && b->sprite<R->n_spr && R->spr[b->sprite].w>0 && R->spr[b->sprite].h>0){
        double xs=(double)rm.width/R->spr[b->sprite].w;
        double ys=(double)rm.height/R->spr[b->sprite].h;
        gml_draw_sprite_ext(R,b->sprite,b->subimg,
          b->x+R->spr[b->sprite].originx*xs,b->y+R->spr[b->sprite].originy*ys,
          xs,ys,0,b->blend,b->alpha);
      } else gml_draw_layer_background_sprite(R,b->sprite,b->subimg,b->x,b->y,
                                                b->xs,b->ys,b->blend,b->alpha,b->th,b->tv);
      continue; }
    if(it[k].type==4){ gml_part_system_drawit(R,it[k].idx); continue; }
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
    { const char *sk=getenv("GML_SKIP");                  /* debug: skip drawing a named object */
      if(sk && in->obj>=0 && in->obj<vm->n_objects && vm->objects[in->obj].name
         && strstr(vm->objects[in->obj].name,sk)) continue; }
    if(in->visible<0.5) continue;   /* GM: an invisible instance runs neither
                                       its Draw event nor the automatic sprite draw. */
    /* Draw event replaces default draw; else draw sprite_index automatically. */
    if(event_lookup_from(vm,"Draw_0",in->obj,NULL,NULL)){
      draw_event_hook(vm,in,"Draw_0",1);
      int drew=gml_run_event(vm,in,"Draw_0");
      draw_event_hook(vm,in,"Draw_0",0);
      if(drew) continue;
    }
    if(in->sprite_index>=0)
      gml_draw_sprite_ext(R,(int)in->sprite_index,(int)in->image_index,in->x,in->y,
                          in->image_xscale,in->image_yscale,in->image_angle,
                          (uint32_t)in->image_blend,in->image_alpha);
  }
  if(active_layer) gml_run_layer_script(vm,active_layer->script_end);
  if(active_filter_started) gml_render_layer_filter_end(R,active_filter,effect_time);
  R->active_shader=-1;
  /* All draw scratch (it/lbg/ltl/lsp/tiles/tdepth) is persistent (g_dl_*) — write the possibly-grown
   * tile buffers back and keep everything allocated for next frame; nothing is freed here. */
  g_dl_tiles=tiles; g_dl_tdepth=tdepth; g_dl_tiles_cap=tcap;
}

/* Dispatch Draw_64 events in depth order. Set view_current to 7 when views
 * are enabled and to 0 otherwise for this pass. */
/* run one draw-stage event pass (Draw_72/73 begin-end, Draw_74/75 pre-post, Draw_65/66 GUI
 * begin-end) over all instances in depth order. Cheap no-op when no object has the event. */
void gml_vm_draw_pass(GmlVM *vm, const char *suffix){
  GmlRender *R=(GmlRender*)vm->render; if(!R) return;
  int n=vm->inst_count;
  int *ord=vm_draw_order_scratch(vm,n>0?n:1); if(!ord) return;
  int m=0;
  for(int i=0;i<n;i++){ GmlInstance *in=&vm->inst[i];
    if(!in->active||in->marked||in->visible<0.5||!instance_draw_layer_visible(vm,in)) continue;
    if(event_lookup_from(vm,suffix,in->obj,NULL,NULL)) ord[m++]=i;
  }
  if(m>1) sort_slots_by_creation(vm,ord,m);
  for(int a=0;a<m;a++) for(int b=a+1;b<m;b++)
    if(vm->inst[ord[b]].depth>vm->inst[ord[a]].depth){ int t=ord[a]; ord[a]=ord[b]; ord[b]=t; }
  for(int k=0;k<m;k++){
    GmlInstance *in=&vm->inst[ord[k]];
    if(getenv("GML_LOG_DRAW_PASS")){ extern long g_vm_frame;
      const char *on=(in->obj>=0&&in->obj<vm->n_objects&&vm->objects[in->obj].name)?vm->objects[in->obj].name:"?";
      int ci=-1; event_lookup_from(vm,suffix,in->obj,NULL,&ci);
      const char *cn=(ci>=0&&vm->win&&ci<vm->win->n_code)?vm->win->code[ci].name:"?";
      fprintf(stderr,"[drawpass] f%ld %s obj=%s id=%d depth=%.0f code=%d:%s\n",
              g_vm_frame,suffix,on,in->id,in->depth,ci,cn);
    }
    draw_event_hook(vm,in,suffix,1);
    gml_run_event(vm,in,suffix);
    draw_event_hook(vm,in,suffix,0);
  }
}
void gml_vm_draw_gui(GmlVM *vm){
  GmlRender *R=(GmlRender*)vm->render; if(!R) return;
  int n=vm->inst_count;
  int *ord=vm_draw_order_scratch(vm,n>0?n:1); if(!ord) return;
  int m=0;
  for(int i=0;i<n;i++){ GmlInstance *in=&vm->inst[i];
    if(!in->active||in->marked||in->visible<0.5||!instance_draw_layer_visible(vm,in)) continue;
    if(event_lookup_from(vm,"Draw_64",in->obj,NULL,NULL)) ord[m++]=i;
  }
  if(m>1) sort_slots_by_creation(vm,ord,m);
  for(int a=0;a<m;a++) for(int b=a+1;b<m;b++)            /* depth descending (back -> front) */
    if(vm->inst[ord[b]].depth>vm->inst[ord[a]].depth){ int t=ord[a]; ord[a]=ord[b]; ord[b]=t; }
  int views_on=0;
  for(int v=0;v<8;v++) if(get_global_arr_d(vm,"view_visible",v)>=0.5){ views_on=1; break; }
  *gml_varmap_put(&vm->globals,"view_current")=vreal(views_on?7:0);
  for(int k=0;k<m;k++){
    GmlInstance *in=&vm->inst[ord[k]];
    if(getenv("GML_LOG_DRAW_PASS")){ extern long g_vm_frame;
      const char *on=(in->obj>=0&&in->obj<vm->n_objects&&vm->objects[in->obj].name)?vm->objects[in->obj].name:"?";
      int ci=-1; event_lookup_from(vm,"Draw_64",in->obj,NULL,&ci);
      const char *cn=(ci>=0&&vm->win&&ci<vm->win->n_code)?vm->win->code[ci].name:"?";
      fprintf(stderr,"[drawpass] f%ld Draw_64 obj=%s id=%d depth=%.0f code=%d:%s\n",
              g_vm_frame,on,in->id,in->depth,ci,cn);
    }
    draw_event_hook(vm,in,"Draw_64",1);
    gml_run_event(vm,in,"Draw_64");
    draw_event_hook(vm,in,"Draw_64",0);
  }
  *gml_varmap_put(&vm->globals,"view_current")=vreal(0);
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
  double x0, x1, y0, y1;
  if(vm->win && vm->win->classic_version){
    x0=(s->ml-s->originx)*xs;
    y0=(s->mt-s->originy)*ys;
    x1=x0+(s->mr+1.0-s->ml)*xs-1.0;
    y1=y0+(s->mb+1.0-s->mt)*ys-1.0;
  } else {
    x0=s->ml; x1=s->mr+1.0; y0=s->mt; y1=s->mb+1.0;
  }
  double corners[4][2]={{x0,y0},{x1,y0},{x0,y1},{x1,y1}};
  for(int i=0;i<4;i++){
    double px, py;
    if(vm->win && vm->win->classic_version){ px=corners[i][0]; py=corners[i][1]; }
    else { px=(corners[i][0]-s->originx)*xs; py=(corners[i][1]-s->originy)*ys; }
    double wx=atx + px*c + py*sn;
    double wy=aty - px*sn + py*c;
    if(wx<minx) minx=wx;
    if(wx>maxx) maxx=wx;
    if(wy<miny) miny=wy;
    if(wy>maxy) maxy=wy;
  }
  if(vm->win && vm->win->classic_version){
    *l=classic_round_even(minx); *t=classic_round_even(miny);
    *r=classic_round_even(maxx); *b=classic_round_even(maxy);
  } else {
    *l=floor(minx); *t=floor(miny); *r=ceil(maxx)-1.0; *b=ceil(maxy)-1.0;
  }
  return 1;
}
static int vm_bbox(GmlVM *vm, GmlInstance *in, double *l, double *t, double *r, double *b){
  return vm_bbox_at(vm,in,in->x,in->y,l,t,r,b);
}
static int vm_overlap(double l1,double t1,double r1,double b1,double l2,double t2,double r2,double b2){
  return l1<=r2 && l2<=r1 && t1<=b2 && t2<=b1;
}
static int vm_mask_hit_world(GmlVM *vm, GmlRender *R, GmlInstance *in, GmlSprite *s,
                             int sprite, int wx, int wy){
  double xs=in->image_xscale, ys=in->image_yscale;
  if(fabs(xs)<1e-9 || fabs(ys)<1e-9) return 0;
  double ang=in->image_angle*M_PI/180.0, c=cos(ang), sn=sin(ang);
  double ox=in->x, oy=in->y;
  if(vm->win && vm->win->classic_version){
    ox=classic_round_even(ox); oy=classic_round_even(oy);
  }
  double rx=(double)wx-ox, ry=(double)wy-oy;
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
    if(!vm_mask_hit_world(vm,R,a,ap,as,wx,wy)) continue;
    if( vm_mask_hit_world(vm,R,b,bp,bs,wx,wy)) return 1;
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
  if(vm->win && vm->win->classic_version){
    for(int i=0;i<vm->n_key_events;i++) for(int j=i+1;j<vm->n_key_events;j++){
      int swap=vm->key_events[j].kind<vm->key_events[i].kind ||
        (vm->key_events[j].kind==vm->key_events[i].kind && vm->key_events[j].vk<vm->key_events[i].vk);
      if(swap){ typeof(vm->key_events[0]) t=vm->key_events[i]; vm->key_events[i]=vm->key_events[j]; vm->key_events[j]=t; }
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
  if(vm->win && vm->win->classic_version)
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
    /* Resolve collision subtype suffixes as numeric object indices or object names. */
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
static void colcand_reset(void){
  for(int i=0;i<48;i++){
    free(g_colcand[i].slots);
    memset(&g_colcand[i],0,sizeof(g_colcand[i]));
  }
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
  for(int r=0;r<nr;r++){
    int nd=0; const int *dl=object_descendants(vm,roots[r],&nd);
    for(int k=0;k<nd;k++){ int d=dl[k];
      for(int i=vm->obj_head[d]; i>=0; i=vm->inst_next[i])
        if(i<vm->inst_count) bits[i>>6]|=1ull<<(i&63);
    }
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
  uint64_t *classic_done=NULL;
  int classic_done_n=0, classic_done_cap=0;
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
      int solid_pair=si->solid || oi->solid;
      int classic_pair=solid_pair && vm->win && vm->win->classic_version;
      /* GM8 and GMS2 present solid contacts from their pre-movement coordinates. Studio 1
       * dispatches the event at the current coordinates instead; treating every
       * bytecode<17 package as a classic transaction changes the late state of otherwise stable
       * collision-heavy rooms.  Use the package family, not a content-specific exception. */
      int rollback_pair=classic_pair ||
        (solid_pair && vm->win && !vm->win->classic_version && vm->win->bytecode>=17);
      uint64_t pair_key=((uint64_t)(unsigned)(i<j?i:j)<<32)|(unsigned)(i<j?j:i);
      int pair_done=0;
      if(classic_pair)
        for(int p=0;p<classic_done_n;p++) if(classic_done[p]==pair_key){ pair_done=1; break; }
      if(pair_done) continue;
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
        /* Remember which participant entered the contact during this step. The
         * Collision code gets the first opportunity to resolve the overlap. */
        int oi_moved=oi->x!=oi->xprevious || oi->y!=oi->yprevious;
        int oi_kinematic=oi->hspeed!=0.0 || oi->vspeed!=0.0;
        /* Transactional solid contacts present both participants at their pre-movement
         * positions. Event code may then resolve the contact explicitly. */
        if(rollback_pair){
          si->x=si->xprevious; si->y=si->yprevious;
          oi->x=oi->xprevious; oi->y=oi->yprevious;
          si->path_position=si->path_positionprevious;
          oi->path_position=oi->path_positionprevious;
          gml_colgrid_touch(si); gml_colgrid_touch(oi);
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
        if(classic_pair){
          /* Classic collision dispatch treats the two directed events as one transaction. */
          if(si->active && !si->marked && oi->active && !oi->marked){
            int reverse_handler=-1, reverse_target=-1, reverse_code=-1;
            if(col_event_for_pair(vm,oi->obj,si->obj,&reverse_handler,&reverse_target,&reverse_code)){
              char reverse_suffix[32]; snprintf(reverse_suffix,sizeof reverse_suffix,"Collision_%d",reverse_target);
              run_event_code_from(vm,oi,si,reverse_suffix,reverse_handler,reverse_code);
            }
          }
          if(si->active && !si->marked){ si->x+=si->hspeed; si->y+=si->vspeed; gml_colgrid_touch(si); }
          if(oi->active && !oi->marked){ oi->x+=oi->hspeed; oi->y+=oi->vspeed; gml_colgrid_touch(oi); }
          if(si->active && !si->marked && oi->active && !oi->marked){
            double cl1,ct1,cr1,cb1,cl2,ct2,cr2,cb2;
            int still_hit=vm_bbox(vm,si,&cl1,&ct1,&cr1,&cb1) &&
                          vm_bbox(vm,oi,&cl2,&ct2,&cr2,&cb2) &&
                          vm_overlap(cl1,ct1,cr1,cb1,cl2,ct2,cr2,cb2) &&
                          vm_masks_overlap(vm,si,oi,cl1,ct1,cr1,cb1,cl2,ct2,cr2,cb2);
            if(still_hit){
              si->x=si->xprevious; si->y=si->yprevious; si->path_position=si->path_positionprevious;
              oi->x=oi->xprevious; oi->y=oi->yprevious; oi->path_position=oi->path_positionprevious;
              gml_colgrid_touch(si); gml_colgrid_touch(oi);
            }
          }
          if(classic_done_n>=classic_done_cap){
            int nc=classic_done_cap?classic_done_cap*2:16;
            uint64_t *np=realloc(classic_done,(size_t)nc*sizeof(*np));
            if(np){ classic_done=np; classic_done_cap=nc; }
          }
          if(classic_done_n<classic_done_cap) classic_done[classic_done_n++]=pair_key;
        }
        /* If Collision code left the pair intersecting, apply the solid fallback
         * to the participant that entered the contact. Classic actions that stop
         * an incoming motion restore the pre-contact coordinate too; motion that
         * remains active stays under the event's explicit contact resolution. */
        int oi_stopped=oi->hspeed==0.0 && oi->vspeed==0.0;
        if(!rollback_pair && si->active && !si->marked && oi->active && !oi->marked &&
           si->solid && oi_moved && (!oi_kinematic ||
             (vm->win && vm->win->classic_version && oi_stopped))){
          double pl1,pt1,pr1,pb1,pl2,pt2,pr2,pb2;
          int post_hit=vm_bbox(vm,si,&pl1,&pt1,&pr1,&pb1) &&
                       vm_bbox(vm,oi,&pl2,&pt2,&pr2,&pb2) &&
                       vm_overlap(pl1,pt1,pr1,pb1,pl2,pt2,pr2,pb2) &&
                       vm_masks_overlap(vm,si,oi,pl1,pt1,pr1,pb1,pl2,pt2,pr2,pb2);
          if(post_hit){
            oi->x=oi->xprevious; oi->y=oi->yprevious; gml_colgrid_touch(oi);
          }
        }
        if(!si->active||si->marked) break;   /* self destroyed by the event */
        if(!vm_bbox(vm,si,&l1,&t1,&r1,&b1)) break;
        /* the event may have mutated the world (and the shared candidate cache): finish this
         * si with the plain linear scan from the next slot — identical event sequence */
        if(jlin<0 && fcn>=0){ jlin=j; fcn=-1; }
      }
    }
  }
  free(classic_done);
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

int gml_object_set_parent(GmlVM *vm, int object, int parent){
  if(!vm || object<0 || object>=vm->n_objects) return 0;
  if(parent<0 || parent>=vm->n_objects) parent=-1;
  if(parent==object) return 0;
  for(int p=parent, guard=0; p>=0 && p<vm->n_objects && guard++<vm->n_objects;
      p=vm->objects[p].parent)
    if(p==object) return 0;
  if(vm->objects[object].parent==parent) return 1;
  vm->objects[object].parent=parent;

  classic_dispatch_cache_reset();
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
  colcand_reset();
  gml_colgrid_invalidate(vm);
  return 1;
}
/* fire the room/view boundary "Other" events for instances whose bbox left the room/view. GM:
 * "Outside" = bbox entirely outside; "Intersect Boundary" = bbox not entirely inside (partly OR fully out). */
static void classic_boundary_box(GmlVM *vm, GmlInstance *in,
                                 double *l, double *t, double *r, double *b){
  if(vm_bbox(vm,in,l,t,r,b)) return;
  /* Sprite-less classic instances use their point, with the same directed rounding as the
   * historical rectangle test. Boundary events are commonly used by invisible controllers. */
  *l=floor(in->x); *t=floor(in->y); *r=ceil(in->x); *b=ceil(in->y);
}
static void run_boundary_events(GmlVM *vm){
  if(!vm->render) return;
  GmlRoom rm; int hr=(gml_room_get(vm->win,vm->room_index,&rm)==0);
  double vx=get_global_arr_d(vm,"view_xview",0), vy=get_global_arr_d(vm,"view_yview",0);
  double vw=get_global_arr_d(vm,"view_wview",0), vh=get_global_arr_d(vm,"view_hview",0);
  int has_view=(vw>0 && vh>0);
  /* Classic dispatch completes one boundary subtype at a time, grouped by exact object
   * resource inside that subtype. Keep Studio's instance-major dispatch below unchanged. */
  if(vm->win && vm->win->classic_version){
    int view_count=0;
    if(hr && rm.view_ptr){ view_count=(int)u32(vm->win->data,rm.view_ptr); if(view_count>8)view_count=8; }
    for(int phase=0;phase<18;phase++){
      int bit=1<<phase, sub=phase<2?phase:(phase<10?38+phase:40+phase);
      int view=phase<2?-1:(phase<10?phase-2:phase-10);
      if((view<0 && !hr) || (view>=0 && view>=view_count)) continue;
      double x1=0,y1=0,x2=rm.width,y2=rm.height;
      if(view>=0){
        x1=get_global_arr_d(vm,"view_xview",view); y1=get_global_arr_d(vm,"view_yview",view);
        x2=x1+get_global_arr_d(vm,"view_wview",view);
        y2=y1+get_global_arr_d(vm,"view_hview",view);
      }
      char suffix[16]; snprintf(suffix,sizeof suffix,"Other_%d",sub);
      for(int object=0;object<vm->n_objects;object++){
        if(!(vm->objects[object].bevents&bit)) continue;
        int count=classic_collect_object_slots(vm,object); if(count<0) continue;
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
    double l,t,r,b; if(!vm_bbox(vm,in,&l,&t,&r,&b)) continue;
    if((bits&1) && hr && (r<0||l>rm.width||b<0||t>rm.height)) gml_run_event(vm,in,"Other_0");
    if(!in->active||in->marked) continue;
    if((bits&2) && hr && !(l>=0&&r<=rm.width&&t>=0&&b<=rm.height)
       && (r>=0&&l<=rm.width&&b>=0&&t<=rm.height)) gml_run_event(vm,in,"Other_1");
    if(!in->active||in->marked) continue;
    if((bits&4) && has_view && (r<vx||l>vx+vw||b<vy||t>vy+vh)) gml_run_event(vm,in,"Other_40");
    if(!in->active||in->marked) continue;
    if((bits&(1<<10)) && has_view && !(l>=vx&&r<=vx+vw&&t>=vy&&b<=vy+vh)) gml_run_event(vm,in,"Other_50");
  }
}

/* WELL512 follows the Lomont recurrence credited in LICENSES/WELL512.txt.
 * Expand seeds using MSVC-LCG arithmetic; random(x) scales the next value by x/2^32. */
void gml_rng_seed(GmlVM *vm, uint32_t seed){
  uint32_t s=seed;
  for(int i=0;i<16;i++){ s=((s*214013u+2531011u)>>16)&0x7fffffffu; vm->rng_well[i]=s; }
  vm->rng_index=0;
  vm->rng_classic_state=seed;
}
static uint32_t gml_rng_next(GmlVM *vm){
  static int log_calls=-1;
  if(log_calls<0) log_calls=getenv("GML_LOG_RNG_CALL")?1:0;
  if(vm->win && vm->win->classic_version){
    vm->rng_classic_state=vm->rng_classic_state*0x08088405u+1u;
    if(log_calls){
      const char *object=(vm->cur_self && vm->cur_self->obj>=0 && vm->cur_self->obj<vm->n_objects)
        ? vm->objects[vm->cur_self->obj].name : "?";
      fprintf(stderr,"[rng-call] f%ld seed=%u id=%u object=%s event=%s\n",g_vm_frame,
        vm->rng_classic_state,vm->cur_self?vm->cur_self->id:0,object?object:"?",
        vm->cur_event?vm->cur_event:"?");
    }
    return vm->rng_classic_state;
  }
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
  if(log_calls){
    const char *object=(vm->cur_self && vm->cur_self->obj>=0 && vm->cur_self->obj<vm->n_objects)
      ? vm->objects[vm->cur_self->obj].name : "?";
    fprintf(stderr,"[rng-call] f%ld value=%u id=%u object=%s event=%s\n",g_vm_frame,a,
      vm->cur_self?vm->cur_self->id:0,object?object:"?",vm->cur_event?vm->cur_event:"?");
  }
  return a;
}
double gml_rng_value(GmlVM *vm){ return (double)gml_rng_next(vm) / 4294967296.0; }  /* [0,1) */

/* The input API stores keyboard remapping as physical/source VK -> logical/destination VK.
 * The frontend exposes physical state by VK, so aggregate every source that currently maps
 * to the requested logical key. Aggregating current/previous state before testing an edge is
 * important when several physical keys map to the same logical key. */
static void keyboard_source_state(int source, int *cur, int *prev){
  int c=gml_input_key(source,0);
  int pressed=gml_input_key(source,1);
  int released=gml_input_key(source,2);
  int p=released ? 1 : (pressed ? 0 : c);
  if(cur) *cur=c;
  if(prev) *prev=p;
}
void gml_keyboard_unset_map(GmlVM *vm){
  if(!vm) return;
  for(int i=0;i<256;i++) vm->key_map[i]=(int16_t)i;
}
void gml_keyboard_set_map(GmlVM *vm, int source, int destination){
  if(!vm || source<0 || source>255 || destination < -1 || destination>255) return;
  vm->key_map[source]=(int16_t)destination;
  if(getenv("GML_LOG_KEYMAP")){
    extern long g_vm_frame;
    fprintf(stderr,"[keymap] f%ld source=%d destination=%d\n",g_vm_frame,source,destination);
  }
}
int gml_keyboard_get_map(GmlVM *vm, int source){
  if(!vm || source<0 || source>255) return source;
  return vm->key_map[source];
}
int gml_keyboard_check(GmlVM *vm, int vk, int edge){
  if(!vm) return gml_input_key(vk,edge);
  if(edge<0 || edge>2) edge=0;
  int any_cur=0, any_prev=0;
  if(vk==0 || vk==1){
    for(int source=2;source<256;source++){
      if(vm->key_map[source]<2) continue; /* disabled and sentinel destinations are not keys */
      int cur=0,prev=0;
      keyboard_source_state(source,&cur,&prev);
      any_cur|=cur; any_prev|=prev;
    }
    if(vk==0){ any_cur=!any_cur; any_prev=!any_prev; }
  } else {
    if(vk<0 || vk>255) return 0;
    for(int source=2;source<256;source++) if(vm->key_map[source]==vk){
      int cur=0,prev=0;
      keyboard_source_state(source,&cur,&prev);
      any_cur|=cur; any_prev|=prev;
    }
  }
  return edge==1 ? (any_cur && !any_prev) : edge==2 ? (!any_cur && any_prev) : any_cur;
}
void gml_keyboard_clear(GmlVM *vm, int logical_vk){
  if(!vm){ gml_input_key_clear(logical_vk); return; }
  if(logical_vk==0) return;
  for(int source=2;source<256;source++)
    if((logical_vk==1 && vm->key_map[source]>=2) || vm->key_map[source]==logical_vk)
      gml_input_key_clear(source);
}

int gml_vm_init(GmlVM *vm, GmlWin *win){
  classic_dispatch_cache_reset();
  memset(vm,0,sizeof(*vm));
  gml_keyboard_unset_map(vm);
  { extern void gml_d3_reset(void); gml_d3_reset(); }
  vm->cg_built_frame=-1;   /* memset leaves 0, which would collide with g_vm_frame==0 at boot */
  { extern void gml_part_reset_all(void); gml_part_reset_all(); }   /* fresh particle pools per game */
  colcand_reset();          /* cache slots belong to the previous VM across a cold Restart */
  vm->win=win; vm->pending_room=-1; vm->room_index=-1; vm->next_id=100000; vm->rng_state=0;
  vm->cur_code_index=-1;
  vm->code_static_count=(win && win->n_code>0)?win->n_code:0;
  vm->code_static=vm->code_static_count?calloc((size_t)vm->code_static_count,sizeof(*vm->code_static)):NULL;
  vm->code_static_init=vm->code_static_count?calloc((size_t)vm->code_static_count,1):NULL;
  if(vm->code_static_count && (!vm->code_static || !vm->code_static_init)){
    free(vm->code_static); free(vm->code_static_init);
    vm->code_static=NULL; vm->code_static_init=NULL; vm->code_static_count=0;
    return 1;
  }
  snprintf(vm->os_language,sizeof vm->os_language,"en");
  snprintf(vm->os_region,sizeof vm->os_region,"us");
  snprintf(vm->language_tag,sizeof vm->language_tag,"en-US");
  vm->rng_classic_state=0;
  vm->math_epsilon=(win && win->classic_version)?1e-13:1e-5;
  vm->potential_max_rotation=30; vm->potential_rotate_step=10;
  vm->potential_check_distance=3; vm->potential_rotate_on_spot=1;
  vm->listener_forward_z=-1; vm->listener_up_y=1;
  /* GM6-8 exposes these as writable built-in variables. Keeping the defaults in the ordinary
   * global map lets compiled source read/write them without a presentation-specific lookup. */
  if(win && win->classic_version){
    gml_set_global_scalar(vm,"transition_kind",0);
    gml_set_global_scalar(vm,"transition_steps",80);
  }
  vm->room_state_count=gml_room_count(win);
  vm->room_stored=calloc((size_t)(vm->room_state_count>0?vm->room_state_count:1),1);
  /* Seed dynamic IDs above room-placed IDs across the project to avoid collisions
   * between persistent dynamic instances and instances placed in later rooms. */
  { const GmlChunk *rc=gml_chunk(win,"ROOM");
    if(rc){ const uint8_t *d=win->data; uint32_t nr=u32(d,rc->off);
      for(uint32_t ri=0;ri<nr;ri++){
        uint32_t rp=u32(d,rc->off+4+ri*4); if(!rp) continue;
        GmlRoom r; if(gml_room_get(win,(int)ri,&r)!=0 || !r.obj_ptr) continue;
        uint32_t cnt=u32(d,r.obj_ptr);
        for(uint32_t i=0;i<cnt;i++){
          uint32_t ip=u32(d,r.obj_ptr+4+i*4); if(!ip) continue;
          uint32_t rid=u32(d,ip+12);
          if(rid>=vm->next_id && rid<0x40000000u) vm->next_id=rid+1;
        }
      }
    } }
  vm->next_buffer_id=1;
  vm->next_ds_id=1;
  vm->ds_list_compat_repair=0;
  vm->next_time_source_id=GML_TIME_SOURCE_ID_BASE;
  vm->time_source_game_state=1;
  { /* Optional initial seed for deterministic cross-run fidelity captures. Games that call
     * randomize() still use GML_RANDOMIZE_SEED at that call site; without this variable the
     * runtime uses the normal default seed. */
    const char *fixed=getenv("GML_RNG_SEED");
    uint32_t seed=fixed ? (uint32_t)strtoll(fixed,NULL,10) : 0u;
    gml_rng_seed(vm,seed);
  }
  gml_part_bind_vm(vm);
  parse_objects(vm);
  parse_paths(vm);
  parse_timelines(vm);
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
  /* Classic action libraries can carry creation code executed once before the
   * first room. The compiler emits one reserved CODE
   * entry only when such code exists; ordinary packages take this fast miss. */
  {
    int ci=gml_code_index_by_name(win,"gml_GlobalScript___gmlc_classic_startup");
    if(ci>=0){
      GmlInstance scratch;
      memset(&scratch,0,sizeof(scratch));
      scratch.active=1; scratch.obj=-1; scratch.id=0;
      scratch.image_xscale=scratch.image_yscale=1; scratch.image_alpha=1;
      scratch.sprite_index=-1; scratch.mask_index=-1; scratch.path_index=-1;
      scratch.timeline_index=-1; scratch.timeline_speed=1;
      for(int a=0;a<GML_ALARMS;a++) scratch.alarm[a]=-1;
      GmlVal result=gml_vm_run_code(vm,ci,&scratch,NULL,NULL,0);
      if(result.t==V_STR && result.d!=0) free((char*)result.s);
      varmap_free_ex(&scratch.vars,0);
    }
  }
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
  vm->n_async_http=0;
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
    m->last_lookup=-1;
  }
  vm->ds_map_last_slot=-1;
  for(int i=0;i<GML_DS_LIST_MAX;i++){
    free(vm->ds_list[i].item);
    free(vm->ds_list[i].child_kind);
    memset(&vm->ds_list[i],0,sizeof(vm->ds_list[i]));
  }
  for(int i=0;i<GML_DS_GRID_MAX;i++){ free(vm->ds_grid[i].cell); memset(&vm->ds_grid[i],0,sizeof(vm->ds_grid[i])); }
  vm->next_ds_id=1;
  vm->ds_list_compat_repair=0;
}
void gml_vm_free(GmlVM *vm){
  classic_dispatch_cache_reset();
  gml_colgrid_invalidate(vm);
  if(vm->obj_desc){ for(int i=0;i<vm->n_objects;i++) free(vm->obj_desc[i]); }
  free(vm->obj_desc); vm->obj_desc=NULL; free(vm->obj_desc_n); vm->obj_desc_n=NULL;
  free(vm->obj_alive); vm->obj_alive=NULL;
  free(vm->obj_head); vm->obj_head=NULL; free(vm->inst_next); vm->inst_next=NULL; free(vm->inst_prev); vm->inst_prev=NULL;
  free(vm->event_ord); vm->event_ord=NULL; vm->event_ord_cap=0;
  free(vm->step_free); vm->step_free=NULL;
  vm->step_free_n=vm->step_free_pos=vm->step_free_cap=0;
  free(vm->cg_off); vm->cg_off=NULL; free(vm->cg_items); vm->cg_items=NULL; vm->cg_items_cap=0;
  free(vm->cg_overlay); vm->cg_overlay=NULL; vm->cg_overlay_cap=vm->cg_overlay_n=0;
  free(vm->draw_ord); vm->draw_ord=NULL; vm->draw_ord_cap=0;
  freeset_begin();
  varmap_free(&vm->globals);
  for(int i=0;i<vm->code_static_count;i++) varmap_free(&vm->code_static[i]);
  for(int i=0;i<vm->inst_count;i++) varmap_free(&vm->inst[i].vars);
  for(int i=0;i<vm->n_structs;i++) if(vm->structs[i]) varmap_free(&vm->structs[i]->vars);
  for(int i=0;i<GML_TIME_SOURCE_MAX;i++) if(vm->time_source[i].live){
    val_free(vm->time_source[i].callback); val_free(vm->time_source[i].args);
  }
  freeset_end();
  memset(vm->time_source,0,sizeof(vm->time_source));
  free(vm->code_static); free(vm->code_static_init);
  vm->code_static=NULL; vm->code_static_init=NULL; vm->code_static_count=0;
  for(int i=0;i<vm->n_structs;i++) free(vm->structs[i]);
  free(vm->structs); free(vm->struct_gen); free(vm->struct_free);
  ini_free(vm);
  io_free(vm);
  ds_maps_free(vm);
  free(vm->inst);
  for(int i=0;i<vm->n_objects;i++) free(vm->objects[i].events);
  for(int i=0;i<vm->n_paths;i++) free(vm->paths[i].pts);
  free(vm->paths);
  for(int i=0;i<vm->n_timelines;i++){
    free(vm->timelines[i].moments);
    free(vm->timelines[i].owned_name);
  }
  free(vm->timelines);
  free(vm->objects); free(vm->col_events); free(vm->col_pair_cache); free(vm->event_cache);
  gml_tilemaps_clear(vm);
  free(vm->rtl); free(vm->rte); free(vm->view_ovr); free(vm->tilemaps);
  free(vm->room_stored); vm->room_stored=NULL; vm->room_state_count=0;
  free(vm->audio_room_warm_scan); vm->audio_room_warm_scan=NULL; vm->audio_room_warm_scan_n=0;
  gml_d3_reset();
}

/* ---------------- save-state runtime serialization ---------------- */
typedef struct { uint8_t *data; size_t cap, pos; int ok; GmlVM *vm; int compact_strings, array_meta; } StateW;
typedef struct { const uint8_t *data; size_t cap, pos; int ok; GmlVM *vm; int compact_strings, array_meta, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34, v35, v36, v37; } StateR;

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
static const char *state_intern_ex(GmlVM *vm, char *owned, int *remains_owned){
  if(remains_owned) *remains_owned=0;
  if(!owned) return "";
  const char *hit=gml_win_intern_lookup(vm->win,owned);   /* Look up an existing interned STRG string. */
  if(hit){ free(owned); return hit; }
  if(remains_owned) *remains_owned=1;
  return owned; /* the caller retains it as an owned map key or runtime string */
}
static const char *state_intern(GmlVM *vm, char *owned){
  return state_intern_ex(vm,owned,NULL);
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
    uint32_t max_len = sparse ? 16000000u : 1000000u;
    if(len>max_len || (!sparse && len>remain/4)){
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
    if(!A->is_2d){
      for(uint32_t i=0;i<len;i++) if(A->data[i].t==V_ARR && A->data[i].arr){
        A->nested_2d=1; break;
      }
    }
    GmlVal v=vreal(0); v.t=V_ARR; v.arr=A; return v;
  }
  state_debug("bad value type",s->pos,t);
  s->ok=0; return vreal(0);
}
static void sw_varmap(StateW *s, GmlVarMap *m){
  static int dbg=-1; if(dbg<0) dbg=getenv("GML_DBG_STATEVAR")!=NULL;
  uint32_t n=0;
  for(int i=0;i<m->cap;i++) if(m->slots[i].key) n++;
  if(n!=(uint32_t)m->len) state_debug("varmap len mismatch",s->pos,(uint32_t)m->len);
  sw_u32(s,n);
  for(int i=0;i<m->cap;i++) if(m->slots[i].key){
    if(dbg) fprintf(stderr,"[svar] %s t=%d\n",m->slots[i].key,m->slots[i].val.t);
    sw_str(s,m->slots[i].key);
    sw_val(s,m->slots[i].val,0);
  }
}
static int sr_varmap(GmlVM *vm, StateR *s, GmlVarMap *m){
  static int dbg=-1; if(dbg<0) dbg=getenv("GML_DBG_STATEVAR_LOAD")!=NULL;
  uint32_t n=sr_u32(s);
  if(n>100000){ state_debug("varmap too large",s->pos,n); s->ok=0; return 0; }
  for(uint32_t i=0;i<n && s->ok;i++){
    size_t key_pos=s->pos;
    int key_owned=0;
    char *k=sr_str_dup(s); const char *key=state_intern_ex(vm,k,&key_owned);
    if(dbg) fprintf(stderr,"[lvar] pos=%llu key=%s\n",(unsigned long long)key_pos,key?key:"");
    GmlVal v=sr_val(vm,s,0);
    if(!s->ok){ if(key_owned) free((char*)key); break; }
    gml_arr_mark_escaped(v);
    if(key_owned) *gml_varmap_put_owned_h(m,(char*)key,strhash(key))=v;
    else *gml_varmap_put(m,key)=v;
  }
  return s->ok;
}
/* GMV6 per-instance layout (LOSSLESS compaction; a state is written every frame under rewind):
 *  flags byte (active|marked<<1|deactivated<<2), id u32, obj i32,
 *  field mask u32 (bit set = value differs from its default and follows as a double, in bit
 *  order), x/y/xprev/yprev/xstart/ystart always as doubles, alarm mask u16 (bit = alarm != -1,
 *  set ones follow), path block (11 doubles) only when mask bit 20 is set, timeline block
 *  (5 doubles) only when mask bit 21 is set, then the varmap.
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
static int gmv6_timeline_present(GmlInstance *in){
  return in->timeline_index!=-1 || in->timeline_position!=0 || in->timeline_speed!=1 ||
         in->timeline_running!=0 || in->timeline_loop!=0;
}
static void sw_instance(StateW *s, GmlInstance *in){
  uint8_t fl=(in->active?1:0)|(in->marked?2:0)|(in->deactivated?4:0)|
             (in->room_dormant?8:0)|(in->room_was_deactivated?16:0)|(in->room_placed?32:0);
  sw_raw(s,&fl,1);
  sw_u32(s,in->id); sw_i32(s,in->obj); sw_i32(s,in->room_owner);
  sw_i64(s,(int64_t)in->creation_seq);
  uint32_t fm=0;
  #define FCHK(bit,field) if(in->field!=gmv6_def[bit]) fm|=1u<<(bit);
  GMV6_FIELDS(FCHK)
  #undef FCHK
  if(gmv6_path_present(in)) fm|=1u<<20;
  if(gmv6_timeline_present(in)) fm|=1u<<21;
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
  if(fm&(1u<<21)){
    sw_d(s,in->timeline_index); sw_d(s,in->timeline_position); sw_d(s,in->timeline_speed);
    sw_d(s,in->timeline_running); sw_d(s,in->timeline_loop);
  }
  sw_i32(s,in->draw_layer_order);
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
    in->room_dormant=(fl>>3)&1; in->room_was_deactivated=(fl>>4)&1;
    in->room_placed=s->v26?((fl>>5)&1):0;
    in->id=sr_u32(s); in->obj=sr_i32(s);
    in->room_owner=s->v19?sr_i32(s):vm->room_index;
    if(s->v33) in->creation_seq=(uint64_t)sr_i64(s);
    else if(in->active||in->deactivated||in->room_dormant) in->creation_seq=vm->next_creation_seq++;
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
    if(fm&(1u<<21)){
      in->timeline_index=sr_d(s); in->timeline_position=sr_d(s); in->timeline_speed=sr_d(s);
      in->timeline_running=sr_d(s); in->timeline_loop=sr_d(s);
    } else {
      in->timeline_index=-1; in->timeline_position=0; in->timeline_speed=1;
      in->timeline_running=0; in->timeline_loop=0;
    }
    in->draw_layer_order=s->v30?sr_i32(s):-1;
    in->draw_layer_element_order=-1; /* rebuilt from the current ROOM layer records after load */
    sr_varmap(vm,s,&in->vars);
    return;
  }
  in->active=sr_i32(s); in->marked=sr_i32(s); in->deactivated=sr_i32(s);
  in->id=sr_u32(s); in->obj=sr_i32(s);
  if(in->active||in->deactivated) in->creation_seq=vm->next_creation_seq++;
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
  in->timeline_index=-1; in->timeline_position=0; in->timeline_speed=1;
  in->timeline_running=0; in->timeline_loop=0;
  in->draw_layer_order=-1;
  in->draw_layer_element_order=-1;
  sr_varmap(vm,s,&in->vars);
}

static int state_instance_is_room_placed(GmlVM *vm, const GmlInstance *in){
  if(!vm || !vm->win || !in || in->room_owner<0) return 0;
  GmlRoom room;
  if(gml_room_get(vm->win,in->room_owner,&room)!=0 || !room.obj_ptr ||
     room.obj_ptr>vm->win->size || vm->win->size-room.obj_ptr<4) return 0;
  const uint8_t *data=vm->win->data;
  uint32_t count=u32(data,room.obj_ptr);
  if(count>100000 || (uint64_t)room.obj_ptr+4u+(uint64_t)count*4u>vm->win->size) return 0;
  for(uint32_t i=0;i<count;i++){
    uint32_t ptr=u32(data,room.obj_ptr+4+i*4);
    if(ptr<=vm->win->size && vm->win->size-ptr>=16 && u32(data,ptr+12)==in->id) return 1;
  }
  return 0;
}

static void runtime_clear(GmlVM *vm){
  freeset_begin();
  varmap_free(&vm->globals);
  for(int i=0;i<vm->code_static_count;i++) varmap_free(&vm->code_static[i]);
  for(int i=0;i<vm->inst_count;i++) varmap_free(&vm->inst[i].vars);
  for(int i=0;i<vm->n_structs;i++) if(vm->structs[i]) varmap_free(&vm->structs[i]->vars);
  for(int i=0;i<GML_TIME_SOURCE_MAX;i++) if(vm->time_source[i].live){
    val_free(vm->time_source[i].callback); val_free(vm->time_source[i].args);
  }
  freeset_end();
  memset(vm->time_source,0,sizeof(vm->time_source));
  vm->next_time_source_id=GML_TIME_SOURCE_ID_BASE;
  vm->time_source_game_state=1;
  if(vm->code_static_init && vm->code_static_count>0)
    memset(vm->code_static_init,0,(size_t)vm->code_static_count);
  for(int i=0;i<vm->n_structs;i++){ free(vm->structs[i]); vm->structs[i]=NULL; }
  if(vm->struct_gen && vm->cap_structs>0) memset(vm->struct_gen,0,(size_t)vm->cap_structs);
  vm->n_structs=0; vm->n_struct_free=0; vm->structs_last_gc_frame=0;
  if(vm->inst && vm->inst_cap>0) memset(vm->inst,0,(size_t)vm->inst_cap*sizeof(GmlInstance));
  ini_free(vm);
  io_free(vm);
  ds_maps_free(vm);
  vm->inst_count=0; vm->cur_self=vm->cur_other=NULL; vm->cur_event=NULL; vm->cur_event_obj=0;
  vm->next_creation_seq=1;
  vm->event_type=0; vm->event_number=0;
  vm->step_alloc_base=0; vm->action_relative=0;
  vm->potential_max_rotation=30; vm->potential_rotate_step=10;
  vm->potential_check_distance=3; vm->potential_rotate_on_spot=1;
  vm->window_x=0; vm->window_y=0; vm->window_cursor=0;
  memset(vm->emitter_live,0,sizeof(vm->emitter_live));
  memset(vm->emitter_gain,0,sizeof(vm->emitter_gain));
  memset(vm->emitter_x,0,sizeof(vm->emitter_x)); memset(vm->emitter_y,0,sizeof(vm->emitter_y));
  memset(vm->emitter_z,0,sizeof(vm->emitter_z)); memset(vm->emitter_ref,0,sizeof(vm->emitter_ref));
  memset(vm->emitter_max,0,sizeof(vm->emitter_max)); memset(vm->emitter_factor,0,sizeof(vm->emitter_factor));
  vm->listener_x=vm->listener_y=vm->listener_z=0;
  vm->listener_forward_x=vm->listener_forward_y=0; vm->listener_forward_z=-1;
  vm->listener_up_x=vm->listener_up_z=0; vm->listener_up_y=1; vm->audio_falloff_model=0;
  gml_keyboard_unset_map(vm);
  memset(vm->phys_fixture,0,sizeof(vm->phys_fixture));
  memset(vm->phys_joint,0,sizeof(vm->phys_joint));
  vm->phys_next_id=0; vm->phys_gravity_x=0; vm->phys_gravity_y=0;
  vm->phys_update_speed=0; vm->phys_update_iterations=0; vm->phys_paused=0; vm->phys_debug_draw=0;
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
  sw_u32(s,0x55564D47u); /* GMV37: GMV36 plus runtime room-view resource overrides */
  sw_i32(s,vm->inst_count); sw_u32(s,vm->next_id);
  sw_i64(s,(int64_t)vm->next_creation_seq);
  sw_i32(s,vm->room_index); sw_i32(s,vm->pending_room); sw_i32(s,vm->game_end);
  sw_i32(s,vm->started); sw_d(s,vm->last_key); sw_d(s,vm->window_fullscreen);
  { extern long g_vm_frame;
    long age=g_vm_frame - vm->room_enter_frame;
    if(age<0) age=0;
    sw_i64(s,(int64_t)age);
  }
  sw_i32(s,vm->action_relative);
  sw_d(s,vm->math_epsilon);
  sw_d(s,vm->potential_max_rotation); sw_d(s,vm->potential_rotate_step);
  sw_d(s,vm->potential_check_distance); sw_i32(s,vm->potential_rotate_on_spot);
  sw_i32(s,vm->classic_info_active);
  sw_i32(s,vm->script_argc); for(int i=0;i<16;i++) sw_val(s,vm->script_args[i],0);
  for(int i=0;i<16;i++) sw_u32(s,vm->rng_well[i]);
  sw_i32(s,vm->rng_index); sw_u32(s,vm->rng_state);
  sw_u32(s,vm->rng_classic_state);
  { int flags[GML_D3_STATE_FLAG_COUNT]; double values[GML_D3_STATE_VALUE_COUNT];
    uint32_t colors[GML_D3_STATE_COLOR_COUNT];
    gml_d3_state_get(flags,values,colors);
    for(int i=0;i<GML_D3_STATE_FLAG_COUNT;i++) sw_i32(s,flags[i]);
    for(int i=0;i<GML_D3_STATE_VALUE_COUNT;i++) sw_d(s,values[i]);
    for(int i=0;i<GML_D3_STATE_COLOR_COUNT;i++) sw_u32(s,colors[i]);
  }
  {
    size_t model_size=gml_d3_models_state_size();
    if(model_size>UINT32_MAX){ s->ok=0; model_size=0; }
    sw_u32(s,(uint32_t)model_size);
    if(s->data){
      if(s->pos<=s->cap && model_size<=s->cap-s->pos){
        if(!gml_d3_models_state_save(s->data+s->pos,model_size)) s->ok=0;
      } else s->ok=0;
    }
    s->pos+=model_size;
  }
  sw_i32(s,vm->room_state_count);
  if(vm->room_state_count>0) sw_raw(s,vm->room_stored,(size_t)vm->room_state_count);
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
      sw_i32(s,m->entry[j].child_kind);
    }
  }
  int dl_live=0; for(int i=0;i<GML_DS_LIST_MAX;i++) if(vm->ds_list[i].live) dl_live++;
  sw_i32(s,dl_live);
  for(int i=0;i<GML_DS_LIST_MAX;i++) if(vm->ds_list[i].live){
    GmlDSList *l=&vm->ds_list[i];
    sw_u32(s,l->id); sw_i32(s,l->len);
    for(int j=0;j<l->len;j++){
      sw_val(s,l->item[j],0);
      sw_i32(s,l->child_kind?l->child_kind[j]:0);
    }
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
    sw_raw(s,l->name,sizeof(l->name)); sw_i32(s,l->touched);
    sw_i32(s,l->script_begin); sw_i32(s,l->script_end); }
  int live_e=0; for(int i=0;i<vm->n_rte;i++) if(vm->rte[i].used) live_e++;
  sw_i32(s,live_e);
  for(int i=0;i<vm->n_rte;i++) if(vm->rte[i].used){ GmlRtElem *e=&vm->rte[i];
    sw_i32(s,e->id); sw_i32(s,e->layer); sw_i32(s,e->type); sw_i32(s,e->sprite);
    sw_d(s,e->x); sw_d(s,e->y);
    sw_i32(s,e->sx); sw_i32(s,e->sy); sw_i32(s,e->w); sw_i32(s,e->h);
    sw_d(s,e->xs); sw_d(s,e->ys); sw_d(s,e->alpha);
    sw_i32(s,e->visible); sw_u32(s,e->blend);
    sw_i32(s,e->htiled); sw_i32(s,e->vtiled); sw_i32(s,e->stretch);
    sw_raw(s,e->name,sizeof(e->name));
    sw_d(s,e->image_index); sw_d(s,e->image_speed); sw_d(s,e->image_angle); }
  sw_u32(s,vm->phys_next_id);
  sw_d(s,vm->phys_gravity_x); sw_d(s,vm->phys_gravity_y); sw_d(s,vm->phys_update_speed);
  sw_i32(s,vm->phys_update_iterations); sw_i32(s,vm->phys_paused); sw_i32(s,vm->phys_debug_draw);
  int live_pf=0; for(int i=0;i<GML_PHYS_FIXTURE_MAX;i++) if(vm->phys_fixture[i].live) live_pf++;
  sw_i32(s,live_pf);
  for(int i=0;i<GML_PHYS_FIXTURE_MAX;i++) if(vm->phys_fixture[i].live){
    GmlPhysicsFixture *f=&vm->phys_fixture[i];
    sw_u32(s,f->id); sw_i32(s,f->shape); sw_i32(s,f->bound_inst); sw_i32(s,f->points);
    sw_d(s,f->density); sw_d(s,f->friction); sw_d(s,f->restitution);
    sw_d(s,f->lin_damp); sw_d(s,f->ang_damp); sw_d(s,f->awake);
    sw_d(s,f->radius); sw_d(s,f->w); sw_d(s,f->h);
    sw_d(s,f->x1); sw_d(s,f->y1); sw_d(s,f->x2); sw_d(s,f->y2);
    for(int p=0;p<GML_PHYS_FIXTURE_POINTS;p++){ sw_d(s,f->px[p]); sw_d(s,f->py[p]); }
  }
  int live_pj=0; for(int i=0;i<GML_PHYS_JOINT_MAX;i++) if(vm->phys_joint[i].live) live_pj++;
  sw_i32(s,live_pj);
  for(int i=0;i<GML_PHYS_JOINT_MAX;i++) if(vm->phys_joint[i].live){
    GmlPhysicsJoint *j=&vm->phys_joint[i];
    sw_u32(s,j->id); sw_i32(s,j->type); sw_i32(s,j->value_count);
    sw_d(s,j->a); sw_d(s,j->b); sw_d(s,j->x1); sw_d(s,j->y1); sw_d(s,j->x2); sw_d(s,j->y2);
    for(int p=0;p<24;p++) sw_d(s,j->params[p]);
  }
  sw_i32(s,vm->window_cursor);
  sw_particle_state(s);
  sw_raw(s,vm->key_map,sizeof(vm->key_map));
  sw_raw(s,vm->emitter_live,sizeof(vm->emitter_live));
  for(int i=0;i<GML_MAX_EMITTERS;i++){
    sw_d(s,vm->emitter_gain[i]); sw_d(s,vm->emitter_x[i]); sw_d(s,vm->emitter_y[i]); sw_d(s,vm->emitter_z[i]);
    sw_d(s,vm->emitter_ref[i]); sw_d(s,vm->emitter_max[i]); sw_d(s,vm->emitter_factor[i]);
  }
  sw_d(s,vm->listener_x); sw_d(s,vm->listener_y); sw_d(s,vm->listener_z);
  sw_d(s,vm->listener_forward_x); sw_d(s,vm->listener_forward_y); sw_d(s,vm->listener_forward_z);
  sw_d(s,vm->listener_up_x); sw_d(s,vm->listener_up_y); sw_d(s,vm->listener_up_z);
  sw_i32(s,vm->audio_falloff_model);
  int static_live=0;
  for(int i=0;i<vm->code_static_count;i++)
    if((vm->code_static_init && vm->code_static_init[i]) || vm->code_static[i].len>0) static_live++;
  sw_i32(s,static_live);
  for(int i=0;i<vm->code_static_count;i++){
    if((!vm->code_static_init || !vm->code_static_init[i]) && vm->code_static[i].len<=0) continue;
    sw_i32(s,i); sw_i32(s,vm->code_static_init?vm->code_static_init[i]!=0:0);
    sw_varmap(s,&vm->code_static[i]);
  }
  sw_u32(s,vm->next_time_source_id);
  sw_i32(s,vm->time_source_game_state);
  int time_source_live=0;
  for(int i=0;i<GML_TIME_SOURCE_MAX;i++) if(vm->time_source[i].live) time_source_live++;
  sw_i32(s,time_source_live);
  for(int i=0;i<GML_TIME_SOURCE_MAX;i++) if(vm->time_source[i].live){
    GmlTimeSource *source=&vm->time_source[i];
    sw_u32(s,source->id); sw_i32(s,source->parent);
    sw_d(s,source->period); sw_d(s,source->remaining);
    sw_i32(s,source->units); sw_i32(s,source->state);
    sw_i32(s,source->repetitions); sw_i32(s,source->reps_remaining);
    sw_i32(s,source->reps_completed); sw_i32(s,source->expiry_type);
    sw_val(s,source->callback,0); sw_val(s,source->args,0);
  }
  /* window_set_size and display_set_gui_size are persistent presentation state. Retain them
   * across restoration so simulation and input use the same window and GUI geometry. */
  sw_i32(s,vm->window_w); sw_i32(s,vm->window_h);
  sw_i32(s,vm->gui_w); sw_i32(s,vm->gui_h);
  int view_override_live=0;
  for(int i=0;i<vm->n_view_ovr;i++) if(vm->view_ovr &&
      (vm->view_ovr[i].set || vm->view_ovr[i].full || vm->view_ovr[i].room_enabled_set))
    view_override_live++;
  sw_i32(s,view_override_live);
  for(int i=0;i<vm->n_view_ovr;i++) if(vm->view_ovr &&
      (vm->view_ovr[i].set || vm->view_ovr[i].full || vm->view_ovr[i].room_enabled_set)){
    struct GmlViewOvr *o=&vm->view_ovr[i];
    sw_i32(s,i);
    sw_i32(s,o->set); sw_i32(s,o->full); sw_i32(s,o->vis);
    sw_i32(s,o->room_enabled_set); sw_i32(s,o->room_enabled);
    sw_i32(s,o->x); sw_i32(s,o->y); sw_i32(s,o->w); sw_i32(s,o->h);
    sw_i32(s,o->view_x); sw_i32(s,o->view_y); sw_i32(s,o->view_w); sw_i32(s,o->view_h);
    sw_i32(s,o->hborder); sw_i32(s,o->vborder); sw_i32(s,o->hspeed); sw_i32(s,o->vspeed);
    sw_i32(s,o->object);
  }
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
      && magic!=0x3D564D47u && magic!=0x3E564D47u && magic!=0x3F564D47u
      && magic!=0x40564D47u && magic!=0x41564D47u && magic!=0x42564D47u
      && magic!=0x43564D47u && magic!=0x44564D47u && magic!=0x45564D47u
      && magic!=0x46564D47u && magic!=0x47564D47u && magic!=0x48564D47u
      && magic!=0x49564D47u && magic!=0x4A564D47u && magic!=0x4B564D47u
      && magic!=0x4C564D47u && magic!=0x4D564D47u && magic!=0x4E564D47u
      && magic!=0x4F564D47u && magic!=0x50564D47u && magic!=0x51564D47u
      && magic!=0x52564D47u && magic!=0x53564D47u && magic!=0x54564D47u
      && magic!=0x55564D47u) || !s.ok){ state_debug("bad vm magic",s.pos,magic); return 0; }
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
  s.v14 = magic>=0x3E564D47u;
  s.v15 = magic>=0x3F564D47u;
  s.v16 = magic>=0x40564D47u;
  s.v17 = magic>=0x41564D47u;
  s.v18 = magic>=0x42564D47u;
  s.v19 = magic>=0x43564D47u;
  s.v20 = magic>=0x44564D47u;
  s.v21 = magic>=0x45564D47u;
  s.v22 = magic>=0x46564D47u;
  s.v23 = magic>=0x47564D47u;
  s.v24 = magic>=0x48564D47u;
  s.v25 = magic>=0x49564D47u;
  s.v26 = magic>=0x4A564D47u;
  s.v27 = magic>=0x4B564D47u;
  s.v28 = magic>=0x4C564D47u;
  s.v29 = magic>=0x4D564D47u;
  s.v30 = magic>=0x4E564D47u;
  s.v31 = magic>=0x4F564D47u;
  s.v32 = magic>=0x50564D47u;
  s.v33 = magic>=0x51564D47u;
  s.v34 = magic>=0x52564D47u;
  s.v35 = magic>=0x53564D47u;
  s.v36 = magic>=0x54564D47u;
  s.v37 = magic>=0x55564D47u;
  void *render=vm->render, *audio=vm->audio;
  runtime_clear(vm);
  vm->ds_list_compat_repair = !s.v8;
  int inst_count=sr_i32(&s); if(inst_count<0 || inst_count>vm->inst_cap) s.ok=0;
  if(!s.ok) state_debug("bad inst_count",s.pos,(uint32_t)inst_count);
  vm->next_id=sr_u32(&s);
  vm->next_creation_seq=s.v33?(uint64_t)sr_i64(&s):1;
  vm->room_index=sr_i32(&s); vm->pending_room=sr_i32(&s);
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
  vm->math_epsilon=s.v32?sr_d(&s):((vm->win && vm->win->classic_version)?1e-13:1e-5);
  if(s.v24){
    vm->potential_max_rotation=sr_d(&s); vm->potential_rotate_step=sr_d(&s);
    vm->potential_check_distance=sr_d(&s); vm->potential_rotate_on_spot=sr_i32(&s);
  }
  vm->classic_info_active=s.v25?sr_i32(&s):0;
  vm->script_argc=sr_i32(&s); for(int i=0;i<16;i++){ vm->script_args[i]=sr_val(vm,&s,0); gml_arr_mark_escaped(vm->script_args[i]); }
  for(int i=0;i<16;i++) vm->rng_well[i]=sr_u32(&s);
  vm->rng_index=sr_i32(&s); vm->rng_state=sr_u32(&s);
  vm->rng_classic_state=s.v19?sr_u32(&s):vm->rng_state;
  if(s.v19){ int flags[GML_D3_STATE_FLAG_COUNT]={0};
    double values[GML_D3_STATE_VALUE_COUNT]={0};
    uint32_t colors[GML_D3_STATE_COLOR_COUNT]={0};
    colors[9]=0x262626u;
    flags[21]=1; flags[22]=1; flags[24]=1;
    values[49]=41.2; values[51]=1; values[52]=32000;
    values[56]=values[61]=values[66]=values[71]=1;
    int flag_count=s.v21?GML_D3_STATE_FLAG_COUNT:(s.v20?21:19);
    int value_count=s.v31?GML_D3_STATE_VALUE_COUNT:(s.v21?584:(s.v20?49:44));
    int color_count=s.v23?GML_D3_STATE_COLOR_COUNT:(s.v21?9:8);
    for(int i=0;i<flag_count;i++) flags[i]=sr_i32(&s);
    for(int i=0;i<value_count;i++) values[i]=sr_d(&s);
    for(int i=0;i<color_count;i++) colors[i]=sr_u32(&s);
    if(s.ok) gml_d3_state_set(flags,values,colors);
  } else gml_d3_reset();
  if(s.v22){
    uint32_t model_size=sr_u32(&s);
    if(s.ok && s.pos<=s.cap && model_size<=s.cap-s.pos){
      if(!gml_d3_models_state_load(s.data+s.pos,model_size)) s.ok=0;
      s.pos+=model_size;
    } else s.ok=0;
  }
  if(s.v19){
    int count=sr_i32(&s);
    if(count<0 || count>100000){ s.ok=0; count=0; }
    if(count!=vm->room_state_count){
      unsigned char *stored=calloc((size_t)(count>0?count:1),1);
      if(!stored && count>0) s.ok=0;
      else { free(vm->room_stored); vm->room_stored=stored; vm->room_state_count=count; }
    }
    if(count>0) sr_raw(&s,vm->room_stored,(size_t)count);
  } else if(vm->room_stored && vm->room_state_count>0)
    memset(vm->room_stored,0,(size_t)vm->room_state_count);
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
  if(vm->ini_n<0 || vm->ini_n>GML_INI_MAX){ state_debug("bad ini count",s.pos,(uint32_t)vm->ini_n); s.ok=0; }
  int ini_n=vm->ini_n; vm->ini_n=0;
  for(int i=0;i<ini_n && i<GML_INI_MAX;i++){
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
    m->last_lookup=-1;
    if(m->len<0 || m->len>100000){ state_debug("bad ds_map len",s.pos,(uint32_t)m->len); s.ok=0; m->len=0; }
    m->cap=m->len; m->entry=m->cap?calloc((size_t)m->cap,sizeof(GmlDSMapEntry)):NULL;
    for(int j=0;j<m->len;j++){
      m->entry[j].key=sr_str_dup(&s);
      m->entry[j].key_val=sr_val(vm,&s,0);
      m->entry[j].val=sr_val(vm,&s,0);
      if(s.v29){
        int kind=sr_i32(&s);
        if(kind<0 || kind>2){ state_debug("bad ds_map child kind",s.pos,(uint32_t)kind); s.ok=0; kind=0; }
        m->entry[j].child_kind=(unsigned char)kind;
      }
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
      l->child_kind=l->cap?calloc((size_t)l->cap,1):NULL;
      if(l->cap && (!l->item || !l->child_kind)){ s.ok=0; free(l->item); free(l->child_kind); l->item=NULL; l->child_kind=NULL; l->len=l->cap=0; break; }
      for(int j=0;j<l->len;j++){
        l->item[j]=sr_val(vm,&s,0);
        if(s.v29){
          int kind=sr_i32(&s);
          if(kind<0 || kind>2){ state_debug("bad ds_list child kind",s.pos,(uint32_t)kind); s.ok=0; kind=0; }
          l->child_kind[j]=(unsigned char)kind;
        }
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
  if(!s.v26 && vm->win && vm->win->classic_version)
    for(int i=0;i<vm->inst_count;i++)
      vm->inst[i].room_placed=(unsigned char)state_instance_is_room_placed(vm,&vm->inst[i]);
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
      if(s.v16){ l->script_begin=sr_i32(&s); l->script_end=sr_i32(&s); }
      else { l->script_begin=-1; l->script_end=-1; }
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
      if(s.v14){
        sr_raw(&s,e->name,sizeof(e->name));
        e->name[sizeof(e->name)-1]=0;
        e->image_index=sr_d(&s); e->image_speed=sr_d(&s); e->image_angle=sr_d(&s);
      } else {
        e->name[0]=0; e->image_index=0; e->image_speed=0; e->image_angle=0;
      }
      e->id=id;
    }
  }
  if(s.v17 && s.ok){
    vm->phys_next_id=sr_u32(&s);
    vm->phys_gravity_x=sr_d(&s); vm->phys_gravity_y=sr_d(&s); vm->phys_update_speed=sr_d(&s);
    vm->phys_update_iterations=sr_i32(&s); vm->phys_paused=sr_i32(&s); vm->phys_debug_draw=sr_i32(&s);
    int nf=sr_i32(&s);
    if(nf<0 || nf>GML_PHYS_FIXTURE_MAX){ state_debug("bad physics fixture count",s.pos,(uint32_t)nf); s.ok=0; nf=0; }
    for(int i=0;i<nf && s.ok;i++){
      GmlPhysicsFixture *f=&vm->phys_fixture[i];
      memset(f,0,sizeof(*f));
      f->live=1; f->id=sr_u32(&s); f->shape=sr_i32(&s); f->bound_inst=sr_i32(&s); f->points=sr_i32(&s);
      if(f->points<0) f->points=0;
      if(f->points>GML_PHYS_FIXTURE_POINTS) f->points=GML_PHYS_FIXTURE_POINTS;
      f->density=sr_d(&s); f->friction=sr_d(&s); f->restitution=sr_d(&s);
      f->lin_damp=sr_d(&s); f->ang_damp=sr_d(&s); f->awake=sr_d(&s);
      f->radius=sr_d(&s); f->w=sr_d(&s); f->h=sr_d(&s);
      f->x1=sr_d(&s); f->y1=sr_d(&s); f->x2=sr_d(&s); f->y2=sr_d(&s);
      for(int p=0;p<GML_PHYS_FIXTURE_POINTS;p++){ f->px[p]=sr_d(&s); f->py[p]=sr_d(&s); }
    }
    int nj=sr_i32(&s);
    if(nj<0 || nj>GML_PHYS_JOINT_MAX){ state_debug("bad physics joint count",s.pos,(uint32_t)nj); s.ok=0; nj=0; }
    for(int i=0;i<nj && s.ok;i++){
      GmlPhysicsJoint *j=&vm->phys_joint[i];
      memset(j,0,sizeof(*j));
      j->live=1; j->id=sr_u32(&s); j->type=sr_i32(&s); j->value_count=sr_i32(&s);
      if(j->value_count<0) j->value_count=0;
      if(j->value_count>24) j->value_count=24;
      j->a=sr_d(&s); j->b=sr_d(&s); j->x1=sr_d(&s); j->y1=sr_d(&s); j->x2=sr_d(&s); j->y2=sr_d(&s);
      for(int p=0;p<24;p++) j->params[p]=sr_d(&s);
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
  if(s.v27 && s.ok){
    sr_raw(&s,vm->key_map,sizeof(vm->key_map));
    for(int i=0;i<256;i++) if(vm->key_map[i] < -1 || vm->key_map[i] > 255){
      state_debug("bad keyboard map",s.pos,(uint32_t)(uint16_t)vm->key_map[i]);
      s.ok=0;
      break;
    }
  }
  if(s.v28 && s.ok){
    sr_raw(&s,vm->emitter_live,sizeof(vm->emitter_live));
    for(int i=0;i<GML_MAX_EMITTERS;i++){
      vm->emitter_gain[i]=sr_d(&s); vm->emitter_x[i]=sr_d(&s); vm->emitter_y[i]=sr_d(&s); vm->emitter_z[i]=sr_d(&s);
      vm->emitter_ref[i]=sr_d(&s); vm->emitter_max[i]=sr_d(&s); vm->emitter_factor[i]=sr_d(&s);
      if(!isfinite(vm->emitter_gain[i]) || vm->emitter_gain[i]<0 || !isfinite(vm->emitter_x[i]) ||
         !isfinite(vm->emitter_y[i]) || !isfinite(vm->emitter_z[i]) || !isfinite(vm->emitter_ref[i]) ||
         !isfinite(vm->emitter_max[i]) || !isfinite(vm->emitter_factor[i])) s.ok=0;
    }
    vm->listener_x=sr_d(&s); vm->listener_y=sr_d(&s); vm->listener_z=sr_d(&s);
    vm->listener_forward_x=sr_d(&s); vm->listener_forward_y=sr_d(&s); vm->listener_forward_z=sr_d(&s);
    vm->listener_up_x=sr_d(&s); vm->listener_up_y=sr_d(&s); vm->listener_up_z=sr_d(&s);
    vm->audio_falloff_model=sr_i32(&s);
    if(vm->audio_falloff_model<0 || vm->audio_falloff_model>6) s.ok=0;
  }
  if(s.v34 && s.ok){
    int live=sr_i32(&s);
    if(live<0 || live>vm->code_static_count){ state_debug("bad static scope count",s.pos,(uint32_t)live); s.ok=0; live=0; }
    for(int i=0;i<live && s.ok;i++){
      int ci=sr_i32(&s), initialized=sr_i32(&s);
      if(ci<0 || ci>=vm->code_static_count || (initialized!=0 && initialized!=1)){
        state_debug("bad static scope",s.pos,(uint32_t)ci); s.ok=0; break;
      }
      vm->code_static_init[ci]=(unsigned char)initialized;
      sr_varmap(vm,&s,&vm->code_static[ci]);
    }
  }
  if(s.v35 && s.ok){
    vm->next_time_source_id=sr_u32(&s);
    vm->time_source_game_state=sr_i32(&s);
    int live=sr_i32(&s);
    if(vm->next_time_source_id<GML_TIME_SOURCE_ID_BASE || live<0 || live>GML_TIME_SOURCE_MAX ||
       vm->time_source_game_state<1 || vm->time_source_game_state>3){
      state_debug("bad time source header",s.pos,(uint32_t)live); s.ok=0; live=0;
    }
    for(int i=0;i<live && s.ok;i++){
      GmlTimeSource *source=&vm->time_source[i];
      source->live=1; source->id=sr_u32(&s); source->parent=sr_i32(&s);
      source->period=sr_d(&s); source->remaining=sr_d(&s);
      source->units=sr_i32(&s); source->state=sr_i32(&s);
      source->repetitions=sr_i32(&s); source->reps_remaining=sr_i32(&s);
      source->reps_completed=sr_i32(&s); source->expiry_type=sr_i32(&s);
      source->callback=sr_val(vm,&s,0); source->args=sr_val(vm,&s,0);
      gml_arr_mark_escaped(source->callback); gml_arr_mark_escaped(source->args);
      if(source->id<GML_TIME_SOURCE_ID_BASE || !isfinite(source->period) || source->period<0.0 ||
         !isfinite(source->remaining) || source->remaining<0.0 || source->units<0 || source->units>1 ||
         source->state<0 || source->state>3 || source->repetitions<-1 || source->reps_remaining<-1 ||
         source->reps_completed<0 || source->expiry_type<0 || source->expiry_type>1 ||
         (source->args.t!=V_ARR && source->args.t!=V_UNDEF)){
        state_debug("bad time source",s.pos,source->id); s.ok=0;
      }
    }
    for(int i=0;i<live && s.ok;i++){
      int parent=vm->time_source[i].parent, found=parent==0 || parent==1;
      for(int j=0;j<live && !found;j++) found=(int)vm->time_source[j].id==parent;
      if(!found){ state_debug("bad time source parent",s.pos,(uint32_t)parent); s.ok=0; }
    }
  }
  if(s.v36 && s.ok){
    vm->window_w=sr_i32(&s); vm->window_h=sr_i32(&s);
    vm->gui_w=sr_i32(&s); vm->gui_h=sr_i32(&s);
    if(vm->window_w<0 || vm->window_h<0 || vm->gui_w<0 || vm->gui_h<0 ||
       vm->window_w>32768 || vm->window_h>32768 || vm->gui_w>32768 || vm->gui_h>32768){
      state_debug("bad presentation geometry",s.pos,(uint32_t)vm->window_w);
      s.ok=0;
    }
  }
  if(s.v37 && s.ok){
    int slots=vm->win ? gml_room_count(vm->win)*8 : 0;
    int live=sr_i32(&s);
    if(slots<0 || live<0 || live>slots){
      state_debug("bad view override count",s.pos,(uint32_t)live);
      s.ok=0; live=0;
    }
    free(vm->view_ovr); vm->view_ovr=NULL; vm->n_view_ovr=0;
    if(live>0){
      vm->view_ovr=calloc((size_t)slots,sizeof(*vm->view_ovr));
      if(!vm->view_ovr) s.ok=0;
      else vm->n_view_ovr=slots;
    }
    for(int i=0;i<live && s.ok;i++){
      int slot=sr_i32(&s);
      if(slot<0 || slot>=slots){ state_debug("bad view override slot",s.pos,(uint32_t)slot); s.ok=0; break; }
      struct GmlViewOvr *o=&vm->view_ovr[slot];
      o->set=(unsigned char)sr_i32(&s); o->full=(unsigned char)sr_i32(&s);
      o->vis=(unsigned char)sr_i32(&s); o->room_enabled_set=(unsigned char)sr_i32(&s);
      o->room_enabled=(unsigned char)sr_i32(&s);
      o->x=sr_i32(&s); o->y=sr_i32(&s); o->w=sr_i32(&s); o->h=sr_i32(&s);
      o->view_x=sr_i32(&s); o->view_y=sr_i32(&s);
      o->view_w=sr_i32(&s); o->view_h=sr_i32(&s);
      o->hborder=sr_i32(&s); o->vborder=sr_i32(&s);
      o->hspeed=sr_i32(&s); o->vspeed=sr_i32(&s); o->object=sr_i32(&s);
      if(o->set>1 || o->full>1 || o->vis>1 || o->room_enabled_set>1 || o->room_enabled>1 ||
         o->w<0 || o->h<0 || o->view_w<0 || o->view_h<0){
        state_debug("bad view override",s.pos,(uint32_t)slot); s.ok=0;
      }
    }
  }
  vm->cur_self=vm->cur_other=NULL; vm->cur_event=NULL; vm->cur_event_obj=0;
  vm->step_active=0; vm->step_alloc_base=0;
  vm->step_free_n=vm->step_free_pos=0;
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
  if(s.ok && !s.v15 && vm->win && vm->room_index>=0 &&
     gml_room_layer_list(vm,vm->room_index,NULL)){
    typedef struct { int id, visible, script_begin, script_end; double depth; char name[32]; } MigLayer;
    typedef struct {
      int id, layer, type, sprite, sx, sy, w, h, visible, htiled, vtiled, stretch;
      double x, y, xs, ys, alpha, image_index, image_speed, image_angle;
      uint32_t blend;
      char name[64];
    } MigElem;
    size_t snap_cap=gml_vm_state_size(vm), snap_wr=0;
    unsigned char *snap=snap_cap?malloc(snap_cap):NULL;
    if(snap && gml_vm_state_save(vm,snap,snap_cap,&snap_wr)){
      if(getenv("GML_LOG_RTL")){
        fprintf(stderr,"[rtl-state] migrating pre-GMV15 state: replay Room Start for GMS2 layer setup\n");
      }
      int n0=vm->inst_count;
      for(int i=0;i<n0;i++) if(vm->inst[i].active && !vm->inst[i].marked)
        gml_run_event(vm,&vm->inst[i],"Other_4");
      reap(vm);
      int ml_n=0, me_n=0;
      for(int i=0;i<vm->n_rtl;i++) if(vm->rtl[i].used) ml_n++;
      for(int i=0;i<vm->n_rte;i++) if(vm->rte[i].used) me_n++;
      MigLayer *ml=ml_n?calloc((size_t)ml_n,sizeof(*ml)):NULL;
      MigElem *me=me_n?calloc((size_t)me_n,sizeof(*me)):NULL;
      if((ml_n && !ml) || (me_n && !me)){
        free(ml); free(me);
        size_t dummy=0;
        if(!gml_vm_state_load(vm,snap,snap_wr,&dummy)) s.ok=0;
      }else{
        int k=0;
        for(int i=0;i<vm->n_rtl;i++) if(vm->rtl[i].used){
          GmlRtLayer *l=&vm->rtl[i];
          ml[k].id=l->id; ml[k].visible=l->visible; ml[k].depth=l->depth;
          ml[k].script_begin=l->script_begin; ml[k].script_end=l->script_end;
          memcpy(ml[k].name,l->name,sizeof(ml[k].name));
          k++;
        }
        k=0;
        for(int i=0;i<vm->n_rte;i++) if(vm->rte[i].used){
          GmlRtElem *e=&vm->rte[i];
          me[k].id=e->id; me[k].layer=e->layer; me[k].type=e->type; me[k].sprite=e->sprite;
          me[k].x=e->x; me[k].y=e->y; me[k].sx=e->sx; me[k].sy=e->sy; me[k].w=e->w; me[k].h=e->h;
          me[k].xs=e->xs; me[k].ys=e->ys; me[k].alpha=e->alpha; me[k].visible=e->visible; me[k].blend=e->blend;
          me[k].htiled=e->htiled; me[k].vtiled=e->vtiled; me[k].stretch=e->stretch;
          me[k].image_index=e->image_index; me[k].image_speed=e->image_speed; me[k].image_angle=e->image_angle;
          memcpy(me[k].name,e->name,sizeof(me[k].name));
          k++;
        }
        size_t dummy=0;
        if(gml_vm_state_load(vm,snap,snap_wr,&dummy)){
          for(int i=0;i<ml_n;i++){
            GmlRtLayer *l=gml_rt_layer_find(vm,ml[i].id);
            if(!l && ml[i].name[0]) l=gml_rt_layer_find_by_name(vm,ml[i].name);
            if(l){ l->visible=ml[i].visible; l->depth=ml[i].depth;
              l->script_begin=ml[i].script_begin; l->script_end=ml[i].script_end; }
          }
          for(int i=0;i<me_n;i++){
            GmlRtElem *e=gml_rt_elem_find(vm,me[i].id);
            if(!e && me[i].name[0]){
              for(int j=0;j<vm->n_rte;j++) if(vm->rte[j].used && vm->rte[j].layer==me[i].layer &&
                   vm->rte[j].type==me[i].type && !strcmp(vm->rte[j].name,me[i].name)){ e=&vm->rte[j]; break; }
            }
            if(!e) continue;
            e->layer=me[i].layer; e->type=me[i].type; e->sprite=me[i].sprite;
            e->x=me[i].x; e->y=me[i].y; e->sx=me[i].sx; e->sy=me[i].sy; e->w=me[i].w; e->h=me[i].h;
            e->xs=me[i].xs; e->ys=me[i].ys; e->alpha=me[i].alpha; e->visible=me[i].visible; e->blend=me[i].blend;
            e->htiled=me[i].htiled; e->vtiled=me[i].vtiled; e->stretch=me[i].stretch;
            e->image_index=me[i].image_index; e->image_speed=me[i].image_speed; e->image_angle=me[i].image_angle;
            memcpy(e->name,me[i].name,sizeof(e->name));
          }
          if(vm->win && vm->room_index>=0) gml_room_reload_layers_mode(vm, vm->room_index, 0);
        }else s.ok=0;
        free(ml); free(me);
      }
    }
    free(snap);
  }
  if(used) *used=s.pos;
  if(s.ok && s.pos<=len){
    vm_warm_audio_for_room_window(vm);
    vm_prefetch_room_assets(vm);
  }
  return s.ok && s.pos<=len;
}
