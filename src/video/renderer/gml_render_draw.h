/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef GML_RENDER_DRAW_H
#define GML_RENDER_DRAW_H

#include <stddef.h>
#include <stdint.h>

struct GmlPlanShader;

enum { GML_RENDER_MAX_SAMPLERS=8, GML_RENDER_MAX_UNCERTAIN_TARGETS=8 };

/* Resolved, API-neutral work. No entry may look up mutable content after submission. */
typedef enum GmlRenderOperationCode {
#define GML_RENDER_OPERATION(name, family) GML_RENDER_OP_##name,
#include "gml_render_operations.def"
#undef GML_RENDER_OPERATION
  GML_RENDER_OPERATION_COUNT
} GmlRenderOperationCode;

enum {
  GML_RENDER_FAMILY_TEXTURE=1u<<0,
  GML_RENDER_FAMILY_PRIMITIVE=1u<<1,
  GML_RENDER_FAMILY_SURFACE=1u<<2,
  GML_RENDER_FAMILY_EFFECT=1u<<3,
  GML_RENDER_FAMILY_PRESENTATION=1u<<4,
  GML_RENDER_FAMILY_SHADER=1u<<5,
  GML_RENDER_FAMILY_SOFTWARE3D=1u<<6
};

typedef enum GmlRenderResourceClass {
  GML_RENDER_RESOURCE_SCREEN=1,
  GML_RENDER_RESOURCE_APPLICATION=2,
  GML_RENDER_RESOURCE_SURFACE=3,
  GML_RENDER_RESOURCE_ATLAS=4,
  GML_RENDER_RESOURCE_SPRITE=5,
  GML_RENDER_RESOURCE_SCRATCH=6
} GmlRenderResourceClass;

typedef enum GmlRenderPixelFormat {
  GML_RENDER_PIXEL_XRGB8888=1,
  GML_RENDER_PIXEL_ARGB8888=2,
  GML_RENDER_PIXEL_RGBA8888=3
} GmlRenderPixelFormat;

typedef struct GmlRenderRect {
  int32_t x,y;
  uint32_t width,height;
} GmlRenderRect;

typedef struct GmlRenderImageRef {
  uint32_t resource_class;
  uint32_t identity;
  uint64_t incarnation;
  uint64_t content_version;
} GmlRenderImageRef;

typedef struct GmlRenderTargetRef {
  GmlRenderImageRef image;
  uint64_t storage_version;
  uint32_t width,height;
  uint32_t pixel_format;
  GmlRenderRect region;
} GmlRenderTargetRef;

typedef enum GmlRenderFilter {
  GML_RENDER_FILTER_NEAREST=1,
  GML_RENDER_FILTER_BILINEAR=2,
  GML_RENDER_FILTER_BOX=3,
  GML_RENDER_FILTER_NEAREST_PHASED=4,
  GML_RENDER_FILTER_PRESENTATION=4,
  GML_RENDER_FILTER_NEAREST_PHASED_COPY=5,
  GML_RENDER_FILTER_NEAREST_PHASED_OPAQUE_ALPHA=6
} GmlRenderFilter;

typedef enum GmlRenderEdgeRule {
  GML_RENDER_EDGE_TRANSPARENT=1,
  GML_RENDER_EDGE_CLAMP_SUBIMAGE=2,
  GML_RENDER_EDGE_REPEAT_SUBIMAGE=3,
  /* A packed atlas cell exposes physical neighbour texels only for taps crossing an authored
   * logical margin; every other tap clamps to its visible source rectangle. */
  GML_RENDER_EDGE_LOGICAL_TPAG=4
} GmlRenderEdgeRule;

enum {
  GML_RENDER_MARGIN_LEFT=1u,
  GML_RENDER_MARGIN_TOP=2u,
  GML_RENDER_MARGIN_RIGHT=4u,
  GML_RENDER_MARGIN_BOTTOM=8u,
  /* General-sprite vertex colours cover the authored selection, while its CPU sampler skips
   * fragment centres outside the visible packed page before any bilinear edge taps. */
  GML_RENDER_MARGIN_VISIBLE_CENTRE=32u
};

typedef struct GmlRenderSamplingPolicy {
  uint32_t filter;
  uint32_t edge_rule;
  GmlRenderRect source_region;
  uint32_t quantisation;
  uint32_t logical_margin_mask;
} GmlRenderSamplingPolicy;

/* Resolved blend semantics, independent of any device API's enumerant values. Zeroed factors
 * and equations describe disabled blending; unsupported combinations are rejected before writes. */
typedef enum GmlRenderBlendFactor {
  GML_RENDER_BLEND_ZERO=1,
  GML_RENDER_BLEND_ONE=2,
  GML_RENDER_BLEND_SOURCE_ALPHA=3,
  GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA=4
} GmlRenderBlendFactor;

typedef enum GmlRenderBlendEquation {
  GML_RENDER_BLEND_ADD=1,
  /* Source-over with eight-bit factors and truncating division by 256. The destination must be
   * snapshotted before this operation; fixed-function blending is not this rule. */
  GML_RENDER_BLEND_EXACT8_TRUNC=2
} GmlRenderBlendEquation;

typedef struct GmlRenderDrawStateSnapshot {
  uint32_t rgb_source_factor,rgb_destination_factor;
  uint32_t alpha_source_factor,alpha_destination_factor;
  uint32_t rgb_equation,alpha_equation;
  uint32_t write_mask;
  uint32_t alpha_test_enabled,alpha_test_reference;
  uint32_t colour;
  uint8_t alpha;
  uint8_t coverage_policy;
  uint16_t sampler_count;
  GmlRenderRect clip;
  GmlRenderSamplingPolicy samplers[GML_RENDER_MAX_SAMPLERS];
  uint64_t material_identity,material_version;
} GmlRenderDrawStateSnapshot;

typedef enum GmlRenderQuadShape {
  GML_RENDER_QUAD_AXIS_ALIGNED=1,
  GML_RENDER_QUAD_CARDINAL=2,
  GML_RENDER_QUAD_GENERAL=3,
  GML_RENDER_QUAD_REPEATED=4,
  GML_RENDER_QUAD_WARPED=5
} GmlRenderQuadShape;

typedef struct GmlRenderQuad {
  double x[4],y[4];
  /* WARPED carries page-local texel coordinates in u/v; other nonrotated
   * quads carry normalized source UVs. */
  double u[4],v[4];
  /* Rotated checkpoint: local = (pixel centre - anchor) * [x,y] + offset. */
  double inverse_anchor[2],inverse_x[2],inverse_y[2],inverse_offset[2];
  /* Modern filtered rotation optionally limits each row to the CPU quad span. Axis-aligned
   * box-filtered stretches instead carry [logical size, destination size, trim, visible size]
   * in these four components per axis, with span_active left clear. */
  double span_x[4],span_y[4];
  uint32_t span_active;
  uint32_t corner_colour[4];
  uint32_t shape;
  GmlRenderSamplingPolicy sampling;
} GmlRenderQuad;

typedef struct GmlRenderCopyRegion {
  GmlRenderRect source,destination;
  uint32_t needs_source_snapshot;
} GmlRenderCopyRegion;

/* Fully resolved flat-colour coverage. The rectangle is a half-open pixel region in target
 * coordinates; it does not borrow a texture or mutable draw state after submission. */
typedef struct GmlRenderPrimitiveRect {
  GmlRenderRect region;
  uint32_t colour;
} GmlRenderPrimitiveRect;

typedef enum GmlRenderReconstructionMode {
  GML_RENDER_REPLAY_EXACT=1,
  GML_RENDER_DEVICE_CHECKPOINT=2,
  GML_RENDER_CPU_ONLY=3
} GmlRenderReconstructionMode;

typedef enum GmlRenderSceneFamily {
  GML_RENDER_SCENE_UNCLASSIFIED=0,
  GML_RENDER_SCENE_SPRITE=1,
  GML_RENDER_SCENE_BACKGROUND=2,
  GML_RENDER_SCENE_TILE=3,
  GML_RENDER_SCENE_SURFACE=4,
  GML_RENDER_SCENE_TEXT=5,
  GML_RENDER_SCENE_PRIMITIVE=6,
  GML_RENDER_SCENE_FAMILY_COUNT=7
} GmlRenderSceneFamily;

typedef struct GmlRenderDrawOp {
  uint64_t sequence;
  uint32_t opcode;
  uint32_t state_index;
  uint32_t reconstruction_mode;
  uint32_t scene_family;
  uint32_t source_count;
  GmlRenderTargetRef target;
  const GmlRenderImageRef *sources;
  union {
    uint32_t clear_colour;
    GmlRenderQuad quad;
    GmlRenderCopyRegion copy;
    GmlRenderPrimitiveRect primitive;
    /* Lent through this submission. Shader output is non-reconstructible and must checkpoint. */
    const struct GmlPlanShader *shader;
  } payload;
} GmlRenderDrawOp;

/* Snapshots and per-operation source arrays are immutable for this submission. The caller owns
 * every referenced array until submit returns; a reconstruction record must copy or pin anything
 * it still needs afterward. */
typedef struct GmlRenderDrawBatch {
  const GmlRenderDrawOp *operations;
  uint32_t operation_count;
  const GmlRenderDrawStateSnapshot *states;
  uint32_t state_count;
} GmlRenderDrawBatch;

/* Upload borrows bytes only during the call. Readback writes caller-owned bytes and completes
 * before returning success; asynchronous transfers require a separate future contract. */
typedef struct GmlRenderImageUpload {
  GmlRenderImageRef image;
  uint32_t storage_width,storage_height;
  GmlRenderRect region;
  uint32_t pixel_format;
  uint32_t stride_bytes;
  size_t byte_capacity;
  const void *bytes;
} GmlRenderImageUpload;

typedef struct GmlRenderImageReadback {
  GmlRenderImageRef image;
  uint32_t storage_width,storage_height;
  GmlRenderRect region;
  uint32_t pixel_format;
  uint32_t stride_bytes;
  size_t byte_capacity;
  void *bytes;
} GmlRenderImageReadback;

typedef struct GmlRenderDeviceCaps {
  uint32_t max_texture_extent;
  uint32_t max_batch_quads;
  uint64_t supported_operations;
  uint32_t supported_families;
  uint32_t supports_rgb_alpha_separate;
  uint64_t soft_cache_bytes;
} GmlRenderDeviceCaps;

typedef enum GmlRenderExecutionStatus {
  GML_RENDER_EXEC_ACCEPTED=1,
  GML_RENDER_EXEC_UNSUPPORTED=2,
  GML_RENDER_EXEC_RECOVERABLE_ERROR=3,
  GML_RENDER_EXEC_RESOURCE_ERROR=4
} GmlRenderExecutionStatus;

/* An accepted prefix is ordered and may still be pending on the device. On an error, every
 * potentially modified target must be named, or all_destinations_uncertain must be set. */
typedef struct GmlRenderExecutionResult {
  uint32_t status;
  uint32_t accepted_prefix;
  uint32_t uncertain_target_count;
  uint32_t all_destinations_uncertain;
  GmlRenderImageRef uncertain_targets[GML_RENDER_MAX_UNCERTAIN_TARGETS];
} GmlRenderExecutionResult;

#endif
