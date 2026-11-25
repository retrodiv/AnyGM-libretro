/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "gml_builtin.h"
#include "gml_value_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int expect(int condition,const char *name){
  if(!condition) fprintf(stderr,"JSON contract failed: %s\n",name);
  return condition;
}

static int expect_text(GmlVal value,const char *expected,const char *name){
  int ok=expect(value.t==V_STR && value.s && !strcmp(value.s,expected),name);
  if(value.t==V_STR && value.d!=0) free((char *)value.s);
  return ok;
}

static void wrap_arrays(char *output,size_t depth,const char *value){
  size_t length=strlen(value);
  memset(output,'[',depth);
  memcpy(output+depth,value,length);
  memset(output+depth+length,']',depth);
  output[2*depth+length]=0;
}

static void count_depth_warning(void *userdata,AnygmLogLevel level,const char *message){
  if(level==ANYGM_LOG_WARN && strstr(message,"exceeded depth")) (*(int *)userdata)++;
}

int main(void){
  GmlWin win={.bytecode=17};
  int warnings=0;
  AnygmHostServices host={.struct_size=sizeof host,.abi_version=ANYGM_HOST_SERVICES_VERSION,
                         .userdata=&warnings,.log=count_depth_warning};
  GmlVM vm={0};
  if(gml_vm_init(&vm,&win,&host)!=0) return 1;

  GmlVal root=gml_builtin_call(&vm,"ds_map_create",NULL,0);
  GmlVal list=gml_builtin_call(&vm,"ds_list_create",NULL,0);
  GmlVal map_entry[]={root,vstr("children"),list};
  GmlVal list_entry[]={list,root};
  GmlVal list_index[]={list,vreal(0)};
  gml_builtin_call(&vm,"ds_map_add_list",map_entry,3);
  gml_builtin_call(&vm,"ds_list_add",list_entry,2);
  gml_builtin_call(&vm,"ds_list_mark_as_map",list_index,2);
  int ok=expect(root.t==V_REAL && list.t==V_REAL && root.d>=0 && list.d>=0 &&
                root.d!=list.d,"map and list receive distinct identifiers");
  ok &= expect_text(gml_builtin_call(&vm,"json_encode",&root,1),"{\"children\":[null]}",
                    "a map and list cycle ends at the repeated ancestor");

  char nested[300];
  wrap_arrays(nested,128,"37");
  GmlVal text=vstr(nested);
  GmlVal parsed=gml_builtin_call(&vm,"json_parse",&text,1);
  *gml_varmap_put(&vm.globals,"deep_array")=parsed;
  ok &= expect_text(gml_builtin_call(&vm,"json_stringify",&parsed,1),nested,
                    "maximum-depth array preserves its leaf");

  char lists[300],legacy[330];
  wrap_arrays(lists,127,"37");
  snprintf(legacy,sizeof legacy,"{\"values\":%s}",lists);
  text=vstr(legacy);
  GmlVal map=gml_builtin_call(&vm,"json_decode",&text,1);
  ok &= expect_text(gml_builtin_call(&vm,"json_encode",&map,1),legacy,
                    "maximum-depth legacy map and lists preserve their leaf");
  ok &= expect(warnings==0,"accepted depth is quiet");

  GmlVal shared=gml_arr_new(2,parsed);
  *gml_varmap_put(&vm.globals,"shared")=shared;
  char truncated[300], expected[610];
  wrap_arrays(truncated,127,"null");
  snprintf(expected,sizeof expected,"[%s,%s]",truncated,truncated);
  ok &= expect_text(gml_builtin_call(&vm,"json_stringify",&shared,1),expected,
                    "overflow produces JSON that stays within the parser boundary");
  ok &= expect(warnings==1,"two overflowing branches produce one encoder warning");

  GmlInstance *record=gml_struct_new(&vm);
  if(!record){ gml_vm_free(&vm); return 1; }
  GmlVal record_ref=vobject(record->id);
  *gml_varmap_put(&record->vars,"self")=record_ref;
  GmlVal repeated=gml_arr_new(2,record_ref);
  *gml_varmap_put(&vm.globals,"repeated")=repeated;
  ok &= expect_text(gml_builtin_call(&vm,"json_stringify",&repeated,1),
                    "[{\"self\":null},{\"self\":null}]",
                    "cycles become null and shared structs are emitted twice");

  GmlVal cycle=gml_arr_new(1,vundef());
  gml_arr_set(cycle,0,cycle);
  *gml_varmap_put(&vm.globals,"array_cycle")=cycle;
  ok &= expect_text(gml_builtin_call(&vm,"json_stringify",&cycle,1),"[null]",
                    "an array can refer to itself");
  ok &= expect(warnings==1,"cycles do not report depth truncation");

  wrap_arrays(nested,128,"[]");
  text=vstr(nested);
  GmlVal empty_leaf=gml_builtin_call(&vm,"json_parse",&text,1);
  *gml_varmap_put(&vm.globals,"empty_leaf")=empty_leaf;
  ok &= expect_text(gml_builtin_call(&vm,"json_stringify",&empty_leaf,1),nested,
                    "an empty container at the boundary needs no truncation");
  ok &= expect(warnings==1,"an empty boundary container is quiet");

  wrap_arrays(lists,128,"37");
  snprintf(legacy,sizeof legacy,"{\"values\":%s}",lists);
  text=vstr(legacy);
  GmlVal invalid=gml_builtin_call(&vm,"json_decode",&text,1);
  ok &= expect(invalid.t==V_REAL && invalid.d==-1 && warnings==2,
               "legacy decoding warns once when a value exceeds the reader boundary");

  gml_vm_free(&vm);
  free(win.str_hix);
  free(win.str_hix_hash);
  if(ok) puts("JSON depth and cycle contracts passed");
  return ok?0:1;
}
