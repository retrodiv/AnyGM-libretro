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
  motor_joint_constructors();
  gear_joint_constructor();
  joint_fields();
  overlap_queries();
  raycast_queries();
  kinematic_fixture_templates();
  chain_fixtures();
  chain_state_budget();
  puts("physics: units, fixture lifetime, offsets, rotation, contacts, joints and restore passed");
  return 0;
}
