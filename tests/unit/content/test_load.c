/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me> */
/* test_load - Report container structure and decode diagnostics for an input path. */
#include "gml_win.h"
#include <stdio.h>
#include <stdlib.h>
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
  const char *call_filter = argc>2?argv[2]:NULL;
  GmlWin w;
  if(gml_win_load(&w,path)){ fprintf(stderr,"load failed: %s\n",path); return 1; }
  printf("# %s  bytecode=%u gameid=%u speed=%.3f chunks=%d strings=%d code=%d refs=%d\n",
         path,w.bytecode,w.gameid,w.game_speed,w.n_chunks,w.n_strs,w.n_code,w.n_refs);
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

  /* Optionally index call sites matching a caller-supplied substring. */
  if(call_filter && *call_filter){
    printf("\n## CALL SITES containing '%s'\n",call_filter);
    for(int i=0;i<w.n_code;i++){
      uint32_t a=w.code[i].start,end=a+w.code[i].length;
      for(;a<end;){
        GmlInsn in; int sz=gml_decode_bc(w.data,a,w.bytecode,&in); if(sz<=0) break;
        if(in.kind==OP_CALL){
          const char *name=gml_ref_name(&w,in.refaddr);
          if(name && strstr(name,call_filter))
            printf("  [%d] %-64s +%-6u %s(%u)\n",i,w.code[i].name,a-w.code[i].start,name,in.argc);
        }
        a+=(uint32_t)sz;
      }
    }
  }

  /* Print a decoded instruction sample, or a selected full entry when argv[3] is supplied. The
   * selector may be a numeric index or an exact or partial code name. */
  int selected=-1;
  if(argc>3){
    char *end=NULL; long index=strtol(argv[3],&end,10);
    if(end && *end==0) selected=(int)index;
    else {
      for(int i=0;i<w.n_code;i++)
        if(w.code[i].name && (!strcmp(w.code[i].name,argv[3]) || strstr(w.code[i].name,argv[3]))){ selected=i; break; }
      if(selected<0) fprintf(stderr,"code selector not found: %s\n",argv[3]);
    }
  }
  int max_lines=argc>4?atoi(argv[4]):22;
  if(max_lines<=0) max_lines=22;
  for(int i=0;i<w.n_code;i++){
    if(selected>=0 && i!=selected) continue;
    if(w.code[i].length==0){
      if(selected>=0)
        printf("\n=== [%d] %s (start=%u, 0 bytes) ===\n",i,w.code[i].name,w.code[i].start);
      continue;
    }
    printf("\n=== [%d] %s (start=%u, %u bytes) ===\n",
           i,w.code[i].name,w.code[i].start,w.code[i].length);
    uint32_t a=w.code[i].start, end=a+w.code[i].length; int line=0;
    while(a<end && line<max_lines){
      GmlInsn in; int sz=gml_decode_bc(w.data,a,w.bytecode,&in);
      printf("  %5u: %s", a-w.code[i].start, gml_op_mnemonic(in.kind));
      switch(in.kind){
        case OP_CMP: printf(".%s.%s %s",DT[in.type1],DT[in.type2],CMP[in.cmp<=6?in.cmp:0]); break;
        case OP_B:case OP_BT:case OP_BF:case OP_PUSHENV:case OP_POPENV:
          printf(" -> %u",(a-w.code[i].start)+in.jump*4); break;
        case OP_POP: printf(".%s.%s %s.%s",DT[in.type1],DT[in.type2],inst_name(in.inst),gml_ref_name(&w,in.refaddr)); break;
        case OP_CALL: printf(" %s(%u)",gml_ref_name(&w,in.refaddr),in.argc); break;
        case OP_CALLV: printf(" argc=%u",in.argc); break;
        case OP_DUP: printf(" inst=0x%x",(unsigned)(uint16_t)in.inst); break;
        case OP_BREAK:
          printf(" %d",in.sval);
          if(in.sval==-11) printf(" ref=0x%08x %s",(uint32_t)in.ival,gml_ref_name(&w,in.refaddr));
          break;
        case OP_PUSH:
          if(in.type1==DT_INT16) printf(".e %d",in.sval);
          else if(in.type1==DT_DOUBLE) printf(".d %g",in.dval);
          else if(in.type1==DT_INT32){
            const char *ref=gml_ref_name(&w,a+4);
            printf(".i32 %d",in.ival);
            if(ref && ref[0]!='?') printf(" [%s]",ref);
          }
          else if(in.type1==DT_INT64) printf(".i64 %lld",(long long)in.lval);
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
