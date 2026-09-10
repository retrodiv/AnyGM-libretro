/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* VM-owned map, list, grid, and priority-queue adaptation and serialization. */
#include "gml_builtin_internal.h"
#include "anygm_host.h"
#include "anygm_vfs.h"

#include <ctype.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int log_ds_on(GmlVM *vm){
  GmlBuiltinState *state=builtin_state_ensure(vm);
  if(!state) return builtin_setting(vm,"GML_LOG_DS")!=NULL;
  if(state->log_ds<0) state->log_ds=builtin_setting(vm,"GML_LOG_DS")!=NULL;
  return state->log_ds;
}


static char *ds_key_make(GmlVal v){
  char buf[96];
  if(v.t==V_STR){
    const char *s=v.s?v.s:"";
    size_t n=strlen(s);
    char *o=malloc(n+3);
    if(!o) return strdup("s:");
    o[0]='s'; o[1]=':'; memcpy(o+2,s,n+1);
    return o;
  }
  if(v.t==V_UNDEF) return strdup("u:");
  snprintf(buf,sizeof(buf),"r:%.17g",v.t==V_REAL?v.d:0.0);
  return strdup(buf);
}
typedef struct { char buf[160]; char *heap; } DsKeyTemp;
static const char *ds_key_temp(GmlVal v, DsKeyTemp *t){
  if(!t) return NULL;
  t->heap=NULL;
  if(v.t==V_STR){
    const char *s=v.s?v.s:"";
    size_t n=strlen(s);
    if(n+3<=sizeof(t->buf)){
      t->buf[0]='s'; t->buf[1]=':'; memcpy(t->buf+2,s,n+1);
      return t->buf;
    }
    t->heap=malloc(n+3);
    if(!t->heap){
      t->buf[0]='s'; t->buf[1]=':'; t->buf[2]=0;
      return t->buf;
    }
    t->heap[0]='s'; t->heap[1]=':'; memcpy(t->heap+2,s,n+1);
    return t->heap;
  }
  if(v.t==V_UNDEF) return "u:";
  snprintf(t->buf,sizeof(t->buf),"r:%.17g",v.t==V_REAL?v.d:0.0);
  return t->buf;
}
static void ds_key_temp_free(DsKeyTemp *t){
  if(!t) return;
  free(t->heap);
  t->heap=NULL;
}
static char *ds_key_temp_steal(DsKeyTemp *t){
  if(!t) return NULL;
  char *heap=t->heap;
  t->heap=NULL;
  return heap;
}
static GmlVal ds_key_val_clone(GmlVal v){
  /* Own a copy, like ds_val_clone below: a bare reference dangles once str_gc frees the
   * caller's temporary value. */
  if(v.t==V_STR){ char *c=v.s?strdup(v.s):NULL; return c?vstr_owned(c):vstr(""); }
  if(v.t==V_UNDEF) return vundef();
  return vreal(v.t==V_REAL?v.d:0.0);
}
/* free one map entry: ONLY the lookup key. The owned key_val/val strings are deliberately
 * LEAKED: ds_ret hands them out as d=0 references that user code may retain in globals or
 * instance vars long after the entry (or the whole map) is destroyed — freeing them here
 * turns those retained references into use-after-free at the next state save. */
static void ds_entry_free(GmlDSMapEntry *e){
  free(e->key);
}
/* Return a value stored in a ds structure. Strings are handed back as NON-owned references (d=0) so
 * the VM's OP_CALL result tracker does not adopt and free the structure's copy at scope exit. */
static GmlVal ds_ret(GmlVal v){ if(v.t==V_STR) v.d=0; return v; }
static GmlVal ds_val_clone(GmlVal v){
  /* Own an independent copy of stored strings. The source is often a per-run temporary value that
   * str_gc frees at scope exit, so storing the bare pointer makes later reads unsafe. */
  if(v.t==V_STR){ char *c=v.s?strdup(v.s):NULL; return c?vstr_owned(c):vstr(""); }
  if(v.t==V_ARR){ gml_arr_mark_escaped(v); return v; }   /* ds structures outlive the scope */
  if(v.t==V_UNDEF) return vundef();
  return vreal(v.t==V_REAL?v.d:0.0);
}
GmlDSMap *ds_map_slot(GmlVM *vm, int id){
  if(!vm) return NULL;
  int li=vm->builtins->ds_map_last_slot;
  if(li>=0 && li<GML_DS_MAP_MAX && vm->builtins->ds_map[li].live && (int)vm->builtins->ds_map[li].id==id)
    return &vm->builtins->ds_map[li];
  for(int i=0;i<GML_DS_MAP_MAX;i++) if(vm->builtins->ds_map[i].live && (int)vm->builtins->ds_map[i].id==id){
    vm->builtins->ds_map_last_slot=i;
    return &vm->builtins->ds_map[i];
  }
  return NULL;
}
int ds_map_create_id(GmlVM *vm){
  for(int i=0;i<GML_DS_MAP_MAX;i++) if(!vm->builtins->ds_map[i].live){
    vm->builtins->ds_map[i].live=1;
    vm->builtins->ds_map[i].id=(uint32_t)vm->builtins->next_ds_id++;
    vm->builtins->ds_map[i].last_lookup=-1;
    vm->builtins->ds_map_last_slot=i;
    if(vm->builtins->next_ds_id<=0) vm->builtins->next_ds_id=1;
    return (int)vm->builtins->ds_map[i].id;
  }
  return -1;
}
/* ---- ds_list: an ordered list of GmlVal, parallel to ds_map. Identifiers share the same
 * next_ds_id counter so maps and lists never collide. */
GmlDSList *ds_list_slot(GmlVM *vm, int id){
  for(int i=0;i<GML_DS_LIST_MAX;i++) if(vm->builtins->ds_list[i].live && (int)vm->builtins->ds_list[i].id==id) return &vm->builtins->ds_list[i];
  return NULL;
}
GmlDSList *ds_list_slot_repair(GmlVM *vm, int id){
  GmlDSList *l=ds_list_slot(vm,id);
  if(l || !vm->builtins->ds_list_compat_repair || id<=0 || id>=vm->builtins->next_ds_id) return l;
  for(int i=0;i<GML_DS_LIST_MAX;i++) if(!vm->builtins->ds_list[i].live){
    vm->builtins->ds_list[i].live=1;
    vm->builtins->ds_list[i].id=(uint32_t)id;
    vm->builtins->ds_list[i].len=0;
    vm->builtins->ds_list[i].cap=0;
    vm->builtins->ds_list[i].item=NULL;
    vm->builtins->ds_list[i].child_kind=NULL;
    return &vm->builtins->ds_list[i];
  }
  return NULL;
}
int ds_list_create_id(GmlVM *vm){
  for(int i=0;i<GML_DS_LIST_MAX;i++) if(!vm->builtins->ds_list[i].live){
    vm->builtins->ds_list[i].live=1; vm->builtins->ds_list[i].len=0;
    vm->builtins->ds_list[i].id=(uint32_t)vm->builtins->next_ds_id++;
    if(vm->builtins->next_ds_id<=0) vm->builtins->next_ds_id=1;
    return (int)vm->builtins->ds_list[i].id;
  }
  return -1;
}
void ds_list_push_kind(GmlDSList *l, GmlVal v, int kind){
  if(!l) return;
  if(l->len>=l->cap){
    int old=l->cap, nc=l->cap?l->cap*2:8;
    GmlVal *ni=realloc(l->item,(size_t)nc*sizeof(GmlVal)); if(!ni) return;
    l->item=ni;
    unsigned char *nk=realloc(l->child_kind,(size_t)nc); if(!nk) return;
    l->child_kind=nk; memset(nk+old,0,(size_t)(nc-old)); l->cap=nc;
  }
  l->item[l->len]=v;
  l->child_kind[l->len]=(unsigned char)((kind>=1 && kind<=2)?kind:0);
  l->len++;
}
void ds_list_push(GmlDSList *l, GmlVal v){ ds_list_push_kind(l,v,0); }


static GmlVal ds_priority_entry(GmlVal val, double pri){
  GmlVal e=arr_newv(2);
  if(e.t!=V_ARR || !e.arr) return ds_val_clone(val);
  GmlArr *A=(GmlArr*)e.arr;
  A->data[0]=ds_val_clone(val);
  A->data[1]=vreal(pri);
  gml_arr_mark_escaped(e);
  return e;
}
static GmlVal ds_priority_value(GmlVal e){
  if(e.t==V_ARR && e.arr){
    GmlArr *A=(GmlArr*)e.arr;
    if(A->len>0) return ds_ret(A->data[0]);
  }
  return ds_ret(e);
}
static double ds_priority_priority(GmlVal e){
  if(e.t==V_ARR && e.arr){
    GmlArr *A=(GmlArr*)e.arr;
    if(A->len>1) return N(&A->data[1],1,0);
  }
  return 0;
}
static int ds_value_sort_compare(const void *left, const void *right){
  return gml_val_sort_compare(
    *(const GmlVal*)left,*(const GmlVal*)right
  );
}
static int ds_priority_best_index(GmlDSList *l, int want_max){
  if(!l || l->len<=0) return -1;
  int bi=0;
  double bp=ds_priority_priority(l->item[0]);
  for(int i=1;i<l->len;i++){
    double p=ds_priority_priority(l->item[i]);
    if((want_max && p>bp) || (!want_max && p<bp)){ bp=p; bi=i; }
  }
  return bi;
}
static int ds_priority_value_index(GmlDSList *l, GmlVal value){
  if(!l) return -1;
  for(int i=0;i<l->len;i++)
    if(ds_val_equal(ds_priority_value(l->item[i]),value)) return i;
  return -1;
}
static void ds_grid_store(GmlDSGrid *g, int x, int y, GmlVal v){
  if(g && g->cell && x>=0 && y>=0 && x<g->w && y<g->h)
    g->cell[(size_t)y*g->w+x]=ds_val_clone(v);
}
/* ---- ds_grid: two-dimensional GmlVal storage. ---- */
static GmlDSGrid *ds_grid_slot(GmlVM *vm, int id){
  for(int i=0;i<32;i++) if(vm->builtins->ds_grid[i].live && (int)vm->builtins->ds_grid[i].id==id) return &vm->builtins->ds_grid[i];
  return NULL;
}
static int ds_grid_make(GmlVM *vm, int w, int h){
  if(w<0) w=0;
  if(h<0) h=0;
  if((long)w*h>8000000) return -1;
  for(int i=0;i<32;i++) if(!vm->builtins->ds_grid[i].live){
    vm->builtins->ds_grid[i].live=1; vm->builtins->ds_grid[i].w=w; vm->builtins->ds_grid[i].h=h;
    vm->builtins->ds_grid[i].cell=(w&&h)?calloc((size_t)w*h,sizeof(GmlVal)):NULL;
    vm->builtins->ds_grid[i].id=(uint32_t)vm->builtins->next_ds_id++;
    if(vm->builtins->next_ds_id<=0) vm->builtins->next_ds_id=1;
    return (int)vm->builtins->ds_grid[i].id;
  }
  return -1;
}
static int ds_grid_resize_cells(GmlDSGrid *g,int w,int h){
  if(!g || w<0 || h<0 || (long)w*h>8000000) return 0;
  if(w==g->w && h==g->h) return 1;
  GmlVal *cell=NULL;
  if(w>0 && h>0){
    cell=calloc((size_t)w*h,sizeof(*cell));
    if(!cell) return 0;
    int copy_w=w<g->w?w:g->w, copy_h=h<g->h?h:g->h;
    if(g->cell && copy_w>0)
      for(int y=0;y<copy_h;y++)
        memcpy(cell+(size_t)y*w,g->cell+(size_t)y*g->w,(size_t)copy_w*sizeof(*cell));
  }
  free(g->cell);
  g->cell=cell; g->w=w; g->h=h;
  return 1;
}
static int ds_grid_find_value(GmlDSGrid *g, int x1, int y1, int x2, int y2,
                              GmlVal value, int *found_x, int *found_y){
  if(x1>x2){ int t=x1; x1=x2; x2=t; }
  if(y1>y2){ int t=y1; y1=y2; y2=t; }
  if(!g || !g->cell) return 0;
  if(x1<0) x1=0;
  if(y1<0) y1=0;
  if(x2>=g->w) x2=g->w-1;
  if(y2>=g->h) y2=g->h-1;
  for(int y=y1;y<=y2;y++) for(int x=x1;x<=x2;x++){
    if(!ds_val_equal(g->cell[(size_t)y*g->w+x],value)) continue;
    if(found_x) *found_x=x;
    if(found_y) *found_y=y;
    return 1;
  }
  return 0;
}
/* ds_map lookup uses a linear scan below the small-map threshold and a lazy open-addressing index
 * above it, preventing quadratic insertion behavior for maps with many keys. */
static uint32_t ds_key_hash(const char *k){
  uint32_t h=2166136261u; while(*k){ h^=(unsigned char)*k++; h*=16777619u; } return h;
}
static void ds_map_index_free(GmlDSMap *m){ free(m->hidx); m->hidx=NULL; m->hcap=0; m->hdirty=0; }
static void ds_map_index_rebuild(GmlDSMap *m){
  ds_map_index_free(m);
  int hc=64; while(hc < m->len*2) hc*=2;
  m->hidx=malloc((size_t)hc*sizeof(int));
  if(!m->hidx) return;
  for(int i=0;i<hc;i++) m->hidx[i]=-1;
  m->hcap=hc;
  for(int i=0;i<m->len;i++){
    if(!m->entry[i].key) continue;
    uint32_t h=ds_key_hash(m->entry[i].key)&(uint32_t)(hc-1);
    while(m->hidx[h]>=0) h=(h+1)&(uint32_t)(hc-1);
    m->hidx[h]=i;
  }
  m->hdirty=0;
}
static void ds_map_index_add(GmlDSMap *m, int i){
  if(!m->hidx || m->hdirty) return;
  if(m->len*2 > m->hcap){ ds_map_index_rebuild(m); return; }
  uint32_t h=ds_key_hash(m->entry[i].key)&(uint32_t)(m->hcap-1);
  while(m->hidx[h]>=0) h=(h+1)&(uint32_t)(m->hcap-1);
  m->hidx[h]=i;
}
static int ds_map_find_entry(GmlDSMap *m, const char *key){
  if(!m||!key) return -1;
  int li=m->last_lookup;
  if(li>=0 && li<m->len && m->entry[li].key && !strcmp(m->entry[li].key,key)) return li;
  int ni=li+1;
  if(ni>=0 && ni<m->len && m->entry[ni].key && !strcmp(m->entry[ni].key,key)){
    m->last_lookup=ni;
    return ni;
  }
  int pi=li-1;
  if(pi>=0 && pi<m->len && m->entry[pi].key && !strcmp(m->entry[pi].key,key)){
    m->last_lookup=pi;
    return pi;
  }
  if(m->len>=48){
    if(!m->hidx || m->hdirty) ds_map_index_rebuild(m);
    if(m->hidx){
      uint32_t h=ds_key_hash(key)&(uint32_t)(m->hcap-1);
      while(m->hidx[h]>=0){
        int i=m->hidx[h];
        if(i<m->len && m->entry[i].key && !strcmp(m->entry[i].key,key)){
          m->last_lookup=i;
          return i;
        }
        h=(h+1)&(uint32_t)(m->hcap-1);
      }
      return -1;
    }
  }
  for(int i=0;i<m->len;i++) if(m->entry[i].key && !strcmp(m->entry[i].key,key)){
    m->last_lookup=i;
    return i;
  }
  return -1;
}
static int ds_map_reserve(GmlDSMap *m, int n){
  if(n<=m->cap) return 1;
  int nc=m->cap?m->cap:8; while(nc<n) nc*=2;
  GmlDSMapEntry *e=realloc(m->entry,(size_t)nc*sizeof(*e));
  if(!e) return 0;
  memset(e+m->cap,0,(size_t)(nc-m->cap)*sizeof(*e));
  m->entry=e; m->cap=nc; return 1;
}
int ds_map_put(GmlVM *vm, int id, GmlVal keyv, GmlVal val, int overwrite){
  GmlDSMap *m=ds_map_slot(vm,id);
  if(!m) return 0;
  DsKeyTemp kt;
  const char *lookup=ds_key_temp(keyv,&kt);
  int i=ds_map_find_entry(m,lookup);
  if(i>=0){
    if(overwrite){
      m->entry[i].val=ds_val_clone(val);   /* old val leaks: refs may have escaped via ds_ret */
      m->entry[i].child_kind=0;
    }
    ds_key_temp_free(&kt);
    return 1;
  }
  char *key=ds_key_temp_steal(&kt);
  if(!key) key=ds_key_make(keyv);
  ds_key_temp_free(&kt);
  if(!key) return 0;
  if(!ds_map_reserve(m,m->len+1)){ free(key); return 0; }
  m->entry[m->len].key=key;
  m->entry[m->len].key_val=ds_key_val_clone(keyv);
  m->entry[m->len].val=ds_val_clone(val);
  m->entry[m->len].child_kind=0;
  m->len++;
  ds_map_index_add(m,m->len-1);
  return 1;
}
void ds_map_mark_child(GmlVM *vm, int id, GmlVal keyv, int kind){
  GmlDSMap *m=ds_map_slot(vm,id);
  DsKeyTemp kt={0};
  const char *key=ds_key_temp(keyv,&kt);
  int i=ds_map_find_entry(m,key);
  ds_key_temp_free(&kt);
  if(i>=0) m->entry[i].child_kind=(unsigned char)((kind>=1 && kind<=2)?kind:0);
}
GmlVal ds_map_lookup_s(GmlVM *vm, int id, const char *key, int *ok){
  if(ok) *ok=0;
  GmlDSMap *m=ds_map_slot(vm,id);
  DsKeyTemp kt;
  const char *dk=ds_key_temp(vstr(key?key:""),&kt);
  int i=ds_map_find_entry(m,dk);
  ds_key_temp_free(&kt);
  if(i<0) return vundef();
  if(ok) *ok=1;
  return ds_ret(m->entry[i].val);
}
static void ds_map_clear_entries(GmlDSMap *m){
  if(!m) return;
  for(int i=0;i<m->len;i++) ds_entry_free(&m->entry[i]);
  m->len=0;
  m->hdirty=1;
  m->last_lookup=-1;
}
typedef struct {
  unsigned char map[GML_DS_MAP_MAX];
  unsigned char list[GML_DS_LIST_MAX];
} DsDestroyCtx;
static int ds_map_slot_index(GmlVM *vm, GmlDSMap *m){
  return vm && m && m>=vm->builtins->ds_map && m<vm->builtins->ds_map+GML_DS_MAP_MAX ? (int)(m-vm->builtins->ds_map) : -1;
}
static int ds_list_slot_index(GmlVM *vm, GmlDSList *l){
  return vm && l && l>=vm->builtins->ds_list && l<vm->builtins->ds_list+GML_DS_LIST_MAX ? (int)(l-vm->builtins->ds_list) : -1;
}
static int ds_owned_id(GmlVal v, int *id){
  if(v.t!=V_REAL || !isfinite(v.d)) return 0;
  int n=(int)v.d;
  if(fabs(v.d-(double)n)>=1e-9) return 0;
  if(id) *id=n;
  return 1;
}
static void ds_map_destroy_ctx(GmlVM *vm, GmlDSMap *m, DsDestroyCtx *ctx);
static void ds_list_destroy_ctx(GmlVM *vm, GmlDSList *l, DsDestroyCtx *ctx);
static void ds_owned_destroy_ctx(GmlVM *vm, GmlVal v, int kind, DsDestroyCtx *ctx){
  int id;
  if(!ds_owned_id(v,&id)) return;
  if(kind==1) ds_list_destroy_ctx(vm,ds_list_slot(vm,id),ctx);
  else if(kind==2) ds_map_destroy_ctx(vm,ds_map_slot(vm,id),ctx);
}
static void ds_map_destroy_children(GmlVM *vm, GmlDSMap *m, DsDestroyCtx *ctx){
  if(!m) return;
  for(int i=0;i<m->len;i++){
    int kind=m->entry[i].child_kind;
    m->entry[i].child_kind=0;
    if(kind) ds_owned_destroy_ctx(vm,m->entry[i].val,kind,ctx);
  }
}
static void ds_list_destroy_children(GmlVM *vm, GmlDSList *l, DsDestroyCtx *ctx){
  if(!l) return;
  for(int i=0;i<l->len;i++){
    int kind=l->child_kind?l->child_kind[i]:0;
    if(l->child_kind) l->child_kind[i]=0;
    if(kind) ds_owned_destroy_ctx(vm,l->item[i],kind,ctx);
  }
}
static void ds_map_destroy_ctx(GmlVM *vm, GmlDSMap *m, DsDestroyCtx *ctx){
  int slot=ds_map_slot_index(vm,m);
  if(slot<0 || !m->live || ctx->map[slot]) return;
  ctx->map[slot]=1;
  m->live=0;                         /* break self/cross cycles before walking children */
  if(vm->builtins->ds_map_last_slot==slot) vm->builtins->ds_map_last_slot=-1;
  ds_map_destroy_children(vm,m,ctx);
  ds_map_clear_entries(m);
  free(m->entry);
  ds_map_index_free(m);
  memset(m,0,sizeof(*m));
  m->last_lookup=-1;
}
static void ds_list_destroy_ctx(GmlVM *vm, GmlDSList *l, DsDestroyCtx *ctx){
  int slot=ds_list_slot_index(vm,l);
  if(slot<0 || !l->live || ctx->list[slot]) return;
  ctx->list[slot]=1;
  l->live=0;
  ds_list_destroy_children(vm,l,ctx);
  free(l->item);
  free(l->child_kind);
  memset(l,0,sizeof(*l));
}
void ds_map_destroy_id(GmlVM *vm, int id){
  DsDestroyCtx ctx={0};
  ds_map_destroy_ctx(vm,ds_map_slot(vm,id),&ctx);
}
void ds_list_destroy_id(GmlVM *vm, int id){
  DsDestroyCtx ctx={0};
  ds_list_destroy_ctx(vm,ds_list_slot(vm,id),&ctx);
}
static void ds_map_clear_owned(GmlVM *vm, GmlDSMap *m){
  DsDestroyCtx ctx={0}; int slot=ds_map_slot_index(vm,m);
  if(slot>=0) ctx.map[slot]=1;
  ds_map_destroy_children(vm,m,&ctx);
  ds_map_clear_entries(m);
}
static void ds_list_clear_owned(GmlVM *vm, GmlDSList *l){
  DsDestroyCtx ctx={0}; int slot=ds_list_slot_index(vm,l);
  if(slot>=0) ctx.list[slot]=1;
  ds_list_destroy_children(vm,l,&ctx);
  if(l) l->len=0;
}
static int ds_map_replace_from_map(GmlVM *vm, int dst_id, int src_id){
  GmlDSMap *dst=ds_map_slot(vm,dst_id);
  GmlDSMap *src=ds_map_slot(vm,src_id);
  if(!dst||!src) return 0;
  if(dst==src) return 1;
  ds_map_clear_owned(vm,dst);
  for(int i=0;i<src->len;i++){
    ds_map_put(vm,dst_id,src->entry[i].key_val,src->entry[i].val,1);
    ds_map_mark_child(vm,dst_id,src->entry[i].key_val,src->entry[i].child_kind);
    /* This helper is used to consume a temporary decoded map. Transfer nested
     * ownership to the destination before that temporary root is destroyed. */
    src->entry[i].child_kind=0;
  }
  return 1;
}
static void ds_log_val_simple(GmlVM *vm,GmlVal v){
  if(v.t==V_STR) anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"\"%s\"",v.s?v.s:"");
  else if(v.t==V_ARR) anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"<array>");
  else if(v.t==V_UNDEF) anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"undefined");
  else anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"%g",v.t==V_REAL?v.d:0.0);
}
static void ds_log_val(GmlVM *vm, GmlVal v, int depth){
  if(!builtin_setting(vm,"GML_LOG_DS_VERBOSE") || depth>2){
    ds_log_val_simple(vm,v);
    return;
  }
  if(v.t==V_REAL && GML_IS_STRUCT_ID(v.d)){
    GmlInstance *st=gml_struct_find(vm,(unsigned)v.d);
    anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"{struct id=%u", (unsigned)v.d);
    if(st){
      int shown=0;
      for(int i=0;i<st->vars.cap && shown<12;i++){
        GmlVarSlot *slot=&st->vars.slots[i];
        if(!slot->key) continue;
        anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG," %s=",slot->key);
        ds_log_val(vm,slot->val,depth+1);
        shown++;
      }
    }
    anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"}");
    return;
  }
  if(v.t==V_ARR && v.arr){
    GmlArr *A=(GmlArr*)v.arr;
    anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[array len=%d",A->len);
    int max=A->len<6?A->len:6;
    for(int i=0;i<max;i++){
      anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG," %d=",i);
      ds_log_val(vm,A->data[i],depth+1);
    }
    if(A->len>max) anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG," ...");
    anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"]");
    return;
  }
  ds_log_val_simple(vm,v);
}
GmlVal gml_ds_map_find_value_direct(GmlVM *vm, int id, GmlVal keyv, int has_key){
  GmlDSMap *m=ds_map_slot(vm,id);
  DsKeyTemp kt={0};
  const char *key=has_key?ds_key_temp(keyv,&kt):NULL;
  int i=ds_map_find_entry(m,key);
  GmlVal out=(i>=0)?m->entry[i].val:vundef();
  if(log_ds_on(vm)){
    anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[ds_map_find_value] id=%d live=%d key=%s raw=",id,m?m->len:-1,key?key:"<null>");
    if(has_key) ds_log_val(vm,keyv,0); else anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"<missing>");
    anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG," hit=%d out=",i);
    ds_log_val(vm,out,0);
    anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"\n");
  }
  ds_key_temp_free(&kt);
  return ds_ret(out);
}
GmlVal gml_ds_map_find_first_direct(GmlVM *vm, int id){
  GmlDSMap *m=ds_map_slot(vm,id);
  return (m && m->len>0) ? ds_ret(m->entry[0].key_val) : vstr("");
}
GmlVal gml_ds_map_find_next_direct(GmlVM *vm, int id, GmlVal keyv, int has_key){
  GmlDSMap *m=ds_map_slot(vm,id);
  DsKeyTemp kt={0};
  const char *key=has_key?ds_key_temp(keyv,&kt):NULL;
  int i=ds_map_find_entry(m,key);
  ds_key_temp_free(&kt);
  int j=(i<0)?-1:i+1;
  return (m && j>=0 && j<m->len) ? ds_ret(m->entry[j].key_val) : vundef();
}
int gml_ds_map_size_direct(GmlVM *vm, int id){
  GmlDSMap *m=ds_map_slot(vm,id);
  return m?m->len:0;
}
void gml_ds_list_clear_direct(GmlVM *vm, int id){
  GmlDSList *l=ds_list_slot_repair(vm,id);
  if(l) l->len=0;
}
GmlVal gml_ds_map_find_previous_direct(GmlVM *vm, int id,
                                       GmlVal keyv, int has_key){
  GmlDSMap *m=ds_map_slot(vm,id);
  DsKeyTemp kt={0};
  const char *key=has_key?ds_key_temp(keyv,&kt):NULL;
  int i=ds_map_find_entry(m,key);
  ds_key_temp_free(&kt);
  int j=(i<0)?-1:i-1;
  return (m && j>=0 && j<m->len) ? ds_ret(m->entry[j].key_val) : vundef();
}
int gml_ds_map_exists_direct(GmlVM *vm, int id, GmlVal keyv, int has_key){
  GmlDSMap *m=ds_map_slot(vm,id);
  DsKeyTemp kt={0};
  const char *key=has_key?ds_key_temp(keyv,&kt):NULL;
  int index=ds_map_find_entry(m,key);
  ds_key_temp_free(&kt);
  return index>=0;
}
int gml_ds_map_empty_direct(GmlVM *vm, int id){
  GmlDSMap *m=ds_map_slot(vm,id);
  return !m || m->len==0;
}
GmlVal gml_ds_map_find_last_direct(GmlVM *vm, int id){
  GmlDSMap *m=ds_map_slot(vm,id);
  return (m && m->len>0) ? ds_ret(m->entry[m->len-1].key_val) : vstr("");
}
GmlVal gml_ds_list_find_value_direct(GmlVM *vm, int id, int position){
  GmlDSList *l=ds_list_slot_repair(vm,id);
  return (l && position>=0 && position<l->len)
           ? ds_ret(l->item[position]) : vreal(0);
}
int gml_ds_list_size_direct(GmlVM *vm, int id){
  GmlDSList *l=ds_list_slot_repair(vm,id);
  return l?l->len:0;
}
int gml_ds_map_view_count(GmlVM *vm, int id){
  GmlDSMap *map=ds_map_slot(vm,id);
  return map?map->len:-1;
}
int gml_ds_map_item_view(GmlVM *vm, int id, int index,
                         GmlBuiltinMapItemView *item){
  GmlDSMap *map=ds_map_slot(vm,id);
  if(!map || !item || index<0 || index>=map->len) return 0;
  item->key=map->entry[index].key_val;
  item->value=map->entry[index].val;
  item->nested_kind=map->entry[index].child_kind;
  return 1;
}
int gml_ds_list_view_count(GmlVM *vm, int id){
  GmlDSList *list=ds_list_slot(vm,id);
  return list?list->len:-1;
}
int gml_ds_list_item_view(GmlVM *vm, int id, int index,
                          GmlBuiltinListItemView *item){
  GmlDSList *list=ds_list_slot(vm,id);
  if(!list || !item || index<0 || index>=list->len) return 0;
  item->value=list->item[index];
  item->nested_kind=list->child_kind?list->child_kind[index]:0;
  return 1;
}
int gml_ds_list_append_direct(GmlVM *vm, int id, GmlVal value,
                              int nested_kind){
  GmlDSList *list=ds_list_slot(vm,id);
  if(!list) return 0;
  ds_list_push_kind(list,value,nested_kind);
  return 1;
}


static GmlVal ds_map_write_text(GmlVM *vm, int id){
  GmlDSMap *m=ds_map_slot(vm,id);
  GmlBuiltinJsonWriter b={0};
  if(!m || !gml_builtin_json_writer_puts(&b,"{\"__gml_ds_map__\":[")){
    gml_builtin_json_writer_discard(&b);
    return vstr_owned(strdup("{}"));
  }
  for(int i=0;i<m->len;i++){
    if(i && !gml_builtin_json_writer_putc(&b,',')){
      gml_builtin_json_writer_discard(&b);
      return vstr_owned(strdup("{}"));
    }
    if(!gml_builtin_json_writer_putc(&b,'[')){
      gml_builtin_json_writer_discard(&b);
      return vstr_owned(strdup("{}"));
    }
    GmlVal key=m->entry[i].key_val;
    if(key.t==V_STR){
      if(!gml_builtin_json_writer_puts(&b,"\"s\",") ||
         !gml_builtin_json_writer_put_value(vm,&b,key,0,0)){
        gml_builtin_json_writer_discard(&b);
        return vstr_owned(strdup("{}"));
      }
    } else if(key.t==V_UNDEF){
      if(!gml_builtin_json_writer_puts(&b,"\"u\",null")){
        gml_builtin_json_writer_discard(&b);
        return vstr_owned(strdup("{}"));
      }
    } else {
      if(!gml_builtin_json_writer_puts(&b,"\"r\",") ||
         !gml_builtin_json_writer_put_value(
           vm,&b,vreal(key.t==V_REAL?key.d:0.0),0,0)){
        gml_builtin_json_writer_discard(&b);
        return vstr_owned(strdup("{}"));
      }
    }
    if(!gml_builtin_json_writer_putc(&b,',') ||
       !gml_builtin_json_writer_put_value(vm,&b,m->entry[i].val,0,0) ||
       !gml_builtin_json_writer_putc(&b,']')){
      gml_builtin_json_writer_discard(&b);
      return vstr_owned(strdup("{}"));
    }
  }
  if(!gml_builtin_json_writer_puts(&b,"]}")){
    gml_builtin_json_writer_discard(&b);
    return vstr_owned(strdup("{}"));
  }
  return vstr_owned(gml_builtin_json_writer_take(&b,"{}"));
}
static int ds_map_read_text(GmlVM *vm, int dst_id, const char *text){
  GmlVal parsed=gml_builtin_json_decode_ds(vm,text);
  if(parsed.t!=V_REAL) return 0;
  int src_id=(int)parsed.d;
  if(fabs(parsed.d-(double)src_id)>=1e-9) return 0;
  GmlDSMap *src=ds_map_slot(vm,src_id);
  if(!src) return 0;
  int wi=ds_map_find_entry(src,"s:__gml_ds_map__");
  if(wi>=0 && src->entry[wi].child_kind==1 && src->entry[wi].val.t==V_REAL){
    GmlDSList *rows=ds_list_slot(vm,(int)src->entry[wi].val.d);
    GmlDSMap *dst=ds_map_slot(vm,dst_id);
    if(!dst || !rows){ ds_map_destroy_id(vm,src_id); return 0; }
    ds_map_clear_owned(vm,dst);
    for(int i=0;i<rows->len;i++){
      if(!rows->child_kind || rows->child_kind[i]!=1 || rows->item[i].t!=V_REAL) continue;
      GmlDSList *row=ds_list_slot(vm,(int)rows->item[i].d);
      if(!row || row->len<3) continue;
      const char *kind=(row->item[0].t==V_STR && row->item[0].s)?row->item[0].s:"";
      GmlVal key=vundef();
      char formatted[64];
      if(kind[0]=='s'){
        key = row->item[1].t==V_STR ? row->item[1] : vstr(gm_string_format(row->item[1],formatted));
      } else if(kind[0]=='r'){
        key = row->item[1].t==V_REAL ? row->item[1] : vreal(atof(gm_string_format(row->item[1],formatted)));
      } else if(kind[0]!='u') {
        continue;
      }
      ds_map_put(vm,dst_id,key,row->item[2],1);
    }
    ds_map_destroy_id(vm,src_id);
    return 1;
  }
  /* Accept states produced by the older in-core JSON implementation, where the
   * private ds_map_write envelope decoded to VM arrays instead of DS lists. */
  if(wi>=0 && src->entry[wi].val.t==V_ARR && src->entry[wi].val.arr){
    GmlDSMap *dst=ds_map_slot(vm,dst_id);
    if(!dst){ ds_map_destroy_id(vm,src_id); return 0; }
    ds_map_clear_owned(vm,dst);
    GmlArr *rows=(GmlArr*)src->entry[wi].val.arr;
    for(int i=0;i<rows->len;i++){
      if(rows->data[i].t!=V_ARR || !rows->data[i].arr) continue;
      GmlArr *row=(GmlArr*)rows->data[i].arr;
      if(row->len<3) continue;
      const char *kind=(row->data[0].t==V_STR && row->data[0].s)?row->data[0].s:"";
      GmlVal key=vundef();
      char formatted[64];
      if(kind[0]=='s'){
        key=row->data[1].t==V_STR?row->data[1]:vstr(gm_string_format(row->data[1],formatted));
      } else if(kind[0]=='r'){
        key=row->data[1].t==V_REAL?row->data[1]:vreal(atof(gm_string_format(row->data[1],formatted)));
      }
      else if(kind[0]!='u') continue;
      ds_map_put(vm,dst_id,key,row->data[2],1);
    }
    ds_map_destroy_id(vm,src_id);
    return 1;
  }
  int ok=ds_map_replace_from_map(vm,dst_id,src_id);
  ds_map_destroy_id(vm,src_id);
  return ok;
}
static GmlVal ds_list_write_text(GmlVM *vm,int id){
  GmlDSList *list=ds_list_slot(vm,id);
  GmlBuiltinJsonWriter buffer={0};
  if(!list ||
     !gml_builtin_json_writer_puts(&buffer,"{\"__gml_ds_list__\":[")){
    gml_builtin_json_writer_discard(&buffer);
    return vstr_owned(strdup("{}"));
  }
  for(int i=0;i<list->len;i++){
    int kind=list->child_kind?list->child_kind[i]:0;
    char prefix[8];
    snprintf(prefix,sizeof prefix,i?",[%d,":"[%d,",kind);
    int allow=kind==1?2:(kind==2?3:0);
    if(!gml_builtin_json_writer_puts(&buffer,prefix) ||
       !gml_builtin_json_writer_put_value(
         vm,&buffer,list->item[i],0,allow) ||
       !gml_builtin_json_writer_putc(&buffer,']')){
      gml_builtin_json_writer_discard(&buffer);
      return vstr_owned(strdup("{}"));
    }
  }
  if(!gml_builtin_json_writer_puts(&buffer,"]}")){
    gml_builtin_json_writer_discard(&buffer);
    return vstr_owned(strdup("{}"));
  }
  return vstr_owned(gml_builtin_json_writer_take(&buffer,"{}"));
}
static void ds_list_transfer_items(GmlVM *vm,GmlDSList *destination,GmlDSList *source){
  if(!destination || !source || destination==source) return;
  ds_list_clear_owned(vm,destination);
  for(int i=0;i<source->len;i++){
    int kind=source->child_kind?source->child_kind[i]:0;
    ds_list_push_kind(destination,ds_val_clone(source->item[i]),kind);
    /* Ownership of nested DS resources moves to the destination. The temporary JSON tree
     * may still destroy its list shells, but must not recursively destroy a transferred child. */
    if(source->child_kind) source->child_kind[i]=0;
  }
}
static int ds_list_read_text(GmlVM *vm,int dst_id,const char *text){
  GmlDSList *destination=ds_list_slot_repair(vm,dst_id);
  if(!destination) return 0;
  ds_list_clear_owned(vm,destination);
  GmlVal parsed=gml_builtin_json_decode_ds(vm,text);
  if(parsed.t!=V_REAL || !isfinite(parsed.d)) return 0;
  int root_id=(int)parsed.d;
  if(fabs(parsed.d-(double)root_id)>=1e-9) return 0;
  GmlDSMap *root=ds_map_slot(vm,root_id);
  if(!root) return 0;
  int wrapper=ds_map_find_entry(root,"s:__gml_ds_list__");
  if(wrapper>=0 && root->entry[wrapper].child_kind==1 && root->entry[wrapper].val.t==V_REAL){
    GmlDSList *rows=ds_list_slot(vm,(int)root->entry[wrapper].val.d);
    if(rows){
      for(int i=0;i<rows->len;i++){
        if(!rows->child_kind || rows->child_kind[i]!=1 || rows->item[i].t!=V_REAL) continue;
        GmlDSList *row=ds_list_slot(vm,(int)rows->item[i].d);
        if(!row || row->len<2 || row->item[0].t!=V_REAL) continue;
        int kind=(int)row->item[0].d;
        if(kind<0 || kind>2) kind=0;
        int actual=row->child_kind?row->child_kind[1]:0;
        if(kind==1 && (actual!=1 || row->item[1].t!=V_REAL ||
                       !ds_list_slot(vm,(int)row->item[1].d))) kind=0;
        if(kind==2 && (actual!=2 || row->item[1].t!=V_REAL ||
                       !ds_map_slot(vm,(int)row->item[1].d))) kind=0;
        ds_list_push_kind(destination,ds_val_clone(row->item[1]),kind);
        if(kind && row->child_kind) row->child_kind[1]=0;
      }
      ds_map_destroy_id(vm,root_id);
      return 1;
    }
  }
  /* Also accept a plain JSON array stored by early builds. json_decode wraps a non-object root
   * in the map key "default", preserving nested child-kind information on the decoded list. */
  int fallback=ds_map_find_entry(root,"s:default");
  if(fallback>=0 && root->entry[fallback].child_kind==1 && root->entry[fallback].val.t==V_REAL){
    GmlDSList *source=ds_list_slot(vm,(int)root->entry[fallback].val.d);
    int source_exists=source!=NULL;
    ds_list_transfer_items(vm,destination,source);
    root->entry[fallback].child_kind=0;
    ds_map_destroy_id(vm,root_id);
    return source_exists;
  }
  ds_map_destroy_id(vm,root_id);
  return 0;
}



/* Hex-encoded data-structure envelope: a kind marker, header, then typed values.
 * Validate the entire buffer before changing the destination. */
#define GML_DS_NATIVE_LIST_MARKER 301u
#define GML_DS_NATIVE_PRIORITY_MARKER 503u
#define GML_DS_NATIVE_GRID_MARKER 601u
static int ds_native_nibble(int c){
  if(c>='0' && c<='9') return c-'0';
  if(c>='a' && c<='f') return c-'a'+10;
  if(c>='A' && c<='F') return c-'A'+10;
  return -1;
}
static uint8_t *ds_native_decode_hex(const char *text,size_t *out_size){
  if(!text) return NULL;
  size_t len=strlen(text);
  while(len && (text[len-1]=='\r' || text[len-1]=='\n' || text[len-1]==' ')) len--;
  if(len<8 || (len&1u)) return NULL;
  uint8_t *bytes=malloc(len/2);
  if(!bytes) return NULL;
  for(size_t i=0;i<len;i+=2){
    int hi=ds_native_nibble((unsigned char)text[i]);
    int lo=ds_native_nibble((unsigned char)text[i+1]);
    if(hi<0 || lo<0){ free(bytes); return NULL; }
    bytes[i/2]=(uint8_t)((hi<<4)|lo);
  }
  *out_size=len/2;
  return bytes;
}
static void ds_native_put_u32(uint8_t *b,size_t *off,uint32_t v){
  b[*off]=(uint8_t)(v&255u); b[*off+1]=(uint8_t)((v>>8)&255u);
  b[*off+2]=(uint8_t)((v>>16)&255u); b[*off+3]=(uint8_t)((v>>24)&255u);
  *off+=4;
}
static uint32_t ds_native_u32(const uint8_t *b,size_t off){
  return (uint32_t)b[off]|((uint32_t)b[off+1]<<8)|
         ((uint32_t)b[off+2]<<16)|((uint32_t)b[off+3]<<24);
}
/* Advances past one entry without building a value. Answers zero when the entry does not fit,
 * which is what lets the caller reject a buffer whose entries do not add up. */
static int ds_native_skip_value(const uint8_t *b,size_t size,size_t *off){
  if(*off>size || size-*off<4) return 0;
  uint32_t type=ds_native_u32(b,*off);
  if(type==0){
    if(size-*off<12) return 0;
    *off+=12;
    return 1;
  }
  if(type==1){
    if(size-*off<8) return 0;
    uint32_t length=ds_native_u32(b,*off+4);
    if((size_t)length>size-(*off+8)) return 0;
    *off+=8+length;
    return 1;
  }
  return 0;
}
/* Only called after the same walk has been validated, so the entry is known to fit. */
static int ds_native_take_value_checked(const uint8_t *b,size_t *off,GmlVal *out){
  uint32_t type=ds_native_u32(b,*off);
  if(type==1){
    uint32_t length=ds_native_u32(b,*off+4);
    char *text=malloc((size_t)length+1);
    if(!text) return 0;
    memcpy(text,b+*off+8,length);
    text[length]=0;
    *out=vstr_owned(text);
    *off+=8+length;
    return 1;
  }
  uint64_t bits=(uint64_t)ds_native_u32(b,*off+4)|((uint64_t)ds_native_u32(b,*off+8)<<32);
  double value=0.0;
  memcpy(&value,&bits,sizeof value);
  *off+=12;
  *out=vreal(value);
  return 1;
}
static GmlVal ds_native_take_value(const uint8_t *b,size_t *off){
  GmlVal out=vstr("");
  if(!ds_native_take_value_checked(b,off,&out))
    *off+=8+ds_native_u32(b,*off+4);
  return out;
}
/* Encode the layout accepted by the reader below. A real uses a typed double;
 * a string uses a tag, length and bytes. Other types use the numeric fallback because the
 * reader admits only these two value types. */
static GmlVal ds_grid_write_native(GmlVM *vm,int id){
  GmlDSGrid *g=ds_grid_slot(vm,id);
  if(!g || !g->cell || g->w<0 || g->h<0) return vstr_owned(strdup(""));
  size_t total=12;
  for(int x=0;x<g->w;x++) for(int y=0;y<g->h;y++){
    GmlVal v=g->cell[(size_t)y*g->w+x];
    total += (v.t==V_STR) ? 8u+(v.s?strlen(v.s):0u) : 12u;
  }
  uint8_t *bytes=malloc(total?total:1);
  if(!bytes) return vstr_owned(strdup(""));
  size_t off=0;
  ds_native_put_u32(bytes,&off,GML_DS_NATIVE_GRID_MARKER);
  ds_native_put_u32(bytes,&off,(uint32_t)g->w);
  ds_native_put_u32(bytes,&off,(uint32_t)g->h);
  for(int x=0;x<g->w;x++) for(int y=0;y<g->h;y++){
    GmlVal v=g->cell[(size_t)y*g->w+x];
    if(v.t==V_STR){
      const char *text=v.s?v.s:"";
      size_t length=strlen(text);
      ds_native_put_u32(bytes,&off,1u);
      ds_native_put_u32(bytes,&off,(uint32_t)length);
      memcpy(bytes+off,text,length);
      off+=length;
    } else {
      double value=v.t==V_REAL?v.d:0.0;
      ds_native_put_u32(bytes,&off,0u);
      memcpy(bytes+off,&value,sizeof value);
      off+=sizeof value;
    }
  }
  char *hex=malloc(total*2u+1u);
  if(!hex){ free(bytes); return vstr_owned(strdup("")); }
  static const char digits[]="0123456789ABCDEF";
  for(size_t i=0;i<total;i++){
    hex[i*2]=digits[bytes[i]>>4];
    hex[i*2+1]=digits[bytes[i]&15u];
  }
  hex[total*2]=0;
  free(bytes);
  return vstr_owned(hex);
}
/* A grid is written one column at a time: every cell of x=0 top to bottom, then x=1. */
static int ds_grid_read_native(GmlVM *vm,int id,const char *text){
  size_t size=0;
  uint8_t *bytes=ds_native_decode_hex(text,&size);
  if(!bytes) return 0;
  int ok=0;
  if(size>=12 && ds_native_u32(bytes,0)==GML_DS_NATIVE_GRID_MARKER){
    uint32_t w=ds_native_u32(bytes,4), h=ds_native_u32(bytes,8);
    if((double)w*(double)h<=8000000.0){
      size_t off=12; ok=1;
      for(uint32_t i=0;i<(uint32_t)w*h;i++) if(!ds_native_skip_value(bytes,size,&off)){ ok=0; break; }
      if(ok && off!=size) ok=0;
      if(ok){
        GmlDSGrid *g=ds_grid_slot(vm,id);
        if(!g || !ds_grid_resize_cells(g,(int)w,(int)h)) ok=0;
        else {
          off=12;
          for(uint32_t x=0;x<w;x++)
            for(uint32_t y=0;y<h;y++)
              ds_grid_store(g,(int)x,(int)y,ds_native_take_value(bytes,&off));
        }
      }
    }
  }
  free(bytes);
  return ok;
}
static int ds_list_read_native(GmlVM *vm,int id,const char *text){
  size_t size=0;
  uint8_t *bytes=ds_native_decode_hex(text,&size);
  if(!bytes) return 0;
  int ok=0;
  if(size>=8 && ds_native_u32(bytes,0)==GML_DS_NATIVE_LIST_MARKER){
    uint32_t count=ds_native_u32(bytes,4);
    if((double)count*12.0<=(double)size){
      size_t off=8; ok=1;
      for(uint32_t i=0;i<count;i++) if(!ds_native_skip_value(bytes,size,&off)){ ok=0; break; }
      if(ok && off!=size) ok=0;
      if(ok){
        GmlDSList *list=ds_list_slot_repair(vm,id);
        if(!list) ok=0;
        else {
          ds_list_clear_owned(vm,list);
          off=8;
          for(uint32_t i=0;i<count;i++) ds_list_push(list,ds_native_take_value(bytes,&off));
        }
      }
    }
  }
  free(bytes);
  return ok;
}
/* Encode the layout accepted by the reader below. Peek at each entry without
 * cloning it; emit typed-real priorities first and then the values in the reader's
 * two-pass order. */
static GmlVal ds_priority_entry_peek(GmlVal e){
  if(e.t==V_ARR && e.arr){
    GmlArr *A=(GmlArr*)e.arr;
    if(A->len>0) return A->data[0];
  }
  return e;
}
static GmlVal ds_priority_write_native(GmlVM *vm,int id){
  GmlDSList *l=ds_list_slot_repair(vm,id);
  if(!l) return vstr_owned(strdup(""));
  size_t total=8+(size_t)l->len*12u;
  for(int i=0;i<l->len;i++){
    GmlVal v=ds_priority_entry_peek(l->item[i]);
    total += (v.t==V_STR) ? 8u+(v.s?strlen(v.s):0u) : 12u;
  }
  uint8_t *bytes=malloc(total?total:1);
  if(!bytes) return vstr_owned(strdup(""));
  size_t off=0;
  ds_native_put_u32(bytes,&off,GML_DS_NATIVE_PRIORITY_MARKER);
  ds_native_put_u32(bytes,&off,(uint32_t)l->len);
  for(int i=0;i<l->len;i++){
    double p=ds_priority_priority(l->item[i]);
    ds_native_put_u32(bytes,&off,0u);
    memcpy(bytes+off,&p,sizeof p);
    off+=sizeof p;
  }
  for(int i=0;i<l->len;i++){
    GmlVal v=ds_priority_entry_peek(l->item[i]);
    if(v.t==V_STR){
      const char *text=v.s?v.s:"";
      size_t length=strlen(text);
      ds_native_put_u32(bytes,&off,1u);
      ds_native_put_u32(bytes,&off,(uint32_t)length);
      memcpy(bytes+off,text,length);
      off+=length;
    } else {
      double value=v.t==V_REAL?v.d:0.0;
      ds_native_put_u32(bytes,&off,0u);
      memcpy(bytes+off,&value,sizeof value);
      off+=sizeof value;
    }
  }
  char *hex=malloc(total*2u+1u);
  if(!hex){ free(bytes); return vstr_owned(strdup("")); }
  static const char digits[]="0123456789ABCDEF";
  for(size_t i=0;i<total;i++){ hex[i*2]=digits[bytes[i]>>4]; hex[i*2+1]=digits[bytes[i]&15u]; }
  hex[total*2]=0;
  free(bytes);
  return vstr_owned(hex);
}
static int ds_priority_read_native(GmlVM *vm,int id,const char *text){
  size_t size=0;
  uint8_t *bytes=ds_native_decode_hex(text,&size);
  if(!bytes) return 0;
  int ok=0;
  double *priorities=NULL;
  if(size>=8 && ds_native_u32(bytes,0)==GML_DS_NATIVE_PRIORITY_MARKER){
    uint32_t count=ds_native_u32(bytes,4);
    /* Each priority is a typed real (12 bytes), and every value is at least eight bytes. */
    if(count<=8000000u && (size_t)count<=(size-8)/20){
      priorities=count?malloc((size_t)count*sizeof(*priorities)):NULL;
      if(!count || priorities){
        size_t off=8;
        ok=1;
        for(uint32_t i=0;i<count;i++){
          if(off+12>size || ds_native_u32(bytes,off)!=0){ ok=0; break; }
          memcpy(&priorities[i],bytes+off+4,sizeof priorities[i]);
          off+=12;
        }
        size_t values_off=off;
        for(uint32_t i=0;ok && i<count;i++)
          if(!ds_native_skip_value(bytes,size,&off)) ok=0;
        if(ok && off!=size) ok=0;
        if(ok){
          GmlDSList *list=ds_list_slot_repair(vm,id);
          if(!list) ok=0;
          else if((uint32_t)list->cap<count){
            GmlVal *items=realloc(list->item,(size_t)count*sizeof(*items));
            if(!items) ok=0;
            else {
              list->item=items;
              unsigned char *kinds=realloc(list->child_kind,(size_t)count);
              if(!kinds) ok=0;
              else { list->child_kind=kinds; list->cap=(int)count; }
            }
          }
          if(ok){
            ds_list_clear_owned(vm,list);
            off=values_off;
            for(uint32_t i=0;i<count;i++){
              GmlVal value=ds_native_take_value(bytes,&off);
              ds_list_push(list,ds_priority_entry(value,priorities[i]));
              if(value.t==V_STR && value.d!=0) free((void*)value.s);
            }
          }
        }
      }
    }
  }
  free(priorities);
  free(bytes);
  return ok;
}

/* Sequence text is an external value tree, not a VM state snapshot. The reader
 * stages every allocation before replacing the existing list-backed resource. */
#define DS_SEQUENCE_BYTES (64u*1024u*1024u)
#define DS_SEQUENCE_VALUES 1000000u
#define DS_SEQUENCE_DEPTH 64u
typedef struct {
  const uint8_t *bytes;
  size_t size, off;
  unsigned values;
  int rows;
} DsSequenceReader;
static int ds_sequence_word(DsSequenceReader *r,uint32_t *word){
  if(r->off>r->size || r->size-r->off<4) return 0;
  *word=ds_native_u32(r->bytes,r->off); r->off+=4; return 1;
}
static int ds_sequence_value(DsSequenceReader *r,unsigned depth,GmlVal *out);
static int ds_sequence_array(DsSequenceReader *r,uint32_t count,unsigned depth,GmlVal *out){
  if(depth>=DS_SEQUENCE_DEPTH || r->off>r->size || count>(r->size-r->off)/4 ||
     count>DS_SEQUENCE_VALUES-r->values) return 0;
  GmlVal array=gml_arr_new((int)count,vundef());
  if(!array.arr || gml_val_array_length(array)!=(int)count){
    gml_values_release_owned(&array,1); return 0;
  }
  GmlArr *storage=array.arr;
  for(uint32_t i=0;i<count;i++) if(!ds_sequence_value(r,depth+1,&storage->data[i])){
    gml_values_release_owned(&array,1); return 0;
  }
  *out=array; return 1;
}
static int ds_sequence_value(DsSequenceReader *r,unsigned depth,GmlVal *out){
  uint32_t type;
  size_t start=r->off;
  *out=vundef();
  if(depth>=DS_SEQUENCE_DEPTH || r->values>=DS_SEQUENCE_VALUES || !ds_sequence_word(r,&type)) return 0;
  r->values++;
  if(type==0 || type==1){
    size_t end=start;
    if(!ds_native_skip_value(r->bytes,r->size,&end)) return 0;
    if(type==1 && memchr(r->bytes+start+8,0,end-start-8)) return 0;
    r->off=start;
    return ds_native_take_value_checked(r->bytes,&r->off,out);
  }
  if(type==5) return 1;
  if(type==7){
    uint32_t word;
    if(!ds_sequence_word(r,&word)) return 0;
    *out=vreal((double)word-(word>INT32_MAX?4294967296.0:0.0)); return 1;
  }
  if(type==13 || type==10){
    uint32_t low,high;
    if(!ds_sequence_word(r,&low) || !ds_sequence_word(r,&high)) return 0;
    uint64_t bits=(uint64_t)low|((uint64_t)high<<32);
    if(type==13){
      double value; memcpy(&value,&bits,sizeof value); *out=vreal(value!=0.0); return 1;
    }
    /* No typed int64 exists in this value model. Admit only exactly representable
     * integers; never silently round a save-file value or copy a pointer. */
    int negative=(high&0x80000000u)!=0;
    uint64_t magnitude=negative?~bits+1:bits, scan=magnitude;
    unsigned significant=0;
    while(scan){ significant++; scan>>=1; }
    if(significant>53 && (magnitude&((UINT64_C(1)<<(significant-53))-1))) return 0;
    *out=vreal(negative?-(double)magnitude:(double)magnitude); return 1;
  }
  if(type==2){
    uint32_t count;
    if(!ds_sequence_word(r,&count)) return 0;
    if(!r->rows) return ds_sequence_array(r,count,depth,out);
    if(count==1){
      if(!ds_sequence_word(r,&count)) return 0;
      return ds_sequence_array(r,count,depth,out);
    }
    if(depth+1>=DS_SEQUENCE_DEPTH || count>(r->size-r->off)/4 ||
       count>DS_SEQUENCE_VALUES-r->values) return 0;
    GmlVal array=gml_arr_new((int)count,vundef());
    if(!array.arr || gml_val_array_length(array)!=(int)count){
      gml_values_release_owned(&array,1); return 0;
    }
    GmlArr *storage=array.arr;
    for(uint32_t row=0;row<count;row++){
      uint32_t length;
      if(r->values>=DS_SEQUENCE_VALUES || !ds_sequence_word(r,&length)){
        gml_values_release_owned(&array,1); return 0;
      }
      r->values++;
      if(!ds_sequence_array(r,length,depth+1,&storage->data[row])){
        gml_values_release_owned(&array,1); return 0;
      }
    }
    *out=array; return 1;
  }
  return 0;
}
static int ds_sequence_read(GmlVM *vm,GmlDSList *dst,const char *text,int queue){
  if(!text || strnlen(text,(size_t)DS_SEQUENCE_BYTES*2+1)>(size_t)DS_SEQUENCE_BYTES*2) return 0;
  size_t size=0;
  uint8_t *bytes=ds_native_decode_hex(text,&size);
  if(!bytes) return 0;
  DsSequenceReader r={.bytes=bytes,.size=size};
  uint32_t marker,count,first=0,reserved;
  int ok=ds_sequence_word(&r,&marker) &&
    (marker==(queue?203u:103u) || marker==(queue?202u:102u)) && ds_sequence_word(&r,&count);
  if(ok && queue) ok=ds_sequence_word(&r,&first) && ds_sequence_word(&r,&reserved);
  GmlDSList staged={0};
  if(ok) ok=first<=count && count<=DS_SEQUENCE_VALUES && count<=(size-r.off)/4;
  if(ok){
    r.rows=marker==(queue?202u:102u);
    for(uint32_t i=0;ok && i<count;i++){
      GmlVal value=vundef();
      ok=ds_sequence_value(&r,0,&value);
      if(ok && i>=first){
        int previous=staged.len;
        ds_list_push(&staged,value);
        if(staged.len==previous+1) continue;
        ok=0;
      }
      gml_values_release_owned(&value,1);
    }
    if(r.off!=size) ok=0;
  }
  if(ok){
    ds_list_clear_owned(vm,dst);
    free(dst->item); free(dst->child_kind);
    dst->item=staged.item; dst->child_kind=staged.child_kind;
    dst->len=staged.len; dst->cap=staged.cap;
  } else {
    gml_values_release_owned(staged.item,(size_t)staged.len);
    free(staged.item); free(staged.child_kind);
  }
  free(bytes); return ok;
}
static int ds_sequence_space(size_t *off,size_t count){
  if(*off>DS_SEQUENCE_BYTES || count>DS_SEQUENCE_BYTES-*off) return 0;
  *off+=count; return 1;
}
static int ds_sequence_emit(GmlVM *vm,GmlVal value,uint8_t *bytes,size_t *off,
                            const void **parents,unsigned depth,unsigned *values){
  if(depth>=DS_SEQUENCE_DEPTH || *values>=DS_SEQUENCE_VALUES) return 0;
  (*values)++;
  size_t start=*off;
  if(value.t==V_REAL){
    if(isfinite(value.d) && GML_IS_STRUCT_ID(value.d) && gml_struct_find(vm,(unsigned)value.d)) return 0;
    if(!ds_sequence_space(off,12)) return 0;
    if(bytes){
      uint64_t bits; memcpy(&bits,&value.d,sizeof bits);
      ds_native_put_u32(bytes,&start,0);
      ds_native_put_u32(bytes,&start,(uint32_t)bits);
      ds_native_put_u32(bytes,&start,(uint32_t)(bits>>32));
    }
    return 1;
  }
  if(value.t==V_STR){
    const char *text=value.s?value.s:"";
    size_t length=strnlen(text,DS_SEQUENCE_BYTES+1u);
    if(!ds_sequence_space(off,8) || !ds_sequence_space(off,length)) return 0;
    if(bytes){
      ds_native_put_u32(bytes,&start,1); ds_native_put_u32(bytes,&start,(uint32_t)length);
      memcpy(bytes+start,text,length);
    }
    return 1;
  }
  if(value.t==V_UNDEF){
    if(!ds_sequence_space(off,4)) return 0;
    if(bytes) ds_native_put_u32(bytes,&start,5);
    return 1;
  }
  if(value.t!=V_ARR || !value.arr) return 0;
  for(unsigned i=0;i<depth;i++) if(parents[i]==value.arr) return 0;
  parents[depth]=value.arr;
  GmlArr *array=value.arr;
  int count=gml_val_array_length(value);
  if(count<0 || (unsigned)count>DS_SEQUENCE_VALUES-*values || array->len<0 ||
     array->len>array->cap || array->cap>GML_ARR_MAX_CAP || (array->len && !array->data)) return 0;
  if(!ds_sequence_space(off,8)) return 0;
  if(bytes){ ds_native_put_u32(bytes,&start,2); ds_native_put_u32(bytes,&start,(uint32_t)count); }
  if(array->is_2d){
    if(depth+1>=DS_SEQUENCE_DEPTH || count>array->row_cap || (count && !array->row_len)) return 0;
    for(int row=0;row<count;row++){
      int length=gml_val_array_length_2d(value,row);
      if(length<0 || (unsigned)length>DS_SEQUENCE_VALUES-*values || *values>=DS_SEQUENCE_VALUES) return 0;
      (*values)++;
      start=*off;
      if(!ds_sequence_space(off,8)) return 0;
      if(bytes){ ds_native_put_u32(bytes,&start,2); ds_native_put_u32(bytes,&start,(uint32_t)length); }
      parents[depth+1]=NULL;
      for(int column=0;column<length;column++)
        if(!ds_sequence_emit(vm,gml_arr_get_2d(value,row,column),bytes,off,parents,depth+2,values)) return 0;
    }
  } else {
    for(int i=0;i<count;i++)
      if(!ds_sequence_emit(vm,gml_arr_get(value,i),bytes,off,parents,depth+1,values)) return 0;
  }
  return 1;
}
static GmlVal ds_sequence_write(GmlVM *vm,GmlDSList *src,int queue){
  if(src->len<0 || (unsigned)src->len>DS_SEQUENCE_VALUES) return vundef();
  size_t size=queue?16:8;
  unsigned values=0;
  const void *parents[DS_SEQUENCE_DEPTH]={0};
  for(int i=0;i<src->len;i++)
    if(!ds_sequence_emit(vm,src->item[i],NULL,&size,parents,0,&values)) return vundef();
  uint8_t *bytes=malloc(size);
  char *text=malloc(size*2+1);
  if(!bytes || !text){ free(bytes); free(text); return vundef(); }
  size_t off=0;
  ds_native_put_u32(bytes,&off,queue?203:103); ds_native_put_u32(bytes,&off,(uint32_t)src->len);
  if(queue){ ds_native_put_u32(bytes,&off,0); ds_native_put_u32(bytes,&off,(uint32_t)src->len); }
  values=0;
  for(int i=0;i<src->len;i++) if(!ds_sequence_emit(vm,src->item[i],bytes,&off,parents,0,&values)){
    free(bytes); free(text); return vundef();
  }
  static const char digits[]="0123456789ABCDEF";
  for(size_t i=0;i<size;i++){ text[i*2]=digits[bytes[i]>>4]; text[i*2+1]=digits[bytes[i]&15]; }
  text[size*2]=0; free(bytes); return vstr_owned(text);
}

GmlVal gml_builtin_try_ds(GmlVM *vm, const char *nm, GmlVal *a, int n){
  GmlRender *R=(GmlRender*)vm->render;
  (void)R;
  if(!strcmp(nm,"ds_map_create")){
    return vreal((double)ds_map_create_id(vm));
  }
  if(!strcmp(nm,"ds_list_create")) return vreal((double)ds_list_create_id(vm));
  if(!strcmp(nm,"ds_list_destroy")){ ds_list_destroy_id(vm,(int)N(a,n,0)); return vreal(0); }
  if(!strcmp(nm,"ds_list_clear")){ GmlDSList *l=ds_list_slot_repair(vm,(int)N(a,n,0)); ds_list_clear_owned(vm,l); return vreal(0); }
  if(!strcmp(nm,"ds_list_add")){ GmlDSList *l=ds_list_slot_repair(vm,(int)N(a,n,0));
    for(int i=1;i<n;i++) ds_list_push(l,ds_val_clone(a[i]));
    return vreal(0); }
  if(!strcmp(nm,"ds_list_write")) return ds_list_write_text(vm,(int)N(a,n,0));
  if(!strcmp(nm,"ds_list_read")){
    /* Accept the external hex envelope before the runtime's text envelope. */
    if(!ds_list_read_native(vm,(int)N(a,n,0),S(vm,a,n,1)))
      (void)ds_list_read_text(vm,(int)N(a,n,0),S(vm,a,n,1));
    return vreal(0);
  }
  if(!strcmp(nm,"ds_list_size")){ GmlDSList *l=ds_list_slot_repair(vm,(int)N(a,n,0)); return vreal(l?l->len:0); }
  if(!strcmp(nm,"ds_list_empty")){ GmlDSList *l=ds_list_slot_repair(vm,(int)N(a,n,0)); return vreal(!l||l->len==0); }
  if(!strcmp(nm,"ds_list_find_value")){ GmlDSList *l=ds_list_slot_repair(vm,(int)N(a,n,0)); int p=(int)N(a,n,1);
    return (l && p>=0 && p<l->len)? ds_ret(l->item[p]) : vreal(0); }
  if(!strcmp(nm,"ds_list_mark_as_list")||!strcmp(nm,"ds_list_mark_as_map")){
    GmlDSList *l=ds_list_slot_repair(vm,(int)N(a,n,0)); int p=(int)N(a,n,1);
    int kind=!strcmp(nm,"ds_list_mark_as_list")?1:2, child=-1;
    if(l && p>=0 && p<l->len && ds_owned_id(l->item[p],&child) &&
       (kind==1?ds_list_slot(vm,child)!=NULL:ds_map_slot(vm,child)!=NULL)){
      if(l->child_kind) l->child_kind[p]=(unsigned char)kind;
      return vreal(child);
    }
    return vreal(-1);
  }
  if(!strcmp(nm,"ds_list_is_list")||!strcmp(nm,"ds_list_is_map")){
    GmlDSList *l=ds_list_slot_repair(vm,(int)N(a,n,0)); int p=(int)N(a,n,1);
    int kind=!strcmp(nm,"ds_list_is_list")?1:2;
    return vreal(l && l->child_kind && p>=0 && p<l->len && l->child_kind[p]==kind);
  }
  if(!strcmp(nm,"ds_list_find_index")){ GmlDSList *l=ds_list_slot_repair(vm,(int)N(a,n,0));
    if(l && n>=2) for(int i=0;i<l->len;i++){ GmlVal x=l->item[i];
      if(x.t==a[1].t && (x.t==V_STR? (x.s&&a[1].s&&!strcmp(x.s,a[1].s)) : x.d==a[1].d)) return vreal(i); }
    return vreal(-1); }
  if(!strcmp(nm,"ds_list_set")){ GmlDSList *l=ds_list_slot_repair(vm,(int)N(a,n,0)); int p=(int)N(a,n,1);
    if(l && p>=0 && n>=3){ while(l->len<=p) ds_list_push(l,vreal(0)); l->item[p]=ds_val_clone(a[2]); if(l->child_kind) l->child_kind[p]=0; } return vreal(0); }
  if(!strcmp(nm,"ds_list_replace")){ GmlDSList *l=ds_list_slot_repair(vm,(int)N(a,n,0)); int p=(int)N(a,n,1);
    if(l && p>=0 && p<l->len && n>=3){ l->item[p]=ds_val_clone(a[2]); if(l->child_kind) l->child_kind[p]=0; } return vreal(0); }
  if(!strcmp(nm,"ds_list_sort")){ GmlDSList *l=ds_list_slot_repair(vm,(int)N(a,n,0));
    if(l && l->len>1){
      int ascending=n<2||N(a,n,1)!=0;
      if(l->child_kind){
        for(int i=1;i<l->len;i++){
          GmlVal v=l->item[i]; unsigned char k=l->child_kind[i]; int j=i;
          while(j>0 && (ascending ? gml_val_sort_compare(l->item[j-1],v)>0
                                  : gml_val_sort_compare(l->item[j-1],v)<0)){
            l->item[j]=l->item[j-1]; l->child_kind[j]=l->child_kind[j-1]; j--;
          }
          l->item[j]=v; l->child_kind[j]=k;
        }
      } else {
        qsort(l->item,(size_t)l->len,sizeof(GmlVal),ds_value_sort_compare);
        if(!ascending) gml_val_sort_reverse(l->item,l->len);
      }
    }
    return vreal(0); }
  if(!strcmp(nm,"ds_list_shuffle")){ GmlDSList *l=ds_list_slot_repair(vm,(int)N(a,n,0));
    if(l) for(int i=l->len-1;i>0;i--){ int j=(int)floor(gml_rng_value(vm)*(i+1)); if(j<0) j=0; if(j>i) j=i; GmlVal t=l->item[i]; l->item[i]=l->item[j]; l->item[j]=t;
      if(l->child_kind){ unsigned char k=l->child_kind[i]; l->child_kind[i]=l->child_kind[j]; l->child_kind[j]=k; } }
    return vreal(0); }
  if(!strcmp(nm,"ds_list_delete")){ GmlDSList *l=ds_list_slot_repair(vm,(int)N(a,n,0)); int p=(int)N(a,n,1);
    if(l && p>=0 && p<l->len){ memmove(&l->item[p],&l->item[p+1],(size_t)(l->len-p-1)*sizeof(GmlVal));
      if(l->child_kind) memmove(&l->child_kind[p],&l->child_kind[p+1],(size_t)(l->len-p-1));
      l->len--; } return vreal(0); }
  if(!strcmp(nm,"ds_list_insert")){ GmlDSList *l=ds_list_slot_repair(vm,(int)N(a,n,0)); int p=(int)N(a,n,1);
    if(l && p>=0 && p<=l->len && n>=3){ ds_list_push(l,vreal(0));
      memmove(&l->item[p+1],&l->item[p],(size_t)(l->len-1-p)*sizeof(GmlVal));
      if(l->child_kind) memmove(&l->child_kind[p+1],&l->child_kind[p],(size_t)(l->len-1-p));
      l->item[p]=ds_val_clone(a[2]); if(l->child_kind) l->child_kind[p]=0; } return vreal(0); }
  /* Implement FIFO queues and LIFO stacks through GmlDSList storage and shared IDs. */
  if(!strcmp(nm,"ds_queue_create")||!strcmp(nm,"ds_stack_create")) return vreal((double)ds_list_create_id(vm));
  if(!strcmp(nm,"ds_queue_read") || !strcmp(nm,"ds_stack_read") ||
     !strcmp(nm,"ds_queue_write") || !strcmp(nm,"ds_stack_write")){
    int queue=nm[3]=='q', read=strstr(nm,"_read")!=NULL;
    GmlDSList *list=NULL;
    if(n>=1 && a[0].t==V_REAL && isfinite(a[0].d) && a[0].d>=INT_MIN && a[0].d<=INT_MAX)
      list=ds_list_slot(vm,(int)a[0].d);
    if(list && read && n>=2 && a[1].t==V_STR && ds_sequence_read(vm,list,a[1].s,queue)) return vreal(1);
    if(list && !read){
      GmlVal result=ds_sequence_write(vm,list,queue);
      if(result.t==V_STR) return result;
    }
    anygm_host_logf(vm->host,ANYGM_LOG_WARN,"%s: invalid handle or unsupported sequence data",nm);
    return read?vreal(0):vundef();
  }
  if(!strcmp(nm,"ds_queue_copy")||!strcmp(nm,"ds_stack_copy")){
    if(n<2) return vreal(0);
    GmlDSList *dst=ds_list_slot(vm,(int)N(a,n,0));
    GmlDSList *src=ds_list_slot(vm,(int)N(a,n,1));
    if(!dst || !src || dst==src) return vreal(0);
    /* Stage through the existing list storage operation before replacing the
     * destination. A failed allocation must not consume either sequence. */
    GmlDSList staged={0};
    for(int i=0;i<src->len;i++){
      GmlVal value=ds_val_clone(src->item[i]);
      if(src->item[i].t==V_STR && src->item[i].s && src->item[i].s[0] &&
         (!value.s || !value.s[0])) break;
      ds_list_push(&staged,value);
      if(staged.len!=i+1){
        if(value.t==V_STR && value.d!=0) free((void *)value.s);
        break;
      }
    }
    if(staged.len==src->len){
      ds_list_clear_owned(vm,dst);
      free(dst->item);
      free(dst->child_kind);
      dst->item=staged.item;
      dst->child_kind=staged.child_kind;
      dst->len=staged.len;
      dst->cap=staged.cap;
    } else {
      for(int i=0;i<staged.len;i++)
        if(staged.item[i].t==V_STR && staged.item[i].d!=0)
          free((void *)staged.item[i].s);
      free(staged.item);
      free(staged.child_kind);
    }
    return vreal(0);
  }
  if(!strcmp(nm,"ds_queue_destroy")||!strcmp(nm,"ds_stack_destroy")){ ds_list_destroy_id(vm,(int)N(a,n,0)); return vreal(0); }
  if(!strcmp(nm,"ds_queue_clear")||!strcmp(nm,"ds_stack_clear")){ GmlDSList *l=ds_list_slot_repair(vm,(int)N(a,n,0)); ds_list_clear_owned(vm,l); return vreal(0); }
  if(!strcmp(nm,"ds_queue_size")||!strcmp(nm,"ds_stack_size")){ GmlDSList *l=ds_list_slot_repair(vm,(int)N(a,n,0)); return vreal(l?l->len:0); }
  if(!strcmp(nm,"ds_queue_empty")||!strcmp(nm,"ds_stack_empty")){ GmlDSList *l=ds_list_slot_repair(vm,(int)N(a,n,0)); return vreal(!l||l->len==0); }
  if(!strcmp(nm,"ds_queue_enqueue")||!strcmp(nm,"ds_stack_push")){ GmlDSList *l=ds_list_slot_repair(vm,(int)N(a,n,0));
    for(int i=1;i<n;i++) ds_list_push(l,ds_val_clone(a[i]));
    return vreal(0); }
  if(!strcmp(nm,"ds_queue_head")){ GmlDSList *l=ds_list_slot_repair(vm,(int)N(a,n,0)); return (l&&l->len>0)?ds_ret(l->item[0]):vreal(0); }
  if(!strcmp(nm,"ds_queue_tail")){ GmlDSList *l=ds_list_slot_repair(vm,(int)N(a,n,0)); return (l&&l->len>0)?ds_ret(l->item[l->len-1]):vreal(0); }
  if(!strcmp(nm,"ds_stack_top")){ GmlDSList *l=ds_list_slot_repair(vm,(int)N(a,n,0)); return (l&&l->len>0)?ds_ret(l->item[l->len-1]):vreal(0); }
  if(!strcmp(nm,"ds_queue_dequeue")){ GmlDSList *l=ds_list_slot_repair(vm,(int)N(a,n,0));
    if(!l||l->len==0) return vreal(0);
    GmlVal v=l->item[0]; if(v.t==V_STR) v.d=0;
    memmove(&l->item[0],&l->item[1],(size_t)(l->len-1)*sizeof(GmlVal));
    if(l->child_kind) memmove(&l->child_kind[0],&l->child_kind[1],(size_t)(l->len-1));
    l->len--; return v; }
  if(!strcmp(nm,"ds_stack_pop")){ GmlDSList *l=ds_list_slot_repair(vm,(int)N(a,n,0));
    if(!l||l->len==0) return vreal(0);
    GmlVal v=l->item[--l->len]; if(v.t==V_STR) v.d=0; return v; }
  if(!strcmp(nm,"ds_priority_create")) return vreal((double)ds_list_create_id(vm));
  if(!strcmp(nm,"ds_priority_destroy")){ ds_list_destroy_id(vm,(int)N(a,n,0)); return vreal(0); }
  if(!strcmp(nm,"ds_priority_clear")){ GmlDSList *l=ds_list_slot_repair(vm,(int)N(a,n,0));
    ds_list_clear_owned(vm,l);
    return vreal(0); }
  if(!strcmp(nm,"ds_priority_size")){ GmlDSList *l=ds_list_slot_repair(vm,(int)N(a,n,0)); return vreal(l?l->len:0); }
  if(!strcmp(nm,"ds_priority_empty")){ GmlDSList *l=ds_list_slot_repair(vm,(int)N(a,n,0)); return vreal(!l||l->len==0); }
  if(!strcmp(nm,"ds_priority_add")){ GmlDSList *l=ds_list_slot_repair(vm,(int)N(a,n,0));
    if(l && n>=3) ds_list_push(l,ds_priority_entry(a[1],N(a,n,2)));
    return vreal(0); }
  if(!strcmp(nm,"ds_priority_write")) return ds_priority_write_native(vm,(int)N(a,n,0));
  if(!strcmp(nm,"ds_priority_read")){
    (void)ds_priority_read_native(vm,(int)N(a,n,0),S(vm,a,n,1));
    return vreal(0); }
  if(!strcmp(nm,"ds_priority_copy")){
    GmlDSList *dst=ds_list_slot_repair(vm,(int)N(a,n,0));
    GmlDSList *src=ds_list_slot_repair(vm,(int)N(a,n,1));
    if(dst && src && dst!=src){
      dst->len=0;
      for(int i=0;i<src->len;i++)
        ds_list_push(dst,ds_priority_entry(ds_priority_value(src->item[i]),
                                           ds_priority_priority(src->item[i])));
    }
    return vreal(0);
  }
  if(!strcmp(nm,"ds_priority_find_min")||!strcmp(nm,"ds_priority_find_max")){
    GmlDSList *l=ds_list_slot_repair(vm,(int)N(a,n,0));
    int bi=ds_priority_best_index(l,!strcmp(nm,"ds_priority_find_max"));
    return bi>=0?ds_priority_value(l->item[bi]):vreal(0); }
  if(!strcmp(nm,"ds_priority_find_priority")){
    GmlDSList *l=ds_list_slot_repair(vm,(int)N(a,n,0));
    int bi=n>=2?ds_priority_value_index(l,a[1]):-1;
    return bi>=0?vreal(ds_priority_priority(l->item[bi])):vundef(); }
  if(!strcmp(nm,"ds_priority_change_priority")){
    GmlDSList *l=ds_list_slot_repair(vm,(int)N(a,n,0));
    int bi=n>=3?ds_priority_value_index(l,a[1]):-1;
    if(bi>=0 && l->item[bi].t==V_ARR && l->item[bi].arr){
      GmlArr *entry=(GmlArr*)l->item[bi].arr;
      if(entry->len>1) entry->data[1]=vreal(N(a,n,2));
    }
    return vreal(0); }
  if(!strcmp(nm,"ds_priority_delete_value")){
    GmlDSList *l=ds_list_slot_repair(vm,(int)N(a,n,0));
    int bi=n>=2?ds_priority_value_index(l,a[1]):-1;
    if(bi>=0){
      memmove(&l->item[bi],&l->item[bi+1],(size_t)(l->len-bi-1)*sizeof(GmlVal));
      if(l->child_kind) memmove(&l->child_kind[bi],&l->child_kind[bi+1],(size_t)(l->len-bi-1));
      l->len--;
    }
    return vreal(0); }
  if(!strcmp(nm,"ds_priority_delete_min")||!strcmp(nm,"ds_priority_delete_max")){
    GmlDSList *l=ds_list_slot_repair(vm,(int)N(a,n,0));
    int bi=ds_priority_best_index(l,!strcmp(nm,"ds_priority_delete_max"));
    if(bi<0) return vreal(0);
    GmlVal v=ds_priority_value(l->item[bi]);
    memmove(&l->item[bi],&l->item[bi+1],(size_t)(l->len-bi-1)*sizeof(GmlVal));
    if(l->child_kind) memmove(&l->child_kind[bi],&l->child_kind[bi+1],(size_t)(l->len-bi-1));
    l->len--;
    return v; }
  if(!strcmp(nm,"ds_exists")){ /* ds_exists(id, ds_type): type 0=map,1=list,4=grid — we track by id */
    int id=(int)N(a,n,0); return vreal(ds_list_slot_repair(vm,id)!=NULL || ds_map_slot(vm,id)!=NULL || ds_grid_slot(vm,id)!=NULL); }
  if(!strcmp(nm,"ds_grid_create")) return vreal((double)ds_grid_make(vm,(int)N(a,n,0),(int)N(a,n,1)));
  if(!strcmp(nm,"ds_grid_destroy")){ GmlDSGrid *g=ds_grid_slot(vm,(int)N(a,n,0));
    if(g){ free(g->cell); memset(g,0,sizeof(*g)); } return vreal(0); }
  if(!strcmp(nm,"ds_grid_resize")){ GmlDSGrid *g=ds_grid_slot(vm,(int)N(a,n,0));
    return vreal(ds_grid_resize_cells(g,(int)N(a,n,1),(int)N(a,n,2))); }
  if(!strcmp(nm,"ds_grid_width")){ GmlDSGrid *g=ds_grid_slot(vm,(int)N(a,n,0)); return vreal(g?g->w:0); }
  if(!strcmp(nm,"ds_grid_height")){ GmlDSGrid *g=ds_grid_slot(vm,(int)N(a,n,0)); return vreal(g?g->h:0); }
  if(!strcmp(nm,"ds_grid_get")){ GmlDSGrid *g=ds_grid_slot(vm,(int)N(a,n,0)); int x=(int)N(a,n,1),y=(int)N(a,n,2);
    return (g && g->cell && x>=0 && y>=0 && x<g->w && y<g->h)? ds_ret(g->cell[(size_t)y*g->w+x]) : vreal(0); }
  if(!strcmp(nm,"ds_grid_set")||!strcmp(nm,"ds_grid_set_post")){ GmlDSGrid *g=ds_grid_slot(vm,(int)N(a,n,0)); int x=(int)N(a,n,1),y=(int)N(a,n,2);
    if(n>=4) ds_grid_store(g,x,y,a[3]);
    return n>=4?a[3]:vreal(0); }
  if(!strcmp(nm,"ds_grid_write")) return ds_grid_write_native(vm,(int)N(a,n,0));
  if(!strcmp(nm,"ds_grid_read")){
    return vreal(ds_grid_read_native(vm,(int)N(a,n,0),S(vm,a,n,1)));
  }
  if(!strcmp(nm,"ds_grid_copy")){ GmlDSGrid *d=ds_grid_slot(vm,(int)N(a,n,0)), *s=ds_grid_slot(vm,(int)N(a,n,1));
    if(!d || !s || d==s) return vreal(0);
    if(!ds_grid_resize_cells(d,s->w,s->h)) return vreal(0);
    for(int y=0;y<s->h;y++)
      for(int x=0;x<s->w;x++)
        ds_grid_store(d,x,y,s->cell[(size_t)y*s->w+x]);
    return vreal(1); }
  if(!strcmp(nm,"ds_grid_add")){ GmlDSGrid *g=ds_grid_slot(vm,(int)N(a,n,0)); int x=(int)N(a,n,1),y=(int)N(a,n,2);
    if(g && g->cell && x>=0 && y>=0 && x<g->w && y<g->h && n>=4){
      GmlVal old=g->cell[(size_t)y*g->w+x];
      g->cell[(size_t)y*g->w+x]=ds_val_clone(vreal(N(&old,1,0)+N(a,n,3)));
    }
    return vreal(0); }
  if(!strcmp(nm,"ds_grid_value_exists")||!strcmp(nm,"ds_grid_value_x")||!strcmp(nm,"ds_grid_value_y")){
    GmlDSGrid *g=ds_grid_slot(vm,(int)N(a,n,0)); int x1=(int)N(a,n,1),y1=(int)N(a,n,2),x2=(int)N(a,n,3),y2=(int)N(a,n,4);
    int found_x=-1,found_y=-1;
    int found=ds_grid_find_value(g,x1,y1,x2,y2,n>=6?a[5]:vreal(0),&found_x,&found_y);
    if(!strcmp(nm,"ds_grid_value_exists")) return vreal(found);
    return vreal(found?(!strcmp(nm,"ds_grid_value_x")?found_x:found_y):-1); }
  /* Region aggregates share one walk. Rectangles clamp to the grid and accept
   * reversed bounds; disks visit only cells within the requested radius.
   * Empty regions return zero and mean uses the number of visited cells. */
  /* Keep all eight names literal so the registry inventory can discover them. */
  if(!strcmp(nm,"ds_grid_get_max")||!strcmp(nm,"ds_grid_get_min")||
     !strcmp(nm,"ds_grid_get_sum")||!strcmp(nm,"ds_grid_get_mean")||
     !strcmp(nm,"ds_grid_get_disk_max")||!strcmp(nm,"ds_grid_get_disk_min")||
     !strcmp(nm,"ds_grid_get_disk_sum")||!strcmp(nm,"ds_grid_get_disk_mean")){
    GmlDSGrid *g=ds_grid_slot(vm,(int)N(a,n,0));
    int disk=strstr(nm,"disk")!=NULL;
    int x1,y1,x2,y2;
    double cx=0,cy=0,radius=0;
    if(disk){
      cx=N(a,n,1); cy=N(a,n,2); radius=N(a,n,3);
      x1=(int)(cx-radius); y1=(int)(cy-radius);
      x2=(int)(cx+radius); y2=(int)(cy+radius);
    }else{
      x1=(int)N(a,n,1); y1=(int)N(a,n,2); x2=(int)N(a,n,3); y2=(int)N(a,n,4);
    }
    if(x1>x2){ int t=x1; x1=x2; x2=t; }
    if(y1>y2){ int t=y1; y1=y2; y2=t; }
    if(!g||!g->cell) return vreal(0);
    if(x1<0) x1=0;
    if(y1<0) y1=0;
    if(x2>=g->w) x2=g->w-1;
    if(y2>=g->h) y2=g->h-1;
    double total=0, best=0;
    int seen=0;
    for(int y=y1;y<=y2;y++) for(int x=x1;x<=x2;x++){
      if(disk){
        double dx=(double)x-cx, dy=(double)y-cy;
        if(dx*dx+dy*dy > radius*radius) continue;
      }
      GmlVal cell=g->cell[(size_t)y*(size_t)g->w+(size_t)x];
      double value=N(&cell,1,0);
      total+=value;
      if(!seen || (strstr(nm,"max") ? value>best : value<best)) best=value;
      seen++;
    }
    if(!seen) return vreal(0);
    if(strstr(nm,"sum")) return vreal(total);
    if(strstr(nm,"mean")) return vreal(total/(double)seen);
    return vreal(best);
  }
  /* Region and disk mutators share a rectangle-or-circle walk. Each visited
   * cell is replaced, added to or multiplied; grid-region forms read a second
   * grid's corresponding cell rather than using a constant. */
  if(!strcmp(nm,"ds_grid_set_region")||!strcmp(nm,"ds_grid_add_region")||
     !strcmp(nm,"ds_grid_multiply_region")||!strcmp(nm,"ds_grid_set_disk")||
     !strcmp(nm,"ds_grid_add_disk")||!strcmp(nm,"ds_grid_multiply_disk")||
     !strcmp(nm,"ds_grid_set_grid_region")||!strcmp(nm,"ds_grid_add_grid_region")||
     !strcmp(nm,"ds_grid_multiply_grid_region")){
    GmlDSGrid *g=ds_grid_slot(vm,(int)N(a,n,0));
    int disk=strstr(nm,"disk")!=NULL, from_grid=strstr(nm,"grid_region")!=NULL;
    int add=strstr(nm,"_add_")!=NULL, mul=strstr(nm,"_multiply_")!=NULL;
    if(!g||!g->cell) return vreal(0);
    int x1,y1,x2,y2, sx=0, sy=0;
    double cx=0,cy=0,radius=0;
    GmlDSGrid *src=NULL;
    GmlVal value=vreal(0);
    if(disk){
      cx=N(a,n,1); cy=N(a,n,2); radius=N(a,n,3);
      x1=(int)(cx-radius); y1=(int)(cy-radius); x2=(int)(cx+radius); y2=(int)(cy+radius);
      value=n>4?a[4]:vreal(0);
    }else if(from_grid){
      /* The source bounds select cells; the destination corner sets their placement. */
      src=ds_grid_slot(vm,(int)N(a,n,1));
      sx=(int)N(a,n,2); sy=(int)N(a,n,3);
      int sx2=(int)N(a,n,4), sy2=(int)N(a,n,5);
      if(sx>sx2){ int t=sx; sx=sx2; sx2=t; }
      if(sy>sy2){ int t=sy; sy=sy2; sy2=t; }
      x1=(int)N(a,n,6); y1=(int)N(a,n,7);
      x2=x1+(sx2-sx); y2=y1+(sy2-sy);
      if(!src||!src->cell) return vreal(0);
    }else{
      x1=(int)N(a,n,1); y1=(int)N(a,n,2); x2=(int)N(a,n,3); y2=(int)N(a,n,4);
      value=n>5?a[5]:vreal(0);
    }
    if(x1>x2){ int t=x1; x1=x2; x2=t; }
    if(y1>y2){ int t=y1; y1=y2; y2=t; }
    for(int y=y1;y<=y2;y++) for(int x=x1;x<=x2;x++){
      if(x<0||y<0||x>=g->w||y>=g->h) continue;
      if(disk){
        double dx=(double)x-cx, dy=(double)y-cy;
        if(dx*dx+dy*dy > radius*radius) continue;
      }
      GmlVal incoming=value;
      if(from_grid){
        int ux=sx+(x-x1), uy=sy+(y-y1);
        if(ux<0||uy<0||ux>=src->w||uy>=src->h) continue;
        incoming=src->cell[(size_t)uy*(size_t)src->w+(size_t)ux];
      }
      GmlVal *cell=&g->cell[(size_t)y*(size_t)g->w+(size_t)x];
      if(add||mul){
        double had=N(cell,1,0), got=N(&incoming,1,0);
        *cell=ds_val_clone(vreal(add?had+got:had*got));
      }else{
        *cell=ds_val_clone(incoming);
      }
    }
    return vreal(0);
  }
  if(!strcmp(nm,"ds_grid_sort")){ GmlDSGrid *g=ds_grid_slot(vm,(int)N(a,n,0));
    int column=(int)N(a,n,1), ascending=n<3||N(a,n,2)!=0;
    /* Sort whole rows by the named key column. Insertion sort preserves the
     * relative order of rows with equal keys, as ds_list_sort does. */
    if(g && g->cell && g->h>1 && column>=0 && column<g->w){
      GmlVal *row=(GmlVal*)malloc((size_t)g->w*sizeof(*row));
      if(row){
        size_t stride=(size_t)g->w*sizeof(*row);
        for(int y=1;y<g->h;y++){
          memcpy(row,g->cell+(size_t)y*g->w,stride);
          int j=y;
          while(j>0){
            int order=gml_val_sort_compare(g->cell[(size_t)(j-1)*g->w+column],row[column]);
            if(ascending?order<=0:order>=0) break;
            memcpy(g->cell+(size_t)j*g->w,g->cell+(size_t)(j-1)*g->w,stride);
            j--;
          }
          memcpy(g->cell+(size_t)j*g->w,row,stride);
        }
        free(row);
      }
    }
    return vreal(0); }
  if(!strcmp(nm,"ds_grid_clear")){ GmlDSGrid *g=ds_grid_slot(vm,(int)N(a,n,0)); GmlVal v=n>=2?a[1]:vreal(0);
    if(g && g->cell) for(size_t i=0;i<(size_t)g->w*g->h;i++) g->cell[i]=v;
    return vreal(0); }
  if(!strcmp(nm,"load_csv")){
    /* Read a CSV file into a ds_grid (GM semantics: each cell a string; numbers stay strings). Rows are
     * lines, columns are comma-separated; the grid is width=max columns, height=row count. */
    char *fn=resolve_read_path(vm,S(vm,a,n,0)); uint8_t *data=NULL; size_t size=0;
    int loaded=fn && anygm_vfs_read_all(vm->host,fn,&data,&size,256u*1024u*1024u); free(fn);
    if(!loaded) return vreal(ds_grid_make(vm,0,0));
    /* first pass: rows and max columns */
    int rows=0, maxcol=0, col=1, inq=0, c;
    size_t position=0;
    for(;;){ c=position<size?data[position++]:EOF; if(c==EOF){ if(col>0||rows>0){ if(col>maxcol)maxcol=col; rows++; } break; }
      if(c=='"'){ inq=!inq; }
      else if(c==','&&!inq){ col++; }
      else if((c=='\n')&&!inq){ if(col>maxcol)maxcol=col; rows++; col=1; } else if(c=='\r'){} }
    if(rows<=0||maxcol<=0){ free(data); return vreal(ds_grid_make(vm,0,0)); }
    int gid=ds_grid_make(vm,maxcol,rows); GmlDSGrid *g=ds_grid_slot(vm,gid);
    if(!g||!g->cell){ free(data); return vreal(gid); }
    position=0;
    /* second pass: fill cells */
    char buf[1024]; int bl=0, cx=0, cy=0; inq=0;
    #define CSV_EMIT() do{ buf[bl]=0; if(cx<g->w && cy<g->h){ char *s=strdup(buf); if(s) g->cell[(size_t)cy*g->w+cx]=vstr_owned(s); } bl=0; }while(0)
    for(;;){ c=position<size?data[position++]:EOF;
      if(c==EOF){ CSV_EMIT(); break; }
      if(c=='"'){ inq=!inq; }
      else if(c==','&&!inq){ CSV_EMIT(); cx++; }
      else if(c=='\n'&&!inq){ CSV_EMIT(); cx=0; cy++; }
      else if(c=='\r'){}
      else if(bl<(int)sizeof(buf)-1){ buf[bl++]=(char)c; } }
    #undef CSV_EMIT
    free(data);
    return vreal(gid);
  }
  if(!strcmp(nm,"ds_map_add")){
    if(n>=3) ds_map_put(vm,(int)N(a,n,0),a[1],a[2],0);
    return vreal(0);
  }
  if(!strcmp(nm,"ds_map_write")){
    return ds_map_write_text(vm,(int)N(a,n,0));
  }
  if(!strcmp(nm,"ds_map_read")){
    return vreal(ds_map_read_text(vm,(int)N(a,n,0),S(vm,a,n,1)));
  }
  /* secure (encrypted) map save/load — no persistence yet: report success on save, hand back a fresh
   * EMPTY map on load (a valid id the game can query, i.e. "no saved data" rather than a bogus 0). */
  if(!strcmp(nm,"ds_map_secure_save")||!strcmp(nm,"ds_map_secure_save_buffer")) return vreal(1);
  if(!strcmp(nm,"ds_map_secure_load")||!strcmp(nm,"ds_map_secure_load_buffer")) return vreal((double)ds_map_create_id(vm));
  if(!strcmp(nm,"ds_map_exists")){ GmlDSMap *m=ds_map_slot(vm,(int)N(a,n,0));
    DsKeyTemp kt={0}; const char *key=(n>=2)?ds_key_temp(a[1],&kt):NULL;
    int i=ds_map_find_entry(m,key); ds_key_temp_free(&kt); return vreal(i>=0); }
  if(!strcmp(nm,"ds_map_size")){ GmlDSMap *m=ds_map_slot(vm,(int)N(a,n,0)); return vreal(m?m->len:0); }
  if(!strcmp(nm,"ds_map_empty")){ GmlDSMap *m=ds_map_slot(vm,(int)N(a,n,0)); return vreal(!m||m->len==0); }
  if(!strcmp(nm,"ds_map_is_list")||!strcmp(nm,"ds_map_is_map")){
    GmlDSMap *m=ds_map_slot(vm,(int)N(a,n,0));
    DsKeyTemp kt={0}; const char *key=(n>=2)?ds_key_temp(a[1],&kt):NULL;
    int i=ds_map_find_entry(m,key), kind=!strcmp(nm,"ds_map_is_list")?1:2;
    ds_key_temp_free(&kt);
    return vreal(i>=0 && m->entry[i].child_kind==kind);
  }
  if(!strcmp(nm,"ds_map_set_pre")||!strcmp(nm,"ds_map_set_post")){
    if(n<3) return vundef();
    GmlVal result=a[2];
    double index=N(a,n,0);
    if(!isfinite(index) || index<0 || index>INT_MAX) return result;
    int id=(int)index;
    GmlDSMap *map=ds_map_slot(vm,id);
    if(!map) return result;
    if(!strcmp(nm,"ds_map_set_post")){
      DsKeyTemp key={0};
      int entry=ds_map_find_entry(map,ds_key_temp(a[1],&key));
      result=entry>=0?ds_ret(map->entry[entry].val):vundef();
      ds_key_temp_free(&key);
    }
    /* Preserve the ordinary map storage and escaped-value lifetime rules. */
    ds_map_put(vm,id,a[1],a[2],1);
    return result;
  }
  if(!strcmp(nm,"ds_map_set")||!strcmp(nm,"ds_map_replace")){
    if(n>=3) ds_map_put(vm,(int)N(a,n,0),a[1],a[2],1);
    return n>=3 ? a[2] : vreal(0);
  }
  if(!strcmp(nm,"ds_map_delete")){
    GmlDSMap *m=ds_map_slot(vm,(int)N(a,n,0));
    DsKeyTemp kt={0};
    const char *key=(n>=2)?ds_key_temp(a[1],&kt):NULL;
    int i=ds_map_find_entry(m,key);
    if(i>=0){
      ds_entry_free(&m->entry[i]);
      memmove(&m->entry[i],&m->entry[i+1],(size_t)(m->len-i-1)*sizeof(m->entry[0]));
      m->len--;
      m->hdirty=1;   /* indices shifted: rebuild the hash index on next lookup */
      m->last_lookup=-1;
    }
    ds_key_temp_free(&kt);
    return vreal(0);
  }
  if(!strcmp(nm,"ds_map_destroy")){
    ds_map_destroy_id(vm,(int)N(a,n,0));
    return vreal(0);
  }
  if(!strcmp(nm,"ds_map_clear")){
    GmlDSMap *m=ds_map_slot(vm,(int)N(a,n,0));
    ds_map_clear_owned(vm,m);
    return vreal(0);
  }
  if(!strcmp(nm,"ds_map_copy")){
    int dst_id=(int)N(a,n,0), src_id=(int)N(a,n,1);
    GmlDSMap *dst=ds_map_slot(vm,dst_id), *src=ds_map_slot(vm,src_id);
    if(dst && src && dst!=src){
      /* Snapshot before clearing so destination storage can be rebuilt independently while map
       * values retain their normal reference semantics. */
      int count=src->len;
      GmlVal *keys=count?malloc((size_t)count*sizeof(*keys)):NULL;
      GmlVal *values=count?malloc((size_t)count*sizeof(*values)):NULL;
      if(!count || (keys && values)){
        for(int i=0;i<count;i++){
          keys[i]=ds_key_val_clone(src->entry[i].key_val);
          values[i]=ds_val_clone(src->entry[i].val);
        }
        ds_map_clear_owned(vm,dst);
        for(int i=0;i<count;i++) ds_map_put(vm,dst_id,keys[i],values[i],1);
      }
      free(keys); free(values);
    }
    return vreal(0);
  }
  if(!strcmp(nm,"ds_list_copy")){          /* ds_list_copy(dest, src): dest := copy of src */
    GmlDSList *dst=ds_list_slot_repair(vm,(int)N(a,n,0)), *src=ds_list_slot_repair(vm,(int)N(a,n,1));
    if(dst && src){
      dst->len=0;
      for(int i=0;i<src->len;i++) ds_list_push_kind(dst, ds_val_clone(src->item[i]),
                                                     src->child_kind?src->child_kind[i]:0);
    }
    return vreal(0);
  }
  if(!strcmp(nm,"ds_map_find_first")){
    GmlDSMap *m=ds_map_slot(vm,(int)N(a,n,0));
    if(m && m->len>0) return ds_ret(m->entry[0].key_val);
    return vstr("");
  }
  if(!strcmp(nm,"ds_map_find_last")){
    GmlDSMap *m=ds_map_slot(vm,(int)N(a,n,0));
    if(m && m->len>0) return ds_ret(m->entry[m->len-1].key_val);
    return vstr("");
  }
  /* Key iteration follows insertion order. GM returns undefined past either end. */
  if(!strcmp(nm,"ds_map_find_next")||!strcmp(nm,"ds_map_find_previous")){
    GmlDSMap *m=ds_map_slot(vm,(int)N(a,n,0));
    DsKeyTemp kt={0};
    const char *key=(n>=2)?ds_key_temp(a[1],&kt):NULL;
    int i=ds_map_find_entry(m,key); ds_key_temp_free(&kt);
    int j = (i<0) ? -1 : (nm[12]=='n' ? i+1 : i-1);
    if(m && j>=0 && j<m->len) return ds_ret(m->entry[j].key_val);
    return vundef();
  }
  /* Return keys or values in the same insertion order used by map cursors. */
  if(!strcmp(nm,"ds_map_keys_to_array")||!strcmp(nm,"ds_map_values_to_array")){
    GmlDSMap *m=ds_map_slot(vm,(int)N(a,n,0));
    int len=m?m->len:0;
    GmlVal out=arr_newv(len);
    if(out.t!=V_ARR) return out;
    GmlArr *A=(GmlArr*)out.arr;
    int keys=!strcmp(nm,"ds_map_keys_to_array");
    for(int i=0;i<len;i++) A->data[i]=ds_ret(keys?m->entry[i].key_val:m->entry[i].val);
    return out;
  }
  /* ds_map_add_list/map mark the child for nested destroy/JSON in GM; storing the id keeps
   * lookups working (json_encode walks ids the same way). */
  if(!strcmp(nm,"ds_map_add_list")||!strcmp(nm,"ds_map_add_map")||!strcmp(nm,"ds_map_replace_list")||!strcmp(nm,"ds_map_replace_map")){
    if(n>=3){
      ds_map_put(vm,(int)N(a,n,0),a[1],a[2],1);
      ds_map_mark_child(vm,(int)N(a,n,0),a[1],strstr(nm,"_list")?1:2);
    }
    return vreal(0);
  }
  if(!strcmp(nm,"ds_map_find_value")){
    int id=(int)N(a,n,0);
    return gml_ds_map_find_value_direct(vm,id,n>=2?a[1]:vundef(),n>=2);
  }
  return gml_builtin_try_json(vm,nm,a,n);
}
