/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "gml_builtin.h"
#include "anygm_test_runner.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int hash_is(GmlVal value,const char *expected){
  int ok=value.t==V_STR && value.s && !strcmp(value.s,expected);
  if(!ok) fprintf(stderr,"buffer SHA-1 did not match %s\n",expected);
  gml_values_release(&value,1);
  return ok;
}
static GmlVal query(GmlVM *vm,GmlVal *args,int n,int cached,int *ok){
  if(!cached) return gml_builtin_call(vm,"buffer_sha1",args,n);
  int id=gml_builtin_fast_id(vm,"buffer_sha1");
  if(id<0){ *ok=0; return vundef(); }
  return gml_builtin_call_fast_id(vm,id,"buffer_sha1",args,n);
}
static int vectors_case(void){
  int ok=1;
  for(int cached=0;cached<2;cached++){
    GmlVM vm={0};
    GmlVal create[]={vreal(256),vreal(0),vreal(1)};
    GmlVal buffer=gml_builtin_call(&vm,"buffer_create",create,3);
    for(int i=0;i<256;i++){
      GmlVal poke[]={buffer,vreal(i),vreal(1),vreal(i)};
      gml_builtin_call(&vm,"buffer_poke",poke,4);
    }
    GmlVal seek[]={buffer,vreal(0),vreal(31)};
    gml_builtin_call(&vm,"buffer_seek",seek,3);
    GmlVal args[]={buffer,vreal(0),vreal(256)};
    ok &= hash_is(query(&vm,args,3,cached,&ok),"4916d6bdb7f78e6803698cab32d1586ea457dfc8");
    args[1]=vreal(97); args[2]=vreal(3);
    ok &= hash_is(query(&vm,args,3,cached,&ok),"a9993e364706816aba3e25717850c26c9cd0d89d");
    args[1]=vreal(256); args[2]=vreal(0);
    ok &= hash_is(query(&vm,args,3,cached,&ok),"da39a3ee5e6b4b0d3255bfef95601890afd80709");
    GmlVal position=gml_builtin_call(&vm,"buffer_tell",&buffer,1);
    ok &= position.t==V_REAL && position.d==31;
    GmlVal peek[]={buffer,vreal(255),vreal(1)};
    GmlVal byte=gml_builtin_call(&vm,"buffer_peek",peek,3);
    ok &= byte.t==V_REAL && byte.d==255;
    gml_vm_free(&vm);
  }
  return ok;
}
/* Invalid ranges do not silently publish the digest of a different byte range. */
static int invalid_case(void){
  int ok=1;
  GmlVM vm={0};
  GmlVal create[]={vreal(8),vreal(0),vreal(1)};
  GmlVal buffer=gml_builtin_call(&vm,"buffer_create",create,3);
  for(int cached=0;cached<2;cached++){
    GmlVal args[]={buffer,vreal(0),vreal(1)};
    ok &= hash_is(query(&vm,args,2,cached,&ok),"");
    double invalid[]={-1,9,1e100,NAN,INFINITY};
    for(int i=0;i<5;i++){
      args[1]=vreal(invalid[i]); args[2]=vreal(1);
      ok &= hash_is(query(&vm,args,3,cached,&ok),"");
      args[1]=vreal(0); args[2]=vreal(invalid[i]);
      ok &= hash_is(query(&vm,args,3,cached,&ok),"");
    }
    args[0]=vreal(-1); args[1]=vreal(0); args[2]=vreal(0);
    ok &= hash_is(query(&vm,args,3,cached,&ok),"");
  }
  gml_vm_free(&vm);
  return ok;
}
int main(void){
  static const AnygmTestCase cases[]={{"known_binary_and_subrange",vectors_case},
                                      {"invalid_ranges",invalid_case}};
  const AnygmTestGroup group={"buffer_hash",cases,sizeof cases/sizeof cases[0]};
  AnygmTestResult result;
  anygm_test_run_groups(&group,1,NULL,&result);
  printf("buffer hash: passed=%d failed=%d\n",result.passed,result.failed);
  return result.failed?EXIT_FAILURE:EXIT_SUCCESS;
}
