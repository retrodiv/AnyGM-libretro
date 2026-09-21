/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef GML_PHYSICS_SOLVER_H
#define GML_PHYSICS_SOLVER_H

#include <stdint.h>

#define GML_PHYS_FIXTURE_POINTS 16

/* Canonical resource descriptions. Native solver objects never enter state bytes. */
typedef struct {
  int live, shape, bound_inst, points;
  uint32_t id;
  double density, friction, restitution, lin_damp, ang_damp, awake;
  double radius, w, h, x1, y1, x2, y2;
  double px[GML_PHYS_FIXTURE_POINTS], py[GML_PHYS_FIXTURE_POINTS];
  double offset_x, offset_y;
  int sensor, group;
} GmlPhysicsFixture;

typedef struct {
  int live, type, value_count;
  uint32_t id;
  double a, b, x1, y1, x2, y2, params[24];
  double anchor_ax, anchor_ay, anchor_bx, anchor_by, reference_angle;
  int initialized;
} GmlPhysicsJoint;

/* A step's value snapshot, in pixels, seconds and clockwise degrees. The caller owns
 * persistence and instance identity. Forces and mass properties use SI units. */
typedef struct {
  uint32_t id;
  int object, active, kinematic, fixed_rotation, bullet;
  double x, y, angle, vx, vy, omega, linear_damping, angular_damping;
  double force_x, force_y, torque;
  double mass, inertia, center_x, center_y;
  int explicit_mass;
} GmlPhysicsBody;

typedef struct {
  uint32_t a, b;
  double x, y, nx, ny;
} GmlPhysicsContact;

typedef int (*GmlPhysicsFilter)(void *context,int object_a,int object_b);

#ifdef __cplusplus
extern "C" {
#endif
void gml_physics_solver_mass(GmlPhysicsBody *body,const GmlPhysicsFixture *fixtures,
                             int fixture_count,double scale);
int gml_physics_solver_step(GmlPhysicsBody *bodies,int body_count,
                            const GmlPhysicsFixture *fixtures,int fixture_count,
                            const GmlPhysicsJoint *joints,int joint_count,
                            double scale,double gravity_x,double gravity_y,
                            double seconds,int substeps,int iterations,
                            GmlPhysicsFilter filter,void *context,
                            GmlPhysicsContact *contacts,int contact_capacity);
#ifdef __cplusplus
}
#endif
#endif
