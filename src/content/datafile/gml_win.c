/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me> */
/* gml_win.c - FORM container loader; versioned bytecode layouts live in gml_bc*.c. */
#include "gml_win.h"
#include "gml_bytecode.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

static uint32_t u32(const uint8_t *d, uint32_t o){
  return (uint32_t)d[o] | (uint32_t)d[o+1]<<8 | (uint32_t)d[o+2]<<16 | (uint32_t)d[o+3]<<24;
}

/* ---------------- loader ---------------- */
const GmlChunk *gml_chunk(const GmlWin *w, const char *name){
  for(int i=0;i<w->n_chunks;i++) if(!strncmp(w->chunks[i].name,name,4)) return &w->chunks[i];
  return NULL;
}
const char *gml_str_by_index(const GmlWin *w, uint32_t idx){
  return (idx<(uint32_t)w->n_strs)?w->strs[idx]:"<#?>";
}
const char *gml_str_by_ptr(const GmlWin *w, uint32_t off){
  /* str_charoff is ascending; binary search */
  int lo=0,hi=w->n_strs-1;
  while(lo<=hi){int m=(lo+hi)/2; if(w->str_charoff[m]==off)return w->strs[m];
    if(w->str_charoff[m]<off)lo=m+1;else hi=m-1;}
  return "<@?>";
}
const char *gml_ref_name(const GmlWin *w, uint32_t addr){
  GmlWin *mw=(GmlWin*)w;
  if(w && !w->ref_hix && w->n_refs>0){
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
  if(w && w->ref_hix && w->ref_hix_cap){
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


/* Lazily build an open-addressed STRG index. Return the interned pointer
 * on a matching string, or NULL when absent or the index cannot be allocated. */
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

static void parse_strg(GmlWin *w){
  const GmlChunk *c=gml_chunk(w,"STRG"); if(!c)return;
  uint32_t off=c->off, count=u32(w->data,off);
  w->n_strs=(int)count;
  w->strs=calloc(count,sizeof(char*));
  w->str_charoff=calloc(count,sizeof(uint32_t));
  for(uint32_t i=0;i<count;i++){
    uint32_t p=u32(w->data,off+4+i*4);
    uint32_t ln=u32(w->data,p);
    w->str_charoff[i]=p+4;
    w->strs[i]=(char*)(w->data+p+4); /* NUL-terminated in file; point in place */
    (void)ln;
  }
}

static void parse_code(GmlWin *w){
  const GmlChunk *c=gml_chunk(w,"CODE"); if(!c)return;
  uint32_t off=c->off, count=u32(w->data,off);
  w->n_code=(int)count; w->code=calloc(count,sizeof(GmlCode));
  for(uint32_t i=0;i<count;i++){
    uint32_t p=u32(w->data,off+4+i*4);
    w->code[i].name=gml_str_by_ptr(w,u32(w->data,p));
    w->code[i].length=u32(w->data,p+4);
    /* CODE-v2 child functions share their parent's bytecode span. Their serialized Length is the
     * complete parent span while Offset selects the child's first instruction, so the callable
     * suffix ends at Length-Offset rather than extending that full length again. */
    if(w->bytecode>=15 && p<=w->size && w->size-p>=20){
      uint32_t code_offset=u32(w->data,p+16);
      if(code_offset<=w->code[i].length) w->code[i].length-=code_offset;
      else w->code[i].length=0;
    }
    if(!gml_bc_code_start(w,p,&w->code[i].start)) w->code[i].start=0;
  }
}

typedef struct { uint32_t addr; const char *name; } RefRec;
static int cmp_ref_addr(const void *A, const void *B){
  uint32_t a=((const RefRec*)A)->addr, b=((const RefRec*)B)->addr;
  return a<b?-1:(a>b?1:0);
}
/* Walk VARI+FUNC occurrence chains -> ref_addr/ref_name map. */
static void parse_refs(GmlWin *w){
  /* count total occurrences first */
  uint32_t total=0;
  const char *chunks[2]={"VARI","FUNC"};
  for(int ci=0;ci<2;ci++){
    GmlRefLayout l;
    if(!gml_bc_ref_layout(w,chunks[ci],&l)) continue;
    for(uint32_t i=0;i<l.count;i++){
      uint32_t o=l.start+i*l.stride;
      uint32_t occ=u32(w->data,o+l.occ_off);
      if(occ > (uint32_t)w->size/4) occ=(uint32_t)w->size/4;
      if(total > UINT32_MAX-occ){ total=UINT32_MAX; break; }
      total += occ;
    }
  }
  w->ref_addr=calloc(total>0?total:1,sizeof(uint32_t));
  w->ref_name=calloc(total>0?total:1,sizeof(char*));
  int n=0;
  for(int ci=0;ci<2;ci++){
    GmlRefLayout l;
    if(!gml_bc_ref_layout(w,chunks[ci],&l)) continue;
    for(uint32_t i=0;i<l.count;i++){
      uint32_t o=l.start+i*l.stride;
      const char *nm=gml_str_by_ptr(w,u32(w->data,o));
      uint32_t occ=u32(w->data,o+l.occ_off), addr=u32(w->data,o+l.addr_off);
      for(uint32_t k=0;k<occ;k++){
        if(n>=(int)total || addr==0 || addr==UINT32_MAX || addr>w->size || 8>w->size-addr) break;
        w->ref_addr[n]=addr+l.ref_off; w->ref_name[n]=nm; n++; /* key by the reference-word addr (matches GmlInsn.refaddr) */
        uint32_t ref=u32(w->data,addr+l.chain_off);
        uint32_t nxt=ref & 0x07FFFFFF;
        if(nxt==0) break;
        addr+=nxt;
      }
    }
  }
  w->n_refs=n;
  /* Sort address/name records together, then copy them back into the parallel
   * arrays used for binary search. */
  if(n>1){
    RefRec *rec=malloc((size_t)n*sizeof(*rec));
    if(rec){
      for(int i=0;i<n;i++){ rec[i].addr=w->ref_addr[i]; rec[i].name=w->ref_name[i]; }
      qsort(rec,(size_t)n,sizeof(*rec),cmp_ref_addr);
      for(int i=0;i<n;i++){ w->ref_addr[i]=rec[i].addr; w->ref_name[i]=rec[i].name; }
      free(rec);
    }
  }
}

int gml_room_count(const GmlWin *w){
  const GmlChunk *c=gml_chunk(w,"ROOM"); return c?(int)u32(w->data,c->off):0;
}
int gml_room_get(const GmlWin *w, int idx, GmlRoom *o){
  const GmlChunk *c=gml_chunk(w,"ROOM"); if(!c) return -1;
  uint32_t n=u32(w->data,c->off); if(idx<0||(uint32_t)idx>=n) return -1;
  uint32_t p=u32(w->data,c->off+4+idx*4); const uint8_t *d=w->data;
  o->name=gml_str_by_ptr(w,u32(d,p));
  o->width=u32(d,p+8); o->height=u32(d,p+12); o->speed=u32(d,p+16);
  o->persistent=(int)u32(d,p+20);
  o->bgcolor=u32(d,p+24); o->draw_bg=(int)u32(d,p+28);
  o->creation_code=(int)u32(d,p+32);
  o->flags=u32(d,p+36); o->view_enabled=(o->flags & 1u)!=0;
  o->bg_ptr=u32(d,p+40); o->view_ptr=u32(d,p+44);
  o->obj_ptr=u32(d,p+48); o->tile_ptr=u32(d,p+52);
  return 0;
}

int gml_win_from_mem(GmlWin *w, uint8_t *data, size_t size, int owns){
  memset(w,0,sizeof(*w));
  w->data=data; w->size=size; w->owns=owns;
  if(size<8 || memcmp(data,"FORM",4)) return -1;
  uint32_t total=u32(data,4), o=8, end=8+total;
  while(o<end && o+8<=size && w->n_chunks<40){
    GmlChunk *c=&w->chunks[w->n_chunks++];
    memcpy(c->name,data+o,4); c->name[4]=0;
    c->size=u32(data,o+4); c->off=o+8; o+=8+c->size;
  }
  const GmlChunk *g=gml_chunk(w,"GEN8");
  if(g){
    w->bytecode=data[g->off+1]; w->gameid=u32(data,g->off+20);
    w->disp_w=u32(data,g->off+60); w->disp_h=u32(data,g->off+64);
    /* RoomOrder: fixed-layout offset +128 (DebuggerPort present in bc14). */
    uint32_t ro=g->off+128, cnt=u32(data,ro);
    const GmlChunk *rc=gml_chunk(w,"ROOM");
    uint32_t nroom = rc?u32(data,rc->off):0;
    if(cnt==0 || cnt>nroom){ ro=g->off+124; cnt=u32(data,ro); } /* fallback: no DebuggerPort */
    if(cnt>0 && cnt<=nroom){
      w->n_room_order=(int)cnt; w->room_order=malloc(cnt*sizeof(uint32_t));
      for(uint32_t i=0;i<cnt;i++) w->room_order[i]=u32(data,ro+4+i*4);
      /* GMS2 stores the global game cadence after RoomOrder and its 40-byte project hash block.
       * Older formats end the chunk at RoomOrder, so the bounds/range checks leave their speed at 0. */
      uint64_t so=(uint64_t)ro+4u+(uint64_t)cnt*4u+40u;
      uint64_t gend=(uint64_t)g->off+g->size;
      if(so+4u<=gend && so+4u<=w->size){
        uint32_t bits=u32(data,(uint32_t)so); float speed;
        memcpy(&speed,&bits,sizeof(speed));
        if(isfinite(speed) && speed>=1.0f && speed<=1000.0f) w->game_speed=speed;
      }
    }
  }
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
      if(information_size<=classic->size-24u){
        w->classic_game_information=data+classic->off+24;
        w->classic_game_information_size=information_size;
      }
    }
  }
  parse_strg(w); parse_code(w); parse_refs(w);
  return 0;
}

int gml_win_load(GmlWin *w, const char *path){
  uint8_t *buf=NULL;
  size_t sz=0;
  int storage=0;
#ifdef _WIN32
  HANDLE file=CreateFileA(path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,
                          FILE_ATTRIBUTE_NORMAL|FILE_FLAG_RANDOM_ACCESS,NULL);
  if(file==INVALID_HANDLE_VALUE) return -1;
  LARGE_INTEGER length;
  if(!GetFileSizeEx(file,&length) || length.QuadPart<=0 ||
     (uint64_t)length.QuadPart>(uint64_t)SIZE_MAX){ CloseHandle(file); return -1; }
  sz=(size_t)length.QuadPart;
  HANDLE mapping=CreateFileMappingA(file,NULL,PAGE_READONLY,0,0,NULL);
  if(mapping){
    buf=(uint8_t*)MapViewOfFile(mapping,FILE_MAP_READ,0,0,0);
    CloseHandle(mapping);
  }
  CloseHandle(file);
  if(buf) storage=2;
#else
  int fd=open(path,O_RDONLY);
  if(fd<0) return -1;
  struct stat st;
  if(fstat(fd,&st) || st.st_size<=0 || (uint64_t)st.st_size>(uint64_t)SIZE_MAX){
    close(fd); return -1;
  }
  sz=(size_t)st.st_size;
  void *view=mmap(NULL,sz,PROT_READ,MAP_PRIVATE,fd,0);
  close(fd);
  if(view!=MAP_FAILED){ buf=(uint8_t*)view; storage=2; }
#endif
  /* Mapping may be unavailable on an unusual frontend/filesystem. Preserve the old heap path
   * as a bounded fallback, but avoid it for payloads too large for stdio's long offsets. */
  if(!buf){
    FILE *f=fopen(path,"rb"); if(!f) return -1;
    if(fseek(f,0,SEEK_END)){ fclose(f); return -1; }
    long end=ftell(f);
    if(end<=0 || (uint64_t)end>(uint64_t)SIZE_MAX || fseek(f,0,SEEK_SET)){
      fclose(f); return -1;
    }
    sz=(size_t)end;
    buf=(uint8_t*)malloc(sz);
    if(!buf){ fclose(f); return -1; }
    if(fread(buf,1,sz,f)!=sz){ fclose(f); free(buf); return -1; }
    fclose(f);
    storage=1;
  }
  int rc=gml_win_from_mem(w,buf,sz,storage);
  if(rc!=0){
#ifdef _WIN32
    if(storage==2) UnmapViewOfFile(buf); else free(buf);
#else
    if(storage==2) munmap(buf,sz); else free(buf);
#endif
    memset(w,0,sizeof(*w));
    return rc;
  }
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
    /* Resolve content_dir to an absolute path when possible, so paths built
     * from working_directory do not receive the content prefix a second time. */
#ifndef _WIN32
    { char abs[4096];
      if(realpath(w->content_dir,abs)) snprintf(w->content_dir,sizeof(w->content_dir),"%s",abs); }
#else
    { char abs[4096];
      if(_fullpath(abs,w->content_dir,sizeof abs)) snprintf(w->content_dir,sizeof(w->content_dir),"%s",abs); }
#endif
  }
  return rc;
}

void gml_win_free(GmlWin *w){
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
  else if(w->owns==2 && w->data && w->size){
#ifdef _WIN32
    UnmapViewOfFile(w->data);
#else
    munmap(w->data,w->size);
#endif
  }
  memset(w,0,sizeof(*w));
}
