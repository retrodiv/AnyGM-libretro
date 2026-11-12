/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef GML_PHYSICS_PARTICLES_H
#define GML_PHYSICS_PARTICLES_H

#include <stdint.h>

enum {
  GML_PHYS_PARTICLE_ZOMBIE=2, GML_PHYS_PARTICLE_WALL=4,
  GML_PHYS_PARTICLE_SPRING=8, GML_PHYS_PARTICLE_ELASTIC=16,
  GML_PHYS_PARTICLE_VISCOUS=32, GML_PHYS_PARTICLE_POWDER=64,
  GML_PHYS_PARTICLE_TENSILE=128, GML_PHYS_PARTICLE_COLOUR=256
};
enum { GML_PHYS_GROUP_SOLID=1, GML_PHYS_GROUP_RIGID=2 };

#define GML_PHYS_PARTICLE_CONSTANTS(ENTRY) \
  ENTRY(0,"phy_particle_flag_water") \
  ENTRY(2,"phy_particle_flag_zombie") \
  ENTRY(4,"phy_particle_flag_wall") \
  ENTRY(8,"phy_particle_flag_spring") \
  ENTRY(16,"phy_particle_flag_elastic") \
  ENTRY(32,"phy_particle_flag_viscous") \
  ENTRY(64,"phy_particle_flag_powder") \
  ENTRY(128,"phy_particle_flag_tensile") \
  ENTRY(256,"phy_particle_flag_colourmixing") \
  ENTRY(256,"phy_particle_flag_colormixing") \
  ENTRY(1,"phy_particle_data_flag_typeflags") \
  ENTRY(2,"phy_particle_data_flag_position") \
  ENTRY(4,"phy_particle_data_flag_velocity") \
  ENTRY(8,"phy_particle_data_flag_colour") \
  ENTRY(8,"phy_particle_data_flag_color") \
  ENTRY(16,"phy_particle_data_flag_category") \
  ENTRY(0,"phy_particle_water") \
  ENTRY(2,"phy_particle_zombie") \
  ENTRY(4,"phy_particle_wall") \
  ENTRY(8,"phy_particle_spring") \
  ENTRY(16,"phy_particle_elastic") \
  ENTRY(32,"phy_particle_viscous") \
  ENTRY(64,"phy_particle_powder") \
  ENTRY(128,"phy_particle_tensile") \
  ENTRY(256,"phy_particle_colourmixing") \
  ENTRY(256,"phy_particle_colormixing") \
  ENTRY(1,"phy_particle_data_flags") \
  ENTRY(2,"phy_particle_data_position") \
  ENTRY(4,"phy_particle_data_velocity") \
  ENTRY(8,"phy_particle_data_colour") \
  ENTRY(8,"phy_particle_data_color") \
  ENTRY(16,"phy_particle_data_category") \
  ENTRY(1,"phy_particle_group_flag_solid") \
  ENTRY(2,"phy_particle_group_flag_rigid") \
  ENTRY(1,"phy_particle_group_solid") \
  ENTRY(2,"phy_particle_group_rigid")

/* Positions, velocities and connection lengths use metres and seconds.
 * Indices are compacted together at the next simulated step after deletion. */
typedef struct {
  double x,y,vx,vy;
  uint32_t flags,colour;
  int32_t category;
  uint32_t group;
  double local_x,local_y,depth;
} GmlPhysicsParticle;

typedef struct {
  uint32_t first,second,flags;
  double length;
} GmlPhysicsParticleBond;

typedef struct {
  uint32_t id,flags;
  double x,y,angle,strength;
} GmlPhysicsParticleGroup;

typedef struct {
  int active,shape,count;
  uint32_t flags,group_flags,colour;
  int32_t category;
  double x,y,angle,vx,vy,omega,strength,rx,ry;
  double px[8],py[8];
} GmlPhysicsParticleGroupDefinition;

typedef struct {
  int count;
  double mass,inertia,cx,cy,vx,vy,omega;
} GmlPhysicsParticleGroupStatistics;

typedef struct {
  GmlPhysicsParticle *points;
  GmlPhysicsParticleBond *bonds;
  int count,capacity,bond_count,bond_capacity;
  GmlPhysicsParticleGroup *groups;
  int group_count,group_capacity;
  uint32_t next_group;
  GmlPhysicsParticleGroupDefinition definition;
} GmlPhysicsParticleState;

typedef struct {
  double radius,density,damping,gravity_scale;
  int max_count;
} GmlPhysicsParticleConfig;

#ifdef __cplusplus
extern "C" {
#endif
void gml_physics_particles_clear(GmlPhysicsParticleState *state);
int gml_physics_particles_add(GmlPhysicsParticleState *state,
                              const GmlPhysicsParticleConfig *config,
                              GmlPhysicsParticle particle);
int gml_physics_particles_flags(GmlPhysicsParticleState *state,int index,uint32_t flags);
void gml_physics_particles_compact(GmlPhysicsParticleState *state);
int gml_physics_particles_config_valid(const GmlPhysicsParticleConfig *config);
GmlPhysicsParticleGroup *gml_physics_particle_group_find(GmlPhysicsParticleState *state,uint32_t id);
int gml_physics_particle_group_create(GmlPhysicsParticleState *state,
                                    const GmlPhysicsParticleConfig *config);
void gml_physics_particle_group_statistics(const GmlPhysicsParticleState *state,
                                         const GmlPhysicsParticleConfig *config,uint32_t id,
                                         GmlPhysicsParticleGroupStatistics *statistics);
void gml_physics_particle_group_flags(GmlPhysicsParticleState *state,uint32_t id,uint32_t flags);
int gml_physics_particle_group_join(GmlPhysicsParticleState *state,
                                  const GmlPhysicsParticleConfig *config,uint32_t to,uint32_t from);
#ifdef __cplusplus
}
class b2World;
/* This operates on the existing disposable world, including its live rigid bodies. */
void gml_physics_particles_step(GmlPhysicsParticleState *state,
                                const GmlPhysicsParticleConfig *config,
                                b2World &world,double gx,double gy,
                                double seconds,int iterations);
#endif
#endif
