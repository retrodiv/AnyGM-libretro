<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 retrodiv <retrodiv@proton.me> -->

# Renderer backend correspondence

The neutral operation IDs and families live only in
`src/video/renderer/gml_render_operations.def`. Private CPU and GPU ledgers are
`cpu/gml_render_cpu_operations.def` and `gpu/gml_render_gpu_operations.def`.
`tests/architecture/check_render_operations.py` checks one classification per ID,
real definitions for supported/fallback anchors, naming of the eight required
OpenGL counterparts, absence of fake definitions for planned entries, and the
live repair-level map's required fields, ledger status and source anchors.
`--require-delivered` rejects any planned entry; it is a final-delivery gate,
not the P1 CPU milestone. A planned entry cannot be selected. The historical
P1 CPU-only classification is retained below only as a dated architectural
snapshot; the live ledger and rule table here describe current support.

The later resident prototype supports clear, qualified whole-image quads, and nearest-filtered
interior atlas subrectangles with integer-sized axis-aligned destinations and optional X/Y mirrors.
A narrow ordinary sprite path accepts unit-scale, integer-aligned authored trim when its atlas crop
has only alpha 0 or 255; other texture-stage filters are irrelevant to this unprogrammed base-image
draw, and a binary-alpha crop uses endpoint source-over. Its CPU journal replay rejects any
intermediate-alpha source. These variants are replayable from retained source pixels after context
loss. A separate first-generation filtered path accepts qualified ordinary sprites with
partial alpha, tint, per-draw alpha, authored trim, partial source crops and either scale sign. It preserves snapped
destination coverage and fractional texture phase. For logical padding around a packed atlas
cell, taps crossing a padded edge use physical atlas coordinates on both axes; other taps clamp to
the selected source rectangle. The path requires physical margin pixels where needed, uses
source-over RGB with the application's source-alpha-squared coverage rule, and checkpoints the
device result rather than claiming exact CPU replay. Qualified cardinal and arbitrary rotations
use the same source-over checkpoint with the first-generation CPU's nearest-texel inverse map;
near-zero angles classified as axis draws, special sprite states and unqualified crop geometry
retain the ordered CPU fallback. A separate modern-layer filtered rotation branch uses the CPU's
four-tap inverse map, nonzero-alpha crop bounds and conditional per-row quadrilateral span. It
requires a proven opaque application target and uses the modern screen target's opaque-alpha
blend policy; transparent targets still cross the ordered CPU barrier. Both branches retain
the CPU checkpoint for lost-device reconstruction.
The public `draw_sprite_general` entry selects its source part independently of the authored
sprite origin. Uniform-corner rotation uses the existing rotated CPU blitter or the qualified
resident inverse-map quad. Distinct corner colours use a CPU two-triangle colour interpolation
path; an ordinary atlas selection, including one crossing logical trim, can instead submit
the same two transformed, vertex-coloured triangles on GPU. The authored rectangle defines
geometry and colour interpolation while the whole packed page defines filtered sampling.
The selected-source and colour variants preserve ordered CPU fallbacks for runtime sprites,
nondefault blend and other unqualified states. A first-generation cardinal source edge exactly on a pixel
centre also falls back: its incremental fixed-point CPU row walk can choose a different texel
from an independently sampled GPU fragment. These are bounded ordinary-scene qualifications,
not a claim that arbitrary surface, shader, GUI or skeletal draws are resident.
Ordinary magnified `draw_sprite_stretched` atlas draws in first-generation and modern-layer
policies have a separate bounded filtered path:
it uses the CPU leaf's floored destination origin and rounded extent, preserves the full logical
cell around authored trim, and returns transparent samples only on authored margin sides.
It requires a proven opaque application target, bilinear sampling, positive magnification on
both axes and physical atlas space for the full logical quad. Transparent targets, special
blend state, out-of-atlas logical cells, deep minification and the full-target coarse-mip effect
retain ordered CPU execution. This qualification does not cover nine-slice or surfaces.
Ordinary non-full-target minification and mixed-axis stretches use a separate bounded box-filter
quad: each destination pixel averages the same integer source box as the CPU, excluding authored
trim texels from both the sum and divisor. The device checkpoint preserves the exact result
without a context. The first-generation and modern-layer paths accept up to eight covered
source texels per axis (64 taps), a proven opaque destination and ordinary blend state, with
or without global interpolation. Deeper boxes and the full-target coarse-mip veil remain
ordered CPU fallbacks; the latter is a separate effect qualification, not an ordinary box sample.
The public `draw_sprite_pos` entry can keep an ordinary axis-aligned atlas rectangle resident
through the existing nearest or bilinear cell preparation. Its complete, cropped, trimmed,
binary-alpha and filtered/intermediate-alpha forms preserve the mapped corner geometry; the
filtered first-generation and modern-layer paths retain their distinct target-alpha and
fractional-edge rules, including the modern half-camera tie. A separate checkpointed quad
keeps convex non-axis atlas pages resident for ordinary source-over: affine parallelograms use
the CPU triangles' common inverse map, while genuinely warped forms interpolate local page
coordinates across the same two triangles. Nearest and bilinear sampling, clipped edges,
authored crops, tint-free draw alpha and intermediate source alpha retain their characterized
CPU pixels. Non-convex, self-intersecting or special-blend forms remain on the ordered CPU
path. Fractional non-affine coordinates, freely rotated nearest affine edges and nearest
warps without horizontal/vertical leading edges are conservatively excluded pending exact
pixel-centre tie rules; the corresponding filtered forms are qualified. Modern-layer
intermediate-alpha draws require a proven opaque target; first-generation source-over can
also preserve a translucent target. This does not make surface, shader, GUI or skeletal
positioned draws resident.
CPU text resolves a preceding resident sprite before writing glyph pixels; this ordering barrier
does not accelerate text. A similarly bounded first-generation nearest background path accepts
complete untrimmed
pages at unit positive scale with intermediate alpha. It checkpoints the device's source-over
result, treats only the low RGB 24 bits of a GML white tint as colour, and retains the existing
binary-alpha replay path unchanged. A resident-only background draw whose full untrimmed source
lies conservatively outside either target axis writes no pixels and does not force a CPU barrier;
this does not qualify visible mirrors or trimmed backgrounds. A terminal automatic
application-surface underlay may also use the exact current resident
image: the existing plan's integer nearest axes select the pixels, while the GL executor reads the
scene texture directly without a CPU upload. This requires an opaque, owned application surface,
no intervening GUI draw, and the guarded prototype setting. A failed or lost device reconstructs
the source from the resource journal before the existing CPU presentation writes it. Arbitrary
rotation and GUI draws over the underlay remain unqualified and use the CPU fallback. This narrow terminal path
does not establish a no-full-frame-transfer route for general gameplay.

The narrow ordinary-sprite route now copies consecutive compatible textured-quad
descriptors into one renderer-owned, bounded (2,048-operation) pending batch. It
keeps the immutable atlas source borrowed, projects each target version in draw
order, and submits the batch at a full queue, source/state change, CPU pixel
borrow, target/lifecycle boundary, or the end of the world pass. The existing
resource authority and CPU replay path still own partial device failure and
context-loss recovery. A successful full-target clear supersedes the older
replay journal only after the device accepts that clear; an unsuccessful clear
retains the older journal for fallback. These optimizations do not broaden the
set of qualified scene operations or change CPU raster bodies.

## Current operation support and repair-level P3/P4 rules

This is the live operation-ledger view, distinct from the P1 snapshot below.
`TEXTURED_QUAD` also carries the already-qualified two-triangle positioned and
corner-colour geometry; the separate `TEXTURED_TRIANGLES` opcode is not selected.
The terminal resident-source optimization belongs to the existing render plan,
not to the still-planned neutral `PRESENT_IMAGE` scene opcode.

| Neutral ID | Current GPU ledger | Current interpretation |
|---|---|---|
| `TEXTURED_QUAD` | `supported` | Only the bounded rule groups below; an unqualified public draw uses an ordered CPU barrier. |
| `TEXTURED_TRIANGLES` | `planned` | No separate selected executor; positioned geometry is a qualified textured quad. |
| `COLOURED_PRIMITIVE` | `planned` | P6 CPU bridge. |
| `CLEAR` | `supported` | Qualified resident target clear. |
| `COPY_IMAGE` | `planned` | P5 resource/copy path. |
| `COMPOSE_SURFACE` | `planned` | P5 surface composition. |
| `LAYER_EFFECT` | `planned` | P7 effect qualification. |
| `PRESENT_IMAGE` | `planned` | Separate plan resident-source optimization exists; no scene-op claim. |
| `CONTENT_SHADER_DRAW` | `planned` | P3 neutral-device checkpoint probe is selected only through its private batch; general resident public content-program dispatch remains P7. |
| `SOFTWARE3D_PASS` | `fallback` | Ordered CPU bridge, not accelerated 3D. |

The anchors below use `path::symbol` relative to `src/video/renderer/`, except
`tests/` paths, which are relative to the repository root. They name actual
functions or stable shader-branch labels in source. A shader label identifies
the fragment or vertex arithmetic, not an invented executor. Rules may share
one hot quad kernel. The comparison column names a core-owned discriminator
where one exists; the same stable rule ID identifies additional device/CPU
comparisons in the external test owner's record. `pending` means the named
variant still lacks complete comparison evidence, not that registry coverage
proves pixel equivalence. The shared column owns source, geometry, eligibility
and draw-state decisions; the CPU and GPU columns own their respective pixel
rounding/sampling. Special shaders, surfaces, GUI, software3d and unqualified
blend/coverage are ordered CPU fallbacks unless a row expressly qualifies them.

<!-- R03-RULES:START -->
| Rule ID | Public operation and grouped variants | Shared preparation | Actual CPU implementation | GPU preparation and executor | Shader rule where arithmetic lives | Current limit and fallback | Discriminating comparison |
|---|---|---|---|---|---|---|---|
| R03-P3-JOURNAL | Versioned resident resource submission, bounded replay and CPU borrow | `common/gml_render_resources.c::gml_render_resource_submit`; `common/gml_render_resources.c::gml_render_resource_acquire_cpu_read` | `cpu/gml_render_cpu_replay.c::gml_render_cpu_replay_one`; `common/gml_render_resources.c::replay_history` | `gpu/opengl/gml_gpu_opengl_batch.c::gml_gpu_opengl_scene_submit`; `gpu/opengl/gml_gpu_opengl_resources.c::gml_gpu_opengl_scene_readback` | No shader-specific rule; each retained draw uses its operation's sampling/blend rule. | Exact replay only for qualified clear/nearest integer quads; unsupported or over-budget work falls back or checkpoints transactionally. | `tests/unit/video/renderer/test_render_resources.c::main`; device comparison R03-P3-JOURNAL. |
| R03-P3-CLEAR | Owned application/surface clear with deferred fill and resident target | `gml_render.c::gml_render_flush_pending_fill`; `common/gml_render_surfaces.c::gml_render_application_resource_ensure` | `cpu/gml_render_cpu_replay.c::gml_render_cpu_replay_one`; `gml_render.c::gml_render_flush_pending_fill` | `gpu/opengl/gml_gpu_opengl_batch.c::gml_gpu_opengl_scene_submit`; `gpu/opengl/gml_render_gpu_opengl_primitives.c::gml_render_gpu_opengl_clear` | No shader invocation: qualified clear uses the device clear operation. | Full owned target and valid resource only; other clears use existing CPU fill. | `tests/unit/video/renderer/test_render_resources.c::main`; device comparison R03-P3-CLEAR. |
| R03-P3-EXACT-OVERLAP | Ordered destination-dependent eight-bit source-over for overlapping quads | `common/gml_render_resources.c::gml_render_resource_submit`; `cpu/gml_render_cpu_replay.c::gml_render_cpu_replay_eligible` | `cpu/gml_render_cpu_replay.c::gml_render_cpu_replay_one` | `gpu/opengl/gml_gpu_opengl_batch.c::draw_exact_quad`; `gpu/opengl/gml_gpu_opengl_batch.c::exact_repetitions` | `gpu/opengl/gml_gpu_opengl_shaders.h::R03_SHADER_EXACT_OVERLAP` | Bounded exact groups and target region snapshots; unqualified blend, alpha target or geometry retains CPU/barrier. | `tests/unit/video/renderer/test_render_resources.c::main`; device comparison R03-P3-EXACT-OVERLAP. |
| R03-P3-RESIDENT-COPY | Prototype surface source-to-target copy through a qualified textured quad | `common/gml_render_resources.c::gml_render_resource_source_view`; `common/gml_render_surfaces.c::gml_render_surface_scene_submit` | `cpu/gml_render_cpu_replay.c::gml_render_cpu_replay_one` | `gpu/opengl/gml_gpu_opengl_batch.c::gml_gpu_opengl_scene_submit`; `gpu/opengl/gml_render_gpu_opengl_blit.c::gml_render_gpu_opengl_draw_textured_quad` | `gpu/opengl/gml_gpu_opengl_shaders.h::R03_SHADER_DIRECT_TEXTURE` | Exact nearest integer subrectangle only; general `COPY_IMAGE` and surface composition remain P5. | `tests/unit/video/renderer/test_render_resources.c::main`; device comparison R03-P3-RESIDENT-COPY. |
| R03-P3-SHADER-CHECKPOINT | Prototype content-program draw with immediate canonical-pixel checkpoint | `common/gml_render_resources.c::gml_render_resource_submit`; `common/gml_render_resources.c::batch_valid` | `common/gml_render_resources.c::gml_render_resource_acquire_cpu_read` (checkpoint pixels; no CPU content-program reimplementation) | `gpu/opengl/gml_gpu_opengl_batch.c::gml_gpu_opengl_scene_submit`; `gpu/opengl/gml_gpu_opengl_programs.c::gml_gpu_opengl_scene_shader_draw` | No shader-specific rule: the content program owns arithmetic; readback seals the result. | Device and successful readback are mandatory; this bounded P3 handoff does not deliver general P7 shader/effect residency. | `tests/unit/video/renderer/test_render_resources.c::main`; device comparison R03-P3-SHADER-CHECKPOINT. |
| R03-P3-ATLAS-CACHE | Immutable atlas version upload/reuse, subrectangle sampling and retirement | `common/gml_render_resources.c::source_view_valid`; `common/gml_render_resources.c::upload_sources` | `cpu/gml_render_cpu_replay.c::gml_render_cpu_replay_one` | `gpu/opengl/gml_gpu_opengl_resources.c::gml_gpu_opengl_scene_upload`; `gpu/opengl/gml_gpu_opengl_resources.c::gml_gpu_opengl_scene_retire` | `gpu/opengl/gml_gpu_opengl_shaders.h::R03_SHADER_DIRECT_TEXTURE` | Reuse requires same immutable source/version/format; mutable surface identity and broader cache remain P5. | `tests/unit/video/renderer/test_render_resources.c::main`; device comparison R03-P3-ATLAS-CACHE. |
| R03-P3-CONTEXT-RESET | Device loss, CPU reconstruction, resource rebind and current-format state recovery | `common/gml_render_draw.c::gml_render_bind_device`; `common/gml_render_resources.c::gml_render_resource_context_lost` | `common/gml_render_resources.c::gml_render_resource_acquire_cpu_read`; `cpu/gml_render_cpu_replay.c::gml_render_cpu_replay_one` | `gpu/opengl/gml_gpu_opengl_resources.c::gml_gpu_opengl_scene_reset`; `gpu/opengl/gml_gpu_opengl_resources.c::gml_gpu_opengl_scene_upload` | No shader-specific rule: context loss invalidates device objects, not canonical pixel semantics. | Replayable records reconstruct the current format; invalid/uncertain resources reject rather than silently accepting stale device bytes. | `tests/unit/video/renderer/test_render_resources.c::main`; device comparison R03-P3-CONTEXT-RESET. |
| R03-P3-OPAQUE-ELISION | Full-target opaque GUI overwrite cancels hidden deferred application underlay | `gml_render.c::gml_render_prepare_opaque_rect`; `gml_render.c::gml_render_cancel_pending_underlay` | `cpu/gml_render_cpu_blit.c::gml_render_cpu_blit_sprite_stretched_resolved` | `gml_render.c::gml_render_cancel_pending_underlay` (no device operation survives) | No shader invocation: the old underlay is proven fully hidden. | Only fully opaque unmasked full-target cover; all other GUI draws retain the ordinary CPU composition path. | `tests/unit/video/renderer/test_renderer_effects.c::check_opaque_stretched_underlay`; core-only comparison R03-P3-OPAQUE-ELISION (the GPU operation is canceled). |
| R03-SPR-NEAREST | Ordinary sprite complete/partial atlas and nearest stretched alias, binary-alpha trim, integer scale and mirror | `common/gml_render_sprites.c::resident_integer_sprite`; `common/gml_render_sprites.c::gml_render_resident_atlas_crop`; `common/gml_render_sprites.c::gml_draw_sprite_stretched` | `cpu/gml_render_cpu_blit.c::gml_render_cpu_blit_sprite_ext_resolved`; `cpu/gml_render_cpu_blit.c::gml_render_cpu_blit_sprite_stretched_resolved` | `gpu/opengl/gml_render_gpu_opengl_blit.c::gml_render_gpu_opengl_draw_textured_quad`; `gpu/opengl/gml_gpu_opengl_batch.c::scene_filter` | `gpu/opengl/gml_gpu_opengl_shaders.h::R03_SHADER_DIRECT_TEXTURE` | Exact nearest/replay only with bounded binary-alpha atlas and ordinary state; filtered, runtime and special states go to the original CPU leaf. | `tests/unit/video/renderer/test_renderer_tiles.c::check_first_generation_integer_magnification_tie`; device comparison R03-SPR-NEAREST. |
| R03-SPR-FILTER-TRIM | Filtered fractional sprite with logical trim, partial crop, mirror, tint and draw/source alpha | `common/gml_render_sprites.c::resident_filtered_sprite`; `common/gml_render_sprites.c::resident_filtered_atlas_page` | `cpu/gml_render_cpu_blit.c::gml_render_cpu_blit_with_phase`; `cpu/gml_render_cpu_blit.c::gml_render_cpu_blit_sprite_ext_resolved` | `gpu/opengl/gml_render_gpu_opengl_blit.c::gml_render_gpu_opengl_draw_textured_quad`; `gpu/opengl/gml_gpu_opengl_batch.c::scene_filter` | `gpu/opengl/gml_gpu_opengl_shaders.h::R03_SHADER_FILTERED_MARGIN`; `gpu/opengl/gml_gpu_opengl_shaders.h::R03_SHADER_TINT_ALPHA` | Requires physical atlas margins, ordinary source-over and a device checkpoint; missing edges, unsupported policy/state or modern transparent target use CPU. | `tests/unit/video/renderer/test_renderer_tiles.c::check_first_generation_magnification_keeps_every_source_row`; device comparison R03-SPR-FILTER-TRIM. |
| R03-SPR-ROTATE-GENERAL | First-generation nearest and modern four-tap rotation, selected source and uniform corner tint | `common/gml_render_sprites.c::resident_filtered_sprite`; `common/gml_render_sprites.c::gml_draw_sprite_general` | `cpu/gml_render_cpu_blit.c::gml_render_cpu_blit_sprite_ext_resolved`; `cpu/gml_render_cpu_blit.c::gml_render_cpu_blit_sprite_general_rotated` | `gpu/opengl/gml_render_gpu_opengl_blit.c::gml_render_gpu_opengl_draw_textured_quad`; `gpu/opengl/gml_gpu_opengl_batch.c::scene_filter` | `gpu/opengl/gml_gpu_opengl_shaders.h::R03_SHADER_INVERSE_GEOMETRY`; `gpu/opengl/gml_gpu_opengl_shaders.h::R03_SHADER_ROW_SPAN`; `gpu/opengl/gml_gpu_opengl_shaders.h::R03_SHADER_FILTERED_MARGIN` | Near-zero-axis ties, unqualified source overlap and special states use CPU; modern four-tap requires an opaque target and checkpoints. | `tests/unit/video/renderer/test_renderer_tiles.c::check_positioned_sprite_geometry`; device comparison R03-SPR-ROTATE-GENERAL. |
| R03-SPR-CORNER-COLOUR | Public general sprite with four different corner colours and selected atlas source, including logical-trim overlap | `common/gml_render_sprites.c::gml_draw_sprite_general`; `common/gml_render_backgrounds.c::gml_render_tpag_part_view`; `common/gml_render_sprites.c::resident_filtered_atlas_page` | `cpu/gml_render_cpu_blit.c::gml_render_cpu_blit_sprite_general_coloured` | `gpu/opengl/gml_render_gpu_opengl_blit.c::gml_render_gpu_opengl_draw_textured_quad`; `gpu/opengl/gml_gpu_opengl_batch.c::scene_filter` | `gpu/opengl/gml_gpu_opengl_shaders.h::R03_SHADER_VERTEX_COLOUR`; `gpu/opengl/gml_gpu_opengl_shaders.h::R03_SHADER_GENERAL_VISIBLE_CENTRE`; `gpu/opengl/gml_gpu_opengl_shaders.h::R03_SHADER_FILTERED_MARGIN`; `gpu/opengl/gml_gpu_opengl_shaders.h::R03_SHADER_TINT_ALPHA` | Authored selection owns quad and vertex-colour interpolation; the whole packed page owns filtered taps and centres outside it are skipped. Ordinary source-over only; runtime sprites, missing physical atlas edge, unqualified policy and nondefault blend keep CPU triangles. | Core comparison pending; device comparison R03-SPR-CORNER-COLOUR. |
| R03-SPR-POSITIONED-AXIS | Public positioned sprite rectangular complete/cropped/filtered/trimmed source | `common/gml_render_sprites.c::gml_render_sprite_pos_geometry`; `common/gml_render_sprites.c::resident_positioned_axis_sprite` | `cpu/gml_render_cpu_blit.c::gml_render_cpu_blit_sprite_pos_resolved` | `gpu/opengl/gml_render_gpu_opengl_blit.c::gml_render_gpu_opengl_draw_textured_quad`; `gpu/opengl/gml_gpu_opengl_batch.c::scene_filter` | `gpu/opengl/gml_gpu_opengl_shaders.h::R03_SHADER_DIRECT_TEXTURE`; `gpu/opengl/gml_gpu_opengl_shaders.h::R03_SHADER_FILTERED_MARGIN` | First-generation/modern target-alpha and half-camera policies remain distinct; unqualified geometry or state falls back before the unchanged CPU triangles. | `tests/unit/video/renderer/test_renderer_tiles.c::check_positioned_sprite_geometry`; device comparison R03-SPR-POSITIONED-AXIS. |
| R03-SPR-POSITIONED-QUAD | Public positioned sprite cardinal, affine and bounded convex two-triangle warp | `common/gml_render_sprites.c::gml_render_sprite_pos_geometry`; `common/gml_render_sprites.c::resident_positioned_quad_sprite` | `cpu/gml_render_cpu_blit.c::gml_render_cpu_blit_sprite_pos_resolved` | `gpu/opengl/gml_render_gpu_opengl_blit.c::gml_render_gpu_opengl_draw_textured_quad`; `gpu/opengl/gml_gpu_opengl_batch.c::scene_filter` | `gpu/opengl/gml_gpu_opengl_shaders.h::R03_SHADER_INVERSE_GEOMETRY`; `gpu/opengl/gml_gpu_opengl_shaders.h::R03_SHADER_POSITIONED_NEAREST`; `gpu/opengl/gml_gpu_opengl_shaders.h::R03_SHADER_WARP_BILINEAR`; `gpu/opengl/gml_gpu_opengl_shaders.h::R03_SHADER_ROW_SPAN` | CPU triangle order and page-local UVs are preserved; nonconvex, fractional nonaffine and characterized nearest texel ties or special state use exact CPU fallback. | `tests/unit/video/renderer/test_renderer_tiles.c::check_positioned_sprite_geometry`; device comparison R03-SPR-POSITIONED-QUAD. |
| R03-SPR-STRETCH-MAG | Public stretched aliases, filtered positive magnification with logical trim | `common/gml_render_sprites.c::gml_draw_sprite_stretched`; `common/gml_render_sprites.c::resident_filtered_atlas_page` | `cpu/gml_render_cpu_blit.c::gml_render_cpu_blit_sprite_stretched_resolved` | `gpu/opengl/gml_render_gpu_opengl_blit.c::gml_render_gpu_opengl_draw_textured_quad`; `gpu/opengl/gml_gpu_opengl_batch.c::scene_filter` | `gpu/opengl/gml_gpu_opengl_shaders.h::R03_SHADER_FILTERED_MARGIN`; `gpu/opengl/gml_gpu_opengl_shaders.h::R03_SHADER_TINT_ALPHA` | Positive magnification, opaque application target and physical logical-cell space only; nine-slice, transparent target and special blend stay CPU. | `tests/unit/video/renderer/test_renderer_tiles.c::check_first_generation_magnification_keeps_every_source_row`; device comparison R03-SPR-STRETCH-MAG. |
| R03-SPR-STRETCH-BOX | Public stretched aliases, bounded minification/mixed-axis source boxes | `common/gml_render_sprites.c::gml_draw_sprite_stretched`; `common/gml_render_sprites.c::resident_filtered_atlas_page` | `cpu/gml_render_cpu_blit.c::gml_render_cpu_blit_sprite_stretched_resolved` | `gpu/opengl/gml_render_gpu_opengl_blit.c::gml_render_gpu_opengl_draw_textured_quad`; `gpu/opengl/gml_gpu_opengl_batch.c::scene_filter` | `gpu/opengl/gml_gpu_opengl_shaders.h::R03_SHADER_BOX_REDUCTION` | Up to eight covered source texels per axis on an opaque target; deeper boxes and full-target coarse-mip effect stay CPU. | `tests/unit/video/renderer/test_renderer_tiles.c::check_first_generation_filtered_minification`; device comparison R03-SPR-STRETCH-BOX. |
| R03-SPR-TILED-PHASE | Public tiled sprite complete nearest unit-scale cells, including clipped phase | `common/gml_render_sprites.c::gml_render_resident_tiled_sprite`; `common/gml_render_sprites.c::gml_render_sprite_tiled_ext_unmasked` | `cpu/gml_render_cpu_blit.c::gml_render_cpu_blit_with_phase` | `gpu/opengl/gml_render_gpu_opengl_blit.c::gml_render_gpu_opengl_draw_textured_quad`; `gpu/opengl/gml_gpu_opengl_batch.c::scene_filter` | `gpu/opengl/gml_gpu_opengl_shaders.h::R03_SHADER_DIRECT_TEXTURE` | At most 256 complete untrimmed cells, integer camera, nearest and default blend; filtered, fractional-scale, trimmed and denser repeats fall back. | Core comparison pending; device comparison R03-SPR-TILED-PHASE. |
| R03-SPR-LAYER-UNTILED | Untiled room/background-layer sprite, full untrimmed nearest atlas page and snapped camera-relative placement | `common/gml_render_sprites.c::gml_draw_layer_background_sprite`; `common/gml_render_sprites.c::gml_render_resident_atlas_page` | `cpu/gml_render_cpu_blit.c::gml_render_cpu_blit_with_phase` | `gpu/opengl/gml_render_gpu_opengl_blit.c::gml_render_gpu_opengl_draw_textured_quad`; `gpu/opengl/gml_gpu_opengl_batch.c::scene_filter` | `gpu/opengl/gml_gpu_opengl_shaders.h::R03_SHADER_DIRECT_TEXTURE` | Only untiled, complete untrimmed, nearest, positive-scale, white/full-alpha atlas sprite with ordinary state; bounded unit-scale repeated variants have their own R03-SPR-LAYER-REPEAT row, while runtime/filtered/other variants retain CPU fallback. | Core comparison pending; device comparison R03-SPR-LAYER-UNTILED. |
| R03-SPR-LAYER-REPEAT | Room/background-layer sprite repeated independently on horizontal, vertical or both axes with rounded camera phase | `common/gml_render_sprites.c::gml_draw_layer_background_sprite`; `common/gml_render_sprites.c::gml_render_resident_atlas_page` | `cpu/gml_render_cpu_blit.c::gml_render_cpu_blit_with_phase` | `gpu/opengl/gml_render_gpu_opengl_blit.c::gml_render_gpu_opengl_draw_textured_quad`; `gpu/opengl/gml_gpu_opengl_batch.c::scene_filter` | `gpu/opengl/gml_gpu_opengl_shaders.h::R03_SHADER_DIRECT_TEXTURE` | At most 256 complete untrimmed, unit-scale nearest cells with white/full-alpha and ordinary state; cropped phase is exact, while trim, filtering, nondefault blend, runtime pages and denser repetition preserve CPU fallback. | Core comparison pending; device comparison R03-SPR-LAYER-REPEAT. |
| R03-BG-DIRECT | Public direct background simple/ext/part and stretched alias, nearest and bounded filtered/fractional transforms with direct tint/alpha/trim | `common/gml_render_backgrounds.c::gml_draw_background`; `common/gml_render_backgrounds.c::gml_draw_background_ext`; `common/gml_render_sprites.c::gml_render_resident_filtered_atlas_page`; `common/gml_render_backgrounds.c::gml_draw_background_stretched`; `common/gml_render_backgrounds.c::gml_render_tpag_part_view`; `common/gml_render_backgrounds.c::gml_render_resident_filtered_atlas_part` | `cpu/gml_render_cpu_blit.c::gml_render_cpu_blit_background_phase`; `cpu/gml_render_cpu_blit.c::gml_render_cpu_blit_tpag_part_background` | `gpu/opengl/gml_render_gpu_opengl_blit.c::gml_render_gpu_opengl_draw_textured_quad`; `gpu/opengl/gml_gpu_opengl_batch.c::scene_filter` | `gpu/opengl/gml_gpu_opengl_shaders.h::R03_SHADER_DIRECT_TEXTURE`; `gpu/opengl/gml_gpu_opengl_shaders.h::R03_SHADER_FILTERED_MARGIN`; `gpu/opengl/gml_gpu_opengl_shaders.h::R03_SHADER_TINT_ALPHA` | First-generation filtered direct simple/ext and integral partial crops preserve qualified opaque-source tint, intermediate draw alpha, mirrors and physical margins; direct full-source logical X/Y/XY trim is admitted only with physical margins. Nondefault blend/missing physical edge, partial fractional/trimmed source and other special states retain exact CPU fallback. Rotation and broader combinations remain B1 work. | `tests/unit/video/renderer/test_renderer_tiles.c::check_filtered_fractional_tiled_background_reference`; device comparison R03-BG-DIRECT. |
| R03-BG-REPEAT | Tiled background, room background layer and edge-clipped repeated cells | `common/gml_render_backgrounds.c::gml_draw_background_tiled_ext`; `common/gml_render_backgrounds.c::gml_draw_room_backgrounds` | `cpu/gml_render_cpu_blit.c::gml_render_cpu_blit_background_phase` | `gpu/opengl/gml_render_gpu_opengl_blit.c::gml_render_gpu_opengl_draw_textured_quad`; `gpu/opengl/gml_gpu_opengl_batch.c::scene_filter` | `gpu/opengl/gml_gpu_opengl_shaders.h::R03_SHADER_DIRECT_TEXTURE`; `gpu/opengl/gml_gpu_opengl_shaders.h::R03_SHADER_FILTERED_MARGIN` | Integer nearest and selected filtered/fractional subsets only; rotated/repeated-cell and combined layer variants remain P4 fallback. | `tests/unit/video/renderer/test_renderer_tiles.c::check_trimmed_background_tiling_period`; device comparison R03-BG-REPEAT. |
| R03-TILE-CELL | Public background/tileset tile and room tile atlas subrect: nearest crop/mirror/partial alpha, plus filtered integral 2D part crops with fractional transform and bounded tint/alpha | `common/gml_render_tiles.c::gml_draw_tileset_tile`; `common/gml_render_tiles.c::gml_draw_tile`; `common/gml_render_tiles.c::gml_draw_room_tiles`; `common/gml_render_backgrounds.c::gml_draw_background_tile`; `common/gml_render_backgrounds.c::resident_integer_background_part`; `common/gml_render_backgrounds.c::gml_render_resident_filtered_atlas_part`; `common/gml_render_backgrounds.c::gml_render_tpag_part_view` | `cpu/gml_render_cpu_blit.c::gml_render_cpu_blit_tpag_part_tile`; `cpu/gml_render_cpu_blit.c::gml_render_cpu_blit_background_phase`; `cpu/gml_render_cpu_blit.c::gml_render_cpu_blit_rotated_background_tile` | `gpu/opengl/gml_render_gpu_opengl_blit.c::gml_render_gpu_opengl_draw_textured_quad`; `gpu/opengl/gml_gpu_opengl_batch.c::scene_filter` | `gpu/opengl/gml_gpu_opengl_shaders.h::R03_SHADER_DIRECT_TEXTURE`; `gpu/opengl/gml_gpu_opengl_shaders.h::R03_SHADER_FILTERED_MARGIN`; `gpu/opengl/gml_gpu_opengl_shaders.h::R03_SHADER_TINT_ALPHA` | Filtered part requires a complete untrimmed opaque atlas source, integral at-least-1×1 crop, physical margin, ordinary state and no rotation; trimmed, missing-margin, additive and rotated variants preserve the CPU path. Filtered public draw_tile with fractional world scale and room tile records with fractional camera are supported by the shared part preparation; other mixed edge/scale combinations and broader rotation remain P4 work. | `tests/unit/video/renderer/test_renderer_tiles.c::check_fractional_transform_equivalence`; device comparison R03-TILE-CELL. |
| R03-SCENE-ORDER | Versioned immutable atlas batches, accepted/no-op/CPU boundaries and checkpoint | `common/gml_render_surfaces.c::gml_render_application_scene_enqueue`; `common/gml_render_surfaces.c::gml_render_application_scene_flush` | `gml_render.c::gml_render_resident_application_cpu_barrier` | `gpu/opengl/gml_gpu_opengl_batch.c::gml_gpu_opengl_scene_submit` | No shader-specific rule; the queued operation retains its own sampling label. | Flush at source/state/target/batch or CPU borrow, preserve version order; dynamic surface/runtime-resource versioning is P5. | `tests/integration/test_gpu_scene_route.c::main`; device comparison R03-SCENE-ORDER. |
| R03-ALPHA-NOOP | Provably zero-alpha ordinary sprite and stretched draw before an active resident pass | `common/gml_render_sprites.c::gml_render_sprite_ext_unmasked`; `common/gml_render_sprites.c::gml_draw_sprite_stretched` | `cpu/gml_render_cpu_blit.c::gml_render_cpu_blit_sprite_ext_resolved`; `cpu/gml_render_cpu_blit.c::gml_render_cpu_blit_sprite_stretched_resolved` | `common/gml_render_sprites.c::gml_render_sprite_ext_unmasked` (returns before queue) | No shader invocation: zero contribution is proven before dispatch. | Only ordinary, nonshader/non-3D/non-GUI state; special-state or uncertain alpha preserves CPU barrier. | Core comparison pending; device comparison R03-ALPHA-NOOP. |
| R03-RESIDENT-PRESENT | Terminal opaque application image as plan underlay without CPU reupload | `common/gml_render_presentation.c::render_record_deferred_underlay`; `gml_render.c::gml_render_defer_resident_underlay` | `cpu/gml_render_cpu_presentation.c::gml_render_cpu_present_image`; `cpu/gml_render_cpu_plan.c::gml_render_cpu_plan_execute` | `gpu/opengl/gml_render_gpu_opengl_plan.c::gml_gpu_opengl_execute` | `gpu/opengl/gml_gpu_opengl_shaders.h::R03_SHADER_RESIDENT_PRESENT` | Opaque owned image, terminal nearest integer plan and no intervening GUI; other presentation remains hybrid/CPU. | `tests/integration/test_gpu_scene_route.c::main`; device comparison R03-RESIDENT-PRESENT. |
<!-- R03-RULES:END -->

The neutral scene-op ledger still lists a dedicated triangle opcode as planned;
its name must not be used to infer that P4's positioned quads are absent.
Conversely, resident texture sampling in this table does not imply P5 surfaces,
P6 text/primitives or P7 shaders/effects are delivered. A future phase updates
the affected row or adds a rule group when its actual backend branch and
comparison are available.

## Historical P1 CPU-milestone snapshot (not current support)

| Neutral ID | Shared rule owner | Current CPU execution anchor | OpenGL/OpenGL ES target at P1 | Barrier or fallback |
|---|---|---|---|---|
| `TEXTURED_QUAD` | Sprite, background, tile and text facades; frame/region and transform | `gml_render_cpu_blit_tpag_part` and connected runtime-image blitters | `gml_render_gpu_opengl_draw_textured_quad` — planned | Pixel reads require the latest image version before CPU sampling. |
| `TEXTURED_TRIANGLES` | Positioned-sprite and skeletal geometry | `gml_render_cpu_blit_sprite_pos_resolved` and skeletal raster closure | `gml_render_gpu_opengl_draw_textured_triangles` — planned | Preserve triangle/UV order, camera phase and source lifetime. |
| `COLOURED_PRIMITIVE` | `common/gml_render_primitives.c` shape/rectangle policy | `gml_render_cpu_rectangle` and connected primitive leaves | `gml_render_gpu_opengl_draw_primitive` — planned | CPU coverage remains the checkpoint for any unqualified shape. |
| `CLEAR` | Target/colour-write and deferred-fill rules | `gml_render_clear` | `gml_render_gpu_opengl_clear` — planned | Flush pending work at target and mask boundaries. |
| `COPY_IMAGE` | Surface IDs, versions, aliases and target stack | `gml_surface_copy` and copy-part closure | `gml_render_gpu_opengl_copy_image` — planned | Self/overlapping copies use a source snapshot. |
| `COMPOSE_SURFACE` | Shared surface request and screen-raster policy | `gml_render_cpu_draw_surface_stretched_impl` and part/rotated kernels | `gml_render_gpu_opengl_compose_surface` — planned | Resolve current source version, target alias and suspended fill. |
| `LAYER_EFFECT` | Layer capture and recognized-family policy | `gml_render_cpu_layer_filter_end` | `gml_render_gpu_opengl_apply_effect` — planned | Unsupported families fall back as a whole pass, preserving order. |
| `PRESENT_IMAGE` | Presentation region, axes and host eligibility | `gml_render_cpu_present_image` | `gml_render_gpu_opengl_present_image` — planned | Readback only when the frontend, state or CPU consumer requires pixels. |
| `CONTENT_SHADER_DRAW` | Shader recognition, uniforms, sampler and host-handoff policy | `gml_render_cpu_shader_pass_surface` — explicit host-program/CPU-composition fallback | Resident content-program pass — planned | No invented exact software interpreter; checkpoint the produced pixels and retain the established host handoff. |
| `SOFTWARE3D_PASS` | Existing software-3D projection, depth and draw-state policy | `gml_d3_draw_sprite_2d` and connected software-3D leaves | Explicit CPU fallback; no accelerated 3D promise | Flush GPU work, synchronize the target for CPU, then upload only changed content before later GPU work. |

The target executor naming pair for the first eight rows is
`gml_render_cpu_<suffix>` / `gml_render_gpu_opengl_<suffix>`, with suffixes
`draw_textured_quad`, `draw_textured_triangles`, `draw_primitive`, `clear`,
`copy_image`, `compose_surface`, `apply_effect`, and `present_image`.
The current CPU anchors above are the preserved implementation bodies; P1 does
not add forwarding aliases or change the call graph merely to give those bodies
new names. New executor entries introduced with the scene dispatcher use the
paired names and real work. The coarse render plan is a separate contract:
`gml_render_cpu_plan_execute` already executes it, while the existing OpenGL
plan execution is extracted under `gml_render_gpu_opengl_plan_execute` in P3.

Rule ownership is deliberate: a geometry or authored-source fix belongs in
`common/`, a sampling/coverage fix in `cpu/` and its qualified GPU counterpart,
and an OpenGL context/FBO/program fix only in `gpu/opengl/`. The one
`GmlRender` record, resource versions and canonical state remain shared; no
backend gets a second logical surface or sprite table. CPU reads require a
barrier from the newest GPU version. CPU writes commit a new version before a
later GPU read. Target changes, feedback/aliasing, shader host handoffs,
fallback, presentation, Reset and context loss are explicit synchronization
boundaries, not implicit per-frame full-frame transfers.

For diagnostics, a neutral report should identify the operation ID, selected
executor or fallback, reason, source/target version, and transfer count. For
example, a synthetic `TEXTURED_QUAD` after a pixel read would record the CPU
checkpoint and one resumed GPU upload; a `SOFTWARE3D_PASS` between two GPU
quads would record a full-pass CPU fallback with the required barriers. A
`planned` row is never reported as an executed GPU operation. Do not call an
existing hybrid plan pass proof that ordinary scene drawing is resident.
