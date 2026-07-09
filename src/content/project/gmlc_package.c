/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me> */
#include "gmlc_package.h"
#include "gmlc_bytecode.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  uint8_t *data;
  size_t len, cap;
} Buf;

typedef struct {
  char **items;
  uint32_t *char_off;
  int n, cap;
} StrTab;

typedef struct {
  uint32_t pos;
  int sid;
} StrPatch;

typedef struct {
  uint32_t pos;
  int frame;
} FramePatch;

typedef struct {
  char *name;
  GmlcRefKind kind;
  uint32_t instr_abs;
  uint32_t ref_abs;
  uint32_t high_bits;
} CodeRef;

typedef struct {
  Buf b;
  StrTab strs;
  StrPatch *patches;
  int n_patches, cap_patches;
  FramePatch *frame_patches;
  int n_frame_patches, cap_frame_patches;
  uint32_t *frame_tpag_ptr;
  int n_frames;
  CodeRef *code_refs;
  int n_code_refs, cap_code_refs;
  int refs_patched;
  int compiled_code;
  int placeholder_code;
} Pkg;

static int reserve(Buf *b, size_t n){
  if(b->len+n<=b->cap) return 1;
  size_t nc=b->cap?b->cap*2:4096;
  while(b->len+n>nc) nc*=2;
  uint8_t *nd=(uint8_t*)realloc(b->data,nc);
  if(!nd) return 0;
  b->data=nd; b->cap=nc;
  return 1;
}

static int wbytes(Buf *b, const void *p, size_t n){
  if(!reserve(b,n)) return 0;
  memcpy(b->data+b->len,p,n);
  b->len+=n;
  return 1;
}

static int wu8(Buf *b, uint8_t v){ return wbytes(b,&v,1); }
static int wu16(Buf *b, uint16_t v){ uint8_t x[2]={(uint8_t)v,(uint8_t)(v>>8)}; return wbytes(b,x,2); }
static int wu32(Buf *b, uint32_t v){ uint8_t x[4]={(uint8_t)v,(uint8_t)(v>>8),(uint8_t)(v>>16),(uint8_t)(v>>24)}; return wbytes(b,x,4); }
static int wi32(Buf *b, int32_t v){ return wu32(b,(uint32_t)v); }
static int wf32(Buf *b, float f){ uint32_t u; memcpy(&u,&f,4); return wu32(b,u); }
static int zfill(Buf *b, size_t n){ if(!reserve(b,n)) return 0; memset(b->data+b->len,0,n); b->len+=n; return 1; }
static void patch32(Buf *b, size_t pos, uint32_t v){ b->data[pos]=(uint8_t)v; b->data[pos+1]=(uint8_t)(v>>8); b->data[pos+2]=(uint8_t)(v>>16); b->data[pos+3]=(uint8_t)(v>>24); }

static int intern(Pkg *p, const char *s){
  if(!s) s="";
  for(int i=0;i<p->strs.n;i++) if(!strcmp(p->strs.items[i],s)) return i;
  if(p->strs.n>=p->strs.cap){
    int nc=p->strs.cap?p->strs.cap*2:128;
    char **ni=(char**)realloc(p->strs.items,(size_t)nc*sizeof(*ni));
    uint32_t *no=(uint32_t*)realloc(p->strs.char_off,(size_t)nc*sizeof(*no));
    if(!ni || !no){ free(ni); free(no); return -1; }
    p->strs.items=ni; p->strs.char_off=no; p->strs.cap=nc;
  }
  p->strs.items[p->strs.n]=gmlc_strdup(s);
  p->strs.char_off[p->strs.n]=0;
  return p->strs.items[p->strs.n] ? p->strs.n++ : -1;
}

static int add_str_patch(Pkg *p, uint32_t pos, int sid){
  if(p->n_patches>=p->cap_patches){
    int nc=p->cap_patches?p->cap_patches*2:128;
    StrPatch *np=(StrPatch*)realloc(p->patches,(size_t)nc*sizeof(*np));
    if(!np) return 0;
    p->patches=np; p->cap_patches=nc;
  }
  p->patches[p->n_patches].pos=pos;
  p->patches[p->n_patches].sid=sid;
  p->n_patches++;
  return 1;
}

static int add_frame_patch(Pkg *p, uint32_t pos, int frame){
  if(p->n_frame_patches>=p->cap_frame_patches){
    int nc=p->cap_frame_patches?p->cap_frame_patches*2:128;
    FramePatch *np=(FramePatch*)realloc(p->frame_patches,(size_t)nc*sizeof(*np));
    if(!np) return 0;
    p->frame_patches=np; p->cap_frame_patches=nc;
  }
  p->frame_patches[p->n_frame_patches].pos=pos;
  p->frame_patches[p->n_frame_patches].frame=frame;
  p->n_frame_patches++;
  return 1;
}

static int add_code_ref(Pkg *p, const GmlcRefSite *src, uint32_t code_start){
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
  return r->name!=NULL;
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

static int wstrptr(Pkg *p, int sid){
  uint32_t pos=(uint32_t)p->b.len;
  if(!wu32(&p->b,0)) return 0;
  return add_str_patch(p,pos,sid);
}

static size_t chunk_begin(Pkg *p, const char name[4]){
  wbytes(&p->b,name,4);
  size_t szpos=p->b.len;
  wu32(&p->b,0);
  return szpos;
}

static void chunk_end(Pkg *p, size_t szpos){
  patch32(&p->b,szpos,(uint32_t)(p->b.len-(szpos+4)));
}

static int empty_list_chunk(Pkg *p, const char name[4]){
  size_t s=chunk_begin(p,name);
  if(!wu32(&p->b,0)) return 0;
  chunk_end(p,s);
  return 1;
}

static int total_sprite_frames(const GmlcProject *p){
  int n=0;
  for(int i=0;i<p->n_sprites;i++) n += p->sprites[i].n_frames;
  return n;
}

static int global_frame_index(const GmlcProject *p, int sprite, int frame){
  int n=0;
  for(int i=0;i<sprite;i++) n += p->sprites[i].n_frames;
  return n+frame;
}

static int write_sprt(Pkg *pkg, const GmlcProject *p){
  size_t s=chunk_begin(pkg,"SPRT");
  uint32_t n=(uint32_t)p->n_sprites;
  wu32(&pkg->b,n);
  size_t table=pkg->b.len;
  zfill(&pkg->b,(size_t)n*4);
  for(uint32_t i=0;i<n;i++){
    const GmlcSprite *sp=&p->sprites[i];
    patch32(&pkg->b,table+i*4,(uint32_t)pkg->b.len);
    int sid=intern(pkg,sp->name);
    wstrptr(pkg,sid);
    wu32(&pkg->b,(uint32_t)sp->width);
    wu32(&pkg->b,(uint32_t)sp->height);
    wi32(&pkg->b,sp->bbox_left);
    wi32(&pkg->b,sp->bbox_right);
    wi32(&pkg->b,sp->bbox_bottom);
    wi32(&pkg->b,sp->bbox_top);
    wu32(&pkg->b,0);
    wu32(&pkg->b,0);
    wu32(&pkg->b,1);
    wu32(&pkg->b,0);
    wu32(&pkg->b,0);
    wi32(&pkg->b,sp->xorig);
    wi32(&pkg->b,sp->yorig);
    wu32(&pkg->b,(uint32_t)sp->n_frames);
    for(int f=0;f<sp->n_frames;f++){
      uint32_t pos=(uint32_t)pkg->b.len;
      wu32(&pkg->b,0);
      add_frame_patch(pkg,pos,global_frame_index(p,(int)i,f));
    }
    wu32(&pkg->b,0);
  }
  chunk_end(pkg,s);
  return 1;
}

static int write_tpag(Pkg *pkg, const GmlcProject *p){
  pkg->n_frames=total_sprite_frames(p);
  pkg->frame_tpag_ptr=(uint32_t*)calloc((size_t)(pkg->n_frames?pkg->n_frames:1),sizeof(uint32_t));
  if(!pkg->frame_tpag_ptr) return 0;
  size_t s=chunk_begin(pkg,"TPAG");
  wu32(&pkg->b,(uint32_t)pkg->n_frames);
  size_t table=pkg->b.len;
  zfill(&pkg->b,(size_t)pkg->n_frames*4);
  int gf=0;
  for(int i=0;i<p->n_sprites;i++){
    const GmlcSprite *sp=&p->sprites[i];
    for(int f=0;f<sp->n_frames;f++,gf++){
      uint32_t rec=(uint32_t)pkg->b.len;
      pkg->frame_tpag_ptr[gf]=rec;
      patch32(&pkg->b,table+(size_t)gf*4,rec);
      wu16(&pkg->b,0); wu16(&pkg->b,0);
      wu16(&pkg->b,(uint16_t)sp->width); wu16(&pkg->b,(uint16_t)sp->height);
      wu16(&pkg->b,0); wu16(&pkg->b,0);
      wu16(&pkg->b,(uint16_t)sp->width); wu16(&pkg->b,(uint16_t)sp->height);
      wu16(&pkg->b,(uint16_t)sp->width); wu16(&pkg->b,(uint16_t)sp->height);
      wu16(&pkg->b,(uint16_t)gf);
      wu16(&pkg->b,0);
    }
  }
  chunk_end(pkg,s);
  for(int i=0;i<pkg->n_frame_patches;i++){
    FramePatch *fp=&pkg->frame_patches[i];
    if(fp->frame>=0 && fp->frame<pkg->n_frames) patch32(&pkg->b,fp->pos,pkg->frame_tpag_ptr[fp->frame]);
  }
  return 1;
}

static int read_blob(const char *path, uint8_t **out, size_t *out_len){
  FILE *f=fopen(path,"rb");
  if(!f) return 0;
  fseek(f,0,SEEK_END);
  long sz=ftell(f);
  rewind(f);
  if(sz<=0){ fclose(f); return 0; }
  uint8_t *buf=(uint8_t*)malloc((size_t)sz);
  if(!buf){ fclose(f); return 0; }
  if(fread(buf,1,(size_t)sz,f)!=(size_t)sz){ fclose(f); free(buf); return 0; }
  fclose(f);
  *out=buf; *out_len=(size_t)sz;
  return 1;
}

static int write_txtr(Pkg *pkg, const GmlcProject *p, char *err, size_t errcap){
  int nf=total_sprite_frames(p);
  size_t s=chunk_begin(pkg,"TXTR");
  wu32(&pkg->b,(uint32_t)nf);
  size_t table=pkg->b.len;
  zfill(&pkg->b,(size_t)nf*4);
  size_t *blob_patch=(size_t*)calloc((size_t)(nf?nf:1),sizeof(size_t));
  if(!blob_patch) return 0;
  for(int i=0;i<nf;i++){
    patch32(&pkg->b,table+(size_t)i*4,(uint32_t)pkg->b.len);
    wu32(&pkg->b,0);
    blob_patch[i]=pkg->b.len;
    wu32(&pkg->b,0);
  }
  int gf=0;
  for(int i=0;i<p->n_sprites;i++){
    const GmlcSprite *sp=&p->sprites[i];
    for(int f=0;f<sp->n_frames;f++,gf++){
      uint8_t *blob=NULL; size_t blen=0;
      if(!sp->frame_paths || !sp->frame_paths[f] || !read_blob(sp->frame_paths[f],&blob,&blen)){
        snprintf(err,errcap,"%s: sprite frame read failed",sp->frame_paths&&sp->frame_paths[f]?sp->frame_paths[f]:"<missing>");
        free(blob_patch);
        return 0;
      }
      patch32(&pkg->b,blob_patch[gf],(uint32_t)pkg->b.len);
      wbytes(&pkg->b,blob,blen);
      free(blob);
    }
  }
  free(blob_patch);
  chunk_end(pkg,s);
  return 1;
}

static int total_object_events(const GmlcProject *p){
  int n=0;
  for(int i=0;i<p->n_objects;i++) n += p->objects[i].n_events;
  return n;
}

static int room_code_count(const GmlcProject *p){
  return p->n_rooms>0 ? p->n_rooms : 1;
}

static int script_code_index(const GmlcProject *p, int script_index){
  return room_code_count(p) + script_index;
}

static const char *event_suffix(const GmlcObjectEvent *ev, char *buf, size_t cap){
  switch(ev->event_type){
    case 0: snprintf(buf,cap,"Create_0"); break;
    case 1: snprintf(buf,cap,"Destroy_0"); break;
    case 2: snprintf(buf,cap,"Alarm_%d",ev->event_number); break;
    case 3: snprintf(buf,cap,"Step_%d",ev->event_number); break;
    case 4: snprintf(buf,cap,"Collision_%d",ev->collision_object_id); break;
    case 5: snprintf(buf,cap,"Keyboard_%d",ev->event_number); break;
    case 6: snprintf(buf,cap,"Mouse_%d",ev->event_number); break;
    case 7: snprintf(buf,cap,"Other_%d",ev->event_number); break;
    case 8: snprintf(buf,cap,"Draw_%d",ev->event_number); break;
    case 9: snprintf(buf,cap,"KeyPress_%d",ev->event_number); break;
    case 10: snprintf(buf,cap,"KeyRelease_%d",ev->event_number); break;
    default: snprintf(buf,cap,"Other_%d",ev->event_number); break;
  }
  return buf;
}

static char *object_event_code_name(const GmlcObject *obj, const GmlcObjectEvent *ev){
  char suffix[64];
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

static int write_sond(Pkg *pkg, const GmlcProject *p){
  size_t s=chunk_begin(pkg,"SOND");
  uint32_t n=(uint32_t)p->n_sounds;
  wu32(&pkg->b,n);
  size_t table=pkg->b.len;
  zfill(&pkg->b,(size_t)n*4);
  for(uint32_t i=0;i<n;i++){
    const GmlcSound *snd=&p->sounds[i];
    patch32(&pkg->b,table+i*4,(uint32_t)pkg->b.len);
    int sid_name=intern(pkg,snd->name);
    int sid_file=intern(pkg,snd->name);
    wstrptr(pkg,sid_name);
    wu32(&pkg->b,0);
    wu32(&pkg->b,0);
    wstrptr(pkg,sid_file);
    wu32(&pkg->b,0);
    wf32(&pkg->b,snd->volume);
    wf32(&pkg->b,snd->pitch==0.0f?1.0f:snd->pitch);
    wi32(&pkg->b,0);
    wi32(&pkg->b,(int32_t)i);
  }
  chunk_end(pkg,s);
  return 1;
}

static int write_scpt(Pkg *pkg, const GmlcProject *p){
  size_t s=chunk_begin(pkg,"SCPT");
  uint32_t n=(uint32_t)p->n_scripts;
  wu32(&pkg->b,n);
  size_t table=pkg->b.len;
  zfill(&pkg->b,(size_t)n*4);
  for(uint32_t i=0;i<n;i++){
    const GmlcScript *sc=&p->scripts[i];
    patch32(&pkg->b,table+i*4,(uint32_t)pkg->b.len);
    int sid=intern(pkg,sc->name);
    wstrptr(pkg,sid);
    wi32(&pkg->b,script_code_index(p,(int)i));
  }
  chunk_end(pkg,s);
  return 1;
}

static int write_shdr(Pkg *pkg, const GmlcProject *p){
  size_t s=chunk_begin(pkg,"SHDR");
  uint32_t n=(uint32_t)p->n_shaders;
  wu32(&pkg->b,n);
  size_t table=pkg->b.len;
  zfill(&pkg->b,(size_t)n*4);
  uint32_t out_i=0;
  for(int i=0;i<p->n_resources;i++){
    const GmlcResource *r=&p->resources[i];
    if(r->kind!=GMLC_RES_SHADER) continue;
    patch32(&pkg->b,table+(size_t)out_i*4,(uint32_t)pkg->b.len);
    int sid=intern(pkg,r->name?r->name:"");
    wstrptr(pkg,sid);
    wu32(&pkg->b,0x80000001u);
    wu32(&pkg->b,0);
    wu32(&pkg->b,0);
    out_i++;
  }
  chunk_end(pkg,s);
  return 1;
}

static int write_audo(Pkg *pkg, const GmlcProject *p, char *err, size_t errcap){
  size_t s=chunk_begin(pkg,"AUDO");
  uint32_t n=(uint32_t)p->n_sounds;
  wu32(&pkg->b,n);
  size_t table=pkg->b.len;
  zfill(&pkg->b,(size_t)n*4);
  for(uint32_t i=0;i<n;i++){
    const GmlcSound *snd=&p->sounds[i];
    uint8_t *blob=NULL; size_t blen=0;
    if(!snd->data_path || !read_blob(snd->data_path,&blob,&blen)){
      snprintf(err,errcap,"%s: sound blob read failed",snd->data_path?snd->data_path:"<missing>");
      return 0;
    }
    patch32(&pkg->b,table+i*4,(uint32_t)pkg->b.len);
    wu32(&pkg->b,(uint32_t)blen);
    wbytes(&pkg->b,blob,blen);
    free(blob);
  }
  chunk_end(pkg,s);
  return 1;
}

static int fixed_zero_chunk(Pkg *p, const char name[4], size_t n){
  size_t s=chunk_begin(p,name);
  if(!zfill(&p->b,n)) return 0;
  chunk_end(p,s);
  return 1;
}

static int write_agrp(Pkg *p){
  size_t s=chunk_begin(p,"AGRP");
  wu32(&p->b,1);
  wu32(&p->b,0);
  wu32(&p->b,0);
  chunk_end(p,s);
  return 1;
}

static int write_gen8(Pkg *pkg, const GmlcProject *p){
  int sid_name=intern(pkg,p->name?p->name:"source_project");
  int sid_cfg=intern(pkg,"default");
  if(sid_name<0 || sid_cfg<0) return 0;
  int dw=640, dh=480;
  if(p->n_rooms>0){ dw=p->rooms[0].port_w>0?p->rooms[0].port_w:p->rooms[0].width; dh=p->rooms[0].port_h>0?p->rooms[0].port_h:p->rooms[0].height; }
  size_t s=chunk_begin(pkg,"GEN8");
  size_t base=pkg->b.len;
  zfill(&pkg->b,128);
  pkg->b.data[base+0]=1;
  pkg->b.data[base+1]=15;
  add_str_patch(pkg,(uint32_t)(base+4),sid_name);
  add_str_patch(pkg,(uint32_t)(base+8),sid_cfg);
  patch32(&pkg->b,base+12,100000);
  patch32(&pkg->b,base+16,100000);
  patch32(&pkg->b,base+20,0);
  add_str_patch(pkg,(uint32_t)(base+44),sid_name);
  patch32(&pkg->b,base+60,(uint32_t)dw);
  patch32(&pkg->b,base+64,(uint32_t)dh);
  wu32(&pkg->b,(uint32_t)p->n_rooms);
  for(int i=0;i<p->n_rooms;i++) wu32(&pkg->b,(uint32_t)i);
  while(pkg->b.len < base+208) wu8(&pkg->b,0);
  chunk_end(pkg,s);
  return 1;
}

static int write_objt(Pkg *pkg, const GmlcProject *p){
  size_t s=chunk_begin(pkg,"OBJT");
  uint32_t n=(uint32_t)p->n_objects;
  wu32(&pkg->b,n);
  size_t table=pkg->b.len;
  zfill(&pkg->b,(size_t)n*4);
  for(uint32_t i=0;i<n;i++){
    patch32(&pkg->b,table+i*4,(uint32_t)pkg->b.len);
    const GmlcObject *o=&p->objects[i];
    int sid=intern(pkg,o->name);
    wstrptr(pkg,sid);
    wi32(&pkg->b,o->sprite_id);
    wu32(&pkg->b,(uint32_t)(o->visible?1:0));
    wu32(&pkg->b,(uint32_t)(o->solid?1:0));
    wi32(&pkg->b,0);
    wu32(&pkg->b,(uint32_t)(o->persistent?1:0));
    wi32(&pkg->b,o->parent_id>=0?o->parent_id:-100);
    wi32(&pkg->b,o->mask_id);
  }
  chunk_end(pkg,s);
  return 1;
}

static int write_room_views(Pkg *pkg, const GmlcRoom *r, uint32_t *out_ptr){
  *out_ptr=(uint32_t)pkg->b.len;
  wu32(&pkg->b,8);
  size_t table=pkg->b.len;
  zfill(&pkg->b,8*4);
  for(int i=0;i<8;i++){
    patch32(&pkg->b,table+(size_t)i*4,(uint32_t)pkg->b.len);
    int visible=(i==0 && r->view_enabled)?1:0;
    wu32(&pkg->b,(uint32_t)visible);
    wi32(&pkg->b,0); wi32(&pkg->b,0);
    wi32(&pkg->b,r->view_w>0?r->view_w:r->width);
    wi32(&pkg->b,r->view_h>0?r->view_h:r->height);
    wi32(&pkg->b,0); wi32(&pkg->b,0);
    wi32(&pkg->b,r->port_w>0?r->port_w:r->width);
    wi32(&pkg->b,r->port_h>0?r->port_h:r->height);
    wi32(&pkg->b,32); wi32(&pkg->b,32);
    wi32(&pkg->b,-1); wi32(&pkg->b,-1);
    wi32(&pkg->b,-1);
  }
  return 1;
}

static int write_room_instances(Pkg *pkg, const GmlcRoom *r, uint32_t *out_ptr){
  *out_ptr=(uint32_t)pkg->b.len;
  wu32(&pkg->b,(uint32_t)r->n_instances);
  size_t table=pkg->b.len;
  zfill(&pkg->b,(size_t)r->n_instances*4);
  for(int i=0;i<r->n_instances;i++){
    patch32(&pkg->b,table+(size_t)i*4,(uint32_t)pkg->b.len);
    const GmlcRoomInstance *in=&r->instances[i];
    wi32(&pkg->b,in->x);
    wi32(&pkg->b,in->y);
    wi32(&pkg->b,in->object_id);
    wu32(&pkg->b,(uint32_t)in->instance_id);
    wi32(&pkg->b,-1);
    wf32(&pkg->b,in->sx==0.0f?1.0f:in->sx);
    wf32(&pkg->b,in->sy==0.0f?1.0f:in->sy);
    wu32(&pkg->b,in->color?in->color:0xFFFFFFFFu);
    wf32(&pkg->b,in->rotation);
  }
  return 1;
}

static int write_room_empty_list(Pkg *pkg, uint32_t *out_ptr){
  *out_ptr=(uint32_t)pkg->b.len;
  return wu32(&pkg->b,0);
}

static int write_room(Pkg *pkg, const GmlcRoom *r, int room_index, uint32_t *record_ptr){
  *record_ptr=(uint32_t)pkg->b.len;
  int sid=intern(pkg,r->name);
  wstrptr(pkg,sid);
  wi32(&pkg->b,room_index);
  wu32(&pkg->b,(uint32_t)r->width);
  wu32(&pkg->b,(uint32_t)r->height);
  wu32(&pkg->b,(uint32_t)(r->speed>0?r->speed:60));
  wi32(&pkg->b,0);
  wu32(&pkg->b,0xFF000000u);
  wu32(&pkg->b,1);
  wi32(&pkg->b,0);
  wi32(&pkg->b,0);
  size_t bg_pos=pkg->b.len; wu32(&pkg->b,0);
  size_t view_pos=pkg->b.len; wu32(&pkg->b,0);
  size_t obj_pos=pkg->b.len; wu32(&pkg->b,0);
  size_t tile_pos=pkg->b.len; wu32(&pkg->b,0);
  uint32_t bg=0, view=0, obj=0, tile=0;
  write_room_empty_list(pkg,&bg);
  write_room_views(pkg,r,&view);
  write_room_instances(pkg,r,&obj);
  write_room_empty_list(pkg,&tile);
  patch32(&pkg->b,bg_pos,bg);
  patch32(&pkg->b,view_pos,view);
  patch32(&pkg->b,obj_pos,obj);
  patch32(&pkg->b,tile_pos,tile);
  return 1;
}

static int write_room_chunk(Pkg *pkg, const GmlcProject *p){
  size_t s=chunk_begin(pkg,"ROOM");
  uint32_t n=(uint32_t)p->n_rooms;
  wu32(&pkg->b,n);
  size_t table=pkg->b.len;
  zfill(&pkg->b,(size_t)n*4);
  for(uint32_t i=0;i<n;i++){
    uint32_t ptr=0;
    write_room(pkg,&p->rooms[i],(int)i,&ptr);
    patch32(&pkg->b,table+i*4,ptr);
  }
  chunk_end(pkg,s);
  return 1;
}

static int write_strg(Pkg *pkg){
  size_t s=chunk_begin(pkg,"STRG");
  wu32(&pkg->b,(uint32_t)pkg->strs.n);
  size_t table=pkg->b.len;
  zfill(&pkg->b,(size_t)pkg->strs.n*4);
  for(int i=0;i<pkg->strs.n;i++){
    uint32_t len=(uint32_t)strlen(pkg->strs.items[i]);
    patch32(&pkg->b,table+(size_t)i*4,(uint32_t)pkg->b.len);
    wu32(&pkg->b,len);
    pkg->strs.char_off[i]=(uint32_t)pkg->b.len;
    wbytes(&pkg->b,pkg->strs.items[i],len);
    wu8(&pkg->b,0);
  }
  chunk_end(pkg,s);
  for(int i=0;i<pkg->n_patches;i++){
    StrPatch *sp=&pkg->patches[i];
    if(sp->sid>=0 && sp->sid<pkg->strs.n) patch32(&pkg->b,sp->pos,pkg->strs.char_off[sp->sid]);
  }
  return 1;
}

static int write_one_code_blob(Pkg *pkg, int sid, GmlcCodeBlob *blob, char *err, size_t errcap){
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
  wstrptr(pkg,sid);
  wu32(&pkg->b,(uint32_t)blob->size);
  wu32(&pkg->b,0);
  wi32(&pkg->b,8);
  wu32(&pkg->b,0);
  uint32_t code_start=(uint32_t)pkg->b.len;
  if(!wbytes(&pkg->b,blob->data,blob->size)) return 0;
  for(int i=0;i<blob->n_refs;i++) if(!add_code_ref(pkg,&blob->refs[i],code_start)) return 0;
  return 1;
}

static int compile_code_blob(Pkg *pkg, const GmlcProject *p, const char *path, GmlcCodeBlob *blob){
  memset(blob,0,sizeof(*blob));
  if(path && *path){
    char berr[512]={0};
    if(gmlc_bytecode_compile_source(p,path,blob,berr,sizeof(berr))){
      if(blob->is_placeholder){
        pkg->placeholder_code++;
        if(blob->diagnostic)
          fprintf(stderr,"source_to_win: code placeholder: %s: %s\n",path,blob->diagnostic);
      } else {
        pkg->compiled_code++;
      }
      return 1;
    }
    if(!gmlc_bytecode_emit_empty(blob)) return 0;
    blob->diagnostic=gmlc_strdup(berr[0]?berr:"source read failed");
    pkg->placeholder_code++;
    fprintf(stderr,"source_to_win: code placeholder: %s: %s\n",path,blob->diagnostic?blob->diagnostic:"source read failed");
    return 1;
  }
  if(!gmlc_bytecode_emit_empty(blob)) return 0;
  pkg->placeholder_code++;
  return 1;
}

static int write_compiled_code_entry(Pkg *pkg, const GmlcProject *p, size_t table, int *ci, int sid, const char *path, char *err, size_t errcap){
  GmlcCodeBlob blob;
  if(!compile_code_blob(pkg,p,path,&blob)) return 0;
  patch32(&pkg->b,table+(size_t)(*ci)++*4,(uint32_t)pkg->b.len);
  int ok=write_one_code_blob(pkg,sid,&blob,err,errcap);
  gmlc_bytecode_free(&blob);
  return ok;
}

static int write_code(Pkg *pkg, const GmlcProject *p, char *err, size_t errcap){
  size_t s=chunk_begin(pkg,"CODE");
  int count=room_code_count(p) + p->n_scripts + total_object_events(p);
  wu32(&pkg->b,(uint32_t)count);
  size_t table=pkg->b.len;
  zfill(&pkg->b,(size_t)count*4);
  int ci=0;
  for(int i=0;i<room_code_count(p);i++){
    char name[64];
    snprintf(name,sizeof(name),"gml_RoomCC_%d",i);
    int sid=intern(pkg,name);
    const char *path=(i<p->n_rooms)?p->rooms[i].creation_code_path:NULL;
    if(!write_compiled_code_entry(pkg,p,table,&ci,sid,path,err,errcap)) return 0;
  }
  for(int i=0;i<p->n_scripts;i++){
    char *name=script_code_name(&p->scripts[i]);
    int nsid=intern(pkg,name?name:"");
    free(name);
    if(!write_compiled_code_entry(pkg,p,table,&ci,nsid,p->scripts[i].source_path,err,errcap)) return 0;
  }
  for(int oi=0;oi<p->n_objects;oi++){
    const GmlcObject *obj=&p->objects[oi];
    for(int ei=0;ei<obj->n_events;ei++){
      char *name=object_event_code_name(obj,&obj->events[ei]);
      int nsid=intern(pkg,name?name:"");
      free(name);
      if(!write_compiled_code_entry(pkg,p,table,&ci,nsid,obj->events[ei].source_path,err,errcap)) return 0;
    }
  }
  if(!patch_ref_chains(pkg)){
    snprintf(err,errcap,"reference chain patch failed");
    return 0;
  }
  chunk_end(pkg,s);
  return 1;
}

static int write_vari(Pkg *pkg){
  if(!patch_ref_chains(pkg)) return 0;
  size_t s=chunk_begin(pkg,"VARI");
  zfill(&pkg->b,12);
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
  chunk_end(pkg,s);
  return 1;
}

static int write_func(Pkg *pkg){
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
  chunk_end(pkg,s);
  return 1;
}

static int write_file(const char *path, const uint8_t *data, size_t len, char *err, size_t errcap){
  FILE *f=fopen(path,"wb");
  if(!f){ snprintf(err,errcap,"%s: open for write failed",path); return 0; }
  if(fwrite(data,1,len,f)!=len){ fclose(f); snprintf(err,errcap,"%s: write failed",path); return 0; }
  fclose(f);
  return 1;
}

int gmlc_package_write_structural(const GmlcProject *p, const char *out_path, char *err, size_t errcap){
  Pkg pkg;
  memset(&pkg,0,sizeof(pkg));
  wbytes(&pkg.b,"FORM",4);
  size_t form_size_pos=pkg.b.len;
  wu32(&pkg.b,0);
  if(!write_gen8(&pkg,p) ||
     !fixed_zero_chunk(&pkg,"OPTN",80) ||
     !fixed_zero_chunk(&pkg,"LANG",12) ||
     !empty_list_chunk(&pkg,"EXTN") ||
     !write_sond(&pkg,p) ||
     !write_agrp(&pkg) ||
     !write_sprt(&pkg,p) ||
     !empty_list_chunk(&pkg,"BGND") ||
     !empty_list_chunk(&pkg,"PATH") ||
     !write_scpt(&pkg,p) ||
     !empty_list_chunk(&pkg,"GLOB") ||
     !write_shdr(&pkg,p) ||
     !empty_list_chunk(&pkg,"FONT") ||
     !empty_list_chunk(&pkg,"TMLN") ||
     !write_objt(&pkg,p) ||
     !write_room_chunk(&pkg,p) ||
     !fixed_zero_chunk(&pkg,"DAFL",0) ||
     !fixed_zero_chunk(&pkg,"EMBI",8) ||
     !write_tpag(&pkg,p) ||
     !write_code(&pkg,p,err,errcap) ||
     !write_vari(&pkg) ||
     !write_func(&pkg) ||
     !write_strg(&pkg) ||
     !write_txtr(&pkg,p,err,errcap) ||
     !write_audo(&pkg,p,err,errcap)){
    if(!err[0]) snprintf(err,errcap,"out of memory while writing package");
    free(pkg.b.data);
    for(int i=0;i<pkg.strs.n;i++) free(pkg.strs.items[i]);
    free(pkg.strs.items); free(pkg.strs.char_off); free(pkg.patches);
    free(pkg.frame_patches); free(pkg.frame_tpag_ptr);
    for(int i=0;i<pkg.n_code_refs;i++) free(pkg.code_refs[i].name);
    free(pkg.code_refs);
    return 0;
  }
  patch32(&pkg.b,form_size_pos,(uint32_t)(pkg.b.len-8));
  int ok=write_file(out_path,pkg.b.data,pkg.b.len,err,errcap);
  free(pkg.b.data);
  for(int i=0;i<pkg.strs.n;i++) free(pkg.strs.items[i]);
  free(pkg.strs.items); free(pkg.strs.char_off); free(pkg.patches);
  free(pkg.frame_patches); free(pkg.frame_tpag_ptr);
  for(int i=0;i<pkg.n_code_refs;i++) free(pkg.code_refs[i].name);
  free(pkg.code_refs);
  return ok;
}
