/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Canonical body values and the disposable physics solve graph. */
#include "gml_builtin_internal.h"
#include "anygm_host.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static double value(const GmlInstance *in,const char *name,double fallback){
  GmlVal *v=gml_varmap_get((GmlVarMap *)&in->vars,name);
  return v && v->t==V_REAL && isfinite(v->d)?v->d:fallback;
}
static void store(GmlInstance *in,const char *name,double number){
  GmlVal *v=gml_varmap_put(&in->vars,name);
  if(v) *v=vreal(number);
}
static int initialized(const GmlInstance *in){ return value(in,"__phy_body",0)!=0; }

int gml_physics_body_enabled(GmlVM *vm,const GmlInstance *in){
  if(!vm || !in || gml_physics_world_scale(vm)<=0) return 0;
  return initialized(in) || (in->obj>=0 && in->obj<vm->n_objects &&
                            vm->objects[in->obj].physics_enabled);
}

static void initialize_values(GmlInstance *in){
  store(in,"__phy_body",1);
  store(in,"phy_rotation",value(in,"phy_rotation",-in->image_angle));
  store(in,"phy_linear_velocity_x",value(in,"phy_linear_velocity_x",0));
  store(in,"phy_linear_velocity_y",value(in,"phy_linear_velocity_y",0));
  store(in,"phy_angular_velocity",value(in,"phy_angular_velocity",0));
  store(in,"phy_active",value(in,"phy_active",1));
}

uint32_t gml_physics_bind_fixture(GmlVM *vm,const GmlPhysicsFixture *source,
                                 GmlInstance *in,double xoffset,double yoffset){
  if(!vm || !source || !in || !isfinite(xoffset) || !isfinite(yoffset)) return 0;
  GmlPhysicsFixture copy=*source;
  GmlPhysicsFixture *bound=gml_physics_fixture_new(vm);
  if(!bound) return 0;
  uint32_t id=bound->id;
  *bound=copy; bound->id=id; bound->live=1; bound->bound_inst=(int)in->id;
  bound->offset_x=xoffset; bound->offset_y=yoffset;
  if(!initialized(in)) initialize_values(in);
  store(in,"phy_linear_damping",copy.lin_damp);
  store(in,"phy_angular_damping",copy.ang_damp);
  return id;
}

void gml_physics_initialize_body(GmlVM *vm,GmlInstance *in){
  if(!gml_physics_body_enabled(vm,in) || initialized(in)) return;
  initialize_values(in);
  const GmlObject *o=&vm->objects[in->obj];
  GmlPhysicsFixture f;
  memset(&f,0,sizeof(f));
  f.density=o->physics_density; f.friction=o->physics_friction;
  f.restitution=o->physics_restitution; f.lin_damp=o->physics_linear_damping;
  f.ang_damp=o->physics_angular_damping; f.awake=o->physics_awake;
  f.group=o->physics_group; f.sensor=o->physics_sensor;
  if(o->physics_shape==0 && o->physics_point_count>0){
    f.shape=1; f.radius=fabs(o->physics_point[0][0]*in->image_xscale);
  } else if(o->physics_point_count>=3 && o->physics_point_count<=GML_PHYS_FIXTURE_POINTS){
    f.shape=3; f.points=o->physics_point_count;
    for(int i=0;i<f.points;i++){
      f.px[i]=o->physics_point[i][0]*in->image_xscale;
      f.py[i]=o->physics_point[i][1]*in->image_yscale;
    }
  }
  if(f.shape) (void)gml_physics_bind_fixture(vm,&f,in,0,0);
  store(in,"__phy_kinematic",o->physics_kinematic);
}

static void read_body(GmlVM *vm,GmlInstance *in,GmlPhysicsBody *body){
  memset(body,0,sizeof(*body));
  gml_physics_initialize_body(vm,in);
  body->id=in->id; body->object=in->obj;
  body->active=in->active && !in->marked && value(in,"phy_active",1)!=0;
  body->kinematic=value(in,"__phy_kinematic",0)!=0;
  body->fixed_rotation=value(in,"phy_fixed_rotation",0)!=0;
  body->bullet=value(in,"phy_bullet",0)!=0;
  body->x=in->x; body->y=in->y; body->angle=value(in,"phy_rotation",-in->image_angle);
  body->vx=value(in,"phy_linear_velocity_x",0);
  body->vy=value(in,"phy_linear_velocity_y",0);
  body->omega=value(in,"phy_angular_velocity",0);
  body->linear_damping=value(in,"phy_linear_damping",0);
  body->angular_damping=value(in,"phy_angular_damping",0);
  body->force_x=value(in,"__phy_force_x",0); body->force_y=value(in,"__phy_force_y",0);
  body->torque=value(in,"__phy_torque",0);
  gml_physics_solver_mass(body,vm->builtins->phys_fixture,GML_PHYS_FIXTURE_MAX,
                          gml_physics_world_scale(vm));
  body->explicit_mass=value(in,"__phy_mass_override",0)!=0;
  if(body->explicit_mass){
    body->mass=value(in,"phy_mass",body->mass);
    body->inertia=value(in,"phy_inertia",body->inertia);
    body->center_x=value(in,"__phy_center_x",body->center_x);
    body->center_y=value(in,"__phy_center_y",body->center_y);
  }
}

static void local_to_world(const GmlPhysicsBody *b,double x,double y,double *wx,double *wy){
  double angle=b->angle*M_PI/180.0,c=cos(angle),s=sin(angle);
  *wx=b->x+c*x-s*y; *wy=b->y+s*x+c*y;
}
static void world_to_local(GmlInstance *in,double x,double y,double *lx,double *ly){
  double angle=value(in,"phy_rotation",-in->image_angle)*M_PI/180.0;
  double dx=x-in->x,dy=y-in->y,c=cos(angle),s=sin(angle);
  *lx=c*dx+s*dy; *ly=-s*dx+c*dy;
}

void gml_physics_initialize_joint(GmlVM *vm,GmlPhysicsJoint *j){
  GmlInstance *a=gml_vm_instance_by_id(vm,j->a), *b=gml_vm_instance_by_id(vm,j->b);
  if(!a || !b || a==b) return;
  gml_physics_initialize_body(vm,a); gml_physics_initialize_body(vm,b);
  if(!initialized(a) || !initialized(b)) return;
  if(j->type==1){
    world_to_local(a,j->x1,j->y1,&j->anchor_ax,&j->anchor_ay);
    world_to_local(b,j->x1,j->y1,&j->anchor_bx,&j->anchor_by);
  } else if(j->type==4){
    /* Rope endpoints are local offsets in the language API. */
    j->anchor_ax=j->x1; j->anchor_ay=j->y1;
    j->anchor_bx=j->x2; j->anchor_by=j->y2;
  } else return;
  j->reference_angle=value(b,"phy_rotation",0)-value(a,"phy_rotation",0);
  j->initialized=1;
}

void gml_physics_apply(GmlVM *vm,GmlInstance *in,double x,double y,
                       double fx,double fy,int impulse,int local){
  if(!gml_physics_body_enabled(vm,in) || !isfinite(x) || !isfinite(y) ||
     !isfinite(fx) || !isfinite(fy)) return;
  GmlPhysicsBody b; read_body(vm,in,&b);
  if(b.mass<=0 || b.kinematic || !b.active || vm->builtins->phys_paused) return;
  double scale=gml_physics_world_scale(vm),cx,cy;
  local_to_world(&b,b.center_x,b.center_y,&cx,&cy);
  if(local){
    double angle=b.angle*M_PI/180.0,c=cos(angle),s=sin(angle),wx,wy;
    local_to_world(&b,x,y,&wx,&wy); x=wx; y=wy;
    double tx=c*fx-s*fy; fy=s*fx+c*fy; fx=tx;
  }
  double torque=((x-cx)*fy-(y-cy)*fx)*scale;
  if(impulse){
    store(in,"phy_linear_velocity_x",b.vx+fx/(b.mass*scale));
    store(in,"phy_linear_velocity_y",b.vy+fy/(b.mass*scale));
    if(b.inertia>0 && !b.fixed_rotation)
      store(in,"phy_angular_velocity",b.omega+torque/b.inertia*180.0/M_PI);
  } else {
    store(in,"__phy_force_x",b.force_x+fx); store(in,"__phy_force_y",b.force_y+fy);
    store(in,"__phy_torque",b.torque+torque);
  }
}

void gml_physics_apply_torque(GmlVM *vm,GmlInstance *in,double torque,int impulse){
  if(!gml_physics_body_enabled(vm,in) || !isfinite(torque)) return;
  GmlPhysicsBody b; read_body(vm,in,&b);
  if(b.mass<=0 || b.inertia<=0 || b.kinematic || b.fixed_rotation || !b.active ||
     vm->builtins->phys_paused) return;
  if(impulse) store(in,"phy_angular_velocity",b.omega+torque/b.inertia*180.0/M_PI);
  else store(in,"__phy_torque",b.torque+torque);
}

int gml_physics_variable_get(GmlVM *vm,GmlInstance *in,const char *name,GmlVal *out){
  if(!gml_physics_body_enabled(vm,in)) return 0;
  gml_physics_initialize_body(vm,in);
  double hz=fmax(1,gml_room_speed(vm)),number;
  if(!strcmp(name,"phy_position_x")) number=in->x;
  else if(!strcmp(name,"phy_position_y")) number=in->y;
  else if(!strcmp(name,"phy_speed_x")) number=value(in,"phy_linear_velocity_x",0)/hz;
  else if(!strcmp(name,"phy_speed_y")) number=value(in,"phy_linear_velocity_y",0)/hz;
  else if(!strcmp(name,"phy_speed"))
    number=hypot(value(in,"phy_linear_velocity_x",0),value(in,"phy_linear_velocity_y",0))/hz;
  else if(!strcmp(name,"phy_mass") || !strcmp(name,"phy_inertia") ||
          !strcmp(name,"phy_com_x") || !strcmp(name,"phy_com_y") ||
          !strcmp(name,"phy_dynamic") || !strcmp(name,"phy_kinematic")){
    GmlPhysicsBody b; read_body(vm,in,&b);
    if(!strcmp(name,"phy_mass")) number=b.mass;
    else if(!strcmp(name,"phy_inertia")) number=b.inertia;
    else if(!strcmp(name,"phy_dynamic")) number=b.mass>0 && !b.kinematic;
    else if(!strcmp(name,"phy_kinematic")) number=b.kinematic;
    else { double x,y; local_to_world(&b,b.center_x,b.center_y,&x,&y);
      number=!strcmp(name,"phy_com_x")?x:y; }
  } else if(!strcmp(name,"phy_sleeping")) number=0;
  else if(!strcmp(name,"phy_rotation") || !strcmp(name,"phy_linear_velocity_x") ||
          !strcmp(name,"phy_linear_velocity_y") || !strcmp(name,"phy_angular_velocity") ||
          !strcmp(name,"phy_linear_damping") || !strcmp(name,"phy_angular_damping") ||
          !strcmp(name,"phy_active") || !strcmp(name,"phy_bullet") ||
          !strcmp(name,"phy_fixed_rotation")) number=value(in,name,0);
  else return 0;
  *out=vreal(number); return 1;
}

int gml_physics_variable_set(GmlVM *vm,GmlInstance *in,const char *name,GmlVal v){
  if(!gml_physics_body_enabled(vm,in)) return 0;
  gml_physics_initialize_body(vm,in);
  double d=N(&v,1,0);
  if(!isfinite(d) || fabs(d)>1e12) return 1;
  double hz=fmax(1,gml_room_speed(vm));
  if(!strcmp(name,"phy_position_x")){ in->x=d; gml_colgrid_touch(vm,in); return 1; }
  if(!strcmp(name,"phy_position_y")){ in->y=d; gml_colgrid_touch(vm,in); return 1; }
  if(!strcmp(name,"phy_speed_x")){ name="phy_linear_velocity_x"; d*=hz; }
  if(!strcmp(name,"phy_speed_y")){ name="phy_linear_velocity_y"; d*=hz; }
  if(!strcmp(name,"phy_rotation")) { in->image_angle=-d; gml_colgrid_touch(vm,in); }
  else if(!strcmp(name,"phy_linear_velocity_x") || !strcmp(name,"phy_linear_velocity_y") ||
          !strcmp(name,"phy_angular_velocity")){
    GmlPhysicsBody b; read_body(vm,in,&b);
    if(b.mass<=0 && d!=0) store(in,"__phy_kinematic",1);
  } else if(strcmp(name,"phy_active") && strcmp(name,"phy_bullet") &&
            strcmp(name,"phy_fixed_rotation") && strcmp(name,"phy_linear_damping") &&
            strcmp(name,"phy_angular_damping")) return 0;
  store(in,name,d); return 1;
}

static int body_compare(const void *va,const void *vb){
  uint32_t a=((const GmlPhysicsBody *)va)->id,b=((const GmlPhysicsBody *)vb)->id;
  return a>b?1:a<b?-1:0;
}
static int contact_compare(const void *va,const void *vb){
  const GmlPhysicsContact *a=va,*b=vb;
  if(a->a!=b->a) return a->a>b->a?1:-1;
  return a->b>b->b?1:a->b<b->b?-1:0;
}
static int filter(void *context,int a,int b){
  return gml_vm_physics_collision_allowed(context,a,b);
}

void gml_physics_step(GmlVM *vm){
  double scale=gml_physics_world_scale(vm);
  if(scale<=0 || !vm->builtins || vm->builtins->phys_paused) return;
  /* Reclaim even the last destroyed body, before an empty world can return early.
   * Explicit-id lookup keeps deactivated instances and their attachments alive. */
  for(int i=0;i<GML_PHYS_FIXTURE_MAX;i++){
    GmlPhysicsFixture *f=&vm->builtins->phys_fixture[i];
    if(f->live && f->bound_inst>=0 && !gml_vm_instance_by_id(vm,f->bound_inst)) f->live=0;
  }
  for(int i=0;i<GML_PHYS_JOINT_MAX;i++){
    GmlPhysicsJoint *j=&vm->builtins->phys_joint[i];
    if(j->live && (!gml_vm_instance_by_id(vm,j->a) || !gml_vm_instance_by_id(vm,j->b))) j->live=0;
  }
  int count=0;
  for(int i=0;i<vm->inst_count;i++)
    if(!vm->inst[i].marked && (vm->inst[i].active || vm->inst[i].deactivated) &&
       gml_physics_body_enabled(vm,&vm->inst[i])) count++;
  if(!count) return;
  GmlPhysicsBody *bodies=calloc((size_t)count,sizeof(*bodies));
  int contact_capacity=GML_PHYS_FIXTURE_MAX*4;
  GmlPhysicsContact *contacts=calloc((size_t)contact_capacity,sizeof(*contacts));
  if(!bodies || !contacts){ free(bodies); free(contacts); return; }
  int at=0;
  for(int i=0;i<vm->inst_count;i++){
    GmlInstance *in=&vm->inst[i];
    if(!in->marked && (in->active || in->deactivated) && gml_physics_body_enabled(vm,in))
      read_body(vm,in,&bodies[at++]);
  }
  qsort(bodies,(size_t)count,sizeof(*bodies),body_compare);
  double hz=fmax(1,gml_room_speed(vm));
  int substeps=(int)fmin(16,fmax(1,ceil(vm->builtins->phys_update_speed/hz)));
  int iterations=vm->builtins->phys_update_iterations>0?vm->builtins->phys_update_iterations:10;
  int contact_count=gml_physics_solver_step(bodies,count,vm->builtins->phys_fixture,
      GML_PHYS_FIXTURE_MAX,vm->builtins->phys_joint,GML_PHYS_JOINT_MAX,scale,
      vm->builtins->phys_gravity_x,vm->builtins->phys_gravity_y,1.0/hz,substeps,
      iterations,filter,vm,contacts,contact_capacity);
  for(int i=0;i<count;i++){
    GmlPhysicsBody *b=&bodies[i]; GmlInstance *in=gml_vm_instance_by_id(vm,b->id);
    if(!in) continue;
    if(b->active){
      in->x=b->x; in->y=b->y; in->image_angle=-b->angle;
      store(in,"phy_rotation",b->angle);
      store(in,"phy_linear_velocity_x",b->vx); store(in,"phy_linear_velocity_y",b->vy);
      store(in,"phy_angular_velocity",b->omega);
      gml_colgrid_touch(vm,in);
    }
    store(in,"__phy_force_x",0); store(in,"__phy_force_y",0); store(in,"__phy_torque",0);
  }
  qsort(contacts,(size_t)contact_count,sizeof(*contacts),contact_compare);
  for(int i=0;i<contact_count;i++){
    const GmlPhysicsContact *c=&contacts[i];
    gml_vm_physics_collision_event(vm,c->a,c->b,c->x,c->y,c->nx,c->ny);
  }
  free(contacts); free(bodies);
}
