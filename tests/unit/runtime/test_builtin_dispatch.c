/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "gml_builtin.h"
#include "gml_builtin_registry.h"
#include "gml_vm.h"
#include "gml_vm_internal.h"
#include "gml_value_internal.h"
#include "gml_render_internal.h"

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
                         LogFixture *log_fixture,const char *first_script_name){
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
  win->code[0].name=first_script_name;
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

/* Reinitializing a payload in the same stack owner must not reuse the previous script answer. */
static int payload_shadow_cache_lifetime(GmlWin *win,GmlVM *vm,
                                         AnygmHostServices *host,LogFixture *log_fixture){
  GmlVal argument=vreal(-9);
  int ok=setup_fixture(win,vm,host,log_fixture,"gml_Script_other_name");
  if(ok){
    int fast=gml_builtin_fast_id(vm,"abs");
    GmlVal answer=gml_builtin_call(vm,"abs",&argument,1);
    ok=fast>=0 && expect_real("builtin after content replacement",answer,9);
  }
  gml_vm_free(vm);
  gml_win_free(win);
  if(!ok) return 0;
  ok=setup_fixture(win,vm,host,log_fixture,"gml_Script_abs");
  if(ok){
    int fast=gml_builtin_fast_id(vm,"abs");
    GmlVal answer=gml_builtin_call(vm,"abs",&argument,1);
    ok=fast==-1 && expect_real("payload script after content replacement",answer,73);
  }
  gml_vm_free(vm);
  gml_win_free(win);
  if(!ok) return 0;
  ok=setup_fixture(win,vm,host,log_fixture,"gml_Script_draw_set_colour");
  if(ok){
    GmlVal color=vreal(0);
    int canonical=gml_builtin_fast_id(vm,"draw_set_color");
    int alias=gml_builtin_fast_id(vm,"draw_set_colour");
    GmlVal answer=gml_builtin_call(vm,"draw_set_colour",&color,1);
    ok=canonical>=0 && alias==-1 &&
       expect_real("same-ID alias has its own script precedence",answer,73);
  }
  gml_vm_free(vm);
  gml_win_free(win);
  return ok;
}

static int embedded_function_binding(GmlWin *win,GmlVM *vm,
                                     AnygmHostServices *host,LogFixture *log_fixture){
  if(!setup_fixture(win,vm,host,log_fixture,"gml_Script_abs")) return 0;
  win->code[0].embedded_function=1;
  GmlVal argument=vreal(-9);
  GmlVal bare=gml_builtin_call(vm,"abs",&argument,1);
  GmlVal qualified=gml_builtin_call(vm,"gml_Script_abs",&argument,1);
  int ok=expect_real("bare name selects the builtin beside a modern embedded function",bare,9);
  ok=expect_real("qualified name selects the embedded function",qualified,73) && ok;
  if(gml_builtin_fast_id(vm,"abs")!=BID_ABS){
    fprintf(stderr,"bare name beside an embedded function must retain its builtin ID\n");
    ok=0;
  }
  gml_vm_free(vm);
  gml_win_free(win);
  return ok;
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

/* The fixture declares a payload script for two canonical names (see ScriptFixture), and the
 * reference runner resolves the payload's own script before a builtin of the same name, so the
 * registry must report no id at all for them. */
static int payload_shadows(const char *name){
  return !strcmp(name,"abs") || !strcmp(name,"draw_set_blend_mode");
}

static int payload_script_precedes_exact_builtin(GmlVM *vm){
  GmlVal argument=vreal(-4);
  int id=gml_builtin_fast_id(vm,"abs");
  GmlVal direct=gml_builtin_call(vm,"abs",&argument,1);
  GmlVal blend_argument=vreal(0);
  return id==-1 &&
         expect_real("payload script before the same-named exact builtin",direct,73) &&
         expect_real("characterized hot exact name before script",
                     gml_builtin_call(vm,"draw_set_blend_mode",&blend_argument,1),79);
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

static int texture_interpolation_ext_alias(GmlVM *vm,LogFixture *log_fixture){
  void *prior_render=vm->render;
  GmlRender render={0};
  vm->render=&render;
  int logs_before=log_fixture->count;
  int ok=1;
  const char *names[]={"texture_set_interpolation_ext","gpu_set_texfilter_ext",
                       "gpu_set_tex_filter_ext"};
  GmlVal enable[]={vreal(7),vreal(1)};
  GmlVal disable[]={vreal(7),vreal(0)};
  for(size_t i=0;i<sizeof names/sizeof names[0];i++){
    ok&=expect_real("texture interpolation ext enable return",
                    call_fast(vm,names[i],enable,2,&ok),0);
    ok&=render.interp==0;
    ok&=expect_real("auxiliary interpolation enabled",
                    gml_builtin_call(vm,"gpu_get_texfilter_ext",enable,1),1);
    ok&=expect_real("texture interpolation ext disable return",
                    gml_builtin_call(vm,names[i],disable,2),0);
    ok&=render.interp==0;
    ok&=expect_real("auxiliary interpolation disabled",
                    gml_builtin_call(vm,"gpu_get_texfilter_ext",disable,1),0);
  }
  ok&=log_fixture->count==logs_before;
  if(!ok) fprintf(stderr,"extended filtering escaped its sampler or lost alias dispatch\n");
  vm->render=prior_render;
  return ok;
}

static int internal_instance_receiver_conversion(GmlVM *vm,
                                                 LogFixture *log_fixture){
  int logs_before=log_fixture->count;
  int ok=1;
  GmlVal receiver=vreal(100000);
  GmlVal converted=call_fast(vm,"@@GetInstance@@",&receiver,1,&ok);
  if(!expect_real("internal instance receiver conversion",converted,100000) ||
     log_fixture->count!=logs_before){
    fprintf(stderr,"@@GetInstance@@ did not preserve the receiver reference\n");
    return 0;
  }
  return ok;
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

static int action_mouse_contract(void){
  GmlWin win={0};GmlVM vm={0};AnygmHostServices host={0};LogFixture log={0};
  MouseFixture input={0};int ok=setup_fixture(&win,&vm,&host,&log,"gml_Script_abs");
  if(!ok) goto done;
  vm.input.userdata=&input;vm.input.mouse=mouse_fixture_read;
  int id=gml_builtin_fast_id(&vm,"action_if_mouse");
  ok=id>=0;
  const int selectors[]={0,1,2,3,-1,4,5};
  vm.action_relative=1;
  for(int current=0;current<=5 && ok;current++){
    *gml_varmap_put(&vm.globals,"mouse_button")=vreal(current);
    for(input.held=0;input.held<32 && ok;input.held++)
      for(input.released=0;input.released<32 && ok;input.released++)
        for(unsigned k=0;k<sizeof selectors/sizeof selectors[0] && ok;k++){
          int button=selectors[k];GmlVal arg=vreal(button);
          int expected=button>=1 && button<=3?
            ((input.held|input.released)&(1<<(button-1)))!=0:current==0;
          for(int cached=0;cached<2 && ok;cached++){
            GmlVal out=cached?gml_builtin_call_fast_id(&vm,id,"action_if_mouse",&arg,1):
                              gml_builtin_call(&vm,"action_if_mouse",&arg,1);
            ok=expect_real("legacy mouse same-frame selector",out,expected) && vm.action_relative==1;
          }
        }
  }
  vm.input.mouse=NULL;
  *gml_varmap_put(&vm.globals,"mouse_button")=vreal(0);
  for(unsigned k=0;k<sizeof selectors/sizeof selectors[0] && ok;k++){
    GmlVal arg=vreal(selectors[k]);
    ok=expect_real("legacy mouse without a host callback",gml_builtin_call(&vm,"action_if_mouse",&arg,1),
                   selectors[k]>=1 && selectors[k]<=3?0:1);
  }
  const double invalid[]={NAN,INFINITY,-INFINITY,1e100,-1e100};
  for(unsigned k=0;k<sizeof invalid/sizeof invalid[0] && ok;k++){
    GmlVal arg=vreal(invalid[k]);
    ok=expect_real("legacy mouse rejects unsafe numeric conversion",
                   gml_builtin_call_fast_id(&vm,id,"action_if_mouse",&arg,1),0);
  }
  ok=ok && expect_real("legacy mouse missing argument",gml_builtin_call(&vm,"action_if_mouse",NULL,0),0);
  if(ok) puts("legacy mouse: named/cached held-release/selector matrix and unbound guards passed");
done:
  gml_vm_free(&vm);gml_win_free(&win);return ok;
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

static int scalar_shape_queries_accept_target_arrays(GmlVM *vm){
  GmlObject *prior_objects=vm->objects;
  int prior_n_objects=vm->n_objects;
  int *prior_obj_alive=vm->obj_alive;
  void *prior_render=vm->render;
  int prior_inst_count=vm->inst_count;
  GmlObject objects[2]={{0}};
  int obj_alive[2]={1,1};
  GmlRender render={0};
  GmlSprite sprite={0};
  GmlInstance *instances=realloc(vm->inst,
    (size_t)(prior_inst_count+1)*sizeof(*instances));
  if(!instances) return 0;
  vm->inst=instances;
  vm->inst_cap=prior_inst_count+1;
  objects[0].parent=objects[1].parent=-1;
  sprite.w=sprite.h=1;
  sprite.ml=sprite.mt=sprite.mr=sprite.mb=0;
  sprite.collision_kind=1;
  render.n_spr=1;
  render.spr=&sprite;
  vm->objects=objects;
  vm->n_objects=2;
  vm->obj_alive=obj_alive;
  vm->render=&render;
  GmlInstance *target=&vm->inst[prior_inst_count];
  memset(target,0,sizeof(*target));
  target->active=1;
  target->id=100002;
  target->obj=1;
  target->x=target->y=64;
  target->sprite_index=0;
  target->mask_index=-1;
  target->image_xscale=target->image_yscale=1;
  vm->inst_count=prior_inst_count+1;
  gml_colgrid_invalidate(vm);

  GmlVal scalar_args[7]={vreal(64),vreal(64),vreal(64),vreal(64),
    vreal(1),vreal(0),vreal(0)};
  GmlVal scalar_hit=gml_builtin_call(vm,"collision_rectangle",scalar_args,7);
  GmlVal targets=gml_arr_new(2,vreal(0));
  gml_arr_set(targets,0,vreal(0));
  gml_arr_set(targets,1,vreal(1));
  scalar_args[4]=targets;
  GmlVal array_hit=gml_builtin_call(vm,"collision_rectangle",scalar_args,7);
  GmlVal point_args[5]={vreal(64),vreal(64),targets,vreal(0),vreal(0)};
  GmlVal point_hit=gml_builtin_call(vm,"collision_point",point_args,5);
  GmlVal circle_args[6]={vreal(64),vreal(64),vreal(1),targets,vreal(0),vreal(0)};
  GmlVal circle_hit=gml_builtin_call(vm,"collision_circle",circle_args,6);
  int ok=expect_real("scalar collision object target",scalar_hit,100002) &&
         expect_real("rectangle collision array target",array_hit,100002) &&
         expect_real("point collision array target",point_hit,100002) &&
         expect_real("circle collision array target",circle_hit,100002);
  gml_values_release(&targets,1);

  vm->inst_count=prior_inst_count;
  vm->objects=prior_objects;
  vm->n_objects=prior_n_objects;
  vm->obj_alive=prior_obj_alive;
  vm->render=prior_render;
  gml_colgrid_invalidate(vm);
  return ok;
}

/* The generic draw_* fallback silently returns for unimplemented names. Assert that
 * draw_surface_general reaches a blit instead of that fallback. */
static int surface_general_reaches_the_screen(GmlVM *vm){
  enum { WIDTH=4, HEIGHT=4 };
  void *prior_render=vm->render;
  GmlRender render;
  uint32_t frame[WIDTH*HEIGHT];
  memset(&render,0,sizeof render);
  for(size_t index=0;index<WIDTH*HEIGHT;index++) frame[index]=0xFF000000u;
  render.fb=render.base_fb=frame;
  render.fbw=render.base_fbw=WIDTH;
  render.fbh=render.base_fbh=HEIGHT;
  render.target_id=-1;
  render.next_surface_id=1;
  render.alphablend=1;
  render.alpha=1.0;
  render.color=0xFFFFFF;
  render.color_write_mask=0x0F;
  render.blend_equation=1;
  render.blend_equation_alpha=1;
  render.app_draw_enable=1;
  render.active_shader=-1;
  render.lut_pal_sprite=-1;
  vm->render=&render;

  GmlVal make[]={vreal(2),vreal(2)};
  GmlVal surface=gml_builtin_call(vm,"surface_create",make,2);
  GmlVal target[]={surface};
  gml_builtin_call(vm,"surface_set_target",target,1);
  GmlVal red[]={vreal(0x0000FF)};   /* GM packs colour as BGR, so this is red. */
  gml_builtin_call(vm,"draw_clear",red,1);
  gml_builtin_call(vm,"surface_reset_target",NULL,0);

  GmlVal general[15]={surface,vreal(0),vreal(0),vreal(2),vreal(2),vreal(1),vreal(1),
                      vreal(1),vreal(1),vreal(0),
                      vreal(0xFFFFFF),vreal(0xFFFFFF),vreal(0xFFFFFF),vreal(0xFFFFFF),vreal(1)};
  gml_builtin_call(vm,"draw_surface_general",general,15);

  int ok=frame[(size_t)1*WIDTH+1]==0xFFFF0000u &&
         frame[(size_t)2*WIDTH+2]==0xFFFF0000u &&
         frame[0]==0xFF000000u &&
         frame[(size_t)3*WIDTH+3]==0xFF000000u;
  if(!ok) fprintf(stderr,
    "draw_surface_general did not blit its region (in=%08x,%08x out=%08x,%08x)\n",
    frame[(size_t)1*WIDTH+1],frame[(size_t)2*WIDTH+2],frame[0],frame[(size_t)3*WIDTH+3]);
  GmlVal free_args[]={surface};
  gml_builtin_call(vm,"surface_free",free_args,1);
  vm->render=prior_render;
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

static int gamepad_threshold_contract(void){
  int ok=1;
  for(int cached=0;cached<2;cached++){
    GmlWin win={.bytecode=17};GmlVM vm={.win=&win},other={.win=&win};
    int get=gml_builtin_fast_id(&vm,"gamepad_get_button_threshold");
    int set=gml_builtin_fast_id(&vm,"gamepad_set_button_threshold");
    ok=ok && get>=0 && set>=0;
    for(int device=0;device<16 && ok;device++){
      GmlVal args[]={vreal(device),vreal((device+1)/32.0)};
      GmlVal initial=cached?gml_builtin_call_fast_id(&vm,get,"gamepad_get_button_threshold",args,1):
        gml_builtin_call(&vm,"gamepad_get_button_threshold",args,1);
      ok=expect_real("threshold default",initial,0.5) && ok;
      if(cached)gml_builtin_call_fast_id(&vm,set,"gamepad_set_button_threshold",args,2);
      else gml_builtin_call(&vm,"gamepad_set_button_threshold",args,2);
      GmlVal changed=cached?gml_builtin_call_fast_id(&vm,get,"gamepad_get_button_threshold",args,1):
        gml_builtin_call(&vm,"gamepad_get_button_threshold",args,1);
      ok=expect_real("device threshold",changed,args[1].d) &&
        expect_real("VM threshold isolation",gml_builtin_call(&other,"gamepad_get_button_threshold",args,1),0.5) && ok;
    }
    const double values[]={-1,2,NAN,INFINITY,-INFINITY};
    const double expected[]={0,1,1,1,1};
    for(unsigned i=0;i<sizeof values/sizeof values[0] && ok;i++){
      GmlVal args[]={vreal(0),vreal(values[i])};
      gml_builtin_call(&vm,"gamepad_set_button_threshold",args,2);
      ok=expect_real("threshold finite bounds",gml_builtin_call(&vm,"gamepad_get_button_threshold",args,1),expected[i]);
    }
    const double invalid[]={-1,16,NAN,INFINITY};
    for(unsigned i=0;i<sizeof invalid/sizeof invalid[0] && ok;i++){
      GmlVal args[]={vreal(invalid[i]),vreal(0.25)};
      gml_builtin_call(&vm,"gamepad_set_button_threshold",args,2);
      ok=expect_real("invalid device threshold",gml_builtin_call(&vm,"gamepad_get_button_threshold",args,1),0.5);
    }
    gml_builtin_call(&vm,"gamepad_set_button_threshold",NULL,0);
    gml_builtin_state_reset(vm.builtins);
    for(int device=0;device<16 && ok;device++){
      GmlVal arg=vreal(device);
      ok=expect_real("threshold reset",gml_builtin_call(&vm,"gamepad_get_button_threshold",&arg,1),0.5);
    }
    gml_vm_free(&vm);gml_vm_free(&other);
  }
  if(ok)puts("gamepad thresholds: named/cached device defaults, mutation, bounds, isolation and reset passed");
  return ok;
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

typedef struct {
  int count,invalid_polls;
  unsigned connected,directions[16];
} HatFixture;

static int hat_fixture_count(void *userdata){ return ((HatFixture *)userdata)->count; }
static int hat_fixture_connected(void *userdata,int device){
  HatFixture *f=userdata;
  if(device<0 || device>=f->count || device>=16){f->invalid_polls++;return 0;}
  return (f->connected&(1u<<device))!=0;
}
static int hat_fixture_button(void *userdata,int device,int button,int edge){
  HatFixture *f=userdata;
  if(device<0 || device>=f->count || device>=16 || edge ||
     button<32781 || button>32784){f->invalid_polls++;return 0;}
  static const unsigned bits[]={1,4,8,2};
  return (int)(f->directions[device]&bits[button-32781]);
}
static GmlVal hat_query(GmlVM *vm,const char *name,GmlVal *args,int count,int route,int *ok){
  if(route==0) return gml_builtin_call(vm,name,args,count);
  if(route==1) return call_fast(vm,name,args,count,ok);
  return gml_builtin_call_fast_id(vm,BID_INPUT_KBGP,name,args,count);
}
static int gamepad_hat_contract(GmlVM *vm){
  void *saved_userdata=vm->input.userdata;
  int (*saved_count)(void *)=vm->input.gamepad_device_count;
  int (*saved_connected)(void *,int)=vm->input.gamepad_connected;
  int (*saved_button)(void *,int,int,int)=vm->input.gamepad;
  HatFixture f={.count=4,.connected=15};
  vm->input.userdata=&f;vm->input.gamepad_device_count=hat_fixture_count;
  vm->input.gamepad_connected=hat_fixture_connected;vm->input.gamepad=hat_fixture_button;
  int ok=gml_builtin_fast_id(vm,"gamepad_hat_count")==BID_GAMEPAD_HAT_COUNT &&
         gml_builtin_fast_id(vm,"gamepad_hat_value")==BID_GAMEPAD_HAT_VALUE;
  /* Hand-authored direction truth table, including cancellation of opposing pairs. */
  static const int expected[]={0,1,2,3,4,0,6,2,8,9,0,1,12,8,4,0};
  for(int route=0;route<3;route++){
    for(int device=0;device<4;device++) for(unsigned directions=0;directions<16;directions++){
      for(int other=0;other<4;other++) f.directions[other]=(directions+(unsigned)other*3u)&15u;
      f.directions[device]=directions;
      GmlVal args[]={vreal(device),vreal(0)};
      ok=expect_real("one hat per connected pad",hat_query(vm,"gamepad_hat_count",args,1,route,&ok),1)&&ok;
      ok=expect_real("hat directions use the selected pad",hat_query(vm,"gamepad_hat_value",args,2,route,&ok),expected[directions])&&ok;
    }
    GmlVal args[]={vreal(2),vreal(0)};
    f.connected=11;
    ok=expect_real("disconnected pad has no hats",hat_query(vm,"gamepad_hat_count",args,1,route,&ok),0)&&ok;
    ok=expect_real("disconnected pad has no directions",hat_query(vm,"gamepad_hat_value",args,2,route,&ok),0)&&ok;
    f.connected=15;
    static const double invalid[]={-1,-0.25,4,1e100,NAN,INFINITY,-INFINITY};
    for(unsigned i=0;i<sizeof invalid/sizeof invalid[0];i++){
      args[0]=vreal(invalid[i]);args[1]=vreal(0);
      ok=expect_real("invalid hat device count",hat_query(vm,"gamepad_hat_count",args,1,route,&ok),0)&&ok;
      ok=expect_real("invalid hat device value",hat_query(vm,"gamepad_hat_value",args,2,route,&ok),0)&&ok;
      args[0]=vreal(0);args[1]=vreal(invalid[i]);
      ok=expect_real("invalid hat index",hat_query(vm,"gamepad_hat_value",args,2,route,&ok),0)&&ok;
    }
    args[0]=vreal(0);args[1]=vreal(1);
    ok=expect_real("second hat is unavailable",hat_query(vm,"gamepad_hat_value",args,2,route,&ok),0)&&ok;
    ok=expect_real("missing hat count argument",hat_query(vm,"gamepad_hat_count",NULL,0,route,&ok),0)&&ok;
    ok=expect_real("missing hat index",hat_query(vm,"gamepad_hat_value",args,1,route,&ok),0)&&ok;
    f.count=16;f.connected=1u<<15;f.directions[15]=9;
    args[0]=vreal(15.75);args[1]=vreal(0.75);
    ok=expect_real("host slot capacity is not hardcoded",hat_query(vm,"gamepad_hat_count",args,1,route,&ok),1)&&ok;
    ok=expect_real("finite fractional indices truncate",hat_query(vm,"gamepad_hat_value",args,2,route,&ok),9)&&ok;
    f.count=4;f.connected=15;
  }
  vm->input.gamepad_device_count=NULL;
  GmlVal args[]={vreal(0),vreal(0)};
  ok=expect_real("missing capability callback is bounded",hat_query(vm,"gamepad_hat_count",args,1,0,&ok),0)&&ok;
  vm->input.gamepad_device_count=hat_fixture_count;vm->input.gamepad_connected=NULL;
  ok=expect_real("missing connection callback is neutral",hat_query(vm,"gamepad_hat_value",args,2,0,&ok),0)&&ok;
  ok=ok && f.invalid_polls==0;
  vm->input.userdata=saved_userdata;vm->input.gamepad_device_count=saved_count;
  vm->input.gamepad_connected=saved_connected;vm->input.gamepad=saved_button;
  if(ok) puts("gamepad hats: named/exact/prefix routes, all directions, isolated slots and bounds passed");
  return ok;
}

static int gamepad_guid_device_count(void *userdata){
  return *(int *)userdata;
}

static int gamepad_guid_capability_contract(GmlVM *vm){
  void *saved_userdata=vm->input.userdata;
  int (*saved_count)(void *)=vm->input.gamepad_device_count;
  int (*saved_connected)(void *,int)=vm->input.gamepad_connected;
  int count=4,ok=1;
  vm->input.userdata=&count;
  vm->input.gamepad_device_count=gamepad_guid_device_count;
  vm->input.gamepad_connected=gamepad_fixture_connected;
  static const struct { double index; const char *expected; } cases[]={
    {0,"none"},{1,"none"},{3,"none"},{0.75,"none"},{3.75,"none"},
    {-1,"device index out of range"},{-0.5,"device index out of range"},
    {4,"device index out of range"},{1e300,"device index out of range"},
    {NAN,"device index out of range"},{INFINITY,"device index out of range"},
    {-INFINITY,"device index out of range"}
  };
  for(size_t i=0;i<sizeof cases/sizeof cases[0];i++){
    GmlVal argument=vreal(cases[i].index);
    ok=expect_string("GUID ordinary capability result",
      gml_builtin_call(vm,"gamepad_get_guid",&argument,1),cases[i].expected)&&ok;
    ok=expect_string("GUID cached capability result",
      call_fast(vm,"gamepad_get_guid",&argument,1,&ok),cases[i].expected)&&ok;
    ok=expect_string("GUID prefix-cached capability result",
      gml_builtin_call_fast_id(vm,BID_INPUT_KBGP,"gamepad_get_guid",&argument,1),
      cases[i].expected)&&ok;
  }
  GmlVal slot=vreal(2);
  count=2;
  ok=expect_string("GUID uses the host slot count",
    gml_builtin_call(vm,"gamepad_get_guid",&slot,1),"device index out of range")&&ok;
  count=12;
  slot=vreal(11);
  ok=expect_string("GUID accepts a wider host slot interval",
    call_fast(vm,"gamepad_get_guid",&slot,1,&ok),"none")&&ok;
  ok=expect_string("GUID missing argument is invalid",
    gml_builtin_call(vm,"gamepad_get_guid",NULL,0),"device index out of range")&&ok;
  slot=vreal(0);
  vm->input.gamepad_device_count=NULL;
  ok=expect_string("GUID absent slot service is an empty interval",
    call_fast(vm,"gamepad_get_guid",&slot,1,&ok),"device index out of range")&&ok;
  vm->input.userdata=saved_userdata;
  vm->input.gamepad_device_count=saved_count;
  vm->input.gamepad_connected=saved_connected;
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
    int expected=payload_shadows(name)?-1:EXPECTED_##cache(id); \
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
    int expected=payload_shadows(name)?-1:EXPECTED_##cache(id); \
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

static int rounded_contextual_precedence(GmlVM *vm){
  const char *names[]={"draw_roundrect_color_ext","draw_roundrect_colour_ext","draw_roundrect_ext"};
  GmlRender render={0}; uint32_t pixels[24*20]={0},reference[24*20];
  render.color=0xFFFFFF; render.alpha=1; render.color_write_mask=15;
  render.circle_precision=64; vm->render=&render;
  gml_vm_software3d_reset(vm);
  gml_render_begin(&render,pixels,24,20,0,0);
  int ok=1;
  for(int i=0;i<3;i++){
    ok=expect_real("2D rounded operation precedes same-named script",
      gml_builtin_call(vm,names[i],NULL,0),0)&&ok;
    if(gml_builtin_fast_id(vm,names[i])!=-1) ok=0;
  }
  (void)gml_builtin_call(vm,"d3d_start",NULL,0);
  GmlVal projection[]={vreal(0),vreal(0),vreal(24),vreal(20),vreal(0)};
  (void)gml_builtin_call(vm,"d3d_set_projection_ortho",projection,5);
  for(int i=0;i<3;i++){
    memset(pixels,0,sizeof pixels);
    GmlVal args[]={vreal(2),vreal(2),vreal(18),vreal(14),vreal(4),vreal(4),
      vreal(i==2?0:255),vreal(16711680),vreal(0)};
    ok=expect_real("active 3D rounded operation retains contextual precedence",
      gml_builtin_call(vm,names[i],args,i==2?7:9),0)&&ok;
    if(pixels[2*24+2] || !(pixels[8*24+10]>>24)){
      fprintf(stderr,"contextual rounded %s: corner=%08x centre=%08x\n",
        names[i],pixels[2*24+2],pixels[8*24+10]); ok=0;
    }
    if(i==0) memcpy(reference,pixels,sizeof pixels);
    if(i==1 && memcmp(reference,pixels,sizeof pixels)){
      fprintf(stderr,"equal-depth rounded redraw changed coverage\n"); ok=0;
    }
  }
  memcpy(reference,pixels,sizeof pixels);
  GmlVal offset[]={vreal(0),vreal(0),vreal(2)};
  (void)gml_builtin_call(vm,"d3d_transform_set_translation",offset,3);
  GmlVal shape[]={vreal(2),vreal(2),vreal(18),vreal(14),vreal(4),vreal(4),
    vreal(255),vreal(255),vreal(0)};
  (void)gml_builtin_call(vm,"draw_roundrect_color_ext",shape,9);
  if(memcmp(reference,pixels,sizeof pixels)){
    fprintf(stderr,"farther rounded fill passed the depth test\n"); ok=0;
  }
  offset[2]=vreal(-2);
  (void)gml_builtin_call(vm,"d3d_transform_set_translation",offset,3);
  (void)gml_builtin_call(vm,"draw_roundrect_color_ext",shape,9);
  if(pixels[8*24+10]!=0xFFFF0000u){
    fprintf(stderr,"nearer rounded fill failed the depth test\n"); ok=0;
  }
  (void)gml_builtin_call(vm,"d3d_end",NULL,0);
  gml_render_free(&render); vm->render=NULL;
  return ok;
}

static int instance_id_view_contract(void){
  GmlWin content={0};
  GmlVM vm;
  if(gml_vm_init(&vm,&content,NULL)) return 0;
  vm.inst_count=5;
  for(int i=0;i<5;i++){
    vm.inst[i].id=100017u+(uint32_t)i*3u;
    vm.inst[i].active=1;
  }
  vm.inst[0].active=0;
  vm.inst[0].deactivated=1;
  vm.inst[2].marked=1;
  vm.cur_self=&vm.inst[1];
  int ok=1;
  int fast=gml_builtin_fast_id(&vm,"instance_id_get");
  ok=fast==BID_INSTANCE_ID_GET && ok;
  for(int cached=0;cached<2;cached++){
    const double indices[]={0,1,2,3,-1,0.75,INFINITY,-INFINITY,NAN,1e100};
    const double expected[]={100020,100026,100029,-4,-4,100020,-4,-4,-4,-4};
    for(unsigned i=0;i<sizeof(indices)/sizeof(indices[0]);i++){
      GmlVal argument=vreal(indices[i]);
      GmlVal value=cached?gml_builtin_call_fast_id(&vm,fast,"instance_id_get",&argument,1)
                         :gml_builtin_call(&vm,"instance_id_get",&argument,1);
      ok=expect_real("virtual instance enumeration",value,expected[i]) && ok;
    }
    GmlVal missing=cached?gml_builtin_call_fast_id(&vm,fast,"instance_id_get",NULL,0)
                         :gml_builtin_call(&vm,"instance_id_get",NULL,0);
    ok=expect_real("missing instance index",missing,-4) && ok;
  }
  ok=expect_real("bare instance view",gml_vm_identifier_get(&vm,"instance_id"),-4) && ok;
  int globals_before=vm.globals.len;
  gml_vm_assign_current_scope(&vm,"instance_id",vreal(12));
  ok=expect_real("read-only instance view",gml_vm_variable_get_h(&vm,IT_GLOBAL,"instance_id",gml_value_name_hash("instance_id")),-4) &&
     vm.globals.len==globals_before && !gml_varmap_get(&vm.cur_self->vars,"instance_id") && ok;
  vm.inst[0].active=1;
  vm.inst[0].deactivated=0;
  ok=expect_real("reactivated view",gml_builtin_instance_id_at(&vm,0),100017) && ok;
  vm.inst[0].marked=1;
  ok=expect_real("destroyed view",gml_builtin_instance_id_at(&vm,0),100020) && ok;
  vm.inst_count=0;
  ok=expect_real("empty instance view",gml_builtin_instance_id_at(&vm,0),-4) && ok;
  vm.inst_count=5;
  GmlInstance *record=gml_struct_new(&vm);
  if(record){
    vm.cur_self=record;
    gml_vm_assign_current_scope(&vm,"instance_id",vreal(81));
    ok=expect_real("struct field retains its name",gml_vm_identifier_get(&vm,"instance_id"),81) && ok;
    vm.cur_self=&vm.inst[1];
  } else ok=0;
  gml_vm_free(&vm);
  if(ok) puts("instance IDs: named/cached active view, bounds and read-only scalar passed");
  return ok;
}

typedef struct {
  GmlWin *win;
  uint32_t cursor;
} ExceptionProgram;

static uint32_t exception_emit_word(ExceptionProgram *program,uint32_t word){
  uint32_t site=program->cursor;
  store_u32le(program->win->data+site,word);program->cursor+=4;
  return site;
}
static uint32_t exception_emit_integer(ExceptionProgram *program,int value){
  return exception_emit_word(program,UINT32_C(0x84000000)|(uint16_t)value);
}
static void exception_emit_call(ExceptionProgram *program,const char *name,int argc){
  exception_emit_word(program,(uint32_t)OP_CALL<<24|(uint32_t)DT_VAR<<16|(unsigned)argc);
  uint32_t reference=exception_emit_word(program,0);
  int index=program->win->n_refs++;
  program->win->ref_addr[index]=reference;
  program->win->ref_name[index]=name;
  program->win->ref_kind[index]=1;
}
static void exception_emit_statement(ExceptionProgram *program,const char *name,int argc){
  exception_emit_call(program,name,argc);
  exception_emit_word(program,(uint32_t)OP_POPZ<<24|(uint32_t)DT_VAR<<16);
}

static int finish_finally_execution_contract(void){
  int ok=1;
  for(int paused=0;paused<2;paused++) for(int uncached=0;uncached<2;uncached++)
  for(int embedded=0;embedded<2;embedded++){
    if(paused && uncached) continue; /* The established wait owner requires a decoded cache. */
    GmlWin win={.bytecode=17,.size=2048,.owns=1};GmlVM vm={0};
    win.data=calloc(win.size,1);win.code=calloc(2,sizeof *win.code);win.n_code=2;
    win.ref_addr=calloc(32,sizeof *win.ref_addr);win.ref_name=calloc(32,sizeof *win.ref_name);
    win.ref_kind=calloc(32,sizeof *win.ref_kind);
    if(!win.data || !win.code || !win.ref_addr || !win.ref_name || !win.ref_kind){
      gml_win_free(&win);return 0;
    }
    const uint32_t start=128,offset=embedded?32:0;
    ExceptionProgram program={&win,start};
    exception_emit_integer(&program,-1);
    uint32_t catch_site=exception_emit_integer(&program,0);
    exception_emit_statement(&program,"@@try_hook@@",2);
    exception_emit_statement(&program,"exception_child",0);
    exception_emit_integer(&program,-101);
    exception_emit_word(&program,(uint32_t)OP_RET<<24|(uint32_t)DT_VAR<<16);
    uint32_t catch_offset=program.cursor-start+offset;
    exception_emit_statement(&program,"@@try_unhook@@",0);
    if(paused){
      exception_emit_integer(&program,999);
      exception_emit_statement(&program,"random_set_seed",1);
    }
    exception_emit_word(&program,(uint32_t)OP_RET<<24|(uint32_t)DT_VAR<<16);
    win.code[0]=(GmlCode){.name="neutral_exception_parent",.start=start,
      .length=program.cursor-start,.bytecode_offset=offset,.embedded_function=embedded,
      .cache_bad=uncached};
    store_u32le(win.data+catch_site,UINT32_C(0x84000000)|catch_offset);
    uint32_t child_start=program.cursor;
    uint32_t finally_site=exception_emit_integer(&program,0);
    exception_emit_integer(&program,-1);
    exception_emit_statement(&program,"@@try_hook@@",2);
    exception_emit_integer(&program,42);
    exception_emit_statement(&program,"@@throw@@",1);
    exception_emit_integer(&program,-102);
    exception_emit_word(&program,(uint32_t)OP_RET<<24|(uint32_t)DT_VAR<<16);
    uint32_t finally_offset=program.cursor-child_start+offset;
    exception_emit_statement(&program,"@@try_unhook@@",0);
    exception_emit_integer(&program,777);
    exception_emit_statement(&program,"random_set_seed",1);
    if(paused){
      uint32_t loop=program.cursor;
      exception_emit_call(&program,"random_get_seed",0);
      exception_emit_integer(&program,777);
      exception_emit_word(&program,(uint32_t)OP_CMP<<24|(uint32_t)DT_VAR<<16|
                                   (uint32_t)CMP_EQ<<8);
      uint32_t leave=exception_emit_word(&program,(uint32_t)OP_BF<<24);
      exception_emit_integer(&program,0);
      exception_emit_statement(&program,"sleep",1);
      uint32_t back=program.cursor;
      exception_emit_word(&program,(uint32_t)OP_B<<24|((loop-back)/4u&0x7fffffu));
      store_u32le(win.data+leave,(uint32_t)OP_BF<<24|((program.cursor-leave)/4u));
    }
    exception_emit_statement(&program,"@@finish_finally@@",0);
    exception_emit_integer(&program,-103);
    exception_emit_word(&program,(uint32_t)OP_RET<<24|(uint32_t)DT_VAR<<16);
    win.code[1]=(GmlCode){.name="gml_Script_exception_child",.start=child_start,
      .length=program.cursor-child_start,.bytecode_offset=offset,.embedded_function=embedded,
      .cache_bad=uncached};
    store_u32le(win.data+finally_site,UINT32_C(0x84000000)|finally_offset);
    if(gml_vm_init(&vm,&win,NULL)){ gml_win_free(&win);return 0; }
    GmlInstance *self=NULL;
    if(paused){
      self=gml_vm_instances_alloc(&vm);
      if(!self){gml_vm_free(&vm);gml_win_free(&win);return 0;}
      self->active=1;self->obj=-1;self->id=100001;vm.wait_event_scope=1;
    }
    GmlVal result=gml_vm_run_code(&vm,0,self,NULL,NULL,0);
    int passed=1;
    if(paused){
      passed=vm.wait.active && vm.wait.n_frames==2 &&
        vm.wait.frames[0].exceptions.deferred_n==1 &&
        vm.wait.frames[0].exceptions.deferred[0] &&
        !strcmp(vm.wait.frames[0].exceptions.deferred[0],"42") &&
        vm.wait.frames[1].exceptions.handler_n==1;
      size_t capacity=passed?gml_vm_state_size(&vm):0,written=0,used=0,repeated=0;
      unsigned char *state=malloc(capacity?capacity:1),*again=malloc(capacity?capacity:1);
      passed=passed && state && again &&
        gml_vm_state_save(&vm,state,capacity,&written) &&
        gml_vm_state_load(&vm,state,written,&used) && used==written &&
        gml_vm_state_save(&vm,again,capacity,&repeated) && repeated==written &&
        !memcmp(state,again,written);
      free(state);free(again);
      GmlVal release=vreal(888);
      (void)gml_builtin_call(&vm,"random_set_seed",&release,1);
      passed=passed && gml_vm_wait_resume(&vm) && !vm.wait.active && vm.rng_state==999;
    }
    GmlInstance *error=paused?(vm.n_structs?vm.structs[vm.n_structs-1]:NULL)
      :(gml_val_is_number_or_object(result) && GML_IS_STRUCT_ID(result.d)
        ?gml_struct_find(&vm,(unsigned)result.d):NULL);
    GmlVal *message=error?gml_varmap_get(&error->vars,"message"):NULL;
    passed=passed && message && message->t==V_STR && message->s && !strcmp(message->s,"42") &&
      vm.rng_state==(paused?999u:777u) && !vm.exception_context && vm.execution_depth==0;
    if(!passed){ fprintf(stderr,"exception route failed: paused=%d uncached=%d embedded=%d result=%g seed=%u\n",
                         paused,uncached,embedded,result.d,vm.rng_state);ok=0; }
    gml_vm_free(&vm);gml_win_free(&win);
  }
  if(ok) puts("internal finalization: throw/finally/caller catch, cached/raw/shared spans and exact parked-chain resume passed");
  return ok;
}

static int finish_finally_contract(GmlVM *vm,LogFixture *logs){
  int ok=1,id=gml_builtin_fast_id(vm,"@@finish_finally@@");
  ok &= id>0 && id!=gml_builtin_fast_id(vm,"@@finally@@");
  uint32_t rng[16],seed=vm->rng_state,classic=vm->rng_classic_state;
  memcpy(rng,vm->rng_well,sizeof rng);int rng_index=vm->rng_index;
  int relative=vm->action_relative,wait=vm->wait_requested;
  GmlExceptionContext *saved=vm->exception_context;
  for(int cached=0;cached<2;cached++){
    GmlExceptionContext context={0};context.code_length=128;
    vm->exception_context=&context;
    context.state.deferred[0]=strdup("neutral outer exception");
    context.state.deferred[1]=strdup("neutral inner exception");
    context.state.deferred_n=2;
    for(int i=0;i<2;i++){
      int before=logs->count;
      GmlVal actual=cached?call_fast(vm,"@@finish_finally@@",NULL,0,&ok)
                          :gml_builtin_call(vm,"@@finish_finally@@",NULL,0);
      const char *expected=i==0?"neutral inner exception":"neutral outer exception";
      ok=actual.t==V_UNDEF && logs->count==before &&
         context.state.pending && !strcmp(context.state.pending,expected) &&
         context.state.deferred_n==1-i && ok;
      free(context.state.pending);context.state.pending=NULL;
    }
    GmlVal empty=gml_builtin_call(vm,"@@finish_finally@@",NULL,0);
    ok=empty.t==V_UNDEF && !context.state.pending && ok;
    GmlVal thrown=vstr("replacement exception");
    (void)gml_builtin_call(vm,"@@throw@@",&thrown,1);
    ok=context.state.pending && !strcmp(context.state.pending,"replacement exception") && ok;
    gml_vm_exception_state_clear(&context.state);
  }
  vm->exception_context=saved;
  ok=!memcmp(rng,vm->rng_well,sizeof rng) && vm->rng_index==rng_index &&
     vm->rng_state==seed && vm->rng_classic_state==classic &&
     vm->action_relative==relative && vm->wait_requested==wait && ok;
  if(ok) puts("internal finalization: named/cached owned deferred messages and unchanged control state passed");
  return ok;
}

static int legacy_question_contract(GmlVM *vm,LogFixture *logs){
  int ok=1,canonical=gml_builtin_fast_id(vm,"show_question");
  ok &= gml_builtin_fast_id(vm,"action_if_question")==canonical && canonical>0;
  uint32_t rng[16],seed=vm->rng_state,classic=vm->rng_classic_state;
  memcpy(rng,vm->rng_well,sizeof rng);int rng_index=vm->rng_index;
  int relative=vm->action_relative,wait=vm->wait_requested;
  GmlVal questions[]={vstr(""),vstr("neutral question")};
  for(int cached=0;cached<2;cached++) for(int i=0;i<2;i++){
    GmlVal expected=gml_builtin_call(vm,"show_question",&questions[i],1);
    int before=logs->count;
    GmlVal actual=cached?call_fast(vm,"action_if_question",&questions[i],1,&ok)
                        :gml_builtin_call(vm,"action_if_question",&questions[i],1);
    ok=expect_real("legacy question uses the canonical unavailable answer",actual,expected.d) && ok;
    ok=actual.t==expected.t && logs->count==before && ok;
  }
  ok=!memcmp(rng,vm->rng_well,sizeof rng) && vm->rng_index==rng_index &&
     vm->rng_state==seed && vm->rng_classic_state==classic &&
     vm->action_relative==relative && vm->wait_requested==wait && ok;
  if(ok) puts("legacy question: named/cached canonical answer and unchanged control state passed");
  return ok;
}

static int unavailable_steam_contract(GmlVM *vm,LogFixture *logs){
  const char *names[]={"steam_file_exists","steam_file_read","steam_file_write",
                       "steam_get_persona_name","steam_stats_ready"};
  GmlVal args[]={vstr("neutral-cloud-slot"),vstr("neutral payload")};
  int ok=1;
  for(int cached=0;cached<2;cached++){
    for(int i=0;i<5;i++){
      int before=logs->count;
      GmlVal result=cached?call_fast(vm,names[i],args,2,&ok)
                          :gml_builtin_call(vm,names[i],args,2);
      if(i==1 || i==3) ok=expect_string(names[i],result,"") && ok;
      else ok=expect_real(names[i],result,0) && ok;
      if(logs->count!=before){
        fprintf(stderr,"%s must be a recognized unavailable capability\n",names[i]);
        ok=0;
      }
      gml_values_release(&result,1);
    }
  }
  if(ok) puts("Steam unavailable capabilities: named/cached typed answers passed");
  return ok;
}

static int internal_value_helpers_contract(void){
  GmlWin win={.bytecode=17};
  GmlVM vm;
  if(gml_vm_init(&vm,&win,NULL)) return 0;
  GmlVal values=gml_arr_new(4,vreal(0));
  GmlVal *root=gml_varmap_put(&vm.globals,"helper_values");
  if(values.t!=V_ARR || !root){gml_vm_free(&vm);return 0;}
  *root=values;
  gml_arr_set(values,1,vstr("leaf"));
  gml_arr_set(values,2,vint64(INT64_C(9007199254740993)));
  gml_arr_set(values,3,gml_arr_new(1,vreal(77)));
  GmlVal legacy=gml_arr_new(0,vreal(0));
  GmlVal *legacy_root=gml_varmap_put(&vm.globals,"helper_legacy_values");
  if(!legacy.arr || !legacy_root){gml_vm_free(&vm);return 0;}
  *legacy_root=legacy;
  ((GmlArr *)legacy.arr)->is_2d=1;
  gml_arr_set_2d(legacy,1,2,vreal(17));
  int ok=gml_builtin_fast_id(&vm,"@@array_get@@")>0 &&
         gml_builtin_fast_id(&vm,"@@string@@")>0;
  for(int cached=0;cached<2;cached++){
    gml_arr_set(values,0,vreal(31));
    gml_arr_set(values,1,vstr("leaf"));
    GmlVal args[]={values,vreal(0)};
    GmlVal number=cached?call_fast(&vm,"@@array_get@@",args,2,&ok)
                        :gml_builtin_call(&vm,"@@array_get@@",args,2);
    ok=expect_real("internal numeric array read",number,31) && ok;
    args[1]=vreal(1);
    GmlVal text=cached?call_fast(&vm,"@@array_get@@",args,2,&ok)
                      :gml_builtin_call(&vm,"@@array_get@@",args,2);
    gml_arr_set(values,1,vstr("changed"));
    ok=expect_string("internal string array read",text,"leaf") && ok;
    args[1]=vreal(2);
    GmlVal wide=cached?call_fast(&vm,"@@array_get@@",args,2,&ok)
                      :gml_builtin_call(&vm,"@@array_get@@",args,2);
    ok=wide.t==V_REAL && gml_val_number_kind(wide)==GML_NUM_INT64 &&
       wide.integer==INT64_C(9007199254740993) && ok;
    args[1]=vreal(3);
    GmlVal nested=cached?call_fast(&vm,"@@array_get@@",args,2,&ok)
                        :gml_builtin_call(&vm,"@@array_get@@",args,2);
    ok=nested.t==V_ARR && nested.arr==gml_arr_get(values,3).arr && ok;
    ok=expect_real("internal nested array read",gml_arr_get(nested,0),77) && ok;
    gml_arr_set(nested,0,vreal(79));
    ok=expect_real("internal read retains the nested reference",
                   gml_arr_get(gml_arr_get(values,3),0),79) && ok;
    gml_arr_set(nested,0,vreal(77));
    gml_arr_set(values,0,vreal(61));args[1]=vreal(0);
    GmlVal changed=cached?call_fast(&vm,"@@array_get@@",args,2,&ok)
                         :gml_builtin_call(&vm,"@@array_get@@",args,2);
    ok=expect_real("internal reader observes later writes",changed,61) && ok;
    GmlVal inputs[]={text,wide,vstr("{0}")};
    const char *expected[]={"leaf","9007199254740993","{0}"};
    GmlVal strings[3];
    for(int i=0;i<3;i++){
      strings[i]=cached?call_fast(&vm,"@@string@@",&inputs[i],1,&ok)
                        :gml_builtin_call(&vm,"@@string@@",&inputs[i],1);
      ok=expect_string("internal scalar conversion",strings[i],expected[i]) && ok;
    }
    gml_values_release(strings,3);
    gml_values_release(&text,1);
    const GmlVal invalid[][2]={
      {vreal(0),vreal(0)}, {values,vreal(-1)}, {values,vreal(4)},
      {values,vreal(NAN)}, {values,vreal(1e30)}
    };
    for(size_t i=0;i<sizeof invalid/sizeof invalid[0];i++){
      GmlVal bad[]={invalid[i][0],invalid[i][1]};
      GmlVal result=cached?call_fast(&vm,"@@array_get@@",bad,2,&ok)
                          :gml_builtin_call(&vm,"@@array_get@@",bad,2);
      ok=result.t==V_UNDEF && ok;
    }
    for(int count=0;count<2;count++){
      GmlVal result=cached?call_fast(&vm,"@@array_get@@",args,count,&ok)
                          :gml_builtin_call(&vm,"@@array_get@@",args,count);
      ok=result.t==V_UNDEF && ok;
    }
    GmlVal format[]={vstr("{{0}}:{1}"),vstr("leaf"),vreal(9)};
    GmlVal formatted=cached?call_fast(&vm,"@@string@@",format,3,&ok)
                           :gml_builtin_call(&vm,"@@string@@",format,3);
    ok=expect_string("internal interpolation keeps literal outer braces",formatted,"{leaf}:9") && ok;
    gml_values_release(&formatted,1);
    GmlVal undefined=vundef();
    GmlVal converted=cached?call_fast(&vm,"@@string@@",&undefined,1,&ok)
                           :gml_builtin_call(&vm,"@@string@@",&undefined,1);
    ok=expect_string("internal undefined conversion",converted,"undefined") && ok;
    gml_values_release(&converted,1);
    GmlVal empty=cached?call_fast(&vm,"@@string@@",NULL,0,&ok)
                       :gml_builtin_call(&vm,"@@string@@",NULL,0);
    ok=expect_string("internal empty conversion",empty,"") && ok;
    gml_values_release(&empty,1);
    GmlVal legacy_args[]={legacy,vreal(GML_2D_STRIDE+2)};
    GmlVal cell=cached?call_fast(&vm,"@@array_get@@",legacy_args,2,&ok)
                      :gml_builtin_call(&vm,"@@array_get@@",legacy_args,2);
    ok=expect_real("internal legacy flattened value",cell,17) && ok;
    legacy_args[1]=vreal(0);
    GmlVal hole=cached?call_fast(&vm,"@@array_get@@",legacy_args,2,&ok)
                      :gml_builtin_call(&vm,"@@array_get@@",legacy_args,2);
    ok=hole.t==V_UNDEF && ok;
  }
  gml_vm_free(&vm);
  if(ok) puts("internal value helpers: named/cached reads, nested aliases and scalar strings passed");
  return ok;
}

int main(int argc,char **argv){
  int guid_only=argc==3 && !strcmp(argv[1],"--case") &&
                !strcmp(argv[2],"input.gamepad_guid");
  if(argc!=1 && !guid_only){
    fprintf(stderr,"usage: %s [--case input.gamepad_guid]\n",argv[0]);
    return 2;
  }
  GmlWin win={0};
  GmlVM vm={0};
  AnygmHostServices host={0};
  LogFixture log_fixture={0};
  if(!setup_fixture(&win,&vm,&host,&log_fixture,"gml_Script_abs")){
    fprintf(stderr,"builtin dispatch fixture setup failed\n");
    gml_vm_free(&vm);
    gml_win_free(&win);
    return 1;
  }
  int ok=guid_only ? gamepad_guid_capability_contract(&vm) :
         canonical_registry_resolution(&vm) &&
         finish_finally_execution_contract() &&
         finish_finally_contract(&vm,&log_fixture) &&
         legacy_question_contract(&vm,&log_fixture) &&
         unavailable_steam_contract(&vm,&log_fixture) &&
         internal_value_helpers_contract() &&
         instance_id_view_contract() &&
         gamepad_guid_capability_contract(&vm) &&
         gamepad_hat_contract(&vm) &&
         payload_script_precedes_exact_builtin(&vm) &&
         script_resolution_order(&vm,&log_fixture) &&
         show_error_contract(&vm,&log_fixture) &&
         function_value_and_alias_resolution(&vm) &&
         texture_interpolation_ext_alias(&vm,&log_fixture) &&
         internal_instance_receiver_conversion(&vm,&log_fixture) &&
         hsv_color_byte_wrapping(&vm) &&
         gain_conversion(&vm) &&
         action_variable_comparisons(&vm) &&
         action_variable_string_comparisons(&vm) &&
         room_dimension_mutation(&vm) &&
         mouse_none_semantics(&vm) &&
         action_mouse_contract() &&
         ds_fast_interface(&vm) &&
         classic_dynamic_global_declaration(&vm) &&
         external_audio_definition_dispatch(&vm) &&
         portable_service_contracts(&vm) &&
         portable_joystick_contract(&vm) &&
         external_joydll_binding_reaches_the_pad(&vm) &&
         gamepad_threshold_contract() &&
         a_second_pad_is_its_own_device(&vm) &&
         gamepad_axis_orientation_contract(&vm) &&
         digital_pad_reaches_axis_readers(&vm) &&
         layer_instance_move(&vm) &&
         scalar_shape_queries_accept_target_arrays(&vm) &&
         layer_fx_answers_none(&vm) &&
         missing_stream_is_not_sound_zero(&vm) &&
         inherited_collision_resolves_numeric_suffix(&vm) &&
         surface_general_reaches_the_screen(&vm) &&
         rounded_contextual_precedence(&vm);
  gml_vm_free(&vm);
  gml_win_free(&win);
  if(!guid_only){
    ok=payload_shadow_cache_lifetime(&win,&vm,&host,&log_fixture) && ok;
    ok=embedded_function_binding(&win,&vm,&host,&log_fixture) && ok;
  }
  if(!ok) return 1;
  puts(guid_only ? "gamepad identity capability contract: ok" :
                  "builtin dispatch precedence fixtures: ok");
  return 0;
}
