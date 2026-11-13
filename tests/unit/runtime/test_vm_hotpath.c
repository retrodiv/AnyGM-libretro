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
#include "gml_builtin.h"

#include <stdio.h>
#include <math.h>
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
  "undefined","game_id","room","room_speed","fps","delta_time","view_current","view_enabled",
  "background_color","event_type","event_data","mouse_x","mouse_button","mouse_lastbutton","current_time","os_type","os_windows",
  "room_width","room_height","instance_count","health","lives","score","id","object_index",
  "sprite_width","image_number","x","y","xprevious","sprite_index","image_index","image_alpha",
  "depth","visible","solid","hspeed","vspeed","direction","speed","layer",
  "phy_position_x","gravity","path_index","path_endaction","timeline_index","timeline_loop",
  "transition_kind","transition_steps","time_source_global","time_source_state_stopped",
  "cursor_sprite","working_directory","async_load",
  "timezone_local","timezone_utc",
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

  /* Asset-layer sprites retain their authored element sequence within one layer. */
  GmlDrawItem asset_layer[3];
  memset(asset_layer,0,sizeof asset_layer);
  const int authored_shuffle[3]={2,0,1};
  for(int i=0;i<3;i++){
    asset_layer[i].depth=100;
    asset_layer[i].type=5;
    asset_layer[i].order=4;
    asset_layer[i].seq=authored_shuffle[i];
  }
  ok &= sort_with_scratch(asset_layer,3);
  if(ok && (asset_layer[0].seq!=0 || asset_layer[1].seq!=1 || asset_layer[2].seq!=2)){
    fail("studio asset-layer sprites lost authored order");
    ok=0;
  }

  free(items);
  if(ok) return 1;
  failures++;
  return 0;
}

static void argument_and_receiver_resolution(void){
  GmlWin content={.bytecode=17};GmlVM vm={0};GmlInstance instance={0};
  vm.win=&content;vm.cur_self=&instance;vm.cur_code_index=0;
  vm.script_argc=2;vm.script_args[0]=vreal(17);vm.script_args[1]=vstr("neutral");
  const char *names[]={"argument0","argument_count","argumentative","argument16"};
  *gml_varmap_put(&instance.vars,"argumentative")=vreal(31);
  *gml_varmap_put(&instance.vars,"argument16")=vreal(37);
  const double expected[]={17,2,31,37};
  for(unsigned i=0;i<4u;i++){
    GmlVal value=gml_vm_variable_get_h(&vm,IT_SELF,names[i],gml_value_name_hash(names[i]));
    if(value.t!=V_REAL || value.d!=expected[i])fail("argument or ordinary-prefix resolution changed");
  }
  GmlVal value=gml_vm_variable_get_h(&vm,IT_SELF,"argument1",gml_value_name_hash("argument1"));
  if(value.t!=V_STR || strcmp(value.s,"neutral"))fail("argument value category changed");
  value=gml_vm_variable_get_h(&vm,IT_SELF,"argument2",gml_value_name_hash("argument2"));
  if(value.t!=V_UNDEF)fail("a missing Studio argument lost undefined");
  GmlVarMap statics={0};vm.code_static=&statics;vm.code_static_count=1;
  *gml_varmap_put(&statics,"argument0")=vreal(91);
  value=gml_vm_variable_get_h(&vm,IT_STATIC,"argument0",gml_value_name_hash("argument0"));
  if(value.t!=V_REAL || value.d!=91)fail("static scope lost priority over an argument name");
  content.classic_version=800;content.compatibility=NULL;
  *gml_varmap_put(&vm.globals,"argument0")=vreal(43);
  if(!gml_vm_declare_globalvar(&vm,"argument0"))fail("could not declare the Classic argument-name global");
  value=gml_vm_variable_get_h(&vm,IT_SELF,"argument0",gml_value_name_hash("argument0"));
  if(value.t!=V_REAL || value.d!=43)fail("a declared Classic global lost priority over an argument");
  value=gml_vm_variable_get_h(&vm,IT_SELF,"argument2",gml_value_name_hash("argument2"));
  if(value.t!=V_REAL || value.d!=0)fail("a missing Classic argument lost zero");
  content.classic_version=0;content.compatibility=NULL;
  GmlInstance *record=gml_struct_new(&vm);
  if(!record)fail("could not create a field receiver");
  else{
    *gml_varmap_put(&record->vars,"argument0")=vreal(99);
    int found=0;value=gml_inst_var_get_val(&vm,vobject((uint32_t)record->id),"argument0",&found);
    if(!found || value.t!=V_REAL || value.d!=99)fail("a struct field was replaced by a script argument");
  }
  gml_varmap_free_ex(&instance.vars,1);gml_varmap_free_ex(&statics,1);gml_varmap_free_ex(&vm.globals,1);
  for(int i=0;i<vm.n_structs;i++)if(vm.structs[i]){gml_varmap_free_ex(&vm.structs[i]->vars,1);free(vm.structs[i]);}
  free(vm.structs);free(vm.struct_gen);free(vm.struct_free);free(vm.special_var_hash);
}

static void game_identity_semantics(void){
  GmlWin win={0};
  GmlVM vm={.win=&win};
  vm.inst=calloc(1,sizeof *vm.inst);
  if(!vm.inst){ fail("identity receiver allocation"); return; }
  vm.inst_count=vm.inst_cap=1;
  vm.inst[0].id=100001; vm.inst[0].active=1;
  vm.cur_self=vm.inst;
  const uint32_t identities[]={0,9,42,UINT32_MAX};
  for(size_t i=0;i<sizeof identities/sizeof identities[0];i++){
    win.gameid=identities[i];
    unsigned long unset=gml_vm_unset_reads(&vm);
    GmlVal value=gml_vm_identifier_get(&vm,"game_id");
    GmlVal global=gml_vm_variable_get_h(&vm,IT_GLOBAL,"game_id",gml_value_name_hash("game_id"));
    if(value.t!=V_REAL || value.d!=(double)identities[i] ||
       global.t!=V_REAL || global.d!=(double)identities[i] ||
       gml_vm_unset_reads(&vm)!=unset) fail("game_id must read loaded metadata without an unset fallback");
    gml_vm_assign_current_scope(&vm,"game_id",vreal(7));
    value=gml_vm_identifier_get(&vm,"game_id");
    if(value.t!=V_REAL || value.d!=(double)identities[i] ||
       gml_varmap_get(&vm.inst[0].vars,"game_id")) fail("game_id is read-only");
  }
  vm.win=NULL;
  gml_vm_free(&vm);
}

static void event_data_outside_payload_events(void){
  GmlWin win={.bytecode=17};
  GmlVM vm={.win=&win};
  vm.inst=calloc(1,sizeof *vm.inst);
  if(!vm.inst){fail("event data receiver allocation");return;}
  vm.inst_count=vm.inst_cap=1;
  vm.inst[0].id=100001;vm.inst[0].active=1;vm.cur_self=vm.inst;
  const int events[]={0,3,8};
  const int scopes[]={IT_SELF,IT_GLOBAL,IT_OTHER};
  vm.cur_other=vm.inst;
  for(unsigned e=0;e<sizeof events/sizeof events[0];e++){
    vm.event_type=events[e];
    unsigned long unset=gml_vm_unset_reads(&vm);
    GmlVal identifier=gml_vm_identifier_get(&vm,"event_data");
    if(identifier.t!=V_REAL || identifier.d!=-1)fail("event_data identifier outside payload events");
    for(unsigned i=0;i<sizeof scopes/sizeof scopes[0];i++){
      GmlVal value=gml_vm_variable_get_h(&vm,scopes[i],"event_data",gml_value_name_hash("event_data"));
      if(value.t!=V_REAL || value.d!=-1)fail("event_data scoped read outside payload events");
    }
    if(gml_vm_unset_reads(&vm)!=unset)fail("event_data must not use an unset variable fallback");
  }
  gml_vm_assign_current_scope(&vm,"event_data",vreal(7));
  GmlVal reflect[]={vstr("event_data"),vreal(99)};
  gml_builtin_call(&vm,"variable_global_set",reflect,2);
  GmlVal reflected=gml_builtin_call(&vm,"variable_global_get",reflect,1);
  if(reflected.t!=V_REAL || reflected.d!=-1 ||
     gml_builtin_call(&vm,"variable_global_exists",reflect,1).d!=1)
    fail("event_data remains read-only through global reflection");
  GmlVal assigned=gml_vm_identifier_get(&vm,"event_data");
  if(assigned.t!=V_REAL || assigned.d!=-1 || gml_varmap_get(&vm.cur_self->vars,"event_data"))
    fail("event_data must not be shadowed by an ordinary instance write");
  GmlInstance *record=gml_struct_new(&vm);
  if(!record)fail("event data struct allocation");
  else{
    *gml_varmap_put(&record->vars,"event_data")=vreal(42);
    int found=0;GmlVal field=gml_inst_var_get_val(&vm,vobject((uint32_t)record->id),"event_data",&found);
    if(!found || field.t!=V_REAL || field.d!=42)fail("event_data struct field retains ordinary ownership");
  }
  vm.win=NULL;gml_vm_free(&vm);
}

static void event_data_broadcast_payload(void){
  GmlWin win={.bytecode=17,.size=4,.owns=1,.n_code=1};
  win.data=calloc(4,1);win.code=calloc(1,sizeof *win.code);
  if(!win.data || !win.code){fail("broadcast code allocation");gml_win_free(&win);return;}
  GmlCode *code=win.code;code->name="gml_Object_payload_receiver_Other_76";
  code->n_insn=7;code->insn=calloc(7,sizeof *code->insn);
  code->insn_pc=calloc(7,sizeof *code->insn_pc);code->branch_index=calloc(7,sizeof *code->branch_index);
  if(!code->insn || !code->insn_pc || !code->branch_index){fail("broadcast instruction allocation");gml_win_free(&win);return;}
  const char *payload="event_data",*capture="captured_map",*seen="seen_payload";
  code->insn[0]=(GmlInsn){.kind=OP_PUSH,.type1=DT_VAR,.inst=IT_SELF,.reftype=0xa0,.refname=payload,.refhash=gml_value_name_hash(payload)};
  code->insn[1]=(GmlInsn){.kind=OP_POP,.type1=DT_VAR,.type2=DT_VAR,.inst=IT_GLOBAL,.reftype=0xa0,.refname=seen,.refhash=gml_value_name_hash(seen)};
  code->insn[2]=code->insn[0];
  code->insn[3]=(GmlInsn){.kind=OP_PUSH,.type1=DT_VAR,.inst=IT_GLOBAL,.reftype=0xa0,.refname=capture,.refhash=gml_value_name_hash(capture)};
  code->insn[4]=(GmlInsn){.kind=OP_CALL,.argc=2,.refname="ds_map_copy",.funcval_ci=-2};
  code->insn[5]=(GmlInsn){.kind=OP_POPZ};code->insn[6]=(GmlInsn){.kind=OP_EXIT};
  GmlVM vm={.win=&win,.next_id=100002,.n_objects=1,.inst_count=1,.inst_cap=1};
  vm.objects=calloc(1,sizeof *vm.objects);vm.inst=calloc(1,sizeof *vm.inst);
  if(!vm.objects || !vm.inst){fail("broadcast receiver allocation");vm.win=NULL;gml_vm_free(&vm);gml_win_free(&win);return;}
  vm.objects[0].name="payload_receiver";vm.objects[0].parent=-1;
  vm.inst[0].id=100001;vm.inst[0].obj=0;vm.inst[0].active=1;vm.cur_self=vm.inst;
  GmlVal saved=gml_builtin_call(&vm,"ds_map_create",NULL,0);
  *gml_varmap_put(&vm.globals,capture)=saved;
  for(int nested=0;nested<2;nested++){
    if(nested)*gml_varmap_put(&vm.globals,payload)=saved;
    gml_fire_broadcast_message(&vm,"sprite event","pulse",77);
    const char *keys[]={"event_type","message","element_id"};
    for(int i=0;i<3;i++){
      GmlVal args[]={saved,vstr(keys[i])};GmlVal value=gml_builtin_call(&vm,"ds_map_find_value",args,2);
      if(i==2 ? value.t!=V_REAL || value.d!=77 : value.t!=V_STR || strcmp(value.s,i==0?"sprite event":"pulse"))
        fail("broadcast handler must observe all payload fields");
    }
    GmlVal current=gml_vm_identifier_get(&vm,payload);
    if(current.t!=V_REAL || current.d!=(nested?saved.d:-1))fail("broadcast must restore outer payload scope");
    GmlVal expired=vreal(gml_global_num(&vm,seen));
    GmlVal args[]={expired,vstr("message")};
    if(expired.d<0 || expired.d==saved.d || gml_builtin_call(&vm,"ds_map_find_value",args,2).t!=V_UNDEF)
      fail("broadcast payload map must be destroyed after its callback");
  }
  vm.win=NULL;gml_vm_free(&vm);gml_win_free(&win);
}

static void event_message_word(uint8_t *data,size_t at,uint32_t value){
  for(unsigned i=0;i<4;i++)data[at+i]=(uint8_t)(value>>(i*8));
}
static void event_message_metadata(void){
  for(int variant=0;variant<7;variant++){
    uint8_t data[1024]={0};char *strings[]={"pulse"};uint32_t offsets[]={300};
    GmlWin win={.bytecode=17,.data=data,.size=sizeof data,.strs=strings,
      .str_charoff=offsets,.n_strs=1,.n_chunks=2};
    memcpy(win.chunks[0].name,"SPRT",5);win.chunks[0].off=16;win.chunks[0].size=384;
    memcpy(win.chunks[1].name,"SEQN",5);win.chunks[1].off=400;win.chunks[1].size=300;
    event_message_word(data,16,1);event_message_word(data,20,64);
    event_message_word(data,120,UINT32_MAX);event_message_word(data,124,2);
    event_message_word(data,140,192);event_message_word(data,192,1);
    event_message_word(data,228,1);event_message_word(data,248,1);
    event_message_word(data,256,1);event_message_word(data,260,300);
    event_message_word(data,400,1);event_message_word(data,404,1);event_message_word(data,408,480);
    event_message_word(data,480,300);event_message_word(data,484,1);
    event_message_word(data,488,UINT32_C(0x3f800000));event_message_word(data,492,1);
    event_message_word(data,496,UINT32_C(0x40800000));event_message_word(data,512,1);
    event_message_word(data,516,UINT32_C(0x3f800000));event_message_word(data,532,1);
    event_message_word(data,540,1);event_message_word(data,544,300);
    if(variant==1)event_message_word(data,244,1);
    if(variant==2)event_message_word(data,192,2);
    if(variant==3)win.chunks[0].size=244;
    if(variant==4)event_message_word(data,232,UINT32_C(0x7fc00000));
    if(variant==5)event_message_word(data,228,4097);
    if(variant==6)event_message_word(data,260,301);
    GmlVM vm={.win=&win};
    if(!gml_vm_rooms_init(&vm))fail("broadcast metadata initialization");
    int count=vm.n_sprite_messages?vm.sprite_messages[0].count:0;
    if(count!=(variant?0:1))fail("sprite broadcast metadata must retain valid and refuse malformed stores");
    if(!variant && (vm.sprite_messages[0].keys[0].frame!=0 ||
       strcmp(vm.sprite_messages[0].keys[0].message,"pulse")))fail("sprite broadcast key and string");
    if(vm.n_sequences!=1 || vm.sequences[0].messages.count!=1 ||
       vm.sequences[0].messages.keys[0].frame!=1 ||
       strcmp(vm.sequences[0].messages.keys[0].message,"pulse"))fail("sequence broadcast uses the same bounded store");
    vm.win=NULL;gml_vm_free(&vm);
  }
}
static void event_message_crossings(void){
  GmlBroadcastKey keys[]={{3,"c"},{0,"a"},{1,"b"}};
  GmlBroadcastTrack track={.keys=keys,.count=3};
  const struct { double before,after;int playback;const char *order; } cases[]={
    {0,1,1,"a"},{1,2,1,"b"},{3,5,1,"ca"},{1,-1,1,"ba"},
    {0,9,1,"abcabca"},{0,8,2,"abccb"},{0,10,0,"abc"},{2,2,1,""}
  };
  for(unsigned test=0;test<sizeof cases/sizeof cases[0];test++){
    GmlVM vm={0};
    if(!gml_vm_broadcast_queue(&vm,&track,1,77,cases[test].before,cases[test].after,4,cases[test].playback) ||
       vm.n_broadcast_pending!=(int)strlen(cases[test].order))fail("broadcast crossing count");
    else for(int i=0;i<vm.n_broadcast_pending;i++)
      if(vm.broadcast_pending[i].message[0]!=cases[test].order[i] ||
         vm.broadcast_pending[i].element!=77 || vm.broadcast_pending[i].kind!=1)
        fail("broadcast crossing order and source identity");
    free(vm.broadcast_pending);
  }
  GmlBroadcastKey duplicates[]={{1,"b"},{1,"d"}};
  GmlBroadcastTrack repeated={.keys=duplicates,.count=2};
  for(int backward=0;backward<2;backward++){
    GmlVM test={0};
    if(!gml_vm_broadcast_queue(&test,&repeated,1,77,1,backward?0:2,4,1) ||
       test.n_broadcast_pending!=2 ||
       test.broadcast_pending[0].message[0]!=(backward?'d':'b') ||
       test.broadcast_pending[1].message[0]!=(backward?'b':'d'))
      fail("broadcast duplicate keys retain direction-dependent order");
    free(test.broadcast_pending);
  }
  GmlVM vm={0};
  if(!gml_vm_broadcast_queue(&vm,&track,1,77,0,1,4,1) || vm.n_broadcast_pending!=1 ||
     gml_vm_broadcast_queue(&vm,&track,1,77,0,1e20,4,1) || vm.n_broadcast_pending!=1 ||
     gml_vm_broadcast_queue(&vm,&track,1,77,NAN,1,4,1) || vm.n_broadcast_pending!=1)
    fail("broadcast refuses nonfinite/unbounded work transactionally");
  free(vm.broadcast_pending);
}

int main(void){
  special_name_gate();
  game_identity_semantics();
  event_data_outside_payload_events();
  event_data_broadcast_payload();
  event_message_metadata();
  event_message_crossings();
  global_lookup_semantics();
  argument_and_receiver_resolution();
  draw_item_sort_order();
  if(failures) return 1;
  puts("vm hot paths: ok");
  return 0;
}
