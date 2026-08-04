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

int main(void){
  special_name_gate();
  global_lookup_semantics();
  if(failures) return 1;
  puts("vm hot-path name resolution: ok");
  return 0;
}
