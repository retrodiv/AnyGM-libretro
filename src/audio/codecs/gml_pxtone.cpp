/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Portable pxtone rendering adapter. The playback implementation is vendored from a
 * cross-platform fork of Studio Pixel's playback sources; see THIRD_PARTY_NOTICES.md
 * and src/third_party/pxtone/LICENSE.txt. */
#include "gml_pxtone.h"
#include "pxtnService.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The imported playback code uses only the core language allocation operators. Keeping these
 * local implementations lets the C-owned runtime archive remain linkable by freestanding C hosts
 * without imposing a platform-specific C++ standard-library ABI. */
void *operator new(size_t size){ return malloc(size?size:1u); }
void *operator new[](size_t size){ return malloc(size?size:1u); }
void operator delete(void *value) noexcept { free(value); }
void operator delete[](void *value) noexcept { free(value); }
void operator delete(void *value,size_t) noexcept { free(value); }
void operator delete[](void *value,size_t) noexcept { free(value); }

typedef struct PxtoneMemory {
  const uint8_t *data;
  size_t size;
  size_t position;
} PxtoneMemory;

static bool pxtone_read(void *opaque,void *destination,int32_t width,int32_t count){
  PxtoneMemory *source=(PxtoneMemory*)opaque;
  if(!source || !destination || width<=0 || count<0) return false;
  size_t item_size=(size_t)width,item_count=(size_t)count;
  if(item_count && item_size>SIZE_MAX/item_count) return false;
  size_t bytes=item_size*item_count;
  if(source->position>source->size || bytes>source->size-source->position) return false;
  memcpy(destination,source->data+source->position,bytes);
  source->position+=bytes;
  if(_is_big_endian())
    pxtnData::_correct_endian((unsigned char*)destination,width,count);
  return true;
}

static bool pxtone_write(void *,const void *,int32_t,int32_t){ return false; }

static bool pxtone_seek(void *opaque,int mode,int32_t distance){
  PxtoneMemory *source=(PxtoneMemory*)opaque;
  if(!source) return false;
  int64_t base=mode==SEEK_SET?0:mode==SEEK_CUR?(int64_t)source->position:
    mode==SEEK_END?(int64_t)source->size:-1;
  int64_t position=base+(int64_t)distance;
  if(base<0 || position<0 || (uint64_t)position>source->size) return false;
  source->position=(size_t)position;
  return true;
}

static bool pxtone_position(void *opaque,int32_t *position){
  PxtoneMemory *source=(PxtoneMemory*)opaque;
  if(!source || !position || source->position>INT32_MAX) return false;
  *position=(int32_t)source->position;
  return true;
}

extern "C" int gml_pxtone_render(const uint8_t *data,size_t size,int16_t **pcm,
                                  uint32_t *frames,uint32_t *loop_start_frame){
  enum { CHANNELS=2, SAMPLE_RATE=44100, MAX_PCM_BYTES=128*1024*1024 };
  if(pcm) *pcm=NULL;
  if(frames) *frames=0;
  if(loop_start_frame) *loop_start_frame=0;
  if(!data || !size || !pcm || !frames || !loop_start_frame) return 0;
  PxtoneMemory source={data,size,0};
  pxtnService service(pxtone_read,pxtone_write,pxtone_seek,pxtone_position);
  if(service.init()!=pxtnOK || !service.set_destination_quality(CHANNELS,SAMPLE_RATE) ||
     service.read(&source)!=pxtnOK || service.tones_ready()!=pxtnOK) return 0;
  pxtnVOMITPREPARATION preparation={};
  preparation.master_volume=1.0f;
  if(!service.moo_preparation(&preparation)) return 0;
  int32_t frame_count=service.moo_get_sampling_end();
  if(frame_count<=0 || (uint64_t)(uint32_t)frame_count*CHANNELS*sizeof(int16_t)>
     MAX_PCM_BYTES) return 0;
  size_t pcm_bytes=(size_t)(uint32_t)frame_count*CHANNELS*sizeof(int16_t);
  int16_t *rendered=(int16_t*)malloc(pcm_bytes);
  if(!rendered) return 0;
  size_t written=0;
  while(written<pcm_bytes){
    size_t remaining=pcm_bytes-written;
    int32_t request=remaining>(size_t)INT32_MAX?INT32_MAX:(int32_t)remaining;
    request-=request%(CHANNELS*(int)sizeof(int16_t));
    int32_t produced=0;
    if(request<=0 || !service.Moo((uint8_t*)rendered+written,request,&produced) ||
       produced<=0 || produced>request){ free(rendered); return 0; }
    written+=(size_t)produced;
  }
  int32_t repeat_measure=service.master->get_repeat_meas();
  int32_t repeat_frame=pxtnService_moo_CalcSampleNum(
    repeat_measure,service.master->get_beat_num(),SAMPLE_RATE,
    service.master->get_beat_tempo());
  if(repeat_frame<0 || repeat_frame>=frame_count) repeat_frame=0;
  *pcm=rendered;
  *frames=(uint32_t)frame_count;
  *loop_start_frame=(uint32_t)repeat_frame;
  return 1;
}
