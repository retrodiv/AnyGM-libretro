/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Guards for the two name-resolution paths the interpreter runs most often.
 *
 * Both were accelerated after profiling showed variable-name resolution dominating a frame, and
 * both accelerate by changing *how* a name is found, never *what* it finds.  These cases pin the
 * observable result so a future acceleration cannot silently start missing entries: a gate that
 * stopped recognizing a builtin name would reroute reads to ordinary variables, and a global
 * lookup that stopped finding a key would report zero for live view and camera state. */
#include "gml_value_internal.h"
#include "gml_vm.h"
#include "gml_vm_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static void fail(const char *label){
  fprintf(stderr,"vm hot path: %s\n",label);
  failures++;
}

/* A spread of builtin names the gate must always claim.  These are engine-level GameMaker names,
 * not content, and the sample deliberately spans the whole table so a broken probe sequence cannot
 * pass by covering only its first entries. */
static const char *const recognized_names[]={
  "undefined","room","room_speed","fps","delta_time","view_current","view_enabled",
  "background_color","event_type","mouse_x","current_time","os_type","os_windows",
  "room_width","room_height","instance_count","health","lives","score","id","object_index",
  "sprite_width","image_number","x","y","xprevious","sprite_index","image_index","image_alpha",
  "depth","visible","solid","hspeed","vspeed","direction","speed","layer",
  "phy_position_x","gravity","path_index","path_endaction","timeline_index","timeline_loop",
  "transition_kind","transition_steps","time_source_global","time_source_state_stopped",
  "cursor_sprite","working_directory","async_load",
};

/* Prefix-matched families the gate also claims. */
static const char *const recognized_prefixes[]={
  "argument0","argument7","argument_count","bbox_left","bbox_bottom",
};

/* Names no builtin owns: the gate must let these reach ordinary variable storage. */
static const char *const ordinary_names[]={
  "counter","player_health","zz_not_a_builtin","view","speed_boost","xx","image",
  "argumentative","bbox","roomy","idx","layered",
};

static int special_name_gate(void){
  GmlVM vm;
  memset(&vm,0,sizeof vm);
  int ok=1;

  for(size_t i=0;i<sizeof recognized_names/sizeof *recognized_names;i++){
    const char *name=recognized_names[i];
    if(!gml_vm_variable_name_maybe_special(&vm,name,gml_value_name_hash(name))){
      fprintf(stderr,"vm hot path: builtin name '%s' was not recognized\n",name);
      ok=0;
    }
  }
  for(size_t i=0;i<sizeof recognized_prefixes/sizeof *recognized_prefixes;i++){
    const char *name=recognized_prefixes[i];
    if(!gml_vm_variable_name_maybe_special(&vm,name,gml_value_name_hash(name))){
      fprintf(stderr,"vm hot path: prefixed builtin '%s' was not recognized\n",name);
      ok=0;
    }
  }
  /* The gate is allowed to over-claim (the chains behind it confirm with their own comparisons),
   * so an ordinary name is only reported when every sampled one is claimed: that pattern means the
   * gate degenerated into "always special" and stopped gating at all. */
  int claimed=0;
  const size_t ordinary_count=sizeof ordinary_names/sizeof *ordinary_names;
  for(size_t i=0;i<ordinary_count;i++){
    const char *name=ordinary_names[i];
    if(gml_vm_variable_name_maybe_special(&vm,name,gml_value_name_hash(name))) claimed++;
  }
  if((size_t)claimed==ordinary_count){
    fail("the builtin-name gate claimed every ordinary name");
    ok=0;
  }

  free(vm.special_var_hash);
  if(!ok) failures++;
  return ok;
}

static int global_lookup_semantics(void){
  GmlVM vm;
  memset(&vm,0,sizeof vm);
  int ok=1;

  GmlVal *scalar=gml_varmap_put_hashed(&vm.globals,"fixture_scalar",
                                       gml_value_name_hash("fixture_scalar"));
  if(scalar) *scalar=vreal(42);
  GmlVal *array=gml_varmap_put_hashed(&vm.globals,"fixture_array",
                                      gml_value_name_hash("fixture_array"));
  if(array){
    *array=gml_arr_new(4,vreal(0));
    gml_arr_set(*array,0,vreal(10));
    gml_arr_set(*array,2,vreal(320));
  }
  /* A key that exists but holds a scalar must not answer an array read. */
  GmlVal *decoy=gml_varmap_put_hashed(&vm.globals,"fixture_decoy",
                                      gml_value_name_hash("fixture_decoy"));
  if(decoy) *decoy=vreal(7);
  if(!scalar || !array || !decoy){ fail("global fixture allocation failed"); return 0; }

  struct { const char *label; double actual; double expected; } checks[]={
    {"array element 0",       gml_global_arr(&vm,"fixture_array",0),   10},
    {"array element 2",       gml_global_arr(&vm,"fixture_array",2),   320},
    {"array element unset",   gml_global_arr(&vm,"fixture_array",1),   0},
    {"array index past end",  gml_global_arr(&vm,"fixture_array",4),   0},
    {"array negative index",  gml_global_arr(&vm,"fixture_array",-1),  0},
    {"array read of scalar",  gml_global_arr(&vm,"fixture_decoy",0),   0},
    {"array read of missing", gml_global_arr(&vm,"fixture_absent",0),  0},
    {"scalar value",          gml_global_num(&vm,"fixture_scalar"),    42},
    {"scalar read of missing",gml_global_num(&vm,"fixture_absent"),    0},
  };
  for(size_t i=0;i<sizeof checks/sizeof *checks;i++){
    if(checks[i].actual!=checks[i].expected){
      fprintf(stderr,"vm hot path: %s expected %.17g, got %.17g\n",
              checks[i].label,checks[i].expected,checks[i].actual);
      ok=0;
    }
  }

  /* Enough distinct globals to force the map past a grow-and-rehash, which is where a hashed
   * lookup that mishandles probe sequences starts losing keys a full scan would still find. */
  char name[32];
  for(int i=0;i<256;i++){
    snprintf(name,sizeof name,"fixture_bulk_%d",i);
    /* The map borrows literal keys; these are generated, so hand ownership to the map. */
    char *owned=malloc(strlen(name)+1);
    if(!owned){ fail("bulk key allocation failed"); ok=0; break; }
    memcpy(owned,name,strlen(name)+1);
    GmlVal *slot=gml_varmap_put_owned_hashed(&vm.globals,owned,gml_value_name_hash(owned));
    if(slot) *slot=vreal(i);
  }
  for(int i=0;i<256;i++){
    snprintf(name,sizeof name,"fixture_bulk_%d",i);
    if(gml_global_num(&vm,name)!=(double)i){
      fprintf(stderr,"vm hot path: global '%s' was not found after rehash\n",name);
      ok=0;
      break;
    }
  }
  if(gml_global_arr(&vm,"fixture_array",2)!=320){
    fail("array global was lost after the map grew");
    ok=0;
  }

  gml_varmap_free(&vm.globals);
  free(vm.special_var_hash);
  if(!ok) failures++;
  return ok;
}

/* The draw-list sort was accelerated the same way: gml_vm_draw_items_sort
 * changes *how* the list is ordered — natural-run merge instead of qsort —
 * never *what* order results.
 *
 * The comparator is a total order only on part of its reachable domain: at
 * equal depth, records that fall through to the final descending-seq rule can
 * form comparison cycles against families ordered by an earlier rule (layer
 * orders, ascending-seq tile and background rules). Where a cycle is
 * possible, qsort's order was never portable to begin with — each C runtime
 * called the comparator in a different sequence. So the guard pins two
 * different things:
 *
 *   - on provably transitive domains (unique depths; each tie-break family
 *     among its own kind), the sorted order must equal qsort's exactly;
 *   - on the full tie-heavy mixed domain, the output must be a permutation
 *     of the input whose every adjacent pair satisfies the comparator — the
 *     merge establishes that invariant even where the comparator is cyclic,
 *     which is more than qsort ever promised there.
 *
 * Generated lists stay within the assembler's reachable domain: seq is the
 * array index, tiles/particles/classic backgrounds carry no layer order, and
 * classic is uniform per list because a frame shares one runtime policy. */
static unsigned sort_rng_state;
static unsigned sort_rng(void){
  sort_rng_state=sort_rng_state*1664525u+1013904223u;
  return sort_rng_state>>8;
}
static void fill_draw_item(GmlDrawItem *d, int seq, int classic){
  static const double depths[]={-100.0,0.0,0.0,0.0,32.0,32.0,1e6};
  static const int types[]={0,1,2,3,4,5,7};
  int type=types[sort_rng()%(sizeof types/sizeof *types)];
  d->depth=depths[sort_rng()%(sizeof depths/sizeof *depths)];
  d->seq=seq;
  d->type=type;
  d->idx=(int)(sort_rng()%64);
  d->order= (type==0||type==2||type==3||type==5||type==7) ? (int)(sort_rng()%5)-1 : -1;
  d->element_order= type==0 ? (int)(sort_rng()%5)-1 : -1;
  d->classic= type==0 ? classic : 0;
  d->obj=(int)(sort_rng()%4);
  d->placed= (type==0 && classic) ? (int)(sort_rng()%2) : 0;
}
static int sort_with_scratch(GmlDrawItem *items, int n){
  GmlDrawItem *aux=malloc(n?(size_t)n*sizeof *aux:1);
  int *runs=malloc(((size_t)n+1)*sizeof *runs);
  if(!aux||!runs){ free(aux); free(runs); fail("sort guard allocation"); return 0; }
  gml_vm_draw_items_sort(items,aux,runs,n);
  free(aux); free(runs);
  return 1;
}
static int check_equals_qsort(GmlDrawItem *items, int n, const char *label){
  size_t un=n>0?(size_t)n:0;
  GmlDrawItem *expect=malloc(un>0?un*sizeof *expect:1);
  if(!expect){ fail("sort guard allocation"); return 0; }
  if(un>0) memcpy(expect,items,un*sizeof *expect);
  qsort(expect,un,sizeof *expect,gml_vm_draw_item_cmp);
  int ok=sort_with_scratch(items,n);
  if(ok && un>0 && memcmp(items,expect,un*sizeof *items)){
    fprintf(stderr,"vm hot path: draw sort diverged from qsort (%s, n=%d)\n",label,n);
    failures++;
    ok=0;
  }
  free(expect);
  return ok;
}
static int check_sorted_permutation(GmlDrawItem *items, int n, const char *label){
  size_t un=n>0?(size_t)n:0;
  GmlDrawItem *input=malloc(un>0?un*sizeof *input:1);
  if(!input){ fail("sort guard allocation"); return 0; }
  if(un>0) memcpy(input,items,un*sizeof *input);       /* input[i].seq==i */
  int ok=sort_with_scratch(items,n);
  for(int i=0;ok && i<n;i++){
    int seq=items[i].seq;
    if(seq<0 || seq>=n || memcmp(&items[i],&input[seq],sizeof *items)){
      fprintf(stderr,"vm hot path: draw sort lost an item (%s, n=%d)\n",label,n);
      failures++; ok=0;
    }
    input[seq].seq=-1;                                  /* each item exactly once */
  }
  for(int i=1;ok && i<n;i++){
    if(gml_vm_draw_item_cmp(&items[i-1],&items[i])>0){
      fprintf(stderr,"vm hot path: draw sort left an unsorted pair (%s, n=%d, at %d)\n",label,n,i);
      failures++; ok=0;
    }
  }
  free(input);
  return ok;
}
static int draw_item_sort_order(void){
  static const int sizes[]={0,1,2,3,5,16,63,257,1000,4999};
  enum { GUARD_MAX=4999 };
  GmlDrawItem *items=malloc(GUARD_MAX*sizeof *items);
  if(!items){ fail("sort guard allocation"); return 0; }
  int ok=1;
  sort_rng_state=0x013527c6u;

  /* full tie-heavy mixed domain: permutation + adjacent order */
  for(size_t s=0;s<sizeof sizes/sizeof *sizes;s++){
    int n=sizes[s];
    for(int classic=0;classic<=2;classic++){
      for(int i=0;i<n;i++) fill_draw_item(&items[i],i,classic);
      ok &= check_sorted_permutation(items,n,"mixed ties");
    }
  }

  /* unique depths: the first comparator rule decides every pair */
  for(size_t s=0;s<sizeof sizes/sizeof *sizes;s++){
    int n=sizes[s];
    for(int i=0;i<n;i++){
      fill_draw_item(&items[i],i,(int)(sort_rng()%3));
      items[i].classic= items[i].type==0 ? items[i].classic : 0;
      items[i].depth=(double)((unsigned)i*2654435761u);   /* odd multiplier: injective */
    }
    ok &= check_equals_qsort(items,n,"unique depths");
  }

  /* each tie-break family among its own kind, all at one depth */
  int n=1000;
  for(int i=0;i<n;i++){ fill_draw_item(&items[i],i,0); items[i].type=0; items[i].classic=0; items[i].depth=7.0;
    items[i].element_order=(int)(sort_rng()%5); }        /* all listed: eo rule stays transitive */
  ok &= check_equals_qsort(items,n,"studio layer instances");
  for(int i=0;i<n;i++){ fill_draw_item(&items[i],i,0); items[i].type=0; items[i].classic=0; items[i].depth=7.0;
    items[i].element_order=-1; }
  ok &= check_equals_qsort(items,n,"studio dynamic instances");
  for(int i=0;i<n;i++){ fill_draw_item(&items[i],i,1); items[i].type=0; items[i].classic=1; items[i].order=-1; items[i].depth=7.0; }
  ok &= check_equals_qsort(items,n,"classic executable instances");
  for(int i=0;i<n;i++){ fill_draw_item(&items[i],i,2); items[i].type=0; items[i].classic=2; items[i].order=-1; items[i].depth=7.0; }
  ok &= check_equals_qsort(items,n,"classic project instances");
  for(int i=0;i<n;i++){ fill_draw_item(&items[i],i,0); items[i].type=1; items[i].order=-1; items[i].depth=3.0; }
  ok &= check_equals_qsort(items,n,"room tiles");
  for(int i=0;i<n;i++){ fill_draw_item(&items[i],i,0); items[i].type=6; items[i].order=-1; items[i].depth=1e9; }
  ok &= check_equals_qsort(items,n,"classic background slots");

  free(items);
  if(ok) return 1;
  failures++;
  return 0;
}

int main(void){
  special_name_gate();
  global_lookup_semantics();
  draw_item_sort_order();
  if(failures) return 1;
  puts("vm hot paths: ok");
  return 0;
}
