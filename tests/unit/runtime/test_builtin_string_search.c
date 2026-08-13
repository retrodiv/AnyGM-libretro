/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Synthetic positional string-search and civil-datetime cases.
 * Forward search skips the first start-position characters; backward search
 * accepts a match beginning at or before that many characters in. */
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

  GmlVal fwd0[]={vstr("o"),vstr("Hello World"),vreal(0)};
  ok &= expect("a zero start searches from the beginning",
               gml_builtin_call(&vm,"string_pos_ext",fwd0,3),5);

  GmlVal fwd5[]={vstr("o"),vstr("Hello World"),vreal(5)};
  ok &= expect("the first start_pos characters are skipped",
               gml_builtin_call(&vm,"string_pos_ext",fwd5,3),8);

  GmlVal fwdpast[]={vstr("o"),vstr("Hello World"),vreal(11)};
  ok &= expect("a start at or past the end finds nothing",
               gml_builtin_call(&vm,"string_pos_ext",fwdpast,3),0);

  GmlVal fwdneg[]={vstr("("),vstr(":::gml_Script_x(...)"),vreal(-3)};
  ok &= expect("a negative start clamps to the beginning",
               gml_builtin_call(&vm,"string_pos_ext",fwdneg,3),16);

  GmlVal fwdmiss[]={vstr("z"),vstr("Hello World"),vreal(0)};
  ok &= expect("an absent needle answers zero",
               gml_builtin_call(&vm,"string_pos_ext",fwdmiss,3),0);

  GmlVal last[]={vstr("o"),vstr("Hello World")};
  ok &= expect("the last occurrence is reported",
               gml_builtin_call(&vm,"string_last_pos",last,2),8);

  GmlVal lastone[]={vstr("H"),vstr("Hello World")};
  ok &= expect("a sole occurrence at the start is found",
               gml_builtin_call(&vm,"string_last_pos",lastone,2),1);

  GmlVal lastmiss[]={vstr("z"),vstr("Hello World")};
  ok &= expect("an absent needle answers zero backwards too",
               gml_builtin_call(&vm,"string_last_pos",lastmiss,2),0);

  GmlVal back6[]={vstr("o"),vstr("Hello World"),vreal(6)};
  ok &= expect("the backward search accepts a match beginning at or before start_pos",
               gml_builtin_call(&vm,"string_last_pos_ext",back6,3),5);

  GmlVal back7[]={vstr("o"),vstr("Hello World"),vreal(7)};
  ok &= expect("a match beginning exactly start_pos characters in is accepted",
               gml_builtin_call(&vm,"string_last_pos_ext",back7,3),8);

  GmlVal backwide[]={vstr("o"),vstr("Hello World"),vreal(999)};
  ok &= expect("a start past the end degrades to the plain backward search",
               gml_builtin_call(&vm,"string_last_pos_ext",backwide,3),8);

  /* The Unix epoch is serial 25569: the anchor both conversions in this family already use. */
  GmlVal epoch[]={vreal(1970),vreal(1),vreal(1),vreal(0),vreal(0),vreal(0)};
  ok &= expect("the Unix epoch lands on the family's anchor serial",
               gml_builtin_call(&vm,"date_create_datetime",epoch,6),25569.0);

  GmlVal noon[]={vreal(1970),vreal(1),vreal(2),vreal(12),vreal(0),vreal(0)};
  ok &= expect("a day and a half advances the serial by 1.5",
               gml_builtin_call(&vm,"date_create_datetime",noon,6),25570.5);

  GmlVal leap[]={vreal(2000),vreal(3),vreal(1),vreal(0),vreal(0),vreal(0)};
  GmlVal before[]={vreal(2000),vreal(2),vreal(29),vreal(0),vreal(0),vreal(0)};
  double a=gml_builtin_call(&vm,"date_create_datetime",leap,6).d;
  double b=gml_builtin_call(&vm,"date_create_datetime",before,6).d;
  ok &= expect("Gregorian leap day advances by one serial day",vreal(a-b),1.0);

  /* Round-trip through the family's own reader, so the two directions cannot drift apart. */
  GmlVal made[]={vreal(2026),vreal(8),vreal(13),vreal(14),vreal(30),vreal(15)};
  GmlVal serial=gml_builtin_call(&vm,"date_create_datetime",made,6);
  GmlVal back[]={serial};
  ok &= expect("the created year reads back",gml_builtin_call(&vm,"date_get_year",back,1),2026);
  ok &= expect("the created month reads back",gml_builtin_call(&vm,"date_get_month",back,1),8);
  ok &= expect("the created day reads back",gml_builtin_call(&vm,"date_get_day",back,1),13);
  ok &= expect("the created hour reads back",gml_builtin_call(&vm,"date_get_hour",back,1),14);
  ok &= expect("the created minute reads back",gml_builtin_call(&vm,"date_get_minute",back,1),30);
  ok &= expect("the created second reads back",gml_builtin_call(&vm,"date_get_second",back,1),15);

  if(ok) printf("string search: positional and backward searches answer GameMaker positions\n");
  return ok?0:1;
}
