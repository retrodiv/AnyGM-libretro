/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "gml_builtin.h"
#include "gml_render_internal.h"
#include "anygm_test_runner.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int expect(int condition,const char *message);
static int real_is(GmlVal value,double number);
static GmlVal query(GmlVM *vm,const char *name,GmlVal argument,int cached,int *ok);

static void group_word(unsigned char *data,size_t offset,uint32_t value){
  for(int i=0;i<4;i++) data[offset+i]=(unsigned char)(value>>(8*i));
}
static uint32_t group_fixture(GmlWin *win,unsigned char data[512],int layout){
  static char *strings[]={"texture_group","",".yytex"};
  static uint32_t offsets[]={320,352,360};
  memset(data,0,512);memset(win,0,sizeof(*win));
  win->data=data;win->size=512;win->strs=strings;win->str_charoff=offsets;win->n_strs=3;
  win->n_chunks=2;
  win->chunks[0]=(GmlChunk){"TGIN",16,208};
  win->chunks[1]=(GmlChunk){"TXTR",256,16};
  group_word(data,16,1);group_word(data,20,1);group_word(data,24,40);
  group_word(data,40,320);
  uint32_t list=layout==0?64u:layout==1?76u:72u;
  if(layout){
    group_word(data,44,352);group_word(data,48,360);group_word(data,52,0);
    group_word(data,56,list);
  }else group_word(data,44,list);
  group_word(data,list,2);group_word(data,list+4,2);group_word(data,list+8,0);
  group_word(data,256,3);
  return list;
}
static int texture_groups_case(void){
  int ok=1;
  for(int cached=0;cached<2;cached++) for(int layout=0;layout<3;layout++){
    unsigned char data[512],original[512],pixels[24]={0};GmlWin win;
    group_fixture(&win,data,layout);memcpy(original,data,sizeof(data));
    GmlAtlas atlases[3]={{0}};
    for(int i=0;i<3;i++){atlases[i].w=2;atlases[i].h=3;atlases[i].px=pixels;}
    GmlRender render={.win=&win,.atlas=atlases,.n_atlas=3};
    GmlVM vm={.win=&win,.render=&render};
    GmlVal result=query(&vm,"texturegroup_get_textures",vstr("texture_group"),cached,&ok);
    ok &= expect(gml_val_array_length(result)==2,"group query returns every authored page");
    for(int i=0;i<2;i++){
      GmlVal handle=gml_arr_get(result,i);
      int expected=gml_render_atlas_texture_handle(&render,i?0:2);
      GmlRenderTextureMetrics metrics;
      ok &= expect(real_is(handle,expected) &&
                   gml_render_texture_metrics(&render,expected,&metrics) &&
                   metrics.kind==GML_RENDER_TEXTURE_ATLAS && metrics.full_width==2 &&
                   metrics.full_height==3,"group order and full-page texture metrics agree");
      (void)query(&vm,"texture_prefetch",handle,cached,&ok);
    }
    ok &= expect(!memcmp(data,original,sizeof(data)),"group queries preserve immutable metadata");
    gml_values_release(&result,1);
    GmlVal missing=query(&vm,"texturegroup_get_textures",vstr("Texture_group"),cached,&ok);
    ok &= expect(gml_val_array_length(missing)==0,"group names use exact case-sensitive matching");
    gml_values_release(&missing,1);
    vm.render=NULL;gml_vm_free(&vm);
  }
  return ok;
}
static int malformed_texture_groups_case(void){
  int ok=1;
  for(int defect=0;defect<12;defect++){
    unsigned char data[512];GmlWin win;uint32_t list=group_fixture(&win,data,1);
    GmlRender render={.win=&win,.n_atlas=3};GmlVM vm={.win=&win,.render=&render};
    switch(defect){
      case 0: win.n_chunks=0;break;
      case 1: group_word(data,16,2);break;
      case 2: group_word(data,20,UINT32_MAX);break;
      case 3: group_word(data,24,UINT32_MAX);break;
      case 4: group_word(data,40,319);break;
      case 5: group_word(data,48,359);break;
      case 6: group_word(data,52,3);break;
      case 7: group_word(data,56,UINT32_MAX);break;
      case 8: group_word(data,list,UINT32_MAX);break;
      case 9: group_word(data,list+4,3);break;
      case 10: win.chunks[0].size=10;break;
      case 11: win.size=270;break;
    }
    GmlVal result=query(&vm,"texturegroup_get_textures",vstr("texture_group"),defect%2,&ok);
    ok &= expect(result.t==V_ARR && gml_val_array_length(result)==0,
                 "malformed texture-group spans and references return an empty array");
    gml_values_release(&result,1);vm.render=NULL;gml_vm_free(&vm);
  }
  return ok;
}

static int expect(int condition,const char *message){
  if(!condition) fprintf(stderr,"%s\n",message);
  return condition;
}
static int real_is(GmlVal value,double number){
  return value.t==V_REAL && value.d==number;
}
static GmlVal query(GmlVM *vm,const char *name,GmlVal argument,int cached,int *ok){
  if(!cached) return gml_builtin_call(vm,name,&argument,1);
  int id=gml_builtin_fast_id(vm,name);
  *ok &= expect(id>=0,"query must have a cached builtin ID");
  return id>=0?gml_builtin_call_fast_id(vm,id,name,&argument,1):vundef();
}
static int object_flags_case(void){
  int ok=1;
  const char *names[]={"object_get_solid","object_get_persistent"};
  for(int cached=0;cached<2;cached++){
    GmlObject objects[2]={{.solid=1,.persistent=0},{.solid=0,.persistent=1}};
    GmlInstance instance={.solid=0,.persistent=1};
    GmlVM vm={.objects=objects,.n_objects=2,.cur_self=&instance};
    for(int i=0;i<2;i++){
      ok &= expect(real_is(query(&vm,names[i],vreal(0),cached,&ok),1-i),
                   "object zero returns its asset flag, not the live instance flag");
      ok &= expect(real_is(query(&vm,names[i],vreal(1),cached,&ok),i),
                   "object one keeps an independent authored flag");
    }
    ok &= expect(objects[0].solid==1 && objects[0].persistent==0 &&
                 instance.solid==0 && instance.persistent==1,
                 "object queries do not mutate assets or the current instance");
    vm.objects=NULL; vm.n_objects=0; vm.cur_self=NULL;
    gml_vm_free(&vm);
  }
  return ok;
}
static int timeline_size_case(void){
  int ok=1;
  for(int cached=0;cached<2;cached++){
    GmlVM vm={0};
    GmlVal empty=gml_builtin_call(&vm,"timeline_add",NULL,0);
    GmlVal populated=gml_builtin_call(&vm,"timeline_add",NULL,0);
    if(!real_is(empty,0) || !real_is(populated,1)){ gml_vm_free(&vm); return 0; }
    GmlTimeline *timeline=&vm.timelines[1];
    timeline->moments=calloc(3,sizeof *timeline->moments);
    if(!timeline->moments){ gml_vm_free(&vm); return 0; }
    timeline->n=3; timeline->last_step=20;
    for(int i=0;i<3;i++) timeline->moments[i]=(GmlTimelineMoment){i*10,i};
    ok &= expect(real_is(query(&vm,"timeline_size",empty,cached,&ok),0),
                 "a new timeline has no active moments");
    ok &= expect(real_is(query(&vm,"timeline_size",populated,cached,&ok),3),
                 "timeline size counts moments, not the last step");
    ok &= expect(timeline->n==3 && timeline->last_step==20,
                 "querying a populated timeline leaves its schedule unchanged");
    gml_builtin_call(&vm,"timeline_clear",&populated,1);
    ok &= expect(real_is(query(&vm,"timeline_size",populated,cached,&ok),0) &&
                 real_is(gml_builtin_call(&vm,"timeline_exists",&populated,1),1),
                 "clearing a timeline removes its moments, not its identity");
    gml_vm_free(&vm);
  }
  return ok;
}
static int deadzone_case(void){
  int ok=1;
  for(int cached=0;cached<2;cached++){
    GmlVM vm={0}, other={0};
    GmlVal settings[]={vreal(0),vreal(0.25)};
    gml_builtin_call(&vm,"gamepad_set_axis_deadzone",settings,2);
    settings[0]=vreal(1); settings[1]=vreal(0.75);
    gml_builtin_call(&vm,"gamepad_set_axis_deadzone",settings,2);
    settings[0]=vreal(0); settings[1]=vreal(0.5);
    gml_builtin_call(&other,"gamepad_set_axis_deadzone",settings,2);
    ok &= expect(real_is(query(&vm,"gamepad_get_axis_deadzone",vreal(0),cached,&ok),0.25) &&
                 real_is(query(&vm,"gamepad_get_axis_deadzone",vreal(1),cached,&ok),0.75) &&
                 real_is(query(&other,"gamepad_get_axis_deadzone",vreal(0),cached,&ok),0.5),
                 "deadzone queries preserve per-device and per-VM configuration");
    for(int endpoint=0;endpoint<=1;endpoint++){
      settings[1]=vreal(endpoint);
      gml_builtin_call(&vm,"gamepad_set_axis_deadzone",settings,2);
      ok &= expect(real_is(query(&vm,"gamepad_get_axis_deadzone",vreal(0),cached,&ok),endpoint) &&
                   real_is(query(&vm,"gamepad_get_axis_deadzone",vreal(1),cached,&ok),0.75),
                   "deadzone queries observe updates and both endpoints without changing another slot");
    }
    gml_vm_free(&vm); gml_vm_free(&other);
  }
  return ok;
}
/* Defensive input policy for invalid resource queries. */
static int invalid_case(void){
  int ok=1;
  GmlVM vm={0};
  const char *names[]={"object_get_solid","object_get_persistent","timeline_size"};
  double invalid[]={-1,1e100,NAN,INFINITY};
  for(int cached=0;cached<2;cached++){
    for(int i=0;i<3;i++){
      for(int j=0;j<4;j++)
        ok &= expect(real_is(query(&vm,names[i],vreal(invalid[j]),cached,&ok),0),
                     "invalid resource indices return zero without accessing an asset");
      ok &= expect(real_is(gml_builtin_call(&vm,names[i],NULL,0),0),
                   "a missing resource index returns zero");
    }
    GmlVal fallback=query(&vm,"gamepad_get_axis_deadzone",vreal(0),cached,&ok);
    for(int j=0;j<4;j++)
      ok &= expect(real_is(query(&vm,"gamepad_get_axis_deadzone",vreal(invalid[j]),cached,&ok),fallback.d),
                   "invalid devices use the existing unconfigured-device deadzone policy");
  }
  gml_vm_free(&vm);
  return ok;
}
int main(void){
  static const AnygmTestCase cases[]={
    {"authored_texture_groups",texture_groups_case},
    {"malformed_texture_groups",malformed_texture_groups_case},
    {"object_asset_flags",object_flags_case},
    {"timeline_active_moments",timeline_size_case},
    {"gamepad_configured_deadzone",deadzone_case},
    {"defensive_invalid_arguments",invalid_case},
  };
  const AnygmTestGroup group={"resource_queries",cases,sizeof cases/sizeof cases[0]};
  AnygmTestResult result;
  anygm_test_run_groups(&group,1,NULL,&result);
  return result.failed?EXIT_FAILURE:EXIT_SUCCESS;
}
