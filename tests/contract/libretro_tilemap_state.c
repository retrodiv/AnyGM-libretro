/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "libretro_internal.h"
#include "engine_internal.h"
#include "gml_builtin.h"
#include "synthetic_content.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Unlike the transport-only stub, this host links the actual adapter and engine.
 * Internal inspection observes map identities; every snapshot crosses libretro. */
static int negotiation,option_update,monitor_changed;
static unsigned frames,frame_width,frame_height;

static bool environment(unsigned command,void *data){
  if(command==RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS){
    if(negotiation>0) *(uint64_t*)data|=RETRO_SERIALIZATION_QUIRK_FRONT_VARIABLE_SIZE;
    return negotiation>=0;
  }
  if(command==RETRO_ENVIRONMENT_SET_PIXEL_FORMAT ||
     command==RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO ||
     command==RETRO_ENVIRONMENT_SET_GEOMETRY) return true;
  if(command==RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE){
    *(bool*)data=option_update!=0; option_update=0; return true;
  }
  if(command==RETRO_ENVIRONMENT_GET_VARIABLE){
    struct retro_variable *variable=data;
    variable->value=NULL;
    if(!strcmp(variable->key,"anygm_render_game_resolution")) variable->value="Off";
    if(!strcmp(variable->key,"anygm_width_resolution"))
      variable->value=monitor_changed?"1920":"1280";
    if(!strcmp(variable->key,"anygm_height_resolution"))
      variable->value=monitor_changed?"1080":"960";
    return variable->value!=NULL;
  }
  return false;
}

static void video(const void *data,unsigned width,unsigned height,size_t pitch){
  (void)pitch;
  if(data){ frames++; frame_width=width; frame_height=height; }
}

typedef struct Ring {
  uint8_t *bytes;
  size_t capacity,initial;
  unsigned accepted,rejected,serialize_calls;
} Ring;

static int push(Ring *ring){
  size_t answer=retro_serialize_size();
  if(!answer || (negotiation<=0 && answer!=ring->initial)){
    ring->rejected++; return 0;
  }
  if(answer>ring->capacity){
    if(negotiation<=0){ ring->rejected++; return 0; }
    uint8_t *grown=realloc(ring->bytes,answer);
    if(!grown) return 0;
    ring->bytes=grown; ring->capacity=answer;
  }
  /* The frontend size check happens before entering the core serializer. */
  ring->serialize_calls++;
  if(!retro_serialize(ring->bytes,ring->capacity)){ ring->rejected++; return 0; }
  ring->accepted++;
  return 1;
}

static int retained(int id,int next){
  GmlTileMap *map=gml_tilemap_find(&g_libretro.engine->vm,id);
  if(!map || map->x!=9.25 || map->y!=-3.5 ||
     g_libretro.engine->vm.next_tilemap_id!=next) return 0;
  for(int y=0;y<2;y++) for(int x=0;x<2;x++)
    if(map->tiles[4*(y*2+x)]!=(uint8_t)(1+y*2+x)) return 0;
  return g_libretro.engine->vm.n_tilemaps==2 &&
         g_libretro.engine->vm.tilemaps[1].x==0;
}

#define REQUIRE(condition,message) do { if(!(condition)){ \
  fprintf(stderr,"tilemap frontend (%d): %s\n",negotiation,message); ok=0; goto done; \
} } while(0)

static int run_case(int response){
  AnygmSyntheticContent fixture={0};
  Ring ring={0};
  uint8_t *cold=NULL,*saved=NULL,*before_reset_load=NULL,*after_reset_load=NULL;
  size_t saved_size=0;
  int ok=1;
  negotiation=response; frames=frame_width=frame_height=0;
  monitor_changed=option_update=0;
  REQUIRE(anygm_synthetic_tilemap_content_create(&fixture),"create fixture");
  retro_set_environment(environment); retro_set_video_refresh(video); retro_init();
  struct retro_game_info info={fixture.path,NULL,0,NULL};
  REQUIRE(retro_load_game(&info),"load authored tilemaps");
  REQUIRE(g_libretro.variable_state_supported==(response>0),"explicit negotiation");
  REQUIRE(g_libretro.engine->vm.n_tilemaps==2,"two loaded maps");
  int id=g_libretro.engine->vm.tilemaps[0].id,next=g_libretro.engine->vm.next_tilemap_id;

  ring.initial=ring.capacity=retro_serialize_size();
  REQUIRE(ring.capacity>0 && ring.capacity<1280u*960u*4u,"compact cold capacity");
  REQUIRE(g_libretro.startup_ring_compact,"compact session policy selected");
  ring.bytes=malloc(ring.capacity); cold=malloc(ring.capacity);
  REQUIRE(ring.bytes && cold,"allocate ring before first frame");
  REQUIRE(push(&ring),"cold push");
  memcpy(cold,ring.bytes,ring.initial);
  retro_run();
  REQUIRE(frames==1 && frame_width==1280 && frame_height==960,"first authored raster");
  REQUIRE(push(&ring),"first completed-frame push");

  GmlVal xargs[]={vreal(id),vreal(9.25)};
  GmlVal yargs[]={vreal(id),vreal(-3.5)};
  gml_builtin_call(&g_libretro.engine->vm,"tilemap_x",xargs,2);
  gml_builtin_call(&g_libretro.engine->vm,"tilemap_y",yargs,2);
  GmlTileMap *map=gml_tilemap_find(&g_libretro.engine->vm,id);
  REQUIRE(map!=NULL,"stable map handle");
  for(int y=0;y<2;y++) for(int x=0;x<2;x++)
    REQUIRE(gml_tilemap_set_cell(map,x,y,(uint32_t)(1+y*2+x)),"edit every cell");
  /* A detailed completed image forces a compact fixed slot to omit the optional
   * image instead of accidentally passing only because a blank image compresses. */
  size_t pixels=(size_t)frame_width*frame_height;
  for(size_t i=0;i<pixels;i++)
    g_libretro.engine->screen[i]=0xFF000000u|((uint32_t)i*2654435761u & 0xFFFFFFu);
  REQUIRE(push(&ring),"maximum fixture mutation and detailed-frame push");
  REQUIRE(retained(id,next),"mutations took effect before snapshot");
  saved_size=ring.capacity; saved=malloc(saved_size);
  REQUIRE(saved!=NULL,"retain same-execution snapshot");
  memcpy(saved,ring.bytes,saved_size);
  if(response<=0)
    REQUIRE(anygm_state_size(g_libretro.engine)>ring.capacity,"fixed push used frame-free fallback");

  gml_room_enter(&g_libretro.engine->vm,1);
  REQUIRE(g_libretro.engine->vm.n_tilemaps==0 && push(&ring),"empty-room push");
  REQUIRE(retro_unserialize(saved,saved_size) && retained(id,next),"restore maps from empty room");

  monitor_changed=option_update=1;
  retro_run();
  REQUIRE(g_libretro.engine->config.monitor_width==1920 &&
          g_libretro.engine->config.monitor_height==1080,"live monitor option transition");
  REQUIRE(push(&ring),"push after live monitor transition");

  retro_reset();
  REQUIRE(g_libretro.reset_pending_frame,"Reset awaits its first new frame");
  size_t reset_size=anygm_state_size(g_libretro.engine),written=0;
  before_reset_load=malloc(reset_size); after_reset_load=malloc(reset_size);
  REQUIRE(before_reset_load && after_reset_load,"allocate Reset guard");
  REQUIRE(anygm_state_save(g_libretro.engine,before_reset_load,reset_size,&written)==ANYGM_OK &&
          written==reset_size,"snapshot freshly reset state");
  REQUIRE(!retro_unserialize(cold,ring.initial),"old compact ring held before first reset frame");
  REQUIRE(anygm_state_save(g_libretro.engine,after_reset_load,reset_size,&written)==ANYGM_OK &&
          written==reset_size && !memcmp(before_reset_load,after_reset_load,reset_size),
          "rejected pre-reset ring leaves engine unchanged");
  REQUIRE(push(&ring),"fresh Reset push");
  retro_run();
  REQUIRE(push(&ring),"first reset-frame push");
  REQUIRE(retro_unserialize(saved,saved_size) && retained(id,next),"restore retained map after Reset");
  REQUIRE(push(&ring),"restored map push");
  REQUIRE(ring.accepted==8 && ring.rejected==0 && ring.serialize_calls==8,"all wrapper pushes accepted");
  printf("tilemap frontend: negotiation=%d cold=%zu final=%zu accepted=%u rejected=%u\n",
         response,ring.initial,ring.capacity,ring.accepted,ring.rejected);
done:
  free(before_reset_load); free(after_reset_load); free(cold); free(saved); free(ring.bytes);
  retro_unload_game(); retro_deinit(); anygm_synthetic_content_destroy(&fixture);
  return ok;
}

int main(void){
  for(int response=-1;response<=1;response++) if(!run_case(response)) return 1;
  return 0;
}
