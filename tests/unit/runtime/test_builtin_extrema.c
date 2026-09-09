/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "gml_builtin.h"
#include "anygm_test_runner.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int number_is(GmlVal value,double expected){
  return value.t==V_REAL && (isnan(expected)?isnan(value.d):
    value.d==expected && (expected!=0.0 || !!signbit(value.d)==!!signbit(expected)));
}
static GmlVal call(GmlVM *vm,const char *name,GmlVal *args,int count,int cached,int *ok){
  if(!cached) return gml_builtin_call(vm,name,args,count);
  int id=gml_builtin_fast_id(vm,name);
  if(id<0){ *ok=0; return vundef(); }
  return gml_builtin_call_fast_id(vm,id,name,args,count);
}
static int check(const double values[4],int count,double low,double high){
  int ok=1;
  for(int cached=0;cached<2;cached++){
    GmlVM vm={0};
    GmlVal args[4],before[4];
    for(int i=0;i<4;i++) args[i]=vreal(values[i]);
    memcpy(before,args,sizeof args);
    ok &= number_is(call(&vm,"min3",args,count,cached,&ok),low);
    ok &= number_is(call(&vm,"max3",args,count,cached,&ok),high);
    ok &= !memcmp(before,args,sizeof args);
    gml_vm_free(&vm);
  }
  return ok;
}
static int permutations_case(void){
  const double vectors[][4]={{-7,2.5,11,0},{-7,11,2.5,0},{2.5,-7,11,0},
    {2.5,11,-7,0},{11,-7,2.5,0},{11,2.5,-7,0}};
  int ok=1;
  for(unsigned i=0;i<sizeof vectors/sizeof vectors[0];i++)
    ok &= check(vectors[i],3,-7,11);
  const double equal[]={4,4,4,0};
  return check(equal,3,4,4) && ok;
}
static int fixed_arity_case(void){
  const double lower[]={3,7,5,-99}, upper[]={3,7,5,99};
  int ok=check(lower,4,3,7) & check(upper,4,3,7);
  for(int cached=0;cached<2;cached++){
    GmlVM vm={0};
    GmlVal args[]={vreal(3),vreal(7),vreal(5),vreal(-99)};
    ok &= number_is(call(&vm,"min",args,4,cached,&ok),-99);
    args[3]=vreal(99);
    ok &= number_is(call(&vm,"max",args,4,cached,&ok),99);
    gml_vm_free(&vm);
  }
  return ok;
}
static int exceptional_numbers_case(void){
  const double first_nan[]={NAN,2,3,0}, middle_nan[]={2,NAN,3,0}, last_nan[]={2,3,NAN,0};
  const double negative_zero[]={-0.0,0.0,0.0,0}, positive_zero[]={0.0,-0.0,-0.0,0};
  const double infinities[]={-INFINITY,0,INFINITY,0};
  return check(first_nan,3,NAN,NAN) & check(middle_nan,3,2,3) & check(last_nan,3,2,3) &
    check(negative_zero,3,-0.0,-0.0) & check(positive_zero,3,0.0,0.0) &
    check(infinities,3,-INFINITY,INFINITY);
}
/* Defensive policy, not an assertion about an original runner's argument error. */
static int missing_arguments_case(void){
  int ok=1;
  for(int cached=0;cached<2;cached++){
    GmlVM vm={0}; GmlVal args[]={vreal(19),vreal(23)};
    for(int count=0;count<3;count++){
      ok &= number_is(call(&vm,"min3",count?args:NULL,count,cached,&ok),0);
      ok &= number_is(call(&vm,"max3",count?args:NULL,count,cached,&ok),0);
    }
    gml_vm_free(&vm);
  }
  return ok;
}
int main(void){
  const AnygmTestCase cases[]={{"argument_permutations",permutations_case},
    {"fixed_three_argument_boundary",fixed_arity_case},
    {"exceptional_number_order",exceptional_numbers_case},
    {"defensive_missing_arguments",missing_arguments_case}};
  const AnygmTestGroup group={"extrema",cases,sizeof cases/sizeof cases[0]};
  AnygmTestResult result;
  anygm_test_run_groups(&group,1,NULL,&result);
  printf("extrema: passed=%d failed=%d\n",result.passed,result.failed);
  return result.failed?EXIT_FAILURE:EXIT_SUCCESS;
}
