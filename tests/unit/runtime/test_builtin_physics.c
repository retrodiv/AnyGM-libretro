/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Authored bodies exercise language units, fixture ownership and restored constraints. */
#include "gml_builtin_internal.h"
#include "gml_vm_internal.h"
#include "gml_particle.h"
#include "gml_physics_solver.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static GmlVal call(GmlVM *vm,const char *name,const double *numbers,int count){
  GmlVal arguments[12];
  assert(count<=12);
  for(int i=0;i<count;i++) arguments[i]=vreal(numbers[i]);
  return gml_builtin_call(vm,name,arguments,count);
}
#define CALL(vm,name,...) call(vm,name,(double[]){__VA_ARGS__},(int)(sizeof((double[]){__VA_ARGS__})/sizeof(double)))
static double get(GmlVM *vm,int slot,const char *name){
  GmlVal v=vundef();
  assert(gml_physics_variable_get(vm,&vm->inst[slot],name,&v));
  assert(v.t==V_REAL);
  return v.d;
}
static void setup(GmlVM *vm,GmlWin *win){
  memset(vm,0,sizeof(*vm)); memset(win,0,sizeof(*win));
  vm->win=win; vm->room_index=-1; vm->pending_room=-1; vm->next_creation_seq=4;
  vm->next_id=100004; vm->n_objects=1;
  vm->objects=calloc(1,sizeof(*vm->objects));
  vm->objects[0].parent=-1;
  vm->particles=gml_particle_state_create(vm);
  assert(vm->particles);
  gml_vm_software3d_reset(vm);
  vm->inst_count=vm->inst_cap=3;
  vm->inst=calloc(3,sizeof(*vm->inst));
  for(int i=0;i<3;i++){
    vm->inst[i].id=100001+(uint32_t)i; vm->inst[i].active=1;
    vm->inst[i].image_xscale=vm->inst[i].image_yscale=1;
    vm->inst[i].creation_seq=(uint64_t)i+1;
  }
  CALL(vm,"physics_world_create",0.1);
}
static double bind_box(GmlVM *vm,int slot,double x,double y,double w,double h,double density){
  vm->inst[slot].x=x; vm->inst[slot].y=y;
  double f=call(vm,"physics_fixture_create",NULL,0).d;
  CALL(vm,"physics_fixture_set_box_shape",f,w,h);
  CALL(vm,"physics_fixture_set_density",f,density);
  CALL(vm,"physics_fixture_set_friction",f,0.7);
  CALL(vm,"physics_fixture_set_linear_damping",f,0);
  CALL(vm,"physics_fixture_set_angular_damping",f,0);
  CALL(vm,"physics_fixture_set_collision_group",f,1);
  double bound=CALL(vm,"physics_fixture_bind",f,vm->inst[slot].id).d;
  assert(bound>0 && bound!=f);
  CALL(vm,"physics_fixture_delete",f);
  return bound;
}
static void step(GmlVM *vm,int frames){ for(int i=0;i<frames;i++) gml_physics_step(vm); }

static void units_and_ownership(void){
  GmlVM vm; GmlWin win; setup(&vm,&win);
  double bound=bind_box(&vm,0,0,0,5,5,2);
  assert(fabs(get(&vm,0,"phy_mass")-2)<1e-6);
  vm.cur_self=&vm.inst[0];
  CALL(&vm,"physics_apply_impulse",0,0,2,0);
  assert(fabs(get(&vm,0,"phy_linear_velocity_x")-10)<1e-5);
  CALL(&vm,"physics_world_gravity",0,10);
  step(&vm,1);
  double hz=gml_room_speed(&vm);
  assert(fabs(get(&vm,0,"phy_linear_velocity_y")-100/hz)<1e-4);
  assert(fabs(vm.inst[0].x-10/hz)<1e-4);
  double x=vm.inst[0].x,y=vm.inst[0].y;
  CALL(&vm,"physics_pause_enable",1); step(&vm,8);
  assert(vm.inst[0].x==x && vm.inst[0].y==y);
  CALL(&vm,"physics_pause_enable",0);
  assert(gml_physics_variable_set(&vm,&vm.inst[0],"phy_active",vreal(0)));
  step(&vm,8); assert(vm.inst[0].x==x && vm.inst[0].y==y);
  CALL(&vm,"physics_remove_fixture",vm.inst[0].id,bound);
  assert(get(&vm,0,"phy_mass")==0);
  bind_box(&vm,0,0,0,5,5,2);
  vm.inst[0].marked=1;
  step(&vm,1);
  vm.inst[0].marked=0;
  assert(get(&vm,0,"phy_mass")==0);
  gml_vm_free(&vm);
}

static void offset_and_rotation(void){
  GmlVM vm; GmlWin win; setup(&vm,&win);
  double f=call(&vm,"physics_fixture_create",NULL,0).d;
  CALL(&vm,"physics_fixture_set_box_shape",f,5,5);
  CALL(&vm,"physics_fixture_set_density",f,2);
  CALL(&vm,"physics_fixture_bind_ext",f,vm.inst[0].id,3,4);
  CALL(&vm,"physics_fixture_delete",f);
  assert(fabs(get(&vm,0,"phy_com_x")+3)<1e-6);
  assert(fabs(get(&vm,0,"phy_com_y")+4)<1e-6);
  vm.cur_self=&vm.inst[0];
  CALL(&vm,"physics_apply_impulse",-3,-4,0,2);
  assert(fabs(get(&vm,0,"phy_angular_velocity"))<1e-4);
  CALL(&vm,"physics_apply_impulse",2,-4,0,2);
  assert(get(&vm,0,"phy_angular_velocity")>100);
  step(&vm,1);
  assert(get(&vm,0,"phy_rotation")>0 && vm.inst[0].image_angle<0);
  gml_vm_free(&vm);
}

/* Native runner controls distinguish the body position from the visual origin,
 * including deferred setters, a second attachment and an initially rotated bind. */
static void binding_origin_and_phase(void){
  GmlVM vm; GmlWin win; setup(&vm,&win);
  CALL(&vm,"physics_world_gravity",0,0);
  vm.inst[0].x=300; vm.inst[0].y=200;
  double f=call(&vm,"physics_fixture_create",NULL,0).d;
  CALL(&vm,"physics_fixture_set_box_shape",f,12,8);
  CALL(&vm,"physics_fixture_set_density",f,1);
  CALL(&vm,"physics_fixture_bind_ext",f,vm.inst[0].id,-12,-8);
  assert(get(&vm,0,"phy_position_x")==312 && get(&vm,0,"phy_position_y")==208);
  assert(fabs(get(&vm,0,"phy_com_x")-312)<1e-5 && fabs(get(&vm,0,"phy_com_y")-208)<1e-5);
  assert(fabs(get(&vm,0,"phy_mass")-3.84)<1e-5);
  assert(gml_physics_variable_set(&vm,&vm.inst[0],"phy_rotation",vreal(90)));
  assert(gml_physics_variable_set(&vm,&vm.inst[0],"phy_position_x",vreal(400)));
  assert(gml_physics_variable_set(&vm,&vm.inst[0],"phy_position_y",vreal(500)));
  CALL(&vm,"physics_fixture_set_circle_shape",f,2);
  CALL(&vm,"physics_fixture_bind_ext",f,vm.inst[0].id,-3,-4);
  assert(fabs(get(&vm,0,"phy_com_x")-400)<1e-5 && fabs(get(&vm,0,"phy_com_y")-500)<1e-5);
  assert(vm.inst[0].x==300 && vm.inst[0].y==200 && vm.inst[0].image_angle==0);
  size_t capacity=gml_vm_state_size(&vm),written=0,used=0;
  unsigned char *snapshot=malloc(capacity);
  assert(gml_vm_state_save(&vm,snapshot,capacity,&written));
  for(int replay=0;replay<2;replay++){
    if(replay) assert(gml_vm_state_load(&vm,snapshot,written,&used) && used==written);
    step(&vm,1);
    assert(fabs(vm.inst[0].x-408)<1e-4 && fabs(vm.inst[0].y-488)<1e-4);
    assert(fabs(vm.inst[0].image_angle+90)<1e-4);
    vm.inst[0].x+=10; vm.inst[0].y+=20; vm.inst[0].image_angle=0;
    assert(get(&vm,0,"phy_position_x")==400 && get(&vm,0,"phy_position_y")==500);
    step(&vm,1);
    assert(fabs(vm.inst[0].x-408)<1e-4 && fabs(vm.inst[0].y-488)<1e-4);
    assert(fabs(vm.inst[0].image_angle+90)<1e-4);
  }
  free(snapshot);
  vm.inst[1].x=300; vm.inst[1].y=200; vm.inst[1].image_angle=90;
  CALL(&vm,"physics_fixture_bind_ext",f,vm.inst[1].id,-12,-8);
  assert(fabs(get(&vm,1,"phy_position_x")-308)<1e-5);
  assert(fabs(get(&vm,1,"phy_position_y")-188)<1e-5);
  GmlPhysicsJoint stored_joint={.type=1,.a=vm.inst[0].id,.b=vm.inst[1].id,.x1=400,.y1=500};
  gml_physics_initialize_joint(&vm,&stored_joint);
  GmlPhysicsJoint *joint=&stored_joint;
  assert(fabs(joint->anchor_ax)<1e-5 && fabs(joint->anchor_ay)<1e-5);
  assert(fabs(joint->anchor_bx+312)<1e-5 && fabs(joint->anchor_by-92)<1e-5);
  gml_vm_free(&vm);
}

static void floor_joints_and_restore(void){
  GmlVM vm; GmlWin win; setup(&vm,&win);
  bind_box(&vm,0,0,100,100,5,0);
  bind_box(&vm,1,-5,20,3,5,1);
  bind_box(&vm,2,5,20,3,5,1);
  CALL(&vm,"physics_joint_revolute_create",vm.inst[1].id,vm.inst[2].id,0,20,-35,35,1,0,0,0,0);
  CALL(&vm,"physics_world_gravity",0,10);
  step(&vm,12);
  assert(vm.inst[1].y>20 && vm.inst[2].y>20);
  size_t capacity=gml_vm_state_size(&vm),written=0,used=0;
  unsigned char *snapshot=malloc(capacity);
  assert(gml_vm_state_save(&vm,snapshot,capacity,&written));
  step(&vm,100);
  double expected[6]={vm.inst[1].x,vm.inst[1].y,get(&vm,1,"phy_rotation"),
                      vm.inst[2].x,vm.inst[2].y,get(&vm,2,"phy_rotation")};
  assert(vm.inst[1].y>85 && vm.inst[1].y<96);
  assert(vm.inst[2].y>85 && vm.inst[2].y<96);
  assert(hypot(vm.inst[1].x-vm.inst[2].x,vm.inst[1].y-vm.inst[2].y)<10.2);
  assert(fabs(get(&vm,1,"phy_rotation")-get(&vm,2,"phy_rotation"))<36);
  assert(gml_vm_state_load(&vm,snapshot,written,&used) && used==written);
  step(&vm,100);
  double actual[6]={vm.inst[1].x,vm.inst[1].y,get(&vm,1,"phy_rotation"),
                    vm.inst[2].x,vm.inst[2].y,get(&vm,2,"phy_rotation")};
  assert(!memcmp(expected,actual,sizeof(expected)));
  free(snapshot); gml_vm_free(&vm);
}

static int allow_contact(void *context,int a,int b){
  assert(a==0 && b==0); return *(int *)context;
}
static void filtering_and_sensors(void){
  GmlPhysicsBody bodies[2]; GmlPhysicsFixture fixtures[2];
  GmlPhysicsContact contacts[2];
  int allow=0;
  for(int mode=0;mode<5;mode++){
    memset(bodies,0,sizeof bodies); memset(fixtures,0,sizeof fixtures);
    for(int i=0;i<2;i++){
      bodies[i].id=100001+(uint32_t)i; bodies[i].active=1;
      bodies[i].x=i*6; bodies[i].mass=i?0:1;
      fixtures[i].live=1; fixtures[i].id=(uint32_t)i+1;
      fixtures[i].bound_inst=(int)bodies[i].id; fixtures[i].shape=2;
      fixtures[i].w=fixtures[i].h=5; fixtures[i].density=i?0:1;
      fixtures[i].group=mode==1?1:mode==2?-1:0;
      fixtures[i].sensor=mode==3;
    }
    allow=mode>=2;
    if(mode==4) bodies[0].active=0;
    int count=gml_physics_solver_step(bodies,2,fixtures,2,NULL,0,0.1,0,0,
                                     1.0/60,1,10,allow_contact,&allow,contacts,2);
    assert(count==((mode==1 || mode==3)?1:0));
    if(mode==3) assert(bodies[0].x==0 && bodies[1].x>5.999);
  }
}

static void malformed_geometry_and_mass(void){
  GmlVM vm; GmlWin win; setup(&vm,&win);
  /* Invalid script or state values stop at the adapter, before native assertions. */
  double f=call(&vm,"physics_fixture_create",NULL,0).d;
  CALL(&vm,"physics_fixture_set_polygon_shape",f);
  CALL(&vm,"physics_fixture_add_point",f,0,0);
  CALL(&vm,"physics_fixture_add_point",f,0.001,0);
  CALL(&vm,"physics_fixture_add_point",f,0,0.001);
  CALL(&vm,"physics_fixture_bind_ext",f,vm.inst[0].id,10000000,10000000);
  CALL(&vm,"physics_fixture_delete",f);
  assert(get(&vm,0,"phy_mass")==0);
  bind_box(&vm,1,0,10,5,5,1);
  vm.cur_self=&vm.inst[1];
  CALL(&vm,"physics_mass_properties",1,3,4,0);
  step(&vm,1);
  assert(isfinite(vm.inst[1].x) && isfinite(vm.inst[1].y));
  CALL(&vm,"physics_joint_revolute_create",vm.inst[0].id,vm.inst[1].id,NAN,0,-35,35,1,0,0,0,0);
  bind_box(&vm,2,0,0,1e100,1e100,1);
  assert(get(&vm,2,"phy_mass")==0);
  step(&vm,1);
  assert(isfinite(vm.inst[1].x) && isfinite(vm.inst[1].y));
  gml_vm_free(&vm);
}

int main(void){
  units_and_ownership(); offset_and_rotation(); binding_origin_and_phase(); floor_joints_and_restore(); filtering_and_sensors(); malformed_geometry_and_mass();
  puts("physics: units, fixture lifetime, offsets, rotation, contacts, joints and restore passed");
  return 0;
}
