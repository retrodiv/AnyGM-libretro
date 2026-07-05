/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me> */
/* gml_bc14.c - Studio bytecode 14 instruction decoding. */
#include "gml_bytecode.h"
#include <string.h>

static uint32_t u32(const uint8_t *d, uint32_t o){
  return (uint32_t)d[o] | (uint32_t)d[o+1]<<8 | (uint32_t)d[o+2]<<16 | (uint32_t)d[o+3]<<24;
}
static int16_t i16(const uint8_t *d, uint32_t o){
  return (int16_t)((uint16_t)d[o] | (uint16_t)d[o+1]<<8);
}

int gml_bc14_code_start(const GmlWin *w, uint32_t entry_ptr, uint32_t *start){
  if(!w || !start) return 0;
  *start=entry_ptr+8;
  return *start <= w->size;
}

int gml_bc14_ref_layout(const GmlWin *w, const char *chunk, GmlRefLayout *out){
  if(!w || !chunk || !out) return 0;
  const GmlChunk *c=gml_chunk(w,chunk);
  if(!c) return 0;
  memset(out,0,sizeof(*out));
  out->ref_off=4; out->chain_off=4;   /* entry addr = instruction; ref word is the 2nd word */
  uint32_t end=c->off+c->size;
  if(end<c->off || end>w->size) end=(uint32_t)w->size;
  out->start=c->off;
  out->stride=12;
  out->occ_off=4;
  out->addr_off=8;
  out->count=(end-c->off)/out->stride;
  return 1;
}

static uint8_t old2new(uint8_t k){
  switch(k){
    case 0x03:return OP_CONV; case 0x04:return OP_MUL; case 0x05:return OP_DIV; case 0x06:return OP_REM;
    case 0x07:return OP_MOD;  case 0x08:return OP_ADD; case 0x09:return OP_SUB; case 0x0A:return OP_AND;
    case 0x0B:return OP_OR;   case 0x0C:return OP_XOR; case 0x0D:return OP_NEG; case 0x0E:return OP_NOT;
    case 0x0F:return OP_SHL;  case 0x10:return OP_SHR;
    case 0x11:case 0x12:case 0x13:case 0x14:case 0x16:return OP_CMP;
    case 0x41:return OP_POP; case 0x82:return OP_DUP; case 0xB7:return OP_B; case 0xB8:return OP_BT;
    case 0xB9:return OP_BF; case 0xBB:return OP_PUSHENV; case 0x9D:return OP_RET; case 0x9E:return OP_EXIT;
    case 0x9F:return OP_POPZ; case 0xBC:return OP_POPENV; case 0xDA:return OP_CALL;
    default:return k;
  }
}

int gml_decode_bc14(const uint8_t *d, uint32_t ia, GmlInsn *o){
  memset(o,0,sizeof(*o));
  uint32_t fw=u32(d,ia);
  uint8_t kb=(uint8_t)(fw>>24);
  uint8_t nk=old2new(kb);
  uint8_t b2=(uint8_t)((fw>>16)&0xFF);
  o->oldkind=kb; o->kind=nk; o->inst=(int16_t)(fw&0xFFFF);
  switch(nk){
    case OP_NEG:case OP_NOT:case OP_DUP:case OP_RET:case OP_EXIT:case OP_POPZ:case OP_CALLV:
      o->type1=b2&0xF; o->type2=b2>>4; o->size=4; return 4;
    case OP_CONV:case OP_MUL:case OP_DIV:case OP_REM:case OP_MOD:case OP_ADD:case OP_SUB:
    case OP_AND:case OP_OR:case OP_XOR:case OP_SHL:case OP_SHR:
      o->type1=b2&0xF; o->type2=b2>>4; o->size=4; return 4;
    case OP_CMP:
      o->type1=b2&0xF; o->type2=b2>>4; o->cmp=(uint8_t)(kb-0x10); o->size=4; return 4;
    case OP_B:case OP_BT:case OP_BF:case OP_PUSHENV:case OP_POPENV:{
      int32_t off=(fw&0x800000) ? (int32_t)(fw|0xFF000000u) : (int32_t)(fw&0xFFFFFF);
      o->jump=off; o->size=4; return 4;
    }
    case OP_POP:
      o->type1=b2&0xF; o->type2=b2>>4;
      if(o->type1!=DT_INT16){ o->refaddr=ia+4; o->reftype=(uint8_t)((u32(d,ia+4)>>24)&0xF8); o->size=8; return 8; }
      o->size=4; return 4;
    case OP_PUSH:
      o->type1=b2;
      switch(b2){
        case DT_DOUBLE: memcpy(&o->dval,d+ia+4,8);       o->size=12; return 12;
        case DT_INT32:  o->ival=(int32_t)u32(d,ia+4);    o->size=8;  return 8;
        case DT_INT64:  memcpy(&o->lval,d+ia+4,8);       o->size=12; return 12;
        case DT_STRING: o->strindex=u32(d,ia+4);         o->size=8;  return 8;
        case DT_VAR:    o->refaddr=ia+4; o->reftype=(uint8_t)((u32(d,ia+4)>>24)&0xF8); o->size=8; return 8;
        case DT_INT16:  o->sval=i16(d,ia);               o->size=4;  return 4;
        case DT_FLOAT:  o->ival=(int32_t)u32(d,ia+4);    o->size=8;  return 8;
        case DT_BOOL:   o->ival=(int32_t)u32(d,ia+4);    o->size=8;  return 8;
        default:        o->size=4; return 4;
      }
    case OP_CALL:
      o->type1=b2; o->argc=(uint16_t)(fw&0xFFFF); o->refaddr=ia+4; o->size=8; return 8;
    case OP_BREAK:
      o->sval=(int16_t)(fw&0xFFFF); o->type1=b2; o->size=4; return 4;
    default:
      o->size=0; return 0;
  }
}
