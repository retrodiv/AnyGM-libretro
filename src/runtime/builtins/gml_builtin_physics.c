/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Physics fixture/joint resources, mass lookup, and ordered dispatch. */
#include "gml_builtin_internal.h"
#include "gml_render.h"
#include "anygm_host.h"

#include <math.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
void gml_physics_report_error(GmlVM *vm,const char *message){
  if(vm && vm->diagnostics.physics_error_output)
    anygm_host_logf(vm->host,ANYGM_LOG_ERROR,"[physics] %s\n",message);
}

static uint32_t phys_next_id(GmlVM *vm){
  if(!vm->builtins->phys_next_id) vm->builtins->phys_next_id=4000001u;
  return vm->builtins->phys_next_id++;
}
static GmlPhysicsFixture *phys_fixture_find(GmlVM *vm, int id){
  if(!vm || id<=0){
    gml_physics_report_error(vm,"fixture handle is not live");
    return NULL;
  }
  for(int i=0;i<GML_PHYS_FIXTURE_MAX;i++)
    if(vm->builtins->phys_fixture[i].live && vm->builtins->phys_fixture[i].id==(uint32_t)id) return &vm->builtins->phys_fixture[i];
  gml_physics_report_error(vm,"fixture handle is not live");
  return NULL;
}
static int fixture_units_used(const GmlVM *vm){
  int used=0;
  for(int i=0;i<GML_PHYS_FIXTURE_MAX;i++){
    const GmlPhysicsFixture *fixture=&vm->builtins->phys_fixture[i];
    if(fixture->live) used+=gml_physics_fixture_units(fixture->points);
  }
  return used;
}

GmlPhysicsFixture *gml_physics_fixture_new(GmlVM *vm,int units){
  if(!vm || fixture_units_used(vm)>GML_PHYS_FIXTURE_MAX-units){
    gml_physics_report_error(vm,"fixture capacity exceeded");
    return NULL;
  }
  for(int i=0;i<GML_PHYS_FIXTURE_MAX;i++) if(!vm->builtins->phys_fixture[i].live){
    GmlPhysicsFixture *f=&vm->builtins->phys_fixture[i];
    memset(f,0,sizeof(*f));
    f->live=1; f->id=phys_next_id(vm); f->bound_inst=-1; f->awake=1;
    f->density=GML_PHYS_FIXTURE_DEFAULT_DENSITY; f->friction=0.2; f->lin_damp=0.1; f->ang_damp=0.1;
    return f;
  }
  gml_physics_report_error(vm,"fixture capacity exceeded");
  return NULL;
}

static void fixture_clear_chain(GmlPhysicsFixture *fixture){
  if(!gml_physics_is_chain(fixture->shape)) return;
  free(fixture->chain);
  fixture->chain=NULL;
  fixture->chain_capacity=0;
  fixture->points=0;
}

void gml_physics_fixture_drop(GmlPhysicsFixture *fixture){
  fixture_clear_chain(fixture);
  fixture->live=0;
}

static void fixture_append_chain(GmlVM *vm,GmlPhysicsFixture *fixture,double x,double y){
  if(fixture->bound_inst>=0 || !isfinite(x) || !isfinite(y) ||
     fabs(x)>1e12 || fabs(y)>1e12 || fixture->points>=GML_PHYS_CHAIN_POINT_LIMIT) return;
  int count=fixture->points;
  if(gml_physics_fixture_units(count+1)>gml_physics_fixture_units(count) &&
     fixture_units_used(vm)>=GML_PHYS_FIXTURE_MAX) return;
  if(count>=fixture->chain_capacity){
    int capacity=fixture->chain_capacity?fixture->chain_capacity*2:GML_PHYS_FIXTURE_POINTS;
    if(capacity>GML_PHYS_CHAIN_POINT_LIMIT) capacity=GML_PHYS_CHAIN_POINT_LIMIT;
    GmlPhysicsPoint *points=realloc(fixture->chain,(size_t)capacity*sizeof(*points));
    if(!points) return;
    fixture->chain=points;
    fixture->chain_capacity=capacity;
  }
  fixture->chain[count]=(GmlPhysicsPoint){x,y};
  fixture->points++;
}

GmlPhysicsJoint *gml_physics_find_joint(GmlVM *vm,double id){
  if(!vm || !vm->builtins || !(id>=1 && id<=UINT32_MAX)) return NULL;
  for(int i=0;i<GML_PHYS_JOINT_MAX;i++)
    if(vm->builtins->phys_joint[i].live && vm->builtins->phys_joint[i].id==(uint32_t)id) return &vm->builtins->phys_joint[i];
  return NULL;
}
static GmlPhysicsJoint *phys_joint_find_checked(GmlVM *vm,double id){
  GmlPhysicsJoint *joint=gml_physics_find_joint(vm,id);
  if(!joint) gml_physics_report_error(vm,"joint handle is not live");
  return joint;
}
void gml_physics_drop_joint(GmlVM *vm,GmlPhysicsJoint *joint){
  joint->live=0;
  if(joint->type!=GML_PHYS_JOINT_REVOLUTE && joint->type!=GML_PHYS_JOINT_PRISMATIC) return;
  for(int i=0;i<GML_PHYS_JOINT_MAX;i++){
    GmlPhysicsJoint *gear=&vm->builtins->phys_joint[i];
    if(gear->live && gear->type==GML_PHYS_JOINT_GEAR &&
       (gear->x1==joint->id || gear->x2==joint->id)) gear->live=0;
  }
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

static GmlInstance *query_fixture_owner(GmlVM *vm,const GmlPhysicsFixture *fixture){
  if(!fixture->live || fixture->bound_inst<0) return NULL;
  GmlInstance *in=gml_vm_instance_by_id(vm,fixture->bound_inst);
  if(!in || !in->active || in->marked) return NULL;
  return in;
}

static int query_target(GmlVM *vm,GmlInstance *in,GmlVal selector){
  return gml_val_is_number_or_object(selector) &&
         selector.d>=INT_MIN && selector.d<=INT_MAX &&
         physics_target(vm,in,(int)selector.d);
}

static int ray_target(GmlVM *vm,GmlInstance *in,GmlVal selector){
  if(selector.t!=V_ARR) return query_target(vm,in,selector);
  int length=gml_val_array_length(selector);
  for(int i=0;i<length;i++)
    if(query_target(vm,in,gml_arr_get(selector,i))) return 1;
  return 0;
}

static int physics_overlap(GmlVM *vm,GmlVal *arguments,int count){
  GmlInstance *self=vm->cur_self;
  double scale=gml_physics_world_scale(vm);
  if(count<4 || !self || scale<=0 || self->marked) return 0;
  GmlPhysicsBody pose=gml_physics_placement_pose(self,N(arguments,count,0),
                                                N(arguments,count,1),N(arguments,count,2));
  GmlVal selector=arguments[3];
  const GmlPhysicsFixture *fixtures=vm->builtins->phys_fixture;
  int sources[GML_PHYS_FIXTURE_MAX],source_count=0;
  for(int i=0;i<GML_PHYS_FIXTURE_MAX;i++)
    if(fixtures[i].live && fixtures[i].bound_inst==(int)self->id) sources[source_count++]=i;
  if(!source_count) return 0;
  for(int i=0;i<GML_PHYS_FIXTURE_MAX;i++){
    const GmlPhysicsFixture *target=&fixtures[i];
    GmlInstance *in=query_fixture_owner(vm,target);
    if(!in || !query_target(vm,in,selector)) continue;
    if(in==self && selector.d!=-1 && selector.d!=self->id) continue;
    GmlPhysicsBody other=gml_physics_body_pose(in);
    for(int j=0;j<source_count;j++)
      if(gml_physics_fixture_overlap(&fixtures[sources[j]],&pose,target,&other,scale)) return 1;
  }
  return 0;
}

static int ray_hit_compare(const void *left,const void *right){
  const GmlPhysicsRayHit *a=left,*b=right;
  if(a->fraction!=b->fraction) return a->fraction<b->fraction?-1:1;
  if(a->fixture!=b->fixture) return a->fixture<b->fixture?-1:1;
  return a->child<b->child?-1:a->child>b->child;
}

typedef struct {
  GmlPhysicsRayHit *hits;
  int count,capacity,all,failed;
} PhysicsRayHits;

static int collect_ray_hit(void *context,const GmlPhysicsRayHit *hit){
  PhysicsRayHits *result=context;
  if(!result->all){
    if(!result->count || ray_hit_compare(hit,result->hits)<0){
      *result->hits=*hit;
      result->count=1;
    }
    return 1;
  }
  if(result->count==result->capacity){
    int capacity=result->capacity?result->capacity*2:16;
    if(capacity>GML_PHYS_CHAIN_POINT_LIMIT) capacity=GML_PHYS_CHAIN_POINT_LIMIT;
    if(capacity<=result->capacity){ result->failed=1; return 0; }
    GmlPhysicsRayHit *hits=realloc(result->hits,(size_t)capacity*sizeof(*hits));
    if(!hits){ result->failed=1; return 0; }
    result->hits=hits; result->capacity=capacity;
  }
  result->hits[result->count++]=*hit;
  return 1;
}

static GmlVal physics_raycast(GmlVM *vm,GmlVal *arguments,int count){
  double scale=gml_physics_world_scale(vm);
  if(count<5 || scale<=0) return vundef();
  GmlPhysicsRay ray={N(arguments,count,0),N(arguments,count,1),
                     N(arguments,count,2),N(arguments,count,3),count>=7?N(arguments,count,6):1};
  if(!isfinite(ray.max_fraction) || ray.max_fraction<0) return vundef();
  ray.max_fraction=fmin(ray.max_fraction,1);
  GmlPhysicsRayHit closest;
  PhysicsRayHits result={0};
  result.all=count>=6 && N(arguments,count,5)!=0;
  if(!result.all) result.hits=&closest;
  for(int i=0;i<GML_PHYS_FIXTURE_MAX && !result.failed;i++){
    const GmlPhysicsFixture *fixture=&vm->builtins->phys_fixture[i];
    GmlInstance *in=query_fixture_owner(vm,fixture);
    if(!in || !ray_target(vm,in,arguments[4])) continue;
    GmlPhysicsBody pose=gml_physics_body_pose(in);
    if(!pose.active) continue;
    gml_physics_fixture_raycast(fixture,&pose,&ray,scale,collect_ray_hit,&result);
    if(!result.all && result.count) ray.max_fraction=closest.fraction;
  }
  int found=result.count;
  GmlPhysicsRayHit *hits=result.hits;
  if(!found || result.failed){ if(result.all) free(hits); return vundef(); }
  if(found>1) qsort(hits,(size_t)found,sizeof(*hits),ray_hit_compare);
  GmlVal array=gml_arr_new(found,vundef());
  int complete=gml_val_array_length(array)==found;
  const char *const names[]={"instance","hitpointX","hitpointY","normalX","normalY","fraction"};
  for(int i=0;i<found && complete;i++){
    GmlInstance *record=gml_struct_new(vm);
    if(!record){ complete=0; break; }
    const GmlPhysicsRayHit *hit=&hits[i];
    const double values[]={hit->instance,hit->x,hit->y,hit->nx,hit->ny,hit->fraction};
    for(unsigned j=0;j<sizeof names/sizeof names[0];j++){
      GmlVal *field=gml_varmap_put(&record->vars,names[j]);
      if(!field){ complete=0; break; }
      *field=vreal(values[j]);
    }
    if(complete) gml_arr_set(array,i,vreal(record->id));
  }
  if(result.all) free(hits);
  if(!complete){ gml_values_release_owned(&array,1); return vundef(); }
  return array;
}

/* Writable fields use the same canonical slot for get and set. */
static double *joint_setting(GmlPhysicsJoint *joint,int field){
  switch(joint->type){
    case GML_PHYS_JOINT_REVOLUTE:
      switch(field){
        case GML_JOINT_MOTOR_SPEED: return &joint->params[2];
        case GML_JOINT_MAX_MOTOR_TORQUE: return &joint->params[1];
        case GML_JOINT_LOWER_ANGLE_LIMIT: return &joint->x2;
        case GML_JOINT_UPPER_ANGLE_LIMIT: return &joint->y2;
        case GML_JOINT_ANGLE_LIMITS: return &joint->params[0];
      }
      break;
    case GML_PHYS_JOINT_PRISMATIC:
      if(field==GML_JOINT_MOTOR_SPEED) return &joint->params[4];
      if(field==GML_JOINT_MAX_MOTOR_FORCE) return &joint->params[3];
      break;
    case GML_PHYS_JOINT_WHEEL:
      switch(field){
        case GML_JOINT_MOTOR_SPEED: return &joint->params[2];
        case GML_JOINT_MAX_MOTOR_TORQUE: return &joint->params[1];
        case GML_JOINT_FREQUENCY: return &joint->params[3];
        case GML_JOINT_DAMPING_RATIO: return &joint->params[4];
      }
      break;
    case GML_PHYS_JOINT_DISTANCE:
      if(field==GML_JOINT_LENGTH_1) return &joint->params[0];
      if(field==GML_JOINT_FREQUENCY) return &joint->params[2];
      if(field==GML_JOINT_DAMPING_RATIO) return &joint->params[3];
      break;
    case GML_PHYS_JOINT_WELD:
      if(field==GML_JOINT_FREQUENCY) return &joint->params[0];
      if(field==GML_JOINT_DAMPING_RATIO) return &joint->params[1];
      break;
    case GML_PHYS_JOINT_ROPE:
      if(field==GML_JOINT_MAX_LENGTH) return &joint->params[0];
      break;
    case GML_PHYS_JOINT_FRICTION:
      if(field==GML_JOINT_MAX_FORCE) return &joint->params[0];
      if(field==GML_JOINT_MAX_TORQUE) return &joint->params[1];
      break;
  }
  return NULL;
}

static double joint_setting_scale(GmlVM *vm,const GmlPhysicsJoint *joint,int field){
  if(field==GML_JOINT_LENGTH_1 || field==GML_JOINT_MAX_LENGTH)
    return gml_physics_world_scale(vm);
  if(field==GML_JOINT_MOTOR_SPEED && joint->type==GML_PHYS_JOINT_REVOLUTE)
    return M_PI/180.0;
  return 1;
}

static double joint_get_value(GmlVM *vm,GmlPhysicsJoint *joint,int field){
  double *setting=joint_setting(joint,field);
  if(setting){
    switch(field){
      case GML_JOINT_ANGLE_LIMITS: return *setting!=0;
      case GML_JOINT_LOWER_ANGLE_LIMIT: return fmin(joint->x2,joint->y2);
      case GML_JOINT_UPPER_ANGLE_LIMIT: return fmax(joint->x2,joint->y2);
      case GML_JOINT_MOTOR_SPEED: return *setting*joint_setting_scale(vm,joint,field);
      case GML_JOINT_LENGTH_1:
      case GML_JOINT_MAX_LENGTH: return fmax(.005,*setting*joint_setting_scale(vm,joint,field));
      default: return fmax(0,*setting*joint_setting_scale(vm,joint,field));
    }
  }
  if(joint->value_count<=GML_JOINT_SAMPLE_FORCE_X){
    switch(field){
      case GML_JOINT_REACTION_FORCE_X: return joint->params[GML_JOINT_SAMPLE_FORCE_X];
      case GML_JOINT_REACTION_FORCE_Y: return joint->params[GML_JOINT_SAMPLE_FORCE_Y];
      case GML_JOINT_REACTION_TORQUE: return joint->params[GML_JOINT_SAMPLE_TORQUE];
      case GML_JOINT_MOTOR_TORQUE: return joint->params[GML_JOINT_SAMPLE_MOTOR_TORQUE];
      case GML_JOINT_MOTOR_FORCE: return joint->params[GML_JOINT_SAMPLE_MOTOR_FORCE];
    }
  }
  return gml_physics_joint_geometry(vm,joint,field);
}

static void joint_set_value(GmlVM *vm,GmlPhysicsJoint *joint,int field,double number){
  double *setting=joint_setting(joint,field);
  if(!setting || !isfinite(number)) return;
  double scale=joint_setting_scale(vm,joint,field);
  if(scale<=0) return;
  double canonical=number/scale;
  if(fabs(canonical)>1e12) return;
  switch(field){
    case GML_JOINT_ANGLE_LIMITS: canonical=number>0.5; break;
    case GML_JOINT_LOWER_ANGLE_LIMIT: canonical=fmin(canonical,joint->y2); break;
    case GML_JOINT_UPPER_ANGLE_LIMIT: canonical=fmax(canonical,joint->x2); break;
    case GML_JOINT_MOTOR_SPEED: break;
    case GML_JOINT_LENGTH_1:
    case GML_JOINT_MAX_LENGTH:
      if(number>1e6) return;
      canonical=fmax(0,canonical);
      break;
    default: canonical=fmax(0,canonical); break;
  }
  *setting=canonical;
}

static void physics_outline_line(void *context,double x1,double y1,double x2,double y2){
  GmlVal args[]={vreal(x1),vreal(y1),vreal(x2),vreal(y2)};
  gml_builtin_draw_primitive((GmlVM*)context,"draw_line",args,4);
}
static void physics_outline_circle(void *context,double x,double y,double radius){
  GmlVal args[]={vreal(x),vreal(y),vreal(radius),vreal(1)};
  gml_builtin_draw_primitive((GmlVM*)context,"draw_circle",args,4);
}
static GmlVal physics_draw_debug(GmlVM *vm){
  GmlInstance *self=vm->cur_self;
  double scale=gml_physics_world_scale(vm);
  if(!vm->render || !self || !self->active || self->marked || scale<=0) return vundef();
  gml_physics_initialize_body(vm,self);
  GmlPhysicsBody pose=gml_physics_body_pose(self);
  for(int i=0;i<GML_PHYS_FIXTURE_MAX;i++){
    const GmlPhysicsFixture *fixture=&vm->builtins->phys_fixture[i];
    if(fixture->live && fixture->bound_inst==(int)self->id)
      gml_physics_fixture_outline(fixture,&pose,scale,physics_outline_line,physics_outline_circle,vm);
  }
  return vundef();
}

static int particle_integer(double value,int *out){
  if(!isfinite(value) || value<INT_MIN || value>INT_MAX) return 0;
  *out=(int)value;return 1;
}
static int particle_mask(double value,uint32_t *out){
  if(!isfinite(value) || value<INT32_MIN || value>UINT32_MAX) return 0;
  *out=(uint32_t)(int64_t)value;return 1;
}
static GmlVal physics_particle_create(GmlVM *vm,GmlVal *args,int count){
  double scale=gml_physics_world_scale(vm);uint32_t flags,colour;int category;
  if(count<8 || scale<=0 || !particle_mask(N(args,count,0),&flags) ||
     !particle_mask(N(args,count,5),&colour) ||
     !particle_integer(N(args,count,7),&category) || !category) return vreal(-1);
  double alpha=N(args,count,6);
  if(!isfinite(alpha)) return vreal(-1);
  alpha=fmax(0,fmin(1,alpha));
  GmlPhysicsParticle particle={N(args,count,1)*scale,N(args,count,2)*scale,
    N(args,count,3)*scale,N(args,count,4)*scale,flags,
    (colour&0xffffffu)|((uint32_t)(alpha*255)<<24),category,0,0,0,0};
  GmlBuiltinState *state=vm->builtins;
  GmlPhysicsParticleConfig config={state->phys_particle_radius_m,state->phys_particle_density,
    state->phys_particle_damping,state->phys_particle_gravity_scale,state->phys_particle_max_count};
  return vreal(gml_physics_particles_add(&state->phys_particles,&config,particle));
}
static GmlVal physics_particle_remove(GmlVM *vm,GmlVal *args,int count,int shape){
  double scale=gml_physics_world_scale(vm);GmlPhysicsParticleState *state=&vm->builtins->phys_particles;
  if(scale<=0) return vundef();
  if(!shape){
    int index;
    if(count>0 && particle_integer(N(args,count,0),&index) && index>=0 && index<state->count)
      state->points[index].flags|=GML_PHYS_PARTICLE_ZOMBIE;
    return vundef();
  }
  double xs[8],ys[8],cx=0,cy=0,rx=0,ry=0;int vertices=0;
  if(shape==1 || shape==2){
    if(count<(shape==1?3:4)) return vundef();
    cx=N(args,count,0);cy=N(args,count,1);rx=fabs(N(args,count,2));
    ry=shape==1?rx:fabs(N(args,count,3));
    if(!isfinite(cx) || !isfinite(cy) || !isfinite(rx) || !isfinite(ry)) return vundef();
  }else{
    int id=0,array=count>0 && args[0].t==V_ARR;
    if(count<1 || (!array && !particle_integer(N(args,count,0),&id))) return vundef();
    int size=array?gml_val_array_length(args[0]):gml_ds_list_size_direct(vm,id);
    if(size<6 || size>16 || size%2) return vundef();
    vertices=size/2;
    for(int i=0;i<vertices;i++){
      GmlVal x=array?gml_arr_get(args[0],i*2):gml_ds_list_find_value_direct(vm,id,i*2);
      GmlVal y=array?gml_arr_get(args[0],i*2+1):gml_ds_list_find_value_direct(vm,id,i*2+1);
      xs[i]=N(&x,1,0);ys[i]=N(&y,1,0);
      if(!isfinite(xs[i]) || !isfinite(ys[i]) || fabs(xs[i])>1e12 || fabs(ys[i])>1e12) return vundef();
    }
  }
  for(int i=0;i<state->count;i++){
    GmlPhysicsParticle *point=&state->points[i];double x=point->x/scale,y=point->y/scale;
    int inside=shape==1?hypot(x-cx,y-cy)<=rx:
      shape==2?(fabs(x-cx)<=rx && fabs(y-cy)<=ry):gml_builtin_point_in_polygon(x,y,xs,ys,vertices);
    if(inside) point->flags|=GML_PHYS_PARTICLE_ZOMBIE;
  }
  return vundef();
}
static GmlVal physics_particle_flags(GmlVM *vm,GmlVal *args,int count,int category){
  int target;uint32_t flags;
  if(count<2 || gml_physics_world_scale(vm)<=0 ||
     !particle_integer(N(args,count,0),&target) || !particle_mask(N(args,count,1),&flags)) return vundef();
  GmlPhysicsParticleState *state=&vm->builtins->phys_particles;
  if(!category) gml_physics_particles_flags(state,target,flags);
  else for(int i=0;i<state->count;i++)
    if(!target || state->points[i].category==target) gml_physics_particles_flags(state,i,flags);
  return vundef();
}
static void particle_data_word(unsigned char *bytes,int *offset,uint32_t value){
  for(unsigned i=0;i<4;i++) bytes[(*offset)++]=(unsigned char)(value>>(8*i));
}
static void particle_data_real(unsigned char *bytes,int *offset,double value){
  float number=(float)value;uint32_t bits;memcpy(&bits,&number,sizeof bits);
  particle_data_word(bytes,offset,bits);
}
static GmlVal physics_particle_data(GmlVM *vm,GmlVal *args,int count,int single){
  double scale=gml_physics_world_scale(vm);int id,index=0;uint32_t flags;
  if(count<(single?3:2) || scale<=0 ||
     !particle_integer(N(args,count,single?1:0),&id) ||
     !particle_mask(N(args,count,single?2:1),&flags) ||
     (single && !particle_integer(N(args,count,0),&index))) return vundef();
  GmlPhysicsParticleState *state=&vm->builtins->phys_particles;
  if(single==1 && (index<0 || index>=state->count)) return vundef();
  if(single==2 && !gml_physics_particle_group_find(state,(uint32_t)index)) return vundef();
  int slot=vm_buffer_slot(vm,id);GmlBuiltinBuffer *buffer=slot>=0?buffer_at(vm,slot):NULL;
  if(!buffer) return vundef();
  int previous=buffer->pos;
  for(int i=single==1?index:0;i<(single==1?index+1:state->count);i++){
    const GmlPhysicsParticle *point=&state->points[i];unsigned char bytes[28];int offset=0;
    if(single==2 && point->group!=(uint32_t)index) continue;
    if(flags&1u) particle_data_word(bytes,&offset,point->flags);
    if(flags&2u){particle_data_real(bytes,&offset,point->x/scale);particle_data_real(bytes,&offset,point->y/scale);}
    if(flags&4u){particle_data_real(bytes,&offset,point->vx/scale);particle_data_real(bytes,&offset,point->vy/scale);}
    if(flags&8u) particle_data_word(bytes,&offset,(point->colour&0xff00ff00u)|
      ((point->colour&0xffu)<<16)|((point->colour>>16)&0xffu));
    if(flags&16u) particle_data_word(bytes,&offset,(uint32_t)point->category);
    if(offset>INT_MAX-buffer->pos) break;
    int expected=buffer->pos+offset;buffer_write_raw(vm,slot,bytes,offset);
    if(buffer->pos!=expected) break;
  }
  buffer->pos=previous;return vundef();
}

static GmlPhysicsParticleConfig particle_group_config(GmlVM *vm){
  GmlBuiltinState *s=vm->builtins;
  GmlPhysicsParticleConfig config={s->phys_particle_radius_m,s->phys_particle_density,
    s->phys_particle_damping,s->phys_particle_gravity_scale,s->phys_particle_max_count};
  return config;
}
static GmlVal particle_group_definition(GmlVM *vm,const char *nm,GmlVal *args,int count){
  double scale=gml_physics_world_scale(vm);
  if(scale<=0) return !strcmp(nm,"physics_particle_group_end")?vreal(-1):vundef();
  GmlPhysicsParticleState *state=&vm->builtins->phys_particles;
  GmlPhysicsParticleGroupDefinition *definition=&state->definition;
  const double radians=0.017453292519943295;
  if(!strcmp(nm,"physics_particle_group_begin")){
    *definition=(GmlPhysicsParticleGroupDefinition){0};
    uint32_t flags,group_flags,colour;int category;
    if(count<12 || !particle_mask(N(args,count,0),&flags) ||
       !particle_mask(N(args,count,1),&group_flags) || !particle_mask(N(args,count,8),&colour) ||
       !particle_integer(N(args,count,11),&category) || !category) return vundef();
    for(int i=2;i<11;i++) if(!isfinite(N(args,count,i))) return vundef();
    definition->flags=flags;definition->group_flags=group_flags;definition->category=category;
    definition->x=N(args,count,2)*scale;definition->y=N(args,count,3)*scale;
    definition->angle=-N(args,count,4)*radians;
    definition->vx=N(args,count,5)*scale;definition->vy=N(args,count,6)*scale;
    definition->omega=N(args,count,7)*radians;
    definition->colour=(colour&0xffffffu)|((uint32_t)(fmax(0,fmin(1,N(args,count,9)))*255)<<24);
    definition->strength=fmax(0,fmin(1,N(args,count,10)));
    if(!isfinite(definition->x) || !isfinite(definition->y) ||
       !isfinite(definition->vx) || !isfinite(definition->vy) ||
       fabs(definition->x)>1e6 || fabs(definition->y)>1e6 ||
       fabs(definition->vx)>1e6 || fabs(definition->vy)>1e6) return vundef();
    definition->active=1;return vundef();
  }
  if(!strcmp(nm,"physics_particle_group_end")){
    GmlPhysicsParticleConfig config=particle_group_config(vm);
    return vreal(gml_physics_particle_group_create(state,&config));
  }
  if(!definition->active) return vundef();
  if(!strcmp(nm,"physics_particle_group_polygon")){
    definition->shape=3;definition->count=0;definition->rx=definition->ry=0;
    memset(definition->px,0,sizeof definition->px);memset(definition->py,0,sizeof definition->py);
  }else if(!strcmp(nm,"physics_particle_group_add_point")){
    double x=N(args,count,0)*scale,y=N(args,count,1)*scale;
    if(count>=2 && definition->shape==3 && definition->count<8 &&
       isfinite(x) && isfinite(y) && fabs(x)<=1e6 && fabs(y)<=1e6){
      definition->px[definition->count]=x;definition->py[definition->count++]=y;
    }
  }else if(!strcmp(nm,"physics_particle_group_circle") || !strcmp(nm,"physics_particle_group_box")){
    int shape=!strcmp(nm,"physics_particle_group_circle")?1:2;
    double x=fabs(N(args,count,0)*scale),y=shape==1?x:fabs(N(args,count,1)*scale);
    if(count>=(shape==1?1:2) && isfinite(x) && isfinite(y) && x>0 && y>0 && x<=1e6 && y<=1e6){
      definition->shape=shape;definition->rx=x;definition->ry=y;definition->count=0;
      memset(definition->px,0,sizeof definition->px);memset(definition->py,0,sizeof definition->py);
    }
  }
  return vundef();
}
static GmlVal particle_group_value(GmlVM *vm,const char *nm,GmlVal *args,int count){
  int id;double scale=gml_physics_world_scale(vm);int write=!strcmp(nm,"physics_particle_set_group_flags");
  if(count<1 || scale<=0 || !particle_integer(N(args,count,0),&id) || id<=0)
    return write?vundef():vreal(-1);
  GmlPhysicsParticleState *state=&vm->builtins->phys_particles;
  GmlPhysicsParticleGroup *group=gml_physics_particle_group_find(state,(uint32_t)id);
  if(!group) return write?vundef():vreal(-1);
  if(write){
    uint32_t flags;
    if(count>=2 && particle_mask(N(args,count,1),&flags))
      gml_physics_particle_group_flags(state,(uint32_t)id,flags);
    return vundef();
  }
  if(!strcmp(nm,"physics_particle_get_group_flags")) return vreal(group->flags);
  if(!strcmp(nm,"physics_particle_group_get_x")) return vreal(group->x/scale);
  if(!strcmp(nm,"physics_particle_group_get_y")) return vreal(group->y/scale);
  if(!strcmp(nm,"physics_particle_group_get_angle"))
    return vreal(remainder(group->angle,6.2831853071795864769)/0.017453292519943295);
  GmlPhysicsParticleConfig config=particle_group_config(vm);GmlPhysicsParticleGroupStatistics stats;
  gml_physics_particle_group_statistics(state,&config,(uint32_t)id,&stats);
  if(!strcmp(nm,"physics_particle_group_count")) return vreal(stats.count);
  if(!strcmp(nm,"physics_particle_group_get_mass")) return vreal(stats.mass);
  if(!strcmp(nm,"physics_particle_group_get_inertia")) return vreal(stats.inertia);
  if(!strcmp(nm,"physics_particle_group_get_centre_x")) return vreal(stats.cx/scale);
  if(!strcmp(nm,"physics_particle_group_get_centre_y")) return vreal(stats.cy/scale);
  if(!strcmp(nm,"physics_particle_group_get_vel_x")) return vreal(stats.vx/scale);
  if(!strcmp(nm,"physics_particle_group_get_vel_y")) return vreal(stats.vy/scale);
  if(!strcmp(nm,"physics_particle_group_get_ang_vel")) return vreal(stats.omega/0.017453292519943295);
  return vundef();
}
static GmlVal physics_particle_draw(GmlVM *vm,GmlVal *args,int count,int extended){
  double scale=gml_physics_world_scale(vm);int category,sprite;uint32_t mask;
  if(!vm->render || scale<=0 || count<(extended?9:4) ||
     !particle_mask(N(args,count,0),&mask) || !particle_integer(N(args,count,1),&category) ||
     !particle_integer(N(args,count,2),&sprite)) return vundef();
  double subimage=N(args,count,3);
  if(!isfinite(subimage) || subimage<INT_MIN || subimage>INT_MAX) return vundef();
  if(extended) for(int i=4;i<9;i++) if(!isfinite(N(args,count,i))) return vundef();
  GmlPhysicsParticleState *state=&vm->builtins->phys_particles;
  for(int i=0;i<state->count;i++){
    const GmlPhysicsParticle *point=&state->points[i];
    if((point->flags && !(point->flags&mask)) || (category && point->category!=category)) continue;
    gml_draw_sprite_ext(vm->render,sprite,gml_draw_subimg(vm,subimage),point->x/scale,point->y/scale,
      extended?N(args,count,4):1,extended?N(args,count,5):1,extended?N(args,count,6):0,
      extended?NU32(args,count,7):(point->colour&0xffffffu),extended?N(args,count,8):(point->colour>>24)/255.0);
  }
  return vundef();
}

static GmlVal particle_configuration(GmlVM *vm,int property,int write,
                                    GmlVal *arguments,int count){
  double scale=gml_physics_world_scale(vm);
  if(scale<=0) return write?vundef():vreal(-1);
  GmlBuiltinState *state=vm->builtins;
  double *properties[]={&state->phys_particle_radius_m,&state->phys_particle_density,
                        &state->phys_particle_damping,&state->phys_particle_gravity_scale};
  if(!write){
    double value=property==4?state->phys_particle_max_count:*properties[property];
    return vreal(property==0?value/scale:value);
  }
  double value=N(arguments,count,0);
  if(count<1 || !isfinite(value)) return vundef();
  if(property==4){
    if(value<0 || value>INT_MAX || (value>0 && value<128)) return vundef();
    state->phys_particle_max_count=(int)value;
  }else{
    if(property<2 && value<=0) return vundef();
    if(property==0){
      value*=scale;
      if(!isfinite(value) || value<=0 || !isfinite(value/scale)) return vundef();
    }
    if(property==2 && value<0) value=0;
    *properties[property]=value;
  }
  return vundef();
}

GmlVal gml_builtin_try_physics(GmlVM *vm, const char *nm, GmlVal *a, int n){
  if(!strcmp(nm,"physics_particle_get_group_flags") || !strcmp(nm,"physics_particle_set_group_flags") ||
     !strcmp(nm,"physics_particle_group_count") || !strcmp(nm,"physics_particle_group_get_data") ||
     !strcmp(nm,"physics_particle_group_get_inertia") || !strcmp(nm,"physics_particle_group_get_mass") ||
     !strcmp(nm,"physics_particle_group_get_centre_x") || !strcmp(nm,"physics_particle_group_get_centre_y") ||
     !strcmp(nm,"physics_particle_group_get_vel_x") || !strcmp(nm,"physics_particle_group_get_vel_y") ||
     !strcmp(nm,"physics_particle_group_get_ang_vel") || !strcmp(nm,"physics_particle_group_get_x") ||
     !strcmp(nm,"physics_particle_group_get_y") || !strcmp(nm,"physics_particle_group_get_angle"))
    return !strcmp(nm,"physics_particle_group_get_data")?physics_particle_data(vm,a,n,2):particle_group_value(vm,nm,a,n);
  if(!strcmp(nm,"physics_particle_group_join") || !strcmp(nm,"physics_particle_group_delete")){
    int id,from;
    if(gml_physics_world_scale(vm)<=0 || n<1 || !particle_integer(N(a,n,0),&id) || id<=0) return vundef();
    GmlPhysicsParticleState *state=&vm->builtins->phys_particles;
    if(!strcmp(nm,"physics_particle_group_delete")){
      if(gml_physics_particle_group_find(state,(uint32_t)id))
        for(int i=0;i<state->count;i++) if(state->points[i].group==(uint32_t)id)
          state->points[i].flags|=GML_PHYS_PARTICLE_ZOMBIE;
    }else if(n>=2 && particle_integer(N(a,n,1),&from) && from>0){
      GmlPhysicsParticleConfig config=particle_group_config(vm);
      gml_physics_particle_group_join(state,&config,(uint32_t)id,(uint32_t)from);
    }
    return vundef();
  }
  if(!strcmp(nm,"physics_particle_group_begin") || !strcmp(nm,"physics_particle_group_circle") ||
     !strcmp(nm,"physics_particle_group_box") || !strcmp(nm,"physics_particle_group_polygon") ||
     !strcmp(nm,"physics_particle_group_add_point") || !strcmp(nm,"physics_particle_group_end"))
    return particle_group_definition(vm,nm,a,n);
  if(!strcmp(nm,"physics_particle_create")) return physics_particle_create(vm,a,n);
  if(!strcmp(nm,"physics_particle_count"))
    return vreal(gml_physics_world_scale(vm)>0?vm->builtins->phys_particles.count:-1);
  if(!strcmp(nm,"physics_particle_delete")) return physics_particle_remove(vm,a,n,0);
  if(!strcmp(nm,"physics_particle_delete_region_circle")) return physics_particle_remove(vm,a,n,1);
  if(!strcmp(nm,"physics_particle_delete_region_box")) return physics_particle_remove(vm,a,n,2);
  if(!strcmp(nm,"physics_particle_delete_region_poly")) return physics_particle_remove(vm,a,n,3);
  if(!strcmp(nm,"physics_particle_set_flags")) return physics_particle_flags(vm,a,n,0);
  if(!strcmp(nm,"physics_particle_set_category_flags")) return physics_particle_flags(vm,a,n,1);
  if(!strcmp(nm,"physics_particle_get_data")) return physics_particle_data(vm,a,n,0);
  if(!strcmp(nm,"physics_particle_get_data_particle")) return physics_particle_data(vm,a,n,1);
  if(!strcmp(nm,"physics_particle_draw")) return physics_particle_draw(vm,a,n,0);
  if(!strcmp(nm,"physics_particle_draw_ext")) return physics_particle_draw(vm,a,n,1);
  if(!strcmp(nm,"physics_particle_get_radius"))
    return particle_configuration(vm,0,0,a,n);
  if(!strcmp(nm,"physics_particle_set_radius"))
    return particle_configuration(vm,0,1,a,n);
  if(!strcmp(nm,"physics_particle_get_density"))
    return particle_configuration(vm,1,0,a,n);
  if(!strcmp(nm,"physics_particle_set_density"))
    return particle_configuration(vm,1,1,a,n);
  if(!strcmp(nm,"physics_particle_get_damping"))
    return particle_configuration(vm,2,0,a,n);
  if(!strcmp(nm,"physics_particle_set_damping"))
    return particle_configuration(vm,2,1,a,n);
  if(!strcmp(nm,"physics_particle_get_gravity_scale"))
    return particle_configuration(vm,3,0,a,n);
  if(!strcmp(nm,"physics_particle_set_gravity_scale"))
    return particle_configuration(vm,3,1,a,n);
  if(!strcmp(nm,"physics_particle_get_max_count"))
    return particle_configuration(vm,4,0,a,n);
  if(!strcmp(nm,"physics_particle_set_max_count"))
    return particle_configuration(vm,4,1,a,n);
  if(!strcmp(nm,"physics_debug")){
    double enabled=N(a,n,0);
    if(n>0 && isfinite(enabled)) vm->diagnostics.physics_error_output=enabled>=0.5;
    return vundef();
  }
  if(!strcmp(nm,"physics_draw_debug")) return physics_draw_debug(vm);
  if(!strcmp(nm,"physics_test_overlap")) return vreal(physics_overlap(vm,a,n));
  if(!strcmp(nm,"physics_raycast")) return physics_raycast(vm,a,n);
  if(!strcmp(nm,"physics_fixture_create")){
    GmlPhysicsFixture *f=gml_physics_fixture_new(vm,1);
    return vreal(f?(double)f->id:0);
  }
  if(!strcmp(nm,"physics_fixture_delete")){
    GmlPhysicsFixture *f=phys_fixture_find(vm,(int)N(a,n,0));
    if(f && f->bound_inst<0) gml_physics_fixture_drop(f);
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
        if(vm->inst[i].id==(uint32_t)f->bound_inst && physics_target(vm,&vm->inst[i],(int)N(a,n,0))){
          gml_physics_fixture_drop(f);
          break;
        }
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
    if(f && gml_physics_is_chain(f->shape)){
      if(n>=3) fixture_append_chain(vm,f,N(a,n,1),N(a,n,2));
    } else if(f && f->points<GML_PHYS_FIXTURE_POINTS){
      int p=f->points++;
      f->px[p]=N(a,n,1); f->py[p]=N(a,n,2);
    }
    return vreal(0);
  }
  if(!strcmp(nm,"physics_fixture_set_chain_shape")){
    double id=N(a,n,0);
    if(n<2 || !(id>0 && id<=INT_MAX)) return vreal(0);
    GmlPhysicsFixture *f=phys_fixture_find(vm,(int)id);
    if(f && f->bound_inst<0){
      fixture_clear_chain(f);
      f->points=0;
      f->shape=N(a,n,1)!=0?GML_PHYS_FIXTURE_LOOP:GML_PHYS_FIXTURE_CHAIN;
    }
    return vreal(0);
  }
  if(!strcmp(nm,"physics_fixture_set_polygon_shape")){
    GmlPhysicsFixture *f=phys_fixture_find(vm,(int)N(a,n,0));
    if(f){ fixture_clear_chain(f); f->shape=3; f->points=0; }
    return vreal(0);
  }
  if(!strcmp(nm,"physics_fixture_set_edge_shape")){
    GmlPhysicsFixture *f=phys_fixture_find(vm,(int)N(a,n,0));
    if(f){ fixture_clear_chain(f); f->shape=4; f->x1=N(a,n,1); f->y1=N(a,n,2); f->x2=N(a,n,3); f->y2=N(a,n,4); }
    return vreal(0);
  }
  if(!strcmp(nm,"physics_fixture_set_circle_shape")){
    GmlPhysicsFixture *f=phys_fixture_find(vm,(int)N(a,n,0));
    if(f){ fixture_clear_chain(f); f->shape=1; f->radius=N(a,n,1); }
    return vreal(0);
  }
  if(!strcmp(nm,"physics_fixture_set_box_shape")){
    GmlPhysicsFixture *f=phys_fixture_find(vm,(int)N(a,n,0));
    if(f){ fixture_clear_chain(f); f->shape=2; f->w=N(a,n,1); f->h=N(a,n,2); }
    return vreal(0);
  }
  if(!strcmp(nm,"physics_fixture_set_density")){
    GmlPhysicsFixture *f=phys_fixture_find(vm,(int)N(a,n,0));
    if(f){
      f->density=N(a,n,1);
      if(f->bound_inst<0) f->density_set=1;
    }
    return vreal(0);
  }
  if(!strcmp(nm,"physics_fixture_set_friction")){
    GmlPhysicsFixture *f=phys_fixture_find(vm,(int)N(a,n,0)); if(f) f->friction=N(a,n,1); return vreal(0); }
  if(!strcmp(nm,"physics_fixture_set_restitution")){
    GmlPhysicsFixture *f=phys_fixture_find(vm,(int)N(a,n,0)); if(f) f->restitution=N(a,n,1); return vreal(0); }
  if(!strcmp(nm,"physics_fixture_set_linear_damping")){
    GmlPhysicsFixture *f=phys_fixture_find(vm,(int)N(a,n,0)); if(f) f->lin_damp=N(a,n,1); return vreal(0); }
  if(!strcmp(nm,"physics_fixture_set_angular_damping")){
    GmlPhysicsFixture *f=phys_fixture_find(vm,(int)N(a,n,0)); if(f) f->ang_damp=N(a,n,1); return vreal(0); }
  if(!strcmp(nm,"physics_fixture_set_kinematic")){
    double id=N(a,n,0);
    if(n<1 || !(id>0 && id<=INT_MAX)) return vreal(0);
    GmlPhysicsFixture *fixture=phys_fixture_find(vm,(int)id);
    if(fixture && fixture->bound_inst<0) fixture->kinematic=1;
    return vreal(0);
  }
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
  if(!strcmp(nm,"physics_joint_gear_create")){
    double first=N(a,n,2),second=N(a,n,3),ratio=N(a,n,4);
    if(n<5 || !(first>0 && first<=INT_MAX && second>0 && second<=INT_MAX) ||
       !isfinite(ratio) || fabs(ratio)>1e12 || gml_physics_world_scale(vm)<=0) return vreal(-1);
    const GmlPhysicsJoint *j1=gml_physics_find_joint(vm,first);
    const GmlPhysicsJoint *j2=gml_physics_find_joint(vm,second);
    if(!j1 || !j2 || !j1->initialized || !j2->initialized ||
       (j1->type!=GML_PHYS_JOINT_REVOLUTE && j1->type!=GML_PHYS_JOINT_PRISMATIC) ||
       (j2->type!=GML_PHYS_JOINT_REVOLUTE && j2->type!=GML_PHYS_JOINT_PRISMATIC)) return vreal(-1);
    double a_id=N(a,n,0),b_id=N(a,n,1);
    if(a_id==-1 && vm->cur_self) a_id=vm->cur_self->id;
    if(a_id==-2 && vm->cur_other) a_id=vm->cur_other->id;
    if(b_id==-1 && vm->cur_self) b_id=vm->cur_self->id;
    if(b_id==-2 && vm->cur_other) b_id=vm->cur_other->id;
    if(a_id!=j1->b || b_id!=j2->b || a_id==b_id || a_id==j2->a || b_id==j1->a)
      return vreal(-1);
    double pose[12];
    int shared_ground=j1->a==j2->a;
    if(!gml_physics_capture_gear_pose(vm,j1,shared_ground,pose) ||
       !gml_physics_capture_gear_pose(vm,j2,shared_ground,pose+6)) return vreal(-1);
    GmlPhysicsJoint *gear=phys_joint_new(vm,GML_PHYS_JOINT_GEAR);
    if(!gear) return vreal(-1);
    gear->a=a_id; gear->b=b_id; gear->x1=j1->id; gear->x2=j2->id;
    gear->params[0]=ratio;
    memcpy(gear->params+1,pose,sizeof pose);
    gear->value_count=13;
    gear->initialized=1;
    return vreal(gear->id);
  }
  if(!strcmp(nm,"physics_joint_enable_motor")){
    double id=N(a,n,0);
    if(n<2 || !(id>0 && id<=INT_MAX)) return vreal(0);
    GmlPhysicsJoint *j=phys_joint_find_checked(vm,id);
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
    GmlPhysicsJoint *j=phys_joint_find_checked(vm,N(a,n,0));
    if(j) gml_physics_drop_joint(vm,j);
    return vreal(0);
  }
  if(!strcmp(nm,"physics_joint_get_value") || !strcmp(nm,"physics_joint_set_value")){
    int write=!strcmp(nm,"physics_joint_set_value");
    double field=N(a,n,1);
    if(n<(write?3:2) || !(field>=0 && field<GML_JOINT_FIELD_COUNT)) return vreal(0);
    GmlPhysicsJoint *j=phys_joint_find_checked(vm,N(a,n,0));
    if(!j || !j->initialized) return vreal(0);
    if(!write) return vreal(joint_get_value(vm,j,(int)field));
    joint_set_value(vm,j,(int)field,N(a,n,2));
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
    }else gml_physics_report_error(vm,"world scale is outside the supported domain");
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
