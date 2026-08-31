/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef ANYGM_TEST_GRAPHICS_DRIVER_FIXTURE_H
#define ANYGM_TEST_GRAPHICS_DRIVER_FIXTURE_H

#include <stddef.h>
#include <stdint.h>

/* A graphics driver written here, so the backend's lifecycle and its call sequence can be exercised
 * without one being installed.
 *
 * What a fake driver can prove is exactly what a real one makes awkward: that every entry point is
 * resolved through the host's callback and a missing one is named and refused, that a context which
 * disappeared is forgotten rather than deleted into its successor, that a destroy outside a current
 * context issues no call at all, and that every piece of shared state a pass depends on is set
 * rather than inherited. What it cannot prove is which pixel comes out; that question needs a separate
 * real-device framebuffer comparison against the software executor.
 *
 * One copy, used by the focused graphics suite and by the engine-level state suite, because two
 * fakes of one driver drift and the one that drifts is always the one nobody is reading. */

void anygm_test_graphics_reset(void);
/* Refuse to resolve this entry point, so a missing one can be tested by name rather than by
 * deleting code. */
void anygm_test_graphics_withhold(const char *name);
/* Fail shader compilation or program linking, to exercise the refusal paths. */
void anygm_test_graphics_fail_compile(int failing);
void anygm_test_graphics_fail_link(int failing);
/* Anything reaching the driver after this is counted as a call that should not have happened. */
void anygm_test_graphics_forbid_calls(void);

/* The two callbacks a host supplies, in their raw shapes. A caller casts them into whichever
 * boundary it is testing; both boundaries declare the same two shapes. */
void (*anygm_test_graphics_proc(void *userdata,const char *name))(void);
uintptr_t anygm_test_graphics_framebuffer(void *userdata);

/* Observations. */
int anygm_test_graphics_deletes(void);
int anygm_test_graphics_calls_after_forget(void);
int anygm_test_graphics_draw_calls(void);
/* Quads drawn through a content program, as opposed to the presentation triangle. */
int anygm_test_graphics_quad_draw_calls(void);
/* The most recent source text handed to the driver for the vertex (0) or fragment (1) stage. */
const char *anygm_test_graphics_shader_source(int fragment);
/* Resolve no vertex attribute, so a program without in_Position can be tested. */
void anygm_test_graphics_withhold_attributes(int withhold);
/* Attribute arrays left enabled: a content draw must leave none behind. */
int anygm_test_graphics_attributes_enabled(void);
unsigned anygm_test_graphics_bound_buffer(void);
unsigned anygm_test_graphics_float_uniforms(void);
unsigned anygm_test_graphics_matrix_uniforms(void);
/* Read-backs performed, and the texture attached to the off-screen target (0 for none). */
unsigned anygm_test_graphics_read_pixels(void);
/* Read-backs issued into a pack buffer, which do not wait for the device. */
unsigned anygm_test_graphics_async_read_pixels(void);
/* The texture coordinate of one corner of the last quad drawn through a content program. */
int anygm_test_graphics_quad_texcoord(int corner,float *u,float *v);
/* The vertex colour of one corner of the last quad drawn through a content program. */
int anygm_test_graphics_quad_colour(int corner,float *rgba);
unsigned anygm_test_graphics_attached_texture(void);
int anygm_test_graphics_clear_calls(void);
unsigned anygm_test_graphics_framebuffer_queries(void);
int anygm_test_graphics_blend_enabled(void);
int anygm_test_graphics_depth_enabled(void);
int anygm_test_graphics_stencil_enabled(void);
int anygm_test_graphics_cull_enabled(void);
int anygm_test_graphics_dither_enabled(void);
int anygm_test_graphics_scissor_enabled(void);
int anygm_test_graphics_color_mask_all(void);
int anygm_test_graphics_depth_mask(void);
unsigned anygm_test_graphics_bound_program(void);
unsigned anygm_test_graphics_bound_vertex_array(void);
int anygm_test_graphics_unpack_row_length(void);
void anygm_test_graphics_viewport(int *x,int *y,int *width,int *height);
void anygm_test_graphics_scissor(int *x,int *y,int *width,int *height);
void anygm_test_graphics_clear_colour(float *red,float *green,float *blue,float *alpha);
/* Leave graphics state set the way a frontend that shares the context might. */
void anygm_test_graphics_disturb(void);

/* The most recent, and the first, texture created with this internal format, or zero. The formats
 * are the two the backend uses: a normalized eight-bit source and an unsigned integer axis map, and
 * a pass creates one map per axis, so which of them is being read has to be stated. */
unsigned anygm_test_graphics_texture_with_format(int internal_format);
unsigned anygm_test_graphics_first_texture_with_format(int internal_format);
int anygm_test_graphics_texture_width(unsigned name);
int anygm_test_graphics_texture_height(unsigned name);
unsigned anygm_test_graphics_texture_uploads(unsigned name);
const unsigned char *anygm_test_graphics_texture_bytes(unsigned name,size_t *count);

#endif
