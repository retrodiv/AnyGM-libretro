/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "gmlc_package_internal.h"

#include "anygm_host.h"
#include "gml_win.h"

#include <ctype.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int add_code_ref(Pkg *p, const GmlcRefSite *src, uint32_t code_start, int code_index){
  if(p->n_code_refs>=p->cap_code_refs){
    int nc=p->cap_code_refs?p->cap_code_refs*2:256;
    CodeRef *nr=(CodeRef*)realloc(p->code_refs,(size_t)nc*sizeof(*nr));
    if(!nr) return 0;
    p->code_refs=nr; p->cap_code_refs=nc;
  }
  CodeRef *r=&p->code_refs[p->n_code_refs++];
  r->name=gmlc_strdup(src->name?src->name:"");
  r->kind=src->kind;
  r->instr_abs=code_start+src->instr_off;
  r->ref_abs=code_start+src->ref_off;
  r->high_bits=src->high_bits;
  r->code_index=code_index;
  r->inst=src->inst;
  return r->name!=NULL;
}

static int add_code_blob_patch(Pkg *p, uint32_t rel_pos, uint32_t blob_off){
  if(p->n_code_blob_patches>=p->cap_code_blob_patches){
    int nc=p->cap_code_blob_patches?p->cap_code_blob_patches*2:128;
    CodeBlobPatch *np=(CodeBlobPatch*)realloc(p->code_blob_patches,(size_t)nc*sizeof(*np));
    if(!np) return 0;
    p->code_blob_patches=np; p->cap_code_blob_patches=nc;
  }
  p->code_blob_patches[p->n_code_blob_patches].rel_pos=rel_pos;
  p->code_blob_patches[p->n_code_blob_patches].blob_off=blob_off;
  p->n_code_blob_patches++;
  return 1;
}

static int add_code_name_ref(Pkg *p, int sid){
  if(p->n_code_name_refs>=p->cap_code_name_refs){
    int nc=p->cap_code_name_refs?p->cap_code_name_refs*2:128;
    CodeNameRef *np=(CodeNameRef*)realloc(p->code_name_refs,(size_t)nc*sizeof(*np));
    if(!np) return 0;
    p->code_name_refs=np; p->cap_code_name_refs=nc;
  }
  p->code_name_refs[p->n_code_name_refs++].sid=sid;
  return 1;
}

static int emit_code_data_prefix(Pkg *p){
  if(p->code_data_emitted) return 1;
  if(p->b.len>UINT32_MAX) return 0;
  p->code_blob_base=(uint32_t)p->b.len;
  if(p->code_data.len && !wbytes(&p->b,p->code_data.data,p->code_data.len)) return 0;
  for(int i=0;i<p->n_code_refs;i++){
    p->code_refs[i].instr_abs+=p->code_blob_base;
    p->code_refs[i].ref_abs+=p->code_blob_base;
  }
  p->code_data_emitted=1;
  return 1;
}

static int finalize_code_blobs(Pkg *p){
  if(p->code_blobs_finalized) return 1;
  if(!p->code_data_emitted && !emit_code_data_prefix(p)) return 0;
  uint32_t blob_base=p->code_blob_base;
  for(int i=0;i<p->n_code_blob_patches;i++){
    const CodeBlobPatch *bp=&p->code_blob_patches[i];
    uint32_t blob_abs=blob_base+bp->blob_off;
    int64_t rel=(int64_t)blob_abs-(int64_t)bp->rel_pos;
    if(rel<INT32_MIN || rel>INT32_MAX) return 0;
    patch32(&p->b,bp->rel_pos,(uint32_t)(int32_t)rel);
  }
  p->code_blobs_finalized=1;
  return 1;
}

static int same_ref_name(const CodeRef *a, const CodeRef *b){
  return a->kind==b->kind && a->name && b->name && !strcmp(a->name,b->name);
}

typedef struct {
  int index;
  uint32_t addr;
} RefOrder;

static int cmp_ref_order(const void *A, const void *B){
  const RefOrder *a=(const RefOrder*)A, *b=(const RefOrder*)B;
  return a->addr<b->addr?-1:(a->addr>b->addr?1:0);
}

static int patch_ref_chains(Pkg *p){
  if(p->refs_patched) return 1;
  for(int i=0;i<p->n_code_refs;i++){
    int seen=0;
    for(int j=0;j<i;j++) if(same_ref_name(&p->code_refs[i],&p->code_refs[j])){ seen=1; break; }
    if(seen) continue;
    int count=0;
    for(int j=i;j<p->n_code_refs;j++) if(same_ref_name(&p->code_refs[i],&p->code_refs[j])) count++;
    RefOrder *ord=(RefOrder*)malloc((size_t)(count?count:1)*sizeof(*ord));
    if(!ord) return 0;
    int n=0;
    for(int j=i;j<p->n_code_refs;j++) if(same_ref_name(&p->code_refs[i],&p->code_refs[j])){
      ord[n].index=j;
      ord[n].addr=p->code_refs[j].instr_abs;
      n++;
    }
    qsort(ord,(size_t)n,sizeof(*ord),cmp_ref_order);
    for(int k=0;k<n;k++){
      CodeRef *r=&p->code_refs[ord[k].index];
      uint32_t delta=0;
      if(k+1<n){
        uint32_t next=p->code_refs[ord[k+1].index].instr_abs;
        if(next<r->instr_abs || next-r->instr_abs>0x07FFFFFFu){ free(ord); return 0; }
        delta=next-r->instr_abs;
      } else {
        int sid=intern(p,r->name?r->name:"");
        if(sid<0 || sid>0x07FFFFFF){ free(ord); return 0; }
        delta=(uint32_t)sid;
      }
      patch32(&p->b,r->ref_abs,(r->high_bits & 0xF8000000u) | delta);
    }
    free(ord);
  }
  p->refs_patched=1;
  return 1;
}

static int ref_group_count(const Pkg *p, GmlcRefKind kind){
  int n=0;
  for(int i=0;i<p->n_code_refs;i++){
    if(p->code_refs[i].kind!=kind) continue;
    int seen=0;
    for(int j=0;j<i;j++) if(p->code_refs[j].kind==kind && same_ref_name(&p->code_refs[i],&p->code_refs[j])){ seen=1; break; }
    if(!seen) n++;
  }
  return n;
}

static int ref_group_stats(const Pkg *p, int idx, uint32_t *occ, uint32_t *first_addr){
  const CodeRef *base=&p->code_refs[idx];
  uint32_t n=0, first=0xFFFFFFFFu;
  for(int i=0;i<p->n_code_refs;i++){
    if(!same_ref_name(base,&p->code_refs[i])) continue;
    n++;
    if(p->code_refs[i].instr_abs<first) first=p->code_refs[i].instr_abs;
  }
  *occ=n;
  *first_addr=first==0xFFFFFFFFu?0:first;
  return 1;
}

static int total_object_events(const GmlcProject *p){
  int n=0;
  for(int i=0;i<p->n_objects;i++) n += p->objects[i].n_events;
  return n;
}

int total_timeline_moments(const GmlcProject *p){
  int n=0;
  for(int i=0;i<p->n_timelines;i++) n+=p->timelines[i].n_moments;
  return n;
}

int event_type_rank(int event_type){
  switch(event_type){
    case 0: return 0;   /* Create */
    case 1: return 1;   /* Destroy */
    case 2: return 2;   /* Alarm */
    case 3: return 3;   /* Step */
    case 4: return 4;   /* Collision */
    case 5: return 5;   /* Keyboard */
    case 6: return 6;   /* Mouse */
    case 7: return 7;   /* Other */
    case 8: return 8;   /* Draw */
    case 9: return 9;   /* KeyPress */
    case 10: return 10; /* KeyRelease */
    case 12: return 12; /* CleanUp */
    default: return 11 + event_type;
  }
}

static int room_has_creation_code(const GmlcRoom *r){
  return r && r->creation_code_path && *r->creation_code_path;
}

int room_code_count(const GmlcProject *p){
  int n=0;
  for(int i=0;i<p->n_rooms;i++) if(room_has_creation_code(&p->rooms[i])) n++;
  return n;
}

static int startup_code_count(const GmlcProject *p){
  return p && p->startup_code_path && *p->startup_code_path ? 1 : 0;
}

int room_creation_code_index(const GmlcProject *p, int room_index){
  if(!p || room_index<0 || room_index>=p->n_rooms ||
     !room_has_creation_code(&p->rooms[room_index])) return -1;
  int idx=0;
  for(int i=0;i<room_index;i++) if(room_has_creation_code(&p->rooms[i])) idx++;
  return idx;
}

int timeline_moment_code_index(const GmlcProject *p, int timeline_index, int moment_index){
  int index=room_code_count(p)+p->n_scripts;
  for(int i=0;i<timeline_index;i++) index+=p->timelines[i].n_moments;
  return index+moment_index;
}

static int room_instance_creation_code_count(const GmlcProject *p){
  int n=0;
  for(int ri=0;ri<p->n_rooms;ri++){
    const GmlcRoom *r=&p->rooms[ri];
    for(int ii=0;ii<r->n_instances;ii++){
      if(r->instances[ii].creation_code_path && *r->instances[ii].creation_code_path) n++;
    }
  }
  return n;
}

static int trigger_code_base(const GmlcProject *p){
  return room_code_count(p)+p->n_scripts+total_timeline_moments(p)+total_object_events(p)+
         room_instance_creation_code_count(p)+startup_code_count(p);
}

static int room_instance_creation_code_base(const GmlcProject *p){
  return room_code_count(p) + p->n_scripts + total_timeline_moments(p) + total_object_events(p);
}

int room_instance_creation_code_index(const GmlcProject *p, int room_index, int inst_index){
  if(room_index<0 || room_index>=p->n_rooms) return -1;
  const GmlcRoom *target=&p->rooms[room_index];
  if(inst_index<0 || inst_index>=target->n_instances) return -1;
  const GmlcRoomInstance *in=&target->instances[inst_index];
  if(!in->creation_code_path || !*in->creation_code_path) return -1;
  int idx=room_instance_creation_code_base(p);
  for(int ri=0;ri<room_index;ri++){
    const GmlcRoom *r=&p->rooms[ri];
    for(int ii=0;ii<r->n_instances;ii++)
      if(r->instances[ii].creation_code_path && *r->instances[ii].creation_code_path) idx++;
  }
  for(int ii=0;ii<inst_index;ii++)
    if(target->instances[ii].creation_code_path && *target->instances[ii].creation_code_path) idx++;
  return idx;
}

static int room_instance_index_by_id(const GmlcRoom *r, uint32_t instance_id){
  for(int i=0;i<r->n_instances;i++){
    if((uint32_t)r->instances[i].instance_id==instance_id) return i;
  }
  return -1;
}

static int room_layer_contains_instance(const GmlcRoom *r, int inst_index){
  uint32_t instance_id=(uint32_t)r->instances[inst_index].instance_id;
  for(int li=0;li<r->n_layers;li++){
    const GmlcRoomLayer *ly=&r->layers[li];
    for(int k=0;k<ly->n_instance_ids;k++) if(ly->instance_ids[k]==instance_id) return 1;
  }
  return 0;
}

static int room_instance_creation_ordinal(const GmlcRoom *r, int target_index){
  int ordinal=0;
  for(int li=r->n_layers-1;li>=0;li--){
    const GmlcRoomLayer *ly=&r->layers[li];
    for(int k=0;k<ly->n_instance_ids;k++){
      int ii=room_instance_index_by_id(r,ly->instance_ids[k]);
      if(ii<0 || !r->instances[ii].creation_code_path || !*r->instances[ii].creation_code_path) continue;
      if(ii==target_index) return ordinal;
      ordinal++;
    }
  }
  for(int ii=0;ii<r->n_instances;ii++){
    if(room_layer_contains_instance(r,ii) ||
       !r->instances[ii].creation_code_path || !*r->instances[ii].creation_code_path) continue;
    if(ii==target_index) return ordinal;
    ordinal++;
  }
  return -1;
}

static void id_token(char *dst, size_t cap, const char *src){
  if(!dst || cap==0) return;
  size_t j=0;
  if(src){
    for(size_t i=0;src[i] && j+1<cap;i++) dst[j++]=(src[i]=='-')?'_':src[i];
  }
  dst[j]=0;
}

static const char *event_suffix(const GmlcObjectEvent *ev, char *buf, size_t cap){
  switch(ev->event_type){
    case 0: snprintf(buf,cap,"Create_0"); break;
    case 1: snprintf(buf,cap,"Destroy_0"); break;
    case 2: snprintf(buf,cap,"Alarm_%d",ev->event_number); break;
    case 3: snprintf(buf,cap,"Step_%d",ev->event_number); break;
    case 4:
      if(ev->collision_id && *ev->collision_id && strcmp(ev->collision_id,"00000000-0000-0000-0000-000000000000")){
        char idbuf[64];
        id_token(idbuf,sizeof(idbuf),ev->collision_id);
        snprintf(buf,cap,"Collision_%s",idbuf);
      }
      else snprintf(buf,cap,"Collision_%d",ev->collision_object_id);
      break;
    case 5: snprintf(buf,cap,"Keyboard_%d",ev->event_number); break;
    case 6: snprintf(buf,cap,"Mouse_%d",ev->event_number); break;
    case 7: snprintf(buf,cap,"Other_%d",ev->event_number); break;
    case 8: snprintf(buf,cap,"Draw_%d",ev->event_number); break;
    case 9: snprintf(buf,cap,"KeyPress_%d",ev->event_number); break;
    case 10: snprintf(buf,cap,"KeyRelease_%d",ev->event_number); break;
    case 11: snprintf(buf,cap,"Trigger_%d",ev->event_number); break;
    case 12: snprintf(buf,cap,"CleanUp_%d",ev->event_number); break;
    default: snprintf(buf,cap,"Other_%d",ev->event_number); break;
  }
  return buf;
}

static char *object_event_code_name(const GmlcObject *obj, const GmlcObjectEvent *ev){
  char suffix[96];
  event_suffix(ev,suffix,sizeof(suffix));
  size_t n=strlen(obj->name?obj->name:"")+strlen(suffix)+16;
  char *out=(char*)malloc(n);
  if(out) snprintf(out,n,"gml_Object_%s_%s",obj->name?obj->name:"",suffix);
  return out;
}

static char *script_code_name(const GmlcScript *script){
  size_t n=strlen(script->name?script->name:"")+12;
  char *out=(char*)malloc(n);
  if(out) snprintf(out,n,"gml_Script_%s",script->name?script->name:"");
  return out;
}

typedef struct {
  int sid;
  uint32_t size;
  uint32_t blob_off;
} CodeEntryPlan;

static int compile_code_blob(Pkg *pkg, const GmlcProject *p, const GmlcFunctionRegistry *funcs, int script_index, const char *path, GmlcCodeBlob *blob);

typedef struct {
  uint32_t off;
  int kind;
  int index;
} StringSeedEvent;

static int cmp_string_seed_event(const void *A, const void *B){
  const StringSeedEvent *a=(const StringSeedEvent*)A, *b=(const StringSeedEvent*)B;
  if(a->off<b->off) return -1;
  if(a->off>b->off) return 1;
  return a->kind-b->kind;
}

static int seed_code_blob_strings(Pkg *pkg, const GmlcCodeBlob *blob){
  int n=blob->n_refs + blob->n_strings;
  if(n<=0) return 1;
  StringSeedEvent *ev=(StringSeedEvent*)malloc((size_t)n*sizeof(*ev));
  if(!ev) return 0;
  int k=0;
  for(int i=0;i<blob->n_refs;i++){
    ev[k].off=blob->refs[i].instr_off;
    ev[k].kind=0;
    ev[k].index=i;
    k++;
  }
  for(int i=0;i<blob->n_strings;i++){
    ev[k].off=blob->strings[i].payload_off;
    ev[k].kind=1;
    ev[k].index=i;
    k++;
  }
  qsort(ev,(size_t)n,sizeof(*ev),cmp_string_seed_event);
  for(int i=0;i<n;i++){
    int sid = ev[i].kind==0
      ? intern(pkg,blob->refs[ev[i].index].name?blob->refs[ev[i].index].name:"")
      : intern(pkg,blob->strings[ev[i].index].value?blob->strings[ev[i].index].value:"");
    if(sid<0){ free(ev); return 0; }
  }
  free(ev);
  return 1;
}

static int seed_compiled_path_strings(Pkg *pkg, const GmlcProject *p, const GmlcFunctionRegistry *funcs, int script_index, const char *path){
  GmlcCodeBlob blob;
  int compiled_save=pkg->compiled_code;
  int placeholder_save=pkg->placeholder_code;
  if(!compile_code_blob(pkg,p,funcs,script_index,path,&blob)) return 0;
  pkg->compiled_code=compiled_save;
  pkg->placeholder_code=placeholder_save;
  int ok=seed_code_blob_strings(pkg,&blob);
  gmlc_bytecode_free(&blob);
  return ok;
}

static int seed_function_strings(Pkg *pkg, const GmlcProject *p, const GmlcFunctionRegistry *funcs, const GmlcFunctionDef *def){
  GmlcCodeBlob blob;
  if(pkg->code_placeholders){
    if(!gmlc_bytecode_emit_empty(&blob)) return 0;
    int ok=seed_code_blob_strings(pkg,&blob);
    gmlc_bytecode_free(&blob);
    return ok;
  }
  char berr[512]={0};
  if(!gmlc_bytecode_compile_function_body(p,funcs,def,&blob,berr,sizeof(berr))){
    if(!gmlc_bytecode_emit_empty(&blob)) return 0;
  }
  int ok=seed_code_blob_strings(pkg,&blob);
  gmlc_bytecode_free(&blob);
  return ok;
}

int seed_code_string_order(Pkg *pkg, const GmlcProject *p, char *err, size_t errcap){
  if(intern(pkg,"prototype")<0 || intern(pkg,"@@array@@")<0 || intern(pkg,"arguments")<0) return 0;
  if(pkg->code_placeholders) return 1;
  int base_count=trigger_code_base(p)+p->n_triggers;
  GmlcFunctionRegistry funcs;
  if(!gmlc_bytecode_collect_functions(p,base_count,&funcs,err,errcap)) return 0;
  int ok=0;
  for(int i=0;i<p->n_rooms;i++){
    if(!room_has_creation_code(&p->rooms[i])) continue;
    if(!seed_compiled_path_strings(pkg,p,&funcs,-1,p->rooms[i].creation_code_path)) goto done;
  }
  for(int i=0;i<p->n_scripts;i++){
    if(!seed_compiled_path_strings(pkg,p,&funcs,i,p->scripts[i].source_path)) goto done;
  }
  for(int i=0;i<p->n_timelines;i++) for(int m=0;m<p->timelines[i].n_moments;m++){
    if(!seed_compiled_path_strings(pkg,p,&funcs,-1,p->timelines[i].moments[m].source_path)) goto done;
  }
  for(int oi=0;oi<p->n_objects;oi++){
    const GmlcObject *obj=&p->objects[oi];
    int max_rank=-1;
    for(int ei=0;ei<obj->n_events;ei++){
      int rank=event_type_rank(obj->events[ei].event_type);
      if(rank>max_rank) max_rank=rank;
    }
    for(int rank=0;rank<=max_rank;rank++){
      for(int ei=0;ei<obj->n_events;ei++){
        if(event_type_rank(obj->events[ei].event_type)!=rank) continue;
        if(!seed_compiled_path_strings(pkg,p,&funcs,-1,obj->events[ei].source_path)) goto done;
      }
    }
  }
  for(int ri=0;ri<p->n_rooms;ri++){
    const GmlcRoom *r=&p->rooms[ri];
    for(int ii=0;ii<r->n_instances;ii++){
      const GmlcRoomInstance *in=&r->instances[ii];
      if(!in->creation_code_path || !*in->creation_code_path) continue;
      if(!seed_compiled_path_strings(pkg,p,&funcs,-1,in->creation_code_path)) goto done;
    }
  }
  if(startup_code_count(p) &&
     !seed_compiled_path_strings(pkg,p,&funcs,-1,p->startup_code_path)) goto done;
  for(int i=0;i<p->n_triggers;i++)
    if(!seed_compiled_path_strings(pkg,p,&funcs,-1,p->triggers[i].condition_path)) goto done;
  for(int i=0;i<funcs.n_defs;i++){
    if(funcs.defs[i].is_script_wrapper) continue;
    if(!seed_function_strings(pkg,p,&funcs,&funcs.defs[i])) goto done;
  }
  ok=1;
done:
  gmlc_function_registry_free(&funcs);
  return ok;
}

static int prepare_code_blob(Pkg *pkg, int sid, int code_index, GmlcCodeBlob *blob, CodeEntryPlan *entry, char *err, size_t errcap){
  for(int i=0;i<blob->n_strings;i++){
    GmlcStringSite *s=&blob->strings[i];
    if(s->payload_off+4>blob->size){
      snprintf(err,errcap,"string literal patch outside code blob");
      return 0;
    }
    int sid_lit=intern(pkg,s->value?s->value:"");
    if(sid_lit<0) return 0;
    blob->data[s->payload_off]=(uint8_t)sid_lit;
    blob->data[s->payload_off+1]=(uint8_t)(sid_lit>>8);
    blob->data[s->payload_off+2]=(uint8_t)(sid_lit>>16);
    blob->data[s->payload_off+3]=(uint8_t)(sid_lit>>24);
  }
  if(!add_code_name_ref(pkg,sid)) return 0;
  uint32_t code_start=(uint32_t)pkg->code_data.len;
  entry->sid=sid;
  entry->size=(uint32_t)blob->size;
  entry->blob_off=code_start;
  if(!wbytes(&pkg->code_data,blob->data,blob->size)) return 0;
  for(int i=0;i<blob->n_refs;i++) if(!add_code_ref(pkg,&blob->refs[i],code_start,code_index)) return 0;
  return 1;
}

static int write_code_entry_header(Pkg *pkg, const CodeEntryPlan *entry){
  wstrptr(pkg,entry->sid);
  wu32(&pkg->b,entry->size);
  wu32(&pkg->b,0);
  uint32_t rel_pos=(uint32_t)pkg->b.len;
  wi32(&pkg->b,0);
  wu32(&pkg->b,0);
  return add_code_blob_patch(pkg,rel_pos,entry->blob_off);
}

static int keep_or_emit_placeholder_blob(GmlcCodeBlob *blob){
  if(blob->data && blob->size>0 && blob->is_placeholder) return 1;
  gmlc_bytecode_free(blob);
  return gmlc_bytecode_emit_empty(blob);
}

static int compile_code_blob(Pkg *pkg, const GmlcProject *p, const GmlcFunctionRegistry *funcs, int script_index, const char *path, GmlcCodeBlob *blob){
  memset(blob,0,sizeof(*blob));
  if(pkg->code_placeholders){
    if(!gmlc_bytecode_emit_empty(blob)) return 0;
    pkg->placeholder_code++;
    return 1;
  }
  if(path && *path){
    if(pkg->log_code_compile)
      anygm_host_logf(p ? p->host : NULL,ANYGM_LOG_DEBUG,"source_to_win: compiling code: %s\n",path);
    char berr[512]={0};
    if(gmlc_bytecode_compile_source_ex(p,funcs,script_index,path,blob,berr,sizeof(berr))){
      if(blob->is_placeholder){
        pkg->placeholder_code++;
        if(blob->diagnostic)
          anygm_host_logf(p ? p->host : NULL,ANYGM_LOG_DEBUG,"source_to_win: code placeholder: %s: %s\n",path,blob->diagnostic);
      } else {
        pkg->compiled_code++;
      }
      return 1;
    }
    char *diagnostic=blob->diagnostic ? gmlc_strdup(blob->diagnostic) : NULL;
    if(!keep_or_emit_placeholder_blob(blob)){
      free(diagnostic);
      return 0;
    }
    if(!blob->diagnostic)
      blob->diagnostic=diagnostic ? diagnostic : gmlc_strdup(berr[0]?berr:"source read failed");
    else
      free(diagnostic);
    pkg->placeholder_code++;
    anygm_host_logf(p ? p->host : NULL,ANYGM_LOG_DEBUG,"source_to_win: code placeholder: %s: %s\n",path,blob->diagnostic?blob->diagnostic:"source read failed");
    return 1;
  }
  if(!gmlc_bytecode_emit_empty(blob)) return 0;
  /* An event/action with no source is a genuine no-op, not a failed compile. */
  return 1;
}

static int write_compiled_code_entry(Pkg *pkg, const GmlcProject *p, const GmlcFunctionRegistry *funcs, int script_index, CodeEntryPlan *entries, int *ci, int sid, const char *path, char *err, size_t errcap){
  GmlcCodeBlob blob;
  if(!compile_code_blob(pkg,p,funcs,script_index,path,&blob)) return 0;
  int ok=prepare_code_blob(pkg,sid,*ci,&blob,&entries[*ci],err,errcap);
  if(ok) (*ci)++;
  gmlc_bytecode_free(&blob);
  return ok;
}

static char *function_code_name(const GmlcFunctionDef *def){
  if(def->name && *def->name){
    size_t n=strlen(def->name)+12;
    char *out=(char*)malloc(n);
    if(out) snprintf(out,n,"gml_Script_%s",def->name);
    return out;
  }
  char tmp[64];
  snprintf(tmp,sizeof(tmp),"gml_Function_%d",def->code_index);
  return gmlc_strdup(tmp);
}

static int write_function_code_entry(Pkg *pkg, const GmlcProject *p, const GmlcFunctionRegistry *funcs, CodeEntryPlan *entries, int *ci, const GmlcFunctionDef *def, char *err, size_t errcap){
  if(*ci!=def->code_index){
    snprintf(err,errcap,"function code index mismatch");
    return 0;
  }
  char *name=function_code_name(def);
  if(!name) return 0;
  int sid=intern(pkg,name);
  free(name);
  GmlcCodeBlob blob;
  if(pkg->code_placeholders){
    if(!gmlc_bytecode_emit_empty(&blob)) return 0;
    pkg->placeholder_code++;
    int ok=prepare_code_blob(pkg,sid,*ci,&blob,&entries[*ci],err,errcap);
    if(ok) (*ci)++;
    gmlc_bytecode_free(&blob);
    return ok;
  }
  char berr[512]={0};
  if(pkg->log_code_compile)
    anygm_host_logf(p ? p->host : NULL,ANYGM_LOG_DEBUG,"source_to_win: compiling function: %d\n",def->code_index);
  if(!gmlc_bytecode_compile_function_body(p,funcs,def,&blob,berr,sizeof(berr))){
    char *diagnostic=blob.diagnostic ? gmlc_strdup(blob.diagnostic) : NULL;
    if(!keep_or_emit_placeholder_blob(&blob)){
      free(diagnostic);
      return 0;
    }
    if(!blob.diagnostic)
      blob.diagnostic=diagnostic ? diagnostic : gmlc_strdup(berr[0]?berr:"function source read failed");
    else
      free(diagnostic);
    pkg->placeholder_code++;
    anygm_host_logf(p ? p->host : NULL,ANYGM_LOG_DEBUG,"source_to_win: code placeholder: function %d: %s\n",def->code_index,blob.diagnostic?blob.diagnostic:"function source read failed");
  } else if(blob.is_placeholder){
    pkg->placeholder_code++;
    if(blob.diagnostic)
      anygm_host_logf(p ? p->host : NULL,ANYGM_LOG_DEBUG,"source_to_win: code placeholder: function %d: %s\n",def->code_index,blob.diagnostic);
  } else {
    pkg->compiled_code++;
  }
  int ok=prepare_code_blob(pkg,sid,*ci,&blob,&entries[*ci],err,errcap);
  if(ok) (*ci)++;
  gmlc_bytecode_free(&blob);
  return ok;
}

int write_code(Pkg *pkg, const GmlcProject *p, char *err, size_t errcap){
  size_t s=chunk_begin(pkg,"CODE");
  int base_count=trigger_code_base(p)+p->n_triggers;
  GmlcFunctionRegistry funcs;
  memset(&funcs,0,sizeof(funcs));
  if(!pkg->code_placeholders && !gmlc_bytecode_collect_functions(p,base_count,&funcs,err,errcap)) return 0;
  int count=base_count + gmlc_function_registry_extra_count(&funcs);
  wu32(&pkg->b,(uint32_t)count);
  size_t table=pkg->b.len;
  zfill(&pkg->b,(size_t)count*4);
  CodeEntryPlan *entries=(CodeEntryPlan*)calloc((size_t)(count?count:1),sizeof(*entries));
  if(!entries){
    gmlc_function_registry_free(&funcs);
    snprintf(err,errcap,"out of memory while writing code entries");
    return 0;
  }
  int ci=0;
  int ok=0;
  for(int i=0;i<p->n_rooms;i++){
    if(!room_has_creation_code(&p->rooms[i])) continue;
    char name[256];
    snprintf(name,sizeof(name),"gml_Room_%s_Create",p->rooms[i].name?p->rooms[i].name:"");
    int sid=intern(pkg,name);
    if(!write_compiled_code_entry(pkg,p,&funcs,-1,entries,&ci,sid,p->rooms[i].creation_code_path,err,errcap)) goto done;
  }
  for(int i=0;i<p->n_scripts;i++){
    char *name=script_code_name(&p->scripts[i]);
    int nsid=intern(pkg,name?name:"");
    free(name);
    if(!write_compiled_code_entry(pkg,p,&funcs,i,entries,&ci,nsid,p->scripts[i].source_path,err,errcap)) goto done;
  }
  for(int i=0;i<p->n_timelines;i++){
    const GmlcTimeline *timeline=&p->timelines[i];
    for(int m=0;m<timeline->n_moments;m++){
      char name[256];
      snprintf(name,sizeof(name),"gml_Timeline_%s_%d",timeline->name?timeline->name:"",timeline->moments[m].step);
      int sid=intern(pkg,name);
      if(!write_compiled_code_entry(pkg,p,&funcs,-1,entries,&ci,sid,timeline->moments[m].source_path,err,errcap)) goto done;
    }
  }
  for(int oi=0;oi<p->n_objects;oi++){
    const GmlcObject *obj=&p->objects[oi];
    int max_rank=-1;
    for(int ei=0;ei<obj->n_events;ei++){
      int rank=event_type_rank(obj->events[ei].event_type);
      if(rank>max_rank) max_rank=rank;
    }
    for(int rank=0;rank<=max_rank;rank++){
      for(int ei=0;ei<obj->n_events;ei++){
        if(event_type_rank(obj->events[ei].event_type)!=rank) continue;
        char *name=object_event_code_name(obj,&obj->events[ei]);
        int nsid=intern(pkg,name?name:"");
        free(name);
        if(!write_compiled_code_entry(pkg,p,&funcs,-1,entries,&ci,nsid,obj->events[ei].source_path,err,errcap)) goto done;
      }
    }
  }
  for(int ri=0;ri<p->n_rooms;ri++){
    const GmlcRoom *r=&p->rooms[ri];
    for(int ii=0;ii<r->n_instances;ii++){
      const GmlcRoomInstance *in=&r->instances[ii];
      if(!in->creation_code_path || !*in->creation_code_path) continue;
      int creation_ordinal=room_instance_creation_ordinal(r,ii);
      if(creation_ordinal<0) creation_ordinal=ii;
      char name[256];
      snprintf(name,sizeof(name),"gml_RoomCC_%s_%d_Create",r->name?r->name:"",creation_ordinal);
      int sid=intern(pkg,name);
      if(!write_compiled_code_entry(pkg,p,&funcs,-1,entries,&ci,sid,in->creation_code_path,err,errcap)) goto done;
    }
  }
  if(startup_code_count(p)){
    int sid=intern(pkg,"gml_GlobalScript___gmlc_classic_startup");
    if(!write_compiled_code_entry(pkg,p,&funcs,-1,entries,&ci,sid,p->startup_code_path,err,errcap)) goto done;
  }
  for(int i=0;i<p->n_triggers;i++){
    char name[64]; snprintf(name,sizeof(name),"gml_TriggerCondition_%d",p->triggers[i].runtime_id);
    int sid=intern(pkg,name);
    if(!write_compiled_code_entry(pkg,p,&funcs,-1,entries,&ci,sid,p->triggers[i].condition_path,err,errcap)) goto done;
  }
  for(int i=0;i<funcs.n_defs;i++){
    if(funcs.defs[i].is_script_wrapper) continue;
    if(!write_function_code_entry(pkg,p,&funcs,entries,&ci,&funcs.defs[i],err,errcap)) goto done;
  }
  if(ci!=count){
    snprintf(err,errcap,"code entry count mismatch");
    goto done;
  }
  if(!emit_code_data_prefix(pkg)){
    snprintf(err,errcap,"code blob write failed");
    goto done;
  }
  for(int i=0;i<count;i++){
    patch32(&pkg->b,table+(size_t)i*4,(uint32_t)pkg->b.len);
    if(!write_code_entry_header(pkg,&entries[i])){
      snprintf(err,errcap,"code entry header write failed");
      goto done;
    }
  }
  if(!finalize_code_blobs(pkg)){
    snprintf(err,errcap,"code blob layout patch failed");
    goto done;
  }
  if(!patch_ref_chains(pkg)){
    snprintf(err,errcap,"reference chain patch failed");
    goto done;
  }
  chunk_end(pkg,s);
  ok=1;
done:
  free(entries);
  gmlc_function_registry_free(&funcs);
  return ok;
}

int write_trig(Pkg *pkg, const GmlcProject *p){
  size_t s=chunk_begin(pkg,"TRIG");
  wu32(&pkg->b,(uint32_t)p->n_triggers);
  int base=trigger_code_base(p);
  for(int i=0;i<p->n_triggers;i++){
    wi32(&pkg->b,p->triggers[i].runtime_id);
    wi32(&pkg->b,p->triggers[i].moment);
    wi32(&pkg->b,base+i);
  }
  chunk_end(pkg,s);
  return 1;
}

int write_vari(Pkg *pkg){
  if(!patch_ref_chains(pkg)) return 0;
  size_t s=chunk_begin(pkg,"VARI");
  zfill(&pkg->b,12);
  int sid=intern(pkg,"prototype");
  if(sid<0) return 0;
  wstrptr(pkg,sid); wu32(&pkg->b,0); wu32(&pkg->b,0); wu32(&pkg->b,0); wu32(&pkg->b,UINT32_MAX);
  sid=intern(pkg,"@@array@@");
  if(sid<0) return 0;
  wstrptr(pkg,sid); wu32(&pkg->b,0); wu32(&pkg->b,0); wu32(&pkg->b,0); wu32(&pkg->b,UINT32_MAX);
  int arguments_sid=intern(pkg,"arguments");
  if(arguments_sid<0) return 0;
  for(int i=0;i<pkg->n_code_name_refs;i++){
    wstrptr(pkg,arguments_sid); wu32(&pkg->b,0); wu32(&pkg->b,0); wu32(&pkg->b,0); wu32(&pkg->b,UINT32_MAX);
  }
  for(int i=0;i<pkg->n_code_refs;i++){
    if(pkg->code_refs[i].kind!=GMLC_REF_VARI) continue;
    int seen=0;
    for(int j=0;j<i;j++) if(pkg->code_refs[j].kind==GMLC_REF_VARI && same_ref_name(&pkg->code_refs[i],&pkg->code_refs[j])){ seen=1; break; }
    if(seen) continue;
    uint32_t occ=0, first=0;
    ref_group_stats(pkg,i,&occ,&first);
    int sid=intern(pkg,pkg->code_refs[i].name);
    wstrptr(pkg,sid);
    wu32(&pkg->b,0);
    wu32(&pkg->b,0);
    wu32(&pkg->b,occ);
    wu32(&pkg->b,first);
  }
  for(int i=0;i<pkg->n_code_refs;i++){
    if(pkg->code_refs[i].kind!=GMLC_REF_VARI) continue;
    int seen=0;
    for(int j=0;j<i;j++) if(pkg->code_refs[j].kind==GMLC_REF_VARI && same_ref_name(&pkg->code_refs[i],&pkg->code_refs[j])){ seen=1; break; }
    if(seen) continue;
    if(pkg->code_refs[i].name && !strncmp(pkg->code_refs[i].name,"argument",8)) continue;
    int groups=0;
    int has_nonlocal=0;
    for(int j=i;j<pkg->n_code_refs;j++){
      if(!same_ref_name(&pkg->code_refs[i],&pkg->code_refs[j])) continue;
      if(pkg->code_refs[j].inst!=IT_LOCAL){
        has_nonlocal=1;
        continue;
      }
      int group_seen=0;
      for(int k=i;k<j;k++){
        if(same_ref_name(&pkg->code_refs[i],&pkg->code_refs[k]) &&
           pkg->code_refs[k].inst==IT_LOCAL &&
           pkg->code_refs[k].code_index==pkg->code_refs[j].code_index){
          group_seen=1;
          break;
        }
      }
      if(!group_seen) groups++;
    }
    if(groups<=0) continue;
    if(has_nonlocal) groups++;
    int dup_sid=intern(pkg,pkg->code_refs[i].name);
    if(dup_sid<0) return 0;
    for(int g=1;g<groups;g++){
      wstrptr(pkg,dup_sid); wu32(&pkg->b,0); wu32(&pkg->b,0); wu32(&pkg->b,0); wu32(&pkg->b,UINT32_MAX);
    }
  }
  chunk_end(pkg,s);
  return 1;
}

int write_func(Pkg *pkg){
  if(!patch_ref_chains(pkg)) return 0;
  size_t s=chunk_begin(pkg,"FUNC");
  wu32(&pkg->b,(uint32_t)ref_group_count(pkg,GMLC_REF_FUNC));
  for(int i=0;i<pkg->n_code_refs;i++){
    if(pkg->code_refs[i].kind!=GMLC_REF_FUNC) continue;
    int seen=0;
    for(int j=0;j<i;j++) if(pkg->code_refs[j].kind==GMLC_REF_FUNC && same_ref_name(&pkg->code_refs[i],&pkg->code_refs[j])){ seen=1; break; }
    if(seen) continue;
    uint32_t occ=0, first=0;
    ref_group_stats(pkg,i,&occ,&first);
    int sid=intern(pkg,pkg->code_refs[i].name);
    wstrptr(pkg,sid);
    wu32(&pkg->b,occ);
    wu32(&pkg->b,first);
  }
  int arguments_sid=intern(pkg,"arguments");
  if(arguments_sid<0) return 0;
  wu32(&pkg->b,(uint32_t)pkg->n_code_name_refs);
  for(int i=0;i<pkg->n_code_name_refs;i++){
    wu32(&pkg->b,1);
    wstrptr(pkg,pkg->code_name_refs[i].sid);
    wu32(&pkg->b,0);
    wstrptr(pkg,arguments_sid);
  }
  chunk_end(pkg,s);
  return 1;
}
