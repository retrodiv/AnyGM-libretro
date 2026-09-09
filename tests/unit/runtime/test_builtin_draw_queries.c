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

static GmlVal query(GmlVM *vm,const char *name,GmlVal *args,int count,int cached,int *ok){
  if(!cached) return gml_builtin_call(vm,name,args,count);
  int id=gml_builtin_fast_id(vm,name);
  if(id<0){ *ok=0; return vundef(); }
  return gml_builtin_call_fast_id(vm,id,name,args,count);
}
static int real_is(GmlVal v,double number){ return v.t==V_REAL && v.d==number; }
static int string_is(GmlVal v,const char *text){
  int ok=v.t==V_STR && v.s && !strcmp(v.s,text);
  gml_values_release(&v,1); return ok;
}
static int circle_case(void){
  int ok=1;
  for(int cached=0;cached<2;cached++){
    GmlRender render={0}; GmlVM vm={.render=&render};
    int values[]={4,12,32,64};
    for(int i=0;i<4;i++){
      GmlVal arg=vreal(values[i]);
      gml_builtin_call(&vm,"draw_set_circle_precision",&arg,1);
      ok &= real_is(query(&vm,"draw_get_circle_precision",NULL,0,cached,&ok),values[i]);
      GmlRenderDrawState state;
      gml_render_draw_state_get(&render,&state);
      ok &= state.circle_precision==values[i];
    }
    vm.render=NULL; gml_vm_free(&vm);
  }
  return ok;
}
static void word(uint8_t *data,int offset,uint32_t value){
  for(int i=0;i<4;i++) data[offset+i]=(uint8_t)(value>>(i*8));
}
static int shader_case(void){
  int ok=1;
  for(int cached=0;cached<2;cached++){
    uint8_t bytes[64]={0};
    char first[]="shade_first",second[]="shade_second";
    char *names[]={first,second}; uint32_t offsets[]={40,52};
    GmlWin win={.data=bytes,.size=sizeof bytes,.n_chunks=1,
      .strs=names,.str_charoff=offsets,.n_strs=2};
    win.chunks[0]=(GmlChunk){"SHDR",8,12};
    word(bytes,8,2); word(bytes,12,24); word(bytes,16,28);
    word(bytes,24,40); word(bytes,28,52);
    GmlVM vm={.win=&win};
    GmlVal arg=vreal(0);
    GmlVal name=query(&vm,"shader_get_name",&arg,1,cached,&ok);
    first[0]='X';
    ok &= string_is(name,"shade_first"); /* own the returned name, not borrowed content */
    arg=vreal(1); ok &= string_is(query(&vm,"shader_get_name",&arg,1,cached,&ok),second);
    double bad[]={-1,2,NAN,INFINITY,1e100};
    for(int i=0;i<5;i++){
      arg=vreal(bad[i]); ok &= string_is(query(&vm,"shader_get_name",&arg,1,cached,&ok),"");
    }
    vm.win=NULL; gml_vm_free(&vm);
  }
  return ok;
}
static int depth_matches(GmlVal result,const int *ids,int count){
  GmlArr *array=result.arr;
  int ok=result.t==V_ARR && array && gml_val_array_length(result)==(count?count:1);
  if(ok && !count) ok=real_is(array->data[0],-1);
  for(int i=0;ok && i<count;i++){
    int found=0;
    for(int j=0;j<count;j++) found+=real_is(array->data[j],ids[i]);
    ok &= found==1;
  }
  gml_values_release(&result,1); return ok;
}
static int layers_case(void){
  int ok=1;
  for(int cached=0;cached<2;cached++){
    GmlVM vm={0}; GmlVal args[]={vreal(12.5),vstr("first")};
    GmlVal first=gml_builtin_call(&vm,"layer_create",args,2);
    args[1]=vstr("second"); GmlVal second=gml_builtin_call(&vm,"layer_create",args,2);
    args[0]=vreal(-20); args[1]=vstr("other"); gml_builtin_call(&vm,"layer_create",args,2);
    int ids[]={(int)first.d,(int)second.d}; GmlVal depth=vreal(12.5);
    ok &= depth_matches(query(&vm,"layer_get_id_at_depth",&depth,1,cached,&ok),ids,2);
    GmlVal move[]={first,vreal(99)}; gml_builtin_call(&vm,"layer_depth",move,2);
    ok &= depth_matches(query(&vm,"layer_get_id_at_depth",&depth,1,cached,&ok),ids+1,1);
    gml_builtin_call(&vm,"layer_destroy",&second,1);
    ok &= depth_matches(query(&vm,"layer_get_id_at_depth",&depth,1,cached,&ok),NULL,0);
    depth=vreal(NAN);
    ok &= depth_matches(query(&vm,"layer_get_id_at_depth",&depth,1,cached,&ok),NULL,0);
    gml_vm_free(&vm);
  }
  return ok;
}
static int alpha_case(void){
  GmlRender render={0}; GmlVM vm={.render=&render}; int ok=1;
  for(int enabled=1;enabled>=0;enabled--){
    GmlVal arg=vreal(enabled);
    gml_builtin_call(&vm,"draw_set_alpha_test",&arg,1);
    ok &= real_is(gml_builtin_call(&vm,"gpu_get_alphatestenable",NULL,0),enabled);
  }
  int references[]={17,128,255,0};
  for(int i=0;i<4;i++){
    GmlVal arg=vreal(references[i]);
    gml_builtin_call(&vm,"draw_set_alpha_test_ref_value",&arg,1);
    ok &= real_is(gml_builtin_call(&vm,"gpu_get_alphatestref",NULL,0),references[i]);
  }
  ok &= gml_builtin_fast_id(&vm,"draw_set_alpha_test")==-1 &&
        gml_builtin_fast_id(&vm,"draw_set_alpha_test_ref_value")==-1;
  vm.render=NULL; gml_vm_free(&vm); return ok;
}
int main(void){
  const AnygmTestCase cases[]={{"circle_precision",circle_case},{"shader_asset_names",shader_case},
    {"all_live_layers_at_depth",layers_case},{"retired_alpha_controls",alpha_case}};
  const AnygmTestGroup group={"draw_queries",cases,sizeof cases/sizeof cases[0]};
  AnygmTestResult result;
  anygm_test_run_groups(&group,1,NULL,&result);
  printf("draw queries: passed=%d failed=%d\n",result.passed,result.failed);
  return result.failed?EXIT_FAILURE:EXIT_SUCCESS;
}
