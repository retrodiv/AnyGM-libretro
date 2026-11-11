/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "libretro_internal.h"
#include "engine_internal.h"
#include "gml_builtin.h"
#include "gml_audio.h"
#include "gml_software3d_internal.h"
#include "synthetic_content.h"

#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Unlike the transport-only stub, this host links the actual adapter and engine.
 * Internal inspection observes map identities; every snapshot crosses libretro. */
static int negotiation,option_update,monitor_changed;
static unsigned frames,frame_width,frame_height;
static retro_keyboard_event_t text_callback;

static bool environment(unsigned command,void *data){
  if(command==RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK){
    text_callback=((struct retro_keyboard_callback*)data)->callback; return true;
  }
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

static uint64_t read_word64(const uint8_t *bytes){
  uint64_t value=0;
  for(int i=0;i<8;i++) value|=(uint64_t)bytes[i]<<(8*i);
  return value;
}
static void write_word64(uint8_t *bytes,uint64_t value){
  for(int i=0;i<8;i++) bytes[i]=(uint8_t)(value>>(8*i));
}
static uint64_t payload_hash(const uint8_t *bytes,size_t size){
  uint64_t hash=UINT64_C(1469598103934665603);
  while(size>=8){ hash^=read_word64(bytes); hash*=UINT64_C(1099511628211); bytes+=8; size-=8; }
  while(size--){ hash^=*bytes++; hash*=UINT64_C(1099511628211); }
  return hash;
}

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

static void count_root(void *context,GmlVal value){
  (void)value; (*(int*)context)++;
}

static int timer_roots(void){
  int count=0;
  gml_builtin_state_visit_values(g_libretro.engine->vm.builtins,count_root,&count);
  return count;
}
static int maximum_text(void){
  GmlVal text=gml_vm_identifier_get(&g_libretro.engine->vm,"keyboard_string");
  int correct=text.t==V_STR && text.s && strlen(text.s)==4096;
  if(correct) for(size_t i=0;i<1024;i++)
    if(memcmp(text.s+4*i,"\xf0\x9f\x99\x82",4)){ correct=0; break; }
  if(text.t==V_STR && text.d) free((char*)text.s);
  return correct;
}
static int empty_text(void){
  GmlVal text=gml_vm_identifier_get(&g_libretro.engine->vm,"keyboard_string");
  int correct=text.t==V_STR && text.s && !text.s[0];
  if(text.t==V_STR && text.d) free((char*)text.s);
  return correct;
}

#define REQUIRE(condition,message) do { if(!(condition)){ \
  fprintf(stderr,"tilemap frontend (%d): %s\n",negotiation,message); ok=0; goto done; \
} } while(0)

static int run_case(int response){
  AnygmSyntheticContent fixture={0};
  Ring ring={0};
  uint8_t *cold=NULL,*saved=NULL,*dormant=NULL,*before_reset_load=NULL,*after_reset_load=NULL;
  size_t saved_size=0,dormant_size=0;
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
  /* Fill the bounded shared pool after the frontend allocated its cold ring.
   * The authored startup code is an inert callable; native/language tests own
   * callback effects, while this fixture observes transport and exact expiry. */
  REQUIRE(g_libretro.engine->win.n_code>0,"authored inert callback code");
  GmlVal timer_args[]={vreal(4),vreal(1),vreal(GML_FUNCVAL_TAG)};
  for(int i=0;i<256;i++){
    GmlVal timer=gml_builtin_call(&g_libretro.engine->vm,"call_later",timer_args,3);
    REQUIRE(timer.t==V_REAL && timer.d>=3,"allocate pending call after cold capacity query");
  }
  REQUIRE(timer_roots()==512,"all pending calls retained before first frame");
  uint32_t text_batch[ANYGM_MAX_TEXT_EVENTS];
  for(size_t i=0;i<ANYGM_MAX_TEXT_EVENTS;i++) text_batch[i]=0x1f642;
  for(int batch=0;batch<3;batch++)
    REQUIRE(gml_input_text(&g_libretro.engine->vm,text_batch,ANYGM_MAX_TEXT_EVENTS),
            "prepare Unicode growth after the cold capacity query");
  REQUIRE(text_callback,"text callback is accepted");
  for(size_t i=0;i<ANYGM_MAX_TEXT_EVENTS;i++)
    text_callback(true,RETROK_UNKNOWN,0x1f642,0);
  retro_run();
  REQUIRE(frames==1 && frame_width==1280 && frame_height==960,"first authored raster");
  REQUIRE(maximum_text() && !g_libretro.keyboard_text_count,
          "first input phase completes the largest UTF-8 text buffer exactly once");
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
  REQUIRE(maximum_text(),"frontend restoration preserves maximum text contents");
  REQUIRE(timer_roots()==512,"hidden callbacks survive room change and frontend restoration");

  *gml_varmap_put(&g_libretro.engine->vm.globals,"room_persistent")=vreal(1);
  gml_room_enter(&g_libretro.engine->vm,1);
  REQUIRE(g_libretro.engine->vm.n_tilemaps==0 && push(&ring),"dormant-room push");
  dormant_size=ring.capacity; dormant=malloc(dormant_size);
  REQUIRE(dormant!=NULL,"retain dormant same-execution snapshot");
  memcpy(dormant,ring.bytes,dormant_size);
  gml_room_enter(&g_libretro.engine->vm,0);
  REQUIRE(retained(id,next),"persistent return before restoration");
  REQUIRE(retro_unserialize(dormant,dormant_size) &&
          g_libretro.engine->vm.room_index==1 && g_libretro.engine->vm.n_tilemaps==0,
          "restore inactive map ownership through frontend");
  gml_room_enter(&g_libretro.engine->vm,0);
  REQUIRE(retained(id,next) && push(&ring),"reactivated dormant-map push");

  monitor_changed=option_update=1;
  retro_run();
  REQUIRE(g_libretro.engine->config.monitor_width==1920 &&
          g_libretro.engine->config.monitor_height==1080,"live monitor option transition");
  REQUIRE(push(&ring),"push after live monitor transition");
  REQUIRE(maximum_text(),"live geometry does not alter text contents");

  retro_reset();
  REQUIRE(g_libretro.reset_pending_frame,"Reset awaits its first new frame");
  REQUIRE(timer_roots()==0,"Reset removes pending callbacks");
  REQUIRE(empty_text(),"Reset restores empty text defaults with the old ring alive");
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
  REQUIRE(maximum_text(),"a same-run pre-Reset snapshot restores maximum text");
  REQUIRE(timer_roots()==512,"pre-Reset pending callbacks restore through frontend");
  REQUIRE(push(&ring),"restored map push");
  REQUIRE(retro_unserialize(dormant,dormant_size) && g_libretro.engine->vm.room_index==1,
          "restore dormant maps after Reset");
  gml_room_enter(&g_libretro.engine->vm,0);
  REQUIRE(retained(id,next) && push(&ring),"post-Reset dormant-map push");
  REQUIRE(ring.accepted==11 && ring.rejected==0 && ring.serialize_calls==11,"all wrapper pushes accepted");
  if(response>0){
    long before=g_libretro.engine->vm.frame;
    retro_run();
    REQUIRE(g_libretro.engine->vm.frame==before && timer_roots()==512,
            "complete-state presentation does not advance pending callbacks");
  }
  retro_run(); retro_run();
  REQUIRE(timer_roots()==512,"restoration preserves the remaining callback delay");
  retro_run();
  REQUIRE(timer_roots()==0,"restored one-shots expire and release all callback roots");
  REQUIRE(push(&ring) && ring.accepted==12 && ring.rejected==0 && ring.serialize_calls==12,
          "frontend still accepts a snapshot after callback completion");
  printf("tilemap frontend: negotiation=%d cold=%zu final=%zu accepted=%u rejected=%u\n",
         response,ring.initial,ring.capacity,ring.accepted,ring.rejected);
done:
  free(before_reset_load); free(after_reset_load); free(cold); free(saved); free(dormant); free(ring.bytes);
  retro_unload_game(); retro_deinit(); anygm_synthetic_content_destroy(&fixture);
  return ok;
}

static int run_ds_case(int response){
  AnygmSyntheticContent fixture={0};
  Ring ring={0};
  uint8_t *cold=NULL,*saved=NULL;
  int ok=1;
  GmlVal map=vreal(-1),list=vreal(-1),buffer=vreal(-1);
  /* DS strings may escape their containers and intentionally outlive destroy/reset.
   * These synthetic keys escape nowhere else; retain their copies until engine teardown. */
  char *fixture_keys[2300]={0};
  negotiation=response;
  monitor_changed=option_update=0;
  REQUIRE(anygm_synthetic_content_create(&fixture),"create DS capacity fixture");
  retro_set_environment(environment); retro_set_video_refresh(video); retro_init();
  struct retro_game_info info={fixture.path,NULL,0,NULL};
  REQUIRE(retro_load_game(&info),"load DS capacity fixture");
  ring.initial=ring.capacity=retro_serialize_size();
  ring.bytes=malloc(ring.capacity);
  cold=malloc(ring.capacity);
  REQUIRE(ring.bytes && cold && push(&ring),"DS ring allocated before first frame");
  memcpy(cold,ring.bytes,ring.capacity);
  retro_run();
  REQUIRE(push(&ring),"DS first completed frame");
  for(int i=0;i<2300;i++){
    GmlVM *vm=&g_libretro.engine->vm;
    map=gml_builtin_call(vm,"ds_map_create",NULL,0);
    list=gml_builtin_call(vm,"ds_list_create",NULL,0);
    REQUIRE(map.d>0 && list.d>0,"grow both DS pools after cold ring allocation");
    GmlVal add[]={map,vstr("number"),vreal(i)};
    GmlVal push_args[]={list,vreal(i)};
    gml_builtin_call(vm,"ds_map_add",add,3);
    gml_builtin_call(vm,"ds_list_add",push_args,2);
    GmlVal fixture_key=gml_builtin_call(vm,"ds_map_find_first",&map,1);
    REQUIRE(fixture_key.t==V_STR && fixture_key.s &&
            !strcmp(fixture_key.s,"number"),
            "retain the fixture-owned cloned map key");
    fixture_keys[i]=(char*)fixture_key.s;
    if(i<64){
      GmlVal size=vreal(8192);
      buffer=gml_builtin_call(vm,"buffer_create",&size,1);
      REQUIRE(buffer.d>0,"grow buffer handles after cold ring allocation");
      GmlVal poke[]={buffer,vreal(8191),vreal(1),vreal(i)};
      gml_builtin_call(vm,"buffer_poke",poke,4);
    }
  }
  REQUIRE(push(&ring),"DS maximum growth push");
  size_t saved_size=ring.capacity;
  saved=malloc(saved_size);
  REQUIRE(saved,"retain DS same-execution snapshot");
  memcpy(saved,ring.bytes,saved_size);
  gml_builtin_call(&g_libretro.engine->vm,"ds_map_destroy",&map,1);
  gml_builtin_call(&g_libretro.engine->vm,"ds_list_destroy",&list,1);
  gml_builtin_call(&g_libretro.engine->vm,"buffer_delete",&buffer,1);
  REQUIRE(retro_unserialize(saved,saved_size),"restore destroyed DS handles");
  GmlVal get_map[]={map,vstr("number")},get_list[]={list,vreal(0)};
  GmlVal peek_buffer[]={buffer,vreal(8191),vreal(1)};
  REQUIRE(gml_builtin_call(&g_libretro.engine->vm,"buffer_peek",peek_buffer,3).d==63,
          "restored buffer tail bytes beyond former handle capacity");
  REQUIRE(gml_builtin_call(&g_libretro.engine->vm,"ds_map_find_value",get_map,2).d==2299,
          "restored last map value beyond former capacity");
  REQUIRE(gml_builtin_call(&g_libretro.engine->vm,"ds_list_find_value",get_list,2).d==2299,
          "restored last list value beyond former capacity");
  /* Corrupt the final object-table count, after both expanded pools decoded.
   * Keep framing/checksum valid so the engine must exercise its rollback path. */
  memcpy(ring.bytes,saved,saved_size);
  size_t vm_end=112+(size_t)read_word64(saved+64)+(size_t)read_word64(saved+72)+
                     (size_t)read_word64(saved+80);
  size_t object_bytes=4+28*(size_t)g_libretro.engine->vm.n_objects;
  /* This neutral fixture has neither reordered instances nor array ownership. Validate both
   * trailers and every original object word before corrupting the count, not a property. */
  const size_t trailer_bytes=24; /* order count, then the empty ownership trailer */
  REQUIRE(vm_end>=object_bytes+trailer_bytes && vm_end<=saved_size &&
          !g_libretro.engine->vm.next_array_owner &&
          !memcmp(saved+vm_end-trailer_bytes,(const unsigned char[24]){0},trailer_bytes),
          "locate VM object table before empty order and ownership trailers");
  size_t object_at=vm_end-trailer_bytes-object_bytes;
  REQUIRE((uint32_t)read_word64(saved+object_at)==(uint32_t)g_libretro.engine->vm.n_objects,
          "verify original object-table count before corruption");
  for(int object=0;object<g_libretro.engine->vm.n_objects;object++){
    const GmlObject *live=&g_libretro.engine->vm.objects[object];
    const int words[]={live->sprite_index,live->mask_index,live->parent,live->depth,
                       live->visible,live->solid,live->persistent};
    for(unsigned word=0;word<7;word++)
      REQUIRE((uint32_t)read_word64(saved+object_at+4+28*(size_t)object+4*word)==
              (uint32_t)words[word],"verify original object words before corruption");
  }
  memset(ring.bytes+object_at,255,4);
  size_t payload_size=(size_t)read_word64(saved+96);
  REQUIRE(payload_size<=saved_size-112,"validate payload checksum span");
  write_word64(ring.bytes+56,payload_hash(ring.bytes+112,payload_size));
  REQUIRE(!retro_unserialize(ring.bytes,saved_size),"reject malformed VM after DS decoding");
  REQUIRE(retro_serialize(ring.bytes,saved_size) && !memcmp(ring.bytes,saved,saved_size),
          "expanded DS restoration rejection leaves the complete engine unchanged");
  monitor_changed=option_update=1;
  retro_run();
  REQUIRE(push(&ring),"DS push after live geometry change");
  retro_reset();
  REQUIRE(push(&ring),"DS push immediately across Reset");
  REQUIRE(retro_unserialize(saved,saved_size)==(response>0),
          "Reset accepts complete variable states but rejects frame-free fixed entries before its first frame");
  retro_run();
  REQUIRE(push(&ring),"DS push after first fresh reset frame");
  REQUIRE(retro_unserialize(saved,saved_size),"old DS ring entry after reset first frame");
  REQUIRE(push(&ring),"DS restored ring entry after Reset");
  REQUIRE(retro_unserialize(cold,ring.initial),"restore cold state into grown DS session");
  REQUIRE(push(&ring),"DS cold restoration push");
  REQUIRE(ring.accepted==8 && ring.rejected==0,"all DS frontend pushes accepted");
  printf("DS frontend: negotiation=%d cold=%zu final=%zu accepted=%u rejected=%u\n",
         response,ring.initial,ring.capacity,ring.accepted,ring.rejected);
done:
  free(cold); free(saved); free(ring.bytes);
  retro_unload_game(); retro_deinit(); anygm_synthetic_content_destroy(&fixture);
  for(int i=0;i<2300;i++) free(fixture_keys[i]);
  return ok;
}

static int run_vertex_case(int response){
  AnygmSyntheticContent fixture={0}; Ring ring={0};
  uint8_t *cold=NULL,*saved=NULL; size_t saved_size=0;
  int ok=1;
  negotiation=response; monitor_changed=option_update=0;
  REQUIRE(anygm_synthetic_tilemap_content_create(&fixture),"create vertex capacity fixture");
  retro_set_environment(environment); retro_set_video_refresh(video); retro_init();
  struct retro_game_info info={fixture.path,NULL,0,NULL};
  REQUIRE(retro_load_game(&info),"load vertex capacity fixture");
  ring.initial=ring.capacity=retro_serialize_size();
  REQUIRE(ring.capacity>0 && g_libretro.startup_ring_compact,"vertex cold compact policy");
  ring.bytes=malloc(ring.capacity); cold=malloc(ring.capacity);
  REQUIRE(ring.bytes && cold && push(&ring),"vertex ring before first frame");
  memcpy(cold,ring.bytes,ring.initial); retro_run();
  REQUIRE(push(&ring),"vertex first completed frame");
  GmlVM *vm=&g_libretro.engine->vm;
  gml_builtin_call(vm,"vertex_format_begin",NULL,0);
  gml_builtin_call(vm,"vertex_format_add_position",NULL,0);
  gml_builtin_call(vm,"vertex_format_add_colour",NULL,0);
  GmlVal format=gml_builtin_call(vm,"vertex_format_end",NULL,0);
  GmlVal create[]={vreal(512*12),vreal(0),vreal(1)};
  GmlVal source=gml_builtin_call(vm,"buffer_create",create,3);
  GmlVal poke[]={source,vreal(0),vreal(8),vreal(1.25)};
  gml_builtin_call(vm,"buffer_poke",poke,4);
  poke[1]=vreal(4); poke[3]=vreal(2.5); gml_builtin_call(vm,"buffer_poke",poke,4);
  GmlVal from[]={source,format};
  GmlVal buffer=gml_builtin_call(vm,"vertex_create_buffer_from_buffer",from,2);
  REQUIRE(buffer.t==V_REAL && buffer.d>=0,"create vertex data after cold allocation");
  GmlVal pending=gml_builtin_call(vm,"vertex_create_buffer",NULL,0);
  GmlVal begin[]={pending,format}; gml_builtin_call(vm,"vertex_begin",begin,2);
  GmlVal position[]={pending,vreal(17.25),vreal(19.5)};
  gml_builtin_call(vm,"vertex_position",position,3);
  REQUIRE(push(&ring),"vertex resources and unfinished vertex fit old ring");
  saved_size=ring.capacity; saved=malloc(saved_size);
  REQUIRE(saved,"allocate vertex snapshot copy"); memcpy(saved,ring.bytes,saved_size);
  poke[1]=vreal(0); poke[3]=vreal(99); gml_builtin_call(vm,"buffer_poke",poke,4);
  GmlVal update[]={buffer,vreal(0),source,vreal(0),vreal(8)};
  gml_builtin_call(vm,"vertex_update_buffer_from_buffer",update,5);
  REQUIRE(vm->software3d->vertex_buffer[(int)buffer.d].vertex[0].x==99,"observe changed vertex before rewind");
  REQUIRE(push(&ring),"updated vertex ring push");
  REQUIRE(retro_unserialize(saved,saved_size),"restore vertex resource ring entry");
  vm=&g_libretro.engine->vm;
  GmlVertexBuffer *restored=&vm->software3d->vertex_buffer[(int)buffer.d];
  REQUIRE(restored->vertex_n==512 && restored->vertex[0].x==1.25 && restored->vertex[0].y==2.5,
          "rewind restores original vertex data");
  REQUIRE(vm->software3d->vertex_buffer[(int)pending.d].attr_index==1 &&
          vm->software3d->vertex_buffer[(int)pending.d].partial.x==17.25,
          "rewind restores unfinished vertex construction");
  monitor_changed=option_update=1; retro_run();
  REQUIRE(push(&ring),"vertex ring accepts live high-resolution geometry");
  retro_reset(); REQUIRE(push(&ring),"vertex ring immediately across Reset");
  REQUIRE(retro_unserialize(saved,saved_size)==(response>0),"vertex Reset follows fixed and variable state policy");
  retro_run(); REQUIRE(push(&ring),"vertex ring after fresh reset frame");
  REQUIRE(retro_unserialize(saved,saved_size) && push(&ring),"vertex resources restore after Reset");
  REQUIRE(retro_unserialize(cold,ring.initial) && push(&ring),"vertex cold state restores empty resources");
  REQUIRE(!gml_software3d_vertex_buffer_exists(g_libretro.engine->vm.software3d,(int)buffer.d),
          "cold state removes later vertex resources");
  REQUIRE(ring.accepted==9 && ring.rejected==0,"every vertex frontend push accepted");
  printf("vertex frontend: negotiation=%d cold=%zu final=%zu accepted=%u rejected=%u\n",
         response,ring.initial,ring.capacity,ring.accepted,ring.rejected);
done:
  free(cold); free(saved); free(ring.bytes);
  retro_unload_game(); retro_deinit(); anygm_synthetic_content_destroy(&fixture);
  return ok;
}

static int run_timeline_case(int response){
  AnygmSyntheticContent fixture={0}; Ring ring={0};
  uint8_t *cold=NULL,*saved=NULL; size_t saved_size=0;
  int ok=1;
  negotiation=response; monitor_changed=option_update=0;
  REQUIRE(anygm_synthetic_tilemap_content_create(&fixture),"create timeline capacity fixture");
  retro_set_environment(environment); retro_set_video_refresh(video); retro_init();
  struct retro_game_info info={fixture.path,NULL,0,NULL};
  REQUIRE(retro_load_game(&info),"load timeline capacity fixture");
  ring.initial=ring.capacity=retro_serialize_size();
  REQUIRE(ring.capacity>0 && g_libretro.startup_ring_compact,"timeline cold compact policy");
  ring.bytes=malloc(ring.capacity); cold=malloc(ring.capacity);
  REQUIRE(ring.bytes && cold && push(&ring),"timeline ring before first frame");
  memcpy(cold,ring.bytes,ring.initial); retro_run();
  REQUIRE(push(&ring),"timeline first completed frame");
  GmlVM *vm=&g_libretro.engine->vm;
  REQUIRE(vm->win->n_code>0,"timeline inert callback code");
  GmlVal timeline=gml_builtin_call(vm,"timeline_add",NULL,0);
  REQUIRE(timeline.t==V_REAL && timeline.d>=0,"create timeline after cold capacity");
  int index=(int)timeline.d;
  GmlVal add[]={timeline,vreal(0),vreal(GML_FUNCVAL_TAG)};
  for(int step=0;step<512;step++){
    add[1]=vreal(step); gml_builtin_call(vm,"timeline_moment_add_script",add,3);
  }
  REQUIRE(vm->timelines[index].n==512,"largest authored timeline transition");
  REQUIRE(push(&ring),"timeline growth fits existing frontend ring");
  saved_size=ring.capacity; saved=malloc(saved_size);
  REQUIRE(saved,"retain timeline snapshot"); memcpy(saved,ring.bytes,saved_size);
  gml_builtin_call(vm,"timeline_delete",&timeline,1);
  REQUIRE(!vm->timelines[index].name && push(&ring),"timeline deletion push");
  REQUIRE(retro_unserialize(saved,saved_size),"rewind restores deleted timeline");
  vm=&g_libretro.engine->vm;
  REQUIRE(vm->timelines[index].name && vm->timelines[index].n==512 &&
          vm->timelines[index].moments[511].callbacks[0].d==GML_FUNCVAL_TAG,
          "rewind restores exact timeline callable/step table");
  gml_builtin_call(vm,"game_restart",NULL,0); retro_run();
  vm=&g_libretro.engine->vm;
  REQUIRE(vm->n_timelines>index && vm->timelines[index].name && vm->timelines[index].n==512 &&
          vm->timelines[index].moments[511].callbacks[0].d==GML_FUNCVAL_TAG,
          "game_restart preserves shared timeline resources");
  REQUIRE(push(&ring),"timeline ring accepts game_restart");
  monitor_changed=option_update=1; retro_run();
  REQUIRE(push(&ring),"timeline ring accepts live high-resolution geometry");
  retro_reset(); REQUIRE(push(&ring),"timeline ring immediately across Reset");
  REQUIRE(retro_unserialize(saved,saved_size)==(response>0),"timeline Reset fixed/variable policy");
  retro_run(); REQUIRE(push(&ring),"timeline ring after fresh reset frame");
  REQUIRE(retro_unserialize(saved,saved_size) && push(&ring),"timeline resources restore after Reset");
  REQUIRE(retro_unserialize(cold,ring.initial) && push(&ring),"timeline cold state restores authored table");
  REQUIRE(g_libretro.engine->vm.n_timelines==index,"cold state removes later timeline identities");
  REQUIRE(ring.accepted==10 && ring.rejected==0,"every timeline frontend push accepted");
  printf("timeline frontend: negotiation=%d cold=%zu final=%zu accepted=%u rejected=%u\n",
         response,ring.initial,ring.capacity,ring.accepted,ring.rejected);
done:
  free(cold); free(saved); free(ring.bytes);
  retro_unload_game(); retro_deinit(); anygm_synthetic_content_destroy(&fixture);
  return ok;
}

static int run_music_case(int response){
  AnygmSyntheticContent fixture={0}; Ring ring={0};
  uint8_t *cold=NULL,*saved=NULL; size_t saved_size=0;
  int ok=1;
  negotiation=response; monitor_changed=option_update=0;
  REQUIRE(anygm_synthetic_tilemap_content_create(&fixture),"create music capacity fixture");
  retro_set_environment(environment); retro_set_video_refresh(video); retro_init();
  struct retro_game_info info={fixture.path,NULL,0,NULL};
  REQUIRE(retro_load_game(&info),"load music capacity fixture");
  ring.initial=ring.capacity=retro_serialize_size();
  REQUIRE(ring.capacity>0 && g_libretro.startup_ring_compact,"music cold compact policy");
  ring.bytes=malloc(ring.capacity); cold=malloc(ring.capacity);
  REQUIRE(ring.bytes && cold && push(&ring),"music ring before first frame");
  memcpy(cold,ring.bytes,ring.initial); retro_run(); REQUIRE(push(&ring),"music first completed frame");
  GmlVM *vm=&g_libretro.engine->vm;
  GmlVal create[]={vreal(65536),vreal(0),vreal(1)};
  GmlVal source=gml_builtin_call(vm,"buffer_create",create,3);
  GmlVal queue_args[]={vreal(4),vreal(44100),vreal(0)};
  GmlVal queue=gml_builtin_call(vm,"audio_create_play_queue",queue_args,3);
  GmlVal submit[]={queue,source,vreal(0),vreal(65536)};
  gml_builtin_call(vm,"audio_queue_sound",submit,4);
  /* Seed the same audio owner directly: this neutral transport fixture is a later profile,
   * while public legacy-profile dispatch is proved by the PCM and language controls. */
  gml_audio_music_gain(g_libretro.engine->audio,0.75,0);
  int voice=gml_audio_music_play(g_libretro.engine->audio,(int)queue.d,0);
  gml_audio_music_gain(g_libretro.engine->audio,0.25,1000);
  REQUIRE(voice>=1000000 && gml_audio_music_is_playing(g_libretro.engine->audio),"music voice after cold capacity query");
  REQUIRE(push(&ring),"largest music/PCM queue transition fits cold ring");
  saved_size=ring.capacity; saved=malloc(saved_size);
  REQUIRE(saved,"retain music snapshot"); memcpy(saved,ring.bytes,saved_size);
  gml_audio_music_stop(g_libretro.engine->audio);
  REQUIRE(!gml_audio_music_is_playing(g_libretro.engine->audio) && push(&ring),"stopped music push");
  REQUIRE(retro_unserialize(saved,saved_size) && gml_audio_music_is_playing(g_libretro.engine->audio),
          "rewind restores music identity and pending PCM");
  monitor_changed=option_update=1; retro_run(); REQUIRE(push(&ring),"music ring after live geometry");
  retro_reset(); REQUIRE(!gml_audio_music_is_playing(g_libretro.engine->audio),"Reset clears music ownership");
  REQUIRE(push(&ring),"music ring immediately across Reset");
  REQUIRE(retro_unserialize(saved,saved_size)==(response>0),"music Reset fixed/variable policy");
  retro_run(); REQUIRE(push(&ring),"music ring after first reset frame");
  REQUIRE(retro_unserialize(saved,saved_size) && gml_audio_music_is_playing(g_libretro.engine->audio) && push(&ring),
          "music resource and fade restore after Reset");
  REQUIRE(retro_unserialize(cold,ring.initial) && !gml_audio_music_is_playing(g_libretro.engine->audio) && push(&ring),
          "cold state removes later music ownership");
  REQUIRE(ring.accepted==9 && ring.rejected==0,"every music frontend push accepted");
  printf("music frontend: negotiation=%d cold=%zu final=%zu accepted=%u rejected=%u\n",
         response,ring.initial,ring.capacity,ring.accepted,ring.rejected);
done:
  free(cold); free(saved); free(ring.bytes);
  retro_unload_game(); retro_deinit(); anygm_synthetic_content_destroy(&fixture);
  return ok;
}

static int run_emitter_case(int response){
  AnygmSyntheticContent fixture={0}; Ring ring={0};
  uint8_t *cold=NULL,*saved=NULL; size_t saved_size=0;
  int ok=1;
  negotiation=response; monitor_changed=option_update=0;
  REQUIRE(anygm_synthetic_tilemap_content_create(&fixture),"create emitter capacity fixture");
  retro_set_environment(environment); retro_set_video_refresh(video); retro_init();
  struct retro_game_info info={fixture.path,NULL,0,NULL};
  REQUIRE(retro_load_game(&info),"load emitter capacity fixture");
  ring.initial=ring.capacity=retro_serialize_size();
  REQUIRE(ring.capacity>0 && g_libretro.startup_ring_compact,"emitter cold compact policy");
  ring.bytes=malloc(ring.capacity); cold=malloc(ring.capacity);
  REQUIRE(ring.bytes && cold && push(&ring),"emitter ring before first frame");
  memcpy(cold,ring.bytes,ring.initial); retro_run(); REQUIRE(push(&ring),"emitter first frame");
  GmlVM *vm=&g_libretro.engine->vm;
  GmlVal create[]={vreal(65536),vreal(0),vreal(1)};
  GmlVal source=gml_builtin_call(vm,"buffer_create",create,3);
  GmlVal queue_args[]={vreal(4),vreal(44100),vreal(0)};
  GmlVal queue=gml_builtin_call(vm,"audio_create_play_queue",queue_args,3);
  GmlVal submit[]={queue,source,vreal(0),vreal(65536)};
  gml_builtin_call(vm,"audio_queue_sound",submit,4);
  GmlVal emitter=gml_builtin_call(vm,"audio_emitter_create",NULL,0);
  GmlVal position[]={emitter,vreal(0),vreal(0),vreal(10)};
  GmlVal pitch[]={emitter,vreal(2)};
  GmlVal velocity[]={emitter,vreal(0),vreal(0),vreal(-171.65)};
  gml_builtin_call(vm,"audio_emitter_position",position,4);
  gml_builtin_call(vm,"audio_emitter_pitch",pitch,2);
  gml_builtin_call(vm,"audio_emitter_velocity",velocity,4);
  GmlVal play[]={emitter,queue,vreal(0),vreal(0)};
  GmlVal voice=gml_builtin_call(vm,"audio_play_sound_on",play,4);
  REQUIRE(voice.d>=1000000 && push(&ring),"largest emitter and PCM transition fits cold ring");
  saved_size=ring.capacity; saved=malloc(saved_size);
  REQUIRE(saved,"retain emitter snapshot"); memcpy(saved,ring.bytes,saved_size);
  gml_builtin_call(vm,"audio_emitter_free",&emitter,1);
  REQUIRE(!gml_audio_is_playing(g_libretro.engine->audio,(int)voice.d) && push(&ring),"freed emitter push");
  REQUIRE(retro_unserialize(saved,saved_size) && gml_audio_is_playing(g_libretro.engine->audio,(int)voice.d),
          "rewind restores emitter voice");
  monitor_changed=option_update=1; retro_run(); REQUIRE(push(&ring),"emitter ring after geometry");
  retro_reset(); vm=&g_libretro.engine->vm;
  REQUIRE(gml_builtin_call(vm,"audio_emitter_exists",&emitter,1).d==0 && push(&ring),"Reset clears emitters");
  REQUIRE(retro_unserialize(saved,saved_size)==(response>0),"emitter Reset fixed/variable policy");
  retro_run(); REQUIRE(push(&ring),"emitter ring after first reset frame");
  REQUIRE(retro_unserialize(saved,saved_size),"emitter resources restore after Reset");
  vm=&g_libretro.engine->vm;
  /* New playback after restore must derive pitch from the restored emitter metadata. */
  gml_audio_stop(g_libretro.engine->audio,(int)voice.d);
  gml_builtin_call(vm,"audio_queue_sound",submit,4);
  voice=gml_builtin_call(vm,"audio_play_sound_on",play,4);
  int16_t output[32]; gml_audio_mix(g_libretro.engine->audio,output,16);
  double progress=gml_audio_sound_get_track_position(g_libretro.engine->audio,(int)voice.d);
  REQUIRE(voice.d>=1000000 && fabs(progress-64.0/44100.0)<1e-12 && push(&ring),
          "restored pitch and velocity affect new queue playback");
  REQUIRE(retro_unserialize(cold,ring.initial) &&
          gml_builtin_call(&g_libretro.engine->vm,"audio_emitter_exists",&emitter,1).d==0 && push(&ring),
          "cold state removes later emitters");
  REQUIRE(ring.accepted==9 && ring.rejected==0,"every emitter frontend push accepted");
  printf("emitter frontend: negotiation=%d cold=%zu final=%zu accepted=%u rejected=%u\n",
         response,ring.initial,ring.capacity,ring.accepted,ring.rejected);
done:
  free(cold); free(saved); free(ring.bytes);
  retro_unload_game(); retro_deinit(); anygm_synthetic_content_destroy(&fixture);
  return ok;
}

static int blend_factors_equal(GmlVM *vm,const int expected[4]){
  GmlVal value=gml_builtin_call(vm,"gpu_get_blendmode_ext_sepalpha",NULL,0);
  int ok=value.t==V_ARR && gml_val_array_length(value)==4;
  for(int i=0;i<4 && ok;i++)ok=gml_arr_get(value,i).d==expected[i];
  gml_values_release_owned(&value,1);return ok;
}

static int run_blend_case(int response){
  AnygmSyntheticContent fixture={0};Ring ring={0};
  uint8_t *cold=NULL,*saved=NULL;size_t saved_size=0;int ok=1;
  const int stacked[]={2,1,1,2},live[]={7,8,9,10},normal[]={5,6,5,6};
  negotiation=response;monitor_changed=option_update=0;
  REQUIRE(anygm_synthetic_tilemap_content_create(&fixture),"create blend capacity fixture");
  retro_set_environment(environment);retro_set_video_refresh(video);retro_init();
  struct retro_game_info info={fixture.path,NULL,0,NULL};
  REQUIRE(retro_load_game(&info),"load blend capacity fixture");
  ring.initial=ring.capacity=retro_serialize_size();
  REQUIRE(ring.capacity>0 && g_libretro.startup_ring_compact,"blend cold compact policy");
  ring.bytes=malloc(ring.capacity);cold=malloc(ring.capacity);
  REQUIRE(ring.bytes && cold && push(&ring),"blend ring before first frame");
  memcpy(cold,ring.bytes,ring.initial);retro_run();REQUIRE(push(&ring),"blend first frame");
  GmlVM *vm=&g_libretro.engine->vm;
  gml_builtin_call(vm,"gpu_set_blendmode_ext_sepalpha",(GmlVal[]){vreal(2),vreal(1),vreal(1),vreal(2)},4);
  gml_builtin_call(vm,"gpu_set_blendequation_sepalpha",(GmlVal[]){vreal(3),vreal(5)},2);
  for(int i=0;i<16;i++)gml_builtin_call(vm,"gpu_push_state",NULL,0);
  gml_builtin_call(vm,"gpu_set_blendmode_ext_sepalpha",(GmlVal[]){vreal(7),vreal(8),vreal(9),vreal(10)},4);
  REQUIRE(blend_factors_equal(vm,live) && push(&ring),"largest blend stack fits cold ring");
  saved_size=ring.capacity;saved=malloc(saved_size);REQUIRE(saved,"retain blend snapshot");
  memcpy(saved,ring.bytes,saved_size);
  gml_builtin_call(vm,"gpu_set_blendmode",(GmlVal[]){vreal(0)},1);
  REQUIRE(blend_factors_equal(vm,normal) && push(&ring),"changed blend push");
  REQUIRE(retro_unserialize(saved,saved_size) && blend_factors_equal(&g_libretro.engine->vm,live),
          "rewind restores independent live factors");
  monitor_changed=option_update=1;retro_run();REQUIRE(push(&ring),"blend ring after geometry");
  retro_reset();vm=&g_libretro.engine->vm;
  REQUIRE(blend_factors_equal(vm,normal) && push(&ring),"Reset clears live blend");
  REQUIRE(retro_unserialize(saved,saved_size)==(response>0),"blend Reset fixed/variable policy");
  retro_run();REQUIRE(push(&ring),"blend ring after first reset frame");
  REQUIRE(retro_unserialize(saved,saved_size) && blend_factors_equal(&g_libretro.engine->vm,live),
          "blend restore after Reset");
  vm=&g_libretro.engine->vm;gml_builtin_call(vm,"gpu_pop_state",NULL,0);
  REQUIRE(blend_factors_equal(vm,stacked) && push(&ring),"restored stack contains independent factors");
  REQUIRE(retro_unserialize(cold,ring.initial) && blend_factors_equal(&g_libretro.engine->vm,normal) && push(&ring),
          "cold restore removes later blend stack");
  REQUIRE(ring.accepted==9 && ring.rejected==0,"every blend frontend push accepted");
  printf("blend frontend: negotiation=%d cold=%zu final=%zu accepted=%u rejected=%u\n",
         response,ring.initial,ring.capacity,ring.accepted,ring.rejected);
done:
  free(cold);free(saved);free(ring.bytes);
  retro_unload_game();retro_deinit();anygm_synthetic_content_destroy(&fixture);return ok;
}

static int gamepad_thresholds_equal(GmlVM *vm,int defaults){
  for(int device=0;device<16;device++){
    GmlVal value=gml_builtin_call(vm,"gamepad_get_button_threshold",(GmlVal[]){vreal(device)},1);
    if(value.t!=V_REAL || value.d!=(defaults?0.5:device/16.0))return 0;
  }
  return 1;
}

static int run_gamepad_threshold_case(int response){
  AnygmSyntheticContent fixture={0};Ring ring={0};uint8_t *cold=NULL,*saved=NULL;
  size_t saved_size=0;int ok=1;
  negotiation=response;monitor_changed=option_update=0;
  REQUIRE(anygm_synthetic_tilemap_content_create(&fixture),"create gamepad threshold fixture");
  retro_set_environment(environment);retro_set_video_refresh(video);retro_init();
  struct retro_game_info info={fixture.path,NULL,0,NULL};
  REQUIRE(retro_load_game(&info),"load gamepad threshold fixture");
  ring.initial=ring.capacity=retro_serialize_size();
  REQUIRE(ring.capacity>0 && g_libretro.startup_ring_compact,"threshold cold compact policy");
  ring.bytes=malloc(ring.capacity);cold=malloc(ring.capacity);
  REQUIRE(ring.bytes && cold && push(&ring),"threshold ring before first frame");
  memcpy(cold,ring.bytes,ring.initial);retro_run();REQUIRE(push(&ring),"threshold first frame");
  GmlVM *vm=&g_libretro.engine->vm;
  for(int device=0;device<16;device++)
    gml_builtin_call(vm,"gamepad_set_button_threshold",(GmlVal[]){vreal(device),vreal(device/16.0)},2);
  REQUIRE(gamepad_thresholds_equal(vm,0) && push(&ring),"all thresholds fit cold ring");
  saved_size=ring.capacity;saved=malloc(saved_size);REQUIRE(saved,"retain threshold snapshot");
  memcpy(saved,ring.bytes,saved_size);
  for(int device=0;device<16;device++)
    gml_builtin_call(vm,"gamepad_set_button_threshold",(GmlVal[]){vreal(device),vreal(0.5)},2);
  REQUIRE(gamepad_thresholds_equal(vm,1) && push(&ring),"changed threshold push");
  REQUIRE(retro_unserialize(saved,saved_size) && gamepad_thresholds_equal(&g_libretro.engine->vm,0),
          "rewind restores all device thresholds");
  monitor_changed=option_update=1;retro_run();REQUIRE(push(&ring),"threshold ring after geometry");
  retro_reset();REQUIRE(gamepad_thresholds_equal(&g_libretro.engine->vm,1) && push(&ring),"Reset defaults thresholds");
  REQUIRE(retro_unserialize(saved,saved_size)==(response>0),"threshold Reset fixed/variable policy");
  retro_run();REQUIRE(push(&ring),"threshold first reset frame");
  REQUIRE(retro_unserialize(saved,saved_size) && gamepad_thresholds_equal(&g_libretro.engine->vm,0) && push(&ring),
          "threshold restore after Reset");
  REQUIRE(retro_unserialize(cold,ring.initial) && gamepad_thresholds_equal(&g_libretro.engine->vm,1) && push(&ring),
          "cold restore removes threshold edits");
  REQUIRE(ring.accepted==9 && ring.rejected==0,"every threshold frontend push accepted");
  printf("gamepad threshold frontend: negotiation=%d cold=%zu final=%zu accepted=%u rejected=%u\n",
         response,ring.initial,ring.capacity,ring.accepted,ring.rejected);
done:
  free(cold);free(saved);free(ring.bytes);
  retro_unload_game();retro_deinit();anygm_synthetic_content_destroy(&fixture);return ok;
}

int main(void){
  for(int response=-1;response<=1;response++) if(!run_case(response)) return 1;
  for(int response=-1;response<=1;response++) if(!run_ds_case(response)) return 1;
  for(int response=-1;response<=1;response++) if(!run_vertex_case(response)) return 1;
  for(int response=-1;response<=1;response++) if(!run_timeline_case(response)) return 1;
  for(int response=-1;response<=1;response++) if(!run_music_case(response)) return 1;
  for(int response=-1;response<=1;response++) if(!run_emitter_case(response)) return 1;
  for(int response=-1;response<=1;response++) if(!run_blend_case(response)) return 1;
  for(int response=-1;response<=1;response++) if(!run_gamepad_threshold_case(response)) return 1;
  return 0;
}
