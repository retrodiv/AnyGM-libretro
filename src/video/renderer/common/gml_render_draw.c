/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Neutral device binding. Operation preparation and barriers grow here as scene families land. */
#include "gml_render_internal.h"
#include "gml_render_surfaces_internal.h"
#include "gml_render_plan.h"

#include <math.h>
#include <string.h>

static void dispatch_add(GmlRenderDispatchMetrics *metrics,uint64_t *value,uint64_t count);

static void dispatch_frame_add(GmlRenderDispatchFrames *frames,uint64_t *value,uint64_t count){
  if(count>UINT64_MAX-*value){ *value=UINT64_MAX; frames->overflow=1; }
  else *value+=count;
}

static void dispatch_frame_unknown(GmlRenderDispatchFrames *frames){
  if(!frames || !frames->enabled || frames->unknown_seen) return;
  dispatch_frame_add(frames,&frames->unknown_frames,1u);
  frames->unknown_seen=1;
}

void gml_render_dispatch_frame_bind(GmlRender *render,GmlRenderDispatchFrames *frames){
  if(render) render->dispatch_frame_accounting=frames;
}

void gml_render_dispatch_frame_open(GmlRenderDispatchFrames *frames){
  if(!frames || !frames->enabled) return;
  if(frames->active){
    dispatch_frame_unknown(frames);
    dispatch_frame_add(frames,&frames->failed_frames,1u);
  }
  frames->active=1;
  frames->cpu_seen=frames->no_scene_seen=frames->unknown_seen=0;
  frames->pending_cause=GML_RENDER_RESIDENT_FRAME_START;
  frames->loss_cause=GML_RENDER_RESIDENT_UNKNOWN;
  dispatch_frame_add(frames,&frames->frames,1u);
}

void gml_render_dispatch_frame_close(GmlRenderDispatchFrames *frames,int succeeded){
  if(!frames || !frames->enabled) return;
  if(!frames->active){
    /* A missing boundary is not evidence of a zero-fallback frame. */
    frames->unknown_seen=0;
    dispatch_frame_unknown(frames);
    return;
  }
  if(!succeeded) dispatch_frame_add(frames,&frames->failed_frames,1u);
  frames->active=0;
}

void gml_render_dispatch_resident_skip(GmlRender *render,unsigned cause){
  GmlRenderDispatchFrames *frames=render?render->dispatch_frame_accounting:NULL;
  if(!frames || !frames->enabled || !frames->active) return;
  frames->pending_cause=cause<GML_RENDER_RESIDENT_CAUSE_COUNT?cause:GML_RENDER_RESIDENT_UNKNOWN;
}

int gml_render_dispatch_resident_reject(GmlRender *render,unsigned cause){
  gml_render_dispatch_resident_skip(render,cause);
  return 0;
}

void gml_render_dispatch_resident_admitted(GmlRender *render){
  GmlRenderDispatchFrames *frames=render?render->dispatch_frame_accounting:NULL;
  if(!frames || !frames->enabled || !frames->active) return;
  /* An unsuccessful alternative is not a CPU frame when the selected pass succeeds. */
  frames->pending_cause=frames->loss_cause=GML_RENDER_RESIDENT_UNKNOWN;
}

void gml_render_dispatch_resident_lost(GmlRender *render,unsigned cause){
  GmlRenderDispatchFrames *frames=render?render->dispatch_frame_accounting:NULL;
  if(!frames || !frames->enabled || !frames->active || frames->loss_cause) return;
  frames->loss_cause=cause<GML_RENDER_RESIDENT_CAUSE_COUNT?cause:GML_RENDER_RESIDENT_UNKNOWN;
}

static void dispatch_frame_cpu(GmlRender *render,unsigned family,unsigned reason,uint64_t count){
  GmlRenderDispatchFrames *frames=render->dispatch_frame_accounting;
  if(!frames || !frames->enabled) return;
  if(!frames->active){
    dispatch_frame_add(frames,&frames->outside_cpu,count);
    return;
  }
  if(frames->cpu_seen && (reason!=GML_RENDER_DISPATCH_NO_SCENE || frames->no_scene_seen)) return;
  unsigned cause=frames->loss_cause?frames->loss_cause:frames->pending_cause;
  if(frames->pending_cause!=GML_RENDER_RESIDENT_UNKNOWN &&
     frames->pending_cause!=GML_RENDER_RESIDENT_FRAME_START)
    cause=frames->pending_cause;
  if(reason==GML_RENDER_DISPATCH_NO_DEVICE) cause=GML_RENDER_RESIDENT_NO_DEVICE;
  if(reason==GML_RENDER_DISPATCH_COMPOSITION) cause=GML_RENDER_RESIDENT_COMPOSITION;
  /* A direct executor is known even without a pass-loss annotation. Do not publish
   * this local attribution as a loss: a later no-scene draw needs its own cause. */
  if(cause==GML_RENDER_RESIDENT_UNKNOWN && reason==GML_RENDER_DISPATCH_UNQUALIFIED)
    cause=GML_RENDER_RESIDENT_EXEC_CPU;
  if(cause>=GML_RENDER_RESIDENT_CAUSE_COUNT) cause=GML_RENDER_RESIDENT_UNKNOWN;
  if(!cause) dispatch_frame_unknown(frames);
  if(!frames->cpu_seen){
    frames->cpu_seen=1;
    dispatch_frame_add(frames,&frames->cpu_frames,1u);
    dispatch_frame_add(frames,&frames->cpu_causes[cause],1u);
    dispatch_frame_add(frames,&frames->first_cpu_family[family],1u);
  }
  if(reason==GML_RENDER_DISPATCH_NO_SCENE && !frames->no_scene_seen){
    frames->no_scene_seen=1;
    dispatch_frame_add(frames,&frames->no_scene_frames,1u);
    dispatch_frame_add(frames,&frames->no_scene_causes[cause],1u);
  }
}

void gml_render_set_skip_cpu_draws(GmlRender *render,int enabled){
  if(render) render->skip_cpu_draws=enabled?1:0;
}
int gml_render_cpu_draws_disabled(const GmlRender *render){
  return render && render->skip_cpu_draws;
}
int gml_render_cpu_draw_barrier(GmlRender *render){
  if(gml_render_cpu_draws_disabled(render)){
    if(render->dispatch_metrics.enabled)
      dispatch_add(&render->dispatch_metrics,&render->dispatch_metrics.skipped,1u);
    return 0;
  }
  return gml_render_resident_application_cpu_barrier(render);
}

void gml_render_dispatch_diagnostics_enable(GmlRender *render,int enabled){
  if(!render) return;
  /* A subsequent re-enable cannot hide work performed during an accounting gap. */
  if(render->dispatch_metrics.enabled && !enabled){
    dispatch_add(&render->dispatch_metrics,&render->dispatch_metrics.unclassified,1u);
    dispatch_frame_unknown(render->dispatch_frame_accounting);
  }
  render->dispatch_metrics.enabled=enabled?1:0;
}

int gml_render_dispatch_metrics(const GmlRender *render,GmlRenderDispatchMetrics *out){
  if(!out) return 0;
  if(!render){memset(out,0,sizeof *out);return 0;}
  *out=render->dispatch_metrics;
  /* A standalone renderer snapshot cannot certify accounting for a whole load.
   * The engine owns that guarantee across renderer replacement and teardown. */
  out->complete=0;
  return 1;
}

static void dispatch_add(GmlRenderDispatchMetrics *metrics,uint64_t *value,uint64_t count){
  if(count>UINT64_MAX-*value){
    *value=UINT64_MAX;
    metrics->overflow=1;
  } else *value+=count;
}

void gml_render_dispatch_metrics_accumulate(GmlRenderDispatchMetrics *total,
                                            const GmlRenderDispatchMetrics *epoch){
  if(!total || !epoch) return;
  for(unsigned family=0;family<GML_RENDER_SCENE_FAMILY_COUNT;family++){
    dispatch_add(total,&total->gpu[family],epoch->gpu[family]);
    for(unsigned reason=0;reason<GML_RENDER_DISPATCH_REASON_COUNT;reason++)
      dispatch_add(total,&total->cpu[family][reason],epoch->cpu[family][reason]);
    for(unsigned exception=0;exception<GML_RENDER_DISPATCH_EXCEPTION_COUNT;exception++)
      dispatch_add(total,&total->exceptions[family][exception],epoch->exceptions[family][exception]);
  }
  dispatch_add(total,&total->skipped,epoch->skipped);
  dispatch_add(total,&total->failed,epoch->failed);
  dispatch_add(total,&total->unclassified,epoch->unclassified);
  total->overflow=total->overflow || epoch->overflow;
  total->enabled=total->enabled || epoch->enabled;
  /* This metadata helper cannot certify a whole content-load session. */
  total->complete=0;
}

unsigned gml_render_dispatch_cpu_reason(const GmlRender *render){
  if(!gml_render_device(render)) return GML_RENDER_DISPATCH_NO_DEVICE;
  if(!render->resident_application_pass) return GML_RENDER_DISPATCH_NO_SCENE;
  /* Admission has multiple independent exclusions. Do not infer a particular
   * geometry, blend or mask rejection merely from the current draw state. */
  return GML_RENDER_DISPATCH_UNQUALIFIED;
}

void gml_render_dispatch_cpu_record(GmlRender *render,unsigned family,unsigned reason,
                                     uint64_t count){
  if(!render || !render->dispatch_metrics.enabled || !count) return;
  GmlRenderDispatchMetrics *metrics=&render->dispatch_metrics;
  if(!family || family>=GML_RENDER_SCENE_FAMILY_COUNT ||
     reason>=GML_RENDER_DISPATCH_REASON_COUNT){
    dispatch_add(metrics,&metrics->unclassified,count);
    return;
  }
  dispatch_add(metrics,&metrics->cpu[family][reason],count);
  dispatch_frame_cpu(render,family,reason,count);
}

void gml_render_dispatch_cpu_exception_record(GmlRender *render,unsigned family,
                                              unsigned exception,uint64_t count){
  if(!render || !render->dispatch_metrics.enabled || !count) return;
  GmlRenderDispatchMetrics *metrics=&render->dispatch_metrics;
  if(!family || family>=GML_RENDER_SCENE_FAMILY_COUNT ||
     exception>=GML_RENDER_DISPATCH_EXCEPTION_COUNT){
    dispatch_add(metrics,&metrics->unclassified,count);
    return;
  }
  dispatch_add(metrics,&metrics->exceptions[family][exception],count);
}

void gml_render_dispatch_cpu_composition(GmlRender *render,unsigned family){
  gml_render_dispatch_cpu_record(render,family,GML_RENDER_DISPATCH_COMPOSITION,1u);
}

void gml_render_dispatch_gpu_composition(GmlRender *render,unsigned family){
  if(!render || !render->dispatch_metrics.enabled) return;
  GmlRenderDispatchMetrics *metrics=&render->dispatch_metrics;
  if(!family || family>=GML_RENDER_SCENE_FAMILY_COUNT)
    dispatch_add(metrics,&metrics->unclassified,1u);
  else dispatch_add(metrics,&metrics->gpu[family],1u);
}

void gml_render_dispatch_cpu_replay_completion(GmlRender *render,
                                               const GmlRenderDrawOp *operation,int succeeded){
  GmlRenderDrawBatch completed={operation,operation?1u:0u,NULL,0u};
  gml_render_dispatch_batch_record(render,&completed,
    succeeded?GML_RENDER_RESOURCE_EXEC_CPU:GML_RENDER_RESOURCE_EXEC_FAILED);
}

void gml_render_dispatch_gpu_plan_completion(GmlRender *render,
                                              const GmlRenderPlan *plan,int succeeded){
  if(!render || !render->dispatch_metrics.enabled) return;
  GmlRenderDispatchMetrics *metrics=&render->dispatch_metrics;
  if(!plan || !plan->operation_count || plan->operation_count>GML_PLAN_MAX_OPERATIONS){
    dispatch_add(metrics,&metrics->unclassified,1u);
    return;
  }
  if(!succeeded){
    /* Partial device prefixes are not credited as completed operations. */
    dispatch_add(metrics,&metrics->failed,plan->operation_count);
    return;
  }
  for(uint32_t index=0;index<plan->operation_count;index++){
    unsigned family;
    switch(plan->operations[index].opcode){
      case GML_PLAN_OP_CLEAR_XRGB: family=GML_RENDER_SCENE_PRIMITIVE; break;
      case GML_PLAN_OP_BLIT_OPAQUE_NEAREST:
      case GML_PLAN_OP_BLIT_OPAQUE_BOX:
      case GML_PLAN_OP_BLIT_OPAQUE_SHARP_BILINEAR:
      case GML_PLAN_OP_PRESENT_CPU_FRAME: family=GML_RENDER_SCENE_SURFACE; break;
      case GML_PLAN_OP_SHADER_DRAW: family=GML_RENDER_SCENE_EFFECT; break;
      default: dispatch_add(metrics,&metrics->unclassified,1u); continue;
    }
    dispatch_add(metrics,&metrics->gpu[family],1u);
  }
}

void gml_render_dispatch_batch_record(GmlRender *render,const GmlRenderDrawBatch *batch,
                                       GmlRenderResourceExecution result){
  if(render && result==GML_RENDER_RESOURCE_EXEC_SKIPPED){
    /* Preparators may have predicted coverage for this draw before device refusal.
     * Omission cannot certify that prediction,even with diagnostics disabled. */
    render->fb_opaque_known=render->fb_all_opaque=render->fb_all_transparent=0;
    render->app_surface_opaque=render->app_presentation_coverage_active=0;
    render->classic_phase_opaque_known=render->classic_phase_all_opaque=0;
    render->classic_phase_all_transparent=0;
  }
  if(!render || !render->dispatch_metrics.enabled) return;
  GmlRenderDispatchMetrics *metrics=&render->dispatch_metrics;
  if(!batch || !batch->operations || !batch->operation_count){
    dispatch_add(metrics,&metrics->unclassified,1u);
    return;
  }
  if(result==GML_RENDER_RESOURCE_EXEC_FAILED){
    /* A failed or retried batch is not completed GPU work. The device's existing
     * partial-prefix counters retain physical writes from an unsuccessful batch. */
    dispatch_add(metrics,&metrics->failed,batch->operation_count);
    return;
  }
  if(result==GML_RENDER_RESOURCE_EXEC_SKIPPED){
    dispatch_add(metrics,&metrics->skipped,batch->operation_count);
    return;
  }
  if(result!=GML_RENDER_RESOURCE_EXEC_DEVICE && result!=GML_RENDER_RESOURCE_EXEC_CPU){
    dispatch_add(metrics,&metrics->unclassified,batch->operation_count);
    return;
  }
  if(result==GML_RENDER_RESOURCE_EXEC_CPU && render->resident_application_pass)
    gml_render_dispatch_resident_lost(render,GML_RENDER_RESIDENT_EXEC_CPU);
  for(uint32_t index=0;index<batch->operation_count;index++){
    unsigned family=batch->operations[index].scene_family;
    /* Clears are source-free primitive operations; older prepared records did
     * not need a content-family tag. Do not change their journal/state bytes. */
    if(!family && batch->operations[index].opcode==GML_RENDER_OP_CLEAR)
      family=GML_RENDER_SCENE_PRIMITIVE;
    if(result==GML_RENDER_RESOURCE_EXEC_CPU)
      gml_render_dispatch_cpu_record(render,family,GML_RENDER_DISPATCH_REPLAY,1u);
    else if(!family || family>=GML_RENDER_SCENE_FAMILY_COUNT)
      dispatch_add(metrics,&metrics->unclassified,1u);
    else dispatch_add(metrics,&metrics->gpu[family],1u);
  }
}

static int classic_blended_rect_valid(const GmlRenderDrawOp *operation,unsigned kind){
  if(!operation || operation->opcode!=GML_RENDER_OP_COLOURED_PRIMITIVE ||
     operation->reconstruction_mode!=GML_RENDER_DEVICE_CHECKPOINT ||
     operation->scene_family!=GML_RENDER_SCENE_PRIMITIVE ||
     operation->source_count || operation->sources) return 0;
  const GmlRenderPrimitiveRect *p=&operation->payload.primitive;
  const GmlRenderTargetRef *t=&operation->target;
  return p->kind==kind &&
    (t->image.resource_class==GML_RENDER_RESOURCE_SCREEN ||
     (kind==GML_RENDER_PRIMITIVE_STUDIO_ALPHA_RECT &&
      t->image.resource_class==GML_RENDER_RESOURCE_APPLICATION)) &&
    t->pixel_format==GML_RENDER_PIXEL_ARGB8888 && t->width && t->height &&
    t->width<=16384u && t->height<=16384u && !t->region.x && !t->region.y &&
    t->region.width==t->width && t->region.height==t->height &&
    p->region.x>=0 && p->region.y>=0 && p->region.width && p->region.height &&
    (uint64_t)(uint32_t)p->region.x+p->region.width<=t->width &&
    (uint64_t)(uint32_t)p->region.y+p->region.height<=t->height &&
    (p->colour>>24)==255u && p->alpha_blend==1u && !p->interpolate &&
    isfinite(p->alpha) && p->alpha>0.0 &&
    (kind==GML_RENDER_PRIMITIVE_CLASSIC_INVERSE_RECT?p->alpha==1.0:p->alpha<1.0);
}

int gml_render_classic_alpha_rect_valid(const GmlRenderDrawOp *operation){
  return classic_blended_rect_valid(operation,GML_RENDER_PRIMITIVE_CLASSIC_ALPHA_RECT);
}

int gml_render_studio_alpha_rect_valid(const GmlRenderDrawOp *operation){
  return classic_blended_rect_valid(operation,GML_RENDER_PRIMITIVE_STUDIO_ALPHA_RECT) &&
    operation->payload.primitive.alpha>=1.0/256.0;
}

int gml_render_classic_inverse_rect_valid(const GmlRenderDrawOp *operation){
  return classic_blended_rect_valid(operation,GML_RENDER_PRIMITIVE_CLASSIC_INVERSE_RECT);
}

int gml_render_textured_vertex_row_valid(const GmlRenderDrawOp *operation){
  if(!operation || operation->source_count!=1u || !operation->sources ||
     operation->payload.primitive.kind!=GML_RENDER_PRIMITIVE_TEXTURED_VERTEX_ROW ||
     !(operation->payload.primitive.rectangle_flags&GML_RENDER_RECTANGLE_VERTEX_SPAN) ||
     operation->payload.primitive.interpolate>1u) return 0;
  GmlRenderDrawOp rectangle=*operation;
  rectangle.source_count=0u;rectangle.sources=NULL;
  rectangle.payload.primitive.kind=GML_RENDER_PRIMITIVE_RECTANGLE;
  rectangle.payload.primitive.interpolate=0u;
  if(!gml_render_rectangle_valid(&rectangle)) return 0;
  const GmlRenderPrimitiveRect *p=&operation->payload.primitive;
  const double first[2]={p->centre_x,p->centre_y},step[2]={p->scale_x,p->scale_y};
  for(unsigned axis=0;axis<2u;axis++){
    double end=first[axis]+(p->region.width-1u)*step[axis];
    if(!isfinite(first[axis]) || !isfinite(step[axis]) || !isfinite(end) ||
       fabs(first[axis])>4096.0 || fabs(end)>4096.0 || fabs(step[axis])>4096.0) return 0;
  }
  return 1;
}

int gml_render_rectangle_valid(const GmlRenderDrawOp *operation){
  if(!operation || operation->opcode!=GML_RENDER_OP_COLOURED_PRIMITIVE ||
     operation->reconstruction_mode!=GML_RENDER_DEVICE_CHECKPOINT ||
     operation->scene_family!=GML_RENDER_SCENE_PRIMITIVE ||
     operation->source_count || operation->sources) return 0;
  const GmlRenderPrimitiveRect *p=&operation->payload.primitive;
  const GmlRenderTargetRef *t=&operation->target;
  if(p->kind!=GML_RENDER_PRIMITIVE_RECTANGLE ||
     (t->image.resource_class!=GML_RENDER_RESOURCE_SCREEN &&
      t->image.resource_class!=GML_RENDER_RESOURCE_APPLICATION &&
      t->image.resource_class!=GML_RENDER_RESOURCE_SURFACE) ||
     t->pixel_format!=GML_RENDER_PIXEL_ARGB8888 || !t->width || !t->height ||
     t->width>16384u || t->height>16384u || t->region.x || t->region.y ||
     t->region.width!=t->width || t->region.height!=t->height ||
     p->region.x<0 || p->region.y<0 || !p->region.width || !p->region.height ||
     (uint64_t)(uint32_t)p->region.x+p->region.width>t->width ||
     (uint64_t)(uint32_t)p->region.y+p->region.height>t->height ||
     p->authored_region.x < -32768 || p->authored_region.y < -32768 ||
     p->authored_region.x>32767 || p->authored_region.y>32767 ||
     !p->authored_region.width || !p->authored_region.height ||
     p->authored_region.width>65536u || p->authored_region.height>65536u ||
     (int64_t)p->authored_region.x+p->authored_region.width-1>32767 ||
     (int64_t)p->authored_region.y+p->authored_region.height-1>32767 ||
     p->region.x<p->authored_region.x || p->region.y<p->authored_region.y ||
     (int64_t)p->region.x+p->region.width>
       (int64_t)p->authored_region.x+p->authored_region.width ||
     (int64_t)p->region.y+p->region.height>
       (int64_t)p->authored_region.y+p->authored_region.height ||
     (p->rectangle_flags&~4095u) || p->alpha_blend>1u || p->interpolate ||
     !isfinite(p->alpha) || p->alpha<0.0 || p->alpha>1.0) return 0;
  if((p->rectangle_flags&GML_RENDER_RECTANGLE_SOURCE_OVER_ALPHA) &&
     p->rectangle_flags!=(GML_RENDER_RECTANGLE_TARGET_ALPHA|
                          GML_RENDER_RECTANGLE_SOURCE_OVER_ALPHA)) return 0;
  if(p->rectangle_flags&GML_RENDER_RECTANGLE_PIXEL_SPAN){
    if(p->pixel_write_mask>15u || !(p->rectangle_flags&GML_RENDER_RECTANGLE_OUTLINE) ||
       ((p->rectangle_flags&GML_RENDER_RECTANGLE_GRADIENT) &&
        !(p->rectangle_flags&GML_RENDER_RECTANGLE_CIRCLE_FAN)) ||
       (p->region.width!=1u && p->region.height!=1u) ||
       p->region.x!=p->authored_region.x || p->region.y!=p->authored_region.y ||
       p->region.width!=p->authored_region.width || p->region.height!=p->authored_region.height ||
       (!(p->rectangle_flags&GML_RENDER_RECTANGLE_CIRCLE_FAN) &&
        (p->corner_colour[0]!=p->corner_colour[1] || p->corner_colour[0]!=p->corner_colour[2] ||
         p->corner_colour[0]!=p->corner_colour[3]))) return 0;
  }else if(p->pixel_write_mask) return 0;
  if(p->rectangle_flags&GML_RENDER_RECTANGLE_VERTEX_SPAN){
    if(!(p->rectangle_flags&GML_RENDER_RECTANGLE_PIXEL_SPAN) || p->region.height!=1u ||
       p->alpha!=1.0 || p->colour!=0xFFFFFFFFu) return 0;
    for(unsigned c=0;c<4u;c++){
      double last=p->vertex_first[c]+(p->region.width-1u)*p->vertex_step[c];
      double maximum=c==3u?1.0:255.0;
      if(!isfinite(p->vertex_first[c]) || !isfinite(p->vertex_step[c]) || !isfinite(last) ||
         p->vertex_first[c]<-1e-5 || p->vertex_first[c]>maximum+1e-5 ||
         last<-1e-5 || last>maximum+1e-5) return 0;
    }
  }
  unsigned blend=(p->rectangle_flags&GML_RENDER_RECTANGLE_BLEND_MASK)>>
                 GML_RENDER_RECTANGLE_BLEND_SHIFT;
  int fan=(p->rectangle_flags&GML_RENDER_RECTANGLE_CIRCLE_FAN)!=0u;
  if(fan){
    if(!(p->rectangle_flags&GML_RENDER_RECTANGLE_OUTLINE) ||
       !(p->rectangle_flags&GML_RENDER_RECTANGLE_PIXEL_SPAN) ||
       !(p->rectangle_flags&GML_RENDER_RECTANGLE_GRADIENT) ||
       (p->rectangle_flags&GML_RENDER_RECTANGLE_VERTEX_SPAN) ||
       p->corner_colour[0]!=p->corner_colour[1] ||
       p->corner_colour[2]!=p->corner_colour[3] ||
       !isfinite(p->centre_x) || !isfinite(p->centre_y) ||
       fabs(p->centre_x)>32766.0 || fabs(p->centre_y)>32766.0 ||
       !isfinite(p->scale_x) || !isfinite(p->scale_y) || p->scale_x<=0.0 || p->scale_y<=0.0 ||
       p->scale_x>32766.0 || p->scale_y>32766.0 ||
       !isfinite(p->angle) || p->angle<4.0 || p->angle>64.0 || fmod(p->angle,4.0)!=0.0)
      return 0;
  }
  if(blend>GML_RENDER_BLEND_DEST_ALPHA_DEST_COLOUR || blend==3u || blend==6u ||
     (p->alpha==0.0 && (!p->alpha_blend || !gml_render_blend_uses_destination_colour((int)blend)))) return 0;
  if((p->rectangle_flags&GML_RENDER_RECTANGLE_PIXEL_SPAN) && p->pixel_write_mask!=15u &&
     (!p->alpha_blend || (blend!=4u && blend!=7u && blend!=8u &&
                         !gml_render_blend_uses_destination_colour((int)blend)))) return 0;
  for(unsigned corner=0;corner<4u;corner++)
    if((p->corner_colour[corner]>>24)!=255u) return 0;
  int gradient=fan || (!(p->rectangle_flags&GML_RENDER_RECTANGLE_OUTLINE) &&
    (p->corner_colour[0]!=p->corner_colour[1] || p->corner_colour[0]!=p->corner_colour[2] ||
     p->corner_colour[0]!=p->corner_colour[3]));
  if(gradient && !fan && blend && (!p->alpha_blend || !gml_render_blend_uses_destination_colour((int)blend))) return 0;
  return p->colour==p->corner_colour[0] &&
    ((p->rectangle_flags&GML_RENDER_RECTANGLE_GRADIENT)!=0u)==gradient;
}

int gml_render_particle_disc_valid(const GmlRenderDrawOp *operation){
  if(!operation || operation->opcode!=GML_RENDER_OP_COLOURED_PRIMITIVE ||
     operation->reconstruction_mode!=GML_RENDER_DEVICE_CHECKPOINT ||
     operation->scene_family!=GML_RENDER_SCENE_PRIMITIVE ||
     operation->source_count || operation->sources) return 0;
  const GmlRenderPrimitiveRect *p=&operation->payload.primitive;
  const GmlRenderTargetRef *t=&operation->target;
  return p->kind==GML_RENDER_PRIMITIVE_PARTICLE_DISC &&
    (t->image.resource_class==GML_RENDER_RESOURCE_SCREEN ||
     t->image.resource_class==GML_RENDER_RESOURCE_APPLICATION ||
     t->image.resource_class==GML_RENDER_RESOURCE_SURFACE) &&
    t->pixel_format==GML_RENDER_PIXEL_ARGB8888 && t->width && t->height &&
    t->width<=16384u && t->height<=16384u && !t->region.x && !t->region.y &&
    t->region.width==t->width && t->region.height==t->height &&
    p->region.x>=0 && p->region.y>=0 && p->region.width && p->region.height==1u &&
    (uint64_t)(unsigned)p->region.x+p->region.width<=t->width &&
    (unsigned)p->region.y<t->height && (p->colour>>24)==255u &&
    p->alpha_blend<=1u && !p->interpolate &&
    isfinite(p->centre_x) && isfinite(p->centre_y) &&
    fabs(p->centre_x)<=16384.0 && fabs(p->centre_y)<=16384.0 &&
    isfinite(p->scale_x) && isfinite(p->scale_y) &&
    p->scale_x>=.25 && p->scale_y>=.25 && p->scale_x<=16384.0 && p->scale_y<=16384.0 &&
    isfinite(p->alpha) && p->alpha>0.0 && p->alpha<=1.0;
}

static int particle_line_contains(const GmlRenderPrimitiveRect *line,
                                  double cosine,double sine,int x,int y){
  double dx=x-line->centre_x,dy=y-line->centre_y;
  double lx=cosine*dx-sine*dy,ly=sine*dx+cosine*dy;
  double sx=(lx+.5)/line->scale_x-.5,sy=(ly+.5)/line->scale_y-.5;
  if(line->interpolate) return sx>-29.0 && sx<28.0 && sy>-6.0 && sy<5.0;
  sx=floor(sx+.5);sy=floor(sy+.5);
  return sx>-29.0 && sx<28.0 && sy>-5.75 && sy<4.75;
}

int gml_render_particle_line_row_region(const GmlRenderPrimitiveRect *line,
                                       double cosine,double sine,int row,GmlRenderRect *region){
  if(!line || !region || row<line->region.y ||
     (uint64_t)(row-line->region.y)>=line->region.height) return 0;
  double first=line->region.x,last=(double)line->region.x+line->region.width-1.0;
  const double lower[2]={line->interpolate?-29.0:-28.5,line->interpolate?-6.0:-5.5};
  const double upper[2]={line->interpolate?28.0:27.5,line->interpolate?5.0:4.5};
  const double scale[2]={line->scale_x,line->scale_y};
  const double slope[2]={cosine,sine};
  double dy=row-line->centre_y;
  const double offset[2]={-cosine*line->centre_x-sine*dy,
                         -sine*line->centre_x+cosine*dy};
  /* Intersect the two support slabs before testing their final pixel edges.
   * A small conservative fringe retains binary64 half-open nearest ties;
   * only geometry is classified, never opacity or destination channels. */
  for(unsigned axis=0;axis<2u;axis++){
    if(fabs(slope[axis])<1e-8) continue;
    double a=((lower[axis]+.5)*scale[axis]-.5-offset[axis])/slope[axis];
    double b=((upper[axis]+.5)*scale[axis]-.5-offset[axis])/slope[axis];
    first=fmax(first,floor(fmin(a,b))-2.0);
    last=fmin(last,ceil(fmax(a,b))+2.0);
  }
  if(first>last) return 0;
  int left=(int)first,right=(int)last;
  while(left<=right && !particle_line_contains(line,cosine,sine,left,row)) left++;
  while(right>=left && !particle_line_contains(line,cosine,sine,right,row)) right--;
  if(left>right) return 0;
  *region=(GmlRenderRect){left,row,(uint32_t)(right-left+1),1u};
  return 1;
}

int gml_render_particle_line_valid(const GmlRenderDrawOp *operation){
  if(!operation || operation->opcode!=GML_RENDER_OP_COLOURED_PRIMITIVE ||
     operation->reconstruction_mode!=GML_RENDER_DEVICE_CHECKPOINT ||
     operation->scene_family!=GML_RENDER_SCENE_PRIMITIVE ||
     operation->source_count || operation->sources) return 0;
  const GmlRenderPrimitiveRect *p=&operation->payload.primitive;
  const GmlRenderTargetRef *t=&operation->target;
  if(p->kind!=GML_RENDER_PRIMITIVE_PARTICLE_LINE ||
     (t->image.resource_class!=GML_RENDER_RESOURCE_SCREEN &&
      t->image.resource_class!=GML_RENDER_RESOURCE_APPLICATION &&
      t->image.resource_class!=GML_RENDER_RESOURCE_SURFACE) ||
     t->pixel_format!=GML_RENDER_PIXEL_ARGB8888 || !t->width || !t->height ||
     t->width>16384u || t->height>16384u ||
     t->region.x || t->region.y || t->region.width!=t->width || t->region.height!=t->height ||
     p->region.x<0 || p->region.y<0 || !p->region.width || !p->region.height ||
     (uint64_t)(uint32_t)p->region.x+p->region.width>t->width ||
     (uint64_t)(uint32_t)p->region.y+p->region.height>t->height ||
     (p->colour>>24)!=255u || p->interpolate>1u || p->alpha_blend>1u ||
     !isfinite(p->centre_x) || !isfinite(p->centre_y) ||
     !isfinite(p->scale_x) || !isfinite(p->scale_y) || !isfinite(p->angle) ||
     !isfinite(p->alpha) || p->alpha<=0.0 || p->alpha>1.0 ||
     fabs(p->centre_x)>16384.0 || fabs(p->centre_y)>16384.0 ||
     fabs(p->scale_x)<1.0/128.0 || fabs(p->scale_y)<1.0/128.0 ||
     fabs(p->scale_x)>128.0 || fabs(p->scale_y)>128.0 ||
     fabs(p->angle)>360.0) return 0;
  return 1;
}

int gml_render_stretched_float_visible_phase_exact(uint32_t logical,uint32_t destination,
                                                  uint32_t first,uint32_t count){
  if(!logical || logical>16384u || logical>=destination || destination>1000000u ||
     !count || first>=destination || count>destination-first ||
     ((uint64_t)(first+count-1u)*2u+1u)*logical+destination>INT32_MAX) return 0;
  const uint32_t denominator=destination*2u;
  for(uint32_t pixel=first;pixel<first+count;pixel++){
    /* The preserved CPU band divides in double before subtracting its integer
     * tap. Near a float rounding boundary that cancellation need not match a
     * correctly rounded rational phase. Qualify the whole axis before writes;
     * this is an admission guard, not another sampling kernel. Clipped large
     * magnifications qualify only the visible interval, before any target write. */
    double phase=(pixel+0.5)*logical/(double)destination-0.5;
    uint32_t numerator=(pixel*2u+1u)*logical+destination;
    if((float)(phase-floor(phase))!=
       (float)((numerator%denominator)/(double)denominator)) return 0;
  }
  return 1;
}

int gml_render_stretched_float_phase_exact(uint32_t logical,uint32_t destination){
  /* Retained full-axis recipes keep their original size limit and qualify
   * every phase, including clipped pixels, for the existing CPU replay. */
  return destination<=16384u &&
    gml_render_stretched_float_visible_phase_exact(logical,destination,0u,destination);
}

int gml_render_filtered_font_quad_valid(const GmlRenderQuad *q,
                                        uint32_t width,uint32_t height){
  /* Ordinary GUI sprites use the same exact channel/blend arithmetic. Their
   * independent extent and physical-neighbour TPAG taps are distinct metadata,
   * not another sampling implementation or a fabricated TEXT operation. */
  int sprite=q && q->sampling.quantisation==GML_RENDER_QUANTISE_SPRITE_FIRSTGEN_INDEPENDENT;
  if(!q || !width || !height || width>16384u || height>16384u ||
     (q->sampling.quantisation!=GML_RENDER_QUANTISE_FONT_FIRSTGEN &&
      q->sampling.quantisation!=GML_RENDER_QUANTISE_FONT_MODERN &&
      q->sampling.quantisation!=GML_RENDER_QUANTISE_FONT_FIRSTGEN_INDEPENDENT && !sprite) ||
     q->shape!=GML_RENDER_QUAD_AXIS_ALIGNED || q->span_active ||
     q->sampling.filter!=GML_RENDER_FILTER_BILINEAR ||
     (sprite
       ? q->sampling.logical_margin_mask>15u ||
         q->sampling.edge_rule!=(q->sampling.logical_margin_mask
           ?GML_RENDER_EDGE_LOGICAL_TPAG:GML_RENDER_EDGE_CLAMP_SUBIMAGE)
       : q->sampling.edge_rule!=GML_RENDER_EDGE_CLAMP_SUBIMAGE ||
         q->sampling.logical_margin_mask) ||
     !q->sampling.source_region.width || q->sampling.source_region.width>(sprite?16384u:256u) ||
     !q->sampling.source_region.height || q->sampling.source_region.height>(sprite?16384u:256u) ||
     !isfinite(q->filtered_draw_alpha) || q->filtered_draw_alpha<0x1p-32 ||
     q->filtered_draw_alpha>1.0 || q->inverse_x[1]!=0.0 || q->inverse_y[0]!=0.0 ||
     q->inverse_offset[0]!=0.0 || q->inverse_offset[1]!=0.0) return 0;
  for(unsigned axis=0;axis<2u;axis++){
    double origin=q->inverse_anchor[axis];
    double scale=axis?q->inverse_y[1]:q->inverse_x[0];
    double first=axis?q->y[0]:q->x[0];
    double last=axis?q->y[2]:q->x[1];
    double extent=axis?q->sampling.source_region.height:q->sampling.source_region.width;
    if(!isfinite(origin) || fpclassify(origin)==FP_SUBNORMAL || fabs(origin)>1048576.0 ||
       !isfinite(scale) || (sprite?fabs(scale)<0x1p-16 || fabs(scale)>4.0:
                                               scale<0x1p-16 || scale>1.0) ||
       first!=floor(origin+0.5)-(sprite && scale<0.0?floor(extent*-scale+0.5):0.0) ||
       last!=floor(last) ||
       last<=first || last<=0.0 || first>=(double)(axis?height:width)) return 0;
    double expected=q->sampling.quantisation==GML_RENDER_QUANTISE_FONT_FIRSTGEN &&
      fabs(scale-nearbyint(scale))>1e-9
        ? floor(origin+extent*scale+0.5)-first : floor(extent*fabs(scale)+0.5);
    if(last-first!=expected) return 0;
  }
  if(q->x[3]!=q->x[0] || q->x[2]!=q->x[1] ||
     q->y[1]!=q->y[0] || q->y[3]!=q->y[2]) return 0;
  for(unsigned corner=0;corner<4u;corner++)
    if(q->corner_colour[corner]!=q->corner_colour[0] ||
       (q->corner_colour[corner]>>24)!=255u) return 0;
  return 1;
}

int gml_render_automatic_surface_quad_valid(const GmlRenderQuad *q,
                                             uint32_t width,uint32_t height){
  if(!q || !width || !height || width>16384u || height>16384u ||
     (q->sampling.quantisation!=GML_RENDER_QUANTISE_AUTOMATIC_SURFACE_FIRSTGEN &&
      q->sampling.quantisation!=GML_RENDER_QUANTISE_AUTOMATIC_SURFACE_MODERN) ||
     q->shape!=GML_RENDER_QUAD_AXIS_ALIGNED || q->span_active || q->source_opaque>1u ||
     (q->sampling.filter!=GML_RENDER_FILTER_NEAREST &&
      q->sampling.filter!=GML_RENDER_FILTER_BOX) ||
     q->sampling.edge_rule!=GML_RENDER_EDGE_CLAMP_SUBIMAGE ||
     q->sampling.logical_margin_mask || q->sampling.source_region.x ||
     q->sampling.source_region.y || !q->sampling.source_region.width ||
     !q->sampling.source_region.height || q->sampling.source_region.width>16384u ||
     q->sampling.source_region.height>16384u) return 0;
  uint32_t samples=1u;
  for(unsigned axis=0;axis<2u;axis++){
    double first=axis?q->y[0]:q->x[0],last=axis?q->y[2]:q->x[1];
    uint32_t source=axis?q->sampling.source_region.height:q->sampling.source_region.width;
    if(!isfinite(first) || !isfinite(last) || floor(first)!=first || floor(last)!=last ||
       first<-16384.0 || first>=16384.0 || last>32768.0 || last<=0.0 ||
       first>=(double)(axis?height:width) || last<=first || last-first>16384.0) return 0;
    uint32_t extent=(uint32_t)(last-first);
    samples*=(source+extent-1u)/extent;
  }
  if(q->sampling.filter==GML_RENDER_FILTER_BOX && samples>4096u) return 0;
  if(q->x[3]!=q->x[0] || q->x[2]!=q->x[1] ||
     q->y[1]!=q->y[0] || q->y[3]!=q->y[2]) return 0;
  for(unsigned corner=0;corner<4u;corner++)
    if(q->corner_colour[corner]!=0xFFFFFFFFu ||
       q->u[corner]!=(corner==0u || corner==3u?0.0:1.0) ||
       q->v[corner]!=(corner<2u?0.0:1.0)) return 0;
  return 1;
}

static void render_unbind_device(GmlRender *render,int context_current){
  if(!render) return;
  if(context_current) (void)gml_render_checkpoint_pending_resources(render);
  /* A queued exact batch can replay through the CPU after the device binding is removed. A
   * non-reconstructible pending result instead becomes invalid when its context is already lost. */
  render->device_bound=0;
  render->scene_device_enabled=0;
  (void)gml_render_application_scene_flush(render);
  for(int index=0;index<GML_MAX_SURFACES;index++)
    gml_render_resource_context_lost(render->surface[index].resource);
  gml_render_resource_context_lost(render->app_surface_resource);
  gml_render_resource_context_lost(render->base_scene_resource);
  gml_render_resource_context_lost(render->retained_base_resource);
  gml_render_resource_context_lost(render->retained_base_history_resource);
  gml_render_resource_context_lost(render->view_base_resource);
  gml_render_resource_context_lost(render->view_base_history_resource);
  gml_render_resource_context_lost(render->base_history_resource);
  gml_render_resource_context_lost(render->classic_phase_resource);
  for(unsigned index=0;index<3u;index++)
    gml_render_resource_context_lost(render->classic_interp_resource[index].resource);
  memset(&render->device,0,sizeof render->device);
}

void gml_render_abandon_device(GmlRender *render){
  render_unbind_device(render,0);
}

void gml_render_bind_device(GmlRender *render,const GmlRenderDeviceServices *services){
  if(!render) return;
  if(!services || !services->context || !services->capabilities || !services->submit ||
     !services->upload || !services->readback || !services->retire || !services->flush ||
     !services->wait){
    render_unbind_device(render,1);
    return;
  }
  if(!gml_render_application_scene_flush(render)) return;
  if(!gml_render_checkpoint_pending_resources(render)) return;
  render->device=*services;
  render->device_bound=1;
}

const GmlRenderDeviceServices *gml_render_device(const GmlRender *render){
  return render && render->device_bound?&render->device:NULL;
}
void gml_render_set_scene_device(GmlRender *render,int enabled){
  if(render) render->scene_device_enabled=enabled && gml_render_device(render);
}
