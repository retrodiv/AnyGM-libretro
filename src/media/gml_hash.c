/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "gml_hash.h"

#include <string.h>

static uint32_t rotate_right(uint32_t value,unsigned bits){
  return (value>>bits)|(value<<(32u-bits));
}

static uint32_t load_be32(const uint8_t *data){
  return (uint32_t)data[0]<<24|(uint32_t)data[1]<<16|(uint32_t)data[2]<<8|data[3];
}

static void store_be32(uint8_t *data,uint32_t value){
  data[0]=(uint8_t)(value>>24); data[1]=(uint8_t)(value>>16);
  data[2]=(uint8_t)(value>>8); data[3]=(uint8_t)value;
}

static void sha256_transform(GmlSha256 *hash,const uint8_t block[64]){
  static const uint32_t constants[64]={
    0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,
    0x923f82a4u,0xab1c5ed5u,0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,
    0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,0xe49b69c1u,0xefbe4786u,
    0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
    0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,
    0x06ca6351u,0x14292967u,0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,
    0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,0xa2bfe8a1u,0xa81a664bu,
    0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
    0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,
    0x5b9cca4fu,0x682e6ff3u,0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,
    0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
  };
  uint32_t schedule[64];
  for(unsigned i=0;i<16;i++) schedule[i]=load_be32(block+i*4u);
  for(unsigned i=16;i<64;i++){
    uint32_t s0=rotate_right(schedule[i-15],7)^rotate_right(schedule[i-15],18)^
      (schedule[i-15]>>3);
    uint32_t s1=rotate_right(schedule[i-2],17)^rotate_right(schedule[i-2],19)^
      (schedule[i-2]>>10);
    schedule[i]=schedule[i-16]+s0+schedule[i-7]+s1;
  }
  uint32_t a=hash->state[0],b=hash->state[1],c=hash->state[2],d=hash->state[3];
  uint32_t e=hash->state[4],f=hash->state[5],g=hash->state[6],h=hash->state[7];
  for(unsigned i=0;i<64;i++){
    uint32_t upper=rotate_right(e,6)^rotate_right(e,11)^rotate_right(e,25);
    uint32_t choose=(e&f)^(~e&g);
    uint32_t first=h+upper+choose+constants[i]+schedule[i];
    uint32_t lower=rotate_right(a,2)^rotate_right(a,13)^rotate_right(a,22);
    uint32_t majority=(a&b)^(a&c)^(b&c);
    uint32_t second=lower+majority;
    h=g; g=f; f=e; e=d+first; d=c; c=b; b=a; a=first+second;
  }
  hash->state[0]+=a; hash->state[1]+=b; hash->state[2]+=c; hash->state[3]+=d;
  hash->state[4]+=e; hash->state[5]+=f; hash->state[6]+=g; hash->state[7]+=h;
}

void gml_sha256_update(GmlSha256 *hash,const void *data,size_t size){
  const uint8_t *source=(const uint8_t*)data;
  hash->bytes+=(uint64_t)size;
  while(size){
    size_t available=sizeof(hash->block)-hash->used;
    size_t take=size<available?size:available;
    memcpy(hash->block+hash->used,source,take);
    hash->used+=take; source+=take; size-=take;
    if(hash->used==sizeof(hash->block)){
      sha256_transform(hash,hash->block); hash->used=0;
    }
  }
}

void gml_sha256_init(GmlSha256 *hash){
  const GmlSha256 initial={{
    0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,
    0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u
  },0,{0},0};
  *hash=initial;
}

void gml_sha256_final(GmlSha256 *hash,uint8_t digest[32]){
  uint64_t bits=hash->bytes*8u;
  const uint8_t marker=0x80;
  gml_sha256_update(hash,&marker,1);
  const uint8_t zero=0;
  while(hash->used!=56u) gml_sha256_update(hash,&zero,1);
  uint8_t length[8];
  for(unsigned i=0;i<8;i++) length[7u-i]=(uint8_t)(bits>>(i*8u));
  gml_sha256_update(hash,length,sizeof(length));
  for(unsigned i=0;i<8;i++) store_be32(digest+i*4u,hash->state[i]);
}

void gml_sha256(const void *data,size_t size,uint8_t digest[32]){
  GmlSha256 hash;
  gml_sha256_init(&hash);
  gml_sha256_update(&hash,data,size);
  gml_sha256_final(&hash,digest);
}
