/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "content_config.h"
#include "content_source.h"
#include "gml_hash.h"
#include "../../support/anygm_test_runner.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Independently authored COPY/ADD windows, without proprietary fixture bytes. */
static const uint8_t first_patch[] = {
  0xd6,0xc3,0xc4,0,0,1,3,0,12,6,0,3,3,1,'d','e','f',19,3,4,0
};
static const uint8_t second_patch[] = {
  0xd6,0xc3,0xc4,0,0,1,6,0,10,7,0,1,3,1,'!',19,6,2,0
};

static void hex(const void *data,size_t size,char out[65]){
  uint8_t digest[32];
  gml_sha256(data,size,digest);
  for(size_t i=0;i<32;i++) snprintf(out+i*2,3,"%02x",digest[i]);
}

static void chain_text(char *out,size_t capacity,const char *scope,const char *order){
  char source[65],middle[65],result[65],first[65],second[65];
  hex("abc",3,source); hex("abcdef",6,middle); hex("abcdef!",7,result);
  hex(first_patch,sizeof first_patch,first); hex(second_patch,sizeof second_patch,second);
  int size=snprintf(out,capacity,
    "[%spatches]\nfirst=xdelta | patches/first.xdelta | %s | %s | %s | 6\n"
    "second=xdelta | patches/second.xdelta | %s | %s | %s | 7\n"
    "[%spipelines]\ninput.final=%s\n",
    scope,source,first,middle,middle,second,result,scope,order);
  assert(size>0 && (size_t)size<capacity);
}

static void bind_one(AnygmContentTransforms *set,const char *name,const void *patch,size_t size){
  uint8_t *bytes=malloc(size);
  char error[256];
  assert(bytes); memcpy(bytes,patch,size);
  assert(anygm_content_transforms_bind_patch(set,name,&bytes,size,error,sizeof error));
  assert(!bytes);
}

static AnygmContentTransforms *make_chain(const char *order,int bind){
  char text[2048],error[256];
  chain_text(text,sizeof text,"",order);
  AnygmContentTransforms *set=anygm_content_transforms_create();
  assert(set && anygm_content_transforms_parse(set,text,strlen(text),error,sizeof error));
  assert(anygm_content_source_configured(set));
  if(bind){
    bind_one(set,"first",first_patch,sizeof first_patch);
    bind_one(set,"second",second_patch,sizeof second_patch);
  }
  return set;
}

static void expect_chain(AnygmContentTransforms *set){
  uint8_t source[]="abc",*output=NULL;
  size_t size=0;
  char error[256];
  int ok=anygm_content_source_prepare(set,source,3,NULL,NULL,&output,&size,error,sizeof error);
  if(!ok) fprintf(stderr,"%s\n",error);
  assert(ok && output && size==7 && !memcmp(output,"abcdef!",7));
  assert(!strcmp((const char*)source,"abc"));
  free(output);
}

static int ordered(void){
  AnygmContentTransforms *set=make_chain("first | second",1);
  expect_chain(set);
  assert(anygm_content_transforms_patch_count(set)==2);
  AnygmContentPatchSpec spec; char name[64];
  assert(anygm_content_transforms_patch_at(set,0,name,&spec));
  assert(!strcmp(name,"first") && !strcmp(spec.path,"patches/first.xdelta") && spec.result_size==6);
  assert(!anygm_content_transforms_patch_at(set,2,name,&spec));
  anygm_content_transforms_destroy(set); return 1;
}

static int reversed(void){
  AnygmContentTransforms *set=make_chain("second | first",1);
  uint8_t *output=(uint8_t*)(uintptr_t)1; size_t size=99; char error[256];
  assert(!anygm_content_source_prepare(set,"abc",3,NULL,NULL,&output,&size,error,sizeof error));
  assert(!output && !size && strstr(error,"base or patch order"));
  anygm_content_transforms_destroy(set); return 1;
}

static int identities(void){
  AnygmContentTransforms *set=make_chain("first | second",1);
  uint8_t before[32],after[32]; char error[256];
  anygm_content_transforms_hash(set,before);
  uint8_t *bad=malloc(sizeof first_patch),*saved;
  assert(bad); memcpy(bad,first_patch,sizeof first_patch); bad[15]^=1; saved=bad;
  assert(!anygm_content_transforms_bind_patch(set,"first",&bad,sizeof first_patch,error,sizeof error));
  assert(bad==saved && strstr(error,"SHA-256 mismatch")); free(bad);
  anygm_content_transforms_hash(set,after); assert(!memcmp(before,after,32));
  expect_chain(set);
  uint8_t *output=NULL; size_t size=0;
  assert(!anygm_content_source_prepare(set,"abd",3,NULL,NULL,&output,&size,error,sizeof error));
  assert(!output && !size && strstr(error,"source SHA-256"));

  char first_hash[65],base_hash[65],wrong_hash[65],text[1024];
  hex(first_patch,sizeof first_patch,first_hash); hex("abc",3,base_hash); hex("wrong",5,wrong_hash);
  snprintf(text,sizeof text,"[patches]\nfirst=xdelta|first.xdelta|%s|%s|%s|6\n",
    base_hash,first_hash,wrong_hash);
  assert(anygm_content_transforms_parse_layer(set,text,strlen(text),5,error,sizeof error));
  bind_one(set,"first",first_patch,sizeof first_patch);
  assert(!anygm_content_source_prepare(set,"abc",3,NULL,NULL,&output,&size,error,sizeof error));
  assert(!output && !size && strstr(error,"result SHA-256"));
  anygm_content_transforms_destroy(set); return 1;
}

static int binding_isolation(void){
  AnygmContentTransforms *set=make_chain("first | second",0);
  AnygmContentTransforms *unbound=anygm_content_transforms_create(),*bound=anygm_content_transforms_create();
  assert(unbound && bound && anygm_content_transforms_copy(unbound,set));
  uint8_t before[32],after[32];
  anygm_content_transforms_hash(set,before);
  bind_one(set,"first",first_patch,sizeof first_patch);
  bind_one(set,"second",second_patch,sizeof second_patch);
  anygm_content_transforms_hash(set,after); assert(!memcmp(before,after,32));
  assert(anygm_content_transforms_copy(bound,set));
  anygm_content_transforms_destroy(set);
  expect_chain(bound);
  uint8_t *output=NULL; size_t size=0; char error[256];
  assert(!anygm_content_source_prepare(unbound,"abc",3,NULL,NULL,&output,&size,error,sizeof error));
  assert(!output && !size && strstr(error,"missing bound resource"));
  bind_one(unbound,"first",first_patch,sizeof first_patch);
  assert(!anygm_content_source_prepare(unbound,"abc",3,NULL,NULL,&output,&size,error,sizeof error));
  assert(!output && !size); /* The first successful intermediate is not published. */
  anygm_content_transforms_destroy(unbound); anygm_content_transforms_destroy(bound); return 1;
}

static int layers(void){
  AnygmContentTransforms *set=make_chain("first | second",1);
  uint8_t before[32],after[32]; char error[256],text[2048];
  anygm_content_transforms_hash(set,before);
  const char broken[]="[pipelines]\ninput.final=second|first\n[patches]\ninvalid=no\n";
  assert(!anygm_content_transforms_parse_layer(set,broken,sizeof broken-1,9,error,sizeof error));
  anygm_content_transforms_hash(set,after); assert(!memcmp(before,after,32)); expect_chain(set);
  const char reverse[]="[pipelines]\ninput.final=second|first\n";
  assert(anygm_content_transforms_parse_layer(set,reverse,sizeof reverse-1,5,error,sizeof error));
  chain_text(text,sizeof text,"","first|second");
  assert(anygm_content_transforms_parse_layer(set,text,strlen(text),1,error,sizeof error));
  /* Lower priority cannot replace the higher-priority chain. The lower layer's
   * patch declarations do replace lower-priority entries and require rebinding. */
  bind_one(set,"first",first_patch,sizeof first_patch); bind_one(set,"second",second_patch,sizeof second_patch);
  uint8_t *output=NULL; size_t size=0;
  assert(!anygm_content_source_prepare(set,"abc",3,NULL,NULL,&output,&size,error,sizeof error));
  assert(!output && !size && strstr(error,"base or patch order"));
  anygm_content_transforms_destroy(set); return 1;
}

static int paths_and_declarations(void){
  char base[65],delta[65],result[65],text[2048],error[256];
  hex("abc",3,base); hex(first_patch,sizeof first_patch,delta); hex("abcdef",6,result);
  const char *invalid[]={"../first.xdelta","a/../first.xdelta","a\\..\\first.xdelta",
    "/first.xdelta","C:\\first.xdelta","a//first.xdelta","./first.xdelta","folder/","a\001b"};
  for(size_t i=0;i<sizeof invalid/sizeof invalid[0];i++){
    snprintf(text,sizeof text,"xdelta|%s|%s|%s|%s|6",invalid[i],base,delta,result);
    assert(!anygm_content_patch_parse(text,strlen(text),error,sizeof error));
  }
  snprintf(text,sizeof text,"xdelta|patch folder\\first.xdelta|%s|%s|%s|6",base,delta,result);
  AnygmContentPatch *patch=anygm_content_patch_parse(text,strlen(text),error,sizeof error);
  AnygmContentPatchSpec spec; assert(patch); anygm_content_patch_spec(patch,&spec);
  assert(!strcmp(spec.path,"patch folder/first.xdelta")); anygm_content_patch_release(patch);
  snprintf(text,sizeof text,"xdelta|first.xdelta|%s|%s|%s|1073741825",base,delta,result);
  assert(!anygm_content_patch_parse(text,strlen(text),error,sizeof error));
  chain_text(text,sizeof text,"","first|second");
  strcat(text,"[transforms]\nfirst=buffer same(){return slice(0,input_size);}\n");
  assert(!anygm_content_config_parse(text,strlen(text),error,sizeof error));
  return 1;
}

static int never_validate(void *context,const void *data,size_t size,char *error,size_t capacity){
  (void)data; (void)size; (void)error; (void)capacity;
  ++*(int*)context; return -1;
}

static int final_after_decline(void){
  const char *probes[]={"write64(scratch,8,input_size);return scratch_slice(0,80);",
                        "return scratch_slice(0,0);"};
  for(size_t i=0;i<2;i++){
    AnygmContentTransforms *set=make_chain("first|second",1);
    char config[512],error[256];
    snprintf(config,sizeof config,"[transforms]\ninput=buffer same(){return slice(0,input_size);}\n"
      "input.probe=buffer probe(){%s}\n",probes[i]);
    assert(anygm_content_transforms_parse(set,config,strlen(config),error,sizeof error));
    int calls=0; uint8_t *output=NULL; size_t size=0;
    assert(anygm_content_source_prepare(set,"abc",3,never_validate,&calls,&output,&size,error,sizeof error));
    assert(!calls && size==7 && !memcmp(output,"abcdef!",7));
    free(output); anygm_content_transforms_destroy(set);
  }
  return 1;
}

static int selectors(void){
  char text[2048],scope[80],digest_hex[65],overrides[256],error[256];
  uint8_t digest[32]; hex("abc",3,digest_hex); gml_sha256("abc",3,digest);
  snprintf(scope,sizeof scope,"sha256:%s.",digest_hex);
  chain_text(text,sizeof text,scope,"first|second");
  AnygmContentConfig *config=anygm_content_config_parse(text,strlen(text),error,sizeof error);
  assert(config && anygm_content_config_has_input(config));
  AnygmContentTransforms *set=anygm_content_transforms_create(); assert(set);
  assert(anygm_content_config_apply(config,NULL,set,0,overrides,sizeof overrides,error,sizeof error));
  assert(!anygm_content_transforms_patch_count(set));
  assert(anygm_content_config_apply(config,digest,set,10,overrides,sizeof overrides,error,sizeof error));
  assert(anygm_content_transforms_patch_count(set)==2 && anygm_content_source_configured(set));
  bind_one(set,"first",first_patch,sizeof first_patch); bind_one(set,"second",second_patch,sizeof second_patch);
  expect_chain(set);
  anygm_content_transforms_destroy(set); anygm_content_config_destroy(config); return 1;
}

int main(int argc,char **argv){
  static const AnygmTestCase cases[]={
    {"ordered",ordered},{"reversed",reversed},{"identities",identities},
    {"binding_isolation",binding_isolation},{"layers",layers},
    {"paths_and_declarations",paths_and_declarations},
    {"final_after_decline",final_after_decline},{"selectors",selectors}
  };
  AnygmTestGroup group={"content.patch",cases,sizeof cases/sizeof cases[0]};
  AnygmTestResult result;
  int ok=anygm_test_run_groups(&group,1,argc>1?argv[1]:NULL,&result);
  printf("content patches: %d passed, %d failed\n",result.passed,result.failed);
  return ok?0:1;
}
