/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Physics fixture/joint resources, mass lookup, and ordered dispatch. */
#include "gml_builtin_internal.h"
#include "gml_render.h"
#include "anygm_host.h"

#include <math.h>
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
static GmlPhysicsFixture *phys_fixture_new(GmlVM *vm){
  if(!vm) return NULL;
  for(int i=0;i<GML_PHYS_FIXTURE_MAX;i++) if(!vm->builtins->phys_fixture[i].live){
    GmlPhysicsFixture *f=&vm->builtins->phys_fixture[i];
    memset(f,0,sizeof(*f));
    f->live=1; f->id=phys_next_id(vm); f->bound_inst=-1; f->awake=1;
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

static double physics_room_scale(GmlVM *vm){
  if(!vm || !vm->win) return 0.0;
  GmlVal *dynamic=gml_varmap_get(&vm->globals,"__physics_world_scale");
  GmlVal *dynamic_room=gml_varmap_get(&vm->globals,"__physics_world_scale_room");
  if(dynamic && dynamic->t==V_REAL && dynamic->d>0.0 && dynamic_room &&
     dynamic_room->t==V_REAL && (int)dynamic_room->d==vm->room_index) return dynamic->d;
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

static double physics_instance_mass(GmlVM *vm, GmlInstance *in, double scale){
  if(!vm || !in || scale<=0.0) return 0.0;
  GmlVal *explicit_mass=gml_varmap_get(&in->vars,"phy_mass");
  if(explicit_mass && explicit_mass->t==V_REAL && explicit_mass->d>0.0) return explicit_mass->d;
  if(in->obj<0 || in->obj>=vm->n_objects) return 0.0;
  GmlObject *o=&vm->objects[in->obj];
  if(!o->physics_enabled || o->physics_kinematic || o->physics_density<=0.0 || o->physics_area_px<=0.0)
    return 0.0;
  return o->physics_density*o->physics_area_px*scale*scale;
}

GmlVal gml_builtin_try_physics(GmlVM *vm, const char *nm, GmlVal *a, int n){
  GmlRender *R=(GmlRender*)vm->render;
  (void)R;
  if(!strcmp(nm,"physics_fixture_create")){
    GmlPhysicsFixture *f=phys_fixture_new(vm);
    return vreal(f?(double)f->id:0);
  }
  if(!strcmp(nm,"physics_fixture_delete")){
    GmlPhysicsFixture *f=phys_fixture_find(vm,(int)N(a,n,0));
    if(f) f->live=0;
    return vreal(0);
  }
  if(!strcmp(nm,"physics_fixture_bind")){
    GmlPhysicsFixture *f=phys_fixture_find(vm,(int)N(a,n,0));
    if(f) f->bound_inst=(int)N(a,n,1);
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
  if(!strcmp(nm,"physics_joint_revolute_create")||!strcmp(nm,"physics_joint_prismatic_create")||
     !strcmp(nm,"physics_joint_wheel_create")||!strcmp(nm,"physics_joint_rope_create")){
    int type=!strcmp(nm,"physics_joint_revolute_create")?1:
             !strcmp(nm,"physics_joint_prismatic_create")?2:
             !strcmp(nm,"physics_joint_wheel_create")?3:4;
    GmlPhysicsJoint *j=phys_joint_new(vm,type);
    if(!j) return vreal(0);
    j->a=N(a,n,0); j->b=N(a,n,1); j->x1=N(a,n,2); j->y1=N(a,n,3);
    j->x2=N(a,n,4); j->y2=N(a,n,5);
    int pc=n-6; if(pc<0) pc=0; if(pc>24) pc=24;
    j->value_count=pc;
    for(int i=0;i<pc;i++) j->params[i]=N(a,n,6+i);
    return vreal((double)j->id);
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
      *gml_varmap_put(&vm->cur_self->vars,"phy_mass")=vreal(N(a,n,0));
      *gml_varmap_put(&vm->cur_self->vars,"phy_inertia")=vreal(N(a,n,3));
    }
    return vreal(0);
  }
  if(!strcmp(nm,"physics_world_create")){
    double scale=N(a,n,0);
    if(scale>0.0){
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
  if(!strcmp(nm,"physics_apply_impulse")){
    /* Convert Box2D's metre/second impulse response to the lightweight backend's pixels/step.
     * The two-metre translation cap is the public engine's per-step tunnelling guard. */
    GmlInstance *s=vm->cur_self;
    if(s && !vm->builtins->phys_paused){
      double scale=physics_room_scale(vm);
      double mass=physics_instance_mass(vm,s,scale);
      double hz=gml_room_speed(vm);
      if(mass>0.0 && hz>0.0){
        double ix=N(a,n,2), iy=N(a,n,3);
        double vx=s->hspeed+ix/(mass*scale*hz);
        double vy=s->vspeed+iy/(mass*scale*hz);
        double max_step=2.0/scale, step=hypot(vx,vy);
        if(step>max_step){ double k=max_step/step; vx*=k; vy*=k; }
        s->hspeed=vx; s->vspeed=vy;
        motion_from_components(s);
        if(builtin_setting(vm,"GML_LOG_PHYSICS"))
          anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[physics] impulse id=%u mass=%.6g scale=%.6g vel=(%.6g,%.6g)\n",
                  s->id,mass,scale,s->hspeed,s->vspeed);
      }
    }
    return vreal(0);
  }
  if(!strcmp(nm,"physics_apply_local_force")) return vreal(0);
  if(!strncmp(nm,"physics_",8)) return vreal(0);
  return gml_builtin_try_platform_extensions(vm,nm,a,n);
}

