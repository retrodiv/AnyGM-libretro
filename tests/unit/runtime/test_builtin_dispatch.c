/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "gml_builtin.h"
#include "gml_builtin_registry.h"
#include "gml_vm.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  const char *name;
  int value;
} ScriptFixture;

typedef struct {
  int held;
  int pressed;
  int released;
} MouseFixture;

static void mouse_fixture_read(
    void *userdata,double *room_x,double *room_y,double *gui_x,double *gui_y,
    double *window_x,double *window_y,int *held,int *pressed,int *released,int *wheel){
  MouseFixture *fixture=(MouseFixture *)userdata;
  if(room_x) *room_x=0;
  if(room_y) *room_y=0;
  if(gui_x) *gui_x=0;
  if(gui_y) *gui_y=0;
  if(window_x) *window_x=0;
  if(window_y) *window_y=0;
  if(held) *held=fixture->held;
  if(pressed) *pressed=fixture->pressed;
  if(released) *released=fixture->released;
  if(wheel) *wheel=0;
}

static void store_u32le(uint8_t *destination,uint32_t value){
  destination[0]=(uint8_t)value;
  destination[1]=(uint8_t)(value>>8);
  destination[2]=(uint8_t)(value>>16);
  destination[3]=(uint8_t)(value>>24);
}

static int expect_real(const char *case_name,GmlVal actual,double expected){
  if(actual.t==V_REAL && actual.d==expected) return 1;
  fprintf(stderr,"%s: expected %.17g, got type=%d value=%.17g\n",
          case_name,expected,actual.t,actual.d);
  return 0;
}

static GmlVal call_fast(GmlVM *vm,const char *name,
                        GmlVal *arguments,int count,int *ok){
  int id=gml_builtin_fast_id(vm,name);
  if(id<0){
    fprintf(stderr,"%s: expected a canonical fast builtin ID\n",name);
    *ok=0;
    return vundef();
  }
  return gml_builtin_call_fast_id(vm,id,name,arguments,count);
}

static int setup_fixture(GmlWin *win,GmlVM *vm){
  static const ScriptFixture scripts[]={
    {"gml_Script_abs",73},
    {"gml_Script_neutral_dispatch",74},
    {"gml_GlobalScript_neutral_global_dispatch",75},
    {"gml_Script_draw_neutral_dispatch",76},
    {"gml_Script_neutral_function_value",77},
    {"gml_Script_mouse_wheel_up",78},
    {"gml_Script_draw_set_blend_mode",79},
  };
  const int count=(int)(sizeof(scripts)/sizeof(scripts[0]));
  win->bytecode=17;
  win->size=256;
  win->owns=1;
  win->data=calloc(win->size,1);
  win->n_code=count;
  win->code=calloc((size_t)count,sizeof(*win->code));
  if(!win->data || !win->code) return 0;
  for(int index=0;index<count;index++){
    uint8_t *code=win->data+(size_t)index*8;
    store_u32le(code,(uint32_t)(0x84000000u|(uint16_t)scripts[index].value));
    store_u32le(code+4,0x9C050000u);
    win->code[index]=(GmlCode){
      .name=scripts[index].name,
      .start=(uint32_t)index*8,
      .length=8,
    };
  }
  win->n_chunks=1;
  memcpy(win->chunks[0].name,"ROOM",5);
  win->chunks[0].off=64;
  win->chunks[0].size=192;
  store_u32le(win->data+64,1);
  store_u32le(win->data+68,72);
  store_u32le(win->data+72,160);
  store_u32le(win->data+80,320);
  store_u32le(win->data+84,240);
  store_u32le(win->data+88,60);
  memcpy(win->data+160,"neutral_room",13);
  win->strs=calloc(1,sizeof(*win->strs));
  win->str_charoff=calloc(1,sizeof(*win->str_charoff));
  if(!win->strs || !win->str_charoff) return 0;
  win->strs[0]=(char *)win->data+160;
  win->str_charoff[0]=160;
  win->n_strs=1;
  return gml_vm_init(vm,win,NULL)==0;
}

static int room_dimension_mutation(GmlVM *vm){
  GmlRoom room={0};
  if(gml_vm_room_get(vm,0,&room)!=0 || room.width!=320 || room.height!=240){
    fprintf(stderr,"room dimension fixture baseline failed\n");
    return 0;
  }
  GmlVal width_args[]={vreal(0),vreal(640.75)};
  GmlVal height_args[]={vreal(0),vreal(360)};
  int ok=expect_real("room_set_width return",
                     gml_builtin_call(vm,"room_set_width",width_args,2),0) &&
         expect_real("room_set_height return",
                     gml_builtin_call(vm,"room_set_height",height_args,2),0) &&
         gml_vm_room_get(vm,0,&room)==0 && room.width==640 && room.height==360;
  if(!ok){
    fprintf(stderr,"room dimensions were not mutated: %ux%u\n",room.width,room.height);
    return 0;
  }

  size_t state_size=gml_vm_state_size(vm), written=0, used=0;
  void *state=malloc(state_size);
  if(!state || !gml_vm_state_save(vm,state,state_size,&written) || written!=state_size){
    fprintf(stderr,"room dimension state save failed\n");
    free(state);
    return 0;
  }
  width_args[1]=vreal(111);
  (void)gml_builtin_call(vm,"room_set_width",width_args,2);
  if(!gml_vm_state_load(vm,state,state_size,&used) || used!=state_size ||
     gml_vm_room_get(vm,0,&room)!=0 || room.width!=640 || room.height!=360){
    fprintf(stderr,"room dimensions did not survive state roundtrip\n");
    free(state);
    return 0;
  }
  free(state);

  GmlVal invalid_room[]={vreal(1),vreal(700)};
  GmlVal invalid_size[]={vreal(0),vreal(-1)};
  (void)gml_builtin_call(vm,"room_set_width",invalid_room,2);
  (void)gml_builtin_call(vm,"room_set_height",invalid_size,2);
  return gml_vm_room_get(vm,0,&room)==0 && room.width==640 && room.height==360;
}

static int exact_builtin_precedes_same_named_script(GmlVM *vm){
  GmlVal argument=vreal(-4);
  GmlVal direct=gml_builtin_call(vm,"abs",&argument,1);
  int id=gml_builtin_fast_id(vm,"abs");
  GmlVal cached=gml_builtin_call_fast_id(vm,id,"abs",&argument,1);
  GmlVal blend_argument=vreal(0);
  return id>=0 &&
         expect_real("exact builtin before same-named script",direct,4) &&
         expect_real("cached exact builtin parity",cached,4) &&
         expect_real("characterized hot exact name before script",
                     gml_builtin_call(vm,"draw_set_blend_mode",
                                      &blend_argument,1),0);
}

static int script_resolution_order(GmlVM *vm){
  int ok=1;
  ok&=expect_real("bare script fallback",
                  gml_builtin_call(vm,"neutral_dispatch",NULL,0),74);
  ok&=expect_real("prefixed script fallback",
                  gml_builtin_call(vm,"gml_Script_neutral_dispatch",NULL,0),74);
  ok&=expect_real("global-script fallback",
                  gml_builtin_call(vm,"neutral_global_dispatch",NULL,0),75);
  ok&=expect_real("script before broad prefix fallback",
                  gml_builtin_call(vm,"draw_neutral_dispatch",NULL,0),76);
  ok&=expect_real("script before registered late exact fallback",
                  gml_builtin_call(vm,"mouse_wheel_up",NULL,0),78);
  ok&=expect_real("deliberate unknown fallback",
                  gml_builtin_call(vm,"neutral_unknown_dispatch",NULL,0),0);
  return ok;
}

static int function_value_and_alias_resolution(GmlVM *vm){
  GmlVal callable=vreal((double)(GML_FUNCVAL_TAG|4));
  GmlVal function_result=gml_builtin_call(vm,"script_execute",&callable,1);
  GmlVal color_args[]={vreal(3),vreal(5),vreal(7)};
  GmlVal color=gml_builtin_call(vm,"make_color",color_args,3);
  GmlVal colour=gml_builtin_call(vm,"make_colour",color_args,3);
  double expected=(double)(3|(5<<8)|(7<<16));
  return expect_real("tagged function value dispatch",function_result,77) &&
         expect_real("canonical exact alias",color,expected) &&
         expect_real("alternate exact alias",colour,expected);
}

static int gain_conversion(GmlVM *vm){
  int ok=1;
  GmlVal decibels=vreal(-6.0);
  GmlVal linear=call_fast(vm,"db_to_lin",&decibels,1,&ok);
  GmlVal roundtrip=call_fast(vm,"lin_to_db",&linear,1,&ok);
  double expected=pow(10.0,-6.0/20.0);
  if(linear.t!=V_REAL || fabs(linear.d-expected)>1e-12 ||
     roundtrip.t!=V_REAL || fabs(roundtrip.d+6.0)>1e-12){
    fprintf(stderr,"gain conversion mismatch: linear=%.17g roundtrip=%.17g\n",
            linear.d,roundtrip.d);
    return 0;
  }
  return ok;
}

static int mouse_none_semantics(GmlVM *vm){
  MouseFixture fixture={0};
  GmlVal none=vreal(0);
  GmlVal any=vreal(-1);
  vm->input.userdata=&fixture;
  vm->input.mouse=mouse_fixture_read;
  int ok=
    expect_real("mouse none held without input",
                gml_builtin_call(vm,"mouse_check_button",&none,1),1) &&
    expect_real("mouse none pressed without input",
                gml_builtin_call(vm,"mouse_check_button_pressed",&none,1),1) &&
    expect_real("mouse none released without input",
                gml_builtin_call(vm,"mouse_check_button_released",&none,1),1);
  fixture.held=1;
  fixture.pressed=1;
  ok=ok &&
    expect_real("mouse none held with left input",
                gml_builtin_call(vm,"mouse_check_button",&none,1),0) &&
    expect_real("mouse none pressed with left edge",
                gml_builtin_call(vm,"mouse_check_button_pressed",&none,1),0) &&
    expect_real("mouse any pressed with left edge",
                gml_builtin_call(vm,"mouse_check_button_pressed",&any,1),1) &&
    expect_real("mouse none released without release edge",
                gml_builtin_call(vm,"mouse_check_button_released",&none,1),1);
  fixture.held=0;
  fixture.pressed=0;
  fixture.released=1;
  ok=ok &&
    expect_real("mouse none held after release",
                gml_builtin_call(vm,"mouse_check_button",&none,1),1) &&
    expect_real("mouse none pressed after release",
                gml_builtin_call(vm,"mouse_check_button_pressed",&none,1),1) &&
    expect_real("mouse none released with left edge",
                gml_builtin_call(vm,"mouse_check_button_released",&none,1),0);
  vm->input.mouse=NULL;
  vm->input.userdata=NULL;
  return ok;
}

static int ds_fast_interface(GmlVM *vm){
  int ok=1;
  GmlVal map=gml_builtin_call(vm,"ds_map_create",NULL,0);
  GmlVal first_entry[]={map,vreal(1),vreal(11)};
  GmlVal second_entry[]={map,vreal(2),vreal(22)};
  (void)gml_builtin_call(vm,"ds_map_add",first_entry,3);
  (void)gml_builtin_call(vm,"ds_map_add",second_entry,3);

  GmlVal first_key[]={map,vreal(1)};
  GmlVal second_key[]={map,vreal(2)};
  ok&=expect_real("fast ds_map_find_value",
                  call_fast(vm,"ds_map_find_value",first_key,2,&ok),11);
  ok&=expect_real("fast ds_map_find_first",
                  call_fast(vm,"ds_map_find_first",&map,1,&ok),1);
  ok&=expect_real("fast ds_map_find_next",
                  call_fast(vm,"ds_map_find_next",first_key,2,&ok),2);
  ok&=expect_real("fast ds_map_find_previous",
                  call_fast(vm,"ds_map_find_previous",second_key,2,&ok),1);
  ok&=expect_real("fast ds_map_exists",
                  call_fast(vm,"ds_map_exists",first_key,2,&ok),1);
  ok&=expect_real("fast ds_map_size",
                  call_fast(vm,"ds_map_size",&map,1,&ok),2);
  ok&=expect_real("fast ds_map_empty",
                  call_fast(vm,"ds_map_empty",&map,1,&ok),0);
  ok&=expect_real("fast ds_map_find_last",
                  call_fast(vm,"ds_map_find_last",&map,1,&ok),2);

  GmlVal list=gml_builtin_call(vm,"ds_list_create",NULL,0);
  GmlVal additions[]={list,vreal(7),vreal(8),vreal(9)};
  (void)gml_builtin_call(vm,"ds_list_add",additions,4);
  GmlVal at_one[]={list,vreal(1)};
  ok&=expect_real("fast ds_list_find_value",
                  call_fast(vm,"ds_list_find_value",at_one,2,&ok),8);
  ok&=expect_real("fast ds_list_size",
                  call_fast(vm,"ds_list_size",&list,1,&ok),3);
  (void)gml_builtin_call(vm,"ds_list_clear",&list,1);
  ok&=expect_real("fast ds_list_clear",
                  call_fast(vm,"ds_list_size",&list,1,&ok),0);

  (void)gml_builtin_call(vm,"ds_map_destroy",&map,1);
  (void)gml_builtin_call(vm,"ds_list_destroy",&list,1);
  return ok;
}

static int layer_instance_move(GmlVM *vm){
  GmlVal rear_args[]={vreal(320),vstr("neutral_rear")};
  GmlVal front_args[]={vreal(-40),vstr("neutral_front")};
  GmlVal rear_id=gml_builtin_call(vm,"layer_create",rear_args,2);
  GmlVal front_id=gml_builtin_call(vm,"layer_create",front_args,2);
  if(rear_id.t!=V_REAL || front_id.t!=V_REAL) return 0;

  GmlInstance *instances=realloc(vm->inst,sizeof(*instances));
  if(!instances) return 0;
  vm->inst=instances;
  vm->inst_count=vm->inst_cap=1;
  memset(&vm->inst[0],0,sizeof(vm->inst[0]));
  vm->inst[0].active=1;
  vm->inst[0].id=100001;
  vm->inst[0].depth=12;
  vm->inst[0].draw_layer_order=-1;
  vm->inst[0].draw_layer_element_order=4;

  GmlRtLayer *rear=gml_rt_layer_find(vm,(int)rear_id.d);
  GmlRtLayer *front=gml_rt_layer_find(vm,(int)front_id.d);
  GmlVal numeric_move[]={rear_id,vreal(vm->inst[0].id)};
  GmlVal named_move[]={vstr("neutral_front"),vreal(vm->inst[0].id)};
  int ok=rear && front &&
         gml_builtin_fast_id(vm,"layer_add_instance")>=0 &&
         expect_real("numeric layer move",
                     gml_builtin_call(vm,"layer_add_instance",numeric_move,2),0) &&
         vm->inst[0].depth==rear->depth &&
         vm->inst[0].draw_layer_order==rear->order &&
         vm->inst[0].draw_layer_element_order==-1 &&
         expect_real("named layer move",
                     gml_builtin_call(vm,"layer_add_instance",named_move,2),0) &&
         vm->inst[0].depth==front->depth &&
         vm->inst[0].draw_layer_order==front->order;
  if(!ok) fprintf(stderr,"instance did not move to the requested runtime layer\n");
  return ok;
}

static int canonical_registry_resolution(GmlVM *vm){
  int ok=1;
  int checked=0;
#define EXPECTED_ALWAYS(id) BID_##id
#define EXPECTED_DS_LOG_DISABLED(id) BID_##id
#define EXPECTED_NEVER(id) (-1)
#define CHECK_EXACT(value,id,name,owner,stage,cache) do { \
    int actual=gml_builtin_fast_id(vm,name); \
    int expected=EXPECTED_##cache(id); \
    checked++; \
    if(actual!=expected || BID_##id!=(value)){ \
      fprintf(stderr, \
              "canonical registry %s: expected id=%d value=%d, got %d\n", \
              name,expected,(value),actual); \
      ok=0; \
    } \
  } while(0);
#define CHECK_ALIAS(id,name,owner,stage,cache) do { \
    int actual=gml_builtin_fast_id(vm,name); \
    int expected=EXPECTED_##cache(id); \
    checked++; \
    if(actual!=expected){ \
      fprintf(stderr,"canonical registry alias %s: expected %d, got %d\n", \
              name,expected,actual); \
      ok=0; \
    } \
  } while(0);
  GML_BUILTIN_EXACT_REGISTRY(CHECK_EXACT,CHECK_ALIAS)
#undef CHECK_ALIAS
#undef CHECK_EXACT
#undef EXPECTED_NEVER
#undef EXPECTED_DS_LOG_DISABLED
#undef EXPECTED_ALWAYS
  if(gml_builtin_fast_id(vm,"fmod_registry_probe")!=BID_FMOD_PREFIX ||
     gml_builtin_fast_id(vm,"gamepad_registry_probe")!=BID_INPUT_KBGP ||
     gml_builtin_fast_id(vm,"neutral_registry_probe")!=-1){
    fprintf(stderr,"canonical registry dynamic slow-path ordering failed\n");
    ok=0;
  }
  if(checked<1500){
    fprintf(stderr,"canonical registry coverage is unexpectedly small: %d\n",
            checked);
    ok=0;
  }
  return ok;
}

int main(void){
  GmlWin win={0};
  GmlVM vm={0};
  if(!setup_fixture(&win,&vm)){
    fprintf(stderr,"builtin dispatch fixture setup failed\n");
    gml_vm_free(&vm);
    gml_win_free(&win);
    return 1;
  }
  int ok=canonical_registry_resolution(&vm) &&
         exact_builtin_precedes_same_named_script(&vm) &&
         script_resolution_order(&vm) &&
         function_value_and_alias_resolution(&vm) &&
         gain_conversion(&vm) &&
         room_dimension_mutation(&vm) &&
         mouse_none_semantics(&vm) &&
         ds_fast_interface(&vm) &&
         layer_instance_move(&vm);
  gml_vm_free(&vm);
  gml_win_free(&win);
  if(!ok) return 1;
  puts("builtin dispatch precedence fixtures: ok");
  return 0;
}
