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

static int scene_service_contract_case(void){
  static const GmlRenderOperationCode unselected[]={
    GML_RENDER_OP_TEXTURED_TRIANGLES,GML_RENDER_OP_COPY_IMAGE,
    GML_RENDER_OP_COMPOSE_SURFACE,GML_RENDER_OP_PRESENT_IMAGE
  };
  const uint64_t supported_operations=(UINT64_C(1)<<GML_RENDER_OP_CLEAR)|
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
    for(unsigned variant=0;variant<10u;variant++){
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

/* Call/state proof only. Composed pixel qualification needs a real device. */
static int scene_hardware_filtered_batch_case(unsigned kind){
  enum { QUADS=2049 };
  int additive=kind==1u,combined=kind==2u,surface=kind>=3u,nearest=kind>=4u;
  const uint8_t pixels[16]={11,22,33,255,44,55,66,100,77,88,99,0,111,122,133,200};
  for(unsigned api=0;api<2u;api++)
  for(unsigned excluded=0;excluded<(nearest?8u:(combined || surface)?7u:4u);excluded++){
    GmlGpuContext context=fake_context(api?GML_GPU_API_OPENGLES3:GML_GPU_API_OPENGL_CORE);
    GmlRenderImageRef source={kind==5u?GML_RENDER_RESOURCE_APPLICATION:surface?GML_RENDER_RESOURCE_SURFACE:
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
    upload.image=source;upload.storage_width=upload.storage_height=2u;
    upload.region=(GmlRenderRect){0,0,2u,2u};upload.pixel_format=surface?
      GML_RENDER_PIXEL_ARGB8888:GML_RENDER_PIXEL_RGBA8888;
    upload.bytes=pixels;upload.byte_capacity=sizeof pixels;upload.stride_bytes=8u;
    REQUIRE(device.upload(device.context,&upload),"hardware batch atlas uploaded");
    state.write_mask=15u;state.alpha=255u;state.colour=0xFFFFFFFFu;state.sampler_count=1u;
    state.samplers[0].filter=nearest?GML_RENDER_FILTER_NEAREST:GML_RENDER_FILTER_HARDWARE_BILINEAR;
    state.samplers[0].edge_rule=GML_RENDER_EDGE_CLAMP_SUBIMAGE;
    state.samplers[0].source_region=upload.region;
    if(combined) state.samplers[0].quantisation=GML_RENDER_QUANTISE_HARDWARE_COMBINED;
    state.rgb_source_factor=state.rgb_destination_factor=GML_RENDER_BLEND_ONE;
    if(combined) state.rgb_source_factor=GML_RENDER_BLEND_SOURCE_ALPHA;
    if(!additive) state.rgb_destination_factor=GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA;
    state.alpha_source_factor=GML_RENDER_BLEND_SOURCE_ALPHA;
    if(surface){
      state.rgb_source_factor=GML_RENDER_BLEND_SOURCE_ALPHA;
      state.alpha_source_factor=GML_RENDER_BLEND_ONE;
      state.coverage_policy=GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA;
    }
    state.alpha_destination_factor=GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA;
    state.rgb_equation=state.alpha_equation=GML_RENDER_BLEND_ADD;
    for(unsigned index=0;index<QUADS+1u;index++){
      GmlRenderDrawOp *op=&ops[index];
      op->sequence=index+1u;
      op->target.image=(GmlRenderImageRef){combined?GML_RENDER_RESOURCE_APPLICATION:
        GML_RENDER_RESOURCE_SURFACE,92u,1u,index+1u};
      op->target.storage_version=1u;op->target.width=op->target.height=64u;
      op->target.pixel_format=GML_RENDER_PIXEL_ARGB8888;
      op->target.region=(GmlRenderRect){0,0,64u,64u};
      op->opcode=index?GML_RENDER_OP_TEXTURED_QUAD:GML_RENDER_OP_CLEAR;
      if(!index){op->payload.clear_colour=0x40123456u;continue;}
      op->reconstruction_mode=GML_RENDER_DEVICE_CHECKPOINT;
      op->scene_family=surface?GML_RENDER_SCENE_SURFACE:
        additive?GML_RENDER_SCENE_SPRITE:GML_RENDER_SCENE_BACKGROUND;
      op->sources=&source;op->source_count=1u;
      GmlRenderQuad *quad=&op->payload.quad;
      quad->shape=GML_RENDER_QUAD_AXIS_ALIGNED;quad->sampling=state.samplers[0];
      quad->filtered_draw_alpha=additive || surface?.7:1.0;
      for(unsigned corner=0;corner<4u;corner++)
        quad->corner_colour[corner]=combined?0xFFFFFFFFu:0xB3123456u;
      quad->x[1]=quad->x[2]=combined || nearest?2.0:8.0;
      quad->y[2]=quad->y[3]=combined || nearest?2.0:8.0;
      quad->u[1]=quad->u[2]=quad->v[2]=quad->v[3]=1.0;
    }
    if(excluded==3u) ops[QUADS].payload.quad.filtered_draw_alpha=NAN;
    if(excluded==4u){
      if(surface) ops[QUADS].scene_family=GML_RENDER_SCENE_BACKGROUND;
      else for(unsigned index=0;index<QUADS+1u;index++)
        ops[index].target.image.resource_class=GML_RENDER_RESOURCE_SURFACE;
    }
    if(excluded==5u) ops[QUADS].payload.quad.corner_colour[0]=0xFFFEFFFFu;
    if(excluded==6u) ops[QUADS].payload.quad.u[1]=.9;
    if(excluded==7u) ops[QUADS].payload.quad.x[1]=ops[QUADS].payload.quad.x[2]=3.0;
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
      REQUIRE(anygm_test_graphics_scene_draw_calls()==(surface?4:2) &&
              anygm_test_graphics_scene_vertices()==QUADS*(surface?12:6),
              "only the declared quad capacity cuts compatible overlaps");
      REQUIRE(anygm_test_graphics_scene_filter()==(nearest?GL_NEAREST:GL_LINEAR) &&
              factors[0]==(combined || surface?GL_SRC_ALPHA:GL_ONE) &&
              factors[1]==(additive?GL_ONE:GL_ONE_MINUS_SRC_ALPHA) &&
              factors[2]==(surface?GL_ONE:GL_SRC_ALPHA) &&
              factors[3]==GL_ONE_MINUS_SRC_ALPHA,
              "hardware filtering and independent alpha factors are configured");
      REQUIRE(!anygm_test_graphics_scene_copies() && !anygm_test_graphics_read_pixels(),
              "ordinary overlap needs no destination snapshots or CPU readbacks");
    }
    gml_gpu_destroy(gpu,1);free(ops);
  }
  return 1;
}

static int scene_particle_line_batch_case(void){
  for(unsigned variant=0;variant<7u;variant++){
    enum { LINES=2049 };
    GmlRenderDrawOp *ops=(GmlRenderDrawOp*)calloc(LINES+1u,sizeof *ops);
    GmlGpuContext context=fake_context(GML_GPU_API_OPENGLES3);
    GmlRenderDeviceServices device={0};
    unsigned factors[4]={0};
    anygm_test_graphics_reset();
    if(variant==1u) anygm_test_graphics_withhold("glBlendFuncSeparate");
    GmlGpu *gpu=gml_gpu_create();
    REQUIRE(ops && gpu && gml_gpu_context_reset(gpu,&context),"particle context created");
    gml_gpu_device_services(gpu,&device);
    ops[0].opcode=GML_RENDER_OP_CLEAR;
    ops[0].target=(GmlRenderTargetRef){
      .image={GML_RENDER_RESOURCE_APPLICATION,141u,1u,2u},.storage_version=1u,
      .width=96u,.height=48u,.pixel_format=GML_RENDER_PIXEL_ARGB8888,
      .region={0,0,96u,48u}};
    ops[0].payload.clear_colour=0x49273859u;
    for(unsigned line=0;line<LINES;line++){
      ops[line+1u]=ops[0];
      ops[line+1u].opcode=GML_RENDER_OP_COLOURED_PRIMITIVE;
      ops[line+1u].scene_family=GML_RENDER_SCENE_PRIMITIVE;
      ops[line+1u].reconstruction_mode=GML_RENDER_DEVICE_CHECKPOINT;
      ops[line+1u].target.image.content_version=3u+line;
      ops[line+1u].payload.primitive=(GmlRenderPrimitiveRect){
        .kind=GML_RENDER_PRIMITIVE_PARTICLE_LINE,.region={0,0,96u,48u},
        .colour=0xFF7193B5u,.centre_x=48.25,.centre_y=23.75,
        .scale_x=1.25,.scale_y=0.5,.angle=17,.alpha=0.375,
        .interpolate=line%2u,.alpha_blend=1u};
    }
    if(variant==2u) ops[LINES].payload.primitive.alpha=NAN;
    if(variant==3u) ops[LINES].payload.primitive.kind=9u;
    if(variant==4u) ops[LINES].reconstruction_mode=GML_RENDER_REPLAY_EXACT;
    if(variant==5u) ops[LINES].payload.primitive.region.width=97u;
    if(variant==6u) ops[LINES].payload.primitive.alpha_blend=0u;
    GmlRenderDrawBatch batch={ops,LINES+1u,NULL,0u};
    GmlRenderExecutionResult result=device.submit(device.context,&batch);
    if(variant && variant!=6u){
      REQUIRE(result.status==GML_RENDER_EXEC_UNSUPPORTED && !result.accepted_prefix &&
              !anygm_test_graphics_clear_calls() && !anygm_test_graphics_scene_draw_calls(),
              "particle refusal is transactional before its clear prefix");
    }else{
      anygm_test_graphics_blend_factors(factors);
      REQUIRE(result.status==GML_RENDER_EXEC_ACCEPTED && result.accepted_prefix==LINES+1u,
              "source-free particle batch accepted");
      REQUIRE(anygm_test_graphics_scene_draw_calls()==2 &&
              anygm_test_graphics_scene_vertices()==LINES*6,
              "compatible lines batch to the declared capacity, independent of interpolation");
      REQUIRE(factors[0]==GL_SRC_ALPHA && factors[1]==GL_ONE_MINUS_SRC_ALPHA &&
              factors[2]==GL_ONE && factors[3]==GL_ONE_MINUS_SRC_ALPHA,
              "particle source-over preserves the separate destination alpha rule");
      REQUIRE(!anygm_test_graphics_scene_copies() && !anygm_test_graphics_read_pixels(),
              "particle batch needs no target snapshot or ordinary readback");
    }
    gml_gpu_destroy(gpu,1); free(ops);
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
  return scene_hardware_filtered_batch_case(5);
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
  for(unsigned mode=0;mode<4u;mode++){
    unsigned stamp=mode==1u;
    unsigned modern=mode==3u;
    GmlRenderImageRef source={GML_RENDER_RESOURCE_ATLAS,93u,1u,1u};
    GmlRenderImageRef target={GML_RENDER_RESOURCE_SURFACE,94u,1u,1u};
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
      state->alpha_source_factor=mode>=2u?GML_RENDER_BLEND_SOURCE_ALPHA:GML_RENDER_BLEND_ONE;
      state->rgb_destination_factor=state->alpha_destination_factor=
        GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA;
      state->rgb_equation=state->alpha_equation=GML_RENDER_BLEND_ADD;
      state->samplers[0].filter=stamp?GML_RENDER_FILTER_NEAREST_PHASED_OPAQUE_ALPHA:
                                      GML_RENDER_FILTER_NEAREST_PHASED;
      state->samplers[0].edge_rule=GML_RENDER_EDGE_CLAMP_SUBIMAGE;
      state->samplers[0].source_region=upload.region;
      if(modern) state->samplers[0].quantisation=GML_RENDER_QUANTISE_SPRITE_MODERN_NEAREST;
      op->opcode=GML_RENDER_OP_TEXTURED_QUAD;op->scene_family=GML_RENDER_SCENE_SPRITE;
      op->reconstruction_mode=GML_RENDER_DEVICE_CHECKPOINT;
      op->state_index=mask;op->sources=&source;op->source_count=1u;
      GmlRenderQuad *quad=&op->payload.quad;
      quad->shape=GML_RENDER_QUAD_AXIS_ALIGNED;quad->sampling=state->samplers[0];
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
      unsigned expected=stamp?(mask<8u?2u:mask==8u?8u:0u):1u;
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
    for(unsigned variant=0;variant<(modern?28u:12u);variant++){
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
      op->payload.quad.sampling=state->samplers[0];
      int clears=anygm_test_graphics_clear_calls();
      int draws=anygm_test_graphics_scene_draw_calls();
      copies=anygm_test_graphics_scene_copies();
      result=service.submit(service.context,&invalid_batch);
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

static int scene_alpha_composition_api(GmlGpuApi api,const char *renderer,int font_flush){
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
    REQUIRE(anygm_test_graphics_scene_draw_calls()==draws+(api==GML_GPU_API_OPENGLES3?6:2) &&
            anygm_test_graphics_flush_calls()==flushes+
              (api==GML_GPU_API_OPENGLES3 && font_flush?2u:0u) &&
            anygm_test_graphics_color_mask_all() &&
            !anygm_test_graphics_font_channel_errors(),
            "font writes use the API-qualified pass count and restore the complete colour mask");
    if(api==GML_GPU_API_OPENGLES3)
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
  if(api==GML_GPU_API_OPENGLES3){
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
          anygm_test_graphics_scene_draw_calls()==draws+(api==GML_GPU_API_OPENGLES3?6:2) &&
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
  unsigned passes=api==GML_GPU_API_OPENGLES3?3u:1u;
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
  return scene_alpha_composition_api(GML_GPU_API_OPENGL_CORE,"V3D synthetic",0);
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
          drivers[index].renderer,drivers[index].font_flush)) return 0;
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
    {"unbound_context_access",unbound_context_access_case},
    {"completed_frame_snapshot",completed_frame_snapshot_case},
    {"destroy_without_context",lifecycle_destroy_without_context_case},
    {"missing_entry_point",missing_entry_point_case},
    {"program_failure",program_failure_case},
    {"scene_service_contract",scene_service_contract_case},
    {"scene_optional_entry_refusal",scene_optional_entry_refusal_case},
    {"scene_content_source_orientation",scene_content_source_orientation_case},
    {"scene_resident_batch",scene_resident_batch_case},
    {"scene_particle_line_batch",scene_particle_line_batch_case},
    {"scene_hardware_additive_batch",scene_hardware_additive_batch_case},
    {"scene_hardware_background_batch",scene_hardware_background_batch_case},
    {"scene_hardware_combined_batch",scene_hardware_combined_batch_case},
    {"scene_hardware_surface_batch",scene_hardware_surface_batch_case},
    {"scene_nearest_surface_batch",scene_nearest_surface_batch_case},
    {"scene_nearest_application_batch",scene_nearest_application_batch_case},
    {"scene_destination_surface",scene_destination_surface_case},
    {"scene_readback_formats",scene_readback_formats_case},
    {"incremental_checkpoint_contract",incremental_checkpoint_contract_case},
    {"incremental_checkpoint_budget",incremental_checkpoint_budget_case},
    {"scene_atlas_retire_advanced_version",scene_atlas_retire_advanced_version_case},
    {"scene_alpha_composition",scene_alpha_composition_case},
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
