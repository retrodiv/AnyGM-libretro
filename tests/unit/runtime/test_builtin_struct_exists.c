/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* variable_struct_exists tests key presence independently of the stored value. */
#include "gml_vm.h"

#include <stdio.h>
#include <string.h>

GmlVal gml_builtin_call(GmlVM *vm, const char *name, GmlVal *args, int count);

static int says(const char *label, GmlVal v, int want){
  int got = (v.t==V_REAL && v.d!=0.0) ? 1 : 0;
  if(got!=want){
    fprintf(stderr,"%s: expected %s, got type %d value %g\n",
            label, want?"true":"false", (int)v.t, v.t==V_REAL?v.d:0.0);
    return 0;
  }
  return 1;
}

int main(void){
  GmlVM vm;
  memset(&vm,0,sizeof vm);
  int ok=1;

  GmlInstance *made=gml_struct_new(&vm);
  if(!made){
    fprintf(stderr,"could not allocate a struct\n");
    printf("FAILED\n");
    return 1;
  }
  GmlVal st=vreal((double)made->id);

  GmlVal set_a[]={st,vstr("up"),vreal(7)};
  gml_builtin_call(&vm,"variable_struct_set",set_a,3);
  GmlVal set_b[]={st,vstr("down"),vundef()};
  gml_builtin_call(&vm,"variable_struct_set",set_b,3);

  GmlVal q_up[]={st,vstr("up")};
  ok &= says("a key that was set exists",
             gml_builtin_call(&vm,"variable_struct_exists",q_up,2),1);

  GmlVal q_down[]={st,vstr("down")};
  ok &= says("a key set to undefined still exists",
             gml_builtin_call(&vm,"variable_struct_exists",q_down,2),1);

  GmlVal q_missing[]={st,vstr("left")};
  ok &= says("a key never set does not exist",
             gml_builtin_call(&vm,"variable_struct_exists",q_missing,2),0);

  printf("%s\n", ok?"ok":"FAILED");
  return ok?0:1;
}
