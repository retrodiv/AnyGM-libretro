/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Foreign byte vectors and transactional sequence-text contracts. */
#include "gml_builtin.h"
#include "anygm_test_runner.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int expect(int condition,const char *message){
  if(!condition) fprintf(stderr,"%s\n",message);
  return condition;
}
static int real_is(GmlVal v,double n){ return v.t==V_REAL && v.d==n; }
static int string_is(GmlVal v,const char *s){ return v.t==V_STR && v.s && !strcmp(v.s,s); }
static int hex_is(GmlVal v,const char *s){
  if(v.t!=V_STR || !v.s || strlen(v.s)!=strlen(s)) return 0;
  for(size_t i=0;s[i];i++) if(tolower((unsigned char)v.s[i])!=s[i]) return 0;
  return 1;
}
static GmlVal call(GmlVM *vm,const char *name,GmlVal *args,int n,int cached,int *ok){
  if(!cached) return gml_builtin_call(vm,name,args,n);
  int id=gml_builtin_fast_id(vm,name);
  *ok &= expect(id>=0,"sequence codec must have an exact cached ID");
  return id>=0?gml_builtin_call_fast_id(vm,id,name,args,n):vundef();
}
static const char *scalar_hex[]={
  "cb000000050000000000000005000000000000000000000000001c40010000000400000074657874"
  "0000000000000000000004c00100000000000000000000000000000000000080",
  "6700000005000000000000000000000000001c40010000000400000074657874"
  "0000000000000000000004c00100000000000000000000000000000000000080"
};
static const char *empty_hex[]={"cb000000000000000000000000000000","6700000000000000"};
static const char *nested_hex[]={
  "cb000000040000000000000004000000020000000300000000000000000000000000f03f02000000"
  "0200000001000000060000006e6573746564050000000200000000000000050000000d000000000000"
  "000000f03f0d0000000000000000000000",
  "6700000004000000020000000300000000000000000000000000f03f020000000200000001000000"
  "060000006e6573746564050000000200000000000000050000000d000000000000000000f03f0d0000"
  "000000000000000000"
};
static int scalar_case(void){
  int ok=1;
  for(int stack=0;stack<2;stack++) for(int cached=0;cached<2;cached++){
    GmlVM vm={0};
    const char *create=stack?"ds_stack_create":"ds_queue_create";
    const char *push=stack?"ds_stack_push":"ds_queue_enqueue";
    const char *size=stack?"ds_stack_size":"ds_queue_size";
    const char *pop=stack?"ds_stack_pop":"ds_queue_dequeue";
    const char *read=stack?"ds_stack_read":"ds_queue_read";
    const char *write=stack?"ds_stack_write":"ds_queue_write";
    GmlVal source=gml_builtin_call(&vm,create,NULL,0);
    GmlVal target=gml_builtin_call(&vm,create,NULL,0);
    GmlVal values[]={source,vreal(7),vstr("text"),vreal(-2.5),vstr(""),vreal(-0.0)};
    GmlVal old[]={target,vreal(99),vreal(100)};
    gml_builtin_call(&vm,push,values,6); gml_builtin_call(&vm,push,old,3);
    GmlVal encoded=call(&vm,write,&source,1,cached,&ok);
    ok &= expect(hex_is(encoded,scalar_hex[stack]),"writer emits the independently verified scalar bytes");
    GmlVal input[]={target,vstr(scalar_hex[stack])};
    call(&vm,read,input,2,cached,&ok);
    ok &= expect(real_is(gml_builtin_call(&vm,size,&target,1),5),"foreign read replaces old contents");
    GmlVal consumed[5];
    for(int i=0;i<5;i++){
      consumed[i]=gml_builtin_call(&vm,pop,&target,1);
      GmlVal expected=values[1+(stack?4-i:i)];
      ok &= expect(expected.t==V_STR?string_is(consumed[i],expected.s):
                   real_is(consumed[i],expected.d),"foreign read preserves FIFO or LIFO values");
      if(expected.t==V_REAL && expected.d==0.0)
        ok &= expect(consumed[i].t==V_REAL && signbit(consumed[i].d),"negative zero retains its bit");
      /* These popped strings have no remaining DS or VM reference in this fixture. */
      if(consumed[i].t==V_STR) consumed[i].d=1;
    }
    ok &= expect(real_is(gml_builtin_call(&vm,size,&source,1),5),"write does not consume its source");
    input[1]=vstr(empty_hex[stack]); call(&vm,read,input,2,cached,&ok);
    GmlVal empty=call(&vm,write,&target,1,cached,&ok);
    ok &= expect(hex_is(empty,empty_hex[stack]),"empty sequences have a valid typed envelope");
    gml_values_release(consumed,5); gml_values_release(&encoded,1); gml_values_release(&empty,1);
    gml_vm_free(&vm);
  }
  return ok;
}
static int arrays_case(void){
  int ok=1;
  for(int stack=0;stack<2;stack++) for(int cached=0;cached<2;cached++){
    GmlVM vm={0};
    GmlVal id=gml_builtin_call(&vm,stack?"ds_stack_create":"ds_queue_create",NULL,0);
    GmlVal input[]={id,vstr(nested_hex[stack])};
    call(&vm,stack?"ds_stack_read":"ds_queue_read",input,2,cached,&ok);
    ok &= expect(real_is(gml_builtin_call(&vm,"ds_list_size",&id,1),4),"nested input retains four entries");
    GmlVal at[]={id,vreal(0)};
    GmlVal outer=gml_builtin_call(&vm,"ds_list_find_value",at,2);
    GmlVal inner=gml_arr_get(outer,1);
    ok &= expect(gml_val_array_length(outer)==3 && real_is(gml_arr_get(outer,0),1) &&
                 gml_val_array_length(inner)==2 && string_is(gml_arr_get(inner,0),"nested") &&
                 gml_arr_get(inner,1).t==V_UNDEF && gml_arr_get(outer,2).t==V_ARR,
                 "nested arrays and undefined values decode recursively");
    at[1]=vreal(1); ok &= expect(gml_builtin_call(&vm,"ds_list_find_value",at,2).t==V_UNDEF,
                               "top-level undefined is not converted to zero");
    for(int i=2;i<4;i++){
      at[1]=vreal(i);
      ok &= expect(real_is(gml_builtin_call(&vm,"ds_list_find_value",at,2),i==2),
                   "encoded booleans normalize to the existing real representation");
    }
    GmlVal encoded=call(&vm,stack?"ds_stack_write":"ds_queue_write",&id,1,cached,&ok);
    GmlVal target=gml_builtin_call(&vm,"ds_queue_create",NULL,0);
    /* The matching kind is required even though both share one storage owner. */
    GmlVal again[]={target,encoded};
    call(&vm,stack?"ds_stack_read":"ds_queue_read",again,2,cached,&ok);
    at[0]=target; at[1]=vreal(0);
    GmlVal copied=gml_builtin_call(&vm,"ds_list_find_value",at,2);
    ok &= expect(copied.t==V_ARR && copied.arr!=outer.arr &&
                 string_is(gml_arr_get(gml_arr_get(copied,1),0),"nested"),
                 "writing nested arrays produces an independently allocated readable tree");
    gml_values_release(&encoded,1); gml_vm_free(&vm);
  }
  return ok;
}
static int legacy_case(void){
  const char *vectors[]={
    "660000000100000002000000010000000200000000000000000000000000f03f010000000100000078",
    "660000000100000002000000020000000100000000000000000000000000f03f01000000010000000100000078"
  };
  int ok=1;
  for(int rows=0;rows<2;rows++){
    GmlVM vm={0}; GmlVal id=gml_builtin_call(&vm,"ds_stack_create",NULL,0);
    GmlVal input[]={id,vstr(vectors[rows])};
    call(&vm,"ds_stack_read",input,2,rows,&ok);
    GmlVal a=gml_builtin_call(&vm,"ds_stack_top",&id,1);
    ok &= expect(gml_val_array_length(a)==2 &&
                 real_is(rows?gml_arr_get(gml_arr_get(a,0),0):gml_arr_get(a,0),1) &&
                 string_is(rows?gml_arr_get(gml_arr_get(a,1),0):gml_arr_get(a,1),"x"),
                 "legacy row counts distinguish flat and nested arrays");
    gml_vm_free(&vm);
  }
  GmlVM vm={0}; GmlVal id=gml_builtin_call(&vm,"ds_queue_create",NULL,0);
  GmlVal input[]={id,vstr("cb00000003000000010000000200000000000000000000000000f03f"
                        "000000000000000000000040000000000000000000000840")};
  call(&vm,"ds_queue_read",input,2,1,&ok);
  ok &= expect(real_is(gml_builtin_call(&vm,"ds_queue_size",&id,1),2) &&
               real_is(gml_builtin_call(&vm,"ds_queue_head",&id,1),2) &&
               real_is(gml_builtin_call(&vm,"ds_queue_tail",&id,1),3),
               "queue first offset discards only the consumed prefix");
  gml_vm_free(&vm); return ok;
}
static int rejection_case(void){
  int ok=1;
  const char *bad[]={"", "no", "67000000ffffffff", "6700000001000000050000",
    "670000000100000001000000ffffffff", "6700000001000000fe000000",
    "67000000010000000100000003000000610062", "670000000000000000",
    "6500000000000000", "6700000001000000030000000000000000000000",
    "670000000200000001000000040000006c65616bfe000000",
    "67000000010000000a0000000100000000002000"};
  GmlVM vm={0}; GmlVal id=gml_builtin_call(&vm,"ds_stack_create",NULL,0);
  GmlVal seed[]={id,vreal(42)}; gml_builtin_call(&vm,"ds_stack_push",seed,2);
  for(size_t i=0;i<sizeof bad/sizeof bad[0];i++){
    GmlVal input[]={id,vstr(bad[i])}; call(&vm,"ds_stack_read",input,2,(int)(i&1),&ok);
    ok &= expect(real_is(gml_builtin_call(&vm,"ds_stack_size",&id,1),1) &&
                 real_is(gml_builtin_call(&vm,"ds_stack_top",&id,1),42),
                 "invalid, unsupported and trailing bytes leave the destination unchanged");
  }
  for(size_t length=0;length<strlen(scalar_hex[1]);length++){
    char text[256]; memcpy(text,scalar_hex[1],length); text[length]=0;
    GmlVal input[]={id,vstr(text)}; call(&vm,"ds_stack_read",input,2,0,&ok);
    ok &= expect(real_is(gml_builtin_call(&vm,"ds_stack_top",&id,1),42),
                 "every scalar-vector truncation is transactional");
  }
  GmlVal invalid[]={vreal(NAN),vreal(INFINITY),vreal(-1),vstr("invalid")};
  for(size_t i=0;i<sizeof invalid/sizeof invalid[0];i++){
    GmlVal out=call(&vm,"ds_stack_write",&invalid[i],1,0,&ok);
    ok &= expect(out.t==V_UNDEF,"invalid writer handle returns explicit failure, not fake data");
    gml_values_release(&out,1);
  }
  gml_vm_free(&vm); return ok;
}
/* Defensive value-model and byte-preservation policies, not native type fidelity. */
static int numeric_and_text_case(void){
  const char *numbers[]={
    "670000000100000007000000ffffffff",
    "67000000010000000a0000000100000001000000",
    "67000000010000000a0000000000000000000080",
    "670000000100000000000000000000000000f07f",
    "670000000100000000000000000000000000f87f"
  };
  const double expected[]={-1,4294967297.0,-9223372036854775808.0,INFINITY,NAN};
  int ok=1;
  for(size_t i=0;i<sizeof numbers/sizeof numbers[0];i++){
    GmlVM vm={0}; GmlVal id=gml_builtin_call(&vm,"ds_stack_create",NULL,0);
    GmlVal input[]={id,vstr(numbers[i])}; call(&vm,"ds_stack_read",input,2,1,&ok);
    GmlVal value=gml_builtin_call(&vm,"ds_stack_top",&id,1);
    ok &= expect(value.t==V_REAL && (isnan(expected[i])?isnan(value.d):value.d==expected[i]),
                 "integer normalization is exact and nonfinite real bits survive");
    gml_vm_free(&vm);
  }
  const char *bytes="67000000020000000100000005000000c3a1e6b0b40100000004000000f09f9880";
  GmlVM vm={0}; GmlVal id=gml_builtin_call(&vm,"ds_stack_create",NULL,0);
  GmlVal input[]={id,vstr(bytes)}; call(&vm,"ds_stack_read",input,2,0,&ok);
  GmlVal encoded=call(&vm,"ds_stack_write",&id,1,1,&ok);
  ok &= expect(hex_is(encoded,bytes),"UTF-8 byte lengths and bytes survive without recoding");
  gml_values_release(&encoded,1); gml_vm_free(&vm); return ok;
}
static int graph_limits_case(void){
  int ok=1;
  for(int depth=63;depth<=64;depth++){
    char text[1200]="6700000001000000";
    for(int i=0;i<depth;i++) strcat(text,"0200000001000000");
    strcat(text,"05000000");
    GmlVM vm={0}; GmlVal id=gml_builtin_call(&vm,"ds_stack_create",NULL,0);
    GmlVal seed[]={id,vreal(42)}; gml_builtin_call(&vm,"ds_stack_push",seed,2);
    GmlVal input[]={id,vstr(text)}; call(&vm,"ds_stack_read",input,2,1,&ok);
    GmlVal value=gml_builtin_call(&vm,"ds_stack_top",&id,1);
    ok &= expect(depth==63?value.t==V_ARR:real_is(value,42),
                 "depth boundary accepts the bounded tree and rejects the next level transactionally");
    GmlVal encoded=call(&vm,"ds_stack_write",&id,1,0,&ok);
    if(depth==63) ok &= expect(hex_is(encoded,text),"writer and reader have the same depth budget");
    gml_values_release(&encoded,1); gml_vm_free(&vm);
  }
  GmlVM vm={0}; GmlVal id=gml_builtin_call(&vm,"ds_queue_create",NULL,0);
  GmlVal cycle=gml_arr_new(1,vundef()); gml_arr_set(cycle,0,cycle);
  GmlVal input[]={id,cycle}; gml_builtin_call(&vm,"ds_queue_enqueue",input,2);
  GmlVal encoded=call(&vm,"ds_queue_write",&id,1,1,&ok);
  ok &= expect(encoded.t==V_UNDEF && gml_arr_get(cycle,0).arr==cycle.arr,
               "cyclic writes fail without mutating the source graph");
  gml_values_release(&encoded,1); gml_vm_free(&vm);
  GmlVM bounded={0}; id=gml_builtin_call(&bounded,"ds_stack_create",NULL,0);
  GmlArr large={0};
  input[0]=id; input[1]=(GmlVal){.t=V_ARR,.arr=&large};
  gml_builtin_call(&bounded,"ds_stack_push",input,2);
  /* Install a valid empty descriptor through normal escape tracking first.
   * Only the codec sees the synthetic bound; enqueue must not traverse it. */
  large.len=large.cap=1000001;
  encoded=call(&bounded,"ds_stack_write",&id,1,0,&ok);
  ok &= expect(encoded.t==V_UNDEF,"oversized arrays reject before any element access");
  /* Remove the synthetic borrowed descriptor before ordinary owner teardown. */
  gml_builtin_call(&bounded,"ds_stack_pop",&id,1);
  gml_values_release(&encoded,1); gml_vm_free(&bounded); return ok;
}
int main(int argc,char **argv){
  static const AnygmTestCase cases[]={
    {"scalar_vectors",scalar_case}, {"nested_arrays",arrays_case},
    {"legacy_rows_and_queue_offset",legacy_case}, {"transactional_rejection",rejection_case},
    {"numeric_and_text_policies",numeric_and_text_case}, {"graph_limits",graph_limits_case}
  };
  static const AnygmTestGroup group={"sequence_codecs",cases,sizeof cases/sizeof cases[0]};
  const char *filter=NULL;
  if(argc==3 && !strcmp(argv[1],"--case")) filter=argv[2];
  else if(argc!=1) return EXIT_FAILURE;
  AnygmTestResult result; anygm_test_run_groups(&group,1,filter,&result);
  return result.failed?EXIT_FAILURE:EXIT_SUCCESS;
}
