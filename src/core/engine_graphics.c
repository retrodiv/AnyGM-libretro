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
#include "anygm_host.h"


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

int engine_graphics_active(const AnygmEngine *engine){
  return engine && engine->gpu && gml_gpu_context_active(engine->gpu);
}

void engine_graphics_release(AnygmEngine *engine,int context_is_current){
  if(!engine || !engine->gpu) return;
  /* Report before the counters go away with the object. A host releases the context when content
   * closes, which is before the engine's own teardown. */
  engine_graphics_report(engine);
  gml_gpu_destroy(engine->gpu,context_is_current);
  engine->gpu=NULL;
}

/* One bounded line, only when asked for. A normal build pays no frame scan and no formatting for
 * this; the counters themselves are increments at pass granularity. */
void engine_graphics_report(AnygmEngine *engine){
  GmlGpuCounters counters;
  if(!engine || !engine->gpu) return;
  if(!anygm_host_development_setting(&engine->host,"GML_HYBRID_GPU_STATS")) return;
  gml_gpu_counters(engine->gpu,&counters);
  engine_logf(engine,ANYGM_LOG_INFO,
    "[hybrid-gpu] frames=%u accepted=%u replayed=%u transport=%u draws=%u uploads=%u "
    "bytes=%llu resets=%u destroys=%u losses=%u program_failures=%u "
    "fallback_unsupported=%u fallback_box=%u fallback_context=%u fallback_upload=%u "
    "fallback_overflow=%u materializations=%u screen_passes=%u canvas_passes=%u\n",
    counters.frames_offered,counters.passes_accepted,counters.passes_replayed,
    counters.cpu_upload_frames,counters.draw_calls,counters.full_uploads,
    (unsigned long long)counters.uploaded_bytes,
    counters.context_resets,counters.context_destroys,counters.context_losses,
    counters.program_failures,
    counters.fallbacks[GML_PLAN_FALLBACK_UNSUPPORTED_OPERATION],
    counters.fallbacks[GML_PLAN_FALLBACK_BOX_REDUCTION],
    counters.fallbacks[GML_PLAN_FALLBACK_CONTEXT_UNAVAILABLE],
    counters.fallbacks[GML_PLAN_FALLBACK_RESOURCE_UPLOAD_FAILURE],
    counters.fallbacks[GML_PLAN_FALLBACK_PLAN_OVERFLOW],
    engine->frame_materializations,engine->screen_pass_frames,engine->canvas_pass_frames);
}

/* The frame's last operation, executed where it lands instead of on the processor.
 *
 * The eligible shape is deliberately narrow, and it is a structural fact about the pass rather than
 * anything about the content: the renderer recorded a terminal presentation that covers the whole
 * screen, that screen is the frame the host receives, and nothing wraps it. Everything else keeps
 * the software path, and the record stays available so the canonical pixels can still be produced
 * for anything that needs them. */
int engine_present_hardware_screen(AnygmEngine *engine,unsigned *width,unsigned *height){
  GmlRenderDeferredPresentation record;
  GmlRenderPlan *plan;
  GmlPlanImage source;
  GmlPlanRect whole,destination;
  GmlPlanAxis axis_x,axis_y;
  uint32_t image;
  if(!engine || !engine->gpu || !gml_gpu_context_active(engine->gpu)) return 0;
  if(engine->host_canvas_active) return 0;
  if(!gml_render_deferred_presentation(&engine->render,&record)) return 0;
  if(record.target_pixels!=engine->screen) return 0;
  if((unsigned)record.target_width!=engine->output_width ||
     (unsigned)record.target_height!=engine->output_height) return 0;
  if(record.source_width<=0 || record.source_height<=0 ||
     record.destination_width<=0 || record.destination_height<=0) return 0;
  plan=&engine->host_plan;
  gml_render_plan_reset(plan,GML_PLAN_TARGET_HOST_FRAMEBUFFER,
                        engine->output_width,engine->output_height);
  whole.x=0;
  whole.y=0;
  whole.width=engine->output_width;
  whole.height=engine->output_height;
  /* The graphics target starts undefined, so the plan produces its own ground: the fill the
   * presentation was recorded with when there was one, and otherwise black under a presentation
   * that covers everything anyway. */
  if(!gml_render_plan_add_clear(plan,whole,record.has_fill?record.fill_color:0x000000u)) return 0;
  memset(&source,0,sizeof source);
  source.image_class=GML_PLAN_IMAGE_APPLICATION_SURFACE;
  source.identity=record.identity;
  source.content_generation=record.generation;
  source.pixel_generation=record.generation;
  source.width=(uint32_t)record.source_width;
  source.height=(uint32_t)record.source_height;
  source.pitch_pixels=(uint32_t)record.source_pitch;
  source.pixel_format=GML_PLAN_PIXEL_XRGB8888;
  source.opaque=1u;
  source.cpu_pixels=record.source_pixels;
  image=gml_render_plan_add_image(plan,&source);
  if(image==GML_PLAN_NO_IMAGE) return 0;
  destination.x=record.destination_x;
  destination.y=record.destination_y;
  destination.width=(uint32_t)record.destination_width;
  destination.height=(uint32_t)record.destination_height;
  memset(&axis_x,0,sizeof axis_x);
  memset(&axis_y,0,sizeof axis_y);
  /* The two conventions differ in what a destination index means, so the axis says which one this
   * presentation was recorded under rather than assuming. The pixel-centre rule counts in target
   * coordinates over a fractional rectangle; the leading-edge rule counts in destination-local
   * indices over an integer extent. */
  if(record.sampling_rule==GML_RENDER_PRESENTATION_LEADING_EDGE){
    if(record.extent_x<=0.0 || record.extent_y<=0.0) return 0;
    axis_x.rule=GML_PLAN_AXIS_LEADING_EDGE;
    axis_x.source_extent=(uint32_t)record.source_width;
    axis_x.destination_extent=(uint32_t)record.extent_x;
    axis_x.destination_first=record.local_offset_x;
    axis_y.rule=GML_PLAN_AXIS_LEADING_EDGE;
    axis_y.source_extent=(uint32_t)record.source_height;
    axis_y.destination_extent=(uint32_t)record.extent_y;
    axis_y.destination_first=record.local_offset_y;
  } else {
    axis_x.rule=GML_PLAN_AXIS_PIXEL_CENTRE;
    axis_x.source_extent=(uint32_t)record.source_width;
    axis_x.destination_extent=engine->output_width;
    axis_x.destination_first=record.destination_x;
    axis_x.origin=record.origin_x;
    axis_x.extent=record.extent_x;
    axis_y.rule=GML_PLAN_AXIS_PIXEL_CENTRE;
    axis_y.source_extent=(uint32_t)record.source_height;
    axis_y.destination_extent=engine->output_height;
    axis_y.destination_first=record.destination_y;
    axis_y.origin=record.origin_y;
    axis_y.extent=record.extent_y;
  }
  /* The presentation writes an opaque frame: the top byte is set, not carried. */
  if(!gml_render_plan_add_blit_nearest(plan,image,destination,axis_x,axis_y,0xFFu)) return 0;
  if(!gml_gpu_execute_plan(engine->gpu,plan)) return 0;
  engine->host_plan_valid=1;
  engine->screen_pass_frames++;
  /* The processor's copy of the frame was not written. Saying so is what keeps anything that needs
   * those pixels from reading the buffer instead of asking for them. */
  engine->frame_authority=ENGINE_FRAME_GPU_PRESENTED_CPU_RECONSTRUCTIBLE;
  engine->host_clear_valid=0;
  if(width) *width=engine->output_width;
  if(height) *height=engine->output_height;
  return 1;
}

int engine_present_hardware_canvas(AnygmEngine *engine,unsigned *width,unsigned *height){
  unsigned host_width=0,host_height=0;
  if(!engine || !engine->gpu || !gml_gpu_context_active(engine->gpu)) return 0;
  if(!engine->screen || !engine->output_width || !engine->output_height) return 0;
  /* This pass reads the completed frame, so it has to be the canonical one. */
  engine_materialize_completed_frame(engine);
  engine_host_extent(engine,&host_width,&host_height);
  if(!host_width || !host_height) return 0;
  if(!engine->host_canvas_active){
    /* Nothing wraps the completed frame, so the presentation is the frame itself. That is the
     * transport shape, and it is handled by the caller's fallback rather than duplicated here. */
    return 0;
  }
  /* The graphics target's contents are undefined at the start of a frame, so this plan always
   * produces its own margins; the software path may keep them from the previous frame because its
   * buffer persists. */
  if(!engine_build_host_plan(engine,&engine->host_plan,GML_PLAN_TARGET_HOST_FRAMEBUFFER,
                             host_width,host_height,1)) return 0;
  if(!gml_gpu_execute_plan(engine->gpu,&engine->host_plan)) return 0;
  engine->host_plan_valid=1;
  engine->canvas_pass_frames++;
  /* The host-sized copy was not written this frame, so its margins can no longer be assumed to
   * survive: a later software frame has to clear them again. */
  engine->host_clear_valid=0;
  if(width) *width=host_width;
  if(height) *height=host_height;
  return 1;
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

int engine_present_hardware_canvas(AnygmEngine *engine,unsigned *width,unsigned *height){
  (void)engine;
  (void)width;
  (void)height;
  return 0;
}

int engine_present_hardware_screen(AnygmEngine *engine,unsigned *width,unsigned *height){
  (void)engine;
  (void)width;
  (void)height;
  return 0;
}

void engine_graphics_report(AnygmEngine *engine){ (void)engine; }

int engine_graphics_active(const AnygmEngine *engine){
  (void)engine;
  return 0;
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
