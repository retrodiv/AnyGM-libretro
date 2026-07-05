/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me> */
/* test_load - Report container structure and decode diagnostics for an input path. */
#include "gml_win.h"
#include <stdio.h>
#include <string.h>

static const char *DT[16]={"d","f","i32","i64","b","v","s","?","?","?","?","?","?","?","?","e"};
static const char *CMP[7]={"?","<","<=","==","!=",">=",">"};
static const char *inst_name(int16_t it){
  switch(it){case IT_SELF:return"self";case IT_OTHER:return"other";case IT_ALL:return"all";
    case IT_GLOBAL:return"global";case IT_LOCAL:return"local";case IT_STACK:return"stack";
    case IT_BUILTIN:return"builtin";default:return"obj";}
}

int main(int argc,char**argv){
  const char *path = argc>1?argv[1]:"data.win";
  GmlWin w;
  if(gml_win_load(&w,path)){ fprintf(stderr,"load failed: %s\n",path); return 1; }
  printf("# %s  bytecode=%u gameid=%u  chunks=%d strings=%d code=%d refs=%d\n",
         path,w.bytecode,w.gameid,w.n_chunks,w.n_strs,w.n_code,w.n_refs);
  for(int i=0;i<w.n_chunks;i++)
    printf("  %-4s off=%-10u size=%u\n",w.chunks[i].name,w.chunks[i].off,w.chunks[i].size);

  /* global decode sanity */
  long total=0, unknown=0;
  for(int i=0;i<w.n_code;i++){
    uint32_t a=w.code[i].start, end=a+w.code[i].length;
    while(a<end){
      GmlInsn in; int sz=gml_decode_bc(w.data,a,w.bytecode,&in);
      if(sz==0){ fprintf(stderr,"decode error in %s @%u\n",w.code[i].name,a); break; }
      if(in.kind==0 || !strcmp(gml_op_mnemonic(in.kind),"?")) unknown++;
      total++; a+=sz;
    }
  }
  printf("\n## GLOBAL: %ld instructions, %ld unknown opcodes\n", total, unknown);

  /* Print instructions from the first non-empty code entry. */
  for(int i=0;i<w.n_code;i++){
    if(w.code[i].length==0) continue;
    printf("\n=== [%d] %s (%u bytes) ===\n",i,w.code[i].name,w.code[i].length);
    uint32_t a=w.code[i].start, end=a+w.code[i].length; int line=0;
    while(a<end && line<22){
      GmlInsn in; int sz=gml_decode_bc(w.data,a,w.bytecode,&in);
      printf("  %5u: %s", a-w.code[i].start, gml_op_mnemonic(in.kind));
      switch(in.kind){
        case OP_CMP: printf(".%s.%s %s",DT[in.type1],DT[in.type2],CMP[in.cmp<=6?in.cmp:0]); break;
        case OP_B:case OP_BT:case OP_BF:case OP_PUSHENV:case OP_POPENV:
          printf(" -> %u",(a-w.code[i].start)+in.jump*4); break;
        case OP_POP: printf(".%s.%s %s.%s",DT[in.type1],DT[in.type2],inst_name(in.inst),gml_ref_name(&w,in.refaddr)); break;
        case OP_CALL: printf(" %s(%u)",gml_ref_name(&w,in.refaddr),in.argc); break;
        case OP_PUSH:
          if(in.type1==DT_INT16) printf(".e %d",in.sval);
          else if(in.type1==DT_DOUBLE) printf(".d %g",in.dval);
          else if(in.type1==DT_INT32) printf(".i32 %d",in.ival);
          else if(in.type1==DT_STRING) printf(".s \"%s\"",gml_str_by_index(&w,in.strindex));
          else if(in.type1==DT_VAR) printf(".v %s.%s",inst_name(in.inst),gml_ref_name(&w,in.refaddr));
          else printf(".%s",DT[in.type1&0xF]);
          break;
        default:
          if(in.type1||in.type2) printf(".%s",DT[in.type1&0xF]);
      }
      printf("\n"); a+=sz; line++;
    }
    break;
  }
  gml_win_free(&w);
  return 0;
}
