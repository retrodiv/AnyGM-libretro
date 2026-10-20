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

enum { GML_SCENE_BATCH_LIMIT=2048, GML_SCENE_PAIRED_LIMIT=2*GML_SCENE_BATCH_LIMIT,
       GML_SCENE_SURFACE_RUN_LIMIT=8192,
       GML_SCENE_PREPARING_SURFACE=2,
       GML_SCENE_STATE_LIMIT=128,
       GML_SCENE_SOURCE_LIMIT=64 };
struct GmlRenderPendingSceneBatch {
  GmlRenderDrawOp *operations;
  uint32_t operation_capacity;
  GmlRenderDrawStateSnapshot states[GML_SCENE_STATE_LIMIT];
  GmlRenderResourceSource sources[GML_SCENE_SOURCE_LIMIT];
  GmlRenderImageRef source_refs[GML_SCENE_SOURCE_LIMIT];
  uint32_t count,state_count,source_count,phase_count;
  int coalesce_sources;
  int preparing;
  struct GmlRenderPendingSceneBatch *prepared_parent;
  int saved_opaque_known,saved_all_opaque,saved_all_transparent;
  int saved_application_opaque,saved_presentation_coverage;
  int saved_phase_opaque_known,saved_phase_all_opaque,saved_phase_all_transparent;
};

/* Ordinary images keep their delivery budget. An atomic paired root stages
 * both budgets;a homogeneous surface preparation owns its bounded cell run.
 * Neither preparation is submitted halfway. */
static int scene_pending_reserve(GmlRenderPendingSceneBatch *pending,uint32_t count){
  if(count<=pending->operation_capacity) return 1;
  if(count>GML_SCENE_SURFACE_RUN_LIMIT) return 0;
  uint32_t capacity=count>GML_SCENE_PAIRED_LIMIT?GML_SCENE_SURFACE_RUN_LIMIT:
    count>GML_SCENE_BATCH_LIMIT?GML_SCENE_PAIRED_LIMIT:GML_SCENE_BATCH_LIMIT;
  GmlRenderDrawOp *operations=pending->operations?
    realloc(pending->operations,(size_t)capacity*sizeof *operations):calloc(capacity,sizeof *operations);
  if(!operations) return 0;
  if(pending->operation_capacity)
    memset(operations+pending->operation_capacity,0,
      (size_t)(capacity-pending->operation_capacity)*sizeof *operations);
  pending->operations=operations;pending->operation_capacity=capacity;
  return 1;
}

static void scene_pending_free(GmlRenderPendingSceneBatch *pending){
  if(!pending) return;
  free(pending->operations);free(pending);
}

static int same_scene_source(GmlRenderImageRef a,GmlRenderImageRef b){
  return a.resource_class==b.resource_class && a.identity==b.identity &&
         a.incarnation==b.incarnation && a.content_version==b.content_version;
}

static int same_scene_image(GmlRenderImageRef a,GmlRenderImageRef b){
  return a.resource_class==b.resource_class && a.identity==b.identity &&
         a.incarnation==b.incarnation;
}

static int paired_root_targets(const GmlRender *r,GmlRenderImageRef a,
                                GmlRenderImageRef b){
  if(!r || !r->classic_phase_resource) return 0;
  GmlRenderImageRef phase=gml_render_resource_image_ref(r->classic_phase_resource);
  return (same_scene_image(a,r->classic_phase_base_image) && same_scene_image(b,phase)) ||
         (same_scene_image(b,r->classic_phase_base_image) && same_scene_image(a,phase));
}

static int same_scene_source_view(const GmlRenderResourceSource *a,
                                  const GmlRenderResourceSource *b){
  return a->bytes==b->bytes && a->byte_capacity==b->byte_capacity &&
         a->width==b->width && a->height==b->height &&
         a->pixel_format==b->pixel_format && a->stride_bytes==b->stride_bytes &&
         a->borrowed_immutable==b->borrowed_immutable &&
         a->resident_resource==b->resident_resource;
}

int gml_render_application_scene_prepare_begin(GmlRender *r){
  GmlRenderPendingSceneBatch *parent=r?r->pending_scene_batch:NULL;
  if(!r || r->classic_phase_scene_target || !r->resident_application_pass || r->pending_fill || r->pending_underlay ||
     r->pending_presentation || (parent && parent->preparing)) return 0;
  GmlRenderPendingSceneBatch *prepared=calloc(1,sizeof *prepared);
  if(!prepared) return 0;
  if(parent){
    *prepared=*parent;
    prepared->operations=NULL;prepared->operation_capacity=0u;
    if(parent->count){
      if(!scene_pending_reserve(prepared,parent->count)){scene_pending_free(prepared);return 0;}
      memcpy(prepared->operations,parent->operations,(size_t)parent->count*sizeof *prepared->operations);
    }
    for(uint32_t index=0;index<prepared->count;index++){
      GmlRenderDrawOp *operation=&prepared->operations[index];
      if(!operation->source_count) continue;
      uint32_t source=0u;
      while(source<parent->source_count && operation->sources!=&parent->source_refs[source]) source++;
      if(source==parent->source_count){scene_pending_free(prepared);return 0;}
      operation->sources=&prepared->source_refs[source];
    }
  }
  prepared->preparing=1;prepared->prepared_parent=parent;
  prepared->saved_opaque_known=r->fb_opaque_known;
  prepared->saved_all_opaque=r->fb_all_opaque;
  prepared->saved_all_transparent=r->fb_all_transparent;
  prepared->saved_application_opaque=r->app_surface_opaque;
  prepared->saved_presentation_coverage=r->app_presentation_coverage_active;
  prepared->saved_phase_opaque_known=r->classic_phase_opaque_known;
  prepared->saved_phase_all_opaque=r->classic_phase_all_opaque;
  prepared->saved_phase_all_transparent=r->classic_phase_all_transparent;
  r->pending_scene_batch=prepared;
  return 1;
}

int gml_render_application_scene_prepare_finish(GmlRender *r,int accepted){
  GmlRenderPendingSceneBatch *prepared=r?r->pending_scene_batch:NULL;
  if(!prepared || !prepared->preparing) return 0;
  if(r->classic_phase_scene_target && !gml_render_classic_phase_scene_select(r,0)) return 0;
  r->classic_phase_scene_preparing=0;
  r->classic_phase_scene_background_normalised=0;
  GmlRenderPendingSceneBatch *parent=prepared->prepared_parent;
  if(accepted){
    prepared->preparing=0;prepared->prepared_parent=NULL;
    scene_pending_free(parent);
  }else{
    r->pending_scene_batch=parent;
    r->fb_opaque_known=prepared->saved_opaque_known;
    r->fb_all_opaque=prepared->saved_all_opaque;
    r->fb_all_transparent=prepared->saved_all_transparent;
    r->app_surface_opaque=prepared->saved_application_opaque;
    r->app_presentation_coverage_active=prepared->saved_presentation_coverage;
    r->classic_phase_opaque_known=prepared->saved_phase_opaque_known;
    r->classic_phase_all_opaque=prepared->saved_phase_all_opaque;
    r->classic_phase_all_transparent=prepared->saved_phase_all_transparent;
    scene_pending_free(prepared);
  }
  return accepted!=0;
}

GmlRenderImageRef gml_render_application_scene_next_image_ref(
  GmlRender *r,GmlRenderResource *resource){
  GmlRenderImageRef image=gml_render_resource_image_ref(resource);
  uint32_t pending=0u;
  GmlRenderPendingSceneBatch *batch=r?r->pending_scene_batch:NULL;
  if(batch && batch->count){
    if(!batch->phase_count){
      if(same_scene_image(image,batch->operations[0].target.image)) pending=batch->count;
    }else if(resource==r->classic_phase_resource) pending=batch->phase_count;
    else if(same_scene_image(image,r->classic_phase_base_image))
      pending=batch->count-batch->phase_count;
  }
  if(image.content_version>UINT64_MAX-pending) image.content_version=UINT64_MAX;
  else image.content_version+=pending;
  return image;
}

static GmlRenderResource *scene_image_target(GmlRender *r,GmlRenderImageRef image){
  GmlRenderResource *target=NULL;
  if(image.resource_class==GML_RENDER_RESOURCE_SCREEN){
    target=r->base_scene_resource;
    if(!same_scene_image(gml_render_resource_image_ref(target),image))
      target=r->retained_base_resource;
  }
  else if(image.resource_class==GML_RENDER_RESOURCE_APPLICATION) target=r->app_surface_resource;
  else if(image.resource_class==GML_RENDER_RESOURCE_SURFACE &&
          image.identity>0u && image.identity<=GML_MAX_SURFACES)
    target=r->surface[image.identity-1u].resource;
  else if(r->classic_phase_resource &&
          same_scene_image(image,gml_render_resource_image_ref(r->classic_phase_resource)))
    target=r->classic_phase_resource;
  return target && same_scene_image(gml_render_resource_image_ref(target),image)?target:NULL;
}

static void scene_remove_target(GmlRenderPendingSceneBatch *pending,GmlRenderImageRef image){
  uint32_t kept=0u;
  for(uint32_t index=0;index<pending->count;index++)
    if(!same_scene_image(pending->operations[index].target.image,image))
      pending->operations[kept++]=pending->operations[index];
  pending->count=kept;
  if(image.resource_class==GML_RENDER_RESOURCE_SCRATCH) pending->phase_count=0u;
  if(!kept) pending->state_count=pending->source_count=0u;
}

int gml_render_application_scene_flush(GmlRender *r){
  GmlRenderPendingSceneBatch *pending=r?r->pending_scene_batch:NULL;
  GmlRenderDrawOp *group=NULL;
  if(!pending || !pending->count) return 1;
  if(pending->preparing || r->classic_phase_scene_target) return 0;
  GmlRenderImageRef first=pending->operations[0].target.image;
  for(uint32_t index=1u;index<pending->count;index++){
    GmlRenderImageRef image=pending->operations[index].target.image;
    if(same_scene_image(first,image)) continue;
    if(!paired_root_targets(r,first,image)) return 0;
    if(!group){
      group=malloc((size_t)pending->count*sizeof *group);
      if(!group) return 0;
    }
  }
  do {
    GmlRenderImageRef image=pending->operations[0].target.image;
    GmlRenderResource *target=scene_image_target(r,image);
    if(!target){free(group);return 0;}
    GmlRenderResourceSource sources[GML_SCENE_SOURCE_LIMIT];
    uint32_t source_count=pending->source_count,count=pending->count;
    const GmlRenderResourceSource *views=pending->sources;
    if(group){
      unsigned char used[GML_SCENE_SOURCE_LIMIT]={0};
      count=source_count=0u;
      for(uint32_t index=0;index<pending->count;index++){
        const GmlRenderDrawOp *op=&pending->operations[index];
        if(!same_scene_image(op->target.image,image)) continue;
        group[count++]=*op;
        for(uint32_t source=0;source<op->source_count;source++){
          uint32_t found=0u;
          while(found<pending->source_count &&
                !same_scene_source(op->sources[source],pending->source_refs[found])) found++;
          if(found==pending->source_count || !pending->sources[found].borrowed_immutable){
            free(group);return 0;
          }
          used[found]=1u;
        }
      }
      for(uint32_t index=0;index<pending->source_count;index++)
        if(used[index]) sources[source_count++]=pending->sources[index];
      views=sources;
    }
    GmlRenderDrawBatch batch={group?group:pending->operations,count,
                              pending->states,pending->state_count};
    GmlRenderResourceExecution result=gml_render_resource_submit_deferred(
      target,gml_render_device(r),&batch,views,source_count);
    gml_render_dispatch_batch_record(r,&batch,result);
    if(result==GML_RENDER_RESOURCE_EXEC_FAILED){
      GmlRenderJournalMetrics metrics={0};
      gml_render_resource_journal_metrics(target,&metrics);
      /* Immutable bytes permit a pre-submit retry. A mutable runtime atlas may relocate at the
       * next glyph demand, so its failed submission must not leave a borrowed pointer queued;
       * the caller will cross the CPU barrier and draw the glyph there instead. */
      int mutable_atlas=0;
      for(uint32_t index=0;index<source_count;index++)
        mutable_atlas|=!views[index].borrowed_immutable && !views[index].resident_resource;
      if(metrics.authority==GML_RENDER_RESOURCE_INVALID || mutable_atlas)
        scene_remove_target(pending,image);
      free(group);return 0;
    }
    /* Delivery is irreversible. Retire this plane's prefix before attempting
     * its peer, so a pre-write refusal cannot replay an already blended plane. */
    scene_remove_target(pending,image);
  } while(pending->count);
  free(group);
  return 1;
}

int gml_render_checkpoint_pending_resources(GmlRender *r){
  const GmlRenderDeviceServices *device=gml_render_device(r);
  if(!r) return 0;
  if(!gml_render_base_history_observe(r)) return 0;
  for(int index=0;index<GML_MAX_SURFACES;index++)
    if(r->surface[index].resource &&
       !gml_render_resource_checkpoint(r->surface[index].resource,device)) return 0;
  if(r->app_surface_resource &&
     !gml_render_resource_checkpoint(r->app_surface_resource,device)) return 0;
  if(r->retained_base_resource &&
     !gml_render_resource_checkpoint(r->retained_base_resource,device)) return 0;
  if(r->view_base_resource &&
     !gml_render_resource_checkpoint(r->view_base_resource,device)) return 0;
  if(r->classic_phase_resource &&
     !gml_render_resource_checkpoint(r->classic_phase_resource,device)) return 0;
  return !r->base_scene_resource ||
    gml_render_resource_checkpoint(r->base_scene_resource,device);
}

int gml_render_observe_device_resources(GmlRender *r){
  const GmlRenderDeviceServices *device=gml_render_device(r);
  if(!r || !device || !gml_render_application_scene_flush(r)) return 0;
  if(!gml_render_base_history_observe(r)) return 0;
  for(int index=0;index<GML_MAX_SURFACES;index++)
    if(r->surface[index].resource &&
       !gml_render_resource_observe_device(r->surface[index].resource,device)) return 0;
  if(r->app_surface_resource &&
     !gml_render_resource_observe_device(r->app_surface_resource,device)) return 0;
  if(r->retained_base_resource &&
     !gml_render_resource_observe_device(r->retained_base_resource,device)) return 0;
  if(r->view_base_resource &&
     !gml_render_resource_observe_device(r->view_base_resource,device)) return 0;
  if(r->classic_phase_resource &&
     !gml_render_resource_observe_device(r->classic_phase_resource,device)) return 0;
  return !r->base_scene_resource ||
    gml_render_resource_observe_device(r->base_scene_resource,device);
}

void gml_render_application_scene_queue_dispose(GmlRender *r){
  if(!r) return;
  r->classic_phase_scene_preparing=0;
  r->classic_phase_scene_background_normalised=0;
  if(r->classic_phase_scene_target) (void)gml_render_classic_phase_scene_select(r,0);
  if(r->pending_scene_batch && r->pending_scene_batch->preparing)
    scene_pending_free(r->pending_scene_batch->prepared_parent);
  scene_pending_free(r->pending_scene_batch);
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
         resource_loses_device_pixels(r->retained_base_resource) ||
         resource_loses_device_pixels(r->view_base_resource) ||
         resource_loses_device_pixels(r->base_scene_resource) ||
         resource_loses_device_pixels(r->classic_phase_resource) ||
         (r->retained_base_history_count>(size_t)r->retained_base_width*r->retained_base_height &&
          resource_loses_device_pixels(r->retained_base_history_resource)) ||
         (r->view_base_history_count>(size_t)r->view_base_width*r->view_base_height &&
          resource_loses_device_pixels(r->view_base_history_resource)) ||
         (r->base_history_count>(size_t)r->base_scene_resource_width*r->base_scene_resource_height &&
          resource_loses_device_pixels(r->base_history_resource));
}

void gml_render_lose_volatile_device(GmlRender *r){
  uint64_t lost_surfaces=0u;
  int lost_application,owned_width,owned_height;
  if(!r) return;
  for(int index=0;index<GML_MAX_SURFACES;index++)
    if(resource_loses_device_pixels(r->surface[index].resource))
      lost_surfaces|=UINT64_C(1)<<index;
  lost_application=resource_loses_device_pixels(r->app_surface_resource) ||
    (r->app_surface==r->base_scene_resource_pixels &&
     resource_loses_device_pixels(r->base_scene_resource)) ||
    (r->app_surface==r->retained_base_pixels &&
     resource_loses_device_pixels(r->retained_base_resource)) ||
    (r->app_surface==r->view_base_pixels &&
     resource_loses_device_pixels(r->view_base_resource));
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
  gml_render_retained_base_dispose(r);
  gml_render_view_base_dispose(r);
  gml_render_classic_phase_resource_dispose(r);
  if(r->resident_application_pass)
    gml_render_dispatch_resident_lost(r,GML_RENDER_RESIDENT_NO_DEVICE);
  r->resident_application_pass=r->resident_scene_pass=0;
  r->fb_opaque_known=r->fb_all_opaque=r->fb_all_transparent=0;
}

int gml_render_application_scene_enqueue_primitive(GmlRender *r,
                                                   const GmlRenderDrawOp *operation){
  if(!r || r->classic_phase_scene_target || !r->resident_application_pass || !gml_render_device(r) ||
     (!gml_render_particle_line_valid(operation) &&
      !gml_render_classic_alpha_rect_valid(operation) &&
      !gml_render_studio_alpha_rect_valid(operation) &&
      !gml_render_classic_inverse_rect_valid(operation) &&
      !gml_render_rectangle_valid(operation))) return 0;
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
  if(!scene_pending_reserve(pending,pending->count+1u)) return 0;
  pending->operations[pending->count]=*operation;
  pending->operations[pending->count++].sources=NULL;
  if(r->fb==r->app_surface_owned){
    r->app_surface_opaque=0;
    r->app_presentation_coverage_active=0;
  }
  return 1;
}

static int enqueue_primitives(GmlRender *r,const GmlRenderDrawOp *operations,uint32_t count,
                              const GmlRenderResourceSource *source){
  if(!r || r->classic_phase_scene_target || !r->resident_application_pass || !gml_render_device(r) ||
     !operations || !count || count>65536u || !gml_render_scene_target_matches(r)) return 0;
  GmlRenderResource *target=gml_render_scene_target_resource_ensure(r);
  if(!target) return 0;
  GmlRenderImageRef next=gml_render_application_scene_next_image_ref(r,target);
  if(next.content_version>UINT64_MAX-count) return 0;
  for(uint32_t i=0;i<count;i++){
    const GmlRenderDrawOp *op=&operations[i];
    if((source?!gml_render_textured_vertex_row_valid(op) ||
                !same_scene_source(op->sources[0],source->image):
               (!gml_render_rectangle_valid(op) && !gml_render_particle_disc_valid(op))) ||
       op->target.image.resource_class!=next.resource_class ||
       op->target.image.identity!=next.identity || op->target.image.incarnation!=next.incarnation ||
       op->target.image.content_version!=next.content_version+i+1u) return 0;
  }
  if(count>GML_SCENE_BATCH_LIMIT){
    GmlRenderDrawBatch batch={operations,count,NULL,0u};
    GmlRenderResourceExecution result=gml_render_application_scene_submit(r,&batch,source,source?1u:0u);
    if(result==GML_RENDER_RESOURCE_EXEC_CPU) r->resident_application_pass=0;
    return result!=GML_RENDER_RESOURCE_EXEC_FAILED;
  }
  GmlRenderPendingSceneBatch *pending=r->pending_scene_batch;
  if(pending && pending->count &&
     ((source?(pending->source_count!=1u ||
                !same_scene_source(pending->source_refs[0],source->image) ||
                !same_scene_source_view(&pending->sources[0],source)):pending->source_count!=0u) ||
      pending->count>GML_SCENE_BATCH_LIMIT-count ||
      pending->operations[0].target.image.resource_class!=next.resource_class ||
      pending->operations[0].target.image.identity!=next.identity ||
      pending->operations[0].target.image.incarnation!=next.incarnation) &&
     !gml_render_application_scene_flush(r)) return 0;
  if(!pending){
    pending=(GmlRenderPendingSceneBatch*)calloc(1,sizeof *pending);
    if(!pending) return 0;
    r->pending_scene_batch=pending;
  }
  if(!scene_pending_reserve(pending,pending->count+count)) return 0;
  if(source){
    pending->source_count=1u;pending->sources[0]=*source;
    pending->source_refs[0]=source->image;pending->coalesce_sources=0;
  }
  memcpy(&pending->operations[pending->count],operations,(size_t)count*sizeof *operations);
  if(source) for(uint32_t i=0;i<count;i++)
    pending->operations[pending->count+i].sources=&pending->source_refs[0];
  pending->count+=count;
  if(r->fb==r->app_surface_owned){r->app_surface_opaque=0;r->app_presentation_coverage_active=0;}
  return 1;
}

int gml_render_application_scene_enqueue_primitives(GmlRender *r,
  const GmlRenderDrawOp *operations,uint32_t count){
  return enqueue_primitives(r,operations,count,NULL);
}

int gml_render_application_scene_enqueue_textured_primitives(GmlRender *r,
  const GmlRenderDrawOp *operations,uint32_t count,const GmlRenderResourceSource *source){
  return source && enqueue_primitives(r,operations,count,source);
}

int gml_render_application_scene_enqueue(GmlRender *r,const GmlRenderDrawOp *operation,
                                          const GmlRenderDrawStateSnapshot *state,
                                          const GmlRenderResourceSource *source){
  return gml_render_application_scene_enqueue_quads(r,operation,1u,state,source);
}

int gml_render_application_scene_enqueue_quads(GmlRender *r,
  const GmlRenderDrawOp *operation,uint32_t count,
  const GmlRenderDrawStateSnapshot *state,const GmlRenderResourceSource *source){
  GmlRenderPendingSceneBatch *pending;
  GmlRenderImageRef next;
  uint32_t state_index=0,source_index=0;
  int coalescible,exact_filtered_font,versioned_surface;
  int mutable_text=operation && source &&
    operation->scene_family==GML_RENDER_SCENE_TEXT &&
    operation->reconstruction_mode==GML_RENDER_DEVICE_CHECKPOINT &&
    source->image.resource_class==GML_RENDER_RESOURCE_ATLAS && !source->resident_resource &&
    r && source->image.identity<(uint32_t)r->n_atlas &&
    r->atlas[source->image.identity].runtime_font_page && r->device.context &&
    r->device.observe_on_demand && r->device.observe_on_demand(r->device.context);
  versioned_surface=operation && source && source->resident_resource &&
    (source->image.resource_class==GML_RENDER_RESOURCE_SCREEN ||
     source->image.resource_class==GML_RENDER_RESOURCE_SURFACE ||
     source->image.resource_class==GML_RENDER_RESOURCE_APPLICATION ||
     (source->image.resource_class==GML_RENDER_RESOURCE_SCRATCH && operation &&
      operation->payload.quad.sampling.quantisation==GML_RENDER_QUANTISE_SURFACE_ROTATED_POINT)) &&
    operation->scene_family==GML_RENDER_SCENE_SURFACE &&
    operation->reconstruction_mode==GML_RENDER_DEVICE_CHECKPOINT &&
    operation->opcode==GML_RENDER_OP_TEXTURED_QUAD &&
    ((operation->payload.quad.sampling.filter==GML_RENDER_FILTER_NEAREST &&
      (operation->payload.quad.sampling.quantisation==GML_RENDER_QUANTISE_SURFACE_NEAREST_FUNCTION ||
       operation->payload.quad.sampling.quantisation==GML_RENDER_QUANTISE_SURFACE_TILED_POINT ||
       operation->payload.quad.sampling.quantisation==GML_RENDER_QUANTISE_SURFACE_ROTATED_POINT)) ||
     (operation->payload.quad.sampling.filter==GML_RENDER_FILTER_BOX &&
      operation->payload.quad.sampling.quantisation==GML_RENDER_QUANTISE_SURFACE_BOX_FUNCTION) ||
     operation->payload.quad.sampling.filter==GML_RENDER_FILTER_HARDWARE_BILINEAR ||
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
     !count || count>GML_SCENE_BATCH_LIMIT ||
     (count>1u && !source->borrowed_immutable && !mutable_text) ||
     (!source->borrowed_immutable && !versioned_surface &&
      (source->image.resource_class!=GML_RENDER_RESOURCE_ATLAS ||
       source->resident_resource)) || !operation->sources ||
     operation->source_count!=1u || operation->opcode!=GML_RENDER_OP_TEXTURED_QUAD ||
     (operation->reconstruction_mode!=GML_RENDER_REPLAY_EXACT &&
      operation->reconstruction_mode!=GML_RENDER_DEVICE_CHECKPOINT) ||
     !same_scene_source(operation->sources[0],source->image)) return 0;
  /* Validate the homogeneous group before a capacity flush or any append. Payload geometry
   * remains the caller's typed preparation; ownership and versions cannot vary within it. */
  if(operation->target.image.content_version>UINT64_MAX-(count-1u)) return 0;
  for(uint32_t index=1u;index<count;index++){
    const GmlRenderDrawOp *part=&operation[index];
    if(part->opcode!=operation->opcode || part->reconstruction_mode!=operation->reconstruction_mode ||
       part->scene_family!=operation->scene_family || part->source_count!=1u || !part->sources ||
       !same_scene_source(part->sources[0],source->image) ||
       part->target.image.resource_class!=operation->target.image.resource_class ||
       part->target.image.identity!=operation->target.image.identity ||
       part->target.image.incarnation!=operation->target.image.incarnation ||
       part->target.image.content_version!=operation->target.image.content_version+index ||
       part->target.storage_version!=operation->target.storage_version ||
       part->target.width!=operation->target.width || part->target.height!=operation->target.height ||
       part->target.pixel_format!=operation->target.pixel_format ||
       memcmp(&part->target.region,&operation->target.region,sizeof part->target.region) ||
       memcmp(&part->payload.quad.sampling,&operation->payload.quad.sampling,
              sizeof part->payload.quad.sampling)) return 0;
  }
  exact_filtered_font=operation->reconstruction_mode==GML_RENDER_REPLAY_EXACT &&
    operation->scene_family==GML_RENDER_SCENE_TEXT &&
    state->rgb_equation==GML_RENDER_BLEND_EXACT_FILTERED_FONT &&
    state->alpha_equation==GML_RENDER_BLEND_EXACT_FILTERED_FONT &&
    gml_render_filtered_font_quad_valid(&operation->payload.quad,
                                        operation->target.width,operation->target.height);
  coalescible=(source->borrowed_immutable || mutable_text) &&
    (((operation->scene_family==GML_RENDER_SCENE_TEXT ||
     operation->scene_family==GML_RENDER_SCENE_SPRITE) &&
    (operation->reconstruction_mode==GML_RENDER_DEVICE_CHECKPOINT || exact_filtered_font) &&
    (operation->payload.quad.sampling.filter==GML_RENDER_FILTER_BILINEAR ||
     (operation->payload.quad.sampling.filter==GML_RENDER_FILTER_HARDWARE_BILINEAR &&
      (operation->payload.quad.sampling.quantisation==GML_RENDER_QUANTISE_HARDWARE_SPRITE_FIRSTGEN ||
       operation->payload.quad.sampling.quantisation==GML_RENDER_QUANTISE_HARDWARE_SPRITE_MODERN ||
       operation->payload.quad.sampling.quantisation==GML_RENDER_QUANTISE_FIXED_STRETCHED)) ||
     operation->payload.quad.sampling.filter==GML_RENDER_FILTER_NEAREST_PHASED ||
     operation->payload.quad.sampling.filter==GML_RENDER_FILTER_NEAREST_PHASED_PACKET_ALPHA ||
     (operation->payload.quad.sampling.filter==GML_RENDER_FILTER_BOX &&
      state->coverage_policy==GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA) ||
     operation->payload.quad.sampling.filter==GML_RENDER_FILTER_NEAREST)) ||
     ((operation->scene_family==GML_RENDER_SCENE_BACKGROUND ||
       operation->scene_family==GML_RENDER_SCENE_TILE) &&
      (operation->reconstruction_mode==GML_RENDER_REPLAY_EXACT ||
       ((operation->scene_family==GML_RENDER_SCENE_TILE ||
         (operation->scene_family==GML_RENDER_SCENE_BACKGROUND && r->classic_phase_scene_preparing)) &&
        operation->reconstruction_mode==GML_RENDER_DEVICE_CHECKPOINT)) &&
      operation->payload.quad.sampling.filter==GML_RENDER_FILTER_NEAREST) ||
     (operation->scene_family==GML_RENDER_SCENE_BACKGROUND &&
      operation->reconstruction_mode==GML_RENDER_DEVICE_CHECKPOINT &&
      operation->payload.quad.sampling.filter==GML_RENDER_FILTER_HARDWARE_BILINEAR &&
      state->coverage_policy==GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA));
  pending=r->pending_scene_batch;
  if(pending && pending->preparing && !source->borrowed_immutable && !versioned_surface) return 0;
  if(pending && pending->count){
    int paired_preparation=pending->preparing && r->classic_phase_scene_preparing;
    int surface_preparation=pending->preparing==GML_SCENE_PREPARING_SURFACE && versioned_surface;
    uint32_t target_limit=surface_preparation?GML_SCENE_SURFACE_RUN_LIMIT:GML_SCENE_BATCH_LIMIT;
    uint32_t limit=paired_preparation?GML_SCENE_PAIRED_LIMIT:target_limit;
    uint32_t target_count=paired_preparation?
      (r->classic_phase_scene_target?pending->phase_count:pending->count-pending->phase_count):pending->count;
    int flush=!pending->source_count || target_count>target_limit-count ||
      pending->count>limit-count ||
      pending->coalesce_sources!=coalescible;
    GmlRenderImageRef previous=pending->operations[0].target.image;
    if(!same_scene_image(previous,operation->target.image)){
      int paired=paired_root_targets(r,previous,operation->target.image) &&
        source->borrowed_immutable;
      for(uint32_t index=0;paired && index<pending->source_count;index++)
        paired=pending->sources[index].borrowed_immutable;
      if(!paired) flush=1;
    }
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
   * CPU readers and capacities retain their existing boundaries. Runtime-font
   * pages keep mutable views; their writer flushes before a version mutation. */
  next=gml_render_application_scene_next_image_ref(
    r,gml_render_scene_target_resource_ensure(r));
  if(next.content_version==UINT64_MAX ||
     operation->target.image.content_version!=next.content_version+1u ||
     operation->target.image.resource_class!=next.resource_class ||
     operation->target.image.identity!=next.identity ||
     operation->target.image.incarnation!=next.incarnation) return 0;
  if(!scene_pending_reserve(pending,pending->count+count)) return 0;
  if(!pending->count){
    pending->states[0]=*state;
    pending->state_count=1u;
    pending->sources[0]=*source;
    pending->source_refs[0]=source->image;
    pending->source_count=1u;
    pending->coalesce_sources=coalescible;
  } else if(coalescible){
    if(state_index==pending->state_count)
      pending->states[pending->state_count++]=*state;
    if(source_index==pending->source_count){
      pending->sources[pending->source_count]=*source;
      pending->source_refs[pending->source_count++]=source->image;
    }
  }
  for(uint32_t index=0;index<count;index++){
    pending->operations[pending->count]=operation[index];
    pending->operations[pending->count].sources=&pending->source_refs[source_index];
    pending->operations[pending->count].state_index=state_index;
    pending->count++;
  }
  if(r->classic_phase_scene_target) pending->phase_count+=count;
  if(r->fb==r->app_surface_owned){
    r->app_surface_opaque=0;
    r->app_presentation_coverage_active=0;
  }
  /* Checkpoint-only runtime glyphs may share a stable page version. Font
   * growth/deletion flushes it before mutating bytes; it never becomes an
   * immutable replay source. Legacy mutable packets retain immediate delivery. */
  /* Surface writes, target changes, resize and free flush this version before mutation. */
  return source->borrowed_immutable || versioned_surface || mutable_text ||
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
  if(r->base_history_resource){
    if(device && device->retire)
      device->retire(device->context,gml_render_resource_image_ref(r->base_history_resource));
    gml_render_resource_destroy(r->base_history_resource);
  }
  free(r->base_history_pixels);
  r->base_history_resource=NULL;
  r->base_history_pixels=NULL;
  r->base_history_count=0;
  r->base_scene_resource=NULL;
  r->base_scene_resource_pixels=NULL;
  r->base_scene_resource_width=r->base_scene_resource_height=0;
  r->base_scene_resource_masked=0;
}

GmlRenderResource *gml_render_base_resource_lookup(const GmlRender *r,
  const uint32_t *pixels,int *width,int *height){
  if(!r || !pixels) return NULL;
  if(r->base_scene_resource && pixels==r->base_scene_resource_pixels){
    if(width) *width=r->base_scene_resource_width;
    if(height) *height=r->base_scene_resource_height;
    return r->base_scene_resource;
  }
  if(r->retained_base_resource && pixels==r->retained_base_pixels){
    if(width) *width=r->retained_base_width;
    if(height) *height=r->retained_base_height;
    return r->retained_base_resource;
  }
  if(r->view_base_resource && pixels==r->view_base_pixels){
    if(width) *width=r->view_base_width;
    if(height) *height=r->view_base_height;
    return r->view_base_resource;
  }
  return NULL;
}

/* Move one explicitly registered inactive image, including its raw-linear tail,
 * through the existing base selector and resize implementation. */
static void view_base_swap_retained(GmlRender *r){
#define SWAP_VIEW_BASE(type,retained,view) do { type temporary=r->retained; \
  r->retained=r->view;r->view=temporary; } while(0)
  SWAP_VIEW_BASE(GmlRenderResource *,retained_base_resource,view_base_resource);
  SWAP_VIEW_BASE(uint32_t *,retained_base_pixels,view_base_pixels);
  SWAP_VIEW_BASE(int,retained_base_width,view_base_width);
  SWAP_VIEW_BASE(int,retained_base_height,view_base_height);
  SWAP_VIEW_BASE(int,retained_base_masked,view_base_masked);
  SWAP_VIEW_BASE(GmlRenderResource *,retained_base_history_resource,view_base_history_resource);
  SWAP_VIEW_BASE(uint32_t *,retained_base_history_pixels,view_base_history_pixels);
  SWAP_VIEW_BASE(size_t,retained_base_history_count,view_base_history_count);
#undef SWAP_VIEW_BASE
}

void gml_render_view_base_dispose(GmlRender *r){
  if(!r || !r->view_base_resource) return;
  view_base_swap_retained(r);
  gml_render_retained_base_dispose(r);
  view_base_swap_retained(r);
}

void gml_render_retained_base_dispose(GmlRender *r){
  if(!r || !r->retained_base_resource) return;
  (void)gml_render_application_scene_flush(r);
  const GmlRenderDeviceServices *device=gml_render_device(r);
  if(device && device->retire)
    device->retire(device->context,gml_render_resource_image_ref(r->retained_base_resource));
  gml_render_resource_destroy(r->retained_base_resource);
  if(r->retained_base_history_resource){
    if(device && device->retire)
      device->retire(device->context,gml_render_resource_image_ref(r->retained_base_history_resource));
    gml_render_resource_destroy(r->retained_base_history_resource);
  }
  free(r->retained_base_history_pixels);
  r->retained_base_history_resource=NULL;r->retained_base_history_pixels=NULL;
  r->retained_base_history_count=0;
  r->retained_base_resource=NULL;r->retained_base_pixels=NULL;
  r->retained_base_width=r->retained_base_height=0;
  r->retained_base_masked=0;
}

int gml_render_can_begin_borrowed_screen(const GmlRender *r,const uint32_t *pixels){
  const GmlRenderDeviceServices *device=gml_render_device(r);
  return r && pixels && r->base_scene_resource && r->classic && r->win &&
    (!anygm_policy_classic_modern_presentation(r->win) ||
     (r->classic_phase_resource && r->classic_phase_y==r->classic_phase_resource_pixels &&
      r->app_w==r->classic_phase_resource_width && r->app_h==r->classic_phase_resource_height &&
      same_scene_image(gml_render_resource_image_ref(r->base_scene_resource),r->classic_phase_base_image))) &&
    r->scene_device_enabled &&
    device && device->observe_on_demand && device->observe_on_demand(device->context) &&
    r->app_surface==r->base_scene_resource_pixels && r->app_surface!=r->app_surface_owned &&
    pixels!=r->app_surface && r->app_w==r->base_scene_resource_width &&
    r->app_h==r->base_scene_resource_height &&
    (!r->retained_base_resource || r->retained_base_pixels==pixels);
}

int gml_render_base_resource_select(GmlRender *r,uint32_t *pixels){
  if(!r || pixels==r->base_scene_resource_pixels) return 0;
  if(r->view_base_resource && pixels==r->view_base_pixels) view_base_swap_retained(r);
  if(r->retained_base_resource){
    if(pixels!=r->retained_base_pixels) return r->base_scene_resource?-1:0;
  } else if(!gml_render_can_begin_borrowed_screen(r,pixels)){
    /* A raw outside-mask allocation can precede the first actual SCREEN.
     * Reserve its existing second slot rather than discarding device-only bytes.
     * Other third bindings retain the established two-image refusal. */
    const GmlRenderDeviceServices *device=gml_render_device(r);
    if(!r->scene_device_enabled || !r->base_scene_resource_masked || !device ||
       !device->observe_on_demand || !device->observe_on_demand(device->context)) return 0;
  }
  GmlRenderResource *resource=r->base_scene_resource;
  uint32_t *previous=r->base_scene_resource_pixels;
  int width=r->base_scene_resource_width,height=r->base_scene_resource_height;
  int masked=r->base_scene_resource_masked;
  GmlRenderResource *history=r->base_history_resource;
  uint32_t *history_pixels=r->base_history_pixels;
  size_t history_count=r->base_history_count;
  r->base_scene_resource=r->retained_base_resource;
  r->base_scene_resource_pixels=r->retained_base_pixels;
  r->base_scene_resource_width=r->retained_base_width;
  r->base_scene_resource_height=r->retained_base_height;
  r->base_scene_resource_masked=r->retained_base_masked;
  r->retained_base_resource=resource;r->retained_base_pixels=previous;
  r->retained_base_width=width;r->retained_base_height=height;
  r->retained_base_masked=masked;
  r->base_history_resource=r->retained_base_history_resource;
  r->base_history_pixels=r->retained_base_history_pixels;
  r->base_history_count=r->retained_base_history_count;
  r->retained_base_history_resource=history;r->retained_base_history_pixels=history_pixels;
  r->retained_base_history_count=history_count;
  return 1;
}

/* Only the inactive suffix needs this observation. Active CPU writes never overwrite that
 * suffix; conversely a history checkpoint must not overwrite a more recent active prefix. */
static int base_history_observe(GmlRenderResource *history,uint32_t *target,
                                 size_t active,size_t count){
  const uint32_t *pixels=NULL;
  if(!history || count<=active) return 1;
  if(!gml_render_resource_acquire_cpu_read(history,&pixels)) return 0;
  memcpy(target+active,pixels+active,(count-active)*sizeof(uint32_t));
  return 1;
}

int gml_render_base_history_observe(GmlRender *r){
  return r && base_history_observe(r->base_history_resource,r->base_scene_resource_pixels,
    (size_t)r->base_scene_resource_width*r->base_scene_resource_height,r->base_history_count) &&
    base_history_observe(r->retained_base_history_resource,r->retained_base_pixels,
    (size_t)r->retained_base_width*r->retained_base_height,r->retained_base_history_count) &&
    base_history_observe(r->view_base_history_resource,r->view_base_pixels,
    (size_t)r->view_base_width*r->view_base_height,r->view_base_history_count);
}

static void base_stage_dispose(const GmlRenderDeviceServices *device,
                               GmlRenderResource *resource){
  if(!resource) return;
  if(device->retire) device->retire(device->context,gml_render_resource_image_ref(resource));
  gml_render_resource_destroy(resource);
}

static GmlRenderResource *base_stage_create(uint32_t *pixels,int width,int height,
                                            uint32_t identity){
  uint64_t incarnation=gml_render_scene_incarnation_allocate();
  if(!incarnation) return NULL;
  GmlRenderImageRef image={GML_RENDER_RESOURCE_SCREEN,identity,incarnation,1u};
  return gml_render_resource_create(image,(uint32_t)width,(uint32_t)height,
    GML_RENDER_PIXEL_ARGB8888,pixels,(GmlRenderResourceLimits){0});
}

static int resource_clear(GmlRenderResource *resource,const GmlRenderDeviceServices *device,
                            unsigned width,unsigned height,uint32_t colour,GmlRender *owner){
  GmlRenderDrawOp operation={0};
  operation.opcode=GML_RENDER_OP_CLEAR;
  operation.reconstruction_mode=GML_RENDER_DEVICE_CHECKPOINT;
  operation.target=(GmlRenderTargetRef){gml_render_resource_image_ref(resource),1u,
    width,height,GML_RENDER_PIXEL_ARGB8888,{0,0,width,height}};
  if(operation.target.image.content_version==UINT64_MAX) return 0;
  operation.target.image.content_version++;
  operation.payload.clear_colour=colour;
  GmlRenderDrawBatch batch={&operation,1u,NULL,0u};
  GmlRenderResourceExecution result=gml_render_resource_submit_deferred(
    resource,device,&batch,NULL,0u);
  if(owner) gml_render_dispatch_batch_record(owner,&batch,result);
  return result==GML_RENDER_RESOURCE_EXEC_DEVICE;
}

static int base_stage_clear(GmlRenderResource *resource,const GmlRenderDeviceServices *device,
                             unsigned width,unsigned height){
  return resource_clear(resource,device,width,height,0u,NULL);
}

static int multiview_device_ready(GmlRender *r){
  const GmlRenderDeviceServices *device=gml_render_device(r);
  return r && r->scene_device_enabled && device && device->observe_on_demand &&
    device->observe_on_demand(device->context) &&
    !(r->pending_scene_batch && r->pending_scene_batch->preparing);
}

static GmlRenderResource *multiview_base_ensure(GmlRender *r,uint32_t *pixels,int w,int h){
  int previous_w=0,previous_h=0;
  GmlRenderResource *resource=gml_render_base_resource_lookup(r,pixels,&previous_w,&previous_h);
  if(resource){
    if(previous_w==w && previous_h==h) return resource;
    uint32_t *previous=r->base_scene_resource_pixels;
    int swapped=resource!=r->base_scene_resource;
    if(swapped && gml_render_base_resource_select(r,pixels)!=1) return NULL;
    int resized=gml_render_base_resource_resize(r,w,h);
    if(swapped && gml_render_base_resource_select(r,previous)!=1) return NULL;
    return resized?gml_render_base_resource_lookup(r,pixels,NULL,NULL):NULL;
  }
  /* No eviction. Only this coordinator boundary may reserve the third image;
   * ordinary base admission retains its existing two-image capacity policy. */
  if(r->base_scene_resource && r->retained_base_resource && r->view_base_resource) return NULL;
  resource=base_stage_create(pixels,w,h,0u);
  if(!resource) return NULL;
  gml_render_resource_bind_dispatch(resource,r);
  if(!r->base_scene_resource){
    r->base_scene_resource=resource;r->base_scene_resource_pixels=pixels;
    r->base_scene_resource_width=w;r->base_scene_resource_height=h;
  }else if(!r->retained_base_resource){
    r->retained_base_resource=resource;r->retained_base_pixels=pixels;
    r->retained_base_width=w;r->retained_base_height=h;
  }else{
    r->view_base_resource=resource;r->view_base_pixels=pixels;
    r->view_base_width=w;r->view_base_height=h;
  }
  return resource;
}

int gml_render_multiview_prepare(GmlRender *r,uint32_t *root,int width,int height,
                                 uint32_t *view,int vw,int vh){
  if(!multiview_device_ready(r) || !root || !view || root==view ||
     width<=0 || height<=0 || vw<=0 || vh<=0 || width>4096 || height>4096 ||
     vw>4096 || vh>4096 || r->target_sp ||
     (r->target_id!=-1 && !(r->target_id==0 && !r->fb)) ||
     r->pending_fill || r->pending_underlay || r->pending_presentation ||
     !gml_render_application_scene_flush(r)) return 0;
  return multiview_base_ensure(r,root,width,height) &&
    multiview_base_ensure(r,view,vw,vh);
}

int gml_render_multiview_clear(GmlRender *r,uint32_t *pixels,int width,int height,uint32_t colour){
  int w=0,h=0;
  if(!multiview_device_ready(r) || !gml_render_application_scene_flush(r)) return 0;
  GmlRenderResource *resource=gml_render_base_resource_lookup(r,pixels,&w,&h);
  return resource && w==width && h==height &&
    resource_clear(resource,gml_render_device(r),(unsigned)w,(unsigned)h,colour,r);
}

int gml_render_multiview_copy(GmlRender *r,const uint32_t *pixels,int sw,int sh,
  uint32_t *destination,int dw,int dh,int x,int y,int width,int height,int seed){
  GmlRenderResourceSource source={0};
  GmlRenderResource *origin,*target;
  int ow=0,oh=0,tw=0,th=0;
  if(!multiview_device_ready(r) || !pixels || !destination || pixels==destination ||
     sw<=0 || sh<=0 || dw<=0 || dh<=0 || sw>4096 || sh>4096 || dw>4096 || dh>4096 ||
     width<=0 || height<=0 || width>16384 || height>16384 ||
     x < -16384 || x>16384 || y < -16384 || y>16384 || (seed!=0 && seed!=1) ||
     !gml_render_application_scene_flush(r)) return 0;
  origin=gml_render_base_resource_lookup(r,pixels,&ow,&oh);
  target=gml_render_base_resource_lookup(r,destination,&tw,&th);
  if(destination==r->app_surface_owned && r->app_w==dw && r->app_h==dh){
    target=gml_render_application_resource_ensure(r);tw=dw;th=dh;
  }
  if(!origin || !target || origin==target || ow!=sw || oh!=sh || tw!=dw || th!=dh ||
     (!gml_render_resource_device_source_view(origin,gml_render_device(r),&source) &&
      !gml_render_resource_current_cpu_source_view(origin,&source))) return 0;
  int64_t x0=x<0?0:x,y0=y<0?0:y,x1=(int64_t)x+width,y1=(int64_t)y+height;
  int clip_w=seed?sw:dw,clip_h=seed?sh:dh;
  if(x1>clip_w) x1=clip_w;
  if(y1>clip_h) y1=clip_h;
  if(x0>=x1 || y0>=y1) return 1;
  GmlRenderDrawStateSnapshot state={0};
  state.write_mask=15u;state.colour=0xFFFFFFFFu;state.alpha=255u;state.sampler_count=1u;
  state.samplers[0]=(GmlRenderSamplingPolicy){
    seed?GML_RENDER_FILTER_VIEW_SEED:GML_RENDER_FILTER_VIEW_PIXEL_CENTRE,
    GML_RENDER_EDGE_CLAMP_SUBIMAGE,{0,0,(uint32_t)sw,(uint32_t)sh},0u,0u};
  GmlRenderDrawOp op={0};
  op.opcode=GML_RENDER_OP_TEXTURED_QUAD;op.scene_family=GML_RENDER_SCENE_SURFACE;
  op.reconstruction_mode=GML_RENDER_DEVICE_CHECKPOINT;
  op.target=(GmlRenderTargetRef){gml_render_resource_image_ref(target),1u,
    (uint32_t)dw,(uint32_t)dh,GML_RENDER_PIXEL_ARGB8888,{0,0,(uint32_t)dw,(uint32_t)dh}};
  if(op.target.image.content_version==UINT64_MAX) return 0;
  op.target.image.content_version++;
  op.sources=&source.image;op.source_count=1u;
  GmlRenderQuad *q=&op.payload.quad;
  q->shape=GML_RENDER_QUAD_AXIS_ALIGNED;q->sampling=state.samplers[0];
  q->inverse_anchor[0]=x;q->inverse_anchor[1]=y;
  q->inverse_x[0]=width;q->inverse_y[1]=height;
  q->span_x[0]=dw;q->span_x[1]=dh;
  for(unsigned corner=0;corner<4u;corner++){
    unsigned right=corner==1u || corner==2u,bottom=corner>=2u;
    q->x[corner]=seed?(right?dw:0):(double)(right?x1:x0);
    q->y[corner]=seed?(bottom?dh:0):(double)(bottom?y1:y0);
    /* Raw samplers use the integer map, not interpolated coordinates. */
    q->u[corner]=right;q->v[corner]=bottom;q->corner_colour[corner]=0xFFFFFFFFu;
  }
  GmlRenderDrawBatch batch={&op,1u,&state,1u};
  GmlRenderResourceExecution result=gml_render_resource_submit_deferred(
    target,gml_render_device(r),&batch,&source,1u);
  gml_render_dispatch_batch_record(r,&batch,result);
  return result==GML_RENDER_RESOURCE_EXEC_DEVICE;
}

int gml_render_multiview_copy_to_owned_application(GmlRender *r,
  const uint32_t *source,int width,int height){
  int dw=0,dh=0;
  return gml_render_application_surface_owned_extent(r,&dw,&dh) &&
    gml_render_multiview_copy(r,source,width,height,r->app_surface_owned,
      dw,dh,0,0,dw,dh,0);
}

typedef struct RawSurfaceCopyView {
  uint32_t *pixels;
  int width,height;
  GmlRenderResource *resource;
  GmlRenderResourceSource physical;
} RawSurfaceCopyView;

static int raw_surface_copy_view(GmlRender *r,int id,RawSurfaceCopyView *view){
  memset(view,0,sizeof *view);
  if(id==0){
    view->pixels=r->app_surface;
    view->width=r->app_w?r->app_w:(r->base_fbw?r->base_fbw:r->fbw);
    view->height=r->app_h?r->app_h:(r->base_fbh?r->base_fbh:r->fbh);
    if(!view->pixels || view->width<=0 || view->height<=0) return 0;
    if(view->pixels==r->app_surface_owned)
      view->resource=gml_render_application_resource_ensure(r);
    else{
      view->resource=gml_render_base_resource_lookup(r,view->pixels,NULL,NULL);
      for(int slot=0;!view->resource && slot<GML_MAX_SURFACES;slot++)
        if(r->surface[slot].live && r->surface[slot].px==view->pixels)
          view->resource=gml_render_surface_resource_ensure(r,slot+1);
      if(!view->resource)
        view->resource=multiview_base_ensure(r,view->pixels,view->width,view->height);
    }
  }else{
    int slot=surface_slot(id);
    if(slot<0 || !r->surface[slot].live || !r->surface[slot].px) return 0;
    view->pixels=r->surface[slot].px;view->width=r->surface[slot].w;view->height=r->surface[slot].h;
    view->resource=gml_render_surface_resource_ensure(r,id);
  }
  return view->resource && view->width>0 && view->height>0 &&
    (gml_render_resource_device_source_view(view->resource,gml_render_device(r),&view->physical) ||
     gml_render_resource_current_cpu_source_view(view->resource,&view->physical));
}

static int raw_surface_copy_suspended_fills(GmlRender *r,RawSurfaceCopyView *view){
  for(int index=0;index<r->target_sp;index++){
    if(r->target_stack[index].fb!=view->pixels || !r->target_stack[index].pending_fill) continue;
    int width=r->target_stack[index].w,height=r->target_stack[index].h;
    if(width<=0 || height<=0) continue;
    /* A differently pitched pending fill needs its own qualified raw clear.
     * Never write a complete physical image for a logical prefix fill. */
    if((uint32_t)width!=view->physical.width || (uint32_t)height!=view->physical.height ||
       !resource_clear(view->resource,gml_render_device(r),(unsigned)width,(unsigned)height,
         r->target_stack[index].fill_color,r)) return 0;
    r->target_stack[index].pending_fill=0;
  }
  return 1;
}

int gml_render_resident_surface_copy(GmlRender *r,int dst,int x,int y,int src,
  int source_x,int source_y,int width,int height,int full_source){
  const GmlRenderDeviceServices *device=gml_render_device(r);
  GmlRenderDeviceCaps caps={0};
  RawSurfaceCopyView origin,destination;
  if(!r || !r->scene_device_enabled || !device || !device->capabilities ||
     !device->observe_on_demand || !device->observe_on_demand(device->context) ||
     (r->pending_scene_batch && r->pending_scene_batch->preparing) ||
     !device->capabilities(device->context,&caps) ||
     !(caps.supported_operations&(UINT64_C(1)<<GML_RENDER_OP_COPY_IMAGE)) ||
     !raw_surface_copy_view(r,src,&origin) || !raw_surface_copy_view(r,dst,&destination)) return 0;
  if(origin.pixels==r->fb || destination.pixels==r->fb || src==r->target_id ||
     dst==r->target_id || (r->pending_presentation && r->presentation.shader>=0))
    gml_render_maybe_prepare_draw(r);
  if(!gml_render_application_scene_flush(r) ||
     !raw_surface_copy_view(r,src,&origin) || !raw_surface_copy_view(r,dst,&destination) ||
     !raw_surface_copy_suspended_fills(r,&origin) ||
     !raw_surface_copy_suspended_fills(r,&destination) ||
     !raw_surface_copy_view(r,src,&origin) || !raw_surface_copy_view(r,dst,&destination)) return 0;
  int64_t sx=full_source?0:source_x,sy=full_source?0:source_y,dx=x,dy=y;
  int64_t cw=full_source?origin.width:width,ch=full_source?origin.height:height;
  if(sx<0){int64_t skip=-sx;sx=0;dx+=skip;cw-=skip;}
  if(sy<0){int64_t skip=-sy;sy=0;dy+=skip;ch-=skip;}
  if(dx<0){int64_t skip=-dx;dx=0;sx+=skip;cw-=skip;}
  if(dy<0){int64_t skip=-dy;dy=0;sy+=skip;ch-=skip;}
  if(cw<=0 || ch<=0 || sx>=origin.width || sy>=origin.height ||
     dx>=destination.width || dy>=destination.height) return 1;
  if(cw>(int64_t)origin.width-sx)cw=(int64_t)origin.width-sx;
  if(ch>(int64_t)origin.height-sy)ch=(int64_t)origin.height-sy;
  if(cw>(int64_t)destination.width-dx)cw=(int64_t)destination.width-dx;
  if(ch>(int64_t)destination.height-dy)ch=(int64_t)destination.height-dy;
  if(cw<=0 || ch<=0) return 1;
  /* Reject unavailable high-water bytes before issuing a prefix. Physical
   * row wrapping itself is supported by the typed raw device copy. */
  uint64_t source_end=(uint64_t)(sy+ch-1)*origin.width+(uint64_t)(sx+cw);
  uint64_t destination_end=(uint64_t)(dy+ch-1)*destination.width+(uint64_t)(dx+cw);
  if(source_end>(uint64_t)origin.physical.width*origin.physical.height ||
     destination_end>(uint64_t)destination.physical.width*destination.physical.height) return 0;
  if(!gml_render_resource_prepare_device_source(origin.resource,device,&origin.physical)) return 0;
  GmlRenderDrawOp op={0};GmlRenderDrawStateSnapshot state={0};
  op.opcode=GML_RENDER_OP_COPY_IMAGE;op.scene_family=GML_RENDER_SCENE_SURFACE;
  op.reconstruction_mode=GML_RENDER_DEVICE_CHECKPOINT;
  op.target=(GmlRenderTargetRef){gml_render_resource_image_ref(destination.resource),1u,
    destination.physical.width,destination.physical.height,GML_RENDER_PIXEL_ARGB8888,
    {0,0,destination.physical.width,destination.physical.height}};
  if(op.target.image.content_version==UINT64_MAX) return 0;
  op.target.image.content_version++;
  op.source_count=1u;op.sources=&origin.physical.image;
  op.payload.copy=(GmlRenderCopyRegion){
    {(int32_t)sx,(int32_t)sy,(uint32_t)cw,(uint32_t)ch},
    {(int32_t)dx,(int32_t)dy,(uint32_t)cw,(uint32_t)ch},
    origin.resource==destination.resource,(uint32_t)origin.width,(uint32_t)destination.width};
  GmlRenderDrawBatch batch={&op,1u,&state,1u};
  GmlRenderResourceExecution result=gml_render_resource_submit_deferred(
    destination.resource,device,&batch,&origin.physical,1u);
  if(result!=GML_RENDER_RESOURCE_EXEC_DEVICE) return 0;
  gml_render_dispatch_batch_record(r,&batch,result);
  int slot=surface_slot(dst);
  if(slot>=0){
    r->surface[slot].dirty=1;r->surface[slot].opaque_known=0;
    r->surface[slot].all_opaque=r->surface[slot].all_transparent=0;
  }
  if(dst==0) r->app_surface_opaque=0;
  if(dst==r->target_id)
    r->fb_opaque_known=r->fb_all_opaque=r->fb_all_transparent=0;
  return 1;
}

int gml_render_resident_owned_application_clear(GmlRender *r,uint32_t colour){
  const GmlRenderDeviceServices *device=gml_render_device(r);
  GmlRenderResource *resource;
  if(!r || !r->scene_device_enabled || !device || !device->observe_on_demand ||
     !device->observe_on_demand(device->context) ||
     (r->pending_scene_batch && r->pending_scene_batch->preparing) ||
     !(resource=gml_render_application_resource_ensure(r))) return 0;
  if(!gml_render_application_scene_flush(r) ||
     !resource_clear(resource,device,(unsigned)r->app_w,(unsigned)r->app_h,colour,r)) return 0;
  r->app_surface_opaque=(colour>>24)==255u;
  if(r->fb==r->app_surface_owned){
    r->fb_opaque_known=1;r->fb_all_opaque=(colour>>24)==255u;
    r->fb_all_transparent=(colour>>24)==0u;
  }
  return 1;
}

int gml_render_application_surface_owned_stage_base(GmlRender *r,const uint32_t *base,
  int width,int height,int x,int y,uint32_t background){
  const GmlRenderDeviceServices *device=gml_render_device(r);
  GmlRenderResourceSource source={0};
  GmlRenderResource *origin,*target;
  int source_width=0,source_height=0;
  if(!r || !base || width<=0 || height<=0 || width>4096 || height>4096 ||
     !r->scene_device_enabled || !device || !device->observe_on_demand ||
     !device->observe_on_demand(device->context) ||
     r->target_sp || r->target_id!=-1 || r->fb!=base ||
     r->app_w!=width || r->app_h!=height || !r->app_surface_owned ||
     base==r->app_surface_owned ||
     (r->pending_scene_batch && r->pending_scene_batch->preparing) ||
     !gml_render_application_scene_flush(r) ||
     !(origin=gml_render_base_resource_lookup(r,base,&source_width,&source_height)) ||
     source_width!=width || source_height!=height ||
     !(target=gml_render_application_resource_ensure(r)) || origin==target ||
     gml_render_resource_image_ref(target).content_version>UINT64_MAX-2u ||
     (!gml_render_resource_device_source_view(origin,device,&source) &&
      !gml_render_resource_current_cpu_source_view(origin,&source))) return 0;
  /* Resolve clipping in wide integers, including INT_MIN/MAX ports. The packet
   * remains an ordinary raw nearest quad, not a new sampling/blending shader. */
  int64_t sx=x<0?-(int64_t)x:0,sy=y<0?-(int64_t)y:0;
  int64_t dx=x>0?x:0,dy=y>0?y:0;
  int64_t w=(int64_t)width-sx,h=(int64_t)height-sy;
  if(w>(int64_t)width-dx) w=(int64_t)width-dx;
  if(h>(int64_t)height-dy) h=(int64_t)height-dy;
  if(!gml_render_resident_owned_application_clear(r,background)) return 0;
  if(w>0 && h>0){
    GmlRenderDrawStateSnapshot state={0};
    state.write_mask=15u;state.colour=0xFFFFFFFFu;state.alpha=255u;
    state.sampler_count=1u;
    state.samplers[0]=(GmlRenderSamplingPolicy){GML_RENDER_FILTER_NEAREST,
      GML_RENDER_EDGE_CLAMP_SUBIMAGE,{0,0,(uint32_t)width,(uint32_t)height},0u,0u};
    GmlRenderDrawOp op={0};
    op.opcode=GML_RENDER_OP_TEXTURED_QUAD;
    op.scene_family=GML_RENDER_SCENE_SURFACE;
    op.reconstruction_mode=GML_RENDER_DEVICE_CHECKPOINT;
    op.target=(GmlRenderTargetRef){gml_render_resource_image_ref(target),1u,
      (uint32_t)width,(uint32_t)height,GML_RENDER_PIXEL_ARGB8888,
      {0,0,(uint32_t)width,(uint32_t)height}};
    op.target.image.content_version++;
    op.sources=&source.image;op.source_count=1u;
    GmlRenderQuad *q=&op.payload.quad;
    q->shape=GML_RENDER_QUAD_AXIS_ALIGNED;q->sampling=state.samplers[0];
    for(unsigned corner=0;corner<4u;corner++){
      unsigned right=corner==1u || corner==2u,bottom=corner>=2u;
      q->x[corner]=(double)(dx+(right?w:0));q->y[corner]=(double)(dy+(bottom?h:0));
      q->u[corner]=(double)(sx+(right?w:0))/width;
      q->v[corner]=(double)(sy+(bottom?h:0))/height;
      q->corner_colour[corner]=0xFFFFFFFFu;
    }
    GmlRenderDrawBatch batch={&op,1u,&state,1u};
    GmlRenderResourceExecution result=gml_render_resource_submit_deferred(
      target,device,&batch,&source,1u);
    gml_render_dispatch_batch_record(r,&batch,result);
    if(result!=GML_RENDER_RESOURCE_EXEC_DEVICE) return 0;
  }
  /* The coordinator keeps its established opaque presentation policy. No public
   * draw state or active target is borrowed or changed by the staging packets. */
  return 1;
}

/* Split at both pitches: every copied band maps identical linear indices and samples texels
 * nearest1:1. No scaling, blending, destination feedback, shader arithmetic or CPU replay. */
static int base_stage_copy(GmlRenderResource *target,const GmlRenderDeviceServices *device,
                           unsigned width,unsigned height,const GmlRenderResourceSource *source,
                           size_t begin,size_t end){
  enum { BAND_LIMIT=GML_SCENE_BATCH_LIMIT };
  GmlRenderDrawOp *operations=calloc(BAND_LIMIT,sizeof *operations);
  GmlRenderDrawStateSnapshot state={0};
  if(!operations) return 0;
  state.write_mask=15u;state.colour=0xFFFFFFFFu;state.alpha=255u;
  state.sampler_count=1u;
  state.samplers[0]=(GmlRenderSamplingPolicy){GML_RENDER_FILTER_NEAREST,
    GML_RENDER_EDGE_CLAMP_SUBIMAGE,{0,0,source->width,source->height},0u,0u};
  while(begin<end){
    uint32_t count=0;
    GmlRenderImageRef image=gml_render_resource_image_ref(target);
    while(begin<end && count<BAND_LIMIT){
      unsigned sx=(unsigned)(begin%source->width),sy=(unsigned)(begin/source->width);
      unsigned dx=(unsigned)(begin%width),dy=(unsigned)(begin/width);
      size_t length=source->width-sx;
      if(length>width-dx) length=width-dx;
      if(length>end-begin) length=end-begin;
      /* Equal pitches can coalesce complete rows; mismatched pitches remain disjoint bands. */
      unsigned rows=1u;
      if(!sx && !dx && source->width==width && length==width){
        size_t available=(end-begin)/width;
        if(available>height-dy) available=height-dy;
        rows=(unsigned)available;
      }
      GmlRenderDrawOp *op=&operations[count++];
      memset(op,0,sizeof *op);
      op->opcode=GML_RENDER_OP_TEXTURED_QUAD;
      op->reconstruction_mode=GML_RENDER_DEVICE_CHECKPOINT;
      op->target=(GmlRenderTargetRef){image,1u,width,height,GML_RENDER_PIXEL_ARGB8888,
                                     {0,0,width,height}};
      op->target.image.content_version+=count;
      op->sources=&source->image;op->source_count=1u;
      GmlRenderQuad *q=&op->payload.quad;
      q->shape=GML_RENDER_QUAD_AXIS_ALIGNED;
      /* Complete-source clamp is shared by consecutive bands. Integer-centre sampling
       * never crosses a band edge; changing the clamp per band would defeat batching. */
      q->sampling=state.samplers[0];
      for(unsigned corner=0;corner<4u;corner++){
        unsigned right=corner==1u || corner==2u,bottom=corner>=2u;
        q->x[corner]=dx+(right?length:0u);q->y[corner]=dy+(bottom?rows:0u);
        q->u[corner]=(double)(sx+(right?length:0u))/source->width;
        q->v[corner]=(double)(sy+(bottom?rows:0u))/source->height;
        q->corner_colour[corner]=0xFFFFFFFFu;
      }
      begin+=length*rows;
    }
    GmlRenderDrawBatch batch={operations,count,&state,1u};
    if(gml_render_resource_submit_deferred(target,device,&batch,source,1u)!=
       GML_RENDER_RESOURCE_EXEC_DEVICE){ free(operations);return 0; }
  }
  free(operations);
  return 1;
}

int gml_render_base_resource_resize(GmlRender *r,int width,int height){
  const GmlRenderDeviceServices *device=gml_render_device(r);
  GmlRenderResourceSource active={0},history={0},fresh={0},staged={0};
  GmlRenderResource *next=NULL,*next_history=NULL;
  uint32_t *history_pixels=NULL;
  size_t old_count,new_count,retained;
  unsigned hw,hh;
  if(!r || !r->base_scene_resource || width<=0 || height<=0 ||
     width>4096 || height>4096 || !device || !device->observe_on_demand ||
     !device->observe_on_demand(device->context)) return 0;
  old_count=(size_t)r->base_scene_resource_width*r->base_scene_resource_height;
  new_count=(size_t)width*height;
  retained=old_count>r->base_history_count?old_count:r->base_history_count;
  /* CPU-current storage without a retained GPU tail keeps the established cheap host path. */
  if(!r->base_history_resource &&
     gml_render_resource_current_cpu_source_view(r->base_scene_resource,&active)) return 0;
  if(!gml_render_resource_device_source_view(r->base_scene_resource,device,&active) &&
     !gml_render_resource_current_cpu_source_view(r->base_scene_resource,&active)) return 0;
  if(r->base_history_resource && old_count<retained &&
     !gml_render_resource_device_source_view(r->base_history_resource,device,&history) &&
     !gml_render_resource_current_cpu_source_view(r->base_history_resource,&history)) return 0;
  if(retained<new_count) retained=new_count;
  hw=(unsigned)(retained<2048u?retained:2048u);
  hh=(unsigned)((retained+hw-1u)/hw);
  history_pixels=calloc((size_t)hw*hh,sizeof *history_pixels);
  if(!history_pixels) return 0;
  next_history=base_stage_create(history_pixels,(int)hw,(int)hh,1u);
  /* Failed submission can checkpoint a staged prefix. Never lend it the live CPU allocation. */
  next=base_stage_create(history_pixels,width,height,0u);
  if(!next_history || !next || !base_stage_clear(next_history,device,hw,hh) ||
     !base_stage_copy(next_history,device,hw,hh,&active,0u,old_count)) goto failed;
  if(old_count<r->base_history_count &&
     !base_stage_copy(next_history,device,hw,hh,&history,old_count,r->base_history_count)) goto failed;
  size_t previous=old_count>r->base_history_count?old_count:r->base_history_count;
  if(previous<new_count){
    fresh=(GmlRenderResourceSource){.image={GML_RENDER_RESOURCE_SCREEN,2u,
      gml_render_scene_incarnation_allocate(),1u},.width=(uint32_t)width,
      .height=(uint32_t)height,.pixel_format=GML_RENDER_PIXEL_ARGB8888,
      .stride_bytes=(uint32_t)width*4u,.byte_capacity=new_count*4u,
      .bytes=r->base_scene_resource_pixels};
    if(!fresh.image.incarnation ||
       !base_stage_copy(next_history,device,hw,hh,&fresh,previous,new_count)) goto failed;
    if(device->retire) device->retire(device->context,fresh.image);
    fresh.image.incarnation=0u;
  }
  if(!gml_render_resource_device_source_view(next_history,device,&staged) ||
     !base_stage_clear(next,device,(unsigned)width,(unsigned)height) ||
     !base_stage_copy(next,device,(unsigned)width,(unsigned)height,&staged,0u,new_count) ||
     !gml_render_resource_rebind_resident_pixels(next,r->base_scene_resource_pixels)) goto failed;
  /* No externally visible owner changes until both complete staging images are valid. */
  base_stage_dispose(device,r->base_scene_resource);
  base_stage_dispose(device,r->base_history_resource);
  free(r->base_history_pixels);
  r->base_scene_resource=next;
  r->base_scene_resource_width=width;r->base_scene_resource_height=height;
  r->base_history_resource=next_history;r->base_history_pixels=history_pixels;
  r->base_history_count=retained;
  gml_render_resource_bind_dispatch(next,r);
  gml_render_resource_bind_dispatch(next_history,r);
  return 1;
failed:
  if(fresh.image.incarnation && device->retire) device->retire(device->context,fresh.image);
  base_stage_dispose(device,next);base_stage_dispose(device,next_history);
  free(history_pixels);
  return 0;
}

int gml_render_masked_bases_observe(GmlRender *r){
  if(!r) return 0;
  GmlRenderResource *resources[]={r->base_scene_resource,r->retained_base_resource,r->view_base_resource};
  int masked[]={r->base_scene_resource_masked,r->retained_base_masked,r->view_base_masked};
  for(unsigned i=0;i<3u;i++){
    if(!resources[i] || !masked[i]) continue;
    if(!gml_render_base_history_observe(r) ||
       !gml_render_resource_acquire_cpu_read(resources[i],NULL)) return 0;
  }
  return 1;
}

static unsigned outside_mask_regions(GmlRenderRect regions[4],int width,int height,
                                     int x,int y,int inside_width,int inside_height){
  unsigned count=0;
  int64_t left=x,top=y,right=(int64_t)x+inside_width,bottom=(int64_t)y+inside_height;
  if(left<0) left=0;
  if(top<0) top=0;
  if(right>width) right=width;
  if(bottom>height) bottom=height;
  if(inside_width<=0 || inside_height<=0 || right<=left || bottom<=top){
    regions[count++]=(GmlRenderRect){0,0,(uint32_t)width,(uint32_t)height};
  }else{
    if(top>0) regions[count++]=(GmlRenderRect){0,0,(uint32_t)width,(uint32_t)top};
    if(bottom<height) regions[count++]=(GmlRenderRect){0,(int)bottom,(uint32_t)width,(uint32_t)(height-bottom)};
    if(left>0) regions[count++]=(GmlRenderRect){0,(int)top,(uint32_t)left,(uint32_t)(bottom-top)};
    if(right<width) regions[count++]=(GmlRenderRect){(int)right,(int)top,(uint32_t)(width-right),(uint32_t)(bottom-top)};
  }
  return count;
}

static GmlRenderResourceExecution submit_outside_mask(GmlRender *r,
  GmlRenderResource *resource,const GmlRenderDeviceServices *device,int width,int height,
  const GmlRenderRect *regions,unsigned count,int cpu_only){
  GmlRenderDrawOp operations[4]={0};
  GmlRenderImageRef image=gml_render_resource_image_ref(resource);
  if(!count || count>4u || image.content_version>UINT64_MAX-count)
    return GML_RENDER_RESOURCE_EXEC_FAILED;
  for(unsigned i=0;i<count;i++){
    operations[i].opcode=cpu_only?GML_RENDER_OP_COLOURED_PRIMITIVE:GML_RENDER_OP_CLEAR;
    operations[i].scene_family=GML_RENDER_SCENE_PRIMITIVE;
    operations[i].reconstruction_mode=cpu_only?GML_RENDER_REPLAY_EXACT:GML_RENDER_DEVICE_CHECKPOINT;
    operations[i].target=(GmlRenderTargetRef){image,1u,(uint32_t)width,(uint32_t)height,
      GML_RENDER_PIXEL_ARGB8888,cpu_only?(GmlRenderRect){0,0,(uint32_t)width,(uint32_t)height}:regions[i]};
    operations[i].target.image.content_version+=i+1u;
    if(cpu_only){
      operations[i].payload.primitive.kind=GML_RENDER_PRIMITIVE_FLAT;
      operations[i].payload.primitive.region=regions[i];
      operations[i].payload.primitive.colour=0xFF000000u;
    }else operations[i].payload.clear_colour=0xFF000000u;
  }
  GmlRenderDrawBatch batch={operations,count,NULL,0u};
  GmlRenderResourceExecution result=gml_render_resource_submit_deferred(resource,device,&batch,NULL,0u);
  if(result==GML_RENDER_RESOURCE_EXEC_DEVICE || (cpu_only && result==GML_RENDER_RESOURCE_EXEC_CPU))
    gml_render_dispatch_batch_record(r,&batch,result);
  return result;
}

/* The coordinator uses this only when a GPU-owned world replaces a CPU root
 * which would be masked before its existing full-raster application copy.
 * Explicit application canvases retain their independent root-only mask. */
int gml_render_application_surface_owned_mask(GmlRender *r,int width,int height,
                                               int x,int y,int inside_width,int inside_height){
  const GmlRenderDeviceServices *device=gml_render_device(r);
  GmlRenderRect regions[4];
  if(!r || width<=0 || height<=0 || width>4096 || height>4096 ||
     !r->scene_device_enabled ||
     r->target_id!=-1 || r->target_sp || r->gui_pass_active ||
     !r->app_surface_owned || r->fb!=r->app_surface_owned ||
     r->app_w!=width || r->app_h!=height || r->fbw!=width || r->fbh!=height ||
     !device || !device->context ||
     !gml_render_application_scene_flush(r)) return 0;
  unsigned count=outside_mask_regions(regions,width,height,x,y,inside_width,inside_height);
  if(!count) return 1;
  int cpu_only=!device->observe_on_demand || !device->observe_on_demand(device->context);
  /* Legacy recording services have exact replay, not a current-context
   * witness. Observe their existing source and reuse the CPU flat rectangle;
   * never turn an ordinary mask into an error or call an unowned context. */
  if(cpu_only && !gml_render_application_surface_owned_view(r,NULL)) return 0;
  GmlRenderResource *resource=gml_render_application_resource_ensure(r);
  if(!resource || submit_outside_mask(r,resource,cpu_only?NULL:device,width,height,regions,count,cpu_only)!=
                  (cpu_only?GML_RENDER_RESOURCE_EXEC_CPU:GML_RENDER_RESOURCE_EXEC_DEVICE)) return 0;
  r->app_surface_opaque=0;r->app_presentation_coverage_active=0;
  r->fb_opaque_known=0;r->fb_all_transparent=0;
  return 1;
}

int gml_render_completed_screen_mask(GmlRender *r,uint32_t *pixels,int width,int height,
                                     int x,int y,int inside_width,int inside_height){
  const GmlRenderDeviceServices *device=gml_render_device(r);
  GmlRenderResource *resource;
  GmlRenderRect regions[4];
  int previous_width=0,previous_height=0,created=0;
  if(!r || !pixels || width<=0 || height<=0 || width>4096 || height>4096 ||
     !r->scene_device_enabled || !r->resident_application_pass ||
     !device || !device->observe_on_demand ||
     !device->observe_on_demand(device->context) ||
     !gml_render_application_scene_flush(r)) return 0;
  if(pixels==r->app_surface_owned) return 0;
  for(int i=0;i<GML_MAX_SURFACES;i++)
    if(r->surface[i].live && pixels==r->surface[i].px) return 0;
  unsigned count=outside_mask_regions(regions,width,height,x,y,inside_width,inside_height);
  if(!count) return 1;
  resource=gml_render_base_resource_lookup(r,pixels,&previous_width,&previous_height);
  if(resource && (previous_width!=width || previous_height!=height)){
    int swapped=resource!=r->base_scene_resource;
    uint32_t *previous=r->base_scene_resource_pixels;
    if(swapped && gml_render_base_resource_select(r,pixels)!=1) return 0;
    int resized=gml_render_base_resource_resize(r,width,height);
    if(swapped && gml_render_base_resource_select(r,previous)!=1) return 0;
    if(!resized) return 0;
    resource=gml_render_base_resource_lookup(r,pixels,NULL,NULL);
  }
  if(!resource){
    /* A third borrowed allocation is not an alias of either existing image. */
    if(r->retained_base_resource) return 0;
    resource=base_stage_create(pixels,width,height,0u);
    if(!resource) return 0;
    gml_render_resource_bind_dispatch(resource,r);created=1;
  }
  GmlRenderResourceExecution result=submit_outside_mask(r,resource,device,width,height,regions,count,0);
  if(result!=GML_RENDER_RESOURCE_EXEC_DEVICE) goto rejected;
  if(created){
    r->retained_base_resource=resource;r->retained_base_pixels=pixels;
    r->retained_base_width=width;r->retained_base_height=height;
    r->retained_base_masked=1;
  }
  if(resource==r->base_scene_resource) r->base_scene_resource_masked=1;
  if(resource==r->retained_base_resource) r->retained_base_masked=1;
  if(resource==r->view_base_resource) r->view_base_masked=1;
  return 1;
rejected:
  if(created) base_stage_dispose(device,resource);
  return 0;
}
GmlRenderResource *gml_render_scene_target_resource_ensure(GmlRender *r){
  GmlRenderImageRef image;
  uint64_t incarnation;
  if(!gml_render_scene_target_matches(r)) return NULL;
  if(r->classic_phase_scene_target){
    if(!r->classic_phase_resource || r->target_sp || r->target_id!=-1 ||
       r->fb!=r->base_fb || r->classic_phase_y!=r->classic_phase_resource_pixels ||
       r->fbw!=r->classic_phase_resource_width || r->fbh!=r->classic_phase_resource_height ||
       !same_scene_image(gml_render_resource_image_ref(r->base_scene_resource),
                         r->classic_phase_base_image)) return NULL;
    return r->classic_phase_resource;
  }
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

int gml_render_classic_phase_resource_bind(GmlRender *r){
  const GmlRenderDeviceServices *device=gml_render_device(r);
  if(!r || !r->classic || r->interp || !r->classic_phase_y ||
     r->classic_phase_y==r->base_fb || r->fbw<=0 || r->fbw>4096 || r->fbh<=0 || r->fbh>4096 ||
     r->classic_phase_scene_target || !r->resident_application_pass ||
     r->target_id!=-1 || r->target_sp || r->fb!=r->base_fb ||
     !device || !device->observe_on_demand || !device->observe_on_demand(device->context)) return 0;
  for(unsigned index=0;index<3u;index++)
    if(r->classic_interp_phase[index]) return 0;
  /* Distinct pointers can still be views into one allocation. Such an alias
   * cannot own two independently versioned targets or be reordered by plane. */
  uintptr_t phase_begin=(uintptr_t)r->classic_phase_y,base_begin=(uintptr_t)r->base_fb;
  size_t bytes=(size_t)r->fbw*(size_t)r->fbh*sizeof(uint32_t);
  if((phase_begin<base_begin?base_begin-phase_begin:phase_begin-base_begin)<bytes) return 0;
  GmlRenderResource *base=gml_render_scene_target_resource_ensure(r);
  if(!base || base!=r->base_scene_resource) return 0;
  GmlRenderImageRef image=gml_render_resource_image_ref(base);
  if(r->classic_phase_resource){
    /* Never retire a differently bound borrow implicitly: its owner may still
     * need unobserved pixels before resizing or replacing the allocation. */
    return r->classic_phase_resource_pixels==r->classic_phase_y &&
      r->classic_phase_resource_width==r->fbw && r->classic_phase_resource_height==r->fbh &&
      same_scene_image(image,r->classic_phase_base_image);
  }
  uint64_t incarnation=gml_render_scene_incarnation_allocate();
  if(!incarnation) return 0;
  GmlRenderImageRef phase={GML_RENDER_RESOURCE_SCRATCH,0u,incarnation,1u};
  GmlRenderResource *resource=gml_render_resource_create(phase,(uint32_t)r->fbw,
    (uint32_t)r->fbh,GML_RENDER_PIXEL_ARGB8888,r->classic_phase_y,(GmlRenderResourceLimits){0});
  if(!resource) return 0;
  gml_render_resource_bind_dispatch(resource,r);
  r->classic_phase_resource=resource;r->classic_phase_base_image=image;
  r->classic_phase_resource_pixels=r->classic_phase_y;
  r->classic_phase_resource_width=r->fbw;r->classic_phase_resource_height=r->fbh;
  r->classic_phase_opaque_known=r->classic_phase_all_opaque=r->classic_phase_all_transparent=0;
  return 1;
}

int gml_render_classic_phase_resource_observe(GmlRender *r){
  if(!r || !gml_render_application_scene_flush(r)) return 0;
  return !r->classic_phase_resource ||
    gml_render_resource_observe_device(r->classic_phase_resource,gml_render_device(r));
}

int gml_render_classic_phase_cpu_barrier(GmlRender *r){
  if(!r) return 0;
  if(!r->classic_phase_resource || !r->classic_phase_y || r->target_sp || r->fb!=r->base_fb)
    return 1;
  if(r->classic_phase_scene_target || r->classic_phase_y!=r->classic_phase_resource_pixels ||
     r->fbw!=r->classic_phase_resource_width || r->fbh!=r->classic_phase_resource_height ||
     !gml_render_application_scene_flush(r)) return 0;
  uint32_t *pixels=NULL;
  if(!gml_render_resource_acquire_cpu_write(r->classic_phase_resource,&pixels) ||
     pixels!=r->classic_phase_y ||
     !gml_render_resource_commit_cpu_write(r->classic_phase_resource)) return 0;
  r->classic_phase_opaque_known=r->classic_phase_all_opaque=r->classic_phase_all_transparent=0;
  return 1;
}

void gml_render_classic_phase_resource_dispose(GmlRender *r){
  if(!r || !r->classic_phase_resource) return;
  if(r->pending_scene_batch && r->pending_scene_batch->preparing)
    (void)gml_render_application_scene_prepare_finish(r,0);
  (void)gml_render_application_scene_flush(r);
  if(r->pending_scene_batch)
    scene_remove_target(r->pending_scene_batch,gml_render_resource_image_ref(r->classic_phase_resource));
  const GmlRenderDeviceServices *device=gml_render_device(r);
  if(device && device->retire)
    device->retire(device->context,gml_render_resource_image_ref(r->classic_phase_resource));
  gml_render_resource_destroy(r->classic_phase_resource);
  r->classic_phase_resource=NULL;r->classic_phase_resource_pixels=NULL;
  r->classic_phase_base_image=(GmlRenderImageRef){0};
  r->classic_phase_resource_width=r->classic_phase_resource_height=0;
  r->classic_phase_opaque_known=r->classic_phase_all_opaque=r->classic_phase_all_transparent=0;
}

int gml_render_sample_storage_prepare(GmlRender *r,uint32_t *vertical,
                                      int width,int height){
  if(!r) return 0;
  if(!r->classic_phase_resource) return 1;
  int same_base=(r->base_scene_resource &&
    same_scene_image(gml_render_resource_image_ref(r->base_scene_resource),r->classic_phase_base_image)) ||
    (r->retained_base_resource &&
    same_scene_image(gml_render_resource_image_ref(r->retained_base_resource),r->classic_phase_base_image));
  if(vertical && vertical==r->classic_phase_resource_pixels &&
     width==r->classic_phase_resource_width && height==r->classic_phase_resource_height &&
     same_base) return 1;
  if(!gml_render_classic_phase_resource_observe(r)) return 0;
  uint32_t *previous=r->classic_phase_resource_pixels;
  gml_render_classic_phase_resource_dispose(r);
  if(r->classic_phase_y==previous) r->classic_phase_y=NULL;
  if(r->app_phase_y==previous) r->app_phase_y=NULL;
  return 1;
}

int gml_render_sample_planes_resident_bind(GmlRender *r){
  return gml_render_classic_phase_resource_bind(r);
}

int gml_render_classic_phase_scene_select(GmlRender *r,int vertical){
  if(!r || (vertical!=0 && vertical!=1)) return 0;
  if(r->classic_phase_scene_target==vertical) return 1;
  if(!r->pending_scene_batch || !r->pending_scene_batch->preparing) return 0;
  if(vertical && (!r->classic_phase_resource || !gml_render_classic_phase_resource_bind(r))) return 0;
  int known=r->fb_opaque_known,opaque=r->fb_all_opaque,transparent=r->fb_all_transparent;
  r->fb_opaque_known=r->classic_phase_opaque_known;
  r->fb_all_opaque=r->classic_phase_all_opaque;
  r->fb_all_transparent=r->classic_phase_all_transparent;
  r->classic_phase_opaque_known=known;r->classic_phase_all_opaque=opaque;
  r->classic_phase_all_transparent=transparent;
  r->classic_phase_scene_target=vertical;
  return 1;
}

int gml_render_resident_classic_phase_clear(GmlRender *r,uint32_t colour){
  if(!r || !r->classic_phase_resource || r->classic_phase_scene_preparing ||
     r->pending_fill || r->pending_underlay || r->pending_presentation ||
     !gml_render_classic_phase_resource_bind(r)) return 0;
  GmlRenderPendingSceneBatch *pending=r->pending_scene_batch;
  if(pending && pending->preparing) return 0;
  GmlRenderResource *targets[2]={r->base_scene_resource,r->classic_phase_resource};
  GmlRenderImageRef images[2];
  for(unsigned plane=0;plane<2u;plane++){
    images[plane]=gml_render_application_scene_next_image_ref(r,targets[plane]);
    if(images[plane].content_version==UINT64_MAX) return 0;
  }
  /* A clear observes draw order, not predecessor bytes. Publish any queued
   * prefix before replacing both images; a failed peer remains retryable. */
  if(!gml_render_application_scene_flush(r)) return 0;
  pending=r->pending_scene_batch;
  if(!pending){
    pending=calloc(1,sizeof *pending);
    if(!pending) return 0;
    r->pending_scene_batch=pending;
  }
  if(!scene_pending_reserve(pending,2u)) return 0;
  for(unsigned plane=0;plane<2u;plane++){
    GmlRenderDrawOp operation={0};
    operation.opcode=GML_RENDER_OP_CLEAR;
    operation.reconstruction_mode=GML_RENDER_DEVICE_CHECKPOINT;
    operation.target=(GmlRenderTargetRef){images[plane],1u,
      (uint32_t)r->fbw,(uint32_t)r->fbh,GML_RENDER_PIXEL_ARGB8888,
      {0,0,(uint32_t)r->fbw,(uint32_t)r->fbh}};
    operation.target.image.content_version++;
    operation.payload.clear_colour=colour;
    pending->operations[plane]=operation;
  }
  pending->count=2u;pending->phase_count=1u;
  pending->source_count=pending->state_count=0u;pending->coalesce_sources=0;
  r->fb_opaque_known=r->classic_phase_opaque_known=1;
  r->fb_all_opaque=r->classic_phase_all_opaque=(colour>>24)==255u;
  r->fb_all_transparent=r->classic_phase_all_transparent=(colour>>24)==0u;
  if(r->app_surface && r->fb==r->app_surface) r->app_presentation_coverage_active=0;
  return 1;
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
static int resident_surface_nearest_function(GmlRender *r,int surf,double dx,double dy,
  double dw,double dh,uint32_t colour,double alpha,int automatic);

static int resident_classic_phase_present(GmlRender *r,
  double dx,double dy,double dw,double dh,uint32_t colour,double alpha,int automatic){
  const GmlRenderDeviceServices *device=gml_render_device(r);
  if(!r || !r->classic || r->interp || !r->classic_phase_resource ||
     !r->resident_application_pass || !gml_render_scene_target_matches(r) ||
     !r->app_surface || r->app_surface==r->fb || !r->app_phase_y ||
     r->app_phase_y!=r->classic_phase_resource_pixels ||
     r->app_w!=r->classic_phase_resource_width || r->app_h!=r->classic_phase_resource_height ||
     r->classic_phase_scene_target || r->classic_phase_scene_preparing ||
     r->pending_underlay || r->pending_presentation || r->active_shader>=0 ||
     (!automatic && (r->fog_flat || gml_d3_is_active(r))) || r->alpha_test_enable ||
     (r->blendmode!=0 && r->blendmode!=6) || (colour&0xFFFFFFu)!=0xFFFFFFu ||
     !isfinite(alpha) || alpha<1.0 || !isfinite(dx) || !isfinite(dy) ||
     !isfinite(dw) || !isfinite(dh) ||
     fabs(dw-2.0*r->app_w)>.001 || fabs(dh-2.0*r->app_h)>.001 ||
     !device || !device->observe_on_demand || !device->observe_on_demand(device->context)) return 0;
  int sw=0,sh=0;
  GmlRenderResource *root=gml_render_base_resource_lookup(r,r->app_surface,&sw,&sh);
  if(!root || sw!=r->app_w || sh!=r->app_h || sw<=0 || sh<=0 ||
     !same_scene_image(gml_render_resource_image_ref(root),r->classic_phase_base_image)) return 0;
  double left=floor(dx-r->cam_x),top=floor(dy-r->cam_y);
  if(!isfinite(left) || !isfinite(top) || fabs(left)>16384.0 || fabs(top)>16384.0) return 0;
  if(left>=r->fbw || top>=r->fbh || left+2.0*sw<=0.0 || top+2.0*sh<=0.0) return 1;
  uint32_t capacity=4u*(uint32_t)sh;
  if(r->classic_phase_composition_capacity<capacity){
    GmlRenderDrawOp *next=realloc(r->classic_phase_composition,(size_t)capacity*sizeof *next);
    if(!next) return 0;
    r->classic_phase_composition=next;r->classic_phase_composition_capacity=capacity;
  }
  gml_render_flush_pending_fill(r);
  if(r->pending_fill || !gml_render_application_scene_flush(r)) return 0;
  GmlRenderResource *target=gml_render_scene_target_resource_ensure(r);
  if(!target || target==root || target==r->classic_phase_resource) return 0;
  GmlRenderResourceSource sources[2]={0};
  GmlRenderResource *resources[2]={root,r->classic_phase_resource};
  GmlRenderImageRef refs[2];
  for(unsigned side=0;side<2u;side++){
    if((!gml_render_resource_device_source_view(resources[side],device,&sources[side]) &&
        !gml_render_resource_current_cpu_source_view(resources[side],&sources[side])) ||
       sources[side].pixel_format!=GML_RENDER_PIXEL_ARGB8888 ||
       sources[side].width!=(uint32_t)sw || sources[side].height!=(uint32_t)sh) return 0;
    refs[side]=sources[side].image;
  }
  GmlRenderImageRef image=gml_render_application_scene_next_image_ref(r,target);
  if(image.content_version>UINT64_MAX-capacity) return 0;
  GmlRenderDrawStateSnapshot state={.colour=0xFFFFFFFFu,.alpha=255u,
    .write_mask=15u,.sampler_count=1u};
  state.samplers[0]=(GmlRenderSamplingPolicy){.filter=GML_RENDER_FILTER_NEAREST,
    .edge_rule=GML_RENDER_EDGE_CLAMP_SUBIMAGE,.source_region={0,0,(uint32_t)sw,(uint32_t)sh}};
  uint32_t count=0u;
  /* Interior pixels are raw copies, including zero/partial alpha. The CPU's
   * final column has a missing quarter-colour tap but keeps source alpha.
   * Reuse the qualified constant tint there; only that filtered fringe is tolerant. */
  for(unsigned side=0;side<2u;side++) for(int row=0;row<sh;row++){
    double y=top+2.0*row+side;
    if(y<0.0 || y>=r->fbh) continue;
    for(unsigned fringe=0;fringe<2u;fringe++){
      double x0=fringe?left+2.0*sw-1.0:fmax(left,0.0);
      double x1=fringe?x0+1.0:fmin(left+2.0*sw-1.0,r->fbw);
      if(x0<0.0 || x0>=r->fbw || x1<=x0) continue;
      GmlRenderDrawOp *op=&r->classic_phase_composition[count];
      memset(op,0,sizeof *op);
      op->opcode=GML_RENDER_OP_TEXTURED_QUAD;op->scene_family=GML_RENDER_SCENE_SURFACE;
      op->reconstruction_mode=GML_RENDER_DEVICE_CHECKPOINT;
      op->sources=&refs[side];op->source_count=1u;
      op->target=(GmlRenderTargetRef){image,1u,(uint32_t)r->fbw,(uint32_t)r->fbh,
        GML_RENDER_PIXEL_ARGB8888,{0,0,(uint32_t)r->fbw,(uint32_t)r->fbh}};
      op->target.image.content_version+=count+1u;
      GmlRenderQuad *q=&op->payload.quad;
      q->shape=GML_RENDER_QUAD_AXIS_ALIGNED;q->sampling=state.samplers[0];q->filtered_draw_alpha=1.0;
      for(unsigned corner=0;corner<4u;corner++){
        q->x[corner]=corner==1u || corner==2u?x1:x0;
        q->y[corner]=corner>=2u?y+1.0:y;
        q->u[corner]=fringe?((double)sw-.5)/sw:(q->x[corner]-left+1.0)/(2.0*sw);
        q->v[corner]=((double)row+.5)/sh;
        q->corner_colour[corner]=fringe?0xFFBFBFBFu:0xFFFFFFFFu;
      }
      count++;
    }
  }
  if(!count) return 1;
  GmlRenderDrawBatch batch={r->classic_phase_composition,count,&state,1u};
  if(gml_render_application_scene_submit(r,&batch,sources,2u)!=GML_RENDER_RESOURCE_EXEC_DEVICE) return 0;
  r->fb_opaque_known=r->fb_all_opaque=r->fb_all_transparent=0;
  return 1;
}

int gml_render_resident_classic_phase_present(GmlRender *r,
  double dx,double dy,double dw,double dh,uint32_t colour,double alpha){
  return resident_classic_phase_present(r,dx,dy,dw,dh,colour,alpha,0);
}

static int resident_classic_sample_underlay(GmlRender *r){
  const GmlRenderDeviceServices *device=gml_render_device(r);
  if(!r || !r->classic || !r->win ||
     !anygm_policy_classic_modern_presentation(r->win) || !r->pending_underlay ||
     !r->resident_application_pass || !gml_render_scene_target_matches(r) ||
     !device || !device->context || !device->observe_on_demand ||
     !device->observe_on_demand(device->context) ||
     r->fb!=r->base_fb || r->target_sp || r->target_id>=0 ||
     !r->app_surface || r->app_surface==r->fb || r->pending_presentation ||
     r->rotated_batch_count || r->rotated_batch_building ||
     r->active_shader>=0 ||
     r->blendmode || r->blend_equation!=1 || r->blend_equation_alpha!=1 || !r->alphablend ||
     (r->alpha_test_enable && r->alpha_test_ref) ||
     r->fbw<=0 || r->fbh<=0 || r->fbw>4096 || r->fbh>4096 ||
     r->color_write_mask!=15u || r->app_w<=0 || r->app_h<=0 ||
     r->app_w>4096 || r->app_h>4096 || r->underlay_x < -16384 || r->underlay_x>16384 ||
     r->underlay_y < -16384 || r->underlay_y>16384) return 0;
  unsigned planes=1u,step=1u;
  if(r->underlay_w!=r->app_w || r->underlay_h!=r->app_h){
    if(!r->interp || r->app_w>2048 || r->app_h>2048 ||
       r->underlay_w!=r->app_w*2 || r->underlay_h!=r->app_h*2 ||
       !r->app_interp_phase[0] || !r->app_interp_phase[1] || !r->app_interp_phase[2]) return 0;
    planes=4u;step=2u;
  }
  GmlRenderResourceSource current={0};
  int width=0,height=0;
  GmlRenderResource *origin=r->app_surface==r->app_surface_owned
    ?r->app_surface_resource:gml_render_base_resource_lookup(r,r->app_surface,&width,&height);
  /* Never upload a stale CPU shadow of a device-owned logical root. The existing
   * resident phase owner remains first choice; this branch borrows CPU-current planes. */
  if(origin && (!gml_render_resource_current_cpu_source_view(origin,&current) ||
                current.pixel_format!=GML_RENDER_PIXEL_ARGB8888 ||
                current.width!=(uint32_t)r->app_w || current.height!=(uint32_t)r->app_h)) return 0;
  const uint32_t *pixels[4]={r->app_surface,r->app_interp_phase[0],
                           r->app_interp_phase[1],r->app_interp_phase[2]};
  for(unsigned plane=0;plane<planes;plane++){
    double x=r->underlay_x+(int)(plane&1u),y=r->underlay_y+(int)(plane>>1u);
    if(x>=r->fbw || y>=r->fbh || x+(double)step*r->app_w<=0.0 ||
       y+(double)step*r->app_h<=0.0) continue;
    uintptr_t input=(uintptr_t)pixels[plane],output=(uintptr_t)r->fb;
    size_t input_bytes=(size_t)r->app_w*r->app_h*4u;
    size_t output_bytes=(size_t)r->fbw*r->fbh*4u;
    if(input>UINTPTR_MAX-input_bytes || output>UINTPTR_MAX-output_bytes ||
       (input<output+output_bytes && output<input+input_bytes)) return 0;
  }
  if(!gml_render_application_scene_flush(r)) return 0;
  gml_render_flush_pending_fill(r);
  if(r->pending_fill || !r->resident_application_pass) return 0;
  GmlRenderResource *target=gml_render_scene_target_resource_ensure(r);
  if(!target) return 0;
  GmlRenderImageRef image=gml_render_resource_image_ref(target);
  if(image.content_version>UINT64_MAX-planes) return 0;
  GmlRenderResourceSource sources[4]={0};
  GmlRenderDrawOp operations[4]={0};
  GmlRenderDrawStateSnapshot state={0};
  state.write_mask=15u;state.colour=0xFFFFFFFFu;state.alpha=255u;state.sampler_count=1u;
  state.samplers[0]=(GmlRenderSamplingPolicy){GML_RENDER_FILTER_SAMPLE_PLANE_COPY,
    GML_RENDER_EDGE_CLAMP_SUBIMAGE,{0,0,(uint32_t)r->app_w,(uint32_t)r->app_h},0u,0u};
  unsigned prepared=0u;
  int accepted=0;
  for(unsigned plane=0;plane<planes;plane++){
    double anchor_x=r->underlay_x+(int)(plane&1u),anchor_y=r->underlay_y+(int)(plane>>1u);
    double left=fmax(0.0,anchor_x),top=fmax(0.0,anchor_y);
    double right=fmin(r->fbw,anchor_x+(double)step*r->app_w);
    double bottom=fmin(r->fbh,anchor_y+(double)step*r->app_h);
    if(right<=left || bottom<=top) continue;
    uint64_t incarnation=gml_render_scene_incarnation_allocate();
    if(!incarnation) goto sample_done;
    GmlRenderResourceSource *source=&sources[prepared];
    *source=(GmlRenderResourceSource){.image={GML_RENDER_RESOURCE_SCREEN,plane,incarnation,1u},
      .width=(uint32_t)r->app_w,.height=(uint32_t)r->app_h,
      .pixel_format=GML_RENDER_PIXEL_ARGB8888,.stride_bytes=(uint32_t)r->app_w*4u,
      .byte_capacity=(size_t)r->app_w*r->app_h*4u,.bytes=pixels[plane]};
    GmlRenderDrawOp *op=&operations[prepared];
    op->opcode=GML_RENDER_OP_TEXTURED_QUAD;op->scene_family=GML_RENDER_SCENE_SURFACE;
    op->reconstruction_mode=GML_RENDER_DEVICE_CHECKPOINT;
    op->target=(GmlRenderTargetRef){image,1u,(uint32_t)r->fbw,(uint32_t)r->fbh,
      GML_RENDER_PIXEL_ARGB8888,{0,0,(uint32_t)r->fbw,(uint32_t)r->fbh}};
    op->target.image.content_version+=prepared+1u;
    op->sources=&source->image;op->source_count=1u;
    GmlRenderQuad *q=&op->payload.quad;
    q->shape=GML_RENDER_QUAD_AXIS_ALIGNED;q->sampling=state.samplers[0];
    q->inverse_anchor[0]=anchor_x;q->inverse_anchor[1]=anchor_y;
    q->inverse_x[0]=q->inverse_y[1]=step;
    q->span_x[0]=planes==4u || r->app_surface_opaque;
    for(unsigned corner=0;corner<4u;corner++){
      unsigned far_x=corner==1u || corner==2u,far_y=corner>=2u;
      q->x[corner]=far_x?right:left;q->y[corner]=far_y?bottom:top;
      q->u[corner]=far_x;q->v[corner]=far_y;q->corner_colour[corner]=0xFFFFFFFFu;
    }
    prepared++;
  }
  if(prepared){
    GmlRenderDrawBatch batch={operations,prepared,&state,1u};
    GmlRenderResourceExecution result=gml_render_resource_submit_deferred(
      target,device,&batch,sources,prepared);
    gml_render_dispatch_batch_record(r,&batch,result);
    accepted=result==GML_RENDER_RESOURCE_EXEC_DEVICE;
  } else accepted=1;
sample_done:
  /* These CPU-authored sources are consumed by this immediate submission, not
   * retained as a replay recipe or a cache whose next version could be stale. */
  if(device->retire) for(unsigned plane=0;plane<prepared;plane++)
    device->retire(device->context,sources[plane].image);
  if(accepted){
    r->pending_underlay=0;
    if(prepared) r->fb_opaque_known=r->fb_all_opaque=r->fb_all_transparent=0;
  }
  return accepted;
}

int gml_render_compose_resident_underlay(GmlRender *r){
  if(r && r->classic){
    if(r->pending_underlay && r->classic_phase_resource && r->app_phase_y){
      r->pending_underlay=0;
      int accepted=resident_classic_phase_present(r,
        r->underlay_x+r->cam_x,r->underlay_y+r->cam_y,
        r->underlay_w,r->underlay_h,0xFFFFFFu,1.0,1);
      if(!accepted) r->pending_underlay=1;
      return accepted;
    }
    if(resident_classic_sample_underlay(r)) return 1;
    if(!r->resident_application_pass || !r->win ||
       anygm_policy_classic_modern_presentation(r->win) ||
       !r->pending_underlay || r->fb!=r->base_fb || r->target_sp || r->target_id>=0 ||
       r->app_phase_y || r->classic_phase_y ||
       (r->interp && (r->underlay_w!=r->app_w || r->underlay_h!=r->app_h))) return 0;
    for(unsigned i=0;i<3u;i++)
      if(r->classic_interp_phase[i] || r->app_interp_phase[i]) return 0;
    /* The internal presentation rectangle is already in raster coordinates.
     * Share the existing surface-zero point writer without requiring publication
     * as a GML resource or applying GUI/world mapping a second time. */
    r->pending_underlay=0;
    int accepted=resident_surface_nearest_function(r,0,r->underlay_x+r->cam_x,
      r->underlay_y+r->cam_y,r->underlay_w,r->underlay_h,0xFFFFFFu,1.0,1);
    if(!accepted) r->pending_underlay=1;
    return accepted;
  }
  GmlRenderResourceSource source={0};
  GmlRenderDrawOp operation={0};
  GmlRenderDrawStateSnapshot state={0};
  GmlRenderResource *target;
  GmlRenderImageRef image,source_ref;
  /* Automatic first-generation presentation forces opaque coverage, and a
   * modern opaque certificate already guarantees alpha 255. An active cutout
   * below that endpoint cannot change this composition. Keep legacy recipes
   * and uncertified partial sources behind their established state fence. */
  int opaque_cutout=r && r->win && r->alpha_test_ref<255u &&
    (anygm_policy_uses_first_generation_studio(r->win) || r->app_surface_opaque) &&
    r->device.context && r->device.observe_on_demand &&
    r->device.observe_on_demand(r->device.context);
  if(!r || !r->pending_underlay || !r->resident_application_pass || !r->win ||
     r->classic || r->fb!=r->base_fb || r->fb==r->app_surface_owned ||
     !gml_render_scene_target_matches(r) || !gml_render_device(r) ||
     r->app_surface!=r->app_surface_owned ||
     r->app_presentation_coverage_active || r->pending_presentation ||
     r->rotated_batch_count || r->rotated_batch_building ||
     r->target_sp || r->target_id>=0 || r->active_shader>=0 || r->blendmode ||
     !r->alphablend || r->blend_equation!=1 || r->blend_equation_alpha!=1 ||
     (r->alpha_test_enable && !opaque_cutout) || r->color_write_mask!=15u ||
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
    } else resource=gml_render_base_resource_lookup(r,r->app_surface,NULL,NULL);
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
                                   const GmlRenderShaderGeometry *geometry,int stretched,
                                   const GmlRenderRect *selected_region);
static int resident_surface_filtered(GmlRender *r,int surf,double dx,double dy,double dw,double dh,
                                     uint32_t blend,double alpha);
/* An explicitly disabled unknown program follows the CPU's plain draw policy.
 * A requested or available content executor keeps its refusal/failure boundary. */
static int resident_surface_shader_selected(const GmlRender *r){
  return r && r->active_shader>=0 &&
    (r->shader_device_present || r->shader_device_expected ||
     !gml_render_shader_cpu_passthrough(r));
}
typedef struct {
  double x,y,width,height;
} ResidentSurfaceSelection;
static int resident_surface_nearest_selection(GmlRender *r,int surf,double dx,double dy,
  double dw,double dh,uint32_t colour,double alpha,int automatic,
  const ResidentSurfaceSelection *selection,int resolved_canvas);
static int resident_surface_filtered_resolved(GmlRender *r,int surf,double dx,double dy,
  double dw,double dh,uint32_t blend,double alpha,int resolved_canvas);

static int recover_classic_empty_screen(GmlRender *r,uint32_t *pixels,int width,int height,
                                        double x,double y,double w,double h,int zero_camera){
  const GmlRenderDeviceServices *device=gml_render_device(r);
  if(!r || !r->win || anygm_policy_classic_modern_presentation(r->win) ||
     !r->resident_application_pass || !gml_render_scene_target_matches(r) ||
     r->fb!=pixels || r->fbw!=width || r->fbh!=height || r->target_sp || r->target_id!=-1 ||
     !r->app_surface_published || !r->app_surface || r->app_surface==pixels ||
     r->app_w<=0 || r->app_h<=0 || r->app_w>4096 || r->app_h>4096 ||
     r->app_surface==r->app_surface_owned || w!=r->app_w || h!=r->app_h ||
     r->interp || r->aspect_fullwidth || r->world_transform_active ||
     gml_render_gui_transform_active(r) || r->cam_x!=0.0 || r->cam_y!=0.0 ||
     zero_camera || r->pending_fill || r->pending_underlay || r->pending_presentation ||
     r->active_shader>=0 || !r->alphablend || r->blendmode || r->alpha_test_enable ||
     r->color_write_mask!=15u || r->blend_equation!=1 || r->blend_equation_alpha!=1 ||
     gml_d3_is_active(r) || !isfinite(x) || !isfinite(y) ||
     fabs(x)>16384.0 || fabs(y)>16384.0 ||
     !device || !device->observe_on_demand || !device->observe_on_demand(device->context) ||
     !device->has_rgb) return 0;
  int has_rgb=0;
  if(!gml_render_completed_screen_has_rgb(r,pixels,width,height,&has_rgb)) return 0;
  if(has_rgb) return 1;
  int sw=0,sh=0;
  const uint32_t *source=gml_surface_pixels_read(r,0,&sw,&sh);
  if(!source || sw!=r->app_w || sh!=r->app_h) return 0;
  size_t count=(size_t)sw*(size_t)sh;
  for(size_t index=0;index<count;index++) if(source[index]&0xFFFFFFu){
    /* This is the engine's white unit recovery, not a new public surface-zero
     * variant. Keep its existing empty/source observations and exact compositor. */
    gml_render_set_pending_underlay(r,(int)floor(x),(int)floor(y),(int)w,(int)h);
    if(gml_render_compose_resident_underlay(r)) return 1;
    r->pending_underlay=0;
    return 0;
  }
  return 1;
}

int gml_render_recover_empty_screen(GmlRender *r,uint32_t *pixels,int width,int height,
                                     double x,double y,double w,double h,int zero_camera){
  if(r && r->classic)
    return recover_classic_empty_screen(r,pixels,width,height,x,y,w,h,zero_camera);
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

/* Resolve the existing CPU canvas geometry before selecting a device executor.
 * A world composite re-enters the public part mapper in the CPU owner; retain
 * that second mapping rather than inventing a different presentation policy. */
static int resident_surface_canvas(GmlRender *r,int surf,double dx,double dy,
                                    double dw,double dh,uint32_t colour,double alpha){
  if(!r || !r->resident_application_pass || r->surface_canvas<=0 ||
     (surf!=r->surface_canvas && r->target_id!=r->surface_canvas)) return 0;
  int source_width=gml_surface_width(r,surf),source_height=gml_surface_height(r,surf);
  if(source_width<=0 || source_height<=0) return 0;
  int present=surf==r->surface_canvas && r->target_sp==0 && r->target_id<0 &&
    fabs(dx)<.001 && fabs(dy)<.001 && dw>=source_width && dh>=source_height &&
    fabs(dw*source_height-dh*source_width)<.001;
  int explicit_raster=present || gml_render_surface_draw_targets_screen_raster(r,surf,dw,dh);
  if(!explicit_raster){gml_render_draw_map_point(r,&dx,&dy);gml_render_draw_map_scale(r,&dw,&dh);}
  if(present){dx=r->cam_x;dy=r->cam_y;dw=r->fbw;dh=r->fbh;}
  int world=r->target_id==r->surface_canvas && surf==0 && r->surface_canvas_width>0 &&
    fabs(dx)<.001 && fabs(dw-r->surface_canvas_width)<.001 && dh>0.0;
  if(world){dx=r->cam_x;dw=r->fbw;}
  if(world && r->surface_canvas_world_width>0){
    double sx=(double)r->surface_canvas_room_x*source_width/r->surface_canvas_world_width;
    double sw=(double)r->surface_canvas_room_width*source_width/r->surface_canvas_world_width;
    if(!(sw>0.0)) return 1;
    double x=dx+sx*dw/source_width,y=dy,xs=dw/source_width,ys=dh/source_height;
    if(!gml_render_surface_draw_targets_screen_raster(r,surf,sw*xs,source_height*ys)){
      gml_render_draw_map_point(r,&x,&y);gml_render_draw_map_scale(r,&xs,&ys);
    }
    ResidentSurfaceSelection selection={sx,0.0,sw,(double)source_height};
    if(resident_surface_nearest_selection(r,surf,x,y,sw*xs,source_height*ys,
                                          colour,alpha,0,&selection,1)) return 1;
    if(sx==0.0 && sw==source_width)
      return resident_surface_filtered_resolved(r,surf,x,y,sw*xs,source_height*ys,colour,alpha,1);
    return 0;
  }
  if(r->aspect_fullwidth && surf==0 && !r->target_sp &&
     source_width==r->aspect_wide_w && source_height==r->aspect_wide_h &&
     dw>0.0 && dh>0.0 && r->fbw>0 && r->fbh>0 &&
     dh>=(double)r->fbh*.75 && dw/dh+.01<(double)source_width/source_height){
    double scale=dh/source_height,want_width=source_width*scale;
    if(want_width<=r->fbw+.5){dw=want_width;dx=((double)r->fbw-dw)*.5+r->cam_x;}
    else{scale=(double)r->fbw/source_width;dw=r->fbw;dh=source_height*scale;
      dx=r->cam_x;dy=((double)r->fbh-dh)*.5+r->cam_y;}
  }
  return resident_surface_nearest_selection(r,surf,dx,dy,dw,dh,colour,alpha,0,NULL,1) ||
    resident_surface_filtered_resolved(r,surf,dx,dy,dw,dh,colour,alpha,1);
}

void gml_draw_surface_stretched(GmlRender *r,int surf,double dx,double dy,
                                double dw,double dh,uint32_t blend,double alpha){
  int resident_success=resident_surface_canvas(r,surf,dx,dy,dw,dh,blend,alpha);
  int source_slot=surface_slot(surf);
  if(!resident_success && r && ((source_slot>=0 && r->surface[source_slot].live) ||
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
        mapped_width,mapped_height,blend,alpha,&geometry,1,NULL);
      if(!resident_success)
        resident_success=resident_surface_filtered(r,surf,mapped_x,mapped_y,
          mapped_width,mapped_height,blend,alpha);
      if(!resident_success)
        resident_success=resident_surface_nearest_function(r,surf,mapped_x,mapped_y,
                                                          mapped_width,mapped_height,blend,alpha,0);
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

/* Ordinary point surfaces retain the CPU's floor origin and integral extent.
 * The device evaluates those integral loop ratios, rather than interpolating a
 * float phase or expanding the image into a Cartesian set of texel quads. */
static int resident_surface_nearest_function(GmlRender *r,int surf,double dx,double dy,
  double dw,double dh,uint32_t colour,double alpha,int automatic){
  if(surf==0 && r && r->classic_phase_resource && r->app_phase_y &&
     gml_render_resident_classic_phase_present(r,dx,dy,dw,dh,colour,alpha)) return 1;
  return resident_surface_nearest_selection(r,surf,dx,dy,dw,dh,colour,alpha,automatic,NULL,0);
}
static int resident_surface_nearest_selection(GmlRender *r,int surf,double dx,double dy,
  double dw,double dh,uint32_t colour,double alpha,int automatic,
  const ResidentSurfaceSelection *selection,int resolved_canvas){
  int slot=surface_slot(surf);
  const GmlRenderDeviceServices *device=gml_render_device(r);
  GmlRenderResourceSource source={0};
  GmlRenderDrawOp operation={0};
  GmlRenderDrawStateSnapshot state={0};
  GmlRenderResource *resource,*target;
  GmlRenderImageRef image,source_ref;
  GmlRenderResource *borrowed=NULL;
  int borrowed_width=0,borrowed_height=0;
  int tiled=resolved_canvas==2;
  int destination_blend=tiled && r && r->alphablend &&
    gml_render_blend_uses_destination_colour(r->blendmode);
  if(r && ((automatic && r->classic && r->win && !anygm_policy_classic_modern_presentation(r->win)) ||
           selection || resolved_canvas) &&
     surf==0 && r->app_surface!=r->app_surface_owned &&
     (borrowed=gml_render_base_resource_lookup(r,r->app_surface,&borrowed_width,&borrowed_height)) &&
     (borrowed_width!=r->app_w || borrowed_height!=r->app_h)) borrowed=NULL;
  if(!r || !r->win || !device || !device->observe_on_demand ||
     !device->observe_on_demand(device->context) ||
     (surf==0? (!automatic && !r->app_surface_published) || !r->app_surface ||
               (r->app_surface!=r->app_surface_owned && !borrowed):
                slot<0 || !r->surface[slot].live) ||
     resident_surface_shader_selected(r) ||
     /* Internal automatic presentation is already in raster coordinates and
      * bypasses the persistent world projection and flat fog. */
     (!automatic && (gml_d3_is_active(r) || r->fog_flat)) ||
     r->blendmode<0 || (tiled?r->blendmode>11:(r->blendmode>2 && r->blendmode!=6)) ||
     (destination_blend?(r->blend_equation<1 || r->blend_equation>5 ||
                        r->blend_equation_alpha<1 || r->blend_equation_alpha>5):
                       (r->blend_equation!=1 || r->blend_equation_alpha!=1)) || r->color_write_mask>15u ||
     !isfinite(dx) || !isfinite(dy) || !isfinite(dw) || !isfinite(dh) ||
     fabs(dw)>16384.0 || fabs(dh)>16384.0 || !isfinite(alpha)) return 0;
  int sw=surf==0?r->app_w:r->surface[slot].w;
  int sh=surf==0?r->app_h:r->surface[slot].h;
  if(selection && (!isfinite(selection->x) || !isfinite(selection->y) ||
      !isfinite(selection->width) || !isfinite(selection->height) ||
      fabs(selection->x)>16384.0 || fabs(selection->y)>16384.0 ||
      selection->width<=0.0 || selection->height<=0.0 ||
      selection->width>16384.0 || selection->height>16384.0)) return 0;
  int whole=!selection || (fabs(selection->x)<.001 && fabs(selection->y)<.001 &&
    fabs(selection->width-sw)<.001 && fabs(selection->height-sh)<.001);
  double left=floor(dx-r->cam_x),top=floor(dy-r->cam_y);
  int signed_width=(int)lround(dw),signed_height=(int)lround(dh);
  int width=abs(signed_width),height=abs(signed_height);
  if(sw<=0 || sh<=0 || sw>4096 || sh>4096 || !isfinite(left) || !isfinite(top) ||
     fabs(left)>16384.0 || fabs(top)>16384.0) return 0;
  if(!width || !height || alpha<=0.0 ||
     left>=r->fbw || top>=r->fbh || left+width<=0.0 || top+height<=0.0) return 1;
  int unit=whole && width==sw && height==sh && signed_width>0 && signed_height>0;
  int opaque=surface_known_opaque(r,surf);
  int requires_filter=r->alpha_test_enable && (!automatic || r->alpha_test_ref>0u);
  /* The unchanged full-source normal owner point-magnifies even with texture
   * interpolation enabled. Classic application presentation is its separate
   * leading-edge bilinear exception, not an ordinary point surface. */
  int ordinary_filtered_point=surf>0 && r->interp && r->alphablend &&
    !r->blendmode && r->color_write_mask==15u;
  int point_magnification=(selection || resolved_canvas || ordinary_filtered_point) && whole &&
    signed_width>=sw && signed_height>=sh &&
    (!r->blendmode || (r->blendmode==6 && opaque)) && alpha>=1.0 &&
    (colour&0xFFFFFFu)==0xFFFFFFu && (!requires_filter || (opaque && r->alpha_test_ref<255u)) &&
    !(r->classic && surf==0 && (!tiled || (signed_width>sw && signed_height>sh)));
  int full_normal_sampling=whole && signed_width>0 && signed_height>0 &&
    (!r->blendmode || (r->blendmode==6 && opaque)) && alpha>=1.0 &&
    (colour&0xFFFFFFu)==0xFFFFFFu && (!requires_filter || (opaque && r->alpha_test_ref<255u));
  int generic_point_magnification=tiled && width>=sw && height>=sh &&
    (r->blendmode!=0 || signed_width<0 || signed_height<0 || width==sw || height==sh) &&
    !(full_normal_sampling && r->classic && surf==0 && signed_width>sw && signed_height>sh);
  int bilinear=tiled && r->interp && signed_width>sw && signed_height>sh &&
    ((!r->blendmode && !full_normal_sampling) || (full_normal_sampling && r->classic && surf==0));
  int box=tiled && r->interp && !unit && !point_magnification && !generic_point_magnification && !bilinear;
  if((r->interp && !unit && !point_magnification && !generic_point_magnification && !box) ||
     (box && (sw>width*8 || sh>height*8)) || !r->resident_application_pass ||
     !gml_render_scene_target_matches(r) || r->pending_presentation ||
     (surf==0?r->app_surface:r->surface[slot].px)==r->fb ||
     (!selection && !resolved_canvas && r->surface_canvas>0 &&
       (surf==r->surface_canvas || r->target_id==r->surface_canvas)) ||
     (surf==0 && ((!selection && !resolved_canvas && r->aspect_fullwidth) ||
       r->app_phase_y || r->app_presentation_coverage_active))) return 0;
  for(unsigned index=0;index<3u;index++)
    if(surf==0 && (r->classic_interp_phase[index] || r->app_interp_phase[index])) return 0;
  if(!selection && !resolved_canvas && surf==0 && r->win && anygm_policy_uses_first_generation_studio(r->win) &&
     gml_render_gui_transform_active(r) && !r->target_sp && r->target_id<0 &&
     !r->interp && !r->blendmode && r->color_write_mask==15u &&
     surface_known_opaque(r,0) && alpha>=1.0 &&
     (colour&0xFFFFFFu)==0xFFFFFFu && !r->app_draw_enable) return 0;
  /* The queued application prefix precedes any ordinary surface suffix,
   * regardless of which independent source that suffix samples. */
  if(r->pending_underlay &&
     (!gml_render_gui_transform_active(r) || r->cam_x!=0.0 || r->cam_y!=0.0 ||
      !gml_render_compose_resident_underlay(r) || !r->resident_application_pass)) return 0;
  resource=surf==0?(borrowed?borrowed:gml_render_application_resource_ensure(r)):
                   gml_render_surface_resource_ensure(r,surf);
  if(!resource || (!gml_render_resource_device_source_view(resource,device,&source) &&
                  !gml_render_resource_current_cpu_source_view(resource,&source)) ||
     source.pixel_format!=GML_RENDER_PIXEL_ARGB8888) return 0;
  if(alpha>1.0) alpha=1.0;
  int fast_unit=unit && (!requires_filter || (opaque && r->alpha_test_ref<255u));
  int masked_unit=fast_unit && r->color_write_mask!=15u &&
    (r->blendmode==0 || r->blendmode==6) && alpha>=1.0 && (colour&0xFFFFFFu)==0xFFFFFFu;
  /* The unit white copy merges independent channels. The retained generic
   * point writer and its unit add/inverse branches write complete pixels. */
  unsigned write_mask=masked_unit || (tiled && (r->blendmode==3 || r->blendmode==4 ||
    r->blendmode==8 || destination_blend))?r->color_write_mask:15u;
  if(!write_mask) return 1;
  int replace=masked_unit?!r->alphablend:r->blendmode==6 || (r->classic && surf==0);
  int additive=fast_unit && r->blendmode==1;
  int inverse=fast_unit && r->blendmode==2;
  if((automatic || selection || resolved_canvas) && fast_unit && (additive || inverse)) replace=0;
  /* The early-classic normal nonunit leaf uses the unchanged scaled mapper,
   * which blends partial source coverage rather than copying its RGB. */
  if(automatic && !unit && !r->blendmode &&
     (!requires_filter || (opaque && r->alpha_test_ref<255u))) replace=0;
  int normal_unit=!r->blendmode || (tiled && (r->blendmode==5 || r->blendmode==7));
  int fast_white=fast_unit && !masked_unit && normal_unit && (colour&0xFFFFFFu)==0xFFFFFFu &&
    (alpha>=1.0 || !r->alphablend);
  /* A mapped full-source part uses the unit tint/alpha writer before the
   * Classic application's generic alphaless-copy branch. */
  if((selection || resolved_canvas) && fast_unit && !masked_unit &&
     r->classic && surf==0 && normal_unit && !fast_white) replace=0;
  int ordinary_full=whole && signed_width>0 && signed_height>0 &&
    (!r->blendmode || (r->blendmode==6 && opaque)) && alpha>=1.0 &&
    (colour&0xFFFFFFu)==0xFFFFFFu && (!requires_filter || (opaque && r->alpha_test_ref<255u));
  /* Full-source public parts and resolved canvases enter the same scaled
   * normal leaf as automatic presentation, before the Classic generic copy. */
  int classic_scaled_copy=r->classic && r->win &&
    anygm_policy_classic_modern_presentation(r->win) && r->win->classic_scaling<0;
  if((selection || resolved_canvas || (!automatic && !classic_scaled_copy)) &&
     !unit && ordinary_full) replace=0;
  int selected=selection && !fast_unit && !ordinary_full &&
    (selection->x!=0.0 || selection->y!=0.0 || selection->width!=sw || selection->height!=sh);
  gml_render_flush_pending_fill(r);
  if(r->pending_fill || !r->resident_application_pass ||
     !(target=gml_render_scene_target_resource_ensure(r))) return 0;
  image=gml_render_application_scene_next_image_ref(r,target);
  if(image.content_version==UINT64_MAX) return 0;
  source_ref=source.image;
  operation.opcode=GML_RENDER_OP_TEXTURED_QUAD;
  operation.scene_family=GML_RENDER_SCENE_SURFACE;
  operation.reconstruction_mode=GML_RENDER_DEVICE_CHECKPOINT;
  operation.sources=&source_ref;operation.source_count=1u;
  operation.target=(GmlRenderTargetRef){.image=image,.storage_version=1u,
    .width=(uint32_t)r->fbw,.height=(uint32_t)r->fbh,.pixel_format=GML_RENDER_PIXEL_ARGB8888,
    .region={0,0,(uint32_t)r->fbw,(uint32_t)r->fbh}};
  operation.target.image.content_version++;
  GmlRenderQuad *q=&operation.payload.quad;
  q->shape=GML_RENDER_QUAD_AXIS_ALIGNED;
  q->sampling=(GmlRenderSamplingPolicy){box?GML_RENDER_FILTER_BOX:GML_RENDER_FILTER_NEAREST,GML_RENDER_EDGE_CLAMP_SUBIMAGE,
    {0,0,(uint32_t)sw,(uint32_t)sh},box?GML_RENDER_QUANTISE_SURFACE_BOX_FUNCTION:
      tiled?GML_RENDER_QUANTISE_SURFACE_TILED_POINT:GML_RENDER_QUANTISE_SURFACE_NEAREST_FUNCTION,0u};
  q->filtered_draw_alpha=fast_white?1.0:alpha;
  q->x[0]=q->x[3]=left;q->x[1]=q->x[2]=left+width;
  q->y[0]=q->y[1]=top;q->y[2]=q->y[3]=top+height;
  q->inverse_anchor[0]=left;q->inverse_anchor[1]=top;
  q->inverse_x[0]=signed_width;q->inverse_y[1]=signed_height;
  int copy_source_alpha=replace && (!(r->classic && surf==0 && !masked_unit) ||
    r->blendmode==6);
  q->span_y[0]=copy_source_alpha?2.0:0.0;
  if(automatic && r->blendmode==6 && !masked_unit) q->span_y[0]=2.0;
  if(!fast_unit && !replace && !r->alphablend) q->span_y[0]=3.0;
  if(selected){
    q->span_x[2]=selection->x;q->span_x[3]=selection->y;
    q->span_y[1]=selection->width;q->span_y[2]=selection->height;
    q->span_y[3]=selection->x==floor(selection->x) && selection->y==floor(selection->y) &&
      selection->width==floor(selection->width) && selection->height==floor(selection->height)?2.0:1.0;
  }
  /* The white full-source normal leaf centre-samples reductions, assigning an
   * exact source boundary to the preceding texel. Tinted/cutout generic draws
   * retain their leading-edge indices instead. */
  if(!box && !selected && signed_width>0 && signed_height>0 &&
     (!r->blendmode || (r->blendmode==6 && opaque)) && alpha>=1.0 &&
     (colour&0xFFFFFFu)==0xFFFFFFu &&
     (!r->alpha_test_enable || (opaque && r->alpha_test_ref<255u))){
    if(width<sw) q->span_x[0]=3.0;
    if(height<sh) q->span_x[1]=3.0;
  }
  if(!box && !selected && r->classic && r->win && anygm_policy_classic_modern_presentation(r->win) &&
     r->win->classic_scaling<0 && (!r->interp || point_magnification) && signed_width>=sw && signed_height>=sh &&
     !r->blendmode && alpha>=1.0 && (colour&0xFFFFFFu)==0xFFFFFFu &&
     (!r->alpha_test_enable || (opaque && r->alpha_test_ref<255u))){
    q->span_x[0]=width%sw?1.0:2.0;
    q->span_x[1]=height%sh?1.0:2.0;
  }
  uint32_t tint=((colour&255u)<<16)|(colour&0xFF00u)|((colour>>16)&255u)|
    ((uint32_t)lround(q->filtered_draw_alpha*255.0)<<24);
  for(unsigned corner=0;corner<4u;corner++){
    q->corner_colour[corner]=tint;
    q->u[corner]=corner==0u || corner==3u?0.0:1.0;q->v[corner]=corner<2u?0.0:1.0;
  }
  state.colour=0xFFFFFFFFu;state.alpha=255u;state.write_mask=write_mask;
  state.alpha_test_enabled=!!r->alpha_test_enable;state.alpha_test_reference=r->alpha_test_ref;
  state.coverage_policy=q->span_y[0]==2.0 || masked_unit?0u:GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA;
  if(!replace && (r->alphablend || additive || inverse || fast_white || !fast_unit)){
    state.rgb_source_factor=inverse?GML_RENDER_BLEND_ZERO:GML_RENDER_BLEND_SOURCE_ALPHA;
    state.rgb_destination_factor=inverse?GML_RENDER_BLEND_ONE_MINUS_SOURCE_COLOUR:
      additive?GML_RENDER_BLEND_ONE:GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA;
    state.alpha_source_factor=inverse?GML_RENDER_BLEND_ZERO:GML_RENDER_BLEND_ONE;
    state.alpha_destination_factor=inverse?GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA:
      additive?GML_RENDER_BLEND_ONE:GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA;
    state.rgb_equation=state.alpha_equation=GML_RENDER_BLEND_ADD;
    if((additive || inverse) && r->target_sp>0) state.coverage_policy=0u;
  }
  if(tiled && (r->blendmode==3 || r->blendmode==4 || r->blendmode==8 || destination_blend)){
    int preserve=r->blendmode==3 || r->blendmode==4?r->target_sp>0:
      gml_render_target_preserves_alpha(r);
    state.rgb_equation=state.alpha_equation=GML_RENDER_BLEND_ADD;
    state.coverage_policy=preserve?0u:GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA;
    if(r->blendmode==3){
      state.rgb_source_factor=GML_RENDER_BLEND_DESTINATION_COLOUR;
      state.rgb_destination_factor=GML_RENDER_BLEND_ZERO;
      state.alpha_source_factor=preserve?GML_RENDER_BLEND_ZERO:GML_RENDER_BLEND_ONE;
      state.alpha_destination_factor=preserve?GML_RENDER_BLEND_ONE:GML_RENDER_BLEND_ZERO;
    }else if(r->blendmode==4){
      state.rgb_source_factor=GML_RENDER_BLEND_SOURCE_ALPHA;
      state.rgb_destination_factor=GML_RENDER_BLEND_ONE_MINUS_SOURCE_COLOUR;
      state.alpha_source_factor=preserve?GML_RENDER_BLEND_SOURCE_ALPHA:GML_RENDER_BLEND_ONE;
      state.alpha_destination_factor=preserve?GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA:GML_RENDER_BLEND_ZERO;
    }else if(r->blendmode==8){
      state.rgb_source_factor=GML_RENDER_BLEND_ONE_MINUS_DESTINATION_COLOUR;
      state.rgb_destination_factor=GML_RENDER_BLEND_ZERO;
      state.alpha_source_factor=preserve?GML_RENDER_BLEND_ONE_MINUS_DESTINATION_ALPHA:GML_RENDER_BLEND_ONE;
      state.alpha_destination_factor=GML_RENDER_BLEND_ZERO;
    }else{
      state.rgb_source_factor=state.alpha_source_factor=
        r->blendmode==GML_RENDER_BLEND_DEST_ALPHA_DEST_COLOUR?
          GML_RENDER_BLEND_DESTINATION_ALPHA:GML_RENDER_BLEND_DESTINATION_COLOUR;
      state.rgb_destination_factor=state.alpha_destination_factor=
        r->blendmode==GML_RENDER_BLEND_DEST_SOURCE_COLOUR?
          GML_RENDER_BLEND_SOURCE_COLOUR:GML_RENDER_BLEND_DESTINATION_COLOUR;
      state.rgb_equation=r->blend_equation==1?GML_RENDER_BLEND_ADD:
        GML_RENDER_BLEND_FIXED_MAX+(uint32_t)r->blend_equation-2u;
      state.alpha_equation=r->blend_equation_alpha==1?GML_RENDER_BLEND_ADD:
        GML_RENDER_BLEND_FIXED_MAX+(uint32_t)r->blend_equation_alpha-2u;
    }
    q->span_y[0]=(destination_blend || (r->blendmode==3 && fast_unit))?4.0:0.0;
  }
  state.samplers[0]=q->sampling;state.sampler_count=1u;
  if(!gml_render_application_scene_enqueue(r,&operation,&state,&source)) return 0;
  if(write_mask&8u){
    r->fb_opaque_known=r->fb_all_opaque=r->fb_all_transparent=0;
    if(r->fb==r->app_surface_owned) r->app_surface_opaque=r->app_presentation_coverage_active=0;
  }
  return 1;
}

/* Full-source 1:1, bounded Classic integer and filtered magnified surfaces use CPU coverage,
 * hardware sampling and fixed RGB blending. An alpha-only batch preserves written alpha. */
static int resident_surface_filtered(GmlRender *r,int surf,double dx,double dy,double dw,double dh,
                                     uint32_t blend,double alpha){
  return resident_surface_filtered_resolved(r,surf,dx,dy,dw,dh,blend,alpha,0);
}
static int resident_surface_filtered_resolved(GmlRender *r,int surf,double dx,double dy,
  double dw,double dh,uint32_t blend,double alpha,int resolved_canvas){
  int slot=surface_slot(surf);
  int source_width,source_height,unit,point_presentation,unit_gui_blend,classic_point=0;
  double left,top,width,height;
  GmlRenderResource *resource,*target;
  GmlRenderResourceSource source={0};
  GmlRenderDrawOp operation={0};
  GmlRenderDrawStateSnapshot state={0};
  GmlRenderImageRef source_ref,image;
  const GmlRenderDeviceServices *device;
  int destination_blend=r && gml_render_blend_uses_destination_colour(r->blendmode);
  int raw_replace=resolved_canvas==2 && r && r->blendmode==6 &&
    surface_known_opaque(r,surf) && alpha>=1.0 && (blend&0xFFFFFFu)==0xFFFFFFu;
  if(!r || (surf!=0 && (slot<0 || !r->surface[slot].live)) ||
     (surf==0 && (!r->app_surface_published || !r->app_surface ||
                  r->app_surface!=r->app_surface_owned)) ||
     !r->resident_scene_pass || !gml_render_scene_target_matches(r) ||
     !(device=gml_render_device(r)) || !device->observe_on_demand ||
     !device->observe_on_demand(device->context) ||
     (surf==0?r->app_surface:r->surface[slot].px)==r->fb ||
     (!resolved_canvas && r->surface_canvas>0 &&
       (surf==r->surface_canvas || r->target_id==r->surface_canvas)) ||
     r->pending_presentation ||
     resident_surface_shader_selected(r) ||
     (!r->alphablend && resolved_canvas!=2) || (r->blendmode && !destination_blend && !raw_replace) ||
     (r->color_write_mask!=15u && resolved_canvas!=2) ||
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
  /* Named Classic surfaces have ordinary source coverage, not automatic
   * application phase planes. Integer magnification agrees with both leading
   * and centre point sampling. Reuse the device view without scanning alpha. */
  int classic_filter=resolved_canvas && r->classic && surf==0 && r->interp &&
    (!r->blendmode || raw_replace) && alpha>=1.0 && (blend&0xFFFFFFu)==0xFFFFFFu &&
    width>source_width && height>source_height && width<=16384.0 && height<=16384.0 &&
    (resolved_canvas!=2 || !r->alpha_test_enable || r->alpha_test_ref==0u ||
      (surface_known_opaque(r,surf) && r->alpha_test_ref<255u));
  if(classic_filter){
    if(r->app_phase_y) return 0;
    for(unsigned axis=0;axis<3u;axis++)
      if(r->classic_interp_phase[axis] || r->app_interp_phase[axis]) return 0;
  }
  if(r->classic && !classic_filter && resolved_canvas!=2){
    if(surf<=0 || r->interp || r->target_sp || r->target_id>=0 ||
       r->aspect_fullwidth || gml_render_gui_transform_active(r) ||
       r->world_transform_active || r->pending_underlay || destination_blend ||
       r->fog_flat || alpha<1.0 || (blend&0xFFFFFFu)!=0xFFFFFFu ||
       source_width<=0 || source_height<=0 ||
       dw!=width || dh!=height || dx-r->cam_x!=left || dy-r->cam_y!=top ||
       width<source_width || height<source_height || width>16384.0 || height>16384.0 ||
       fmod(width,(double)source_width)!=0.0 || fmod(height,(double)source_height)!=0.0 ||
       left<0.0 || top<0.0 || left+width>r->fbw || top+height>r->fbh) return 0;
    classic_point=1;
  }
  /* The preserved first-generation GUI leaf selects pixel-centre point samples
   * for opaque, white content-owned application presentation. Other ordinary
   * surface requests keep their floor/lround geometry and filter selection. */
  point_presentation=resolved_canvas!=2 && surf==0 && r->win &&
    anygm_policy_uses_first_generation_studio(r->win) &&
    gml_render_gui_transform_active(r) && !r->target_sp && r->target_id<0 &&
    !r->interp && !destination_blend && alpha>=1.0 &&
    (blend&0xFFFFFFu)==0xFFFFFFu && surface_known_opaque(r,0) &&
    (!r->app_draw_enable ||
     (fabs(dw-(double)source_width*r->gui_scale_x)<=.001 &&
      fabs(dh-(double)source_height*r->gui_scale_y)<=.001 &&
      (fabs(dw-lround(dw))>=.001 || fabs(dh-lround(dh))>=.001)));
  /* This established opaque GUI presentation leaf does not inspect sampled
   * alpha. Other old owners keep their cutoff fence; the ordinary function
   * executor below carries and tests its raw surface alpha explicitly. */
  if(r->alpha_test_enable && !point_presentation && resolved_canvas!=2) return 0;
  /* The CPU float band can overshoot an all-255 alpha by a rounding ULP.
   * Its inclusive maximum cutoff therefore has no natural hardware coverage. */
  if(resolved_canvas==2 && r->alpha_test_enable && r->alpha_test_ref==255u) return 0;
  /* Fixed sampler precision at a nonopaque cutoff can change fragment
   * admission,not just colour. Keep that measured coverage boundary explicit. */
  if(resolved_canvas==2 && r->alpha_test_enable && r->alpha_test_ref>0u &&
     !surface_known_opaque(r,surf)) return 0;
  /* Partial-alpha composition uses the ordinary leading-edge unit kernel,
   * not the opaque GUI leaf's pixel-centre geometry. Admit only its corrected
   * 1:1 extent; all nonunit point blends retain their existing owner. */
  unit_gui_blend=resolved_canvas!=2 && surf==0 && r->win &&
    anygm_policy_uses_first_generation_studio(r->win) &&
    gml_render_gui_transform_active(r) && !r->target_sp && r->target_id<0 &&
    !destination_blend && alpha<1.0 && !r->app_draw_enable &&
    (blend&0xFFFFFFu)==0xFFFFFFu && surface_known_opaque(r,0);
  /* Preserve the CPU compositor's source-aspect correction before preparing
   * either GUI rectangle. A corrected unit draw needs no filtering even when
   * interpolation is enabled. Other states retain their existing owner. */
  if(resolved_canvas!=2 && surf==0 && r->aspect_fullwidth && !r->target_sp &&
     source_width==r->aspect_wide_w && source_height==r->aspect_wide_h &&
     dh>=(double)r->fbh*.75 && dw/dh+.01<(double)source_width/source_height){
    if((!point_presentation && !unit_gui_blend) || source_width<=0 || source_height<=0 ||
       r->fbw<=0 || r->fbh<=0) return 0;
    double scale=dh/(double)source_height;
    double want_w=(double)source_width*scale;
    if(want_w<=(double)r->fbw+.5){
      dw=want_w;
      dx=((double)r->fbw-dw)*.5+r->cam_x;
    } else {
      scale=(double)r->fbw/(double)source_width;
      dw=(double)r->fbw;dh=(double)source_height*scale;
      dx=r->cam_x;dy=((double)r->fbh-dh)*.5+r->cam_y;
    }
    left=floor(dx-r->cam_x);top=floor(dy-r->cam_y);
    width=(double)lround(dw);height=(double)lround(dh);
    if(!point_presentation && (width!=source_width || height!=source_height)) return 0;
  }
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
  /* A whole numbered source in the CPU's white/full-alpha normal leaf uses
   * leading-edge point magnification even with interpolation enabled. Keep
   * unit packets and true filtered partial/tinted draws on their old owners. */
  if(!resolved_canvas && surf>0 && !unit && r->interp && !r->blendmode &&
     alpha>=1.0 && (blend&0xFFFFFFu)==0xFFFFFFu &&
     width>=source_width && height>=source_height)
    return resident_surface_nearest_selection(r,surf,dx,dy,dw,dh,blend,alpha,0,NULL,0);
  /* Bound exact integer phase products and filtered tap accumulation. The CPU
   * destination-factor leaf uses left-edge point/box sampling, even when enlarged. */
  if(destination_blend && (alpha<0x1p-32 || width<1.0 || height<1.0 ||
     width>4096.0 || height>4096.0 || source_width>4096 || source_height>4096 ||
     (r->interp && ((double)source_width>width*8.0 ||
                    (double)source_height>height*8.0)))) return 0;
  if(!isfinite(left) || !isfinite(top) || left<(double)INT_MIN/2 ||
     top<(double)INT_MIN/2 || left>(double)INT_MAX/2 || top>(double)INT_MAX/2 ||
     source_width<=0 || source_height<=0 ||
     (!point_presentation && !destination_blend && !classic_point && !unit &&
      (!r->interp || width<=source_width || height<=source_height ||
       (resolved_canvas!=2 && !classic_filter && surf==0 && alpha>=1.0 && (blend&0xFFFFFFu)==0xFFFFFFu)))) return 0;
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
  /* A queued automatic presentation precedes this public GUI draw even when
   * content has disabled future automatic draws. Reuse its established compositor;
   * non-GUI and nonzero-camera variants retain their prior ordered CPU owner. */
  if(r->pending_underlay &&
     (!gml_render_gui_transform_active(r) || r->cam_x!=0.0 || r->cam_y!=0.0 ||
      !gml_render_compose_resident_underlay(r) || !r->resident_application_pass)) return 0;
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
    :(unit || classic_point)?GML_RENDER_FILTER_NEAREST:GML_RENDER_FILTER_HARDWARE_BILINEAR;
  quad->sampling.edge_rule=GML_RENDER_EDGE_CLAMP_SUBIMAGE;
  quad->sampling.source_region=(GmlRenderRect){0,0,source.width,source.height};
  quad->filtered_draw_alpha=alpha;
  quad->x[0]=quad->x[3]=left;quad->x[1]=quad->x[2]=left+width;
  quad->y[0]=quad->y[1]=top;quad->y[2]=quad->y[3]=top+height;
  quad->u[1]=quad->u[2]=quad->v[2]=quad->v[3]=1.0;
  if(classic_filter){
    quad->sampling.quantisation=resolved_canvas==2?GML_RENDER_QUANTISE_SURFACE_TILED_LEADING:
      GML_RENDER_QUANTISE_HARDWARE_SURFACE_LEADING;
    double u=.5/source_width-.5/width,v=.5/source_height-.5/height;
    for(unsigned corner=0;corner<4u;corner++){
      quad->u[corner]+=u;quad->v[corner]+=v;
    }
  }
  else if(resolved_canvas==2) quad->sampling.quantisation=GML_RENDER_QUANTISE_SURFACE_TILED_FILTERED;
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
  if(resolved_canvas==2 && !r->alphablend){
    state.rgb_source_factor=state.rgb_destination_factor=0u;
    state.alpha_source_factor=state.alpha_destination_factor=0u;
    state.rgb_equation=state.alpha_equation=0u;
  }
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
  if(resolved_canvas==2){
    state.alpha_test_enabled=!!r->alpha_test_enable;
    state.alpha_test_reference=r->alpha_test_ref;
  }
  state.sampler_count=1u;state.samplers[0]=quad->sampling;
  if(!gml_render_application_scene_enqueue(r,&operation,&state,&source)) return 0;
  /* R03-P5-GUI-OPAQUE-SUFFIX: these known-opaque GUI sources write opaque
   * SCREEN alpha at every covered pixel, including partial draw alpha in the
   * unit leaf. Full coverage proves the destination without observing it. */
  int opaque_gui=(point_presentation || (unit_gui_blend && unit)) &&
    left<=0.0 && top<=0.0 && left+width>=r->fbw && top+height>=r->fbh;
  r->fb_opaque_known=r->fb_all_opaque=opaque_gui;
  r->fb_all_transparent=0;
  return 1;
}

/* A contained integer content-program rectangle uses the terminal plan's compiler/draw state.
 * A numbered destination retains its device result until a CPU observer or the frame boundary;
 * a later distinct surface draw can consume that result on the same device. */
static int resident_surface_shader(GmlRender *r,int surf,double dx,double dy,double dw,double dh,
                                   uint32_t blend,double alpha,
                                   const GmlRenderShaderGeometry *geometry,int stretched,
                                   const GmlRenderRect *selected_region){
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
  int threshold=r && r->scene_device_enabled &&
    gml_render_shader_threshold_palette_snapshot(r,r->active_shader,&shader);
  int lut=!threshold && r && r->scene_device_enabled &&
    gml_render_shader_lut_snapshot(r,r->active_shader,&shader,&sources[1]);
  int palette=threshold || lut;
  int palette_replace=0,palette_opaque_alpha=0;
  static const float white[4]={1.0f,1.0f,1.0f,1.0f};
  if(!r || (surf!=0 && (slot<0 || !r->surface[slot].live)) ||
     (surf==0 && (!r->app_surface_published || !r->app_surface ||
                  r->app_surface==r->fb)) ||
     !r->resident_application_pass ||
     !gml_render_device(r) || r->active_shader<0 ||
     (!palette && (!r->shader_device_present ||
                   !gml_render_shader_content_candidate(r,r->active_shader))) ||
     r->pending_underlay || r->pending_presentation ||
     (r->blendmode && !(palette && r->blendmode==6)) ||
     (r->alpha_test_enable && (!palette || r->alpha_test_ref==255u)) ||
     (!palette && r->color_write_mask!=0x0Fu) ||
     r->blend_equation!=1 || r->blend_equation_alpha!=1 || gml_d3_is_active(r) ||
     (!palette && (blend&0xFFFFFFu)!=0xFFFFFFu) || !isfinite(alpha) || alpha<1.0 ||
     !isfinite(dx) || !isfinite(dy) || !isfinite(dw) || !isfinite(dh) ||
     (surf!=0 && r->surface[slot].px==r->fb)) return 0;
  /* Recognized fixed evaluators retain their separately qualified full-source
   * contract. Arbitrary programs already consume a source rectangle in the
   * terminal executor; preserve that same intent on a resident source. */
  if(selected_region && palette &&
     (selected_region->x || selected_region->y ||
      selected_region->width!=(uint32_t)(surf==0?r->app_w:r->surface[slot].w) ||
      selected_region->height!=(uint32_t)(surf==0?r->app_h:r->surface[slot].h))) return 0;
  int cropped=selected_region &&
    (selected_region->x || selected_region->y ||
     selected_region->width!=(uint32_t)(surf==0?r->app_w:r->surface[slot].w) ||
     selected_region->height!=(uint32_t)(surf==0?r->app_h:r->surface[slot].h));
  if(cropped && (!r->alphablend || r->fbw>4096 || r->fbh>4096)) return 0;
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
  if(!palette && !gml_render_shader_plan_snapshot(r,r->active_shader,0u,white,&shader)) return 0;
  if(palette){
    int source_width=surf==0?r->app_w:r->surface[slot].w;
    int source_height=surf==0?r->app_h:r->surface[slot].h;
    if(source_width<=0 || source_height<=0 ||
       fmod(dw,(double)source_width)!=0.0 || fmod(dh,(double)source_height)!=0.0 ||
       dw<source_width || dh<source_height || r->app_phase_y ||
       r->classic_interp_phase[0] || r->classic_interp_phase[1] || r->classic_interp_phase[2]) return 0;
    /* The white unit CPU leaf replaces every nonzero source-alpha sample with
     * an opaque palette colour. Other geometries/tints retain source coverage:
     * without an opaque source they cannot share this fixed replacement packet. */
    if(lut && !surface_known_opaque(r,surf) &&
         (dw!=source_width || dh!=source_height || (blend&0xFFFFFFu)!=0xFFFFFFu ||
          r->alpha_test_enable)) return 0;
    if(threshold && !surface_known_opaque(r,surf)){
      const struct GmlShaderPal *p=&r->shader_pal[r->active_shader];
      if(p->alpha_discard || p->ordered_dither) return 0;
      int unit=dw==source_width && dh==source_height;
      int unit_leaf=unit && !(r->alpha_test_enable && r->alpha_test_ref>0u);
      int white=(blend&0xFFFFFFu)==0xFFFFFFu;
      int source_alpha=1;
      int cutoff=r->alpha_test_enable?(int)r->alpha_test_ref:0;
      /* The unit white mapping leaf copies coverage rather than blending it,
       * even for partial alpha. Its disabled-alpha branch also writes zero.
       * Other normal leaves blend RGB and stamp opaque alpha only on coverage. */
      palette_replace=r->blendmode==6 || (unit_leaf && (white || !r->alphablend)) ||
        (!unit_leaf && r->classic && surf==0);
      if(unit_leaf && white) cutoff=r->alphablend?0:-1;
      else if((unit_leaf && !r->alphablend && r->blendmode!=6) ||
              (!unit_leaf && r->classic && surf==0 && r->blendmode!=6)) source_alpha=0;
      palette_opaque_alpha=!palette_replace;
      shader.uniforms[14].value[0]=(float)source_alpha;
      shader.uniforms[14].value[1]=(float)cutoff;
    }
    shader.vertex_colour[0]=(float)(blend&255u)/255.0f;
    shader.vertex_colour[1]=(float)((blend>>8)&255u)/255.0f;
    shader.vertex_colour[2]=(float)((blend>>16)&255u)/255.0f;
    /* The existing first-generation GUI terminal-copy policy bypasses recognized shaders.
     * In this integral domain its transformed fractional-extent branch cannot apply; the
     * content-owned presentation branch still must copy the source, not recolour it. Part
     * calls do not select that software leaf. Preserve the policy without changing it. */
    shader.uniforms[threshold?13u:5u].value[0]=stretched && surf==0 && r->win &&
      surface_known_opaque(r,surf) &&
      anygm_policy_uses_first_generation_studio(r->win) &&
      gml_render_gui_transform_active(r) && !r->target_sp && r->target_id<0 &&
      !r->interp && !r->blendmode && r->color_write_mask==15u &&
      (blend&0xFFFFFFu)==0xFFFFFFu && !r->app_draw_enable;
  }
  if(!geometry || !geometry->valid) return 0;
  shader.geometry=palette?(GmlRenderShaderGeometry){left,top,dw,dh,1u}:*geometry;
  source_resource=surf==0?gml_render_application_resource_ensure(r):
    gml_render_surface_resource_ensure(r,surf);
  if(source_resource && gml_render_resource_device_source_view(
       source_resource,gml_render_device(r),&sources[0])){
    if(sources[0].pixel_format!=GML_RENDER_PIXEL_ARGB8888) return 0;
  } else if(palette && source_resource &&
            gml_render_resource_source_view(source_resource,&sources[0])){
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
  GmlRenderRect source_region=selected_region?*selected_region:
    (GmlRenderRect){0,0,sources[0].width,sources[0].height};
  if(source_region.x<0 || source_region.y<0 || !source_region.width || !source_region.height ||
     source_region.width>sources[0].width || source_region.height>sources[0].height ||
     (uint32_t)source_region.x>sources[0].width-source_region.width ||
     (uint32_t)source_region.y>sources[0].height-source_region.height) return 0;
  if(palette && (sources[0].width!=(uint32_t)(surf==0?r->app_w:r->surface[slot].w) ||
                 sources[0].height!=(uint32_t)(surf==0?r->app_h:r->surface[slot].h))) return 0;
  source_refs[0]=sources[0].image;
  if(lut){
    source_refs[1]=sources[1].image;
  } else if(shader.sampler_count){
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
  if(palette_replace){
    state.rgb_source_factor=state.rgb_destination_factor=0u;
    state.alpha_source_factor=state.alpha_destination_factor=0u;
    state.rgb_equation=state.alpha_equation=0u;
  }
  state.write_mask=15u;
  state.colour=0xFFFFFFFFu;
  state.alpha=255u;
  state.sampler_count=1u;
  state.samplers[0].filter=!palette && r->interp?GML_RENDER_FILTER_BILINEAR:GML_RENDER_FILTER_NEAREST;
  state.samplers[0].edge_rule=GML_RENDER_EDGE_CLAMP_SUBIMAGE;
  state.samplers[0].source_region=source_region;
  if(cropped || palette_opaque_alpha)
    state.coverage_policy=GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA;
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
    if(!palette && device && device->content_program_failed &&
       device->content_program_failed(device->context,&shader))
      gml_render_shader_mark_failed(r,r->active_shader);
    return 0;
  }
  r->fb_opaque_known=r->fb_all_opaque=palette && surface_known_opaque(r,surf) &&
    left==0.0 && top==0.0 &&
    dw==(double)r->fbw && dh==(double)r->fbh;
  r->fb_all_transparent=0;
  if(r->fb_all_opaque && r->fb==r->app_surface_owned) r->app_surface_opaque=1;
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
     resident_surface_shader_selected(r) ||
     r->interp || r->blendmode || r->alpha_test_enable ||
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

int gml_render_resident_surface_tiled(GmlRender *r,int surf,double x,double y,
  double xs,double ys,uint32_t colour,double alpha){
  const GmlRenderDeviceServices *device=gml_render_device(r);
  if(!r || !r->resident_application_pass || !device || !device->observe_on_demand ||
     !device->observe_on_demand(device->context)) return 0;
  if(surf<0) return 0;
  int slot=surface_slot(surf);
  int sw=surf==0?r->app_w:slot>=0 && r->surface[slot].live?r->surface[slot].w:0;
  int sh=surf==0?r->app_h:slot>=0 && r->surface[slot].live?r->surface[slot].h:0;
  const uint32_t *pixels=surf==0?r->app_surface:slot>=0?r->surface[slot].px:NULL;
  if(!pixels || sw<=0 || sh<=0 || pixels==r->fb) return 1;
  if(!r->win || !gml_render_scene_target_matches(r) || gml_d3_is_active(r) || r->fog_flat ||
     r->pending_presentation || r->classic_phase_scene_target ||
     r->rotated_batch_count || r->rotated_batch_building) return 0;
  for(int index=0;index<r->target_sp;index++)
    if(r->target_stack[index].fb==pixels && r->target_stack[index].pending_fill) return 0;
  /* Cells are mapped parts,not independent automatic presentations. Preserve
   * the original signed cell extents and unrounded repeat phase. */
  gml_render_draw_map_point(r,&x,&y);gml_render_draw_map_scale(r,&xs,&ys);
  double width=fabs(sw*xs),height=fabs(sh*ys);
  double relative_x=x-r->cam_x,relative_y=y-r->cam_y;
  if(!isfinite(width) || !isfinite(height) || !isfinite(relative_x) || !isfinite(relative_y)) return 0;
  if(width<.5 || height<.5 || width>((double)INT_MAX-r->fbw-2)/2 ||
     height>((double)INT_MAX-r->fbh-2)/2) return 1;
  if(sw>4096 || sh>4096 || width>16384.0 || height>16384.0) return 0;
  double first_x=fmod(relative_x,width),first_y=fmod(relative_y,height);
  if(first_x>0) first_x-=width;
  if(first_y>0) first_y-=height;
  double columns=ceil((r->fbw-first_x)/width)+1,rows=ceil((r->fbh-first_y)/height)+1;
  if(columns>GML_SCENE_SURFACE_RUN_LIMIT || rows>GML_SCENE_SURFACE_RUN_LIMIT ||
     columns*rows>GML_SCENE_SURFACE_RUN_LIMIT) return 0;
  int opaque=surface_known_opaque(r,surf);
  int normal=r->blendmode==0 || r->blendmode==1 || r->blendmode==2 ||
    r->blendmode==5 || r->blendmode==7;
  int unit=(int)lround(width)==sw && (int)lround(height)==sh && xs>0 && ys>0 &&
    (!r->alpha_test_enable || !r->alpha_test_ref || (opaque && r->alpha_test_ref<255u));
  int full_normal=!r->blendmode && xs>0 && ys>0 && alpha>=1.0 &&
    (colour&0xFFFFFFu)==0xFFFFFFu &&
    (!r->alpha_test_enable || !r->alpha_test_ref || (opaque && r->alpha_test_ref<255u));
  int classic_copy=r->classic && surf==0 && (!full_normal ||
    (unit && (alpha>=1.0 || !r->alphablend) && (colour&0xFFFFFFu)==0xFFFFFFu));
  /* A fractional period may overlay four rounded cells. Their independent
   * CPU truncations can exceed the individual fixed-blend budget (measured4).
   * Copies and destination-factor presets do not enter that rounding chain. */
  if(normal && !classic_copy && (!opaque || (r->alphablend && alpha<1.0)) &&
     lround(width)>width && lround(height)>height) return 0;
  if(r->pending_underlay && !gml_render_compose_resident_underlay(r)) return 0;
  gml_render_flush_pending_fill(r);if(r->pending_fill) return 0;
  ResidentSurfaceSelection selection={0,0,(double)sw,(double)sh};
  for(unsigned attempt=0;attempt<2u;attempt++){
    if(!gml_render_application_scene_prepare_begin(r)) return 0;
    r->pending_scene_batch->preparing=GML_SCENE_PREPARING_SURFACE;
    int accepted=1;
    for(int64_t row=0;row<(int64_t)rows && accepted;row++)
      for(int64_t column=0;column<(int64_t)columns;column++){
        double dx=first_x+column*width+r->cam_x,dy=first_y+row*height+r->cam_y;
        accepted=resident_surface_nearest_selection(r,surf,dx,dy,sw*xs,sh*ys,
          colour,alpha,0,&selection,2) ||
          (!gml_render_blend_uses_destination_colour(r->blendmode) &&
           resident_surface_filtered_resolved(r,surf,dx,dy,sw*xs,sh*ys,colour,alpha,2));
        if(!accepted) break;
      }
    if(gml_render_application_scene_prepare_finish(r,accepted)) return 1;
    if(attempt || !gml_render_application_scene_flush(r)) return 0;
  }
  return 0;
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
  /* Keep the part call's coordinate mapping and independently bounded source
   * rectangle through the existing content-program handoff. Unqualified crop
   * or destination geometry retains the original ordered CPU path below. */
  if(r){
    int slot=surface_slot(surf);
    int source_width=surf==0?r->app_w:
      slot>=0 && r->surface[slot].live?r->surface[slot].w:0;
    int source_height=surf==0?r->app_h:
      slot>=0 && r->surface[slot].live?r->surface[slot].h:0;
    if(source_width>0 && source_height>0 &&
       isfinite(sx) && isfinite(sy) && isfinite(sw) && isfinite(sh) &&
       sx>=0.0 && sy>=0.0 && sw>0.0 && sh>0.0 &&
       sx+sw<=source_width && sy+sh<=source_height &&
       floor(sx)==sx && floor(sy)==sy && floor(sw)==sw && floor(sh)==sh){
      GmlRenderRect region={(int32_t)sx,(int32_t)sy,(uint32_t)sw,(uint32_t)sh};
      if(resident_surface_shader(r,surf,dx,dy,sw*xs,sh*ys,blend,alpha,&geometry,0,&region)) return;
    }
  }
  ResidentSurfaceSelection selection={sx,sy,sw,sh};
  if(resident_surface_nearest_selection(r,surf,dx,dy,sw*xs,sh*ys,blend,alpha,0,&selection,1)) return;
  if(r && sx==0.0 && sy==0.0 && sw==gml_surface_width(r,surf) && sh==gml_surface_height(r,surf) &&
     resident_surface_filtered_resolved(r,surf,dx,dy,sw*xs,sh*ys,blend,alpha,1)) return;
  if(resident_surface_part(r,surf,sx,sy,sw,sh,dx,dy,xs,ys,blend,alpha)) return;
  if(!gml_render_resident_application_cpu_barrier(r)) return;
  gml_render_cpu_draw_surface_part_mapped(r,surf,sx,sy,sw,sh,dx,dy,xs,ys,blend,alpha,&geometry);
}
typedef struct {int x,y,width,u,v;} ResidentSurfacePointRun;

/* Only genuine source/target feedback needs a snapshot. It is device-only,
 * fully submitted before its temporary owner retires,and never CPU-observed. */
static GmlRenderResource *resident_surface_point_snapshot(const GmlRenderDeviceServices *device,
  const GmlRenderResourceSource *source,uint32_t *shadow){
  uint64_t incarnation=gml_render_scene_incarnation_allocate();
  if(!incarnation) return NULL;
  GmlRenderImageRef image={GML_RENDER_RESOURCE_SCRATCH,0u,incarnation,1u};
  GmlRenderResource *snapshot=gml_render_resource_create(image,source->width,source->height,
    GML_RENDER_PIXEL_ARGB8888,shadow,(GmlRenderResourceLimits){0});
  if(!snapshot) return NULL;
  if(!base_stage_clear(snapshot,device,source->width,source->height)) goto failed;
  GmlRenderDrawOp op={.opcode=GML_RENDER_OP_COPY_IMAGE,.scene_family=GML_RENDER_SCENE_SURFACE,
    .reconstruction_mode=GML_RENDER_DEVICE_CHECKPOINT,.source_count=1u,.sources=&source->image};
  op.target=(GmlRenderTargetRef){gml_render_resource_image_ref(snapshot),1u,source->width,
    source->height,GML_RENDER_PIXEL_ARGB8888,{0,0,source->width,source->height}};
  op.target.image.content_version++;
  op.payload.copy=(GmlRenderCopyRegion){{0,0,source->width,source->height},
    {0,0,source->width,source->height},0u,source->width,source->width};
  GmlRenderDrawStateSnapshot state={0};GmlRenderDrawBatch batch={&op,1u,&state,1u};
  if(gml_render_resource_submit_deferred(snapshot,device,&batch,source,1u)==GML_RENDER_RESOURCE_EXEC_DEVICE)
    return snapshot;
failed:
  device->retire(device->context,gml_render_resource_image_ref(snapshot));
  gml_render_resource_destroy(snapshot);return NULL;
}

/* Resolve only affine coverage/selected texels. No source pixel is inspected,
 * and the original CPU colour/filter/blend writer remains below the facade. */
static int resident_surface_rotated_function(GmlRender *r,int surf,double x,double y,
  double xs,double ys,double rotation,uint32_t colour,double alpha){
  const GmlRenderDeviceServices *device=gml_render_device(r);
  int slot=surface_slot(surf);
  if(!r || !r->win || !r->resident_application_pass || !device ||
     !device->observe_on_demand || !device->observe_on_demand(device->context) ||
     !gml_render_scene_target_matches(r) || r->pending_presentation ||
     r->classic_phase_scene_target || resident_surface_shader_selected(r) ||
     gml_d3_is_active(r) || r->fog_flat || r->rotated_batch_count || r->rotated_batch_building ||
     r->blendmode<0 || r->blendmode>11 || r->color_write_mask>15u ||
     r->fbw<=0 || r->fbh<=0 || r->fbw>16384 || r->fbh>16384 ||
     !isfinite(x) || !isfinite(y) || !isfinite(xs) || !isfinite(ys) ||
     !isfinite(rotation) || !isfinite(alpha)) return 0;
  int destination=r->alphablend && gml_render_blend_uses_destination_colour(r->blendmode);
  /* The CPU consults independent equations only for an active destination
   * pair. Disabled blending retains the selected equations without using them. */
  if(r->blend_equation<1 || r->blend_equation>5 ||
     r->blend_equation_alpha<1 || r->blend_equation_alpha>5) return 0;
  if(!r->color_write_mask || !xs || !ys || (alpha<=0 && !destination)) return 1;
  if(alpha>1) alpha=1;
  if(alpha<0) alpha=0;
  const uint32_t *pixels=surf==0?r->app_surface:
    slot>=0 && r->surface[slot].live?r->surface[slot].px:NULL;
  int sw=surf==0?r->app_w:slot>=0 && r->surface[slot].live?r->surface[slot].w:0;
  int sh=surf==0?r->app_h:slot>=0 && r->surface[slot].live?r->surface[slot].h:0;
  if(!pixels || sw<=0 || sh<=0) return 1;
  int alias=pixels==r->fb;
  if(sw>4096 || sh>4096 || (surf==0 && !r->app_surface_published)) return 0;
  if(alias){
    GmlRenderDeviceCaps caps={0};
    if(sw!=r->fbw || sh!=r->fbh || !device->retire || !device->capabilities ||
       !device->capabilities(device->context,&caps) ||
       !(caps.supported_operations&(UINT64_C(1)<<GML_RENDER_OP_COPY_IMAGE))) return 0;
  }
  for(int i=0;i<r->target_sp;i++)
    if(r->target_stack[i].fb==pixels && r->target_stack[i].pending_fill) return 0;
  gml_render_draw_map_point(r,&x,&y);gml_render_draw_map_scale(r,&xs,&ys);
  if(!isfinite(x) || !isfinite(y) || !isfinite(xs) || !isfinite(ys)) return 0;
  if(!xs || !ys) return 1;
  double c,sn;render_rotation_sincos(rotation,&c,&sn);
  double ax=x-r->cam_x,ay=y-r->cam_y;
  render_modern_cardinal_anchor(r,rotation,xs,ys,c,sn,&ax,&ay);
  double local[4][2]={{0,0},{sw*xs,0},{sw*xs,sh*ys},{0,sh*ys}};
  double minx=1e30,miny=1e30,maxx=-1e30,maxy=-1e30;
  for(unsigned i=0;i<4u;i++){
    double px=ax+local[i][0]*c+local[i][1]*sn;
    double py=ay-local[i][0]*sn+local[i][1]*c;
    if(!isfinite(px) || !isfinite(py) || fabs(px)>1000000 || fabs(py)>1000000) return 0;
    if(px<minx) minx=px;
    if(px>maxx) maxx=px;
    if(py<miny) miny=py;
    if(py>maxy) maxy=py;
  }
  int x0=(int)floor(minx)-1,y0=(int)floor(miny)-1;
  int x1=(int)ceil(maxx)+1,y1=(int)ceil(maxy)+1;
  if(x0<0) x0=0;
  if(y0<0) y0=0;
  if(x1>r->fbw) x1=r->fbw;
  if(y1>r->fbh) y1=r->fbh;
  if(x1<=x0 || y1<=y0) return 1;
  if((uint64_t)(unsigned)(x1-x0)*(unsigned)(y1-y0)>16777216u) return 0;
  ResidentSurfacePointRun *runs=malloc(GML_SCENE_SURFACE_RUN_LIMIT*sizeof *runs);
  if(!runs) return 0;
  uint32_t count=0;int result=0;GmlRenderResource *snapshot=NULL;uint32_t *snapshot_shadow=NULL;
  for(int py=y0;py<y1;py++) for(int px=x0;px<x1;px++){
    double rx=px+.5-ax,ry=py+.5-ay;
    double u=(rx*c-ry*sn)/xs,v=(rx*sn+ry*c)/ys;
    if(u<0 || v<0 || u>=sw || v>=sh) continue;
    /* A single integral tap is identical under both CPU filtering and natural
     * hardware sampling. Fractional four-tap truncation is measured D2 debt. */
    if(r->interp && (u-.5!=floor(u-.5) || v-.5!=floor(v-.5))) goto rotated_done;
    int su=(int)floor(u),sv=(int)floor(v);
    if(count && runs[count-1u].y==py && runs[count-1u].x+runs[count-1u].width==px &&
       runs[count-1u].u==su && runs[count-1u].v==sv){runs[count-1u].width++;continue;}
    if(count==GML_SCENE_SURFACE_RUN_LIMIT) goto rotated_done;
    runs[count++]=(ResidentSurfacePointRun){px,py,1,su,sv};
  }
  if(!count){result=1;goto rotated_done;}
  if(r->pending_underlay && !gml_render_compose_resident_underlay(r)) goto rotated_done;
  gml_render_flush_pending_fill(r);if(r->pending_fill || !r->resident_application_pass) goto rotated_done;
  if(alias && !gml_render_application_scene_flush(r)) goto rotated_done;
  GmlRenderResource *sample=surf==0?(pixels==r->app_surface_owned?
    gml_render_application_resource_ensure(r):gml_render_base_resource_lookup(r,pixels,NULL,NULL)):
    gml_render_surface_resource_ensure(r,surf);
  GmlRenderResourceSource source={0};
  if(!sample || !gml_render_resource_prepare_device_source(sample,device,&source) ||
     source.width!=(unsigned)sw || source.height!=(unsigned)sh ||
     source.pixel_format!=GML_RENDER_PIXEL_ARGB8888) goto rotated_done;
  if(alias){
    snapshot_shadow=calloc((size_t)sw*sh,sizeof *snapshot_shadow);
    if(!snapshot_shadow || !(snapshot=resident_surface_point_snapshot(device,&source,snapshot_shadow)) ||
       !gml_render_resource_device_source_view(snapshot,device,&source)) goto rotated_done;
  }
  GmlRenderDrawStateSnapshot state={.colour=0xFFFFFFFFu,.alpha=255u,.write_mask=r->color_write_mask,
    .coverage_policy=GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA,.sampler_count=1u};
  int mode=r->blendmode,paint_zero=mode==1 || mode==2 || mode==3 || mode==4 || mode==8 || destination || !r->alphablend;
  int rounded_alpha=mode==4 || mode==8 || destination;
  if(mode==3 || mode==4 || mode==8 || destination){
    int preserve=mode==3 || mode==4?r->target_sp>0:gml_render_target_preserves_alpha(r);
    state.coverage_policy=preserve?0u:GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA;
    state.rgb_equation=state.alpha_equation=GML_RENDER_BLEND_ADD;
    if(mode==3){
      state.rgb_source_factor=GML_RENDER_BLEND_DESTINATION_COLOUR;
      state.rgb_destination_factor=GML_RENDER_BLEND_ZERO;
      state.alpha_source_factor=preserve?GML_RENDER_BLEND_ZERO:GML_RENDER_BLEND_ONE;
      state.alpha_destination_factor=preserve?GML_RENDER_BLEND_ONE:GML_RENDER_BLEND_ZERO;
    }else if(mode==4){
      state.rgb_source_factor=GML_RENDER_BLEND_SOURCE_ALPHA;
      state.rgb_destination_factor=GML_RENDER_BLEND_ONE_MINUS_SOURCE_COLOUR;
      state.alpha_source_factor=preserve?GML_RENDER_BLEND_SOURCE_ALPHA:GML_RENDER_BLEND_ONE;
      state.alpha_destination_factor=preserve?GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA:GML_RENDER_BLEND_ZERO;
    }else if(mode==8){
      state.rgb_source_factor=GML_RENDER_BLEND_ONE_MINUS_DESTINATION_COLOUR;
      state.rgb_destination_factor=GML_RENDER_BLEND_ZERO;
      state.alpha_source_factor=preserve?GML_RENDER_BLEND_ONE_MINUS_DESTINATION_ALPHA:GML_RENDER_BLEND_ONE;
      state.alpha_destination_factor=GML_RENDER_BLEND_ZERO;
    }else{
      state.rgb_source_factor=state.alpha_source_factor=mode==11?
        GML_RENDER_BLEND_DESTINATION_ALPHA:GML_RENDER_BLEND_DESTINATION_COLOUR;
      state.rgb_destination_factor=state.alpha_destination_factor=mode==9?
        GML_RENDER_BLEND_SOURCE_COLOUR:GML_RENDER_BLEND_DESTINATION_COLOUR;
      state.rgb_equation=r->blend_equation==1?GML_RENDER_BLEND_ADD:
        GML_RENDER_BLEND_FIXED_MAX+(uint32_t)r->blend_equation-2u;
      state.alpha_equation=r->blend_equation_alpha==1?GML_RENDER_BLEND_ADD:
        GML_RENDER_BLEND_FIXED_MAX+(uint32_t)r->blend_equation_alpha-2u;
      state.alpha_test_enabled=!!r->alpha_test_enable;state.alpha_test_reference=r->alpha_test_ref;
    }
  }else if(r->alphablend || mode==1 || mode==2){
    state.rgb_source_factor=mode==2?GML_RENDER_BLEND_ZERO:GML_RENDER_BLEND_SOURCE_ALPHA;
    state.rgb_destination_factor=mode==2?GML_RENDER_BLEND_ONE_MINUS_SOURCE_COLOUR:
      mode==1?GML_RENDER_BLEND_ONE:GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA;
    state.alpha_source_factor=mode==2?GML_RENDER_BLEND_ZERO:GML_RENDER_BLEND_ONE;
    state.alpha_destination_factor=mode==2?GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA:
      mode==1?GML_RENDER_BLEND_ONE:GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA;
    state.rgb_equation=state.alpha_equation=GML_RENDER_BLEND_ADD;
  }
  state.samplers[0]=(GmlRenderSamplingPolicy){GML_RENDER_FILTER_NEAREST,GML_RENDER_EDGE_CLAMP_SUBIMAGE,
    {0,0,(uint32_t)sw,(uint32_t)sh},GML_RENDER_QUANTISE_SURFACE_ROTATED_POINT,0u};
  uint32_t tint=((colour&255u)<<16)|(colour&0xFF00u)|((colour>>16)&255u)|((uint32_t)lround(alpha*255)<<24);
  for(unsigned attempt=0;attempt<2u;attempt++){
    if(!gml_render_application_scene_prepare_begin(r)) break;
    r->pending_scene_batch->preparing=GML_SCENE_PREPARING_SURFACE;
    int accepted=1;
    for(uint32_t i=0;i<count;i++){
      GmlRenderResource *target=gml_render_scene_target_resource_ensure(r);if(!target){accepted=0;break;}
      GmlRenderDrawOp op={.opcode=GML_RENDER_OP_TEXTURED_QUAD,.scene_family=GML_RENDER_SCENE_SURFACE,
        .reconstruction_mode=GML_RENDER_DEVICE_CHECKPOINT,.source_count=1u,.sources=&source.image};
      op.target=(GmlRenderTargetRef){gml_render_application_scene_next_image_ref(r,target),1u,
        (uint32_t)r->fbw,(uint32_t)r->fbh,GML_RENDER_PIXEL_ARGB8888,{0,0,(uint32_t)r->fbw,(uint32_t)r->fbh}};
      op.target.image.content_version++;
      GmlRenderQuad *q=&op.payload.quad;ResidentSurfacePointRun *run=&runs[i];
      q->shape=GML_RENDER_QUAD_AXIS_ALIGNED;q->sampling=state.samplers[0];q->filtered_draw_alpha=alpha;
      q->x[0]=q->x[3]=run->x;q->x[1]=q->x[2]=run->x+run->width;
      q->y[0]=q->y[1]=run->y;q->y[2]=q->y[3]=run->y+1;
      q->span_y[0]=paint_zero;q->span_y[1]=rounded_alpha;
      for(unsigned corner=0;corner<4u;corner++){
        q->u[corner]=(run->u+.5)/sw;q->v[corner]=(run->v+.5)/sh;q->corner_colour[corner]=tint;
      }
      if(!gml_render_application_scene_enqueue(r,&op,&state,&source)){accepted=0;break;}
    }
    if(gml_render_application_scene_prepare_finish(r,accepted)){result=1;break;}
    if(attempt || !gml_render_application_scene_flush(r)) break;
  }
  if(result && snapshot && !gml_render_application_scene_flush(r)){
    /* This preparation had no parent prefix. A refused device batch must not
     * retain a pointer to the temporary source as the CPU facade takes over. */
    GmlRenderResource *target=gml_render_scene_target_resource_ensure(r);
    if(target && r->pending_scene_batch)
      scene_remove_target(r->pending_scene_batch,gml_render_resource_image_ref(target));
    result=0;
  }
rotated_done:
  if(snapshot){device->retire(device->context,gml_render_resource_image_ref(snapshot));
    gml_render_resource_destroy(snapshot);}
  free(snapshot_shadow);
  free(runs);return result;
}

void gml_draw_surface_ext(GmlRender *r,int surf,double x,double y,
                          double xs,double ys,double rot,uint32_t blend,double alpha){
  double rr=fmod(rot,360.0); if(rr<0) rr+=360.0;
  if(fabs(rr)<0.001 || fabs(rr-360.0)<0.001){
    int sw=gml_surface_width(r,surf),sh=gml_surface_height(r,surf);
    if(sw>0&&sh>0) gml_draw_surface_stretched(r,surf,x,y,sw*xs,sh*ys,blend,alpha);
    return;
  }
  if(resident_surface_rotated_function(r,surf,x,y,xs,ys,rr,blend,alpha)) return;
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
/* Stage a new incarnation before replacing any borrowed allocation. Resize is a raw RGBA
 * copy, not a draw with the caller's blend, colour, alpha test or camera state. The old
 * resource remains authoritative if staging fails; the established CPU copy can then retry. */
static GmlRenderResource *surface_resize_resident(
  GmlRender *r,GmlRenderResource *old,int oldw,int oldh,uint32_t *pixels,int w,int h,
  uint32_t resource_class,uint32_t identity){
  const GmlRenderDeviceServices *device=gml_render_device(r);
  GmlRenderResourceSource source={0};
  GmlRenderDrawStateSnapshot state={0};
  GmlRenderDrawOp operations[2]={{0}};
  GmlRenderDrawBatch batch={operations,2u,&state,1u};
  GmlRenderResource *replacement;
  uint64_t incarnation;
  int cw=oldw<w?oldw:w,ch=oldh<h?oldh:h;
  if(!old || !device || !device->context || !device->observe_on_demand ||
     !device->observe_on_demand(device->context) || cw<=0 || ch<=0 ||
     /* Already-current host pixels need neither an upload nor a device observation. */
     gml_render_resource_current_cpu_source_view(old,&source) ||
     !gml_render_resource_device_source_view(old,device,&source) ||
     source.width!=(uint32_t)oldw || source.height!=(uint32_t)oldh ||
     !(incarnation=gml_render_scene_incarnation_allocate())) return NULL;
  GmlRenderImageRef image={resource_class,identity,incarnation,1u};
  replacement=gml_render_resource_create(image,(uint32_t)w,(uint32_t)h,
    GML_RENDER_PIXEL_ARGB8888,pixels,(GmlRenderResourceLimits){0});
  if(!replacement) return NULL;
  operations[0].opcode=GML_RENDER_OP_CLEAR;
  operations[0].reconstruction_mode=GML_RENDER_DEVICE_CHECKPOINT;
  operations[0].target=(GmlRenderTargetRef){image,1u,(uint32_t)w,(uint32_t)h,
    GML_RENDER_PIXEL_ARGB8888,{0,0,(uint32_t)w,(uint32_t)h}};
  operations[0].target.image.content_version=2u;
  operations[1]=operations[0];
  operations[1].opcode=GML_RENDER_OP_TEXTURED_QUAD;
  operations[1].target.image.content_version=3u;
  operations[1].sources=&source.image;
  operations[1].source_count=1u;
  GmlRenderQuad *quad=&operations[1].payload.quad;
  quad->shape=GML_RENDER_QUAD_AXIS_ALIGNED;
  quad->sampling=(GmlRenderSamplingPolicy){GML_RENDER_FILTER_NEAREST,
    GML_RENDER_EDGE_CLAMP_SUBIMAGE,{0,0,(uint32_t)cw,(uint32_t)ch},0u,0u};
  quad->x[1]=quad->x[2]=(double)cw;
  quad->y[2]=quad->y[3]=(double)ch;
  quad->u[1]=quad->u[2]=(double)cw/oldw;
  quad->v[2]=quad->v[3]=(double)ch/oldh;
  for(int corner=0;corner<4;corner++) quad->corner_colour[corner]=0xFFFFFFFFu;
  state.write_mask=15u;
  state.colour=0xFFFFFFFFu;
  state.alpha=255u;
  /* Zero blend fields are the existing unconditional replacement contract. */
  state.sampler_count=1u;
  state.samplers[0]=quad->sampling;
  if(gml_render_resource_submit_deferred(replacement,device,&batch,&source,1u)!=
     GML_RENDER_RESOURCE_EXEC_DEVICE){
    if(device->retire)
      device->retire(device->context,gml_render_resource_image_ref(replacement));
    gml_render_resource_destroy(replacement);
    return NULL;
  }
  gml_render_resource_bind_dispatch(replacement,r);
  return replacement;
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
  GmlRenderResource *old_resource=NULL,*replacement=NULL;
  if(old==r->app_surface_owned && r->app_surface_resource){
    oldw=r->app_surface_resource_width;
    oldh=r->app_surface_resource_height;
    old_resource=r->app_surface_resource;
  } else old_resource=gml_render_base_resource_lookup(r,old,NULL,NULL);
  uint32_t *px=calloc((size_t)w*(size_t)h,sizeof(uint32_t));
  if(!px) return 0;
  replacement=surface_resize_resident(r,old_resource,oldw,oldh,px,w,h,
    GML_RENDER_RESOURCE_APPLICATION,0u);
  if(!replacement && old_resource &&
     !gml_render_resource_acquire_cpu_read(old_resource,NULL)){ free(px); return 0; }
  int cw=oldw<w?oldw:w, ch=oldh<h?oldh:h;
  if(!replacement && old && cw>0 && ch>0)
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
  r->app_surface_resource=replacement;
  if(replacement){
    r->app_surface_resource_width=w;
    r->app_surface_resource_height=h;
  }
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
  GmlRenderResource *replacement=had_pending_fill?NULL:
    surface_resize_resident(r,r->surface[i].resource,r->surface[i].w,
      r->surface[i].h,px,w,h,GML_RENDER_RESOURCE_SURFACE,(uint32_t)id);
  const uint32_t *old=replacement?r->surface[i].px:surface_pixels_observe(r,id,NULL,NULL);
  if((r->target_id==id && r->pending_fill) || old!=r->surface[i].px){
    free(px);
    return;
  }
  int was_transparent=had_pending_fill?0:r->surface[i].all_transparent;
  int cw=w<r->surface[i].w?w:r->surface[i].w, ch=h<r->surface[i].h?h:r->surface[i].h;
  if(!replacement)
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
  r->surface[i].resource=replacement;
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
  const GmlRenderDeviceServices *device=gml_render_device(r);
  /* Initialization may author numbered targets before any world pass exists.
   * A usable owning context admits only that explicit child, never its CPU
   * SCREEN/application parent or the global scene. The saved parent pass still
   * owns restoration, including nested Studio and replacing Classic targets. */
  int independent=!r->resident_scene_pass && r->scene_device_enabled &&
    si>=0 && r->surface[si].live && device && device->observe_on_demand &&
    device->observe_on_demand(device->context);
  int resident=(r->resident_scene_pass || independent) && device &&
    (!r->classic || id>0 || (id==0 && r->app_surface==r->app_surface_owned));
  if(resident){
    /* Resolve pending parent writes before another binding can alias its allocation.
     * Binding itself borrows storage metadata, not CPU-current raster bytes. */
    if(!independent || r->resident_application_pass){
      gml_render_flush_pending_underlay(r);
      gml_render_flush_pending_fill(r);
      if(r->pending_underlay || r->pending_fill) return 0;
    }
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
    } else if(resident && id==0 && r->app_surface==r->app_surface_owned){
      px=r->app_surface_owned;w=r->app_w;h=r->app_h;
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
    /* An independent clear can update a CPU parent's borrowed allocation.
     * Restore that parent only after its pixels are current, without admitting
     * its SCREEN pass or losing the child's exact resource version. */
    typeof(r->target_stack[0]) *parent=&r->target_stack[r->target_sp-1];
    if(r->scene_device_enabled && !r->resident_scene_pass &&
       !parent->resident_pass && parent->fb==r->fb &&
       !gml_render_resident_application_cpu_barrier(r)) return;
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
    /* An empty reset on the already admitted base changes no image binding.
     * Do not admit another owner or revive a parent that already crossed to CPU. */
    int retain_resident=r->resident_scene_pass && r->resident_application_pass &&
      gml_render_device(r) && r->target_id==-1 && r->fb==r->base_fb &&
      r->fbw==r->base_fbw && r->fbh==r->base_fbh &&
      gml_render_scene_target_matches(r);
    r->fb=r->base_fb; r->fbw=r->base_fbw; r->fbh=r->base_fbh;
    r->target_id=-1;
    if(r->resident_application_pass && !retain_resident)
      gml_render_dispatch_resident_lost(r,GML_RENDER_RESIDENT_PASS_RESTART);
    r->resident_application_pass=retain_resident;
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
