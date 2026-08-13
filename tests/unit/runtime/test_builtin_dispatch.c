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

typedef struct {
  char message[256];
  int count;
  AnygmLogLevel level;
} LogFixture;

static int gamepad_fixture_connected(void *userdata,int device){
  (void)userdata;
  return device==0;
}

static int gamepad_fixture_button(void *userdata,int button,int edge){
  (void)userdata;
  return button==32771 && edge==0;
}

static double gamepad_fixture_axis(void *userdata,int device,int axis){
  (void)userdata;
  return device==0 && axis==32785?0.625:0.0;
}

static void log_fixture_write(void *userdata,AnygmLogLevel level,
                              const char *message){
  LogFixture *fixture=(LogFixture *)userdata;
  if(!fixture || (level!=ANYGM_LOG_DEBUG && level!=ANYGM_LOG_WARN &&
                  level!=ANYGM_LOG_ERROR)) return;
  snprintf(fixture->message,sizeof fixture->message,"%s",message?message:"");
  fixture->count++;
  fixture->level=level;
}

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

static int expect_string(const char *case_name,GmlVal actual,const char *expected){
  if(actual.t==V_STR && actual.s && !strcmp(actual.s,expected)) return 1;
  fprintf(stderr,"%s: expected string \"%s\", got type=%d value=\"%s\"\n",
          case_name,expected,actual.t,actual.s?actual.s:"");
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

static int setup_fixture(GmlWin *win,GmlVM *vm,AnygmHostServices *host,
                         LogFixture *log_fixture){
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
  win->size=384;
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
  win->n_chunks=2;
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
  memcpy(win->chunks[1].name,"SCPT",5);
  win->chunks[1].off=256;
  win->chunks[1].size=96;
  store_u32le(win->data+256,2);
  store_u32le(win->data+260,268);
  store_u32le(win->data+264,276);
  store_u32le(win->data+268,300);
  store_u32le(win->data+272,1);
  store_u32le(win->data+276,324);
  store_u32le(win->data+280,UINT32_MAX);
  memcpy(win->data+300,"neutral_asset_alias",20);
  memcpy(win->data+324,"neutral_declared_no_body",25);
  win->strs=calloc(3,sizeof(*win->strs));
  win->str_charoff=calloc(3,sizeof(*win->str_charoff));
  if(!win->strs || !win->str_charoff) return 0;
  win->strs[0]=(char *)win->data+160;
  win->str_charoff[0]=160;
  win->strs[1]=(char *)win->data+300;
  win->str_charoff[1]=300;
  win->strs[2]=(char *)win->data+324;
  win->str_charoff[2]=324;
  win->n_strs=3;
  memset(host,0,sizeof *host);
  host->struct_size=sizeof *host;
  host->userdata=log_fixture;
  host->log=log_fixture_write;
  return gml_vm_init(vm,win,host)==0;
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

static int script_resolution_order(GmlVM *vm,LogFixture *log_fixture){
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
  ok&=expect_real("script asset mapped to differently named code",
                  gml_builtin_call(vm,"neutral_asset_alias",NULL,0),74);
  int logs_before=log_fixture->count;
  ok&=expect_real("declared script without code body",
                  gml_builtin_call(vm,"neutral_declared_no_body",NULL,0),0);
  if(log_fixture->count!=logs_before){
    fprintf(stderr,"declared bodyless script was logged as unknown: %s\n",
            log_fixture->message);
    ok=0;
  }
  ok&=expect_real("deliberate unknown fallback",
                  gml_builtin_call(vm,"neutral_unknown_dispatch",NULL,0),0);
  if(log_fixture->count!=logs_before+1 ||
     !strstr(log_fixture->message,"unknown builtin: neutral_unknown_dispatch")){
    fprintf(stderr,"true unknown builtin diagnostic mismatch: %s\n",
            log_fixture->message);
    ok=0;
  }
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

static int hsv_color_byte_wrapping(GmlVM *vm){
  static const struct {
    double hue;
    double expected;
  } cases[]={
    {540,2855624},
    {1080,2869398},
    {2520,12266440},
    {3594,2838728},
    {-40,12266440},
  };
  int ok=1;
  for(size_t index=0;index<sizeof(cases)/sizeof(cases[0]);index++){
    GmlVal arguments[]={vreal(cases[index].hue),vreal(200),vreal(200)};
    char case_name[96];
    snprintf(case_name,sizeof case_name,"HSV byte wrapping at %.0f",cases[index].hue);
    ok&=expect_real(case_name,
                    gml_builtin_call(vm,"make_color_hsv",arguments,3),
                    cases[index].expected);
  }
  GmlVal alias_arguments[]={vreal(2520),vreal(200),vreal(200)};
  return ok && expect_real("HSV byte wrapping alternate alias",
                           gml_builtin_call(vm,"make_colour_hsv",alias_arguments,3),
                           12266440);
}

static int show_error_contract(GmlVM *vm,LogFixture *log_fixture){
  int logs_before=log_fixture->count;
  GmlVal warning_args[]={vstr("neutral warning"),vreal(0)};
  GmlVal fatal_args[]={vstr("neutral fatal"),vreal(1)};
  int ok=expect_real("nonfatal show_error",
                     gml_builtin_call(vm,"show_error",warning_args,2),0) &&
         vm->game_end==0 && log_fixture->count==logs_before+1 &&
         log_fixture->level==ANYGM_LOG_WARN &&
         strstr(log_fixture->message,"[gml error] neutral warning");
  ok&=expect_real("fatal show_error",
                  gml_builtin_call(vm,"show_error",fatal_args,2),0) &&
      vm->game_end==1 && log_fixture->count==logs_before+2 &&
      log_fixture->level==ANYGM_LOG_ERROR &&
      strstr(log_fixture->message,"[gml error] neutral fatal");
  vm->game_end=0;
  if(!ok) fprintf(stderr,"show_error contract failed: %s\n",log_fixture->message);
  return ok;
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

static int action_variable_comparisons(GmlVM *vm){
  struct {
    double variable;
    double comparison;
    int operation;
    double expected;
    const char *name;
  } cases[]={
    {2,2,0,1,"equal"},
    {2,3,0,0,"equal mismatch"},
    {2,3,1,1,"below"},
    {3,2,2,1,"above"},
    {2,2,3,1,"at most inclusive"},
    {1.04,1,4,1,"at least inclusive"},
    {2,3,5,1,"unequal"},
    {2,2,5,0,"unequal mismatch"},
    {2,2,99,0,"unknown selector"},
  };
  int ok=1;
  for(size_t index=0;index<sizeof(cases)/sizeof(cases[0]);index++){
    GmlVal arguments[]={
      vreal(cases[index].variable),
      vreal(cases[index].comparison),
      vreal(cases[index].operation),
    };
    ok&=expect_real(cases[index].name,
                    gml_builtin_call(vm,"action_if_variable",arguments,3),
                    cases[index].expected);
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

static int classic_dynamic_global_declaration(GmlVM *vm){
  int prior_classic_version=vm->win->classic_version;
  const struct AnygmCompatibilityProfile *prior_profile=vm->win->compatibility;
  vm->win->classic_version=800;
  vm->win->compatibility=NULL;
  GmlVal declaration=vstr(" globalvar neutral_first, neutral_second; ");
  GmlVal malformed=vstr("globalvar neutral_bad, ;");
  (void)gml_builtin_call(vm,"execute_string",&declaration,1);
  (void)gml_builtin_call(vm,"execute_string",&malformed,1);
  GmlVal first_name=vstr("neutral_first");
  GmlVal second_name=vstr("neutral_second");
  GmlVal bad_name=vstr("neutral_bad");
  GmlVal first_exists=gml_builtin_call(
      vm,"variable_global_exists",&first_name,1);
  GmlVal second_exists=gml_builtin_call(
      vm,"variable_global_exists",&second_name,1);
  GmlVal bad_exists=gml_builtin_call(
      vm,"variable_global_exists",&bad_name,1);
  GmlVal assignment[]={second_name,vreal(42)};
  (void)gml_builtin_call(vm,"variable_global_set",assignment,2);
  GmlVal second=gml_builtin_call(vm,"variable_global_get",&second_name,1);
  vm->win->compatibility=prior_profile;
  vm->win->classic_version=prior_classic_version;
  return expect_real("dynamic first global declaration",first_exists,1) &&
         expect_real("dynamic second global declaration",second_exists,1) &&
         expect_real("transactional malformed global declaration",bad_exists,0) &&
         expect_real("dynamic global storage",second,42);
}

static int external_audio_definition_dispatch(GmlVM *vm){
  GmlVal definition[]={
    vstr("SGAudio.dll"),vstr("sga_Init"),vreal(0),vreal(0),vreal(0)
  };
  GmlVal unknown_definition[]={
    vstr("neutral.dll"),vstr("sga_Init"),vreal(0),vreal(0),vreal(0)
  };
  GmlVal bgm_definition[]={
    vstr("bgm.dll"),vstr("bgm_init"),vreal(0),vreal(0),vreal(0)
  };
  GmlVal fmod_definition[]={
    vstr("GMFMODSimple.dll"),vstr("FMODinit"),vreal(0),vreal(0),vreal(0)
  };
  GmlVal pxtone_definition[]={
    vstr("pxwrap.dll"),vstr("pxtone_init"),vreal(0),vreal(0),vreal(0)
  };
  GmlVal input_definition[]={
    vstr("GMXInput.dll"),vstr("getCtrlState"),vreal(0),vreal(0),vreal(0)
  };
  GmlVal steam_definition[]={
    vstr("gmSteam.dll"),vstr("SteamSetAchievement"),vreal(0),vreal(0),vreal(0)
  };
  GmlVal steam_string_definition[]={
    vstr("Steamworks.dll"),vstr("SteamGetPersonaName"),vreal(0),vreal(0),vreal(0)
  };
  GmlVal handle=gml_builtin_call(vm,"external_define",definition,5);
  GmlVal bgm_handle=gml_builtin_call(vm,"external_define",bgm_definition,5);
  GmlVal fmod_handle=gml_builtin_call(vm,"external_define",fmod_definition,5);
  GmlVal pxtone_handle=gml_builtin_call(vm,"external_define",pxtone_definition,5);
  GmlVal input_handle=gml_builtin_call(vm,"external_define",input_definition,5);
  GmlVal steam_handle=gml_builtin_call(vm,"external_define",steam_definition,5);
  GmlVal steam_string_handle=gml_builtin_call(
      vm,"external_define",steam_string_definition,5);
  GmlVal unknown=gml_builtin_call(
      vm,"external_define",unknown_definition,5);
  GmlVal call_args[]={handle};
  GmlVal initialized=gml_builtin_call(vm,"external_call",call_args,1);
  GmlVal steam_args[]={steam_handle};
  GmlVal steam_result=gml_builtin_call(vm,"external_call",steam_args,1);
  GmlVal steam_string_args[]={steam_string_handle};
  GmlVal steam_string_result=gml_builtin_call(
      vm,"external_call",steam_string_args,1);
  GmlVal encoded_initialized=gml_builtin_call(vm,
      "__anygm_external_5347417564696f2e646c6c_7367615f496e6974",NULL,0);
  GmlVal malformed_encoded=gml_builtin_call(vm,
      "__anygm_external_not_hex_7367615f496e6974",NULL,0);
  static const char encoded_prefix[]="__anygm_external_";
  static const char encoded_symbol[]="7367615f496e6974";
  size_t oversized_size=sizeof(encoded_prefix)-1u+8194u+1u+
    sizeof(encoded_symbol)-1u;
  char *oversized_name=(char*)malloc(oversized_size+1u);
  GmlVal oversized_encoded=vreal(-1);
  if(oversized_name){
    size_t at=0;
    memcpy(oversized_name+at,encoded_prefix,sizeof(encoded_prefix)-1u);
    at+=sizeof(encoded_prefix)-1u;
    memset(oversized_name+at,'4',8194u); at+=8194u;
    oversized_name[at++]='_';
    memcpy(oversized_name+at,encoded_symbol,sizeof(encoded_symbol)-1u);
    at+=sizeof(encoded_symbol)-1u;
    oversized_name[at]='\0';
    oversized_encoded=gml_builtin_call(vm,oversized_name,NULL,0);
  }
  GmlVal library=vstr("SGAudio.dll");
  GmlVal freed=gml_builtin_call(vm,"external_free",&library,1);
  int ok=oversized_name && handle.t==V_REAL && handle.d>0.0 &&
         bgm_handle.t==V_REAL && bgm_handle.d>0.0 &&
         fmod_handle.t==V_REAL && fmod_handle.d>0.0 &&
         pxtone_handle.t==V_REAL && pxtone_handle.d>0.0 &&
         input_handle.t==V_REAL && input_handle.d>0.0 &&
         steam_handle.t==V_REAL && steam_handle.d>0.0 &&
         steam_string_handle.t==V_REAL && steam_string_handle.d>0.0 &&
         expect_real("unknown external library",unknown,0) &&
         expect_real("portable external audio init",initialized,1) &&
         expect_real("offline Steam real no-op",steam_result,0) &&
         expect_string("offline Steam string no-op",steam_string_result,"") &&
         expect_real("encoded portable external audio init",encoded_initialized,1) &&
         expect_real("malformed encoded external call",malformed_encoded,0) &&
         expect_real("oversized encoded external call",oversized_encoded,0) &&
         expect_real("external library release",freed,0);
  free(oversized_name);
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

/* Resolve a numeric collision suffix through a parent declaration when
 * no CODE name exists for that inherited handler. */
static int inherited_collision_resolves_numeric_suffix(GmlVM *vm){
  /* The fixture content declares no objects, so the case supplies its own three-entry table:
   * a parent carrying a native collision declaration against the target, and a child whose
   * lookup must reach it through the numeric suffix the collision dispatcher spells. */
  GmlObject *prior_objects=vm->objects;
  int prior_n_objects=vm->n_objects;
  static GmlObject table[3];
  static int declared[3];
  memset(table,0,sizeof table);
  table[0].name=(char*)"obj_test_parent";
  table[1].name=(char*)"obj_test_child";
  table[2].name=(char*)"obj_test_target";
  table[0].parent=-1; table[1].parent=0; table[2].parent=-1;
  declared[0]=4; declared[1]=2; declared[2]=0;   /* evtype=collision, subtype=target, code entry 0 */
  table[0].events=(typeof(table[0].events))(void*)declared;
  table[0].n_events=1;
  vm->objects=table;
  vm->n_objects=3;
  int handler=-1, code=-1;
  int found=gml_vm_instances_event_lookup(vm,"Collision_2",1,&handler,&code);
  int ok=found && handler==0 && code==0;
  vm->objects=prior_objects;
  vm->n_objects=prior_n_objects;
  if(!ok) fprintf(stderr,
    "numeric collision suffix did not reach the parent's declared handler (found=%d handler=%d code=%d)\n",
    found,handler,code);
  return ok;
}

/* A plain layer has no assigned FX. A visibility walk must not hide it. */
static int layer_fx_answers_none(GmlVM *vm){
  GmlVal create_args[]={vreal(64),vstr("neutral_fx_probe")};
  GmlVal layer_id=gml_builtin_call(vm,"layer_create",create_args,2);
  if(layer_id.t!=V_REAL || layer_id.d<0.0) return 0;
  GmlVal by_id=gml_builtin_call(vm,"layer_get_fx",&layer_id,1);
  GmlVal name_arg=vstr("neutral_fx_probe");
  GmlVal by_name=gml_builtin_call(vm,"layer_get_fx",&name_arg,1);
  GmlVal enable_args[]={layer_id,vreal(0)};
  GmlVal enabled=gml_builtin_call(vm,"layer_enable_fx",enable_args,2);
  int ok=expect_real("fx of a plain layer by id",by_id,-1) &&
         expect_real("fx of a plain layer by name",by_name,-1) &&
         expect_real("fx enable is an accepted no-op",enabled,0);
  if(ok && by_id.t==V_REAL && by_id.d!=-1.0) ok=0;
  if(ok){
    /* Hide only layers with an assigned effect. */
    if(by_id.d!=-1.0){
      GmlVal hide_args[]={layer_id,vreal(0)};
      gml_builtin_call(vm,"layer_set_visible",hide_args,2);
    }
    GmlVal visible=gml_builtin_call(vm,"layer_get_visible",&layer_id,1);
    ok=expect_real("a plain layer survives the effect-stripping walk",visible,1);
  }
  GmlVal destroy_ok=gml_builtin_call(vm,"layer_destroy",&layer_id,1);
  (void)destroy_ok;
  if(!ok) fprintf(stderr,"layer fx contract failed\n");
  return ok;
}

static int portable_service_contracts(GmlVM *vm){
  GmlVal player=gml_builtin_call(vm,"FOCAL_NetworkGetPlayer",NULL,0);
  GmlVal opponent=gml_builtin_call(vm,"FOCAL_NetworkGetOpponentName",NULL,0);
  GmlVal dropped=gml_builtin_call(vm,"file_drop_get_files",NULL,0);
  return expect_real("offline network player",player,-1) &&
         expect_string("offline network opponent",opponent,"") &&
         expect_string("unavailable file drop",dropped,"") &&
         expect_real("unavailable process launch",
                     gml_builtin_call(vm,"execute_shell_simple",NULL,0),0) &&
         expect_real("unavailable clipboard bridge",
                     gml_builtin_call(vm,"drago_clipboard_set_text",NULL,0),0) &&
         expect_real("unavailable presence bridge",
                     gml_builtin_call(vm,"Discord_UpdatePresence",NULL,0),0) &&
         expect_real("unavailable borderless window",
                     gml_builtin_call(vm,"BorderlessToggle",NULL,0),0);
}

static int portable_joystick_contract(GmlVM *vm){
  vm->input.gamepad_connected=gamepad_fixture_connected;
  vm->input.gamepad=gamepad_fixture_button;
  vm->input.gamepad_axis=gamepad_fixture_axis;
  GmlVal device=vreal(0);
  GmlVal button_args[]={device,vreal(2)};
  GmlVal axis_args[]={device,vreal(0)};
  int ok=expect_real("portable joystick count",
                     gml_builtin_call(vm,"joy_count",NULL,0),1) &&
         expect_string("portable joystick name",
                       gml_builtin_call(vm,"joy_name",&device,1),"AnyGM Gamepad") &&
         expect_real("portable joystick button",
                     gml_builtin_call(vm,"joy_button",button_args,2),1) &&
         expect_real("portable joystick axis",
                     gml_builtin_call(vm,"joy_axis",axis_args,2),0.625);
  vm->input.gamepad_connected=NULL;
  vm->input.gamepad=NULL;
  vm->input.gamepad_axis=NULL;
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
  AnygmHostServices host={0};
  LogFixture log_fixture={0};
  if(!setup_fixture(&win,&vm,&host,&log_fixture)){
    fprintf(stderr,"builtin dispatch fixture setup failed\n");
    gml_vm_free(&vm);
    gml_win_free(&win);
    return 1;
  }
  int ok=canonical_registry_resolution(&vm) &&
         exact_builtin_precedes_same_named_script(&vm) &&
         script_resolution_order(&vm,&log_fixture) &&
         show_error_contract(&vm,&log_fixture) &&
         function_value_and_alias_resolution(&vm) &&
         hsv_color_byte_wrapping(&vm) &&
         gain_conversion(&vm) &&
         action_variable_comparisons(&vm) &&
         room_dimension_mutation(&vm) &&
         mouse_none_semantics(&vm) &&
         ds_fast_interface(&vm) &&
         classic_dynamic_global_declaration(&vm) &&
         external_audio_definition_dispatch(&vm) &&
         portable_service_contracts(&vm) &&
         portable_joystick_contract(&vm) &&
         layer_instance_move(&vm) &&
         layer_fx_answers_none(&vm) &&
         inherited_collision_resolves_numeric_suffix(&vm);
  gml_vm_free(&vm);
  gml_win_free(&win);
  if(!ok) return 1;
  puts("builtin dispatch precedence fixtures: ok");
  return 0;
}
