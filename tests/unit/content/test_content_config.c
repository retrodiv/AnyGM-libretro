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
    "[pipelines]\ninput=copy|keep\n[unrelated]\nunchanged=value\n";
  const char higher[]="[transforms]\ncopy=buffer edit() {\n"
    "/*\n[overrides]\n$injected=1\n*/\nwrite8(work,0,'z'); return slice(0,input_size);\n}\n"
    "[overrides]\n; ignored\nostype|6\n"
    "[pipelines]\ninput=keep|copy\n";
  char error[256],overrides[4096];
  AnygmContentConfig *config=anygm_content_config_parse(text,sizeof text-1,error,sizeof error);
  AnygmContentConfig *overlay=anygm_content_config_parse(higher,sizeof higher-1,error,sizeof error);
  assert(config && overlay);
  AnygmContentTransforms *set=anygm_content_transforms_create(),*copy=anygm_content_transforms_create();
  assert(set && copy);
  assert(anygm_content_config_apply(config,set,0,overrides,sizeof overrides,error,sizeof error));
  assert(!strcmp(overrides,"ostype|0\n"));
  assert(anygm_content_transforms_copy(copy,set));
  uint8_t before[32],after[32];
  anygm_content_transforms_hash(set,before);
  assert(!anygm_content_config_apply(overlay,set,100,overrides,2,error,sizeof error));
  anygm_content_transforms_hash(set,after);
  assert(!memcmp(before,after,32));
  assert(anygm_content_config_apply(overlay,set,100,overrides,sizeof overrides,error,sizeof error));
  assert(!strcmp(overrides,"ostype|6\n"));
  uint8_t *output=NULL; size_t size=0;
  assert(anygm_content_transform_run(set,"copy","abc",3,NULL,0,&output,&size,error,sizeof error));
  assert(size==3 && !memcmp(output,"zbc",3)); free(output);
  assert(anygm_content_transforms_has(set,"keep"));
  assert(anygm_content_transform_run(set,"input","abc",3,NULL,0,&output,&size,error,sizeof error));
  assert(size==3 && !memcmp(output,"zbc",3)); free(output);
  assert(anygm_content_transform_run(copy,"copy","abc",3,NULL,0,&output,&size,error,sizeof error));
  assert(size==3 && !memcmp(output,"abc",3)); free(output);
  assert(anygm_content_transform_run(copy,"input","abc",3,NULL,0,&output,&size,error,sizeof error));
  assert(size==3 && !memcmp(output,"abc",3)); free(output);
  anygm_content_transforms_destroy(copy); anygm_content_transforms_destroy(set);
  anygm_content_config_destroy(config);
  anygm_content_config_destroy(overlay);
}

static void rejection(void){
  const char *invalid[]={
    "[sha256:" DIGEST ".overrides]\n$x=1\n",
    "[sha256:" DIGEST ".transforms]\nx=buffer same(){return slice(0,input_size);}\n",
    "[sha256:" DIGEST ".patches]\n",
    "[sha256:" DIGEST ".pipelines]\nx=builtin.byteswap16\n",
    "[sha256:abc.overrides]\n$x=1\n",
    "[sha256:" DIGEST ".unknown]\n",
    "[sha256:" DIGEST ".overrides]\n[sha256:" DIGEST ".overrides]\n",
    "[overrides]\n$x=1\n[overrides]\n$x=2\n",
    "[sha256:" DIGEST ".transforms]\nx=buffer broken() { unknown(); }\n",
    "[sha256:" DIGEST ".transforms]\nx=buffer broken() { /*\n[overrides]\n$x=1\n",
    "[transforms]\nx=buffer copy() {return slice(0,input_size);} trailing\n",
    "[transforms]\nx=buffer copy() {return slice(0,input_size);}\nx=buffer copy() {}\n",
    "[transforms]\nx=buffer copy() {return slice(0,input_size);}\n[pipelines]\nx=builtin.byteswap16\n",
    "[pipelines]\nx=builtin.byteswap16\n[transforms]\nx=buffer copy() {return slice(0,input_size);}\n",
    "[sha256:" DIGEST ".pipelines]\nx=builtin.zlib:0\n",
    "[pipelines]\nx=builtin.byteswap16\n[pipelines]\ny=builtin.byteswap16\n"
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

static void fast_selectors(void){
  const char text[]="[overrides]\n$x=1\n"
    "[zip:123:1234ABCD.overrides]\n$x=2\n"
    "[sample-v1:456:" DIGEST ".overrides]\n$x=3\n"
    "[zip:123:1234abcd.transforms]\ncopy=buffer f(){return slice(0,input_size);}\n"
    "[zip:124:1234abcd.transforms]\ncopy=buffer f(){return slice(0,input_size);}\n";
  char error[256],out[4096];
  AnygmContentConfig *config=anygm_content_config_parse(text,sizeof text-1,error,sizeof error);
  AnygmContentTransforms *set=anygm_content_transforms_create();
  assert(config && set);
  assert(anygm_content_config_candidate(config,"zip",123));
  assert(!anygm_content_config_candidate(config,"zip",125));
  assert(anygm_content_config_candidate(config,"sample-v1",456));
  assert(!anygm_content_config_candidate(config,"sample-v1",123));
  assert(anygm_content_config_apply(config,set,0,out,sizeof out,error,sizeof error));
  assert(!strcmp(out,"$x=1\n") && !anygm_content_transforms_has(set,"copy"));
  assert(anygm_content_config_select(config,"zip:123:1234abcd",set,100,out,sizeof out,error,sizeof error));
  assert(!strcmp(out,"$x=2\n") && anygm_content_transforms_has(set,"copy"));
  assert(anygm_content_config_select(config,"zip:123:1234abce",set,100,out,sizeof out,error,sizeof error));
  assert(!out[0]);
  assert(anygm_content_config_select(config,"sample-v1:456:" DIGEST,set,100,out,sizeof out,error,sizeof error));
  assert(!strcmp(out,"$x=3\n"));
  anygm_content_config_destroy(config);anygm_content_transforms_destroy(set);
  const char *invalid[]={
    "[zip:123:1234abcd.overrides]\n[zip:123:1234ABCD.overrides]\n",
    "[zip:0:1234abcd.overrides]\n", "[zip:01:1234abcd.overrides]\n",
    "[zip:18446744073709551616:1234abcd.overrides]\n",
    "[zip:123:1234abcg.overrides]\n", "[sample-v1:123:1234abcd.overrides]\n",
    "[zip:123:1234abcd.unknown]\n",
    "[zip:123:1234abcd.transforms]\ninput=buffer f(){return slice(0,input_size);}\n",
    "[zip:123:1234abcd.transforms]\nx=buffer bad(){unknown();}\n"};
  for(size_t i=0;i<sizeof invalid/sizeof invalid[0];i++)
    assert(!anygm_content_config_parse(invalid[i],strlen(invalid[i]),error,sizeof error));
}

int main(void){
  layers(); rejection(); fast_selectors();
  puts("Content configuration selection, isolation and rejection: ok");
  return 0;
}
