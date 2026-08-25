/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Synthetic comparison checks for array references and nonnumeric values. */
#include "gml_vm.h"

#include <stdio.h>
#include <string.h>

static int expect(const char *label, int actual, int want){
  if(actual==want) return 1;
  fprintf(stderr,"%s: expected %d, got %d\n",label,want,actual);
  return 0;
}

int main(void){
  GmlVM vm;
  memset(&vm,0,sizeof vm);
  int ok=1;

  GmlArr one={0}, two={0};
  GmlVal array_one=vundef(); array_one.t=V_ARR; array_one.arr=&one;
  GmlVal array_two=vundef(); array_two.t=V_ARR; array_two.arr=&two;
  GmlVal zero=vreal(0);
  GmlVal text=vstr("0");

  ok &= expect("an array is not equal to zero",
               gml_vm_value_compare(&vm,array_one,zero,CMP_EQ),0);
  ok &= expect("an array differs from zero",
               gml_vm_value_compare(&vm,array_one,zero,CMP_NEQ),1);
  ok &= expect("zero is not equal to an array",
               gml_vm_value_compare(&vm,zero,array_one,CMP_EQ),0);
  ok &= expect("an array is not equal to a string",
               gml_vm_value_compare(&vm,array_one,text,CMP_EQ),0);
  ok &= expect("an array is equal to itself",
               gml_vm_value_compare(&vm,array_one,array_one,CMP_EQ),1);
  ok &= expect("two arrays are different values",
               gml_vm_value_compare(&vm,array_one,array_two,CMP_EQ),0);
  ok &= expect("two arrays are unequal",
               gml_vm_value_compare(&vm,array_one,array_two,CMP_NEQ),1);
  /* Ordered comparisons have no answer for a reference, exactly as they have none for undefined. */
  ok &= expect("an array does not order against a number",
               gml_vm_value_compare(&vm,array_one,zero,CMP_GT),0);
  ok &= expect("a number does not order against an array",
               gml_vm_value_compare(&vm,zero,array_one,CMP_LT),0);

  /* The rule the undefined value already had, kept alongside it. */
  ok &= expect("undefined equals undefined",
               gml_vm_value_compare(&vm,vundef(),vundef(),CMP_EQ),1);
  ok &= expect("undefined differs from zero",
               gml_vm_value_compare(&vm,vundef(),zero,CMP_NEQ),1);

  if(!ok) return 1;
  printf("ok\n");
  return 0;
}
