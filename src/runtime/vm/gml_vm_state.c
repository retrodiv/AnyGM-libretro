/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* gml_vm_state.c - canonical VM payload serialization and restore. */
#include "gml_vm.h"
#include "gml_vm_internal.h"
#include "gml_vm_state_codec.h"
#include "gml_value_internal.h"
#include "gml_builtin.h"
#include "gml_render.h"
#include "gml_audio.h"
#include "gml_particle.h"
#include "anygm_host.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <limits.h>

/* ---------------- save-state runtime serialization ---------------- */
enum { GML_VM_STATE_SCHEMA=4 };
#define GML_VM_STATE_MAGIC UINT32_C(0x534D5641)
/* Writing a state walks every instance's variables, and the names repeat across them: every
 * instance carries the same handful of built-in names, each time as the very same pointer into
 * the content mapping. Resolving one costs a hash of the whole text, a comparison against the
 * candidate it lands on, and a search for its position, so the same answer was being recomputed
 * thousands of times per state.
 *
 * Remembering the last answer for each pointer turns those repeats into a single comparison. It
 * is a memo, not a change of rule: a miss resolves exactly as before, and the bytes written are
 * the same either way. */
#define STATE_STR_MEMO_SLOTS 65536u
struct GmlVmStateStringMemo {
  const char *key[STATE_STR_MEMO_SLOTS];
  int32_t index[STATE_STR_MEMO_SLOTS];
};

/* The memo outlives one state, because the same names recur in the next one. A host recording a
 * rewind buffer writes a state every frame, so rebuilding the answers each time would throw away
 * the work a frame earlier had already done. It hangs off the content mapping the answers point
 * into, and dies with it. */
static struct GmlVmStateStringMemo *state_str_memo(GmlVM *vm){
  if(!vm) return NULL;
  if(!vm->state_str_memo){
    vm->state_str_memo=calloc(1,sizeof(struct GmlVmStateStringMemo));
    if(vm->state_str_memo)
      for(unsigned i=0;i<STATE_STR_MEMO_SLOTS;i++)
        ((struct GmlVmStateStringMemo*)vm->state_str_memo)->index[i]=-1;
  }
  return (struct GmlVmStateStringMemo*)vm->state_str_memo;
}

struct GmlVmStateWriter {
  uint8_t *data;
  size_t cap, pos;
  int ok;
  GmlVM *vm;
  int compact_strings, array_meta;
  struct GmlVmStateStringMemo *memo;
};
struct GmlVmStateReader {
  const uint8_t *data;
  size_t cap, pos;
  int ok;
  GmlVM *vm;
  int compact_strings, array_meta;
};
typedef GmlVmStateWriter StateW;
typedef GmlVmStateReader StateR;

static int state_debug_enabled(const GmlVM *vm){
  return vm&&anygm_host_development_setting(vm->host,"GML_STATE_DEBUG")!=NULL;
}
static void state_debug(const GmlVM *vm,const char *msg,size_t pos,uint32_t v){
  if(state_debug_enabled(vm))
    anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[state] %s pos=%llu v=%u\n",msg,(unsigned long long)pos,v);
}

static void sw_raw(StateW *s, const void *p, size_t n){
  if(n>SIZE_MAX-s->pos){ s->ok=0; s->pos=SIZE_MAX; return; }
  if(s->data){
    if(s->pos<=s->cap && n<=s->cap-s->pos) memcpy(s->data+s->pos,p,n);
    else s->ok=0;
  }
  s->pos+=n;
}
static void sr_raw(StateR *s, void *p, size_t n){
  if(n>SIZE_MAX-s->pos){ memset(p,0,n); s->ok=0; s->pos=SIZE_MAX; return; }
  if(s->pos<=s->cap && n<=s->cap-s->pos) memcpy(p,s->data+s->pos,n);
  else { memset(p,0,n); s->ok=0; }
  s->pos+=n;
}
static void sw_u32(StateW *s, uint32_t v){
  uint8_t b[4]={(uint8_t)v,(uint8_t)(v>>8),(uint8_t)(v>>16),(uint8_t)(v>>24)};
  sw_raw(s,b,sizeof b);
}
static void sw_i32(StateW *s, int v){ sw_u32(s,(uint32_t)(int32_t)v); }
static void sw_i64(StateW *s, int64_t v){
  uint64_t u=(uint64_t)v; uint8_t b[8]; for(unsigned i=0;i<8;i++) b[i]=(uint8_t)(u>>(i*8));
  sw_raw(s,b,sizeof b);
}
static void sw_d(StateW *s, double v){ uint64_t bits=0; memcpy(&bits,&v,sizeof bits); sw_i64(s,(int64_t)bits); }
static uint32_t sr_u32(StateR *s){
  uint8_t b[4]={0}; sr_raw(s,b,sizeof b);
  return (uint32_t)b[0]|((uint32_t)b[1]<<8)|((uint32_t)b[2]<<16)|((uint32_t)b[3]<<24);
}
static int sr_i32(StateR *s){ return (int)(int32_t)sr_u32(s); }
static int64_t sr_i64(StateR *s){
  uint8_t b[8]={0}; sr_raw(s,b,sizeof b); uint64_t u=0;
  for(unsigned i=0;i<8;i++) u|=(uint64_t)b[i]<<(i*8);
  return (int64_t)u;
}
static double sr_d(StateR *s){ uint64_t bits=(uint64_t)sr_i64(s); double v=0; memcpy(&v,&bits,sizeof v); return v; }
static void sw_particle_state(StateW *s){
  size_t pn=gml_part_state_size(s->vm->particles);
  if(pn>UINT32_MAX){ s->ok=0; pn=0; }
  sw_u32(s,(uint32_t)pn);
  if(s->data){
    if(s->pos+pn<=s->cap){
      size_t wr=0;
      if(!gml_part_state_save(s->vm->particles,s->data+s->pos,pn,&wr) || wr!=pn) s->ok=0;
    } else {
      s->ok=0;
    }
  }
  s->pos+=pn;
}
static unsigned state_str_memo_slot(const char *p){
  uintptr_t v=(uintptr_t)p;
  v^=v>>33; v*=UINT64_C(0xff51afd7ed558ccd); v^=v>>29;
  return (unsigned)(v&(STATE_STR_MEMO_SLOTS-1u));
}

static int state_str_index_by_ptr(GmlVM *vm, const char *p){
  if(!vm || !vm->win || !p) return -1;
  /* Canonicalize by string content, not by the allocation that currently owns the bytes.
   * Runtime text loaded from a sidecar may equal a STRG entry while living outside the content
   * mapping. A state load interns that text, so pointer-only compaction made save-load-save choose
   * two different encodings for the same value. */
  p=gml_win_intern_lookup(vm->win,p);
  if(!p) return -1;
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
    unsigned slot=state_str_memo_slot(p);
    int idx;
    if(s->memo && s->memo->key[slot]==p) idx=s->memo->index[slot];
    else {
      idx=state_str_index_by_ptr(s->vm,p);
      if(s->memo){ s->memo->key[slot]=p; s->memo->index[slot]=idx; }
    }
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
  if(n>1024*1024){ state_debug(s->vm,"string too large",s->pos,n); s->ok=0; return strdup(""); }
  char *p=malloc((size_t)n+1);
  if(!p){ s->ok=0; return NULL; }
  sr_raw(s,p,n); p[n]=0; return p;
}
static const char *state_intern_ex(GmlVM *vm, char *owned, int *remains_owned){
  if(remains_owned) *remains_owned=0;
  if(!owned) return "";
  const char *hit=gml_win_intern_lookup(vm->win,owned);   /* O(1), avoiding repeated linear STRG scans */
  if(hit){ free(owned); return hit; }
  if(remains_owned) *remains_owned=1;
  return owned; /* the caller retains it as an owned map key or runtime string */
}
static const char *state_runtime_string(StateR *s, char *owned){
  if(!owned){ s->ok=0; return ""; }
  const char *hit=gml_win_intern_lookup(s->vm->win,owned);
  if(hit){ free(owned); return hit; }
  GmlVM *vm=s->vm;
  if(vm->state_runtime_string_count>=vm->state_runtime_string_capacity){
    int next=vm->state_runtime_string_capacity?vm->state_runtime_string_capacity*2:64;
    char **strings=realloc(vm->state_runtime_strings,(size_t)next*sizeof(*strings));
    if(!strings){ free(owned); s->ok=0; return ""; }
    vm->state_runtime_strings=strings;
    vm->state_runtime_string_capacity=next;
  }
  vm->state_runtime_strings[vm->state_runtime_string_count++]=owned;
  return owned;
}
void gml_vm_state_runtime_strings_clear(GmlVM *vm){
  if(!vm) return;
  for(int i=0;i<vm->state_runtime_string_count;i++) free(vm->state_runtime_strings[i]);
  vm->state_runtime_string_count=0;
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
        sw_u32(s,A->nested_2d?1u:0u);
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
      sw_u32(s,A->nested_2d?1u:0u);
    }
    for(int i=0;i<A->len;i++) sw_val(s,A->data[i],depth+1);
    return;
  }
  sw_u32(s,0);
}
static GmlVal sr_val(GmlVM *vm, StateR *s, int depth){
  uint32_t t=sr_u32(s);
  if(t==V_REAL) return vreal(sr_d(s));
  if(t==V_STR) return vstr(state_runtime_string(s,sr_str_dup(s)));
  if(t==V_UNDEF) return vundef();
  if(t==V_ARR && depth<8){
    uint32_t raw_len=sr_u32(s);
    int sparse = s->compact_strings && (raw_len&0x80000000u);
    uint32_t len = raw_len&0x7fffffffu;
    size_t remain = s->pos <= s->cap ? s->cap - s->pos : 0;
    uint32_t max_len = sparse ? 16000000u : 1000000u;
    if(len>max_len || (!sparse && len>remain/4)){
      state_debug(s->vm,"array too large",s->pos,len);
      s->ok=0; return vreal(0);
    }
    GmlArr *A=calloc(1,sizeof(GmlArr));
    if(!A){ s->ok=0; return vreal(0); }
    if(len>32000 && anygm_host_development_setting(vm->host,"GML_DBG_STATE_TIME") &&
       (!s->vm || s->vm->diagnostics.big_array_log_count++<20))
      anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[vmload]   big array len=%u sparse=%d\n",len,sparse);
    A->len=A->cap=(int)len;
    A->data=calloc(len?len:1,sizeof(GmlVal));
    if(!A->data){ free(A); s->ok=0; return vreal(0); }
    /* calloc IS the fill: vreal(0) = {V_REAL=0, 0.0, NULL, NULL} = all-zero bytes. sparse 2D
     * arrays have logical lengths in the hundreds of thousands (row*32000 stride); explicitly
     * storing vreal(0) into every slot touched a large amount of fresh pages per state load, which made
     * state load slow and the host's rewind a slideshow. Only the sparse entries below
     * touch memory now. */
    if(s->array_meta){
      uint32_t h=sr_u32(s);
      if(h>100000){ state_debug(s->vm,"array meta too large",s->pos,h); s->ok=0; h=0; }
      if(h>0){
        A->is_2d=1;
        gml_arr_row_ensure(A,(int)h-1);
        for(uint32_t i=0;i<h;i++){
          uint32_t w=sr_u32(s);
          if(w>GML_2D_STRIDE){ state_debug(s->vm,"array row too large",s->pos,w); s->ok=0; w=GML_2D_STRIDE; }
          if(A->row_len && i<(uint32_t)A->row_cap) A->row_len[i]=(int)w;
        }
      }
      uint32_t nested=sr_u32(s);
      if(nested>1){
        state_debug(s->vm,"bad nested array flag",s->pos,nested);
        s->ok=0;
      } else {
        A->nested_2d=(int)nested;
      }
    }
    if(sparse){
      uint32_t count=sr_u32(s);
      remain = s->pos <= s->cap ? s->cap - s->pos : 0;
      if(count>len || count>remain/8){ state_debug(s->vm,"sparse array too large",s->pos,count); s->ok=0; count=0; }
      for(uint32_t n=0;n<count;n++){
        uint32_t idx=sr_u32(s);
        GmlVal elem=sr_val(vm,s,depth+1);
        if(idx<len) A->data[idx]=elem;
        else s->ok=0;
      }
    } else {
      for(uint32_t i=0;i<len;i++) A->data[i]=sr_val(vm,s,depth+1);
    }
    if(!s->array_meta) gml_arr_rebuild_legacy_2d_meta(A);
    GmlVal v=vreal(0); v.t=V_ARR; v.arr=A; return v;
  }
  state_debug(s->vm,"bad value type",s->pos,t);
  s->ok=0; return vreal(0);
}

void gml_vm_state_write_raw(GmlVmStateWriter *writer,
                            const void *bytes,size_t size){
  sw_raw(writer,bytes,size);
}
void gml_vm_state_write_u32(GmlVmStateWriter *writer,uint32_t value){
  sw_u32(writer,value);
}
void gml_vm_state_write_i32(GmlVmStateWriter *writer,int value){
  sw_i32(writer,value);
}
void gml_vm_state_write_real(GmlVmStateWriter *writer,double value){
  sw_d(writer,value);
}
void gml_vm_state_write_string(GmlVmStateWriter *writer,const char *value){
  sw_str(writer,value);
}
void gml_vm_state_write_value(GmlVmStateWriter *writer,GmlVal value){
  sw_val(writer,value,0);
}
void gml_vm_state_read_raw(GmlVmStateReader *reader,
                           void *bytes,size_t size){
  sr_raw(reader,bytes,size);
}
uint32_t gml_vm_state_read_u32(GmlVmStateReader *reader){
  return sr_u32(reader);
}
int gml_vm_state_read_i32(GmlVmStateReader *reader){
  return sr_i32(reader);
}
double gml_vm_state_read_real(GmlVmStateReader *reader){
  return sr_d(reader);
}
char *gml_vm_state_read_string(GmlVmStateReader *reader){
  return sr_str_dup(reader);
}
GmlVal gml_vm_state_read_value(GmlVmStateReader *reader){
  return sr_val(reader->vm,reader,0);
}
int gml_vm_state_reader_ok(const GmlVmStateReader *reader){
  return reader && reader->ok;
}
size_t gml_vm_state_reader_position(const GmlVmStateReader *reader){
  return reader?reader->pos:0;
}
void gml_vm_state_reader_fail(GmlVmStateReader *reader,
                              const char *message,uint32_t detail){
  if(!reader) return;
  state_debug(reader->vm,message,reader->pos,detail);
  reader->ok=0;
}
size_t gml_vm_state_measure_string(GmlVM *vm,const char *value){
  StateW writer={0};
  writer.ok=1;
  writer.vm=vm;
  writer.compact_strings=1;
  writer.array_meta=1;
  sw_str(&writer,value);
  return writer.pos;
}
size_t gml_vm_state_measure_value(GmlVM *vm,GmlVal value){
  StateW writer={0};
  writer.ok=1;
  writer.vm=vm;
  writer.compact_strings=1;
  writer.array_meta=1;
  sw_val(&writer,value,0);
  return writer.pos;
}
static int state_var_slot_compare(const void *left,const void *right){
  const GmlVarSlot *a=*(GmlVarSlot *const *)left;
  const GmlVarSlot *b=*(GmlVarSlot *const *)right;
  return strcmp(a->key,b->key);
}

static void sw_varmap(StateW *s, GmlVarMap *m){
  int dbg=anygm_host_development_setting(s->vm?s->vm->host:NULL,"GML_DBG_STATEVAR")!=NULL;
  if(s->vm){
    if(s->vm->diagnostics.state_variable_debug<0)
      s->vm->diagnostics.state_variable_debug=dbg;
    dbg=s->vm->diagnostics.state_variable_debug;
  }
  uint32_t n=0;
  for(int i=0;i<m->cap;i++) if(m->slots[i].key) n++;
  if(n!=(uint32_t)m->len) state_debug(s->vm,"varmap len mismatch",s->pos,(uint32_t)m->len);
  sw_u32(s,n);
  if(!n) return;
  if(!s->vm || n>(uint32_t)INT_MAX){ s->ok=0; return; }
  if((int)n>s->vm->state_sort_slots_capacity){
    GmlVarSlot **next=realloc(s->vm->state_sort_slots,(size_t)n*sizeof(*next));
    if(!next){ s->ok=0; return; }
    s->vm->state_sort_slots=next;
    s->vm->state_sort_slots_capacity=(int)n;
  }
  uint32_t entry=0;
  for(int i=0;i<m->cap;i++) if(m->slots[i].key)
    s->vm->state_sort_slots[entry++]=&m->slots[i];
  qsort(s->vm->state_sort_slots,n,sizeof(*s->vm->state_sort_slots),state_var_slot_compare);
  for(uint32_t i=0;i<n;i++){
    GmlVarSlot *slot=s->vm->state_sort_slots[i];
    if(dbg) anygm_host_logf(s->vm ? s->vm->host : NULL,ANYGM_LOG_DEBUG,"[svar] %s t=%d\n",slot->key,slot->val.t);
    sw_str(s,slot->key);
    sw_val(s,slot->val,0);
  }
}
static int sr_varmap(GmlVM *vm, StateR *s, GmlVarMap *m){
  if(vm->diagnostics.state_variable_load_debug<0)
    vm->diagnostics.state_variable_load_debug=anygm_host_development_setting(vm->host,"GML_DBG_STATEVAR_LOAD")!=NULL;
  int dbg=vm->diagnostics.state_variable_load_debug;
  uint32_t n=sr_u32(s);
  if(n>100000){ state_debug(s->vm,"varmap too large",s->pos,n); s->ok=0; return 0; }
  for(uint32_t i=0;i<n && s->ok;i++){
    size_t key_pos=s->pos;
    int key_owned=0;
    char *k=sr_str_dup(s); const char *key=state_intern_ex(vm,k,&key_owned);
    if(dbg) anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[lvar] pos=%llu key=%s\n",(unsigned long long)key_pos,key?key:"");
    GmlVal v=sr_val(vm,s,0);
    if(!s->ok){ if(key_owned) free((char*)key); break; }
    gml_arr_mark_escaped(v);
    if(key_owned) *gml_varmap_put_owned_hashed(m,(char*)key,gml_value_name_hash(key))=v;
    else *gml_varmap_put(m,key)=v;
  }
  return s->ok;
}
/* Compact per-instance layout (lossless; a state may be written every frame under rewind):
 *  flags byte (active|marked<<1|deactivated<<2), id u32, obj i32,
 *  field mask u32 (bit set = value differs from its default and follows as a double, in bit
 *  order), x/y/xprev/yprev/xstart/ystart always as doubles, alarm mask u16 (bit = alarm != -1,
 *  set ones follow), path block (11 doubles) only when mask bit 20 is set, timeline block
 *  (5 doubles) only when mask bit 21 is set, then the varmap.
 *  Defaults mirror init_inst; equality is exact, so untouched fields round-trip bit-perfectly
 *  and anything else is written verbatim. Typical terrain instance: 308 -> ~80 bytes. */
#define STATE_INSTANCE_OPTIONAL_COUNT 20
static const double state_instance_default[STATE_INSTANCE_OPTIONAL_COUNT]={-1,-1,0,1,1,1,0,1,16777215,0,1,0,0,0,0,0,0,0,270,0};
#define STATE_INSTANCE_FIELDS(X) \
  X(0,sprite_index) X(1,mask_index) X(2,image_index) X(3,image_speed) X(4,image_xscale) \
  X(5,image_yscale) X(6,image_angle) X(7,image_alpha) X(8,image_blend) X(9,depth) \
  X(10,visible) X(11,solid) X(12,persistent) X(13,hspeed) X(14,vspeed) X(15,direction) \
  X(16,speed) X(17,gravity) X(18,gravity_direction) X(19,friction)
static int state_instance_path_present(GmlInstance *in){
  return in->path_index!=-1 || in->path_position!=0 || in->path_positionprevious!=0 ||
         in->path_speed!=0 || in->path_orientation!=0 || in->path_scale!=1 ||
         in->path_endaction!=0 || in->path_xoff!=0 || in->path_yoff!=0 ||
         in->path_origin_x!=0 || in->path_origin_y!=0;
}
static int state_instance_timeline_present(GmlInstance *in){
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
  #define FCHK(bit,field) if(in->field!=state_instance_default[bit]) fm|=1u<<(bit);
  STATE_INSTANCE_FIELDS(FCHK)
  #undef FCHK
  if(state_instance_path_present(in)) fm|=1u<<20;
  if(state_instance_timeline_present(in)) fm|=1u<<21;
  sw_u32(s,fm);
  sw_d(s,in->x); sw_d(s,in->y); sw_d(s,in->xprevious); sw_d(s,in->yprevious);
  sw_d(s,in->xstart); sw_d(s,in->ystart);
  #define FWR(bit,field) if(fm&(1u<<(bit))) sw_d(s,in->field);
  STATE_INSTANCE_FIELDS(FWR)
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
  if(!anygm_host_development_setting(vm->host,"GML_VM_STATE_PROFILE") || !vm) return;
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
  if(total <= vm->diagnostics.state_profile_last_total) return;
  vm->diagnostics.state_profile_last_total = total;
  anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[vm-state-profile] globals_total=%llu globals=%d\n",
          (unsigned long long)total, vm->globals.len);
  for(int i=0;i<10 && top[i].key;i++)
    anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[vm-state-profile] global[%d] %s=%llu\n",
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
  anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[vm-state-profile] instances_total=%llu inst_count=%d\n",
          (unsigned long long)inst_total, vm->inst_count);
  for(int i=0;i<10 && inst_top[i].key;i++)
    anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[vm-state-profile] inst[%d] %s=%llu\n",
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
  anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[vm-state-profile] structs_total=%llu structs=%d/%d\n",
          (unsigned long long)struct_total, struct_live, vm->n_structs);
  for(int i=0;i<10 && struct_top[i].key;i++)
    anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[vm-state-profile] struct[%d] %s=%llu\n",
            i, struct_top[i].key, (unsigned long long)struct_top[i].bytes);
  size_t ds_totals[3]={0};
  int ds_live[3]={0};
  gml_builtin_state_profile_ds(vm->builtins,ds_totals,ds_live);
  anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[vm-state-profile] ds_map=%d/%llu ds_list=%d/%llu ds_grid=%d/%llu\n",
          ds_live[0],(unsigned long long)ds_totals[0],
          ds_live[1],(unsigned long long)ds_totals[1],
          ds_live[2],(unsigned long long)ds_totals[2]);
}
static void sr_instance(GmlVM *vm, StateR *s, GmlInstance *in){
  uint8_t fl=0; sr_raw(s,&fl,1);
  in->active=fl&1; in->marked=(fl>>1)&1; in->deactivated=(fl>>2)&1;
  in->room_dormant=(fl>>3)&1; in->room_was_deactivated=(fl>>4)&1;
  in->room_placed=(fl>>5)&1;
  in->id=sr_u32(s); in->obj=sr_i32(s);
  in->room_owner=sr_i32(s);
  in->creation_seq=(uint64_t)sr_i64(s);
  uint32_t fm=sr_u32(s);
  in->x=sr_d(s); in->y=sr_d(s); in->xprevious=sr_d(s); in->yprevious=sr_d(s);
  in->xstart=sr_d(s); in->ystart=sr_d(s);
  #define FRD(bit,field) in->field = (fm&(1u<<(bit))) ? sr_d(s) : state_instance_default[bit];
  STATE_INSTANCE_FIELDS(FRD)
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
  in->draw_layer_order=sr_i32(s);
  in->draw_layer_element_order=-1; /* rebuilt from the current ROOM layer records after load */
  sr_varmap(vm,s,&in->vars);
}

static void runtime_release_builtin_value(void *userdata,GmlVal value){
  gml_val_free((GmlValueFreeContext *)userdata,value);
}

static void runtime_clear(GmlVM *vm){
  GmlValueFreeContext free_context={0};
  gml_value_free_context_begin(&free_context);
  gml_varmap_free_with_context(&vm->globals,0,&free_context);
  for(int i=0;i<vm->code_static_count;i++) gml_varmap_free_with_context(&vm->code_static[i],0,&free_context);
  for(int i=0;i<vm->inst_count;i++) gml_varmap_free_with_context(&vm->inst[i].vars,0,&free_context);
  for(int i=0;i<vm->n_structs;i++) if(vm->structs[i]) gml_varmap_free_with_context(&vm->structs[i]->vars,0,&free_context);
  gml_builtin_state_take_owned_values(vm->builtins,
                                      runtime_release_builtin_value,
                                      &free_context);
  gml_value_free_context_end(&free_context);
  gml_vm_state_runtime_strings_clear(vm);
  gml_builtin_state_reset(vm->builtins);
  if(vm->code_static_init && vm->code_static_count>0)
    memset(vm->code_static_init,0,(size_t)vm->code_static_count);
  for(int i=0;i<vm->n_structs;i++){ free(vm->structs[i]); vm->structs[i]=NULL; }
  if(vm->struct_gen && vm->cap_structs>0) memset(vm->struct_gen,0,(size_t)vm->cap_structs);
  vm->n_structs=0; vm->n_struct_free=0; vm->structs_last_gc_frame=0;
  if(vm->inst && vm->inst_cap>0) memset(vm->inst,0,(size_t)vm->inst_cap*sizeof(GmlInstance));
  vm->inst_count=0; vm->cur_self=vm->cur_other=NULL; vm->cur_event=NULL; vm->cur_event_obj=0;
  vm->next_creation_seq=1;
  vm->event_type=0; vm->event_number=0;
  vm->step_alloc_base=0; vm->action_relative=0;
  vm->potential_max_rotation=30; vm->potential_rotate_step=10;
  vm->potential_check_distance=3; vm->potential_rotate_on_spot=1;
  vm->window_x=0; vm->window_y=0; vm->window_cursor=0;
  gml_keyboard_unset_map(vm);
}
static int tilemap_diff_count(const GmlTileMap *tm){
  if(!tm || !tm->owned_tiles || !tm->base_tiles || tm->cols<=0 || tm->rows<=0) return 0;
  int cells=tm->cols*tm->rows, n=0;
  for(int c=0;c<cells;c++){
    uint32_t off=(uint32_t)c*4u;
    if(gml_vm_read_u32_le(tm->owned_tiles,off)!=
       gml_vm_read_u32_le(tm->base_tiles,off)) n++;
  }
  return n;
}
static void sw_vm(StateW *s, GmlVM *vm){
  s->vm=vm; s->compact_strings=1; s->array_meta=1;
  GmlBuiltinState *builtin_state=gml_builtin_state_ensure(vm);
  if(!builtin_state){ s->ok=0; return; }
  sw_u32(s,GML_VM_STATE_MAGIC);  /* AVMS */
  sw_u32(s,GML_VM_STATE_SCHEMA);
  sw_i32(s,vm->inst_count); sw_u32(s,vm->next_id);
  sw_i64(s,(int64_t)vm->next_creation_seq);
  sw_i32(s,vm->room_index); sw_i32(s,vm->pending_room); sw_i32(s,vm->game_end);
  sw_i32(s,vm->started); sw_d(s,vm->last_key); sw_d(s,vm->window_fullscreen);
  { 
    long age=vm->frame - vm->room_enter_frame;
    if(age<0) age=0;
    sw_i64(s,(int64_t)age);
  }
  sw_i32(s,vm->action_relative);
  /* The deterministic intra-frame clock feeds current_time/get_timer, so a frame that reads one
   * draws from it. Restoring a state has to restore the clock with it, or the redraw that follows
   * the load reads a different value than the frame being restored did and shows something else. */
  sw_i64(s,(int64_t)vm->time_sample_frame); sw_d(s,vm->time_sample_cpu_ms);
  sw_i32(s,vm->animation_due);
  sw_d(s,vm->math_epsilon);
  sw_d(s,vm->potential_max_rotation); sw_d(s,vm->potential_rotate_step);
  sw_d(s,vm->potential_check_distance); sw_i32(s,vm->potential_rotate_on_spot);
  sw_i32(s,vm->classic_info_active);
  sw_i32(s,vm->script_argc); for(int i=0;i<16;i++) sw_val(s,vm->script_args[i],0);
  for(int i=0;i<16;i++) sw_u32(s,vm->rng_well[i]);
  sw_i32(s,vm->rng_index); sw_u32(s,vm->rng_state);
  sw_u32(s,vm->rng_classic_state);
  { int flags[GML_SOFTWARE3D_STATE_FLAG_COUNT]; double values[GML_SOFTWARE3D_STATE_VALUE_COUNT];
    uint32_t colors[GML_SOFTWARE3D_STATE_COLOR_COUNT];
    gml_vm_software3d_state_get(vm,flags,values,colors);
    for(int i=0;i<GML_SOFTWARE3D_STATE_FLAG_COUNT;i++) sw_i32(s,flags[i]);
    for(int i=0;i<GML_SOFTWARE3D_STATE_VALUE_COUNT;i++) sw_d(s,values[i]);
    for(int i=0;i<GML_SOFTWARE3D_STATE_COLOR_COUNT;i++) sw_u32(s,colors[i]);
  }
  {
    size_t model_size=gml_d3_models_state_size(gml_vm_software3d_ensure(vm));
    if(model_size>UINT32_MAX){ s->ok=0; model_size=0; }
    sw_u32(s,(uint32_t)model_size);
    if(s->data){
      if(s->pos<=s->cap && model_size<=s->cap-s->pos){
        if(!gml_d3_models_state_save(gml_vm_software3d_ensure(vm),s->data+s->pos,model_size)) s->ok=0;
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
      uint32_t datum=gml_vm_read_u32_le(tm->owned_tiles,off);
      if(datum==gml_vm_read_u32_le(tm->base_tiles,off)) continue;
      sw_i32(s,c);
      sw_u32(s,datum);
    }
  }
  gml_builtin_state_write_ini_ds(builtin_state,s);
  sw_varmap(s,&vm->globals);
  for(int i=0;i<vm->inst_count;i++) sw_instance(s,&vm->inst[i]);
  sw_i32(s,vm->n_structs);
  for(int i=0;i<vm->n_structs;i++){
    sw_i32(s,vm->struct_gen ? (int)(vm->struct_gen[i]&0x7F) : 0);
    sw_i32(s,vm->structs[i]!=NULL);
    if(vm->structs[i]) sw_instance(s,vm->structs[i]);
  }
  sw_d(s,vm->window_x); sw_d(s,vm->window_y);
  /* Runtime layers and elements must survive rewind and state restoration. */
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
  gml_builtin_state_write_physics(builtin_state,s);
  sw_i32(s,vm->window_cursor);
  sw_particle_state(s);
  sw_raw(s,vm->key_map,sizeof(vm->key_map));
  gml_builtin_state_write_audio(builtin_state,s);
  int static_live=0;
  for(int i=0;i<vm->code_static_count;i++)
    if((vm->code_static_init && vm->code_static_init[i]) || vm->code_static[i].len>0) static_live++;
  sw_i32(s,static_live);
  for(int i=0;i<vm->code_static_count;i++){
    if((!vm->code_static_init || !vm->code_static_init[i]) && vm->code_static[i].len<=0) continue;
    sw_i32(s,i); sw_i32(s,vm->code_static_init?vm->code_static_init[i]!=0:0);
    sw_varmap(s,&vm->code_static[i]);
  }
  gml_builtin_state_write_time_sources(builtin_state,s);
  /* window_set_size/display_set_gui_size are persistent presentation state.  Without these values a
   * state restored into a fresh host session inherited the boot geometry instead, so the
   * same simulation could be presented and addressed through a different-sized window. */
  sw_i32(s,vm->window_w); sw_i32(s,vm->window_h);
  sw_i32(s,vm->gui_w); sw_i32(s,vm->gui_h);
  sw_i32(s,vm->gui_maximise_active);
  sw_d(s,vm->gui_maximise_xscale); sw_d(s,vm->gui_maximise_yscale);
  sw_d(s,vm->gui_maximise_xoffset); sw_d(s,vm->gui_maximise_yoffset);
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
  
  if(vm && vm->n_structs>0 && vm->structs_last_gc_frame!=vm->frame){
    vm->structs_last_gc_frame=vm->frame;
    gml_struct_gc(vm);
  }
  StateW s={0}; s.ok=1; s.vm=vm; s.compact_strings=1; s.memo=state_str_memo(vm);
  sw_vm(&s,vm); return s.pos;
}
int gml_vm_state_save(GmlVM *vm, void *data, size_t len, size_t *written){
  
  if(vm && vm->n_structs>0 && vm->structs_last_gc_frame!=vm->frame){
    vm->structs_last_gc_frame=vm->frame;
    gml_struct_gc(vm);
  }
  StateW s={.data=(uint8_t*)data,.cap=len,.pos=0,.ok=1,.vm=vm,.compact_strings=1,
            .memo=state_str_memo(vm)};
  sw_vm(&s,vm); if(written) *written=s.pos; return s.ok && s.pos<=len;
}
int gml_vm_state_load(GmlVM *vm, const void *data, size_t len, size_t *used){
  gml_colgrid_invalidate(vm);   /* wholesale: every instance is about to be rewritten */
  StateR s={.data=(const uint8_t*)data,.cap=len,.pos=0,.ok=1,.vm=vm};
  uint32_t magic=sr_u32(&s);
  uint32_t schema=sr_u32(&s);
  if(magic!=GML_VM_STATE_MAGIC || schema!=GML_VM_STATE_SCHEMA || !s.ok){
    state_debug(vm,"bad VM state header",s.pos,magic); return 0;
  }
  s.compact_strings=1;
  s.array_meta=1;
  GmlBuiltinState *builtin_state=gml_builtin_state_ensure(vm);
  if(!builtin_state) return 0;
  void *render=vm->render, *audio=vm->audio;
  runtime_clear(vm);
  int inst_count=sr_i32(&s); if(inst_count<0 || inst_count>vm->inst_cap) s.ok=0;
  if(!s.ok) state_debug(vm,"bad inst_count",s.pos,(uint32_t)inst_count);
  vm->next_id=sr_u32(&s);
  vm->next_creation_seq=(uint64_t)sr_i64(&s);
  vm->room_index=sr_i32(&s); vm->pending_room=sr_i32(&s);
  vm->game_end=sr_i32(&s); vm->started=sr_i32(&s);
  vm->last_key=sr_d(&s); vm->window_fullscreen=sr_d(&s);
  int64_t room_age=sr_i64(&s);
  if(room_age<0) room_age=0;
  if(room_age>(int64_t)LONG_MAX) room_age=(int64_t)LONG_MAX;
  { 
    vm->room_enter_frame = vm->frame - (long)room_age;
  }
  vm->action_relative=sr_i32(&s);
  vm->time_sample_frame=(long)sr_i64(&s); vm->time_sample_cpu_ms=sr_d(&s);
  vm->animation_due=sr_i32(&s);
  vm->math_epsilon=sr_d(&s);
  vm->potential_max_rotation=sr_d(&s); vm->potential_rotate_step=sr_d(&s);
  vm->potential_check_distance=sr_d(&s); vm->potential_rotate_on_spot=sr_i32(&s);
  vm->classic_info_active=sr_i32(&s);
  vm->script_argc=sr_i32(&s); for(int i=0;i<16;i++){ vm->script_args[i]=sr_val(vm,&s,0); gml_arr_mark_escaped(vm->script_args[i]); }
  for(int i=0;i<16;i++) vm->rng_well[i]=sr_u32(&s);
  vm->rng_index=sr_i32(&s); vm->rng_state=sr_u32(&s);
  vm->rng_classic_state=sr_u32(&s);
  { int flags[GML_SOFTWARE3D_STATE_FLAG_COUNT]={0};
    double values[GML_SOFTWARE3D_STATE_VALUE_COUNT]={0};
    uint32_t colors[GML_SOFTWARE3D_STATE_COLOR_COUNT]={0};
    for(int i=0;i<GML_SOFTWARE3D_STATE_FLAG_COUNT;i++) flags[i]=sr_i32(&s);
    for(int i=0;i<GML_SOFTWARE3D_STATE_VALUE_COUNT;i++) values[i]=sr_d(&s);
    for(int i=0;i<GML_SOFTWARE3D_STATE_COLOR_COUNT;i++) colors[i]=sr_u32(&s);
    if(s.ok) gml_vm_software3d_state_set(vm,flags,values,colors);
  }
  uint32_t model_size=sr_u32(&s);
  if(s.ok && s.pos<=s.cap && model_size<=s.cap-s.pos){
    if(!gml_d3_models_state_load(gml_vm_software3d_ensure(vm),s.data+s.pos,model_size)) s.ok=0;
    s.pos+=model_size;
  } else s.ok=0;
  int count=sr_i32(&s);
  if(count<0 || count>100000){ s.ok=0; count=0; }
  if(count!=vm->room_state_count){
    unsigned char *stored=calloc((size_t)(count>0?count:1),1);
    if(!stored && count>0) s.ok=0;
    else { free(vm->room_stored); vm->room_stored=stored; vm->room_state_count=count; }
  }
  if(count>0) sr_raw(&s,vm->room_stored,(size_t)count);
  vm->n_tile_mut=sr_i32(&s); sr_raw(&s,vm->tile_mut,sizeof(vm->tile_mut));
  vm->n_tile_del_at=sr_i32(&s); sr_raw(&s,vm->tile_del_at,sizeof(vm->tile_del_at));
  if(vm->n_tile_mut<0 || vm->n_tile_mut>64 || vm->n_tile_del_at<0 || vm->n_tile_del_at>64){
    state_debug(vm,"bad tile mutation counts",s.pos,(uint32_t)vm->n_tile_mut);
    s.ok=0;
  }
  typedef struct { int index, cols, rows, n; int *cell; uint32_t *datum; } TileMapState;
  TileMapState *tm_state=NULL;
  int tm_state_n=0;
  if(s.ok){
    tm_state_n=sr_i32(&s);
    if(tm_state_n<0 || tm_state_n>512){ state_debug(vm,"bad tilemap state count",s.pos,(uint32_t)tm_state_n); s.ok=0; tm_state_n=0; }
    tm_state=tm_state_n?calloc((size_t)tm_state_n,sizeof(*tm_state)):NULL;
    if(tm_state_n && !tm_state) s.ok=0;
    for(int i=0;i<tm_state_n;i++){
      tm_state[i].index=sr_i32(&s);
      tm_state[i].cols=sr_i32(&s);
      tm_state[i].rows=sr_i32(&s);
      if(tm_state[i].cols<=0 || tm_state[i].rows<=0 || tm_state[i].cols>8192 || tm_state[i].rows>8192){
        state_debug(vm,"bad tilemap state dims",s.pos,(uint32_t)tm_state[i].cols);
        s.ok=0;
        tm_state[i].cols=tm_state[i].rows=0;
      }
      int maxcells=tm_state[i].cols*tm_state[i].rows;
      tm_state[i].n=sr_i32(&s);
      if(tm_state[i].n<0 || tm_state[i].n>maxcells){
        state_debug(vm,"bad tilemap sparse count",s.pos,(uint32_t)tm_state[i].n);
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
    }
  }
  (void)gml_builtin_state_read_ini_ds(builtin_state,&s);
  if(!s.ok) state_debug(vm,"before globals failed",s.pos,0);
  uint64_t vt0=0,vt1=0,vt2=0;
  int vdbg=anygm_host_development_setting(vm->host,"GML_DBG_STATE_TIME")!=NULL;
  if(vdbg) vt0=anygm_host_monotonic_time_ns(vm->host);
  sr_varmap(vm,&s,&vm->globals);
  if(vdbg) vt1=anygm_host_monotonic_time_ns(vm->host);
  if(!s.ok) state_debug(vm,"globals failed",s.pos,0);
  vm->inst_count=s.ok?inst_count:0;
  for(int i=0;i<vm->inst_count;i++){
    sr_instance(vm,&s,&vm->inst[i]);
    if(!s.ok){ state_debug(vm,"instance failed",s.pos,(uint32_t)i); break; }
  }
  if(s.ok){
    int ns=sr_i32(&s);
    if(ns<0 || ns>GML_STRUCT_SLOT_MAX){ state_debug(vm,"bad struct count",s.pos,(uint32_t)ns); s.ok=0; ns=0; }
    if(s.ok && !gml_vm_struct_ensure_capacity(vm,ns)){ state_debug(vm,"struct alloc failed",s.pos,(uint32_t)ns); s.ok=0; ns=0; }
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
        gml_vm_struct_free_slot_push(vm,i);
      }
    }
  }
  if(vdbg){ vt2=anygm_host_monotonic_time_ns(vm->host);
    if(vm->diagnostics.state_time_log_count++<8) anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[vmload] globals=%.2fms inst(%d)=%.2fms\n",
      (double)(vt1-vt0)/1000000.0,
      vm->inst_count,
      (double)(vt2-vt1)/1000000.0); }
  if(s.ok && s.pos + sizeof(double)*2 <= s.cap){
    vm->window_x=sr_d(&s); vm->window_y=sr_d(&s);
  }
  /* Runtime layers and elements. */
  vm->n_rtl=0; vm->n_rte=0;
  if(s.ok){
    int restored_rt_next_id=sr_i32(&s);
    vm->rt_next_id=restored_rt_next_id;
    int nl=sr_i32(&s);
    if(nl<0 || nl>4096){ state_debug(vm,"bad rt layer count",s.pos,(uint32_t)nl); s.ok=0; nl=0; }
    for(int i=0;i<nl && s.ok;i++){
      GmlRtLayer *l=gml_rt_layer_new(vm);
      if(!l){ s.ok=0; break; }
      int id=sr_i32(&s);
      l->visible=sr_i32(&s); l->depth=sr_d(&s);
      l->x=sr_d(&s); l->y=sr_d(&s); l->hs=sr_d(&s); l->vs=sr_d(&s);
      sr_raw(&s,l->name,sizeof(l->name)); l->name[sizeof(l->name)-1]=0;
      l->touched=sr_i32(&s);
      l->script_begin=sr_i32(&s); l->script_end=sr_i32(&s);
      l->id=id;
      if(anygm_host_development_setting(vm->host,"GML_LOG_RTL")){
        anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[rtl-state] load layer id=%d name=\"%s\" vis=%d depth=%.0f pos=(%.2f,%.2f) speed=(%.2f,%.2f) touched=%d\n",
                l->id,l->name,l->visible,l->depth,l->x,l->y,l->hs,l->vs,l->touched);
      }
    }
    int ne=sr_i32(&s);
    if(ne<0 || ne>1000000){ state_debug(vm,"bad rt elem count",s.pos,(uint32_t)ne); s.ok=0; ne=0; }
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
      sr_raw(&s,e->name,sizeof(e->name));
      e->name[sizeof(e->name)-1]=0;
      e->image_index=sr_d(&s); e->image_speed=sr_d(&s); e->image_angle=sr_d(&s);
      e->id=id;
    }
    /* The generic allocators assign temporary IDs while reconstructing each record. The records
     * immediately receive their serialized IDs, so those allocation-side increments must not
     * advance the language-visible next-ID counter. */
    vm->rt_next_id=restored_rt_next_id;
  }
  if(s.ok) (void)gml_builtin_state_read_physics(builtin_state,&s);
  if(s.ok) vm->window_cursor=sr_i32(&s);
  if(s.ok){
    uint32_t pn=sr_u32(&s);
    if(s.pos>s.cap || pn>s.cap-s.pos){ state_debug(vm,"bad particle state size",s.pos,pn); s.ok=0; }
    else {
      size_t pu=0;
      if(!gml_part_state_load(vm->particles,s.data+s.pos,pn,&pu) || pu>pn){ state_debug(vm,"particle state failed",s.pos,pn); s.ok=0; }
      s.pos+=pn;
    }
  }
  if(s.ok){
    sr_raw(&s,vm->key_map,sizeof(vm->key_map));
    for(int i=0;i<256;i++) if(vm->key_map[i] < -1 || vm->key_map[i] > 255){
      state_debug(vm,"bad keyboard map",s.pos,(uint32_t)(uint16_t)vm->key_map[i]);
      s.ok=0;
      break;
    }
  }
  if(s.ok) (void)gml_builtin_state_read_audio(builtin_state,&s);
  if(s.ok){
    int live=sr_i32(&s);
    if(live<0 || live>vm->code_static_count){ state_debug(vm,"bad static scope count",s.pos,(uint32_t)live); s.ok=0; live=0; }
    for(int i=0;i<live && s.ok;i++){
      int ci=sr_i32(&s), initialized=sr_i32(&s);
      if(ci<0 || ci>=vm->code_static_count || (initialized!=0 && initialized!=1)){
        state_debug(vm,"bad static scope",s.pos,(uint32_t)ci); s.ok=0; break;
      }
      vm->code_static_init[ci]=(unsigned char)initialized;
      sr_varmap(vm,&s,&vm->code_static[ci]);
    }
  }
  if(s.ok) (void)gml_builtin_state_read_time_sources(builtin_state,&s);
  if(s.ok){
    vm->window_w=sr_i32(&s); vm->window_h=sr_i32(&s);
    vm->gui_w=sr_i32(&s); vm->gui_h=sr_i32(&s);
    vm->gui_maximise_active=sr_i32(&s);
    vm->gui_maximise_xscale=sr_d(&s); vm->gui_maximise_yscale=sr_d(&s);
    vm->gui_maximise_xoffset=sr_d(&s); vm->gui_maximise_yoffset=sr_d(&s);
    if(vm->window_w<0 || vm->window_h<0 || vm->gui_w<0 || vm->gui_h<0 ||
       vm->window_w>32768 || vm->window_h>32768 || vm->gui_w>32768 || vm->gui_h>32768){
      state_debug(vm,"bad presentation geometry",s.pos,(uint32_t)vm->window_w);
      s.ok=0;
    }
    if(vm->gui_maximise_active<0 || vm->gui_maximise_active>1 ||
       !isfinite(vm->gui_maximise_xscale) || !isfinite(vm->gui_maximise_yscale) ||
       !isfinite(vm->gui_maximise_xoffset) || !isfinite(vm->gui_maximise_yoffset) ||
       vm->gui_maximise_xscale<0.0 || vm->gui_maximise_yscale<0.0 ||
       (vm->gui_maximise_active &&
        ((vm->gui_maximise_xscale==0.0)!=(vm->gui_maximise_yscale==0.0)))){
      state_debug(vm,"bad GUI maximise transform",s.pos,(uint32_t)vm->gui_maximise_active);
      s.ok=0;
    }
  }
  if(s.ok){
    int slots=vm->win ? gml_room_count(vm->win)*8 : 0;
    int live=sr_i32(&s);
    if(slots<0 || live<0 || live>slots){
      state_debug(vm,"bad view override count",s.pos,(uint32_t)live);
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
      if(slot<0 || slot>=slots){ state_debug(vm,"bad view override slot",s.pos,(uint32_t)slot); s.ok=0; break; }
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
        state_debug(vm,"bad view override",s.pos,(uint32_t)slot); s.ok=0;
      }
    }
  }
  vm->cur_self=vm->cur_other=NULL; vm->cur_event=NULL; vm->cur_event_obj=0;
  vm->step_active=0; vm->step_alloc_base=0;
  vm->step_free_n=vm->step_free_pos=0;
  vm->render=render; vm->audio=audio;
  gml_obj_alive_recount(vm);   /* family live counts rebuilt from the restored pool */
  /* Rebuild collision maps, which point into immutable content data and are not serialized. */
  if(vm->win && vm->room_index>=0) gml_vm_room_reload_layers_mode(vm,vm->room_index,0);
  for(int i=0;i<tm_state_n;i++){
    TileMapState *ts=&tm_state[i];
    if(ts->index>=0 && ts->index<vm->n_tilemaps){
      GmlTileMap *tm=&vm->tilemaps[ts->index];
      if(tm->cols==ts->cols && tm->rows==ts->rows && ts->n>0 && gml_vm_tilemap_ensure_owned(tm)){
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
    free(ts->cell);
    free(ts->datum);
  }
  free(tm_state);
  /* The sampling clock is restored above rather than invalidated here: it stopped being a cached
   * host timestamp and became part of what a frame draws from, so discarding it would make the
   * redraw after a load read a different value than the frame being restored. */
  vm->time_sample_draw_ms=vm->time_sample_cpu_ms;
  if(used) *used=s.pos;
  if(s.ok && s.pos<=len){
    gml_vm_warm_audio_for_room_window(vm);
    gml_vm_prefetch_room_assets(vm);
  }
  return s.ok && s.pos<=len;
}
