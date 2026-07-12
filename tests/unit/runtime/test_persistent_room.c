/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me> */
#include "gml_vm.h"
#include "gml_render.h"
#include "gmlc/gmlc_package.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int gml_input_key(int key,int edge){ (void)key;(void)edge;return 0; }
int gml_input_gamepad(int button,int edge){ (void)button;(void)edge;return 0; }
GmlVal gml_builtin_call(GmlVM *vm,const char *name,GmlVal *args,int count);

static GmlInstance *find_slot(GmlVM *vm,uint32_t id){
  for(int i=0;i<vm->inst_count;i++) if(vm->inst[i].id==id) return &vm->inst[i];
  return NULL;
}


static double global_array_value(GmlVM *vm,const char *name,int index){
  GmlVal *value=gml_varmap_get(&vm->globals,name);
  GmlArr *array=value&&value->t==V_ARR?(GmlArr*)value->arr:NULL;
  return array&&index>=0&&index<array->len&&array->data[index].t==V_REAL?array->data[index].d:0;
}

int main(void){
  GmlcProject project; GmlcObject objects[2]; GmlcRoom rooms[2];
  int room_order[2]={0,1};
  GmlcObjectEvent object_events[3];
  GmlcProjectTrigger trigger;
  GmlcProjectIncludedFile included;
  unsigned char included_data[]={1,3,5,7};
  char included_name[64];
  GmlcProjectConstant constant={(char*)"fixture_constant",(char*)"6*7"};
  memset(&project,0,sizeof(project)); memset(objects,0,sizeof(objects)); memset(rooms,0,sizeof(rooms));
  memset(object_events,0,sizeof(object_events)); memset(&trigger,0,sizeof(trigger)); memset(&included,0,sizeof(included));
  project.name="persistent-room-fixture"; project.objects=objects; project.n_objects=2;
  project.classic_version=800;
  project.constants=&constant; project.n_constants=project.cap_constants=1;
  project.triggers=&trigger; project.n_triggers=project.cap_triggers=1;
  snprintf(included_name,sizeof(included_name),"gml-included-%ld.dat",(long)getpid());
  included.file_name=included_name; included.data=included_data; included.data_size=sizeof(included_data);
  included.export_mode=2; included.overwrite_file=1;
  project.included_files=&included; project.n_included_files=project.cap_included_files=1;
  project.rooms=rooms; project.n_rooms=2; project.room_order=room_order; project.n_room_order=2;
  objects[0].name="obj_fixture"; objects[0].sprite_id=-1; objects[0].mask_id=-1; objects[0].parent_id=-1; objects[0].visible=1;
  objects[0].events=object_events; objects[0].n_events=objects[0].cap_events=2;
  objects[1].name="obj_changed"; objects[1].sprite_id=-1; objects[1].mask_id=-1; objects[1].parent_id=-1; objects[1].visible=1;
  objects[1].events=&object_events[2]; objects[1].n_events=objects[1].cap_events=1;
  object_events[0].event_type=11; object_events[0].event_number=0;
  object_events[1].event_type=3; object_events[1].event_number=0;
  object_events[2].event_type=3; object_events[2].event_number=2;
  trigger.name=(char*)"fixture_trigger"; trigger.moment=1; trigger.runtime_id=0;
  for(int i=0;i<2;i++){ rooms[i].name=i?"room_b":"room_a"; rooms[i].width=320; rooms[i].height=240; rooms[i].speed=30; }
  rooms[0].persistent=1;
  char startup[]="/tmp/gml-startup-XXXXXX"; int startup_fd=mkstemp(startup); if(startup_fd<0)return 1;
  FILE *startup_file=fdopen(startup_fd,"wb");
  const char startup_source[]="global.startup_value = fixture_constant;\n";
  if(!startup_file || fwrite(startup_source,1,sizeof(startup_source)-1,startup_file)!=sizeof(startup_source)-1 ||
     fclose(startup_file)!=0){ unlink(startup); return 1; }
  project.startup_code_path=startup;
  char condition[]="/tmp/gml-trigger-condition-XXXXXX"; int condition_fd=mkstemp(condition); if(condition_fd<0)return 1;
  FILE *condition_file=fdopen(condition_fd,"wb"); const char condition_source[]="return (global.startup_value == 42);\n";
  if(!condition_file || fwrite(condition_source,1,sizeof(condition_source)-1,condition_file)!=sizeof(condition_source)-1 || fclose(condition_file)!=0)return 1;
  trigger.condition_path=condition;
  char event[]="/tmp/gml-trigger-event-XXXXXX"; int event_fd=mkstemp(event); if(event_fd<0)return 1;
  FILE *event_file=fdopen(event_fd,"wb"); const char event_source[]="global.trigger_hits += 1;\n";
  if(!event_file || fwrite(event_source,1,sizeof(event_source)-1,event_file)!=sizeof(event_source)-1 || fclose(event_file)!=0)return 1;
  object_events[0].source_path=event;
  char step[]="/tmp/gml-step-event-XXXXXX"; int step_fd=mkstemp(step); if(step_fd<0)return 1;
  FILE *step_file=fdopen(step_fd,"wb");
  const char step_source[]=
    "if (!global.spawned_once) { global.spawned_once=1; "
    "with(instance_create(50,50,obj_changed)){ hspeed=3; } } "
    "if (hspeed > 0) hspeed -= 1; if (hspeed < 0) hspeed += 1; "
    "hspeed = round(hspeed); x += 5;\n";
  if(!step_file || fwrite(step_source,1,sizeof(step_source)-1,step_file)!=sizeof(step_source)-1 || fclose(step_file)!=0)return 1;
  object_events[1].source_path=step;
  char end_step[]="/tmp/gml-end-step-event-XXXXXX"; int end_step_fd=mkstemp(end_step); if(end_step_fd<0)return 1;
  FILE *end_step_file=fdopen(end_step_fd,"wb"); const char end_step_source[]="x += 7;\n";
  if(!end_step_file || fwrite(end_step_source,1,sizeof(end_step_source)-1,end_step_file)!=sizeof(end_step_source)-1 || fclose(end_step_file)!=0)return 1;
  object_events[2].source_path=end_step;
  char path[]="/tmp/gml-persistent-room-XXXXXX"; int fd=mkstemp(path); if(fd<0)return 1; close(fd);
  char err[256]={0};
  if(!gmlc_package_write_structural(&project,path,err,sizeof(err))){ fprintf(stderr,"package: %s\n",err); unlink(path); unlink(startup); return 1; }
  char included_path[96]; snprintf(included_path,sizeof(included_path),"/tmp/%s",included_name);
  FILE *included_file=fopen(included_path,"rb"); unsigned char observed[sizeof(included_data)]={0};
  int included_ok=included_file && fread(observed,1,sizeof(observed),included_file)==sizeof(observed) &&
                  !memcmp(observed,included_data,sizeof(observed));
  if(included_file) fclose(included_file);
  if(!included_ok){ fprintf(stderr,"included file was not exported\n"); return 1; }
  GmlWin win; if(gml_win_load(&win,path)){ unlink(path); return 1; }
  GmlVM vm; if(gml_vm_init(&vm,&win)){ gml_win_free(&win); unlink(path); return 1; }
  GmlVal *startup_value=gml_varmap_get(&vm.globals,"startup_value");
  if(!startup_value || startup_value->t!=V_REAL || startup_value->d!=42){
    fprintf(stderr,"startup code or project constant did not run\n"); return 1;
  }
  gml_room_enter(&vm,0);
  GmlInstance *created=gml_instance_create(&vm,12,34,0); if(!created)return 1;
  vm.cur_self=created;
  GmlVal change_args[2]={vreal(1),vreal(0)};
  (void)gml_builtin_call(&vm,"action_change_object",change_args,2);
  if(created->obj!=1){ fprintf(stderr,"classic change-object action did not replace the object\n"); return 1; }
  change_args[0]=vreal(0);
  (void)gml_builtin_call(&vm,"action_change_object",change_args,2);
  vm.cur_self=created;
  GmlVal object_args[4]={vreal(1),vreal(32),vreal(0),vreal(1)};
  GmlVal object_hit=gml_builtin_call(&vm,"action_if_object",object_args,4);
  GmlVal previous_room=gml_builtin_call(&vm,"action_if_previous_room",NULL,0);
  if(object_hit.t!=V_REAL || object_hit.d!=0 || previous_room.t!=V_REAL || previous_room.d!=0){
    fprintf(stderr,"classic object/previous-room conditions did not match: object=%.0f previous=%.0f\n",
      object_hit.d,previous_room.d); return 1;
  }
  created->path_index=0;
  GmlVal path_speed=vreal(0.75);
  (void)gml_builtin_call(&vm,"action_path_speed",&path_speed,1);
  (void)gml_builtin_call(&vm,"action_path_end",NULL,0);
  GmlVal timeline_args[4]={vreal(1),vreal(3),vreal(1),vreal(0)};
  (void)gml_builtin_call(&vm,"action_timeline_set",timeline_args,4);
  GmlVal fullscreen=vreal(2);
  (void)gml_builtin_call(&vm,"action_fullscreen",&fullscreen,1);
  if(created->path_index!=-1 || created->path_speed!=0.75 ||
     created->timeline_index!=1 || created->timeline_position!=3 ||
     created->timeline_running!=1 || created->timeline_loop!=0 || vm.window_fullscreen!=1){
    fprintf(stderr,"classic path/timeline/fullscreen actions did not update state\n"); return 1;
  }
  GmlVal gravity_args[2]={vreal(270),vreal(1)};
  (void)gml_builtin_call(&vm,"action_set_gravity",gravity_args,2);
  vm.action_relative=1; gravity_args[0]=vreal(10); gravity_args[1]=vreal(0.5);
  (void)gml_builtin_call(&vm,"action_set_gravity",gravity_args,2);
  vm.action_relative=0;
  if(created->gravity_direction!=280 || created->gravity!=1.5){
    fprintf(stderr,"classic gravity action argument order mismatch: direction=%.0f gravity=%.1f\n",
      created->gravity_direction,created->gravity); return 1;
  }
  created->gravity=0;
  GmlVal potential_args[4]={vreal(22),vreal(34),vreal(2),vreal(0)};
  (void)gml_builtin_call(&vm,"action_potential_step",potential_args,4);
  if(created->x!=14 || created->y!=34){
    fprintf(stderr,"classic potential-step action did not move toward its target: (%.0f,%.0f)\n",
      created->x,created->y); return 1;
  }
  gml_rng_seed(&vm,4);
  GmlVal move_args[2]={vstr("101101101"),vreal(5)};
  (void)gml_builtin_call(&vm,"action_move",move_args,2);
  if(created->direction!=180 || vm.rng_classic_state!=1503470698u){
    fprintf(stderr,"classic direction-pad rejection mismatch: direction=%.0f seed=%u\n",
      created->direction,vm.rng_classic_state); return 1;
  }
  created->direction=created->speed=created->hspeed=created->vspeed=0;
  GmlVal distance_arg=vreal(0);
  GmlVal object_distance=gml_builtin_call(&vm,"distance_to_object",&distance_arg,1);
  distance_arg=vreal((double)created->id);
  GmlVal self_distance=gml_builtin_call(&vm,"distance_to_object",&distance_arg,1);
  vm.cur_self=NULL;
  if(object_distance.t!=V_REAL || object_distance.d!=1000000 ||
     self_distance.t!=V_REAL || self_distance.d!=1000000){
    fprintf(stderr,"distance_to_object missing/self sentinel mismatch: object=%.0f self=%.0f\n",
      object_distance.d,self_distance.d); return 1;
  }
  GmlRender collision_render; GmlSprite collision_sprites[1];
  memset(&collision_render,0,sizeof(collision_render));
  memset(collision_sprites,0,sizeof(collision_sprites));
  collision_render.n_spr=1; collision_render.spr=collision_sprites;
  collision_sprites[0].w=2; collision_sprites[0].h=2;
  collision_sprites[0].ml=collision_sprites[0].mt=0;
  collision_sprites[0].mr=collision_sprites[0].mb=1;
  collision_sprites[0].collision_kind=1;
  vm.render=&collision_render;
  created->x=0; created->y=100; created->sprite_index=created->mask_index=0;
  created->image_xscale=created->image_yscale=1;
  GmlInstance *contact=gml_instance_create(&vm,5,100,1); if(!contact)return 1;
  contact->sprite_index=contact->mask_index=0;
  contact->image_xscale=contact->image_yscale=1;
  contact->solid=0;
  vm.cur_self=created;
  GmlVal contact_direction=vreal(0);
  (void)gml_builtin_call(&vm,"move_contact",&contact_direction,1);
  if(created->x!=3){
    fprintf(stderr,"move_contact omitted-distance/any-instance mismatch: x=%.0f\n",created->x); return 1;
  }
  created->x=0;
  GmlVal contact_solid_args[2]={vreal(0),vreal(5)};
  (void)gml_builtin_call(&vm,"move_contact_solid",contact_solid_args,2);
  if(created->x!=5){
    fprintf(stderr,"move_contact_solid incorrectly stopped at a non-solid: x=%.0f\n",created->x); return 1;
  }
  created->x=0; contact->solid=1;
  (void)gml_builtin_call(&vm,"move_contact_solid",contact_solid_args,2);
  if(created->x!=3){
    fprintf(stderr,"move_contact_solid did not stop before a solid: x=%.0f\n",created->x); return 1;
  }
  GmlVal potential_settings[4]={vreal(30),vreal(10),vreal(3),vreal(1)};
  (void)gml_builtin_call(&vm,"mp_potential_settings",potential_settings,4);
  created->x=0; created->y=100; created->direction=0; contact->x=5; contact->y=100;
  GmlVal blocked_goal[4]={vreal(5),vreal(100),vreal(10),vreal(0)};
  GmlVal reached=gml_builtin_call(&vm,"mp_potential_step",blocked_goal,4);
  if(reached.t!=V_REAL || reached.d!=0 || created->x!=0 || created->y!=100){
    fprintf(stderr,"potential-step moved into a blocked nearby goal: reached=%.0f pos=(%.1f,%.1f)\n",
      reached.d,created->x,created->y); return 1;
  }
  GmlVal ahead_goal[4]={vreal(20),vreal(100),vreal(2),vreal(0)};
  (void)gml_builtin_call(&vm,"mp_potential_step",ahead_goal,4);
  if(created->x==2 && created->y==100){
    fprintf(stderr,"potential-step ignored its forward obstacle probe\n"); return 1;
  }
  potential_settings[2]=vreal(1);
  (void)gml_builtin_call(&vm,"mp_potential_settings",potential_settings,4);
  created->x=0; created->y=100; created->direction=0;
  (void)gml_builtin_call(&vm,"mp_potential_step",ahead_goal,4);
  if(created->x!=2 || created->y!=100){
    fprintf(stderr,"potential-step settings did not update check distance: (%.1f,%.1f)\n",
      created->x,created->y); return 1;
  }
  contact->x=100; created->x=10; created->y=100; vm.action_relative=1;
  GmlVal relative_goal[4]={vreal(2),vreal(0),vreal(2),vreal(0)};
  reached=gml_builtin_call(&vm,"action_potential_step",relative_goal,4);
  vm.action_relative=0;
  if(reached.t!=V_REAL || reached.d!=1 || created->x!=12 || created->y!=100){
    fprintf(stderr,"relative potential-step target mismatch: reached=%.0f pos=(%.1f,%.1f)\n",
      reached.d,created->x,created->y); return 1;
  }
  *gml_varmap_put(&created->vars,"side")=vreal(180);
  GmlVal local_name=vstr("side");
  vm.cur_self=created;
  GmlVal local_exists=gml_builtin_call(&vm,"variable_local_exists",&local_name,1);
  if(local_exists.t!=V_REAL || local_exists.d!=1){
    fprintf(stderr,"classic instance-local field was reported missing\n"); return 1;
  }
  GmlVal alarm_args[2]={vreal(1.6),vreal(0)};
  (void)gml_builtin_call(&vm,"action_set_alarm",alarm_args,2);
  vm.cur_self=NULL;
  if(created->alarm[0]!=2){
    fprintf(stderr,"classic fractional alarm was not rounded: %.3f\n",created->alarm[0]); return 1;
  }
  GmlVal message_args[4]={vstr("prompt"),vstr(""),vstr("accept"),vstr("cancel")};
  GmlVal message_result=gml_builtin_call(&vm,"show_message_ext",message_args,4);
  if(message_result.t!=V_REAL || message_result.d!=2){
    fprintf(stderr,"non-interactive message button selection mismatch: %.0f\n",message_result.d); return 1;
  }
  GmlVal sleep_args[2]={vreal(1000),vreal(1)};
  (void)gml_builtin_call(&vm,"action_sleep",sleep_args,2);
  (void)gml_builtin_call(&vm,"sleep",sleep_args,1);
  (void)gml_builtin_call(&vm,"action_restart_game",NULL,0);
  if(vm.game_end!=2){ fprintf(stderr,"classic restart action did not request a cold boot\n"); return 1; }
  vm.game_end=0;
  (void)gml_builtin_call(&vm,"action_end_game",NULL,0);
  if(vm.game_end!=1){ fprintf(stderr,"classic end action did not request shutdown\n"); return 1; }
  vm.game_end=0;
  created->gravity=0; created->gravity_direction=270;
  created->hspeed=-1e-16; created->vspeed=0;
  double position_before_step=created->x;
  gml_vm_step(&vm);
  if(created->x!=position_before_step+5 || created->xprevious!=position_before_step || created->hspeed!=0){
    fprintf(stderr,"previous position/cardinal gravity mismatch: x=%.0f previous=%.0f hspeed=%.17g\n",
      created->x,created->xprevious,created->hspeed); return 1;
  }
  GmlInstance *same_step_spawn=NULL;
  for(int i=0;i<vm.inst_count;i++) if(vm.inst[i].active && !vm.inst[i].marked &&
      vm.inst[i].obj==1 && vm.inst[i].x>=50 && vm.inst[i].x<70){ same_step_spawn=&vm.inst[i]; break; }
  if(!same_step_spawn || same_step_spawn->x!=60){
    fprintf(stderr,"classic same-step movement/end-step mismatch: x=%.0f\n",
      same_step_spawn?same_step_spawn->x:-1.0); return 1;
  }
  created->gravity=created->vspeed=0;
  GmlVal *trigger_hits=gml_varmap_get(&vm.globals,"trigger_hits");
  if(!trigger_hits || trigger_hits->t!=V_REAL || trigger_hits->d!=1){ fprintf(stderr,"classic trigger did not fire\n"); return 1; }
  created->x=200.6; created->y=100.4;
  created->hspeed=created->vspeed=created->gravity=0;
  gml_set_global_arr(&vm,"view_visible",0,0);
  gml_set_global_arr(&vm,"view_visible",1,1);
  gml_set_global_arr(&vm,"view_xview",1,0); gml_set_global_arr(&vm,"view_yview",1,0);
  gml_set_global_arr(&vm,"view_wview",1,100); gml_set_global_arr(&vm,"view_hview",1,100);
  gml_set_global_arr(&vm,"view_hborder",1,10); gml_set_global_arr(&vm,"view_vborder",1,50);
  gml_set_global_arr(&vm,"view_hspeed",1,4); gml_set_global_arr(&vm,"view_vspeed",1,-1);
  gml_set_global_arr(&vm,"view_object",1,(double)created->id);
  gml_vm_step(&vm);
  if(global_array_value(&vm,"view_xview",1)!=4 || global_array_value(&vm,"view_yview",1)!=50){
    fprintf(stderr,"classic secondary-view limited follow mismatch: x=%.0f y=%.0f\n",
      global_array_value(&vm,"view_xview",1),global_array_value(&vm,"view_yview",1)); return 1;
  }
  gml_set_global_arr(&vm,"view_xview",1,0);
  gml_set_global_arr(&vm,"view_hspeed",1,-1);
  gml_vm_step(&vm);
  if(global_array_value(&vm,"view_xview",1)!=121){
    fprintf(stderr,"classic secondary-view snap/round mismatch: x=%.0f\n",
      global_array_value(&vm,"view_xview",1)); return 1;
  }
  gml_set_global_arr(&vm,"background_x",0,123);
  gml_set_global_arr(&vm,"view_xview",0,77);
  *gml_varmap_put(&vm.globals,"room_speed")=vreal(55);
  gml_tile_layer_shift(&vm,300,8,9);
  uint32_t id=created->id; *gml_varmap_put(&created->vars,"value")=vreal(42);
  gml_room_enter(&vm,1);
  GmlInstance *slot=find_slot(&vm,id);
  if(!slot||!slot->room_dormant||slot->active){ fprintf(stderr,"room was not stored\n"); return 1; }
  size_t size=gml_vm_state_size(&vm),written=0,used=0; void *state=malloc(size);
  if(!state||!gml_vm_state_save(&vm,state,size,&written)||written!=size)return 1;
  gml_room_enter(&vm,0); slot=find_slot(&vm,id);
  if(!slot||!slot->active||slot->room_dormant)return 1;
  *gml_varmap_put(&slot->vars,"value")=vreal(99);
  if(!gml_vm_state_load(&vm,state,written,&used)||used!=written)return 1;
  gml_room_enter(&vm,0); slot=find_slot(&vm,id);
  GmlVal *value=slot?gml_varmap_get(&slot->vars,"value"):NULL;
  GmlVal *room_speed=gml_varmap_get(&vm.globals,"room_speed");
  int ok=slot&&slot->active&&!slot->room_dormant&&value&&value->t==V_REAL&&value->d==42 &&
    global_array_value(&vm,"background_x",0)==123 && global_array_value(&vm,"view_xview",0)==77 &&
    room_speed&&room_speed->t==V_REAL&&room_speed->d==55 && vm.n_tile_mut==1 &&
    vm.tile_mut[0].depth==300&&vm.tile_mut[0].dx==8&&vm.tile_mut[0].dy==9 &&
    vm.potential_max_rotation==30 && vm.potential_rotate_step==10 &&
    vm.potential_check_distance==1 && vm.potential_rotate_on_spot==1;
  if(!ok) fprintf(stderr,
    "persistent room state did not roundtrip: slot=%d value=%.0f bg=%.0f view=%.0f speed=%.0f tiles=%d depth=%d shift=(%.0f,%.0f)\n",
    slot&&slot->active&&!slot->room_dormant,value&&value->t==V_REAL?value->d:-1,
    global_array_value(&vm,"background_x",0),global_array_value(&vm,"view_xview",0),
    room_speed&&room_speed->t==V_REAL?room_speed->d:-1,vm.n_tile_mut,
    vm.n_tile_mut?vm.tile_mut[0].depth:-1,vm.n_tile_mut?vm.tile_mut[0].dx:0,vm.n_tile_mut?vm.tile_mut[0].dy:0);
  free(state); gml_vm_free(&vm); gml_win_free(&win); unlink(path); unlink(startup); unlink(condition); unlink(event); unlink(step); unlink(end_step); unlink(included_path);
  if(ok) puts("persistent room fixtures: ok");
  return ok?0:1;
}
