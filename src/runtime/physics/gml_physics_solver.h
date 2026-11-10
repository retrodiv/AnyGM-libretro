/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef GML_PHYSICS_SOLVER_H
#define GML_PHYSICS_SOLVER_H

#include <stdint.h>

#define GML_PHYS_FIXTURE_POINTS 16
#define GML_PHYS_FIXTURE_DEFAULT_DENSITY 0.5

/* Canonical resource descriptions. Native solver objects never enter state bytes. */
typedef struct {
  int live, shape, bound_inst, points;
  uint32_t id;
  double density, friction, restitution, lin_damp, ang_damp, awake;
  double radius, w, h, x1, y1, x2, y2;
  double px[GML_PHYS_FIXTURE_POINTS], py[GML_PHYS_FIXTURE_POINTS];
  int sensor, group, kinematic, density_set;
} GmlPhysicsFixture;

enum {
  GML_PHYS_JOINT_REVOLUTE=1,
  GML_PHYS_JOINT_PRISMATIC,
  GML_PHYS_JOINT_WHEEL,
  GML_PHYS_JOINT_ROPE,
  GML_PHYS_JOINT_DISTANCE,
  GML_PHYS_JOINT_FRICTION,
  GML_PHYS_JOINT_WELD,
  GML_PHYS_JOINT_PULLEY,
  GML_PHYS_JOINT_GEAR
};

#define GML_PHYS_JOINT_FIELDS(ENTRY) \
  ENTRY(ANCHOR_1_X, 0, "phy_joint_anchor_1_x") \
  ENTRY(ANCHOR_1_Y, 1, "phy_joint_anchor_1_y") \
  ENTRY(ANCHOR_2_X, 2, "phy_joint_anchor_2_x") \
  ENTRY(ANCHOR_2_Y, 3, "phy_joint_anchor_2_y") \
  ENTRY(REACTION_FORCE_X, 4, "phy_joint_reaction_force_x") \
  ENTRY(REACTION_FORCE_Y, 5, "phy_joint_reaction_force_y") \
  ENTRY(REACTION_TORQUE, 6, "phy_joint_reaction_torque") \
  ENTRY(MOTOR_SPEED, 7, "phy_joint_motor_speed") \
  ENTRY(ANGLE, 8, "phy_joint_angle") \
  ENTRY(MOTOR_TORQUE, 9, "phy_joint_motor_torque") \
  ENTRY(MAX_MOTOR_TORQUE, 10, "phy_joint_max_motor_torque") \
  ENTRY(TRANSLATION, 11, "phy_joint_translation") \
  ENTRY(SPEED, 12, "phy_joint_speed") \
  ENTRY(MOTOR_FORCE, 13, "phy_joint_motor_force") \
  ENTRY(MAX_MOTOR_FORCE, 14, "phy_joint_max_motor_force") \
  ENTRY(LENGTH_1, 15, "phy_joint_length_1") \
  ENTRY(LENGTH_2, 16, "phy_joint_length_2") \
  ENTRY(DAMPING_RATIO, 17, "phy_joint_damping_ratio") \
  ENTRY(FREQUENCY, 18, "phy_joint_frequency") \
  ENTRY(LOWER_ANGLE_LIMIT, 19, "phy_joint_lower_angle_limit") \
  ENTRY(UPPER_ANGLE_LIMIT, 20, "phy_joint_upper_angle_limit") \
  ENTRY(ANGLE_LIMITS, 21, "phy_joint_angle_limits") \
  ENTRY(MAX_LENGTH, 22, "phy_joint_max_length") \
  ENTRY(MAX_TORQUE, 23, "phy_joint_max_torque") \
  ENTRY(MAX_FORCE, 24, "phy_joint_max_force")

enum {
#define GML_JOINT_FIELD(symbol, value, name) GML_JOINT_##symbol=value,
  GML_PHYS_JOINT_FIELDS(GML_JOINT_FIELD)
#undef GML_JOINT_FIELD
  GML_JOINT_FIELD_COUNT
};

/* The final five slots hold solver observations. Older records that used
 * this tail as arbitrary parameters have value_count greater than its start. */
enum {
  GML_JOINT_SAMPLE_FORCE_X=19,
  GML_JOINT_SAMPLE_FORCE_Y,
  GML_JOINT_SAMPLE_TORQUE,
  GML_JOINT_SAMPLE_MOTOR_TORQUE,
  GML_JOINT_SAMPLE_MOTOR_FORCE
};

/* Prismatic and wheel x2/y2 hold the unit axis in body A's local frame.
 * Prismatic parameters: lower and upper translation (pixels), enable limit,
 * motor force, motor speed (m/s), enable motor, collide.
 * Wheel parameters: enable motor, motor torque, motor speed (rad/s),
 * frequency, damping ratio, collide.
 * Other parameter layouts: distance (length, collide), friction (force,
 * torque, collide), weld (frequency, damping ratio, collide), and pulley
 * (ratio, collide, initial length A, initial length B). Lengths are in pixels.
 * Pulley x1/y1 and x2/y2 are the two fixed world anchors.
 * Gear x1/x2 hold source joint IDs. Its parameters contain the ratio followed
 * by creation-time x, y and angle for the two bodies of each source, A then B. */
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

typedef struct {
  double x1,y1,x2,y2,max_fraction;
} GmlPhysicsRay;

typedef struct {
  uint32_t instance,fixture;
  double x,y,nx,ny,fraction;
} GmlPhysicsRayHit;

typedef int (*GmlPhysicsFilter)(void *context,int object_a,int object_b);

#ifdef __cplusplus
extern "C" {
#endif
int gml_physics_fixture_overlap(const GmlPhysicsFixture *a,const GmlPhysicsBody *pose_a,
                                const GmlPhysicsFixture *b,const GmlPhysicsBody *pose_b,double scale);
int gml_physics_fixture_raycast(const GmlPhysicsFixture *fixture,const GmlPhysicsBody *pose,
                                const GmlPhysicsRay *ray,double scale,GmlPhysicsRayHit *hit);
void gml_physics_solver_mass(GmlPhysicsBody *body,const GmlPhysicsFixture *fixtures,
                             int fixture_count,double scale);
int gml_physics_solver_step(GmlPhysicsBody *bodies,int body_count,
                            const GmlPhysicsFixture *fixtures,int fixture_count,
                            GmlPhysicsJoint *joints,int joint_count,
                            double scale,double gravity_x,double gravity_y,
                            double seconds,int substeps,int iterations,
                            GmlPhysicsFilter filter,void *context,
                            GmlPhysicsContact *contacts,int contact_capacity);
#ifdef __cplusplus
}
#endif
#endif
