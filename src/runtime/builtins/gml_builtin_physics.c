/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Physics fixture/joint resources, mass lookup, and ordered dispatch. */
#include "gml_builtin_internal.h"
#include "gml_render.h"
#include "anygm_host.h"

#include <math.h>
#include <limits.h>
#include <string.h>
static uint32_t phys_next_id(GmlVM *vm){
  if(!vm->builtins->phys_next_id) vm->builtins->phys_next_id=4000001u;
  return vm->builtins->phys_next_id++;
}
static GmlPhysicsFixture *phys_fixture_find(GmlVM *vm, int id){
  if(!vm || id<=0) return NULL;
  for(int i=0;i<GML_PHYS_FIXTURE_MAX;i++)
    if(vm->builtins->phys_fixture[i].live && vm->builtins->phys_fixture[i].id==(uint32_t)id) return &vm->builtins->phys_fixture[i];
  return NULL;
}
GmlPhysicsFixture *gml_physics_fixture_new(GmlVM *vm){
  if(!vm) return NULL;
  for(int i=0;i<GML_PHYS_FIXTURE_MAX;i++) if(!vm->builtins->phys_fixture[i].live){
    GmlPhysicsFixture *f=&vm->builtins->phys_fixture[i];
    memset(f,0,sizeof(*f));
    f->live=1; f->id=phys_next_id(vm); f->bound_inst=-1; f->awake=1;
    f->density=0.5; f->friction=0.2; f->lin_damp=0.1; f->ang_damp=0.1;
    return f;
  }
  return NULL;
}
static GmlPhysicsJoint *phys_joint_find(GmlVM *vm, int id){
  if(!vm || id<=0) return NULL;
  for(int i=0;i<GML_PHYS_JOINT_MAX;i++)
    if(vm->builtins->phys_joint[i].live && vm->builtins->phys_joint[i].id==(uint32_t)id) return &vm->builtins->phys_joint[i];
  return NULL;
}
static GmlPhysicsJoint *phys_joint_new(GmlVM *vm, int type){
  if(!vm) return NULL;
  for(int i=0;i<GML_PHYS_JOINT_MAX;i++) if(!vm->builtins->phys_joint[i].live){
    GmlPhysicsJoint *j=&vm->builtins->phys_joint[i];
    memset(j,0,sizeof(*j));
    j->live=1; j->id=phys_next_id(vm); j->type=type;
    return j;
  }
  return NULL;
}

double gml_physics_world_scale(GmlVM *vm){
  if(!vm) return 0.0;
  GmlVal *dynamic=gml_varmap_get(&vm->globals,"__physics_world_scale");
  GmlVal *dynamic_room=gml_varmap_get(&vm->globals,"__physics_world_scale_room");
  if(dynamic && dynamic->t==V_REAL && dynamic->d>0.0 && dynamic_room &&
     dynamic_room->t==V_REAL && isfinite(dynamic->d) && dynamic->d<=1000.0 &&
     (int)dynamic_room->d==vm->room_index) return dynamic->d;
  if(!vm->win) return 0.0;
  const GmlChunk *rc=gml_chunk(vm->win,"ROOM");
  if(!rc || vm->room_index<0) return 0.0;
  uint64_t rend=(uint64_t)rc->off+rc->size;
  uint64_t slot=(uint64_t)rc->off+4u+(uint64_t)(uint32_t)vm->room_index*4u;
  if(slot+4u>rend) return 0.0;
  const uint8_t *d=vm->win->data;
  uint32_t rp=u32(d,(uint32_t)slot);
  if(rp<rc->off || (uint64_t)rp+88u>rend || u32(d,rp+56)!=1) return 0.0;
  uint32_t bits=u32(d,rp+84); float scale;
  memcpy(&scale,&bits,sizeof(scale));
  return isfinite(scale) && scale>0.000001f && scale<=1000.0f ? scale : 0.0;
}

static int physics_target(GmlVM *vm,GmlInstance *in,int target){
  if(!in->active || in->marked) return 0;
  if(target==-1) return in==vm->cur_self;
  if(target==-2) return in==vm->cur_other;
  if(target==-3) return 1;
  if(target>=100000) return in->id==(uint32_t)target;
  return target>=0 && gml_object_is(vm,in->obj,target);
}

GmlVal gml_builtin_try_physics(GmlVM *vm, const char *nm, GmlVal *a, int n){
  GmlRender *R=(GmlRender*)vm->render;
  (void)R;
  if(!strcmp(nm,"physics_fixture_create")){
    GmlPhysicsFixture *f=gml_physics_fixture_new(vm);
    return vreal(f?(double)f->id:0);
  }
  if(!strcmp(nm,"physics_fixture_delete")){
    GmlPhysicsFixture *f=phys_fixture_find(vm,(int)N(a,n,0));
    if(f && f->bound_inst<0) f->live=0;
    return vreal(0);
  }
  if(!strcmp(nm,"physics_fixture_bind") || !strcmp(nm,"physics_fixture_bind_ext")){
    GmlPhysicsFixture *f=phys_fixture_find(vm,(int)N(a,n,0));
    uint32_t result=0;
    if(f && f->bound_inst<0){
      GmlPhysicsFixture source=*f;
      int target=(int)N(a,n,1);
      for(int i=0;i<vm->inst_count;i++) if(physics_target(vm,&vm->inst[i],target)){
        gml_physics_initialize_body(vm,&vm->inst[i]);
        uint32_t id=gml_physics_bind_fixture(vm,&source,&vm->inst[i],N(a,n,2),N(a,n,3));
        if(!result) result=id;
      }
    }
    return vreal(result);
  }
  if(!strcmp(nm,"physics_remove_fixture")){
    GmlPhysicsFixture *f=phys_fixture_find(vm,(int)N(a,n,1));
    if(f && f->bound_inst>=0){
      for(int i=0;i<vm->inst_count;i++)
        if(vm->inst[i].id==(uint32_t)f->bound_inst && physics_target(vm,&vm->inst[i],(int)N(a,n,0)))
          f->live=0;
    }
    return vreal(0);
  }
  if(!strcmp(nm,"physics_get_density") || !strcmp(nm,"physics_set_density") ||
     !strcmp(nm,"physics_get_friction") || !strcmp(nm,"physics_set_friction") ||
     !strcmp(nm,"physics_get_restitution") || !strcmp(nm,"physics_set_restitution")){
    double id=N(a,n,0);
    if(n<1 || !isfinite(id) || id<1 || id>INT_MAX || floor(id)!=id) return vreal(0);
    GmlPhysicsFixture *fixture=phys_fixture_find(vm,(int)id);
    if(!fixture || fixture->bound_inst<0 || !vm->cur_self ||
       (uint32_t)fixture->bound_inst!=vm->cur_self->id) return vreal(0);
    double *value=strstr(nm,"density")?&fixture->density:
                  strstr(nm,"friction")?&fixture->friction:&fixture->restitution;
    if(nm[8]=='g') return vreal(*value);
    double next=N(a,n,1);
    if(n>=2 && isfinite(next) && next>=0) *value=next;
    return vreal(0);
  }
  if(!strcmp(nm,"physics_fixture_add_point")){
    GmlPhysicsFixture *f=phys_fixture_find(vm,(int)N(a,n,0));
    if(f && f->points<GML_PHYS_FIXTURE_POINTS){
      int p=f->points++;
      f->px[p]=N(a,n,1); f->py[p]=N(a,n,2);
    }
    return vreal(0);
  }
  if(!strcmp(nm,"physics_fixture_set_polygon_shape")){
    GmlPhysicsFixture *f=phys_fixture_find(vm,(int)N(a,n,0));
    if(f){ f->shape=3; f->points=0; }
    return vreal(0);
  }
  if(!strcmp(nm,"physics_fixture_set_edge_shape")){
    GmlPhysicsFixture *f=phys_fixture_find(vm,(int)N(a,n,0));
    if(f){ f->shape=4; f->x1=N(a,n,1); f->y1=N(a,n,2); f->x2=N(a,n,3); f->y2=N(a,n,4); }
    return vreal(0);
  }
  if(!strcmp(nm,"physics_fixture_set_circle_shape")){
    GmlPhysicsFixture *f=phys_fixture_find(vm,(int)N(a,n,0));
    if(f){ f->shape=1; f->radius=N(a,n,1); }
    return vreal(0);
  }
  if(!strcmp(nm,"physics_fixture_set_box_shape")){
    GmlPhysicsFixture *f=phys_fixture_find(vm,(int)N(a,n,0));
    if(f){ f->shape=2; f->w=N(a,n,1); f->h=N(a,n,2); }
    return vreal(0);
  }
  if(!strcmp(nm,"physics_fixture_set_density")){
    GmlPhysicsFixture *f=phys_fixture_find(vm,(int)N(a,n,0)); if(f) f->density=N(a,n,1); return vreal(0); }
  if(!strcmp(nm,"physics_fixture_set_friction")){
    GmlPhysicsFixture *f=phys_fixture_find(vm,(int)N(a,n,0)); if(f) f->friction=N(a,n,1); return vreal(0); }
  if(!strcmp(nm,"physics_fixture_set_restitution")){
    GmlPhysicsFixture *f=phys_fixture_find(vm,(int)N(a,n,0)); if(f) f->restitution=N(a,n,1); return vreal(0); }
  if(!strcmp(nm,"physics_fixture_set_linear_damping")){
    GmlPhysicsFixture *f=phys_fixture_find(vm,(int)N(a,n,0)); if(f) f->lin_damp=N(a,n,1); return vreal(0); }
  if(!strcmp(nm,"physics_fixture_set_angular_damping")){
    GmlPhysicsFixture *f=phys_fixture_find(vm,(int)N(a,n,0)); if(f) f->ang_damp=N(a,n,1); return vreal(0); }
  if(!strcmp(nm,"physics_fixture_set_awake")){
    GmlPhysicsFixture *f=phys_fixture_find(vm,(int)N(a,n,0)); if(f) f->awake=N(a,n,1); return vreal(0); }
  if(!strcmp(nm,"physics_fixture_set_sensor")){
    GmlPhysicsFixture *f=phys_fixture_find(vm,(int)N(a,n,0)); if(f) f->sensor=N(a,n,1)!=0; return vreal(0); }
  if(!strcmp(nm,"physics_fixture_set_collision_group")){
    GmlPhysicsFixture *f=phys_fixture_find(vm,(int)N(a,n,0)); if(f) f->group=(int)N(a,n,1); return vreal(0); }
  if(!strcmp(nm,"physics_joint_distance_create") || !strcmp(nm,"physics_joint_friction_create") ||
     !strcmp(nm,"physics_joint_weld_create") || !strcmp(nm,"physics_joint_pulley_create") ||
     !strcmp(nm,"physics_joint_prismatic_create") || !strcmp(nm,"physics_joint_wheel_create")){
    GmlPhysicsJoint joint={0};
    int required=7;
    if(!strcmp(nm,"physics_joint_distance_create")) joint.type=GML_PHYS_JOINT_DISTANCE;
    else if(!strcmp(nm,"physics_joint_friction_create")) joint.type=GML_PHYS_JOINT_FRICTION;
    else if(!strcmp(nm,"physics_joint_weld_create")){
      joint.type=GML_PHYS_JOINT_WELD; required=8;
    } else if(!strcmp(nm,"physics_joint_pulley_create")){
      joint.type=GML_PHYS_JOINT_PULLEY; required=12;
    } else if(!strcmp(nm,"physics_joint_prismatic_create")){
      joint.type=GML_PHYS_JOINT_PRISMATIC; required=13;
    } else {
      joint.type=GML_PHYS_JOINT_WHEEL; required=12;
    }
    double scale=gml_physics_world_scale(vm);
    if(n<required || scale<=0) return vreal(-1);
    for(int i=0;i<required;i++){
      double arg=N(a,n,i);
      if(!isfinite(arg) || fabs(arg)>1e12) return vreal(-1);
    }
    joint.a=N(a,n,0); joint.b=N(a,n,1);
    if(joint.a==-1 && vm->cur_self) joint.a=vm->cur_self->id;
    if(joint.a==-2 && vm->cur_other) joint.a=vm->cur_other->id;
    if(joint.b==-1 && vm->cur_self) joint.b=vm->cur_self->id;
    if(joint.b==-2 && vm->cur_other) joint.b=vm->cur_other->id;
    if(joint.a<0 || joint.b<0 || joint.a>UINT32_MAX || joint.b>UINT32_MAX) return vreal(-1);
    joint.x1=N(a,n,2); joint.y1=N(a,n,3);
    if(joint.type==GML_PHYS_JOINT_DISTANCE){
      joint.x2=N(a,n,4); joint.y2=N(a,n,5);
      joint.params[1]=N(a,n,6)!=0;
      joint.value_count=2;
    } else if(joint.type==GML_PHYS_JOINT_PULLEY){
      joint.x2=N(a,n,4); joint.y2=N(a,n,5);
      joint.anchor_ax=N(a,n,6); joint.anchor_ay=N(a,n,7);
      joint.anchor_bx=N(a,n,8); joint.anchor_by=N(a,n,9);
      joint.params[0]=N(a,n,10); joint.params[1]=N(a,n,11)!=0;
      if(joint.params[0]<1e-12) return vreal(-1);
      joint.value_count=4;
    } else if(joint.type==GML_PHYS_JOINT_PRISMATIC || joint.type==GML_PHYS_JOINT_WHEEL){
      joint.x2=N(a,n,4); joint.y2=N(a,n,5);
      if(joint.x2==0 && joint.y2==0) return vreal(-1);
      joint.value_count=required-6;
      for(int i=0;i<joint.value_count;i++) joint.params[i]=N(a,n,6+i);
    } else {
      int first=joint.type==GML_PHYS_JOINT_WELD?5:4;
      /* Weld input gives A relative to B, while the solver stores B relative to A. */
      if(joint.type==GML_PHYS_JOINT_WELD) joint.reference_angle=-N(a,n,4);
      joint.params[0]=fmax(0,N(a,n,first));
      joint.params[1]=fmax(0,N(a,n,first+1));
      joint.params[2]=N(a,n,first+2)!=0;
      joint.value_count=3;
    }
    gml_physics_initialize_joint(vm,&joint);
    if(!joint.initialized) return vreal(-1);
    double limit=fmin(1e12,1e6/scale);
    const double anchors[]={joint.anchor_ax,joint.anchor_ay,joint.anchor_bx,joint.anchor_by};
    for(unsigned i=0;i<sizeof anchors/sizeof anchors[0];i++)
      if(!isfinite(anchors[i]) || fabs(anchors[i])>limit) return vreal(-1);
    if(joint.type==GML_PHYS_JOINT_DISTANCE && joint.params[0]>limit) return vreal(-1);
    if(joint.type==GML_PHYS_JOINT_PRISMATIC &&
       (fabs(joint.params[0])>limit || fabs(joint.params[1])>limit ||
        joint.params[0]>joint.params[1])) return vreal(-1);
    if(joint.type==GML_PHYS_JOINT_PULLEY &&
       (fabs(joint.x1)>limit || fabs(joint.y1)>limit ||
        fabs(joint.x2)>limit || fabs(joint.y2)>limit ||
        !isfinite(joint.params[2]) || !isfinite(joint.params[3]) ||
        joint.params[2]>limit || joint.params[3]>limit)) return vreal(-1);
    GmlPhysicsJoint *owned=phys_joint_new(vm,joint.type);
    if(!owned) return vreal(-1);
    joint.id=owned->id;
    joint.live=1;
    *owned=joint;
    return vreal(joint.id);
  }
  if(!strcmp(nm,"physics_joint_revolute_create") || !strcmp(nm,"physics_joint_rope_create")){
    int type=!strcmp(nm,"physics_joint_revolute_create")?GML_PHYS_JOINT_REVOLUTE:GML_PHYS_JOINT_ROPE;
    GmlPhysicsJoint *j=phys_joint_new(vm,type);
    if(!j) return vreal(0);
    j->a=N(a,n,0); j->b=N(a,n,1); j->x1=N(a,n,2); j->y1=N(a,n,3);
    j->x2=N(a,n,4); j->y2=N(a,n,5);
    int pc=n-6; if(pc<0) pc=0; if(pc>24) pc=24;
    j->value_count=pc;
    for(int i=0;i<pc;i++) j->params[i]=N(a,n,6+i);
    gml_physics_initialize_joint(vm,j);
    return vreal((double)j->id);
  }
  if(!strcmp(nm,"physics_joint_enable_motor")){
    double id=N(a,n,0);
    if(n<2 || !(id>0 && id<=INT_MAX)) return vreal(0);
    GmlPhysicsJoint *j=phys_joint_find(vm,(int)id);
    if(!j) return vreal(0);
    int enabled=N(a,n,1)!=0;
    switch(j->type){
      case GML_PHYS_JOINT_REVOLUTE: j->params[3]=enabled; break;
      case GML_PHYS_JOINT_PRISMATIC: j->params[5]=enabled; break;
      case GML_PHYS_JOINT_WHEEL: j->params[0]=enabled; break;
    }
    return vreal(0);
  }
  if(!strcmp(nm,"physics_joint_delete")){
    GmlPhysicsJoint *j=phys_joint_find(vm,(int)N(a,n,0));
    if(j) j->live=0;
    return vreal(0);
  }
  if(!strcmp(nm,"physics_joint_set_value")){
    GmlPhysicsJoint *j=phys_joint_find(vm,(int)N(a,n,0));
    int idx=(int)N(a,n,1);
    if(j && idx>=0 && idx<24){ j->params[idx]=N(a,n,2); if(idx>=j->value_count) j->value_count=idx+1; }
    return vreal(0);
  }
  if(!strcmp(nm,"physics_mass_properties")){
    if(vm->cur_self){
      gml_physics_initialize_body(vm,vm->cur_self);
      *gml_varmap_put(&vm->cur_self->vars,"__phy_mass_override")=vreal(1);
      *gml_varmap_put(&vm->cur_self->vars,"phy_mass")=vreal(N(a,n,0));
      *gml_varmap_put(&vm->cur_self->vars,"phy_inertia")=vreal(N(a,n,3));
      *gml_varmap_put(&vm->cur_self->vars,"__phy_center_x")=vreal(N(a,n,1));
      *gml_varmap_put(&vm->cur_self->vars,"__phy_center_y")=vreal(N(a,n,2));
    }
    return vreal(0);
  }
  if(!strcmp(nm,"physics_world_create")){
    double scale=N(a,n,0);
    if(isfinite(scale) && scale>0.000001 && scale<=1000.0){
      *gml_varmap_put(&vm->globals,"__physics_world_scale")=vreal(scale);
      *gml_varmap_put(&vm->globals,"__physics_world_scale_room")=vreal(vm->room_index);
    }
    return vreal(0);
  }
  if(!strcmp(nm,"physics_world_gravity")){ vm->builtins->phys_gravity_x=N(a,n,0); vm->builtins->phys_gravity_y=N(a,n,1); return vreal(0); }
  if(!strcmp(nm,"physics_world_update_speed")){ vm->builtins->phys_update_speed=N(a,n,0); return vreal(0); }
  if(!strcmp(nm,"physics_world_update_iterations")){ vm->builtins->phys_update_iterations=(int)N(a,n,0); return vreal(0); }
  if(!strcmp(nm,"physics_pause_enable")){ vm->builtins->phys_paused=(int)N(a,n,0); return vreal(0); }
  if(!strcmp(nm,"physics_world_draw_debug")){ vm->builtins->phys_debug_draw=(int)N(a,n,0); return vreal(0); }
  if(!strcmp(nm,"physics_apply_impulse") || !strcmp(nm,"physics_apply_local_impulse") ||
     !strcmp(nm,"physics_apply_force") || !strcmp(nm,"physics_apply_local_force")){
    gml_physics_apply(vm,vm->cur_self,N(a,n,0),N(a,n,1),N(a,n,2),N(a,n,3),
                      strstr(nm,"impulse")!=NULL,strstr(nm,"local")!=NULL);
    return vreal(0);
  }
  if(!strcmp(nm,"physics_apply_torque") || !strcmp(nm,"physics_apply_angular_impulse")){
    gml_physics_apply_torque(vm,vm->cur_self,N(a,n,0),strstr(nm,"impulse")!=NULL);
    return vreal(0);
  }
  if(!strncmp(nm,"physics_",8)) return vreal(0);
  return gml_builtin_try_platform_extensions(vm,nm,a,n);
}
