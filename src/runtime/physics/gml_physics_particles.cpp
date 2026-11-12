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
    if(i==index || (other.flags&GML_PHYS_PARTICLE_ZOMBIE) || !(other.flags&kinds)) continue;
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

void collide(GmlPhysicsParticle &point,b2World &world,double radius,double density,double dt){
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
  std::free(state->points);std::free(state->bonds);*state={};
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
      double pa=(a.flags&GML_PHYS_PARTICLE_POWDER)?0:std::max(0.0,weights[size_t(pair.a)]-1);
      double pb=(b.flags&GML_PHYS_PARTICLE_POWDER)?0:std::max(0.0,weights[size_t(pair.b)]-1);
      double pressure=0.1*config->radius/dt*std::min(4.0,pa+pb)*pair.weight;
      if(flags&(GML_PHYS_PARTICLE_POWDER|GML_PHYS_PARTICLE_WALL))
        pressure+=0.25*std::max(0.0,diameter-pair.distance)/dt;
      if(flags&GML_PHYS_PARTICLE_TENSILE) pressure-=0.02*config->radius/dt*pair.weight*(1-pair.weight);
      double approach=(b.vx-a.vx)*pair.nx+(b.vy-a.vy)*pair.ny;
      if(approach<0) pressure-=std::min(1.0,config->damping*pair.weight)*approach;
      velocity_pair(a,b,pair.nx*pressure,pair.ny*pressure);
      if(flags&GML_PHYS_PARTICLE_VISCOUS){
        double blend=std::min(1.0,8*dt*pair.weight);
        velocity_pair(a,b,(a.vx-b.vx)*blend,(a.vy-b.vy)*blend);
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
      double relative=(b.vx-a.vx)*x/length+(b.vy-a.vy)*y/length;
      double correction=strength*(bond.length-length)/dt-std::min(1.0,16*dt)*relative;
      velocity_pair(a,b,correction*x/length,correction*y/length);
    }
    for(int i=0;i<state->count;i++){
      auto &point=state->points[i];
      if(!finite_point(point)){point.vx=point.vy=0;continue;}
      if(point.flags&GML_PHYS_PARTICLE_WALL){
        if(world.GetBodyCount()) collide(point,world,config->radius,config->density,dt);
        continue;
      }
      if(world.GetBodyCount()) collide(point,world,config->radius,config->density,dt);
      else {point.x+=point.vx*dt;point.y+=point.vy*dt;}
    }
  }
}
