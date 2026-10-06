/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "gml_vm.h"
#include "gml_value_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static void expect_text(GmlVM *vm,const char *name,const char *expected){
  GmlVal actual=gml_vm_identifier_get(vm,name);
  int matches=actual.t==V_STR && actual.s && !strcmp(actual.s,expected);
  if(actual.t==V_STR && actual.d) free((char*)actual.s);
  if(matches) return;
  fprintf(stderr,"keyboard text: %s: expected string \"%s\", got type %d\n",
          name,expected,(int)actual.t);
  failures++;
}

int main(void){
  GmlVM vm={0};
  GmlInstance first={0},second={0};
  expect_text(&vm,"keyboard_string","");
  expect_text(&vm,"keyboard_lastchar","");

  /* A language-owned text buffer is shared by instance scopes, unlike an
   * ordinary user variable with no global declaration. */
  vm.cur_self=&first;
  gml_vm_assign_current_scope(&vm,"keyboard_string",vstr("neutral"));
  gml_vm_assign_current_scope(&vm,"keyboard_lastchar",vstr("q"));
  gml_vm_assign_current_scope(&vm,"local_control",vreal(42));
  vm.cur_self=&second;
  expect_text(&vm,"keyboard_string","neutral");
  expect_text(&vm,"keyboard_lastchar","q");
  GmlVal control=gml_vm_identifier_get(&vm,"local_control");
  if(control.t!=V_REAL || control.d!=0){
    fputs("keyboard text: ordinary instance storage escaped its scope\n",stderr);
    failures++;
  }
  gml_vm_assign_current_scope(&vm,"keyboard_string",vstr(""));
  gml_vm_assign_current_scope(&vm,"keyboard_lastchar",vstr(""));
  vm.cur_self=&first;
  expect_text(&vm,"keyboard_string","");
  expect_text(&vm,"keyboard_lastchar","");
  control=gml_vm_identifier_get(&vm,"local_control");
  if(control.t!=V_REAL || control.d!=42){
    fputs("keyboard text: the positive instance control was lost\n",stderr);
    failures++;
  }
  const uint32_t typed[]={'a',0xe9,0x1f642};
  if(!gml_input_text(&vm,typed,3)) failures++;
  expect_text(&vm,"keyboard_string","a\xc3\xa9\xf0\x9f\x99\x82");
  expect_text(&vm,"keyboard_lastchar","\xf0\x9f\x99\x82");
  GmlVal copy=gml_vm_identifier_get(&vm,"keyboard_string");
  const uint32_t edits[]={8,8,'b',8,'c',0,13,127,0x85,0xd800,0x110000};
  if(!gml_input_text(&vm,edits,sizeof edits/sizeof edits[0])) failures++;
  expect_text(&vm,"keyboard_string","ac");
  expect_text(&vm,"keyboard_lastchar","c");
  if(copy.t!=V_STR || strcmp(copy.s,"a\xc3\xa9\xf0\x9f\x99\x82")){
    fputs("keyboard text: a retained value changed with later input\n",stderr);
    failures++;
  }
  if(copy.t==V_STR && copy.d) free((char*)copy.s);
  if(!gml_input_text(&vm,NULL,0)) failures++;
  expect_text(&vm,"keyboard_lastchar","c");
  GmlVM other={0};
  expect_text(&other,"keyboard_string","");
  const uint32_t independent[]={'x'};
  if(!gml_input_text(&other,independent,1)) failures++;
  expect_text(&other,"keyboard_string","x");
  expect_text(&vm,"keyboard_string","ac");
  gml_vm_assign_current_scope(&vm,"keyboard_string",vstr(""));
  uint32_t many[1025];
  for(size_t i=0;i<1024;i++) many[i]=0xe9;
  many[1024]='q';
  if(!gml_input_text(&vm,many,1025)) failures++;
  GmlVal tail=gml_vm_identifier_get(&vm,"keyboard_string");
  if(tail.t!=V_STR || strlen(tail.s)!=2047 ||
     tail.s[2046]!='q' || gml_utf8_character_bytes((const unsigned char*)tail.s)!=2){
    fputs("keyboard text: the latest 1024-character boundary split UTF-8\n",stderr);
    failures++;
  }
  if(tail.t==V_STR && tail.d) free((char*)tail.s);
  const uint32_t erase[]={8};
  if(!gml_input_text(&vm,erase,1)) failures++;
  tail=gml_vm_identifier_get(&vm,"keyboard_string");
  if(tail.t!=V_STR || strlen(tail.s)!=2046) failures++;
  if(tail.t==V_STR && tail.d) free((char*)tail.s);
  gml_vm_free(&other);
  gml_varmap_free(&first.vars);
  gml_varmap_free(&second.vars);
  gml_vm_free(&vm);
  if(failures) return 1;
  puts("keyboard text: shared scope, Unicode, edits, latest-character bound and isolation: ok");
  return 0;
}
