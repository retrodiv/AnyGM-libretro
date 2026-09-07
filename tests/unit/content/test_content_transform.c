/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "content_transform.h"
#include "content_source.h"
#include "gml_image_codec.h"
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
static void pipelines(void){
  const char config[]=
    "[transforms]\n"
    "unwrap=buffer remove_header() { if(input_size<4) reject(); return slice(4,input_size-4); }\n"
    "patch=buffer normalize_header() { write32(work,0,0x4d524f46); return slice(0,input_size); }\n"
    "[pipelines]\n"
    "input=unwrap | decompress | builtin.byteswap16 | patch\n"
    "decompress=builtin.zlib:64\n";
  char error[256]; uint8_t before[32],after[32];
  AnygmContentTransforms *set=anygm_content_transforms_create(),*copy=anygm_content_transforms_create();
  assert(set && copy && anygm_content_transforms_parse(set,config,sizeof config-1,error,sizeof error));
  assert(anygm_content_transform_validate(set,"input",error,sizeof error));
  assert(anygm_content_transforms_copy(copy,set));
  anygm_content_transforms_hash(set,before); anygm_content_transforms_hash(copy,after);
  assert(!memcmp(before,after,32));
  const uint8_t plain[]={0,0,0,0,2,1,4,3};
  const uint8_t expected[]={'F','O','R','M',1,2,3,4};
  GmlMediaBuffer compressed={0};
  assert(gml_deflate_encode_zlib(plain,sizeof plain,&compressed));
  uint8_t *wrapped=malloc(compressed.size+4); assert(wrapped);
  memcpy(wrapped,"WRAP",4); memcpy(wrapped+4,compressed.data,compressed.size);
  uint8_t *output=NULL; size_t size=0;
  assert(anygm_content_transform_run(copy,"input",wrapped,compressed.size+4,
    &output,&size,error,sizeof error));
  assert(size==sizeof expected && !memcmp(output,expected,size)); free(output);
  assert(!memcmp(wrapped,"WRAP",4) && !memcmp(wrapped+4,compressed.data,compressed.size));
  wrapped[compressed.size+3u]^=1u;
  assert(!anygm_content_transform_run(copy,"input",wrapped,compressed.size+4,
    &output,&size,error,sizeof error));
  assert(!output && !size); wrapped[compressed.size+3u]^=1u;
  assert(anygm_content_transform_run(copy,"builtin.deflate:64",compressed.data+2u,compressed.size-6u,
    &output,&size,error,sizeof error));
  assert(size==sizeof plain && !memcmp(output,plain,size)); free(output);
  const char small[]="[pipelines]\ndecompress=builtin.zlib:4\n";
  assert(anygm_content_transforms_parse_layer(set,small,sizeof small-1,2,error,sizeof error));
  anygm_content_transforms_hash(set,after); assert(memcmp(before,after,32));
  assert(!anygm_content_transform_run(set,"input",wrapped,compressed.size+4,
    &output,&size,error,sizeof error));
  assert(!output && !size && error[0]);
  assert(anygm_content_transforms_parse_layer(set,config,sizeof config-1,1,error,sizeof error));
  assert(!anygm_content_transform_run(set,"input",wrapped,compressed.size+4,
    &output,&size,error,sizeof error));
  assert(!output && !size);
  free(wrapped); gml_media_buffer_release(&compressed);
  const char *invalid[]={
    "[pipelines]\nx=\n", "[pipelines]\nx=|patch\n", "[pipelines]\nx=patch|\n",
    "[pipelines]\nx=patch||patch\n", "[pipelines]\nx=patch| \n",
    "[pipelines]\nx=builtin.zlib:0\n", "[pipelines]\nx=builtin.zlib:1073741825\n",
    "[pipelines]\nx=builtin.zlib:18446744073709551616\n",
    "[pipelines]\nx=builtin.deflate:01\n", "[pipelines]\nx=builtin.missing\n",
    "[pipelines]\nbuiltin.byteswap16=patch\n", "[pipelines]\nx=patch\nx=patch\n",
    "[pipelines]\nx=patch\n[pipelines]\ny=patch\n",
    "[transforms]\nx=buffer x(){return slice(0,input_size);}\n[pipelines]\nx=patch\n"
  };
  anygm_content_transforms_hash(set,before);
  for(size_t i=0;i<sizeof invalid/sizeof invalid[0];i++){
    assert(!anygm_content_transforms_parse(set,invalid[i],strlen(invalid[i]),error,sizeof error));
    anygm_content_transforms_hash(set,after); assert(!memcmp(before,after,32));
  }
  const char cycle[]="[pipelines]\nx=y\ny=x\n";
  assert(anygm_content_transforms_parse(set,cycle,sizeof cycle-1,error,sizeof error));
  assert(!anygm_content_transform_validate(set,"x",error,sizeof error));
  assert(!anygm_content_transform_run(set,"x",plain,sizeof plain,&output,&size,error,sizeof error));
  assert(!output && !size);
  const char excess[]="[pipelines]\nx=patch|patch|patch|patch|patch|patch|patch|patch|patch\n"
    "y=x|x\n";
  assert(anygm_content_transforms_parse(set,excess,sizeof excess-1,error,sizeof error));
  assert(!anygm_content_transform_validate(set,"y",error,sizeof error));
  assert(!anygm_content_transform_run(set,"y",plain,sizeof plain,&output,&size,error,sizeof error));
  assert(!output && !size);
  const char missing[]="[pipelines]\nx=patch|missing\n";
  assert(anygm_content_transforms_parse(set,missing,sizeof missing-1,error,sizeof error));
  assert(!anygm_content_transform_validate(set,"x",error,sizeof error));
  assert(!anygm_content_transform_run(set,"x",plain,sizeof plain,&output,&size,error,sizeof error));
  assert(!output && !size);
  assert(!anygm_content_transform_run(set,"builtin.byteswap16",plain,3,&output,&size,error,sizeof error));
  assert(!output && !size);
  assert(anygm_content_transform_run(set,"builtin.byteswap32",plain,sizeof plain,&output,&size,error,sizeof error));
  assert(size==sizeof plain && output[4]==3 && output[5]==4 && output[6]==1 && output[7]==2);
  free(output);
  anygm_content_transforms_destroy(set); anygm_content_transforms_destroy(copy);
}

static void distributed_adapters(void){
  FILE *file=fopen("examples/input_transforms.ini","rb");
  assert(file);
  char config[8192]; size_t length=fread(config,1,sizeof config,file);
  assert(length<sizeof config && !ferror(file) && !fclose(file));
  AnygmContentTransforms *set=anygm_content_transforms_create();
  char error[256];
  assert(set && anygm_content_transforms_parse(set,config,length,error,sizeof error));
  /* A synthetic single-member ZIP concatenated after an inert data prefix.
   * Central-directory offsets are relative to the original standalone archive. */
  uint8_t input[256]={0};
  const size_t prefix=64,local_size=32,central_size=47,end=prefix+local_size+central_size;
  memcpy(input,"WRAP",4);
  memcpy(input+prefix,"PK\003\004",4);
  memcpy(input+prefix+14,"\xd3\xff\x6b\x9e",4);
  input[prefix+4]=20; input[prefix+18]=1; input[prefix+22]=1; input[prefix+26]=1;
  input[prefix+30]='x'; input[prefix+31]='!';
  memcpy(input+prefix+local_size,"PK\001\002",4);
  memcpy(input+prefix+local_size+16,"\xd3\xff\x6b\x9e",4);
  input[prefix+local_size+4]=20; input[prefix+local_size+6]=20;
  input[prefix+local_size+20]=1; input[prefix+local_size+24]=1;
  input[prefix+local_size+28]=1; input[prefix+local_size+46]='x';
  memcpy(input+end,"PK\005\006",4);
  input[end+8]=input[end+10]=1;
  input[end+12]=(uint8_t)central_size; input[end+16]=(uint8_t)local_size;
  uint8_t *output=NULL; size_t size=0;
  assert(anygm_content_transform_run(set,"embedded_zip",input,end+22,&output,&size,error,sizeof error));
  assert(size==end+22-prefix && !memcmp(output,input+prefix,size)); free(output);
  assert(!memcmp(input,"WRAP",4));
  /* Changing the locator, directory framing or terminal comment length must not
   * turn a matching signature elsewhere in the input into an accepted archive. */
  const size_t corruptions[]={end+4,end+8,end+10,end+12,end+16,end+20,prefix+local_size,prefix};
  for(size_t i=0;i<sizeof corruptions/sizeof corruptions[0];i++){
    input[corruptions[i]]^=128;
    assert(!anygm_content_transform_run(set,"embedded_zip",input,end+22,&output,&size,error,sizeof error));
    assert(!output && !size); input[corruptions[i]]^=128;
  }
  assert(anygm_content_transform_run(set,"little_endian_words","abcd",4,&output,&size,error,sizeof error));
  assert(size==4 && !memcmp(output,"dcba",4)); free(output);
  GmlMediaBuffer compressed={0};
  assert(gml_deflate_encode_zlib((const uint8_t*)"payload",7,&compressed));
  assert(compressed.size+4<=sizeof input);
  memset(input,0,4); input[0]=4; memcpy(input+4,compressed.data,compressed.size);
  assert(anygm_content_transform_run(set,"sized_zlib",input,compressed.size+4,&output,&size,error,sizeof error));
  assert(size==7 && !memcmp(output,"payload",7)); free(output);
  gml_media_buffer_release(&compressed); anygm_content_transforms_destroy(set);
}

static void scratch_results(void){
  const char config[]="[transforms]\nexpand=buffer expand(){write32(scratch,65532,0x44434241);"
    "return scratch_slice(65532,4);}\nempty=buffer empty(){return scratch_slice(65536,0);}\n"
    "bad=buffer bad(){return scratch_slice(65536,1);}\n";
  AnygmContentTransforms *set=anygm_content_transforms_create();
  uint8_t *output=NULL; size_t size=0; char error[256];
  assert(set && anygm_content_transforms_parse(set,config,sizeof config-1,error,sizeof error));
  assert(anygm_content_transform_run(set,"expand",NULL,0,&output,&size,error,sizeof error));
  assert(size==4 && !memcmp(output,"ABCD",4)); free(output);
  assert(anygm_content_transform_run(set,"empty",NULL,0,&output,&size,error,sizeof error));
  assert(output && !size); free(output);
  assert(!anygm_content_transform_run(set,"bad",NULL,0,&output,&size,error,sizeof error));
  assert(!output && !size);
  anygm_content_transforms_destroy(set);
  Program p={0}; emit(&p,ANYGM_TRANSFORM_RETURN,2,31,0,0); reject(&p);
}

typedef struct { unsigned calls; int fatal; } CandidateCheck;
static int validate_candidate(void *context,const void *data,size_t size,char *error,size_t capacity){
  CandidateCheck *check=context;
  check->calls++;
  if(check->fatal && check->calls==(unsigned)check->fatal){
    snprintf(error,capacity,"fatal validator failure"); return -1;
  }
  int valid=size==4 && !memcmp(data,"GOOD",4);
  if(!valid) snprintf(error,capacity,"invalid authored record");
  return valid;
}

static void candidate_sources(void){
  static const struct { const char *probe; const char *data; int ok; unsigned calls; const char *result; } cases[]={
    {"write64(scratch,0,0);write64(scratch,8,8);write64(scratch,16,4);write64(scratch,24,4);"
     "return scratch_slice(0,32);","bad!GOOD",1,2,"GOOD"},
    {"write64(scratch,0,0);write64(scratch,8,4);write64(scratch,16,4);write64(scratch,24,4);"
     "return scratch_slice(0,32);","GOODGOOD",0,2,NULL},
    {"write64(scratch,0,4);write64(scratch,8,4);return scratch_slice(0,16);","bad!bad!",0,1,NULL},
    {"return scratch_slice(0,0);","ignored",1,0,NULL},
    {"write64(scratch,8,input_size);return scratch_slice(0,16);","unchanged",1,0,NULL},
    /* Whole-table checks precede every callback, even when the first row is valid. */
    {"write64(scratch,8,4);write64(scratch,16,99);return scratch_slice(0,32);","GOOD",0,0,NULL},
    {"write64(scratch,8,4);return scratch_slice(0,32);","GOOD",0,0,NULL},
    {"write64(scratch,0,3);write64(scratch,8,2);return scratch_slice(0,16);","GOOD",0,0,NULL},
    {"write64(scratch,0,0xffffffffffffffff);return scratch_slice(0,16);","GOOD",0,0,NULL},
    {"return scratch_slice(0,15);","GOOD",0,0,NULL},
    {"return scratch_slice(0,1040);","GOOD",0,0,NULL},
  };
  for(size_t i=0;i<sizeof cases/sizeof cases[0];i++){
    char config[2048],error[256];
    snprintf(config,sizeof config,"[transforms]\ninput=buffer copy(){return slice(0,input_size);}\n"
      "input.probe=buffer probe(){%s}\n",cases[i].probe);
    AnygmContentTransforms *set=anygm_content_transforms_create();
    assert(set && anygm_content_transforms_parse(set,config,strlen(config),error,sizeof error));
    CandidateCheck check={0}; uint8_t *output=(uint8_t*)set; size_t size=99;
    char original[32]; strcpy(original,cases[i].data);
    int ok=anygm_content_source_prepare(set,original,strlen(original),validate_candidate,&check,
      &output,&size,error,sizeof error);
    assert(ok==cases[i].ok && check.calls==cases[i].calls && !strcmp(original,cases[i].data));
    if(cases[i].result) assert(size==4 && output && !memcmp(output,cases[i].result,4));
    else assert(!output && !size);
    if(!ok) assert(error[0]);
    free(output); anygm_content_transforms_destroy(set);
  }
  /* A fatal validator result cannot silently choose a previously valid candidate. */
  const char fatal_config[]="[transforms]\ninput=buffer copy(){return slice(0,input_size);}\n"
    "input.probe=buffer probe(){write64(scratch,8,4);write64(scratch,16,4);write64(scratch,24,4);"
    "return scratch_slice(0,32);}\n";
  char error[256]; AnygmContentTransforms *set=anygm_content_transforms_create();
  assert(set && anygm_content_transforms_parse(set,fatal_config,sizeof fatal_config-1,error,sizeof error));
  CandidateCheck check={0,2}; uint8_t *output=NULL; size_t size=0;
  assert(!anygm_content_source_prepare(set,"GOODGOOD",8,validate_candidate,&check,
    &output,&size,error,sizeof error) && !output && !size && check.calls==2);
  assert(strstr(error,"fatal validator"));
  assert(!anygm_content_source_prepare(set,"bad!GOOD",8,NULL,NULL,&output,&size,error,sizeof error));
  anygm_content_transforms_destroy(set);
  const char missing[]="[transforms]\ninput.probe=buffer none(){return scratch_slice(0,0);}\n";
  set=anygm_content_transforms_create();
  assert(set && anygm_content_transforms_parse(set,missing,sizeof missing-1,error,sizeof error));
  check.calls=0;
  assert(!anygm_content_source_prepare(set,"GOOD",4,validate_candidate,&check,
    &output,&size,error,sizeof error) && !check.calls && !output && !size);
  anygm_content_transforms_destroy(set);
  const char maximum[]="[transforms]\ninput=buffer copy(){return slice(0,input_size);}\n"
    "input.probe=buffer ranges(){for(uint64_t i=0;i<64;i++){write64(scratch,16*i,i);"
    "write64(scratch,16*i+8,1);}return scratch_slice(0,1024);}\n";
  uint8_t bytes[64]={0};
  set=anygm_content_transforms_create();
  assert(set && anygm_content_transforms_parse(set,maximum,sizeof maximum-1,error,sizeof error));
  check.calls=0; check.fatal=0;
  assert(!anygm_content_source_prepare(set,bytes,sizeof bytes,validate_candidate,&check,
    &output,&size,error,sizeof error) && check.calls==64 && !output && !size);
  anygm_content_transforms_destroy(set);
  assert(anygm_content_source_prepare(NULL,NULL,0,NULL,NULL,&output,&size,error,sizeof error));
  assert(!output && !size);
}

int main(void){
  arithmetic(); buffers_and_branches(); failures(); configuration(); source_programs(); source_limits();
  pipelines();
  distributed_adapters();
  scratch_results(); candidate_sources();
  puts("Content transform isolation, validation, and configuration: ok"); return 0;
}
