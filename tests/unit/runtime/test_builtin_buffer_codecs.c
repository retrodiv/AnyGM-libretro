/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "gml_builtin.h"
#include "anygm_test_runner.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static GmlVal call(GmlVM *vm,const char *name,GmlVal *args,int n,int cached){
  if(!cached) return gml_builtin_call(vm,name,args,n);
  int id=gml_builtin_fast_id(vm,name);
  return id<0?vundef():gml_builtin_call_fast_id(vm,id,name,args,n);
}
static int widths(void){
  int ok=1; GmlVM vm={0};
  const int expected[]={0,1,1,2,2,4,4,2,4,8,1,0,8,0};
  for(int cached=0;cached<2;cached++){
    for(int type=0;type<14;type++){
      GmlVal arg=vreal(type),value=call(&vm,"buffer_sizeof",&arg,1,cached);
      ok &= value.t==V_REAL && value.d==expected[type];
    }
    double invalid[]={-1,14,1.5,NAN,INFINITY,1e100};
    for(int k=0;k<6;k++){
      GmlVal arg=vreal(invalid[k]);
      ok &= call(&vm,"buffer_sizeof",&arg,1,cached).d==0;
    }
    ok &= call(&vm,"buffer_sizeof",NULL,0,cached).d==0;
  }
  gml_vm_free(&vm); return ok;
}
static int codecs(void){
  int ok=1;
  for(int cached=0;cached<2;cached++){
    GmlVM vm={0}; GmlVal create[]={vreal(256),vreal(0),vreal(1)};
    GmlVal source=call(&vm,"buffer_create",create,3,cached);
    for(int k=0;k<256;k++){
      GmlVal poke[]={source,vreal(k),vreal(1),vreal(k)};
      call(&vm,"buffer_poke",poke,4,cached);
    }
    GmlVal seek[]={source,vreal(0),vreal(31)};
    call(&vm,"buffer_seek",seek,3,cached);
    GmlVal args[]={source,vreal(17),vreal(113)};
    GmlVal packed=call(&vm,"buffer_compress",args,3,cached);
    GmlVal unpacked=call(&vm,"buffer_decompress",&packed,1,cached);
    ok &= packed.t==V_REAL && packed.d>0 && packed.d!=source.d &&
          unpacked.t==V_REAL && unpacked.d>0 && unpacked.d!=packed.d;
    ok &= call(&vm,"buffer_get_size",&unpacked,1,cached).d==113;
    ok &= call(&vm,"buffer_tell",&unpacked,1,cached).d==0;
    ok &= call(&vm,"buffer_tell",&source,1,cached).d==31;
    for(int k=0;k<256;k++){
      GmlVal peek[]={source,vreal(k),vreal(1)};
      ok &= call(&vm,"buffer_peek",peek,3,cached).d==k;
      if(k<113){ peek[0]=unpacked; peek[1]=vreal(k);
        ok &= call(&vm,"buffer_peek",peek,3,cached).d==k+17; }
    }
    args[1]=vreal(256); args[2]=vreal(0);
    GmlVal empty=call(&vm,"buffer_compress",args,3,cached);
    GmlVal decoded_empty=call(&vm,"buffer_decompress",&empty,1,cached);
    ok &= decoded_empty.d>0 && call(&vm,"buffer_get_size",&decoded_empty,1,cached).d==0;
    ok &= call(&vm,"buffer_decompress",&source,1,cached).d==-1;
    GmlVal resize[]={packed,vreal(3)};
    call(&vm,"buffer_resize",resize,2,cached);
    ok &= call(&vm,"buffer_decompress",&packed,1,cached).d==-1;
    double invalid[]={-1,0,999,1.5,NAN,INFINITY,1e100};
    for(int k=0;k<7;k++){
      GmlVal arg=vreal(invalid[k]);
      ok &= call(&vm,"buffer_decompress",&arg,1,cached).d==-1;
    }
    args[1]=vreal(255); args[2]=vreal(2);
    ok &= call(&vm,"buffer_compress",args,3,cached).d==-1;
    ok &= call(&vm,"buffer_compress",args,2,cached).d==-1;
    call(&vm,"buffer_delete",&empty,1,cached);
    ok &= call(&vm,"buffer_decompress",&empty,1,cached).d==-1;
    ok &= call(&vm,"buffer_decompress",NULL,0,cached).d==-1;
    gml_vm_free(&vm);
  }
  return ok;
}
int main(void){
  static const AnygmTestCase cases[]={{"fixed_width_types",widths},{"zlib_buffer_ownership",codecs}};
  const AnygmTestGroup group={"buffer_codecs",cases,2}; AnygmTestResult result;
  anygm_test_run_groups(&group,1,NULL,&result);
  return result.failed?EXIT_FAILURE:EXIT_SUCCESS;
}
