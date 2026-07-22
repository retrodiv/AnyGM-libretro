/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* gml_win.c - GameMaker data.win FORM loader. Bytecode-version details live in gml_bc*.c. */
#include "gml_win.h"
#include "gml_bytecode.h"
#include "anygm_vfs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static uint32_t u32(const uint8_t *d, uint32_t o){
  return (uint32_t)d[o] | (uint32_t)d[o+1]<<8 | (uint32_t)d[o+2]<<16 | (uint32_t)d[o+3]<<24;
}

static int size_mul(size_t a,size_t b,size_t *out){
  if(!out || (a && b>SIZE_MAX/a)) return 0;
  *out=a*b;
  return 1;
}

static int span_has(size_t size,size_t offset,size_t length){
  return offset<=size && length<=size-offset;
}

static int win_has(const GmlWin *w,size_t offset,size_t length){
  return w && w->data && span_has(w->size,offset,length);
}

static int chunk_has(const GmlWin *w,const GmlChunk *chunk,size_t relative,size_t length){
  return chunk && relative<=chunk->size && length<=chunk->size-relative &&
         win_has(w,(size_t)chunk->off+relative,length);
}

static int chunk_read_u32(const GmlWin *w,const GmlChunk *chunk,size_t relative,
                          uint32_t *value){
  if(!value || !chunk_has(w,chunk,relative,4)) return 0;
  *value=u32(w->data,(uint32_t)((size_t)chunk->off+relative));
  return 1;
}

static int string_by_pointer(const GmlWin *w,uint32_t offset,const char **value){
  if(value) *value=NULL;
  if(!w || !value || w->n_strs<=0 || !w->strs || !w->str_charoff) return 0;
  int lo=0,hi=w->n_strs-1;
  while(lo<=hi){
    int middle=lo+(hi-lo)/2;
    if(w->str_charoff[middle]==offset){ *value=w->strs[middle]; return 1; }
    if(w->str_charoff[middle]<offset) lo=middle+1;
    else hi=middle-1;
  }
  return 0;
}

/* A failed memory parse leaves ownership with the caller, even when ownership
 * would have transferred after a successful parse. */
static int discard_partial_win(GmlWin *w){
  if(w){ w->owns=0; gml_win_free(w); }
  return -1;
}

/* ---------------- loader ---------------- */
const GmlChunk *gml_chunk(const GmlWin *w, const char *name){
  if(!w || !name) return NULL;
  for(int i=0;i<w->n_chunks;i++) if(!memcmp(w->chunks[i].name,name,4)) return &w->chunks[i];
  return NULL;
}
const char *gml_str_by_index(const GmlWin *w, uint32_t idx){
  return (w && w->strs && idx<(uint32_t)w->n_strs)?w->strs[idx]:"<#?>";
}
const char *gml_str_by_ptr(const GmlWin *w, uint32_t off){
  const char *value=NULL;
  return string_by_pointer(w,off,&value)?value:"<@?>";
}
const char *gml_ref_name(const GmlWin *w, uint32_t addr){
  if(!w) return "?";
  GmlWin *mw=(GmlWin*)w;
  if(!w->ref_hix && w->n_refs>0){
    uint32_t cap=1;
    while(cap < (uint32_t)w->n_refs*2u) cap<<=1;
    mw->ref_hix=malloc((size_t)cap*sizeof(int32_t));
    if(mw->ref_hix){
      for(uint32_t i=0;i<cap;i++) mw->ref_hix[i]=-1;
      mw->ref_hix_cap=cap;
      for(int i=0;i<w->n_refs;i++){
        uint32_t h=(w->ref_addr[i]*2654435761u)&(cap-1);
        while(mw->ref_hix[h]>=0){
          if(w->ref_addr[mw->ref_hix[h]]==w->ref_addr[i]) break; /* preserve first duplicate */
          h=(h+1)&(cap-1);
        }
        if(mw->ref_hix[h]<0) mw->ref_hix[h]=i;
      }
    }
  }
  if(w->ref_hix && w->ref_hix_cap){
    uint32_t h=(addr*2654435761u)&(w->ref_hix_cap-1);
    for(uint32_t probe=0; probe<w->ref_hix_cap; probe++){
      int32_t i=w->ref_hix[h];
      if(i<0) return "?";
      if(i<w->n_refs && w->ref_addr[i]==addr) return w->ref_name[i];
      h=(h+1)&(w->ref_hix_cap-1);
    }
    return "?";
  }
  int lo=0,hi=w->n_refs-1;
  while(lo<=hi){int m=(lo+hi)/2; if(w->ref_addr[m]==addr)return w->ref_name[m];
    if(w->ref_addr[m]<addr)lo=m+1;else hi=m-1;}
  return "?";
}


/* O(1) content lookup over the STRG table (lazy open-addressed index). Returns the interned
 * pointer (w->strs[i]) or NULL. State loads intern many runtime strings, so a linear STRG scan
 * for every string scales poorly. */
static uint32_t win_strhash(const char *s){
  uint32_t h=2166136261u; while(*s){ h^=(uint8_t)*s++; h*=16777619u; } return h;
}
const char *gml_win_intern_lookup(GmlWin *w, const char *s){
  if(!w || !s || w->n_strs<=0) return NULL;
  if(!w->str_hix){
    uint32_t cap=1; while(cap < (uint32_t)w->n_strs*2u) cap<<=1;
    w->str_hix=malloc((size_t)cap*sizeof(int32_t));
    if(!w->str_hix) return NULL;
    for(uint32_t i=0;i<cap;i++) w->str_hix[i]=-1;
    w->str_hix_cap=cap;
    for(int i=0;i<w->n_strs;i++){
      if(!w->strs[i]) continue;
      uint32_t h=win_strhash(w->strs[i]) & (cap-1);
      while(w->str_hix[h]>=0) h=(h+1)&(cap-1);
      w->str_hix[h]=i;
    }
  }
  uint32_t h=win_strhash(s) & (w->str_hix_cap-1);
  for(uint32_t probe=0; probe<w->str_hix_cap; probe++){
    int32_t i=w->str_hix[h];
    if(i<0) return NULL;
    if(w->strs[i] && !strcmp(w->strs[i],s)) return w->strs[i];
    h=(h+1)&(w->str_hix_cap-1);
  }
  return NULL;
}

static int parse_strg(GmlWin *w){
  const GmlChunk *c=gml_chunk(w,"STRG");
  if(!c) return 1;
  uint32_t count=0;
  size_t table_bytes=0,allocation_bytes=0;
  if(!chunk_read_u32(w,c,0,&count) || count>GML_WIN_MAX_STRINGS ||
     !size_mul((size_t)count,4,&table_bytes) ||
     !chunk_has(w,c,4,table_bytes)) return 0;
  if(count){
    if(!size_mul((size_t)count,sizeof(*w->strs),&allocation_bytes)) return 0;
    w->strs=calloc(1,allocation_bytes);
    if(!size_mul((size_t)count,sizeof(*w->str_charoff),&allocation_bytes)) return 0;
    w->str_charoff=calloc(1,allocation_bytes);
    if(!w->strs || !w->str_charoff) return 0;
  }
  for(uint32_t i=0;i<count;i++){
    uint32_t record=u32(w->data,c->off+4+i*4u),length=0;
    size_t character_offset=(size_t)record+4u;
    if(record<(size_t)c->off+4u+table_bytes || !win_has(w,record,4) ||
       !chunk_has(w,c,(size_t)record-c->off,4) ||
       !chunk_read_u32(w,c,(size_t)record-c->off,&length) ||
       length>GML_WIN_MAX_STRING_BYTES ||
       !chunk_has(w,c,character_offset-c->off,(size_t)length+1u) ||
       w->data[character_offset+length]!=0 ||
       (length && memchr(w->data+character_offset,0,length)) ||
       character_offset>UINT32_MAX ||
       (i && character_offset<w->str_charoff[i-1])) return 0;
    w->str_charoff[i]=(uint32_t)character_offset;
    w->strs[i]=(char*)(w->data+character_offset);
  }
  w->n_strs=(int)count;
  return 1;
}

static int parse_code(GmlWin *w){
  const GmlChunk *c=gml_chunk(w,"CODE");
  if(!c) return 1;
  uint32_t count=0;
  size_t table_bytes=0,allocation_bytes=0;
  if(!chunk_read_u32(w,c,0,&count) || count>GML_WIN_MAX_CODE_ENTRIES ||
     !size_mul((size_t)count,4,&table_bytes) || !chunk_has(w,c,4,table_bytes)) return 0;
  if(count){
    if(!size_mul((size_t)count,sizeof(*w->code),&allocation_bytes)) return 0;
    w->code=calloc(1,allocation_bytes);
    if(!w->code) return 0;
  }
  w->n_code=(int)count;
  for(uint32_t i=0;i<count;i++){
    uint32_t p=u32(w->data,c->off+4+i*4u);
    size_t record_size=w->bytecode>=15?20u:8u;
    const char *name=NULL;
    if(p<(size_t)c->off+4u+table_bytes ||
       !chunk_has(w,c,(size_t)p-c->off,record_size) ||
       !string_by_pointer(w,u32(w->data,p),&name)) return 0;
    w->code[i].name=name;
    uint32_t serialized_length=u32(w->data,p+4);
    w->code[i].length=serialized_length;
    /* CODE-v2 child functions share their parent's bytecode span. Their serialized Length is the
     * complete parent span while Offset selects the child's first instruction, so the callable
     * suffix ends at Length-Offset rather than extending that full length again. */
    if(w->bytecode>=15){
      uint32_t code_offset=u32(w->data,p+16);
      if(code_offset>serialized_length) return 0;
      w->code[i].length-=code_offset;
    }
    if(!gml_bc_code_start(w,p,&w->code[i].start) ||
       w->code[i].start<(size_t)c->off+4u+table_bytes ||
       w->code[i].start>(size_t)c->off+c->size ||
       w->code[i].length>(size_t)c->off+c->size-w->code[i].start) return 0;
  }
  return 1;
}

typedef struct { uint32_t addr; const char *name; } RefRec;
static int cmp_ref_addr(const void *A, const void *B){
  uint32_t a=((const RefRec*)A)->addr, b=((const RefRec*)B)->addr;
  return a<b?-1:(a>b?1:0);
}
static int ref_layout_valid(const GmlWin *w,const GmlRefLayout *layout){
  if(!w || !layout || !layout->stride || layout->count>GML_WIN_MAX_REFERENCES) return 0;
  if(layout->occ_off>layout->stride || 4u>layout->stride-layout->occ_off ||
     layout->addr_off>layout->stride || 4u>layout->stride-layout->addr_off ||
     4u>layout->stride) return 0;
  size_t bytes=0;
  return size_mul((size_t)layout->count,layout->stride,&bytes) &&
         win_has(w,layout->start,bytes);
}

/* Walk VARI+FUNC occurrence chains -> ref_addr/ref_name map. */
static int parse_refs(GmlWin *w){
  /* count total occurrences first */
  uint32_t total=0;
  const char *chunks[2]={"VARI","FUNC"};
  for(int ci=0;ci<2;ci++){
    GmlRefLayout l;
    if(!gml_chunk(w,chunks[ci])) continue;
    if(!gml_bc_ref_layout(w,chunks[ci],&l) || !ref_layout_valid(w,&l)) return 0;
    for(uint32_t i=0;i<l.count;i++){
      uint32_t o=l.start+i*l.stride;
      uint32_t occ=u32(w->data,o+l.occ_off);
      if(occ>GML_WIN_MAX_REFERENCES-total) return 0;
      total += occ;
    }
  }
  if(total){
    size_t allocation_bytes=0;
    if(!size_mul((size_t)total,sizeof(*w->ref_addr),&allocation_bytes)) return 0;
    w->ref_addr=calloc(1,allocation_bytes);
    if(!size_mul((size_t)total,sizeof(*w->ref_name),&allocation_bytes)) return 0;
    w->ref_name=calloc(1,allocation_bytes);
    if(!w->ref_addr || !w->ref_name) return 0;
  }
  uint32_t n=0;
  for(int ci=0;ci<2;ci++){
    GmlRefLayout l;
    if(!gml_chunk(w,chunks[ci])) continue;
    if(!gml_bc_ref_layout(w,chunks[ci],&l) || !ref_layout_valid(w,&l)) return 0;
    for(uint32_t i=0;i<l.count;i++){
      uint32_t o=l.start+i*l.stride;
      const char *nm=NULL;
      if(!string_by_pointer(w,u32(w->data,o),&nm)) return 0;
      uint32_t occ=u32(w->data,o+l.occ_off), addr=u32(w->data,o+l.addr_off);
      for(uint32_t k=0;k<occ;k++){
        if(n>=total || addr==0 || l.ref_off>UINT32_MAX-addr ||
           l.chain_off>UINT32_MAX-addr ||
           !win_has(w,(size_t)addr+l.ref_off,4) ||
           !win_has(w,(size_t)addr+l.chain_off,4)) return 0;
        w->ref_addr[n]=addr+l.ref_off; w->ref_name[n]=nm; n++;
        /* The key is the reference-word address, matching GmlInsn.refaddr. */
        uint32_t ref=u32(w->data,addr+l.chain_off);
        uint32_t nxt=ref & 0x07FFFFFF;
        if(k+1u<occ){
          if(!nxt || nxt>UINT32_MAX-addr) return 0;
          addr+=nxt;
        }
      }
    }
  }
  if(n!=total) return 0;
  w->n_refs=(int)n;
  /* Sort by address for binary search; addresses are unique per site. Sorting records avoids
   * the quadratic behavior of parallel-array insertion when files contain many references. */
  if(n>1){
    size_t allocation_bytes=0;
    if(!size_mul((size_t)n,sizeof(RefRec),&allocation_bytes)) return 0;
    RefRec *rec=malloc(allocation_bytes);
    if(!rec) return 0;
    for(uint32_t i=0;i<n;i++){ rec[i].addr=w->ref_addr[i]; rec[i].name=w->ref_name[i]; }
    qsort(rec,(size_t)n,sizeof(*rec),cmp_ref_addr);
    for(uint32_t i=1;i<n;i++) if(rec[i-1].addr==rec[i].addr){ free(rec); return 0; }
    for(uint32_t i=0;i<n;i++){ w->ref_addr[i]=rec[i].addr; w->ref_name[i]=rec[i].name; }
    free(rec);
  }
  return 1;
}

int gml_room_count(const GmlWin *w){
  const GmlChunk *c=gml_chunk(w,"ROOM");
  uint32_t count=0;
  return c && chunk_read_u32(w,c,0,&count) && count<=GML_WIN_MAX_ROOMS?(int)count:0;
}
int gml_room_get(const GmlWin *w, int idx, GmlRoom *o){
  const GmlChunk *c=gml_chunk(w,"ROOM");
  uint32_t n=0;
  if(!c || !o || !chunk_read_u32(w,c,0,&n) || n>GML_WIN_MAX_ROOMS ||
     idx<0 || (uint32_t)idx>=n || !chunk_has(w,c,4,(size_t)n*4u)) return -1;
  uint32_t p=u32(w->data,c->off+4+(uint32_t)idx*4u);
  const uint8_t *d=w->data;
  const char *name=NULL;
  if(p<(size_t)c->off+4u+(size_t)n*4u || !chunk_has(w,c,(size_t)p-c->off,56) ||
     !string_by_pointer(w,u32(d,p),&name)) return -1;
  o->name=name;
  o->width=u32(d,p+8); o->height=u32(d,p+12); o->speed=u32(d,p+16);
  o->persistent=(int)u32(d,p+20);
  o->bgcolor=u32(d,p+24); o->draw_bg=(int)u32(d,p+28);
  o->creation_code=(int)u32(d,p+32);
  o->flags=u32(d,p+36); o->view_enabled=(o->flags & 1u)!=0;
  o->bg_ptr=u32(d,p+40); o->view_ptr=u32(d,p+44);
  o->obj_ptr=u32(d,p+48); o->tile_ptr=u32(d,p+52);
  return 0;
}

static int room_table_valid(const GmlWin *w){
  const GmlChunk *c=gml_chunk(w,"ROOM");
  if(!c) return 1;
  uint32_t count=0;
  size_t table_bytes=0;
  if(!chunk_read_u32(w,c,0,&count) || count>GML_WIN_MAX_ROOMS ||
     !size_mul((size_t)count,4,&table_bytes) || !chunk_has(w,c,4,table_bytes)) return 0;
  for(uint32_t i=0;i<count;i++){
    uint32_t record=u32(w->data,c->off+4+i*4u);
    const char *name=NULL;
    if(record<(size_t)c->off+4u+table_bytes ||
       !chunk_has(w,c,(size_t)record-c->off,56) ||
       !string_by_pointer(w,u32(w->data,record),&name)) return 0;
  }
  return 1;
}

static int room_order_candidate(const GmlWin *w,const GmlChunk *gen8,size_t relative,
                                uint32_t room_count,uint32_t *count){
  size_t table_bytes=0;
  if(!chunk_read_u32(w,gen8,relative,count) || *count>GML_WIN_MAX_ROOM_ORDER ||
     *count>room_count || (*count==0 && room_count>0) ||
     !size_mul((size_t)*count,4,&table_bytes) ||
     !chunk_has(w,gen8,relative+4u,table_bytes)) return 0;
  for(uint32_t i=0;i<*count;i++)
    if(u32(w->data,(uint32_t)((size_t)gen8->off+relative+4u+(size_t)i*4u))>=room_count)
      return 0;
  return 1;
}

static int parse_gen8(GmlWin *w){
  const GmlChunk *gen8=gml_chunk(w,"GEN8");
  if(!gen8) return 1;
  if(!chunk_has(w,gen8,0,68)) return 0;
  w->bytecode=w->data[gen8->off+1];
  w->gameid=u32(w->data,gen8->off+20);
  w->disp_w=u32(w->data,gen8->off+60);
  w->disp_h=u32(w->data,gen8->off+64);

  uint32_t room_count=(uint32_t)gml_room_count(w),count=0;
  size_t order_offset=128;
  int have_order=room_order_candidate(w,gen8,order_offset,room_count,&count);
  if(!have_order){
    order_offset=124;
    have_order=room_order_candidate(w,gen8,order_offset,room_count,&count);
  }
  if(!have_order) return 0;
  if(count){
    size_t allocation_bytes=0;
    if(!size_mul((size_t)count,sizeof(*w->room_order),&allocation_bytes)) return 0;
    w->room_order=malloc(allocation_bytes);
    if(!w->room_order) return 0;
    for(uint32_t i=0;i<count;i++)
      w->room_order[i]=u32(w->data,(uint32_t)((size_t)gen8->off+order_offset+4u+(size_t)i*4u));
  }
  w->n_room_order=(int)count;

  /* Modern packages store the global cadence after RoomOrder and a fixed project-hash block.
   * Bounds and range checks leave earlier layouts at their ordinary room-speed fallback. */
  size_t speed_relative=order_offset+4u+(size_t)count*4u+40u;
  if(chunk_has(w,gen8,speed_relative,4)){
    uint32_t bits=u32(w->data,(uint32_t)((size_t)gen8->off+speed_relative));
    float speed=0.0f;
    memcpy(&speed,&bits,sizeof speed);
    if(isfinite(speed) && speed>=1.0f && speed<=1000.0f) w->game_speed=speed;
  }
  return 1;
}

int gml_win_from_mem(GmlWin *w, uint8_t *data, size_t size, int owns){
  if(!w) return -1;
  memset(w,0,sizeof(*w));
  if(!data || size<8 || size>GML_WIN_MAX_FILE_BYTES || memcmp(data,"FORM",4)) return -1;
  w->data=data; w->size=size; w->owns=owns;
  uint32_t total=u32(data,4);
  if((size_t)total!=size-8u) return discard_partial_win(w);
  size_t offset=8,end=size;
  while(offset<end){
    if(!span_has(end,offset,8) || w->n_chunks>=(int)GML_WIN_MAX_CHUNKS)
      return discard_partial_win(w);
    uint32_t chunk_size=u32(data,(uint32_t)offset+4u);
    size_t body=offset+8u;
    if(!span_has(end,body,chunk_size)) return discard_partial_win(w);
    for(int i=0;i<w->n_chunks;i++)
      if(!memcmp(w->chunks[i].name,data+offset,4)) return discard_partial_win(w);
    GmlChunk *c=&w->chunks[w->n_chunks++];
    memcpy(c->name,data+offset,4); c->name[4]=0;
    c->size=chunk_size; c->off=(uint32_t)body;
    offset=body+chunk_size;
  }
  if(offset!=end || !parse_strg(w) || !room_table_valid(w) || !parse_gen8(w))
    return discard_partial_win(w);
  const GmlChunk *opt=gml_chunk(w,"OPTN");
  if(opt && opt->size>=16 && u32(data,opt->off)==0x80000000u){
    w->option_flags=(uint64_t)u32(data,opt->off+8) |
                    (uint64_t)u32(data,opt->off+12)<<32;
  }
  const GmlChunk *classic=gml_chunk(w,"CLSC");
  if(classic && classic->size>=4){
    w->classic_version=(int)u32(data,classic->off);
    if(classic->size>=8) w->classic_scaling=(int32_t)u32(data,classic->off+4);
    if(classic->size>=12) w->classic_interpolate=(int)u32(data,classic->off+8);
    if(classic->size>=16) w->classic_outside_color=u32(data,classic->off+12);
    if(classic->size>=20) w->classic_swap_creation_events=(int)u32(data,classic->off+16);
    if(classic->size>=24){
      uint32_t information_size=u32(data,classic->off+20);
      if(information_size>GML_WIN_MAX_CLASSIC_INFO_BYTES || information_size>classic->size-24u)
        return discard_partial_win(w);
      w->classic_game_information=data+classic->off+24;
      w->classic_game_information_size=information_size;
      size_t provenance=24u+(size_t)information_size;
      if(provenance+4u<=classic->size)
        w->classic_executable_layout=(int)u32(data,(uint32_t)((size_t)classic->off+provenance))!=0;
    }
  }
  if(!parse_code(w) || !parse_refs(w)) return discard_partial_win(w);
  return 0;
}

int gml_win_load_host(GmlWin *w,const struct AnygmHostServices *host,const char *path){
  uint8_t *buf=NULL;
  size_t sz=0;
  void *mapping=NULL;
  int storage=0;
  if(!w || !host || !path) return -1;
  if(host->file_map && host->file_unmap){
    const void *mapped_data=NULL;
    mapping=host->file_map(host->userdata,path,&mapped_data,&sz);
    if(mapping){
      if(!mapped_data || !sz || sz>GML_WIN_MAX_FILE_BYTES){
        host->file_unmap(host->userdata,mapping,mapped_data,sz);
        return -1;
      }
      buf=(uint8_t *)(uintptr_t)mapped_data;
      storage=2;
    }
  }
  if(!buf){
    if(!anygm_vfs_read_all(host,path,&buf,&sz,GML_WIN_MAX_FILE_BYTES)) return -1;
    storage=1;
  }
  int rc=gml_win_from_mem(w,buf,sz,storage);
  if(rc!=0){
    if(storage==2) host->file_unmap(host->userdata,mapping,buf,sz);
    else free(buf);
    memset(w,0,sizeof(*w));
    return rc;
  }
  w->host=host;
  w->mapping_handle=mapping;
  if(rc==0 && path){
    const char *slash=strrchr(path,'/');
    const char *bslash=strrchr(path,'\\');
    if(bslash && (!slash || bslash>slash)) slash=bslash;
    if(slash){
      size_t n=(size_t)(slash-path);
      if(n>=sizeof(w->content_dir)) n=sizeof(w->content_dir)-1;
      memcpy(w->content_dir,path,n);
      w->content_dir[n]=0;
    } else {
      snprintf(w->content_dir,sizeof(w->content_dir),".");
    }
  }
  return rc;
}

void gml_win_free(GmlWin *w){
  if(!w) return;
  if(w->code){
    for(int i=0;i<w->n_code;i++){
      free(w->code[i].insn);
      free(w->code[i].insn_pc);
      free(w->code[i].branch_index);
    }
  }
  free(w->strs); free(w->str_charoff); free(w->code);
  free(w->str_hix);
  free(w->code_hix);
  free(w->ref_hix);
  free(w->ref_addr); free(w->ref_name); free(w->room_order);
  if(w->owns==1) free(w->data);
  else if(w->owns==2 && w->mapping_handle && w->host && w->host->file_unmap)
    w->host->file_unmap(w->host->userdata,w->mapping_handle,w->data,w->size);
  memset(w,0,sizeof(*w));
}
