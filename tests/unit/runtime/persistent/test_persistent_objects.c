/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "persistent_test_fixture.h"
#include "gml_builtin.h"
#include "gml_particle.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int objects_fixture(GmlVM *vm,GmlWin *win){
  memset(vm,0,sizeof *vm);
  vm->win=win; vm->room_index=vm->pending_room=-1;
  vm->next_id=100000; vm->next_creation_seq=1;
  vm->inst_cap=16; vm->inst=calloc(16,sizeof *vm->inst);
  vm->n_objects=3; vm->objects=calloc(3,sizeof *vm->objects);
  vm->obj_alive=calloc(3,sizeof *vm->obj_alive);
  vm->particles=gml_particle_state_create(vm);
  gml_vm_software3d_reset(vm);
  if(!vm->inst || !vm->objects || !vm->obj_alive || !vm->particles) return 0;
  for(int i=0;i<3;i++) vm->objects[i]=(GmlObject){
    .name=i==0?"obj_root":i==1?"obj_child":"obj_other",
    .sprite_index=-1,.mask_index=-1,.parent=-100,.depth=31+i,.visible=1
  };
  return 1;
}

static GmlVal object_call(GmlVM *vm,const char *name,double object,double value,
                          int count,int cached,int *ok){
  GmlVal arguments[]={vreal(object),vreal(value)};
  int id=gml_builtin_fast_id(vm,name);
  if(strstr(name,"depth")){
    if(id>=0){ fputs("retired object depth must retain script lookup\n",stderr); *ok=0; }
  } else if(cached && id<0){
    fprintf(stderr,"object operation has no cached ID: %s\n",name); *ok=0;
  }
  return cached && id>=0?gml_builtin_call_fast_id(vm,id,name,arguments,count):
                         gml_builtin_call(vm,name,arguments,count);
}

static int object_expect(int condition,const char *message){
  if(!condition) fprintf(stderr,"object defaults: %s\n",message);
  return condition;
}

int expect_object_defaults(void){
  int ok=1;
  for(int cached=0;cached<2;cached++){
    GmlWin win={0}; GmlVM vm={0};
    if(!objects_fixture(&vm,&win)){ gml_vm_free(&vm); return 0; }
    GmlInstance *before=gml_instance_create(&vm,10,20,1);
    GmlVal depth=object_call(&vm,"object_get_depth",1,0,1,cached,&ok);
    ok &= object_expect(depth.t==V_REAL && depth.d==32,"getter reads authored depth");
    object_call(&vm,"object_set_sprite",1,4,2,cached,&ok);
    object_call(&vm,"object_set_mask",1,7,2,cached,&ok);
    object_call(&vm,"object_set_solid",1,1,2,cached,&ok);
    object_call(&vm,"object_set_persistent",1,1,2,cached,&ok);
    object_call(&vm,"object_set_depth",1,-42,2,cached,&ok);
    GmlInstance *after=gml_instance_create(&vm,30,40,1);
    GmlInstance *override=gml_instance_create_depth(&vm,50,60,1,1,19);
    ok &= object_expect(before && before->sprite_index==-1 && before->mask_index==-1 &&
      before->solid==0 && before->persistent==0 && before->depth==32,
      "setters must not change existing instances");
    ok &= object_expect(after && after->sprite_index==4 && after->mask_index==7 &&
      after->solid==1 && after->persistent==1 && after->depth==-42 && after->visible==1,
      "new instances inherit the changed defaults");
    ok &= object_expect(override && override->depth==19 && override->solid==1,
      "explicit creation depth overrides only the depth default");
    ok &= object_expect(vm.objects[0].depth==31 && vm.objects[0].sprite_index==-1 &&
      vm.objects[2].persistent==0,"other objects retain their own defaults");
    object_call(&vm,"object_set_sprite",1,-1,2,cached,&ok);
    object_call(&vm,"object_set_mask",1,-1,2,cached,&ok);
    object_call(&vm,"object_set_solid",1,0,2,cached,&ok);
    object_call(&vm,"object_set_persistent",1,0,2,cached,&ok);
    GmlInstance *cleared=gml_instance_create(&vm,0,0,1);
    ok &= object_expect(cleared && cleared->sprite_index==-1 && cleared->mask_index==-1 &&
      cleared->solid==0 && cleared->persistent==0 && cleared->depth==-42,
      "removal sentinels and false defaults take effect on later instances");
    gml_vm_free(&vm);
  }
  return ok;
}

/* The state contract, unlike a language setter, must preserve every retained word.
 * Directly authored synthetic values isolate missing transport from missing builtins. */
static int object_state_case(int fresh){
  GmlWin win={0}; GmlVM source={0},target={0};
  int ok=objects_fixture(&source,&win) && objects_fixture(&target,&win);
  uint8_t *bytes=NULL,*again=NULL;
  if(!ok) goto done;
  source.objects[1].sprite_index=4; source.objects[1].mask_index=7;
  source.objects[1].depth=-42; source.objects[1].solid=1;
  source.objects[1].persistent=1;
  GmlVal visibility[]={vreal(1),vreal(0)};
  gml_builtin_call(&source,"object_set_visible",visibility,2);
  ok=gml_object_set_parent(&source,1,0);
  GmlInstance *saved=gml_instance_create(&source,10,20,1);
  uint32_t saved_id=saved?saved->id:0;
  size_t size=gml_vm_state_size(&source),written=0,used=0;
  bytes=malloc(size?size:1); again=malloc(size?size:1);
  ok=ok && saved && bytes && again && gml_vm_state_save(&source,bytes,size,&written) &&
     written==size;
  GmlVM *destination=fresh?&target:&source;
  if(!ok) goto done;
  destination->objects[1].depth=900;
  destination->objects[1].sprite_index=8; destination->objects[1].mask_index=9;
  destination->objects[1].visible=1;
  destination->objects[1].solid=destination->objects[1].persistent=0;
  ok=gml_object_set_parent(destination,1,2);
  /* Populate family caches under the hierarchy which must be replaced. */
  (void)gml_instance_number(destination,2);
  ok=ok && gml_vm_state_load(destination,bytes,size,&used) && used==size;
  GmlObject *object=&destination->objects[1];
  ok &= object_expect(object->sprite_index==4 && object->mask_index==7 && object->depth==-42 &&
    object->visible==0 && object->solid==1 && object->persistent==1 && object->parent==0,
    fresh?"fresh VM restores all object properties":"rewind restores all object properties");
  ok &= object_expect(gml_object_is(destination,1,0) && !gml_object_is(destination,1,2) &&
    gml_instance_number(destination,0)==1 && gml_instance_number(destination,2)==0,
    "restoration rebuilds inherited family counts");
  GmlInstance *restored=find_slot(destination,saved_id);
  ok &= object_expect(restored && restored->sprite_index==4 && restored->depth==-42,
    "the already saved instance also survives");
  ok &= object_expect(gml_vm_state_size(destination)==size &&
    gml_vm_state_save(destination,again,size,&written) && written==size &&
    !memcmp(bytes,again,size),"object state reserializes byte-identically");
  GmlInstance *created=gml_instance_create(destination,30,40,1);
  ok &= object_expect(created && created->sprite_index==4 && created->mask_index==7 &&
    created->depth==-42 && created->visible==0 && created->solid==1 && created->persistent==1,
    "new instances after load use restored defaults");
done:
  free(bytes); free(again); gml_vm_free(&source); gml_vm_free(&target);
  return ok;
}

int expect_object_state_rewind(void){ return object_state_case(0); }
int expect_object_state_fresh(void){ return object_state_case(1); }
