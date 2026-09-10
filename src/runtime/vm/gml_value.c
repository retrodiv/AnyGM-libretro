/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Canonical language-value, array, variable-map, and lifetime implementation. */
#include "gml_value_internal.h"

#include <stdlib.h>
#include <string.h>


/* ---------------- var map (key = interned name pointer) ---------------- */
/* hash/compare keys by CONTENT (FNV-1a + strcmp), not pointer identity: VM names come from
 * the data.win string table while C-side writers (builtins, room init) use literals — the same
 * variable must hit the same slot regardless of which pointer carries the name. */
unsigned gml_value_name_hash(const char *p){ unsigned h=2166136261u; if(!p) p=""; while(*p){ h^=(unsigned char)*p++; h*=16777619u; } return h; }

static void varmap_grow(GmlVarMap *m){
  int nc = m->cap? m->cap*2 : 16;
  GmlVarSlot *ns = calloc(nc,sizeof(GmlVarSlot));
  for(int i=0;i<m->cap;i++) if(m->slots[i].key){
    if(!m->slots[i].hash) m->slots[i].hash=gml_value_name_hash(m->slots[i].key);
    unsigned h=m->slots[i].hash&(nc-1);
    while(ns[h].key) h=(h+1)&(nc-1);
    ns[h]=m->slots[i];
  }
  free(m->slots); m->slots=ns; m->cap=nc;
}
GmlVal *gml_varmap_get_hashed(GmlVarMap *m, const char *key, uint32_t kh){
  if(!m->cap) return NULL;
  unsigned h=kh&(m->cap-1);
  while(m->slots[h].key){
    if(m->slots[h].hash==kh && (m->slots[h].key==key || !strcmp(m->slots[h].key,key))) return &m->slots[h].val;
    h=(h+1)&(m->cap-1);
  }
  return NULL;
}
GmlVal *gml_varmap_get(GmlVarMap *m, const char *key){
  return key?gml_varmap_get_hashed(m,key,gml_value_name_hash(key)):NULL;
}
static GmlVarSlot *gml_varmap_put_slot_h(GmlVarMap *m, const char *key, uint32_t kh,
                                         int key_owned){
  if(!key){ key=""; kh=gml_value_name_hash(key); key_owned=0; }
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
GmlVal *gml_varmap_put_hashed(GmlVarMap *m, const char *key, uint32_t kh){
  return &gml_varmap_put_slot_h(m,key,kh,0)->val;
}
GmlVal *gml_varmap_put_owned_hashed(GmlVarMap *m, char *key, uint32_t kh){
  return &gml_varmap_put_slot_h(m,key,kh,1)->val;
}
GmlVal *gml_varmap_put(GmlVarMap *m, const char *key){
  return gml_varmap_put_hashed(m,key,key?gml_value_name_hash(key):gml_value_name_hash(""));
}
GmlVal *gml_varmap_put_owned(GmlVarMap *m, char *key){
  return gml_varmap_put_owned_hashed(m,key,gml_value_name_hash(key?key:""));
}

void gml_arr_row_ensure(GmlArr *A, int row){
  if(!A || row<0) return;
  if(row>=A->row_cap){
    int nc=A->row_cap?A->row_cap:8; while(nc<=row) nc*=2;
    int *nr=realloc(A->row_len,(size_t)nc*sizeof(int)); if(!nr) return;
    A->row_len=nr; for(int i=A->row_cap;i<nc;i++) A->row_len[i]=0; A->row_cap=nc;
  }
  if(row>=A->height2d) A->height2d=row+1;
}
void gml_arr_note_legacy_2d_set(GmlArr *A, int idx){
  if(!A || idx<0) return;
  if(idx>=GML_2D_STRIDE || A->is_2d){
    if(!A->is_2d){
      A->is_2d=1;
      if(A->len>0){
        gml_arr_row_ensure(A,0);
        A->row_len[0]=A->len<GML_2D_STRIDE?A->len:GML_2D_STRIDE;
      }
    }
    int row=idx/GML_2D_STRIDE, col=idx%GML_2D_STRIDE;
    gml_arr_row_ensure(A,row);
    if(A->row_len && col+1>A->row_len[row]) A->row_len[row]=col+1;
  }
}
static int arr_value_nonzero(GmlVal v){ return !(v.t==V_REAL && v.d==0.0); }
void gml_arr_rebuild_legacy_2d_meta(GmlArr *A){
  if(!A || A->len<=GML_2D_STRIDE) return;
  free(A->row_len); A->row_len=NULL; A->row_cap=0; A->height2d=0; A->is_2d=0;
  for(int i=0;i<A->len;i++){
    if(!arr_value_nonzero(A->data[i])) continue;
    int row=i/GML_2D_STRIDE, col=i%GML_2D_STRIDE;
    if(row<=0) continue;
    if(!A->is_2d) A->is_2d=1;
    gml_arr_row_ensure(A,row);
    if(A->row_len && col+1>A->row_len[row]) A->row_len[row]=col+1;
  }
  if(A->is_2d){
    int row0=0;
    int lim=A->len<GML_2D_STRIDE?A->len:GML_2D_STRIDE;
    for(int i=0;i<lim;i++) if(arr_value_nonzero(A->data[i])) row0=i+1;
    gml_arr_row_ensure(A,0);
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
GmlArr *gml_arr_slot_ensure(GmlVal *slot){
  if(slot->t!=V_ARR){ GmlArr *A=calloc(1,sizeof(GmlArr)); slot->t=V_ARR; slot->arr=A; }
  return (GmlArr*)slot->arr;
}
/* Reject oversized indices before capacity doubling can overflow and zero-fill an
 * unbounded allocation. Out-of-range requests leave the array unchanged. */
/* Optional callback configured by the VM diagnostics owner; normally NULL. */
void (*gml_arr_growth_hook)(int index,int length)=NULL;
void gml_arr_index_ensure(GmlArr *A, int idx){
  if(idx<0 || idx>=GML_ARR_MAX_INDEX) return;
  if(gml_arr_growth_hook && A && idx>=A->len) gml_arr_growth_hook(idx,A->len);
  if(idx>=A->cap){ int nc=A->cap?A->cap:8; while(nc<=idx) nc*=2;
    /* A zeroed GmlVal IS vreal(0) — V_REAL is 0, 0.0 is all-zero bits and both pointers are null —
     * so the grown region needs no initialising pass. That pass was the whole resident cost of a
     * legacy 2D array: those live flat at row*32000+col, so a large table reaches a huge index and
     * writing a zero into every hole faulted in a great many pages that hold nothing. calloc
     * hands back untouched zero pages instead, and only the rows that carry data get copied. */
    if(A->is_2d && A->row_len){
      GmlVal *grown=(GmlVal*)calloc((size_t)nc,sizeof(GmlVal));
      if(!grown) return;
      if(A->data){
        for(int r=0;r<A->height2d && r<A->row_cap;r++){
          long base=(long)r*GML_2D_STRIDE;
          long span=A->row_len[r];
          if(base>=A->cap || span<=0) continue;
          if(base+span>A->cap) span=A->cap-base;
          memcpy(grown+base,A->data+base,(size_t)span*sizeof(GmlVal));
        }
        free(A->data);
      }
      A->data=grown;
    } else {
      /* A dense array has no holes to avoid: extend in place where the allocator can and write
       * the zeros, which is cheaper than allocating a second block and copying the whole of it. */
      GmlVal *grown=(GmlVal*)realloc(A->data,(size_t)nc*sizeof(GmlVal));
      if(!grown) return;
      A->data=grown;
      for(int i=A->cap;i<nc;i++) A->data[i]=vreal(0);
    }
    A->cap=nc; }
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
  if(size<0) size=0;
  if(size>16000000) size=16000000;
  if(A && size>0){ A->data=malloc((size_t)size*sizeof(GmlVal));
    if(A->data){ A->cap=size; A->len=size; for(int i=0;i<size;i++) A->data[i]=arr_store_clone(fill); } }
  if(A) A->escaped=1;
  GmlVal v; v.t=V_ARR; v.d=0; v.s=NULL; v.arr=A; return v;
}
void gml_arr_set(GmlVal arr, int idx, GmlVal val){
  if(arr.t!=V_ARR || !arr.arr || idx<0) return;
  GmlArr *A=arr.arr; gml_arr_index_ensure(A,idx);
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
    gml_arr_index_ensure(A,idx);
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
    int idx=(int)flat; gml_arr_note_legacy_2d_set(A,idx); gml_arr_index_ensure(A,idx);
    if(idx<A->cap) A->data[idx]=arr_store_clone(val);
    return;
  }
  gml_arr_index_ensure(A,row); if(row>=A->cap) return;
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
int gml_arr_nested_set_flat(GmlVal arr,int flat,GmlVal value){
  if(arr.t!=V_ARR || !arr.arr || !((GmlArr*)arr.arr)->nested_2d) return 0;
  if(flat>=0) gml_arr_set_2d(arr,flat/GML_2D_STRIDE,flat%GML_2D_STRIDE,value);
  return 1;
}
int gml_arr_nested_get_flat(GmlVal arr,int flat,GmlVal *out){
  if(arr.t!=V_ARR || !arr.arr || !((GmlArr*)arr.arr)->nested_2d) return 0;
  *out=flat>=0?gml_arr_get_2d(arr,flat/GML_2D_STRIDE,flat%GML_2D_STRIDE):vreal(0);
  return 1;
}
void gml_arr_push(GmlVal arr, GmlVal val){
  if(arr.t!=V_ARR || !arr.arr) return;
  GmlArr *A=arr.arr; int idx=A->len; gml_arr_index_ensure(A,idx);
  if(idx<A->cap){ A->data[idx]=arr_store_clone(val); A->len=idx+1; }
}
GmlVal gml_arr_pop(GmlVal arr){   /* remove+return last; transfer ownership of an owned string out */
  if(arr.t!=V_ARR || !arr.arr) return vreal(0);
  GmlArr *A=arr.arr; if(A->len<=0) return vreal(0);
  GmlVal v=A->data[--A->len]; A->data[A->len]=vreal(0); return v;
}
void gml_arr_resize(GmlVal arr, int size){
  if(arr.t!=V_ARR || !arr.arr || size<0 || size>16000000) return;
  GmlArr *A=arr.arr; if(size>0) gml_arr_index_ensure(A,size-1); if(size<A->len){ for(int i=size;i<A->len;i++) A->data[i]=vreal(0); } A->len=size;
}
void gml_arr_copy(GmlVal dst, int di, GmlVal src, int si, int count){
  if(dst.t!=V_ARR || !dst.arr || src.t!=V_ARR || !src.arr || di<0 || si<0 || count<=0) return;
  GmlArr *D=dst.arr, *S=src.arr;
  for(int k=0;k<count;k++){ if(si+k>=S->len) break; gml_arr_index_ensure(D,di+k); if(di+k<D->cap){ D->data[di+k]=arr_store_clone(S->data[si+k]); if(di+k>=D->len) D->len=di+k+1; } }
}
void gml_arr_insert(GmlVal arr, int index, GmlVal *values, int count){
  if(arr.t!=V_ARR || !arr.arr || !values || count<=0) return;
  GmlArr *A=arr.arr;
  int old_len=A->len;
  if(index<0) index+=old_len;       /* -1 addresses the current last element. */
  if(index<0) index=0;
  if(index>16000000-count) return;
  if(index>old_len){
    gml_arr_resize(arr,index+count); /* gml_arr_index_ensure zero-fills the gap. */
  } else {
    gml_arr_resize(arr,old_len+count);
    if(A->len!=old_len+count) return;
    memmove(&A->data[index+count],&A->data[index],
            (size_t)(old_len-index)*sizeof(*A->data));
  }
  for(int i=0;i<count;i++) A->data[index+i]=arr_store_clone(values[i]);
}
/* A teardown operation owns its deduplication set. Aliased arrays can span globals, instances and
 * structs, so full VM teardown passes one context across every map without introducing process
 * state or coupling separate VM instances. */
static int freeset_seen(GmlValueFreeContext *context,void *p){
  if(!context || !context->active) return 0;
  if(context->count*4 >= context->capacity*3){
    size_t nc=context->capacity?context->capacity*2:256;
    void **ns=calloc(nc,sizeof(void*));
    if(!ns) return 0;
    for(size_t i=0;i<context->capacity;i++) if(context->set[i]){
      size_t h=((uintptr_t)context->set[i]>>4)&(nc-1);
      while(ns[h]) h=(h+1)&(nc-1);
      ns[h]=context->set[i];
    }
    free(context->set); context->set=ns; context->capacity=nc;
  }
  size_t h=((uintptr_t)p>>4)&(context->capacity-1);
  while(context->set[h]){ if(context->set[h]==p) return 1; h=(h+1)&(context->capacity-1); }
  context->set[h]=p; context->count++;
  return 0;
}
void gml_value_free_context_begin(GmlValueFreeContext *context){
  if(!context) return;
  context->active=1;
  context->count=0;
  if(context->set) memset(context->set,0,context->capacity*sizeof(void*));
}
void gml_value_free_context_end(GmlValueFreeContext *context){
  if(!context) return;
  free(context->set);
  memset(context,0,sizeof(*context));
}
void gml_val_free(GmlValueFreeContext *context,GmlVal v){
  if(v.t==V_STR && v.d!=0 && v.s && context && context->owned_strings){
    if(!freeset_seen(context,(void*)v.s)) free((void*)v.s);
    return;
  }
  if(v.t!=V_ARR || !v.arr) return;
  GmlArr *A=v.arr;
  if(freeset_seen(context,A)) return;
  /* Escaped arrays are shared reference values. Outside a full teardown sweep,
   * a local/container cleanup must not free a child another live slot still owns. */
  if(A->escaped && (!context || context->skip_escaped || !context->active)) return;
  /* defend against a corrupt GmlArr (garbage len/cap/data from a bad savestate) — free only the
   * struct, never walk a garbage data pointer. */
  if(A->len<0 || A->cap<A->len || A->cap>GML_ARR_MAX_CAP || (A->len>0 && !A->data)){
    free(A->row_len); if((uintptr_t)A->data>0x1000 && A->cap>=0 && A->cap<=GML_ARR_MAX_CAP) free(A->data); free(A); return;
  }
  /* Walk occupied row spans in the flat row*32000+col array layout. */
  if(A->is_2d && A->row_len){
    for(int r=0;r<A->height2d && r<A->row_cap;r++){
      long base=(long)r*GML_2D_STRIDE;
      for(int c=0;c<A->row_len[r] && base+c<A->len;c++) gml_val_free(context,A->data[base+c]);
    }
  } else {
    for(int i=0;i<A->len;i++) gml_val_free(context,A->data[i]);
  }
  free(A->row_len);
  free(A->data);
  free(A);
}
/* GM arrays are value types (copied on assignment); our VM stores GmlArr POINTERS instead.
 * When a script does `var b; b[0]=...; global.x=b`, both slots alias one GmlArr — freeing the
 * locals at scope exit killed the global's array.
 * Escaped arrays (stored to a persistent slot, a ds structure, or returned) are marked and the
 * locals cleanup skips them; full teardown (instance destroy, state load, vm free) still frees. */
void gml_varmap_free_with_context(GmlVarMap *m,int skip_escaped,GmlValueFreeContext *context){
  GmlValueFreeContext local={0};
  int owns_context=0;
  if(!context){
    context=&local;
    if(skip_escaped) gml_value_free_context_begin(context);
    owns_context=1;
  }
  int previous_skip=context->skip_escaped;
  if(skip_escaped) context->skip_escaped=1;
  for(int i=0;i<m->cap;i++) if(m->slots && m->slots[i].key && m->slots[i].val.t==V_ARR){
    gml_val_free(context,m->slots[i].val); }
  for(int i=0;i<m->cap;i++) if(m->slots && m->slots[i].key_owned){
    free((char*)m->slots[i].key); }
  context->skip_escaped=previous_skip;
  if(owns_context) gml_value_free_context_end(context);
  free(m->slots); m->slots=0; m->cap=m->len=0;
}
void gml_varmap_free_ex(GmlVarMap *m,int skip_escaped){
  gml_varmap_free_with_context(m,skip_escaped,NULL);
}
void gml_varmap_free(GmlVarMap *m){ gml_varmap_free_ex(m,0); }
static void values_release(GmlVal *values,size_t count,int owned_strings){
  GmlValueFreeContext context={0};
  if(!values && count) return;
  gml_value_free_context_begin(&context);
  context.owned_strings=owned_strings;
  for(size_t i=0;i<count;i++) gml_val_free(&context,values[i]);
  gml_value_free_context_end(&context);
}
void gml_values_release(GmlVal *values,size_t count){ values_release(values,count,0); }
void gml_values_release_owned(GmlVal *values,size_t count){ values_release(values,count,1); }
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
