/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Map assignment expression results retain the existing value-storage owner. */
#include "gml_builtin.h"
#include "gml_builtin_internal.h"
#include "anygm_test_runner.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int expect(int condition,const char *message){
  if(!condition) fprintf(stderr,"%s\n",message);
  return condition;
}
static int real_is(GmlVal value,double number){return value.t==V_REAL && value.d==number;}
static int string_is(GmlVal value,const char *text){
  return value.t==V_STR && value.s && !strcmp(value.s,text);
}
static GmlVal call(GmlVM *vm,const char *name,GmlVal *args,int count,int cached,int *ok){
  if(!cached) return gml_builtin_call(vm,name,args,count);
  int id=gml_builtin_fast_id(vm,name);
  *ok &= expect(id>=0,"map assignment has an exact cached identity");
  return id>=0?gml_builtin_call_fast_id(vm,id,name,args,count):vundef();
}
static int pre_case(void){
  int ok=1;
  for(int cached=0;cached<2;cached++){
    GmlVM vm={0}; GmlVal map=gml_builtin_call(&vm,"ds_map_create",NULL,0);
    GmlVal args[]={map,vreal(7),vreal(12)},lookup[]={map,vreal(7)};
    ok &= expect(real_is(call(&vm,"ds_map_set_pre",args,3,cached,&ok),12),"pre insertion returns the supplied value");
    ok &= expect(real_is(gml_builtin_call(&vm,"ds_map_find_value",lookup,2),12),"pre insertion retains the value");
    args[2]=vreal(-5.5);
    ok &= expect(real_is(call(&vm,"ds_map_set_pre",args,3,cached,&ok),-5.5),"pre replacement returns the new value");
    ok &= expect(real_is(gml_builtin_call(&vm,"ds_map_find_value",lookup,2),-5.5) &&
                 real_is(gml_builtin_call(&vm,"ds_map_size",&map,1),1),"pre replacement keeps one map entry");
    args[2]=vundef();
    ok &= expect(call(&vm,"ds_map_set_pre",args,3,cached,&ok).t==V_UNDEF &&
                 gml_builtin_call(&vm,"ds_map_find_value",lookup,2).t==V_UNDEF &&
                 real_is(gml_builtin_call(&vm,"ds_map_exists",lookup,2),1),"undefined is a stored value, not deletion");
    gml_vm_free(&vm);
  }
  return ok;
}
static int post_case(void){
  int ok=1;
  for(int cached=0;cached<2;cached++){
    GmlVM vm={0}; GmlVal map=gml_builtin_call(&vm,"ds_map_create",NULL,0);
    GmlVal args[]={map,vreal(3),vreal(17)},lookup[]={map,vreal(3)};
    gml_builtin_call(&vm,"ds_map_set",args,3); args[2]=vreal(29);
    ok &= expect(real_is(call(&vm,"ds_map_set_post",args,3,cached,&ok),17),"post replacement returns the previous value");
    ok &= expect(real_is(gml_builtin_call(&vm,"ds_map_find_value",lookup,2),29),"post replacement still retains the new value");
    args[1]=vreal(4); lookup[1]=vreal(4); args[2]=vreal(31);
    ok &= expect(call(&vm,"ds_map_set_post",args,3,cached,&ok).t==V_UNDEF,"post insertion returns undefined for an absent key");
    ok &= expect(real_is(gml_builtin_call(&vm,"ds_map_find_value",lookup,2),31),"post insertion retains the supplied value");
    args[2]=vundef();
    ok &= expect(real_is(call(&vm,"ds_map_set_post",args,3,cached,&ok),31) &&
                 gml_builtin_call(&vm,"ds_map_find_value",lookup,2).t==V_UNDEF,"post keeps undefined distinct from its old value");
    gml_vm_free(&vm);
  }
  return ok;
}
static int strings_case(void){
  int ok=1;
  for(int cached=0;cached<2;cached++){
    GmlVM vm={0}; GmlVal map=gml_builtin_call(&vm,"ds_map_create",NULL,0);
    GmlVal args[]={map,vreal(1),vstr("before")},lookup[]={map,vreal(1)};
    gml_builtin_call(&vm,"ds_map_set",args,3);
    GmlVal owned[2]={gml_builtin_call(&vm,"ds_map_find_value",lookup,2),vundef()};
    args[2]=vstr("after");
    GmlVal previous=call(&vm,"ds_map_set_post",args,3,cached,&ok);
    owned[1]=gml_builtin_call(&vm,"ds_map_find_value",lookup,2);
    ok &= expect(string_is(previous,"before") && previous.d==0 && string_is(owned[1],"after"),
                 "post returns a non-owning prior string without changing its contents");
    gml_builtin_call(&vm,"ds_map_destroy",&map,1);
    ok &= expect(string_is(previous,"before") && string_is(owned[1],"after"),
                 "published map strings remain readable after map destruction");
    gml_vm_free(&vm);
    /* This fixture retains the only remaining references after the final check. */
    for(int i=0;i<2;i++) if(owned[i].t==V_STR) owned[i].d=1;
    gml_values_release_owned(owned,2);
  }
  return ok;
}
static int arrays_case(void){
  int ok=1;
  for(int cached=0;cached<2;cached++){
    GmlVM vm={0}; GmlVal map=gml_builtin_call(&vm,"ds_map_create",NULL,0);
    GmlVal retained[]={gml_arr_new(1,vreal(0)),gml_arr_new(1,vreal(0))};
    gml_arr_set(retained[0],0,vreal(11)); gml_arr_set(retained[1],0,vreal(22));
    GmlVal args[]={map,vreal(2),retained[0]},lookup[]={map,vreal(2)};
    GmlVal inserted=call(&vm,"ds_map_set_pre",args,3,cached,&ok);
    ok &= expect(inserted.t==V_ARR && inserted.arr==retained[0].arr,"pre preserves the supplied array identity");
    args[2]=retained[1]; GmlVal previous=call(&vm,"ds_map_set_post",args,3,cached,&ok);
    GmlVal current=gml_builtin_call(&vm,"ds_map_find_value",lookup,2);
    ok &= expect(previous.t==V_ARR && previous.arr==retained[0].arr &&
                 current.t==V_ARR && current.arr==retained[1].arr,"post returns the previous array while retaining the new array");
    gml_arr_set(retained[0],0,vreal(33));
    ok &= expect(real_is(gml_arr_get(retained[0],0),33) && real_is(gml_arr_get(current,0),22),
                 "old and new array mutations remain independent");
    gml_vm_free(&vm); gml_values_release_owned(retained,2);
  }
  return ok;
}
static int bounds_case(void){
  const double bad[]={-1,2147483648.0,NAN,INFINITY,-INFINITY,1e100};
  const char *names[]={"ds_map_set_pre","ds_map_set_post"}; int ok=1;
  for(int cached=0;cached<2;cached++) for(int operation=0;operation<2;operation++){
    GmlVM vm={0},other={0}; GmlVal map=gml_builtin_call(&vm,"ds_map_create",NULL,0);
    GmlVal second=gml_builtin_call(&other,"ds_map_create",NULL,0);
    GmlVal args[]={map,vreal(1),vreal(71)},lookup[]={map,vreal(1)};
    gml_builtin_call(&vm,"ds_map_set",args,3);
    for(size_t i=0;i<sizeof bad/sizeof bad[0];i++){
      args[0]=vreal(bad[i]); args[2]=vreal(91);
      ok &= expect(real_is(call(&vm,names[operation],args,3,cached,&ok),91),"invalid map returns the supplied expression value");
      ok &= expect(real_is(gml_builtin_call(&vm,"ds_map_find_value",lookup,2),71),"invalid map cannot mutate an existing map");
    }
    args[0]=map;
    for(int n=0;n<3;n++) ok &= expect(call(&vm,names[operation],args,n,cached,&ok).t==V_UNDEF,
                                    "missing assignment arguments are a defensive undefined no-op");
    ok &= expect(real_is(gml_builtin_call(&other,"ds_map_size",&second,1),0),"another engine's map is unchanged");
    gml_vm_free(&vm); gml_vm_free(&other);
  }
  return ok;
}
static int static_keys_case(void){
  static const char *const keys[]={"key-00","key-01","key-02","key-03",
    "key-04","key-05","key-06","key-07","key-08","key-09","key-10","key-11",
    "key-12","key-13","key-14","key-15"};
  GmlVM vm={0},other={0}; GmlWin win={0}; AnygmHostServices host={0};
  host.struct_size=sizeof host; host.abi_version=ANYGM_HOST_SERVICES_VERSION;
  int initialized=gml_vm_init(&vm,&win,&host)==0 && gml_vm_init(&other,&win,&host)==0;
  int ok=expect(initialized,"static-key fixture initializes both VMs");
  GmlVal retained[16]={0}; unsigned char *state=NULL,*control=NULL;
  if(!initialized) goto cleanup;
  int map=ds_map_create_id(&vm),copy=ds_map_create_id(&other);
  for(int i=0;i<16;i++){
    ok &= expect(ds_map_put_static_key(&vm,map,keys[i],vreal(i),1),"insert permanent metadata key");
    ok &= expect(ds_map_put(&other,copy,vstr(keys[i]),vreal(i),1),"insert ordinary cloned control key");
  }
  GmlDSMap *stored=ds_map_slot(&vm,map),*cloned=ds_map_slot(&other,copy);
  for(int i=0;i<16;i++){
    ok &= expect(stored && stored->len==16 && stored->entry[i].key_val.s==keys[i] &&
      stored->entry[i].key_val.d==0,"permanent key uses its stable non-owned reference");
    if(cloned && cloned->len==16) retained[i]=cloned->entry[i].key_val;
    int found=0;
    ok &= expect(real_is(ds_map_lookup_s(&vm,map,keys[i],&found),i) && found,
                 "static-key lookup survives index growth");
  }
  size_t size=gml_vm_state_size(&vm),control_size=gml_vm_state_size(&other),written=0,control_written=0;
  state=malloc(size?size:1); control=malloc(control_size?control_size:1);
  ok &= expect(size>0 && size==control_size && state && control &&
    gml_vm_state_save(&vm,state,size,&written) && gml_vm_state_save(&other,control,control_size,&control_written) &&
    written==size && control_written==size && !memcmp(state,control,size),
    "key storage lifetime does not alter canonical state bytes");
  ok &= expect(ds_map_put_static_key(&vm,map,keys[0],vreal(91),0),"keep existing entry without overwrite");
  int found=0;
  ok &= expect(real_is(ds_map_lookup_s(&vm,map,keys[0],&found),0) && found,"non-overwrite preserves the old value");
  ok &= expect(ds_map_put_static_key(&vm,map,keys[0],vreal(91),1) &&
    real_is(ds_map_lookup_s(&vm,map,keys[0],&found),91),"overwrite retains the original insertion rule");
  GmlVal escaped=gml_builtin_call(&vm,"ds_map_find_first",(GmlVal[]){vreal(map)},1);
  ds_map_destroy_id(&vm,map);
  ok &= expect(string_is(escaped,keys[0]) && escaped.s==keys[0] && escaped.d==0,
               "an escaped permanent key remains valid after destruction");
  ok &= expect(!ds_map_put_static_key(&vm,map,keys[0],vreal(1),1) &&
    !ds_map_put_static_key(&other,copy,NULL,vreal(1),1),"dead map and null permanent key are rejected");
cleanup:
  free(state); free(control); gml_vm_free(&vm); gml_vm_free(&other);
  gml_values_release_owned(retained,16);
  return ok;
}
int main(int argc,char **argv){
  const AnygmTestCase cases[]={{"pre",pre_case},{"post",post_case},{"strings",strings_case},
                               {"arrays",arrays_case},{"bounds",bounds_case},{"static_keys",static_keys_case}};
  const AnygmTestGroup group={"map_access",cases,sizeof cases/sizeof cases[0]};
  AnygmTestResult result={0}; const char *filter=argc==3&&!strcmp(argv[1],"--case")?argv[2]:NULL;
  int ok=anygm_test_run_groups(&group,1,filter,&result);
  printf("map access: %d passed, %d failed\n",result.passed,result.failed);
  return ok?0:1;
}
