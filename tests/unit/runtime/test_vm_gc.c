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
  GmlInstance *keeper=gml_struct_new(&vm);
  if(!keeper){ free_chain(chain,DEPTH); return 0; }
  /* Keep a real struct reachable through the graph on both collections. */
  chain[DEPTH-1]->data[0]=vobject((uint32_t)keeper->id);

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
  free(vm.structs);free(vm.struct_gen);free(vm.struct_free);
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
  free(vm.structs);free(vm.struct_gen);free(vm.struct_free);
  free(self->data); free(self);
  free(peer->data); free(peer);
  return ok;
}

static int an_empty_pool_skips_roots_and_reused_slots_resume_collection(void){
  GmlVM vm={0};GmlArr *root=array_of(1);
  if(!root) return 0;
  GmlInstance *temporary=gml_struct_new(&vm);
  if(!temporary){free_chain(&root,1);return 0;}
  GmlVal value=vundef();value.t=V_ARR;value.arr=root;
  *gml_varmap_put(&vm.globals,"root")=value;
  gml_struct_gc(&vm);
  int ok=vm.n_structs==1 && vm.n_struct_free==1 && !vm.structs[0];
  unsigned epoch=vm.gc_epoch,array_epoch=root->gc_epoch;
  int free_slot=vm.struct_free[0];
  gml_struct_gc(&vm);
  if(vm.gc_epoch!=epoch || root->gc_epoch!=array_epoch || vm.struct_free[0]!=free_slot){
    fprintf(stderr,"an empty pool walked roots or changed slot order\n");ok=0;
  }
  GmlInstance *keeper=gml_struct_new(&vm);
  if(!keeper){ok=0;goto done;}
  root->data[0]=vobject((uint32_t)keeper->id);
  gml_struct_gc(&vm);
  if(vm.gc_epoch==epoch || root->gc_epoch!=vm.gc_epoch || vm.structs[0]!=keeper || vm.n_struct_free){
    fprintf(stderr,"reusing an empty slot failed to resume reachable collection\n");ok=0;
  }
  root->data[0]=vreal(0);
  gml_struct_gc(&vm);
  if(vm.structs[0] || vm.n_struct_free!=1){fprintf(stderr,"unrooted reused slot survived\n");ok=0;}
 done:
  gml_varmap_free_ex(&vm.globals,1);
  for(int i=0;i<vm.n_structs;i++)if(vm.structs[i]){gml_varmap_free_ex(&vm.structs[i]->vars,1);free(vm.structs[i]);}
  free(vm.structs);free(vm.struct_gen);free(vm.struct_free);free_chain(&root,1);
  return ok;
}

/* A small transient batch must not repeatedly walk a large surviving graph.
 * The small-heap node guard and independent byte guard remain active. */
static int collection_work_is_amortized_over_live_values(void){
  GmlWin content={0};GmlVM vm;
  if(gml_vm_init(&vm,&content,NULL))return 0;
  int ok=1;
#define GC_BUDGET_EXPECT(condition) do { if(!(condition)){ \
  fprintf(stderr,"live heap budget: %s\n",#condition);ok=0;goto budget_done; \
} }while(0)
  GmlVal roots=gml_arr_new(4096,vreal(0));
  GC_BUDGET_EXPECT(roots.t==V_ARR && roots.arr);
  for(int i=0;i<4096;i++){
    GmlVal child=gml_arr_new(1,vreal(i));
    GC_BUDGET_EXPECT(child.t==V_ARR && child.arr);
    gml_arr_set(roots,i,child);
  }
  GC_BUDGET_EXPECT(gml_array_heap_adopt(&vm.array_heap,roots));
  *gml_varmap_put(&vm.globals,"roots")=roots;
  gml_struct_gc(&vm);
  GC_BUDGET_EXPECT(gml_array_heap_count(vm.array_heap)==4097);
  for(int i=0;i<256;i++){
    GmlVal temporary=gml_arr_new(0,vreal(0));
    GC_BUDGET_EXPECT(temporary.t==V_ARR && temporary.arr);
    GC_BUDGET_EXPECT(gml_array_heap_adopt(&vm.array_heap,temporary));
  }
  GC_BUDGET_EXPECT(!gml_array_heap_collection_due(vm.array_heap));
  for(int i=256;i<1024;i++){
    GmlVal temporary=gml_arr_new(0,vreal(0));
    GC_BUDGET_EXPECT(temporary.t==V_ARR && temporary.arr);
    GC_BUDGET_EXPECT(gml_array_heap_adopt(&vm.array_heap,temporary));
  }
  GC_BUDGET_EXPECT(gml_array_heap_collection_due(vm.array_heap));
  gml_struct_gc(&vm);
  GC_BUDGET_EXPECT(gml_array_heap_count(vm.array_heap)==4097);
  for(int i=0;i<4096;i++)
    GC_BUDGET_EXPECT(gml_arr_get(gml_arr_get(roots,i),0).d==i);
  GC_BUDGET_EXPECT(!gml_array_heap_collection_due(vm.array_heap));
  {GmlVal large=gml_arr_new((9u*1024u*1024u)/sizeof(GmlVal)+1u,vreal(0));
    GC_BUDGET_EXPECT(large.t==V_ARR && large.arr);
    GC_BUDGET_EXPECT(gml_array_heap_adopt(&vm.array_heap,large));}
  GC_BUDGET_EXPECT(gml_array_heap_collection_due(vm.array_heap));
  gml_struct_gc(&vm);
  GC_BUDGET_EXPECT(gml_array_heap_count(vm.array_heap)==4097);
  *gml_varmap_get(&vm.globals,"roots")=vreal(0);
  gml_struct_gc(&vm);
  GC_BUDGET_EXPECT(gml_array_heap_count(vm.array_heap)==0);
  for(int i=0;i<255;i++){
    GmlVal temporary=gml_arr_new(0,vreal(0));
    GC_BUDGET_EXPECT(temporary.t==V_ARR && temporary.arr);
    GC_BUDGET_EXPECT(gml_array_heap_adopt(&vm.array_heap,temporary));
  }
  GC_BUDGET_EXPECT(!gml_array_heap_collection_due(vm.array_heap));
  {GmlVal temporary=gml_arr_new(0,vreal(0));
    GC_BUDGET_EXPECT(temporary.t==V_ARR && temporary.arr);
    GC_BUDGET_EXPECT(gml_array_heap_adopt(&vm.array_heap,temporary));}
  GC_BUDGET_EXPECT(gml_array_heap_collection_due(vm.array_heap));
  gml_struct_gc(&vm);
  GC_BUDGET_EXPECT(gml_array_heap_count(vm.array_heap)==0);
 budget_done:
  gml_vm_free(&vm);
#undef GC_BUDGET_EXPECT
  return ok;
}

int main(void){
  int ok=1;
  ok &= collection_work_is_amortized_over_live_values();
  ok &= a_shared_graph_is_walked_once_per_array();
  ok &= a_cycle_terminates();
  ok &= an_empty_pool_skips_roots_and_reused_slots_resume_collection();
  if(ok) printf("vm gc: shared graphs and cycles are walked once per array; empty pools skip roots\n");
  return ok?0:1;
}
