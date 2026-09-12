/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "persistent_test_fixture.h"

#include "gml_builtin.h"
#include "gml_vm_internal.h"
#include "gml_render.h"
#include "gml_render_internal.h"
#include "gml_audio.h"
#include "gmlc_package.h"
#include "anygm_compatibility.h"
#include "stdio_vfs.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct FixtureHostClock {
  uint64_t next_ns;
  uint64_t stride_ns;
  unsigned calls;
} FixtureHostClock;

static uint64_t fixture_host_clock_read(void *userdata){
  FixtureHostClock *clock=userdata;
  uint64_t value=clock->next_ns;
  clock->next_ns+=clock->stride_ns;
  clock->calls++;
  return value;
}

static int sample_frame_clock(GmlVM *vm,double values[3]){
  GmlVal first=gml_vm_identifier_get(vm,"current_time");
  values[0]=first.t==V_REAL?first.d:-1.0;
  values[1]=gml_vm_get_timer_us(vm);
  GmlVal second=gml_vm_identifier_get(vm,"current_time");
  values[2]=second.t==V_REAL?second.d:-1.0;
  return first.t==V_REAL && second.t==V_REAL;
}

int expect_frame_clock_ignores_host_time(void){
  GmlWin win={0};
  win.game_speed=60;
  FixtureHostClock first_clock={UINT64_C(7000000000),UINT64_C(13000000),0};
  FixtureHostClock second_clock={UINT64_C(900000000000),UINT64_C(3000000000),0};
  AnygmHostServices first_host={0},second_host={0};
  first_host.struct_size=sizeof first_host;
  first_host.abi_version=ANYGM_HOST_SERVICES_VERSION;
  first_host.userdata=&first_clock;
  first_host.monotonic_time_ns=fixture_host_clock_read;
  second_host.struct_size=sizeof second_host;
  second_host.abi_version=ANYGM_HOST_SERVICES_VERSION;
  second_host.userdata=&second_clock;
  second_host.monotonic_time_ns=fixture_host_clock_read;
  GmlVM first={0},second={0};
  first.win=second.win=&win;
  first.host=&first_host;
  second.host=&second_host;
  first.frame=second.frame=90;
  double first_values[3]={0},second_values[3]={0};
  int sampled=sample_frame_clock(&first,first_values) &&
              sample_frame_clock(&second,second_values);
  int ok=sampled && first_clock.calls==0 && second_clock.calls==0 &&
    first_values[0]==1500.0 && first_values[1]==1501000.0 && first_values[2]==1502.0 &&
    memcmp(first_values,second_values,sizeof first_values)==0;
  if(!ok)
    fprintf(stderr,
      "frame clock depended on host time: first=(%.0f,%.0f,%.0f calls=%u)"
      " second=(%.0f,%.0f,%.0f calls=%u)\n",
      first_values[0],first_values[1],first_values[2],first_clock.calls,
      second_values[0],second_values[1],second_values[2],second_clock.calls);
  return ok;
}


int expect_classic_timeline_index_activation(void){
  GmlWin win={0}; GmlVM vm={0}; GmlInstance instance={0};
  win.bytecode=15; win.classic_version=530; vm.win=&win;
  instance.timeline_index=-1; instance.timeline_position=4; instance.timeline_speed=1;
  if(!gml_vm_instance_builtin_set(&vm,&instance,"timeline_index",vreal(2)) ||
     instance.timeline_index!=2 || instance.timeline_position!=4 || instance.timeline_running!=1){
    fprintf(stderr,"classic timeline assignment did not activate playback: index=%.0f position=%.0f running=%.0f\n",
            instance.timeline_index,instance.timeline_position,instance.timeline_running);
    return 0;
  }
  (void)gml_vm_instance_builtin_set(&vm,&instance,"timeline_index",vreal(-1));
  if(instance.timeline_index!=-1 || instance.timeline_running!=0){
    fprintf(stderr,"classic timeline removal did not stop playback: index=%.0f running=%.0f\n",
            instance.timeline_index,instance.timeline_running);
    return 0;
  }
  win.classic_version=0; instance.timeline_running=0;
  (void)gml_vm_instance_builtin_set(&vm,&instance,"timeline_index",vreal(3));
  if(instance.timeline_index!=3 || instance.timeline_running!=0){
    fprintf(stderr,"modern timeline assignment changed explicit playback state: index=%.0f running=%.0f\n",
            instance.timeline_index,instance.timeline_running);
    return 0;
  }
  return 1;
}
#include <sys/stat.h>
#include <unistd.h>

int expect_early_native_layer_animation(void){
  uint8_t data[512]={0};
  fixture_w32(data,0,1);
  fixture_w32(data,4,16);
  fixture_w32(data,16+88,180);
  fixture_w32(data,180,1);
  fixture_w32(data,184,200);
  fixture_w32(data,200,400);
  fixture_w32(data,208,1);
  fixture_w32(data,232,1);
  fixture_w32(data,236,1);
  fixture_w32(data,268,8);
  memcpy(data+400,"fixture_layer",14);

  GmlWin win={0};
  win.data=data;
  win.size=sizeof data;
  win.bytecode=15;
  win.game_speed=60;
  win.n_chunks=1;
  memcpy(win.chunks[0].name,"ROOM",5);
  win.chunks[0].off=0;
  win.chunks[0].size=sizeof data;

  uint32_t detected_layers=0;
  if(gml_room_layer_list(&win,0,&detected_layers)!=180 || detected_layers!=1){
    fputs("early native ROOM layer list was not detected structurally\n",stderr);
    return 0;
  }

  GmlRtLayer layer={0};
  layer.id=1001;
  layer.used=1;
  memcpy(layer.name,"fixture_layer",14);
  GmlRtElem element={0};
  element.used=1;
  element.type=1;
  element.layer=layer.id;
  element.image_speed=8;

  GmlVM vm={0};
  vm.win=&win;
  vm.room_index=0;
  vm.rtl=&layer;
  vm.n_rtl=1;
  vm.rte=&element;
  vm.n_rte=1;
  gml_vm_frame_advance_layers(&vm);
  if(fabs(element.image_index-(8.0/60.0))>1e-12){
    fprintf(stderr,"early native layer background did not animate: %.2f\n",
            element.image_index);
    return 0;
  }

  fixture_w32(data,208,2);
  element.image_index=0;
  gml_vm_frame_advance_layers(&vm);
  if(element.image_index!=0){
    fprintf(stderr,"early compatibility background used native layer cadence: %.2f\n",
            element.image_index);
    return 0;
  }
  return 1;
}

int expect_authored_long_layer_background_binding(void){
  uint8_t data[512]={0};
  static const char layer_name[]="neutral_background_layer_name_longer_than_cache";
  fixture_w32(data,0,1);
  fixture_w32(data,4,16);
  fixture_w32(data,16+88,180);
  fixture_w32(data,180,1);
  fixture_w32(data,184,200);
  fixture_w32(data,200,400);
  fixture_w32(data,208,1);
  fixture_w32(data,212,7);
  fixture_w32(data,232,1);
  fixture_w32(data,236,1);
  fixture_w32(data,244,0);
  fixture_w32(data,260,0xFFFFFFFFu);
  memcpy(data+400,layer_name,sizeof layer_name);

  GmlWin win={0};
  win.data=data;
  win.size=sizeof data;
  win.bytecode=17;
  win.game_speed=60;
  win.n_chunks=1;
  memcpy(win.chunks[0].name,"ROOM",5);
  win.chunks[0].off=0;
  win.chunks[0].size=sizeof data;

  GmlVM vm={0};
  vm.win=&win;
  vm.room_index=0;
  gml_vm_room_reload_layers_mode(&vm,0,1);
  GmlVal layer=vreal(vm.n_rtl==1?vm.rtl[0].id:-1);
  GmlVal elements=gml_builtin_call(&vm,"layer_get_all_elements",&layer,1);
  int element_count=gml_val_array_length(elements);
  int ok=vm.n_rtl==1 && vm.n_rte==1 && element_count==1 &&
    vm.rte[0].layer==vm.rtl[0].id && vm.rte[0].sprite==0;
  if(!ok)
    fprintf(stderr,
      "authored long layer did not bind its background: layers=%d elements=%d listed=%d\n",
      vm.n_rtl,vm.n_rte,element_count);
  gml_values_release(&elements,1);
  gml_vm_rooms_clear_tilemaps(&vm);
  free(vm.tilemaps);
  free(vm.rtl);
  free(vm.rte);
  return ok;
}

static int retired_builtin_script_shadow_case(const char *script_name){
  uint8_t data[20]={0};
  fixture_word(data,0,((uint32_t)OP_CALL<<24)|((uint32_t)DT_VAR<<16));
  fixture_word(data,1,0);
  fixture_word(data,2,((uint32_t)OP_RET<<24)|((uint32_t)DT_VAR<<16));
  fixture_word(data,3,((uint32_t)OP_PUSH<<24)|((uint32_t)DT_INT16<<16)|91u);
  fixture_word(data,4,((uint32_t)OP_RET<<24)|((uint32_t)DT_VAR<<16));
  GmlCode code[2]={0};
  code[0].name=(char*)"gml_Script_neutral_retired_builtin_caller";
  code[0].start=0;
  code[0].length=12;
  char code_name[96];
  snprintf(code_name,sizeof code_name,"gml_Script_%s",script_name);
  code[1].name=code_name;
  code[1].start=12;
  code[1].length=8;
  uint32_t reference_address=4;
  const char *reference_name=script_name;
  GmlWin win={0};
  win.data=data;
  win.size=sizeof data;
  win.bytecode=17;
  win.code=code;
  win.n_code=2;
  win.ref_addr=&reference_address;
  win.ref_name=&reference_name;
  win.n_refs=1;
  GmlVM vm={0};
  vm.win=&win;
  vm.cur_code_index=-1;
  vm.math_epsilon=1e-5;
  GmlVal result=vundef();
  int ok=1;
  /* Repeat the call site so a cached native ID cannot displace the payload script. */
  for(int repeat=0;ok && repeat<3;repeat++){
    result=gml_vm_run_code(&vm,0,NULL,NULL,NULL,0);
    ok=result.t==V_REAL && result.d==91;
  }
  if(!ok)
    fprintf(stderr,"retired builtin %s replaced the packaged compatibility script: %.0f\n",
            script_name,result.t==V_REAL?result.d:-1.0);
  gml_vm_free(&vm);
  for(int index=0;index<2;index++){
    free(code[index].insn);
    free(code[index].insn_pc);
    free(code[index].branch_index);
  }
  free(win.code_hix);
  free(win.ref_hix);
  return ok;
}

int expect_retired_builtin_script_shadow(void){
  return retired_builtin_script_shadow_case("action_move_to") &&
         retired_builtin_script_shadow_case("background_get_height") &&
         retired_builtin_script_shadow_case("background_get_width") &&
         retired_builtin_script_shadow_case("draw_background") &&
         retired_builtin_script_shadow_case("draw_background_ext") &&
         retired_builtin_script_shadow_case("object_get_depth") &&
         retired_builtin_script_shadow_case("object_set_depth");
}

int expect_deactivated_instance_reference(void){
  GmlInstance instance={0};
  instance.deactivated=1;
  instance.id=100001;
  instance.x=73;
  GmlVM vm={0};
  vm.inst=&instance;
  vm.inst_count=1;
  vm.inst_cap=1;
  int found=0;
  GmlVal x=gml_inst_var_get_val(&vm,vreal(instance.id),"x",&found);
  GmlInstance *resolved=gml_vm_instance_by_id(&vm,instance.id);
  if(!found || x.t!=V_REAL || x.d!=73 || resolved!=&instance){
    fprintf(stderr,"deactivated explicit-id lookup mismatch: found=%d x=%.0f resolved=%d\n",
            found,x.t==V_REAL?x.d:-1.0,resolved==&instance);
    return 0;
  }
  return 1;
}

int expect_destroy_callback_reference(void){
  /* Both disposal events read a saved explicit ID, not the implicit self shortcut. */
  uint8_t data[72]={0};
  uint32_t refs[6];
  const char *names[6]={"x","destroy_seen","instance_destroy",
                        "x","cleanup_seen","instance_destroy"};
  GmlCode code[2]={{0}};
  for(int event=0;event<2;event++){
    int word=event*9;
    fixture_word(data,word+0,(OP_PUSH<<24)|(DT_INT32<<16));
    fixture_word(data,word+1,100001);
    fixture_word(data,word+2,(OP_PUSH<<24)|(DT_VAR<<16));
    fixture_word(data,word+3,0x80000000u);
    fixture_word(data,word+4,(OP_POP<<24)|(DT_VAR<<16)|(DT_VAR<<20)|(uint16_t)IT_GLOBAL);
    fixture_word(data,word+5,0xa0000000u);
    fixture_word(data,word+6,(OP_CALL<<24));
    fixture_word(data,word+7,0);
    fixture_word(data,word+8,(OP_EXIT<<24)|(DT_VAR<<16));
    refs[event*3+0]=(uint32_t)(word+3)*4;
    refs[event*3+1]=(uint32_t)(word+5)*4;
    refs[event*3+2]=(uint32_t)(word+7)*4;
    code[event].name=event?(char*)"gml_Object_neutral_disposal_CleanUp_0":
                           (char*)"gml_Object_neutral_disposal_Destroy_0";
    code[event].start=(uint32_t)word*4;
    code[event].length=36;
  }
  GmlWin win={0};
  win.bytecode=17; win.data=data; win.size=sizeof data;
  win.code=code; win.n_code=2;
  win.ref_addr=refs; win.ref_name=names; win.n_refs=6;
  GmlVM vm={0};
  vm.win=&win; vm.cur_code_index=-1;
  vm.inst=calloc(1,sizeof *vm.inst);
  vm.objects=calloc(1,sizeof *vm.objects);
  if(!vm.inst || !vm.objects){ free(vm.inst); free(vm.objects); return 0; }
  vm.inst_count=vm.inst_cap=vm.n_objects=1;
  vm.objects[0].name=(char*)"neutral_disposal";
  vm.objects[0].parent=-1;
  vm.inst[0].active=1; vm.inst[0].id=100001; vm.inst[0].x=73;
  gml_instance_destroy(&vm,&vm.inst[0]);
  GmlVal *destroy=gml_varmap_get(&vm.globals,"destroy_seen");
  GmlVal *cleanup=gml_varmap_get(&vm.globals,"cleanup_seen");
  int ok=destroy && cleanup && destroy->t==V_REAL && cleanup->t==V_REAL &&
         destroy->d==73 && cleanup->d==73 && vm.inst[0].marked &&
         !gml_vm_instance_by_id(&vm,100001) && !gml_find_instance(&vm,0);
  if(!ok) fprintf(stderr,"explicit receiver disappeared during disposal: destroy=%.0f cleanup=%.0f\n",
                  destroy?destroy->d:-1.0,cleanup?cleanup->d:-1.0);
  gml_vm_free(&vm);
  for(int event=0;event<2;event++){
    free(code[event].insn); free(code[event].insn_pc); free(code[event].branch_index);
  }
  free(win.code_hix); free(win.ref_hix);
  return ok;
}

int expect_room_camera_reservation(void){
  uint8_t data[512]={0};
  char *strings[]={(char*)"neutral_camera_room"};
  uint32_t string_offsets[]={400};
  fixture_w32(data,0,1);
  fixture_w32(data,4,16);
  fixture_w32(data,16,400);
  fixture_w32(data,24,2048);
  fixture_w32(data,28,1728);
  fixture_w32(data,32,60);
  fixture_w32(data,52,1);
  fixture_w32(data,56,284);
  fixture_w32(data,60,128);
  fixture_w32(data,64,288);
  fixture_w32(data,68,292);
  fixture_w32(data,128,1);
  fixture_w32(data,132,160);
  fixture_w32(data,160,1);
  fixture_w32(data,164,32);
  fixture_w32(data,168,48);
  fixture_w32(data,172,384);
  fixture_w32(data,176,216);
  fixture_w32(data,188,384);
  fixture_w32(data,192,216);
  fixture_w32(data,204,(uint32_t)-1);
  fixture_w32(data,208,(uint32_t)-1);
  fixture_w32(data,212,(uint32_t)-1);

  GmlWin win={0};
  win.data=data;
  win.size=sizeof data;
  win.bytecode=17;
  win.game_speed=60;
  win.n_chunks=1;
  memcpy(win.chunks[0].name,"ROOM",5);
  win.chunks[0].off=0;
  win.chunks[0].size=sizeof data;
  win.strs=strings;
  win.str_charoff=string_offsets;
  win.n_strs=1;
  AnygmCompatibilityProfile profile={0};
  profile.has_modern_layer_semantics=1;
  profile.legacy_view_slots=1;
  win.compatibility=&profile;

  GmlVM vm;
  if(gml_vm_init(&vm,&win,NULL)) return 0;
  GmlVal dynamic_camera=gml_builtin_call(&vm,"camera_create",NULL,0);
  GmlVal dynamic_position[3]={dynamic_camera,vreal(960),vreal(560)};
  (void)gml_builtin_call(&vm,"camera_set_view_pos",dynamic_position,3);
  GmlVal dynamic_size[3]={dynamic_camera,vreal(427),vreal(240)};
  (void)gml_builtin_call(&vm,"camera_set_view_size",dynamic_size,3);
  GmlVal room_camera_args[2]={vreal(0),vreal(0)};
  GmlVal before_binding=gml_builtin_call(&vm,"room_get_camera",room_camera_args,2);
  GmlVal set_binding_args[3]={vreal(0),vreal(0),dynamic_camera};
  GmlVal set_binding=gml_builtin_call(&vm,"room_set_camera",set_binding_args,3);
  GmlVal stored_binding=gml_builtin_call(&vm,"room_get_camera",room_camera_args,2);
  gml_room_enter(&vm,0);
  GmlVal view=vreal(0);
  GmlVal room_camera=gml_builtin_call(&vm,"view_get_camera",&view,1);
  GmlVal room_x=gml_builtin_call(&vm,"camera_get_view_x",&room_camera,1);
  GmlVal room_y=gml_builtin_call(&vm,"camera_get_view_y",&room_camera,1);
  GmlVal room_w=gml_builtin_call(&vm,"camera_get_view_width",&room_camera,1);
  GmlVal room_h=gml_builtin_call(&vm,"camera_get_view_height",&room_camera,1);
  int dynamic_id=dynamic_camera.t==V_REAL?(int)dynamic_camera.d:-1;
  int ok=dynamic_id>=GML_ROOM_CAMERA_COUNT &&
    before_binding.t==V_REAL && before_binding.d==0 &&
    set_binding.t==V_REAL && set_binding.d==0 &&
    stored_binding.t==V_REAL && stored_binding.d==dynamic_camera.d &&
    room_camera.t==V_REAL && room_camera.d==dynamic_camera.d &&
    room_x.t==V_REAL && room_x.d==960 && room_y.t==V_REAL && room_y.d==560 &&
    room_w.t==V_REAL && room_w.d==427 && room_h.t==V_REAL && room_h.d==240 &&
    gml_global_arr(&vm,"__gml_camera_x",0)==32 &&
    gml_global_arr(&vm,"__gml_camera_y",0)==48 &&
    gml_global_arr(&vm,"__gml_camera_w",0)==384 &&
    gml_global_arr(&vm,"__gml_camera_h",0)==216 &&
    gml_global_arr(&vm,"__gml_camera_x",dynamic_id)==960 &&
    gml_global_arr(&vm,"__gml_camera_y",dynamic_id)==560;
  if(!ok)
    fprintf(stderr,
      "room/dynamic camera binding failed: dynamic=%d before=%.0f stored=%.0f room=%.0f"
      " rect=(%.0f,%.0f %.0fx%.0f)\n",
      dynamic_id,before_binding.t==V_REAL?before_binding.d:-2.0,
      stored_binding.t==V_REAL?stored_binding.d:-2.0,
      room_camera.t==V_REAL?room_camera.d:-1.0,
      room_x.t==V_REAL?room_x.d:-1.0,room_y.t==V_REAL?room_y.d:-1.0,
      room_w.t==V_REAL?room_w.d:-1.0,room_h.t==V_REAL?room_h.d:-1.0);
  gml_vm_free(&vm);
  return ok;
}


int expect_revision16_room_uses_legacy_view(void){
  uint8_t data[512]={0};
  char *strings[]={(char*)"legacy_camera_room"};
  uint32_t string_offsets[]={400};
  fixture_w32(data,0,1);
  fixture_w32(data,4,16);
  fixture_w32(data,16,400);
  fixture_w32(data,24,320);
  fixture_w32(data,28,480);
  fixture_w32(data,32,30);
  fixture_w32(data,52,1);
  fixture_w32(data,56,284);
  fixture_w32(data,60,128);
  fixture_w32(data,64,288);
  fixture_w32(data,68,292);
  fixture_w32(data,128,1);
  fixture_w32(data,132,160);
  fixture_w32(data,160,1);
  fixture_w32(data,164,0);
  fixture_w32(data,168,48);
  fixture_w32(data,172,320);
  fixture_w32(data,176,240);
  fixture_w32(data,188,640);
  fixture_w32(data,192,480);
  fixture_w32(data,204,(uint32_t)-1);
  fixture_w32(data,208,(uint32_t)-1);
  fixture_w32(data,212,(uint32_t)-1);

  GmlWin win={0};
  win.data=data;
  win.size=sizeof data;
  win.bytecode=16;
  win.option_flags=UINT64_C(0x00400000);
  win.n_chunks=1;
  memcpy(win.chunks[0].name,"ROOM",5);
  win.chunks[0].off=0;
  win.chunks[0].size=sizeof data;
  win.strs=strings;
  win.str_charoff=string_offsets;
  win.n_strs=1;

  AnygmCompatibilityProfile profile={0};
  profile.has_modern_layer_semantics=1;
  profile.uses_legacy_room_cameras=1;
  profile.legacy_view_slots=1;
  win.compatibility=&profile;

  GmlVM vm;
  if(gml_vm_init(&vm,&win,NULL)) return 0;
  gml_room_enter(&vm,0);
  double camera=gml_global_arr(&vm,"view_camera",0);
  int ok=camera==-1 &&
    gml_global_arr(&vm,"__gml_camera_live",0)==0 &&
    gml_global_arr(&vm,"view_yview",0)==48;
  if(!ok)
    fprintf(stderr,
      "revision-16 legacy room view was replaced by a synthetic camera: camera=%.0f live=%.0f y=%.0f\n",
      camera,
      gml_global_arr(&vm,"__gml_camera_live",0),gml_global_arr(&vm,"view_yview",0));
  gml_vm_free(&vm);
  return ok;
}

int expect_room_order_boundaries(void){
  GmlcProject project={0};
  GmlcObject object={0};
  GmlcObjectEvent event={0};
  GmlcRoom rooms[3]={{0}};
  GmlcRoomInstance placed={0};
  int room_order[3]={2,0,1};
  char *room_names[3]={(char*)"neutral_middle",(char*)"neutral_last",(char*)"neutral_first"};
  AnygmHostServices services={0};
  char source_path[]="/tmp/gml-room-order-boundaries-source-XXXXXX";
  char package_path[]="/tmp/gml-room-order-boundaries-package-XXXXXX";
  int source_fd=-1,package_fd=-1;
  int ok=0;

  source_fd=mkstemp(source_path);
  package_fd=mkstemp(package_path);
  if(source_fd<0 || package_fd<0) goto cleanup;
  close(source_fd); source_fd=-1;
  close(package_fd); package_fd=-1;
  if(!fixture_write_text(source_path,
       "global.first_id=room_first; global.last_id=room_last; "
       "global.next_id=room_next(room_first); global.previous_id=room_previous(room_last); "
       "global.before_first=room_previous(room_first); global.after_last=room_next(room_last); "
       "global.current_id=room;\n"))
    goto cleanup;

  services.struct_size=sizeof services;
  services.abi_version=ANYGM_HOST_SERVICES_VERSION;
  anygm_stdio_vfs_services_init(&services);
  project.name="room-order-boundary-fixture";
  project.host=&services;
  project.objects=&object;
  project.n_objects=project.cap_objects=1;
  project.rooms=rooms;
  project.n_rooms=project.cap_rooms=3;
  project.room_order=room_order;
  project.n_room_order=3;

  object.id=object.name=(char*)"obj_boundary_probe";
  object.sprite_id=object.mask_id=object.parent_id=-1;
  object.visible=1;
  object.events=&event;
  object.n_events=object.cap_events=1;
  event.event_type=0;
  event.event_number=0;
  event.source_path=source_path;

  for(int index=0;index<3;index++){
    rooms[index].id=rooms[index].name=room_names[index];
    rooms[index].width=320;
    rooms[index].height=240;
    rooms[index].speed=60;
  }
  placed.id=placed.name=(char*)"placed_boundary_probe";
  placed.object_id=0;
  placed.instance_id=100000;
  rooms[2].instances=&placed;
  rooms[2].n_instances=rooms[2].cap_instances=1;

  {
    char error[256]={0};
    if(!gmlc_package_write_structural(&project,package_path,error,sizeof error)){
      fprintf(stderr,"room-order boundary package failed: %s\n",error);
      goto cleanup;
    }
  }
  {
    GmlWin win;
    if(anygm_stdio_load_win(&win,package_path)) goto cleanup;
    GmlVM vm;
    if(gml_vm_init(&vm,&win,&services)){
      gml_win_free(&win);
      goto cleanup;
    }
    gml_room_enter(&vm,2);
    GmlVal *first=gml_varmap_get(&vm.globals,"first_id");
    GmlVal *last=gml_varmap_get(&vm.globals,"last_id");
    GmlVal *next=gml_varmap_get(&vm.globals,"next_id");
    GmlVal *previous=gml_varmap_get(&vm.globals,"previous_id");
    GmlVal *before_first=gml_varmap_get(&vm.globals,"before_first");
    GmlVal *after_last=gml_varmap_get(&vm.globals,"after_last");
    GmlVal *current=gml_varmap_get(&vm.globals,"current_id");
    ok=first&&first->t==V_REAL&&first->d==2 &&
       last&&last->t==V_REAL&&last->d==1 &&
       next&&next->t==V_REAL&&next->d==0 &&
       previous&&previous->t==V_REAL&&previous->d==0 &&
       before_first&&before_first->t==V_REAL&&before_first->d==-1 &&
       after_last&&after_last->t==V_REAL&&after_last->d==-1 &&
       current&&current->t==V_REAL&&current->d==2;
    if(!ok)
      fprintf(stderr,
        "room-order boundary mismatch: first=%.0f last=%.0f next=%.0f previous=%.0f before=%.0f after=%.0f current=%.0f\n",
        first&&first->t==V_REAL?first->d:-99.0,last&&last->t==V_REAL?last->d:-99.0,
        next&&next->t==V_REAL?next->d:-99.0,previous&&previous->t==V_REAL?previous->d:-99.0,
        before_first&&before_first->t==V_REAL?before_first->d:-99.0,
        after_last&&after_last->t==V_REAL?after_last->d:-99.0,
        current&&current->t==V_REAL?current->d:-99.0);
    gml_vm_free(&vm);
    gml_win_free(&win);
  }

cleanup:
  if(source_fd>=0) close(source_fd);
  if(package_fd>=0) close(package_fd);
  unlink(source_path);
  unlink(package_path);
  return ok;
}


static int expect_alarm_dispatch_order_for_revision(const char *package_path,
                                                    uint32_t bytecode,
                                                    double expected_order,
                                                    const char *family){
  GmlWin win;
  if(anygm_stdio_load_win(&win,package_path)) return 0;
  win.bytecode=(uint8_t)bytecode;
  AnygmContentFacts facts={0};
  AnygmCompatibilityProfile profile={0};
  char error[256]={0};
  if(!anygm_content_facts_detect(&win,&facts,error,sizeof error) ||
     !anygm_compatibility_resolve(&facts,&profile,error,sizeof error)){
    fprintf(stderr,"%s alarm dispatch profile failed: %s\n",family,error);
    gml_win_free(&win);
    return 0;
  }
  win.compatibility=&profile;
  GmlVM vm;
  if(gml_vm_init(&vm,&win,NULL)){
    gml_win_free(&win);
    return 0;
  }
  gml_room_enter(&vm,0);
  *gml_varmap_put(&vm.globals,"alarm_order")=vreal(0);
  gml_vm_step(&vm);
  GmlVal *order=gml_varmap_get(&vm.globals,"alarm_order");
  int ok=order && order->t==V_REAL && order->d==expected_order;
  if(!ok)
    fprintf(stderr,"%s alarm dispatch order mismatch: got=%.0f expected=%.0f\n",
      family,order&&order->t==V_REAL?order->d:-1.0,expected_order);
  gml_vm_free(&vm);
  win.compatibility=NULL;
  gml_win_free(&win);
  return ok;
}


int expect_alarm_dispatch_order(void){
  GmlcProject project={0};
  GmlcObject objects[5]={{0}};
  GmlcObjectEvent base_events[2]={{0}};
  GmlcRoom room={0};
  GmlcRoomInstance placed[5]={{0}};
  int room_order[1]={0};
  const char *object_names[5]={
    "obj_alarm_base","obj_alarm_alpha","obj_alarm_beta",
    "obj_alarm_gamma","obj_alarm_delta"
  };
  const int placed_objects[5]={3,1,4,1,2};
  const int placed_tokens[5]={31,11,41,12,21};
  AnygmHostServices services={0};
  char create_path[]="/tmp/gml-alarm-dispatch-create-XXXXXX";
  char alarm_path[]="/tmp/gml-alarm-dispatch-alarm-XXXXXX";
  char package_path[]="/tmp/gml-alarm-dispatch-package-XXXXXX";
  int create_fd=-1,alarm_fd=-1,package_fd=-1;
  int ok=0;

  create_fd=mkstemp(create_path);
  alarm_fd=mkstemp(alarm_path);
  package_fd=mkstemp(package_path);
  if(create_fd<0 || alarm_fd<0 || package_fd<0) goto cleanup;
  close(create_fd); create_fd=-1;
  close(alarm_fd); alarm_fd=-1;
  close(package_fd); package_fd=-1;
  if(!fixture_write_text(create_path,"alarm[1]=1;\n") ||
     !fixture_write_text(alarm_path,
       "global.alarm_order=global.alarm_order*100+x;\n"))
    goto cleanup;

  services.struct_size=sizeof services;
  services.abi_version=ANYGM_HOST_SERVICES_VERSION;
  anygm_stdio_vfs_services_init(&services);
  project.name="alarm-dispatch-order-fixture";
  project.host=&services;
  project.objects=objects;
  project.n_objects=project.cap_objects=5;
  project.rooms=&room;
  project.n_rooms=project.cap_rooms=1;
  project.room_order=room_order;
  project.n_room_order=1;
  for(int index=0;index<5;index++){
    objects[index].id=objects[index].name=(char*)object_names[index];
    objects[index].sprite_id=objects[index].mask_id=-1;
    objects[index].parent_id=index?0:-1;
    objects[index].visible=1;
  }
  objects[0].events=base_events;
  objects[0].n_events=objects[0].cap_events=2;
  base_events[0].event_type=0;
  base_events[0].event_number=0;
  base_events[0].source_path=create_path;
  base_events[1].event_type=2;
  base_events[1].event_number=1;
  base_events[1].source_path=alarm_path;

  room.id=room.name=(char*)"room_alarm_dispatch";
  room.width=320;
  room.height=240;
  room.speed=30;
  room.instances=placed;
  room.n_instances=room.cap_instances=5;
  for(int index=0;index<5;index++){
    placed[index].id=placed[index].name=(char*)"placed_alarm_probe";
    placed[index].object_id=placed_objects[index];
    placed[index].instance_id=100000+index;
    placed[index].x=placed_tokens[index];
  }

  {
    char error[256]={0};
    if(!gmlc_package_write_structural(&project,package_path,error,sizeof error)){
      fprintf(stderr,"alarm dispatch package failed: %s\n",error);
      goto cleanup;
    }
  }
  ok=expect_alarm_dispatch_order_for_revision(
       package_path,16,1112213141.0,"first-generation Studio") &&
     expect_alarm_dispatch_order_for_revision(
       package_path,17,3111411221.0,"second-generation Studio");

cleanup:
  if(create_fd>=0) close(create_fd);
  if(alarm_fd>=0) close(alarm_fd);
  if(package_fd>=0) close(package_fd);
  unlink(create_path);
  unlink(alarm_path);
  unlink(package_path);
  return ok;
}


int expect_legacy_jump_to_start(void){
  int ok=1;
  for(int cached=0;cached<2;cached++){
    GmlVM vm={0};
    vm.inst=calloc(1,sizeof(*vm.inst));
    if(!vm.inst) return 0;
    vm.inst_cap=1;
    GmlInstance *subject=vm.cur_self=vm.inst;
    subject->x=90; subject->y=80;
    subject->xstart=12; subject->ystart=15;
    subject->xprevious=70; subject->yprevious=60;
    subject->hspeed=3; subject->vspeed=4; subject->speed=5;
    subject->direction=53; subject->path_position=0.25;
    /* Exercise the touched-instance overlay of a valid synthetic grid. */
    vm.cg_gen=1;
    int id=gml_builtin_fast_id(&vm,"action_move_start");
    if(cached) gml_builtin_call_fast_id(&vm,id,"action_move_start",NULL,0);
    else gml_builtin_call(&vm,"action_move_start",NULL,0);
    int restored=subject->x==12 && subject->y==15;
    ok=ok && restored && id>0 && vm.cg_overlay_n==1 &&
       vm.cg_overlay[0]==0;
    subject->xstart=3.5; subject->ystart=-4.25;
    vm.action_relative=1;
    if(cached) gml_builtin_call_fast_id(&vm,id,"action_move_start",NULL,0);
    else gml_builtin_call(&vm,"action_move_start",NULL,0);
    int edited=subject->x==3.5 && subject->y==-4.25;
    ok=ok && edited && subject->xprevious==70 && subject->yprevious==60 &&
       subject->hspeed==3 && subject->vspeed==4 && subject->speed==5 &&
       subject->direction==53 && subject->path_position==0.25 &&
       subject->xstart==3.5 && subject->ystart==-4.25;
    vm.cur_self=NULL;
    if(cached) gml_builtin_call_fast_id(&vm,id,"action_move_start",NULL,0);
    else gml_builtin_call(&vm,"action_move_start",NULL,0);
    if(!restored || !edited)
      fprintf(stderr,"jump-to-start mode=%d original=%d edited=%d\n",cached,restored,edited);
    gml_vm_free(&vm);
  }
  return ok;
}

static double fixture_linear_step(GmlVM *vm,int cached,const char *name,
                                  double x,double y,double step,double target){
  GmlVal args[]={vreal(x),vreal(y),vreal(step),vreal(target)};
  int id=gml_builtin_fast_id(vm,name);
  GmlVal result=cached?gml_builtin_call_fast_id(vm,id,name,args,4)
                      :gml_builtin_call(vm,name,args,4);
  return result.t==V_REAL?result.d:-1;
}

int expect_linear_motion_collision_filters(void){
  int ok=1;
  for(int cached=0;cached<2;cached++){
    GmlSprite sprite={.w=1,.h=1,.collision_kind=1};
    GmlRender render={.spr=&sprite,.n_spr=1};
    GmlObject objects[3]={{.parent=-1},{.parent=-1},{.parent=1}};
    GmlInstance instances[2]={0};
    for(int i=0;i<2;i++){
      instances[i].id=100000+i;
      instances[i].obj=i;
      instances[i].active=1;
      instances[i].image_xscale=instances[i].image_yscale=1;
    }
    instances[1].x=4;
    GmlVM vm={.render=&render,.inst=instances,.inst_count=2,.inst_cap=2,
              .cur_self=&instances[0],.objects=objects,.n_objects=3};
    gml_colgrid_invalidate(&vm);
    const char *name="mp_linear_step_object";
    double result=fixture_linear_step(&vm,cached,name,8,0,4,1);
    int blocked=result==0 && instances[0].x==0;
    ok=ok && blocked;
    result=fixture_linear_step(&vm,cached,name,8,0,4,100001);
    ok=ok && result==0 && instances[0].x==0;
    result=fixture_linear_step(&vm,cached,name,8,0,4,-3);
    ok=ok && result==0 && instances[0].x==0;
    instances[1].obj=2;
    result=fixture_linear_step(&vm,cached,name,8,0,4,1);
    ok=ok && result==0 && instances[0].x==0; /* An inherited object also blocks. */
    instances[1].obj=1;
    result=fixture_linear_step(&vm,cached,name,8,0,4,2);
    int ignored=result==0 && instances[0].x==4;
    ok=ok && ignored;
    result=fixture_linear_step(&vm,cached,name,8,0,4,2);
    int arrived=result==1 && instances[0].x==8;
    ok=ok && arrived;
    result=fixture_linear_step(&vm,cached,name,8,0,4,-3);
    ok=ok && result==1 && instances[0].x==8;
    instances[0].x=0; gml_colgrid_touch(&vm,&instances[0]);
    result=fixture_linear_step(&vm,cached,name,2,0,4,1);
    ok=ok && result==1 && instances[0].x==2;
    instances[0].x=0; gml_colgrid_touch(&vm,&instances[0]);
    instances[1].active=0; gml_colgrid_invalidate(&vm);
    result=fixture_linear_step(&vm,cached,name,8,0,4,-3);
    ok=ok && result==0 && instances[0].x==4;
    instances[1].active=1; instances[0].x=0; gml_colgrid_invalidate(&vm);
    result=fixture_linear_step(&vm,cached,"mp_linear_step",8,0,4,0);
    ok=ok && result==0 && instances[0].x==4;
    instances[0].x=0; instances[1].solid=1; gml_colgrid_invalidate(&vm);
    result=fixture_linear_step(&vm,cached,"mp_linear_step",8,0,4,0);
    int solid=result==0 && instances[0].x==0;
    ok=ok && solid;
    instances[1].solid=0;
    result=fixture_linear_step(&vm,cached,"mp_linear_step",8,0,4,1);
    ok=ok && result==0 && instances[0].x==0;
    result=fixture_linear_step(&vm,cached,name,NAN,0,4,1);
    ok=ok && result==0 && instances[0].x==0;
    result=fixture_linear_step(&vm,cached,name,8,0,4,NAN);
    ok=ok && result==0 && instances[0].x==0;
    if(!blocked || !ignored || !arrived || !solid)
      fprintf(stderr,"linear step mode=%d blocked=%d ignored=%d arrived=%d solid=%d\n",
              cached,blocked,ignored,arrived,solid);
    /* A free step turns the retained velocity; a blocked final step still reports
     * that the target is within one step. Neither collision path takes a detour. */
    const char *names[]={"mp_linear_step_object","mp_linear_step"};
    for(int variant=0;variant<2;variant++){
      GmlInstance *self=&instances[0],*wall=&instances[1];
      double selector=variant?1:100001;
      wall->x=20; wall->y=20;
      self->x=self->y=0; self->direction=123; self->speed=2;
      gml_colgrid_invalidate(&vm);
      result=fixture_linear_step(&vm,cached,names[variant],0,-8,4,selector);
      ok &= result==0 && self->x==0 && self->y==-4 && self->direction==90 &&
            self->speed==2 && fabs(self->hspeed)<1e-12 && self->vspeed==-2;
      self->x=self->y=0;
      result=fixture_linear_step(&vm,cached,names[variant],3,4,2.5,selector);
      ok &= result==0 && self->x==1.5 && self->y==2 &&
            fabs(self->direction-306.869897645844)<1e-9 && self->speed==2 &&
            fabs(self->hspeed-1.2)<1e-12 && fabs(self->vspeed-1.6)<1e-12;
      self->x=self->y=0; self->direction=123;
      wall->x=0; wall->y=-4; gml_colgrid_invalidate(&vm);
      double hs=self->hspeed,vs=self->vspeed;
      result=fixture_linear_step(&vm,cached,names[variant],0,-8,4,selector);
      ok &= result==0 && self->x==0 && self->y==0 && self->direction==123 &&
            self->hspeed==hs && self->vspeed==vs;
      for(int step=4;step<=8;step+=4){
        result=fixture_linear_step(&vm,cached,names[variant],0,-4,step,selector);
        ok &= result==1 && self->x==0 && self->y==0 && self->direction==123 &&
              self->hspeed==hs && self->vspeed==vs;
      }
      result=fixture_linear_step(&vm,cached,names[variant],0,0,4,selector);
      ok &= result==1 && self->direction==123 && self->hspeed==hs && self->vspeed==vs;
      result=fixture_linear_step(&vm,cached,names[variant],8,0,0,selector);
      ok &= result==0 && self->x==0 && self->y==0 && self->direction==0 &&
            self->hspeed==2 && self->vspeed==0;
      result=fixture_linear_step(&vm,cached,names[variant],0,-8,-4,selector);
      ok &= result==0 && self->x==0 && self->y==4 && self->direction==270 &&
            fabs(self->hspeed)<1e-12 && self->vspeed==2;
      self->x=self->y=0; gml_colgrid_touch(&vm,self);
      result=fixture_linear_step(&vm,cached,names[variant],1e-10,0,1e-12,selector);
      ok &= result==0 && fabs(self->x-1e-12)<1e-24 && self->y==0 && self->direction==0;
      self->x=1e308; self->y=0; self->direction=123;
      result=fixture_linear_step(&vm,cached,names[variant],0,0,-1e308,selector);
      ok &= result==0 && self->x==1e308 && self->y==0 && self->direction==123;
    }
    vm.render=NULL; vm.inst=NULL; vm.inst_count=vm.inst_cap=0;
    vm.cur_self=NULL; vm.objects=NULL; vm.n_objects=0;
    gml_vm_free(&vm);
  }
  return ok;
}

int expect_automatic_motion_order(void){
  GmlcProject project={0};
  GmlcObject objects[2]={{0}};
  GmlcObjectEvent controller_events[2]={{0}};
  GmlcObjectEvent probe_events[2]={{0}};
  GmlcRoom room={0};
  GmlcRoomInstance placed={0};
  int room_order[1]={0};
  AnygmHostServices services={0};
  char controller_create_path[]="/tmp/gml-automatic-motion-controller-create-XXXXXX";
  char controller_alarm_path[]="/tmp/gml-automatic-motion-controller-alarm-XXXXXX";
  char probe_create_path[]="/tmp/gml-automatic-motion-probe-create-XXXXXX";
  char probe_step_path[]="/tmp/gml-automatic-motion-probe-step-XXXXXX";
  char package_path[]="/tmp/gml-automatic-motion-package-XXXXXX";
  int controller_create_fd=-1,controller_alarm_fd=-1,probe_create_fd=-1,probe_step_fd=-1;
  int package_fd=-1;
  int ok=0;

  controller_create_fd=mkstemp(controller_create_path);
  controller_alarm_fd=mkstemp(controller_alarm_path);
  probe_create_fd=mkstemp(probe_create_path);
  probe_step_fd=mkstemp(probe_step_path);
  package_fd=mkstemp(package_path);
  if(controller_create_fd<0 || controller_alarm_fd<0 || probe_create_fd<0 ||
     probe_step_fd<0 || package_fd<0)
    goto cleanup;
  close(controller_create_fd); controller_create_fd=-1;
  close(controller_alarm_fd); controller_alarm_fd=-1;
  close(probe_create_fd); probe_create_fd=-1;
  close(probe_step_fd); probe_step_fd=-1;
  close(package_fd); package_fd=-1;
  if(!fixture_write_text(controller_create_path,
       "alarm[0]=1; global.motion_probe_steps=0;\n") ||
     !fixture_write_text(controller_alarm_path,
       "global.motion_probe=instance_create(10,20,obj_motion_probe);\n") ||
     !fixture_write_text(probe_create_path,
       "direction=0; speed=3; friction=0.1;\n") ||
     !fixture_write_text(probe_step_path,"global.motion_probe_steps+=1;\n"))
    goto cleanup;
  services.struct_size=sizeof services;
  services.abi_version=ANYGM_HOST_SERVICES_VERSION;
  anygm_stdio_vfs_services_init(&services);
  project.name="automatic-motion-order-fixture";
  project.host=&services;
  project.objects=objects;
  project.n_objects=project.cap_objects=2;
  project.rooms=&room;
  project.n_rooms=project.cap_rooms=1;
  project.room_order=room_order;
  project.n_room_order=1;
  objects[0].id=objects[0].name=(char*)"obj_motion_controller";
  objects[0].sprite_id=objects[0].mask_id=objects[0].parent_id=-1;
  objects[0].visible=1;
  objects[0].events=controller_events;
  objects[0].n_events=objects[0].cap_events=2;
  controller_events[0].event_type=0;
  controller_events[0].event_number=0;
  controller_events[0].source_path=controller_create_path;
  controller_events[1].event_type=2;
  controller_events[1].event_number=0;
  controller_events[1].source_path=controller_alarm_path;
  objects[1].id=objects[1].name=(char*)"obj_motion_probe";
  objects[1].sprite_id=objects[1].mask_id=objects[1].parent_id=-1;
  objects[1].visible=1;
  objects[1].events=probe_events;
  objects[1].n_events=objects[1].cap_events=2;
  probe_events[0].event_type=0;
  probe_events[0].event_number=0;
  probe_events[0].source_path=probe_create_path;
  probe_events[1].event_type=3;
  probe_events[1].event_number=0;
  probe_events[1].source_path=probe_step_path;
  room.id=room.name=(char*)"room_motion_probe";
  room.width=320;
  room.height=240;
  room.speed=30;
  room.instances=&placed;
  room.n_instances=room.cap_instances=1;
  placed.id=placed.name=(char*)"placed_motion_controller";
  placed.object_id=0;
  placed.instance_id=100000;

  {
    char error[256]={0};
    if(!gmlc_package_write_structural(&project,package_path,error,sizeof error)){
      fprintf(stderr,"automatic motion package failed: %s\n",error);
      goto cleanup;
    }
  }
  {
    GmlWin win;
    if(anygm_stdio_load_win(&win,package_path)) goto cleanup;
    win.bytecode=16;
    AnygmContentFacts facts={0};
    AnygmCompatibilityProfile profile={0};
    char error[256]={0};
    if(!anygm_content_facts_detect(&win,&facts,error,sizeof error) ||
       !anygm_compatibility_resolve(&facts,&profile,error,sizeof error)){
      fprintf(stderr,"automatic motion profile failed: %s\n",error);
      gml_win_free(&win);
      goto cleanup;
    }
    win.compatibility=&profile;
    GmlVM vm;
    if(gml_vm_init(&vm,&win,NULL)){
      win.compatibility=NULL;
      gml_win_free(&win);
      goto cleanup;
    }
    gml_room_enter(&vm,0);
    gml_vm_step(&vm);
    GmlVal *probe_id=gml_varmap_get(&vm.globals,"motion_probe");
    GmlVal *probe_steps=gml_varmap_get(&vm.globals,"motion_probe_steps");
    GmlInstance *probe=probe_id&&probe_id->t==V_REAL?
      find_slot(&vm,(uint32_t)probe_id->d):NULL;
    if(probe){
      ok=fabs(probe->x-12.9)<1e-9 && fabs(probe->speed-2.9)<1e-9 &&
        fabs(probe->hspeed-2.9)<1e-9 && probe_steps && probe_steps->t==V_REAL &&
        fabs(probe_steps->d-1.0)<1e-9;
      if(!ok)
        fprintf(stderr,
          "first-generation Studio automatic motion mismatch: x=%.3f speed=%.3f "
          "hspeed=%.3f steps=%.3f\n",
          probe->x,probe->speed,probe->hspeed,
          probe_steps&&probe_steps->t==V_REAL?probe_steps->d:-1.0);
    }
    gml_vm_free(&vm);
    win.compatibility=NULL;
    gml_win_free(&win);
  }

cleanup:
  if(controller_create_fd>=0) close(controller_create_fd);
  if(controller_alarm_fd>=0) close(controller_alarm_fd);
  if(probe_create_fd>=0) close(probe_create_fd);
  if(probe_step_fd>=0) close(probe_step_fd);
  if(package_fd>=0) close(package_fd);
  unlink(controller_create_path);
  unlink(controller_alarm_path);
  unlink(probe_create_path);
  unlink(probe_step_path);
  unlink(package_path);
  return ok;
}


static int expect_event_boundary_room_transition_mode(int classic,int step_number){
  GmlcProject project={0};
  GmlcObject objects[2]={{0}};
  GmlcObjectEvent events[2]={{0}};
  GmlcRoom rooms[2]={{0}};
  GmlcRoomInstance placed[2]={{0}};
  int room_order[2]={0,1};
  AnygmHostServices services={0};
  char first_source[]="/tmp/gml-event-room-first-XXXXXX";
  char late_source[]="/tmp/gml-event-room-late-XXXXXX";
  char package_path[]="/tmp/gml-event-room-package-XXXXXX";
  int first_fd=-1,late_fd=-1,package_fd=-1;
  int ok=0;

  first_fd=mkstemp(first_source);
  late_fd=mkstemp(late_source);
  package_fd=mkstemp(package_path);
  if(first_fd<0 || late_fd<0 || package_fd<0) goto cleanup;
  close(first_fd); first_fd=-1;
  close(late_fd); late_fd=-1;
  close(package_fd); package_fd=-1;
  if(!fixture_write_text(first_source,
       "global.first_event_hits += 1; room_goto_next(); transition_state = 4;\n") ||
     !fixture_write_text(late_source,
       "global.late_event_hits += 1; obj_first.transition_state = 1;\n"))
    goto cleanup;

  services.struct_size=sizeof services;
  services.abi_version=ANYGM_HOST_SERVICES_VERSION;
  anygm_stdio_vfs_services_init(&services);
  project.name="event-boundary-room-fixture";
  project.host=&services;
  project.classic_version=classic?800:0;
  project.classic_executable_layout=classic?1:0;
  project.objects=objects;
  project.n_objects=project.cap_objects=2;
  project.rooms=rooms;
  project.n_rooms=project.cap_rooms=2;
  project.room_order=room_order;
  project.n_room_order=2;

  objects[0].id=objects[0].name=(char*)"obj_first";
  objects[0].sprite_id=objects[0].mask_id=objects[0].parent_id=-1;
  objects[0].visible=1;
  objects[0].persistent=1;
  objects[0].events=&events[0];
  objects[0].n_events=objects[0].cap_events=1;
  objects[1].id=objects[1].name=(char*)"obj_late";
  objects[1].sprite_id=objects[1].mask_id=objects[1].parent_id=-1;
  objects[1].visible=1;
  objects[1].events=&events[1];
  objects[1].n_events=objects[1].cap_events=1;
  for(int index=0;index<2;index++){
    events[index].event_type=3;
    events[index].event_number=step_number;
  }
  events[0].source_path=first_source;
  events[1].source_path=late_source;

  for(int index=0;index<2;index++){
    rooms[index].id=rooms[index].name=index?(char*)"room_second":(char*)"room_first";
    rooms[index].width=320;
    rooms[index].height=240;
    rooms[index].speed=60;
  }
  placed[0].id=placed[0].name=(char*)"placed_first";
  placed[0].object_id=0;
  placed[0].instance_id=100000;
  placed[1].id=placed[1].name=(char*)"placed_late";
  placed[1].object_id=1;
  placed[1].instance_id=100001;
  rooms[0].instances=placed;
  rooms[0].n_instances=rooms[0].cap_instances=2;

  {
    char error[256]={0};
    if(!gmlc_package_write_structural(&project,package_path,error,sizeof error)){
      fprintf(stderr,"event-boundary package failed: %s\n",error);
      goto cleanup;
    }
  }
  {
    GmlWin win;
    if(anygm_stdio_load_win(&win,package_path)) goto cleanup;
    GmlVM vm;
    if(gml_vm_init(&vm,&win,&services)){
      gml_win_free(&win);
      goto cleanup;
    }
    gml_room_enter(&vm,0);
    *gml_varmap_put(&vm.globals,"first_event_hits")=vreal(0);
    *gml_varmap_put(&vm.globals,"late_event_hits")=vreal(0);
    gml_vm_step(&vm);
    GmlInstance *survivor=find_slot(&vm,100000);
    GmlVal *first_hits=gml_varmap_get(&vm.globals,"first_event_hits");
    GmlVal *late_hits=gml_varmap_get(&vm.globals,"late_event_hits");
    GmlVal *transition_state=survivor?
      gml_varmap_get(&survivor->vars,"transition_state"):NULL;
    ok=vm.room_index==1 && survivor && survivor->active && survivor->persistent &&
       first_hits && first_hits->t==V_REAL && first_hits->d==1 &&
       late_hits && late_hits->t==V_REAL && late_hits->d==0 &&
       transition_state && transition_state->t==V_REAL && transition_state->d==4;
    if(!ok)
      fprintf(stderr,
        "%s Step_%d event-boundary room transition mismatch: room=%d survivor=%d first=%.0f late=%.0f state=%.0f\n",
        classic?"classic":"Studio",step_number,vm.room_index,survivor&&survivor->active,
        first_hits&&first_hits->t==V_REAL?first_hits->d:-1.0,
        late_hits&&late_hits->t==V_REAL?late_hits->d:-1.0,
        transition_state&&transition_state->t==V_REAL?transition_state->d:-1.0);
    gml_vm_free(&vm);
    gml_win_free(&win);
  }

cleanup:
  if(first_fd>=0) close(first_fd);
  if(late_fd>=0) close(late_fd);
  if(package_fd>=0) close(package_fd);
  unlink(first_source);
  unlink(late_source);
  unlink(package_path);
  return ok;
}


static int expect_studio_begin_step_transition_continuation(void){
  GmlcProject project={0};
  GmlcObject objects[3]={{0}};
  GmlcObjectEvent events[5]={{0}};
  GmlcRoom rooms[2]={{0}};
  GmlcRoomInstance placed[3]={{0}};
  int room_order[2]={0,1};
  AnygmHostServices services={0};
  char transition_path[]="/tmp/gml-begin-room-transition-XXXXXX";
  char old_late_path[]="/tmp/gml-begin-room-old-late-XXXXXX";
  char entered_create_path[]="/tmp/gml-begin-room-entered-create-XXXXXX";
  char entered_alarm_path[]="/tmp/gml-begin-room-entered-alarm-XXXXXX";
  char entered_step_path[]="/tmp/gml-begin-room-entered-step-XXXXXX";
  char package_path[]="/tmp/gml-begin-room-package-XXXXXX";
  int transition_fd=-1,old_late_fd=-1,entered_create_fd=-1;
  int entered_alarm_fd=-1,entered_step_fd=-1,package_fd=-1;
  int ok=0;

  transition_fd=mkstemp(transition_path);
  old_late_fd=mkstemp(old_late_path);
  entered_create_fd=mkstemp(entered_create_path);
  entered_alarm_fd=mkstemp(entered_alarm_path);
  entered_step_fd=mkstemp(entered_step_path);
  package_fd=mkstemp(package_path);
  if(transition_fd<0 || old_late_fd<0 || entered_create_fd<0 ||
     entered_alarm_fd<0 || entered_step_fd<0 || package_fd<0)
    goto cleanup;
  close(transition_fd); transition_fd=-1;
  close(old_late_fd); old_late_fd=-1;
  close(entered_create_fd); entered_create_fd=-1;
  close(entered_alarm_fd); entered_alarm_fd=-1;
  close(entered_step_fd); entered_step_fd=-1;
  close(package_fd); package_fd=-1;
  if(!fixture_write_text(transition_path,"room_goto_next();\n") ||
     !fixture_write_text(old_late_path,"global.old_begin_hits += 1;\n") ||
     !fixture_write_text(entered_create_path,"alarm[0]=1;\n") ||
     !fixture_write_text(entered_alarm_path,"global.target_alarm_hits += 1;\n") ||
     !fixture_write_text(entered_step_path,"global.target_step_hits += 1;\n"))
    goto cleanup;

  services.struct_size=sizeof services;
  services.abi_version=ANYGM_HOST_SERVICES_VERSION;
  anygm_stdio_vfs_services_init(&services);
  project.name="begin-step-room-continuation-fixture";
  project.host=&services;
  project.objects=objects;
  project.n_objects=project.cap_objects=3;
  project.rooms=rooms;
  project.n_rooms=project.cap_rooms=2;
  project.room_order=room_order;
  project.n_room_order=2;

  objects[0].id=objects[0].name=(char*)"obj_transition";
  objects[0].sprite_id=objects[0].mask_id=objects[0].parent_id=-1;
  objects[0].visible=objects[0].persistent=1;
  objects[0].events=&events[0];
  objects[0].n_events=objects[0].cap_events=1;
  events[0].event_type=3;
  events[0].event_number=1;
  events[0].source_path=transition_path;

  objects[1].id=objects[1].name=(char*)"obj_old_late";
  objects[1].sprite_id=objects[1].mask_id=objects[1].parent_id=-1;
  objects[1].visible=1;
  objects[1].events=&events[1];
  objects[1].n_events=objects[1].cap_events=1;
  events[1].event_type=3;
  events[1].event_number=1;
  events[1].source_path=old_late_path;

  objects[2].id=objects[2].name=(char*)"obj_entered";
  objects[2].sprite_id=objects[2].mask_id=objects[2].parent_id=-1;
  objects[2].visible=1;
  objects[2].events=&events[2];
  objects[2].n_events=objects[2].cap_events=3;
  events[2].event_type=0;
  events[2].event_number=0;
  events[2].source_path=entered_create_path;
  events[3].event_type=2;
  events[3].event_number=0;
  events[3].source_path=entered_alarm_path;
  events[4].event_type=3;
  events[4].event_number=0;
  events[4].source_path=entered_step_path;

  for(int index=0;index<2;index++){
    rooms[index].id=rooms[index].name=index?(char*)"room_target":(char*)"room_source";
    rooms[index].width=320;
    rooms[index].height=240;
    rooms[index].speed=30;
  }
  placed[0].id=placed[0].name=(char*)"placed_transition";
  placed[0].object_id=0;
  placed[0].instance_id=100000;
  placed[1].id=placed[1].name=(char*)"placed_old_late";
  placed[1].object_id=1;
  placed[1].instance_id=100001;
  rooms[0].instances=&placed[0];
  rooms[0].n_instances=rooms[0].cap_instances=2;
  placed[2].id=placed[2].name=(char*)"placed_entered";
  placed[2].object_id=2;
  placed[2].instance_id=100002;
  rooms[1].instances=&placed[2];
  rooms[1].n_instances=rooms[1].cap_instances=1;

  {
    char error[256]={0};
    if(!gmlc_package_write_structural(&project,package_path,error,sizeof error)){
      fprintf(stderr,"Begin Step room continuation package failed: %s\n",error);
      goto cleanup;
    }
  }
  {
    GmlWin win;
    if(anygm_stdio_load_win(&win,package_path)) goto cleanup;
    win.bytecode=16;
    AnygmContentFacts facts={0};
    AnygmCompatibilityProfile profile={0};
    char error[256]={0};
    if(!anygm_content_facts_detect(&win,&facts,error,sizeof error) ||
       !anygm_compatibility_resolve(&facts,&profile,error,sizeof error)){
      fprintf(stderr,"Begin Step room continuation profile failed: %s\n",error);
      gml_win_free(&win);
      goto cleanup;
    }
    win.compatibility=&profile;
    GmlVM vm;
    if(gml_vm_init(&vm,&win,&services)){
      win.compatibility=NULL;
      gml_win_free(&win);
      goto cleanup;
    }
    gml_room_enter(&vm,0);
    *gml_varmap_put(&vm.globals,"old_begin_hits")=vreal(0);
    *gml_varmap_put(&vm.globals,"target_alarm_hits")=vreal(0);
    *gml_varmap_put(&vm.globals,"target_step_hits")=vreal(0);
    gml_vm_step(&vm);
    GmlVal *old_hits=gml_varmap_get(&vm.globals,"old_begin_hits");
    GmlVal *alarm_hits=gml_varmap_get(&vm.globals,"target_alarm_hits");
    GmlVal *step_hits=gml_varmap_get(&vm.globals,"target_step_hits");
    ok=vm.room_index==1 && old_hits && old_hits->t==V_REAL && old_hits->d==0 &&
      alarm_hits && alarm_hits->t==V_REAL && alarm_hits->d==1 &&
      step_hits && step_hits->t==V_REAL && step_hits->d==1;
    if(!ok)
      fprintf(stderr,
        "Studio Begin Step room continuation mismatch: room=%d old=%.0f alarm=%.0f step=%.0f\n",
        vm.room_index,old_hits&&old_hits->t==V_REAL?old_hits->d:-1.0,
        alarm_hits&&alarm_hits->t==V_REAL?alarm_hits->d:-1.0,
        step_hits&&step_hits->t==V_REAL?step_hits->d:-1.0);
    gml_vm_free(&vm);
    win.compatibility=NULL;
    gml_win_free(&win);
  }

cleanup:
  if(transition_fd>=0) close(transition_fd);
  if(old_late_fd>=0) close(old_late_fd);
  if(entered_create_fd>=0) close(entered_create_fd);
  if(entered_alarm_fd>=0) close(entered_alarm_fd);
  if(entered_step_fd>=0) close(entered_step_fd);
  if(package_fd>=0) close(package_fd);
  unlink(transition_path);
  unlink(old_late_path);
  unlink(entered_create_path);
  unlink(entered_alarm_path);
  unlink(entered_step_path);
  unlink(package_path);
  return ok;
}


int expect_event_boundary_room_transition(void){
  return expect_event_boundary_room_transition_mode(0,0) &&
         expect_event_boundary_room_transition_mode(1,0) &&
         expect_event_boundary_room_transition_mode(1,1) &&
         expect_event_boundary_room_transition_mode(1,2) &&
         expect_studio_begin_step_transition_continuation();
}


int expect_room_transition_animation_phase(void){
  GmlcProject project={0};
  GmlcSprite project_sprite={0};
  GmlcObject objects[2]={{0}};
  GmlcObjectEvent event={0};
  GmlcRoom rooms[2]={{0}};
  GmlcRoomInstance placed[2]={{0}};
  int room_order[2]={0,1};
  AnygmHostServices services={0};
  char source_path[]="/tmp/gml-room-animation-phase-source-XXXXXX";
  char package_path[]="/tmp/gml-room-animation-phase-package-XXXXXX";
  int source_fd=-1,package_fd=-1;
  int ok=0;

  source_fd=mkstemp(source_path);
  package_fd=mkstemp(package_path);
  if(source_fd<0 || package_fd<0) goto cleanup;
  close(source_fd); source_fd=-1;
  close(package_fd); package_fd=-1;
  if(!fixture_write_text(source_path,"room_goto_next();\n")) goto cleanup;

  services.struct_size=sizeof services;
  services.abi_version=ANYGM_HOST_SERVICES_VERSION;
  anygm_stdio_vfs_services_init(&services);
  project.name="room-animation-phase-fixture";
  project.host=&services;
  project.sprites=&project_sprite;
  project.n_sprites=project.cap_sprites=1;
  project.objects=objects;
  project.n_objects=project.cap_objects=2;
  project.rooms=rooms;
  project.n_rooms=project.cap_rooms=2;
  project.room_order=room_order;
  project.n_room_order=2;

  project_sprite.id=project_sprite.name=(char*)"spr_phase";
  project_sprite.runtime_id=0;
  project_sprite.width=project_sprite.height=1;
  project_sprite.bbox_right=project_sprite.bbox_bottom=0;

  objects[0].id=objects[0].name=(char*)"obj_carried";
  objects[0].sprite_id=0;
  objects[0].mask_id=objects[0].parent_id=-1;
  objects[0].visible=1;
  objects[0].persistent=1;
  objects[0].events=&event;
  objects[0].n_events=objects[0].cap_events=1;
  event.event_type=3;
  event.event_number=0;
  event.source_path=source_path;

  objects[1].id=objects[1].name=(char*)"obj_entered";
  objects[1].sprite_id=0;
  objects[1].mask_id=objects[1].parent_id=-1;
  objects[1].visible=1;

  for(int index=0;index<2;index++){
    rooms[index].id=rooms[index].name=index?(char*)"room_target":(char*)"room_source";
    rooms[index].width=320;
    rooms[index].height=240;
    rooms[index].speed=60;
  }
  placed[0].id=placed[0].name=(char*)"placed_carried";
  placed[0].object_id=0;
  placed[0].instance_id=100000;
  placed[1].id=placed[1].name=(char*)"placed_entered";
  placed[1].object_id=1;
  placed[1].instance_id=100001;
  rooms[0].instances=&placed[0];
  rooms[0].n_instances=rooms[0].cap_instances=1;
  rooms[1].instances=&placed[1];
  rooms[1].n_instances=rooms[1].cap_instances=1;

  {
    char error[256]={0};
    if(!gmlc_package_write_structural(&project,package_path,error,sizeof error)){
      fprintf(stderr,"room animation phase package failed: %s\n",error);
      goto cleanup;
    }
  }
  {
    GmlWin win;
    if(anygm_stdio_load_win(&win,package_path)) goto cleanup;
    GmlVM vm;
    if(gml_vm_init(&vm,&win,&services)){
      gml_win_free(&win);
      goto cleanup;
    }
    GmlSprite sprite={0};
    GmlRender render={0};
    sprite.n_frames=5;
    render.win=&win;
    render.spr=&sprite;
    render.n_spr=1;
    vm.render=&render;
    gml_room_enter(&vm,0);
    gml_vm_step(&vm);
    GmlInstance *carried=find_slot(&vm,100000);
    GmlInstance *entered=find_slot(&vm,100001);
    ok=vm.room_index==1 && carried && carried->active && entered && entered->active &&
       carried->image_index==1 && entered->image_index==0;
    if(!ok)
      fprintf(stderr,
        "Studio Step room animation phase mismatch: room=%d carried=%.2f entered=%.2f\n",
        vm.room_index,
        carried?carried->image_index:-1.0,entered?entered->image_index:-1.0);
    vm.render=NULL;
    gml_vm_free(&vm);
    gml_win_free(&win);
  }

cleanup:
  if(source_fd>=0) close(source_fd);
  if(package_fd>=0) close(package_fd);
  unlink(source_path);
  unlink(package_path);
  return ok;
}


int expect_animation_end_room_transition_reuses_step_slot(void){
  GmlcProject project={0};
  GmlcSprite project_sprite={0};
  GmlcObject objects[4]={{0}};
  GmlcObjectEvent events[4]={{0}};
  GmlcRoom rooms[2]={{0}};
  GmlcRoomInstance placed[4]={{0}};
  int room_order[2]={0,1};
  AnygmHostServices services={0};
  char transition_path[]="/tmp/gml-animation-room-transition-XXXXXX";
  char entered_step_path[]="/tmp/gml-animation-room-entered-step-XXXXXX";
  char new_type_step_path[]="/tmp/gml-animation-room-new-type-step-XXXXXX";
  char inherited_step_path[]="/tmp/gml-animation-room-inherited-step-XXXXXX";
  char package_path[]="/tmp/gml-animation-room-package-XXXXXX";
  int transition_fd=-1,entered_step_fd=-1,new_type_step_fd=-1,inherited_step_fd=-1,package_fd=-1;
  int ok=0;

  transition_fd=mkstemp(transition_path);
  entered_step_fd=mkstemp(entered_step_path);
  new_type_step_fd=mkstemp(new_type_step_path);
  inherited_step_fd=mkstemp(inherited_step_path);
  package_fd=mkstemp(package_path);
  if(transition_fd<0 || entered_step_fd<0 || new_type_step_fd<0 ||
     inherited_step_fd<0 || package_fd<0) goto cleanup;
  close(transition_fd); transition_fd=-1;
  close(entered_step_fd); entered_step_fd=-1;
  close(new_type_step_fd); new_type_step_fd=-1;
  close(inherited_step_fd); inherited_step_fd=-1;
  close(package_fd); package_fd=-1;
  if(!fixture_write_text(transition_path,"room_goto_next();\n") ||
     !fixture_write_text(entered_step_path,
       "global.entered_step_hits = global.entered_step_hits + 1;\n"
       "global.step_order = global.step_order * 10 + 1;\n") ||
     !fixture_write_text(new_type_step_path,
       "global.new_type_step_hits = global.new_type_step_hits + 1;\n"
       "global.step_order = global.step_order * 10 + 2;\n") ||
     !fixture_write_text(inherited_step_path,
       "global.inherited_step_hits = global.inherited_step_hits + 1;\n")) goto cleanup;

  services.struct_size=sizeof services;
  services.abi_version=ANYGM_HOST_SERVICES_VERSION;
  anygm_stdio_vfs_services_init(&services);
  project.name="animation-room-transition-fixture";
  project.host=&services;
  project.sprites=&project_sprite;
  project.n_sprites=project.cap_sprites=1;
  project.objects=objects;
  project.n_objects=project.cap_objects=4;
  project.rooms=rooms;
  project.n_rooms=project.cap_rooms=2;
  project.room_order=room_order;
  project.n_room_order=2;

  project_sprite.id=project_sprite.name=(char*)"spr_transition";
  project_sprite.runtime_id=0;
  project_sprite.width=project_sprite.height=1;
  project_sprite.bbox_right=project_sprite.bbox_bottom=0;

  objects[0].id=objects[0].name=(char*)"obj_transition";
  objects[0].sprite_id=0;
  objects[0].mask_id=objects[0].parent_id=-1;
  objects[0].visible=1;
  objects[0].events=&events[0];
  objects[0].n_events=objects[0].cap_events=2;
  events[0].event_type=7;
  events[0].event_number=7;
  events[0].source_path=transition_path;
  events[1].event_type=3;
  events[1].event_number=0;
  events[1].source_path=entered_step_path;

  objects[1].id=objects[1].name=(char*)"obj_direct_entered";
  objects[1].sprite_id=0;
  objects[1].mask_id=objects[1].parent_id=-1;
  objects[1].visible=1;
  objects[1].events=&events[2];
  objects[1].n_events=objects[1].cap_events=1;
  events[2].event_type=3;
  events[2].event_number=0;
  events[2].source_path=new_type_step_path;

  objects[2].id=objects[2].name=(char*)"obj_inherited_handler";
  objects[2].sprite_id=0;
  objects[2].mask_id=objects[2].parent_id=-1;
  objects[2].visible=1;
  objects[2].events=&events[3];
  objects[2].n_events=objects[2].cap_events=1;
  events[3].event_type=3;
  events[3].event_number=0;
  events[3].source_path=inherited_step_path;

  objects[3].id=objects[3].name=(char*)"obj_inherited_entered";
  objects[3].sprite_id=0;
  objects[3].mask_id=-1;
  objects[3].parent_id=2;
  objects[3].visible=1;

  for(int index=0;index<2;index++){
    rooms[index].id=rooms[index].name=index?(char*)"room_target":(char*)"room_source";
    rooms[index].width=320;
    rooms[index].height=240;
    rooms[index].speed=60;
  }
  placed[0].id=placed[0].name=(char*)"placed_transition";
  placed[0].object_id=0;
  placed[0].instance_id=100000;
  placed[1].id=placed[1].name=(char*)"placed_new_type";
  placed[1].object_id=1;
  placed[1].instance_id=100002;
  placed[2].id=placed[2].name=(char*)"placed_entered";
  placed[2].object_id=0;
  placed[2].instance_id=100001;
  placed[3].id=placed[3].name=(char*)"placed_inherited_type";
  placed[3].object_id=3;
  placed[3].instance_id=100003;
  rooms[0].instances=&placed[0];
  rooms[0].n_instances=rooms[0].cap_instances=1;
  rooms[1].instances=&placed[1];
  rooms[1].n_instances=rooms[1].cap_instances=3;

  {
    char error[256]={0};
    if(!gmlc_package_write_structural(&project,package_path,error,sizeof error)){
      fprintf(stderr,"animation room transition package failed: %s\n",error);
      goto cleanup;
    }
  }
  {
    GmlWin win;
    if(anygm_stdio_load_win(&win,package_path)) goto cleanup;
    GmlVM vm;
    if(gml_vm_init(&vm,&win,&services)){
      gml_win_free(&win);
      goto cleanup;
    }
    GmlSprite sprite={0};
    GmlRender render={0};
    sprite.n_frames=2;
    render.win=&win;
    render.spr=&sprite;
    render.n_spr=1;
    vm.render=&render;
    gml_room_enter(&vm,0);
    *gml_varmap_put(&vm.globals,"entered_step_hits")=vreal(0);
    *gml_varmap_put(&vm.globals,"new_type_step_hits")=vreal(0);
    *gml_varmap_put(&vm.globals,"inherited_step_hits")=vreal(0);
    *gml_varmap_put(&vm.globals,"step_order")=vreal(0);
    GmlInstance *source=find_slot(&vm,100000);
    if(source){
      source->image_index=1;
      source->image_speed=1;
      gml_vm_step(&vm);
    }
    GmlInstance *entered=find_slot(&vm,100001);
    GmlVal *hits=gml_varmap_get(&vm.globals,"entered_step_hits");
    GmlVal *new_type_hits=gml_varmap_get(&vm.globals,"new_type_step_hits");
    GmlVal *inherited_hits=gml_varmap_get(&vm.globals,"inherited_step_hits");
    GmlVal *step_order=gml_varmap_get(&vm.globals,"step_order");
    int first_ok=vm.room_index==1 && entered && entered->active &&
      entered->image_index==1 && hits && hits->t==V_REAL && hits->d==1 &&
      new_type_hits && new_type_hits->t==V_REAL && new_type_hits->d==1 &&
      inherited_hits && inherited_hits->t==V_REAL && inherited_hits->d==0 &&
      step_order && step_order->t==V_REAL && step_order->d==21;
    if(first_ok){
      gml_vm_step(&vm);
      entered=find_slot(&vm,100001);
      hits=gml_varmap_get(&vm.globals,"entered_step_hits");
      new_type_hits=gml_varmap_get(&vm.globals,"new_type_step_hits");
      inherited_hits=gml_varmap_get(&vm.globals,"inherited_step_hits");
    }
    ok=first_ok && entered && entered->active &&
      hits && hits->t==V_REAL && hits->d==2 &&
      new_type_hits && new_type_hits->t==V_REAL && new_type_hits->d==2 &&
      inherited_hits && inherited_hits->t==V_REAL && inherited_hits->d==1;
    if(!ok)
      fprintf(stderr,
        "Animation End room transition phase mismatch: room=%d entered=%d index=%.2f reused=%.0f direct=%.0f inherited=%.0f order=%.0f\n",
        vm.room_index,entered&&entered->active,entered?entered->image_index:-1.0,
        hits&&hits->t==V_REAL?hits->d:-1.0,
        new_type_hits&&new_type_hits->t==V_REAL?new_type_hits->d:-1.0,
        inherited_hits&&inherited_hits->t==V_REAL?inherited_hits->d:-1.0,
        step_order&&step_order->t==V_REAL?step_order->d:-1.0);
    vm.render=NULL;
    gml_vm_free(&vm);
    gml_win_free(&win);
  }

cleanup:
  if(transition_fd>=0) close(transition_fd);
  if(entered_step_fd>=0) close(entered_step_fd);
  if(new_type_step_fd>=0) close(new_type_step_fd);
  if(inherited_step_fd>=0) close(inherited_step_fd);
  if(package_fd>=0) close(package_fd);
  unlink(transition_path);
  unlink(entered_step_path);
  unlink(new_type_step_path);
  unlink(inherited_step_path);
  unlink(package_path);
  return ok;
}


int expect_frozen_animation_wrap_fires_animation_end(void){
  /* This synthetic fixture verifies that an out-of-range frozen non-classic index
   * wraps and dispatches Animation End once, while an in-range frozen index
   * neither moves nor dispatches the event. */
  GmlcProject project={0};
  GmlcSprite project_sprite={0};
  GmlcObject object={0};
  GmlcObjectEvent event={0};
  GmlcRoom room={0};
  GmlcRoomInstance placed={0};
  int room_order[1]={0};
  AnygmHostServices services={0};
  char source_path[]="/tmp/gml-frozen-animation-wrap-source-XXXXXX";
  char package_path[]="/tmp/gml-frozen-animation-wrap-package-XXXXXX";
  int source_fd=-1,package_fd=-1;
  int ok=0;

  source_fd=mkstemp(source_path);
  package_fd=mkstemp(package_path);
  if(source_fd<0 || package_fd<0) goto cleanup;
  close(source_fd); source_fd=-1;
  close(package_fd); package_fd=-1;
  if(!fixture_write_text(source_path,
       "global.frozen_wrap_ends = global.frozen_wrap_ends + 1;\n")) goto cleanup;

  services.struct_size=sizeof services;
  services.abi_version=ANYGM_HOST_SERVICES_VERSION;
  anygm_stdio_vfs_services_init(&services);
  project.name="frozen-animation-wrap-fixture";
  project.host=&services;
  project.sprites=&project_sprite;
  project.n_sprites=project.cap_sprites=1;
  project.objects=&object;
  project.n_objects=project.cap_objects=1;
  project.rooms=&room;
  project.n_rooms=project.cap_rooms=1;
  project.room_order=room_order;
  project.n_room_order=1;

  project_sprite.id=project_sprite.name=(char*)"spr_frozen";
  project_sprite.runtime_id=0;
  project_sprite.width=project_sprite.height=1;
  project_sprite.bbox_right=project_sprite.bbox_bottom=0;

  object.id=object.name=(char*)"obj_frozen";
  object.sprite_id=0;
  object.mask_id=object.parent_id=-1;
  object.visible=1;
  object.events=&event;
  object.n_events=object.cap_events=1;
  event.event_type=7;      /* Other */
  event.event_number=7;    /* Animation End */
  event.source_path=source_path;

  room.id=room.name=(char*)"room_frozen";
  room.width=320;
  room.height=240;
  room.speed=60;
  placed.id=placed.name=(char*)"placed_frozen";
  placed.object_id=0;
  placed.instance_id=100000;
  room.instances=&placed;
  room.n_instances=room.cap_instances=1;

  {
    char error[256]={0};
    if(!gmlc_package_write_structural(&project,package_path,error,sizeof error)){
      fprintf(stderr,"frozen animation wrap package failed: %s\n",error);
      goto cleanup;
    }
  }
  {
    GmlWin win;
    if(anygm_stdio_load_win(&win,package_path)) goto cleanup;
    GmlVM vm;
    if(gml_vm_init(&vm,&win,&services)){
      gml_win_free(&win);
      goto cleanup;
    }
    GmlSprite sprite={0};
    GmlRender render={0};
    sprite.n_frames=2;
    render.win=&win;
    render.spr=&sprite;
    render.n_spr=1;
    vm.render=&render;
    gml_room_enter(&vm,0);
    *gml_varmap_put(&vm.globals,"frozen_wrap_ends")=vreal(0);
    GmlInstance *frozen=find_slot(&vm,100000);
    if(frozen){
      /* frozen inside the sprite: nothing may fire and the index must hold */
      frozen->image_speed=0;
      frozen->image_index=1;
      gml_vm_step(&vm);
      frozen=find_slot(&vm,100000);
    }
    GmlVal *ends=gml_varmap_get(&vm.globals,"frozen_wrap_ends");
    int held=frozen && frozen->image_index==1.0 &&
             ends && ends->t==V_REAL && ends->d==0;
    if(frozen){
      /* frozen one past the last frame: wrap to the first frame and fire exactly once */
      frozen->image_index=2;
      gml_vm_step(&vm);
      frozen=find_slot(&vm,100000);
      gml_vm_step(&vm);
      frozen=find_slot(&vm,100000);
    }
    ends=gml_varmap_get(&vm.globals,"frozen_wrap_ends");
    ok=held && frozen && frozen->image_index==0.0 && frozen->image_speed==0 &&
       ends && ends->t==V_REAL && ends->d==1;
    if(!ok)
      fprintf(stderr,
        "frozen animation wrap mismatch: held=%d index=%.2f speed=%.2f ends=%.0f\n",
        held,frozen?frozen->image_index:-1.0,frozen?frozen->image_speed:-1.0,
        ends&&ends->t==V_REAL?ends->d:-1.0);
    vm.render=NULL;
    gml_vm_free(&vm);
    gml_win_free(&win);
  }

cleanup:
  if(source_fd>=0) close(source_fd);
  if(package_fd>=0) close(package_fd);
  unlink(source_path);
  unlink(package_path);
  return ok;
}


int expect_event_perform_dispatches_mouse_event(void){
  GmlcProject project={0};
  GmlcObject object={0};
  GmlcObjectEvent events[2]={{0}};
  GmlcRoom room={0};
  GmlcRoomInstance placed={0};
  int room_order[1]={0};
  AnygmHostServices services={0};
  char create_path[]="/tmp/gml-event-perform-create-XXXXXX";
  char mouse_path[]="/tmp/gml-event-perform-mouse-XXXXXX";
  char package_path[]="/tmp/gml-event-perform-package-XXXXXX";
  int create_fd=-1,mouse_fd=-1,package_fd=-1;
  int ok=0;

  create_fd=mkstemp(create_path);
  mouse_fd=mkstemp(mouse_path);
  package_fd=mkstemp(package_path);
  if(create_fd<0 || mouse_fd<0 || package_fd<0) goto cleanup;
  close(create_fd); create_fd=-1;
  close(mouse_fd); mouse_fd=-1;
  close(package_fd); package_fd=-1;
  if(!fixture_write_text(create_path,
       "global.performed_mouse_hits=0; event_perform(ev_mouse,ev_left_press);\n") ||
     !fixture_write_text(mouse_path,
       "global.performed_mouse_hits+=1;\n"))
    goto cleanup;

  services.struct_size=sizeof services;
  services.abi_version=ANYGM_HOST_SERVICES_VERSION;
  anygm_stdio_vfs_services_init(&services);
  project.name="event-perform-mouse-fixture";
  project.host=&services;
  project.classic_version=800;
  project.classic_executable_layout=1;
  project.objects=&object;
  project.n_objects=project.cap_objects=1;
  project.rooms=&room;
  project.n_rooms=project.cap_rooms=1;
  project.room_order=room_order;
  project.n_room_order=1;

  object.id=object.name=(char*)"obj_event_probe";
  object.sprite_id=object.mask_id=object.parent_id=-1;
  object.visible=1;
  object.events=events;
  object.n_events=object.cap_events=2;
  events[0].event_type=0;
  events[0].event_number=0;
  events[0].source_path=create_path;
  events[1].event_type=6;
  events[1].event_number=4;
  events[1].source_path=mouse_path;

  room.id=room.name=(char*)"room_event_probe";
  room.width=320;
  room.height=240;
  room.speed=60;
  placed.id=placed.name=(char*)"placed_event_probe";
  placed.object_id=0;
  placed.instance_id=100000;
  room.instances=&placed;
  room.n_instances=room.cap_instances=1;

  {
    char error[256]={0};
    if(!gmlc_package_write_structural(&project,package_path,error,sizeof error)){
      fprintf(stderr,"event_perform mouse package failed: %s\n",error);
      goto cleanup;
    }
  }
  {
    GmlWin win;
    if(anygm_stdio_load_win(&win,package_path)) goto cleanup;
    GmlVM vm;
    if(gml_vm_init(&vm,&win,&services)){
      gml_win_free(&win);
      goto cleanup;
    }
    gml_room_enter(&vm,0);
    GmlVal *hits=gml_varmap_get(&vm.globals,"performed_mouse_hits");
    ok=hits && hits->t==V_REAL && hits->d==1;
    if(!ok)
      fprintf(stderr,"event_perform did not dispatch Mouse_4: hits=%.0f\n",
        hits&&hits->t==V_REAL?hits->d:-1.0);
    gml_vm_free(&vm);
    gml_win_free(&win);
  }

cleanup:
  if(create_fd>=0) close(create_fd);
  if(mouse_fd>=0) close(mouse_fd);
  if(package_fd>=0) close(package_fd);
  unlink(create_path);
  unlink(mouse_path);
  unlink(package_path);
  return ok;
}

int expect_event_starts_with_the_relative_flag_clear(void){
  /* A synthetic nested Create event must start with a clear action-relative flag even when its caller set the flag. The caller's flag resumes afterward. The fixture pins absolute and relative movement separately. */
  GmlcProject project={0};
  GmlcObject objects[2]={{0},{0}};
  GmlcObjectEvent events[2]={{0},{0}};
  GmlcRoom room={0};
  GmlcRoomInstance placed={0};
  int room_order[1]={0};
  AnygmHostServices services={0};
  char spawned_path[]="/tmp/gml-relative-spawned-source-XXXXXX";
  char maker_path[]="/tmp/gml-relative-maker-source-XXXXXX";
  char package_path[]="/tmp/gml-relative-package-XXXXXX";
  int spawned_fd=-1,maker_fd=-1,package_fd=-1;
  int ok=0;

  spawned_fd=mkstemp(spawned_path);
  maker_fd=mkstemp(maker_path);
  package_fd=mkstemp(package_path);
  if(spawned_fd<0 || maker_fd<0 || package_fd<0) goto cleanup;
  close(spawned_fd); spawned_fd=-1;
  close(maker_fd); maker_fd=-1;
  close(package_fd); package_fd=-1;
  /* The created instance's own list: one absolute jump, stated the way a normalized list states an
   * action that did not ask for relative - by saying nothing about the flag at all. */
  if(!fixture_write_text(spawned_path,
       "action_move_to(100, 40);\n"
       "global.spawned_x = x;\n"
       "global.spawned_y = y;\n")) goto cleanup;
  /* The creating list: relative, create, and one more relative action after the create. */
  if(!fixture_write_text(maker_path,
       "action_set_relative(1);\n"
       "action_create_object(0, 0, 0);\n"
       "action_move_to(10, 10);\n"
       "action_set_relative(0);\n"
       "global.maker_x = x;\n"
       "global.maker_y = y;\n")) goto cleanup;

  services.struct_size=sizeof services;
  services.abi_version=ANYGM_HOST_SERVICES_VERSION;
  anygm_stdio_vfs_services_init(&services);
  project.name="event-relative-flag-fixture";
  project.host=&services;
  project.objects=objects;
  project.n_objects=project.cap_objects=2;
  project.rooms=&room;
  project.n_rooms=project.cap_rooms=1;
  project.room_order=room_order;
  project.n_room_order=1;

  objects[0].id=objects[0].name=(char*)"obj_spawned";
  objects[0].sprite_id=objects[0].mask_id=objects[0].parent_id=-1;
  objects[0].visible=1;
  objects[0].events=&events[0];
  objects[0].n_events=objects[0].cap_events=1;
  events[0].event_type=0;    /* Create */
  events[0].event_number=0;
  events[0].source_path=spawned_path;

  objects[1].id=objects[1].name=(char*)"obj_maker";
  objects[1].sprite_id=objects[1].mask_id=objects[1].parent_id=-1;
  objects[1].visible=1;
  objects[1].events=&events[1];
  objects[1].n_events=objects[1].cap_events=1;
  events[1].event_type=0;    /* Create */
  events[1].event_number=0;
  events[1].source_path=maker_path;

  room.id=room.name=(char*)"room_relative";
  room.width=320;
  room.height=240;
  room.speed=60;
  placed.id=placed.name=(char*)"placed_maker";
  placed.object_id=1;
  placed.instance_id=100000;
  placed.x=200;
  placed.y=120;
  room.instances=&placed;
  room.n_instances=room.cap_instances=1;

  {
    char error[256]={0};
    if(!gmlc_package_write_structural(&project,package_path,error,sizeof error)){
      fprintf(stderr,"event relative flag package failed: %s\n",error);
      goto cleanup;
    }
  }
  {
    GmlWin win;
    if(anygm_stdio_load_win(&win,package_path)) goto cleanup;
    GmlVM vm;
    if(gml_vm_init(&vm,&win,&services)){
      gml_win_free(&win);
      goto cleanup;
    }
    gml_room_enter(&vm,0);
    GmlVal *spawned_x=gml_varmap_get(&vm.globals,"spawned_x");
    GmlVal *spawned_y=gml_varmap_get(&vm.globals,"spawned_y");
    GmlVal *maker_x=gml_varmap_get(&vm.globals,"maker_x");
    GmlVal *maker_y=gml_varmap_get(&vm.globals,"maker_y");
    ok=spawned_x && spawned_x->t==V_REAL && spawned_x->d==100 &&
       spawned_y && spawned_y->t==V_REAL && spawned_y->d==40 &&
       maker_x && maker_x->t==V_REAL && maker_x->d==210 &&
       maker_y && maker_y->t==V_REAL && maker_y->d==130;
    if(!ok)
      fprintf(stderr,
        "event relative flag mismatch: spawned=(%.2f,%.2f) expected (100,40), "
        "maker=(%.2f,%.2f) expected (210,130)\n",
        spawned_x&&spawned_x->t==V_REAL?spawned_x->d:-1.0,
        spawned_y&&spawned_y->t==V_REAL?spawned_y->d:-1.0,
        maker_x&&maker_x->t==V_REAL?maker_x->d:-1.0,
        maker_y&&maker_y->t==V_REAL?maker_y->d:-1.0);
    gml_vm_free(&vm);
    gml_win_free(&win);
  }

cleanup:
  if(spawned_fd>=0) close(spawned_fd);
  if(maker_fd>=0) close(maker_fd);
  if(package_fd>=0) close(package_fd);
  unlink(spawned_path);
  unlink(maker_path);
  unlink(package_path);
  return ok;
}

int expect_stopped_mover_restored_from_solid(void){
  /* This synthetic contact fixture leaves a stopped mover inside a stationary
   * solid. The post-event fallback restores its pre-contact position. */
  GmlcProject project={0};
  GmlcSprite project_sprite={0};
  GmlcObject objects[2]={{0}};
  GmlcObjectEvent event={0};
  GmlcRoom room={0};
  GmlcRoomInstance placed[2]={{0}};
  int room_order[1]={0};
  AnygmHostServices services={0};
  char source_path[]="/tmp/gml-solid-stop-restore-source-XXXXXX";
  char package_path[]="/tmp/gml-solid-stop-restore-package-XXXXXX";
  int source_fd=-1,package_fd=-1;
  int ok=0;

  source_fd=mkstemp(source_path);
  package_fd=mkstemp(package_path);
  if(source_fd<0 || package_fd<0) goto cleanup;
  close(source_fd); source_fd=-1;
  close(package_fd); package_fd=-1;
  if(!fixture_write_text(source_path,
       "global.solid_stops = global.solid_stops + 1;\n"
       "vspeed = 0; speed = 0; gravity = 0; y = other.y;\n")) goto cleanup;

  services.struct_size=sizeof services;
  services.abi_version=ANYGM_HOST_SERVICES_VERSION;
  anygm_stdio_vfs_services_init(&services);
  project.name="solid-stop-restore-fixture";
  project.host=&services;
  project.sprites=&project_sprite;
  project.n_sprites=project.cap_sprites=1;
  project.objects=objects;
  project.n_objects=project.cap_objects=2;
  project.rooms=&room;
  project.n_rooms=project.cap_rooms=1;
  project.room_order=room_order;
  project.n_room_order=1;

  project_sprite.id=project_sprite.name=(char*)"spr_solid_stop";
  project_sprite.runtime_id=0;
  project_sprite.width=project_sprite.height=16;
  project_sprite.bbox_right=project_sprite.bbox_bottom=15;

  objects[0].id=objects[0].name=(char*)"obj_faller";
  objects[0].sprite_id=0;
  objects[0].mask_id=objects[0].parent_id=-1;
  objects[0].visible=1;
  objects[0].events=&event;
  objects[0].n_events=objects[0].cap_events=1;
  event.event_type=4;              /* Collision */
  event.collision_object_id=1;     /* with obj_block */
  event.source_path=source_path;

  objects[1].id=objects[1].name=(char*)"obj_block";
  objects[1].sprite_id=0;
  objects[1].mask_id=objects[1].parent_id=-1;
  objects[1].visible=1;
  objects[1].solid=1;

  room.id=room.name=(char*)"room_solid_stop";
  room.width=320;
  room.height=240;
  room.speed=60;
  placed[0].id=placed[0].name=(char*)"placed_faller";
  placed[0].object_id=0;
  placed[0].instance_id=100000;
  placed[0].x=100;
  placed[0].y=100;
  placed[1].id=placed[1].name=(char*)"placed_block";
  placed[1].object_id=1;
  placed[1].instance_id=100001;
  placed[1].x=100;
  placed[1].y=140;
  room.instances=placed;
  room.n_instances=room.cap_instances=2;

  {
    char error[256]={0};
    if(!gmlc_package_write_structural(&project,package_path,error,sizeof error)){
      fprintf(stderr,"solid stop restore package failed: %s\n",error);
      goto cleanup;
    }
  }
  {
    GmlWin win;
    if(anygm_stdio_load_win(&win,package_path)) goto cleanup;
    GmlVM vm;
    if(gml_vm_init(&vm,&win,&services)){
      gml_win_free(&win);
      goto cleanup;
    }
    GmlSprite sprite={0};
    GmlRender render={0};
    sprite.n_frames=1;
    sprite.w=sprite.h=16;
    sprite.mr=sprite.mb=15;
    render.win=&win;
    render.spr=&sprite;
    render.n_spr=1;
    vm.render=&render;
    gml_room_enter(&vm,0);
    *gml_varmap_put(&vm.globals,"solid_stops")=vreal(0);
    GmlInstance *faller=find_slot(&vm,100000);
    if(faller){
      faller->vspeed=4;
      gml_colgrid_invalidate(&vm);
      /* 100 -> 104 ... -> 124 free; the step to 128 overlaps the block at 140..155 */
      for(int frame=0;frame<7 && faller;frame++){
        gml_vm_step(&vm);
        faller=find_slot(&vm,100000);
      }
    }
    GmlVal *stops=gml_varmap_get(&vm.globals,"solid_stops");
    ok=faller && faller->y==124.0 && faller->vspeed==0 && faller->gravity==0 &&
       stops && stops->t==V_REAL && stops->d==1;
    if(!ok)
      fprintf(stderr,
        "stopped mover restore mismatch: y=%.2f vspeed=%.2f gravity=%.2f stops=%.0f\n",
        faller?faller->y:-1.0,faller?faller->vspeed:-1.0,faller?faller->gravity:-1.0,
        stops&&stops->t==V_REAL?stops->d:-1.0);
    vm.render=NULL;
    gml_vm_free(&vm);
    gml_win_free(&win);
  }

cleanup:
  if(source_fd>=0) close(source_fd);
  if(package_fd>=0) close(package_fd);
  unlink(source_path);
  unlink(package_path);
  return ok;
}


int expect_stationary_embed_nudge_reverted(void){
  /* This synthetic stationary-overlap fixture dispatches six Collision events.
   * Each event nudges a stopped handler by one pixel; post-event restoration
   * keeps its frame-start position fixed while the event count advances. */
  GmlcProject project={0};
  GmlcSprite project_sprite={0};
  GmlcObject objects[2]={{0}};
  GmlcObjectEvent event={0};
  GmlcRoom room={0};
  GmlcRoomInstance placed[2]={{0}};
  int room_order[1]={0};
  AnygmHostServices services={0};
  char source_path[]="/tmp/gml-embed-nudge-source-XXXXXX";
  char package_path[]="/tmp/gml-embed-nudge-package-XXXXXX";
  int source_fd=-1,package_fd=-1;
  int ok=0;

  source_fd=mkstemp(source_path);
  package_fd=mkstemp(package_path);
  if(source_fd<0 || package_fd<0) goto cleanup;
  close(source_fd); source_fd=-1;
  close(package_fd); package_fd=-1;
  if(!fixture_write_text(source_path,
       "global.nudges = global.nudges + 1;\n"
       "y = y - 1;\n")) goto cleanup;

  services.struct_size=sizeof services;
  services.abi_version=ANYGM_HOST_SERVICES_VERSION;
  anygm_stdio_vfs_services_init(&services);
  project.name="embed-nudge-fixture";
  project.host=&services;
  project.sprites=&project_sprite;
  project.n_sprites=project.cap_sprites=1;
  project.objects=objects;
  project.n_objects=project.cap_objects=2;
  project.rooms=&room;
  project.n_rooms=project.cap_rooms=1;
  project.room_order=room_order;
  project.n_room_order=1;

  project_sprite.id=project_sprite.name=(char*)"spr_embed_nudge";
  project_sprite.runtime_id=0;
  project_sprite.width=project_sprite.height=16;
  project_sprite.bbox_right=project_sprite.bbox_bottom=15;

  objects[0].id=objects[0].name=(char*)"obj_stander";
  objects[0].sprite_id=0;
  objects[0].mask_id=objects[0].parent_id=-1;
  objects[0].visible=1;
  objects[0].events=&event;
  objects[0].n_events=objects[0].cap_events=1;
  event.event_type=4;              /* Collision */
  event.collision_object_id=1;     /* with obj_block */
  event.source_path=source_path;

  objects[1].id=objects[1].name=(char*)"obj_block";
  objects[1].sprite_id=0;
  objects[1].mask_id=objects[1].parent_id=-1;
  objects[1].visible=1;
  objects[1].solid=1;

  room.id=room.name=(char*)"room_embed_nudge";
  room.width=320;
  room.height=240;
  room.speed=60;
  placed[0].id=placed[0].name=(char*)"placed_stander";
  placed[0].object_id=0;
  placed[0].instance_id=100000;
  placed[0].x=100;
  placed[0].y=100;
  placed[1].id=placed[1].name=(char*)"placed_block";
  placed[1].object_id=1;
  placed[1].instance_id=100001;
  placed[1].x=100;
  placed[1].y=108;
  room.instances=placed;
  room.n_instances=room.cap_instances=2;

  {
    char error[256]={0};
    if(!gmlc_package_write_structural(&project,package_path,error,sizeof error)){
      fprintf(stderr,"embed nudge package failed: %s\n",error);
      goto cleanup;
    }
  }
  {
    GmlWin win;
    if(anygm_stdio_load_win(&win,package_path)) goto cleanup;
    GmlVM vm;
    if(gml_vm_init(&vm,&win,&services)){
      gml_win_free(&win);
      goto cleanup;
    }
    GmlSprite sprite={0};
    GmlRender render={0};
    sprite.n_frames=1;
    sprite.w=sprite.h=16;
    sprite.mr=sprite.mb=15;
    render.win=&win;
    render.spr=&sprite;
    render.n_spr=1;
    vm.render=&render;
    gml_room_enter(&vm,0);
    *gml_varmap_put(&vm.globals,"nudges")=vreal(0);
    GmlInstance *stander=find_slot(&vm,100000);
    if(stander){
      gml_colgrid_invalidate(&vm);
      /* embedded rows 108..115 the whole time: the event fires every step and its pixel of
       * displacement is restored every step */
      for(int frame=0;frame<6 && stander;frame++){
        gml_vm_step(&vm);
        stander=find_slot(&vm,100000);
      }
    }
    GmlVal *nudges=gml_varmap_get(&vm.globals,"nudges");
    ok=stander && stander->y==100.0 && stander->x==100.0 &&
       nudges && nudges->t==V_REAL && nudges->d==6;
    if(!ok)
      fprintf(stderr,
        "stationary embed nudge mismatch: x=%.2f y=%.2f nudges=%.0f\n",
        stander?stander->x:-1.0,stander?stander->y:-1.0,
        nudges&&nudges->t==V_REAL?nudges->d:-1.0);
    vm.render=NULL;
    gml_vm_free(&vm);
    gml_win_free(&win);
  }

cleanup:
  if(source_fd>=0) close(source_fd);
  if(package_fd>=0) close(package_fd);
  unlink(source_path);
  unlink(package_path);
  return ok;
}


int expect_hash_layer_gpu_gap_closure(void){
  GmlVM vm={0}; GmlRender render={0};
  vm.render=&render;
  GmlVal ascii=vstr("abc"), euro=vstr("\342\202\254");
  GmlVal md5u=gml_builtin_call(&vm,"md5_string_unicode",&ascii,1);
  GmlVal sha8=gml_builtin_call(&vm,"sha1_string_utf8",&ascii,1);
  GmlVal shau=gml_builtin_call(&vm,"sha1_string_unicode",&ascii,1);
  GmlVal euro_u=gml_builtin_call(&vm,"sha1_string_unicode",&euro,1);
  int ok=md5u.t==V_STR && md5u.s && !strcmp(md5u.s,"ce1473cf80c6b3fda8e3dfc006adc315") &&
         sha8.t==V_STR && sha8.s && !strcmp(sha8.s,"a9993e364706816aba3e25717850c26c9cd0d89d") &&
         shau.t==V_STR && shau.s && !strcmp(shau.s,"9f04f41a848514162050e3d68c1a7abb441dc2b5") &&
         euro_u.t==V_STR && euro_u.s && !strcmp(euro_u.s,"cb73f8a8dc63786596be419db0c652c9b70c9421");
  if(md5u.d!=0) free((void*)md5u.s);
  if(sha8.d!=0) free((void*)sha8.s);
  if(shau.d!=0) free((void*)shau.s);
  if(euro_u.d!=0) free((void*)euro_u.s);

  vm.rtl=calloc(1,sizeof(*vm.rtl)); vm.inst=calloc(3,sizeof(*vm.inst));
  if(!vm.rtl || !vm.inst){ free(vm.rtl); free(vm.inst); return 0; }
  vm.n_rtl=vm.cap_rtl=1; vm.inst_count=vm.inst_cap=3;
  vm.rtl[0].used=1; vm.rtl[0].id=23; vm.rtl[0].order=7;
  snprintf(vm.rtl[0].name,sizeof vm.rtl[0].name,"Actors");
  vm.inst[0].active=1; vm.inst[0].draw_layer_order=7;
  vm.inst[1].active=1; vm.inst[1].draw_layer_order=8;
  vm.inst[2].deactivated=1; vm.inst[2].draw_layer_order=7;
  GmlVal layer_name=vstr("Actors");
  (void)gml_builtin_call(&vm,"instance_deactivate_layer",&layer_name,1);
  ok=ok && !vm.inst[0].active && vm.inst[0].deactivated && vm.inst[1].active && vm.inst[2].deactivated;
  GmlVal layer_id=vreal(23);
  (void)gml_builtin_call(&vm,"instance_activate_layer",&layer_id,1);
  ok=ok && vm.inst[0].active && !vm.inst[0].deactivated && vm.inst[1].active &&
     vm.inst[2].active && !vm.inst[2].deactivated;

  GmlVal alpha_ref=vreal(511),alpha_on=vreal(1);
  (void)gml_builtin_call(&vm,"gpu_set_alphatestref",&alpha_ref,1);
  (void)gml_builtin_call(&vm,"gpu_set_alphatestenable",&alpha_on,1);
  (void)gml_builtin_call(&vm,"gpu_push_state",NULL,0);
  alpha_ref=vreal(3); alpha_on=vreal(0);
  (void)gml_builtin_call(&vm,"gpu_set_alphatestref",&alpha_ref,1);
  (void)gml_builtin_call(&vm,"gpu_set_alphatestenable",&alpha_on,1);
  (void)gml_builtin_call(&vm,"gpu_pop_state",NULL,0);
  GmlVal ref=gml_builtin_call(&vm,"gpu_get_alphatestref",NULL,0);
  GmlVal enabled=gml_builtin_call(&vm,"gpu_get_alphatestenable",NULL,0);
  GmlVal shaders=gml_builtin_call(&vm,"shaders_are_supported",NULL,0);
  ok=ok && ref.t==V_REAL && ref.d==255 && enabled.t==V_REAL && enabled.d==1 &&
     shaders.t==V_REAL && shaders.d==1;

  render.interp=1;
  GmlVal texfilter_ext[2]={vreal(37),vreal(0)};
  (void)gml_builtin_call(&vm,"gpu_set_texfilter_ext",texfilter_ext,2);
  ok=ok && render.interp==0;
  GmlVal texfilter=vreal(1);
  (void)gml_builtin_call(&vm,"gpu_set_texfilter",&texfilter,1);
  GmlVal texfilter_get=gml_builtin_call(&vm,"gpu_get_texfilter",NULL,0);
  int filter_ok=render.interp==1 && texfilter_get.t==V_REAL && texfilter_get.d==1;
  ok=ok && filter_ok;
  GmlVal texrepeat[2]={vreal(17),vreal(1)};
  GmlVal texrepeat_result=gml_builtin_call(&vm,"gpu_set_texrepeat",texrepeat,2);
  ok=ok && texrepeat_result.t==V_REAL && texrepeat_result.d==0;

  GmlVal create_view[10]={vreal(12),vreal(34),vreal(320),vreal(180),vreal(7),
                          vreal(100042),vreal(5),vreal(6),vreal(9),vreal(10)};
  GmlVal source_camera=gml_builtin_call(&vm,"camera_create_view",create_view,10);
  GmlVal target_camera=gml_builtin_call(&vm,"camera_create",NULL,0);
  GmlVal copy_camera[2]={target_camera,source_camera};
  (void)gml_builtin_call(&vm,"camera_copy_transforms",copy_camera,2);
  GmlVal camera_x=gml_builtin_call(&vm,"camera_get_view_x",&target_camera,1);
  GmlVal camera_y=gml_builtin_call(&vm,"camera_get_view_y",&target_camera,1);
  GmlVal camera_w=gml_builtin_call(&vm,"camera_get_view_width",&target_camera,1);
  GmlVal camera_h=gml_builtin_call(&vm,"camera_get_view_height",&target_camera,1);
  GmlVal camera_angle=gml_builtin_call(&vm,"camera_get_view_angle",&target_camera,1);
  GmlVal camera_target=gml_builtin_call(&vm,"camera_get_view_target",&target_camera,1);
  GmlVal camera_bx=gml_builtin_call(&vm,"camera_get_view_border_x",&target_camera,1);
  GmlVal camera_by=gml_builtin_call(&vm,"camera_get_view_border_y",&target_camera,1);
  GmlVal window_min=vreal(240);
  GmlVal min_result=gml_builtin_call(&vm,"window_set_min_width",&window_min,1);
  int camera_ok=source_camera.d>=0 && target_camera.d>=0 && camera_x.d==12 && camera_y.d==34 &&
    camera_w.d==320 && camera_h.d==180 && camera_angle.d==7 && camera_target.d==100042 &&
    camera_bx.d==9 && camera_by.d==10 && min_result.t==V_REAL && min_result.d==0;
  ok=ok && camera_ok;

  GmlVal lookat_args[9]={vreal(132),vreal(91),vreal(-10),vreal(132),vreal(91),vreal(0),
                          vreal(0),vreal(1),vreal(0)};
  GmlVal ortho_args[4]={vreal(400),vreal(300),vreal(1),vreal(10000)};
  GmlVal view_matrix=gml_builtin_call(&vm,"matrix_build_lookat",lookat_args,9);
  GmlVal projection_matrix=gml_builtin_call(&vm,"matrix_build_projection_ortho",ortho_args,4);
  GmlVal set_view_matrix[2]={target_camera,view_matrix};
  GmlVal set_projection_matrix[2]={target_camera,projection_matrix};
  (void)gml_builtin_call(&vm,"camera_set_view_mat",set_view_matrix,2);
  (void)gml_builtin_call(&vm,"camera_set_proj_mat",set_projection_matrix,2);
  camera_x=gml_builtin_call(&vm,"camera_get_view_x",&target_camera,1);
  camera_y=gml_builtin_call(&vm,"camera_get_view_y",&target_camera,1);
  camera_w=gml_builtin_call(&vm,"camera_get_view_width",&target_camera,1);
  camera_h=gml_builtin_call(&vm,"camera_get_view_height",&target_camera,1);
  int matrix_camera_ok=view_matrix.t==V_ARR && projection_matrix.t==V_ARR &&
    fabs(gml_arr_get(projection_matrix,0).d-.005)<1e-12 &&
    fabs(gml_arr_get(projection_matrix,5).d-(2.0/300.0))<1e-12 &&
    fabs(camera_x.d-(-68))<1e-9 && fabs(camera_y.d-(-59))<1e-9 &&
    fabs(camera_w.d-400)<1e-9 && fabs(camera_h.d-300)<1e-9;
  ok=ok && matrix_camera_ok;

  GmlVal layer_shader_args[2]={layer_name,vreal(2)};
  (void)gml_builtin_call(&vm,"layer_shader",layer_shader_args,2);
  GmlVal layer_shader=gml_builtin_call(&vm,"layer_get_shader",&layer_name,1);
  int layer_shader_ok=layer_shader.t==V_REAL && layer_shader.d==2 &&
    gml_global_arr(&vm,"__gml_layer_shader",0)==3;
  ok=ok && layer_shader_ok;

  uint32_t source[2]={0x7FFF0000u,0xFFFF0000u};
  uint32_t target[2]={0xFF102030u,0xFF102030u};
  struct GmlShaderPal alpha_shader={0};
  alpha_shader.alpha_discard=1; alpha_shader.alpha_discard_cutoff=0.99f;
  gml_render_begin(&render,target,2,1,0,0);
  render.alphablend=1; render.alpha_test_enable=0;
  render.app_surface=source; render.app_w=2; render.app_h=1;
  render.shader_pal=&alpha_shader; render.n_shader_pal=1; render.active_shader=0;
  gml_draw_surface_stretched(&render,0,0,0,2,1,0xFFFFFF,1);
  int surface_alpha_ok=target[0]==0xFF102030u && target[1]==0xFFFF0000u;
  ok=ok && surface_alpha_ok;

  render.shader_pal=NULL; render.n_shader_pal=0; render.app_surface=NULL;

  /* Object setters mutate the asset default for future instances, not instances already alive. */
  GmlObject object={0}; object.visible=1;
  vm.objects=&object; vm.n_objects=1; vm.inst[0].visible=1;
  GmlVal object_visible_args[2]={vreal(0),vreal(0)};
  (void)gml_builtin_call(&vm,"object_set_visible",object_visible_args,2);
  GmlVal object_id=vreal(0);
  GmlVal object_visible=gml_builtin_call(&vm,"object_get_visible",&object_id,1);
  ok=ok && object.visible==0 && vm.inst[0].visible==1 &&
     object_visible.t==V_REAL && object_visible.d==0;
  vm.objects=NULL; vm.n_objects=0;
  free(vm.rtl); free(vm.inst);
  if(!ok) fprintf(stderr,"hash/layer/GPU gap-closure fixture failed (filter=%d camera=%d matrix=%d layer_shader=%d interp=%d get=%.0f surface=%d pixels=%08X,%08X)\n",
    filter_ok,camera_ok,matrix_camera_ok,layer_shader_ok,render.interp,texfilter_get.t==V_REAL?texfilter_get.d:-1.0,
    surface_alpha_ok,target[0],target[1]);
  return ok;
}


int expect_array_function_gap_closure(void){
  GmlVM vm={0}; vm.math_epsilon=1e-5;
  GmlVal array=gml_arr_new(2,vreal(0));
  gml_arr_set(array,0,vreal(1)); gml_arr_set(array,1,vreal(4));
  GmlVal insert[4]={array,vreal(1),vreal(2),vreal(3)};
  (void)gml_builtin_call(&vm,"array_insert",insert,4);
  int ok=gml_val_array_length(array)==4 && gml_arr_get(array,0).d==1 &&
    gml_arr_get(array,1).d==2 && gml_arr_get(array,2).d==3 && gml_arr_get(array,3).d==4;
  GmlVal gap[3]={array,vreal(6),vreal(9)};
  (void)gml_builtin_call(&vm,"array_insert",gap,3);
  ok=ok && gml_val_array_length(array)==7 && gml_arr_get(array,4).d==0 &&
    gml_arr_get(array,5).d==0 && gml_arr_get(array,6).d==9;

  GmlVal negative=gml_arr_new(3,vreal(0));
  for(int i=0;i<3;i++) gml_arr_set(negative,i,vreal(i+1));
  GmlVal before_last[3]={negative,vreal(-1),vreal(8)};
  (void)gml_builtin_call(&vm,"array_insert",before_last,3);
  ok=ok && gml_val_array_length(negative)==4 && gml_arr_get(negative,2).d==8 &&
    gml_arr_get(negative,3).d==3;

  GmlVal nested_a=gml_arr_new(2,vreal(0)), nested_b=gml_arr_new(2,vreal(0));
  gml_arr_set(nested_a,0,vstr("value")); gml_arr_set(nested_b,0,vstr("value"));
  gml_arr_set(nested_a,1,array); gml_arr_set(nested_b,1,array);
  GmlVal equal_args[2]={nested_a,nested_b};
  GmlVal equal=gml_builtin_call(&vm,"array_equals",equal_args,2);
  gml_arr_set(nested_b,0,vstr("different"));
  GmlVal different=gml_builtin_call(&vm,"array_equals",equal_args,2);
  GmlVal nan_a=gml_arr_new(1,vreal(NAN)), nan_b=gml_arr_new(1,vreal(NAN));
  GmlVal nan_args[2]={nan_a,nan_b};
  GmlVal nan_equal=gml_builtin_call(&vm,"array_equals",nan_args,2);
  ok=ok && equal.t==V_REAL && equal.d==1 && different.t==V_REAL && different.d==0 &&
    nan_equal.t==V_REAL && nan_equal.d==0;

  GmlVal union_left=gml_arr_new(4,vreal(0));
  gml_arr_set(union_left,0,vreal(4));
  gml_arr_set(union_left,1,vstr("label"));
  gml_arr_set(union_left,2,vreal(4));
  gml_arr_set(union_left,3,vreal(7));
  GmlVal union_right=gml_arr_new(3,vreal(0));
  gml_arr_set(union_right,0,vstr("label"));
  gml_arr_set(union_right,1,vreal(9));
  gml_arr_set(union_right,2,vreal(7));
  GmlVal union_args[2]={union_left,union_right};
  GmlVal union_result=gml_builtin_call(&vm,"array_union",union_args,2);
  GmlVal contains_args[2]={union_result,vreal(4)};
  GmlVal contains_four=gml_builtin_call(&vm,"array_contains",contains_args,2);
  contains_args[1]=vstr("label");
  GmlVal contains_label=gml_builtin_call(&vm,"array_contains",contains_args,2);
  contains_args[1]=vreal(9);
  GmlVal contains_nine=gml_builtin_call(&vm,"array_contains",contains_args,2);
  ok=ok && union_result.t==V_ARR && gml_val_array_length(union_result)==4 &&
    contains_four.t==V_REAL && contains_four.d==1 &&
    contains_label.t==V_REAL && contains_label.d==1 &&
    contains_nine.t==V_REAL && contains_nine.d==1 &&
    gml_val_array_length(union_left)==4 && gml_val_array_length(union_right)==3;

  /* A chained store creates intermediate containers lazily. Keep this neutral three-dimensional
   * matrix fixture separate from array_set_2D: pushac/popaf chaining represents each dimension
   * as a nested array. */
  GmlVal matrix=gml_arr_new(0,vreal(0));
  GmlVal language=gml_arr_chain_ensure(matrix,7);
  GmlVal section=gml_arr_chain_ensure(language,1);
  gml_arr_set(section,2,vstr("label"));
  GmlVal language_read=gml_arr_get(matrix,7);
  GmlVal section_read=gml_arr_get(language_read,1);
  GmlVal label=gml_arr_get(section_read,2);
  gml_arr_set(matrix,3,vreal(9));
  GmlVal scalar=gml_arr_chain_ensure(matrix,3);
  ok=ok && language.t==V_ARR && section.t==V_ARR &&
    label.t==V_STR && label.s && !strcmp(label.s,"label") &&
    scalar.t==V_REAL && scalar.d==9;
  GmlVal ordered=gml_arr_new(5,vreal(0));
  for(int i=0;i<5;i++) gml_arr_set(ordered,i,vreal(i+1));
  gml_rng_seed(&vm,12345);
  GmlVal shuffled=gml_builtin_call(&vm,"array_shuffle",&ordered,1);
  int seen[6]={0};
  if(shuffled.t==V_ARR && shuffled.arr && gml_val_array_length(shuffled)==5)
    for(int i=0;i<5;i++){
      int value=(int)gml_arr_get(shuffled,i).d;
      if(value>=1 && value<=5) seen[value]++;
    }
  ok=ok && gml_arr_get(ordered,0).d==1 && gml_arr_get(ordered,4).d==5;
  for(int i=1;i<=5;i++) ok=ok && seen[i]==1;
  GmlVal range_args[3]={ordered,vreal(-2),vreal(-2)};
  GmlVal range=gml_builtin_call(&vm,"array_shuffle",range_args,3);
  double sum=0;
  if(range.t==V_ARR && range.arr)
    for(int i=0;i<gml_val_array_length(range);i++) sum+=gml_arr_get(range,i).d;
  ok=ok && gml_val_array_length(range)==2 && sum==7; /* source indices 3 then 2: values 4+3 */
  if(!ok) fprintf(stderr,"array insert/equivalence fixture failed\n");
  return ok;
}


static int expect_array_compound_shape(int retained_reference){
  unsigned char data[128]={0};
  uint32_t reference_addresses[2]={0};
  const char *reference_names[2]={"argument0","argument0"};
  int word=0,reference_count=0;
#define EMIT_WORD(value) fixture_word(data,word++,(uint32_t)(value))
#define EMIT_ARGUMENT() do { \
  EMIT_WORD(0xC1000000u); \
  reference_addresses[reference_count++]=(uint32_t)word*4u; \
  EMIT_WORD(0xA0000000u); \
} while(0)
  EMIT_ARGUMENT();
  EMIT_WORD((OP_PUSH<<24)|(DT_INT16<<16));
  EMIT_WORD((OP_CONV<<24)|(((DT_INT32<<4)|DT_VAR)<<16));
  if(retained_reference) EMIT_WORD((OP_DUP<<24)|(DT_INT32<<16)|4u);
  EMIT_WORD((OP_BREAK<<24)|(DT_INT16<<16)|(uint16_t)-8);
  EMIT_WORD((OP_BREAK<<24)|(DT_INT16<<16)|(uint16_t)-2);
  EMIT_WORD((OP_PUSH<<24)|(DT_INT16<<16)|7u);
  EMIT_WORD((OP_CONV<<24)|(((DT_VAR<<4)|DT_INT16)<<16));
  EMIT_WORD((OP_SUB<<24)|(((DT_VAR<<4)|DT_VAR)<<16));
  EMIT_WORD((OP_BREAK<<24)|(DT_INT16<<16)|(uint16_t)-9);
  if(retained_reference) EMIT_WORD((OP_DUP<<24)|(DT_INT32<<16)|0xA804u);
  EMIT_WORD((OP_BREAK<<24)|(DT_INT16<<16)|(uint16_t)-3);
  EMIT_ARGUMENT();
  EMIT_WORD((OP_PUSH<<24)|(DT_INT16<<16));
  EMIT_WORD((OP_BREAK<<24)|(DT_INT16<<16)|(uint16_t)-2);
  EMIT_WORD((OP_RET<<24)|(DT_VAR<<16));
#undef EMIT_ARGUMENT
#undef EMIT_WORD

  GmlCode code={0};
  code.name=(char*)(retained_reference
    ?"gml_Script_compound_retained_fixture"
    :"gml_Script_compound_saved_fixture");
  code.start=0;
  code.length=(uint32_t)word*4u;
  GmlWin win={0};
  win.data=data;
  win.size=sizeof data;
  win.bytecode=17;
  win.code=&code;
  win.n_code=1;
  win.ref_addr=reference_addresses;
  win.ref_name=reference_names;
  win.n_refs=reference_count;
  GmlVM vm={0};
  vm.win=&win;
  vm.cur_code_index=-1;
  vm.math_epsilon=1e-5;
  GmlVal array=gml_arr_new(1,vreal(100));
  GmlVal result=gml_vm_run_code(&vm,0,NULL,NULL,&array,1);
  int ok=result.t==V_REAL && result.d==93 &&
    gml_arr_get(array,0).t==V_REAL && gml_arr_get(array,0).d==93;
  if(!ok) fprintf(stderr,"compound array fixture failed: retained=%d result=%.0f stored=%.0f\n",
    retained_reference,result.t==V_REAL?result.d:-1.0,gml_arr_get(array,0).d);
  gml_values_release(&array,1);
  free(code.insn);
  free(code.insn_pc);
  free(code.branch_index);
  free(win.ref_hix);
  return ok;
}


/* DUP measures encoded stack bytes, not logical values. Exercise mixed-width
 * duplication, zero-argument member shuffles and compound-array shapes. */
int expect_typed_stack_dup(void){
  unsigned char data[64]={0};
  const uint32_t normal[]={
    (OP_PUSH<<24)|(DT_INT16<<16)|10u,
    (OP_CONV<<24)|(((DT_VAR<<4)|DT_INT16)<<16),
    (OP_PUSH<<24)|(DT_INT16<<16)|20u,
    (OP_PUSH<<24)|(DT_INT16<<16)|30u,
    (OP_DUP<<24)|(DT_INT32<<16)|5u,
    (OP_POPZ<<24)|(DT_INT16<<16),
    (OP_POPZ<<24)|(DT_INT16<<16),
    (OP_POPZ<<24)|(DT_VAR<<16),
    (OP_ADD<<24)|(((DT_INT16<<4)|DT_INT16)<<16),
    (OP_ADD<<24)|(((DT_VAR<<4)|DT_INT16)<<16),
    (OP_RET<<24)|(DT_VAR<<16)
  };
  for(int i=0;i<(int)(sizeof normal/sizeof *normal);i++) fixture_word(data,i,normal[i]);
  GmlCode code={0}; code.name=(char*)"gml_Script_typed_stack_fixture";
  code.start=0; code.length=(uint32_t)sizeof normal;
  GmlWin win={0}; win.data=data; win.size=sizeof data; win.bytecode=17;
  win.code=&code; win.n_code=1;
  GmlVM vm={0}; vm.win=&win; vm.cur_code_index=-1; vm.math_epsilon=1e-5;
  GmlVal sum=gml_vm_run_code(&vm,0,NULL,NULL,NULL,0);
  free(code.insn); free(code.insn_pc); free(code.branch_index);

  memset(data,0,sizeof data); memset(&code,0,sizeof code);
  const uint32_t swapped[]={
    (OP_PUSH<<24)|(DT_INT16<<16)|1u,
    (OP_CONV<<24)|(((DT_VAR<<4)|DT_INT16)<<16),
    (OP_PUSH<<24)|(DT_INT16<<16)|2u,
    (OP_CONV<<24)|(((DT_VAR<<4)|DT_INT16)<<16),
    (OP_DUP<<24)|(DT_VAR<<16)|0x8801u,
    (OP_SUB<<24)|(((DT_VAR<<4)|DT_VAR)<<16),
    (OP_RET<<24)|(DT_VAR<<16)
  };
  for(int i=0;i<(int)(sizeof swapped/sizeof *swapped);i++) fixture_word(data,i,swapped[i]);
  code.name=(char*)"gml_Script_typed_stack_swap_fixture";
  code.start=0; code.length=(uint32_t)sizeof swapped;
  win.data=data; win.size=sizeof data; win.code=&code;
  GmlVal difference=gml_vm_run_code(&vm,0,NULL,NULL,NULL,0);
  free(code.insn); free(code.insn_pc); free(code.branch_index);

  memset(data,0,sizeof data); memset(&code,0,sizeof code);
  const uint32_t zero_argument_shuffle[]={
    (OP_PUSH<<24)|(DT_INT16<<16)|7u,                  /* enclosing loop counter */
    (OP_PUSH<<24)|(DT_INT16<<16)|9u,                  /* member receiver */
    (OP_CONV<<24)|(((DT_VAR<<4)|DT_INT16)<<16),
    (OP_DUP<<24)|(DT_VAR<<16)|0x8800u,                /* move receiver over zero args */
    (OP_POPZ<<24)|(DT_VAR<<16),                       /* stand in for the completed call */
    (OP_RET<<24)|(DT_VAR<<16)
  };
  for(int i=0;i<(int)(sizeof zero_argument_shuffle/sizeof *zero_argument_shuffle);i++)
    fixture_word(data,i,zero_argument_shuffle[i]);
  code.name=(char*)"gml_Script_zero_argument_member_shuffle_fixture";
  code.start=0; code.length=(uint32_t)sizeof zero_argument_shuffle;
  win.data=data; win.size=sizeof data; win.code=&code;
  GmlVal retained_counter=gml_vm_run_code(&vm,0,NULL,NULL,NULL,0);
  free(code.insn); free(code.insn_pc); free(code.branch_index);
  int retained_ok=expect_array_compound_shape(1);
  int saved_ok=expect_array_compound_shape(0);
  int ok=sum.t==V_REAL && sum.d==60 && difference.t==V_REAL && difference.d==1 &&
    retained_counter.t==V_REAL && retained_counter.d==7 && retained_ok && saved_ok;
  if(!ok) fprintf(stderr,
                  "typed stack DUP fixture failed: sum=%.0f swap=%.0f zero=%.0f retained=%d saved=%d\n",
                  sum.t==V_REAL?sum.d:-1.0,difference.t==V_REAL?difference.d:-1.0,
                  retained_counter.t==V_REAL?retained_counter.d:-1.0,retained_ok,saved_ok);
  return ok;
}

int expect_member_callable_receiver(void){
  unsigned char data[24]={0};
  fixture_word(data,0,(OP_PUSH<<24)|(DT_VAR<<16)|(uint16_t)IT_ARG);
  fixture_word(data,1,0xA0000000u);
  fixture_word(data,2,(OP_PUSH<<24)|(DT_VAR<<16)|(uint16_t)IT_SELF);
  fixture_word(data,3,0xA0000000u);
  fixture_word(data,4,(OP_MUL<<24)|((DT_VAR<<4)|DT_VAR)<<16);
  fixture_word(data,5,(OP_RET<<24)|(DT_VAR<<16));
  uint32_t reference_addresses[2]={4,12};
  const char *reference_names[2]={"argument0","factor"};
  GmlCode code={0};
  code.name=(char*)"gml_Script_member_receiver_fixture";
  code.start=0;
  code.length=sizeof data;
  GmlWin win={0};
  win.data=data;
  win.size=sizeof data;
  win.bytecode=17;
  win.code=&code;
  win.n_code=1;
  win.ref_addr=reference_addresses;
  win.ref_name=reference_names;
  win.n_refs=2;
  GmlVM vm={0};
  vm.win=&win;
  vm.cur_code_index=-1;
  vm.math_epsilon=1e-5;
  GmlInstance *receiver=gml_struct_new(&vm);
  GmlInstance *caller=gml_struct_new(&vm);
  if(!receiver || !caller) return 0;
  *gml_varmap_put(&receiver->vars,"factor")=vreal(5);
  *gml_varmap_put(&caller->vars,"factor")=vreal(99);
  vm.cur_self=caller;
  GmlVal callable=vreal((double)(GML_FUNCVAL_TAG|0));
  GmlVal argument=vreal(2);
  GmlVal direct=gml_vm_call_member_callable(
    &vm,vreal((double)receiver->id),callable,&argument,1);
  GmlVal static_arguments[2]={vreal(-16),callable};
  GmlVal static_method=gml_builtin_call(&vm,"method",static_arguments,2);
  GmlVal late_bound=gml_vm_call_member_callable(
    &vm,vreal((double)receiver->id),static_method,&argument,1);
  GmlVal bound_arguments[2]={vreal((double)caller->id),callable};
  GmlVal bound_method=gml_builtin_call(&vm,"method",bound_arguments,2);
  GmlVal explicitly_bound=gml_vm_call_member_callable(
    &vm,vreal((double)receiver->id),bound_method,&argument,1);
  int ok=direct.t==V_REAL && direct.d==10 &&
    late_bound.t==V_REAL && late_bound.d==10 &&
    explicitly_bound.t==V_REAL && explicitly_bound.d==198;
  if(!ok)
    fprintf(stderr,
      "member callable receiver fixture failed: direct=%.0f static=%.0f bound=%.0f\n",
      direct.t==V_REAL?direct.d:-1.0,
      late_bound.t==V_REAL?late_bound.d:-1.0,
      explicitly_bound.t==V_REAL?explicitly_bound.d:-1.0);
  gml_vm_free(&vm);
  free(code.insn);
  free(code.insn_pc);
  free(code.branch_index);
  free(win.ref_hix);
  return ok;
}

int expect_builtin_numeric_constants(void){
  GmlVM vm={0};
  GmlVal infinity=gml_vm_identifier_get(&vm,"infinity");
  int ok=infinity.t==V_REAL && isinf(infinity.d) && infinity.d>0;
  if(!ok)
    fprintf(stderr,"builtin infinity constant mismatch: type=%d value=%.17g\n",
            (int)infinity.t,infinity.t==V_REAL?infinity.d:0.0);
  free(vm.special_var_hash);
  return ok;
}

static void smooth_path_fixture_f32(uint8_t *data,size_t offset,float value){
  uint32_t bits=0;
  memcpy(&bits,&value,sizeof bits);
  fixture_w32(data,offset,bits);
}

int expect_smooth_path_midpoint_interpolation(void){
  uint8_t data[128]={0};
  fixture_w32(data,0,1);
  fixture_w32(data,4,16);
  fixture_w32(data,20,1);
  fixture_w32(data,24,0);
  fixture_w32(data,28,4);
  fixture_w32(data,32,3);
  const float points[9]={
    0,0,100,
    100,100,100,
    200,0,100,
  };
  for(size_t index=0;index<sizeof points/sizeof points[0];index++)
    smooth_path_fixture_f32(data,36+index*4,points[index]);

  GmlWin win={0};
  win.data=data;
  win.size=sizeof data;
  win.bytecode=15;
  win.game_speed=60;
  win.n_chunks=1;
  memcpy(win.chunks[0].name,"PATH",5);
  win.chunks[0].off=0;
  win.chunks[0].size=sizeof data;
  GmlVM vm;
  if(gml_vm_init(&vm,&win,NULL)) return 0;
  double x=0,y=0;
  gml_path_eval_public(&vm,0,0.5,&x,&y);
  int ok=fabs(x-100.0)<1e-9 && fabs(y-75.0)<1e-9;
  if(!ok)
    fprintf(stderr,"smooth path midpoint interpolation mismatch: (%.9f,%.9f)\n",x,y);
  gml_vm_free(&vm);
  return ok;
}


static int expect_persistent_lifecycle_exit_code(void){
  GmlcProject project; GmlcObject objects[3]; GmlcRoom rooms[2];
  GmlcScript scripts[4]; char *script_order[4];
  GmlcPath fixture_path; GmlcPathPoint fixture_path_points[2];
  GmlcTimeline timeline; GmlcTimelineMoment timeline_moments[3];
  GmlcRoomInstance placed_instance;
  int room_order[2]={0,1};
  GmlcObjectEvent object_events[26];
  GmlcProjectTrigger trigger;
  GmlcProjectIncludedFile included;
  unsigned char included_data[]={1,3,5,7};
  char included_name[64];
  GmlcProjectConstant constant={(char*)"fixture_constant",(char*)"6*7"};
  memset(&project,0,sizeof(project)); memset(objects,0,sizeof(objects)); memset(rooms,0,sizeof(rooms));
  AnygmHostServices file_services={0};
  file_services.struct_size=sizeof file_services;
  file_services.abi_version=ANYGM_HOST_SERVICES_VERSION;
  anygm_stdio_vfs_services_init(&file_services);
  file_services.development_setting=fixture_development_setting;
  project.host=&file_services;
  memset(scripts,0,sizeof(scripts));
  memset(&fixture_path,0,sizeof(fixture_path)); memset(fixture_path_points,0,sizeof(fixture_path_points));
  memset(&timeline,0,sizeof(timeline)); memset(timeline_moments,0,sizeof(timeline_moments));
  memset(&placed_instance,0,sizeof(placed_instance));
  memset(object_events,0,sizeof(object_events)); memset(&trigger,0,sizeof(trigger)); memset(&included,0,sizeof(included));
  project.name="persistent-room-fixture"; project.objects=objects; project.n_objects=3;
  project.classic_version=800;
  project.classic_executable_layout=1;
  scripts[0].id=scripts[0].name=(char*)"script_implicit_result";
  scripts[1].id=scripts[1].name=(char*)"crear";
  scripts[2].id=scripts[2].name=(char*)"array_ext_callback";
  scripts[3].id=scripts[3].name=(char*)"studio_with_order";
  script_order[0]=scripts[0].id;
  script_order[1]=scripts[1].id;
  script_order[2]=scripts[2].id;
  script_order[3]=scripts[3].id;
  project.scripts=scripts; project.n_scripts=project.cap_scripts=4;
  project.script_order_ids=script_order; project.n_script_order=4;
  fixture_path.id=fixture_path.name=(char*)"fixture_path";
  fixture_path.precision=4; fixture_path.points=fixture_path_points; fixture_path.n_points=2;
  fixture_path_points[0].speed=fixture_path_points[1].speed=100;
  fixture_path_points[1].x=100;
  project.paths=&fixture_path; project.n_paths=project.cap_paths=1;
  timeline.id=timeline.name=(char*)"fixture_timeline";
  timeline.moments=timeline_moments; timeline.n_moments=timeline.cap_moments=3;
  timeline_moments[0].step=0; timeline_moments[1].step=2; timeline_moments[2].step=4;
  project.timelines=&timeline; project.n_timelines=project.cap_timelines=1;
  project.constants=&constant; project.n_constants=project.cap_constants=1;
  project.triggers=&trigger; project.n_triggers=project.cap_triggers=1;
  snprintf(included_name,sizeof(included_name),"gml-included-%ld.dat",(long)getpid());
  included.file_name=included_name; included.data=included_data; included.data_size=sizeof(included_data);
  included.export_mode=2; included.overwrite_file=1;
  project.included_files=&included; project.n_included_files=project.cap_included_files=1;
  project.rooms=rooms; project.n_rooms=2; project.room_order=room_order; project.n_room_order=2;
  objects[0].name="obj_fixture"; objects[0].sprite_id=-1; objects[0].mask_id=-1; objects[0].parent_id=-1; objects[0].visible=1;
  objects[0].events=object_events; objects[0].n_events=objects[0].cap_events=10;
  objects[1].name="obj_changed"; objects[1].sprite_id=-1; objects[1].mask_id=-1; objects[1].parent_id=-1; objects[1].visible=1;
  objects[1].events=&object_events[10]; objects[1].n_events=objects[1].cap_events=11;
  objects[2].name="obj_create_order"; objects[2].sprite_id=-1; objects[2].mask_id=-1; objects[2].parent_id=-1; objects[2].visible=0;
  objects[2].events=&object_events[21]; objects[2].n_events=objects[2].cap_events=5;
  object_events[0].event_type=11; object_events[0].event_number=0;
  object_events[1].event_type=3; object_events[1].event_number=0;
  object_events[2].event_type=2; object_events[2].event_number=0;
  object_events[3].event_type=2; object_events[3].event_number=1;
  object_events[4].event_type=5; object_events[4].event_number=65;
  object_events[5].event_type=6; object_events[5].event_number=50;
  object_events[6].event_type=7; object_events[6].event_number=0;
  object_events[7].event_type=7; object_events[7].event_number=1;
  object_events[8].event_type=7; object_events[8].event_number=41;
  object_events[9].event_type=7; object_events[9].event_number=51;
  object_events[10].event_type=3; object_events[10].event_number=2;
  object_events[11].event_type=3; object_events[11].event_number=0;
  object_events[12].event_type=2; object_events[12].event_number=0;
  object_events[13].event_type=2; object_events[13].event_number=1;
  object_events[14].event_type=5; object_events[14].event_number=65;
  object_events[15].event_type=6; object_events[15].event_number=50;
  object_events[16].event_type=7; object_events[16].event_number=0;
  object_events[17].event_type=7; object_events[17].event_number=1;
  object_events[18].event_type=7; object_events[18].event_number=41;
  object_events[19].event_type=7; object_events[19].event_number=51;
  object_events[20].event_type=11; object_events[20].event_number=0;
  object_events[21].event_type=0; object_events[21].event_number=0;
  object_events[22].event_type=6; object_events[22].event_number=23;
  object_events[23].event_type=4; object_events[23].event_number=1;
  object_events[23].collision_object_id=1;
  object_events[24].event_type=1; object_events[24].event_number=0;
  object_events[25].event_type=12; object_events[25].event_number=0;
  trigger.name=(char*)"fixture_trigger"; trigger.moment=1; trigger.runtime_id=0;
  for(int i=0;i<2;i++){
    rooms[i].name=i?"room_b":"room_a"; rooms[i].width=320; rooms[i].height=240; rooms[i].speed=30;
    rooms[i].view_enabled=1; rooms[i].n_views=2;
    for(int v=0;v<2;v++){
      rooms[i].views[v].wview=rooms[i].views[v].wport=v?100:320;
      rooms[i].views[v].hview=rooms[i].views[v].hport=v?100:240;
      rooms[i].views[v].hspeed=rooms[i].views[v].vspeed=-1;
      rooms[i].views[v].object_id=-1;
    }
  }
  rooms[1].view_enabled=0;
  rooms[0].persistent=1;
  placed_instance.id=(char*)"placed_create_order";
  placed_instance.name=(char*)"placed_create_order";
  placed_instance.object_id=2; placed_instance.instance_id=100000;
  rooms[0].instances=&placed_instance; rooms[0].n_instances=rooms[0].cap_instances=1;
  char startup[]="/tmp/gml-startup-XXXXXX"; int startup_fd=mkstemp(startup); if(startup_fd<0)return 1;
  FILE *startup_file=fdopen(startup_fd,"wb");
  const char startup_source[]=
    "global.startup_value = fixture_constant; "
    "global.date_minute_span_fixture = date_minute_span(100, 99.5); "
    "global.view_fixture_scalar = 7; global.background_fixture_scalar = 9; "
    "view_enabled[0] = true; global.view_enabled_alias = view_enabled[1]; "
    "room_caption = \"fixture caption\"; global.room_caption_fixture = room_caption; "
    "global.delta_fixture = delta_time; "
    "global.working_fixture = working_directory; global.program_fixture = program_directory; "
    "global.fixture_orange = c_orange; global.fixture_rain = ef_rain;\n";
  if(!startup_file || fwrite(startup_source,1,sizeof(startup_source)-1,startup_file)!=sizeof(startup_source)-1 ||
     fclose(startup_file)!=0){ unlink(startup); return 1; }
  project.startup_code_path=startup;
  char implicit_script[]="/tmp/gml-implicit-script-XXXXXX";
  int implicit_script_fd=mkstemp(implicit_script); if(implicit_script_fd<0)return 1;
  FILE *implicit_script_file=fdopen(implicit_script_fd,"wb");
  const char implicit_script_source[]=
    "if (argument0 == -4) return (1 == 1.000001); "
    "transition_kind=12; transition_steps=40; "
    "if (argument0 >= 0) instance_exists(argument0); "
    "else if (argument0 == -2) return all.fixture_all_scope; "
    "else if (argument0 == -3) { all.fixture_all_scope=77; return 77; } "
    "else global.implicit_assignment = 42;\n";
  if(!implicit_script_file ||
     fwrite(implicit_script_source,1,sizeof(implicit_script_source)-1,implicit_script_file)!=sizeof(implicit_script_source)-1 ||
     fclose(implicit_script_file)!=0)return 1;
  scripts[0].source_path=implicit_script;
  char shadowed_alias_script[]="/tmp/gml-shadowed-alias-script-XXXXXX";
  int shadowed_alias_fd=mkstemp(shadowed_alias_script); if(shadowed_alias_fd<0)return 1;
  FILE *shadowed_alias_file=fdopen(shadowed_alias_fd,"wb");
  const char shadowed_alias_source[]="global.user_crear_hits += argument0;\n";
  if(!shadowed_alias_file ||
     fwrite(shadowed_alias_source,1,sizeof(shadowed_alias_source)-1,shadowed_alias_file)!=sizeof(shadowed_alias_source)-1 ||
     fclose(shadowed_alias_file)!=0)return 1;
  scripts[1].source_path=shadowed_alias_script;
  char array_ext_callback[]="/tmp/gml-array-ext-callback-XXXXXX";
  int array_ext_callback_fd=mkstemp(array_ext_callback); if(array_ext_callback_fd<0)return 1;
  FILE *array_ext_callback_file=fdopen(array_ext_callback_fd,"wb");
  const char array_ext_callback_source[]=
    "if (argument_count > 1) global.time_source_fixture += argument1; "
    "return argument0 * factor;\n";
  if(!array_ext_callback_file ||
     fwrite(array_ext_callback_source,1,sizeof(array_ext_callback_source)-1,array_ext_callback_file)!=sizeof(array_ext_callback_source)-1 ||
     fclose(array_ext_callback_file)!=0)return 1;
  scripts[2].source_path=array_ext_callback;
  char studio_with_order[]="/tmp/gml-studio-with-order-XXXXXX";
  int studio_with_order_fd=mkstemp(studio_with_order); if(studio_with_order_fd<0)return 1;
  FILE *studio_with_order_file=fdopen(studio_with_order_fd,"wb");
  const char studio_with_order_source[]=
    "global.with_order=0; with(obj_changed){ "
    "global.with_order=global.with_order*10+x; "
    "if(x==3){ with(global.with_victim) instance_destroy(); } } "
    "return global.with_order;\n";
  if(!studio_with_order_file ||
     fwrite(studio_with_order_source,1,sizeof(studio_with_order_source)-1,studio_with_order_file)!=sizeof(studio_with_order_source)-1 ||
     fclose(studio_with_order_file)!=0)return 1;
  scripts[3].source_path=studio_with_order;
  char condition[]="/tmp/gml-trigger-condition-XXXXXX"; int condition_fd=mkstemp(condition); if(condition_fd<0)return 1;
  FILE *condition_file=fdopen(condition_fd,"wb"); const char condition_source[]="return (global.startup_value == 42);\n";
  if(!condition_file || fwrite(condition_source,1,sizeof(condition_source)-1,condition_file)!=sizeof(condition_source)-1 || fclose(condition_file)!=0)return 1;
  trigger.condition_path=condition;
  char event[]="/tmp/gml-trigger-event-XXXXXX"; int event_fd=mkstemp(event); if(event_fd<0)return 1;
  FILE *event_file=fdopen(event_fd,"wb");
  const char event_source[]="global.trigger_hits += 1; global.trigger_order=global.trigger_order*10+1;\n";
  if(!event_file || fwrite(event_source,1,sizeof(event_source)-1,event_file)!=sizeof(event_source)-1 || fclose(event_file)!=0)return 1;
  object_events[0].source_path=event;
  char changed_trigger[]="/tmp/gml-trigger-changed-XXXXXX"; int changed_trigger_fd=mkstemp(changed_trigger); if(changed_trigger_fd<0)return 1;
  FILE *changed_trigger_file=fdopen(changed_trigger_fd,"wb");
  const char changed_trigger_source[]="global.trigger_hits += 1; global.trigger_order=global.trigger_order*10+2;\n";
  if(!changed_trigger_file || fwrite(changed_trigger_source,1,sizeof(changed_trigger_source)-1,changed_trigger_file)!=sizeof(changed_trigger_source)-1 ||
     fclose(changed_trigger_file)!=0)return 1;
  object_events[20].source_path=changed_trigger;
  char create_order[]="/tmp/gml-create-order-XXXXXX"; int create_order_fd=mkstemp(create_order); if(create_order_fd<0)return 1;
  FILE *create_order_file=fdopen(create_order_fd,"wb");
  const char create_order_source[]=
    "global.create_order=global.create_order*10+2; "
    "crear(5); "
    "image_speed=0.5; image_single[0]=7; global.image_single_indexed=image_single[13];\n";
  if(!create_order_file || fwrite(create_order_source,1,sizeof(create_order_source)-1,create_order_file)!=sizeof(create_order_source)-1 ||
     fclose(create_order_file)!=0)return 1;
  object_events[21].source_path=create_order;
  char joystick_event[]="/tmp/gml-joystick-event-XXXXXX"; int joystick_event_fd=mkstemp(joystick_event); if(joystick_event_fd<0)return 1;
  FILE *joystick_event_file=fdopen(joystick_event_fd,"wb");
  const char joystick_event_source[]="global.joystick_event_hits += 1;\n";
  if(!joystick_event_file || fwrite(joystick_event_source,1,sizeof(joystick_event_source)-1,joystick_event_file)!=sizeof(joystick_event_source)-1 ||
     fclose(joystick_event_file)!=0)return 1;
  object_events[22].source_path=joystick_event;
  char solid_collision[]="/tmp/gml-solid-collision-XXXXXX";
  int solid_collision_fd=mkstemp(solid_collision); if(solid_collision_fd<0)return 1;
  FILE *solid_collision_file=fdopen(solid_collision_fd,"wb");
  const char solid_collision_source[]="y -= 1; global.studio_solid_hits += 1;\n";
  if(!solid_collision_file ||
     fwrite(solid_collision_source,1,sizeof(solid_collision_source)-1,solid_collision_file)!=sizeof(solid_collision_source)-1 ||
     fclose(solid_collision_file)!=0)return 1;
  object_events[23].source_path=solid_collision;
  char destroy_reentry[]="/tmp/gml-destroy-reentry-XXXXXX";
  int destroy_reentry_fd=mkstemp(destroy_reentry); if(destroy_reentry_fd<0)return 1;
  FILE *destroy_reentry_file=fdopen(destroy_reentry_fd,"wb");
  const char destroy_reentry_source[]=
    "global.destroy_reentry_hits += 1; instance_destroy();\n";
  if(!destroy_reentry_file ||
     fwrite(destroy_reentry_source,1,sizeof(destroy_reentry_source)-1,destroy_reentry_file)!=sizeof(destroy_reentry_source)-1 ||
     fclose(destroy_reentry_file)!=0)return 1;
  object_events[24].source_path=destroy_reentry;
  char cleanup_counter[]="/tmp/gml-cleanup-counter-XXXXXX";
  int cleanup_counter_fd=mkstemp(cleanup_counter); if(cleanup_counter_fd<0)return 1;
  FILE *cleanup_counter_file=fdopen(cleanup_counter_fd,"wb");
  const char cleanup_counter_source[]="global.cleanup_counter_hits += 1;\n";
  if(!cleanup_counter_file ||
     fwrite(cleanup_counter_source,1,sizeof(cleanup_counter_source)-1,cleanup_counter_file)!=sizeof(cleanup_counter_source)-1 ||
     fclose(cleanup_counter_file)!=0)return 1;
  object_events[25].source_path=cleanup_counter;
  char instance_order[]="/tmp/gml-instance-order-XXXXXX"; int instance_order_fd=mkstemp(instance_order); if(instance_order_fd<0)return 1;
  FILE *instance_order_file=fdopen(instance_order_fd,"wb");
  const char instance_order_source[]="global.create_order=global.create_order*10+1;\n";
  if(!instance_order_file || fwrite(instance_order_source,1,sizeof(instance_order_source)-1,instance_order_file)!=sizeof(instance_order_source)-1 ||
     fclose(instance_order_file)!=0)return 1;
  placed_instance.creation_code_path=instance_order;
  char step[]="/tmp/gml-step-event-XXXXXX"; int step_fd=mkstemp(step); if(step_fd<0)return 1;
  FILE *step_file=fdopen(step_fd,"wb");
  const char step_source[]=
    "global.step_order=global.step_order*10+1; "
    "view_surface_id[0]=2468; "
    "cursor_sprite=2469; "
    "persistent_array[0]=7; "
    "var persistent_alias=persistent_array; "
    "global.persistent_alias_len=array_length_1d(persistent_alias); "
    "global.persistent_alias_value=persistent_alias[0]; "
    "if (!global.spawned_once) { global.spawned_once=1; "
    "with(instance_create(50,50,obj_changed)){ hspeed=3; } } "
    "if (global.churn_pool) { with(instance_create(70,70,obj_changed)){ instance_destroy(); } } "
    "if (hspeed > 0) hspeed -= 1; if (hspeed < 0) hspeed += 1; "
    "hspeed = round(hspeed); x += 5;\n";
  if(!step_file || fwrite(step_source,1,sizeof(step_source)-1,step_file)!=sizeof(step_source)-1 || fclose(step_file)!=0)return 1;
  object_events[1].source_path=step;
  char end_step[]="/tmp/gml-end-step-event-XXXXXX"; int end_step_fd=mkstemp(end_step); if(end_step_fd<0)return 1;
  FILE *end_step_file=fdopen(end_step_fd,"wb"); const char end_step_source[]="x += 7;\n";
  if(!end_step_file || fwrite(end_step_source,1,sizeof(end_step_source)-1,end_step_file)!=sizeof(end_step_source)-1 || fclose(end_step_file)!=0)return 1;
  object_events[10].source_path=end_step;
  char changed_step[]="/tmp/gml-changed-step-event-XXXXXX"; int changed_step_fd=mkstemp(changed_step); if(changed_step_fd<0)return 1;
  FILE *changed_step_file=fdopen(changed_step_fd,"wb");
  const char changed_step_source[]=
    "if (id == global.transition_actor) { "
    "global.transition_step_hits += 1; "
    "if (x > room_width - 16) x = room_width - 16; } "
    "global.step_order=global.step_order*10+2; global.changed_step_hits += 1; "
    "global.engine_event_other_is_self=(other.id==id); "
    "global.nested_empty_with=0; "
    "with(other.id){ global.nested_empty_with=1; "
    "with(noone){ global.nested_empty_with=-1; } "
    "global.nested_empty_with=12; }\n";
  if(!changed_step_file || fwrite(changed_step_source,1,sizeof(changed_step_source)-1,changed_step_file)!=sizeof(changed_step_source)-1 || fclose(changed_step_file)!=0)return 1;
  object_events[11].source_path=changed_step;
  char alarm_files[4][40];
  const char *alarm_sources[4]={
    "if (global.transition_alarm_fixture) { "
    "var actor=instance_create(0,0,obj_changed); actor.persistent=true; "
    "room_goto(1); actor.x=1120; global.transition_actor=actor.id; "
    "} else global.alarm_order=global.alarm_order*10+1;\n",
    "global.alarm_order=global.alarm_order*10+3;\n",
    "global.alarm_order=global.alarm_order*10+2;\n",
    "global.alarm_order=global.alarm_order*10+4;\n"
  };
  const int alarm_events[4]={2,3,12,13};
  for(int i=0;i<4;i++){
    snprintf(alarm_files[i],sizeof(alarm_files[i]),"/tmp/gml-alarm-order-%d-XXXXXX",i);
    int alarm_fd=mkstemp(alarm_files[i]); if(alarm_fd<0)return 1;
    FILE *alarm_file=fdopen(alarm_fd,"wb"); size_t alarm_len=strlen(alarm_sources[i]);
    if(!alarm_file || fwrite(alarm_sources[i],1,alarm_len,alarm_file)!=alarm_len || fclose(alarm_file)!=0)return 1;
    object_events[alarm_events[i]].source_path=alarm_files[i];
  }
  char key_files[2][40]; const int key_events[2]={4,14};
  for(int i=0;i<2;i++){
    snprintf(key_files[i],sizeof(key_files[i]),"/tmp/gml-key-order-%d-XXXXXX",i);
    int key_fd=mkstemp(key_files[i]); if(key_fd<0)return 1;
    FILE *key_file=fdopen(key_fd,"wb");
    char key_source[64]; int key_len=snprintf(key_source,sizeof(key_source),
      "global.key_order=global.key_order*10+%d;\n",i+1);
    if(!key_file || fwrite(key_source,1,(size_t)key_len,key_file)!=(size_t)key_len || fclose(key_file)!=0)return 1;
    object_events[key_events[i]].source_path=key_files[i];
  }
  char mouse_files[2][40]; const int mouse_events[2]={5,15};
  for(int i=0;i<2;i++){
    snprintf(mouse_files[i],sizeof(mouse_files[i]),"/tmp/gml-mouse-order-%d-XXXXXX",i);
    int mouse_fd=mkstemp(mouse_files[i]); if(mouse_fd<0)return 1;
    FILE *mouse_file=fdopen(mouse_fd,"wb");
    char mouse_source[72]; int mouse_len=snprintf(mouse_source,sizeof(mouse_source),
      "global.mouse_order=global.mouse_order*10+%d;\n",i+1);
    if(!mouse_file || fwrite(mouse_source,1,(size_t)mouse_len,mouse_file)!=(size_t)mouse_len || fclose(mouse_file)!=0)return 1;
    object_events[mouse_events[i]].source_path=mouse_files[i];
  }
  char boundary_files[4][44]; const int boundary_events[4]={6,16,7,17};
  for(int i=0;i<4;i++){
    snprintf(boundary_files[i],sizeof(boundary_files[i]),"/tmp/gml-boundary-order-%d-XXXXXX",i);
    int boundary_fd=mkstemp(boundary_files[i]); if(boundary_fd<0)return 1;
    FILE *boundary_file=fdopen(boundary_fd,"wb");
    char boundary_source[80]; int boundary_len=snprintf(boundary_source,sizeof(boundary_source),
      "global.boundary_order=global.boundary_order*10+%d;\n",i+1);
    if(!boundary_file || fwrite(boundary_source,1,(size_t)boundary_len,boundary_file)!=(size_t)boundary_len ||
       fclose(boundary_file)!=0)return 1;
    object_events[boundary_events[i]].source_path=boundary_files[i];
  }
  char view_boundary_files[4][48]; const int view_boundary_events[4]={8,18,9,19};
  for(int i=0;i<4;i++){
    snprintf(view_boundary_files[i],sizeof(view_boundary_files[i]),"/tmp/gml-view-boundary-%d-XXXXXX",i);
    int view_boundary_fd=mkstemp(view_boundary_files[i]); if(view_boundary_fd<0)return 1;
    FILE *view_boundary_file=fdopen(view_boundary_fd,"wb");
    char view_boundary_source[90]; int view_boundary_len=snprintf(view_boundary_source,sizeof(view_boundary_source),
      "global.secondary_boundary_order=global.secondary_boundary_order*10+%d;\n",i+1);
    if(!view_boundary_file || fwrite(view_boundary_source,1,(size_t)view_boundary_len,view_boundary_file)!=(size_t)view_boundary_len ||
       fclose(view_boundary_file)!=0)return 1;
    object_events[view_boundary_events[i]].source_path=view_boundary_files[i];
  }
  char timeline_files[3][44];
  const char *timeline_sources[3]={
    "global.timeline_order=global.timeline_order*10+1; global.dynamic_timeline=timeline_add();\n",
    "global.timeline_order=global.timeline_order*10+2;\n",
    "global.timeline_order=global.timeline_order*10+3;\n"
  };
  for(int i=0;i<3;i++){
    snprintf(timeline_files[i],sizeof(timeline_files[i]),"/tmp/gml-timeline-moment-%d-XXXXXX",i);
    int timeline_fd=mkstemp(timeline_files[i]); if(timeline_fd<0)return 1;
    FILE *timeline_file=fdopen(timeline_fd,"wb"); size_t timeline_len=strlen(timeline_sources[i]);
    if(!timeline_file || fwrite(timeline_sources[i],1,timeline_len,timeline_file)!=timeline_len ||
       fclose(timeline_file)!=0)return 1;
    timeline_moments[i].source_path=timeline_files[i];
  }
  char path[]="/tmp/gml-persistent-room-XXXXXX"; int fd=mkstemp(path); if(fd<0)return 1; close(fd);
  char err[256]={0};
  if(!gmlc_package_write_structural(&project,path,err,sizeof(err))){ fprintf(stderr,"package: %s\n",err); unlink(path); unlink(startup); return 1; }
  if(!expect_native_timeline_import(path)) return 1;
  char included_path[96]; snprintf(included_path,sizeof(included_path),"/tmp/%s",included_name);
  FILE *included_file=fopen(included_path,"rb"); unsigned char observed[sizeof(included_data)]={0};
  int included_ok=included_file && fread(observed,1,sizeof(observed),included_file)==sizeof(observed) &&
                  !memcmp(observed,included_data,sizeof(observed));
  if(included_file) fclose(included_file);
  if(!included_ok){ fprintf(stderr,"included file was not exported\n"); return 1; }
  GmlWin win; if(anygm_stdio_load_win(&win,path)){ unlink(path); return 1; }
  if(!win.classic_executable_layout){
    fprintf(stderr,"classic embedded-layout marker was not serialized\n"); return 1;
  }
  /* The remaining fixture intentionally exercises editor-project ordering. */
  win.classic_executable_layout=0;
  char save_root[]="/tmp/gml-save-root-XXXXXX";
  if(!mkdtemp(save_root)){ gml_win_free(&win); unlink(path); return 1; }
  snprintf(win.save_dir,sizeof win.save_dir,"%s",save_root);
  GmlVM vm; if(gml_vm_init(&vm,&win,&file_services)){ gml_win_free(&win); unlink(path); return 1; }
  fixture_attach_input(&vm);
  {
    int callback_ci=gml_code_index_by_name(&win,"gml_Script_array_ext_callback");
    GmlInstance *receiver=gml_struct_new(&vm);
    if(callback_ci<0 || !receiver) return 1;
    *gml_varmap_put(&receiver->vars,"factor")=vreal(5);
    GmlVal bind_args[2]={vreal((double)receiver->id),
                         vreal((double)(GML_FUNCVAL_TAG|callback_ci))};
    GmlVal callback=gml_builtin_call(&vm,"method",bind_args,2);
    *gml_varmap_put(&receiver->vars,"__ctor")=vreal(callback_ci);
    *gml_varmap_put(&vm.code_static[callback_ci],"shared_fixture")=vreal(42);
    int inherited_ok=0;
    GmlVal inherited=gml_inst_var_get_val(&vm,vreal((double)receiver->id),
                                           "shared_fixture",&inherited_ok);
    GmlInstance *saved_self=vm.cur_self;
    vm.cur_self=receiver;
    GmlVal static_bind_args[2]={vreal(-16),vreal((double)(GML_FUNCVAL_TAG|callback_ci))};
    GmlVal static_callback=gml_builtin_call(&vm,"method",static_bind_args,2);
    GmlVal static_input=vreal(2);
    GmlVal static_result=gml_vm_call_callable(&vm,static_callback,&static_input,1);
    vm.cur_self=saved_self;
    if(!inherited_ok || inherited.t!=V_REAL || inherited.d!=42 ||
       static_result.t!=V_REAL || static_result.d!=10){
      fprintf(stderr,"constructor static field/method fixture failed\n"); return 1;
    }
    GmlVal create_args[2]={vreal(4),callback};
    GmlVal generated=gml_builtin_call(&vm,"array_create_ext",create_args,2);
    if(generated.t!=V_ARR || gml_val_array_length(generated)!=4 ||
       gml_arr_get(generated,0).d!=0 || gml_arr_get(generated,1).d!=5 ||
       gml_arr_get(generated,2).d!=10 || gml_arr_get(generated,3).d!=15){
      fprintf(stderr,"array_create_ext callback/receiver fixture failed\n"); return 1;
    }
    GmlVal mapped=gml_arr_new(5,vreal(0));
    for(int i=0;i<5;i++) gml_arr_set(mapped,i,vreal(i+1));
    GmlVal map_args[4]={mapped,callback,vreal(-2),vreal(-3)};
    GmlVal map_count=gml_builtin_call(&vm,"array_map_ext",map_args,4);
    if(map_count.t!=V_REAL || map_count.d!=3 || gml_val_array_length(mapped)!=5 ||
       gml_arr_get(mapped,0).d!=1 || gml_arr_get(mapped,1).d!=10 ||
       gml_arr_get(mapped,2).d!=15 || gml_arr_get(mapped,3).d!=20 ||
       gml_arr_get(mapped,4).d!=5){
      fprintf(stderr,"array_map_ext callback/range fixture failed\n"); return 1;
    }
    *gml_varmap_put(&vm.globals,"time_source_fixture")=vreal(0);
    GmlVal array_foreach_args[4]={mapped,callback,vreal(-2),vreal(-3)};
    GmlVal foreach_result=gml_builtin_call(&vm,"array_foreach",array_foreach_args,4);
    GmlVal *foreach_total=gml_varmap_get(&vm.globals,"time_source_fixture");
    if(foreach_result.t!=V_UNDEF || !foreach_total || foreach_total->d!=6 ||
       gml_arr_get(mapped,0).d!=1 || gml_arr_get(mapped,1).d!=10 ||
       gml_arr_get(mapped,2).d!=15 || gml_arr_get(mapped,3).d!=20 ||
       gml_arr_get(mapped,4).d!=5){
      fprintf(stderr,"array_foreach callback/range fixture failed\n"); return 1;
    }
    GmlVal words=gml_arr_new(9,vstr(""));
    const char *word_values[9]={"a","b","c","d","e","d","c","b","a"};
    for(int i=0;i<9;i++) gml_arr_set(words,i,vstr(word_values[i]));
    GmlVal first_args[2]={words,vstr("d")};
    GmlVal omitted_args[3]={words,vstr("d"),vreal(6)};
    GmlVal reverse_args[4]={words,vstr("d"),vreal(-1),vreal(-INFINITY)};
    GmlVal clamped_args[4]={words,vstr("a"),vreal(INFINITY),vreal(-INFINITY)};
    GmlVal empty_args[4]={words,vstr("d"),vreal(3),vreal(0)};
    GmlVal first=gml_builtin_call(&vm,"array_get_index",first_args,2);
    GmlVal omitted=gml_builtin_call(&vm,"array_get_index",omitted_args,3);
    GmlVal reverse=gml_builtin_call(&vm,"array_get_index",reverse_args,4);
    GmlVal clamped=gml_builtin_call(&vm,"array_get_index",clamped_args,4);
    GmlVal empty=gml_builtin_call(&vm,"array_get_index",empty_args,4);
    if(first.t!=V_REAL || first.d!=3 || omitted.t!=V_REAL || omitted.d!=-1 ||
       reverse.t!=V_REAL || reverse.d!=5 || clamped.t!=V_REAL || clamped.d!=8 ||
       empty.t!=V_REAL || empty.d!=-1){
      fprintf(stderr,"array_get_index range fixture failed\n"); return 1;
    }
    *gml_varmap_put(&vm.globals,"time_source_fixture")=vreal(0);
    GmlVal timer_args=gml_arr_new(2,vreal(0));
    gml_arr_set(timer_args,1,vreal(3));
    GmlVal timer_create_args[7]={vreal(0),vreal(1),vreal(1),callback,timer_args,vreal(2),vreal(1)};
    GmlVal timer=gml_builtin_call(&vm,"time_source_create",timer_create_args,7);
    (void)gml_builtin_call(&vm,"time_source_start",&timer,1);
    gml_vm_step(&vm);
    GmlVal first_timer_state=gml_builtin_call(&vm,"time_source_get_state",&timer,1);
    GmlVal first_timer_reps=gml_builtin_call(&vm,"time_source_get_reps_remaining",&timer,1);
    GmlVal *timer_total=gml_varmap_get(&vm.globals,"time_source_fixture");
    if(timer.t!=V_REAL || first_timer_state.t!=V_REAL || first_timer_state.d!=1 ||
       first_timer_reps.t!=V_REAL || first_timer_reps.d!=1 || !timer_total || timer_total->d!=3){
      fprintf(stderr,"time source first-frame callback fixture failed\n"); return 1;
    }
    gml_vm_step(&vm);
    GmlVal final_timer_state=gml_builtin_call(&vm,"time_source_get_state",&timer,1);
    GmlVal final_timer_reps=gml_builtin_call(&vm,"time_source_get_reps_completed",&timer,1);
    timer_total=gml_varmap_get(&vm.globals,"time_source_fixture");
    if(final_timer_state.t!=V_REAL || final_timer_state.d!=3 || final_timer_reps.t!=V_REAL ||
       final_timer_reps.d!=2 || !timer_total || timer_total->d!=6){
      fprintf(stderr,"time source repetition fixture failed\n"); return 1;
    }
    GmlVal foreach_args[2]={vstr("A\342\202\254"),callback};
    (void)gml_builtin_call(&vm,"string_foreach",foreach_args,2);
    timer_total=gml_varmap_get(&vm.globals,"time_source_fixture");
    if(!timer_total || timer_total->t!=V_REAL || timer_total->d!=9){
      fprintf(stderr,"string_foreach Unicode position fixture failed\n"); return 1;
    }
    GmlVal letters=gml_arr_new(10,vstr(""));
    const char *letter_values[10]={"a","b","c","d","e","f","g","h","i","j"};
    for(int i=0;i<10;i++) gml_arr_set(letters,i,vstr(letter_values[i]));
    GmlVal concatenate_args[3]={letters,vreal(-5),vreal(-3)};
    GmlVal concatenated=gml_builtin_call(&vm,"string_concat_ext",concatenate_args,3);
    GmlVal suffix_args[2]={letters,vreal(8)};
    GmlVal suffix=gml_builtin_call(&vm,"string_concat_ext",suffix_args,2);
    if(concatenated.t!=V_STR || strcmp(concatenated.s?concatenated.s:"","fed") ||
       suffix.t!=V_STR || strcmp(suffix.s?suffix.s:"","ij")){
      fprintf(stderr,"string_concat_ext range fixture failed\n"); return 1;
    }
    if(concatenated.d!=0) free((void*)concatenated.s);
    if(suffix.d!=0) free((void*)suffix.s);
    *gml_varmap_put(&vm.globals,"fixture_time_source_handle")=timer;
    *gml_varmap_put(&receiver->vars,"callback")=callback;
    GmlVal inner=gml_arr_new(1,vreal(7));
    GmlVal outer=gml_arr_new(1,inner);
    GmlVal shallow_args[2]={outer,vreal(0)};
    GmlVal deep_args[2]={outer,vreal(1)};
    GmlVal shallow=gml_builtin_call(&vm,"variable_clone",shallow_args,2);
    GmlVal deep=gml_builtin_call(&vm,"variable_clone",deep_args,2);
    GmlVal shallow_inner=gml_arr_get(shallow,0), deep_inner=gml_arr_get(deep,0);
    if(shallow.t!=V_ARR || shallow.arr==outer.arr || shallow_inner.arr!=inner.arr ||
       deep.t!=V_ARR || deep.arr==outer.arr || deep_inner.t!=V_ARR || deep_inner.arr==inner.arr ||
       gml_arr_get(deep_inner,0).d!=7){
      fprintf(stderr,"variable_clone array depth fixture failed\n"); return 1;
    }
    GmlVal receiver_value=vreal((double)receiver->id);
    GmlVal cloned_receiver_value=gml_builtin_call(&vm,"variable_clone",&receiver_value,1);
    GmlInstance *cloned_receiver=gml_struct_find(&vm,(unsigned)cloned_receiver_value.d);
    GmlVal *cloned_callback=cloned_receiver?gml_varmap_get(&cloned_receiver->vars,"callback"):NULL;
    GmlVal callback_value=cloned_callback?*cloned_callback:vundef();
    GmlVal callback_input=vreal(2);
    GmlVal callback_result=gml_vm_call_callable(&vm,callback_value,&callback_input,1);
    if(!cloned_receiver || cloned_receiver==receiver || callback_result.t!=V_REAL || callback_result.d!=10){
      fprintf(stderr,"variable_clone bound receiver fixture failed\n"); return 1;
    }
    *gml_varmap_put(&receiver->vars,"outer")=outer;
    *gml_varmap_put(&receiver->vars,"shallow")=shallow;
    *gml_varmap_put(&receiver->vars,"deep")=deep;
    *gml_varmap_put(&vm.globals,"fixture_clone_root")=receiver_value;
  }
  {
    GmlVal build_args[9]={vreal(10),vreal(20),vreal(30),vreal(0),vreal(0),vreal(0),
                          vreal(2),vreal(3),vreal(4)};
    GmlVal matrix=gml_builtin_call(&vm,"matrix_build",build_args,9);
    GmlVal transform_args[4]={matrix,vreal(1),vreal(2),vreal(3)};
    GmlVal xyz=gml_builtin_call(&vm,"matrix_transform_vertex",transform_args,4);
    GmlVal reuse=gml_arr_new(4,vreal(-1));
    GmlVal reuse_args[5]={matrix,vreal(1),vreal(2),vreal(3),reuse};
    GmlVal xyzw=gml_builtin_call(&vm,"matrix_transform_vertex",reuse_args,5);
    GmlVal vector_args[5]={matrix,vreal(1),vreal(2),vreal(3),vreal(0)};
    GmlVal vector=gml_builtin_call(&vm,"matrix_transform_vertex",vector_args,5);
    int matrix_ok=xyz.t==V_ARR && gml_val_array_length(xyz)==3 &&
      fabs(gml_arr_get(xyz,0).d-12)<1e-9 && fabs(gml_arr_get(xyz,1).d-26)<1e-9 &&
      fabs(gml_arr_get(xyz,2).d-42)<1e-9 && xyzw.t==V_ARR && xyzw.arr==reuse.arr &&
      gml_val_array_length(xyzw)==4 && fabs(gml_arr_get(xyzw,3).d-1)<1e-9 &&
      vector.t==V_ARR && gml_val_array_length(vector)==4 &&
      fabs(gml_arr_get(vector,0).d-2)<1e-9 && fabs(gml_arr_get(vector,1).d-6)<1e-9 &&
      fabs(gml_arr_get(vector,2).d-12)<1e-9 && fabs(gml_arr_get(vector,3).d)<1e-9;
    GmlVal look_args[9]={vreal(5),vreal(6),vreal(-7),vreal(5),vreal(6),vreal(-6),
                         vreal(0),vreal(1),vreal(0)};
    GmlVal look=gml_builtin_call(&vm,"matrix_build_lookat",look_args,9);
    GmlVal eye_args[4]={look,vreal(5),vreal(6),vreal(-7)};
    GmlVal target_args[4]={look,vreal(5),vreal(6),vreal(-6)};
    GmlVal eye=gml_builtin_call(&vm,"matrix_transform_vertex",eye_args,4);
    GmlVal target=gml_builtin_call(&vm,"matrix_transform_vertex",target_args,4);
    matrix_ok=matrix_ok && eye.t==V_ARR && target.t==V_ARR &&
      fabs(gml_arr_get(eye,0).d)<1e-9 && fabs(gml_arr_get(eye,1).d)<1e-9 &&
      fabs(gml_arr_get(eye,2).d)<1e-9 && fabs(gml_arr_get(target,0).d)<1e-9 &&
      fabs(gml_arr_get(target,1).d)<1e-9 && fabs(gml_arr_get(target,2).d-1)<1e-9;
    GmlVal perspective_args[4]={vreal(90),vreal(2),vreal(1),vreal(101)};
    GmlVal perspective=gml_builtin_call(
      &vm,"matrix_build_projection_perspective_fov",perspective_args,4);
    matrix_ok=matrix_ok && perspective.t==V_ARR &&
      fabs(gml_arr_get(perspective,0).d-.5)<1e-9 &&
      fabs(gml_arr_get(perspective,5).d-1)<1e-9 &&
      fabs(gml_arr_get(perspective,10).d-1.01)<1e-9 &&
      fabs(gml_arr_get(perspective,11).d-1)<1e-9 &&
      fabs(gml_arr_get(perspective,14).d+1.01)<1e-9 &&
      fabs(gml_arr_get(perspective,15).d)<1e-9;
    if(!matrix_ok){ fprintf(stderr,"matrix construction/vertex transform fixture failed\n"); return 1; }
  }
  {
    GmlVal path_index=gml_builtin_call(&vm,"path_add",NULL,0);
    GmlVal first[4]={path_index,vreal(0),vreal(0),vreal(25)};
    GmlVal second[4]={path_index,vreal(3),vreal(4),vreal(75)};
    (void)gml_builtin_call(&vm,"path_add_point",first,4);
    (void)gml_builtin_call(&vm,"path_add_point",second,4);
    GmlVal closed=gml_builtin_call(&vm,"path_get_closed",&path_index,1);
    GmlVal closed_length=gml_builtin_call(&vm,"path_get_length",&path_index,1);
    GmlVal speed_args[2]={path_index,vreal(1)};
    GmlVal speed=gml_builtin_call(&vm,"path_get_point_speed",speed_args,2);
    GmlVal open_args[2]={path_index,vreal(0)};
    (void)gml_builtin_call(&vm,"path_set_closed",open_args,2);
    GmlVal open_length=gml_builtin_call(&vm,"path_get_length",&path_index,1);
    if(closed.t!=V_REAL || closed.d!=1 || closed_length.t!=V_REAL ||
       fabs(closed_length.d-10)>1e-9 || speed.t!=V_REAL || speed.d!=75 ||
       open_length.t!=V_REAL || fabs(open_length.d-5)>1e-9){
      fprintf(stderr,"runtime path closure/length fixture failed\n"); return 1;
    }
  }
  {
    GmlVal now=gml_builtin_call(&vm,"date_current_datetime",NULL,0);
    GmlVal weekday=gml_builtin_call(&vm,"date_get_weekday",&now,1);
    GmlVal datetime=gml_builtin_call(&vm,"date_datetime_string",&now,1);
    GmlVal env_name=vstr("GML_ENVIRONMENT_FIXTURE");
    GmlVal environment=gml_builtin_call(&vm,"environment_get_variable",&env_name,1);
    if(weekday.t!=V_REAL || weekday.d<0 || weekday.d>6 || datetime.t!=V_STR ||
       !datetime.s || !datetime.s[0] || environment.t!=V_STR || strcmp(environment.s,"visible")){
      fprintf(stderr,"date/environment query fixture failed\n"); return 1;
    }
  }
  {
    GmlInstance *st=gml_struct_new(&vm); if(!st) return 1;
    GmlVal ref=vreal((double)st->id);
    GmlVal set_alpha[3]={ref,vstr("alpha"),vreal(7)};
    GmlVal set_label[3]={ref,vstr("label"),vstr("owned")};
    (void)gml_builtin_call(&vm,"struct_set",set_alpha,3);
    (void)gml_builtin_call(&vm,"variable_struct_set",set_label,3);
    GmlVal key_args[2]={ref,vstr("alpha")};
    GmlVal alpha=gml_builtin_call(&vm,"struct_get",key_args,2);
    GmlVal names=gml_builtin_call(&vm,"variable_struct_get_names",&ref,1);
    GmlVal count=gml_builtin_call(&vm,"struct_names_count",&ref,1);
    GmlVal exists=gml_builtin_call(&vm,"struct_exists",key_args,2);
    GmlVal method_args[2]={vreal(IT_GLOBAL),vreal((double)(GML_FUNCVAL_TAG|1))};
    GmlVal method=gml_builtin_call(&vm,"method",method_args,2);
    GmlVal is_method=gml_builtin_call(&vm,"is_method",&method,1);
    GmlVal method_names=gml_builtin_call(&vm,"struct_get_names",&method,1);
    int struct_ok=alpha.t==V_REAL && alpha.d==7 && names.t==V_ARR &&
      gml_val_array_length(names)==2 && count.t==V_REAL && count.d==2 &&
      exists.t==V_REAL && exists.d==1 && is_method.t==V_REAL && is_method.d==1 &&
      method_names.t==V_ARR && gml_val_array_length(method_names)==0;
    (void)gml_builtin_call(&vm,"struct_remove",key_args,2);
    exists=gml_builtin_call(&vm,"variable_struct_exists",key_args,2);
    if(!struct_ok || exists.t!=V_REAL || exists.d!=0){
      fprintf(stderr,"struct reflection/method fixture failed\n"); return 1;
    }

    GmlVal src=gml_builtin_call(&vm,"ds_map_create",NULL,0);
    GmlVal dst=gml_builtin_call(&vm,"ds_map_create",NULL,0);
    GmlVal src_add[3]={src,vstr("score"),vreal(11)};
    GmlVal dst_add[3]={dst,vstr("stale"),vreal(3)};
    (void)gml_builtin_call(&vm,"ds_map_add",src_add,3);
    (void)gml_builtin_call(&vm,"ds_map_add",dst_add,3);
    GmlVal copy_args[2]={dst,src};
    (void)gml_builtin_call(&vm,"ds_map_copy",copy_args,2);
    src_add[2]=vreal(19);
    (void)gml_builtin_call(&vm,"ds_map_replace",src_add,3);
    GmlVal find_dst[2]={dst,vstr("score")};
    GmlVal copied=gml_builtin_call(&vm,"ds_map_find_value",find_dst,2);
    GmlVal stale_args[2]={dst,vstr("stale")};
    GmlVal stale=gml_builtin_call(&vm,"ds_map_exists",stale_args,2);
    if(copied.t!=V_REAL || copied.d!=11 || stale.t!=V_REAL || stale.d!=0){
      fprintf(stderr,"ds_map_copy replacement fixture failed\n"); return 1;
    }
    (void)gml_builtin_call(&vm,"ds_map_destroy",&src,1);
    (void)gml_builtin_call(&vm,"ds_map_destroy",&dst,1);
  }
  {
    GmlVal *transition_kind=gml_varmap_get(&vm.globals,"transition_kind");
    GmlVal *transition_steps=gml_varmap_get(&vm.globals,"transition_steps");
    if(!transition_kind || transition_kind->t!=V_REAL || transition_kind->d!=0 ||
       !transition_steps || transition_steps->t!=V_REAL || transition_steps->d!=80){
      fprintf(stderr,"classic transition defaults mismatch: kind=%.0f steps=%.0f\n",
        transition_kind&&transition_kind->t==V_REAL?transition_kind->d:-1.0,
        transition_steps&&transition_steps->t==V_REAL?transition_steps->d:-1.0);
      return 1;
    }
  }
  GmlVal *startup_value=gml_varmap_get(&vm.globals,"startup_value");
  GmlVal *view_fixture_scalar=gml_varmap_get(&vm.globals,"view_fixture_scalar");
  GmlVal *background_fixture_scalar=gml_varmap_get(&vm.globals,"background_fixture_scalar");
  GmlVal *view_enabled_alias=gml_varmap_get(&vm.globals,"view_enabled_alias");
  GmlVal *room_caption_fixture=gml_varmap_get(&vm.globals,"room_caption_fixture");
  GmlVal *room_caption=gml_varmap_get(&vm.globals,"room_caption");
  GmlVal *delta_fixture=gml_varmap_get(&vm.globals,"delta_fixture");
  GmlVal *working_fixture=gml_varmap_get(&vm.globals,"working_fixture");
  GmlVal *program_fixture=gml_varmap_get(&vm.globals,"program_fixture");
  GmlVal *fixture_orange=gml_varmap_get(&vm.globals,"fixture_orange");
  GmlVal *fixture_rain=gml_varmap_get(&vm.globals,"fixture_rain");
  GmlVal *date_minute_span_fixture=gml_varmap_get(&vm.globals,"date_minute_span_fixture");
  char expected_working[640],expected_program[640];
  /* This compiler fixture is a bytecode-15 Studio package: working_directory remains the
   * installed content root, while writes are still redirected through the file sandbox. */
  snprintf(expected_working,sizeof expected_working,"%s/",win.content_dir);
  snprintf(expected_program,sizeof expected_program,"%s/",win.content_dir);
  if(!startup_value || startup_value->t!=V_REAL || startup_value->d!=42 ||
     !view_fixture_scalar || view_fixture_scalar->t!=V_REAL || view_fixture_scalar->d!=7 ||
     !background_fixture_scalar || background_fixture_scalar->t!=V_REAL || background_fixture_scalar->d!=9 ||
     !view_enabled_alias || view_enabled_alias->t!=V_REAL || view_enabled_alias->d!=1 ||
     !room_caption_fixture || room_caption_fixture->t!=V_STR ||
       strcmp(room_caption_fixture->s,"fixture caption") ||
     !room_caption || room_caption->t!=V_STR || strcmp(room_caption->s,"fixture caption") ||
     !delta_fixture || delta_fixture->t!=V_REAL || fabs(delta_fixture->d-1000000.0/60.0)>1e-6 ||
     !working_fixture || working_fixture->t!=V_STR || strcmp(working_fixture->s,expected_working) ||
     !program_fixture || program_fixture->t!=V_STR || strcmp(program_fixture->s,expected_program) ||
     !fixture_orange || fixture_orange->t!=V_REAL || fixture_orange->d!=0x40A0FF ||
     !fixture_rain || fixture_rain->t!=V_REAL || fixture_rain->d!=10 ||
     !date_minute_span_fixture || date_minute_span_fixture->t!=V_REAL ||
       date_minute_span_fixture->d!=720){
    fprintf(stderr,"startup code or project constant did not run: startup=%.9g view=%.9g background=%.9g delta=%.17g orange=%.9g rain=%.9g\n",
      startup_value&&startup_value->t==V_REAL?startup_value->d:-1.0,
      view_fixture_scalar&&view_fixture_scalar->t==V_REAL?view_fixture_scalar->d:-1.0,
      background_fixture_scalar&&background_fixture_scalar->t==V_REAL?background_fixture_scalar->d:-1.0,
      delta_fixture&&delta_fixture->t==V_REAL?delta_fixture->d:-1.0,
      fixture_orange&&fixture_orange->t==V_REAL?fixture_orange->d:-1.0,
      fixture_rain&&fixture_rain->t==V_REAL?fixture_rain->d:-1.0); return 1;
  }
  if(!expect_file_sandbox(&vm,save_root)) return 1;
  gml_room_enter(&vm,0);
  GmlVal *room_view_enabled=gml_varmap_get(&vm.globals,"view_enabled");
  if(!room_view_enabled || room_view_enabled->t!=V_REAL || room_view_enabled->d!=1){
    fprintf(stderr,"room flags did not enable the legacy view system: %.0f\n",
      room_view_enabled&&room_view_enabled->t==V_REAL?room_view_enabled->d:-1.0);
    return 1;
  }
  GmlInstance *timeline_probe=find_slot(&vm,100000); if(!timeline_probe)return 1;
  GmlVal *image_single_indexed=gml_varmap_get(&vm.globals,"image_single_indexed");
  GmlVal *user_crear_hits=gml_varmap_get(&vm.globals,"user_crear_hits");
  if(timeline_probe->image_index!=7 || timeline_probe->image_speed!=0 ||
     !image_single_indexed || image_single_indexed->t!=V_REAL || image_single_indexed->d!=7 ||
     !user_crear_hits || user_crear_hits->t!=V_REAL || user_crear_hits->d!=5){
    fprintf(stderr,"classic indexed image_single or project-script precedence mismatch: index=%.1f speed=%.1f read=%.1f script=%.1f\n",
      timeline_probe->image_index,timeline_probe->image_speed,
      image_single_indexed&&image_single_indexed->t==V_REAL?image_single_indexed->d:-1.0,
      user_crear_hits&&user_crear_hits->t==V_REAL?user_crear_hits->d:-1.0); return 1;
  }
  /* The optional false flag suppresses Destroy without suppressing Clean Up. This is distinct from
   * target selection: the requested instance must still become dead, and the normal one-argument
   * form continues to use the event-producing wrapper exercised below. */
  uint32_t destroy_flag_next_id=vm.next_id;
  GmlInstance *destroy_flag_probe=gml_instance_create(&vm,40,40,2);
  if(!destroy_flag_probe)return 1;
  GmlVal destroy_flag_args[2]={vreal((double)destroy_flag_probe->id),vreal(0)};
  (void)gml_builtin_call(&vm,"instance_destroy",destroy_flag_args,2);
  GmlVal *destroy_flag_hits=gml_varmap_get(&vm.globals,"destroy_reentry_hits");
  GmlVal *cleanup_counter_hits=gml_varmap_get(&vm.globals,"cleanup_counter_hits");
  if(!destroy_flag_probe->marked ||
     (destroy_flag_hits && (destroy_flag_hits->t!=V_REAL || destroy_flag_hits->d!=0)) ||
     !cleanup_counter_hits || cleanup_counter_hits->t!=V_REAL || cleanup_counter_hits->d!=1){
    fprintf(stderr,"instance_destroy false flag mismatch: marked=%d destroy=%.0f cleanup=%.0f\n",
      destroy_flag_probe->marked,
      destroy_flag_hits&&destroy_flag_hits->t==V_REAL?destroy_flag_hits->d:-1.0,
      cleanup_counter_hits&&cleanup_counter_hits->t==V_REAL?cleanup_counter_hits->d:-1.0);
    return 1;
  }
  gml_vm_instances_reap(&vm);
  vm.next_id=destroy_flag_next_id;
  *gml_varmap_put(&vm.globals,"destroy_reentry_hits")=vreal(0);
  *gml_varmap_put(&vm.globals,"cleanup_counter_hits")=vreal(0);
  uint32_t path_next_id=vm.next_id;
  GmlInstance *path_probe=gml_instance_create(&vm,100,100,2); if(!path_probe)return 1;
  GmlInstance *path_control=gml_instance_create(&vm,100,100,2); if(!path_control)return 1;
  GmlInstance *engine_other_probe=gml_instance_create(&vm,110,100,1); if(!engine_other_probe)return 1;
  gml_run_event(&vm,engine_other_probe,"Step_0");
  GmlVal *engine_event_other_is_self=gml_varmap_get(&vm.globals,"engine_event_other_is_self");
  if(!engine_event_other_is_self || engine_event_other_is_self->t!=V_REAL ||
     engine_event_other_is_self->d!=1){
    fprintf(stderr,"engine event did not expose self as other: %.0f\n",
      engine_event_other_is_self&&engine_event_other_is_self->t==V_REAL?
      engine_event_other_is_self->d:-1.0);
    return 1;
  }
  GmlVal *nested_empty_with=gml_varmap_get(&vm.globals,"nested_empty_with");
  if(!nested_empty_with || nested_empty_with->t!=V_REAL || nested_empty_with->d!=12){
    fprintf(stderr,"empty nested with closed its parent scope: %.0f\n",
      nested_empty_with&&nested_empty_with->t==V_REAL?nested_empty_with->d:-1.0);
    return 1;
  }
  gml_instance_destroy(&vm,engine_other_probe);
  gml_set_global_scalar(&vm,"step_order",0);
  gml_set_global_scalar(&vm,"create_order",12);
  uint32_t path_probe_id=path_probe->id, path_control_id=path_control->id;
  path_probe=find_slot(&vm,path_probe_id); path_control=find_slot(&vm,path_control_id);
  if(!path_probe || !path_control)return 1;
  gml_path_start(&vm,path_probe,0,10,0,0);
  gml_path_start(&vm,path_control,0,10,0,0);
  path_probe->path_orientation=180;
  path_probe->path_scale=2;
  gml_path_start(&vm,path_probe,0,10,0,0);
  if(path_probe->path_orientation!=0 || path_probe->path_scale!=1){
    fprintf(stderr,"path_start retained a previous traversal transform: orientation=%.1f scale=%.1f\n",
      path_probe->path_orientation,path_probe->path_scale); return 1;
  }
  path_probe->hspeed=7; path_probe->vspeed=0; path_probe->speed=7; path_probe->direction=0;
  gml_vm_step(&vm);
  path_probe=find_slot(&vm,path_probe_id); path_control=find_slot(&vm,path_control_id);
  if(!path_probe || !path_control)return 1;
  if(fabs(path_probe->x-path_control->x)>1e-6 || fabs(path_probe->y-path_control->y)>1e-6 ||
     path_probe->hspeed!=0 || path_probe->vspeed!=0 || path_probe->speed!=0 ||
     fabs(path_probe->direction)>1e-9){
    fprintf(stderr,"path motion retained stale ordinary velocity: probe=(%.6f,%.6f) control=(%.6f,%.6f) direction=%.6f velocity=(%.6f,%.6f) speed=%.6f\n",
      path_probe->x,path_probe->y,path_control->x,path_control->y,path_probe->direction,
      path_probe->hspeed,path_probe->vspeed,path_probe->speed);
    return 1;
  }
  double paused_start=path_probe->x;
  path_probe->path_speed=0; path_probe->hspeed=2; path_probe->speed=2; path_probe->direction=0;
  gml_vm_step(&vm);
  path_probe=find_slot(&vm,path_probe_id); path_control=find_slot(&vm,path_control_id);
  if(!path_probe || !path_control)return 1;
  if(fabs(path_probe->x-(paused_start+2))>1e-6 || path_probe->hspeed!=2 || path_probe->speed!=2){
    fprintf(stderr,"paused path suppressed ordinary velocity: start=%.6f expected=%.6f x=%.6f hspeed=%.6f speed=%.6f\n",
      paused_start,paused_start+2,path_probe->x,path_probe->hspeed,path_probe->speed); return 1;
  }
  double reverse_start=path_probe->x;
  path_probe->path_orientation=180; path_probe->path_scale=2;
  gml_path_start(&vm,path_probe,0,-10,0,0);
  if(path_probe->path_position!=1 || path_probe->path_positionprevious!=1 ||
     path_probe->x!=reverse_start || path_probe->path_orientation!=0 || path_probe->path_scale!=1){
    fprintf(stderr,"reverse path did not start at its far endpoint: pos=%.1f previous=%.1f x=%.1f\n",
      path_probe->path_position,path_probe->path_positionprevious,path_probe->x); return 1;
  }
  gml_instance_destroy(&vm,path_probe); gml_instance_destroy(&vm,path_control);

  /* A bytecode-15 path step that advances the instance clears ordinary velocity. The position
   * rule remains generation-specific: a paused path is evaluated after automatic movement,
   * restoring the current path point while leaving that movement's velocity readable. */
  win.classic_version=0; win.bytecode=15;
  path_probe=gml_instance_create(&vm,100,100,2); if(!path_probe)return 1;
  path_control=gml_instance_create(&vm,100,100,2); if(!path_control)return 1;
  path_probe_id=path_probe->id; path_control_id=path_control->id;
  gml_path_start(&vm,path_probe,0,10,0,0);
  gml_path_start(&vm,path_control,0,10,0,0);
  path_probe->hspeed=7; path_probe->vspeed=0; path_probe->speed=7; path_probe->direction=0;
  gml_vm_step(&vm);
  path_probe=find_slot(&vm,path_probe_id); path_control=find_slot(&vm,path_control_id);
  if(!path_probe || !path_control || fabs(path_probe->x-path_control->x)>1e-6 ||
     fabs(path_probe->y-path_control->y)>1e-6 || path_probe->hspeed!=0 || path_probe->vspeed!=0 ||
     path_probe->speed!=0){
    fprintf(stderr,"bytecode-15 path left a pre-path velocity readable: probe=(%.6f,%.6f) control=(%.6f,%.6f) velocity=(%.6f,%.6f) speed=%.6f\n",
      path_probe?path_probe->x:-1.0,path_probe?path_probe->y:-1.0,
      path_control?path_control->x:-1.0,path_control?path_control->y:-1.0,
      path_probe?path_probe->hspeed:-1.0,path_probe?path_probe->vspeed:-1.0,
      path_probe?path_probe->speed:-1.0); return 1;
  }
  paused_start=path_probe->x;
  path_probe->path_speed=0; path_probe->hspeed=2; path_probe->speed=2; path_probe->direction=0;
  gml_vm_step(&vm);
  path_probe=find_slot(&vm,path_probe_id);
  if(!path_probe || fabs(path_probe->x-paused_start)>1e-6 ||
     path_probe->hspeed!=2 || path_probe->speed!=2){
    fprintf(stderr,"Studio 1.x paused path did not override ordinary displacement: start=%.6f x=%.6f hspeed=%.6f speed=%.6f\n",
      paused_start,path_probe?path_probe->x:-1.0,path_probe?path_probe->hspeed:-1.0,
      path_probe?path_probe->speed:-1.0); return 1;
  }
  gml_instance_destroy(&vm,path_probe); gml_instance_destroy(&vm,path_control);
  gml_set_global_scalar(&vm,"create_order",12);
  win.classic_version=800; win.bytecode=15;
  GmlVal *destroy_reentry_hits=gml_varmap_get(&vm.globals,"destroy_reentry_hits");
  if(!path_probe->marked || !path_control->marked || !destroy_reentry_hits ||
     destroy_reentry_hits->t!=V_REAL || destroy_reentry_hits->d!=4){
    fprintf(stderr,"recursive Destroy event was not single-shot: marked=(%d,%d) hits=%.0f\n",
      path_probe->marked,path_control->marked,
      destroy_reentry_hits&&destroy_reentry_hits->t==V_REAL?destroy_reentry_hits->d:-1.0);
    return 1;
  }
  vm.next_id=path_next_id;
  *gml_varmap_put(&vm.globals,"timeline_order")=vreal(0);
  timeline_probe->timeline_index=0; timeline_probe->timeline_position=0;
  timeline_probe->timeline_speed=-1; timeline_probe->timeline_running=1; timeline_probe->timeline_loop=0;
  gml_vm_step(&vm);
  GmlVal *timeline_order=gml_varmap_get(&vm.globals,"timeline_order");
  if(!timeline_order || timeline_order->t!=V_REAL || timeline_order->d!=1 ||
     timeline_probe->timeline_position!=0 || timeline_probe->timeline_running!=0){
    fprintf(stderr,"reverse timeline did not fire its initial moment: order=%.0f pos=%.1f running=%.0f\n",
      timeline_order&&timeline_order->t==V_REAL?timeline_order->d:-1.0,
      timeline_probe->timeline_position,timeline_probe->timeline_running); return 1;
  }
  timeline_order->d=0; timeline_probe->timeline_position=0; timeline_probe->timeline_speed=1;
  timeline_probe->timeline_running=1; timeline_probe->timeline_loop=0;
  gml_vm_step(&vm); gml_vm_step(&vm); gml_vm_step(&vm);
  if(timeline_order->d!=12 || timeline_probe->timeline_position!=3 || !timeline_probe->timeline_running){
    fprintf(stderr,"forward timeline endpoint ownership mismatch: order=%.0f pos=%.1f running=%.0f\n",
      timeline_order->d,timeline_probe->timeline_position,timeline_probe->timeline_running); return 1;
  }
  timeline_order->d=0; timeline_probe->timeline_position=4; timeline_probe->timeline_speed=2;
  timeline_probe->timeline_running=1; timeline_probe->timeline_loop=1;
  gml_vm_step(&vm);
  if(timeline_order->d!=31 || timeline_probe->timeline_position!=1){
    fprintf(stderr,"forward timeline wrap mismatch: order=%.0f pos=%.1f\n",
      timeline_order->d,timeline_probe->timeline_position); return 1;
  }
  timeline_order->d=0; timeline_probe->timeline_position=0; timeline_probe->timeline_speed=-2;
  timeline_probe->timeline_running=1; timeline_probe->timeline_loop=1;
  gml_vm_step(&vm);
  if(timeline_order->d!=13 || timeline_probe->timeline_position!=3){
    fprintf(stderr,"reverse timeline wrap mismatch: order=%.0f pos=%.1f\n",
      timeline_order->d,timeline_probe->timeline_position); return 1;
  }
  timeline_probe->timeline_running=0; timeline_probe->timeline_loop=0;
  if(vm.n_timelines<2 || !vm.timelines[vm.n_timelines-1].name){
    fprintf(stderr,"timeline_add from a running moment did not append an asset\n"); return 1;
  }
  int timeline_count=vm.n_timelines;
  GmlVal added_timeline=gml_builtin_call(&vm,"timeline_add",NULL,0);
  GmlVal added_arg=added_timeline;
  GmlVal added_exists=gml_builtin_call(&vm,"timeline_exists",&added_arg,1);
  if(added_timeline.t!=V_REAL || added_timeline.d!=timeline_count ||
     added_exists.t!=V_REAL || added_exists.d!=1 || vm.n_timelines!=timeline_count+1){
    fprintf(stderr,"runtime timeline allocation mismatch: index=%.0f count=%d exists=%.0f\n",
      added_timeline.d,vm.n_timelines,added_exists.d); return 1;
  }
  GmlVal imported_timeline=vreal(0);
  (void)gml_builtin_call(&vm,"timeline_clear",&imported_timeline,1);
  GmlVal imported_exists=gml_builtin_call(&vm,"timeline_exists",&imported_timeline,1);
  if(vm.timelines[0].n!=0 || vm.timelines[0].last_step!=-1 ||
     imported_exists.t!=V_REAL || imported_exists.d!=1){
    fprintf(stderr,"timeline_clear removed the asset or retained its moments\n"); return 1;
  }
  int implicit_code=gml_code_index_by_name(&win,"gml_Script_script_implicit_result");
  GmlVal implicit_arg=vreal(2);
  GmlVal implicit_result=implicit_code>=0?
    gml_vm_run_code(&vm,implicit_code,NULL,NULL,&implicit_arg,1):vreal(0);
  if(implicit_code<0 || implicit_result.t!=V_REAL || implicit_result.d!=1){
    fprintf(stderr,"classic script implicit result mismatch: code=%d result=%.0f\n",
      implicit_code,implicit_result.t==V_REAL?implicit_result.d:-1.0); return 1;
  }
  {
    int saved_classic=win.classic_version, saved_bytecode=win.bytecode;
    double saved_epsilon=vm.math_epsilon;
    implicit_arg=vreal(-4);
    win.classic_version=0; win.bytecode=15; vm.math_epsilon=1e-5;
    GmlVal studio1_compare=gml_vm_run_code(&vm,implicit_code,NULL,NULL,&implicit_arg,1);
    win.bytecode=17;
    GmlVal studio2_compare=gml_vm_run_code(&vm,implicit_code,NULL,NULL,&implicit_arg,1);
    win.classic_version=saved_classic; win.bytecode=saved_bytecode;
    vm.math_epsilon=saved_epsilon;
    if(studio1_compare.t!=V_REAL || studio1_compare.d!=1 ||
       studio2_compare.t!=V_REAL || studio2_compare.d!=1){
      fprintf(stderr,"Studio comparison-family epsilon mismatch: bytecode15=%.0f bytecode17=%.0f\n",
        studio1_compare.t==V_REAL?studio1_compare.d:-1.0,
        studio2_compare.t==V_REAL?studio2_compare.d:-1.0); return 1;
    }
  }
  if(gml_global_num(&vm,"transition_kind")!=12 || gml_global_num(&vm,"transition_steps")!=40){
    fprintf(stderr,"classic unqualified transition variables did not route globally: kind=%.0f steps=%.0f\n",
      gml_global_num(&vm,"transition_kind"),gml_global_num(&vm,"transition_steps")); return 1;
  }
  gml_set_global_scalar(&vm,"transition_kind",0);
  gml_set_global_scalar(&vm,"transition_steps",80);
  implicit_arg=vreal(-1);
  implicit_result=gml_vm_run_code(&vm,implicit_code,NULL,NULL,&implicit_arg,1);
  if(implicit_result.t!=V_REAL || implicit_result.d!=42){
    fprintf(stderr,"classic script implicit assignment result mismatch: %.0f\n",
      implicit_result.t==V_REAL?implicit_result.d:-1.0); return 1;
  }
  GmlVal *create_order_value=gml_varmap_get(&vm.globals,"create_order");
  if(!create_order_value || create_order_value->t!=V_REAL || create_order_value->d!=12){
    fprintf(stderr,"classic instance/Create order mismatch: %.0f\n",
      create_order_value&&create_order_value->t==V_REAL?create_order_value->d:-1.0); return 1;
  }
  GmlInstance *created=gml_instance_create(&vm,12,34,0); if(!created)return 1;
  if(created->id!=100001){
    fprintf(stderr,"placed instances consumed a dynamic instance id: %u\n",created->id); return 1;
  }
  int exact_family=gml_instance_number(&vm,0);
  GmlVal parent_args[2]={vreal(0),vreal(1)};
  (void)gml_builtin_call(&vm,"object_set_parent",parent_args,2);
  GmlVal child_arg=vreal(0);
  GmlVal observed_parent=gml_builtin_call(&vm,"object_get_parent",&child_arg,1);
  if(observed_parent.t!=V_REAL || observed_parent.d!=1 || !gml_object_is(&vm,0,1) ||
     gml_instance_number(&vm,1)!=exact_family){
    fprintf(stderr,"runtime object parent did not update hierarchy queries\n"); return 1;
  }
  parent_args[0]=vreal(1); parent_args[1]=vreal(0);
  (void)gml_builtin_call(&vm,"object_set_parent",parent_args,2);
  if(vm.objects[1].parent==0 || gml_object_is(&vm,1,0)){
    fprintf(stderr,"runtime object parent accepted an inheritance cycle\n"); return 1;
  }
  parent_args[0]=vreal(0); parent_args[1]=vreal(-100);
  (void)gml_builtin_call(&vm,"object_set_parent",parent_args,2);
  if(vm.objects[0].parent!=-1 || gml_instance_number(&vm,1)!=0){
    fprintf(stderr,"runtime object parent detach did not rebuild family counts\n"); return 1;
  }
  GmlInstance *all_first=NULL;
  for(int i=0;i<vm.inst_count;i++) if(vm.inst[i].active && !vm.inst[i].marked){ all_first=&vm.inst[i]; break; }
  if(!all_first || all_first==created) return 1;
  *gml_varmap_put(&all_first->vars,"fixture_all_scope")=vreal(11);
  *gml_varmap_put(&created->vars,"fixture_all_scope")=vreal(22);
  implicit_arg=vreal(-2);
  implicit_result=gml_vm_run_code(&vm,implicit_code,created,NULL,&implicit_arg,1);
  if(implicit_result.t!=V_REAL || implicit_result.d!=11){
    fprintf(stderr,"all-scope read did not use the first active instance: %.0f\n",
      implicit_result.t==V_REAL?implicit_result.d:-1.0); return 1;
  }
  implicit_arg=vreal(-3);
  implicit_result=gml_vm_run_code(&vm,implicit_code,created,NULL,&implicit_arg,1);
  for(int i=0;i<vm.inst_count;i++) if(vm.inst[i].active && !vm.inst[i].marked){
    GmlVal *all_value=gml_varmap_get(&vm.inst[i].vars,"fixture_all_scope");
    if(!all_value || all_value->t!=V_REAL || all_value->d!=77){
      fprintf(stderr,"all-scope write did not fan out to instance %u\n",vm.inst[i].id); return 1;
    }
  }
  vm.cur_self=created;
  GmlVal change_args[2]={vreal(1),vreal(0)};
  (void)gml_builtin_call(&vm,"action_change_object",change_args,2);
  if(created->obj!=1){ fprintf(stderr,"classic change-object action did not replace the object\n"); return 1; }
  change_args[0]=vreal(0);
  (void)gml_builtin_call(&vm,"action_change_object",change_args,2);
  vm.cur_self=created;
  GmlVal object_args[4]={vreal(1),vreal(32),vreal(0),vreal(1)};
  GmlVal object_hit=gml_builtin_call(&vm,"action_if_object",object_args,4);
  GmlVal exists_arg=vreal((double)created->id);
  GmlVal legacy_exists=gml_builtin_call(&vm,"existe",&exists_arg,1);
  GmlVal previous_room=gml_builtin_call(&vm,"action_if_previous_room",NULL,0);
  if(object_hit.t!=V_REAL || object_hit.d!=0 || legacy_exists.t!=V_REAL || legacy_exists.d!=1 ||
     previous_room.t!=V_REAL || previous_room.d!=0){
    fprintf(stderr,"classic object/existence/previous-room conditions did not match: object=%.0f exists=%.0f previous=%.0f\n",
      object_hit.d,legacy_exists.d,previous_room.d); return 1;
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
  if(created->direction!=180 || created->hspeed!=-5 || created->vspeed!=0 ||
     vm.rng_classic_state!=1503470698u){
    fprintf(stderr,"classic direction-pad rejection mismatch: direction=%.0f velocity=(%.17g,%.17g) seed=%u\n",
      created->direction,created->hspeed,created->vspeed,vm.rng_classic_state); return 1;
  }
  created->hspeed=3; created->vspeed=4;
  created->speed=5; created->direction=atan2(-4.0,3.0)*180.0/M_PI+360.0;
  vm.action_relative=1;
  GmlVal relative_motion[2]={vreal(0),vreal(2)};
  (void)gml_builtin_call(&vm,"action_set_motion",relative_motion,2);
  vm.action_relative=0;
  if(created->hspeed!=5 || created->vspeed!=4 ||
     fabs(created->speed-sqrt(41.0))>1e-12 ||
     fabs(created->direction-(atan2(-4.0,5.0)*180.0/M_PI+360.0))>1e-12){
    fprintf(stderr,"classic relative motion did not add vector components: direction=%.17g speed=%.17g velocity=(%.17g,%.17g)\n",
      created->direction,created->speed,created->hspeed,created->vspeed); return 1;
  }
  created->direction=created->speed=created->hspeed=created->vspeed=0;
  vm.cur_self=created;
  GmlVal cardinal_motion[2]={vreal(450),vreal(1)};
  (void)gml_builtin_call(&vm,"motion_set",cardinal_motion,2);
  if(created->direction!=90 || created->hspeed!=0 || created->vspeed!=-1){
    fprintf(stderr,"classic cardinal motion normalization mismatch: direction=%.17g velocity=(%.17g,%.17g)\n",
      created->direction,created->hspeed,created->vspeed); return 1;
  }
  GmlVal cardinal_target[3]={vreal(created->x),vreal(created->y+10),vreal(1)};
  (void)gml_builtin_call(&vm,"move_towards_point",cardinal_target,3);
  if(created->direction!=270 || created->hspeed!=0 || created->vspeed!=1){
    fprintf(stderr,"classic cardinal target motion mismatch: direction=%.17g velocity=(%.17g,%.17g)\n",
      created->direction,created->hspeed,created->vspeed); return 1;
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
  GmlInstance *late_order=gml_instance_create(&vm,300,100,0); if(!late_order)return 1;
  collision_sprites[0].w=collision_sprites[0].h=1;
  collision_sprites[0].mr=collision_sprites[0].mb=0;
  created->x=created->y=created->xprevious=created->yprevious=0;
  contact->x=1; contact->y=0; contact->solid=1;
  vm.cur_self=created;
  gml_colgrid_invalidate(&vm);
  GmlVal place_empty_any_args[2]={vreal(1),vreal(0)};
  GmlVal place_empty_target_args[3]={vreal(1),vreal(0),vreal(0)};
  GmlVal place_empty_any=gml_builtin_call(&vm,"place_empty",place_empty_any_args,2);
  GmlVal place_empty_target=gml_builtin_call(&vm,"place_empty",place_empty_target_args,3);
  if(place_empty_any.t!=V_REAL || place_empty_any.d!=0 ||
     place_empty_target.t!=V_REAL || place_empty_target.d!=1){
    fprintf(stderr,"place_empty optional target mismatch: any=%.0f target=%.0f\n",
      place_empty_any.t==V_REAL?place_empty_any.d:-1.0,
      place_empty_target.t==V_REAL?place_empty_target.d:-1.0); return 1;
  }
  GmlVal collision_list=gml_builtin_call(&vm,"ds_list_create",NULL,0);
  GmlVal rectangle_list_args[9]={vreal(1),vreal(0),vreal(1),vreal(0),
    vreal((double)contact->id),vreal(0),vreal(0),collision_list,vreal(1)};
  gml_colgrid_invalidate(&vm);
  GmlVal rectangle_count=gml_builtin_call(&vm,"collision_rectangle_list",rectangle_list_args,9);
  GmlVal list_at[2]={collision_list,vreal(0)};
  GmlVal rectangle_hit=gml_builtin_call(&vm,"ds_list_find_value",list_at,2);
  if(rectangle_count.t!=V_REAL || rectangle_count.d!=1 || rectangle_hit.t!=V_REAL ||
     rectangle_hit.d!=(double)contact->id){
    fprintf(stderr,"collision_rectangle_list fixture failed: count=%.0f id=%.0f\n",
      rectangle_count.t==V_REAL?rectangle_count.d:-1.0,
      rectangle_hit.t==V_REAL?rectangle_hit.d:-1.0); return 1;
  }
  (void)gml_builtin_call(&vm,"ds_list_clear",&collision_list,1);
  GmlVal circle_list_args[8]={vreal(1),vreal(0),vreal(0),vreal((double)contact->id),
    vreal(0),vreal(0),collision_list,vreal(1)};
  GmlVal circle_count=gml_builtin_call(&vm,"collision_circle_list",circle_list_args,8);
  GmlVal circle_hit=gml_builtin_call(&vm,"ds_list_find_value",list_at,2);
  if(circle_count.t!=V_REAL || circle_count.d!=1 || circle_hit.t!=V_REAL ||
     circle_hit.d!=(double)contact->id){
    fprintf(stderr,"collision_circle_list fixture failed: count=%.0f id=%.0f\n",
      circle_count.t==V_REAL?circle_count.d:-1.0,
      circle_hit.t==V_REAL?circle_hit.d:-1.0); return 1;
  }
  (void)gml_builtin_call(&vm,"ds_list_destroy",&collision_list,1);
  GmlVal bounce_motion[2]={vreal(0),vreal(1)};
  (void)gml_builtin_call(&vm,"motion_set",bounce_motion,2);
  GmlVal advanced_bounce=vreal(1);
  gml_colgrid_invalidate(&vm);
  (void)gml_builtin_call(&vm,"move_bounce_solid",&advanced_bounce,1);
  if(created->direction!=180 || created->hspeed!=-1 || created->vspeed!=0){
    fprintf(stderr,"classic advanced bounce did not reflect around the sampled free directions: direction=%.17g velocity=(%.17g,%.17g)\n",
      created->direction,created->hspeed,created->vspeed); return 1;
  }
  contact->x=1; contact->y=1;
  bounce_motion[0]=vreal(315);
  (void)gml_builtin_call(&vm,"motion_set",bounce_motion,2);
  GmlVal basic_bounce=vreal(0);
  gml_colgrid_invalidate(&vm);
  (void)gml_builtin_call(&vm,"move_bounce_solid",&basic_bounce,1);
  if(fabs(created->direction-135)>1e-9 ||
     fabs(created->hspeed+sqrt(0.5))>1e-9 || fabs(created->vspeed+sqrt(0.5))>1e-9){
    fprintf(stderr,"classic basic bounce omitted the diagonal contact: direction=%.17g velocity=(%.17g,%.17g)\n",
      created->direction,created->hspeed,created->vspeed); return 1;
  }
  collision_sprites[0].w=collision_sprites[0].h=2;
  collision_sprites[0].mr=collision_sprites[0].mb=1;
  created->x=0; created->y=100; created->xprevious=0; created->yprevious=100;
  created->direction=created->speed=created->hspeed=created->vspeed=0;
  contact->x=5; contact->y=100; contact->solid=0;
  gml_colgrid_invalidate(&vm);
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
  created->x=4;
  (void)gml_builtin_call(&vm,"move_contact_solid",contact_solid_args,2);
  if(created->x!=4){
    fprintf(stderr,"move_contact_solid moved an initially overlapping instance: x=%.0f\n",created->x); return 1;
  }
  contact->solid=0;
  (void)gml_builtin_call(&vm,"move_contact",&contact_direction,1);
  if(created->x!=4){
    fprintf(stderr,"move_contact moved an initially overlapping instance: x=%.0f\n",created->x); return 1;
  }
  created->x=0;
  GmlVal fractional_contact_args[2]={vreal(0),vreal(4.6)};
  (void)gml_builtin_call(&vm,"move_contact_solid",fractional_contact_args,2);
  if(created->x!=5){
    fprintf(stderr,"move_contact_solid did not round a positive maximum distance: x=%.0f\n",created->x); return 1;
  }
  collision_sprites[0].w=collision_sprites[0].h=1;
  collision_sprites[0].mr=collision_sprites[0].mb=0;
  created->x=0.49; created->y=100; contact->x=0; contact->y=100;
  GmlVal rounded_mask_args[3]={vreal(created->x),vreal(created->y),vreal(1)};
  GmlVal rounded_mask_hit=gml_builtin_call(&vm,"place_meeting",rounded_mask_args,3);
  if(rounded_mask_hit.t!=V_REAL || rounded_mask_hit.d!=1){
    fprintf(stderr,"classic precise mask did not use its rounded instance origin\n"); return 1;
  }
  uint8_t edge_mask=0x80;
  collision_sprites[0].w=2; collision_sprites[0].h=1;
  collision_sprites[0].mr=1; collision_sprites[0].mb=0;
  collision_sprites[0].collision_kind=0;
  collision_sprites[0].mask=&edge_mask;
  collision_sprites[0].mask_rowb=collision_sprites[0].mask_count=1;
  created->x=0.5; contact->x=0;
  rounded_mask_args[0]=vreal(created->x);
  rounded_mask_hit=gml_builtin_call(&vm,"place_meeting",rounded_mask_args,3);
  if(rounded_mask_hit.t!=V_REAL || rounded_mask_hit.d!=1){
    fprintf(stderr,"classic collision geometry did not round a half coordinate to even\n"); return 1;
  }
  collision_sprites[0].mask=NULL;
  collision_sprites[0].mask_rowb=collision_sprites[0].mask_count=0;
  collision_sprites[0].collision_kind=1;
  collision_sprites[0].w=collision_sprites[0].h=2;
  collision_sprites[0].mr=collision_sprites[0].mb=1;
  win.classic_version=0;
  win.bytecode=15;
  win.option_flags=0;
  if(anygm_policy_round_collision_bounds(&win)){
    fprintf(stderr,"Studio bytecode-15 unexpectedly enabled rounded collision bounds\n"); return 1;
  }
  win.option_flags=UINT64_C(0x08000000);
  if(!anygm_policy_round_collision_bounds(&win)){
    fprintf(stderr,"Studio collision-compatibility option did not enable rounded bounds\n"); return 1;
  }
  win.option_flags=0;
  win.bytecode=16;
  if(!anygm_policy_round_collision_bounds(&win)){
    fprintf(stderr,"Studio bytecode-16 did not enable rounded collision bounds\n"); return 1;
  }
  collision_sprites[0].w=collision_sprites[0].h=1;
  collision_sprites[0].mr=collision_sprites[0].mb=0;
  created->x=0; created->y=0; contact->x=0; contact->y=1;
  gml_colgrid_invalidate(&vm);
  rounded_mask_args[0]=vreal(0); rounded_mask_args[1]=vreal(0.4);
  rounded_mask_hit=gml_builtin_call(&vm,"place_meeting",rounded_mask_args,3);
  if(rounded_mask_hit.t!=V_REAL || rounded_mask_hit.d!=0){
    fprintf(stderr,"Studio bytecode-16 collision did not round fractional mask bounds\n"); return 1;
  }
  rounded_mask_args[1]=vreal(0.6);
  rounded_mask_hit=gml_builtin_call(&vm,"place_meeting",rounded_mask_args,3);
  if(rounded_mask_hit.t!=V_REAL || rounded_mask_hit.d!=1){
    fprintf(stderr,"Studio bytecode-16 collision rounded fractional mask bounds incorrectly\n"); return 1;
  }
  collision_sprites[0].w=collision_sprites[0].h=2;
  collision_sprites[0].mr=collision_sprites[0].mb=1;
  contact->x=5; contact->y=100;
  created->x=4; created->y=100; contact->solid=1;
  gml_colgrid_invalidate(&vm);
  (void)gml_builtin_call(&vm,"move_contact_solid",contact_solid_args,2);
  if(created->x!=3){
    fprintf(stderr,"Studio move_contact_solid did not recover an initial overlap: x=%.0f\n",created->x); return 1;
  }
  created->x=0; contact->solid=0;
  (void)gml_builtin_call(&vm,"move_contact_solid",fractional_contact_args,2);
  if(created->x!=4){
    fprintf(stderr,"Studio move_contact_solid did not retain fractional-distance truncation: x=%.0f\n",created->x); return 1;
  }
  win.classic_version=800;
  contact->solid=1;
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
  created->x=14; created->y=25; vm.action_relative=1;
  GmlVal snap_args[2]={vreal(8),vreal(6)};
  (void)gml_builtin_call(&vm,"action_snap",snap_args,2);
  vm.action_relative=0;
  if(created->x!=16 || created->y!=24){
    fprintf(stderr,"legacy snap action did not use absolute grid spacing: (%.1f,%.1f)\n",
      created->x,created->y); return 1;
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
  vm.cur_self=created;
  GmlVal alarm_set_args[2]={vreal(1),vreal(6.4)};
  (void)gml_builtin_call(&vm,"alarm_set",alarm_set_args,2);
  GmlVal alarm_index=vreal(1);
  GmlVal alarm_value=gml_builtin_call(&vm,"alarm_get",&alarm_index,1);
  vm.cur_self=NULL;
  if(created->alarm[1]!=6 || alarm_value.t!=V_REAL || alarm_value.d!=6){
    fprintf(stderr,"alarm_set/get accessor mismatch: stored=%.3f read=%.3f\n",
      created->alarm[1],alarm_value.t==V_REAL?alarm_value.d:-999.0); return 1;
  }
  GmlVal message_args[4]={vstr("prompt"),vstr(""),vstr("accept"),vstr("cancel")};
  GmlVal message_result=gml_builtin_call(&vm,"show_message_ext",message_args,4);
  if(message_result.t!=V_REAL || message_result.d!=2){
    fprintf(stderr,"non-interactive message button selection mismatch: %.0f\n",message_result.d); return 1;
  }
  GmlVal sleep_args[2]={vreal(1000),vreal(1)};
  (void)gml_builtin_call(&vm,"action_sleep",sleep_args,2);
  (void)gml_builtin_call(&vm,"sleep",sleep_args,1);
  GmlVal another_room_args[2]={vreal(1),vreal(21)};
  (void)gml_builtin_call(&vm,"action_another_room",another_room_args,2);
  GmlVal *transition_kind=gml_varmap_get(&vm.globals,"transition_kind");
  if(vm.pending_room!=1 || !transition_kind || transition_kind->t!=V_REAL ||
     transition_kind->d!=21){
    fprintf(stderr,"classic another-room action mismatch: pending=%d transition=%.0f\n",
      vm.pending_room,transition_kind&&transition_kind->t==V_REAL?transition_kind->d:-1.0);
    return 1;
  }
  vm.pending_room=-1;
  (void)gml_builtin_call(&vm,"action_restart_game",NULL,0);
  if(vm.game_end!=2){ fprintf(stderr,"classic restart action did not request a cold boot\n"); return 1; }
  vm.game_end=0;
  (void)gml_builtin_call(&vm,"action_end_game",NULL,0);
  if(vm.game_end!=1){ fprintf(stderr,"classic end action did not request shutdown\n"); return 1; }
  vm.game_end=0;
  created->alarm[0]=created->alarm[1]=1;
  contact->alarm[0]=contact->alarm[1]=1;
  late_order->alarm[0]=late_order->alarm[1]=1;
  created->gravity=0; created->gravity_direction=270;
  created->hspeed=-1e-16; created->vspeed=0;
  GmlInstance *friction_probe=find_slot(&vm,100000); if(!friction_probe)return 1;
  friction_probe->direction=0; friction_probe->speed=-0.4;
  friction_probe->hspeed=-0.4; friction_probe->vspeed=0; friction_probe->friction=0.2;
  double friction_position=friction_probe->x;
  double position_before_step=created->x;
  fixture_joystick_button3=1;
  gml_vm_step(&vm);
  fixture_joystick_button3=0;
  if(created->x!=position_before_step+5 || created->xprevious!=position_before_step || created->hspeed!=0){
    fprintf(stderr,"previous position/cardinal gravity mismatch: x=%.0f previous=%.0f hspeed=%.17g\n",
      created->x,created->xprevious,created->hspeed); return 1;
  }
  if(fabs(friction_probe->speed+0.2)>1e-9 || fabs(friction_probe->x-(friction_position-0.2))>1e-9){
    fprintf(stderr,"classic negative-speed friction mismatch: speed=%.3f x=%.3f\n",
      friction_probe->speed,friction_probe->x); return 1;
  }
  GmlInstance *same_step_spawn=NULL;
  for(int i=0;i<vm.inst_count;i++) if(vm.inst[i].active && !vm.inst[i].marked &&
      vm.inst[i].obj==1 && vm.inst[i].x>=50 && vm.inst[i].x<70){ same_step_spawn=&vm.inst[i]; break; }
  if(!same_step_spawn || same_step_spawn->x!=60){
    fprintf(stderr,"classic same-step movement/end-step mismatch: x=%.0f\n",
      same_step_spawn?same_step_spawn->x:-1.0); return 1;
  }
  GmlVal *step_order=gml_varmap_get(&vm.globals,"step_order");
  if(!step_order || step_order->t!=V_REAL || step_order->d!=1122){
    fprintf(stderr,"classic object-group Step order mismatch: %.0f\n",
      step_order&&step_order->t==V_REAL?step_order->d:-1.0); return 1;
  }
  GmlVal *persistent_alias_len=gml_varmap_get(&vm.globals,"persistent_alias_len");
  GmlVal *persistent_alias_value=gml_varmap_get(&vm.globals,"persistent_alias_value");
  if(!persistent_alias_len || persistent_alias_len->t!=V_REAL || persistent_alias_len->d!=1 ||
     !persistent_alias_value || persistent_alias_value->t!=V_REAL || persistent_alias_value->d!=7){
    fprintf(stderr,"persistent array/local alias lifetime mismatch: len=%.0f value=%.0f\n",
      persistent_alias_len&&persistent_alias_len->t==V_REAL?persistent_alias_len->d:-1.0,
      persistent_alias_value&&persistent_alias_value->t==V_REAL?persistent_alias_value->d:-1.0);
    return 1;
  }
  if(global_array_value(&vm,"view_surface_id",0)!=2468){
    fprintf(stderr,"direct view-surface array assignment was not global: %.0f\n",
      global_array_value(&vm,"view_surface_id",0)); return 1;
  }
  if(gml_global_num(&vm,"cursor_sprite")!=2469){
    fprintf(stderr,"built-in cursor sprite assignment was not global: %.0f\n",
      gml_global_num(&vm,"cursor_sprite")); return 1;
  }
  GmlVal *alarm_order=gml_varmap_get(&vm.globals,"alarm_order");
  if(!alarm_order || alarm_order->t!=V_REAL || alarm_order->d!=112334){
    fprintf(stderr,"classic alarm/object order mismatch: %.0f\n",
      alarm_order&&alarm_order->t==V_REAL?alarm_order->d:-1.0); return 1;
  }
  GmlVal *key_order=gml_varmap_get(&vm.globals,"key_order");
  if(!key_order || key_order->t!=V_REAL || key_order->d!=112){
    fprintf(stderr,"classic keyboard/object order mismatch: %.0f\n",
      key_order&&key_order->t==V_REAL?key_order->d:-1.0); return 1;
  }
  GmlVal *mouse_order=gml_varmap_get(&vm.globals,"mouse_order");
  if(!mouse_order || mouse_order->t!=V_REAL || mouse_order->d!=112){
    fprintf(stderr,"classic mouse/object order mismatch: %.0f\n",
      mouse_order&&mouse_order->t==V_REAL?mouse_order->d:-1.0); return 1;
  }
  GmlVal *joystick_event_hits=gml_varmap_get(&vm.globals,"joystick_event_hits");
  if(!joystick_event_hits || joystick_event_hits->t!=V_REAL || joystick_event_hits->d!=1){
    fprintf(stderr,"classic joystick button event mismatch: %.0f\n",
      joystick_event_hits&&joystick_event_hits->t==V_REAL?joystick_event_hits->d:-1.0); return 1;
  }
  GmlVal *trigger_hits=gml_varmap_get(&vm.globals,"trigger_hits");
  GmlVal *trigger_order=gml_varmap_get(&vm.globals,"trigger_order");
  if(!trigger_hits || trigger_hits->t!=V_REAL || trigger_hits->d!=3 ||
     !trigger_order || trigger_order->t!=V_REAL || trigger_order->d!=112){
    fprintf(stderr,"classic trigger did not fire\n"); return 1;
  }
  *gml_varmap_put(&vm.globals,"secondary_boundary_order")=vreal(0);
  for(int i=0;i<vm.inst_count;i++) if(vm.inst[i].active && !vm.inst[i].marked){
    vm.inst[i].x=400; vm.inst[i].y=100;
    vm.inst[i].hspeed=vm.inst[i].vspeed=vm.inst[i].gravity=0;
    vm.inst[i].sprite_index=vm.inst[i].mask_index=-1;
    vm.inst[i].image_xscale=vm.inst[i].image_yscale=1;
  }
  gml_vm_step(&vm);
  GmlVal *boundary_order=gml_varmap_get(&vm.globals,"boundary_order");
  if(!boundary_order || boundary_order->t!=V_REAL || boundary_order->d!=11223344){
    fprintf(stderr,"classic boundary/object order mismatch: %.0f\n",
      boundary_order&&boundary_order->t==V_REAL?boundary_order->d:-1.0); return 1;
  }
  GmlVal *view_boundary_order=gml_varmap_get(&vm.globals,"secondary_boundary_order");
  if(!view_boundary_order || view_boundary_order->t!=V_REAL || view_boundary_order->d!=11223344){
    fprintf(stderr,"classic secondary-view boundary order mismatch: %.0f\n",
      view_boundary_order&&view_boundary_order->t==V_REAL?view_boundary_order->d:-1.0); return 1;
  }
  created->gravity=created->vspeed=0;
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
  /* Bytecode-15 legacy views use this limited-speed rule. Exercise it directly so the neutral
   * assertion cannot advance or perturb the surrounding event-order fixture. */
  if(gml_legacy_view_follow_axis(0,400,100,10,3)!=3 ||
     gml_legacy_view_follow_axis(0,400,100,10,0)!=0 ||
     gml_legacy_view_follow_axis(0,400,100,10,-1)!=310){
    fprintf(stderr,"bytecode-15 legacy-view follow rule mismatch\n"); return 1;
  }
  {
    GmlVal camera_args[10]={vreal(0),vreal(0),vreal(100),vreal(100),vreal(0),
      vreal(created->id),vreal(4),vreal(-1),vreal(10),vreal(50)};
    GmlVal camera=gml_builtin_call(&vm,"camera_create_view",camera_args,10);
    if(camera.t!=V_REAL || camera.d<0){
      fprintf(stderr,"studio camera follow fixture allocation failed\n"); return 1;
    }
    int camera_id=(int)camera.d;
    gml_vm_step(&vm);
    if(fabs(global_array_value(&vm,"__gml_camera_x",camera_id)-4)>1e-9 ||
       fabs(global_array_value(&vm,"__gml_camera_y",camera_id)-50.4)>1e-9){
      fprintf(stderr,"studio camera limited follow mismatch: x=%.3f y=%.3f\n",
        global_array_value(&vm,"__gml_camera_x",camera_id),
        global_array_value(&vm,"__gml_camera_y",camera_id)); return 1;
    }
    GmlVal speed_args[3]={camera,vreal(-1),vreal(-1)};
    (void)gml_builtin_call(&vm,"camera_set_view_speed",speed_args,3);
    gml_vm_step(&vm);
    if(fabs(global_array_value(&vm,"__gml_camera_x",camera_id)-130.6)>1e-9 ||
       fabs(global_array_value(&vm,"__gml_camera_y",camera_id)-50.4)>1e-9){
      fprintf(stderr,"studio camera snap/fraction mismatch: x=%.3f y=%.3f\n",
        global_array_value(&vm,"__gml_camera_x",camera_id),
        global_array_value(&vm,"__gml_camera_y",camera_id)); return 1;
    }
    (void)gml_builtin_call(&vm,"camera_destroy",&camera,1);
  }
  gml_set_global_arr(&vm,"background_x",0,123);
  gml_set_global_arr(&vm,"view_xview",0,77);
  *gml_varmap_put(&vm.globals,"room_speed")=vreal(55);
  gml_tile_layer_shift(&vm,300,8,9);
  uint32_t id=created->id; *gml_varmap_put(&created->vars,"value")=vreal(42);
  gml_room_enter(&vm,1);
  room_view_enabled=gml_varmap_get(&vm.globals,"view_enabled");
  if(!room_view_enabled || room_view_enabled->t!=V_REAL || room_view_enabled->d!=0){
    fprintf(stderr,"room flags did not disable the legacy view system: %.0f\n",
      room_view_enabled&&room_view_enabled->t==V_REAL?room_view_enabled->d:-1.0);
    return 1;
  }
  GmlInstance *slot=find_slot(&vm,id);
  if(!slot||!slot->room_dormant||slot->active){ fprintf(stderr,"room was not stored\n"); return 1; }
  int static_ci=gml_code_index_by_name(&win,"gml_Script_array_ext_callback");
  if(static_ci<0 || static_ci>=vm.code_static_count) return 1;
  vm.code_static_init[static_ci]=1;
  *gml_varmap_put(&vm.code_static[static_ci],"fixture_static")=vreal(73);
  GmlVal *timer_before_save=gml_varmap_get(&vm.globals,"fixture_time_source_handle");
  GmlVal *receiver_before_save=gml_varmap_get(&vm.globals,"fixture_clone_root");
  GmlInstance *callback_receiver=receiver_before_save && receiver_before_save->t==V_REAL
    ? gml_struct_find(&vm,(unsigned)receiver_before_save->d) : NULL;
  GmlVal *timer_callback=callback_receiver
    ? gml_varmap_get(&callback_receiver->vars,"callback") : NULL;
  if(!timer_before_save || timer_before_save->t!=V_REAL ||
     !timer_callback || timer_callback->t!=V_REAL) return 1;
  GmlVal restored_timer_args=gml_arr_new(2,vreal(0));
  gml_arr_set(restored_timer_args,1,vreal(4));
  GmlVal timer_reconfigure_args[7]={
    *timer_before_save,vreal(1),vreal(1),*timer_callback,
    restored_timer_args,vreal(2),vreal(1)
  };
  (void)gml_builtin_call(&vm,"time_source_reconfigure",timer_reconfigure_args,7);
  (void)gml_builtin_call(&vm,"time_source_start",timer_before_save,1);
  *gml_varmap_put(&vm.globals,"time_source_fixture")=vreal(0);
  size_t size=gml_vm_state_size(&vm),written=0,used=0; void *state=malloc(size);
  if(!state||!gml_vm_state_save(&vm,state,size,&written)||written!=size)return 1;
  /* Writing the same state twice has to produce the same bytes. The second write answers from
   * what the first one remembered, and a remembered answer that differs from the one it stands in
   * for would encode a different string for the same text: states written a frame apart would
   * stop matching, which is invisible until one is loaded. A host recording a rewind buffer
   * writes a state every frame, so this is the ordinary case, not an unusual one. */
  { size_t again_written=0; void *again=malloc(size);
    if(!again){ free(state); return 1; }
    int same=gml_vm_state_save(&vm,again,size,&again_written) && again_written==written &&
             memcmp(again,state,written)==0;
    free(again);
    if(!same){
      fprintf(stderr,"state written twice differs the second time\n");
      free(state); return 1;
    } }
  gml_room_enter(&vm,0); slot=find_slot(&vm,id);
  if(!slot||!slot->active||slot->room_dormant)return 1;
  *gml_varmap_put(&slot->vars,"value")=vreal(99);
  vm.code_static_init[static_ci]=0;
  *gml_varmap_put(&vm.code_static[static_ci],"fixture_static")=vreal(99);
  if(!gml_vm_state_load(&vm,state,written,&used)||used!=written)return 1;
  gml_room_enter(&vm,0); slot=find_slot(&vm,id);
  GmlVal *value=slot?gml_varmap_get(&slot->vars,"value"):NULL;
  GmlVal *room_speed=gml_varmap_get(&vm.globals,"room_speed");
  GmlVal *static_value=gml_varmap_get(&vm.code_static[static_ci],"fixture_static");
  GmlVal *timer_handle=gml_varmap_get(&vm.globals,"fixture_time_source_handle");
  GmlVal restored_timer_state=timer_handle?
    gml_builtin_call(&vm,"time_source_get_state",timer_handle,1):vundef();
  gml_time_sources_tick(&vm);
  GmlVal restored_timer_reps=timer_handle?
    gml_builtin_call(&vm,"time_source_get_reps_remaining",timer_handle,1):vundef();
  GmlVal *restored_timer_total=gml_varmap_get(&vm.globals,"time_source_fixture");
  int ok=slot&&slot->active&&!slot->room_dormant&&value&&value->t==V_REAL&&value->d==42 &&
    global_array_value(&vm,"background_x",0)==123 && global_array_value(&vm,"view_xview",0)==77 &&
    room_speed&&room_speed->t==V_REAL&&room_speed->d==55 && vm.n_tile_mut==1 &&
    vm.tile_mut[0].depth==300&&vm.tile_mut[0].dx==8&&vm.tile_mut[0].dy==9 &&
    vm.potential_max_rotation==30 && vm.potential_rotate_step==10 &&
    vm.potential_check_distance==1 && vm.potential_rotate_on_spot==1 &&
    vm.code_static_init[static_ci]==1 && static_value && static_value->t==V_REAL && static_value->d==73 &&
    timer_handle && timer_handle->t==V_REAL &&
    restored_timer_state.t==V_REAL && restored_timer_state.d==1 &&
    restored_timer_reps.t==V_REAL && restored_timer_reps.d==1 &&
    restored_timer_total && restored_timer_total->t==V_REAL && restored_timer_total->d==4;
  if(!ok) fprintf(stderr,
    "persistent room state did not roundtrip: slot=%d value=%.0f bg=%.0f view=%.0f speed=%.0f tiles=%d depth=%d shift=(%.0f,%.0f)\n",
    slot&&slot->active&&!slot->room_dormant,value&&value->t==V_REAL?value->d:-1,
    global_array_value(&vm,"background_x",0),global_array_value(&vm,"view_xview",0),
    room_speed&&room_speed->t==V_REAL?room_speed->d:-1,vm.n_tile_mut,
    vm.n_tile_mut?vm.tile_mut[0].depth:-1,vm.n_tile_mut?vm.tile_mut[0].dx:0,vm.n_tile_mut?vm.tile_mut[0].dy:0);
  collision_sprites[0].n_frames=2;
  slot->sprite_index=slot->mask_index=0;
  slot->image_index=0;
  slot->image_speed=0.5;
  uint32_t animation_id=slot->id;
  gml_vm_step(&vm);
  slot=find_slot(&vm,animation_id);
  if(!slot){ fprintf(stderr,"classic animation fixture instance disappeared\n"); return 1; }
  if(slot->image_index!=0){
    fprintf(stderr,"classic animation advanced before the draw phase: index=%.2f\n",slot->image_index); return 1;
  }
  /* The advance is owed by the draw and taken at the start of the next step, so a state serialized
   * on the frame boundary holds the animation of the frame it was saved from rather than the one
   * after it. Asserting it immediately after post_draw would be asserting the superseded contract:
   * what has to be true is that the draw does not advance it and the next step does. */
  gml_vm_post_draw(&vm);
  if(slot->image_index!=0){
    fprintf(stderr,"classic animation advanced inside the draw phase: index=%.2f\n",slot->image_index); return 1;
  }
  gml_vm_step(&vm);
  slot=find_slot(&vm,animation_id);
  if(!slot){ fprintf(stderr,"classic animation fixture instance disappeared\n"); return 1; }
  if(slot->image_index!=0.5){
    fprintf(stderr,"classic animation did not advance on the step owed by the draw: index=%.2f\n",slot->image_index); return 1;
  }
  slot->sprite_index=slot->mask_index=-1;
  slot->image_index=0;
  slot->image_speed=0.5;
  gml_vm_post_draw(&vm);
  gml_vm_step(&vm);
  slot=find_slot(&vm,animation_id);
  if(!slot){ fprintf(stderr,"classic animation fixture instance disappeared\n"); return 1; }
  if(slot->image_index!=0.5){
    fprintf(stderr,"classic sprite-less drawing frame did not advance: index=%.2f\n",slot->image_index); return 1;
  }
  win.classic_version=0;
  win.bytecode=15;
  GmlInstance *studio_contact=gml_instance_create(&vm,40,40,1); if(!studio_contact)return 1;
  uint32_t studio_contact_id=studio_contact->id;
  GmlInstance *studio_actor=gml_instance_create(&vm,40,40,2); if(!studio_actor)return 1;
  uint32_t studio_actor_id=studio_actor->id;
  studio_contact=find_slot(&vm,studio_contact_id); studio_actor=find_slot(&vm,studio_actor_id);
  if(!studio_contact || !studio_actor)return 1;
  studio_contact->sprite_index=studio_contact->mask_index=0;
  studio_actor->sprite_index=studio_actor->mask_index=0;
  studio_contact->image_xscale=studio_contact->image_yscale=1;
  studio_actor->image_xscale=studio_actor->image_yscale=1;
  studio_contact->solid=1;
  studio_contact->speed=studio_contact->hspeed=studio_contact->vspeed=0;
  studio_actor->speed=1; studio_actor->direction=270;
  studio_actor->hspeed=0; studio_actor->vspeed=-1;
  studio_contact->xprevious=studio_contact->x; studio_contact->yprevious=studio_contact->y;
  studio_actor->y=41; studio_actor->xprevious=studio_actor->x; studio_actor->yprevious=40;
  *gml_varmap_put(&vm.globals,"studio_solid_hits")=vreal(0);
  gml_colgrid_invalidate(&vm);
  gml_vm_step(&vm);
  studio_actor=find_slot(&vm,studio_actor_id);
  GmlVal *studio_solid_hits=gml_varmap_get(&vm.globals,"studio_solid_hits");
  if(!studio_actor || studio_actor->y!=39 || !studio_solid_hits ||
     studio_solid_hits->t!=V_REAL || studio_solid_hits->d!=1){
    fprintf(stderr,"Studio 1.x solid collision current-position mismatch: y=%.0f hits=%.0f\n",
      studio_actor?studio_actor->y:-1.0,
      studio_solid_hits&&studio_solid_hits->t==V_REAL?studio_solid_hits->d:-1.0); return 1;
  }

  /* A Studio physics room uses the fixture polygons stored in OBJT rather than sprite masks.
   * Exercise the contact, sensor and negative collision-group rules with the existing directed
   * collision event so the test also proves that a physical pair is dispatched only once. */
  GmlObject saved_contact_object=vm.objects[1];
  GmlObject saved_actor_object=vm.objects[2];
  unsigned char *saved_active=malloc((size_t)vm.inst_count);
  if(!saved_active) return 1;
  for(int index=0;index<vm.inst_count;index++){
    saved_active[index]=(unsigned char)vm.inst[index].active;
    if(&vm.inst[index]!=studio_contact && &vm.inst[index]!=studio_actor)
      vm.inst[index].active=0;
  }
  GmlObject *physics_objects[2]={&vm.objects[1],&vm.objects[2]};
  for(int object_index=0;object_index<2;object_index++){
    GmlObject *object=physics_objects[object_index];
    object->physics_enabled=1;
    object->physics_shape=1;
    object->physics_density=1.0;
    object->physics_area_px=100.0;
    object->physics_point_count=4;
    object->physics_point[0][0]=-5; object->physics_point[0][1]=-5;
    object->physics_point[1][0]= 5; object->physics_point[1][1]=-5;
    object->physics_point[2][0]= 5; object->physics_point[2][1]= 5;
    object->physics_point[3][0]=-5; object->physics_point[3][1]= 5;
  }
  vm.objects[1].physics_kinematic=1;
  *gml_varmap_put(&vm.globals,"__physics_world_scale_room")=vreal(vm.room_index);
  studio_contact->active=studio_actor->active=1;
  studio_contact->marked=studio_actor->marked=0;
  studio_contact->x=studio_contact->xprevious=120;
  studio_contact->y=studio_contact->yprevious=120;
  studio_actor->x=studio_actor->xprevious=127;
  studio_actor->y=studio_actor->yprevious=120;
  *gml_varmap_put(&vm.globals,"studio_solid_hits")=vreal(0);
  gml_colgrid_invalidate(&vm);
  gml_vm_instances_run_collisions(&vm);
  studio_solid_hits=gml_varmap_get(&vm.globals,"studio_solid_hits");
  if(fabs(studio_actor->x-129.99)>1e-6 || studio_actor->y!=119 ||
     !studio_solid_hits || studio_solid_hits->t!=V_REAL || studio_solid_hits->d!=1){
    fprintf(stderr,"physics fixture contact mismatch: position=(%.3f,%.3f) hits=%.0f\n",
      studio_actor->x,studio_actor->y,
      studio_solid_hits&&studio_solid_hits->t==V_REAL?studio_solid_hits->d:-1.0);
    free(saved_active); return 1;
  }
  vm.objects[1].physics_sensor=1;
  studio_actor->x=studio_actor->xprevious=127;
  studio_actor->y=studio_actor->yprevious=120;
  studio_solid_hits->d=0;
  gml_vm_instances_run_collisions(&vm);
  if(studio_actor->x!=127 || studio_actor->y!=119 || studio_solid_hits->d!=1){
    fprintf(stderr,"physics sensor resolution mismatch: position=(%.3f,%.3f) hits=%.0f\n",
      studio_actor->x,studio_actor->y,studio_solid_hits->d);
    free(saved_active); return 1;
  }
  vm.objects[1].physics_sensor=0;
  vm.objects[1].physics_group=vm.objects[2].physics_group=-7;
  studio_actor->x=studio_actor->xprevious=127;
  studio_actor->y=studio_actor->yprevious=120;
  studio_solid_hits->d=0;
  gml_vm_instances_run_collisions(&vm);
  if(studio_actor->x!=127 || studio_actor->y!=120 || studio_solid_hits->d!=0){
    fprintf(stderr,"negative physics collision group was not suppressed\n");
    free(saved_active); return 1;
  }
  vm.objects[1]=saved_contact_object;
  vm.objects[2]=saved_actor_object;
  for(int index=0;index<vm.inst_count;index++) vm.inst[index].active=saved_active[index];
  free(saved_active);
  *gml_varmap_put(&vm.globals,"__physics_world_scale_room")=vundef();
  studio_contact->marked=1; studio_actor->marked=1;

  /* GMS2 changed solid dispatch to expose pre-movement coordinates to the event. Keep the
   * two families explicit so fixing one cannot silently alter the other. */
  win.bytecode=17;
  studio_contact=gml_instance_create(&vm,80,80,1); if(!studio_contact)return 1;
  studio_contact_id=studio_contact->id;
  studio_actor=gml_instance_create(&vm,80,80,2); if(!studio_actor)return 1;
  studio_actor_id=studio_actor->id;
  studio_contact=find_slot(&vm,studio_contact_id); studio_actor=find_slot(&vm,studio_actor_id);
  if(!studio_contact || !studio_actor)return 1;
  studio_contact->sprite_index=studio_contact->mask_index=0;
  studio_actor->sprite_index=studio_actor->mask_index=0;
  studio_contact->image_xscale=studio_contact->image_yscale=1;
  studio_actor->image_xscale=studio_actor->image_yscale=1;
  studio_contact->solid=1;
  studio_contact->speed=studio_contact->hspeed=studio_contact->vspeed=0;
  studio_actor->speed=1; studio_actor->direction=270;
  studio_actor->hspeed=0; studio_actor->vspeed=-1;
  studio_contact->xprevious=studio_contact->x; studio_contact->yprevious=studio_contact->y;
  studio_actor->y=81; studio_actor->xprevious=studio_actor->x; studio_actor->yprevious=80;
  /* A Studio alarm without a corresponding event is a normal user-visible counter.  It must not
   * be consumed by the automatic alarm phase. */
  studio_actor->alarm[5]=1;
  *gml_varmap_put(&vm.globals,"studio_solid_hits")=vreal(0);
  gml_colgrid_invalidate(&vm);
  gml_vm_step(&vm);
  studio_actor=find_slot(&vm,studio_actor_id);
  studio_solid_hits=gml_varmap_get(&vm.globals,"studio_solid_hits");
  if(!studio_actor || studio_actor->y!=80 || studio_actor->alarm[5]!=1 || !studio_solid_hits ||
     studio_solid_hits->t!=V_REAL || studio_solid_hits->d!=1){
    fprintf(stderr,"GMS2 solid/alarm semantics mismatch: y=%.0f alarm=%.0f hits=%.0f\n",
      studio_actor?studio_actor->y:-1.0,
      studio_actor?studio_actor->alarm[5]:-1.0,
      studio_solid_hits&&studio_solid_hits->t==V_REAL?studio_solid_hits->d:-1.0); return 1;
  }

  /* Studio preserves an empty native Alarm declaration even though it has no CODE entry.  Its
   * counter must run to completion; this differs from the truly undeclared slot checked above. */
  GmlObject *studio_actor_object=&vm.objects[studio_actor->obj];
  int empty_event_index=studio_actor_object->n_events;
  void *grown_events=realloc(studio_actor_object->events,
    (size_t)(empty_event_index+1)*sizeof(*studio_actor_object->events));
  if(!grown_events) return 1;
  studio_actor_object->events=grown_events;
  studio_actor_object->events[empty_event_index].evtype=2;
  studio_actor_object->events[empty_event_index].subtype=5;
  studio_actor_object->events[empty_event_index].code=-1;
  studio_actor_object->n_events++;
  studio_actor->alarm[5]=1;
  gml_vm_step(&vm);
  studio_actor=find_slot(&vm,studio_actor_id);
  if(!studio_actor || studio_actor->alarm[5]!=-1){
    fprintf(stderr,"empty native alarm declaration did not count down: %.0f\n",
      studio_actor?studio_actor->alarm[5]:-999.0); return 1;
  }

  /* Studio treats a value within its real-comparison epsilon of an integer as that integer when
   * advancing an instance animation. Repeated decimal image speeds otherwise leave e.g. 0.2 * 5
   * just below one and render the preceding subimage for an extra frame. */
  studio_actor->image_index=0;
  studio_actor->image_speed=0.2;
  studio_actor->sprite_index=studio_actor->mask_index=0;
  for(int frame=0;frame<5;frame++) gml_vm_step(&vm);
  studio_actor=find_slot(&vm,studio_actor_id);
  if(!studio_actor || studio_actor->image_index!=1.0){
    fprintf(stderr,"Studio fractional animation did not snap at its epsilon: %.17g\n",
      studio_actor?studio_actor->image_index:-1.0); return 1;
  }

  /* Studio takes a fixed event snapshot at frame start, but dead slots from older frames are
   * safe to recycle.  Exercise both halves together: a newly created instance placed in an old
   * low slot must NOT receive Step in its birth frame, and create/destroy churn must stay bounded
   * without moving any live instance pointer. */
  int changed_before=gml_instance_number(&vm,1);
  *gml_varmap_put(&vm.globals,"spawned_once")=vreal(0);
  *gml_varmap_put(&vm.globals,"churn_pool")=vreal(0);
  *gml_varmap_put(&vm.globals,"changed_step_hits")=vreal(0);
  GmlInstance *stable=find_slot(&vm,id);
  uint32_t stable_id=stable?stable->id:0;
  gml_vm_step(&vm);
  GmlVal *changed_hits=gml_varmap_get(&vm.globals,"changed_step_hits");
  if(!stable || find_slot(&vm,stable_id)!=stable || !changed_hits ||
     changed_hits->t!=V_REAL || changed_hits->d!=changed_before){
    fprintf(stderr,"Studio recycled-slot snapshot mismatch: before=%d hits=%.0f stable=%d\n",
      changed_before,changed_hits&&changed_hits->t==V_REAL?changed_hits->d:-1.0,
      stable&&find_slot(&vm,stable_id)==stable); return 1;
  }
  *gml_varmap_put(&vm.globals,"spawned_once")=vreal(1);
  *gml_varmap_put(&vm.globals,"churn_pool")=vreal(1);
  int churn_base=vm.inst_count;
  int churn_producers=gml_instance_number(&vm,0);
  for(int frame=0;frame<96;frame++) gml_vm_step(&vm);
  *gml_varmap_put(&vm.globals,"churn_pool")=vreal(0);
  if(vm.inst_count>churn_base+churn_producers+2 || find_slot(&vm,stable_id)!=stable){
    fprintf(stderr,"instance pool churn was unbounded/moved a live slot: base=%d final=%d producers=%d stable=%d\n",
      churn_base,vm.inst_count,churn_producers,find_slot(&vm,stable_id)==stable); return 1;
  }
  /* Modern family-scoped with() walks newest-first and skips a member destroyed by an earlier
   * body. Use a dedicated clean family so unrelated event fixtures cannot mask either property. */
  for(int i=0;i<vm.inst_count;i++)
    if(vm.inst[i].active && !vm.inst[i].marked && vm.inst[i].obj==1) gml_instance_destroy(&vm,&vm.inst[i]);
  gml_vm_step(&vm);
  GmlInstance *with_old=gml_instance_create(&vm,1,0,1);
  GmlInstance *with_mid=gml_instance_create(&vm,2,0,1);
  GmlInstance *with_new=gml_instance_create(&vm,3,0,1);
  int with_code=gml_code_index_by_name(&win,"gml_Script_studio_with_order");
  if(!with_old || !with_mid || !with_new || with_code<0) return 1;
  *gml_varmap_put(&vm.globals,"with_victim")=vreal((double)with_old->id);
  int saved_classic=win.classic_version;
  win.classic_version=0;
  GmlVal with_result=gml_vm_run_code(&vm,with_code,with_new,with_new,NULL,0);
  win.classic_version=saved_classic;
  if(with_result.t!=V_REAL || with_result.d!=32){
    fprintf(stderr,"Studio with order/dead-member mismatch: %.0f\n",
      with_result.t==V_REAL?with_result.d:-1.0); return 1;
  }
  free(state); gml_vm_free(&vm);
  {
    int transition_ok=0;
    GmlVM transition_vm;
    if(!gml_vm_init(&transition_vm,&win,&file_services)){
      gml_room_enter(&transition_vm,0);
      *gml_varmap_put(&transition_vm.globals,"transition_alarm_fixture")=vreal(1);
      *gml_varmap_put(&transition_vm.globals,"transition_actor")=vreal(-1);
      *gml_varmap_put(&transition_vm.globals,"transition_step_hits")=vreal(0);
      GmlInstance *controller=gml_instance_create(&transition_vm,24,24,0);
      if(controller){
        controller->alarm[0]=1;
        gml_vm_step(&transition_vm);
        GmlVal *actor_id=gml_varmap_get(&transition_vm.globals,"transition_actor");
        GmlVal *step_hits=gml_varmap_get(&transition_vm.globals,"transition_step_hits");
        GmlInstance *actor=actor_id&&actor_id->t==V_REAL?
          find_slot(&transition_vm,(uint32_t)actor_id->d):NULL;
        transition_ok=transition_vm.room_index==1 && actor && actor->active &&
          actor->persistent && actor->x==1120 && step_hits &&
          step_hits->t==V_REAL && step_hits->d==0;
        if(!transition_ok)
          fprintf(stderr,
            "classic Alarm room transition phase mismatch: room=%d actor=%d x=%.0f steps=%.0f\n",
            transition_vm.room_index,actor&&actor->active,
            actor?actor->x:-1.0,step_hits&&step_hits->t==V_REAL?step_hits->d:-1.0);
      }
      gml_vm_free(&transition_vm);
    }
    ok=ok&&transition_ok;
  }
  gml_win_free(&win); unlink(path); unlink(startup); unlink(implicit_script); unlink(shadowed_alias_script); unlink(array_ext_callback); unlink(studio_with_order); unlink(condition); unlink(event); unlink(changed_trigger); unlink(create_order); unlink(joystick_event); unlink(solid_collision); unlink(destroy_reentry); unlink(cleanup_counter); unlink(instance_order); unlink(step); unlink(end_step); unlink(changed_step); unlink(included_path);
  for(int i=0;i<4;i++) unlink(alarm_files[i]);
  for(int i=0;i<2;i++) unlink(key_files[i]);
  for(int i=0;i<2;i++) unlink(mouse_files[i]);
  for(int i=0;i<4;i++) unlink(boundary_files[i]);
  for(int i=0;i<4;i++) unlink(view_boundary_files[i]);
  for(int i=0;i<3;i++) unlink(timeline_files[i]);
  rmdir(save_root);
  if(ok) puts("persistent room fixtures: ok");
  return ok?0:1;
}


int expect_persistent_lifecycle(void){
  return expect_persistent_lifecycle_exit_code()==0;
}
