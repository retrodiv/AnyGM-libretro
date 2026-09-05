/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "content_transform.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Program { uint8_t bytes[64*12]; size_t size; } Program;
static void emit(Program *program,unsigned op,unsigned d,unsigned a,unsigned b,uint64_t imm){
  assert(program->size+12<=sizeof program->bytes);
  uint8_t *p=program->bytes+program->size;
  p[0]=(uint8_t)op; p[1]=(uint8_t)d; p[2]=(uint8_t)a; p[3]=(uint8_t)b;
  for(unsigned i=0;i<8;i++) p[4+i]=(uint8_t)(imm>>(8u*i));
  program->size+=12;
}
static void constant(Program *p,unsigned d,uint64_t value){
  emit(p,ANYGM_TRANSFORM_CONSTANT,d,0,0,value);
}
static void return_all(Program *p){ emit(p,ANYGM_TRANSFORM_RETURN,0,31,0,0); }
static int execute(Program *program,uint64_t limit,uint8_t **output,size_t *size){
  static const uint8_t input[8]={3,5,7,9,11,13,15,17};
  static const uint8_t parameter[8]={19,21,23,25,27,29,31,33};
  uint8_t mutable_input[8];
  memcpy(mutable_input,input,sizeof input);
  char error[128];
  int ok=anygm_content_transform_execute(program->bytes,program->size,
    parameter,sizeof parameter,mutable_input,sizeof mutable_input,limit,output,size,error,sizeof error);
  assert(!memcmp(mutable_input,input,sizeof input));
  if(!ok){ assert(error[0]); assert(!*output && !*size); }
  return ok;
}
static void reject(Program *p){
  uint8_t *output=(uint8_t*)p; size_t size=99;
  assert(!execute(p,100,&output,&size));
}
static void arithmetic(void){
  static const struct { unsigned op; uint64_t left,right,expected; } cases[]={
    {ANYGM_TRANSFORM_ADD,UINT64_MAX,2,1}, {ANYGM_TRANSFORM_SUBTRACT,0,1,UINT64_MAX},
    {ANYGM_TRANSFORM_MULTIPLY,UINT64_MAX,2,UINT64_MAX-1},
    {ANYGM_TRANSFORM_DIVIDE,27,4,6}, {ANYGM_TRANSFORM_REMAINDER,27,4,3},
    {ANYGM_TRANSFORM_AND,10,6,2}, {ANYGM_TRANSFORM_OR,10,6,14}, {ANYGM_TRANSFORM_XOR,10,6,12},
    {ANYGM_TRANSFORM_SHIFT_LEFT,1,63,UINT64_C(1)<<63},
    {ANYGM_TRANSFORM_SHIFT_RIGHT,UINT64_MAX,63,1},
    {ANYGM_TRANSFORM_EQUAL,8,8,1}, {ANYGM_TRANSFORM_EQUAL,8,9,0},
    {ANYGM_TRANSFORM_LESS,8,9,1}, {ANYGM_TRANSFORM_LESS,UINT64_MAX,0,0}
  };
  for(size_t i=0;i<sizeof cases/sizeof cases[0];i++){
    Program p={0}; constant(&p,2,cases[i].left); constant(&p,3,cases[i].right);
    emit(&p,cases[i].op,4,2,3,0); emit(&p,ANYGM_TRANSFORM_STORE64,4,31,1,0); return_all(&p);
    uint8_t *output=NULL; size_t size=0;
    assert(execute(&p,0,&output,&size) && size==8);
    for(unsigned n=0;n<8;n++) assert(output[n]==(uint8_t)(cases[i].expected>>(8u*n)));
    free(output);
  }
}
static void buffers_and_branches(void){
  Program p={0};
  emit(&p,ANYGM_TRANSFORM_LOAD64,2,31,2,0);
  emit(&p,ANYGM_TRANSFORM_STORE64,2,31,3,65528);
  emit(&p,ANYGM_TRANSFORM_LOAD32,3,31,3,65532);
  emit(&p,ANYGM_TRANSFORM_STORE32,3,31,1,4);
  emit(&p,ANYGM_TRANSFORM_LOAD8,4,31,0,7);
  emit(&p,ANYGM_TRANSFORM_STORE8,4,31,1,0);
  constant(&p,5,1);
  emit(&p,ANYGM_TRANSFORM_JUMP_ZERO,0,31,0,9);
  emit(&p,ANYGM_TRANSFORM_REJECT,0,0,0,0);
  emit(&p,ANYGM_TRANSFORM_JUMP_NONZERO,0,5,0,11);
  emit(&p,ANYGM_TRANSFORM_REJECT,0,0,0,0);
  emit(&p,ANYGM_TRANSFORM_MOVE,6,0,0,0);
  constant(&p,7,4); emit(&p,ANYGM_TRANSFORM_RETURN,0,7,7,0);
  uint8_t *output=NULL; size_t size=0;
  assert(execute(&p,0,&output,&size) && size==4);
  const uint8_t expected[]={27,29,31,33};
  assert(!memcmp(output,expected,sizeof expected)); free(output);
}
static void failures(void){
  Program p={0};
  emit(&p,ANYGM_TRANSFORM_JUMP,0,0,0,0); reject(&p);
  p.size=0; emit(&p,ANYGM_TRANSFORM_REJECT,0,0,0,0); reject(&p);
  p.size=0; constant(&p,2,42); reject(&p);
  p.size=0; emit(&p,ANYGM_TRANSFORM_DIVIDE,2,0,31,0); return_all(&p); reject(&p);
  p.bytes[0]=ANYGM_TRANSFORM_REMAINDER; reject(&p);
  p.size=0; constant(&p,2,64); emit(&p,ANYGM_TRANSFORM_SHIFT_LEFT,3,0,2,0); return_all(&p); reject(&p);
  p.bytes[12]=ANYGM_TRANSFORM_SHIFT_RIGHT; reject(&p);
  for(unsigned op=ANYGM_TRANSFORM_LOAD8;op<=ANYGM_TRANSFORM_STORE64;op++){
    p.size=0; emit(&p,op,2,31,1,8); return_all(&p); reject(&p);
  }
  p.size=0; constant(&p,2,UINT64_MAX); emit(&p,ANYGM_TRANSFORM_LOAD8,3,2,0,1); return_all(&p); reject(&p);
  p.size=0; emit(&p,ANYGM_TRANSFORM_STORE8,0,31,0,0); return_all(&p); reject(&p);
  p.bytes[3]=2; reject(&p); p.bytes[3]=4; reject(&p);
  p.size=0; emit(&p,ANYGM_TRANSFORM_RETURN,0,0,0,0); reject(&p);
  p.size=0; return_all(&p); emit(&p,255,0,0,0,0); reject(&p);
  p.bytes[12]=ANYGM_TRANSFORM_MOVE; p.bytes[13]=32; reject(&p);
  p.size=0; emit(&p,ANYGM_TRANSFORM_JUMP,0,0,0,1); reject(&p);
  p.size=0; return_all(&p); p.size--; reject(&p);
  uint8_t *output=NULL; size_t size=0; char error[128];
  p.size=0; return_all(&p);
  assert(!anygm_content_transform_execute(p.bytes,p.size,NULL,0,NULL,
    (size_t)ANYGM_TRANSFORM_MAX_INPUT_BYTES+1u,0,&output,&size,error,sizeof error));
  assert(!output && !size);
  assert(anygm_content_transform_execute(p.bytes,p.size,NULL,0,NULL,0,0,&output,&size,error,sizeof error));
  assert(output && !size); free(output);
}
static void configuration(void){
  const char identity[]="[transforms]\nidentity=00001f000000000000000000:\n";
  const char reject_program[]="[transforms]\nidentity=010000000000000000000000:\n";
  const char duplicate[]="[transforms]\nidentity=00001f000000000000000000:\nidentity=010000000000000000000000:\n";
  const char malformed[]="[transforms]\nidentity=010000000000000000000000:\nsecond=not-hex:\n";
  const char ordered[]="[transforms]\none=00001f000000000000000000:01\ntwo=00001f000000000000000000:02\n";
  const char reverse[]="[transforms]\ntwo=00001f000000000000000000:02\none=00001f000000000000000000:01\n";
  AnygmContentTransforms *set=anygm_content_transforms_create(),*other=anygm_content_transforms_create();
  assert(set && other);
  char error[128]; uint8_t before[32],after[32],*output=NULL; size_t size=0;
  assert(!anygm_content_transform_run(set,"identity","abc",3,&output,&size,error,sizeof error));
  assert(!output && !size && strstr(error,"missing user program"));
  assert(anygm_content_transforms_parse(set,identity,strlen(identity),error,sizeof error));
  assert(anygm_content_transforms_has(set,"identity")); anygm_content_transforms_hash(set,before);
  assert(!anygm_content_transforms_parse(set,duplicate,strlen(duplicate),error,sizeof error));
  assert(!anygm_content_transforms_parse(set,malformed,strlen(malformed),error,sizeof error));
  anygm_content_transforms_hash(set,after); assert(!memcmp(before,after,32));
  assert(anygm_content_transform_run(set,"identity","abc",3,&output,&size,error,sizeof error));
  assert(size==3 && !memcmp(output,"abc",3)); free(output);
  assert(anygm_content_transforms_parse(set,reject_program,strlen(reject_program),error,sizeof error));
  anygm_content_transforms_hash(set,after); assert(memcmp(before,after,32));
  assert(!anygm_content_transform_run(set,"identity","abc",3,&output,&size,error,sizeof error));
  anygm_content_transforms_destroy(set); set=anygm_content_transforms_create();
  assert(anygm_content_transforms_parse_layer(set,identity,strlen(identity),2,error,sizeof error));
  assert(anygm_content_transforms_parse_layer(set,reject_program,strlen(reject_program),1,error,sizeof error));
  assert(anygm_content_transform_run(set,"identity","abc",3,&output,&size,error,sizeof error));
  assert(size==3 && !memcmp(output,"abc",3)); free(output);
  assert(anygm_content_transforms_parse_layer(set,reject_program,strlen(reject_program),2,error,sizeof error));
  assert(!anygm_content_transform_run(set,"identity","abc",3,&output,&size,error,sizeof error));
  anygm_content_transforms_destroy(set); set=anygm_content_transforms_create();
  const char bom_cr[]="\xef\xbb\xbf[transforms]\ridentity=00001f000000000000000000:\r";
  assert(anygm_content_transforms_parse(set,bom_cr,strlen(bom_cr),error,sizeof error));
  assert(anygm_content_transform_run(set,"identity","abc",3,&output,&size,error,sizeof error));
  assert(size==3 && !memcmp(output,"abc",3)); free(output);
  anygm_content_transforms_destroy(set); set=anygm_content_transforms_create();
  assert(set && anygm_content_transforms_parse(set,ordered,strlen(ordered),error,sizeof error));
  assert(anygm_content_transforms_parse(other,reverse,strlen(reverse),error,sizeof error));
  anygm_content_transforms_hash(set,before); anygm_content_transforms_hash(other,after); assert(!memcmp(before,after,32));
  anygm_content_transforms_destroy(set); anygm_content_transforms_destroy(other);
}
int main(void){
  arithmetic(); buffers_and_branches(); failures(); configuration();
  puts("Content transform isolation, validation, and configuration: ok"); return 0;
}
