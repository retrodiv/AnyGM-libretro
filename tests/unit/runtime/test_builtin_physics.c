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

static int bound_fixture_properties(void){
  static const char *const getters[]={"physics_get_density","physics_get_friction","physics_get_restitution"};
  static const char *const setters[]={"physics_set_density","physics_set_friction","physics_set_restitution"};
  const double initial[]={2,.25,.5},changed[]={4,1.5,1.25};
  int ok=1;
  for(int cached=0;cached<2;cached++){
    GmlVM vm;GmlWin win;setup(&vm,&win);
    double source=call(&vm,"physics_fixture_create",NULL,0).d;
    CALL(&vm,"physics_fixture_set_box_shape",source,5,5);
    CALL(&vm,"physics_fixture_set_density",source,initial[0]);
    CALL(&vm,"physics_fixture_set_friction",source,initial[1]);
    CALL(&vm,"physics_fixture_set_restitution",source,initial[2]);
    double bound=CALL(&vm,"physics_fixture_bind",source,vm.inst[0].id).d;
    double sibling=CALL(&vm,"physics_fixture_bind",source,vm.inst[1].id).d;
    assert(bound>0 && sibling>0 && bound!=sibling && bound!=source);
    CALL(&vm,"physics_fixture_delete",source);
    vm.cur_self=&vm.inst[0];
    for(unsigned property=0;property<3u;property++){
      GmlVal args[]={vreal(bound),vreal(changed[property])};
      int get_id=gml_builtin_fast_id(&vm,getters[property]);
      int set_id=gml_builtin_fast_id(&vm,setters[property]);
      ok &= get_id>=0 && set_id>=0;
      GmlVal before=cached?gml_builtin_call_fast_id(&vm,get_id,getters[property],args,1):
                           gml_builtin_call(&vm,getters[property],args,1);
      ok &= before.t==V_REAL && before.d==initial[property];
      if(cached) gml_builtin_call_fast_id(&vm,set_id,setters[property],args,2);
      else gml_builtin_call(&vm,setters[property],args,2);
      GmlVal after=cached?gml_builtin_call_fast_id(&vm,get_id,getters[property],args,1):
                          gml_builtin_call(&vm,getters[property],args,1);
      ok &= after.t==V_REAL && after.d==changed[property];
      args[0]=vreal(sibling);
      vm.cur_self=&vm.inst[1];
      GmlVal other=gml_builtin_call(&vm,getters[property],args,1);
      ok &= other.t==V_REAL && other.d==initial[property];
      vm.cur_self=&vm.inst[0];
    }
    ok &= fabs(get(&vm,0,"phy_mass")-4)<1e-6 && fabs(get(&vm,1,"phy_mass")-2)<1e-6;
    size_t capacity=gml_vm_state_size(&vm),written=0,used=0;
    void *snapshot=malloc(capacity);assert(snapshot);
    assert(gml_vm_state_save(&vm,snapshot,capacity,&written));
    CALL(&vm,"physics_set_density",bound,9);
    CALL(&vm,"physics_set_friction",bound,9);
    CALL(&vm,"physics_set_restitution",bound,9);
    assert(gml_vm_state_load(&vm,snapshot,written,&used) && used==written);
    vm.cur_self=&vm.inst[0];
    for(unsigned property=0;property<3u;property++){
      GmlVal value=call(&vm,getters[property],&bound,1);
      ok &= value.t==V_REAL && value.d==changed[property];
    }
    vm.cur_self=&vm.inst[0];CALL(&vm,"physics_apply_impulse",0,0,2,0);
    ok &= fabs(get(&vm,0,"phy_linear_velocity_x")-5)<1e-5;
    free(snapshot);gml_vm_free(&vm);
  }
  puts(ok?"bound fixture properties: all six named/cached, independent copies, mass and restore passed":
          "bound fixture properties: failed");
  return ok;
}

static void read_joint_motion(GmlVM *vm,double result[12]){
  const char *const fields[]={"phy_position_x","phy_position_y","phy_rotation",
                              "phy_linear_velocity_x","phy_linear_velocity_y","phy_angular_velocity"};
  for(int body=0;body<2;body++)
    for(int field=0;field<6;field++) result[body*6+field]=get(vm,body,fields[field]);
}

static void direct_joint_constructors(void){
  static const struct {const char *name; int count,steps; double args[10];} cases[]={
    {"physics_joint_distance_create",7,8,{4,0,36,0,0}},
    {"physics_joint_friction_create",7,3,{40,0,1,.03,0}},
    {"physics_joint_weld_create",8,1192,{20,0,30,4,.8,0}},
    {"physics_joint_pulley_create",12,8,{3,0,77,0,3,0,-3,0,2,0}}
  };
  for(int cached=0;cached<2;cached++) for(unsigned index=0;index<4;index++){
    int pulley=index==3;
    GmlVM vm;GmlWin win;setup(&vm,&win);
    CALL(&vm,"physics_world_gravity",0,0);
    bind_box(&vm,0,0,pulley?40:0,2,2,pulley?1:0);
    bind_box(&vm,1,pulley?80:40,pulley?40:0,2,2,1);
    vm.cur_self=&vm.inst[0];vm.cur_other=&vm.inst[1];
    GmlVal args[12]={vreal(-1),vreal(-2)};
    const char *name=cases[index].name;
    int count=cases[index].count,id=gml_builtin_fast_id(&vm,name);
    assert(id>=0);
    for(int arg=2;arg<count;arg++) args[arg]=vreal(cases[index].args[arg-2]);
    assert(gml_builtin_call(&vm,name,args,count-1).d==-1);
    GmlVal joint=cached?gml_builtin_call_fast_id(&vm,id,name,args,count):
                         gml_builtin_call(&vm,name,args,count);
    assert(joint.t==V_REAL && joint.d>0);
    if(index==0){
      vm.cur_self=&vm.inst[1];CALL(&vm,"physics_apply_impulse",40,0,-.5,0);
    } else if(index==1){
      assert(gml_physics_variable_set(&vm,&vm.inst[1],"phy_linear_velocity_x",vreal(30)));
      assert(gml_physics_variable_set(&vm,&vm.inst[1],"phy_angular_velocity",vreal(120)));
    } else if(index==2){
      /* The off-centre body increases angular inertia. Verify the spring
       * begins moving before checking its settled angle and anchor. */
      step(&vm,8);
      double angle=get(&vm,1,"phy_rotation");
      assert(angle<0 && angle>-20);
    } else if(pulley){
      for(int body=0;body<2;body++)
        assert(gml_physics_variable_set(&vm,&vm.inst[body],"phy_fixed_rotation",vreal(1)));
      assert(gml_physics_variable_set(&vm,&vm.inst[0],"phy_position_y",vreal(45)));
      CALL(&vm,"physics_apply_impulse",0,45,0,.2);
    }
    size_t size=gml_vm_state_size(&vm),written=0,used=0;
    void *snapshot=malloc(size);
    assert(snapshot && gml_vm_state_save(&vm,snapshot,size,&written));
    double expected[12],actual[12];
    step(&vm,cases[index].steps);
    read_joint_motion(&vm,expected);
    assert(gml_vm_state_load(&vm,snapshot,written,&used) && used==written);
    step(&vm,cases[index].steps);
    read_joint_motion(&vm,actual);
    assert(!memcmp(expected,actual,sizeof actual));
    if(index==0){
      assert(fabs(actual[6]-40)<.001 && fabs(actual[9])<.001);
      CALL(&vm,"physics_joint_delete",joint.d);
      vm.cur_self=&vm.inst[1];CALL(&vm,"physics_apply_impulse",40,0,-.5,0);
      step(&vm,1);assert(get(&vm,1,"phy_position_x")<39.5);
    } else if(index==1){
      assert(actual[6]>40.1 && actual[9]>0 && actual[9]<29);
      assert(actual[11]>0 && actual[11]<119);
    } else if(index==2){
      assert(fabs(actual[8]+30)<.05);
      assert(fabs(actual[6]-(20+10*sqrt(3)))<.02 && fabs(actual[7]+10)<.02);
    } else {
      assert(actual[1]>40.5 && actual[7]<39.75);
      assert(fabs(actual[1]+2*actual[7]-120)<.002);
    }
    free(snapshot);gml_vm_free(&vm);
  }
  puts("physics joint constructors: distance, friction, weld and pulley motion and restore passed");
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
  if(!bound_fixture_properties()) return EXIT_FAILURE;
  direct_joint_constructors();
  puts("physics: units, fixture lifetime, offsets, rotation, contacts, joints and restore passed");
  return 0;
}
