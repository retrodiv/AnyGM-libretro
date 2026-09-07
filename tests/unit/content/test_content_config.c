/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "content_config.h"
#include "gml_hash.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DIGEST "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"

static void layers(void){
  const char text[]="\xef\xbb\xbf[overrides]\r\nostype|0\r\n"
    "[transforms]\ncopy=buffer copy() { return slice(0,input_size); }\n"
    "keep=buffer keep() { return slice(0,input_size); }\n"
    "[sha256:" DIGEST ".transforms]\ncopy=buffer edit() {\n"
    "/*\n[overrides]\n$injected=1\n*/\nwrite8(work,0,'z'); return slice(0,input_size);\n}\n"
    "[sha256:" DIGEST ".overrides]\n; ignored\nostype|6\n"
    "[unrelated]\nunchanged=value\n";
  char error[256],overrides[4096];
  AnygmContentConfig *config=anygm_content_config_parse(text,sizeof text-1,error,sizeof error);
  assert(config);
  AnygmContentTransforms *set=anygm_content_transforms_create(),*copy=anygm_content_transforms_create();
  assert(set && copy);
  assert(anygm_content_config_apply(config,NULL,set,0,overrides,sizeof overrides,error,sizeof error));
  assert(!strcmp(overrides,"ostype|0\n"));
  assert(anygm_content_transforms_copy(copy,set));
  uint8_t digest[32]; gml_sha256("abc",3,digest);
  uint8_t before[32],after[32];
  anygm_content_transforms_hash(set,before);
  assert(!anygm_content_config_apply(config,digest,set,100,overrides,2,error,sizeof error));
  anygm_content_transforms_hash(set,after);
  assert(!memcmp(before,after,32));
  assert(anygm_content_config_apply(config,digest,set,100,overrides,sizeof overrides,error,sizeof error));
  assert(!strcmp(overrides,"ostype|6\n"));
  uint8_t *output=NULL; size_t size=0;
  assert(anygm_content_transform_run(set,"copy","abc",3,&output,&size,error,sizeof error));
  assert(size==3 && !memcmp(output,"zbc",3)); free(output);
  assert(anygm_content_transforms_has(set,"keep"));
  assert(anygm_content_transform_run(copy,"copy","abc",3,&output,&size,error,sizeof error));
  assert(size==3 && !memcmp(output,"abc",3)); free(output);
  digest[0]^=1;
  assert(anygm_content_config_apply(config,digest,copy,100,overrides,sizeof overrides,error,sizeof error));
  assert(!overrides[0]);
  anygm_content_transforms_destroy(copy); anygm_content_transforms_destroy(set);
  anygm_content_config_destroy(config);
}

static void rejection(void){
  const char *invalid[]={
    "[sha256:abc.overrides]\n$x=1\n",
    "[sha256:" DIGEST ".unknown]\n",
    "[sha256:" DIGEST ".overrides]\n[sha256:" DIGEST ".overrides]\n",
    "[overrides]\n$x=1\n[overrides]\n$x=2\n",
    "[sha256:" DIGEST ".transforms]\nx=buffer broken() { unknown(); }\n",
    "[sha256:" DIGEST ".transforms]\nx=buffer broken() { /*\n[overrides]\n$x=1\n",
    "[transforms]\nx=buffer copy() {return slice(0,input_size);} trailing\n",
    "[transforms]\nx=buffer copy() {return slice(0,input_size);}\nx=buffer copy() {}\n"
  };
  for(size_t i=0;i<sizeof invalid/sizeof invalid[0];i++){
    char error[256];
    AnygmContentConfig *config=anygm_content_config_parse(invalid[i],strlen(invalid[i]),error,sizeof error);
    assert(!config && error[0]);
  }
  char error[256],oversized[4200];
  memset(oversized,'x',sizeof oversized); memcpy(oversized,"[overrides]\n",12);
  assert(!anygm_content_config_parse(oversized,sizeof oversized,error,sizeof error));
  const char nul[]="[overrides]\n$x=1\0\n";
  assert(!anygm_content_config_parse(nul,sizeof nul-1,error,sizeof error));
}

int main(void){
  layers(); rejection();
  puts("Content configuration selection, isolation and rejection: ok");
  return 0;
}
