/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "gml_physics_particles.h"
#include "box2d/box2d.h"
#include "box2d/b2_distance.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <climits>
#include <limits>
#include <vector>

namespace {
template<class T> bool reserve(T *&data,int &capacity,int needed){
  if(needed<=capacity) return true;
  if(needed<0 || size_t(needed)>SIZE_MAX/sizeof(T)) return false;
  int next=capacity>0?capacity:32;
  while(next<needed){ if(next>INT_MAX/2){next=needed;break;}next*=2; }
  if(size_t(next)>SIZE_MAX/sizeof(T)) return false;
  void *replacement=std::realloc(data,size_t(next)*sizeof(T));
  if(!replacement) return false;
  data=static_cast<T *>(replacement);capacity=next;return true;
}

bool finite_point(const GmlPhysicsParticle &p){
  return std::isfinite(p.x) && std::isfinite(p.y) &&
    std::isfinite(p.vx) && std::isfinite(p.vy) &&
    std::fabs(p.x)<=1e6 && std::fabs(p.y)<=1e6 &&
    std::fabs(p.vx)<=1e6 && std::fabs(p.vy)<=1e6;
}

bool connect(GmlPhysicsParticleState &state,int index){
  auto &point=state.points[index];
  uint32_t kinds=point.flags&(GML_PHYS_PARTICLE_SPRING|GML_PHYS_PARTICLE_ELASTIC);
  if(!kinds) return true;
  int nearest[3]={-1,-1,-1};double distances[3]={INFINITY,INFINITY,INFINITY};
  int wanted=(kinds&GML_PHYS_PARTICLE_ELASTIC)?3:1;
  for(int i=0;i<state.count;i++){
    const auto &other=state.points[i];
    if(i==index || other.group!=point.group ||
       (other.flags&GML_PHYS_PARTICLE_ZOMBIE) || !(other.flags&kinds)) continue;
    double length=std::hypot(point.x-other.x,point.y-other.y);
    for(int slot=0;slot<wanted;slot++) if(length<distances[slot]){
      for(int j=wanted-1;j>slot;j--){nearest[j]=nearest[j-1];distances[j]=distances[j-1];}
      nearest[slot]=i;distances[slot]=length;break;
    }
  }
  if(state.bond_count>INT_MAX-wanted ||
     !reserve(state.bonds,state.bond_capacity,state.bond_count+wanted)) return false;
  for(int slot=0;slot<wanted && nearest[slot]>=0;slot++){
    uint32_t a=uint32_t(std::min(index,nearest[slot]));
    uint32_t b=uint32_t(std::max(index,nearest[slot]));
    bool exists=false;
    for(int j=0;j<state.bond_count;j++)
      if(state.bonds[j].first==a && state.bonds[j].second==b){exists=true;break;}
    if(!exists) state.bonds[state.bond_count++]={a,b,kinds&state.points[nearest[slot]].flags,distances[slot]};
  }
  return true;
}

struct Cell { int64_t x,y;int index; };
bool cell_less(const Cell &a,const Cell &b){
  return a.x!=b.x?a.x<b.x:a.y!=b.y?a.y<b.y:a.index<b.index;
}
struct Pair {int a,b;double nx,ny,weight,distance;};

std::vector<Pair> neighbours(const GmlPhysicsParticleState &state,double diameter){
  std::vector<Cell> cells;cells.reserve(size_t(state.count));
  for(int i=0;i<state.count;i++) if(finite_point(state.points[i]))
    cells.push_back({int64_t(std::floor(state.points[i].x/diameter)),
                     int64_t(std::floor(state.points[i].y/diameter)),i});
  std::sort(cells.begin(),cells.end(),cell_less);
  std::vector<Pair> pairs;
  for(int i=0;i<state.count;i++){
    const auto &a=state.points[i];if(!finite_point(a)) continue;
    int64_t cx=int64_t(std::floor(a.x/diameter)),cy=int64_t(std::floor(a.y/diameter));
    for(int dx=-1;dx<=1;dx++) for(int dy=-1;dy<=1;dy++){
      Cell key={cx+dx,cy+dy,-1};
      auto begin=std::lower_bound(cells.begin(),cells.end(),key,cell_less);
      for(auto it=begin;it!=cells.end() && it->x==key.x && it->y==key.y;it++){
        int j=it->index;if(j<=i) continue;
        const auto &b=state.points[j];double x=b.x-a.x,y=b.y-a.y,length=std::hypot(x,y);
        if(length>=diameter) continue;
        pairs.push_back({i,j,length>0?x/length:1,length>0?y/length:0,1-length/diameter,length});
      }
    }
  }
  return pairs;
}

void velocity_pair(GmlPhysicsParticle &a,GmlPhysicsParticle &b,double dx,double dy){
  double ia=(a.flags&GML_PHYS_PARTICLE_WALL)?0:1;
  double ib=(b.flags&GML_PHYS_PARTICLE_WALL)?0:1;
  double total=ia+ib;if(total==0) return;
  if(ia){a.vx-=dx/total;a.vy-=dy/total;}
  if(ib){b.vx+=dx/total;b.vy+=dy/total;}
}

uint32_t mix_colour(uint32_t a,uint32_t b,double fraction){
  uint32_t result=0;
  for(unsigned shift=0;shift<32;shift+=8){
    double value=double((a>>shift)&255u)*(1-fraction)+double((b>>shift)&255u)*fraction;
    result|=uint32_t(std::lround(value))<<shift;
  }
  return result;
}

struct Nearby : b2QueryCallback {
  std::vector<b2Fixture *> fixtures;
  bool ReportFixture(b2Fixture *fixture) override {
    if(!fixture->IsSensor()) fixtures.push_back(fixture);
    return true;
  }
};

bool overlap(b2Fixture &fixture,const b2CircleShape &circle,const b2Transform &position,
             b2Vec2 &normal,float &separation,b2Vec2 &contact){
  b2Manifold manifold{};const b2Shape *shape=fixture.GetShape();
  const b2Transform &transform=fixture.GetBody()->GetTransform();
  switch(shape->GetType()){
    case b2Shape::e_circle:
      b2CollideCircles(&manifold,static_cast<const b2CircleShape *>(shape),transform,&circle,position);break;
    case b2Shape::e_polygon:
      b2CollidePolygonAndCircle(&manifold,static_cast<const b2PolygonShape *>(shape),transform,&circle,position);break;
    case b2Shape::e_edge:
      b2CollideEdgeAndCircle(&manifold,static_cast<const b2EdgeShape *>(shape),transform,&circle,position);break;
    default:return false;
  }
  if(!manifold.pointCount) return false;
  b2WorldManifold world;world.Initialize(&manifold,transform,shape->m_radius,position,circle.m_radius);
  normal=world.normal;separation=world.separations[0];contact=world.points[0];return true;
}

void collide(GmlPhysicsParticle &point,b2World &world,double radius,double density,double dt,
             double solid_depth=0){
  b2CircleShape circle;circle.m_radius=float(radius);circle.m_p.SetZero();
  b2Vec2 position(float(point.x),float(point.y));
  b2Vec2 velocity(float(point.vx),float(point.vy));
  double remaining=dt;
  const bool wall=(point.flags&GML_PHYS_PARTICLE_WALL)!=0;
  bool touched=false;
  for(int pass=0;pass<4 && remaining>0;pass++){
    b2Vec2 end=position+float(remaining)*velocity;
    b2Vec2 padding{float(radius),float(radius)};b2AABB bounds;
    bounds.lowerBound=b2Min(position,end)-padding;bounds.upperBound=b2Max(position,end)+padding;
    Nearby query;world.QueryAABB(&query,bounds);
    b2Fixture *selected=nullptr;b2Vec2 normal{},contact{};float separation=0,fraction=1;
    bool penetrated=false;
    b2Transform start;start.Set(position,0);
    for(b2Fixture *fixture:query.fixtures){
      b2Vec2 candidate_normal,candidate_contact;float candidate_separation;
      if(overlap(*fixture,circle,start,candidate_normal,candidate_separation,candidate_contact) &&
         candidate_separation<0){
        if(!penetrated || candidate_separation<separation){
          selected=fixture;normal=candidate_normal;contact=candidate_contact;
          separation=candidate_separation;fraction=0;penetrated=true;
        }
      }else if(!penetrated){
        b2ShapeCastInput input;input.proxyA.Set(fixture->GetShape(),0);input.proxyB.Set(&circle,0);
        input.transformA=fixture->GetBody()->GetTransform();input.transformB=start;
        input.translationB=end-position;b2ShapeCastOutput result;
        if(b2ShapeCast(&result,&input) && result.lambda<fraction){
          selected=fixture;fraction=result.lambda;normal=result.normal;contact=result.point;separation=0;
        }
      }
    }
    if(!selected){
      if(!touched){point.x+=point.vx*dt;point.y+=point.vy*dt;return;}
      position=end;remaining=0;break;
    }
    touched=true;
    if(penetrated && !wall) position+=(-separation+float(radius*1e-5))*normal;
    else position+=fraction*(end-position);
    remaining*=1-fraction;
    b2Body *body=selected->GetBody();
    b2Vec2 relative=velocity-body->GetLinearVelocityFromWorldPoint(contact);
    float approach=b2Dot(relative,normal);
    if(penetrated && solid_depth>0)
      approach-=float(std::min(radius,solid_depth)*0.2/dt);
    if(approach<0){
      double mass=density*2.25*radius*radius;
      double inverse=wall?0:1/mass;
      double body_inverse=body->GetType()==b2_dynamicBody && body->GetMass()>0?1/body->GetMass():0;
      double arm=b2Cross(contact-body->GetWorldCenter(),normal);
      b2Vec2 local_center=body->GetLocalCenter();
      double inertia=body->GetInertia()-body->GetMass()*b2Dot(local_center,local_center);
      double inverse_inertia=!body_inverse || body->IsFixedRotation() || inertia<=0?0:1/inertia;
      double angular=arm*arm*inverse_inertia;
      double restitution=selected->GetRestitution();
      if(point.flags&GML_PHYS_PARTICLE_ELASTIC) restitution=std::max(restitution,0.5);
      double denominator=inverse+body_inverse+angular;
      if(denominator<=0) break;
      double impulse=-(1+restitution)*approach/denominator;
      b2Vec2 force=float(impulse)*normal;
      if(inverse) velocity+=float(inverse)*force;
      if(body_inverse) body->ApplyLinearImpulse(-force,contact,true);
      b2Vec2 tangent{-normal.y,normal.x};
      double tangent_arm=b2Cross(contact-body->GetWorldCenter(),tangent);
      double tangent_inverse=inverse+body_inverse+tangent_arm*tangent_arm*inverse_inertia;
      if(tangent_inverse>0){
        b2Vec2 after=velocity-body->GetLinearVelocityFromWorldPoint(contact);
        double tangential=-b2Dot(after,tangent)/tangent_inverse;
        double limit=std::max(0.0,double(selected->GetFriction()))*impulse;
        tangential=std::max(-limit,std::min(limit,tangential));
        b2Vec2 friction=float(tangential)*tangent;
        if(inverse) velocity+=float(inverse)*friction;
        if(body_inverse) body->ApplyLinearImpulse(-friction,contact,true);
      }
    }
    if(wall){
      if(penetrated && body->GetType()==b2_dynamicBody)
        body->SetTransform(body->GetPosition()+separation*normal,body->GetAngle());
      return;
    }
    if(fraction==0 && !penetrated) break;
  }

  point.x=position.x;point.y=position.y;point.vx=velocity.x;point.vy=velocity.y;
}
}

extern "C" int gml_physics_particles_config_valid(const GmlPhysicsParticleConfig *config){
  if(!config) return 0;
  double mass=config->density*2.25*config->radius*config->radius;
  return std::isfinite(float(mass)) && float(mass)>0 && std::isfinite(float(1/mass)) && std::isfinite(config->radius) && config->radius>b2_epsilon && config->radius<=1e6 &&
    std::isfinite(config->density) && config->density>0 && config->density<=1e6 &&
    std::isfinite(config->damping) && config->damping>=0 &&
    std::isfinite(config->gravity_scale) && std::fabs(config->gravity_scale)<=1e6;
}

extern "C" void gml_physics_particles_clear(GmlPhysicsParticleState *state){
  if(!state) return;
  std::free(state->points);std::free(state->bonds);std::free(state->groups);*state={};
}

extern "C" int gml_physics_particles_add(GmlPhysicsParticleState *state,
                                         const GmlPhysicsParticleConfig *config,
                                         GmlPhysicsParticle particle){
  if(!state || !gml_physics_particles_config_valid(config) || !finite_point(particle) ||
     state->count==INT_MAX || (config->max_count>0 && state->count>=config->max_count) ||
     !reserve(state->points,state->capacity,state->count+1)) return -1;
  if(particle.flags&GML_PHYS_PARTICLE_WALL) particle.vx=particle.vy=0;
  int index=state->count++;state->points[index]=particle;
  if(!connect(*state,index)){state->count--;return -1;}
  return index;
}

extern "C" int gml_physics_particles_flags(GmlPhysicsParticleState *state,int index,uint32_t flags){
  if(!state || index<0 || index>=state->count) return 0;
  uint32_t original=state->points[index].flags;state->points[index].flags=flags;
  if(!connect(*state,index)){state->points[index].flags=original;return 0;}
  if(flags&GML_PHYS_PARTICLE_WALL) state->points[index].vx=state->points[index].vy=0;
  return 1;
}

extern "C" void gml_physics_particles_compact(GmlPhysicsParticleState *state){
  if(!state || !state->count) return;
  bool zombies=false;
  for(int i=0;i<state->count;i++) zombies|=(state->points[i].flags&GML_PHYS_PARTICLE_ZOMBIE)!=0;
  if(!zombies) return;
  std::vector<int> indices(size_t(state->count),-1);int live=0;
  for(int i=0;i<state->count;i++) if(!(state->points[i].flags&GML_PHYS_PARTICLE_ZOMBIE)){
    indices[size_t(i)]=live;state->points[live++]=state->points[i];
  }
  int bonds=0;
  for(int i=0;i<state->bond_count;i++){
    auto bond=state->bonds[i];int a=indices[bond.first],b=indices[bond.second];
    if(a<0 || b<0) continue;
    bond.first=uint32_t(a);bond.second=uint32_t(b);state->bonds[bonds++]=bond;
  }
  state->count=live;state->bond_count=bonds;
  std::vector<unsigned char> present(size_t(state->group_count),0);
  for(int i=0;i<live;i++){
    auto *group=gml_physics_particle_group_find(state,state->points[i].group);
    if(group) present[size_t(group-state->groups)]=1;
  }
  int groups=0;
  for(int i=0;i<state->group_count;i++) if(present[size_t(i)]) state->groups[groups++]=state->groups[i];
  state->group_count=groups;
}

extern "C" GmlPhysicsParticleGroup *gml_physics_particle_group_find(GmlPhysicsParticleState *state,uint32_t id){
  if(!state || !id || !state->group_count) return nullptr;
  auto *end=state->groups+state->group_count;
  auto *group=std::lower_bound(state->groups,end,id,
    [](const GmlPhysicsParticleGroup &candidate,uint32_t key){return candidate.id<key;});
  return group!=end && group->id==id?group:nullptr;
}

extern "C" void gml_physics_particle_group_statistics(const GmlPhysicsParticleState *state,
    const GmlPhysicsParticleConfig *config,uint32_t id,GmlPhysicsParticleGroupStatistics *out){
  *out={};
  for(int i=0;i<state->count;i++) if(state->points[i].group==id){
    const auto &p=state->points[i];out->count++;
    out->cx+=p.x;out->cy+=p.y;out->vx+=p.vx;out->vy+=p.vy;
  }
  if(!out->count) return;
  out->cx/=out->count;out->cy/=out->count;out->vx/=out->count;out->vy/=out->count;
  const double mass=config->density*2.25*config->radius*config->radius;
  out->mass=mass*out->count;
  for(int i=0;i<state->count;i++) if(state->points[i].group==id){
    const auto &p=state->points[i];double x=p.x-out->cx,y=p.y-out->cy;
    out->inertia+=mass*(x*x+y*y);
    out->omega+=mass*(x*(p.vy-out->vy)-y*(p.vx-out->vx));
  }
  out->omega=out->inertia>0?out->omega/out->inertia:0;
}

extern "C" void gml_physics_particle_group_flags(GmlPhysicsParticleState *state,uint32_t id,uint32_t flags){
  auto *group=gml_physics_particle_group_find(state,id);if(!group) return;
  if((flags&GML_PHYS_GROUP_RIGID) && !(group->flags&GML_PHYS_GROUP_RIGID)){
    double c=std::cos(group->angle),s=std::sin(group->angle);
    for(int i=0;i<state->count;i++) if(state->points[i].group==id){
      auto &p=state->points[i];double x=p.x-group->x,y=p.y-group->y;
      p.local_x=c*x+s*y;p.local_y=-s*x+c*y;
    }
  }
  group->flags=flags;
}

namespace {
bool connect_group(GmlPhysicsParticleState &state,double radius,uint32_t id){
  std::vector<uint64_t> existing;existing.reserve(size_t(state.bond_count));
  for(int i=0;i<state.bond_count;i++) existing.push_back(
    (uint64_t(state.bonds[i].first)<<32)|state.bonds[i].second);
  std::sort(existing.begin(),existing.end());std::vector<GmlPhysicsParticleBond> additions;
  for(const auto &pair:neighbours(state,2*radius)){
    const auto &a=state.points[pair.a];const auto &b=state.points[pair.b];
    uint32_t flags=a.flags&b.flags&(GML_PHYS_PARTICLE_SPRING|GML_PHYS_PARTICLE_ELASTIC);
    if(a.group!=id || b.group!=id || !flags) continue;
    uint64_t key=(uint64_t(uint32_t(pair.a))<<32)|uint32_t(pair.b);
    if(!std::binary_search(existing.begin(),existing.end(),key))
      additions.push_back({uint32_t(pair.a),uint32_t(pair.b),flags,pair.distance});
  }
  if(additions.size()>size_t(INT_MAX-state.bond_count) ||
     !reserve(state.bonds,state.bond_capacity,state.bond_count+int(additions.size()))) return false;
  for(const auto &bond:additions) state.bonds[state.bond_count++]=bond;
  return true;
}
}

extern "C" int gml_physics_particle_group_create(GmlPhysicsParticleState *state,
                                                 const GmlPhysicsParticleConfig *config){
  if(!state) return -1;
  auto definition=state->definition;state->definition={};
  if(!definition.active || !gml_physics_particles_config_valid(config) ||
     !definition.category || definition.shape<1 || definition.shape>3 ||
     state->next_group>uint32_t(INT_MAX) || state->group_count==INT_MAX) return -1;
  const double values[]={definition.x,definition.y,definition.angle,definition.vx,definition.vy,
                         definition.omega,definition.strength,definition.rx,definition.ry};
  for(double value:values) if(!std::isfinite(value)) return -1;
  if(definition.strength<0 || definition.strength>1) return -1;
  double low_x=0,low_y=0,high_x=0,high_y=0;
  b2PolygonShape polygon;
  if(definition.shape==3){
    if(definition.count<3 || definition.count>8) return -1;
    b2Vec2 vertices[8];int unique=0;
    for(int i=0;i<definition.count;i++){
      if(!std::isfinite(definition.px[i]) || !std::isfinite(definition.py[i]) ||
         std::fabs(definition.px[i])>1e6 || std::fabs(definition.py[i])>1e6) return -1;
      b2Vec2 v{float(definition.px[i]),float(definition.py[i])};bool repeated=false;
      for(int j=0;j<unique;j++)
        if(b2DistanceSquared(v,vertices[j])<(0.5f*b2_linearSlop)*(0.5f*b2_linearSlop)) repeated=true;
      if(!repeated) vertices[unique++]=v;
    }
    float area=0;
    for(int a=0;a<unique;a++) for(int b=a+1;b<unique;b++) for(int k=b+1;k<unique;k++)
      area=std::max(area,std::fabs(b2Cross(vertices[b]-vertices[a],vertices[k]-vertices[a])));
    if(unique<3 || area<=2*b2_epsilon) return -1;
    polygon.Set(vertices,unique);
    low_x=high_x=polygon.m_vertices[0].x;low_y=high_y=polygon.m_vertices[0].y;
    for(int i=1;i<polygon.m_count;i++){
      low_x=std::min(low_x,double(polygon.m_vertices[i].x));high_x=std::max(high_x,double(polygon.m_vertices[i].x));
      low_y=std::min(low_y,double(polygon.m_vertices[i].y));high_y=std::max(high_y,double(polygon.m_vertices[i].y));
    }
  }else{
    if(!std::isfinite(definition.rx) || definition.rx<=0 ||
       (definition.shape==2 && (!std::isfinite(definition.ry) || definition.ry<=0))) return -1;
    high_x=definition.rx;high_y=definition.shape==1?definition.rx:definition.ry;
    low_x=-high_x;low_y=-high_y;
  }
  const double stride=1.5*config->radius;
  double min_x=std::ceil(low_x/stride),min_y=std::ceil(low_y/stride);
  double max_x=std::floor(high_x/stride),max_y=std::floor(high_y/stride);
  if(definition.shape==1){max_x=std::ceil(high_x/stride)-1;max_y=std::ceil(high_y/stride)-1;}
  double nx=max_x-min_x+1,ny=max_y-min_y+1;
  /* The bound is on rasterization work, including rejected sites of a thin hull. */
  if(!std::isfinite(nx) || !std::isfinite(ny) || nx<1 || ny<1 ||
     nx*ny>1048576 || std::fabs(min_x)>double(INT64_MAX)/2 ||
     std::fabs(min_y)>double(INT64_MAX)/2 || std::fabs(max_x)>double(INT64_MAX)/2 ||
     std::fabs(max_y)>double(INT64_MAX)/2) return -1;
  uint32_t id=state->next_group?state->next_group:1;
  double c=std::cos(definition.angle),s=std::sin(definition.angle);
  std::vector<GmlPhysicsParticle> points;
  for(int64_t iy=int64_t(min_y);iy<=int64_t(max_y);iy++)
    for(int64_t ix=int64_t(min_x);ix<=int64_t(max_x);ix++){
      double x=double(ix)*stride,y=double(iy)*stride,depth=0;
      if(definition.shape==1){depth=definition.rx-std::hypot(x,y);if(depth<0) continue;}
      else if(definition.shape==2) depth=std::min(definition.rx-std::fabs(x),definition.ry-std::fabs(y));
      else{
        depth=INFINITY;
        for(int k=0;k<polygon.m_count;k++){
          const auto &v=polygon.m_vertices[k];const auto &normal=polygon.m_normals[k];
          depth=std::min(depth,-((x-v.x)*normal.x+(y-v.y)*normal.y));
        }
        if(depth<0) continue;
      }
      double rx=c*x-s*y,ry=s*x+c*y;
      GmlPhysicsParticle p={definition.x+rx,definition.y+ry,
        definition.vx-definition.omega*ry,definition.vy+definition.omega*rx,
        definition.flags,definition.colour,definition.category,id,x,y,depth};
      if(p.flags&GML_PHYS_PARTICLE_WALL) p.vx=p.vy=0;
      if(!finite_point(p) || points.size()>=size_t(INT_MAX-state->count) ||
         (config->max_count>0 && points.size()>=size_t(std::max(0,config->max_count-state->count)))) return -1;
      points.push_back(p);
    }
  if(points.empty() || !reserve(state->groups,state->group_capacity,state->group_count+1) ||
     !reserve(state->points,state->capacity,state->count+int(points.size()))) return -1;
  int previous=state->count,previous_bonds=state->bond_count;
  for(const auto &point:points) state->points[state->count++]=point;
  if(!connect_group(*state,config->radius,id)){
    state->count=previous;state->bond_count=previous_bonds;return -1;
  }
  state->groups[state->group_count++]={id,definition.group_flags,definition.x,definition.y,
                                     definition.angle,definition.strength};
  state->next_group=id+1;return int(id);
}

extern "C" int gml_physics_particle_group_join(GmlPhysicsParticleState *state,
    const GmlPhysicsParticleConfig *config,uint32_t to,uint32_t from){
  auto *a=gml_physics_particle_group_find(state,to);auto *b=gml_physics_particle_group_find(state,from);
  if(!a || !b || a==b) return 0;
  /* Keep the source identities until all new connection allocation has succeeded. */
  std::vector<int> source;
  for(int i=0;i<state->count;i++) if(state->points[i].group==from) source.push_back(i);
  for(int i:source) state->points[i].group=to;
  int old_bonds=state->bond_count;
  if(!connect_group(*state,config->radius,to)){
    for(int i:source) state->points[i].group=from;
    state->bond_count=old_bonds;
    return 0;
  }
  a->flags|=b->flags;
  double c=std::cos(a->angle),s=std::sin(a->angle);
  for(int i=0;i<state->count;i++) if(state->points[i].group==to){
    auto &p=state->points[i];double x=p.x-a->x,y=p.y-a->y;
    p.local_x=c*x+s*y;p.local_y=-s*x+c*y;
  }
  int index=int(b-state->groups);
  for(int i=index+1;i<state->group_count;i++) state->groups[i-1]=state->groups[i];
  state->group_count--;return 1;
}

namespace {
struct RigidPoint {
  int index;
  double chord_x,chord_y,tangent_x,tangent_y;
};
struct RigidMotion {
  uint32_t id;
  double local_x=0,local_y=0,angle=0;
  bool fixed=false,rigid=false,elastic=false;
  std::vector<RigidPoint> points;
};
std::vector<RigidMotion> groups_prepare(GmlPhysicsParticleState &state,double dt){
  std::vector<RigidMotion> motions;
  if(!state.group_count) return motions;
  std::vector<int> indices(size_t(state.group_count),-1);
  std::vector<unsigned char> elastic(size_t(state.group_count),0);
  for(int i=0;i<state.count;i++) if(state.points[i].flags&GML_PHYS_PARTICLE_ELASTIC){
    auto *group=gml_physics_particle_group_find(&state,state.points[i].group);
    if(group) elastic[size_t(group-state.groups)]=1;
  }
  for(int i=0;i<state.group_count;i++) if((state.groups[i].flags&GML_PHYS_GROUP_RIGID) || elastic[size_t(i)]){
    indices[size_t(i)]=int(motions.size());RigidMotion motion;motion.id=state.groups[i].id;
    motion.rigid=(state.groups[i].flags&GML_PHYS_GROUP_RIGID)!=0;
    motion.elastic=elastic[size_t(i)]!=0;
    motions.push_back(std::move(motion));
  }
  if(motions.empty()) return motions;
  for(int i=0;i<state.count;i++){
    auto *group=gml_physics_particle_group_find(&state,state.points[i].group);
    if(!group) continue;
    int index=indices[size_t(group-state.groups)];
    if(index>=0){
      auto &motion=motions[size_t(index)];motion.points.push_back({i,0,0,0,0});
      motion.elastic|=(state.points[i].flags&GML_PHYS_PARTICLE_ELASTIC)!=0;
    }
  }
  for(auto &motion:motions){
    if(motion.points.empty() || (!motion.rigid && !motion.elastic)) continue;
    auto *group=gml_physics_particle_group_find(&state,motion.id);
    double cx=0,cy=0,vx=0,vy=0,omega=0,inertia=0,count=double(motion.points.size());
    for(const auto &entry:motion.points){
      const auto &p=state.points[entry.index];cx+=p.x;cy+=p.y;vx+=p.vx;vy+=p.vy;
      motion.local_x+=p.local_x;motion.local_y+=p.local_y;
      motion.fixed|=(p.flags&GML_PHYS_PARTICLE_WALL)!=0;
    }
    cx/=count;cy/=count;vx/=count;vy/=count;motion.local_x/=count;motion.local_y/=count;
    for(const auto &entry:motion.points){
      const auto &p=state.points[entry.index];double x=p.x-cx,y=p.y-cy;
      inertia+=x*x+y*y;omega+=x*(p.vy-vy)-y*(p.vx-vx);
    }
    omega=inertia>0?omega/inertia:0;
    if(motion.fixed) vx=vy=omega=0;
    motion.angle=group->angle+(motion.rigid?omega*dt:0);
    if(!motion.rigid) continue;
    double c=std::cos(motion.angle),s=std::sin(motion.angle);
    for(auto &entry:motion.points){
      auto &p=state.points[entry.index];double lx=p.local_x-motion.local_x,ly=p.local_y-motion.local_y;
      double x=c*lx-s*ly,y=s*lx+c*ly;
      entry.chord_x=(cx+vx*dt+x-p.x)/dt;entry.chord_y=(cy+vy*dt+y-p.y)/dt;
      entry.tangent_x=vx-omega*y;entry.tangent_y=vy+omega*x;
      p.vx=entry.chord_x;p.vy=entry.chord_y;
    }
  }
  return motions;
}
void groups_finish(GmlPhysicsParticleState &state,std::vector<RigidMotion> &motions,double dt){
  for(auto &motion:motions){
    if(motion.points.empty() || (!motion.rigid && !motion.elastic)) continue;
    auto *group=gml_physics_particle_group_find(&state,motion.id);
    double cx=0,cy=0,vx=0,vy=0,count=double(motion.points.size()),dot=0,cross=0;
    for(const auto &entry:motion.points){
      auto &p=state.points[entry.index];cx+=p.x;cy+=p.y;
      p.vx=entry.tangent_x+(p.vx-entry.chord_x);
      p.vy=entry.tangent_y+(p.vy-entry.chord_y);
      vx+=p.vx;vy+=p.vy;
    }
    cx/=count;cy/=count;vx/=count;vy/=count;
    for(const auto &entry:motion.points){
      const auto &p=state.points[entry.index];double x=p.local_x-motion.local_x,y=p.local_y-motion.local_y;
      dot+=x*(p.x-cx)+y*(p.y-cy);cross+=x*(p.y-cy)-y*(p.x-cx);
    }
    const double turn=6.2831853071795864769;
    double angle=motion.angle;
    if(dot!=0 || cross!=0) angle+=std::remainder(std::atan2(cross,dot)-angle,turn);
    double c=std::cos(angle),s=std::sin(angle),omega=0,inertia=0;
    if(!motion.rigid){
      double blend=std::min(1.0,16*dt*group->strength);
      for(const auto &entry:motion.points){
        auto &p=state.points[entry.index];
        if(!(p.flags&GML_PHYS_PARTICLE_ELASTIC) || (p.flags&GML_PHYS_PARTICLE_WALL)) continue;
        double x=p.local_x-motion.local_x,y=p.local_y-motion.local_y;
        double dx=blend*(cx+c*x-s*y-p.x),dy=blend*(cy+s*x+c*y-p.y);
        p.x+=dx;p.y+=dy;p.vx+=dx/dt;p.vy+=dy/dt;
      }
      continue;
    }
    group->angle=angle;group->x=cx-c*motion.local_x+s*motion.local_y;
    group->y=cy-s*motion.local_x-c*motion.local_y;
    for(const auto &entry:motion.points){
      auto &p=state.points[entry.index];double x=p.local_x-motion.local_x,y=p.local_y-motion.local_y;
      p.x=cx+c*x-s*y;p.y=cy+s*x+c*y;
      x=p.x-cx;y=p.y-cy;inertia+=x*x+y*y;
      omega+=x*(p.vy-vy)-y*(p.vx-vx);
    }
    omega=inertia>0?omega/inertia:0;
    if(motion.fixed) vx=vy=omega=0;
    for(const auto &entry:motion.points){
      auto &p=state.points[entry.index];p.vx=vx-omega*(p.y-cy);p.vy=vy+omega*(p.x-cx);
    }
  }
}
}

void gml_physics_particles_step(GmlPhysicsParticleState *state,
                                const GmlPhysicsParticleConfig *config,
                                b2World &world,double gx,double gy,double seconds,int iterations){
  if(!state || !gml_physics_particles_config_valid(config) || !std::isfinite(seconds) || seconds<=0) return;
  gml_physics_particles_compact(state);
  if(!state->count) return;
  const double diameter=2*config->radius;
  iterations=std::max(1,std::min(32,iterations));
  double speed=0;
  for(int i=0;i<state->count;i++)
    if(!(state->points[i].flags&GML_PHYS_PARTICLE_WALL))
      speed=std::max(speed,std::hypot(state->points[i].vx,state->points[i].vy));
  speed+=std::hypot(gx,gy)*std::fabs(config->gravity_scale)*seconds;
  double travel=speed*seconds/(config->radius*0.5);
  iterations=std::max(iterations,travel>=32?32:int(std::ceil(travel)));
  double dt=seconds/iterations;
  for(int step=0;step<iterations;step++){
    for(int i=0;i<state->count;i++){
      auto &point=state->points[i];if(point.flags&GML_PHYS_PARTICLE_WALL) continue;
      point.vx+=gx*config->gravity_scale*dt;
      point.vy+=gy*config->gravity_scale*dt;
    }
    auto pairs=neighbours(*state,diameter);std::vector<double> weights(size_t(state->count),0);
    for(const auto &pair:pairs){weights[size_t(pair.a)]+=pair.weight;weights[size_t(pair.b)]+=pair.weight;}
    for(const auto &pair:pairs){
      auto &a=state->points[pair.a];auto &b=state->points[pair.b];uint32_t flags=a.flags|b.flags;
      auto *ga=gml_physics_particle_group_find(state,a.group);
      auto *gb=gml_physics_particle_group_find(state,b.group);
      bool rigid_pair=ga && ga==gb && (ga->flags&GML_PHYS_GROUP_RIGID);
      if(!rigid_pair){
      double pa=(a.flags&GML_PHYS_PARTICLE_POWDER)?0:std::max(0.0,weights[size_t(pair.a)]-1);
      double pb=(b.flags&GML_PHYS_PARTICLE_POWDER)?0:std::max(0.0,weights[size_t(pair.b)]-1);
      double pressure=0.1*config->radius/dt*std::min(4.0,pa+pb)*pair.weight;
      if(flags&(GML_PHYS_PARTICLE_POWDER|GML_PHYS_PARTICLE_WALL))
        pressure+=0.25*std::max(0.0,diameter-pair.distance)/dt;
      if(flags&GML_PHYS_PARTICLE_TENSILE) pressure-=0.02*config->radius/dt*pair.weight*(1-pair.weight);
      if(a.group!=b.group){
        double depth=ga && (ga->flags&GML_PHYS_GROUP_SOLID)?a.depth:0;
        if(gb && (gb->flags&GML_PHYS_GROUP_SOLID)) depth+=b.depth;
        pressure+=0.2*std::min(depth,diameter)/dt*pair.weight;
      }
      double approach=(b.vx-a.vx)*pair.nx+(b.vy-a.vy)*pair.ny;
      if(approach<0) pressure-=std::min(1.0,config->damping*pair.weight)*approach;
      velocity_pair(a,b,pair.nx*pressure,pair.ny*pressure);
      if(flags&GML_PHYS_PARTICLE_VISCOUS){
        double blend=std::min(1.0,8*dt*pair.weight);
        velocity_pair(a,b,(a.vx-b.vx)*blend,(a.vy-b.vy)*blend);
      }
      }
      if(step==0 && (a.flags&b.flags&GML_PHYS_PARTICLE_COLOUR)){
        uint32_t ca=a.colour,cb=b.colour;double blend=std::min(0.5,4*seconds*pair.weight);
        if(a.flags&GML_PHYS_PARTICLE_COLOUR) a.colour=mix_colour(ca,cb,blend);
        if(b.flags&GML_PHYS_PARTICLE_COLOUR) b.colour=mix_colour(cb,ca,blend);
      }
    }
    for(int i=0;i<state->bond_count;i++){
      const auto &bond=state->bonds[i];auto &a=state->points[bond.first];auto &b=state->points[bond.second];
      if(!(a.flags&b.flags&bond.flags)) continue;
      double x=b.x-a.x,y=b.y-a.y,length=std::hypot(x,y);if(length<=0) continue;
      double strength=(bond.flags&GML_PHYS_PARTICLE_ELASTIC)?0.4:0.2;
      if(a.group && a.group==b.group){
        auto *group=gml_physics_particle_group_find(state,a.group);
        if(group) strength*=group->strength;
      }
      double relative=(b.vx-a.vx)*x/length+(b.vy-a.vy)*y/length;
      double correction=strength*(bond.length-length)/dt-std::min(1.0,16*dt)*relative;
      velocity_pair(a,b,correction*x/length,correction*y/length);
    }
    auto motions=groups_prepare(*state,dt);
    for(int i=0;i<state->count;i++){
      auto &point=state->points[i];
      if(!finite_point(point)){point.vx=point.vy=0;continue;}
      auto *group=gml_physics_particle_group_find(state,point.group);
      double solid_depth=group && (group->flags&GML_PHYS_GROUP_SOLID)?point.depth:0;
      if(point.flags&GML_PHYS_PARTICLE_WALL){
        if(world.GetBodyCount()) collide(point,world,config->radius,config->density,dt,solid_depth);
        continue;
      }
      if(world.GetBodyCount()) collide(point,world,config->radius,config->density,dt,solid_depth);
      else {point.x+=point.vx*dt;point.y+=point.vy*dt;}
    }
    groups_finish(*state,motions,dt);
  }
}
