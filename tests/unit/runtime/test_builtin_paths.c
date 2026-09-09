/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "gml_builtin.h"
#include "gml_particle.h"
#include "anygm_test_runner.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int expect(int condition,const char *label){
  if(!condition) fprintf(stderr,"path contract: %s\n",label);
  return condition;
}
static GmlVal call(GmlVM *vm,const char *name,GmlVal *args,int count,int cached,int *ok){
  if(!cached) return gml_builtin_call(vm,name,args,count);
  int id=gml_builtin_fast_id(vm,name);
  if(id<0){ *ok=0; return vundef(); }
  return gml_builtin_call_fast_id(vm,id,name,args,count);
}
static GmlVal path(GmlVM *vm){
  GmlVal id=gml_builtin_call(vm,"path_add",NULL,0);
  GmlVal closed[]={id,vreal(0)};
  gml_builtin_call(vm,"path_set_closed",closed,2);
  GmlVal first[]={id,vreal(10),vreal(20),vreal(100)};
  GmlVal second[]={id,vreal(13),vreal(24),vreal(0)};
  gml_builtin_call(vm,"path_add_point",first,4);
  gml_builtin_call(vm,"path_add_point",second,4);
  return id;
}
static int copies_case(void){
  int ok=1;
  for(int cached=0;cached<2;cached++){
    GmlVM vm={0}; GmlVal source=path(&vm), destination=path(&vm);
    vm.paths[0].kind=1; vm.paths[0].precision=6;
    GmlVal duplicate=call(&vm,"path_duplicate",&source,1,cached,&ok);
    int created=duplicate.t==V_REAL && duplicate.d==2 && vm.n_paths==3;
    ok &= expect(created,"duplicate creates a distinct path ID");
    GmlVal args[]={destination,source};
    call(&vm,"path_assign",args,2,cached,&ok);
    ok &= expect(vm.paths[1].kind==1 && vm.paths[1].precision==6 &&
                 vm.paths[1].len==5 && vm.paths[1].closed==0 && vm.paths[1].runtime_dirty,
                 "assignment copies geometry and traversal properties");
    gml_builtin_call(&vm,"path_clear_points",&source,1);
    ok &= expect(vm.paths[1].n==2 && vm.paths[1].pts[1].x==13,
                 "assignment owns its points independently");
    if(created) ok &= expect(vm.paths[2].n==2 && vm.paths[2].pts!=vm.paths[1].pts &&
                             vm.paths[2].pts[1].sp==0 && vm.paths[2].precision==6,
                             "duplication owns its points and preserves speed samples");
    args[0]=args[1]=destination;
    call(&vm,"path_assign",args,2,cached,&ok);
    ok &= expect(vm.paths[1].n==2 && vm.paths[1].len==5,"self-assignment is unchanged");
    args[1]=source;
    call(&vm,"path_assign",args,2,cached,&ok);
    ok &= expect(vm.paths[1].n==0 && vm.paths[1].len==0,"empty source replaces populated destination");
    GmlVal empty=call(&vm,"path_duplicate",&source,1,cached,&ok);
    ok &= expect(empty.t==V_REAL && empty.d==3 && vm.n_paths==4 && vm.paths[3].n==0,
                 "empty duplication still creates an independent path");
    double invalid[]={-1,NAN,INFINITY,1e100};
    int before=vm.n_paths;
    for(int i=0;i<4;i++){
      GmlVal bad=vreal(invalid[i]);
      GmlVal result=call(&vm,"path_duplicate",&bad,1,cached,&ok);
      ok &= expect(result.t==V_REAL && result.d==-1 && vm.n_paths==before,
                   "invalid duplicate does not publish a path");
    }
    gml_vm_free(&vm);
  }
  return ok;
}
static int speeds_case(void){
  int ok=1;
  for(int cached=0;cached<2;cached++){
    GmlVM vm={0}; GmlVal source=path(&vm);
    const double positions[]={0,0.25,0.5,0.75,1};
    for(int i=0;i<5;i++){
      GmlVal args[]={source,vreal(positions[i])};
      GmlVal value=call(&vm,"path_get_speed",args,2,cached,&ok);
      ok &= expect(value.t==V_REAL && value.d==100*(1-positions[i]),
                   "speed interpolates by distance along the current path");
    }
    GmlVal closed[]={source,vreal(1)};
    gml_builtin_call(&vm,"path_set_closed",closed,2);
    GmlVal args[]={source,vreal(0.75)};
    GmlVal value=call(&vm,"path_get_speed",args,2,cached,&ok);
    ok &= expect(value.t==V_REAL && value.d==50,"closed return segment interpolates speed");
    args[1]=vreal(1); value=call(&vm,"path_get_speed",args,2,cached,&ok);
    ok &= expect(value.t==V_REAL && value.d==100,"closed endpoint reaches first speed");
    args[1]=vreal(NAN); value=call(&vm,"path_get_speed",args,2,cached,&ok);
    ok &= expect(value.t==V_REAL && value.d==0,"nonfinite query is rejected");
    gml_vm_free(&vm);
  }
  return ok;
}
static int restore_case(void){
  GmlVM vm={0}; vm.room_index=vm.pending_room=-1; vm.next_creation_seq=1;
  vm.particles=gml_particle_state_create(&vm);
  if(!vm.particles) return 0;
  gml_vm_software3d_reset(&vm);
  GmlVal source=path(&vm), destination=path(&vm); int ok=1;
  GmlVal duplicate=call(&vm,"path_duplicate",&source,1,0,&ok);
  GmlVal assign[]={destination,source}; call(&vm,"path_assign",assign,2,0,&ok);
  size_t size=gml_vm_state_size(&vm), written=0, used=0;
  void *bytes=malloc(size?size:1);
  ok &= expect(bytes && gml_vm_state_save(&vm,bytes,size,&written) && written==size,
               "copied paths serialize through the existing state owner");
  gml_builtin_call(&vm,"path_clear_points",&destination,1);
  if(ok) ok &= expect(gml_vm_state_load(&vm,bytes,written,&used) && used==written,
                       "same-execution path snapshot restores");
  if(ok){
    GmlVal args[]={destination,vreal(0.5)};
    GmlVal value=call(&vm,"path_get_speed",args,2,0,&ok);
    ok &= expect(vm.n_paths==3 && duplicate.t==V_REAL && duplicate.d==2 &&
                 vm.paths[1].n==2 && vm.paths[2].n==2 && vm.paths[1].pts!=vm.paths[2].pts &&
                 value.t==V_REAL && value.d==50,"restored copies remain independent and usable");
  }
  free(bytes); gml_vm_free(&vm); return ok;
}
int main(void){
  const AnygmTestCase cases[]={{"independent_copies",copies_case},{"interpolated_speed",speeds_case},
                               {"current_state_restore",restore_case}};
  const AnygmTestGroup group={"paths",cases,sizeof cases/sizeof cases[0]};
  AnygmTestResult result; anygm_test_run_groups(&group,1,NULL,&result);
  printf("path contracts: passed=%d failed=%d\n",result.passed,result.failed);
  return result.failed?EXIT_FAILURE:EXIT_SUCCESS;
}
