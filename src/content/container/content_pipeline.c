/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "content_pipeline.h"
#include "content_transform.h"
#include "gml_image_codec.h"

#include <stdlib.h>
#include <string.h>

static uint32_t adler32(const uint8_t *bytes,size_t size){
  uint32_t first=1,second=0;
  while(size){
    size_t count=size<5552u?size:5552u;
    size-=count;
    while(count--){ first+=*bytes++; second+=first; }
    first%=65521u; second%=65521u;
  }
  return (second<<16)|first;
}

static int operation(const char *step,size_t *capacity){
  *capacity=0;
  if(!step) return 0;
  if(!strcmp(step,"builtin.byteswap16")) return 2;
  if(!strcmp(step,"builtin.byteswap32")) return 4;
  int kind=0;
  if(!strncmp(step,"builtin.zlib:",13)){ kind=8; step+=13; }
  else if(!strncmp(step,"builtin.deflate:",16)){ kind=9; step+=16; }
  else return 0;
  if(*step<'1' || *step>'9') return 0;
  uint64_t value=0;
  for(;*step;step++){
    if(*step<'0' || *step>'9' || value>ANYGM_TRANSFORM_MAX_INPUT_BYTES/10u) return 0;
    value=value*10u+(unsigned)(*step-'0');
    if(value>ANYGM_TRANSFORM_MAX_INPUT_BYTES) return 0;
  }
  *capacity=(size_t)value;
  return kind;
}

int anygm_content_pipeline_builtin_valid(const char *step){
  size_t capacity;
  return operation(step,&capacity)!=0;
}

int anygm_content_pipeline_builtin_run(const char *step,const void *input,size_t size,
                                        uint8_t **output,size_t *output_size){
  if(output) *output=NULL;
  if(output_size) *output_size=0;
  size_t capacity;
  int kind=operation(step,&capacity);
  if(!output || !output_size || !kind || (size && !input) ||
     size>ANYGM_TRANSFORM_MAX_INPUT_BYTES) return 0;
  const uint8_t *bytes=input;
  if(kind==8 && (size<6u || (bytes[0]&15u)!=8u || (bytes[0]>>4)>7u ||
      (((unsigned)bytes[0]<<8)|bytes[1])%31u || (bytes[1]&32u))) return 0;
  if(kind==2 || kind==4){
    if(size%(unsigned)kind) return 0;
    capacity=size;
  }
  uint8_t *result=malloc(capacity?capacity:1u);
  if(!result) return 0;
  size_t length=size;
  if(kind==2 || kind==4){
    for(size_t at=0;at<size;at+=(unsigned)kind)
      for(unsigned byte=0;byte<(unsigned)kind;byte++)
        result[at+byte]=bytes[at+(unsigned)kind-1u-byte];
  } else if(!gml_deflate_decode_to_buffer(kind==8?bytes+2u:bytes,
      kind==8?size-6u:size,GML_DEFLATE_RAW,result,capacity,&length)){
    free(result); return 0;
  }
  if(kind==8){
    const uint8_t *tail=bytes+size-4u;
    uint32_t expected=(uint32_t)tail[0]<<24|(uint32_t)tail[1]<<16|
      (uint32_t)tail[2]<<8|tail[3];
    if(adler32(result,length)!=expected){ free(result); return 0; }
  }
  *output=result; *output_size=length;
  return 1;
}
