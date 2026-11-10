/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Struct key presence and ownership across construction, cloning and removal. */
#include "gml_vm.h"
#include "gml_value_internal.h"

#include <stdio.h>
#include <stdlib.h>
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

static int json_struct_key_lifetimes(void){
  char content_key[]="content_key";
  char *content_strings[]={content_key};
  GmlWin win={.strs=content_strings,.n_strs=1};
  GmlVM vm={0};
  if(gml_vm_init(&vm,&win,NULL)!=0) return 0;

  GmlVal json=vstr("{\"dynamic_key\":1,\"content_key\":2,\"dynamic_key\":3}");
  GmlVal ref=gml_builtin_call(&vm,"json_parse",&json,1);
  GmlInstance *original=gml_struct_find(&vm,(unsigned)ref.d);
  int owned=0,interned=0;
  if(original){
    for(int i=0;i<original->vars.cap;i++){
      const GmlVarSlot *slot=&original->vars.slots[i];
      if(!slot->key) continue;
      if(!strcmp(slot->key,"dynamic_key"))
        owned=slot->key_owned && slot->val.t==V_REAL && slot->val.d==3;
      if(slot->key==content_key)
        interned=!slot->key_owned && slot->val.t==V_REAL && slot->val.d==2;
    }
  }
  int ok=original && original->vars.len==2 && owned && interned;
  if(ok){
    GmlVal copy=gml_builtin_call(&vm,"variable_clone",&ref,1);
    GmlVal remove[]={ref,vstr("dynamic_key")};
    GmlVal removed=gml_builtin_call(&vm,"struct_remove",remove,2);
    GmlVal get[]={copy,vstr("dynamic_key")};
    GmlVal value=gml_builtin_call(&vm,"struct_get",get,2);
    ok=removed.t==V_REAL && removed.d==1 && value.t==V_REAL && value.d==3;
    GmlVal remove_content[]={copy,vstr("content_key")};
    (void)gml_builtin_call(&vm,"struct_remove",remove_content,2);
    GmlInstance *clone=gml_struct_find(&vm,(unsigned)copy.d);
    ok=ok && clone && clone->vars.len==1;
    if(ok){
      for(int i=0;i<clone->vars.cap;i++){
        const GmlVarSlot *slot=&clone->vars.slots[i];
        if(slot->key)
          ok=slot->key_owned && !strcmp(slot->key,"dynamic_key") && slot->val.d==3;
      }
    }
  }
  gml_vm_free(&vm);
  free(win.str_hix);
  if(!ok) fprintf(stderr,"JSON struct keys did not retain the expected ownership\n");
  return ok;
}

int main(void){
  GmlVM vm;
  memset(&vm,0,sizeof vm);
  int ok=json_struct_key_lifetimes();

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
