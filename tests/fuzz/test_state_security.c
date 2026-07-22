/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "anygm.h"
#include "stdio_vfs.h"
#include "synthetic_content.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { STATE_HEADER_SIZE=112 };

static int fail(const char *message){
  fprintf(stderr,"state security: %s\n",message);
  return 0;
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
    hash^=read_u64(data);
    hash*=UINT64_C(1099511628211);
    data+=8;
    size-=8;
  }
  while(size--){
    hash^=*data++;
    hash*=UINT64_C(1099511628211);
  }
  return hash;
}

static int save_state(AnygmEngine *engine,uint8_t **bytes,size_t *size){
  size_t capacity=anygm_state_size(engine),written=0;
  uint8_t *data=malloc(capacity?capacity:1);
  if(!data || anygm_state_save(engine,data,capacity,&written)!=ANYGM_OK ||
     written!=capacity){
    free(data);
    return 0;
  }
  *bytes=data;
  *size=written;
  return 1;
}

static int engine_matches(AnygmEngine *engine,const uint8_t *baseline,size_t baseline_size){
  uint8_t *current=NULL;
  size_t current_size=0;
  int same=save_state(engine,&current,&current_size)&&current_size==baseline_size&&
           !memcmp(current,baseline,baseline_size);
  free(current);
  return same;
}

static int reject_unchanged(AnygmEngine *engine,const uint8_t *candidate,size_t size,
                            const uint8_t *baseline,size_t baseline_size,const char *label){
  if(anygm_state_load(engine,candidate,size)!=ANYGM_ERROR_STATE_MISMATCH){
    fprintf(stderr,"state security: accepted %s\n",label);
    return 0;
  }
  if(!engine_matches(engine,baseline,baseline_size)){
    fprintf(stderr,"state security: %s changed the engine\n",label);
    return 0;
  }
  return 1;
}

static void refresh_checksum(uint8_t *state){
  size_t payload_size=(size_t)read_u64(state+96);
  write_u64(state+56,state_checksum(state+STATE_HEADER_SIZE,payload_size));
}

static int reject_payload_u32(AnygmEngine *engine,uint8_t *candidate,size_t size,
                              const uint8_t *baseline,size_t baseline_size,size_t offset,
                              uint32_t value,const char *label){
  memcpy(candidate,baseline,baseline_size);
  if(offset>baseline_size-4) return fail("mutation offset is outside the state");
  write_u32(candidate+offset,value);
  refresh_checksum(candidate);
  return reject_unchanged(engine,candidate,size,baseline,baseline_size,label);
}

int main(void){
  AnygmSyntheticContent fixture;
  if(!anygm_synthetic_content_create(&fixture)) return fail("fixture creation failed")?0:1;
  uint8_t *content=NULL;
  size_t content_size=0;
  if(!anygm_synthetic_content_read(&fixture,&content,&content_size))
    return fail("fixture read failed")?0:1;

  AnygmHostServices services={0};
  services.struct_size=sizeof services;
  services.abi_version=ANYGM_HOST_SERVICES_VERSION;
  anygm_stdio_vfs_services_init(&services);
  AnygmEngine *engine=NULL;
  if(anygm_create(&services,&engine)!=ANYGM_OK) return fail("engine creation failed")?0:1;
  AnygmContentSource source={0};
  source.struct_size=sizeof source;
  source.kind=ANYGM_CONTENT_MEMORY;
  source.path="synthetic-state.win";
  source.data=content;
  source.size=content_size;
  if(anygm_load(engine,&source,NULL)!=ANYGM_OK) return fail("content load failed")?0:1;

  AnygmInputFrame input={0};
  AnygmFrameOutput output={0};
  input.struct_size=sizeof input;
  input.pointer_x=input.pointer_y=-1;
  output.struct_size=sizeof output;
  if(anygm_run_frame(engine,&input,&output)!=ANYGM_OK) return fail("initial frame failed")?0:1;

  uint8_t *baseline=NULL;
  size_t state_size=0;
  if(!save_state(engine,&baseline,&state_size)||state_size<=STATE_HEADER_SIZE)
    return fail("baseline state failed")?0:1;
  uint8_t *candidate=malloc(state_size+64);
  if(!candidate) return fail("mutation allocation failed")?0:1;
  int ok=1;

  const size_t header_offsets[]={0,4,8,12,16,24,32,36,40,48,56,64,72,80,88,96,104};
  for(size_t i=0;ok&&i<sizeof header_offsets/sizeof header_offsets[0];i++){
    memcpy(candidate,baseline,state_size);
    candidate[header_offsets[i]]^=UINT8_C(0x5a);
    ok=reject_unchanged(engine,candidate,state_size,baseline,state_size,"mutated header field");
  }

  uint64_t core_size=read_u64(baseline+64);
  uint64_t render_size=read_u64(baseline+72);
  uint64_t vm_size=read_u64(baseline+80);
  uint64_t audio_size=read_u64(baseline+88);
  size_t render_start=STATE_HEADER_SIZE+(size_t)core_size;
  size_t vm_start=render_start+(size_t)render_size;
  size_t audio_start=vm_start+(size_t)vm_size;
  if(audio_start+(size_t)audio_size!=state_size) ok=fail("section arithmetic is inconsistent");

  size_t truncations[]={0,1,3,4,7,8,15,16,STATE_HEADER_SIZE-1,STATE_HEADER_SIZE,
                        render_start-1,render_start,vm_start-1,vm_start,
                        audio_start-1,audio_start,state_size-1};
  for(size_t i=0;ok&&i<sizeof truncations/sizeof truncations[0];i++)
    ok=reject_unchanged(engine,baseline,truncations[i],baseline,state_size,
                        "truncated section boundary");

  const size_t payload_mutations[]={STATE_HEADER_SIZE,STATE_HEADER_SIZE+core_size/2,
                                    render_start,render_start+render_size/2,
                                    vm_start,vm_start+vm_size/2,audio_start,
                                    audio_start+audio_size/2,state_size-1};
  for(size_t i=0;ok&&i<sizeof payload_mutations/sizeof payload_mutations[0];i++){
    memcpy(candidate,baseline,state_size);
    candidate[payload_mutations[i]]^=UINT8_C(0xa5);
    ok=reject_unchanged(engine,candidate,state_size,baseline,state_size,
                        "mutated checksummed payload");
  }

  if(ok){
    memcpy(candidate,baseline,state_size);
    memset(candidate+state_size,0,64);
    ok=anygm_state_load(engine,candidate,state_size+64)==ANYGM_OK&&
       engine_matches(engine,baseline,state_size);
    if(!ok) fail("zero-filled transport capacity was rejected");
  }
  if(ok){
    candidate[state_size+31]=1;
    ok=reject_unchanged(engine,candidate,state_size+64,baseline,state_size,
                        "nonzero transport tail");
  }

  if(ok) ok=reject_payload_u32(engine,candidate,state_size,baseline,state_size,
                               STATE_HEADER_SIZE,0,"zero frame width");
  if(ok){
    memcpy(candidate,baseline,state_size);
    write_u64(candidate+STATE_HEADER_SIZE+12,UINT64_C(0x7ff8000000000000));
    refresh_checksum(candidate);
    ok=reject_unchanged(engine,candidate,state_size,baseline,state_size,"non-finite frame rate");
  }
  if(ok) ok=reject_payload_u32(engine,candidate,state_size,baseline,state_size,
                               render_start,UINT32_MAX,"extreme font count");
  if(ok) ok=reject_payload_u32(engine,candidate,state_size,baseline,state_size,
                               render_start+20,UINT32_MAX,"extreme font map count");
  if(ok) ok=reject_payload_u32(engine,candidate,state_size,baseline,state_size,
                               vm_start,0,"invalid VM magic");
  if(ok) ok=reject_payload_u32(engine,candidate,state_size,baseline,state_size,
                               vm_start+4,UINT32_MAX,"invalid VM schema");
  if(ok) ok=reject_payload_u32(engine,candidate,state_size,baseline,state_size,
                               vm_start+8,UINT32_MAX,"extreme VM instance count");
  if(ok) ok=reject_payload_u32(engine,candidate,state_size,baseline,state_size,
                               audio_start,0,"invalid audio magic");

  if(ok&&render_size>=4){
    memcpy(candidate,baseline,state_size);
    write_u64(candidate+64,core_size+4);
    write_u64(candidate+72,render_size-4);
    ok=reject_unchanged(engine,candidate,state_size,baseline,state_size,"shifted core boundary");
  }
  if(ok&&vm_size>=4){
    memcpy(candidate,baseline,state_size);
    write_u64(candidate+72,render_size+4);
    write_u64(candidate+80,vm_size-4);
    ok=reject_unchanged(engine,candidate,state_size,baseline,state_size,"shifted render boundary");
  }
  if(ok&&audio_size>=4){
    memcpy(candidate,baseline,state_size);
    write_u64(candidate+80,vm_size+4);
    write_u64(candidate+88,audio_size-4);
    ok=reject_unchanged(engine,candidate,state_size,baseline,state_size,"shifted VM boundary");
  }
  if(ok){
    memcpy(candidate,baseline,state_size);
    write_u64(candidate+64,UINT64_MAX);
    ok=reject_unchanged(engine,candidate,state_size,baseline,state_size,"overflowing section size");
  }

  if(ok){
    char long_expression[ANYGM_MAX_RUNTIME_OVERRIDE_EXPRESSION+2];
    memset(long_expression,'x',sizeof long_expression-1);
    long_expression[sizeof long_expression-1]=0;
    ok=anygm_set_runtime_override(engine,0,1,"")==ANYGM_ERROR_INVALID_ARGUMENT&&
       anygm_set_runtime_override(engine,ANYGM_MAX_RUNTIME_OVERRIDES,1,"x")==
         ANYGM_ERROR_INVALID_ARGUMENT&&
       anygm_set_runtime_override(engine,0,1,long_expression)==ANYGM_ERROR_INVALID_ARGUMENT&&
       anygm_set_runtime_override(engine,0,0,NULL)==ANYGM_OK&&
       engine_matches(engine,baseline,state_size);
    if(!ok) fail("runtime override bounds were not transactional");
  }

  free(candidate);
  free(baseline);
  anygm_destroy(engine);
  free(content);
  anygm_synthetic_content_destroy(&fixture);
  if(!ok) return 1;
  puts("bounded state corpus: ok");
  return 0;
}
