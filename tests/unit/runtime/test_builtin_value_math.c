/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* log2, ln, exp, the dot products, is_nan, is_infinity and string_starts_with.
 *
 * The checks distinguish mathematical values, types and argument shapes; unknown names
 * would otherwise return the generic zero value.
 *
 * The assertions here are the ones that separate a correct implementation from a plausible one:
 * the base of the logarithm, the difference between the plain and normalised dot products, and the
 * fact that only a real can be NaN or infinite - a string coerced to a number must not become
 * either.
 */
#include "gml_vm.h"
#include "gml_builtin.h"
#include "gml_vm_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

GmlVal gml_builtin_call(GmlVM *vm, const char *name, GmlVal *args, int count);

static int expect(const char *label, GmlVal actual, double want){
  if(actual.t==V_REAL && fabs(actual.d-want)<1e-9) return 1;
  fprintf(stderr,"%s: expected %g, got %g\n",label,want,actual.t==V_REAL?actual.d:-999);
  return 0;
}

static int integer_type_queries(GmlVM *vm){
  GmlVal values[]={vint32(0),vint32(-1),vint32(INT32_MAX),vint64(0),
    vint64(INT64_MIN),vint64(INT64_MAX),vreal(0),vreal(7),vreal(-0.0),
    vreal(7.5),vreal(NAN),vreal(INFINITY),vstr("7"),vundef(),vobject(7)};
  const char *names[]={"is_int32","is_int64"};
  int ok=1;
  for(unsigned which=0;which<2;which++){
    int id=gml_builtin_fast_id(vm,names[which]);
    if(id<0) return 0;
    for(unsigned index=0;index<sizeof values/sizeof values[0];index++){
      double wanted=gml_val_number_kind(values[index])==
        (which?GML_NUM_INT64:GML_NUM_INT32);
      ok&=expect("named integer kind",gml_builtin_call(vm,names[which],&values[index],1),wanted);
      ok&=expect("cached integer kind",gml_builtin_call_fast_id(vm,id,names[which],&values[index],1),wanted);
    }
    ok&=expect("missing integer kind",gml_builtin_call(vm,names[which],NULL,0),0);
  }
  GmlVal text=vstr("9007199254740993");
  GmlVal wide=gml_builtin_call(vm,"int64",&text,1);
  char formatted[64];
  if(gml_val_number_kind(wide)!=GML_NUM_INT64 || wide.integer!=INT64_C(9007199254740993) ||
     !gml_val_format_integer(wide,formatted,sizeof formatted) || strcmp(formatted,"9007199254740993")){
    fprintf(stderr,"integer type: explicit conversion lost bits above double precision\n");ok=0;
  }
  GmlVal array=gml_arr_new(0,vreal(0));
  gml_arr_set(array,31,vint64(0));
  gml_arr_set(array,0,wide);
  GmlVal zero=gml_arr_get(array,31),hole=gml_arr_get(array,30),stored=gml_arr_get(array,0);
  if(gml_val_number_kind(zero)!=GML_NUM_INT64 || zero.integer ||
     gml_val_number_kind(hole)!=GML_NUM_REAL || gml_val_number_kind(stored)!=GML_NUM_INT64 ||
     stored.integer!=wide.integer){
    fprintf(stderr,"integer type: array copy or zero elision lost numeric identity\n");ok=0;
  }
  gml_values_release_owned(&array,1);
  GmlVal computed;
  if(!gml_vm_integer_binary(OP_ADD,wide,vint64(2),1,&computed) ||
     gml_val_number_kind(computed)!=GML_NUM_INT64 || computed.integer!=INT64_C(9007199254740995)) ok=0;
  if(!gml_vm_integer_binary(OP_DIV,vint64(5),vint64(2),1,&computed) ||
     gml_val_number_kind(computed)!=GML_NUM_INT64 || computed.integer!=2) ok=0;
  if(!gml_vm_integer_binary(OP_SHL,vint32(1),vint32(60),1,&computed) ||
     gml_val_number_kind(computed)!=GML_NUM_INT64 || computed.integer!=INT64_C(1152921504606846976)) ok=0;
  if(!gml_vm_integer_binary(OP_ADD,vint32(7),vint32(1),1,&computed) ||
     gml_val_number_kind(computed)!=GML_NUM_INT32 || computed.integer!=8) ok=0;
  if(gml_vm_integer_binary(OP_DIV,vint64(5),vreal(2),1,&computed) ||
     gml_vm_integer_binary(OP_AND,vreal(7),vreal(3),0,&computed)) ok=0;
  if(ok) puts("integer types: named/cached kind matrix, exact conversion and array identity passed");
  return ok;
}

/* A compact integer operand is a VM stack representation. A typed store into
 * a variable boxes it as a real; an already boxed integer keeps its identity. */
static int encoded_numeric_store_contract(void){
  int ok=1;
  for(int raw=0;raw<2;raw++) for(int local=0;local<2;local++)
  for(int source=0;source<4;source++){
    GmlWin win={.bytecode=17,.size=128,.owns=1};
    GmlVM vm={0};
    win.data=calloc(win.size,1);
    win.code=calloc(1,sizeof *win.code);win.n_code=1;
    win.ref_addr=calloc(3,sizeof *win.ref_addr);
    win.ref_name=calloc(3,sizeof *win.ref_name);
    win.ref_kind=calloc(3,sizeof *win.ref_kind);
    if(!win.data || !win.code || !win.ref_addr || !win.ref_name || !win.ref_kind){
      gml_win_free(&win);return 0;
    }
    uint32_t cursor=0;
    #define STORE_WORD(word) do{ uint32_t numeric_word=(word); \
      for(unsigned b=0;b<4;b++)win.data[cursor++]=(uint8_t)(numeric_word>>(8*b)); }while(0)
    #define STORE_REFERENCE(name) do{ \
      win.ref_addr[win.n_refs]=cursor;win.ref_name[win.n_refs++]=(name); \
      STORE_WORD(UINT32_C(0xa0000000)); }while(0)
    unsigned type=source==0?DT_INT32:source==1?DT_VAR:
      source==2?DT_INT64:DT_DOUBLE;
    if(source==0){
      STORE_WORD(UINT32_C(0x840f0007));
    }else if(source==1){
      STORE_WORD((uint32_t)OP_PUSH<<24|(uint32_t)DT_VAR<<16|(uint16_t)IT_LOCAL);
      STORE_REFERENCE("argument0");
    }else{
      STORE_WORD((uint32_t)OP_PUSH<<24|type<<16);
      uint64_t bits=7;
      if(source==3){double value=7;memcpy(&bits,&value,sizeof bits);}
      STORE_WORD((uint32_t)bits);STORE_WORD((uint32_t)(bits>>32));
    }
    int scope=local?IT_LOCAL:IT_GLOBAL;
    STORE_WORD((uint32_t)OP_POP<<24|(type<<4|DT_VAR)<<16|(uint16_t)scope);
    STORE_REFERENCE("numeric_slot");
    STORE_WORD((uint32_t)OP_PUSH<<24|(uint32_t)DT_VAR<<16|(uint16_t)scope);
    STORE_REFERENCE("numeric_slot");
    if(source==0){
      STORE_WORD(UINT32_C(0x840f0009));
      STORE_WORD((uint32_t)OP_CONV<<24|(DT_VAR<<4|DT_INT32)<<16);
      STORE_WORD((uint32_t)OP_DIV<<24|(DT_VAR<<4|DT_VAR)<<16);
    }
    STORE_WORD((uint32_t)OP_RET<<24|(uint32_t)DT_VAR<<16);
    #undef STORE_REFERENCE
    #undef STORE_WORD
    win.code[0].name="gml_Script_numeric_store";
    win.code[0].length=cursor;
    win.code[0].cache_bad=raw;
    int initialized=gml_vm_init(&vm,&win,NULL)==0;
    GmlVal argument=vint32(7);
    GmlVal actual=initialized?gml_vm_run_code(&vm,0,NULL,NULL,&argument,1):vundef();
    GmlNumberKind wanted=source==1?GML_NUM_INT32:
      source==2?GML_NUM_INT64:GML_NUM_REAL;
    double number=source==0?7.0/9.0:7.0;
    if(!initialized || actual.t!=V_REAL ||
       gml_val_number_kind(actual)!=wanted || fabs(actual.d-number)>1e-12){
      fprintf(stderr,"numeric store: raw=%d local=%d source=%d kind=%d value=%.17g\n",
        raw,local,source,(int)gml_val_number_kind(actual),actual.d);ok=0;
    }
    gml_vm_free(&vm);gml_win_free(&win);
  }
  if(ok) puts("numeric stores: cached/raw local/global primitive boxing and variant integer identity passed");
  return ok;
}

int main(void){
  GmlVM vm;
  memset(&vm,0,sizeof vm);
  int ok=1;
  ok&=integer_type_queries(&vm);
  ok&=encoded_numeric_store_contract();

  /* Independent Euclidean-distance controls: all three axes, translation,
   * symmetry, coincidence, and cached dispatch must agree. */
  GmlVal points[]={vreal(-5),vreal(7),vreal(-11),vreal(-2),vreal(11),vreal(1)};
  ok &= expect("three-axis distance",gml_builtin_call(&vm,"point_distance_3d",points,6),13);
  GmlVal reversed[]={points[3],points[4],points[5],points[0],points[1],points[2]};
  ok &= expect("distance is symmetric",gml_builtin_call(&vm,"point_distance_3d",reversed,6),13);
  GmlVal vertical[]={vreal(2),vreal(3),vreal(-4),vreal(2),vreal(3),vreal(5)};
  ok &= expect("z axis contributes",gml_builtin_call(&vm,"point_distance_3d",vertical,6),9);
  GmlVal coincident[]={vreal(2),vreal(3),vreal(5),vreal(2),vreal(3),vreal(5)};
  ok &= expect("coincident points",gml_builtin_call(&vm,"point_distance_3d",coincident,6),0);
  int distance_id=gml_builtin_fast_id(&vm,"point_distance_3d");
  if(distance_id<0){ fprintf(stderr,"point_distance_3d: missing cached dispatch\n"); ok=0; }
  else ok &= expect("cached three-axis distance",
      gml_builtin_call_fast_id(&vm,distance_id,"point_distance_3d",points,6),13);

  /* ---- logarithms and the exponent ---- */
  GmlVal four[]={vreal(4)};
  ok &= expect("log2 of 4 is 2",gml_builtin_call(&vm,"log2",four,1),2.0);
  GmlVal big[]={vreal(256)};
  ok &= expect("log2 of 256 is 8",gml_builtin_call(&vm,"log2",big,1),8.0);
  GmlVal one[]={vreal(1)};
  ok &= expect("ln of 1 is 0",gml_builtin_call(&vm,"ln",one,1),0.0);
  /* Names the base: the natural logarithm of 100 is 4.605, the base-ten one is 2. */
  GmlVal hundred[]={vreal(100)};
  ok &= expect("ln is the natural logarithm",gml_builtin_call(&vm,"ln",hundred,1),4.605170185988092);
  GmlVal zero[]={vreal(0)};
  ok &= expect("exp of 0 is 1",gml_builtin_call(&vm,"exp",zero,1),1.0);
  GmlVal unit[]={vreal(1)};
  ok &= expect("exp of 1 is e",gml_builtin_call(&vm,"exp",unit,1),2.718281828459045);
  GmlVal three[]={vreal(3)};
  GmlVal e3=gml_builtin_call(&vm,"exp",three,1);
  GmlVal back[]={e3};
  ok &= expect("ln undoes exp",gml_builtin_call(&vm,"ln",back,1),3.0);

  /* ---- dot products ---- */
  GmlVal perpendicular[]={vreal(1),vreal(0),vreal(0),vreal(1)};
  ok &= expect("perpendicular vectors have no projection",
               gml_builtin_call(&vm,"dot_product",perpendicular,4),0.0);
  GmlVal along[]={vreal(3),vreal(4),vreal(3),vreal(4)};
  ok &= expect("a vector against itself is its squared length",
               gml_builtin_call(&vm,"dot_product",along,4),25.0);
  /* The normalised form is the cosine, so the same pair answers 1 rather than 25. */
  ok &= expect("the normalised form answers the cosine",
               gml_builtin_call(&vm,"dot_product_normalised",along,4),1.0);
  ok &= expect("the American spelling is the same operation",
               gml_builtin_call(&vm,"dot_product_normalized",along,4),1.0);
  GmlVal opposed[]={vreal(1),vreal(0),vreal(-1),vreal(0)};
  ok &= expect("opposed unit vectors answer minus one",
               gml_builtin_call(&vm,"dot_product_normalised",opposed,4),-1.0);
  /* A zero-length vector has no direction: answer 0 rather than dividing by zero. */
  GmlVal degenerate[]={vreal(0),vreal(0),vreal(1),vreal(1)};
  ok &= expect("a vector with no length normalises to nothing",
               gml_builtin_call(&vm,"dot_product_normalised",degenerate,4),0.0);
  GmlVal spatial[]={vreal(1),vreal(2),vreal(3),vreal(4),vreal(5),vreal(6)};
  ok &= expect("the 3d form sums three products",
               gml_builtin_call(&vm,"dot_product_3d",spatial,6),32.0);
  GmlVal spatial_along[]={vreal(0),vreal(0),vreal(2),vreal(0),vreal(0),vreal(9)};
  ok &= expect("the normalised 3d form answers the cosine",
               gml_builtin_call(&vm,"dot_product_3d_normalised",spatial_along,6),1.0);

  /* ---- the two predicates ---- */
  GmlVal nan_value[]={vreal(NAN)};
  ok &= expect("a NaN is recognised",gml_builtin_call(&vm,"is_nan",nan_value,1),1.0);
  GmlVal inf_value[]={vreal(INFINITY)};
  ok &= expect("an infinity is recognised",gml_builtin_call(&vm,"is_infinity",inf_value,1),1.0);
  GmlVal neg_inf[]={vreal(-INFINITY)};
  ok &= expect("a negative infinity is one too",
               gml_builtin_call(&vm,"is_infinity",neg_inf,1),1.0);
  GmlVal ordinary[]={vreal(42)};
  ok &= expect("an ordinary number is neither",gml_builtin_call(&vm,"is_nan",ordinary,1),0.0);
  ok &= expect("an ordinary number is not infinite",
               gml_builtin_call(&vm,"is_infinity",ordinary,1),0.0);
  /* A string is neither, and must not become either by being coerced to a number first. */
  GmlVal text[]={vstr("nan")};
  ok &= expect("a string is not a NaN",gml_builtin_call(&vm,"is_nan",text,1),0.0);
  GmlVal text_inf[]={vstr("inf")};
  ok &= expect("a string is not an infinity",
               gml_builtin_call(&vm,"is_infinity",text_inf,1),0.0);
  ok &= expect("no argument is not a NaN",gml_builtin_call(&vm,"is_nan",NULL,0),0.0);

  /* ---- the prefix test, beside the suffix test it was missing next to ---- */
  GmlVal has_prefix[]={vstr("status-ready"),vstr("status")};
  ok &= expect("a matching prefix is reported",
               gml_builtin_call(&vm,"string_starts_with",has_prefix,2),1.0);
  GmlVal wrong_case[]={vstr("status-ready"),vstr("STATUS")};
  ok &= expect("prefix matching is case-sensitive",
               gml_builtin_call(&vm,"string_starts_with",wrong_case,2),0.0);
  GmlVal empty_prefix[]={vstr("status-ready"),vstr("")};
  ok &= expect("every string starts with the empty string",
               gml_builtin_call(&vm,"string_starts_with",empty_prefix,2),1.0);
  GmlVal too_long[]={vstr("ok"),vstr("okay")};
  ok &= expect("a prefix longer than the string does not match",
               gml_builtin_call(&vm,"string_starts_with",too_long,2),0.0);
  /* The suffix, not the prefix: the two must not answer each other. */
  GmlVal suffix_not_prefix[]={vstr("status-ready"),vstr("ready")};
  ok &= expect("a suffix is not a prefix",
               gml_builtin_call(&vm,"string_starts_with",suffix_not_prefix,2),0.0);

  if(ok) printf("value math: logarithms, dot products, the two predicates and the prefix test\n");
  return ok?0:1;
}
