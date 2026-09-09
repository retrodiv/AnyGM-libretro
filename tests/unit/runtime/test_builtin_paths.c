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
static double query(GmlVM *vm,const char *name,GmlVal id,int point){
  GmlVal args[]={id,vreal(point)};
  GmlVal value=gml_builtin_call(vm,name,args,2);
  return value.t==V_REAL?value.d:NAN;
}
static int controls_case(void){
  int ok=1;
  for(int cached=0;cached<2;cached++){
    GmlVM vm={0}; GmlVal id=path(&vm);
    GmlVal insert[]={id,vreal(1),vreal(12),vreal(30),vreal(40)};
    call(&vm,"path_insert_point",insert,5,cached,&ok);
    ok &= expect(query(&vm,"path_get_number",id,0)==3 &&
                 query(&vm,"path_get_point_y",id,1)==30 &&
                 query(&vm,"path_get_point_speed",id,1)==40,
                 "insertion precedes the indexed defining point");
    GmlVal kind[]={id,vreal(1)},precision[]={id,vreal(2)};
    call(&vm,"path_set_kind",kind,2,cached,&ok);
    call(&vm,"path_set_precision",precision,2,cached,&ok);
    ok &= expect(query(&vm,"path_get_kind",id,0)==1 &&
                 query(&vm,"path_get_precision",id,0)==2 && vm.paths[0].n>3 &&
                 query(&vm,"path_get_number",id,0)==3 &&
                 query(&vm,"path_get_point_y",id,1)==30,
                 "smoothing retains three defining points independently of samples");
    GmlVal change[]={id,vreal(1),vreal(11),vreal(25),vreal(75)};
    call(&vm,"path_change_point",change,5,cached,&ok);
    ok &= expect(query(&vm,"path_get_point_x",id,1)==11 &&
                 query(&vm,"path_get_point_speed",id,1)==75,
                 "point replacement addresses controls after smoothing");
    GmlVal remove[]={id,vreal(1)};
    call(&vm,"path_delete_point",remove,2,cached,&ok);
    kind[1]=vreal(0); call(&vm,"path_set_kind",kind,2,cached,&ok);
    ok &= expect(vm.paths[0].n==2 && query(&vm,"path_get_number",id,0)==2 &&
                 query(&vm,"path_get_length",id,0)==5 &&
                 query(&vm,"path_get_point_y",id,1)==24,
                 "deletion and straight restoration recover the original two controls");
    gml_vm_free(&vm);
  }
  return ok;
}
static int transforms_case(void){
  int ok=1;
  for(int cached=0;cached<2;cached++){
    GmlVM vm={0}; GmlVal id=path(&vm);
    GmlVal scale[]={id,vreal(2),vreal(3)};
    call(&vm,"path_rescale",scale,3,cached,&ok);
    ok &= expect(query(&vm,"path_get_point_x",id,0)==8.5 &&
                 query(&vm,"path_get_point_y",id,0)==16 &&
                 query(&vm,"path_get_point_x",id,1)==14.5 &&
                 query(&vm,"path_get_point_y",id,1)==28,
                 "rescale uses the path center, not the coordinate origin");
    call(&vm,"path_mirror",&id,1,cached,&ok);
    call(&vm,"path_flip",&id,1,cached,&ok);
    ok &= expect(query(&vm,"path_get_point_x",id,0)==14.5 &&
                 query(&vm,"path_get_point_y",id,0)==28,
                 "mirror and flip reflect about the horizontal and vertical center");
    call(&vm,"path_reverse",&id,1,cached,&ok);
    ok &= expect(query(&vm,"path_get_point_x",id,0)==8.5 &&
                 query(&vm,"path_get_point_speed",id,0)==0,
                 "reversal reverses defining points together with their speed");
    GmlVal rotate[]={id,vreal(90)};
    call(&vm,"path_rotate",rotate,2,cached,&ok);
    ok &= expect(fabs(query(&vm,"path_get_point_x",id,0)-5.5)<1e-10 &&
                 fabs(query(&vm,"path_get_point_y",id,0)-25)<1e-10,
                 "rotation is counterclockwise about the center in y-down coordinates");
    GmlVal shift[]={id,vreal(4),vreal(-7)};
    call(&vm,"path_shift",shift,3,cached,&ok);
    ok &= expect(fabs(query(&vm,"path_get_point_x",id,0)-9.5)<1e-10 &&
                 fabs(query(&vm,"path_get_point_y",id,0)-18)<1e-10,
                 "shift translates every defining point");
    double old_x=query(&vm,"path_get_point_x",id,0);
    shift[1]=vreal(INFINITY); call(&vm,"path_shift",shift,3,cached,&ok);
    ok &= expect(query(&vm,"path_get_point_x",id,0)==old_x,
                 "nonfinite mutation leaves the path unchanged");
    gml_vm_free(&vm);
  }
  return ok;
}
static int append_case(void){
  int ok=1;
  for(int cached=0;cached<2;cached++){
    GmlVM vm={0}; GmlVal destination=path(&vm),source=path(&vm);
    GmlVal args[]={destination,source};
    call(&vm,"path_append",args,2,cached,&ok);
    ok &= expect(query(&vm,"path_get_number",destination,0)==4 &&
                 query(&vm,"path_get_point_x",destination,2)==10 &&
                 query(&vm,"path_get_point_speed",destination,3)==0,
                 "append joins defining points without relocating them");
    ok &= expect(query(&vm,"path_get_number",source,0)==0 &&
                 query(&vm,"path_exists",source,0)==1,
                 "append leaves the source as an existing empty path");
    gml_vm_free(&vm);
  }
  return ok;
}
static int names_case(void){
  int ok=1;
  for(int cached=0;cached<2;cached++){
    GmlVM vm={0}; GmlVal first=path(&vm),second=path(&vm);
    GmlVal value=call(&vm,"path_get_name",&first,1,cached,&ok);
    ok &= expect(value.t==V_STR && !strcmp(value.s,"_newpath0"),
                 "first dynamic path has its generated identity");
    GmlVal args[]={second,first}; call(&vm,"path_assign",args,2,cached,&ok);
    value=call(&vm,"path_get_name",&second,1,cached,&ok);
    ok &= expect(value.t==V_STR && !strcmp(value.s,"_newpath1"),
                 "assignment preserves the destination identity");
    GmlVal third=call(&vm,"path_duplicate",&first,1,cached,&ok);
    value=call(&vm,"path_get_name",&third,1,cached,&ok);
    ok &= expect(value.t==V_STR && !strcmp(value.s,"_newpath2"),
                 "duplicate receives a distinct generated identity");
    gml_vm_free(&vm);
  }
  return ok;
}
static void put32(unsigned char *bytes,unsigned value){
  for(int i=0;i<4;i++) bytes[i]=(unsigned char)(value>>(8*i));
}
static void put_float(unsigned char *bytes,float value){
  unsigned bits; memcpy(&bits,&value,sizeof(bits)); put32(bytes,bits);
}
static int authored_case(void){
  /* One bounded PATH record, using the normalized character pointer into STRG. */
  unsigned char bytes[128]={0};
  char *strings[]={"authored_curve"}; uint32_t offsets[]={80};
  put32(bytes,1); put32(bytes+4,8); put32(bytes+8,80);
  put32(bytes+12,1); put32(bytes+16,0); put32(bytes+20,2); put32(bytes+24,3);
  const float controls[3][3]={{10,20,100},{30,50,70},{50,20,0}};
  for(int i=0;i<3;i++) for(int j=0;j<3;j++) put_float(bytes+28+12*i+4*j,controls[i][j]);
  GmlWin win={0}; win.data=bytes; win.size=sizeof bytes; win.bytecode=15;
  win.strs=strings; win.str_charoff=offsets; win.n_strs=1; win.n_chunks=1;
  memcpy(win.chunks[0].name,"PATH",5); win.chunks[0].off=0; win.chunks[0].size=64;
  GmlVM vm={0};
  if(gml_vm_init(&vm,&win,NULL)) return 0;
  int ok=expect(vm.n_paths==1 && vm.n_authored_paths==1 && vm.paths[0].n==13,
                "authored smooth path keeps the established sample subdivision");
  GmlVal id=vreal(0),name=gml_builtin_call(&vm,"path_get_name",&id,1);
  ok &= expect(name.t==V_STR && !strcmp(name.s,"authored_curve") &&
               query(&vm,"path_get_number",id,0)==3 &&
               query(&vm,"path_get_point_y",id,1)==50,
               "authored identity and defining controls survive parsing");
  GmlPathPt before[13];
  if(ok) memcpy(before,vm.paths[0].pts,sizeof before);
  GmlVal change[]={id,vreal(1),vreal(35),vreal(60),vreal(80)};
  gml_builtin_call(&vm,"path_change_point",change,5);
  change[2]=vreal(30); change[3]=vreal(50); change[4]=vreal(70);
  gml_builtin_call(&vm,"path_change_point",change,5);
  if(ok) ok &= expect(vm.paths[0].n==13 && !memcmp(before,vm.paths[0].pts,sizeof before),
                       "restoring the same controls reproduces every original sample bit");
  GmlVal duplicate=gml_builtin_call(&vm,"path_duplicate",&id,1);
  name=gml_builtin_call(&vm,"path_get_name",&duplicate,1);
  ok &= expect(name.t==V_STR && !strcmp(name.s,"_newpath0"),
               "dynamic naming counts creations rather than authored resources");
  GmlVal lookup=gml_builtin_call(&vm,"asset_get_index",&name,1);
  ok &= expect(lookup.t==V_REAL && lookup.d==duplicate.d,
               "a runtime path name resolves back to its resource");
  gml_vm_free(&vm); free(win.str_hix);
  return ok;
}
static int edited_restore_case(void){
  GmlVM vm={0}; vm.room_index=vm.pending_room=-1; vm.next_creation_seq=1;
  vm.particles=gml_particle_state_create(&vm);
  if(!vm.particles) return 0;
  gml_vm_software3d_reset(&vm);
  GmlVal id=path(&vm),removed=path(&vm);
  GmlVal third[]={id,vreal(17),vreal(20),vreal(100)};
  gml_builtin_call(&vm,"path_add_point",third,4);
  GmlVal smooth[]={id,vreal(1)};
  gml_builtin_call(&vm,"path_set_kind",smooth,2);
  vm.inst=calloc(2,sizeof(*vm.inst));
  if(!vm.inst){ gml_vm_free(&vm); return 0; }
  vm.inst_count=vm.inst_cap=2;
  for(int i=0;i<2;i++){
    vm.inst[i].active=1; vm.inst[i].id=100000u+(unsigned)i; vm.inst[i].creation_seq=1u+(unsigned)i;
    vm.inst[i].obj=-1; vm.inst[i].x=100; vm.inst[i].y=200;
    gml_path_start(&vm,&vm.inst[i],(int)id.d,2,0,i);
  }
  vm.next_creation_seq=3;
  GmlVal shift[]={id,vreal(20),vreal(-10)};
  gml_builtin_call(&vm,"path_shift",shift,3);
  int ok=expect(vm.inst[0].path_origin_x==30 && vm.inst[0].path_origin_y==10 &&
                vm.inst[1].path_origin_x==0 && vm.inst[1].path_origin_y==0,
                "translation moves only the relative follower's local pivot");
  gml_builtin_call(&vm,"path_delete",&removed,1);
  ok &= expect(query(&vm,"path_exists",removed,0)==0,"deleted path is no longer a live resource");
  size_t size=gml_vm_state_size(&vm),written=0,used=0;
  unsigned char *bytes=malloc(size?size:1),*after=malloc(size?size:1);
  ok &= expect(bytes && after && gml_vm_state_save(&vm,bytes,size,&written) && written==size,
               "edited controls, tombstones and placement mode serialize");
  gml_builtin_call(&vm,"path_clear_points",&id,1);
  gml_builtin_call(&vm,"path_add",NULL,0);
  if(ok) ok &= expect(gml_vm_state_load(&vm,bytes,size,&used) && used==size &&
                       gml_vm_state_save(&vm,after,size,&written) && written==size &&
                       !memcmp(bytes,after,size),"edited path state roundtrips byte for byte");
  if(ok){
    GmlVal point[]={id,vreal(1),vreal(45),vreal(16),vreal(33)};
    gml_builtin_call(&vm,"path_change_point",point,5);
    ok &= expect(query(&vm,"path_get_point_x",id,1)==45 &&
                 query(&vm,"path_get_point_speed",id,1)==33 && vm.paths[0].n>3,
                 "restored smooth controls remain editable");
    shift[1]=vreal(3); shift[2]=vreal(4);
    gml_builtin_call(&vm,"path_shift",shift,3);
    ok &= expect(vm.inst[0].path_relative && !vm.inst[1].path_relative &&
                 vm.inst[0].path_origin_x==33 && vm.inst[0].path_origin_y==14 &&
                 vm.inst[1].path_origin_x==0,
                 "restored translation preserves absolute versus relative placement");
    GmlVal next=gml_builtin_call(&vm,"path_add",NULL,0);
    GmlVal name=gml_builtin_call(&vm,"path_get_name",&next,1);
    ok &= expect(next.t==V_REAL && next.d==2 && name.t==V_STR && !strcmp(name.s,"_newpath2"),
                 "restored creation ordinal includes deleted IDs but not future allocations");
  }
  free(bytes); free(after); gml_vm_free(&vm); return ok;
}
int main(void){
  const AnygmTestCase cases[]={{"independent_copies",copies_case},{"interpolated_speed",speeds_case},
                               {"current_state_restore",restore_case},
                               {"defining_point_edits",controls_case},
                               {"centered_transforms",transforms_case},
                               {"append_transfer",append_case},{"resource_names",names_case},
                               {"authored_controls",authored_case},
                               {"edited_state_restore",edited_restore_case}};
  const AnygmTestGroup group={"paths",cases,sizeof cases/sizeof cases[0]};
  AnygmTestResult result; anygm_test_run_groups(&group,1,NULL,&result);
  printf("path contracts: passed=%d failed=%d\n",result.passed,result.failed);
  return result.failed?EXIT_FAILURE:EXIT_SUCCESS;
}
