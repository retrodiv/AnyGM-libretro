/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me> */
/* gml_particle.h - Particle types, systems and emitters. */
#ifndef GML_PARTICLE_H
#define GML_PARTICLE_H
#include <stddef.h>
#include <stdint.h>
#include "gml_render.h"

void gml_part_reset_all(void);       /* clear all types/systems/emitters/particles */
void gml_part_update_all(void);      /* advance every auto-update system one step */
void gml_part_system_draw_all(GmlRender *r);   /* draw every auto-draw system */

size_t gml_part_state_size(void);
int gml_part_state_save(void *data, size_t len, size_t *written);
int gml_part_state_load(const void *data, size_t len, size_t *used);

int  gml_part_type_create(void);
int  gml_part_type_exists(int id);
void gml_part_type_destroy(int id);
void gml_part_type_clear(int id);
void gml_part_type_sprite(int id,int spr,int animate,int stretch,int random);
void gml_part_type_shape(int id,int shape);
void gml_part_type_size(int id,double mn,double mx,double incr,double wig);
void gml_part_type_scale(int id,double xs,double ys);
void gml_part_type_speed(int id,double mn,double mx,double incr,double wig);
void gml_part_type_direction(int id,double mn,double mx,double incr,double wig);
void gml_part_type_gravity(int id,double amt,double dir);
void gml_part_type_life(int id,double mn,double mx);
void gml_part_type_orientation(int id,double mn,double mx,double incr,double wig,int rel);
void gml_part_type_color(int id,int ncol,uint32_t c1,uint32_t c2,uint32_t c3);
void gml_part_type_color_rgb(int id,double rmin,double rmax,double gmin,double gmax,double bmin,double bmax);
void gml_part_type_color_mix(int id,uint32_t c1,uint32_t c2);
void gml_part_type_color_hsv(int id,double hmin,double hmax,double smin,double smax,double vmin,double vmax);
void gml_part_type_alpha(int id,int na,double a1,double a2,double a3);
void gml_part_type_blend(int id,int additive);

int  gml_part_system_create(void);
int  gml_part_system_exists(int id);
void gml_part_system_destroy(int id);
void gml_part_system_clear(int id);
void gml_part_system_position(int id,double x,double y);
void gml_part_system_automatic_update(int id,int on);
void gml_part_system_automatic_draw(int id,int on);
void gml_part_system_depth(int id,double depth);
int  gml_part_system_count(int id);
int  gml_part_system_auto_draw_nth(int nth,int *id,double *depth);
void gml_part_system_update(int id);
void gml_part_system_drawit(GmlRender *r, int id);

void gml_part_particles_create(int sysid,double x,double y,int type,int number);
void gml_part_particles_create_color(int sysid,double x,double y,int type,uint32_t col,int number);

int  gml_part_emitter_create(int sysid);
int  gml_part_emitter_exists(int sysid,int em);
void gml_part_emitter_destroy(int em);
void gml_part_emitter_destroy_all(int sysid);
void gml_part_emitter_clear(int sysid,int em);
void gml_part_emitter_region(int sysid,int em,double xmin,double xmax,double ymin,double ymax,int shape,int dist);
void gml_part_emitter_burst(int sysid,int em,int type,int number);
void gml_part_emitter_stream(int sysid,int em,int type,int number);
void gml_effect_create(int above,int kind,double x,double y,int size,uint32_t color);

#endif
