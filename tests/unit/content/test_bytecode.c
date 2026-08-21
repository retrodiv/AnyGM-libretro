/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "gmlc_bytecode.h"
#include "gml_win.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void *fixture_file_open(void *userdata,const char *path,AnygmFileMode mode){
  (void)userdata;
  return mode==ANYGM_FILE_READ?fopen(path,"rb"):NULL;
}

static size_t fixture_file_read(void *userdata,void *file,void *data,size_t size){
  (void)userdata;
  return fread(data,1,size,(FILE *)file);
}

static void fixture_file_close(void *userdata,void *file){
  (void)userdata;
  fclose((FILE *)file);
}

static int compile_fixture(const GmlcProject *project, const char *text, int expect_ok){
  char path[]="/tmp/gmlc-bytecode-XXXXXX";
  int descriptor=mkstemp(path);
  if(descriptor<0) return 0;
  FILE *file=fdopen(descriptor,"wb");
  if(!file) return 0;
  size_t length=strlen(text);
  int wrote=fwrite(text,1,length,file)==length;
  fclose(file);
  if(!wrote){ remove(path); return 0; }
  GmlcCodeBlob blob;
  GmlcFunctionRegistry registry;
  char err[256]={0};
  memset(&blob,0,sizeof(blob));
  memset(&registry,0,sizeof(registry));
  int have_registry=project->n_constants>0;
  int registry_ok=!have_registry ||
    gmlc_bytecode_collect_functions(project,0,&registry,err,sizeof(err));
  int ok=registry_ok && (have_registry
    ? gmlc_bytecode_compile_source_ex(project,&registry,-1,path,&blob,err,sizeof(err))
    : gmlc_bytecode_compile_source(project,path,&blob,err,sizeof(err)));
  remove(path);
  int matched=expect_ok ? (ok && !blob.is_placeholder && blob.size>0) :
                          (!ok && blob.is_placeholder && blob.diagnostic);
  if(!matched)
    fprintf(stderr,"bytecode fixture unexpectedly %s: %s%s%s\n",
      ok?"compiled":"failed",err,
      blob.diagnostic?" / ":"",blob.diagnostic?blob.diagnostic:"");
  gmlc_bytecode_free(&blob);
  gmlc_function_registry_free(&registry);
  return matched;
}

static int compile_fixture_has_swap(const GmlcProject *project, const char *text){
  char path[]="/tmp/gmlc-bytecode-swap-XXXXXX";
  int descriptor=mkstemp(path);
  if(descriptor<0) return 0;
  FILE *file=fdopen(descriptor,"wb");
  if(!file) return 0;
  size_t length=strlen(text);
  int wrote=fwrite(text,1,length,file)==length;
  fclose(file);
  if(!wrote){ remove(path); return 0; }
  GmlcCodeBlob blob; char err[256]={0};
  memset(&blob,0,sizeof(blob));
  int ok=gmlc_bytecode_compile_source(project,path,&blob,err,sizeof(err));
  remove(path);
  int found=0;
  for(size_t i=0;ok && i+4<=blob.size;i+=4){
    uint32_t word=(uint32_t)blob.data[i] | ((uint32_t)blob.data[i+1]<<8) |
                  ((uint32_t)blob.data[i+2]<<16) | ((uint32_t)blob.data[i+3]<<24);
    if((word>>24)==OP_DUP && ((word>>16)&0xFF)==DT_VAR && (word&0xFFFF)==0x8801){
      found=1;
      break;
    }
  }
  if(!found) fprintf(stderr,"chained assignment did not preserve its resolved receiver: %s\n",err);
  gmlc_bytecode_free(&blob);
  return ok && found;
}

static int compile_fixture_has_string(const GmlcProject *project,
                                      const char *text,
                                      const char *expected){
  char path[]="/tmp/gmlc-bytecode-string-XXXXXX";
  int descriptor=mkstemp(path);
  if(descriptor<0) return 0;
  FILE *file=fdopen(descriptor,"wb");
  if(!file) return 0;
  size_t length=strlen(text);
  int wrote=fwrite(text,1,length,file)==length;
  fclose(file);
  if(!wrote){ remove(path); return 0; }
  GmlcCodeBlob blob;
  char err[256]={0};
  memset(&blob,0,sizeof(blob));
  int ok=gmlc_bytecode_compile_source(project,path,&blob,err,sizeof(err));
  remove(path);
  int found=0;
  for(int index=0;ok && index<blob.n_strings;index++)
    if(blob.strings[index].value &&
       !strcmp(blob.strings[index].value,expected)){
      found=1;
      break;
    }
  if(!found)
    fprintf(stderr,"long string literal was not preserved: %s\n",err);
  gmlc_bytecode_free(&blob);
  return ok && found;
}

static int compile_fixture_logical_value(const GmlcProject *project,
                                         const char *expression,
                                         int expected){
  char path[]="/tmp/gmlc-bytecode-logical-XXXXXX";
  int descriptor=mkstemp(path);
  if(descriptor<0) return 0;
  FILE *file=fdopen(descriptor,"wb");
  if(!file) return 0;
  char source[256];
  int source_size=snprintf(source,sizeof source,
                           "show_debug_message(%s);\n",expression);
  int wrote=source_size>0 && (size_t)source_size<sizeof source &&
            fwrite(source,1,(size_t)source_size,file)==(size_t)source_size;
  fclose(file);
  if(!wrote){ remove(path); return 0; }
  GmlcCodeBlob blob;
  char err[256]={0};
  memset(&blob,0,sizeof blob);
  int ok=gmlc_bytecode_compile_source(project,path,&blob,err,sizeof err);
  remove(path);

  int stack[32], stack_size=0, observed=-1;
  size_t pc=0, steps=0;
  while(ok && pc+4<=blob.size && steps++<128){
    uint32_t word=(uint32_t)blob.data[pc] |
                  ((uint32_t)blob.data[pc+1]<<8) |
                  ((uint32_t)blob.data[pc+2]<<16) |
                  ((uint32_t)blob.data[pc+3]<<24);
    uint8_t op=(uint8_t)(word>>24);
    if(op==0x84 || (op==OP_PUSH && ((word>>16)&0xFF)==DT_INT16)){
      if(stack_size>=(int)(sizeof stack/sizeof stack[0])){ ok=0; break; }
      stack[stack_size++]=(int16_t)(word&0xFFFF);
      pc+=4;
    } else if(op==OP_CONV){
      if(stack_size<1){ ok=0; break; }
      stack[stack_size-1]=stack[stack_size-1]!=0;
      pc+=4;
    } else if(op==OP_B || op==OP_BT || op==OP_BF){
      int32_t jump=(int32_t)((word&0x7FFFFF)<<9)>>9;
      int take=op==OP_B;
      if(op!=OP_B){
        if(stack_size<1){ ok=0; break; }
        int condition=stack[--stack_size]!=0;
        take=op==OP_BT?condition:!condition;
      }
      pc=take ? (size_t)((int64_t)pc+(int64_t)jump*4) : pc+4;
    } else if(op==OP_CALL){
      if(stack_size<1){ ok=0; break; }
      observed=stack[stack_size-1]!=0;
      break;
    } else {
      fprintf(stderr,"logical precedence fixture reached opcode 0x%02x at byte %zu\n",
              op,pc);
      ok=0;
      break;
    }
  }
  if(!ok || observed!=expected)
    fprintf(stderr,
            "logical precedence fixture '%s' produced %d instead of %d: %s\n",
            expression,observed,expected,err);
  gmlc_bytecode_free(&blob);
  return ok && observed==expected;
}

static int compile_fixture_lacks_ref(const GmlcProject *project, const char *text,
                                     const char *forbidden){
  char path[]="/tmp/gmlc-bytecode-ref-XXXXXX";
  int descriptor=mkstemp(path);
  if(descriptor<0) return 0;
  FILE *file=fdopen(descriptor,"wb");
  if(!file) return 0;
  size_t length=strlen(text);
  int wrote=fwrite(text,1,length,file)==length;
  fclose(file);
  if(!wrote){ remove(path); return 0; }
  GmlcCodeBlob blob; char err[256]={0};
  memset(&blob,0,sizeof(blob));
  int ok=gmlc_bytecode_compile_source(project,path,&blob,err,sizeof(err));
  remove(path);
  int found=0;
  for(int i=0;ok && i<blob.n_refs;i++)
    if(blob.refs[i].name && !strcmp(blob.refs[i].name,forbidden)){ found=1; break; }
  if(!ok || found)
    fprintf(stderr,"classic conditional syntax leaked a '%s' variable reference: %s\n",
            forbidden,err);
  gmlc_bytecode_free(&blob);
  return ok && !found;
}

static int compile_fixture_function_ref(const GmlcProject *project, const char *text,
                                        const char *expected, const char *unexpected){
  char path[]="/tmp/gmlc-bytecode-func-ref-XXXXXX";
  int descriptor=mkstemp(path);
  if(descriptor<0) return 0;
  FILE *file=fdopen(descriptor,"wb");
  if(!file) return 0;
  size_t length=strlen(text);
  int wrote=fwrite(text,1,length,file)==length;
  fclose(file);
  if(!wrote){ remove(path); return 0; }
  GmlcCodeBlob blob; char err[256]={0};
  memset(&blob,0,sizeof(blob));
  int ok=gmlc_bytecode_compile_source(project,path,&blob,err,sizeof(err));
  remove(path);
  int found_expected=0, found_unexpected=0;
  for(int i=0;ok && i<blob.n_refs;i++) if(blob.refs[i].kind==GMLC_REF_FUNC){
    if(blob.refs[i].name && !strcmp(blob.refs[i].name,expected)) found_expected=1;
    if(blob.refs[i].name && !strcmp(blob.refs[i].name,unexpected)) found_unexpected=1;
  }
  if(!ok || !found_expected || found_unexpected)
    fprintf(stderr,"function reference canonicalization mismatch: expected=%s unexpected=%s error=%s\n",
            expected,unexpected,err);
  gmlc_bytecode_free(&blob);
  return ok && found_expected && !found_unexpected;
}

static int compile_fixture_named_constant(const GmlcProject *project, const char *name,
                                          int expected){
  char source[128];
  snprintf(source,sizeof(source),"show_debug_message(%s);\n",name);
  char path[]="/tmp/gmlc-bytecode-constant-XXXXXX";
  int descriptor=mkstemp(path);
  if(descriptor<0) return 0;
  FILE *file=fdopen(descriptor,"wb");
  if(!file) return 0;
  size_t length=strlen(source);
  int wrote=fwrite(source,1,length,file)==length;
  fclose(file);
  if(!wrote){ remove(path); return 0; }
  GmlcCodeBlob blob; char err[256]={0};
  memset(&blob,0,sizeof(blob));
  int ok=gmlc_bytecode_compile_source(project,path,&blob,err,sizeof(err));
  remove(path);
  int matched=0;
  if(ok && blob.size>=4){
    uint32_t word=(uint32_t)blob.data[0] | ((uint32_t)blob.data[1]<<8) |
                  ((uint32_t)blob.data[2]<<16) | ((uint32_t)blob.data[3]<<24);
    matched=(word>>24)==0x84 && (int16_t)(word&0xFFFF)==expected;
  }
  for(int i=0;ok && i<blob.n_refs;i++)
    if(blob.refs[i].kind==GMLC_REF_VARI && blob.refs[i].name &&
       !strcmp(blob.refs[i].name,name)) matched=0;
  if(!matched)
    fprintf(stderr,"named constant %s did not compile to %d: %s\n",name,expected,err);
  gmlc_bytecode_free(&blob);
  return ok && matched;
}

int main(int argc, char **argv){
  GmlcProject project;
  memset(&project,0,sizeof(project));
  AnygmHostServices services={0};
  services.struct_size=sizeof services;
  services.abi_version=ANYGM_HOST_SERVICES_VERSION;
  services.file_open=fixture_file_open;
  services.file_read=fixture_file_read;
  services.file_close=fixture_file_close;
  project.host=&services;
  if(argc>1){
    int all_ok=1;
    for(int i=1;i<argc;i++){
      GmlcCodeBlob blob; char err[256]={0};
      memset(&blob,0,sizeof(blob));
      int ok=gmlc_bytecode_compile_source(&project,argv[i],&blob,err,sizeof(err));
      printf("%s: %s%s%s\n",argv[i],ok?"ok":"failed",
        blob.diagnostic?": ":"",blob.diagnostic?blob.diagnostic:err);
      if(!ok) all_ok=0;
      gmlc_bytecode_free(&blob);
    }
    return all_ok?0:1;
  }
  int ok=1;
  ok &= compile_fixture(&project,
    "result=(instance_place(1,2,3)).object_index;\n",1);
  ok &= compile_fixture(&project,
    "result=instance_nearest(1,2,3).x;\n",1);
  ok &= compile_fixture(&project,
    "if score>0 { show_message_ext(\"q\",\"yes\",\"\",\"no\")\n"
    "case 1: { score=1; }; case 3: { score=3; }; }\n",1);
  ok &= compile_fixture(&project,
    "speed=.5.25; label=\"prefix\"++name;\n",1);
  ok &= compile_fixture(&project,
    "result=\"x\"+global.name+\"!\";\n",1);
  ok &= compile_fixture(&project,
    "show_message(\"first\")\nshow_message(\"second \"+global.name+\"!\");\n",1);
  static const char long_literal[]=
    "neutral_000,neutral_001,neutral_002,neutral_003,neutral_004,"
    "neutral_005,neutral_006,neutral_007,neutral_008,neutral_009,"
    "neutral_010,neutral_011,neutral_012,neutral_013,neutral_014,"
    "neutral_015,neutral_016,neutral_017,neutral_018,neutral_019";
  char long_source[sizeof(long_literal)+40];
  snprintf(long_source,sizeof(long_source),
           "show_debug_message(\"%s\");\n",long_literal);
  ok &= compile_fixture_has_string(&project,long_source,long_literal);
  ok &= compile_fixture_named_constant(&project,"vk_shift",16);
  ok &= compile_fixture_named_constant(&project,"vk_control",17);
  ok &= compile_fixture_named_constant(&project,"vk_alt",18);
  ok &= compile_fixture_named_constant(&project,"vk_return",13);
  ok &= compile_fixture_named_constant(&project,"vk_lshift",160);
  ok &= compile_fixture_named_constant(&project,"vk_ralt",165);
  ok &= compile_fixture_named_constant(&project,"mb_any",-1);
  ok &= compile_fixture_named_constant(&project,"mb_none",0);
  ok &= compile_fixture_named_constant(&project,"mb_left",1);
  ok &= compile_fixture_named_constant(&project,"mb_right",2);
  ok &= compile_fixture_named_constant(&project,"mb_middle",3);
  ok &= compile_fixture_named_constant(&project,"vk_numpad0",96);
  ok &= compile_fixture_named_constant(&project,"vk_numpad9",105);
  ok &= compile_fixture_named_constant(&project,"vk_f12",123);
  ok &= compile_fixture_named_constant(&project,"ev_create",0);
  ok &= compile_fixture_named_constant(&project,"ev_step",3);
  ok &= compile_fixture_named_constant(&project,"ev_draw",8);
  ok &= compile_fixture_named_constant(&project,"ev_cleanup",12);
  ok &= compile_fixture_named_constant(&project,"ev_step_end",2);
  ok &= compile_fixture_named_constant(&project,"ev_draw_normal",0);
  ok &= compile_fixture_named_constant(&project,"ev_user0",10);
  ok &= compile_fixture_named_constant(&project,"ev_user7",17);
  ok &= compile_fixture_named_constant(&project,"ev_user15",25);
  ok &= compile_fixture_lacks_ref(&project,
    "if (score=0) then { result=1; } else result=2;\n","then");
  ok &= compile_fixture_lacks_ref(&project,
    "if (score=0) { result=1; }; else { result=2; };\n","else");
  ok &= compile_fixture(&project,
    "global.actor.part.node.x=4; result=global.actor.part.node.x;\n",1);
  ok &= compile_fixture_has_swap(&project,
    "global.actor.y=10;\n");
  GmlcScript case_scripts[2];
  memset(case_scripts,0,sizeof(case_scripts));
  case_scripts[0].name=(char*)"FixtureMotion";
  case_scripts[1].name=(char*)"AlternateMotion";
  project.scripts=case_scripts; project.n_scripts=project.cap_scripts=2;
  project.classic_version=800;
  ok &= compile_fixture_logical_value(&project,"1 or 0 and 0",0);
  ok &= compile_fixture_logical_value(&project,"1 || 0 && 0",0);
  ok &= compile_fixture_function_ref(&project,"fixturemotion();\n","FixtureMotion","fixturemotion");
  GmlcFunctionAlias aliases[]={
    {(char*)"fixture_alias",(char*)"FixtureMotion",0},
    {(char*)"fixture_route",(char*)"AlternateMotion",0},
    {(char*)"uncertain_alias",(char*)"AlternateMotion",1},
    {(char*)"fixturemotion",(char*)"AlternateMotion",0}
  };
  project.function_aliases=aliases;
  project.n_function_aliases=project.cap_function_aliases=4;
  ok &= compile_fixture_function_ref(&project,"FIXTURE_ALIAS();\n","FixtureMotion","FIXTURE_ALIAS");
  ok &= compile_fixture_function_ref(&project,"FixtureRoute();\n","AlternateMotion","FixtureRoute");
  ok &= compile_fixture_function_ref(&project,"uncertain_alias();\n","uncertain_alias","AlternateMotion");
  ok &= compile_fixture_function_ref(&project,"fixturemotion();\n","FixtureMotion","AlternateMotion");
  ok &= compile_fixture(&project,
    "for (counter=0; counter<limit counter=counter+1) { total+=counter; }\n",1);
  ok &= compile_fixture(&project,
    "switch status begin\n"
    "case \"ready\": result=1; break;\n"
    "case \"waiting\": result=2; break;\n"
    "default: result=0;\n"
    "end;\n",1);
  ok &= compile_fixture(&project,
    "items[index].x=other_items[index].x; result=items[index].x;\n",1);
  ok &= compile_fixture(&project,
    "base=\"folder\\leaf\"; tail=\"folder\\\"+name;\n",1);
  project.classic_version=0;
  ok &= compile_fixture_logical_value(&project,"1 or 0 and 0",1);
  ok &= compile_fixture_logical_value(&project,"1 || 0 && 0",1);
  ok &= compile_fixture_function_ref(&project,"fixturemotion();\n","fixturemotion","FixtureMotion");
  ok &= compile_fixture_function_ref(&project,"fixture_alias();\n","fixture_alias","FixtureMotion");
  project.scripts=NULL; project.n_scripts=project.cap_scripts=0;
  project.function_aliases=NULL;
  project.n_function_aliases=project.cap_function_aliases=0;
  ok &= compile_fixture(&project,
    "speed=0\n(instance_create(1,2,3)).hspeed=-.5\ninstance_create(4,5,6)\n"
    "(instance_create(7,8,9)).hspeed=.5\n",1);
  ok &= compile_fixture(&project,
    "(instance_create(1,2,3)).alarm[0]=32;\n",1);
  GmlcProjectConstant constants[]={
    {(char*)"fixture_base",(char*)"6*7"},
    {(char*)"fixture_nested",(char*)"fixture_base+1"},
    {(char*)"fixture_string",(char*)"\"ready\""}
  };
  project.constants=constants; project.n_constants=project.cap_constants=3;
  ok &= compile_fixture(&project,
    "result=fixture_nested; label=fixture_string+string(fixture_base);\n",1);
  project.constants=NULL; project.n_constants=project.cap_constants=0;
  ok &= compile_fixture(&project,"result=;\n",0);
  if(ok) puts("gmlc bytecode fixtures: ok");
  return ok?0:1;
}
