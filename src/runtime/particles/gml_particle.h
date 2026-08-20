/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Instance-owned particle runtime. */
#ifndef GML_PARTICLE_H
#define GML_PARTICLE_H

#include <stddef.h>
#include <stdint.h>
#include "gml_render.h"

struct GmlVM;
typedef struct GmlParticleState GmlParticleState;

GmlParticleState *gml_particle_state_create(struct GmlVM *vm);
void gml_particle_state_rebind(GmlParticleState *state,struct GmlVM *vm);
void gml_particle_state_destroy(GmlParticleState *state);
void gml_part_reset_all(GmlParticleState *state);
void gml_part_update_all(GmlParticleState *state);
void gml_part_system_draw_all(GmlParticleState *state, GmlRender *render);

size_t gml_part_state_size(GmlParticleState *state);
int gml_part_state_save(GmlParticleState *state, void *data, size_t len, size_t *written);
int gml_part_state_load(GmlParticleState *state, const void *data, size_t len, size_t *used);

int  gml_part_type_create(GmlParticleState *state);
int  gml_part_type_exists(GmlParticleState *state, int id);
void gml_part_type_destroy(GmlParticleState *state, int id);
void gml_part_type_clear(GmlParticleState *state, int id);
void gml_part_type_sprite(GmlParticleState *state, int id, int sprite, int animate, int stretch, int random);
void gml_part_type_shape(GmlParticleState *state, int id, int shape);
void gml_part_type_size(GmlParticleState *state, int id, double min, double max, double increment, double wiggle);
void gml_part_type_scale(GmlParticleState *state, int id, double xscale, double yscale);
void gml_part_type_speed(GmlParticleState *state, int id, double min, double max, double increment, double wiggle);
void gml_part_type_direction(GmlParticleState *state, int id, double min, double max, double increment, double wiggle);
void gml_part_type_gravity(GmlParticleState *state, int id, double amount, double direction);
void gml_part_type_life(GmlParticleState *state, int id, double min, double max);
void gml_part_type_step(GmlParticleState *state, int id, int number, int type);
void gml_part_type_death(GmlParticleState *state, int id, int number, int type);
void gml_part_type_orientation(GmlParticleState *state, int id, double min, double max, double increment, double wiggle, int relative);
void gml_part_type_color(GmlParticleState *state, int id, int count, uint32_t color1, uint32_t color2, uint32_t color3);
void gml_part_type_color_rgb(GmlParticleState *state, int id, double rmin, double rmax, double gmin, double gmax, double bmin, double bmax);
void gml_part_type_color_mix(GmlParticleState *state, int id, uint32_t color1, uint32_t color2);
void gml_part_type_color_hsv(GmlParticleState *state, int id, double hmin, double hmax, double smin, double smax, double vmin, double vmax);
void gml_part_type_alpha(GmlParticleState *state, int id, int count, double alpha1, double alpha2, double alpha3);
void gml_part_type_blend(GmlParticleState *state, int id, int additive);

int  gml_part_system_create(GmlParticleState *state);
int  gml_part_system_exists(GmlParticleState *state, int id);
void gml_part_system_destroy(GmlParticleState *state, int id);
void gml_part_system_clear(GmlParticleState *state, int id);
void gml_part_system_position(GmlParticleState *state, int id, double x, double y);
void gml_part_system_automatic_update(GmlParticleState *state, int id, int enabled);
void gml_part_system_automatic_draw(GmlParticleState *state, int id, int enabled);
void gml_part_system_depth(GmlParticleState *state, int id, double depth);
int  gml_part_system_count(GmlParticleState *state, int id);
int  gml_part_system_auto_draw_nth(GmlParticleState *state, int nth, int *id, double *depth);
void gml_part_system_update(GmlParticleState *state, int id);
void gml_part_system_drawit(GmlParticleState *state, GmlRender *render, int id);

void gml_part_particles_create(GmlParticleState *state, int system, double x, double y, int type, int number);
void gml_part_particles_create_color(GmlParticleState *state, int system, double x, double y, int type, uint32_t color, int number);

int  gml_part_emitter_create(GmlParticleState *state, int system);
int  gml_part_emitter_exists(GmlParticleState *state, int system, int emitter);
void gml_part_emitter_destroy(GmlParticleState *state, int emitter);
void gml_part_emitter_destroy_all(GmlParticleState *state, int system);
void gml_part_emitter_clear(GmlParticleState *state, int system, int emitter);
void gml_part_emitter_region(GmlParticleState *state, int system, int emitter, double xmin, double xmax, double ymin, double ymax, int shape, int distribution);
void gml_part_emitter_burst(GmlParticleState *state, int system, int emitter, int type, int number);
void gml_part_emitter_stream(GmlParticleState *state, int system, int emitter, int type, int number);
void gml_effect_create(GmlParticleState *state, int above, int kind, double x, double y, int size, uint32_t color);

#endif
