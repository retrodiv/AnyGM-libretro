/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me> */
/* gml_bytecode.c - Shared dispatch for version-specific bytecode readers. */
#include "gml_bytecode.h"

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

int gml_decode_bc(const uint8_t *d, uint32_t ia, uint8_t bytecode, GmlInsn *out){
  return bytecode>=15 ? gml_decode_bc15(d,ia,out) : gml_decode_bc14(d,ia,out);
}

int gml_decode(const uint8_t *d, uint32_t ia, GmlInsn *out){
  return gml_decode_bc14(d,ia,out);
}

int gml_bc_code_start(const GmlWin *w, uint32_t entry_ptr, uint32_t *start){
  return w && w->bytecode>=15 ? gml_bc15_code_start(w,entry_ptr,start)
                              : gml_bc14_code_start(w,entry_ptr,start);
}

int gml_bc_ref_layout(const GmlWin *w, const char *chunk, GmlRefLayout *out){
  if(!w) return 0;
  if(w->bytecode>=17) return gml_bc17_ref_layout(w,chunk,out);
  return w->bytecode>=15 ? gml_bc15_ref_layout(w,chunk,out)
                         : gml_bc14_ref_layout(w,chunk,out);
}
