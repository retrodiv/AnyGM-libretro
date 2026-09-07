/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "persistent_test_fixture.h"

#include "gml_builtin.h"
#include "gml_value.h"
#include "gml_value_internal.h"
#include "gml_vm_internal.h"
#include "gml_render_internal.h"
#include "gmlc_package.h"
#include "gmlc_project.h"
#include "stdio_vfs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *bbox_fixture_setting(void *userdata,const char *name){
  (void)userdata;
  return name && !strcmp(name,"GML_NO_COLGRID")?"1":NULL;
}

/* A shared integer edge is sufficient for two rectangular masks. Precise masks still sample
 * their authored planes; a shared plane must not become rectangular just because its count is one.
 * Exercise the query and Collision event paths with the same fractional, reflected geometry. */
int expect_rectangular_mask_edge_contacts(void){
  char source_path[]="/tmp/gml-rectangle-contact-source-XXXXXX";
  char package_path[]="/tmp/gml-rectangle-contact-package-XXXXXX";
  int source_fd=mkstemp(source_path),package_fd=mkstemp(package_path),ok=0;
  if(source_fd<0 || package_fd<0) goto cleanup;
  close(source_fd); source_fd=-1;
  close(package_fd); package_fd=-1;
  if(!fixture_write_text(source_path,"global.contacts += 1;\n")) goto cleanup;
  AnygmHostServices services={0};
  services.struct_size=sizeof services;
  services.abi_version=ANYGM_HOST_SERVICES_VERSION;
  anygm_stdio_vfs_services_init(&services);
  services.development_setting=bbox_fixture_setting;
  GmlcProject project={0};
  GmlcObject objects[2]={{0}};
  GmlcObjectEvent event={0};
  GmlcRoom room={0};
  GmlcRoomInstance placed[2]={{0}};
  int room_order=0;
  project.host=&services;
  project.name="rectangle-contact-fixture";
  project.objects=objects; project.n_objects=project.cap_objects=2;
  project.rooms=&room; project.n_rooms=project.cap_rooms=1;
  project.room_order=&room_order; project.n_room_order=1;
  objects[0].id=objects[0].name="obj_probe";
  objects[1].id=objects[1].name="obj_target";
  for(int i=0;i<2;i++){
    objects[i].sprite_id=objects[i].mask_id=objects[i].parent_id=-1;
    placed[i].id=placed[i].name=i?"placed_target":"placed_probe";
    placed[i].object_id=i; placed[i].instance_id=100000u+(unsigned)i;
  }
  objects[0].events=&event; objects[0].n_events=objects[0].cap_events=1;
  event.event_type=4; event.collision_object_id=1; event.source_path=source_path;
  room.id=room.name="room_contact";
  room.width=room.height=64; room.speed=60;
  room.instances=placed; room.n_instances=room.cap_instances=2;
  char error[256]={0};
  if(!gmlc_package_write_structural(&project,package_path,error,sizeof error)){
    fprintf(stderr,"rectangle contact package failed: %s\n",error); goto cleanup;
  }
  GmlWin win;
  if(anygm_stdio_load_win(&win,package_path)) goto cleanup;
  GmlVM vm;
  if(gml_vm_init(&vm,&win,&services)){ gml_win_free(&win); goto cleanup; }
  win.bytecode=15;
  GmlSprite sprites[2]={{0}};
  GmlRender render={0};
  uint8_t full[4]={0xf0,0xf0,0xf0,0xf0},empty[4]={0};
  render.win=&win; render.spr=sprites; render.n_spr=2;
  for(int i=0;i<2;i++){
    sprites[i].w=sprites[i].h=4;
    sprites[i].mr=sprites[i].mb=3;
    sprites[i].n_frames=sprites[i].mask_count=sprites[i].mask_rowb=1;
    sprites[i].mask=full;
  }
  vm.render=&render;
  gml_room_enter(&vm,0);
  GmlInstance *a=find_slot(&vm,100000),*b=find_slot(&vm,100001);
  const struct { const char *label; double ax,ay,asx,bx,by,bsx;
                 int akind,bkind,empty_target,hit; } cases[]={
    {"fractional shared row",0,0.5,1,0,3.1,1,1,1,0,1},
    {"reflected shared row",4,0.5,-1,4,3.1,-1,1,1,0,1},
    {"fractional shared column",0.5,0,1,3.1,0,1,1,1,0,1},
    {"negative shared row",-4,-3.5,1,-4,-0.9,1,1,1,0,1},
    {"separated row",0,0.5,1,0,4.1,1,1,1,0,0},
    {"precise target hole",0,0,1,0,3,1,1,0,1,0},
    {"precise target contact",0,0,1,0,3,1,1,0,0,1},
    {"precise source and target hole",0,0,1,0,3,1,0,0,1,0},
    {"shared precise planes",0,0,1,0,3,1,0,0,0,1},
  };
  ok=a && b;
  for(unsigned i=0;ok && i<sizeof cases/sizeof cases[0];i++){
    a->sprite_index=-1; a->mask_index=0;
    b->sprite_index=-1; b->mask_index=1;
    a->x=cases[i].ax; a->y=cases[i].ay; a->image_xscale=cases[i].asx;
    b->x=cases[i].bx; b->y=cases[i].by; b->image_xscale=cases[i].bsx;
    a->image_yscale=b->image_yscale=1;
    sprites[0].collision_kind=cases[i].akind;
    sprites[1].collision_kind=cases[i].bkind;
    sprites[1].mask=cases[i].empty_target?empty:full;
    gml_colgrid_invalidate(&vm);
    vm.cur_self=a;
    GmlVal query[3]={vreal(a->x),vreal(a->y),vreal((double)b->id)};
    GmlVal meeting=gml_builtin_call(&vm,"place_meeting",query,3);
    GmlVal found=gml_builtin_call(&vm,"instance_place",query,3);
    *gml_varmap_put(&vm.globals,"contacts")=vreal(0);
    gml_vm_instances_run_collisions(&vm);
    GmlVal *contacts=gml_varmap_get(&vm.globals,"contacts");
    if(meeting.t!=V_REAL || meeting.d!=cases[i].hit || found.t!=V_REAL ||
       found.d!=(cases[i].hit?(double)b->id:IT_NOONE) ||
       !contacts || contacts->t!=V_REAL || contacts->d!=cases[i].hit){
      fprintf(stderr,"%s: meeting=%.0f instance=%.0f events=%.0f expected hit=%d\n",
        cases[i].label,meeting.t==V_REAL?meeting.d:-1.0,found.t==V_REAL?found.d:-1.0,
        contacts && contacts->t==V_REAL?contacts->d:-1.0,cases[i].hit);
      ok=0;
    }
  }
  vm.render=NULL;
  gml_vm_free(&vm); gml_win_free(&win);
cleanup:
  if(source_fd>=0) close(source_fd);
  if(package_fd>=0) close(package_fd);
  unlink(source_path); unlink(package_path);
  return ok;
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
