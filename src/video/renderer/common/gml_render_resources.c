/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Canonical pixel authority and bounded resolved-operation retention. A device name is never
 * canonical state: the existing surface owns pixels, and this owner tracks how to update them. */
#include "gml_render_resources.h"
#include "gml_render_cpu_replay.h"
#include "gml_render_plan.h"
#include "gml_render.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

enum { DEFAULT_JOURNAL_BYTES=8u*1024u*1024u,
       DEFAULT_RETAINED_BYTES=64u*1024u*1024u,
       DEFAULT_CHECKPOINT_STAGING_BYTES=64u*1024u*1024u,
       MAX_REPLAY_RECIPE_DEPTH=8u };

typedef struct ReplayRecipe ReplayRecipe;

typedef struct PinnedSource {
  GmlRenderResourceSource view;
  uint8_t *owned;
  size_t charged_bytes;
  size_t recipe_bytes;
  ReplayRecipe *recipe;
} PinnedSource;

typedef struct JournalEntry {
  GmlRenderDrawOp operation;
  GmlRenderDrawStateSnapshot state;
  GmlRenderImageRef source;
  uint32_t source_index;
} JournalEntry;

/* Immutable version, rooted at an exact full clear. No completed pixel plane is retained.
 * Source versions form a bounded DAG; materialisation uses the existing CPU replay owner. */
struct ReplayRecipe {
  GmlRenderImageRef image;
  uint32_t width,height,entry_count,source_count,depth,references;
  JournalEntry *entries;
  PinnedSource *sources;
  size_t journal_bytes,retained_bytes;
};

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
  size_t retained_bytes,recipe_bytes;
  ReplayRecipe *frozen_recipe;
  uint32_t authority;
  int cpu_write_acquired;
  const void *device_identity;
  GmlRenderDeviceServices checkpoint_device;
  uint32_t *checkpoint_staging;
  uint64_t checkpoint_staging_version;
  const void *checkpoint_staging_device;
  uint64_t cpu_replays,device_checkpoints,consolidations,cpu_fallbacks;
  uint64_t checkpoint_allocations;
  GmlRender *dispatch_owner;
};

static void release_recipe(ReplayRecipe *recipe);
static ReplayRecipe *retain_resource_recipe(GmlRenderResource *resource);
static int materialise_pin(PinnedSource *pin,GmlRender *dispatch_owner);

static void release_pin(PinnedSource *pin){
  free(pin->owned);
  release_recipe(pin->recipe);
  memset(pin,0,sizeof *pin);
}

static void release_recipe(ReplayRecipe *recipe){
  if(!recipe || --recipe->references) return;
  for(uint32_t index=0;index<recipe->source_count;index++)
    release_pin(&recipe->sources[index]);
  free(recipe->sources);
  free(recipe->entries);
  free(recipe);
}

static void discard_frozen_recipe(GmlRenderResource *resource){
  release_recipe(resource->frozen_recipe);
  resource->frozen_recipe=NULL;
}

static int recipe_available(const GmlRenderResource *resource){
  const GmlRenderDrawOp *first;
  if(!resource || resource->authority!=GML_RENDER_RESOURCE_GPU_REPLAYABLE ||
     !resource->entry_count) return 0;
  first=&resource->entries[0].operation;
  return first->opcode==GML_RENDER_OP_CLEAR &&
    first->reconstruction_mode==GML_RENDER_REPLAY_EXACT &&
    first->target.region.x==0 && first->target.region.y==0 &&
    first->target.region.width==resource->width &&
    first->target.region.height==resource->height;
}

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
           (resident->authority==GML_RENDER_RESOURCE_GPU_RESIDENT ||
            resident->authority==GML_RENDER_RESOURCE_GPU_PENDING_CHECKPOINT ||
            resident->authority==GML_RENDER_RESOURCE_GPU_REPLAYABLE ||
            resident->authority==GML_RENDER_RESOURCE_BOTH_CURRENT) &&
           !resident->cpu_write_acquired &&
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
  if(!view->bytes){
    pin->recipe=retain_resource_recipe((GmlRenderResource*)view->resident_resource);
    if(!pin->recipe) return 0;
    pin->view.resident_resource=NULL;
    pin->recipe_bytes=pin->recipe->journal_bytes;
    pin->charged_bytes=pin->recipe->retained_bytes;
    return 1;
  }
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

static ReplayRecipe *retain_resource_recipe(GmlRenderResource *resource){
  ReplayRecipe *recipe;
  if(!recipe_available(resource)) return NULL;
  if(resource->frozen_recipe){
    if(resource->frozen_recipe->references==UINT32_MAX) return NULL;
    resource->frozen_recipe->references++;
    return resource->frozen_recipe;
  }
  recipe=(ReplayRecipe*)calloc(1,sizeof *recipe);
  if(!recipe) return NULL;
  recipe->references=1u;
  recipe->image=resource->image;
  recipe->width=resource->width; recipe->height=resource->height;
  recipe->depth=1u;
  recipe->journal_bytes=sizeof *recipe+
    (size_t)resource->entry_count*sizeof *recipe->entries+
    (size_t)resource->source_count*sizeof *recipe->sources;
  if(recipe->journal_bytes>resource->limits.journal_bytes) goto failed;
  recipe->entries=(JournalEntry*)malloc((size_t)resource->entry_count*sizeof *recipe->entries);
  if(!recipe->entries) goto failed;
  memcpy(recipe->entries,resource->entries,(size_t)resource->entry_count*sizeof *recipe->entries);
  recipe->entry_count=resource->entry_count;
  if(resource->source_count){
    recipe->sources=(PinnedSource*)calloc(resource->source_count,sizeof *recipe->sources);
    if(!recipe->sources) goto failed;
  }
  for(uint32_t index=0;index<resource->source_count;index++){
    PinnedSource *pin=&recipe->sources[index];
    const PinnedSource *original=&resource->sources[index];
    if(original->recipe){
      if(original->recipe->references==UINT32_MAX ||
         original->recipe->depth>=MAX_REPLAY_RECIPE_DEPTH)
        goto failed;
      pin->view=original->view;
      pin->view.bytes=NULL; pin->view.byte_capacity=0u;
      pin->recipe=original->recipe; pin->recipe->references++;
      pin->recipe_bytes=original->recipe_bytes;
      pin->charged_bytes=original->charged_bytes;
      if(recipe->depth<=pin->recipe->depth) recipe->depth=pin->recipe->depth+1u;
    } else if(!pin_source(pin,&original->view)) goto failed;
    recipe->source_count++;
    if(pin->recipe_bytes>resource->limits.journal_bytes-recipe->journal_bytes ||
       pin->charged_bytes>resource->limits.retained_mutable_bytes-recipe->retained_bytes)
      goto failed;
    recipe->journal_bytes+=pin->recipe_bytes;
    recipe->retained_bytes+=pin->charged_bytes;
  }
  resource->frozen_recipe=recipe;
  recipe->references++;
  return recipe;
failed:
  release_recipe(recipe);
  return NULL;
}

static int replay_entries(JournalEntry *entries,uint32_t entry_count,
                          PinnedSource *sources,uint32_t width,uint32_t height,
                          uint32_t *destination,GmlRender *dispatch_owner){
  for(uint32_t index=0;index<entry_count;index++){
    const JournalEntry *entry=&entries[index];
    PinnedSource *pin=entry->operation.source_count?&sources[entry->source_index]:NULL;
    PinnedSource materialised={0};
    GmlRenderDrawOp operation=entry->operation;
    if(pin && !pin->view.bytes){
      materialised=*pin;
      materialised.owned=NULL;
      if(!materialise_pin(&materialised,dispatch_owner)) return 0;
      pin=&materialised;
    }
    operation.sources=operation.source_count?&entry->source:NULL;
    int applied=gml_render_cpu_replay_one(destination,width,height,&operation,&entry->state,
                                          pin?&pin->view:NULL);
    gml_render_dispatch_cpu_replay_completion(dispatch_owner,&operation,applied);
    free(materialised.owned);
    if(!applied) return 0;
  }
  return 1;
}

static int materialise_pin(PinnedSource *pin,GmlRender *dispatch_owner){
  size_t bytes;
  uint32_t *pixels;
  if(pin->view.bytes) return 1;
  if(!pin->recipe || (size_t)pin->view.width>SIZE_MAX/4u/pin->view.height) return 0;
  bytes=(size_t)pin->view.width*pin->view.height*4u;
  pixels=(uint32_t*)calloc(1,bytes);
  if(!pixels) return 0;
  if(!replay_entries(pin->recipe->entries,pin->recipe->entry_count,pin->recipe->sources,
                     pin->recipe->width,pin->recipe->height,pixels,dispatch_owner)){
    free(pixels);
    return 0;
  }
  pin->owned=(uint8_t*)pixels;
  pin->view.bytes=pixels;
  pin->view.byte_capacity=bytes;
  return 1;
}

static void clear_history(GmlRenderResource *resource){
  for(uint32_t index=0;index<resource->source_count;index++)
    release_pin(&resource->sources[index]);
  discard_frozen_recipe(resource);
  free(resource->sources);
  free(resource->entries);
  resource->sources=NULL;
  resource->entries=NULL;
  resource->source_count=resource->entry_count=0;
  resource->retained_bytes=resource->recipe_bytes=0;
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
  resource->limits.checkpoint_staging_bytes=limits.checkpoint_staging_bytes
    ?limits.checkpoint_staging_bytes:DEFAULT_CHECKPOINT_STAGING_BYTES;
  resource->authority=GML_RENDER_RESOURCE_CPU_CURRENT;
  return resource;
}

void gml_render_resource_destroy(GmlRenderResource *resource){
  if(!resource) return;
  clear_history(resource);
  free(resource->checkpoint_staging);
  free(resource);
}

void gml_render_resource_bind_dispatch(GmlRenderResource *resource,GmlRender *render){
  if(resource) resource->dispatch_owner=render;
}

GmlRenderImageRef gml_render_resource_image_ref(const GmlRenderResource *resource){
  GmlRenderImageRef image={0};
  return resource?resource->image:image;
}

static int replay_history(const GmlRenderResource *resource,uint32_t *destination){
  return replay_entries(resource->entries,resource->entry_count,resource->sources,
                         resource->width,resource->height,destination,resource->dispatch_owner);
}

int gml_render_resource_acquire_cpu_read(GmlRenderResource *resource,
                                          const uint32_t **out_pixels){
  uint32_t *candidate;
  size_t bytes;
  if(out_pixels) *out_pixels=NULL;
  if(!resource || !resource->pixels || resource->authority==GML_RENDER_RESOURCE_INVALID) return 0;
  if((resource->authority==GML_RENDER_RESOURCE_GPU_RESIDENT ||
      resource->authority==GML_RENDER_RESOURCE_GPU_PENDING_CHECKPOINT) &&
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

int gml_render_resource_current_cpu_source_view(
  GmlRenderResource *resource,GmlRenderResourceSource *out_source){
  if(out_source) memset(out_source,0,sizeof *out_source);
  if(!out_source || !resource || resource->cpu_write_acquired ||
     (resource->authority!=GML_RENDER_RESOURCE_CPU_CURRENT &&
      resource->authority!=GML_RENDER_RESOURCE_BOTH_CURRENT) ||
     resource->cpu_version!=resource->image.content_version) return 0;
  return gml_render_resource_source_view(resource,out_source);
}

int gml_render_resource_device_source_view(
  GmlRenderResource *resource,const GmlRenderDeviceServices *device,
  GmlRenderResourceSource *out_source){
  if(out_source) memset(out_source,0,sizeof *out_source);
  if(!out_source || !resource || !device || !device->context ||
     (resource->authority!=GML_RENDER_RESOURCE_GPU_RESIDENT &&
      resource->authority!=GML_RENDER_RESOURCE_GPU_PENDING_CHECKPOINT &&
      resource->authority!=GML_RENDER_RESOURCE_GPU_REPLAYABLE &&
      resource->authority!=GML_RENDER_RESOURCE_BOTH_CURRENT) ||
     resource->cpu_write_acquired || !resource->width || !resource->height ||
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
  discard_frozen_recipe(resource);
  resource->cpu_version=resource->image.content_version;
  resource->authority=GML_RENDER_RESOURCE_CPU_CURRENT;
  resource->cpu_write_acquired=0;
  return 1;
}

void gml_render_resource_context_lost(GmlRenderResource *resource){
  if(!resource) return;
  if(resource->authority==GML_RENDER_RESOURCE_GPU_RESIDENT ||
     resource->authority==GML_RENDER_RESOURCE_GPU_PENDING_CHECKPOINT)
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
    PinnedSource pin={0};
    GmlRenderResourceSource observed_source={0};
    if(source && !source->bytes){
      if(pin_source(&pin,source)){
        if(!materialise_pin(&pin,resource->dispatch_owner)){
          release_pin(&pin); free(copy); return GML_RENDER_RESOURCE_EXEC_FAILED;
        }
        source=&pin.view;
      } else {
        /* Recipe pressure is a CPU boundary, not lost source data. Observe the still-current
         * owner only here, before destination publication; ordinary device retention stays
         * byte-free and no over-budget recipe is manufactured for the fallback. */
        if(!gml_render_resource_source_view((GmlRenderResource*)source->resident_resource,
                                             &observed_source)){
          release_pin(&pin); free(copy); return GML_RENDER_RESOURCE_EXEC_FAILED;
        }
        source=&observed_source;
      }
    }
    if(!gml_render_cpu_replay_one(copy,resource->width,resource->height,
                                  operation,state,source)){
      release_pin(&pin);
      free(copy);
      return GML_RENDER_RESOURCE_EXEC_FAILED;
    }
    release_pin(&pin);
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
      if(source && !source->bytes && !recipe_available(source->resident_resource))
        *checkpoint_required=1;
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
    release_pin(&resource->sources[index]);
  discard_frozen_recipe(resource);
  free(resource->sources);
  resource->sources=NULL;
  resource->source_count=0;
  resource->retained_bytes=resource->recipe_bytes=0;
  resource->entry_count=0;
}

static int resource_checkpoint(GmlRenderResource *resource,
                                 const GmlRenderDeviceServices *device,
                                 int observe_replayable){
  GmlRenderImageReadback readback={0};
  GmlRenderImageReadbackResult result={0};
  uint32_t *checkpoint;
  size_t bytes;
  if(!resource || resource->authority==GML_RENDER_RESOURCE_INVALID) return 0;
  int pending=resource->authority==GML_RENDER_RESOURCE_GPU_PENDING_CHECKPOINT;
  int resident=resource->authority==GML_RENDER_RESOURCE_GPU_RESIDENT;
  if(!pending && !resident &&
     (!observe_replayable || resource->authority!=GML_RENDER_RESOURCE_GPU_REPLAYABLE))
    return 1;
  if(!device && !observe_replayable) device=&resource->checkpoint_device;
  if(!device || !device->readback || !device->context ||
     device->context!=resource->device_identity ||
     resource->gpu_version!=resource->image.content_version){
    if(pending && !observe_replayable) resource->authority=GML_RENDER_RESOURCE_INVALID;
    return 0;
  }
  bytes=(size_t)resource->width*resource->height*4u;
  checkpoint=resource->checkpoint_staging;
  if(!checkpoint){
    checkpoint=(uint32_t*)malloc(bytes);
    if(!checkpoint){
      if(pending && !observe_replayable) resource->authority=GML_RENDER_RESOURCE_INVALID;
      return 0;
    }
    resource->checkpoint_allocations++;
    /* Extents are immutable for this owner. Keep one bounded host plane for the two owned
     * frame targets; other resources and over-budget targets retain transient allocation.
     * Staging is scratch, not a checkpoint: only a complete successful read is published. */
    if((resource->image.resource_class==GML_RENDER_RESOURCE_SCREEN ||
        resource->image.resource_class==GML_RENDER_RESOURCE_APPLICATION) &&
       bytes<=resource->limits.checkpoint_staging_bytes)
      resource->checkpoint_staging=checkpoint;
  }
  readback.image=resource->image;
  readback.storage_width=resource->width;
  readback.storage_height=resource->height;
  readback.region=(GmlRenderRect){0,0,resource->width,resource->height};
  readback.pixel_format=resource->pixel_format;
  readback.stride_bytes=resource->width*4u;
  readback.byte_capacity=bytes;
  readback.bytes=checkpoint;
  result.updated_region=readback.region;
  readback.result=&result;
  if(checkpoint==resource->checkpoint_staging){
    readback.checkpoint=1u;
    if(resource->checkpoint_staging_device==device->context &&
       resource->checkpoint_staging_version==resource->cpu_version)
      readback.checkpoint_baseline_version=resource->checkpoint_staging_version;
  }
  if(!device->readback(device->context,&readback) ||
     (result.updated_region.width &&
      (result.updated_region.x<0 || result.updated_region.y<0 || !result.updated_region.height ||
       (uint64_t)(uint32_t)result.updated_region.x+result.updated_region.width>resource->width ||
       (uint64_t)(uint32_t)result.updated_region.y+result.updated_region.height>resource->height)) ||
     (!readback.checkpoint_baseline_version &&
      (result.updated_region.x!=0 || result.updated_region.y!=0 ||
       result.updated_region.width!=resource->width || result.updated_region.height!=resource->height))){
    if(checkpoint!=resource->checkpoint_staging) free(checkpoint);
    resource->checkpoint_staging_version=0;
    resource->checkpoint_staging_device=NULL;
    if(pending && !observe_replayable) resource->authority=GML_RENDER_RESOURCE_INVALID;
    return 0;
  }
  if(result.updated_region.x==0 && result.updated_region.y==0 &&
     result.updated_region.width==resource->width && result.updated_region.height==resource->height)
    memcpy(resource->pixels,checkpoint,readback.byte_capacity);
  else for(uint32_t row=0;row<result.updated_region.height && result.updated_region.width;row++){
    size_t first=(size_t)((uint32_t)result.updated_region.y+row)*resource->width+
                   (uint32_t)result.updated_region.x;
    memcpy(resource->pixels+first,checkpoint+first,(size_t)result.updated_region.width*4u);
  }
  if(checkpoint==resource->checkpoint_staging){
    resource->checkpoint_staging_version=resource->image.content_version;
    resource->checkpoint_staging_device=device->context;
  }
  if(checkpoint!=resource->checkpoint_staging) free(checkpoint);
  clear_history(resource);
  resource->cpu_version=resource->image.content_version;
  resource->authority=GML_RENDER_RESOURCE_BOTH_CURRENT;
  resource->device_checkpoints++;
  resource->cpu_write_acquired=0;
  memset(&resource->checkpoint_device,0,sizeof resource->checkpoint_device);
  return 1;
}

int gml_render_resource_checkpoint(GmlRenderResource *resource,
                                    const GmlRenderDeviceServices *device){
  return resource_checkpoint(resource,device,0);
}

int gml_render_resource_observe_device(GmlRenderResource *resource,
                                        const GmlRenderDeviceServices *device){
  return resource_checkpoint(resource,device,1);
}

static GmlRenderResourceExecution resource_submit(
  GmlRenderResource *resource,const GmlRenderDeviceServices *device,
  const GmlRenderDrawBatch *batch,const GmlRenderResourceSource *sources,
  uint32_t source_count,int defer_checkpoint){
  GmlRenderExecutionResult device_result;
  uint32_t old_source_count;
  size_t old_retained_bytes,old_recipe_bytes;
  int checkpoint_required,replaces_history,pending_prefix,batch_checkpoint_required;
  int resident_policy;
  if(!resource || resource->authority==GML_RENDER_RESOURCE_INVALID ||
     resource->cpu_write_acquired ||
     !batch_valid(resource,batch,sources,source_count,&checkpoint_required))
    return GML_RENDER_RESOURCE_EXEC_FAILED;
  batch_checkpoint_required=checkpoint_required;
  resident_policy=resource->authority==GML_RENDER_RESOURCE_GPU_RESIDENT ||
    (device && device->context && device->observe_on_demand &&
     device->observe_on_demand(device->context));
  if(resident_policy){
    checkpoint_required=1;
    defer_checkpoint=1;
  }
  pending_prefix=resource->authority==GML_RENDER_RESOURCE_GPU_RESIDENT ||
    resource->authority==GML_RENDER_RESOURCE_GPU_PENDING_CHECKPOINT;
  if(pending_prefix && !defer_checkpoint){
    if(!gml_render_resource_checkpoint(resource,device))
      return GML_RENDER_RESOURCE_EXEC_FAILED;
    pending_prefix=0;
  }
  if(pending_prefix){
    if(!device || device->context!=resource->device_identity ||
       resource->gpu_version!=resource->image.content_version){
      resource->authority=GML_RENDER_RESOURCE_INVALID;
      return GML_RENDER_RESOURCE_EXEC_FAILED;
    }
    /* The earlier device-only prefix prevents exact CPU replay of the combined span. Keep
     * its canonical checkpoint intact and seal the final GPU version at the real observer. */
    checkpoint_required=1;
  }
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
      (resource->limits.journal_bytes-resource->recipe_bytes)/sizeof(JournalEntry) ||
      resource->retained_bytes>resource->limits.retained_mutable_bytes)){
    if(!gml_render_resource_acquire_cpu_read(resource,NULL))
      return GML_RENDER_RESOURCE_EXEC_FAILED;
    resource->consolidations++;
  }
  if(!checkpoint_required &&
     batch->operation_count>resource->limits.journal_bytes/sizeof(JournalEntry))
    return cpu_fallback(resource,batch,sources,source_count);
  if(!ensure_target_current(resource,device,batch))
    return batch_checkpoint_required?GML_RENDER_RESOURCE_EXEC_FAILED:
      cpu_fallback(resource,batch,sources,source_count);
  old_source_count=resource->source_count;
  old_retained_bytes=resource->retained_bytes;
  old_recipe_bytes=resource->recipe_bytes;
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
      PinnedSource *pin=&resource->sources[resource->source_count];
      size_t journal_bytes=(size_t)(resource->entry_count+batch->operation_count)*
                             sizeof(JournalEntry)+resource->recipe_bytes;
      if(pin->charged_bytes>resource->limits.retained_mutable_bytes-resource->retained_bytes ||
         journal_bytes>resource->limits.journal_bytes ||
         pin->recipe_bytes>resource->limits.journal_bytes-journal_bytes){
        release_pin(pin);
        goto staging_failed;
      }
      resource->retained_bytes+=pin->charged_bytes;
      resource->recipe_bytes+=pin->recipe_bytes;
      resource->source_count++;
    }
  }
  if(!upload_sources(device,batch,sources,source_count)) goto staging_failed;
  device_result=device->submit(device->context,batch);
  if(device_result.status!=GML_RENDER_EXEC_ACCEPTED ||
     device_result.accepted_prefix!=batch->operation_count){
    if(pending_prefix && device_result.status==GML_RENDER_EXEC_UNSUPPORTED &&
       !device_result.accepted_prefix && !device_result.all_destinations_uncertain &&
       !device_result.uncertain_target_count){
      /* A proven pre-write refusal leaves the earlier resident version intact. It can still
       * be sealed for ordered CPU fallback; a partially failed device result cannot. */
      if(!gml_render_resource_checkpoint(resource,device))
        return GML_RENDER_RESOURCE_EXEC_FAILED;
      return batch_checkpoint_required?GML_RENDER_RESOURCE_EXEC_FAILED:
        cpu_fallback(resource,batch,sources,source_count);
    }
    if(pending_prefix) resource->authority=GML_RENDER_RESOURCE_INVALID;
    if(device->retire) device->retire(device->context,resource->image);
    resource->gpu_version=0;
    resource->device_identity=NULL;
    goto staging_failed;
  }
  discard_frozen_recipe(resource);
  resource->image.content_version=batch->operations[batch->operation_count-1u]
                                  .target.image.content_version;
  resource->gpu_version=resource->image.content_version;
  resource->device_identity=device->context;
  if(replaces_history) discard_superseded_history(resource);
  if(checkpoint_required){
    if(resident_policy) clear_history(resource);
    resource->authority=resident_policy?GML_RENDER_RESOURCE_GPU_RESIDENT:
      GML_RENDER_RESOURCE_GPU_PENDING_CHECKPOINT;
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
    release_pin(&resource->sources[index]);
  resource->source_count=old_source_count;
  resource->retained_bytes=old_retained_bytes;
  resource->recipe_bytes=old_recipe_bytes;
  if(pending_prefix && resource->authority!=GML_RENDER_RESOURCE_INVALID){
    if(!gml_render_resource_checkpoint(resource,device))
      return GML_RENDER_RESOURCE_EXEC_FAILED;
    return batch_checkpoint_required?GML_RENDER_RESOURCE_EXEC_FAILED:
        cpu_fallback(resource,batch,sources,source_count);
  }
  if(resource->authority==GML_RENDER_RESOURCE_INVALID)
    return GML_RENDER_RESOURCE_EXEC_FAILED;
  return batch_checkpoint_required?GML_RENDER_RESOURCE_EXEC_FAILED:
    cpu_fallback(resource,batch,sources,source_count);
}

GmlRenderResourceExecution gml_render_resource_submit(
  GmlRenderResource *resource,const GmlRenderDeviceServices *device,
  const GmlRenderDrawBatch *batch,const GmlRenderResourceSource *sources,
  uint32_t source_count){
  return resource_submit(resource,device,batch,sources,source_count,0);
}

GmlRenderResourceExecution gml_render_resource_submit_deferred(
  GmlRenderResource *resource,const GmlRenderDeviceServices *device,
  const GmlRenderDrawBatch *batch,const GmlRenderResourceSource *sources,
  uint32_t source_count){
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
  out_metrics->journal_bytes=(size_t)resource->entry_count*sizeof(JournalEntry)+
                               resource->recipe_bytes;
  out_metrics->retained_mutable_bytes=resource->retained_bytes;
  if(resource->frozen_recipe){
    out_metrics->cached_replay_bytes=resource->frozen_recipe->journal_bytes+
                                      resource->frozen_recipe->retained_bytes;
    out_metrics->retained_replay_depth=resource->frozen_recipe->depth;
  }
  for(uint32_t index=0;index<resource->source_count;index++)
    if(resource->sources[index].recipe &&
       out_metrics->retained_replay_depth<resource->sources[index].recipe->depth)
      out_metrics->retained_replay_depth=resource->sources[index].recipe->depth;
  out_metrics->checkpoint_staging_bytes=resource->checkpoint_staging
    ?(size_t)resource->width*resource->height*4u:0u;
  out_metrics->checkpoint_allocations=resource->checkpoint_allocations;
  out_metrics->journal_operations=resource->entry_count;
  out_metrics->retained_versions=resource->source_count;
  out_metrics->authority=resource->authority;
}
