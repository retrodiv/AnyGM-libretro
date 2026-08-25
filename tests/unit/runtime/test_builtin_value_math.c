/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* log2, ln, exp, the dot products, is_nan, is_infinity and string_starts_with.
 *
 * The checks distinguish mathematical values, types and argument shapes; unknown names
 * would otherwise return the generic zero value.
 *
 * The assertions here are the ones that separate a correct implementation from a plausible one:
 * the base of the logarithm, the difference between the plain and normalised dot products, and the
 * fact that only a real can be NaN or infinite - a string coerced to a number must not become
 * either.
 */
#include "gml_vm.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

GmlVal gml_builtin_call(GmlVM *vm, const char *name, GmlVal *args, int count);

static int expect(const char *label, GmlVal actual, double want){
  if(actual.t==V_REAL && fabs(actual.d-want)<1e-9) return 1;
  fprintf(stderr,"%s: expected %g, got %g\n",label,want,actual.t==V_REAL?actual.d:-999);
  return 0;
}

int main(void){
  GmlVM vm;
  memset(&vm,0,sizeof vm);
  int ok=1;

  /* ---- logarithms and the exponent ---- */
  GmlVal four[]={vreal(4)};
  ok &= expect("log2 of 4 is 2",gml_builtin_call(&vm,"log2",four,1),2.0);
  GmlVal big[]={vreal(256)};
  ok &= expect("log2 of 256 is 8",gml_builtin_call(&vm,"log2",big,1),8.0);
  GmlVal one[]={vreal(1)};
  ok &= expect("ln of 1 is 0",gml_builtin_call(&vm,"ln",one,1),0.0);
  /* Names the base: the natural logarithm of 100 is 4.605, the base-ten one is 2. */
  GmlVal hundred[]={vreal(100)};
  ok &= expect("ln is the natural logarithm",gml_builtin_call(&vm,"ln",hundred,1),4.605170185988092);
  GmlVal zero[]={vreal(0)};
  ok &= expect("exp of 0 is 1",gml_builtin_call(&vm,"exp",zero,1),1.0);
  GmlVal unit[]={vreal(1)};
  ok &= expect("exp of 1 is e",gml_builtin_call(&vm,"exp",unit,1),2.718281828459045);
  GmlVal three[]={vreal(3)};
  GmlVal e3=gml_builtin_call(&vm,"exp",three,1);
  GmlVal back[]={e3};
  ok &= expect("ln undoes exp",gml_builtin_call(&vm,"ln",back,1),3.0);

  /* ---- dot products ---- */
  GmlVal perpendicular[]={vreal(1),vreal(0),vreal(0),vreal(1)};
  ok &= expect("perpendicular vectors have no projection",
               gml_builtin_call(&vm,"dot_product",perpendicular,4),0.0);
  GmlVal along[]={vreal(3),vreal(4),vreal(3),vreal(4)};
  ok &= expect("a vector against itself is its squared length",
               gml_builtin_call(&vm,"dot_product",along,4),25.0);
  /* The normalised form is the cosine, so the same pair answers 1 rather than 25. */
  ok &= expect("the normalised form answers the cosine",
               gml_builtin_call(&vm,"dot_product_normalised",along,4),1.0);
  ok &= expect("the American spelling is the same operation",
               gml_builtin_call(&vm,"dot_product_normalized",along,4),1.0);
  GmlVal opposed[]={vreal(1),vreal(0),vreal(-1),vreal(0)};
  ok &= expect("opposed unit vectors answer minus one",
               gml_builtin_call(&vm,"dot_product_normalised",opposed,4),-1.0);
  /* A zero-length vector has no direction: answer 0 rather than dividing by zero. */
  GmlVal degenerate[]={vreal(0),vreal(0),vreal(1),vreal(1)};
  ok &= expect("a vector with no length normalises to nothing",
               gml_builtin_call(&vm,"dot_product_normalised",degenerate,4),0.0);
  GmlVal spatial[]={vreal(1),vreal(2),vreal(3),vreal(4),vreal(5),vreal(6)};
  ok &= expect("the 3d form sums three products",
               gml_builtin_call(&vm,"dot_product_3d",spatial,6),32.0);
  GmlVal spatial_along[]={vreal(0),vreal(0),vreal(2),vreal(0),vreal(0),vreal(9)};
  ok &= expect("the normalised 3d form answers the cosine",
               gml_builtin_call(&vm,"dot_product_3d_normalised",spatial_along,6),1.0);

  /* ---- the two predicates ---- */
  GmlVal nan_value[]={vreal(NAN)};
  ok &= expect("a NaN is recognised",gml_builtin_call(&vm,"is_nan",nan_value,1),1.0);
  GmlVal inf_value[]={vreal(INFINITY)};
  ok &= expect("an infinity is recognised",gml_builtin_call(&vm,"is_infinity",inf_value,1),1.0);
  GmlVal neg_inf[]={vreal(-INFINITY)};
  ok &= expect("a negative infinity is one too",
               gml_builtin_call(&vm,"is_infinity",neg_inf,1),1.0);
  GmlVal ordinary[]={vreal(42)};
  ok &= expect("an ordinary number is neither",gml_builtin_call(&vm,"is_nan",ordinary,1),0.0);
  ok &= expect("an ordinary number is not infinite",
               gml_builtin_call(&vm,"is_infinity",ordinary,1),0.0);
  /* A string is neither, and must not become either by being coerced to a number first. */
  GmlVal text[]={vstr("nan")};
  ok &= expect("a string is not a NaN",gml_builtin_call(&vm,"is_nan",text,1),0.0);
  GmlVal text_inf[]={vstr("inf")};
  ok &= expect("a string is not an infinity",
               gml_builtin_call(&vm,"is_infinity",text_inf,1),0.0);
  ok &= expect("no argument is not a NaN",gml_builtin_call(&vm,"is_nan",NULL,0),0.0);

  /* ---- the prefix test, beside the suffix test it was missing next to ---- */
  GmlVal has_prefix[]={vstr("status-ready"),vstr("status")};
  ok &= expect("a matching prefix is reported",
               gml_builtin_call(&vm,"string_starts_with",has_prefix,2),1.0);
  GmlVal wrong_case[]={vstr("status-ready"),vstr("STATUS")};
  ok &= expect("prefix matching is case-sensitive",
               gml_builtin_call(&vm,"string_starts_with",wrong_case,2),0.0);
  GmlVal empty_prefix[]={vstr("status-ready"),vstr("")};
  ok &= expect("every string starts with the empty string",
               gml_builtin_call(&vm,"string_starts_with",empty_prefix,2),1.0);
  GmlVal too_long[]={vstr("ok"),vstr("okay")};
  ok &= expect("a prefix longer than the string does not match",
               gml_builtin_call(&vm,"string_starts_with",too_long,2),0.0);
  /* The suffix, not the prefix: the two must not answer each other. */
  GmlVal suffix_not_prefix[]={vstr("status-ready"),vstr("ready")};
  ok &= expect("a suffix is not a prefix",
               gml_builtin_call(&vm,"string_starts_with",suffix_not_prefix,2),0.0);

  if(ok) printf("value math: logarithms, dot products, the two predicates and the prefix test\n");
  return ok?0:1;
}
