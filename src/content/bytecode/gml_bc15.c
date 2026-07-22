/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* gml_bc15.c - GameMaker: Studio bytecode 15+ instruction decode.
 * Covers the bc15/bc16 layouts currently exercised by the local compatibility tests.
 */
#include "gml_bytecode.h"
#include <string.h>

static uint32_t u32(const uint8_t *d, uint32_t o){
  return (uint32_t)d[o] | (uint32_t)d[o+1]<<8 | (uint32_t)d[o+2]<<16 | (uint32_t)d[o+3]<<24;
}
static int16_t i16(const uint8_t *d, uint32_t o){
  return (int16_t)((uint16_t)d[o] | (uint16_t)d[o+1]<<8);
}
static int can_read(const GmlWin *w, uint32_t o, uint32_t n){
  return o <= w->size && n <= w->size - o;
}

int gml_bc15_code_start(const GmlWin *w, uint32_t entry_ptr, uint32_t *start){
  if(!w || !start || !can_read(w,entry_ptr,20)) return 0;
  int32_t rel=(int32_t)u32(w->data,entry_ptr+12);
  int64_t s=(int64_t)(entry_ptr+12)+rel;
  /* CODE-v2 stores a relative bytecode address at +12 and an additional
   * entry offset at +16. Add both to locate this entry's instruction stream. */
  s += (int64_t)u32(w->data,entry_ptr+16);
  if(s<0 || s>(int64_t)w->size) return 0;
  *start=(uint32_t)s;
  return 1;
}

int gml_bc15_ref_layout(const GmlWin *w, const char *chunk, GmlRefLayout *out){
  if(!w || !chunk || !out) return 0;
  const GmlChunk *c=gml_chunk(w,chunk);
  if(!c) return 0;
  memset(out,0,sizeof(*out));
  out->ref_off=4; out->chain_off=4;   /* entry addr = instruction; ref word is the 2nd word */
  uint32_t end=c->off+c->size;
  if(end<c->off || end>w->size) end=(uint32_t)w->size;
  if(!strcmp(chunk,"FUNC")){
    if(!can_read(w,c->off,4)) return 0;
    out->count=u32(w->data,c->off);
    out->start=c->off+4;
    out->stride=12;
    out->occ_off=4;
    out->addr_off=8;
  } else {
    if(!can_read(w,c->off,12)) return 0;
    /* bc15+ VARI starts with metadata, not the full record count. */
    out->start=c->off+12;
    out->stride=20;
    out->occ_off=12;
    out->addr_off=16;
    out->count=(end-out->start)/out->stride;
  }
  if(out->start>end) return 0;
  if(out->count > (end-out->start)/out->stride) return 0;
  return 1;
}

int gml_decode_bc15(const uint8_t *d, uint32_t ia, GmlInsn *o){
  memset(o,0,sizeof(*o));
  uint32_t fw=u32(d,ia);
  uint8_t nk=(uint8_t)(fw>>24);
  uint8_t b2=(uint8_t)((fw>>16)&0xFF);
  o->oldkind=nk; o->kind=nk; o->inst=(int16_t)(fw&0xFFFF);
  switch(nk){
    case 0x84:
      o->kind=OP_PUSH; o->type1=DT_INT16; o->sval=(int16_t)(fw&0xFFFF); o->size=4; return 4;
    case OP_NEG:case OP_NOT:case OP_DUP:case OP_RET:case OP_EXIT:case OP_POPZ:
      o->type1=b2&0xF; o->type2=b2>>4; o->size=4; return 4;
    case OP_CALLV:   /* GMS2.3 call-a-value: argc in the low 16 bits, function value on the stack */
      o->type1=b2&0xF; o->argc=(uint16_t)(fw&0xFFFF); o->size=4; return 4;
    case OP_CONV:case OP_MUL:case OP_DIV:case OP_REM:case OP_MOD:case OP_ADD:case OP_SUB:
    case OP_AND:case OP_OR:case OP_XOR:case OP_SHL:case OP_SHR:
      o->type1=b2&0xF; o->type2=b2>>4; o->size=4; return 4;
    case OP_CMP:
      o->type1=b2&0xF; o->type2=b2>>4; o->cmp=(uint8_t)((fw>>8)&0xFF); o->size=4; return 4;
    case OP_B:case OP_BT:case OP_BF:case OP_PUSHENV:case OP_POPENV:{
      /* bc15+ branch offset is a SIGNED 23-BIT word offset in the low 24 bits (bit 23 set =
       * the popenv-exit magic 0xF00000, i.e. `break` out of a with()). Reading only int16
       * worked for short jumps but silently wrapped on sufficiently large scripts. A large
       * dispatch table can branch beyond the signed 16-bit word range and land at an invalid
       * instruction if the complete field is not decoded. */
      uint32_t v = fw & 0xFFFFFF;
      if(v & 0x800000) o->jump = 0;                    /* popenv-exit magic: keep legacy no-jump */
      else o->jump = (int32_t)(v << 9) >> 9;
      o->size=4; return 4;
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
    case 0xC1:case 0xC2:case 0xC3:
      o->kind=OP_PUSH; o->type1=DT_VAR;
      if(nk==0xC1) o->inst=IT_LOCAL;
      else if(nk==0xC2) o->inst=IT_GLOBAL;
      else o->inst=(int16_t)(fw&0xFFFF);
      o->refaddr=ia+4;
      o->reftype=(uint8_t)((u32(d,ia+4)>>24)&0xF8);
      o->size=8;
      return 8;
    case OP_CALL:
      o->type1=b2; o->argc=(uint16_t)(fw&0xFFFF); o->refaddr=ia+4; o->size=8; return 8;
    case OP_BREAK:
      o->sval=(int16_t)(fw&0xFFFF); o->type1=b2;
      /* The pushref variant (-11) carries an extra reference word.
       * Other break variants occupy one instruction word. */
      if(o->sval==-11){
        o->ival=(int32_t)u32(d,ia+4);
        /* The extra word participates in the FUNC occurrence chain when pushref names a
         * function. Expose its address just like push.i32/call do so the VM can distinguish a
         * callable reference from an ordinary asset id without interpreting the raw id itself. */
        o->refaddr=ia+4;
        o->size=8;
        return 8;
      }
      o->size=4; return 4;
    default:
      o->size=0; return 0;
  }
}
