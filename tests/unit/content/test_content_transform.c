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
  const char identity[]="[transforms]\nidentity=buffer identity() { return slice(0, input_size); }\n";
  const char reject_program[]="[transforms]\nidentity=buffer stop() { reject(); }\n";
  const char duplicate[]="[transforms]\nidentity=buffer copy() { return slice(0,input_size); }\nidentity=buffer stop() { reject(); }\n";
  const char malformed[]="[transforms]\nidentity=buffer stop() { reject(); }\nsecond=invalid\n";
  const char ordered[]="[transforms]\none=buffer copy() { parameters(\"a\"); return slice(0,input_size); }\ntwo=buffer copy() { parameters(\"b\"); return slice(0,input_size); }\n";
  const char reverse[]="[transforms]\ntwo=buffer copy() { parameters(\"b\"); return slice(0,input_size); }\none=buffer copy() { parameters(\"a\"); return slice(0,input_size); }\n";
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
  const char bom_cr[]="\xef\xbb\xbf[transforms]\ridentity=buffer copy() { return slice(0,input_size); }\r";
  assert(anygm_content_transforms_parse(set,bom_cr,strlen(bom_cr),error,sizeof error));
  assert(anygm_content_transform_run(set,"identity","abc",3,&output,&size,error,sizeof error));
  assert(size==3 && !memcmp(output,"abc",3)); free(output);
  anygm_content_transforms_destroy(set); set=anygm_content_transforms_create();
  assert(set && anygm_content_transforms_parse(set,ordered,strlen(ordered),error,sizeof error));
  assert(anygm_content_transforms_parse(other,reverse,strlen(reverse),error,sizeof error));
  anygm_content_transforms_hash(set,before); anygm_content_transforms_hash(other,after); assert(!memcmp(before,after,32));
  anygm_content_transforms_destroy(set); anygm_content_transforms_destroy(other);
}
static void source_programs(void){
  const char source[]=
    "[transforms]\n"
    "edit = buffer edit_bytes() {\n"
    "  parameters(\"A\\x00\\\\\\\"\");\n"
    "  // The original input remains separate from work.\n"
    "  uint64_t total = 0;\n"
    "  for (uint64_t i = 0; i < input_size; i++) {\n"
    "    if (i == 1) continue;\n"
    "    if (i >= 4) break;\n"
    "    total += read8(input, i);\n"
    "  }\n"
    "  write8(work, 0, total);\n"
    "  uint64_t i = 0;\n"
    "  while (i < 4) { write8(scratch, i, read8(parameters, i)); i++; }\n"
    "  write32(work, 4, read32(scratch, 0));\n"
    "  if (0 && read8(input, input_size)) reject();\n"
    "  if (1 || (1 / 0)) { write8(work, 1, 2 + 3 * 4 << 1); }\n"
    "  if (3 < 2) reject(); else if (2 <= 2 && 3 > 2 && 3 != 4) write8(work, 2, ~0);\n"
    "  write8(work, 3, (1 < 2 < 2) + !7 + ('Z' - 'A'));\n"
    "  return slice(0, input_size);\n"
    "}\n[other]\nunchanged=value\n";
  AnygmContentTransforms *set=anygm_content_transforms_create();
  uint8_t input[]={1,2,3,4,5,6,7,8},*output=NULL;
  const uint8_t expected[]={8,28,255,26,'A',0,'\\','"'};
  size_t size=0; char error[256];
  assert(set && anygm_content_transforms_parse(set,source,strlen(source),error,sizeof error));
  assert(anygm_content_transform_run(set,"edit",input,sizeof input,&output,&size,error,sizeof error));
  assert(size==sizeof expected && !memcmp(output,expected,size)); free(output);
  assert(input[0]==1 && input[7]==8);
  uint8_t before[32],after[32]; anygm_content_transforms_hash(set,before);
  const char *invalid[]={
    "00001f000000000000000000:",
    "buffer bad() { uint64_t x = x; return slice(0,0); }",
    "buffer bad() { input_size = 1; }",
    "buffer bad() { write8(input,0,1); }",
    "buffer bad() { write32(parameters,0,1); }",
    "buffer bad() { return slice(0,0); unknown(); }",
    "buffer bad() { break; }", "buffer bad() { continue; }",
    "buffer bad() { uint64_t x = 18446744073709551616; }",
    "buffer bad() { uint64_t x = 0x; }", "buffer bad() { uint64_t x = 010; }",
    "buffer bad() { uint64_t x = 1.5; }",
    "buffer bad() { uint64_t x = 0; { uint64_t x = 1; } }",
    "buffer bad() { if (1) uint64_t x = 0; x = 1; }",
    "buffer bad() { for(uint64_t i=0;i<1;i++) {} i=0; }",
    "buffer bad() { parameters(\"\\q\"); }",
    "buffer bad() { parameters(\"\\x0\"); }",
    "buffer bad() { /* unfinished", "buffer bad() { return slice(0,0);",
    "buffer bad() {} trailing", "buffer bad(int x) {}", "buffer bad() { malloc(4); }"
  };
  for(size_t i=0;i<sizeof invalid/sizeof invalid[0];i++){
    char config[1024]; snprintf(config,sizeof config,"[transforms]\nedit=%s\n",invalid[i]);
    assert(!anygm_content_transforms_parse(set,config,strlen(config),error,sizeof error));
    assert(error[0]); anygm_content_transforms_hash(set,after); assert(!memcmp(before,after,32));
  }
  const char *runtime_failures[]={
    "return slice(0,input_size+1);", "write64(work,1,0);", "uint64_t x=1/0;",
    "uint64_t x=1<<64;", "uint64_t x=read8(input,0xffffffffffffffff);",
    "while(1) {}", "uint64_t x=0;"
  };
  for(size_t i=0;i<sizeof runtime_failures/sizeof runtime_failures[0];i++){
    char config[256]; snprintf(config,sizeof config,"[transforms]\nbad=buffer bad() { %s }\n",runtime_failures[i]);
    assert(anygm_content_transforms_parse(set,config,strlen(config),error,sizeof error));
    assert(!anygm_content_transform_run(set,"bad",input,sizeof input,&output,&size,error,sizeof error));
    assert(!output && !size && input[0]==1 && input[7]==8);
  }
  const char diagnostic[]="[transforms]\nbad=buffer bad() {\n  uint64_t x = missing;\n}\n";
  assert(!anygm_content_transforms_parse(set,diagnostic,strlen(diagnostic),error,sizeof error));
  assert(strstr(error,"'bad'") && strstr(error,"line 2, column 16"));
  anygm_content_transforms_destroy(set);
}
static void source_limits(void){
  AnygmContentTransforms *set=anygm_content_transforms_create();
  char *config=(char*)malloc(70000),error[256]; assert(set && config);
  size_t at=(size_t)sprintf(config,"[transforms]\nlimit=buffer bounded() { parameters(\"");
  memset(config+at,'a',4096); at+=4096;
  strcpy(config+at,"\"); return slice(0,0); }\n");
  assert(anygm_content_transforms_parse(set,config,strlen(config),error,sizeof error));
  memmove(config+at+1,config+at,strlen(config+at)+1); config[at]='a';
  assert(!anygm_content_transforms_parse(set,config,strlen(config),error,sizeof error));
  assert(strstr(error,"parameter limit"));
  at=(size_t)sprintf(config,"[transforms]\nlimit=buffer bounded() { uint64_t value=");
  memset(config+at,'(',100); at+=100; config[at++]='1';
  memset(config+at,')',100); at+=100; strcpy(config+at,"; }\n");
  assert(!anygm_content_transforms_parse(set,config,strlen(config),error,sizeof error));
  assert(strstr(error,"nesting limit"));
  at=(size_t)sprintf(config,"[transforms]\nlimit=buffer bounded() {\n");
  for(unsigned i=0;i<32;i++) at+=(size_t)sprintf(config+at,"uint64_t value%u = %u;\n",i,i);
  strcpy(config+at,"}\n");
  assert(!anygm_content_transforms_parse(set,config,strlen(config),error,sizeof error));
  assert(strstr(error,"too many live"));
  at=(size_t)sprintf(config,"[transforms]\nlimit=buffer bounded() { uint64_t x=0;\n");
  for(unsigned i=0;i<600;i++){ memcpy(config+at,"x++;\n",5); at+=5; }
  strcpy(config+at,"}\n");
  assert(!anygm_content_transforms_parse(set,config,strlen(config),error,sizeof error));
  assert(strstr(error,"instruction limit"));
  at=(size_t)sprintf(config,"[transforms]\nlimit=buffer bounded() { /*");
  memset(config+at,'a',66000); at+=66000; strcpy(config+at,"*/ }\n");
  assert(!anygm_content_transforms_parse(set,config,strlen(config),error,sizeof error));
  uint8_t before[32],after[32]; anygm_content_transforms_hash(set,before);
  /* Deterministic malformed-source mutations exercise lexer and parser rejection. */
  uint32_t rng=1234567;
  static const char sample[]="[transforms]\nlimit=buffer bounded() { uint64_t value=read8(input,0); return slice(value,input_size-value); }\n";
  for(unsigned i=0;i<2000;i++){
    memcpy(config,sample,sizeof sample);
    rng=rng*1664525u+1013904223u; size_t offset=13+rng%(sizeof sample-14);
    rng=rng*1664525u+1013904223u; config[offset]=(char)(rng&127);
    if(!anygm_content_transforms_parse(set,config,sizeof sample-1,error,sizeof error)){
      anygm_content_transforms_hash(set,after); assert(!memcmp(before,after,32));
    } else anygm_content_transforms_hash(set,before);
  }
  free(config); anygm_content_transforms_destroy(set);
}
int main(void){
  arithmetic(); buffers_and_branches(); failures(); configuration(); source_programs(); source_limits();
  puts("Content transform isolation, validation, and configuration: ok"); return 0;
}
