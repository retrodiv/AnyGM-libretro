/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Exercise both instance-name queries with a synthetic variable map. The returned
 * count and array must agree because a caller can size a loop with one and index the other. */
#include "gml_vm.h"

#include <stdio.h>
#include <string.h>

GmlVal gml_builtin_call(GmlVM *vm, const char *name, GmlVal *args, int count);

int main(void){
  GmlVM vm;
  memset(&vm,0,sizeof vm);
  int ok=1;

  GmlInstance *made=gml_struct_new(&vm);
  if(!made){ fprintf(stderr,"could not allocate\n"); return 1; }
  GmlVal ref=vreal((double)made->id);

  GmlVal none[]={ref};
  GmlVal empty_count=gml_builtin_call(&vm,"variable_instance_names_count",none,1);
  if(!(empty_count.t==V_REAL && empty_count.d==0.0)){
    fprintf(stderr,"a fresh instance should carry no names, got %g\n",
            empty_count.t==V_REAL?empty_count.d:-1.0);
    ok=0;
  }

  const char *wanted[]={"health","ammo","name"};
  for(int i=0;i<3;i++){
    GmlVal set[]={ref,vstr(wanted[i]),vreal(i+1)};
    gml_builtin_call(&vm,"variable_instance_set",set,3);
  }

  GmlVal count=gml_builtin_call(&vm,"variable_instance_names_count",none,1);
  if(!(count.t==V_REAL && count.d==3.0)){
    fprintf(stderr,"expected 3 names, got %g\n",count.t==V_REAL?count.d:-1.0);
    ok=0;
  }

  GmlVal names=gml_builtin_call(&vm,"variable_instance_get_names",none,1);
  /* The count and the array must agree: a loop sized by one and indexed into the other is how a
   * caller meets this pair, so a disagreement here is an out-of-bounds read in content. */
  int seen=0;
  for(int i=0;i<3;i++){
    GmlVal entry=gml_arr_get(names,i);
    if(entry.t!=V_STR || !entry.s) continue;
    for(int w=0;w<3;w++) if(!strcmp(entry.s,wanted[w])) seen++;
  }
  if(seen!=3){
    fprintf(stderr,"the array did not carry the three names that were set (matched %d)\n",seen);
    ok=0;
  }

  if(ok) printf("instance names: the count and the array agree on what was set\n");
  return ok?0:1;
}
