/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Struct key presence and ownership across construction, cloning and removal. */
#include "gml_builtin.h"
#include "gml_value_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static GmlVal hash_call(GmlVM *vm,const char *name,GmlVal *args,int count,int cached){
  if(!cached) return gml_builtin_call(vm,name,args,count);
  int id=gml_builtin_fast_id(vm,name);
  return id>=0?gml_builtin_call_fast_id(vm,id,name,args,count):vundef();
}

static int hashed_struct_members(void){
  int all=1;
  for(int cached=0;cached<2;cached++){
    int ok=1; unsigned char *state=NULL,*again=NULL;
    char content_key[]="compiled_member"; char *strings[]={content_key};
    GmlWin win={.bytecode=17,.strs=strings,.n_strs=1};
    GmlVM vm={0},other={0};
    if(gml_vm_init(&vm,&win,NULL) || gml_vm_init(&other,&win,NULL)) return 0;
#define HASH_REQUIRE(expr) do { if(!(expr)){fprintf(stderr,"hashed struct: %s at %d\n",#expr,__LINE__);ok=0;goto done;} } while(0)
    GmlInstance *target=gml_struct_new(&vm),*isolated=gml_struct_new(&other);
    HASH_REQUIRE(target && isolated);
    GmlVal ref=vobject(target->id),other_ref=vobject(isolated->id);
    *gml_varmap_put(&vm.globals,"root")=ref;
    char dynamic_key[40];snprintf(dynamic_key,sizeof dynamic_key,"runtime_%u",17u);
    GmlVal name=vstr(dynamic_key);
    GmlVal token=hash_call(&vm,"variable_get_hash",&name,1,cached);
    HASH_REQUIRE(token.t==V_REAL && token.d==gml_value_name_hash("runtime_17"));
    int registered=vm.member_names.len;
    HASH_REQUIRE(hash_call(&vm,"variable_get_hash",&name,1,cached).d==token.d &&
                 vm.member_names.len==registered);
    memset(dynamic_key,'x',strlen(dynamic_key));
    GmlVal args[]={ref,token,vint64(INT64_C(9007199254740993))};
    hash_call(&vm,"struct_set_from_hash",args,3,cached);
    GmlVal named[]={ref,vstr("runtime_17")};
    GmlVal value=gml_builtin_call(&vm,"struct_get",named,2);
    HASH_REQUIRE(value.t==V_REAL && value.number_kind==GML_NUM_INT64 &&
                 value.integer==INT64_C(9007199254740993));
    int owns_key=0;
    for(int i=0;i<target->vars.cap;i++)
      if(target->vars.slots[i].key && !strcmp(target->vars.slots[i].key,"runtime_17"))
        owns_key=target->vars.slots[i].key_owned;
    HASH_REQUIRE(owns_key);
    HASH_REQUIRE(hash_call(&vm,"struct_exists_from_hash",args,2,cached).d==1);
    HASH_REQUIRE(gml_builtin_call(&vm,"struct_get_from_hash",args,2).integer==value.integer);
    /* The same numeric token cannot leak a dynamic spelling into another VM. */
    GmlVal other_args[]={other_ref,token,vreal(1)};
    hash_call(&other,"struct_set_from_hash",other_args,3,cached);
    HASH_REQUIRE(other.member_names.len==0 && isolated->vars.len==0);
    args[2]=vundef();hash_call(&vm,"struct_set_from_hash",args,3,cached);
    HASH_REQUIRE(hash_call(&vm,"struct_exists_from_hash",args,2,cached).d==1 &&
                 gml_builtin_call(&vm,"struct_get_from_hash",args,2).t==V_UNDEF);
    args[1]=vreal(gml_int32_from_bits((uint32_t)token.d));args[2]=vreal(29);
    hash_call(&vm,"struct_set_from_hash",args,3,cached);
    HASH_REQUIRE(gml_builtin_call(&vm,"struct_get",named,2).d==29);
    args[1]=vreal(gml_value_name_hash(content_key));args[2]=vreal(37);
    hash_call(&vm,"struct_set_from_hash",args,3,cached);
    GmlVal *compiled=gml_varmap_get(&target->vars,content_key);
    HASH_REQUIRE(compiled && compiled->d==37);
    name=vstr("array_member");GmlVal array_token=hash_call(&vm,"variable_get_hash",&name,1,cached);
    GmlVal array=gml_arr_new(1,vint64(INT64_C(9007199254740993)));
    args[1]=array_token;args[2]=array;hash_call(&vm,"struct_set_from_hash",args,3,cached);
    value=gml_builtin_call(&vm,"struct_get_from_hash",args,2);
    HASH_REQUIRE(value.t==V_ARR && gml_arr_get(value,0).integer==INT64_C(9007199254740993));
    name=vstr("__fn");args[1]=hash_call(&vm,"variable_get_hash",&name,1,cached);args[2]=vreal(0);
    target->method_bound=1;hash_call(&vm,"struct_set_from_hash",args,3,cached);
    HASH_REQUIRE(!target->method_bound);
    const double invalid[]={NAN,INFINITY,0.5,-2147483649.0,4294967296.0};
    int fields=target->vars.len;
    for(unsigned i=0;i<sizeof invalid/sizeof invalid[0];i++){
      args[1]=vreal(invalid[i]);hash_call(&vm,"struct_set_from_hash",args,3,cached);
      HASH_REQUIRE(hash_call(&vm,"struct_exists_from_hash",args,2,cached).d==0 && target->vars.len==fields);
    }
    const GmlVal bad_targets[]={vreal(NAN),vreal(INFINITY),vreal(1e300),vreal(IT_GLOBAL),
                                vreal(GML_STRUCT_ID_BASE+0.5),vstr("not a struct")};
    int globals=vm.globals.len;args[1]=token;
    for(unsigned i=0;i<sizeof bad_targets/sizeof bad_targets[0];i++){
      args[0]=bad_targets[i];hash_call(&vm,"struct_set_from_hash",args,3,cached);
      HASH_REQUIRE(hash_call(&vm,"struct_exists_from_hash",args,2,cached).d==0 &&
                   target->vars.len==fields && vm.globals.len==globals);
    }
    args[0]=ref;
    HASH_REQUIRE(hash_call(&vm,"variable_get_hash",NULL,0,cached).t==V_UNDEF);
    HASH_REQUIRE(hash_call(&vm,"struct_exists_from_hash",NULL,0,cached).d==0);
    hash_call(&vm,"struct_set_from_hash",NULL,0,cached);
    HASH_REQUIRE(target->vars.len==fields);
    /* Save the name before any member can supply it back to the reader. */
    name=vstr("orphan_member");GmlVal orphan=hash_call(&vm,"variable_get_hash",&name,1,cached);
    HASH_REQUIRE(!gml_varmap_get(&target->vars,"orphan_member"));
    registered=vm.member_names.len;
    size_t capacity=gml_vm_state_size(&vm),written=0,used=0,repeated=0;
    state=malloc(capacity);again=malloc(capacity);
    HASH_REQUIRE(state && again && gml_vm_state_save(&vm,state,capacity,&written));
    HASH_REQUIRE(gml_vm_state_save(&vm,again,capacity,&repeated) &&
                 repeated==written && !memcmp(state,again,written));
    name=vstr("future_member");hash_call(&vm,"variable_get_hash",&name,1,cached);
    HASH_REQUIRE(vm.member_names.len==registered+1);
    HASH_REQUIRE(gml_vm_state_load(&vm,state,written,&used) && used==written);
    HASH_REQUIRE(vm.member_names.len==registered && !gml_varmap_get(&vm.member_names,"future_member"));
    HASH_REQUIRE(gml_vm_state_save(&vm,again,capacity,&repeated) &&
                 repeated==written && !memcmp(state,again,written));
    args[0]=ref;args[1]=orphan;args[2]=vreal(41);
    hash_call(&vm,"struct_set_from_hash",args,3,cached);
    named[1]=vstr("orphan_member");
    HASH_REQUIRE(gml_builtin_call(&vm,"struct_get",named,2).d==41);
done:
    free(state);free(again);gml_vm_free(&vm);gml_vm_free(&other);free(win.str_hix);free(win.str_hix_hash);
    all &= ok;
#undef HASH_REQUIRE
  }
  return all;
}

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
  free(win.str_hix);free(win.str_hix_hash);
  if(!ok) fprintf(stderr,"JSON struct keys did not retain the expected ownership\n");
  return ok;
}

int main(void){
  GmlVM vm;
  memset(&vm,0,sizeof vm);
  int ok=json_struct_key_lifetimes() && hashed_struct_members();

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
