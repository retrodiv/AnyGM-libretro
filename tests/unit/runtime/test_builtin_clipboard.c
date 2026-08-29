/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Exercise clipboard_set_text, clipboard_get_text and clipboard_has_text with synthetic
 * values. A get result must own its storage: the next set replaces the process buffer, so a
 * borrowed pointer would become invalid. Keep the first read alive across that replacement. */
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

  ok &= expect("an untouched clipboard holds no text",
               gml_builtin_call(&vm,"clipboard_has_text",NULL,0),0);

  GmlVal empty[]={vstr("")};
  gml_builtin_call(&vm,"clipboard_set_text",empty,1);
  ok &= expect("the empty string is not text",
               gml_builtin_call(&vm,"clipboard_has_text",NULL,0),0);

  GmlVal cake[]={vstr("cake")};
  gml_builtin_call(&vm,"clipboard_set_text",cake,1);
  ok &= expect("text that was set is text",
               gml_builtin_call(&vm,"clipboard_has_text",NULL,0),1);

  /* Read it, then replace the clipboard underneath that read. The first value must still be
   * readable: it is the caller's, not a window onto the buffer. */
  GmlVal first=gml_builtin_call(&vm,"clipboard_get_text",NULL,0);
  if(!(first.t==V_STR && first.s && !strcmp(first.s,"cake"))){
    fprintf(stderr,"clipboard_get_text: expected \"cake\"\n");
    ok=0;
  }
  GmlVal replacement[]={vstr("a longer clipboard string")};
  gml_builtin_call(&vm,"clipboard_set_text",replacement,1);
  if(!(first.t==V_STR && first.s && !strcmp(first.s,"cake"))){
    fprintf(stderr,"a read taken before a set did not survive it: got %s\n",
            first.t==V_STR && first.s?first.s:"<non-string>");
    ok=0;
  }
  gml_values_release(&first,1);

  GmlVal second=gml_builtin_call(&vm,"clipboard_get_text",NULL,0);
  if(!(second.t==V_STR && second.s && !strcmp(second.s,"a longer clipboard string"))){
    fprintf(stderr,"the replacement did not read back\n");
    ok=0;
  }
  gml_values_release(&second,1);

  if(ok) printf("clipboard: text set is text read, and a read survives the next set\n");
  return ok?0:1;
}
