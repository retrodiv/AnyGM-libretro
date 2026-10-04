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
#include "gml_render_shaders.h"


#if ANYGM_HARDWARE_RENDER

static void engine_graphics_lose_scene_context(AnygmEngine *engine){
  gml_render_lose_volatile_device(&engine->render);
  engine->host_plan_valid=0u;
  engine->host_clear_valid=0;
  if(engine->frame_authority!=ENGINE_FRAME_CPU_MATERIALIZED &&
     engine->frame_authority!=ENGINE_FRAME_NONE){
    engine->frame_authority=ENGINE_FRAME_NONE;
    engine->have_presented_frame=0;
    engine->state_frame_available=0;
    engine->state_just_loaded=0;
  }
}

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
  adopted.is_current=(GmlGpuIsCurrentFn)context->is_current;
  if(!engine->gpu){
    engine->gpu=gml_gpu_create();
    if(!engine->gpu) return ANYGM_ERROR_OUT_OF_MEMORY;
  }
  /* Context reset replaces the old GL namespace before this callback; its pending-only images
   * cannot be read through the new context and must not masquerade as canonical CPU pixels. */
  if(engine_graphics_active(engine) &&
     engine->scene_renderer_session==ANYGM_SCENE_RENDERER_OPENGL)
    engine_graphics_lose_scene_context(engine);
  else gml_render_abandon_device(&engine->render);
  if(!gml_gpu_context_reset(engine->gpu,&adopted)){
    const char *reason=gml_gpu_last_error(engine->gpu);
    engine_logf(engine,ANYGM_LOG_WARN,"The graphics context could not be adopted: %s\n",
                reason?reason:"unknown");
    /* The software engine is untouched and remains the complete implementation. */
    return ANYGM_ERROR_UNSUPPORTED;
  }
  engine_graphics_bind_renderer(engine);
  return ANYGM_OK;
}

void engine_graphics_bind_renderer(AnygmEngine *engine){
  GmlRenderDeviceServices services;
  if(!engine || !engine_graphics_active(engine)) return;
  gml_gpu_device_services(engine->gpu,&services);
  gml_render_bind_device(&engine->render,&services);
  /* Hybrid presentation and content GLSL share this context but are independent policies. */
  gml_render_set_shader_device(&engine->render,engine_content_shaders_active(engine));
}

void anygm_graphics_context_destroy(AnygmEngine *engine,uint32_t context_is_current){
  if(!engine || engine->guard!=ANYGM_ENGINE_GUARD) return;
  engine_graphics_release(engine,context_is_current?1:0);
}

int engine_graphics_active(const AnygmEngine *engine){
  return engine && engine->gpu && gml_gpu_context_active(engine->gpu);
}

int engine_graphics_is_current(const AnygmEngine *engine){
  return engine && gml_gpu_context_is_current(engine->gpu);
}

int engine_graphics_has_access_proof(const AnygmEngine *engine){
  return engine && gml_gpu_context_has_access_proof(engine->gpu);
}

int engine_hybrid_presentation_active(const AnygmEngine *engine){
  return engine_graphics_active(engine) && engine->config.hybrid_gpu_presentation?1:0;
}

int engine_content_shaders_active(const AnygmEngine *engine){
  return engine_graphics_active(engine) && engine->config.content_shader_device_expected &&
         engine->config.content_shader_readback!=ANYGM_SHADER_READBACK_NEVER?1:0;
}

void engine_graphics_release(AnygmEngine *engine,int context_is_current){
  int preserved=1;
  if(!engine || !engine->gpu) return;
  if(context_is_current && engine_graphics_has_access_proof(engine) &&
     !engine_graphics_is_current(engine)) context_is_current=0;
  if(context_is_current && engine->scene_renderer_session==ANYGM_SCENE_RENDERER_OPENGL &&
     engine_graphics_has_access_proof(engine)){
    preserved=0;
    /* The destruction notification cannot defer host teardown. Retry one transient failure,
     * but never replace an unread GPU image by its approximate CPU recipe. */
    for(unsigned attempt=0;attempt<2u;attempt++){
      if(gml_render_observe_device_resources(&engine->render) &&
         engine_materialize_completed_frame(engine)){
        preserved=1;
        break;
      }
    }
    if(!preserved) engine_logf(engine,ANYGM_LOG_ERROR,
      "GPU pixels could not be preserved before controlled graphics destruction; "
      "unobserved volatile pixels are lost\n");
  }
  if(context_is_current && preserved) gml_render_bind_device(&engine->render,NULL);
  else if(engine->scene_renderer_session==ANYGM_SCENE_RENDERER_OPENGL)
    engine_graphics_lose_scene_context(engine);
  else gml_render_abandon_device(&engine->render);
  gml_render_set_shader_device(&engine->render,0);
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
  GmlRenderResourceMetrics resources={0};
  if(!engine) return;
  if(!anygm_host_development_setting(&engine->host,"GML_GRAPHICS_DEVICE_STATS")) return;
  if(!engine->gpu){
    if(engine->scene_graphics_reported) return;
    engine_logf(engine,ANYGM_LOG_INFO,
      "[graphics-scene] requested=%s active=CPU reason=%s offered=0 accepted=0\n",
      engine->scene_renderer_session==ANYGM_SCENE_RENDERER_OPENGL?"OpenGL":"CPU",
      engine->scene_renderer_session==ANYGM_SCENE_RENDERER_OPENGL?
        "context-unavailable":"not-requested");
    engine->scene_graphics_reported=1u;
    return;
  }
  gml_gpu_counters(engine->gpu,&counters);
  gml_render_resource_metrics(&engine->render,&resources);
  engine->scene_graphics_reported=1u;
  engine_logf(engine,ANYGM_LOG_INFO,
    "[graphics-device] frames=%u accepted=%u replayed=%u transport=%u draws=%u uploads=%u "
    "bytes=%llu resets=%u destroys=%u losses=%u program_failures=%u "
    "fallback_unsupported=%u fallback_box=%u fallback_context=%u fallback_upload=%u "
    "fallback_overflow=%u fallback_shader=%u materializations=%u screen_passes=%u canvas_passes=%u"
    " readback_passes=%u readback_ms_per_frame=%.3f readback_pipelined=%d readback_refused=%d"
    " last_error=\"%s\"\n",
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
    counters.fallbacks[GML_PLAN_FALLBACK_SHADER_FAILURE],
    engine->frame_materializations,engine->screen_pass_frames,engine->canvas_pass_frames,
    engine->readback_pass_count,
    engine->readback_frames_measured
      ?(double)engine->readback_frame_ns/(double)engine->readback_frames_measured/1e6:0.0,
    engine->readback_is_pipelined,engine->readback_refused,
    gml_gpu_last_error(engine->gpu)?gml_gpu_last_error(engine->gpu):"");
  engine_logf(engine,ANYGM_LOG_INFO,
    "[graphics-scene] requested=%s active=%s reason=%s offered=%llu accepted=%llu "
    "refused=%llu partial_prefix=%llu "
    "texture=%llu primitive=%llu surface=%llu effect=%llu presentation=%llu "
    "shader=%llu software3d=%llu uploads=%llu upload_bytes=%llu "
    "readbacks=%llu readback_bytes=%llu rgb_queries=%llu rgb_query_bytes=%llu "
    "checkpoint_masks=%llu checkpoint_mask_bytes=%llu "
    "rgb_query_refusals=%llu screen_cpu_reads=%llu screen_cpu_materializations=%llu\n",
    engine->scene_renderer_session==ANYGM_SCENE_RENDERER_OPENGL?"OpenGL":"CPU",
    counters.scene_batches_accepted?"OpenGL":"CPU",
    counters.scene_batches_accepted?"accepted":
      counters.scene_batches_offered?"refused-or-unsupported":"no-eligible-scene",
    (unsigned long long)counters.scene_batches_offered,
    (unsigned long long)counters.scene_batches_accepted,
    (unsigned long long)counters.scene_batches_refused,
    (unsigned long long)counters.scene_partial_prefix,
    (unsigned long long)(counters.scene_operations[GML_RENDER_OP_TEXTURED_QUAD]+
                         counters.scene_operations[GML_RENDER_OP_TEXTURED_TRIANGLES]),
    (unsigned long long)(counters.scene_operations[GML_RENDER_OP_COLOURED_PRIMITIVE]+
                         counters.scene_operations[GML_RENDER_OP_CLEAR]),
    (unsigned long long)(counters.scene_operations[GML_RENDER_OP_COPY_IMAGE]+
                         counters.scene_operations[GML_RENDER_OP_COMPOSE_SURFACE]),
    (unsigned long long)counters.scene_operations[GML_RENDER_OP_LAYER_EFFECT],
    (unsigned long long)counters.scene_operations[GML_RENDER_OP_PRESENT_IMAGE],
    (unsigned long long)counters.scene_operations[GML_RENDER_OP_CONTENT_SHADER_DRAW],
    (unsigned long long)counters.scene_operations[GML_RENDER_OP_SOFTWARE3D_PASS],
    (unsigned long long)counters.scene_uploads,
    (unsigned long long)counters.scene_uploaded_bytes,
    (unsigned long long)counters.scene_readbacks,
    (unsigned long long)counters.scene_readback_bytes,
    (unsigned long long)counters.scene_rgb_queries,
    (unsigned long long)(counters.scene_rgb_queries*4u),
    (unsigned long long)counters.scene_readback_masks,
    (unsigned long long)counters.scene_readback_mask_bytes,
    (unsigned long long)counters.scene_rgb_query_refusals,
    (unsigned long long)resources.completed_screen_cpu_reads,
    (unsigned long long)resources.completed_screen_cpu_materializations);
  engine_logf(engine,ANYGM_LOG_INFO,
    "[graphics-completed-state] copies=%llu copy_bytes=%llu reads=%llu read_bytes=%llu\n",
    (unsigned long long)counters.completed_frame_copies,
    (unsigned long long)counters.completed_frame_copy_bytes,
    (unsigned long long)counters.completed_frame_reads,
    (unsigned long long)counters.completed_frame_read_bytes);
  engine_logf(engine,ANYGM_LOG_INFO,
    "[graphics-scene-families] sprite=%llu background=%llu tile=%llu surface=%llu text=%llu primitive=%llu effect=%llu unclassified=%llu\n",
    (unsigned long long)counters.scene_families[GML_RENDER_SCENE_SPRITE],
    (unsigned long long)counters.scene_families[GML_RENDER_SCENE_BACKGROUND],
    (unsigned long long)counters.scene_families[GML_RENDER_SCENE_TILE],
    (unsigned long long)counters.scene_families[GML_RENDER_SCENE_SURFACE],
    (unsigned long long)counters.scene_families[GML_RENDER_SCENE_TEXT],
    (unsigned long long)counters.scene_families[GML_RENDER_SCENE_PRIMITIVE],
    (unsigned long long)counters.scene_families[GML_RENDER_SCENE_EFFECT],
    (unsigned long long)counters.scene_families[GML_RENDER_SCENE_UNCLASSIFIED]);
}

/* The frame's last operation, executed where it lands instead of on the processor.
 *
 * The eligible shape is deliberately narrow, and it is a structural fact about the pass rather than
 * anything about the content: the renderer recorded a terminal presentation that covers the whole
 * screen, that screen is the frame the host receives, and nothing wraps it. Everything else keeps
 * the software path, and the record stays available so the canonical pixels can still be produced
 * for anything that needs them. */
/* A content program's presentation: the frame drawn through the content's own shader, with the
 * values and pictures the content bound. Everything comes from the renderer by name; nothing here
 * knows what the program computes. Returns the plan-building result; a refusal is the caller's
 * unshaded blit. */
static int engine_plan_content_program_part(AnygmEngine *engine,GmlRenderPlan *plan,uint32_t image,
                                            GmlPlanRect destination,GmlPlanRect source_rect,
                                            int shader_id,uint32_t generation,int linear,
                                            const float colour[4]){
  GmlPlanShader shader;
  GmlRenderShaderSampler samplers[GML_PLAN_MAX_SAMPLERS];
  uint32_t count;
  if(!gml_render_shader_plan_snapshot(&engine->render,shader_id,generation,colour,&shader))
    return 0;
  count=gml_render_shader_samplers(&engine->render,shader_id,samplers,GML_PLAN_MAX_SAMPLERS);
  for(uint32_t index=0;index<count;index++){
    GmlPlanSampler *sampler=&shader.samplers[index];
    GmlPlanImage picture;
    int w=0,h=0;
    const uint32_t *pixels=NULL;
    memset(&picture,0,sizeof picture);
    if(samplers[index].surface>=0){
      if(!gml_render_surface_plane(&engine->render,samplers[index].surface,&w,&h,&pixels)) return 0;
      picture.image_class=GML_PLAN_IMAGE_SURFACE;
      picture.identity=(uint32_t)samplers[index].surface;
    } else if(samplers[index].sprite>=0){
      if(!gml_render_sprite_frame_plane(&engine->render,samplers[index].sprite,
                                        samplers[index].frame,&w,&h,&pixels)) return 0;
      picture.image_class=GML_PLAN_IMAGE_SPRITE;
      picture.identity=((uint32_t)samplers[index].sprite<<10)|((uint32_t)samplers[index].frame&0x3FFu);
    } else continue;
    /* A sampler picture is re-uploaded every frame: neither a surface nor a scratch plane carries
     * a generation the backend could trust across frames. */
    picture.content_generation=generation;
    picture.pixel_generation=generation;
    picture.width=(uint32_t)w;
    picture.height=(uint32_t)h;
    picture.pitch_pixels=(uint32_t)w;
    picture.pixel_format=GML_PLAN_PIXEL_XRGB8888;
    picture.opaque=0u;
    picture.cpu_pixels=pixels;
    sampler->image=gml_render_plan_add_image(plan,&picture);
    if(sampler->image==GML_PLAN_NO_IMAGE) return 0;
  }
  return gml_render_plan_add_shader_draw_part(plan,image,destination,source_rect,&shader,
                                             linear?1u:0u);
}

static int engine_plan_content_program(AnygmEngine *engine,GmlRenderPlan *plan,uint32_t image,
                                       GmlPlanRect destination,int shader_id,uint32_t generation,
                                       int linear){
  static const float white[4]={1.0f,1.0f,1.0f,1.0f};
  GmlPlanRect whole={0,0,0,0};
  return engine_plan_content_program_part(engine,plan,image,destination,whole,shader_id,generation,
                                          linear,white);
}

/* The renderer's request to run a content program in the middle of a frame: the source is
 * uploaded as it is now, the program runs over it into an off-screen target of the requested
 * size, and the result is read back into the renderer's plane. */
/* A read-back stalls the device until everything queued before it has finished, and what that
 * costs is the device's, not the picture's: a unified-memory part answers in a fraction of a
 * millisecond, a discrete laptop part behind a mux in several. Measured on the latter, three such
 * draws a frame cost four times the software renderer's whole frame. So the first passes are timed,
 * and a device that cannot afford them keeps the terminal presentation on the device and draws
 * the mid-frame ones plain for the rest of the session, saying so once. */
int engine_readback_over_budget(uint64_t frame_total_ns,uint32_t frames){
  if(frames<ENGINE_READBACK_CALIBRATION_FRAMES) return 0;
  return frame_total_ns/frames>(uint64_t)ENGINE_READBACK_BUDGET_US_PER_FRAME*1000u;
}

void engine_readback_open_frame(AnygmEngine *engine){
  if(!engine) return;
  if(engine->readback_frame_spent){
    engine->readback_frames_measured++;
    engine->readback_frame_spent=0;
  }
}

int engine_execute_content_shader(void *context,const GmlRenderShaderRequest *request){
  AnygmEngine *engine=(AnygmEngine*)context;
  GmlRenderPlan plan;
  GmlPlanImage source;
  GmlPlanRect whole;
  uint32_t image;
  uint64_t started;
  if(!engine || !request || !engine_content_shaders_active(engine)) return 0;
  if(engine->readback_refused) return 0;
  /* Start pipelined read-back with the first pass. Waiting for the device adds latency;
   * the prior pass's answer is one frame old, while Always requests the exact answer. */
  if(!engine->readback_is_pipelined &&
     engine->config.content_shader_readback!=ANYGM_SHADER_READBACK_ALWAYS){
    engine->readback_is_pipelined=1;
    gml_gpu_set_readback_pipelined(engine->gpu,1);
  }
  if(request->width<=0 || request->height<=0 || !request->output || !request->source) return 0;
  if(request->source_width<=0 || request->source_height<=0 || request->source_pitch<request->source_width) return 0;
  {
    /* A fragment that reads its place on the render target must be evaluated there, so the target
     * is the render target's own size and the draw lands at its own offset inside it; only the
     * rectangle is read back. A request that names no target keeps the destination-sized one. */
    uint32_t tw=request->target_width>0?(uint32_t)request->target_width:(uint32_t)request->width;
    uint32_t th=request->target_height>0?(uint32_t)request->target_height:(uint32_t)request->height;
    int dx=request->target_width>0?request->dest_x:0;
    int dy=request->target_height>0?request->dest_y:0;
    if(dx<0 || dy<0 || (uint32_t)dx+(uint32_t)request->width>tw ||
       (uint32_t)dy+(uint32_t)request->height>th){ tw=(uint32_t)request->width; th=(uint32_t)request->height; dx=dy=0; }
    gml_render_plan_reset(&plan,GML_PLAN_TARGET_READBACK,tw,th);
    plan.readback_pixels=request->output;
    plan.readback_pitch_pixels=(uint32_t)request->width;
    plan.readback_rect.x=dx;
    plan.readback_rect.y=dy;
    plan.readback_rect.width=(uint32_t)request->width;
    plan.readback_rect.height=(uint32_t)request->height;
    whole.x=dx;
    whole.y=dy;
    whole.width=(uint32_t)request->width;
    whole.height=(uint32_t)request->height;
  }
  if(!gml_render_plan_add_clear(&plan,whole,0u)) return 0;
  memset(&source,0,sizeof source);
  source.image_class=GML_PLAN_IMAGE_SURFACE;
  source.identity=request->source_identity;
  /* A surface's pixels change between requests with the same identity, so every request carries
   * a generation of its own and the backend uploads it afresh. */
  source.content_generation=request->serial;
  source.pixel_generation=request->serial;
  source.width=(uint32_t)request->source_width;
  source.height=(uint32_t)request->source_height;
  source.pitch_pixels=(uint32_t)request->source_pitch;
  source.pixel_format=GML_PLAN_PIXEL_XRGB8888;
  source.opaque=0u;
  source.cpu_pixels=request->source;
  image=gml_render_plan_add_image(&plan,&source);
  if(image==GML_PLAN_NO_IMAGE) return 0;
  {
    GmlPlanRect region={0,0,0,0};
    if(request->region_width>0 && request->region_height>0){
      region.x=request->region_x;
      region.y=request->region_y;
      region.width=(uint32_t)request->region_width;
      region.height=(uint32_t)request->region_height;
    }
    static const float white[4]={1.0f,1.0f,1.0f,1.0f};
    const float *colour=(request->vertex_colour[3]>0.0f||request->vertex_colour[0]>0.0f)
                        ?request->vertex_colour:white;
    if(!engine_plan_content_program_part(engine,&plan,image,whole,region,request->shader,
                                         request->serial,request->linear,colour)) return 0;
  }
  started=anygm_host_monotonic_time_ns(&engine->host);
  if(!gml_gpu_execute_plan(engine->gpu,&plan)){
    if(plan.fallback_reason==GML_PLAN_FALLBACK_SHADER_FAILURE)
      gml_render_shader_mark_failed(&engine->render,request->shader);
    return 0;
  }
  engine->readback_pass_count++;
  {
    uint64_t spent=anygm_host_monotonic_time_ns(&engine->host)-started;
    engine->readback_pass_ns+=spent;
    engine->readback_pass_measured++;
    /* Accumulate by frame: the passes of one frame are what that frame pays. */
    engine->readback_frame_spent=1;
    engine->readback_frame_ns+=spent;
    if(engine->config.content_shader_readback==ANYGM_SHADER_READBACK_BUDGETED &&
       engine_readback_over_budget(engine->readback_frame_ns,engine->readback_frames_measured)){
      engine->readback_refused=1;
      engine_logf(engine,ANYGM_LOG_WARN,
        "[content-glsl] shaders inside a frame draw plain on this device: even without "
        "waiting for it they cost %.2f ms a frame, over the %.2f ms budget. Set the "
        "Game shaders (GLSL) option to Exact to run them anyway.\n",
        (double)engine->readback_frame_ns/(double)engine->readback_frames_measured/1e6,
        ENGINE_READBACK_BUDGET_US_PER_FRAME/1000.0);
    }
  }
  return 1;
}

int engine_present_hardware_screen(AnygmEngine *engine,unsigned *width,unsigned *height){
  GmlRenderDeferredPresentation record;
  GmlRenderPlan *plan;
  GmlPlanImage source;
  GmlPlanRect whole,destination;
  GmlPlanAxis axis_x,axis_y;
  uint32_t image;
  if(!engine) return 0;
  int hybrid=engine_hybrid_presentation_active(engine);
  int shaders=engine_content_shaders_active(engine);
  int scene=engine->scene_renderer_session==ANYGM_SCENE_RENDERER_OPENGL &&
            engine_graphics_active(engine);
  if(!hybrid && !shaders && !scene) return 0;
  int crt_readback=engine->host_crt_active && engine->host_canvas_active;
  if(engine->host_canvas_active && !crt_readback) return 0;
  if(!gml_render_deferred_presentation(&engine->render,&record)){
    GmlRenderImageRef completed={0};
    /* R03_COMPLETED_SCREEN_PRESENT: the scene already owns the final raster. The
     * application surface is not its substitute, and CPU pixels are not an upload source. */
    if(!scene || engine->host_canvas_active ||
       engine->present_shift_x || engine->present_shift_y ||
       engine->present_crop_left || engine->present_crop_top ||
       engine->present_crop_right || engine->present_crop_bottom ||
       !gml_render_completed_screen_image(&engine->render,engine->screen,
                                          (int)engine->output_width,
                                          (int)engine->output_height,&completed)) return 0;
    memset(&record,0,sizeof record);
    record.target_pixels=engine->screen;
    record.target_width=record.source_width=record.source_pitch=record.destination_width=
      (int)engine->output_width;
    record.target_height=record.source_height=record.destination_height=(int)engine->output_height;
    record.identity=completed.identity;
    record.generation=engine->host_frame_generation;
    record.resident_image=completed;
    record.shader=-1;
    record.sampling_rule=GML_RENDER_PRESENTATION_LEADING_EDGE;
    record.extent_x=engine->output_width;
    record.extent_y=engine->output_height;
  }
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
  source.image_class=record.resident_image.resource_class==GML_RENDER_RESOURCE_SCREEN
    ?GML_PLAN_IMAGE_COMPLETED_FRAME:GML_PLAN_IMAGE_APPLICATION_SURFACE;
  source.identity=record.identity;
  source.content_generation=record.generation;
  source.pixel_generation=record.generation;
  source.width=(uint32_t)record.source_width;
  source.height=(uint32_t)record.source_height;
  source.pitch_pixels=(uint32_t)record.source_pitch;
  source.pixel_format=GML_PLAN_PIXEL_XRGB8888;
  source.opaque=1u;
  source.cpu_pixels=record.resident_image.resource_class?NULL:record.source_pixels;
  source.resident_image=record.resident_image;
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
  if(record.shader>=0 && shaders){
    /* The content drew its frame through its own program. Run that program on the device; when
     * the device refuses it, the renderer is told so the next frame draws unshaded rather than
     * asking again, and this frame falls back to the unshaded blit below. */
    if(engine_plan_content_program(engine,plan,image,destination,record.shader,record.generation,
                                   record.linear)){
      if(crt_readback){
        /* Preserve the content's terminal program at its authored extent before the CPU CRT
         * filter. A terminal frame must be current even when mid-frame readbacks are pipelined. */
        plan->target=GML_PLAN_TARGET_READBACK;
        plan->readback_pixels=engine->screen;
        plan->readback_pitch_pixels=engine->output_width;
        gml_gpu_set_readback_pipelined(engine->gpu,0);
        int executed=gml_gpu_execute_plan(engine->gpu,plan);
        gml_gpu_set_readback_pipelined(engine->gpu,engine->readback_is_pipelined);
        if(executed){
          gml_render_discard_deferred_presentation(&engine->render);
          engine->frame_authority=ENGINE_FRAME_CPU_MATERIALIZED;
          engine->frame_materializations++;
          engine->host_clear_valid=0;
          return 0; /* The caller still has to fit and present these pixels. */
        }
      } else if(gml_gpu_execute_plan(engine->gpu,plan)) goto presented;
      if(plan->fallback_reason==GML_PLAN_FALLBACK_SHADER_FAILURE)
        gml_render_shader_mark_failed(&engine->render,record.shader);
    }
    gml_render_plan_reset(plan,GML_PLAN_TARGET_HOST_FRAMEBUFFER,
                          engine->output_width,engine->output_height);
    if(!gml_render_plan_add_clear(plan,whole,record.has_fill?record.fill_color:0x000000u)) return 0;
    image=gml_render_plan_add_image(plan,&source);
    if(image==GML_PLAN_NO_IMAGE) return 0;
  }
  if(crt_readback) return 0;
  /* A resident scene may present its final image without selecting unrelated hybrid passes.
   * With only content GLSL enabled, an ineligible program still returns to software. */
  if(!hybrid && !scene) return 0;
  /* The presentation writes an opaque frame: the top byte is set, not carried. */
  if(!gml_render_plan_add_blit_nearest(plan,image,destination,axis_x,axis_y,0xFFu)) return 0;
  if(!gml_gpu_execute_plan(engine->gpu,plan)) return 0;
presented:
  if(scene && engine_graphics_is_current(engine)){
    if(!gml_gpu_capture_completed_frame(engine->gpu,engine->output_width,engine->output_height,
                                        engine->host_frame_generation)) return 0;
    engine->frame_authority=ENGINE_FRAME_GPU_PRESENTED_DEVICE_RESIDENT;
  } else engine->frame_authority=ENGINE_FRAME_GPU_PRESENTED_CPU_RECONSTRUCTIBLE;
  engine->host_plan_valid=1;
  engine->screen_pass_frames++;
  /* The processor's copy of the frame was not written. Saying so is what keeps anything that needs
   * those pixels from reading the buffer instead of asking for them. */
  engine->host_clear_valid=0;
  if(width) *width=engine->output_width;
  if(height) *height=engine->output_height;
  return 1;
}

int engine_present_hardware_canvas(AnygmEngine *engine,unsigned *width,unsigned *height){
  unsigned host_width=0,host_height=0;
  if(!engine_hybrid_presentation_active(engine)) return 0;
  if(!engine->screen || !engine->output_width || !engine->output_height) return 0;
  /* This pass reads the completed frame, so it has to be the canonical one. */
  if(!engine_materialize_completed_frame(engine)) return 0;
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

void engine_graphics_bind_renderer(AnygmEngine *engine){ (void)engine; }

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

int engine_execute_content_shader(void *context,const GmlRenderShaderRequest *request){
  (void)context;
  (void)request;
  return 0;
}

void engine_readback_open_frame(AnygmEngine *engine){ (void)engine; }

int engine_readback_over_budget(uint64_t frame_total_ns,uint32_t frames){
  if(frames<ENGINE_READBACK_CALIBRATION_FRAMES) return 0;
  return frame_total_ns/frames>(uint64_t)ENGINE_READBACK_BUDGET_US_PER_FRAME*1000u;
}

void engine_graphics_report(AnygmEngine *engine){ (void)engine; }

int engine_graphics_active(const AnygmEngine *engine){
  (void)engine;
  return 0;
}

int engine_graphics_is_current(const AnygmEngine *engine){
  (void)engine;
  return 0;
}

int engine_graphics_has_access_proof(const AnygmEngine *engine){
  (void)engine;
  return 0;
}

int engine_hybrid_presentation_active(const AnygmEngine *engine){
  (void)engine;
  return 0;
}

int engine_content_shaders_active(const AnygmEngine *engine){
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

int engine_graphics_finish_scene(AnygmEngine *engine){
  if(!engine) return 0;
  /* A witnessed scene retains actual persistent pixels until an explicit observer. CPU and
   * hosts without a save-access witness retain the established context-free recovery path. */
  if(engine->scene_renderer_session==ANYGM_SCENE_RENDERER_OPENGL &&
     engine_graphics_is_current(engine))
    return gml_render_scene_submit_pending(&engine->render);
  return gml_render_scene_flush(&engine->render);
}
