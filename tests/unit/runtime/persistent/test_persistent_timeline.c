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
  if(!ok) fprintf(stderr,"native timeline import fixture failed\n");
  gml_vm_free(&vm); gml_win_free(&win);
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
  for(int index=0;index<3;++index){
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
     !expect_native_timeline_import(path)){
    if(error[0]) fprintf(stderr,"timeline package fixture: %s\n",error);
    ok=0;
  }
  if(descriptor>=0) unlink(path);
  for(int index=0;index<3;++index)
    if(moments[index].source_path) unlink(moment_paths[index]);
  return ok;
}
