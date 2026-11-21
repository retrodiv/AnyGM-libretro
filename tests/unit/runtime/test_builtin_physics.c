/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Authored bodies exercise language units, fixture ownership and restored constraints. */
#include "gml_builtin_internal.h"
#include "gml_vm_internal.h"
#include "gml_particle.h"
#include "gml_physics_solver.h"
#include "gml_render_internal.h"
#include "anygm_host.h"
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

/* A room change destroys every body. An instance that survives it keeps the ordinary coordinates
 * written before the change, and the next world builds its body there with no carried motion. */
static void room_change_rebuilds_body(void){
  GmlVM vm; GmlWin win; setup(&vm,&win);
  CALL(&vm,"physics_world_gravity",0,0);
  vm.objects[0].physics_enabled=1;
  vm.objects[0].physics_shape=0;
  vm.objects[0].physics_point_count=1;
  vm.objects[0].physics_point[0][0]=4;
  vm.objects[0].physics_density=1;
  vm.inst[0].x=10; vm.inst[0].y=20;
  step(&vm,1);
  assert(get(&vm,0,"phy_position_x")==10 && get(&vm,0,"phy_position_y")==20);
  assert(gml_physics_variable_set(&vm,&vm.inst[0],"phy_linear_velocity_x",vreal(30)));
  assert(gml_physics_variable_set(&vm,&vm.inst[0],"phy_fixed_rotation",vreal(1)));
  vm.inst[0].x=500; vm.inst[0].y=600;
  gml_builtin_physics_room_reset(&vm);
  CALL(&vm,"physics_world_gravity",0,0);
  step(&vm,1);
  assert(vm.inst[0].x==500 && vm.inst[0].y==600);
  assert(get(&vm,0,"phy_position_x")==500 && get(&vm,0,"phy_position_y")==600);
  assert(get(&vm,0,"phy_linear_velocity_x")==0 && get(&vm,0,"phy_fixed_rotation")==0);
  assert(get(&vm,0,"phy_mass")>0);
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

static void motor_joint_constructors(void){
  const char *toggle="physics_joint_enable_motor";
  static const struct {const char *name; int count; double args[11];} cases[]={
    {"physics_joint_prismatic_create",13,{20,0,3,0,-8,8,1,10,1,0,0}},
    {"physics_joint_wheel_create",12,{20,0,0,3,0,10,2,4,.8,0}},
    {"physics_joint_revolute_create",11,{20,0,-90,90,0,10,30,0,0}}
  };
  for(int cached=0;cached<2;cached++) for(unsigned index=0;index<3;index++){
    GmlVM vm;GmlWin win;setup(&vm,&win);
    CALL(&vm,"physics_world_gravity",0,0);
    bind_box(&vm,0,0,0,2,2,0);
    bind_box(&vm,1,20,0,2,2,1);
    assert(gml_physics_variable_set(&vm,&vm.inst[0],"phy_rotation",vreal(90)));
    const char *name=cases[index].name;
    int count=cases[index].count,id=gml_builtin_fast_id(&vm,name);
    int motor=gml_builtin_fast_id(&vm,toggle);
    assert(id>=0 && motor>=0);
    GmlVal args[13]={vreal(vm.inst[0].id),vreal(vm.inst[1].id)};
    for(int arg=2;arg<count;arg++) args[arg]=vreal(cases[index].args[arg-2]);
    if(index<2){
      assert(gml_builtin_call(&vm,name,args,count-1).d==-1);
      GmlVal x=args[4],y=args[5];
      args[4]=args[5]=vreal(0);
      assert(gml_builtin_call(&vm,name,args,count).d==-1);
      args[4]=x;args[5]=y;
      if(index==0){
        args[6]=vreal(9);
        assert(gml_builtin_call(&vm,name,args,count).d==-1);
        args[6]=vreal(cases[index].args[4]);
      }
    }
    GmlVal joint=cached?gml_builtin_call_fast_id(&vm,id,name,args,count):
                         gml_builtin_call(&vm,name,args,count);
    assert(joint.t==V_REAL && joint.d>0);
    step(&vm,1);
    assert(fabs(get(&vm,1,"phy_position_x")-20)<.001);
    assert(fabs(get(&vm,1,"phy_angular_velocity"))<.001);
    GmlVal on[2]={joint,vreal(1)};
    if(cached) gml_builtin_call_fast_id(&vm,motor,toggle,on,2);
    else gml_builtin_call(&vm,toggle,on,2);
    if(index==0){
      assert(gml_physics_variable_set(&vm,&vm.inst[1],"phy_linear_velocity_y",vreal(35)));
      assert(gml_physics_variable_set(&vm,&vm.inst[1],"phy_angular_velocity",vreal(30)));
      step(&vm,3);
      assert(fabs(get(&vm,1,"phy_linear_velocity_x")-10)<.001);
      assert(fabs(get(&vm,1,"phy_position_y"))<.001);
      assert(fabs(get(&vm,1,"phy_rotation"))<.001);
    } else if(index==1){
      assert(gml_physics_variable_set(&vm,&vm.inst[1],"phy_linear_velocity_x",vreal(35)));
      assert(gml_physics_variable_set(&vm,&vm.inst[1],"phy_position_y",vreal(5)));
      step(&vm,1);
      assert(fabs(get(&vm,1,"phy_position_x")-20)<.001);
      assert(get(&vm,1,"phy_position_y")>0 && get(&vm,1,"phy_position_y")<5);
      assert(fabs(get(&vm,1,"phy_angular_velocity")-2*180/acos(-1))<.001);
    } else {
      step(&vm,1);
      assert(fabs(get(&vm,1,"phy_angular_velocity")-30)<.001);
    }

    size_t size=gml_vm_state_size(&vm),written=0,used=0;
    void *snapshot=malloc(size);
    assert(snapshot && gml_vm_state_save(&vm,snapshot,size,&written));
    double expected[12],actual[12];
    step(&vm,75);
    read_joint_motion(&vm,expected);
    assert(gml_vm_state_load(&vm,snapshot,written,&used) && used==written);
    step(&vm,75);
    read_joint_motion(&vm,actual);
    assert(!memcmp(expected,actual,sizeof actual));
    if(index==0) assert(fabs(actual[6]-28)<.06);
    if(index==1) assert(fabs(actual[7])<.02);

    on[1]=vreal(0);
    if(cached) gml_builtin_call_fast_id(&vm,motor,toggle,on,2);
    else gml_builtin_call(&vm,toggle,on,2);
    if(index==0){
      assert(gml_physics_variable_set(&vm,&vm.inst[1],"phy_linear_velocity_x",vreal(-6)));
      step(&vm,3);
      assert(get(&vm,1,"phy_position_x")<27.9);
    } else {
      assert(gml_physics_variable_set(&vm,&vm.inst[1],"phy_angular_velocity",vreal(0)));
      step(&vm,3);
      assert(fabs(get(&vm,1,"phy_angular_velocity"))<.001);
    }
    free(snapshot);gml_vm_free(&vm);
  }
  puts("motor joints: prismatic limit, wheel suspension, motor switches and restore passed");
}

static double gear_source(GmlVM *vm,int slot,int prismatic,double x){
  if(prismatic){
    const double args[]={vm->inst[0].id,vm->inst[slot].id,x,0,1,0,-100,100,0,0,0,0,0};
    GmlVal values[13];
    for(int i=0;i<13;i++) values[i]=vreal(args[i]);
    return gml_builtin_call(vm,"physics_joint_prismatic_create",values,13).d;
  }
  return CALL(vm,"physics_joint_revolute_create",vm->inst[0].id,vm->inst[slot].id,
              x,0,-90,90,0,0,0,0,0).d;
}

static void gear_joint_constructor(void){
  const char *name="physics_joint_gear_create";
  for(int cached=0;cached<2;cached++) for(int pair=0;pair<4;pair++){
    int first_prismatic=(pair&2)!=0,second_prismatic=(pair&1)!=0;
    double ratio=pair&1?2:-.5;
    const char *first_coordinate=first_prismatic?"phy_position_x":"phy_rotation";
    const char *second_coordinate=second_prismatic?"phy_position_x":"phy_rotation";
    const char *first_speed=first_prismatic?"phy_linear_velocity_x":"phy_angular_velocity";
    const double first_unit=first_prismatic?.1:acos(-1)/180;
    const double second_unit=second_prismatic?.1:acos(-1)/180;
    GmlVM vm;GmlWin win;setup(&vm,&win);
    CALL(&vm,"physics_world_gravity",0,0);
    bind_box(&vm,0,0,0,2,2,0);
    bind_box(&vm,1,20,0,2,2,1);
    bind_box(&vm,2,60,0,2,2,1);
    double source_a=gear_source(&vm,1,first_prismatic,20);
    double source_b=gear_source(&vm,2,second_prismatic,60);
    assert(source_a>0 && source_b>0);
    double initial_a=first_prismatic?24:12,initial_b=second_prismatic?64:-7;
    assert(gml_physics_variable_set(&vm,&vm.inst[1],first_coordinate,vreal(initial_a)));
    assert(gml_physics_variable_set(&vm,&vm.inst[2],second_coordinate,vreal(initial_b)));
    vm.cur_self=&vm.inst[1];vm.cur_other=&vm.inst[2];
    int id=gml_builtin_fast_id(&vm,name);
    assert(id>=0);
    GmlVal args[]={vreal(-1),vreal(-2),vreal(source_a),vreal(source_b),vreal(ratio)};
    assert(gml_builtin_call(&vm,name,args,4).d==-1);
    args[3]=vreal(source_a);
    assert(gml_builtin_call(&vm,name,args,5).d==-1);
    args[3]=vreal(source_b);
    GmlVal gear=cached?gml_builtin_call_fast_id(&vm,id,name,args,5):
                       gml_builtin_call(&vm,name,args,5);
    assert(gear.t==V_REAL && gear.d>0);

    /* Perturb an already coupled body before the first native solve. Capturing
     * the constant at the first step would make this displacement permanent. */
    assert(gml_physics_variable_set(&vm,&vm.inst[1],first_coordinate,vreal(initial_a+5)));
    assert(gml_physics_variable_set(&vm,&vm.inst[1],first_speed,vreal(18)));
    step(&vm,6);
    double observed_a=get(&vm,1,first_coordinate),observed_b=get(&vm,2,second_coordinate);
    assert(fabs((observed_a-initial_a)*first_unit+
                ratio*(observed_b-initial_b)*second_unit)<.0001);
    assert(fabs(observed_b-initial_b)>.01);

    size_t size=gml_vm_state_size(&vm),written=0,used=0;
    void *snapshot=malloc(size);
    assert(snapshot && gml_vm_state_save(&vm,snapshot,size,&written));
    step(&vm,12);
    double final_a=get(&vm,1,first_coordinate),final_b=get(&vm,2,second_coordinate);
    assert(fabs((final_a-initial_a)*first_unit+ratio*(final_b-initial_b)*second_unit)<.0001);
    assert(fabs(final_b-observed_b)>.01);
    assert(gml_vm_state_load(&vm,snapshot,written,&used) && used==written);
    step(&vm,12);
    assert(get(&vm,1,first_coordinate)==final_a && get(&vm,2,second_coordinate)==final_b);

    CALL(&vm,"physics_joint_delete",source_a);
    for(int i=0;i<GML_PHYS_JOINT_MAX;i++)
      assert(!vm.builtins->phys_joint[i].live || vm.builtins->phys_joint[i].id!=gear.d);
    step(&vm,1);
    free(snapshot);gml_vm_free(&vm);
  }
  puts("gear joint: all source pairs, signed ratios, fixed coupling, deletion and restore passed");
}

static double joint_field(GmlVM *vm,int cached,double joint,int field){
  const char *name="physics_joint_get_value";
  GmlVal args[]={vreal(joint),vreal(field)};
  GmlVal result=cached?gml_builtin_call_fast_id(vm,gml_builtin_fast_id(vm,name),name,args,2):
                       gml_builtin_call(vm,name,args,2);
  assert(result.t==V_REAL);
  return result.d;
}

static void write_joint_field(GmlVM *vm,int cached,double joint,int field,double value){
  const char *name="physics_joint_set_value";
  GmlVal args[]={vreal(joint),vreal(field),vreal(value)};
  if(cached) gml_builtin_call_fast_id(vm,gml_builtin_fast_id(vm,name),name,args,3);
  else gml_builtin_call(vm,name,args,3);
}

static void joint_fields(void){
  for(int cached=0;cached<2;cached++){
    GmlVM vm;GmlWin win;setup(&vm,&win);
#define GET(j,field) joint_field(&vm,cached,j,GML_JOINT_##field)
#define SET(j,field,value) write_joint_field(&vm,cached,j,GML_JOINT_##field,value)
#define EXPECT_CONSTANT(symbol, value, name) do { \
      GmlVal result=gml_vm_identifier_get(&vm,name); \
      assert(result.t==V_REAL && result.d==value); \
    } while(0);
    GML_PHYS_JOINT_FIELDS(EXPECT_CONSTANT)
#undef EXPECT_CONSTANT
    bind_box(&vm,0,0,0,2,2,0);
    bind_box(&vm,1,40,0,2,2,1);
    bind_box(&vm,2,0,40,2,2,1);
    CALL(&vm,"physics_world_gravity",5,0);
    double distance=CALL(&vm,"physics_joint_distance_create",vm.inst[0].id,
                         vm.inst[1].id,0,0,40,0,0).d;
    assert(distance>0);
    assert(GET(distance,ANCHOR_1_X)==0 && GET(distance,ANCHOR_1_Y)==0);
    assert(GET(distance,ANCHOR_2_X)==4 && GET(distance,ANCHOR_2_Y)==0);
    assert(GET(distance,LENGTH_1)==4 && GET(distance,LENGTH_2)==0);
    assert(GET(distance,TRANSLATION)==0 && GET(distance,REACTION_FORCE_X)==0);
    SET(distance,ANCHOR_2_X,99);
    assert(GET(distance,ANCHOR_2_X)==4);
    GmlPhysicsJoint *old_joint=gml_physics_find_joint(&vm,distance);
    assert(old_joint);
    old_joint->value_count=24;
    old_joint->params[GML_JOINT_SAMPLE_FORCE_X]=123;
    assert(GET(distance,REACTION_FORCE_X)==0);
    step(&vm,1);
    double force=GET(distance,REACTION_FORCE_X);
    assert(fabs(force+5*get(&vm,1,"phy_mass"))<.0001);
    assert(GET(distance,REACTION_FORCE_Y)==0 && GET(distance,REACTION_TORQUE)==0);
    size_t size=gml_vm_state_size(&vm),written=0,used=0;
    void *snapshot=malloc(size);
    assert(snapshot && gml_vm_state_save(&vm,snapshot,size,&written));
    SET(distance,LENGTH_1,3);
    SET(distance,FREQUENCY,4);
    SET(distance,DAMPING_RATIO,.8);
    assert(GET(distance,LENGTH_1)==3 && GET(distance,FREQUENCY)==4 && GET(distance,DAMPING_RATIO)==.8);
    step(&vm,1);
    assert(get(&vm,1,"phy_position_x")>30 && get(&vm,1,"phy_position_x")<40);
    step(&vm,80);
    assert(fabs(get(&vm,1,"phy_position_x")-30)<.1);
    assert(gml_vm_state_load(&vm,snapshot,written,&used) && used==written);
    assert(GET(distance,REACTION_FORCE_X)==force && GET(distance,LENGTH_1)==4);
    assert(GET(distance,FREQUENCY)==0 && GET(distance,DAMPING_RATIO)==0);
    free(snapshot);
    CALL(&vm,"physics_joint_delete",distance);
    assert(GET(distance,REACTION_FORCE_X)==0);

    CALL(&vm,"physics_world_gravity",0,0);
    double prismatic=gear_source(&vm,1,1,40);
    assert(prismatic>0);
    assert(gml_physics_variable_set(&vm,&vm.inst[1],"phy_position_x",vreal(44)));
    assert(gml_physics_variable_set(&vm,&vm.inst[1],"phy_linear_velocity_x",vreal(5)));
    double translation=GET(prismatic,TRANSLATION),speed=GET(prismatic,SPEED);
    assert(fabs(translation-.4)<1e-12 && fabs(speed-.5)<1e-12);
    SET(prismatic,MOTOR_SPEED,2);
    SET(prismatic,MAX_MOTOR_FORCE,.25);
    assert(GET(prismatic,MOTOR_SPEED)==2 && GET(prismatic,MAX_MOTOR_FORCE)==.25);
    CALL(&vm,"physics_joint_enable_motor",prismatic,1);
    step(&vm,1);
    assert(fabs(GET(prismatic,MOTOR_FORCE)-.25)<1e-6 && GET(prismatic,SPEED)>.5);
    assert(GET(prismatic,MOTOR_TORQUE)==0);
    CALL(&vm,"physics_joint_delete",prismatic);

    double revolute=gear_source(&vm,2,0,0);
    assert(revolute>0);
    SET(revolute,LOWER_ANGLE_LIMIT,100);
    assert(GET(revolute,LOWER_ANGLE_LIMIT)==90 && GET(revolute,UPPER_ANGLE_LIMIT)==90);
    SET(revolute,LOWER_ANGLE_LIMIT,-40);
    SET(revolute,UPPER_ANGLE_LIMIT,60);
    SET(revolute,ANGLE_LIMITS,1);
    SET(revolute,MOTOR_SPEED,2);
    SET(revolute,MAX_MOTOR_TORQUE,.5);
    assert(GET(revolute,LOWER_ANGLE_LIMIT)==-40 && GET(revolute,UPPER_ANGLE_LIMIT)==60);
    assert(GET(revolute,ANGLE_LIMITS)==1 && fabs(GET(revolute,MOTOR_SPEED)-2)<1e-12);
    assert(GET(revolute,MAX_MOTOR_TORQUE)==.5);
    assert(gml_physics_variable_set(&vm,&vm.inst[2],"phy_rotation",vreal(30)));
    assert(fabs(GET(revolute,ANGLE)-acos(-1)/6)<1e-12);
    CALL(&vm,"physics_joint_enable_motor",revolute,1);
    step(&vm,1);
    assert(fabs(GET(revolute,MOTOR_TORQUE))>0 && fabs(GET(revolute,MOTOR_TORQUE))<=.500001);
    CALL(&vm,"physics_joint_delete",revolute);

    double pulley=CALL(&vm,"physics_joint_pulley_create",vm.inst[0].id,
                       vm.inst[1].id,0,-10,40,-10,0,0,0,0,1,0).d;
    assert(pulley>0);
    assert(fabs(GET(pulley,LENGTH_1)-1)<1e-12 && GET(pulley,LENGTH_2)>1);
    double friction=CALL(&vm,"physics_joint_friction_create",vm.inst[0].id,
                         vm.inst[1].id,40,0,1,2,0).d;
    assert(friction>0);
    SET(friction,MAX_FORCE,4);
    SET(friction,MAX_TORQUE,5);
    assert(GET(friction,MAX_FORCE)==4 && GET(friction,MAX_TORQUE)==5);
    double rope=CALL(&vm,"physics_joint_rope_create",vm.inst[0].id,vm.inst[1].id,
                     0,0,0,0,60,0).d;
    assert(rope>0 && GET(rope,MAX_LENGTH)==6);
    SET(rope,MAX_LENGTH,3);
    assert(GET(rope,MAX_LENGTH)==3);

    double wheel=CALL(&vm,"physics_joint_wheel_create",vm.inst[0].id,
                      vm.inst[2].id,0,40,0,1,0,2,-3,5,.6,0).d;
    assert(wheel>0);
    assert(GET(wheel,FREQUENCY)==5 && GET(wheel,DAMPING_RATIO)==.6);
    SET(wheel,FREQUENCY,7);
    SET(wheel,DAMPING_RATIO,.7);
    assert(GET(wheel,FREQUENCY)==7 && GET(wheel,DAMPING_RATIO)==.7);
    double wheel_y=get(&vm,2,"phy_position_y");
    assert(gml_physics_variable_set(&vm,&vm.inst[2],"phy_position_y",vreal(wheel_y+5)));
    assert(gml_physics_variable_set(&vm,&vm.inst[2],"phy_angular_velocity",vreal(-90)));
    assert(fabs(GET(wheel,TRANSLATION)-.5)<1e-12);
    assert(fabs(GET(wheel,SPEED)+acos(-1)/2)<1e-12);

    double weld=CALL(&vm,"physics_joint_weld_create",vm.inst[0].id,
                     vm.inst[2].id,0,40,30,4,.8,0).d;
    assert(weld>0);
    assert(fabs(GET(weld,ANGLE)+acos(-1)/6)<1e-12);
    SET(weld,FREQUENCY,6);
    SET(weld,DAMPING_RATIO,.9);
    assert(GET(weld,FREQUENCY)==6 && GET(weld,DAMPING_RATIO)==.9);
#undef SET
#undef GET
    gml_vm_free(&vm);
  }
  puts("joint properties: constants, live geometry, solver forces, typed writes and restore passed");
}

static GmlVal physics_query(GmlVM *vm,int cached,const char *name,GmlVal *args,int count){
  int id=gml_builtin_fast_id(vm,name);
  assert(id>=0);
  return cached?gml_builtin_call_fast_id(vm,id,name,args,count):gml_builtin_call(vm,name,args,count);
}

typedef struct {
  int count;
  AnygmLogLevel level;
  char text[256];
} PhysicsErrorLog;

static void physics_error_log(void *userdata,AnygmLogLevel level,const char *text){
  PhysicsErrorLog *log=userdata;
  log->count++;log->level=level;
  snprintf(log->text,sizeof log->text,"%s",text?text:"");
}

static void error_output_control(void){
  for(int cached=0;cached<2;cached++){
    GmlVM vm,peer;GmlWin win,peer_win;
    setup(&vm,&win);setup(&peer,&peer_win);
    PhysicsErrorLog log={0},peer_log={0};
    AnygmHostServices host={.struct_size=sizeof(AnygmHostServices),
      .abi_version=ANYGM_HOST_SERVICES_VERSION,
      .userdata=&log,.log=physics_error_log};
    AnygmHostServices peer_host=host;peer_host.userdata=&peer_log;
    vm.host=&host;peer.host=&peer_host;
    assert(gml_builtin_fast_id(&vm,"physics_debug")>0);
    CALL(&vm,"physics_world_create",-1);
    CALL(&vm,"physics_fixture_set_circle_shape",987654,2);
    assert(log.count==0);

    size_t size=gml_vm_state_size(&vm),written=0,repeated=0;
    unsigned char *before=malloc(size),*after=malloc(size);
    assert(before && after && gml_vm_state_save(&vm,before,size,&written));
    GmlVal enabled=vreal(1),disabled=vreal(0);
    GmlVal result=physics_query(&vm,cached,"physics_debug",&enabled,1);
    assert(result.t==V_UNDEF && log.count==0);
    assert(gml_vm_state_save(&vm,after,size,&repeated));
    assert(written==repeated && !memcmp(before,after,written));
    free(before);free(after);

    CALL(&vm,"physics_fixture_set_circle_shape",987654,2);
    assert(log.count==1 && log.level==ANYGM_LOG_ERROR && strstr(log.text,"[physics]"));
    CALL(&vm,"physics_joint_delete",987654);
    assert(log.count==2);
    CALL(&vm,"physics_world_create",-1);
    assert(log.count==3);
    double fixture=call(&vm,"physics_fixture_create",NULL,0).d;
    CALL(&vm,"physics_fixture_set_box_shape",fixture,2,3);
    CALL(&vm,"physics_fixture_delete",fixture);
    assert(log.count==3);
    fixture=call(&vm,"physics_fixture_create",NULL,0).d;
    CALL(&vm,"physics_fixture_set_chain_shape",fixture,0);
    CALL(&vm,"physics_fixture_add_point",fixture,0,0);
    assert(CALL(&vm,"physics_fixture_bind",fixture,vm.inst[0].id).d==0);
    assert(log.count==4);
    CALL(&peer,"physics_world_create",-1);
    assert(peer_log.count==0);
    result=physics_query(&vm,cached,"physics_debug",NULL,0);
    assert(result.t==V_UNDEF);
    CALL(&vm,"physics_world_create",-1);
    assert(log.count==5);
    GmlVal invalid=vreal(NAN);
    result=physics_query(&vm,cached,"physics_debug",&invalid,1);
    assert(result.t==V_UNDEF);
    CALL(&vm,"physics_world_create",-1);
    assert(log.count==6);
    result=physics_query(&vm,cached,"physics_debug",&disabled,1);
    assert(result.t==V_UNDEF);
    CALL(&vm,"physics_world_create",-1);
    assert(log.count==6);
    vm.host=NULL;
    physics_query(&vm,cached,"physics_debug",&enabled,1);
    CALL(&vm,"physics_world_create",-1);
    gml_vm_free(&vm);gml_vm_free(&peer);
  }
  puts("physics error output: named/cached default-off, gated failures, VM isolation and unchanged game-state bytes passed");
}

static double hit_field(GmlVM *vm,GmlVal hits,int index,const char *field){
  GmlVal record=gml_arr_get(hits,index);
  assert(record.t==V_REAL);
  GmlInstance *instance=gml_struct_find(vm,(uint32_t)record.d);
  assert(instance);
  GmlVal *value=gml_varmap_get(&instance->vars,field);
  assert(value && value->t==V_REAL);
  return value->d;
}

static void overlap_queries(void){
  const char *name="physics_test_overlap";
  for(int cached=0;cached<2;cached++){
    GmlVM vm;GmlWin win;setup(&vm,&win);
    vm.cur_self=&vm.inst[0]; vm.cur_other=&vm.inst[1];
    double fixture=call(&vm,"physics_fixture_create",NULL,0).d;
    CALL(&vm,"physics_fixture_set_box_shape",fixture,6,1);
    assert(CALL(&vm,"physics_fixture_bind_ext",fixture,vm.inst[0].id,3,0).d>0);
    CALL(&vm,"physics_fixture_delete",fixture);
    bind_box(&vm,1,20,11,1,1,0);
    bind_box(&vm,2,50,11,1,1,0);
    GmlVal args[]={vreal(20),vreal(20),vreal(90),vreal(-2)};
    assert(physics_query(&vm,cached,name,args,4).d==1);
    args[2]=vreal(-90);
    assert(physics_query(&vm,cached,name,args,4).d==0);
    args[2]=vreal(90);
    args[3]=vreal(vm.inst[2].id);
    assert(physics_query(&vm,cached,name,args,4).d==0);
    args[3]=vobject(0);
    assert(physics_query(&vm,cached,name,args,4).d==1);
    args[0]=vreal(0); args[1]=vreal(0); args[2]=vreal(0); args[3]=vreal(-3);
    assert(physics_query(&vm,cached,name,args,4).d==0);
    args[0]=vreal(NAN);
    assert(physics_query(&vm,cached,name,args,4).d==0);
    assert(get(&vm,0,"phy_position_x")==-3 && get(&vm,0,"phy_rotation")==0);
    assert(vm.inst[0].x==0 && vm.inst[0].y==0);
    gml_vm_free(&vm);
  }
}

static void raycast_queries(void){
  const char *name="physics_raycast";
  for(int cached=0;cached<2;cached++){
    GmlVM vm;GmlWin win;setup(&vm,&win);
    GmlObject *objects=realloc(vm.objects,2*sizeof(*objects));
    assert(objects);
    vm.objects=objects; vm.n_objects=2;
    memset(&vm.objects[1],0,sizeof(vm.objects[1]));
    vm.objects[1].parent=0;
    vm.inst[1].obj=1;
    bind_box(&vm,2,70,0,2,2,0);
    bind_box(&vm,1,20,0,2,2,0);
    double edge=call(&vm,"physics_fixture_create",NULL,0).d;
    CALL(&vm,"physics_fixture_set_edge_shape",edge,4,-2,4,2);
    assert(CALL(&vm,"physics_fixture_bind",edge,vm.inst[1].id).d>0);
    CALL(&vm,"physics_fixture_delete",edge);

    GmlVal args[]={vreal(0),vreal(0),vreal(100),vreal(0),vobject(0),vreal(1),vreal(1)};
    GmlVal hits=physics_query(&vm,cached,name,args,5);
    assert(gml_val_array_length(hits)==1);
    assert(hit_field(&vm,hits,0,"instance")==vm.inst[1].id);
    assert(fabs(hit_field(&vm,hits,0,"hitpointX")-18)<.0001);
    assert(hit_field(&vm,hits,0,"hitpointY")==0 && hit_field(&vm,hits,0,"normalX")==-1);
    assert(hit_field(&vm,hits,0,"normalY")==0 && fabs(hit_field(&vm,hits,0,"fraction")-.18)<1e-6);
    gml_values_release_owned(&hits,1);

    GmlVal selectors=gml_arr_new(2,vobject(0));
    gml_arr_set(selectors,1,vreal(vm.inst[1].id));
    args[4]=selectors;
    hits=physics_query(&vm,cached,name,args,7);
    assert(gml_val_array_length(hits)==3);
    assert(hit_field(&vm,hits,1,"instance")==vm.inst[1].id);
    assert(fabs(hit_field(&vm,hits,1,"hitpointX")-24)<.0001);
    assert(hit_field(&vm,hits,2,"instance")==vm.inst[2].id);
    assert(fabs(hit_field(&vm,hits,2,"hitpointX")-68)<.0001);
    gml_values_release_owned(&hits,1);
    gml_values_release_owned(&selectors,1);

    args[4]=vreal(vm.inst[2].id);
    args[6]=vreal(.5);
    assert(physics_query(&vm,cached,name,args,7).t==V_UNDEF);
    args[4]=vreal(vm.inst[1].id);
    hits=physics_query(&vm,cached,name,args,7);
    assert(gml_val_array_length(hits)==2);
    gml_values_release_owned(&hits,1);
    assert(gml_physics_variable_set(&vm,&vm.inst[1],"phy_active",vreal(0)));
    assert(physics_query(&vm,cached,name,args,7).t==V_UNDEF);
    args[4]=vobject(0); args[6]=vreal(1);
    args[2]=vreal(0);
    assert(physics_query(&vm,cached,name,args,7).t==V_UNDEF);
    args[2]=vreal(INFINITY);
    assert(physics_query(&vm,cached,name,args,7).t==V_UNDEF);
    gml_vm_free(&vm);
  }
  puts("physics queries: rotated offset overlap, compound ray hits, selectors and fractions passed");
}

static void kinematic_fixture_templates(void){
  const char *name="physics_fixture_set_kinematic";
  for(int cached=0;cached<2;cached++){
    GmlVM vm;GmlWin win;setup(&vm,&win);
    vm.inst[1].x=50; vm.inst[2].x=100;
    double implicit=call(&vm,"physics_fixture_create",NULL,0).d;
    double explicit=call(&vm,"physics_fixture_create",NULL,0).d;
    assert(implicit>0 && explicit>0);
    CALL(&vm,"physics_fixture_set_box_shape",implicit,2,2);
    CALL(&vm,"physics_fixture_set_box_shape",explicit,2,2);
    CALL(&vm,"physics_fixture_set_linear_damping",implicit,0);
    CALL(&vm,"physics_fixture_set_density",explicit,.5);
    double ordinary=CALL(&vm,"physics_fixture_bind",implicit,vm.inst[0].id).d;
    assert(ordinary>0 && get(&vm,0,"phy_dynamic")==1);
    GmlVal first=vreal(implicit),second=vreal(explicit);
    physics_query(&vm,cached,name,&first,1);
    physics_query(&vm,cached,name,&second,1);
    double additional=CALL(&vm,"physics_fixture_bind",implicit,vm.inst[0].id).d;
    assert(additional>0 && get(&vm,0,"phy_dynamic")==1 && get(&vm,0,"phy_kinematic")==0);
    vm.cur_self=&vm.inst[0];
    assert(CALL(&vm,"physics_get_density",ordinary).d==.5);

    size_t size=gml_vm_state_size(&vm),written=0,used=0;
    void *snapshot=malloc(size);
    assert(snapshot && gml_vm_state_save(&vm,snapshot,size,&written));
    CALL(&vm,"physics_fixture_delete",implicit);
    CALL(&vm,"physics_fixture_delete",explicit);
    assert(gml_vm_state_load(&vm,snapshot,written,&used) && used==written);
    free(snapshot);
    double kinematic=CALL(&vm,"physics_fixture_bind",implicit,vm.inst[1].id).d;
    double dynamic=CALL(&vm,"physics_fixture_bind",explicit,vm.inst[2].id).d;
    assert(kinematic>0 && dynamic>0);
    assert(get(&vm,1,"phy_kinematic")==1 && get(&vm,1,"phy_dynamic")==0);
    assert(get(&vm,2,"phy_kinematic")==0 && get(&vm,2,"phy_dynamic")==1);
    vm.cur_self=&vm.inst[1];
    assert(CALL(&vm,"physics_get_density",kinematic).d==0);
    vm.cur_self=&vm.inst[2];
    assert(CALL(&vm,"physics_get_density",dynamic).d==.5);

    CALL(&vm,"physics_world_gravity",0,9.8);
    vm.cur_self=&vm.inst[1];
    assert(gml_physics_variable_set(&vm,&vm.inst[1],"phy_linear_velocity_x",vreal(30)));
    CALL(&vm,"physics_apply_impulse",50,0,5,5);
    assert(get(&vm,1,"phy_linear_velocity_x")==30 && get(&vm,1,"phy_linear_velocity_y")==0);
    size=gml_vm_state_size(&vm);
    snapshot=malloc(size);
    assert(snapshot && gml_vm_state_save(&vm,snapshot,size,&written));
    step(&vm,8);
    double final_x=get(&vm,1,"phy_position_x");
    assert(final_x>50 && get(&vm,1,"phy_position_y")==0);
    assert(fabs(get(&vm,1,"phy_linear_velocity_x")-30)<1e-6);
    assert(get(&vm,2,"phy_position_y")>0);
    assert(gml_vm_state_load(&vm,snapshot,written,&used) && used==written);
    step(&vm,8);
    assert(get(&vm,1,"phy_position_x")==final_x && get(&vm,1,"phy_kinematic")==1);
    free(snapshot);

    CALL(&vm,"physics_remove_fixture",vm.inst[1].id,kinematic);
    assert(CALL(&vm,"physics_fixture_bind",explicit,vm.inst[1].id).d>0);
    assert(get(&vm,1,"phy_kinematic")==0 && get(&vm,1,"phy_dynamic")==1);
    gml_vm_free(&vm);
  }
  puts("kinematic fixtures: template isolation, density precedence, motion and restore passed");
}

static void select_chain_shape(GmlVM *vm,int cached,double fixture,int loop){
  GmlVal args[]={vreal(fixture),vreal(loop)};
  physics_query(vm,cached,"physics_fixture_set_chain_shape",args,2);
}

static void chain_fixtures(void){
  for(int cached=0;cached<2;cached++){
    GmlVM vm;GmlWin win;setup(&vm,&win);
    vm.inst[0].x=10;vm.inst[0].y=20;
    vm.inst[1].x=100;vm.inst[1].y=30;
    double open=call(&vm,"physics_fixture_create",NULL,0).d;
    select_chain_shape(&vm,cached,open,0);
    for(int i=0;i<=20;i++) CALL(&vm,"physics_fixture_add_point",open,i*4,0);
    double open_bound=CALL(&vm,"physics_fixture_bind",open,vm.inst[0].id).d;
    assert(open_bound>0 && get(&vm,0,"phy_mass")==0);
    CALL(&vm,"physics_fixture_set_circle_shape",open,1);
    CALL(&vm,"physics_fixture_delete",open);

    double loop=call(&vm,"physics_fixture_create",NULL,0).d;
    select_chain_shape(&vm,cached,loop,1);
    CALL(&vm,"physics_fixture_add_point",loop,0,0);
    CALL(&vm,"physics_fixture_add_point",loop,16,0);
    assert(CALL(&vm,"physics_fixture_bind",loop,vm.inst[1].id).d==0);
    CALL(&vm,"physics_fixture_add_point",loop,16,16);
    CALL(&vm,"physics_fixture_add_point",loop,0,0);
    assert(CALL(&vm,"physics_fixture_bind",loop,vm.inst[1].id).d==0);
    select_chain_shape(&vm,cached,loop,1);
    CALL(&vm,"physics_fixture_add_point",loop,0,0);
    CALL(&vm,"physics_fixture_add_point",loop,16,0);
    CALL(&vm,"physics_fixture_add_point",loop,16,16);
    CALL(&vm,"physics_fixture_set_sensor",loop,1);
    double loop_bound=CALL(&vm,"physics_fixture_bind",loop,vm.inst[1].id).d;
    assert(loop_bound>0);
    CALL(&vm,"physics_fixture_delete",loop);

    GmlVal args[]={vreal(95),vreal(38),vreal(120),vreal(38),vreal(vm.inst[1].id),vreal(1)};
    GmlVal hits=physics_query(&vm,cached,"physics_raycast",args,6);
    assert(gml_val_array_length(hits)==2);
    assert(fabs(hit_field(&vm,hits,0,"hitpointX")-108)<.001);
    assert(fabs(hit_field(&vm,hits,1,"hitpointX")-116)<.001);
    gml_values_release_owned(&hits,1);
    args[0]=vreal(87);args[1]=vreal(27);args[2]=vreal(87);args[3]=vreal(13);
    args[4]=vreal(vm.inst[0].id);
    hits=physics_query(&vm,cached,"physics_raycast",args,6);
    assert(gml_val_array_length(hits)==1);
    assert(fabs(hit_field(&vm,hits,0,"hitpointY")-20)<.001);
    assert(hit_field(&vm,hits,0,"normalY")==1);
    gml_values_release_owned(&hits,1);

    bind_box(&vm,2,87,10,1,1,1);
    vm.cur_self=&vm.inst[2];
    GmlVal overlap[]={vreal(87),vreal(20),vreal(0),vreal(vm.inst[0].id)};
    assert(physics_query(&vm,cached,"physics_test_overlap",overlap,4).d==1);
    overlap[1]=vreal(17);
    assert(physics_query(&vm,cached,"physics_test_overlap",overlap,4).d==0);
    CALL(&vm,"physics_world_gravity",0,9.8);
    CALL(&vm,"physics_fixture_set_collision_group",open_bound,1);

    size_t size=gml_vm_state_size(&vm),written=0,used=0;
    void *snapshot=malloc(size);
    assert(snapshot && gml_vm_state_save(&vm,snapshot,size,&written));
    step(&vm,60);
    double y=get(&vm,2,"phy_position_y");
    assert(y>17 && y<20 && fabs(get(&vm,2,"phy_linear_velocity_y"))<.001);
    CALL(&vm,"physics_remove_fixture",vm.inst[0].id,open_bound);
    assert(physics_query(&vm,cached,"physics_raycast",args,6).t==V_UNDEF);
    assert(gml_vm_state_load(&vm,snapshot,written,&used) && used==written);
    step(&vm,60);
    assert(get(&vm,2,"phy_position_y")==y);
    hits=physics_query(&vm,cached,"physics_raycast",args,6);
    assert(gml_val_array_length(hits)==1);
    gml_values_release_owned(&hits,1);
    free(snapshot);
    CALL(&vm,"physics_remove_fixture",vm.inst[1].id,loop_bound);
    double dense=call(&vm,"physics_fixture_create",NULL,0).d;
    select_chain_shape(&vm,cached,dense,0);
    CALL(&vm,"physics_fixture_add_point",dense,0,0);
    CALL(&vm,"physics_fixture_add_point",dense,16,0);
    CALL(&vm,"physics_fixture_set_density",dense,1);
    assert(CALL(&vm,"physics_fixture_bind",dense,vm.inst[1].id).d>0);
    assert(get(&vm,1,"phy_mass")==1 && get(&vm,1,"phy_dynamic")==1);
    step(&vm,3);
    assert(get(&vm,1,"phy_position_y")>30);
    gml_vm_free(&vm);
  }
  puts("chain fixtures: open and closed geometry, long chains, contacts, ray hits and restore passed");
}

static void chain_state_budget(void){
  GmlVM vm;GmlWin win;setup(&vm,&win);
  double chain=call(&vm,"physics_fixture_create",NULL,0).d;
  select_chain_shape(&vm,0,chain,0);
  for(int i=0;i<=GML_PHYS_CHAIN_POINT_LIMIT;i++)
    CALL(&vm,"physics_fixture_add_point",chain,i,0);
  assert(vm.builtins->phys_fixture[0].points==GML_PHYS_CHAIN_POINT_LIMIT);
  assert(call(&vm,"physics_fixture_create",NULL,0).d==0);
  size_t chain_size=gml_vm_state_size(&vm),written=0,used=0;
  void *snapshot=malloc(chain_size);
  assert(snapshot && gml_vm_state_save(&vm,snapshot,chain_size,&written));
  gml_builtin_physics_room_reset(&vm);
  for(int i=0;i<GML_PHYS_FIXTURE_MAX;i++)
    assert(call(&vm,"physics_fixture_create",NULL,0).d>0);
  assert(chain_size<gml_vm_state_size(&vm));
  assert(gml_vm_state_load(&vm,snapshot,written,&used) && used==written);
  assert(vm.builtins->phys_fixture[0].points==GML_PHYS_CHAIN_POINT_LIMIT);
  assert(vm.builtins->phys_fixture[0].chain[GML_PHYS_CHAIN_POINT_LIMIT-1].x==GML_PHYS_CHAIN_POINT_LIMIT-1);
  free(snapshot);
  gml_vm_free(&vm);
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
                                     1.0/60,1,10,allow_contact,&allow,contacts,2,NULL,NULL);
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

typedef struct {
  GmlPhysicsPoint lines[16][2];
  int line_count,circle_count;
  double x,y,radius;
} OutlineCapture;
static void capture_outline_line(void *context,double x1,double y1,double x2,double y2){
  OutlineCapture *capture=context;
  assert(capture->line_count<16);
  GmlPhysicsPoint *line=capture->lines[capture->line_count++];
  line[0]=(GmlPhysicsPoint){x1,y1}; line[1]=(GmlPhysicsPoint){x2,y2};
}
static void capture_outline_circle(void *context,double x,double y,double radius){
  OutlineCapture *capture=context;
  capture->circle_count++; capture->x=x; capture->y=y; capture->radius=radius;
}
static void outline_geometry(void){
  GmlPhysicsBody pose={.x=12,.y=14,.angle=90};
  GmlPhysicsFixture fixture={.shape=2,.w=3,.h=2};
  OutlineCapture capture={0};
  gml_physics_fixture_outline(&fixture,&pose,1,capture_outline_line,capture_outline_circle,&capture);
  const double corners[][2]={{14,11},{14,17},{10,17},{10,11}};
  assert(capture.line_count==4 && capture.circle_count==0);
  for(int i=0;i<4;i++) for(int point=0;point<2;point++){
    const GmlPhysicsPoint *actual=&capture.lines[i][point];
    const double *expected=corners[(i+point)%4];
    assert(fabs(actual->x-expected[0])<0.00001 && fabs(actual->y-expected[1])<0.00001);
  }
  capture=(OutlineCapture){0}; fixture.shape=1; fixture.radius=3;
  gml_physics_fixture_outline(&fixture,&pose,1,capture_outline_line,capture_outline_circle,&capture);
  assert(capture.line_count==0 && capture.circle_count==1);
  assert(capture.x==12 && capture.y==14 && capture.radius==3);
  capture=(OutlineCapture){0}; fixture.shape=4; fixture.x1=-3; fixture.x2=3;
  gml_physics_fixture_outline(&fixture,&pose,1,capture_outline_line,capture_outline_circle,&capture);
  assert(capture.line_count==1 && fabs(capture.lines[0][0].y-11)<0.00001);
  assert(fabs(capture.lines[0][1].y-17)<0.00001);
  GmlPhysicsPoint chain[]={{0,0},{3,0},{0,3}};
  fixture.shape=GML_PHYS_FIXTURE_CHAIN; fixture.points=3; fixture.chain=chain;
  capture=(OutlineCapture){0};
  gml_physics_fixture_outline(&fixture,&pose,1,capture_outline_line,capture_outline_circle,&capture);
  assert(capture.line_count==2);
  fixture.shape=GML_PHYS_FIXTURE_LOOP; capture=(OutlineCapture){0};
  gml_physics_fixture_outline(&fixture,&pose,1,capture_outline_line,capture_outline_circle,&capture);
  assert(capture.line_count==3 && fabs(capture.lines[2][1].x-12)<0.00001);
  fixture.shape=3; fixture.chain=NULL;
  fixture.px[1]=3; fixture.py[2]=3; capture=(OutlineCapture){0};
  gml_physics_fixture_outline(&fixture,&pose,1,capture_outline_line,capture_outline_circle,&capture);
  assert(capture.line_count==3);
  capture=(OutlineCapture){0}; pose.x=NAN;
  gml_physics_fixture_outline(&fixture,&pose,1,capture_outline_line,capture_outline_circle,&capture);
  assert(capture.line_count==0 && capture.circle_count==0);
}
static void instance_debug_drawing(void){
  for(int cached=0;cached<2;cached++) for(int active3d=0;active3d<2;active3d++) for(int shape=0;shape<3;shape++){
    GmlVM vm; GmlWin win; setup(&vm,&win); win.bytecode=17;
    CALL(&vm,"physics_world_create",1);
    uint32_t pixels[32*32]={0},expected[32*32]={0};
    GmlRender render={.fb=pixels,.base_fb=pixels,.fbw=32,.fbh=32,.base_fbw=32,.base_fbh=32,
      .color=0x00ff00,.alpha=0.5,.alphablend=1,.color_write_mask=15,.active_shader=-1,.target_id=-1};
    vm.render=&render; vm.cur_self=&vm.inst[0];
    if(active3d){
      call(&vm,"d3d_start",NULL,0);
      CALL(&vm,"d3d_set_projection_ortho",0,0,32,32,0);
      CALL(&vm,"d3d_set_hidden",0);
    }
    double fixture=call(&vm,"physics_fixture_create",NULL,0).d;
    vm.inst[0].x=10; vm.inst[0].y=11;
    if(shape==0) CALL(&vm,"physics_fixture_set_box_shape",fixture,3,2);
    else if(shape==1) CALL(&vm,"physics_fixture_set_circle_shape",fixture,3);
    else CALL(&vm,"physics_fixture_set_edge_shape",fixture,-3,0,3,0);
    double bound=CALL(&vm,"physics_fixture_bind",fixture,vm.inst[0].id).d;
    assert(bound>0);
    bind_box(&vm,1,24,24,2,2,1);
    GmlVal value=physics_query(&vm,cached,"physics_draw_debug",NULL,0);
    assert(value.t==V_UNDEF);
    gml_render_flush_rotated_batch(&render);
    render.fb=render.base_fb=expected;
    if(shape==0){
      CALL(&vm,"draw_line",7,9,13,9); CALL(&vm,"draw_line",13,9,13,13);
      CALL(&vm,"draw_line",13,13,7,13); CALL(&vm,"draw_line",7,13,7,9);
    } else if(shape==1) CALL(&vm,"draw_circle",10,11,3,1);
    else CALL(&vm,"draw_line",7,11,13,11);
    gml_render_flush_rotated_batch(&render);
    assert(!memcmp(pixels,expected,sizeof pixels));
    int count=0; for(int i=0;i<32*32;i++) count+=pixels[i]!=0;
    assert(count>0 && render.color==0x00ff00 && render.alpha==0.5);
    render.fb=render.base_fb=pixels;
    memset(pixels,0,sizeof pixels);
    vm.inst[0].marked=1;
    physics_query(&vm,cached,"physics_draw_debug",NULL,0);
    vm.inst[0].marked=0; vm.cur_self=NULL;
    physics_query(&vm,cached,"physics_draw_debug",NULL,0);
    vm.cur_self=&vm.inst[0];
    CALL(&vm,"physics_remove_fixture",vm.inst[0].id,bound);
    physics_query(&vm,cached,"physics_draw_debug",NULL,0);
    for(int i=0;i<32*32;i++) assert(pixels[i]==0);
    vm.render=NULL; gml_vm_free(&vm); free(render.rotated_batch);
  }
  puts("physics debug: named/cached 2D/3D outlines, color, opacity, ownership and deletion passed");
}

static int particle_configuration(void){
  const char *const getters[]={
    "physics_particle_get_radius","physics_particle_get_density",
    "physics_particle_get_damping","physics_particle_get_gravity_scale",
    "physics_particle_get_max_count"
  };
  const char *const setters[]={
    "physics_particle_set_radius","physics_particle_set_density",
    "physics_particle_set_damping","physics_particle_set_gravity_scale",
    "physics_particle_set_max_count"
  };
  const double values[]={7.25,0.375,2.5,-0.5,512};
  GmlVM vm,peer; GmlWin win,peer_win; setup(&vm,&win); setup(&peer,&peer_win);
  double peer_values[5];
  for(int i=0;i<5;i++) peer_values[i]=call(&peer,getters[i],NULL,0).d;
  for(int i=0;i<5;i++){
    GmlVal result=call(&vm,setters[i],values+i,1);
    GmlVal actual=call(&vm,getters[i],NULL,0);
    if(result.t!=V_UNDEF || actual.t!=V_REAL || fabs(actual.d-values[i])>1e-12){
      fprintf(stderr,"particle configuration: %s did not retain the authored value\n",setters[i]);
      gml_vm_free(&vm); gml_vm_free(&peer); return 0;
    }
    assert(call(&peer,getters[i],NULL,0).d==peer_values[i]);
    call(&vm,setters[i],NULL,0);
    double invalid[]={NAN,INFINITY,-INFINITY};
    for(int j=0;j<3;j++) call(&vm,setters[i],invalid+j,1);
    assert(fabs(call(&vm,getters[i],NULL,0).d-values[i])<1e-12);
  }
  for(int i=0;i<5;i++){
    int setter=gml_builtin_fast_id(&vm,setters[i]),getter=gml_builtin_fast_id(&vm,getters[i]);
    assert(setter>0 && getter>0);
    GmlVal argument=vreal(values[i]*2);
    assert(gml_builtin_call_fast_id(&vm,setter,setters[i],&argument,1).t==V_UNDEF);
    assert(fabs(gml_builtin_call_fast_id(&vm,getter,getters[i],NULL,0).d-values[i]*2)<1e-12);
    call(&vm,setters[i],values+i,1);
  }
  CALL(&peer,"physics_world_create",0.25);
  CALL(&peer,setters[0],values[0]);
  assert(call(&peer,getters[0],NULL,0).d==values[0]);
  assert(fabs(vm.builtins->phys_particle_radius_m-values[0]*0.1)<1e-14);
  assert(peer.builtins->phys_particle_radius_m==values[0]*0.25);
  for(int i=0;i<2;i++){
    CALL(&vm,setters[i],-1);
    assert(fabs(call(&vm,getters[i],NULL,0).d-values[i])<1e-12);
  }
  CALL(&vm,setters[0],0); CALL(&vm,setters[1],0);
  assert(fabs(call(&vm,getters[0],NULL,0).d-values[0])<1e-12);
  assert(call(&vm,getters[1],NULL,0).d==values[1]);
  CALL(&vm,setters[2],-1); assert(call(&vm,getters[2],NULL,0).d==0);
  CALL(&vm,setters[2],0); assert(call(&vm,getters[2],NULL,0).d==0);
  CALL(&vm,setters[2],values[2]);
  const double ignored_counts[]={-1,0.5,1,127,127.9,2147483648.0};
  for(unsigned i=0;i<sizeof ignored_counts/sizeof *ignored_counts;i++){
    call(&vm,setters[4],ignored_counts+i,1);
    assert(call(&vm,getters[4],NULL,0).d==512);
  }
  CALL(&vm,setters[4],128.75); assert(call(&vm,getters[4],NULL,0).d==128);
  CALL(&vm,setters[4],0); assert(call(&vm,getters[4],NULL,0).d==0);
  CALL(&vm,setters[4],values[4]);
  size_t size=gml_vm_state_size(&vm),written=0,used=0,repeated=0;
  unsigned char *snapshot=malloc(size),*again=malloc(size),*bad=malloc(size);
  assert(snapshot && again && bad && gml_vm_state_save(&vm,snapshot,size,&written));
  for(int i=0;i<5;i++) CALL(&vm,setters[i],1024);
  assert(gml_vm_state_load(&vm,snapshot,written,&used) && used==written);
  for(int i=0;i<5;i++) assert(fabs(call(&vm,getters[i],NULL,0).d-values[i])<1e-12);
  assert(gml_vm_state_save(&vm,again,size,&repeated));
  assert(repeated==written && !memcmp(snapshot,again,written));
  memcpy(bad,snapshot,written); bad[4]^=1;
  assert(!gml_vm_state_load(&vm,bad,written,&used));
  for(int i=0;i<5;i++) assert(fabs(call(&vm,getters[i],NULL,0).d-values[i])<1e-12);
  CALL(&peer,"physics_world_create",0.1);
  gml_builtin_physics_room_reset(&vm);
  for(int i=0;i<5;i++) assert(call(&vm,getters[i],NULL,0).d==peer_values[i]);
  vm.room_index=91;
  for(int i=0;i<5;i++){
    assert(call(&vm,getters[i],NULL,0).d==-1);
    assert(CALL(&vm,setters[i],2048).t==V_UNDEF);
  }
  vm.room_index=-1;
  for(int i=0;i<5;i++) assert(call(&vm,getters[i],NULL,0).d==peer_values[i]);
  free(snapshot); free(again); free(bad); gml_vm_free(&vm); gml_vm_free(&peer);
  puts("physics: particle configuration, isolation, units, reset and exact state passed");
  return 1;
}

static GmlPhysicsParticleConfig particle_config_for(GmlVM *vm){
  GmlBuiltinState *s=vm->builtins;
  GmlPhysicsParticleConfig config={s->phys_particle_radius_m,s->phys_particle_density,
    s->phys_particle_damping,s->phys_particle_gravity_scale,s->phys_particle_max_count};
  return config;
}
static int kernel_particle(GmlVM *vm,double x,double y,double vx,double vy,
                           uint32_t flags,int category,uint32_t colour){
  double scale=gml_physics_world_scale(vm);GmlPhysicsParticleConfig config=particle_config_for(vm);
  GmlPhysicsParticle particle={x*scale,y*scale,vx*scale,vy*scale,flags,colour,category,0,0,0,0};
  return gml_physics_particles_add(&vm->builtins->phys_particles,&config,particle);
}
static void particle_kernel_transport(void){
  GmlVM vm,peer;GmlWin win,peer_win;setup(&vm,&win);setup(&peer,&peer_win);
  CALL(&vm,"physics_particle_set_radius",1);CALL(&peer,"physics_particle_set_radius",1);
  CALL(&vm,"physics_particle_set_damping",0);CALL(&peer,"physics_particle_set_damping",0);
  assert(kernel_particle(&vm,0,0,60,30,0,1,0xff0000ff)==0);
  assert(kernel_particle(&peer,0,0,60,30,0,1,0xff0000ff)==0);
  bind_box(&peer,0,1000,1000,5,5,0);
  step(&vm,60);step(&peer,60);
  GmlPhysicsParticle *a=&vm.builtins->phys_particles.points[0];
  GmlPhysicsParticle *b=&peer.builtins->phys_particles.points[0];
  double elapsed=60/fmax(1,gml_room_speed(&vm));
  assert(fabs(a->x-6*elapsed)<1e-6 && fabs(a->y-3*elapsed)<1e-6);
  assert(a->x==b->x && a->y==b->y && a->vx==b->vx && a->vy==b->vy);
  CALL(&vm,"physics_particle_set_damping",4);step(&vm,1);
  assert(a->vx==6 && a->vy==3);
  CALL(&vm,"physics_world_gravity",0,10);step(&vm,60);
  assert(fabs(a->vy-(3+10*elapsed))<1e-5);
  assert(kernel_particle(&vm,500,500,10,20,GML_PHYS_PARTICLE_WALL,2,0xffffffff)==1);
  assert(kernel_particle(&vm,600,600,0,0,GML_PHYS_PARTICLE_ZOMBIE,3,0xffffffff)==2);
  CALL(&vm,"physics_pause_enable",1);step(&vm,1);assert(vm.builtins->phys_particles.count==3);
  CALL(&vm,"physics_pause_enable",0);step(&vm,1);assert(vm.builtins->phys_particles.count==2);
  b=&vm.builtins->phys_particles.points[1];assert(b->x==50 && b->y==50 && b->vx==0 && b->vy==0);
  assert(gml_physics_particles_flags(&vm.builtins->phys_particles,0,GML_PHYS_PARTICLE_ZOMBIE));
  step(&vm,1);assert(vm.builtins->phys_particles.count==1);
  assert(vm.builtins->phys_particles.points[0].category==2);
  gml_builtin_physics_room_reset(&vm);assert(vm.builtins->phys_particles.count==0);
  assert(!vm.builtins->phys_particles.points && !vm.builtins->phys_particles.bonds);
  gml_vm_free(&vm);gml_vm_free(&peer);
  puts("particle kernel: empty-body world, free motion, distant-fixture invariance, gravity, walls, pause and compaction passed");
}
static void particle_kernel_contacts(void){
  GmlVM vm;GmlWin win;setup(&vm,&win);
  CALL(&vm,"physics_particle_set_radius",2);CALL(&vm,"physics_particle_set_damping",0);
  CALL(&vm,"physics_world_gravity",0,10);bind_box(&vm,0,0,50,50,5,0);
  assert(kernel_particle(&vm,0,0,0,100,0,1,0xffffffff)==0);
  step(&vm,120);GmlPhysicsParticle *p=&vm.builtins->phys_particles.points[0];
  assert(p->y<=4.302 && p->y>=4.25 && fabs(p->vy)<0.01);
  gml_vm_free(&vm);setup(&vm,&win);
  CALL(&vm,"physics_particle_set_radius",2);CALL(&vm,"physics_particle_set_damping",0);
  bind_box(&vm,0,100,0,5,5,2);
  assert(kernel_particle(&vm,80,0,100,0,0,1,0xffffffff)==0);
  step(&vm,12);p=&vm.builtins->phys_particles.points[0];
  double rigid_velocity=get(&vm,0,"phy_linear_velocity_x")*0.1;
  double rigid_mass=get(&vm,0,"phy_mass");
  assert(rigid_velocity>0 && p->vx<10);
  assert(fabs(rigid_mass*rigid_velocity+0.09*p->vx-0.9)<1e-4);
  gml_vm_free(&vm);setup(&vm,&win);
  CALL(&vm,"physics_particle_set_radius",2);CALL(&vm,"physics_particle_set_damping",0);
  bind_box(&vm,0,0,50,50,5,0);
  assert(kernel_particle(&vm,0,0,40,100,0,1,0xffffffff)==0);
  step(&vm,120);p=&vm.builtins->phys_particles.points[0];
  assert(fabs(p->vx)<0.01);
  gml_vm_free(&vm);setup(&vm,&win);
  CALL(&vm,"physics_particle_set_radius",2);CALL(&vm,"physics_particle_set_damping",0);
  bind_box(&vm,0,90,0,5,5,2);
  assert(gml_physics_variable_set(&vm,&vm.inst[0],"phy_linear_velocity_x",vreal(100)));
  assert(kernel_particle(&vm,100,0,0,0,GML_PHYS_PARTICLE_WALL,1,0xffffffff)==0);
  step(&vm,12);p=&vm.builtins->phys_particles.points[0];
  assert(p->x==10 && p->vx==0 && get(&vm,0,"phy_position_x")<=93.02);
  assert(fabs(get(&vm,0,"phy_linear_velocity_x"))<0.01);
  gml_vm_free(&vm);setup(&vm,&win);
  CALL(&vm,"physics_particle_set_radius",2);CALL(&vm,"physics_particle_set_damping",0);
  double sensor=bind_box(&vm,0,0,50,50,5,0);
  CALL(&vm,"physics_fixture_set_sensor",sensor,1);
  assert(kernel_particle(&vm,0,0,0,100,0,1,0xffffffff)==0);
  step(&vm,60);assert(vm.builtins->phys_particles.points[0].y>10);
  gml_vm_free(&vm);setup(&vm,&win);
  CALL(&vm,"physics_particle_set_radius",2);CALL(&vm,"physics_particle_set_damping",0);
  vm.inst[0].x=100;
  double fixture=call(&vm,"physics_fixture_create",NULL,0).d;
  CALL(&vm,"physics_fixture_set_polygon_shape",fixture);
  CALL(&vm,"physics_fixture_add_point",fixture,10,10);
  CALL(&vm,"physics_fixture_add_point",fixture,20,10);
  CALL(&vm,"physics_fixture_add_point",fixture,20,20);
  CALL(&vm,"physics_fixture_add_point",fixture,10,20);
  CALL(&vm,"physics_fixture_set_density",fixture,2);
  CALL(&vm,"physics_fixture_set_friction",fixture,0);
  CALL(&vm,"physics_fixture_bind",fixture,vm.inst[0].id);
  CALL(&vm,"physics_fixture_delete",fixture);
  assert(kernel_particle(&vm,107.9,18,100,0,0,1,0xffffffff)==0);
  GmlPhysicsBody body=gml_physics_body_pose(&vm.inst[0]);
  body.id=vm.inst[0].id;
  gml_physics_solver_mass(&body,vm.builtins->phys_fixture,GML_PHYS_FIXTURE_MAX,0.1);
  GmlPhysicsParticleConfig config=particle_config_for(&vm);GmlPhysicsContact contacts[1];
  gml_physics_solver_step(&body,1,vm.builtins->phys_fixture,GML_PHYS_FIXTURE_MAX,
      NULL,0,0.1,0,0,0.01,1,1,NULL,NULL,contacts,1,&vm.builtins->phys_particles,&config);
  p=&vm.builtins->phys_particles.points[0];
  double omega=body.omega*M_PI/180;
  double centre_y=(body.y+body.center_y)*0.1;
  double contact_vx=body.vx*0.1-omega*(p->y-centre_y);
  if(fabs(p->vx-contact_vx)>=0.001)
    fprintf(stderr,"offset impulse: particle=(%.17g,%.17g) velocity=(%.17g,%.17g) body=(%.17g,%.17g) centre=(%.17g,%.17g) mass=%.17g inertia=%.17g velocity=(%.17g,%.17g) omega=%.17g contact_vx=%.17g\n",
      p->x,p->y,p->vx,p->vy,body.x,body.y,body.center_x,body.center_y,
      body.mass,body.inertia,body.vx,body.vy,omega,contact_vx);
  assert(fabs(p->vx-contact_vx)<0.001);
  assert(fabs(omega)>0.001);
  gml_vm_free(&vm);
  puts("particle kernel: rigid floor, two-way momentum, friction, fixed walls, sensors and offset COM impulses passed");
}
static void particle_kernel_flags(void){
  GmlPhysicsParticleConfig config={0.1,1,0,1,128};
  GmlPhysicsContact contacts[1];
  const uint32_t flags[]={GML_PHYS_PARTICLE_VISCOUS,GML_PHYS_PARTICLE_POWDER,
    GML_PHYS_PARTICLE_TENSILE,GML_PHYS_PARTICLE_SPRING,GML_PHYS_PARTICLE_ELASTIC,
    GML_PHYS_PARTICLE_COLOUR};
  for(unsigned mode=0;mode<sizeof flags/sizeof *flags;mode++){
    GmlPhysicsParticleState state={0};
    GmlPhysicsParticle a={0,0,mode==0?1:0,0,flags[mode],0xff0000ff,1,0,0,0,0};
    GmlPhysicsParticle b={0.15,0,mode==0?-1:0,0,mode==5?0:flags[mode],0xffff0000,1,0,0,0,0};
    assert(gml_physics_particles_add(&state,&config,a)==0);
    assert(gml_physics_particles_add(&state,&config,b)==1);
    if(mode==3 || mode==4){
      assert(state.bond_count==1 && state.bonds[0].first==0 && state.bonds[0].second==1);
      assert(fabs(state.bonds[0].length-0.15)<1e-12);state.points[1].x=0.3;
    }
    gml_physics_solver_step(NULL,0,NULL,0,NULL,0,1,0,0,1.0/60,1,1,NULL,NULL,contacts,1,&state,&config);
    assert(fabs(state.points[0].vx+state.points[1].vx)<1e-12);
    if(mode==0) assert(fabs(state.points[0].vx-state.points[1].vx)<2);
    if(mode==1) assert(state.points[0].vx<0 && state.points[1].vx>0);
    if(mode==2 || mode==3 || mode==4) assert(state.points[0].vx>0 && state.points[1].vx<0);
    if(mode==5){
      assert(state.points[0].colour==a.colour && state.points[1].colour==b.colour);
      assert(gml_physics_particles_flags(&state,1,GML_PHYS_PARTICLE_COLOUR));
      gml_physics_solver_step(NULL,0,NULL,0,NULL,0,1,0,0,1.0/60,1,10,NULL,NULL,contacts,1,&state,&config);
      assert(state.points[0].colour!=a.colour && state.points[1].colour!=b.colour);
    }
    gml_physics_particles_clear(&state);
  }
  GmlPhysicsParticleState state={0};
  for(int i=0;i<128;i++){
    GmlPhysicsParticle point={i,0,0,0,0,0xffffffff,1,0,0,0,0};
    assert(gml_physics_particles_add(&state,&config,point)==i);
  }
  GmlPhysicsParticle extra={0};assert(gml_physics_particles_add(&state,&config,extra)==-1);
  gml_physics_particles_clear(&state);
  for(int i=0;i<4;i++){
    GmlPhysicsParticle point={0,0,0,0,0,0xffffffff,1,0,0,0,0};
    assert(gml_physics_particles_add(&state,&config,point)==i);
  }
  gml_physics_solver_step(NULL,0,NULL,0,NULL,0,1,0,0,1.0/60,1,1,NULL,NULL,contacts,1,&state,&config);
  assert(state.points[0].x<state.points[3].x);
  for(int i=0;i<4;i++) assert(isfinite(state.points[i].x) && isfinite(state.points[i].vx));
  gml_physics_particles_clear(&state);
  puts("particle kernel: viscous, powder, tensile, spring, elastic, paired colour mixing, caps and fluid pressure passed");
}

static GmlVal particle_api(GmlVM *vm,int cached,const char *name,const double *values,int count){
  GmlVal args[12];assert(count<=12);
  for(int i=0;i<count;i++) args[i]=vreal(values[i]);
  return physics_query(vm,cached,name,args,count);
}
#define PARTICLE(vm,cached,name,...) particle_api(vm,cached,name,(double[]){__VA_ARGS__},(int)(sizeof((double[]){__VA_ARGS__})/sizeof(double)))

static void particle_api_lifetime(void){
  for(int cached=0;cached<2;cached++){
    GmlVM vm;GmlWin win;setup(&vm,&win);
#define EXPECT_PARTICLE_CONSTANT(value,label) do { GmlVal constant=gml_vm_identifier_get(&vm,label);assert(constant.t==V_REAL && constant.d==value); } while(0);
    GML_PHYS_PARTICLE_CONSTANTS(EXPECT_PARTICLE_CONSTANT)
#undef EXPECT_PARTICLE_CONSTANT
    CALL(&vm,"physics_particle_set_radius",1);
    assert(particle_api(&vm,cached,"physics_particle_count",NULL,0).d==0);
    assert(PARTICLE(&vm,cached,"physics_particle_create",0,10,20,30,-40,0x112233,0.5,7).d==0);
    GmlPhysicsParticle *p=&vm.builtins->phys_particles.points[0];
    assert(p->x==1 && p->y==2 && p->vx==3 && p->vy==-4);
    assert(p->colour==0x7f112233u && p->category==7);
    assert(PARTICLE(&vm,cached,"physics_particle_create",0,0,0,0,0,0xffffff,1,0).d==-1);
    assert(PARTICLE(&vm,cached,"physics_particle_create",0,NAN,0,0,0,0xffffff,1,1).d==-1);
    assert(PARTICLE(&vm,cached,"physics_particle_create",NAN,0,0,0,0,0xffffff,1,1).d==-1);
    assert(PARTICLE(&vm,cached,"physics_particle_create",0,0,0,0,0,0xffffff,INFINITY,1).d==-1);
    assert(PARTICLE(&vm,cached,"physics_particle_create",0,30,20,0,0,0xffffff,1,8).d==1);
    assert(PARTICLE(&vm,cached,"physics_particle_create",0,50,20,0,0,0xffffff,1,7).d==2);
    PARTICLE(&vm,cached,"physics_particle_set_category_flags",7,GML_PHYS_PARTICLE_VISCOUS);
    assert(p->flags==GML_PHYS_PARTICLE_VISCOUS);
    assert(vm.builtins->phys_particles.points[1].flags==0);
    assert(vm.builtins->phys_particles.points[2].flags==GML_PHYS_PARTICLE_VISCOUS);
    PARTICLE(&vm,cached,"physics_particle_set_flags",0,GML_PHYS_PARTICLE_WALL);
    assert(p->vx==0 && p->vy==0 && p->flags==GML_PHYS_PARTICLE_WALL);
    PARTICLE(&vm,cached,"physics_particle_set_flags",-1,GML_PHYS_PARTICLE_ZOMBIE);
    assert(particle_api(&vm,cached,"physics_particle_count",NULL,0).d==3);
    PARTICLE(&vm,cached,"physics_particle_delete",1);
    assert(vm.builtins->phys_particles.points[1].flags==GML_PHYS_PARTICLE_ZOMBIE);
    assert(particle_api(&vm,cached,"physics_particle_count",NULL,0).d==3);
    step(&vm,1);assert(particle_api(&vm,cached,"physics_particle_count",NULL,0).d==2);
    assert(vm.builtins->phys_particles.points[1].category==7);
    PARTICLE(&vm,cached,"physics_particle_set_category_flags",0,GML_PHYS_PARTICLE_WALL);
    assert(vm.builtins->phys_particles.points[1].flags==GML_PHYS_PARTICLE_WALL);
    vm.room_index++;
    assert(particle_api(&vm,cached,"physics_particle_count",NULL,0).d==-1);
    assert(PARTICLE(&vm,cached,"physics_particle_create",0,0,0,0,0,0xffffff,1,1).d==-1);
    gml_vm_free(&vm);
    for(int shape=0;shape<3;shape++){
      setup(&vm,&win);CALL(&vm,"physics_particle_set_radius",1);
      for(int i=0;i<3;i++) assert(PARTICLE(&vm,cached,"physics_particle_create",0,10+10*i,10,0,0,0xffffff,1,i+1).d==i);
      if(shape==0) PARTICLE(&vm,cached,"physics_particle_delete_region_circle",10,10,3);
      else if(shape==1) PARTICLE(&vm,cached,"physics_particle_delete_region_box",10,10,3,4);
      else{
        double list=call(&vm,"ds_list_create",NULL,0).d;
        CALL(&vm,"ds_list_add",list,7,6,13,6,13,14,7,14);
        PARTICLE(&vm,cached,"physics_particle_delete_region_poly",list);
        CALL(&vm,"ds_list_destroy",list);
      }
      assert(vm.builtins->phys_particles.points[0].flags==GML_PHYS_PARTICLE_ZOMBIE);
      assert(vm.builtins->phys_particles.points[1].flags==0 && vm.builtins->phys_particles.points[2].flags==0);
      step(&vm,1);assert(vm.builtins->phys_particles.count==2);
      assert(vm.builtins->phys_particles.points[0].category==2);
      gml_vm_free(&vm);
    }
  }
  for(int array=0;array<2;array++){
    GmlVM vm;GmlWin win;setup(&vm,&win);
    const double polygon[]={0,0,20,0,20,5,5,5,5,20,0,20};
    for(int i=0;i<3;i++) assert(CALL(&vm,"physics_particle_create",0,i==0?2:i==1?12:30,12,0,0,0xffffff,1,i+1).d==i);
    GmlVal points=array?gml_arr_new(12,vreal(0)):call(&vm,"ds_list_create",NULL,0);
    for(int i=0;i<12;i++){
      if(array) gml_arr_set(points,i,vreal(polygon[i]));
      else CALL(&vm,"ds_list_add",points.d,polygon[i]);
    }
    physics_query(&vm,array,"physics_particle_delete_region_poly",&points,1);
    assert(vm.builtins->phys_particles.points[0].flags==GML_PHYS_PARTICLE_ZOMBIE);
    assert(vm.builtins->phys_particles.points[1].flags==0 && vm.builtins->phys_particles.points[2].flags==0);
    if(array) gml_values_release_owned(&points,1);
    else CALL(&vm,"ds_list_destroy",points.d);
    gml_vm_free(&vm);
  }
  puts("particle API: named/cached creation, units, invalid inputs, categories, walls, deferred deletion and three regions passed");
}

static void particle_api_data(void){
  for(int cached=0;cached<2;cached++){
    GmlVM vm;GmlWin win;setup(&vm,&win);
    assert(PARTICLE(&vm,cached,"physics_particle_create",0,12.5,22.5,32.5,-42.5,0x112233,0.5,7).d==0);
    assert(PARTICLE(&vm,cached,"physics_particle_create",GML_PHYS_PARTICLE_VISCOUS,50,60,70,80,0xabcdef,1,-8).d==1);
    double id=CALL(&vm,"buffer_create",4,1,1).d;
    CALL(&vm,"buffer_poke",id,0,1,0x5a);
    CALL(&vm,"buffer_seek",id,0,4);
    PARTICLE(&vm,cached,"physics_particle_get_data",id,31);
    assert(CALL(&vm,"buffer_tell",id).d==4);
    assert(CALL(&vm,"buffer_peek",id,0,1).d==0x5a);
    assert(CALL(&vm,"buffer_get_size",id).d>=60);
    assert(CALL(&vm,"buffer_peek",id,4,5).d==0);
    assert(CALL(&vm,"buffer_peek",id,8,8).d==12.5);
    assert(CALL(&vm,"buffer_peek",id,12,8).d==22.5);
    assert(CALL(&vm,"buffer_peek",id,16,8).d==32.5);
    assert(CALL(&vm,"buffer_peek",id,20,8).d==-42.5);
    assert(CALL(&vm,"buffer_peek",id,24,5).d==0x7f332211u);
    assert(CALL(&vm,"buffer_peek",id,28,5).d==7);
    assert(CALL(&vm,"buffer_peek",id,32,5).d==GML_PHYS_PARTICLE_VISCOUS);
    assert(CALL(&vm,"buffer_peek",id,56,6).d==-8);
    PARTICLE(&vm,cached,"physics_particle_get_data_particle",1,id,18);
    assert(CALL(&vm,"buffer_peek",id,4,8).d==50);
    assert(CALL(&vm,"buffer_peek",id,8,8).d==60);
    assert(CALL(&vm,"buffer_peek",id,12,6).d==-8);
    assert(CALL(&vm,"buffer_tell",id).d==4);
    assert(CALL(&vm,"buffer_peek",id,0,1).d==0x5a);
    PARTICLE(&vm,cached,"physics_particle_get_data_particle",-1,id,31);
    assert(CALL(&vm,"buffer_peek",id,4,8).d==50);
    PARTICLE(&vm,cached,"physics_particle_get_data",-1,31);
    gml_vm_free(&vm);
  }
  puts("particle API: named/cached packed records, selected fields, growth, units, colour/alpha, signed categories and cursor preservation passed");
}

static void particle_api_drawing(void){
  for(int cached=0;cached<2;cached++) for(int extended=0;extended<2;extended++){
    GmlVM vm;GmlWin win;setup(&vm,&win);win.bytecode=17;
    GmlRender render;assert(!gml_render_init(&render,&win));vm.render=&render;
    uint8_t *rgba=malloc(16);assert(rgba);memset(rgba,255,16);
    int sprite=gml_sprite_append_from_rgba(&render,rgba,2,2,0,0,NULL);assert(sprite>=0);
    uint32_t actual[32*32]={0},expected[32*32]={0};
    assert(PARTICLE(&vm,cached,"physics_particle_create",GML_PHYS_PARTICLE_VISCOUS,5,6,0,0,0xff,0.5,7).d==0);
    assert(PARTICLE(&vm,cached,"physics_particle_create",0,15,6,0,0,0xff00,1,8).d==1);
    assert(PARTICLE(&vm,cached,"physics_particle_create",GML_PHYS_PARTICLE_VISCOUS|GML_PHYS_PARTICLE_ZOMBIE,25,6,0,0,0xffffff,1,7).d==2);
    gml_render_begin(&render,actual,32,32,0,0);
    render.color=0x123456;render.alpha=0.25;
    if(extended) PARTICLE(&vm,cached,"physics_particle_draw_ext",GML_PHYS_PARTICLE_VISCOUS,7,sprite,0,2,3,90,0x445566,0.75);
    else PARTICLE(&vm,cached,"physics_particle_draw",GML_PHYS_PARTICLE_VISCOUS,7,sprite,0);
    gml_render_prepare_draw(&render);
    assert(render.color==0x123456 && render.alpha==0.25);
    int drawn=0;for(int i=0;i<32*32;i++) drawn+=actual[i]!=0;assert(drawn>0);
    gml_render_begin(&render,expected,32,32,0,0);
    CALL(&vm,"draw_sprite_ext",sprite,0,5,6,extended?2:1,extended?3:1,extended?90:0,extended?0x445566:0xff,extended?0.75:127.0/255);
    CALL(&vm,"draw_sprite_ext",sprite,0,25,6,extended?2:1,extended?3:1,extended?90:0,extended?0x445566:0xffffff,extended?0.75:1);
    gml_render_prepare_draw(&render);assert(!memcmp(actual,expected,sizeof actual));
    memset(actual,0,sizeof actual);memset(expected,0,sizeof expected);
    gml_render_begin(&render,actual,32,32,0,0);
    PARTICLE(&vm,cached,"physics_particle_draw",0,0,sprite,0);
    gml_render_prepare_draw(&render);
    gml_render_begin(&render,expected,32,32,0,0);
    CALL(&vm,"draw_sprite_ext",sprite,0,15,6,1,1,0,0xff00,1);
    gml_render_prepare_draw(&render);assert(!memcmp(actual,expected,sizeof actual));
    assert(vm.builtins->phys_particles.points[0].colour==0x7f0000ffu);
    assert(vm.builtins->phys_particles.points[0].x==0.5 && fabs(vm.builtins->phys_particles.points[0].y-0.6)<1e-12);
    vm.render=NULL;gml_vm_free(&vm);gml_render_free(&render);
  }
  puts("particle API: named/cached sprite facade, water/mask/category filtering, pending zombie visibility, stored/override styles and simulation invariance passed");
}

static void particle_api_state(void){
  GmlVM vm;GmlWin win;setup(&vm,&win);
  CALL(&vm,"physics_particle_set_radius",1);
  CALL(&vm,"physics_particle_set_damping",0.25);
  CALL(&vm,"physics_world_gravity",0,4);
  assert(CALL(&vm,"physics_particle_create",GML_PHYS_PARTICLE_SPRING,10,20,2,3,0x112233,0.5,7).d==0);
  assert(CALL(&vm,"physics_particle_create",GML_PHYS_PARTICLE_SPRING,12,20,-2,-3,0xabcdef,1,8).d==1);
  assert(CALL(&vm,"physics_particle_create",GML_PHYS_PARTICLE_ZOMBIE,50,50,0,0,0xffffff,1,9).d==2);
  GmlPhysicsParticleState *particles=&vm.builtins->phys_particles;
  assert(particles->count==3 && particles->bond_count==1);
  double rest=particles->bonds[0].length;particles->points[1].x+=0.15;
  size_t capacity=gml_vm_state_size(&vm),written=0,used=0,first_size=0;
  unsigned char *before=malloc(capacity),*first=malloc(capacity),*second=malloc(capacity);
  assert(before && first && second && gml_vm_state_save(&vm,before,capacity,&written));
  for(int replay=0;replay<2;replay++){
    CALL(&vm,"physics_particle_set_category_flags",0,GML_PHYS_PARTICLE_ZOMBIE);step(&vm,1);
    assert(particles->count==0);
    assert(gml_vm_state_load(&vm,before,written,&used) && used==written);
    particles=&vm.builtins->phys_particles;
    assert(particles->count==3 && particles->bond_count==1 && particles->bonds[0].length==rest);
    size_t resaved=0;assert(gml_vm_state_save(&vm,second,capacity,&resaved));
    assert(resaved==written && !memcmp(before,second,written));
    step(&vm,30);assert(particles->count==2 && particles->bond_count==1);
    assert(particles->points[0].y>2 && particles->points[1].y>2);
    assert(gml_vm_state_save(&vm,second,capacity,&resaved));
    if(!replay){first_size=resaved;memcpy(first,second,resaved);}
    else assert(first_size==resaved && !memcmp(first,second,resaved));
  }
  gml_builtin_physics_room_reset(&vm);
  assert(!particles->count && !particles->bond_count && !particles->points && !particles->bonds);
  free(before);free(first);free(second);gml_vm_free(&vm);
  puts("particle state: live points, persistent rest lengths, pending zombies, exact resave/replay and room cleanup passed");
}

static double group_begin_box(GmlVM *vm,int cached,int flags,int group_flags,
                              double x,double y,double half,double strength){
  PARTICLE(vm,cached,"physics_particle_group_begin",flags,group_flags,x,y,0,0,0,0,0x123456,0.5,strength,7);
  PARTICLE(vm,cached,"physics_particle_group_box",half,half);
  return particle_api(vm,cached,"physics_particle_group_end",NULL,0).d;
}
static void particle_group_api(void){
  for(int cached=0;cached<2;cached++){
    GmlVM vm,peer;GmlWin win,peer_win;setup(&vm,&win);setup(&peer,&peer_win);
    CALL(&vm,"physics_particle_set_radius",1);CALL(&vm,"physics_particle_set_density",2);
    CALL(&vm,"physics_world_gravity",0,0);CALL(&vm,"physics_particle_set_damping",0);
    PARTICLE(&vm,cached,"physics_particle_group_begin",0,2,10,20,90,30,-40,120,0x112233,0.5,0.6,7);
    assert(vm.builtins->phys_particles.definition.active);
    assert(vm.builtins->phys_particles.group_count==0 && vm.builtins->phys_particles.count==0);
    PARTICLE(&vm,cached,"physics_particle_group_box",1.5,1.5);
    double first=particle_api(&vm,cached,"physics_particle_group_end",NULL,0).d;assert(first>0);
    assert(!vm.builtins->phys_particles.definition.active);
    assert(PARTICLE(&vm,cached,"physics_particle_group_count",first).d==9);
    assert(PARTICLE(&vm,cached,"physics_particle_group_count",NAN).d==-1);
    assert(PARTICLE(&peer,cached,"physics_particle_group_count",first).d==-1);
    assert(PARTICLE(&vm,cached,"physics_particle_get_group_flags",first).d==2);
    const char *queries[]={"physics_particle_group_get_x","physics_particle_group_get_y",
      "physics_particle_group_get_angle","physics_particle_group_get_centre_x",
      "physics_particle_group_get_centre_y","physics_particle_group_get_vel_x",
      "physics_particle_group_get_vel_y","physics_particle_group_get_ang_vel",
      "physics_particle_group_get_mass","physics_particle_group_get_inertia"};
    const double expected[]={10,20,-90,10,20,30,-40,120,0.405,0.01215};
    for(int i=0;i<10;i++){
      double value=PARTICLE(&vm,cached,queries[i],first).d;
      assert(fabs(value-expected[i])<1e-10);
      assert(PARTICLE(&vm,cached,queries[i],first+1000).d==-1);
    }
    assert(PARTICLE(&vm,cached,"physics_particle_create",0,500,500,0,0,0xffffff,1,8).d==9);
    double buffer=CALL(&vm,"buffer_create",4,1,1).d;
    CALL(&vm,"buffer_poke",buffer,0,1,0x5a);CALL(&vm,"buffer_poke",buffer,256,5,0x6a);
    CALL(&vm,"buffer_seek",buffer,0,4);
    PARTICLE(&vm,cached,"physics_particle_group_get_data",first,buffer,31);
    assert(CALL(&vm,"buffer_tell",buffer).d==4);
    assert(CALL(&vm,"buffer_peek",buffer,0,1).d==0x5a);
    assert(CALL(&vm,"buffer_peek",buffer,8,8).d==8.5);
    assert(CALL(&vm,"buffer_peek",buffer,12,8).d==21.5);
    assert(CALL(&vm,"buffer_peek",buffer,24,5).d==0x7f332211u);
    assert(CALL(&vm,"buffer_peek",buffer,252,6).d==7);
    assert(CALL(&vm,"buffer_peek",buffer,256,5).d==0x6a);
    PARTICLE(&vm,cached,"physics_particle_group_get_data",first+1000,buffer,31);
    assert(CALL(&vm,"buffer_peek",buffer,256,5).d==0x6a);
    PARTICLE(&vm,cached,"physics_particle_group_begin",0,1,12,20,0,0,0,0,0xffffff,1,0.5,-9);
    PARTICLE(&vm,cached,"physics_particle_group_circle",2);
    double second=particle_api(&vm,cached,"physics_particle_group_end",NULL,0).d;
    assert(second>first && PARTICLE(&vm,cached,"physics_particle_group_count",second).d==5);
    PARTICLE(&vm,cached,"physics_particle_set_group_flags",second,3);
    assert(PARTICLE(&vm,cached,"physics_particle_get_group_flags",second).d==3);
    PARTICLE(&vm,cached,"physics_particle_group_join",first,second);
    assert(PARTICLE(&vm,cached,"physics_particle_group_count",first).d==14);
    assert(PARTICLE(&vm,cached,"physics_particle_group_count",second).d==-1);
    assert(PARTICLE(&vm,cached,"physics_particle_get_group_flags",first).d==3);
    PARTICLE(&vm,cached,"physics_particle_group_begin",0,0,100,100,0,0,0,0,0xffffff,1,1,2);
    particle_api(&vm,cached,"physics_particle_group_polygon",NULL,0);
    const double polygon[][2]={{-3,-3},{3,-3},{0,0},{3,3},{-3,3}};
    for(int i=0;i<5;i++) PARTICLE(&vm,cached,"physics_particle_group_add_point",polygon[i][0],polygon[i][1]);
    double third=particle_api(&vm,cached,"physics_particle_group_end",NULL,0).d;
    assert(third>second && PARTICLE(&vm,cached,"physics_particle_group_count",third).d==25);
    PARTICLE(&vm,cached,"physics_particle_group_join",first,third);
    assert(PARTICLE(&vm,cached,"physics_particle_group_count",first).d==39);
    assert(PARTICLE(&vm,cached,"physics_particle_group_count",third).d==-1);
    PARTICLE(&vm,cached,"physics_particle_group_delete",first);
    assert(PARTICLE(&vm,cached,"physics_particle_group_count",first).d==39);
    CALL(&vm,"physics_pause_enable",1);step(&vm,1);
    assert(PARTICLE(&vm,cached,"physics_particle_group_count",first).d==39);
    CALL(&vm,"physics_pause_enable",0);step(&vm,1);
    assert(PARTICLE(&vm,cached,"physics_particle_group_count",first).d==-1);
    assert(PARTICLE(&vm,cached,"physics_particle_group_count",third).d==-1);
    PARTICLE(&vm,cached,"physics_particle_group_delete",third);step(&vm,1);
    assert(vm.builtins->phys_particles.count==1 && vm.builtins->phys_particles.group_count==0);
    CALL(&vm,"physics_particle_set_max_count",128);
    uint32_t next=vm.builtins->phys_particles.next_group;
    assert(group_begin_box(&vm,cached,0,0,0,0,100,1)==-1);
    assert(vm.builtins->phys_particles.count==1 && vm.builtins->phys_particles.next_group==next);
    assert(group_begin_box(&vm,cached,0,0,0,0,1e7,1)==-1);
    PARTICLE(&vm,cached,"physics_particle_group_begin",0,0,NAN,0,0,0,0,0,0,1,1,2);
    assert(particle_api(&vm,cached,"physics_particle_group_end",NULL,0).d==-1);
    PARTICLE(&vm,cached,"physics_particle_group_begin",0,0,0,0,0,0,0,0,0,1,1,2);
    particle_api(&vm,cached,"physics_particle_group_polygon",NULL,0);
    PARTICLE(&vm,cached,"physics_particle_group_add_point",0,0);
    PARTICLE(&vm,cached,"physics_particle_group_add_point",1,1);
    assert(particle_api(&vm,cached,"physics_particle_group_end",NULL,0).d==-1);
    assert(group_begin_box(&vm,cached,0,0,0,0,1,1)>third);
    PARTICLE(&vm,cached,"physics_particle_group_begin",0,0,50,50,0,0,0,0,0,1,1,7);
    PARTICLE(&vm,cached,"physics_particle_group_circle",-1.5);
    double boundary=particle_api(&vm,cached,"physics_particle_group_end",NULL,0).d;
    assert(boundary>third && PARTICLE(&vm,cached,"physics_particle_group_count",boundary).d==3);
    assert(PARTICLE(&vm,cached,"physics_particle_group_get_centre_x",boundary).d==49.5);
    assert(PARTICLE(&vm,cached,"physics_particle_group_get_x",boundary).d==50);
    gml_builtin_physics_room_reset(&vm);
    assert(!vm.builtins->phys_particles.groups && !vm.builtins->phys_particles.definition.active);
    gml_vm_free(&vm);gml_vm_free(&peer);
  }
  puts("particle groups: all22 named/cached APIs, shapes/hull, live statistics, records, flags, joins, deferred lifetime, stale handles and bounded refusal passed");
}
static void particle_group_motion(void){
  GmlVM vm;GmlWin win;setup(&vm,&win);
  CALL(&vm,"physics_particle_set_radius",1);CALL(&vm,"physics_particle_set_damping",0);
  CALL(&vm,"physics_world_gravity",0,0);
  CALL(&vm,"physics_particle_group_begin",0,2,100,100,0,30,0,120,0xffffff,1,1,7);
  CALL(&vm,"physics_particle_group_box",1.5,1.5);
  double id=call(&vm,"physics_particle_group_end",NULL,0).d;assert(id>0);
  double inertia=CALL(&vm,"physics_particle_group_get_inertia",id).d;
  assert(gml_room_speed(&vm)==30);
  const double elapsed=60*(double)(float)(1.0/30);
  double angle=120*elapsed*0.017453292519943295;
  step(&vm,60);
  assert(fabs(CALL(&vm,"physics_particle_group_get_centre_x",id).d-(100+30*elapsed))<1e-8);
  assert(fabs(CALL(&vm,"physics_particle_group_get_centre_y",id).d-100)<1e-8);
  assert(fabs(CALL(&vm,"physics_particle_group_get_angle",id).d-remainder(120*elapsed,360))<1e-7);
  assert(fabs(CALL(&vm,"physics_particle_group_get_ang_vel",id).d-120)<1e-7);
  assert(fabs(CALL(&vm,"physics_particle_group_get_inertia",id).d-inertia)<1e-10);
  for(int i=0;i<vm.builtins->phys_particles.count;i++){
    const GmlPhysicsParticle *p=&vm.builtins->phys_particles.points[i];
    assert(fabs(p->x-(10+3*elapsed)-(cos(angle)*p->local_x-sin(angle)*p->local_y))<1e-8);
    assert(fabs(p->y-10-(sin(angle)*p->local_x+cos(angle)*p->local_y))<1e-8);
  }
  CALL(&vm,"physics_particle_set_group_flags",id,0);
  step(&vm,10);
  double x=vm.builtins->phys_particles.points[0].x;
  CALL(&vm,"physics_particle_set_group_flags",id,2);
  assert(vm.builtins->phys_particles.points[0].x==x);
  step(&vm,1);assert(isfinite(CALL(&vm,"physics_particle_group_get_angle",id).d));
  gml_vm_free(&vm);
  double separation[2],position[2];
  for(int solid=0;solid<2;solid++){
    setup(&vm,&win);CALL(&vm,"physics_particle_set_radius",1);CALL(&vm,"physics_world_gravity",0,0);
    CALL(&vm,"physics_particle_set_damping",0);
    assert(group_begin_box(&vm,0,GML_PHYS_PARTICLE_WALL,solid?GML_PHYS_GROUP_SOLID:0,0,0,3,1)>0);
    int index=(int)CALL(&vm,"physics_particle_create",0,2.25,0,0,0,0xffffff,1,9).d;assert(index>=0);
    GmlPhysicsParticle original=vm.builtins->phys_particles.points[index];
    GmlPhysicsParticleConfig config=particle_config_for(&vm);GmlPhysicsContact contact[1];
    gml_physics_solver_step(NULL,0,NULL,0,NULL,0,0.1,0,0,0.0001,1,1,NULL,NULL,
                           contact,1,&vm.builtins->phys_particles,&config);
    separation[solid]=vm.builtins->phys_particles.points[index].vx;
    vm.builtins->phys_particles.points[index]=original;
    step(&vm,1);position[solid]=vm.builtins->phys_particles.points[index].x;
    gml_vm_free(&vm);
  }
  assert(separation[1]>separation[0]);
  assert(position[1]>position[0]);
  for(int material=0;material<3;material++){
  double error[2];
  for(int strength=0;strength<2;strength++){
    setup(&vm,&win);CALL(&vm,"physics_particle_set_radius",1);CALL(&vm,"physics_world_gravity",0,0);
    CALL(&vm,"physics_particle_set_damping",0);
    int flag=material==0?GML_PHYS_PARTICLE_SPRING:material==1?GML_PHYS_PARTICLE_ELASTIC:0;
    assert(group_begin_box(&vm,0,flag,0,0,0,1.5,strength)>0);
    if(material==2){
      CALL(&vm,"physics_particle_set_flags",8,GML_PHYS_PARTICLE_ELASTIC);
      assert(vm.builtins->phys_particles.bond_count==0);
    }
    vm.builtins->phys_particles.points[8].x+=0.03;
    GmlPhysicsParticleConfig config=particle_config_for(&vm);GmlPhysicsContact contact[1];
    gml_physics_solver_step(NULL,0,NULL,0,NULL,0,0.1,0,0,0.001,1,1,NULL,NULL,
                           contact,1,&vm.builtins->phys_particles,&config);
    error[strength]=fabs(vm.builtins->phys_particles.points[8].x-
                        vm.builtins->phys_particles.points[7].x-0.15);
    gml_vm_free(&vm);
  }
  assert(error[1]<error[0]);
  }
  puts("particle groups: exact rigid pose/spin/shape, live flags, positive solid force/displacement and actual cohesion strength passed");
}

static void particle_group_state(void){
  GmlVM vm;GmlWin win;setup(&vm,&win);
  CALL(&vm,"physics_particle_set_radius",1);CALL(&vm,"physics_world_gravity",0,0);
  double first=group_begin_box(&vm,0,GML_PHYS_PARTICLE_SPRING,GML_PHYS_GROUP_RIGID,10,20,1.5,0.4);
  double second=group_begin_box(&vm,0,0,0,100,100,1.5,1);assert(first>0 && second>first);
  CALL(&vm,"physics_particle_delete",0);
  CALL(&vm,"physics_particle_group_begin",0,1,200,200,30,0,0,0,0x123456,0.5,0.75,9);
  call(&vm,"physics_particle_group_polygon",NULL,0);
  CALL(&vm,"physics_particle_group_add_point",-3,-3);
  CALL(&vm,"physics_particle_group_add_point",3,-3);
  size_t capacity=gml_vm_state_size(&vm),written=0,used=0,repeated=0;
  unsigned char *snapshot=malloc(capacity),*again=malloc(capacity);assert(snapshot && again);
  assert(gml_vm_state_save(&vm,snapshot,capacity,&written));
  CALL(&vm,"physics_particle_group_delete",second);step(&vm,1);
  assert(CALL(&vm,"physics_particle_group_count",second).d==-1);
  assert(gml_vm_state_load(&vm,snapshot,written,&used) && used==written);
  assert(CALL(&vm,"physics_particle_group_count",first).d==9);
  assert(CALL(&vm,"physics_particle_group_count",second).d==9);
  assert(vm.builtins->phys_particles.points[0].flags&GML_PHYS_PARTICLE_ZOMBIE);
  assert(vm.builtins->phys_particles.definition.active && vm.builtins->phys_particles.definition.count==2);
  assert(gml_vm_state_save(&vm,again,capacity,&repeated));
  assert(repeated==written && !memcmp(again,snapshot,written));
  CALL(&vm,"physics_particle_group_add_point",0,3);
  double third=call(&vm,"physics_particle_group_end",NULL,0).d;assert(third>second);
  assert(CALL(&vm,"physics_particle_group_count",third).d>0);
  step(&vm,1);assert(CALL(&vm,"physics_particle_group_count",first).d==8);
  free(snapshot);free(again);gml_vm_free(&vm);
  puts("particle groups: live members/bonds, pending deletion and unfinished polygon restore and exact resave passed");
}

int main(void){
  particle_group_api();
  particle_group_motion();
  particle_group_state();
  particle_api_state();
  particle_api_lifetime();
  particle_api_data();
  particle_api_drawing();
  particle_kernel_transport();
  particle_kernel_contacts();
  particle_kernel_flags();
  if(!particle_configuration()) return EXIT_FAILURE;
  error_output_control();
  units_and_ownership(); offset_and_rotation(); binding_origin_and_phase(); floor_joints_and_restore(); room_change_rebuilds_body(); filtering_and_sensors(); malformed_geometry_and_mass();
  if(!bound_fixture_properties()) return EXIT_FAILURE;
  direct_joint_constructors();
  motor_joint_constructors();
  gear_joint_constructor();
  joint_fields();
  overlap_queries();
  raycast_queries();
  kinematic_fixture_templates();
  chain_fixtures();
  chain_state_budget();
  outline_geometry();
  instance_debug_drawing();
  puts("physics: units, fixture lifetime, offsets, rotation, room changes, contacts, joints and restore passed");
  return 0;
}
