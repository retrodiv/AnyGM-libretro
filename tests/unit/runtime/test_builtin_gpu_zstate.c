/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Check that GPU and D3D depth-control names produce equivalent state for both
 * boolean values. This compares behavior without pinning an internal flag-array index. */
#include "gml_vm.h"
#include "gml_software3d.h"

#include <stdio.h>
#include <string.h>

GmlVal gml_builtin_call(GmlVM *vm, const char *name, GmlVal *args, int count);

static void snapshot(GmlVM *vm, int flags[GML_SOFTWARE3D_STATE_FLAG_COUNT]){
  double values[GML_SOFTWARE3D_STATE_VALUE_COUNT];
  uint32_t colors[GML_SOFTWARE3D_STATE_COLOR_COUNT];
  gml_vm_software3d_state_get(vm, flags, values, colors);
}

/* Set one name on a freshly reset 3D state and report the flags it leaves. */
static void after(GmlVM *vm, const char *name, double argument,
                  int flags[GML_SOFTWARE3D_STATE_FLAG_COUNT]){
  gml_vm_software3d_reset(vm);
  GmlVal a[] = { vreal(argument) };
  gml_builtin_call(vm, name, a, 1);
  snapshot(vm, flags);
}

static int same(const char *left, const char *right, double argument,
                const int a[GML_SOFTWARE3D_STATE_FLAG_COUNT],
                const int b[GML_SOFTWARE3D_STATE_FLAG_COUNT]){
  if(!memcmp(a, b, sizeof(int) * GML_SOFTWARE3D_STATE_FLAG_COUNT)) return 1;
  fprintf(stderr, "%s(%g) and %s(%g) leave different 3D state\n",
          left, argument, right, argument);
  return 0;
}

int main(void){
  GmlVM vm;
  memset(&vm, 0, sizeof vm);
  int ok = 1;
  int viaD3D[GML_SOFTWARE3D_STATE_FLAG_COUNT], viaGPU[GML_SOFTWARE3D_STATE_FLAG_COUNT];

  /* Both directions: a name that only ever agreed on the default would pass a one-value test. */
  for(int on = 0; on <= 1; on++){
    after(&vm, "d3d_set_zwriteenable", on, viaD3D);
    after(&vm, "gpu_set_zwriteenable", on, viaGPU);
    ok &= same("d3d_set_zwriteenable", "gpu_set_zwriteenable", on, viaD3D, viaGPU);

    after(&vm, "d3d_set_hidden", on, viaD3D);
    after(&vm, "gpu_set_ztestenable", on, viaGPU);
    ok &= same("d3d_set_hidden", "gpu_set_ztestenable", on, viaD3D, viaGPU);
  }

  if(ok) printf("gpu z-state: the GMS2 spellings reach the same control as the d3d ones\n");
  return ok ? 0 : 1;
}
