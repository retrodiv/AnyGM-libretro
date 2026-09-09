/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "persistent_test_fixture.h"
#include "gml_builtin.h"
#include "gml_particle.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int objects_fixture(GmlVM *vm,GmlWin *win){
  memset(vm,0,sizeof *vm);
  vm->win=win; vm->room_index=vm->pending_room=-1;
  vm->next_id=100000; vm->next_creation_seq=1;
  vm->inst_cap=16; vm->inst=calloc(16,sizeof *vm->inst);
  vm->objects=calloc(3,sizeof *vm->objects);
  vm->obj_alive=calloc(3,sizeof *vm->obj_alive);
  vm->particles=gml_particle_state_create(vm);
  gml_vm_software3d_reset(vm);
  if(!vm->inst || !vm->objects || !vm->obj_alive || !vm->particles) return 0;
  vm->n_objects=3;
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
    GmlRtLayer *layer=gml_rt_layer_new(&vm);
    if(layer){ layer->depth=73; layer->order=5; }
    GmlInstance *layered=layer?gml_instance_create_layer(&vm,70,80,1,layer->id):NULL;
    ok &= object_expect(before && before->sprite_index==-1 && before->mask_index==-1 &&
      before->solid==0 && before->persistent==0 && before->depth==32,
      "setters must not change existing instances");
    ok &= object_expect(after && after->sprite_index==4 && after->mask_index==7 &&
      after->solid==1 && after->persistent==1 && after->depth==-42 && after->visible==1,
      "new instances inherit the changed defaults");
    ok &= object_expect(override && override->depth==19 && override->solid==1,
      "explicit creation depth overrides only the depth default");
    ok &= object_expect(layered && layered->depth==73 && layered->draw_layer_order==5 &&
      layered->sprite_index==4 && layered->mask_index==7 && layered->solid==1 &&
      layered->persistent==1 && vm.objects[1].depth==-42,
      "layer depth and order override instance placement without changing asset defaults");
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

/* These are selected defensive/coercion policies, not undocumented error
 * messages or original-runner crashes promoted into language expectations. */
int expect_object_argument_bounds(void){
  const char *setters[]={"object_set_sprite","object_set_mask","object_set_solid",
                        "object_set_persistent","object_set_depth"};
  const double invalid[]={-1,-.25,3,1e100,NAN,INFINITY,-INFINITY};
  int ok=1;
  for(int cached=0;cached<2;cached++){
    GmlWin win={0}; GmlVM vm={0};
    if(!objects_fixture(&vm,&win)){ gml_vm_free(&vm); return 0; }
    GmlObject original[3]; memcpy(original,vm.objects,sizeof original);
    for(unsigned i=0;i<sizeof setters/sizeof setters[0];i++){
      object_call(&vm,setters[i],1,0,0,cached,&ok);
      object_call(&vm,setters[i],1,0,1,cached,&ok);
      for(unsigned j=0;j<sizeof invalid/sizeof invalid[0];j++)
        object_call(&vm,setters[i],invalid[j],1,2,cached,&ok);
      object_call(&vm,setters[i],1,NAN,2,cached,&ok);
      object_call(&vm,setters[i],1,INFINITY,2,cached,&ok);
      object_call(&vm,setters[i],1,-INFINITY,2,cached,&ok);
    }
    for(unsigned i=0;i<sizeof invalid/sizeof invalid[0];i++){
      GmlVal result=object_call(&vm,"object_get_depth",invalid[i],0,1,cached,&ok);
      ok &= object_expect(result.t==V_REAL && result.d==0,"invalid depth query is bounded");
    }
    for(int i=0;i<3;i++){
      const char *name=i==0?setters[0]:i==1?setters[1]:setters[4];
      object_call(&vm,name,1,(double)INT_MAX+1,2,cached,&ok);
      object_call(&vm,name,1,(double)INT_MIN-1,2,cached,&ok);
    }
    ok &= object_expect(!memcmp(original,vm.objects,sizeof original),
      "missing, invalid and nonfinite inputs do not mutate any object");
    object_call(&vm,"object_set_depth",1,INT_MIN,2,cached,&ok);
    ok &= object_expect(vm.objects[1].depth==INT_MIN,"minimum signed depth remains representable");
    object_call(&vm,"object_set_depth",1,INT_MAX,2,cached,&ok);
    ok &= object_expect(vm.objects[1].depth==INT_MAX,"maximum signed depth remains representable");
    object_call(&vm,"object_set_depth",1,-3.75,2,cached,&ok);
    ok &= object_expect(vm.objects[1].depth==-3,"fractional depth uses selected truncation policy");
    for(int classic=0;classic<2;classic++){
      win.classic_version=classic?800:0;
      for(int i=2;i<4;i++){
        object_call(&vm,setters[i],1,.5,2,cached,&ok);
        int value=i==2?vm.objects[1].solid:vm.objects[1].persistent;
        ok &= object_expect(value==classic,"half-boolean follows the selected generation policy");
        object_call(&vm,setters[i],1,-1,2,cached,&ok);
        value=i==2?vm.objects[1].solid:vm.objects[1].persistent;
        ok &= object_expect(value==0,"negative numeric booleans are false");
      }
    }
    gml_vm_free(&vm);
  }
  return ok;
}

int expect_object_state_bounds(void){
  GmlWin win={0}; GmlVM vm={0};
  if(!objects_fixture(&vm,&win)){ gml_vm_free(&vm); return 0; }
  size_t size=gml_vm_state_size(&vm),written=0,used=0;
  uint8_t *bytes=malloc(size?size:1),*candidate=malloc(size?size:1);
  int ok=bytes && candidate && size>=88 &&
    gml_vm_state_save(&vm,bytes,size,&written) && written==size;
  GmlObject original[3]; memcpy(original,vm.objects,sizeof original);
  if(ok){
    size_t table=size-88;
    ok=fixture_u32(bytes,table)==3;
    const struct { size_t offset; uint32_t value; const char *label; } changes[]={
      {table,UINT32_MAX,"negative object count"},
      {table,2,"missing object record"},
      {table,4,"object count exceeds loaded content"},
      {table+12,0,"self parent"},
      {table+12,3,"parent beyond loaded content"},
    };
    for(unsigned i=0;i<sizeof changes/sizeof changes[0] && ok;i++){
      memcpy(candidate,bytes,size);
      fixture_w32(candidate,changes[i].offset,changes[i].value);
      ok=object_expect(!gml_vm_state_load(&vm,candidate,size,&used) &&
        !memcmp(original,vm.objects,sizeof original),changes[i].label);
    }
    if(ok){
      memcpy(candidate,bytes,size);
      fixture_w32(candidate,table+12,1);
      fixture_w32(candidate,table+40,2);
      fixture_w32(candidate,table+68,0);
      ok=object_expect(!gml_vm_state_load(&vm,candidate,size,&used) &&
        !memcmp(original,vm.objects,sizeof original),"longer parent cycle cannot publish");
    }
    if(ok) ok=object_expect(!gml_vm_state_load(&vm,bytes,size-1,&used) &&
      !memcmp(original,vm.objects,sizeof original),"truncated property record cannot publish");
    if(ok){
      memcpy(candidate,bytes,size);
      fixture_w32(candidate,4,10);
      ok=object_expect(!gml_vm_state_load(&vm,candidate,size,&used),"previous VM schema is rejected");
    }
    /* A forward edge is valid; an ordered setter replay must not reject it
     * merely because the previous live hierarchy points in the opposite direction. */
    if(ok){
      ok=object_expect(gml_object_set_parent(&vm,2,0),"opposite live hierarchy is valid");
      memcpy(candidate,bytes,size);
      fixture_w32(candidate,table+12,2);
      ok=ok && object_expect(gml_vm_state_load(&vm,candidate,size,&used) && used==size &&
        gml_object_is(&vm,0,2) && !gml_object_is(&vm,2,0),"acyclic forward parent restores");
    }
  }
  free(bytes); free(candidate); gml_vm_free(&vm);
  return ok;
}
