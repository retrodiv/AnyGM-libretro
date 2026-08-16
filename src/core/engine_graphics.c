/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* The optional host graphics target: adopting a context, releasing it, and presenting a frame
 * through it.
 *
 * The whole feature reaches the rest of the engine through this file and one pointer. In a build
 * without the hardware backend the public entry points remain, answer that they are unsupported,
 * and the engine is otherwise unchanged — which is what keeps removing the backend a deletion of
 * bounded modules rather than a reversal of the renderer architecture. */
#include "engine_internal.h"

#if ANYGM_HARDWARE_RENDER

AnygmResult anygm_graphics_context_reset(AnygmEngine *engine,const AnygmGraphicsContext *context){
  GmlGpuContext adopted;
  if(!engine || engine->guard!=ANYGM_ENGINE_GUARD) return ANYGM_ERROR_INVALID_STATE;
  if(!context || context->struct_size<sizeof *context) return ANYGM_ERROR_INCOMPATIBLE_ABI;
  if(!context->get_proc_address || !context->get_current_framebuffer)
    return ANYGM_ERROR_INVALID_ARGUMENT;
  memset(&adopted,0,sizeof adopted);
  switch(context->api){
    case ANYGM_GRAPHICS_OPENGL_CORE: adopted.api=GML_GPU_API_OPENGL_CORE; break;
    case ANYGM_GRAPHICS_OPENGLES3: adopted.api=GML_GPU_API_OPENGLES3; break;
    default: return ANYGM_ERROR_UNSUPPORTED;
  }
  adopted.version_major=context->version_major;
  adopted.version_minor=context->version_minor;
  adopted.userdata=context->userdata;
  /* The two callback shapes are identical across the public and the subsystem boundary, and the
   * cast is what keeps the public header free of a subsystem type. */
  adopted.get_proc_address=(GmlGpuGetProcFn)context->get_proc_address;
  adopted.get_current_framebuffer=(GmlGpuGetFramebufferFn)context->get_current_framebuffer;
  if(!engine->gpu){
    engine->gpu=gml_gpu_create();
    if(!engine->gpu) return ANYGM_ERROR_OUT_OF_MEMORY;
  }
  if(!gml_gpu_context_reset(engine->gpu,&adopted)){
    const char *reason=gml_gpu_last_error(engine->gpu);
    engine_logf(engine,ANYGM_LOG_WARN,"The graphics context could not be adopted: %s\n",
                reason?reason:"unknown");
    /* The software engine is untouched and remains the complete implementation. */
    return ANYGM_ERROR_UNSUPPORTED;
  }
  return ANYGM_OK;
}

void anygm_graphics_context_destroy(AnygmEngine *engine,uint32_t context_is_current){
  if(!engine || engine->guard!=ANYGM_ENGINE_GUARD) return;
  engine_graphics_release(engine,context_is_current?1:0);
}

void engine_graphics_release(AnygmEngine *engine,int context_is_current){
  if(!engine || !engine->gpu) return;
  gml_gpu_destroy(engine->gpu,context_is_current);
  engine->gpu=NULL;
}

int engine_present_hardware_frame(AnygmEngine *engine,const uint32_t *pixels,
                                  unsigned width,unsigned height){
  GmlRenderPlan plan;
  GmlPlanImage frame;
  GmlPlanRect whole;
  uint32_t image;
  if(!engine || !engine->gpu || !gml_gpu_context_active(engine->gpu)) return 0;
  if(!pixels || !width || !height) return 0;
  /* Fallback transport, not acceleration: the software renderer produced this complete frame and
   * the backend puts it on the host's target unchanged. Diagnostics count it separately so that a
   * frame carried this way is never read as an accelerated one. */
  gml_render_plan_reset(&plan,GML_PLAN_TARGET_HOST_FRAMEBUFFER,width,height);
  memset(&frame,0,sizeof frame);
  frame.image_class=GML_PLAN_IMAGE_COMPLETED_FRAME;
  frame.identity=1u;
  frame.content_generation=engine->host_frame_generation;
  frame.pixel_generation=engine->host_frame_generation;
  frame.width=width;
  frame.height=height;
  frame.pitch_pixels=width;
  frame.pixel_format=GML_PLAN_PIXEL_XRGB8888;
  frame.opaque=1u;
  frame.cpu_pixels=pixels;
  image=gml_render_plan_add_image(&plan,&frame);
  if(image==GML_PLAN_NO_IMAGE) return 0;
  whole.x=0;
  whole.y=0;
  whole.width=width;
  whole.height=height;
  if(!gml_render_plan_add_present_cpu_frame(&plan,image,whole)) return 0;
  if(!gml_gpu_execute_plan(engine->gpu,&plan)) return 0;
  return 1;
}

#else

AnygmResult anygm_graphics_context_reset(AnygmEngine *engine,const AnygmGraphicsContext *context){
  if(!engine || engine->guard!=ANYGM_ENGINE_GUARD) return ANYGM_ERROR_INVALID_STATE;
  if(!context || context->struct_size<sizeof *context) return ANYGM_ERROR_INCOMPATIBLE_ABI;
  return ANYGM_ERROR_UNSUPPORTED;
}

void anygm_graphics_context_destroy(AnygmEngine *engine,uint32_t context_is_current){
  (void)engine;
  (void)context_is_current;
}

void engine_graphics_release(AnygmEngine *engine,int context_is_current){
  (void)engine;
  (void)context_is_current;
}

int engine_present_hardware_frame(AnygmEngine *engine,const uint32_t *pixels,
                                  unsigned width,unsigned height){
  (void)engine;
  (void)pixels;
  (void)width;
  (void)height;
  return 0;
}

#endif
