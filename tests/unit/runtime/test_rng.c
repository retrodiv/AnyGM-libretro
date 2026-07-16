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

static int check_classic_comparisons(void){
  double accumulated=0.0;
  for(int i=0;i<100;i++) accumulated+=0.06;
  if(accumulated>=6.0 ||
     !gml_real_compare(accumulated,6.0,CMP_EQ,1) ||
     gml_real_compare(accumulated,6.0,CMP_NEQ,1) ||
     gml_real_compare(accumulated,6.0,CMP_LT,1) ||
     !gml_real_compare(accumulated,6.0,CMP_LTE,1) ||
     gml_real_compare(accumulated,6.0,CMP_GT,1) ||
     !gml_real_compare(accumulated,6.0,CMP_GTE,1) ||
     gml_real_compare(accumulated,6.0,CMP_EQ,0) ||
     !gml_real_compare(accumulated,6.0,CMP_LT,0)){
    fprintf(stderr,"classic real comparison tolerance mismatch: %.17g\n",accumulated);
    return 0;
  }
  if(gml_real_compare(6.0-1e-12,6.0,CMP_EQ,1) ||
     !gml_real_compare(6.0-1e-12,6.0,CMP_LT,1) ||
     gml_real_compare(6.0+1e-12,6.0,CMP_EQ,1) ||
     !gml_real_compare(6.0+1e-12,6.0,CMP_GT,1)){
    fprintf(stderr,"classic real comparison tolerance exceeded its boundary\n");
    return 0;
  }
  return 1;
}

int main(void){
  GmlWin classic;
  GmlVM vm;
  memset(&classic,0,sizeof(classic));
  memset(&vm,0,sizeof(vm));
  if(!check_classic_comparisons()) return 1;
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
  gml_rng_seed(&vm,0);
  system=gml_part_system_create();
  type=gml_part_type_create();
  int emitter=gml_part_emitter_create(system);
  gml_part_emitter_region(system,emitter,12,12,34,34,0,0);
  gml_part_emitter_burst(system,emitter,type,1);
  GmlVM emitter_expected;
  memset(&emitter_expected,0,sizeof(emitter_expected));
  emitter_expected.win=&classic;
  gml_rng_seed(&emitter_expected,0);
  for(int i=0;i<3;i++) (void)gml_rng_value(&emitter_expected);
  if(vm.rng_classic_state!=emitter_expected.rng_classic_state){
    fprintf(stderr,"classic point-emitter RNG consumption mismatch: got=%08x expected=%08x\n",
            vm.rng_classic_state,emitter_expected.rng_classic_state);
    return 1;
  }
  gml_part_reset_all();
  gml_rng_seed(&vm,0);
  gml_effect_create(1,0,32,24,0,0x40A0FF);
  GmlVM expected;
  memset(&expected,0,sizeof(expected));
  expected.win=&classic;
  gml_rng_seed(&expected,0);
  for(int i=0;i<82;i++) (void)gml_rng_value(&expected);
  if(vm.rng_classic_state!=expected.rng_classic_state){
    fprintf(stderr,"classic explosion RNG consumption mismatch: got=%08x expected=%08x\n",
            vm.rng_classic_state,expected.rng_classic_state);
    return 1;
  }
  gml_part_reset_all();
  gml_rng_seed(&vm,0);
  gml_effect_create(1,7,32,24,0,0x40A0FF);
  memset(&expected,0,sizeof(expected));
  expected.win=&classic;
  gml_rng_seed(&expected,0);
  for(int i=0;i<2;i++) (void)gml_rng_value(&expected);
  if(vm.rng_classic_state!=expected.rng_classic_state){
    fprintf(stderr,"classic spark RNG consumption mismatch: got=%08x expected=%08x\n",
            vm.rng_classic_state,expected.rng_classic_state);
    return 1;
  }
  gml_part_reset_all();
  gml_rng_seed(&vm,0);
  gml_effect_create(1,4,32,24,0,0x808080);
  memset(&expected,0,sizeof(expected));
  expected.win=&classic;
  gml_rng_seed(&expected,0);
  for(int i=0;i<24;i++) (void)gml_rng_value(&expected);
  if(vm.rng_classic_state!=expected.rng_classic_state){
    fprintf(stderr,"classic smoke RNG consumption mismatch: got=%08x expected=%08x\n",
            vm.rng_classic_state,expected.rng_classic_state);
    return 1;
  }
  gml_part_reset_all();
  gml_rng_seed(&vm,0);
  gml_effect_create(1,5,32,24,0,0x808080);
  memset(&expected,0,sizeof(expected));
  expected.win=&classic;
  gml_rng_seed(&expected,0);
  for(int i=0;i<30;i++) (void)gml_rng_value(&expected);
  if(vm.rng_classic_state!=expected.rng_classic_state){
    fprintf(stderr,"classic rising smoke RNG consumption mismatch: got=%08x expected=%08x\n",
            vm.rng_classic_state,expected.rng_classic_state);
    return 1;
  }
  gml_part_reset_all();
  gml_rng_seed(&vm,0);
  gml_effect_create(1,9,32,24,2,0xFFFFFF);
  memset(&expected,0,sizeof(expected));
  expected.win=&classic;
  gml_rng_seed(&expected,0);
  (void)gml_rng_value(&expected);
  if(vm.rng_classic_state!=expected.rng_classic_state){
    fprintf(stderr,"classic cloud RNG consumption mismatch: got=%08x expected=%08x\n",
            vm.rng_classic_state,expected.rng_classic_state);
    return 1;
  }
  gml_part_reset_all();
  gml_rng_seed(&vm,0);
  gml_effect_create(1,11,32,24,2,0xFFFFFF);
  memset(&expected,0,sizeof(expected));
  expected.win=&classic;
  gml_rng_seed(&expected,0);
  for(int i=0;i<49;i++) (void)gml_rng_value(&expected);
  if(vm.rng_classic_state!=expected.rng_classic_state){
    fprintf(stderr,"classic snowfall RNG consumption mismatch: got=%08x expected=%08x\n",
            vm.rng_classic_state,expected.rng_classic_state);
    return 1;
  }
  gml_part_reset_all();
  puts("classic RNG fixtures: ok");
  return 0;
}
