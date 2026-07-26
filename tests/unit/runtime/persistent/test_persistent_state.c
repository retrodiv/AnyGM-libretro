/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "persistent_test_fixture.h"

#include "gml_particle.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


int expect_vm_state_case(void){
  GmlWin win={0};
  GmlVM vm={0};
  vm.win=&win;
  vm.particles=gml_particle_state_create(&vm);
  if(!vm.particles) return 0;
  *gml_varmap_put(&vm.globals,"state_value")=vreal(37);
  size_t size=gml_vm_state_size(&vm),first_size=0,second_size=0,used=0;
  void *first=malloc(size?size:1);
  void *second=malloc(size?size:1);
  int ok=first && second &&
    gml_vm_state_save(&vm,first,size,&first_size) && first_size==size &&
    gml_vm_state_save(&vm,second,size,&second_size) && second_size==first_size &&
    !memcmp(first,second,size);
  *gml_varmap_put(&vm.globals,"state_value")=vreal(-1);
  if(ok) ok=gml_vm_state_load(&vm,first,size,&used) && used==size;
  GmlVal *value=gml_varmap_get(&vm.globals,"state_value");
  ok=ok && value && value->t==V_REAL && value->d==37;
  if(!ok) fprintf(stderr,"canonical VM state fixture failed\n");
  free(second);
  free(first);
  gml_vm_free(&vm);
  return ok;
}
