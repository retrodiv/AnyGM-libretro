/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "anygm.h"
#include "stdio_vfs.h"
#include "synthetic_content.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int save_state(AnygmEngine *engine,uint8_t **data,size_t *written){
  size_t capacity=anygm_state_size(engine);
  *data=malloc(capacity?capacity:1);
  if(!*data) return 0;
  return anygm_state_save(engine,*data,capacity,written)==ANYGM_OK;
}

static uint64_t read_u64(const uint8_t *data){
  uint64_t value=0;
  for(unsigned i=0;i<8;i++) value|=(uint64_t)data[i]<<(i*8);
  return value;
}

static void write_u32(uint8_t *data,uint32_t value){
  for(unsigned i=0;i<4;i++) data[i]=(uint8_t)(value>>(i*8));
}

static void write_u64(uint8_t *data,uint64_t value){
  for(unsigned i=0;i<8;i++) data[i]=(uint8_t)(value>>(i*8));
}

static uint64_t state_checksum(const uint8_t *data,size_t size){
  uint64_t hash=UINT64_C(1469598103934665603);
  while(size>=8){
    uint64_t word=read_u64(data);
    hash^=word; hash*=UINT64_C(1099511628211);
    data+=8; size-=8;
  }
  while(size--){ hash^=*data++; hash*=UINT64_C(1099511628211); }
  return hash;
}

static int expect_rejected_unchanged(AnygmEngine *engine,const uint8_t *candidate,size_t size,
                                     const uint8_t *baseline,size_t baseline_size,
                                     const char *label){
  if(anygm_state_load(engine,candidate,size)!=ANYGM_ERROR_STATE_MISMATCH){
    fprintf(stderr,"%s was not rejected\n",label);
    return 0;
  }
  uint8_t *after=NULL;
  size_t after_size=0;
  int unchanged=save_state(engine,&after,&after_size) && after_size==baseline_size &&
                !memcmp(after,baseline,baseline_size);
  if(!unchanged && after && after_size==baseline_size){
    size_t first=0;
    while(first<baseline_size && after[first]==baseline[first]) first++;
    if(first<baseline_size)
      fprintf(stderr,"%s first state difference at %zu: %02x != %02x\n",
              label,first,after[first],baseline[first]);
    first=112;
    while(first<baseline_size && after[first]==baseline[first]) first++;
    if(first<baseline_size)
      fprintf(stderr,"%s first payload difference at %zu: %02x != %02x\n",
              label,first,after[first],baseline[first]);
    if(first<baseline_size){
      size_t from=first>16?first-16:0,to=first+32<baseline_size?first+32:baseline_size;
      fputs("after:   ",stderr); for(size_t i=from;i<to;i++) fprintf(stderr,"%02x",after[i]); fputc('\n',stderr);
      fputs("baseline:",stderr); for(size_t i=from;i<to;i++) fprintf(stderr,"%02x",baseline[i]); fputc('\n',stderr);
    }
  }
  free(after);
  if(!unchanged) fprintf(stderr,"%s changed the engine despite rejection\n",label);
  return unchanged;
}

int main(void){
  AnygmSyntheticContent fixture;
  if(!anygm_synthetic_content_create(&fixture)) return 1;

  AnygmHostServices services={0};
  services.struct_size=sizeof services;
  services.abi_version=ANYGM_HOST_SERVICES_VERSION;
  anygm_stdio_vfs_services_init(&services);
  AnygmEngine *first=NULL,*second=NULL;
  if(anygm_create(&services,&first)!=ANYGM_OK ||
     anygm_create(&services,&second)!=ANYGM_OK){
    fprintf(stderr,"engine creation failed\n");
    return 1;
  }

  AnygmContentSource source={0};
  source.struct_size=sizeof source;
  source.kind=ANYGM_CONTENT_PATH;
  source.path=fixture.path;
  source.cache_directory=fixture.directory;
  source.save_directory=fixture.directory;
  if(anygm_load(first,&source,NULL)!=ANYGM_OK || anygm_load(second,&source,NULL)!=ANYGM_OK){
    char error[512]={0};
    anygm_get_last_error(first,error,sizeof error);
    fprintf(stderr,"engine load failed: %s\n",error);
    return 1;
  }

  AnygmInputFrame input={0};
  input.struct_size=sizeof input;
  input.pointer_x=input.pointer_y=-1;
  AnygmFrameOutput first_output={0},second_output={0};
  first_output.struct_size=sizeof first_output;
  second_output.struct_size=sizeof second_output;
  if(anygm_run_frame(first,&input,&first_output)!=ANYGM_OK ||
     anygm_run_frame(second,&input,&second_output)!=ANYGM_OK ||
     !first_output.pixels || !second_output.pixels || first_output.pixels==second_output.pixels){
    fprintf(stderr,"interleaved frame ownership failed\n");
    return 1;
  }

  uint8_t *first_state=NULL,*second_state=NULL;
  size_t first_written=0,second_written=0;
  if(!save_state(first,&first_state,&first_written) ||
     !save_state(second,&second_state,&second_written) ||
     first_written!=second_written || memcmp(first_state,second_state,first_written)){
    fprintf(stderr,"equal interleaved engine states diverged\n");
    return 1;
  }
  free(first_state); free(second_state);

  first_output.struct_size=sizeof first_output;
  if(anygm_run_frame(first,&input,&first_output)!=ANYGM_OK ||
     !save_state(first,&first_state,&first_written) ||
     !save_state(second,&second_state,&second_written) ||
     (first_written==second_written && !memcmp(first_state,second_state,first_written))){
    fprintf(stderr,"independently advanced engine state was not isolated\n");
    return 1;
  }
  free(first_state); free(second_state);

  second_output.struct_size=sizeof second_output;
  if(anygm_run_frame(second,&input,&second_output)!=ANYGM_OK ||
     !save_state(first,&first_state,&first_written) ||
     !save_state(second,&second_state,&second_written) ||
     first_written!=second_written || memcmp(first_state,second_state,first_written)){
    fprintf(stderr,"resynchronized engine states diverged\n");
    return 1;
  }

  uint8_t *deterministic=NULL;
  size_t deterministic_size=0;
  if(!save_state(first,&deterministic,&deterministic_size) ||
     deterministic_size!=first_written || memcmp(deterministic,first_state,first_written)){
    fprintf(stderr,"repeated serialization was not deterministic\n");
    return 1;
  }
  free(deterministic);

  uint8_t *damaged=malloc(first_written);
  if(!damaged) return 1;
  memcpy(damaged,first_state,first_written);
  damaged[4]^=1;
  if(!expect_rejected_unchanged(first,damaged,first_written,first_state,first_written,
                                "unknown state schema")) return 1;
  if(!expect_rejected_unchanged(first,first_state,first_written-1,first_state,first_written,
                                "truncated state")) return 1;
  memcpy(damaged,first_state,first_written);
  damaged[112]^=1;
  if(!expect_rejected_unchanged(first,damaged,first_written,first_state,first_written,
                                "payload checksum mismatch")) return 1;

  memcpy(damaged,first_state,first_written);
  uint64_t core_size=read_u64(damaged+64);
  uint64_t render_size=read_u64(damaged+72);
  uint64_t payload_size=read_u64(damaged+96);
  uint64_t vm_offset=112+core_size+render_size;
  if(vm_offset+12>first_written || payload_size>first_written-112) return 1;
  write_u32(damaged+(size_t)vm_offset+8,UINT32_MAX);
  write_u64(damaged+56,state_checksum(damaged+112,(size_t)payload_size));
  if(!expect_rejected_unchanged(first,damaged,first_written,first_state,first_written,
                                "invalid VM section")) return 1;
  free(damaged);

  first_output.struct_size=sizeof first_output;
  if(anygm_run_frame(first,&input,&first_output)!=ANYGM_OK ||
     anygm_state_load(first,first_state,first_written)!=ANYGM_OK ||
     !save_state(first,&deterministic,&deterministic_size) ||
     deterministic_size!=first_written || memcmp(deterministic,first_state,first_written)){
    fprintf(stderr,"state roundtrip did not restore exact serialized state\n");
    return 1;
  }
  free(deterministic);

  free(first_state); free(second_state);
  anygm_destroy(first);
  anygm_destroy(second);
  anygm_synthetic_content_destroy(&fixture);
  puts("independent engine instances: ok");
  return 0;
}
