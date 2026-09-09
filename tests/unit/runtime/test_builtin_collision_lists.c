/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "gml_builtin.h"
#include "gml_render_internal.h"
#include <math.h>
#include <stdio.h>

int main(void){
  int ok=1;
  for(int cached=0;cached<2;cached++){
    GmlSprite sprite={.w=1,.h=1,.collision_kind=1};
    GmlRender render={.spr=&sprite,.n_spr=1};
    GmlInstance instances[4]={0};
    double positions[][2]={{10,10},{17,10},{12,10},{0,0}};
    for(int i=0;i<4;i++){
      instances[i].id=100000+i; instances[i].active=1;
      instances[i].x=positions[i][0]; instances[i].y=positions[i][1];
      instances[i].image_xscale=instances[i].image_yscale=1;
    }
    GmlVM vm={.render=&render,.inst=instances,.inst_count=4,.cur_self=&instances[0]};
    GmlVal list=gml_builtin_call(&vm,"ds_list_create",NULL,0);
    GmlVal add[]={list,vreal(777)}; gml_builtin_call(&vm,"ds_list_add",add,2);
    GmlVal args[]={vreal(0),vreal(0),vreal(20),vreal(20),vreal(-3),vreal(0),
                   vreal(1),list,vreal(1)};
    int id=gml_builtin_fast_id(&vm,"collision_ellipse_list");
    GmlVal found=cached && id>=0 ? gml_builtin_call_fast_id(&vm,id,"collision_ellipse_list",args,9)
                                 : gml_builtin_call(&vm,"collision_ellipse_list",args,9);
    ok &= id>=0 && found.t==V_REAL && found.d==2;
    double expected[]={777,100002,100001};
    for(int i=0;i<3;i++){
      GmlVal index[]={list,vreal(i)};
      GmlVal value=gml_builtin_call(&vm,"ds_list_find_value",index,2);
      ok &= value.t==V_REAL && value.d==expected[i];
    }
    args[0]=vreal(NAN);
    found=gml_builtin_call(&vm,"collision_ellipse_list",args,9);
    ok &= found.t==V_REAL && found.d==0;
    GmlVal size=gml_builtin_call(&vm,"ds_list_size",&list,1);
    ok &= size.t==V_REAL && size.d==3;
    vm.render=NULL; vm.inst=NULL; vm.inst_count=0; vm.cur_self=NULL;
    gml_vm_free(&vm);
  }
  puts(ok?"ellipse list contracts: passed":"ellipse list contracts: failed");
  return ok?0:1;
}
