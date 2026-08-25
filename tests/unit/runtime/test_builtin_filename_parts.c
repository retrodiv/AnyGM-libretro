/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* filename_name, filename_ext, filename_path, filename_dir, filename_drive, filename_change_ext.
 *
 * These operations split path strings without opening or resolving any file; the checks
 * therefore use synthetic names without a filesystem.
 *
 * Three distinctions are the point, because an implementation can be wrong about any of them and
 * still look right: filename_path keeps the final separator while filename_dir drops it; the
 * extension carries its own dot and an absent extension is the empty string rather than the whole
 * name; and a dot inside a directory is not an extension.
 */
#include "gml_vm.h"

#include <stdio.h>
#include <string.h>

GmlVal gml_builtin_call(GmlVM *vm, const char *name, GmlVal *args, int count);

static int expect(const char *label, GmlVal actual, const char *want){
  int ok=actual.t==V_STR && actual.s && !strcmp(actual.s,want);
  if(!ok)
    fprintf(stderr,"%s: expected \"%s\", got %s\n",label,want,
            actual.t==V_STR && actual.s?actual.s:"<non-string>");
  gml_values_release(&actual,1);
  return ok;
}

int main(void){
  GmlVM vm;
  memset(&vm,0,sizeof vm);
  int ok=1;

  GmlVal windows[]={vstr("C:\\saves\\slot1.sav")};
  ok &= expect("the name keeps its extension and loses its path",
               gml_builtin_call(&vm,"filename_name",windows,1),"slot1.sav");
  ok &= expect("the extension carries its dot",
               gml_builtin_call(&vm,"filename_ext",windows,1),".sav");
  ok &= expect("the path keeps its final separator",
               gml_builtin_call(&vm,"filename_path",windows,1),"C:\\saves\\");
  ok &= expect("the directory drops the separator the path keeps",
               gml_builtin_call(&vm,"filename_dir",windows,1),"C:\\saves");
  ok &= expect("the drive is the letter and its colon",
               gml_builtin_call(&vm,"filename_drive",windows,1),"C:");

  /* Content authored anywhere but Windows reaches the same names with the other separator. */
  GmlVal posix[]={vstr("saves/slot1.sav")};
  ok &= expect("a forward slash separates too",
               gml_builtin_call(&vm,"filename_name",posix,1),"slot1.sav");
  ok &= expect("and the path it produces keeps that separator",
               gml_builtin_call(&vm,"filename_path",posix,1),"saves/");

  /* A bare name has no path, directory or drive. */
  GmlVal bare[]={vstr("readme.txt")};
  ok &= expect("a bare name is its own name",
               gml_builtin_call(&vm,"filename_name",bare,1),"readme.txt");
  ok &= expect("a bare name has no path",
               gml_builtin_call(&vm,"filename_path",bare,1),"");
  ok &= expect("a bare name has no directory",
               gml_builtin_call(&vm,"filename_dir",bare,1),"");
  ok &= expect("a bare name has no drive",
               gml_builtin_call(&vm,"filename_drive",bare,1),"");

  GmlVal extensionless[]={vstr("LICENSE")};
  ok &= expect("an absent extension is empty, not the name",
               gml_builtin_call(&vm,"filename_ext",extensionless,1),"");

  /* A dot in a directory is not an extension. */
  GmlVal dotted_directory[]={vstr("saves.old/file")};
  ok &= expect("a dot in a directory is not an extension",
               gml_builtin_call(&vm,"filename_ext",dotted_directory,1),"");
  ok &= expect("and the name after it is still the name",
               gml_builtin_call(&vm,"filename_name",dotted_directory,1),"file");

  /* The last dot wins, so a doubled extension changes only its tail. */
  GmlVal doubled[]={vstr("archive.tar.gz")};
  ok &= expect("the last dot is the extension",
               gml_builtin_call(&vm,"filename_ext",doubled,1),".gz");

  GmlVal changed[]={vstr("C:\\saves\\slot1.sav"),vstr(".bak")};
  ok &= expect("the new extension replaces the old one",
               gml_builtin_call(&vm,"filename_change_ext",changed,2),"C:\\saves\\slot1.bak");
  GmlVal stripped[]={vstr("C:\\saves\\slot1.sav"),vstr("")};
  ok &= expect("an empty extension removes it",
               gml_builtin_call(&vm,"filename_change_ext",stripped,2),"C:\\saves\\slot1");
  GmlVal added[]={vstr("LICENSE"),vstr(".txt")};
  ok &= expect("a name with no extension gains one",
               gml_builtin_call(&vm,"filename_change_ext",added,2),"LICENSE.txt");

  if(ok) printf("filename parts: synthetic path splitting passed\n");
  return ok?0:1;
}
