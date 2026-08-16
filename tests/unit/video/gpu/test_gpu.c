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
#include "gml_gpu_gl_api.h"
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
  };
  static const AnygmTestCase execution_cases[]={
    {"state_and_geometry",execute_state_and_geometry_case},
    {"upload_and_reuse",upload_and_reuse_case},
    {"reduction_falls_back",reduction_falls_back_case},
    {"no_context_falls_back",no_context_falls_back_case},
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
