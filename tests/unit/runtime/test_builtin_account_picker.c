/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "gml_builtin_internal.h"
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int expect(int condition,const char *message){
  if(!condition) fprintf(stderr,"account picker: %s\n",message);
  return condition;
}
static GmlVal request(GmlVM *vm,double pad,int cached){
  GmlVal arguments[]={vreal(pad),vreal(0)};
  if(!cached) return gml_builtin_call(vm,"xboxone_show_account_picker",arguments,2);
  int id=gml_builtin_fast_id(vm,"xboxone_show_account_picker");
  if(id<0) return vundef();
  return gml_builtin_call_fast_id(vm,id,"xboxone_show_account_picker",arguments,2);
}
static int real_is(GmlVal value,double number){return value.t==V_REAL && value.d==number;}
static void word(unsigned char *p,uint32_t value){
  for(unsigned i=0;i<4;i++) p[i]=(unsigned char)(value>>(8*i));
}
static int deferred_capacity_and_reset(int bytecode){
  GmlWin win={.bytecode=bytecode};GmlVM vm={0};
  if(gml_vm_init(&vm,&win,NULL)) return 0;
  int ok=1;uint32_t rng[16];memcpy(rng,vm.rng_well,sizeof rng);
  *gml_varmap_put(&vm.globals,"async_load")=vint64(991);
  for(int i=0;i<GML_ACCOUNT_PICKER_REQUEST_MAX;i++){
    ok=expect(real_is(request(&vm,i-3,i%2),i+1),"distinct named/cached identity") && ok;
    ok=expect(gml_builtin_account_picker_pending_count(&vm)==i+1 &&
              vm.builtins->account_picker[i].pad==i-3,"deferred controller metadata") && ok;
  }
  ok=expect(real_is(request(&vm,0,0),-1) && vm.builtins->async_seq==16,
            "full queue preserves the next identity") && ok;
  vm.wait.active=1;gml_fire_async_account_picker(&vm);vm.wait.active=0;
  ok=expect(vm.builtins->n_account_picker==16,"parked simulation retains completions") && ok;
  gml_fire_async_account_picker(&vm);
  GmlVal *outer=gml_varmap_get(&vm.globals,"async_load");
  ok=expect(gml_builtin_account_picker_pending_count(&vm)==0 && vm.builtins->ds_maps.live_count==0 &&
            outer && real_is(*outer,991) && gml_val_number_kind(*outer)==GML_NUM_INT64,
            "drain retires maps and restores the full outer value") && ok;
  ok=expect(!memcmp(rng,vm.rng_well,sizeof rng),"request and drain preserve RNG") && ok;
  int sequence=vm.builtins->async_seq;
  for(int cached=0;cached<2;cached++)
    ok=expect(real_is(request(&vm,NAN,cached),-1) &&
              real_is(request(&vm,INFINITY,cached),-1) &&
              real_is(request(&vm,(double)INT_MAX+1.0,cached),-1) &&
              vm.builtins->async_seq==sequence,"invalid conversion is bounded") && ok;
  vm.builtins->async_seq=INT_MAX;
  ok=expect(real_is(request(&vm,0,0),-1) && vm.builtins->n_account_picker==0,
            "identity exhaustion cannot wrap") && ok;
  gml_builtin_state_reset(vm.builtins);
  ok=expect(vm.builtins->n_account_picker==0 && vm.builtins->async_seq==0 &&
            real_is(request(&vm,2,1),1),"reset starts a fresh lifetime") && ok;
  gml_vm_free(&vm);return ok;
}
static int pending_state_is_canonical_and_validated(int bytecode){
  GmlWin win={.bytecode=bytecode};GmlVM vm={0};
  if(gml_vm_init(&vm,&win,NULL)) return 0;
  int ok=1;
  for(int i=0;i<3;i++) ok=real_is(request(&vm,i-1,i%2),i+1) && ok;
  size_t size=gml_vm_state_size(&vm),written=0,used=0;
  unsigned char *saved=malloc(size),*repeated=malloc(size),*damaged=malloc(size);
  if(!saved || !repeated || !damaged){
    free(saved);free(repeated);free(damaged);gml_vm_free(&vm);return 0;
  }
  ok=expect(gml_vm_state_save(&vm,saved,size,&written) && written==size,"save pending queue") && ok;
  gml_fire_async_account_picker(&vm);
  ok=expect(gml_vm_state_load(&vm,saved,size,&used) && used==size &&
            vm.builtins->n_account_picker==3 && vm.builtins->async_seq==3,"restore pending identities") && ok;
  ok=expect(gml_vm_state_save(&vm,repeated,size,&written) && written==size &&
            !memcmp(saved,repeated,size),"pending state has canonical bytes") && ok;
  ok=expect(real_is(request(&vm,7,1),4),"restored cursor does not reuse identities") && ok;
  /* This new final section has a bounded count followed by request/controller pairs. */
  size_t tail=size-4-3*8;
  for(int defect=0;defect<7;defect++){
    memcpy(damaged,saved,size);size_t length=size;
    if(defect==0) word(damaged+tail,17);
    if(defect==1) word(damaged+tail,UINT32_MAX);
    if(defect==2) word(damaged+tail+4,0);
    if(defect==3) word(damaged+tail+4,4);
    if(defect==4) word(damaged+tail+12,1);
    if(defect==5) length--;
    if(defect==6) word(damaged+4,43);
    GmlVM rejected={0};
    if(gml_vm_init(&rejected,&win,NULL)){ok=0;continue;}
    /* Internal decode uses a disposable target; engine controls test public transactionality. */
    ok=expect(!gml_vm_state_load(&rejected,damaged,length,&used),"malformed queue/schema rejection") && ok;
    gml_vm_free(&rejected);
  }
  free(saved);free(repeated);free(damaged);gml_vm_free(&vm);return ok;
}
int main(void){
  int ok=1;
  for(int version=16;version<=17;version++){
    ok=deferred_capacity_and_reset(version) && ok;
    ok=pending_state_is_canonical_and_validated(version) && ok;
  }
  if(ok) puts("account picker ownership, bounds, reset and state: ok");
  return ok?0:1;
}
