/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* A member access owned by a scope sentinel reads that scope. This case covers the predicate,
 * not the full bytecode dispatch. Struct identifiers stay on the instance path in the caller;
 * IT_STACK marks another value below it rather than a scope. */
#include "gml_win.h"

#include <stdio.h>

static int says(const char *label, int got, int want){
  if(got!=want){
    fprintf(stderr,"%s: expected %d, got %d\n", label, want, got);
    return 0;
  }
  return 1;
}

int main(void){
  int ok=1;

  /* The global scope is a direct numeric owner. */
  ok &= says("global scope reads",      gml_it_is_scope_to_read(IT_GLOBAL), 1);

  /* Other scope sentinels can also be StackTop owners. */
  ok &= says("self reads",              gml_it_is_scope_to_read(IT_SELF),   1);
  ok &= says("other reads",             gml_it_is_scope_to_read(IT_OTHER),  1);
  ok &= says("local reads",             gml_it_is_scope_to_read(IT_LOCAL),  1);
  ok &= says("static reads",            gml_it_is_scope_to_read(IT_STATIC), 1);
  ok &= says("argument reads",          gml_it_is_scope_to_read(IT_ARG),    1);

  /* A marker, not a scope: the value below it is the owner. */
  ok &= says("the stack marker is not a scope", gml_it_is_scope_to_read(IT_STACK), 0);

  /* Instance ids and object indices belong to the instance path. */
  ok &= says("instance id is not a scope",  gml_it_is_scope_to_read(100001), 0);
  ok &= says("object index is not a scope", gml_it_is_scope_to_read(0),      0);
  ok &= says("object index 7 is not a scope", gml_it_is_scope_to_read(7),    0);

  printf(ok?"PASSED\n":"FAILED\n");
  return ok?0:1;
}
