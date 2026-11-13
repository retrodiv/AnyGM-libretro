/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "gml_builtin.h"
#include "gml_builtin_internal.h"
#include "gml_bytecode.h"
#include "gml_vm_internal.h"
#include "gml_value_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(test) do { if(!(test)){ \
  fprintf(stderr,"instance copy: %s at line %d\n",#test,__LINE__); ok=0; goto done; \
} } while(0)

static uint64_t copy_test_u64(const unsigned char *bytes){
  uint64_t value=0;
  for(unsigned i=0;i<8;i++) value|=(uint64_t)bytes[i]<<(8*i);
  return value;
}

static int copy_storage_and_identity(int modern,int cached){
  GmlWin content={0};
  content.bytecode=modern?17:16;
  content.classic_version=modern?0:800;
  GmlVM vm;
  if(gml_vm_init(&vm,&content,NULL)) return 0;
  int ok=1;
  vm.n_objects=1;
  vm.objects=calloc(1,sizeof(*vm.objects));
  REQUIRE(vm.objects!=NULL);
  vm.objects[0].parent=-1;
  vm.objects[0].sprite_index=-1;
  vm.objects[0].mask_index=-1;
  gml_obj_alive_recount(&vm);
  GmlInstance *source=gml_instance_create(&vm,11,22,0);
  REQUIRE(source!=NULL);
  vm.cur_self=source;
  vm.cur_other=source;
  source->alarm[0]=7;
  source->depth=-25;
  source->image_angle=37;
  source->path_index=3;
  source->path_xoff=91;
  source->timeline_index=2;
  source->timeline_running=1;
  *gml_varmap_put_owned(&source->vars,strdup("owned_key"))=vstr("alpha");
  *gml_varmap_put(&source->vars,"self_reference")=vreal(source->id);
  GmlVal array=gml_arr_new(2,vreal(31));
  GmlVal nested=gml_arr_new(1,vreal(45));
  gml_arr_set(array,1,nested);
  *gml_varmap_put(&source->vars,"vector")=array;
  GmlVal argument=vreal(0);
  int fast=gml_builtin_fast_id(&vm,"instance_copy");
  REQUIRE(fast>=0);
  GmlVal result=cached?gml_builtin_call_fast_id(&vm,fast,"instance_copy",&argument,1)
                      :gml_builtin_call(&vm,"instance_copy",&argument,1);
  REQUIRE(result.t==V_REAL && result.d>=(double)source->id+1);
  GmlInstance *copy=NULL;
  for(int i=0;i<vm.inst_count;i++) if(vm.inst[i].id==(uint32_t)result.d) copy=&vm.inst[i];
  REQUIRE(copy && copy!=source && copy->active && !copy->marked);
  REQUIRE(copy->creation_seq!=source->creation_seq && vm.obj_alive[0]==2);
  REQUIRE(copy->x==11 && copy->y==22 && copy->depth==-25 && copy->image_angle==37);
  REQUIRE(copy->alarm[0]==7 && copy->path_index==3 && copy->path_xoff==91);
  REQUIRE(copy->timeline_index==2 && copy->timeline_running==1);
  REQUIRE(vm.cur_self==source && vm.cur_other==source);
  REQUIRE(gml_varmap_get(&copy->vars,"self_reference")->d==source->id);
  REQUIRE(!strcmp(gml_varmap_get(&copy->vars,"owned_key")->s,"alpha"));
  GmlVal copy_array=*gml_varmap_get(&copy->vars,"vector");
  REQUIRE((copy_array.arr==array.arr)==modern);
  GmlVal *binding=gml_varmap_get(&copy->vars,"vector");
  REQUIRE(gml_vm_array_prepare_write(&vm,binding,gml_varmap_value_owner(binding))!=NULL);
  REQUIRE(binding->arr==copy_array.arr && vm.next_array_owner==0);
  gml_arr_set(copy_array,0,vreal(99));
  REQUIRE(gml_arr_get(array,0).d==(modern?99:31));
  gml_arr_set(gml_arr_get(copy_array,1),0,vreal(101));
  REQUIRE(gml_arr_get(nested,0).d==(modern?101:45));
  copy->alarm[0]=91;
  REQUIRE(source->alarm[0]==7);
  gml_instance_destroy(&vm,copy);
  gml_vm_instances_reap(&vm);
  gml_struct_gc(&vm);
  REQUIRE(gml_array_heap_count(vm.array_heap)==2);
  REQUIRE(source->active && gml_arr_get(array,0).d==(modern?99:31));
  REQUIRE(!strcmp(gml_varmap_get(&source->vars,"owned_key")->s,"alpha"));
  source->active=0;
  source->deactivated=1;
  result=gml_builtin_call(&vm,"instance_copy",&argument,1);
  REQUIRE(result.d>source->id);
  REQUIRE(vm.inst_count==2 && vm.inst[1].active && !vm.inst[1].deactivated);
  REQUIRE(!source->active && source->deactivated);
  /* An exhausted pool must neither destroy the source nor replace the other copy. */
  vm.inst_cap=vm.inst_count;
  uint32_t next=vm.next_id,other_id=vm.inst[1].id;
  result=gml_builtin_call(&vm,"instance_copy",&argument,1);
  REQUIRE(result.t==V_REAL && result.d==-4);
  REQUIRE(vm.next_id==next && source->deactivated && vm.inst[1].id==other_id);
  vm.cur_self=NULL;
  REQUIRE(gml_builtin_call(&vm,"instance_copy",&argument,1).d==-4);
done:
  gml_vm_free(&vm);
  return ok;
}

static int copy_graphs_are_collected_between_frames(void){
  GmlWin content={.bytecode=16,.classic_version=800};
  GmlVM vm;
  if(gml_vm_init(&vm,&content,NULL)) return 0;
  int ok=1;
  unsigned char *state=NULL,*restored=NULL;
  vm.n_objects=1;
  vm.objects=calloc(1,sizeof(*vm.objects));
  REQUIRE(vm.objects!=NULL);
  vm.objects[0].parent=-1;
  vm.objects[0].sprite_index=vm.objects[0].mask_index=-1;
  gml_obj_alive_recount(&vm);
  GmlInstance *source=gml_instance_create(&vm,0,0,0);
  REQUIRE(source!=NULL);
  vm.cur_self=source;
  GmlVal root=gml_arr_new(3,vreal(0));
  GmlVal child=gml_arr_new(1,vreal(47));
  gml_arr_set(root,0,child); gml_arr_set(root,1,child); gml_arr_set(root,2,root);
  *gml_varmap_put(&source->vars,"graph")=root;
  for(int iteration=0;iteration<300;iteration++){
    int64_t id=gml_instance_copy(&vm,0);
    REQUIRE(id>=(int64_t)source->id+1);
    GmlInstance *copy=NULL;
    for(int i=0;i<vm.inst_count;i++) if(vm.inst[i].id==(uint32_t)id) copy=&vm.inst[i];
    REQUIRE(copy!=NULL);
    GmlVal copied=*gml_varmap_get(&copy->vars,"graph");
    REQUIRE(copied.arr!=root.arr);
    REQUIRE(gml_arr_get(copied,0).arr==gml_arr_get(copied,1).arr);
    REQUIRE(gml_arr_get(copied,2).arr==copied.arr);
    REQUIRE(gml_array_heap_count(vm.array_heap)==4);
    gml_arr_set(copied,3,gml_arr_new(0,vreal(0)));
    REQUIRE(gml_array_heap_count(vm.array_heap)==5);
    gml_instance_destroy(&vm,copy); gml_vm_instances_reap(&vm);
    gml_struct_gc(&vm);
    REQUIRE(vm.n_structs==0 && gml_array_heap_count(vm.array_heap)==2);
    REQUIRE(gml_arr_get(child,0).d==47);
  }
  /* The ordinary frame boundary also collects a burst without any structs or
   * an explicit test-only collection call. Retained nodes do not retrigger it. */
  for(int iteration=0;iteration<300;iteration++){
    REQUIRE(gml_instance_copy(&vm,0)>0);
    gml_instance_destroy(&vm,&vm.inst[1]); gml_vm_instances_reap(&vm);
  }
  REQUIRE(gml_array_heap_count(vm.array_heap)==602);
  REQUIRE(gml_array_heap_collection_due(vm.array_heap));
  gml_vm_step(&vm);
  REQUIRE(gml_array_heap_count(vm.array_heap)==2);
  REQUIRE(!gml_array_heap_collection_due(vm.array_heap));
  /* A child retained independently of its dead parent must survive the sweep. */
  REQUIRE(gml_instance_copy(&vm,0)>0);
  GmlInstance *copy=&vm.inst[1];
  GmlVal copied=*gml_varmap_get(&copy->vars,"graph");
  GmlVal retained=gml_arr_get(copied,0);
  *gml_varmap_put(&vm.globals,"retained")=retained;
  GmlVal list=gml_builtin_call(&vm,"ds_list_create",NULL,0);
  GmlVal args[2]={list,retained};
  gml_builtin_call(&vm,"ds_list_add",args,2);
  gml_instance_destroy(&vm,copy); gml_vm_instances_reap(&vm);
  gml_struct_gc(&vm);
  REQUIRE(gml_array_heap_count(vm.array_heap)==3);
  *gml_varmap_put(&vm.globals,"retained")=vreal(0);
  gml_struct_gc(&vm);
  REQUIRE(gml_array_heap_count(vm.array_heap)==3 && gml_arr_get(retained,0).d==47);
  gml_builtin_call(&vm,"ds_list_destroy",&list,1);
  gml_struct_gc(&vm);
  REQUIRE(gml_array_heap_count(vm.array_heap)==2);
  /* Parked locals, argument registers and stacks are live roots, even when the
   * copy's instance and all ordinary references have gone away. */
  REQUIRE(gml_instance_copy(&vm,0)>0);
  copy=&vm.inst[1]; copied=*gml_varmap_get(&copy->vars,"graph");
  vm.wait.n_frames=1;
  *gml_varmap_put(&vm.wait.frames[0].locals,"held")=copied;
  vm.wait.frames[0].args[0]=copied;
  vm.wait.frames[0].stack=calloc(1,sizeof(GmlVal));
  REQUIRE(vm.wait.frames[0].stack!=NULL);
  vm.wait.frames[0].stack[0]=copied; vm.wait.frames[0].stack_n=1;
  gml_instance_destroy(&vm,copy); gml_vm_instances_reap(&vm);
  gml_struct_gc(&vm);
  REQUIRE(gml_array_heap_count(vm.array_heap)==4);
  *gml_varmap_get(&vm.wait.frames[0].locals,"held")=vreal(0);
  gml_struct_gc(&vm); REQUIRE(gml_array_heap_count(vm.array_heap)==4);
  vm.wait.frames[0].args[0]=vreal(0);
  gml_struct_gc(&vm); REQUIRE(gml_array_heap_count(vm.array_heap)==4);
  vm.wait.frames[0].stack[0]=vreal(0);
  gml_struct_gc(&vm); REQUIRE(gml_array_heap_count(vm.array_heap)==2);
  gml_vm_wait_cancel(&vm);
  *gml_varmap_put(&vm.globals,"child")=child;
  size_t capacity=gml_vm_state_size(&vm),written=0,used=0,again=0;
  REQUIRE(capacity>0);
  state=malloc(capacity); restored=malloc(capacity);
  REQUIRE(state && restored);
  REQUIRE(gml_vm_state_save(&vm,state,capacity,&written));
  REQUIRE(gml_vm_state_load(&vm,state,written,&used) && used==written);
  REQUIRE(gml_array_heap_count(vm.array_heap)==2);
  REQUIRE(gml_vm_state_save(&vm,restored,capacity,&again));
  REQUIRE(again==written && !memcmp(state,restored,written));
  source=&vm.inst[0];
  root=*gml_varmap_get(&source->vars,"graph");
  child=*gml_varmap_get(&vm.globals,"child");
  REQUIRE(gml_arr_get(root,0).arr==child.arr && gml_arr_get(root,2).arr==root.arr);
  gml_instance_destroy(&vm,source); gml_vm_instances_reap(&vm);
  gml_struct_gc(&vm);
  REQUIRE(gml_array_heap_count(vm.array_heap)==1 && gml_arr_get(child,0).d==47);
  *gml_varmap_put(&vm.globals,"child")=vreal(0);
  gml_struct_gc(&vm);
  REQUIRE(gml_array_heap_count(vm.array_heap)==0);
done:
  free(state); free(restored);
  gml_vm_free(&vm);
  return ok;
}

static int copied_arrays_fork_on_named_writes(int mode,int cached){
  GmlWin content={.bytecode=mode?17:16};
  if(mode==1) content.has_array_owner_operations=1;
  if(mode==2) content.option_flags=UINT64_C(0x20000000);
  GmlVM vm;
  if(gml_vm_init(&vm,&content,NULL)) return 0;
  int ok=1;
  unsigned char *state=NULL,*again=NULL;
  vm.n_objects=1; vm.objects=calloc(1,sizeof(*vm.objects));
  REQUIRE(vm.objects!=NULL);
  vm.objects[0].parent=-1;
  vm.objects[0].sprite_index=vm.objects[0].mask_index=-1;
  gml_obj_alive_recount(&vm);
  GmlInstance *source=gml_instance_create(&vm,0,0,0);
  REQUIRE(source!=NULL);
  vm.cur_self=source;
  GmlVal array=gml_arr_new(3,vreal(13)),child=gml_arr_new(1,vreal(47));
  gml_arr_set(array,1,child);
  /* Persistent immutable text remains valid in both the fork and its source. */
  ((GmlArr *)array.arr)->data[2]=vstr("stable");
  *gml_varmap_put(&source->vars,"vector")=array;
  GmlVal *slot=gml_varmap_get(&source->vars,"vector");
  REQUIRE(gml_vm_array_prepare_write(&vm,slot,gml_varmap_value_owner(slot))!=NULL);
  uint64_t source_owner=*gml_varmap_value_owner(slot);
  REQUIRE(source_owner==1 && ((GmlArr *)slot->arr)->cow_owner==source_owner);
  GmlVal arg=vreal(0);
  int fast=gml_builtin_fast_id(&vm,"instance_copy");
  GmlVal id=cached?gml_builtin_call_fast_id(&vm,fast,"instance_copy",&arg,1)
                 :gml_builtin_call(&vm,"instance_copy",&arg,1);
  REQUIRE(id.d>source->id && vm.inst_count==2);
  GmlInstance *copy=&vm.inst[1];
  GmlVal *copy_slot=gml_varmap_get(&copy->vars,"vector");
  REQUIRE(copy_slot->arr==slot->arr && ((GmlArr *)slot->arr)->cow_owner==0);
  REQUIRE(gml_vm_array_prepare_write(&vm,copy_slot,gml_varmap_value_owner(copy_slot))!=NULL);
  REQUIRE(copy_slot->arr!=slot->arr && *gml_varmap_value_owner(copy_slot)==2);
  gml_arr_set(*copy_slot,0,vreal(81));
  REQUIRE(gml_arr_get(*slot,0).d==13);
  REQUIRE(gml_arr_get(*copy_slot,1).arr==gml_arr_get(*slot,1).arr);
  gml_arr_set(gml_arr_get(*copy_slot,1),0,vreal(71));
  REQUIRE(gml_arr_get(gml_arr_get(*slot,1),0).d==71);
  void *copy_storage=copy_slot->arr;
  REQUIRE(gml_vm_array_prepare_write(&vm,copy_slot,gml_varmap_value_owner(copy_slot))!=NULL);
  REQUIRE(copy_slot->arr==copy_storage);
  REQUIRE(!strcmp(gml_arr_get(*copy_slot,2).s,"stable"));
  /* Rehashing the table moves slots, but not their lvalue identities. */
  for(int i=0;i<100;i++){
    char name[32]; snprintf(name,sizeof name,"extra_%d",i);
    *gml_varmap_put_owned(&source->vars,strdup(name))=vreal(i);
  }
  slot=gml_varmap_get(&source->vars,"vector");
  REQUIRE(*gml_varmap_value_owner(slot)==source_owner);
  REQUIRE(gml_vm_array_prepare_write(&vm,slot,gml_varmap_value_owner(slot))!=NULL);
  gml_arr_set(*slot,0,vreal(91));
  REQUIRE(gml_arr_get(*copy_slot,0).d==81);
  REQUIRE(gml_instance_copy(&vm,0)>0 && vm.inst_count==3);
  GmlVal *third=gml_varmap_get(&vm.inst[2].vars,"vector");
  REQUIRE(third->arr==slot->arr && *gml_varmap_value_owner(third)==0);
  size_t capacity=gml_vm_state_size(&vm),written=0,used=0,resaved=0;
  state=malloc(capacity); again=malloc(capacity);
  REQUIRE(state && again && gml_vm_state_save(&vm,state,capacity,&written));
  REQUIRE(gml_vm_state_load(&vm,state,written,&used) && used==written);
  REQUIRE(vm.next_array_owner==2);
  REQUIRE(gml_vm_state_save(&vm,again,capacity,&resaved));
  REQUIRE(resaved==written && !memcmp(state,again,written));
  /* Two keyed bindings and one owned array occupy this fixture's exact trailer.
   * Malformed ownership must fail in a separate reader, without leaking partial
   * arrays or changing the still-live original VM. */
  /* Verify both absent parked owners and absent video before locating the request. */
  REQUIRE(written>=12 && !vm.wait.broadcast_map && !vm.wait.async_map &&
          !memcmp(state+written-12,(const unsigned char[12]){0},12));
  size_t ownership_end=written-12;
  REQUIRE(!vm.pending_game_load && ownership_end>=77 &&
          !memcmp(state+ownership_end-4,(const unsigned char[4]){0},4) &&
          state[ownership_end-5]==0 && copy_test_u64(state+ownership_end-77)==2);
  for(int fault=0;fault<7;fault++){
    memcpy(again,state,written);
    unsigned char *tail=again+ownership_end-77;
    if(fault==0) memset(tail,0,8);                 /* counter below live keys */
    if(fault==1) memcpy(tail+32,tail+12,8);       /* repeated lvalue key */
    if(fault==2) tail[20]=3;                     /* unknown lvalue kind */
    if(fault==3) memset(tail+24,255,4);           /* missing map target */
    if(fault==4) memset(tail+28,255,4);           /* invalid key ordinal */
    if(fault==5) memset(tail+56,0,4);             /* null owned-array ID */
    if(fault==6) memset(tail+60,0,8);             /* absent array owner */
    GmlVM rejected;
    REQUIRE(!gml_vm_init(&rejected,&content,NULL));
    rejected.n_objects=1; rejected.objects=calloc(1,sizeof(*rejected.objects));
    if(!rejected.objects){gml_vm_free(&rejected);REQUIRE(0);}
    rejected.objects[0].parent=-1;
    rejected.objects[0].sprite_index=rejected.objects[0].mask_index=-1;
    gml_obj_alive_recount(&rejected);
    int refused=!gml_vm_state_load(&rejected,again,written,&used);
    int cleared=gml_array_heap_count(rejected.array_heap)==0;
    gml_vm_free(&rejected);
    REQUIRE(refused && cleared);
  }
  source=&vm.inst[0];copy=&vm.inst[1];
  slot=gml_varmap_get(&source->vars,"vector");
  copy_slot=gml_varmap_get(&copy->vars,"vector");
  third=gml_varmap_get(&vm.inst[2].vars,"vector");
  REQUIRE(*gml_varmap_value_owner(slot)==1 && *gml_varmap_value_owner(copy_slot)==2);
  REQUIRE(third->arr==slot->arr);
  copy_storage=copy_slot->arr;
  REQUIRE(gml_vm_array_prepare_write(&vm,copy_slot,gml_varmap_value_owner(copy_slot))!=NULL);
  REQUIRE(copy_slot->arr==copy_storage);
  /* Exhaustion refuses a new identity without damaging existing bindings. */
  vm.next_array_owner=GML_ARRAY_OWNER_MAX;
  void *shared=third->arr;
  REQUIRE(!gml_vm_array_prepare_write(&vm,third,gml_varmap_value_owner(third)));
  REQUIRE(third->arr==shared && third->arr==slot->arr && *gml_varmap_value_owner(third)==0);
  REQUIRE(gml_vm_array_prepare_write(&vm,copy_slot,gml_varmap_value_owner(copy_slot))!=NULL);
  vm.next_array_owner=2;
  REQUIRE(gml_vm_array_prepare_write(&vm,third,gml_varmap_value_owner(third))!=NULL);
  REQUIRE(vm.next_array_owner==3 && third->arr!=slot->arr);
  gml_arr_set(*third,0,vreal(105));
  REQUIRE(gml_arr_get(*slot,0).d==91 && gml_arr_get(*copy_slot,0).d==81);
done:
  free(state);free(again);gml_vm_free(&vm);
  return ok;
}

static int argument_and_view_owners_survive_state(void){
  GmlWin content={.bytecode=17,.option_flags=UINT64_C(0x20000000),.n_code=1};
  content.code=calloc(1,sizeof(*content.code));
  if(!content.code) return 0;
  content.code[0].name="neutral";
  GmlVM vm;
  if(gml_vm_init(&vm,&content,NULL)){gml_win_free(&content);return 0;}
  int ok=1;
  unsigned char *state=NULL,*again=NULL;
  vm.script_argc=1;
  vm.script_args[0]=gml_arr_new(1,vreal(11));
  REQUIRE(gml_vm_array_prepare_write(&vm,&vm.script_args[0],&vm.script_array_owner[0])!=NULL);
  vm.wait.active=1;vm.wait.n_frames=1;
  GmlWaitFrame *frame=&vm.wait.frames[0];
  frame->ci=0;frame->argc=1;
  frame->args[0]=gml_arr_new(1,vreal(23));
  REQUIRE(gml_vm_array_prepare_write(&vm,&frame->args[0],&frame->array_owner[0])!=NULL);
  GmlVal *held=gml_varmap_put(&vm.globals,"held");
  *held=gml_arr_new(1,vreal(37));
  REQUIRE(gml_vm_array_prepare_write(&vm,held,gml_varmap_value_owner(held))!=NULL);
  frame->stack=calloc(3,sizeof(*frame->stack));
  frame->stack_type=calloc(3,sizeof(*frame->stack_type));
  REQUIRE(frame->stack && frame->stack_type);
  frame->stack_n=3;
  frame->stack[0]=frame->args[0];frame->stack[0].d=2;
  frame->stack[1]=*held;frame->stack[2]=vreal(47);
  size_t capacity=gml_vm_state_size(&vm),written=0,used=0,resaved=0;
  state=malloc(capacity);again=malloc(capacity);
  REQUIRE(state && again && gml_vm_state_save(&vm,state,capacity,&written));
  REQUIRE(gml_vm_state_load(&vm,state,written,&used) && used==written);
  REQUIRE(vm.next_array_owner==3 && vm.script_array_owner[0]==1);
  frame=&vm.wait.frames[0];
  REQUIRE(frame->array_owner[0]==2 && frame->stack[0].d==2 && frame->stack[1].d==0);
  REQUIRE(frame->stack[0].arr==frame->args[0].arr && gml_arr_get(frame->stack[0],0).d==23);
  void *storage=frame->args[0].arr;
  REQUIRE(gml_vm_array_prepare_write(&vm,&frame->args[0],&frame->array_owner[0])!=NULL);
  REQUIRE(frame->args[0].arr==storage);
  REQUIRE(gml_vm_state_save(&vm,again,capacity,&resaved));
  REQUIRE(resaved==written && !memcmp(state,again,written));
  /* Three lvalues, three arrays and one writable view occupy this exact fixture
   * trailer. Exercise every new selector plus the old-schema refusal. */
  REQUIRE(written>=12 && !vm.wait.broadcast_map && !vm.wait.async_map &&
          !memcmp(state+written-12,(const unsigned char[12]){0},12));
  size_t ownership_end=written-12;
  REQUIRE(!vm.pending_game_load && ownership_end>=137 &&
          !memcmp(state+ownership_end-4,(const unsigned char[4]){0},4) &&
          state[ownership_end-5]==0 && copy_test_u64(state+ownership_end-137)==3);
  for(int fault=0;fault<12;fault++){
    memcpy(again,state,written);
    unsigned char *tail=again+ownership_end-137;
    if(fault==0) tail[24]=1;                       /* live arguments have no frame target */
    if(fault==1) tail[28]=16;                      /* invalid live argument register */
    if(fault==2) tail[44]=1;                       /* absent parked frame */
    if(fault==3) tail[48]=16;                      /* invalid parked argument register */
    if(fault==4) tail[112]=2;                      /* too many view records */
    if(fault==5) tail[116]=1;                      /* absent view frame */
    if(fault==6) tail[120]=3;                      /* view beyond stack */
    if(fault==7) tail[120]=2;                      /* scalar stack target */
    if(fault==8) memset(tail+124,0,8);             /* absent view owner */
    if(fault==9) tail[124]=4;                      /* owner beyond saved counter */
    if(fault==10){tail[124]=255;tail[131]=127;}     /* non-exact view owner */
    if(fault==11) again[4]=30;                    /* preceding VM schema */
    GmlVM rejected;
    REQUIRE(!gml_vm_init(&rejected,&content,NULL));
    int refused=!gml_vm_state_load(&rejected,again,written,&used);
    int cleared=gml_array_heap_count(rejected.array_heap)==0;
    gml_vm_free(&rejected);
    REQUIRE(refused && cleared);
  }
done:
  free(state);free(again);gml_vm_free(&vm);gml_win_free(&content);
  return ok;
}

static void copy_test_word(GmlWin *content,int ci,uint32_t value){
  GmlCode *code=&content->code[ci];
  uint32_t at=code->start+code->length;
  for(unsigned i=0;i<4;i++) content->data[at+i]=(uint8_t)(value>>(8*i));
  code->length+=4;
}
static void copy_test_variable(GmlWin *content,int ci,unsigned opcode,int scope,
                               unsigned reference,const char *name){
  unsigned types=opcode==OP_POP?(DT_VAR<<4)|DT_VAR:DT_VAR;
  copy_test_word(content,ci,(opcode<<24)|(types<<16)|(uint16_t)scope);
  int entry=content->n_refs++;
  content->ref_addr[entry]=content->code[ci].start+content->code[ci].length;
  content->ref_name[entry]=name;content->ref_kind[entry]=GML_REF_VARIABLE;
  copy_test_word(content,ci,reference<<24);
}
static void copy_test_integer(GmlWin *content,int ci,int value){
  copy_test_word(content,ci,(UINT32_C(0x84)<<24)|(DT_INT16<<16)|(uint16_t)value);
}
static void copy_test_break(GmlWin *content,int ci,int operation){
  copy_test_word(content,ci,((uint32_t)OP_BREAK<<24)|(DT_INT16<<16)|(uint16_t)operation);
}

/* Independently authored native-format instructions distinguish specialized
 * whole-root pushes from generic indexed roots. No editor-produced fixture is
 * embedded: decoded raw opcode, COW policy and uncached/cached execution agree. */
static int specialized_array_roots_preserve_the_stack(int kind,int cow,int cached){
  GmlWin content={.bytecode=17,.owns=1,.size=512,.n_code=2,
                 .option_flags=cow?UINT64_C(0x20000000):0};
  content.data=calloc(content.size,1);content.code=calloc(2,sizeof(*content.code));
  content.ref_addr=calloc(32,sizeof(*content.ref_addr));
  content.ref_name=calloc(32,sizeof(*content.ref_name));
  content.ref_kind=calloc(32,sizeof(*content.ref_kind));
  if(!content.data || !content.code || !content.ref_addr || !content.ref_name || !content.ref_kind){
    gml_win_free(&content);return 0;
  }
  unsigned opcode=kind==0?0xC1:kind==1?0xC2:0xC3;
  int scope=kind==0?IT_LOCAL:kind==1?IT_GLOBAL:kind==2?IT_ARG:kind==3?IT_BUILTIN:IT_SELF;
  const char *name=(kind<2 || kind==4)?"vector":"argument0";
  uint32_t root_pc[2];
  for(int ci=0;ci<2;ci++){
    content.code[ci].name=ci?"neutral_array_write":"neutral_array_read";
    content.code[ci].start=ci?content.code[0].length:0;
    content.code[ci].cache_bad=!cached;
    if(kind==0){
      copy_test_variable(&content,ci,0xC2,IT_GLOBAL,0xA0,"vector");
      copy_test_variable(&content,ci,OP_POP,IT_LOCAL,0xA0,"vector");
    }
    if(ci) copy_test_integer(&content,ci,71);
    root_pc[ci]=content.code[ci].start+content.code[ci].length;
    copy_test_variable(&content,ci,opcode,scope,ci?0x90:0x10,name);
    if(ci){
      copy_test_integer(&content,ci,0);copy_test_break(&content,ci,-4);
      copy_test_integer(&content,ci,0);copy_test_break(&content,ci,-3);
      copy_test_variable(&content,ci,opcode,scope,0x10,name);
    }
    copy_test_word(&content,ci,((uint32_t)OP_RET<<24)|(DT_VAR<<16));
  }
  GmlVM vm;
  if(gml_vm_init(&vm,&content,NULL)){gml_win_free(&content);return 0;}
  int ok=1;
  vm.n_objects=1;vm.objects=calloc(1,sizeof(*vm.objects));
  REQUIRE(vm.objects!=NULL);
  vm.objects[0].parent=-1;vm.objects[0].sprite_index=vm.objects[0].mask_index=-1;
  gml_obj_alive_recount(&vm);
  GmlInstance *source=gml_instance_create(&vm,0,0,0);
  REQUIRE(source!=NULL);vm.cur_self=source;
  GmlVal root=gml_arr_new(2,vreal(0));
  gml_arr_set(root,0,gml_arr_new(1,vreal(17)));
  gml_arr_set(root,1,gml_arr_new(1,vreal(23)));
  *gml_varmap_put(&source->vars,"vector")=root;
  REQUIRE(gml_instance_copy(&vm,0)>0 && vm.inst_count==2);
  GmlInstance *copy=&vm.inst[1];
  GmlVal copied=*gml_varmap_get(&copy->vars,"vector");
  *gml_varmap_put(&vm.globals,"vector")=copied;
  for(int ci=0;ci<2;ci++){
    GmlInsn decoded;
    REQUIRE(gml_decode_bc_bounded(content.data,content.size,root_pc[ci],17,&decoded)==8);
    REQUIRE(decoded.kind==OP_PUSH && decoded.oldkind==opcode && decoded.inst==scope);
    REQUIRE(decoded.reftype==(ci?0x90:0x10));
  }
  GmlVal read=gml_vm_run_code(&vm,0,copy,NULL,&copied,1);
  REQUIRE((content.code[0].insn!=NULL)==cached);
  REQUIRE(read.t==V_ARR && read.arr==root.arr && vm.next_array_owner==0);
  REQUIRE(gml_arr_get(gml_arr_get(read,0),0).d==17);
  GmlVal result=gml_vm_run_code(&vm,1,copy,NULL,&copied,1);
  REQUIRE((content.code[1].insn!=NULL)==cached);
  REQUIRE(result.t==V_ARR && result.arr);
  REQUIRE((result.arr!=root.arr)==cow);
  REQUIRE(gml_arr_get(gml_arr_get(result,0),0).d==71);
  REQUIRE(gml_arr_get(gml_arr_get(root,0),0).d==(cow?17:71));
  REQUIRE(gml_arr_get(result,1).arr==gml_arr_get(root,1).arr);
  REQUIRE(gml_varmap_get(&copy->vars,"vector")->arr==(kind==4?result.arr:root.arr));
  REQUIRE(vm.script_argc==0 && vm.script_array_owner[0]==0);
  /* Direct access through an untouched sibling stays shared after the fork. */
  gml_arr_set(gml_arr_get(result,1),0,vreal(89));
  REQUIRE(gml_arr_get(gml_arr_get(root,1),0).d==89);
done:
  if(!ok) fprintf(stderr,"specialized root kind=%d cow=%d cached=%d\n",kind,cow,cached);
  gml_vm_free(&vm);gml_win_free(&content);
  return ok;
}

static int copied_text_survives_an_owned_pop(int classic){
  GmlWin content={.bytecode=classic?16:17,.classic_version=classic?800:0,
                 .has_array_owner_operations=classic?0:1};
  GmlVM vm;
  if(gml_vm_init(&vm,&content,NULL)) return 0;
  int ok=1;
  GmlVal popped=vreal(0);
  vm.n_objects=1; vm.objects=calloc(1,sizeof(*vm.objects));
  REQUIRE(vm.objects!=NULL);
  vm.objects[0].parent=-1;
  vm.objects[0].sprite_index=vm.objects[0].mask_index=-1;
  gml_obj_alive_recount(&vm);
  GmlInstance *source=gml_instance_create(&vm,0,0,0);
  REQUIRE(source!=NULL);
  vm.cur_self=source;
  GmlVal array=gml_arr_new(1,vstr("owned text"));
  *gml_varmap_put(&source->vars,"text_array")=array;
  *gml_varmap_put(&source->vars,"scalar_text")=vstr_owned(strdup("scalar text"));
  REQUIRE(gml_instance_copy(&vm,0)>0);
  GmlVal *copy=gml_varmap_get(&vm.inst[1].vars,"text_array");
  REQUIRE(gml_vm_array_prepare_write(&vm,copy,gml_varmap_value_owner(copy))!=NULL);
  popped=gml_arr_pop(array);
  GmlVal retained=gml_arr_get(*copy,0);
  /* An owned pop may be freed by the caller. A surviving copy must either own
   * different text or borrow a value whose shared lifetime was retained. */
  REQUIRE(popped.t==V_STR && retained.t==V_STR);
  REQUIRE(popped.d==0 || popped.s!=retained.s);
  if(popped.d!=0){free((char *)popped.s);popped=vreal(0);}
  REQUIRE(!strcmp(retained.s,"owned text"));
  gml_struct_gc(&vm);
  REQUIRE(!strcmp(gml_arr_get(*copy,0).s,"owned text"));
  popped=vreal(0);
  for(int i=0;i<300;i++){
    gml_arr_set(*copy,0,vstr("replacement"));
    gml_struct_gc(&vm);
    REQUIRE(!strcmp(gml_arr_get(*copy,0).s,"replacement"));
    REQUIRE(gml_array_heap_count(vm.array_heap)==4);
    REQUIRE(!strcmp(gml_varmap_get(&vm.inst[1].vars,"scalar_text")->s,"scalar text"));
  }
  gml_instance_destroy(&vm,&vm.inst[1]);gml_instance_destroy(&vm,source);
  gml_vm_instances_reap(&vm);gml_struct_gc(&vm);
  REQUIRE(gml_array_heap_count(vm.array_heap)==0);
done:
  gml_vm_free(&vm);
  if(popped.t==V_STR && popped.d!=0) free((char *)popped.s);
  return ok;
}

static int copied_methods_bind_to_the_new_identity(int cached,int cold){
  GmlWin content={.bytecode=17,.n_code=1};
  content.code=calloc(1,sizeof(*content.code));
  if(!content.code) return 0;
  content.code[0].name="neutral_method";
  GmlVM vm;
  if(gml_vm_init(&vm,&content,NULL)){ gml_win_free(&content); return 0; }
  int ok=1; unsigned char *state=NULL,*again=NULL;
  vm.n_objects=1; vm.objects=calloc(1,sizeof(*vm.objects));
  REQUIRE(vm.objects!=NULL);
  vm.objects[0].parent=vm.objects[0].sprite_index=vm.objects[0].mask_index=-1;
  gml_obj_alive_recount(&vm);
  GmlInstance *source=gml_instance_create(&vm,1,2,0);
  GmlInstance *other=gml_instance_create(&vm,3,4,0);
  REQUIRE(source && other);
  vm.cur_self=source;
  GmlVal arguments[]={vreal(source->id),vreal(GML_FUNCVAL_TAG)};
  GmlVal method=gml_builtin_call(&vm,"method",arguments,2);
  arguments[0]=vreal(other->id);
  GmlVal external=gml_builtin_call(&vm,"method",arguments,2);
  GmlInstance *wrapper=gml_struct_find(&vm,(unsigned)method.d);
  GmlInstance *structure=gml_struct_new(&vm);
  REQUIRE(wrapper && structure);
  wrapper->method_bound=!cold;
  *gml_varmap_put(&structure->vars,"method")=method;
  *gml_varmap_put(&source->vars,"method")=method;
  *gml_varmap_put(&source->vars,"method_alias")=method;
  *gml_varmap_put(&source->vars,"external")=external;
  *gml_varmap_put(&source->vars,"ordinary")=vreal(structure->id);
  *gml_varmap_put(&source->vars,"function")=arguments[1];
  *gml_varmap_put(&source->vars,"self_reference")=vreal(source->id);
  GmlVal array=gml_arr_new(1,method);
  *gml_varmap_put(&source->vars,"methods_in_array")=array;
  GmlVal perf=vreal(0);
  int fast=gml_builtin_fast_id(&vm,"instance_copy");
  GmlVal id=cached?gml_builtin_call_fast_id(&vm,fast,"instance_copy",&perf,1)
                  :gml_builtin_call(&vm,"instance_copy",&perf,1);
  REQUIRE(id.d>other->id && vm.inst_count==3);
  GmlInstance *copy=&vm.inst[2];
  GmlVal rebound=*gml_varmap_get(&copy->vars,"method");
  REQUIRE(rebound.d!=method.d);
  REQUIRE(gml_varmap_get(&copy->vars,"method_alias")->d==rebound.d);
  REQUIRE(gml_builtin_call(&vm,"method_get_self",&rebound,1).d==copy->id);
  REQUIRE(gml_builtin_call(&vm,"method_get_self",&method,1).d==source->id);
  REQUIRE(gml_varmap_get(&copy->vars,"external")->d==external.d);
  REQUIRE(gml_varmap_get(&copy->vars,"ordinary")->d==structure->id);
  REQUIRE(gml_varmap_get(&copy->vars,"function")->d==GML_FUNCVAL_TAG);
  REQUIRE(gml_varmap_get(&copy->vars,"self_reference")->d==source->id);
  REQUIRE(gml_varmap_get(&copy->vars,"methods_in_array")->arr==array.arr);
  REQUIRE(gml_arr_get(array,0).d==method.d && gml_varmap_get(&structure->vars,"method")->d==method.d);
  size_t capacity=gml_vm_state_size(&vm),written=0,used=0,resaved=0;
  state=malloc(capacity); again=malloc(capacity);
  REQUIRE(state && again && gml_vm_state_save(&vm,state,capacity,&written));
  REQUIRE(gml_vm_state_load(&vm,state,written,&used) && used==written);
  rebound=*gml_varmap_get(&vm.inst[2].vars,"method");
  REQUIRE(gml_builtin_call(&vm,"method_get_self",&rebound,1).d==id.d);
  REQUIRE(gml_vm_state_save(&vm,again,capacity,&resaved));
  REQUIRE(resaved==written && !memcmp(state,again,written));
done:
  free(state); free(again); gml_vm_free(&vm); gml_win_free(&content); return ok;
}

static int copied_method_calls_keep_the_expression(int cached,int collision){
  GmlWin content={.bytecode=17,.owns=1,.size=128,.n_code=2};
  content.data=calloc(content.size,1); content.code=calloc(2,sizeof(*content.code));
  content.ref_addr=calloc(2,sizeof(*content.ref_addr));
  content.ref_name=calloc(2,sizeof(*content.ref_name));
  content.ref_kind=calloc(2,sizeof(*content.ref_kind));
  if(!content.data || !content.code || !content.ref_addr || !content.ref_name || !content.ref_kind){
    gml_win_free(&content); return 0;
  }
  content.code[0].name=collision?"gml_Script_self_method":"gml_Script_neutral_receiver";
  content.code[1].name="gml_Script_neutral_invoke";
  copy_test_variable(&content,0,OP_PUSH,IT_SELF,0xA0,"id");
  copy_test_word(&content,0,((uint32_t)OP_RET<<24)|(DT_VAR<<16));
  content.code[1].start=content.code[0].length;
  copy_test_integer(&content,1,79);
  copy_test_word(&content,1,((uint32_t)OP_CALL<<24)|(DT_INT32<<16));
  content.ref_addr[1]=content.code[1].start+content.code[1].length;
  content.ref_name[1]="self_method"; content.ref_kind[1]=GML_REF_FUNCTION; content.n_refs=2;
  copy_test_word(&content,1,0);
  copy_test_word(&content,1,((uint32_t)OP_ADD<<24)|(((DT_VAR<<4)|DT_VAR)<<16));
  copy_test_word(&content,1,((uint32_t)OP_RET<<24)|(DT_VAR<<16));
  content.code[0].cache_bad=content.code[1].cache_bad=!cached;
  GmlVM vm;
  if(gml_vm_init(&vm,&content,NULL)){ gml_win_free(&content); return 0; }
  int ok=1;
  vm.n_objects=1; vm.objects=calloc(1,sizeof(*vm.objects));
  REQUIRE(vm.objects!=NULL);
  vm.objects[0].parent=vm.objects[0].sprite_index=vm.objects[0].mask_index=-1;
  gml_obj_alive_recount(&vm);
  GmlInstance *source=gml_instance_create(&vm,1,2,0);
  REQUIRE(source!=NULL); vm.cur_self=source;
  GmlVal arguments[]={vreal(source->id),vreal(GML_FUNCVAL_TAG)};
  GmlVal method=gml_builtin_call(&vm,"method",arguments,2);
  *gml_varmap_put(&source->vars,"self_method")=method;
  int64_t id=gml_instance_copy(&vm,0);
  REQUIRE(id>source->id && vm.inst_count==2);
  GmlInstance *copy=&vm.inst[1];
  REQUIRE(gml_vm_run_code(&vm,1,source,NULL,NULL,0).d==79+source->id);
  REQUIRE(gml_vm_run_code(&vm,1,copy,NULL,NULL,0).d==79+id);
  REQUIRE((content.code[1].insn!=NULL)==cached);
  *gml_varmap_get(&copy->vars,"self_method")=method;
  REQUIRE(gml_vm_run_code(&vm,1,copy,NULL,NULL,0).d==79+(collision?id:source->id));
done:
  gml_vm_free(&vm); gml_win_free(&content); return ok;
}

static int copied_physics_remains_unbound(int mode){
  GmlWin content={.bytecode=17};
  GmlVM vm;
  if(gml_vm_init(&vm,&content,NULL)) return 0;
  int ok=1; unsigned char *state=NULL,*again=NULL;
  vm.n_objects=1; vm.objects=calloc(1,sizeof(*vm.objects));
  REQUIRE(vm.objects!=NULL);
  GmlObject *object=&vm.objects[0];
  object->parent=object->sprite_index=object->mask_index=-1;
  object->physics_enabled=mode!=2;
  object->physics_density=1;
  object->physics_point_count=1;
  object->physics_point[0][0]=3;
  gml_obj_alive_recount(&vm);
  GmlInstance *source=gml_instance_create(&vm,11,22,0);
  REQUIRE(source!=NULL);
  vm.cur_self=source;
  GmlVal scale=vreal(0.1);
  (void)gml_builtin_call(&vm,"physics_world_create",&scale,1);
  if(mode==1) gml_physics_initialize_body(&vm,source);
  if(mode==2){
    GmlVal fixture=gml_builtin_call(&vm,"physics_fixture_create",NULL,0);
    GmlVal shape[]={fixture,vreal(3)};
    (void)gml_builtin_call(&vm,"physics_fixture_set_circle_shape",shape,2);
    GmlVal bind[]={fixture,vreal(source->id)};
    REQUIRE(gml_builtin_call(&vm,"physics_fixture_bind",bind,2).d>0);
  }
  *gml_varmap_put(&source->vars,"phy_linear_velocity_x")=vreal(17);
  *gml_varmap_put(&source->vars,"__phy_force_x")=vreal(19);
  *gml_varmap_put(&source->vars,"phy_collision_x")=vreal(23);
  *gml_varmap_put(&source->vars,"phy_user_data")=vreal(37);
  int64_t id=gml_instance_copy(&vm,0);
  REQUIRE(id>source->id && vm.inst_count==2);
  GmlInstance *copy=&vm.inst[1];
  REQUIRE(!gml_physics_body_enabled(&vm,copy));
  REQUIRE(!gml_varmap_get(&copy->vars,"phy_linear_velocity_x"));
  REQUIRE(!gml_varmap_get(&copy->vars,"__phy_force_x"));
  REQUIRE(!gml_varmap_get(&copy->vars,"phy_collision_x"));
  REQUIRE(gml_varmap_get(&copy->vars,"phy_user_data")->d==37);
  REQUIRE(gml_varmap_get(&copy->vars,"__phy_body")->d==-1);
  gml_physics_step(&vm);
  REQUIRE(copy->x==11 && copy->y==22 && gml_physics_body_enabled(&vm,source));
  for(int i=0;i<GML_PHYS_FIXTURE_MAX;i++)
    REQUIRE(!vm.builtins->phys_fixture[i].live || vm.builtins->phys_fixture[i].bound_inst!=(int)id);
  size_t capacity=gml_vm_state_size(&vm),written=0,used=0,resaved=0;
  state=malloc(capacity); again=malloc(capacity);
  REQUIRE(state && again && gml_vm_state_save(&vm,state,capacity,&written));
  REQUIRE(gml_vm_state_load(&vm,state,written,&used) && used==written);
  copy=&vm.inst[1];
  REQUIRE(!gml_physics_body_enabled(&vm,copy));
  REQUIRE(gml_vm_state_save(&vm,again,capacity,&resaved));
  REQUIRE(resaved==written && !memcmp(state,again,written));
  /* An explicit fixture binding can opt the new instance into physics later. */
  GmlVal fixture=gml_builtin_call(&vm,"physics_fixture_create",NULL,0);
  GmlVal shape[]={fixture,vreal(2)};
  (void)gml_builtin_call(&vm,"physics_fixture_set_circle_shape",shape,2);
  GmlVal bind[]={fixture,vreal((double)id)};
  REQUIRE(gml_builtin_call(&vm,"physics_fixture_bind",bind,2).d>0);
  REQUIRE(gml_physics_body_enabled(&vm,copy) && gml_varmap_get(&copy->vars,"__phy_body")->d==1);
done:
  free(state); free(again); gml_vm_free(&vm); return ok;
}

static int many_map_owners_preserve_canonical_state(void){
  GmlWin content={.bytecode=17,.option_flags=UINT64_C(0x20000000)};
  GmlVM vm;
  if(gml_vm_init(&vm,&content,NULL)) return 0;
  int ok=1;
  unsigned char *state=NULL,*again=NULL;
  /* Binding order opposes canonical key order and crosses the writer's first
   * owner-buffer growth boundary. */
  for(int index=79;index>=0;index--){
    char key[32];
    snprintf(key,sizeof(key),"value-%03d",index);
    GmlVal *binding=gml_varmap_put_owned(&vm.globals,strdup(key));
    REQUIRE(binding!=NULL);
    *binding=gml_arr_new(1,vreal(index));
    REQUIRE(gml_vm_array_prepare_write(&vm,binding,gml_varmap_value_owner(binding))!=NULL);
  }
  REQUIRE(vm.next_array_owner==80);
  size_t capacity=gml_vm_state_size(&vm),written=0,used=0,resaved=0;
  state=malloc(capacity);
  again=malloc(capacity);
  REQUIRE(state && again && gml_vm_state_save(&vm,state,capacity,&written));
  size_t rejected=0;
  vm.globals.len--;
  int accepted=gml_vm_state_save(&vm,again,capacity,&rejected);
  vm.globals.len++;
  REQUIRE(!accepted);
  REQUIRE(gml_vm_state_load(&vm,state,written,&used) && used==written);
  REQUIRE(vm.next_array_owner==80);
  for(int index=0;index<80;index++){
    char key[32];
    snprintf(key,sizeof(key),"value-%03d",index);
    GmlVal *binding=gml_varmap_get(&vm.globals,key);
    REQUIRE(binding && *gml_varmap_value_owner(binding)==(uint64_t)(80-index));
    REQUIRE(gml_arr_get(*binding,0).d==index);
  }
  REQUIRE(gml_vm_state_save(&vm,again,capacity,&resaved));
  REQUIRE(resaved==written && !memcmp(state,again,written));
done:
  free(state);
  free(again);
  gml_vm_free(&vm);
  return ok;
}

int main(void){
  int ok=1;
  for(int modern=0;modern<=1;modern++) for(int cached=0;cached<=1;cached++)
    ok &= copy_storage_and_identity(modern,cached);
  ok &= copy_graphs_are_collected_between_frames();
  for(int mode=0;mode<3;mode++) for(int cached=0;cached<2;cached++)
    ok &= copied_arrays_fork_on_named_writes(mode,cached);
  ok &= argument_and_view_owners_survive_state();
  for(int kind=0;kind<5;kind++) for(int cow=0;cow<2;cow++) for(int cached=0;cached<2;cached++)
    ok &= specialized_array_roots_preserve_the_stack(kind,cow,cached);
  ok &= copied_text_survives_an_owned_pop(1);
  ok &= copied_text_survives_an_owned_pop(0);
  for(int cached=0;cached<2;cached++) for(int cold=0;cold<2;cold++)
    ok &= copied_methods_bind_to_the_new_identity(cached,cold);
  for(int mode=0;mode<3;mode++) ok &= copied_physics_remains_unbound(mode);
  for(int cached=0;cached<2;cached++) for(int collision=0;collision<2;collision++)
    ok &= copied_method_calls_keep_the_expression(cached,collision);
  ok &= many_map_owners_preserve_canonical_state();
  if(ok) puts("instance copy: storage/dispatch, bounded lifetime, COW, parked ownership and twenty native root controls passed");
  return ok?0:1;
}
