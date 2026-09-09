/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Documented sequence-copy contracts, through ordered and cached dispatch. */
#include "gml_builtin.h"
#include "gml_particle.h"
#include "anygm_test_runner.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int expect(int condition,const char *message){
  if(!condition) fprintf(stderr,"%s\n",message);
  return condition;
}
static int real_is(GmlVal v,double number){ return v.t==V_REAL && v.d==number; }
static int string_is(GmlVal v,const char *s){ return v.t==V_STR && v.s && !strcmp(v.s,s); }
static GmlVal invoke(GmlVM *vm,const char *name,GmlVal *a,int n,int cached,int *ok){
  if(!cached) return gml_builtin_call(vm,name,a,n);
  int id=gml_builtin_fast_id(vm,name);
  *ok &= expect(id>=0,"sequence operation must have a cached builtin ID");
  return id>=0?gml_builtin_call_fast_id(vm,id,name,a,n):vundef();
}
static int concat_case(void){
  int ok=1;
  for(int cached=0;cached<2;cached++){
    GmlVM vm={0};
    GmlVal left=gml_arr_new(2,vreal(7)), right=gml_arr_new(1,vstr("text"));
    GmlVal empty=gml_arr_new(0,vreal(0));
    GmlVal args[]={left,empty,right,left};
    GmlVal joined=invoke(&vm,"array_concat",args,4,cached,&ok);
    ok &= expect(joined.t==V_ARR && joined.arr && joined.arr!=left.arr &&
                 joined.arr!=right.arr && gml_val_array_length(joined)==5,
                 "concat returns a fresh array containing every element");
    ok &= expect(real_is(gml_arr_get(joined,0),7) && real_is(gml_arr_get(joined,1),7) &&
                 string_is(gml_arr_get(joined,2),"text") &&
                 real_is(gml_arr_get(joined,3),7) && real_is(gml_arr_get(joined,4),7),
                 "concat preserves order, types and duplicates");
    gml_arr_set(joined,0,vreal(19));
    gml_arr_set(left,1,vreal(23));
    ok &= expect(real_is(gml_arr_get(left,0),7) && real_is(gml_arr_get(joined,1),7) &&
                 gml_val_array_length(left)==2 && gml_val_array_length(right)==1,
                 "copy and source have independent top-level storage");
    GmlVal empties[]={empty,empty};
    GmlVal zero=invoke(&vm,"array_concat",empties,2,cached,&ok);
    ok &= expect(zero.t==V_ARR && zero.arr && zero.arr!=empty.arr &&
                 gml_val_array_length(zero)==0,"empty concat returns a new array");
    gml_values_release(&right,1);
    ok &= expect(string_is(gml_arr_get(joined,2),"text"),
                 "concat strings survive source destruction");
    GmlVal roots[]={left,empty,joined,zero};
    gml_values_release(roots,4);
    gml_vm_free(&vm);
  }
  return ok;
}
static int sequence_case(int stack){
  const char *create=stack?"ds_stack_create":"ds_queue_create";
  const char *push=stack?"ds_stack_push":"ds_queue_enqueue";
  const char *pop=stack?"ds_stack_pop":"ds_queue_dequeue";
  const char *size=stack?"ds_stack_size":"ds_queue_size";
  const char *peek=stack?"ds_stack_top":"ds_queue_head";
  const char *copy=stack?"ds_stack_copy":"ds_queue_copy";
  const char *clear=stack?"ds_stack_clear":"ds_queue_clear";
  int ok=1;
  for(int cached=0;cached<2;cached++){
    GmlVM vm={0};
    GmlVal src=gml_builtin_call(&vm,create,NULL,0), dst=gml_builtin_call(&vm,create,NULL,0);
    GmlVal values[]={src,vreal(7),vstr("text"),vreal(11)};
    GmlVal stale[]={dst,vreal(99),vreal(100),vreal(101),vreal(102)};
    gml_builtin_call(&vm,push,values,4);
    gml_builtin_call(&vm,push,stale,5);
    GmlVal args[]={dst,src};
    invoke(&vm,copy,args,2,cached,&ok);
    ok &= expect(real_is(gml_builtin_call(&vm,size,&dst,1),3) &&
                 real_is(gml_builtin_call(&vm,size,&src,1),3),
                 "copy replaces the destination without consuming the source");
    ok &= expect(real_is(gml_builtin_call(&vm,pop,&dst,1),stack?11:7) &&
                 string_is(gml_builtin_call(&vm,pop,&dst,1),"text") &&
                 real_is(gml_builtin_call(&vm,pop,&dst,1),stack?7:11),
                 "copy preserves FIFO or LIFO order, including strings");
    ok &= expect(real_is(gml_builtin_call(&vm,size,&src,1),3) &&
                 real_is(gml_builtin_call(&vm,peek,&src,1),stack?11:7),
                 "consuming the copy leaves the source unchanged");
    invoke(&vm,copy,args,2,cached,&ok);
    gml_builtin_call(&vm,clear,&src,1);
    ok &= expect(real_is(gml_builtin_call(&vm,size,&dst,1),3),
                 "clearing the source does not clear its copy");
    invoke(&vm,copy,args,2,cached,&ok);
    ok &= expect(real_is(gml_builtin_call(&vm,size,&dst,1),0),
                 "an empty source clears the destination");
    gml_vm_free(&vm);
  }
  return ok;
}
static int queue_case(void){ return sequence_case(0); }
static int stack_case(void){ return sequence_case(1); }
/* Defensive input policy for invalid calls. */
static int invalid_case(void){
  int ok=1;
  GmlVM vm={0};
  GmlVal empty=gml_arr_new(0,vreal(0));
  GmlVal wrong[]={empty,vreal(1)};
  ok &= expect(gml_builtin_call(&vm,"array_concat",wrong,2).t==V_UNDEF,
               "a scalar concat argument must not produce a partial array");
  ok &= expect(gml_builtin_call(&vm,"array_concat",NULL,0).t==V_UNDEF &&
               gml_builtin_call(&vm,"array_concat",&empty,1).t==V_UNDEF,
               "concat rejects missing required arguments");
  /* Synthetic lengths prove rejection before any element access or allocation. */
  GmlArr large={.len=16000000}, extra={.len=1};
  GmlVal oversized[]={{.t=V_ARR,.arr=&large},{.t=V_ARR,.arr=&extra}};
  ok &= expect(gml_builtin_call(&vm,"array_concat",oversized,2).t==V_UNDEF,
               "concat rejects a sum above the array allocation bound");
  const char *copies[]={"ds_queue_copy","ds_stack_copy"};
  for(int i=0;i<2;i++){
    GmlVal dst=gml_builtin_call(&vm,"ds_queue_create",NULL,0);
    GmlVal value[]={dst,vreal(42)};
    gml_builtin_call(&vm,"ds_queue_enqueue",value,2);
    GmlVal args[]={dst,vreal(-1)};
    gml_builtin_call(&vm,copies[i],args,2);
    args[1]=dst;
    gml_builtin_call(&vm,copies[i],args,2);
    gml_builtin_call(&vm,copies[i],&dst,1);
    ok &= expect(real_is(gml_builtin_call(&vm,"ds_queue_size",&dst,1),1) &&
                 real_is(gml_builtin_call(&vm,"ds_queue_head",&dst,1),42),
                 "invalid source, missing source and self-copy preserve the destination");
  }
  gml_values_release(&empty,1);
  gml_vm_free(&vm);
  return ok;
}
static int state_case(void){
  GmlWin win={0};
  GmlVM vm={0};
  vm.win=&win;
  vm.room_index=-1;
  vm.pending_room=-1;
  vm.next_creation_seq=1;
  vm.particles=gml_particle_state_create(&vm);
  if(!vm.particles) return 0;
  gml_vm_software3d_reset(&vm);
  GmlVal source=gml_arr_new(1,vstr("saved"));
  GmlVal arrays[]={source,source};
  GmlVal joined=gml_builtin_call(&vm,"array_concat",arrays,2);
  *gml_varmap_put(&vm.globals,"source")=source;
  *gml_varmap_put(&vm.globals,"joined")=joined;
  GmlVal queue=gml_builtin_call(&vm,"ds_queue_create",NULL,0);
  GmlVal stack=gml_builtin_call(&vm,"ds_stack_create",NULL,0);
  GmlVal queue_copy=gml_builtin_call(&vm,"ds_queue_create",NULL,0);
  GmlVal stack_copy=gml_builtin_call(&vm,"ds_stack_create",NULL,0);
  GmlVal enqueue[]={queue,joined}, push[]={stack,joined};
  gml_builtin_call(&vm,"ds_queue_enqueue",enqueue,2);
  gml_builtin_call(&vm,"ds_stack_push",push,2);
  GmlVal queue_args[]={queue_copy,queue}, stack_args[]={stack_copy,stack};
  gml_builtin_call(&vm,"ds_queue_copy",queue_args,2);
  gml_builtin_call(&vm,"ds_stack_copy",stack_args,2);
  size_t size=gml_vm_state_size(&vm), written=0, used=0;
  void *bytes=malloc(size?size:1);
  int ok=expect(bytes && gml_vm_state_save(&vm,bytes,size,&written) && written==size,
                "new sequence results serialize through the existing state owner");
  if(ok){
    gml_arr_set(joined,0,vreal(0));
    gml_builtin_call(&vm,"ds_queue_clear",&queue_copy,1);
    gml_builtin_call(&vm,"ds_stack_clear",&stack_copy,1);
    ok &= expect(gml_vm_state_load(&vm,bytes,written,&used) && used==written,
                 "same-execution sequence snapshot restores");
    if(ok){
      GmlVal *restored=gml_varmap_get(&vm.globals,"joined");
      GmlVal *original=gml_varmap_get(&vm.globals,"source");
      GmlVal head=gml_builtin_call(&vm,"ds_queue_head",&queue_copy,1);
      GmlVal top=gml_builtin_call(&vm,"ds_stack_top",&stack_copy,1);
      ok &= expect(restored && original && restored->t==V_ARR &&
                   restored->arr!=original->arr && gml_val_array_length(*restored)==2 &&
                   string_is(gml_arr_get(*restored,0),"saved") &&
                   head.t==V_ARR && head.arr==restored->arr &&
                   top.t==V_ARR && top.arr==restored->arr,
                   "restore preserves contents, separate arrays and shared DS element references");
    }
  }
  free(bytes);
  gml_vm_free(&vm);
  return ok;
}
int main(void){
  static const AnygmTestCase cases[]={
    {"array_concat",concat_case},{"queue_copy",queue_case},{"stack_copy",stack_case},
    {"defensive_policy",invalid_case},
    {"current_state",state_case},
  };
  const AnygmTestGroup group={"sequence_copy",cases,sizeof(cases)/sizeof(cases[0])};
  AnygmTestResult result={0};
  return anygm_test_run_groups(&group,1,NULL,&result)?0:1;
}
