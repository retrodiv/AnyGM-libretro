/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "gml_builtin_internal.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if(!(condition)){ \
  fprintf(stderr,"DS capacity check failed at line %d: %s\n",__LINE__,#condition); \
  exit(1); } } while(0)

static GmlVal invoke(GmlVM *vm,const char *name,GmlVal *args,int count){
  return gml_builtin_call(vm,name,args,count);
}
static GmlVal map_get(GmlVM *vm,GmlVal map,const char *key){
  GmlVal args[]={map,vstr(key)};
  return invoke(vm,"ds_map_find_value",args,2);
}
static GmlVal list_get(GmlVM *vm,GmlVal list,int at){
  GmlVal args[]={list,vreal(at)};
  return invoke(vm,"ds_list_find_value",args,2);
}
static void verify_tail(GmlVM *vm,GmlVal root,int count){
  CHECK(map_get(vm,root,"last").d==7654321);
  GmlVal nodes=map_get(vm,root,"nodes");
  CHECK(invoke(vm,"ds_list_size",&nodes,1).d==count);
  GmlVal tail=list_get(vm,nodes,count-1);
  CHECK(map_get(vm,tail,"number").d==count-1);
  GmlVal values=map_get(vm,tail,"values");
  CHECK(list_get(vm,values,0).d==count-1);
  GmlVal text=list_get(vm,values,1);
  CHECK(text.t==V_STR && !strcmp(text.s,"tail"));
}
int main(void){
  enum { count=2300 };
  GmlVM vm={0};
  GmlWin win={0};
  CHECK(gml_vm_init(&vm,&win,NULL)==0);
  GmlVal anchor=invoke(&vm,"ds_map_create",NULL,0);
  GmlVal list=invoke(&vm,"ds_list_create",NULL,0);
  GmlVal add[]={anchor,vstr("anchor"),vstr("retained")};
  invoke(&vm,"ds_map_add",add,3);
  GmlVal push[]={list,vreal(42)};
  invoke(&vm,"ds_list_add",push,2);
  GmlDSMap *map_address=ds_map_slot(&vm,(int)anchor.d);
  GmlDSList *list_address=ds_list_slot(&vm,(int)list.d);
  char *json=malloc((size_t)count*80+100);
  CHECK(json);
  size_t length=(size_t)sprintf(json,"{\"nodes\":[");
  for(int i=0;i<count;i++)
    length+=(size_t)sprintf(json+length,
      "%s{\"number\":%d,\"values\":[%d,\"tail\"]}",i?",":"",i,i);
  sprintf(json+length,"],\"last\":7654321}");
  GmlVal input=vstr(json);
  GmlVal root=invoke(&vm,"json_decode",&input,1);
  CHECK(root.t==V_REAL && root.d>0);
  free(json);
  verify_tail(&vm,root,count);
  CHECK(ds_map_slot(&vm,(int)anchor.d)==map_address);
  CHECK(ds_list_slot(&vm,(int)list.d)==list_address);
  CHECK(!strcmp(map_get(&vm,anchor,"anchor").s,"retained"));
  CHECK(list_get(&vm,list,0).d==42);
  CHECK(vm.builtins->ds_maps.live_count==count+2);
  CHECK(vm.builtins->ds_lists.live_count==count+2);
  size_t size=gml_vm_state_size(&vm),written=0,used=0;
  unsigned char *before=malloc(size),*after=malloc(size);
  CHECK(before && after);
  CHECK(gml_vm_state_save(&vm,before,size,&written) && written==size);
  invoke(&vm,"ds_map_destroy",&root,1);
  CHECK(vm.builtins->ds_maps.live_count==1 && vm.builtins->ds_lists.live_count==1);
  for(int i=0;i<count*2;i++) CHECK(invoke(&vm,"ds_map_create",NULL,0).d>0);
  CHECK(gml_vm_state_load(&vm,before,size,&used) && used==size);
  verify_tail(&vm,root,count);
  CHECK(gml_vm_state_save(&vm,after,size,&written) && written==size);
  CHECK(!memcmp(before,after,size));
  CHECK(!gml_vm_state_load(&vm,before,size-1,&used));
  /* The VM section is an in-place codec; the engine owns the enclosing rollback
   * transaction. Reapply the valid section before checking its canonical bytes. */
  CHECK(gml_vm_state_load(&vm,before,size,&used));
  verify_tail(&vm,root,count);
  CHECK(gml_vm_state_save(&vm,after,size,&written) && !memcmp(before,after,size));
  invoke(&vm,"ds_map_destroy",&root,1);
  CHECK(vm.builtins->ds_maps.live_count==1 && vm.builtins->ds_lists.live_count==1);
  /* Clearing a cycle preserves the root and releases its children. */
  GmlVal cycle=invoke(&vm,"ds_list_create",NULL,0);
  GmlVal child[]={anchor,vstr("cycle"),cycle};
  invoke(&vm,"ds_map_add_list",child,3);
  GmlVal back[]={cycle,anchor};
  invoke(&vm,"ds_list_add",back,2);
  GmlVal mark[]={cycle,vreal(0)};
  invoke(&vm,"ds_list_mark_as_map",mark,2);
  invoke(&vm,"ds_map_clear",&anchor,1);
  CHECK(vm.builtins->ds_maps.live_count==1 && vm.builtins->ds_lists.live_count==1);
  CHECK(ds_map_slot(&vm,(int)anchor.d));
  CHECK(!ds_list_slot(&vm,(int)cycle.d));
  GmlVal fresh=invoke(&vm,"ds_map_create",NULL,0);
  CHECK(fresh.d>cycle.d && fresh.d!=root.d);
  invoke(&vm,"ds_map_destroy",&fresh,1);
  vm.builtins->next_ds_id=INT_MAX;
  CHECK(invoke(&vm,"ds_map_create",NULL,0).d==-1);
  CHECK(invoke(&vm,"ds_list_create",NULL,0).d==-1);
  CHECK(vm.builtins->next_ds_id==INT_MAX);
  free(before);
  free(after);
  gml_vm_free(&vm);
  puts("DS capacity, stable addresses, destruction and canonical state: PASS");
  return 0;
}
