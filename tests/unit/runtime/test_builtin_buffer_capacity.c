/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "gml_builtin_internal.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if(!(condition)){ \
  fprintf(stderr,"buffer capacity check failed at line %d: %s\n",__LINE__,#condition); \
  exit(1); } } while(0)

static GmlVal call(GmlVM *vm,const char *name,GmlVal *args,int count){
  return gml_builtin_call(vm,name,args,count);
}
static double peek(GmlVM *vm,GmlVal buffer,int offset){
  GmlVal args[]={buffer,vreal(offset),vreal(1)};
  return call(vm,"buffer_peek",args,3).d;
}
int main(void){
  GmlVM vm={0}; GmlWin win={0};
  CHECK(gml_vm_init(&vm,&win,NULL)==0);
  GmlVal ids[96],size=vreal(32);
  GmlBuiltinBuffer *first=NULL;
  unsigned char *address=NULL;
  for(int i=0;i<96;i++){
    ids[i]=call(&vm,"buffer_create",&size,1);
    CHECK(ids[i].t==V_REAL && ids[i].d==i+1);
    GmlVal write[]={ids[i],vreal(1),vreal(i)};
    call(&vm,"buffer_write",write,3);
    if(!i){ first=buffer_at(&vm,vm_buffer_slot(&vm,1)); address=first->data; }
  }
  CHECK(first==buffer_at(&vm,vm_buffer_slot(&vm,1)) && first->data==address);
  CHECK(peek(&vm,ids[0],0)==0 && peek(&vm,ids[95],0)==95);
  GmlVal digest_args[]={ids[95],vreal(0),vreal(1)};
  GmlVal digest=call(&vm,"buffer_sha1",digest_args,3);
  CHECK(digest.t==V_STR && digest.s && strlen(digest.s)==40);
  if(digest.d!=0) free((void*)digest.s);
  GmlVal grow[]={ids[95],vreal(4097),vreal(1),vreal(201)};
  call(&vm,"buffer_poke",grow,4);
  CHECK(peek(&vm,ids[95],4097)==201 && peek(&vm,ids[95],4096)==0);
  CHECK(call(&vm,"buffer_tell",ids+95,1).d==1);
  for(int i=1;i<95;i+=2) call(&vm,"buffer_delete",ids+i,1);
  GmlVal replacement=call(&vm,"buffer_create",&size,1);
  CHECK(replacement.d==97 && call(&vm,"buffer_exists",ids+1,1).d==0);
  CHECK(first==buffer_at(&vm,vm_buffer_slot(&vm,1)) && first->data==address);
  size_t bytes=gml_vm_state_size(&vm),written=0,used=0;
  unsigned char *before=malloc(bytes),*after=malloc(bytes);
  CHECK(before && after && gml_vm_state_save(&vm,before,bytes,&written) && written==bytes);
  call(&vm,"buffer_delete",ids+95,1);
  for(int i=0;i<96;i++) CHECK(call(&vm,"buffer_create",&size,1).d>97);
  CHECK(gml_vm_state_load(&vm,before,bytes,&used) && used==bytes);
  CHECK(peek(&vm,ids[95],0)==95 && peek(&vm,ids[95],4097)==201);
  CHECK(call(&vm,"buffer_tell",ids+95,1).d==1);
  CHECK(call(&vm,"buffer_exists",ids+1,1).d==0);
  CHECK(gml_vm_state_save(&vm,after,bytes,&written) && written==bytes && !memcmp(before,after,bytes));
  GmlVal destination=call(&vm,"buffer_create",&size,1);
  CHECK(call(&vm,"game_save_buffer",&destination,1).d==1);
  double saved_size=call(&vm,"buffer_get_size",&destination,1).d;
  CHECK(call(&vm,"game_save_buffer",&destination,1).d==1);
  CHECK(call(&vm,"buffer_get_size",&destination,1).d==saved_size);
  call(&vm,"buffer_delete",ids+95,1);
  CHECK(call(&vm,"game_load_buffer",&destination,1).d==1);
  CHECK(peek(&vm,ids[95],4097)==201);
  CHECK(call(&vm,"buffer_exists",&destination,1).d==1);
  CHECK(call(&vm,"buffer_get_size",&destination,1).d==0);
  vm.builtins->next_buffer_id=65536;
  GmlVal high=call(&vm,"buffer_create",&size,1);
  CHECK(high.d==65536);
  CHECK(call(&vm,"buffer_get_address",&high,1).d!=call(&vm,"buffer_get_address",ids,1).d);
  vm.builtins->next_buffer_id=INT_MAX;
  CHECK(call(&vm,"buffer_create",&size,1).d==-1);
  gml_builtin_state_reset(vm.builtins);
  CHECK(call(&vm,"buffer_exists",&high,1).d==0);
  CHECK(call(&vm,"buffer_create",&size,1).d==1);
  free(before); free(after); gml_vm_free(&vm);
  puts("growing buffer handles, bytes, destruction and canonical restoration: ok");
  return 0;
}
