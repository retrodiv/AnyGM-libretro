/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me> */
#include "gml_vm.h"
#include "gmlc/gmlc_package.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int gml_input_key(int key,int edge){ (void)key;(void)edge;return 0; }
int gml_input_gamepad(int button,int edge){ (void)button;(void)edge;return 0; }

static GmlInstance *find_slot(GmlVM *vm,uint32_t id){
  for(int i=0;i<vm->inst_count;i++) if(vm->inst[i].id==id) return &vm->inst[i];
  return NULL;
}

int main(void){
  GmlcProject project; GmlcObject object; GmlcRoom rooms[2];
  memset(&project,0,sizeof(project)); memset(&object,0,sizeof(object)); memset(rooms,0,sizeof(rooms));
  project.name="persistent-room-fixture"; project.objects=&object; project.n_objects=1;
  project.rooms=rooms; project.n_rooms=2;
  object.name="obj_fixture"; object.sprite_id=-1; object.mask_id=-1; object.parent_id=-1; object.visible=1;
  for(int i=0;i<2;i++){ rooms[i].name=i?"room_b":"room_a"; rooms[i].width=320; rooms[i].height=240; rooms[i].speed=30; }
  rooms[0].persistent=1;
  char path[]="/tmp/gml-persistent-room-XXXXXX"; int fd=mkstemp(path); if(fd<0)return 1; close(fd);
  char err[256]={0};
  if(!gmlc_package_write_structural(&project,path,err,sizeof(err))){ fprintf(stderr,"package: %s\n",err); unlink(path); return 1; }
  GmlWin win; if(gml_win_load(&win,path)){ unlink(path); return 1; }
  GmlVM vm; if(gml_vm_init(&vm,&win)){ gml_win_free(&win); unlink(path); return 1; }
  gml_room_enter(&vm,0);
  GmlInstance *created=gml_instance_create(&vm,12,34,0); if(!created)return 1;
  uint32_t id=created->id; *gml_varmap_put(&created->vars,"value")=vreal(42);
  gml_room_enter(&vm,1);
  GmlInstance *slot=find_slot(&vm,id);
  if(!slot||!slot->room_dormant||slot->active){ fprintf(stderr,"room was not stored\n"); return 1; }
  size_t size=gml_vm_state_size(&vm),written=0,used=0; void *state=malloc(size);
  if(!state||!gml_vm_state_save(&vm,state,size,&written)||written!=size)return 1;
  gml_room_enter(&vm,0); slot=find_slot(&vm,id);
  if(!slot||!slot->active||slot->room_dormant)return 1;
  *gml_varmap_put(&slot->vars,"value")=vreal(99);
  if(!gml_vm_state_load(&vm,state,written,&used)||used!=written)return 1;
  gml_room_enter(&vm,0); slot=find_slot(&vm,id);
  GmlVal *value=slot?gml_varmap_get(&slot->vars,"value"):NULL;
  int ok=slot&&slot->active&&!slot->room_dormant&&value&&value->t==V_REAL&&value->d==42;
  if(!ok) fprintf(stderr,"persistent room state did not roundtrip\n");
  free(state); gml_vm_free(&vm); gml_win_free(&win); unlink(path);
  if(ok) puts("persistent room fixtures: ok");
  return ok?0:1;
}
