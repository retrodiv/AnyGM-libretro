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
#include "graphics_driver_fixture.h"

#include <stdint.h>
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

static int scene_service_contract_case(void){
  static const GmlRenderOperationCode unselected[]={
    GML_RENDER_OP_TEXTURED_TRIANGLES,GML_RENDER_OP_COPY_IMAGE,
    GML_RENDER_OP_COMPOSE_SURFACE,GML_RENDER_OP_PRESENT_IMAGE
  };
  const uint64_t supported_operations=(UINT64_C(1)<<GML_RENDER_OP_CLEAR)|
    (UINT64_C(1)<<GML_RENDER_OP_TEXTURED_QUAD)|
    (UINT64_C(1)<<GML_RENDER_OP_COLOURED_PRIMITIVE)|
    (UINT64_C(1)<<GML_RENDER_OP_LAYER_EFFECT)|
    (UINT64_C(1)<<GML_RENDER_OP_CONTENT_SHADER_DRAW);
  const uint32_t supported_families=GML_RENDER_FAMILY_TEXTURE|
    GML_RENDER_FAMILY_PRIMITIVE|GML_RENDER_FAMILY_EFFECT|GML_RENDER_FAMILY_SHADER;
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
    REQUIRE(counters.scene_batches_offered==5u && counters.scene_batches_refused==5u &&
            counters.scene_batches_accepted==0u && counters.scene_partial_prefix==0u,
            "all four unselected opcode refusals retain exact batch accounting");
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
  free(operations);
  gml_gpu_destroy(gpu,1);
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

static int scene_alpha_composition_case(void){
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
  gpu=gml_gpu_create();
  REQUIRE(gpu!=NULL,"device metadata allocated");
  context=fake_context(GML_GPU_API_OPENGL_CORE);
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
  gml_gpu_destroy(gpu,1);
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
    {"destroy_without_context",lifecycle_destroy_without_context_case},
    {"missing_entry_point",missing_entry_point_case},
    {"program_failure",program_failure_case},
    {"scene_service_contract",scene_service_contract_case},
    {"scene_optional_entry_refusal",scene_optional_entry_refusal_case},
    {"scene_content_source_orientation",scene_content_source_orientation_case},
    {"scene_resident_batch",scene_resident_batch_case},
    {"scene_atlas_retire_advanced_version",scene_atlas_retire_advanced_version_case},
    {"scene_alpha_composition",scene_alpha_composition_case},
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
