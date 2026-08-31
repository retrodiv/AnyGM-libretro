/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* A CONV to bool must produce 0 or 1, not merely relabel the slot. Branch opcodes
 * test truthiness, while arithmetic and bitwise opcodes read the numeric result. */
#include "gml_win.h"
#include "gml_value_internal.h"
#include "gml_vm.h"
#include "gml_vm_internal.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void says_real(const char *label, GmlVal got, double want){
  if(got.t!=V_REAL || got.d!=want){
    fprintf(stderr,"conv: %s: expected %g, got type %d value %g\n",label,want,(int)got.t,got.d);
    failures++;
  }
}

int main(void){
  /* The case that was wrong: a non-zero number that is not 1. */
  says_real("15 to bool is 1", gml_vm_conv_value(vreal(15),DT_BOOL), 1);
  says_real("0 to bool is 0",  gml_vm_conv_value(vreal(0),DT_BOOL),  0);

  /* The conversion must use the language's own truthiness and not C's. GameMaker calls a real
     true when it is >= 0.5, so a negative number is false and 0.5 is the boundary that is true.
     Writing !=0 here instead would be a second bug wearing the first one's clothes. */
  says_real("-3 to bool is 0",   gml_vm_conv_value(vreal(-3),DT_BOOL),   0);
  says_real("0.25 to bool is 0", gml_vm_conv_value(vreal(0.25),DT_BOOL), 0);
  says_real("0.5 to bool is 1",  gml_vm_conv_value(vreal(0.5),DT_BOOL),  1);
  says_real("1 to bool stays 1", gml_vm_conv_value(vreal(1),DT_BOOL),    1);

  /* Every other conversion is a relabelling and must not touch the value: rounding here would
     change arithmetic everywhere, which is the opposite mistake. */
  says_real("15 to int32 is untouched", gml_vm_conv_value(vreal(15),DT_INT32), 15);
  says_real("0.5 to double is untouched", gml_vm_conv_value(vreal(0.5),DT_DOUBLE), 0.5);
  says_real("-3 to int16 is untouched", gml_vm_conv_value(vreal(-3),DT_INT16), -3);

  /* Undefined stays undefined rather than becoming false: the two are distinguishable everywhere
     else in this VM and collapsing them here would hide an unset read. */
  {
    GmlVal undefined=gml_vm_conv_value(vundef(),DT_BOOL);
    if(undefined.t!=V_UNDEF){
      fprintf(stderr,"conv: undefined to bool became type %d\n",(int)undefined.t);
      failures++;
    }
  }

  printf(failures?"FAILED\n":"PASSED\n");
  return failures?1:0;
}
