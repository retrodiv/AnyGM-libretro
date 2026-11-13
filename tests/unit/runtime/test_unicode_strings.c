/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "gml_builtin_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int failures;
static void check(GmlVM *vm,const char *name,GmlVal *a,int n,const char *s,double number){
  GmlVal v=gml_builtin_call(vm,name,a,n);
  GmlVal slow=gml_builtin_try_values_strings(vm,name,a,n);
  int same=s?(slow.t==V_STR && slow.s && !strcmp(slow.s,s)):(slow.t==V_REAL && slow.d==number);
  if(!same){ fprintf(stderr,"Unicode ordered string contract failed: %s\n",name); failures++; }
  if(slow.t==V_STR && slow.d) free((void*)slow.s);
  int good=s?(v.t==V_STR && v.s && !strcmp(v.s,s)):(v.t==V_REAL && v.d==number);
  if(!good){ fprintf(stderr,"Unicode string contract failed: %s\n",name); failures++; }
  if(v.t==V_STR && v.d) free((void*)v.s);
}
int main(void){
  GmlVM vm={0};
  GmlVal a[]={vstr("A\xc3\xb3\xe2\x82\xac\xf0\x9f\x99\x82Z"),vreal(2),vreal(1)};
  check(&vm,"string_length",a,1,NULL,5);
  check(&vm,"string_byte_length",a,1,NULL,11);
  check(&vm,"string_copy",a,3,"\xc3\xb3",0);
  check(&vm,"string_char_at",a,2,"\xc3\xb3",0);
  check(&vm,"string_ord_at",a,2,NULL,243);
  check(&vm,"string_byte_at",a,2,NULL,195);
  GmlVal accent=vstr("\xc3\xb3"); check(&vm,"ord",&accent,1,NULL,243);
  GmlVal code=vreal(243); check(&vm,"chr",&code,1,"\xc3\xb3",0);
  a[1]=vreal(4); check(&vm,"string_copy",a,3,"\xf0\x9f\x99\x82",0);
  check(&vm,"string_char_at",a,2,"\xf0\x9f\x99\x82",0);
  check(&vm,"string_ord_at",a,2,NULL,128578);
  code=vreal(128578); check(&vm,"chr",&code,1,"\xf0\x9f\x99\x82",0);
  a[1]=vreal(6); check(&vm,"string_char_at",a,2,"",0);
  check(&vm,"string_copy",a,3,"",0);
  gml_vm_free(&vm);
  return failures?1:0;
}
