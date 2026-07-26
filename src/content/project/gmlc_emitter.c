/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "gmlc_bytecode_internal.h"
#include "gml_win.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static int reserve(CodeBuf *b, size_t n){
  if(b->len+n<=b->cap) return 1;
  size_t nc=b->cap?b->cap*2:256;
  while(b->len+n>nc) nc*=2;
  uint8_t *nd=(uint8_t*)realloc(b->data,nc);
  if(!nd) return 0;
  b->data=nd; b->cap=nc;
  return 1;
}

int emit_u32(CodeBuf *b, uint32_t v){
  if(!reserve(b,4)) return 0;
  b->data[b->len++]=(uint8_t)v;
  b->data[b->len++]=(uint8_t)(v>>8);
  b->data[b->len++]=(uint8_t)(v>>16);
  b->data[b->len++]=(uint8_t)(v>>24);
  return 1;
}

static int emit_i32(CodeBuf *b, int32_t v){ return emit_u32(b,(uint32_t)v); }

static int emit_double(CodeBuf *b, double d){
  if(!reserve(b,8)) return 0;
  memcpy(b->data+b->len,&d,8);
  b->len+=8;
  return 1;
}

static void patch_u32(CodeBuf *b, size_t pos, uint32_t v){
  b->data[pos]=(uint8_t)v;
  b->data[pos+1]=(uint8_t)(v>>8);
  b->data[pos+2]=(uint8_t)(v>>16);
  b->data[pos+3]=(uint8_t)(v>>24);
}

uint32_t fw(uint8_t op, uint8_t type_byte, int16_t low){
  return ((uint32_t)op<<24) | ((uint32_t)type_byte<<16) | (uint16_t)low;
}

static int add_ref(Compiler *c, const char *name, GmlcRefKind kind, uint32_t instr, uint32_t ref, uint32_t high, int inst){
  if(c->n_refs>=c->cap_refs){
    int nc=c->cap_refs?c->cap_refs*2:64;
    GmlcRefSite *nr=(GmlcRefSite*)realloc(c->refs,(size_t)nc*sizeof(*nr));
    if(!nr) return 0;
    c->refs=nr; c->cap_refs=nc;
  }
  GmlcRefSite *r=&c->refs[c->n_refs++];
  memset(r,0,sizeof(*r));
  r->name=gmlc_strdup(name);
  r->kind=kind;
  r->instr_off=instr;
  r->ref_off=ref;
  r->high_bits=high;
  r->inst=inst;
  return r->name!=NULL;
}

static int add_string_site(Compiler *c, const char *value, uint32_t payload_off){
  if(c->n_strings>=c->cap_strings){
    int nc=c->cap_strings?c->cap_strings*2:32;
    GmlcStringSite *ns=(GmlcStringSite*)realloc(c->strings,(size_t)nc*sizeof(*ns));
    if(!ns) return 0;
    c->strings=ns; c->cap_strings=nc;
  }
  GmlcStringSite *s=&c->strings[c->n_strings++];
  s->value=gmlc_strdup(value);
  s->payload_off=payload_off;
  return s->value!=NULL;
}

int emit_push_real(Compiler *c, double d){
  if(floor(d)==d && d>=-32768.0 && d<=32767.0)
    return emit_u32(&c->code,fw(0x84,DT_INT16,(int16_t)d));
  if(floor(d)==d && d>=-2147483648.0 && d<=2147483647.0){
    if(!emit_u32(&c->code,fw(OP_PUSH,DT_INT32,0))) return 0;
    return emit_i32(&c->code,(int32_t)d);
  }
  if(!emit_u32(&c->code,fw(OP_PUSH,DT_DOUBLE,0))) return 0;
  return emit_double(&c->code,d);
}

int emit_push_i16_full(Compiler *c, int16_t v){
  return emit_u32(&c->code,fw(OP_PUSH,DT_INT16,v));
}

int emit_push_i32_full(Compiler *c, int32_t v){
  if(!emit_u32(&c->code,fw(OP_PUSH,DT_INT32,0))) return 0;
  return emit_i32(&c->code,v);
}

int emit_const_number(Compiler *c, double d){
  size_t start=c->code.len;
  if(!emit_push_real(c,d)) return 0;
  c->expr_const=1;
  c->expr_const_value=d;
  c->expr_const_start=start;
  c->expr_boolish=0;
  return 1;
}

void expr_not_const(Compiler *c){
  c->expr_const=0;
}

int emit_push_string_literal(Compiler *c, const char *s){
  if(!emit_u32(&c->code,fw(OP_PUSH,DT_STRING,0))) return 0;
  uint32_t payload=(uint32_t)c->code.len;
  if(!emit_u32(&c->code,0)) return 0;
  return add_string_site(c,s,payload);
}

int emit_push_var(Compiler *c, int inst, const char *name, uint8_t reftype){
  uint32_t instr=(uint32_t)c->code.len;
  uint8_t op=(inst==IT_LOCAL && reftype==0xA0) ? 0xC1 : OP_PUSH;
  if(!emit_u32(&c->code,fw(op,DT_VAR,(int16_t)inst))) return 0;
  uint32_t ref=(uint32_t)c->code.len;
  if(!emit_u32(&c->code,(uint32_t)reftype<<24)) return 0;
  return add_ref(c,name,GMLC_REF_VARI,instr,ref,(uint32_t)reftype<<24,inst);
}

int emit_pop_var(Compiler *c, int inst, const char *name, uint8_t reftype, uint8_t type1){
  uint8_t tb=(uint8_t)((DT_VAR<<4) | (type1&0xF));
  uint32_t instr=(uint32_t)c->code.len;
  if(!emit_u32(&c->code,fw(OP_POP,tb,(int16_t)inst))) return 0;
  uint32_t ref=(uint32_t)c->code.len;
  if(!emit_u32(&c->code,(uint32_t)reftype<<24)) return 0;
  return add_ref(c,name,GMLC_REF_VARI,instr,ref,(uint32_t)reftype<<24,inst);
}

static int classic_identifier_equal(const char *a, const char *b){
  while(*a && *b){
    if(tolower((unsigned char)*a)!=tolower((unsigned char)*b)) return 0;
    a++; b++;
  }
  return *a==*b;
}

static int classic_alias_identifier_equal(const char *a, const char *b){
  while(1){
    while(*a=='_') a++;
    while(*b=='_') b++;
    if(!*a || !*b) break;
    if(tolower((unsigned char)*a)!=tolower((unsigned char)*b)) return 0;
    a++; b++;
  }
  while(*a=='_') a++;
  while(*b=='_') b++;
  return *a==*b;
}

static const char *canonical_call_name(Compiler *c, const char *name){
  if(!c->project || !c->project->classic_version) return name;
  for(int i=0;i<c->project->n_scripts;i++){
    const char *candidate=c->project->scripts[i].name;
    if(candidate && classic_identifier_equal(candidate,name)) return candidate;
  }
  for(int i=0;i<c->project->n_function_aliases;i++){
    const GmlcFunctionAlias *alias=&c->project->function_aliases[i];
    if(!alias->ambiguous && alias->public_name && alias->target_name &&
       classic_identifier_equal(alias->public_name,name)) return alias->target_name;
  }
  const char *target=NULL;
  for(int i=0;i<c->project->n_function_aliases;i++){
    const GmlcFunctionAlias *alias=&c->project->function_aliases[i];
    if(!alias->public_name || !alias->target_name ||
       !classic_alias_identifier_equal(alias->public_name,name)) continue;
    if(alias->ambiguous) return name;
    if(target && !classic_identifier_equal(target,alias->target_name)) return name;
    target=alias->target_name;
  }
  if(target) return target;
  return name;
}

int emit_call(Compiler *c, const char *name, int argc){
  uint32_t instr=(uint32_t)c->code.len;
  if(!emit_u32(&c->code,fw(OP_CALL,DT_INT32,(int16_t)argc))) return 0;
  uint32_t ref=(uint32_t)c->code.len;
  if(!emit_u32(&c->code,0)) return 0;
  return add_ref(c,canonical_call_name(c,name),GMLC_REF_FUNC,instr,ref,0,0);
}

int emit_callv(Compiler *c, int argc){
  return emit_u32(&c->code,fw(OP_CALLV,DT_VAR,(int16_t)argc));
}

size_t emit_branch(Compiler *c, uint8_t op){
  size_t pos=c->code.len;
  emit_u32(&c->code,fw(op,0,0));
  return pos;
}

void patch_branch(Compiler *c, size_t pos, size_t target){
  int32_t delta=(int32_t)((target - pos) / 4);
  uint32_t old=(uint32_t)c->code.data[pos] | ((uint32_t)c->code.data[pos+1]<<8) | ((uint32_t)c->code.data[pos+2]<<16) | ((uint32_t)c->code.data[pos+3]<<24);
  uint32_t v=(old & 0xFF000000u) | ((uint32_t)delta & 0x7FFFFFu);
  patch_u32(&c->code,pos,v);
}

int emit_binary(Compiler *c, uint8_t op){
  return emit_u32(&c->code,fw(op,(uint8_t)((DT_VAR<<4)|DT_VAR),0));
}

int emit_binary_typed(Compiler *c, uint8_t op, uint8_t type1, uint8_t type2){
  return emit_u32(&c->code,fw(op,(uint8_t)((type2<<4)|(type1&0xF)),0));
}

int emit_cmp(Compiler *c, uint8_t cmp){
  return emit_u32(&c->code,((uint32_t)OP_CMP<<24) | ((uint32_t)((DT_VAR<<4)|DT_VAR)<<16) | ((uint32_t)cmp<<8));
}

int emit_cmp_typed(Compiler *c, uint8_t cmp, uint8_t type1, uint8_t type2){
  uint8_t types=(uint8_t)((type2<<4)|(type1&0xF));
  return emit_u32(&c->code,((uint32_t)OP_CMP<<24) | ((uint32_t)types<<16) | ((uint32_t)cmp<<8));
}

int emit_conv(Compiler *c, uint8_t type1, uint8_t type2){
  return emit_u32(&c->code,fw(OP_CONV,(uint8_t)((type2<<4)|(type1&0xF)),0));
}

int emit_condition_bool(Compiler *c){
  return c->expr_boolish || emit_conv(c,DT_VAR,DT_BOOL);
}
