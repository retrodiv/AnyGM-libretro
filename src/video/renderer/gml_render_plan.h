/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef GML_RENDER_PLAN_H
#define GML_RENDER_PLAN_H

#include <stddef.h>
#include <stdint.h>

/* A bounded, value-copy description of coarse render work: what a pass must produce, not how a
 * particular graphics API produces it. No graphics API type, no host callback and no function
 * pointer appears here, and nothing in this header is serialized.
 *
 * The plan exists so that one pass can be executed either by the exact software kernels or, when
 * every one of its operations is supported exactly, by a GPU backend. Both consumers derive their
 * sampling from the same axis rules below, so the source texel a destination pixel selects has one
 * definition rather than two that can drift. */

typedef enum GmlPlanPixelFormat {
  GML_PLAN_PIXEL_XRGB8888=0
} GmlPlanPixelFormat;

/* The resource classes a plan may reference. Identity plus generation, never a bare pointer:
 * a freed resource's address can be handed out again, and a GPU mirror keyed on it would then
 * describe the wrong pixels. */
typedef enum GmlPlanImageClass {
  GML_PLAN_IMAGE_NONE=0,
  GML_PLAN_IMAGE_COMPLETED_FRAME=1,
  GML_PLAN_IMAGE_APPLICATION_SURFACE=2,
  /* A runtime surface other than the application surface, by surface id. */
  GML_PLAN_IMAGE_SURFACE=3,
  /* A sprite frame the content bound to a shader sampler, identity = sprite<<10 | frame. */
  GML_PLAN_IMAGE_SPRITE=4
} GmlPlanImageClass;

typedef struct GmlPlanImage {
  uint32_t image_class;
  uint32_t identity;
  uint32_t content_generation;
  uint32_t pixel_generation;
  uint32_t width;
  uint32_t height;
  uint32_t pitch_pixels;
  uint32_t pixel_format;
  uint32_t opaque;
  /* A borrow that is valid for the frame in which the plan was built, and for as long afterwards
   * as the owner keeps the plan retained. NULL when the owner did not lend the pixels. */
  const uint32_t *cpu_pixels;
} GmlPlanImage;

typedef enum GmlPlanAxisRule {
  /* The doubled-accumulator recurrence the completed-frame magnification walks: exact integer
   * arithmetic, defined only where the destination extent is at least the source extent. */
  GML_PLAN_AXIS_ACCUMULATOR=0,
  /* floor(((destination + 0.5) - origin) * source_extent / extent), clamped into the source. This
   * is the fixed-function viewport convention a content-owned presentation reproduces, and its
   * fractional origin and extent are why it is not the recurrence above. */
  GML_PLAN_AXIS_PIXEL_CENTRE=1,
  /* (destination * source_extent) / destination_extent in integer arithmetic, clamped. The runtime's
   * automatic application-surface presentation anchors at the leading output edge rather than at
   * the pixel centre; the two disagree by one source texel at most fractional boundaries, which is
   * exactly why the rules are named separately instead of one standing in for the other. */
  GML_PLAN_AXIS_LEADING_EDGE=2
} GmlPlanAxisRule;

typedef struct GmlPlanAxis {
  uint32_t rule;
  uint32_t source_extent;
  /* The destination extent the rule is defined over, before clipping. Clipping changes which
   * destination indices are written, never which source index one of them selects. */
  uint32_t destination_extent;
  /* The first destination index actually written, in the same space as destination_extent. */
  int32_t destination_first;
  double origin;
  double extent;
} GmlPlanAxis;

typedef enum GmlPlanOpcode {
  GML_PLAN_OP_CLEAR_XRGB=0,
  GML_PLAN_OP_BLIT_OPAQUE_NEAREST=1,
  /* The box-averaging reduction. Software-only: an ordinary bilinear sample is not equivalent, and
   * no GPU reduction has been characterized against its integer channel sums and division. */
  GML_PLAN_OP_BLIT_OPAQUE_BOX=2,
  /* Fallback transport: present a complete CPU frame unchanged. */
  GML_PLAN_OP_PRESENT_CPU_FRAME=3,
  /* Draw the source through the content's own shader program, described by the plan's shader
   * record: the program text the content ships, the uniforms the content set, and the samplers it
   * bound. GPU-only: there is no software equivalent, and a plan carrying it that cannot run on
   * the device is replayed as the unshaded presentation the renderer recorded. */
  GML_PLAN_OP_SHADER_DRAW=4,
  /* Software sharp bilinear: integer nearest prescale followed by bilinear at pixel centres.
   * The shared prescale is ceil(min(destination_width/source_width,
   * destination_height/source_height)), at least one. */
  GML_PLAN_OP_BLIT_OPAQUE_SHARP_BILINEAR=5
} GmlPlanOpcode;

typedef struct GmlPlanRect {
  int32_t x;
  int32_t y;
  uint32_t width;
  uint32_t height;
} GmlPlanRect;

typedef struct GmlPlanOp {
  uint32_t opcode;
  uint32_t source;
  GmlPlanRect destination;
  GmlPlanAxis axis_x;
  GmlPlanAxis axis_y;
  uint32_t clear_color;
  /* The byte written into the top channel of an XRGB destination. The completed-frame
   * magnification clears it and the application-surface presentation sets it; neither is read as
   * coverage, and stating it keeps a GPU execution from having to guess. */
  uint32_t alpha_write;
  /* SHADER_DRAW: the source is sampled linearly rather than by nearest texel. */
  uint32_t linear;
  /* SHADER_DRAW: the region of the source image the destination shows, in source pixels. A zero
   * width or height means the whole image, which is what a plain surface presentation wants. */
  GmlPlanRect source_rect;
} GmlPlanOp;

enum {
  GML_PLAN_UNIFORM_NAME=32,
  GML_PLAN_MAX_UNIFORMS=32,
  GML_PLAN_MAX_SAMPLERS=4,
  GML_PLAN_UNIFORM_VALUES=16
};

/* One uniform the content set on its shader: by name, because the program is the content's own and
 * the runtime knows none of its controls. `count` is the number of components (1..4 for a vector,
 * 16 for a matrix); `integer` says the values were set through the integer setter. */
typedef struct GmlPlanUniform {
  char name[GML_PLAN_UNIFORM_NAME];
  float value[GML_PLAN_UNIFORM_VALUES];
  uint32_t count;
  uint32_t integer;
} GmlPlanUniform;

/* A sampler declared by the content's fragment program and the plan image bound to it. */
typedef struct GmlPlanSampler {
  char name[GML_PLAN_UNIFORM_NAME];
  uint32_t image;
} GmlPlanSampler;

/* The content's shader program as the plan carries it. The four sources are lent from the content
 * image, NUL-terminated: the OpenGL ES 1.00 pair every Studio payload ships and the desktop GLSL
 * pair beside it. The backend picks the pair its context accepts and translates when neither is
 * accepted as written. The identity keys the compiled program's cache. */
typedef struct GmlPlanShader {
  uint32_t identity;
  uint32_t content_generation;
  const char *vertex_es;
  const char *fragment_es;
  const char *vertex_gl;
  const char *fragment_gl;
  uint32_t uniform_count;
  GmlPlanUniform uniforms[GML_PLAN_MAX_UNIFORMS];
  uint32_t sampler_count;
  /* The colour the drawing carried, as the quad's vertex colour: a program multiplies its answer
   * by it after program evaluation rather than before. RGBA in
   * 0..1; all ones when the caller has nothing to say. */
  float vertex_colour[4];
  GmlPlanSampler samplers[GML_PLAN_MAX_SAMPLERS];
} GmlPlanShader;

typedef enum GmlPlanTarget {
  GML_PLAN_TARGET_CPU_FRAME=0,
  GML_PLAN_TARGET_HOST_FRAMEBUFFER=1,
  /* An off-screen target of the plan's extent whose pixels are read back into the caller's plane
   * once the plan has executed: a content program run in the middle of a frame, whose result the
   * software renderer composes like any other picture. */
  GML_PLAN_TARGET_READBACK=2
} GmlPlanTarget;

/* Why a pass was not executed on a GPU. A stable internal enum; the strings exist only for
 * opt-in diagnostics. */
typedef enum GmlPlanFallback {
  GML_PLAN_FALLBACK_NONE=0,
  GML_PLAN_FALLBACK_UNSUPPORTED_OPERATION=1,
  GML_PLAN_FALLBACK_SOURCE_ALIAS=2,
  GML_PLAN_FALLBACK_SOURCE_LIFETIME=3,
  GML_PLAN_FALLBACK_BOX_REDUCTION=4,
  GML_PLAN_FALLBACK_BLEND_NOT_EXACT=5,
  GML_PLAN_FALLBACK_SURFACE_READBACK=6,
  GML_PLAN_FALLBACK_PLAN_OVERFLOW=7,
  GML_PLAN_FALLBACK_CONTEXT_UNAVAILABLE=8,
  GML_PLAN_FALLBACK_RESOURCE_UPLOAD_FAILURE=9,
  /* The content's shader program did not compile or link on this context. */
  GML_PLAN_FALLBACK_SHADER_FAILURE=10,
  GML_PLAN_FALLBACK_COUNT=11
} GmlPlanFallback;

enum {
  GML_PLAN_MAX_IMAGES=8,
  GML_PLAN_MAX_OPERATIONS=8,
  GML_PLAN_NO_IMAGE=0xFFFFFFFFu,
  /* Every extent a plan may describe. Larger than the engine's own framebuffer bound, so the plan
   * rejects an impossible value rather than inheriting one. */
  GML_PLAN_MAX_EXTENT=16384u
};

typedef struct GmlRenderPlan {
  uint32_t target;
  uint32_t target_width;
  uint32_t target_height;
  uint32_t terminal;
  uint32_t image_count;
  GmlPlanImage images[GML_PLAN_MAX_IMAGES];
  uint32_t operation_count;
  GmlPlanOp operations[GML_PLAN_MAX_OPERATIONS];
  uint32_t overflowed;
  uint32_t fallback_reason;
  /* Present when an operation is SHADER_DRAW. One per plan: the frame's terminal presentation. */
  uint32_t has_shader;
  GmlPlanShader shader;
  /* READBACK target only: where the executed target lands, rows from the top, in XRGB/ARGB words. */
  uint32_t *readback_pixels;
  uint32_t readback_pitch_pixels;
  /* The part of the target that is read back. A zero extent means the whole target. */
  GmlPlanRect readback_rect;
} GmlRenderPlan;

void gml_render_plan_reset(GmlRenderPlan *plan,uint32_t target,
                           uint32_t target_width,uint32_t target_height);

/* Returns the image index, or GML_PLAN_NO_IMAGE when the table is full or the record is not
 * self-consistent. A refused image marks the plan overflowed so a caller that ignores the result
 * still cannot execute a plan that is missing a source. */
uint32_t gml_render_plan_add_image(GmlRenderPlan *plan,const GmlPlanImage *image);

int gml_render_plan_add_clear(GmlRenderPlan *plan,GmlPlanRect destination,uint32_t color);
int gml_render_plan_add_blit_nearest(GmlRenderPlan *plan,uint32_t source,
                                     GmlPlanRect destination,
                                     GmlPlanAxis axis_x,GmlPlanAxis axis_y,
                                     uint32_t alpha_write);
int gml_render_plan_add_blit_box(GmlRenderPlan *plan,uint32_t source,
                                 GmlPlanRect destination,uint32_t alpha_write);
int gml_render_plan_add_blit_sharp_bilinear(GmlRenderPlan *plan,uint32_t source,
                                            GmlPlanRect destination,uint32_t alpha_write);
/* The terminal presentation drawn through the content's own program. The shader record is copied
 * into the plan; its source pointers stay lent. */
/* `source_rect` names the part of the source the destination shows; pass a zero-sized rectangle
 * for the whole image. */
int gml_render_plan_add_shader_draw_part(GmlRenderPlan *plan,uint32_t source,GmlPlanRect destination,
                                         GmlPlanRect source_rect,const GmlPlanShader *shader,
                                         uint32_t linear);
int gml_render_plan_add_shader_draw(GmlRenderPlan *plan,uint32_t source,GmlPlanRect destination,
                                    const GmlPlanShader *shader,uint32_t linear);
int gml_render_plan_add_present_cpu_frame(GmlRenderPlan *plan,uint32_t source,
                                          GmlPlanRect destination);

/* Complete structural validation: bounded extents, overflow-safe rectangle and pitch arithmetic,
 * destinations inside the target, sources inside their images, and a lent CPU pointer wherever
 * software execution or upload will need one. */
int gml_render_plan_validate(const GmlRenderPlan *plan);

/* The source index each destination index selects, written into map[0..count-1] where map index i
 * corresponds to destination index axis->destination_first + i. Returns zero when the axis is not
 * usable, in which case nothing is written. */
int gml_render_plan_axis_map(const GmlPlanAxis *axis,uint16_t *map,uint32_t count);

/* map[i] = inner[outer[i]]: the source index a destination reaches through two nearest mappings in
 * sequence. Composing the indices is exact because nearest sampling is a lookup, and it is what
 * lets one pass replace two without a full-size intermediate target. */
int gml_render_plan_compose_maps(const uint16_t *outer,const uint16_t *inner,
                                 uint16_t *map,uint32_t count,uint32_t inner_count);

/* Every operation is one a GPU backend has been proved to reproduce exactly. Sets the plan's
 * fallback reason when it is not. */
int gml_render_plan_gpu_eligible(GmlRenderPlan *plan);

const char *gml_render_plan_fallback_name(uint32_t reason);

/* Exact software execution into a caller-owned XRGB8888 target. Returns zero without writing
 * anything when the plan does not validate. */
int gml_render_plan_execute_software(const GmlRenderPlan *plan,uint32_t *target,
                                     uint32_t target_pitch_pixels);

/* The same execution, with row-independent full-frame operations split across the renderer's
 * row-band pool when one is offered and the destination is large enough to pay for the fork.
 * The renderer is a pool owner here and nothing else; NULL executes on the calling thread and is
 * exactly the function above. Identical bytes either way, at any band count. */
struct GmlRender;
int gml_render_plan_execute_software_pooled(const GmlRenderPlan *plan,uint32_t *target,
                                            uint32_t target_pitch_pixels,
                                            struct GmlRender *pool_owner);

#endif
