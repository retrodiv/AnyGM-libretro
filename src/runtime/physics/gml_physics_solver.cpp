/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "gml_physics_solver.h"
#include "box2d/box2d.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

namespace {
const double radians = 0.017453292519943295;

bool bounded(double x) { return std::isfinite(x) && std::fabs(x) <= 1e12; }
bool spatial(double x,double scale) { return bounded(x) && std::fabs(x*scale)<=1e6; }
float nonnegative(double x) { return bounded(x) && x > 0 ? float(x) : 0.0f; }

struct Shape {
  b2CircleShape circle;
  b2PolygonShape polygon;
  b2EdgeShape edge;

  const b2Shape *make(const GmlPhysicsFixture &f, double scale) {
    /* Bound converted geometry and density before the single-precision mass integrals.
     * Finite doubles alone can still overflow those integrals into invalid native bodies. */
    const double dimensions[]={f.radius,f.w,f.h,f.x1,f.y1,f.x2,f.y2};
    for (double coordinate:dimensions)
      if (!bounded(coordinate) || std::fabs(coordinate*scale)>1e6) return nullptr;
    if (!bounded(f.density) || f.density>1e6) return nullptr;
    if (f.shape == 1 && f.radius*scale > b2_epsilon) {
      circle.m_radius = float(f.radius * scale);
      circle.m_p.SetZero();
      return &circle;
    }
    if (f.shape == 2 && f.w*scale > b2_epsilon && f.h*scale > b2_epsilon) {
      polygon.SetAsBox(float(f.w * scale), float(f.h * scale));
      return &polygon;
    }
    if (f.shape == 4 && bounded(f.x1) && bounded(f.x2) && bounded(f.y1) && bounded(f.y2)) {
      b2Vec2 a(float(f.x1 * scale), float(f.y1 * scale));
      b2Vec2 b(float(f.x2 * scale), float(f.y2 * scale));
      if ((a-b).LengthSquared() <= b2_epsilon*b2_epsilon) return nullptr;
      edge.SetTwoSided(a,b);
      return &edge;
    }
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

extern "C" void gml_physics_solver_mass(GmlPhysicsBody *body,
                                          const GmlPhysicsFixture *fixtures,
                                          int count,double scale) {
  if (!body || !fixtures || !bounded(scale) || scale<=0) return;
  double mass=0,inertia=0,cx=0,cy=0;
  for (int i=0;i<count;i++) {
    const auto &f=fixtures[i];
    if (!f.live || f.bound_inst!=int(body->id)) continue;
    Shape storage;
    const b2Shape *shape=storage.make(f,scale);
    if (!shape) continue;
    b2MassData data;
    shape->ComputeMass(&data,nonnegative(f.density));
    mass+=data.mass; inertia+=data.I;
    cx+=data.mass*data.center.x; cy+=data.mass*data.center.y;
  }
  if (mass>0) { cx/=mass; cy/=mass; }
  body->mass=mass;
  body->inertia=std::max(0.0,inertia-mass*(cx*cx+cy*cy));
  body->center_x=cx/scale; body->center_y=cy/scale;
}

extern "C" int gml_physics_solver_step(GmlPhysicsBody *bodies,int body_count,
                                         const GmlPhysicsFixture *fixtures,int fixture_count,
                                         const GmlPhysicsJoint *joints,int joint_count,
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
    const b2Shape *shape=storage.make(*f,scale);
    if (!shape) continue;
    b2FixtureDef def;
    def.shape=shape; def.density=nonnegative(f->density);
    def.friction=nonnegative(f->friction); def.restitution=nonnegative(f->restitution);
    def.isSensor=f->sensor!=0;
    def.filter.groupIndex=int16(std::max(-32768,std::min(32767,f->group)));
    body->CreateFixture(&def);
  }
  for (int i=0;i<body_count;i++) {
    b2Body *body=native[size_t(i)];
    const auto &b=bodies[i];
    if (!body) continue;
    if (b.explicit_mass && b.mass>0 && bounded(b.mass) && bounded(b.inertia) &&
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
  std::vector<const GmlPhysicsJoint *> ordered_joints;
  for (int i=0;i<joint_count;i++)
    if (joints[i].live && joints[i].initialized) ordered_joints.push_back(&joints[i]);
  std::sort(ordered_joints.begin(),ordered_joints.end(),[](const GmlPhysicsJoint *a,const GmlPhysicsJoint *b){return a->id<b->id;});
  for (auto *j:ordered_joints) {
    const double values[]={j->a,j->b,j->anchor_ax,j->anchor_ay,j->anchor_bx,
                           j->anchor_by,j->reference_angle,j->x2,j->y2};
    bool valid=true;
    for (double value:values) if (!bounded(value)) valid=false;
    for (double value:j->params) if (!bounded(value)) valid=false;
    if (!valid || j->a<0 || j->b<0 || j->a>UINT32_MAX || j->b>UINT32_MAX ||
        !spatial(j->anchor_ax,scale) || !spatial(j->anchor_ay,scale) ||
        !spatial(j->anchor_bx,scale) || !spatial(j->anchor_by,scale) ||
        (j->type==4 && !spatial(j->params[0],scale))) continue;
    b2Body *a=find_body(native,uint32_t(j->a)), *b=find_body(native,uint32_t(j->b));
    if (!a || !b || a==b) continue;
    if (j->type==1) {
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
      world.CreateJoint(&def);
    } else if (j->type==4) {
      b2DistanceJointDef def;
      def.bodyA=a; def.bodyB=b;
      def.localAnchorA.Set(float(j->anchor_ax*scale),float(j->anchor_ay*scale));
      def.localAnchorB.Set(float(j->anchor_bx*scale),float(j->anchor_by*scale));
      def.minLength=b2_linearSlop;
      def.maxLength=std::max(b2_linearSlop,float(j->params[0]*scale));
      def.length=def.maxLength; def.collideConnected=j->params[1]!=0;
      world.CreateJoint(&def);
    }
  }
  substeps=std::max(1,std::min(16,substeps));
  iterations=std::max(1,std::min(64,iterations));
  for (int step=0;step<substeps;step++)
    world.Step(float(seconds/substeps),iterations,iterations);
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
