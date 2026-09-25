<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 retrodiv <retrodiv@proton.me> -->

# Renderer backend correspondence

The neutral operation IDs and families live only in
`src/video/renderer/gml_render_operations.def`. Private CPU and GPU ledgers are
`cpu/gml_render_cpu_operations.def` and `gpu/gml_render_gpu_operations.def`.
`tests/architecture/check_render_operations.py` checks one classification per ID,
real definitions for supported/fallback anchors, naming of the eight required
OpenGL counterparts, and absence of fake definitions for planned entries.
`--require-delivered` rejects any planned entry; it is a final-delivery gate,
not the P1 CPU milestone. A planned entry cannot be selected. P1 has no new
scene-GPU dispatcher; the existing hybrid presentation/content-program path
remains separate and unchanged.

The later resident prototype supports clear, qualified whole-image quads, and nearest-filtered
interior atlas subrectangles with integer-sized axis-aligned destinations and optional X/Y mirrors.
The subrectangle variant is replayable from retained source pixels after context loss. Bilinear
subrectangles, scaling, rotation, general sprite/background/tile dispatch, and resident final
presentation are not yet qualified; the corresponding ordinary game calls still execute on CPU.

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
