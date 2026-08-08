/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Exercise numbered string placeholders with authored synthetic arguments, including
 * repeats, reordering, missing indices and non-placeholder braces. */
#include "gml_vm.h"

#include <stdio.h>
#include <string.h>

GmlVal gml_builtin_call(GmlVM *vm, const char *name, GmlVal *args, int count);

static int expect(const char *label, GmlVal actual, const char *want){
  if(actual.t==V_STR && actual.s && !strcmp(actual.s,want)) return 1;
  fprintf(stderr,"%s: expected \"%s\", got \"%s\"\n",label,want,
          actual.t==V_STR&&actual.s?actual.s:"<not a string>");
  return 0;
}

int main(void){
  GmlVM vm;
  memset(&vm,0,sizeof vm);
  int ok=1;

  GmlVal one[]={vstr("plain")};
  ok &= expect("a template with no arguments is returned as it is",
               gml_builtin_call(&vm,"string",one,1),"plain");

  GmlVal number[]={vreal(42)};
  ok &= expect("a number still converts",
               gml_builtin_call(&vm,"string",number,1),"42");

  GmlVal two[]={vstr("{0}x{1}"),vreal(1920),vreal(1080)};
  ok &= expect("both placeholders are filled",
               gml_builtin_call(&vm,"string",two,3),"1920x1080");

  GmlVal repeat[]={vstr("{0} and {0}"),vstr("a")};
  ok &= expect("an index may be used twice",
               gml_builtin_call(&vm,"string",repeat,2),"a and a");

  GmlVal reorder[]={vstr("{1},{0}"),vstr("first"),vstr("second")};
  ok &= expect("indices are positions, not order of appearance",
               gml_builtin_call(&vm,"string",reorder,3),"second,first");

  /* A missing argument leaves its placeholder unchanged. */
  GmlVal missing[]={vstr("a{0}b{7}c"),vstr("X")};
  ok &= expect("an index with no argument keeps its braces",
               gml_builtin_call(&vm,"string",missing,2),"aXb{7}c");

  GmlVal notindex[]={vstr("{}{a}{ 0}"),vstr("X")};
  ok &= expect("only a brace pair holding digits is a placeholder",
               gml_builtin_call(&vm,"string",notindex,2),"{}{a}{ 0}");


  /* Extra arguments leave a string without numbered placeholders unchanged and do not
   * alter ordinary conversion of a non-template value. */
  GmlVal legacy[]={vstr("no placeholders here"),vreal(1),vreal(2)};
  ok &= expect("a template without placeholders is untouched by extra arguments",
               gml_builtin_call(&vm,"string",legacy,3),"no placeholders here");

  GmlVal legacy_single[]={vreal(7),vreal(9)};
  ok &= expect("a number with an additional argument still converts",
               gml_builtin_call(&vm,"string",legacy_single,2),"7");

  if(ok) printf("string format: placeholders are substituted by index\n");
  return ok?0:1;
}
