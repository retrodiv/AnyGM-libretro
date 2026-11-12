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
static GmlVal sprite_info_field(GmlVM *vm,GmlVal object,const char *name){
  GmlInstance *record=gml_val_is_number_or_object(object) && GML_IS_STRUCT_ID(object.d)
    ?gml_struct_find(vm,(unsigned)object.d):NULL;
  GmlVal *field=record?gml_varmap_get(&record->vars,name):NULL;
  return field?*field:vundef();
}
static int sprite_information_case(void){
  int all=1;
  for(int cached=0;cached<2;cached++){
    int ok=1;GmlWin win={.bytecode=17};GmlVM vm={0};
    if(gml_vm_init(&vm,&win,NULL))return 0;
    int frame=0;
    GmlSprite sprite={.name="neutral_bitmap",.w=20,.h=12,.n_frames=1,.frame=&frame,
      .originx=3,.originy=4,.ml=2,.mt=1,.mr=18,.mb=10,.mask_count=1,
      .metadata_flags=GML_SPRITE_META_SMOOTH|GML_SPRITE_META_NINESLICE,
      .playback_speed=0.25f,.playback_speed_type=1,.playback_speed_valid=1,
      .ns_enabled=1,.ns_l=2,.ns_t=1,.ns_r=3,.ns_b=2,.ns_tile={0,1,2,3,4}};
    GmlTpag page={.sx=7,.sy=9,.sw=8,.sh=6,.tx=2,.ty=1,.tw=16,.th=10,
                  .bw=20,.bh=12,.atlas=0};
    GmlAtlas atlas={.w=64,.h=64};
    GmlRender render={.win=&win,.spr=&sprite,.n_spr=1,.tpag=&page,.n_tpag=1,
                      .atlas=&atlas,.n_atlas=1};
    vm.render=&render;
    GmlVal info=query(&vm,"sprite_get_info",vreal(0),cached,&ok);
    ok &= expect(GML_IS_STRUCT_ID(info.d),"sprite info returns a real struct");
    ok &= expect(real_is(sprite_info_field(&vm,info,"width"),20) &&
                 real_is(sprite_info_field(&vm,info,"height"),12) &&
                 real_is(sprite_info_field(&vm,info,"xoffset"),3) &&
                 real_is(sprite_info_field(&vm,info,"yoffset"),4),
                 "information preserves logical metrics and origin");
    ok &= expect(real_is(sprite_info_field(&vm,info,"smooth"),1) &&
                 real_is(sprite_info_field(&vm,info,"use_mask"),1) &&
                 real_is(sprite_info_field(&vm,info,"frame_speed"),0.25),
                 "information preserves resource flags, masks and playback");
    GmlVal frames=sprite_info_field(&vm,info,"frames");
    GmlVal first=gml_arr_get(frames,0);
    ok &= expect(gml_val_array_length(frames)==1 &&
                 real_is(sprite_info_field(&vm,first,"x"),7) &&
                 real_is(sprite_info_field(&vm,first,"y"),9) &&
                 real_is(sprite_info_field(&vm,first,"w"),8) &&
                 real_is(sprite_info_field(&vm,first,"crop_width"),16) &&
                 real_is(sprite_info_field(&vm,first,"original_width"),20) &&
                 real_is(sprite_info_field(&vm,first,"texture"),gml_render_atlas_texture_handle(&render,0)),
                 "frame texture, packed, cropped and original geometry remain distinct");
    GmlVal slice=sprite_info_field(&vm,info,"nineslice");
    GmlVal modes=sprite_info_field(&vm,slice,"tilemode");
    ok &= expect(real_is(sprite_info_field(&vm,slice,"left"),2) &&
                 gml_val_array_length(modes)==5 && gml_arr_get(modes,4).d==4,
                 "nine-slice information retains authored borders and modes");
    gml_arr_set(modes,4,vreal(0));
    GmlInstance *record=gml_struct_find(&vm,(unsigned)info.d);
    if(record)*gml_varmap_put(&record->vars,"width")=vreal(99);
    record=gml_struct_find(&vm,(unsigned)first.d);
    if(record)*gml_varmap_put(&record->vars,"original_width")=vreal(99);
    GmlVal fresh=query(&vm,"sprite_get_info",vreal(0),cached,&ok);
    GmlVal fresh_frame=gml_arr_get(sprite_info_field(&vm,fresh,"frames"),0);
    GmlVal fresh_slice=sprite_info_field(&vm,fresh,"nineslice");
    ok &= expect(real_is(sprite_info_field(&vm,fresh,"width"),20) &&
                 real_is(sprite_info_field(&vm,fresh_frame,"original_width"),20) &&
                 gml_arr_get(sprite_info_field(&vm,fresh_slice,"tilemode"),4).d==4 &&
                 sprite.w==20 && sprite.ns_tile[4]==4 && !atlas.decode_attempted,
                 "mutating nested snapshots changes no resource or later query and decodes no image");
    sprite.name="renamed_bitmap";sprite.metadata_flags=GML_SPRITE_META_ROTATED;
    fresh=query(&vm,"sprite_get_info",vreal(0),cached,&ok);
    GmlVal retained_name=sprite_info_field(&vm,info,"name");
    ok &= expect(retained_name.t==V_STR && !strcmp(retained_name.s,"neutral_bitmap") &&
                 real_is(sprite_info_field(&vm,fresh,"rotated_bounds"),1) &&
                 real_is(sprite_info_field(&vm,fresh,"use_mask"),0),
                 "retained text and rotated-bounds metadata stay independent of later source mutation");
    const double invalid[]={-1,1,1e100,NAN,INFINITY};
    for(unsigned i=0;i<sizeof invalid/sizeof invalid[0];i++)
      ok &= expect(query(&vm,"sprite_get_info",vreal(invalid[i]),cached,&ok).t==V_UNDEF,
                   "invalid sprite information returns undefined");
    vm.render=NULL;
    ok &= expect(query(&vm,"sprite_get_info",vreal(0),cached,&ok).t==V_UNDEF,
                 "an absent renderer cannot supply sprite information");
    gml_vm_free(&vm);free(win.str_hix);all &=ok;
  }
  return all;
}
/* The legacy spelling uses the authored asset table with or without a renderer. */
static int legacy_object_names_case(void){
  int ok=1;
  const int indices[]={0,1,2,-1,3};
  const char *expected[]={"neutral_alpha","neutral_beta","","",""};
  for(int bound=0;bound<2;bound++) for(int cached=0;cached<2;cached++){
    GmlObject objects[3]={{.name="neutral_alpha"},{.name="neutral_beta"},{0}};
    GmlObject original[3];memcpy(original,objects,sizeof objects);
    GmlWin win={0};GmlRender render={.win=&win};
    GmlVM vm={.win=&win,.objects=objects,.n_objects=3,.render=bound?&render:NULL};
    int alias_id=gml_builtin_fast_id(&vm,"object_name");
    int canonical_id=gml_builtin_fast_id(&vm,"object_get_name");
    ok &= expect(alias_id>=0 && alias_id==canonical_id,
                 "legacy object name shares the established cached ID");
    for(unsigned index=0;index<sizeof indices/sizeof indices[0];index++){
      GmlVal result=query(&vm,"object_name",vreal(indices[index]),cached,&ok);
      ok &= expect(result.t==V_STR && result.s && !strcmp(result.s,expected[index]),
                   "legacy object name returns authored text or the existing empty fallback");
    }
    GmlVal missing=cached && alias_id>=0
      ?gml_builtin_call_fast_id(&vm,alias_id,"object_name",NULL,0)
      :gml_builtin_call(&vm,"object_name",NULL,0);
    ok &= expect(missing.t==V_STR && missing.s && !*missing.s,
                 "legacy object name without an index returns empty text");
    ok &= expect(!memcmp(objects,original,sizeof objects),
                 "object-name reads preserve all authored object metadata");
    vm.objects=NULL;vm.n_objects=0;vm.render=NULL;gml_vm_free(&vm);
  }
  return ok;
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
    for(int i=0;i<3;i++) timeline->moments[i]=(GmlTimelineMoment){.step=i*10,.code=i};
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
    {"sprite_information",sprite_information_case},
    {"legacy_object_names",legacy_object_names_case},
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
