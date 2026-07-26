/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "persistent_test_fixture.h"

#include "gml_builtin.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


int expect_ds_list_text_roundtrip(void){
  GmlVM vm={0};
  GmlVal parent=gml_builtin_call(&vm,"ds_list_create",NULL,0);
  GmlVal child=gml_builtin_call(&vm,"ds_list_create",NULL,0);
  GmlVal add_child[3]={child,vreal(7),vstr("nested")};
  (void)gml_builtin_call(&vm,"ds_list_add",add_child,3);
  GmlVal add_parent[4]={parent,vreal(3.5),vstr("text"),child};
  (void)gml_builtin_call(&vm,"ds_list_add",add_parent,4);
  GmlVal mark_args[2]={parent,vreal(2)};
  (void)gml_builtin_call(&vm,"ds_list_mark_as_list",mark_args,2);
  GmlVal encoded=gml_builtin_call(&vm,"ds_list_write",&parent,1);
  GmlVal restored=gml_builtin_call(&vm,"ds_list_create",NULL,0);
  GmlVal read_args[2]={restored,encoded};
  (void)gml_builtin_call(&vm,"ds_list_read",read_args,2);
  GmlVal size=gml_builtin_call(&vm,"ds_list_size",&restored,1);
  GmlVal at0[2]={restored,vreal(0)},at1[2]={restored,vreal(1)},at2[2]={restored,vreal(2)};
  GmlVal first=gml_builtin_call(&vm,"ds_list_find_value",at0,2);
  GmlVal second=gml_builtin_call(&vm,"ds_list_find_value",at1,2);
  GmlVal nested=gml_builtin_call(&vm,"ds_list_find_value",at2,2);
  GmlVal nested_size=gml_builtin_call(&vm,"ds_list_size",&nested,1);
  GmlVal nested_at[2]={nested,vreal(1)};
  GmlVal nested_text=gml_builtin_call(&vm,"ds_list_find_value",nested_at,2);
  GmlVal is_list=gml_builtin_call(&vm,"ds_list_is_list",at2,2);
  int ok=encoded.t==V_STR && encoded.s && strstr(encoded.s,"__gml_ds_list__") &&
    size.t==V_REAL && size.d==3 && first.t==V_REAL && first.d==3.5 &&
    second.t==V_STR && second.s && !strcmp(second.s,"text") &&
    nested_size.t==V_REAL && nested_size.d==2 && nested_text.t==V_STR && nested_text.s &&
    !strcmp(nested_text.s,"nested") && is_list.t==V_REAL && is_list.d==1;
  if(encoded.t==V_STR && encoded.d!=0) free((void*)encoded.s);
  (void)gml_builtin_call(&vm,"ds_list_destroy",&parent,1);
  (void)gml_builtin_call(&vm,"ds_list_destroy",&restored,1);
  if(!ok) fprintf(stderr,"ds_list text round-trip fixture failed\n");
  return ok;
}


int expect_ds_priority_lookup_mutation(void){
  GmlVM vm={0};
  GmlVal queue=gml_builtin_call(&vm,"ds_priority_create",NULL,0);
  GmlVal add_first[3]={queue,vstr("first"),vreal(5)};
  GmlVal add_second[3]={queue,vstr("second"),vreal(2)};
  (void)gml_builtin_call(&vm,"ds_priority_add",add_first,3);
  (void)gml_builtin_call(&vm,"ds_priority_add",add_second,3);

  GmlVal lookup_first[2]={queue,vstr("first")};
  GmlVal lookup_missing[2]={queue,vstr("missing")};
  GmlVal first_priority=gml_builtin_call(&vm,"ds_priority_find_priority",lookup_first,2);
  GmlVal missing_priority=gml_builtin_call(&vm,"ds_priority_find_priority",lookup_missing,2);
  GmlVal change_second[3]={queue,vstr("second"),vreal(8)};
  (void)gml_builtin_call(&vm,"ds_priority_change_priority",change_second,3);
  GmlVal maximum=gml_builtin_call(&vm,"ds_priority_find_max",&queue,1);
  (void)gml_builtin_call(&vm,"ds_priority_delete_value",lookup_first,2);
  GmlVal deleted_priority=gml_builtin_call(&vm,"ds_priority_find_priority",lookup_first,2);
  GmlVal size=gml_builtin_call(&vm,"ds_priority_size",&queue,1);

  int ok=first_priority.t==V_REAL && first_priority.d==5 &&
    missing_priority.t==V_UNDEF && maximum.t==V_STR && maximum.s &&
    !strcmp(maximum.s,"second") && deleted_priority.t==V_UNDEF &&
    size.t==V_REAL && size.d==1;
  (void)gml_builtin_call(&vm,"ds_priority_destroy",&queue,1);
  if(!ok) fprintf(stderr,"ds_priority lookup/mutation fixture failed\n");
  return ok;
}
