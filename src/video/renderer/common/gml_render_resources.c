/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Canonical pixel authority and bounded resolved-operation retention. A device name is never
 * canonical state: the existing surface owns pixels, and this owner tracks how to update them. */
#include "gml_render_resources.h"
#include "gml_render_cpu_replay.h"
#include "gml_render_plan.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

enum { DEFAULT_JOURNAL_BYTES=8u*1024u*1024u,
       DEFAULT_RETAINED_BYTES=64u*1024u*1024u };

typedef struct PinnedSource {
  GmlRenderResourceSource view;
  uint8_t *owned;
  size_t charged_bytes;
} PinnedSource;

typedef struct JournalEntry {
  GmlRenderDrawOp operation;
  GmlRenderDrawStateSnapshot state;
  GmlRenderImageRef source;
  uint32_t source_index;
} JournalEntry;

struct GmlRenderResource {
  GmlRenderImageRef image;
  uint64_t storage_version,cpu_version,gpu_version;
  uint32_t width,height,pixel_format;
  uint32_t *pixels;
  GmlRenderResourceLimits limits;
  JournalEntry *entries;
  uint32_t entry_count;
  PinnedSource *sources;
  uint32_t source_count;
  size_t retained_bytes;
  uint32_t authority;
  int cpu_write_acquired;
  const void *device_identity;
  GmlRenderDeviceServices checkpoint_device;
  uint64_t cpu_replays,device_checkpoints,consolidations,cpu_fallbacks;
};

static int same_logical(GmlRenderImageRef a,GmlRenderImageRef b){
  return a.resource_class==b.resource_class && a.identity==b.identity &&
         a.incarnation==b.incarnation;
}

static int same_version(GmlRenderImageRef a,GmlRenderImageRef b){
  return same_logical(a,b) && a.content_version==b.content_version;
}

static int source_view_valid(const GmlRenderResourceSource *source){
  uint64_t bytes;
  const GmlRenderResource *resident;
  if(!source || !source->image.incarnation ||
     !source->image.content_version || !source->width || !source->height ||
     source->width>UINT32_MAX/4u || source->stride_bytes<source->width*4u ||
     (source->pixel_format!=GML_RENDER_PIXEL_XRGB8888 &&
      source->pixel_format!=GML_RENDER_PIXEL_ARGB8888 &&
      source->pixel_format!=GML_RENDER_PIXEL_RGBA8888)) return 0;
  resident=source->resident_resource;
  if(!source->bytes){
    return resident && !source->borrowed_immutable &&
           resident->authority==GML_RENDER_RESOURCE_GPU_PENDING_CHECKPOINT &&
           same_version(resident->image,source->image) &&
           resident->gpu_version==source->image.content_version &&
           resident->device_identity && resident->width==source->width &&
           resident->height==source->height &&
           resident->pixel_format==source->pixel_format &&
           source->stride_bytes==source->width*4u &&
           source->byte_capacity==0u;
  }
  bytes=(uint64_t)(source->height-1u)*source->stride_bytes+(uint64_t)source->width*4u;
  if(bytes>source->byte_capacity) return 0;
  if(!resident) return 1;
  return !source->borrowed_immutable &&
         resident->authority!=GML_RENDER_RESOURCE_INVALID &&
         same_version(resident->image,source->image) &&
         resident->cpu_version==source->image.content_version &&
         resident->width==source->width && resident->height==source->height &&
         resident->pixel_format==source->pixel_format &&
         source->bytes==resident->pixels &&
         source->stride_bytes==source->width*4u;
}

static const GmlRenderResourceSource *find_input(const GmlRenderResourceSource *sources,
                                                 uint32_t count,GmlRenderImageRef ref){
  if(!sources) return NULL;
  for(uint32_t index=0;index<count;index++)
    if(same_version(sources[index].image,ref) && source_view_valid(&sources[index]))
      return &sources[index];
  return NULL;
}

static int pin_source(PinnedSource *pin,const GmlRenderResourceSource *view){
  size_t bytes;
  memset(pin,0,sizeof *pin);
  if(!source_view_valid(view)) return 0;
  pin->view=*view;
  if(view->borrowed_immutable) return 1;
  if((size_t)view->width>SIZE_MAX/4u/(size_t)view->height) return 0;
  bytes=(size_t)view->width*view->height*4u;
  pin->owned=(uint8_t*)malloc(bytes);
  if(!pin->owned) return 0;
  for(uint32_t row=0;row<view->height;row++)
    memcpy(pin->owned+(size_t)row*view->width*4u,
           (const uint8_t*)view->bytes+(size_t)row*view->stride_bytes,
           (size_t)view->width*4u);
  pin->view.bytes=pin->owned;
  pin->view.resident_resource=NULL;
  pin->view.stride_bytes=view->width*4u;
  pin->view.byte_capacity=bytes;
  pin->charged_bytes=bytes;
  return 1;
}

static void clear_history(GmlRenderResource *resource){
  for(uint32_t index=0;index<resource->source_count;index++)
    free(resource->sources[index].owned);
  free(resource->sources);
  free(resource->entries);
  resource->sources=NULL;
  resource->entries=NULL;
  resource->source_count=resource->entry_count=0;
  resource->retained_bytes=0;
}

GmlRenderResource *gml_render_resource_create(GmlRenderImageRef image,uint32_t width,
                                               uint32_t height,uint32_t pixel_format,
                                               uint32_t *canonical_pixels,
                                               GmlRenderResourceLimits limits){
  GmlRenderResource *resource;
  if(!canonical_pixels || !image.incarnation || !image.content_version ||
     !width || !height || width>UINT32_MAX/4u ||
     (uint64_t)width*height>INT_MAX ||
     (pixel_format!=GML_RENDER_PIXEL_XRGB8888 &&
      pixel_format!=GML_RENDER_PIXEL_ARGB8888)) return NULL;
  resource=(GmlRenderResource*)calloc(1,sizeof *resource);
  if(!resource) return NULL;
  resource->image=image;
  resource->storage_version=1u;
  resource->cpu_version=image.content_version;
  resource->width=width;
  resource->height=height;
  resource->pixel_format=pixel_format;
  resource->pixels=canonical_pixels;
  resource->limits.journal_bytes=limits.journal_bytes
    ?limits.journal_bytes:DEFAULT_JOURNAL_BYTES;
  resource->limits.retained_mutable_bytes=limits.retained_mutable_bytes
    ?limits.retained_mutable_bytes:DEFAULT_RETAINED_BYTES;
  resource->authority=GML_RENDER_RESOURCE_CPU_CURRENT;
  return resource;
}

void gml_render_resource_destroy(GmlRenderResource *resource){
  if(!resource) return;
  clear_history(resource);
  free(resource);
}

GmlRenderImageRef gml_render_resource_image_ref(const GmlRenderResource *resource){
  GmlRenderImageRef image={0};
  return resource?resource->image:image;
}

static int replay_history(const GmlRenderResource *resource,uint32_t *destination){
  for(uint32_t index=0;index<resource->entry_count;index++){
    const JournalEntry *entry=&resource->entries[index];
    const GmlRenderResourceSource *source=entry->operation.source_count
      ?&resource->sources[entry->source_index].view:NULL;
    GmlRenderDrawOp operation=entry->operation;
    operation.sources=operation.source_count?&entry->source:NULL;
    if(!gml_render_cpu_replay_one(destination,resource->width,resource->height,
                                  &operation,&entry->state,source)) return 0;
  }
  return 1;
}

int gml_render_resource_acquire_cpu_read(GmlRenderResource *resource,
                                          const uint32_t **out_pixels){
  uint32_t *candidate;
  size_t bytes;
  if(out_pixels) *out_pixels=NULL;
  if(!resource || !resource->pixels || resource->authority==GML_RENDER_RESOURCE_INVALID) return 0;
  if(resource->authority==GML_RENDER_RESOURCE_GPU_PENDING_CHECKPOINT &&
     !gml_render_resource_checkpoint(resource,NULL)) return 0;
  if(resource->entry_count){
    bytes=(size_t)resource->width*resource->height*4u;
    candidate=(uint32_t*)malloc(bytes);
    if(!candidate) return 0;
    memcpy(candidate,resource->pixels,bytes);
    if(!replay_history(resource,candidate)){ free(candidate); return 0; }
    memcpy(resource->pixels,candidate,bytes);
    free(candidate);
    clear_history(resource);
    resource->cpu_version=resource->image.content_version;
    resource->cpu_replays++;
    resource->authority=resource->gpu_version==resource->image.content_version
      ?GML_RENDER_RESOURCE_BOTH_CURRENT:GML_RENDER_RESOURCE_CPU_CURRENT;
  }
  if(out_pixels) *out_pixels=resource->pixels;
  return 1;
}

int gml_render_resource_source_view(GmlRenderResource *resource,
                                    GmlRenderResourceSource *out_source){
  const uint32_t *pixels=NULL;
  if(out_source) memset(out_source,0,sizeof *out_source);
  if(!out_source || !gml_render_resource_acquire_cpu_read(resource,&pixels) ||
     resource->width>UINT32_MAX/4u) return 0;
  out_source->image=resource->image;
  out_source->width=resource->width;
  out_source->height=resource->height;
  out_source->pixel_format=resource->pixel_format;
  out_source->stride_bytes=resource->width*4u;
  out_source->byte_capacity=(size_t)resource->width*resource->height*4u;
  out_source->bytes=pixels;
  out_source->resident_resource=resource;
  return 1;
}

int gml_render_resource_pending_device_source_view(
  GmlRenderResource *resource,const GmlRenderDeviceServices *device,
  GmlRenderResourceSource *out_source){
  if(out_source) memset(out_source,0,sizeof *out_source);
  if(!out_source || !resource || !device || !device->context ||
     resource->authority!=GML_RENDER_RESOURCE_GPU_PENDING_CHECKPOINT ||
     resource->device_identity!=device->context ||
     resource->gpu_version!=resource->image.content_version ||
     resource->width>UINT32_MAX/4u) return 0;
  out_source->image=resource->image;
  out_source->width=resource->width;
  out_source->height=resource->height;
  out_source->pixel_format=resource->pixel_format;
  out_source->stride_bytes=resource->width*4u;
  out_source->resident_resource=resource;
  return 1;
}

int gml_render_resource_acquire_cpu_write(GmlRenderResource *resource,
                                           uint32_t **out_pixels){
  const uint32_t *current=NULL;
  if(out_pixels) *out_pixels=NULL;
  if(!gml_render_resource_acquire_cpu_read(resource,&current)) return 0;
  resource->cpu_write_acquired=1;
  if(out_pixels) *out_pixels=resource->pixels;
  return 1;
}

int gml_render_resource_commit_cpu_write(GmlRenderResource *resource){
  if(!resource || resource->authority==GML_RENDER_RESOURCE_INVALID ||
     !resource->cpu_write_acquired || resource->entry_count ||
     resource->image.content_version==UINT64_MAX) return 0;
  resource->image.content_version++;
  resource->cpu_version=resource->image.content_version;
  resource->authority=GML_RENDER_RESOURCE_CPU_CURRENT;
  resource->cpu_write_acquired=0;
  return 1;
}

void gml_render_resource_context_lost(GmlRenderResource *resource){
  if(!resource) return;
  if(resource->authority==GML_RENDER_RESOURCE_GPU_PENDING_CHECKPOINT)
    resource->authority=GML_RENDER_RESOURCE_INVALID;
  resource->gpu_version=0;
  resource->device_identity=NULL;
  memset(&resource->checkpoint_device,0,sizeof resource->checkpoint_device);
  if(resource->authority==GML_RENDER_RESOURCE_BOTH_CURRENT)
    resource->authority=GML_RENDER_RESOURCE_CPU_CURRENT;
}

static int copy_checkpoint(const GmlRenderResource *resource,uint32_t **out_pixels){
  size_t bytes=(size_t)resource->width*resource->height*4u;
  uint32_t *copy=(uint32_t*)malloc(bytes);
  if(!copy) return 0;
  memcpy(copy,resource->pixels,bytes);
  if(!replay_history(resource,copy)){ free(copy); return 0; }
  *out_pixels=copy;
  return 1;
}

static GmlRenderResourceExecution cpu_fallback(GmlRenderResource *resource,
                                                const GmlRenderDrawBatch *batch,
                                                const GmlRenderResourceSource *sources,
                                                uint32_t source_count){
  uint32_t *copy=NULL;
  size_t bytes=(size_t)resource->width*resource->height*4u;
  if(!copy_checkpoint(resource,&copy)) return GML_RENDER_RESOURCE_EXEC_FAILED;
  for(uint32_t index=0;index<batch->operation_count;index++){
    const GmlRenderDrawOp *operation=&batch->operations[index];
    const GmlRenderDrawStateSnapshot *state=
      (operation->opcode==GML_RENDER_OP_CLEAR ||
       operation->opcode==GML_RENDER_OP_COLOURED_PRIMITIVE)
      ?NULL:&batch->states[operation->state_index];
    const GmlRenderResourceSource *source=operation->source_count
      ?find_input(sources,source_count,operation->sources[0]):NULL;
    if(!gml_render_cpu_replay_one(copy,resource->width,resource->height,
                                  operation,state,source)){
      free(copy);
      return GML_RENDER_RESOURCE_EXEC_FAILED;
    }
  }
  memcpy(resource->pixels,copy,bytes);
  free(copy);
  clear_history(resource);
  resource->image.content_version=batch->operations[batch->operation_count-1u]
                                  .target.image.content_version;
  resource->cpu_version=resource->image.content_version;
  resource->gpu_version=0;
  resource->device_identity=NULL;
  resource->authority=GML_RENDER_RESOURCE_CPU_CURRENT;
  resource->cpu_fallbacks++;
  return GML_RENDER_RESOURCE_EXEC_CPU;
}

static int batch_valid(const GmlRenderResource *resource,const GmlRenderDrawBatch *batch,
                       const GmlRenderResourceSource *sources,uint32_t source_count,
                       int *checkpoint_required){
  uint64_t version=resource->image.content_version;
  if(!batch || !batch->operations || !batch->operation_count ||
     batch->operation_count>8192u) return 0;
  *checkpoint_required=0;
  for(uint32_t index=0;index<batch->operation_count;index++){
    const GmlRenderDrawOp *operation=&batch->operations[index];
    const GmlRenderDrawStateSnapshot *state=NULL;
    const GmlRenderResourceSource *source=NULL;
    if(version==UINT64_MAX || !same_logical(operation->target.image,resource->image) ||
       operation->target.image.content_version!=version+1u ||
       operation->target.width!=resource->width ||
       operation->target.height!=resource->height ||
       operation->target.pixel_format!=resource->pixel_format ||
       operation->target.storage_version!=resource->storage_version) return 0;
    version++;
    if(operation->opcode==GML_RENDER_OP_CLEAR ||
       operation->opcode==GML_RENDER_OP_COLOURED_PRIMITIVE){
      if(operation->source_count || operation->sources) return 0;
    } else {
      if(!batch->states || operation->state_index>=batch->state_count ||
         !operation->source_count || !operation->sources) return 0;
      if(operation->opcode==GML_RENDER_OP_CONTENT_SHADER_DRAW){
        if(!operation->payload.shader ||
           operation->payload.shader->sampler_count>GML_PLAN_MAX_SAMPLERS ||
           operation->source_count!=1u+operation->payload.shader->sampler_count) return 0;
      } else if(operation->source_count!=1u) return 0;
      state=&batch->states[operation->state_index];
      source=find_input(sources,source_count,operation->sources[0]);
      if(!source) return 0;
      for(uint32_t bound=1;bound<operation->source_count;bound++)
        if(!find_input(sources,source_count,operation->sources[bound])) return 0;
    }
    if(operation->opcode==GML_RENDER_OP_CONTENT_SHADER_DRAW &&
       (!operation->payload.shader ||
        operation->reconstruction_mode!=GML_RENDER_DEVICE_CHECKPOINT)) return 0;
    if(operation->reconstruction_mode==GML_RENDER_REPLAY_EXACT){
      if(source && !source->bytes) *checkpoint_required=1;
      else if(!gml_render_cpu_replay_eligible(operation,state,source)) return 0;
    } else if(operation->reconstruction_mode==GML_RENDER_DEVICE_CHECKPOINT){
      *checkpoint_required=1;
    } else return 0;
  }
  return 1;
}

static int upload_sources(const GmlRenderDeviceServices *device,
                           const GmlRenderDrawBatch *batch,
                           const GmlRenderResourceSource *sources,uint32_t source_count){
  for(uint32_t index=0;index<batch->operation_count;index++){
    const GmlRenderDrawOp *operation=&batch->operations[index];
    if(!operation->source_count) continue;
    for(uint32_t bound=0;bound<operation->source_count;bound++){
      const GmlRenderResourceSource *view;
      GmlRenderImageUpload transfer={0};
      int seen=0;
      /* A compatible quad run reuses the immediately preceding source. Avoid walking the
       * whole batch for every operation; mixed-source batches still use the original search. */
      if(index && batch->operations[index-1u].source_count &&
         same_version(batch->operations[index-1u].sources[0],operation->sources[bound]))
        seen=1;
      else for(uint32_t earlier=0;earlier<index && !seen;earlier++)
        for(uint32_t previous=0;previous<batch->operations[earlier].source_count;previous++)
          if(same_version(batch->operations[earlier].sources[previous],
                          operation->sources[bound])){ seen=1; break; }
      if(!seen) for(uint32_t previous=0;previous<bound;previous++)
        if(same_version(operation->sources[previous],operation->sources[bound])){
          seen=1; break;
        }
      if(seen) continue;
      view=find_input(sources,source_count,operation->sources[bound]);
      if(!view) return 0;
      if(view->resident_resource &&
         view->resident_resource->gpu_version==view->image.content_version &&
         view->resident_resource->device_identity==device->context) continue;
      transfer.image=view->image;
      transfer.storage_width=view->width;
      transfer.storage_height=view->height;
      transfer.region=(GmlRenderRect){0,0,view->width,view->height};
      transfer.pixel_format=view->pixel_format;
      transfer.stride_bytes=view->stride_bytes;
      transfer.byte_capacity=view->byte_capacity;
      transfer.bytes=view->bytes;
      if(!device->upload(device->context,&transfer)) return 0;
    }
  }
  return 1;
}

static int ensure_target_current(GmlRenderResource *resource,
                                  const GmlRenderDeviceServices *device,
                                  const GmlRenderDrawBatch *batch){
  GmlRenderImageUpload transfer={0};
  if(batch->operations[0].opcode==GML_RENDER_OP_CLEAR &&
     batch->operations[0].target.region.x==0 &&
     batch->operations[0].target.region.y==0 &&
     batch->operations[0].target.region.width==resource->width &&
     batch->operations[0].target.region.height==resource->height) return 1;
  if(resource->gpu_version==resource->image.content_version &&
     resource->device_identity==device->context) return 1;
  if(!gml_render_resource_acquire_cpu_read(resource,NULL)) return 0;
  transfer.image=resource->image;
  transfer.storage_width=resource->width;
  transfer.storage_height=resource->height;
  transfer.region=(GmlRenderRect){0,0,resource->width,resource->height};
  transfer.pixel_format=resource->pixel_format;
  transfer.stride_bytes=resource->width*4u;
  transfer.byte_capacity=(size_t)resource->width*resource->height*4u;
  transfer.bytes=resource->pixels;
  if(!device->upload(device->context,&transfer)) return 0;
  resource->gpu_version=resource->image.content_version;
  resource->device_identity=device->context;
  resource->authority=GML_RENDER_RESOURCE_BOTH_CURRENT;
  return 1;
}

static int append_history(GmlRenderResource *resource,const GmlRenderDrawBatch *batch,
                           const GmlRenderResourceSource *inputs,uint32_t input_count){
  for(uint32_t index=0;index<batch->operation_count;index++){
    const GmlRenderDrawOp *operation=&batch->operations[index];
    JournalEntry *entry=&resource->entries[resource->entry_count++];
    memset(entry,0,sizeof *entry);
    entry->operation=*operation;
    entry->operation.sources=NULL;
    if(operation->opcode!=GML_RENDER_OP_CLEAR &&
       operation->opcode!=GML_RENDER_OP_COLOURED_PRIMITIVE){
      int found=0;
      entry->state=batch->states[operation->state_index];
      entry->source=operation->sources[0];
      for(uint32_t source_index=0;source_index<resource->source_count;source_index++)
        if(same_version(resource->sources[source_index].view.image,entry->source)){
          entry->source_index=source_index;
          found=1;
          break;
        }
      if(!found) return 0;
    }
  }
  (void)inputs;(void)input_count;
  return 1;
}

/* A single exact full-target clear supersedes every previous reconstruction record. Keep the
 * already-reserved entry array for the new clear; release only its old source pins and cursor. */
static void discard_superseded_history(GmlRenderResource *resource){
  for(uint32_t index=0;index<resource->source_count;index++)
    free(resource->sources[index].owned);
  free(resource->sources);
  resource->sources=NULL;
  resource->source_count=0;
  resource->retained_bytes=0;
  resource->entry_count=0;
}

int gml_render_resource_checkpoint(GmlRenderResource *resource,
                                    const GmlRenderDeviceServices *device){
  GmlRenderImageReadback readback={0};
  uint32_t *checkpoint;
  if(!resource) return 0;
  if(resource->authority!=GML_RENDER_RESOURCE_GPU_PENDING_CHECKPOINT) return 1;
  if(!device) device=&resource->checkpoint_device;
  if(!device || !device->readback || !device->context ||
     device->context!=resource->device_identity ||
     resource->gpu_version!=resource->image.content_version){
    resource->authority=GML_RENDER_RESOURCE_INVALID;
    return 0;
  }
  checkpoint=(uint32_t*)malloc((size_t)resource->width*resource->height*4u);
  if(!checkpoint){ resource->authority=GML_RENDER_RESOURCE_INVALID; return 0; }
  readback.image=resource->image;
  readback.storage_width=resource->width;
  readback.storage_height=resource->height;
  readback.region=(GmlRenderRect){0,0,resource->width,resource->height};
  readback.pixel_format=resource->pixel_format;
  readback.stride_bytes=resource->width*4u;
  readback.byte_capacity=(size_t)resource->width*resource->height*4u;
  readback.bytes=checkpoint;
  if(!device->readback(device->context,&readback)){
    free(checkpoint);
    resource->authority=GML_RENDER_RESOURCE_INVALID;
    return 0;
  }
  memcpy(resource->pixels,checkpoint,readback.byte_capacity);
  free(checkpoint);
  clear_history(resource);
  resource->cpu_version=resource->image.content_version;
  resource->authority=GML_RENDER_RESOURCE_BOTH_CURRENT;
  resource->device_checkpoints++;
  resource->cpu_write_acquired=0;
  memset(&resource->checkpoint_device,0,sizeof resource->checkpoint_device);
  return 1;
}

static GmlRenderResourceExecution resource_submit(
  GmlRenderResource *resource,const GmlRenderDeviceServices *device,
  const GmlRenderDrawBatch *batch,const GmlRenderResourceSource *sources,
  uint32_t source_count,int defer_checkpoint){
  GmlRenderExecutionResult device_result;
  uint32_t old_source_count;
  size_t old_retained_bytes;
  int checkpoint_required,replaces_history;
  if(!resource || resource->authority==GML_RENDER_RESOURCE_INVALID ||
     resource->cpu_write_acquired ||
     !batch_valid(resource,batch,sources,source_count,&checkpoint_required))
    return GML_RENDER_RESOURCE_EXEC_FAILED;
  if(resource->authority==GML_RENDER_RESOURCE_GPU_PENDING_CHECKPOINT &&
     !gml_render_resource_checkpoint(resource,device))
    return GML_RENDER_RESOURCE_EXEC_FAILED;
  replaces_history=!checkpoint_required && batch->operation_count==1u &&
    batch->operations[0].opcode==GML_RENDER_OP_CLEAR &&
    batch->operations[0].target.region.x==0 &&
    batch->operations[0].target.region.y==0 &&
    batch->operations[0].target.region.width==resource->width &&
    batch->operations[0].target.region.height==resource->height;
  if(!device || !device->submit || !device->upload || !device->readback){
    return checkpoint_required?GML_RENDER_RESOURCE_EXEC_FAILED:
      cpu_fallback(resource,batch,sources,source_count);
  }
  if(!checkpoint_required && !replaces_history && resource->entry_count &&
     ((uint64_t)resource->entry_count+batch->operation_count>
      resource->limits.journal_bytes/sizeof(JournalEntry) ||
      resource->retained_bytes>resource->limits.retained_mutable_bytes)){
    if(!gml_render_resource_acquire_cpu_read(resource,NULL))
      return GML_RENDER_RESOURCE_EXEC_FAILED;
    resource->consolidations++;
  }
  if(!checkpoint_required &&
     batch->operation_count>resource->limits.journal_bytes/sizeof(JournalEntry))
    return cpu_fallback(resource,batch,sources,source_count);
  if(!ensure_target_current(resource,device,batch))
    return checkpoint_required?GML_RENDER_RESOURCE_EXEC_FAILED:
      cpu_fallback(resource,batch,sources,source_count);
  old_source_count=resource->source_count;
  old_retained_bytes=resource->retained_bytes;
  if(!checkpoint_required){
    JournalEntry *entries=(JournalEntry*)realloc(resource->entries,
      (size_t)(resource->entry_count+batch->operation_count)*sizeof *entries);
    if(!entries) return cpu_fallback(resource,batch,sources,source_count);
    resource->entries=entries;
    for(uint32_t index=0;index<batch->operation_count;index++){
      const GmlRenderDrawOp *operation=&batch->operations[index];
      const GmlRenderResourceSource *view;
      int found=0;
      if(!operation->source_count) continue;
      for(uint32_t existing=0;existing<resource->source_count;existing++)
        if(same_version(resource->sources[existing].view.image,operation->sources[0])){
          found=1;break;
        }
      if(found) continue;
      view=find_input(sources,source_count,operation->sources[0]);
      if(!view || resource->source_count==UINT32_MAX) goto staging_failed;
      {
        PinnedSource *grown=(PinnedSource*)realloc(resource->sources,
          (size_t)(resource->source_count+1u)*sizeof *grown);
        if(!grown) goto staging_failed;
        resource->sources=grown;
      }
      if(!pin_source(&resource->sources[resource->source_count],view)) goto staging_failed;
      if(resource->sources[resource->source_count].charged_bytes>
         resource->limits.retained_mutable_bytes-resource->retained_bytes){
        free(resource->sources[resource->source_count].owned);
        memset(&resource->sources[resource->source_count],0,
               sizeof resource->sources[resource->source_count]);
        goto staging_failed;
      }
      resource->retained_bytes+=resource->sources[resource->source_count].charged_bytes;
      resource->source_count++;
    }
  }
  if(!upload_sources(device,batch,sources,source_count)) goto staging_failed;
  device_result=device->submit(device->context,batch);
  if(device_result.status!=GML_RENDER_EXEC_ACCEPTED ||
     device_result.accepted_prefix!=batch->operation_count){
    if(device->retire) device->retire(device->context,resource->image);
    resource->gpu_version=0;
    resource->device_identity=NULL;
    goto staging_failed;
  }
  resource->image.content_version=batch->operations[batch->operation_count-1u]
                                  .target.image.content_version;
  resource->gpu_version=resource->image.content_version;
  resource->device_identity=device->context;
  if(replaces_history) discard_superseded_history(resource);
  if(checkpoint_required){
    resource->authority=GML_RENDER_RESOURCE_GPU_PENDING_CHECKPOINT;
    resource->checkpoint_device=*device;
    if(!defer_checkpoint && !gml_render_resource_checkpoint(resource,device))
      return GML_RENDER_RESOURCE_EXEC_FAILED;
  } else {
    if(!append_history(resource,batch,sources,source_count)){
      resource->authority=GML_RENDER_RESOURCE_INVALID;
      return GML_RENDER_RESOURCE_EXEC_FAILED;
    }
    resource->authority=GML_RENDER_RESOURCE_GPU_REPLAYABLE;
  }
  return GML_RENDER_RESOURCE_EXEC_DEVICE;
staging_failed:
  for(uint32_t index=old_source_count;index<resource->source_count;index++)
    free(resource->sources[index].owned);
  resource->source_count=old_source_count;
  resource->retained_bytes=old_retained_bytes;
  return checkpoint_required?GML_RENDER_RESOURCE_EXEC_FAILED:
    cpu_fallback(resource,batch,sources,source_count);
}

GmlRenderResourceExecution gml_render_resource_submit(
  GmlRenderResource *resource,const GmlRenderDeviceServices *device,
  const GmlRenderDrawBatch *batch,const GmlRenderResourceSource *sources,
  uint32_t source_count){
  return resource_submit(resource,device,batch,sources,source_count,0);
}

GmlRenderResourceExecution gml_render_resource_submit_deferred_shader(
  GmlRenderResource *resource,const GmlRenderDeviceServices *device,
  const GmlRenderDrawBatch *batch,const GmlRenderResourceSource *sources,
  uint32_t source_count){
  if(!batch || batch->operation_count!=1u ||
     batch->operations[0].opcode!=GML_RENDER_OP_CONTENT_SHADER_DRAW ||
     batch->operations[0].reconstruction_mode!=GML_RENDER_DEVICE_CHECKPOINT)
    return GML_RENDER_RESOURCE_EXEC_FAILED;
  return resource_submit(resource,device,batch,sources,source_count,1);
}

void gml_render_resource_journal_metrics(const GmlRenderResource *resource,
                                          GmlRenderJournalMetrics *out_metrics){
  if(!out_metrics) return;
  memset(out_metrics,0,sizeof *out_metrics);
  if(!resource) return;
  out_metrics->logical_version=resource->image.content_version;
  out_metrics->cpu_version=resource->cpu_version;
  out_metrics->gpu_version=resource->gpu_version;
  out_metrics->cpu_replays=resource->cpu_replays;
  out_metrics->device_checkpoints=resource->device_checkpoints;
  out_metrics->consolidations=resource->consolidations;
  out_metrics->cpu_fallbacks=resource->cpu_fallbacks;
  out_metrics->journal_bytes=(size_t)resource->entry_count*sizeof(JournalEntry);
  out_metrics->retained_mutable_bytes=resource->retained_bytes;
  out_metrics->journal_operations=resource->entry_count;
  out_metrics->retained_versions=resource->source_count;
  out_metrics->authority=resource->authority;
}
