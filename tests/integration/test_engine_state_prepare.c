/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* A host announcing its next save lets the engine write the runtime section in the background. The
 * save that follows writes exactly the bytes of an unannounced save, and any call that can change
 * the runtime in between abandons the background work rather than saving a stale runtime. */
#include "anygm.h"
#include "engine_internal.h"
#include "stdio_vfs.h"
#include "synthetic_content.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int run_frames(AnygmEngine *engine,int count){
  AnygmInputFrame input={0};
  input.struct_size=sizeof input;
  input.pointer_x=input.pointer_y=-1;
  for(int frame=0;frame<count;frame++){
    AnygmFrameOutput output={0};
    output.struct_size=sizeof output;
    if(anygm_run_frame(engine,&input,&output)!=ANYGM_OK) return 0;
  }
  return 1;
}

typedef struct { uint8_t *data; size_t size; } Saved;

static int save(AnygmEngine *engine,Saved *saved){
  size_t capacity=anygm_state_size(engine);
  free(saved->data);
  saved->data=malloc(capacity?capacity:1);
  return saved->data && anygm_state_save(engine,saved->data,capacity,&saved->size)==ANYGM_OK;
}

static int same(const Saved *a,const Saved *b){
  return a->size==b->size && !memcmp(a->data,b->data,a->size);
}

/* The completed-frame section as the state format defines it, written the plain way: dimensions,
 * then raw pixels or a row table and runs over the literal rows, whichever is smaller. */
static size_t reference_frame(const uint32_t *pixels,uint32_t width,uint32_t height,
                              uint32_t *out,size_t capacity){
  size_t at=0,runs=0,literal=0;
  uint32_t *rows=malloc((size_t)height*sizeof *rows);
  if(!rows) return 0;
  for(uint32_t y=0;y<height;y++){
    rows[y]=y && !memcmp(pixels+(size_t)y*width,pixels+(size_t)(y-1)*width,width*4u)?rows[y-1]:y;
    if(rows[y]==y){
      literal++;
      for(uint32_t x=0;x<width;x++)
        if(!x || pixels[(size_t)y*width+x]!=pixels[(size_t)y*width+x-1]) runs++;
    }
  }
  size_t raw=(size_t)width*height,rle=(size_t)height+2u+runs*2u;
  if(capacity<3u+(raw<rle?raw:rle)){ free(rows); return 0; }
  out[at++]=width; out[at++]=height;
  if(raw<=rle){
    out[at++]=1u;
    memcpy(out+at,pixels,raw*4u); at+=raw;
  } else {
    out[at++]=2u;
    for(uint32_t y=0;y<height;y++) out[at++]=rows[y]==y?UINT32_C(0xFFFFFFFF):rows[y];
    out[at++]=(uint32_t)literal; out[at++]=(uint32_t)runs;
    for(uint32_t y=0;y<height;y++){
      if(rows[y]!=y) continue;
      const uint32_t *row=pixels+(size_t)y*width;
      for(uint32_t x=0;x<width;){
        uint32_t end=x+1;
        while(end<width && row[end]==row[x]) end++;
        out[at++]=end-x; out[at++]=row[x]; x=end;
      }
    }
  }
  free(rows);
  return at*4u;
}

static uint32_t read_u32(const uint8_t *bytes){
  return (uint32_t)bytes[0]|(uint32_t)bytes[1]<<8|(uint32_t)bytes[2]<<16|(uint32_t)bytes[3]<<24;
}

/* The completed frame closes the core section: header size at byte 8, core size at byte 64. */
static int frame_section_matches(const Saved *saved,const uint32_t *reference,size_t bytes){
  if(saved->size<72u) return 0;
  uint64_t header=read_u32(saved->data+8);
  uint64_t core=read_u32(saved->data+64)|(uint64_t)read_u32(saved->data+68)<<32;
  if(header+core>saved->size || core<bytes) return 0;
  return !memcmp(saved->data+header+core-bytes,reference,bytes);
}

#define REQUIRE(condition,message) do { if(!(condition)){ \
  fprintf(stderr,"state prepare: %s\n",message); ok=0; goto done; } } while(0)

int main(void){
  AnygmSyntheticContent fixture={0};
  AnygmEngine *engine=NULL;
  Saved plain={0},announced={0},advanced={0},after={0};
  uint32_t *reference=NULL;
  int ok=1;
  REQUIRE(anygm_synthetic_content_create(&fixture),"fixture creation");
  AnygmHostServices services={0};
  services.struct_size=sizeof services;
  services.abi_version=ANYGM_HOST_SERVICES_VERSION;
  anygm_stdio_vfs_services_init(&services);
  AnygmContentSource source={0};
  source.struct_size=sizeof source;
  source.kind=ANYGM_CONTENT_PATH;
  source.path=fixture.path;
  source.cache_directory=fixture.directory;
  source.save_directory=fixture.directory;
  REQUIRE(anygm_create(&services,&engine)==ANYGM_OK && anygm_load(engine,&source,NULL)==ANYGM_OK,
          "engine load");
  REQUIRE(run_frames(engine,4) && save(engine,&plain),"opening frames and first save");
  /* The background writer serves runtimes whose section is worth a thread; present this small
   * fixture as one, keeping a capacity above its real size. */
  engine->state_vm_size_hint=65536u;

  /* An announced save writes the unannounced bytes, from the background work. */
  REQUIRE(save(engine,&plain),"unannounced save");
  engine->state_vm_size_hint=65536u;
  anygm_state_prepare(engine);
  REQUIRE(engine_state_prepared_pending(engine),"the announcement starts background work");
  REQUIRE(save(engine,&announced),"announced save");
  REQUIRE(engine_state_prepared_consumed(engine)==2,
          "the save consumed the background work and the sections written before it");
  REQUIRE(same(&plain,&announced),"announced and unannounced saves are byte-identical");

  /* A frame between the announcement and the save abandons the work: the save describes the
   * runtime after that frame, exactly as an unannounced save does. */
  engine->state_vm_size_hint=65536u;
  anygm_state_prepare(engine);
  REQUIRE(run_frames(engine,1),"frame after an announcement");
  REQUIRE(!engine_state_prepared_pending(engine),"a frame abandons the background work");
  REQUIRE(save(engine,&advanced) && save(engine,&after),"saves after the frame");
  REQUIRE(same(&advanced,&after) && !same(&advanced,&plain),
          "the save after a frame describes the advanced runtime");

  /* Loading a state abandons it too, and destroying an engine with work pending is clean. */
  engine->state_vm_size_hint=65536u;
  anygm_state_prepare(engine);
  REQUIRE(anygm_state_load(engine,plain.data,plain.size)==ANYGM_OK,"load after an announcement");
  REQUIRE(!engine_state_prepared_pending(engine),"a load abandons the background work");
  REQUIRE(save(engine,&after) && same(&after,&plain),"the loaded runtime saves its own bytes");

  /* A monitor-sized completed frame is encoded by bands of rows, and an announced save encodes it
   * on its own worker; both write the bytes of the plain encoding: distinct rows of runs,
   * magnified rows, and detail that only raw pixels keep smaller. The fixture presents its
   * native picture, so the presented extent is set to a monitor's here, over the full-size
   * frame buffer. */
  {
    REQUIRE(run_frames(engine,1) && engine->have_presented_frame,"presented frame");
    engine->output_width=1920u;engine->output_height=1080u;
    uint32_t width=engine->output_width,height=engine->output_height;
    REQUIRE((size_t)width*height>=262144u && height>=4u,"the frame is large enough for bands");
    size_t capacity=(size_t)width*height+3u;
    reference=malloc(capacity*4u);
    REQUIRE(reference,"reference buffer");
    uint32_t seed=0x2545F491u;
    for(unsigned pattern=0;pattern<3u;pattern++){
      for(uint32_t y=0;y<height;y++) for(uint32_t x=0;x<width;x++){
        uint32_t value;
        seed=seed*1664525u+1013904223u;
        if(pattern==0u) value=(x/(1u+(y%37u)))*0x01010101u^y;
        else if(pattern==1u) value=((x/3u)%5u)*0x00102030u+(y/3u)*0x10000u;
        else value=seed;
        engine->screen[(size_t)y*width+x]=value;
      }
      size_t bytes=reference_frame(engine->screen,width,height,reference,capacity);
      REQUIRE(bytes,"reference encoding");
      REQUIRE(save(engine,&plain) && frame_section_matches(&plain,reference,bytes),
              "the banded encoder writes the plain frame encoding");
      engine->state_vm_size_hint=65536u;
      anygm_state_prepare(engine);
      REQUIRE(save(engine,&announced),"announced monitor-sized save");
      REQUIRE(engine_state_prepared_consumed(engine)==2,
              "the announced save consumed the frame encoded on its worker");
      REQUIRE(same(&plain,&announced),"an announced save writes the same frame encoding");
    }
  }
  engine->state_vm_size_hint=65536u;
  anygm_state_prepare(engine);
  printf("state prepare: ok (%zu bytes)\n",plain.size);
done:
  if(engine) anygm_destroy(engine);
  free(plain.data); free(announced.data); free(advanced.data); free(after.data);
  free(reference);
  anygm_synthetic_content_destroy(&fixture);
  return ok?EXIT_SUCCESS:EXIT_FAILURE;
}
