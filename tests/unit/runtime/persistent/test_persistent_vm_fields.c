/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "persistent_test_fixture.h"

#include "gml_builtin.h"
#include "gml_value.h"
#include "gml_value_internal.h"
#include "gml_vm_internal.h"
#include "gml_render_internal.h"

#include <stdio.h>
#include <string.h>

static const char *bbox_fixture_setting(void *userdata,const char *name){
  (void)userdata;
  return name && !strcmp(name,"GML_NO_COLGRID")?"1":NULL;
}

/* The synthetic fixture pins both language-visible readings of one inclusive collision box.
 * A later-format marker selects one-past far edges unless the legacy option is set; marker
 * absence retains inclusive values. Every edge and both access paths are checked. Collision
 * operations remain inclusive under either reported reading. */
int expect_bounding_box_far_edges_by_generation(void){
  GmlWin win={0};
  GmlVM vm={0};
  AnygmHostServices services={0};
  GmlRender render={0};
  GmlSprite sprites[2]={{0}};
  GmlObject objects[2]={{0}};
  GmlInstance instances[2]={{0}};

  render.n_spr=2;
  render.spr=sprites;
  sprites[0].w=20; sprites[0].h=30;
  sprites[0].mr=19; sprites[0].mb=29;
  sprites[0].collision_kind=1;
  sprites[1].w=20; sprites[1].h=20;
  sprites[1].mr=19; sprites[1].mb=19;
  sprites[1].collision_kind=1;

  objects[0].parent=objects[1].parent=-1;
  services.development_setting=bbox_fixture_setting;
  vm.win=&win;
  vm.host=&services;
  vm.render=&render;
  vm.objects=objects;
  vm.n_objects=2;
  vm.inst=instances;
  vm.inst_count=vm.inst_cap=2;

  instances[0].active=1;
  instances[0].id=100000;
  instances[0].obj=0;
  instances[0].x=150;
  instances[0].y=420;
  instances[0].sprite_index=instances[0].mask_index=0;
  instances[0].image_xscale=instances[0].image_yscale=1;
  instances[1].active=1;
  instances[1].id=100001;
  instances[1].obj=1;
  instances[1].x=150;
  instances[1].y=450;
  instances[1].sprite_index=instances[1].mask_index=1;
  instances[1].image_xscale=instances[1].image_yscale=1;
  vm.cur_self=&instances[0];

  const struct { const char *label; int bytecode; int classic_version;
                 int release_marker; uint64_t option_flags;
                 double right; double bottom; } readings[]={
    {"first-generation-studio",16,0,0,0,169,449},
    {"second-generation-without-later-marker",17,0,0,0,169,449},
    {"second-generation-with-later-marker",17,0,1,0,170,450},
    {"same-payload-with-the-old-collision-option",17,0,1,UINT64_C(1)<<27,169,449},
    {"classic",14,810,0,0,169,449},
  };
  for(unsigned i=0;i<sizeof readings/sizeof readings[0];i++){
    win.bytecode=readings[i].bytecode;
    win.classic_version=readings[i].classic_version;
    win.has_exclusive_bbox_marker=readings[i].release_marker;
    win.option_flags=readings[i].option_flags;
    GmlVal right=gml_vm_variable_get_h(
      &vm,IT_SELF,"bbox_right",gml_value_name_hash("bbox_right"));
    GmlVal bottom=gml_vm_variable_get_h(
      &vm,IT_SELF,"bbox_bottom",gml_value_name_hash("bbox_bottom"));
    GmlVal left=gml_vm_variable_get_h(
      &vm,IT_SELF,"bbox_left",gml_value_name_hash("bbox_left"));
    GmlVal top=gml_vm_variable_get_h(
      &vm,IT_SELF,"bbox_top",gml_value_name_hash("bbox_top"));
    int found=0;
    GmlVal referenced_bottom=gml_inst_var_get_val(
      &vm,vreal((double)instances[0].id),"bbox_bottom",&found);
    GmlVal referenced_right=gml_inst_var_get_val(
      &vm,vreal((double)instances[0].id),"bbox_right",&found);
    if(right.t!=V_REAL || right.d!=readings[i].right ||
       bottom.t!=V_REAL || bottom.d!=readings[i].bottom ||
       left.t!=V_REAL || left.d!=150 || top.t!=V_REAL || top.d!=420 ||
       !found || referenced_bottom.t!=V_REAL ||
       referenced_bottom.d!=readings[i].bottom ||
       referenced_right.t!=V_REAL || referenced_right.d!=readings[i].right){
      fprintf(stderr,
        "%s bbox fields mismatch: left=%.0f top=%.0f right=%.0f bottom=%.0f "
        "referenced right=%.0f bottom=%.0f (expected right=%.0f bottom=%.0f)\n",
        readings[i].label,
        left.t==V_REAL?left.d:-1.0,top.t==V_REAL?top.d:-1.0,
        right.t==V_REAL?right.d:-1.0,bottom.t==V_REAL?bottom.d:-1.0,
        referenced_right.t==V_REAL?referenced_right.d:-1.0,
        referenced_bottom.t==V_REAL?referenced_bottom.d:-1.0,
        readings[i].right,readings[i].bottom);
      return 0;
    }
  }

  /* The collision engine keeps the inclusive bounds whichever way they are reported. */
  win.bytecode=17;
  win.classic_version=0;
  win.has_exclusive_bbox_marker=1;
  win.option_flags=0;
  GmlVal adjacent_query[7]={
    vreal(150),vreal(420),vreal(169),vreal(449),vreal(1),vreal(0),vreal(0)
  };
  GmlVal adjacent_hit=gml_builtin_call(&vm,"collision_rectangle",adjacent_query,7);
  adjacent_query[3]=vreal(450);
  GmlVal enlarged_hit=gml_builtin_call(&vm,"collision_rectangle",adjacent_query,7);
  if(adjacent_hit.t!=V_REAL || adjacent_hit.d!=IT_NOONE ||
     enlarged_hit.t!=V_REAL || enlarged_hit.d!=(double)instances[1].id){
    fprintf(stderr,"adjacent bbox collision mismatch: exact=%.0f enlarged=%.0f\n",
      adjacent_hit.t==V_REAL?adjacent_hit.d:-99.0,
      enlarged_hit.t==V_REAL?enlarged_hit.d:-99.0);
    gml_builtin_state_destroy(vm.builtins);
    return 0;
  }
  gml_builtin_state_destroy(vm.builtins);
  return 1;
}

/* A fractional-position box must round both edges consistently. The synthetic
 * neighbour distinguishes the positions immediately around the half-pixel boundary. */
int expect_bounding_box_far_edges_at_a_fractional_position(void){
  GmlWin win={0};
  GmlVM vm={0};
  AnygmHostServices services={0};
  GmlRender render={0};
  GmlSprite sprites[2]={{0}};
  GmlObject objects[2]={{0}};
  GmlInstance instances[2]={{0}};

  render.n_spr=2;
  render.spr=sprites;
  sprites[0].w=20; sprites[0].h=30;
  sprites[0].mr=19; sprites[0].mb=29;
  /* Rectangle masks: a per-pixel mask samples whole rows and answers a different question, which
   * the case above already covers. This one is about the box. */
  sprites[0].collision_kind=0;
  sprites[1].w=20; sprites[1].h=20;
  sprites[1].mr=19; sprites[1].mb=19;
  sprites[1].collision_kind=0;
  objects[0].parent=objects[1].parent=-1;
  services.development_setting=bbox_fixture_setting;
  vm.win=&win;
  vm.host=&services;
  vm.render=&render;
  vm.objects=objects;
  vm.n_objects=2;
  vm.inst=instances;
  vm.inst_count=vm.inst_cap=2;
  /* Exercise the format configuration without the exclusive far-edge marker. */
  win.bytecode=14;
  win.classic_version=0;
  win.has_exclusive_bbox_marker=0;
  win.option_flags=0;

  instances[0].active=1;
  instances[0].id=100000;
  instances[0].obj=0;
  instances[0].x=150;
  instances[0].sprite_index=instances[0].mask_index=0;
  instances[0].image_xscale=instances[0].image_yscale=1;
  instances[1].active=1;
  instances[1].id=100001;
  instances[1].obj=1;
  instances[1].x=150;
  instances[1].y=450;
  instances[1].sprite_index=instances[1].mask_index=1;
  instances[1].image_xscale=instances[1].image_yscale=1;
  vm.cur_self=&instances[0];

  /* This is about the collision box, not the reported one: bbox_bottom has its own reading, pinned
   * by the case above. Both edges are read off the same rounded position, which is the position the
   * reported box is read off too, so the collision box changes where the instance's own row changes
   * - at the half pixel - and not a whole pixel early on one side of it. At 420 it ends on the row
   * before the neighbour's first, which the case above pins; it goes on ending there while the
   * instance rounds to 420, and reaches the neighbour once it rounds to 421. */
  const struct { const char *label; double y; int touching; } readings[]={
    {"whole",420.0,0},
    {"a tenth above",419.9,0},
    {"a tenth below",420.1,0},
    {"just under the half",420.4,0},
    {"the half itself, which rounds to even",420.5,0},
    {"past the half",420.6,1},
    {"the next whole row",421.0,1},
  };
  for(unsigned i=0;i<sizeof readings/sizeof readings[0];i++){
    instances[0].y=readings[i].y;
    gml_colgrid_invalidate(&vm);
    GmlVal meeting_args[3]={vreal(instances[0].x),vreal(instances[0].y),
                            vreal((double)instances[1].id)};
    GmlVal meeting=gml_builtin_call(&vm,"place_meeting",meeting_args,3);
    int touching=(meeting.t==V_REAL && meeting.d!=0);
    if(touching!=readings[i].touching){
      fprintf(stderr,"%s: y=%.2f touching=%d, expected %d\n",
        readings[i].label,readings[i].y,touching,readings[i].touching);
      gml_builtin_state_destroy(vm.builtins);
      return 0;
    }
  }
  gml_builtin_state_destroy(vm.builtins);
  return 1;
}
