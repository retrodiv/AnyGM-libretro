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
/* A renderer identity belongs to the current synthetic context; NULL tests an unavailable query. */
void anygm_test_graphics_renderer(const char *renderer);
/* A replacement GL namespace may recycle a numeric name from a lost context. Arrange that the
 * next generated object gets that otherwise free value, without restoring the old object. */
int anygm_test_graphics_reuse_name(unsigned name);
/* Refuse to resolve this entry point, so a missing one can be tested by name rather than by
 * deleting code. */
void anygm_test_graphics_withhold(const char *name);
/* Fail shader compilation or program linking, to exercise the refusal paths. */
void anygm_test_graphics_fail_compile(int failing);
void anygm_test_graphics_fail_link(int failing);
void anygm_test_graphics_texture_unit_limit(int units);
void anygm_test_graphics_active_uniform_limit(int count);
/* Anything reaching the driver after this is counted as a call that should not have happened. */
void anygm_test_graphics_forbid_calls(void);
void anygm_test_graphics_allow_calls(void);
/* Query results test ownership/call order only; real-device controls establish RGB pixels. */
void anygm_test_graphics_rgb_query_result(unsigned result);
void anygm_test_graphics_active_query(unsigned name);
unsigned anygm_test_graphics_rgb_queries(void);

/* The two callbacks a host supplies, in their raw shapes. A caller casts them into whichever
 * boundary it is testing; both boundaries declare the same two shapes. */
void (*anygm_test_graphics_proc(void *userdata,const char *name))(void);
uintptr_t anygm_test_graphics_framebuffer(void *userdata);

/* Observations. */
int anygm_test_graphics_deletes(void);
int anygm_test_graphics_calls_after_forget(void);
int anygm_test_graphics_draw_calls(void);
int anygm_test_graphics_scene_draw_calls(void);
int anygm_test_graphics_scene_vertices(void);
/* Number of scene draws made with this exact RGBA write mask, not a pixel emulator. */
unsigned anygm_test_graphics_scene_mask_draws(unsigned mask);
unsigned anygm_test_graphics_scene_texture(void);
int anygm_test_graphics_scene_filter(void);
void anygm_test_graphics_blend_factors(unsigned factors[4]);
/* Quads drawn through a content program, as opposed to the presentation triangle. */
int anygm_test_graphics_quad_draw_calls(void);
unsigned anygm_test_graphics_compile_calls(void);
unsigned anygm_test_graphics_flush_calls(void);
/* Exact per-channel font writes must pair the selected component with its colour mask,
 * and write alpha only with the final component. */
unsigned anygm_test_graphics_font_channel_draws(unsigned channel);
unsigned anygm_test_graphics_font_channel_errors(void);
void anygm_test_graphics_fail_scene_draw_at(unsigned ordinal);
/* Refuse data uploads to this texture without changing its stored bytes; zero disables it. */
void anygm_test_graphics_fail_texture_upload(unsigned name);
/* Texture bound to this sampler at the most recent scene draw, not shader emulation. */
unsigned anygm_test_graphics_scene_sampler_texture(unsigned unit);
/* Texture identity and minification filter captured at the last content draw. */
unsigned anygm_test_graphics_quad_texture(unsigned unit);
int anygm_test_graphics_quad_filter(unsigned unit);
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
/* Alpha returned by ordinary synthetic RGBA reads; reset restores opaque alpha. */
void anygm_test_graphics_readback_alpha(unsigned alpha);
/* Authored six-bit 3x2 mask and an absolute failing read ordinal. These model driver replies,
 * never the comparison shader; real-context controls prove which tiles actually change. */
void anygm_test_graphics_checkpoint_mask(unsigned bits);
void anygm_test_graphics_fail_readback_at(unsigned ordinal);
/* Persistent driver failure distinguishes a bounded retry from an endless destructor loop. */
void anygm_test_graphics_fail_all_readbacks(int failing);
void anygm_test_graphics_fail_copy_at(unsigned ordinal);
unsigned anygm_test_graphics_last_read_framebuffer(void);
unsigned anygm_test_graphics_last_copy_framebuffer(void);
/* Read-backs issued into a pack buffer, which do not wait for the device. */
unsigned anygm_test_graphics_async_read_pixels(void);
/* The texture coordinate of one corner of the last quad drawn through a content program. */
int anygm_test_graphics_quad_texcoord(int corner,float *u,float *v);
/* The vertex colour of one corner of the last quad drawn through a content program. */
int anygm_test_graphics_quad_colour(int corner,float *rgba);
/* The submitted position attribute, before the content's vertex program runs. */
int anygm_test_graphics_quad_position(int corner,float *xyz);
/* One of the last five content-program matrices, in the submitted column-major order. */
int anygm_test_graphics_content_matrix(unsigned index,float *matrix);
unsigned anygm_test_graphics_attached_texture(void);
int anygm_test_graphics_clear_calls(void);
unsigned anygm_test_graphics_framebuffer_queries(void);
int anygm_test_graphics_blend_enabled(void);
unsigned anygm_test_graphics_blend_configurations(void);
unsigned anygm_test_graphics_scene_copies(void);
uint64_t anygm_test_graphics_scene_copied_pixels(void);
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
int anygm_test_graphics_texture_deleted(unsigned name);
unsigned anygm_test_graphics_texture_data_uploads(unsigned name);
const unsigned char *anygm_test_graphics_texture_bytes(unsigned name,size_t *count);

#endif
