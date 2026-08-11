/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "gml_vm.h"
#include "gml_builtin.h"
#include "gml_particle.h"
#include "anygm_host.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static AnygmResult fixture_wall_time(void *userdata,AnygmWallTime *wall){
  (void)userdata;
  if(!wall || wall->struct_size<sizeof *wall) return ANYGM_ERROR_INVALID_ARGUMENT;
  wall->flags=ANYGM_WALL_TIME_OFFSET_VALID;
  wall->unix_seconds=0;
  wall->utc_offset_minutes=60;
  wall->reserved=0;
  return ANYGM_OK;
}

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
     !gml_real_compare(accumulated,6.0,CMP_EQ,0) ||
     gml_real_compare(accumulated,6.0,CMP_LT,0)){
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
  if(!gml_real_compare(6.0-1e-6,6.0,CMP_EQ,0) ||
     gml_real_compare(6.0-2e-5,6.0,CMP_EQ,0) ||
     !gml_real_compare(6.0-2e-5,6.0,CMP_LT,0)){
    fprintf(stderr,"studio real comparison epsilon mismatch\n");
    return 0;
  }
  return 1;
}

int main(void){
  GmlWin classic;
  GmlVM vm;
  memset(&classic,0,sizeof(classic));
  memset(&vm,0,sizeof(vm));
  AnygmHostServices services={0};
  services.struct_size=sizeof services;
  services.abi_version=ANYGM_HOST_SERVICES_VERSION;
  services.wall_time=fixture_wall_time;
  vm.host=&services;
  vm.particles=gml_particle_state_create(&vm);
  if(!vm.particles) return 1;
  if(!check_classic_comparisons()) return 1;
  {
    static const uint64_t expected[]={
      16,5,13,17,13,0,10,15,15,9,18,14,10,19,16,12
    };
    static const uint64_t expected_selection[]={
      0,0,0,3,3,1,1,1,0,0,0,0,0,0,0,3
    };
    static const uint32_t expected_seeded_words[]={
      0xaac66936u,0xfbf72704u,0x82363772u,0xc5a5c08cu,
      0x8d7c7ddcu,0xb4ce3f12u,0xc41ec733u,0x879fbf68u
    };
    GmlWin studio;
    memset(&studio,0,sizeof studio);
    studio.bytecode=17;
    vm.win=&studio;
    gml_rng_seed(&vm,1);
    if(floor(gml_rng_value(&vm)*32768.0)!=22277.0){
      fprintf(stderr,"studio seeded real RNG range fixture mismatch\n");
      return 1;
    }
    gml_rng_seed(&vm,777);
    for(size_t i=0;i<sizeof expected_seeded_words/sizeof expected_seeded_words[0];i++){
      double actual=gml_rng_value(&vm);
      double expected_value=(double)expected_seeded_words[i]/4294967296.0;
      if(actual!=expected_value){
        fprintf(stderr,"studio seeded RNG mismatch at %zu: got=%.17g expected=%.17g\n",
                i,actual,expected_value);
        return 1;
      }
    }
    gml_rng_seed(&vm,12345);
    for(size_t i=0;i<sizeof expected/sizeof expected[0];i++){
      uint64_t actual=gml_rng_integer(&vm,20);
      if(actual!=expected[i]){
        fprintf(stderr,"studio integer RNG mismatch at %zu: got=%" PRIu64
                       " expected=%" PRIu64 "\n",i,actual,expected[i]);
        return 1;
      }
    }
    gml_rng_seed(&vm,12345);
    if(gml_rng_integer(&vm,16777215u)!=0x2e1bc8u ||
       gml_rng_integer(&vm,16777215u)!=0x10b680u){
      fprintf(stderr,"studio wide integer RNG composition mismatch\n");
      return 1;
    }
    gml_rng_seed(&vm,12345);
    if(gml_rng_integer(&vm,0)!=0 ||
       gml_rng_value(&vm)!=(double)0x3610b680u/4294967296.0){
      fprintf(stderr,"studio zero-bound integer RNG consumption mismatch\n");
      return 1;
    }
    gml_rng_seed(&vm,12345);
    for(size_t i=0;i<sizeof expected_selection/sizeof expected_selection[0];i++){
      uint64_t actual=gml_rng_select(&vm,4);
      if(actual!=expected_selection[i]){
        fprintf(stderr,"studio RNG selection mismatch at %zu: got=%" PRIu64
                       " expected=%" PRIu64 "\n",i,actual,expected_selection[i]);
        return 1;
      }
    }
  }
  classic.classic_version=810;
  vm.win=&classic;
  {
    AnygmCalendarTime expected={0};
    if(!anygm_calendar_from_unix_seconds(60*60,&expected)){
      fprintf(stderr,"fixture calendar setup failed\n");
      return 1;
    }
    if(gml_global_num(&vm,"current_year")!=expected.year ||
       gml_global_num(&vm,"current_month")!=expected.month ||
       gml_global_num(&vm,"current_day")!=expected.day ||
       gml_global_num(&vm,"current_weekday")!=expected.weekday ||
       gml_global_num(&vm,"current_hour")!=expected.hour ||
       gml_global_num(&vm,"current_minute")!=expected.minute){
      fprintf(stderr,"calendar built-in component mismatch\n");
      return 1;
    }
  }
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
  vm.window_w=1024; vm.window_h=600;
  vm.gui_w=512; vm.gui_h=300;
  vm.gui_maximise_active=1;
  vm.gui_maximise_xscale=1.5; vm.gui_maximise_yscale=2.5;
  vm.gui_maximise_xoffset=7.0; vm.gui_maximise_yoffset=-9.0;
  size_t size=gml_vm_state_size(&vm), written=0, used=0;
  void *state=malloc(size);
  if(!state || !gml_vm_state_save(&vm,state,size,&written) || written!=size) return 1;
  void *repeated_state=malloc(size);
  size_t repeated_written=0;
  if(!repeated_state ||
     !gml_vm_state_save(&vm,repeated_state,size,&repeated_written) ||
     repeated_written!=written || memcmp(repeated_state,state,written)){
    fprintf(stderr,"repeated VM serialization was not canonical\n");
    free(repeated_state);
    free(state);
    return 1;
  }
  free(repeated_state);
  double expected_third=(double)(0x08088406u*0x08088405u+1u)/4294967296.0;
  (void)gml_rng_value(&vm);
  vm.window_w=640; vm.window_h=480;
  vm.gui_w=vm.gui_h=0;
  vm.gui_maximise_active=0;
  vm.gui_maximise_xscale=vm.gui_maximise_yscale=0.0;
  vm.gui_maximise_xoffset=vm.gui_maximise_yoffset=0.0;
  int loaded=gml_vm_state_load(&vm,state,written,&used);
  double restored_third=gml_rng_value(&vm);
  if(!loaded || used!=written || restored_third!=expected_third ||
     vm.window_w!=1024 || vm.window_h!=600 || vm.gui_w!=512 || vm.gui_h!=300 ||
     !vm.gui_maximise_active || vm.gui_maximise_xscale!=1.5 ||
     vm.gui_maximise_yscale!=2.5 || vm.gui_maximise_xoffset!=7.0 ||
     vm.gui_maximise_yoffset!=-9.0){
    fprintf(stderr,"classic RNG state roundtrip mismatch: loaded=%d used=%zu/%zu "
                   "random=%.17g/%.17g window=%dx%d gui=%dx%d\n",
      loaded,used,written,restored_third,expected_third,
      vm.window_w,vm.window_h,vm.gui_w,vm.gui_h);
    free(state);
    return 1;
  }
  free(state);

  gml_part_reset_all(vm.particles);
  gml_rng_seed(&vm,0);
  int system=gml_part_system_create(vm.particles);
  int type=gml_part_type_create(vm.particles);
  gml_part_particles_create(vm.particles,system,0,0,type,1);
  if(vm.rng_classic_state!=1u){
    fprintf(stderr,"constant particle ranges advanced the classic RNG\n");
    return 1;
  }
  gml_part_type_size(vm.particles,type,1,2,0,0);
  gml_rng_seed(&vm,0);
  gml_part_particles_create(vm.particles,system,0,0,type,1);
  if(vm.rng_classic_state!=0x08088406u){
    fprintf(stderr,"variable particle range RNG sequence mismatch\n");
    return 1;
  }
  gml_part_reset_all(vm.particles);
  gml_rng_seed(&vm,0);
  system=gml_part_system_create(vm.particles);
  type=gml_part_type_create(vm.particles);
  int emitter=gml_part_emitter_create(vm.particles,system);
  gml_part_emitter_region(vm.particles,system,emitter,12,12,34,34,0,0);
  gml_part_emitter_burst(vm.particles,system,emitter,type,1);
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
  gml_part_reset_all(vm.particles);
  gml_rng_seed(&vm,0);
  gml_effect_create(vm.particles,1,0,32,24,0,0x40A0FF);
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
  gml_part_reset_all(vm.particles);
  gml_rng_seed(&vm,0);
  gml_effect_create(vm.particles,1,7,32,24,0,0x40A0FF);
  memset(&expected,0,sizeof(expected));
  expected.win=&classic;
  gml_rng_seed(&expected,0);
  for(int i=0;i<2;i++) (void)gml_rng_value(&expected);
  if(vm.rng_classic_state!=expected.rng_classic_state){
    fprintf(stderr,"classic spark RNG consumption mismatch: got=%08x expected=%08x\n",
            vm.rng_classic_state,expected.rng_classic_state);
    return 1;
  }
  gml_part_reset_all(vm.particles);
  gml_rng_seed(&vm,0);
  gml_effect_create(vm.particles,1,4,32,24,0,0x808080);
  memset(&expected,0,sizeof(expected));
  expected.win=&classic;
  gml_rng_seed(&expected,0);
  for(int i=0;i<24;i++) (void)gml_rng_value(&expected);
  if(vm.rng_classic_state!=expected.rng_classic_state){
    fprintf(stderr,"classic smoke RNG consumption mismatch: got=%08x expected=%08x\n",
            vm.rng_classic_state,expected.rng_classic_state);
    return 1;
  }
  gml_part_reset_all(vm.particles);
  gml_rng_seed(&vm,0);
  gml_effect_create(vm.particles,1,5,32,24,0,0x808080);
  memset(&expected,0,sizeof(expected));
  expected.win=&classic;
  gml_rng_seed(&expected,0);
  for(int i=0;i<30;i++) (void)gml_rng_value(&expected);
  if(vm.rng_classic_state!=expected.rng_classic_state){
    fprintf(stderr,"classic rising smoke RNG consumption mismatch: got=%08x expected=%08x\n",
            vm.rng_classic_state,expected.rng_classic_state);
    return 1;
  }
  gml_part_reset_all(vm.particles);
  gml_rng_seed(&vm,0);
  gml_effect_create(vm.particles,1,9,32,24,2,0xFFFFFF);
  memset(&expected,0,sizeof(expected));
  expected.win=&classic;
  gml_rng_seed(&expected,0);
  (void)gml_rng_value(&expected);
  if(vm.rng_classic_state!=expected.rng_classic_state){
    fprintf(stderr,"classic cloud RNG consumption mismatch: got=%08x expected=%08x\n",
            vm.rng_classic_state,expected.rng_classic_state);
    return 1;
  }
  gml_part_reset_all(vm.particles);
  gml_rng_seed(&vm,0);
  gml_effect_create(vm.particles,1,11,32,24,2,0xFFFFFF);
  memset(&expected,0,sizeof(expected));
  expected.win=&classic;
  gml_rng_seed(&expected,0);
  for(int i=0;i<49;i++) (void)gml_rng_value(&expected);
  if(vm.rng_classic_state!=expected.rng_classic_state){
    fprintf(stderr,"classic snowfall RNG consumption mismatch: got=%08x expected=%08x\n",
            vm.rng_classic_state,expected.rng_classic_state);
    return 1;
  }
  gml_part_reset_all(vm.particles);
  {
    GmlVM other;
    memset(&other,0,sizeof(other));
    other.win=&classic;
    other.particles=gml_particle_state_create(&other);
    if(!other.particles) return 1;
    int first_system=gml_part_system_create(vm.particles);
    int first_type=gml_part_type_create(vm.particles);
    int second_system=gml_part_system_create(other.particles);
    int second_type=gml_part_type_create(other.particles);
    gml_part_particles_create(vm.particles,first_system,0,0,first_type,1);
    gml_part_particles_create(other.particles,second_system,0,0,second_type,3);
    gml_part_reset_all(vm.particles);
    if(gml_part_system_count(other.particles,second_system)!=3 ||
       gml_part_system_exists(vm.particles,first_system)){
      fprintf(stderr,"particle contexts contaminated each other\n");
      return 1;
    }
    gml_particle_state_destroy(other.particles);
  }
  gml_builtin_state_destroy(vm.builtins);
  vm.builtins=NULL;
  gml_particle_state_destroy(vm.particles);
  puts("RNG fixtures: ok");
  return 0;
}
