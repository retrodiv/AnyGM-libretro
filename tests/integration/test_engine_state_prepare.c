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

#define REQUIRE(condition,message) do { if(!(condition)){ \
  fprintf(stderr,"state prepare: %s\n",message); ok=0; goto done; } } while(0)

int main(void){
  AnygmSyntheticContent fixture={0};
  AnygmEngine *engine=NULL;
  Saved plain={0},announced={0},advanced={0},after={0};
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
  engine->state_vm_size_hint=65536u;
  anygm_state_prepare(engine);
  printf("state prepare: ok (%zu bytes)\n",plain.size);
done:
  if(engine) anygm_destroy(engine);
  free(plain.data); free(announced.data); free(advanced.data); free(after.data);
  anygm_synthetic_content_destroy(&fixture);
  return ok?EXIT_SUCCESS:EXIT_FAILURE;
}
