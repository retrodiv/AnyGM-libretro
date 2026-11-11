/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "gml_physics_solver.h"
#include "box2d/box2d.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <utility>
#include <vector>

namespace {
const double radians = 0.017453292519943295;

bool bounded(double x) { return std::isfinite(x) && std::fabs(x) <= 1e12; }
bool spatial(double x,double scale) { return bounded(x) && std::fabs(x*scale)<=1e6; }
float nonnegative(double x) { return bounded(x) && x > 0 ? float(x) : 0.0f; }

int fixture_children(const GmlPhysicsFixture &fixture) {
  if (!gml_physics_is_chain(fixture.shape)) return 1;
  if (!fixture.chain || fixture.points<2) return 0;
  return fixture.shape==GML_PHYS_FIXTURE_LOOP?fixture.points:fixture.points-1;
}

struct Shape {
  b2CircleShape circle;
  b2PolygonShape polygon;
  b2EdgeShape edge;

  const b2Shape *segment(double x1,double y1,double x2,double y2,double scale) {
    if (!spatial(x1,scale) || !spatial(y1,scale) || !spatial(x2,scale) || !spatial(y2,scale))
      return nullptr;
    b2Vec2 a(float(x1*scale),float(y1*scale)),b(float(x2*scale),float(y2*scale));
    if ((a-b).LengthSquared()<=b2_epsilon*b2_epsilon) return nullptr;
    edge.SetTwoSided(a,b);
    return &edge;
  }

  const b2Shape *make(const GmlPhysicsFixture &f,double scale,int child=0) {
    if (!bounded(f.density) || f.density>1e6) return nullptr;
    if (gml_physics_is_chain(f.shape)) {
      const GmlPhysicsPoint &a=f.chain[child],&b=f.chain[(child+1)%f.points];
      return segment(a.x,a.y,b.x,b.y,scale);
    }
    /* Bound converted geometry and density before the single-precision mass integrals.
     * Finite doubles alone can still overflow those integrals into invalid native bodies. */
    const double dimensions[]={f.radius,f.w,f.h,f.x1,f.y1,f.x2,f.y2};
    for (double coordinate:dimensions)
      if (!bounded(coordinate) || std::fabs(coordinate*scale)>1e6) return nullptr;
    if (f.shape == 1 && f.radius*scale > b2_epsilon) {
      circle.m_radius = float(f.radius * scale);
      circle.m_p.SetZero();
      return &circle;
    }
    if (f.shape == 2 && f.w*scale > b2_epsilon && f.h*scale > b2_epsilon) {
      polygon.SetAsBox(float(f.w * scale), float(f.h * scale));
      return &polygon;
    }
    if (f.shape == 4) return segment(f.x1,f.y1,f.x2,f.y2,scale);
    if (f.shape == 3 && f.points >= 3 && f.points <= GML_PHYS_FIXTURE_POINTS) {
      /* Set computes a convex hull, but requires at least three distinct, non-collinear
       * points. Validate those preconditions before entering the vendor assertion boundary. */
      b2Vec2 points[b2_maxPolygonVertices];
      int count=0;
      for (int i=0;i<f.points;i++) {
        if (!bounded(f.px[i]) || !bounded(f.py[i]) ||
            std::fabs(f.px[i]*scale)>1e6 || std::fabs(f.py[i]*scale)>1e6) return nullptr;
        b2Vec2 p(float(f.px[i]*scale),float(f.py[i]*scale));
        bool duplicate=false;
        for (int k=0;k<count;k++)
          if ((p-points[k]).LengthSquared() < 0.25f*b2_linearSlop*b2_linearSlop)
            duplicate=true;
        if (!duplicate) {
          if (count==b2_maxPolygonVertices) return nullptr;
          points[count++]=p;
        }
      }
      if (count<3) return nullptr;
      float area=0;
      for (int i=2;i<count;i++)
        area=std::max(area,std::fabs(b2Cross(points[1]-points[0],points[i]-points[0])));
      if (area<=b2_epsilon) return nullptr;
      polygon.Set(points,count);
      return &polygon;
    }
    return nullptr;
  }
};

bool query_transform(const GmlPhysicsBody &pose,double scale,b2Transform &transform) {
  if (!spatial(pose.x,scale) || !spatial(pose.y,scale) || !bounded(pose.angle)) return false;
  transform.Set(b2Vec2(float(pose.x*scale),float(pose.y*scale)),float(pose.angle*radians));
  return true;
}

struct Filter : b2ContactFilter {
  GmlPhysicsFilter callback;
  void *context;
  bool ShouldCollide(b2Fixture *a,b2Fixture *b) override {
    const b2Filter &fa=a->GetFilterData(), &fb=b->GetFilterData();
    if (fa.groupIndex && fa.groupIndex==fb.groupIndex) return fa.groupIndex>0;
    auto *ba=reinterpret_cast<GmlPhysicsBody *>(a->GetBody()->GetUserData().pointer);
    auto *bb=reinterpret_cast<GmlPhysicsBody *>(b->GetBody()->GetUserData().pointer);
    return !callback || callback(context,ba->object,bb->object);
  }
};

struct Contacts : b2ContactListener {
  GmlPhysicsContact *output;
  int count=0, capacity;
  double scale;
  void record(b2Contact *contact) {
    if (!contact->IsTouching()) return;
    auto *a=reinterpret_cast<GmlPhysicsBody *>(contact->GetFixtureA()->GetBody()->GetUserData().pointer);
    auto *b=reinterpret_cast<GmlPhysicsBody *>(contact->GetFixtureB()->GetBody()->GetUserData().pointer);
    for (int i=0;i<count;i++)
      if ((output[i].a==a->id && output[i].b==b->id) ||
          (output[i].a==b->id && output[i].b==a->id)) return;
    if (count==capacity) return;
    b2WorldManifold manifold;
    manifold.normal.SetZero(); manifold.points[0].SetZero();
    contact->GetWorldManifold(&manifold);
    auto &out=output[count++];
    out.a=a->id; out.b=b->id;
    out.x=manifold.points[0].x/scale; out.y=manifold.points[0].y/scale;
    out.nx=manifold.normal.x; out.ny=manifold.normal.y;
  }
  void BeginContact(b2Contact *contact) override { record(contact); }
  void PreSolve(b2Contact *contact,const b2Manifold *) override { record(contact); }
};

b2Body *find_body(const std::vector<b2Body *> &bodies,uint32_t id) {
  for (auto *body:bodies)
    if (body && reinterpret_cast<GmlPhysicsBody *>(body->GetUserData().pointer)->id==id)
      return body;
  return nullptr;
}
}

extern "C" int gml_physics_chain_valid(const GmlPhysicsFixture *fixture,double scale) {
  const int count=fixture_children(*fixture);
  if (scale<=0 || !count ||
      fixture->points<(fixture->shape==GML_PHYS_FIXTURE_LOOP?3:2)) return 0;
  for (int i=0;i<fixture->points;i++) {
    const GmlPhysicsPoint &p=fixture->chain[i];
    if (!spatial(p.x,scale) || !spatial(p.y,scale)) return 0;
  }
  for (int child=0;child<count;child++) {
    const GmlPhysicsPoint &a=fixture->chain[child],&b=fixture->chain[(child+1)%fixture->points];
    b2Vec2 first(float(a.x*scale),float(a.y*scale));
    b2Vec2 second(float(b.x*scale),float(b.y*scale));
    if (b2DistanceSquared(first,second)<b2_linearSlop*b2_linearSlop) return 0;
  }
  return 1;
}

extern "C" void gml_physics_fixture_outline(const GmlPhysicsFixture *fixture,
                                            const GmlPhysicsBody *pose,double scale,
                                            GmlPhysicsOutlineLine line,
                                            GmlPhysicsOutlineCircle circle,void *context) {
  b2Transform transform;
  if (!fixture || !pose || !line || !circle || !bounded(scale) || scale<=0 ||
      !query_transform(*pose,scale,transform)) return;
  Shape storage;
  const int count=fixture_children(*fixture);
  for (int child=0;child<count;child++) {
    const b2Shape *shape=storage.make(*fixture,scale,child);
    if (!shape) continue;
    if (shape->GetType()==b2Shape::e_circle) {
      b2Vec2 center=b2Mul(transform,storage.circle.m_p);
      circle(context,center.x/scale,center.y/scale,storage.circle.m_radius/scale);
    } else if (shape->GetType()==b2Shape::e_polygon) {
      const auto &polygon=storage.polygon;
      for (int i=0;i<polygon.m_count;i++) {
        b2Vec2 a=b2Mul(transform,polygon.m_vertices[i]);
        b2Vec2 b=b2Mul(transform,polygon.m_vertices[(i+1)%polygon.m_count]);
        line(context,a.x/scale,a.y/scale,b.x/scale,b.y/scale);
      }
    } else if (shape->GetType()==b2Shape::e_edge) {
      b2Vec2 a=b2Mul(transform,storage.edge.m_vertex1);
      b2Vec2 b=b2Mul(transform,storage.edge.m_vertex2);
      line(context,a.x/scale,a.y/scale,b.x/scale,b.y/scale);
    }
  }
}

extern "C" int gml_physics_fixture_overlap(const GmlPhysicsFixture *a,const GmlPhysicsBody *pose_a,
                                           const GmlPhysicsFixture *b,const GmlPhysicsBody *pose_b,
                                           double scale) {
  b2Transform transform_a,transform_b;
  if (!query_transform(*pose_a,scale,transform_a) ||
      !query_transform(*pose_b,scale,transform_b)) return 0;
  Shape storage_a,storage_b;
  const int count_a=fixture_children(*a),count_b=fixture_children(*b);
  for (int i=0;i<count_a;i++) {
    const b2Shape *shape_a=storage_a.make(*a,scale,i);
    if (!shape_a) continue;
    for (int j=0;j<count_b;j++) {
      const b2Shape *shape_b=storage_b.make(*b,scale,j);
      if (shape_b && b2TestOverlap(shape_a,0,shape_b,0,transform_a,transform_b)) return 1;
    }
  }
  return 0;
}

extern "C" void gml_physics_fixture_raycast(const GmlPhysicsFixture *fixture,
                                            const GmlPhysicsBody *pose,const GmlPhysicsRay *ray,
                                            double scale,GmlPhysicsRayVisitor visit,void *context) {
  b2Transform transform;
  if (!query_transform(*pose,scale,transform) ||
      !spatial(ray->x1,scale) || !spatial(ray->y1,scale) ||
      !spatial(ray->x2,scale) || !spatial(ray->y2,scale)) return;
  b2RayCastInput input;
  input.p1.Set(float(ray->x1*scale),float(ray->y1*scale));
  input.p2.Set(float(ray->x2*scale),float(ray->y2*scale));
  if (input.p1==input.p2) return;
  input.maxFraction=float(ray->max_fraction);
  Shape storage;
  const int count=fixture_children(*fixture);
  for (int child=0;child<count;child++) {
    const b2Shape *shape=storage.make(*fixture,scale,child);
    b2RayCastOutput output;
    if (!shape || !shape->RayCast(&output,input,transform,0)) continue;
    GmlPhysicsRayHit hit;
    hit.instance=uint32_t(fixture->bound_inst);
    hit.fixture=fixture->id;
    hit.child=uint32_t(child);
    hit.fraction=output.fraction;
    hit.x=ray->x1+(ray->x2-ray->x1)*hit.fraction;
    hit.y=ray->y1+(ray->y2-ray->y1)*hit.fraction;
    hit.nx=output.normal.x;
    hit.ny=output.normal.y;
    if (!visit(context,&hit)) break;
  }
}

extern "C" void gml_physics_solver_mass(GmlPhysicsBody *body,
                                          const GmlPhysicsFixture *fixtures,
                                          int count,double scale) {
  if (!body || !fixtures || !bounded(scale) || scale<=0) return;
  double mass=0,inertia=0,cx=0,cy=0;
  const GmlPhysicsFixture *first=nullptr;
  for (int i=0;i<count;i++) {
    const auto &f=fixtures[i];
    if (!f.live || f.bound_inst!=int(body->id)) continue;
    if (!first || f.id<first->id) first=&f;
    if (gml_physics_is_chain(f.shape)) continue;
    Shape storage;
    const b2Shape *shape=storage.make(f,scale);
    if (!shape) continue;
    b2MassData data;
    shape->ComputeMass(&data,nonnegative(f.density));
    mass+=data.mass; inertia+=data.I;
    cx+=data.mass*data.center.x; cy+=data.mass*data.center.y;
  }
  body->chain_mass=mass<=0 && first && gml_physics_is_chain(first->shape) &&
                   nonnegative(first->density)>0;
  if (mass>0) { cx/=mass; cy/=mass; }
  body->mass=body->chain_mass?1:mass; /* Dynamic segments have zero area. */
  body->inertia=std::max(0.0,inertia-mass*(cx*cx+cy*cy));
  body->center_x=cx/scale; body->center_y=cy/scale;
}

/* Box2D derives the coupling constant when it constructs the gear. Give it
 * the authored source poses for that calculation, then restore the current
 * poses before any solve or contact detection. */
static b2Joint *create_gear(b2World &world,const GmlPhysicsJoint &gear,double scale,
                        b2Body *a,b2Body *b,b2Joint *first,b2Joint *second) {
  if (!first || !second || first->GetBodyB()!=a || second->GetBodyB()!=b ||
      a->GetType()!=b2_dynamicBody || b->GetType()!=b2_dynamicBody ||
      a==second->GetBodyA() || b==first->GetBodyA()) return nullptr;
  b2Body *const bodies[]={first->GetBodyA(),a,second->GetBodyA(),b};
  b2Vec2 positions[4];
  float angles[4];
  bool moved[4];
  for (int i=0;i<4;i++) {
    const double *pose=gear.params+1+3*i;
    if (!bodies[i]->IsEnabled() || !spatial(pose[0],scale) || !spatial(pose[1],scale)) return nullptr;
    positions[i]=bodies[i]->GetPosition();
    angles[i]=bodies[i]->GetAngle();
    moved[i]=false;
  }
  /* The supported shared ground has no independent solver motion. */
  if (bodies[0]==bodies[2] && bodies[0]->GetType()==b2_dynamicBody) return nullptr;
  for (int i=0;i<4;i++) {
    if (i==2 && bodies[0]==bodies[2]) continue;
    const double *pose=gear.params+1+3*i;
    b2Vec2 position(float(pose[0]*scale),float(pose[1]*scale));
    float angle=float(pose[2]*radians);
    moved[i]=position!=positions[i] || angle!=angles[i];
    if (moved[i]) bodies[i]->SetTransform(position,angle);
  }
  b2GearJointDef def;
  def.bodyA=a; def.bodyB=b;
  def.joint1=first; def.joint2=second;
  def.ratio=float(gear.params[0]);
  b2Joint *joint=world.CreateJoint(&def);
  for (int i=0;i<4;i++)
    if (moved[i]) bodies[i]->SetTransform(positions[i],angles[i]);
  return joint;
}

struct StepJoint {
  GmlPhysicsJoint *description;
  b2Joint *native;
};

static void sample_joint(StepJoint &entry,float inv_dt) {
  GmlPhysicsJoint &joint=*entry.description;
  double *sample=joint.params;
  for (int i=GML_JOINT_SAMPLE_FORCE_X;i<=GML_JOINT_SAMPLE_MOTOR_FORCE;i++) sample[i]=0;
  if (joint.value_count>GML_JOINT_SAMPLE_FORCE_X) joint.value_count=GML_JOINT_SAMPLE_FORCE_X;
  b2Joint *native=entry.native;
  if (!native || !native->IsEnabled() ||
      (native->GetBodyA()->GetType()==b2_staticBody && native->GetBodyB()->GetType()==b2_staticBody)) return;
  b2Vec2 force=native->GetReactionForce(inv_dt);
  sample[GML_JOINT_SAMPLE_FORCE_X]=force.x;
  sample[GML_JOINT_SAMPLE_FORCE_Y]=force.y;
  sample[GML_JOINT_SAMPLE_TORQUE]=native->GetReactionTorque(inv_dt);
  switch (joint.type) {
    case GML_PHYS_JOINT_REVOLUTE:
      sample[GML_JOINT_SAMPLE_MOTOR_TORQUE]=static_cast<b2RevoluteJoint *>(native)->GetMotorTorque(inv_dt);
      break;
    case GML_PHYS_JOINT_WHEEL:
      sample[GML_JOINT_SAMPLE_MOTOR_TORQUE]=static_cast<b2WheelJoint *>(native)->GetMotorTorque(inv_dt);
      break;
    case GML_PHYS_JOINT_PRISMATIC:
      sample[GML_JOINT_SAMPLE_MOTOR_FORCE]=static_cast<b2PrismaticJoint *>(native)->GetMotorForce(inv_dt);
      break;
  }
  for (int i=GML_JOINT_SAMPLE_FORCE_X;i<=GML_JOINT_SAMPLE_MOTOR_FORCE;i++)
    if (!std::isfinite(sample[i])) sample[i]=0;
}

extern "C" int gml_physics_solver_step(GmlPhysicsBody *bodies,int body_count,
                                         const GmlPhysicsFixture *fixtures,int fixture_count,
                                         GmlPhysicsJoint *joints,int joint_count,
                                         double scale,double gx,double gy,double seconds,
                                         int substeps,int iterations,GmlPhysicsFilter callback,
                                         void *context,GmlPhysicsContact *output,int capacity) {
  if (!bodies || body_count<=0 || !bounded(scale) || scale<=0 || !bounded(gx) ||
      !bounded(gy) || !bounded(seconds) || seconds<=0 || !output || capacity<0) return 0;
  b2World world{b2Vec2(float(gx),float(gy))};
  /* The native world is a disposable solve graph. Disabling hidden contact impulses and sleep
   * timers makes every future frame a function of the canonical body/fixture/joint values,
   * including the first frame after a state restore. */
  world.SetWarmStarting(false);
  world.SetAllowSleeping(false);
  world.SetAutoClearForces(false);
  Filter filter; filter.callback=callback; filter.context=context;
  Contacts contacts; contacts.output=output; contacts.capacity=capacity; contacts.scale=scale;
  world.SetContactFilter(&filter); world.SetContactListener(&contacts);
  std::vector<b2Body *> native(size_t(body_count),nullptr);
  for (int i=0;i<body_count;i++) {
    auto &b=bodies[i];
    if (!spatial(b.x,scale) || !spatial(b.y,scale) || !bounded(b.angle) ||
        !spatial(b.vx,scale) || !spatial(b.vy,scale) || !bounded(b.omega)) continue;
    b2BodyDef def;
    def.type=b.kinematic?b2_kinematicBody:b.mass>0?b2_dynamicBody:b2_staticBody;
    def.position.Set(float(b.x*scale),float(b.y*scale));
    def.angle=float(b.angle*radians);
    def.linearVelocity.Set(float(b.vx*scale),float(b.vy*scale));
    def.angularVelocity=float(b.omega*radians);
    def.linearDamping=nonnegative(b.linear_damping);
    def.angularDamping=nonnegative(b.angular_damping);
    def.fixedRotation=b.fixed_rotation!=0; def.bullet=b.bullet!=0;
    def.enabled=b.active!=0; def.allowSleep=false;
    def.userData.pointer=reinterpret_cast<uintptr_t>(&b);
    native[size_t(i)]=world.CreateBody(&def);
  }
  std::vector<const GmlPhysicsFixture *> ordered;
  for (int i=0;i<fixture_count;i++)
    if (fixtures[i].live && fixtures[i].bound_inst>=0) ordered.push_back(&fixtures[i]);
  std::sort(ordered.begin(),ordered.end(),[](const GmlPhysicsFixture *a,const GmlPhysicsFixture *b){return a->id<b->id;});
  for (auto *f:ordered) {
    b2Body *body=find_body(native,uint32_t(f->bound_inst));
    if (!body) continue;
    Shape storage;
    b2FixtureDef def;
    def.density=nonnegative(f->density);
    def.friction=nonnegative(f->friction); def.restitution=nonnegative(f->restitution);
    def.isSensor=f->sensor!=0;
    def.filter.groupIndex=int16(std::max(-32768,std::min(32767,f->group)));
    const int count=fixture_children(*f);
    for (int child=0;child<count;child++) {
      def.shape=storage.make(*f,scale,child);
      if (def.shape) body->CreateFixture(&def);
    }
  }
  for (int i=0;i<body_count;i++) {
    b2Body *body=native[size_t(i)];
    const auto &b=bodies[i];
    if (!body) continue;
    if ((b.explicit_mass || b.chain_mass) && b.mass>0 && bounded(b.mass) && bounded(b.inertia) &&
        spatial(b.center_x,scale) && spatial(b.center_y,scale)) {
      b2MassData data;
      data.mass=float(b.mass);
      data.center.Set(float(b.center_x*scale),float(b.center_y*scale));
      float offset_inertia=data.mass*b2Dot(data.center,data.center);
      data.I=float(std::max(0.0,b.inertia))+offset_inertia;
      /* Zero or sub-precision inertia must not become a positive origin inertia whose
       * centre correction is zero: SetMassData requires a positive corrected value. */
      if (data.I<=offset_inertia) data.I=0;
      body->SetMassData(&data);
    }
    /* Creating fixtures can move the centre of mass. Restore the caller's centre velocity
     * after that calculation, otherwise reconstruction adds an angular cross term per frame. */
    body->SetLinearVelocity(b2Vec2(float(b.vx*scale),float(b.vy*scale)));
    body->SetAngularVelocity(float(b.omega*radians));
    if (bounded(b.force_x) && bounded(b.force_y))
      body->ApplyForceToCenter(b2Vec2(float(b.force_x),float(b.force_y)),true);
    if (bounded(b.torque)) body->ApplyTorque(float(b.torque),true);
  }
  std::vector<StepJoint> ordered_joints;
  for (int i=0;i<joint_count;i++)
    if (joints[i].live && joints[i].initialized) ordered_joints.push_back({&joints[i],nullptr});
  std::sort(ordered_joints.begin(),ordered_joints.end(),[](const StepJoint &a,const StepJoint &b){
    return a.description->id<b.description->id;
  });
  const bool has_gear=std::any_of(ordered_joints.begin(),ordered_joints.end(),[](const StepJoint &entry){
    return entry.description->type==GML_PHYS_JOINT_GEAR;
  });
  using GearSource=std::pair<uint32_t,b2Joint *>;
  std::vector<GearSource> gear_sources;
  auto find_source=[&gear_sources](double id) -> b2Joint * {
    auto at=std::lower_bound(gear_sources.begin(),gear_sources.end(),id,
                            [](const GearSource &entry,double key){return entry.first<key;});
    return at!=gear_sources.end() && at->first==id?at->second:nullptr;
  };
  for (StepJoint &entry:ordered_joints) {
    const GmlPhysicsJoint *j=entry.description;
    const double values[]={j->a,j->b,j->anchor_ax,j->anchor_ay,j->anchor_bx,
                           j->anchor_by,j->reference_angle,j->x2,j->y2};
    bool valid=true;
    for (double value:values) if (!bounded(value)) valid=false;
    for (int i=0;i<GML_JOINT_SAMPLE_FORCE_X;i++)
      if (!bounded(j->params[i])) valid=false;
    if (!valid || j->a<0 || j->b<0 || j->a>UINT32_MAX || j->b>UINT32_MAX ||
        !spatial(j->anchor_ax,scale) || !spatial(j->anchor_ay,scale) ||
        !spatial(j->anchor_bx,scale) || !spatial(j->anchor_by,scale) ||
        (j->type==GML_PHYS_JOINT_ROPE && !spatial(j->params[0],scale))) continue;
    b2Body *a=find_body(native,uint32_t(j->a)), *b=find_body(native,uint32_t(j->b));
    if (!a || !b || a==b) continue;
    if (j->type==GML_PHYS_JOINT_REVOLUTE) {
      b2RevoluteJointDef def;
      def.bodyA=a; def.bodyB=b;
      def.localAnchorA.Set(float(j->anchor_ax*scale),float(j->anchor_ay*scale));
      def.localAnchorB.Set(float(j->anchor_bx*scale),float(j->anchor_by*scale));
      def.referenceAngle=float(j->reference_angle*radians);
      def.lowerAngle=float(j->x2*radians); def.upperAngle=float(j->y2*radians);
      if (def.lowerAngle>def.upperAngle) std::swap(def.lowerAngle,def.upperAngle);
      def.enableLimit=j->params[0]!=0;
      def.maxMotorTorque=nonnegative(j->params[1]);
      def.motorSpeed=float(j->params[2]*radians); def.enableMotor=j->params[3]!=0;
      def.collideConnected=j->params[4]!=0;
      entry.native=world.CreateJoint(&def);
      if (has_gear) gear_sources.emplace_back(j->id,entry.native);
    } else if (j->type==GML_PHYS_JOINT_PRISMATIC || j->type==GML_PHYS_JOINT_WHEEL) {
      const b2Vec2 local_a(float(j->anchor_ax*scale),float(j->anchor_ay*scale));
      const b2Vec2 local_b(float(j->anchor_bx*scale),float(j->anchor_by*scale));
      if (std::fabs(j->x2*j->x2+j->y2*j->y2-1.0)>1e-6) continue;
      const b2Vec2 axis(float(j->x2),float(j->y2));
      if (j->type==GML_PHYS_JOINT_PRISMATIC) {
        if (!spatial(j->params[0],scale) || !spatial(j->params[1],scale) ||
            j->params[0]>j->params[1]) continue;
        b2PrismaticJointDef def;
        def.bodyA=a; def.bodyB=b;
        def.localAnchorA=local_a; def.localAnchorB=local_b; def.localAxisA=axis;
        def.referenceAngle=float(j->reference_angle*radians);
        def.lowerTranslation=float(j->params[0]*scale);
        def.upperTranslation=float(j->params[1]*scale);
        def.enableLimit=j->params[2]!=0;
        def.maxMotorForce=nonnegative(j->params[3]);
        def.motorSpeed=float(j->params[4]); def.enableMotor=j->params[5]!=0;
        def.collideConnected=j->params[6]!=0;
        entry.native=world.CreateJoint(&def);
        if (has_gear) gear_sources.emplace_back(j->id,entry.native);
      } else {
        b2WheelJointDef def;
        def.bodyA=a; def.bodyB=b;
        def.localAnchorA=local_a; def.localAnchorB=local_b; def.localAxisA=axis;
        def.enableMotor=j->params[0]!=0;
        def.maxMotorTorque=nonnegative(j->params[1]);
        def.motorSpeed=float(j->params[2]);
        b2LinearStiffness(def.stiffness,def.damping,nonnegative(j->params[3]),
                          nonnegative(j->params[4]),a,b);
        if (!std::isfinite(def.stiffness) || !std::isfinite(def.damping)) continue;
        def.collideConnected=j->params[5]!=0;
        entry.native=world.CreateJoint(&def);
      }
    } else if (j->type==GML_PHYS_JOINT_GEAR) {
      entry.native=create_gear(world,*j,scale,a,b,find_source(j->x1),find_source(j->x2));
    } else if (j->type==GML_PHYS_JOINT_ROPE) {
      b2DistanceJointDef def;
      def.bodyA=a; def.bodyB=b;
      def.localAnchorA.Set(float(j->anchor_ax*scale),float(j->anchor_ay*scale));
      def.localAnchorB.Set(float(j->anchor_bx*scale),float(j->anchor_by*scale));
      def.minLength=b2_linearSlop;
      def.maxLength=std::max(b2_linearSlop,float(j->params[0]*scale));
      def.length=def.maxLength; def.collideConnected=j->params[1]!=0;
      entry.native=world.CreateJoint(&def);
    } else if (j->type>=GML_PHYS_JOINT_DISTANCE && j->type<=GML_PHYS_JOINT_PULLEY) {
      const b2Vec2 local_a(float(j->anchor_ax*scale),float(j->anchor_ay*scale));
      const b2Vec2 local_b(float(j->anchor_bx*scale),float(j->anchor_by*scale));
      switch (j->type) {
        case GML_PHYS_JOINT_DISTANCE: {
          if (!spatial(j->params[0],scale)) break;
          b2DistanceJointDef def;
          def.bodyA=a; def.bodyB=b;
          def.localAnchorA=local_a; def.localAnchorB=local_b;
          def.length=std::max(b2_linearSlop,float(j->params[0]*scale));
          if (j->params[2]>0) {
            b2LinearStiffness(def.stiffness,def.damping,nonnegative(j->params[2]),
                              nonnegative(j->params[3]),a,b);
            if (!std::isfinite(def.stiffness) || !std::isfinite(def.damping)) break;
          } else def.minLength=def.maxLength=def.length;
          def.collideConnected=j->params[1]!=0;
          entry.native=world.CreateJoint(&def);
          break;
        }
        case GML_PHYS_JOINT_FRICTION: {
          b2FrictionJointDef def;
          def.bodyA=a; def.bodyB=b;
          def.localAnchorA=local_a; def.localAnchorB=local_b;
          def.maxForce=nonnegative(j->params[0]);
          def.maxTorque=nonnegative(j->params[1]);
          def.collideConnected=j->params[2]!=0;
          entry.native=world.CreateJoint(&def);
          break;
        }
        case GML_PHYS_JOINT_WELD: {
          b2WeldJointDef def;
          def.bodyA=a; def.bodyB=b;
          def.localAnchorA=local_a; def.localAnchorB=local_b;
          def.referenceAngle=float(j->reference_angle*radians);
          b2AngularStiffness(def.stiffness,def.damping,nonnegative(j->params[0]),
                            nonnegative(j->params[1]),a,b);
          if (!std::isfinite(def.stiffness) || !std::isfinite(def.damping)) break;
          def.collideConnected=j->params[2]!=0;
          entry.native=world.CreateJoint(&def);
          break;
        }
        case GML_PHYS_JOINT_PULLEY: {
          if (j->params[0]<1e-12 ||
              !spatial(j->x1,scale) || !spatial(j->y1,scale) ||
              !spatial(j->x2,scale) || !spatial(j->y2,scale) ||
              !spatial(j->params[2],scale) || !spatial(j->params[3],scale)) break;
          b2PulleyJointDef def;
          def.bodyA=a; def.bodyB=b;
          def.localAnchorA=local_a; def.localAnchorB=local_b;
          def.groundAnchorA.Set(float(j->x1*scale),float(j->y1*scale));
          def.groundAnchorB.Set(float(j->x2*scale),float(j->y2*scale));
          def.lengthA=float(j->params[2]*scale);
          def.lengthB=float(j->params[3]*scale);
          def.ratio=float(j->params[0]);
          def.collideConnected=j->params[1]!=0;
          entry.native=world.CreateJoint(&def);
          break;
        }
      }
    }
  }
  substeps=std::max(1,std::min(16,substeps));
  iterations=std::max(1,std::min(64,iterations));
  const float time_step=float(seconds/substeps);
  for (int step=0;step<substeps;step++) world.Step(time_step,iterations,iterations);
  for (StepJoint &entry:ordered_joints) sample_joint(entry,1.0f/time_step);
  for (int i=0;i<body_count;i++) {
    b2Body *body=native[size_t(i)]; auto &b=bodies[i];
    if (!body) continue;
    b.x=body->GetPosition().x/scale; b.y=body->GetPosition().y/scale;
    b.angle=body->GetAngle()/radians;
    b.vx=body->GetLinearVelocity().x/scale; b.vy=body->GetLinearVelocity().y/scale;
    b.omega=body->GetAngularVelocity()/radians;
  }
  /* Destruction can emit EndContact callbacks. Detach stack-owned adapters first. */
  world.SetContactListener(nullptr); world.SetContactFilter(nullptr);
  return contacts.count;
}
