/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Lifecycle and call-level contract for the hardware backend, driven by a graphics driver written
 * here rather than by a real one.
 *
 * The split is deliberate. What a fake driver can prove is exactly what a real one makes awkward:
 * that every entry point is resolved through the host callback and a missing one is named and
 * refused, that a context which disappeared is forgotten rather than deleted into its successor,
 * that every piece of shared state a pass depends on is set rather than inherited, and that the
 * rectangle handed to the driver is the flipped one. What it cannot prove is which pixel comes
 * out; restating the shader here would only compare a rule with itself. That question belongs to
 * a separate hardware-host comparison against the software executor. */
#include "anygm_test_runner.h"

#include "gml_gpu.h"
#include "gml_gpu_opengl_api.h"
#include "gml_gpu_opengl_internal.h"
#include "graphics_driver_fixture.h"

#include <stdint.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(condition,label) do{ \
  if(!(condition)){ \
    fprintf(stderr,"gpu failed: %s\n",label); \
    return 0; \
  } \
}while(0)

static GmlGpuContext fake_context(GmlGpuApi api){
  GmlGpuContext context;
  memset(&context,0,sizeof context);
  context.api=api;
  context.version_major=3;
  context.version_minor=api==GML_GPU_API_OPENGLES3?0u:3u;
  context.get_proc_address=anygm_test_graphics_proc;
  context.get_current_framebuffer=anygm_test_graphics_framebuffer;
  return context;
}

/* Packet bounds and origin handling only. Real driver/CPU pairs own pixels. */
static int scene_clamped_atlas_window_case(void){
  GmlRenderImageRef source={GML_RENDER_RESOURCE_ATLAS,1u,1u,1u};
  GmlRenderDrawOp op={0};GmlRenderDrawStateSnapshot state={0};
  op.opcode=GML_RENDER_OP_TEXTURED_QUAD;op.scene_family=GML_RENDER_SCENE_SPRITE;
  op.reconstruction_mode=GML_RENDER_DEVICE_CHECKPOINT;op.source_count=1u;op.sources=&source;
  op.target.image=(GmlRenderImageRef){GML_RENDER_RESOURCE_SCREEN,2u,1u,1u};
  op.target.width=op.target.height=8u;op.target.storage_version=1u;
  op.target.pixel_format=GML_RENDER_PIXEL_ARGB8888;op.target.region=(GmlRenderRect){0,0,8u,8u};
  GmlRenderQuad *q=&op.payload.quad;
  q->shape=GML_RENDER_QUAD_AXIS_ALIGNED;
  q->sampling.filter=GML_RENDER_FILTER_HARDWARE_BILINEAR;
  q->sampling.edge_rule=GML_RENDER_EDGE_CLAMP_SUBIMAGE;
  q->sampling.quantisation=GML_RENDER_QUANTISE_HARDWARE_SPRITE_FIRSTGEN;
  q->sampling.source_region=(GmlRenderRect){512,1024,64u,32u};
  q->filtered_draw_alpha=1.0;q->inverse_x[0]=q->inverse_y[1]=1.0;
  for(unsigned c=0;c<4u;c++){
    q->x[c]=(c==1u || c==2u)?8.0:0.0;q->y[c]=c>=2u?8.0:0.0;
    q->u[c]=(512.0+q->x[c])/2.0;q->v[c]=1024.0+q->y[c];q->corner_colour[c]=0xffffffffu;
  }
  state.write_mask=15u;state.alpha=255u;state.colour=0xffffffffu;
  float vertices[6u*GL_SCENE_VERTEX_FLOATS];
  REQUIRE(gml_gpu_opengl_clamped_atlas_valid(&op,2u,1u),"bounded virtual atlas window admitted");
  REQUIRE(gml_render_gpu_opengl_draw_textured_quad(&op,&state,2u,1u,0,vertices),
          "virtual atlas vertices retain authored source coordinates");
  REQUIRE(!gml_render_gpu_opengl_draw_textured_quad(&op,&state,2u,1u,1,vertices),
          "rendered-origin windows cannot inherit atlas clamping");
  op.scene_family=GML_RENDER_SCENE_BACKGROUND;
  q->sampling.quantisation=GML_RENDER_QUANTISE_HARDWARE_SPRITE_MODERN;
  REQUIRE(gml_gpu_opengl_clamped_atlas_valid(&op,2u,1u),"background physical sampler shares bounded policy");
  op.scene_family=GML_RENDER_SCENE_TEXT;
  REQUIRE(gml_gpu_opengl_clamped_atlas_valid(&op,2u,1u),"bounded filtered glyph shares physical clamping");
  GmlRenderDrawOp large_glyph=op;
  large_glyph.payload.quad.sampling.source_region.width=257u;
  REQUIRE(!gml_gpu_opengl_clamped_atlas_valid(&large_glyph,2u,1u),"oversized clamped glyph refused");
  large_glyph=op;large_glyph.payload.quad.sampling.source_region.height=257u;
  REQUIRE(!gml_gpu_opengl_clamped_atlas_valid(&large_glyph,2u,1u),"oversized clamped glyph row refused");
  op.scene_family=GML_RENDER_SCENE_BACKGROUND;
  for(unsigned bad=0;bad<18u;bad++){
    GmlRenderDrawOp changed=op;GmlRenderImageRef other=source;
    GmlRenderQuad *quad=&changed.payload.quad;changed.sources=&other;
    switch(bad){
      case 0:quad->sampling.source_region.x=-1;break;
      case 1:quad->sampling.source_region.y=-1;break;
      case 2:quad->sampling.source_region.width=0u;break;
      case 3:quad->sampling.source_region.height=0u;break;
      case 4:quad->sampling.source_region.width=4097u;break;
      case 5:quad->sampling.source_region.height=4097u;break;
      case 6:quad->sampling.source_region.x=65537;break;
      case 7:quad->sampling.source_region.y=65537;break;
      case 8:other.resource_class=GML_RENDER_RESOURCE_SURFACE;break;
      case 9:changed.scene_family=GML_RENDER_SCENE_TILE;break;
      case 10:quad->sampling.filter=GML_RENDER_FILTER_NEAREST;break;
      case 11:quad->sampling.quantisation=0u;break;
      case 12:quad->span_active=1u;break;
      case 13:quad->shape=GML_RENDER_QUAD_GENERAL;break;
      case 14:quad->sampling.edge_rule=GML_RENDER_EDGE_LOGICAL_TPAG;break;
      case 15:quad->sampling.logical_margin_mask=GML_RENDER_MARGIN_UNIT_TEXEL;break;
      case 16:changed.reconstruction_mode=GML_RENDER_REPLAY_EXACT;break;
      default:quad->sampling.source_region=(GmlRenderRect){0,0,2u,1u};break;
    }
    REQUIRE(!gml_gpu_opengl_clamped_atlas_valid(&changed,2u,1u),"unqualified virtual source policy refused");
  }
  REQUIRE(!gml_gpu_opengl_clamped_atlas_valid(&op,0u,1u) &&
          !gml_gpu_opengl_clamped_atlas_valid(&op,2u,0u) &&
          !gml_gpu_opengl_clamped_atlas_valid(&op,16385u,1u) &&
          !gml_gpu_opengl_clamped_atlas_valid(&op,2u,16385u),"physical storage bounds checked");
  q->sampling.source_region=(GmlRenderRect){65536,65536,4096u,4096u};
  REQUIRE(gml_gpu_opengl_clamped_atlas_valid(&op,1u,1u),"finite largest virtual window remains bounded");
  return 1;
}

/* A plan that magnifies a small opaque source into a larger target with black margins: the shape
 * the host-canvas pass produces. */
static int build_canvas_plan(GmlRenderPlan *plan,const uint32_t *pixels,
                             uint32_t source_width,uint32_t source_height,
                             uint32_t target_width,uint32_t target_height,
                             int32_t canvas_x,int32_t canvas_y,
                             uint32_t canvas_width,uint32_t canvas_height){
  GmlPlanImage image;
  GmlPlanRect whole={0,0,0,0};
  GmlPlanRect canvas;
  GmlPlanAxis axis_x,axis_y;
  uint32_t index;
  whole.width=target_width;
  whole.height=target_height;
  memset(&image,0,sizeof image);
  image.image_class=GML_PLAN_IMAGE_COMPLETED_FRAME;
  image.identity=1u;
  image.content_generation=1u;
  image.pixel_generation=1u;
  image.width=source_width;
  image.height=source_height;
  image.pitch_pixels=source_width;
  image.pixel_format=GML_PLAN_PIXEL_XRGB8888;
  image.opaque=1u;
  image.cpu_pixels=pixels;
  memset(&axis_x,0,sizeof axis_x);
  memset(&axis_y,0,sizeof axis_y);
  axis_x.rule=GML_PLAN_AXIS_ACCUMULATOR;
  axis_x.source_extent=source_width;
  axis_x.destination_extent=canvas_width;
  axis_y.rule=GML_PLAN_AXIS_ACCUMULATOR;
  axis_y.source_extent=source_height;
  axis_y.destination_extent=canvas_height;
  canvas.x=canvas_x;
  canvas.y=canvas_y;
  canvas.width=canvas_width;
  canvas.height=canvas_height;
  gml_render_plan_reset(plan,GML_PLAN_TARGET_HOST_FRAMEBUFFER,target_width,target_height);
  if(!gml_render_plan_add_clear(plan,whole,0u)) return 0;
  index=gml_render_plan_add_image(plan,&image);
  if(index==GML_PLAN_NO_IMAGE) return 0;
  return gml_render_plan_add_blit_nearest(plan,index,canvas,axis_x,axis_y,0u);
}

static int lifecycle_reset_case(void){
  GmlGpu *gpu;
  anygm_test_graphics_reset();
  gpu=gml_gpu_create();
  REQUIRE(gpu!=NULL,"the subsystem is created without a context");
  REQUIRE(!gml_gpu_context_active(gpu),"creation adopts no context");
  REQUIRE(gml_gpu_context_reset(gpu,&(GmlGpuContext){0})==0,"an empty context is refused");
  {
    GmlGpuContext context=fake_context(GML_GPU_API_OPENGL_CORE);
    REQUIRE(gml_gpu_context_reset(gpu,&context),"a desktop context is adopted");
    REQUIRE(gml_gpu_context_active(gpu),"the adopted context is active");
  }
  {
    /* A reset with no destroy before it: the previous context is already gone, so nothing may be
     * deleted into the new one. */
    GmlGpuContext context=fake_context(GML_GPU_API_OPENGLES3);
    int before=anygm_test_graphics_deletes();
    REQUIRE(gml_gpu_context_reset(gpu,&context),"an embedded context is adopted");
    REQUIRE(anygm_test_graphics_deletes()==before,"a reset without a destroy deletes nothing");
  }
  {
    GmlGpuCounters counters;
    gml_gpu_counters(gpu,&counters);
    REQUIRE(counters.context_resets==2,"both resets are counted");
  }
  gml_gpu_destroy(gpu,1);
  REQUIRE(anygm_test_graphics_deletes()>0,"a controlled destroy deletes while the context is current");
  return 1;
}

static uint32_t completed_context_is_current(void *userdata){
  return *(const uint32_t*)userdata;
}

static int integer_presentation_shift_case(void){
  for(unsigned api=GML_GPU_API_OPENGL_CORE;api<=GML_GPU_API_OPENGLES3;api++){
    uint32_t current=1u,pixels[8u*4u]={0};GmlGpuCounters before={0},after={0};
    GmlGpuContext context=fake_context(api);GmlRenderPlan plan={0};
    context.userdata=&current;context.is_current=completed_context_is_current;
    anygm_test_graphics_reset();GmlGpu *gpu=gml_gpu_create();
    REQUIRE(gpu && gml_gpu_context_reset(gpu,&context) &&
            gml_gpu_capture_completed_frame(gpu,8u,4u,11u),"shift adopts canonical storage");
    gml_gpu_counters(gpu,&before);
    REQUIRE(!gml_gpu_present_shifted_frame(gpu,8u,4u,12u,1,-1,0) &&
            !gml_gpu_present_shifted_frame(gpu,7u,4u,11u,1,-1,0) &&
            !gml_gpu_present_shifted_frame(gpu,8u,4u,11u,1,-1,1) &&
            !gml_gpu_present_shifted_frame(gpu,8u,4u,11u,1,-1,2) &&
            !gml_gpu_present_shifted_frame(gpu,0u,4u,11u,1,-1,0),
            "shift refuses wrong versions,dimensions and absent sources before drawing");
    gml_gpu_counters(gpu,&after);
    REQUIRE(after.draw_calls==before.draw_calls,"shift source refusal submits nothing");
    current=0u;anygm_test_graphics_forbid_calls();
    REQUIRE(!gml_gpu_present_shifted_frame(gpu,8u,4u,11u,1,-1,0) &&
            !anygm_test_graphics_calls_after_forget(),"revoked shift access calls no GL entry");
    anygm_test_graphics_allow_calls();current=1u;
    REQUIRE(gml_gpu_present_shifted_frame(gpu,8u,4u,11u,1,-1,0),
            "canonical shift resumes when the owning context returns");
    gml_gpu_counters(gpu,&after);
    REQUIRE(after.draw_calls==before.draw_calls+1u &&
            after.completed_frame_copies==before.completed_frame_copies &&
            after.completed_frame_reads==before.completed_frame_reads &&
            after.scene_readbacks==before.scene_readbacks &&
            after.full_uploads==before.full_uploads,"shift adds one draw and no pixel transport");
    REQUIRE(build_canvas_plan(&plan,pixels,8,4,8,4,0,0,8,4),"scratch host plan builds");
    plan.target=GML_PLAN_TARGET_DEVICE_PRESENTATION;
    REQUIRE(gml_gpu_execute_plan(gpu,&plan) &&
            gml_gpu_present_shifted_frame(gpu,8u,4u,12u,-1,1,1) &&
            !gml_gpu_present_shifted_frame(gpu,8u,4u,12u,-1,1,1),
            "device-only host scratch is consumed exactly once");
    REQUIRE(gml_gpu_execute_plan(gpu,&plan) && gml_gpu_context_reset(gpu,&context) &&
            !gml_gpu_present_shifted_frame(gpu,8u,4u,11u,1,-1,0) &&
            !gml_gpu_present_shifted_frame(gpu,8u,4u,12u,-1,1,1),
            "unexpected context reset invalidates both delivery sources");
    gml_gpu_context_lost(gpu);anygm_test_graphics_forbid_calls();
    REQUIRE(!gml_gpu_present_shifted_frame(gpu,8u,4u,11u,1,-1,0) &&
            !anygm_test_graphics_calls_after_forget(),"lost shift context is not accessed");
    anygm_test_graphics_allow_calls();gml_gpu_destroy(gpu,0);
  }
  return 1;
}

static int scene_preparation_case(void){
  for(unsigned api=GML_GPU_API_OPENGL_CORE;api<=GML_GPU_API_OPENGLES3;api++){
    GmlGpuContext context=fake_context(api);
    GmlGpuCounters counters={0};
    uint32_t current=1u;
    unsigned texture,compiles,queries;
    unsigned preparation_compiles=api==GML_GPU_API_OPENGLES3?6u:4u;
    context.userdata=&current;
    context.is_current=completed_context_is_current;
    anygm_test_graphics_reset();
    GmlGpu *gpu=gml_gpu_create();
    REQUIRE(gpu && !gml_gpu_prepare_scene(gpu,16u,12u),"no preparation before adoption");
    REQUIRE(gml_gpu_context_reset(gpu,&context),"preparation context adopted");
    compiles=anygm_test_graphics_compile_calls();
    REQUIRE(!gml_gpu_prepare_scene(gpu,0u,12u) &&
            !gml_gpu_prepare_scene(gpu,UINT32_MAX,12u),"invalid preparation extents refused");
    REQUIRE(anygm_test_graphics_compile_calls()==compiles,"invalid extents compile nothing");
    REQUIRE(gml_gpu_prepare_scene(gpu,16u,12u),"common scene resources prepared");
    REQUIRE(anygm_test_graphics_compile_calls()==compiles+preparation_compiles,
            "common scene, composition and the fixed font program compile once");
    texture=anygm_test_graphics_first_texture_with_format(GL_RGBA8);
    REQUIRE(texture && anygm_test_graphics_texture_width(texture)==16 &&
            anygm_test_graphics_texture_height(texture)==12 &&
            anygm_test_graphics_texture_uploads(texture)==1u &&
            anygm_test_graphics_texture_data_uploads(texture)==0u,
            "destination-copy capacity allocated without pixel upload");
    REQUIRE(gml_gpu_prepare_scene(gpu,16u,12u) && gml_gpu_prepare_scene(gpu,8u,6u),
            "repeated and smaller preparation reuse resources");
    REQUIRE(anygm_test_graphics_compile_calls()==compiles+preparation_compiles &&
            anygm_test_graphics_texture_uploads(texture)==1u,"no repeated compile or storage");
    REQUIRE(gml_gpu_prepare_scene(gpu,20u,6u) &&
            anygm_test_graphics_texture_width(texture)==20 &&
            anygm_test_graphics_texture_height(texture)==12 &&
            anygm_test_graphics_texture_uploads(texture)==2u,
            "growth retains the other axis capacity");
    REQUIRE(anygm_test_graphics_draw_calls()==1 && anygm_test_graphics_scene_draw_calls()==1 &&
            anygm_test_graphics_read_pixels()==0 && anygm_test_graphics_scene_copies()==1u &&
            anygm_test_graphics_framebuffer_queries()==1u &&
            anygm_test_graphics_last_draw_framebuffer()!=0u &&
            anygm_test_graphics_last_draw_framebuffer()!=7u &&
            anygm_test_graphics_finish_calls()==1u &&
            anygm_test_graphics_bound_framebuffer()==7u,
            "private terminal/composition draws never write or read the restored frontend target");
    gml_gpu_counters(gpu,&counters);
    REQUIRE(!counters.scene_batches_offered && !counters.scene_uploads &&
            !counters.scene_readbacks && !counters.completed_frame_copies,
            "resource preparation does not claim draw or transport credit");
    REQUIRE(counters.draw_calls==2u && counters.full_uploads==1u && counters.uploaded_bytes==8u,
            "private preparation reports both actual draws and reuses its tiny upload");
    current=0u;
    queries=anygm_test_graphics_framebuffer_queries();
    anygm_test_graphics_forbid_calls();
    REQUIRE(!gml_gpu_prepare_scene(gpu,24u,12u) &&
            anygm_test_graphics_calls_after_forget()==0 &&
            anygm_test_graphics_framebuffer_queries()==queries,
            "revoked access refuses preparation before any driver call");
    gml_gpu_context_lost(gpu);
    REQUIRE(!gml_gpu_prepare_scene(gpu,16u,12u) &&
            anygm_test_graphics_calls_after_forget()==0,"lost context cannot prepare");
    anygm_test_graphics_allow_calls();
    current=1u;
    int deletes=anygm_test_graphics_deletes();
    REQUIRE(gml_gpu_context_reset(gpu,&context),"replacement context adopted");
    REQUIRE(anygm_test_graphics_deletes()==deletes,"old namespace never deleted into replacement");
    REQUIRE(gml_gpu_prepare_scene(gpu,16u,12u),"replacement context rebuilds preparation");
    REQUIRE(anygm_test_graphics_draw_calls()==2 && anygm_test_graphics_scene_draw_calls()==2 &&
            anygm_test_graphics_bound_framebuffer()==7u,
            "context replacement repeats private preparation and restores the frontend target");
    gml_gpu_destroy(gpu,1);
  }
  return 1;
}

static int unbound_context_access_case(void){
  for(unsigned api=GML_GPU_API_OPENGL_CORE;api<=GML_GPU_API_OPENGLES3;api++){
    uint32_t current=1u,pixels[4]={0xFF102030u,0xFF405060u,0xFF708090u,0xFFA0B0C0u};
    uint32_t observed[4]={0},before[4]={0};
    GmlGpuContext context=fake_context(api);
    GmlRenderImageRef image={GML_RENDER_RESOURCE_ATLAS,8u,1u,1u};
    GmlRenderImageUpload upload={0};
    GmlRenderImageReadback readback={0};
    GmlRenderDrawOp clear={0};
    GmlRenderDrawBatch batch={0};
    GmlRenderDeviceServices services={0};
    GmlRenderDeviceCaps caps={0};
    GmlRenderExecutionResult result;
    GmlGpuCounters counters={0};
    GmlRenderPlan plan;
    int has_rgb=-1;
    unsigned framebuffer_queries;
    context.userdata=&current;
    context.is_current=completed_context_is_current;
    anygm_test_graphics_reset();
    GmlGpu *gpu=gml_gpu_create();
    REQUIRE(gpu && gml_gpu_context_reset(gpu,&context),"access context adopted while current");
    gml_gpu_device_services(gpu,&services);
    REQUIRE(services.observe_on_demand && services.observe_on_demand(services.context),
            "positive owning-context proof enables resident observation without replay");
    upload.image=image;
    upload.storage_width=upload.storage_height=2u;
    upload.region=(GmlRenderRect){0,0,2u,2u};
    upload.pixel_format=GML_RENDER_PIXEL_ARGB8888;
    upload.stride_bytes=8u;upload.byte_capacity=sizeof pixels;upload.bytes=pixels;
    REQUIRE(services.upload(services.context,&upload),"current access upload succeeds");
    clear.opcode=GML_RENDER_OP_CLEAR;
    clear.target.image=(GmlRenderImageRef){GML_RENDER_RESOURCE_SURFACE,9u,1u,1u};
    clear.target.storage_version=1u;
    clear.target.width=clear.target.height=2u;
    clear.target.pixel_format=GML_RENDER_PIXEL_ARGB8888;
    clear.target.region=(GmlRenderRect){0,0,2u,2u};
    batch.operations=&clear;batch.operation_count=1u;
    result=services.submit(services.context,&batch);
    REQUIRE(result.status==GML_RENDER_EXEC_ACCEPTED && result.accepted_prefix==1u,
            "current access submits a real target clear");
    readback.image=clear.target.image;readback.region=upload.region;
    readback.storage_width=readback.storage_height=2u;
    readback.pixel_format=upload.pixel_format;
    readback.stride_bytes=8u;readback.byte_capacity=sizeof observed;readback.bytes=observed;
    REQUIRE(services.readback(services.context,&readback),"current access reads its device target");
    memcpy(before,observed,sizeof before);
    REQUIRE(build_canvas_plan(&plan,pixels,2u,2u,8u,8u,2,2,4u,4u),
            "an otherwise eligible terminal plan is built");
    current=0u;
    anygm_test_graphics_forbid_calls();
    framebuffer_queries=anygm_test_graphics_framebuffer_queries();
    REQUIRE(gml_gpu_context_active(gpu) && gml_gpu_context_has_access_proof(gpu) &&
            !gml_gpu_context_is_current(gpu) &&
            !services.observe_on_demand(services.context) &&
            services.capabilities(services.context,&caps),
            "temporary unbinding keeps adopted metadata without driver calls");
    REQUIRE(!services.upload(services.context,&upload),"unbound upload is refused");
    REQUIRE(!services.readback(services.context,&readback) &&
            !memcmp(observed,before,sizeof observed),"unbound read publishes no caller bytes");
    clear.target.image.content_version++;
    result=services.submit(services.context,&batch);
    REQUIRE(result.status==GML_RENDER_EXEC_UNSUPPORTED && !result.accepted_prefix &&
            !result.uncertain_target_count && !result.all_destinations_uncertain,
            "unbound scene refuses before modifying any destination");
    REQUIRE(!services.flush(services.context) && !services.wait(services.context) &&
            !services.has_rgb(services.context,image,&has_rgb) && has_rgb==-1,
            "unbound flush, wait and synchronous query are refused");
    services.retire(services.context,
                    (GmlRenderImageRef){GML_RENDER_RESOURCE_ATLAS,99u,1u,1u});
    REQUIRE(!gml_gpu_execute_plan(gpu,&plan) &&
            plan.fallback_reason==GML_PLAN_FALLBACK_CONTEXT_UNAVAILABLE &&
            !anygm_test_graphics_calls_after_forget() &&
            framebuffer_queries==anygm_test_graphics_framebuffer_queries(),
            "unbound execution makes neither GL calls nor framebuffer queries");
    gml_gpu_counters(gpu,&counters);
    REQUIRE(counters.fallbacks[GML_PLAN_FALLBACK_CONTEXT_UNAVAILABLE]==1u &&
            counters.passes_replayed==1u && counters.scene_readbacks==1u,
            "unavailable access is counted without inventing completed work");
    current=1u;
    anygm_test_graphics_allow_calls();
    REQUIRE(services.upload(services.context,&upload) &&
            services.readback(services.context,&readback),
            "rebound access reuses the existing image version without a context reset");
    result=services.submit(services.context,&batch);
    REQUIRE(result.status==GML_RENDER_EXEC_ACCEPTED && result.accepted_prefix==1u &&
            gml_gpu_execute_plan(gpu,&plan),"rebound scene and terminal plan can retry");
    current=0u;
    anygm_test_graphics_forbid_calls();
    gml_gpu_destroy(gpu,1);
    REQUIRE(!anygm_test_graphics_calls_after_forget(),"unbound destruction revokes its caller flag");
    gpu=gml_gpu_create();
    REQUIRE(gpu && !gml_gpu_context_reset(gpu,&context) && !gml_gpu_context_active(gpu) &&
            !anygm_test_graphics_calls_after_forget(),
            "initial adoption cannot resolve or execute an explicitly unbound context");
    current=1u;
    anygm_test_graphics_allow_calls();
    REQUIRE(gml_gpu_context_reset(gpu,&context),"initial adoption can retry after binding");
    gml_gpu_destroy(gpu,1);
  }
  return 1;
}

static int completed_frame_snapshot_case(void){
  GmlGpu *gpu;
  GmlGpuContext context=fake_context(GML_GPU_API_OPENGLES3);
  GmlGpuCounters counters={0};
  GmlRenderPlan plan;
  uint32_t current=1u,pixels[40],before[40],source[16]={0},other[32]={0};
  unsigned owned_framebuffer,copy_texture;
  context.userdata=&current;
  context.is_current=completed_context_is_current;
  anygm_test_graphics_reset();
  gpu=gml_gpu_create();
  REQUIRE(gpu && gml_gpu_context_reset(gpu,&context),"completed context adopted");
  REQUIRE(gml_gpu_capture_completed_frame(gpu,8u,4u,11u) &&
          !anygm_test_graphics_read_pixels() &&
          anygm_test_graphics_scene_copies()==1u &&
          anygm_test_graphics_last_copy_framebuffer()==7u,
          "completed capture copies the borrowed target without a CPU read");
  copy_texture=anygm_test_graphics_attached_texture();
  REQUIRE(copy_texture && anygm_test_graphics_texture_width(copy_texture)==8 &&
          anygm_test_graphics_texture_height(copy_texture)==4,
          "completed capture has separately owned storage");
  for(unsigned index=0;index<40u;index++) pixels[index]=UINT32_C(0xDEADBEEF);
  memcpy(before,pixels,sizeof before);
  anygm_test_graphics_readback_alpha(0x41u);
  anygm_test_graphics_fail_readback_at(1u);
  REQUIRE(!gml_gpu_read_completed_frame(gpu,pixels,10u,8u,4u,11u) &&
          !memcmp(before,pixels,sizeof pixels),
          "failed completed read does not publish partial pixels");
  owned_framebuffer=anygm_test_graphics_last_read_framebuffer();
  REQUIRE(owned_framebuffer && owned_framebuffer!=7u,
          "completed observation reads its own target, not the frontend target");
  anygm_test_graphics_fail_readback_at(0u);
  REQUIRE(gml_gpu_read_completed_frame(gpu,pixels,10u,8u,4u,11u),
          "a failed completed read can retry the same generation");
  for(unsigned row=0;row<4u;row++){
    for(unsigned column=0;column<8u;column++)
      REQUIRE(pixels[row*10u+column]==(UINT32_C(0x41000033)|
                ((3u-row)<<16)|(column<<8)),
              "completed read preserves RGBA and flips rows exactly once");
    REQUIRE(pixels[row*10u+8u]==UINT32_C(0xDEADBEEF) &&
            pixels[row*10u+9u]==UINT32_C(0xDEADBEEF),"completed read respects caller pitch");
  }
  REQUIRE(build_canvas_plan(&plan,source,4u,4u,8u,4u,0,0,8u,4u),
          "separate ordinary readback plan builds");
  plan.target=GML_PLAN_TARGET_READBACK;
  plan.readback_pixels=other;
  plan.readback_pitch_pixels=8u;
  REQUIRE(gml_gpu_execute_plan(gpu,&plan) &&
          anygm_test_graphics_attached_texture()!=copy_texture &&
          gml_gpu_read_completed_frame(gpu,pixels,10u,8u,4u,11u) &&
          anygm_test_graphics_last_read_framebuffer()==owned_framebuffer,
          "ordinary shader readback cannot replace the completed-frame target");
  gml_gpu_counters(gpu,&counters);
  REQUIRE(counters.completed_frame_copies==1u && counters.completed_frame_copy_bytes==128u &&
          counters.completed_frame_reads==3u && counters.completed_frame_read_bytes==384u,
          "completed copy and attempted read traffic have separate counters");
  anygm_test_graphics_forbid_calls();
  REQUIRE(!gml_gpu_read_completed_frame(gpu,pixels,10u,8u,4u,12u) &&
          !gml_gpu_read_completed_frame(gpu,pixels,7u,8u,4u,11u) &&
          !gml_gpu_read_completed_frame(gpu,pixels,10u,7u,4u,11u) &&
          !anygm_test_graphics_calls_after_forget(),
          "stale generation, extent and pitch reject before a graphics call");
  current=0u;
  REQUIRE(!gml_gpu_capture_completed_frame(gpu,8u,4u,12u) &&
          !gml_gpu_read_completed_frame(gpu,pixels,10u,8u,4u,11u) &&
          !anygm_test_graphics_calls_after_forget(),
          "an unbound context cannot capture or read completed pixels");
  current=1u;
  gml_gpu_context_lost(gpu);
  REQUIRE(!gml_gpu_capture_completed_frame(gpu,8u,4u,12u) &&
          !gml_gpu_read_completed_frame(gpu,pixels,10u,8u,4u,11u) &&
          !anygm_test_graphics_calls_after_forget(),
          "a lost completed-frame context is never called");
  anygm_test_graphics_allow_calls();
  REQUIRE(gml_gpu_context_reset(gpu,&context),"completed context is recreated");
  anygm_test_graphics_forbid_calls();
  REQUIRE(!gml_gpu_read_completed_frame(gpu,pixels,10u,8u,4u,11u) &&
          !anygm_test_graphics_calls_after_forget(),
          "a replacement context cannot reuse a stale completed snapshot");
  anygm_test_graphics_allow_calls();
  anygm_test_graphics_fail_copy_at(anygm_test_graphics_scene_copies()+1u);
  REQUIRE(!gml_gpu_capture_completed_frame(gpu,5u,3u,12u),
          "a failed GPU copy is not published as a completed frame");
  anygm_test_graphics_fail_copy_at(0u);
  REQUIRE(gml_gpu_capture_completed_frame(gpu,5u,3u,12u) &&
          gml_gpu_read_completed_frame(gpu,pixels,10u,5u,3u,12u),
          "completed capture retries allocation after a failed resize");
  gml_gpu_destroy(gpu,1);
  return 1;
}

static int lifecycle_loss_case(void){
  GmlGpu *gpu;
  GmlGpuContext context;
  anygm_test_graphics_reset();
  gpu=gml_gpu_create();
  context=fake_context(GML_GPU_API_OPENGL_CORE);
  REQUIRE(gml_gpu_context_reset(gpu,&context),"context adopted");
  gml_gpu_context_lost(gpu);
  REQUIRE(!gml_gpu_context_active(gpu),"a lost context is no longer active");
  REQUIRE(anygm_test_graphics_deletes()==0,"a lost context is forgotten, not deleted");
  /* Anything reaching the driver from here would be a call into a context that is gone. */
  anygm_test_graphics_forbid_calls();
  gml_gpu_destroy(gpu,1);
  REQUIRE(anygm_test_graphics_calls_after_forget()==0,"destroying after a loss issues no graphics call");
  return 1;
}

static int lifecycle_destroy_without_context_case(void){
  GmlGpu *gpu;
  GmlGpuContext context;
  anygm_test_graphics_reset();
  gpu=gml_gpu_create();
  context=fake_context(GML_GPU_API_OPENGL_CORE);
  REQUIRE(gml_gpu_context_reset(gpu,&context),"context adopted");
  anygm_test_graphics_forbid_calls();
  gml_gpu_destroy(gpu,0);
  REQUIRE(anygm_test_graphics_calls_after_forget()==0,"destroying without a current context issues no call");
  return 1;
}

static int missing_entry_point_case(void){
  GmlGpu *gpu;
  GmlGpuContext context;
  anygm_test_graphics_reset();
  anygm_test_graphics_withhold("glTexSubImage2D");
  gpu=gml_gpu_create();
  context=fake_context(GML_GPU_API_OPENGL_CORE);
  REQUIRE(!gml_gpu_context_reset(gpu,&context),"a missing entry point rejects the context");
  REQUIRE(!gml_gpu_context_active(gpu),"a rejected context is not active");
  {
    const char *error=gml_gpu_last_error(gpu);
    REQUIRE(error!=NULL,"a rejected context leaves a reason");
    REQUIRE(strstr(error,"glTexSubImage2D")!=NULL,"the reason names the exact missing entry point");
  }
  gml_gpu_destroy(gpu,1);
  return 1;
}

static int program_failure_case(void){
  GmlGpu *gpu;
  GmlGpuContext context;
  anygm_test_graphics_reset();
  anygm_test_graphics_fail_link(1);
  gpu=gml_gpu_create();
  context=fake_context(GML_GPU_API_OPENGLES3);
  REQUIRE(!gml_gpu_context_reset(gpu,&context),"a program that will not link rejects the context");
  {
    const char *error=gml_gpu_last_error(gpu);
    REQUIRE(error && strstr(error,"link")!=NULL,"the reason names the failing stage");
  }
  {
    GmlGpuCounters counters;
    gml_gpu_counters(gpu,&counters);
    REQUIRE(counters.program_failures==1,"the failure is counted once");
  }
  gml_gpu_destroy(gpu,1);
  return 1;
}

static int scene_staged_upload_case(void){
  uint8_t pixels[128u*129u*4u],expected[128u*128u*4u];
  const char *missing[]={NULL,"glMapBufferRange","glUnmapBuffer"};
  for(unsigned api=GML_GPU_API_OPENGL_CORE;api<=GML_GPU_API_OPENGLES3;api++)
  for(unsigned omitted=0;omitted<3u;omitted++)
  for(unsigned format=GML_RENDER_PIXEL_XRGB8888;format<=GML_RENDER_PIXEL_RGBA8888;format++)
  for(unsigned rendered=0;rendered<2u;rendered++){
    GmlGpuContext context=fake_context(api);
    GmlRenderDeviceServices services;
    GmlRenderImageUpload upload={0};
    anygm_test_graphics_reset();
    anygm_test_graphics_withhold(missing[omitted]);
    GmlGpu *gpu=gml_gpu_create();
    if(omitted){
      REQUIRE(gpu && !gml_gpu_context_reset(gpu,&context) &&
              strstr(gml_gpu_last_error(gpu),missing[omitted]),
              "missing established mapping entry point still rejects context adoption");
      gml_gpu_destroy(gpu,0);
      continue;
    }
    REQUIRE(gpu && gml_gpu_context_reset(gpu,&context),"staging context adopted");
    gml_gpu_device_services(gpu,&services);
    upload.image=(GmlRenderImageRef){rendered?GML_RENDER_RESOURCE_SURFACE:
      GML_RENDER_RESOURCE_ATLAS,3u,1u,1u};
    upload.storage_width=upload.storage_height=128u;
    upload.region=(GmlRenderRect){0,0,128u,128u};upload.pixel_format=format;
    upload.stride_bytes=129u*4u;upload.byte_capacity=sizeof pixels;upload.bytes=pixels;
    memset(pixels,0xAC,sizeof pixels);
    for(unsigned row=0;row<128u;row++) for(unsigned column=0;column<128u;column++){
      uint8_t r=(uint8_t)(row*7u+column),g=(uint8_t)(row+column*11u);
      uint8_t b=(uint8_t)(row^column),a=(uint8_t)(row*13u+column*17u);
      uint32_t word=((uint32_t)a<<24)|((uint32_t)r<<16)|((uint32_t)g<<8)|b;
      uint8_t rgba[4]={r,g,b,a};
      memcpy(pixels+(size_t)row*upload.stride_bytes+column*4u,
             format==GML_RENDER_PIXEL_RGBA8888?(const void*)rgba:(const void*)&word,4u);
      size_t index=((size_t)(rendered?127u-row:row)*128u+column)*4u;
      memcpy(expected+index,rgba,4u);
      if(format==GML_RENDER_PIXEL_XRGB8888) expected[index+3u]=255u;
    }
    REQUIRE(services.upload(services.context,&upload),"strided converted upload succeeds");
    unsigned texture=anygm_test_graphics_first_texture_with_format(GL_RGBA8);
    size_t count=0;const unsigned char *bytes=anygm_test_graphics_texture_bytes(texture,&count);
    REQUIRE(bytes && count==sizeof expected && !memcmp(bytes,expected,count),
            "all RGBA bytes, padding exclusion and target row order are exact");
    AnygmTestGraphicsUnpack unpack=anygm_test_graphics_unpack();
    REQUIRE(!unpack.errors && !unpack.bound &&
            unpack.allocations==(omitted?0u:1u) && unpack.maps==(omitted?0u:1u) &&
            unpack.unmaps==(omitted?0u:1u) && unpack.uploads==(omitted?0u:1u),
            "staging is balanced and leaves no unpack binding");
    if(!rendered){
      REQUIRE(services.upload(services.context,&upload),"same immutable version remains resident");
      REQUIRE(anygm_test_graphics_texture_data_uploads(texture)==1u &&
              anygm_test_graphics_unpack().allocations==unpack.allocations,
              "a cached page neither stages nor resubmits its bytes");
    }
    upload.image.content_version++;
    REQUIRE(services.upload(services.context,&upload),"changed version uploads again");
    REQUIRE(anygm_test_graphics_texture_data_uploads(texture)==2u &&
            (omitted || (anygm_test_graphics_unpack().buffer==unpack.buffer &&
                         anygm_test_graphics_unpack().allocations==2u)),
            "the staging name is reused with a freshly orphaned store");
    if(!omitted) for(unsigned phase=1u;phase<=3u;phase++){
      GmlGpuCounters before={0},after={0};
      upload.image.content_version++;
      gml_gpu_counters(gpu,&before);
      anygm_test_graphics_fail_unpack(phase);
      REQUIRE(!services.upload(services.context,&upload),"failed staging is refused");
      gml_gpu_counters(gpu,&after);
      REQUIRE(after.scene_uploads==before.scene_uploads &&
              !anygm_test_graphics_unpack().bound,
              "failed staging publishes no uploaded version or unpack binding");
      anygm_test_graphics_fail_unpack(0u);
      REQUIRE(services.upload(services.context,&upload),"same version retries after failure");
    }
    unsigned deletes=(unsigned)anygm_test_graphics_deletes();
    REQUIRE(gml_gpu_context_reset(gpu,&context),"replacement namespace adopted");
    REQUIRE((unsigned)anygm_test_graphics_deletes()==deletes,
            "lost staging storage is forgotten without deleting into its successor");
    gml_gpu_device_services(gpu,&services);
    REQUIRE(services.upload(services.context,&upload),"replacement context reuploads immutable bytes");
    REQUIRE(!anygm_test_graphics_unpack().errors,"all staging stores preceded their maps");
    gml_gpu_destroy(gpu,1);
    REQUIRE(anygm_test_graphics_unpack().deletes==(omitted?0u:1u),
            "only the current namespace's staging buffer is destroyed");
  }
  for(unsigned large=0;large<2u;large++){
    uint32_t width=large?2048u:64u,height=large?2049u:64u;
    size_t count=(size_t)width*height*4u;
    uint8_t *bytes=(uint8_t*)malloc(count);
    GmlGpuContext context=fake_context(GML_GPU_API_OPENGLES3);
    GmlRenderDeviceServices service;
    GmlRenderImageUpload upload={.image={GML_RENDER_RESOURCE_ATLAS,1u,1u,1u},
      .storage_width=width,.storage_height=height,.region={0,0,width,height},
      .pixel_format=GML_RENDER_PIXEL_RGBA8888,.stride_bytes=(size_t)width*4u,
      .byte_capacity=count,.bytes=bytes};
    REQUIRE(bytes!=NULL,"direct-path boundary pixels allocated");
    memset(bytes,0x73,count);
    anygm_test_graphics_reset();
    GmlGpu *gpu=gml_gpu_create();
    REQUIRE(gpu && gml_gpu_context_reset(gpu,&context),"direct boundary context adopted");
    gml_gpu_device_services(gpu,&service);
    REQUIRE(service.upload(service.context,&upload) &&
            !anygm_test_graphics_unpack().allocations &&
            !anygm_test_graphics_unpack().bound,
            "tiny and over-budget transfers retain direct uploads without staging allocation");
    gml_gpu_destroy(gpu,1);
    free(bytes);
  }
  return 1;
}

static int scene_service_contract_case(void){
  static const GmlRenderOperationCode unselected[]={
    GML_RENDER_OP_TEXTURED_TRIANGLES,
    GML_RENDER_OP_COMPOSE_SURFACE,GML_RENDER_OP_PRESENT_IMAGE
  };
  const uint64_t supported_operations=(UINT64_C(1)<<GML_RENDER_OP_CLEAR)|
    (UINT64_C(1)<<GML_RENDER_OP_COPY_IMAGE)|
    (UINT64_C(1)<<GML_RENDER_OP_TEXTURED_QUAD)|
    (UINT64_C(1)<<GML_RENDER_OP_COLOURED_PRIMITIVE)|
    (UINT64_C(1)<<GML_RENDER_OP_LAYER_EFFECT)|
    (UINT64_C(1)<<GML_RENDER_OP_CONTENT_SHADER_DRAW)|
    (UINT64_C(1)<<GML_RENDER_OP_RECOVER_EMPTY_SCREEN);
  const uint32_t supported_families=GML_RENDER_FAMILY_TEXTURE|
    GML_RENDER_FAMILY_PRIMITIVE|GML_RENDER_FAMILY_EFFECT|GML_RENDER_FAMILY_SHADER|
    GML_RENDER_FAMILY_SURFACE;
  GmlGpu *gpu;
  GmlGpuContext context;
  GmlRenderDeviceServices services;
  GmlRenderDeviceCaps caps;
  GmlRenderDrawOp op;
  GmlRenderDrawBatch batch;
  GmlRenderExecutionResult result;
  int draws;
  anygm_test_graphics_reset();
  gpu=gml_gpu_create();
  REQUIRE(gpu!=NULL,"device metadata allocated");
  memset(&services,0xA5,sizeof services);
  gml_gpu_device_services(gpu,&services);
  REQUIRE(!services.context && !services.submit,"no service is bound before context adoption");
  context=fake_context(GML_GPU_API_OPENGL_CORE);
  REQUIRE(gml_gpu_context_reset(gpu,&context),"context adopted");
  gml_gpu_device_services(gpu,&services);
  REQUIRE(services.context==gpu && services.capabilities && services.submit &&
          services.upload && services.readback && services.retire && services.flush && services.wait,
          "the neutral service has a complete callback table");
  memset(&caps,0xA5,sizeof caps);
  REQUIRE(services.capabilities(services.context,&caps),"capability query succeeds");
  REQUIRE(caps.max_texture_extent==4096 && caps.max_batch_quads==2048 &&
          caps.supported_operations==supported_operations &&
          caps.supported_families==supported_families,
          "the exact selected operation and neutral family masks are advertised");
  memset(&op,0,sizeof op);
  op.opcode=GML_RENDER_OP_LAYER_EFFECT;
  memset(&batch,0,sizeof batch);
  batch.operations=&op;
  batch.operation_count=1;
  draws=anygm_test_graphics_draw_calls();
  result=services.submit(services.context,&batch);
  REQUIRE(result.status==GML_RENDER_EXEC_UNSUPPORTED && result.accepted_prefix==0 &&
          result.uncertain_target_count==0 && !result.all_destinations_uncertain,
          "unsupported scene submission refuses before a write");
  REQUIRE(anygm_test_graphics_draw_calls()==draws,"unsupported scene work reaches no driver draw");
  {
    GmlGpuCounters counters;
    gml_gpu_counters(gpu,&counters);
    REQUIRE(counters.scene_batches_offered==1u &&
            counters.scene_batches_accepted==0u &&
            counters.scene_batches_refused==1u &&
            counters.scene_partial_prefix==0u &&
            counters.scene_operations[GML_RENDER_OP_LAYER_EFFECT]==0u,
            "a refused scene batch cannot appear as completed work");
  }
  {
    GmlRenderDrawOp operations[2]={0};
    GmlRenderDrawBatch refused={operations,2u,NULL,0u};
    GmlGpuCounters counters;
    int clears=anygm_test_graphics_clear_calls();
    operations[0].opcode=GML_RENDER_OP_CLEAR;
    operations[0].target.image=(GmlRenderImageRef){GML_RENDER_RESOURCE_SURFACE,7u,1u,1u};
    operations[0].target.storage_version=1u;
    operations[0].target.width=operations[0].target.height=8u;
    operations[0].target.region=(GmlRenderRect){0,0,8u,8u};
    operations[0].target.pixel_format=GML_RENDER_PIXEL_ARGB8888;
    operations[0].payload.clear_colour=0xFF123456u;
    operations[1].target=operations[0].target;
    operations[1].target.image.content_version=2u;
    for(size_t index=0;index<sizeof unselected/sizeof *unselected;index++){
      operations[1].opcode=unselected[index];
      result=services.submit(services.context,&refused);
      REQUIRE(result.status==GML_RENDER_EXEC_UNSUPPORTED && result.accepted_prefix==0u &&
              result.uncertain_target_count==0u && !result.all_destinations_uncertain,
              "each unselected opcode rejects the whole batch before its valid clear prefix");
      REQUIRE(anygm_test_graphics_clear_calls()==clears &&
              anygm_test_graphics_draw_calls()==draws,
              "an unselected opcode causes no clear or draw");
      gml_gpu_counters(gpu,&counters);
      REQUIRE(counters.scene_operations[unselected[index]]==0u &&
              counters.scene_operations[GML_RENDER_OP_CLEAR]==0u,
              "neither an unselected opcode nor its refused prefix is counted as GPU work");
    }
    gml_gpu_counters(gpu,&counters);
    REQUIRE(counters.scene_batches_offered==1u+sizeof unselected/sizeof *unselected &&
            counters.scene_batches_refused==1u+sizeof unselected/sizeof *unselected &&
            counters.scene_batches_accepted==0u && counters.scene_partial_prefix==0u,
            "all unselected opcode refusals retain exact batch accounting");
    refused.operation_count=1u;
    result=services.submit(services.context,&refused);
    REQUIRE(result.status==GML_RENDER_EXEC_ACCEPTED && result.accepted_prefix==1u &&
            anygm_test_graphics_clear_calls()==clears+1,
            "the identical clear prefix is valid when submitted without an unselected suffix");
  }
  gml_gpu_context_lost(gpu);
  gml_gpu_device_services(gpu,&services);
  REQUIRE(!services.context && !services.submit,"a lost context offers no service");
  gml_gpu_destroy(gpu,0);
  return 1;
}

static int scene_optional_entry_refusal_case(void){
  GmlGpu *gpu;
  GmlGpuContext context;
  GmlRenderDeviceServices services;
  GmlRenderDeviceCaps caps;
  anygm_test_graphics_reset();
  anygm_test_graphics_withhold("glGetIntegerv");
  gpu=gml_gpu_create();
  REQUIRE(gpu!=NULL,"metadata allocated");
  context=fake_context(GML_GPU_API_OPENGL_CORE);
  REQUIRE(gml_gpu_context_reset(gpu,&context),
          "a missing scene-only entry point does not reject the established hybrid context");
  gml_gpu_device_services(gpu,&services);
  REQUIRE(!services.capabilities(services.context,&caps),
          "the scene executor does not advertise unqueryable limits");
  REQUIRE(!gml_gpu_prepare_scene(gpu,16u,12u),
          "optional preparation does not require missing scene entry points");
  gml_gpu_destroy(gpu,1);
  anygm_test_graphics_reset();
  gpu=gml_gpu_create();
  context=fake_context(GML_GPU_API_OPENGLES3);
  REQUIRE(gpu && gml_gpu_context_reset(gpu,&context),"preparation retry context adopted");
  anygm_test_graphics_fail_compile(1);
  REQUIRE(!gml_gpu_prepare_scene(gpu,16u,12u),"preparation compile failure is nonfatal");
  anygm_test_graphics_fail_compile(0);
  REQUIRE(gml_gpu_prepare_scene(gpu,16u,12u),"ordinary preparation can retry after failure");
  REQUIRE(anygm_test_graphics_scene_draw_calls()==1 && anygm_test_graphics_read_pixels()==0,
          "failed and retried preparation never publishes pixels");
  gml_gpu_context_lost(gpu);
  REQUIRE(gml_gpu_context_reset(gpu,&context),"terminal retry context adopted");
  unsigned draws=(unsigned)anygm_test_graphics_draw_calls();
  unsigned finishes=anygm_test_graphics_finish_calls();
  anygm_test_graphics_fail_terminal_draw_at(draws+1u);
  REQUIRE(!gml_gpu_prepare_scene(gpu,16u,12u) &&
          anygm_test_graphics_bound_framebuffer()==7u &&
          anygm_test_graphics_finish_calls()==finishes && !anygm_test_graphics_read_pixels(),
          "failed private terminal preparation restores the frontend without observation");
  anygm_test_graphics_fail_terminal_draw_at(0u);
  REQUIRE(gml_gpu_prepare_scene(gpu,16u,12u) && gml_gpu_prepare_scene(gpu,16u,12u) &&
          anygm_test_graphics_draw_calls()==(int)draws+2 &&
          anygm_test_graphics_finish_calls()==finishes+1u &&
          anygm_test_graphics_bound_framebuffer()==7u,
          "failed terminal warm-up retries once, then reuses its completed preparation");
  gml_gpu_context_lost(gpu);
  REQUIRE(gml_gpu_context_reset(gpu,&context),"composition retry context adopted");
  unsigned scene_draws=(unsigned)anygm_test_graphics_scene_draw_calls();
  finishes=anygm_test_graphics_finish_calls();
  anygm_test_graphics_fail_scene_draw_at(scene_draws+1u);
  REQUIRE(!gml_gpu_prepare_scene(gpu,16u,12u) &&
          anygm_test_graphics_bound_framebuffer()==7u &&
          anygm_test_graphics_finish_calls()==finishes && !anygm_test_graphics_read_pixels(),
          "failed private composition restores the frontend without observation");
  anygm_test_graphics_fail_scene_draw_at(0u);
  REQUIRE(gml_gpu_prepare_scene(gpu,16u,12u) && gml_gpu_prepare_scene(gpu,16u,12u) &&
          anygm_test_graphics_scene_draw_calls()==(int)scene_draws+2 &&
          anygm_test_graphics_finish_calls()==finishes+1u &&
          anygm_test_graphics_bound_framebuffer()==7u,
          "failed composition preparation retries once and then reuses completion");
  gml_gpu_destroy(gpu,1);
  return 1;
}

static int scene_content_source_orientation_case(void){
  uint32_t pixels[4]={0xFF102030u,0xFF405060u,0xFF708090u,0xFFA0B0C0u};
  GmlRenderDeviceServices services;
  GmlRenderImageUpload upload={0};
  GmlGpu *gpu;
  GmlGpuContext context;
  const unsigned char *bytes;
  size_t count=0;
  unsigned source_name,scratch_name;
  anygm_test_graphics_reset();
  gpu=gml_gpu_create();
  REQUIRE(gpu!=NULL,"orientation device allocated");
  context=fake_context(GML_GPU_API_OPENGL_CORE);
  REQUIRE(gml_gpu_context_reset(gpu,&context),"orientation context adopted");
  gml_gpu_device_services(gpu,&services);
  upload.image=(GmlRenderImageRef){GML_RENDER_RESOURCE_SHADER_SOURCE,0u,1u,1u};
  upload.storage_width=upload.storage_height=2u;
  upload.region=(GmlRenderRect){0,0,2u,2u};
  upload.pixel_format=GML_RENDER_PIXEL_ARGB8888;
  upload.stride_bytes=8u;
  upload.byte_capacity=sizeof pixels;
  upload.bytes=pixels;
  REQUIRE(services.upload(services.context,&upload),"content source uploaded");
  source_name=anygm_test_graphics_first_texture_with_format(GL_RGBA8);
  bytes=anygm_test_graphics_texture_bytes(source_name,&count);
  REQUIRE(bytes && count==sizeof pixels && bytes[0]==0x10u &&
          bytes[1]==0x20u && bytes[2]==0x30u && bytes[3]==0xFFu,
          "content source keeps the terminal program's top source row first");
  upload.image=(GmlRenderImageRef){GML_RENDER_RESOURCE_SCRATCH,0u,2u,1u};
  REQUIRE(services.upload(services.context,&upload),"ordinary scratch uploaded");
  scratch_name=anygm_test_graphics_texture_with_format(GL_RGBA8);
  bytes=anygm_test_graphics_texture_bytes(scratch_name,&count);
  REQUIRE(scratch_name!=source_name && bytes && count==sizeof pixels &&
          bytes[0]==0x70u && bytes[1]==0x80u && bytes[2]==0x90u &&
          bytes[3]==0xFFu,
          "other scene scratch keeps its established render-target orientation");
  gml_gpu_destroy(gpu,1);
  for(unsigned missing=0;missing<2u;missing++){
    GmlRenderImageRef source={GML_RENDER_RESOURCE_SURFACE,3u,3u,1u};
    GmlRenderDrawOp operations[2]={0};
    GmlRenderDrawStateSnapshot state={0};
    GmlRenderDrawBatch batch={operations,2u,&state,1u};
    GmlPlanShader shader={0};
    GmlRenderExecutionResult result;
    anygm_test_graphics_reset();
    if(missing) anygm_test_graphics_withhold("glBlitFramebuffer");
    gpu=gml_gpu_create();
    REQUIRE(gpu && gml_gpu_context_reset(gpu,&context),"content-view context adopted");
    gml_gpu_device_services(gpu,&services);
    upload.image=source;
    REQUIRE(services.upload(services.context,&upload),"mutable content source uploaded");
    shader.identity=1u;shader.content_generation=1u;
    shader.vertex_es="attribute vec3 in_Position; uniform mat4 gm_Matrices[5]; void main(){gl_Position=gm_Matrices[4]*vec4(in_Position,1.0);}";
    shader.fragment_es="uniform sampler2D gm_BaseTexture; void main(){gl_FragColor=texture2D(gm_BaseTexture,vec2(0.5));}";
    for(unsigned channel=0;channel<4u;channel++) shader.vertex_colour[channel]=1.0f;
    for(unsigned op=0;op<2u;op++){
      operations[op].target.image=(GmlRenderImageRef){GML_RENDER_RESOURCE_SCREEN,4u,4u,op+1u};
      operations[op].target.width=operations[op].target.height=4u;
      operations[op].target.storage_version=1u;
      operations[op].target.pixel_format=GML_RENDER_PIXEL_ARGB8888;
      operations[op].target.region=(GmlRenderRect){0,0,4u,4u};
    }
    operations[0].opcode=GML_RENDER_OP_CLEAR;
    operations[0].reconstruction_mode=GML_RENDER_REPLAY_EXACT;
    operations[0].payload.clear_colour=0xFF223344u;
    operations[1].opcode=GML_RENDER_OP_CONTENT_SHADER_DRAW;
    operations[1].reconstruction_mode=GML_RENDER_DEVICE_CHECKPOINT;
    operations[1].source_count=1u;operations[1].sources=&source;
    operations[1].payload.shader=&shader;
    operations[1].target.region=(GmlRenderRect){1,1,2u,2u};
    state.write_mask=15u;state.alpha=255u;state.colour=0xFFFFFFFFu;
    state.sampler_count=1u;
    state.samplers[0].filter=GML_RENDER_FILTER_NEAREST;
    state.samplers[0].edge_rule=GML_RENDER_EDGE_CLAMP_SUBIMAGE;
    state.samplers[0].source_region=(GmlRenderRect){0,0,2u,2u};
    result=services.submit(services.context,&batch);
    if(missing){
      REQUIRE(result.status==GML_RENDER_EXEC_UNSUPPORTED && result.accepted_prefix==0u &&
              anygm_test_graphics_clear_calls()==0 &&
              anygm_test_graphics_quad_draw_calls()==0 &&
              anygm_test_graphics_scene_copies()==0u,
              "a missing orientation entry point refuses the whole batch before its clear");
      gml_gpu_destroy(gpu,1);
      continue;
    }
    {
      float u=-1.0f,v=-1.0f;
      REQUIRE(result.status==GML_RENDER_EXEC_ACCEPTED && result.accepted_prefix==2u &&
              anygm_test_graphics_scene_copies()==1u &&
              anygm_test_graphics_scene_copied_pixels()==4u &&
              anygm_test_graphics_read_pixels()==0u &&
              anygm_test_graphics_quad_texcoord(0,&u,&v) && u==0.0f && v==0.0f,
              "contained content geometry uses a native device copy and unchanged authored UV");
    }
    for(unsigned op=0;op<2u;op++) operations[op].target.image.content_version+=2u;
    result=services.submit(services.context,&batch);
    REQUIRE(result.status==GML_RENDER_EXEC_ACCEPTED && anygm_test_graphics_scene_copies()==1u,
            "a current content-source version reuses its oriented device view");
    source.content_version++;upload.image=source;
    REQUIRE(services.upload(services.context,&upload),"a newer source version uploaded");
    for(unsigned op=0;op<2u;op++) operations[op].target.image.content_version+=2u;
    result=services.submit(services.context,&batch);
    REQUIRE(result.status==GML_RENDER_EXEC_ACCEPTED && anygm_test_graphics_scene_copies()==2u,
            "a changed source version refreshes its device-only view");
    {
      static const GmlRenderRect invalid[]={{-1,1,2u,2u},{1,1,0u,2u},
        {4,1,2u,2u},{INT32_MAX,1,UINT32_MAX,2u}};
      int clears=anygm_test_graphics_clear_calls(),draws=anygm_test_graphics_quad_draw_calls();
      for(unsigned index=0;index<sizeof invalid/sizeof invalid[0];index++){
        operations[1].target.region=invalid[index];
        result=services.submit(services.context,&batch);
        REQUIRE(result.status==GML_RENDER_EXEC_UNSUPPORTED && result.accepted_prefix==0u &&
                anygm_test_graphics_clear_calls()==clears &&
                anygm_test_graphics_quad_draw_calls()==draws &&
                anygm_test_graphics_scene_copies()==2u,
                "invalid content regions reject the complete batch before writes or copies");
      }
    }
    {
      static const GmlRenderRect invalid_source[]={{-1,0,1u,2u},{0,0,0u,2u},
        {2,0,1u,2u},{INT32_MAX,0,UINT32_MAX,2u}};
      operations[1].target.region=(GmlRenderRect){1,1,2u,2u};
      state.rgb_source_factor=GML_RENDER_BLEND_SOURCE_ALPHA;
      state.rgb_destination_factor=GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA;
      state.alpha_source_factor=GML_RENDER_BLEND_ONE;
      state.alpha_destination_factor=GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA;
      state.rgb_equation=state.alpha_equation=GML_RENDER_BLEND_ADD;
      state.coverage_policy=GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA;
      state.samplers[0].source_region=(GmlRenderRect){1,0,1u,2u};
      for(unsigned op=0;op<2u;op++) operations[op].target.image.content_version+=2u;
      unsigned alpha_draws=anygm_test_graphics_scene_mask_draws(8u);
      result=services.submit(services.context,&batch);
      REQUIRE(result.status==GML_RENDER_EXEC_ACCEPTED && result.accepted_prefix==2u &&
              anygm_test_graphics_scene_copies()==2u &&
              anygm_test_graphics_read_pixels()==0u &&
              anygm_test_graphics_scene_filter()==GL_NEAREST &&
              anygm_test_graphics_scene_mask_draws(8u)==alpha_draws+1u,
              "a cropped program uses the device-only intermediate and covered-alpha pass");
      int clears=anygm_test_graphics_clear_calls(),draws=anygm_test_graphics_quad_draw_calls();
      for(unsigned index=0;index<sizeof invalid_source/sizeof invalid_source[0];index++){
        state.samplers[0].source_region=invalid_source[index];
        result=services.submit(services.context,&batch);
        REQUIRE(result.status==GML_RENDER_EXEC_UNSUPPORTED && result.accepted_prefix==0u &&
                anygm_test_graphics_clear_calls()==clears &&
                anygm_test_graphics_quad_draw_calls()==draws &&
                anygm_test_graphics_scene_copies()==2u,
                "invalid source crops refuse before clears, draws or orientation copies");
      }
      state.samplers[0].source_region=(GmlRenderRect){1,0,1u,2u};
      state.coverage_policy=0u;
      result=services.submit(services.context,&batch);
      REQUIRE(result.status==GML_RENDER_EXEC_UNSUPPORTED && result.accepted_prefix==0u &&
              anygm_test_graphics_clear_calls()==clears &&
              anygm_test_graphics_quad_draw_calls()==draws,
              "a crop without the qualified covered-alpha policy refuses atomically");
      state.samplers[0].source_region=(GmlRenderRect){0,0,2u,2u};
      state.rgb_source_factor=state.rgb_destination_factor=0u;
      state.alpha_source_factor=state.alpha_destination_factor=0u;
      state.rgb_equation=state.alpha_equation=0u;
    }
    {
      int deletes=anygm_test_graphics_deletes();
      services.retire(services.context,source);
      REQUIRE(anygm_test_graphics_deletes()==deletes,
              "logical retirement never deletes while the context might be absent");
      source.incarnation++;upload.image=source;
      REQUIRE(services.upload(services.context,&upload),"a new incarnation allocates while current");
      REQUIRE(anygm_test_graphics_deletes()>=deletes+4,
              "current allocation collects the retired original and oriented image names");
    }
    operations[1].target.region=(GmlRenderRect){1,1,2u,2u};
    for(unsigned op=0;op<2u;op++) operations[op].target.image.content_version+=2u;
    result=services.submit(services.context,&batch);
    REQUIRE(result.status==GML_RENDER_EXEC_ACCEPTED && anygm_test_graphics_scene_copies()==3u,
            "a new incarnation creates its own oriented view before context loss");
    gml_gpu_context_lost(gpu);
    anygm_test_graphics_forbid_calls();
    gml_gpu_destroy(gpu,1);
    REQUIRE(anygm_test_graphics_calls_after_forget()==0,
            "an oriented content source makes no call after context loss");
  }
  return 1;
}

static int scene_empty_screen_recovery_case(void){
  for(unsigned embedded=0;embedded<2u;embedded++){
    GmlGpu *gpu;
    GmlGpuContext context=fake_context(embedded?GML_GPU_API_OPENGLES3:GML_GPU_API_OPENGL_CORE);
    GmlRenderDeviceServices device={0};
    GmlRenderImageRef source={GML_RENDER_RESOURCE_APPLICATION,0u,1u,1u};
    GmlRenderDrawStateSnapshot state={0};
    GmlRenderDrawOp operations[2]={0};
    GmlRenderDrawBatch batch={operations,2u,&state,1u};
    uint32_t pixels[9u*5u]={0};
    GmlRenderImageUpload upload={0};
    anygm_test_graphics_reset();
    gpu=gml_gpu_create();
    REQUIRE(gpu && gml_gpu_context_reset(gpu,&context),"recovery context adopted");
    gml_gpu_device_services(gpu,&device);
    upload.image=source;upload.storage_width=9u;upload.storage_height=5u;
    upload.region=(GmlRenderRect){0,0,9u,5u};upload.pixel_format=GML_RENDER_PIXEL_ARGB8888;
    upload.stride_bytes=36u;upload.byte_capacity=sizeof pixels;upload.bytes=pixels;
    REQUIRE(device.upload(device.context,&upload),"odd application uploaded");
    state.write_mask=15u;state.alpha=255u;state.colour=0xFFFFFFFFu;
    state.rgb_source_factor=GML_RENDER_BLEND_SOURCE_ALPHA;
    state.rgb_destination_factor=GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA;
    state.alpha_source_factor=GML_RENDER_BLEND_ONE;
    state.alpha_destination_factor=GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA;
    state.rgb_equation=state.alpha_equation=GML_RENDER_BLEND_ADD;
    state.coverage_policy=GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA;
    state.sampler_count=1u;
    state.samplers[0]=(GmlRenderSamplingPolicy){0};
    state.samplers[0].filter=GML_RENDER_FILTER_NEAREST;
    state.samplers[0].edge_rule=GML_RENDER_EDGE_CLAMP_SUBIMAGE;
    state.samplers[0].source_region=upload.region;
    for(unsigned index=0;index<2u;index++){
      GmlRenderDrawOp *op=&operations[index];
      op->target.image=(GmlRenderImageRef){GML_RENDER_RESOURCE_SCREEN,0u,2u,index+1u};
      op->target.storage_version=1u;op->target.width=35u;op->target.height=17u;
      op->target.pixel_format=GML_RENDER_PIXEL_ARGB8888;
      op->target.region=(GmlRenderRect){0,0,35u,17u};
      op->reconstruction_mode=index?GML_RENDER_DEVICE_CHECKPOINT:GML_RENDER_REPLAY_EXACT;
      op->opcode=index?GML_RENDER_OP_RECOVER_EMPTY_SCREEN:GML_RENDER_OP_CLEAR;
      if(!index) continue;
      op->scene_family=GML_RENDER_SCENE_SURFACE;op->sources=&source;op->source_count=1u;
      GmlRenderQuad *q=&op->payload.quad;
      q->shape=GML_RENDER_QUAD_AXIS_ALIGNED;q->sampling=state.samplers[0];
      q->filtered_draw_alpha=1.0;
      q->x[1]=q->x[2]=35.0;q->y[2]=q->y[3]=17.0;
      q->u[1]=q->u[2]=q->v[2]=q->v[3]=1.0;
      for(unsigned corner=0;corner<4u;corner++) q->corner_colour[corner]=0xFFFFFFFFu;
    }
    GmlRenderExecutionResult result=device.submit(device.context,&batch);
    REQUIRE(result.status==GML_RENDER_EXEC_ACCEPTED && result.accepted_prefix==2u &&
            !anygm_test_graphics_rgb_queries() && !anygm_test_graphics_read_pixels() &&
            !anygm_test_graphics_scene_copies() && anygm_test_graphics_scene_draw_calls()==2,
            "reduction and conditional RGB/alpha blit have no query, snapshot or CPU read");
    REQUIRE(anygm_test_graphics_color_mask_all() && !anygm_test_graphics_attributes_enabled() &&
            !anygm_test_graphics_bound_program() && !anygm_test_graphics_bound_vertex_array(),
            "conditional draw restores shared write/array/program state");
    for(unsigned cutoff=0;cutoff<256u;cutoff++){
      state.alpha_test_enabled=1u;state.alpha_test_reference=cutoff;
      result=device.submit(device.context,&batch);
      REQUIRE(result.status==GML_RENDER_EXEC_ACCEPTED && result.accepted_prefix==2u &&
              !anygm_test_graphics_rgb_queries() && !anygm_test_graphics_read_pixels() &&
              !anygm_test_graphics_scene_copies(),
              "every raw conditional cutoff remains device-only");
    }
    state.alpha_test_enabled=state.alpha_test_reference=0u;
    for(unsigned variant=0;variant<12u;variant++){
      GmlRenderDrawOp invalid[2];GmlRenderDrawStateSnapshot bad_state=state;
      GmlRenderDrawBatch refused={invalid,2u,&bad_state,1u};
      memcpy(invalid,operations,sizeof invalid);
      if(variant==0u) invalid[1].reconstruction_mode=GML_RENDER_REPLAY_EXACT;
      if(variant==1u) invalid[1].scene_family=GML_RENDER_SCENE_SPRITE;
      if(variant==2u) invalid[1].target.image.resource_class=GML_RENDER_RESOURCE_APPLICATION;
      if(variant==3u) invalid[1].payload.quad.x[1]=35.25;
      if(variant==4u) invalid[1].payload.quad.sampling.filter=GML_RENDER_FILTER_BILINEAR;
      if(variant==5u) bad_state.coverage_policy=0u;
      if(variant==6u) bad_state.write_mask=7u;
      if(variant==7u) bad_state.clip=(GmlRenderRect){1,1,2u,2u};
      if(variant==8u) invalid[1].payload.quad.corner_colour[2]=0x80FFFFFFu;
      if(variant==9u) invalid[1].payload.quad.u[1]=0.5;
      if(variant==10u) bad_state.alpha_test_enabled=2u;
      if(variant==11u) bad_state.alpha_test_reference=256u;
      int clears=anygm_test_graphics_clear_calls(),draws=anygm_test_graphics_draw_calls();
      result=device.submit(device.context,&refused);
      REQUIRE(result.status==GML_RENDER_EXEC_UNSUPPORTED && !result.accepted_prefix &&
              clears==anygm_test_graphics_clear_calls() && draws==anygm_test_graphics_draw_calls(),
              "invalid conditional metadata refuses before the valid clear prefix");
    }
    gml_gpu_context_lost(gpu);
    anygm_test_graphics_forbid_calls();
    result=device.submit(device.context,&batch);
    REQUIRE(result.status==GML_RENDER_EXEC_UNSUPPORTED && !anygm_test_graphics_calls_after_forget(),
            "lost conditional namespace is never reused or observed");
    anygm_test_graphics_allow_calls();gml_gpu_destroy(gpu,0);
  }
  return 1;
}

static int scene_completed_screen_query_case(void){
  for(unsigned api=GML_GPU_API_OPENGL_CORE;api<=GML_GPU_API_OPENGLES3;api++){
    uint32_t pixels[6]={0xFF000000u,0u,0xFF000000u,0u,0u,0x00000001u};
    GmlRenderImageRef image={GML_RENDER_RESOURCE_SCREEN,0u,91u,1u};
    GmlRenderImageUpload upload={0};
    GmlRenderDeviceServices services={0};
    GmlGpuCounters counters={0};
    int has_rgb=-1;
    anygm_test_graphics_reset();
    GmlGpu *gpu=gml_gpu_create();
    GmlGpuContext context=fake_context(api);
    REQUIRE(gpu && gml_gpu_context_reset(gpu,&context),"query context adopted");
    gml_gpu_device_services(gpu,&services);
    upload.image=image;
    upload.storage_width=2u; upload.storage_height=3u;
    upload.region=(GmlRenderRect){0,0,2u,3u};
    upload.pixel_format=GML_RENDER_PIXEL_ARGB8888;
    upload.stride_bytes=8u; upload.byte_capacity=sizeof pixels; upload.bytes=pixels;
    REQUIRE(services.upload(services.context,&upload),"current SCREEN uploaded for query fixture");
    anygm_test_graphics_rgb_query_result(0u);
    REQUIRE(services.has_rgb && services.has_rgb(services.context,image,&has_rgb) && !has_rgb,
            "a completed exact query may report RGB black");
    anygm_test_graphics_rgb_query_result(1u);
    REQUIRE(services.has_rgb(services.context,image,&has_rgb) && has_rgb &&
            anygm_test_graphics_rgb_queries()==2u && !anygm_test_graphics_read_pixels(),
            "query returns a boolean without pixel readback");
    REQUIRE(anygm_test_graphics_color_mask_all() &&
            !anygm_test_graphics_bound_program() && !anygm_test_graphics_bound_vertex_array(),
            "query restores write mask and unbinds its program and array");
    GmlRenderImageRef stale=image; stale.content_version++;
    REQUIRE(!services.has_rgb(services.context,stale,&has_rgb) &&
            anygm_test_graphics_rgb_queries()==2u,"stale image rejected before query draw");
    anygm_test_graphics_active_query(77u);
    REQUIRE(!services.has_rgb(services.context,image,&has_rgb) &&
            anygm_test_graphics_rgb_queries()==2u,"a foreign active query is not commandeered");
    anygm_test_graphics_active_query(0u);
    gml_gpu_counters(gpu,&counters);
    REQUIRE(counters.scene_rgb_queries==2u && counters.scene_rgb_query_refusals==2u &&
            counters.scene_readbacks==0u,"query costs and refusals remain separate from checkpoints");
    gml_gpu_context_lost(gpu);
    anygm_test_graphics_forbid_calls();
    REQUIRE(!services.has_rgb(services.context,image,&has_rgb),"lost context refuses without a call");
    gml_gpu_destroy(gpu,0);
    REQUIRE(!anygm_test_graphics_calls_after_forget(),"query names are forgotten outside context");
    const char *optional_queries[]={"glGenQueries","glDeleteQueries","glBeginQuery",
      "glEndQuery","glGetQueryiv","glGetQueryObjectuiv","glPolygonMode"};
    for(unsigned missing=0;missing<sizeof optional_queries/sizeof optional_queries[0];missing++){
      if(api==GML_GPU_API_OPENGLES3 && missing==6u) continue;
      anygm_test_graphics_reset();
      anygm_test_graphics_withhold(optional_queries[missing]);
      gpu=gml_gpu_create(); context=fake_context(api);
      REQUIRE(gpu && gml_gpu_context_reset(gpu,&context),
              "missing optional query entries do not reject a working graphics context");
      gml_gpu_device_services(gpu,&services);
      REQUIRE(services.upload(services.context,&upload) &&
              !services.has_rgb(services.context,image,&has_rgb) &&
              !anygm_test_graphics_rgb_queries() && !anygm_test_graphics_read_pixels(),
              "an unavailable exact query refuses before drawing or reading pixels");
      gml_gpu_destroy(gpu,1);
    }
  }
  return 1;
}

static int scene_resident_batch_case(void){
  enum { QUADS=500 };
  uint32_t pixels[4]={0xFF112233u,0xFF445566u,0xFF778899u,0xFFAABBCCu};
  GmlRenderDrawOp *operations;
  GmlRenderDrawStateSnapshot state;
  GmlRenderDrawBatch batch;
  GmlRenderImageRef source_ref={GML_RENDER_RESOURCE_ATLAS,4u,1u,1u};
  GmlRenderImageRef target_ref={GML_RENDER_RESOURCE_SURFACE,7u,1u,1u};
  GmlRenderImageUpload upload;
  GmlRenderImageReadback readback;
  GmlRenderDeviceServices services;
  GmlRenderExecutionResult result;
  GmlGpu *gpu;
  GmlGpuContext context;
  uint32_t output[4]={0};
  size_t uploaded_size=0;
  const unsigned char *uploaded;
  unsigned atlas_name;
  anygm_test_graphics_reset();
  gpu=gml_gpu_create();
  REQUIRE(gpu!=NULL,"metadata allocated");
  context=fake_context(GML_GPU_API_OPENGL_CORE);
  REQUIRE(gml_gpu_context_reset(gpu,&context),"context adopted");
  gml_gpu_device_services(gpu,&services);
  memset(&upload,0,sizeof upload);
  upload.image=source_ref;
  upload.storage_width=upload.storage_height=2u;
  upload.region=(GmlRenderRect){0,0,2u,2u};
  upload.pixel_format=GML_RENDER_PIXEL_XRGB8888;
  upload.stride_bytes=8u;
  upload.byte_capacity=sizeof pixels;
  upload.bytes=pixels;
  REQUIRE(services.upload(services.context,&upload),"the atlas uploads once");
  atlas_name=anygm_test_graphics_first_texture_with_format(GL_RGBA8);
  uploaded=anygm_test_graphics_texture_bytes(atlas_name,&uploaded_size);
  REQUIRE(uploaded && uploaded_size==sizeof pixels &&
          uploaded[0]==0x11u && uploaded[1]==0x22u &&
          uploaded[2]==0x33u && uploaded[3]==0xFFu,
          "packed source words become ordered RGBA bytes without swapping channels");
  operations=(GmlRenderDrawOp*)calloc(QUADS+1u,sizeof *operations);
  REQUIRE(operations!=NULL,"batch allocated");
  memset(&state,0,sizeof state);
  state.write_mask=15u;
  state.alpha=255u;
  state.colour=0xFFFFFFFFu;
  state.sampler_count=1u;
  state.samplers[0].filter=GML_RENDER_FILTER_NEAREST;
  state.samplers[0].edge_rule=GML_RENDER_EDGE_CLAMP_SUBIMAGE;
  state.samplers[0].source_region=(GmlRenderRect){0,0,2u,2u};
  for(uint32_t index=0;index<QUADS+1u;index++){
    GmlRenderDrawOp *op=&operations[index];
    op->sequence=index+1u;
    op->target.image=target_ref;
    op->target.image.content_version=index+1u;
    op->target.storage_version=index+1u;
    op->target.width=64u;
    op->target.height=64u;
    op->target.pixel_format=GML_RENDER_PIXEL_XRGB8888;
    op->target.region=(GmlRenderRect){0,0,64u,64u};
    if(index==0u){
      op->opcode=GML_RENDER_OP_CLEAR;
      op->payload.clear_colour=0xFF123456u;
      continue;
    }
    op->opcode=GML_RENDER_OP_TEXTURED_QUAD;
    op->state_index=0u;
    op->source_count=1u;
    op->sources=&source_ref;
    op->payload.quad.shape=GML_RENDER_QUAD_AXIS_ALIGNED;
    op->payload.quad.sampling=state.samplers[0];
    for(uint32_t corner=0;corner<4;corner++)
      op->payload.quad.corner_colour[corner]=0xFFFFFFFFu;
    {
      double x=(double)((index*7u)%60u),y=(double)((index*11u)%60u);
      op->payload.quad.x[0]=op->payload.quad.x[3]=x;
      op->payload.quad.x[1]=op->payload.quad.x[2]=x+4.0;
      op->payload.quad.y[0]=op->payload.quad.y[1]=y;
      op->payload.quad.y[2]=op->payload.quad.y[3]=y+4.0;
      op->payload.quad.u[0]=op->payload.quad.u[3]=0.0;
      op->payload.quad.u[1]=op->payload.quad.u[2]=1.0;
      op->payload.quad.v[0]=op->payload.quad.v[1]=0.0;
      op->payload.quad.v[2]=op->payload.quad.v[3]=1.0;
    }
  }
  batch.operations=operations;
  batch.operation_count=QUADS+1u;
  batch.states=&state;
  batch.state_count=1u;
  result=services.submit(services.context,&batch);
  REQUIRE(result.status==GML_RENDER_EXEC_ACCEPTED && result.accepted_prefix==QUADS+1u,
          "clear and 500 ordered quads are accepted completely");
  REQUIRE(anygm_test_graphics_scene_draw_calls()==1 &&
          anygm_test_graphics_scene_vertices()==QUADS*6,
          "500 quads issue one scene draw call");
  REQUIRE(anygm_test_graphics_buffer_bytes()==QUADS*52u*sizeof(float),
          "ordinary scene upload carries one compact record per quad");
  REQUIRE(anygm_test_graphics_clear_calls()==1 &&
          anygm_test_graphics_scene_texture()==atlas_name,
          "the batch uses the resident atlas and target clear");
  REQUIRE(anygm_test_graphics_read_pixels()==0,
          "ordinary resident submission never downloads its target");
  REQUIRE(anygm_test_graphics_texture_data_uploads(atlas_name)==1,
          "one atlas data upload serves all 500 quads");
  {
    GmlGpuCounters counters;
    gml_gpu_counters(gpu,&counters);
    REQUIRE(counters.scene_batches_offered==1u && counters.scene_batches_accepted==1u &&
            counters.scene_batches_refused==0u &&
            counters.scene_operations[GML_RENDER_OP_CLEAR]==1u &&
            counters.scene_operations[GML_RENDER_OP_TEXTURED_QUAD]==QUADS &&
            counters.scene_uploads==1u && counters.scene_uploaded_bytes==sizeof pixels &&
            counters.scene_readbacks==0u && counters.scene_readback_bytes==0u &&
            counters.cpu_upload_frames==0u,
            "resident scene statistics separate accepted draws and transport");
  }
  REQUIRE(services.flush(services.context) && services.wait(services.context),
          "issuing and waiting are separate service operations");
  memset(&readback,0,sizeof readback);
  target_ref.content_version=QUADS+1u;
  readback.image=target_ref;
  readback.storage_width=readback.storage_height=64u;
  readback.region=(GmlRenderRect){0,0,2u,2u};
  readback.pixel_format=GML_RENDER_PIXEL_XRGB8888;
  readback.stride_bytes=8u;
  readback.byte_capacity=sizeof output;
  readback.bytes=output;
  REQUIRE(services.readback(services.context,&readback),"an explicit CPU read completes");
  REQUIRE(anygm_test_graphics_read_pixels()==1 &&
          output[0]==0xFF010033u && output[2]==0xFF000033u,
          "readback flips rows and packs the runtime word only on demand");
  {
    GmlGpuCounters counters;
    gml_gpu_counters(gpu,&counters);
    REQUIRE(counters.scene_readbacks==1u && counters.scene_readback_bytes==sizeof output,
            "the explicit subregion readback is counted by byte area");
  }
  services.retire(services.context,target_ref);
  {
    AnygmTestGraphicsStream stream=anygm_test_graphics_stream();
    const size_t bytes=QUADS*52u*sizeof(float);
    REQUIRE(stream.allocations==1u && stream.updates==1u && stream.offset==0u &&
            stream.capacity==4u*2048u*52u*sizeof(float) && stream.errors==0u,
            "compact stream allocates once and uses correct instance attributes");
    /* Sixteen 500-quad batches fit; the seventeenth must orphan, never overwrite
     * an in-flight region. A new target version also proves the cursor survives submit. */
    for(unsigned repeat=1;repeat<18u;repeat++){
      for(unsigned index=0;index<QUADS+1u;index++){
        operations[index].target.image.content_version+=QUADS+1u;
        operations[index].target.storage_version+=QUADS+1u;
      }
      result=services.submit(services.context,&batch);
      stream=anygm_test_graphics_stream();
      REQUIRE(result.status==GML_RENDER_EXEC_ACCEPTED &&
              stream.updates==repeat+1u && stream.allocations==(repeat<16u?1u:2u) &&
              stream.offset==(repeat%16u)*bytes && stream.errors==0u &&
              stream.uploaded_bytes==(repeat+1u)*bytes,
              "stream appends across submits and orphans only when full");
    }
    REQUIRE(anygm_test_graphics_scene_draw_calls()==18 &&
            anygm_test_graphics_read_pixels()==1,
            "ring wrap adds no draw, fence or target readback");
    gml_gpu_context_lost(gpu);
    REQUIRE(gml_gpu_context_reset(gpu,&context),"stream context recreated after loss");
    gml_gpu_device_services(gpu,&services);
    REQUIRE(services.upload(services.context,&upload),"immutable source recreated after loss");
    result=services.submit(services.context,&batch);
    stream=anygm_test_graphics_stream();
    REQUIRE(result.status==GML_RENDER_EXEC_ACCEPTED && stream.allocations==3u &&
            stream.updates==19u && stream.offset==0u && stream.errors==0u,
            "replacement context has fresh storage and no stale stream cursor");
  }
  free(operations);
  gml_gpu_destroy(gpu,1);
  return 1;
}

/* Call/state proof only. Composed pixel qualification needs a real device. */
static int scene_hardware_filtered_batch_case(unsigned kind){
  enum { QUADS=2049 };
  int unit_font=kind==14u;
  int gui_background=kind==13u;
  int application_sprite=kind==11u;
  int leading_surface=kind==12u;
  int integer_surface=kind==10u;
  int additive=kind==1u,combined=kind==2u,surface=(kind>=3u && kind<=5u) || integer_surface || leading_surface;
  int nearest=kind==4u || kind==5u || integer_surface,screen_stretch=(kind>=6u && kind<=9u) || application_sprite || unit_font;
  int screen_sprite=kind==7u || kind==8u || kind==9u || application_sprite || unit_font;
  const uint8_t pixels[64]={11,22,33,255,44,55,66,100,77,88,99,0,111,122,133,200};
  for(unsigned api=0;api<2u;api++)
  for(unsigned excluded=0;excluded<(unit_font?12u:gui_background?9u:application_sprite?12u:leading_surface?9u:integer_surface?9u:kind==9u?11u:nearest || screen_stretch?8u:(combined || surface)?7u:4u);excluded++){
    GmlGpuContext context=fake_context(api?GML_GPU_API_OPENGLES3:GML_GPU_API_OPENGL_CORE);
    GmlRenderImageRef source={kind==5u || leading_surface?GML_RENDER_RESOURCE_APPLICATION:surface?GML_RENDER_RESOURCE_SURFACE:
      GML_RENDER_RESOURCE_ATLAS,91u,1u,1u};
    GmlRenderDrawOp *ops=(GmlRenderDrawOp*)calloc(QUADS+1u,sizeof *ops);
    GmlRenderDrawStateSnapshot state={0};
    GmlRenderDeviceServices device={0};
    GmlRenderImageUpload upload={0};
    GmlRenderExecutionResult result;
    unsigned factors[4]={0};
    GmlGpu *gpu;
    REQUIRE(ops,"hardware batch allocated");
    anygm_test_graphics_reset();
    if(excluded==1u) anygm_test_graphics_withhold("glBlendFuncSeparate");
    if(excluded==2u) anygm_test_graphics_withhold("glBlendEquationSeparate");
    gpu=gml_gpu_create();
    REQUIRE(gpu && gml_gpu_context_reset(gpu,&context),"hardware batch context adopted");
    gml_gpu_device_services(gpu,&device);
    upload.image=source;upload.storage_width=upload.storage_height=kind==9u?4u:2u;
    upload.region=(GmlRenderRect){0,0,upload.storage_width,upload.storage_height};upload.pixel_format=surface?
      GML_RENDER_PIXEL_ARGB8888:GML_RENDER_PIXEL_RGBA8888;
    upload.bytes=pixels;upload.byte_capacity=sizeof pixels;upload.stride_bytes=upload.storage_width*4u;
    REQUIRE(device.upload(device.context,&upload),"hardware batch atlas uploaded");
    state.write_mask=15u;state.alpha=255u;state.colour=0xFFFFFFFFu;state.sampler_count=1u;
    state.samplers[0].filter=nearest?GML_RENDER_FILTER_NEAREST:GML_RENDER_FILTER_HARDWARE_BILINEAR;
    state.samplers[0].edge_rule=GML_RENDER_EDGE_CLAMP_SUBIMAGE;
    state.samplers[0].source_region=upload.region;
    if(unit_font){
      state.samplers[0].quantisation=GML_RENDER_QUANTISE_HARDWARE_SPRITE_FIRSTGEN;
      state.samplers[0].logical_margin_mask=GML_RENDER_MARGIN_UNIT_TEXEL;
    }
    if(kind==9u){
      state.samplers[0].source_region=(GmlRenderRect){1,1,2u,2u};
      state.samplers[0].edge_rule=GML_RENDER_EDGE_LOGICAL_TPAG;
      state.samplers[0].logical_margin_mask=15u;
    }
    if(combined) state.samplers[0].quantisation=GML_RENDER_QUANTISE_HARDWARE_COMBINED;
    if(leading_surface) state.samplers[0].quantisation=GML_RENDER_QUANTISE_HARDWARE_SURFACE_LEADING;
    state.rgb_source_factor=state.rgb_destination_factor=GML_RENDER_BLEND_ONE;
    if(combined) state.rgb_source_factor=GML_RENDER_BLEND_SOURCE_ALPHA;
    if(!additive) state.rgb_destination_factor=GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA;
    state.alpha_source_factor=GML_RENDER_BLEND_SOURCE_ALPHA;
    if(surface || screen_stretch || gui_background){
      state.rgb_source_factor=GML_RENDER_BLEND_SOURCE_ALPHA;
      state.alpha_source_factor=gui_background?GML_RENDER_BLEND_SOURCE_ALPHA:GML_RENDER_BLEND_ONE;
      state.coverage_policy=GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA;
    }
    state.alpha_destination_factor=GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA;
    state.rgb_equation=state.alpha_equation=GML_RENDER_BLEND_ADD;
    for(unsigned index=0;index<QUADS+1u;index++){
      GmlRenderDrawOp *op=&ops[index];
      op->sequence=index+1u;
      op->target.image=(GmlRenderImageRef){application_sprite?GML_RENDER_RESOURCE_APPLICATION:gui_background || screen_stretch || integer_surface?GML_RENDER_RESOURCE_SCREEN:combined?GML_RENDER_RESOURCE_APPLICATION:
        GML_RENDER_RESOURCE_SURFACE,92u,1u,index+1u};
      op->target.storage_version=1u;op->target.width=op->target.height=64u;
      op->target.pixel_format=GML_RENDER_PIXEL_ARGB8888;
      op->target.region=(GmlRenderRect){0,0,64u,64u};
      op->opcode=index?GML_RENDER_OP_TEXTURED_QUAD:GML_RENDER_OP_CLEAR;
      if(!index){op->payload.clear_colour=0x40123456u;continue;}
      op->reconstruction_mode=GML_RENDER_DEVICE_CHECKPOINT;
      op->scene_family=kind==8u || unit_font?GML_RENDER_SCENE_TEXT:surface?GML_RENDER_SCENE_SURFACE:
        additive || screen_stretch?GML_RENDER_SCENE_SPRITE:GML_RENDER_SCENE_BACKGROUND;
      op->sources=&source;op->source_count=1u;
      GmlRenderQuad *quad=&op->payload.quad;
      quad->shape=GML_RENDER_QUAD_AXIS_ALIGNED;quad->sampling=state.samplers[0];
      quad->filtered_draw_alpha=additive || surface?.7:1.0;
      for(unsigned corner=0;corner<4u;corner++)
        quad->corner_colour[corner]=combined || screen_stretch?0xFFFFFFFFu:0xB3123456u;
      quad->x[1]=quad->x[2]=combined || nearest?2.0:8.0;
      quad->y[2]=quad->y[3]=combined || nearest?2.0:8.0;
      quad->u[1]=quad->u[2]=quad->v[2]=quad->v[3]=1.0;
      if(gui_background){
        quad->filtered_draw_alpha=.7;
        quad->x[0]=quad->x[3]=-2.0;
        quad->u[0]=quad->u[3]=-.25;
        quad->u[1]=quad->u[2]=1.25;
      }
      if(leading_surface){
        quad->filtered_draw_alpha=1.0;
        for(unsigned corner=0;corner<4u;corner++){
          quad->corner_colour[corner]=0xFFFFFFFFu;
          quad->u[corner]+=.5/upload.storage_width-.5/8.0;
          quad->v[corner]+=.5/upload.storage_height-.5/8.0;
        }
      }
      if(integer_surface){
        quad->filtered_draw_alpha=1.0;
        quad->x[1]=quad->x[2]=8.0;quad->y[2]=quad->y[3]=6.0;
        for(unsigned corner=0;corner<4u;corner++) quad->corner_colour[corner]=0xFFFFFFFFu;
      }
      if(screen_sprite){
        quad->filtered_draw_alpha=.7;
        quad->inverse_x[0]=quad->inverse_y[1]=.6;
        quad->x[1]=quad->x[2]=quad->y[2]=quad->y[3]=1.0;
        quad->u[1]=quad->u[2]=quad->v[2]=quad->v[3]=1.0/1.2;
        for(unsigned corner=0;corner<4u;corner++) quad->corner_colour[corner]=0xB3123456u;
      }
      if(unit_font){
        quad->inverse_x[0]=quad->inverse_y[1]=1.0;
        quad->x[1]=quad->x[2]=quad->y[2]=quad->y[3]=2.0;
        quad->u[1]=quad->u[2]=quad->v[2]=quad->v[3]=1.0;
      }
      if(kind==9u){
        quad->u[0]=quad->u[3]=quad->v[0]=quad->v[1]=.25;
        quad->u[1]=quad->u[2]=quad->v[2]=quad->v[3]=(1.0+1.0/.6)/4.0;
      }
    }
    if(excluded==3u) ops[QUADS].payload.quad.filtered_draw_alpha=NAN;
    if(excluded==4u){
      if(surface || screen_stretch) ops[QUADS].scene_family=GML_RENDER_SCENE_BACKGROUND;
      else for(unsigned index=0;index<QUADS+1u;index++)
        ops[index].target.image.resource_class=GML_RENDER_RESOURCE_SURFACE;
    }
    if(excluded==5u) ops[QUADS].payload.quad.corner_colour[0]=0xFFFEFFFFu;
    if(excluded==6u) ops[QUADS].payload.quad.u[1]=.9;
    if(excluded==7u) ops[QUADS].payload.quad.x[1]=ops[QUADS].payload.quad.x[2]=unit_font?3.0:screen_stretch?2.0:3.0;
    if(gui_background && excluded==7u)
      ops[QUADS].payload.quad.x[1]=ops[QUADS].payload.quad.x[2]=-2.0;
    if(integer_surface && excluded==8u)
      for(unsigned index=0;index<QUADS+1u;index++)
        ops[index].target.image.resource_class=GML_RENDER_RESOURCE_APPLICATION;
    if(application_sprite){
      if(excluded==8u) for(unsigned index=0;index<QUADS+1u;index++)
        ops[index].target.image.resource_class=GML_RENDER_RESOURCE_SURFACE;
      if(excluded==9u) ops[QUADS].scene_family=GML_RENDER_SCENE_TEXT;
      if(excluded==10u) state.coverage_policy=0u;
      if(excluded==11u) state.write_mask=7u;
    }else if(unit_font){
      if(excluded==8u) ops[QUADS].payload.quad.sampling.logical_margin_mask=17u;
      if(excluded==9u) ops[QUADS].payload.quad.inverse_anchor[0]=.25;
      if(excluded==10u) ops[QUADS].scene_family=GML_RENDER_SCENE_SPRITE;
      if(excluded==11u) ops[QUADS].payload.quad.inverse_x[0]=.5;
    }else{
      if(excluded==8u && !integer_surface) ops[QUADS].payload.quad.sampling.logical_margin_mask=16u;
      if(excluded==9u) ops[QUADS].payload.quad.sampling.source_region.x=0;
      if(excluded==10u) ops[QUADS].scene_family=GML_RENDER_SCENE_TEXT;
    }
    GmlRenderDrawBatch batch={ops,QUADS+1u,&state,1u};
    result=device.submit(device.context,&batch);
    if(excluded){
      REQUIRE(result.status==GML_RENDER_EXEC_UNSUPPORTED && !result.accepted_prefix &&
              !anygm_test_graphics_clear_calls() && !anygm_test_graphics_scene_draw_calls(),
              "missing blend service or invalid hardware policy rejects before the clear prefix");
    }else{
      anygm_test_graphics_blend_factors(factors);
      REQUIRE(result.status==GML_RENDER_EXEC_ACCEPTED && result.accepted_prefix==QUADS+1u,
              "ordered overlapping filtered quads accepted");
      REQUIRE(anygm_test_graphics_scene_draw_calls()==(surface || screen_stretch || gui_background?4:2) &&
              anygm_test_graphics_scene_vertices()==QUADS*(surface || screen_stretch || gui_background?12:6),
              "only the declared quad capacity cuts compatible overlaps");
      REQUIRE(anygm_test_graphics_scene_filter()==(nearest?GL_NEAREST:GL_LINEAR) &&
              factors[0]==(combined || surface || screen_stretch || gui_background?GL_SRC_ALPHA:GL_ONE) &&
              factors[1]==(additive?GL_ONE:GL_ONE_MINUS_SRC_ALPHA) &&
              factors[2]==(surface || screen_stretch?GL_ONE:GL_SRC_ALPHA) &&
              factors[3]==GL_ONE_MINUS_SRC_ALPHA,
              "hardware filtering and independent alpha factors are configured");
      REQUIRE(!anygm_test_graphics_scene_copies() && !anygm_test_graphics_read_pixels(),
              "ordinary overlap needs no destination snapshots or CPU readbacks");
    }
    gml_gpu_destroy(gpu,1);free(ops);
  }
  return 1;
}

static int scene_source_free_alpha_batch_case(unsigned kind){
  int procedural_line=kind==GML_RENDER_PRIMITIVE_PARTICLE_LINE;
  int inverse=kind==GML_RENDER_PRIMITIVE_CLASSIC_INVERSE_RECT;
  int studio=kind==GML_RENDER_PRIMITIVE_STUDIO_ALPHA_RECT;
  int rectangle=kind==GML_RENDER_PRIMITIVE_CLASSIC_ALPHA_RECT || inverse || studio;
  for(unsigned variant=0;variant<(studio || procedural_line?10u:inverse?9u:8u);variant++){
    enum { LINES=2049 };
    GmlRenderDrawOp *ops=(GmlRenderDrawOp*)calloc(LINES+1u,sizeof *ops);
    GmlGpuContext context=fake_context(GML_GPU_API_OPENGLES3);
    GmlRenderDeviceServices device={0};
    unsigned factors[4]={0};
    anygm_test_graphics_reset();
    if(variant==1u) anygm_test_graphics_withhold("glBlendFuncSeparate");
    if(variant==7u) anygm_test_graphics_withhold("glBlendEquationSeparate");
    GmlGpu *gpu=gml_gpu_create();
    REQUIRE(ops && gpu && gml_gpu_context_reset(gpu,&context),"particle context created");
    gml_gpu_device_services(gpu,&device);
    ops[0].opcode=GML_RENDER_OP_CLEAR;
    ops[0].target=(GmlRenderTargetRef){
      .image={rectangle
                ?GML_RENDER_RESOURCE_SCREEN:GML_RENDER_RESOURCE_APPLICATION,141u,1u,2u},.storage_version=1u,
      .width=96u,.height=48u,.pixel_format=GML_RENDER_PIXEL_ARGB8888,
      .region={0,0,96u,48u}};
    ops[0].payload.clear_colour=rectangle
      ?0xFF273859u:0x49273859u;
    for(unsigned line=0;line<LINES;line++){
      ops[line+1u]=ops[0];
      ops[line+1u].opcode=GML_RENDER_OP_COLOURED_PRIMITIVE;
      ops[line+1u].scene_family=GML_RENDER_SCENE_PRIMITIVE;
      ops[line+1u].reconstruction_mode=GML_RENDER_DEVICE_CHECKPOINT;
      ops[line+1u].target.image.content_version=3u+line;
      ops[line+1u].payload.primitive=(GmlRenderPrimitiveRect){
        .kind=kind,.region={0,0,96u,kind==GML_RENDER_PRIMITIVE_PARTICLE_DISC?1u:48u},
        .colour=0xFF7193B5u,.centre_x=48.25,.centre_y=23.75,
        .scale_x=1.25,.scale_y=0.5,.angle=17,.alpha=inverse?1.0:0.375,
        .interpolate=kind==GML_RENDER_PRIMITIVE_PARTICLE_LINE?line%2u:0u,.alpha_blend=1u};
    }
    if(variant==2u) ops[LINES].payload.primitive.alpha=NAN;
    if(variant==3u) ops[LINES].payload.primitive.kind=9u;
    if(variant==4u) ops[LINES].reconstruction_mode=GML_RENDER_REPLAY_EXACT;
    if(variant==5u) ops[LINES].payload.primitive.region.width=97u;
    if(variant==6u) ops[LINES].payload.primitive.alpha_blend=0u;
    if(studio && variant==8u) ops[LINES].payload.primitive.alpha=1.0/512.0;
    if(studio && variant==9u)
      for(unsigned index=0;index<LINES+1u;index++)
        ops[index].target.image.resource_class=GML_RENDER_RESOURCE_SURFACE;
    if(inverse && variant==8u){
      ops[2].payload.primitive.kind=GML_RENDER_PRIMITIVE_CLASSIC_ALPHA_RECT;
      ops[2].payload.primitive.alpha=0.375;
    }
    unsigned count=inverse && variant==8u?4u:LINES+1u;
    if(procedural_line && variant>=8u){
      count=2u;
      if(variant==8u){
        for(unsigned index=0;index<count;index++){
          ops[index].target.width=ops[index].target.height=4096u;
          ops[index].target.region=(GmlRenderRect){0,0,4096u,4096u};
        }
        ops[1].payload.primitive.region=(GmlRenderRect){0,0,4096u,4096u};
        ops[1].payload.primitive.centre_x=ops[1].payload.primitive.centre_y=2048.0;
        ops[1].payload.primitive.scale_x=ops[1].payload.primitive.scale_y=128.0;
        ops[1].payload.primitive.angle=43.0;
        anygm_test_graphics_fail_scene_draw_at(1u);
      }else ops[1].payload.primitive.centre_x=ops[1].payload.primitive.centre_y=-1024.0;
    }
    GmlRenderDrawBatch batch={ops,count,NULL,0u};
    GmlRenderExecutionResult result=device.submit(device.context,&batch);
    if(procedural_line && variant==8u){
      REQUIRE(result.status==GML_RENDER_EXEC_RECOVERABLE_ERROR && result.accepted_prefix==1u &&
              result.uncertain_target_count==1u && anygm_test_graphics_clear_calls()==1 &&
              anygm_test_graphics_scene_draw_calls()==1,
              "failed mid-line capacity flush fences partial rows without accepting the logical draw");
      anygm_test_graphics_fail_scene_draw_at(0u);
    }else if(procedural_line && variant==9u){
      REQUIRE(result.status==GML_RENDER_EXEC_ACCEPTED && result.accepted_prefix==count &&
              anygm_test_graphics_clear_calls()==1 && !anygm_test_graphics_scene_draw_calls(),
              "empty line completes its logical operation without inventing device vertices");
    }else if(variant && !(inverse && variant==8u) && (variant!=6u || rectangle)){
      REQUIRE(result.status==GML_RENDER_EXEC_UNSUPPORTED && !result.accepted_prefix &&
              !anygm_test_graphics_clear_calls() && !anygm_test_graphics_scene_draw_calls(),
              "particle refusal is transactional before its clear prefix");
    }else{
      anygm_test_graphics_blend_factors(factors);
      REQUIRE(result.status==GML_RENDER_EXEC_ACCEPTED && result.accepted_prefix==count,
              "source-free particle batch accepted");
      unsigned expected_quads=count-1u,expected_draws=studio?4u:variant==8u?3u:2u;
      if(procedural_line){
        unsigned queued=0u,previous_blend=1u;
        expected_quads=expected_draws=0u;
        for(unsigned index=1;index<count;index++){
          const GmlRenderPrimitiveRect *line=&ops[index].payload.primitive;
          if(queued && previous_blend!=line->alpha_blend){expected_draws++;queued=0u;}
          previous_blend=line->alpha_blend;
          double radians=line->angle*3.14159265358979323846/180.0;
          double cosine=cos(radians),sine=sin(radians);
          for(unsigned row=0;row<line->region.height;row++){
            GmlRenderRect region={0};
            if(!gml_render_particle_line_row_region(line,cosine,sine,line->region.y+(int)row,&region)) continue;
            while(row+1u<line->region.height){
              GmlRenderRect next={0};
              if(!gml_render_particle_line_row_region(line,cosine,sine,line->region.y+(int)row+1,&next) ||
                 next.x!=region.x || next.width!=region.width) break;
              row++;
            }
            if(queued==2048u){expected_draws++;queued=0u;}
            queued++;expected_quads++;
          }
        }
        if(queued) expected_draws++;
      }
      REQUIRE(anygm_test_graphics_scene_draw_calls()==(int)expected_draws &&
              anygm_test_graphics_scene_vertices()==(int)expected_quads*6*(studio?2:1),
              "compatible primitives batch to capacity and split at a changed blend rule");
      REQUIRE(factors[0]==(inverse?GL_ZERO:GL_SRC_ALPHA) &&
              factors[1]==(inverse?GL_ONE_MINUS_SRC_COLOR:GL_ONE_MINUS_SRC_ALPHA) &&
              factors[2]==GL_ONE && factors[3]==(inverse?GL_ZERO:GL_ONE_MINUS_SRC_ALPHA),
              "particle source-over preserves the separate destination alpha rule");
      REQUIRE(!anygm_test_graphics_scene_copies() && !anygm_test_graphics_read_pixels(),
              "particle batch needs no target snapshot or ordinary readback");
    }
    gml_gpu_destroy(gpu,1); free(ops);
  }
  return 1;
}

static int scene_particle_line_batch_case(void){
  return scene_source_free_alpha_batch_case(GML_RENDER_PRIMITIVE_PARTICLE_LINE);
}

static int scene_particle_disc_batch_case(void){
  return scene_source_free_alpha_batch_case(GML_RENDER_PRIMITIVE_PARTICLE_DISC);
}

static int scene_classic_alpha_rect_batch_case(void){
  return scene_source_free_alpha_batch_case(GML_RENDER_PRIMITIVE_CLASSIC_ALPHA_RECT);
}

static int scene_studio_alpha_rect_batch_case(void){
  return scene_source_free_alpha_batch_case(GML_RENDER_PRIMITIVE_STUDIO_ALPHA_RECT);
}

static int scene_classic_inverse_rect_batch_case(void){
  return scene_source_free_alpha_batch_case(GML_RENDER_PRIMITIVE_CLASSIC_INVERSE_RECT);
}

static int scene_rectangle_function_batch_case(void){
  enum { QUADS=2049 };
  static const unsigned expected_factors[12][4]={
    {0,0,0,0},{GL_SRC_ALPHA,GL_ONE,GL_ONE,GL_ONE},
    {GL_ZERO,GL_ONE_MINUS_SRC_COLOR,GL_ZERO,GL_ONE_MINUS_SRC_ALPHA},{0,0,0,0},
    {GL_SRC_ALPHA,GL_ONE_MINUS_SRC_COLOR,GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA},
    {GL_SRC_COLOR,GL_ONE,GL_SRC_ALPHA,GL_ONE},{0,0,0,0},
    {GL_ONE_MINUS_DST_ALPHA,GL_DST_ALPHA,GL_ONE_MINUS_DST_ALPHA,GL_DST_ALPHA},
    {GL_ONE_MINUS_DST_COLOR,GL_ZERO,GL_ONE_MINUS_DST_ALPHA,GL_ZERO},
    {GL_DST_COLOR,GL_SRC_COLOR,GL_DST_ALPHA,GL_SRC_ALPHA},
    {GL_DST_COLOR,GL_DST_COLOR,GL_DST_ALPHA,GL_DST_ALPHA},
    {GL_DST_ALPHA,GL_DST_COLOR,GL_DST_ALPHA,GL_DST_ALPHA}
  };
  for(unsigned api=GML_GPU_API_OPENGL_CORE;api<=GML_GPU_API_OPENGLES3;api++)
  for(unsigned preset=0;preset<12u;preset++)
  for(unsigned preserve_alpha=0;preserve_alpha<2u;preserve_alpha++)
  for(unsigned blend=0;blend<3u;blend++)
  for(unsigned outline=0;outline<(preset==1u?2u:1u);outline++){
    if(preset==3u || preset==6u) continue;
    GmlRenderDrawOp *ops=calloc(QUADS+1u,sizeof *ops);
    GmlGpuContext context=fake_context(api);
    GmlRenderDeviceServices device={0};
    unsigned factors[4]={0};
    anygm_test_graphics_reset();
    GmlGpu *gpu=gml_gpu_create();
    REQUIRE(ops && gpu && gml_gpu_context_reset(gpu,&context),"rectangle context adopted");
    gml_gpu_device_services(gpu,&device);
    ops[0].opcode=GML_RENDER_OP_CLEAR;
    ops[0].target=(GmlRenderTargetRef){
      .image={preserve_alpha?GML_RENDER_RESOURCE_SURFACE:GML_RENDER_RESOURCE_SCREEN,141u,1u,2u},
      .storage_version=1u,.width=96u,.height=48u,
      .pixel_format=GML_RENDER_PIXEL_ARGB8888,.region={0,0,96u,48u}};
    ops[0].payload.clear_colour=0x40273859u;
    for(unsigned q=0;q<QUADS;q++){
      ops[q+1u]=ops[0];ops[q+1u].opcode=GML_RENDER_OP_COLOURED_PRIMITIVE;
      ops[q+1u].scene_family=GML_RENDER_SCENE_PRIMITIVE;
      ops[q+1u].reconstruction_mode=GML_RENDER_DEVICE_CHECKPOINT;
      ops[q+1u].target.image.content_version=3u+q;
      ops[q+1u].payload.primitive=(GmlRenderPrimitiveRect){
        .kind=GML_RENDER_PRIMITIVE_RECTANGLE,.region={0,0,96u,48u},
        .authored_region={-2,-1,100u,50u},.colour=0xFF7193B5u,
        .corner_colour={0xFF7193B5u,0xFF1375B9u,0xFFFCEB05u,0xFF17E305u},
        .alpha=blend==2u?1.0:0.375,.alpha_blend=blend!=0u,
        .rectangle_flags=GML_RENDER_RECTANGLE_GRADIENT|
          (preserve_alpha?GML_RENDER_RECTANGLE_TARGET_ALPHA:0u)};
      if(preset){
        for(unsigned c=0;c<4u;c++) ops[q+1u].payload.primitive.corner_colour[c]=0xFF7193B5u;
        ops[q+1u].payload.primitive.rectangle_flags=
          (preserve_alpha?GML_RENDER_RECTANGLE_TARGET_ALPHA:0u)|
          (outline?GML_RENDER_RECTANGLE_OUTLINE:0u)|
          (preset<<GML_RENDER_RECTANGLE_BLEND_SHIFT);
      }
    }
    GmlRenderDrawBatch batch={ops,QUADS+1u,NULL,0u};
    GmlRenderExecutionResult result=device.submit(device.context,&batch);
    int passes=blend==1u && !preserve_alpha &&
      (!preset || preset==1u || preset==4u)?2:1;
    REQUIRE(result.status==GML_RENDER_EXEC_ACCEPTED && result.accepted_prefix==QUADS+1u,
            "rectangle matrix packets accepted");
    REQUIRE(anygm_test_graphics_scene_draw_calls()==2*passes &&
            anygm_test_graphics_scene_vertices()==QUADS*6*passes,
            "only quad capacity splits compatible source-free rectangles");
    if(blend==1u && !preset){
      anygm_test_graphics_blend_factors(factors);
      REQUIRE(factors[0]==GL_SRC_ALPHA && factors[1]==GL_ONE_MINUS_SRC_ALPHA &&
              factors[2]==GL_SRC_ALPHA && factors[3]==GL_ONE_MINUS_SRC_ALPHA,
              "numbered rectangle alpha uses its separate source-alpha factor");
    }
    if(preset && blend){
      anygm_test_graphics_blend_factors(factors);
      unsigned source=preset==1u && !outline?GL_ONE:expected_factors[preset][0];
      REQUIRE(factors[0]==source && factors[1]==expected_factors[preset][1] &&
              factors[2]==(preserve_alpha?expected_factors[preset][2]:GL_ONE) &&
              factors[3]==(preserve_alpha?expected_factors[preset][3]:GL_ZERO),
              "rectangle presets set every separate factor rather than inherit host state");
    }
    REQUIRE(!anygm_test_graphics_scene_copies() && !anygm_test_graphics_read_pixels(),
            "rectangle overlaps need no snapshots or ordinary reads");
    ops[QUADS].payload.primitive.authored_region.width=65537u;
    anygm_test_graphics_reset();
    result=device.submit(device.context,&batch);
    REQUIRE(result.status==GML_RENDER_EXEC_UNSUPPORTED && !result.accepted_prefix &&
            !anygm_test_graphics_clear_calls() && !anygm_test_graphics_scene_draw_calls(),
            "malformed rectangle refuses before the clear prefix");
    gml_gpu_destroy(gpu,1);free(ops);
  }
  return 1;
}

static int scene_hardware_additive_batch_case(void){
  return scene_hardware_filtered_batch_case(1);
}
static int scene_hardware_background_batch_case(void){
  return scene_hardware_filtered_batch_case(0);
}
static int scene_hardware_combined_batch_case(void){
  return scene_hardware_filtered_batch_case(2);
}
static int scene_hardware_surface_batch_case(void){
  return scene_hardware_filtered_batch_case(3);
}
static int scene_nearest_surface_batch_case(void){
  return scene_hardware_filtered_batch_case(4);
}
static int scene_nearest_application_batch_case(void){
  return scene_hardware_filtered_batch_case(5) && scene_hardware_filtered_batch_case(12);
}
/* Typed admission and capacity, not shader pixels; the public paired generator
 * owns sampling, cutoff, masks and complete RGBA comparisons. */
static int scene_nearest_surface_function_case(void){
  enum { QUADS=2049 };
  const uint32_t pixels[4]={0x001267B9u,0x552679B3u,0xAAC39526u,0xFF7193C5u};
  for(unsigned api=0;api<2u;api++) for(unsigned target_kind=0;target_kind<3u;target_kind++)
  for(unsigned source_kind=0;source_kind<3u;source_kind++)
  for(unsigned selection=0;selection<3u;selection++)
  for(unsigned invalid=0;invalid<(selection?20u:12u);invalid++){
    anygm_test_graphics_reset();
    GmlGpuContext context=fake_context(api?GML_GPU_API_OPENGLES3:GML_GPU_API_OPENGL_CORE);
    GmlGpu *gpu=gml_gpu_create();GmlRenderDeviceServices device={0};
    GmlRenderDrawOp *ops=calloc(QUADS+1u,sizeof *ops);
    GmlRenderImageRef source={source_kind==2u?GML_RENDER_RESOURCE_SCREEN:
      source_kind?GML_RENDER_RESOURCE_APPLICATION:GML_RENDER_RESOURCE_SURFACE,
      source_kind==2u?162u:161u,1u,1u};
    GmlRenderImageUpload upload={.image=source,.storage_width=2u,.storage_height=2u,
      .region={0,0,2u,2u},.pixel_format=GML_RENDER_PIXEL_ARGB8888,
      .stride_bytes=8u,.byte_capacity=sizeof pixels,.bytes=pixels};
    GmlRenderDrawStateSnapshot state={0};
    REQUIRE(gpu && ops && gml_gpu_context_reset(gpu,&context),"nearest surface function adopts context");
    gml_gpu_device_services(gpu,&device);
    REQUIRE(device.upload(device.context,&upload),"nearest surface function uploads source");
    state.write_mask=7u;state.alpha=255u;state.colour=0xFFFFFFFFu;state.sampler_count=1u;
    state.alpha_test_enabled=1u;state.alpha_test_reference=127u;
    state.rgb_source_factor=GML_RENDER_BLEND_SOURCE_ALPHA;
    state.rgb_destination_factor=state.alpha_destination_factor=GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA;
    state.alpha_source_factor=GML_RENDER_BLEND_ONE;
    state.rgb_equation=state.alpha_equation=GML_RENDER_BLEND_ADD;
    state.samplers[0]=(GmlRenderSamplingPolicy){GML_RENDER_FILTER_NEAREST,
      GML_RENDER_EDGE_CLAMP_SUBIMAGE,{0,0,2u,2u},GML_RENDER_QUANTISE_SURFACE_NEAREST_FUNCTION,0u};
    for(unsigned index=0;index<QUADS+1u;index++){
      GmlRenderDrawOp *op=&ops[index];
      op->sequence=index+1u;
      op->target=(GmlRenderTargetRef){.image={target_kind==0u?GML_RENDER_RESOURCE_SCREEN:
        target_kind==1u?GML_RENDER_RESOURCE_APPLICATION:GML_RENDER_RESOURCE_SURFACE,
        162u,source_kind==2u && target_kind==0u?2u:1u,index+1u},
        .storage_version=1u,.width=8u,.height=8u,.pixel_format=GML_RENDER_PIXEL_ARGB8888,
        .region={0,0,8u,8u}};
      if(!index){op->opcode=GML_RENDER_OP_CLEAR;op->payload.clear_colour=0x78273859u;continue;}
      op->opcode=GML_RENDER_OP_TEXTURED_QUAD;op->scene_family=GML_RENDER_SCENE_SURFACE;
      op->reconstruction_mode=GML_RENDER_DEVICE_CHECKPOINT;op->sources=&source;op->source_count=1u;
      GmlRenderQuad *q=&op->payload.quad;
      q->shape=GML_RENDER_QUAD_AXIS_ALIGNED;q->sampling=state.samplers[0];q->filtered_draw_alpha=.4;
      q->x[0]=q->x[3]=-1;q->x[1]=q->x[2]=2;q->y[0]=q->y[1]=1;q->y[2]=q->y[3]=4;
      q->inverse_anchor[0]=-1;q->inverse_anchor[1]=1;q->inverse_x[0]=-3;q->inverse_y[1]=3;
      if(selection){
        q->span_x[2]=selection==1u?-1.0:.25;q->span_x[3]=0.0;
        q->span_y[1]=selection==1u?4.0:1.5;q->span_y[2]=2.0;
        q->span_y[3]=selection==1u?2.0:1.0;
      }
      for(unsigned corner=0;corner<4u;corner++){
        q->u[corner]=corner==0u || corner==3u?0.0:1.0;q->v[corner]=corner<2u?0.0:1.0;
        q->corner_colour[corner]=0x663470D0u;
      }
    }
    GmlRenderQuad *last=&ops[QUADS].payload.quad;
    if(invalid==1u) last->inverse_x[0]=NAN;
    if(invalid==2u) last->inverse_anchor[0]=-.5;
    if(invalid==3u) last->inverse_y[1]=4;
    if(invalid==4u) last->span_x[0]=4;
    if(invalid==5u) last->span_y[0]=3;
    if(invalid==6u) last->filtered_draw_alpha=.5;
    if(invalid==7u) last->corner_colour[1]^=1u;
    if(invalid==8u) last->u[1]=.9;
    if(invalid==9u) ops[QUADS].scene_family=GML_RENDER_SCENE_SPRITE;
    if(invalid==10u) ops[QUADS].reconstruction_mode=GML_RENDER_REPLAY_EXACT;
    if(invalid==11u) last->span_active=1u;
    if(invalid==12u) last->span_y[3]=0.0;
    if(invalid==13u) last->span_y[3]=4.0;
    if(invalid==14u) last->span_x[2]=NAN;
    if(invalid==15u) last->span_y[1]=0.0;
    if(invalid==16u){last->span_y[3]=2.0;last->span_x[2]=.5;}
    if(invalid==17u) last->span_y[2]=16385.0;
    if(invalid==18u) last->span_y[3]=3.0;
    if(invalid==19u) last->span_x[0]=1.0;
    GmlRenderDrawBatch batch={ops,QUADS+1u,&state,1u};
    GmlRenderExecutionResult result=device.submit(device.context,&batch);
    if(invalid){
      REQUIRE(result.status==GML_RENDER_EXEC_UNSUPPORTED && !result.accepted_prefix &&
        !anygm_test_graphics_clear_calls() && !anygm_test_graphics_scene_draw_calls() &&
        !anygm_test_graphics_scene_copies(),"invalid nearest surface suffix refuses before clear and draws");
    }else{
      REQUIRE(result.status==GML_RENDER_EXEC_ACCEPTED && result.accepted_prefix==QUADS+1u &&
        anygm_test_graphics_scene_draw_calls()==2 && anygm_test_graphics_scene_vertices()==QUADS*6 &&
        !anygm_test_graphics_scene_copies() && !anygm_test_graphics_read_pixels(),
        "nearest surface groups at capacity without destination copies or observations");
    }
    gml_gpu_destroy(gpu,1);free(ops);
  }
  return 1;
}
static int scene_rotated_surface_point_case(void){
  enum {QUADS=2049};
  const uint32_t pixels[]={0x001267B9u,0x552679B3u,0xAAC39526u,0xFF7193C5u};
  const unsigned classes[]={GML_RENDER_RESOURCE_SCREEN,GML_RENDER_RESOURCE_APPLICATION,
    GML_RENDER_RESOURCE_SURFACE,GML_RENDER_RESOURCE_SCRATCH};
  for(unsigned api=0;api<2u;api++) for(unsigned target_kind=0;target_kind<3u;target_kind++)
  for(unsigned source_kind=0;source_kind<4u;source_kind++) for(unsigned invalid=0;invalid<17u;invalid++){
    anygm_test_graphics_reset();GmlGpu *gpu=gml_gpu_create();GmlRenderDeviceServices device={0};
    GmlGpuContext context=fake_context(api?GML_GPU_API_OPENGLES3:GML_GPU_API_OPENGL_CORE);
    GmlRenderDrawOp *ops=calloc(QUADS+1u,sizeof *ops);
    GmlRenderImageRef source={classes[source_kind],181u,1u,1u};
    GmlRenderImageUpload upload={.image=source,.storage_width=2u,.storage_height=2u,
      .region={0,0,2u,2u},.pixel_format=GML_RENDER_PIXEL_ARGB8888,
      .stride_bytes=8u,.byte_capacity=sizeof pixels,.bytes=pixels};
    REQUIRE(gpu && ops && gml_gpu_context_reset(gpu,&context),"rotated point context");
    gml_gpu_device_services(gpu,&device);REQUIRE(device.upload(device.context,&upload),"rotated point source");
    GmlRenderDrawStateSnapshot state={.colour=0xFFFFFFFFu,.alpha=255u,.write_mask=15u,
      .coverage_policy=GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA,.sampler_count=1u};
    state.samplers[0]=(GmlRenderSamplingPolicy){GML_RENDER_FILTER_NEAREST,GML_RENDER_EDGE_CLAMP_SUBIMAGE,
      {0,0,2u,2u},GML_RENDER_QUANTISE_SURFACE_ROTATED_POINT,0u};
    for(unsigned index=0;index<QUADS+1u;index++){
      GmlRenderDrawOp *op=&ops[index];op->sequence=index+1u;
      op->target=(GmlRenderTargetRef){.image={classes[target_kind],182u,1u,index+1u},
        .storage_version=1u,.width=8u,.height=8u,.pixel_format=GML_RENDER_PIXEL_ARGB8888,
        .region={0,0,8u,8u}};
      if(!index){op->opcode=GML_RENDER_OP_CLEAR;op->payload.clear_colour=0x78273859u;continue;}
      op->opcode=GML_RENDER_OP_TEXTURED_QUAD;op->scene_family=GML_RENDER_SCENE_SURFACE;
      op->reconstruction_mode=GML_RENDER_DEVICE_CHECKPOINT;op->sources=&source;op->source_count=1u;
      GmlRenderQuad *q=&op->payload.quad;q->shape=GML_RENDER_QUAD_AXIS_ALIGNED;
      q->sampling=state.samplers[0];q->filtered_draw_alpha=.61;q->span_y[0]=1;
      q->x[0]=q->x[3]=1;q->x[1]=q->x[2]=2;q->y[0]=q->y[1]=1;q->y[2]=q->y[3]=2;
      for(unsigned corner=0;corner<4u;corner++){
        q->u[corner]=index&1u?.25:.75;q->v[corner]=.25;q->corner_colour[corner]=0x9CA8C4E0u;
      }
    }
    GmlRenderQuad *last=&ops[QUADS].payload.quad;
    if(invalid==1u) last->sampling.quantisation++;
    if(invalid==2u) last->sampling.filter=GML_RENDER_FILTER_HARDWARE_BILINEAR;
    if(invalid==3u) for(unsigned corner=0;corner<4u;corner++) last->u[corner]=.125;
    if(invalid==4u) last->v[0]=NAN;
    if(invalid==5u) last->shape=GML_RENDER_QUAD_GENERAL;
    if(invalid==6u) state.write_mask=0;
    if(invalid==7u) last->span_x[1]=1;
    if(invalid==8u) last->span_y[0]=0;
    if(invalid==9u) last->span_y[1]=1;
    if(invalid==10u) last->y[2]=last->y[3]=3;
    if(invalid==11u) last->sampling.source_region.width=1;
    if(invalid==12u) source.resource_class=GML_RENDER_RESOURCE_ATLAS;
    if(invalid==13u) last->corner_colour[1]^=1;
    if(invalid==14u) last->filtered_draw_alpha=NAN;
    if(invalid==15u) ops[QUADS].source_count=0;
    if(invalid==16u) last->inverse_anchor[0]=1;
    GmlRenderDrawBatch batch={ops,QUADS+1u,&state,1u};
    GmlRenderExecutionResult result=device.submit(device.context,&batch);
    if(invalid){
      REQUIRE(result.status==GML_RENDER_EXEC_UNSUPPORTED && !result.accepted_prefix &&
        !anygm_test_graphics_clear_calls() && !anygm_test_graphics_scene_draw_calls() &&
        !anygm_test_graphics_scene_copies(),"rotated point invalid suffix refuses the entire batch prewrite");
    }else{
      REQUIRE(result.status==GML_RENDER_EXEC_ACCEPTED && result.accepted_prefix==QUADS+1u &&
        anygm_test_graphics_scene_draw_calls()==4 && !anygm_test_graphics_scene_copies() &&
        !anygm_test_graphics_read_pixels(),"rotated point batches at capacity with grouped alpha stamps");
    }
    gml_gpu_destroy(gpu,1);free(ops);
  }
  return 1;
}

static int scene_tiled_surface_packets_case(void){
  enum {QUADS=2049,SW=8,SH=6};
  const unsigned equations[]={GML_RENDER_BLEND_ADD,GML_RENDER_BLEND_FIXED_MAX,
    GML_RENDER_BLEND_FIXED_REVERSE_SUBTRACT,GML_RENDER_BLEND_FIXED_SUBTRACT,GML_RENDER_BLEND_FIXED_MIN};
  uint32_t pixels[SW*SH];for(unsigned p=0;p<SW*SH;p++) pixels[p]=0x803579B1u+p;
  for(unsigned api=0;api<2u;api++) for(unsigned target_kind=0;target_kind<3u;target_kind++)
  for(unsigned kind=0;kind<6u;kind++) for(unsigned equation=0;equation<(kind==2u || kind==3u?5u:1u);equation++)
  for(unsigned invalid=0;invalid<14u;invalid++){
    if(invalid==13u && kind==5u) continue;
    anygm_test_graphics_reset();GmlGpu *gpu=gml_gpu_create();GmlRenderDeviceServices device={0};
    GmlGpuContext context=fake_context(api?GML_GPU_API_OPENGLES3:GML_GPU_API_OPENGL_CORE);
    GmlRenderDrawOp *ops=calloc(QUADS+1u,sizeof *ops);
    GmlRenderImageRef source={GML_RENDER_RESOURCE_APPLICATION,161u,1u,1u};
    GmlRenderImageUpload upload={.image=source,.storage_width=SW,.storage_height=SH,
      .region={0,0,SW,SH},.pixel_format=GML_RENDER_PIXEL_ARGB8888,
      .stride_bytes=SW*4u,.byte_capacity=sizeof pixels,.bytes=pixels};
    REQUIRE(gpu && ops && gml_gpu_context_reset(gpu,&context),"tiled surface packet adopts context");
    gml_gpu_device_services(gpu,&device);REQUIRE(device.upload(device.context,&upload),"tiled source uploads once");
    GmlRenderDrawStateSnapshot state={.write_mask=15u,.alpha=255u,.colour=0xFFFFFFFFu,.sampler_count=1u};
    state.rgb_equation=state.alpha_equation=GML_RENDER_BLEND_ADD;
    state.rgb_source_factor=GML_RENDER_BLEND_SOURCE_ALPHA;
    state.rgb_destination_factor=state.alpha_destination_factor=GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA;
    state.alpha_source_factor=GML_RENDER_BLEND_ONE;
    if(kind==0u){state.rgb_source_factor=GML_RENDER_BLEND_DESTINATION_COLOUR;
      state.rgb_destination_factor=state.alpha_source_factor=GML_RENDER_BLEND_ZERO;
      state.alpha_destination_factor=GML_RENDER_BLEND_ONE;}
    if(kind==1u){state.rgb_destination_factor=GML_RENDER_BLEND_ONE_MINUS_SOURCE_COLOUR;
      state.alpha_source_factor=GML_RENDER_BLEND_SOURCE_ALPHA;}
    if(kind==2u || kind==3u){
      state.rgb_source_factor=state.alpha_source_factor=kind==3u?
        GML_RENDER_BLEND_DESTINATION_ALPHA:GML_RENDER_BLEND_DESTINATION_COLOUR;
      state.rgb_destination_factor=state.alpha_destination_factor=kind==3u?
        GML_RENDER_BLEND_DESTINATION_COLOUR:GML_RENDER_BLEND_SOURCE_COLOUR;
      state.rgb_equation=equations[equation];state.alpha_equation=equations[(equation+2u)%5u];
      state.coverage_policy=target_kind?0u:GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA;
    }
    if(kind>=4u) state.coverage_policy=GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA;
    if(kind==4u){state.rgb_source_factor=state.rgb_destination_factor=0u;
      state.alpha_source_factor=state.alpha_destination_factor=0u;
      state.rgb_equation=state.alpha_equation=0u;}
    state.samplers[0]=(GmlRenderSamplingPolicy){kind>=4u?GML_RENDER_FILTER_HARDWARE_BILINEAR:
      kind==3u?GML_RENDER_FILTER_BOX:GML_RENDER_FILTER_NEAREST,GML_RENDER_EDGE_CLAMP_SUBIMAGE,
      {0,0,SW,SH},kind==5u?GML_RENDER_QUANTISE_SURFACE_TILED_LEADING:
       kind==4u?GML_RENDER_QUANTISE_SURFACE_TILED_FILTERED:
       kind==3u?GML_RENDER_QUANTISE_SURFACE_BOX_FUNCTION:GML_RENDER_QUANTISE_SURFACE_TILED_POINT,0u};
    for(unsigned index=0;index<QUADS+1u;index++){
      GmlRenderDrawOp *op=&ops[index];
      op->target=(GmlRenderTargetRef){.image={target_kind==0u?GML_RENDER_RESOURCE_SCREEN:
        target_kind==1u?GML_RENDER_RESOURCE_APPLICATION:GML_RENDER_RESOURCE_SURFACE,162u,1u,index+1u},
        .storage_version=1u,.width=8u,.height=8u,.pixel_format=GML_RENDER_PIXEL_ARGB8888,.region={0,0,8u,8u}};
      if(!index){op->opcode=GML_RENDER_OP_CLEAR;op->payload.clear_colour=0x78273859u;continue;}
      op->opcode=GML_RENDER_OP_TEXTURED_QUAD;op->scene_family=GML_RENDER_SCENE_SURFACE;
      op->reconstruction_mode=GML_RENDER_DEVICE_CHECKPOINT;op->sources=&source;op->source_count=1u;
      GmlRenderQuad *q=&op->payload.quad;double width=kind>=4u?12:3,height=kind>=4u?9:3;
      q->shape=GML_RENDER_QUAD_AXIS_ALIGNED;q->sampling=state.samplers[0];q->filtered_draw_alpha=kind==5u?1:.4;
      q->x[0]=q->x[3]=-1;q->x[1]=q->x[2]=-1+width;q->y[0]=q->y[1]=1;q->y[2]=q->y[3]=1+height;
      if(kind<4u){q->inverse_anchor[0]=-1;q->inverse_anchor[1]=1;q->inverse_x[0]=-width;q->inverse_y[1]=height;}
      if(kind==0u || kind==2u || kind==3u) q->span_y[0]=4;
      for(unsigned corner=0;corner<4u;corner++){
        q->u[corner]=(corner==0u || corner==3u?0.0:1.0)+(kind==5u?.5/SW-.5/width:0);
        q->v[corner]=(corner<2u?0.0:1.0)+(kind==5u?.5/SH-.5/height:0);
        q->corner_colour[corner]=kind==5u?0xFFFFFFFFu:0x663470D0u;
      }
    }
    GmlRenderDrawOp *last=&ops[QUADS];GmlRenderQuad *q=&last->payload.quad;
    GmlRenderImageRef alias=last->target.image;
    if(invalid==1u) q->filtered_draw_alpha=NAN;
    if(invalid==2u) q->span_active=1u;
    if(invalid==3u) last->source_count=0u;
    if(invalid==4u) last->scene_family=GML_RENDER_SCENE_SPRITE;
    if(invalid==5u) last->reconstruction_mode=GML_RENDER_REPLAY_EXACT;
    if(invalid==6u) q->sampling.quantisation=0u;
    if(invalid==7u) state.samplers[0].quantisation++;
    if(invalid==8u) state.alpha_test_enabled=2u;
    if(invalid==9u) state.rgb_equation=UINT32_MAX;
    if(invalid==10u) q->corner_colour[1]^=1u;
    if(invalid==11u) last->target.pixel_format=GML_RENDER_PIXEL_XRGB8888;
    if(invalid==12u) last->sources=&alias;
    if(invalid==13u){
      state.samplers[0].quantisation=kind>=4u?0u:GML_RENDER_QUANTISE_SURFACE_NEAREST_FUNCTION;
      for(unsigned index=1;index<=QUADS;index++) ops[index].payload.quad.sampling=state.samplers[0];
    }
    GmlRenderDrawBatch batch={ops,QUADS+1u,&state,1u};
    GmlRenderExecutionResult result=device.submit(device.context,&batch);
    if(!invalid && (result.status!=GML_RENDER_EXEC_ACCEPTED || result.accepted_prefix!=QUADS+1u ||
       anygm_test_graphics_scene_draw_calls()!=(kind>=4u?4:2) ||
       anygm_test_graphics_scene_copies() || anygm_test_graphics_read_pixels()))
      fprintf(stderr,"tiled packet: api=%u target=%u kind=%u equation=%u status=%u prefix=%u draws=%d copies=%u reads=%d\n",
        api,target_kind,kind,equation,result.status,result.accepted_prefix,
        anygm_test_graphics_scene_draw_calls(),anygm_test_graphics_scene_copies(),anygm_test_graphics_read_pixels());
    if(invalid) REQUIRE(result.status==GML_RENDER_EXEC_UNSUPPORTED && !result.accepted_prefix &&
      !anygm_test_graphics_clear_calls() && !anygm_test_graphics_scene_draw_calls() &&
      !anygm_test_graphics_scene_copies(),"malformed or legacy tiled packet refuses before the whole prefix");
    else REQUIRE(result.status==GML_RENDER_EXEC_ACCEPTED && result.accepted_prefix==QUADS+1u &&
      anygm_test_graphics_scene_draw_calls()==(kind>=4u?4:2) &&
      !anygm_test_graphics_scene_copies() && !anygm_test_graphics_read_pixels(),
      "typed tiled surface batches at capacity without snapshots or observations");
    gml_gpu_destroy(gpu,1);free(ops);
  }
  return 1;
}

static int scene_integer_screen_surface_batch_case(void){
  return scene_hardware_filtered_batch_case(10);
}
static int scene_gui_filtered_background_batch_case(void){
  return scene_hardware_filtered_batch_case(13);
}
static int scene_hardware_screen_stretch_batch_case(void){
  return scene_hardware_filtered_batch_case(6);
}
/* Typed row admission/atomic refusal only; real paired controls own RGBA. */
static int scene_filtered_affine_row_batch_case(void){
  enum { QUADS=2049 };
  const uint8_t pixels[16]={11,22,33,255,44,55,66,255,77,88,99,255,111,122,133,255};
  for(unsigned api=0;api<2u;api++) for(unsigned owner=0;owner<3u;owner++)
  for(unsigned positive=0;positive<17u;positive++)
  for(unsigned background=0;background<5u;background++)
  for(unsigned mask_index=0;mask_index<(background>=3u?4u:1u);mask_index++)
  for(unsigned invalid=0;invalid<(background>=3u?23u:background?17u:positive?16u:14u);invalid++){
    if(background && !positive) continue;
    if(!background && positive>=4u) continue;
    if(background==2u && positive==3u) continue;
    if(background==1u && positive>=5u) continue;
    if(background<3u && positive>=7u) continue;
    if(background>=3u && (positive==3u || positive==5u || positive==6u)) continue;
    unsigned quads=background>=3u && (positive!=1u || mask_index)?3u:QUADS;
    anygm_test_graphics_reset();
    GmlGpu *gpu=gml_gpu_create();GmlRenderDeviceServices device={0};
    GmlGpuContext context=fake_context(api?GML_GPU_API_OPENGLES3:GML_GPU_API_OPENGL_CORE);
    GmlRenderImageRef source={GML_RENDER_RESOURCE_ATLAS,181u,1u,1u};
    GmlRenderImageUpload upload={.image=source,.storage_width=2u,.storage_height=2u,
      .region={0,0,2u,2u},.pixel_format=GML_RENDER_PIXEL_RGBA8888,
      .stride_bytes=8u,.byte_capacity=sizeof pixels,.bytes=pixels};
    GmlRenderDrawOp *ops=calloc(quads+1u,sizeof *ops);
    GmlRenderDrawStateSnapshot state={0};
    REQUIRE(gpu && ops && gml_gpu_context_reset(gpu,&context),"affine row context adopted");
    gml_gpu_device_services(gpu,&device);
    REQUIRE(device.upload(device.context,&upload),"affine row atlas uploaded");
    state.write_mask=15u;state.alpha=255u;state.colour=0xFFFFFFFFu;state.sampler_count=1u;
    if(background>=3u){
      static const uint32_t masks[]={15u,7u,8u,0u};state.write_mask=masks[mask_index];
    }
    state.samplers[0]=(GmlRenderSamplingPolicy){GML_RENDER_FILTER_HARDWARE_BILINEAR,
      GML_RENDER_EDGE_CLAMP_SUBIMAGE,{0,0,2u,2u},background>=3u?
        (background==3u?GML_RENDER_QUANTISE_INTEGER_TINT_TRUNC:GML_RENDER_QUANTISE_INTEGER_TINT_ROUND):background==2u?
        GML_RENDER_QUANTISE_HARDWARE_SPRITE_FIRSTGEN:GML_RENDER_QUANTISE_HARDWARE_SPRITE_MODERN,0u};
    state.rgb_source_factor=GML_RENDER_BLEND_SOURCE_ALPHA;
    state.rgb_destination_factor=state.alpha_destination_factor=GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA;
    state.alpha_source_factor=GML_RENDER_BLEND_ONE;
    state.rgb_equation=state.alpha_equation=GML_RENDER_BLEND_ADD;
    if(positive){
      state.coverage_policy=GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA;
      state.alpha_test_enabled=1u;state.alpha_test_reference=0u;
      if(positive==2u){
        state.rgb_source_factor=state.rgb_destination_factor=0u;
        state.alpha_source_factor=state.alpha_destination_factor=0u;
        state.rgb_equation=state.alpha_equation=0u;
      }else if(positive==3u){
        state.coverage_policy=0u;
        state.rgb_source_factor=GML_RENDER_BLEND_ONE;
        state.rgb_destination_factor=GML_RENDER_BLEND_ZERO;
        state.alpha_source_factor=GML_RENDER_BLEND_SOURCE_ALPHA;
      }else if(positive>=7u){
        if(positive==7u || positive==16u){
          state.rgb_source_factor=GML_RENDER_BLEND_ONE;
          state.rgb_destination_factor=GML_RENDER_BLEND_ONE;
          state.alpha_source_factor=GML_RENDER_BLEND_SOURCE_ALPHA;
        }else if(positive==8u || positive==9u){
          state.coverage_policy=positive==8u?GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA:0u;
          state.rgb_destination_factor=GML_RENDER_BLEND_ONE_MINUS_SOURCE_COLOUR;
          state.alpha_source_factor=positive==8u?GML_RENDER_BLEND_ONE:GML_RENDER_BLEND_SOURCE_ALPHA;
          state.alpha_destination_factor=positive==8u?GML_RENDER_BLEND_ZERO:GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA;
        }else if(positive==10u || positive==11u){
          state.coverage_policy=positive==11u?GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA:0u;
          state.rgb_source_factor=positive==10u?GML_RENDER_BLEND_ONE_MINUS_DESTINATION_ALPHA:GML_RENDER_BLEND_ONE_MINUS_DESTINATION_COLOUR;
          state.rgb_destination_factor=positive==10u?GML_RENDER_BLEND_DESTINATION_ALPHA:GML_RENDER_BLEND_ZERO;
          state.alpha_source_factor=positive==10u?GML_RENDER_BLEND_ONE_MINUS_DESTINATION_ALPHA:GML_RENDER_BLEND_ONE;
          state.alpha_destination_factor=positive==10u?GML_RENDER_BLEND_DESTINATION_ALPHA:GML_RENDER_BLEND_ZERO;
        }else if(positive==12u || positive==13u){
          state.coverage_policy=positive==13u?GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA:0u;
          state.rgb_source_factor=state.alpha_source_factor=positive==12u?GML_RENDER_BLEND_DESTINATION_COLOUR:GML_RENDER_BLEND_DESTINATION_ALPHA;
          state.rgb_destination_factor=state.alpha_destination_factor=positive==12u?GML_RENDER_BLEND_SOURCE_COLOUR:GML_RENDER_BLEND_DESTINATION_COLOUR;
        }else if(positive!=15u){
          state.coverage_policy=GML_RENDER_COVERAGE_REPLACE_SOURCE_ALPHA;
          state.rgb_source_factor=state.rgb_destination_factor=0u;
          state.alpha_source_factor=state.alpha_destination_factor=0u;
          state.rgb_equation=state.alpha_equation=0u;
        }
      }else if(positive>=4u){
        state.coverage_policy=positive==5u?GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA:0u;
        state.alpha_source_factor=GML_RENDER_BLEND_SOURCE_ALPHA;
        if(positive>=5u){
          state.rgb_source_factor=GML_RENDER_BLEND_ZERO;
          state.rgb_destination_factor=GML_RENDER_BLEND_ONE_MINUS_SOURCE_COLOUR;
        }
      }
    }
    if(background>=3u) state.alpha_test_enabled=0u;
    for(unsigned index=0;index<quads+1u;index++){
      GmlRenderDrawOp *op=&ops[index];op->sequence=index+1u;
      op->target=(GmlRenderTargetRef){.image={owner==0u?GML_RENDER_RESOURCE_SCREEN:
        owner==1u?GML_RENDER_RESOURCE_APPLICATION:GML_RENDER_RESOURCE_SURFACE,182u,1u,index+1u},
        .storage_version=1u,.width=8u,.height=8u,.pixel_format=GML_RENDER_PIXEL_ARGB8888,
        .region={0,0,8u,8u}};
      if(!index){op->opcode=GML_RENDER_OP_CLEAR;op->payload.clear_colour=0x40273859u;continue;}
      op->opcode=GML_RENDER_OP_TEXTURED_QUAD;
      op->scene_family=background?GML_RENDER_SCENE_BACKGROUND:GML_RENDER_SCENE_SPRITE;
      op->sources=&source;op->source_count=1u;op->reconstruction_mode=GML_RENDER_DEVICE_CHECKPOINT;
      GmlRenderQuad *q=&op->payload.quad;q->shape=GML_RENDER_QUAD_AXIS_ALIGNED;
      q->sampling=state.samplers[0];q->filtered_draw_alpha=positive==14u?0.0:
        positive==15u?0x1p-40:positive==16u?1e-300:.7;
      q->span_active=positive?GML_RENDER_SPAN_FILTERED_POSITIVE_ROW:GML_RENDER_SPAN_FILTERED_AFFINE_ROW;
      q->inverse_x[0]=background?16.0:1.0;q->inverse_x[1]=.5;
      q->inverse_anchor[0]=background?-.5:0.0;
      q->x[1]=q->x[2]=2.0;q->y[2]=q->y[3]=1.0;
      for(unsigned corner=0;corner<4u;corner++){
        int right=corner==1u || corner==2u;
        q->corner_colour[corner]=positive>=14u?0x00123456u:0xB3123456u;
        q->u[corner]=background?(right?12.0:-4.0):(right?1.0:0.0);
        q->v[corner]=right?.625:.125;
      }
    }
    GmlRenderQuad *last=&ops[quads].payload.quad;
    if(invalid==1u) last->y[2]=last->y[3]=2.0;
    if(invalid==2u) last->inverse_x[1]=NAN;
    if(invalid==3u) last->inverse_offset[0]=1.0;
    if(invalid==4u) last->span_x[0]=1.0;
    if(invalid==5u) last->inverse_anchor[0]=background?20.0:2.0;
    if(invalid==6u) last->inverse_y[1]=1.0;
    if(invalid==7u) last->u[1]=.9;
    if(invalid==8u) ops[quads].scene_family=GML_RENDER_SCENE_TEXT;
    if(invalid==9u) ops[quads].reconstruction_mode=GML_RENDER_REPLAY_EXACT;
    if(invalid==10u) last->sampling.quantisation=0u;
    if(invalid==11u) last->span_active=4u;
    if(invalid==12u) last->inverse_x[0]=last->inverse_x[1]=0.0;
    if(invalid==13u){state.alpha_test_enabled=1u;state.alpha_test_reference=1u;}
    if(invalid==14u) last->filtered_draw_alpha=background>=3u?-.1:0x1p-33;
    if(invalid==15u){
      if(positive>=4u || background>=3u) last->inverse_offset[1]=1.0;
      else state.coverage_policy=positive==3u?GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA:0u;
    }
    if(invalid==16u) state.write_mask=background>=3u?16u:7u;
    if(invalid==17u) ops[quads].scene_family=GML_RENDER_SCENE_TILE;
    if(invalid==18u) last->sampling.filter=GML_RENDER_FILTER_NEAREST;
    if(invalid==19u) state.colour=0xFF123456u;
    if(invalid==20u) last->sampling.quantisation=GML_RENDER_QUANTISE_INTEGER_ROW_TINT_TRUNC;
    if(invalid==21u){state.alpha_test_enabled=1u;state.alpha_test_reference=0u;}
    if(invalid==22u){
      state.rgb_source_factor=GML_RENDER_BLEND_SOURCE_ALPHA;
      state.rgb_destination_factor=GML_RENDER_BLEND_ONE;
      state.alpha_source_factor=GML_RENDER_BLEND_SOURCE_ALPHA;
      state.alpha_destination_factor=GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA;
      state.rgb_equation=state.alpha_equation=GML_RENDER_BLEND_ADD;
    }
    GmlRenderDrawBatch batch={ops,quads+1u,&state,1u};
    GmlRenderExecutionResult result=device.submit(device.context,&batch);
    if(invalid){
      REQUIRE(result.status==GML_RENDER_EXEC_UNSUPPORTED && !result.accepted_prefix &&
        !anygm_test_graphics_clear_calls() && !anygm_test_graphics_scene_draw_calls() &&
        !anygm_test_graphics_scene_copies(),"invalid row suffix refuses before the clear prefix");
    }else{
      unsigned passes=(positive==1u || positive==5u || (background>=3u &&
        (positive==7u || positive==8u || positive==15u || positive==16u))) && (state.write_mask&8u)?2u:1u;
      REQUIRE(result.status==GML_RENDER_EXEC_ACCEPTED && result.accepted_prefix==quads+1u &&
        anygm_test_graphics_scene_draw_calls()==(int)((quads>2048u?2u:1u)*passes) &&
        anygm_test_graphics_scene_vertices()==(int)(quads*6u*passes) &&
        anygm_test_graphics_scene_filter()==GL_LINEAR && !anygm_test_graphics_scene_copies() &&
        !anygm_test_graphics_read_pixels(),"continuous row quads batch only at capacity without observation");
    }
    gml_gpu_destroy(gpu,1);free(ops);
  }
  return 1;
}
static int scene_hardware_screen_sprite_batch_case(void){
  return scene_hardware_filtered_batch_case(7) && scene_hardware_filtered_batch_case(11) &&
    scene_filtered_affine_row_batch_case();
}
static int scene_hardware_screen_font_batch_case(void){
  return scene_hardware_filtered_batch_case(8) && scene_hardware_filtered_batch_case(14);
}
static int scene_hardware_screen_trim_batch_case(void){
  return scene_hardware_filtered_batch_case(9);
}

/* Driver/state evidence only; pixel-centre point indices need real GL controls. */
static int scene_application_point_batch_case(void){
  enum { QUADS=2049 };
  const uint32_t pixels[4]={0xFF1267B9u,0xFF2679B3u,0xFFC39526u,0xFF7193C5u};
  for(unsigned api=GML_GPU_API_OPENGL_CORE;api<=GML_GPU_API_OPENGLES3;api++)
  for(unsigned variant=0;variant<9u;variant++){
    GmlRenderDrawOp *ops=calloc(QUADS+1u,sizeof *ops);
    GmlGpuContext context=fake_context(api);
    GmlRenderDeviceServices device={0};
    GmlRenderImageRef source={GML_RENDER_RESOURCE_APPLICATION,141u,1u,1u};
    GmlRenderImageUpload upload={0};
    GmlRenderDrawStateSnapshot state={0};
    anygm_test_graphics_reset();
    GmlGpu *gpu=gml_gpu_create();
    REQUIRE(ops && gpu && gml_gpu_context_reset(gpu,&context),"point presentation context adopted");
    gml_gpu_device_services(gpu,&device);
    upload.image=source;upload.storage_width=upload.storage_height=2u;
    upload.region=(GmlRenderRect){0,0,2u,2u};upload.pixel_format=GML_RENDER_PIXEL_ARGB8888;
    upload.bytes=pixels;upload.byte_capacity=sizeof pixels;upload.stride_bytes=8u;
    REQUIRE(device.upload(device.context,&upload),"point application uploaded");
    state.write_mask=15u;state.alpha=255u;state.colour=0xFFFFFFFFu;state.sampler_count=1u;
    state.samplers[0].filter=GML_RENDER_FILTER_HARDWARE_PIXEL_CENTRE_POINT;
    state.samplers[0].edge_rule=GML_RENDER_EDGE_CLAMP_SUBIMAGE;
    state.samplers[0].source_region=upload.region;
    state.rgb_source_factor=GML_RENDER_BLEND_SOURCE_ALPHA;
    state.rgb_destination_factor=GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA;
    state.alpha_source_factor=GML_RENDER_BLEND_ONE;
    state.alpha_destination_factor=GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA;
    state.rgb_equation=state.alpha_equation=GML_RENDER_BLEND_ADD;
    state.coverage_policy=GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA;
    for(unsigned index=0;index<QUADS+1u;index++){
      GmlRenderDrawOp *op=&ops[index];
      op->opcode=index?GML_RENDER_OP_TEXTURED_QUAD:GML_RENDER_OP_CLEAR;
      op->sequence=index+1u;
      op->target=(GmlRenderTargetRef){.image={GML_RENDER_RESOURCE_SCREEN,142u,1u,index+1u},
        .storage_version=1u,.width=64u,.height=48u,.pixel_format=GML_RENDER_PIXEL_ARGB8888,
        .region={0,0,64u,48u}};
      if(!index){op->payload.clear_colour=0x40123456u;continue;}
      op->scene_family=GML_RENDER_SCENE_SURFACE;
      op->reconstruction_mode=GML_RENDER_DEVICE_CHECKPOINT;
      op->sources=&source;op->source_count=1u;
      GmlRenderQuad *q=&op->payload.quad;
      q->shape=GML_RENDER_QUAD_AXIS_ALIGNED;q->sampling=state.samplers[0];
      q->source_opaque=1u;q->filtered_draw_alpha=1.0;
      q->inverse_anchor[0]=2.25;q->inverse_anchor[1]=3.125;
      q->inverse_x[0]=13.25;q->inverse_y[1]=11.5;
      q->x[0]=q->x[3]=2;q->x[1]=q->x[2]=15;
      q->y[0]=q->y[1]=3;q->y[2]=q->y[3]=15;
      for(unsigned corner=0;corner<4u;corner++){
        q->corner_colour[corner]=0xFFFFFFFFu;
        q->u[corner]=(q->x[corner]-q->inverse_anchor[0])/q->inverse_x[0];
        q->v[corner]=(q->y[corner]-q->inverse_anchor[1])/q->inverse_y[1];
      }
    }
    GmlRenderQuad *last=&ops[QUADS].payload.quad;
    if(variant==1u) last->source_opaque=0u;
    if(variant==2u) last->filtered_draw_alpha=.7;
    if(variant==3u) last->inverse_anchor[0]=NAN;
    if(variant==4u) last->u[1]=.9;
    if(variant==5u) last->corner_colour[1]=0xFFFEFFFFu;
    if(variant==6u) last->x[1]=last->x[2]=16;
    if(variant==7u) ops[QUADS].scene_family=GML_RENDER_SCENE_SPRITE;
    if(variant==8u) ops[QUADS].reconstruction_mode=GML_RENDER_REPLAY_EXACT;
    GmlRenderDrawBatch batch={ops,QUADS+1u,&state,1u};
    GmlRenderExecutionResult result=device.submit(device.context,&batch);
    if(variant){
      REQUIRE(result.status==GML_RENDER_EXEC_UNSUPPORTED && !result.accepted_prefix &&
              !anygm_test_graphics_clear_calls() && !anygm_test_graphics_scene_draw_calls(),
              "unqualified point presentation refuses before its clear prefix");
    }else{
      REQUIRE(result.status==GML_RENDER_EXEC_ACCEPTED && result.accepted_prefix==QUADS+1u &&
              anygm_test_graphics_scene_draw_calls()==2 &&
              anygm_test_graphics_scene_vertices()==QUADS*6 &&
              anygm_test_graphics_scene_filter()==GL_NEAREST,
              "opaque point presentation batches only at capacity");
      REQUIRE(!anygm_test_graphics_scene_copies() && !anygm_test_graphics_read_pixels(),
              "point presentation needs no predecessor or CPU observation");
      const char *fragment=anygm_test_graphics_shader_source(1);
      const char *point_dispatch=fragment?strstr(fragment,"if(u_filter==30){"):NULL;
      REQUIRE(point_dispatch && !strstr(point_dispatch+1,"if(u_filter==30){"),
              "point presentation selector has one fragment dispatch branch");
    }
    gml_gpu_destroy(gpu,1);free(ops);
  }
  return 1;
}

/* Dispatch, ordered predecessor copies and whole-batch refusal, not shader pixel evidence. */
static int scene_destination_surface_case(void){
  const uint32_t pixels[4]={0x001267B9u,0x552679B3u,0xAAC39526u,0xFF7193C5u};
  const unsigned extents[][2]={{2u,2u},{1u,1u},{3u,4u},{2u,3u}};
  for(unsigned api=0;api<2u;api++) for(unsigned mode=9;mode<=11u;mode++)
  for(unsigned scale=0;scale<sizeof extents/sizeof *extents;scale++)
  for(unsigned filtered=0;filtered<2u;filtered++)
  for(unsigned excluded=0;excluded<18u;excluded++){
    GmlGpuContext context=fake_context(api?GML_GPU_API_OPENGLES3:GML_GPU_API_OPENGL_CORE);
    GmlRenderImageRef source={GML_RENDER_RESOURCE_SURFACE,95u,1u,1u};
    GmlRenderImageUpload upload={0};GmlRenderDrawOp ops[3]={0};
    GmlRenderDrawStateSnapshot states[2]={0};GmlRenderDeviceServices device={0};
    anygm_test_graphics_reset();
    if(excluded==1u) anygm_test_graphics_withhold("glCopyTexSubImage2D");
    GmlGpu *gpu=gml_gpu_create();
    REQUIRE(gpu && gml_gpu_context_reset(gpu,&context),"destination surface context adopted");
    gml_gpu_device_services(gpu,&device);
    upload.image=source;upload.storage_width=upload.storage_height=2u;
    upload.region=(GmlRenderRect){0,0,2u,2u};upload.pixel_format=GML_RENDER_PIXEL_ARGB8888;
    upload.bytes=pixels;upload.byte_capacity=sizeof pixels;upload.stride_bytes=8u;
    REQUIRE(device.upload(device.context,&upload),"destination surface source uploaded");
    for(unsigned index=0;index<3u;index++){
      GmlRenderDrawOp *op=&ops[index];
      op->target.image=(GmlRenderImageRef){GML_RENDER_RESOURCE_SURFACE,96u,1u,index+1u};
      op->target.storage_version=1u;op->target.width=op->target.height=8u;
      op->target.pixel_format=GML_RENDER_PIXEL_ARGB8888;
      op->target.region=(GmlRenderRect){0,0,8u,8u};op->sequence=index+1u;
      if(!index){op->opcode=GML_RENDER_OP_CLEAR;op->payload.clear_colour=0x71123456u;continue;}
      GmlRenderDrawStateSnapshot *state=&states[index-1u];
      state->write_mask=15u;state->alpha=255u;state->colour=0xFFFFFFFFu;
      state->rgb_source_factor=state->alpha_source_factor=mode==11u?
        GML_RENDER_BLEND_DESTINATION_ALPHA:GML_RENDER_BLEND_DESTINATION_COLOUR;
      state->rgb_destination_factor=state->alpha_destination_factor=mode==9u?
        GML_RENDER_BLEND_SOURCE_COLOUR:GML_RENDER_BLEND_DESTINATION_COLOUR;
      state->rgb_equation=GML_RENDER_BLEND_EXACT_DESTINATION_ADD;
      state->alpha_equation=index==1u?GML_RENDER_BLEND_EXACT_DESTINATION_MIN:
                                    GML_RENDER_BLEND_EXACT_DESTINATION_MAX;
      state->sampler_count=1u;
      state->samplers[0].filter=filtered?GML_RENDER_FILTER_BOX:GML_RENDER_FILTER_NEAREST;
      state->samplers[0].edge_rule=GML_RENDER_EDGE_CLAMP_SUBIMAGE;
      state->samplers[0].source_region=upload.region;
      op->opcode=GML_RENDER_OP_TEXTURED_QUAD;op->scene_family=GML_RENDER_SCENE_SURFACE;
      op->state_index=index-1u;op->sources=&source;op->source_count=1u;
      op->reconstruction_mode=GML_RENDER_DEVICE_CHECKPOINT;
      GmlRenderQuad *q=&op->payload.quad;
      q->shape=GML_RENDER_QUAD_AXIS_ALIGNED;q->sampling=state->samplers[0];
      q->filtered_draw_alpha=.5;
      q->x[1]=q->x[2]=extents[scale][0];q->y[2]=q->y[3]=extents[scale][1];
      q->u[1]=q->u[2]=q->v[2]=q->v[3]=1.0;
      for(unsigned corner=0;corner<4u;corner++) q->corner_colour[corner]=0x809371C5u;
    }
    if(excluded==2u) ops[2].payload.quad.filtered_draw_alpha=NAN;
    if(excluded==3u) ops[2].payload.quad.x[1]=ops[2].payload.quad.x[2]=3.5;
    if(excluded==4u) ops[2].scene_family=GML_RENDER_SCENE_BACKGROUND;
    if(excluded==5u) ops[2].payload.quad.corner_colour[1]^=1u;
    if(excluded==6u) ops[2].payload.quad.u[1]=.9;
    if(excluded==7u) states[1].write_mask=7u;
    if(excluded==8u) states[1].alpha_test_enabled=1u;
    if(excluded==9u) states[1].alpha_source_factor=GML_RENDER_BLEND_ONE;
    if(excluded==10u) states[1].rgb_equation=GML_RENDER_BLEND_ADD;
    if(excluded==11u) ops[2].payload.quad.filtered_draw_alpha=0x1p-40;
    if(excluded==12u) for(unsigned corner=0;corner<4u;corner++)
      ops[2].payload.quad.corner_colour[corner]=0x7F9371C5u;
    if(excluded==13u) states[1].samplers[0].filter=GML_RENDER_FILTER_HARDWARE_BILINEAR;
    if(excluded==14u) ops[2].payload.quad.x[1]=ops[2].payload.quad.x[2]=4097.0;
    if(excluded==15u) states[1].samplers[0].quantisation=GML_RENDER_QUANTISE_STRETCHED_FLOAT;
    if(excluded==16u) states[1].samplers[0].edge_rule=GML_RENDER_EDGE_TRANSPARENT;
    if(excluded==17u) ops[2].payload.quad.y[2]=ops[2].payload.quad.y[3]=0.0;
    ops[2].payload.quad.sampling=states[1].samplers[0];
    GmlRenderDrawBatch batch={ops,3u,states,2u};
    GmlRenderExecutionResult result=device.submit(device.context,&batch);
    if(excluded){
      REQUIRE(result.status==GML_RENDER_EXEC_UNSUPPORTED && !result.accepted_prefix &&
        !anygm_test_graphics_clear_calls() && !anygm_test_graphics_scene_draw_calls() &&
        !anygm_test_graphics_scene_copies(),"invalid suffix refuses before clear, copies or draws");
    } else {
      REQUIRE(result.status==GML_RENDER_EXEC_ACCEPTED && result.accepted_prefix==3u &&
        anygm_test_graphics_scene_draw_calls()==2 && anygm_test_graphics_scene_copies()==2 &&
        anygm_test_graphics_scene_copied_pixels()==2u*extents[scale][0]*extents[scale][1] &&
        !anygm_test_graphics_read_pixels(),
        "each ordered surface samples a bounded predecessor without CPU readback");
    }
    gml_gpu_destroy(gpu,1);
  }
  return 1;
}

/* The fake driver supplies masks and faults; the real-device comparator proves shader pixels. */
static int incremental_checkpoint_contract_case(void){
  for(unsigned api=0;api<2u;api++) for(unsigned owner=0;owner<2u;owner++)
  for(unsigned variant=0;variant<7u;variant++){
    uint32_t pixels[65u*33u],before[65u*33u];
    GmlGpu *gpu;
    GmlGpuContext context=fake_context(api?GML_GPU_API_OPENGLES3:GML_GPU_API_OPENGL_CORE);
    GmlRenderDeviceServices device={0};
    GmlRenderDrawOp clear={0};
    GmlRenderDrawBatch batch={&clear,1u,NULL,0u};
    GmlRenderImageReadback readback={0};
    GmlRenderImageReadbackResult result={0};
    GmlGpuCounters counters;
    unsigned reads;
    anygm_test_graphics_reset();
    if(variant==5u) anygm_test_graphics_withhold("glCopyTexSubImage2D");
    gpu=gml_gpu_create();
    REQUIRE(gpu && gml_gpu_context_reset(gpu,&context),"checkpoint context adopted");
    gml_gpu_device_services(gpu,&device);
    clear.opcode=GML_RENDER_OP_CLEAR;
    clear.target.image=(GmlRenderImageRef){owner?GML_RENDER_RESOURCE_APPLICATION:GML_RENDER_RESOURCE_SCREEN,
                                         101u,1u,2u};
    clear.target.storage_version=1u;clear.target.width=65u;clear.target.height=33u;
    clear.target.pixel_format=GML_RENDER_PIXEL_ARGB8888;
    clear.target.region=(GmlRenderRect){0,0,65u,33u};
    REQUIRE(device.submit(device.context,&batch).status==GML_RENDER_EXEC_ACCEPTED,"checkpoint target created");
    readback.image=clear.target.image;readback.storage_width=65u;readback.storage_height=33u;
    readback.region=clear.target.region;readback.pixel_format=GML_RENDER_PIXEL_ARGB8888;
    readback.stride_bytes=65u*4u;readback.byte_capacity=sizeof pixels;readback.bytes=pixels;
    readback.checkpoint=1u;readback.result=&result;
    REQUIRE(device.readback(device.context,&readback) && result.pixel_reads==1u &&
            result.pixel_bytes==sizeof pixels && !result.mask_reads,"cold checkpoint reads all bytes");
    memcpy(before,pixels,sizeof before);
    clear.target.image.content_version++;
    REQUIRE(device.submit(device.context,&batch).status==GML_RENDER_EXEC_ACCEPTED,"next checkpoint version drawn");
    readback.image=clear.target.image;readback.checkpoint_baseline_version=2u;
    anygm_test_graphics_checkpoint_mask(0u);
    reads=anygm_test_graphics_read_pixels();
    if(variant==1u) anygm_test_graphics_fail_compile(1);
    if(variant==6u) anygm_test_graphics_fail_link(1);
    if(variant==2u) anygm_test_graphics_active_query(999u);
    if(variant==3u) anygm_test_graphics_fail_readback_at(reads+1u);
    if(variant==4u){
      anygm_test_graphics_checkpoint_mask(32u);
      anygm_test_graphics_fail_readback_at(reads+2u);
    }
    if(variant==4u){
      REQUIRE(!device.readback(device.context,&readback) && !memcmp(before,pixels,sizeof before) &&
              result.mask_reads==1u && result.pixel_reads==1u && result.pixel_bytes==4u,
              "failed sparse read preserves destination bytes and counts attempted work");
      REQUIRE(device.readback(device.context,&readback) && result.pixel_bytes==sizeof pixels &&
              !result.mask_reads,"failed sparse read cannot reuse its GPU baseline");
    }else{
      REQUIRE(device.readback(device.context,&readback),"checkpoint detection or full fallback succeeds");
      REQUIRE(result.pixel_reads==(variant?1u:0u) && result.pixel_bytes==(variant?sizeof pixels:0u),
              "unchanged mask elides pixels; optional detector failures retain full reads");
      REQUIRE(result.mask_reads==(variant==0u || variant==3u?1u:0u),"mask attempts remain explicit");
      REQUIRE(!memcmp(before,pixels,sizeof before),"unchanged or fallback buffer is exact");
      if(variant==1u || variant==6u){
        unsigned compiles=anygm_test_graphics_compile_calls();
        REQUIRE(device.readback(device.context,&readback) && !result.mask_reads &&
                result.pixel_bytes==sizeof pixels && anygm_test_graphics_compile_calls()==compiles,
                "a refused optional detector does not retry its compilation every checkpoint");
      }
    }
    if(variant==0u){
      clear.target.image.content_version++;
      REQUIRE(device.submit(device.context,&batch).status==GML_RENDER_EXEC_ACCEPTED,"sparse version drawn");
      readback.image=clear.target.image;readback.checkpoint_baseline_version=3u;
      anygm_test_graphics_checkpoint_mask(32u);
      REQUIRE(device.readback(device.context,&readback) && result.pixel_reads==1u && result.pixel_bytes==4u &&
              result.mask_reads==1u && result.mask_bytes==24u && result.updated_region.x==64 &&
              result.updated_region.y==0 && result.updated_region.width==1u && result.updated_region.height==1u,
              "bottom-up mask clips the odd top-right tile to one canonical pixel");
      for(unsigned index=0;index<65u*33u;index++)
        REQUIRE(pixels[index]==(index==64u?0xFF000033u:before[index]),"sparse write preserves every other byte");
      REQUIRE(strstr(anygm_test_graphics_shader_source(1),"checkpoint_tile_changed") &&
              strstr(anygm_test_graphics_shader_source(1),"texelFetch"),"real comparison branch is compiled");
      gml_gpu_counters(gpu,&counters);
      REQUIRE(counters.scene_readbacks==2u && counters.scene_readback_bytes==sizeof pixels+4u &&
              counters.scene_readback_masks==2u && counters.scene_readback_mask_bytes==48u,
              "facade counters reflect physical pixels and auxiliary masks, not logical image sizes");
      gml_gpu_context_lost(gpu);
      REQUIRE(gml_gpu_context_reset(gpu,&context),"replacement context adopted");
      gml_gpu_device_services(gpu,&device);
      REQUIRE(device.submit(device.context,&batch).status==GML_RENDER_EXEC_ACCEPTED,"replacement target drawn");
      REQUIRE(device.readback(device.context,&readback) && !result.mask_reads && result.pixel_bytes==sizeof pixels,
              "same logical version in a new context cannot reuse a forgotten checkpoint");
    }
    anygm_test_graphics_active_query(0u);anygm_test_graphics_fail_compile(0);anygm_test_graphics_fail_link(0);
    gml_gpu_destroy(gpu,1);
  }
  return 1;
}

static int incremental_checkpoint_budget_case(void){
  const uint32_t width=4096u,height=2049u;
  size_t bytes=(size_t)width*height*4u;
  uint8_t *pixels=(uint8_t*)malloc(bytes+16u);
  GmlGpu *gpu;
  GmlGpuContext context=fake_context(GML_GPU_API_OPENGL_CORE);
  GmlRenderDeviceServices device={0};
  GmlRenderDrawOp clear={0};
  GmlRenderDrawBatch batch={&clear,1u,NULL,0u};
  GmlRenderImageReadback readback={0};
  GmlRenderImageReadbackResult result={0};
  REQUIRE(pixels,"over-budget checkpoint buffer allocated");
  memset(pixels+bytes,0xA5,16u);
  anygm_test_graphics_reset();
  gpu=gml_gpu_create();
  REQUIRE(gpu && gml_gpu_context_reset(gpu,&context),"budget context adopted");
  gml_gpu_device_services(gpu,&device);
  clear.opcode=GML_RENDER_OP_CLEAR;
  clear.target.image=(GmlRenderImageRef){GML_RENDER_RESOURCE_SCREEN,102u,1u,1u};
  clear.target.storage_version=1u;clear.target.width=width;clear.target.height=height;
  clear.target.pixel_format=GML_RENDER_PIXEL_XRGB8888;
  clear.target.region=(GmlRenderRect){0,0,width,height};
  readback.storage_width=width;readback.storage_height=height;readback.region=clear.target.region;
  readback.pixel_format=GML_RENDER_PIXEL_XRGB8888;readback.stride_bytes=width*4u;
  readback.byte_capacity=bytes;readback.bytes=pixels;readback.checkpoint=1u;readback.result=&result;
  for(unsigned pass=0;pass<2u;pass++){
    clear.target.image.content_version++;
    REQUIRE(device.submit(device.context,&batch).status==GML_RENDER_EXEC_ACCEPTED,"over-budget target drawn");
    readback.image=clear.target.image;
    readback.checkpoint_baseline_version=pass?clear.target.image.content_version-1u:0u;
    REQUIRE(device.readback(device.context,&readback) && result.pixel_reads==1u &&
            result.pixel_bytes==bytes && !result.mask_reads && !anygm_test_graphics_scene_copies(),
            "a target above 32 MiB never allocates or trusts an incremental baseline");
    for(unsigned index=0;index<16u;index++) REQUIRE(pixels[bytes+index]==0xA5u,"over-budget guards preserved");
  }
  gml_gpu_destroy(gpu,1);free(pixels);
  return 1;
}

/* Readback conversion is host byte packing, not a restatement of a shader. The driver supplies
 * independent RGBA rows; check the public formats and every untouched byte around their output. */
static int scene_readback_formats_case(void){
  static const uint32_t widths[]={1u,2u,3u,4u,7u,8u,15u,16u,17u,31u,32u,33u,
                                  127u,128u,129u,257u};
  static const uint32_t formats[]={GML_RENDER_PIXEL_XRGB8888,GML_RENDER_PIXEL_ARGB8888,
                                   GML_RENDER_PIXEL_RGBA8888};
  static const unsigned alphas[]={0u,1u,127u,128u,254u,255u};
  uint8_t output[(257u*4u+5u)*3u+32u];
  for(unsigned api_index=0;api_index<2u;api_index++){
    GmlGpuApi api=api_index?GML_GPU_API_OPENGLES3:GML_GPU_API_OPENGL_CORE;
    GmlGpuContext context=fake_context(api);
    GmlGpu *gpu;
    GmlRenderDeviceServices services={0};
    GmlRenderDrawOp clear={0};
    GmlRenderDrawBatch batch={0};
    GmlRenderImageReadback readback={0};
    anygm_test_graphics_reset();
    gpu=gml_gpu_create();
    REQUIRE(gpu && gml_gpu_context_reset(gpu,&context),"format context adopted");
    gml_gpu_device_services(gpu,&services);
    clear.sequence=1u;
    clear.opcode=GML_RENDER_OP_CLEAR;
    clear.target.image=(GmlRenderImageRef){GML_RENDER_RESOURCE_SURFACE,101u,1u,1u};
    clear.target.storage_version=1u;
    clear.target.width=259u;clear.target.height=9u;
    clear.target.pixel_format=GML_RENDER_PIXEL_ARGB8888;
    clear.target.region=(GmlRenderRect){0,0,259u,9u};
    batch.operations=&clear;batch.operation_count=1u;
    REQUIRE(services.submit(services.context,&batch).status==GML_RENDER_EXEC_ACCEPTED,
            "format target is a valid rendered image");
    readback.image=clear.target.image;
    readback.storage_width=259u;readback.storage_height=9u;
    for(size_t width_index=0;width_index<sizeof widths/sizeof widths[0];width_index++)
      for(uint32_t height=1u;height<=3u;height+=2u)
        for(size_t format_index=0;format_index<sizeof formats/sizeof formats[0];format_index++)
          for(size_t alpha_index=0;alpha_index<sizeof alphas/sizeof alphas[0];alpha_index++)
            for(unsigned padded=0;padded<2u;padded++)
              for(unsigned unaligned=0;unaligned<2u;unaligned++){
                uint32_t width=widths[width_index],stride=width*4u+padded*5u;
                uint8_t *pixels=output+16u+unaligned;
                readback.region=(GmlRenderRect){1,2,width,height};
                readback.pixel_format=formats[format_index];
                readback.stride_bytes=stride;
                readback.byte_capacity=(size_t)(height-1u)*stride+width*4u;
                readback.bytes=pixels;
                memset(output,0xA5,sizeof output);
                anygm_test_graphics_readback_alpha(alphas[alpha_index]);
                REQUIRE(services.readback(services.context,&readback),
                        "tight and padded unaligned format transfers succeed");
                for(size_t index=0;index<sizeof output;index++){
                  size_t offset=index-(size_t)(pixels-output);
                  if(index<(size_t)(pixels-output) || offset>=readback.byte_capacity ||
                     offset%stride>=width*4u)
                    REQUIRE(output[index]==0xA5u,"readback leaves guards and row padding intact");
                }
                for(uint32_t row=0;row<height;row++)
                  for(uint32_t column=0;column<width;column++){
                    const uint8_t *pixel=pixels+(size_t)row*stride+(size_t)column*4u;
                    if(readback.pixel_format==GML_RENDER_PIXEL_RGBA8888){
                      REQUIRE(pixel[0]==height-1u-row && pixel[1]==(uint8_t)column &&
                              pixel[2]==0x33u && pixel[3]==alphas[alpha_index],
                              "RGBA preserves byte order, alpha and top-down rows");
                    }else{
                      uint32_t actual,alpha=readback.pixel_format==GML_RENDER_PIXEL_XRGB8888
                                             ?255u:alphas[alpha_index];
                      uint32_t expected=(alpha<<24)|((height-1u-row)<<16)|
                                        ((uint32_t)(uint8_t)column<<8)|0x33u;
                      memcpy(&actual,pixel,sizeof actual);
                      REQUIRE(actual==expected,"native packed words preserve channels and alpha policy");
                    }
                  }
              }
    for(unsigned invalid=0;invalid<5u;invalid++){
      unsigned reads=anygm_test_graphics_read_pixels();
      readback.region=(GmlRenderRect){1,2,17u,3u};
      readback.pixel_format=GML_RENDER_PIXEL_ARGB8888;
      readback.stride_bytes=73u;readback.byte_capacity=214u;readback.bytes=output+16u;
      if(invalid==0u) readback.byte_capacity--;
      if(invalid==1u) readback.stride_bytes=67u;
      if(invalid==2u) readback.pixel_format=UINT32_MAX;
      if(invalid==3u) readback.region.x=-1;
      if(invalid==4u) readback.region.y=7;
      memset(output,0xA5,sizeof output);
      REQUIRE(!services.readback(services.context,&readback) &&
              anygm_test_graphics_read_pixels()==reads,
              "invalid transfers refuse before a driver read");
      for(size_t index=0;index<sizeof output;index++)
        REQUIRE(output[index]==0xA5u,"a refused transfer writes no destination byte");
    }
    gml_gpu_destroy(gpu,1);
  }
  return 1;
}

static int scene_atlas_retire_advanced_version_case(void){
  uint8_t pixels[16]={255u,0u,0u,255u};
  GmlRenderImageRef page={GML_RENDER_RESOURCE_ATLAS,11u,29u,1u};
  GmlRenderImageUpload upload={0};
  GmlRenderDeviceServices services;
  GmlGpu *gpu;
  GmlGpuContext context;
  int before;
  unsigned old_name,new_name;
  anygm_test_graphics_reset();
  gpu=gml_gpu_create();
  REQUIRE(gpu!=NULL,"atlas retirement device allocated");
  context=fake_context(GML_GPU_API_OPENGL_CORE);
  REQUIRE(gml_gpu_context_reset(gpu,&context),"atlas retirement context adopted");
  gml_gpu_device_services(gpu,&services);
  upload.image=page;
  upload.storage_width=upload.storage_height=2u;
  upload.region=(GmlRenderRect){0,0,2u,2u};
  upload.pixel_format=GML_RENDER_PIXEL_RGBA8888;
  upload.stride_bytes=8u;
  upload.byte_capacity=sizeof pixels;
  upload.bytes=pixels;
  REQUIRE(services.upload(services.context,&upload),"runtime atlas old version uploaded");
  old_name=anygm_test_graphics_texture_with_format(GL_RGBA8);
  REQUIRE(old_name!=0u,"old atlas texture has a physical identity");
  before=anygm_test_graphics_deletes();
  page.content_version++;
  services.retire(services.context,page);
  REQUIRE(anygm_test_graphics_deletes()==before,
          "retirement is logical until the next current-context access");
  pixels[0]=0u;
  upload.image=page;
  REQUIRE(services.upload(services.context,&upload),
          "changed page uploads after the retired version");
  new_name=anygm_test_graphics_texture_with_format(GL_RGBA8);
  REQUIRE(anygm_test_graphics_deletes()==before+1 && new_name!=old_name &&
          anygm_test_graphics_texture_data_uploads(new_name)==1u,
          "next current upload deletes the stale texture and creates a fresh version");
  gml_gpu_destroy(gpu,1);
  return 1;
}

static int scene_numbered_rotated_texel_spans_case(void){
  const uint8_t pixels[16]={255,0,0,128,0,255,0,0,0,0,255,255,255,255,255,64};
  for(unsigned mode=0;mode<2u;mode++)
  for(unsigned api=0;api<2u;api++){
    GmlRenderImageRef source={GML_RENDER_RESOURCE_ATLAS,95u,1u,1u};
    GmlRenderImageRef target={GML_RENDER_RESOURCE_SURFACE,96u,1u,1u};
    GmlRenderDrawStateSnapshot states[2]={0};GmlRenderDrawOp operations[3]={0};
    GmlRenderImageUpload upload={0};GmlRenderDeviceServices service;
    GmlGpuContext context=fake_context(api?GML_GPU_API_OPENGLES3:GML_GPU_API_OPENGL_CORE);
    anygm_test_graphics_reset();
    GmlGpu *gpu=gml_gpu_create();
    REQUIRE(gpu && gml_gpu_context_reset(gpu,&context),"texel-span context adopted");
    gml_gpu_device_services(gpu,&service);
    upload.image=source;upload.storage_width=upload.storage_height=2u;
    upload.region=(GmlRenderRect){0,0,2u,2u};upload.pixel_format=GML_RENDER_PIXEL_RGBA8888;
    upload.stride_bytes=8u;upload.byte_capacity=sizeof pixels;upload.bytes=pixels;
    REQUIRE(service.upload(service.context,&upload),"texel-span source uploaded");
    for(unsigned index=0;index<3u;index++){
      GmlRenderDrawOp *op=&operations[index];
      op->target.image=target;op->target.image.content_version=index+1u;
      op->target.storage_version=1u;op->target.width=op->target.height=8u;
      op->target.pixel_format=GML_RENDER_PIXEL_ARGB8888;
      op->target.region=(GmlRenderRect){0,0,8u,8u};
      if(!index){op->opcode=GML_RENDER_OP_CLEAR;op->payload.clear_colour=0x40102030u;continue;}
      GmlRenderDrawStateSnapshot *state=&states[index-1u];
      state->write_mask=15u;state->alpha=255u;state->colour=0xffffffffu;state->sampler_count=1u;
      state->rgb_source_factor=state->alpha_source_factor=GML_RENDER_BLEND_SOURCE_ALPHA;
      state->rgb_destination_factor=state->alpha_destination_factor=GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA;
      state->rgb_equation=state->alpha_equation=GML_RENDER_BLEND_ADD;
      state->samplers[0].filter=GML_RENDER_FILTER_NEAREST_PHASED;
      state->samplers[0].edge_rule=GML_RENDER_EDGE_CLAMP_SUBIMAGE;
      state->samplers[0].quantisation=GML_RENDER_QUANTISE_SPRITE_MODERN_ROTATED_NEAREST;
      state->samplers[0].source_region=(GmlRenderRect){(int)index-1,0,1u,1u};
      op->opcode=GML_RENDER_OP_TEXTURED_QUAD;op->scene_family=GML_RENDER_SCENE_SPRITE;
      op->reconstruction_mode=GML_RENDER_DEVICE_CHECKPOINT;
      op->state_index=index-1u;op->sources=&source;op->source_count=1u;
      GmlRenderQuad *q=&op->payload.quad;q->shape=GML_RENDER_QUAD_AXIS_ALIGNED;
      q->sampling=state->samplers[0];q->filtered_draw_alpha=0.5;
      q->x[0]=q->x[3]=(index-1u)*3u;q->x[1]=q->x[2]=index*3u;
      q->y[2]=q->y[3]=1.0;
      for(unsigned corner=0;corner<4u;corner++){
        q->u[corner]=((double)index-0.5)/2.0;q->v[corner]=0.25;
        q->corner_colour[corner]=0x80402010u;
      }
      if(mode){
        q->span_active=GML_RENDER_SPAN_FIXED20_ROW;
        q->sampling.source_region=(GmlRenderRect){0,0,2u,2u};
        state->samplers[0]=q->sampling;
        q->span_x[0]=index==1u?8.0:24.0;
        q->span_x[2]=index==1u?8.0:-8.0;
        q->span_y[0]=8.0;
        for(unsigned corner=0;corner<4u;corner++) q->u[corner]=q->v[corner]=0.0;
      }
    }
    GmlRenderDrawBatch batch={operations,3u,states,2u};
    unsigned copies=anygm_test_graphics_scene_copies();
    GmlRenderExecutionResult result=service.submit(service.context,&batch);
    REQUIRE(result.status==GML_RENDER_EXEC_ACCEPTED && result.accepted_prefix==3u,
            "two disjoint CPU-selected texel spans accepted");
    REQUIRE(anygm_test_graphics_scene_copies()==copies+1u && anygm_test_graphics_read_pixels()==0u,
            "disjoint spans share one device predecessor without CPU observation");
    REQUIRE(anygm_test_graphics_color_mask_all() && !anygm_test_graphics_blend_enabled(),
            "texel-span composition restores write mask and blending");
    for(unsigned variant=0;variant<(mode?10u:8u);variant++){
      GmlRenderDrawStateSnapshot invalid_states[2];GmlRenderDrawOp invalid_operations[3];
      memcpy(invalid_states,states,sizeof states);memcpy(invalid_operations,operations,sizeof operations);
      GmlRenderDrawStateSnapshot *state=&invalid_states[0];GmlRenderQuad *q=&invalid_operations[1].payload.quad;
      if(mode){
        if(variant==0u) state->write_mask=7u;
        if(variant==1u) q->y[2]=q->y[3]=2.0;
        if(variant==2u) q->span_active=1u;
        if(variant==3u) q->span_x[1]=65536.0;
        if(variant==4u) q->span_x[0]=32768.0;
        if(variant==5u){q->span_x[2]=32767.0;q->span_x[3]=65535.0;}
        if(variant==6u){q->span_x[0]=32767.0;q->span_x[1]=65535.0;}
        if(variant==7u) q->filtered_draw_alpha=0.0;
        if(variant==8u) q->span_y[1]=0.5;
        if(variant==9u) invalid_operations[1].target.width=16385u;
      } else {
        if(variant==0u) state->write_mask=7u;
        if(variant==1u) q->y[2]=q->y[3]=2.0;
        if(variant==2u) q->sampling.source_region.x=2;
        if(variant==3u) q->u[0]+=0.125;
        if(variant==4u) q->span_active=1u;
        if(variant==5u) invalid_operations[1].target.image.resource_class=GML_RENDER_RESOURCE_SCREEN;
        if(variant==6u) invalid_operations[1].reconstruction_mode=GML_RENDER_REPLAY_EXACT;
        if(variant==7u) q->filtered_draw_alpha=0.0;
      }
      state->samplers[0]=q->sampling;
      GmlRenderDrawBatch invalid={invalid_operations,3u,invalid_states,2u};
      result=service.submit(service.context,&invalid);
      REQUIRE(result.status!=GML_RENDER_EXEC_ACCEPTED && result.accepted_prefix==0u,
              "unqualified texel span is refused before any target write");
    }
    gml_gpu_destroy(gpu,1);
  }
  return 1;
}

/* Driver call/state proof for independent channel masks. Real pixels are qualified separately. */
static int scene_numbered_sprite_channel_masks_case(void){
  const uint8_t pixels[16]={255,0,0,128,0,255,0,0,0,0,255,255,255,255,255,64};
  for(unsigned api=0;api<2u;api++)
  for(unsigned mode=0;mode<5u;mode++){
    unsigned packet_alpha=mode==4u;
    unsigned stamp=mode==1u || packet_alpha;
    unsigned modern=mode==3u;
    GmlRenderImageRef source={GML_RENDER_RESOURCE_ATLAS,93u,1u,1u};
    GmlRenderImageRef target={GML_RENDER_RESOURCE_SURFACE,94u,1u,1u};
    if(packet_alpha) target.resource_class=GML_RENDER_RESOURCE_SCREEN;
    GmlRenderImageUpload upload={0};
    GmlRenderDrawStateSnapshot states[16]={0};
    GmlRenderDrawOp operations[17]={0};
    GmlRenderDeviceServices service;
    GmlGpuContext context=fake_context(api?GML_GPU_API_OPENGLES3:GML_GPU_API_OPENGL_CORE);
    anygm_test_graphics_reset();
    GmlGpu *gpu=gml_gpu_create();
    REQUIRE(gpu && gml_gpu_context_reset(gpu,&context),"masked sprite context adopted");
    gml_gpu_device_services(gpu,&service);
    upload.image=source;upload.storage_width=upload.storage_height=2u;
    upload.region=(GmlRenderRect){0,0,2u,2u};
    upload.pixel_format=GML_RENDER_PIXEL_RGBA8888;
    upload.stride_bytes=8u;upload.byte_capacity=sizeof pixels;upload.bytes=pixels;
    REQUIRE(service.upload(service.context,&upload),"masked sprite source uploaded");
    for(unsigned index=0;index<17u;index++){
      GmlRenderDrawOp *op=&operations[index];
      op->sequence=index+1u;op->target.image=target;
      op->target.image.content_version=index+1u;op->target.storage_version=1u;
      op->target.width=op->target.height=8u;
      op->target.pixel_format=GML_RENDER_PIXEL_ARGB8888;
      op->target.region=(GmlRenderRect){0,0,8u,8u};
      if(!index){op->opcode=GML_RENDER_OP_CLEAR;op->payload.clear_colour=0x40102030u;continue;}
      unsigned mask=index-1u;
      GmlRenderDrawStateSnapshot *state=&states[mask];
      state->write_mask=mask;state->alpha=255u;state->colour=0xffffffffu;
      state->sampler_count=1u;
      state->rgb_source_factor=GML_RENDER_BLEND_SOURCE_ALPHA;
      state->alpha_source_factor=mode>=2u && !packet_alpha?GML_RENDER_BLEND_SOURCE_ALPHA:GML_RENDER_BLEND_ONE;
      state->rgb_destination_factor=state->alpha_destination_factor=
        GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA;
      state->rgb_equation=state->alpha_equation=GML_RENDER_BLEND_ADD;
      state->samplers[0].filter=packet_alpha?GML_RENDER_FILTER_NEAREST_PHASED_PACKET_ALPHA:
                                stamp?GML_RENDER_FILTER_NEAREST_PHASED_OPAQUE_ALPHA:
                                      GML_RENDER_FILTER_NEAREST_PHASED;
      if(packet_alpha){state->alpha_test_enabled=1u;state->alpha_test_reference=17u*mask;}
      state->samplers[0].edge_rule=GML_RENDER_EDGE_CLAMP_SUBIMAGE;
      state->samplers[0].source_region=upload.region;
      if(modern) state->samplers[0].quantisation=GML_RENDER_QUANTISE_SPRITE_MODERN_NEAREST;
      op->opcode=GML_RENDER_OP_TEXTURED_QUAD;op->scene_family=GML_RENDER_SCENE_SPRITE;
      op->reconstruction_mode=GML_RENDER_DEVICE_CHECKPOINT;
      op->state_index=mask;op->sources=&source;op->source_count=1u;
      GmlRenderQuad *quad=&op->payload.quad;
      quad->shape=GML_RENDER_QUAD_AXIS_ALIGNED;quad->sampling=state->samplers[0];
      if(packet_alpha && (mask&1u)) quad->span_active=GML_RENDER_SPAN_OPAQUE_SCALAR_ALPHA;
      if(modern){
        quad->filtered_draw_alpha=0.5;
        quad->inverse_x[0]=quad->inverse_y[1]=2.0;
        quad->inverse_x[1]=quad->inverse_y[0]=0.5;
      }
      quad->x[1]=quad->x[2]=quad->y[2]=quad->y[3]=4.0;
      quad->u[1]=quad->u[2]=quad->v[2]=quad->v[3]=1.0;
      for(unsigned corner=0;corner<4u;corner++) quad->corner_colour[corner]=0x80402010u;
    }
    GmlRenderDrawBatch batch={operations,17u,states,16u};
    unsigned copies=anygm_test_graphics_scene_copies();
    GmlRenderExecutionResult result=service.submit(service.context,&batch);
    REQUIRE(result.status==GML_RENDER_EXEC_ACCEPTED && result.accepted_prefix==17u,
            "all channel masks submit on a numbered sprite target");
    for(unsigned mask=0;mask<16u;mask++){
      unsigned expected=packet_alpha?(mask==8u?9u:1u):
                        stamp?(mask<8u?2u:mask==8u?8u:0u):1u;
      REQUIRE(anygm_test_graphics_scene_mask_draws(mask)==expected,
              "mask changes split batches and alpha stamping respects its enabled bit");
    }
    REQUIRE(anygm_test_graphics_color_mask_all() && !anygm_test_graphics_blend_enabled(),
            "masked draws restore complete writes and disabled blending");
    REQUIRE(anygm_test_graphics_read_pixels()==0,"channel masks do not observe the target");
    if(modern) REQUIRE(anygm_test_graphics_scene_copies()==copies+16u,
            "modern masked source-over takes ordered device predecessors across mask changes");
    if(modern){
      unsigned map_x=anygm_test_graphics_first_texture_with_format(GL_R16UI);
      unsigned map_y=anygm_test_graphics_texture_with_format(GL_R16UI);
      REQUIRE(map_x && map_y && map_x!=map_y &&
              anygm_test_graphics_scene_sampler_texture(2u)==map_x &&
              anygm_test_graphics_scene_sampler_texture(3u)==map_y &&
              anygm_test_graphics_texture_width(map_x)==4 &&
              anygm_test_graphics_texture_height(map_x)==1 &&
              anygm_test_graphics_texture_width(map_y)==4 &&
              anygm_test_graphics_texture_height(map_y)==1,
              "both independent integer point axes bind at the real scene draw");
      GmlRenderDrawOp upload_operations[2]={operations[0],operations[8]};
      GmlRenderDrawBatch upload_batch={upload_operations,2u,states,16u};
      upload_operations[1].payload.quad.inverse_x[1]=0.25;
      unsigned uploaded=anygm_test_graphics_texture_data_uploads(map_x);
      int draws=anygm_test_graphics_scene_draw_calls();
      unsigned before_copies=anygm_test_graphics_scene_copies();
      anygm_test_graphics_fail_texture_upload(map_x);
      result=service.submit(service.context,&upload_batch);
      REQUIRE(result.status!=GML_RENDER_EXEC_ACCEPTED &&
              anygm_test_graphics_scene_draw_calls()==draws &&
              anygm_test_graphics_scene_copies()==before_copies &&
              anygm_test_graphics_texture_data_uploads(map_x)==uploaded,
              "failed point-axis replacement refuses before its predecessor or draw");
      anygm_test_graphics_fail_texture_upload(0u);
      result=service.submit(service.context,&upload_batch);
      REQUIRE(result.status==GML_RENDER_EXEC_ACCEPTED && result.accepted_prefix==2u &&
              anygm_test_graphics_texture_data_uploads(map_x)==uploaded+1u,
              "retry uploads the rejected point-axis key instead of reusing stale samples");
    }
    for(unsigned variant=0;variant<(modern?28u:packet_alpha?15u:stamp?14u:12u);variant++){
      GmlRenderDrawStateSnapshot invalid_states[16];
      GmlRenderDrawOp invalid_operations[17];
      memcpy(invalid_states,states,sizeof states);
      memcpy(invalid_operations,operations,sizeof operations);
      GmlRenderDrawBatch invalid_batch={invalid_operations,17u,invalid_states,16u};
      GmlRenderDrawStateSnapshot *state=&invalid_states[14];
      GmlRenderDrawOp *op=&invalid_operations[15];
      if(variant<2u) for(unsigned index=0;index<17u;index++)
        invalid_operations[index].target.image.resource_class=variant?
          GML_RENDER_RESOURCE_APPLICATION:GML_RENDER_RESOURCE_SCREEN;
      if(variant==2u) op->scene_family=GML_RENDER_SCENE_BACKGROUND;
      if(variant==3u) state->samplers[0].filter=GML_RENDER_FILTER_BILINEAR;
      if(variant==4u) state->samplers[0].quantisation=GML_RENDER_QUANTISE_STRETCHED_FLOAT;
      if(variant==5u) state->samplers[0].logical_margin_mask=1u;
      if(variant==6u) state->coverage_policy=GML_RENDER_COVERAGE_ROTATED_VECTOR_ALPHA;
      if(variant==7u) state->rgb_source_factor=GML_RENDER_BLEND_ZERO;
      if(variant==8u) op->reconstruction_mode=GML_RENDER_REPLAY_EXACT;
      if(variant==9u) op->payload.quad.shape=GML_RENDER_QUAD_GENERAL;
      if(variant==10u) state->alpha_test_enabled=1u;
      if(variant==11u) state->write_mask=16u;
      if(variant==12u) op->payload.quad.filtered_draw_alpha=0.0;
      if(variant==13u) op->payload.quad.filtered_draw_alpha=1.1;
      if(variant==14u) op->payload.quad.filtered_draw_alpha=NAN;
      if(variant==15u) op->payload.quad.corner_colour[1]^=1u;
      if(variant==16u) for(unsigned corner=0;corner<4u;corner++)
        op->payload.quad.corner_colour[corner]^=0x01000000u;
      if(variant==17u) op->payload.quad.x[0]=op->payload.quad.x[3]=0.25;
      if(variant==18u) for(unsigned index=0;index<17u;index++)
        invalid_operations[index].target.pixel_format=GML_RENDER_PIXEL_XRGB8888;
      if(variant==19u) op->payload.quad.inverse_x[0]=0.0;
      if(variant==20u) op->payload.quad.inverse_y[1]=NAN;
      if(variant==21u) op->payload.quad.inverse_anchor[0]=0.25;
      if(variant==22u) op->payload.quad.inverse_anchor[1]=INFINITY;
      if(variant==23u) op->payload.quad.inverse_x[1]=-0.25;
      if(variant==24u) op->payload.quad.inverse_y[0]=1.25;
      if(variant==25u) op->payload.quad.inverse_x[0]=-2.0;
      if(variant==26u) op->payload.quad.inverse_y[1]=2049.0;
      if(variant==27u) op->payload.quad.span_active=1u;
      if(stamp && variant>=12u){
        for(unsigned index=0;index<17u;index++)
          invalid_operations[index].target.image.resource_class=GML_RENDER_RESOURCE_SCREEN;
        if(variant==12u) state->alpha_test_reference=256u;
        else if(variant==13u) state->alpha_test_enabled=2u;
        else op->payload.quad.span_active=GML_RENDER_SPAN_FIXED20_ROW;
      }
      op->payload.quad.sampling=state->samplers[0];
      int clears=anygm_test_graphics_clear_calls();
      int draws=anygm_test_graphics_scene_draw_calls();
      copies=anygm_test_graphics_scene_copies();
      result=service.submit(service.context,&invalid_batch);
      if(stamp && (variant<=1u || (packet_alpha && variant==10u))){
        REQUIRE(result.status==GML_RENDER_EXEC_ACCEPTED && result.accepted_prefix==17u &&
                anygm_test_graphics_read_pixels()==0 &&
                anygm_test_graphics_scene_copies()==copies &&
                anygm_test_graphics_color_mask_all() && !anygm_test_graphics_blend_enabled(),
                "nonpreserving axis stamp accepts every mask without a predecessor or read");
        continue;
      }
      REQUIRE(result.status==GML_RENDER_EXEC_UNSUPPORTED && !result.accepted_prefix &&
              !result.uncertain_target_count && !result.all_destinations_uncertain &&
              anygm_test_graphics_clear_calls()==clears &&
              anygm_test_graphics_scene_draw_calls()==draws &&
              anygm_test_graphics_scene_copies()==copies &&
              anygm_test_graphics_color_mask_all() && !anygm_test_graphics_blend_enabled(),
              "unqualified masked suffix refuses before the clear and qualified prefix");
    }
    GmlRenderDrawOp failing_operations[2]={operations[0],operations[8]};
    GmlRenderDrawBatch failing_batch={failing_operations,2u,states,16u};
    anygm_test_graphics_fail_scene_draw_at((unsigned)anygm_test_graphics_scene_draw_calls()+1u);
    result=service.submit(service.context,&failing_batch);
    REQUIRE(result.status==GML_RENDER_EXEC_RECOVERABLE_ERROR && result.accepted_prefix==1u &&
            result.uncertain_target_count==1u &&
            result.uncertain_targets[0].identity==target.identity &&
            anygm_test_graphics_color_mask_all() && !anygm_test_graphics_blend_enabled() &&
            !anygm_test_graphics_attributes_enabled() &&
            anygm_test_graphics_read_pixels()==0,
            "failed masked draw fences its target and restores shared driver state without a read");
    anygm_test_graphics_fail_scene_draw_at(0u);
    result=service.submit(service.context,&batch);
    REQUIRE(result.status==GML_RENDER_EXEC_ACCEPTED && result.accepted_prefix==17u,
            "fresh clear and masked batch recover after a fenced driver failure");
    gml_gpu_destroy(gpu,1);
  }
  return 1;
}

static int scene_screen_filtered_alpha_stamp_case(void){
  const uint8_t pixels[16]={255,0,0,128,0,255,0,0,0,0,255,255,255,255,255,64};
  GmlRenderImageRef source={GML_RENDER_RESOURCE_ATLAS,91u,1u,1u};
  GmlRenderImageRef target={GML_RENDER_RESOURCE_SCREEN,92u,1u,1u};
  GmlRenderImageUpload upload={0};
  GmlRenderDrawStateSnapshot state={0};
  GmlRenderDrawOp operations[3]={0};
  GmlRenderDrawBatch batch={0};
  GmlRenderDeviceServices service;
  GmlGpuContext context;
  GmlGpu *gpu;
  anygm_test_graphics_reset();
  gpu=gml_gpu_create();
  REQUIRE(gpu!=NULL,"screen alpha device allocated");
  context=fake_context(GML_GPU_API_OPENGL_CORE);
  REQUIRE(gml_gpu_context_reset(gpu,&context),"screen alpha context adopted");
  gml_gpu_device_services(gpu,&service);
  upload.image=source;
  upload.storage_width=upload.storage_height=2u;
  upload.region=(GmlRenderRect){0,0,2u,2u};
  upload.pixel_format=GML_RENDER_PIXEL_RGBA8888;
  upload.stride_bytes=8u;
  upload.byte_capacity=sizeof pixels;
  upload.bytes=pixels;
  REQUIRE(service.upload(service.context,&upload),"screen alpha source uploaded");
  state.write_mask=15u;
  state.alpha=255u;
  state.colour=0xffffffffu;
  state.sampler_count=1u;
  state.coverage_policy=GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA;
  state.rgb_source_factor=state.alpha_source_factor=GML_RENDER_BLEND_SOURCE_ALPHA;
  state.rgb_destination_factor=state.alpha_destination_factor=
    GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA;
  state.rgb_equation=state.alpha_equation=GML_RENDER_BLEND_ADD;
  state.samplers[0].filter=GML_RENDER_FILTER_BILINEAR;
  state.samplers[0].edge_rule=GML_RENDER_EDGE_CLAMP_SUBIMAGE;
  state.samplers[0].source_region=upload.region;
  for(unsigned index=0;index<3u;index++){
    GmlRenderDrawOp *op=&operations[index];
    op->sequence=index+1u;
    op->target.image=target;
    op->target.image.content_version=index+1u;
    op->target.storage_version=index+1u;
    op->target.width=op->target.height=8u;
    op->target.pixel_format=GML_RENDER_PIXEL_ARGB8888;
    op->target.region=(GmlRenderRect){0,0,8u,8u};
    if(!index){
      op->opcode=GML_RENDER_OP_CLEAR;
      op->payload.clear_colour=0x40102030u;
      continue;
    }
    op->opcode=GML_RENDER_OP_TEXTURED_QUAD;
    op->scene_family=GML_RENDER_SCENE_BACKGROUND;
    op->reconstruction_mode=GML_RENDER_DEVICE_CHECKPOINT;
    op->sources=&source;
    op->source_count=1u;
    op->payload.quad.shape=GML_RENDER_QUAD_AXIS_ALIGNED;
    op->payload.quad.filtered_draw_alpha=0.6;
    op->payload.quad.sampling=state.samplers[0];
    for(unsigned corner=0;corner<4u;corner++)
      op->payload.quad.corner_colour[corner]=0x99a0c0e0u;
    op->payload.quad.x[0]=op->payload.quad.x[3]=1.0;
    op->payload.quad.x[1]=op->payload.quad.x[2]=3.0;
    op->payload.quad.y[0]=op->payload.quad.y[1]=1.0;
    op->payload.quad.y[2]=op->payload.quad.y[3]=3.0;
    op->payload.quad.u[1]=op->payload.quad.u[2]=1.0;
    op->payload.quad.v[2]=op->payload.quad.v[3]=1.0;
  }
  batch.operations=operations;
  batch.operation_count=3u;
  batch.states=&state;
  batch.state_count=1u;
  GmlRenderExecutionResult result=service.submit(service.context,&batch);
  REQUIRE(result.status==GML_RENDER_EXEC_ACCEPTED && result.accepted_prefix==3u &&
          anygm_test_graphics_scene_draw_calls()==2 &&
          anygm_test_graphics_scene_vertices()==24 &&
          anygm_test_graphics_color_mask_all() && !anygm_test_graphics_blend_enabled() &&
          anygm_test_graphics_read_pixels()==0 && anygm_test_graphics_scene_copies()==0,
          "two ordered filtered quads reuse one RGB draw and one alpha-only draw without copies");
  for(unsigned variant=0;variant<13u;variant++){
    GmlRenderDrawStateSnapshot invalid_state=state;
    GmlRenderDrawOp invalid_operations[3];
    GmlRenderDrawBatch invalid_batch=batch;
    memcpy(invalid_operations,operations,sizeof operations);
    invalid_batch.operations=invalid_operations;
    invalid_batch.states=&invalid_state;
    if(variant==0) invalid_state.coverage_policy=3u;
    if(variant==1) for(unsigned index=0;index<3u;index++)
      invalid_operations[index].target.image.resource_class=GML_RENDER_RESOURCE_SURFACE;
    if(variant==2) invalid_state.samplers[0].filter=GML_RENDER_FILTER_NEAREST;
    if(variant==3) invalid_operations[2].reconstruction_mode=GML_RENDER_REPLAY_EXACT;
    if(variant==4) invalid_operations[2].payload.quad.shape=GML_RENDER_QUAD_GENERAL;
    if(variant==5) invalid_operations[2].scene_family=GML_RENDER_SCENE_TEXT;
    if(variant==6) invalid_state.alpha_source_factor=GML_RENDER_BLEND_ONE;
    if(variant==7) invalid_state.samplers[0].quantisation=GML_RENDER_QUANTISE_STRETCHED_FLOAT;
    if(variant==8) invalid_state.write_mask=7u;
    if(variant==9) invalid_operations[2].target.pixel_format=GML_RENDER_PIXEL_XRGB8888;
    if(variant==10) invalid_operations[2].payload.quad.filtered_draw_alpha=0.0;
    if(variant==11) invalid_operations[2].payload.quad.filtered_draw_alpha=NAN;
    if(variant==12) invalid_operations[2].payload.quad.corner_colour[0]=0x00a0c0e0u;
    if(variant==2 || variant==7) for(unsigned index=1;index<3u;index++)
      invalid_operations[index].payload.quad.sampling=invalid_state.samplers[0];
    int clears=anygm_test_graphics_clear_calls();
    int draws=anygm_test_graphics_scene_draw_calls();
    result=service.submit(service.context,&invalid_batch);
    REQUIRE(result.status==GML_RENDER_EXEC_UNSUPPORTED && result.accepted_prefix==0u &&
            anygm_test_graphics_clear_calls()==clears &&
            anygm_test_graphics_scene_draw_calls()==draws,
            "unqualified screen coverage refuses the complete batch before its clear and prefix");
  }
  state.samplers[0].filter=GML_RENDER_FILTER_NEAREST;
  for(unsigned index=1;index<3u;index++){
    GmlRenderQuad *quad=&operations[index].payload.quad;
    operations[index].scene_family=GML_RENDER_SCENE_SPRITE;
    quad->shape=index==1u?GML_RENDER_QUAD_CARDINAL:GML_RENDER_QUAD_GENERAL;
    quad->sampling=state.samplers[0];
    quad->inverse_anchor[0]=quad->inverse_anchor[1]=1.0;
    quad->inverse_x[0]=quad->inverse_y[1]=1.0;
  }
  int draws=anygm_test_graphics_scene_draw_calls();
  unsigned copies=anygm_test_graphics_scene_copies();
  result=service.submit(service.context,&batch);
  REQUIRE(result.status==GML_RENDER_EXEC_ACCEPTED && result.accepted_prefix==3u &&
          anygm_test_graphics_scene_draw_calls()==draws+2 &&
          anygm_test_graphics_color_mask_all() && !anygm_test_graphics_blend_enabled() &&
          anygm_test_graphics_read_pixels()==0 && anygm_test_graphics_scene_copies()==copies+2u,
          "overlapping cardinal/arbitrary quads retain distinct exact device-local predecessors");
  for(unsigned variant=0;variant<10u;variant++){
    GmlRenderDrawStateSnapshot invalid_state=state;
    GmlRenderDrawOp invalid_operations[3];
    GmlRenderDrawBatch invalid_batch=batch;
    memcpy(invalid_operations,operations,sizeof operations);
    invalid_batch.operations=invalid_operations;
    invalid_batch.states=&invalid_state;
    GmlRenderQuad *quad=&invalid_operations[2].payload.quad;
    if(variant==0) invalid_operations[2].scene_family=GML_RENDER_SCENE_BACKGROUND;
    if(variant==1){
      invalid_state.samplers[0].filter=GML_RENDER_FILTER_BILINEAR;
      for(unsigned index=1;index<3u;index++)
        invalid_operations[index].payload.quad.sampling=invalid_state.samplers[0];
    }
    if(variant==2) quad->filtered_draw_alpha=1.9;
    if(variant==3) quad->inverse_x[0]=NAN;
    if(variant==4) for(unsigned index=0;index<3u;index++)
      invalid_operations[index].target.image.resource_class=GML_RENDER_RESOURCE_APPLICATION;
    if(variant==5) quad->shape=GML_RENDER_QUAD_AXIS_ALIGNED;
    if(variant==6){
      quad->x[1]=quad->x[2]=513.0;
      quad->y[2]=quad->y[3]=513.0;
    }
    if(variant==7) quad->corner_colour[1]=0x00a0c0e0u;
    if(variant==8) invalid_operations[2].reconstruction_mode=GML_RENDER_REPLAY_EXACT;
    if(variant==9) invalid_state.write_mask=7u;
    int clears=anygm_test_graphics_clear_calls();
    draws=anygm_test_graphics_scene_draw_calls();
    result=service.submit(service.context,&invalid_batch);
    REQUIRE(result.status==GML_RENDER_EXEC_UNSUPPORTED && !result.accepted_prefix &&
            anygm_test_graphics_clear_calls()==clears &&
            anygm_test_graphics_scene_draw_calls()==draws,
            "unqualified rotated SCREEN metadata refuses before the clear and valid prefix");
  }
  draws=anygm_test_graphics_scene_draw_calls();
  anygm_test_graphics_fail_scene_draw_at((unsigned)draws+2u);
  result=service.submit(service.context,&batch);
  REQUIRE(result.status==GML_RENDER_EXEC_RECOVERABLE_ERROR &&
          result.accepted_prefix==2u &&
          result.uncertain_target_count==1u &&
          result.uncertain_targets[0].identity==target.identity,
          "failure of the second exact rotated draw fences its partially written target");
  anygm_test_graphics_fail_scene_draw_at(0u);
  state.coverage_policy=GML_RENDER_COVERAGE_ROTATED_VECTOR_ALPHA;
  state.alpha_source_factor=GML_RENDER_BLEND_ONE;
  draws=anygm_test_graphics_scene_draw_calls();
  copies=anygm_test_graphics_scene_copies();
  result=service.submit(service.context,&batch);
  REQUIRE(result.status==GML_RENDER_EXEC_ACCEPTED && result.accepted_prefix==3u &&
          anygm_test_graphics_scene_draw_calls()==draws+2 &&
          anygm_test_graphics_color_mask_all() && !anygm_test_graphics_blend_enabled() &&
          anygm_test_graphics_read_pixels()==0 && anygm_test_graphics_scene_copies()==copies+2u,
          "vector rotated coverage uses exact ordered RGBA blending without a CPU read");
  for(unsigned variant=0;variant<4u;variant++){
    GmlRenderDrawStateSnapshot invalid_state=state;
    GmlRenderDrawOp invalid_operations[3];
    GmlRenderDrawBatch invalid_batch=batch;
    memcpy(invalid_operations,operations,sizeof operations);
    invalid_batch.operations=invalid_operations;
    invalid_batch.states=&invalid_state;
    if(variant==0) invalid_state.alpha_source_factor=GML_RENDER_BLEND_SOURCE_ALPHA;
    if(variant==1) invalid_operations[2].payload.quad.shape=GML_RENDER_QUAD_AXIS_ALIGNED;
    if(variant==2){
      invalid_state.samplers[0].filter=GML_RENDER_FILTER_BILINEAR;
      for(unsigned index=1;index<3u;index++)
        invalid_operations[index].payload.quad.sampling=invalid_state.samplers[0];
    }
    if(variant==3) invalid_state.alpha_destination_factor=GML_RENDER_BLEND_ZERO;
    int clears=anygm_test_graphics_clear_calls();
    draws=anygm_test_graphics_scene_draw_calls();
    result=service.submit(service.context,&invalid_batch);
    REQUIRE(result.status==GML_RENDER_EXEC_UNSUPPORTED && !result.accepted_prefix &&
            anygm_test_graphics_clear_calls()==clears &&
            anygm_test_graphics_scene_draw_calls()==draws,
            "vector coverage rejects a different sampler, geometry or alpha equation before writes");
  }
  GmlRenderDrawOp disjoint[3];
  GmlRenderDrawStateSnapshot mixed_states[2]={state,state};
  GmlRenderDrawBatch mixed_batch=batch;
  memcpy(disjoint,operations,sizeof disjoint);
  mixed_batch.operations=disjoint;
  mixed_batch.states=mixed_states;
  mixed_batch.state_count=2u;
  mixed_states[1].coverage_policy=GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA;
  mixed_states[1].alpha_source_factor=GML_RENDER_BLEND_SOURCE_ALPHA;
  disjoint[2].state_index=1u;
  disjoint[2].target.storage_version=disjoint[1].target.storage_version;
  disjoint[2].payload.quad.x[0]=disjoint[2].payload.quad.x[3]=3.0;
  disjoint[2].payload.quad.x[1]=disjoint[2].payload.quad.x[2]=5.0;
  draws=anygm_test_graphics_scene_draw_calls();
  copies=anygm_test_graphics_scene_copies();
  uint64_t copied_pixels=anygm_test_graphics_scene_copied_pixels();
  result=service.submit(service.context,&mixed_batch);
  REQUIRE(result.status==GML_RENDER_EXEC_ACCEPTED && result.accepted_prefix==3u &&
          anygm_test_graphics_scene_draw_calls()==draws+1 &&
          anygm_test_graphics_scene_copies()==copies+1u &&
          anygm_test_graphics_scene_copied_pixels()==copied_pixels+8u &&
          !anygm_test_graphics_read_pixels(),
          "disjoint scalar/vector regions share one exact eight-pixel GPU-local predecessor");
  gml_gpu_destroy(gpu,1);
  anygm_test_graphics_reset();
  anygm_test_graphics_withhold("glCopyTexSubImage2D");
  gpu=gml_gpu_create();
  REQUIRE(gpu && gml_gpu_context_reset(gpu,&context),"no-copy rotated context adopted");
  gml_gpu_device_services(gpu,&service);
  REQUIRE(service.upload(service.context,&upload),"no-copy source uploaded");
  result=service.submit(service.context,&batch);
  REQUIRE(result.status==GML_RENDER_EXEC_UNSUPPORTED && !result.accepted_prefix &&
          !anygm_test_graphics_clear_calls() && !anygm_test_graphics_scene_draw_calls(),
          "missing exact snapshot service rejects rotated metadata before clear and prefix");
  gml_gpu_destroy(gpu,1);
  return 1;
}

static int scene_integer_function_sprite_case(void){
  for(unsigned background=0;background<3u;background++)
  for(unsigned alpha_test=0;alpha_test<2u;alpha_test++)
  for(unsigned api=0;api<2u;api++) for(unsigned rounded=0;rounded<3u;rounded++)
  for(unsigned target_kind=0;target_kind<4u;target_kind++) for(unsigned blend=0;blend<12u;blend++){
    if(blend>=7u && background!=1u) continue;
    uint8_t pixels[16]={208,32,16,128,16,208,32,255,32,16,208,0,192,176,160,64};
    GmlRenderImageRef source={GML_RENDER_RESOURCE_ATLAS,31u,1u,1u};
    GmlRenderImageRef target={target_kind==0u?GML_RENDER_RESOURCE_SCREEN:
      target_kind==1u?GML_RENDER_RESOURCE_APPLICATION:
      target_kind==2u?GML_RENDER_RESOURCE_SURFACE:GML_RENDER_RESOURCE_SCRATCH,32u,1u,1u};
    GmlRenderImageUpload upload={.image=source,.storage_width=2u,.storage_height=2u,
      .region={0,0,2u,2u},.pixel_format=GML_RENDER_PIXEL_RGBA8888,
      .stride_bytes=8u,.byte_capacity=sizeof pixels,.bytes=pixels};
    GmlRenderDrawStateSnapshot state={0};GmlRenderDrawOp ops[3]={0};
    GmlRenderDrawBatch batch={ops,3u,&state,1u};GmlRenderDeviceServices device;
    GmlGpuContext context=fake_context(api?GML_GPU_API_OPENGLES3:GML_GPU_API_OPENGL_CORE);
    anygm_test_graphics_reset();GmlGpu *gpu=gml_gpu_create();
    REQUIRE(gpu && gml_gpu_context_reset(gpu,&context),"integer sprite adopts context");
    gml_gpu_device_services(gpu,&device);
    REQUIRE(device.upload(device.context,&upload),"integer sprite uploads atlas");
    state.write_mask=15u;state.alpha=255u;state.colour=0xFFFFFFFFu;
    state.alpha_test_enabled=alpha_test;state.alpha_test_reference=127u;
    if(blend==4u) state.coverage_policy=GML_RENDER_COVERAGE_REPLACE_ZERO_ALPHA;
    else if(blend){
      state.rgb_source_factor=GML_RENDER_BLEND_SOURCE_ALPHA;
      state.rgb_destination_factor=state.alpha_destination_factor=GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA;
      state.alpha_source_factor=blend==2u?GML_RENDER_BLEND_SOURCE_ALPHA:GML_RENDER_BLEND_ONE;
      state.rgb_equation=state.alpha_equation=GML_RENDER_BLEND_ADD;
      if(blend==3u){
        state.rgb_source_factor=state.alpha_source_factor=GML_RENDER_BLEND_ZERO;
        state.rgb_destination_factor=GML_RENDER_BLEND_ONE_MINUS_SOURCE_COLOUR;
      }
      if((blend==1u || blend==3u) && target_kind!=2u)
        state.coverage_policy=GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA;
      if(blend==5u || blend==6u){
        int preserve=target_kind==2u,colour=blend==6u;
        state.rgb_source_factor=colour?GML_RENDER_BLEND_ONE_MINUS_DESTINATION_COLOUR:
          GML_RENDER_BLEND_ONE_MINUS_DESTINATION_ALPHA;
        state.rgb_destination_factor=colour?GML_RENDER_BLEND_ZERO:GML_RENDER_BLEND_DESTINATION_ALPHA;
        state.alpha_source_factor=preserve?GML_RENDER_BLEND_ONE_MINUS_DESTINATION_ALPHA:GML_RENDER_BLEND_ONE;
        state.alpha_destination_factor=preserve && !colour?GML_RENDER_BLEND_DESTINATION_ALPHA:GML_RENDER_BLEND_ZERO;
        state.coverage_policy=preserve?0u:GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA;
      }
      if(blend==7u){
        state.rgb_destination_factor=state.alpha_source_factor=state.alpha_destination_factor=GML_RENDER_BLEND_ONE;
        state.coverage_policy=GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA;
      }
      if(blend==8u){
        state.rgb_destination_factor=GML_RENDER_BLEND_ONE_MINUS_SOURCE_COLOUR;
        state.alpha_source_factor=target_kind==2u?GML_RENDER_BLEND_SOURCE_ALPHA:GML_RENDER_BLEND_ONE;
        state.alpha_destination_factor=target_kind==2u?GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA:GML_RENDER_BLEND_ZERO;
        state.coverage_policy=target_kind==2u?0u:GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA;
      }
      if(blend>=9u){
        state.rgb_source_factor=state.alpha_source_factor=blend==11u?
          GML_RENDER_BLEND_DESTINATION_ALPHA:GML_RENDER_BLEND_DESTINATION_COLOUR;
        state.rgb_destination_factor=state.alpha_destination_factor=blend==9u?
          GML_RENDER_BLEND_SOURCE_COLOUR:GML_RENDER_BLEND_DESTINATION_COLOUR;
        state.coverage_policy=target_kind==2u?0u:GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA;
      }
    }
    state.sampler_count=1u;
    state.samplers[0]=(GmlRenderSamplingPolicy){.filter=GML_RENDER_FILTER_NEAREST,
      .edge_rule=GML_RENDER_EDGE_CLAMP_SUBIMAGE,.source_region={0,0,2u,2u},
      .quantisation=rounded==2u?GML_RENDER_QUANTISE_INTEGER_FLAT_RGB:
        rounded?GML_RENDER_QUANTISE_INTEGER_TINT_ROUND:GML_RENDER_QUANTISE_INTEGER_TINT_TRUNC};
    for(unsigned index=0;index<3u;index++){
      GmlRenderDrawOp *op=&ops[index];op->sequence=index+1u;
      op->target=(GmlRenderTargetRef){.image=target,.storage_version=1u,
        .width=8u,.height=8u,.pixel_format=GML_RENDER_PIXEL_ARGB8888,.region={0,0,8u,8u}};
      op->target.image.content_version=index+1u;
      if(!index){op->opcode=GML_RENDER_OP_CLEAR;op->payload.clear_colour=0x40273859u;continue;}
      op->opcode=GML_RENDER_OP_TEXTURED_QUAD;
      op->scene_family=background==2u?GML_RENDER_SCENE_TILE:
        background?GML_RENDER_SCENE_BACKGROUND:GML_RENDER_SCENE_SPRITE;
      op->reconstruction_mode=GML_RENDER_DEVICE_CHECKPOINT;op->sources=&source;op->source_count=1u;
      GmlRenderQuad *q=&op->payload.quad;q->shape=GML_RENDER_QUAD_AXIS_ALIGNED;
      q->sampling=state.samplers[0];q->filtered_draw_alpha=0.4;
      q->x[0]=q->x[3]=-1.0;q->x[1]=q->x[2]=3.0;
      q->y[0]=q->y[1]=1.0;q->y[2]=q->y[3]=3.0;
      q->u[0]=q->u[3]=1.0;q->v[0]=q->v[1]=1.0;
      for(unsigned corner=0;corner<4u;corner++) q->corner_colour[corner]=0x663470D0u;
    }
    GmlRenderExecutionResult result=device.submit(device.context,&batch);
    if((rounded==2u && background) || (blend>=5u && (rounded==2u || background==2u))){
      REQUIRE(result.status==GML_RENDER_EXEC_UNSUPPORTED && !result.accepted_prefix &&
        !anygm_test_graphics_clear_calls() && !anygm_test_graphics_scene_draw_calls() &&
        !anygm_test_graphics_scene_copies() && !anygm_test_graphics_read_pixels(),
        "flat RGB and destination-factor sprite extensions refuse outside their typed scope before a clear prefix");
      gml_gpu_destroy(gpu,1);continue;
    }
    int draws=(blend==1u || blend==3u) && target_kind!=2u?2:1;
    if(blend>=7u){
      draws=anygm_test_graphics_scene_draw_calls();
      REQUIRE(draws>=1 && draws<=2,"background preset needs at most color and coverage passes");
    }
    REQUIRE(result.status==GML_RENDER_EXEC_ACCEPTED && result.accepted_prefix==3u &&
      anygm_test_graphics_scene_draw_calls()==draws && anygm_test_graphics_scene_copies()==0u &&
      anygm_test_graphics_read_pixels()==0u,
      "integer tint and fixed blend group two reflected sprites without a snapshot");
    if(background){
      GmlRenderDrawStateSnapshot original=state;
      for(unsigned extension=0;extension<2u;extension++){
        state=original;
        for(unsigned i=0;i<3u;i++) ops[i].target.image.content_version=i+4u;
        if(!extension) state.write_mask=background==1u && blend>=5u?16u:7u;
        else{
          state.rgb_source_factor=GML_RENDER_BLEND_SOURCE_ALPHA;
          state.rgb_destination_factor=state.alpha_source_factor=
            state.alpha_destination_factor=GML_RENDER_BLEND_ONE;
          state.rgb_equation=state.alpha_equation=GML_RENDER_BLEND_ADD;
          state.coverage_policy=0u;
        }
        result=device.submit(device.context,&batch);
        REQUIRE(result.status==GML_RENDER_EXEC_UNSUPPORTED && !result.accepted_prefix &&
          anygm_test_graphics_scene_draw_calls()==draws && !anygm_test_graphics_scene_copies() &&
          !anygm_test_graphics_read_pixels(),
          "unqualified masks and unstamped additive background state refuse before writes");
      }
      state=original;
    }
    GmlRenderQuad pristine=ops[1].payload.quad;
    for(unsigned invalid=0;invalid<8u;invalid++){
      ops[1].payload.quad=pristine;GmlRenderQuad *q=&ops[1].payload.quad;
      for(unsigned i=0;i<3u;i++) ops[i].target.image.content_version=i+4u;
      if(invalid==0u) q->corner_colour[1]^=1u;
      if(invalid==1u) q->filtered_draw_alpha=0.0;
      if(invalid==2u) q->filtered_draw_alpha=0.5;
      if(invalid==3u) q->u[0]=0.5;
      if(invalid==4u) q->x[0]=q->x[3]=-0.5;
      if(invalid==5u) q->sampling.quantisation=255u;
      if(invalid==6u) q->sampling.filter=GML_RENDER_FILTER_BILINEAR;
      if(invalid==7u) q->span_active=1u;
      result=device.submit(device.context,&batch);
      REQUIRE(result.status==GML_RENDER_EXEC_UNSUPPORTED && !result.accepted_prefix &&
        anygm_test_graphics_scene_draw_calls()==draws && anygm_test_graphics_scene_copies()==0u &&
        anygm_test_graphics_read_pixels()==0u,"invalid integer sprite refuses before the clear prefix");
    }
    /* Prepared nearest sprite intervals also admit fractional scales. Keep
     * their explicit grouped 2.5-scale witness and independent bound refusal. */
    unsigned intervals=background==2u?4u:3u;
    for(unsigned interval=0;interval<intervals;interval++){
      for(unsigned i=0;i<3u;i++) ops[i].target.image.content_version=4u+interval*3u+i;
      for(unsigned i=1u;i<3u;i++){
        ops[i].payload.quad=pristine;GmlRenderQuad *q=&ops[i].payload.quad;
        if(interval==3u){for(unsigned c=0;c<4u;c++){q->u[c]=0.25;q->v[c]=0.75;}}
        else if(interval==1u){for(unsigned c=0;c<4u;c++) q->u[c]=0.25;}
        else if(interval==2u){q->u[1]=q->u[2]=0.2;}
        else{
          q->x[1]=q->x[2]=1.0;q->u[1]=q->u[2]=0.5;
        }
      }
      result=device.submit(device.context,&batch);
      REQUIRE(result.status==GML_RENDER_EXEC_ACCEPTED && result.accepted_prefix==3u &&
        anygm_test_graphics_scene_draw_calls()==draws*(int)(interval+2u) &&
        anygm_test_graphics_scene_copies()==0u && anygm_test_graphics_read_pixels()==0u,
        "phased nearest interval and constant-centre strips remain snapshot-free and grouped");
    }
    for(unsigned invalid=0;invalid<2u;invalid++){
      for(unsigned i=0;i<3u;i++) ops[i].target.image.content_version=10u+i;
      for(unsigned i=1u;i<3u;i++){
        ops[i].payload.quad=pristine;GmlRenderQuad *q=&ops[i].payload.quad;
        if(invalid){q->u[1]=q->u[2]=background==1u?0.99999999:0.99;}
        else{for(unsigned c=0;c<4u;c++) q->u[c]=0.125;}
      }
      result=device.submit(device.context,&batch);
      REQUIRE(result.status==GML_RENDER_EXEC_UNSUPPORTED && !result.accepted_prefix &&
        anygm_test_graphics_scene_draw_calls()==draws*(int)(intervals+1u) && !anygm_test_graphics_scene_copies() &&
        !anygm_test_graphics_read_pixels(),"non-centre constants and out-of-contract inverse scales refuse transactionally");
    }
    GmlRenderDrawStateSnapshot changed_states[2]={state,state};
    batch.states=changed_states;batch.state_count=2u;ops[2].state_index=1u;
    for(unsigned i=1u;i<3u;i++) ops[i].payload.quad=pristine;
    for(unsigned transition=0;transition<2u;transition++){
      for(unsigned i=0;i<3u;i++) ops[i].target.image.content_version=13u+transition*3u+i;
      changed_states[0]=changed_states[1]=state;
      changed_states[0].alpha_test_enabled=1u;
      changed_states[1].alpha_test_enabled=transition?1u:0u;
      changed_states[1].alpha_test_reference=128u;
      int calls=anygm_test_graphics_scene_draw_calls();
      result=device.submit(device.context,&batch);
      REQUIRE(result.status==GML_RENDER_EXEC_ACCEPTED && result.accepted_prefix==3u &&
        anygm_test_graphics_scene_draw_calls()==calls+2*draws &&
        !anygm_test_graphics_scene_copies() && !anygm_test_graphics_read_pixels(),
        "cutout enable or cutoff changes split a compatible batch without reading the target");
    }
    for(unsigned invalid=0;invalid<2u;invalid++){
      changed_states[0]=changed_states[1]=state;
      changed_states[1].alpha_test_enabled=invalid?1u:2u;
      changed_states[1].alpha_test_reference=invalid?256u:127u;
      int calls=anygm_test_graphics_scene_draw_calls();
      result=device.submit(device.context,&batch);
      REQUIRE(result.status==GML_RENDER_EXEC_UNSUPPORTED && !result.accepted_prefix &&
        anygm_test_graphics_scene_draw_calls()==calls && !anygm_test_graphics_scene_copies() &&
        !anygm_test_graphics_read_pixels(),"invalid cutout state rejects the whole batch before its clear");
    }
    batch.states=&state;batch.state_count=1u;ops[2].state_index=0u;
    const double wide_scales[]={8.0,12.0,31.5,64.0};
    for(unsigned wide=0;wide<sizeof wide_scales/sizeof wide_scales[0];wide++){
      for(unsigned i=0;i<3u;i++) ops[i].target.image.content_version=20u+wide*3u+i;
      for(unsigned i=1u;i<3u;i++){
        ops[i].payload.quad=pristine;
        ops[i].payload.quad.x[1]=ops[i].payload.quad.x[2]=-1.0+2.0*wide_scales[wide];
      }
      int calls=anygm_test_graphics_scene_draw_calls();
      result=device.submit(device.context,&batch);
      REQUIRE(result.status==GML_RENDER_EXEC_ACCEPTED && result.accepted_prefix==3u &&
        anygm_test_graphics_scene_draw_calls()==calls+draws &&
        !anygm_test_graphics_scene_copies() && !anygm_test_graphics_read_pixels(),
        "wide nearest intervals through the upper bound retain grouped device execution");
    }
    /* Background magnification is limited by projected extent. Other packet
     * families must not inherit that domain from a shared validator. */
    const double background_scales[]={65.0,207.0,8192.0,16384.0};
    for(unsigned wide=0;wide<sizeof background_scales/sizeof background_scales[0];wide++)
    for(unsigned axis=0;axis<2u;axis++){
      for(unsigned i=0;i<3u;i++) ops[i].target.image.content_version=100u+wide*6u+axis*3u+i;
      for(unsigned i=1u;i<3u;i++){
        ops[i].payload.quad=pristine;GmlRenderQuad *q=&ops[i].payload.quad;
        double extent=background_scales[wide]*(wide==3u?1.0:2.0);
        if(axis){
          q->y[2]=q->y[3]=q->y[0]+extent;
          if(wide==3u) q->v[2]=q->v[3]=0.5;
        }else{
          q->x[1]=q->x[2]=q->x[0]+extent;
          if(wide==3u) q->u[1]=q->u[2]=0.5;
        }
      }
      int calls=anygm_test_graphics_scene_draw_calls(),clears=anygm_test_graphics_clear_calls();
      result=device.submit(device.context,&batch);
      REQUIRE(result.status==(background==1u?GML_RENDER_EXEC_ACCEPTED:GML_RENDER_EXEC_UNSUPPORTED) &&
        result.accepted_prefix==(background==1u?3u:0u) &&
        anygm_test_graphics_scene_draw_calls()==calls+(background==1u?draws:0) &&
        (background==1u || anygm_test_graphics_clear_calls()==clears) &&
        !anygm_test_graphics_scene_copies() && !anygm_test_graphics_read_pixels(),
        "wide background intervals group while sprite and tile bounds refuse before writes");
    }
    for(unsigned i=0;i<3u;i++) ops[i].target.image.content_version=200u+i;
    for(unsigned i=1u;i<3u;i++){
      ops[i].payload.quad=pristine;
      ops[i].payload.quad.x[1]=ops[i].payload.quad.x[2]=pristine.x[0]+16385.0;
    }
    int calls=anygm_test_graphics_scene_draw_calls(),clears=anygm_test_graphics_clear_calls();
    result=device.submit(device.context,&batch);
    REQUIRE(result.status==GML_RENDER_EXEC_UNSUPPORTED && !result.accepted_prefix &&
      anygm_test_graphics_scene_draw_calls()==calls && anygm_test_graphics_clear_calls()==clears &&
      !anygm_test_graphics_scene_copies() && !anygm_test_graphics_read_pixels(),
      "projected extent overflow refuses the whole batch before its clear");
    if(!background && !rounded && blend==4u){
      state.coverage_policy=GML_RENDER_COVERAGE_REPLACE_SOURCE_ALPHA;
      for(unsigned opacity=0;opacity<2u;opacity++){
        for(unsigned i=0;i<3u;i++) ops[i].target.image.content_version=300u+opacity*3u+i;
        for(unsigned i=1u;i<3u;i++){
          ops[i].payload.quad=pristine;
          ops[i].payload.quad.filtered_draw_alpha=opacity?.4:0.0;
          for(unsigned c=0;c<4u;c++)
            ops[i].payload.quad.corner_colour[c]=opacity?0x663470D0u:0x003470D0u;
        }
        int prior=anygm_test_graphics_scene_draw_calls();
        result=device.submit(device.context,&batch);
        REQUIRE(result.status==GML_RENDER_EXEC_ACCEPTED && result.accepted_prefix==3u &&
          anygm_test_graphics_scene_draw_calls()==prior+1 &&
          !anygm_test_graphics_scene_copies() && !anygm_test_graphics_read_pixels(),
          "straight replacement groups zero/full raw coverage without a destination snapshot");
      }
      for(unsigned invalid=0;invalid<4u;invalid++){
        for(unsigned i=0;i<3u;i++) ops[i].target.image.content_version=306u+i;
        for(unsigned i=1u;i<3u;i++){
          ops[i].payload.quad=pristine;ops[i].scene_family=GML_RENDER_SCENE_SPRITE;
          if(invalid==0u) ops[i].scene_family=GML_RENDER_SCENE_TILE;
          if(invalid==1u) ops[i].payload.quad.sampling.quantisation=GML_RENDER_QUANTISE_INTEGER_TINT_ROUND;
          if(invalid==2u) ops[i].payload.quad.filtered_draw_alpha=-.1;
        }
        state.coverage_policy=invalid==3u?6u:GML_RENDER_COVERAGE_REPLACE_SOURCE_ALPHA;
        int prior=anygm_test_graphics_scene_draw_calls(),clear_count=anygm_test_graphics_clear_calls();
        result=device.submit(device.context,&batch);
        REQUIRE(result.status==GML_RENDER_EXEC_UNSUPPORTED && !result.accepted_prefix &&
          anygm_test_graphics_scene_draw_calls()==prior && anygm_test_graphics_clear_calls()==clear_count &&
          !anygm_test_graphics_scene_copies() && !anygm_test_graphics_read_pixels(),
          "unqualified replacement family, tint, alpha or coverage refuses before its clear");
      }
    }
    gml_gpu_destroy(gpu,1);
  }
  return 1;
}

static int scene_raw_copy_case(void){
  const uint32_t classes[]={GML_RENDER_RESOURCE_SCREEN,GML_RENDER_RESOURCE_APPLICATION,
    GML_RENDER_RESOURCE_SURFACE,GML_RENDER_RESOURCE_SCRATCH};
  for(unsigned api=0;api<2u;api++) for(unsigned owner=0;owner<4u;owner++)
  for(unsigned pitched=0;pitched<2u;pitched++) for(unsigned alias=0;alias<2u;alias++){
    uint32_t pixels[64];for(unsigned index=0;index<64u;index++)pixels[index]=0x01345768u+index*0x01010101u;
    GmlGpuContext context=fake_context(api?GML_GPU_API_OPENGLES3:GML_GPU_API_OPENGL_CORE);
    GmlRenderDeviceServices device={0};
    GmlRenderImageRef source={classes[owner],61u,1u,1u};
    GmlRenderImageUpload upload={.image=source,.storage_width=8u,.storage_height=8u,
      .region={0,0,8u,8u},.pixel_format=GML_RENDER_PIXEL_ARGB8888,.stride_bytes=32u,
      .byte_capacity=sizeof pixels,.bytes=pixels};
    GmlRenderDrawOp ops[2]={{0}};GmlRenderDrawStateSnapshot state={0};
    GmlRenderTargetRef target={.image={classes[owner],alias?61u:62u,1u,1u},
      .storage_version=1u,.width=8u,.height=8u,.pixel_format=GML_RENDER_PIXEL_ARGB8888,
      .region={0,0,8u,8u}};
    anygm_test_graphics_reset();GmlGpu *gpu=gml_gpu_create();
    REQUIRE(gpu && gml_gpu_context_reset(gpu,&context),"raw-copy context created");
    gml_gpu_device_services(gpu,&device);
    REQUIRE(device.upload(device.context,&upload),"raw-copy source uploaded");
    ops[0].opcode=GML_RENDER_OP_CLEAR;ops[0].target=target;
    GmlRenderDrawBatch initial={ops,1u,NULL,0u};
    if(!alias)REQUIRE(device.submit(device.context,&initial).status==GML_RENDER_EXEC_ACCEPTED,
      "raw-copy target cleared");
    ops[1]=ops[0];ops[1].opcode=GML_RENDER_OP_COPY_IMAGE;
    ops[1].scene_family=GML_RENDER_SCENE_SURFACE;
    ops[1].reconstruction_mode=GML_RENDER_DEVICE_CHECKPOINT;
    ops[1].source_count=1u;ops[1].sources=&source;ops[1].target.image.content_version=2u;
    ops[1].payload.copy=(GmlRenderCopyRegion){{1,1,5u,3u},{2,1,5u,3u},alias,
      pitched?10u:0u,pitched?9u:0u};
    /* Raw transport must not accidentally apply a sampled draw's colour mask,
     * alpha or cutout. The public caller retains these readable values. */
    state.write_mask=1u;state.alpha_test_enabled=1u;state.alpha_test_reference=255u;
    GmlRenderDrawBatch batch={&ops[1],1u,&state,1u};
    unsigned before=anygm_test_graphics_scene_copies();
    GmlRenderExecutionResult result=device.submit(device.context,&batch);
    unsigned copies=anygm_test_graphics_scene_copies();
    REQUIRE(result.status==GML_RENDER_EXEC_ACCEPTED && result.accepted_prefix==1u &&
      (pitched?copies-before>=2u+alias:copies-before==1u+alias) && !anygm_test_graphics_read_pixels() &&
      !anygm_test_graphics_scene_draw_calls(),"raw-copy blits and one alias snapshot need no draw or CPU read");
    if(alias)source.content_version=2u;
    GmlRenderDrawOp pristine=ops[1];
    for(unsigned invalid=0;invalid<7u;invalid++){
      ops[1]=pristine;ops[1].target.image.content_version=4u;
      ops[0].target.image.content_version=3u;batch.operations=ops;batch.operation_count=2u;
      if(invalid==0u)ops[1].payload.copy.source.width=UINT32_MAX;
      if(invalid==1u)ops[1].payload.copy.destination.height++;
      if(invalid==2u)ops[1].payload.copy.source.x=-1;
      if(invalid==3u)ops[1].payload.copy.source_pitch=1u;
      if(invalid==4u)ops[1].payload.copy.needs_source_snapshot=!alias;
      if(invalid==5u)ops[1].source_count=0u;
      if(invalid==6u)ops[1].scene_family=GML_RENDER_SCENE_SPRITE;
      result=device.submit(device.context,&batch);
      REQUIRE(result.status==GML_RENDER_EXEC_UNSUPPORTED && !result.accepted_prefix &&
        anygm_test_graphics_scene_copies()==copies && !anygm_test_graphics_read_pixels() &&
        !anygm_test_graphics_scene_draw_calls(),"raw-copy invalid suffix refuses before its clear prefix");
    }
    gml_gpu_destroy(gpu,1);
  }
  return 1;
}

static int scene_source_alpha_key_case(void){
  for(unsigned api=0;api<2u;api++) for(unsigned owner=0;owner<4u;owner++)
  for(unsigned family=0;family<2u;family++) for(unsigned rounded=0;rounded<2u;rounded++){
    uint8_t pixels[16]={208,32,16,128,16,208,32,255,32,16,208,0,192,176,160,64};
    GmlGpuContext context=fake_context(api?GML_GPU_API_OPENGLES3:GML_GPU_API_OPENGL_CORE);
    GmlRenderDeviceServices device={0};
    GmlRenderImageRef source={GML_RENDER_RESOURCE_ATLAS,61u,1u,1u};
    GmlRenderImageUpload upload={.image=source,.storage_width=2u,.storage_height=2u,
      .region={0,0,2u,2u},.pixel_format=GML_RENDER_PIXEL_RGBA8888,.stride_bytes=8u,
      .byte_capacity=sizeof pixels,.bytes=pixels};
    GmlRenderDrawOp clear={0},ops[2]={{0}};
    GmlRenderDrawStateSnapshot states[2]={{0}};
    GmlRenderDrawBatch batch={ops,2u,states,2u};
    anygm_test_graphics_reset();GmlGpu *gpu=gml_gpu_create();
    REQUIRE(gpu && gml_gpu_context_reset(gpu,&context),"source-key context created");
    gml_gpu_device_services(gpu,&device);
    REQUIRE(device.upload(device.context,&upload),"source-key atlas uploaded");
    clear.opcode=GML_RENDER_OP_CLEAR;
    clear.target=(GmlRenderTargetRef){.image={owner==0u?GML_RENDER_RESOURCE_SCREEN:
      owner==1u?GML_RENDER_RESOURCE_APPLICATION:owner==2u?GML_RENDER_RESOURCE_SURFACE:
        GML_RENDER_RESOURCE_SCRATCH,62u,1u,1u},.storage_version=1u,
      .width=8u,.height=8u,.pixel_format=GML_RENDER_PIXEL_ARGB8888,.region={0,0,8u,8u}};
    GmlRenderDrawBatch initial={&clear,1u,NULL,0u};
    REQUIRE(device.submit(device.context,&initial).status==GML_RENDER_EXEC_ACCEPTED,"source-key target clear");
    states[0].write_mask=15u;states[0].colour=0xFFFFFFFFu;states[0].alpha=255u;
    states[0].source_alpha_key=(1u<<9)|128u;states[0].sampler_count=1u;
    states[0].samplers[0]=(GmlRenderSamplingPolicy){GML_RENDER_FILTER_NEAREST,
      GML_RENDER_EDGE_CLAMP_SUBIMAGE,{0,0,2u,2u},rounded?GML_RENDER_QUANTISE_INTEGER_TINT_ROUND:
        GML_RENDER_QUANTISE_INTEGER_TINT_TRUNC,0u};
    states[1]=states[0];
    for(unsigned index=0;index<2u;index++){
      ops[index]=clear;ops[index].opcode=GML_RENDER_OP_TEXTURED_QUAD;
      ops[index].scene_family=family?GML_RENDER_SCENE_TILE:GML_RENDER_SCENE_BACKGROUND;
      ops[index].reconstruction_mode=GML_RENDER_DEVICE_CHECKPOINT;
      ops[index].sources=&source;ops[index].source_count=1u;ops[index].state_index=index;
      GmlRenderQuad *q=&ops[index].payload.quad;q->shape=GML_RENDER_QUAD_AXIS_ALIGNED;
      q->sampling=states[0].samplers[0];q->filtered_draw_alpha=1.0;
      q->x[0]=q->x[3]=index*2.0;q->x[1]=q->x[2]=index*2.0+4.0;
      q->y[2]=q->y[3]=4.0;q->u[1]=q->u[2]=q->v[2]=q->v[3]=1.0;
      for(unsigned corner=0;corner<4u;corner++)q->corner_colour[corner]=0xFFFFFFFFu;
    }
    unsigned draws=(unsigned)anygm_test_graphics_scene_draw_calls();
    for(unsigned transition=0;transition<3u;transition++){
      states[0].source_alpha_key=transition==2u?0u:(1u<<9)|128u;
      states[1].source_alpha_key=transition?(2u<<9)|64u:states[0].source_alpha_key;
      for(unsigned index=0;index<2u;index++)ops[index].target.image.content_version=2u+2u*transition+index;
      GmlRenderExecutionResult result=device.submit(device.context,&batch);
      draws+=transition?2u:1u;
      REQUIRE(result.status==GML_RENDER_EXEC_ACCEPTED && result.accepted_prefix==2u &&
        (unsigned)anygm_test_graphics_scene_draw_calls()==draws && !anygm_test_graphics_scene_copies() &&
        !anygm_test_graphics_read_pixels(),"source-key grouping,changes and disabled state stay snapshot-free");
    }
    GmlRenderDrawOp pristine=ops[1];
    const uint32_t invalid_keys[]={1u,(1u<<9)|257u,4u<<9,UINT32_MAX};
    for(unsigned invalid=0;invalid<7u;invalid++){
      ops[0]=clear;ops[0].target.image.content_version=20u;ops[0].state_index=0u;
      ops[1]=pristine;ops[1].target.image.content_version=21u;
      states[0].source_alpha_key=0u;
      states[1].source_alpha_key=invalid<4u?invalid_keys[invalid]:(1u<<9)|128u;
      if(invalid==4u)ops[1].scene_family=GML_RENDER_SCENE_SPRITE;
      if(invalid==5u)ops[1].payload.quad.sampling.filter=GML_RENDER_FILTER_BILINEAR;
      if(invalid==6u)states[0].source_alpha_key=states[1].source_alpha_key;
      GmlRenderExecutionResult result=device.submit(device.context,&batch);
      REQUIRE(result.status==GML_RENDER_EXEC_UNSUPPORTED && !result.accepted_prefix &&
        (unsigned)anygm_test_graphics_scene_draw_calls()==draws && !anygm_test_graphics_scene_copies() &&
        !anygm_test_graphics_read_pixels(),"unsupported source key refuses the complete batch before clear");
    }
    gml_gpu_destroy(gpu,1);
  }
  return 1;
}

static int scene_nearest_rows_case(void){
  for(unsigned api=0;api<2u;api++) for(unsigned kind=0;kind<3u;kind++)
  for(unsigned blend=0;blend<4u;blend++){
    uint8_t pixels[16]={208,32,16,255,16,208,32,0,32,16,208,128,192,176,160,64};
    GmlRenderImageRef source={GML_RENDER_RESOURCE_ATLAS,31u,1u,1u};
    GmlRenderImageRef target={kind==0u?GML_RENDER_RESOURCE_SCREEN:
      kind==1u?GML_RENDER_RESOURCE_APPLICATION:GML_RENDER_RESOURCE_SURFACE,32u,1u,1u};
    GmlRenderImageUpload upload={.image=source,.storage_width=2u,.storage_height=2u,
      .region={0,0,2u,2u},.pixel_format=GML_RENDER_PIXEL_RGBA8888,
      .stride_bytes=8u,.byte_capacity=sizeof pixels,.bytes=pixels};
    GmlRenderDrawStateSnapshot state={.write_mask=15u,.alpha=255u,.colour=0xFFFFFFFFu,
      .alpha_test_enabled=1u,.sampler_count=1u};
    GmlRenderDrawOp ops[3]={0};GmlRenderDrawBatch batch={ops,3u,&state,1u};
    GmlRenderDeviceServices device;
    GmlGpuContext context=fake_context(api?GML_GPU_API_OPENGLES3:GML_GPU_API_OPENGL_CORE);
    anygm_test_graphics_reset();GmlGpu *gpu=gml_gpu_create();
    REQUIRE(gpu && gml_gpu_context_reset(gpu,&context),"nearest rows adopt context");
    gml_gpu_device_services(gpu,&device);REQUIRE(device.upload(device.context,&upload),"nearest rows upload atlas");
    state.samplers[0]=(GmlRenderSamplingPolicy){.filter=GML_RENDER_FILTER_NEAREST,
      .edge_rule=GML_RENDER_EDGE_CLAMP_SUBIMAGE,.source_region={0,0,2u,2u},
      .quantisation=GML_RENDER_QUANTISE_INTEGER_ROW_TINT_TRUNC};
    if(blend){
      state.rgb_source_factor=GML_RENDER_BLEND_SOURCE_ALPHA;
      state.rgb_destination_factor=state.alpha_destination_factor=GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA;
      state.alpha_source_factor=GML_RENDER_BLEND_SOURCE_ALPHA;
      state.rgb_equation=state.alpha_equation=GML_RENDER_BLEND_ADD;
      if(blend==2u){
        state.rgb_source_factor=state.alpha_source_factor=GML_RENDER_BLEND_ZERO;
        state.rgb_destination_factor=GML_RENDER_BLEND_ONE_MINUS_SOURCE_COLOUR;
      }else if(blend==3u){
        state.rgb_destination_factor=state.alpha_source_factor=state.alpha_destination_factor=GML_RENDER_BLEND_ONE;
        state.coverage_policy=GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA;
      }
    }
    for(unsigned i=0;i<3u;i++){
      GmlRenderDrawOp *op=&ops[i];op->sequence=i+1u;
      op->target=(GmlRenderTargetRef){.image=target,.storage_version=1u,.width=8u,.height=8u,
        .pixel_format=GML_RENDER_PIXEL_ARGB8888,.region={0,0,8u,8u}};
      op->target.image.content_version=i+1u;
      if(!i){op->opcode=GML_RENDER_OP_CLEAR;op->payload.clear_colour=0x40273859u;continue;}
      op->opcode=GML_RENDER_OP_TEXTURED_QUAD;op->scene_family=GML_RENDER_SCENE_SPRITE;
      op->reconstruction_mode=GML_RENDER_DEVICE_CHECKPOINT;op->sources=&source;op->source_count=1u;
      GmlRenderQuad *q=&op->payload.quad;q->shape=GML_RENDER_QUAD_AXIS_ALIGNED;
      q->sampling=state.samplers[0];q->filtered_draw_alpha=.4;
      q->x[0]=q->x[3]=1;q->x[1]=q->x[2]=5;
      q->y[0]=q->y[1]=i;q->y[2]=q->y[3]=i+1u;
      q->u[0]=q->u[3]=q->v[0]=q->v[3]=.125;
      q->u[1]=q->u[2]=q->v[1]=q->v[2]=.875;
      for(unsigned c=0;c<4u;c++)q->corner_colour[c]=0x663470D0u;
    }
    GmlRenderExecutionResult result=device.submit(device.context,&batch);
    int calls=blend==3u?2:1;
    REQUIRE(result.status==GML_RENDER_EXEC_ACCEPTED && result.accepted_prefix==3u &&
      anygm_test_graphics_scene_draw_calls()==calls && !anygm_test_graphics_scene_copies() &&
      !anygm_test_graphics_read_pixels(),"two source axes share a bounded height-one nearest batch");
    GmlRenderDrawOp pristine=ops[1];
    for(unsigned invalid=0;invalid<10u;invalid++){
      ops[1]=pristine;GmlRenderQuad *q=&ops[1].payload.quad;
      for(unsigned i=0;i<3u;i++)ops[i].target.image.content_version=4u+i;
      state.alpha_test_enabled=1u;
      if(invalid==0u)q->y[2]=q->y[3]=q->y[0]+2;
      if(invalid==1u)q->v[3]+=.01;
      if(invalid==2u)ops[1].scene_family=GML_RENDER_SCENE_BACKGROUND;
      if(invalid==3u)state.alpha_test_enabled=0u;
      if(invalid==4u)q->corner_colour[1]^=1u;
      if(invalid==5u)q->sampling.quantisation=GML_RENDER_QUANTISE_INTEGER_TINT_TRUNC;
      if(invalid==6u)q->sampling.filter=GML_RENDER_FILTER_BILINEAR;
      if(invalid==7u)q->u[0]=q->u[3]=-.01;
      if(invalid==8u)q->x[1]=q->x[2]=q->x[0]+4097;
      if(invalid==9u)for(unsigned c=0;c<4u;c++)q->v[c]=.125;
      int clears=anygm_test_graphics_clear_calls();
      result=device.submit(device.context,&batch);
      REQUIRE(result.status==GML_RENDER_EXEC_UNSUPPORTED && !result.accepted_prefix &&
        anygm_test_graphics_scene_draw_calls()==calls && anygm_test_graphics_clear_calls()==clears &&
        !anygm_test_graphics_scene_copies() && !anygm_test_graphics_read_pixels(),
        "invalid row family, geometry, coverage or sampling refuses before the clear prefix");
    }
    gml_gpu_destroy(gpu,1);
  }
  return 1;
}

static int scene_opaque_target_sourceover_api(GmlGpuApi api,int numbered,unsigned phase){
  uint8_t pixels[16]={208,32,16,128,16,208,32,255,32,16,208,0,192,176,160,64};
  int source_endpoint=numbered==3;
  int endpoint=numbered==2 || source_endpoint;
  if(endpoint){pixels[3]=255;pixels[15]=255;}
  if(source_endpoint) for(unsigned pixel=0;pixel<4u;pixel++)
    if(pixels[pixel*4u+3u]) for(unsigned channel=0;channel<3u;channel++)
      pixels[pixel*4u+channel]=((pixel+channel)&1u)?255u:0u;
  GmlRenderImageRef source={GML_RENDER_RESOURCE_ATLAS,31u,1u,1u};
  GmlRenderImageRef target={GML_RENDER_RESOURCE_SCREEN,32u,1u,1u};
  if(numbered) target.resource_class=GML_RENDER_RESOURCE_SURFACE;
  GmlRenderImageUpload upload={.image=source,.storage_width=2u,.storage_height=2u,
    .region={0,0,2u,2u},.pixel_format=GML_RENDER_PIXEL_RGBA8888,
    .stride_bytes=8u,.byte_capacity=sizeof pixels,.bytes=pixels};
  GmlRenderDrawStateSnapshot state={0};
  GmlRenderDrawOp operations[3]={0};
  GmlRenderDrawBatch batch={.operations=operations,.operation_count=3u,
    .states=&state,.state_count=1u};
  GmlGpu *gpu;
  GmlGpuContext context=fake_context(api);
  GmlRenderDeviceServices device;
  anygm_test_graphics_reset();
  gpu=gml_gpu_create();
  REQUIRE(gpu && gml_gpu_context_reset(gpu,&context),"opaque-target source-over adopts context");
  gml_gpu_device_services(gpu,&device);
  REQUIRE(device.upload(device.context,&upload),"opaque-target source-over uploads RGBA atlas");
  state.write_mask=15u;state.alpha=255u;state.colour=0xFFFFFFFFu;
  state.rgb_source_factor=GML_RENDER_BLEND_SOURCE_ALPHA;
  state.rgb_destination_factor=GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA;
  state.alpha_source_factor=numbered && !endpoint?GML_RENDER_BLEND_SOURCE_ALPHA:GML_RENDER_BLEND_ONE;
  state.alpha_destination_factor=GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA;
  state.rgb_equation=state.alpha_equation=GML_RENDER_BLEND_ADD;
  state.coverage_policy=numbered?0u:GML_RENDER_COVERAGE_OPAQUE_TARGET;
  state.sampler_count=1u;
  state.samplers[0]=(GmlRenderSamplingPolicy){.filter=GML_RENDER_FILTER_NEAREST,
    .edge_rule=GML_RENDER_EDGE_CLAMP_SUBIMAGE,.source_region={0,0,2u,2u}};
  for(unsigned index=0;index<3u;index++){
    GmlRenderDrawOp *op=&operations[index];
    op->sequence=index+1u;
    op->target=(GmlRenderTargetRef){.image=target,.storage_version=1u,
      .width=8u,.height=8u,.pixel_format=GML_RENDER_PIXEL_ARGB8888,.region={0,0,8u,8u}};
    op->target.image.content_version=index+1u;
    if(!index){
      op->opcode=GML_RENDER_OP_CLEAR;
      op->payload.clear_colour=numbered?0x40123456u:0xFF123456u;
      continue;
    }
    op->opcode=GML_RENDER_OP_TEXTURED_QUAD;
    op->scene_family=GML_RENDER_SCENE_SPRITE;
    op->reconstruction_mode=GML_RENDER_DEVICE_CHECKPOINT;
    op->source_count=1u;op->sources=&source;
    op->payload.quad.shape=GML_RENDER_QUAD_AXIS_ALIGNED;
    op->payload.quad.sampling=state.samplers[0];
    for(unsigned corner=0;corner<4u;corner++) op->payload.quad.corner_colour[corner]=0x80FFFFFFu;
    op->payload.quad.x[0]=op->payload.quad.x[3]=1.0;
    op->payload.quad.x[1]=op->payload.quad.x[2]=3.0;
    op->payload.quad.y[0]=op->payload.quad.y[1]=1.0;
    op->payload.quad.y[2]=op->payload.quad.y[3]=3.0;
    op->payload.quad.u[1]=op->payload.quad.u[2]=1.0;
    op->payload.quad.v[2]=op->payload.quad.v[3]=1.0;
    if(phase){
      for(unsigned corner=0;corner<4u;corner++){
        op->payload.quad.u[corner]=phase==1u?
          ((corner==1u || corner==2u)?0.875:0.125):0.25;
        op->payload.quad.v[corner]=corner>=2u?0.875:0.125;
      }
    }
    if(endpoint){
      op->payload.quad.source_endpoint_rgb_mask=source_endpoint?7u:0u;
      for(unsigned corner=0;corner<4u;corner++){
        op->payload.quad.corner_colour[corner]=index==1u?0xFFFF00FFu:0xFF00FFFFu;
        if(source_endpoint) op->payload.quad.corner_colour[corner]=index==1u?0xFF123456u:0xFFFE8001u;
        op->payload.quad.x[corner]=(corner==1u || corner==2u)?(phase?3.0:5.0):(phase?-1.0:1.0);
        op->payload.quad.y[corner]=corner>=2u?(phase==2u?3.0:5.0):(phase==2u?-1.0:1.0);
        op->payload.quad.u[corner]=(corner==1u || corner==2u)?1.0:0.0;
        op->payload.quad.v[corner]=corner>=2u?1.0:0.0;
      }
    }
  }
  GmlRenderExecutionResult result=device.submit(device.context,&batch);
  unsigned factors[4]={0};
  anygm_test_graphics_blend_factors(factors);
  REQUIRE(result.status==GML_RENDER_EXEC_ACCEPTED && result.accepted_prefix==3u &&
          anygm_test_graphics_scene_draw_calls()==1u &&
          anygm_test_graphics_scene_vertices()==12u &&
          anygm_test_graphics_scene_copies()==0u && anygm_test_graphics_read_pixels()==0u &&
          anygm_test_graphics_blend_configurations()==(numbered && !endpoint?0u:1u) &&
          factors[0]==GL_SRC_ALPHA && factors[1]==GL_ONE_MINUS_SRC_ALPHA &&
          factors[2]==(numbered && !endpoint?GL_SRC_ALPHA:GL_ONE) && factors[3]==GL_ONE_MINUS_SRC_ALPHA,
          "two resident alpha sprites use one fixed-function batch without destination copies or reads");
  for(unsigned variant=0;!numbered && variant<8u;variant++){
    GmlRenderDrawStateSnapshot prior=state;
    GmlRenderQuad prior_quad=operations[1].payload.quad;
    if(variant==0u) state.coverage_policy=0u;
    if(variant==1u) state.coverage_policy=UINT8_MAX;
    if(variant==2u) state.alpha_source_factor=GML_RENDER_BLEND_SOURCE_ALPHA;
    if(variant==3u) state.write_mask=7u;
    if(variant==4u) operations[1].payload.quad.corner_colour[0]=0x80FF00FFu;
    if(variant==5u){
      operations[1].payload.quad.x[0]=operations[1].payload.quad.x[3]=1.25;
      operations[1].payload.quad.x[1]=operations[1].payload.quad.x[2]=3.25;
    }
    if(variant==6u) operations[1].payload.quad.u[0]=-0.01;
    if(variant==7u) operations[1].payload.quad.v[2]=1.01;
    int clears=anygm_test_graphics_clear_calls();
    int draws=anygm_test_graphics_scene_draw_calls();
    result=device.submit(device.context,&batch);
    REQUIRE(result.status==GML_RENDER_EXEC_UNSUPPORTED && !result.accepted_prefix &&
            anygm_test_graphics_clear_calls()==clears &&
            anygm_test_graphics_scene_draw_calls()==draws,
            "uncertified, malformed, squared-alpha, masked, tinted or fractional screen packets refuse before writes");
    state=prior;operations[1].payload.quad=prior_quad;
  }
  for(unsigned variant=0;endpoint && variant<(source_endpoint?11u:7u);variant++){
    GmlRenderDrawOp prior=operations[1];
    if(variant==0u) operations[1].payload.quad.corner_colour[0]=0xFFFF00FEu;
    if(variant==1u) for(unsigned corner=0;corner<4u;corner++) operations[1].payload.quad.corner_colour[corner]=0x80FF00FFu;
    if(variant==2u) operations[1].payload.quad.u[0]=0.01;
    if(variant==3u) operations[1].payload.quad.x[0]=operations[1].payload.quad.x[3]=-1.25;
    if(variant==4u) operations[1].target.image.resource_class=GML_RENDER_RESOURCE_SCREEN;
    if(variant==5u) operations[1].payload.quad.x[1]=operations[1].payload.quad.x[2]=operations[1].payload.quad.x[0]+10;
    if(variant==6u) operations[1].reconstruction_mode=GML_RENDER_REPLAY_EXACT;
    if(variant==7u) operations[1].payload.quad.source_endpoint_rgb_mask=0u;
    if(variant==8u) operations[1].payload.quad.source_endpoint_rgb_mask=6u;
    if(variant==9u) operations[1].payload.quad.source_endpoint_rgb_mask=8u;
    if(variant==10u) operations[1].payload.quad.source_endpoint_rgb_mask=UINT32_MAX;
    int clears=anygm_test_graphics_clear_calls(),draws=anygm_test_graphics_scene_draw_calls();
    result=device.submit(device.context,&batch);
    REQUIRE(result.status==GML_RENDER_EXEC_UNSUPPORTED && !result.accepted_prefix &&
            anygm_test_graphics_clear_calls()==clears && anygm_test_graphics_scene_draw_calls()==draws,
            "unqualified endpoint tint, alpha, phase, extent, target or replay refuses before writes");
    operations[1]=prior;
  }
  gml_gpu_destroy(gpu,1);
  return 1;
}

static int scene_gui_tinted_text_case(void){
  for(unsigned api=0;api<2u;api++){
    const uint8_t pixels[16]={208,32,16,128,16,208,32,255,32,16,208,0,192,176,160,64};
    GmlRenderImageRef source={GML_RENDER_RESOURCE_ATLAS,31u,1u,1u};
    GmlRenderImageRef target={GML_RENDER_RESOURCE_SCREEN,32u,1u,1u};
    GmlRenderImageUpload upload={.image=source,.storage_width=2u,.storage_height=2u,
      .region={0,0,2u,2u},.pixel_format=GML_RENDER_PIXEL_RGBA8888,
      .stride_bytes=8u,.byte_capacity=sizeof pixels,.bytes=pixels};
    GmlRenderDrawStateSnapshot state={0};
    GmlRenderDrawOp ops[3]={0};
    GmlRenderDrawBatch batch={ops,3u,&state,1u};
    GmlRenderDeviceServices device;
    GmlGpuContext context=fake_context(api?GML_GPU_API_OPENGLES3:GML_GPU_API_OPENGL_CORE);
    anygm_test_graphics_reset();
    GmlGpu *gpu=gml_gpu_create();
    REQUIRE(gpu && gml_gpu_context_reset(gpu,&context),"tinted GUI text adopts context");
    gml_gpu_device_services(gpu,&device);
    REQUIRE(device.upload(device.context,&upload),"tinted GUI text uploads authored atlas");
    state.write_mask=15u;state.alpha=255u;state.colour=0xFFFFFFFFu;
    state.rgb_source_factor=GML_RENDER_BLEND_SOURCE_ALPHA;
    state.alpha_source_factor=GML_RENDER_BLEND_ONE;
    state.rgb_destination_factor=state.alpha_destination_factor=GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA;
    state.rgb_equation=state.alpha_equation=GML_RENDER_BLEND_ADD;
    state.coverage_policy=GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA;
    state.sampler_count=1u;
    state.samplers[0]=(GmlRenderSamplingPolicy){.filter=GML_RENDER_FILTER_NEAREST,
      .edge_rule=GML_RENDER_EDGE_CLAMP_SUBIMAGE,.source_region={0,0,2u,2u}};
    for(unsigned index=0;index<3u;index++){
      GmlRenderDrawOp *op=&ops[index];
      op->sequence=index+1u;
      op->target=(GmlRenderTargetRef){.image=target,.storage_version=1u,
        .width=8u,.height=8u,.pixel_format=GML_RENDER_PIXEL_ARGB8888,.region={0,0,8u,8u}};
      op->target.image.content_version=index+1u;
      if(!index){op->opcode=GML_RENDER_OP_CLEAR;op->payload.clear_colour=0x40123456u;continue;}
      op->opcode=GML_RENDER_OP_TEXTURED_QUAD;op->scene_family=GML_RENDER_SCENE_TEXT;
      op->reconstruction_mode=GML_RENDER_DEVICE_CHECKPOINT;op->sources=&source;op->source_count=1u;
      GmlRenderQuad *quad=&op->payload.quad;
      quad->shape=GML_RENDER_QUAD_AXIS_ALIGNED;quad->sampling=state.samplers[0];
      quad->filtered_draw_alpha=1.0;
      quad->x[0]=quad->x[3]=index;quad->x[1]=quad->x[2]=index+2.0;
      quad->y[0]=quad->y[1]=1.0;quad->y[2]=quad->y[3]=3.0;
      quad->u[1]=quad->u[2]=quad->v[2]=quad->v[3]=1.0;
      for(unsigned corner=0;corner<4u;corner++)
        quad->corner_colour[corner]=index==1u?0xFF563412u:0xFFEDA253u;
    }
    GmlRenderExecutionResult result=device.submit(device.context,&batch);
    REQUIRE(result.status==GML_RENDER_EXEC_ACCEPTED && result.accepted_prefix==3u &&
            anygm_test_graphics_scene_draw_calls()==2u &&
            anygm_test_graphics_scene_vertices()==24u &&
            !anygm_test_graphics_scene_copies() && !anygm_test_graphics_read_pixels(),
            "different uniform text tints batch RGB and alpha without predecessor reads");
    for(unsigned variant=0;variant<4u;variant++){
      GmlRenderDrawOp prior=ops[1];
      if(variant==0u) ops[1].scene_family=GML_RENDER_SCENE_SPRITE;
      if(variant==1u) ops[1].payload.quad.corner_colour[0]^=1u;
      if(variant==2u) ops[1].payload.quad.filtered_draw_alpha=0.5;
      if(variant==3u) ops[1].payload.quad.x[1]=ops[1].payload.quad.x[2]=4.0;
      int clears=anygm_test_graphics_clear_calls(),draws=anygm_test_graphics_scene_draw_calls();
      result=device.submit(device.context,&batch);
      REQUIRE(result.status==GML_RENDER_EXEC_UNSUPPORTED && !result.accepted_prefix &&
              anygm_test_graphics_clear_calls()==clears && anygm_test_graphics_scene_draw_calls()==draws,
              "tinted sprite, gradient, partial alpha or scaled GUI packet rejects before clear");
      ops[1]=prior;
    }
    gml_gpu_destroy(gpu,1);
  }
  return 1;
}

static int scene_opaque_target_sourceover_case(void){
  for(unsigned phase=0;phase<3u;phase++)
    if(!scene_opaque_target_sourceover_api(GML_GPU_API_OPENGLES3,0,phase) ||
       !scene_opaque_target_sourceover_api(GML_GPU_API_OPENGL_CORE,0,phase)) return 0;
  return 1;
}

static int scene_gui_half_point_case(void){
  for(unsigned api=0;api<2u;api++)
  for(unsigned cropped=0;cropped<2u;cropped++){
    uint8_t pixels[8u*6u*4u];memset(pixels,128,sizeof pixels);
    unsigned width=cropped?8u:4u,height=cropped?6u:4u;
    GmlRenderImageRef source={GML_RENDER_RESOURCE_ATLAS,35u,1u,1u};
    GmlRenderImageRef target={GML_RENDER_RESOURCE_SCREEN,36u,1u,1u};
    GmlRenderImageUpload upload={.image=source,.storage_width=width,.storage_height=height,
      .region={0,0,width,height},.pixel_format=GML_RENDER_PIXEL_RGBA8888,
      .stride_bytes=width*4u,.byte_capacity=width*height*4u,.bytes=pixels};
    GmlRenderDrawStateSnapshot state={0};
    GmlRenderDrawOp ops[3]={0};
    GmlRenderDrawBatch batch={ops,3u,&state,1u};
    GmlRenderDeviceServices device;
    GmlGpuContext context=fake_context(api?GML_GPU_API_OPENGLES3:GML_GPU_API_OPENGL_CORE);
    anygm_test_graphics_reset();GmlGpu *gpu=gml_gpu_create();
    REQUIRE(gpu && gml_gpu_context_reset(gpu,&context),"half GUI point adopts context");
    gml_gpu_device_services(gpu,&device);
    REQUIRE(device.upload(device.context,&upload),
      "half GUI immutable source uploads");
    state.write_mask=15u;state.alpha=255u;state.colour=0xFFFFFFFFu;
    state.rgb_source_factor=GML_RENDER_BLEND_SOURCE_ALPHA;
    state.alpha_source_factor=GML_RENDER_BLEND_ONE;
    state.rgb_destination_factor=state.alpha_destination_factor=GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA;
    state.rgb_equation=state.alpha_equation=GML_RENDER_BLEND_ADD;
    state.coverage_policy=GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA;
    state.sampler_count=1u;
    state.samplers[0]=(GmlRenderSamplingPolicy){.filter=GML_RENDER_FILTER_NEAREST,
      .edge_rule=GML_RENDER_EDGE_CLAMP_SUBIMAGE,
      .source_region={cropped?2:0,cropped?1:0,4,4}};
    for(unsigned index=0;index<3u;index++){
      GmlRenderDrawOp *op=&ops[index];op->sequence=index+1u;
      op->target=(GmlRenderTargetRef){.image=target,.storage_version=1u,
        .width=8u,.height=8u,.pixel_format=GML_RENDER_PIXEL_ARGB8888,.region={0,0,8,8}};
      op->target.image.content_version=index+1u;
      if(!index){op->opcode=GML_RENDER_OP_CLEAR;op->payload.clear_colour=0x40123456u;continue;}
      op->opcode=GML_RENDER_OP_TEXTURED_QUAD;op->scene_family=GML_RENDER_SCENE_SPRITE;
      op->reconstruction_mode=GML_RENDER_DEVICE_CHECKPOINT;op->sources=&source;op->source_count=1u;
      GmlRenderQuad *q=&op->payload.quad;q->shape=GML_RENDER_QUAD_AXIS_ALIGNED;
      q->sampling=state.samplers[0];q->filtered_draw_alpha=index==1u?0x1p-16:0.75;
      q->x[0]=q->x[3]=index;q->x[1]=q->x[2]=index+2.0;
      q->y[0]=q->y[1]=1.0;q->y[2]=q->y[3]=3.0;
      for(unsigned c=0;c<4u;c++){
        q->u[c]=(q->sampling.source_region.x+0.5+((c==1u || c==2u)?4.0:0.0))/width;
        q->v[c]=(q->sampling.source_region.y+0.5+(c>=2u?4.0:0.0))/height;
        q->corner_colour[c]=((uint32_t)lround(q->filtered_draw_alpha*255.0)<<24)|0xFFFFFFu;
      }
    }
    GmlRenderExecutionResult result=device.submit(device.context,&batch);
    REQUIRE(result.status==GML_RENDER_EXEC_ACCEPTED && result.accepted_prefix==3u &&
      anygm_test_graphics_scene_draw_calls()==2u && anygm_test_graphics_scene_vertices()==24u &&
      !anygm_test_graphics_scene_copies() && !anygm_test_graphics_read_pixels(),
      "half GUI point extrapolated atlas vertices batch without destination copies");
    for(unsigned variant=0;variant<8u;variant++){
      GmlRenderDrawOp prior=ops[1];
      if(variant==0u) ops[1].payload.quad.corner_colour[0]^=1u;
      if(variant==1u) ops[1].payload.quad.u[0]+=0.001;
      if(variant==2u) ops[1].payload.quad.sampling.source_region.width=3u;
      if(variant==3u) ops[1].payload.quad.filtered_draw_alpha=0;
      if(variant==4u) ops[1].payload.quad.x[1]=ops[1].payload.quad.x[2]=4.0;
      if(variant==5u) ops[1].reconstruction_mode=GML_RENDER_REPLAY_EXACT;
      if(variant==6u) ops[1].scene_family=GML_RENDER_SCENE_TEXT;
      if(variant==7u) ops[1].payload.quad.span_active=1u;
      int clears=anygm_test_graphics_clear_calls(),draws=anygm_test_graphics_scene_draw_calls();
      result=device.submit(device.context,&batch);
      REQUIRE(result.status==GML_RENDER_EXEC_UNSUPPORTED && !result.accepted_prefix &&
        anygm_test_graphics_clear_calls()==clears && anygm_test_graphics_scene_draw_calls()==draws,
        "unqualified half GUI tint, phase, extent, alpha, replay or family rejects transactionally");
      ops[1]=prior;
    }
    gml_gpu_destroy(gpu,1);
  }
  return 1;
}

static int scene_numbered_integer_sourceover_case(void){
  for(unsigned phase=0;phase<3u;phase++)
    if(!scene_opaque_target_sourceover_api(GML_GPU_API_OPENGLES3,1,phase) ||
       !scene_opaque_target_sourceover_api(GML_GPU_API_OPENGL_CORE,1,phase)) return 0;
  return 1;
}

static int scene_integer_endpoint_sprite_case(void){
  for(unsigned phase=0;phase<3u;phase++)
    if(!scene_opaque_target_sourceover_api(GML_GPU_API_OPENGLES3,2,phase) ||
       !scene_opaque_target_sourceover_api(GML_GPU_API_OPENGL_CORE,2,phase)) return 0;
  return 1;
}

static int scene_endpoint_source_sprite_case(void){
  for(unsigned phase=0;phase<3u;phase++)
    if(!scene_opaque_target_sourceover_api(GML_GPU_API_OPENGLES3,3,phase) ||
       !scene_opaque_target_sourceover_api(GML_GPU_API_OPENGL_CORE,3,phase)) return 0;
  return 1;
}

static int scene_alpha_composition_api(GmlGpuApi api,const char *renderer,int font_flush,
                                       int split_font){
  uint32_t source_pixels[4]={0x80FF0000u,0x0000FF00u,0xFF0000FFu,0x40FFFFFFu};
  GmlRenderImageRef source={GML_RENDER_RESOURCE_ATLAS,8u,1u,1u};
  GmlRenderImageRef target={GML_RENDER_RESOURCE_SURFACE,9u,1u,1u};
  GmlRenderImageUpload upload={0};
  GmlRenderDrawStateSnapshot state={0};
  GmlRenderDrawOp operations[3]={0};
  GmlRenderDrawBatch batch={0};
  GmlRenderDeviceServices service;
  GmlRenderExecutionResult result;
  GmlGpu *gpu;
  GmlGpuContext context;
  anygm_test_graphics_reset();
  anygm_test_graphics_renderer(renderer);
  if(split_font)
    anygm_test_graphics_fail_compile_containing("#define GML_EXACT_FONT_MODE -1");
  gpu=gml_gpu_create();
  REQUIRE(gpu!=NULL,"device metadata allocated");
  context=fake_context(api);
  REQUIRE(gml_gpu_context_reset(gpu,&context),"context adopted");
  gml_gpu_device_services(gpu,&service);
  upload.image=source;
  upload.storage_width=upload.storage_height=2u;
  upload.region=(GmlRenderRect){0,0,2u,2u};
  upload.pixel_format=GML_RENDER_PIXEL_ARGB8888;
  upload.stride_bytes=8u;
  upload.byte_capacity=sizeof source_pixels;
  upload.bytes=source_pixels;
  REQUIRE(service.upload(service.context,&upload),"alpha atlas uploaded");
  state.write_mask=15u;
  state.alpha=255u;
  state.colour=0xFFFFFFFFu;
  state.sampler_count=1u;
  state.rgb_source_factor=GML_RENDER_BLEND_SOURCE_ALPHA;
  state.rgb_destination_factor=GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA;
  state.alpha_source_factor=GML_RENDER_BLEND_ONE;
  state.alpha_destination_factor=GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA;
  state.rgb_equation=state.alpha_equation=GML_RENDER_BLEND_ADD;
  state.samplers[0].filter=GML_RENDER_FILTER_NEAREST;
  state.samplers[0].edge_rule=GML_RENDER_EDGE_CLAMP_SUBIMAGE;
  state.samplers[0].source_region=(GmlRenderRect){0,0,2u,2u};
  for(uint32_t index=0;index<3u;index++){
    GmlRenderDrawOp *op=&operations[index];
    op->sequence=index+1u;
    op->target.image=target;
    op->target.image.content_version=index+1u;
    op->target.storage_version=index+1u;
    op->target.width=op->target.height=8u;
    op->target.pixel_format=GML_RENDER_PIXEL_ARGB8888;
    op->target.region=(GmlRenderRect){0,0,8u,8u};
    if(index==0u){
      op->opcode=GML_RENDER_OP_CLEAR;
      op->payload.clear_colour=0x00000000u;
      continue;
    }
    op->opcode=GML_RENDER_OP_TEXTURED_QUAD;
    op->source_count=1u;
    op->sources=&source;
    op->payload.quad.shape=GML_RENDER_QUAD_AXIS_ALIGNED;
    op->payload.quad.sampling=state.samplers[0];
    for(uint32_t corner=0;corner<4u;corner++)
      op->payload.quad.corner_colour[corner]=0xFFFFFFFFu;
    op->payload.quad.x[0]=op->payload.quad.x[3]=1.0;
    op->payload.quad.x[1]=op->payload.quad.x[2]=3.0;
    op->payload.quad.y[0]=op->payload.quad.y[1]=1.0;
    op->payload.quad.y[2]=op->payload.quad.y[3]=3.0;
    op->payload.quad.u[0]=op->payload.quad.u[3]=0.0;
    op->payload.quad.u[1]=op->payload.quad.u[2]=1.0;
    op->payload.quad.v[0]=op->payload.quad.v[1]=0.0;
    op->payload.quad.v[2]=op->payload.quad.v[3]=1.0;
  }
  batch.operations=operations;
  batch.operation_count=3u;
  batch.states=&state;
  batch.state_count=1u;
  result=service.submit(service.context,&batch);
  REQUIRE(result.status==GML_RENDER_EXEC_ACCEPTED && result.accepted_prefix==3u,
          "alpha overlap is submitted in order");
  REQUIRE(anygm_test_graphics_scene_draw_calls()==1 &&
          anygm_test_graphics_scene_vertices()==12 &&
          anygm_test_graphics_blend_configurations()==1 &&
          !anygm_test_graphics_blend_enabled(),
          "two overlapping alpha quads use one ordered draw and restore disabled blending");
  REQUIRE(anygm_test_graphics_read_pixels()==0,
          "transparent composition remains resident until explicit observation");
  state.rgb_equation=state.alpha_equation=GML_RENDER_BLEND_EXACT8_TRUNC;
  for(uint32_t index=0;index<3u;index++){
    operations[index].target.image.identity=10u;
    operations[index].target.pixel_format=GML_RENDER_PIXEL_XRGB8888;
  }
  operations[0].payload.clear_colour=0xFF0000FFu;
  result=service.submit(service.context,&batch);
  REQUIRE(result.status==GML_RENDER_EXEC_ACCEPTED && result.accepted_prefix==3u,
          "exact source-over submits after a full clear");
  REQUIRE(anygm_test_graphics_scene_copies()==2u &&
          anygm_test_graphics_scene_copied_pixels()==8u &&
          anygm_test_graphics_read_pixels()==0u,
          "each overlap snapshots only its 2x2 destination region, without a CPU download");
  {
    int prior_clears=anygm_test_graphics_clear_calls();
    int prior_draws=anygm_test_graphics_scene_draw_calls();
    state.rgb_source_factor=GML_RENDER_BLEND_ZERO;
    result=service.submit(service.context,&batch);
    REQUIRE(result.status==GML_RENDER_EXEC_UNSUPPORTED && result.accepted_prefix==0u &&
            anygm_test_graphics_clear_calls()==prior_clears &&
            anygm_test_graphics_scene_draw_calls()==prior_draws,
            "an unqualified blend refuses the whole batch before clear or draw");
  }
  {
    GmlGpuCounters counters;
    gml_gpu_counters(gpu,&counters);
    REQUIRE(counters.scene_batches_offered==3u &&
            counters.scene_batches_accepted==2u && counters.scene_batches_refused==1u &&
            counters.scene_operations[GML_RENDER_OP_CLEAR]==2u &&
            counters.scene_operations[GML_RENDER_OP_TEXTURED_QUAD]==4u &&
            counters.scene_partial_prefix==0u && counters.scene_readbacks==0u,
            "only the two fully accepted resident batches count as completed scene work");
  }
  /* The full-target coarse veil is a different rounded rule, not fixed-function or /256. */
  source.incarnation=2u;
  upload.image=source;
  upload.storage_width=upload.storage_height=1u;
  upload.region=(GmlRenderRect){0,0,1u,1u};
  upload.stride_bytes=upload.byte_capacity=4u;
  REQUIRE(service.upload(service.context,&upload),"one retained coarse texel uploaded");
  state.rgb_source_factor=GML_RENDER_BLEND_SOURCE_ALPHA;
  state.rgb_equation=state.alpha_equation=GML_RENDER_BLEND_EXACT8_ROUND_VEIL;
  state.samplers[0].source_region=(GmlRenderRect){0,0,1u,1u};
  for(uint32_t index=0;index<3u;index++){
    operations[index].target.image.identity=11u;
    operations[index].target.pixel_format=GML_RENDER_PIXEL_ARGB8888;
    if(index){
      operations[index].scene_family=GML_RENDER_SCENE_SPRITE;
      operations[index].reconstruction_mode=GML_RENDER_REPLAY_EXACT;
      operations[index].payload.quad.sampling=state.samplers[0];
      operations[index].payload.quad.x[0]=operations[index].payload.quad.x[3]=0.0;
      operations[index].payload.quad.x[1]=operations[index].payload.quad.x[2]=8.0;
      operations[index].payload.quad.y[0]=operations[index].payload.quad.y[1]=0.0;
      operations[index].payload.quad.y[2]=operations[index].payload.quad.y[3]=8.0;
    }
  }
  {
    unsigned copies=anygm_test_graphics_scene_copies();
    result=service.submit(service.context,&batch);
    REQUIRE(result.status==GML_RENDER_EXEC_ACCEPTED && result.accepted_prefix==3u &&
            anygm_test_graphics_scene_copies()==copies+2u &&
            !anygm_test_graphics_read_pixels(),
            "rounded veils use ordered device snapshots, never a CPU checkpoint");
  }
  for(int variant=0;variant<7;variant++){
    GmlRenderDrawOp invalid[3];
    GmlRenderDrawStateSnapshot invalid_state=state;
    GmlRenderImageRef old_source={GML_RENDER_RESOURCE_ATLAS,8u,1u,1u};
    GmlRenderDrawBatch refused={invalid,3u,&invalid_state,1u};
    int clears=anygm_test_graphics_clear_calls(),draws=anygm_test_graphics_scene_draw_calls();
    memcpy(invalid,operations,sizeof invalid);
    if(variant==0) invalid[1].target.pixel_format=GML_RENDER_PIXEL_XRGB8888;
    if(variant==1) invalid[1].scene_family=GML_RENDER_SCENE_BACKGROUND;
    if(variant==2) invalid[1].payload.quad.x[0]=invalid[1].payload.quad.x[3]=1.0;
    if(variant==3) invalid[1].payload.quad.sampling.source_region.width=2u;
    if(variant==4) invalid_state.alpha_equation=GML_RENDER_BLEND_ADD;
    if(variant==5) invalid[1].payload.quad.corner_colour[0]=0xFF123456u;
    if(variant==6) invalid[1].sources=&old_source;
    result=service.submit(service.context,&refused);
    REQUIRE(result.status==GML_RENDER_EXEC_UNSUPPORTED && !result.accepted_prefix &&
            clears==anygm_test_graphics_clear_calls() &&
            draws==anygm_test_graphics_scene_draw_calls(),
            "unqualified rounded-veil descriptors refuse before their valid clear prefix");
  }
  /* Constant SCREEN veils retain unknown-alpha coverage without a predecessor copy.
   * Compatible overlapping veils share one RGB pass and one opaque-alpha pass. */
  {
    const uint8_t veil_pixels[4]={255u,0u,0u,128u};
    GmlRenderImageRef veil_source={GML_RENDER_RESOURCE_SCRATCH,101u,1u,1u};
    GmlRenderImageRef argb_source={GML_RENDER_RESOURCE_SCRATCH,102u,1u,1u};
    GmlRenderImageUpload veil_upload=upload;
    GmlRenderImageUpload argb_upload=upload;
    GmlRenderDrawOp veil_ops[3];
    GmlRenderDrawStateSnapshot veil_state=state;
    GmlRenderDrawBatch veil_batch={veil_ops,3u,&veil_state,1u};
    unsigned copies=anygm_test_graphics_scene_copies();
    int draws=anygm_test_graphics_scene_draw_calls();
    veil_upload.image=veil_source;
    veil_upload.pixel_format=GML_RENDER_PIXEL_RGBA8888;
    veil_upload.bytes=veil_pixels;
    REQUIRE(service.upload(service.context,&veil_upload),"constant screen veil uploaded");
    argb_upload.image=argb_source;
    REQUIRE(service.upload(service.context,&argb_upload),"unqualified ARGB screen veil uploaded");
    memcpy(veil_ops,operations,sizeof veil_ops);
    veil_state.rgb_equation=veil_state.alpha_equation=GML_RENDER_BLEND_ADD;
    veil_state.coverage_policy=GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA;
    veil_state.samplers[0].filter=GML_RENDER_FILTER_HARDWARE_BILINEAR;
    for(unsigned index=0;index<3u;index++){
      veil_ops[index].target.image.resource_class=GML_RENDER_RESOURCE_SCREEN;
      veil_ops[index].target.image.identity=13u;
      if(index){
        veil_ops[index].sources=&veil_source;
        veil_ops[index].reconstruction_mode=GML_RENDER_DEVICE_CHECKPOINT;
        veil_ops[index].payload.quad.filtered_draw_alpha=1.0;
        veil_ops[index].payload.quad.sampling=veil_state.samplers[0];
      }
    }
    result=service.submit(service.context,&veil_batch);
    REQUIRE(result.status==GML_RENDER_EXEC_ACCEPTED && result.accepted_prefix==3u &&
            anygm_test_graphics_scene_draw_calls()==draws+2 &&
            anygm_test_graphics_scene_copies()==copies && !anygm_test_graphics_read_pixels(),
            "overlapping constant screen veils batch RGB and opaque alpha without copies or reads");
    for(unsigned variant=0;variant<7u;variant++){
      GmlRenderDrawOp invalid[3];
      GmlRenderDrawBatch refused={invalid,3u,&veil_state,1u};
      int clears=anygm_test_graphics_clear_calls();
      draws=anygm_test_graphics_scene_draw_calls();
      memcpy(invalid,veil_ops,sizeof invalid);
      if(variant==0u) invalid[1].target.image.resource_class=GML_RENDER_RESOURCE_APPLICATION;
      if(variant==1u) invalid[1].scene_family=GML_RENDER_SCENE_BACKGROUND;
      if(variant==2u) invalid[1].payload.quad.x[0]=invalid[1].payload.quad.x[3]=1.0;
      if(variant==3u) invalid[1].payload.quad.filtered_draw_alpha=0.5;
      if(variant==4u) invalid[1].reconstruction_mode=GML_RENDER_REPLAY_EXACT;
      if(variant==5u) invalid[1].payload.quad.sampling.source_region.width=2u;
      if(variant==6u) invalid[1].sources=&argb_source;
      result=service.submit(service.context,&refused);
      REQUIRE(result.status==GML_RENDER_EXEC_UNSUPPORTED && !result.accepted_prefix &&
              clears==anygm_test_graphics_clear_calls() &&
              draws==anygm_test_graphics_scene_draw_calls() &&
              anygm_test_graphics_scene_copies()==copies && !anygm_test_graphics_read_pixels(),
              "unqualified constant veil variants reject transactionally before their clear prefix");
    }
    service.retire(service.context,veil_source);
    service.retire(service.context,argb_source);
  }
  /* Opaque stretched magnification has a distinct phase/weight policy, never
   * an unchecked option for an arbitrary sampled quad. */
  source.incarnation=3u;
  for(int index=0;index<4;index++) source_pixels[index]=0xFF123456u;
  upload.image=source;
  upload.storage_width=upload.storage_height=2u;
  upload.region=(GmlRenderRect){0,0,2u,2u};
  upload.stride_bytes=8u;
  upload.byte_capacity=sizeof source_pixels;
  REQUIRE(service.upload(service.context,&upload),"opaque stretched atlas uploaded");
  memset(&state,0,sizeof state);
  state.write_mask=15u;
  state.alpha=255u;
  state.colour=0xFFFFFFFFu;
  state.sampler_count=1u;
  state.samplers[0].filter=GML_RENDER_FILTER_BILINEAR;
  state.samplers[0].edge_rule=GML_RENDER_EDGE_CLAMP_SUBIMAGE;
  state.samplers[0].source_region=(GmlRenderRect){0,0,2u,2u};
  state.samplers[0].quantisation=GML_RENDER_QUANTISE_STRETCHED_FLOAT;
  for(uint32_t index=0;index<3u;index++){
    operations[index].target.image.identity=12u;
    if(index){
      GmlRenderQuad *quad=&operations[index].payload.quad;
      operations[index].reconstruction_mode=GML_RENDER_REPLAY_EXACT;
      quad->sampling=state.samplers[0];
      quad->span_x[0]=quad->span_y[0]=2.0;
      quad->span_x[1]=quad->span_y[1]=8.0;
      quad->span_x[2]=quad->span_y[2]=0.0;
      quad->span_x[3]=quad->span_y[3]=2.0;
    }
  }
  result=service.submit(service.context,&batch);
  REQUIRE(result.status==GML_RENDER_EXEC_ACCEPTED && result.accepted_prefix==3u &&
          !anygm_test_graphics_read_pixels(),
          "opaque stretched descriptors execute without submission-time CPU reads");
  for(int variant=0;variant<8;variant++){
    GmlRenderDrawOp invalid[3];
    GmlRenderDrawStateSnapshot invalid_state=state;
    GmlRenderDrawBatch refused={invalid,3u,&invalid_state,1u};
    int clears=anygm_test_graphics_clear_calls(),draws=anygm_test_graphics_scene_draw_calls();
    memcpy(invalid,operations,sizeof invalid);
    if(variant==0) invalid[1].payload.quad.sampling.quantisation=2u;
    if(variant==1) invalid[1].payload.quad.sampling.filter=GML_RENDER_FILTER_NEAREST;
    if(variant==2) invalid[1].payload.quad.corner_colour[0]=0xFF123456u;
    if(variant==3) invalid_state.samplers[0].quantisation=0u;
    if(variant==4) invalid[1].payload.quad.span_x[1]=8.5;
    if(variant==5) invalid[1].payload.quad.span_x[0]=16385.0;
    if(variant==6) invalid[1].payload.quad.span_y[2]=1.0;
    if(variant==7) invalid[1].scene_family=GML_RENDER_SCENE_TEXT;
    result=service.submit(service.context,&refused);
    REQUIRE(result.status==GML_RENDER_EXEC_UNSUPPORTED && !result.accepted_prefix &&
            clears==anygm_test_graphics_clear_calls() &&
            draws==anygm_test_graphics_scene_draw_calls(),
            "unqualified stretched descriptors refuse before their valid clear prefix");
  }
  /* Covered float magnifications keep original draw alpha and ordered GPU
   * destination snapshots. A later bad descriptor refuses the entire batch. */
  {
    static const uint8_t low_alpha[16]={255,32,16,1,32,255,16,1,
                                       16,32,255,1,255,255,255,1};
    GmlRenderImageRef covered_source={GML_RENDER_RESOURCE_ATLAS,31u,1u,1u};
    GmlRenderImageUpload covered_upload=upload;
    GmlRenderDrawOp covered_operations[3];
    GmlRenderDrawStateSnapshot covered_state=state;
    GmlRenderDrawBatch covered_batch={covered_operations,3u,&covered_state,1u};
    covered_upload.image=covered_source;
    covered_upload.pixel_format=GML_RENDER_PIXEL_RGBA8888;
    covered_upload.bytes=low_alpha;
    REQUIRE(service.upload(service.context,&covered_upload),"low-alpha stretched atlas uploaded");
    memcpy(covered_operations,operations,sizeof covered_operations);
    covered_state.rgb_source_factor=GML_RENDER_BLEND_SOURCE_ALPHA;
    covered_state.rgb_destination_factor=GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA;
    covered_state.alpha_source_factor=GML_RENDER_BLEND_ONE;
    covered_state.alpha_destination_factor=GML_RENDER_BLEND_ZERO;
    covered_state.rgb_equation=covered_state.alpha_equation=GML_RENDER_BLEND_EXACT_STRETCHED_FLOAT;
    for(unsigned index=0;index<3u;index++){
      covered_operations[index].target.image.resource_class=GML_RENDER_RESOURCE_SCREEN;
      covered_operations[index].target.image.identity=32u;
      if(!index) continue;
      covered_operations[index].sources=&covered_source;
      covered_operations[index].reconstruction_mode=GML_RENDER_DEVICE_CHECKPOINT;
      covered_operations[index].payload.quad.filtered_draw_alpha=index==1u?0.6:0x1p-53;
      for(unsigned corner=0;corner<4u;corner++)
        covered_operations[index].payload.quad.corner_colour[corner]=0xFFa0c0e0u;
    }
    unsigned copies=anygm_test_graphics_scene_copies();
    int draws=anygm_test_graphics_scene_draw_calls();
    result=service.submit(service.context,&covered_batch);
    REQUIRE(result.status==GML_RENDER_EXEC_ACCEPTED && result.accepted_prefix==3u &&
            anygm_test_graphics_scene_copies()==copies+2u &&
            anygm_test_graphics_scene_draw_calls()==draws+2 &&
            !anygm_test_graphics_read_pixels(),
            "overlapping float blends snapshot their own GPU predecessor without CPU reads");
    for(unsigned variant=0;variant<12u;variant++){
      GmlRenderDrawOp invalid[3];
      GmlRenderDrawStateSnapshot invalid_state=covered_state;
      GmlRenderDrawBatch refused={invalid,3u,&invalid_state,1u};
      memcpy(invalid,covered_operations,sizeof invalid);
      if(variant==0) invalid[2].payload.quad.filtered_draw_alpha=NAN;
      if(variant==1) invalid[2].payload.quad.filtered_draw_alpha=0x1p-65;
      if(variant==2) invalid[2].payload.quad.filtered_draw_alpha=1.1;
      if(variant==3) invalid[2].scene_family=GML_RENDER_SCENE_BACKGROUND;
      if(variant==4) invalid[2].reconstruction_mode=GML_RENDER_REPLAY_EXACT;
      if(variant==5) invalid[2].payload.quad.x[0]=invalid[2].payload.quad.x[3]=0.25;
      if(variant==6) invalid[2].payload.quad.span_x[0]=1.0;
      if(variant==7) invalid[2].payload.quad.span_x[1]=1000001.0;
      if(variant==8) invalid[2].payload.quad.corner_colour[1]=0xFF123456u;
      if(variant==9) invalid[2].payload.quad.sampling.quantisation=0u;
      if(variant==10) invalid_state.alpha_equation=GML_RENDER_BLEND_ADD;
      if(variant==11) invalid[2].target.pixel_format=GML_RENDER_PIXEL_XRGB8888;
      int clears=anygm_test_graphics_clear_calls();
      draws=anygm_test_graphics_scene_draw_calls();
      copies=anygm_test_graphics_scene_copies();
      result=service.submit(service.context,&refused);
      REQUIRE(result.status==GML_RENDER_EXEC_UNSUPPORTED && !result.accepted_prefix &&
              anygm_test_graphics_clear_calls()==clears &&
              anygm_test_graphics_scene_draw_calls()==draws &&
              anygm_test_graphics_scene_copies()==copies,
              "unqualified float descriptor refuses before its clear, prefix and snapshot");
    }
    draws=anygm_test_graphics_scene_draw_calls();
    anygm_test_graphics_fail_scene_draw_at((unsigned)draws+2u);
    result=service.submit(service.context,&covered_batch);
    REQUIRE(result.status==GML_RENDER_EXEC_RECOVERABLE_ERROR && result.accepted_prefix==2u &&
            result.uncertain_target_count==1u &&
            result.uncertain_targets[0].identity==32u,
            "a later float draw failure keeps the accepted prefix and marks its target uncertain");
    anygm_test_graphics_fail_scene_draw_at(0u);
  }
  /* Filtered font quantisation carries double phase and generation policy,
   * not the opaque stretched rule or fixed-function source-over. */
  source.incarnation=4u;
  upload.image=source;
  upload.pixel_format=GML_RENDER_PIXEL_RGBA8888;
  REQUIRE(service.upload(service.context,&upload),"filtered real-font RGBA atlas uploaded");
  state.rgb_source_factor=GML_RENDER_BLEND_SOURCE_ALPHA;
  state.rgb_destination_factor=GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA;
  state.alpha_destination_factor=GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA;
  state.rgb_equation=state.alpha_equation=GML_RENDER_BLEND_EXACT_FILTERED_FONT;
  for(unsigned modern=0;modern<2u;modern++) for(unsigned preserve=0;preserve<2u;preserve++){
    state.alpha_source_factor=preserve?GML_RENDER_BLEND_SOURCE_ALPHA:GML_RENDER_BLEND_ONE;
    state.samplers[0].quantisation=modern?GML_RENDER_QUANTISE_FONT_MODERN:
                                          GML_RENDER_QUANTISE_FONT_FIRSTGEN;
    for(unsigned index=0;index<3u;index++){
      operations[index].target.image.identity=13u+modern*2u+preserve;
      operations[index].target.pixel_format=GML_RENDER_PIXEL_ARGB8888;
      if(!index) continue;
      GmlRenderQuad *quad=&operations[index].payload.quad;
      operations[index].scene_family=GML_RENDER_SCENE_TEXT;
      quad->sampling=state.samplers[0];
      quad->x[0]=quad->x[3]=quad->y[0]=quad->y[1]=1.0;
      quad->x[1]=quad->x[2]=quad->y[2]=quad->y[3]=2.0;
      quad->inverse_anchor[0]=quad->inverse_anchor[1]=1.0;
      quad->inverse_x[0]=quad->inverse_y[1]=0.5;
      quad->filtered_draw_alpha=0.7;
      for(unsigned corner=0;corner<4u;corner++) quad->corner_colour[corner]=0xFF40C080u;
    }
    unsigned copies=anygm_test_graphics_scene_copies();
    uint64_t copied=anygm_test_graphics_scene_copied_pixels();
    int draws=anygm_test_graphics_scene_draw_calls();
    unsigned flushes=anygm_test_graphics_flush_calls();
    unsigned channels[3];
    for(unsigned channel=0;channel<3u;channel++)
      channels[channel]=anygm_test_graphics_font_channel_draws(channel);
    result=service.submit(service.context,&batch);
    REQUIRE(result.status==GML_RENDER_EXEC_ACCEPTED && result.accepted_prefix==3u &&
            anygm_test_graphics_scene_copies()==copies+2u &&
            anygm_test_graphics_scene_copied_pixels()==copied+2u &&
            !anygm_test_graphics_read_pixels(),
            "each filtered glyph snapshots only its pixel, without folding order or CPU reads");
    REQUIRE(anygm_test_graphics_scene_draw_calls()==draws+
              (api==GML_GPU_API_OPENGLES3 && split_font?6:2) &&
            anygm_test_graphics_flush_calls()==flushes+
              (api==GML_GPU_API_OPENGLES3 && split_font && font_flush?2u:0u) &&
            anygm_test_graphics_color_mask_all() &&
            !anygm_test_graphics_font_channel_errors(),
            "font writes use the API-qualified pass count and restore the complete colour mask");
    if(api==GML_GPU_API_OPENGLES3 && split_font)
      for(unsigned channel=0;channel<3u;channel++)
        REQUIRE(anygm_test_graphics_font_channel_draws(channel)==channels[channel]+2u,
                "each GLES glyph writes each selected RGB component and alpha only on the last");
    for(unsigned variant=0;variant<18u;variant++){
      GmlRenderDrawOp invalid[3];
      GmlRenderDrawStateSnapshot invalid_state=state;
      GmlRenderDrawBatch refused={invalid,3u,&invalid_state,1u};
      int clears=anygm_test_graphics_clear_calls(),draws=anygm_test_graphics_scene_draw_calls();
      memcpy(invalid,operations,sizeof invalid);
      GmlRenderQuad *quad=&invalid[2].payload.quad;
      if(variant==0u) quad->sampling.quantisation=0u;
      if(variant==1u) quad->sampling.filter=GML_RENDER_FILTER_NEAREST;
      if(variant==2u) quad->corner_colour[1]=0xFF112233u;
      if(variant==3u) invalid_state.samplers[0].quantisation=0u;
      if(variant==4u) quad->inverse_x[0]=0.0;
      if(variant==5u) quad->inverse_y[1]=-0.5;
      if(variant==6u) quad->inverse_anchor[0]=NAN;
      if(variant==7u) quad->inverse_anchor[1]=0x1p-1074;
      if(variant==8u) quad->filtered_draw_alpha=1.01;
      if(variant==9u) quad->filtered_draw_alpha=0x1p-33;
      if(variant==10u) quad->span_active=1u;
      if(variant==11u) quad->sampling.logical_margin_mask=1u;
      if(variant==12u) invalid_state.rgb_source_factor=GML_RENDER_BLEND_ZERO;
      if(variant==13u) invalid[2].scene_family=GML_RENDER_SCENE_SPRITE;
      if(variant==14u) invalid[2].reconstruction_mode=GML_RENDER_DEVICE_CHECKPOINT;
      if(variant==15u) quad->inverse_offset[0]=0.5;
      if(variant==16u) quad->inverse_x[0]=1.5;
      if(variant==17u) quad->x[2]=3.0;
      result=service.submit(service.context,&refused);
      REQUIRE(result.status==GML_RENDER_EXEC_UNSUPPORTED && !result.accepted_prefix &&
              clears==anygm_test_graphics_clear_calls() &&
              draws==anygm_test_graphics_scene_draw_calls() &&
              copies+2u==anygm_test_graphics_scene_copies(),
              "a later invalid glyph refuses before the valid clear and glyph prefix writes");
    }
  }
  if(api==GML_GPU_API_OPENGLES3 && split_font){
    unsigned compiled=anygm_test_graphics_compile_calls();
    int draws=anygm_test_graphics_scene_draw_calls();
    anygm_test_graphics_fail_scene_draw_at((unsigned)draws+2u);
    result=service.submit(service.context,&batch);
    REQUIRE(result.status==GML_RENDER_EXEC_RECOVERABLE_ERROR &&
            result.accepted_prefix==1u && result.uncertain_target_count==1u &&
            anygm_test_graphics_scene_draw_calls()==draws+2 &&
            anygm_test_graphics_color_mask_all() &&
            !anygm_test_graphics_attributes_enabled() &&
            !anygm_test_graphics_bound_program(),
            "a failed middle component stops before blue/alpha and invalidates the partial target");
    anygm_test_graphics_fail_scene_draw_at(0u);
    result=service.submit(service.context,&batch);
    REQUIRE(result.status==GML_RENDER_EXEC_ACCEPTED && result.accepted_prefix==3u &&
            anygm_test_graphics_compile_calls()==compiled &&
            !anygm_test_graphics_font_channel_errors() &&
            !anygm_test_graphics_read_pixels(),
            "a fresh clear recovers with the cached generation program without CPU reads");
  }
  /* The ordinary GUI sprite recipe shares exact sampling arithmetic, but
   * remains a SPRITE checkpoint and stamps only source-covered SCREEN alpha. */
  state.alpha_source_factor=GML_RENDER_BLEND_ONE;
  state.coverage_policy=0u;
  state.samplers[0].quantisation=GML_RENDER_QUANTISE_SPRITE_FIRSTGEN_INDEPENDENT;
  for(unsigned index=0;index<3u;index++){
    operations[index].target.image.resource_class=GML_RENDER_RESOURCE_SCREEN;
    operations[index].target.image.identity=31u;
    if(!index) continue;
    operations[index].scene_family=GML_RENDER_SCENE_SPRITE;
    operations[index].reconstruction_mode=GML_RENDER_DEVICE_CHECKPOINT;
    operations[index].payload.quad.sampling=state.samplers[0];
  }
  unsigned copies=anygm_test_graphics_scene_copies();
  uint64_t copied=anygm_test_graphics_scene_copied_pixels();
  int draws=anygm_test_graphics_scene_draw_calls();
  unsigned compiled=anygm_test_graphics_compile_calls();
  result=service.submit(service.context,&batch);
  REQUIRE(result.status==GML_RENDER_EXEC_ACCEPTED && result.accepted_prefix==3u &&
          anygm_test_graphics_scene_copies()==copies+2u &&
          anygm_test_graphics_scene_copied_pixels()==copied+2u &&
          anygm_test_graphics_scene_draw_calls()==draws+
            (api==GML_GPU_API_OPENGLES3 && split_font?6:2) &&
          anygm_test_graphics_compile_calls()==compiled &&
          anygm_test_graphics_color_mask_all() && !anygm_test_graphics_read_pixels(),
          "ordinary filtered sprites reuse the exact legacy program and bounded GPU snapshots");
  for(unsigned variant=0;variant<12u;variant++){
    GmlRenderDrawOp invalid[3];
    GmlRenderDrawStateSnapshot invalid_state=state;
    GmlRenderDrawBatch refused={invalid,3u,&invalid_state,1u};
    memcpy(invalid,operations,sizeof invalid);
    GmlRenderQuad *quad=&invalid[2].payload.quad;
    if(variant==0u) invalid[2].scene_family=GML_RENDER_SCENE_TEXT;
    if(variant==1u) for(unsigned index=0;index<3u;index++)
      invalid[index].target.image.resource_class=GML_RENDER_RESOURCE_APPLICATION;
    if(variant==2u) invalid[2].reconstruction_mode=GML_RENDER_REPLAY_EXACT;
    if(variant==3u) invalid_state.alpha_source_factor=GML_RENDER_BLEND_SOURCE_ALPHA;
    if(variant==4u) quad->inverse_x[0]=0.0;
    if(variant==5u) quad->inverse_x[0]=4.01;
    if(variant==6u) quad->inverse_anchor[0]=NAN;
    if(variant==7u) quad->inverse_anchor[1]=0x1p-1074;
    if(variant==8u) quad->filtered_draw_alpha=0x1p-33;
    if(variant==9u) quad->sampling.logical_margin_mask=16u;
    if(variant==10u) quad->corner_colour[1]=0xff112233u;
    if(variant==11u) quad->sampling.quantisation=GML_RENDER_QUANTISE_FONT_FIRSTGEN;
    int clears=anygm_test_graphics_clear_calls();
    draws=anygm_test_graphics_scene_draw_calls();
    copies=anygm_test_graphics_scene_copies();
    result=service.submit(service.context,&refused);
    REQUIRE(result.status==GML_RENDER_EXEC_UNSUPPORTED && !result.accepted_prefix &&
            anygm_test_graphics_clear_calls()==clears &&
            anygm_test_graphics_scene_draw_calls()==draws &&
            anygm_test_graphics_scene_copies()==copies,
            "invalid ordinary filtered metadata rejects the complete batch before any write");
  }
  draws=anygm_test_graphics_scene_draw_calls();
  unsigned passes=api==GML_GPU_API_OPENGLES3 && split_font?3u:1u;
  anygm_test_graphics_fail_scene_draw_at((unsigned)draws+passes+1u);
  result=service.submit(service.context,&batch);
  REQUIRE(result.status==GML_RENDER_EXEC_RECOVERABLE_ERROR && result.accepted_prefix==2u &&
          result.uncertain_target_count==1u && anygm_test_graphics_color_mask_all(),
          "a failed ordinary exact draw preserves the prefix and fences its target");
  anygm_test_graphics_fail_scene_draw_at(0u);
  gml_gpu_destroy(gpu,1);
  return 1;
}

static int scene_alpha_composition_case(void){
  return scene_alpha_composition_api(GML_GPU_API_OPENGL_CORE,"V3D synthetic",0,0);
}

static int scene_gles_font_programs_case(void){
  static const struct { const char *renderer; int font_flush; } drivers[]={
    {"D3D12 (NVIDIA synthetic)",0},
    {"llvmpipe synthetic",0},
    {"V3D synthetic",1},
    {"ANGLE (V3D synthetic)",1},
    {NULL,1},
    {"",1},
  };
  for(size_t index=0;index<sizeof drivers/sizeof drivers[0];index++)
    if(!scene_alpha_composition_api(GML_GPU_API_OPENGLES3,
          drivers[index].renderer,drivers[index].font_flush,0)) return 0;
  /* A compiler that refuses the combined exact program retains both established generation
   * programs, including the renderer-qualified masked-write submission workaround. */
  if(!scene_alpha_composition_api(GML_GPU_API_OPENGLES3,
       "D3D12 (NVIDIA synthetic)",0,1) ||
     !scene_alpha_composition_api(GML_GPU_API_OPENGLES3,"V3D synthetic",1,1)) return 0;
  return 1;
}

static int execute_state_and_geometry_case(void){
  enum { SOURCE_WIDTH=4,SOURCE_HEIGHT=3,TARGET_WIDTH=40,TARGET_HEIGHT=30,
         CANVAS_X=4,CANVAS_Y=6,CANVAS_WIDTH=32,CANVAS_HEIGHT=18 };
  uint32_t source[SOURCE_WIDTH*SOURCE_HEIGHT];
  GmlGpu *gpu;
  GmlGpuContext context;
  GmlRenderPlan plan;
  anygm_test_graphics_reset();
  for(size_t index=0;index<SOURCE_WIDTH*SOURCE_HEIGHT;index++)
    source[index]=0xFF000000u|(uint32_t)(index*7u+3u);
  gpu=gml_gpu_create();
  context=fake_context(GML_GPU_API_OPENGL_CORE);
  REQUIRE(gml_gpu_context_reset(gpu,&context),"context adopted");

  /* Hostile prior state: the frontend shares this context and may leave anything enabled. Every
   * piece of state the pass depends on has to be set rather than inherited. */
  anygm_test_graphics_disturb();

  REQUIRE(build_canvas_plan(&plan,source,SOURCE_WIDTH,SOURCE_HEIGHT,
                            TARGET_WIDTH,TARGET_HEIGHT,
                            CANVAS_X,CANVAS_Y,CANVAS_WIDTH,CANVAS_HEIGHT),"plan built");
  REQUIRE(gml_gpu_execute_plan(gpu,&plan),"the plan executes");

  REQUIRE(anygm_test_graphics_framebuffer_queries()==1,"the current framebuffer is queried for the frame");
  REQUIRE(!anygm_test_graphics_blend_enabled(),"blending is disabled");
  REQUIRE(!anygm_test_graphics_depth_enabled(),"depth testing is disabled");
  REQUIRE(!anygm_test_graphics_stencil_enabled(),"stencil testing is disabled");
  REQUIRE(!anygm_test_graphics_cull_enabled(),"face culling is disabled");
  REQUIRE(!anygm_test_graphics_dither_enabled(),"dithering is disabled");
  REQUIRE(anygm_test_graphics_color_mask_all(),"every colour channel is written");
  REQUIRE(!anygm_test_graphics_depth_mask(),"depth writes are off");
  REQUIRE(!anygm_test_graphics_scissor_enabled(),"the scissor test is left disabled for the frontend");
  REQUIRE(anygm_test_graphics_bound_program()==0,"the program is unbound at the end of the pass");
  REQUIRE(anygm_test_graphics_bound_vertex_array()==0,"the vertex array is unbound at the end of the pass");
  REQUIRE(anygm_test_graphics_clear_calls()==1,"the margins are cleared once");
  {
    float red=0,green=0,blue=0,alpha=0;
    anygm_test_graphics_clear_colour(&red,&green,&blue,&alpha);
    REQUIRE(red==0.0f && green==0.0f && blue==0.0f && alpha==1.0f,
            "the clear is exactly black and opaque");
  }
  REQUIRE(anygm_test_graphics_draw_calls()==1,"one draw covers the canvas");

  /* The rectangle reaches the driver flipped, because AnyGM addresses rows from the top and the
   * framebuffer's origin is at the bottom. A pass that got this wrong would still look plausible
   * on a symmetric canvas, so the fixture is deliberately off-centre. */
  {
    int vx=0,vy=0,vw=0,vh=0,sx=0,sy=0,sw=0,sh=0;
    anygm_test_graphics_viewport(&vx,&vy,&vw,&vh);
    anygm_test_graphics_scissor(&sx,&sy,&sw,&sh);
    REQUIRE(vx==CANVAS_X,"viewport x is the canvas origin");
    REQUIRE(vy==TARGET_HEIGHT-(CANVAS_Y+CANVAS_HEIGHT),"viewport y is measured from the bottom");
    REQUIRE(vw==CANVAS_WIDTH && vh==CANVAS_HEIGHT,"the viewport is the canvas extent");
    REQUIRE(sx==vx && sy==vy && sw==vw && sh==vh,"the scissor matches the viewport");
  }
  REQUIRE(anygm_test_graphics_unpack_row_length()==0,"the source pitch override is restored");
  gml_gpu_destroy(gpu,1);
  return 1;
}

static int upload_and_reuse_case(void){
  enum { SOURCE_WIDTH=4,SOURCE_HEIGHT=3,TARGET=16 };
  uint32_t source[SOURCE_WIDTH*SOURCE_HEIGHT];
  GmlGpu *gpu;
  GmlGpuContext context;
  GmlRenderPlan plan;
  uint32_t first_uploads=0;
  anygm_test_graphics_reset();
  for(size_t index=0;index<SOURCE_WIDTH*SOURCE_HEIGHT;index++)
    source[index]=0xFF000000u|(uint32_t)(index*11u+5u);
  gpu=gml_gpu_create();
  context=fake_context(GML_GPU_API_OPENGLES3);
  REQUIRE(gml_gpu_context_reset(gpu,&context),"context adopted");
  REQUIRE(build_canvas_plan(&plan,source,SOURCE_WIDTH,SOURCE_HEIGHT,TARGET,TARGET,
                            0,0,TARGET,TARGET),"plan built");
  REQUIRE(gml_gpu_execute_plan(gpu,&plan),"first execution");

  /* The source arrives as raw XRGB words: the driver must receive exactly those bytes, unswizzled
   * and unpadded, because the shader is what puts the components in order. */
  {
    unsigned name=anygm_test_graphics_texture_with_format(GL_RGBA8);
    size_t count=0;
    const unsigned char *bytes=anygm_test_graphics_texture_bytes(name,&count);
    REQUIRE(name!=0,"the source became a normalized eight-bit texture");
    REQUIRE(anygm_test_graphics_texture_width(name)==SOURCE_WIDTH &&
            anygm_test_graphics_texture_height(name)==SOURCE_HEIGHT,
            "the source keeps its extent");
    REQUIRE(count==sizeof source,"the whole source was uploaded");
    REQUIRE(memcmp(bytes,source,sizeof source)==0,
            "the uploaded bytes are the source words unchanged");
    first_uploads=anygm_test_graphics_texture_uploads(name);
  }
  /* The axis maps are integer textures, and their contents are the exact indices the software
   * executor selects. */
  {
    /* The first integer texture is the horizontal map; a pass creates one per axis. */
    unsigned name=anygm_test_graphics_first_texture_with_format(GL_R16UI);
    size_t count=0;
    REQUIRE(name!=0,"the axis map became an unsigned integer texture");
    REQUIRE(anygm_test_graphics_texture_height(name)==1,"an axis map is one row");
    REQUIRE(anygm_test_graphics_texture_width(name)==TARGET,
            "an axis map covers the destination extent");
    {
      const uint16_t *map=(const uint16_t*)anygm_test_graphics_texture_bytes(name,&count);
      for(uint32_t index=0;index<TARGET;index++){
        uint32_t expected=(uint32_t)(((uint64_t)SOURCE_WIDTH*(2u*(uint64_t)index+1u))/
                                     (2u*(uint64_t)TARGET));
        REQUIRE(map[index]==expected,"the axis map holds the exact source index");
      }
    }
  }
  /* An unchanged generation must not upload again: a monitor-sized re-upload every frame is the
   * cost this path exists to remove. */
  REQUIRE(gml_gpu_execute_plan(gpu,&plan),"second execution");
  REQUIRE(anygm_test_graphics_texture_uploads(
            anygm_test_graphics_texture_with_format(GL_RGBA8))==first_uploads,
          "an unchanged source is not uploaded again");
  /* A changed generation must. */
  plan.images[0].pixel_generation=2u;
  REQUIRE(gml_gpu_execute_plan(gpu,&plan),"third execution");
  REQUIRE(anygm_test_graphics_texture_uploads(
            anygm_test_graphics_texture_with_format(GL_RGBA8))>first_uploads,
          "a changed generation uploads again");
  gml_gpu_destroy(gpu,1);
  return 1;
}

static int reduction_falls_back_case(void){
  enum { SOURCE=32,TARGET=8 };
  uint32_t source[SOURCE*SOURCE];
  GmlGpu *gpu;
  GmlGpuContext context;
  GmlRenderPlan plan;
  GmlPlanImage image;
  GmlPlanRect whole={0,0,TARGET,TARGET};
  uint32_t index;
  anygm_test_graphics_reset();
  memset(source,0,sizeof source);
  gpu=gml_gpu_create();
  context=fake_context(GML_GPU_API_OPENGL_CORE);
  REQUIRE(gml_gpu_context_reset(gpu,&context),"context adopted");
  memset(&image,0,sizeof image);
  image.image_class=GML_PLAN_IMAGE_COMPLETED_FRAME;
  image.identity=1u;
  image.width=SOURCE;
  image.height=SOURCE;
  image.pitch_pixels=SOURCE;
  image.pixel_format=GML_PLAN_PIXEL_XRGB8888;
  image.cpu_pixels=source;
  gml_render_plan_reset(&plan,GML_PLAN_TARGET_HOST_FRAMEBUFFER,TARGET,TARGET);
  index=gml_render_plan_add_image(&plan,&image);
  REQUIRE(gml_render_plan_add_blit_box(&plan,index,whole,0u),"reduction recorded");
  {
    int draws_before=anygm_test_graphics_draw_calls();
    REQUIRE(!gml_gpu_execute_plan(gpu,&plan),"the reduction is refused");
    REQUIRE(anygm_test_graphics_draw_calls()==draws_before,"a refused pass publishes no partial work");
    REQUIRE(plan.fallback_reason==GML_PLAN_FALLBACK_BOX_REDUCTION,"the refusal names itself");
  }
  {
    GmlGpuCounters counters;
    gml_gpu_counters(gpu,&counters);
    REQUIRE(counters.passes_replayed==1,"the software replay is counted");
    REQUIRE(counters.fallbacks[GML_PLAN_FALLBACK_BOX_REDUCTION]==1,"the reason is counted");
    REQUIRE(counters.passes_accepted==0,"nothing was accepted");
  }
  gml_gpu_destroy(gpu,1);
  return 1;
}

static int no_context_falls_back_case(void){
  enum { SOURCE=4,TARGET=8 };
  uint32_t source[SOURCE*SOURCE];
  GmlGpu *gpu;
  GmlRenderPlan plan;
  anygm_test_graphics_reset();
  memset(source,0,sizeof source);
  gpu=gml_gpu_create();
  REQUIRE(build_canvas_plan(&plan,source,SOURCE,SOURCE,TARGET,TARGET,0,0,TARGET,TARGET),
          "plan built");
  REQUIRE(!gml_gpu_execute_plan(gpu,&plan),"a plan without a context is refused");
  REQUIRE(plan.fallback_reason==GML_PLAN_FALLBACK_CONTEXT_UNAVAILABLE,"the refusal names itself");
  REQUIRE(anygm_test_graphics_draw_calls()==0,"nothing reached a driver");
  gml_gpu_destroy(gpu,0);
  return 1;
}

/* ---- The content's own programs ---- */

static const char CONTENT_VERTEX_ES[]=
  "attribute vec3 in_Position;\n"
  "attribute vec4 in_Colour;\n"
  "attribute vec2 in_TextureCoord;\n"
  "varying vec2 v_vTexcoord;\n"
  "varying vec4 v_vColour;\n"
  "void main(){\n"
  "  gl_Position = gm_Matrices[MATRIX_WORLD_VIEW_PROJECTION] * vec4(in_Position, 1.0);\n"
  "  v_vColour = in_Colour;\n"
  "  v_vTexcoord = in_TextureCoord;\n"
  "}\n";
static const char CONTENT_FRAGMENT_ES[]=
  "precision mediump float;\n"
  "varying vec2 v_vTexcoord;\n"
  "varying vec4 v_vColour;\n"
  "uniform vec2 u_size;\n"
  "uniform float u_time;\n"
  "uniform int u_mode;\n"
  "uniform mat4 u_transform;\n"
  "uniform sampler2D samp_mask;\n"
  "void main(){\n"
  "  vec4 base = texture2D(gm_BaseTexture, v_vTexcoord);\n"
  "  vec4 mask = texture2D(samp_mask, v_vTexcoord * u_size);\n"
  "  gl_FragColor = v_vColour * base * mask;\n"
  "}\n";

/* A plan that presents a frame through a content program with one extra sampler, the shape the
 * screen pass builds when the content drew its frame through its own shader. */
static int build_content_plan(GmlRenderPlan *plan,const uint32_t *frame,const uint32_t *mask){
  GmlPlanImage image;
  GmlPlanRect whole={0,0,16,12};
  GmlPlanShader shader;
  uint32_t source,sampler;
  memset(&image,0,sizeof image);
  image.image_class=GML_PLAN_IMAGE_APPLICATION_SURFACE;
  image.identity=1u;
  image.content_generation=1u;
  image.pixel_generation=1u;
  image.width=8u;
  image.height=6u;
  image.pitch_pixels=8u;
  image.pixel_format=GML_PLAN_PIXEL_XRGB8888;
  image.opaque=1u;
  image.cpu_pixels=frame;
  gml_render_plan_reset(plan,GML_PLAN_TARGET_HOST_FRAMEBUFFER,16,12);
  if(!gml_render_plan_add_clear(plan,whole,0u)) return 0;
  source=gml_render_plan_add_image(plan,&image);
  if(source==GML_PLAN_NO_IMAGE) return 0;
  image.image_class=GML_PLAN_IMAGE_SPRITE;
  image.identity=(3u<<10)|0u;
  image.width=4u;
  image.height=4u;
  image.pitch_pixels=4u;
  image.opaque=0u;
  image.cpu_pixels=mask;
  sampler=gml_render_plan_add_image(plan,&image);
  if(sampler==GML_PLAN_NO_IMAGE) return 0;
  memset(&shader,0,sizeof shader);
  shader.identity=0u;
  shader.content_generation=1u;
  shader.vertex_es=CONTENT_VERTEX_ES;
  shader.fragment_es=CONTENT_FRAGMENT_ES;
  snprintf(shader.uniforms[0].name,sizeof shader.uniforms[0].name,"u_size");
  shader.uniforms[0].value[0]=4.0f; shader.uniforms[0].value[1]=4.0f; shader.uniforms[0].count=2u;
  snprintf(shader.uniforms[1].name,sizeof shader.uniforms[1].name,"u_time");
  shader.uniforms[1].value[0]=0.5f; shader.uniforms[1].count=1u;
  snprintf(shader.uniforms[2].name,sizeof shader.uniforms[2].name,"u_mode");
  shader.uniforms[2].value[0]=2.0f; shader.uniforms[2].count=1u; shader.uniforms[2].integer=1u;
  snprintf(shader.uniforms[3].name,sizeof shader.uniforms[3].name,"u_transform");
  for(int index=0;index<16;index++) shader.uniforms[3].value[index]=(index%5)?0.0f:1.0f;
  shader.uniforms[3].count=16u;
  shader.uniform_count=4u;
  snprintf(shader.samplers[0].name,sizeof shader.samplers[0].name,"samp_mask");
  shader.samplers[0].image=sampler;
  shader.sampler_count=1u;
  return gml_render_plan_add_shader_draw(plan,source,whole,&shader,1u);
}

static int content_sampler_filter_case(void){
  uint32_t frame[8*6]={0},mask[4*4]={0};
  GmlRenderPlan plan;
  anygm_test_graphics_reset();
  GmlGpu *gpu=gml_gpu_create();
  GmlGpuContext context=fake_context(GML_GPU_API_OPENGL_CORE);
  REQUIRE(gpu && gml_gpu_context_reset(gpu,&context),"sampler context adopted");
  REQUIRE(build_content_plan(&plan,frame,mask),"sampler plan built");
  plan.shader.samplers[0].image=plan.operations[1].source;
  REQUIRE(gml_gpu_execute_plan(gpu,&plan),"one image drawn through two sampler stages");
  REQUIRE(anygm_test_graphics_quad_filter(0)==GL_LINEAR &&
          anygm_test_graphics_quad_filter(1)==GL_NEAREST &&
          anygm_test_graphics_quad_texture(0)!=anygm_test_graphics_quad_texture(1),
          "opposite filters on one image require independent device texture state");
  plan.shader.samplers[0].linear=1;
  plan.operations[1].linear=0;
  REQUIRE(gml_gpu_execute_plan(gpu,&plan),"opposite filters reversed");
  REQUIRE(anygm_test_graphics_quad_filter(0)==GL_NEAREST &&
          anygm_test_graphics_quad_filter(1)==GL_LINEAR,
          "cached pictures preserve reversed sampler filters");
  gml_gpu_destroy(gpu,1);
  return 1;
}

static int content_program_case(void){
  uint32_t frame[8*6],mask[4*4];
  GmlGpu *gpu;
  GmlGpuContext context;
  GmlRenderPlan plan;
  const char *text;
  anygm_test_graphics_reset();
  for(size_t index=0;index<8*6;index++) frame[index]=0xFF000000u|(uint32_t)index;
  for(size_t index=0;index<4*4;index++) mask[index]=0x80FFFFFFu;
  gpu=gml_gpu_create();
  context=fake_context(GML_GPU_API_OPENGL_CORE);
  REQUIRE(gml_gpu_context_reset(gpu,&context),"context adopted");
  REQUIRE(build_content_plan(&plan,frame,mask),"content plan built");
  REQUIRE(gml_render_plan_validate(&plan),"content plan validates");
  REQUIRE(gml_gpu_execute_plan(gpu,&plan),"the content program presents the frame");
  REQUIRE(anygm_test_graphics_quad_draw_calls()==1,"one quad is drawn through the content program");
  REQUIRE(anygm_test_graphics_quad_filter(0)==GL_LINEAR &&
          anygm_test_graphics_quad_filter(1)==GL_NEAREST,
          "base and auxiliary textures retain their selected filters");
  REQUIRE(anygm_test_graphics_attributes_enabled()==0,"no attribute array is left enabled");
  REQUIRE(anygm_test_graphics_bound_buffer()==0,"no vertex buffer is left bound");
  REQUIRE(anygm_test_graphics_float_uniforms()==2,"the vector and scalar values are set as floats");
  REQUIRE(anygm_test_graphics_matrix_uniforms()==6,"the five runtime matrices and the content's own are set");
  /* The text handed to the driver is the content's, spelled in the dialect the context accepts:
   * the older storage qualifiers, sampling call and fragment output are the only words that
   * change, and a channel-order helper is the only line that is added. */
  text=anygm_test_graphics_shader_source(0);
  REQUIRE(strstr(text,"#version 330 core")==text,"the vertex text targets the core dialect");
  REQUIRE(strstr(text,"in vec3 in_Position;")!=NULL,"the attribute qualifier is respelled");
  REQUIRE(strstr(text,"out vec2 v_vTexcoord;")!=NULL,"a vertex varying becomes an output");
  REQUIRE(strstr(text,"attribute")==NULL && strstr(text,"varying")==NULL,"no older qualifier remains");
  REQUIRE(strstr(text,"gm_Matrices[MATRIX_WORLD_VIEW_PROJECTION] * vec4(in_Position, 1.0)")!=NULL,
          "the content's own expression is untouched");
  text=anygm_test_graphics_shader_source(1);
  REQUIRE(strstr(text,"#version 330 core")==text,"the fragment text targets the core dialect");
  REQUIRE(strstr(text,"precision mediump float;")==NULL,"a precision statement is dropped for the desktop dialect");
  REQUIRE(strstr(text,"out vec4 anygm_fragment;")!=NULL,"the fragment output is declared");
  REQUIRE(strstr(text,"anygm_fragment = v_vColour * base * mask;")!=NULL,"the fragment output is respelled");
  REQUIRE(strstr(text,"anygm_sample(gm_BaseTexture, v_vTexcoord)")!=NULL,"sampling goes through the channel-order helper");
  REQUIRE(strstr(text,"anygm_sample(samp_mask, v_vTexcoord * u_size)")!=NULL,"the content's sampler goes through the helper too");
  REQUIRE(strstr(text,"texture2D")==NULL && strstr(text,"gl_FragColor")==NULL,"no older sampling call or output remains");
  REQUIRE(strstr(text,"in vec2 v_vTexcoord;")!=NULL,"a fragment varying becomes an input");
  REQUIRE(strstr(text,".bgra")!=NULL || strstr(text,".gbar")!=NULL,"the helper reorders the channels of the uploaded word");
  /* A second presentation reuses the program: nothing is compiled again, and it draws again. */
  {
    int deletes=anygm_test_graphics_deletes();
    REQUIRE(build_content_plan(&plan,frame,mask),"content plan rebuilt");
    REQUIRE(gml_gpu_execute_plan(gpu,&plan),"the cached program presents the frame");
    REQUIRE(anygm_test_graphics_quad_draw_calls()==2,"a second quad is drawn");
    REQUIRE(anygm_test_graphics_deletes()==deletes,"the cached program is not rebuilt");
  }
  {
    GmlGpuCounters counters;
    gml_gpu_counters(gpu,&counters);
    REQUIRE(counters.program_failures==0,"no program failed");
    REQUIRE(counters.fallbacks[GML_PLAN_FALLBACK_SHADER_FAILURE]==0,"no shader fallback was taken");
  }
  gml_gpu_destroy(gpu,1);
  return 1;
}

static int content_program_identity_and_limits_case(void){
  uint32_t frame[8*6]={0},mask[4*4]={0};
  char vertex[sizeof CONTENT_VERTEX_ES+48],fragment[sizeof CONTENT_FRAGMENT_ES+48];
  GmlRenderPlan plan;
  GmlGpu *gpu;
  GmlGpuContext context=fake_context(GML_GPU_API_OPENGL_CORE);
  unsigned compiled;
  anygm_test_graphics_reset();
  gpu=gml_gpu_create();
  REQUIRE(gpu && gml_gpu_context_reset(gpu,&context),"content-limit context adopted");
  REQUIRE(build_content_plan(&plan,frame,mask),"content-limit plan built");
  snprintf(vertex,sizeof vertex,"%s",CONTENT_VERTEX_ES);
  snprintf(fragment,sizeof fragment,"%s",CONTENT_FRAGMENT_ES);
  plan.shader.vertex_es=vertex;
  plan.shader.fragment_es=fragment;
  REQUIRE(gml_gpu_execute_plan(gpu,&plan),"initial content program accepted");
  compiled=anygm_test_graphics_compile_calls();
  plan.shader.uniforms[1].value[0]=0.75f;
  REQUIRE(gml_gpu_execute_plan(gpu,&plan) &&
          anygm_test_graphics_compile_calls()==compiled,
          "uniform values do not change compiled program identity");
  strcat(fragment,"/* fragment revision */");
  REQUIRE(gml_gpu_execute_plan(gpu,&plan) &&
          anygm_test_graphics_compile_calls()>compiled &&
          strstr(anygm_test_graphics_shader_source(1),"fragment revision")!=NULL,
          "changed fragment bytes at the same address compile a new program");
  compiled=anygm_test_graphics_compile_calls();
  strcat(vertex,"/* vertex revision */");
  REQUIRE(gml_gpu_execute_plan(gpu,&plan) &&
          anygm_test_graphics_compile_calls()>compiled &&
          strstr(anygm_test_graphics_shader_source(0),"vertex revision")!=NULL,
          "changed vertex bytes at the same address compile a new program");
  compiled=anygm_test_graphics_compile_calls();
  plan.shader.content_generation++;
  REQUIRE(gml_gpu_execute_plan(gpu,&plan) &&
          anygm_test_graphics_compile_calls()>compiled,
          "a new content generation does not inherit an old program");
  plan.shader.content_generation++;
  anygm_test_graphics_active_uniform_limit(65);
  REQUIRE(!gml_gpu_execute_plan(gpu,&plan) &&
          plan.fallback_reason==GML_PLAN_FALLBACK_SHADER_FAILURE,
          "too many active uniforms refuse instead of truncating");
  anygm_test_graphics_active_uniform_limit(-1);
  plan.shader.content_generation++;
  REQUIRE(gml_gpu_execute_plan(gpu,&plan),"a new program identity can recover after refusal");
  gml_gpu_destroy(gpu,1);
  anygm_test_graphics_reset();
  anygm_test_graphics_texture_unit_limit(1);
  gpu=gml_gpu_create();
  REQUIRE(gpu && gml_gpu_context_reset(gpu,&context),"limited-unit context adopted");
  REQUIRE(build_content_plan(&plan,frame,mask),"limited-unit plan built");
  REQUIRE(!gml_gpu_execute_plan(gpu,&plan) &&
          plan.fallback_reason==GML_PLAN_FALLBACK_SHADER_FAILURE,
          "one texture unit refuses base plus auxiliary sampler");
  gml_gpu_destroy(gpu,1);
  anygm_test_graphics_reset();
  gpu=gml_gpu_create();
  REQUIRE(gpu && gml_gpu_context_reset(gpu,&context),"adequate-unit context adopted");
  REQUIRE(build_content_plan(&plan,frame,mask) && gml_gpu_execute_plan(gpu,&plan),
          "adequate texture units accept the same program in a new epoch");
  gml_gpu_destroy(gpu,1);
  return 1;
}

static int content_program_embedded_dialect_case(void){
  uint32_t frame[8*6],mask[4*4];
  GmlGpu *gpu;
  GmlGpuContext context;
  GmlRenderPlan plan;
  const char *text;
  anygm_test_graphics_reset();
  memset(frame,0,sizeof frame);
  memset(mask,0,sizeof mask);
  gpu=gml_gpu_create();
  context=fake_context(GML_GPU_API_OPENGLES3);
  REQUIRE(gml_gpu_context_reset(gpu,&context),"embedded context adopted");
  REQUIRE(build_content_plan(&plan,frame,mask),"content plan built");
  REQUIRE(gml_gpu_execute_plan(gpu,&plan),"the content program presents the frame");
  text=anygm_test_graphics_shader_source(1);
  REQUIRE(strstr(text,"#version 300 es")==text,"the fragment text targets the embedded dialect");
  REQUIRE(strstr(text,"precision highp float;")!=NULL,"a default precision is declared for the embedded dialect");
  REQUIRE(strstr(text,"precision mediump float;")!=NULL,"the content's own precision statement is kept for the embedded dialect");
  gml_gpu_destroy(gpu,1);
  return 1;
}

/* Older authored GLSL can initialize a mutable global from a uniform. Modern GLES requires that
 * work to happen at entry, in declaration order, while helper functions keep reading the global. */
static int content_program_global_initializers_case(void){
  static const char fragment[]=
    "precision mediump float;\n"
    "varying vec2 v_vTexcoord;\n"
    "uniform vec2 u_size;\n"
    "#define DECLARE_FAKE \\\n"
    "  float macro_only = 8.0;\n"
    "// float comment_only = 3.0;\n"
    "const float fixed = 0.5;\n"
    "float paired = 1.0, other = 2.0;\n"
    "vec2 stretch = vec2(u_size.x / u_size.y, 1.0);\n"
    "float bias = stretch.x * 0.25;\n"
    "float stable = 0.5;\n"
    "vec4 sample_colour(){ return vec4(stretch.x + bias + stable); }\n"
    "void main(){\n"
    "  float local = v_vTexcoord.x;\n"
    "  gl_FragColor = sample_colour() + vec4(local);\n"
    "}\n";
  uint32_t frame[8*6]={0},mask[4*4]={0};
  GmlGpu *gpu;
  GmlGpuContext context;
  GmlRenderPlan plan;
  const char *text;
  const char *entry;
  const char *stretch;
  const char *bias;
  const char *stable;
  for(int api=0;api<2;api++){
    anygm_test_graphics_reset();
    gpu=gml_gpu_create();
    context=fake_context(api?GML_GPU_API_OPENGL_CORE:GML_GPU_API_OPENGLES3);
    REQUIRE(gpu && gml_gpu_context_reset(gpu,&context),"global-init context adopted");
    REQUIRE(build_content_plan(&plan,frame,mask),"global-init plan built");
    plan.shader.fragment_es=fragment;
    REQUIRE(gml_gpu_execute_plan(gpu,&plan),"global-init content program submitted");
    text=anygm_test_graphics_shader_source(1);
    REQUIRE(strstr(text,"vec2 stretch =") == NULL &&
            strstr(text,"float bias =") == NULL &&
            strstr(text,"float stable =") == NULL,
            "modern source declares mutable globals without initializers");
    REQUIRE(strstr(text,"float macro_only = 8.0;")!=NULL &&
            strstr(text,"// float comment_only = 3.0;")!=NULL &&
            strstr(text,"const float fixed = 0.5;")!=NULL &&
            strstr(text,"float paired = 1.0, other = 2.0;")!=NULL,
            "comments, constant globals and multi-declarators retain authored text");
    entry=strstr(text,"void main(){");
    REQUIRE(entry!=NULL,"entry remains in authored source");
    stretch=strstr(entry,"stretch = vec2(u_size.x / u_size.y, 1.0);");
    bias=strstr(entry,"bias = stretch.x * 0.25;");
    stable=strstr(entry,"stable = 0.5;");
    REQUIRE(stretch && bias && stable && stretch<bias && bias<stable,
            "authored global expressions execute at entry in declaration order");
    REQUIRE(strstr(entry,"float local = v_vTexcoord.x;")!=NULL,
            "local initialization is untouched");
    gml_gpu_destroy(gpu,1);
  }
  return 1;
}

/* A program the context refuses is a shader failure, named as such, so the runtime can retire the
 * shader rather than pay the refusal every frame; and the refusal is not repeated for the same
 * text. */
static int content_program_refused_case(void){
  uint32_t frame[8*6],mask[4*4];
  GmlGpu *gpu;
  GmlGpuContext context;
  GmlRenderPlan plan;
  anygm_test_graphics_reset();
  memset(frame,0,sizeof frame);
  memset(mask,0,sizeof mask);
  gpu=gml_gpu_create();
  context=fake_context(GML_GPU_API_OPENGL_CORE);
  REQUIRE(gml_gpu_context_reset(gpu,&context),"context adopted");
  anygm_test_graphics_fail_compile(1);
  REQUIRE(build_content_plan(&plan,frame,mask),"content plan built");
  REQUIRE(!gml_gpu_execute_plan(gpu,&plan),"a program that will not compile is refused");
  REQUIRE(plan.fallback_reason==GML_PLAN_FALLBACK_SHADER_FAILURE,"the refusal is the shader's");
  REQUIRE(anygm_test_graphics_quad_draw_calls()==0,"nothing is drawn through a refused program");
  {
    const char *error=gml_gpu_last_error(gpu);
    REQUIRE(error && strstr(error,"compile")!=NULL,"the reason names the failing stage");
  }
  anygm_test_graphics_fail_compile(0);
  {
    /* The same text is not compiled again: the answer stands for the context's lifetime. */
    int deletes=anygm_test_graphics_deletes();
    REQUIRE(build_content_plan(&plan,frame,mask),"content plan rebuilt");
    REQUIRE(!gml_gpu_execute_plan(gpu,&plan),"the refusal stands");
    REQUIRE(plan.fallback_reason==GML_PLAN_FALLBACK_SHADER_FAILURE,"and is still the shader's");
    REQUIRE(anygm_test_graphics_deletes()==deletes,"no second attempt was made");
  }
  {
    GmlGpuCounters counters;
    gml_gpu_counters(gpu,&counters);
    REQUIRE(counters.program_failures==2,"each refused presentation is counted");
    REQUIRE(counters.fallbacks[GML_PLAN_FALLBACK_SHADER_FAILURE]==2,"as a shader fallback");
    REQUIRE(counters.fallbacks[GML_PLAN_FALLBACK_RESOURCE_UPLOAD_FAILURE]==0,"not as an upload failure");
  }
  /* A program without the position attribute cannot be fed a quad. */
  anygm_test_graphics_withhold_attributes(1);
  {
    GmlRenderPlan other;
    REQUIRE(build_content_plan(&other,frame,mask),"content plan built");
    other.shader.identity=7u;
    REQUIRE(!gml_gpu_execute_plan(gpu,&other),"a program without in_Position is refused");
    REQUIRE(other.fallback_reason==GML_PLAN_FALLBACK_SHADER_FAILURE,"as a shader failure");
  }
  gml_gpu_destroy(gpu,1);
  return 1;
}

/* A plan whose target is read back: the program runs into an off-screen target of the plan's
 * extent, and the result lands in the caller's plane rows-from-the-top in the runtime's word. */
static int content_program_readback_case(void){
  enum { WIDTH=5,HEIGHT=3 };
  uint32_t frame[8*6],mask[4*4];
  uint32_t output[WIDTH*HEIGHT];
  GmlGpu *gpu;
  GmlGpuContext context;
  GmlRenderPlan plan;
  anygm_test_graphics_reset();
  memset(frame,0,sizeof frame);
  memset(mask,0,sizeof mask);
  memset(output,0xAA,sizeof output);
  gpu=gml_gpu_create();
  context=fake_context(GML_GPU_API_OPENGL_CORE);
  REQUIRE(gml_gpu_context_reset(gpu,&context),"context adopted");
  REQUIRE(build_content_plan(&plan,frame,mask),"content plan built");
  /* The same operations over an off-screen target of another extent. */
  plan.target=GML_PLAN_TARGET_READBACK;
  plan.target_width=WIDTH;
  plan.target_height=HEIGHT;
  plan.operations[0].destination.width=WIDTH;
  plan.operations[0].destination.height=HEIGHT;
  plan.operations[1].destination.width=WIDTH;
  plan.operations[1].destination.height=HEIGHT;
  REQUIRE(!gml_render_plan_validate(&plan),"a read-back target without a plane does not validate");
  plan.readback_pixels=output;
  plan.readback_pitch_pixels=WIDTH;
  REQUIRE(gml_render_plan_validate(&plan),"a read-back target with a plane validates");
  REQUIRE(gml_gpu_execute_plan(gpu,&plan),"the program executes into the off-screen target");
  REQUIRE(anygm_test_graphics_quad_draw_calls()==1,"one quad is drawn");
  REQUIRE(anygm_test_graphics_read_pixels()==1,"the target is read back once");
  REQUIRE(anygm_test_graphics_attached_texture()!=0,"a texture is attached to the off-screen target");
  /* The driver hands rows from the bottom with the device row in the red byte and the column in
   * the green byte; the plane holds rows from the top in ARGB words. */
  for(int row=0;row<HEIGHT;row++)
    for(int column=0;column<WIDTH;column++){
      uint32_t expected=0xFF000000u|((uint32_t)(HEIGHT-1-row)<<16)|((uint32_t)column<<8)|0x33u;
      if(output[row*WIDTH+column]!=expected){
        fprintf(stderr,"gpu read-back at %d,%d: %08x != %08x\n",column,row,output[row*WIDTH+column],expected);
        return 0;
      }
    }
  REQUIRE(anygm_test_graphics_attributes_enabled()==0,"no attribute array is left enabled");
  gml_gpu_destroy(gpu,1);
  return 1;
}

/* A draw showing part of its source shades that part: the quad's texture coordinates span the
 * named region rather than the whole image. */
static int content_program_source_region_case(void){
  uint32_t frame[8*6],mask[4*4];
  GmlGpu *gpu;
  GmlGpuContext context;
  GmlRenderPlan plan;
  float u=0.0f,v=0.0f;
  anygm_test_graphics_reset();
  memset(frame,0,sizeof frame);
  memset(mask,0,sizeof mask);
  gpu=gml_gpu_create();
  context=fake_context(GML_GPU_API_OPENGL_CORE);
  REQUIRE(gml_gpu_context_reset(gpu,&context),"context adopted");
  REQUIRE(build_content_plan(&plan,frame,mask),"content plan built");
  /* The source is 8x6; show the 4x3 quadrant at (2,3). */
  plan.operations[1].source_rect.x=2;
  plan.operations[1].source_rect.y=3;
  plan.operations[1].source_rect.width=4u;
  plan.operations[1].source_rect.height=3u;
  REQUIRE(gml_render_plan_validate(&plan),"a region inside the source validates");
  REQUIRE(gml_gpu_execute_plan(gpu,&plan),"the program runs over the region");
  REQUIRE(anygm_test_graphics_quad_texcoord(0,&u,&v),"the quad was handed to the driver");
  REQUIRE(u>0.249f && u<0.251f && v>0.499f && v<0.501f,"the first corner is the region's origin");
  REQUIRE(anygm_test_graphics_quad_texcoord(3,&u,&v),"the opposite corner too");
  REQUIRE(u>0.749f && u<0.751f && v>0.999f && v<1.001f,"which ends where the region ends");
  /* A region that leaves the source is refused before anything is uploaded. */
  REQUIRE(build_content_plan(&plan,frame,mask),"content plan rebuilt");
  plan.operations[1].source_rect.x=6;
  plan.operations[1].source_rect.y=0;
  plan.operations[1].source_rect.width=4u;
  plan.operations[1].source_rect.height=3u;
  REQUIRE(!gml_render_plan_validate(&plan),"a region running past the source is rejected");
  gml_gpu_destroy(gpu,1);
  return 1;
}

/* Taking the pipelined read-back: the first pass primes by waiting once, and every pass after it
 * hands back what the previous one left and starts its own without waiting. */
static int content_program_pipelined_readback_case(void){
  enum { W=5,H=3 };
  uint32_t frame[8*6],mask[4*4],output[W*H];
  GmlGpu *gpu;
  GmlGpuContext context;
  GmlRenderPlan plan;
  anygm_test_graphics_reset();
  memset(frame,0,sizeof frame);
  memset(mask,0,sizeof mask);
  gpu=gml_gpu_create();
  context=fake_context(GML_GPU_API_OPENGL_CORE);
  REQUIRE(gml_gpu_context_reset(gpu,&context),"context adopted");
  gml_gpu_set_readback_pipelined(gpu,1);
  {
    /* First pass: nothing to take yet, so it reads the waiting way exactly once. */
    REQUIRE(build_content_plan(&plan,frame,mask),"content plan built");
    plan.target=GML_PLAN_TARGET_READBACK;
    plan.target_width=W; plan.target_height=H;
    plan.readback_pixels=output; plan.readback_pitch_pixels=W;
    plan.operations[0].destination.width=W; plan.operations[0].destination.height=H;
    plan.operations[1].destination.width=W; plan.operations[1].destination.height=H;
    REQUIRE(gml_gpu_execute_plan(gpu,&plan),"the first pass executes");
    REQUIRE(anygm_test_graphics_read_pixels()==1,"it waits for the device once");
    REQUIRE(anygm_test_graphics_async_read_pixels()==1,"and starts its own read without waiting");
  }
  {
    /* Second pass: it takes what the first left and waits for nothing. */
    unsigned waited=(unsigned)anygm_test_graphics_read_pixels();
    REQUIRE(build_content_plan(&plan,frame,mask),"content plan rebuilt");
    plan.target=GML_PLAN_TARGET_READBACK;
    plan.target_width=W; plan.target_height=H;
    plan.readback_pixels=output; plan.readback_pitch_pixels=W;
    plan.operations[0].destination.width=W; plan.operations[0].destination.height=H;
    plan.operations[1].destination.width=W; plan.operations[1].destination.height=H;
    REQUIRE(gml_gpu_execute_plan(gpu,&plan),"the second pass executes");
    REQUIRE((unsigned)anygm_test_graphics_read_pixels()==waited,"nothing waited for the device");
    REQUIRE(anygm_test_graphics_async_read_pixels()==2,"and its own read was started");
  }
  gml_gpu_destroy(gpu,1);
  return 1;
}

/* The drawing colour reaches the program as quad vertex colour; applying it after
 * program evaluation matters when the result saturates. */
static int content_program_vertex_colour_case(void){
  uint32_t frame[8*6],mask[4*4];
  GmlGpu *gpu;
  GmlGpuContext context;
  GmlRenderPlan plan;
  float rgba[4];
  anygm_test_graphics_reset();
  memset(frame,0,sizeof frame);
  memset(mask,0,sizeof mask);
  gpu=gml_gpu_create();
  context=fake_context(GML_GPU_API_OPENGL_CORE);
  REQUIRE(gml_gpu_context_reset(gpu,&context),"context adopted");
  REQUIRE(build_content_plan(&plan,frame,mask),"content plan built");
  REQUIRE(anygm_test_graphics_quad_colour(0,rgba)==0 || 1,"fixture ready");
  plan.shader.vertex_colour[0]=0.25f;
  plan.shader.vertex_colour[1]=0.50f;
  plan.shader.vertex_colour[2]=0.75f;
  plan.shader.vertex_colour[3]=0.5f;
  REQUIRE(gml_gpu_execute_plan(gpu,&plan),"the program presents the frame");
  for(int corner=0;corner<4;corner++){
    REQUIRE(anygm_test_graphics_quad_colour(corner,rgba),"the quad was handed to the driver");
    REQUIRE(rgba[0]>0.24f && rgba[0]<0.26f && rgba[1]>0.49f && rgba[1]<0.51f &&
            rgba[2]>0.74f && rgba[2]<0.76f && rgba[3]>0.49f && rgba[3]<0.51f,
            "every corner carries the drawing's colour");
  }
  /* A caller with nothing to say leaves the quad white, so a program that multiplies by it is
   * unchanged. */
  REQUIRE(build_content_plan(&plan,frame,mask),"content plan rebuilt");
  REQUIRE(gml_gpu_execute_plan(gpu,&plan),"the program presents again");
  REQUIRE(anygm_test_graphics_quad_colour(0,rgba),"the quad was handed over");
  REQUIRE(rgba[0]>0.99f && rgba[1]>0.99f && rgba[2]>0.99f && rgba[3]>0.99f,
          "an unstated colour is white");
  gml_gpu_destroy(gpu,1);
  return 1;
}

int main(int argc,char **argv){
  const char *filter=NULL;
  for(int index=1;index<argc;++index){
    if(strcmp(argv[index],"--case")) continue;
    if(index+1>=argc){
      fprintf(stderr,"--case requires a filter\n");
      return EXIT_FAILURE;
    }
    filter=argv[++index];
  }
  static const AnygmTestCase lifecycle_cases[]={
    {"reset",lifecycle_reset_case},
    {"context_loss",lifecycle_loss_case},
    {"scene_preparation",scene_preparation_case},
    {"unbound_context_access",unbound_context_access_case},
    {"completed_frame_snapshot",completed_frame_snapshot_case},
    {"integer_presentation_shift",integer_presentation_shift_case},
    {"destroy_without_context",lifecycle_destroy_without_context_case},
    {"missing_entry_point",missing_entry_point_case},
    {"program_failure",program_failure_case},
    {"scene_service_contract",scene_service_contract_case},
    {"scene_staged_upload",scene_staged_upload_case},
    {"scene_optional_entry_refusal",scene_optional_entry_refusal_case},
    {"scene_content_source_orientation",scene_content_source_orientation_case},
    {"scene_resident_batch",scene_resident_batch_case},
    {"scene_particle_line_batch",scene_particle_line_batch_case},
    {"scene_particle_disc_batch",scene_particle_disc_batch_case},
    {"scene_classic_alpha_rect_batch",scene_classic_alpha_rect_batch_case},
    {"scene_studio_alpha_rect_batch",scene_studio_alpha_rect_batch_case},
    {"scene_rectangle_function_batch",scene_rectangle_function_batch_case},
    {"scene_classic_inverse_rect_batch",scene_classic_inverse_rect_batch_case},
    {"scene_hardware_additive_batch",scene_hardware_additive_batch_case},
    {"scene_hardware_background_batch",scene_hardware_background_batch_case},
    {"scene_hardware_combined_batch",scene_hardware_combined_batch_case},
    {"scene_hardware_surface_batch",scene_hardware_surface_batch_case},
    {"scene_nearest_surface_batch",scene_nearest_surface_batch_case},
    {"scene_nearest_application_batch",scene_nearest_application_batch_case},
    {"scene_integer_screen_surface_batch",scene_integer_screen_surface_batch_case},
    {"scene_hardware_screen_stretch_batch",scene_hardware_screen_stretch_batch_case},
    {"scene_hardware_screen_sprite_batch",scene_hardware_screen_sprite_batch_case},
    {"scene_hardware_screen_font_batch",scene_hardware_screen_font_batch_case},
    {"scene_hardware_screen_trim_batch",scene_hardware_screen_trim_batch_case},
    {"scene_application_point_batch",scene_application_point_batch_case},
    {"scene_destination_surface",scene_destination_surface_case},
    {"scene_readback_formats",scene_readback_formats_case},
    {"incremental_checkpoint_contract",incremental_checkpoint_contract_case},
    {"incremental_checkpoint_budget",incremental_checkpoint_budget_case},
    {"scene_atlas_retire_advanced_version",scene_atlas_retire_advanced_version_case},
    {"scene_alpha_composition",scene_alpha_composition_case},
    {"scene_opaque_target_sourceover",scene_opaque_target_sourceover_case},
    {"scene_integer_function_sprite",scene_integer_function_sprite_case},
    {"scene_source_alpha_key",scene_source_alpha_key_case},
    {"scene_raw_copy",scene_raw_copy_case},
    {"scene_nearest_rows",scene_nearest_rows_case},
    {"scene_nearest_surface_function",scene_nearest_surface_function_case},
    {"scene_rotated_surface_point",scene_rotated_surface_point_case},
    {"scene_tiled_surface_packets",scene_tiled_surface_packets_case},
    {"scene_gui_tinted_text",scene_gui_tinted_text_case},
    {"scene_gui_filtered_background_batch",scene_gui_filtered_background_batch_case},
    {"scene_clamped_atlas_window",scene_clamped_atlas_window_case},
    {"scene_gui_half_point",scene_gui_half_point_case},
    {"scene_numbered_integer_sourceover",scene_numbered_integer_sourceover_case},
    {"scene_integer_endpoint_sprite",scene_integer_endpoint_sprite_case},
    {"scene_endpoint_source_sprite",scene_endpoint_source_sprite_case},
    {"scene_gles_font_programs",scene_gles_font_programs_case},
    {"scene_screen_filtered_alpha_stamp",scene_screen_filtered_alpha_stamp_case},
    {"scene_numbered_sprite_channel_masks",scene_numbered_sprite_channel_masks_case},
    {"scene_numbered_rotated_texel_spans",scene_numbered_rotated_texel_spans_case},
    {"scene_completed_screen_query",scene_completed_screen_query_case},
    {"scene_empty_screen_recovery",scene_empty_screen_recovery_case},
  };
  static const AnygmTestCase execution_cases[]={
    {"state_and_geometry",execute_state_and_geometry_case},
    {"upload_and_reuse",upload_and_reuse_case},
    {"reduction_falls_back",reduction_falls_back_case},
    {"no_context_falls_back",no_context_falls_back_case},
    {"content_program",content_program_case},
    {"content_program_identity_and_limits",content_program_identity_and_limits_case},
    {"content_sampler_filters",content_sampler_filter_case},
    {"content_program_embedded_dialect",content_program_embedded_dialect_case},
    {"content_program_global_initializers",content_program_global_initializers_case},
    {"content_program_refused",content_program_refused_case},
    {"content_program_readback",content_program_readback_case},
    {"content_program_source_region",content_program_source_region_case},
    {"content_program_pipelined_readback",content_program_pipelined_readback_case},
    {"content_program_vertex_colour",content_program_vertex_colour_case},
  };
  const AnygmTestGroup groups[]={
    {"lifecycle",lifecycle_cases,sizeof lifecycle_cases/sizeof lifecycle_cases[0]},
    {"execution",execution_cases,sizeof execution_cases/sizeof execution_cases[0]},
  };
  AnygmTestResult result;
  anygm_test_run_groups(groups,sizeof groups/sizeof groups[0],filter,&result);
  printf("gpu cases: passed=%d failed=%d\n",result.passed,result.failed);
  return result.failed?EXIT_FAILURE:EXIT_SUCCESS;
}
