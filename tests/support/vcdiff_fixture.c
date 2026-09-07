/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "vcdiff_fixture.h"
#include "gml_hash.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t integer(uint8_t *out,size_t value){
  uint8_t reversed[10]; size_t count=0;
  do { reversed[count++]=(uint8_t)(value&127u); value>>=7; } while(value);
  for(size_t i=0;i<count;i++) out[i]=reversed[count-i-1]|(i+1<count?128u:0u);
  return count;
}

uint8_t *anygm_test_vcdiff_literal(const void *target,size_t size,size_t *patch_size){
  assert(target && size && size<=1024u*1024u && patch_size);
  uint8_t header[64],instructions[16]; size_t h=0;
  instructions[0]=1; /* ADD, with an explicit variable-length size. */
  size_t instruction_size=1+integer(instructions+1,size);
  h+=integer(header+h,size); header[h++]=0;
  h+=integer(header+h,size); h+=integer(header+h,instruction_size); header[h++]=0;
  uint8_t *patch=malloc(size+128u); assert(patch);
  memcpy(patch,"\xd6\xc3\xc4\0\0\0",6); /* Standard table; no source window. */
  size_t n=6; n+=integer(patch+n,h+size+instruction_size);
  memcpy(patch+n,header,h); n+=h;
  memcpy(patch+n,target,size); n+=size;
  memcpy(patch+n,instructions,instruction_size); n+=instruction_size;
  *patch_size=n; return patch;
}

void anygm_test_sha256_hex(const void *data,size_t size,char output[65]){
  uint8_t digest[32]; gml_sha256(data,size,digest);
  for(size_t i=0;i<32;i++) snprintf(output+i*2,3,"%02x",digest[i]);
}
