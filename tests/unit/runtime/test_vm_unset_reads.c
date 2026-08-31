/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* A synthetic unset-read case pins both the count and the existing zero result.
 * A set-variable control proves that ordinary reads do not increment the count. */
#include "gml_vm.h"
#include "gml_vm_internal.h"
#include "gml_value_internal.h"

#include <stdio.h>
#include <string.h>

int main(void){
  GmlVM vm;
  memset(&vm, 0, sizeof vm);

  if(gml_vm_unset_reads(&vm) != 0ul){
    fprintf(stderr, "a fresh VM already counts unset reads\n");
    return 1;
  }

  /* An instance variable nobody set, read with no instance to find it on. */
  GmlVal first = gml_vm_identifier_get(&vm, "a_variable_nobody_ever_set");
  if(gml_vm_unset_reads(&vm) != 1ul){
    fprintf(stderr, "reading a variable nobody set did not count: %lu\n",
            gml_vm_unset_reads(&vm));
    return 1;
  }
  if(first.t != V_REAL || first.d != 0.0){
    fprintf(stderr, "the read stopped answering zero, which changes what every game sees\n");
    return 1;
  }

  /* Every read counts, not merely the first: the log names a site once, the counter does not. */
  (void)gml_vm_identifier_get(&vm, "a_variable_nobody_ever_set");
  if(gml_vm_unset_reads(&vm) != 2ul){
    fprintf(stderr, "the second read of the same name did not count: %lu\n",
            gml_vm_unset_reads(&vm));
    return 1;
  }

  /* The control. A variable that is set must not count, or the number above means nothing. */
  gml_vm_global_array_set(&vm, "a_variable_that_is_set", 0, 7.0);
  unsigned long before = gml_vm_unset_reads(&vm);
  GmlVal found = gml_vm_variable_get_h(&vm, IT_GLOBAL, "a_variable_that_is_set",
                                       gml_value_name_hash("a_variable_that_is_set"));
  if(gml_vm_unset_reads(&vm) != before){
    fprintf(stderr, "reading a variable that IS set counted as unset: the counter cannot tell "
                    "the two apart and no census built on it means anything\n");
    return 1;
  }
  if(found.t == V_REAL && found.d == 0.0){
    fprintf(stderr, "the control read answered zero, so it may not have found anything\n");
    return 1;
  }

  printf("unset reads: counted %lu, answered zero throughout, and a set variable counted none\n",
         gml_vm_unset_reads(&vm));
  return 0;
}
