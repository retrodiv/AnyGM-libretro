/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me> */
/* gml_win.c — data.win loader + bytecode-14 decoder. See gml_win.h. */
#include "gml_win.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t u32(const uint8_t *d, uint32_t o){
  return (uint32_t)d[o] | (uint32_t)d[o+1]<<8 | (uint32_t)d[o+2]<<16 | (uint32_t)d[o+3]<<24;
}
static int16_t i16(const uint8_t *d, uint32_t o){ return (int16_t)((uint16_t)d[o] | (uint16_t)d[o+1]<<8); }

/* old (bc14) opcode byte -> new opcode byte */
static uint8_t old2new(uint8_t k){
  switch(k){
    case 0x03:return 0x07; case 0x04:return 0x08; case 0x05:return 0x09; case 0x06:return 0x0A;
    case 0x07:return 0x0B; case 0x08:return 0x0C; case 0x09:return 0x0D; case 0x0A:return 0x0E;
    case 0x0B:return 0x0F; case 0x0C:return 0x10; case 0x0D:return 0x11; case 0x0E:return 0x12;
    case 0x0F:return 0x13; case 0x10:return 0x14;
    case 0x11:case 0x12:case 0x13:case 0x14:case 0x16:return 0x15;
    case 0x41:return 0x45; case 0x82:return 0x86; case 0xB7:return 0xB6; case 0xB8:return 0xB7;
    case 0xB9:return 0xB8; case 0xBB:return 0xBA; case 0x9D:return 0x9C; case 0x9E:return 0x9D;
    case 0x9F:return 0x9E; case 0xBC:return 0xBB; case 0xDA:return 0xD9;
    default:return k;
  }
}

const char *gml_op_mnemonic(uint8_t k){
  switch(k){
    case OP_CONV:return"conv";case OP_MUL:return"mul";case OP_DIV:return"div";case OP_REM:return"rem";
    case OP_MOD:return"mod";case OP_ADD:return"add";case OP_SUB:return"sub";case OP_AND:return"and";
    case OP_OR:return"or";case OP_XOR:return"xor";case OP_NEG:return"neg";case OP_NOT:return"not";
    case OP_SHL:return"shl";case OP_SHR:return"shr";case OP_CMP:return"cmp";case OP_POP:return"pop";
    case OP_DUP:return"dup";case OP_RET:return"ret";case OP_EXIT:return"exit";case OP_POPZ:return"popz";
    case OP_B:return"b";case OP_BT:return"bt";case OP_BF:return"bf";case OP_PUSHENV:return"pushenv";
    case OP_POPENV:return"popenv";case OP_PUSH:return"push";case OP_CALL:return"call";
    case OP_CALLV:return"callv";case OP_BREAK:return"break";default:return"?";
  }
}

int gml_decode(const uint8_t *d, uint32_t ia, GmlInsn *o){
  memset(o,0,sizeof(*o));
  uint32_t fw = u32(d,ia);
  uint8_t kb = (uint8_t)(fw>>24);
  uint8_t nk = old2new(kb);
  uint8_t b2 = (uint8_t)((fw>>16)&0xFF);
  o->oldkind=kb; o->kind=nk; o->inst=(int16_t)(fw&0xFFFF);
  switch(nk){
    /* single */
    case OP_NEG:case OP_NOT:case OP_DUP:case OP_RET:case OP_EXIT:case OP_POPZ:case OP_CALLV:
      o->type1=b2&0xF; o->type2=b2>>4; o->size=4; return 4;
    /* double */
    case OP_CONV:case OP_MUL:case OP_DIV:case OP_REM:case OP_MOD:case OP_ADD:case OP_SUB:
    case OP_AND:case OP_OR:case OP_XOR:case OP_SHL:case OP_SHR:
      o->type1=b2&0xF; o->type2=b2>>4; o->size=4; return 4;
    /* comparison: kind encoded in old opcode */
    case OP_CMP:
      o->type1=b2&0xF; o->type2=b2>>4; o->cmp=(uint8_t)(kb-0x10); o->size=4; return 4;
    /* goto */
    case OP_B:case OP_BT:case OP_BF:case OP_PUSHENV:case OP_POPENV:{
      int32_t off = (fw & 0x800000) ? (int32_t)(fw | 0xFF000000u) : (int32_t)(fw & 0xFFFFFF);
      o->jump=off; o->size=4; return 4;
    }
    /* pop */
    case OP_POP:{
      o->type1=b2&0xF; o->type2=b2>>4;
      if(o->type1!=DT_INT16){ o->refaddr=ia+4; o->reftype=(uint8_t)((u32(d,ia+4)>>24)&0xF8); o->size=8; return 8; }
      o->size=4; return 4;
    }
    /* push (single opcode, datatype in b2) */
    case OP_PUSH:{
      o->type1=b2;
      switch(b2){
        case DT_DOUBLE: o->dval=*(const double*)(d+ia+4); o->size=12; return 12;
        case DT_INT32:  o->ival=(int32_t)u32(d,ia+4);     o->size=8;  return 8;
        case DT_INT64:  memcpy(&o->lval,d+ia+4,8);        o->size=12; return 12;
        case DT_STRING: o->strindex=u32(d,ia+4);          o->size=8;  return 8;
        case DT_VAR:    o->refaddr=ia+4; o->reftype=(uint8_t)((u32(d,ia+4)>>24)&0xF8); o->size=8; return 8;
        case DT_INT16:  o->sval=i16(d,ia);                o->size=4;  return 4;
        case DT_FLOAT:  o->ival=(int32_t)u32(d,ia+4);     o->size=8;  return 8;
        case DT_BOOL:   o->ival=(int32_t)u32(d,ia+4);     o->size=8;  return 8;
        default:        o->size=4; return 4;
      }
    }
    /* call */
    case OP_CALL:
      o->type1=b2; o->argc=(uint16_t)(fw&0xFFFF); o->refaddr=ia+4; o->size=8; return 8;
    /* break */
    case OP_BREAK:
      o->sval=(int16_t)(fw&0xFFFF); o->type1=b2; o->size=4; return 4;
    default:
      o->size=0; return 0;
  }
}

/* ---------------- loader ---------------- */
static const uint8_t *gp; /* not thread-safe; loader is single-shot */

static int cmp_refaddr(const void *a, const void *b){
  uint32_t x=*(const uint32_t*)a, y=*(const uint32_t*)b; return x<y?-1:(x>y?1:0);
}

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
  int lo=0,hi=w->n_refs-1;
  while(lo<=hi){int m=(lo+hi)/2; if(w->ref_addr[m]==addr)return w->ref_name[m];
    if(w->ref_addr[m]<addr)lo=m+1;else hi=m-1;}
  return "?";
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
    w->code[i].start=p+8;
  }
}

/* Walk VARI+FUNC occurrence chains -> ref_addr/ref_name map. */
static void parse_refs(GmlWin *w){
  /* count total occurrences first */
  int total=0;
  const char *chunks[2]={"VARI","FUNC"};
  for(int ci=0;ci<2;ci++){
    const GmlChunk *c=gml_chunk(w,chunks[ci]); if(!c)continue;
    for(uint32_t o=c->off;o+12<=c->off+c->size;o+=12) total+=(int)u32(w->data,o+4);
  }
  w->ref_addr=calloc(total>0?total:1,sizeof(uint32_t));
  w->ref_name=calloc(total>0?total:1,sizeof(char*));
  int n=0;
  for(int ci=0;ci<2;ci++){
    const GmlChunk *c=gml_chunk(w,chunks[ci]); if(!c)continue;
    for(uint32_t o=c->off;o+12<=c->off+c->size;o+=12){
      const char *nm=gml_str_by_ptr(w,u32(w->data,o));
      uint32_t occ=u32(w->data,o+4), addr=u32(w->data,o+8);
      for(uint32_t k=0;k<occ;k++){
        if(addr==0 || addr+8>w->size) break;
        w->ref_addr[n]=addr+4; w->ref_name[n]=nm; n++; /* key by the reference-word addr (matches GmlInsn.refaddr) */
        uint32_t ref=u32(w->data,addr+4);
        uint32_t nxt=ref & 0x07FFFFFF;
        if(nxt==0) break;
        addr+=nxt;
      }
    }
  }
  w->n_refs=n;
  /* sort by addr for bsearch (stable enough: addrs unique per site) */
  /* simple insertion of parallel arrays via index sort */
  for(int i=1;i<n;i++){
    uint32_t a=w->ref_addr[i]; const char *nm=w->ref_name[i]; int j=i-1;
    while(j>=0 && w->ref_addr[j]>a){ w->ref_addr[j+1]=w->ref_addr[j]; w->ref_name[j+1]=w->ref_name[j]; j--; }
    w->ref_addr[j+1]=a; w->ref_name[j+1]=nm;
  }
  (void)cmp_refaddr; (void)gp;
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
  o->bgcolor=u32(d,p+24); o->draw_bg=(int)u32(d,p+28);
  o->creation_code=(int)u32(d,p+32);
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
    }
  }
  parse_strg(w); parse_code(w); parse_refs(w);
  return 0;
}

int gml_win_load(GmlWin *w, const char *path){
  FILE *f=fopen(path,"rb"); if(!f) return -1;
  fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
  uint8_t *buf=malloc(sz); if(!buf){fclose(f);return -1;}
  if(fread(buf,1,sz,f)!=(size_t)sz){fclose(f);free(buf);return -1;}
  fclose(f);
  return gml_win_from_mem(w,buf,(size_t)sz,1);
}

void gml_win_free(GmlWin *w){
  free(w->strs); free(w->str_charoff); free(w->code);
  free(w->ref_addr); free(w->ref_name); free(w->room_order);
  if(w->owns) free(w->data);
  memset(w,0,sizeof(*w));
}
