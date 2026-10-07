/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Shared surface identity, lifetime, target stack, and read policy. */
#include "gml_render_surfaces_internal.h"
#include "gml_render_cpu_surfaces.h"
#include "gml_render.h"
#include "gml_render_internal.h"
#include "gml_render_shaders.h"
#include "anygm_host.h"

#include <stdint.h>
#include <limits.h>
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

static _Atomic uint64_t next_scene_incarnation=1u;

uint64_t gml_render_scene_incarnation_allocate(void){
  uint64_t current=atomic_load_explicit(&next_scene_incarnation,memory_order_relaxed);
  for(;;){
    if(current==UINT64_MAX) return 0;
    if(atomic_compare_exchange_weak_explicit(&next_scene_incarnation,&current,
                                             current+1u,memory_order_relaxed,
                                             memory_order_relaxed)) return current;
  }
}

/* Colour and coverage counted apart. A surface can be fully authored and hold no colour at all:
 * a scanline or shadow mask is black with an alpha ramp, so a count of non-black RGB reads zero
 * over a mask that drew perfectly, and reading that zero as "nothing was drawn" is a wrong answer
 * an instrument handed out rather than a defect in the renderer. */
static void surface_coverage_counts(const uint32_t *px,int w,int h,int *colour,int *covered){
  int c=0,a=0;
  for(size_t i=0;i<(size_t)w*(size_t)h;i++){
    if(px[i]&0x00FFFFFFu) c++;
    if(px[i]&0xFF000000u) a++;
  }
  *colour=c; *covered=a;
}
int surface_slot(int id){ return id>0 && id<=GML_MAX_SURFACES ? id-1 : -1; }

enum { GML_SCENE_BATCH_LIMIT=2048, GML_SCENE_STATE_LIMIT=128,
       GML_SCENE_SOURCE_LIMIT=64 };
struct GmlRenderPendingSceneBatch {
  GmlRenderDrawOp operations[GML_SCENE_BATCH_LIMIT];
  GmlRenderDrawStateSnapshot states[GML_SCENE_STATE_LIMIT];
  GmlRenderResourceSource sources[GML_SCENE_SOURCE_LIMIT];
  GmlRenderImageRef source_refs[GML_SCENE_SOURCE_LIMIT];
  uint32_t count,state_count,source_count;
  int coalesce_immutable;
};

static int same_scene_source(GmlRenderImageRef a,GmlRenderImageRef b){
  return a.resource_class==b.resource_class && a.identity==b.identity &&
         a.incarnation==b.incarnation && a.content_version==b.content_version;
}

static int same_scene_source_view(const GmlRenderResourceSource *a,
                                  const GmlRenderResourceSource *b){
  return a->bytes==b->bytes && a->byte_capacity==b->byte_capacity &&
         a->width==b->width && a->height==b->height &&
         a->pixel_format==b->pixel_format && a->stride_bytes==b->stride_bytes &&
         a->borrowed_immutable==b->borrowed_immutable &&
         a->resident_resource==b->resident_resource;
}

GmlRenderImageRef gml_render_application_scene_next_image_ref(
  GmlRender *r,GmlRenderResource *resource){
  GmlRenderImageRef image=gml_render_resource_image_ref(resource);
  uint32_t pending=r && r->pending_scene_batch?r->pending_scene_batch->count:0u;
  if(image.content_version>UINT64_MAX-pending) image.content_version=UINT64_MAX;
  else image.content_version+=pending;
  return image;
}

int gml_render_application_scene_flush(GmlRender *r){
  GmlRenderPendingSceneBatch *pending=r?r->pending_scene_batch:NULL;
  GmlRenderResource *target;
  GmlRenderDrawBatch batch;
  GmlRenderResourceExecution result;
  if(!pending || !pending->count) return 1;
  GmlRenderImageRef image=pending->operations[0].target.image;
  if(image.resource_class==GML_RENDER_RESOURCE_SCREEN) target=r->base_scene_resource;
  else if(image.resource_class==GML_RENDER_RESOURCE_APPLICATION) target=r->app_surface_resource;
  else if(image.resource_class==GML_RENDER_RESOURCE_SURFACE &&
          image.identity>0u && image.identity<=GML_MAX_SURFACES)
    target=r->surface[image.identity-1u].resource;
  else target=NULL;
  if(!target) return 0;
  GmlRenderImageRef current=gml_render_resource_image_ref(target);
  if(current.resource_class!=image.resource_class || current.identity!=image.identity ||
     current.incarnation!=image.incarnation) return 0;
  batch=(GmlRenderDrawBatch){pending->operations,pending->count,
                              pending->states,pending->state_count};
  result=gml_render_resource_submit_deferred(target,gml_render_device(r),
                                    &batch,pending->sources,pending->source_count);
  gml_render_dispatch_batch_record(r,&batch,result);
  if(result==GML_RENDER_RESOURCE_EXEC_FAILED){
    GmlRenderJournalMetrics metrics={0};
    gml_render_resource_journal_metrics(target,&metrics);
    /* Immutable bytes permit a pre-submit retry. A mutable runtime atlas may relocate at the
     * next glyph demand, so its failed submission must not leave a borrowed pointer queued;
     * the caller will cross the CPU barrier and draw the glyph there instead. */
    if(metrics.authority==GML_RENDER_RESOURCE_INVALID ||
       (pending->source_count && !pending->sources[0].borrowed_immutable &&
        !pending->sources[0].resident_resource)){
      pending->count=0;
      pending->state_count=0;
      pending->source_count=0;
    }
    return 0;
  }
  pending->count=0;
  pending->state_count=0;
  pending->source_count=0;
  return 1;
}

int gml_render_checkpoint_pending_resources(GmlRender *r){
  const GmlRenderDeviceServices *device=gml_render_device(r);
  if(!r) return 0;
  for(int index=0;index<GML_MAX_SURFACES;index++)
    if(r->surface[index].resource &&
       !gml_render_resource_checkpoint(r->surface[index].resource,device)) return 0;
  if(r->app_surface_resource &&
     !gml_render_resource_checkpoint(r->app_surface_resource,device)) return 0;
  return !r->base_scene_resource ||
    gml_render_resource_checkpoint(r->base_scene_resource,device);
}

int gml_render_observe_device_resources(GmlRender *r){
  const GmlRenderDeviceServices *device=gml_render_device(r);
  if(!r || !device || !gml_render_application_scene_flush(r)) return 0;
  for(int index=0;index<GML_MAX_SURFACES;index++)
    if(r->surface[index].resource &&
       !gml_render_resource_observe_device(r->surface[index].resource,device)) return 0;
  if(r->app_surface_resource &&
     !gml_render_resource_observe_device(r->app_surface_resource,device)) return 0;
  return !r->base_scene_resource ||
    gml_render_resource_observe_device(r->base_scene_resource,device);
}

void gml_render_application_scene_queue_dispose(GmlRender *r){
  if(!r) return;
  free(r->pending_scene_batch);
  r->pending_scene_batch=NULL;
}

static int resource_loses_device_pixels(const GmlRenderResource *resource){
  GmlRenderJournalMetrics metrics={0};
  if(!resource) return 0;
  gml_render_resource_journal_metrics(resource,&metrics);
  return metrics.authority==GML_RENDER_RESOURCE_INVALID ||
         metrics.cpu_version!=metrics.logical_version;
}

int gml_render_device_observation_pending(const GmlRender *r){
  if(!r) return 0;
  if(r->pending_scene_batch && r->pending_scene_batch->count) return 1;
  for(int index=0;index<GML_MAX_SURFACES;index++)
    if(resource_loses_device_pixels(r->surface[index].resource)) return 1;
  return resource_loses_device_pixels(r->app_surface_resource) ||
         resource_loses_device_pixels(r->base_scene_resource);
}

void gml_render_lose_volatile_device(GmlRender *r){
  uint64_t lost_surfaces=0u;
  int lost_application,owned_width,owned_height;
  if(!r) return;
  for(int index=0;index<GML_MAX_SURFACES;index++)
    if(resource_loses_device_pixels(r->surface[index].resource))
      lost_surfaces|=UINT64_C(1)<<index;
  lost_application=resource_loses_device_pixels(r->app_surface_resource);
  owned_width=r->app_surface_resource?r->app_surface_resource_width:
    r->app_surface==r->app_surface_owned?r->app_w:0;
  owned_height=r->app_surface_resource?r->app_surface_resource_height:
    r->app_surface==r->app_surface_owned?r->app_h:0;
  if(r->pending_scene_batch && r->pending_scene_batch->count){
    GmlRenderImageRef image=r->pending_scene_batch->operations[0].target.image;
    if(image.resource_class==GML_RENDER_RESOURCE_SURFACE &&
       image.identity>0u && image.identity<=GML_MAX_SURFACES)
      lost_surfaces|=UINT64_C(1)<<(image.identity-1u);
    else if(image.resource_class==GML_RENDER_RESOURCE_APPLICATION) lost_application=1;
  }
  /* Ordinary unbinding can replay an exact queued batch. Real scene loss must discard it
   * before that existing path, and disable delayed CPU writes before unwinding target borrows. */
  gml_render_application_scene_queue_dispose(r);
  r->pending_fill=r->pending_underlay=0;
  r->rotated_batch_count=r->rotated_batch_building=0;
  gml_render_discard_deferred_presentation(r);
  for(int stack=0;stack<r->target_sp;stack++){
    r->target_stack[stack].pending_fill=r->target_stack[stack].pending_underlay=0;
    r->target_stack[stack].resident_pass=0;
    r->target_stack[stack].opaque_known=r->target_stack[stack].all_opaque=
      r->target_stack[stack].all_transparent=0;
  }
  gml_render_abandon_device(r);
  for(int index=0;index<GML_MAX_SURFACES;index++)
    if(lost_surfaces&(UINT64_C(1)<<index)) gml_surface_free(r,index+1);
  r->content_authored_surfaces&=~lost_surfaces;
  if(lost_application){
    gml_render_application_resource_dispose(r);
    if(r->app_surface_owned && owned_width>0 && owned_height>0)
      memset(r->app_surface_owned,0,(size_t)owned_width*owned_height*sizeof(uint32_t));
    r->app_surface_opaque=r->app_presentation_coverage_active=0;
  }
  /* A freed user surface may have been borrowed as surface zero. Recover the separately
   * owned application allocation only with its known extent; otherwise the next Draw binds it. */
  if(!r->app_surface && r->app_surface_owned && owned_width>0 && owned_height>0)
    gml_render_application_surface_bind(r,r->app_surface_owned,owned_width,owned_height,0);
  gml_render_base_resource_dispose(r);
  r->resident_application_pass=r->resident_scene_pass=0;
  r->fb_opaque_known=r->fb_all_opaque=r->fb_all_transparent=0;
}

int gml_render_application_scene_enqueue_primitive(GmlRender *r,
                                                   const GmlRenderDrawOp *operation){
  if(!r || !r->resident_application_pass || !gml_render_device(r) ||
     (!gml_render_particle_line_valid(operation) &&
      !gml_render_classic_alpha_rect_valid(operation))) return 0;
  GmlRenderResource *target=gml_render_scene_target_resource_ensure(r);
  if(!target) return 0;
  GmlRenderPendingSceneBatch *pending=r->pending_scene_batch;
  if(pending && pending->count &&
     (pending->source_count || pending->count==GML_SCENE_BATCH_LIMIT ||
      pending->operations[0].target.image.resource_class!=operation->target.image.resource_class ||
      pending->operations[0].target.image.identity!=operation->target.image.identity ||
      pending->operations[0].target.image.incarnation!=operation->target.image.incarnation) &&
     !gml_render_application_scene_flush(r)) return 0;
  GmlRenderImageRef next=gml_render_application_scene_next_image_ref(r,target);
  if(next.content_version==UINT64_MAX ||
     operation->target.image.content_version!=next.content_version+1u ||
     operation->target.image.resource_class!=next.resource_class ||
     operation->target.image.identity!=next.identity ||
     operation->target.image.incarnation!=next.incarnation) return 0;
  if(!pending){
    pending=(GmlRenderPendingSceneBatch*)calloc(1,sizeof *pending);
    if(!pending) return 0;
    r->pending_scene_batch=pending;
  }
  pending->operations[pending->count]=*operation;
  pending->operations[pending->count++].sources=NULL;
  if(r->fb==r->app_surface_owned){
    r->app_surface_opaque=0;
    r->app_presentation_coverage_active=0;
  }
  return 1;
}

int gml_render_application_scene_enqueue(GmlRender *r,const GmlRenderDrawOp *operation,
                                          const GmlRenderDrawStateSnapshot *state,
                                          const GmlRenderResourceSource *source){
  GmlRenderPendingSceneBatch *pending;
  GmlRenderImageRef next;
  uint32_t state_index=0,source_index=0;
  int coalescible,exact_filtered_font,versioned_surface;
  versioned_surface=operation && source && source->resident_resource &&
    (source->image.resource_class==GML_RENDER_RESOURCE_SURFACE ||
     source->image.resource_class==GML_RENDER_RESOURCE_APPLICATION) &&
    operation->scene_family==GML_RENDER_SCENE_SURFACE &&
    operation->reconstruction_mode==GML_RENDER_DEVICE_CHECKPOINT &&
    operation->opcode==GML_RENDER_OP_TEXTURED_QUAD &&
    (operation->payload.quad.sampling.filter==GML_RENDER_FILTER_HARDWARE_BILINEAR ||
     operation->payload.quad.sampling.filter==GML_RENDER_FILTER_HARDWARE_PIXEL_CENTRE_POINT ||
     ((operation->payload.quad.sampling.filter==GML_RENDER_FILTER_NEAREST ||
       (operation->payload.quad.sampling.filter==GML_RENDER_FILTER_BOX && state &&
        state->rgb_equation>=GML_RENDER_BLEND_EXACT_DESTINATION_ADD &&
        state->rgb_equation<=GML_RENDER_BLEND_EXACT_DESTINATION_MIN &&
        state->alpha_equation>=GML_RENDER_BLEND_EXACT_DESTINATION_ADD &&
        state->alpha_equation<=GML_RENDER_BLEND_EXACT_DESTINATION_MIN)) && state &&
      (state->coverage_policy==GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA ||
       (state->coverage_policy==0u &&
        state->rgb_equation>=GML_RENDER_BLEND_EXACT_DESTINATION_ADD &&
        state->rgb_equation<=GML_RENDER_BLEND_EXACT_DESTINATION_MIN &&
        state->alpha_equation>=GML_RENDER_BLEND_EXACT_DESTINATION_ADD &&
        state->alpha_equation<=GML_RENDER_BLEND_EXACT_DESTINATION_MIN))));
  if(!r || !r->resident_application_pass || !gml_render_device(r) ||
     !gml_render_scene_target_resource_ensure(r) || !operation || !state || !source ||
     (!source->borrowed_immutable && !versioned_surface &&
      (source->image.resource_class!=GML_RENDER_RESOURCE_ATLAS ||
       source->resident_resource)) || !operation->sources ||
     operation->source_count!=1u || operation->opcode!=GML_RENDER_OP_TEXTURED_QUAD ||
     (operation->reconstruction_mode!=GML_RENDER_REPLAY_EXACT &&
      operation->reconstruction_mode!=GML_RENDER_DEVICE_CHECKPOINT) ||
     !same_scene_source(operation->sources[0],source->image)) return 0;
  exact_filtered_font=operation->reconstruction_mode==GML_RENDER_REPLAY_EXACT &&
    operation->scene_family==GML_RENDER_SCENE_TEXT &&
    state->rgb_equation==GML_RENDER_BLEND_EXACT_FILTERED_FONT &&
    state->alpha_equation==GML_RENDER_BLEND_EXACT_FILTERED_FONT &&
    gml_render_filtered_font_quad_valid(&operation->payload.quad,
                                        operation->target.width,operation->target.height);
  coalescible=source->borrowed_immutable &&
    (operation->scene_family==GML_RENDER_SCENE_TEXT ||
     operation->scene_family==GML_RENDER_SCENE_SPRITE) &&
    (operation->reconstruction_mode==GML_RENDER_DEVICE_CHECKPOINT || exact_filtered_font) &&
    (operation->payload.quad.sampling.filter==GML_RENDER_FILTER_BILINEAR ||
     operation->payload.quad.sampling.filter==GML_RENDER_FILTER_NEAREST_PHASED ||
     operation->payload.quad.sampling.filter==GML_RENDER_FILTER_NEAREST);
  pending=r->pending_scene_batch;
  if(pending && pending->count){
    int flush=!pending->source_count || pending->count==GML_SCENE_BATCH_LIMIT ||
      pending->coalesce_immutable!=coalescible;
    if(!flush && coalescible){
      source_index=pending->source_count;
      for(uint32_t index=0;index<pending->source_count;index++)
        if(same_scene_source(pending->source_refs[index],source->image)){
          if(!same_scene_source_view(&pending->sources[index],source)) flush=1;
          else source_index=index;
          break;
        }
      state_index=pending->state_count;
      for(uint32_t index=0;index<pending->state_count;index++)
        if(!memcmp(&pending->states[index],state,sizeof *state)){
          state_index=index;
          break;
        }
      if(source_index==GML_SCENE_SOURCE_LIMIT ||
         state_index==GML_SCENE_STATE_LIMIT) flush=1;
    } else if(!flush &&
              (!same_scene_source(pending->source_refs[0],source->image) ||
               !same_scene_source_view(&pending->sources[0],source) ||
               memcmp(&pending->states[0],state,sizeof *state))) flush=1;
    if(flush){
      if(!gml_render_application_scene_flush(r)) return 0;
      source_index=state_index=0;
    }
  }
  if(!pending){
    pending=(GmlRenderPendingSceneBatch*)calloc(1,sizeof *pending);
    if(!pending) return 0;
    r->pending_scene_batch=pending;
  }
  /* R03-P9-IMMUTABLE-CHECKPOINT-SPAN: sampler/colour changes and distinct immutable
   * pages stay ordered, including exact filtered glyphs next to checkpoint quads.
   * A mixed span checkpoints once; an exact-only font span retains its journal.
   * CPU readers, mutable sources and capacities retain their existing boundaries. */
  next=gml_render_application_scene_next_image_ref(
    r,gml_render_scene_target_resource_ensure(r));
  if(next.content_version==UINT64_MAX ||
     operation->target.image.content_version!=next.content_version+1u ||
     operation->target.image.resource_class!=next.resource_class ||
     operation->target.image.identity!=next.identity ||
     operation->target.image.incarnation!=next.incarnation) return 0;
  if(!pending->count){
    pending->states[0]=*state;
    pending->state_count=1u;
    pending->sources[0]=*source;
    pending->source_refs[0]=source->image;
    pending->source_count=1u;
    pending->coalesce_immutable=coalescible;
  } else if(coalescible){
    if(state_index==pending->state_count)
      pending->states[pending->state_count++]=*state;
    if(source_index==pending->source_count){
      pending->sources[pending->source_count]=*source;
      pending->source_refs[pending->source_count++]=source->image;
    }
  }
  pending->operations[pending->count]=*operation;
  pending->operations[pending->count].sources=&pending->source_refs[source_index];
  pending->operations[pending->count].state_index=state_index;
  pending->count++;
  if(r->fb==r->app_surface_owned){
    r->app_surface_opaque=0;
    r->app_presentation_coverage_active=0;
  }
  /* A runtime font may grow or relocate its atlas on the next glyph lookup.
   * Pin/upload this mutable version before the caller can mutate those bytes. */
  /* Surface writes, target changes, resize and free flush this version before mutation. */
  return source->borrowed_immutable || versioned_surface ||
    gml_render_application_scene_flush(r);
}

GmlRenderResource *gml_render_surface_resource_ensure(GmlRender *r,int id){
  int slot=surface_slot(id);
  GmlSurface *surface;
  GmlRenderImageRef image;
  uint64_t incarnation;
  if(!r || slot<0) return NULL;
  surface=&r->surface[slot];
  if(!surface->live || !surface->px || surface->w<=0 || surface->h<=0) return NULL;
  if(surface->resource) return surface->resource;
  incarnation=gml_render_scene_incarnation_allocate();
  if(!incarnation) return NULL;
  image=(GmlRenderImageRef){GML_RENDER_RESOURCE_SURFACE,(uint32_t)id,
    incarnation,1u};
  surface->resource=gml_render_resource_create(image,(uint32_t)surface->w,
    (uint32_t)surface->h,GML_RENDER_PIXEL_ARGB8888,surface->px,
    (GmlRenderResourceLimits){0});
  gml_render_resource_bind_dispatch(surface->resource,r);
  return surface->resource;
}

void gml_render_surface_resource_dispose(GmlRender *r,int id){
  int slot=surface_slot(id);
  GmlRenderResource *resource;
  const GmlRenderDeviceServices *device;
  if(!r || slot<0 || !(resource=r->surface[slot].resource)) return;
  device=gml_render_device(r);
  if(device && device->retire)
    device->retire(device->context,gml_render_resource_image_ref(resource));
  gml_render_resource_destroy(resource);
  r->surface[slot].resource=NULL;
}

static GmlRenderResourceExecution surface_scene_submit(
  GmlRender *r,int id,const GmlRenderDrawBatch *batch,
  const GmlRenderResourceSource *sources,uint32_t source_count,int deferred_shader){
  GmlRenderResource *resource=gml_render_surface_resource_ensure(r,id);
  GmlRenderResourceExecution result;
  int slot=surface_slot(id);
  if(!resource) return GML_RENDER_RESOURCE_EXEC_FAILED;
  result=deferred_shader
    ?gml_render_resource_submit_deferred(resource,gml_render_device(r),batch,
                                                sources,source_count)
    :gml_render_resource_submit(resource,gml_render_device(r),batch,sources,source_count);
  gml_render_dispatch_batch_record(r,batch,result);
  if(result!=GML_RENDER_RESOURCE_EXEC_FAILED){
    r->surface[slot].dirty=1;
    r->surface[slot].opaque_known=0;
    r->surface[slot].all_opaque=0;
    r->surface[slot].all_transparent=0;
  }
  return result;
}
GmlRenderResourceExecution gml_render_surface_scene_submit(
  GmlRender *r,int id,const GmlRenderDrawBatch *batch,
  const GmlRenderResourceSource *sources,uint32_t source_count){
  return surface_scene_submit(r,id,batch,sources,source_count,0);
}
GmlRenderResourceExecution gml_render_surface_scene_submit_deferred_shader(
  GmlRender *r,int id,const GmlRenderDrawBatch *batch,
  const GmlRenderResourceSource *sources,uint32_t source_count){
  return surface_scene_submit(r,id,batch,sources,source_count,1);
}
GmlRenderResource *gml_render_application_resource_ensure(GmlRender *r){
  GmlRenderImageRef image;
  uint64_t incarnation;
  if(!r || !r->app_surface_owned || r->app_surface!=r->app_surface_owned ||
     r->app_w<=0 || r->app_h<=0) return NULL;
  if(r->app_surface_resource)
    return r->app_surface_resource_width==r->app_w &&
           r->app_surface_resource_height==r->app_h?r->app_surface_resource:NULL;
  incarnation=gml_render_scene_incarnation_allocate();
  if(!incarnation) return NULL;
  image=(GmlRenderImageRef){GML_RENDER_RESOURCE_APPLICATION,0u,
    incarnation,1u};
  r->app_surface_resource=gml_render_resource_create(image,(uint32_t)r->app_w,
    (uint32_t)r->app_h,GML_RENDER_PIXEL_ARGB8888,r->app_surface_owned,
    (GmlRenderResourceLimits){0});
  if(r->app_surface_resource){
    gml_render_resource_bind_dispatch(r->app_surface_resource,r);
    r->app_surface_resource_width=r->app_w;
    r->app_surface_resource_height=r->app_h;
  }
  return r->app_surface_resource;
}
int gml_render_scene_target_matches(const GmlRender *r){
  if(!r || !r->fb || r->fbw<=0 || r->fbh<=0) return 0;
  int slot=surface_slot(r->target_id);
  if(slot>=0) return r->target_sp>0 && r->surface[slot].live &&
    r->fb==r->surface[slot].px && r->fbw==r->surface[slot].w &&
    r->fbh==r->surface[slot].h;
  return (r->target_id==-1 || r->target_id==0) &&
    ((r->fb==r->app_surface_owned && r->fbw==r->app_w && r->fbh==r->app_h &&
      r->app_surface==r->app_surface_owned) ||
     (r->target_id==-1 && !r->target_sp && r->fb==r->base_scene_resource_pixels &&
      r->fbw==r->base_scene_resource_width &&
      r->fbh==r->base_scene_resource_height));
}
void gml_render_base_resource_dispose(GmlRender *r){
  GmlRenderResource *resource;
  const GmlRenderDeviceServices *device;
  if(!r || !(resource=r->base_scene_resource)) return;
  (void)gml_render_application_scene_flush(r);
  device=gml_render_device(r);
  if(device && device->retire)
    device->retire(device->context,gml_render_resource_image_ref(resource));
  gml_render_resource_destroy(resource);
  r->base_scene_resource=NULL;
  r->base_scene_resource_pixels=NULL;
  r->base_scene_resource_width=r->base_scene_resource_height=0;
}
GmlRenderResource *gml_render_scene_target_resource_ensure(GmlRender *r){
  GmlRenderImageRef image;
  uint64_t incarnation;
  if(!gml_render_scene_target_matches(r)) return NULL;
  if(r->target_id>0) return gml_render_surface_resource_ensure(r,r->target_id);
  if(r->fb==r->app_surface_owned && r->fbw==r->app_w && r->fbh==r->app_h)
    return gml_render_application_resource_ensure(r);
  if(r->fb!=r->base_scene_resource_pixels ||
     r->fbw!=r->base_scene_resource_width ||
     r->fbh!=r->base_scene_resource_height) return NULL;
  if(r->base_scene_resource) return r->base_scene_resource;
  incarnation=gml_render_scene_incarnation_allocate();
  if(!incarnation) return NULL;
  image=(GmlRenderImageRef){GML_RENDER_RESOURCE_SCREEN,0u,incarnation,1u};
  r->base_scene_resource=gml_render_resource_create(image,(uint32_t)r->fbw,
    (uint32_t)r->fbh,GML_RENDER_PIXEL_ARGB8888,r->fb,(GmlRenderResourceLimits){0});
  gml_render_resource_bind_dispatch(r->base_scene_resource,r);
  return r->base_scene_resource;
}
void gml_render_application_resource_dispose(GmlRender *r){
  GmlRenderResource *resource;
  const GmlRenderDeviceServices *device;
  if(!r || !(resource=r->app_surface_resource)) return;
  (void)gml_render_application_scene_flush(r);
  gml_render_application_scene_queue_dispose(r);
  device=gml_render_device(r);
  if(device && device->retire)
    device->retire(device->context,gml_render_resource_image_ref(resource));
  gml_render_resource_destroy(resource);
  r->app_surface_resource=NULL;
  r->app_surface_resource_width=r->app_surface_resource_height=0;
}
GmlRenderResourceExecution gml_render_application_scene_submit(
  GmlRender *r,const GmlRenderDrawBatch *batch,
  const GmlRenderResourceSource *sources,uint32_t source_count){
  GmlRenderResource *resource=r && r->resident_application_pass &&
    gml_render_scene_target_matches(r)
    ?gml_render_scene_target_resource_ensure(r)
    :gml_render_application_resource_ensure(r);
  GmlRenderResourceExecution result;
  if(!resource || !gml_render_application_scene_flush(r))
    return GML_RENDER_RESOURCE_EXEC_FAILED;
  result=gml_render_resource_submit_deferred(resource,gml_render_device(r),batch,sources,source_count);
  gml_render_dispatch_batch_record(r,batch,result);
  if(result!=GML_RENDER_RESOURCE_EXEC_FAILED && r->fb==r->app_surface_owned){
    r->app_surface_opaque=0;
    r->app_presentation_coverage_active=0;
  }
  return result;
}
int gml_render_compose_resident_underlay(GmlRender *r){
  GmlRenderResourceSource source={0};
  GmlRenderDrawOp operation={0};
  GmlRenderDrawStateSnapshot state={0};
  GmlRenderResource *target;
  GmlRenderImageRef image,source_ref;
  if(!r || !r->pending_underlay || !r->resident_application_pass || !r->win ||
     r->classic || r->fb!=r->base_fb || r->fb==r->app_surface_owned ||
     !gml_render_scene_target_matches(r) || !gml_render_device(r) ||
     r->app_surface!=r->app_surface_owned ||
     r->app_presentation_coverage_active || r->pending_presentation ||
     r->rotated_batch_count || r->rotated_batch_building ||
     r->target_sp || r->target_id>=0 || r->active_shader>=0 || r->blendmode ||
     !r->alphablend || r->blend_equation!=1 || r->blend_equation_alpha!=1 ||
     r->alpha_test_enable || r->color_write_mask!=15u ||
     r->classic_phase_y || !isfinite(r->cam_x) || !isfinite(r->cam_y) ||
     !(anygm_policy_uses_first_generation_studio(r->win) ||
       anygm_policy_has_modern_layer_semantics(r->win))) return 0;
  for(unsigned index=0;index<3u;index++)
    if(r->classic_interp_phase[index] || r->app_interp_phase[index]) return 0;
  GmlRenderQuad *q=&operation.payload.quad;
  q->shape=GML_RENDER_QUAD_AXIS_ALIGNED;
  q->x[0]=q->x[3]=floor(r->underlay_x-r->cam_x);
  q->y[0]=q->y[1]=floor(r->underlay_y-r->cam_y);
  q->x[1]=q->x[2]=q->x[0]+r->underlay_w;
  q->y[2]=q->y[3]=q->y[0]+r->underlay_h;
  q->sampling.filter=r->interp?GML_RENDER_FILTER_BOX:GML_RENDER_FILTER_NEAREST;
  q->sampling.edge_rule=GML_RENDER_EDGE_CLAMP_SUBIMAGE;
  q->sampling.source_region=(GmlRenderRect){0,0,(uint32_t)r->app_w,(uint32_t)r->app_h};
  int first_generation=anygm_policy_uses_first_generation_studio(r->win);
  q->sampling.quantisation=first_generation?GML_RENDER_QUANTISE_AUTOMATIC_SURFACE_FIRSTGEN:
                                             GML_RENDER_QUANTISE_AUTOMATIC_SURFACE_MODERN;
  q->source_opaque=first_generation || r->app_surface_opaque;
  for(unsigned corner=0;corner<4u;corner++){
    q->corner_colour[corner]=0xFFFFFFFFu;
    q->u[corner]=corner==0u || corner==3u?0.0:1.0;
    q->v[corner]=corner<2u?0.0:1.0;
  }
  if(!gml_render_automatic_surface_quad_valid(q,(uint32_t)r->fbw,(uint32_t)r->fbh) ||
     !gml_render_application_scene_flush(r))
    return 0;
  const GmlRenderDeviceServices *device=gml_render_device(r);
  /* A CPU-only application pass may not have needed a resource yet. Retain the
   * established witnessed 1:1 CPU-source policy before registering its storage. */
  if(!r->app_surface_resource){
    if(r->underlay_w!=r->app_w || r->underlay_h!=r->app_h ||
       !device->observe_on_demand || !device->observe_on_demand(device->context) ||
       !gml_render_application_resource_ensure(r)) return 0;
  }
  if(!gml_render_resource_device_source_view(r->app_surface_resource,device,&source)){
    /* A mixed application pass can finish on CPU while SCREEN remains resident. Upload its
     * committed 1:1 source without observing SCREEN. Scaled CPU sources, legacy recovery
     * and unavailable ownership keep their existing CPU-source exclusion. */
    if(r->underlay_w!=r->app_w || r->underlay_h!=r->app_h ||
       !device->observe_on_demand || !device->observe_on_demand(device->context) ||
       !gml_render_resource_current_cpu_source_view(r->app_surface_resource,&source)) return 0;
  }
  gml_render_flush_pending_fill(r);
  if(r->pending_fill || !r->resident_application_pass ||
     !(target=gml_render_scene_target_resource_ensure(r))) return 0;
  image=gml_render_resource_image_ref(target);
  if(image.content_version==UINT64_MAX) return 0;
  image.content_version++;
  source_ref=source.image;
  operation.opcode=GML_RENDER_OP_TEXTURED_QUAD;
  operation.scene_family=GML_RENDER_SCENE_SURFACE;
  operation.reconstruction_mode=GML_RENDER_REPLAY_EXACT;
  operation.target=(GmlRenderTargetRef){.image=image,.storage_version=1u,
    .width=(uint32_t)r->fbw,.height=(uint32_t)r->fbh,
    .pixel_format=GML_RENDER_PIXEL_ARGB8888,
    .region={0,0,(uint32_t)r->fbw,(uint32_t)r->fbh}};
  operation.sources=&source_ref; operation.source_count=1u;
  state.write_mask=15u; state.colour=0xFFFFFFFFu; state.alpha=255u;
  state.rgb_source_factor=GML_RENDER_BLEND_SOURCE_ALPHA;
  state.rgb_destination_factor=GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA;
  state.alpha_source_factor=GML_RENDER_BLEND_ONE;
  state.alpha_destination_factor=GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA;
  state.rgb_equation=state.alpha_equation=GML_RENDER_BLEND_EXACT_AUTOMATIC_SURFACE;
  state.sampler_count=1u; state.samplers[0]=q->sampling;
  GmlRenderDrawBatch batch={&operation,1u,&state,1u};
  GmlRenderResourceExecution result=gml_render_application_scene_submit(r,&batch,&source,1u);
  if(result==GML_RENDER_RESOURCE_EXEC_FAILED) return 0;
  r->pending_underlay=0;
  if(result==GML_RENDER_RESOURCE_EXEC_CPU) r->resident_application_pass=0;
  r->fb_opaque_known=r->fb_all_opaque=r->fb_all_transparent=0;
  return 1;
}

int surface_known_opaque(GmlRender *r, int id){
  if(!r) return 0;
  if(id==0) return r->app_surface_opaque;
  int i=surface_slot(id);
  return i>=0 && r->surface[i].live && r->surface[i].opaque_known && r->surface[i].all_opaque;
}
int surface_known_transparent(GmlRender *r, int id){
  if(!r || id==0) return 0;
  int i=surface_slot(id);
  return i>=0 && r->surface[i].live && r->surface[i].all_transparent;
}
static int surface_alpha_all_zero(const uint32_t *px, size_t n){
  if(!px) return 0;
  for(size_t i=0;i<n;i++) if(px[i]>>24) return 0;
  return 1;
}
static void surface_store_target_coverage(GmlRender *r){
  if(!r) return;
  int i=surface_slot(r->target_id);
  if(i>=0 && r->surface[i].live){
    GmlRenderJournalMetrics metrics={0};
    GmlRenderResource *resource=r->surface[i].resource;
    if(resource) gml_render_resource_journal_metrics(resource,&metrics);
    /* No-op draws and readbacks can conservatively dirty this bit; verify cleared targets
     * at target close so later composites can skip surfaces with no covered pixels. */
    if((!resource || metrics.cpu_version==metrics.logical_version) &&
       !r->fb_all_transparent && r->fb_opaque_known && !r->fb_all_opaque &&
       r->fb && r->fbw>0 && r->fbh>0 &&
       surface_alpha_all_zero(r->fb,(size_t)r->fbw*(size_t)r->fbh)){
      r->fb_all_transparent=1;
    }
    r->surface[i].opaque_known=r->fb_opaque_known;
    r->surface[i].all_opaque=r->fb_all_opaque;
    r->surface[i].all_transparent=r->fb_all_transparent;
  }
}

/* Metadata queries do not observe raster bytes or realize suspended fills. Preserve
 * the unavailable-resource fence, but leave valid device authority and journals intact. */
static int surface_dimensions(const GmlRender *r,int id,int *width,int *height){
  const GmlRenderResource *resource=NULL;
  GmlRenderJournalMetrics metrics={0};
  int w=0,h=0;
  if(!r) return 0;
  if(id==GML_RENDER_SHADED_SURFACE){
    if((!r->shaded_plane && !r->shaded_plane_borrowed) ||
       r->shaded_plane_width<=0 || r->shaded_plane_height<=0) return 0;
    w=r->shaded_plane_width; h=r->shaded_plane_height;
  } else if(id==0){
    if(!r->app_surface) return 0;
    if(r->app_surface==r->app_surface_owned && r->app_surface_resource){
      if(r->app_surface_resource_width!=r->app_w ||
         r->app_surface_resource_height!=r->app_h) return 0;
      resource=r->app_surface_resource;
    } else if(r->app_surface==r->base_scene_resource_pixels){
      resource=r->base_scene_resource;
    }
    w=r->app_w?r->app_w:(r->base_fbw?r->base_fbw:r->fbw);
    h=r->app_h?r->app_h:(r->base_fbh?r->base_fbh:r->fbh);
  } else {
    int slot=surface_slot(id);
    if(slot<0 || !r->surface[slot].live || !r->surface[slot].px) return 0;
    w=r->surface[slot].w; h=r->surface[slot].h;
    resource=r->surface[slot].resource;
  }
  if(resource){
    gml_render_resource_journal_metrics(resource,&metrics);
    if(metrics.authority==GML_RENDER_RESOURCE_INVALID) return 0;
  }
  if(width) *width=w;
  if(height) *height=h;
  return 1;
}

int gml_surface_exists(GmlRender *r, int id){
  if(id==0 && (!r || !r->app_surface_published)) return 0;
  int w=0,h=0; return surface_dimensions(r,id,&w,&h) && w>0 && h>0;
}
void gml_render_application_surface_publish(GmlRender *r){
  if(r) r->app_surface_published=1;
}
int gml_surface_width(GmlRender *r, int id){
  int w=0,h=0; return surface_dimensions(r,id,&w,&h)?w:0;
}
int gml_surface_height(GmlRender *r, int id){
  int w=0,h=0; return surface_dimensions(r,id,&w,&h)?h:0;
}

/* A screen-stage compositor can choose either logical application-surface coordinates or explicit
 * window-pixel coordinates. The latter is observable when surface 0 covers exactly the current
 * target extent. Those dimensions are already physical: applying the logical-to-window GUI
 * transform again magnifies the image by the presentation scale and clips the trailing region. */
int gml_render_surface_draw_targets_screen_raster(const GmlRender *r,int surf,
                                                   double width,double height){
  return r && surf==0 && gml_render_gui_transform_active(r) &&
         r->target_sp==0 && r->fbw>0 && r->fbh>0 &&
         gml_render_gui_logical_width(r)<=(double)r->fbw+0.001 &&
         gml_render_gui_logical_height(r)<=(double)r->fbh+0.001 &&
         fabs(fabs(width)-(double)r->fbw)<0.001 &&
         fabs(fabs(height)-(double)r->fbh)<0.001;
}

static int resident_surface_part(GmlRender *r,int surf,double sx,double sy,double sw,double sh,
                                 double dx,double dy,double xs,double ys,
                                 uint32_t blend,double alpha);
static int resident_surface_shader(GmlRender *r,int surf,double dx,double dy,double dw,double dh,
                                   uint32_t blend,double alpha,
                                   const GmlRenderShaderGeometry *geometry);
static int resident_surface_filtered(GmlRender *r,int surf,double dx,double dy,double dw,double dh,
                                     uint32_t blend,double alpha);

int gml_render_recover_empty_screen(GmlRender *r,uint32_t *pixels,int width,int height,
                                     double x,double y,double w,double h,int zero_camera){
  const GmlRenderDeviceServices *device;
  GmlRenderDeviceCaps caps={0};
  GmlRenderResource *application,*target;
  GmlRenderResourceSource source={0};
  GmlRenderDrawOp op={0};
  GmlRenderDrawStateSnapshot state={0};
  GmlRenderImageRef ref;
  double left,top;
  /* Plain Studio full-source magnification uses leading-edge point samples here.
   * Full-width recovery is admitted only when the source and screen are identical rasters;
   * the CPU aspect correction cannot change that rectangle. Other geometry retains its owner. */
  if(!r || r->classic || !r->resident_application_pass ||
     !gml_render_scene_target_matches(r) || r->fb!=pixels || r->fbw!=width ||
     r->fbh!=height || r->target_sp || r->target_id!=-1 ||
     !r->app_surface_published || r->app_surface!=r->app_surface_owned ||
     r->app_surface==pixels || r->app_w<=0 || r->app_h<=0 ||
     r->pending_fill || r->pending_underlay || r->pending_presentation ||
     r->active_shader>=0 || gml_render_gui_transform_active(r) ||
     !r->alphablend || r->blendmode || r->alpha_test_enable ||
     r->color_write_mask!=15u || r->blend_equation!=1 || r->blend_equation_alpha!=1 ||
     gml_d3_is_active(r) || !isfinite(x) || !isfinite(y) ||
     !isfinite(w) || !isfinite(h) || w<r->app_w || h<r->app_h ||
     w>16384.0 || h>16384.0 ||
     !(device=gml_render_device(r)) || !device->observe_on_demand ||
     !device->observe_on_demand(device->context) || !device->capabilities ||
     !device->capabilities(device->context,&caps) ||
     !(caps.supported_operations&(UINT64_C(1)<<GML_RENDER_OP_RECOVER_EMPTY_SCREEN))) return 0;
  gml_render_draw_map_point(r,&x,&y);
  gml_render_draw_map_scale(r,&w,&h);
  if(!isfinite(x) || !isfinite(y) || !isfinite(w) || !isfinite(h) ||
     w<r->app_w || h<r->app_h || w>16384.0 || h>16384.0) return 0;
  if(r->aspect_fullwidth &&
     (!zero_camera || r->world_transform_active || x!=0.0 || y!=0.0 ||
      w!=(double)width || h!=(double)height || r->app_w!=width || r->app_h!=height ||
      r->aspect_wide_w!=width || r->aspect_wide_h!=height)) return 0;
  left=floor(x-(zero_camera?0.0:r->cam_x));
  top=floor(y-(zero_camera?0.0:r->cam_y));
  w=(double)lround(w);h=(double)lround(h);
  if(!isfinite(left) || !isfinite(top) || left< -16384.0 || top< -16384.0 ||
     left>=width || top>=height || left+w<=0.0 || top+h<=0.0) return 0;
  application=gml_render_application_resource_ensure(r);
  target=gml_render_scene_target_resource_ensure(r);
  if(!application || !target || !gml_render_application_scene_flush(r) ||
     (!gml_render_resource_device_source_view(application,device,&source) &&
      !gml_render_resource_current_cpu_source_view(application,&source)) ||
     source.pixel_format!=GML_RENDER_PIXEL_ARGB8888) return 0;
  ref=source.image;
  op.opcode=GML_RENDER_OP_RECOVER_EMPTY_SCREEN;
  op.scene_family=GML_RENDER_SCENE_SURFACE;
  op.reconstruction_mode=GML_RENDER_DEVICE_CHECKPOINT;
  op.sources=&ref;op.source_count=1u;
  op.target.image=gml_render_application_scene_next_image_ref(r,target);
  if(op.target.image.content_version==UINT64_MAX) return 0;
  op.target.image.content_version++;
  op.target.storage_version=1u;
  op.target.width=(uint32_t)width;op.target.height=(uint32_t)height;
  op.target.pixel_format=GML_RENDER_PIXEL_ARGB8888;
  op.target.region=(GmlRenderRect){0,0,(uint32_t)width,(uint32_t)height};
  GmlRenderQuad *quad=&op.payload.quad;
  quad->shape=GML_RENDER_QUAD_AXIS_ALIGNED;
  quad->sampling.filter=GML_RENDER_FILTER_NEAREST;
  quad->sampling.edge_rule=GML_RENDER_EDGE_CLAMP_SUBIMAGE;
  quad->sampling.source_region=(GmlRenderRect){0,0,source.width,source.height};
  quad->filtered_draw_alpha=1.0;
  quad->x[0]=quad->x[3]=left;quad->x[1]=quad->x[2]=left+w;
  quad->y[0]=quad->y[1]=top;quad->y[2]=quad->y[3]=top+h;
  quad->u[1]=quad->u[2]=quad->v[2]=quad->v[3]=1.0;
  for(unsigned corner=0;corner<4u;corner++) quad->corner_colour[corner]=0xFFFFFFFFu;
  state.rgb_source_factor=GML_RENDER_BLEND_SOURCE_ALPHA;
  state.rgb_destination_factor=GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA;
  state.alpha_source_factor=GML_RENDER_BLEND_ONE;
  state.alpha_destination_factor=GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA;
  state.rgb_equation=state.alpha_equation=GML_RENDER_BLEND_ADD;
  state.coverage_policy=GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA;
  state.write_mask=15u;state.alpha=255u;state.colour=0xFFFFFFFFu;
  state.sampler_count=1u;state.samplers[0]=quad->sampling;
  GmlRenderDrawBatch batch={&op,1u,&state,1u};
  if(gml_render_application_scene_submit(r,&batch,&source,1u)!=
     GML_RENDER_RESOURCE_EXEC_DEVICE) return 0;
  r->fb_opaque_known=r->fb_all_opaque=r->fb_all_transparent=0;
  return 1;
}

void gml_draw_surface_stretched(GmlRender *r,int surf,double dx,double dy,
                                double dw,double dh,uint32_t blend,double alpha){
  int resident_success=0;
  int source_slot=surface_slot(surf);
  if(r && ((source_slot>=0 && r->surface[source_slot].live) ||
           (surf==0 && r->app_surface_published && r->app_surface)) &&
     !(source_slot>=0 && surf==r->surface_canvas) &&
     r->resident_scene_pass && gml_render_device(r)){
    int source_width=surf==0?r->app_w:r->surface[source_slot].w;
    int source_height=surf==0?r->app_h:r->surface[source_slot].h;
    if(source_width>0 && source_height>0){
      double mapped_x=dx,mapped_y=dy,mapped_width=dw,mapped_height=dh;
      int mapped=!gml_render_surface_draw_targets_screen_raster(r,surf,dw,dh);
      if(mapped){
        gml_render_draw_map_point(r,&mapped_x,&mapped_y);
        gml_render_draw_map_scale(r,&mapped_width,&mapped_height);
      }
      GmlRenderShaderGeometry geometry=gml_render_shader_geometry(r,mapped_x,mapped_y,
        mapped_width,mapped_height,mapped);
      resident_success=resident_surface_shader(r,surf,mapped_x,mapped_y,
        mapped_width,mapped_height,blend,alpha,&geometry);
      if(!resident_success)
        resident_success=resident_surface_filtered(r,surf,mapped_x,mapped_y,
          mapped_width,mapped_height,blend,alpha);
      if(!resident_success && surf>0)
        resident_success=resident_surface_part(r,surf,0.0,0.0,
          (double)source_width,(double)source_height,mapped_x,mapped_y,
          mapped_width/source_width,mapped_height/source_height,blend,alpha);
    }
  }
  if(!resident_success && !gml_render_resident_application_cpu_barrier(r)) return;
  /* A surface drawn onto the base canvas, rather than into another surface, is content compositing
   * its own screen. A bare refresh means only presentation, and a newly created transparent scratch
   * surface writes no pixels. An authored surface can still count after a transparent clear. */
  int slot=surface_slot(surf);
  if(r && r->target_id<0 && alpha>0.0 && slot>=0 &&
     (!surface_known_transparent(r,surf) ||
      (r->content_authored_surfaces&(UINT64_C(1)<<slot))))
    r->content_composited_screen=1;
  if(resident_success) return;
  gml_render_cpu_draw_surface_stretched_impl(r,surf,dx,dy,dw,dh,blend,alpha,1);
}

/* Full-source 1:1 and filtered magnified surfaces use the CPU rectangle's coverage,
 * hardware sampling and fixed RGB blending. An alpha-only batch preserves written alpha. */
static int resident_surface_filtered(GmlRender *r,int surf,double dx,double dy,double dw,double dh,
                                     uint32_t blend,double alpha){
  int slot=surface_slot(surf);
  int source_width,source_height,unit,point_presentation;
  double left,top,width,height;
  GmlRenderResource *resource,*target;
  GmlRenderResourceSource source={0};
  GmlRenderDrawOp operation={0};
  GmlRenderDrawStateSnapshot state={0};
  GmlRenderImageRef source_ref,image;
  const GmlRenderDeviceServices *device;
  int destination_blend=r && gml_render_blend_uses_destination_colour(r->blendmode);
  if(!r || (surf!=0 && (slot<0 || !r->surface[slot].live)) || r->classic ||
     (surf==0 && (!r->app_surface_published || !r->app_surface ||
                  r->app_surface!=r->app_surface_owned)) ||
     !r->resident_scene_pass || !gml_render_scene_target_matches(r) ||
     !(device=gml_render_device(r)) || !device->observe_on_demand ||
     !device->observe_on_demand(device->context) ||
     (surf==0?r->app_surface:r->surface[slot].px)==r->fb ||
     (r->surface_canvas>0 && (surf==r->surface_canvas || r->target_id==r->surface_canvas)) ||
     r->pending_underlay || r->pending_presentation ||
     r->active_shader>=0 || !r->alphablend || (r->blendmode && !destination_blend) ||
     r->alpha_test_enable || r->color_write_mask!=15u ||
     (destination_blend?(r->blend_equation<1 || r->blend_equation>5 ||
                         r->blend_equation_alpha<1 || r->blend_equation_alpha>5):
                        (r->blend_equation!=1 || r->blend_equation_alpha!=1)) || gml_d3_is_active(r) ||
     !isfinite(alpha) || alpha<=0.0 || !isfinite(dx) || !isfinite(dy) ||
     !isfinite(dw) || !isfinite(dh) || dw<=0.0 || dh<=0.0 ||
     dw>(double)INT_MAX/2 || dh>(double)INT_MAX/2) return 0;
  left=floor(dx-r->cam_x);top=floor(dy-r->cam_y);
  width=(double)lround(dw);height=(double)lround(dh);
  source_width=surf==0?r->app_w:r->surface[slot].w;
  source_height=surf==0?r->app_h:r->surface[slot].h;
  /* The preserved first-generation GUI leaf selects pixel-centre point samples
   * for opaque, white content-owned application presentation. Other ordinary
   * surface requests keep their floor/lround geometry and filter selection. */
  point_presentation=surf==0 && r->win &&
    anygm_policy_uses_first_generation_studio(r->win) &&
    gml_render_gui_transform_active(r) && !r->target_sp && r->target_id<0 &&
    !r->interp && !destination_blend && alpha>=1.0 &&
    (blend&0xFFFFFFu)==0xFFFFFFu && surface_known_opaque(r,0) &&
    (!r->app_draw_enable ||
     (fabs(dw-(double)source_width*r->gui_scale_x)<=.001 &&
      fabs(dh-(double)source_height*r->gui_scale_y)<=.001 &&
      (fabs(dw-lround(dw))>=.001 || fabs(dh-lround(dh))>=.001)));
  /* A full-width correction still belongs to its existing CPU geometry owner. */
  if(surf==0 && r->aspect_fullwidth && !r->target_sp &&
     source_width==r->aspect_wide_w && source_height==r->aspect_wide_h &&
     dh>=(double)r->fbh*.75 && dw/dh+.01<(double)source_width/source_height)
    return 0;
  if(point_presentation){
    if(dw>16384.0 || dh>16384.0 || fabs(dx-r->cam_x)>16384.0 ||
       fabs(dy-r->cam_y)>16384.0) return 0;
    double right=ceil(dx-r->cam_x+dw-.5),bottom=ceil(dy-r->cam_y+dh-.5);
    left=fmax(0.0,ceil(dx-r->cam_x-.5));
    top=fmax(0.0,ceil(dy-r->cam_y-.5));
    width=fmin((double)r->fbw,right)-left;
    height=fmin((double)r->fbh,bottom)-top;
    if(width<=0.0 || height<=0.0) return 1;
  }
  unit=width==source_width && height==source_height;
  /* Bound exact integer phase products and filtered tap accumulation. The CPU
   * destination-factor leaf uses left-edge point/box sampling, even when enlarged. */
  if(destination_blend && (alpha<0x1p-32 || width<1.0 || height<1.0 ||
     width>4096.0 || height>4096.0 || source_width>4096 || source_height>4096 ||
     (r->interp && ((double)source_width>width*8.0 ||
                    (double)source_height>height*8.0)))) return 0;
  if(!isfinite(left) || !isfinite(top) || left<(double)INT_MIN/2 ||
     top<(double)INT_MIN/2 || left>(double)INT_MAX/2 || top>(double)INT_MAX/2 ||
     source_width<=0 || source_height<=0 ||
     (!point_presentation && !destination_blend && !unit &&
      (!r->interp || width<=source_width || height<=source_height ||
       (surf==0 && alpha>=1.0 && (blend&0xFFFFFFu)==0xFFFFFFu)))) return 0;
  /* The CPU rectangle is half-open after these same origin/extent roundings.
   * A fully clipped destination-factor draw cannot sample or write a pixel;
   * preserve the queued prefix without acquiring its source or target. */
  if(left>=r->fbw || top>=r->fbh || left+width<=0.0 || top+height<=0.0)
    return destination_blend?1:0;
  /* A CPU suffix still permits a genuinely empty rectangle above, but never
   * resumes GPU writing for a visible draw or observes its source prematurely. */
  if(!r->resident_application_pass) return 0;
  for(int index=0;index<r->target_sp;index++)
    if(r->target_stack[index].fb==(surf==0?r->app_surface:r->surface[slot].px) &&
       r->target_stack[index].pending_fill) return 0;
  resource=surf==0?gml_render_application_resource_ensure(r):
                   gml_render_surface_resource_ensure(r,surf);
  if(!resource ||
     (!gml_render_resource_device_source_view(resource,device,&source) &&
      !gml_render_resource_current_cpu_source_view(resource,&source)) ||
     source.pixel_format!=GML_RENDER_PIXEL_ARGB8888) return 0;
  gml_render_flush_pending_fill(r);
  if(r->pending_fill || !r->resident_application_pass ||
     !(target=gml_render_scene_target_resource_ensure(r))) return 0;
  image=gml_render_application_scene_next_image_ref(r,target);
  if(image.content_version==UINT64_MAX) return 0;
  if(alpha>1.0) alpha=1.0;
  source_ref=source.image;
  operation.opcode=GML_RENDER_OP_TEXTURED_QUAD;
  operation.scene_family=GML_RENDER_SCENE_SURFACE;
  operation.reconstruction_mode=GML_RENDER_DEVICE_CHECKPOINT;
  operation.sources=&source_ref;operation.source_count=1u;
  operation.target.image=image;operation.target.image.content_version++;
  operation.target.storage_version=1u;
  operation.target.width=(uint32_t)r->fbw;operation.target.height=(uint32_t)r->fbh;
  operation.target.pixel_format=GML_RENDER_PIXEL_ARGB8888;
  operation.target.region=(GmlRenderRect){0,0,(uint32_t)r->fbw,(uint32_t)r->fbh};
  GmlRenderQuad *quad=&operation.payload.quad;
  quad->shape=GML_RENDER_QUAD_AXIS_ALIGNED;
  quad->sampling.filter=point_presentation?GML_RENDER_FILTER_HARDWARE_PIXEL_CENTRE_POINT:destination_blend
    ?(!unit && r->interp?GML_RENDER_FILTER_BOX:GML_RENDER_FILTER_NEAREST)
    :unit?GML_RENDER_FILTER_NEAREST:GML_RENDER_FILTER_HARDWARE_BILINEAR;
  quad->sampling.edge_rule=GML_RENDER_EDGE_CLAMP_SUBIMAGE;
  quad->sampling.source_region=(GmlRenderRect){0,0,source.width,source.height};
  quad->filtered_draw_alpha=alpha;
  quad->x[0]=quad->x[3]=left;quad->x[1]=quad->x[2]=left+width;
  quad->y[0]=quad->y[1]=top;quad->y[2]=quad->y[3]=top+height;
  quad->u[1]=quad->u[2]=quad->v[2]=quad->v[3]=1.0;
  if(point_presentation){
    quad->source_opaque=1u;
    quad->inverse_anchor[0]=dx-r->cam_x;quad->inverse_anchor[1]=dy-r->cam_y;
    quad->inverse_x[0]=dw;quad->inverse_y[1]=dh;
    for(unsigned corner=0;corner<4u;corner++){
      quad->u[corner]=(quad->x[corner]-quad->inverse_anchor[0])/dw;
      quad->v[corner]=(quad->y[corner]-quad->inverse_anchor[1])/dh;
    }
  }
  uint32_t colour=((uint32_t)lround(alpha*255.0)<<24)|
    ((blend&255u)<<16)|(blend&0xFF00u)|((blend>>16)&255u);
  for(int corner=0;corner<4;corner++) quad->corner_colour[corner]=colour;
  state.rgb_source_factor=GML_RENDER_BLEND_SOURCE_ALPHA;
  state.rgb_destination_factor=GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA;
  state.alpha_source_factor=GML_RENDER_BLEND_ONE;
  state.alpha_destination_factor=GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA;
  state.rgb_equation=state.alpha_equation=GML_RENDER_BLEND_ADD;
  state.coverage_policy=GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA;
  if(destination_blend){
    state.rgb_source_factor=state.alpha_source_factor=
      r->blendmode==GML_RENDER_BLEND_DEST_ALPHA_DEST_COLOUR
        ?GML_RENDER_BLEND_DESTINATION_ALPHA:GML_RENDER_BLEND_DESTINATION_COLOUR;
    state.rgb_destination_factor=state.alpha_destination_factor=
      r->blendmode==GML_RENDER_BLEND_DEST_SOURCE_COLOUR
        ?GML_RENDER_BLEND_SOURCE_COLOUR:GML_RENDER_BLEND_DESTINATION_COLOUR;
    state.rgb_equation=GML_RENDER_BLEND_EXACT_DESTINATION_ADD+(uint32_t)r->blend_equation-1u;
    state.alpha_equation=GML_RENDER_BLEND_EXACT_DESTINATION_ADD+(uint32_t)r->blend_equation_alpha-1u;
    state.coverage_policy=gml_render_target_preserves_alpha(r)?0u:
      GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA;
  }
  state.write_mask=15u;state.alpha=255u;state.colour=0xFFFFFFFFu;
  state.sampler_count=1u;state.samplers[0]=quad->sampling;
  if(!gml_render_application_scene_enqueue(r,&operation,&state,&source)) return 0;
  r->fb_opaque_known=r->fb_all_opaque=r->fb_all_transparent=0;
  return 1;
}

/* A contained integer content-program rectangle uses the terminal plan's compiler/draw state.
 * A numbered destination retains its device result until a CPU observer or the frame boundary;
 * a later distinct surface draw can consume that result on the same device. */
static int resident_surface_shader(GmlRender *r,int surf,double dx,double dy,double dw,double dh,
                                   uint32_t blend,double alpha,
                                   const GmlRenderShaderGeometry *geometry){
  enum { MAX_AUXILIARY_SNAPSHOT_BYTES=64u*1024u*1024u };
  int slot=surface_slot(surf),target_id,target_slot,application_target,transient_source=0;
  double left,top;
  GmlRenderResource *source_resource,*target;
  GmlRenderResourceSource sources[1u+GML_PLAN_MAX_SAMPLERS]={0};
  GmlRenderDrawOp operation={0};
  GmlRenderDrawStateSnapshot state={0};
  GmlRenderImageRef source_refs[1u+GML_PLAN_MAX_SAMPLERS],image;
  GmlRenderDrawBatch batch;
  GmlRenderResourceExecution result;
  GmlPlanShader shader;
  GmlRenderShaderSampler bindings[GML_PLAN_MAX_SAMPLERS];
  GmlRenderDeviceCaps caps={0};
  uint32_t *owned_sampler_pixels[GML_PLAN_MAX_SAMPLERS]={0};
  const GmlRenderDeviceServices *device;
  uint32_t prepared=0;
  size_t snapshot_bytes=0;
  static const float white[4]={1.0f,1.0f,1.0f,1.0f};
  if(!r || (surf!=0 && (slot<0 || !r->surface[slot].live)) ||
     (surf==0 && (!r->app_surface_published || !r->app_surface ||
                  r->app_surface==r->fb)) ||
     !r->resident_application_pass ||
     !gml_render_device(r) || !r->shader_device_present || r->active_shader<0 ||
     !gml_render_shader_content_candidate(r,r->active_shader) ||
     r->pending_underlay || r->pending_presentation ||
     r->blendmode || r->alpha_test_enable || r->color_write_mask!=0x0Fu ||
     r->blend_equation!=1 || r->blend_equation_alpha!=1 || gml_d3_is_active(r) ||
     (blend&0xFFFFFFu)!=0xFFFFFFu || !isfinite(alpha) || alpha<1.0 ||
     !isfinite(dx) || !isfinite(dy) || !isfinite(dw) || !isfinite(dh) ||
     (surf!=0 && r->surface[slot].px==r->fb)) return 0;
  left=dx-r->cam_x;
  top=dy-r->cam_y;
  if(!isfinite(left) || !isfinite(top) || left<0.0 || top<0.0 ||
     dw<=0.0 || dh<=0.0 || left+dw>(double)r->fbw || top+dh>(double)r->fbh ||
     floor(left)!=left || floor(top)!=top || floor(dw)!=dw || floor(dh)!=dh)
    return 0;
  target_id=r->target_id;
  application_target=target_id==-1;
  target_slot=surface_slot(target_id);
  if(application_target){
    if(!gml_render_scene_target_matches(r)) return 0;
  } else if(target_slot<0 || target_id==surf || target_id==r->surface_canvas ||
            !r->surface[target_slot].live || r->target_sp<=0 ||
            r->fb!=r->surface[target_slot].px ||
            r->fbw!=r->surface[target_slot].w ||
            r->fbh!=r->surface[target_slot].h) return 0;
  if(!gml_render_shader_plan_snapshot(r,r->active_shader,0u,white,&shader)) return 0;
  if(!geometry || !geometry->valid) return 0;
  shader.geometry=*geometry;
  source_resource=surf==0?gml_render_application_resource_ensure(r):
    gml_render_surface_resource_ensure(r,surf);
  if(source_resource && gml_render_resource_device_source_view(
       source_resource,gml_render_device(r),&sources[0])){
    if(sources[0].pixel_format!=GML_RENDER_PIXEL_ARGB8888) return 0;
  } else if(surf==0){
    const uint32_t *pixels;
    int width=0,height=0;
    size_t bytes;
    pixels=gml_surface_pixels_read(r,0,&width,&height);
    if(!pixels || width<=0 || height<=0 ||
       (size_t)width>SIZE_MAX/4u/(size_t)height ||
       (uint32_t)width>UINT32_MAX/4u ||
       (bytes=(size_t)width*(size_t)height*4u)>MAX_AUXILIARY_SNAPSHOT_BYTES)
      return 0;
    sources[0].image=(GmlRenderImageRef){GML_RENDER_RESOURCE_SHADER_SOURCE,0u,
      gml_render_scene_incarnation_allocate(),1u};
    if(!sources[0].image.incarnation) return 0;
    sources[0].width=(uint32_t)width;
    sources[0].height=(uint32_t)height;
    sources[0].pixel_format=GML_RENDER_PIXEL_ARGB8888;
    sources[0].stride_bytes=(uint32_t)width*4u;
    sources[0].byte_capacity=bytes;
    sources[0].bytes=pixels;
    transient_source=1;
  } else {
    if(!source_resource ||
       !gml_render_resource_source_view(source_resource,&sources[0]) ||
       sources[0].pixel_format!=GML_RENDER_PIXEL_ARGB8888) return 0;
  }
  device=gml_render_device(r);
  source_refs[0]=sources[0].image;
  if(shader.sampler_count){
    if(!device->capabilities || !device->capabilities(device->context,&caps) ||
       !caps.max_texture_extent) return 0;
    if(gml_render_shader_samplers(r,r->active_shader,bindings,GML_PLAN_MAX_SAMPLERS)!=
       shader.sampler_count) return 0;
    for(uint32_t index=0;index<shader.sampler_count;index++){
      const uint32_t *pixels=NULL;
      GmlRenderResourceSource *view=&sources[index+1u];
      GmlRenderShaderSampler *binding=&bindings[index];
      int width=0,height=0;
      size_t bytes;
      if(binding->surface>=0){
        GmlRenderResource *bound=gml_render_surface_resource_ensure(r,binding->surface);
        GmlRenderJournalMetrics metrics={0};
        if(!bound || binding->surface==target_id) goto sampler_fallback;
        gml_render_resource_journal_metrics(bound,&metrics);
        /* Auxiliary preparation retains its immutable CPU snapshot. A stale CPU
         * version is an ordered fallback, not permission to observe device pixels. */
        if(metrics.cpu_version!=metrics.logical_version ||
           metrics.authority==GML_RENDER_RESOURCE_INVALID ||
           !gml_render_surface_plane(r,binding->surface,&width,&height,&pixels))
          goto sampler_fallback;
      } else if(binding->sprite>=0){
        if(!gml_render_sprite_frame_plane(r,binding->sprite,binding->frame,
                                          &width,&height,&pixels)) goto sampler_fallback;
      } else goto sampler_fallback;
      if(!pixels || width<=0 || height<=0 ||
         (uint32_t)width>caps.max_texture_extent ||
         (uint32_t)height>caps.max_texture_extent ||
         (size_t)width>SIZE_MAX/4u/(size_t)height ||
         (uint32_t)width>UINT32_MAX/4u) goto sampler_fallback;
      bytes=(size_t)width*(size_t)height*4u;
      if(bytes>MAX_AUXILIARY_SNAPSHOT_BYTES-snapshot_bytes) goto sampler_fallback;
      owned_sampler_pixels[index]=(uint32_t*)malloc(bytes);
      if(!owned_sampler_pixels[index]) goto sampler_fallback;
      memcpy(owned_sampler_pixels[index],pixels,bytes);
      snapshot_bytes+=bytes;
      view->image=(GmlRenderImageRef){GML_RENDER_RESOURCE_SHADER_SAMPLER,
        index+1u,gml_render_scene_incarnation_allocate(),1u};
      if(!view->image.incarnation) goto sampler_fallback;
      view->width=(uint32_t)width;
      view->height=(uint32_t)height;
      view->pixel_format=GML_RENDER_PIXEL_ARGB8888;
      view->stride_bytes=(uint32_t)width*4u;
      view->byte_capacity=bytes;
      view->bytes=owned_sampler_pixels[index];
      source_refs[index+1u]=view->image;
      shader.samplers[index].image=index+1u;
      prepared++;
    }
  }
  if(!gml_render_application_scene_flush(r)) goto sampler_fallback;
  gml_render_flush_pending_fill(r);
  if(r->pending_fill || !r->resident_application_pass ||
     !(target=application_target?gml_render_scene_target_resource_ensure(r):
       gml_render_surface_resource_ensure(r,target_id))) goto sampler_fallback;
  image=application_target?gml_render_application_scene_next_image_ref(r,target):
    gml_render_resource_image_ref(target);
  if(image.content_version==UINT64_MAX) goto sampler_fallback;
  operation.opcode=GML_RENDER_OP_CONTENT_SHADER_DRAW;
  operation.scene_family=GML_RENDER_SCENE_SURFACE;
  operation.reconstruction_mode=GML_RENDER_DEVICE_CHECKPOINT;
  operation.source_count=1u+shader.sampler_count;
  operation.sources=source_refs;
  operation.target.image=image;
  operation.target.image.content_version++;
  operation.target.storage_version=1u;
  operation.target.width=(uint32_t)r->fbw;
  operation.target.height=(uint32_t)r->fbh;
  operation.target.pixel_format=GML_RENDER_PIXEL_ARGB8888;
  operation.target.region=(GmlRenderRect){(int32_t)left,(int32_t)top,
    (uint32_t)dw,(uint32_t)dh};
  operation.payload.shader=&shader;
  state.rgb_source_factor=GML_RENDER_BLEND_SOURCE_ALPHA;
  state.rgb_destination_factor=GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA;
  state.alpha_source_factor=GML_RENDER_BLEND_ONE;
  state.alpha_destination_factor=GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA;
  state.rgb_equation=GML_RENDER_BLEND_ADD;
  state.alpha_equation=GML_RENDER_BLEND_ADD;
  state.write_mask=15u;
  state.colour=0xFFFFFFFFu;
  state.alpha=255u;
  state.sampler_count=1u;
  state.samplers[0].filter=r->interp?GML_RENDER_FILTER_BILINEAR:GML_RENDER_FILTER_NEAREST;
  state.samplers[0].edge_rule=GML_RENDER_EDGE_CLAMP_SUBIMAGE;
  state.samplers[0].source_region=(GmlRenderRect){0,0,sources[0].width,sources[0].height};
  batch=(GmlRenderDrawBatch){&operation,1u,&state,1u};
  result=application_target?gml_render_application_scene_submit(r,&batch,sources,
      operation.source_count):
    gml_render_surface_scene_submit_deferred_shader(r,target_id,&batch,sources,
      operation.source_count);
  if(transient_source && device->retire) device->retire(device->context,source_refs[0]);
  for(uint32_t index=0;index<prepared;index++)
    if(device->retire) device->retire(device->context,source_refs[index+1u]);
  for(uint32_t index=0;index<shader.sampler_count;index++) free(owned_sampler_pixels[index]);
  if(result!=GML_RENDER_RESOURCE_EXEC_DEVICE){
    const GmlRenderDeviceServices *device=gml_render_device(r);
    if(device && device->content_program_failed &&
       device->content_program_failed(device->context,&shader))
      gml_render_shader_mark_failed(r,r->active_shader);
    return 0;
  }
  r->fb_opaque_known=0;
  r->fb_all_opaque=0;
  r->fb_all_transparent=0;
  r->shaded_draws++;
  return 1;
sampler_fallback:
  for(uint32_t index=0;index<shader.sampler_count;index++) free(owned_sampler_pixels[index]);
  return 0;
}

/* Opaque and binary-alpha integer surface crops share the exact nearest texel rule with
 * full-source stretched draws. Observe the source without manufacturing a CPU write;
 * the resource journal retains its version for CPU readers and context loss. */
static int resident_surface_part(GmlRender *r,int surf,double sx,double sy,double sw,double sh,
                                 double dx,double dy,double xs,double ys,
                                 uint32_t blend,double alpha){
  GmlRenderResource *source_resource,*target;
  GmlRenderResourceSource source={0};
  GmlRenderDrawOp operation={0};
  GmlRenderDrawStateSnapshot state={0};
  GmlRenderImageRef source_ref,image;
  GmlRenderDrawBatch batch;
  GmlRenderResourceExecution result;
  const uint32_t *pixels;
  double left,right,top,bottom;
  int slot=surface_slot(surf),width=0,height=0,binary_alpha=0,covered=0;
  if(!r || slot<0 || !r->resident_application_pass || !gml_render_device(r) ||
     !gml_render_scene_target_matches(r) ||
     r->target_sp || r->target_id>=0 || r->pending_underlay || r->pending_presentation ||
     r->active_shader>=0 || r->interp || r->blendmode || r->alpha_test_enable ||
     r->color_write_mask!=0x0Fu || r->blend_equation!=1 ||
     r->blend_equation_alpha!=1 || gml_d3_is_active(r) ||
     (blend&0xFFFFFFu)!=0xFFFFFFu || !isfinite(alpha) || alpha<1.0 ||
     !isfinite(sx) || !isfinite(sy) || !isfinite(sw) || !isfinite(sh) ||
     !isfinite(dx) || !isfinite(dy) || !isfinite(xs) || !isfinite(ys) ||
     sx<0.0 || sy<0.0 || sw<=0.0 || sh<=0.0 || xs<1.0 || ys<1.0 ||
     floor(sx)!=sx || floor(sy)!=sy || floor(sw)!=sw || floor(sh)!=sh ||
     floor(xs)!=xs || floor(ys)!=ys ||
     r->surface[slot].px==r->fb) return 0;
  left=dx-r->cam_x;
  top=dy-r->cam_y;
  right=left+sw*xs;
  bottom=top+sh*ys;
  if(!isfinite(left) || !isfinite(top) || !isfinite(right) || !isfinite(bottom) ||
     left<0.0 || top<0.0 || right>(double)r->fbw || bottom>(double)r->fbh ||
     floor(left)!=left || floor(top)!=top || floor(right)!=right ||
     floor(bottom)!=bottom || right<=left || bottom<=top) return 0;
  pixels=gml_surface_pixels_read(r,surf,&width,&height);
  if(!pixels || width<=0 || height<=0 || sx+sw>(double)width ||
     sy+sh>(double)height ||
     !(source_resource=gml_render_surface_resource_ensure(r,surf)) ||
     !gml_render_resource_source_view(source_resource,&source) ||
     source.bytes!=pixels || source.pixel_format!=GML_RENDER_PIXEL_ARGB8888)
    return 0;
  for(int y=(int)sy;y<(int)(sy+sh);y++)
    for(int x=(int)sx;x<(int)(sx+sw);x++){
      unsigned coverage=pixels[(size_t)y*width+x]>>24;
      if(coverage==255u) covered=1;
      else if(coverage==0u) binary_alpha=1;
      else return 0;
    }
  if(!covered) return 0;
  gml_render_flush_pending_fill(r);
  if(r->pending_fill || !r->resident_application_pass ||
     !(target=gml_render_scene_target_resource_ensure(r))) return 0;
  image=gml_render_application_scene_next_image_ref(r,target);
  if(image.content_version==UINT64_MAX) return 0;
  source_ref=source.image;
  operation.opcode=GML_RENDER_OP_TEXTURED_QUAD;
  operation.scene_family=GML_RENDER_SCENE_SURFACE;
  operation.reconstruction_mode=GML_RENDER_REPLAY_EXACT;
  operation.source_count=1u;
  operation.sources=&source_ref;
  operation.target.image=image;
  operation.target.image.content_version++;
  operation.target.storage_version=1u;
  operation.target.width=(uint32_t)r->fbw;
  operation.target.height=(uint32_t)r->fbh;
  operation.target.pixel_format=GML_RENDER_PIXEL_ARGB8888;
  operation.target.region=(GmlRenderRect){0,0,(uint32_t)r->fbw,(uint32_t)r->fbh};
  operation.payload.quad.shape=GML_RENDER_QUAD_AXIS_ALIGNED;
  operation.payload.quad.sampling.filter=GML_RENDER_FILTER_NEAREST;
  operation.payload.quad.sampling.edge_rule=GML_RENDER_EDGE_CLAMP_SUBIMAGE;
  operation.payload.quad.sampling.source_region=(GmlRenderRect){(int)sx,(int)sy,
    (uint32_t)sw,(uint32_t)sh};
  for(int corner=0;corner<4;corner++) operation.payload.quad.corner_colour[corner]=0xFFFFFFFFu;
  operation.payload.quad.x[0]=operation.payload.quad.x[3]=left;
  operation.payload.quad.x[1]=operation.payload.quad.x[2]=right;
  operation.payload.quad.y[0]=operation.payload.quad.y[1]=top;
  operation.payload.quad.y[2]=operation.payload.quad.y[3]=bottom;
  operation.payload.quad.u[0]=operation.payload.quad.u[3]=sx/width;
  operation.payload.quad.u[1]=operation.payload.quad.u[2]=(sx+sw)/width;
  operation.payload.quad.v[0]=operation.payload.quad.v[1]=sy/height;
  operation.payload.quad.v[2]=operation.payload.quad.v[3]=(sy+sh)/height;
  state.write_mask=15u;
  state.colour=0xFFFFFFFFu;
  state.alpha=255u;
  if(binary_alpha){
    state.rgb_source_factor=GML_RENDER_BLEND_SOURCE_ALPHA;
    state.rgb_destination_factor=GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA;
    state.alpha_source_factor=GML_RENDER_BLEND_ONE;
    state.alpha_destination_factor=GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA;
    state.rgb_equation=GML_RENDER_BLEND_ADD;
    state.alpha_equation=GML_RENDER_BLEND_ADD;
  }
  state.sampler_count=1u;
  state.samplers[0]=operation.payload.quad.sampling;
  batch=(GmlRenderDrawBatch){&operation,1u,&state,1u};
  result=gml_render_application_scene_submit(r,&batch,&source,1u);
  if(result==GML_RENDER_RESOURCE_EXEC_FAILED) return 0;
  if(result==GML_RENDER_RESOURCE_EXEC_CPU) r->resident_application_pass=0;
  r->fb_all_transparent=0;
  r->fb_opaque_known=0;
  r->fb_all_opaque=0;
  return 1;
}

void gml_draw_surface_part_ext(GmlRender *r,int surf,double sx,double sy,double sw,double sh,
                               double dx,double dy,double xs,double ys,uint32_t blend,double alpha){
  int explicit_target_raster=gml_render_surface_draw_targets_screen_raster(r,surf,sw*xs,sh*ys);
  if(!explicit_target_raster){
    gml_render_draw_map_point(r,&dx,&dy);
    gml_render_draw_map_scale(r,&xs,&ys);
  }
  GmlRenderShaderGeometry geometry=gml_render_shader_geometry(r,dx,dy,sw*xs,sh*ys,
                                                               !explicit_target_raster);
  /* R03-P7-CONTENT-SURFACE: a part call selecting the whole source and covering
   * the whole destination is the same content-program operation as stretched.
   * Preserve the part call's own coordinate mapping, then share its program
   * snapshot, resource checkpoint and ordered fallback with that rule. */
  if(r){
    int slot=surface_slot(surf);
    int source_width=surf==0?r->app_w:
      slot>=0 && r->surface[slot].live?r->surface[slot].w:0;
    int source_height=surf==0?r->app_h:
      slot>=0 && r->surface[slot].live?r->surface[slot].h:0;
    if(sx==0.0 && sy==0.0 && sw==(double)source_width &&
       sh==(double)source_height && source_width>0 && source_height>0 &&
       resident_surface_shader(r,surf,dx,dy,sw*xs,sh*ys,blend,alpha,&geometry)) return;
  }
  if(resident_surface_part(r,surf,sx,sy,sw,sh,dx,dy,xs,ys,blend,alpha)) return;
  if(!gml_render_resident_application_cpu_barrier(r)) return;
  gml_render_cpu_draw_surface_part_mapped(r,surf,sx,sy,sw,sh,dx,dy,xs,ys,blend,alpha,&geometry);
}
void gml_draw_surface_ext(GmlRender *r,int surf,double x,double y,
                          double xs,double ys,double rot,uint32_t blend,double alpha){
  double rr=fmod(rot,360.0); if(rr<0) rr+=360.0;
  if(fabs(rr)<0.001 || fabs(rr-360.0)<0.001){
    int sw=gml_surface_width(r,surf),sh=gml_surface_height(r,surf);
    if(sw>0&&sh>0) gml_draw_surface_stretched(r,surf,x,y,sw*xs,sh*ys,blend,alpha);
    return;
  }
  if(!gml_render_resident_application_cpu_barrier(r)) return;
  gml_render_draw_map_point(r,&x,&y);
  gml_render_draw_map_scale(r,&xs,&ys);
  gml_render_cpu_draw_surface_ext_mapped(r,surf,x,y,xs,ys,rr,blend,alpha);
}
const uint32_t *gml_surface_pixels_read(GmlRender *r, int id, int *w, int *h){
  if(!r) return NULL;
  if(id==r->target_id) gml_render_maybe_prepare_draw(r);
  return surface_pixels_observe(r,id,w,h);
}

const uint32_t *gml_render_screen_pixels_read(GmlRender *r,int *w,int *h){
  return screen_pixels_observe(r,w,h);
}

int gml_surface_create(GmlRender *r, int w, int h){
  if(w<=0||h<=0||w>4096||h>4096) return -1;
  for(int k=0;k<GML_MAX_SURFACES;k++){
    int i=(r->next_surface_id+k-1)%GML_MAX_SURFACES;
    if(!r->surface[i].live){
      uint32_t *px=calloc((size_t)w*h,sizeof(uint32_t));
      if(!px) return -1;
      { uint8_t *rle=r->surface[i].rle; size_t cap=r->surface[i].rle_cap;   /* keep cache alloc across reuse */
        r->surface[i]=(GmlSurface){px,w,h,1,1,1,0,1,rle,0,cap,NULL}; }
      r->next_surface_id=i+2; if(r->next_surface_id>GML_MAX_SURFACES) r->next_surface_id=1;
      return i+1;
    }
  }
  return -1;
}
void gml_surface_free(GmlRender *r, int id){
  int i=surface_slot(id);
  if(!r || i<0 || !r->surface[i].live) return;
  if(r->pending_presentation && r->presentation.shader>=0)
    gml_render_maybe_prepare_draw(r);
  if(!gml_render_application_scene_flush(r)) return;
  uint32_t *old=r->surface[i].px;
  /* Close each active binding while its storage is still valid. The same surface can be
   * nested more than once; a single reset would restore another borrow of the old buffer. */
  while(r->target_id==id && r->target_sp>0){
    int depth=r->target_sp;
    gml_surface_reset_target(r);
    if(r->target_sp>=depth) return;
  }
  /* A suspended binding is a saved target, not an owned copy. Remove every snapshot of
   * this allocation so a later reset cannot revive it after the ID is reused. */
  int retained=0;
  for(int stack=0;stack<r->target_sp;stack++)
    if(r->target_stack[stack].fb!=old)
      r->target_stack[retained++]=r->target_stack[stack];
  r->target_sp=retained;
  if(r->base_fb==old){ r->base_fb=NULL; r->base_fbw=0; r->base_fbh=0; }
  if(r->app_surface==old){
    r->app_surface=NULL;
    r->app_w=r->app_h=0;
    r->app_surface_opaque=0;
    r->app_presentation_coverage_active=0;
  }
  if(r->fb==old){
    r->fb=r->base_fb;
    r->fbw=r->base_fbw;
    r->fbh=r->base_fbh;
    r->target_id=-1;
    r->pending_fill=0;
    r->pending_underlay=0;
    r->fb_opaque_known=r->fb_all_opaque=r->fb_all_transparent=0;
  }
  gml_render_surface_resource_dispose(r,id);
  free(r->surface[i].px); free(r->surface[i].rle); memset(&r->surface[i],0,sizeof(r->surface[i]));
}
static int application_surface_resize(GmlRender *r,int w,int h){
  if(!r || w<=0 || h<=0 || w>4096 || h>4096 ||
     (size_t)w>SIZE_MAX/(size_t)h || (size_t)w*(size_t)h>SIZE_MAX/sizeof(uint32_t))
    return 0;
  if(!gml_render_application_scene_flush(r)) return 0;
  if(r->app_surface_owned && r->app_w==w && r->app_h==h &&
     (!r->app_surface_resource ||
      (r->app_surface_resource_width==w && r->app_surface_resource_height==h))) return 1;
  uint32_t *old=r->app_surface;
  int oldw=r->app_w, oldh=r->app_h;
  if(old==r->app_surface_owned && r->app_surface_resource){
    oldw=r->app_surface_resource_width;
    oldh=r->app_surface_resource_height;
    if(!gml_render_resource_acquire_cpu_read(r->app_surface_resource,NULL)) return 0;
  } else if(old==r->base_scene_resource_pixels && r->base_scene_resource){
    if(!gml_render_resource_acquire_cpu_read(r->base_scene_resource,NULL)) return 0;
  }
  uint32_t *px=calloc((size_t)w*(size_t)h,sizeof(uint32_t));
  if(!px) return 0;
  int cw=oldw<w?oldw:w, ch=oldh<h?oldh:h;
  if(old && cw>0 && ch>0)
    for(int y=0;y<ch;y++) memcpy(px+(size_t)y*w,old+(size_t)y*oldw,(size_t)cw*sizeof(uint32_t));
  uint32_t *owned=r->app_surface_owned;
  /* A game can resize surface 0 while it is the current/nested target. Keep every borrowed
   * target reference coherent before releasing the previous owned allocation. */
  if(r->fb==old){ r->fb=px; r->fbw=w; r->fbh=h; }
  if(r->base_fb==old){ r->base_fb=px; r->base_fbw=w; r->base_fbh=h; }
  for(int i=0;i<r->target_sp;i++) if(r->target_stack[i].fb==old){
    r->target_stack[i].fb=px; r->target_stack[i].w=w; r->target_stack[i].h=h;
  }
  r->app_surface_owned=px;
  r->app_surface=px; r->app_w=w; r->app_h=h;
  r->app_surface_opaque=0;
  r->app_presentation_coverage_active=0;
  gml_render_application_resource_dispose(r);
  free(owned);
  return 1;
}
int gml_render_application_surface_ensure_owned(GmlRender *r,int w,int h){
  return application_surface_resize(r,w,h);
}
void gml_surface_resize(GmlRender *r, int id, int w, int h){
  if(!r || w<=0 || h<=0 || w>4096 || h>4096) return;
  if(r->pending_presentation && r->presentation.shader>=0 &&
     (id==0 || (id>0 && id<=GML_MAX_SURFACES && r->surface[id-1].live)))
    gml_render_maybe_prepare_draw(r);
  if(id==0){
    /* Surface zero follows the same explicit resize request in every generation. */
    if(!r->win) return;
    if(render_setting(r,"GML_LOG_SURF")) anygm_host_logf(r && r->win ? r->win->host : NULL,ANYGM_LOG_DEBUG,"[surf] resize application_surface %dx%d (was %dx%d)\n",w,h,r->app_w,r->app_h);
    (void)application_surface_resize(r,w,h);
    return;
  }
  int i=surface_slot(id); if(i<0 || !r->surface[i].live || w<=0 || h<=0 || w>4096 || h>4096) return;
  if(!gml_render_application_scene_flush(r)) return;
  uint32_t *px=calloc((size_t)w*h,sizeof(uint32_t));
  if(!px) return;
  int had_pending_fill=r->target_id==id && r->pending_fill;
  for(int stack=0;stack<r->target_sp;stack++)
    if(r->target_stack[stack].target_id==id && r->target_stack[stack].pending_fill)
      had_pending_fill=1;
  if(r->target_id==id && r->pending_fill){
    /* A deferred CPU clear must first invalidate a device-current target. Otherwise the
     * subsequent read could replay the older GPU version over the newly materialized fill. */
    if(surface_pixels(r,id,NULL,NULL)!=r->surface[i].px){ free(px); return; }
    gml_render_flush_pending_fill(r);
  }
  const uint32_t *old=surface_pixels_observe(r,id,NULL,NULL);
  if((r->target_id==id && r->pending_fill) || old!=r->surface[i].px){
    free(px);
    return;
  }
  int was_transparent=had_pending_fill?0:r->surface[i].all_transparent;
  int cw=w<r->surface[i].w?w:r->surface[i].w, ch=h<r->surface[i].h?h:r->surface[i].h;
  for(int y=0;y<ch;y++) memcpy(px+(size_t)y*w,old+(size_t)y*r->surface[i].w,(size_t)cw*sizeof(uint32_t));
  /* The target stack borrows the numbered allocation just as it borrows surface zero. */
  if(r->fb==old){ r->fb=px; r->fbw=w; r->fbh=h; }
  if(r->base_fb==old){ r->base_fb=px; r->base_fbw=w; r->base_fbh=h; }
  for(int stack=0;stack<r->target_sp;stack++) if(r->target_stack[stack].fb==old){
    r->target_stack[stack].fb=px;
    r->target_stack[stack].w=w;
    r->target_stack[stack].h=h;
  }
  gml_render_surface_resource_dispose(r,id);
  free(r->surface[i].px); r->surface[i].px=px; r->surface[i].w=w; r->surface[i].h=h;
  r->surface[i].dirty=1;
  r->surface[i].opaque_known=0;
  r->surface[i].all_opaque=0;
  r->surface[i].all_transparent=was_transparent;
}
int gml_surface_set_target(GmlRender *r, int id){
  if(!r || r->target_sp>=GML_SURFACE_STACK) return 0;
  /* A presentation is a base-target operation, never a child-target stack entry. Finish it
   * before changing target identity or permitting writes to its borrowed samplers. */
  if(r->pending_presentation && r->presentation.shader>=0)
    gml_render_maybe_prepare_draw(r);
  if(!gml_render_application_scene_flush(r)) return 0;
  int w=0,h=0,si=surface_slot(id);
  uint32_t *px=NULL;
  int resident=r->resident_scene_pass && gml_render_device(r) && (!r->classic || id>0);
  if(resident){
    /* Resolve pending parent writes before another binding can alias its allocation.
     * Binding itself borrows storage metadata, not CPU-current raster bytes. */
    gml_render_flush_pending_underlay(r);
    gml_render_flush_pending_fill(r);
    if(r->pending_underlay || r->pending_fill) return 0;
    if(si>=0 && r->surface[si].live &&
       gml_render_surface_resource_ensure(r,id)){
      px=r->surface[si].px; w=r->surface[si].w; h=r->surface[si].h;
    } else if(id==0 && r->app_surface==r->app_surface_owned &&
              gml_render_application_resource_ensure(r)){
      px=r->app_surface_owned; w=r->app_w; h=r->app_h;
    }
  }
  if(!px){ resident=0; px=surface_pixels(r,id,&w,&h); }
  if(resident && px){
    GmlRenderJournalMetrics metrics={0};
    GmlRenderResource *resource=si>=0?r->surface[si].resource:r->app_surface_resource;
    gml_render_resource_journal_metrics(resource,&metrics);
    if(metrics.authority==GML_RENDER_RESOURCE_INVALID) return 0;
  }
  if(!px || r->target_sp>=GML_SURFACE_STACK) return 0;
  /* Classic target assignment replaces the current target; one reset returns to the base. Other runtime families retain nested targets. */
  if(r->classic && r->target_id>=0){
    gml_surface_reset_target(r);
    if(resident && si>=0 && r->surface[si].live){
      px=r->surface[si].px;w=r->surface[si].w;h=r->surface[si].h;
    } else px=surface_pixels(r,id,&w,&h);
    if(!px || r->target_sp>=GML_SURFACE_STACK) return 0;
  }
  { if(si>=0) r->surface[si].dirty=1; }   /* about to be drawn into */
  r->target_stack[r->target_sp++]=(typeof(r->target_stack[0])){
    r->fb,r->fbw,r->fbh,r->cam_x,r->cam_y,r->projection_cam_x,r->projection_cam_y,
    r->target_id,r->fb_opaque_known,r->fb_all_opaque,r->fb_all_transparent,
    r->pending_underlay,r->underlay_x,r->underlay_y,r->underlay_w,r->underlay_h,
    r->pending_fill,r->pending_fill_color,r->resident_application_pass
  };
  if(render_setting(r,"GML_LOG_SURF")){
    /* Count existing RGB and alpha before this surface becomes the active target. */
    int lit_=0,cov_=0;
    surface_coverage_counts(px,w,h,&lit_,&cov_);
    anygm_host_logf(r->win?r->win->host:NULL,ANYGM_LOG_DEBUG,
      "[surf] set_target %d px=%p lit_on_entry=%d covered_on_entry=%d of %d\n",
      id,(const void *)px,lit_,cov_,w*h);
  }
  r->fb=px; r->fbw=w; r->fbh=h;
  r->target_id=id;
  r->resident_application_pass=resident;
  r->fb_opaque_known=(si>=0)?r->surface[si].opaque_known:0;
  r->fb_all_opaque=(si>=0)?r->surface[si].all_opaque:0;
  r->fb_all_transparent=(si>=0)?r->surface[si].all_transparent:0;
  r->pending_underlay=0;
  r->underlay_x=r->underlay_y=r->underlay_w=r->underlay_h=0;
  r->pending_fill=0;
  r->pending_fill_color=0;
  /* GM resets the projection to the surface's own coordinates while a surface target is
   * active: draws inside it must NOT be shifted by the room camera/GUI offset. */
  r->projection_cam_x=0; r->projection_cam_y=0;
  r->cam_x=0; r->cam_y=0;
  if(id==r->surface_canvas && r->surface_canvas_width>0 && w>=r->surface_canvas_width)
    r->projection_cam_x=r->cam_x=-((w-r->surface_canvas_width)/2);
  gml_d3_sync_render_camera(r);
  if(render_setting(r,"GML_LOG_SURF")){
    /* Count both channels again at function exit, paired with the entry counts. */
    int lit_=0,cov_=0;
    surface_coverage_counts(px,w,h,&lit_,&cov_);
    anygm_host_logf(r->win?r->win->host:NULL,ANYGM_LOG_DEBUG,
      "[surf] set_target %d lit_on_exit=%d covered_on_exit=%d of %d\n",id,lit_,cov_,w*h);
  }
  return 1;
}
void gml_surface_reset_target(GmlRender *r){
  if(!gml_render_application_scene_flush(r)) return;
  /* Count colour coverage on both sides of the pending-draw flush while the
   * active surface is still the render target. */
  int dbg_=r && render_setting(r,"GML_LOG_SURF")!=NULL && r->fb && r->fbw>0 && r->fbh>0;
  int released_=r?r->target_id:-1;   /* captured before the stack pop restores the previous one */
  int before_=0,before_cov_=0;
  if(dbg_) surface_coverage_counts(r->fb,r->fbw,r->fbh,&before_,&before_cov_);
  if(r && surface_slot(r->target_id)>=0){
    /* Remember that content really authored this surface before a deferred transparent clear is
     * resolved. A later screen blit of that now-transparent surface is still a compositor action;
     * this is distinct from blitting a scratch surface that received no draw at all. */
    if(!r->fb_all_transparent)
      r->content_authored_surfaces|=UINT64_C(1)<<(r->target_id-1);
    gml_render_flush_pending_underlay(r);
    gml_render_flush_pending_fill(r);
    surface_store_target_coverage(r);
  }
  if(dbg_){
    int after_=0,after_cov_=0;
    surface_coverage_counts(r->fb,r->fbw,r->fbh,&after_,&after_cov_);
    anygm_host_logf(r->win?r->win->host:NULL,ANYGM_LOG_DEBUG,
      "[surf] reset_target released=%d px=%p lit_before=%d lit_after=%d"
      " covered_before=%d covered_after=%d of %d\n",
      released_,(const void *)r->fb,before_,after_,before_cov_,after_cov_,r->fbw*r->fbh);
  }
  if(r->target_sp>0){
    typeof(r->target_stack[0]) t=r->target_stack[--r->target_sp];
    if(t.fb==r->fb && r->resident_scene_pass){
      t.opaque_known=r->fb_opaque_known;
      t.all_opaque=r->fb_all_opaque;
      t.all_transparent=r->fb_all_transparent;
    }
    r->fb=t.fb; r->fbw=t.w; r->fbh=t.h;
    r->projection_cam_x=t.projection_cx; r->projection_cam_y=t.projection_cy;
    r->cam_x=t.cx; r->cam_y=t.cy;
    gml_d3_sync_render_camera(r);
    r->target_id=t.target_id;
    r->fb_opaque_known=t.opaque_known;
    r->fb_all_opaque=t.all_opaque;
    r->fb_all_transparent=t.all_transparent;
    r->pending_underlay=t.pending_underlay;
    r->underlay_x=t.underlay_x;
    r->underlay_y=t.underlay_y;
    r->underlay_w=t.underlay_w;
    r->underlay_h=t.underlay_h;
    r->pending_fill=t.pending_fill;
    r->pending_fill_color=t.fill_color;
    r->resident_application_pass=t.resident_pass;
  } else if(r->base_fb){
    r->fb=r->base_fb; r->fbw=r->base_fbw; r->fbh=r->base_fbh;
    r->target_id=-1;
    r->resident_application_pass=0;
    r->fb_opaque_known=0;
    r->fb_all_opaque=0;
    r->fb_all_transparent=0;
    r->pending_underlay=0;
    r->underlay_x=r->underlay_y=r->underlay_w=r->underlay_h=0;
    r->pending_fill=0;
    r->pending_fill_color=0;
  } else {
    r->fb=NULL; r->fbw=0; r->fbh=0;
    r->target_id=-1;
    r->fb_opaque_known=0;
    r->fb_all_opaque=0;
    r->fb_all_transparent=0;
    r->pending_underlay=0;
    r->underlay_x=r->underlay_y=r->underlay_w=r->underlay_h=0;
    r->pending_fill=0;
    r->pending_fill_color=0;
  }
}
int gml_surface_get_target(GmlRender *r){
  return r ? r->target_id : -1;
}
/* Retain a bound program target across a new pass while replacing the base buffer that a later target reset returns to. With no target, use ordinary pass initialization. */
static void begin_retaining_target(GmlRender *r, uint32_t *fb, int w, int h,
                                   double cx, double cy,int resident_selection){
  if(!r || r->target_id<0 || r->target_sp<=0){ gml_render_begin(r,fb,w,h,cx,cy); return; }
  int retained=r->target_id;
  int depth=r->target_sp;
  /* The coverage of a bound target is tracked live and only written back to the surface when the
   * target closes, so it has to cross this call rather than be re-read: a surface the draw phase
   * has just filled still reads transparent in its own record, and a later composite of it would
   * be skipped as empty. */
  int retained_opaque_known=r->fb_opaque_known;
  int retained_all_opaque=r->fb_all_opaque;
  int retained_all_transparent=r->fb_all_transparent;
  typeof(r->target_stack[0]) saved[GML_SURFACE_STACK];
  memcpy(saved,r->target_stack,sizeof(saved));
  gml_render_begin(r,fb,w,h,cx,cy);
  int sw=0,sh=0;
  int si=surface_slot(retained);
  uint32_t *px=NULL;
  if(resident_selection && retained==0 && r->app_surface_resource &&
     r->app_surface==r->app_surface_owned){
    px=r->app_surface_owned;
    sw=r->app_surface_resource_width;
    sh=r->app_surface_resource_height;
  } else if(resident_selection && si>=0 && r->surface[si].live &&
            r->surface[si].resource){
    /* Retaining a target only needs its stable allocation and extent. The CPU accessor would
     * replay a resident journal and mark the GPU copy stale before this pass draws anything. */
    px=r->surface[si].px;
    sw=r->surface[si].w;
    sh=r->surface[si].h;
  } else
    px=surface_pixels(r,retained,&sw,&sh);
  if(!px) return;   /* the surface was freed under the binding: the pass owns the base buffer */
  memcpy(r->target_stack,saved,sizeof(r->target_stack));
  r->target_sp=depth;
  /* The bottom entry described the base of the pass that pushed it, which no longer exists. */
  r->target_stack[0].fb=fb;
  r->target_stack[0].w=w;
  r->target_stack[0].h=h;
  r->target_stack[0].cx=cx;
  r->target_stack[0].cy=cy;
  r->target_stack[0].projection_cx=cx;
  r->target_stack[0].projection_cy=cy;
  r->target_stack[0].target_id=-1;
  r->target_stack[0].opaque_known=0;
  r->target_stack[0].all_opaque=0;
  r->target_stack[0].all_transparent=0;
  r->target_stack[0].pending_underlay=0;
  r->target_stack[0].underlay_x=r->target_stack[0].underlay_y=0;
  r->target_stack[0].underlay_w=r->target_stack[0].underlay_h=0;
  r->target_stack[0].pending_fill=0;
  r->target_stack[0].fill_color=0;
  for(int index=0;index<depth;index++)
    if(!resident_selection) r->target_stack[index].resident_pass=0;
  r->target_stack[0].resident_pass=resident_selection;
  r->fb=px; r->fbw=sw; r->fbh=sh;
  r->target_id=retained;
  r->fb_opaque_known=retained_opaque_known;
  r->fb_all_opaque=retained_all_opaque;
  r->fb_all_transparent=retained_all_transparent;
  if(si>=0) r->surface[si].dirty=1;
}
void gml_render_begin_retaining_target(GmlRender *r, uint32_t *fb, int w, int h,
                                       double cx, double cy){
  begin_retaining_target(r,fb,w,h,cx,cy,0);
}
void gml_render_begin_retaining_resident_target(GmlRender *r,uint32_t *fb,int w,int h,
                                                double cx,double cy){
  begin_retaining_target(r,fb,w,h,cx,cy,1);
}
