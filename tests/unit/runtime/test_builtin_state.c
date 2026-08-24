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
  ok=ok && expect_real(call(vm,"buffer_exists",&buffer,1),0,
                       "transient buffer after restore");
  return ok;
}

int main(void){
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
