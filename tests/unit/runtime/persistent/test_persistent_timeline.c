/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "persistent_test_fixture.h"
#include "gml_builtin.h"

#include "gmlc_package.h"
#include "stdio_vfs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>


/* Re-express the compiler fixture's tagged TMLC records in the independently parsed native
 * [name,count,(step,event-pointer)*count] shape. CODE-name resolution is intentional here: native
 * packages use event graphs while compiler fixtures use direct code indices, and the runtime must
 * not require either representation when the stable timeline CODE identity is available. */
int expect_native_timeline_import(const char *path){
  GmlWin source;
  if(anygm_stdio_load_win(&source,path)) return 0;
  uint8_t *data=malloc(source.size?source.size:1);
  if(!data){ gml_win_free(&source); return 0; }
  memcpy(data,source.data,source.size);
  const GmlChunk *chunk=gml_chunk(&source,"TMLN");
  size_t end=chunk?(size_t)chunk->off+chunk->size:0;
  uint32_t count=chunk&&chunk->size>=4?fixture_u32(data,chunk->off):0;
  int valid=chunk && count>0 && (size_t)chunk->off+4+(size_t)count*4<=end;
  for(uint32_t i=0;valid && i<count;i++){
    uint32_t entry=fixture_u32(data,chunk->off+4+i*4);
    if((size_t)entry+12>end || fixture_u32(data,entry+4)!=0x434C4D54u){ valid=0; break; }
    uint32_t moments=fixture_u32(data,entry+8);
    if(moments>100000 || (size_t)entry+12+(size_t)moments*8>end){ valid=0; break; }
    fixture_w32(data,entry+4,moments);
    for(uint32_t m=0;m<moments;m++){
      fixture_w32(data,entry+8+m*8,fixture_u32(data,entry+12+m*8));
      fixture_w32(data,entry+12+m*8,0); /* CODE name, not this synthetic pointer, owns identity. */
    }
  }
  size_t size=source.size;
  gml_win_free(&source);
  if(!valid){ free(data); return 0; }
  GmlWin win;
  if(gml_win_from_mem(&win,data,size,1)) { free(data); return 0; }
  GmlVM vm;
  if(gml_vm_init(&vm,&win,NULL)){ gml_win_free(&win); return 0; }
  int ok=vm.n_timelines==1 && vm.timelines[0].name &&
    !strcmp(vm.timelines[0].name,"fixture_timeline") && vm.timelines[0].n==3 &&
    vm.timelines[0].moments[0].step==0 && vm.timelines[0].moments[0].code>=0 &&
    vm.timelines[0].moments[1].step==2 && vm.timelines[0].moments[1].code>=0 &&
    vm.timelines[0].moments[2].step==4 && vm.timelines[0].moments[2].code>=0;
  GmlVal timeline=vreal(0);
  GmlVal queried_size=gml_builtin_call(&vm,"timeline_size",&timeline,1);
  ok &= queried_size.t==V_REAL && queried_size.d==3;
  int query_id=gml_builtin_fast_id(&vm,"timeline_size");
  if(query_id<0) ok=0;
  else {
    queried_size=gml_builtin_call_fast_id(&vm,query_id,"timeline_size",&timeline,1);
    ok &= queried_size.t==V_REAL && queried_size.d==3;
  }
  GmlVal maximum=gml_builtin_call(&vm,"timeline_max_moment",&timeline,1);
  ok &= maximum.t==V_REAL && maximum.d==4;
  int maximum_id=gml_builtin_fast_id(&vm,"timeline_max_moment");
  if(maximum_id<0) ok=0;
  else {
    maximum=gml_builtin_call_fast_id(&vm,maximum_id,"timeline_max_moment",&timeline,1);
    ok &= maximum.t==V_REAL && maximum.d==4;
  }
  if(ok){
    gml_room_enter(&vm,0);
    GmlInstance *probe=find_slot(&vm,100000);
    if(!probe) ok=0;
    else {
      *gml_varmap_put(&vm.globals,"timeline_order")=vreal(0);
      probe->timeline_index=0; probe->timeline_position=0; probe->timeline_speed=1;
      probe->timeline_running=1; probe->timeline_loop=0;
      gml_vm_step(&vm); gml_vm_step(&vm); gml_vm_step(&vm);
      GmlVal *order=gml_varmap_get(&vm.globals,"timeline_order");
      ok=order && order->t==V_REAL && order->d==12;
    }
  }
  gml_builtin_call(&vm,"timeline_clear",&timeline,1);
  queried_size=gml_builtin_call(&vm,"timeline_size",&timeline,1);
  ok &= queried_size.t==V_REAL && queried_size.d==0;
  maximum=gml_builtin_call(&vm,"timeline_max_moment",&timeline,1);
  ok &= maximum.t==V_REAL && maximum.d==0; /* neutral empty-resource policy */
  if(!ok) fprintf(stderr,"native timeline import fixture failed\n");
  gml_vm_free(&vm); gml_win_free(&win);
  return ok;
}


static double timeline_global(GmlVM *vm,const char *name){
  GmlVal *value=gml_varmap_get(&vm->globals,name);
  return value?value->d:0;
}

static void timeline_start(GmlInstance *in,int index,double speed){
  in->timeline_index=index; in->timeline_position=0; in->timeline_speed=speed;
  in->timeline_running=1; in->timeline_loop=0;
}

static int expect_timeline_mutations(const char *path){
  GmlWin win={0}; GmlVM vm={0}; int ok=1,loaded=0;
  uint8_t *cold=NULL,*saved=NULL,*again=NULL; size_t cold_size=0,size=0,written=0;
#define CHECK_TL(condition) do { if(!(condition)){ \
  fprintf(stderr,"timeline mutation control failed at line %d: %s\n",__LINE__,#condition); \
  ok=0; goto done; } } while(0)
  CHECK_TL(!anygm_stdio_load_win(&win,path)); loaded=1;
  CHECK_TL(!gml_vm_init(&vm,&win,NULL));
  gml_room_enter(&vm,0);
  GmlInstance *first=find_slot(&vm,100000);
  CHECK_TL(first);
  *gml_varmap_put(&first->vars,"hits")=vreal(0);
  cold_size=gml_vm_state_size(&vm); cold=malloc(cold_size);
  CHECK_TL(cold && gml_vm_state_save(&vm,cold,cold_size,&written) && written==cold_size);
  const char *names[]={"timeline_delete","timeline_moment_clear","timeline_moment_add_script"};
  int ids[3];
  for(int i=0;i<3;i++){ ids[i]=gml_builtin_fast_id(&vm,names[i]); CHECK_TL(ids[i]>=0); }
  GmlVal dynamic=gml_builtin_call(&vm,"timeline_add",NULL,0);
  CHECK_TL(dynamic.t==V_REAL && dynamic.d==1);
  GmlVal callback=vreal(GML_FUNCVAL_TAG|gml_code_index_by_name(&win,"gml_Script_timeline_callback"));
  CHECK_TL(((int)callback.d&0x00FFFFFF)<win.n_code);
  GmlVal add[]={dynamic,vreal(0),callback};
  gml_builtin_call_fast_id(&vm,ids[2],names[2],add,3);
  gml_builtin_call(&vm,names[2],add,3);
  CHECK_TL(vm.timelines[1].n==1 && vm.timelines[1].moments[0].n_callbacks==2);
  add[1]=vreal(7); gml_builtin_call(&vm,names[2],add,3);
  CHECK_TL(vm.timelines[1].n==2 && vm.timelines[1].last_step==7);
  GmlVal clear[]={dynamic,vreal(7)};
  gml_builtin_call_fast_id(&vm,ids[1],names[1],clear,2);
  CHECK_TL(vm.timelines[1].n==2 && vm.timelines[1].last_step==7 &&
           vm.timelines[1].moments[1].n_callbacks==0);
  GmlVal invalid[]={dynamic,vreal(NAN),callback};
  gml_builtin_call(&vm,names[2],invalid,3);
  invalid[1]=vreal(0); invalid[2]=vreal(INFINITY);
  gml_builtin_call(&vm,names[2],invalid,3);
  invalid[0]=vreal(NAN); gml_builtin_call(&vm,names[0],invalid,1);
  CHECK_TL(vm.timelines[1].n==2 && vm.timelines[1].moments[0].n_callbacks==2);
  GmlInstance *second=gml_instance_create(&vm,3,4,0); CHECK_TL(second);
  *gml_varmap_put(&second->vars,"hits")=vreal(0);
  uint32_t second_id=second->id;
  GmlVal method_args[]={vreal(second->id),callback};
  GmlVal method=gml_builtin_call(&vm,"method",method_args,2);
  CHECK_TL(GML_IS_STRUCT_ID(method.d));
  add[1]=vreal(0); add[2]=method; gml_builtin_call(&vm,names[2],add,3);
  gml_struct_gc(&vm);
  CHECK_TL(!gml_struct_find(&vm,(unsigned)method.d));
  timeline_start(first,1,1); timeline_start(second,1,1);
  *gml_varmap_put(&vm.globals,"timeline_order")=vreal(0);
  gml_vm_step(&vm);
  CHECK_TL(timeline_global(&vm,"timeline_order")==111111 &&
           timeline_global(&vm,"timeline_argc")==0 &&
           timeline_global(&vm,"timeline_self")==second_id);
  CHECK_TL(gml_varmap_get(&first->vars,"hits")->d==3 &&
           gml_varmap_get(&second->vars,"hits")->d==3);
  size=gml_vm_state_size(&vm); saved=malloc(size); again=malloc(size);
  CHECK_TL(saved && again && gml_vm_state_save(&vm,saved,size,&written) && written==size);
  gml_builtin_call_fast_id(&vm,ids[0],names[0],&dynamic,1);
  CHECK_TL(!vm.timelines[1].name && vm.timelines[1].n==0);
  CHECK_TL(gml_vm_state_load(&vm,saved,size,&written) && written==size);
  CHECK_TL(gml_vm_state_save(&vm,again,size,&written) && written==size && !memcmp(saved,again,size));
  CHECK_TL(vm.timelines[1].n==2 && vm.timelines[1].moments[0].n_callbacks==3 &&
           !gml_struct_find(&vm,(unsigned)method.d));
  first=find_slot(&vm,100000); second=find_slot(&vm,second_id);
  CHECK_TL(first && second);
  second->timeline_running=0;
  GmlVal authored[]={vreal(0),vreal(2)};
  gml_builtin_call(&vm,names[1],authored,2);
  CHECK_TL(vm.timelines[0].n==3 && vm.timelines[0].moments[1].code==-1);
  CHECK_TL(vm.timelines[0].moments[1].code==-1 && vm.timelines[1].name);
  first=find_slot(&vm,100000); CHECK_TL(first);
  /* Each callback invalidates the active snapshot, including a callback deleting its own array. */
  const char *mutators[]={"timeline_delete_current","timeline_clear_current","timeline_extend_current"};
  for(int k=0;k<3;k++){
    dynamic=gml_builtin_call(&vm,"timeline_add",NULL,0);
    int index=(int)dynamic.d;
    int code=gml_code_index_by_name(&win,mutators[k]);
    if(code<0){ char name[80]; snprintf(name,sizeof name,"gml_Script_%s",mutators[k]); code=gml_code_index_by_name(&win,name); }
    CHECK_TL(code>=0);
    GmlVal actions[]={dynamic,vreal(0),vreal(GML_FUNCVAL_TAG|code)};
    gml_builtin_call(&vm,names[2],actions,3);
    actions[2]=callback; gml_builtin_call(&vm,names[2],actions,3);
    *gml_varmap_put(&vm.globals,"timeline_callback")=callback;
    *gml_varmap_put(&vm.globals,"timeline_order")=vreal(0);
    timeline_start(first,index,1); gml_vm_step(&vm);
    CHECK_TL(timeline_global(&vm,"timeline_order")==9);
    if(k==0) CHECK_TL(!vm.timelines[index].name);
    if(k==1) CHECK_TL(vm.timelines[index].n==1 && vm.timelines[index].moments[0].n_callbacks==0);
    if(k==2) CHECK_TL(vm.timelines[index].moments[0].n_callbacks==3);
  }
  CHECK_TL(gml_vm_state_load(&vm,cold,cold_size,&written) && written==cold_size);
  CHECK_TL(vm.n_timelines==1 && vm.timelines[0].n==3 && vm.timelines[0].moments[1].code>=0);
  CHECK_TL(gml_vm_state_save(&vm,cold,cold_size,&written) && written==cold_size);
  printf("timeline mutations: dispatch, shared callbacks, script context, mutation and state pass\n");
done:
  free(cold); free(saved); free(again);
  if(vm.win) gml_vm_free(&vm);
  if(loaded) gml_win_free(&win);
#undef CHECK_TL
  return ok;
}


int expect_timeline_case(void){
  AnygmHostServices services={0};
  services.struct_size=sizeof services;
  services.abi_version=ANYGM_HOST_SERVICES_VERSION;
  anygm_stdio_vfs_services_init(&services);
  GmlcProject project={0};
  GmlcObject object={0};
  GmlcRoom room={0};
  GmlcRoomInstance instance={0};
  GmlcTimeline timeline={0};
  GmlcTimelineMoment moments[3]={{0}};
  int room_order[]={0};
  GmlcScript scripts[4]={{0}};
  char script_paths[4][44]={{0}};
  const char *script_names[]={"timeline_callback","timeline_delete_current","timeline_clear_current","timeline_extend_current"};
  const char *script_sources[]={
    "global.timeline_order=global.timeline_order*10+1; global.timeline_self=id; global.timeline_argc=argument_count; hits+=1;",
    "timeline_delete(timeline_index); global.timeline_order=9;",
    "timeline_moment_clear(timeline_index,0); global.timeline_order=9;",
    "timeline_moment_add_script(timeline_index,0,global.timeline_callback); repeat(40) timeline_add(); global.timeline_order=9;"
  };
  char moment_paths[3][44];
  const char *sources[]={
    "global.timeline_order=global.timeline_order*10+1;\n",
    "global.timeline_order=global.timeline_order*10+2;\n",
    "global.timeline_order=global.timeline_order*10+3;\n",
  };
  project.host=&services;
  project.name="timeline-fixture";
  project.classic_version=800;
  project.classic_executable_layout=1;
  project.objects=&object;
  project.n_objects=project.cap_objects=1;
  project.rooms=&room;
  project.n_rooms=project.cap_rooms=1;
  project.room_order=room_order;
  project.n_room_order=1;
  project.timelines=&timeline;
  project.n_timelines=project.cap_timelines=1;
  object.name="timeline_object";
  object.sprite_id=object.mask_id=object.parent_id=-1;
  object.visible=1;
  room.name="timeline_room";
  room.width=320;
  room.height=240;
  room.speed=30;
  room.instances=&instance;
  room.n_instances=room.cap_instances=1;
  instance.id=instance.name=(char*)"timeline_instance";
  instance.object_id=0;
  instance.instance_id=100000;
  timeline.id=timeline.name=(char*)"fixture_timeline";
  timeline.moments=moments;
  timeline.n_moments=timeline.cap_moments=3;
  int ok=1;
  project.scripts=scripts; project.n_scripts=project.cap_scripts=4;
  for(int index=0;index<4;index++){
    snprintf(script_paths[index],sizeof script_paths[index],"/tmp/gml-timeline-script-%d-XXXXXX",index);
    int fd=mkstemp(script_paths[index]);
    if(fd<0){ ok=0; break; } close(fd);
    scripts[index].id=scripts[index].name=(char*)script_names[index];
    scripts[index].source_path=script_paths[index];
    if(!fixture_write_text(script_paths[index],script_sources[index])){ ok=0; break; }
  }
  for(int index=0;ok && index<3;++index){
    moments[index].step=index*2;
    snprintf(moment_paths[index],sizeof moment_paths[index],
             "/tmp/gml-timeline-case-%d-XXXXXX",index);
    int descriptor=mkstemp(moment_paths[index]);
    FILE *file=descriptor>=0?fdopen(descriptor,"wb"):NULL;
    size_t length=strlen(sources[index]);
    int wrote=file && fwrite(sources[index],1,length,file)==length;
    int closed=file?fclose(file)==0:0;
    if(!wrote || !closed){
      if(!file && descriptor>=0) close(descriptor);
      ok=0;
      break;
    }
    moments[index].source_path=moment_paths[index];
  }
  char path[]="/tmp/gml-timeline-case-package-XXXXXX";
  int descriptor=ok?mkstemp(path):-1;
  if(descriptor>=0) close(descriptor);
  char error[256]={0};
  if(descriptor<0 ||
     !gmlc_package_write_structural(&project,path,error,sizeof error) ||
     !expect_native_timeline_import(path) || !expect_timeline_mutations(path)){
    if(error[0]) fprintf(stderr,"timeline package fixture: %s\n",error);
    ok=0;
  }
  if(descriptor>=0) unlink(path);
  for(int index=0;index<4;index++) if(script_paths[index][0]) unlink(script_paths[index]);
  for(int index=0;index<3;++index)
    if(moments[index].source_path) unlink(moment_paths[index]);
  return ok;
}
