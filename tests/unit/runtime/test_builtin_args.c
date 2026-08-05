/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Runtime values reach 32-bit builtin arguments as doubles, and content passes negative ones as a
 * matter of course: -1 is the ordinary "no tint" colour.
 *
 * Converting a negative double directly to an unsigned type is undefined in C, and the two
 * architectures this runtime targets disagree in the worst possible way — one wraps to all-ones,
 * the other saturates to zero. A draw that asked for no tint therefore multiplied by white on one
 * host and by black on the other, erasing the drawn artwork on the second.
 *
 * These cases pin the modular result so the conversion cannot drift back to whatever the host
 * hardware happens to do. */
#include "gml_builtin_internal.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void expect(const char *label,uint32_t actual,uint32_t expected){
  if(actual==expected) return;
  fprintf(stderr,"builtin args: %s expected %08x, got %08x\n",label,expected,actual);
  failures++;
}

/* Reading through volatile keeps the compiler from folding these at build time: a folded constant
 * hides the hardware conversion this case exists to pin, and the defect only shows in the
 * instruction the host actually executes. */
static double runtime_value(double value){
  volatile double held=value;
  return held;
}

static int wrapping_conversion(void){
  /* Magnitudes past the 32-bit range are where hosts disagree most: one yields the "integer
   * indefinite" pattern, another saturates, and neither is the modular answer content expects. */
  expect("one past the range at runtime",U32(runtime_value(4294967296.0)),0u);
  expect("one below the range at runtime",U32(runtime_value(-4294967296.0)),0u);
  expect("far past the range at runtime",U32(runtime_value(12884901888.0)),0u);
  expect("no-tint colour at runtime",U32(runtime_value(-1.0)),0xFFFFFFFFu);
  expect("minus 65536 at runtime",U32(runtime_value(-65536.0)),0xFFFF0000u);

  /* The colour every untinted draw passes. Saturating this to zero blacks out the draw. */
  expect("no-tint colour",U32(-1.0),0xFFFFFFFFu);
  expect("no-tint colour masked to 24 bits",U32(-1.0)&0xFFFFFFu,0xFFFFFFu);

  expect("zero",U32(0.0),0u);
  expect("white",U32(16777215.0),16777215u);
  expect("largest 32-bit value",U32(4294967295.0),4294967295u);

  /* Negative values wrap modulo 2^32 rather than clamping. */
  expect("minus two",U32(-2.0),0xFFFFFFFEu);
  expect("minus 65536",U32(-65536.0),0xFFFF0000u);

  /* Out-of-range magnitudes stay modular instead of saturating at either end. */
  expect("one past the range",U32(4294967296.0),0u);
  expect("one below the range",U32(-4294967296.0),0u);
  expect("two ranges up",U32(8589934592.0),0u);

  /* Fractional inputs truncate toward zero before the modular wrap. */
  expect("truncates toward zero",U32(7.9),7u);
  expect("truncates a negative toward zero",U32(-0.5),0u);
  expect("truncates then wraps",U32(-1.5),0xFFFFFFFFu);

  /* Non-finite input has no modular value; it must not read as an arbitrary colour. */
  expect("not a number",U32(0.0/0.0),0u);

  return failures==0;
}

static int argument_reader(void){
  GmlVal args[3];
  memset(args,0,sizeof args);
  args[0]=vreal(-1);
  args[1]=vreal(16711680);
  args[2]=vreal(0);
  expect("negative argument wraps",NU32(args,3,0),0xFFFFFFFFu);
  expect("ordinary colour argument",NU32(args,3,1),16711680u);
  expect("missing argument reads zero",NU32(args,3,7),0u);
  return failures==0;
}

int main(void){
  wrapping_conversion();
  argument_reader();
  if(failures) return 1;
  puts("builtin 32-bit argument conversion: ok");
  return 0;
}
