/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me> */
#include "gml_vm.h"
#include "gml_particle.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int gml_input_key(int key, int edge){ (void)key; (void)edge; return 0; }
int gml_input_gamepad(int button, int edge){ (void)button; (void)edge; return 0; }

int main(void){
  GmlWin classic;
  GmlVM vm;
  memset(&classic,0,sizeof(classic));
  memset(&vm,0,sizeof(vm));
  classic.classic_version=810;
  vm.win=&classic;
  gml_rng_seed(&vm,0);
  double first=gml_rng_value(&vm);
  double second=gml_rng_value(&vm);
  double expected_first=1.0/4294967296.0;
  double expected_second=(double)0x08088406u/4294967296.0;
  if(first!=expected_first || second!=expected_second || vm.rng_classic_state!=0x08088406u){
    fprintf(stderr,"classic RNG sequence mismatch: %.17g %.17g state=%08x\n",
      first,second,vm.rng_classic_state);
    return 1;
  }
  size_t size=gml_vm_state_size(&vm), written=0, used=0;
  void *state=malloc(size);
  if(!state || !gml_vm_state_save(&vm,state,size,&written) || written!=size) return 1;
  double expected_third=(double)(0x08088406u*0x08088405u+1u)/4294967296.0;
  (void)gml_rng_value(&vm);
  if(!gml_vm_state_load(&vm,state,written,&used) || used!=written ||
     gml_rng_value(&vm)!=expected_third){
    fprintf(stderr,"classic RNG state roundtrip mismatch\n");
    free(state);
    return 1;
  }
  free(state);

  gml_part_reset_all();
  gml_part_bind_vm(&vm);
  gml_rng_seed(&vm,0);
  int system=gml_part_system_create();
  int type=gml_part_type_create();
  gml_part_particles_create(system,0,0,type,1);
  if(vm.rng_classic_state!=1u){
    fprintf(stderr,"constant particle ranges advanced the classic RNG\n");
    return 1;
  }
  gml_part_type_size(type,1,2,0,0);
  gml_rng_seed(&vm,0);
  gml_part_particles_create(system,0,0,type,1);
  if(vm.rng_classic_state!=0x08088406u){
    fprintf(stderr,"variable particle range RNG sequence mismatch\n");
    return 1;
  }
  gml_part_reset_all();
  puts("classic RNG fixtures: ok");
  return 0;
}
