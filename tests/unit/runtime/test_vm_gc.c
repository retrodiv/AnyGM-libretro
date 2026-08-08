/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Assert that the collector visits each array once per pass, not once per path. A shared chain
 * has fan-out^depth paths but only depth distinct nodes; a depth cap protects the stack without
 * bounding the cost of repeated visits. */
#include "gml_vm.h"
#include "gml_value_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 8^12 is about 69 billion paths across 12 arrays. Without the stamp this does not finish; with it
 * the collector touches twelve nodes. Kept modest so the failure mode is a hang, not an OOM. */
#define FANOUT 8
#define DEPTH  12

static GmlArr *array_of(int slots){
  GmlArr *a=calloc(1,sizeof *a);
  if(!a) return NULL;
  a->data=calloc((size_t)slots,sizeof *a->data);
  if(!a->data){ free(a); return NULL; }
  a->len=slots; a->cap=slots;
  a->escaped=1;                     /* held from a global below, so scope cleanup must leave it */
  return a;
}

static void free_chain(GmlArr **chain,int levels){
  for(int i=0;i<levels;i++) if(chain[i]){ free(chain[i]->data); free(chain[i]); }
}

static int a_shared_graph_is_walked_once_per_array(void){
  GmlVM vm;
  memset(&vm,0,sizeof vm);

  GmlArr *chain[DEPTH];
  memset(chain,0,sizeof chain);
  for(int level=0;level<DEPTH;level++){
    chain[level]=array_of(FANOUT);
    if(!chain[level]){ fprintf(stderr,"out of memory building the fixture\n"); free_chain(chain,DEPTH); return 0; }
  }
  /* Every slot of a level points at the whole of the next one. Twelve arrays, sixty-nine billion
   * ways to arrive at the last. */
  for(int level=0;level+1<DEPTH;level++)
    for(int slot=0;slot<FANOUT;slot++){
      chain[level]->data[slot].t=V_ARR;
      chain[level]->data[slot].arr=chain[level+1];
    }

  { GmlVal *slot=gml_varmap_put(&vm.globals,"root");
    slot->t=V_ARR; slot->arr=chain[0]; }

  /* n_structs>0 is what asks for a collection at all; the sweep then finds nothing to free. */
  vm.structs=calloc(1,sizeof *vm.structs);
  if(!vm.structs){ fprintf(stderr,"out of memory\n"); free_chain(chain,DEPTH); return 0; }
  vm.n_structs=0;
  vm.cap_structs=1;

  GmlInstance *keeper=calloc(1,sizeof *keeper);
  if(!keeper){ free(vm.structs); free_chain(chain,DEPTH); return 0; }
  vm.structs[0]=keeper;
  vm.n_structs=1;

  gml_struct_gc(&vm);

  int ok=1;
  for(int level=0;level<DEPTH;level++)
    if(chain[level]->gc_epoch!=vm.gc_epoch){
      fprintf(stderr,"level %d was not reached: epoch %u, pass %u\n",
              level,chain[level]->gc_epoch,vm.gc_epoch);
      ok=0;
    }

  /* A second pass must not mistake last pass's stamps for its own, or it marks nothing and the
   * sweep frees live structs. */
  unsigned first=vm.gc_epoch;
  gml_struct_gc(&vm);
  if(vm.gc_epoch==first){ fprintf(stderr,"a second collection reused epoch %u\n",first); ok=0; }
  for(int level=0;level<DEPTH;level++)
    if(chain[level]->gc_epoch!=vm.gc_epoch){
      fprintf(stderr,"level %d was skipped on the second pass\n",level); ok=0;
    }

  gml_varmap_free_ex(&vm.globals,1);  /* the fixture owns the arrays; escaped ones are freed below */
  for(int i=0;i<vm.n_structs;i++) if(vm.structs[i]) free(vm.structs[i]);
  free(vm.structs);
  free_chain(chain,DEPTH);
  return ok;
}

static int a_cycle_terminates(void){
  /* An array holding itself has no depth at all and infinitely many paths. Before the stamp only
   * the depth cap stopped this, which meant it stopped by giving up rather than by finishing. */
  GmlVM vm;
  memset(&vm,0,sizeof vm);

  GmlArr *self=array_of(2);
  GmlArr *peer=array_of(2);
  if(!self||!peer){ free(self); free(peer); return 0; }
  self->data[0].t=V_ARR; self->data[0].arr=self;
  self->data[1].t=V_ARR; self->data[1].arr=peer;
  peer->data[0].t=V_ARR; peer->data[0].arr=self;

  { GmlVal *slot=gml_varmap_put(&vm.globals,"root");
    slot->t=V_ARR; slot->arr=self; }
  vm.structs=calloc(1,sizeof *vm.structs);
  if(!vm.structs){ free(self->data); free(self); free(peer->data); free(peer); return 0; }
  vm.structs[0]=calloc(1,sizeof **vm.structs);
  vm.n_structs=1; vm.cap_structs=1;

  gml_struct_gc(&vm);

  int ok = self->gc_epoch==vm.gc_epoch && peer->gc_epoch==vm.gc_epoch;
  if(!ok) fprintf(stderr,"a cycle left an array unreached\n");

  gml_varmap_free_ex(&vm.globals,1);  /* the fixture owns the arrays; escaped ones are freed below */
  if(vm.structs[0]) free(vm.structs[0]);
  free(vm.structs);
  free(self->data); free(self);
  free(peer->data); free(peer);
  return ok;
}

int main(void){
  int ok=1;
  ok &= a_shared_graph_is_walked_once_per_array();
  ok &= a_cycle_terminates();
  if(ok) printf("vm gc: shared graphs and cycles are walked once per array\n");
  return ok?0:1;
}
