/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "engine_internal.h"
#include "synthetic_content.h"
#include "zip_fixture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { STATE_HEADER_SIZE_OFFSET=8, STATE_CHECKSUM_OFFSET=56 };

static AnygmEngine *engine;
static uint8_t *content,*baseline,*candidate;
static size_t content_size,baseline_size,header_size;

static void destroy_fixture(void){
  anygm_destroy(engine);
  free(content);
  free(baseline);
  free(candidate);
}

static void write_seed(const char *path,const void *data,size_t size){
  FILE *file=fopen(path,"wb");
  if(!file) abort();
  int complete=fwrite(data,1,size,file)==size;
  if(fclose(file) || !complete) abort();
}

static void write_container_seed(const char *path){
  static const uint8_t name[]="data.win";
  const uint8_t zip_selector=0;
  if(content_size>UINT32_MAX) abort();
  ZipEntry entry={.name=name,.name_size=sizeof name-1,.data=content,
                  .compressed_size=(uint32_t)content_size,.uncompressed_size=(uint32_t)content_size};
  Buffer archive={0},seed={0};
  if(!build_zip(&entry,1,&archive) || !buffer_bytes(&seed,&zip_selector,1) ||
     !buffer_bytes(&seed,archive.data,archive.size)) abort();
  write_seed(path,seed.data,seed.size);
  free(seed.data);
  free(archive.data);
}

int LLVMFuzzerInitialize(int *argc,char ***argv){
  if(atexit(destroy_fixture)) abort();
  AnygmSyntheticContent fixture;
  if(!anygm_synthetic_list_override_content_create(&fixture)) abort();
  int read=anygm_synthetic_content_read(&fixture,&content,&content_size);
  anygm_synthetic_content_destroy(&fixture);
  if(!read) abort();

  AnygmHostServices services={.struct_size=sizeof services,
                             .abi_version=ANYGM_HOST_SERVICES_VERSION};
  AnygmContentSource source={.struct_size=sizeof source,.kind=ANYGM_CONTENT_MEMORY,
                             .data=content,.size=content_size};
  if(anygm_create(&services,&engine)!=ANYGM_OK || anygm_load(engine,&source,NULL)!=ANYGM_OK)
    abort();
  AnygmInputFrame input={.struct_size=sizeof input,.pointer_x=-1,.pointer_y=-1};
  AnygmFrameOutput output={.struct_size=sizeof output};
  if(anygm_run_frame(engine,&input,&output)!=ANYGM_OK) abort();

  baseline_size=anygm_state_size(engine);
  baseline=malloc(baseline_size);
  candidate=malloc(baseline_size);
  size_t written=0;
  if(!baseline || !candidate ||
     anygm_state_save(engine,baseline,baseline_size,&written)!=ANYGM_OK || written!=baseline_size ||
     baseline_size<STATE_CHECKSUM_OFFSET+8u) abort();
  for(unsigned i=0;i<4;i++) header_size|=(size_t)baseline[STATE_HEADER_SIZE_OFFSET+i]<<(8u*i);
  if(header_size<STATE_CHECKSUM_OFFSET+8u || header_size>=baseline_size) abort();

  if(*argc==5 && !strcmp((*argv)[1],"--export-seeds")){
    write_seed((*argv)[2],content,content_size);
    write_seed((*argv)[3],baseline,baseline_size);
    write_container_seed((*argv)[4]);
    exit(EXIT_SUCCESS);
  }
  return 0;
}

static void try_state(const uint8_t *data,size_t size){
  (void)anygm_state_load(engine,data,size);
  if(anygm_state_load(engine,baseline,baseline_size)!=ANYGM_OK) abort();
}

int LLVMFuzzerTestOneInput(const uint8_t *data,size_t size){
  try_state(data,size);
  if(size==baseline_size){
    /* Keep the transport test above and also exercise the checksummed sections. */
    memcpy(candidate,baseline,header_size);
    memcpy(candidate+header_size,data+header_size,size-header_size);
    uint64_t checksum=state_hash_bytes(candidate+header_size,size-header_size);
    for(unsigned i=0;i<8;i++) candidate[STATE_CHECKSUM_OFFSET+i]=(uint8_t)(checksum>>(8u*i));
    try_state(candidate,size);
  }
  return 0;
}
