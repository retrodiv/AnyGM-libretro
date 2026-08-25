/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Coverage counts the CODE entries a run entered, and counts nothing when nobody asked.
 *
 * It answers which code entries a route reached, independently of its room count.
 *
 * Deliberately separate from the per-script profiler beside it. That one reads a clock twice per
 * call and cannot be left on; this is a single byte store, so it can.
 *
 * Three properties are pinned here. Off by default, because a diagnostic that allocates when nobody
 * asked is one that gets left on. Idempotent per entry, because the number has to mean "how many
 * entries ran" and not "how many times they ran" — the second is the profiler's job, and confusing
 * them would make one hot loop look like broad coverage. And bounded by the payload, because an
 * index outside it must not write outside the array.
 */
#include "gml_vm.h"
#include "gml_vm_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *setting_from_environment(void *userdata, const char *name){
  (void)userdata;
  return getenv(name);
}

static int entries_marked(const GmlVM *vm){
  int seen = 0;
  if(!vm->diagnostics.code_seen) return -1;
  for(int i = 0; i < vm->win->n_code; i++) if(vm->diagnostics.code_seen[i]) seen++;
  return seen;
}

/* A payload with three empty code entries. Empty is enough: the mark is taken on entry, before any
 * instruction runs, which is exactly the point at which "this entry was reached" becomes true. */
static void build(GmlWin *win, GmlCode *entries, int count){
  memset(win, 0, sizeof *win);
  memset(entries, 0, sizeof(GmlCode) * (size_t)count);
  for(int i = 0; i < count; i++){ entries[i].start = 0; entries[i].length = 0; }
  win->code = entries;
  win->n_code = count;
}

static int failures = 0;
static void check(int condition, const char *what){
  if(!condition){ fprintf(stderr, "code coverage: %s\n", what); failures++; }
}

int main(void){
  AnygmHostServices services = {0};
  services.struct_size = sizeof services;
  services.abi_version = ANYGM_HOST_SERVICES_VERSION;
  services.development_setting = setting_from_environment;

  GmlCode entries[3];
  GmlWin win;

  {
    unsetenv("GML_LOG_COVERAGE");
    GmlVM vm; memset(&vm, 0, sizeof vm);
    vm.host = &services; vm.diagnostics.code_coverage = -1;
    build(&win, entries, 3); vm.win = &win;
    gml_vm_run_code(&vm, 0, NULL, NULL, NULL, 0);
    check(vm.diagnostics.code_seen == NULL, "allocated with the setting absent");
    free(vm.diagnostics.code_seen);
  }
  {
    setenv("GML_LOG_COVERAGE", "1", 1);
    GmlVM vm; memset(&vm, 0, sizeof vm);
    vm.host = &services; vm.diagnostics.code_coverage = -1;
    build(&win, entries, 3); vm.win = &win;

    check(entries_marked(&vm) == -1, "counted something before anything ran");
    gml_vm_run_code(&vm, 1, NULL, NULL, NULL, 0);
    check(entries_marked(&vm) == 1, "one entry run did not mark exactly one");
    gml_vm_run_code(&vm, 1, NULL, NULL, NULL, 0);
    gml_vm_run_code(&vm, 1, NULL, NULL, NULL, 0);
    check(entries_marked(&vm) == 1, "running one entry three times marked more than one");
    gml_vm_run_code(&vm, 2, NULL, NULL, NULL, 0);
    check(entries_marked(&vm) == 2, "a second entry was not counted");
    gml_vm_run_code(&vm, 99, NULL, NULL, NULL, 0);
    gml_vm_run_code(&vm, -1, NULL, NULL, NULL, 0);
    check(entries_marked(&vm) == 2, "an index outside the payload was counted");
    check(vm.diagnostics.code_seen[0] == 0, "an entry that never ran was marked");
    free(vm.diagnostics.code_seen);
    unsetenv("GML_LOG_COVERAGE");
  }

  if(failures){ fprintf(stderr, "code coverage: %d check(s) failed\n", failures); return 1; }
  printf("code coverage: ok\n");
  return 0;
}
