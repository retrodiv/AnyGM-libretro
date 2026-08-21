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

static int gamepad_fixture_button(void *userdata,int device,int button,int edge){
  (void)userdata;
  return device==0 && button==32771 && edge==0;
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

/* Synthetic string operands distinguish equal, unequal and ordered variable-action comparisons, including unknown selectors. */
static int action_variable_string_comparisons(GmlVM *vm){
  struct {
    GmlVal variable;
    GmlVal comparison;
    int operation;
    double expected;
    const char *name;
  } cases[]={
    {vstr("neutral alpha"),vstr("neutral alpha"),0,1,"words equal"},
    {vstr("neutral alpha"),vstr("neutral beta"),0,0,"different words are not equal"},
    {vstr("neutral alpha"),vstr("neutral beta"),5,1,"different words are unequal"},
    {vstr("neutral alpha"),vstr("neutral beta"),1,1,"words order below"},
    {vstr("neutral beta"),vstr("neutral alpha"),2,1,"words order above"},
    {vstr("neutral alpha"),vstr("neutral alpha"),3,1,"words at most inclusive"},
    {vstr("neutral alpha"),vstr("neutral alpha"),4,1,"words at least inclusive"},
    {vstr("neutral alpha"),vstr("neutral beta"),99,0,"words unknown selector"},
  };
  int ok=1;
  for(size_t index=0;index<sizeof(cases)/sizeof(cases[0]);index++){
    GmlVal arguments[]={
      cases[index].variable,
      cases[index].comparison,
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

/* A missing stream must not alias sound index zero, and audio_exists must reject it. */
static int missing_stream_is_not_sound_zero(GmlVM *vm){
  GmlVal path=vstr("no/such/announcer/line.ogg");
  GmlVal handle=gml_builtin_call(vm,"audio_create_stream",&path,1);
  int ok=handle.t==V_REAL && handle.d<0.0;
  if(!ok) fprintf(stderr,"a stream that cannot be created answered %g\n",
                  handle.t==V_REAL?handle.d:-999.0);
  if(ok){
    GmlVal exists=gml_builtin_call(vm,"audio_exists",&handle,1);
    ok=exists.t==V_REAL && exists.d==0.0;
    if(!ok) fprintf(stderr,"audio_exists accepted a stream that was never created\n");
  }
  if(ok){
    GmlVal destroyed=gml_builtin_call(vm,"audio_destroy_stream",&handle,1);
    ok=destroyed.t==V_REAL;
    if(!ok) fprintf(stderr,"destroying an uncreated stream did not answer\n");
  }
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

/* Both vertical axes carry the screen convention here: a positive value means down, which is
 * what the gp_axis* constants promise and what the raw device index must invert. */
static double gamepad_axis_orientation_fixture(void *userdata,int device,int axis){
  (void)userdata;
  if(device!=0) return 0.0;
  if(axis==32785) return 0.5;   /* gp_axislh: right */
  if(axis==32786) return 0.75;  /* gp_axislv: down  */
  if(axis==32787) return 0.25;  /* gp_axisrh: right */
  if(axis==32788) return 0.5;   /* gp_axisrv: down  */
  return 0.0;
}

static int gamepad_axis_orientation_contract(GmlVM *vm){
  double (*saved_axis)(void*,int,int)=vm->input.gamepad_axis;
  int (*saved_connected)(void*,int)=vm->input.gamepad_connected;
  vm->input.gamepad_axis=gamepad_axis_orientation_fixture;
  vm->input.gamepad_connected=gamepad_fixture_connected;
  GmlVal device=vreal(0);
  struct { const char *label; double axis; double expected; } cases[]={
    /* The constants keep GameMaker's screen convention. */
    {"gp_axislh reads right positive",32785,0.5},
    {"gp_axislv reads down positive",32786,0.75},
    {"gp_axisrh reads right positive",32787,0.25},
    {"gp_axisrv reads down positive",32788,0.5},
    /* A raw device index reads the device: XInput's thumbs grow upwards, so the vertical axes
     * arrive negated while the horizontal ones agree with the screen. */
    {"raw left horizontal keeps its sign",0,0.5},
    {"raw left vertical carries the device sign",1,-0.75},
    {"raw right horizontal keeps its sign",2,0.25},
    {"raw right vertical carries the device sign",3,-0.5},
  };
  int ok=1;
  for(size_t i=0;i<sizeof cases/sizeof cases[0];i++){
    GmlVal args[]={device,vreal(cases[i].axis)};
    ok=expect_real(cases[i].label,gml_builtin_call(vm,"gamepad_axis_value",args,2),
                   cases[i].expected) && ok;
  }
  vm->input.gamepad_axis=saved_axis;
  vm->input.gamepad_connected=saved_connected;
  return ok;
}

/* Externally bound joystick symbols and named builtins must read the same host pad. The hat
 * surface uses clockwise compass degrees and -1 for centred rather than an SDL bitmask. */
static int joydll_pad_button(void *userdata,int device,int button,int edge){
  (void)userdata;
  if(edge!=0 || device!=0) return 0;
  return button==32784 || button==32769;   /* gp_padr and gp_face1 held */
}

/* Two pads, holding different things, so an answer that came from the wrong one is visible rather
 * than merely unproven: player one holds right and the bottom face button, player two holds left
 * and the top one. A runtime that ignores the device answers player one for both. */
static int two_pads_connected(void *userdata,int device){
  (void)userdata;
  return device==0 || device==1;
}

static int two_pads_button(void *userdata,int device,int button,int edge){
  (void)userdata;
  if(edge!=0) return 0;
  if(device==0) return button==32784 || button==32769;   /* right, face1 */
  if(device==1) return button==32783 || button==32772;   /* left,  face4 */
  return 0;
}

static double two_pads_axis(void *userdata,int device,int axis){
  (void)userdata;
  if(device==1 && axis==32785) return -0.5;
  return 0.0;
}

static int a_second_pad_is_its_own_device(GmlVM *vm){
  int (*saved_button)(void*,int,int,int)=vm->input.gamepad;
  int (*saved_connected)(void*,int)=vm->input.gamepad_connected;
  double (*saved_axis)(void*,int,int)=vm->input.gamepad_axis;
  vm->input.gamepad=two_pads_button;
  vm->input.gamepad_connected=two_pads_connected;
  vm->input.gamepad_axis=two_pads_axis;
  GmlVal p1_right[]={vreal(0),vreal(32784)};
  GmlVal p2_right[]={vreal(1),vreal(32784)};
  GmlVal p2_left[] ={vreal(1),vreal(32783)};
  GmlVal p1_face1[]={vreal(0),vreal(32769)};
  GmlVal p2_face4[]={vreal(1),vreal(32772)};
  GmlVal joy_p1[]={vreal(0),vreal(0)};
  GmlVal joy_p2[]={vreal(1),vreal(0)};
  GmlVal stick_p2[]={vreal(1),vreal(32785)};
  GmlVal one=vreal(1), two=vreal(2);
  int ok=expect_real("gamepad_button_check reads player one's own pad",
                     gml_builtin_call(vm,"gamepad_button_check",p1_right,2),1) &&
         expect_real("player two is not holding what player one holds",
                     gml_builtin_call(vm,"gamepad_button_check",p2_right,2),0) &&
         expect_real("player two's own direction reaches player two",
                     gml_builtin_call(vm,"gamepad_button_check",p2_left,2),1) &&
         expect_real("player one's face button stays player one's",
                     gml_builtin_call(vm,"gamepad_button_check",p1_face1,2),1) &&
         expect_real("player two's face button reaches player two",
                     gml_builtin_call(vm,"gamepad_button_check",p2_face4,2),1) &&
         expect_real("the extension hat answers each pad separately",
                     gml_builtin_call(vm,"joy_hat",joy_p1,2),90) &&
         expect_real("and answers the second pad its own direction",
                     gml_builtin_call(vm,"joy_hat",joy_p2,2),270) &&
         expect_real("gamepad_axis_value reads the second pad's stick",
                     gml_builtin_call(vm,"gamepad_axis_value",stick_p2,2),-0.5) &&
         /* The legacy joystick family numbers from one, so joystick 2 is device 1. */
         expect_real("joystick_xpos follows the one-based joystick number",
                     gml_builtin_call(vm,"joystick_xpos",&two,1),-1) &&
         expect_real("and joystick 1 still reads the first pad",
                     gml_builtin_call(vm,"joystick_xpos",&one,1),1);
  vm->input.gamepad=saved_button;
  vm->input.gamepad_connected=saved_connected;
  vm->input.gamepad_axis=saved_axis;
  return ok;
}

static int external_joydll_binding_reaches_the_pad(GmlVM *vm){
  int (*saved_button)(void*,int,int,int)=vm->input.gamepad;
  int (*saved_connected)(void*,int)=vm->input.gamepad_connected;
  vm->input.gamepad=joydll_pad_button;
  vm->input.gamepad_connected=gamepad_fixture_connected;
  GmlVal count_definition[]={
    vstr("joydll.dll"),vstr("joy_count"),vreal(0),vreal(0),vreal(0)
  };
  GmlVal hat_definition[]={
    vstr("joydll.dll"),vstr("joy_hat"),vreal(0),vreal(0),vreal(2)
  };
  GmlVal button_definition[]={
    vstr("joydll.dll"),vstr("joy_button"),vreal(0),vreal(0),vreal(2)
  };
  GmlVal count_handle=gml_builtin_call(vm,"external_define",count_definition,5);
  GmlVal hat_handle=gml_builtin_call(vm,"external_define",hat_definition,5);
  GmlVal button_handle=gml_builtin_call(vm,"external_define",button_definition,5);
  GmlVal count_call[]={count_handle};
  GmlVal hat_call[]={hat_handle,vreal(0),vreal(0)};
  GmlVal button_call[]={button_handle,vreal(0),vreal(0)};
  int ok=1;
  if(count_handle.d==0.0 || hat_handle.d==0.0 || button_handle.d==0.0){
    fprintf(stderr,"joydll symbols did not bind to the input surface\n");
    ok=0;
  }
  ok=expect_real("joydll joy_count sees the connected pad",
                 gml_builtin_call(vm,"external_call",count_call,1),1) && ok;
  ok=expect_real("joydll joy_button reads the face button",
                 gml_builtin_call(vm,"external_call",button_call,3),1) && ok;
  ok=expect_real("joydll joy_hat answers right as ninety degrees",
                 gml_builtin_call(vm,"external_call",hat_call,3),90) && ok;
  GmlVal named_hat[]={vreal(0),vreal(0)};
  ok=expect_real("the named joy_hat answers the same ninety degrees",
                 gml_builtin_call(vm,"joy_hat",named_hat,2),90) && ok;
  vm->input.gamepad=NULL;
  ok=expect_real("a resting hat is centred rather than up",
                 gml_builtin_call(vm,"joy_hat",named_hat,2),-1) && ok;
  vm->input.gamepad=saved_button;
  vm->input.gamepad_connected=saved_connected;
  return ok;
}

/* A host that maps an analog stick onto the d-pad reports the stick centred and the pad pressed;
 * a player on a real d-pad produces the same pair. The stick reading has to survive both, whichever
 * name the content asks under. The right stick has no digital counterpart and stays silent. */
static double gamepad_dpad_only_axis(void *userdata,int device,int axis){
  (void)userdata; (void)device; (void)axis;
  return 0.0;
}

static int gamepad_dpad_only_button(void *userdata,int device,int button,int edge){
  (void)userdata;
  if(edge!=0 || device!=0) return 0;
  return button==32784 || button==32782;   /* gp_padr and gp_padd held */
}

static int digital_pad_reaches_axis_readers(GmlVM *vm){
  double (*saved_axis)(void*,int,int)=vm->input.gamepad_axis;
  int (*saved_button)(void*,int,int,int)=vm->input.gamepad;
  int (*saved_connected)(void*,int)=vm->input.gamepad_connected;
  vm->input.gamepad_axis=gamepad_dpad_only_axis;
  vm->input.gamepad=gamepad_dpad_only_button;
  vm->input.gamepad_connected=gamepad_fixture_connected;
  GmlVal device=vreal(0);
  GmlVal left_h[]={device,vreal(32785)};
  GmlVal left_v[]={device,vreal(32786)};
  GmlVal right_h[]={device,vreal(32787)};
  GmlVal joy_h[]={device,vreal(0)};
  GmlVal joy_v[]={device,vreal(1)};
  GmlVal joy_r[]={device,vreal(2)};
  int ok=expect_real("gamepad_axis_value reads the pad horizontally",
                     gml_builtin_call(vm,"gamepad_axis_value",left_h,2),1.0) &&
         expect_real("gamepad_axis_value reads the pad vertically",
                     gml_builtin_call(vm,"gamepad_axis_value",left_v,2),1.0) &&
         expect_real("gamepad_axis_value leaves the right stick silent",
                     gml_builtin_call(vm,"gamepad_axis_value",right_h,2),0.0) &&
         expect_real("joy_axis reads the pad horizontally",
                     gml_builtin_call(vm,"joy_axis",joy_h,2),1.0) &&
         expect_real("joy_axis reads the pad vertically",
                     gml_builtin_call(vm,"joy_axis",joy_v,2),1.0) &&
         expect_real("joy_axis leaves the right stick silent",
                     gml_builtin_call(vm,"joy_axis",joy_r,2),0.0) &&
         expect_real("joystick_xpos reads the pad",
                     gml_builtin_call(vm,"joystick_xpos",&device,1),1.0);
  vm->input.gamepad_axis=saved_axis;
  vm->input.gamepad=saved_button;
  vm->input.gamepad_connected=saved_connected;
  return ok;
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
         /* The identifier describes the controller layout represented by the host mapping. */
         expect_string("portable joystick name",
                       gml_builtin_call(vm,"joy_name",&device,1),"Xbox 360 Controller") &&
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
         action_variable_string_comparisons(&vm) &&
         room_dimension_mutation(&vm) &&
         mouse_none_semantics(&vm) &&
         ds_fast_interface(&vm) &&
         classic_dynamic_global_declaration(&vm) &&
         external_audio_definition_dispatch(&vm) &&
         portable_service_contracts(&vm) &&
         portable_joystick_contract(&vm) &&
         external_joydll_binding_reaches_the_pad(&vm) &&
         a_second_pad_is_its_own_device(&vm) &&
         gamepad_axis_orientation_contract(&vm) &&
         digital_pad_reaches_axis_readers(&vm) &&
         layer_instance_move(&vm) &&
         layer_fx_answers_none(&vm) &&
         missing_stream_is_not_sound_zero(&vm) &&
         inherited_collision_resolves_numeric_suffix(&vm);
  gml_vm_free(&vm);
  gml_win_free(&win);
  if(!ok) return 1;
  puts("builtin dispatch precedence fixtures: ok");
  return 0;
}
