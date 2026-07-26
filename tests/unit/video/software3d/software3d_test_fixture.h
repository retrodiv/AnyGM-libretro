/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef ANYGM_SOFTWARE3D_TEST_FIXTURE_H
#define ANYGM_SOFTWARE3D_TEST_FIXTURE_H

#include "gml_vm.h"
#include "gml_render.h"
#include "gml_render_internal.h"
#include "gml_particle.h"
#include "gml_builtin.h"
#include "anygm_vfs.h"
#include "stdio_vfs.h"
#include "anygm_test_runner.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { SOFTWARE3D_WIDTH=64, SOFTWARE3D_HEIGHT=48 };

typedef struct {
  uint32_t pixels[SOFTWARE3D_WIDTH*SOFTWARE3D_HEIGHT];
  GmlRender render;
  GmlVM vm;
  AnygmHostServices file_services;
  int surface;
  int runtime_sprite;
  int runtime_sprite_2;
  int depth_sprite;
  int flipped_sprite;
} Software3dRasterFixture;

typedef int (*Software3dRasterStage)(Software3dRasterFixture *fixture);

extern int fixture_key;
extern int fixture_key_edge;
extern uint32_t fixture_network_connected;
extern double fixture_mouse_x;
extern double fixture_mouse_y;
extern double fixture_mouse_set_x;
extern double fixture_mouse_set_y;
extern const double software3d_ortho[];
extern const double software3d_enable[];
extern const double software3d_disable[];

uint32_t fixture_capability(void *userdata,uint32_t capability);
void fixture_attach_input(GmlVM *vm);
void call_numbers(GmlVM *vm,const char *name,const double *numbers,int count);
GmlVal call_values(GmlVM *vm,const char *name,GmlVal *args,int count);
int colored_pixels(const uint32_t *pixels,int count);
void free_extension_fixture(GmlVM *vm);
void store_u32le(uint8_t *dst,uint32_t value);
size_t classic_information_record(uint8_t *dst,size_t capacity);
int pushref_function_fixture(void);
int member_function_self_fixture(void);
int member_function_argument_fixture(void);

int software3d_operation_boundary_fixture(void);
int renderer_state_boundary_fixture(void);
int asset_lookup_fixture(void);
int software3d_state_roundtrip_case(void);
int software3d_case_language(Software3dRasterFixture *fixture);
int software3d_case_particles(Software3dRasterFixture *fixture);
int software3d_case_projection(Software3dRasterFixture *fixture);
int software3d_case_assets(Software3dRasterFixture *fixture);
int software3d_case_vm_draw(Software3dRasterFixture *fixture);
int software3d_case_blend_shader(Software3dRasterFixture *fixture);
int software3d_case_primitive_model(Software3dRasterFixture *fixture);
int software3d_raster_run_through(size_t final_stage);

#endif
