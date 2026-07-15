/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me> */
#include "gmlc_bytecode.h"
#include "gml_win.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
    if((word>>24)==OP_DUP && ((word>>16)&0xFF)==DT_VAR && (word&0xFFFF)==0x8800){
      found=1;
      break;
    }
  }
  if(!found) fprintf(stderr,"chained assignment did not preserve its resolved receiver: %s\n",err);
  gmlc_bytecode_free(&blob);
  return ok && found;
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
  ok &= compile_fixture_named_constant(&project,"vk_shift",16);
  ok &= compile_fixture_named_constant(&project,"vk_control",17);
  ok &= compile_fixture_named_constant(&project,"vk_alt",18);
  ok &= compile_fixture_named_constant(&project,"vk_numpad0",96);
  ok &= compile_fixture_named_constant(&project,"vk_numpad9",105);
  ok &= compile_fixture_named_constant(&project,"vk_f12",123);
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
  project.classic_version=0;
  ok &= compile_fixture_function_ref(&project,"fixturemotion();\n","fixturemotion","FixtureMotion");
  ok &= compile_fixture_function_ref(&project,"fixture_alias();\n","fixture_alias","FixtureMotion");
  project.scripts=NULL; project.n_scripts=project.cap_scripts=0;
  project.function_aliases=NULL;
  project.n_function_aliases=project.cap_function_aliases=0;
  ok &= compile_fixture(&project,
    "speed=0\n(instance_create(1,2,3)).hspeed=-.5\ninstance_create(4,5,6)\n"
    "(instance_create(7,8,9)).hspeed=.5\n",1);
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
