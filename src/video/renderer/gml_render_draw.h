/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef GML_RENDER_DRAW_H
#define GML_RENDER_DRAW_H

#include <stddef.h>
#include <stdint.h>

/* Keep the point writer's binary64 addition before division. Device axis
 * preparation reuses this expression instead of approximating texel ties. */
static inline int gml_render_point_axis_index(int pixel,double phase,double scale){
  return (int)((pixel+phase)/scale);
}

/* Preserve the nearest rotation writer's binary64 evaluation and truncation.
 * Device preparation uploads these 256 source-alpha factors, not pixels. */
static inline uint32_t gml_render_rotated_alpha16_factor(unsigned source_alpha,
                                                         double draw_alpha){
  double sa=(source_alpha/255.0)*draw_alpha;
  return sa>=1.0?65536u:(uint32_t)(sa*65536.0);
}

struct GmlPlanShader;

/* Draw coordinates before the renderer's target mapping. The signed extents follow the source
 * corners, including a mirrored composition. Ephemeral shader input, never canonical state. */
typedef struct GmlRenderShaderGeometry {
  double origin_x,origin_y,extent_x,extent_y;
  uint32_t valid;
} GmlRenderShaderGeometry;

/* Admission proof for the retained opaque stretch rule, including the CPU
 * band's double-to-float phase boundary. No target pixels are read or written. */
int gml_render_stretched_float_phase_exact(uint32_t logical,uint32_t destination);
int gml_render_stretched_float_visible_phase_exact(uint32_t logical,uint32_t destination,
                                                  uint32_t first,uint32_t count);

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
  GML_RENDER_RESOURCE_SCRATCH=6,
  /* A per-draw content-shader binding. Its CPU snapshot is oriented like a scene
   * render target and is retired after submission. */
  GML_RENDER_RESOURCE_SHADER_SAMPLER=7,
  /* A CPU-authored primary content source retains the terminal program's
   * texture-coordinate convention rather than the render-target convention. */
  GML_RENDER_RESOURCE_SHADER_SOURCE=8
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
  GML_RENDER_FILTER_NEAREST_PHASED_OPAQUE_ALPHA=6,
  /* Device filtering with the composed RGBA tolerance, not an exact replay recipe. */
  GML_RENDER_FILTER_HARDWARE_BILINEAR=7,
  /* Content-owned application presentation uses destination pixel centres. */
  GML_RENDER_FILTER_HARDWARE_PIXEL_CENTRE_POINT=8
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

enum {
  /* Stretched magnification: integral logical/destination sizes
   * prepare a rational phase and the CPU band's four float weights. */
  GML_RENDER_QUANTISE_STRETCHED_FLOAT=1u,
  /* Real-font four-tap channels retain the CPU double expression and generation blend. */
  GML_RENDER_QUANTISE_FONT_FIRSTGEN=2u,
  GML_RENDER_QUANTISE_FONT_MODERN=3u,
  /* First-generation SCREEN/numbered coverage rounds the extent independently,
   * while keeping the same first-generation channel and blend arithmetic. */
  GML_RENDER_QUANTISE_FONT_FIRSTGEN_INDEPENDENT=4u,
  /* Host-owned full application presentation, before subsequent SCREEN draws. */
  GML_RENDER_QUANTISE_AUTOMATIC_SURFACE_FIRSTGEN=5u,
  GML_RENDER_QUANTISE_AUTOMATIC_SURFACE_MODERN=6u,
  /* Ordinary first-generation SCREEN sprite sampling reuses the exact
   * filtered channel/blend rule, with independent extent and TPAG margins. */
  GML_RENDER_QUANTISE_SPRITE_FIRSTGEN_INDEPENDENT=7u,
  /* Unit-white application draws round the complete source-over sum in the
   * framebuffer, rather than quantising the source contribution separately. */
  GML_RENDER_QUANTISE_HARDWARE_COMBINED=8u,
  /* Modern numbered point sprites round tint and the complete source-over sum. */
  GML_RENDER_QUANTISE_SPRITE_MODERN_NEAREST=9u,
  /* Modern numbered rotation: CPU fixed-point texel spans and truncating exact16 blend. */
  GML_RENDER_QUANTISE_SPRITE_MODERN_ROTATED_NEAREST=10u
};

/* Resolved blend semantics, independent of any device API's enumerant values. Zeroed factors
 * and equations describe disabled blending; unsupported combinations are rejected before writes. */
typedef enum GmlRenderBlendFactor {
  GML_RENDER_BLEND_ZERO=1,
  GML_RENDER_BLEND_ONE=2,
  GML_RENDER_BLEND_SOURCE_ALPHA=3,
  GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA=4,
  GML_RENDER_BLEND_SOURCE_COLOUR=5,
  GML_RENDER_BLEND_DESTINATION_COLOUR=6,
  GML_RENDER_BLEND_DESTINATION_ALPHA=7
} GmlRenderBlendFactor;

/* Software selectors for destination-dependent fixed-function factor pairs. */
enum {
  GML_RENDER_BLEND_DEST_SOURCE_COLOUR=9,
  GML_RENDER_BLEND_DEST_DEST_COLOUR=10,
  GML_RENDER_BLEND_DEST_ALPHA_DEST_COLOUR=11
};

static inline int gml_render_blend_uses_destination_colour(int mode){
  return mode>=GML_RENDER_BLEND_DEST_SOURCE_COLOUR &&
         mode<=GML_RENDER_BLEND_DEST_ALPHA_DEST_COLOUR;
}

typedef enum GmlRenderBlendEquation {
  GML_RENDER_BLEND_ADD=1,
  /* Source-over with eight-bit factors and truncating division by 256. The destination must be
   * snapshotted before this operation; fixed-function blending is not this rule. */
  GML_RENDER_BLEND_EXACT8_TRUNC=2,
  /* Saturated source-alpha additive RGB with truncation and opaque output alpha. */
  GML_RENDER_BLEND_EXACT8_ADDITIVE=3,
  /* Full-target straight-RGBA coarse veil over a known-opaque target: round the complete
   * source-over channel with denominator 255, preserving the CPU veil's double expression. */
  GML_RENDER_BLEND_EXACT8_ROUND_VEIL=4,
  GML_RENDER_BLEND_EXACT_FILTERED_FONT=5,
  GML_RENDER_BLEND_EXACT_AUTOMATIC_SURFACE=6,
  /* The stretched CPU band keeps unrounded float channels/alpha and writes
   * opaque coverage. Mix against an ordered device-local destination snapshot. */
  GML_RENDER_BLEND_EXACT_STRETCHED_FLOAT=7,
  /* Integer destination-factor surface channels: add, max, reverse subtract,
   * subtract and min. These are resolved CPU rules, not GL enum values. */
  GML_RENDER_BLEND_EXACT_DESTINATION_ADD=8,
  GML_RENDER_BLEND_EXACT_DESTINATION_MAX=9,
  GML_RENDER_BLEND_EXACT_DESTINATION_REVERSE_SUBTRACT=10,
  GML_RENDER_BLEND_EXACT_DESTINATION_SUBTRACT=11,
  GML_RENDER_BLEND_EXACT_DESTINATION_MIN=12
} GmlRenderBlendEquation;

enum {
  /* Physical-screen source-over forces opaque alpha only at source-covered texels.
   * Sampling and RGB composition keep their separately resolved rules. */
  GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA=1u,
  /* A first-generation nearest SCREEN SIMD packet blends its packed alpha
   * toward opaque, unlike the scalar tail's unconditional opaque coverage. */
  GML_RENDER_COVERAGE_ROTATED_VECTOR_ALPHA=2u,
  /* Fixed source-over preserves an already opaque target without a destination copy. */
  GML_RENDER_COVERAGE_OPAQUE_TARGET=3u
};

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
  double additive_draw_alpha;
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
  /* Rotated checkpoint: local = (pixel centre - anchor) * [x,y] + offset.
   * Exact filtered font: anchor is the unsnapped origin; diagonal x/y hold the
   * original positive scales, with the other components and offset zero. */
  double inverse_anchor[2],inverse_x[2],inverse_y[2],inverse_offset[2];
  /* Exact filtered fonts and physical-screen opaque-written coverage retain the
   * original positive draw alpha separately from the packed vertex RGB factor. */
  double filtered_draw_alpha;
  /* Modern filtered rotation optionally limits each row to the CPU quad span. Axis-aligned
   * box-filtered or float-band stretches carry [logical size, destination size, trim, visible size]
   * in these four components per axis, with span_active left clear. Modern nearest rotation
   * may instead carry signed fixed20 [base high16, base low16, step high16, step low16],
   * relative to the crop and the row's left edge, under GML_RENDER_SPAN_FIXED20_ROW. */
  double span_x[4],span_y[4];
  uint32_t span_active;
  uint32_t corner_colour[4];
  uint32_t shape;
  GmlRenderSamplingPolicy sampling;
  /* Automatic surface presentation's existing opacity certificate; never a pixel scan. */
  uint32_t source_opaque;
  /* Resolved immutable binary-source certificate: bits 0/1/2 prove that
   * visible red/green/blue samples are endpoints. Used only by whole-texel
   * numbered sprite parts; never a shader instruction or canonical state. */
  uint32_t source_endpoint_rgb_mask;
} GmlRenderQuad;

enum { GML_RENDER_SPAN_FIXED20_ROW=2u };

/* Shared admission for the bounded double-expression font rule, before target writes. */
int gml_render_filtered_font_quad_valid(const GmlRenderQuad *quad,
                                        uint32_t width,uint32_t height);
int gml_render_automatic_surface_quad_valid(const GmlRenderQuad *quad,
                                             uint32_t width,uint32_t height);

typedef struct GmlRenderCopyRegion {
  GmlRenderRect source,destination;
  uint32_t needs_source_snapshot;
} GmlRenderCopyRegion;

/* Fully resolved flat-colour coverage. The rectangle is a half-open pixel region in target
 * coordinates; it does not borrow a texture or mutable draw state after submission. */
typedef struct GmlRenderPrimitiveRect {
  GmlRenderRect region;
  uint32_t colour;
  /* Zero retains the flat rectangle rule. Procedural particle lines carry
   * resolved raster geometry, not a mutable particle-system reference. */
  uint32_t kind;
  double centre_x,centre_y,scale_x,scale_y,angle,alpha;
  uint32_t interpolate,alpha_blend;
  /* Rectangle colours stay in authored space after clipping. These resolved
   * submission fields are transient; canonical state stores the actual image. */
  GmlRenderRect authored_region;
  uint32_t corner_colour[4],rectangle_flags;
} GmlRenderPrimitiveRect;

enum { GML_RENDER_PRIMITIVE_FLAT=0, GML_RENDER_PRIMITIVE_PARTICLE_LINE=1,
       GML_RENDER_PRIMITIVE_CLASSIC_ALPHA_RECT=2,
       GML_RENDER_PRIMITIVE_CLASSIC_INVERSE_RECT=3,
       GML_RENDER_PRIMITIVE_STUDIO_ALPHA_RECT=4,
       GML_RENDER_PRIMITIVE_RECTANGLE=5 };

enum { GML_RENDER_RECTANGLE_OUTLINE=1u, GML_RENDER_RECTANGLE_CLASSIC=2u,
       GML_RENDER_RECTANGLE_TARGET_ALPHA=4u, GML_RENDER_RECTANGLE_GRADIENT=8u };

typedef enum GmlRenderLayerEffectKind {
  GML_RENDER_LAYER_EFFECT_TINT=1,
  GML_RENDER_LAYER_EFFECT_COLOURISE=2,
  GML_RENDER_LAYER_EFFECT_ZOOM_BLUR=3,
  GML_RENDER_LAYER_EFFECT_RGB_NOISE=4
} GmlRenderLayerEffectKind;

typedef struct GmlRenderLayerEffect {
  uint32_t kind;
  uint32_t colour;
  double intensity;
  double centre[2];
  double focus_radius;
  uint32_t linear;
  int32_t phase_seed;
} GmlRenderLayerEffect;

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
  GML_RENDER_SCENE_EFFECT=7,
  GML_RENDER_SCENE_FAMILY_COUNT=8
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
    GmlRenderLayerEffect effect;
    /* Lent through this submission. Shader output is non-reconstructible and must checkpoint. */
    const struct GmlPlanShader *shader;
  } payload;
} GmlRenderDrawOp;

int gml_render_particle_line_valid(const GmlRenderDrawOp *operation);
int gml_render_classic_alpha_rect_valid(const GmlRenderDrawOp *operation);
int gml_render_studio_alpha_rect_valid(const GmlRenderDrawOp *operation);
int gml_render_classic_inverse_rect_valid(const GmlRenderDrawOp *operation);
int gml_render_rectangle_valid(const GmlRenderDrawOp *operation);

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

/* Physical transfers, not logical image size. updated_region is in storage coordinates;
 * an empty region means a preserved checkpoint needs no publication. */
typedef struct GmlRenderImageReadbackResult {
  GmlRenderRect updated_region;
  uint64_t pixel_reads,pixel_bytes,mask_reads,mask_bytes;
} GmlRenderImageReadbackResult;

typedef struct GmlRenderImageReadback {
  GmlRenderImageRef image;
  uint32_t storage_width,storage_height;
  GmlRenderRect region;
  uint32_t pixel_format;
  uint32_t stride_bytes;
  size_t byte_capacity;
  void *bytes;
  /* Opt-in only for a retained complete-target checkpoint buffer. A nonzero baseline names
   * its last successfully published version; the caller preserves those bytes until return.
   * Zero requests a cold full read. No pointer-identity or arbitrary observer cache is allowed. */
  uint32_t checkpoint;
  uint64_t checkpoint_baseline_version;
  GmlRenderImageReadbackResult *result;
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
