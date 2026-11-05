/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Synthetic ownership, reset, and canonical-state coverage for builtin resources. */
#include "gml_builtin.h"
#include "gml_particle.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static GmlVal call(GmlVM *vm,const char *name,GmlVal *args,int count){
  return gml_builtin_call(vm,name,args,count);
}

static int expect_real(GmlVal value,double expected,const char *label){
  if(value.t==V_REAL && value.d==expected) return 1;
  fprintf(stderr,"%s mismatch: type=%d value=%g expected=%g\n",
          label,(int)value.t,value.d,expected);
  return 0;
}

/* Preserve a runtime grid and routed path when restoring state. The fixture
 * checks both the blocked cells and the ability to route again. */
static int populate_planning(GmlVM *vm,GmlVal *mp,GmlVal *route){
  GmlVal create[6]={vreal(0),vreal(0),vreal(8),vreal(4),vreal(16),vreal(16)};
  *mp=call(vm,"mp_grid_create",create,6);
  if(!expect_real(*mp,0,"mp_grid_create")) return 0;
  /* A wall down the middle with a gap on the bottom row, so the only route has to go around it
   * and the restored grid has to still say which cells are blocked. */
  GmlVal wall[5]={*mp,vreal(64),vreal(0),vreal(79),vreal(47)};
  (void)call(vm,"mp_grid_add_rectangle",wall,5);
  *route=call(vm,"path_add",NULL,0);
  GmlVal path_args[7]={*mp,*route,vreal(8),vreal(8),vreal(120),vreal(8),vreal(0)};
  if(!expect_real(call(vm,"mp_grid_path",path_args,7),1,"mp_grid_path")) return 0;
  GmlVal blocked[3]={*mp,vreal(4),vreal(0)};
  return expect_real(call(vm,"mp_grid_get_cell",blocked,3),-1,"blocked cell");
}

static int verify_planning(GmlVM *vm,GmlVal mp,GmlVal route,int points){
  GmlVal blocked[3]={mp,vreal(4),vreal(0)};
  GmlVal open[3]={mp,vreal(0),vreal(0)};
  /* Reading only: the canonical-state comparison below this needs the restore to have changed
   * nothing, so asking the grid to route again waits until those bytes have been compared. */
  return expect_real(call(vm,"mp_grid_get_cell",blocked,3),-1,
                     "restored blocked cell") &&
         expect_real(call(vm,"mp_grid_get_cell",open,3),0,
                     "restored open cell") &&
         expect_real(call(vm,"path_get_number",&route,1),points,
                     "restored path points");
}

/* Present is not the same as usable: a grid whose cells came back but whose extent or cell size
 * did not would still refuse every route. */
static int planning_still_routes(GmlVM *vm,GmlVal mp){
  GmlVal again=call(vm,"path_add",NULL,0);
  GmlVal path_args[7]={mp,again,vreal(8),vreal(40),vreal(120),vreal(40),vreal(0)};
  return expect_real(call(vm,"mp_grid_path",path_args,7),1,
                     "route found after restore");
}

static int populate_resources(GmlVM *vm,GmlVal *map,GmlVal *list,
                              GmlVal *grid,GmlVal *emitter,
                              GmlVal *fixture,GmlVal *joint,
                              GmlVal *timer,GmlVal *buffer){
  GmlVal ini_text=vstr("[state]\nname=synthetic\ncount=17\n");
  if(!expect_real(call(vm,"ini_open_from_string",&ini_text,1),1,
                  "ini_open_from_string")) return 0;

  *map=call(vm,"ds_map_create",NULL,0);
  GmlVal map_add[3]={*map,vstr("answer"),vreal(42)};
  (void)call(vm,"ds_map_add",map_add,3);

  *list=call(vm,"ds_list_create",NULL,0);
  GmlVal list_add[3]={*list,vreal(3),vreal(5)};
  (void)call(vm,"ds_list_add",list_add,3);

  GmlVal grid_create[2]={vreal(3),vreal(2)};
  *grid=call(vm,"ds_grid_create",grid_create,2);
  GmlVal grid_set[4]={*grid,vreal(2),vreal(1),vreal(91)};
  (void)call(vm,"ds_grid_set",grid_set,4);

  *emitter=call(vm,"audio_emitter_create",NULL,0);
  GmlVal emitter_gain[2]={*emitter,vreal(0.375)};
  GmlVal emitter_position[4]={*emitter,vreal(11),vreal(-7),vreal(2)};
  GmlVal emitter_falloff[4]={*emitter,vreal(4),vreal(400),vreal(1.25)};
  (void)call(vm,"audio_emitter_gain",emitter_gain,2);
  (void)call(vm,"audio_emitter_position",emitter_position,4);
  (void)call(vm,"audio_emitter_falloff",emitter_falloff,4);
  GmlVal listener_position[3]={vreal(2),vreal(3),vreal(4)};
  GmlVal listener_orientation[6]={
    vreal(0),vreal(0),vreal(-1),vreal(0),vreal(1),vreal(0)
  };
  GmlVal falloff_model=vreal(4);
  (void)call(vm,"audio_listener_position",listener_position,3);
  (void)call(vm,"audio_listener_orientation",listener_orientation,6);
  (void)call(vm,"audio_falloff_set_model",&falloff_model,1);

  *fixture=call(vm,"physics_fixture_create",NULL,0);
  GmlVal fixture_box[3]={*fixture,vreal(12.5),vreal(7.25)};
  GmlVal fixture_density[2]={*fixture,vreal(2.75)};
  GmlVal fixture_friction[2]={*fixture,vreal(0.625)};
  (void)call(vm,"physics_fixture_set_box_shape",fixture_box,3);
  (void)call(vm,"physics_fixture_set_density",fixture_density,2);
  (void)call(vm,"physics_fixture_set_friction",fixture_friction,2);
  GmlVal joint_args[8]={
    vreal(100001),vreal(100002),vreal(1),vreal(2),
    vreal(3),vreal(4),vreal(0.25),vreal(9.5)
  };
  *joint=call(vm,"physics_joint_revolute_create",joint_args,8);
  GmlVal gravity[2]={vreal(0.5),vreal(9.75)};
  GmlVal update_speed=vreal(60);
  GmlVal update_iterations=vreal(8);
  (void)call(vm,"physics_world_gravity",gravity,2);
  (void)call(vm,"physics_world_update_speed",&update_speed,1);
  (void)call(vm,"physics_world_update_iterations",&update_iterations,1);

  GmlVal timer_create[7]={
    vreal(1),vreal(37),vreal(0),vundef(),vundef(),vreal(4),vreal(1)
  };
  *timer=call(vm,"time_source_create",timer_create,7);
  (void)call(vm,"time_source_start",timer,1);
  (void)call(vm,"time_source_pause",timer,1);

  GmlVal buffer_size=vreal(32);
  *buffer=call(vm,"buffer_create",&buffer_size,1);
  GmlVal buffer_write[3]={*buffer,vreal(1),vreal(123)};
  (void)call(vm,"buffer_write",buffer_write,3);
  return 1;
}

static int verify_restored_resources(GmlVM *vm,GmlVal map,GmlVal list,
                                     GmlVal grid,GmlVal emitter,GmlVal timer,
                                     GmlVal buffer){
  int ok=1;
  GmlVal ini_read[3]={vstr("state"),vstr("name"),vstr("missing")};
  GmlVal ini_value=call(vm,"ini_read_string",ini_read,3);
  ok=ok && ini_value.t==V_STR && ini_value.s &&
     !strcmp(ini_value.s,"synthetic") &&
     gml_builtin_ini_entry_count(vm)==2;
  if(ini_value.t==V_STR && ini_value.d!=0) free((void *)ini_value.s);

  GmlVal map_find[2]={map,vstr("answer")};
  ok=ok && expect_real(call(vm,"ds_map_find_value",map_find,2),42,
                       "restored ds_map");
  GmlVal list_at[2]={list,vreal(1)};
  ok=ok && expect_real(call(vm,"ds_list_find_value",list_at,2),5,
                       "restored ds_list");
  GmlVal grid_at[3]={grid,vreal(2),vreal(1)};
  ok=ok && expect_real(call(vm,"ds_grid_get",grid_at,3),91,
                       "restored ds_grid");
  ok=ok && expect_real(call(vm,"audio_emitter_exists",&emitter,1),1,
                       "restored audio emitter");
  ok=ok && expect_real(call(vm,"audio_emitter_get_gain",&emitter,1),0.375,
                       "restored audio gain");
  ok=ok && expect_real(call(vm,"time_source_exists",&timer,1),1,
                       "restored time source");
  ok=ok && expect_real(call(vm,"time_source_get_state",&timer,1),2,
                       "restored time-source state");
  ok=ok && expect_real(call(vm,"buffer_exists",&buffer,1),1,
                       "restored buffer handle");
  GmlVal buffer_at[3]={buffer,vreal(0),vreal(1)};
  ok=ok && expect_real(call(vm,"buffer_peek",buffer_at,3),123,
                       "restored buffer bytes") &&
     expect_real(call(vm,"buffer_tell",&buffer,1),1,"restored buffer position");
  return ok;
}

static int many_grids_roundtrip(void){
  GmlWin win={0};
  GmlVM vm={0};
  vm.win=&win;
  vm.room_index=vm.pending_room=-1;
  vm.next_creation_seq=1;
  vm.particles=gml_particle_state_create(&vm);
  if(!vm.particles) return 0;
  gml_vm_software3d_reset(&vm);
  GmlVal grids[192];
  GmlVal make[]={vreal(3),vreal(1)};
  unsigned char *before=NULL,*after=NULL;
  int ok=0;
  for(int i=0;i<192;i++){
    grids[i]=call(&vm,"ds_grid_create",make,2);
    if(grids[i].t!=V_REAL || grids[i].d<=0){
      fprintf(stderr,"state grid allocation %d failed\n",i+1);
      goto done;
    }
    GmlVal set[]={grids[i],vreal(2),vreal(0),vreal(i+7)};
    call(&vm,"ds_grid_set",set,4);
  }
  /* Loading compacts holes; canonical bytes must not depend on physical slots. */
  for(int i=0;i<96;i+=2) call(&vm,"ds_grid_destroy",&grids[i],1);
  size_t size=gml_vm_state_size(&vm),written=0,used=0,repeated=0;
  before=malloc(size?size:1);
  after=malloc(size?size:1);
  if(!before || !after ||
     !gml_vm_state_save(&vm,before,size,&written) || written!=size){
    fprintf(stderr,"many-grid state save failed\n");
    goto done;
  }
  gml_builtin_state_reset(vm.builtins);
  if(!gml_vm_state_load(&vm,before,written,&used) || used!=written){
    fprintf(stderr,"many-grid state restore failed\n");
    goto done;
  }
  for(int i=0;i<192;i++){
    GmlVal get[]={grids[i],vreal(2),vreal(0)};
    double expected=i<96 && !(i%2)?0:i+7;
    if(!expect_real(call(&vm,"ds_grid_get",get,3),expected,"restored grid cell"))
      goto done;
  }
  if(!gml_vm_state_save(&vm,after,size,&repeated) || repeated!=written ||
     memcmp(before,after,written)){
    fprintf(stderr,"many-grid canonical bytes changed after compaction\n");
    goto done;
  }
  GmlVal replacement=call(&vm,"ds_grid_create",make,2);
  if(replacement.t!=V_REAL || replacement.d<=grids[191].d){
    fprintf(stderr,"restored grid identity cursor did not advance\n");
    goto done;
  }
  ok=1;
done:
  free(before);
  free(after);
  gml_vm_free(&vm);
  return ok;
}

static int boxed_object_state_roundtrip(void){
  GmlWin win={0};
  GmlVM source={0},restored={0};
  unsigned char *before=NULL,*after=NULL;
  int ok=!gml_vm_init(&source,&win,NULL) && !gml_vm_init(&restored,&win,NULL);
  GmlInstance *owner=ok?gml_struct_new(&source):NULL;
  if(!owner) ok=0;
  GmlVal reference=vundef(),grid_reference=vundef(),grid=vundef();
  if(ok){
    *gml_varmap_put(&owner->vars,"retained")=vreal(41);
    source.cur_self=owner;
    reference=call(&source,"@@This@@",NULL,0);
    source.cur_self=NULL;
    GmlVal roots=gml_arr_new(2,reference);
    gml_arr_set(roots,1,vreal(23));
    gml_arr_mark_escaped(roots);
    *gml_varmap_put(&source.globals,"roots")=roots;
    GmlInstance *grid_owner=gml_struct_new(&source);
    if(!grid_owner){ ok=0; goto boxed_state_done; }
    *gml_varmap_put(&grid_owner->vars,"retained")=vreal(72);
    grid_reference=vobject(grid_owner->id);
    GmlVal shape[]={vreal(1),vreal(1)};
    grid=call(&source,"ds_grid_create",shape,2);
    GmlVal clear[]={grid,vreal(-1)};
    call(&source,"ds_grid_clear",clear,2);
    GmlVal cell[]={grid,vreal(0),vreal(0),grid_reference};
    call(&source,"ds_grid_add",cell,4);
    ok=reference.t==V_OBJECT;
  }
  size_t size=ok?gml_vm_state_size(&source):0,written=0,used=0,repeated=0;
  if(ok){
    before=malloc(size);after=malloc(size);
    ok=size && before && after &&
      gml_vm_state_save(&source,before,size,&written) && written==size &&
      gml_vm_state_load(&restored,before,size,&used) && used==size;
  }
  if(ok){
    GmlVal *roots=gml_varmap_get(&restored.globals,"roots");
    GmlVal boxed=roots?gml_arr_get(*roots,0):vundef();
    GmlVal numeric=roots?gml_arr_get(*roots,1):vundef();
    GmlVal cell[]={grid,vreal(0),vreal(0)};
    GmlVal stored=call(&restored,"ds_grid_get",cell,3);
    GmlInstance *loaded=gml_struct_find(&restored,(unsigned)reference.d);
    GmlVal *value=loaded?gml_varmap_get(&loaded->vars,"retained"):NULL;
    ok=boxed.t==V_OBJECT && boxed.d==reference.d &&
       numeric.t==V_REAL && numeric.d==23 &&
       stored.t==V_OBJECT && stored.d==grid_reference.d &&
       value && value->t==V_REAL && value->d==41 &&
       gml_vm_state_save(&restored,after,size,&repeated) &&
       repeated==size && !memcmp(before,after,size);
    GmlInstance *grid_owner=gml_struct_find(&restored,(unsigned)grid_reference.d);
    GmlVal *grid_value=grid_owner?gml_varmap_get(&grid_owner->vars,"retained"):NULL;
    ok=ok && grid_value && grid_value->t==V_REAL && grid_value->d==72;
  }
boxed_state_done:
  if(!ok) fprintf(stderr,"boxed object roots or container state did not roundtrip\n");
  free(before);free(after);
  gml_vm_free(&source);gml_vm_free(&restored);
  return ok;
}

static int dense_struct_heap_roundtrip(void){
  enum { count=8192, capacity=1024*1024 };
  const char *stage="populate";
  GmlWin win={0};
  GmlVM source={0},restored={0};
  unsigned char *before=NULL,*after=NULL;
  int ok=!gml_vm_init(&source,&win,NULL) && !gml_vm_init(&restored,&win,NULL);
  GmlVal heap=gml_arr_new(count,vreal(0));
  gml_arr_mark_escaped(heap);
  *gml_varmap_put(&source.globals,"retained_heap")=heap;
  GmlVal shared=gml_arr_new(2,vreal(7));
  for(int i=0;i<count && ok;i++){
    GmlInstance *node=gml_struct_new(&source);
    if(!node){ ok=0; break; }
    *gml_varmap_put(&node->vars,"value")=vreal(i);
    gml_arr_set(heap,i,vreal(node->id));
    if(i<2) ok=gml_inst_var_set_val(&source,vreal(node->id),"shared",shared);
    if(i==2){
      node->x=-0.0;
      node->alarm[3]=17;
      node->path_speed=1.25;
      node->path_relative=1;
      node->timeline_position=2.5;
    }
  }
  /* Collection must keep the rooted graph and retain deterministic free-slot order. */
  for(int i=0;i<3 && ok;i++) ok=gml_struct_new(&source)!=NULL;
  size_t measured=ok?gml_vm_state_size(&source):0,written=0,used=0,repeated=0;
  if(!measured || measured>capacity){
    fprintf(stderr,"dictionary heap exceeds its fixed state slot: %zu > %d\n",
            measured,capacity);
    ok=0;
  }
  if(ok){
    stage="save/load/canonical bytes";
    before=malloc(capacity); after=malloc(capacity);
    ok=before && after &&
      gml_vm_state_save(&source,before,capacity,&written) && written==measured &&
      gml_vm_state_load(&restored,before,written,&used) && used==written &&
      gml_vm_state_save(&restored,after,capacity,&repeated) &&
      repeated==written && !memcmp(before,after,written);
    if(!ok && repeated==written && repeated){
      size_t first=0;
      while(first<written && before[first]==after[first]) first++;
      if(first<written)
        fprintf(stderr,"dictionary heap canonical difference at %zu: %u/%u\n",
                first,(unsigned)before[first],(unsigned)after[first]);
    }
  }
  GmlVal *restored_heap=ok?gml_varmap_get(&restored.globals,"retained_heap"):NULL;
  if(ok) stage="restored root";
  if(ok) ok=restored_heap && restored_heap->t==V_ARR &&
    gml_val_array_length(*restored_heap)==count;
  GmlArr *restored_shared=NULL;
  if(ok) stage="restored values, aliases and fields";
  for(int i=0;i<count && ok;i++){
    GmlVal id=gml_arr_get(*restored_heap,i);
    GmlInstance *node=gml_struct_find(&restored,(unsigned)id.d);
    GmlVal *value=node?gml_varmap_get(&node->vars,"value"):NULL;
    ok=value && value->t==V_REAL && value->d==i;
    if(i<2 && ok){
      GmlVal *alias=gml_varmap_get(&node->vars,"shared");
      ok=alias && alias->t==V_ARR && alias->arr && alias->arr!=shared.arr;
      if(i==0 && ok) restored_shared=alias->arr;
      else if(ok) ok=alias->arr==restored_shared;
    }
    if(i==2 && ok){
      double negative_zero=-0.0;
      ok=!memcmp(&node->x,&negative_zero,sizeof negative_zero) &&
        node->alarm[3]==17 && node->path_speed==1.25 &&
        node->path_relative==1 && node->timeline_position==2.5;
    }
  }
  if(ok){
    stage="old schema rejection";
    unsigned char schema[4];
    memcpy(schema,before+4,sizeof schema);
    uint32_t current=(uint32_t)schema[0]|((uint32_t)schema[1]<<8)|
                     ((uint32_t)schema[2]<<16)|((uint32_t)schema[3]<<24);
    uint32_t older=current-1;
    for(unsigned i=0;i<4;i++) before[4+i]=(unsigned char)(older>>(8*i));
    size_t rejected=0;
    void *root=restored_heap->arr;
    int refused=!gml_vm_state_load(&restored,before,written,&rejected);
    GmlVal *retained=gml_varmap_get(&restored.globals,"retained_heap");
    ok=refused && retained && retained->arr==root;
    memcpy(before+4,schema,sizeof schema);
  }
  if(ok){
    stage="allocator order";
    GmlInstance *a=gml_struct_new(&source),*b=gml_struct_new(&restored);
    ok=a && b && a->id==b->id;
  }
  if(!ok) fprintf(stderr,"dense dictionary heap failed at %s: measured=%zu written=%zu "
                 "used=%zu repeated=%zu\n",stage,measured,written,used,repeated);
  else printf("dense dictionary heap: %zu bytes in a %d-byte fixed slot\n",written,capacity);
  free(before); free(after);
  gml_vm_free(&source); gml_vm_free(&restored);
  return ok;
}

int main(void){
  if(!boxed_object_state_roundtrip()) return 1;
  if(!dense_struct_heap_roundtrip()) return 1;
  if(!many_grids_roundtrip()) return 1;
  GmlWin win={0};
  GmlVM vm={0};
  vm.win=&win;
  vm.room_index=-1;
  vm.pending_room=-1;
  vm.next_creation_seq=1;
  vm.particles=gml_particle_state_create(&vm);
  if(!vm.particles){
    fprintf(stderr,"builtin-state particle fixture allocation failed\n");
    return 1;
  }
  gml_vm_software3d_reset(&vm);

  GmlVal map=vundef(),list=vundef(),grid=vundef(),emitter=vundef();
  GmlVal fixture=vundef(),joint=vundef(),timer=vundef(),buffer=vundef();
  GmlVal mp=vundef(),route=vundef();
  if(!populate_resources(&vm,&map,&list,&grid,&emitter,
                         &fixture,&joint,&timer,&buffer) ||
     !populate_planning(&vm,&mp,&route)){
    gml_vm_free(&vm);
    return 1;
  }
  int route_points=(int)call(&vm,"path_get_number",&route,1).d;
  if(route_points<2){
    fprintf(stderr,"planning fixture produced no route (%d points)\n",route_points);
    gml_vm_free(&vm);
    return 1;
  }

  size_t size=gml_vm_state_size(&vm),written=0,used=0;
  unsigned char *before=malloc(size?size:1);
  unsigned char *after=malloc(size?size:1);
  if(!before || !after ||
     !gml_vm_state_save(&vm,before,size,&written) || written!=size){
    fprintf(stderr,"builtin-state canonical save failed\n");
    free(before);
    free(after);
    gml_vm_free(&vm);
    return 1;
  }

  gml_builtin_state_reset(vm.builtins);
  GmlVal zero_type=vreal(0);
  int reset_ok=gml_builtin_ini_entry_count(&vm)==0 &&
    expect_real(call(&vm,"ds_exists",(GmlVal[]){map,zero_type},2),0,
                "reset ds_map") &&
    expect_real(call(&vm,"audio_emitter_exists",&emitter,1),0,
                "reset audio emitter") &&
    expect_real(call(&vm,"time_source_exists",&timer,1),0,
                "reset time source") &&
    expect_real(call(&vm,"buffer_exists",&buffer,1),0,
                "reset buffer");
  /* A destroyed grid answers 0 for every cell, which is also what a free cell answers - so the
   * coordinate asked about here is the one the wall blocked, where -1 and 0 do separate them. */
  reset_ok=reset_ok &&
    expect_real(call(&vm,"mp_grid_get_cell",(GmlVal[]){mp,vreal(4),vreal(0)},3),0,
                "reset motion-planning grid");
  if(!reset_ok ||
     !gml_vm_state_load(&vm,before,written,&used) || used!=written ||
     !verify_restored_resources(&vm,map,list,grid,emitter,timer,buffer) ||
     !verify_planning(&vm,mp,route,route_points)){
    fprintf(stderr,"builtin-state reset/restore fixture failed\n");
    free(before);
    free(after);
    gml_vm_free(&vm);
    return 1;
  }

  size_t repeated=0;
  if(!gml_vm_state_save(&vm,after,size,&repeated) ||
     repeated!=written || memcmp(before,after,written)){
    size_t first=0;
    while(first<written && first<repeated && before[first]==after[first]) first++;
    fprintf(stderr,
            "builtin resource sections changed canonical state bytes: "
            "before=%zu after=%zu first=%zu values=%u/%u\n",
            written,repeated,first,
            first<written?(unsigned)before[first]:0,
            first<repeated?(unsigned)after[first]:0);
    free(before);
    free(after);
    gml_vm_free(&vm);
    return 1;
  }

  if(!planning_still_routes(&vm,mp)){
    free(before);
    free(after);
    gml_vm_free(&vm);
    return 1;
  }

  free(before);
  free(after);
  gml_vm_free(&vm);
  puts("builtin resource ownership and canonical-state fixtures: ok");
  return 0;
}
