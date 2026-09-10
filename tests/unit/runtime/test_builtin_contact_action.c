/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* A contact action adapts arguments without owning the movement algorithm. */
#include "gml_builtin.h"
#include "gml_render_internal.h"
#include "anygm_test_runner.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  GmlWin win;
  GmlSprite sprite;
  GmlRender render;
  GmlInstance instances[3];
  GmlVM vm;
} Fixture;

static Fixture *fixture(int classic){
  Fixture *f=calloc(1,sizeof(*f));
  if(!f) return NULL;
  f->win.classic_version=classic?800:0; f->win.bytecode=16;
  f->sprite=(GmlSprite){.w=2,.h=2,.mr=1,.mb=1,.n_frames=1,.collision_kind=1};
  f->render.win=&f->win; f->render.spr=&f->sprite; f->render.n_spr=1;
  for(int i=0;i<3;i++){
    f->instances[i].id=100000+i; f->instances[i].active=1;
    f->instances[i].mask_index=-1;
    f->instances[i].image_xscale=f->instances[i].image_yscale=1;
    f->instances[i].x=i*5;
  }
  f->instances[2].solid=1;
  f->instances[0].speed=7; f->instances[0].direction=23;
  f->instances[0].hspeed=3; f->instances[0].vspeed=4;
  f->vm.win=&f->win; f->vm.render=&f->render;
  f->vm.inst=f->instances; f->vm.inst_count=3; f->vm.cur_self=&f->instances[0];
  gml_colgrid_invalidate(&f->vm);
  return f;
}
static void release(Fixture *f){
  f->vm.win=NULL; f->vm.render=NULL; f->vm.inst=NULL;
  f->vm.inst_count=0; f->vm.cur_self=NULL;
  gml_vm_free(&f->vm); free(f);
}
static int expect(int condition,const char *message){
  if(!condition) fprintf(stderr,"%s\n",message);
  return condition;
}
static GmlVal action(Fixture *f,GmlVal *args,int count,int cached,int *ok){
  const char *name="action_move_contact";
  if(!cached) return gml_builtin_call(&f->vm,name,args,count);
  int id=gml_builtin_fast_id(&f->vm,name);
  *ok &= expect(id>=0,"contact action has an exact cached identity");
  return id>=0?gml_builtin_call_fast_id(&f->vm,id,name,args,count):vundef();
}
static int selection_case(void){
  int ok=1;
  for(int classic=0;classic<2;classic++) for(int cached=0;cached<2;cached++){
    Fixture *f=fixture(classic); if(!f) return 0;
    const double selectors[]={0,1,-1,0.5,0.6};
    for(int i=0;i<5;i++){
      f->instances[0].x=0; gml_colgrid_invalidate(&f->vm);
      GmlVal args[]={vreal(0),vreal(20),vreal(selectors[i])};
      action(f,args,3,cached,&ok);
      ok &= expect(f->instances[0].x==(selectors[i]>0.5?3:8),"selector stops before the appropriate obstacle");
      ok &= expect(f->instances[0].y==0 && f->instances[0].speed==7 &&
                   f->instances[0].direction==23 && f->instances[0].hspeed==3 &&
                   f->instances[0].vspeed==4,"contact preserves the stored motion vector");
    }
    release(f);
  }
  return ok;
}
static int distance_case(void){
  int ok=1;
  for(int classic=0;classic<2;classic++) for(int cached=0;cached<2;cached++){
    Fixture *f=fixture(classic); if(!f) return 0;
    f->instances[1].active=f->instances[2].active=0;
    const double distances[]={2,0,-2,4.6};
    for(int i=0;i<4;i++){
      f->instances[0].x=0; gml_colgrid_invalidate(&f->vm);
      GmlVal args[]={vreal(0),vreal(distances[i]),vreal(1)};
      action(f,args,3,cached,&ok);
      double wanted=i==3?(classic?5:4):(i==0?2:1000);
      ok &= expect(f->instances[0].x==wanted,"action retains the existing generation-specific distance policy");
    }
    release(f);
  }
  return ok;
}
static int parity_case(void){
  int ok=1;
  for(int classic=0;classic<2;classic++) for(int cached=0;cached<2;cached++) for(int all=0;all<2;all++){
    Fixture *f=fixture(classic),*control=fixture(classic);
    if(!f||!control){ if(f) release(f); if(control) release(control); return 0; }
    f->instances[0].x=control->instances[0].x=4;
    f->instances[1].solid=control->instances[1].solid=1;
    GmlVal args[]={vreal(0),vreal(20),vreal(all)};
    gml_colgrid_invalidate(&f->vm); gml_colgrid_invalidate(&control->vm);
    action(f,args,3,cached,&ok);
    gml_builtin_call(&control->vm,all?"move_contact":"move_contact_solid",args,2);
    ok &= expect(f->instances[0].x==control->instances[0].x &&
                 f->instances[0].y==control->instances[0].y,"action and existing contact share overlap recovery policy");
    release(f); release(control);
  }
  return ok;
}
static int relative_case(void){
  int ok=1;
  for(int cached=0;cached<2;cached++){
    Fixture *f=fixture(1); if(!f) return 0;
    f->vm.action_relative=1; f->instances[0].x=20; f->instances[0].y=20;
    GmlVal args[]={vreal(90),vreal(2),vreal(0)};
    action(f,args,3,cached,&ok);
    ok &= expect(fabs(f->instances[0].x-20)<1e-12 && f->instances[0].y==18 &&
                 f->vm.action_relative==1,"relative mode neither offsets nor consumes contact direction");
    release(f);
  }
  return ok;
}
static int bounds_case(void){
  int ok=1;
  for(int cached=0;cached<2;cached++){
    Fixture *f=fixture(1),*other=fixture(1);
    if(!f||!other){if(f) release(f);if(other) release(other);return 0;}
    GmlVal args[]={vreal(0),vreal(20),vreal(1)};
    for(int count=0;count<3;count++) action(f,args,count,cached,&ok);
    const double invalid[]={NAN,INFINITY,-INFINITY,2147483648.0};
    for(int i=0;i<4;i++){args[1]=vreal(invalid[i]);action(f,args,3,cached,&ok);}
    args[1]=vreal(20);args[0]=vreal(NAN);action(f,args,3,cached,&ok);
    ok &= expect(f->instances[0].x==0,"invalid numeric or missing arguments are defensive no-ops");
    args[0]=vreal(0);f->vm.cur_self=NULL;action(f,args,3,cached,&ok);
    f->vm.cur_self=&f->instances[0];action(f,args,3,cached,&ok);
    ok &= expect(f->instances[0].x==3 && other->instances[0].x==0,"contact acts only on the current engine and instance");
    release(f);release(other);
  }
  return ok;
}
int main(int argc,char **argv){
  const AnygmTestCase cases[]={{"selection",selection_case},{"distance",distance_case},
    {"overlap_parity",parity_case},{"relative",relative_case},{"bounds_isolation",bounds_case}};
  const AnygmTestGroup group={"contact_action",cases,sizeof cases/sizeof cases[0]};
  AnygmTestResult result={0};
  const char *filter=argc==3&&!strcmp(argv[1],"--case")?argv[2]:NULL;
  int ok=anygm_test_run_groups(&group,1,filter,&result);
  printf("contact action: %d passed, %d failed\n",result.passed,result.failed);
  return ok?0:1;
}
