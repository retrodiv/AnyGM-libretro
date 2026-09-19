/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* The advertised `win`, `droid`, and `unx` extensions are the exporters' three names for one
 * Studio data container. Advertising a name is a promise that a frontend may hand that payload
 * over directly, so each name is loaded from a clean fixture under its own file name: the
 * resolution, the container validation, and the first frame all have to work for every one of
 * them, and each load has to produce the same container the fixture was created from. */
#include "engine_internal.h"
#include "stdio_vfs.h"
#include "synthetic_content.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *PAYLOAD_NAMES[]={"data.win","game.droid","game.unx"};

static int load_named_payload(const AnygmHostServices *services,const char *directory,
                              const char *name,const uint8_t *expected,size_t expected_size){
  char path[256];
  int written=snprintf(path,sizeof path,"%s/%s",directory,name);
  if(written<=0 || (size_t)written>=sizeof path){
    printf("FAIL %s: the fixture path does not fit\n",name);
    return 0;
  }
  AnygmEngine *engine=NULL;
  if(anygm_create(services,&engine)!=ANYGM_OK){
    printf("FAIL %s: engine creation failed\n",name);
    return 0;
  }
  AnygmContentSource source={0};
  source.struct_size=sizeof source;
  source.kind=ANYGM_CONTENT_PATH;
  source.path=path;
  source.cache_directory=directory;
  source.save_directory=directory;
  AnygmResult result=anygm_load(engine,&source,NULL);
  if(result!=ANYGM_OK){
    char error[512]={0};
    anygm_get_last_error(engine,error,sizeof error);
    printf("FAIL %s: load returned %d (%s)\n",name,(int)result,error);
    anygm_destroy(engine);
    return 0;
  }
  if(engine->win.size!=expected_size || memcmp(engine->win.data,expected,expected_size)!=0){
    printf("FAIL %s: the loaded container is not the fixture's own bytes\n",name);
    anygm_destroy(engine);
    return 0;
  }
  AnygmInputFrame input={0};
  AnygmFrameOutput output={0};
  input.struct_size=sizeof input;
  input.pointer_x=input.pointer_y=-1;
  output.struct_size=sizeof output;
  if(anygm_run_frame(engine,&input,&output)!=ANYGM_OK){
    printf("FAIL %s: the loaded session did not run a frame\n",name);
    anygm_destroy(engine);
    return 0;
  }
  anygm_destroy(engine);
  return 1;
}

int main(void){
  AnygmSyntheticContent fixture;
  if(!anygm_synthetic_content_create(&fixture)){
    fputs("fixture creation failed\n",stderr);
    return 1;
  }
  uint8_t *bytes=NULL;
  size_t size=0;
  if(!anygm_synthetic_content_read(&fixture,&bytes,&size)){
    fputs("fixture read failed\n",stderr);
    anygm_synthetic_content_destroy(&fixture);
    return 1;
  }
  AnygmHostServices services={0};
  services.struct_size=sizeof services;
  services.abi_version=ANYGM_HOST_SERVICES_VERSION;
  anygm_stdio_vfs_services_init(&services);

  int failures=0;
  char current[256];
  int written=snprintf(current,sizeof current,"%s",fixture.path);
  if(written<=0 || (size_t)written>=sizeof current){
    fputs("fixture path does not fit\n",stderr);
    failures++;
  }
  for(size_t i=0;!failures && i<sizeof PAYLOAD_NAMES/sizeof PAYLOAD_NAMES[0];i++){
    char named[256];
    written=snprintf(named,sizeof named,"%s/%s",fixture.directory,PAYLOAD_NAMES[i]);
    if(written<=0 || (size_t)written>=sizeof named || strcmp(current,named)!=0){
      if(written<=0 || (size_t)written>=sizeof named || rename(current,named)!=0){
        printf("FAIL %s: the fixture could not be renamed\n",PAYLOAD_NAMES[i]);
        failures++;
        break;
      }
      snprintf(current,sizeof current,"%s",named);
    }
    if(!load_named_payload(&services,fixture.directory,PAYLOAD_NAMES[i],bytes,size)) failures++;
  }
  free(bytes);
  anygm_synthetic_content_destroy(&fixture);
  return failures?1:0;
}
