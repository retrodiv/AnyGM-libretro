/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef ANYGM_TEST_ZIP_FIXTURE_H
#define ANYGM_TEST_ZIP_FIXTURE_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct Buffer {
  uint8_t *data;
  size_t size;
  size_t capacity;
} Buffer;

typedef struct ZipEntry {
  const uint8_t *name;
  size_t name_size;
  const uint8_t *data;
  uint32_t compressed_size;
  uint32_t uncompressed_size;
  uint32_t method;
  uint32_t flags;
  uint32_t external_attributes;
  int corrupt_crc;
} ZipEntry;

static inline int buffer_reserve(Buffer *buffer,size_t add){
  if(add>SIZE_MAX-buffer->size) return 0;
  size_t needed=buffer->size+add;
  if(needed<=buffer->capacity) return 1;
  size_t capacity=buffer->capacity?buffer->capacity:256;
  while(capacity<needed){
    if(capacity>SIZE_MAX/2) return 0;
    capacity*=2;
  }
  uint8_t *next=realloc(buffer->data,capacity);
  if(!next) return 0;
  buffer->data=next;
  buffer->capacity=capacity;
  return 1;
}

static inline int buffer_bytes(Buffer *buffer,const void *data,size_t size){
  if(!buffer_reserve(buffer,size)) return 0;
  if(size) memcpy(buffer->data+buffer->size,data,size);
  buffer->size+=size;
  return 1;
}

static inline int buffer_u16(Buffer *buffer,uint16_t value){
  uint8_t encoded[2]={(uint8_t)value,(uint8_t)(value>>8)};
  return buffer_bytes(buffer,encoded,sizeof encoded);
}

static inline int buffer_u32(Buffer *buffer,uint32_t value){
  uint8_t encoded[4]={(uint8_t)value,(uint8_t)(value>>8),(uint8_t)(value>>16),
                      (uint8_t)(value>>24)};
  return buffer_bytes(buffer,encoded,sizeof encoded);
}

static inline uint32_t crc32_bytes(const uint8_t *data,size_t size){
  uint32_t crc=UINT32_MAX;
  for(size_t i=0;i<size;i++){
    crc^=data[i];
    for(unsigned bit=0;bit<8;bit++)
      crc=(crc>>1)^((crc&1u)?UINT32_C(0xedb88320):0u);
  }
  return ~crc;
}

static inline int build_zip(const ZipEntry *entries,size_t count,Buffer *output){
  Buffer local={0},central={0};
  uint32_t *offsets=calloc(count?count:1,sizeof *offsets);
  if(!offsets) return 0;
  int ok=1;
  for(size_t i=0;i<count&&ok;i++){
    const ZipEntry *entry=&entries[i];
    if(entry->name_size>UINT16_MAX || local.size>UINT32_MAX){ ok=0; break; }
    offsets[i]=(uint32_t)local.size;
    uint32_t crc=crc32_bytes(entry->data,entry->compressed_size);
    if(entry->corrupt_crc) crc^=UINT32_C(0x01010101);
    ok=buffer_u32(&local,UINT32_C(0x04034b50)) && buffer_u16(&local,20) &&
       buffer_u16(&local,(uint16_t)entry->flags) &&
       buffer_u16(&local,(uint16_t)entry->method) && buffer_u16(&local,0) &&
       buffer_u16(&local,0) && buffer_u32(&local,crc) &&
       buffer_u32(&local,entry->compressed_size) &&
       buffer_u32(&local,entry->uncompressed_size) &&
       buffer_u16(&local,(uint16_t)entry->name_size) && buffer_u16(&local,0) &&
       buffer_bytes(&local,entry->name,entry->name_size) &&
       buffer_bytes(&local,entry->data,entry->compressed_size);
    if(!ok) break;
    ok=buffer_u32(&central,UINT32_C(0x02014b50)) && buffer_u16(&central,0x0314) &&
       buffer_u16(&central,20) && buffer_u16(&central,(uint16_t)entry->flags) &&
       buffer_u16(&central,(uint16_t)entry->method) && buffer_u16(&central,0) &&
       buffer_u16(&central,0) && buffer_u32(&central,crc) &&
       buffer_u32(&central,entry->compressed_size) &&
       buffer_u32(&central,entry->uncompressed_size) &&
       buffer_u16(&central,(uint16_t)entry->name_size) && buffer_u16(&central,0) &&
       buffer_u16(&central,0) && buffer_u16(&central,0) && buffer_u16(&central,0) &&
       buffer_u32(&central,entry->external_attributes) &&
       buffer_u32(&central,offsets[i]) &&
       buffer_bytes(&central,entry->name,entry->name_size);
  }
  if(ok && (count>UINT16_MAX || local.size>UINT32_MAX || central.size>UINT32_MAX)) ok=0;
  if(ok){
    output->data=local.data;
    output->size=local.size;
    output->capacity=local.capacity;
    local.data=NULL;
    ok=buffer_bytes(output,central.data,central.size) &&
       buffer_u32(output,UINT32_C(0x06054b50)) && buffer_u16(output,0) &&
       buffer_u16(output,0) && buffer_u16(output,(uint16_t)count) &&
       buffer_u16(output,(uint16_t)count) && buffer_u32(output,(uint32_t)central.size) &&
       buffer_u32(output,(uint32_t)local.size) && buffer_u16(output,0);
  }
  free(offsets);
  free(local.data);
  free(central.data);
  if(!ok){ free(output->data); memset(output,0,sizeof *output); }
  return ok;
}

#endif
