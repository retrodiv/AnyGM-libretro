/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Map assignment expression results retain the existing value-storage owner. */
#include "gml_builtin.h"
#include "anygm_test_runner.h"
#include <math.h>
#include <stdio.h>
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
int main(int argc,char **argv){
  const AnygmTestCase cases[]={{"pre",pre_case},{"post",post_case},{"strings",strings_case},
                               {"arrays",arrays_case},{"bounds",bounds_case}};
  const AnygmTestGroup group={"map_access",cases,sizeof cases/sizeof cases[0]};
  AnygmTestResult result={0}; const char *filter=argc==3&&!strcmp(argv[1],"--case")?argv[2]:NULL;
  int ok=anygm_test_run_groups(&group,1,filter,&result);
  printf("map access: %d passed, %d failed\n",result.passed,result.failed);
  return ok?0:1;
}
