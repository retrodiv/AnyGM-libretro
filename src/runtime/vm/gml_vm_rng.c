/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* gml_vm_rng.c — the one compatibility-aware VM random stream. */
#include "gml_vm.h"
#include "anygm_compatibility.h"
#include "anygm_host.h"

/* WELL512 follows the Lomont recurrence credited in LICENSES/WELL512.txt.
 * Expand seeds using MSVC-LCG arithmetic; random(x) scales the next value by x/2^32. */
void gml_rng_seed(GmlVM *vm, uint32_t seed){
  uint32_t s=seed;
  for(int i=0;i<16;i++){ s=((s*214013u+2531011u)>>16)&0x7fffffffu; vm->rng_well[i]=s; }
  vm->rng_index=0;
  vm->rng_classic_state=seed;
}
static uint32_t gml_rng_next(GmlVM *vm){
  if(vm->diagnostics.rng_call_logging<0)
    vm->diagnostics.rng_call_logging=anygm_host_development_setting(vm->host,"GML_LOG_RNG_CALL")?1:0;
  if(vm->win && anygm_policy_uses_classic_runtime(vm->win)){
    vm->rng_classic_state=vm->rng_classic_state*0x08088405u+1u;
    if(vm->diagnostics.rng_call_logging){
      const char *object=(vm->cur_self && vm->cur_self->obj>=0 && vm->cur_self->obj<vm->n_objects)
        ? vm->objects[vm->cur_self->obj].name : "?";
      anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[rng-call] f%ld seed=%u id=%u object=%s event=%s\n",vm->frame,
        vm->rng_classic_state,vm->cur_self?vm->cur_self->id:0,object?object:"?",
        vm->cur_event?vm->cur_event:"?");
    }
    return vm->rng_classic_state;
  }
  uint32_t *st=vm->rng_well; uint32_t idx=vm->rng_index;
  uint32_t a,b,c,d;
  a=st[idx];
  c=st[(idx+13)&15];
  b=a ^ c ^ (a<<16) ^ (c<<15);
  c=st[(idx+9)&15];
  c^=(c>>11);
  st[idx]=a=b^c;
  d=a ^ ((a<<5)&0xDA442D24u);
  idx=(idx+15)&15;
  a=st[idx];
  st[idx]=a=a ^ b ^ d ^ (a<<2) ^ (b<<18) ^ (c<<28);
  vm->rng_index=idx;
  if(vm->diagnostics.rng_call_logging){
    const char *object=(vm->cur_self && vm->cur_self->obj>=0 && vm->cur_self->obj<vm->n_objects)
      ? vm->objects[vm->cur_self->obj].name : "?";
    anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[rng-call] f%ld value=%u id=%u object=%s event=%s\n",vm->frame,a,
      vm->cur_self?vm->cur_self->id:0,object?object:"?",vm->cur_event?vm->cur_event:"?");
  }
  return a;
}
double gml_rng_value(GmlVM *vm){ return (double)gml_rng_next(vm) / 4294967296.0; }  /* [0,1) */
