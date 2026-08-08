/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Map-to-array builtins must return arrays, not the numeric fallback for an
 * unknown builtin. Check the type as well as the length and insertion order. */
#include "gml_vm.h"

#include <stdio.h>
#include <string.h>

GmlVal gml_builtin_call(GmlVM *vm, const char *name, GmlVal *args, int count);

static int is_array(const char *label, GmlVal v, int want_len){
  if(v.t!=V_ARR || !v.arr){
    fprintf(stderr,"%s: expected an array, got type %d\n",label,(int)v.t);
    return 0;
  }
  int len=gml_val_array_length(v);
  if(len!=want_len){
    fprintf(stderr,"%s: expected %d entries, got %d\n",label,want_len,len);
    return 0;
  }
  return 1;
}

int main(void){
  GmlVM vm;
  memset(&vm,0,sizeof vm);
  int ok=1;

  GmlVal none[]={vreal(0)};
  GmlVal empty_keys=gml_builtin_call(&vm,"ds_map_keys_to_array",none,1);
  ok &= is_array("an absent map still answers with an array",empty_keys,0);

  GmlVal created=gml_builtin_call(&vm,"ds_map_create",NULL,0);
  int id=(int)created.d;

  GmlVal add_a[]={vreal(id),vstr("up"),vreal(7)};
  gml_builtin_call(&vm,"ds_map_add",add_a,3);
  GmlVal add_b[]={vreal(id),vstr("down"),vreal(9)};
  gml_builtin_call(&vm,"ds_map_add",add_b,3);

  GmlVal one[]={vreal(id)};
  GmlVal keys=gml_builtin_call(&vm,"ds_map_keys_to_array",one,1);
  ok &= is_array("two keys come back as a two-entry array",keys,2);
  if(keys.t==V_ARR){
    GmlVal first=gml_arr_get(keys,0), second=gml_arr_get(keys,1);
    /* Insertion order, matching ds_map_find_first/find_next, so one map has one iteration order. */
    if(!(first.t==V_STR && first.s && !strcmp(first.s,"up"))){
      fprintf(stderr,"keys[0]: expected \"up\"\n"); ok=0;
    }
    if(!(second.t==V_STR && second.s && !strcmp(second.s,"down"))){
      fprintf(stderr,"keys[1]: expected \"down\"\n"); ok=0;
    }
  }

  GmlVal values=gml_builtin_call(&vm,"ds_map_values_to_array",one,1);
  ok &= is_array("the values come back in the same order",values,2);
  if(values.t==V_ARR){
    if(gml_arr_get(values,0).d!=7.0 || gml_arr_get(values,1).d!=9.0){
      fprintf(stderr,"values: expected 7 then 9\n"); ok=0;
    }
  }

  /* No TAGS parsing is present yet; return an array-shaped empty result. */
  GmlVal tags=gml_builtin_call(&vm,"asset_get_tags",none,1);
  ok &= is_array("asset_get_tags reports no tags as an empty array",tags,0);

  printf("%s\n", ok?"ok":"FAILED");
  return ok?0:1;
}
