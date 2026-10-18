/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Resource authority tests. The fake device verifies transactions and lifetimes; exact GL pixels
 * are checked separately by the real-context scene probe. */
#include "gml_render_resources.h"
#include "gml_render_plan.h"
#include "gml_render_internal.h"
#include "gml_render_state.h"
#include "gml_render_surfaces_internal.h"
#include "gml_render_sprites_internal.h"
#include "gml_render_shaders.h"
#include "gml_render_texture.h"
#include "gml_render_primitives.h"
#include "gml_render_cpu_replay.h"
#include "gml_render_cpu_backend.h"
#include "gml_builtin.h"
#include "gml_particle.h"
#include "gml_software3d.h"

#include <stdint.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if(!(condition)){ \
  fprintf(stderr,"render resources: %s:%d: %s\n",__FILE__,__LINE__,#condition); \
  return 0; \
} } while(0)

typedef struct FakeDevice {
  int on_demand;
  int uploads,submits,shader_submits,readbacks,retires;
  int rgb_queries,fail_rgb_query,rgb_query_result;
  int forbid_calls,forbidden_calls;
  int fail_upload,fail_upload_at,fail_readback,shader_failure,prewrite_refusal;
  int fail_partial_readback;
  int fail_submit_at;
  int incremental_readback;
  uint32_t last_checkpoint;
  uint64_t last_checkpoint_baseline;
  uint32_t status,accepted_prefix;
  uint32_t readback_colour;
  uint32_t max_texture_extent;
  uint64_t supported_operations;
  GmlRenderImageUpload last_upload;
  uint8_t last_upload_rgba[4];
  GmlRenderImageRef last_retired_image;
  double recovery_rect[4];
  uint32_t last_opcode,last_source_count,last_shader_identity;
  uint32_t last_write_mask,last_filter,last_quantisation;
  double last_filtered_draw_alpha;
  GmlRenderDrawStateSnapshot last_quad_state;
  GmlRenderQuad last_quad;
  GmlRenderImageRef last_target_image;
  GmlRenderImageRef submitted_targets[8];
  uint32_t submitted_counts[8];
  GmlRenderRect last_target_region;
  GmlRenderDrawOp last_primitive;
  uint32_t last_batch_operations,last_batch_states;
  uint32_t last_batch_source_ids[3],last_batch_state_indices[3];
  uint32_t last_shader_uniform_count,last_shader_sampler_count;
  float last_shader_value;
} FakeDevice;

static int fake_call_forbidden(FakeDevice *fake){
  if(!fake->forbid_calls) return 0;
  fake->forbidden_calls++;
  return 1;
}

static int fake_upload(void *context,const GmlRenderImageUpload *upload){
  FakeDevice *fake=(FakeDevice*)context;
  if(fake_call_forbidden(fake)) return 0;
  fake->uploads++;
  if(upload) fake->last_upload=*upload;
  if(upload && upload->bytes && upload->byte_capacity>=4u &&
     upload->pixel_format==GML_RENDER_PIXEL_RGBA8888)
    memcpy(fake->last_upload_rgba,upload->bytes,4u);
  return !fake->fail_upload && fake->uploads!=fake->fail_upload_at &&
    upload && upload->bytes && upload->byte_capacity;
}

static GmlRenderExecutionResult fake_submit(void *context,const GmlRenderDrawBatch *batch){
  FakeDevice *fake=(FakeDevice*)context;
  GmlRenderExecutionResult result={0};
  if(fake_call_forbidden(fake)) return result;
  fake->submits++;
  if(batch && batch->operation_count){
    const GmlRenderDrawOp *last=&batch->operations[batch->operation_count-1u];
    if(fake->submits<=8){
      fake->submitted_targets[fake->submits-1]=last->target.image;
      fake->submitted_counts[fake->submits-1]=batch->operation_count;
    }
    fake->last_batch_operations=batch->operation_count;
    fake->last_batch_states=batch->state_count;
    for(uint32_t index=0;index<batch->operation_count && index<3u;index++){
      fake->last_batch_source_ids[index]=batch->operations[index].source_count
        ?batch->operations[index].sources[0].identity:0u;
      fake->last_batch_state_indices[index]=batch->operations[index].state_index;
    }
    fake->last_opcode=last->opcode;
    fake->last_target_image=last->target.image;
    fake->last_target_region=last->target.region;
    fake->last_source_count=last->source_count;
    if(last->opcode==GML_RENDER_OP_COLOURED_PRIMITIVE)
      fake->last_primitive=*last;
    if(last->opcode==GML_RENDER_OP_TEXTURED_QUAD && batch->states &&
       last->state_index<batch->state_count){
      fake->last_write_mask=batch->states[last->state_index].write_mask;
      fake->last_filter=last->payload.quad.sampling.filter;
      fake->last_quantisation=last->payload.quad.sampling.quantisation;
      fake->last_filtered_draw_alpha=last->payload.quad.filtered_draw_alpha;
      fake->last_quad_state=batch->states[last->state_index];
      fake->last_quad=last->payload.quad;
    }
    if(last->opcode==GML_RENDER_OP_RECOVER_EMPTY_SCREEN){
      const GmlRenderQuad *q=&last->payload.quad;
      fake->recovery_rect[0]=q->x[0];fake->recovery_rect[1]=q->y[0];
      fake->recovery_rect[2]=q->x[1]-q->x[0];fake->recovery_rect[3]=q->y[2]-q->y[0];
    }
    if(last->opcode==GML_RENDER_OP_CONTENT_SHADER_DRAW && last->payload.shader){
      fake->shader_submits++;
      fake->last_shader_identity=last->payload.shader->identity;
      fake->last_shader_uniform_count=last->payload.shader->uniform_count;
      fake->last_shader_sampler_count=last->payload.shader->sampler_count;
      fake->last_shader_value=last->payload.shader->uniforms[0].value[0];
    }
  }
  result.status=fake->status?fake->status:GML_RENDER_EXEC_ACCEPTED;
  if(fake->submits==fake->fail_submit_at) result.status=GML_RENDER_EXEC_UNSUPPORTED;
  result.accepted_prefix=fake->accepted_prefix?fake->accepted_prefix:
    (result.status==GML_RENDER_EXEC_ACCEPTED?batch->operation_count:0u);
  result.all_destinations_uncertain=result.status!=GML_RENDER_EXEC_ACCEPTED &&
    !fake->prewrite_refusal;
  return result;
}

static int fake_readback(void *context,const GmlRenderImageReadback *readback){
  FakeDevice *fake=(FakeDevice*)context;
  uint32_t *pixels;
  if(fake_call_forbidden(fake)) return 0;
  fake->readbacks++;
  fake->last_checkpoint=readback?readback->checkpoint:0u;
  fake->last_checkpoint_baseline=readback?readback->checkpoint_baseline_version:0u;
  if(fake->fail_readback || !readback || !readback->bytes ||
     readback->byte_capacity<(size_t)readback->storage_width*readback->storage_height*4u)
    return 0;
  pixels=(uint32_t*)readback->bytes;
  if(fake->fail_partial_readback){ pixels[0]=fake->readback_colour; return 0; }
  if(fake->incremental_readback &&
     (readback->checkpoint_baseline_version || fake->incremental_readback>=5) && readback->result){
    readback->result->updated_region=(GmlRenderRect){1,2,2u,1u};
    if(fake->incremental_readback==2 || fake->incremental_readback==6)
      readback->result->updated_region=(GmlRenderRect){0};
    else {
      pixels[9]=pixels[10]=fake->readback_colour;
      if(fake->incremental_readback==3) return 0;
      if(fake->incremental_readback==4) readback->result->updated_region.x=3;
    }
    return 1;
  }
  for(uint32_t index=0;index<readback->storage_width*readback->storage_height;index++)
    pixels[index]=fake->readback_colour;
  return 1;
}

static void fake_retire(void *context,GmlRenderImageRef image){
  FakeDevice *fake=(FakeDevice*)context;
  if(fake_call_forbidden(fake)) return;
  fake->last_retired_image=image;
  fake->retires++;
}

static int fake_capabilities(void *context,GmlRenderDeviceCaps *out_caps){
  FakeDevice *fake=(FakeDevice*)context;
  if(fake_call_forbidden(fake)) return 0;
  if(!out_caps) return 0;
  memset(out_caps,0,sizeof *out_caps);
  out_caps->max_texture_extent=fake->max_texture_extent?fake->max_texture_extent:4096u;
  out_caps->supported_operations=fake->supported_operations;
  return 1;
}

static int fake_barrier(void *context){
  return !fake_call_forbidden((FakeDevice*)context);
}

static int fake_has_rgb(void *context,GmlRenderImageRef image,int *has_rgb){
  FakeDevice *fake=(FakeDevice*)context;
  if(fake_call_forbidden(fake)) return 0;
  fake->rgb_queries++;
  if(fake->fail_rgb_query || !has_rgb || image.resource_class!=GML_RENDER_RESOURCE_SCREEN)
    return 0;
  *has_rgb=fake->rgb_query_result;
  return 1;
}

static int fake_content_program_failed(void *context,const GmlPlanShader *shader){
  FakeDevice *fake=(FakeDevice*)context;
  if(fake_call_forbidden(fake)) return 0;
  return fake->shader_failure && shader && shader->identity==fake->last_shader_identity;
}

static int fake_observe_on_demand(void *context){
  return ((FakeDevice*)context)->on_demand;
}

static GmlRenderDeviceServices service(FakeDevice *fake){
  GmlRenderDeviceServices device={0};
  device.context=fake;
  device.capabilities=fake_capabilities;
  device.upload=fake_upload;
  device.submit=fake_submit;
  device.readback=fake_readback;
  device.retire=fake_retire;
  device.flush=fake_barrier;
  device.wait=fake_barrier;
  device.content_program_failed=fake_content_program_failed;
  return device;
}

static GmlRenderImageRef target_ref(uint64_t version,uint64_t incarnation){
  GmlRenderImageRef image={GML_RENDER_RESOURCE_SURFACE,1u,incarnation,version};
  return image;
}

static GmlRenderImageRef source_ref(void){
  GmlRenderImageRef image={GML_RENDER_RESOURCE_SPRITE,2u,1u,1u};
  return image;
}

static GmlRenderDrawOp clear_op(uint64_t version,uint32_t colour,uint32_t reconstruction){
  GmlRenderDrawOp operation={0};
  operation.opcode=GML_RENDER_OP_CLEAR;
  operation.reconstruction_mode=reconstruction;
  operation.target.image=target_ref(version,1u);
  operation.target.storage_version=1u;
  operation.target.width=4u;
  operation.target.height=4u;
  operation.target.pixel_format=GML_RENDER_PIXEL_XRGB8888;
  operation.target.region=(GmlRenderRect){0,0,4u,4u};
  operation.payload.clear_colour=colour;
  return operation;
}

static GmlRenderDrawOp flat_rect_op(uint64_t version,GmlRenderRect region,
                                  uint32_t colour){
  GmlRenderDrawOp operation=clear_op(version,0u,GML_RENDER_REPLAY_EXACT);
  operation.opcode=GML_RENDER_OP_COLOURED_PRIMITIVE;
  operation.payload.primitive.region=region;
  operation.payload.primitive.colour=colour;
  return operation;
}

static void fill(uint32_t *pixels,uint32_t colour);

static int source_free_primitive_rect_replay(void){
  uint32_t pixels[16];
  GmlRenderDrawOp operations[2]={
    flat_rect_op(2u,(GmlRenderRect){1,1,2u,2u},0xFF123456u),
    flat_rect_op(3u,(GmlRenderRect){2,0,2u,3u},0xFFABCDEFu)
  };
  GmlRenderDrawBatch batch={operations,2u,NULL,0u};
  GmlRenderJournalMetrics metrics={0};
  FakeDevice fake={0};
  GmlRenderDeviceServices device=service(&fake);
  const uint32_t *observed=NULL;
  GmlRenderResource *resource;
  fill(pixels,0xFF000000u);
  resource=gml_render_resource_create(target_ref(1u,1u),4u,4u,
    GML_RENDER_PIXEL_XRGB8888,pixels,(GmlRenderResourceLimits){0});
  CHECK(resource);
  operations[1].payload.primitive.region=(GmlRenderRect){3,0,2u,3u};
  CHECK(gml_render_resource_submit(resource,&device,&batch,NULL,0u)==
        GML_RENDER_RESOURCE_EXEC_FAILED);
  CHECK(fake.submits==0 && pixels[5]==0xFF000000u);
  operations[1].payload.primitive.region=(GmlRenderRect){2,0,2u,3u};
  CHECK(gml_render_resource_submit(resource,&device,&batch,NULL,0u)==
        GML_RENDER_RESOURCE_EXEC_DEVICE);
  gml_render_resource_journal_metrics(resource,&metrics);
  CHECK(metrics.logical_version==3u && metrics.cpu_version==1u &&
        metrics.gpu_version==3u && metrics.journal_operations==2u &&
        metrics.retained_versions==0u);
  CHECK(fake.submits==1 && fake.uploads==1 && fake.readbacks==0);
  gml_render_resource_context_lost(resource);
  CHECK(gml_render_resource_acquire_cpu_read(resource,&observed));
  CHECK(observed==pixels && pixels[5]==0xFF123456u &&
        pixels[6]==0xFFABCDEFu && pixels[2]==0xFFABCDEFu &&
        pixels[13]==0xFF000000u && fake.readbacks==0);
  gml_render_resource_destroy(resource);
  fill(pixels,0xFF000000u);
  resource=gml_render_resource_create(target_ref(1u,1u),4u,4u,
    GML_RENDER_PIXEL_XRGB8888,pixels,(GmlRenderResourceLimits){0});
  CHECK(resource);
  CHECK(gml_render_resource_submit(resource,NULL,&batch,NULL,0u)==
        GML_RENDER_RESOURCE_EXEC_CPU);
  CHECK(pixels[5]==0xFF123456u && pixels[6]==0xFFABCDEFu &&
        pixels[13]==0xFF000000u);
  gml_render_resource_destroy(resource);
  return 1;
}

static GmlRenderDrawOp quad_op(uint64_t version,const GmlRenderImageRef *source){
  GmlRenderDrawOp operation=clear_op(version,0u,GML_RENDER_REPLAY_EXACT);
  operation.opcode=GML_RENDER_OP_TEXTURED_QUAD;
  operation.source_count=1u;
  operation.sources=source;
  operation.payload.quad.shape=GML_RENDER_QUAD_AXIS_ALIGNED;
  operation.payload.quad.sampling.filter=GML_RENDER_FILTER_NEAREST;
  operation.payload.quad.sampling.edge_rule=GML_RENDER_EDGE_CLAMP_SUBIMAGE;
  operation.payload.quad.sampling.source_region=(GmlRenderRect){0,0,2u,2u};
  for(uint32_t corner=0;corner<4u;corner++)
    operation.payload.quad.corner_colour[corner]=0xFFFFFFFFu;
  operation.payload.quad.x[0]=operation.payload.quad.x[3]=1.0;
  operation.payload.quad.x[1]=operation.payload.quad.x[2]=3.0;
  operation.payload.quad.y[0]=operation.payload.quad.y[1]=1.0;
  operation.payload.quad.y[2]=operation.payload.quad.y[3]=3.0;
  operation.payload.quad.u[0]=operation.payload.quad.u[3]=0.0;
  operation.payload.quad.u[1]=operation.payload.quad.u[2]=1.0;
  operation.payload.quad.v[0]=operation.payload.quad.v[1]=0.0;
  operation.payload.quad.v[2]=operation.payload.quad.v[3]=1.0;
  return operation;
}

static GmlRenderDrawStateSnapshot quad_state(int exact){
  GmlRenderDrawStateSnapshot state={0};
  state.write_mask=15u;
  state.colour=0xFFFFFFFFu;
  state.alpha=255u;
  state.sampler_count=1u;
  state.samplers[0].filter=GML_RENDER_FILTER_NEAREST;
  state.samplers[0].edge_rule=GML_RENDER_EDGE_CLAMP_SUBIMAGE;
  state.samplers[0].source_region=(GmlRenderRect){0,0,2u,2u};
  if(exact){
    state.rgb_source_factor=GML_RENDER_BLEND_SOURCE_ALPHA;
    state.rgb_destination_factor=GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA;
    state.alpha_source_factor=GML_RENDER_BLEND_ONE;
    state.alpha_destination_factor=GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA;
    state.rgb_equation=GML_RENDER_BLEND_EXACT8_TRUNC;
    state.alpha_equation=GML_RENDER_BLEND_EXACT8_TRUNC;
  }
  return state;
}

static GmlRenderResourceSource source_view(const uint32_t *pixels,int immutable){
  GmlRenderResourceSource source={0};
  source.image=source_ref();
  source.width=source.height=2u;
  source.pixel_format=GML_RENDER_PIXEL_ARGB8888;
  source.stride_bytes=8u;
  source.byte_capacity=16u;
  source.bytes=pixels;
  source.borrowed_immutable=immutable;
  return source;
}

static void fill(uint32_t *pixels,uint32_t colour){
  for(int index=0;index<16;index++) pixels[index]=colour;
}

static int nearest_subrectangle_mirrors(void){
  uint32_t source_pixels[12],pixels[16];
  GmlRenderImageRef ref=source_ref();
  GmlRenderResourceSource source=source_view(source_pixels,1);
  for(uint32_t index=0;index<12u;index++) source_pixels[index]=0xFF000000u|(index+1u);
  source.width=4u;
  source.height=3u;
  source.stride_bytes=16u;
  source.byte_capacity=sizeof source_pixels;
  for(unsigned mirror=0;mirror<4u;mirror++){
    FakeDevice fake={0};
    GmlRenderDeviceServices device=service(&fake);
    GmlRenderDrawOp operation=quad_op(2u,&ref);
    GmlRenderDrawStateSnapshot state=quad_state(0);
    GmlRenderDrawBatch batch={&operation,1u,&state,1u};
    GmlRenderResource *resource;
    const uint32_t *observed=NULL;
    GmlRenderRect region={1,1,2u,2u};
    uint32_t expected_top_left=(mirror&2u)?10u:6u;
    if(mirror&1u) expected_top_left++;
    fill(pixels,0xFF000000u);
    operation.target.image=target_ref(2u,mirror+1u);
    operation.payload.quad.sampling.source_region=region;
    state.samplers[0].source_region=region;
    operation.payload.quad.u[0]=operation.payload.quad.u[3]=(mirror&1u)?0.75:0.25;
    operation.payload.quad.u[1]=operation.payload.quad.u[2]=(mirror&1u)?0.25:0.75;
    operation.payload.quad.v[0]=operation.payload.quad.v[1]=(mirror&2u)?1.0:1.0/3.0;
    operation.payload.quad.v[2]=operation.payload.quad.v[3]=(mirror&2u)?1.0/3.0:1.0;
    resource=gml_render_resource_create(target_ref(1u,mirror+1u),4u,4u,
      GML_RENDER_PIXEL_XRGB8888,pixels,(GmlRenderResourceLimits){0});
    CHECK(resource);
    if(!mirror){
      operation.payload.quad.u[2]=0.0;
      CHECK(gml_render_resource_submit(resource,NULL,&batch,&source,1u)==
            GML_RENDER_RESOURCE_EXEC_FAILED);
      CHECK(pixels[5]==0xFF000000u);
      operation.payload.quad.u[2]=0.75;
      operation.payload.quad.x[1]=operation.payload.quad.x[2]=4.0;
      CHECK(gml_render_resource_submit(resource,NULL,&batch,&source,1u)==
            GML_RENDER_RESOURCE_EXEC_FAILED);
      CHECK(pixels[5]==0xFF000000u);
      operation.payload.quad.x[1]=operation.payload.quad.x[2]=3.0;
    }
    CHECK(gml_render_resource_submit(resource,mirror&1u?&device:NULL,&batch,&source,1u)==
          ((mirror&1u)?GML_RENDER_RESOURCE_EXEC_DEVICE:GML_RENDER_RESOURCE_EXEC_CPU));
    if(mirror&1u) gml_render_resource_context_lost(resource);
    CHECK(gml_render_resource_acquire_cpu_read(resource,&observed));
    CHECK(observed==pixels);
    CHECK(pixels[5]==(0xFF000000u|expected_top_left));
    CHECK(pixels[6]==(0xFF000000u|(expected_top_left+((mirror&1u)?-1u:1u))));
    CHECK(pixels[9]==(0xFF000000u|(expected_top_left+((mirror&2u)?-4u:4u))));
    CHECK(pixels[10]==(0xFF000000u|
      (expected_top_left+((mirror&1u)?-1u:1u)+((mirror&2u)?-4u:4u))));
    CHECK(pixels[0]==0xFF000000u && pixels[15]==0xFF000000u);
    CHECK(fake.readbacks==0);
    gml_render_resource_destroy(resource);
  }
  return 1;
}

static int dispatch_retained_replay(void){
  for(unsigned kind=0;kind<4u;kind++){
    uint32_t images[2][16];
    for(int enabled=0;enabled<2;enabled++){
      GmlRender *owner=calloc(1,sizeof *owner);
      CHECK(owner);
      gml_render_dispatch_diagnostics_enable(owner,enabled);
      uint32_t source_pixels[16];
      fill(images[enabled],0xA5123456u);
      fill(source_pixels,0xFF234567u);
      FakeDevice fake={0};
      GmlRenderDeviceServices device=service(&fake);
      GmlRenderResource *destination=gml_render_resource_create(target_ref(1u,1u),4u,4u,
        GML_RENDER_PIXEL_ARGB8888,images[enabled],(GmlRenderResourceLimits){0});
      CHECK(destination);
      gml_render_resource_bind_dispatch(destination,owner);
      GmlRenderResource *source_resource=NULL;
      GmlRenderResourceSource source=source_view(source_pixels,1);
      GmlRenderImageRef source_image=source.image;
      if(kind>=2u){
        GmlRenderImageRef identity={GML_RENDER_RESOURCE_SURFACE,2u,9u,1u};
        source_resource=gml_render_resource_create(identity,4u,4u,GML_RENDER_PIXEL_ARGB8888,
          source_pixels,(GmlRenderResourceLimits){0});
        CHECK(source_resource);
        gml_render_resource_bind_dispatch(source_resource,owner);
        GmlRenderDrawOp clear=clear_op(2u,0xFF234567u,GML_RENDER_REPLAY_EXACT);
        clear.target.image=identity; clear.target.image.content_version=2u;
        clear.target.pixel_format=GML_RENDER_PIXEL_ARGB8888;
        GmlRenderDrawBatch source_batch={&clear,1u,NULL,0u};
        GmlRenderResourceExecution result=gml_render_resource_submit(source_resource,&device,
          &source_batch,NULL,0u);
        gml_render_dispatch_batch_record(owner,&source_batch,result);
        CHECK(result==GML_RENDER_RESOURCE_EXEC_DEVICE &&
          gml_render_resource_device_source_view(source_resource,&device,&source) && !source.bytes);
        source_image=source.image;
      }
      GmlRenderDrawOp operations[2]={clear_op(2u,0xFF102030u,GML_RENDER_REPLAY_EXACT),
                                     quad_op(3u,&source_image)};
      GmlRenderDrawStateSnapshot state=quad_state(0);
      for(unsigned index=0;index<2u;index++) operations[index].target.pixel_format=GML_RENDER_PIXEL_ARGB8888;
      operations[1].scene_family=GML_RENDER_SCENE_SPRITE;
      if(kind>=2u){
        operations[1].payload.quad.sampling.source_region=(GmlRenderRect){0,0,4u,4u};
        operations[1].payload.quad.x[0]=operations[1].payload.quad.x[3]=0.0;
        operations[1].payload.quad.x[1]=operations[1].payload.quad.x[2]=4.0;
        operations[1].payload.quad.y[0]=operations[1].payload.quad.y[1]=0.0;
        operations[1].payload.quad.y[2]=operations[1].payload.quad.y[3]=4.0;
        state.samplers[0].source_region=(GmlRenderRect){0,0,4u,4u};
      }
      GmlRenderDrawBatch batch={operations,2u,&state,1u};
      GmlRenderResourceExecution result=gml_render_resource_submit(destination,&device,&batch,&source,1u);
      gml_render_dispatch_batch_record(owner,&batch,result);
      CHECK(result==GML_RENDER_RESOURCE_EXEC_DEVICE);
      GmlRenderDispatchMetrics metrics={0};
      CHECK(gml_render_dispatch_metrics(owner,&metrics) &&
        !metrics.cpu[GML_RENDER_SCENE_PRIMITIVE][GML_RENDER_DISPATCH_REPLAY] &&
        !metrics.cpu[GML_RENDER_SCENE_SPRITE][GML_RENDER_DISPATCH_REPLAY]);
      if(kind==1u || kind==3u){
        GmlRenderDrawOp suffix=flat_rect_op(4u,(GmlRenderRect){1,1,2u,2u},0xFFABCDEFu);
        suffix.target.pixel_format=GML_RENDER_PIXEL_ARGB8888;
        suffix.scene_family=GML_RENDER_SCENE_PRIMITIVE;
        GmlRenderDrawBatch fallback={&suffix,1u,NULL,0u};
        fake.status=GML_RENDER_EXEC_UNSUPPORTED; fake.prewrite_refusal=1;
        result=gml_render_resource_submit(destination,&device,&fallback,NULL,0u);
        gml_render_dispatch_batch_record(owner,&fallback,result);
        CHECK(result==GML_RENDER_RESOURCE_EXEC_CPU);
      }
      /* Frozen source recipes survive their original resource, without borrowing its owner. */
      gml_render_resource_destroy(source_resource);
      fake.forbid_calls=1;
      CHECK(gml_render_resource_acquire_cpu_read(destination,NULL));
      unsigned primitive_replays=1u+(kind==1u || kind==3u)+(kind>=2u);
      CHECK(gml_render_dispatch_metrics(owner,&metrics) &&
        metrics.cpu[GML_RENDER_SCENE_PRIMITIVE][GML_RENDER_DISPATCH_REPLAY]==
          (enabled?primitive_replays:0u) &&
        metrics.cpu[GML_RENDER_SCENE_SPRITE][GML_RENDER_DISPATCH_REPLAY]==(unsigned)enabled &&
        !metrics.failed && !metrics.unclassified && !metrics.complete &&
        !fake.readbacks && !fake.forbidden_calls);
      GmlRenderDispatchMetrics before=metrics;
      CHECK(gml_render_resource_acquire_cpu_read(destination,NULL) &&
        gml_render_dispatch_metrics(owner,&metrics) && !memcmp(&before,&metrics,sizeof metrics));
      gml_render_resource_destroy(destination);
      free(owner);
    }
    CHECK(!memcmp(images[0],images[1],sizeof images[0]));
  }
  return 1;
}

static int dispatch_replay_owner_move(void){
  uint32_t images[2][3][16];
  for(int enabled=0;enabled<2;enabled++){
    GmlRender *old=calloc(1,sizeof *old),*moved=calloc(1,sizeof *moved);
    CHECK(old && moved);
    gml_render_dispatch_diagnostics_enable(old,enabled);
    FakeDevice fake={0};
    GmlRenderDeviceServices device=service(&fake);
    GmlRenderResource *resources[3]={0};
    for(unsigned index=0;index<3u;index++){
      GmlRenderImageRef identity={index==0?GML_RENDER_RESOURCE_APPLICATION:
        index==1?GML_RENDER_RESOURCE_SCREEN:GML_RENDER_RESOURCE_SURFACE,index==2?1u:0u,7u,1u};
      fill(images[enabled][index],0xFF556677u);
      resources[index]=gml_render_resource_create(identity,4u,4u,GML_RENDER_PIXEL_ARGB8888,
        images[enabled][index],(GmlRenderResourceLimits){0});
      CHECK(resources[index]);
      gml_render_resource_bind_dispatch(resources[index],old);
      GmlRenderDrawOp clear=clear_op(2u,0xFF123456u+index,GML_RENDER_REPLAY_EXACT);
      clear.target.image=identity; clear.target.image.content_version=2u;
      clear.target.pixel_format=GML_RENDER_PIXEL_ARGB8888;
      GmlRenderDrawBatch batch={&clear,1u,NULL,0u};
      CHECK(gml_render_resource_submit(resources[index],&device,&batch,NULL,0u)==
            GML_RENDER_RESOURCE_EXEC_DEVICE);
    }
    old->app_surface_resource=resources[0];
    old->base_scene_resource=resources[1];
    old->surface[0].resource=resources[2];
    *moved=*old;
    GmlWin content={0};
    gml_render_rebind_content(moved,&content);
    gml_render_dispatch_diagnostics_enable(old,0);
    fake.forbid_calls=1;
    for(unsigned index=0;index<3u;index++) CHECK(gml_render_resource_acquire_cpu_read(resources[index],NULL));
    GmlRenderDispatchMetrics metrics={0};
    CHECK(gml_render_dispatch_metrics(moved,&metrics) &&
      metrics.cpu[GML_RENDER_SCENE_PRIMITIVE][GML_RENDER_DISPATCH_REPLAY]==(enabled?3u:0u) &&
      !metrics.failed && !metrics.unclassified && !fake.forbidden_calls && !fake.readbacks);
    CHECK(gml_render_dispatch_metrics(old,&metrics) &&
      !metrics.cpu[GML_RENDER_SCENE_PRIMITIVE][GML_RENDER_DISPATCH_REPLAY]);
    for(unsigned index=0;index<3u;index++) gml_render_resource_destroy(resources[index]);
    free(old); free(moved);
  }
  CHECK(!memcmp(images[0],images[1],sizeof images[0]));
  return 1;
}

static int journal_loss_and_mutation(void){
  uint32_t pixels[16],source_pixels[4]={0x80FF0000u,0x40FF0000u,
                                       0xFFFF0000u,0x00FF0000u};
  const uint32_t *observed=NULL;
  GmlRenderImageRef ref=source_ref();
  GmlRenderDrawOp operation=quad_op(2u,&ref);
  GmlRenderDrawStateSnapshot state=quad_state(1);
  GmlRenderDrawBatch batch={&operation,1u,&state,1u};
  GmlRenderResourceSource source=source_view(source_pixels,0);
  GmlRenderJournalMetrics metrics;
  FakeDevice fake={0};
  GmlRenderDeviceServices device=service(&fake);
  GmlRenderResource *resource;
  fill(pixels,0xFF0000FFu);
  resource=gml_render_resource_create(target_ref(1u,1u),4u,4u,
    GML_RENDER_PIXEL_XRGB8888,pixels,(GmlRenderResourceLimits){0});
  CHECK(resource);
  CHECK(gml_render_resource_submit(resource,&device,&batch,&source,1u)==
        GML_RENDER_RESOURCE_EXEC_DEVICE);
  CHECK(pixels[5]==0xFF0000FFu);
  gml_render_resource_journal_metrics(resource,&metrics);
  CHECK(metrics.authority==GML_RENDER_RESOURCE_GPU_REPLAYABLE);
  CHECK(metrics.logical_version==2u && metrics.cpu_version==1u && metrics.gpu_version==2u);
  CHECK(metrics.journal_operations==1u && metrics.retained_versions==1u);
  CHECK(metrics.retained_mutable_bytes==16u);
  source_pixels[0]=0xFFFFFFFFu; /* A mutable source may change after submission. */
  gml_render_resource_context_lost(resource);
  CHECK(gml_render_resource_acquire_cpu_read(resource,&observed));
  CHECK(observed==pixels);
  CHECK(pixels[5]==0xFF7F007Fu && pixels[6]==0xFF3F00BFu);
  CHECK(pixels[9]==0xFFFF0000u && pixels[10]==0xFF0000FFu);
  CHECK(fake.readbacks==0 && fake.submits==1);
  gml_render_resource_journal_metrics(resource,&metrics);
  CHECK(metrics.cpu_version==2u && metrics.gpu_version==0u && metrics.cpu_replays==1u);
  CHECK(metrics.journal_operations==0u && metrics.retained_mutable_bytes==0u);
  gml_render_resource_destroy(resource);
  return 1;
}

static int argb_surface_replay(void){
  uint32_t pixels[16],source_pixels[4]={0x40010203u,0x00040506u,
                                       0xFF070809u,0x80101112u};
  GmlRenderImageRef source_image=source_ref();
  GmlRenderDrawOp operations[2]={clear_op(2u,0x00556677u,GML_RENDER_REPLAY_EXACT),
                                 quad_op(3u,&source_image)};
  GmlRenderDrawStateSnapshot state=quad_state(0);
  GmlRenderDrawBatch batch={operations,2u,&state,1u};
  GmlRenderResourceSource source=source_view(source_pixels,1);
  FakeDevice fake={0};
  GmlRenderDeviceServices device=service(&fake);
  GmlRenderResource *resource;
  operations[0].target.pixel_format=GML_RENDER_PIXEL_ARGB8888;
  operations[1].target.pixel_format=GML_RENDER_PIXEL_ARGB8888;
  fill(pixels,0x00000000u);
  resource=gml_render_resource_create(target_ref(1u,1u),4u,4u,
    GML_RENDER_PIXEL_ARGB8888,pixels,(GmlRenderResourceLimits){0});
  CHECK(resource);
  CHECK(gml_render_resource_submit(resource,&device,&batch,&source,1u)==
        GML_RENDER_RESOURCE_EXEC_DEVICE);
  CHECK(fake.readbacks==0 && pixels[5]==0x00000000u);
  gml_render_resource_context_lost(resource);
  CHECK(gml_render_resource_acquire_cpu_read(resource,NULL));
  CHECK(pixels[0]==0x00556677u && pixels[15]==0x00556677u);
  CHECK(pixels[5]==source_pixels[0] && pixels[6]==source_pixels[1]);
  CHECK(pixels[9]==source_pixels[2] && pixels[10]==source_pixels[3]);
  CHECK(fake.readbacks==0);
  gml_render_resource_destroy(resource);
  return 1;
}

static int additive_background_exact_replay(void){
  uint32_t pixels[16],source_pixels[4]={0x80DC280Au,0x00B40FC8u,
                                       0xFF5A1EE6u,0x400A0A0Au};
  GmlRenderImageRef source_image=source_ref();
  GmlRenderDrawOp operations[2]={quad_op(2u,&source_image),
                                 quad_op(3u,&source_image)};
  GmlRenderDrawStateSnapshot state=quad_state(0);
  GmlRenderDrawBatch batch={operations,2u,&state,1u};
  GmlRenderResourceSource source=source_view(source_pixels,1);
  FakeDevice fake={0};
  GmlRenderDeviceServices device=service(&fake);
  GmlRenderResource *resource;
  for(unsigned index=0;index<16u;index++) pixels[index]=0x40563412u;
  state.rgb_source_factor=GML_RENDER_BLEND_SOURCE_ALPHA;
  state.rgb_destination_factor=GML_RENDER_BLEND_ONE;
  state.alpha_source_factor=GML_RENDER_BLEND_ONE;
  state.alpha_destination_factor=GML_RENDER_BLEND_ZERO;
  state.rgb_equation=state.alpha_equation=GML_RENDER_BLEND_EXACT8_ADDITIVE;
  state.additive_draw_alpha=0.65;
  for(unsigned index=0;index<2u;index++){
    operations[index].scene_family=GML_RENDER_SCENE_BACKGROUND;
    operations[index].target.pixel_format=GML_RENDER_PIXEL_ARGB8888;
  }
  resource=gml_render_resource_create(target_ref(1u,1u),4u,4u,
    GML_RENDER_PIXEL_ARGB8888,pixels,(GmlRenderResourceLimits){0});
  CHECK(resource);
  CHECK(gml_render_resource_submit(resource,&device,&batch,&source,1u)==
        GML_RENDER_RESOURCE_EXEC_DEVICE);
  CHECK(fake.submits==1 && fake.readbacks==0 && pixels[5]==0x40563412u);
  gml_render_resource_context_lost(resource);
  CHECK(gml_render_resource_acquire_cpu_read(resource,NULL));
  CHECK(pixels[0]==0x40563412u && pixels[5]==0xFFE44E18u &&
        pixels[6]==0x40563412u && pixels[9]==0xFFCA5AFFu &&
        pixels[10]==0xFF583614u && fake.readbacks==0);
  gml_render_resource_destroy(resource);
  /* The clipped destination starts at the second mirrored quad column, whose source is
   * the first texel. Context-loss replay must keep the unclipped source coordinate. */
  source_pixels[0]=0xFFFF0000u;
  source_pixels[1]=source_pixels[2]=source_pixels[3]=0u;
  for(unsigned index=0;index<16u;index++) pixels[index]=0x40112233u;
  operations[0].payload.quad.x[0]=operations[0].payload.quad.x[3]=-1.0;
  operations[0].payload.quad.x[1]=operations[0].payload.quad.x[2]=1.0;
  operations[0].payload.quad.u[0]=operations[0].payload.quad.u[3]=1.0;
  operations[0].payload.quad.u[1]=operations[0].payload.quad.u[2]=0.0;
  state.additive_draw_alpha=1.0;
  batch.operation_count=1u;
  resource=gml_render_resource_create(target_ref(1u,1u),4u,4u,
    GML_RENDER_PIXEL_ARGB8888,pixels,(GmlRenderResourceLimits){0});
  CHECK(resource);
  CHECK(gml_render_resource_submit(resource,&device,&batch,&source,1u)==
        GML_RENDER_RESOURCE_EXEC_DEVICE);
  gml_render_resource_context_lost(resource);
  CHECK(gml_render_resource_acquire_cpu_read(resource,NULL));
  CHECK(pixels[4]==0xFFFF2233u && pixels[5]==0x40112233u &&
        pixels[8]==0x40112233u && fake.readbacks==0);
  gml_render_resource_destroy(resource);
  return 1;
}

static int stretched_float_exact_replay(void){
  CHECK(gml_render_stretched_float_phase_exact(512u,960u));
  CHECK(gml_render_stretched_float_phase_exact(256u,544u));
  CHECK(gml_render_stretched_float_phase_exact(2u,4u));
  CHECK(!gml_render_stretched_float_phase_exact(256u,4095u));
  CHECK(!gml_render_stretched_float_phase_exact(0u,4u));
  CHECK(!gml_render_stretched_float_phase_exact(4u,4u));
  CHECK(!gml_render_stretched_float_phase_exact(2u,16385u));
  CHECK(gml_render_stretched_float_visible_phase_exact(32u,240958u,23998u,12u));
  CHECK(!gml_render_stretched_float_visible_phase_exact(32u,1000001u,23998u,12u));
  CHECK(!gml_render_stretched_float_visible_phase_exact(16384u,1000000u,990000u,10u));
  CHECK(!gml_render_stretched_float_visible_phase_exact(32u,240958u,23998u,0u));
  CHECK(!gml_render_stretched_float_visible_phase_exact(32u,240958u,240957u,2u));
  static const uint8_t authored[16]={255,0,0,255,0,255,0,255,
                                    0,0,255,255,255,255,255,255};
  static const uint32_t expected[16]={
    0xFFFF0000u,0xFFBF4000u,0xFF40BF00u,0xFF00FF00u,
    0xFFBF0040u,0xFF9F4040u,0xFF60BF40u,0xFF40FF40u,
    0xFF4000BFu,0xFF6040BFu,0xFF9FBFBFu,0xFFBFFFBFu,
    0xFF0000FFu,0xFF4040FFu,0xFFBFBFFFu,0xFFFFFFFFu};
  for(unsigned mode=0;mode<3u;mode++){
    uint8_t source_pixels[16];
    uint32_t pixels[16];
    GmlRenderImageRef source_image=source_ref();
    GmlRenderDrawOp operations[2]={clear_op(2u,0xFF112233u,GML_RENDER_REPLAY_EXACT),
                                   quad_op(3u,&source_image)};
    GmlRenderDrawStateSnapshot state=quad_state(0);
    GmlRenderDrawBatch batch={operations,2u,&state,1u};
    GmlRenderResourceSource source=source_view(NULL,0);
    GmlRenderJournalMetrics metrics={0};
    FakeDevice fake={0};
    GmlRenderDeviceServices device=service(&fake);
    memcpy(source_pixels,authored,sizeof source_pixels);
    fill(pixels,0xFF556677u);
    source.pixel_format=GML_RENDER_PIXEL_RGBA8888;
    source.bytes=source_pixels;
    state.samplers[0].filter=GML_RENDER_FILTER_BILINEAR;
    state.samplers[0].quantisation=GML_RENDER_QUANTISE_STRETCHED_FLOAT;
    for(unsigned index=0;index<2u;index++)
      operations[index].target.pixel_format=GML_RENDER_PIXEL_ARGB8888;
    GmlRenderQuad *quad=&operations[1].payload.quad;
    operations[1].scene_family=GML_RENDER_SCENE_SPRITE;
    quad->sampling=state.samplers[0];
    quad->x[0]=quad->x[3]=quad->y[0]=quad->y[1]=0.0;
    quad->x[1]=quad->x[2]=quad->y[2]=quad->y[3]=4.0;
    quad->span_x[0]=quad->span_x[3]=quad->span_y[0]=quad->span_y[3]=2.0;
    quad->span_x[1]=quad->span_y[1]=4.0;
    quad->span_x[2]=quad->span_y[2]=0.0;
    GmlRenderResource *resource=gml_render_resource_create(target_ref(1u,1u),4u,4u,
      GML_RENDER_PIXEL_ARGB8888,pixels,(GmlRenderResourceLimits){0});
    CHECK(resource);
    if(mode==0u) for(unsigned variant=0;variant<8u;variant++){
      GmlRenderDrawOp invalid[2];
      GmlRenderDrawStateSnapshot invalid_state=state;
      GmlRenderResourceSource invalid_source=source;
      memcpy(invalid,operations,sizeof invalid);
      if(variant==0u) invalid[1].payload.quad.sampling.filter=GML_RENDER_FILTER_NEAREST;
      if(variant==1u) source_pixels[3]=128u;
      if(variant==2u) invalid[1].payload.quad.u[0]=1.0;
      if(variant==3u) invalid[1].payload.quad.span_active=1u;
      if(variant==4u) invalid_state.samplers[0].quantisation=0u;
      if(variant==5u) invalid[1].payload.quad.x[0]=0.5;
      if(variant==6u) invalid_source.pixel_format=GML_RENDER_PIXEL_ARGB8888;
      if(variant==7u) invalid[1].payload.quad.span_x[0]=16385.0;
      GmlRenderDrawBatch refused={invalid,2u,&invalid_state,1u};
      CHECK(gml_render_resource_submit(resource,&device,&refused,&invalid_source,1u)==
            GML_RENDER_RESOURCE_EXEC_FAILED);
      CHECK(fake.uploads==0 && fake.submits==0 && fake.readbacks==0 &&
            pixels[0]==0xFF556677u && pixels[15]==0xFF556677u);
      source_pixels[3]=255u;
    }
    if(mode==1u) fake.status=GML_RENDER_EXEC_RECOVERABLE_ERROR;
    if(mode==2u) fake.fail_upload=1;
    CHECK(gml_render_resource_submit(resource,&device,&batch,&source,1u)==
          (mode?GML_RENDER_RESOURCE_EXEC_CPU:GML_RENDER_RESOURCE_EXEC_DEVICE));
    CHECK(fake.readbacks==0);
    if(!mode){
      CHECK(pixels[0]==0xFF556677u);
      gml_render_resource_journal_metrics(resource,&metrics);
      CHECK(metrics.authority==GML_RENDER_RESOURCE_GPU_REPLAYABLE &&
            metrics.journal_operations==2u && metrics.retained_mutable_bytes==16u);
      memset(source_pixels,0,sizeof source_pixels);
      gml_render_resource_context_lost(resource);
    }
    fake.forbid_calls=1;
    CHECK(gml_render_resource_acquire_cpu_read(resource,NULL));
    CHECK(!memcmp(pixels,expected,sizeof expected) &&
          fake.readbacks==0 && fake.forbidden_calls==0);
    gml_render_resource_destroy(resource);
  }
  return 1;
}

static int filtered_font_exact_recovery(void){
  static const uint8_t authored[16]={255,0,0,0,0,255,0,64,
                                    0,0,255,128,255,255,255,255};
  static const uint8_t replacement[16]={0};
  for(unsigned modern=0;modern<2u;modern++) for(unsigned preserve=0;preserve<2u;preserve++)
    for(unsigned failure=0;failure<3u;failure++){
      uint32_t pixels[16];
      GmlRenderImageRef source_image=source_ref();
      source_image.resource_class=GML_RENDER_RESOURCE_ATLAS;
      GmlRenderDrawOp operations[2]={clear_op(2u,0xFF123456u,GML_RENDER_REPLAY_EXACT),
                                     quad_op(3u,&source_image)};
      GmlRenderQuad *quad=&operations[1].payload.quad;
      GmlRenderDrawStateSnapshot state=quad_state(0);
      GmlRenderResourceSource source=source_view(NULL,1);
      FakeDevice fake={0};
      GmlRenderDeviceServices device=service(&fake);
      GmlRenderJournalMetrics before={0},after={0};
      source.image=source_image;
      source.pixel_format=GML_RENDER_PIXEL_RGBA8888;
      source.bytes=authored;
      state.rgb_source_factor=GML_RENDER_BLEND_SOURCE_ALPHA;
      state.rgb_destination_factor=GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA;
      state.alpha_source_factor=preserve?GML_RENDER_BLEND_SOURCE_ALPHA:GML_RENDER_BLEND_ONE;
      state.alpha_destination_factor=GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA;
      state.rgb_equation=state.alpha_equation=GML_RENDER_BLEND_EXACT_FILTERED_FONT;
      state.samplers[0].filter=GML_RENDER_FILTER_BILINEAR;
      state.samplers[0].quantisation=modern?GML_RENDER_QUANTISE_FONT_MODERN:
                                            GML_RENDER_QUANTISE_FONT_FIRSTGEN;
      quad->sampling=state.samplers[0];
      operations[1].scene_family=GML_RENDER_SCENE_TEXT;
      for(unsigned index=0;index<2u;index++)
        operations[index].target.pixel_format=GML_RENDER_PIXEL_ARGB8888;
      quad->x[1]=quad->x[2]=quad->y[2]=quad->y[3]=2.0;
      quad->inverse_anchor[0]=quad->inverse_anchor[1]=1.0;
      quad->inverse_x[0]=quad->inverse_y[1]=0.5;
      quad->filtered_draw_alpha=0.7;
      for(unsigned corner=0;corner<4u;corner++) quad->corner_colour[corner]=0xFF40C080u;
      fill(pixels,0xFF556677u);
      GmlRenderResource *resource=gml_render_resource_create(target_ref(1u,1u),4u,4u,
        GML_RENDER_PIXEL_ARGB8888,pixels,(GmlRenderResourceLimits){0});
      CHECK(resource);
      GmlRenderDrawBatch batch={operations,2u,&state,1u};
      if(failure==0u) for(unsigned variant=0;variant<14u;variant++){
        GmlRenderDrawOp invalid[2];
        GmlRenderDrawStateSnapshot invalid_state=state;
        GmlRenderResourceSource invalid_source=source;
        memcpy(invalid,operations,sizeof invalid);
        if(variant==0u) invalid[1].payload.quad.sampling.filter=GML_RENDER_FILTER_NEAREST;
        if(variant==1u) invalid[1].payload.quad.sampling.quantisation=0u;
        if(variant==2u) invalid_source.borrowed_immutable=0;
        if(variant==3u) invalid_source.pixel_format=GML_RENDER_PIXEL_ARGB8888;
        if(variant==4u){
          invalid_source.image.resource_class=GML_RENDER_RESOURCE_SPRITE;
          invalid[1].sources=&invalid_source.image;
        }
        if(variant==5u) invalid[1].payload.quad.inverse_x[0]=0.0;
        if(variant==6u) invalid[1].payload.quad.inverse_y[1]=-0.5;
        if(variant==7u) invalid[1].payload.quad.inverse_anchor[0]=NAN;
        if(variant==8u) invalid[1].payload.quad.filtered_draw_alpha=1.01;
        if(variant==9u) invalid[1].payload.quad.corner_colour[1]=0xFF112233u;
        if(variant==10u) invalid[1].payload.quad.span_active=1u;
        if(variant==11u) invalid[1].payload.quad.sampling.logical_margin_mask=1u;
        if(variant==12u) invalid_state.rgb_source_factor=GML_RENDER_BLEND_ZERO;
        if(variant==13u) invalid_state.samplers[0].quantisation=0u;
        GmlRenderDrawBatch refused={invalid,2u,&invalid_state,1u};
        CHECK(gml_render_resource_submit(resource,&device,&refused,&invalid_source,1u)==
              GML_RENDER_RESOURCE_EXEC_FAILED);
        CHECK(fake.uploads==0 && fake.submits==0 && fake.readbacks==0 &&
              pixels[0]==0xFF556677u && pixels[15]==0xFF556677u);
      }
      if(failure==1u){ fake.status=GML_RENDER_EXEC_RECOVERABLE_ERROR;fake.accepted_prefix=1u; }
      if(failure==2u) fake.fail_upload=1;
      CHECK(gml_render_resource_submit(resource,&device,&batch,&source,1u)==
            (failure?GML_RENDER_RESOURCE_EXEC_CPU:GML_RENDER_RESOURCE_EXEC_DEVICE));
      CHECK(fake.readbacks==0);
      if(!failure){
        CHECK(pixels[5]==0xFF556677u);
        gml_render_resource_journal_metrics(resource,&before);
        CHECK(before.authority==GML_RENDER_RESOURCE_GPU_REPLAYABLE &&
              before.journal_operations==2u && before.retained_mutable_bytes==0u);
        /* Replacing the caller's source record must not replace its retained immutable version. */
        source.bytes=replacement;
        source.image.content_version++;
        gml_render_resource_context_lost(resource);
      }
      fake.forbid_calls=1;
      CHECK(gml_render_resource_acquire_cpu_read(resource,NULL));
      /* Equal four-tap phase gives first-generation rounded 128/112 channels;
       * modern keeps 127.5/111.75 through tint and the combined blend. */
      uint32_t expected=(preserve?0xC9000000u:0xFF000000u)|
        (modern?0x0016414Fu:0x00164250u);
      for(unsigned pixel=0;pixel<16u;pixel++)
        CHECK(pixels[pixel]==(pixel==5u?expected:0xFF123456u));
      gml_render_resource_journal_metrics(resource,&after);
      CHECK(fake.readbacks==0 && fake.forbidden_calls==0 &&
            after.authority==GML_RENDER_RESOURCE_CPU_CURRENT &&
            (!failure?after.cpu_replays>before.cpu_replays:1));
      gml_render_resource_destroy(resource);
    }
  return 1;
}

static int pressure_and_fallback(void){
  uint32_t pixels[16];
  const uint32_t *observed=NULL;
  GmlRenderDrawOp operations[2]={clear_op(2u,0xFF112233u,GML_RENDER_REPLAY_EXACT),
                                 clear_op(3u,0xFF445566u,GML_RENDER_REPLAY_EXACT)};
  GmlRenderDrawBatch batch={operations,1u,NULL,0u};
  GmlRenderJournalMetrics metrics;
  FakeDevice fake={0};
  GmlRenderDeviceServices device=service(&fake);
  GmlRenderResource *first,*limited;
  fill(pixels,0xFF000000u);
  first=gml_render_resource_create(target_ref(1u,1u),4u,4u,
    GML_RENDER_PIXEL_XRGB8888,pixels,(GmlRenderResourceLimits){0});
  CHECK(first);
  CHECK(gml_render_resource_submit(first,&device,&batch,NULL,0u)==
        GML_RENDER_RESOURCE_EXEC_DEVICE);
  gml_render_resource_journal_metrics(first,&metrics);
  CHECK(metrics.journal_bytes>0u);
  gml_render_resource_destroy(first);
  fill(pixels,0xFF000000u);
  limited=gml_render_resource_create(target_ref(1u,1u),4u,4u,
    GML_RENDER_PIXEL_XRGB8888,pixels,
    (GmlRenderResourceLimits){metrics.journal_bytes,1u,0u});
  CHECK(limited);
  CHECK(gml_render_resource_submit(limited,&device,&batch,NULL,0u)==
        GML_RENDER_RESOURCE_EXEC_DEVICE);
  batch.operations=&operations[1];
  CHECK(gml_render_resource_submit(limited,&device,&batch,NULL,0u)==
        GML_RENDER_RESOURCE_EXEC_DEVICE);
  gml_render_resource_journal_metrics(limited,&metrics);
  /* A complete clear replaces the prior journal without replaying hidden pixels. */
  CHECK(metrics.consolidations==0u && metrics.cpu_replays==0u);
  CHECK(metrics.journal_operations==1u);
  gml_render_resource_context_lost(limited);
  CHECK(gml_render_resource_acquire_cpu_read(limited,&observed));
  CHECK(pixels[0]==0xFF445566u && pixels[15]==0xFF445566u);
  gml_render_resource_destroy(limited);
  /* A non-replacing draw still consolidates when the same descriptor budget is reached. */
  {
    uint32_t source_pixels[4]={0xFFFF0000u,0xFFFF0000u,0xFFFF0000u,0xFFFF0000u};
    GmlRenderImageRef image=source_ref();
    GmlRenderDrawOp quad=quad_op(3u,&image);
    GmlRenderDrawStateSnapshot state=quad_state(0);
    GmlRenderDrawBatch quad_batch={&quad,1u,&state,1u};
    GmlRenderResourceSource source=source_view(source_pixels,1);
    fill(pixels,0xFF000000u);
    limited=gml_render_resource_create(target_ref(1u,1u),4u,4u,
      GML_RENDER_PIXEL_XRGB8888,pixels,
      (GmlRenderResourceLimits){metrics.journal_bytes,1u,0u});
    CHECK(limited);
    batch.operations=&operations[0];
    CHECK(gml_render_resource_submit(limited,&device,&batch,NULL,0u)==
          GML_RENDER_RESOURCE_EXEC_DEVICE);
    CHECK(gml_render_resource_submit(limited,&device,&quad_batch,&source,1u)==
          GML_RENDER_RESOURCE_EXEC_DEVICE);
    gml_render_resource_journal_metrics(limited,&metrics);
    CHECK(metrics.consolidations==1u && metrics.cpu_replays==1u &&
          metrics.journal_operations==1u);
    gml_render_resource_context_lost(limited);
    CHECK(gml_render_resource_acquire_cpu_read(limited,&observed) &&
          pixels[0]==0xFF112233u && pixels[5]==0xFFFF0000u);
    gml_render_resource_destroy(limited);
  }
  /* One descriptor cannot fit. The batch must execute once on CPU, before device work. */
  fill(pixels,0xFF000000u);
  limited=gml_render_resource_create(target_ref(1u,1u),4u,4u,
    GML_RENDER_PIXEL_XRGB8888,pixels,(GmlRenderResourceLimits){1u,1u,0u});
  CHECK(limited);
  batch.operations=&operations[0];
  {
    int submits=fake.submits;
    CHECK(gml_render_resource_submit(limited,&device,&batch,NULL,0u)==
          GML_RENDER_RESOURCE_EXEC_CPU);
    CHECK(fake.submits==submits && pixels[0]==0xFF112233u);
  }
  gml_render_resource_destroy(limited);
  {
    uint32_t source_pixels[4]={0xFFFF0000u,0xFFFF0000u,0xFFFF0000u,0xFFFF0000u};
    GmlRenderImageRef image=source_ref();
    GmlRenderDrawOp quad=quad_op(2u,&image);
    GmlRenderDrawStateSnapshot state=quad_state(0);
    GmlRenderDrawBatch quad_batch={&quad,1u,&state,1u};
    GmlRenderResourceSource source=source_view(source_pixels,0);
    int submits=fake.submits;
    fill(pixels,0xFF000000u);
    limited=gml_render_resource_create(target_ref(1u,1u),4u,4u,
    GML_RENDER_PIXEL_XRGB8888,pixels,(GmlRenderResourceLimits){1024u,8u,0u});
    CHECK(limited);
    CHECK(gml_render_resource_submit(limited,&device,&quad_batch,&source,1u)==
          GML_RENDER_RESOURCE_EXEC_CPU);
    CHECK(fake.submits==submits && pixels[5]==0xFFFF0000u);
    gml_render_resource_journal_metrics(limited,&metrics);
    CHECK(metrics.retained_mutable_bytes==0u && metrics.cpu_fallbacks==1u);
    gml_render_resource_destroy(limited);
  }
  return 1;
}

static int upload_refusal_recovers_cpu(void){
  for(int failed_upload=1;failed_upload<=2;failed_upload++){
    uint32_t pixels[16],source_pixels[4]={0xFFB42834u,0xFFB42834u,
                                          0xFFB42834u,0xFFB42834u};
    GmlRenderImageRef image=source_ref();
    GmlRenderDrawOp operation=quad_op(2u,&image);
    GmlRenderDrawStateSnapshot state=quad_state(0);
    GmlRenderDrawBatch batch={&operation,1u,&state,1u};
    GmlRenderResourceSource source=source_view(source_pixels,0);
    GmlRenderJournalMetrics metrics={0};
    FakeDevice fake={0};
    GmlRenderDeviceServices device=service(&fake);
    GmlRenderResource *resource;
    fill(pixels,0xFF000000u);
    resource=gml_render_resource_create(target_ref(1u,1u),4u,4u,
      GML_RENDER_PIXEL_XRGB8888,pixels,(GmlRenderResourceLimits){0});
    CHECK(resource);
    fake.fail_upload_at=failed_upload;
    CHECK(gml_render_resource_submit(resource,&device,&batch,&source,1u)==
          GML_RENDER_RESOURCE_EXEC_CPU);
    gml_render_resource_journal_metrics(resource,&metrics);
    CHECK(fake.uploads==failed_upload && fake.submits==0 && fake.readbacks==0 &&
          pixels[5]==0xFFB42834u && metrics.logical_version==2u &&
          metrics.cpu_version==2u && metrics.gpu_version==0u &&
          metrics.authority==GML_RENDER_RESOURCE_CPU_CURRENT &&
          metrics.journal_operations==0u && metrics.retained_mutable_bytes==0u &&
          metrics.cpu_fallbacks==1u);
    fake.fail_upload_at=0;
    image.content_version++;
    source.image=image;
    source_pixels[0]=0xFF3975B1u;
    operation.target.image.content_version++;
    CHECK(gml_render_resource_submit(resource,&device,&batch,&source,1u)==
          GML_RENDER_RESOURCE_EXEC_DEVICE);
    CHECK(fake.uploads==failed_upload+2 && fake.submits==1 && fake.readbacks==0);
    gml_render_resource_context_lost(resource);
    CHECK(gml_render_resource_acquire_cpu_read(resource,NULL) &&
          pixels[5]==0xFF3975B1u && pixels[6]==0xFFB42834u);
    gml_render_resource_journal_metrics(resource,&metrics);
    CHECK(metrics.logical_version==3u && metrics.cpu_version==3u &&
          metrics.gpu_version==0u && metrics.journal_operations==0u &&
          metrics.retained_mutable_bytes==0u && fake.readbacks==0);
    gml_render_resource_destroy(resource);
  }
  return 1;
}

static int journal_saturation_stays_bounded(void){
  uint32_t pixels[16],source_pixels[32][4];
  GmlRenderImageRef image=source_ref();
  GmlRenderDrawOp operation=quad_op(2u,&image);
  GmlRenderDrawStateSnapshot state=quad_state(0);
  GmlRenderDrawBatch batch={&operation,1u,&state,1u};
  GmlRenderJournalMetrics metrics={0};
  FakeDevice fake={0};
  GmlRenderDeviceServices device=service(&fake);
  GmlRenderResource *resource;
  size_t one_entry_bytes;
  fill(pixels,0xFF000000u);
  resource=gml_render_resource_create(target_ref(1u,1u),4u,4u,
    GML_RENDER_PIXEL_XRGB8888,pixels,(GmlRenderResourceLimits){0});
  CHECK(resource);
  {
    GmlRenderDrawOp clear=clear_op(2u,0xFF000000u,GML_RENDER_REPLAY_EXACT);
    GmlRenderDrawBatch clear_batch={&clear,1u,NULL,0u};
    CHECK(gml_render_resource_submit(resource,&device,&clear_batch,NULL,0u)==
          GML_RENDER_RESOURCE_EXEC_DEVICE);
    gml_render_resource_journal_metrics(resource,&metrics);
    one_entry_bytes=metrics.journal_bytes;
    CHECK(one_entry_bytes>0u);
  }
  gml_render_resource_destroy(resource);
  fill(pixels,0xFF000000u);
  resource=gml_render_resource_create(target_ref(1u,2u),4u,4u,
    GML_RENDER_PIXEL_XRGB8888,pixels,
    (GmlRenderResourceLimits){one_entry_bytes,16u,0u});
  CHECK(resource);
  operation.target.image=target_ref(2u,2u);
  for(uint32_t step=0;step<32u;step++){
    uint32_t colour=0xFF112200u+step;
    for(int pixel=0;pixel<4;pixel++) source_pixels[step][pixel]=colour;
    image.content_version=(uint64_t)step+1u;
    operation.target.image.content_version=(uint64_t)step+2u;
    GmlRenderResourceSource source=source_view(source_pixels[step],0);
    source.image=image;
    CHECK(gml_render_resource_submit(resource,&device,&batch,&source,1u)==
          GML_RENDER_RESOURCE_EXEC_DEVICE);
    gml_render_resource_journal_metrics(resource,&metrics);
    CHECK(metrics.journal_bytes<=one_entry_bytes &&
          metrics.retained_mutable_bytes<=16u &&
          metrics.journal_operations<=1u && metrics.retained_versions<=1u &&
          metrics.cpu_fallbacks==0u && metrics.logical_version==(uint64_t)step+2u);
  }
  CHECK(metrics.consolidations==31u && metrics.cpu_replays==31u &&
        fake.submits==33 && fake.readbacks==0);
  gml_render_resource_context_lost(resource);
  CHECK(gml_render_resource_acquire_cpu_read(resource,NULL) &&
        pixels[5]==0xFF11221Fu && pixels[0]==0xFF000000u &&
        pixels[15]==0xFF000000u);
  gml_render_resource_journal_metrics(resource,&metrics);
  CHECK(metrics.cpu_replays==32u && metrics.journal_operations==0u &&
        metrics.retained_mutable_bytes==0u && metrics.gpu_version==0u);
  gml_render_resource_destroy(resource);
  return 1;
}

static int recreate_device_before_reader(void){
  uint32_t pixels[16],source_pixels[4]={0xFF11AA33u,0xFF11AA33u,
                                       0xFF11AA33u,0xFF11AA33u};
  GmlRenderImageRef image=source_ref();
  GmlRenderDrawOp operation=quad_op(2u,&image);
  GmlRenderDrawStateSnapshot state=quad_state(0);
  GmlRenderDrawBatch batch={&operation,1u,&state,1u};
  GmlRenderResourceSource source=source_view(source_pixels,1);
  GmlRenderJournalMetrics metrics;
  FakeDevice fake={0};
  GmlRenderDeviceServices device=service(&fake);
  GmlRenderResource *resource;
  fill(pixels,0xFF000000u);
  resource=gml_render_resource_create(target_ref(1u,1u),4u,4u,
    GML_RENDER_PIXEL_XRGB8888,pixels,(GmlRenderResourceLimits){0});
  CHECK(resource);
  CHECK(gml_render_resource_submit(resource,&device,&batch,&source,1u)==
        GML_RENDER_RESOURCE_EXEC_DEVICE);
  CHECK(fake.uploads==2 && pixels[5]==0xFF000000u);
  gml_render_resource_context_lost(resource);
  operation.target.image.content_version=3u;
  CHECK(gml_render_resource_submit(resource,&device,&batch,&source,1u)==
        GML_RENDER_RESOURCE_EXEC_DEVICE);
  CHECK(fake.uploads==4 && fake.submits==2 && pixels[5]==0xFF11AA33u);
  gml_render_resource_journal_metrics(resource,&metrics);
  CHECK(metrics.cpu_replays==1u && metrics.journal_operations==1u);
  CHECK(gml_render_resource_acquire_cpu_read(resource,NULL));
  CHECK(pixels[5]==0xFF11AA33u && fake.readbacks==0);
  gml_render_resource_destroy(resource);
  return 1;
}

static int resident_target_copy(void){
  uint32_t source_pixels[16],destination_pixels[16],other_pixels[16],*write=NULL;
  GmlRenderImageRef source_identity={GML_RENDER_RESOURCE_SURFACE,2u,1u,1u};
  GmlRenderImageRef source_version;
  GmlRenderDrawOp clear=clear_op(2u,0x40010203u,GML_RENDER_REPLAY_EXACT);
  GmlRenderDrawOp copy;
  GmlRenderDrawStateSnapshot state=quad_state(0);
  GmlRenderDrawBatch source_batch={&clear,1u,NULL,0u};
  GmlRenderDrawBatch destination_batch;
  GmlRenderResourceSource view;
  FakeDevice fake={0},other_fake={0};
  GmlRenderDeviceServices device=service(&fake);
  GmlRenderDeviceServices other_device=service(&other_fake);
  GmlRenderResource *source,*destination,*other_destination;
  fill(source_pixels,0u);
  fill(destination_pixels,0u);
  fill(other_pixels,0u);
  source=gml_render_resource_create(source_identity,4u,4u,
    GML_RENDER_PIXEL_ARGB8888,source_pixels,(GmlRenderResourceLimits){0});
  destination=gml_render_resource_create(target_ref(1u,1u),4u,4u,
    GML_RENDER_PIXEL_ARGB8888,destination_pixels,(GmlRenderResourceLimits){0});
  other_destination=gml_render_resource_create(
    (GmlRenderImageRef){GML_RENDER_RESOURCE_SURFACE,3u,1u,1u},4u,4u,
    GML_RENDER_PIXEL_ARGB8888,other_pixels,(GmlRenderResourceLimits){0});
  CHECK(source && destination && other_destination);
  clear.target.image=source_identity;
  clear.target.image.content_version=2u;
  clear.target.pixel_format=GML_RENDER_PIXEL_ARGB8888;
  CHECK(gml_render_resource_submit(source,&device,&source_batch,NULL,0u)==
        GML_RENDER_RESOURCE_EXEC_DEVICE);
  CHECK(gml_render_resource_source_view(source,&view));
  CHECK(source_pixels[0]==0x40010203u && fake.uploads==0);
  source_version=view.image;
  copy=quad_op(2u,&source_version);
  copy.target.pixel_format=GML_RENDER_PIXEL_ARGB8888;
  copy.payload.quad.sampling.source_region=(GmlRenderRect){0,0,4u,4u};
  copy.payload.quad.x[0]=copy.payload.quad.x[3]=0.0;
  copy.payload.quad.x[1]=copy.payload.quad.x[2]=4.0;
  copy.payload.quad.y[0]=copy.payload.quad.y[1]=0.0;
  copy.payload.quad.y[2]=copy.payload.quad.y[3]=4.0;
  state.samplers[0].source_region=(GmlRenderRect){0,0,4u,4u};
  destination_batch=(GmlRenderDrawBatch){&copy,1u,&state,1u};
  CHECK(gml_render_resource_submit(destination,&device,&destination_batch,&view,1u)==
        GML_RENDER_RESOURCE_EXEC_DEVICE);
  CHECK(fake.uploads==1); /* Destination baseline only; source stays resident. */
  copy.target.image.identity=3u;
  CHECK(gml_render_resource_submit(other_destination,&other_device,&destination_batch,
                                   &view,1u)==GML_RENDER_RESOURCE_EXEC_DEVICE);
  CHECK(other_fake.uploads==2); /* A different device needs both source and baseline. */
  copy.target.image.identity=1u;
  CHECK(gml_render_resource_acquire_cpu_write(source,&write));
  write[0]=0xFF998877u;
  CHECK(gml_render_resource_commit_cpu_write(source));
  copy.target.image.content_version=3u;
  CHECK(gml_render_resource_submit(destination,&device,&destination_batch,&view,1u)==
        GML_RENDER_RESOURCE_EXEC_FAILED); /* Stale source version is fenced. */
  CHECK(gml_render_resource_source_view(source,&view));
  source_version=view.image;
  CHECK(source_version.content_version==3u && view.bytes==source_pixels);
  CHECK(gml_render_resource_submit(destination,&device,&destination_batch,&view,1u)==
        GML_RENDER_RESOURCE_EXEC_DEVICE);
  CHECK(fake.uploads==2); /* The new source version uploads on the same device. */
  gml_render_resource_context_lost(destination);
  CHECK(gml_render_resource_acquire_cpu_read(destination,NULL));
  CHECK(destination_pixels[0]==0xFF998877u && destination_pixels[15]==0x40010203u);
  CHECK(fake.readbacks==0 && fake.submits==3);
  CHECK(other_fake.submits==1);
  gml_render_resource_destroy(source);
  gml_render_resource_destroy(destination);
  gml_render_resource_destroy(other_destination);
  return 1;
}

static int resident_source_recipe_versions(void){
  for(unsigned variant=0;variant<6u;variant++){
    uint32_t source_pixels[16],destination_pixels[16];
    GmlRenderImageRef source_identity={GML_RENDER_RESOURCE_SURFACE,2u,9u,1u};
    GmlRenderDrawOp clear=clear_op(2u,0x40010203u,GML_RENDER_REPLAY_EXACT);
    GmlRenderDrawStateSnapshot state=quad_state(0);
    GmlRenderDrawBatch batch={&clear,1u,NULL,0u};
    GmlRenderResourceSource view={0};
    GmlRenderJournalMetrics before={0},after={0};
    FakeDevice fake={0};
    GmlRenderDeviceServices device=service(&fake);
    GmlRenderResourceLimits limits={0};
    if(variant==2u) limits.journal_bytes=sizeof(GmlRenderDrawOp)*2u;
    fill(source_pixels,0xA5123456u);
    fill(destination_pixels,0xA5123456u);
    GmlRenderResource *source=gml_render_resource_create(source_identity,4u,4u,
      GML_RENDER_PIXEL_ARGB8888,source_pixels,(GmlRenderResourceLimits){0});
    GmlRenderResource *destination=gml_render_resource_create(target_ref(1u,1u),4u,4u,
      GML_RENDER_PIXEL_ARGB8888,destination_pixels,limits);
    CHECK(source && destination);
    clear.target.image=source_identity; clear.target.image.content_version=2u;
    clear.target.pixel_format=GML_RENDER_PIXEL_ARGB8888;
    CHECK(gml_render_resource_submit(source,&device,&batch,NULL,0u)==
          GML_RENDER_RESOURCE_EXEC_DEVICE);
    CHECK(gml_render_resource_device_source_view(source,&device,&view) && !view.bytes);
    CHECK(source_pixels[0]==0xA5123456u);
    GmlRenderImageRef version=view.image;
    GmlRenderDrawOp copy=quad_op(2u,&version);
    copy.target.pixel_format=GML_RENDER_PIXEL_ARGB8888;
    copy.payload.quad.sampling.source_region=(GmlRenderRect){0,0,4u,4u};
    copy.payload.quad.x[0]=copy.payload.quad.x[3]=0.0;
    copy.payload.quad.x[1]=copy.payload.quad.x[2]=4.0;
    copy.payload.quad.y[0]=copy.payload.quad.y[1]=0.0;
    copy.payload.quad.y[2]=copy.payload.quad.y[3]=4.0;
    state.samplers[0].source_region=(GmlRenderRect){0,0,4u,4u};
    batch=(GmlRenderDrawBatch){&copy,1u,&state,1u};
    if(variant==3u){ fake.status=GML_RENDER_EXEC_RECOVERABLE_ERROR; fake.accepted_prefix=0; }
    if(variant==4u){ fake.status=GML_RENDER_EXEC_UNSUPPORTED; fake.prewrite_refusal=1; }
    if(variant==5u){ fake.status=GML_RENDER_EXEC_RECOVERABLE_ERROR; fake.accepted_prefix=1u; }
    CHECK(gml_render_resource_submit(destination,&device,&batch,&view,1u)==
          (variant>=2u?GML_RENDER_RESOURCE_EXEC_CPU:GML_RENDER_RESOURCE_EXEC_DEVICE));
    CHECK(source_pixels[0]==0xA5123456u && fake.readbacks==0);
    gml_render_resource_journal_metrics(destination,&before);
    if(variant<2u){
      CHECK(before.authority==GML_RENDER_RESOURCE_GPU_REPLAYABLE &&
            before.retained_mutable_bytes==0u && before.retained_versions==1u &&
            destination_pixels[0]==0xA5123456u);
    }
    if(variant==0u){
      clear.target.image.content_version=3u; clear.payload.clear_colour=0xFFAABBCCu;
      GmlRenderDrawBatch overwrite={&clear,1u,NULL,0u};
      CHECK(gml_render_resource_submit(source,&device,&overwrite,NULL,0u)==
            GML_RENDER_RESOURCE_EXEC_DEVICE);
      CHECK(gml_render_resource_submit(destination,&device,&batch,&view,1u)==
            GML_RENDER_RESOURCE_EXEC_FAILED);
    }
    gml_render_resource_destroy(source);
    fake.forbid_calls=1;
    gml_render_resource_context_lost(destination);
    CHECK(gml_render_resource_acquire_cpu_read(destination,NULL));
    for(unsigned index=0;index<16u;index++) CHECK(destination_pixels[index]==0x40010203u);
    gml_render_resource_journal_metrics(destination,&after);
    CHECK(after.cpu_version==after.logical_version && after.retained_versions==0u &&
          fake.readbacks==0 && fake.forbidden_calls==0);
    gml_render_resource_destroy(destination);
  }
  return 1;
}

static int resident_source_recipe_chain(void){
  enum { COUNT=10 };
  uint32_t pixels[COUNT][16];
  GmlRenderResource *resources[COUNT]={0};
  FakeDevice fake={0};
  GmlRenderDeviceServices device=service(&fake);
  GmlRenderDrawStateSnapshot state=quad_state(0);
  state.samplers[0].source_region=(GmlRenderRect){0,0,4u,4u};
  for(unsigned index=0;index<COUNT;index++){
    fill(pixels[index],0xA5123456u);
    GmlRenderImageRef identity={GML_RENDER_RESOURCE_SURFACE,index+1u,3u,1u};
    resources[index]=gml_render_resource_create(identity,4u,4u,GML_RENDER_PIXEL_ARGB8888,
                                               pixels[index],(GmlRenderResourceLimits){0});
    CHECK(resources[index]);
    GmlRenderDrawOp clear=clear_op(2u,index?0xFF010203u:0x4025C080u,GML_RENDER_REPLAY_EXACT);
    clear.target.image=identity; clear.target.image.content_version=2u;
    clear.target.pixel_format=GML_RENDER_PIXEL_ARGB8888;
    GmlRenderDrawBatch batch={&clear,1u,NULL,0u};
    CHECK(gml_render_resource_submit(resources[index],&device,&batch,NULL,0u)==
          GML_RENDER_RESOURCE_EXEC_DEVICE);
    if(index){
      GmlRenderResourceSource source={0};
      CHECK(gml_render_resource_device_source_view(resources[index-1u],&device,&source));
      GmlRenderImageRef version=source.image;
      GmlRenderDrawOp copy=quad_op(3u,&version);
      copy.target.image=identity; copy.target.image.content_version=3u;
      copy.target.pixel_format=GML_RENDER_PIXEL_ARGB8888;
      copy.payload.quad.sampling.source_region=(GmlRenderRect){0,0,4u,4u};
      copy.payload.quad.x[0]=copy.payload.quad.x[3]=0.0;
      copy.payload.quad.x[1]=copy.payload.quad.x[2]=4.0;
      copy.payload.quad.y[0]=copy.payload.quad.y[1]=0.0;
      copy.payload.quad.y[2]=copy.payload.quad.y[3]=4.0;
      batch=(GmlRenderDrawBatch){&copy,1u,&state,1u};
      CHECK(gml_render_resource_submit(resources[index],&device,&batch,&source,1u)==
            (index==COUNT-1u?GML_RENDER_RESOURCE_EXEC_CPU:GML_RENDER_RESOURCE_EXEC_DEVICE));
    }
    GmlRenderJournalMetrics metrics={0};
    gml_render_resource_journal_metrics(resources[index],&metrics);
    CHECK(metrics.journal_bytes<=8u*1024u*1024u && !metrics.retained_mutable_bytes);
    CHECK(metrics.cached_replay_bytes<=72u*1024u*1024u && metrics.retained_replay_depth<=8u);
    if(index<COUNT-2u) CHECK(pixels[index][0]==0xA5123456u);
  }
  for(unsigned index=0;index<COUNT-3u;index++){
    gml_render_resource_destroy(resources[index]); resources[index]=NULL;
  }
  fake.forbid_calls=1;
  for(unsigned index=COUNT-3u;index<COUNT;index++){
    gml_render_resource_context_lost(resources[index]);
    CHECK(gml_render_resource_acquire_cpu_read(resources[index],NULL));
    for(unsigned pixel=0;pixel<16u;pixel++) CHECK(pixels[index][pixel]==0x4025C080u);
    gml_render_resource_destroy(resources[index]);
  }
  CHECK(!fake.readbacks && !fake.forbidden_calls);
  return 1;
}

static int partial_error_and_write_lease(void){
  uint32_t pixels[16],*write=NULL;
  const uint32_t *read=NULL;
  GmlRenderDrawOp operations[2]={clear_op(2u,0xFF010203u,GML_RENDER_REPLAY_EXACT),
                                 clear_op(3u,0xFFABCDEFu,GML_RENDER_REPLAY_EXACT)};
  GmlRenderDrawBatch batch={operations,2u,NULL,0u};
  GmlRenderJournalMetrics metrics;
  FakeDevice fake={0};
  GmlRenderDeviceServices device=service(&fake);
  GmlRenderResource *resource;
  fake.status=GML_RENDER_EXEC_RECOVERABLE_ERROR;
  fake.accepted_prefix=1u;
  fill(pixels,0xFF000000u);
  resource=gml_render_resource_create(target_ref(1u,1u),4u,4u,
    GML_RENDER_PIXEL_XRGB8888,pixels,(GmlRenderResourceLimits){0});
  CHECK(resource);
  CHECK(gml_render_resource_submit(resource,&device,&batch,NULL,0u)==
        GML_RENDER_RESOURCE_EXEC_CPU);
  CHECK(fake.submits==1 && fake.retires==1 && pixels[0]==0xFFABCDEFu);
  gml_render_resource_journal_metrics(resource,&metrics);
  CHECK(metrics.logical_version==3u && metrics.cpu_version==3u && metrics.gpu_version==0u);
  CHECK(metrics.cpu_fallbacks==1u && metrics.journal_operations==0u);
  CHECK(gml_render_resource_acquire_cpu_write(resource,&write));
  CHECK(write==pixels);
  write[0]=0xFF7799BBu;
  CHECK(!gml_render_resource_submit(resource,&device,&batch,NULL,0u));
  CHECK(gml_render_resource_commit_cpu_write(resource));
  CHECK(!gml_render_resource_commit_cpu_write(resource));
  CHECK(gml_render_resource_acquire_cpu_read(resource,&read) && read[0]==0xFF7799BBu);
  gml_render_resource_journal_metrics(resource,&metrics);
  CHECK(metrics.logical_version==4u && metrics.cpu_version==4u && metrics.gpu_version==0u);
  gml_render_resource_destroy(resource);
  return 1;
}

static int checkpoint_and_failure(void){
  uint32_t pixels[16];
  GmlRenderDrawOp operation=clear_op(2u,0xFF010203u,GML_RENDER_DEVICE_CHECKPOINT);
  GmlRenderDrawBatch batch={&operation,1u,NULL,0u};
  GmlRenderJournalMetrics metrics;
  FakeDevice fake={0};
  GmlRenderDeviceServices device=service(&fake);
  GmlRenderResource *resource;
  fake.readback_colour=0xFF335577u;
  fill(pixels,0xFF000000u);
  resource=gml_render_resource_create(target_ref(1u,1u),4u,4u,
    GML_RENDER_PIXEL_XRGB8888,pixels,(GmlRenderResourceLimits){0});
  CHECK(resource);
  CHECK(gml_render_resource_submit(resource,&device,&batch,NULL,0u)==
        GML_RENDER_RESOURCE_EXEC_DEVICE);
  CHECK(fake.readbacks==1 && pixels[0]==fake.readback_colour);
  gml_render_resource_journal_metrics(resource,&metrics);
  CHECK(metrics.device_checkpoints==1u && metrics.authority==GML_RENDER_RESOURCE_BOTH_CURRENT);
  gml_render_resource_context_lost(resource);
  CHECK(gml_render_resource_acquire_cpu_read(resource,NULL));
  CHECK(pixels[15]==fake.readback_colour && fake.readbacks==1);
  gml_render_resource_destroy(resource);
  fill(pixels,0xFF000000u);
  resource=gml_render_resource_create(target_ref(1u,1u),4u,4u,
    GML_RENDER_PIXEL_XRGB8888,pixels,(GmlRenderResourceLimits){0});
  CHECK(resource);
  fake.fail_readback=1;
  CHECK(gml_render_resource_submit(resource,&device,&batch,NULL,0u)==
        GML_RENDER_RESOURCE_EXEC_FAILED);
  CHECK(pixels[0]==0xFF000000u && !gml_render_resource_acquire_cpu_read(resource,NULL));
  gml_render_resource_journal_metrics(resource,&metrics);
  CHECK(metrics.authority==GML_RENDER_RESOURCE_INVALID);
  gml_render_resource_destroy(resource);
  return 1;
}

/* The fake models authority/fault boundaries, not shader arithmetic. A device-only prefix
 * followed by exact primitive/atlas work must stay resident or fall back once from a sealed
 * prefix. Uncertain partial execution cannot manufacture the missing prefix on CPU. */
static int checkpoint_span_mixed_suffix_and_faults(void){
  for(int variant=0;variant<9;variant++){
    uint32_t pixels[16],source_pixels[4]={0xFF112233u,0xFF112233u,
                                        0xFF112233u,0xFF112233u};
    GmlRenderResourceSource source=source_view(source_pixels,1);
    GmlRenderDrawOp prefix=clear_op(2u,0u,GML_RENDER_DEVICE_CHECKPOINT);
    GmlRenderDrawOp suffix[2]={flat_rect_op(3u,(GmlRenderRect){0,0,1u,1u},
                                            0xFF7799BBu),quad_op(4u,&source.image)};
    GmlRenderDrawStateSnapshot state=quad_state(0);
    GmlRenderDrawBatch first={&prefix,1u,NULL,0u},next={suffix,2u,&state,1u};
    GmlRenderJournalMetrics metrics={0};
    GmlRenderResourceSource resident={0};
    GmlRenderResource *resource;
    GmlRenderResourceExecution result;
    const uint32_t *observed=NULL;
    uint32_t *writer=NULL;
    FakeDevice fake={0};
    GmlRenderDeviceServices device=service(&fake);
    fill(pixels,0xFF010203u);
    fake.readback_colour=0xFF5879A4u;
    resource=gml_render_resource_create(target_ref(1u,1u),4u,4u,
      GML_RENDER_PIXEL_XRGB8888,pixels,(GmlRenderResourceLimits){0});
    CHECK(resource && gml_render_resource_submit_deferred(resource,&device,&first,NULL,0u)==
          GML_RENDER_RESOURCE_EXEC_DEVICE && fake.readbacks==0 && fake.submits==1);
    CHECK(gml_render_resource_device_source_view(resource,&device,&resident) &&
          !resident.bytes && resident.image.content_version==2u);
    if(variant==1 || variant==4 || variant==8){
      fake.status=GML_RENDER_EXEC_UNSUPPORTED;
      fake.prewrite_refusal=1;
    }
    if(variant==2) fake.fail_upload_at=fake.uploads+1;
    if(variant==3){
      fake.status=GML_RENDER_EXEC_RECOVERABLE_ERROR;
      fake.accepted_prefix=1u;
    }
    if(variant==4) fake.fail_readback=1;
    if(variant==7) fake.accepted_prefix=1u; /* Incomplete even with ACCEPTED status. */
    if(variant==8) suffix[0].reconstruction_mode=GML_RENDER_DEVICE_CHECKPOINT;
    result=gml_render_resource_submit_deferred(resource,&device,&next,&source,1u);
    if(variant==1 || variant==2){
      CHECK(result==GML_RENDER_RESOURCE_EXEC_CPU && fake.readbacks==1 &&
            fake.submits==(variant==1?2:1) && pixels[0]==0xFF7799BBu &&
            pixels[5]==0xFF112233u && pixels[15]==fake.readback_colour);
      gml_render_resource_journal_metrics(resource,&metrics);
      CHECK(metrics.authority==GML_RENDER_RESOURCE_CPU_CURRENT &&
            metrics.cpu_fallbacks==1u && metrics.device_checkpoints==1u &&
            metrics.logical_version==4u && metrics.cpu_version==4u &&
            metrics.gpu_version==0u && metrics.journal_operations==0u);
    } else if(variant==3 || variant==4 || variant==7){
      CHECK(result==GML_RENDER_RESOURCE_EXEC_FAILED &&
            fake.readbacks==(variant==4?1:0) && pixels[0]==0xFF010203u &&
            !gml_render_resource_acquire_cpu_read(resource,&observed) && !observed &&
            !gml_render_resource_checkpoint(resource,&device));
      gml_render_resource_journal_metrics(resource,&metrics);
      CHECK(metrics.authority==GML_RENDER_RESOURCE_INVALID &&
            metrics.cpu_fallbacks==0u && metrics.cpu_version==1u &&
            metrics.logical_version==2u && metrics.device_checkpoints==0u);
    } else if(variant==8){
      CHECK(result==GML_RENDER_RESOURCE_EXEC_FAILED && fake.readbacks==1 &&
            gml_render_resource_acquire_cpu_read(resource,&observed) &&
            observed[0]==fake.readback_colour);
      gml_render_resource_journal_metrics(resource,&metrics);
      CHECK(metrics.authority==GML_RENDER_RESOURCE_BOTH_CURRENT &&
            metrics.logical_version==2u && metrics.cpu_version==2u &&
            metrics.gpu_version==2u && metrics.cpu_fallbacks==0u);
    } else {
      CHECK(result==GML_RENDER_RESOURCE_EXEC_DEVICE && fake.readbacks==0 &&
            fake.submits==2 && pixels[0]==0xFF010203u);
      gml_render_resource_journal_metrics(resource,&metrics);
      CHECK(metrics.authority==GML_RENDER_RESOURCE_GPU_PENDING_CHECKPOINT &&
            metrics.logical_version==4u && metrics.gpu_version==4u &&
            metrics.cpu_version==1u && metrics.journal_operations==0u);
      if(variant==5 || variant==6){
        if(variant==5) fake.fail_readback=1;
        else { gml_render_resource_context_lost(resource); fake.forbid_calls=1; }
        CHECK(!gml_render_resource_acquire_cpu_read(resource,&observed) && !observed &&
              !gml_render_resource_checkpoint(resource,&device) &&
              pixels[0]==0xFF010203u && fake.forbidden_calls==0);
      } else {
        CHECK(gml_render_resource_acquire_cpu_write(resource,&writer) && writer==pixels &&
              fake.readbacks==1 && writer[0]==fake.readback_colour);
        writer[0]=0xFFABCDEFu;
        CHECK(gml_render_resource_commit_cpu_write(resource));
        gml_render_resource_journal_metrics(resource,&metrics);
        CHECK(metrics.authority==GML_RENDER_RESOURCE_CPU_CURRENT &&
              metrics.logical_version==5u && metrics.cpu_version==5u &&
              metrics.gpu_version==4u && metrics.device_checkpoints==1u &&
              !gml_render_resource_device_source_view(resource,&device,&resident));
        gml_render_resource_context_lost(resource);
        fake.forbid_calls=1;
        CHECK(gml_render_resource_acquire_cpu_read(resource,&observed) &&
              observed[0]==0xFFABCDEFu && fake.readbacks==1 && fake.forbidden_calls==0);
      }
    }
    gml_render_resource_destroy(resource);
  }
  return 1;
}

/* An explicit staging version authorizes partial publication, never a pointer-identity guess. */
static int incremental_checkpoint_publication(void){
  for(unsigned target=0;target<2u;target++) for(unsigned fault=0;fault<5u;fault++){
    uint32_t pixels[16],before[16],*writer=NULL;
    GmlRenderImageRef image=target_ref(1u,1u);
    GmlRenderDrawOp operation=clear_op(2u,0u,GML_RENDER_DEVICE_CHECKPOINT);
    GmlRenderDrawBatch batch={&operation,1u,NULL,0u};
    FakeDevice fake={0},replacement={0};
    GmlRenderDeviceServices device=service(&fake),next_device=service(&replacement);
    GmlRenderResource *resource;
    image.resource_class=target?GML_RENDER_RESOURCE_APPLICATION:GML_RENDER_RESOURCE_SCREEN;
    fill(pixels,0xFF010203u);
    operation.target.image=image;
    resource=gml_render_resource_create(image,4u,4u,GML_RENDER_PIXEL_XRGB8888,pixels,
                                         (GmlRenderResourceLimits){0});
    CHECK(resource);
    fake.readback_colour=0xFF123456u;
    operation.target.image.content_version=2u;
    if(fault>=3u){
      memcpy(before,pixels,sizeof before);
      fake.incremental_readback=(int)fault+2;
      CHECK(gml_render_resource_submit(resource,&device,&batch,NULL,0u)==GML_RENDER_RESOURCE_EXEC_FAILED);
      CHECK(fake.last_checkpoint_baseline==0u && !memcmp(before,pixels,sizeof before) &&
            !gml_render_resource_acquire_cpu_read(resource,NULL));
      gml_render_resource_destroy(resource);
      continue;
    }
    CHECK(gml_render_resource_submit(resource,&device,&batch,NULL,0u)==GML_RENDER_RESOURCE_EXEC_DEVICE);
    CHECK(fake.last_checkpoint==1u && fake.last_checkpoint_baseline==0u);
    memcpy(before,pixels,sizeof before);
    fake.incremental_readback=1;
    fake.readback_colour=0xFFABCDEFu;
    operation.target.image.content_version=3u;
    CHECK(gml_render_resource_submit(resource,&device,&batch,NULL,0u)==GML_RENDER_RESOURCE_EXEC_DEVICE);
    CHECK(fake.last_checkpoint_baseline==2u);
    for(unsigned index=0;index<16u;index++)
      CHECK(pixels[index]==(index==9u || index==10u?fake.readback_colour:before[index]));
    memcpy(before,pixels,sizeof before);
    fake.incremental_readback=2;
    operation.target.image.content_version=4u;
    CHECK(gml_render_resource_submit(resource,&device,&batch,NULL,0u)==GML_RENDER_RESOURCE_EXEC_DEVICE);
    CHECK(fake.last_checkpoint_baseline==3u && !memcmp(before,pixels,sizeof before));
    CHECK(gml_render_resource_acquire_cpu_write(resource,&writer) && writer==pixels);
    fill(writer,0xFF998877u);
    CHECK(gml_render_resource_commit_cpu_write(resource));
    operation.target.image=gml_render_resource_image_ref(resource);
    operation.target.image.content_version++;
    CHECK(gml_render_resource_submit(resource,&device,&batch,NULL,0u)==GML_RENDER_RESOURCE_EXEC_DEVICE);
    CHECK(fake.last_checkpoint_baseline==0u && pixels[0]==fake.readback_colour);
    operation.target.image.content_version++;
    replacement.readback_colour=0xFF334455u;
    CHECK(gml_render_resource_submit(resource,&next_device,&batch,NULL,0u)==GML_RENDER_RESOURCE_EXEC_DEVICE);
    CHECK(replacement.last_checkpoint_baseline==0u && pixels[15]==replacement.readback_colour);
    memcpy(before,pixels,sizeof before);
    replacement.incremental_readback=fault?fault+2:1;
    replacement.readback_colour=0xFF556677u;
    operation.target.image.content_version++;
    CHECK(gml_render_resource_submit(resource,&next_device,&batch,NULL,0u)==
            (fault?GML_RENDER_RESOURCE_EXEC_FAILED:GML_RENDER_RESOURCE_EXEC_DEVICE));
    if(fault) CHECK(!memcmp(before,pixels,sizeof before) &&
                     !gml_render_resource_acquire_cpu_read(resource,NULL));
    else {
      gml_render_resource_context_lost(resource);
      replacement.forbid_calls=1;
      CHECK(gml_render_resource_acquire_cpu_read(resource,NULL) && pixels[9]==replacement.readback_colour &&
            pixels[0]==before[0] && !replacement.forbidden_calls);
    }
    gml_render_resource_destroy(resource);
  }
  return 1;
}

/* Staging reuse changes allocation lifetime, not device pixels, CPU visibility or failure
 * publication. Cover both retained owner classes and the deliberately transient surface path. */
static int bounded_checkpoint_staging_lifecycle(void){
  const uint32_t classes[]={GML_RENDER_RESOURCE_SCREEN,GML_RENDER_RESOURCE_APPLICATION,
                            GML_RENDER_RESOURCE_SURFACE};
  const uint32_t formats[]={GML_RENDER_PIXEL_XRGB8888,GML_RENDER_PIXEL_ARGB8888};
  for(unsigned owner=0;owner<3u;owner++) for(unsigned format=0;format<2u;format++)
  for(unsigned budget=0;budget<3u;budget++){
    uint32_t pixels[16],larger[64],*writer=NULL;
    const uint32_t *reader=NULL;
    GmlRenderImageRef image=target_ref(1u,1u);
    GmlRenderResourceLimits limits={0};
    GmlRenderJournalMetrics metrics={0};
    GmlRenderDrawOp operation=clear_op(2u,0u,GML_RENDER_DEVICE_CHECKPOINT);
    GmlRenderDrawBatch batch={&operation,1u,NULL,0u};
    FakeDevice fake={0},replacement={0};
    GmlRenderDeviceServices device=service(&fake),next_device=service(&replacement);
    GmlRenderResource *resource;
    int cached=owner<2u && budget!=2u;
    uint32_t last_colour;
    image.resource_class=classes[owner];
    limits.checkpoint_staging_bytes=budget==0u?0u:(budget==1u?64u:63u);
    fill(pixels,0xFF010203u);
    operation.target.image=image;
    operation.target.pixel_format=formats[format];
    resource=gml_render_resource_create(image,4u,4u,formats[format],pixels,limits);
    CHECK(resource);
    gml_render_resource_journal_metrics(resource,&metrics);
    CHECK(metrics.checkpoint_allocations==0u && metrics.checkpoint_staging_bytes==0u);
    for(unsigned pass=0;pass<2u;pass++){
      operation.target.image=gml_render_resource_image_ref(resource);
      operation.target.image.content_version++;
      fake.readback_colour=(format?0x7B000000u:0xFF000000u)|(0x123456u+pass);
      CHECK(gml_render_resource_submit(resource,&device,&batch,NULL,0u)==
            GML_RENDER_RESOURCE_EXEC_DEVICE);
      CHECK(gml_render_resource_acquire_cpu_read(resource,&reader) && reader==pixels);
      for(unsigned index=0;index<16u;index++) CHECK(reader[index]==fake.readback_colour);
    }
    gml_render_resource_journal_metrics(resource,&metrics);
    CHECK(metrics.device_checkpoints==2u && fake.readbacks==2 &&
          metrics.authority==GML_RENDER_RESOURCE_BOTH_CURRENT &&
          metrics.checkpoint_allocations==(cached?1u:2u) &&
          metrics.checkpoint_staging_bytes==(cached?64u:0u));
    CHECK(gml_render_resource_acquire_cpu_write(resource,&writer) && writer==pixels);
    fill(writer,0xFF345678u);
    CHECK(gml_render_resource_commit_cpu_write(resource));
    gml_render_resource_context_lost(resource);
    fake.forbid_calls=1;
    CHECK(gml_render_resource_acquire_cpu_read(resource,&reader) && reader==pixels &&
          reader[15]==0xFF345678u && fake.forbidden_calls==0 && fake.readbacks==2);
    operation.target.image=gml_render_resource_image_ref(resource);
    operation.target.image.content_version++;
    last_colour=replacement.readback_colour=format?0x4D789ABCu:0xFF789ABCu;
    CHECK(gml_render_resource_submit(resource,&next_device,&batch,NULL,0u)==
          GML_RENDER_RESOURCE_EXEC_DEVICE);
    for(unsigned index=0;index<16u;index++) CHECK(pixels[index]==last_colour);
    gml_render_resource_journal_metrics(resource,&metrics);
    CHECK(metrics.checkpoint_allocations==(cached?1u:3u) &&
          metrics.checkpoint_staging_bytes==(cached?64u:0u));
    replacement.fail_partial_readback=1;
    replacement.readback_colour=0xFFEEDDCCu;
    operation.target.image=gml_render_resource_image_ref(resource);
    operation.target.image.content_version++;
    CHECK(gml_render_resource_submit(resource,&next_device,&batch,NULL,0u)==
          GML_RENDER_RESOURCE_EXEC_FAILED);
    for(unsigned index=0;index<16u;index++) CHECK(pixels[index]==last_colour);
    CHECK(!gml_render_resource_acquire_cpu_read(resource,&reader) && !reader &&
          !gml_render_resource_checkpoint(resource,&next_device) && replacement.readbacks==2);
    gml_render_resource_journal_metrics(resource,&metrics);
    CHECK(metrics.authority==GML_RENDER_RESOURCE_INVALID && metrics.device_checkpoints==3u &&
          metrics.checkpoint_allocations==(cached?1u:4u));
    gml_render_resource_destroy(resource);

    /* A resized/recreated owner has new scratch; stale successful or partial bytes cannot
     * become the new incarnation's canonical pixels, including loss before its next seal. */
    image.incarnation++;
    for(unsigned index=0;index<64u;index++) larger[index]=0xFF102030u;
    replacement.fail_partial_readback=0;
    resource=gml_render_resource_create(image,8u,8u,formats[format],larger,limits);
    CHECK(resource);
    gml_render_resource_journal_metrics(resource,&metrics);
    CHECK(metrics.checkpoint_allocations==0u && metrics.checkpoint_staging_bytes==0u);
    operation.target.width=operation.target.height=8u;
    operation.target.region=(GmlRenderRect){0,0,8u,8u};
    cached=owner<2u && budget==0u;
    for(unsigned pass=0;pass<2u;pass++){
      operation.target.image=gml_render_resource_image_ref(resource);
      operation.target.image.content_version++;
      last_colour=replacement.readback_colour=(format?0x3A000000u:0xFF000000u)|(0x456789u+pass);
      CHECK(gml_render_resource_submit(resource,&next_device,&batch,NULL,0u)==
            GML_RENDER_RESOURCE_EXEC_DEVICE);
      for(unsigned index=0;index<64u;index++) CHECK(larger[index]==last_colour);
    }
    gml_render_resource_journal_metrics(resource,&metrics);
    CHECK(metrics.checkpoint_allocations==(cached?1u:2u) &&
          metrics.checkpoint_staging_bytes==(cached?256u:0u));
    operation.target.image=gml_render_resource_image_ref(resource);
    operation.target.image.content_version++;
    CHECK(gml_render_resource_submit_deferred(resource,&next_device,&batch,NULL,0u)==
          GML_RENDER_RESOURCE_EXEC_DEVICE);
    gml_render_resource_context_lost(resource);
    replacement.forbid_calls=1;
    CHECK(!gml_render_resource_acquire_cpu_read(resource,&reader) && !reader &&
          !gml_render_resource_checkpoint(resource,&next_device) &&
          replacement.forbidden_calls==0 && replacement.readbacks==4);
    for(unsigned index=0;index<64u;index++) CHECK(larger[index]==last_colour);
    gml_render_resource_destroy(resource);
  }
  return 1;
}

static int pending_shader_source_cross_target(void){
  uint32_t first_pixels[16],second_pixels[16],source_pixels[4];
  GmlRenderResourceSource source={0},pending={0};
  GmlRenderImageRef first_ref=target_ref(1u,1u),second_ref=target_ref(1u,2u);
  GmlRenderDrawOp first_op={0},second_op={0};
  GmlRenderDrawStateSnapshot first_state=quad_state(0),second_state=quad_state(0);
  GmlPlanShader shader={0};
  GmlRenderDrawBatch first_batch={&first_op,1u,&first_state,1u};
  GmlRenderDrawBatch second_batch={&second_op,1u,&second_state,1u};
  GmlRenderJournalMetrics metrics={0};
  FakeDevice fake={0};
  GmlRenderDeviceServices device=service(&fake);
  GmlRenderResource *first,*second;
  const uint32_t *observed=NULL;
  fill(first_pixels,0xFF010203u);
  fill(second_pixels,0xFF040506u);
  for(int index=0;index<4;index++) source_pixels[index]=0xFF112233u;
  source=source_view(source_pixels,1);
  fake.readback_colour=0xFF6587A9u;
  first=gml_render_resource_create(first_ref,4u,4u,GML_RENDER_PIXEL_ARGB8888,
    first_pixels,(GmlRenderResourceLimits){0});
  second=gml_render_resource_create(second_ref,4u,4u,GML_RENDER_PIXEL_ARGB8888,
    second_pixels,(GmlRenderResourceLimits){0});
  CHECK(first && second);
  shader.vertex_es="void main(){ gl_Position=vec4(0.0); }";
  shader.fragment_es="void main(){ gl_FragColor=vec4(1.0); }";
  for(int index=0;index<4;index++) shader.vertex_colour[index]=1.0f;
  first_op.opcode=second_op.opcode=GML_RENDER_OP_CONTENT_SHADER_DRAW;
  first_op.reconstruction_mode=second_op.reconstruction_mode=GML_RENDER_DEVICE_CHECKPOINT;
  first_op.source_count=second_op.source_count=1u;
  first_op.sources=&source.image;
  first_op.target.image=first_ref;
  first_op.target.image.content_version++;
  first_op.target.storage_version=1u;
  first_op.target.width=first_op.target.height=4u;
  first_op.target.pixel_format=GML_RENDER_PIXEL_ARGB8888;
  first_op.target.region=(GmlRenderRect){0,0,4u,4u};
  first_op.payload.shader=&shader;
  second_op=first_op;
  second_op.target.image=second_ref;
  second_op.target.image.content_version++;
  second_op.sources=&pending.image;
  first_state.sampler_count=second_state.sampler_count=1u;
  first_state.samplers[0].source_region=(GmlRenderRect){0,0,2u,2u};
  second_state.samplers[0].source_region=(GmlRenderRect){0,0,4u,4u};
  CHECK(gml_render_resource_submit_deferred(first,&device,&first_batch,&source,1u)==
        GML_RENDER_RESOURCE_EXEC_DEVICE);
  CHECK(fake.readbacks==0 && first_pixels[0]==0xFF010203u);
  gml_render_resource_journal_metrics(first,&metrics);
  CHECK(metrics.authority==GML_RENDER_RESOURCE_GPU_PENDING_CHECKPOINT &&
        metrics.cpu_version==1u && metrics.gpu_version==2u);
  CHECK(gml_render_resource_device_source_view(first,&device,&pending) &&
        !pending.bytes && pending.resident_resource==first &&
        !gml_render_resource_device_source_view(second,&device,&source));
  CHECK(gml_render_resource_submit(second,&device,&second_batch,&pending,1u)==
        GML_RENDER_RESOURCE_EXEC_DEVICE);
  CHECK(fake.submits==2 && fake.readbacks==1 &&
        first_pixels[0]==0xFF010203u && second_pixels[0]==fake.readback_colour);
  CHECK(gml_render_resource_checkpoint(first,&device) && fake.readbacks==2 &&
        first_pixels[0]==fake.readback_colour);
  CHECK(gml_render_resource_device_source_view(first,&device,&pending) &&
        !pending.bytes && pending.resident_resource==first && fake.readbacks==2);
  {
    FakeDevice foreign={0};
    GmlRenderDeviceServices foreign_device=service(&foreign);
    uint32_t *writer=NULL;
    CHECK(!gml_render_resource_device_source_view(first,&foreign_device,&source) &&
          !source.bytes && !source.resident_resource && fake.readbacks==2);
    CHECK(gml_render_resource_acquire_cpu_write(first,&writer) && writer==first_pixels &&
          !gml_render_resource_device_source_view(first,&device,&source));
    CHECK(gml_render_resource_commit_cpu_write(first) &&
          !gml_render_resource_device_source_view(first,&device,&source) &&
          fake.readbacks==2);
  }
  gml_render_resource_context_lost(first);
  gml_render_resource_context_lost(second);
  CHECK(gml_render_resource_acquire_cpu_read(first,&observed) &&
        observed[0]==fake.readback_colour &&
        gml_render_resource_acquire_cpu_read(second,&observed) &&
        observed[0]==fake.readback_colour && fake.readbacks==2);
  gml_render_resource_destroy(first);
  gml_render_resource_destroy(second);
  return 1;
}

static int public_checkpoint_readback_refusal(void){
  for(int application=0;application<2;application++){
    GmlRender *render=(GmlRender*)calloc(1,sizeof *render);
    FakeDevice fake={0};
    GmlRenderDeviceServices device=service(&fake);
    GmlRenderResource *resource;
    GmlRenderDrawOp operation=clear_op(2u,0xFF315779u,GML_RENDER_DEVICE_CHECKPOINT);
    GmlRenderDrawBatch batch={&operation,1u,NULL,0u};
    GmlRenderJournalMetrics metrics={0};
    GmlRenderApplicationReadView read_view={0};
    int id;
    CHECK(render);
    render->next_surface_id=1;
    if(application){
      CHECK(gml_render_application_surface_ensure_owned(render,4,4));
      id=0;
    } else {
      id=gml_surface_create(render,4,4);
      CHECK(id>0);
    }
    gml_render_bind_device(render,&device);
    resource=application?gml_render_application_resource_ensure(render):
                         gml_render_surface_resource_ensure(render,id);
    CHECK(resource);
    operation.target.image=gml_render_resource_image_ref(resource);
    operation.target.image.content_version++;
    operation.target.pixel_format=GML_RENDER_PIXEL_ARGB8888;
    fake.fail_readback=1;
    if(application){
      CHECK(gml_render_application_scene_submit(render,&batch,NULL,0u)==
            GML_RENDER_RESOURCE_EXEC_DEVICE && fake.readbacks==0);
      CHECK(!gml_render_application_surface_owned_read(render,&read_view) &&
            !read_view.pixels);
    } else {
      CHECK(gml_render_surface_scene_submit(render,id,&batch,NULL,0u)==
            GML_RENDER_RESOURCE_EXEC_FAILED);
    }
    gml_render_resource_journal_metrics(resource,&metrics);
    CHECK(metrics.authority==GML_RENDER_RESOURCE_INVALID &&
          fake.submits==1 && fake.readbacks==1 &&
          (application?render->app_surface_owned[5]:render->surface[id-1].px[5])==0u &&
          !gml_surface_pixels_read(render,id,NULL,NULL));
    if(application)
      CHECK(!gml_render_application_surface_owned_read(render,&read_view) &&
            !read_view.pixels);
    else CHECK(!gml_surface_exists(render,id));
    gml_render_free(render);
    free(render);
  }
  return 1;
}

static int queued_checkpoint_flush_refusal_preserves_target(void){
  GmlRender *render=(GmlRender*)calloc(1,sizeof *render);
  uint32_t base[16]={0},source_pixels[4]={0xFF315779u,0xFF315779u,
                                          0xFF315779u,0xFF315779u};
  GmlRenderResourceSource source=source_view(source_pixels,1);
  GmlRenderImageRef source_image=source.image;
  GmlRenderDrawOp operation=quad_op(2u,&source_image);
  GmlRenderDrawStateSnapshot state=quad_state(0);
  GmlRenderJournalMetrics metrics={0};
  FakeDevice fake={0};
  GmlRenderDeviceServices device=service(&fake);
  GmlRenderResource *resource;
  int id;
  CHECK(render);
  render->next_surface_id=1;
  gml_render_begin(render,base,4,4,0.0,0.0);
  CHECK(gml_render_application_surface_ensure_owned(render,4,4));
  id=gml_surface_create(render,4,4);
  CHECK(id>0);
  gml_render_bind_device(render,&device);
  CHECK(gml_render_begin_resident_owned_application(render,0.0,0.0,0));
  resource=gml_render_application_resource_ensure(render);
  CHECK(resource);
  operation.reconstruction_mode=GML_RENDER_DEVICE_CHECKPOINT;
  operation.target.image=gml_render_resource_image_ref(resource);
  operation.target.image.content_version++;
  operation.target.pixel_format=GML_RENDER_PIXEL_ARGB8888;
  CHECK(gml_render_application_scene_enqueue(render,&operation,&state,&source));
  fake.readback_colour=0xFF315779u;
  fake.fail_upload_at=1;
  CHECK(!gml_surface_set_target(render,id) && fake.uploads==1 &&
        fake.submits==0 && render->target_id==-1 && render->target_sp==0 &&
        render->fb==render->app_surface_owned && gml_surface_exists(render,id));
  gml_render_resource_journal_metrics(resource,&metrics);
  CHECK(metrics.authority==GML_RENDER_RESOURCE_CPU_CURRENT &&
        metrics.logical_version==1u && metrics.cpu_version==1u &&
        metrics.gpu_version==0u && metrics.journal_operations==0u &&
        render->app_surface_owned[5]==0u);
  fake.fail_upload_at=0;
  CHECK(gml_surface_set_target(render,id) && render->target_id==id &&
        render->target_sp==1 && render->fb==render->surface[id-1].px &&
        fake.submits==1 && fake.readbacks==0);
  CHECK(gml_render_application_surface_owned_read(render,NULL) &&
        fake.submits==1 && fake.readbacks==1 &&
        render->app_surface_owned[5]==0xFF315779u);
  gml_render_resource_journal_metrics(resource,&metrics);
  CHECK(metrics.authority==GML_RENDER_RESOURCE_BOTH_CURRENT &&
        metrics.logical_version==2u && metrics.cpu_version==2u &&
        metrics.gpu_version==2u && metrics.device_checkpoints==1u);
  gml_surface_reset_target(render);
  CHECK(render->target_id==-1 && render->target_sp==0 &&
        render->fb==render->app_surface_owned && fake.readbacks==1);
  CHECK(gml_render_begin_resident_owned_application(render,0.0,0.0,0));
  operation.target.image=gml_render_resource_image_ref(resource);
  operation.target.image.content_version++;
  CHECK(gml_render_application_scene_enqueue(render,&operation,&state,&source));
  fake.fail_readback=1;
  /* Switching away no longer observes application pixels. Failed frame-end sealing must
   * still preserve the current binding/stack and refuse the previous canonical bytes. */
  CHECK(!gml_render_scene_flush(render) && render->target_id==-1 &&
        render->target_sp==0 && render->fb==render->app_surface_owned &&
        fake.submits==2 && fake.readbacks==2);
  gml_render_resource_journal_metrics(resource,&metrics);
  CHECK(metrics.authority==GML_RENDER_RESOURCE_INVALID &&
        gml_render_application_scene_flush(render) && fake.submits==2 &&
        !gml_render_application_surface_owned_read(render,NULL));
  gml_render_free(render);
  free(render);
  return 1;
}

static int queued_quad_groups_are_atomic(void){
  for(unsigned variant=0;variant<8u;variant++){
    GmlRender *r=calloc(1,sizeof *r);
    uint32_t pixels[4]={0xFF315779u,0xFF315779u,0xFF315779u,0xFF315779u};
    GmlRenderResourceSource source=source_view(pixels,1);
    GmlRenderImageRef wrong=source.image;
    GmlRenderDrawStateSnapshot state=quad_state(0);
    GmlRenderDrawOp *prefix=calloc(2047u,sizeof *prefix),group[3];
    GmlRenderJournalMetrics metrics={0};
    FakeDevice fake={.on_demand=1};
    GmlRenderDeviceServices device=service(&fake);
    CHECK(r && prefix && gml_render_application_surface_ensure_owned(r,4,4));
    device.observe_on_demand=fake_observe_on_demand;
    gml_render_bind_device(r,&device);
    CHECK(gml_render_begin_resident_owned_application(r,0,0,0));
    GmlRenderResource *target=gml_render_application_resource_ensure(r);
    CHECK(target);
    GmlRenderImageRef image=gml_render_resource_image_ref(target);
    uint32_t prefix_count=variant>=6u?2047u:1u;
    for(uint32_t i=0;i<prefix_count;i++){
      prefix[i]=quad_op(image.content_version+i+1u,&source.image);
      prefix[i].reconstruction_mode=GML_RENDER_DEVICE_CHECKPOINT;
      prefix[i].target.image=image;prefix[i].target.image.content_version+=i+1u;
      prefix[i].target.pixel_format=GML_RENDER_PIXEL_ARGB8888;
    }
    CHECK(gml_render_application_scene_enqueue_quads(r,prefix,prefix_count,&state,&source));
    for(uint32_t i=0;i<3u;i++){
      group[i]=prefix[0];group[i].target.image.content_version+=prefix_count+i;
    }
    wrong.identity++;
    if(variant==1u) group[1].target.image.content_version++;
    if(variant==2u) group[1].sources=&wrong;
    if(variant==3u) group[1].target.width++;
    if(variant==4u) group[1].payload.quad.sampling.filter=GML_RENDER_FILTER_BILINEAR;
    if(variant==5u) source.borrowed_immutable=0;
    if(variant==6u) fake.fail_upload_at=fake.uploads+1;
    if(variant<7u){
      CHECK(!gml_render_application_scene_enqueue_quads(r,group,
        variant==0u?0u:3u,&state,&source));
      CHECK(!fake.submits && !fake.readbacks &&
        gml_render_application_scene_next_image_ref(r,target).content_version==
          image.content_version+prefix_count);
      CHECK(!gml_render_application_scene_enqueue_quads(r,group,2049u,&state,&source));
      fake.fail_upload_at=0;source.borrowed_immutable=1;
      if(variant==6u){
        CHECK(gml_render_application_scene_enqueue_quads(r,group,3u,&state,&source));
        CHECK(fake.submits==1 && fake.last_batch_operations==prefix_count);
      }
    } else CHECK(gml_render_application_scene_enqueue_quads(r,group,3u,&state,&source));
    CHECK(gml_render_application_scene_flush(r) && !fake.readbacks &&
      fake.last_batch_operations==(variant>=6u?3u:prefix_count));
    gml_render_resource_journal_metrics(target,&metrics);
    CHECK(metrics.authority==GML_RENDER_RESOURCE_GPU_RESIDENT && !metrics.cpu_replays &&
      !metrics.journal_operations && metrics.logical_version==
        image.content_version+prefix_count+(variant>=6u?3u:0u));
    gml_render_abandon_device(r);gml_render_free(r);free(r);free(prefix);
  }
  return 1;
}

static int queued_multicell_preparation_is_transactional(void){
  for(unsigned variant=0;variant<5u;variant++){
    GmlRender *r=calloc(1,sizeof *r);
    uint32_t pixels[4]={0xFF315779u,0xFF315779u,0xFF315779u,0xFF315779u};
    GmlRenderResourceSource source=source_view(pixels,1),second=source;
    GmlRenderDrawStateSnapshot state=quad_state(0),changed=state;
    FakeDevice fake={.on_demand=1};GmlRenderDeviceServices device=service(&fake);
    GmlRenderJournalMetrics metrics={0};
    CHECK(r && gml_render_application_surface_ensure_owned(r,4,4));
    device.observe_on_demand=fake_observe_on_demand;
    gml_render_bind_device(r,&device);
    CHECK(gml_render_begin_resident_owned_application(r,0,0,0));
    GmlRenderResource *target=gml_render_application_resource_ensure(r);
    CHECK(target);
    GmlRenderImageRef image=gml_render_resource_image_ref(target);
    unsigned prefix_count=variant==2u?2047u:2u;
    for(unsigned index=0;index<prefix_count;index++){
      GmlRenderDrawOp op=quad_op(image.content_version+index+1u,&source.image);
      op.reconstruction_mode=GML_RENDER_DEVICE_CHECKPOINT;
      op.scene_family=GML_RENDER_SCENE_SPRITE;
      op.target.image=image;op.target.image.content_version+=index+1u;
      op.target.pixel_format=GML_RENDER_PIXEL_ARGB8888;
      CHECK(gml_render_application_scene_enqueue(r,&op,&state,&source));
    }
    r->fb_opaque_known=1;r->fb_all_opaque=0;r->fb_all_transparent=1;
    r->app_surface_opaque=1;r->app_presentation_coverage_active=1;
    CHECK(gml_render_application_scene_prepare_begin(r));
    CHECK(!gml_render_application_scene_prepare_begin(r));
    CHECK(!gml_render_application_scene_flush(r) && !fake.submits && !fake.uploads);
    second.image.identity++;changed.write_mask=7u;
    if(variant==3u) second.borrowed_immutable=0;
    for(unsigned index=0;index<2u;index++){
      GmlRenderDrawOp op=quad_op(image.content_version+prefix_count+index+1u,&second.image);
      op.reconstruction_mode=GML_RENDER_DEVICE_CHECKPOINT;
      op.scene_family=GML_RENDER_SCENE_SPRITE;
      op.target.image=image;op.target.image.content_version+=prefix_count+index+1u;
      op.target.pixel_format=GML_RENDER_PIXEL_ARGB8888;
      int accepted=gml_render_application_scene_enqueue(r,&op,&changed,&second);
      CHECK(accepted!=(int)(variant==3u || (variant==2u && index==1u)));
      if(!accepted) break;
    }
    CHECK(!fake.submits && !fake.uploads && !fake.readbacks);
    r->fb_opaque_known=0;r->fb_all_opaque=1;r->fb_all_transparent=0;
    r->app_surface_opaque=0;r->app_presentation_coverage_active=0;
    if(variant==4u){
      /* Teardown also owns an unfinished preparation's retained parent. */
      gml_render_abandon_device(r);gml_render_free(r);free(r);continue;
    }
    CHECK(gml_render_application_scene_prepare_finish(r,variant==1u)==(int)(variant==1u));
    if(variant!=1u) CHECK(r->fb_opaque_known==1 && !r->fb_all_opaque && r->fb_all_transparent==1 &&
      r->app_surface_opaque==1 && r->app_presentation_coverage_active==1);
    CHECK(!fake.submits && !fake.uploads && !fake.readbacks);
    CHECK(gml_render_application_scene_next_image_ref(r,target).content_version==
      image.content_version+prefix_count+(variant==1u?2u:0u));
    CHECK(gml_render_resource_image_ref(target).content_version==image.content_version);
    CHECK(gml_render_application_scene_flush(r) && fake.submits==1 && !fake.readbacks);
    CHECK(fake.last_batch_operations==prefix_count+(variant==1u?2u:0u));
    if(variant==1u) CHECK(fake.last_batch_states==2u &&
      fake.last_batch_source_ids[0]==source.image.identity &&
      fake.last_batch_source_ids[1]==source.image.identity &&
      fake.last_batch_source_ids[2]==second.image.identity &&
      fake.last_batch_state_indices[0]==fake.last_batch_state_indices[1] &&
      fake.last_batch_state_indices[2]!=fake.last_batch_state_indices[0]);
    gml_render_resource_journal_metrics(target,&metrics);
    CHECK(metrics.authority==GML_RENDER_RESOURCE_GPU_RESIDENT && !metrics.cpu_replays &&
      !metrics.journal_operations && metrics.logical_version==image.content_version+prefix_count+(variant==1u?2u:0u));
    gml_render_abandon_device(r);gml_render_free(r);free(r);
  }
  return 1;
}

static GmlRender *paired_root_fixture(FakeDevice *fake,uint32_t *base,uint32_t *vertical){
  GmlRender *r=calloc(1,sizeof *r);
  if(!r) return NULL;
  GmlRenderDeviceServices device=service(fake);
  device.observe_on_demand=fake_observe_on_demand;
  gml_render_bind_device(r,&device);
  if(!gml_render_begin_resident_base(r,base,4,4,0,0)){
    gml_render_free(r);free(r);return NULL;
  }
  r->classic=1;r->classic_phase_y=vertical;
  if(!gml_render_classic_phase_resource_bind(r)){
    gml_render_free(r);free(r);return NULL;
  }
  return r;
}

static int paired_root_append(GmlRender *r,const GmlRenderResourceSource *source){
  GmlRenderResource *target=gml_render_scene_target_resource_ensure(r);
  GmlRenderImageRef image=gml_render_application_scene_next_image_ref(r,target);
  GmlRenderDrawStateSnapshot state=quad_state(0);
  GmlRenderDrawOp op=quad_op(image.content_version+1u,&source->image);
  op.reconstruction_mode=GML_RENDER_DEVICE_CHECKPOINT;
  op.scene_family=GML_RENDER_SCENE_SPRITE;
  op.target.image=image;op.target.image.content_version++;
  op.target.pixel_format=GML_RENDER_PIXEL_ARGB8888;
  return target && gml_render_application_scene_enqueue(r,&op,&state,source);
}

static int paired_root_queue_groups_and_versions(void){
  uint32_t base[16]={0},vertical[16]={0},pixels[4]={0xFF315779u};
  FakeDevice fake={.on_demand=1};
  GmlRender *r=paired_root_fixture(&fake,base,vertical);
  GmlRenderResourceSource source=source_view(pixels,1);
  CHECK(r && !fake.uploads && !fake.submits && !fake.readbacks);
  CHECK(!gml_render_classic_phase_scene_select(r,1));
  CHECK(gml_render_classic_vertical_phase_blocks_scene(r));
  GmlRenderImageRef base_image=gml_render_resource_image_ref(r->base_scene_resource);
  GmlRenderImageRef phase_image=gml_render_resource_image_ref(r->classic_phase_resource);
  CHECK(base_image.incarnation!=phase_image.incarnation &&
    phase_image.resource_class==GML_RENDER_RESOURCE_SCRATCH);
  for(unsigned draw=0;draw<12u;draw++){
    CHECK(gml_render_application_scene_prepare_begin(r));
    CHECK(paired_root_append(r,&source));
    CHECK(gml_render_classic_phase_scene_select(r,1));
    CHECK(r->fb==base && r->base_fb==base && r->fbw==4 && r->fbh==4);
    CHECK(paired_root_append(r,&source));
    CHECK(gml_render_application_scene_prepare_finish(r,1));
    CHECK(!r->classic_phase_scene_target && !fake.submits && !fake.readbacks);
    CHECK(gml_render_application_scene_next_image_ref(r,r->base_scene_resource).content_version==draw+2u);
    CHECK(gml_render_application_scene_next_image_ref(r,r->classic_phase_resource).content_version==draw+2u);
  }
  CHECK(gml_render_application_scene_flush(r) && fake.submits==2 && !fake.readbacks);
  CHECK(fake.submitted_targets[0].resource_class==GML_RENDER_RESOURCE_SCREEN &&
    fake.submitted_targets[1].resource_class==GML_RENDER_RESOURCE_SCRATCH &&
    fake.submitted_counts[0]==12u && fake.submitted_counts[1]==12u);
  for(unsigned plane=0;plane<2u;plane++){
    GmlRenderResource *resource=plane?r->classic_phase_resource:r->base_scene_resource;
    GmlRenderJournalMetrics metrics={0};gml_render_resource_journal_metrics(resource,&metrics);
    CHECK(metrics.authority==GML_RENDER_RESOURCE_GPU_RESIDENT && metrics.logical_version==13u &&
      metrics.cpu_version==1u && !metrics.cpu_replays && !metrics.journal_operations);
  }
  CHECK(base[0]==0u && vertical[0]==0u && gml_render_device_observation_pending(r));
  fake.readback_colour=0x80456789u;
  CHECK(gml_render_observe_device_resources(r) && fake.readbacks==2);
  CHECK(base[0]==0x80456789u && vertical[0]==0x80456789u &&
    !gml_render_device_observation_pending(r));
  gml_render_free(r);free(r);return 1;
}

static int paired_root_preparation_rolls_back_both_planes(void){
  uint32_t base[16]={0},vertical[16]={0},pixels[4]={0xFF315779u};
  FakeDevice fake={.on_demand=1};GmlRender *r=paired_root_fixture(&fake,base,vertical);
  GmlRenderResourceSource source=source_view(pixels,1);
  CHECK(r && paired_root_append(r,&source));
  r->fb_opaque_known=1;r->fb_all_opaque=0;r->fb_all_transparent=1;
  r->classic_phase_opaque_known=1;r->classic_phase_all_opaque=1;
  CHECK(gml_render_application_scene_prepare_begin(r));
  CHECK(paired_root_append(r,&source) && gml_render_classic_phase_scene_select(r,1));
  CHECK(r->fb_opaque_known && r->fb_all_opaque && !r->fb_all_transparent);
  CHECK(paired_root_append(r,&source));
  r->fb_opaque_known=0;r->fb_all_opaque=0;r->fb_all_transparent=1;
  CHECK(!gml_render_application_scene_prepare_finish(r,0));
  CHECK(!r->classic_phase_scene_target && r->fb_opaque_known &&
    !r->fb_all_opaque && r->fb_all_transparent && r->classic_phase_opaque_known &&
    r->classic_phase_all_opaque && !r->classic_phase_all_transparent);
  CHECK(!fake.submits && !fake.readbacks &&
    gml_render_application_scene_next_image_ref(r,r->base_scene_resource).content_version==2u &&
    gml_render_application_scene_next_image_ref(r,r->classic_phase_resource).content_version==1u);
  CHECK(gml_render_application_scene_flush(r) && fake.submits==1 &&
    fake.last_target_image.resource_class==GML_RENDER_RESOURCE_SCREEN && fake.last_batch_operations==1u);
  gml_render_abandon_device(r);gml_render_free(r);free(r);return 1;
}

static int paired_root_clear_versions_and_coverage(void){
  const uint32_t colours[]={0x00315779u,0x01315779u,0x7F315779u,
                            0x80315779u,0xFE315779u,0xFF315779u};
  for(unsigned control=0;control<6u;control++){
    uint32_t base[16],vertical[16];
    for(unsigned p=0;p<16u;p++){base[p]=0xA039587Bu;vertical[p]=0x407B5839u;}
    FakeDevice fake={.on_demand=1};GmlRender *r=paired_root_fixture(&fake,base,vertical);
    CHECK(r);
    gml_render_application_surface_bind(r,base,4,4,1);
    r->app_surface_opaque=r->app_presentation_coverage_active=1;
    r->color_write_mask=control%2u?0u:7u;r->blendmode=(int)control;
    CHECK(gml_render_resident_classic_phase_clear(r,colours[control]) &&
      !fake.submits && !fake.uploads && !fake.readbacks && !r->pending_fill &&
      !r->classic_phase_scene_target && !r->classic_phase_scene_preparing &&
      gml_render_classic_vertical_phase_blocks_scene(r) && r->app_surface_opaque &&
      !r->app_presentation_coverage_active && r->fb_opaque_known && r->classic_phase_opaque_known &&
      r->fb_all_opaque==(control==5u) && r->classic_phase_all_opaque==(control==5u) &&
      r->fb_all_transparent==(control==0u) && r->classic_phase_all_transparent==(control==0u));
    CHECK(gml_render_application_scene_next_image_ref(r,r->base_scene_resource).content_version==2u &&
      gml_render_application_scene_next_image_ref(r,r->classic_phase_resource).content_version==2u &&
      gml_render_application_scene_flush(r) && fake.submits==2 && !fake.readbacks &&
      fake.last_opcode==GML_RENDER_OP_CLEAR && !fake.last_source_count && !fake.last_batch_states);
    for(unsigned plane=0;plane<2u;plane++){
      GmlRenderJournalMetrics metrics={0};
      gml_render_resource_journal_metrics(plane?r->classic_phase_resource:r->base_scene_resource,&metrics);
      CHECK(metrics.authority==GML_RENDER_RESOURCE_GPU_RESIDENT && metrics.logical_version==2u &&
        metrics.gpu_version==2u && metrics.cpu_version==1u && !metrics.cpu_replays && !metrics.journal_operations);
    }
    for(unsigned p=0;p<16u;p++) CHECK(base[p]==0xA039587Bu && vertical[p]==0x407B5839u);
    CHECK(gml_render_resident_classic_phase_clear(r,0xFF26486Au) &&
      gml_render_application_scene_flush(r) && fake.submits==4 && !fake.readbacks &&
      gml_render_resource_image_ref(r->base_scene_resource).content_version==3u &&
      gml_render_resource_image_ref(r->classic_phase_resource).content_version==3u);
    gml_render_abandon_device(r);gml_render_free(r);free(r);
  }
  return 1;
}

static int paired_root_clear_refusal_preserves_order(void){
  for(unsigned prefix=0;prefix<2u;prefix++){
    uint32_t base[16]={0},vertical[16]={0},pixels[4]={0xFF315779u};
    FakeDevice fake={.on_demand=1,.prewrite_refusal=1,.fail_submit_at=2};
    GmlRender *r=paired_root_fixture(&fake,base,vertical);
    GmlRenderResourceSource source=source_view(pixels,1);
    CHECK(r);
    if(prefix){
      CHECK(gml_render_application_scene_prepare_begin(r) && paired_root_append(r,&source) &&
        gml_render_classic_phase_scene_select(r,1) && paired_root_append(r,&source) &&
        gml_render_application_scene_prepare_finish(r,1));
      r->fb_opaque_known=r->classic_phase_opaque_known=0;
      CHECK(!gml_render_resident_classic_phase_clear(r,0xFF26486Au) &&
        !r->fb_opaque_known && !r->classic_phase_opaque_known);
    }else{
      CHECK(gml_render_resident_classic_phase_clear(r,0xFF315779u) &&
        !gml_render_application_scene_flush(r));
    }
    CHECK(fake.submits==2 && !fake.readbacks &&
      gml_render_resource_image_ref(r->base_scene_resource).content_version==2u &&
      gml_render_resource_image_ref(r->classic_phase_resource).content_version==1u &&
      gml_render_application_scene_next_image_ref(r,r->base_scene_resource).content_version==2u &&
      gml_render_application_scene_next_image_ref(r,r->classic_phase_resource).content_version==2u);
    fake.fail_submit_at=0;
    CHECK(gml_render_resident_classic_phase_clear(r,0x8037597Bu) && fake.submits==3 &&
      !fake.readbacks && fake.submitted_targets[2].resource_class==GML_RENDER_RESOURCE_SCRATCH &&
      fake.submitted_counts[2]==1u &&
      gml_render_application_scene_next_image_ref(r,r->base_scene_resource).content_version==3u &&
      gml_render_application_scene_next_image_ref(r,r->classic_phase_resource).content_version==3u &&
      gml_render_application_scene_flush(r) && fake.submits==5 && !fake.readbacks &&
      gml_render_resource_image_ref(r->base_scene_resource).content_version==3u &&
      gml_render_resource_image_ref(r->classic_phase_resource).content_version==3u);
    gml_render_abandon_device(r);gml_render_free(r);free(r);
  }
  return 1;
}

static int paired_root_clear_scope_is_explicit(void){
  CHECK(!gml_render_resident_classic_phase_clear(NULL,0u));
  for(unsigned excluded=0;excluded<10u;excluded++){
    uint32_t base[16]={0},vertical[16]={0};
    FakeDevice fake={.on_demand=1};GmlRender *r=paired_root_fixture(&fake,base,vertical);
    GmlRender *saved=malloc(sizeof *saved);CHECK(r && saved);
    if(excluded==0u) r->pending_fill=1;
    if(excluded==1u) r->pending_underlay=1;
    if(excluded==2u) r->pending_presentation=1;
    if(excluded==3u) r->classic_phase_y=NULL;
    if(excluded==4u) r->classic_phase_scene_preparing=1;
    if(excluded==5u) r->target_sp=1;
    if(excluded==6u) r->interp=1;
    if(excluded==7u) r->classic_phase_y=base;
    if(excluded==8u) CHECK(gml_render_application_scene_prepare_begin(r));
    if(excluded==9u) fake.on_demand=0;
    memcpy(saved,r,sizeof *r);
    CHECK(!gml_render_resident_classic_phase_clear(r,0xFF315779u) &&
      !memcmp(saved,r,sizeof *r) && !fake.submits && !fake.uploads && !fake.readbacks);
    memcpy(r,saved,sizeof *r);free(saved);
    r->pending_fill=r->pending_underlay=r->pending_presentation=r->gui_pass_active=0;
    r->classic_phase_scene_preparing=r->target_sp=r->interp=0;r->classic_phase_y=vertical;
    if(excluded==8u) CHECK(!gml_render_application_scene_prepare_finish(r,0));
    fake.on_demand=1;
    gml_render_abandon_device(r);gml_render_free(r);free(r);
  }
  return 1;
}

static GmlRender *paired_root_atlas_fixture(FakeDevice *fake,GmlWin *content,
                                            uint32_t *base,uint32_t *vertical,int height){
  GmlRender *r=calloc(1,sizeof *r);
  if(!r || gml_render_init(r,NULL)!=0){free(r);return NULL;}
  r->win=content;r->classic=1;
  /* Initialization already owns the default font atlas. Keep that record
   * and replace only its pixels before any scene source can borrow them. */
  if(!r->atlas || r->n_atlas!=1){gml_render_free(r);free(r);return NULL;}
  free(r->atlas[0].px);r->atlas[0].px=NULL;
  r->atlas[0].w=r->atlas[0].h=2;
  r->atlas[0].px=malloc(16u);
  if(!r->atlas[0].px){gml_render_free(r);free(r);return NULL;}
  const uint8_t pixels[]={43,87,131,255,131,43,87,128,87,131,43,0,43,131,87,255};
  memcpy(r->atlas[0].px,pixels,sizeof pixels);
  GmlRenderDeviceServices device=service(fake);device.observe_on_demand=fake_observe_on_demand;
  gml_render_bind_device(r,&device);
  if(!gml_render_begin_resident_base(r,base,4,height,0,0)){
    gml_render_free(r);free(r);return NULL;
  }
  r->classic_phase_y=vertical;
  if(!gml_render_classic_phase_resource_bind(r)){
    gml_render_free(r);free(r);return NULL;
  }
  r->fb_opaque_known=r->fb_all_opaque=r->classic_phase_opaque_known=r->classic_phase_all_opaque=1;
  return r;
}

static int paired_root_public_clear_keeps_device_authority(void){
  for(unsigned fallback=0;fallback<2u;fallback++){
    uint32_t base[16],vertical[16];
    for(unsigned p=0;p<16u;p++){base[p]=0xFF315779u;vertical[p]=0x8037597Bu;}
    FakeDevice fake={.on_demand=1};GmlWin content={.classic_version=810};
    GmlRender *r=paired_root_atlas_fixture(&fake,&content,base,vertical,4);
    CHECK(r);
    gml_render_set_pending_fill(r,0x7F26486Au);
    CHECK(!r->pending_fill && r->resident_application_pass && !fake.readbacks &&
      gml_render_application_scene_flush(r) && fake.submits==2 && !fake.readbacks);
    for(unsigned p=0;p<16u;p++) CHECK(base[p]==0xFF315779u && vertical[p]==0x8037597Bu);
    gml_render_gui_begin(r,4,4);gml_render_gui_set_size(r,2,2);
    r->interp=(int)fallback;
    gml_render_set_pending_fill(r,0x00315779u);
    if(fallback){
      CHECK(!r->resident_application_pass && r->pending_fill && fake.readbacks==2);
      gml_render_flush_pending_fill(r);
      CHECK(!r->pending_fill && fake.readbacks==2);
      for(unsigned p=0;p<16u;p++) CHECK(base[p]==0x00315779u && vertical[p]==0x00315779u);
    }else{
      CHECK(!r->pending_fill && r->resident_application_pass && !fake.readbacks &&
        gml_render_application_scene_flush(r) && fake.submits==4);
      for(unsigned p=0;p<16u;p++) CHECK(base[p]==0xFF315779u && vertical[p]==0x8037597Bu);
    }
    r->gui_pass_active=r->interp=0;
    gml_render_abandon_device(r);gml_render_free(r);free(r);
  }
  return 1;
}

static int paired_root_public_sprite_and_base_only_glyph(void){
  for(unsigned glyph=0;glyph<2u;glyph++){
    uint32_t base[16],vertical[16];
    for(unsigned p=0;p<16u;p++) base[p]=vertical[p]=0xFF315779u;
    FakeDevice fake={.on_demand=1};GmlWin content={.classic_version=810};
    GmlRender *r=paired_root_atlas_fixture(&fake,&content,base,vertical,4);CHECK(r);
    r->spr=calloc(1,sizeof *r->spr);r->tpag=calloc(1,sizeof *r->tpag);
    CHECK(r->spr && r->tpag);r->n_spr=r->n_tpag=1;
    r->spr[0]=(GmlSprite){.w=2,.h=2,.n_frames=1,.frame=calloc(1,sizeof(int))};
    CHECK(r->spr[0].frame);
    r->tpag[0]=(GmlTpag){.sw=2,.sh=2,.tw=2,.th=2,.bw=2,.bh=2};
    if(glyph) CHECK(gml_render_resident_rotated_font_glyph(r,&r->spr[0],&r->tpag[0],
      1,1,1,1,17,0x73A5D9u,.61));
    else gml_draw_sprite_ext(r,0,0,1,1,1,1,0,0x73A5D9u,.61);
    CHECK(r->resident_application_pass && !r->classic_phase_scene_preparing &&
      !r->classic_phase_scene_target && !fake.readbacks &&
      gml_render_application_scene_flush(r) && fake.submits==(glyph?1:2) && !fake.readbacks);
    GmlRenderJournalMetrics metrics={0};
    gml_render_resource_journal_metrics(r->classic_phase_resource,&metrics);
    CHECK(metrics.logical_version==(glyph?1u:2u) && !metrics.cpu_replays &&
      !metrics.journal_operations && r->classic_phase_opaque_known && r->classic_phase_all_opaque);
    for(unsigned p=0;p<16u;p++) CHECK(base[p]==0xFF315779u && vertical[p]==0xFF315779u);
    gml_render_abandon_device(r);gml_render_free(r);free(r);
  }
  return 1;
}

/* A retained root peer does not own draws on an independent numbered image. */
static int paired_root_numbered_ports_keep_peer_inert(void){
  for(unsigned port=0;port<5u;port++){
    uint32_t base[16],vertical[16],shadow[16];
    for(unsigned p=0;p<16u;p++){base[p]=0xFF315779u;vertical[p]=0x7F27496Bu;}
    FakeDevice fake={.on_demand=1};GmlWin content={.classic_version=810};
    GmlRender *r=paired_root_atlas_fixture(&fake,&content,base,vertical,4);CHECK(r);
    r->tpag=calloc(1,sizeof *r->tpag);CHECK(r->tpag);r->n_tpag=1;
    r->tpag[0]=(GmlTpag){.sw=2,.sh=2,.tw=2,.th=2,.bw=2,.bh=2};
    int id=gml_surface_create(r,4,4);
    CHECK(id>0 && gml_surface_set_target(r,id) && r->classic_phase_resource &&
      r->classic_phase_y==vertical && !gml_render_classic_vertical_phase_blocks_scene(r));
    gml_render_set_pending_fill(r,0x7F315779u);gml_render_flush_pending_fill(r);
    CHECK(gml_render_scene_submit_pending(r));
    int submitted=fake.submits;memcpy(shadow,r->fb,sizeof shadow);
    for(unsigned inactive=0;inactive<2u;inactive++){
      if(inactive) r->classic_phase_y=NULL;
      int accepted=0;
      if(port==0u) accepted=gml_render_resident_font_glyph(r,0,0,0,2,2,1,1,1,1,0xFFFFFFu,1.0);
      else if(port==1u) accepted=gml_render_resident_sprite_font_glyph(r,0,1,1,1,1,0xFFFFFFu,1.0);
      else if(port==2u){
        GmlSprite cell={.w=2,.h=2};
        accepted=gml_render_resident_rotated_font_glyph(r,&cell,&r->tpag[0],1,1,1,1,17,0xFFFFFFu,1.0);
      }else if(port==3u)
        accepted=gml_render_resident_selected_background_page(r,&r->tpag[0],1,1,1,1,0xFFFFFFu,1.0);
      else{
        r->tpag[0].project_authored_edges=1;
        accepted=gml_render_resident_nearest_tile_page(r,&r->tpag[0],1,1,1,1,0xFFFFFFu,1.0);
      }
      if(inactive) CHECK(!accepted && fake.submits==submitted+1);
      else CHECK(accepted && r->resident_application_pass && gml_render_scene_submit_pending(r) &&
        fake.submits==submitted+1);
      CHECK(!fake.readbacks && !fake.rgb_queries && !memcmp(shadow,r->fb,sizeof shadow));
    }
    r->classic_phase_y=vertical;
    GmlRenderJournalMetrics peer={0};
    gml_render_resource_journal_metrics(r->classic_phase_resource,&peer);
    CHECK(peer.logical_version==1u && !peer.cpu_replays && !peer.journal_operations);
    for(unsigned p=0;p<16u;p++) CHECK(base[p]==0xFF315779u && vertical[p]==0x7F27496Bu);
    gml_render_abandon_device(r);gml_render_free(r);free(r);
  }
  return 1;
}

static int paired_root_atlas_ports_are_atomic(void){
  for(int port=GML_RENDER_PHASE_TEXT;port<=GML_RENDER_PHASE_SPRITE_TEXT;port++)
  for(unsigned inverse=0;inverse<2u;inverse++) for(unsigned shape=0;shape<2u;shape++){
    uint32_t base[4*48],vertical[4*48];
    for(unsigned p=0;p<4u*48u;p++) base[p]=vertical[p]=0xFF315779u;
    FakeDevice fake={.on_demand=1};GmlWin content={.classic_version=810};
    GmlRender *r=paired_root_atlas_fixture(&fake,&content,base,vertical,shape?48:4);
    CHECK(r);
    GmlTpag page={.sw=2,.sh=2,.tw=2,.th=2,.bw=2,.bh=2,
      .project_authored_edges=port==GML_RENDER_PHASE_TILE};
    r->blendmode=inverse?2:0;
    CHECK(gml_render_resident_classic_phase_atlas(r,&page,1,1,1,1,0x73A5D9u,
      port==GML_RENDER_PHASE_ROOM_TILE?1.0:.61,(GmlRenderClassicPhaseTexturePort)port));
    CHECK(!fake.uploads && !fake.submits && !fake.readbacks && !r->classic_phase_scene_target &&
      !r->classic_phase_scene_preparing && !r->classic_phase_scene_background_normalised &&
      gml_render_classic_vertical_phase_blocks_scene(r));
    CHECK(gml_render_application_scene_flush(r) && fake.submits==2 && !fake.readbacks &&
      fake.submitted_targets[0].resource_class==GML_RENDER_RESOURCE_SCREEN &&
      fake.submitted_targets[1].resource_class==GML_RENDER_RESOURCE_SCRATCH);
    for(unsigned plane=0;plane<2u;plane++){
      GmlRenderJournalMetrics metrics={0};
      gml_render_resource_journal_metrics(plane?r->classic_phase_resource:r->base_scene_resource,&metrics);
      CHECK(metrics.authority==GML_RENDER_RESOURCE_GPU_RESIDENT &&
        metrics.logical_version==metrics.gpu_version && metrics.cpu_version==1u &&
        !metrics.cpu_replays && !metrics.journal_operations);
    }
    for(unsigned p=0;p<4u*48u;p++) CHECK(base[p]==0xFF315779u && vertical[p]==0xFF315779u);
    gml_render_abandon_device(r);gml_render_free(r);free(r);
  }
  return 1;
}

static int paired_root_atlas_refuses_unqualified_ports(void){
  for(unsigned excluded=0;excluded<12u;excluded++){
    uint32_t base[16]={0},vertical[16]={0};
    FakeDevice fake={.on_demand=1};GmlWin content={.classic_version=810};
    GmlRender *r=paired_root_atlas_fixture(&fake,&content,base,vertical,4);
    GmlRender *saved=malloc(sizeof *saved);CHECK(r && saved);
    GmlTpag page={.sw=2,.sh=2,.tw=2,.th=2,.bw=2,.bh=2};
    double alpha=1.0,xscale=1.0,yscale=1.0;
    GmlRenderClassicPhaseTexturePort port=GML_RENDER_PHASE_TEXT;
    if(excluded==0u) port=(GmlRenderClassicPhaseTexturePort)99;
    if(excluded==1u) r->atlas[0].runtime_font_page=1;
    if(excluded==2u) r->pending_fill=1;
    if(excluded==3u) r->gui_pass_active=1;
    if(excluded==4u) r->color_write_mask=7u;
    if(excluded==5u) alpha=NAN;
    if(excluded==6u) page.project_authored_edges=1;
    if(excluded==7u) port=GML_RENDER_PHASE_TILE;
    if(excluded==8u){port=GML_RENDER_PHASE_ROOM_TILE;alpha=.61;}
    if(excluded==9u){port=GML_RENDER_PHASE_ROOM_TILE;xscale=2.0;}
    if(excluded==10u){port=GML_RENDER_PHASE_ROOM_TILE;yscale=-1.0;}
    if(excluded==11u){port=GML_RENDER_PHASE_ROOM_TILE;page.project_authored_edges=1;}
    memcpy(saved,r,sizeof *r);
    CHECK(!gml_render_resident_classic_phase_atlas(r,&page,1,1,xscale,yscale,0xFFFFFFu,alpha,port) &&
      !memcmp(r,saved,sizeof *r) && !fake.uploads && !fake.submits && !fake.readbacks);
    free(saved);r->atlas[0].runtime_font_page=r->pending_fill=r->gui_pass_active=0;
    r->color_write_mask=15u;
    gml_render_abandon_device(r);gml_render_free(r);free(r);
  }
  return 1;
}

static int paired_root_public_axis_atlas_ports(void){
  for(unsigned port=0;port<10u;port++){
    uint32_t base[16],vertical[16];uint8_t data[64]={0};
    for(unsigned p=0;p<16u;p++) base[p]=vertical[p]=0xFF315779u;
    FakeDevice fake={.on_demand=1};GmlWin content={.classic_version=810};
    GmlRender *r=paired_root_atlas_fixture(&fake,&content,base,vertical,4);CHECK(r);
    r->bg=calloc(1,sizeof *r->bg);r->tpag=calloc(1,sizeof *r->tpag);
    CHECK(r->bg && r->tpag);r->n_bg=r->n_tpag=1;
    r->tpag[0]=(GmlTpag){.sw=2,.sh=2,.tw=2,.th=2,.bw=2,.bh=2};
    if(port==0u) CHECK(gml_render_resident_font_glyph(r,0,0,0,2,2,1,1,1,1,0x73A5D9u,.61));
    if(port==1u) CHECK(gml_render_resident_sprite_font_glyph(r,0,1,1,1,1,0x73A5D9u,.61));
    if(port==2u) gml_draw_background_ext(r,0,1,1,1,1,0x73A5D9u,.61);
    if(port==3u) gml_draw_background_part_ext(r,0,0,0,2,2,1,1,1,1,0x73A5D9u,.61);
    if(port==4u) gml_draw_background_tile(r,0,0,0,2,2,1,1,1,1,0,0,0,0x73A5D9u,.61);
    if(port==5u) gml_draw_tile(r,0,0,0,2,2,1,1);
    if(port==6u){
      data[4]=1;data[8]=16;data[16]=data[20]=1;data[36]=data[40]=2;
      content.data=data;content.size=sizeof data;gml_draw_room_tiles(r,4);
    }
    if(port==7u) gml_draw_background_tiled_ext(r,0,1,1,1,1,0x73A5D9u,.61,1,1);
    if(port==8u){
      data[4]=1;data[8]=16;data[16]=data[28]=data[32]=1;
      content.data=data;content.size=sizeof data;gml_draw_room_backgrounds(r,4,0);
    }
    if(port==9u) gml_draw_background(r,0,1,1);
    CHECK(r->resident_application_pass && !r->classic_phase_scene_preparing &&
      !r->classic_phase_scene_target && !fake.readbacks &&
      gml_render_application_scene_flush(r) && fake.submits==2 && !fake.readbacks &&
      gml_render_resource_image_ref(r->base_scene_resource).content_version>1u &&
      gml_render_resource_image_ref(r->classic_phase_resource).content_version>1u);
    for(unsigned p=0;p<16u;p++) CHECK(base[p]==0xFF315779u && vertical[p]==0xFF315779u);
    gml_render_abandon_device(r);gml_render_free(r);free(r);
  }
  return 1;
}

static int paired_root_composition_keeps_both_sources_resident(void){
  uint32_t base[16]={0},vertical[16]={0},screen[64]={0};
  FakeDevice fake={.on_demand=1};GmlWin content={.classic_version=810};
  GmlRender *r=paired_root_atlas_fixture(&fake,&content,base,vertical,4);CHECK(r);
  gml_render_set_pending_fill(r,0x7F315779u);
  CHECK(gml_render_application_scene_flush(r) && fake.submits==2);
  GmlRenderResource *root=r->base_scene_resource,*peer=r->classic_phase_resource;
  GmlRenderImageRef root_image=gml_render_resource_image_ref(root),peer_image=gml_render_resource_image_ref(peer);
  r->scene_device_enabled=1;gml_render_application_surface_bind(r,base,4,4,0);
  CHECK(gml_render_begin_resident_base(r,screen,8,8,0,0) &&
    r->retained_base_resource==root && r->classic_phase_resource==peer && !fake.readbacks);
  CHECK(gml_render_sample_storage_prepare(r,vertical,4,4) &&
    r->classic_phase_resource==peer && !fake.readbacks);
  GmlRenderSamplePlanes planes={.application_vertical=vertical};
  gml_render_sample_planes_update(r,&planes,GML_RENDER_SAMPLE_PLANES_APPLICATION);
  r->app_surface_published=1;r->blendmode=6;r->color_write_mask=0;
  gml_draw_surface_stretched(r,0,0,0,8,8,0xFFFFFFu,1);
  CHECK(r->resident_application_pass && fake.submits==3 && fake.last_batch_operations==16u &&
    fake.last_write_mask==15u && !fake.readbacks && r->color_write_mask==0u && r->blendmode==6 &&
    gml_render_resource_image_ref(root).content_version==root_image.content_version &&
    gml_render_resource_image_ref(peer).content_version==peer_image.content_version);
  for(unsigned p=0;p<16u;p++) CHECK(!base[p] && !vertical[p]);
  for(unsigned p=0;p<64u;p++) CHECK(!screen[p]);
  CHECK(gml_surface_pixels_read(r,0,NULL,NULL)==base && fake.readbacks==2);
  CHECK(gml_render_begin_resident_base(r,base,4,4,0,0) && r->base_scene_resource==root &&
    r->retained_base_pixels==screen && !r->classic_phase_y);
  r->classic_phase_y=vertical;
  CHECK(gml_render_classic_phase_resource_bind(r) && r->classic_phase_resource==peer);
  gml_render_abandon_device(r);gml_render_free(r);free(r);return 1;
}

static int paired_root_composition_refuses_unsupported_state(void){
  for(unsigned excluded=0;excluded<10u;excluded++){
    uint32_t base[16]={0},vertical[16]={0},screen[64]={0};
    FakeDevice fake={.on_demand=1};GmlWin content={.classic_version=810};
    GmlRender *r=paired_root_atlas_fixture(&fake,&content,base,vertical,4);CHECK(r);
    r->scene_device_enabled=1;gml_render_application_surface_bind(r,base,4,4,0);
    CHECK(gml_render_begin_resident_base(r,screen,8,8,0,0));
    r->app_phase_y=vertical;
    double x=0.0,width=8.0,alpha=1.0;uint32_t colour=0xFFFFFFu;
    if(excluded==0u) r->interp=1;
    if(excluded==1u) r->app_phase_y=NULL;
    if(excluded==2u) alpha=.5;
    if(excluded==3u) width=7.0;
    if(excluded==4u) r->active_shader=0;
    if(excluded==5u) r->pending_underlay=1;
    if(excluded==6u) r->alpha_test_enable=1;
    if(excluded==7u) colour=0x73A5D9u;
    if(excluded==8u) x=NAN;
    if(excluded==9u) r->blendmode=1;
    CHECK(!gml_render_resident_classic_phase_present(r,x,0,width,8,colour,alpha) &&
      !fake.submits && !fake.uploads && !fake.readbacks && !r->classic_phase_composition);
    r->active_shader=-1;r->pending_underlay=0;
    gml_render_abandon_device(r);gml_render_free(r);free(r);
  }
  return 1;
}

static int paired_root_primitives_change_only_the_base(void){
  for(unsigned colour=0;colour<2u;colour++) for(unsigned outline=0;outline<2u;outline++){
    uint32_t base[16]={0},vertical[16]={0};
    FakeDevice fake={.on_demand=1};GmlWin content={.classic_version=810};
    GmlRender *r=paired_root_atlas_fixture(&fake,&content,base,vertical,4);CHECK(r);
    r->alpha=.61;
    if(colour) gml_render_primitive_rectangle_color(r,1,1,3,3,
      0x73A5D9u,0x359B17u,0xA17359u,0x193B57u,(int)outline);
    else gml_render_primitive_rectangle(r,1,1,3,3,0x73A5D9u,(int)outline);
    CHECK(r->resident_application_pass && gml_render_application_scene_flush(r) &&
      fake.submits==1 && !fake.readbacks &&
      gml_render_resource_image_ref(r->base_scene_resource).content_version>1u &&
      gml_render_resource_image_ref(r->classic_phase_resource).content_version==1u);
    for(unsigned p=0;p<16u;p++) CHECK(!base[p] && !vertical[p]);
    gml_render_abandon_device(r);gml_render_free(r);free(r);
  }
  return 1;
}

static int paired_root_full_rectangle_clears_both_samples(void){
  for(unsigned gui=0;gui<2u;gui++)
  for(unsigned disabled=0;disabled<2u;disabled++) for(unsigned port=0;port<2u;port++){
    uint32_t base[16]={0},vertical[16]={0};
    FakeDevice fake={.on_demand=1};GmlWin content={.classic_version=810};
    GmlRender *r=paired_root_atlas_fixture(&fake,&content,base,vertical,4);CHECK(r);
    if(gui){gml_render_gui_begin(r,4,4);gml_render_gui_set_size(r,2,2);}
    r->alpha=disabled?.61:1.0;r->alphablend=!disabled;
    if(port) gml_render_primitive_rectangle_color(r,5,5,-1,-1,
      0x73A5D9u,0x73A5D9u,0x73A5D9u,0x73A5D9u,0);
    else gml_render_primitive_rectangle(r,-1,-1,5,5,0x73A5D9u,0);
    CHECK(r->resident_application_pass && gml_render_application_scene_flush(r) &&
      fake.submits==2 && !fake.readbacks &&
      gml_render_resource_image_ref(r->base_scene_resource).content_version==2u &&
      gml_render_resource_image_ref(r->classic_phase_resource).content_version==2u);
    for(unsigned p=0;p<16u;p++) CHECK(!base[p] && !vertical[p]);
    gml_render_abandon_device(r);gml_render_free(r);free(r);
  }
  return 1;
}

static int paired_root_physical_clear_keeps_sources_independent(void){
  for(unsigned alpha=0;alpha<3u;alpha++){
    uint32_t base[16]={0},vertical[16]={0},screen[64]={0};
    FakeDevice fake={.on_demand=1};GmlWin content={.classic_version=810};
    GmlRender *r=paired_root_atlas_fixture(&fake,&content,base,vertical,4);CHECK(r);
    gml_render_set_pending_fill(r,0x7F315779u);
    CHECK(gml_render_application_scene_flush(r) && fake.submits==2);
    GmlRenderResource *root=r->base_scene_resource,*peer=r->classic_phase_resource;
    uint64_t root_version=gml_render_resource_image_ref(root).content_version;
    uint64_t peer_version=gml_render_resource_image_ref(peer).content_version;
    r->scene_device_enabled=1;gml_render_application_surface_bind(r,base,4,4,0);
    CHECK(gml_render_begin_resident_base(r,screen,8,8,0,0) && !r->classic_phase_y);
    GmlRenderSamplePlanes planes={.application_vertical=vertical};
    gml_render_sample_planes_update(r,&planes,GML_RENDER_SAMPLE_PLANES_APPLICATION);
    gml_render_clear(r,0x73A5D9u,alpha*.5);
    gml_render_flush_pending_fill(r);
    CHECK(r->resident_application_pass && gml_render_application_scene_flush(r) &&
      fake.submits==3 && !fake.readbacks && r->classic_phase_resource==peer &&
      gml_render_resource_image_ref(root).content_version==root_version &&
      gml_render_resource_image_ref(peer).content_version==peer_version);
    for(unsigned p=0;p<16u;p++) CHECK(!base[p] && !vertical[p]);
    for(unsigned p=0;p<64u;p++) CHECK(!screen[p]);
    gml_render_abandon_device(r);gml_render_free(r);free(r);
  }
  return 1;
}

static int paired_root_storage_retirement_is_transactional(void){
  uint32_t base[16]={0},vertical[16]={0},replacement[16]={0};
  FakeDevice fake={.on_demand=1,.readback_colour=0x7F315779u};
  GmlWin content={.classic_version=810};
  GmlRender *r=paired_root_atlas_fixture(&fake,&content,base,vertical,4);CHECK(r);
  GmlRenderResource *peer=r->classic_phase_resource;
  gml_render_set_pending_fill(r,0x7F315779u);
  CHECK(gml_render_sample_storage_prepare(r,vertical,4,4) &&
    r->classic_phase_resource==peer && !fake.submits && !fake.readbacks && !fake.retires);
  r->app_phase_y=vertical;fake.fail_readback=1;
  CHECK(!gml_render_sample_storage_prepare(r,replacement,4,4) &&
    r->classic_phase_resource==peer && r->classic_phase_y==vertical &&
    r->app_phase_y==vertical && fake.submits==2 && fake.readbacks==1 && !fake.retires);
  for(unsigned p=0;p<16u;p++) CHECK(!vertical[p]);
  fake.fail_readback=0;
  CHECK(gml_render_sample_storage_prepare(r,replacement,4,4) &&
    !r->classic_phase_resource && !r->classic_phase_y && !r->app_phase_y &&
    fake.readbacks==2 && fake.retires==1);
  for(unsigned p=0;p<16u;p++) CHECK(vertical[p]==0x7F315779u && !replacement[p]);
  CHECK(gml_render_sample_storage_prepare(r,NULL,0,0) && fake.readbacks==2 && fake.retires==1);
  GmlRenderSamplePlanes planes={.classic_vertical=replacement};
  gml_render_sample_planes_update(r,&planes,GML_RENDER_SAMPLE_PLANES_CLASSIC);
  CHECK(gml_render_sample_planes_resident_bind(r) && r->classic_phase_resource &&
    r->classic_phase_resource_pixels==replacement && fake.readbacks==2);
  CHECK(gml_render_sample_storage_prepare(r,replacement,4,3) &&
    !r->classic_phase_resource && fake.readbacks==2 && fake.retires==2);
  gml_render_abandon_device(r);gml_render_free(r);free(r);return 1;
}

static int paired_root_second_plane_refusal_keeps_only_undelivered_work(void){
  for(unsigned order=0;order<2u;order++){
    uint32_t base[16]={0},vertical[16]={0},pixels[4]={0xFF315779u};
    FakeDevice fake={.on_demand=1,.prewrite_refusal=1,.fail_submit_at=2};
    GmlRender *r=paired_root_fixture(&fake,base,vertical);
    GmlRenderResourceSource source=source_view(pixels,1),second=source;
    second.image.identity++;
    CHECK(r && gml_render_application_scene_prepare_begin(r));
    CHECK(gml_render_classic_phase_scene_select(r,(int)order));
    CHECK(paired_root_append(r,&source));
    CHECK(gml_render_classic_phase_scene_select(r,(int)(1u-order)));
    CHECK(paired_root_append(r,&second) && gml_render_application_scene_prepare_finish(r,1));
    CHECK(!gml_render_application_scene_flush(r) && fake.submits==2 && !fake.readbacks);
    GmlRenderResource *delivered=order?r->classic_phase_resource:r->base_scene_resource;
    GmlRenderResource *waiting=order?r->base_scene_resource:r->classic_phase_resource;
    CHECK(gml_render_resource_image_ref(delivered).content_version==2u &&
      gml_render_resource_image_ref(waiting).content_version==1u);
    CHECK(gml_render_application_scene_next_image_ref(r,delivered).content_version==2u &&
      gml_render_application_scene_next_image_ref(r,waiting).content_version==2u);
    fake.fail_submit_at=0;
    CHECK(gml_render_application_scene_flush(r) && fake.submits==3 && !fake.readbacks &&
      fake.submitted_targets[0].incarnation==gml_render_resource_image_ref(delivered).incarnation &&
      fake.submitted_targets[2].incarnation==gml_render_resource_image_ref(waiting).incarnation &&
      fake.submitted_counts[2]==1u && gml_render_resource_image_ref(delivered).content_version==2u &&
      gml_render_resource_image_ref(waiting).content_version==2u);
    gml_render_abandon_device(r);gml_render_free(r);free(r);
  }
  return 1;
}

static int paired_root_binding_and_loss_boundaries(void){
  for(unsigned excluded=0;excluded<10u;excluded++){
    uint32_t base[16]={0},vertical[16]={0},replacement[16]={0},pixels[4]={0xFF315779u};
    FakeDevice fake={.on_demand=1};GmlRender *r=paired_root_fixture(&fake,base,vertical);
    CHECK(r);
    gml_render_classic_phase_resource_dispose(r);
    if(excluded==0u) r->classic=0;
    if(excluded==1u) r->interp=1;
    if(excluded==2u) r->classic_phase_y=NULL;
    if(excluded==3u) fake.on_demand=0;
    if(excluded==4u) r->classic_interp_phase[1]=replacement;
    if(excluded==5u) r->target_id=0;
    if(excluded==6u) r->target_sp=1;
    if(excluded==7u) r->fb=replacement;
    if(excluded==8u) r->classic_phase_y=base;
    if(excluded==9u) r->classic_phase_y=base+1;
    CHECK(!gml_render_classic_phase_resource_bind(r) && !r->classic_phase_resource &&
      !fake.submits && !fake.readbacks && !fake.uploads);
    r->classic=1;r->interp=0;r->classic_phase_y=vertical;fake.on_demand=1;
    r->classic_interp_phase[1]=NULL;r->target_id=-1;r->target_sp=0;r->fb=base;
    CHECK(gml_render_classic_phase_resource_bind(r));
    GmlRenderImageRef phase=gml_render_resource_image_ref(r->classic_phase_resource);
    r->classic_phase_y=replacement;
    CHECK(!gml_render_classic_phase_resource_bind(r) &&
      gml_render_resource_image_ref(r->classic_phase_resource).incarnation==phase.incarnation);
    r->classic_phase_y=vertical;
    GmlRenderResourceSource source=source_view(pixels,1);
    CHECK(gml_render_application_scene_prepare_begin(r) && paired_root_append(r,&source) &&
      gml_render_classic_phase_scene_select(r,1) && paired_root_append(r,&source) &&
      gml_render_application_scene_prepare_finish(r,1));
    gml_render_lose_volatile_device(r);
    CHECK(!r->classic_phase_resource && !r->base_scene_resource && !r->pending_scene_batch &&
      !r->resident_application_pass && !fake.submits && !fake.readbacks &&
      vertical[0]==0u && base[0]==0u);
    gml_render_free(r);free(r);
  }
  return 1;
}

static int paired_root_cpu_boundary_and_unfinished_teardown(void){
  for(unsigned variant=0;variant<4u;variant++){
    uint32_t base[16]={0},vertical[16]={0},pixels[4]={0xFF315779u};
    FakeDevice fake={.on_demand=1,.readback_colour=0x80315779u};
    GmlRender *r=paired_root_fixture(&fake,base,vertical);
    GmlRenderResourceSource source=source_view(pixels,1);
    CHECK(r && gml_render_application_scene_prepare_begin(r) && paired_root_append(r,&source) &&
      gml_render_classic_phase_scene_select(r,1) && paired_root_append(r,&source));
    if(variant==3u){
      /* Teardown owns a preparation even when its selected peer is current. */
      gml_render_classic_phase_resource_dispose(r);
      CHECK(!r->classic_phase_scene_target && !r->classic_phase_resource &&
        !fake.submits && !fake.readbacks);
    }else{
      CHECK(gml_render_application_scene_prepare_finish(r,1));
      if(variant==1u){
        CHECK(gml_render_application_scene_flush(r));
        fake.fail_readback=1;
        CHECK(!gml_render_resident_application_cpu_barrier(r) && r->resident_application_pass &&
          base[0]==0u && vertical[0]==0u);
      }else if(variant==2u){
        CHECK(gml_render_application_scene_flush(r));
        gml_render_bind_device(r,NULL);
        CHECK(!gml_render_device(r) && fake.readbacks==2 &&
          vertical[0]==0x80315779u && base[0]==0x80315779u);
      }else{
        CHECK(gml_render_resident_application_cpu_barrier(r) && !r->resident_application_pass &&
          fake.submits==2 && fake.readbacks==2 && vertical[0]==0x80315779u && base[0]==0x80315779u);
        GmlRenderJournalMetrics metrics={0};
        gml_render_resource_journal_metrics(r->classic_phase_resource,&metrics);
        CHECK(metrics.authority==GML_RENDER_RESOURCE_CPU_CURRENT &&
          metrics.logical_version==metrics.cpu_version && !metrics.cpu_replays);
      }
    }
    gml_render_abandon_device(r);gml_render_free(r);free(r);
  }
  return 1;
}

typedef struct PhaseSpanProbe {
  int calls,accept;
  const GmlTpag *source;
} PhaseSpanProbe;

static int phase_span_probe(void *context,GmlTpag *page,double x,double y,double xs,double ys){
  PhaseSpanProbe *probe=context;
  probe->calls++;
  if(!page || page->sw!=probe->source->sw || page->sh<=0 ||
     page->sy<probe->source->sy || page->sy+page->sh>probe->source->sy+probe->source->sh ||
     !isfinite(x) || !isfinite(y) || !isfinite(xs) || !isfinite(ys)) return 0;
  return probe->accept;
}

static int classic_phase_geometry_is_pure_and_refusable(void){
  GmlRender *r=calloc(1,sizeof *r),*saved=calloc(1,sizeof *saved);
  uint32_t base[4*208],vertical[4*208];
  GmlTpag page={.sx=2,.sy=4,.sw=3,.sh=3,.tw=3,.th=3,.tx=1,.ty=1,.bw=5,.bh=6};
  GmlTpag original=page;
  CHECK(r && saved);
  for(unsigned p=0;p<4u*208u;p++) base[p]=vertical[p]=0x80436587u;
  r->fb=r->base_fb=base;r->classic_phase_y=vertical;r->fbw=r->base_fbw=4;
  r->fb_opaque_known=1;r->fb_all_transparent=1;
  for(unsigned shape=0;shape<2u;shape++) for(int mode=0;mode<5;mode++)
  for(int accept=0;accept<2;accept++) for(unsigned scale=0;scale<3u;scale++){
    r->fbh=r->base_fbh=shape?208:48;r->cam_y=scale==2u?.75:0.0;
    memcpy(saved,r,sizeof *r);PhaseSpanProbe probe={.accept=accept,.source=&page};
    CHECK(gml_render_classic_phase_spans(r,&page,7.25,9,1.5,
      scale==1u?.75:scale==2u?-1.0:1.0,mode,phase_span_probe,&probe)==accept);
    CHECK(probe.calls>0 && (accept || probe.calls==1) && !memcmp(r,saved,sizeof *r) &&
      !memcmp(&page,&original,sizeof page));
  }
  PhaseSpanProbe probe={.accept=1,.source=&page};
  CHECK(!gml_render_classic_phase_spans(r,&page,0,0,1,1,5,phase_span_probe,&probe) && !probe.calls);
  CHECK(!gml_render_classic_phase_spans(r,&page,0,0,1,1,0,NULL,&probe));
  CHECK(gml_render_classic_phase_spans(r,NULL,0,0,1,1,0,phase_span_probe,&probe) && !probe.calls);
  for(unsigned p=0;p<4u*208u;p++) CHECK(base[p]==0x80436587u && vertical[p]==0x80436587u);
  free(saved);free(r);return 1;
}

static int paired_root_capacity_does_not_deliver_half_a_draw(void){
  uint32_t base[16]={0},vertical[16]={0},pixels[4]={0xFF315779u};
  FakeDevice fake={.on_demand=1};GmlRender *r=paired_root_fixture(&fake,base,vertical);
  GmlRenderResourceSource source=source_view(pixels,1);
  CHECK(r);
  for(unsigned prefix=0;prefix<2047u;prefix++) CHECK(paired_root_append(r,&source));
  CHECK(gml_render_application_scene_prepare_begin(r) && paired_root_append(r,&source) &&
    gml_render_classic_phase_scene_select(r,1));
  CHECK(!paired_root_append(r,&source) && !fake.uploads && !fake.submits && !fake.readbacks);
  CHECK(!gml_render_application_scene_prepare_finish(r,0));
  CHECK(gml_render_application_scene_next_image_ref(r,r->base_scene_resource).content_version==2048u &&
    gml_render_application_scene_next_image_ref(r,r->classic_phase_resource).content_version==1u);
  CHECK(gml_render_application_scene_flush(r) && fake.submits==1 && fake.last_batch_operations==2047u);
  CHECK(gml_render_application_scene_prepare_begin(r) && paired_root_append(r,&source) &&
    gml_render_classic_phase_scene_select(r,1) && paired_root_append(r,&source) &&
    gml_render_application_scene_prepare_finish(r,1));
  CHECK(gml_render_application_scene_flush(r) && fake.submits==3 && !fake.readbacks &&
    gml_render_resource_image_ref(r->base_scene_resource).content_version==2049u &&
    gml_render_resource_image_ref(r->classic_phase_resource).content_version==2u);
  gml_render_abandon_device(r);gml_render_free(r);free(r);return 1;
}

static int paired_root_preparation_keeps_each_image_budget(void){
  for(unsigned disposition=0;disposition<3u;disposition++){
    uint32_t base[16]={0},vertical[16]={0},pixels[4]={0xFF315779u};
    FakeDevice fake={.on_demand=1};GmlRender *r=paired_root_fixture(&fake,base,vertical);
    GmlRenderResourceSource source=source_view(pixels,1);
    CHECK(r);
    if(disposition==2u) CHECK(paired_root_append(r,&source));
    CHECK(gml_render_application_scene_prepare_begin(r));
    r->classic_phase_scene_preparing=1;
    for(unsigned i=disposition==2u?1u:0u;i<2048u;i++) CHECK(paired_root_append(r,&source));
    CHECK(!paired_root_append(r,&source) && gml_render_classic_phase_scene_select(r,1));
    for(unsigned i=0;i<2048u;i++) CHECK(paired_root_append(r,&source));
    CHECK(!paired_root_append(r,&source) && !fake.uploads && !fake.submits && !fake.readbacks);
    if(disposition==2u){
      /* Dispose an expanded open transaction and its independently owned prefix. */
      gml_render_application_scene_queue_dispose(r);
      CHECK(!r->pending_scene_batch && !fake.submits && !fake.readbacks);
    }else if(!disposition){
      CHECK(!gml_render_application_scene_prepare_finish(r,0) && !r->pending_scene_batch);
      CHECK(gml_render_application_scene_next_image_ref(r,r->base_scene_resource).content_version==1u &&
        gml_render_application_scene_next_image_ref(r,r->classic_phase_resource).content_version==1u);
    }else{
      CHECK(gml_render_application_scene_prepare_finish(r,1));
      CHECK(gml_render_application_scene_prepare_begin(r));
      r->classic_phase_scene_preparing=1;
      CHECK(!paired_root_append(r,&source) && !gml_render_application_scene_prepare_finish(r,0));
      CHECK(gml_render_application_scene_next_image_ref(r,r->base_scene_resource).content_version==2049u &&
        gml_render_application_scene_next_image_ref(r,r->classic_phase_resource).content_version==2049u);
      CHECK(gml_render_application_scene_flush(r) && fake.submits==2 &&
        fake.last_batch_operations==2048u && !fake.readbacks &&
        gml_render_resource_image_ref(r->base_scene_resource).content_version==2049u &&
        gml_render_resource_image_ref(r->classic_phase_resource).content_version==2049u);
    }
    gml_render_abandon_device(r);gml_render_free(r);free(r);
  }
  return 1;
}

static int queued_immutable_checkpoint_spans_sources_and_states(void){
  GmlRender *render=(GmlRender*)calloc(1,sizeof *render);
  uint32_t first_pixels[4]={0xFF315779u,0xFF315779u,0xFF315779u,0xFF315779u};
  uint32_t second_pixels[4]={0xFF7193B5u,0xFF7193B5u,0xFF7193B5u,0xFF7193B5u};
  GmlRenderResourceSource first=source_view(first_pixels,1);
  GmlRenderResourceSource second=source_view(second_pixels,1);
  GmlRenderDrawStateSnapshot first_state=quad_state(0),second_state=quad_state(0);
  GmlRenderDrawOp operation=quad_op(2u,&first.image);
  GmlRenderJournalMetrics metrics={0};
  GmlRenderDispatchMetrics dispatch={0};
  FakeDevice fake={0};
  GmlRenderDeviceServices device=service(&fake);
  GmlRenderResource *resource;
  CHECK(render);
  second.image.identity=3u;
  second_state.samplers[0].filter=GML_RENDER_FILTER_BILINEAR;
  second_state.samplers[0].source_region=(GmlRenderRect){1,0,1u,2u};
  CHECK(gml_render_application_surface_ensure_owned(render,4,4));
  gml_render_bind_device(render,&device);
  CHECK(gml_render_begin_resident_owned_application(render,0.0,0.0,0));
  gml_render_dispatch_diagnostics_enable(render,1);
  resource=gml_render_application_resource_ensure(render);
  CHECK(resource);
  operation.reconstruction_mode=GML_RENDER_DEVICE_CHECKPOINT;
  operation.scene_family=GML_RENDER_SCENE_TEXT;
  operation.target.image=gml_render_resource_image_ref(resource);
  operation.target.image.content_version++;
  operation.target.pixel_format=GML_RENDER_PIXEL_ARGB8888;
  CHECK(gml_render_application_scene_enqueue(render,&operation,&first_state,&first));
  operation.target.image.content_version++;
  operation.sources=&second.image;
  operation.payload.quad.sampling.filter=GML_RENDER_FILTER_BILINEAR;
  operation.payload.quad.sampling.source_region=second_state.samplers[0].source_region;
  CHECK(gml_render_application_scene_enqueue(render,&operation,&second_state,&second));
  operation.target.image.content_version++;
  operation.sources=&first.image;
  operation.payload.quad.sampling.filter=GML_RENDER_FILTER_NEAREST;
  operation.payload.quad.sampling.source_region=first_state.samplers[0].source_region;
  CHECK(gml_render_application_scene_enqueue(render,&operation,&first_state,&first));
  CHECK(gml_render_dispatch_metrics(render,&dispatch) &&
        !dispatch.gpu[GML_RENDER_SCENE_TEXT] && !fake.submits && !fake.readbacks);
  CHECK(gml_render_scene_flush(render) && fake.submits==1 && fake.readbacks==1 &&
        fake.last_batch_operations==3u && fake.last_batch_states==2u &&
        fake.last_batch_source_ids[0]==2u && fake.last_batch_source_ids[1]==3u &&
        fake.last_batch_source_ids[2]==2u &&
        fake.last_batch_state_indices[0]==0u &&
        fake.last_batch_state_indices[1]==1u &&
        fake.last_batch_state_indices[2]==0u);
  CHECK(gml_render_dispatch_metrics(render,&dispatch) &&
        dispatch.gpu[GML_RENDER_SCENE_TEXT]==3u && !dispatch.failed);
  gml_render_resource_journal_metrics(resource,&metrics);
  CHECK(metrics.device_checkpoints==1u && metrics.journal_operations==0u &&
        metrics.authority==GML_RENDER_RESOURCE_BOTH_CURRENT);
  first.image.content_version++;
  second.image.content_version++;
  operation.target.image=gml_render_resource_image_ref(resource);
  operation.target.image.content_version++;
  operation.sources=&first.image;
  CHECK(gml_render_application_scene_enqueue(render,&operation,&first_state,&first));
  operation.target.image.content_version++;
  operation.sources=&second.image;
  operation.payload.quad.sampling.filter=GML_RENDER_FILTER_BILINEAR;
  operation.payload.quad.sampling.source_region=second_state.samplers[0].source_region;
  CHECK(gml_render_application_scene_enqueue(render,&operation,&second_state,&second));
  fake.fail_upload_at=fake.uploads+2;
  CHECK(!gml_render_scene_flush(render) && fake.submits==1 && fake.readbacks==1);
  CHECK(gml_render_dispatch_metrics(render,&dispatch) &&
        dispatch.gpu[GML_RENDER_SCENE_TEXT]==3u && dispatch.failed==2u);
  gml_render_resource_journal_metrics(resource,&metrics);
  CHECK(metrics.logical_version==4u &&
        metrics.authority==GML_RENDER_RESOURCE_BOTH_CURRENT);
  fake.fail_upload_at=0;
  CHECK(gml_render_scene_flush(render) && fake.submits==2 && fake.readbacks==2);
  CHECK(gml_render_dispatch_metrics(render,&dispatch) &&
        dispatch.gpu[GML_RENDER_SCENE_TEXT]==5u && dispatch.failed==2u && !dispatch.complete);
  gml_render_resource_journal_metrics(resource,&metrics);
  CHECK(metrics.logical_version==6u && metrics.device_checkpoints==2u &&
        metrics.authority==GML_RENDER_RESOURCE_BOTH_CURRENT);
  gml_render_free(render);
  free(render);
  return 1;
}

static int queued_filtered_font_spans(void){
  static const uint8_t authored[16]={255,0,0,0,0,255,0,64,
                                    0,0,255,128,255,255,255,255};
  for(unsigned mixed=0;mixed<2u;mixed++){
    GmlRender *render=(GmlRender*)calloc(1,sizeof *render);
    GmlRenderResourceSource sources[2]={source_view(NULL,1),source_view(NULL,1)};
    FakeDevice fake={0};
    GmlRenderDeviceServices device=service(&fake);
    GmlRenderJournalMetrics metrics={0};
    CHECK(render && gml_render_application_surface_ensure_owned(render,4,4));
    gml_render_bind_device(render,&device);
    CHECK(gml_render_begin_resident_owned_application(render,0.0,0.0,0));
    GmlRenderResource *resource=gml_render_application_resource_ensure(render);
    CHECK(resource);
    for(unsigned index=0;index<2u;index++){
      sources[index].image.resource_class=GML_RENDER_RESOURCE_ATLAS;
      sources[index].image.identity+=index;
      sources[index].pixel_format=GML_RENDER_PIXEL_RGBA8888;
      sources[index].bytes=authored;
    }
    for(unsigned index=0;index<(mixed?3u:2u);index++){
      unsigned selected=index&1u;
      GmlRenderDrawStateSnapshot state=quad_state(0);
      GmlRenderDrawOp operation=quad_op(2u,&sources[selected].image);
      GmlRenderQuad *quad=&operation.payload.quad;
      operation.target.image=gml_render_application_scene_next_image_ref(render,resource);
      operation.target.image.content_version++;
      operation.target.pixel_format=GML_RENDER_PIXEL_ARGB8888;
      operation.scene_family=GML_RENDER_SCENE_TEXT;
      if(mixed && index==1u) operation.reconstruction_mode=GML_RENDER_DEVICE_CHECKPOINT;
      else {
        state.rgb_source_factor=state.alpha_source_factor=GML_RENDER_BLEND_SOURCE_ALPHA;
        state.rgb_destination_factor=state.alpha_destination_factor=
          GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA;
        state.rgb_equation=state.alpha_equation=GML_RENDER_BLEND_EXACT_FILTERED_FONT;
        state.samplers[0].filter=GML_RENDER_FILTER_BILINEAR;
        state.samplers[0].quantisation=index?GML_RENDER_QUANTISE_FONT_MODERN:
                                             GML_RENDER_QUANTISE_FONT_FIRSTGEN;
        quad->sampling=state.samplers[0];
        quad->x[1]=quad->x[2]=quad->y[2]=quad->y[3]=2.0;
        quad->inverse_anchor[0]=quad->inverse_anchor[1]=1.0;
        quad->inverse_x[0]=quad->inverse_y[1]=0.5;
        quad->filtered_draw_alpha=0.7;
        for(unsigned corner=0;corner<4u;corner++) quad->corner_colour[corner]=0xFF40C080u;
      }
      CHECK(gml_render_application_scene_enqueue(render,&operation,&state,&sources[selected]));
      CHECK(fake.submits==0 && fake.readbacks==0);
    }
    CHECK(gml_render_scene_flush(render) && fake.submits==1 && fake.readbacks==(int)mixed &&
          fake.last_batch_operations==(mixed?3u:2u) &&
          fake.last_batch_states==(mixed?3u:2u) &&
          fake.last_batch_source_ids[0]==2u && fake.last_batch_source_ids[1]==3u &&
          (!mixed || fake.last_batch_source_ids[2]==2u));
    gml_render_resource_journal_metrics(resource,&metrics);
    CHECK(metrics.device_checkpoints==mixed &&
          metrics.journal_operations==(mixed?0u:2u) &&
          metrics.authority==(mixed?GML_RENDER_RESOURCE_BOTH_CURRENT:
                                   GML_RENDER_RESOURCE_GPU_REPLAYABLE));
    gml_render_abandon_device(render);
    fake.forbid_calls=1;
    CHECK(gml_render_resource_acquire_cpu_read(resource,NULL) &&
          fake.forbidden_calls==0 && fake.readbacks==(int)mixed);
    gml_render_free(render);
    free(render);
  }
  return 1;
}

static int rejection_and_isolation(void){
  uint32_t first_pixels[16],second_pixels[16];
  GmlRenderDrawOp operation=clear_op(2u,0xFF102030u,GML_RENDER_REPLAY_EXACT);
  GmlRenderDrawBatch batch={&operation,1u,NULL,0u};
  GmlRenderJournalMetrics first_metrics,second_metrics;
  FakeDevice fake={0};
  GmlRenderDeviceServices device=service(&fake);
  GmlRenderResource *first,*second;
  fill(first_pixels,0xFF000000u);
  fill(second_pixels,0xFF778899u);
  first=gml_render_resource_create(target_ref(1u,1u),4u,4u,
    GML_RENDER_PIXEL_XRGB8888,first_pixels,(GmlRenderResourceLimits){0});
  second=gml_render_resource_create(target_ref(1u,2u),4u,4u,
    GML_RENDER_PIXEL_XRGB8888,second_pixels,(GmlRenderResourceLimits){0});
  CHECK(first && second);
  {
    GmlRenderImageRef source_ref_value=source_ref();
    GmlRenderDrawOp bad_quad=quad_op(2u,&source_ref_value);
    GmlRenderDrawStateSnapshot state=quad_state(0);
    GmlRenderDrawBatch bad_batch={&bad_quad,1u,&state,1u};
    CHECK(gml_render_resource_submit(first,&device,&bad_batch,NULL,1u)==
          GML_RENDER_RESOURCE_EXEC_FAILED);
  }
  CHECK(gml_render_resource_submit(second,&device,&batch,NULL,0u)==
        GML_RENDER_RESOURCE_EXEC_FAILED); /* Same public ID, old incarnation. */
  CHECK(gml_render_resource_submit(first,&device,&batch,NULL,0u)==
        GML_RENDER_RESOURCE_EXEC_DEVICE);
  gml_render_resource_journal_metrics(first,&first_metrics);
  gml_render_resource_journal_metrics(second,&second_metrics);
  CHECK(first_metrics.logical_version==2u && second_metrics.logical_version==1u);
  CHECK(second_pixels[0]==0xFF778899u && fake.submits==1);
  operation.target.image=target_ref(2u,2u);
  operation.target.region.width=3u;
  CHECK(gml_render_resource_submit(second,&device,&batch,NULL,0u)==
        GML_RENDER_RESOURCE_EXEC_FAILED);
  operation.target.region.width=4u;
  CHECK(gml_render_resource_submit(second,&device,&batch,NULL,0u)==
        GML_RENDER_RESOURCE_EXEC_DEVICE);
  gml_render_resource_context_lost(first);
  gml_render_resource_context_lost(second);
  CHECK(gml_render_resource_acquire_cpu_read(first,NULL));
  CHECK(gml_render_resource_acquire_cpu_read(second,NULL));
  CHECK(first_pixels[0]==0xFF102030u && second_pixels[0]==0xFF102030u);
  gml_render_resource_destroy(first);
  gml_render_resource_destroy(second);
  return 1;
}

static int persistent_surface_lifecycle(void){
  GmlRender *render=(GmlRender*)calloc(1,sizeof *render);
  GmlRenderDeviceServices device;
  GmlRenderImageRef old_ref,new_ref;
  GmlRenderDrawOp operation;
  GmlRenderDrawBatch batch;
  GmlRenderJournalMetrics source_before={0},source_after={0};
  FakeDevice fake={0};
  const uint32_t *pixels;
  int first,second,disposable,reused,w,h;
  CHECK(render);
  render->next_surface_id=1;
  first=gml_surface_create(render,4,4);
  second=gml_surface_create(render,4,4);
  CHECK(first>0 && second>0 && first!=second);
  CHECK(!render->surface[first-1].resource && !render->surface[second-1].resource);
  device=service(&fake);
  gml_render_bind_device(render,&device);
  CHECK(gml_render_device(render));
  disposable=gml_surface_create(render,4,4);
  CHECK(disposable>0 && gml_render_surface_resource_ensure(render,disposable));
  gml_surface_free(render,disposable);
  CHECK(fake.retires==1);
  old_ref=gml_render_resource_image_ref(gml_render_surface_resource_ensure(render,first));
  CHECK(old_ref.incarnation!=0u && old_ref.content_version==1u);
  operation=clear_op(2u,0x40224466u,GML_RENDER_REPLAY_EXACT);
  operation.target.image=old_ref;
  operation.target.image.content_version=2u;
  operation.target.pixel_format=GML_RENDER_PIXEL_ARGB8888;
  batch=(GmlRenderDrawBatch){&operation,1u,NULL,0u};
  CHECK(gml_render_surface_scene_submit(render,first,&batch,NULL,0u)==
        GML_RENDER_RESOURCE_EXEC_DEVICE);
  CHECK(render->surface[first-1].px[0]==0u);
  gml_render_bind_device(render,NULL); /* Context is gone before a CPU observation. */
  pixels=gml_surface_pixels_read(render,first,&w,&h);
  CHECK(pixels && w==4 && h==4 && pixels[0]==0x40224466u);
  CHECK(fake.readbacks==0);
  gml_render_dispatch_diagnostics_enable(render,1);
  gml_render_resource_journal_metrics(render->surface[first-1].resource,&source_before);
  gml_surface_copy(render,second,0,0,first);
  gml_render_resource_journal_metrics(render->surface[first-1].resource,&source_after);
  CHECK(source_after.logical_version==source_before.logical_version &&
        source_after.cpu_version==source_before.cpu_version);
  pixels=gml_surface_pixels_read(render,second,&w,&h);
  CHECK(pixels && pixels[0]==0x40224466u && pixels[15]==0x40224466u);
  gml_surface_copy_part(render,second,1,1,first,0,0,2,2);
  gml_render_resource_journal_metrics(render->surface[first-1].resource,&source_after);
  CHECK(source_after.logical_version==source_before.logical_version &&
        source_after.cpu_version==source_before.cpu_version);
  pixels=gml_surface_pixels_read(render,second,&w,&h);
  CHECK(pixels && pixels[5]==0x40224466u && pixels[10]==0x40224466u);
  gml_surface_copy(render,second,4,4,first);
  gml_surface_copy(render,-1,0,0,first);
  gml_surface_copy_part(render,second,0,0,first,0,0,0,2);
  gml_surface_copy_part(render,second,0,0,first,4,4,2,2);
  GmlRenderDispatchMetrics dispatch={0};
  CHECK(gml_render_dispatch_metrics(render,&dispatch) &&
        dispatch.cpu[GML_RENDER_SCENE_SURFACE][GML_RENDER_DISPATCH_NO_DEVICE]==2u);
  gml_surface_copy(render,first,1,1,first);
  gml_surface_copy_part(render,second,1,1,second,0,0,2,2);
  CHECK(gml_render_dispatch_metrics(render,&dispatch) &&
        dispatch.cpu[GML_RENDER_SCENE_SURFACE][GML_RENDER_DISPATCH_NO_DEVICE]==4u &&
        !dispatch.complete && !fake.readbacks);
  gml_surface_free(render,first);
  render->next_surface_id=first;
  reused=gml_surface_create(render,4,4);
  CHECK(reused==first);
  new_ref=gml_render_resource_image_ref(gml_render_surface_resource_ensure(render,reused));
  CHECK(new_ref.identity==old_ref.identity && new_ref.incarnation!=old_ref.incarnation);
  CHECK(render->surface[reused-1].px[0]==0u);
  gml_surface_free(render,reused);
  gml_surface_free(render,second);
  free(render);
  return 1;
}

static int retained_target_stays_resident(void){
  GmlRender *render=(GmlRender*)calloc(1,sizeof *render);
  uint32_t first_base[16]={0},second_base[16]={0};
  GmlRenderDeviceServices device;
  GmlRenderDrawOp operation;
  GmlRenderDrawBatch batch;
  GmlRenderImageRef ref;
  GmlRenderResource *resource;
  GmlRenderResourceSource observed_source={0};
  GmlRenderJournalMetrics before={0},after={0};
  FakeDevice fake={0};
  const uint32_t *pixels;
  int id,w=0,h=0;
  CHECK(render);
  render->next_surface_id=1;
  gml_render_begin(render,first_base,4,4,0.0,0.0);
  id=gml_surface_create(render,4,4);
  CHECK(id>0 && gml_surface_set_target(render,id));
  device=service(&fake);
  gml_render_bind_device(render,&device);
  resource=gml_render_surface_resource_ensure(render,id);
  CHECK(resource);
  ref=gml_render_resource_image_ref(resource);
  operation=clear_op(2u,0x40112233u,GML_RENDER_REPLAY_EXACT);
  operation.target.image=ref;
  operation.target.image.content_version=2u;
  operation.target.pixel_format=GML_RENDER_PIXEL_ARGB8888;
  batch=(GmlRenderDrawBatch){&operation,1u,NULL,0u};
  CHECK(gml_render_surface_scene_submit(render,id,&batch,NULL,0u)==
        GML_RENDER_RESOURCE_EXEC_DEVICE);
  gml_render_resource_journal_metrics(resource,&before);
  CHECK(before.journal_operations==1u && before.cpu_replays==0u &&
        render->surface[id-1].px[0]==0u);
  gml_render_begin_retaining_resident_target(render,second_base,4,4,2.0,3.0);
  gml_render_resource_journal_metrics(resource,&after);
  CHECK(gml_surface_get_target(render)==id && render->fb==render->surface[id-1].px &&
        render->base_fb==second_base && render->fbw==4 && render->fbh==4);
  CHECK(after.journal_operations==before.journal_operations &&
        after.cpu_replays==before.cpu_replays &&
        after.logical_version==before.logical_version &&
        fake.readbacks==0 && render->surface[id-1].px[0]==0u);
  gml_surface_reset_target(render);
  CHECK(render->fb==second_base);
  pixels=gml_surface_pixels_read(render,id,&w,&h);
  CHECK(pixels && w==4 && h==4 && pixels[0]==0x40112233u &&
        pixels[15]==0x40112233u && fake.readbacks==0);
  gml_render_resource_journal_metrics(resource,&after);
  CHECK(after.logical_version==before.logical_version &&
        after.cpu_version==before.logical_version &&
        after.gpu_version==before.logical_version &&
        gml_surface_width(render,id)==4 && gml_surface_height(render,id)==4 &&
        gml_surface_exists(render,id));
  gml_render_resource_journal_metrics(resource,&after);
  CHECK(after.logical_version==before.logical_version &&
        gml_render_resource_source_view(resource,&observed_source) &&
        observed_source.image.content_version==before.logical_version &&
        observed_source.resident_resource==resource &&
        observed_source.bytes==pixels);
  CHECK(gml_surface_set_target(render,id));
  ref=gml_render_resource_image_ref(resource);
  operation=clear_op(ref.content_version+1u,0xFF445566u,GML_RENDER_REPLAY_EXACT);
  operation.target.image=ref;
  operation.target.image.content_version=ref.content_version+1u;
  operation.target.pixel_format=GML_RENDER_PIXEL_ARGB8888;
  CHECK(gml_render_surface_scene_submit(render,id,&batch,NULL,0u)==
        GML_RENDER_RESOURCE_EXEC_DEVICE);
  gml_render_resource_journal_metrics(resource,&before);
  CHECK(before.journal_operations==1u);
  gml_render_begin_retaining_target(render,first_base,4,4,4.0,5.0);
  gml_render_resource_journal_metrics(resource,&after);
  CHECK(after.journal_operations==0u && after.cpu_replays==before.cpu_replays+1u &&
        render->surface[id-1].px[0]==0xFF445566u && fake.readbacks==0);
  gml_render_free(render);
  free(render);
  return 1;
}

static int resident_numbered_target_nested_cpu_boundary(void){
  GmlRender *render=(GmlRender*)calloc(1,sizeof *render);
  FakeDevice fake={0};
  GmlRenderDeviceServices device=service(&fake);
  GmlRenderJournalMetrics outer_metrics={0},inner_metrics={0};
  const uint32_t *pixels;
  int outer,inner;
  CHECK(render && gml_render_init(render,NULL)==0 &&
        gml_render_application_surface_ensure_owned(render,4,4));
  render->next_surface_id=1;
  outer=gml_surface_create(render,4,4);
  inner=gml_surface_create(render,4,4);
  CHECK(outer>0 && inner>0);
  gml_render_bind_device(render,&device);
  CHECK(gml_render_begin_resident_owned_application(render,2.0,3.0,0));
  gml_render_set_pending_fill(render,0xFF123456u);
  CHECK(gml_surface_set_target(render,outer) &&
        render->resident_application_pass && render->cam_x==0.0 && render->cam_y==0.0);
  gml_render_set_pending_fill(render,0xFF315779u);
  gml_render_primitive_rectangle(render,1,1,2,2,0x75A4C3u,0);
  CHECK(fake.last_target_image.resource_class==GML_RENDER_RESOURCE_SURFACE &&
        fake.last_target_image.identity==(uint32_t)outer &&
        render->surface[outer-1].px[5]==0u);
  CHECK(gml_surface_set_target(render,inner) && render->resident_application_pass);
  gml_render_set_pending_fill(render,0xFF26486Au);
  gml_render_flush_pending_fill(render);
  CHECK(render->surface[inner-1].px[0]==0u && fake.readbacks==0);
  render->alpha=0.5;
  gml_render_primitive_line(render,0,0,3,3,0xFFFFFFu,1);
  CHECK(!render->resident_application_pass &&
        render->surface[inner-1].px[1]==0xFF26486Au &&
        render->surface[inner-1].px[0]!=0xFF26486Au && fake.readbacks==0);
  gml_render_resource_journal_metrics(render->surface[inner-1].resource,&inner_metrics);
  CHECK(inner_metrics.authority==GML_RENDER_RESOURCE_CPU_CURRENT &&
        inner_metrics.cpu_version==inner_metrics.logical_version);
  gml_surface_reset_target(render);
  CHECK(render->resident_application_pass && render->target_id==outer &&
        render->surface[outer-1].px[0]==0u);
  render->alpha=1.0;
  gml_render_primitive_rectangle(render,0,0,0,0,0xB3D571u,0);
  gml_render_resource_journal_metrics(render->surface[outer-1].resource,&outer_metrics);
  CHECK(outer_metrics.journal_operations==3u && outer_metrics.cpu_replays==0u &&
        fake.last_target_image.identity==(uint32_t)outer);
  gml_surface_reset_target(render);
  CHECK(render->resident_application_pass && render->target_id==-1 &&
        render->cam_x==2.0 && render->cam_y==3.0);
  pixels=gml_surface_pixels_read(render,outer,NULL,NULL);
  CHECK(pixels && pixels[0]==0xFF71D5B3u && pixels[1]==0xFF315779u &&
        pixels[5]==0xFFC3A475u && pixels[15]==0xFF315779u && fake.readbacks==0);
  CHECK(gml_surface_set_target(render,outer) && render->resident_application_pass &&
        fake.readbacks==0);
  gml_render_resource_journal_metrics(render->surface[outer-1].resource,&outer_metrics);
  CHECK(outer_metrics.journal_operations==0u && outer_metrics.cpu_replays==1u);
  gml_surface_reset_target(render);
  CHECK(gml_surface_set_target(render,0) && render->resident_application_pass);
  gml_render_set_pending_fill(render,0xFF426486u);
  gml_render_flush_pending_fill(render);
  CHECK(fake.last_target_image.resource_class==GML_RENDER_RESOURCE_APPLICATION);
  gml_surface_reset_target(render);
  CHECK(render->resident_application_pass && fake.readbacks==0);
  gml_render_bind_device(render,NULL);
  gml_render_free(render);
  free(render);
  return 1;
}

static int resident_numbered_target_queued_atlas_observation(void){
  const uint8_t rgba[16]={213,29,47,255, 37,191,59,255,
                          61,83,227,255, 197,151,73,255};
  GmlRender *render=(GmlRender*)calloc(1,sizeof *render);
  FakeDevice fake={0};
  GmlRenderDeviceServices device=service(&fake);
  GmlRenderJournalMetrics metrics={0};
  const uint32_t *pixels;
  int outer,inner;
  CHECK(render && gml_render_init(render,NULL)==0 &&
        gml_render_application_surface_ensure_owned(render,4,4));
  render->next_surface_id=1;
  int atlas_index=render->n_atlas;
  GmlAtlas *atlas_records=(GmlAtlas*)realloc(render->atlas,
    (size_t)(atlas_index+1)*sizeof *render->atlas);
  CHECK(atlas_records);
  render->atlas=atlas_records;
  memset(&render->atlas[atlas_index],0,sizeof *render->atlas);
  render->tpag=(GmlTpag*)calloc(1,sizeof *render->tpag);
  render->spr=(GmlSprite*)calloc(1,sizeof *render->spr);
  CHECK(render->atlas && render->tpag && render->spr);
  render->n_atlas=atlas_index+1;
  render->n_tpag=render->n_spr=1;
  render->atlas[atlas_index].px=(uint8_t*)malloc(sizeof rgba);
  render->spr[0].frame=(int*)calloc(1,sizeof(int));
  CHECK(render->atlas[atlas_index].px && render->spr[0].frame);
  memcpy(render->atlas[atlas_index].px,rgba,sizeof rgba);
  render->atlas[atlas_index].w=render->atlas[atlas_index].h=2;
  render->tpag[0].atlas=atlas_index;
  render->tpag[0].sw=render->tpag[0].sh=2;
  render->tpag[0].tw=render->tpag[0].th=2;
  render->tpag[0].bw=render->tpag[0].bh=2;
  render->spr[0].w=render->spr[0].h=2;
  render->spr[0].n_frames=1;
  outer=gml_surface_create(render,4,4);
  inner=gml_surface_create(render,4,4);
  CHECK(outer>0 && inner>0);
  gml_render_bind_device(render,&device);
  CHECK(gml_render_begin_resident_owned_application(render,0.0,0.0,0) &&
        gml_surface_set_target(render,outer));
  gml_render_set_pending_fill(render,0xFF102030u);
  gml_draw_sprite_ext(render,0,0,0,0,1,1,0,0xFFFFFFu,1);
  gml_draw_sprite_ext(render,0,0,1,1,1,1,0,0xFFFFFFu,1);
  CHECK(render->resident_application_pass && fake.submits==1);
  CHECK(gml_surface_set_target(render,inner) && fake.submits==2 &&
        fake.last_batch_operations==2u &&
        fake.last_target_image.identity==(uint32_t)outer &&
        render->surface[outer-1].px[0]==0u);
  gml_render_set_pending_fill(render,0x8031597Bu);
  gml_draw_sprite_ext(render,0,0,0,0,1,1,0,0xFFFFFFu,1);
  gml_draw_sprite_ext(render,0,0,2,2,1,1,0,0xFFFFFFu,1);
  CHECK(render->resident_application_pass && fake.submits==3);
  /* Public observation must submit the complete queued prefix before replaying it. */
  pixels=gml_surface_pixels_read(render,inner,NULL,NULL);
  CHECK(pixels && pixels[0]==0xFFD51D2Fu && pixels[5]==0xFFC59749u &&
        pixels[2]==0x8031597Bu && pixels[10]==0xFFD51D2Fu && fake.submits==4 &&
        fake.last_batch_operations==2u && fake.readbacks==0 &&
        fake.last_target_image.identity==(uint32_t)inner);
  gml_render_resource_journal_metrics(render->surface[inner-1].resource,&metrics);
  CHECK(metrics.cpu_version==metrics.logical_version && metrics.cpu_replays==1u);
  gml_surface_reset_target(render);
  CHECK(render->target_id==outer && render->resident_application_pass);
  gml_draw_sprite_ext(render,0,0,2,2,1,1,0,0xFFFFFFu,1);
  gml_surface_reset_target(render);
  CHECK(fake.submits==5 && fake.last_target_image.identity==(uint32_t)outer &&
        render->resident_application_pass && fake.readbacks==0);
  pixels=gml_surface_pixels_read(render,outer,NULL,NULL);
  CHECK(pixels && pixels[0]==0xFFD51D2Fu && pixels[5]==0xFFD51D2Fu &&
        pixels[10]==0xFFD51D2Fu && pixels[15]==0xFFC59749u);
  gml_render_bind_device(render,NULL);
  gml_render_free(render);
  free(render);
  return 1;
}

static int volatile_loss_discards_queued_nested_target(void){
  GmlRender *render=(GmlRender*)calloc(1,sizeof *render);
  uint32_t source_pixels[4]={0xFF315779u,0xFF315779u,0xFF315779u,0xFF315779u};
  GmlRenderResourceSource source=source_view(source_pixels,1);
  GmlRenderDrawOp operation=quad_op(2u,&source.image);
  GmlRenderDrawStateSnapshot state=quad_state(0);
  GmlRenderResource *resource;
  GmlRenderImageRef previous;
  FakeDevice fake={0};
  GmlRenderDeviceServices device=service(&fake);
  int backed,queued;
  CHECK(render && gml_render_init(render,NULL)==0 &&
        gml_render_application_surface_ensure_owned(render,4,4));
  render->next_surface_id=1;
  backed=gml_surface_create(render,4,4);
  queued=gml_surface_create(render,4,4);
  CHECK(backed>0 && queued>0);
  fill(render->surface[backed-1].px,0x71423324u);
  gml_render_application_surface_publish(render);
  gml_render_bind_device(render,&device);
  CHECK(gml_render_begin_resident_owned_application(render,0.0,0.0,0) &&
        gml_surface_set_target(render,backed) &&
        gml_surface_set_target(render,queued) && gml_surface_set_target(render,queued));
  resource=render->surface[queued-1].resource;
  CHECK(resource && render->target_sp==3);
  previous=gml_render_resource_image_ref(resource);
  gml_render_application_surface_bind(render,render->surface[queued-1].px,4,4,0);
  operation.target.image=previous;
  operation.target.image.content_version++;
  operation.target.pixel_format=GML_RENDER_PIXEL_ARGB8888;
  CHECK(gml_render_application_scene_enqueue(render,&operation,&state,&source) &&
        render->pending_scene_batch && fake.submits==0);
  /* Both the current clear and the suspended CPU-parent clear are delayed work. */
  render->pending_fill=render->target_stack[1].pending_fill=1;
  render->pending_fill_color=render->target_stack[1].fill_color=0xFFABCDEFu;
  fake.forbid_calls=1;
  gml_render_lose_volatile_device(render);
  CHECK(fake.forbidden_calls==0 && !gml_render_device(render) &&
        !render->pending_scene_batch && !render->pending_fill &&
        !gml_surface_exists(render,queued) && gml_surface_exists(render,backed) &&
        render->surface[backed-1].px[0]==0x71423324u &&
        render->target_id==backed && render->target_sp==1 &&
        render->app_surface==render->app_surface_owned && gml_surface_exists(render,0));
  render->next_surface_id=queued;
  CHECK(gml_surface_create(render,4,4)==queued);
  resource=gml_render_surface_resource_ensure(render,queued);
  CHECK(resource && gml_render_resource_image_ref(resource).incarnation!=previous.incarnation);
  gml_render_free(render);
  free(render);
  return 1;
}

static int volatile_loss_recreates_only_unobserved_application(void){
  for(unsigned observed=0u;observed<2u;observed++){
    GmlRender *render=(GmlRender*)calloc(1,sizeof *render);
    GmlRenderResource *resource;
    GmlRenderImageRef previous;
    GmlRenderDrawOp operation=clear_op(2u,0xFF556677u,GML_RENDER_REPLAY_EXACT);
    GmlRenderDrawBatch batch={&operation,1u,NULL,0u};
    GmlRenderJournalMetrics metrics={0};
    FakeDevice fake={0};
    GmlRenderDeviceServices device=service(&fake);
    CHECK(render && gml_render_init(render,NULL)==0 &&
          gml_render_application_surface_ensure_owned(render,4,4));
    fill(render->app_surface_owned,0x71423324u);
    gml_render_application_surface_publish(render);
    gml_render_bind_device(render,&device);
    CHECK(gml_render_begin_resident_owned_application(render,0.0,0.0,0));
    resource=gml_render_application_resource_ensure(render);
    CHECK(resource);
    previous=gml_render_resource_image_ref(resource);
    operation.target.image=previous;
    operation.target.image.content_version++;
    operation.target.pixel_format=GML_RENDER_PIXEL_ARGB8888;
    CHECK(gml_render_application_scene_submit(render,&batch,NULL,0u)==
          GML_RENDER_RESOURCE_EXEC_DEVICE);
    fake.readback_colour=0x50778899u;
    if(observed) CHECK(gml_render_observe_device_resources(render));
    int backed=gml_surface_create(render,4,4);
    CHECK(backed>0);
    render->fb_opaque_known=1;
    render->fb_all_opaque=!observed;
    render->fb_all_transparent=0;
    CHECK(gml_surface_set_target(render,backed) && render->target_sp==1);
    fake.forbid_calls=1;
    gml_render_lose_volatile_device(render);
    CHECK(fake.forbidden_calls==0 && !gml_render_device(render) &&
          !render->pending_scene_batch && !render->base_scene_resource &&
          render->app_surface==render->app_surface_owned && gml_surface_exists(render,0) &&
          !render->target_stack[0].opaque_known && !render->target_stack[0].all_opaque);
    for(int index=0;index<16;index++)
      CHECK(render->app_surface_owned[index]==(observed?fake.readback_colour:0u));
    resource=gml_render_application_resource_ensure(render);
    CHECK(resource &&
          (gml_render_resource_image_ref(resource).incarnation==previous.incarnation)==observed);
    gml_render_resource_journal_metrics(resource,&metrics);
    CHECK(metrics.authority==GML_RENDER_RESOURCE_CPU_CURRENT && !metrics.gpu_version &&
          metrics.cpu_version==metrics.logical_version && !metrics.journal_operations);
    gml_render_free(render);
    free(render);
  }
  return 1;
}

static int resident_numbered_target_rebind_retention_and_reuse(void){
  GmlRender *render=(GmlRender*)calloc(1,sizeof *render);
  FakeDevice fake={0};
  GmlRenderDeviceServices device=service(&fake);
  GmlRenderImageRef before,after;
  const uint32_t *pixels;
  int outer,inner;
  CHECK(render && gml_render_init(render,NULL)==0 &&
        gml_render_application_surface_ensure_owned(render,4,4));
  render->next_surface_id=1;
  outer=gml_surface_create(render,4,4);
  inner=gml_surface_create(render,4,4);
  CHECK(outer>0 && inner>0);
  gml_render_bind_device(render,&device);
  CHECK(gml_render_begin_resident_owned_application(render,0.0,0.0,0) &&
        gml_render_resident_application_cpu_barrier(render) &&
        !render->resident_application_pass && gml_surface_set_target(render,outer) &&
        render->resident_application_pass);
  gml_render_set_pending_fill(render,0x8037597Bu);
  gml_render_flush_pending_fill(render);
  before=gml_render_resource_image_ref(render->surface[outer-1].resource);
  gml_surface_reset_target(render);
  CHECK(!render->resident_application_pass && !render->surface[outer-1].all_transparent &&
        render->surface[outer-1].px[0]==0u && fake.readbacks==0);
  CHECK(gml_surface_set_target(render,outer) && render->resident_application_pass);
  after=gml_render_resource_image_ref(render->surface[outer-1].resource);
  CHECK(after.content_version==before.content_version &&
        render->surface[outer-1].px[0]==0u && fake.readbacks==0);
  CHECK(gml_surface_set_target(render,outer));
  gml_render_set_pending_fill(render,0u);
  gml_render_flush_pending_fill(render);
  gml_surface_reset_target(render);
  CHECK(render->fb_all_transparent && render->resident_application_pass);
  CHECK(gml_render_begin_resident_owned_application(render,2.0,3.0,1) &&
        render->target_id==outer && render->resident_application_pass &&
        fake.readbacks==0);
  gml_render_set_pending_fill(render,0xFF26486Au);
  gml_render_flush_pending_fill(render);
  CHECK(gml_surface_set_target(render,inner));
  gml_surface_free(render,outer);
  CHECK(!gml_surface_exists(render,outer) && render->target_sp==1 &&
        render->target_id==inner && fake.readbacks==0);
  render->next_surface_id=outer;
  CHECK(gml_surface_create(render,4,4)==outer);
  after=gml_render_resource_image_ref(gml_render_surface_resource_ensure(render,outer));
  CHECK(after.incarnation!=before.incarnation && after.content_version==1u);
  gml_surface_reset_target(render);
  CHECK(render->target_id==-1 && render->resident_application_pass &&
        render->cam_x==2.0 && render->cam_y==3.0);
  pixels=gml_surface_pixels_read(render,outer,NULL,NULL);
  CHECK(pixels && pixels[0]==0u && fake.readbacks==0);
  gml_render_bind_device(render,NULL);
  gml_render_free(render);
  free(render);
  return 1;
}

static int resident_empty_reset_preserves_base_owner(void){
  for(unsigned classic=0;classic<2u;classic++)
  for(unsigned target=0;target<3u;target++)
  for(unsigned gui=0;gui<2u;gui++)
  for(unsigned pending=0;pending<3u;pending++){
    GmlRender *render=calloc(1,sizeof *render);
    GmlWin content={.classic_version=classic?700:0,.bytecode=classic?0:15};
    uint32_t base[16]={0};
    FakeDevice fake={.on_demand=1};
    GmlRenderDeviceServices device=service(&fake);
    GmlRenderJournalMetrics before={0},after={0};
    GmlRenderDispatchMetrics dispatch={0};
    device.observe_on_demand=fake_observe_on_demand;
    CHECK(render && gml_render_init(render,NULL)==0);
    render->win=&content;render->classic=classic;
    gml_render_dispatch_diagnostics_enable(render,1);
    gml_render_bind_device(render,&device);
    if(target==2u){
      CHECK(gml_render_application_surface_ensure_owned(render,4,4) &&
            gml_render_begin_resident_owned_application(render,2.0,3.0,0));
    }else{
      if(target==1u) gml_render_application_surface_bind(render,base,4,4,0);
      CHECK(gml_render_begin_resident_base(render,base,4,4,2.0,3.0));
    }
    if(gui) gml_render_gui_begin(render,4,4);
    double camera_x=render->cam_x,camera_y=render->cam_y;
    gml_render_set_pending_fill(render,0x8037597Bu);
    gml_render_flush_pending_fill(render);
    GmlRenderResource *resource=gml_render_scene_target_resource_ensure(render);
    CHECK(resource && fake.submits==1 && !fake.readbacks);
    gml_render_resource_journal_metrics(resource,&before);
    if(pending==1u) gml_render_set_pending_fill(render,0xFF112233u);
    if(pending==2u){
      render->pending_underlay=1;
      render->underlay_x=1;render->underlay_y=1;
      render->underlay_w=2;render->underlay_h=2;
    }
    gml_surface_reset_target(render);
    CHECK(render->target_id==-1 && !render->target_sp &&
          render->fb==render->base_fb && render->fbw==4 && render->fbh==4 &&
          render->resident_scene_pass && render->resident_application_pass &&
          render->cam_x==camera_x && render->cam_y==camera_y &&
          !render->pending_fill && !render->pending_fill_color &&
          !render->pending_underlay && !render->underlay_x && !render->underlay_y &&
          !render->underlay_w && !render->underlay_h &&
          !render->fb_opaque_known && !render->fb_all_opaque && !render->fb_all_transparent);
    gml_render_resource_journal_metrics(resource,&after);
    CHECK(after.authority==GML_RENDER_RESOURCE_GPU_RESIDENT &&
          after.logical_version==before.logical_version &&
          after.cpu_version==before.cpu_version && after.gpu_version==before.gpu_version &&
          !after.cpu_replays && !after.journal_bytes && !fake.readbacks && fake.submits==1);
    /* A repeated reset stays a no-op for authority, not a new admission. */
    gml_surface_reset_target(render);
    CHECK(render->resident_application_pass && !fake.readbacks && fake.submits==1);
    gml_render_set_pending_fill(render,0xFF26486Au);
    gml_render_flush_pending_fill(render);
    CHECK(gml_render_dispatch_metrics(render,&dispatch));
    uint64_t cpu_draws=0;
    for(unsigned reason=0;reason<GML_RENDER_DISPATCH_REASON_COUNT;reason++)
      cpu_draws+=dispatch.cpu[GML_RENDER_SCENE_PRIMITIVE][reason];
    if(dispatch.gpu[GML_RENDER_SCENE_PRIMITIVE]!=2u ||
       cpu_draws || fake.submits!=2 || fake.readbacks)
      fprintf(stderr,"empty reset authority: classic=%u target=%u gui=%u pending=%u GPU=%llu CPU=%llu submits=%d reads=%d\n",
        classic,target,gui,pending,(unsigned long long)dispatch.gpu[GML_RENDER_SCENE_PRIMITIVE],
        (unsigned long long)cpu_draws,fake.submits,fake.readbacks);
    CHECK(gml_render_dispatch_metrics(render,&dispatch) &&
          dispatch.gpu[GML_RENDER_SCENE_PRIMITIVE]==2u &&
          !cpu_draws && !dispatch.failed &&
          !dispatch.unclassified && fake.submits==2 && !fake.readbacks);
    fake.readback_colour=0xFF26486Au;
    const uint32_t *pixels=NULL;
    CHECK(gml_render_resource_acquire_cpu_read(resource,&pixels) &&
          pixels==render->fb && fake.readbacks==1);
    for(unsigned pixel=0;pixel<16u;pixel++) CHECK(pixels[pixel]==0xFF26486Au);
    gml_render_free(render);free(render);
  }
  return 1;
}

static int resident_empty_reset_does_not_admit_a_different_owner(void){
  for(unsigned variant=0;variant<6u;variant++){
    GmlRender *render=calloc(1,sizeof *render);
    uint32_t base[16]={0},other[16]={0};
    FakeDevice fake={.on_demand=1};
    GmlRenderDeviceServices device=service(&fake);
    device.observe_on_demand=fake_observe_on_demand;
    CHECK(render && gml_render_init(render,NULL)==0);
    gml_render_bind_device(render,&device);
    CHECK(gml_render_begin_resident_base(render,base,4,4,2.0,3.0));
    if(variant==0u) CHECK(gml_render_resident_application_cpu_barrier(render));
    if(variant==1u) render->resident_scene_pass=0;
    if(variant==2u) render->base_fb=other;
    if(variant==3u) render->base_fbw=2;
    if(variant==4u) render->base_scene_resource_width=2;
    if(variant==5u) render->target_id=0;
    gml_surface_reset_target(render);
    CHECK(!render->resident_application_pass && render->target_id==-1 &&
          !render->target_sp && render->fb==render->base_fb && !fake.submits &&
          !fake.readbacks && render->cam_x==2.0 && render->cam_y==3.0);
    gml_render_free(render);free(render);
  }
  return 1;
}

static int resident_target_alias_resumes_cpu_parent(void){
  GmlRender *render=(GmlRender*)calloc(1,sizeof *render);
  FakeDevice fake={0};
  GmlRenderDeviceServices device=service(&fake);
  GmlRenderJournalMetrics metrics={0};
  CHECK(render && gml_render_init(render,NULL)==0 &&
        gml_render_application_surface_ensure_owned(render,4,4));
  gml_render_bind_device(render,&device);
  CHECK(gml_render_begin_resident_owned_application(render,0.0,0.0,0) &&
        gml_render_resident_application_cpu_barrier(render) &&
        gml_surface_set_target(render,0) && render->resident_application_pass);
  gml_render_set_pending_fill(render,0xFF315779u);
  gml_render_flush_pending_fill(render);
  CHECK(render->app_surface_owned[0]==0u && fake.submits==1);
  gml_surface_reset_target(render);
  CHECK(!render->resident_application_pass);
  gml_render_primitive_point(render,0,0,0x75A4C3u);
  CHECK(render->app_surface_owned[0]==0xFFC3A475u &&
        render->app_surface_owned[1]==0xFF315779u && fake.readbacks==0);
  gml_render_resource_journal_metrics(render->app_surface_resource,&metrics);
  CHECK(metrics.authority==GML_RENDER_RESOURCE_CPU_CURRENT &&
        metrics.cpu_version==metrics.logical_version && metrics.cpu_replays==1u);
  gml_render_bind_device(render,NULL);
  gml_render_free(render);
  free(render);
  return 1;
}

static int numbered_surface_resize_replays_and_retires(void){
  GmlRender *render=(GmlRender*)calloc(1,sizeof *render);
  uint32_t base[16]={0};
  FakeDevice fake={0};
  GmlRenderDeviceServices device=service(&fake);
  GmlRenderResource *resource;
  GmlRenderImageRef old_ref,new_ref;
  GmlRenderDrawOp operation;
  GmlRenderDrawBatch batch;
  const uint32_t *pixels;
  int outer,inner,w=0,h=0;
  CHECK(render);
  render->next_surface_id=1;
  gml_render_begin(render,base,4,4,0.0,0.0);
  outer=gml_surface_create(render,2,2);
  inner=gml_surface_create(render,2,2);
  CHECK(outer>0 && inner>0 && gml_surface_set_target(render,outer));
  gml_render_bind_device(render,&device);
  resource=gml_render_surface_resource_ensure(render,outer);
  CHECK(resource);
  old_ref=gml_render_resource_image_ref(resource);
  operation=clear_op(old_ref.content_version+1u,0xFF315779u,GML_RENDER_REPLAY_EXACT);
  operation.target.image=old_ref;
  operation.target.image.content_version++;
  operation.target.width=operation.target.height=2u;
  operation.target.region=(GmlRenderRect){0,0,2u,2u};
  operation.target.pixel_format=GML_RENDER_PIXEL_ARGB8888;
  batch=(GmlRenderDrawBatch){&operation,1u,NULL,0u};
  CHECK(gml_render_surface_scene_submit(render,outer,&batch,NULL,0u)==
        GML_RENDER_RESOURCE_EXEC_DEVICE && render->surface[outer-1].px[0]==0u);
  CHECK(gml_surface_set_target(render,inner));
  gml_surface_resize(render,outer,3,3);
  CHECK(render->target_stack[1].fb==render->surface[outer-1].px &&
        render->target_stack[1].w==3 && render->target_stack[1].h==3 &&
        !render->surface[outer-1].resource && fake.retires==1 && !fake.readbacks);
  gml_surface_reset_target(render);
  pixels=gml_surface_pixels_read(render,outer,&w,&h);
  CHECK(pixels==render->fb && w==3 && h==3 &&
        pixels[0]==0xFF315779u && pixels[4]==0xFF315779u &&
        pixels[2]==0u && pixels[8]==0u);
  resource=gml_render_surface_resource_ensure(render,outer);
  CHECK(resource);
  new_ref=gml_render_resource_image_ref(resource);
  CHECK(new_ref.incarnation!=old_ref.incarnation && new_ref.content_version==1u);
  operation.target.image=new_ref;
  operation.target.image.content_version++;
  operation.target.width=operation.target.height=3u;
  operation.target.region=(GmlRenderRect){0,0,3u,3u};
  CHECK(gml_render_surface_scene_submit(render,outer,&batch,NULL,0u)==
        GML_RENDER_RESOURCE_EXEC_DEVICE);
  gml_render_set_pending_fill(render,0xFF7395B7u);
  CHECK(render->pending_fill);
  gml_surface_resize(render,outer,4,4);
  pixels=gml_surface_pixels_read(render,outer,&w,&h);
  CHECK(pixels==render->fb && w==4 && h==4 && !render->pending_fill &&
        pixels[0]==0xFF7395B7u && pixels[10]==0xFF7395B7u &&
        pixels[3]==0u && pixels[15]==0u && fake.retires==2 && !fake.readbacks);
  gml_render_bind_device(render,NULL);
  CHECK(!fake.readbacks);
  gml_surface_reset_target(render);
  gml_surface_free(render,inner);
  gml_surface_free(render,outer);
  gml_render_free(render);
  free(render);
  return 1;
}

static int numbered_surface_free_bound_resource(void){
  GmlRender *render=(GmlRender*)calloc(1,sizeof *render);
  uint32_t base[16]={0};
  FakeDevice fake={0};
  GmlRenderDeviceServices device=service(&fake);
  GmlRenderImageRef old_ref,new_ref;
  GmlRenderDrawOp operation;
  GmlRenderDrawBatch batch;
  int outer,inner,reused;
  CHECK(render);
  render->next_surface_id=1;
  gml_render_begin(render,base,4,4,0.0,0.0);
  outer=gml_surface_create(render,2,2);
  inner=gml_surface_create(render,2,2);
  CHECK(outer>0 && inner>0 && gml_surface_set_target(render,outer));
  gml_render_bind_device(render,&device);
  old_ref=gml_render_resource_image_ref(gml_render_surface_resource_ensure(render,outer));
  CHECK(old_ref.incarnation);
  operation=clear_op(old_ref.content_version+1u,0xFF123456u,GML_RENDER_REPLAY_EXACT);
  operation.target.image=old_ref;
  operation.target.image.content_version++;
  operation.target.width=operation.target.height=2u;
  operation.target.region=(GmlRenderRect){0,0,2u,2u};
  operation.target.pixel_format=GML_RENDER_PIXEL_ARGB8888;
  batch=(GmlRenderDrawBatch){&operation,1u,NULL,0u};
  CHECK(gml_render_surface_scene_submit(render,outer,&batch,NULL,0u)==
        GML_RENDER_RESOURCE_EXEC_DEVICE && gml_surface_set_target(render,inner));
  gml_surface_free(render,outer);
  CHECK(!gml_surface_exists(render,outer) && !fake.readbacks && fake.retires==1 &&
        render->target_id==inner && render->target_sp==1 &&
        render->target_stack[0].fb==base);
  render->next_surface_id=outer;
  reused=gml_surface_create(render,3,3);
  CHECK(reused==outer);
  new_ref=gml_render_resource_image_ref(gml_render_surface_resource_ensure(render,reused));
  CHECK(new_ref.incarnation!=old_ref.incarnation);
  gml_surface_reset_target(render);
  CHECK(render->fb==base && render->target_id==-1 && render->target_sp==0);
  CHECK(gml_surface_set_target(render,reused));
  gml_render_set_pending_fill(render,0xFF7395B7u);
  CHECK(render->pending_fill);
  gml_surface_free(render,reused);
  CHECK(!gml_surface_exists(render,reused) && !fake.readbacks && fake.retires==2 &&
        render->fb==base && render->target_id==-1 && render->target_sp==0 &&
        !render->pending_fill);
  gml_render_bind_device(render,NULL);
  gml_surface_free(render,inner);
  gml_render_free(render);
  free(render);
  return 1;
}

static int owned_application_resize_and_context_loss(void){
  GmlRender *render=(GmlRender*)calloc(1,sizeof *render);
  GmlRenderDeviceServices device;
  GmlRenderResource *resource;
  GmlRenderImageRef first_ref,second_ref;
  GmlRenderDrawOp operation;
  GmlRenderDrawBatch batch;
  GmlRenderJournalMetrics metrics={0};
  GmlRenderApplicationWriteView view={0};
  FakeDevice fake={0};
  uint8_t *state_bytes;
  size_t state_size,written=0,used=0;
  CHECK(render);
  CHECK(gml_render_application_surface_ensure_owned(render,4,4));
  device=service(&fake);
  gml_render_bind_device(render,&device);
  resource=gml_render_application_resource_ensure(render);
  CHECK(resource);
  first_ref=gml_render_resource_image_ref(resource);
  CHECK(first_ref.resource_class==GML_RENDER_RESOURCE_APPLICATION &&
        first_ref.identity==0u && first_ref.incarnation!=0u);
  operation=clear_op(2u,0x40112233u,GML_RENDER_REPLAY_EXACT);
  operation.target.image=first_ref;
  operation.target.image.content_version=2u;
  operation.target.pixel_format=GML_RENDER_PIXEL_ARGB8888;
  batch=(GmlRenderDrawBatch){&operation,1u,NULL,0u};
  CHECK(gml_render_application_scene_submit(render,&batch,NULL,0u)==
        GML_RENDER_RESOURCE_EXEC_DEVICE);
  gml_render_resource_journal_metrics(resource,&metrics);
  CHECK(metrics.journal_operations==1u && metrics.cpu_replays==0u &&
        render->app_surface_owned[0]==0u);
  gml_render_application_surface_bind(render,render->app_surface_owned,7,5,0);
  CHECK(!gml_render_application_resource_ensure(render) &&
        !gml_render_application_surface_owned_view(render,&view));
  CHECK(gml_render_application_surface_ensure_owned(render,6,5));
  CHECK(!render->app_surface_resource && fake.retires==1 && fake.readbacks==0 &&
        render->app_surface_owned[0]==0x40112233u &&
        render->app_surface_owned[3u+3u*6u]==0x40112233u &&
        render->app_surface_owned[4u]==0u);
  resource=gml_render_application_resource_ensure(render);
  CHECK(resource);
  second_ref=gml_render_resource_image_ref(resource);
  CHECK(second_ref.identity==first_ref.identity &&
        second_ref.incarnation!=first_ref.incarnation);
  operation=clear_op(2u,0xFF445566u,GML_RENDER_REPLAY_EXACT);
  operation.target.image=second_ref;
  operation.target.image.content_version=2u;
  operation.target.width=6u;
  operation.target.height=5u;
  operation.target.pixel_format=GML_RENDER_PIXEL_ARGB8888;
  operation.target.region=(GmlRenderRect){0,0,6u,5u};
  CHECK(gml_render_application_scene_submit(render,&batch,NULL,0u)==
        GML_RENDER_RESOURCE_EXEC_DEVICE);
  gml_render_resource_journal_metrics(resource,&metrics);
  CHECK(metrics.journal_operations==1u && metrics.cpu_replays==0u);
  CHECK(gml_render_begin_resident_owned_application(render,2.0,3.0,0));
  CHECK(render->fb==render->app_surface_owned &&
        render->cam_x==2.0 && render->cam_y==3.0);
  gml_render_resource_journal_metrics(resource,&metrics);
  CHECK(metrics.journal_operations==1u && metrics.cpu_replays==0u);
  gml_render_set_pending_fill(render,0xFF010203u);
  CHECK(!gml_render_begin_resident_owned_application(render,4.0,5.0,0) &&
        render->pending_fill);
  gml_render_cancel_pending_fill(render);
  CHECK(gml_surface_set_target(render,0));
  second_ref=gml_render_resource_image_ref(resource);
  operation.target.image=second_ref;
  operation.target.image.content_version=second_ref.content_version+1u;
  CHECK(gml_render_application_scene_submit(render,&batch,NULL,0u)==
        GML_RENDER_RESOURCE_EXEC_DEVICE);
  gml_render_resource_journal_metrics(resource,&metrics);
  CHECK(metrics.journal_operations==1u);
  CHECK(gml_render_begin_resident_owned_application(render,6.0,7.0,1) &&
        render->target_id==0 && render->fb==render->app_surface_owned);
  gml_render_resource_journal_metrics(resource,&metrics);
  CHECK(metrics.journal_operations==1u);
  gml_surface_reset_target(render);
  gml_render_bind_device(render,NULL);
  CHECK(gml_render_application_surface_owned_view(render,&view) &&
        view.pixels==render->app_surface_owned && view.width==6 && view.height==5 &&
        view.pixels[0]==0xFF445566u && view.pixels[29]==0xFF445566u &&
        fake.readbacks==0);
  state_size=gml_render_state_size(render,0);
  CHECK(state_size>0);
  state_bytes=(uint8_t*)malloc(state_size);
  CHECK(state_bytes && gml_render_state_save(render,0,state_bytes,state_size,&written) &&
        written==state_size);
  CHECK(gml_render_state_load(render,state_bytes,state_size,&used) &&
        used==state_size && !render->app_surface_resource &&
        !render->resident_application_pass);
  free(state_bytes);
  gml_render_free(render);
  free(render);
  return 1;
}

static int resident_base_linear_history(void){
  GmlRender *r=calloc(1,sizeof *r);
  FakeDevice fake={.on_demand=1,.readback_colour=0x40123456u};
  GmlRenderDeviceServices device=service(&fake);
  uint32_t pixels[120]={0};
  CHECK(r);
  device.observe_on_demand=fake_observe_on_demand;
  gml_render_bind_device(r,&device);
  CHECK(gml_render_begin_resident_base(r,pixels,8,6,0,0));
  gml_render_set_pending_fill(r,0x40123456u);
  gml_render_flush_pending_fill(r);
  CHECK(gml_render_scene_submit_pending(r) && !fake.readbacks);
  /* Same allocation, changed pitch, then shrink/grow: no canonical observation. */
  const int extents[][2]={{7,5},{3,2},{12,10},{5,4},{8,6}};
  for(unsigned i=0;i<sizeof extents/sizeof extents[0];i++){
    CHECK(gml_render_begin_resident_base(r,pixels,extents[i][0],extents[i][1],0,0));
    CHECK(!fake.readbacks && r->resident_application_pass);
    GmlRenderJournalMetrics metrics={0};
    gml_render_resource_journal_metrics(r->base_scene_resource,&metrics);
    CHECK(metrics.authority==GML_RENDER_RESOURCE_GPU_RESIDENT && !metrics.cpu_replays &&
          !metrics.journal_bytes && !metrics.retained_mutable_bytes);
  }
  /* A failed explicit publication preparation cannot leak partial history into the live tail. */
  fake.fail_partial_readback=1;
  CHECK(!gml_render_completed_screen_prepare_publish(r,pixels,8,6));
  CHECK(pixels[0]==0u && pixels[119]==0u);
  fake.fail_partial_readback=0;
  CHECK(gml_render_completed_screen_prepare_publish(r,pixels,8,6));
  CHECK(pixels[0]==0u && pixels[119]==fake.readback_colour);
  CHECK(gml_render_observe_device_resources(r));
  CHECK(pixels[0]==fake.readback_colour && pixels[119]==fake.readback_colour);
  gml_render_bind_device(r,NULL);
  CHECK(gml_render_completed_screen_read(r,pixels,8,6));
  gml_render_free(r);free(r);
  return 1;
}

static int resident_base_history_failures(void){
  for(unsigned failure=0;failure<5u;failure++){
    GmlRender *r=calloc(1,sizeof *r);
    FakeDevice fake={.on_demand=1,.readback_colour=0x40123456u};
    GmlRenderDeviceServices device=service(&fake);
    uint32_t pixels[120],before[120];
    for(unsigned i=0;i<120u;i++) pixels[i]=0xA5000000u+i;
    memcpy(before,pixels,sizeof before);
    CHECK(r);device.observe_on_demand=fake_observe_on_demand;
    gml_render_bind_device(r,&device);
    CHECK(gml_render_begin_resident_base(r,pixels,8,6,0,0));
    CHECK(!gml_render_resource_rebind_resident_pixels(r->base_scene_resource,pixels));
    gml_render_set_pending_fill(r,0x40123456u);gml_render_flush_pending_fill(r);
    CHECK(gml_render_scene_submit_pending(r));
    GmlRenderResource *old=r->base_scene_resource;
    GmlRenderImageRef ref=gml_render_resource_image_ref(old);
    fake.fail_submit_at=fake.submits+(int)failure+1;
    fake.prewrite_refusal=1;
    CHECK(!gml_render_base_resource_resize(r,12,10));
    CHECK(r->base_scene_resource==old && !r->base_history_resource &&
          gml_render_resource_image_ref(old).incarnation==ref.incarnation &&
          !memcmp(pixels,before,sizeof pixels));
    GmlRenderJournalMetrics metrics={0};
    gml_render_resource_journal_metrics(old,&metrics);
    CHECK(metrics.authority==GML_RENDER_RESOURCE_GPU_RESIDENT);
    fake.fail_submit_at=0;
    CHECK(gml_render_base_resource_resize(r,12,10));
    CHECK(!gml_render_base_resource_resize(r,0,10) &&
          !gml_render_base_resource_resize(r,4097,10));
    fake.forbid_calls=1;
    gml_render_lose_volatile_device(r);
    CHECK(!r->base_scene_resource && !r->base_history_resource && !r->base_history_pixels);
    CHECK(!fake.forbidden_calls);
    gml_render_free(r);free(r);
  }
  return 1;
}

static int resident_surface_resize_storage(void){
  static const int extents[][2]={{6,5},{2,3},{2,6},{7,2},{4,4}};
  for(unsigned binding=0;binding<3u;binding++)
    for(unsigned classic=0;classic<2u;classic++)
      for(unsigned extent=0;extent<sizeof extents/sizeof extents[0];extent++){
        GmlRender *render=(GmlRender*)calloc(1,sizeof *render);
        FakeDevice fake={.on_demand=1};
        GmlRenderDeviceServices device=service(&fake);
        GmlRenderResource *source,*replacement;
        GmlRenderImageRef old_ref,new_ref;
        GmlRenderJournalMetrics metrics={0};
        uint32_t base[16]={0},*old,*pixels;
        int id=0,w=extents[extent][0],h=extents[extent][1];
        CHECK(render);
        render->classic=(int)classic;
        render->next_surface_id=1;
        device.observe_on_demand=fake_observe_on_demand;
        if(binding==2u){
          id=gml_surface_create(render,4,4);
          CHECK(id>0);
          old=render->surface[id-1].px;
          source=gml_render_surface_resource_ensure(render,id);
        } else if(binding==1u){
          gml_render_bind_device(render,&device);
          CHECK(gml_render_begin_resident_base(render,base,4,4,0.0,0.0));
          old=base;
          gml_render_application_surface_bind(render,old,4,4,0);
          source=render->base_scene_resource;
        } else {
          CHECK(gml_render_application_surface_ensure_owned(render,4,4));
          old=render->app_surface_owned;
          source=gml_render_application_resource_ensure(render);
        }
        CHECK(source);
        gml_render_bind_device(render,&device);
        old_ref=gml_render_resource_image_ref(source);
        GmlRenderDrawOp clear=clear_op(old_ref.content_version+1u,
          0x00112233u,GML_RENDER_DEVICE_CHECKPOINT);
        clear.target.image=old_ref;
        clear.target.image.content_version++;
        clear.target.pixel_format=GML_RENDER_PIXEL_ARGB8888;
        GmlRenderDrawBatch batch={&clear,1u,NULL,0u};
        CHECK(gml_render_resource_submit_deferred(source,&device,&batch,NULL,0u)==
              GML_RENDER_RESOURCE_EXEC_DEVICE);
        render->fb=render->base_fb=old;
        render->fbw=render->base_fbw=4;
        render->fbh=render->base_fbh=4;
        render->target_sp=2;
        render->target_stack[0].fb=render->target_stack[1].fb=old;
        render->target_stack[0].w=render->target_stack[1].w=4;
        render->target_stack[0].h=render->target_stack[1].h=4;
        /* Storage copy must ignore the state of an ordinary content draw. */
        render->color=0x123456u;
        render->alpha=0.01;
        if(binding==2u){
          gml_surface_resize(render,id,w,h);
          pixels=render->surface[id-1].px;
          replacement=render->surface[id-1].resource;
        } else {
          CHECK(gml_render_application_surface_ensure_owned(render,w,h));
          pixels=render->app_surface_owned;
          replacement=render->app_surface_resource;
        }
        if(binding==0u && extent==4u){
          CHECK(pixels==old && replacement==source && fake.submits==1);
        } else {
          CHECK(pixels!=old && replacement && fake.submits==2 &&
                fake.last_batch_operations==2u && fake.last_batch_states==1u);
          new_ref=gml_render_resource_image_ref(replacement);
          CHECK(new_ref.incarnation!=old_ref.incarnation &&
                new_ref.content_version==3u &&
                new_ref.resource_class==(binding==2u?GML_RENDER_RESOURCE_SURFACE:
                  GML_RENDER_RESOURCE_APPLICATION) && new_ref.identity==(uint32_t)id);
          CHECK(fake.last_quad.x[1]==(w<4?w:4) &&
                fake.last_quad.y[2]==(h<4?h:4) &&
                fake.last_quad.u[1]==(double)(w<4?w:4)/4 &&
                fake.last_quad.v[2]==(double)(h<4?h:4)/4 &&
                fake.last_quad_state.write_mask==15u &&
                fake.last_quad_state.alpha==255u &&
                fake.last_quad_state.colour==0xFFFFFFFFu &&
                !fake.last_quad_state.rgb_equation &&
                !fake.last_quad_state.alpha_test_enabled);
          gml_render_resource_journal_metrics(replacement,&metrics);
          CHECK(metrics.authority==GML_RENDER_RESOURCE_GPU_RESIDENT &&
                metrics.logical_version==3u && metrics.cpu_version==1u &&
                !metrics.cpu_replays && !metrics.journal_operations);
          for(int pixel=0;pixel<w*h;pixel++) CHECK(pixels[pixel]==0u);
          CHECK(fake.retires==(binding==1u?0:1));
        }
        CHECK(!fake.readbacks && !fake.uploads && !fake.rgb_queries &&
              render->fb==pixels && render->base_fb==pixels &&
              render->fbw==w && render->fbh==h &&
              render->target_stack[0].fb==pixels &&
              render->target_stack[1].fb==pixels &&
              render->target_stack[0].w==w && render->target_stack[1].h==h);
        render->target_sp=0;
        gml_render_free(render);
        free(render);
      }
  return 1;
}

static int resident_surface_resize_refusal(void){
  for(unsigned binding=0;binding<2u;binding++){
    GmlRender *render=(GmlRender*)calloc(1,sizeof *render);
    FakeDevice fake={.on_demand=1,.readback_colour=0x00112233u};
    GmlRenderDeviceServices device=service(&fake);
    GmlRenderResource *resource;
    uint32_t *old,*pixels;
    int id=0;
    CHECK(render);
    render->next_surface_id=1;
    device.observe_on_demand=fake_observe_on_demand;
    if(binding){
      id=gml_surface_create(render,4,4);
      old=render->surface[id-1].px;
      resource=gml_render_surface_resource_ensure(render,id);
    } else {
      CHECK(gml_render_application_surface_ensure_owned(render,4,4));
      old=render->app_surface_owned;
      resource=gml_render_application_resource_ensure(render);
    }
    CHECK(resource);
    gml_render_bind_device(render,&device);
    GmlRenderDrawOp clear=clear_op(2u,0x00112233u,GML_RENDER_DEVICE_CHECKPOINT);
    clear.target.image=gml_render_resource_image_ref(resource);
    clear.target.image.content_version++;
    clear.target.pixel_format=GML_RENDER_PIXEL_ARGB8888;
    GmlRenderDrawBatch batch={&clear,1u,NULL,0u};
    CHECK(gml_render_resource_submit_deferred(resource,&device,&batch,NULL,0u)==
          GML_RENDER_RESOURCE_EXEC_DEVICE);
    fake.status=GML_RENDER_EXEC_UNSUPPORTED;
    fake.prewrite_refusal=1;
    fake.fail_readback=1;
    if(binding){
      gml_surface_resize(render,id,6,5);
      CHECK(render->surface[id-1].px==old && render->surface[id-1].resource==resource);
    } else {
      CHECK(!gml_render_application_surface_ensure_owned(render,6,5));
      CHECK(render->app_surface_owned==old && render->app_surface_resource==resource);
    }
    GmlRenderJournalMetrics metrics={0};
    gml_render_resource_journal_metrics(resource,&metrics);
    CHECK(metrics.authority==GML_RENDER_RESOURCE_GPU_RESIDENT &&
          metrics.logical_version==2u && old[0]==0u);
    fake.fail_readback=0;
    if(binding){
      gml_surface_resize(render,id,6,5);
      pixels=render->surface[id-1].px;
    } else {
      CHECK(gml_render_application_surface_ensure_owned(render,6,5));
      pixels=render->app_surface_owned;
    }
    CHECK(pixels!=old && pixels[0]==0x00112233u &&
          pixels[21]==0x00112233u && pixels[4]==0u && pixels[29]==0u);
    gml_render_free(render);
    free(render);
  }
  return 1;
}

static int resident_surface_resize_state(void){
  for(unsigned numbered=0;numbered<2u;numbered++){
    GmlRender *render=(GmlRender*)calloc(1,sizeof *render);
    FakeDevice fake={.on_demand=1,.readback_colour=0x7FABCDEFu};
    GmlRenderDeviceServices device=service(&fake);
    GmlRenderResource *resource;
    uint8_t *bytes,*again;
    size_t capacity,written=0,used=0,again_written=0;
    int id=0;
    CHECK(render && gml_render_init(render,NULL)==0);
    device.observe_on_demand=fake_observe_on_demand;
    if(numbered){
      id=gml_surface_create(render,4,4);
      CHECK(id>0);
      resource=gml_render_surface_resource_ensure(render,id);
    }else{
      CHECK(gml_render_application_surface_ensure_owned(render,4,4));
      resource=gml_render_application_resource_ensure(render);
    }
    CHECK(resource);
    gml_render_bind_device(render,&device);
    GmlRenderDrawOp clear=clear_op(2u,0x7FABCDEFu,GML_RENDER_DEVICE_CHECKPOINT);
    clear.target.image=gml_render_resource_image_ref(resource);
    clear.target.image.content_version++;
    clear.target.pixel_format=GML_RENDER_PIXEL_ARGB8888;
    GmlRenderDrawBatch batch={&clear,1u,NULL,0u};
    CHECK(gml_render_resource_submit_deferred(resource,&device,&batch,NULL,0u)==
          GML_RENDER_RESOURCE_EXEC_DEVICE);
    if(numbered) gml_surface_resize(render,id,6,5);
    else CHECK(gml_render_application_surface_ensure_owned(render,6,5));
    resource=numbered?render->surface[id-1].resource:render->app_surface_resource;
    CHECK(resource && !fake.readbacks);
    capacity=gml_render_state_capacity(render,0);
    CHECK(capacity>0 && !fake.readbacks);
    if(!numbered){
      /* The root engine observes application pixels separately; the renderer codec owns
       * numbered surface bytes, not the engine's completed/application framebuffer. */
      const uint32_t *pixels=NULL;
      CHECK(gml_render_observe_device_resources(render) && fake.readbacks==1);
      gml_render_resource_context_lost(resource);
      CHECK(gml_render_resource_acquire_cpu_read(resource,&pixels) &&
            pixels==render->app_surface_owned && pixels[0]==0x7FABCDEFu &&
            pixels[29]==0x7FABCDEFu && fake.readbacks==1);
      gml_render_free(render);free(render);
      continue;
    }
    bytes=malloc(capacity);again=malloc(capacity);
    CHECK(bytes && again && gml_render_state_save(render,0,bytes,capacity,&written) &&
          written>0 && written<=capacity && fake.readbacks==1);
    CHECK(gml_render_state_load(render,bytes,written,&used) && used==written);
    uint32_t *pixels=render->surface[id-1].px;
    CHECK(pixels && pixels[0]==0x7FABCDEFu && pixels[29]==0x7FABCDEFu &&
          !render->surface[id-1].resource);
    CHECK(gml_render_state_save(render,0,again,capacity,&again_written) &&
          again_written==written && !memcmp(bytes,again,written) && fake.readbacks==1);
    free(bytes);free(again);
    gml_render_free(render);free(render);
  }
  return 1;
}

static int resident_application_pending_clear(void){
  GmlRender *render=(GmlRender*)calloc(1,sizeof *render);
  GmlRenderDeviceServices device;
  GmlRenderJournalMetrics metrics={0};
  GmlRenderApplicationWriteView view={0};
  FakeDevice fake={0};
  CHECK(render);
  CHECK(gml_render_application_surface_ensure_owned(render,4,4));
  device=service(&fake);
  gml_render_bind_device(render,&device);
  CHECK(gml_render_begin_resident_owned_application(render,1.0,2.0,0));
  gml_render_set_pending_fill(render,0x40112233u);
  gml_render_flush_pending_fill(render);
  gml_render_resource_journal_metrics(render->app_surface_resource,&metrics);
  CHECK(!render->pending_fill && fake.submits==1 && fake.readbacks==0 &&
        metrics.journal_operations==1u && metrics.cpu_replays==0u &&
        render->app_surface_owned[0]==0u);
  CHECK(gml_render_begin_resident_owned_application(render,3.0,4.0,0));
  gml_render_resource_journal_metrics(render->app_surface_resource,&metrics);
  CHECK(metrics.journal_operations==1u && metrics.cpu_replays==0u);
  gml_render_set_pending_fill(render,0xFF445566u);
  fake.status=GML_RENDER_EXEC_UNSUPPORTED;
  gml_render_flush_pending_fill(render);
  gml_render_resource_journal_metrics(render->app_surface_resource,&metrics);
  CHECK(!render->pending_fill && metrics.cpu_fallbacks==1u &&
        metrics.journal_operations==0u && render->app_surface_owned[0]==0xFF445566u &&
        render->app_surface_owned[15]==0xFF445566u);
  fake.status=0;
  gml_render_set_pending_fill(render,0xFF123456u);
  gml_render_flush_pending_fill(render);
  gml_render_resource_journal_metrics(render->app_surface_resource,&metrics);
  CHECK(fake.submits==3 && metrics.journal_operations==1u &&
        render->app_surface_owned[0]==0xFF445566u);
  gml_render_bind_device(render,NULL);
  CHECK(gml_render_resident_application_cpu_barrier(render) &&
        !render->resident_application_pass && fake.readbacks==0);
  gml_render_resource_journal_metrics(render->app_surface_resource,&metrics);
  CHECK(metrics.cpu_replays==1u && metrics.journal_operations==0u &&
        render->app_surface_owned[0]==0xFF123456u &&
        render->app_surface_owned[15]==0xFF123456u);
  gml_render_bind_device(render,&device);
  CHECK(gml_render_begin_owned_application(render,0.0,0.0,0) &&
        !render->resident_application_pass);
  gml_render_set_pending_fill(render,0xFF778899u);
  gml_render_flush_pending_fill(render);
  CHECK(fake.submits==3 && render->app_surface_owned[0]==0xFF778899u);
  CHECK(gml_render_application_surface_owned_view(render,&view) &&
        view.pixels[15]==0xFF778899u);
  gml_render_free(render);
  free(render);
  return 1;
}

static int owned_application_clear_without_cpu_view(void){
  static const int generations[]={530,700,800,13,15,17};
  static const uint32_t colours[]={0x00315987u,0x80315987u,0xFF315987u};
  for(unsigned policy=0;policy<6u;policy++)
  for(unsigned target=0;target<3u;target++)
  for(unsigned mode=0;mode<5u;mode++)
  for(unsigned colour=0;colour<3u;colour++){
    GmlRender *r=calloc(1,sizeof *r);
    GmlWin content={0};uint32_t base[64]={0};
    FakeDevice fake={.on_demand=1,.prewrite_refusal=1};
    GmlRenderDeviceServices device=service(&fake);
    GmlRenderApplicationWriteView view={0};
    GmlRenderDrawState before={0},after={0};
    int admitted=mode==0;
    if(generations[policy]>100) content.classic_version=generations[policy];
    else content.bytecode=generations[policy];
    CHECK(r && !gml_render_init(r,NULL));
    r->win=&content;r->classic=content.classic_version!=0;
    CHECK(gml_render_application_surface_ensure_owned(r,6,5));
    device.observe_on_demand=mode==3?NULL:fake_observe_on_demand;
    gml_render_bind_device(r,&device);gml_render_set_scene_device(r,mode!=2);
    gml_render_begin(r,base,8,8,2,3);
    if(target==1) CHECK(gml_render_begin_owned_application(r,4,5,0));
    if(target==2){
      int surface=gml_surface_create(r,7,4);
      CHECK(surface>0 && gml_surface_set_target(r,surface));
    }
    if(mode==4) fake.status=GML_RENDER_EXEC_UNSUPPORTED;
    uint32_t *fb=r->fb;int id=r->target_id,depth=r->target_sp;
    double cx=r->cam_x,cy=r->cam_y;
    r->color_write_mask=5u;
    CHECK(gml_render_draw_state_get(r,&before));
    int submits=fake.submits;
    CHECK(gml_render_application_surface_owned_clear(r,colours[colour],mode==1?&view:NULL));
    CHECK(gml_render_draw_state_get(r,&after) && !memcmp(&before,&after,sizeof before) &&
          r->fb==fb && r->target_id==id && r->target_sp==depth &&
          r->cam_x==cx && r->cam_y==cy && !fake.readbacks && !fake.uploads);
    if(admitted){
      GmlRenderJournalMetrics metrics={0};
      gml_render_resource_journal_metrics(r->app_surface_resource,&metrics);
      CHECK(fake.submits==submits+1 && fake.last_opcode==GML_RENDER_OP_CLEAR &&
            fake.last_target_image.resource_class==GML_RENDER_RESOURCE_APPLICATION &&
            fake.last_target_region.width==6u && fake.last_target_region.height==5u &&
            metrics.authority==GML_RENDER_RESOURCE_GPU_RESIDENT &&
            !metrics.cpu_replays && !metrics.journal_bytes &&
            !r->app_surface_owned[0] && !r->app_surface_owned[29]);
    }else{
      CHECK(r->app_surface_owned[0]==colours[colour] &&
            r->app_surface_owned[29]==colours[colour] &&
            fake.submits==submits+(mode==4));
      if(mode==1) CHECK(view.pixels==r->app_surface_owned && view.width==6 && view.height==5);
    }
    gml_render_free(r);free(r);
  }
  return 1;
}

static int independent_numbered_target_admission(void){
  static const int generations[]={530,700,800,13,15,17};
  for(unsigned policy=0;policy<6u;policy++)
  for(unsigned parent=0;parent<2u;parent++)
  for(unsigned witness=0;witness<4u;witness++)
  for(unsigned runtime=0;runtime<2u;runtime++){
    GmlRender *r=calloc(1,sizeof *r);
    GmlWin content={0};uint32_t base[64]={0};
    FakeDevice fake={.on_demand=witness==1u,.readback_colour=0xFF315779u};
    GmlRenderDeviceServices device=service(&fake);
    const uint8_t rgba[16]={213,29,47,255,37,191,59,0,61,83,227,255,197,151,73,255};
    int admitted=witness==1u;
    if(generations[policy]>100) content.classic_version=generations[policy];
    else content.bytecode=generations[policy];
    CHECK(r && gml_render_init(r,NULL)==0);
    r->win=&content;r->classic=content.classic_version!=0;
    free(r->atlas[0].px);r->atlas[0].px=malloc(sizeof rgba);
    r->atlas[0].w=r->atlas[0].h=2;
    r->tpag=calloc(1,sizeof *r->tpag);r->spr=calloc(1,sizeof *r->spr);
    CHECK(r->atlas[0].px && r->tpag && r->spr);
    memcpy(r->atlas[0].px,rgba,sizeof rgba);
    r->n_tpag=r->n_spr=1;
    r->tpag[0]=(GmlTpag){.sw=2,.sh=2,.tw=2,.th=2,.bw=2,.bh=2};
    r->spr[0].frame=calloc(1,sizeof(int));CHECK(r->spr[0].frame);
    r->spr[0].n_frames=1;r->spr[0].w=r->spr[0].h=2;
    if(runtime){
      uint8_t *pixels=malloc(sizeof rgba);CHECK(pixels);memcpy(pixels,rgba,sizeof rgba);
      CHECK(gml_sprite_replace_from_rgba(r,0,pixels,2,2,0,0));
    }
    device.observe_on_demand=witness==2u?NULL:fake_observe_on_demand;
    gml_render_bind_device(r,&device);gml_render_set_scene_device(r,witness!=3u);
    if(parent){
      gml_render_begin(r,base,8,8,2.0,3.0);
      gml_render_set_pending_fill(r,0xFF123456u);
    }
    uint32_t *parent_fb=r->fb;int parent_w=r->fbw,parent_h=r->fbh;
    double parent_cx=r->cam_x,parent_cy=r->cam_y;
    int id=gml_surface_create(r,8,8);CHECK(id>0 && gml_surface_set_target(r,id));
    CHECK(!r->resident_scene_pass && r->resident_application_pass==admitted &&
          r->cam_x==0 && r->cam_y==0 && !r->pending_fill);
    for(unsigned draw=0;draw<3u;draw++)
      gml_draw_sprite_ext(r,0,0,draw*2,1,1,1,0,0xFFFFFFu,1);
    CHECK(gml_render_application_scene_flush(r));
    CHECK(!r->resident_scene_pass && r->resident_application_pass==admitted && !fake.readbacks);
    if(admitted){
      GmlRenderJournalMetrics metrics={0};
      gml_render_resource_journal_metrics(r->surface[id-1].resource,&metrics);
      if(fake.submits!=1 || fake.last_batch_operations<3u || fake.last_batch_operations>6u ||
         metrics.authority!=GML_RENDER_RESOURCE_GPU_RESIDENT || metrics.cpu_replays ||
         metrics.journal_bytes || r->fb[8])
        fprintf(stderr,"independent admission: policy=%d parent=%u runtime=%u submits=%d operations=%u authority=%u replay=%llu journal=%llu shadow=%08x\n",
          generations[policy],parent,runtime,fake.submits,fake.last_batch_operations,(unsigned)metrics.authority,
          (unsigned long long)metrics.cpu_replays,(unsigned long long)metrics.journal_bytes,r->fb[8]);
      /* Runtime binary coverage can produce two row packets per sprite;
       * all three public calls must still share one compatible submission. */
      CHECK(fake.submits==1 && fake.last_batch_operations>=3u && fake.last_batch_operations<=6u &&
            metrics.authority==GML_RENDER_RESOURCE_GPU_RESIDENT &&
            !metrics.cpu_replays && !metrics.journal_bytes && !r->fb[8]);
    }else CHECK(!fake.submits && r->fb[8]==0xFFD51D2Fu);
    gml_surface_reset_target(r);
    CHECK(!r->resident_scene_pass && !r->resident_application_pass && !r->target_sp &&
          r->fb==parent_fb && r->fbw==parent_w && r->fbh==parent_h &&
          r->cam_x==parent_cx && r->cam_y==parent_cy && !fake.readbacks &&
          r->pending_fill==(int)parent && (!parent || r->pending_fill_color==0xFF123456u));
    gml_render_free(r);free(r);
  }
  for(unsigned classic=0;classic<2u;classic++) for(unsigned alias=0;alias<2u;alias++){
    GmlRender *r=calloc(1,sizeof *r);uint32_t base[16]={0};
    FakeDevice fake={.on_demand=1,.readback_colour=0xFF315779u};
    GmlRenderDeviceServices device=service(&fake);
    CHECK(r && gml_render_init(r,NULL)==0);r->classic=(int)classic;
    int outer=gml_surface_create(r,4,4),inner=gml_surface_create(r,4,4);
    CHECK(outer>0 && inner>0);
    device.observe_on_demand=fake_observe_on_demand;
    gml_render_bind_device(r,&device);gml_render_set_scene_device(r,1);
    uint32_t *parent=alias?r->surface[outer-1].px:base;
    gml_render_begin(r,parent,4,4,2,3);
    CHECK(gml_surface_set_target(r,outer) && r->resident_application_pass && !r->resident_scene_pass);
    gml_render_set_pending_fill(r,0x7F315779u);
    gml_render_flush_pending_fill(r);
    CHECK(gml_surface_set_target(r,inner) && r->resident_application_pass &&
          !r->resident_scene_pass && r->target_sp==(classic?1:2));
    gml_render_set_pending_fill(r,0x7F123456u);
    gml_render_flush_pending_fill(r);
    gml_surface_reset_target(r);
    CHECK(r->target_id==(classic?-1:outer) && r->resident_application_pass==!classic);
    if(!classic) gml_surface_reset_target(r);
    CHECK(!r->resident_application_pass && !r->resident_scene_pass && !r->target_sp &&
          r->fb==parent && r->cam_x==2 && r->cam_y==3 && fake.readbacks==(int)alias);
    gml_render_free(r);free(r);
  }
  return 1;
}

static int independent_persistent_target_clear(void){
  static const uint32_t colours[]={0x00315779u,0x7F315779u,0xFF315779u};
  for(unsigned application=0;application<2u;application++)
  for(unsigned enabled=0;enabled<2u;enabled++) for(unsigned alpha=0;alpha<3u;alpha++){
    GmlRender *render=calloc(1,sizeof *render);
    uint32_t base[16]={0};
    FakeDevice fake={0};GmlRenderDeviceServices device=service(&fake);
    GmlRenderJournalMetrics metrics={0};
    CHECK(render && gml_render_init(render,NULL)==0);
    gml_render_set_scene_device(render,1);
    CHECK(!render->scene_device_enabled);
    gml_render_bind_device(render,&device);
    gml_render_set_scene_device(render,(int)enabled);
    gml_render_begin(render,base,4,4,2.0,3.0);
    int id=application?0:gml_surface_create(render,4,4);
    CHECK((application?gml_render_application_surface_ensure_owned(render,4,4):id>0) &&
          gml_surface_set_target(render,id));
    CHECK(!render->resident_scene_pass && !render->resident_application_pass);
    gml_render_set_pending_fill(render,colours[alpha]);
    gml_render_flush_pending_fill(render);
    CHECK(!render->pending_fill && !render->resident_scene_pass &&
          !render->resident_application_pass && fake.submits==(int)enabled && !fake.readbacks);
    if(enabled){
      GmlRenderResource *resource=gml_render_scene_target_resource_ensure(render);
      CHECK(resource && render->fb[0]==0u && fake.last_opcode==GML_RENDER_OP_CLEAR &&
            fake.last_target_image.resource_class==(application?GML_RENDER_RESOURCE_APPLICATION:
                                                              GML_RENDER_RESOURCE_SURFACE));
      CHECK(gml_render_resident_application_cpu_barrier(render));
      gml_render_resource_journal_metrics(resource,&metrics);
      CHECK(metrics.authority==GML_RENDER_RESOURCE_CPU_CURRENT &&
            metrics.cpu_version==metrics.logical_version && metrics.cpu_replays==1u);
    }
    CHECK(render->fb[0]==colours[alpha] && render->fb[15]==colours[alpha]);
    /* No manual flush: the CPU suffix's barrier must publish a later clear
     * before borrowing pixels, not replay it over the suffix afterwards. */
    if(enabled){
      gml_render_set_pending_fill(render,0xFF426486u);
      gml_render_primitive_point(render,0,0,0x75A4C3u);
      CHECK(render->fb[0]==0xFFC3A475u && render->fb[1]==0xFF426486u &&
            gml_surface_pixels_read(render,id,NULL,NULL)[0]==0xFFC3A475u && fake.submits==1);
    }
    gml_surface_reset_target(render);
    CHECK(render->fb==base && !render->resident_application_pass &&
          !render->resident_scene_pass && !base[0]);
    gml_render_bind_device(render,NULL);
    CHECK(!render->scene_device_enabled);
    gml_render_free(render);free(render);
  }
  /* A numbered child can alias an unadmitted CPU base. Restoring it may
   * observe the child, but must not silently admit that SCREEN or lose pixels. */
  for(unsigned failure=0;failure<2u;failure++){
    GmlRender *render=calloc(1,sizeof *render);
    FakeDevice fake={0};GmlRenderDeviceServices device=service(&fake);
    CHECK(render && gml_render_init(render,NULL)==0);
    int id=gml_surface_create(render,4,4);CHECK(id>0);
    uint32_t *base=render->surface[id-1].px;
    gml_render_begin(render,base,4,4,0,0);
    gml_render_application_surface_bind(render,base,4,4,0);
    gml_render_bind_device(render,&device);gml_render_set_scene_device(render,1);
    CHECK(gml_surface_set_target(render,id));
    fake.status=failure?GML_RENDER_EXEC_UNSUPPORTED:0;
    gml_render_set_pending_fill(render,0x7F315779u);
    gml_surface_reset_target(render);
    CHECK(render->target_sp==0 && render->target_id==-1 && render->fb==base &&
          !render->resident_application_pass && !render->resident_scene_pass &&
          base[0]==0x7F315779u && base[15]==0x7F315779u && !fake.readbacks);
    gml_render_primitive_point(render,0,0,0x75A4C3u);
    CHECK(base[0]!=0x7F315779u && base[1]==0x7F315779u);
    gml_render_free(render);free(render);
  }
  /* A fresh pending clear followed by a CPU-only suffix has no GPU prefix to
   * observe. The new admission must not invent a submit/readback round trip. */
  {
    GmlRender *render=calloc(1,sizeof *render);uint32_t base[16]={0};
    FakeDevice fake={.on_demand=1};GmlRenderDeviceServices device=service(&fake);
    device.observe_on_demand=fake_observe_on_demand;
    CHECK(render && gml_render_init(render,NULL)==0);
    gml_render_bind_device(render,&device);gml_render_set_scene_device(render,1);
    gml_render_begin(render,base,4,4,0,0);
    int id=gml_surface_create(render,4,4);CHECK(id>0 && gml_surface_set_target(render,id));
    /* Preserve this control's already-selected CPU suffix before queuing
     * its clear; target selection can now independently admit a GPU child. */
    CHECK(gml_render_resident_application_cpu_barrier(render));
    gml_render_set_pending_fill(render,0xFF426486u);
    gml_render_primitive_point(render,0,0,0x75A4C3u);
    CHECK(!render->pending_fill && render->fb[0]==0xFFC3A475u &&
          render->fb[1]==0xFF426486u && !fake.submits && !fake.uploads && !fake.readbacks);
    gml_render_free(render);free(render);
  }
  return 1;
}

static int resident_pending_clear_after_queued_draw(void){
  for(int on_demand=0;on_demand<2;on_demand++)
    for(int target=0;target<3;target++){
      GmlRender *render=(GmlRender*)calloc(1,sizeof *render);
      uint32_t base_pixels[16]={0};
      uint32_t source_pixels[4]={0xFF315779u,0xFF315779u,0xFF315779u,0xFF315779u};
      GmlRenderResourceSource source=source_view(source_pixels,1);
      GmlRenderDrawStateSnapshot state=quad_state(0);
      GmlRenderDrawOp operation=quad_op(2u,&source.image);
      GmlRenderJournalMetrics before={0},after={0};
      GmlRenderDispatchMetrics dispatch={0};
      FakeDevice fake={0};
      fake.on_demand=on_demand;
      GmlRenderDeviceServices device=service(&fake);
      device.observe_on_demand=fake_observe_on_demand;
      GmlRenderResource *resource;
      const uint32_t *pixels=NULL;
      CHECK(render && gml_render_init(render,NULL)==0 &&
            gml_render_application_surface_ensure_owned(render,4,4));
      gml_render_bind_device(render,&device);
      if(target==1){
        CHECK(gml_render_begin_resident_base(render,base_pixels,4,4,0.0,0.0));
      } else {
        CHECK(gml_render_begin_resident_owned_application(render,0.0,0.0,0));
        if(target==2){
          int id=gml_surface_create(render,4,4);
          CHECK(id>0 && gml_surface_set_target(render,id));
        }
      }
      gml_render_dispatch_diagnostics_enable(render,1);
      resource=gml_render_scene_target_resource_ensure(render);
      CHECK(resource);
      gml_render_resource_journal_metrics(resource,&before);
      operation.scene_family=GML_RENDER_SCENE_SPRITE;
      operation.target.image=gml_render_resource_image_ref(resource);
      operation.target.image.content_version++;
      operation.target.pixel_format=GML_RENDER_PIXEL_ARGB8888;
      CHECK(gml_render_application_scene_enqueue(render,&operation,&state,&source) &&
            fake.submits==0 && fake.readbacks==0);
      /* The deferred clear follows the queued draw, even before its version is published. */
      gml_render_set_pending_fill(render,0xFF26486Au);
      gml_render_flush_pending_fill(render);
      gml_render_resource_journal_metrics(resource,&after);
      CHECK(gml_render_dispatch_metrics(render,&dispatch) && !dispatch.failed &&
            !dispatch.unclassified && !dispatch.overflow &&
            dispatch.gpu[GML_RENDER_SCENE_SPRITE]==1u &&
            dispatch.gpu[GML_RENDER_SCENE_PRIMITIVE]==1u &&
            fake.submits==2 && fake.readbacks==0 && !render->pending_fill &&
            after.logical_version==before.logical_version+2u &&
            fake.last_opcode==GML_RENDER_OP_CLEAR &&
            fake.last_target_image.content_version==after.logical_version &&
            render->fb[0]==0u && render->fb[15]==0u);
      CHECK(after.authority==(on_demand?GML_RENDER_RESOURCE_GPU_RESIDENT:
                                        GML_RENDER_RESOURCE_GPU_REPLAYABLE));
      fake.readback_colour=0xFF26486Au;
      CHECK(gml_render_resource_acquire_cpu_read(resource,&pixels) &&
            pixels==render->fb && fake.readbacks==on_demand);
      for(int index=0;index<16;index++) CHECK(pixels[index]==0xFF26486Au);
      gml_render_free(render);
      free(render);
    }
  return 1;
}

static int first_generation_underlay_resource_borrow(void){
  GmlRender *cpu=(GmlRender*)calloc(1,sizeof *cpu);
  GmlRender *resident=(GmlRender*)calloc(1,sizeof *resident);
  GmlWin content={0};
  uint32_t cpu_screen[16],resident_screen[16];
  GmlRenderJournalMetrics before={0},after={0};
  GmlRenderApplicationReadView read_view={0};
  FakeDevice fake={0};
  GmlRenderDeviceServices device=service(&fake);
  CHECK(cpu && resident);
  content.bytecode=15;
  cpu->win=resident->win=&content;
  for(size_t i=0;i<16;i++)
    cpu_screen[i]=resident_screen[i]=0xFF203040u;
  gml_render_begin(cpu,cpu_screen,4,4,0.0,0.0);
  gml_render_begin(resident,resident_screen,4,4,0.0,0.0);
  cpu->color_write_mask=resident->color_write_mask=0x0F;
  CHECK(gml_render_application_surface_ensure_owned(cpu,4,4) &&
        gml_render_application_surface_ensure_owned(resident,4,4));
  gml_render_bind_device(resident,&device);
  CHECK(gml_render_begin_owned_application(cpu,0.0,0.0,0) &&
        gml_render_begin_resident_owned_application(resident,0.0,0.0,0));
  gml_render_set_pending_fill(cpu,0xFF8090A0u);
  gml_render_set_pending_fill(resident,0xFF8090A0u);
  gml_render_flush_pending_fill(cpu);
  gml_render_flush_pending_fill(resident);
  gml_render_resource_journal_metrics(resident->app_surface_resource,&before);
  CHECK(before.journal_operations==1u && before.cpu_replays==0u &&
        fake.submits==1 && fake.readbacks==0 &&
        resident->app_surface_owned[0]==0u);
  gml_render_begin(cpu,cpu_screen,4,4,0.0,0.0);
  gml_render_begin(resident,resident_screen,4,4,0.0,0.0);
  for(int variant=0;variant<2;variant++){
    GmlRender *render=variant?resident:cpu;
    render->app_presentation_coverage=malloc(16);
    render->app_presentation_alpha_scratch=malloc(16);
    CHECK(render->app_presentation_coverage && render->app_presentation_alpha_scratch);
    memset(render->app_presentation_coverage,255,16);
    render->app_presentation_coverage[0]=0;
    render->app_presentation_coverage[1]=128;
    render->app_presentation_coverage_capacity=16;
    render->app_presentation_coverage_active=1;
    gml_render_set_pending_underlay(render,0,0,4,4);
    gml_render_flush_pending_underlay(render);
  }
  gml_render_resource_journal_metrics(resident->app_surface_resource,&after);
  if(memcmp(cpu_screen,resident_screen,sizeof cpu_screen) ||
     cpu_screen[0]!=0xFF203040u || cpu_screen[2]!=0xFF8090A0u)
    fprintf(stderr,"first-generation underlay: CPU=%08x/%08x/%08x resident=%08x/%08x/%08x source=%08x replay=%llu\n",
            cpu_screen[0],cpu_screen[1],cpu_screen[2],
            resident_screen[0],resident_screen[1],resident_screen[2],
            resident->app_surface_owned[0],(unsigned long long)after.cpu_replays);
  CHECK(!memcmp(cpu_screen,resident_screen,sizeof cpu_screen) &&
        cpu_screen[0]==0xFF203040u && cpu_screen[2]==0xFF8090A0u);
  CHECK(gml_render_application_surface_owned_read(resident,&read_view) &&
        read_view.pixels==resident->app_surface_owned &&
        !memcmp(cpu->app_surface_owned,read_view.pixels,16u*sizeof(uint32_t)) &&
        read_view.pixels[0]==0xFF8090A0u &&
        after.logical_version==before.logical_version &&
        after.cpu_version==after.logical_version &&
        after.cpu_replays==before.cpu_replays+1u &&
        after.journal_operations==0u && fake.readbacks==0);
  CHECK(gml_render_begin_owned_application(cpu,0.0,0.0,0) &&
        gml_render_begin_resident_owned_application(resident,0.0,0.0,0));
  gml_render_set_pending_fill(cpu,0xFF405060u);
  gml_render_set_pending_fill(resident,0xFF405060u);
  gml_render_flush_pending_fill(cpu);
  gml_render_flush_pending_fill(resident);
  CHECK(resident->app_surface_owned[0]==0xFF8090A0u && fake.submits==2);
  gml_render_bind_device(resident,NULL);
  for(size_t i=0;i<16;i++) cpu_screen[i]=resident_screen[i]=0xFF203040u;
  gml_render_begin(cpu,cpu_screen,4,4,0.0,0.0);
  gml_render_begin(resident,resident_screen,4,4,0.0,0.0);
  cpu->app_presentation_coverage_active=resident->app_presentation_coverage_active=1;
  gml_render_set_pending_underlay(cpu,0,0,4,4);
  gml_render_set_pending_underlay(resident,0,0,4,4);
  gml_render_flush_pending_underlay(cpu);
  gml_render_flush_pending_underlay(resident);
  CHECK(!memcmp(cpu_screen,resident_screen,sizeof cpu_screen) &&
        !memcmp(cpu->app_surface_owned,resident->app_surface_owned,
                16u*sizeof(uint32_t)) &&
        resident->app_surface_owned[0]==0xFF405060u && fake.readbacks==0);
  gml_render_free(cpu); free(cpu);
  gml_render_free(resident); free(resident);
  return 1;
}

static int resident_layer_filter_capture_barrier(void){
  GmlRender *render=(GmlRender*)calloc(1,sizeof *render);
  GmlLayerFilter filter={0};
  GmlRenderDeviceServices device;
  GmlRenderJournalMetrics metrics={0};
  FakeDevice fake={0};
  CHECK(render);
  CHECK(gml_render_application_surface_ensure_owned(render,4,4));
  device=service(&fake);
  gml_render_bind_device(render,&device);
  filter.kind=GML_LAYER_FILTER_TINT;
  filter.affects_below=1;
  filter.u.tint.colour=0xFFFFFFFFu;
  CHECK(gml_render_begin_resident_owned_application(render,0.0,0.0,0));
  gml_render_set_pending_fill(render,0xFF445566u);
  gml_render_flush_pending_fill(render);
  CHECK(render->app_surface_owned[0]==0u && fake.readbacks==0);
  CHECK(gml_render_layer_filter_begin(render,&filter));
  CHECK(render->layer_filter_src[0]==0xFF445566u &&
        !render->resident_application_pass && fake.readbacks==0);
  fake.status=GML_RENDER_EXEC_UNSUPPORTED;
  gml_render_layer_filter_end(render,&filter,0.0);
  fake.status=0;
  CHECK(render->app_surface_owned[0]==0xFF445566u &&
        render->app_surface_owned[15]==0xFF445566u);
  gml_render_resource_journal_metrics(render->app_surface_resource,&metrics);
  CHECK(metrics.cpu_replays==1u && metrics.journal_operations==0u);

  filter.affects_below=0;
  CHECK(gml_render_begin_resident_owned_application(render,0.0,0.0,0));
  gml_render_set_pending_fill(render,0xFF0000FFu);
  gml_render_flush_pending_fill(render);
  CHECK(render->app_surface_owned[0]==0xFF445566u && fake.readbacks==0);
  CHECK(gml_render_layer_filter_begin(render,&filter));
  CHECK(render->layer_filter_src[0]==0u &&
        render->app_surface_owned[0]==0xFF0000FFu &&
        !render->resident_application_pass && fake.readbacks==0);
  render->fb[0]=0x80800000u;
  render->fb_all_transparent=0;
  fake.status=GML_RENDER_EXEC_UNSUPPORTED;
  gml_render_layer_filter_end(render,&filter,0.0);
  fake.status=0;
  CHECK(render->app_surface_owned[0]==0xFF80007Fu &&
        render->app_surface_owned[1]==0xFF0000FFu);
  {
    uint32_t base[16]={0};
    GmlRenderResource *resource;
    GmlRenderDrawOp operation;
    GmlRenderDrawBatch batch;
    int id;
    gml_render_begin(render,base,4,4,0.0,0.0);
    render->next_surface_id=1;
    id=gml_surface_create(render,4,4);
    CHECK(id>0 && gml_surface_set_target(render,id));
    resource=gml_render_surface_resource_ensure(render,id);
    CHECK(resource);
    operation=clear_op(2u,0xFF123456u,GML_RENDER_REPLAY_EXACT);
    operation.target.image=gml_render_resource_image_ref(resource);
    operation.target.image.content_version++;
    operation.target.pixel_format=GML_RENDER_PIXEL_ARGB8888;
    batch=(GmlRenderDrawBatch){&operation,1u,NULL,0u};
    CHECK(gml_render_surface_scene_submit(render,id,&batch,NULL,0u)==
          GML_RENDER_RESOURCE_EXEC_DEVICE);
    CHECK(render->surface[id-1].px[0]==0u);
    filter.affects_below=1;
    CHECK(gml_render_layer_filter_begin(render,&filter));
    CHECK(render->layer_filter_src[0]==0xFF123456u &&
          render->surface[id-1].px[0]==0xFF123456u && fake.readbacks==0);
    fake.status=GML_RENDER_EXEC_UNSUPPORTED;
    gml_render_layer_filter_end(render,&filter,0.0);
    fake.status=0;
    CHECK(render->surface[id-1].px[15]==0xFF123456u);
  }
  gml_render_free(render);
  free(render);
  return 1;
}

static int resident_inline_layer_effect_cpu_bridge(void){
  GmlRender *cpu=(GmlRender*)calloc(1,sizeof *cpu);
  GmlRender *resident=(GmlRender*)calloc(1,sizeof *resident);
  uint32_t cpu_pixels[16]={0};
  GmlRenderDeviceServices device;
  GmlRenderApplicationWriteView view={0};
  GmlRenderJournalMetrics metrics={0};
  FakeDevice fake={0};
  CHECK(cpu && resident);
  device=service(&fake);
  gml_render_begin(cpu,cpu_pixels,4,4,0.0,0.0);
  gml_render_set_pending_fill(cpu,0xFF445566u);
  CHECK(gml_render_application_surface_ensure_owned(resident,4,4));
  gml_render_bind_device(resident,&device);
  CHECK(gml_render_begin_resident_owned_application(resident,0.0,0.0,0));
  gml_render_set_pending_fill(resident,0xFF445566u);
  gml_render_flush_pending_fill(resident);
  CHECK(resident->app_surface_owned[0]==0u && fake.readbacks==0);
  /* Keep this case on the original CPU bridge when the new effect opcode is
   * refused; the real-EGL comparator owns the direct-GPU arithmetic proof. */
  fake.status=GML_RENDER_EXEC_UNSUPPORTED;
  gml_render_layer_rgb_noise(cpu,0u,0.625,0.375,0x80C040u);
  gml_render_layer_rgb_noise(resident,0u,0.625,0.375,0x80C040u);
  fake.status=0;
  CHECK(cpu_pixels[0]!=0xFF445566u &&
        !memcmp(resident->app_surface_owned,cpu_pixels,sizeof cpu_pixels) &&
        fake.readbacks==0);
  gml_render_layer_tint(cpu,0xC08040A0u);
  gml_render_layer_tint(resident,0xC08040A0u);
  CHECK(!memcmp(resident->app_surface_owned,cpu_pixels,sizeof cpu_pixels) &&
        gml_render_application_surface_owned_view(resident,&view) &&
        !memcmp(view.pixels,cpu_pixels,sizeof cpu_pixels) && fake.readbacks==0);
  gml_render_resource_journal_metrics(resident->app_surface_resource,&metrics);
  CHECK(metrics.journal_operations==0u && metrics.cpu_replays==1u);
  gml_render_bind_device(resident,NULL);
  CHECK(gml_render_application_surface_owned_view(resident,&view) &&
        !memcmp(view.pixels,cpu_pixels,sizeof cpu_pixels));
  gml_render_free(cpu); free(cpu);
  gml_render_free(resident); free(resident);
  {
    GmlRender *surface_cpu=(GmlRender*)calloc(1,sizeof *surface_cpu);
    GmlRender *surface_gpu=(GmlRender*)calloc(1,sizeof *surface_gpu);
    uint32_t base_cpu[16]={0},base_gpu[16]={0};
    GmlRenderResource *resource;
    GmlRenderDrawOp operation;
    GmlRenderDrawBatch batch;
    int cpu_id,gpu_id,width=0,height=0;
    uint32_t *expected,*actual;
    CHECK(surface_cpu && surface_gpu);
    surface_cpu->next_surface_id=surface_gpu->next_surface_id=1;
    gml_render_begin(surface_cpu,base_cpu,4,4,0.0,0.0);
    gml_render_begin(surface_gpu,base_gpu,4,4,0.0,0.0);
    cpu_id=gml_surface_create(surface_cpu,4,4);
    gpu_id=gml_surface_create(surface_gpu,4,4);
    CHECK(cpu_id>0 && gpu_id>0 &&
          gml_surface_set_target(surface_cpu,cpu_id) &&
          gml_surface_set_target(surface_gpu,gpu_id));
    gml_render_set_pending_fill(surface_cpu,0xFF315779u);
    gml_render_flush_pending_fill(surface_cpu);
    gml_render_bind_device(surface_gpu,&device);
    resource=gml_render_surface_resource_ensure(surface_gpu,gpu_id);
    CHECK(resource);
    operation=clear_op(2u,0xFF315779u,GML_RENDER_REPLAY_EXACT);
    operation.target.image=gml_render_resource_image_ref(resource);
    operation.target.image.content_version++;
    operation.target.pixel_format=GML_RENDER_PIXEL_ARGB8888;
    batch=(GmlRenderDrawBatch){&operation,1u,NULL,0u};
    CHECK(gml_render_surface_scene_submit(surface_gpu,gpu_id,&batch,NULL,0u)==
          GML_RENDER_RESOURCE_EXEC_DEVICE && surface_gpu->surface[gpu_id-1].px[0]==0u);
    gml_render_layer_rgb_noise(surface_cpu,0u,0.5,0.625,0xA050D0u);
    gml_render_layer_rgb_noise(surface_gpu,0u,0.5,0.625,0xA050D0u);
    expected=surface_pixels(surface_cpu,cpu_id,NULL,NULL);
    actual=surface_pixels(surface_gpu,gpu_id,&width,&height);
    CHECK(expected && actual && width==4 && height==4 &&
          !memcmp(actual,expected,sizeof cpu_pixels));
    gml_render_layer_tint(surface_cpu,0xD080C060u);
    gml_render_layer_tint(surface_gpu,0xD080C060u);
    CHECK(!memcmp(actual,expected,sizeof cpu_pixels));
    gml_render_bind_device(surface_gpu,NULL);
    CHECK(surface_pixels(surface_gpu,gpu_id,&width,&height)==actual &&
          !memcmp(actual,expected,sizeof cpu_pixels));
    gml_render_free(surface_cpu); free(surface_cpu);
    gml_render_free(surface_gpu); free(surface_gpu);
  }
  return 1;
}

/* A successful application-target effect must return to the resident draw
 * contract. Otherwise later CPU-only drawing changes canonical pixels while
 * the resource still advertises its pre-draw GPU image as current. */
static int resident_layer_effect_continuation_authority(void){
  for(int variant=0;variant<2;variant++){
    GmlRender *render=(GmlRender*)calloc(1,sizeof *render);
    GmlRenderDeviceServices device;
    GmlRenderJournalMetrics after_effect={0},after_cpu={0};
    GmlLayerFilter filter={0};
    FakeDevice fake={0};
    CHECK(render && gml_render_init(render,NULL)==0);
    fake.readback_colour=0xFF203040u;
    device=service(&fake);
    CHECK(gml_render_application_surface_ensure_owned(render,4,4));
    gml_render_bind_device(render,&device);
    CHECK(gml_render_begin_resident_owned_application(render,0.0,0.0,0));
    gml_render_set_pending_fill(render,0xFF102030u);
    if(!variant){
      gml_render_layer_rgb_noise(render,0u,0.5,0.375,0x80C040u);
    } else {
      filter.kind=GML_LAYER_FILTER_TINT;
      filter.affects_below=1;
      filter.u.tint.colour=0xFF80C040u;
      CHECK(gml_render_layer_filter_begin(render,&filter));
      gml_render_layer_filter_end(render,&filter,0.375);
    }
    CHECK(gml_render_application_surface_owned_read(render,NULL));
    gml_render_resource_journal_metrics(render->app_surface_resource,&after_effect);
    CHECK(fake.submits>=2 && fake.readbacks==1 &&
          render->resident_application_pass &&
          after_effect.authority==GML_RENDER_RESOURCE_BOTH_CURRENT &&
          after_effect.cpu_version==after_effect.gpu_version &&
          render->app_surface_owned[0]==fake.readback_colour);
    render->alpha=0.5;
    gml_render_primitive_line(render,0,0,3,3,0xFFFFFFu,1);
    gml_render_resource_journal_metrics(render->app_surface_resource,&after_cpu);
    CHECK(!render->resident_application_pass &&
          after_cpu.authority==GML_RENDER_RESOURCE_CPU_CURRENT &&
          after_cpu.logical_version==after_effect.logical_version+1u &&
          after_cpu.cpu_version==after_cpu.logical_version &&
          after_cpu.gpu_version==after_effect.gpu_version &&
          render->app_surface_owned[0]!=fake.readback_colour);
    gml_render_free(render);
    free(render);
  }
  return 1;
}

static int resident_numbered_layer_effect_target(void){
  for(unsigned family=0;family<3u;family++) for(unsigned below=0;below<2u;below++){
    GmlRender *render=calloc(1,sizeof *render);
    FakeDevice fake={.readback_colour=0xFF203040u};
    GmlRenderDeviceServices device=service(&fake);
    GmlLayerFilter filter={0};GmlRenderJournalMetrics effect={0},writer={0};
    CHECK(render && gml_render_init(render,NULL)==0);
    CHECK(gml_render_application_surface_ensure_owned(render,4,4));
    gml_render_bind_device(render,&device);
    CHECK(gml_render_begin_resident_owned_application(render,0,0,0));
    int id=gml_surface_create(render,4,4);
    CHECK(id>0 && gml_surface_set_target(render,id));
    gml_render_set_pending_fill(render,0xFF193557u);gml_render_flush_pending_fill(render);
    int submits=fake.submits;
    filter.kind=family==0u?GML_LAYER_FILTER_TINT:
      family==1u?GML_LAYER_FILTER_COLOURISE:GML_LAYER_FILTER_ZOOM_BLUR;
    filter.affects_below=(int)below;
    if(!family) filter.u.tint.colour=0xB0C080E0u;
    else if(family==1u){filter.u.colourise.tint_colour=0x0080C040u;filter.u.colourise.intensity=.65;}
    else {filter.u.zoom_blur.centre[0]=.25;filter.u.zoom_blur.centre[1]=.75;
      filter.u.zoom_blur.intensity=.85;filter.u.zoom_blur.focus_radius=.2;}
    CHECK(gml_render_layer_filter_begin(render,&filter) && render->layer_filter_gpu_candidate);
    render->fb[0]=0xA0403020u;render->fb_all_transparent=0;
    gml_render_layer_filter_end(render,&filter,.375);
    CHECK(fake.submits==submits+1 && fake.last_opcode==GML_RENDER_OP_LAYER_EFFECT &&
      fake.last_target_image.resource_class==GML_RENDER_RESOURCE_SURFACE &&
      fake.last_target_image.identity==(uint32_t)id && render->resident_application_pass &&
      fake.readbacks==1 && render->app_surface_owned[0]==0u);
    GmlRenderResource *resource=gml_render_surface_resource_ensure(render,id);
    gml_render_resource_journal_metrics(resource,&effect);
    CHECK(effect.authority==GML_RENDER_RESOURCE_BOTH_CURRENT &&
      effect.cpu_version==effect.logical_version && effect.gpu_version==effect.logical_version);
    render->alpha=.5;
    gml_render_primitive_line(render,0,0,3,3,0xFFFFFFu,1);
    gml_render_resource_journal_metrics(resource,&writer);
    CHECK(!render->resident_application_pass && writer.authority==GML_RENDER_RESOURCE_CPU_CURRENT &&
      writer.logical_version==effect.logical_version+1u && writer.cpu_version==writer.logical_version &&
      writer.gpu_version==effect.gpu_version && render->surface[id-1].px[0]!=fake.readback_colour);
    gml_render_free(render);free(render);
  }
  return 1;
}

static int resident_flat_primitive_public_order(void){
  GmlRender *cpu=(GmlRender*)calloc(1,sizeof *cpu);
  GmlRender *gpu=(GmlRender*)calloc(1,sizeof *gpu);
  uint32_t cpu_pixels[16]={0};
  FakeDevice fake={0};
  GmlRenderDeviceServices device=service(&fake);
  GmlRenderJournalMetrics metrics={0};
  CHECK(cpu && gpu);
  CHECK(gml_render_init(cpu,NULL)==0 && gml_render_init(gpu,NULL)==0);
  gml_render_begin(cpu,cpu_pixels,4,4,0.0,0.0);
  CHECK(gml_render_application_surface_ensure_owned(gpu,4,4));
  gml_render_bind_device(gpu,&device);
  CHECK(gml_render_begin_resident_owned_application(gpu,0.0,0.0,0));
  gml_render_set_pending_fill(cpu,0xFF102030u);
  gml_render_set_pending_fill(gpu,0xFF102030u);
  gml_render_dispatch_diagnostics_enable(cpu,1);
  gml_render_dispatch_diagnostics_enable(gpu,1);
  gml_render_primitive_rectangle(cpu,1,1,2,2,0x345678u,0);
  gml_render_primitive_rectangle(gpu,1,1,2,2,0x345678u,0);
  gml_render_primitive_rectangle_color(cpu,0,0,0,0,0xabcdefu,0xabcdefu,
                                        0xabcdefu,0xabcdefu,0);
  gml_render_primitive_rectangle_color(gpu,0,0,0,0,0xabcdefu,0xabcdefu,
                                        0xabcdefu,0xabcdefu,0);
  gml_render_primitive_point(cpu,3,3,0x0000ffu);
  gml_render_primitive_point(gpu,3,3,0x0000ffu);
  gml_render_primitive_line(cpu,0,3,2,3,0x00ff00u,1);
  gml_render_primitive_line(gpu,0,3,2,3,0x00ff00u,1);
  gml_render_primitive_line_color(cpu,3,0,3,2,0xff0000u,0xff0000u,1);
  gml_render_primitive_line_color(gpu,3,0,3,2,0xff0000u,0xff0000u,1);
  gml_render_resource_journal_metrics(gpu->app_surface_resource,&metrics);
  CHECK(gpu->resident_application_pass && fake.submits==6 && fake.readbacks==0 &&
        metrics.journal_operations==6u && metrics.cpu_replays==0u);
  GmlRenderDispatchMetrics dispatch_cpu={0},dispatch_gpu={0};
  CHECK(gml_render_dispatch_metrics(cpu,&dispatch_cpu) &&
        gml_render_dispatch_metrics(gpu,&dispatch_gpu) &&
        dispatch_cpu.cpu[GML_RENDER_SCENE_PRIMITIVE][GML_RENDER_DISPATCH_NO_DEVICE]==6u &&
        dispatch_gpu.gpu[GML_RENDER_SCENE_PRIMITIVE]==6u && !dispatch_gpu.unclassified &&
        !dispatch_gpu.cpu[GML_RENDER_SCENE_PRIMITIVE][GML_RENDER_DISPATCH_NO_DEVICE] &&
        !dispatch_gpu.complete);
  cpu->alpha=gpu->alpha=0.5;
  gml_render_primitive_line(cpu,0,0,3,3,0xffffffu,1);
  gml_render_primitive_line(gpu,0,0,3,3,0xffffffu,1);
  gml_render_flush_pending_fill(cpu);
  CHECK(!gpu->resident_application_pass && fake.readbacks==0 &&
        memcmp(cpu_pixels,gpu->app_surface_owned,sizeof cpu_pixels)==0);
  for(int index=0;index<3;index++){
    gml_render_primitive_point(cpu,index,0,0xabcdefu);
    gml_render_primitive_point(gpu,index,0,0xabcdefu);
  }
  CHECK(gml_render_dispatch_metrics(gpu,&dispatch_gpu) &&
        dispatch_gpu.cpu[GML_RENDER_SCENE_PRIMITIVE][GML_RENDER_DISPATCH_NO_SCENE]==3u &&
        dispatch_gpu.cpu[GML_RENDER_SCENE_PRIMITIVE][GML_RENDER_DISPATCH_UNQUALIFIED]==1u &&
        !fake.readbacks && !memcmp(cpu_pixels,gpu->app_surface_owned,sizeof cpu_pixels));
  gml_render_free(cpu);
  gml_render_free(gpu);
  free(cpu);
  free(gpu);
  return 1;
}

static int resident_classic_identity_filtered_underlay(void){
  static const int generations[]={530,600,700,800,810};
  for(unsigned generation=0;generation<5u;generation++)
  for(unsigned owned=0;owned<2u;owned++)
  for(unsigned interpolate=0;interpolate<2u;interpolate++)
  for(unsigned enlarged=0;enlarged<2u;enlarged++)
  for(unsigned replace=0;replace<2u;replace++)
  for(unsigned cutoff=0;cutoff<3u;cutoff++){
    GmlRender *render=calloc(1,sizeof *render);
    GmlWin content={.classic_version=generations[generation]};
    uint32_t source[16]={0},screen[64]={0};
    FakeDevice fake={0};
    GmlRenderDeviceServices device=service(&fake);
    CHECK(render && !gml_render_init(render,NULL));
    render->classic=1;render->win=&content;render->interp=(int)interpolate;
    render->presentation_deferral_enabled=1;
    gml_render_bind_device(render,&device);
    if(owned){
      CHECK(gml_render_application_surface_ensure_owned(render,4,4) &&
            gml_render_begin_resident_owned_application(render,0.0,0.0,0));
    } else {
      gml_render_application_surface_bind(render,source,4,4,1);
      CHECK(gml_render_begin_resident_base(render,source,4,4,0.0,0.0));
    }
    gml_render_set_pending_fill(render,0xFF315779u);
    gml_render_flush_pending_fill(render);
    if(owned) CHECK(gml_render_application_surface_select_owned(render,1));
    CHECK(fake.submits==1 && !fake.readbacks);
    int extent=enlarged?8:4;
    gml_render_begin(render,screen,extent,extent,0.0,0.0);
    render->blendmode=replace?6:0;
    render->alpha_test_enable=cutoff!=0u;
    render->alpha_test_ref=cutoff==2u?254u:0u;
    gml_render_set_pending_fill(render,0xFF203040u);
    gml_render_set_pending_underlay(render,0,0,extent,extent);
    int accepted=gml_render_defer_resident_underlay(render);
    int expected=generation<3u && (!interpolate || !enlarged);
    if(accepted!=expected)
      fprintf(stderr,"classic underlay generation=%d owned=%u interpolate=%u enlarged=%u accepted=%d expected=%d\n",
        generations[generation],owned,interpolate,enlarged,accepted,expected);
    CHECK(accepted==expected && !fake.readbacks);
    if(accepted){
      CHECK(!render->pending_underlay && render->pending_presentation &&
            render->presentation.resident_image.incarnation &&
            render->presentation.source_width==4 && render->presentation.source_height==4 &&
            render->presentation.linear==0 && fake.submits==1);
      gml_render_flush_deferred_presentation(render);
      CHECK(!render->pending_presentation && screen[0]==0xFF315779u &&
            screen[extent*extent-1]==0xFF315779u && !fake.readbacks);
    } else CHECK(render->pending_underlay && !render->pending_presentation);
    if(generation<3u && interpolate && !enlarged){
      for(unsigned rejected=0;rejected<10u;rejected++){
        int shader=render->active_shader,mode=render->blendmode;
        int rgb_equation=render->blend_equation,alpha_equation=render->blend_equation_alpha;
        int test=render->alpha_test_enable;
        uint8_t reference=render->alpha_test_ref,mask=render->color_write_mask;
        gml_render_set_pending_fill(render,0xFF203040u);
        gml_render_set_pending_underlay(render,0,0,4,4);
        if(rejected==0u) render->active_shader=0;
        else if(rejected<6u) render->blendmode=(int)rejected;
        else if(rejected==6u) render->blend_equation=2;
        else if(rejected==7u) render->blend_equation_alpha=2;
        else if(rejected==8u){render->alpha_test_enable=1;render->alpha_test_ref=255u;}
        else render->color_write_mask=7u;
        int refused=!gml_render_defer_resident_underlay(render);
        if(!refused || !render->pending_underlay || !render->pending_fill ||
           render->pending_presentation || fake.readbacks)
          fprintf(stderr,"identity underlay refusal: generation=%d owned=%u state=%u refused=%d "
            "underlay=%d fill=%d presentation=%d reads=%d\n",generations[generation],owned,
            rejected,refused,render->pending_underlay,render->pending_fill,
            render->pending_presentation,fake.readbacks);
        CHECK(refused && render->pending_underlay &&
              render->pending_fill && !render->pending_presentation && !fake.readbacks);
        render->active_shader=shader;render->blendmode=mode;
        render->blend_equation=rgb_equation;render->blend_equation_alpha=alpha_equation;
        render->alpha_test_enable=test;render->alpha_test_ref=reference;render->color_write_mask=mask;
      }
    }
    gml_render_free(render);free(render);
  }
  return 1;
}

static int resident_classic_rectangle_scope(void){
  for(unsigned variant=0;variant<3u;variant++){
    GmlRender *cpu=(GmlRender*)calloc(1,sizeof *cpu);
    GmlRender *gpu=(GmlRender*)calloc(1,sizeof *gpu);
    uint32_t cpu_pixels[64]={0},gpu_pixels[64]={0};
    GmlWin content={.classic_version=530};
    FakeDevice fake={0};
    GmlRenderDeviceServices device=service(&fake);
    CHECK(cpu && gpu && gml_render_init(cpu,NULL)==0 && gml_render_init(gpu,NULL)==0);
    cpu->classic=gpu->classic=1;
    cpu->win=gpu->win=&content;
    gml_render_begin(cpu,cpu_pixels,8,8,0.0,0.0);
    gml_render_application_surface_bind(gpu,gpu_pixels,8,8,1);
    gml_render_bind_device(gpu,&device);
    CHECK(gml_render_begin_resident_base(gpu,gpu_pixels,8,8,0.0,0.0));
    gml_render_set_pending_fill(cpu,0xFF102030u);
    gml_render_set_pending_fill(gpu,0xFF102030u);
    gml_render_primitive_rectangle(cpu,1,1,3,3,0x345678u,0);
    gml_render_primitive_rectangle(gpu,1,1,3,3,0x345678u,0);
    CHECK(gpu->resident_application_pass && fake.submits==2 && !fake.readbacks);
    if(variant==0u){
      gml_render_primitive_point(cpu,7,7,0x0000ffu);
      gml_render_primitive_point(gpu,7,7,0x0000ffu);
    } else if(variant==1u){
      gml_render_primitive_line(cpu,0,7,7,7,0x00ff00u,1);
      gml_render_primitive_line(gpu,0,7,7,7,0x00ff00u,1);
    } else {
      gml_render_primitive_line_color(cpu,7,0,7,7,0xff0000u,0xff0000u,1);
      gml_render_primitive_line_color(gpu,7,0,7,7,0xff0000u,0xff0000u,1);
    }
    gml_render_flush_pending_fill(cpu);
    CHECK(!gpu->resident_application_pass && fake.submits==2 && !fake.readbacks &&
          !memcmp(cpu_pixels,gpu_pixels,sizeof cpu_pixels));
    gml_render_free(cpu); gml_render_free(gpu);
    free(cpu); free(gpu);
  }
  return 1;
}

static int resident_classic_owned_rectangle_scope(void){
  static const int generations[]={530,600,700};
  for(unsigned owned=0;owned<2u;owned++)
  for(unsigned generation=0;generation<3u;generation++)
  for(unsigned variant=0;variant<14u;variant++){
    GmlRender *cpu=calloc(1,sizeof *cpu),*gpu=calloc(1,sizeof *gpu);
    GmlWin content={.classic_version=generations[generation]};
    FakeDevice fake={0};
    GmlRenderDeviceServices device=service(&fake);
    uint32_t reference[64]={0},screen[64]={0};
    const uint32_t *sealed=NULL;
    CHECK(cpu && gpu && !gml_render_init(cpu,NULL) && !gml_render_init(gpu,NULL));
    cpu->classic=gpu->classic=1;cpu->win=gpu->win=&content;
    gml_render_begin(cpu,reference,8,8,-3.0,2.0);
    gml_render_bind_device(gpu,&device);
    if(owned){
      CHECK(gml_render_application_surface_ensure_owned(gpu,8,8) &&
            gml_render_begin_resident_owned_application(gpu,-3.0,2.0,0));
    } else {
      gml_render_application_surface_bind(gpu,screen,8,8,1);
      CHECK(gml_render_begin_resident_base(gpu,screen,8,8,-3.0,2.0));
    }
    if(variant==6u) cpu->alpha=gpu->alpha=0.5;
    if(variant==7u) cpu->blendmode=gpu->blendmode=2;
    if(variant==8u) cpu->color_write_mask=gpu->color_write_mask=7u;
    if(variant==9u){gml_render_gui_begin(cpu,8,8);gml_render_gui_begin(gpu,8,8);}
    if(variant==10u) content.classic_version=800;
    if(variant==11u){
      int a=gml_surface_create(cpu,8,8),b=gml_surface_create(gpu,8,8);
      CHECK(a>0 && b>0 && gml_surface_set_target(cpu,a) && gml_surface_set_target(gpu,b));
    }
    if(variant==5u){
      cpu->world_transform_active=gpu->world_transform_active=1;
      cpu->world_scale_x=gpu->world_scale_x=-2.0;
      cpu->world_scale_y=gpu->world_scale_y=0.5;
    }
    gml_render_set_pending_fill(cpu,0xFF102030u);gml_render_set_pending_fill(gpu,0xFF102030u);
    for(unsigned side=0;side<2u;side++){
      GmlRender *r=side?gpu:cpu;
      if(variant==13u) gml_render_primitive_point(r,2,2,0x345678u);
      else if(variant==12u)
        gml_render_primitive_rectangle_color(r,1,1,5,4,0u,0xFFFFFFu,0u,0xFFFFFFu,0);
      else if(variant==1u || variant==3u)
        gml_render_primitive_rectangle_color(r,1,1,5,4,0x345678u,
          variant==3u?0u:0x345678u,variant==3u?0xFFFFFFu:0x345678u,
          variant==3u?0x0000FFu:0x345678u,variant==3u);
      else
        gml_render_primitive_rectangle(r,variant==4u?5:-2,variant==4u?4:1,
          variant==4u?-2:5,variant==4u?1:4,0x345678u,variant==2u || variant==5u);
    }
    CHECK(gpu->resident_application_pass==(variant<6u));
    gml_render_flush_pending_fill(cpu);
    if(variant<6u){
      GmlRenderResource *target=gml_render_scene_target_resource_ensure(gpu);
      CHECK(target && fake.submits && !fake.readbacks &&
            fake.last_opcode==GML_RENDER_OP_COLOURED_PRIMITIVE &&
            fake.last_target_image.resource_class==
              (owned?GML_RENDER_RESOURCE_APPLICATION:GML_RENDER_RESOURCE_SCREEN) &&
            gml_render_resource_acquire_cpu_read(target,&sealed) &&
            !memcmp(cpu->fb,sealed,sizeof reference) && !fake.readbacks);
    } else CHECK(!memcmp(cpu->fb,gpu->fb,sizeof reference));
    gml_render_free(cpu);gml_render_free(gpu);free(cpu);free(gpu);
  }
  return 1;
}

static int resident_rectangle_function_matrix(void){
  static const int generations[]={530,700,800,13,15,17};
  static const double alphas[]={0.0,1.0,0.5,0.4,1.0/256.0,1.0/1024.0};
  for(unsigned generation=0;generation<6u;generation++)
  for(unsigned preset=0;preset<12u;preset++)
  for(unsigned cutout=0;cutout<2u;cutout++)
  for(unsigned enabled=0;enabled<2u;enabled++)
  for(unsigned target_kind=0;target_kind<4u;target_kind++)
  for(unsigned gui=0;gui<2u;gui++)
  for(unsigned alpha=0;alpha<6u;alpha++)
  for(unsigned port=0;port<3u;port++)
  for(unsigned outline=0;outline<2u;outline++){
    GmlRender *cpu=calloc(1,sizeof *cpu),*gpu=calloc(1,sizeof *gpu);
    GmlWin content={0};
    AnygmCompatibilityProfile profile={0};
    AnygmContentFacts facts={.schema_version=ANYGM_COMPATIBILITY_SCHEMA};
    if(generations[generation]>100) facts.classic_revision=generations[generation];
    else facts.bytecode_revision=generations[generation];
    CHECK(anygm_compatibility_resolve(&facts,&profile,NULL,0));
    content.compatibility=&profile;content.classic_version=facts.classic_revision;
    content.bytecode=facts.bytecode_revision;
    FakeDevice fake={.on_demand=1};
    GmlRenderDeviceServices device=service(&fake);
    device.observe_on_demand=fake_observe_on_demand;
    uint32_t reference[64]={0},screen[64]={0},replayed[64];
    CHECK(cpu && gpu && !gml_render_init(cpu,NULL) && !gml_render_init(gpu,NULL));
    cpu->classic=gpu->classic=facts.classic_revision!=0;cpu->win=gpu->win=&content;
    gml_render_begin(cpu,reference,8,8,-3.0,2.0);
    gml_render_bind_device(gpu,&device);
    if(target_kind==1u || target_kind==2u){
      CHECK(gml_render_application_surface_ensure_owned(cpu,8,8) &&
            gml_render_begin_owned_application(cpu,-3.0,2.0,0));
      CHECK(gml_render_application_surface_ensure_owned(gpu,8,8) &&
            gml_render_begin_resident_owned_application(gpu,-3.0,2.0,0));
    } else {
      gml_render_application_surface_bind(cpu,reference,8,8,1);
      gml_render_application_surface_bind(gpu,screen,8,8,1);
      CHECK(gml_render_begin_resident_base(gpu,screen,8,8,-3.0,2.0));
    }
    if(target_kind==2u)
      CHECK(gml_surface_set_target(cpu,0) && gml_surface_set_target(gpu,0));
    if(target_kind==3u){
      int a=gml_surface_create(cpu,8,8),b=gml_surface_create(gpu,8,8);
      CHECK(a>0 && b>0 && gml_surface_set_target(cpu,a) && gml_surface_set_target(gpu,b));
    }
    if(gui){gml_render_gui_begin(cpu,4,4);gml_render_gui_begin(gpu,4,4);}
    cpu->world_transform_active=gpu->world_transform_active=1;
    cpu->world_scale_x=gpu->world_scale_x=-2.0;
    cpu->world_scale_y=gpu->world_scale_y=2.0;
    cpu->alpha=gpu->alpha=alphas[alpha];
    cpu->blendmode=gpu->blendmode=(int)preset;
    cpu->alphablend=gpu->alphablend=(int)enabled;
    cpu->alpha_test_enable=gpu->alpha_test_enable=(int)cutout;
    cpu->alpha_test_ref=gpu->alpha_test_ref=255u;
    const uint32_t background=0x40102030u;
    gml_render_set_pending_fill(cpu,background);gml_render_set_pending_fill(gpu,background);
    gml_render_flush_pending_fill(cpu);gml_render_flush_pending_fill(gpu);
    CHECK(gml_render_application_scene_flush(gpu));
    for(unsigned p=0;p<64u;p++) replayed[p]=background;
    int before=fake.submits;
    for(unsigned side=0;side<2u;side++){
      GmlRender *r=side?gpu:cpu;
      if(port) gml_render_primitive_rectangle_color(r,6,5,-2,1,0x345678u,
        port==2u?0xFFB305u:0x345678u,port==2u?0x00FCADu:0x345678u,
        port==2u?0xFF0000u:0x345678u,(int)outline);
      else gml_render_primitive_rectangle(r,6,5,-2,1,0x345678u,(int)outline);
    }
    int resident=gpu->resident_application_pass,flushed=gml_render_application_scene_flush(gpu);
    if(!resident || fake.readbacks || !flushed)
      fprintf(stderr,"rectangle matrix admission: generation=%d target=%u gui=%u alpha=%u port=%u outline=%u resident=%d reads=%d flush=%d\n",
        generations[generation],target_kind,gui,alpha,port,outline,resident,fake.readbacks,flushed);
    CHECK(resident && !fake.readbacks && flushed);
    CHECK(cpu->blendmode==(int)preset && gpu->blendmode==(int)preset &&
          cpu->alpha_test_enable==(int)cutout && gpu->alpha_test_enable==(int)cutout &&
          cpu->alpha_test_ref==255u && gpu->alpha_test_ref==255u);
    gml_render_flush_pending_fill(cpu);
    if(fake.submits>before && fake.last_primitive.payload.primitive.kind==GML_RENDER_PRIMITIVE_RECTANGLE){
      int valid=gml_render_rectangle_valid(&fake.last_primitive);
      int decoded=valid && gml_render_cpu_replay_one(replayed,8u,8u,&fake.last_primitive,NULL,NULL);
      int equal=decoded && !memcmp(cpu->fb,replayed,sizeof replayed);
      if(!equal){
        fprintf(stderr,"rectangle replay: generation=%d preset=%u cutout=%u enabled=%u target=%u gui=%u alpha=%u port=%u outline=%u flags=%u valid=%d decoded=%d\n",
          generations[generation],preset,cutout,enabled,target_kind,gui,alpha,port,outline,
          fake.last_primitive.payload.primitive.rectangle_flags,valid,decoded);
        for(unsigned p=0;decoded && p<64u;p++) if(cpu->fb[p]!=replayed[p]){
          fprintf(stderr,"rectangle replay: pixel=%u CPU=%08x replay=%08x\n",p,cpu->fb[p],replayed[p]);break;
        }
      }
      CHECK(valid && decoded && equal);
      GmlRenderDrawOp malformed=fake.last_primitive;
      malformed.payload.primitive.authored_region.width=65537u;
      CHECK(!gml_render_rectangle_valid(&malformed));
      malformed=fake.last_primitive;malformed.payload.primitive.alpha=NAN;
      CHECK(!gml_render_rectangle_valid(&malformed));
      malformed=fake.last_primitive;
      malformed.payload.primitive.rectangle_flags^=GML_RENDER_RECTANGLE_GRADIENT;
      CHECK(!gml_render_rectangle_valid(&malformed));
      malformed=fake.last_primitive;
      malformed.payload.primitive.rectangle_flags|=512u;
      CHECK(!gml_render_rectangle_valid(&malformed));
      malformed=fake.last_primitive;
      malformed.payload.primitive.rectangle_flags=
        (malformed.payload.primitive.rectangle_flags&~GML_RENDER_RECTANGLE_BLEND_MASK)|
        (12u<<GML_RENDER_RECTANGLE_BLEND_SHIFT);
      CHECK(!gml_render_rectangle_valid(&malformed));
    }
    gml_render_free(cpu);gml_render_free(gpu);free(cpu);free(gpu);
  }
  return 1;
}

static int resident_circle_pixel_packets(void){
  static const double alphas[]={1.0,0.61,1.0/1024.0};
  for(unsigned blend=0;blend<12u;blend++) for(unsigned mask=0;mask<16u;mask++)
  for(unsigned alpha=0;alpha<3u;alpha++) for(unsigned offscreen=0;offscreen<2u;offscreen++){
    uint32_t expected[12],actual[12];
    for(unsigned i=0;i<12u;i++) expected[i]=actual[i]=0x78273859u;
    GmlRender *cpu=calloc(1,sizeof *cpu);CHECK(cpu);
    cpu->fb=cpu->base_fb=expected;cpu->fbw=4;cpu->fbh=3;
    cpu->target_sp=(int)offscreen;cpu->alpha=alphas[alpha];cpu->alphablend=1;
    cpu->blendmode=(int)blend;cpu->color_write_mask=mask;
    cpu->blend_equation=cpu->blend_equation_alpha=1;cpu->active_shader=-1;
    GmlRenderDrawOp op={0};
    op.opcode=GML_RENDER_OP_COLOURED_PRIMITIVE;op.scene_family=GML_RENDER_SCENE_PRIMITIVE;
    op.reconstruction_mode=GML_RENDER_DEVICE_CHECKPOINT;
    op.target.width=4u;op.target.height=3u;op.target.region=(GmlRenderRect){0,0,4,3};
    op.target.pixel_format=GML_RENDER_PIXEL_ARGB8888;
    op.target.image.resource_class=offscreen?GML_RENDER_RESOURCE_SURFACE:GML_RENDER_RESOURCE_SCREEN;
    GmlRenderPrimitiveRect *p=&op.payload.primitive;
    p->kind=GML_RENDER_PRIMITIVE_RECTANGLE;p->region=p->authored_region=(GmlRenderRect){1,1,2,1};
    p->alpha=alphas[alpha];p->alpha_blend=1u;
    p->pixel_write_mask=blend==4u || blend==7u || blend==8u || blend>=9u?mask:15u;
    unsigned canonical_blend=blend==3u || blend==6u?2u:blend;
    p->rectangle_flags=GML_RENDER_RECTANGLE_OUTLINE|GML_RENDER_RECTANGLE_PIXEL_SPAN|
      (offscreen?GML_RENDER_RECTANGLE_TARGET_ALPHA:0u)|(canonical_blend<<GML_RENDER_RECTANGLE_BLEND_SHIFT);
    for(unsigned c=0;c<4u;c++) p->corner_colour[c]=0xFF274DB9u;
    p->colour=p->corner_colour[0];
    CHECK(gml_render_rectangle_valid(&op));
    for(int x=1;x<=2;x++) gml_render_cpu_backend_draw_pixel_alpha(cpu,x,1,0xB94D27u,alphas[alpha]);
    CHECK(gml_render_cpu_replay_one(actual,4u,3u,&op,NULL,NULL) && !memcmp(expected,actual,sizeof actual));
    GmlRenderDrawOp bad=op;bad.payload.primitive.pixel_write_mask=16u;
    CHECK(!gml_render_rectangle_valid(&bad));
    bad=op;bad.payload.primitive.region.height=bad.payload.primitive.authored_region.height=2u;
    CHECK(!gml_render_rectangle_valid(&bad));
    bad=op;bad.payload.primitive.rectangle_flags&=~GML_RENDER_RECTANGLE_OUTLINE;
    CHECK(!gml_render_rectangle_valid(&bad));
    bad=op;bad.payload.primitive.corner_colour[1]^=1u;
    CHECK(!gml_render_rectangle_valid(&bad));
    bad=op;bad.payload.primitive.rectangle_flags&=~GML_RENDER_RECTANGLE_PIXEL_SPAN;
    if(p->pixel_write_mask) CHECK(!gml_render_rectangle_valid(&bad));
    free(cpu);
  }
  return 1;
}

static int resident_rectangle_outline_coalesced(void){
  static const int generations[]={530,600,700,15,17};
  static const int bounds[][4]={
    {1,1,6,6},{6,6,1,1},{3,1,3,6},{1,3,6,3},{3,3,3,3},
    {-2,1,6,6},{1,-2,6,6},{1,1,10,6},{1,1,6,10},
    {-2,-2,6,6},{-2,-2,10,10},{-4,1,-2,6},{1,1,6,2},
    {1,1,2,6},{-2,3,10,3}
  };
  static const uint32_t backgrounds[]={0x00102030u,0x7F102030u,0xFF102030u};
  for(unsigned generation=0;generation<5u;generation++)
  for(unsigned owned=0;owned<2u;owned++)
  for(unsigned alpha=0;alpha<3u;alpha++)
  for(unsigned port=0;port<2u;port++)
  for(unsigned shape=0;shape<sizeof bounds/sizeof bounds[0];shape++){
    GmlRender *cpu=calloc(1,sizeof *cpu),*gpu=calloc(1,sizeof *gpu);
    GmlWin content={0};
    FakeDevice fake={0};
    GmlRenderDeviceServices device=service(&fake);
    uint32_t reference[64]={0},screen[64]={0};
    const uint32_t *sealed=NULL;
    CHECK(cpu && gpu && !gml_render_init(cpu,NULL) && !gml_render_init(gpu,NULL));
    cpu->classic=gpu->classic=generation<3u;
    if(generation<3u) content.classic_version=generations[generation];
    else content.bytecode=generations[generation];
    cpu->win=gpu->win=&content;
    gml_render_begin(cpu,reference,8,8,0.0,0.0);
    gml_render_bind_device(gpu,&device);
    if(owned) CHECK(gml_render_application_surface_ensure_owned(gpu,8,8) &&
                   gml_render_begin_resident_owned_application(gpu,0.0,0.0,0));
    else {
      gml_render_application_surface_bind(gpu,screen,8,8,1);
      CHECK(gml_render_begin_resident_base(gpu,screen,8,8,0.0,0.0));
    }
    gml_render_set_pending_fill(cpu,backgrounds[alpha]);
    gml_render_set_pending_fill(gpu,backgrounds[alpha]);
    gml_render_primitive_rectangle(cpu,0,0,4,4,0x876543u,0);
    gml_render_primitive_rectangle(gpu,0,0,4,4,0x876543u,0);
    int previous=fake.submits;
    for(unsigned side=0;side<2u;side++){
      GmlRender *r=side?gpu:cpu;
      const int *b=bounds[shape];
      if(port) gml_render_primitive_rectangle_color(r,b[0],b[1],b[2],b[3],
                      0x345678u,0xFF0000u,0x00FF00u,0x0000FFu,1);
      else gml_render_primitive_rectangle(r,b[0],b[1],b[2],b[3],0x345678u,1);
    }
    if(!gpu->resident_application_pass || fake.readbacks ||
       (fake.submits!=previous && fake.last_batch_operations>4u))
      fprintf(stderr,"rectangle coalescing: generation=%u owned=%u alpha=%u port=%u shape=%u resident=%d submits=%d/%d operations=%u reads=%d\n",
          generation,owned,alpha,port,shape,gpu->resident_application_pass,
          fake.submits,previous,fake.last_batch_operations,fake.readbacks);
    CHECK(gpu->resident_application_pass && !fake.readbacks &&
          (fake.submits==previous || fake.last_batch_operations<=4u));
    gml_render_primitive_rectangle(cpu,2,2,4,3,0xABCDEFu,0);
    gml_render_primitive_rectangle(gpu,2,2,4,3,0xABCDEFu,0);
    gml_render_flush_pending_fill(cpu);
    gml_render_bind_device(gpu,NULL);
    GmlRenderResource *target=gml_render_scene_target_resource_ensure(gpu);
    CHECK(target && gml_render_resource_acquire_cpu_read(target,&sealed) &&
          !memcmp(reference,sealed,sizeof reference) && !fake.readbacks);
    gml_render_free(cpu);gml_render_free(gpu);free(cpu);free(gpu);
  }
  /* Retain the former emitted-pixel budget, not merely a four-region budget. */
  for(unsigned over=0;over<2u;over++){
    unsigned width=4096u,height=2u+over;
    GmlRender *cpu=calloc(1,sizeof *cpu),*gpu=calloc(1,sizeof *gpu);
    FakeDevice fake={0};
    GmlRenderDeviceServices device=service(&fake);
    uint32_t *reference=calloc(width*height,sizeof *reference);
    const uint32_t *sealed=NULL;
    CHECK(cpu && gpu && reference && !gml_render_init(cpu,NULL) && !gml_render_init(gpu,NULL));
    gml_render_begin(cpu,reference,(int)width,(int)height,0.0,0.0);
    gml_render_bind_device(gpu,&device);
    CHECK(gml_render_application_surface_ensure_owned(gpu,(int)width,(int)height) &&
          gml_render_begin_resident_owned_application(gpu,0.0,0.0,0));
    gml_render_set_pending_fill(cpu,0x7F102030u);
    gml_render_set_pending_fill(gpu,0x7F102030u);
    gml_render_primitive_rectangle(cpu,-(int)over,0,(int)width-1,(int)height-1,0x345678u,1);
    gml_render_primitive_rectangle(gpu,-(int)over,0,(int)width-1,(int)height-1,0x345678u,1);
    CHECK(gpu->resident_application_pass==!over && !fake.readbacks);
    if(!over) CHECK(fake.last_batch_operations==2u);
    gml_render_flush_pending_fill(cpu);
    gml_render_bind_device(gpu,NULL);
    GmlRenderResource *target=gml_render_scene_target_resource_ensure(gpu);
    CHECK(target && gml_render_resource_acquire_cpu_read(target,&sealed) &&
          !memcmp(reference,sealed,width*height*sizeof *reference) && !fake.readbacks);
    gml_render_free(cpu);gml_render_free(gpu);free(cpu);free(gpu);free(reference);
  }
  return 1;
}

static int resident_studio_gui_rectangle_scope(void){
  for(unsigned generation=0;generation<2u;generation++)
  for(unsigned variant=0;variant<9u;variant++){
    GmlRender *cpu=calloc(1,sizeof *cpu),*gpu=calloc(1,sizeof *gpu);
    GmlWin content={.bytecode=generation?17:15};
    FakeDevice fake={0};
    GmlRenderDeviceServices device=service(&fake);
    uint32_t reference[96]={0},screen[96]={0};
    const uint32_t *sealed=NULL;
    CHECK(cpu && gpu && !gml_render_init(cpu,NULL) && !gml_render_init(gpu,NULL));
    cpu->win=gpu->win=&content;
    gml_render_begin(cpu,reference,12,8,-3.0,2.0);
    gml_render_application_surface_bind(gpu,screen,12,8,1);
    gml_render_bind_device(gpu,&device);
    CHECK(gml_render_begin_resident_base(gpu,screen,12,8,-3.0,2.0));
    gml_render_gui_begin(cpu,10,6);gml_render_gui_begin(gpu,10,6);
    gml_render_gui_set_size(cpu,20,18);gml_render_gui_set_size(gpu,20,18);
    cpu->world_transform_active=gpu->world_transform_active=1;
    cpu->world_scale_x=gpu->world_scale_x=2.0;
    cpu->world_scale_y=gpu->world_scale_y=3.0;
    gml_render_set_pending_fill(cpu,0xFF102030u);
    gml_render_set_pending_fill(gpu,0xFF102030u);
    if(variant==3u) cpu->alpha=gpu->alpha=0.5;
    if(variant==4u){cpu->classic=gpu->classic=1;content.classic_version=530;}
    if(variant==8u) cpu->color_write_mask=gpu->color_write_mask=7u;
    if(variant==7u){
      gml_render_primitive_point(cpu,2,2,0x345678u);
      gml_render_primitive_point(gpu,2,2,0x345678u);
    } else if(variant==6u){
      gml_render_primitive_rectangle_color(cpu,1,1,4,3,0u,0xFFFFFFu,0u,0xFFFFFFu,0);
      gml_render_primitive_rectangle_color(gpu,1,1,4,3,0u,0xFFFFFFu,0u,0xFFFFFFu,0);
    } else if(variant==1u){
      gml_render_primitive_rectangle_color(cpu,4,3,-2,1,0x345678u,0x345678u,0x345678u,0x345678u,0);
      gml_render_primitive_rectangle_color(gpu,4,3,-2,1,0x345678u,0x345678u,0x345678u,0x345678u,0);
    } else {
      int far_x=variant==2u?11:4,far_y=variant==2u?7:3;
      int near_x=variant==2u?0:1,near_y=variant==2u?0:1;
      gml_render_primitive_rectangle(cpu,near_x,near_y,far_x,far_y,0x345678u,variant==5u);
      gml_render_primitive_rectangle(gpu,near_x,near_y,far_x,far_y,0x345678u,variant==5u);
    }
    CHECK(gpu->resident_application_pass==(variant<3u));
    if(variant<3u){
      GmlRenderResource *target=gml_render_scene_target_resource_ensure(gpu);
      CHECK(target && gpu->gui_pass_active && fake.submits && !fake.readbacks &&
            fake.last_opcode==GML_RENDER_OP_COLOURED_PRIMITIVE &&
            fake.last_primitive.payload.primitive.kind==GML_RENDER_PRIMITIVE_FLAT);
      gml_render_flush_pending_fill(cpu);
      CHECK(gml_render_resource_acquire_cpu_read(target,&sealed) &&
            !memcmp(reference,sealed,sizeof reference) && !fake.readbacks);
    }
    gml_render_free(cpu);gml_render_free(gpu);free(cpu);free(gpu);
  }
  return 1;
}

static int resident_classic_alpha_rectangle_scope(void){
  static const double alphas[]={0.2,0.4,0.6000000000000001,0.8,
                                 0.07692307692307693,0.00392156862745098};
  for(unsigned variant=0;variant<sizeof alphas/sizeof alphas[0];variant++){
    GmlRender *cpu=(GmlRender*)calloc(1,sizeof *cpu);
    GmlRender *gpu=(GmlRender*)calloc(1,sizeof *gpu);
    GmlWin content={.classic_version=530};
    FakeDevice fake={.on_demand=1};
    GmlRenderDeviceServices device=service(&fake);
    uint32_t cpu_pixels[64],gpu_pixels[64],replayed[64];
    device.observe_on_demand=fake_observe_on_demand;
    CHECK(cpu && gpu && gml_render_init(cpu,NULL)==0 && gml_render_init(gpu,NULL)==0);
    for(unsigned i=0;i<64u;i++) cpu_pixels[i]=gpu_pixels[i]=replayed[i]=
      0xFF000000u|((i*9317u)&0xFFFFFFu);
    cpu->classic=gpu->classic=1;
    cpu->win=gpu->win=&content;
    gml_render_begin(cpu,cpu_pixels,8,8,0.0,0.0);
    gml_render_application_surface_bind(gpu,gpu_pixels,8,8,1);
    gml_render_bind_device(gpu,&device);
    CHECK(gml_render_begin_resident_base(gpu,gpu_pixels,8,8,0.0,0.0));
    gpu->fb_opaque_known=gpu->fb_all_opaque=1;
    cpu->alpha=gpu->alpha=alphas[variant];
    gml_render_primitive_rectangle(cpu,6,5,-2,1,0x345678u,0);
    gml_render_primitive_rectangle(gpu,6,5,-2,1,0x345678u,0);
    CHECK(gpu->resident_application_pass && !fake.submits && !fake.readbacks);
    CHECK(gml_render_application_scene_flush(gpu) && fake.submits==1 && !fake.readbacks);
    CHECK(gml_render_classic_alpha_rect_valid(&fake.last_primitive));
    CHECK(gml_render_cpu_replay_one(replayed,8u,8u,&fake.last_primitive,NULL,NULL));
    CHECK(!memcmp(cpu_pixels,replayed,sizeof replayed));
    for(unsigned rectangle=0;rectangle<3u;rectangle++)
      gml_render_primitive_rectangle(gpu,rectangle,0,rectangle+1,2,0x56789au,0);
    CHECK(gpu->resident_application_pass && fake.submits==1 && !fake.readbacks);
    CHECK(gml_render_application_scene_flush(gpu) && fake.submits==2 &&
          fake.last_batch_operations==3u && !fake.readbacks);
    GmlRenderDrawOp malformed=fake.last_primitive;
    malformed.payload.primitive.alpha=NAN;
    CHECK(!gml_render_classic_alpha_rect_valid(&malformed) &&
          !gml_render_cpu_replay_one(replayed,8u,8u,&malformed,NULL,NULL));
    malformed=fake.last_primitive;
    malformed.target.image.resource_class=GML_RENDER_RESOURCE_SURFACE;
    CHECK(!gml_render_classic_alpha_rect_valid(&malformed));
    gml_render_free(cpu); gml_render_free(gpu); free(cpu); free(gpu);
  }
  return 1;
}

static int resident_studio_alpha_rectangle_scope_variant(int identity_gui){
  static const double alphas[]={1.0/256.0,1.0/255.0,1.0/13.0,0.2,
                               0.6000000000000001,0.8,0.995,0.9975};
  for(unsigned generation=13u;generation<=16u;generation++)
  for(unsigned owned=0;owned<2u;owned++)
  for(unsigned port=0;port<2u;port++)
  for(unsigned variant=0;variant<sizeof alphas/sizeof alphas[0];variant++){
    GmlRender *cpu=calloc(1,sizeof *cpu),*gpu=calloc(1,sizeof *gpu);
    GmlWin content={.bytecode=generation};
    AnygmCompatibilityProfile profile={0};
    AnygmContentFacts facts={.schema_version=ANYGM_COMPATIBILITY_SCHEMA,
                            .bytecode_revision=generation};
    FakeDevice fake={.on_demand=1};
    GmlRenderDeviceServices device=service(&fake);
    uint32_t reference[64],screen[64],replayed[64];
    device.observe_on_demand=fake_observe_on_demand;
    CHECK(cpu && gpu && !gml_render_init(cpu,NULL) && !gml_render_init(gpu,NULL));
    CHECK(anygm_compatibility_resolve(&facts,&profile,NULL,0));
    content.compatibility=&profile;
    cpu->win=gpu->win=&content;
    for(unsigned i=0;i<64u;i++) reference[i]=screen[i]=replayed[i]=
      (variant%2u?(i%3u)*127u<<24:0xFF000000u)|((i*9317u)&0xFFFFFFu);
    gml_render_begin(cpu,reference,8,8,0.0,0.0);
    gml_render_bind_device(gpu,&device);
    if(owned){
      CHECK(gml_render_application_surface_ensure_owned(gpu,8,8));
      memcpy(gpu->app_surface_owned,screen,sizeof screen);
      CHECK(gml_render_begin_resident_owned_application(gpu,0.0,0.0,0));
    }else{
      gml_render_application_surface_bind(gpu,screen,8,8,1);
      CHECK(gml_render_begin_resident_base(gpu,screen,8,8,0.0,0.0));
    }
    if(identity_gui){gml_render_gui_begin(cpu,8,8);gml_render_gui_begin(gpu,8,8);}
    cpu->alpha=gpu->alpha=identity_gui && variant==7u?0.88:alphas[variant];
    gpu->fb_opaque_known=gpu->fb_all_opaque=variant%2u==0u;
    gpu->world_transform_active=1;
    gpu->world_scale_x=gpu->world_scale_y=2.0;
    for(unsigned side=0;side<2u;side++){
      GmlRender *render=side?gpu:cpu;
      if(port) gml_render_primitive_rectangle_color(render,6,5,-2,1,
        0x345678u,0x345678u,0x345678u,0x345678u,0);
      else gml_render_primitive_rectangle(render,6,5,-2,1,0x345678u,0);
    }
    CHECK(gpu->resident_application_pass && !fake.submits && !fake.readbacks);
    CHECK(gml_render_application_scene_flush(gpu) && fake.submits==1 && !fake.readbacks);
    CHECK(gml_render_studio_alpha_rect_valid(&fake.last_primitive) &&
          !gml_render_classic_alpha_rect_valid(&fake.last_primitive));
    CHECK(!gpu->fb_all_transparent &&
          (variant%2u || (gpu->fb_opaque_known && gpu->fb_all_opaque)));
    CHECK(gml_render_cpu_replay_one(replayed,8u,8u,&fake.last_primitive,NULL,NULL) &&
          !memcmp(reference,replayed,sizeof reference));
    for(unsigned rectangle=0;rectangle<8u;rectangle++)
      gml_render_primitive_rectangle(gpu,rectangle,0,rectangle,2,0x56789au,0);
    CHECK(gpu->resident_application_pass && fake.submits==1 && !fake.readbacks);
    CHECK(gml_render_application_scene_flush(gpu) && fake.submits==2 &&
          fake.last_batch_operations==8u && !fake.readbacks);
    GmlRenderDrawOp malformed=fake.last_primitive;
    malformed.payload.primitive.alpha=NAN;
    CHECK(!gml_render_studio_alpha_rect_valid(&malformed) &&
          !gml_render_cpu_replay_one(replayed,8u,8u,&malformed,NULL,NULL));
    malformed=fake.last_primitive;
    malformed.payload.primitive.alpha=1.0/512.0;
    CHECK(!gml_render_studio_alpha_rect_valid(&malformed));
    malformed=fake.last_primitive;
    malformed.target.image.resource_class=GML_RENDER_RESOURCE_SURFACE;
    CHECK(!gml_render_studio_alpha_rect_valid(&malformed));
    gml_render_free(cpu);gml_render_free(gpu);free(cpu);free(gpu);
  }
  /* Paired function controls supersede ordinary preset exclusions as well as
   * generation, GUI and gradient fences. Pending composition keeps its gate. */
  for(unsigned variant=0;variant<5u;variant++){
    GmlRender *gpu=calloc(1,sizeof *gpu);
    GmlWin content={.bytecode=15,.has_room_layers=variant==0u};
    AnygmCompatibilityProfile profile={0};
    AnygmContentFacts facts={.schema_version=ANYGM_COMPATIBILITY_SCHEMA,
      .bytecode_revision=15,.has_room_layers=variant==0u};
    FakeDevice fake={.on_demand=1};
    GmlRenderDeviceServices device=service(&fake);
    uint32_t screen[64]={0};
    device.observe_on_demand=fake_observe_on_demand;
    CHECK(gpu && !gml_render_init(gpu,NULL));
    CHECK(anygm_compatibility_resolve(&facts,&profile,NULL,0));
    content.compatibility=&profile;
    gpu->win=&content;
    gml_render_bind_device(gpu,&device);
    gml_render_application_surface_bind(gpu,screen,8,8,1);
    CHECK(gml_render_begin_resident_base(gpu,screen,8,8,0.0,0.0));
    gpu->alpha=0.5;
    gpu->gui_pass_active=variant==1u;
    gpu->blendmode=variant==3u;
    gpu->pending_underlay=variant==4u;
    if(variant==2u) gml_render_primitive_rectangle_color(gpu,0,0,2,2,
                          0u,1u,2u,3u,0);
    else gml_render_primitive_rectangle(gpu,0,0,2,2,0x345678u,0);
    CHECK(gpu->resident_application_pass==(variant<4u));
    if(variant<4u)
      CHECK(gml_render_application_scene_flush(gpu) && !fake.readbacks &&
            gml_render_rectangle_valid(&fake.last_primitive));
    gml_render_free(gpu);free(gpu);
  }
  return 1;
}

static int resident_studio_alpha_rectangle_scope(void){
  return resident_studio_alpha_rectangle_scope_variant(0);
}

static int resident_firstgen_gui_alpha_rectangle_scope(void){
  CHECK(resident_studio_alpha_rectangle_scope_variant(1));
  for(unsigned variant=0;variant<10u;variant++){
    GmlRender *render=calloc(1,sizeof *render);
    GmlWin content={.bytecode=15};
    FakeDevice fake={.on_demand=1};
    GmlRenderDeviceServices device=service(&fake);
    uint32_t screen[64]={0};
    device.observe_on_demand=fake_observe_on_demand;
    CHECK(render && !gml_render_init(render,NULL));
    render->win=&content;
    gml_render_bind_device(render,&device);
    gml_render_application_surface_bind(render,screen,8,8,1);
    CHECK(gml_render_begin_resident_base(render,screen,8,8,0.0,0.0));
    gml_render_gui_begin(render,8,8);render->alpha=0.88;
    if(variant==0u) render->gui_base_logical_w=16;
    if(variant==1u) render->gui_logical_h=16;
    if(variant==2u) render->gui_scale_x=2.0;
    if(variant==3u) render->gui_scale_y=0.5;
    if(variant==4u) render->gui_maximise_active=1;
    if(variant==5u) render->gui_maximise_xoffset=1.0;
    if(variant==6u) render->gui_maximise_yoffset=1.0;
    if(variant==7u) render->cam_x=1.0;
    if(variant==8u) render->cam_y=1.0;
    if(variant==9u) fake.on_demand=0;
    gml_render_primitive_rectangle(render,-2,-2,10,10,0u,0);
    /* Typed ports receive resolved raster coordinates even with mapped GUI
     * metadata. A missing on-demand owner still refuses the new combination. */
    CHECK(render->resident_application_pass==(variant<9u));
    if(variant<9u)
      CHECK(gml_render_application_scene_flush(render) && !fake.readbacks &&
            gml_render_rectangle_valid(&fake.last_primitive));
    gml_render_free(render);free(render);
  }
  return 1;
}

static int resident_classic_inverse_rectangle_scope(void){
  static const uint32_t colours[]={0u,0xFFFFFFu,0x345678u,0xFE017Fu};
  for(unsigned generation=0;generation<2u;generation++)
  for(unsigned colour=0;colour<sizeof colours/sizeof colours[0];colour++){
    GmlRender *cpu=(GmlRender*)calloc(1,sizeof *cpu);
    GmlRender *gpu=(GmlRender*)calloc(1,sizeof *gpu);
    GmlWin content={.classic_version=generation?700:530};
    FakeDevice fake={.on_demand=1};
    GmlRenderDeviceServices device=service(&fake);
    uint32_t cpu_pixels[64],gpu_pixels[64],replayed[64];
    device.observe_on_demand=fake_observe_on_demand;
    CHECK(cpu && gpu && gml_render_init(cpu,NULL)==0 && gml_render_init(gpu,NULL)==0);
    for(unsigned i=0;i<64u;i++) cpu_pixels[i]=gpu_pixels[i]=replayed[i]=
      0xFF000000u|((i*9317u)&0xFFFFFFu);
    cpu->classic=gpu->classic=1;
    cpu->win=gpu->win=&content;
    gml_render_begin(cpu,cpu_pixels,8,8,0.0,0.0);
    gml_render_application_surface_bind(gpu,gpu_pixels,8,8,1);
    gml_render_bind_device(gpu,&device);
    CHECK(gml_render_begin_resident_base(gpu,gpu_pixels,8,8,0.0,0.0));
    gpu->fb_opaque_known=gpu->fb_all_opaque=1;
    cpu->blendmode=gpu->blendmode=2;
    gml_render_primitive_rectangle(cpu,6,5,-2,1,colours[colour],0);
    gml_render_primitive_rectangle(gpu,6,5,-2,1,colours[colour],0);
    CHECK(gpu->resident_application_pass && !fake.submits && !fake.readbacks);
    CHECK(gml_render_application_scene_flush(gpu) && fake.submits==1 && !fake.readbacks);
    CHECK(gml_render_classic_inverse_rect_valid(&fake.last_primitive) &&
          !gml_render_classic_alpha_rect_valid(&fake.last_primitive));
    CHECK(gml_render_cpu_replay_one(replayed,8u,8u,&fake.last_primitive,NULL,NULL) &&
          !memcmp(cpu_pixels,replayed,sizeof replayed));
    for(unsigned rectangle=0;rectangle<8u;rectangle++){
      gml_render_primitive_rectangle(cpu,rectangle,0,rectangle,2,colours[colour],0);
      gml_render_primitive_rectangle(gpu,rectangle,0,rectangle,2,colours[colour],0);
    }
    CHECK(gpu->resident_application_pass && fake.submits==1 && !fake.readbacks);
    CHECK(gml_render_application_scene_flush(gpu) && fake.submits==2 &&
          fake.last_batch_operations==8u && !fake.readbacks);
    GmlRenderDrawOp malformed=fake.last_primitive;
    malformed.payload.primitive.alpha=0.5;
    CHECK(!gml_render_classic_inverse_rect_valid(&malformed) &&
          !gml_render_cpu_replay_one(replayed,8u,8u,&malformed,NULL,NULL));
    malformed=fake.last_primitive;
    malformed.target.image.resource_class=GML_RENDER_RESOURCE_SURFACE;
    CHECK(!gml_render_classic_inverse_rect_valid(&malformed));
    /* The paired function matrix now admits partial inverse rectangles through
     * the generic packet; the older narrow packet above still rejects them. */
    cpu->alpha=gpu->alpha=0.5;
    memcpy(replayed,cpu_pixels,sizeof replayed);
    gml_render_primitive_rectangle(cpu,0,0,1,1,colours[colour],0);
    gml_render_primitive_rectangle(gpu,0,0,1,1,colours[colour],0);
    CHECK(gpu->resident_application_pass && !fake.readbacks &&
          gml_render_application_scene_flush(gpu) && fake.submits==3 &&
          gml_render_rectangle_valid(&fake.last_primitive));
    CHECK(gml_render_cpu_replay_one(replayed,8u,8u,&fake.last_primitive,NULL,NULL) &&
          !memcmp(cpu_pixels,replayed,sizeof replayed));
    gml_render_free(cpu); gml_render_free(gpu); free(cpu); free(gpu);
  }
  return 1;
}

static int completed_screen_reader_and_writer(void){
  GmlRender *render=(GmlRender*)calloc(1,sizeof *render);
  uint32_t pixels[12];
  FakeDevice fake={0};
  GmlRenderDeviceServices device=service(&fake);
  GmlRenderImageRef before={0},after={0};
  GmlRenderJournalMetrics metrics={0};
  GmlRenderResourceMetrics observer_metrics={0};
  int has_rgb=-1;
  CHECK(render);
  for(size_t index=0;index<12u;index++) pixels[index]=0x00123456u;
  device.has_rgb=fake_has_rgb;
  gml_render_bind_device(render,&device);
  CHECK(gml_render_begin_resident_base(render,pixels,4,3,0.0,0.0));
  gml_render_set_pending_fill(render,0xFF000000u);
  gml_render_flush_pending_fill(render);
  CHECK(gml_render_scene_flush(render));
  CHECK(gml_render_completed_screen_image(render,pixels,4,3,&before) &&
        before.resource_class==GML_RENDER_RESOURCE_SCREEN);
  gml_render_resource_journal_metrics(render->base_scene_resource,&metrics);
  CHECK(metrics.cpu_replays==0u && metrics.journal_operations==1u &&
        pixels[0]==0x00123456u);
  CHECK(gml_render_completed_screen_has_rgb(render,pixels,4,3,&has_rgb) && !has_rgb &&
        fake.rgb_queries==1 && pixels[0]==0x00123456u);
  gml_render_resource_metrics(render,&observer_metrics);
  CHECK(!observer_metrics.completed_screen_cpu_reads &&
        !observer_metrics.completed_screen_cpu_materializations);
  /* An unavailable or refused exact query must retain the original canonical CPU decision. */
  fake.fail_rgb_query=1;
  CHECK(gml_render_completed_screen_has_rgb(render,pixels,4,3,&has_rgb) && !has_rgb);
  CHECK(gml_render_completed_screen_image(render,pixels,4,3,&after) &&
        before.content_version==after.content_version &&
        before.incarnation==after.incarnation && render->resident_application_pass);
  gml_render_resource_journal_metrics(render->base_scene_resource,&metrics);
  CHECK(metrics.cpu_replays==1u && metrics.gpu_version==metrics.logical_version &&
        metrics.cpu_version==metrics.logical_version && !fake.readbacks &&
        pixels[0]==0xFF000000u && pixels[11]==0xFF000000u);
  gml_render_resource_metrics(render,&observer_metrics);
  CHECK(fake.rgb_queries==2 && observer_metrics.completed_screen_cpu_reads==1u &&
        observer_metrics.completed_screen_cpu_materializations==1u);
  CHECK(gml_render_completed_screen_read(render,pixels,4,3));
  gml_render_resource_metrics(render,&observer_metrics);
  CHECK(observer_metrics.completed_screen_cpu_reads==2u &&
        observer_metrics.completed_screen_cpu_materializations==1u);
  int width=0,height=0;
  const uint32_t *observed=gml_render_screen_pixels_read(render,&width,&height);
  CHECK(observed==pixels && width==4 && height==3 &&
        observed[0]==0xFF000000u && observed[11]==0xFF000000u);
  CHECK(gml_render_completed_screen_image(render,pixels,4,3,&after) &&
        before.content_version==after.content_version &&
        before.incarnation==after.incarnation && render->resident_application_pass);
  CHECK(gml_render_completed_screen_write(render,pixels,4,3) &&
        !render->resident_application_pass &&
        !gml_render_completed_screen_image(render,pixels,4,3,&after));
  pixels[11]=0x00000001u;
  CHECK(gml_render_completed_screen_has_rgb(render,pixels,4,3,&has_rgb) && has_rgb);
  pixels[11]=0xFE000000u;
  CHECK(gml_render_completed_screen_has_rgb(render,pixels,4,3,&has_rgb) && !has_rgb);
  gml_render_bind_device(render,NULL);
  fake.forbid_calls=1;
  CHECK(gml_render_completed_screen_read(render,pixels,4,3) &&
        !fake.forbidden_calls && pixels[11]==0xFE000000u);
  gml_render_free(render);
  free(render);
  return 1;
}

static int conditional_screen_recovery_authority(void){
  GmlRender *render=(GmlRender*)calloc(1,sizeof *render);
  uint32_t screen[14u*9u],sentinel[14u*9u];
  FakeDevice fake={.on_demand=1,.supported_operations=UINT64_C(1)<<GML_RENDER_OP_RECOVER_EMPTY_SCREEN};
  GmlRenderDeviceServices device=service(&fake);
  GmlRenderJournalMetrics metrics={0};
  CHECK(render && gml_render_init(render,NULL)==0);
  CHECK(gml_render_application_surface_ensure_owned(render,4,3) &&
        gml_render_begin_owned_application(render,0.0,0.0,0));
  gml_render_application_surface_publish(render);
  gml_render_set_pending_fill(render,0x7F123456u);
  gml_render_flush_pending_fill(render);
  CHECK(gml_render_application_resource_ensure(render));
  device.observe_on_demand=fake_observe_on_demand;device.has_rgb=fake_has_rgb;
  gml_render_bind_device(render,&device);
  for(unsigned p=0;p<14u*9u;p++) screen[p]=sentinel[p]=0xAB112233u;
  CHECK(gml_render_begin_resident_base(render,screen,14,9,2.0,3.0));
  render->interp=render->alphablend=1;
  gml_render_set_pending_fill(render,0x11000000u);gml_render_flush_pending_fill(render);
  CHECK(gml_render_scene_submit_pending(render));
  int submits=fake.submits;
  for(unsigned variant=0;variant<6u;variant++){
    if(variant==0u) fake.on_demand=0;
    if(variant==1u) fake.supported_operations=0;
    if(variant==2u) render->active_shader=1;
    if(variant==3u) render->aspect_fullwidth=1;
    if(variant==4u) render->color_write_mask=7u;
    if(variant==5u) render->classic=1;
    CHECK(!gml_render_recover_empty_screen(render,screen,14,9,3.0,4.0,10.0,7.0,0) &&
          fake.submits==submits && !fake.rgb_queries && !fake.readbacks);
    fake.on_demand=1;fake.supported_operations=UINT64_C(1)<<GML_RENDER_OP_RECOVER_EMPTY_SCREEN;
    render->interp=1;render->active_shader=-1;render->aspect_fullwidth=0;
    render->color_write_mask=15u;render->classic=0;
  }
  CHECK(gml_render_recover_empty_screen(render,screen,14,9,3.0,4.0,10.0,7.0,0) &&
        fake.submits==submits+1 && fake.last_opcode==GML_RENDER_OP_RECOVER_EMPTY_SCREEN &&
        !fake.rgb_queries && !fake.readbacks && !memcmp(screen,sentinel,sizeof screen) &&
        fake.recovery_rect[0]==1.0 && fake.recovery_rect[1]==1.0 &&
        fake.recovery_rect[2]==10.0 && fake.recovery_rect[3]==7.0);
  gml_render_resource_journal_metrics(render->base_scene_resource,&metrics);
  CHECK(metrics.authority==GML_RENDER_RESOURCE_GPU_RESIDENT &&
        metrics.gpu_version==metrics.logical_version && metrics.cpu_version<metrics.logical_version &&
        !metrics.journal_operations && !metrics.cpu_replays);
  render->interp=0;
  CHECK(gml_render_recover_empty_screen(render,screen,14,9,3.0,4.0,10.0,7.0,0) &&
        fake.submits==submits+2 && !fake.rgb_queries && !fake.readbacks &&
        fake.recovery_rect[0]==1.0 && fake.recovery_rect[1]==1.0 &&
        fake.recovery_rect[2]==10.0 && fake.recovery_rect[3]==7.0);
  CHECK(!gml_render_recover_empty_screen(render,screen,14,9,3.0,4.0,3.0,2.0,0) &&
        fake.submits==submits+2 && !fake.rgb_queries && !fake.readbacks);
  render->interp=1;
  CHECK(gml_render_recover_empty_screen(render,screen,14,9,3.0,4.0,10.0,7.0,1) &&
        fake.submits==submits+3 && !fake.readbacks &&
        fake.recovery_rect[0]==3.0 && fake.recovery_rect[1]==4.0);
  render->world_transform_active=1;render->world_scale_x=render->world_scale_y=2.0;
  CHECK(gml_render_recover_empty_screen(render,screen,14,9,3.0,4.0,10.0,7.0,0) &&
        fake.submits==submits+4 && !fake.readbacks && fake.recovery_rect[0]==4.0 &&
        fake.recovery_rect[1]==5.0 && fake.recovery_rect[2]==20.0 && fake.recovery_rect[3]==14.0);
  gml_render_abandon_device(render);fake.forbid_calls=1;
  CHECK(!gml_render_recover_empty_screen(render,screen,14,9,3.0,4.0,10.0,7.0,0) &&
        !fake.forbidden_calls);
  gml_render_free(render);free(render);return 1;
}

static int conditional_screen_recovery_fullwidth_authority(void){
  GmlRender *render=(GmlRender*)calloc(1,sizeof *render);
  uint32_t screen[12],sentinel[12];
  FakeDevice fake={.on_demand=1,.supported_operations=UINT64_C(1)<<GML_RENDER_OP_RECOVER_EMPTY_SCREEN};
  GmlRenderDeviceServices device=service(&fake);
  GmlRenderJournalMetrics before={0},after={0};
  CHECK(render && gml_render_init(render,NULL)==0);
  CHECK(gml_render_application_surface_ensure_owned(render,4,3) &&
        gml_render_begin_owned_application(render,0.0,0.0,0));
  gml_render_application_surface_publish(render);
  gml_render_set_pending_fill(render,0x7F123456u);
  gml_render_flush_pending_fill(render);
  CHECK(gml_render_application_resource_ensure(render));
  device.observe_on_demand=fake_observe_on_demand;device.has_rgb=fake_has_rgb;
  gml_render_bind_device(render,&device);
  for(unsigned p=0;p<12u;p++) screen[p]=sentinel[p]=0xAB112233u;
  CHECK(gml_render_begin_resident_base(render,screen,4,3,2.0,3.0));
  render->aspect_fullwidth=1;render->aspect_wide_w=4;render->aspect_wide_h=3;
  gml_render_set_pending_fill(render,0x11000000u);gml_render_flush_pending_fill(render);
  CHECK(gml_render_scene_submit_pending(render));
  int submits=fake.submits;
  gml_render_resource_journal_metrics(render->base_scene_resource,&before);
  for(unsigned variant=0;variant<10u;variant++){
    double x=0.0,y=0.0,w=4.0,h=3.0;
    int zero_camera=1;
    if(variant==0u) zero_camera=0;
    if(variant==1u) x=0.25;
    if(variant==2u) y=0.25;
    if(variant==3u) w=5.0;
    if(variant==4u) h=4.0;
    if(variant==5u) render->aspect_wide_w=5;
    if(variant==6u) render->aspect_wide_h=4;
    if(variant==7u){
      render->world_transform_active=1;render->world_scale_x=render->world_scale_y=2.0;
    }
    if(variant==8u) render->app_w=3;
    if(variant==9u) render->app_h=2;
    CHECK(!gml_render_recover_empty_screen(render,screen,4,3,x,y,w,h,zero_camera) &&
          fake.submits==submits && !fake.rgb_queries && !fake.readbacks &&
          !memcmp(screen,sentinel,sizeof screen));
    gml_render_resource_journal_metrics(render->base_scene_resource,&after);
    CHECK(after.logical_version==before.logical_version &&
          after.gpu_version==before.gpu_version && after.cpu_version==before.cpu_version);
    render->aspect_wide_w=render->app_w=4;render->aspect_wide_h=render->app_h=3;
    render->world_transform_active=0;render->world_scale_x=render->world_scale_y=1.0;
  }
  for(unsigned filtered=0;filtered<2u;filtered++){
    render->interp=(int)filtered;
    CHECK(gml_render_recover_empty_screen(render,screen,4,3,0.0,0.0,4.0,3.0,1) &&
          fake.submits==submits+(int)filtered+1 &&
          fake.last_opcode==GML_RENDER_OP_RECOVER_EMPTY_SCREEN &&
          fake.recovery_rect[0]==0.0 && fake.recovery_rect[1]==0.0 &&
          fake.recovery_rect[2]==4.0 && fake.recovery_rect[3]==3.0 &&
          !fake.rgb_queries && !fake.readbacks && !memcmp(screen,sentinel,sizeof screen));
    gml_render_resource_journal_metrics(render->base_scene_resource,&after);
    CHECK(after.authority==GML_RENDER_RESOURCE_GPU_RESIDENT &&
          after.gpu_version==after.logical_version && after.cpu_version<after.logical_version &&
          !after.journal_operations && !after.cpu_replays);
  }
  gml_render_abandon_device(render);fake.forbid_calls=1;
  CHECK(!gml_render_recover_empty_screen(render,screen,4,3,0.0,0.0,4.0,3.0,1) &&
        !fake.forbidden_calls);
  gml_render_free(render);free(render);return 1;
}

static int completed_screen_deferred_upscale(void){
  const int widths[]={7,9,14,19,6,3},heights[]={5,7,10,13,4,2};
  for(unsigned variant=0;variant<36u;variant++){
    GmlRender *cpu=(GmlRender*)calloc(1,sizeof *cpu);
    GmlRender *gpu=(GmlRender*)calloc(1,sizeof *gpu);
    uint32_t cpu_pixels[35]={0},gpu_pixels[35],target[19*13];
    int width=widths[variant%6u],height=heights[variant/6u];
    int box=width<7 || height<5;
    FakeDevice fake={0};
    GmlRenderDeviceServices device=service(&fake);
    GmlRenderJournalMetrics before={0},after={0};
    GmlRenderDeferredPresentation record={0};
    CHECK(cpu && gpu);
    CHECK(gml_render_init(cpu,NULL)==0 && gml_render_init(gpu,NULL)==0);
    for(unsigned at=0;at<35u;at++) gpu_pixels[at]=0x12345678u;
    for(unsigned at=0;at<19u*13u;at++) target[at]=0x89ABCDEFu;
    gml_render_begin(cpu,cpu_pixels,7,5,0.0,0.0);
    gml_render_bind_device(gpu,&device);
    CHECK(gml_render_begin_resident_base(gpu,gpu_pixels,7,5,0.0,0.0));
    gml_render_set_pending_fill(cpu,0xFF102030u);
    gml_render_set_pending_fill(gpu,0xFF102030u);
    gml_render_primitive_rectangle(cpu,1,1,5,3,0x345678u,0);
    gml_render_primitive_rectangle(gpu,1,1,5,3,0x345678u,0);
    gml_render_primitive_point(cpu,6,4,0xABCDEFu);
    gml_render_primitive_point(gpu,6,4,0xABCDEFu);
    gml_render_flush_pending_fill(cpu);
    CHECK(gml_render_scene_flush(gpu));
    gml_render_resource_journal_metrics(gpu->base_scene_resource,&before);
    CHECK(before.cpu_replays==0u && gpu_pixels[0]==0x12345678u && !fake.readbacks);
    CHECK(!gml_render_defer_resident_screen_upscale(gpu,gpu_pixels,7,5,target,width,height));
    gml_render_set_deferred_presentation(gpu,1);
    CHECK(!gml_render_defer_resident_screen_upscale(gpu,gpu_pixels,7,5,gpu_pixels,7,5));
    CHECK(!gml_render_defer_resident_screen_upscale(gpu,cpu_pixels,7,5,target,width,height));
    CHECK(!gml_render_defer_resident_screen_upscale(gpu,gpu_pixels,6,5,target,width,height));
    CHECK(!gml_render_defer_resident_screen_upscale(gpu,gpu_pixels,7,5,target,0,height));
    CHECK(!gml_render_defer_resident_screen_upscale(gpu,gpu_pixels,7,5,target,width,0));
    CHECK(!gml_render_defer_resident_screen_upscale(gpu,gpu_pixels,7,5,target,16385,height));
    CHECK(!gml_render_defer_resident_screen_upscale(gpu,gpu_pixels,7,5,target,width,16385));
    CHECK(gml_render_defer_resident_screen_upscale(gpu,gpu_pixels,7,5,target,width,height));
    CHECK(gml_render_deferred_presentation(gpu,&record) && record.cpu_rgb_only &&
          record.sampling_rule==(box?GML_RENDER_PRESENTATION_BOX:
                                     GML_RENDER_PRESENTATION_PIXEL_CENTRE) &&
          record.resident_image.resource_class==GML_RENDER_RESOURCE_SCREEN &&
          record.target_pixels==target && gpu_pixels[0]==0x12345678u &&
          target[0]==0x89ABCDEFu && !fake.readbacks);
    CHECK(!gml_render_defer_resident_screen_upscale(gpu,gpu_pixels,7,5,target,width,height));
    /* A stale recipe is rejected before writes and remains recoverable. */
    gpu->presentation.resident_image.content_version++;
    gml_render_flush_deferred_presentation(gpu);
    CHECK(gml_render_deferred_presentation(gpu,NULL) && target[0]==0x89ABCDEFu &&
          gpu_pixels[0]==0x12345678u && !fake.readbacks);
    gpu->presentation.resident_image.content_version=record.resident_image.content_version;
    if(variant&1u){
      gml_render_abandon_device(gpu);
      fake.forbid_calls=1;
    }
    gml_render_flush_deferred_presentation(gpu);
    CHECK(!gml_render_deferred_presentation(gpu,NULL) &&
          !fake.readbacks && !fake.forbidden_calls);
    for(int y=0;y<height;y++) for(int x=0;x<width;x++){
      unsigned wanted=0u;
      if(box){
        int sx0=x*7/width,sx1=(x+1)*7/width;
        int sy0=y*5/height,sy1=(y+1)*5/height;
        if(sx1<=sx0) sx1=sx0+1;
        if(sy1<=sy0) sy1=sy0+1;
        unsigned red=0,green=0,blue=0,count=0;
        for(int sy=sy0;sy<sy1;sy++) for(int sx=sx0;sx<sx1;sx++){
          unsigned pixel=cpu_pixels[sy*7+sx];
          red+=(pixel>>16)&255u;green+=(pixel>>8)&255u;blue+=pixel&255u;count++;
        }
        wanted=((red/count)<<16)|((green/count)<<8)|(blue/count);
      } else {
        int sx=((2*x+1)*7)/(2*width),sy=((2*y+1)*5)/(2*height);
        wanted=cpu_pixels[sy*7+sx]&0x00FFFFFFu;
      }
      CHECK(target[y*width+x]==wanted);
    }
    for(unsigned at=(unsigned)(width*height);at<19u*13u;at++)
      CHECK(target[at]==0x89ABCDEFu);
    gml_render_resource_journal_metrics(gpu->base_scene_resource,&after);
    CHECK(after.cpu_replays==1u && after.logical_version==before.logical_version &&
          memcmp(cpu_pixels,gpu_pixels,sizeof cpu_pixels)==0);
    if(!(variant&1u)){
      CHECK(gml_render_completed_screen_write(gpu,gpu_pixels,7,5));
      CHECK(!gml_render_defer_resident_screen_upscale(gpu,gpu_pixels,7,5,target,width,height));
    }
    gml_render_free(cpu); gml_render_free(gpu);
    free(cpu); free(gpu);
  }
  return 1;
}

static int resident_viewless_base_canvas_order(void){
  GmlRender *cpu=(GmlRender*)calloc(1,sizeof *cpu);
  GmlRender *gpu=(GmlRender*)calloc(1,sizeof *gpu);
  uint32_t cpu_pixels[16]={0},gpu_pixels[16]={0},application_pixels[4]={0};
  uint8_t *state_bytes=NULL;
  size_t state_size,written=0,used=0;
  GmlRenderPresentationMetrics presentation={0};
  GmlRenderJournalMetrics metrics={0};
  GmlRenderImageRef image;
  const uint32_t *observed=NULL;
  FakeDevice fake={0};
  GmlRenderDeviceServices device=service(&fake);
  CHECK(cpu && gpu);
  CHECK(gml_render_init(cpu,NULL)==0 && gml_render_init(gpu,NULL)==0);
  gml_render_application_surface_bind(cpu,application_pixels,2,2,0);
  gml_render_application_surface_bind(gpu,application_pixels,2,2,0);
  gml_render_application_surface_set_draw_enabled(cpu,0);
  gml_render_application_surface_set_draw_enabled(gpu,0);
  gml_render_begin(cpu,cpu_pixels,4,4,0.0,0.0);
  gml_render_bind_device(gpu,&device);
  CHECK(gml_render_begin_resident_base(gpu,gpu_pixels,4,4,0.0,0.0));
  CHECK(gml_render_presentation_metrics(gpu,&presentation) &&
        !presentation.application_owned && !presentation.application_draw_enabled &&
        presentation.application_width==2 && presentation.application_height==2 &&
        gpu->fb==gpu_pixels && gpu->resident_application_pass);
  CHECK(gpu->base_scene_resource);
  image=gml_render_resource_image_ref(gpu->base_scene_resource);
  CHECK(image.resource_class==GML_RENDER_RESOURCE_SCREEN && image.incarnation);
  gml_render_set_pending_fill(cpu,0xFF102030u);
  gml_render_set_pending_fill(gpu,0xFF102030u);
  gml_render_primitive_rectangle(cpu,1,1,2,2,0x345678u,0);
  gml_render_primitive_rectangle(gpu,1,1,2,2,0x345678u,0);
  gml_render_resource_journal_metrics(gpu->base_scene_resource,&metrics);
  CHECK(gpu->resident_application_pass && fake.submits==2 && fake.readbacks==0 &&
        metrics.journal_operations==2u && metrics.cpu_replays==0u);
  cpu->alpha=gpu->alpha=0.5;
  gml_render_primitive_line(cpu,0,0,3,3,0xffffffu,1);
  gml_render_primitive_line(gpu,0,0,3,3,0xffffffu,1);
  gml_render_flush_pending_fill(cpu);
  CHECK(!gpu->resident_application_pass && fake.readbacks==0 &&
        memcmp(cpu_pixels,gpu_pixels,sizeof cpu_pixels)==0 &&
        application_pixels[0]==0u && application_pixels[3]==0u);
  CHECK(gml_render_begin_resident_base(gpu,gpu_pixels,4,4,0.0,0.0));
  gml_render_set_pending_fill(gpu,0xFFABCDEFu);
  gml_render_flush_pending_fill(gpu);
  CHECK(gpu->resident_application_pass && fake.submits==3);
  gml_render_abandon_device(gpu);
  CHECK(gml_render_resource_acquire_cpu_read(gpu->base_scene_resource,&observed) &&
        observed==gpu_pixels);
  CHECK(gpu_pixels[0]==0xFFABCDEFu && gpu_pixels[15]==0xFFABCDEFu &&
        application_pixels[0]==0u && fake.readbacks==0);
  CHECK(!gml_render_begin_resident_base(gpu,gpu_pixels,4,4,0.0,0.0));
  state_size=gml_render_state_size(gpu,0);
  CHECK(state_size>0 && (state_bytes=(uint8_t*)malloc(state_size)) &&
        gml_render_state_save(gpu,0,state_bytes,state_size,&written) &&
        written==state_size &&
        gml_render_state_load(gpu,state_bytes,state_size,&used) &&
        used==state_size && !gpu->base_scene_resource &&
        !gpu->resident_application_pass && application_pixels[0]==0u);
  free(state_bytes);
  gml_render_free(cpu);
  gml_render_free(gpu);
  free(cpu);
  free(gpu);
  return 1;
}

static int surface_metadata_preserves_device_authority(void){
  for(unsigned application=0;application<2u;application++){
    GmlRender *render=(GmlRender*)calloc(1,sizeof *render);
    FakeDevice fake={0};
    GmlRenderDeviceServices device=service(&fake);
    GmlRenderJournalMetrics before={0},after={0};
    GmlRenderResource *resource;
    GmlRenderDrawOp operation;
    GmlRenderDrawBatch batch;
    const uint32_t *observed=NULL;
    uint32_t *pixels;
    int id;
    CHECK(render && gml_render_init(render,NULL)==0);
    if(application){
      CHECK(gml_render_application_surface_ensure_owned(render,4,4));
      gml_render_application_surface_publish(render);
      id=0;
      pixels=render->app_surface_owned;
    } else {
      id=gml_surface_create(render,4,4);
      CHECK(id>0);
      pixels=render->surface[id-1].px;
    }
    gml_render_bind_device(render,&device);
    resource=application?gml_render_application_resource_ensure(render):
                         gml_render_surface_resource_ensure(render,id);
    CHECK(resource);
    operation=clear_op(2u,0xFF315779u,GML_RENDER_REPLAY_EXACT);
    operation.target.image=gml_render_resource_image_ref(resource);
    operation.target.image.content_version++;
    operation.target.pixel_format=GML_RENDER_PIXEL_ARGB8888;
    batch=(GmlRenderDrawBatch){&operation,1u,NULL,0u};
    CHECK(gml_render_resource_submit(resource,&device,&batch,NULL,0u)==
          GML_RENDER_RESOURCE_EXEC_DEVICE);
    gml_render_resource_journal_metrics(resource,&before);
    CHECK(before.journal_operations==1u &&
          before.authority==GML_RENDER_RESOURCE_GPU_REPLAYABLE && pixels[0]==0u);
    CHECK(gml_surface_exists(render,id) && gml_surface_width(render,id)==4 &&
          gml_surface_height(render,id)==4);
    gml_render_resource_journal_metrics(resource,&after);
    CHECK(after.authority==before.authority &&
          after.logical_version==before.logical_version &&
          after.cpu_version==before.cpu_version &&
          after.cpu_replays==before.cpu_replays &&
          after.journal_operations==before.journal_operations &&
          pixels[0]==0u && fake.readbacks==0);
    observed=gml_surface_pixels_read(render,id,NULL,NULL);
    CHECK(observed==pixels && pixels[0]==0xFF315779u);
    gml_render_resource_journal_metrics(resource,&after);
    CHECK(after.cpu_replays==before.cpu_replays+1u && fake.readbacks==0);
    if(!application){
      const uint32_t source_pixels[4]={0xFF315779u,0xFF315779u,
                                       0xFF315779u,0xFF315779u};
      GmlRenderResourceSource source=source_view(source_pixels,1);
      GmlRenderImageRef source_image=source.image;
      GmlRenderDrawStateSnapshot state=quad_state(0);
      GmlPlanShader shader={0};
      shader.vertex_es="void main(){gl_Position=vec4(0.0);}";
      shader.fragment_es="void main(){gl_FragColor=vec4(1.0);}";
      operation.opcode=GML_RENDER_OP_CONTENT_SHADER_DRAW;
      operation.reconstruction_mode=GML_RENDER_DEVICE_CHECKPOINT;
      operation.target.image=gml_render_resource_image_ref(resource);
      operation.target.image.content_version++;
      operation.source_count=1u;
      operation.sources=&source_image;
      operation.payload.shader=&shader;
      batch=(GmlRenderDrawBatch){&operation,1u,&state,1u};
      CHECK(gml_render_resource_submit_deferred(resource,&device,&batch,&source,1u)==
            GML_RENDER_RESOURCE_EXEC_DEVICE);
      gml_render_resource_journal_metrics(resource,&before);
      CHECK(before.authority==GML_RENDER_RESOURCE_GPU_PENDING_CHECKPOINT);
      CHECK(gml_surface_exists(render,id) && gml_surface_width(render,id)==4 &&
            gml_surface_height(render,id)==4);
      gml_render_resource_journal_metrics(resource,&after);
      CHECK(after.authority==before.authority &&
            after.device_checkpoints==before.device_checkpoints && fake.readbacks==0);
      fake.readback_colour=0xFF102030u;
      CHECK(gml_surface_pixels_read(render,id,NULL,NULL)==pixels &&
            pixels[0]==fake.readback_colour && fake.readbacks==1);
    }
    CHECK(!gml_surface_exists(render,-1) && gml_surface_width(render,-1)==0 &&
          gml_surface_height(render,GML_MAX_SURFACES+1)==0);
    gml_render_free(render);
    free(render);
  }
  return 1;
}

static int resident_threshold_palette_surface(void){
  for(unsigned target_kind=0;target_kind<3u;target_kind++)
    for(unsigned application_source=0;application_source<2u;application_source++)
      for(unsigned alpha_test=0;alpha_test<2u;alpha_test++)
        for(unsigned port=0;port<3u;port++){
          GmlRender *render=(GmlRender*)calloc(1,sizeof *render);
          uint32_t screen[64]={0};
          FakeDevice fake={.on_demand=1};
          GmlRenderDeviceServices device=service(&fake);
          GmlRenderJournalMetrics before={0},after={0};
          GmlRenderResource *resource;
          int source=0,target=0;
          if(application_source && target_kind==1u){ free(render); continue; }
          CHECK(render && gml_render_init(render,NULL)==0);
          CHECK(gml_render_application_surface_ensure_owned(render,
                  target_kind==1u?8:4,target_kind==1u?8:4));
          if(!application_source){
            source=gml_surface_create(render,4,4);
            CHECK(source>0);
            for(unsigned i=0;i<16u;i++) render->surface[source-1].px[i]=0xFF315779u;
            render->surface[source-1].opaque_known=render->surface[source-1].all_opaque=1;
            render->surface[source-1].all_transparent=0;
          } else {
            for(unsigned i=0;i<16u;i++) render->app_surface_owned[i]=0xFF315779u;
            render->app_surface_opaque=1;
          }
          gml_render_application_surface_set_draw_enabled(render,0);
          gml_render_application_surface_publish(render);
          render->shader_pal=(struct GmlShaderPal*)calloc(1,sizeof *render->shader_pal);
          CHECK(render->shader_pal);
          render->n_shader_pal=1;
          render->shader_pal[0].threshold_palette=1;
          render->shader_pal[0].threshold_palette_set=0x3ffu;
          render->shader_pal[0].threshold_palette_red=0.25f;
          for(unsigned branch=0;branch<2u;branch++) for(unsigned band=0;band<4u;band++)
            render->shader_pal[0].threshold_palette_green[branch][band]=(float)(7u-2u*band)/8.0f;
          for(unsigned colour=0;colour<10u;colour++) for(unsigned channel=0;channel<3u;channel++)
            render->shader_pal[0].threshold_palette_colour[colour][channel]=(uint8_t)(19u+colour*17u+channel*11u);
          gml_render_bind_device(render,&device);
          gml_render_set_scene_device(render,1);
          if(target_kind==1u){
            CHECK(gml_render_begin_resident_owned_application(render,0.0,0.0,0));
            resource=render->app_surface_resource;
          } else {
            CHECK(gml_render_begin_resident_base(render,screen,8,8,0.0,0.0));
            if(target_kind==2u){
              target=gml_surface_create(render,8,8);
              CHECK(target>0 && gml_surface_set_target(render,target));
              resource=gml_render_surface_resource_ensure(render,target);
            } else resource=render->base_scene_resource;
          }
          CHECK(resource);
          gml_render_resource_journal_metrics(resource,&before);
          gml_render_set_pending_fill(render,0xFF010203u);
          render->active_shader=0;
          render->alpha_test_enable=(int)alpha_test;
          render->alpha_test_ref=4;
          render->alphablend=0;
          CHECK(!render->shader_device_present && !render->shader_device_expected);
          if(port==0u) gml_draw_surface_stretched(render,source,1.0,1.0,4.0,4.0,0xFFFFFFu,1.0);
          else if(port==1u) gml_draw_surface_part_ext(render,source,0.0,0.0,4.0,4.0,
                                                     1.0,1.0,1.0,1.0,0xFFFFFFu,1.0);
          else gml_draw_surface_ext(render,source,1.0,1.0,1.0,1.0,0.0,0xFFFFFFu,1.0);
          CHECK(render->resident_application_pass && fake.shader_submits==1 && fake.readbacks==0 &&
                fake.last_opcode==GML_RENDER_OP_CONTENT_SHADER_DRAW &&
                fake.last_shader_uniform_count==14u &&
                fake.last_target_region.x==1 && fake.last_target_region.y==1 &&
                fake.last_target_region.width==4u && fake.last_target_region.height==4u &&
                !render->shader_device_present && !render->shader_device_expected);
          gml_render_resource_journal_metrics(resource,&after);
          CHECK(after.logical_version>before.logical_version && after.cpu_version<after.logical_version &&
                after.authority==GML_RENDER_RESOURCE_GPU_PENDING_CHECKPOINT);
          render->active_shader=-1;
          gml_render_free(render);
          free(render);
        }
  return 1;
}

static int resident_lut_surface(void){
  for(unsigned target_kind=0;target_kind<3u;target_kind++)
    for(unsigned port=0;port<3u;port++){
      GmlRender *render=calloc(1,sizeof *render);
      uint32_t screen[64]={0};
      FakeDevice fake={.on_demand=1};
      GmlRenderDeviceServices device=service(&fake);
      CHECK(render && gml_render_init(render,NULL)==0);
      CHECK(gml_render_application_surface_ensure_owned(render,4,4));
      for(unsigned i=0;i<16u;i++) render->app_surface_owned[i]=0xFF315779u;
      render->app_surface_opaque=1;
      gml_render_application_surface_set_draw_enabled(render,0);
      gml_render_application_surface_publish(render);
      render->spr=calloc(1,sizeof *render->spr);
      render->shader_pal=calloc(1,sizeof *render->shader_pal);
      CHECK(render->spr && render->shader_pal);
      render->n_spr=render->n_shader_pal=1;
      render->spr[0].w=render->spr[0].h=4;
      render->spr[0].n_frames=1;
      render->spr[0].frame=calloc(1,sizeof(int));
      render->spr[0].runtime_rgba=malloc(64u);
      CHECK(render->spr[0].frame && render->spr[0].runtime_rgba);
      memset(render->spr[0].runtime_rgba,127,64u);
      render->lut_pal_sprite=render->lut_pal_frame=0;
      render->shader_pal[0].lut=1;
      render->shader_pal[0].lut_row=.375f;
      gml_render_bind_device(render,&device);
      gml_render_set_scene_device(render,1);
      CHECK(gml_render_begin_resident_base(render,screen,8,8,0,0));
      if(target_kind==1u){
        int target=gml_surface_create(render,8,8);
        CHECK(target>0 && gml_surface_set_target(render,target));
      } else if(target_kind==2u) gml_render_gui_begin(render,8,8);
      render->active_shader=0;
      gml_render_set_pending_fill(render,0xFF010203u);
      if(port==0u) gml_draw_surface_stretched(render,0,1,1,4,4,0xFFFFFFu,1);
      else if(port==1u) gml_draw_surface_part_ext(render,0,0,0,4,4,1,1,1,1,0xFFFFFFu,1);
      else gml_draw_surface_ext(render,0,1,1,1,1,0,0xFFFFFFu,1);
      CHECK(render->resident_application_pass && fake.shader_submits==1 &&
            fake.readbacks==0 && fake.last_opcode==GML_RENDER_OP_CONTENT_SHADER_DRAW &&
            !render->shader_device_present && !render->shader_device_expected);
      if(target_kind==0u && port==0u){
        render->active_shader=-1;
        CHECK(gml_render_resident_application_cpu_barrier(render));
        CHECK(gml_render_begin_resident_base(render,screen,4,4,0,0));
        render->app_surface_opaque=0;
        render->app_surface_owned[0]=0u;
        render->active_shader=0;
        gml_render_set_pending_fill(render,0x7F010203u);
        gml_draw_surface_stretched(render,0,0,0,4,4,0xFFFFFFu,1);
        CHECK(render->resident_application_pass && !render->fb_all_opaque &&
              !render->fb_opaque_known);
      }
      render->active_shader=-1;
      gml_render_free(render);
      free(render);
    }
  return 1;
}

static int resident_centered_full_source_shader(void){
  for(unsigned source_kind=0;source_kind<3u;source_kind++)
    for(unsigned part=0;part<2u;part++)
      for(unsigned linear=0;linear<2u;linear++){
        GmlRender *render=(GmlRender*)calloc(1,sizeof *render);
        uint32_t borrowed[16],screen[64]={0};
        GmlRenderResource *source_resource=NULL;
        GmlRenderJournalMetrics before={0},after={0};
        FakeDevice fake={0};
        GmlRenderDeviceServices device=service(&fake);
        int source=0;
        CHECK(render && gml_render_init(render,NULL)==0);
        for(unsigned index=0;index<16u;index++) borrowed[index]=0xFF315779u;
        render->shader_pal=(struct GmlShaderPal*)calloc(1,sizeof *render->shader_pal);
        CHECK(render->shader_pal);
        render->n_shader_pal=1;
        render->shader_pal[0].source_vertex_es=
          "attribute vec3 in_Position; void main(){gl_Position=vec4(in_Position,1.0);}";
        render->shader_pal[0].source_fragment_es=
          "uniform sampler2D gm_BaseTexture; void main(){gl_FragColor=texture2D(gm_BaseTexture,vec2(0.5));}";
        render->shader_device_present=render->shader_device_expected=1;
        if(source_kind==0u) gml_render_application_surface_bind(render,borrowed,4,4,1);
        else if(source_kind==1u){
          CHECK(gml_render_application_surface_ensure_owned(render,4,4));
          source_resource=gml_render_application_resource_ensure(render);
        } else {
          source=gml_surface_create(render,4,4);
          CHECK(source>0);
          source_resource=gml_render_surface_resource_ensure(render,source);
        }
        gml_render_application_surface_set_draw_enabled(render,0);
        gml_render_application_surface_publish(render);
        gml_render_bind_device(render,&device);
        if(source_kind){
          GmlRenderDrawOp clear=clear_op(2u,0xFF315779u,GML_RENDER_REPLAY_EXACT);
          GmlRenderDrawBatch batch={&clear,1u,NULL,0u};
          CHECK(source_resource);
          clear.target.image=gml_render_resource_image_ref(source_resource);
          clear.target.image.content_version++;
          clear.target.pixel_format=GML_RENDER_PIXEL_ARGB8888;
          CHECK(gml_render_resource_submit(source_resource,&device,&batch,NULL,0u)==
                GML_RENDER_RESOURCE_EXEC_DEVICE);
          gml_render_resource_journal_metrics(source_resource,&before);
          CHECK(before.journal_operations==1u &&
                before.authority==GML_RENDER_RESOURCE_GPU_REPLAYABLE);
        }
        CHECK(gml_render_begin_resident_base(render,screen,8,8,1.0,2.0));
        render->active_shader=0;
        render->interp=(int)linear;
        gml_render_set_pending_fill(render,0xFF010203u);
        fake.readback_colour=0xFF315779u;
        if(part) gml_draw_surface_part_ext(render,source,0.0,0.0,4.0,4.0,
                                           3.0,3.0,1.0,1.5,0xFFFFFFu,1.0);
        else gml_draw_surface_stretched(render,source,3.0,3.0,4.0,6.0,
                                        0xFFFFFFu,1.0);
        CHECK(gml_render_completed_screen_read(render,screen,8,8));
        CHECK(render->resident_application_pass && fake.shader_submits==1 &&
              fake.last_target_region.x==2 && fake.last_target_region.y==1 &&
              fake.last_target_region.width==4u &&
              fake.last_target_region.height==6u && fake.readbacks==1);
        if(source_resource){
          gml_render_resource_journal_metrics(source_resource,&after);
          CHECK(after.authority==before.authority &&
                after.cpu_version==before.cpu_version &&
                after.cpu_replays==before.cpu_replays &&
                after.journal_operations==before.journal_operations);
        }
        gml_render_free(render);
        free(render);
      }
  return 1;
}

static int resident_borrowed_surface_zero_screen_shader(void){
  GmlRender *render=(GmlRender*)calloc(1,sizeof *render);
  uint32_t source[16]={0},screen[16]={0};
  GmlRenderJournalMetrics metrics={0};
  GmlRenderImageRef image;
  const uint32_t *observed=NULL;
  FakeDevice fake={0};
  GmlRenderDeviceServices device=service(&fake);
  CHECK(render && gml_render_init(render,NULL)==0);
  render->shader_pal=(struct GmlShaderPal*)calloc(1,sizeof *render->shader_pal);
  CHECK(render->shader_pal);
  render->n_shader_pal=1;
  render->shader_pal[0].source_vertex_es=
    "attribute vec3 in_Position; void main(){ gl_Position=vec4(in_Position,1.0); }";
  render->shader_pal[0].source_fragment_es=
    "uniform sampler2D gm_BaseTexture; void main(){ gl_FragColor=texture2D(gm_BaseTexture,vec2(0.5)); }";
  render->shader_device_present=render->shader_device_expected=1;
  for(int index=0;index<16;index++) source[index]=0xFF102030u;
  gml_render_application_surface_bind(render,source,4,4,1);
  gml_render_application_surface_set_draw_enabled(render,0);
  gml_render_application_surface_publish(render);
  fake.readback_colour=0xFF405060u;
  gml_render_bind_device(render,&device);
  CHECK(gml_render_begin_resident_base(render,screen,4,4,0.0,0.0));
  gml_render_gui_begin(render,4,4);
  render->active_shader=0;
  gml_render_set_pending_fill(render,0u);
  gml_draw_surface_stretched(render,0,0.0,0.0,4.0,4.0,0xFFFFFFu,1.0);
  CHECK(gml_render_completed_screen_read(render,screen,4,4));
  CHECK(render->resident_application_pass && render->shaded_draws==1u &&
        fake.shader_submits==1 && fake.last_opcode==GML_RENDER_OP_CONTENT_SHADER_DRAW &&
        fake.last_source_count==1u && fake.readbacks==1 &&
        !render->app_surface_owned && !render->app_draw_enable &&
        render->app_surface==source && render->app_w==4 && render->app_h==4);
  image=gml_render_resource_image_ref(render->base_scene_resource);
  gml_render_resource_journal_metrics(render->base_scene_resource,&metrics);
  CHECK(image.resource_class==GML_RENDER_RESOURCE_SCREEN &&
        metrics.authority==GML_RENDER_RESOURCE_BOTH_CURRENT &&
        metrics.device_checkpoints==1u &&
        screen[0]==fake.readback_colour && source[0]==0xFF102030u);
  gml_render_abandon_device(render);
  CHECK(gml_render_resource_acquire_cpu_read(render->base_scene_resource,&observed) &&
        observed==screen && observed[15]==fake.readback_colour &&
        source[15]==0xFF102030u);
  gml_render_free(render);
  free(render);
  return 1;
}

static int resident_identity_gui_filtered_atlas(void){
  GmlRenderQuad exact={0};
  exact.shape=GML_RENDER_QUAD_AXIS_ALIGNED;
  exact.sampling.filter=GML_RENDER_FILTER_BILINEAR;
  exact.sampling.quantisation=GML_RENDER_QUANTISE_SPRITE_FIRSTGEN_INDEPENDENT;
  exact.sampling.edge_rule=GML_RENDER_EDGE_LOGICAL_TPAG;
  exact.sampling.logical_margin_mask=GML_RENDER_MARGIN_LEFT|GML_RENDER_MARGIN_TOP;
  exact.sampling.source_region=(GmlRenderRect){1,1,4u,4u};
  exact.inverse_anchor[0]=4.25; exact.inverse_anchor[1]=1.25;
  exact.inverse_x[0]=-0.75; exact.inverse_y[1]=1.5;
  exact.x[0]=exact.x[3]=1.0; exact.x[1]=exact.x[2]=4.0;
  exact.y[0]=exact.y[1]=1.0; exact.y[2]=exact.y[3]=7.0;
  exact.filtered_draw_alpha=0.6;
  for(unsigned corner=0;corner<4u;corner++) exact.corner_colour[corner]=0xffa0c0e0u;
  CHECK(gml_render_filtered_font_quad_valid(&exact,12u,10u));
  for(unsigned variant=0;variant<8u;variant++){
    GmlRenderQuad invalid=exact;
    if(variant==0u) invalid.sampling.quantisation=GML_RENDER_QUANTISE_FONT_FIRSTGEN_INDEPENDENT;
    if(variant==1u) invalid.sampling.edge_rule=GML_RENDER_EDGE_CLAMP_SUBIMAGE;
    if(variant==2u) invalid.sampling.logical_margin_mask=16u;
    if(variant==3u) invalid.inverse_x[0]=-4.01;
    if(variant==4u) invalid.inverse_y[1]=0x1p-17;
    if(variant==5u) invalid.filtered_draw_alpha=0x1p-33;
    if(variant==6u) invalid.inverse_anchor[0]=0x1p-1074;
    if(variant==7u) invalid.x[3]=2.0;
    CHECK(!gml_render_filtered_font_quad_valid(&invalid,12u,10u));
  }
  CHECK(gml_render_rotated_alpha16_factor(255u,258.0/65536.0-0x1p-40)==257u);
  CHECK(gml_render_rotated_alpha16_factor(255u,258.0/65536.0+0x1p-40)==258u);
  CHECK(gml_render_rotated_alpha16_factor(255u,5.0/16.0)==20480u);
  CHECK(gml_render_rotated_alpha16_factor(1u,1.0/512.0)==0u);
  CHECK(gml_render_rotated_alpha16_factor(1u,1.0/256.0)==1u);
  enum { WIDTH=12,HEIGHT=10,ATLAS=6,PAGE=4 };
  GmlRender *render=(GmlRender*)calloc(1,sizeof *render);
  GmlWin content={0};
  uint32_t cpu[WIDTH*HEIGHT]={0},screen[WIDTH*HEIGHT]={0};
  FakeDevice fake={0};
  GmlRenderDeviceServices device=service(&fake);
  CHECK(render && gml_render_init(render,NULL)==0);
  const int atlas_index=render->n_atlas;
  GmlAtlas *atlases=(GmlAtlas*)realloc(render->atlas,
      (size_t)(atlas_index+2)*sizeof *render->atlas);
  CHECK(atlases);
  render->atlas=atlases;
  memset(&render->atlas[atlas_index],0,2u*sizeof *render->atlas);
  render->n_atlas=atlas_index+2;
  render->tpag=(GmlTpag*)calloc(2,sizeof *render->tpag);
  render->bg=(GmlBg*)calloc(1,sizeof *render->bg);
  render->spr=(GmlSprite*)calloc(2,sizeof *render->spr);
  CHECK(render->atlas && render->tpag && render->bg && render->spr);
  render->n_tpag=render->n_spr=2;
  render->n_bg=1;
  render->atlas[atlas_index].px=(uint8_t*)malloc(ATLAS*ATLAS*4u);
  render->atlas[atlas_index+1].px=(uint8_t*)malloc(ATLAS*ATLAS*4u);
  render->spr[0].frame=(int*)calloc(1,sizeof(int));
  render->spr[1].frame=(int*)calloc(1,sizeof(int));
  CHECK(render->atlas[atlas_index].px && render->atlas[atlas_index+1].px &&
        render->spr[0].frame && render->spr[1].frame);
  for(unsigned index=0;index<ATLAS*ATLAS;index++){
    uint8_t *pixel=render->atlas[atlas_index].px+index*4u;
    pixel[0]=(uint8_t)(32u+index*3u);
    pixel[1]=(uint8_t)(48u+index*2u);
    pixel[2]=(uint8_t)(64u+index);
    pixel[3]=255u;
    memcpy(render->atlas[atlas_index+1].px+index*4u,pixel,4u);
    render->atlas[atlas_index+1].px[index*4u+3u]=1u;
  }
  render->atlas[atlas_index].w=render->atlas[atlas_index].h=ATLAS;
  render->atlas[atlas_index+1].w=render->atlas[atlas_index+1].h=ATLAS;
  render->tpag[0].atlas=atlas_index;
  render->tpag[0].sx=render->tpag[0].sy=1;
  render->tpag[0].sw=render->tpag[0].sh=PAGE;
  render->tpag[0].tw=render->tpag[0].th=PAGE;
  render->tpag[0].bw=render->tpag[0].bh=PAGE;
  render->spr[0].w=render->spr[0].h=PAGE;
  render->spr[0].n_frames=1;
  render->tpag[1]=render->tpag[0];
  render->tpag[1].atlas=atlas_index+1;
  render->spr[1].w=render->spr[1].h=PAGE;
  render->spr[1].n_frames=1;
  render->spr[1].frame[0]=1;
  content.bytecode=15;
  render->win=&content;
  render->interp=1;
  /* Keep the qualified identity cases, then exercise the same pixel rules on
   * an owned nonuniformly mapped GUI screen: ordinary/ext, stretched tint,
   * reflection, trim and bounded reduction share one mapping boundary. */
  for(unsigned variant=0;variant<27u;variant++){
    const int trimmed=variant==11u || variant==12u || variant==15u || variant==19u ||
      variant>=21u;
    const uint32_t fill=variant==15u || variant==19u?0x00102030u:
      variant>=14u?0x40102030u:0xff102030u;
    content.bytecode=variant==20u?17:15;
    render->tpag[0].scene_opaque_checked=0;
    render->tpag[0].tx=render->tpag[0].ty=trimmed;
    render->tpag[0].bw=render->tpag[0].bh=PAGE+trimmed;
    render->spr[0].w=render->spr[0].h=PAGE+trimmed;
    render->tpag[1].tx=render->tpag[1].ty=trimmed;
    render->tpag[1].bw=render->tpag[1].bh=PAGE+trimmed;
    render->spr[1].w=render->spr[1].h=PAGE+trimmed;
    render->spr[0].originx=render->spr[0].originy=variant>=21u?2:0;
    unsigned visible=0;
    for(unsigned accelerated=0;accelerated<2u;accelerated++){
      GmlRenderJournalMetrics baseline={0};
      uint32_t *pixels=accelerated?screen:cpu;
      gml_render_bind_device(render,accelerated?&device:NULL);
      if(accelerated)
        CHECK(gml_render_begin_resident_base(render,pixels,WIDTH,HEIGHT,0.0,0.0));
      else gml_render_begin(render,pixels,WIDTH,HEIGHT,0.0,0.0);
      if(accelerated)
        gml_render_resource_journal_metrics(render->base_scene_resource,&baseline);
      gml_render_gui_begin(render,WIDTH,HEIGHT);
      if(variant>=6u) gml_render_gui_set_size(render,5,4);
      gml_render_set_pending_fill(render,fill);
      int submits=fake.submits;
      switch(variant){
        case 0: gml_draw_background(render,0,0.25,0.3); break;
        case 1: gml_draw_background_ext(render,0,-0.6,-1.25,1.6796875,1.715,
                                       0xffffffu,1.0); break;
        case 2: gml_draw_background_part_ext(render,0,1.0,0.0,2.0,3.0,
                    0.3,0.5,1.715,1.4814814814814814,0xa0c0e0u,0.6); break;
        case 3: gml_draw_sprite_ext(render,0,0,0.3,0.5,1.6796875,1.715,
                                     0.0,0xa0c0e0u,0.6); break;
        case 4: gml_draw_background_stretched(render,0,0.3,0.5,6.0,7.0,
                                              0xffffffu,1.0); break;
        case 5: gml_draw_background_tile(render,0,1.0,1.0,2.0,2.0,
                   0.3,0.5,1.715,1.4814814814814814,1,1,0,0xffffffu,1.0); break;
        case 6: gml_draw_sprite(render,0,0,0.25,0.3); break;
        case 7: gml_draw_sprite_ext(render,0,0,0.3,0.5,0.75,0.65,
                                     0.0,0xa0c0e0u,0.6); break;
        case 8: gml_draw_sprite_stretched(render,0,0,0.3,0.5,6.0,5.0,
                                            0xffffffu,1.0); break;
        case 9: gml_draw_sprite_stretched(render,0,0,-0.3,0.5,6.0,5.0,
                                            0xa0c0e0u,0.6); break;
        case 10: gml_draw_sprite_ext(render,0,0,4.1,0.25,-0.65,0.75,
                                      0.0,0xffffffu,1.0); break;
        case 11: gml_draw_sprite_stretched(render,0,0,-0.3,0.5,6.0,5.0,
                                             0xa0c0e0u,0.6); break;
        case 12: gml_draw_sprite_ext(render,0,0,0.3,0.5,0.75,0.65,
                                      0.0,0xffffffu,1.0); break;
        case 13: gml_draw_sprite_stretched(render,0,0,0.3,0.5,1.0,1.0,
                                             0xffffffu,1.0); break;
        case 14: gml_draw_sprite_stretched(render,0,0,-0.3,0.5,6.0,5.0,
                                             0xa0c0e0u,0.6); break;
        case 15: gml_draw_sprite_stretched(render,0,0,-0.3,0.5,6.0,5.0,
                                             0xa0c0e0u,0.6); break;
        case 16: gml_draw_sprite_stretched(render,0,0,-9999.0,-9999.0,
                                             100399.0,100224.0,0u,1.0); break;
        case 17: gml_draw_sprite_stretched(render,0,0,-9999.0,-9999.0,
                                             100399.0,100224.0,0u,0x1p-53); break;
        case 19: gml_draw_sprite_stretched(render,1,0,-0.3,0.5,6.0,5.0,
                                             0xa0c0e0u,0.6); break;
        case 21: case 22: case 23: case 24: case 25: case 26:
          gml_draw_sprite_ext(render,0,0,1.4,1.9,
            variant==23u?-0.75:0.75,variant==24u?-0.65:0.65,
            variant==22u?90.0:variant==24u?-80.0:-40.0,
            0xa0c0e0u,variant==25u?1.9:variant==26u?0.9999999999999991:0.6);
          break;
        default: gml_draw_sprite_stretched(render,0,0,0.3,0.5,6.0,5.0,
                                             0xffffffu,1.0); break;
      }
      if(!accelerated){
        gml_render_flush_pending_fill(render);
        gml_render_flush_rotated_batch(render);
        for(unsigned index=0;index<WIDTH*HEIGHT;index++)
          if(cpu[index]!=fill) visible++;
        CHECK(visible>0u);
        if(variant==17u){
          /* Positive float alpha still stamps covered destination alpha even
           * when its RGB contribution rounds to zero. */
          for(unsigned index=0;index<WIDTH*HEIGHT;index++)
            CHECK(cpu[index]==0xff102030u);
        }
      } else {
        GmlRenderJournalMetrics metrics={0};
        CHECK(render->resident_application_pass && render->gui_pass_active);
        CHECK(gml_render_scene_flush(render) && fake.submits>submits &&
              fake.last_opcode==GML_RENDER_OP_TEXTURED_QUAD);
        if(variant>=21u)
          CHECK(fake.last_batch_operations>1u && fake.last_batch_states==2u);
        CHECK(gml_render_resource_image_ref(render->base_scene_resource).resource_class==
              GML_RENDER_RESOURCE_SCREEN);
        gml_render_resource_journal_metrics(render->base_scene_resource,&metrics);
        if(variant==8u || variant==18u || variant==20u){
          const uint32_t *recovered=NULL;
          /* The already exact opaque-white stretched sampler retains its
           * recipe rather than publishing a device checkpoint. */
          CHECK(metrics.authority==GML_RENDER_RESOURCE_GPU_REPLAYABLE &&
                metrics.device_checkpoints==baseline.device_checkpoints &&
                metrics.cpu_replays==baseline.cpu_replays);
          CHECK(gml_render_resource_acquire_cpu_read(render->base_scene_resource,&recovered) &&
                !memcmp(recovered,cpu,sizeof cpu));
          gml_render_resource_journal_metrics(render->base_scene_resource,&metrics);
          CHECK(metrics.cpu_replays==baseline.cpu_replays+1u &&
                metrics.device_checkpoints==baseline.device_checkpoints);
        } else {
          CHECK(metrics.authority==GML_RENDER_RESOURCE_BOTH_CURRENT &&
                metrics.device_checkpoints==baseline.device_checkpoints+1u &&
                metrics.cpu_replays==baseline.cpu_replays);
        }
      }
      gml_render_gui_end(render);
    }
  }
  /* Characterize the preserved CPU packet policy within one authored draw.
   * A one-code source contributes less than one RGB/alpha code over this
   * transparent destination. SSE2 packets keep zero destination alpha after
   * truncation; the same row's scalar tail forces opaque written alpha.
   * This is implementation evidence, not an original-runner correctness oracle. */
  content.bytecode=15;
  render->tpag[1].tx=render->tpag[1].ty=0;
  render->tpag[1].bw=render->tpag[1].bh=PAGE;
  render->spr[1].w=render->spr[1].h=PAGE;
  render->spr[1].originx=render->spr[1].originy=0;
  gml_render_bind_device(render,NULL);
  gml_render_begin(render,cpu,WIDTH,HEIGHT,0.0,0.0);
  gml_render_gui_begin(render,WIDTH,HEIGHT);
  gml_render_set_pending_fill(render,0x00102030u);
  gml_draw_sprite_ext(render,1,0,5.2,5.2,1.0,1.0,90.0,0xffffffu,1.0);
  gml_render_flush_pending_fill(render);
  gml_render_flush_rotated_batch(render);
  unsigned opaque=0;
  for(unsigned index=0;index<WIDTH*HEIGHT;index++){
    CHECK(cpu[index]==0x00102030u || cpu[index]==0xff102030u);
    opaque+=(cpu[index]>>24)==255u;
  }
#if defined(__SSE2__)
  CHECK(opaque>0u && opaque<16u);
#else
  CHECK(opaque==16u);
#endif
  GmlRenderRotatedCoverageRegion regions[64];
  uint32_t region_count=0,vector_regions=0,scalar_regions=0;
  uint32_t unchanged[WIDTH*HEIGHT];
  memcpy(unchanged,cpu,sizeof unchanged);
  CHECK(gml_render_rotated_nearest_regions(render,&render->spr[1],&render->tpag[1],
          5.2,5.2,1.0,1.0,90.0,regions,64u,&region_count) && region_count>1u);
  for(uint32_t index=0;index<region_count;index++){
    const GmlRenderRect *region=&regions[index].region;
    CHECK(region->x>=0 && region->y>=0 && region->width && region->height==1u &&
          (uint64_t)(uint32_t)region->x+region->width<=WIDTH && region->y<HEIGHT);
    if(index && region->y==regions[index-1u].region.y)
      CHECK((uint64_t)(uint32_t)regions[index-1u].region.x+
            regions[index-1u].region.width<=(uint32_t)region->x);
    vector_regions+=regions[index].vector_alpha!=0u;
    scalar_regions+=regions[index].vector_alpha==0u;
  }
#if defined(__SSE2__)
  CHECK(vector_regions && scalar_regions);
#else
  CHECK(!vector_regions && scalar_regions);
#endif
  CHECK(!gml_render_rotated_nearest_regions(render,&render->spr[1],&render->tpag[1],
          5.2,5.2,1.0,1.0,90.0,regions,1u,&region_count));
  CHECK(!gml_render_rotated_nearest_regions(render,&render->spr[1],&render->tpag[1],
          1e100,5.2,1.0,1.0,90.0,regions,64u,&region_count));
  CHECK(!gml_render_rotated_nearest_regions(render,&render->spr[1],&render->tpag[1],
          5.2,5.2,0x1p-100,1.0,90.0,regions,64u,&region_count));
  CHECK(!gml_render_rotated_nearest_regions(render,&render->spr[1],&render->tpag[1],
          5.2,5.2,1.0,1.0,NAN,regions,64u,&region_count));
  CHECK(!memcmp(unchanged,cpu,sizeof unchanged));
  gml_render_gui_end(render);
  gml_render_free(render);
  free(render);
  return 1;
}

static int resident_zero_alpha_layer_fill_noop(void){
  GmlRender *cpu=(GmlRender*)calloc(1,sizeof *cpu);
  GmlRender *gpu=(GmlRender*)calloc(1,sizeof *gpu);
  uint32_t cpu_pixels[16]={0};
  FakeDevice fake={0};
  GmlRenderDeviceServices device=service(&fake);
  GmlRenderJournalMetrics before={0},after={0};
  CHECK(cpu && gpu);
  CHECK(gml_render_init(cpu,NULL)==0 && gml_render_init(gpu,NULL)==0);
  gml_render_begin(cpu,cpu_pixels,4,4,0.0,0.0);
  CHECK(gml_render_application_surface_ensure_owned(gpu,4,4));
  gml_render_bind_device(gpu,&device);
  CHECK(gml_render_begin_resident_owned_application(gpu,0.0,0.0,0));
  gml_render_set_pending_fill(cpu,0xFF102030u);
  gml_render_set_pending_fill(gpu,0xFF102030u);
  gml_render_resource_journal_metrics(gpu->app_surface_resource,&before);
  gml_draw_layer_color_fill(cpu,0x00556677u,0.0);
  gml_draw_layer_color_fill(gpu,0x00556677u,0.0);
  cpu->blendmode=gpu->blendmode=1;
  gml_draw_layer_color_fill(cpu,0x00334455u,-1.0);
  gml_draw_layer_color_fill(gpu,0x00334455u,-1.0);
  cpu->blendmode=gpu->blendmode=0;
  gml_render_resource_journal_metrics(gpu->app_surface_resource,&after);
  CHECK(gpu->resident_application_pass && gpu->pending_fill && fake.submits==0 &&
        fake.readbacks==0 && after.logical_version==before.logical_version &&
        after.cpu_replays==before.cpu_replays);
  gml_render_primitive_rectangle(cpu,1,1,2,2,0x345678u,0);
  gml_render_primitive_rectangle(gpu,1,1,2,2,0x345678u,0);
  CHECK(gpu->resident_application_pass && fake.submits==2 && fake.readbacks==0);
  gml_draw_layer_color_fill(cpu,0x0000ff00u,0.5);
  gml_draw_layer_color_fill(gpu,0x0000ff00u,0.5);
  gml_render_flush_pending_fill(cpu);
  CHECK(!gpu->resident_application_pass && fake.readbacks==0 &&
        memcmp(cpu_pixels,gpu->app_surface_owned,sizeof cpu_pixels)==0);
  gml_render_free(cpu);
  gml_render_free(gpu);
  free(cpu);
  free(gpu);
  return 1;
}

static int resident_rectangle_fill_noop(void){
  for(unsigned screen=0;screen<2u;screen++) for(unsigned variant=0;variant<8u;variant++){
    GmlRender *cpu=(GmlRender*)calloc(1,sizeof *cpu);
    GmlRender *gpu=(GmlRender*)calloc(1,sizeof *gpu);
    GmlWin content={0};
    uint32_t cpu_pixels[16]={0},screen_pixels[16]={0};
    FakeDevice fake={0};
    GmlRenderDeviceServices device=service(&fake);
    GmlRenderJournalMetrics before={0},after={0};
    CHECK(cpu && gpu && gml_render_init(cpu,NULL)==0 && gml_render_init(gpu,NULL)==0);
    content.bytecode=15;
    cpu->win=gpu->win=&content;
    CHECK(gml_render_application_surface_ensure_owned(gpu,4,4));
    if(screen){
      CHECK(gml_render_application_surface_ensure_owned(cpu,4,4));
      CHECK(gml_render_application_surface_owned_clear(cpu,0xff405060u,NULL));
      CHECK(gml_render_application_surface_owned_clear(gpu,0xff405060u,NULL));
      gml_render_application_surface_select_owned(cpu,1);
      gml_render_application_surface_select_owned(gpu,1);
    }
    gml_render_begin(cpu,cpu_pixels,4,4,0.0,0.0);
    gml_render_bind_device(gpu,&device);
    if(screen){
      CHECK(gml_render_begin_resident_base(gpu,screen_pixels,4,4,0.0,0.0));
      gml_render_gui_begin(cpu,4,4);
      gml_render_gui_begin(gpu,4,4);
      gml_render_gui_set_size(cpu,2,3);
      gml_render_gui_set_size(gpu,2,3);
    } else CHECK(gml_render_begin_resident_owned_application(gpu,0.0,0.0,0));
    GmlRenderResource *resource=screen?gpu->base_scene_resource:gpu->app_surface_resource;
    uint32_t *gpu_pixels=screen?screen_pixels:gpu->app_surface_owned;
    gml_render_set_pending_fill(cpu,0xff102030u);
    gml_render_set_pending_fill(gpu,0xff102030u);
    if(!(variant&2u)){
      gml_render_flush_pending_fill(cpu);
      gml_render_flush_pending_fill(gpu);
    }
    if(screen && (variant&4u)){
      gml_render_set_pending_underlay(cpu,0,0,4,4);
      gml_render_set_pending_underlay(gpu,0,0,4,4);
      CHECK(cpu->pending_underlay && gpu->pending_underlay);
    }
    cpu->alpha=gpu->alpha=variant==7u?-INFINITY:(variant&1u)?-1.0:0.0;
    cpu->blendmode=gpu->blendmode=(variant==4u)?1:0;
    cpu->alphablend=gpu->alphablend=variant!=5u;
    cpu->color_write_mask=gpu->color_write_mask=variant==6u?7u:15u;
    gml_render_resource_journal_metrics(resource,&before);
    int submits=fake.submits,reads=fake.readbacks,uploads=fake.uploads;
    int pending_fill=gpu->pending_fill,pending_underlay=gpu->pending_underlay;
    uint32_t shadow[16];
    memcpy(shadow,gpu_pixels,sizeof shadow);
    for(unsigned operation=0;operation<4u;operation++){
      if(operation<2u){
        int first=operation?7:-2,last=operation?-2:7;
        gml_render_primitive_rectangle(cpu,first,first,last,last,0xffffffu,0);
        gml_render_primitive_rectangle(gpu,first,first,last,last,0xffffffu,0);
      } else {
        uint32_t last=operation==2u?0x123456u:0xabcdefu;
        gml_render_primitive_rectangle_color(cpu,-1,0,6,4,
          0x123456u,0x123456u,0x123456u,last,0);
        gml_render_primitive_rectangle_color(gpu,-1,0,6,4,
          0x123456u,0x123456u,0x123456u,last,0);
      }
    }
    gml_render_resource_journal_metrics(resource,&after);
    CHECK(gpu->resident_application_pass && gpu->pending_fill==pending_fill &&
          gpu->pending_underlay==pending_underlay && fake.submits==submits &&
          fake.readbacks==reads && fake.uploads==uploads &&
          !memcmp(shadow,gpu_pixels,sizeof shadow) &&
          after.logical_version==before.logical_version &&
          after.cpu_version==before.cpu_version && after.gpu_version==before.gpu_version &&
          after.authority==before.authority && after.journal_operations==before.journal_operations &&
          after.cpu_replays==before.cpu_replays && after.device_checkpoints==before.device_checkpoints);
    /* Outlines retain their established CPU write boundary, even at alpha zero.
     * The following positive-alpha fill also proves the guard does not erase real work. */
    cpu->alpha=gpu->alpha=0.0;
    cpu->blendmode=gpu->blendmode=0;
    cpu->alphablend=gpu->alphablend=1;
    cpu->color_write_mask=gpu->color_write_mask=15u;
    gml_render_bind_device(gpu,NULL);
    gml_render_primitive_rectangle(cpu,0,0,2,2,0xffffffu,1);
    gml_render_primitive_rectangle(gpu,0,0,2,2,0xffffffu,1);
    CHECK(!gpu->resident_application_pass && fake.readbacks==reads);
    cpu->alpha=gpu->alpha=0.5;
    gml_render_primitive_rectangle(cpu,0,0,1,1,0xffffffu,0);
    gml_render_primitive_rectangle(gpu,0,0,1,1,0xffffffu,0);
    gml_render_flush_pending_fill(cpu);
    gml_render_flush_pending_fill(gpu);
    CHECK(!memcmp(cpu_pixels,gpu_pixels,sizeof cpu_pixels));
    CHECK(cpu_pixels[0]!=(screen && (variant&4u)?0xff405060u:0xff102030u));
    gml_render_free(cpu);
    gml_render_free(gpu);
    free(cpu);
    free(gpu);
  }
  return 1;
}

static void circle_alpha_test_port(GmlRender *render,unsigned port,int outline,int degenerate){
  double radius=degenerate?0.0:2.25;
  if(port==0u) gml_render_primitive_circle(render,2,2,degenerate?0:2,degenerate?0:2,
                                          0xabcdefu,outline);
  else if(port==1u) gml_render_primitive_circle_subpixel(render,1.5,1.5,radius,radius,
                                                        0xabcdefu,outline);
  else if(port==2u) gml_render_primitive_circle_color(render,2,2,degenerate?0:2,
                                                      degenerate?0:2,0xabcdefu,0x123456u,outline);
  else gml_render_primitive_circle_color_subpixel(render,1.5,1.5,radius,radius,
                                                  0xabcdefu,0x123456u,outline);
}

static int resident_primitive_alpha_noop_case(unsigned circles){
  for(unsigned screen=0;screen<2u;screen++) for(unsigned variant=0;variant<8u;variant++){
    GmlRender *cpu=calloc(1,sizeof *cpu),*gpu=calloc(1,sizeof *gpu);
    GmlWin content={.bytecode=15};
    uint32_t cpu_pixels[16]={0},screen_pixels[16]={0},shadow[16];
    FakeDevice fake={0};
    GmlRenderDeviceServices device=service(&fake);
    GmlRenderJournalMetrics before={0},after={0};
    CHECK(cpu && gpu && !gml_render_init(cpu,NULL) && !gml_render_init(gpu,NULL));
    cpu->win=gpu->win=&content;
    gml_render_begin(cpu,cpu_pixels,4,4,0.0,0.0);
    /* Circle pixels preserve alpha on the owned target but not on SCREEN.
     * Compare like targets; the historical line reference stays unchanged. */
    if(circles && !screen){
      CHECK(gml_render_application_surface_ensure_owned(cpu,4,4));
      CHECK(gml_render_begin_owned_application(cpu,0.0,0.0,0));
    }
    CHECK(gml_render_application_surface_ensure_owned(gpu,4,4));
    gml_render_bind_device(gpu,&device);
    if(screen) CHECK(gml_render_begin_resident_base(gpu,screen_pixels,4,4,0.0,0.0));
    else CHECK(gml_render_begin_resident_owned_application(gpu,0.0,0.0,0));
    if(variant&1u){
      gml_render_gui_begin(cpu,4,4);gml_render_gui_begin(gpu,4,4);
      gml_render_gui_set_size(cpu,2,3);gml_render_gui_set_size(gpu,2,3);
    }
    GmlRenderResource *resource=screen?gpu->base_scene_resource:gpu->app_surface_resource;
    gml_render_set_pending_fill(cpu,0xff102030u);
    gml_render_set_pending_fill(gpu,0xff102030u);
    if(!(variant&2u)){
      gml_render_flush_pending_fill(cpu);gml_render_flush_pending_fill(gpu);
    }
    cpu->alpha=gpu->alpha=variant==7u?-INFINITY:(variant&1u)?-1.0:0.0;
    cpu->blendmode=gpu->blendmode=variant==4u?1:
      variant==5u?GML_RENDER_BLEND_DEST_SOURCE_COLOUR:0;
    cpu->alphablend=gpu->alphablend=variant!=5u;
    cpu->color_write_mask=gpu->color_write_mask=variant==6u?7u:15u;
    int submits=fake.submits,reads=fake.readbacks,uploads=fake.uploads;
    int pending_fill=gpu->pending_fill;
    memcpy(shadow,gpu->fb,sizeof shadow);
    gml_render_resource_journal_metrics(resource,&before);
    if(circles){
      for(unsigned port=0;port<4u;port++) for(int outline=0;outline<2;outline++)
      for(int degenerate=0;degenerate<2;degenerate++){
        circle_alpha_test_port(cpu,port,outline,degenerate);
        circle_alpha_test_port(gpu,port,outline,degenerate);
      }
    } else {
      gml_render_primitive_line(cpu,-2,1,7,1,0xffffffu,2);
      gml_render_primitive_line(gpu,-2,1,7,1,0xffffffu,2);
      gml_render_primitive_line_color(cpu,0,3,3,0,0xffffffu,0x123456u,1);
      gml_render_primitive_line_color(gpu,0,3,3,0,0xffffffu,0x123456u,1);
      gml_render_primitive_line_color_subpixel(cpu,-0.5,2.75,4.5,1.25,
                                              0xabcdefu,0x345678u,2);
      gml_render_primitive_line_color_subpixel(gpu,-0.5,2.75,4.5,1.25,
                                              0xabcdefu,0x345678u,2);
    }
    gml_render_resource_journal_metrics(resource,&after);
    CHECK(gpu->resident_application_pass && gpu->pending_fill==pending_fill &&
          fake.submits==submits && fake.readbacks==reads && fake.uploads==uploads &&
          !memcmp(shadow,gpu->fb,sizeof shadow) &&
          after.logical_version==before.logical_version &&
          after.cpu_version==before.cpu_version && after.gpu_version==before.gpu_version &&
          after.authority==before.authority && after.journal_operations==before.journal_operations &&
          after.cpu_replays==before.cpu_replays && after.device_checkpoints==before.device_checkpoints);
    /* Positive alpha still reaches the established CPU boundary in GUI/partial state. */
    cpu->alpha=gpu->alpha=0.5;cpu->blendmode=gpu->blendmode=0;
    cpu->alphablend=gpu->alphablend=1;cpu->color_write_mask=gpu->color_write_mask=15u;
    if(circles){
      gml_render_primitive_circle_color_subpixel(cpu,1.5,1.5,2.0,2.0,0xffffffu,0xffffffu,0);
      gml_render_primitive_circle_color_subpixel(gpu,1.5,1.5,2.0,2.0,0xffffffu,0xffffffu,0);
    } else {
      gml_render_primitive_line_color_subpixel(cpu,0,1,3,1,0xffffffu,0xffffffu,2);
      gml_render_primitive_line_color_subpixel(gpu,0,1,3,1,0xffffffu,0xffffffu,2);
    }
    gml_render_flush_pending_fill(cpu);
    if(circles && cpu->fb!=cpu_pixels) memcpy(cpu_pixels,cpu->fb,sizeof cpu_pixels);
    CHECK(!gpu->resident_application_pass && !memcmp(cpu_pixels,gpu->fb,sizeof cpu_pixels));
    CHECK(cpu_pixels[circles?5:4]!=0xff102030u);
    gml_render_free(cpu);gml_render_free(gpu);free(cpu);free(gpu);
  }
  for(int mode=GML_RENDER_BLEND_DEST_SOURCE_COLOUR;
      mode<=GML_RENDER_BLEND_DEST_ALPHA_DEST_COLOUR;mode++)
  for(unsigned port=0;port<(circles?4u:3u);port++)
  for(unsigned shape=0;shape<(circles?4u:1u);shape++){
    GmlRender *cpu=calloc(1,sizeof *cpu),*gpu=calloc(1,sizeof *gpu);
    uint32_t pixels[16]={0};
    FakeDevice fake={0};GmlRenderDeviceServices device=service(&fake);
    CHECK(cpu && gpu && !gml_render_init(cpu,NULL) && !gml_render_init(gpu,NULL));
    gml_render_begin(cpu,pixels,4,4,0.0,0.0);
    if(circles){
      CHECK(gml_render_application_surface_ensure_owned(cpu,4,4));
      CHECK(gml_render_begin_owned_application(cpu,0.0,0.0,0));
    }
    CHECK(gml_render_application_surface_ensure_owned(gpu,4,4));
    gml_render_bind_device(gpu,&device);
    CHECK(gml_render_begin_resident_owned_application(gpu,0.0,0.0,0));
    gml_render_set_pending_fill(cpu,0x80102030u);gml_render_set_pending_fill(gpu,0x80102030u);
    cpu->alpha=gpu->alpha=0.0;cpu->blendmode=gpu->blendmode=mode;
    for(unsigned side=0;side<2u;side++){
      GmlRender *r=side?gpu:cpu;
      if(circles) circle_alpha_test_port(r,port,(int)(shape&1u),(int)(shape>>1));
      else if(port==0u) gml_render_primitive_line(r,0,1,3,1,0xabcdefu,1);
      else if(port==1u) gml_render_primitive_line_color(r,0,1,3,1,0xabcdefu,0x123456u,1);
      else gml_render_primitive_line_color_subpixel(r,0.0,1.0,3.0,1.0,0xabcdefu,0x123456u,1);
    }
    if(circles) memcpy(pixels,cpu->fb,sizeof pixels);
    CHECK(!gpu->resident_application_pass && !memcmp(pixels,gpu->fb,sizeof pixels));
    if(circles){
      int changed=0;
      for(unsigned pixel=0;pixel<16u;pixel++) changed|=pixels[pixel]!=0x80102030u;
      CHECK(changed);
    } else CHECK(pixels[4]!=0x80102030u);
    gml_render_free(cpu);gml_render_free(gpu);free(cpu);free(gpu);
  }
  return 1;
}

static int resident_line_alpha_noop(void){
  return resident_primitive_alpha_noop_case(0u);
}

static int resident_circle_alpha_noop(void){
  return resident_primitive_alpha_noop_case(1u);
}

static int resident_unit_world_primitive_order(void){
  const double scales[][2]={{1.0,1.0},{0.5,0.50370370370370365},
                            {2.0,0.75},{-1.0,1.5}};
  for(unsigned variant=0;variant<sizeof scales/sizeof scales[0];variant++){
    GmlRender *cpu=(GmlRender*)calloc(1,sizeof *cpu);
    GmlRender *gpu=(GmlRender*)calloc(1,sizeof *gpu);
    uint32_t cpu_pixels[16]={0};
    FakeDevice fake={0};
    GmlRenderDeviceServices device=service(&fake);
    GmlRenderJournalMetrics before={0},after={0};
    CHECK(cpu && gpu);
    CHECK(gml_render_init(cpu,NULL)==0 && gml_render_init(gpu,NULL)==0);
    gml_render_begin(cpu,cpu_pixels,4,4,0.0,0.0);
    CHECK(gml_render_application_surface_ensure_owned(gpu,4,4));
    gml_render_bind_device(gpu,&device);
    CHECK(gml_render_begin_resident_owned_application(gpu,0.0,0.0,0));
    cpu->world_transform_active=gpu->world_transform_active=1;
    cpu->world_scale_x=cpu->world_scale_y=1.0;
    gpu->world_scale_x=gpu->world_scale_y=1.0;
    gml_render_set_pending_fill(cpu,0xFF102030u);
    gml_render_set_pending_fill(gpu,0xFF102030u);
    gml_render_primitive_rectangle(cpu,-1,1,2,3,0x345678u,0);
    gml_render_primitive_rectangle(gpu,-1,1,2,3,0x345678u,0);
    gml_render_resource_journal_metrics(gpu->app_surface_resource,&before);
    CHECK(gpu->resident_application_pass && fake.submits==2 && fake.readbacks==0 &&
          before.journal_operations==2u && before.cpu_replays==0u);
    cpu->world_scale_x=gpu->world_scale_x=scales[variant][0];
    cpu->world_scale_y=gpu->world_scale_y=scales[variant][1];
    /* These entries receive raster coordinates, even while the world map is nonidentity. */
    gml_render_primitive_rectangle(cpu,0,0,4,4,0x112233u,0);
    gml_render_primitive_rectangle(gpu,0,0,4,4,0x112233u,0);
    gml_render_primitive_point(cpu,3,0,0x0000ffu);
    gml_render_primitive_point(gpu,3,0,0x0000ffu);
    gml_render_primitive_rectangle_color(cpu,0,0,0,0,0xabcdefu,0xabcdefu,
                                          0xabcdefu,0xabcdefu,0);
    gml_render_primitive_rectangle_color(gpu,0,0,0,0,0xabcdefu,0xabcdefu,
                                          0xabcdefu,0xabcdefu,0);
    gml_render_primitive_line(cpu,0,2,2,2,0x00ff00u,1);
    gml_render_primitive_line(gpu,0,2,2,2,0x00ff00u,1);
    gml_render_primitive_line_color(cpu,2,0,2,2,0xff0000u,0xff0000u,1);
    gml_render_primitive_line_color(gpu,2,0,2,2,0xff0000u,0xff0000u,1);
    gml_render_resource_journal_metrics(gpu->app_surface_resource,&after);
    CHECK(gpu->resident_application_pass && fake.submits==7 && fake.readbacks==0 &&
          after.journal_operations==7u && after.cpu_replays==before.cpu_replays);
    /* Keep the original ordering proof at a real CPU-only partial-alpha boundary. */
    cpu->alpha=gpu->alpha=0.5;
    gml_render_primitive_line(cpu,0,0,3,3,0xffffffu,1);
    gml_render_primitive_line(gpu,0,0,3,3,0xffffffu,1);
    gml_render_flush_pending_fill(cpu);
    gml_render_resource_journal_metrics(gpu->app_surface_resource,&after);
    CHECK(!gpu->resident_application_pass && fake.submits==7 && fake.readbacks==0 &&
          after.cpu_replays==before.cpu_replays+1u &&
          memcmp(cpu_pixels,gpu->app_surface_owned,sizeof cpu_pixels)==0);
    gml_render_free(cpu);
    gml_render_free(gpu);
    free(cpu);
    free(gpu);
  }
  return 1;
}

static int resident_gradient_primitive_cpu_boundary(void){
  GmlRender *cpu=(GmlRender*)calloc(1,sizeof *cpu);
  GmlRender *gpu=(GmlRender*)calloc(1,sizeof *gpu);
  uint32_t cpu_pixels[64]={0},prefix[64]={0};
  FakeDevice fake={0};
  GmlRenderDeviceServices device=service(&fake);
  GmlRenderJournalMetrics before={0},after={0};
  CHECK(cpu && gpu);
  CHECK(gml_render_init(cpu,NULL)==0 && gml_render_init(gpu,NULL)==0);
  gml_render_begin(cpu,cpu_pixels,8,8,0.0,0.0);
  CHECK(gml_render_application_surface_ensure_owned(gpu,8,8));
  gml_render_bind_device(gpu,&device);
  CHECK(gml_render_begin_resident_owned_application(gpu,0.0,0.0,0));
  gml_render_set_pending_fill(cpu,0xFF102030u);
  gml_render_set_pending_fill(gpu,0xFF102030u);
  gml_render_primitive_rectangle(cpu,0,7,1,7,0xabcdefu,0);
  gml_render_primitive_rectangle(gpu,0,7,1,7,0xabcdefu,0);
  memcpy(prefix,cpu_pixels,sizeof prefix);
  gml_render_resource_journal_metrics(gpu->app_surface_resource,&before);
  gml_render_primitive_rectangle_color(cpu,-3,1,6,6,
                                        0x0000ffu,0x00ff00u,0xff0000u,0xffffffu,0);
  gml_render_primitive_rectangle_color(gpu,-3,1,6,6,
                                        0x0000ffu,0x00ff00u,0xff0000u,0xffffffu,0);
  gml_render_primitive_circle_color(cpu,4,4,2,2,0x123456u,0x654321u,0);
  gml_render_primitive_circle_color(gpu,4,4,2,2,0x123456u,0x654321u,0);
  gml_render_flush_pending_fill(cpu);
  gml_render_resource_journal_metrics(gpu->app_surface_resource,&after);
  CHECK(memcmp(prefix,cpu_pixels,sizeof prefix)!=0 &&
        !gpu->resident_application_pass && fake.submits==2 && fake.readbacks==0 &&
        after.logical_version>before.logical_version &&
        after.cpu_version==after.logical_version &&
        after.gpu_version==before.gpu_version &&
        after.cpu_replays==before.cpu_replays+1u &&
        memcmp(cpu_pixels,gpu->app_surface_owned,sizeof cpu_pixels)==0);
  gml_render_free(cpu);
  gml_render_free(gpu);
  free(cpu);
  free(gpu);
  return 1;
}

static int resident_particle_square_public_order(void){
  GmlRender *cpu=(GmlRender*)calloc(1,sizeof *cpu);
  GmlRender *gpu=(GmlRender*)calloc(1,sizeof *gpu);
  GmlParticleState *particles=gml_particle_state_create(NULL);
  uint32_t cpu_pixels[64]={0};
  FakeDevice fake={0};
  GmlRenderDeviceServices device=service(&fake);
  GmlRenderJournalMetrics metrics={0};
  CHECK(cpu && gpu && particles);
  CHECK(gml_render_init(cpu,NULL)==0 && gml_render_init(gpu,NULL)==0);
  int system=gml_part_system_create(particles);
  int opaque=gml_part_type_create(particles);
  int translucent=gml_part_type_create(particles);
  CHECK(system && opaque && translucent);
  gml_part_type_shape(particles,opaque,0);
  gml_part_type_size(particles,opaque,1,1,0,0);
  gml_part_type_alpha(particles,opaque,1,1,1,1);
  gml_part_particles_create_color(particles,system,3,3,opaque,0x345678u,1);
  gml_render_begin(cpu,cpu_pixels,8,8,0.0,0.0);
  CHECK(gml_render_application_surface_ensure_owned(gpu,8,8));
  gml_render_bind_device(gpu,&device);
  CHECK(gml_render_begin_resident_owned_application(gpu,0.0,0.0,0));
  gml_render_set_pending_fill(cpu,0xFF102030u);
  gml_render_set_pending_fill(gpu,0xFF102030u);
  gml_part_system_drawit(particles,cpu,system);
  gml_part_system_drawit(particles,gpu,system);
  gml_render_resource_journal_metrics(gpu->app_surface_resource,&metrics);
  CHECK(gpu->resident_application_pass && fake.submits==2 && fake.readbacks==0 &&
        metrics.journal_operations==2u && metrics.cpu_replays==0u);
  gml_part_system_clear(particles,system);
  gml_part_type_shape(particles,translucent,0);
  gml_part_type_size(particles,translucent,1,1,0,0);
  gml_part_type_alpha(particles,translucent,1,0.5,0.5,0.5);
  gml_part_particles_create_color(particles,system,5,5,translucent,0x00ff00u,1);
  gml_part_system_drawit(particles,cpu,system);
  gml_part_system_drawit(particles,gpu,system);
  gml_render_flush_pending_fill(cpu);
  CHECK(!gpu->resident_application_pass);
  CHECK(fake.submits==2 && fake.readbacks==0);
  CHECK(memcmp(cpu_pixels,gpu->app_surface_owned,sizeof cpu_pixels)==0);
  gml_particle_state_destroy(particles);
  gml_render_free(cpu);
  gml_render_free(gpu);
  free(cpu);
  free(gpu);
  return 1;
}

static int resident_particle_line_public_order(void){
  GmlRender *gpu=(GmlRender*)calloc(1,sizeof *gpu);
  GmlParticleState *particles=gml_particle_state_create(NULL);
  FakeDevice fake={.on_demand=1,.readback_colour=0x7F123456u};
  GmlRenderDeviceServices device=service(&fake);
  GmlRenderJournalMetrics metrics={0};
  CHECK(gpu && particles && gml_render_init(gpu,NULL)==0);
  device.observe_on_demand=fake_observe_on_demand;
  CHECK(gml_render_application_surface_ensure_owned(gpu,96,48));
  gml_render_bind_device(gpu,&device);
  CHECK(gml_render_begin_resident_owned_application(gpu,0.0,0.0,0));
  int system=gml_part_system_create(particles),type=gml_part_type_create(particles);
  CHECK(system && type);
  gml_part_type_shape(particles,type,3);
  gml_part_type_size(particles,type,1,1,0,0);
  gml_part_type_scale(particles,type,0.75,1.5);
  gml_part_type_orientation(particles,type,17,17,0,0,0);
  gml_part_type_alpha(particles,type,1,0.5,0.5,0.5);
  gml_part_particles_create_color(particles,system,40,24,type,0x123456u,8);
  gml_render_set_pending_fill(gpu,0x7F203040u);
  gml_part_system_drawit(particles,gpu,system);
  CHECK(gpu->resident_application_pass && fake.submits==1 && fake.readbacks==0);
  CHECK(gml_render_scene_submit_pending(gpu));
  CHECK(fake.submits==2 && fake.last_batch_operations==8u &&
        fake.last_batch_states==0u && fake.last_source_count==0u &&
        fake.last_primitive.payload.primitive.kind==GML_RENDER_PRIMITIVE_PARTICLE_LINE &&
        gml_render_particle_line_valid(&fake.last_primitive));
  gml_render_resource_journal_metrics(gpu->app_surface_resource,&metrics);
  CHECK(metrics.authority==GML_RENDER_RESOURCE_GPU_RESIDENT &&
        metrics.journal_operations==0u && metrics.cpu_replays==0u && fake.readbacks==0);
  GmlRenderDrawOp valid=fake.last_primitive,invalid=valid;
  invalid.payload.primitive.alpha=NAN;
  CHECK(!gml_render_particle_line_valid(&invalid));
  invalid=valid; invalid.target.image.resource_class=GML_RENDER_RESOURCE_ATLAS;
  CHECK(!gml_render_particle_line_valid(&invalid));
  invalid=valid; invalid.reconstruction_mode=GML_RENDER_REPLAY_EXACT;
  CHECK(!gml_render_particle_line_valid(&invalid) &&
        !gml_render_cpu_replay_eligible(&invalid,NULL,NULL));
  int submits=fake.submits;
  CHECK(!gml_render_try_particle_line(gpu,40,24,0,1,0,0x123456u,1));
  CHECK(!gml_render_try_particle_line(gpu,40,24,1,1,0,0x123456u,NAN));
  gpu->color_write_mask=7u;
  CHECK(!gml_render_try_particle_line(gpu,40,24,1,1,0,0x123456u,1));
  gpu->color_write_mask=15u;
  CHECK(gml_render_try_particle_line(gpu,-1024,-1024,1,1,0,0x123456u,1));
  CHECK(fake.submits==submits && fake.readbacks==0);
  /* An earlier non-coalescible textured span leaves reusable array slots, not
   * live source/state entries for a later source-free primitive span. */
  uint32_t source_pixels[4]={0xFF7193B5u,0xFF7193B5u,0xFF7193B5u,0xFF7193B5u};
  GmlRenderResourceSource source=source_view(source_pixels,1);
  GmlRenderDrawOp quad=quad_op(1u,&source.image);
  GmlRenderDrawStateSnapshot state=quad_state(0);
  quad.reconstruction_mode=GML_RENDER_DEVICE_CHECKPOINT;
  quad.target=valid.target;
  quad.target.image=gml_render_application_scene_next_image_ref(gpu,gpu->app_surface_resource);
  quad.target.image.content_version++;
  CHECK(gml_render_application_scene_enqueue(gpu,&quad,&state,&source) &&
        gml_render_scene_submit_pending(gpu));
  CHECK(fake.last_batch_operations==1u && fake.last_batch_states==1u &&
        fake.last_source_count==1u && fake.readbacks==0);
  CHECK(gml_render_try_particle_line(gpu,40,24,1,1,0,0x123456u,0.25));
  submits=fake.submits;
  quad.target.image=gml_render_application_scene_next_image_ref(gpu,gpu->app_surface_resource);
  quad.target.image.content_version++;
  CHECK(gml_render_application_scene_enqueue(gpu,&quad,&state,&source));
  CHECK(fake.submits==submits+1 && fake.last_batch_operations==1u &&
        fake.last_batch_states==0u && fake.last_source_count==0u);
  CHECK(gml_render_scene_submit_pending(gpu) && fake.submits==submits+2 &&
        fake.last_batch_operations==1u && fake.last_batch_states==1u &&
        fake.last_source_count==1u && fake.readbacks==0);
  submits=fake.submits;
  CHECK(gml_render_try_particle_line(gpu,40,24,1,1,0,0x123456u,0.25));
  fake.fail_readback=1;
  GmlRenderApplicationWriteView observed={0};
  CHECK(!gml_render_application_surface_owned_view(gpu,&observed));
  CHECK(fake.submits==submits+1 && fake.readbacks==1 && gpu->resident_application_pass);
  fake.fail_readback=0;
  CHECK(gml_render_application_surface_owned_view(gpu,&observed));
  CHECK(fake.submits==submits+1 && fake.readbacks==2 &&
        observed.pixels[0]==fake.readback_colour);
  gml_particle_state_destroy(particles);
  gml_render_free(gpu);
  free(gpu);
  return 1;
}

static int particle_line_recovery_uses_cpu_raster(void){
  enum { WIDTH=96,HEIGHT=48 };
  static const double angles[]={0,17,90,-43};
  GmlRender *cpu=(GmlRender*)calloc(1,sizeof *cpu);
  GmlRender *gpu=(GmlRender*)calloc(1,sizeof *gpu);
  GmlParticleState *particles=gml_particle_state_create(NULL);
  uint32_t cpu_pixels[WIDTH*HEIGHT],replayed[WIDTH*HEIGHT];
  CHECK(cpu && gpu && particles && gml_render_init(cpu,NULL)==0 &&
        gml_render_init(gpu,NULL)==0);
  CHECK(gml_render_application_surface_ensure_owned(gpu,WIDTH,HEIGHT));
  int system=gml_part_system_create(particles),type=gml_part_type_create(particles);
  CHECK(system && type);
  gml_part_type_shape(particles,type,3);
  gml_part_type_size(particles,type,1,1,0,0);
  gml_part_type_alpha(particles,type,1,0.375,0.375,0.375);
  for(unsigned test=0;test<16u;test++){
    FakeDevice fake={.on_demand=1};
    GmlRenderDeviceServices device=service(&fake);
    device.observe_on_demand=fake_observe_on_demand;
    gml_render_bind_device(gpu,&device);
    CHECK(gml_render_begin_resident_owned_application(gpu,0.0,0.0,0));
    for(unsigned pixel=0;pixel<WIDTH*HEIGHT;pixel++)
      cpu_pixels[pixel]=replayed[pixel]=0x49273859u;
    gml_render_begin(cpu,cpu_pixels,WIDTH,HEIGHT,0.0,0.0);
    cpu->interp=gpu->interp=(test>>2u)&1u;
    cpu->alphablend=gpu->alphablend=(test>>3u)&1u;
    gml_part_type_scale(particles,type,test&1u?-0.75:1.25,test&2u?-1.5:0.5);
    gml_part_type_orientation(particles,type,angles[test%4u],angles[test%4u],0,0,0);
    gml_part_system_clear(particles,system);
    gml_part_particles_create_color(particles,system,test&1u?4.25:48.25,23.75,
                                    type,0x7193B5u,1);
    gml_render_set_pending_fill(gpu,0x49273859u);
    gml_part_system_drawit(particles,cpu,system);
    gml_part_system_drawit(particles,gpu,system);
    CHECK(gpu->resident_application_pass && gml_render_scene_submit_pending(gpu));
    CHECK(fake.last_batch_operations==1u && fake.readbacks==0 &&
          gml_render_particle_line_valid(&fake.last_primitive));
    CHECK(gml_render_cpu_replay_one(replayed,WIDTH,HEIGHT,&fake.last_primitive,NULL,NULL));
    CHECK(memcmp(cpu_pixels,replayed,sizeof replayed)==0);
    gml_render_bind_device(gpu,NULL);
  }
  gml_particle_state_destroy(particles);
  gml_render_free(cpu); gml_render_free(gpu);
  free(cpu); free(gpu);
  return 1;
}

static int resident_software3d_primitive_cpu_bridge(void){
  GmlRender *cpu=(GmlRender*)calloc(1,sizeof *cpu);
  GmlRender *gpu=(GmlRender*)calloc(1,sizeof *gpu);
  GmlSoftware3D *cpu_graphics=gml_software3d_create();
  GmlSoftware3D *gpu_graphics=gml_software3d_create();
  uint32_t cpu_pixels[64]={0};
  FakeDevice fake={0};
  GmlRenderDeviceServices device=service(&fake);
  GmlRenderJournalMetrics before={0},after={0};
  CHECK(cpu && gpu && cpu_graphics && gpu_graphics);
  CHECK(gml_render_init(cpu,NULL)==0 && gml_render_init(gpu,NULL)==0);
  gml_software3d_reset(cpu_graphics);
  gml_software3d_reset(gpu_graphics);
  gml_render_bind_software3d(cpu,cpu_graphics);
  gml_render_bind_software3d(gpu,gpu_graphics);
  gml_render_begin(cpu,cpu_pixels,8,8,0.0,0.0);
  CHECK(gml_render_application_surface_ensure_owned(gpu,8,8));
  gml_render_bind_device(gpu,&device);
  CHECK(gml_render_begin_resident_owned_application(gpu,0.0,0.0,0));
  gml_render_set_pending_fill(cpu,0xFF102030u);
  gml_render_set_pending_fill(gpu,0xFF102030u);
  gml_render_primitive_rectangle(cpu,1,1,3,3,0x0000ffu,0);
  gml_render_primitive_rectangle(gpu,1,1,3,3,0x0000ffu,0);
  gml_render_resource_journal_metrics(gpu->app_surface_resource,&before);
  gml_software3d_primitive_2d_begin(cpu_graphics,1,-1);
  gml_software3d_primitive_2d_begin(gpu_graphics,1,-1);
  CHECK(gml_software3d_primitive_2d_append(cpu_graphics,3,3,0,0,0x00ff00u,1.0));
  CHECK(gml_software3d_primitive_2d_append(gpu_graphics,3,3,0,0,0x00ff00u,1.0));
  gml_software3d_primitive_2d_end(cpu);
  gml_software3d_primitive_2d_end(gpu);
  gml_render_resource_journal_metrics(gpu->app_surface_resource,&after);
  gml_render_flush_pending_fill(cpu);
  CHECK(!gpu->resident_application_pass && fake.submits==2 && fake.readbacks==0 &&
        memcmp(cpu_pixels,gpu->app_surface_owned,sizeof cpu_pixels)==0 &&
        cpu_pixels[3u*8u+3u]!=0xFF102030u);
  CHECK(after.logical_version>before.logical_version &&
        after.cpu_version==after.logical_version &&
        after.gpu_version==before.gpu_version &&
        after.cpu_replays==before.cpu_replays+1u);
  gml_render_free(cpu);
  gml_render_free(gpu);
  gml_software3d_destroy(cpu_graphics);
  gml_software3d_destroy(gpu_graphics);
  free(cpu);
  free(gpu);
  return 1;
}

static int resident_filled_span_public_order(void){
  GmlRender *cpu=(GmlRender*)calloc(1,sizeof *cpu);
  GmlRender *gpu=(GmlRender*)calloc(1,sizeof *gpu);
  uint32_t cpu_pixels[64]={0};
  FakeDevice fake={0};
  GmlRenderDeviceServices device=service(&fake);
  GmlRenderJournalMetrics metrics={0};
  CHECK(cpu && gpu);
  CHECK(gml_render_init(cpu,NULL)==0 && gml_render_init(gpu,NULL)==0);
  gml_render_begin(cpu,cpu_pixels,8,8,0.0,0.0);
  CHECK(gml_render_application_surface_ensure_owned(gpu,8,8));
  gml_render_bind_device(gpu,&device);
  CHECK(gml_render_begin_resident_owned_application(gpu,0.0,0.0,0));
  gml_render_set_pending_fill(cpu,0xFF101010u);
  gml_render_set_pending_fill(gpu,0xFF101010u);
  gml_render_primitive_circle(cpu,2,3,2,2,0x0000FFu,0);
  gml_render_primitive_circle(gpu,2,3,2,2,0x0000FFu,0);
  gml_render_primitive_triangle_alpha(cpu,4,1,7,1,4,6,0x00FF00u,1.0,0);
  gml_render_primitive_triangle_alpha(gpu,4,1,7,1,4,6,0x00FF00u,1.0,0);
  gml_render_resource_journal_metrics(gpu->app_surface_resource,&metrics);
  CHECK(gpu->resident_application_pass && fake.submits==3 && fake.readbacks==0 &&
        metrics.journal_operations>3u && metrics.cpu_replays==0u);
  gml_render_flush_pending_fill(cpu);
  CHECK(cpu_pixels[3u*8u+2u]==0xFFFF0000u &&
        cpu_pixels[2u*8u+5u]==0xFF00FF00u);
  gml_render_bind_device(gpu,NULL);
  CHECK(gml_render_resident_application_cpu_barrier(gpu) &&
        !gpu->resident_application_pass && fake.readbacks==0 &&
        memcmp(cpu_pixels,gpu->app_surface_owned,sizeof cpu_pixels)==0);
  gml_render_free(cpu);
  gml_render_free(gpu);
  free(cpu);
  free(gpu);
  return 1;
}

static int resident_borrowed_application_screen_handoff(void){
  for(unsigned policy=0;policy<3u;policy++) for(unsigned lifecycle=0;lifecycle<4u;lifecycle++){
    GmlRender *r=calloc(1,sizeof *r);
    GmlWin content={.classic_version=policy==0u?530:policy==1u?600:700};
    FakeDevice fake={.on_demand=1,.readback_colour=0xFF315779u};
    GmlRenderDeviceServices device=service(&fake);
    uint32_t world[16*12]={0},screen[16*12]={0},third[16*12]={0};
    GmlRenderImageRef world_image={0},screen_image={0},image={0};
    CHECK(r && !gml_render_init(r,NULL));r->win=&content;r->classic=1;
    device.observe_on_demand=fake_observe_on_demand;
    gml_render_bind_device(r,&device);gml_render_set_scene_device(r,1);
    gml_render_application_surface_bind(r,world,16,12,1);
    CHECK(gml_render_begin_resident_base(r,world,16,12,0,0));
    gml_render_set_pending_fill(r,0xFF315779u);gml_render_flush_pending_fill(r);
    CHECK(gml_render_completed_screen_image(r,world,16,12,&world_image));
    int retired=fake.retires;
    CHECK(gml_render_begin_resident_base(r,screen,16,12,0,0));
    CHECK(fake.retires==retired && !fake.readbacks &&
          gml_render_completed_screen_image(r,world,16,12,&image) &&
          image.incarnation==world_image.incarnation &&
          image.content_version==world_image.content_version);
    CHECK(r->app_surface==world && !r->app_surface_owned && r->app_w==16 && r->app_h==12);
    gml_render_set_pending_fill(r,0xFF112233u);gml_render_flush_pending_fill(r);
    gml_render_set_pending_underlay(r,0,0,16,12);
    CHECK(gml_render_compose_resident_underlay(r) && gml_render_scene_submit_pending(r) &&
          !r->pending_underlay && r->resident_application_pass && !fake.readbacks &&
          fake.last_opcode==GML_RENDER_OP_TEXTURED_QUAD);
    CHECK(gml_render_completed_screen_image(r,screen,16,12,&screen_image));
    /* The next Draw must recover the same image, not upload a stale CPU mirror. */
    CHECK(gml_render_begin_resident_base(r,world,16,12,0,0) &&
          gml_render_completed_screen_image(r,world,16,12,&image) &&
          image.incarnation==world_image.incarnation &&
          gml_render_completed_screen_image(r,screen,16,12,&image) &&
          image.incarnation==screen_image.incarnation && !fake.readbacks &&
          fake.retires==retired && !world[0] && !screen[0]);
    /* A third allocation cannot evict either still-live image without observation. */
    CHECK(!gml_render_begin_resident_base(r,third,16,12,0,0) &&
          !fake.readbacks && fake.retires==retired &&
          gml_render_completed_screen_image(r,world,16,12,&image) &&
          image.incarnation==world_image.incarnation);
    CHECK(gml_render_begin_resident_base(r,screen,16,12,0,0) &&
          gml_render_checkpoint_pending_resources(r));
    CHECK(fake.readbacks==2 && world[0]==fake.readback_colour && screen[0]==fake.readback_colour);
    if(lifecycle==1u){
      size_t capacity=gml_render_state_size(r,-1),written=0,used=0;
      void *bytes=malloc(capacity);CHECK(bytes && capacity);
      CHECK(gml_render_state_save(r,-1,bytes,capacity,&written) && written==capacity &&
            gml_render_state_load(r,bytes,written,&used) && used==written);
      CHECK(!r->base_scene_resource && !r->retained_base_resource && fake.readbacks==2);
      free(bytes);
    } else if(lifecycle>=2u){
      CHECK(gml_render_begin_resident_base(r,world,16,12,0,0));
      gml_render_set_pending_fill(r,0xFF556677u);gml_render_flush_pending_fill(r);
      if(lifecycle==2u) CHECK(gml_render_begin_resident_base(r,screen,16,12,0,0));
      CHECK(gml_render_device_observation_pending(r));
      fake.forbid_calls=1;
      gml_render_lose_volatile_device(r);
      CHECK(!fake.forbidden_calls && !r->base_scene_resource && !r->retained_base_resource &&
            !gml_render_device(r) && !r->app_surface_opaque && fake.readbacks==2);
      fake.forbid_calls=0;
    }
    gml_render_bind_device(r,NULL);
    int width=0,height=0;
    CHECK(gml_surface_pixels_read(r,0,&width,&height)==world && width==16 && height==12 &&
          !r->app_surface_owned && fake.readbacks==2);
    gml_render_free(r);free(r);
  }
  return 1;
}

static int resident_line_function_transaction(void){
  GmlRender *r=calloc(1,sizeof *r);
  GmlWin content={.classic_version=530};
  FakeDevice fake={.on_demand=1};GmlRenderDeviceServices device=service(&fake);
  uint32_t screen[16*12]={0},shadow[16*12];
  device.observe_on_demand=fake_observe_on_demand;
  CHECK(r && !gml_render_init(r,NULL));r->win=&content;r->classic=1;
  gml_render_bind_device(r,&device);
  gml_render_application_surface_bind(r,screen,16,12,1);
  CHECK(gml_render_begin_resident_base(r,screen,16,12,0,0));
  gml_render_set_pending_fill(r,0x40123456u);gml_render_flush_pending_fill(r);
  CHECK(gml_render_scene_submit_pending(r));memcpy(shadow,screen,sizeof shadow);
  int before=fake.submits;
  gml_render_primitive_line_color(r,7,10,7,2,0x345678u,0x345678u,1);
  CHECK(r->resident_application_pass && !fake.readbacks && fake.submits==before);
  CHECK(gml_render_scene_submit_pending(r) && fake.submits==before+1);
  CHECK(fake.last_batch_operations==1u && gml_render_rectangle_valid(&fake.last_primitive));
  GmlRenderDrawOp prototype=fake.last_primitive;
  CHECK(prototype.payload.primitive.region.x==7 && prototype.payload.primitive.region.y==2 &&
        prototype.payload.primitive.region.width==1u && prototype.payload.primitive.region.height==9u);
  /* A malformed suffix cannot accept its valid prefix or discard a prior line. */
  gml_render_primitive_line(r,2,1,2,8,0xabcdefu,1);
  GmlRenderResource *target=gml_render_scene_target_resource_ensure(r);
  GmlRenderImageRef next=gml_render_application_scene_next_image_ref(r,target);
  GmlRenderDrawOp pair[2]={prototype,prototype};
  for(unsigned i=0;i<2u;i++){
    pair[i].target.image=next;pair[i].target.image.content_version+=i+1u;
  }
  pair[1].payload.primitive.region.width=0;
  before=fake.submits;
  CHECK(!gml_render_application_scene_enqueue_primitives(r,pair,2u));
  CHECK(fake.submits==before &&
        gml_render_application_scene_next_image_ref(r,target).content_version==next.content_version);
  CHECK(gml_render_scene_submit_pending(r) && fake.last_batch_operations==1u);
  /* A complete draw larger than the pending queue uses one direct batch. */
  enum { LARGE_GROUP=2051 };
  GmlRenderDrawOp *large=calloc(LARGE_GROUP,sizeof *large);CHECK(large);
  next=gml_render_application_scene_next_image_ref(r,target);
  for(unsigned i=0;i<LARGE_GROUP;i++){
    large[i]=prototype;large[i].target.image=next;
    large[i].target.image.content_version+=i+1u;
  }
  CHECK(gml_render_application_scene_enqueue_primitives(r,large,LARGE_GROUP));
  CHECK(fake.last_batch_operations==LARGE_GROUP &&
        gml_render_application_scene_next_image_ref(r,target).content_version==next.content_version+LARGE_GROUP);
  free(large);
  r->alpha=.5;r->alpha_test_enable=1;r->alpha_test_ref=255u;
  gml_render_primitive_line_color_subpixel(r,1.25,2.75,13.25,9.25,0x345678u,0xabcdefu,3);
  CHECK(r->resident_application_pass && gml_render_scene_submit_pending(r));
  CHECK(fake.last_batch_operations>1u && fake.last_primitive.payload.primitive.alpha==.5);
  CHECK(r->alpha_test_enable==1 && r->alpha_test_ref==255u && !fake.readbacks &&
        !memcmp(shadow,screen,sizeof shadow));
  gml_render_free(r);free(r);return 1;
}

static int resident_line_coverage_public_order(void){
  GmlRender *cpu=(GmlRender*)calloc(1,sizeof *cpu);
  GmlRender *gpu=(GmlRender*)calloc(1,sizeof *gpu);
  uint32_t cpu_pixels[64]={0};
  FakeDevice fake={0};
  GmlRenderDeviceServices device=service(&fake);
  GmlRenderJournalMetrics metrics={0};
  CHECK(cpu && gpu);
  CHECK(gml_render_init(cpu,NULL)==0 && gml_render_init(gpu,NULL)==0);
  gml_render_begin(cpu,cpu_pixels,8,8,0.0,0.0);
  CHECK(gml_render_application_surface_ensure_owned(gpu,8,8));
  gml_render_bind_device(gpu,&device);
  CHECK(gml_render_begin_resident_owned_application(gpu,0.0,0.0,0));
  gml_render_set_pending_fill(cpu,0xFF101010u);
  gml_render_set_pending_fill(gpu,0xFF101010u);
  gml_render_primitive_line(cpu,0,0,7,7,0x0000FFu,1);
  gml_render_primitive_line(gpu,0,0,7,7,0x0000FFu,1);
  gml_render_primitive_line_color(cpu,0,7,7,0,0x00FF00u,0xFF0000u,2);
  gml_render_primitive_line_color(gpu,0,7,7,0,0x00FF00u,0xFF0000u,2);
  gml_render_primitive_line_color_subpixel(cpu,0.25,2.75,7.25,4.25,
                                            0xFFFFFFu,0x000000u,1);
  gml_render_primitive_line_color_subpixel(gpu,0.25,2.75,7.25,4.25,
                                            0xFFFFFFu,0x000000u,1);
  gml_render_resource_journal_metrics(gpu->app_surface_resource,&metrics);
  CHECK(gpu->resident_application_pass && fake.submits==4 && fake.readbacks==0 &&
        metrics.journal_operations>12u && metrics.cpu_replays==0u);
  gml_render_flush_pending_fill(cpu);
  CHECK(cpu_pixels[0]!=0xFF101010u && cpu_pixels[7]!=0xFF101010u);
  gml_render_bind_device(gpu,NULL);
  CHECK(gml_render_resident_application_cpu_barrier(gpu) &&
        !gpu->resident_application_pass && fake.readbacks==0 &&
        memcmp(cpu_pixels,gpu->app_surface_owned,sizeof cpu_pixels)==0);
  gml_render_free(cpu);
  gml_render_free(gpu);
  free(cpu);
  free(gpu);
  return 1;
}

static int resident_outline_coverage_public_order(void){
  GmlRender *cpu=(GmlRender*)calloc(1,sizeof *cpu);
  GmlRender *gpu=(GmlRender*)calloc(1,sizeof *gpu);
  uint32_t cpu_pixels[100]={0};
  FakeDevice fake={0};
  GmlRenderDeviceServices device=service(&fake);
  GmlRenderJournalMetrics metrics={0};
  CHECK(cpu && gpu);
  CHECK(gml_render_init(cpu,NULL)==0 && gml_render_init(gpu,NULL)==0);
  gml_render_begin(cpu,cpu_pixels,10,10,0.0,0.0);
  CHECK(gml_render_application_surface_ensure_owned(gpu,10,10));
  gml_render_bind_device(gpu,&device);
  CHECK(gml_render_begin_resident_owned_application(gpu,0.0,0.0,0));
  gml_render_set_pending_fill(cpu,0xFF101010u);
  gml_render_set_pending_fill(gpu,0xFF101010u);
  gml_render_primitive_rectangle(cpu,-1,1,5,6,0x123456u,1);
  gml_render_primitive_rectangle(gpu,-1,1,5,6,0x123456u,1);
  gml_render_primitive_rectangle_color(cpu,3,2,8,8,0x345678u,0x0000ffu,
                                        0x00ff00u,0xff0000u,1);
  gml_render_primitive_rectangle_color(gpu,3,2,8,8,0x345678u,0x0000ffu,
                                        0x00ff00u,0xff0000u,1);
  cpu->circle_precision=gpu->circle_precision=4;
  gml_render_primitive_circle(cpu,4,4,3,2,0xabcdefu,1);
  gml_render_primitive_circle(gpu,4,4,3,2,0xabcdefu,1);
  cpu->circle_precision=gpu->circle_precision=64;
  gml_render_primitive_circle_subpixel(cpu,5.25,5.5,2.25,1.75,0x102030u,1);
  gml_render_primitive_circle_subpixel(gpu,5.25,5.5,2.25,1.75,0x102030u,1);
  cpu->circle_precision=gpu->circle_precision=32;
  gml_render_primitive_circle_color(cpu,5,3,2,2,0x0000ffu,0x00ff00u,1);
  gml_render_primitive_circle_color(gpu,5,3,2,2,0x0000ffu,0x00ff00u,1);
  gml_render_primitive_circle_color_subpixel(cpu,5.5,5.25,1.5,1.25,
                                              0xff0000u,0xff0000u,1);
  gml_render_primitive_circle_color_subpixel(gpu,5.5,5.25,1.5,1.25,
                                              0xff0000u,0xff0000u,1);
  gml_render_primitive_triangle_alpha(cpu,1.2,1.1,8.2,1.9,2.4,8.1,0x2468acu,1.0,1);
  gml_render_primitive_triangle_alpha(gpu,1.2,1.1,8.2,1.9,2.4,8.1,0x2468acu,1.0,1);
  gml_render_primitive_triangle_alpha(cpu,9.5,7.5,4.5,0.5,-1.5,4.5,0x102a6bu,1.0,1);
  gml_render_primitive_triangle_alpha(gpu,9.5,7.5,4.5,0.5,-1.5,4.5,0x102a6bu,1.0,1);
  gml_render_resource_journal_metrics(gpu->app_surface_resource,&metrics);
  CHECK(gpu->resident_application_pass && fake.submits==9 && fake.readbacks==0 &&
        metrics.journal_operations>20u && metrics.cpu_replays==0u);
  gml_render_flush_pending_fill(cpu);
  CHECK(cpu_pixels[1u*10u+5u]!=0xFF101010u &&
        cpu_pixels[4u*10u+4u]!=0xFF101010u);
  gml_render_bind_device(gpu,NULL);
  CHECK(gml_render_resident_application_cpu_barrier(gpu) &&
        !gpu->resident_application_pass && fake.readbacks==0 &&
        memcmp(cpu_pixels,gpu->app_surface_owned,sizeof cpu_pixels)==0);
  gml_render_free(cpu);
  gml_render_free(gpu);
  free(cpu);
  free(gpu);
  return 1;
}

static int resident_atlas_uv_refusal_preserves_queued_prefix(void){
  GmlRender *render=(GmlRender*)calloc(1,sizeof *render);
  GmlWin content={0};
  FakeDevice fake={0};
  GmlRenderDeviceServices device=service(&fake);
  GmlRenderJournalMetrics metrics={0};
  CHECK(render && gml_render_init(render,NULL)==0);
  const int atlas_index=render->n_atlas;
  GmlAtlas *atlases=(GmlAtlas*)realloc(render->atlas,
      (size_t)(atlas_index+1)*sizeof *render->atlas);
  CHECK(atlases);
  render->atlas=atlases;
  memset(&render->atlas[atlas_index],0,sizeof *render->atlas);
  render->n_atlas=atlas_index+1;
  render->atlas[atlas_index].w=render->atlas[atlas_index].h=8;
  render->atlas[atlas_index].px=(uint8_t*)malloc(8u*8u*4u);
  render->tpag=(GmlTpag*)calloc(1,sizeof *render->tpag);
  CHECK(render->atlas[atlas_index].px && render->tpag);
  for(unsigned index=0;index<8u*8u;index++){
    uint8_t *pixel=render->atlas[atlas_index].px+index*4u;
    pixel[0]=32u; pixel[1]=64u; pixel[2]=96u; pixel[3]=128u;
  }
  render->n_tpag=1;
  GmlTpag *page=&render->tpag[0];
  page->atlas=atlas_index;
  page->sx=page->sy=1;
  page->sw=page->sh=page->tw=page->th=page->bw=page->bh=2;
  content.bytecode=15;
  render->win=&content;
  render->interp=1;
  CHECK(gml_render_application_surface_ensure_owned(render,8,8));
  gml_render_bind_device(render,&device);
  CHECK(gml_render_begin_resident_owned_application(render,0.0,0.0,0));
  CHECK(gml_render_resident_filtered_atlas_page(render,0,2.49,2.49,0.6,0.6,
        GML_RENDER_SCENE_SPRITE,0xFFFFFFu,1.0,NULL,0));
  for(unsigned variant=0;variant<4u;variant++){
    page->sx=page->sy=1;
    page->tx=page->ty=0;
    page->bw=page->bh=2;
    double left=2.49,top=2.49,xs=0.6,ys=0.6;
    if(variant==0u){ page->sx=6; page->tx=1; page->bw=3; left=2.47; xs=0.26; }
    if(variant==1u){ page->sx=0; page->bw=3; }
    if(variant==2u){ page->sy=6; page->ty=1; page->bh=3; top=2.47; ys=0.26; }
    if(variant==3u){ page->sy=0; page->bh=3; }
    page->scene_opaque_checked=0;
    /* Fractional snapped coverage can extrapolate a vertex outside the atlas
     * although the stored source rectangle is valid. Refuse before enqueue. */
    CHECK(!gml_render_resident_filtered_atlas_page(render,0,left,top,xs,ys,
          GML_RENDER_SCENE_SPRITE,0xFFFFFFu,1.0,NULL,0));
    CHECK(render->resident_application_pass && fake.submits==0 && fake.readbacks==0);
  }
  fake.readback_colour=0x80406080u;
  CHECK(gml_render_scene_flush(render) && fake.submits==1 &&
        fake.last_batch_operations==1u && fake.readbacks==1);
  gml_render_resource_journal_metrics(render->app_surface_resource,&metrics);
  CHECK(metrics.authority==GML_RENDER_RESOURCE_BOTH_CURRENT &&
        metrics.logical_version==2u && metrics.cpu_version==2u);
  CHECK(gml_render_resident_application_cpu_barrier(render) &&
        !render->resident_application_pass && fake.readbacks==1);
  gml_render_free(render);
  free(render);
  return 1;
}

static int resident_empty_sprite_part_preserves_pending_work(void){
  static const double selections[][2]={{0,3},{3,0},{-1,3},{3,-1},{0,0},{-2,-3},{0,3},{3,0}};
  for(unsigned classic=0;classic<2u;classic++) for(unsigned runtime=0;runtime<2u;runtime++)
  for(unsigned variant=0;variant<8u;variant++){
    GmlRender *r=calloc(1,sizeof *r);
    FakeDevice fake={0};GmlRenderDeviceServices device=service(&fake);
    GmlRenderJournalMetrics before={0},after={0};
    GmlRenderDrawState state_before={0},state_after={0};
    CHECK(r && !gml_render_init(r,NULL) && gml_render_application_surface_ensure_owned(r,4,4));
    r->spr=calloc(1,sizeof *r->spr);r->tpag=calloc(1,sizeof *r->tpag);
    CHECK(r->spr && r->tpag);r->n_spr=r->n_tpag=1;
    r->spr[0].w=r->spr[0].h=1;r->spr[0].n_frames=1;
    r->spr[0].frame=calloc(1,sizeof(int));CHECK(r->spr[0].frame);
    r->tpag[0]=(GmlTpag){.sw=1,.sh=1,.tw=1,.th=1,.bw=1,.bh=1};
    if(runtime){r->spr[0].runtime_rgba=malloc(4u);CHECK(r->spr[0].runtime_rgba);
      memset(r->spr[0].runtime_rgba,255,4u);}
    r->classic=(int)classic;
    gml_render_bind_device(r,&device);
    CHECK(gml_render_begin_resident_owned_application(r,0,0,0));
    r->gui_pass_active=(int)(variant&1u);r->interp=(int)(variant&1u);
    r->alphablend=(int)(variant%3u!=0u);r->blendmode=(int)variant;
    r->color_write_mask=variant;r->alpha_test_enable=1;r->alpha_test_ref=255;
    r->active_shader=variant==6u?0:-1;r->spr[0].ns_enabled=variant==7u;
    gml_render_set_pending_fill(r,0x7B274961u);
    gml_render_resource_journal_metrics(r->app_surface_resource,&before);
    CHECK(gml_render_draw_state_get(r,&state_before));
    int submits=fake.submits,reads=fake.readbacks,uploads=fake.uploads;
    gml_draw_sprite_part_ext(r,0,0,0,0,selections[variant][0],selections[variant][1],
                             1,1,2,-3,0xB94D27u,.37);
    if(variant<6u){
      gml_render_resource_journal_metrics(r->app_surface_resource,&after);
      CHECK(gml_render_draw_state_get(r,&state_after) &&
            !memcmp(&state_before,&state_after,sizeof state_before));
      CHECK(r->resident_application_pass && r->pending_fill &&
            fake.submits==submits && fake.readbacks==reads && fake.uploads==uploads &&
            before.logical_version==after.logical_version && before.cpu_version==after.cpu_version &&
            before.gpu_version==after.gpu_version && before.authority==after.authority &&
            before.journal_operations==after.journal_operations && before.cpu_replays==after.cpu_replays);
    }else CHECK(!r->resident_application_pass);
    r->active_shader=-1;r->spr[0].ns_enabled=0;
    CHECK(gml_render_resident_application_cpu_barrier(r));
    CHECK(r->app_surface_owned[0]==0x7B274961u);
    gml_render_free(r);free(r);
  }
  return 1;
}

static int resident_zero_alpha_sprite_part_noop(void){
  GmlRender *render=(GmlRender*)calloc(1,sizeof *render);
  FakeDevice fake={0};
  GmlRenderDeviceServices device=service(&fake);
  CHECK(render && gml_render_application_surface_ensure_owned(render,4,4));
  render->spr=(GmlSprite*)calloc(1,sizeof *render->spr);
  CHECK(render->spr);
  render->n_spr=1;
  render->spr[0].w=render->spr[0].h=1;
  render->spr[0].n_frames=1;
  render->spr[0].frame=(int*)calloc(1,sizeof(int));
  render->spr[0].runtime_rgba=(uint8_t*)malloc(4u);
  CHECK(render->spr[0].frame && render->spr[0].runtime_rgba);
  render->spr[0].runtime_rgba[0]=0xD0u;
  render->spr[0].runtime_rgba[1]=0x20u;
  render->spr[0].runtime_rgba[2]=0x10u;
  render->spr[0].runtime_rgba[3]=0xFFu;
  render->spr[0].runtime_opaque=1;
  render->color_write_mask=0x0Fu;
  gml_render_bind_device(render,&device);
  for(int classic=0;classic<2;classic++) for(int variant=0;variant<7;variant++){
    GmlRenderJournalMetrics before={0},after={0};
    render->classic=classic;
    CHECK(gml_render_begin_resident_owned_application(render,0.0,0.0,0));
    render->active_shader=variant==2?0:-1;
    render->spr[0].ns_enabled=variant==3;
    render->alphablend=1;
    render->blendmode=variant==4?GML_RENDER_BLEND_DEST_SOURCE_COLOUR:variant==6?6:0;
    render->gui_pass_active=variant==5;
    gml_render_set_pending_fill(render,0xFF102030u);
    gml_render_resource_journal_metrics(render->app_surface_resource,&before);
    int submits=fake.submits,readbacks=fake.readbacks;
    gml_draw_sprite_part_ext(render,0,0,0,0,1,1,1,1,1,1,0xFFFFFFu,
                             variant==1?-1.0:0.0);
    gml_render_resource_journal_metrics(render->app_surface_resource,&after);
    if(variant<2){
      CHECK(render->resident_application_pass && render->pending_fill &&
            fake.submits==submits && fake.readbacks==readbacks &&
            after.logical_version==before.logical_version &&
            after.cpu_replays==before.cpu_replays);
      CHECK(gml_render_resident_application_cpu_barrier(render));
    } else CHECK(!render->resident_application_pass);
    CHECK(render->app_surface_owned[0]==0xFF102030u);
  }
  gml_render_free(render);
  free(render);
  return 1;
}

static int resident_offscreen_sprite_part_preserves_queued_draws(void){
  enum {WIDTH=24,HEIGHT=20};
  for(unsigned shape=0;shape<4u;shape++) for(unsigned edge=0;edge<4u;edge++)
  for(unsigned phase=0;phase<3u;phase++){
    GmlRender *r=calloc(1,sizeof *r);
    GmlWin content={.bytecode=14};
    FakeDevice fake={0};GmlRenderDeviceServices device=service(&fake);
    GmlRenderJournalMetrics before={0},after={0};
    GmlRenderApplicationWriteView view={0};GmlRenderDispatchMetrics dispatch={0};
    uint32_t expected[WIDTH*HEIGHT];
    CHECK(r && gml_render_init(r,NULL)==0 &&
          gml_render_application_surface_ensure_owned(r,WIDTH,HEIGHT));
    r->win=&content;
    free(r->atlas[0].px);
    r->atlas[0]=(GmlAtlas){.w=12,.h=12,.px=malloc(12u*12u*4u)};
    r->tpag=calloc(1,sizeof *r->tpag);r->spr=calloc(1,sizeof *r->spr);
    CHECK(r->atlas[0].px && r->tpag && r->spr);
    r->n_tpag=r->n_spr=1;
    for(unsigned pixel=0;pixel<144u;pixel++){
      uint8_t *rgba=r->atlas[0].px+pixel*4u;
      rgba[0]=230u;rgba[1]=173u;rgba[2]=91u;
      rgba[3]=pixel%3u==0u?0u:pixel%3u==1u?128u:255u;
    }
    const int pw=shape==0u || shape==2u?1:4;
    const int ph=shape==1u || shape==2u?1:5;
    r->tpag[0]=(GmlTpag){.sx=2,.sy=2,.sw=pw,.sh=ph,.tw=pw,.th=ph,
                         .tx=3,.ty=1,.bw=9,.bh=9};
    r->spr[0].w=r->spr[0].h=9;r->spr[0].n_frames=1;
    r->spr[0].originx=4;r->spr[0].originy=3;
    r->spr[0].frame=calloc(1,sizeof(int));CHECK(r->spr[0].frame);
    double left=edge==0u?-100.0:edge==1u?WIDTH+2.0:4.0;
    double top=edge==2u?-100.0:edge==3u?HEIGHT+2.0:4.0;
    left+=(double)phase*0.25;top+=(double)phase*0.25;
    for(unsigned accelerated=0;accelerated<2u;accelerated++){
      gml_render_bind_device(r,accelerated?&device:NULL);
      if(accelerated) CHECK(gml_render_begin_resident_owned_application(r,2,3,0));
      else CHECK(gml_render_begin_owned_application(r,2,3,0));
      r->interp=0;r->active_shader=-1;r->alphablend=1;r->blendmode=0;
      r->blend_equation=r->blend_equation_alpha=1;r->color_write_mask=15u;
      if(accelerated) gml_render_dispatch_diagnostics_enable(r,1);
      gml_render_set_pending_fill(r,0xFF102030u);
      gml_render_primitive_rectangle(r,4,5,8,9,0x2468ACu,0);
      gml_render_resource_journal_metrics(r->app_surface_resource,&before);
      int submits=fake.submits,reads=fake.readbacks;
      gml_draw_sprite_part_ext(r,0,0,1,1,7,7,left,top+3,1,1,0xFFFFFFu,1);
      if(accelerated){
        gml_render_resource_journal_metrics(r->app_surface_resource,&after);
        CHECK(r->resident_application_pass && fake.submits==submits &&
              fake.readbacks==reads && after.logical_version==before.logical_version &&
              after.cpu_replays==before.cpu_replays);
      }
      gml_render_primitive_rectangle(r,6,7,10,11,0x13579Bu,0);
      if(accelerated){
        CHECK(gml_render_scene_submit_pending(r));
        CHECK(gml_render_dispatch_metrics(r,&dispatch) &&
              !dispatch.failed && !dispatch.unclassified && !dispatch.overflow);
        for(unsigned family=0;family<GML_RENDER_SCENE_FAMILY_COUNT;family++)
          for(unsigned reason=0;reason<GML_RENDER_DISPATCH_REASON_COUNT;reason++)
            CHECK(!dispatch.cpu[family][reason]);
      }
      /* This explicit fake-device observation may replay the retained exact
       * recipe. It is separate from ordinary no-op and suffix accounting. */
      CHECK(gml_render_application_surface_owned_view(r,&view));
      if(accelerated) CHECK(!memcmp(expected,view.pixels,sizeof expected) && fake.readbacks==0);
      else memcpy(expected,view.pixels,sizeof expected);
    }
    gml_render_free(r);free(r);
  }
  return 1;
}

static int resident_filtered_gui_offscreen_sprite_noop(void){
  enum {WIDTH=24,HEIGHT=20};
  for(unsigned mapped=0;mapped<2u;mapped++) for(unsigned trim=0;trim<2u;trim++)
  for(unsigned signs=0;signs<4u;signs++) for(unsigned scale=0;scale<2u;scale++)
  for(unsigned edge=0;edge<4u;edge++) for(unsigned phase=0;phase<3u;phase++){
    GmlRender *r=calloc(1,sizeof *r);
    GmlWin content={.bytecode=14};
    FakeDevice fake={0};GmlRenderDeviceServices device=service(&fake);
    GmlRenderJournalMetrics before={0},after={0};
    GmlRenderDispatchMetrics dispatch={0};
    uint32_t expected[WIDTH*HEIGHT],cpu[WIDTH*HEIGHT],screen[WIDTH*HEIGHT];
    const double factor=scale?0.75:1.0;
    const double xs=(signs&1u)?-factor:factor,ys=(signs&2u)?-factor:factor;
    const int pw=(int)floor(6.0*factor+0.5),ph=(int)floor(4.0*factor+0.5);
    const double offset=phase==0u?-0.5:phase==1u?0.0:0.5-0x1p-20;
    double left=edge==0u?-pw:edge==1u?WIDTH:8.0;
    double top=edge==2u?-ph:edge==3u?HEIGHT:8.0;
    if(edge<2u) left+=offset;else top+=offset;
    const double anchor_x=left+((signs&1u)?pw:0)+2.0*xs-trim*xs;
    const double anchor_y=top+((signs&2u)?ph:0)+ys-trim*ys;
    CHECK(r && !gml_render_init(r,NULL));r->win=&content;
    free(r->atlas[0].px);
    r->atlas[0]=(GmlAtlas){.w=10,.h=8,.px=malloc(10u*8u*4u)};
    r->tpag=calloc(1,sizeof *r->tpag);r->spr=calloc(1,sizeof *r->spr);
    CHECK(r->atlas[0].px && r->tpag && r->spr);
    r->n_tpag=r->n_spr=1;
    for(unsigned pixel=0;pixel<80u;pixel++){
      uint8_t *rgba=r->atlas[0].px+pixel*4u;
      rgba[0]=230u;rgba[1]=173u;rgba[2]=91u;rgba[3]=pixel%3u?255u:128u;
    }
    r->tpag[0]=(GmlTpag){.sx=2,.sy=2,.sw=6,.sh=4,.tw=6,.th=4,
                         .tx=(int)trim,.ty=(int)trim,.bw=6+(int)trim*2,.bh=4+(int)trim*2};
    r->spr[0].w=r->tpag[0].bw;r->spr[0].h=r->tpag[0].bh;r->spr[0].n_frames=1;
    r->spr[0].originx=2;r->spr[0].originy=1;
    r->spr[0].frame=calloc(1,sizeof(int));CHECK(r->spr[0].frame);
    for(unsigned accelerated=0;accelerated<2u;accelerated++){
      gml_render_bind_device(r,accelerated?&device:NULL);
      if(accelerated) CHECK(gml_render_begin_resident_base(r,screen,WIDTH,HEIGHT,0,0));
      else gml_render_begin(r,cpu,WIDTH,HEIGHT,0,0);
      r->interp=1;r->active_shader=-1;r->alphablend=1;r->blendmode=0;
      r->blend_equation=r->blend_equation_alpha=1;r->color_write_mask=15u;
      if(accelerated) gml_render_dispatch_diagnostics_enable(r,1);
      gml_render_set_pending_fill(r,0xFF102030u);
      gml_render_primitive_rectangle(r,4,5,8,9,0x2468ACu,0);
      if(!accelerated){gml_render_flush_pending_fill(r);memcpy(expected,cpu,sizeof expected);}
      gml_render_gui_begin(r,WIDTH,HEIGHT);
      if(mapped) gml_render_gui_set_size(r,12,8);
      gml_render_resource_journal_metrics(r->base_scene_resource,&before);
      int submits=fake.submits,reads=fake.readbacks;
      gml_draw_sprite_ext(r,0,0,anchor_x/r->gui_scale_x,anchor_y/r->gui_scale_y,
                         xs/r->gui_scale_x,ys/r->gui_scale_y,0,0xFFFFFFu,0.05);
      if(accelerated){
        gml_render_resource_journal_metrics(r->base_scene_resource,&after);
        CHECK(r->resident_application_pass && fake.submits==submits && fake.readbacks==reads &&
              after.logical_version==before.logical_version && after.cpu_replays==before.cpu_replays);
      } else CHECK(!memcmp(expected,cpu,sizeof expected));
      gml_render_gui_end(r);
      gml_render_primitive_rectangle(r,6,7,10,11,0x13579Bu,0);
      if(accelerated){
        CHECK(gml_render_scene_submit_pending(r) && gml_render_dispatch_metrics(r,&dispatch) &&
              !dispatch.failed && !dispatch.unclassified && !dispatch.overflow);
        for(unsigned family=0;family<GML_RENDER_SCENE_FAMILY_COUNT;family++)
          for(unsigned reason=0;reason<GML_RENDER_DISPATCH_REASON_COUNT;reason++)
            CHECK(!dispatch.cpu[family][reason]);
        /* Explicit fake-device observation is separate from ordinary draw accounting. */
        const uint32_t *sealed=NULL;
        CHECK(gml_render_resource_acquire_cpu_read(r->base_scene_resource,&sealed) &&
              !memcmp(expected,sealed,sizeof expected));
      } else {gml_render_flush_pending_fill(r);memcpy(expected,cpu,sizeof expected);}
    }
    gml_render_free(r);free(r);
  }
  return 1;
}

static int resident_filtered_application_offscreen_sprite_noop(void){
  static const double rotations[]={0.0,360.0,-720.0,0.0005};
  for(unsigned angle=0;angle<4u;angle++) for(unsigned source=0;source<3u;source++){
    GmlRender *r=calloc(1,sizeof *r);
    GmlWin content={.bytecode=14};
    FakeDevice fake={0};GmlRenderDeviceServices device=service(&fake);
    GmlRenderJournalMetrics before={0},after={0};
    uint32_t expected[32u*24u];
    CHECK(r && !gml_render_init(r,NULL));r->win=&content;
    free(r->atlas[0].px);
    r->atlas[0]=(GmlAtlas){.w=20,.h=20,.px=malloc(20u*20u*4u)};
    r->tpag=calloc(1,sizeof *r->tpag);r->spr=calloc(1,sizeof *r->spr);
    CHECK(r->atlas[0].px && r->tpag && r->spr);
    r->n_tpag=r->n_spr=1;
    for(unsigned pixel=0;pixel<400u;pixel++){
      uint8_t *rgba=r->atlas[0].px+4u*pixel;
      rgba[0]=(uint8_t)(pixel*17u);rgba[1]=(uint8_t)(pixel*29u);rgba[2]=93u;
      rgba[3]=(uint8_t)(source==0u?255u:source==1u?(pixel&1u?255u:0u):127u);
    }
    r->tpag[0]=(GmlTpag){.sx=2,.sy=2,.sw=16,.sh=16,.tw=16,.th=16,.bw=16,.bh=16};
    r->spr[0].w=r->spr[0].h=16;r->spr[0].originx=r->spr[0].originy=8;
    r->spr[0].n_frames=1;r->spr[0].frame=calloc(1,sizeof(int));CHECK(r->spr[0].frame);
    CHECK(gml_render_application_surface_ensure_owned(r,32,24));
    for(unsigned side=0;side<2u;side++){
      gml_render_bind_device(r,side?&device:NULL);
      if(side) CHECK(gml_render_begin_resident_owned_application(r,0,0,0));
      else CHECK(gml_render_begin_owned_application(r,0,0,0));
      r->interp=1;r->active_shader=-1;r->alphablend=1;r->blendmode=0;
      r->blend_equation=r->blend_equation_alpha=1;r->color_write_mask=15u;
      gml_render_set_pending_fill(r,0xFF102030u);
      if(!side){gml_render_flush_pending_fill(r);memcpy(expected,r->fb,sizeof expected);}
      gml_render_resource_journal_metrics(r->app_surface_resource,&before);
      int submits=fake.submits,reads=fake.readbacks;
      /* The CPU axis rectangle ends at -1: the conservative rotated envelope
       * alone cannot establish this nearer no-op. Preserve the queued clear. */
      gml_draw_sprite_ext(r,0,0,-2.65,10.0,0.2,0.2,rotations[angle],0xFFFFFFu,0.245);
      if(side){
        gml_render_resource_journal_metrics(r->app_surface_resource,&after);
        CHECK(r->resident_application_pass && fake.submits==submits && fake.readbacks==reads &&
              after.logical_version==before.logical_version && after.cpu_replays==before.cpu_replays);
      } else CHECK(!memcmp(expected,r->fb,sizeof expected));
    }
    gml_render_free(r);free(r);
  }
  return 1;
}

static int resident_empty_sprite_preserves_queued_draws(void){
  const uint8_t rgba[16]={
    0xD0u,0x20u,0x10u,0xFFu, 0x10u,0xD0u,0x20u,0xFFu,
    0x20u,0x10u,0xD0u,0xFFu, 0xC0u,0xB0u,0xA0u,0xFFu
  };
  for(int classic=0;classic<2;classic++) for(int diagnostics=0;diagnostics<2;diagnostics++){
    GmlRender *render=(GmlRender*)calloc(1,sizeof *render);
    FakeDevice fake={0};
    GmlRenderDeviceServices device=service(&fake);
    GmlRenderApplicationWriteView view={0};
    GmlRenderJournalMetrics before={0},after={0};
    GmlRenderDispatchMetrics dispatch={0};
    uint32_t completed[16];
    CHECK(render && gml_render_application_surface_ensure_owned(render,4,4));
    render->atlas=(GmlAtlas*)calloc(1,sizeof *render->atlas);
    render->tpag=(GmlTpag*)calloc(1,sizeof *render->tpag);
    render->spr=(GmlSprite*)calloc(2,sizeof *render->spr);
    CHECK(render->atlas && render->tpag && render->spr);
    render->n_atlas=render->n_tpag=1;render->n_spr=2;
    render->atlas[0].px=(uint8_t*)malloc(sizeof rgba);
    render->spr[1].frame=(int*)calloc(1,sizeof(int));
    CHECK(render->atlas[0].px && render->spr[1].frame);
    memcpy(render->atlas[0].px,rgba,sizeof rgba);
    render->atlas[0].w=render->atlas[0].h=2;
    render->tpag[0].sw=render->tpag[0].sh=2;
    render->tpag[0].tw=render->tpag[0].th=2;
    render->tpag[0].bw=render->tpag[0].bh=2;
    render->spr[1].w=render->spr[1].h=2;render->spr[1].n_frames=1;
    render->classic=classic;render->active_shader=-1;render->color_write_mask=15u;
    gml_render_dispatch_diagnostics_enable(render,diagnostics);
    gml_render_bind_device(render,&device);
    CHECK(gml_render_begin_resident_owned_application(render,0.0,0.0,0));
    gml_render_set_pending_fill(render,0xFF102030u);
    gml_render_resource_journal_metrics(render->app_surface_resource,&before);
    /* Empty and invalid resources have no CPU raster to order after the prefix. */
    const int empty_ids[3]={0,-1,2};
    for(unsigned index=0;index<3u;index++){
      gml_draw_sprite_ext(render,empty_ids[index],0,128.0,237.0,2.0,2.0,0.0,
                          0xFFFFFFu,1.0);
      gml_render_resource_journal_metrics(render->app_surface_resource,&after);
      CHECK(render->resident_application_pass && render->pending_fill &&
            !fake.submits && !fake.readbacks &&
            after.logical_version==before.logical_version &&
            after.cpu_replays==before.cpu_replays);
    }
    gml_draw_sprite_ext(render,1,0,1.0,1.0,1.0,1.0,0.0,0xFFFFFFu,1.0);
    CHECK(render->resident_application_pass);
    int submits=fake.submits;
    gml_render_resource_journal_metrics(render->app_surface_resource,&before);
    gml_draw_sprite_ext(render,0,0,128.0,237.0,2.0,2.0,0.0,0xFFFFFFu,1.0);
    gml_render_resource_journal_metrics(render->app_surface_resource,&after);
    CHECK(render->resident_application_pass && fake.submits==submits &&
          !fake.readbacks && after.logical_version==before.logical_version);
    CHECK(render->resident_application_pass && gml_render_scene_submit_pending(render));
    CHECK(gml_render_dispatch_metrics(render,&dispatch) &&
          dispatch.gpu[GML_RENDER_SCENE_SPRITE]==(uint64_t)diagnostics &&
          !dispatch.failed && !dispatch.unclassified && !dispatch.overflow);
    for(unsigned family=0;family<GML_RENDER_SCENE_FAMILY_COUNT;family++)
      for(unsigned reason=0;reason<GML_RENDER_DISPATCH_REASON_COUNT;reason++)
        CHECK(!dispatch.cpu[family][reason]);
    /* Explicit observation reconstructs the fake device's exact retained result. */
    CHECK(gml_render_application_surface_owned_view(render,&view) &&
          view.pixels[0]==0xFF102030u && view.pixels[5]==0xFFD02010u &&
          view.pixels[6]==0xFF10D020u && view.pixels[9]==0xFF2010D0u &&
          view.pixels[10]==0xFFC0B0A0u && !fake.readbacks);
    memcpy(completed,view.pixels,sizeof completed);
    CHECK(gml_render_begin_owned_application(render,0.0,0.0,0));
    gml_render_set_pending_fill(render,0xFF102030u);
    for(unsigned index=0;index<3u;index++)
      gml_draw_sprite_ext(render,empty_ids[index],0,128.0,237.0,2.0,2.0,0.0,
                          0xFFFFFFu,1.0);
    gml_draw_sprite_ext(render,1,0,1.0,1.0,1.0,1.0,0.0,0xFFFFFFu,1.0);
    gml_render_flush_pending_fill(render);
    CHECK(!memcmp(completed,render->app_surface_owned,sizeof completed));
    gml_render_free(render);free(render);
  }
  return 1;
}

/* Real-device paired controls own pixels; this fake device guards transport
 * and authority for newly admitted nearest public-function variants. */
static int nearest_sprite_extension_transport(GmlRender *render,FakeDevice *fake,
                                             int submits){
  GmlRenderJournalMetrics metrics={0};
  CHECK(render->resident_application_pass && render->pending_scene_batch &&
        fake->submits==submits && !fake->readbacks && !fake->rgb_queries);
  CHECK(gml_render_scene_submit_pending(render) && fake->submits==submits+1 &&
        fake->last_batch_operations>=1u && fake->last_filter==GML_RENDER_FILTER_NEAREST &&
        fake->last_quad_state.write_mask==render->color_write_mask &&
        !fake->readbacks && !fake->rgb_queries);
  GmlRenderResource *resource=gml_render_scene_target_resource_ensure(render);
  CHECK(resource);
  gml_render_resource_journal_metrics(resource,&metrics);
  CHECK(metrics.authority==GML_RENDER_RESOURCE_GPU_RESIDENT &&
        metrics.cpu_version<metrics.logical_version &&
        metrics.gpu_version==metrics.logical_version && !metrics.cpu_replays &&
        !metrics.device_checkpoints && !metrics.journal_operations &&
        !metrics.journal_bytes && !metrics.retained_mutable_bytes);
  return 1;
}

static int resident_black_additive_sprite_noop(void){
  enum {WIDTH=16,HEIGHT=12};
  for(unsigned variant=0;variant<12u;variant++){
    GmlRender *r=calloc(1,sizeof *r);
    GmlWin content={.bytecode=17};
    FakeDevice fake={.on_demand=1};
    GmlRenderDeviceServices device=service(&fake);
    GmlRenderJournalMetrics before={0},after={0};
    uint32_t cpu[WIDTH*HEIGHT],gpu[WIDTH*HEIGHT],shadow[WIDTH*HEIGHT];
    uint32_t fill=variant==6u?0x40102030u:0xFF102030u;
    CHECK(r && gml_render_init(r,NULL)==0);
    r->win=&content;
    free(r->atlas[0].px);memset(&r->atlas[0],0,sizeof r->atlas[0]);
    r->atlas[0].w=r->atlas[0].h=4;
    r->atlas[0].px=malloc(4u*4u*4u);
    r->tpag=calloc(1,sizeof *r->tpag);
    r->spr=calloc(1,sizeof *r->spr);
    CHECK(r->atlas[0].px && r->tpag && r->spr);
    r->n_tpag=r->n_spr=1;
    for(unsigned pixel=0;pixel<16u;pixel++){
      uint8_t *rgba=r->atlas[0].px+pixel*4u;
      rgba[0]=230;rgba[1]=173;rgba[2]=91;
      rgba[3]=pixel%3u==0u?0u:pixel%3u==1u?128u:255u;
    }
    r->tpag[0]=(GmlTpag){.sx=1,.sy=1,.sw=2,.sh=2,.tw=2,.th=2,.bw=3,.bh=3,.tx=1,.ty=1};
    r->spr[0].w=r->spr[0].h=3;r->spr[0].n_frames=1;
    r->spr[0].originx=r->spr[0].originy=1;
    r->spr[0].frame=calloc(1,sizeof(int));CHECK(r->spr[0].frame);
    device.observe_on_demand=fake_observe_on_demand;
    for(unsigned accelerated=0;accelerated<2u;accelerated++){
      gml_render_bind_device(r,accelerated?&device:NULL);
      if(accelerated) CHECK(gml_render_begin_resident_base(r,gpu,WIDTH,HEIGHT,0.25,0.25));
      else gml_render_begin(r,cpu,WIDTH,HEIGHT,0.25,0.25);
      r->interp=variant%2u;
      r->alphablend=1;r->blendmode=1;
      r->active_shader=variant==10u?0:-1;
      r->alpha_test_enable=variant==9u;
      r->blend_equation=variant==11u?2:1;
      gml_render_set_pending_fill(r,fill);
      gml_render_flush_pending_fill(r);
      r->fb_opaque_known=variant!=7u;
      r->fb_all_opaque=variant!=6u;
      if(accelerated) gml_render_resource_journal_metrics(r->base_scene_resource,&before);
      int submits=fake.submits,reads=fake.readbacks;
      if(accelerated) memcpy(shadow,gpu,sizeof shadow);
      fake.readback_colour=fill;
      gml_draw_sprite_ext(r,0,0,2.25,2.75,2.3,1.7,0.0,
                          variant==8u?0xFFFFFFu:0u,
                          variant<6u?(variant/2u+1u)/3.0:1.0);
      if(!accelerated){
        if(variant<6u) for(unsigned pixel=0;pixel<WIDTH*HEIGHT;pixel++) CHECK(cpu[pixel]==fill);
        if(variant==6u){
          unsigned changed=0;
          for(unsigned pixel=0;pixel<WIDTH*HEIGHT;pixel++) changed+=cpu[pixel]!=fill;
          CHECK(changed>0u);
        }
        continue;
      }
      if(variant<6u){
        gml_render_resource_journal_metrics(r->base_scene_resource,&after);
        CHECK(r->resident_application_pass && fake.submits==submits &&
              fake.readbacks==reads && after.logical_version==before.logical_version &&
              after.cpu_replays==before.cpu_replays &&
              after.device_checkpoints==before.device_checkpoints);
        /* Explicit observation after the no-op is separate from ordinary drawing. */
        CHECK(gml_render_resident_application_cpu_barrier(r));
        CHECK(!memcmp(cpu,gpu,sizeof cpu));
      } else if(variant==6u || variant==8u){
        CHECK(nearest_sprite_extension_transport(r,&fake,submits));
        CHECK(fake.last_quad_state.rgb_source_factor==GML_RENDER_BLEND_SOURCE_ALPHA &&
              fake.last_quad_state.rgb_destination_factor==GML_RENDER_BLEND_ONE &&
              fake.last_quad_state.alpha_source_factor==GML_RENDER_BLEND_ONE &&
              fake.last_quad_state.alpha_destination_factor==GML_RENDER_BLEND_ONE &&
              fake.last_quad_state.coverage_policy==GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA);
        CHECK(!memcmp(shadow,gpu,sizeof shadow));
      } else if(variant==7u){
        /* Unknown opacity is not a no-op certificate. The separately paired
         * physical writer executes and stamps coverage instead. */
        CHECK(r->resident_application_pass && gml_render_scene_submit_pending(r) &&
          fake.submits==submits+1 && fake.readbacks==reads &&
          fake.last_filter==GML_RENDER_FILTER_HARDWARE_BILINEAR &&
          fake.last_quantisation==GML_RENDER_QUANTISE_HARDWARE_SPRITE_FIRSTGEN &&
          fake.last_quad_state.rgb_destination_factor==GML_RENDER_BLEND_ONE &&
          fake.last_quad_state.coverage_policy==GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA &&
          !memcmp(shadow,gpu,sizeof shadow));
      } else CHECK(!r->resident_application_pass);
    }
    gml_render_free(r);free(r);
  }
  return 1;
}

static int resident_application_sprite_cpu_fallback(void){
  GmlRender *render=(GmlRender*)calloc(1,sizeof *render);
  GmlRenderDeviceServices device;
  GmlRenderJournalMetrics metrics={0};
  FakeDevice fake={0};
  CHECK(render);
  CHECK(gml_render_application_surface_ensure_owned(render,4,4));
  render->spr=(GmlSprite*)calloc(1,sizeof *render->spr);
  CHECK(render->spr);
  render->n_spr=1;
  render->spr[0].w=render->spr[0].h=1;
  render->spr[0].n_frames=1;
  render->spr[0].frame=(int*)calloc(1,sizeof(int));
  render->spr[0].runtime_rgba=(uint8_t*)malloc(4u);
  CHECK(render->spr[0].frame && render->spr[0].runtime_rgba);
  render->spr[0].runtime_rgba[0]=0xD0u;
  render->spr[0].runtime_rgba[1]=0x20u;
  render->spr[0].runtime_rgba[2]=0x10u;
  render->spr[0].runtime_rgba[3]=0xFFu;
  render->spr[0].runtime_opaque=1;
  render->color_write_mask=0x0Fu;
  device=service(&fake);
  gml_render_bind_device(render,&device);
  CHECK(gml_render_begin_resident_owned_application(render,0.0,0.0,0));
  gml_render_set_pending_fill(render,0xFF000000u);
  gml_draw_sprite_ext(render,0,0,1.0,1.0,1.0,1.0,0.0,0xFFFFFFu,1.0);
  gml_render_resource_journal_metrics(render->app_surface_resource,&metrics);
  CHECK(!render->resident_application_pass && !render->pending_fill &&
        fake.submits==1 && fake.readbacks==0 && metrics.cpu_replays==1u &&
        render->app_surface_owned[0]==0xFF000000u &&
        render->app_surface_owned[5]==0xFFD02010u);
  render->color_write_mask=0x07u;
  CHECK(gml_render_begin_resident_owned_application(render,0.0,0.0,0));
  gml_render_set_pending_fill(render,0x40112233u);
  gml_draw_sprite_ext(render,0,0,1.0,1.0,1.0,1.0,0.0,0xFFFFFFu,1.0);
  gml_render_resource_journal_metrics(render->app_surface_resource,&metrics);
  CHECK(!render->resident_application_pass && fake.submits==2 &&
        fake.readbacks==0 && metrics.cpu_replays==2u &&
        render->app_surface_owned[0]==0x40112233u &&
        render->app_surface_owned[5]==0x40D02010u &&
        render->color_write_mask==0x07u);
  gml_render_free(render);
  free(render);
  return 1;
}

static int resident_clipped_filtered_background_part_noop(void){
  enum { WIDTH=12,HEIGHT=10,ATLAS=8 };
  GmlRender *render=(GmlRender*)calloc(1,sizeof *render);
  GmlWin content={0};
  FakeDevice fake={0};
  GmlRenderDeviceServices device=service(&fake);
  uint32_t cpu[WIDTH*HEIGHT],expected[WIDTH*HEIGHT];
  CHECK(render && gml_render_init(render,NULL)==0 &&
        gml_render_application_surface_ensure_owned(render,WIDTH,HEIGHT));
  int atlas_index=render->n_atlas;
  GmlAtlas *atlases=(GmlAtlas*)realloc(render->atlas,
      (size_t)(atlas_index+1)*sizeof *render->atlas);
  CHECK(atlases);
  render->atlas=atlases;
  memset(&render->atlas[atlas_index],0,sizeof *render->atlas);
  render->n_atlas=atlas_index+1;
  render->atlas[atlas_index].w=render->atlas[atlas_index].h=ATLAS;
  render->atlas[atlas_index].px=(uint8_t*)malloc(ATLAS*ATLAS*4u);
  render->tpag=(GmlTpag*)calloc(2,sizeof *render->tpag);
  render->bg=(GmlBg*)calloc(2,sizeof *render->bg);
  CHECK(render->atlas[atlas_index].px && render->tpag && render->bg);
  render->n_tpag=render->n_bg=2;
  for(unsigned index=0;index<ATLAS*ATLAS;index++){
    uint8_t *pixel=render->atlas[atlas_index].px+index*4u;
    pixel[0]=0xD0u;pixel[1]=0x30u;pixel[2]=0x60u;pixel[3]=255u;
  }
  render->tpag[0].atlas=atlas_index;
  render->tpag[0].sx=render->tpag[0].sy=1;
  render->tpag[0].sw=render->tpag[0].sh=2;
  render->tpag[0].tw=render->tpag[0].th=2;
  render->tpag[0].bw=render->tpag[0].bh=2;
  render->tpag[1]=render->tpag[0];
  render->tpag[1].sx=render->tpag[1].sy=3;
  render->tpag[1].sw=render->tpag[1].sh=4;
  render->tpag[1].tw=render->tpag[1].th=4;
  render->tpag[1].bw=render->tpag[1].bh=5;
  render->tpag[1].tx=render->tpag[1].ty=1;
  render->atlas[atlas_index].px[(3u*ATLAS+3u)*4u+3u]=127u;
  render->bg[1].tpag=1;
  content.bytecode=15;
  render->win=&content;
  CHECK(gml_render_application_surface_select_owned(render,0));
  const double scales[][2]={{1.0,1.0},{0.5,0.75},{0.75,0.625},
                            {1.5,1.25},{0.875,0.75}};
  for(unsigned variant=0;variant<100u;variant++){
    unsigned scale=variant/20u,placement=variant%20u;
    int visible=placement>=16u,mapped=scale==4u;
    double world_x=mapped?scales[scale][0]:1.0;
    double world_y=mapped?scales[scale][1]:1.0;
    double xs=(mapped?1.0:scales[scale][0])*
      (!visible && (placement&4u)?-1.0:1.0);
    double ys=(mapped?1.0:scales[scale][1])*
      (!visible && (placement&8u)?-1.0:1.0);
    double extent_x=2.0*fabs(xs*world_x),extent_y=2.0*fabs(ys*world_y);
    double x=3.0,y=3.0,cam_x=0.125,cam_y=0.25;
    switch(placement&3u){
      case 0: x=visible?WIDTH-1:xs>0?WIDTH:WIDTH+extent_x;break;
      case 1: x=visible?(scale?0.0:-1.0):xs>0?-extent_x:0.0;break;
      case 2: y=visible?HEIGHT-1:ys>0?HEIGHT:HEIGHT+extent_y;break;
      case 3: y=visible?(scale?0.0:-1.0):ys>0?-extent_y:0.0;break;
    }
    x=(x+cam_x+0.3)/world_x;y=(y+cam_y+0.3)/world_y;
    gml_render_bind_device(render,NULL);
    gml_render_begin(render,render->app_surface_owned,WIDTH,HEIGHT,cam_x,cam_y);
    render->world_transform_active=0;
    gml_render_set_pending_fill(render,0x40102030u);
    render->interp=0;
    gml_draw_background(render,0,cam_x+1.0,cam_y+1.0);
    memcpy(cpu,render->app_surface_owned,sizeof cpu);
    render->interp=1;
    render->world_transform_active=mapped;
    render->world_scale_x=world_x;render->world_scale_y=world_y;
    gml_draw_background_part_ext(render,1,1,1,2,2,x,y,xs,ys,0xa0c0e0u,0.6);
    CHECK((memcmp(cpu,render->app_surface_owned,sizeof cpu)!=0)==visible);
    memcpy(expected,render->app_surface_owned,sizeof expected);
    gml_render_bind_device(render,&device);
    CHECK(gml_render_begin_resident_owned_application(render,cam_x,cam_y,0));
    render->world_transform_active=0;
    gml_render_set_pending_fill(render,0x40102030u);
    render->interp=0;
    gml_draw_background(render,0,cam_x+1.0,cam_y+1.0);
    render->interp=1;
    render->world_transform_active=mapped;
    render->world_scale_x=world_x;render->world_scale_y=world_y;
    GmlRenderJournalMetrics before={0},after={0};
    gml_render_resource_journal_metrics(render->app_surface_resource,&before);
    int submits=fake.submits,uploads=fake.uploads,reads=fake.readbacks;
    gml_draw_background_part_ext(render,1,1,1,2,2,x,y,xs,ys,0xa0c0e0u,0.6);
    gml_render_resource_journal_metrics(render->app_surface_resource,&after);
    if(!visible){
      CHECK(render->resident_application_pass && fake.submits==submits &&
            fake.uploads==uploads && fake.readbacks==reads &&
            after.logical_version==before.logical_version &&
            after.authority==before.authority &&
            after.journal_operations==before.journal_operations);
    } else CHECK(!render->resident_application_pass && fake.readbacks==reads);
    gml_render_bind_device(render,NULL);
    GmlRenderApplicationWriteView observed={0};
    CHECK(gml_render_application_surface_owned_view(render,&observed) &&
          !memcmp(observed.pixels,expected,sizeof expected) && fake.readbacks==reads);
  }
  gml_render_free(render);free(render);
  return 1;
}

static void integer_clip_subject(GmlRender *render,int text,int world,int scale){
  if(text) gml_draw_text_transformed(render,world?-0.25:-1.0,world?-0.25:-1.0,"A",
    world?scale/4.0:scale,world?scale/4.0:scale,0.0,0u,1.0);
  else gml_draw_background_tile(render,0,0,0,2,2,world?-0.25:-1.0,
    world?-0.25:-1.0,world?scale/4.0:scale,world?scale/4.0:scale,
    0,0,0,0xFFFFFFu,1);
}

static int resident_integer_tile_clipping(int text){
  for(int scale=2;scale<=(text?4:8);scale++) for(int world=0;world<2;world++)
  for(int observed=0;observed<2;observed++){
    GmlRender *render=calloc(1,sizeof *render);
    FakeDevice fake={.on_demand=observed};
    GmlRenderDeviceServices device=service(&fake);
    uint32_t expected[16],shadow[16];
    const uint8_t rgba[16]={208,32,16,255,16,208,32,255,
                           32,16,208,255,192,176,160,255};
    CHECK(render && gml_render_application_surface_ensure_owned(render,4,4));
    render->atlas=calloc(1,sizeof *render->atlas);
    render->tpag=calloc(1,sizeof *render->tpag);
    render->bg=calloc(1,sizeof *render->bg);
    CHECK(render->atlas && render->tpag && render->bg);
    render->n_atlas=render->n_tpag=render->n_bg=1;
    render->atlas[0].px=malloc(sizeof rgba);
    CHECK(render->atlas[0].px);
    memcpy(render->atlas[0].px,rgba,sizeof rgba);
    render->atlas[0].w=render->atlas[0].h=2;
    render->tpag[0]=(GmlTpag){.sw=2,.sh=2,.tw=2,.th=2,.bw=2,.bh=2};
    if(text){
      GmlFont *font=&render->fonts[0];render->font=0;render->n_fonts=1;
      font->real=1;font->sprite=-1;font->atlas=0;
      font->line_height=font->align_height=2;
      font->glyphs=calloc(1,sizeof *font->glyphs);CHECK(font->glyphs);
      font->n_glyphs=1;font->glyphs_sorted=1;
      for(int index=0;index<256;index++) font->glyph_by_char[index]=-1;
      font->glyph_by_char['A']=0;
      font->glyphs[0]=(GmlGlyph){.w=2,.h=2,.shift=2,.ch='A'};
    }
    render->color_write_mask=15u;render->alpha=1;
    CHECK(gml_render_begin_owned_application(render,0,0,0));
    gml_render_set_pending_fill(render,0xFF123456u);
    gml_draw_background(render,0,2,2);
    render->world_transform_active=world;
    render->world_scale_x=render->world_scale_y=4;
    integer_clip_subject(render,text,world,scale);
    render->world_transform_active=0;
    memcpy(expected,render->app_surface_owned,sizeof expected);
    for(int pixel=0;pixel<16;pixel++) render->app_surface_owned[pixel]=0xA5ABCDEFu;
    memcpy(shadow,render->app_surface_owned,sizeof shadow);
    device.observe_on_demand=fake_observe_on_demand;
    gml_render_bind_device(render,&device);
    CHECK(gml_render_begin_resident_owned_application(render,0,0,0));
    gml_render_set_pending_fill(render,0xFF123456u);
    gml_draw_background(render,0,2,2);
    int submits=fake.submits;
    render->world_transform_active=world;
    render->world_scale_x=render->world_scale_y=4;
    integer_clip_subject(render,text,world,scale);
    render->world_transform_active=0;
    if(fake.readbacks) fprintf(stderr,"integer tile authority: scale=%d world=%d observed=%d resident=%d reads=%d interp=%d classic=%d\n",
      scale,world,observed,render->resident_application_pass,fake.readbacks,render->interp,render->classic);
    CHECK(fake.readbacks==0);
    if(observed){
      GmlRenderJournalMetrics metrics={0};
      CHECK(render->resident_application_pass && fake.submits==submits);
      CHECK(gml_render_scene_submit_pending(render));
      gml_render_resource_journal_metrics(gml_render_application_resource_ensure(render),&metrics);
      CHECK(fake.last_batch_operations==2u && fake.readbacks==0 &&
            fake.last_quad.x[0]==-1 && fake.last_quad.y[0]==-1 &&
            fake.last_quad.x[1]==2*scale-1 && fake.last_quad.y[2]==2*scale-1 &&
            metrics.authority==GML_RENDER_RESOURCE_GPU_RESIDENT && !metrics.cpu_replays &&
            !metrics.journal_bytes && !metrics.retained_mutable_bytes &&
            !memcmp(shadow,render->app_surface_owned,sizeof shadow));
    }else CHECK(!render->resident_application_pass &&
               !memcmp(expected,render->app_surface_owned,sizeof expected));
    gml_render_free(render);free(render);
  }
  return 1;
}

static int resident_integer_endpoint_sprite_part(void){
  for(int excluded=0;excluded<18;excluded++) for(int world=0;world<2;world++){
    GmlRender *r=calloc(1,sizeof *r);
    GmlWin content={.bytecode=17};
    FakeDevice fake={.on_demand=1};
    GmlRenderDeviceServices device=service(&fake);
    const uint8_t rgba[16]={208,32,16,255,16,208,32,255,
                           32,16,208,0,192,176,160,255};
    uint32_t base[16],shadow[16];
    uint32_t *borrowed_phase=NULL;
    CHECK(r && !gml_render_init(r,NULL));
    r->win=&content;
    free(r->atlas[0].px);
    r->atlas[0].px=malloc(sizeof rgba);CHECK(r->atlas[0].px);
    memcpy(r->atlas[0].px,rgba,sizeof rgba);r->atlas[0].w=r->atlas[0].h=2;
    r->tpag=calloc(1,sizeof *r->tpag);r->spr=calloc(1,sizeof *r->spr);
    CHECK(r->tpag && r->spr);r->n_tpag=r->n_spr=1;
    r->tpag[0]=(GmlTpag){.sw=2,.sh=2,.tw=2,.th=2,.bw=4,.bh=4,.tx=1,.ty=1};
    r->spr[0]=(GmlSprite){.w=4,.h=4,.originx=3,.originy=2,.n_frames=1};
    r->spr[0].frame=calloc(1,sizeof(int));CHECK(r->spr[0].frame);
    if(excluded==2) r->atlas[0].px[3]=128;
    if(excluded==17) r->atlas[0].runtime_font_page=1;
    gml_render_application_surface_bind(r,base,4,4,1);
    device.observe_on_demand=fake_observe_on_demand;gml_render_bind_device(r,&device);
    CHECK(gml_render_begin_resident_base(r,base,4,4,0,0));
    int target=gml_surface_create(r,4,4);CHECK(target>0 && gml_surface_set_target(r,target));
    for(int pixel=0;pixel<16;pixel++) r->fb[pixel]=0xA5ABCDEFu;
    memcpy(shadow,r->fb,sizeof shadow);
    gml_render_set_pending_fill(r,0xFF123456u);
    gml_render_flush_pending_fill(r);
    int submits=fake.submits;
    double alpha=1.0,xs=2.0,ys=2.0,x=-1.0;uint32_t tint=0xFF00FFu;
    if(excluded==1) fake.on_demand=0;
    if(excluded==3) alpha=0.5;
    if(excluded==4) tint=0x112233u;
    if(excluded==5) r->active_shader=0;
    if(excluded==6) r->color_write_mask=7;
    if(excluded==7) r->fog_flat=1;
    if(excluded==8) r->alpha_test_enable=1;
    if(excluded==9) r->classic=1;
    if(excluded==10) content.bytecode=15;
    if(excluded==11) r->gui_pass_active=1;
    if(excluded==12){
      borrowed_phase=calloc(16,sizeof(uint32_t));CHECK(borrowed_phase);
      r->app_interp_phase[0]=borrowed_phase;
    }
    if(excluded==13) xs=2.5;
    if(excluded==14) xs=-2.0;
    if(excluded==15) x=-1.25;
    if(excluded==16) xs=5.0;
    r->world_transform_active=world;r->world_scale_x=4;r->world_scale_y=2;
    gml_draw_sprite_part_ext(r,0,0,1,1,2,2,x,-1,xs,ys,tint,alpha);
    if(!excluded){
      GmlRenderJournalMetrics metrics={0};
      CHECK(r->resident_application_pass && fake.submits==submits && !fake.readbacks);
      CHECK(gml_render_scene_submit_pending(r));
      gml_render_resource_journal_metrics(gml_render_scene_target_resource_ensure(r),&metrics);
      CHECK(fake.last_batch_operations==1u && fake.last_filter==GML_RENDER_FILTER_NEAREST &&
            fake.last_quad.x[0]==-1 && fake.last_quad.y[0]==-1 &&
            fake.last_quad.x[1]==3 && fake.last_quad.y[2]==3 &&
            fake.last_quad.corner_colour[0]==0xFFFF00FFu && !fake.readbacks &&
            metrics.authority==GML_RENDER_RESOURCE_GPU_RESIDENT && !metrics.cpu_replays &&
            !metrics.journal_bytes && !metrics.retained_mutable_bytes &&
            !memcmp(shadow,r->fb,sizeof shadow));
    }else if(excluded==2 || excluded==3 || excluded==4 || excluded==6 ||
             excluded==8 || excluded==9 || excluded==10 || excluded==11 ||
             (excluded>=13 && excluded<=16)){
      /* The function-wide part path supersedes these old admission limits;
       * it still must preserve the ordered image without CPU transport. */
      GmlRenderJournalMetrics metrics={0};
      CHECK(r->resident_application_pass && !fake.readbacks);
      CHECK(gml_render_scene_submit_pending(r));
      gml_render_resource_journal_metrics(gml_render_scene_target_resource_ensure(r),&metrics);
      if(fake.last_source_count!=1u) fprintf(stderr,"part new no-source: case=%d world=%d opcode=%u\n",excluded,world,fake.last_opcode);
      CHECK((fake.last_source_count==1u || (excluded==14 && fake.last_opcode==GML_RENDER_OP_CLEAR)) &&
        metrics.authority==GML_RENDER_RESOURCE_GPU_RESIDENT &&
        !metrics.cpu_replays && !metrics.journal_bytes && !memcmp(shadow,r->fb,sizeof shadow));
    }else{
      if(r->resident_application_pass) fprintf(stderr,"integer selected-part exclusion: case=%d world=%d\n",excluded,world);
      CHECK(!r->resident_application_pass);
    }
    /* Sample planes are borrowed; their injecting fixture owns the storage. */
    r->world_transform_active=0;gml_render_free(r);free(borrowed_phase);free(r);
  }
  return 1;
}

static int resident_endpoint_source_sprite_part(void){
  for(unsigned variant=0;variant<9u;variant++){
    GmlRender *r=calloc(1,sizeof *r);
    GmlWin content={.bytecode=17};
    FakeDevice fake={.on_demand=1};
    GmlRenderDeviceServices device=service(&fake);
    uint8_t rgba[16]={255,0,255,255,0,255,0,255,
                     19,37,53,0,255,255,255,255};
    uint32_t base[16],shadow[16];
    CHECK(r && !gml_render_init(r,NULL));r->win=&content;
    if(variant>=1u && variant<=3u) rgba[variant-1u]=127u;
    if(variant==4u) rgba[3]=128u;
    if(variant==7u) rgba[0]=127u;
    free(r->atlas[0].px);
    r->atlas[0].px=malloc(sizeof rgba);CHECK(r->atlas[0].px);
    memcpy(r->atlas[0].px,rgba,sizeof rgba);r->atlas[0].w=r->atlas[0].h=2;
    r->tpag=calloc(1,sizeof *r->tpag);r->spr=calloc(1,sizeof *r->spr);
    CHECK(r->tpag && r->spr);r->n_tpag=r->n_spr=1;
    r->tpag[0]=(GmlTpag){.sw=2,.sh=2,.tw=2,.th=2,.bw=4,.bh=4,.tx=1,.ty=1};
    r->spr[0]=(GmlSprite){.w=4,.h=4,.n_frames=1};
    r->spr[0].frame=calloc(1,sizeof(int));CHECK(r->spr[0].frame);
    if(variant==5u) r->atlas[0].runtime_font_page=1;
    if(variant==6u){
      free(r->atlas[0].px);r->atlas[0].w=257;r->atlas[0].h=256;
      r->atlas[0].px=malloc(257u*256u*4u);CHECK(r->atlas[0].px);
      memset(r->atlas[0].px,255,257u*256u*4u);
      r->tpag[0]=(GmlTpag){.sw=257,.sh=256,.tw=257,.th=256,.bw=257,.bh=256};
      r->spr[0].w=257;r->spr[0].h=256;
    }
    gml_render_application_surface_bind(r,base,4,4,1);
    device.observe_on_demand=fake_observe_on_demand;gml_render_bind_device(r,&device);
    CHECK(gml_render_begin_resident_base(r,base,4,4,0,0));
    int target=gml_surface_create(r,4,4);CHECK(target>0 && gml_surface_set_target(r,target));
    for(int pixel=0;pixel<16;pixel++) r->fb[pixel]=0xA5ABCDEFu;
    memcpy(shadow,r->fb,sizeof shadow);
    gml_render_set_pending_fill(r,0x40123456u);gml_render_flush_pending_fill(r);
    uint32_t tint=variant==7u?0x1234FFu:variant==8u?0x80FE01u:0x123456u;
    gml_draw_sprite_part_ext(r,0,0,1,1,2,2,-1,-1,2,2,tint,1.0);
    if(variant==0u || variant>=7u){
      GmlRenderJournalMetrics metrics={0};
      CHECK(r->resident_application_pass && !fake.readbacks);
      CHECK(gml_render_scene_submit_pending(r));
      gml_render_resource_journal_metrics(gml_render_scene_target_resource_ensure(r),&metrics);
      uint32_t mask=variant==7u?6u:7u;
      CHECK(fake.last_batch_operations==1u && fake.last_filter==GML_RENDER_FILTER_NEAREST &&
            fake.last_quad.source_endpoint_rgb_mask==mask &&
            r->tpag[0].scene_endpoint_rgb_checked && r->tpag[0].scene_endpoint_rgb_mask==mask &&
            metrics.authority==GML_RENDER_RESOURCE_GPU_RESIDENT && !metrics.cpu_replays &&
            !metrics.journal_bytes && !memcmp(shadow,r->fb,sizeof shadow));
      /* Repeated resolved calls reuse this certificate, not a source snapshot. */
      gml_draw_sprite_part_ext(r,0,0,1,1,2,2,-1,-1,2,2,tint,1.0);
      CHECK(r->resident_application_pass && !fake.readbacks &&
            r->tpag[0].scene_endpoint_rgb_mask==mask);
      CHECK(gml_render_scene_submit_pending(r));
      gml_render_texture_page_cache_clear(r,&r->tpag[0]);
      CHECK(!r->tpag[0].scene_endpoint_rgb_checked && !r->tpag[0].scene_endpoint_rgb_mask);
    }else if(variant!=5u){
      GmlRenderJournalMetrics metrics={0};
      CHECK(r->resident_application_pass && !fake.readbacks);
      CHECK(gml_render_scene_submit_pending(r));
      gml_render_resource_journal_metrics(gml_render_scene_target_resource_ensure(r),&metrics);
      CHECK(fake.last_source_count==1u && metrics.authority==GML_RENDER_RESOURCE_GPU_RESIDENT &&
        !metrics.cpu_replays && !metrics.journal_bytes && !memcmp(shadow,r->fb,sizeof shadow));
    }else{
      CHECK(!r->resident_application_pass);
    }
    gml_render_free(r);free(r);
  }
  return 1;
}

static int resident_modern_unit_sprite_part(void){
  for(unsigned excluded=0;excluded<24u;excluded++) for(unsigned world=0;world<2u;world++){
    GmlRender *r=calloc(1,sizeof *r);
    GmlWin content={.bytecode=17};
    FakeDevice fake={.on_demand=1};
    GmlRenderDeviceServices device=service(&fake);
    uint32_t shadow[16*12],*borrowed_phase=NULL;
    CHECK(r && !gml_render_init(r,NULL) && gml_render_application_surface_ensure_owned(r,16,12));
    r->win=&content;
    free(r->atlas[0].px);r->atlas[0].px=malloc(8u*4u*4u);
    r->atlas[0].w=8;r->atlas[0].h=4;
    r->tpag=calloc(2,sizeof *r->tpag);r->spr=calloc(1,sizeof *r->spr);
    CHECK(r->atlas[0].px && r->tpag && r->spr);
    r->n_tpag=2;r->n_spr=1;
    for(unsigned pixel=0;pixel<32u;pixel++){
      r->atlas[0].px[pixel*4u]=(uint8_t)(17u+pixel*3u);
      r->atlas[0].px[pixel*4u+1u]=37u;r->atlas[0].px[pixel*4u+2u]=93u;
      r->atlas[0].px[pixel*4u+3u]=255u;
    }
    r->atlas[0].px[(2u*8u+6u)*4u+3u]=0u;
    if(excluded==3u) r->atlas[0].px[(1u*8u+4u)*4u+3u]=128u;
    r->tpag[0]=(GmlTpag){.sw=2,.sh=2,.tw=2,.th=2,.bw=2,.bh=2};
    r->tpag[1]=(GmlTpag){.sx=4,.sy=1,.sw=3,.sh=2,.tw=3,.th=2,.bw=6,.bh=5,.tx=1,.ty=2};
    r->spr[0]=(GmlSprite){.w=6,.h=5,.n_frames=1,.originx=31,.originy=-7};
    r->spr[0].frame=malloc(sizeof(int));CHECK(r->spr[0].frame);r->spr[0].frame[0]=1;
    device.observe_on_demand=fake_observe_on_demand;gml_render_bind_device(r,&device);
    CHECK(gml_render_begin_resident_owned_application(r,0,0,0));
    for(unsigned pixel=0;pixel<16*12;pixel++) r->fb[pixel]=0xA5ABCDEFu;
    memcpy(shadow,r->fb,sizeof shadow);
    gml_render_set_pending_fill(r,0x40123456u);
    CHECK(gml_render_resident_atlas_page(r,0,2,2,1,1,GML_RENDER_SCENE_BACKGROUND));
    int submits=fake.submits;
    double xs=1,alpha=1,x=1,sx=1;uint32_t colour=0xFFFFFFu;
    if(excluded==1u) fake.on_demand=0;
    if(excluded==2u) content.bytecode=15;
    if(excluded==4u) alpha=0.5;
    if(excluded==5u) colour=0x112233u;
    if(excluded==6u) xs=-1;
    if(excluded==7u) xs=2;
    if(excluded==8u) r->color_write_mask=7u;
    if(excluded==9u) r->blendmode=1;
    if(excluded==10u) r->alpha_test_enable=1;
    if(excluded==11u) r->fog_flat=1;
    if(excluded==12u) r->gui_pass_active=1;
    if(excluded==13u) r->tpag[1].bw=7;
    if(excluded==14u) r->tpag[1].tx=-1;
    if(excluded==15u) r->tpag[1].tw=4;
    if(excluded==16u) r->atlas[0].runtime_font_page=1;
    if(excluded==17u) r->classic=1;
    if(excluded==18u) r->tpag[1].project_authored_edges=1;
    if(excluded==19u) x=1.25;
    if(excluded==20u) sx=1.25;
    if(excluded==21u) r->active_shader=0;
    if(excluded==22u){
      borrowed_phase=calloc(16*12,sizeof(uint32_t));CHECK(borrowed_phase);
      r->app_interp_phase[0]=borrowed_phase;
    }
    if(excluded==23u) r->interp=1;
    r->world_transform_active=(int)world;r->world_scale_x=2;r->world_scale_y=3;
    double wx=world?2:1,wy=world?3:1;
    gml_draw_sprite_part_ext(r,0,0,sx,2,2,1,x/wx,1/wy,xs/wx,1/wy,colour,alpha);
    r->world_transform_active=0;
    if(!excluded){
      GmlRenderJournalMetrics before_empty={0},after_empty={0},metrics={0};
      CHECK(r->resident_application_pass && fake.submits==submits && !fake.readbacks);
      GmlRenderResource *resource=gml_render_scene_target_resource_ensure(r);CHECK(resource);
      gml_render_resource_journal_metrics(resource,&before_empty);
      gml_draw_sprite_part_ext(r,0,0,1,2,2,1,100,100,1,1,colour,alpha);
      gml_render_resource_journal_metrics(resource,&after_empty);
      CHECK(before_empty.logical_version==after_empty.logical_version &&
        before_empty.authority==after_empty.authority && fake.submits==submits);
      CHECK(gml_render_scene_submit_pending(r));
      gml_render_resource_journal_metrics(resource,&metrics);
      CHECK(fake.last_batch_operations==2u && fake.last_filter==GML_RENDER_FILTER_NEAREST &&
        fake.last_quad.x[0]==1 && fake.last_quad.y[0]==1 &&
        fake.last_quad.x[1]==3 && fake.last_quad.y[2]==2 &&
        !fake.readbacks && !fake.rgb_queries && metrics.authority==GML_RENDER_RESOURCE_GPU_RESIDENT &&
        !metrics.cpu_replays && !metrics.journal_bytes && !metrics.retained_mutable_bytes &&
        !memcmp(shadow,r->fb,sizeof shadow));
    }else if((excluded>=2u && excluded<=10u) || excluded==12u || excluded==15u ||
             excluded==17u || excluded==18u || excluded==19u || excluded==20u || excluded==23u){
      GmlRenderJournalMetrics metrics={0};
      CHECK(r->resident_application_pass && !fake.readbacks);
      CHECK(gml_render_scene_submit_pending(r));
      gml_render_resource_journal_metrics(gml_render_scene_target_resource_ensure(r),&metrics);
      CHECK(fake.last_source_count==1u && metrics.authority==GML_RENDER_RESOURCE_GPU_RESIDENT &&
        !metrics.cpu_replays && !metrics.journal_bytes && !memcmp(shadow,r->fb,sizeof shadow));
    }else{
      if(r->resident_application_pass) fprintf(stderr,"modern unit part exclusion: case=%u world=%u\n",excluded,world);
      CHECK(!r->resident_application_pass);
    }
    gml_render_free(r);free(borrowed_phase);free(r);
  }
  return 1;
}

static int resident_trimmed_unit_layer(void){
  for(unsigned excluded=0;excluded<21u;excluded++){
    GmlRender *r=calloc(1,sizeof *r);
    GmlWin content={.bytecode=17};
    FakeDevice fake={.on_demand=1};
    GmlRenderDeviceServices device=service(&fake);
    uint32_t shadow[16*12];
    CHECK(r && !gml_render_init(r,NULL) && gml_render_application_surface_ensure_owned(r,16,12));
    r->win=&content;
    free(r->atlas[0].px);r->atlas[0].px=malloc(8u*4u*4u);
    r->atlas[0].w=8;r->atlas[0].h=4;
    r->tpag=calloc(2,sizeof *r->tpag);r->spr=calloc(1,sizeof *r->spr);
    CHECK(r->atlas[0].px && r->tpag && r->spr);
    r->n_tpag=2;r->n_spr=1;
    for(unsigned pixel=0;pixel<32u;pixel++){
      r->atlas[0].px[pixel*4u]=(uint8_t)(17u+pixel*3u);
      r->atlas[0].px[pixel*4u+1u]=37u;r->atlas[0].px[pixel*4u+2u]=93u;
      r->atlas[0].px[pixel*4u+3u]=255u;
    }
    if(excluded==3u || excluded==20u) r->atlas[0].px[(1u*8u+4u)*4u+3u]=128u;
    r->tpag[0]=(GmlTpag){.sw=2,.sh=2,.tw=2,.th=2,.bw=2,.bh=2};
    r->tpag[1]=(GmlTpag){.sx=4,.sy=1,.sw=3,.sh=2,.tw=3,.th=2,.bw=6,.bh=5,.tx=1,.ty=2};
    r->spr[0]=(GmlSprite){.w=6,.h=5,.n_frames=1,.originx=31,.originy=-7};
    r->spr[0].frame=malloc(sizeof(int));CHECK(r->spr[0].frame);
    r->spr[0].frame[0]=1;
    device.observe_on_demand=fake_observe_on_demand;gml_render_bind_device(r,&device);
    CHECK(gml_render_begin_resident_owned_application(r,0.25,-0.25,0));
    for(unsigned pixel=0;pixel<16*12;pixel++) r->fb[pixel]=0xA5ABCDEFu;
    memcpy(shadow,r->fb,sizeof shadow);
    gml_render_set_pending_fill(r,0x40123456u);
    CHECK(gml_render_resident_atlas_page(r,0,2,2,1,1,GML_RENDER_SCENE_BACKGROUND));
    int submits=fake.submits;
    double xs=1.0,alpha=1.0;uint32_t colour=0xFFFFFFu;
    if(excluded==1u) fake.on_demand=0;
    if(excluded==2u) content.bytecode=15;
    if(excluded==4u) alpha=0.5;
    if(excluded==5u) colour=0x112233u;
    if(excluded==6u) xs=-1.0;
    if(excluded==7u) xs=2.0;
    if(excluded==8u) r->color_write_mask=7u;
    if(excluded==9u) r->blendmode=1;
    if(excluded==10u) r->alpha_test_enable=1;
    if(excluded==11u) r->fog_flat=1;
    if(excluded==12u) r->gui_pass_active=1;
    if(excluded==13u) r->tpag[1].bw=7;
    if(excluded==14u) r->tpag[1].tx=-1;
    if(excluded==15u) r->tpag[1].tw=4;
    if(excluded==16u) r->atlas[0].runtime_font_page=1;
    if(excluded==17u) r->classic=1;
    if(excluded==18u) r->tpag[1].project_authored_edges=1;
    if(excluded==19u || excluded==20u) r->interp=1;
    if(excluded==20u){r->alpha_test_enable=1;r->alpha_test_ref=1;}
    gml_draw_layer_background_sprite(r,0,0,0.25,-0.25,xs,1,colour,alpha,1,1);
    if(!excluded){
      GmlRenderJournalMetrics metrics={0},before_empty={0},after_empty={0};
      if(!r->resident_application_pass || fake.submits!=submits || fake.readbacks)
        fprintf(stderr,"trimmed layer admission: resident=%d submits=%d/%d reads=%d interp=%d runtime_font=%d target=%d/%d mask=%u blend=%d alpha=%d/%d\n",
          r->resident_application_pass,fake.submits,submits,fake.readbacks,r->interp,
          r->atlas[0].runtime_font_page,r->target_id,r->target_sp,r->color_write_mask,
          r->blendmode,r->tpag[1].scene_opaque_checked,r->tpag[1].scene_binary_alpha);
      CHECK(r->resident_application_pass && fake.submits==submits && !fake.readbacks);
      GmlRenderResource *resource=gml_render_scene_target_resource_ensure(r);CHECK(resource);
      gml_render_resource_journal_metrics(resource,&before_empty);
      gml_draw_layer_background_sprite(r,0,0,100,100,1,1,colour,alpha,0,0);
      gml_render_resource_journal_metrics(resource,&after_empty);
      CHECK(before_empty.logical_version==after_empty.logical_version &&
        before_empty.authority==after_empty.authority && fake.submits==submits);
      CHECK(gml_render_scene_submit_pending(r));
      gml_render_resource_journal_metrics(resource,&metrics);
      CHECK(fake.last_batch_operations==7u && !fake.readbacks && !fake.rgb_queries &&
        metrics.authority==GML_RENDER_RESOURCE_GPU_RESIDENT && !metrics.cpu_replays &&
        !metrics.journal_bytes && !metrics.retained_mutable_bytes && !memcmp(shadow,r->fb,sizeof shadow));
    }else if(excluded==2u || excluded==3u || excluded==4u || excluded==5u ||
             excluded==6u || excluded==7u || excluded==9u || excluded==10u ||
             excluded==12u || excluded==15u || excluded==17u || excluded==19u){
      /* The complete layer-cell matrix supersedes these old admission-only
       * frontiers. Retain authority and stale-shadow checks here; real paired
       * device controls own their unchanged CPU pixels and D2 contract. */
      GmlRenderJournalMetrics metrics={0};
      if(!r->resident_application_pass)
        fprintf(stderr,"extended layer admission case=%u\n",excluded);
      CHECK(r->resident_application_pass && !fake.readbacks &&
            !memcmp(shadow,r->fb,sizeof shadow) && gml_render_scene_submit_pending(r));
      gml_render_resource_journal_metrics(gml_render_scene_target_resource_ensure(r),&metrics);
      CHECK(metrics.authority==GML_RENDER_RESOURCE_GPU_RESIDENT && !metrics.cpu_replays &&
            !metrics.journal_bytes && !fake.readbacks);
    }else CHECK(!r->resident_application_pass);
    gml_render_free(r);free(r);
  }
  return 1;
}

static int resident_screen_binary_rotation(void){
  for(int excluded=0;excluded<16;excluded++) for(int world=0;world<2;world++){
    GmlRender *r=calloc(1,sizeof *r);
    GmlWin content={.bytecode=17};
    FakeDevice fake={.on_demand=1};
    GmlRenderDeviceServices device=service(&fake);
    uint32_t shadow[16*12];
    uint32_t *borrowed_phase=NULL;
    const uint8_t rgba[16]={208,32,16,255,16,208,32,255,32,16,208,0,192,176,160,255};
    CHECK(r && gml_render_application_surface_ensure_owned(r,16,12));
    r->win=&content;r->active_shader=-1;r->color_write_mask=15u;r->alpha=1;
    r->atlas=calloc(1,sizeof *r->atlas);r->tpag=calloc(1,sizeof *r->tpag);
    r->spr=calloc(1,sizeof *r->spr);CHECK(r->atlas && r->tpag && r->spr);
    r->n_atlas=r->n_tpag=r->n_spr=1;
    r->atlas[0].px=malloc(sizeof rgba);r->spr[0].frame=calloc(1,sizeof(int));
    CHECK(r->atlas[0].px && r->spr[0].frame);
    memcpy(r->atlas[0].px,rgba,sizeof rgba);r->atlas[0].w=r->atlas[0].h=2;
    /* Choose immutable source bytes before their first retained scene view. */
    if(excluded==2) r->atlas[0].px[3]=128;
    r->tpag[0]=(GmlTpag){.sw=2,.sh=2,.tw=2,.th=2,.bw=4,.bh=4,.tx=1,.ty=1};
    r->spr[0].w=r->spr[0].h=4;r->spr[0].originx=r->spr[0].originy=2;r->spr[0].n_frames=1;
    for(unsigned p=0;p<16*12;p++) r->app_surface_owned[p]=0xA5ABCDEFu;
    memcpy(shadow,r->app_surface_owned,sizeof shadow);
    device.observe_on_demand=fake_observe_on_demand;gml_render_bind_device(r,&device);
    CHECK(gml_render_begin_resident_owned_application(r,0,0,0));
    r->interp=0;gml_render_set_pending_fill(r,0x40123456u);
    if(excluded!=2) gml_draw_sprite_ext(r,0,0,3,3,1,1,0,0xFFFFFFu,1);
    CHECK(r->resident_application_pass && !fake.readbacks);
    int submits=fake.submits;
    double alpha=1.0,angle=27.0;uint32_t colour=0xFFFFFFu;
    if(excluded==1) fake.on_demand=0;
    if(excluded==3) alpha=0.5;
    if(excluded==4) colour=0x112233u;
    if(excluded==5) r->active_shader=0;
    if(excluded==6) r->color_write_mask=7;
    if(excluded==7) r->fog_flat=1;
    if(excluded==8) r->alpha_test_enable=1;
    if(excluded==9) r->classic=1;
    if(excluded==10) content.bytecode=15;
    if(excluded==11) r->gui_pass_active=1;
    if(excluded==12){
      borrowed_phase=calloc(16*12,sizeof(uint32_t));CHECK(borrowed_phase);
      r->app_interp_phase[0]=borrowed_phase;
    }
    if(excluded==13) r->tpag[0].project_authored_edges=1;
    if(excluded==14) angle=0.0005;
    if(excluded==15) colour=0u;
    r->world_transform_active=world;r->world_scale_x=4;r->world_scale_y=2;
    gml_draw_sprite_ext(r,0,0,world?2.0:8.0,world?3.0:6.0,
      world?0.5:2.0,world?1.0:2.0,angle,colour,alpha);
    if(!excluded){
      GmlRenderJournalMetrics metrics={0};
      /* The binary axis prefix uses source-over; opaque regions use replacement.
       * That observable blend-state change flushes exactly the queued prefix. */
      if(!r->resident_application_pass || fake.submits!=submits+1 || fake.readbacks)
        fprintf(stderr,"screen binary rotation admission: world=%d resident=%d submits=%d/%d reads=%d\n",
          world,r->resident_application_pass,fake.submits,submits+1,fake.readbacks);
      CHECK(r->resident_application_pass && fake.submits==submits+1 && !fake.readbacks);
      CHECK(gml_render_scene_submit_pending(r));
      gml_render_resource_journal_metrics(gml_render_scene_target_resource_ensure(r),&metrics);
      CHECK(fake.last_batch_operations>1u && fake.last_filter==GML_RENDER_FILTER_NEAREST &&
            !fake.last_quad.span_active && !fake.last_quad.sampling.quantisation &&
            metrics.authority==GML_RENDER_RESOURCE_GPU_RESIDENT && !metrics.cpu_replays &&
            !metrics.journal_bytes && !metrics.retained_mutable_bytes &&
            !memcmp(shadow,r->app_surface_owned,sizeof shadow));
    } else if(excluded==2 || excluded==3 || excluded==4 || excluded==6 || excluded==7 ||
              (excluded>=8 && excluded<=11) || excluded>=14){
      CHECK(nearest_sprite_extension_transport(r,&fake,submits+1));
      CHECK(!memcmp(shadow,r->app_surface_owned,sizeof shadow));
    } else {
      if(r->resident_application_pass)
        fprintf(stderr,"screen binary rotation exclusion: case=%d world=%d GUIscale=%g,%g\n",
          excluded,world,r->gui_scale_x,r->gui_scale_y);
      CHECK(!r->resident_application_pass);
    }
    /* Sample planes are borrowed; their injecting fixture owns the storage. */
    r->world_transform_active=0;gml_render_free(r);free(borrowed_phase);free(r);
  }
  return 1;
}

static int resident_fractional_screen_font(void){
  for(int excluded=0;excluded<16;excluded++) for(int world=0;world<2;world++){
    GmlRender *render=calloc(1,sizeof *render);
    GmlWin content={.bytecode=17};
    FakeDevice fake={.on_demand=1};
    GmlRenderDeviceServices device=service(&fake);
    uint32_t screen[16*12],shadow[16*12];
    const uint8_t rgba[16]={208,32,16,255,16,208,32,255,
                           32,16,208,0,192,176,160,255};
    CHECK(render && !gml_render_init(render,NULL));
    render->win=&content;
    free(render->atlas[0].px);
    render->atlas[0].px=malloc(sizeof rgba);CHECK(render->atlas[0].px);
    memcpy(render->atlas[0].px,rgba,sizeof rgba);
    /* Author partial alpha before the immutable source is first submitted. */
    if(excluded==4) render->atlas[0].px[3]=128;
    render->atlas[0].w=render->atlas[0].h=2;
    GmlFont *font=&render->fonts[0];
    render->font=0;render->n_fonts=1;
    font->real=1;font->sprite=-1;font->atlas=0;
    font->line_height=font->align_height=2;
    free(font->glyphs);
    font->glyphs=calloc(1,sizeof *font->glyphs);CHECK(font->glyphs);
    font->n_glyphs=1;font->glyphs_sorted=1;
    for(int index=0;index<256;index++) font->glyph_by_char[index]=-1;
    font->glyph_by_char['A']=0;
    font->glyphs[0]=(GmlGlyph){.w=2,.h=2,.shift=2,.ch='A'};
    for(unsigned index=0;index<16*12;index++) screen[index]=0xA5ABCDEFu;
    gml_render_application_surface_bind(render,screen,16,12,1);
    device.observe_on_demand=fake_observe_on_demand;
    gml_render_bind_device(render,&device);
    CHECK(gml_render_begin_resident_base(render,screen,16,12,0,0));
    memcpy(shadow,render->fb,sizeof shadow);
    gml_render_set_pending_fill(render,0xFF123456u);
    gml_draw_text(render,2,2,"A");
    CHECK(render->resident_application_pass && !fake.readbacks);
    int submits=fake.submits;
    double xs=1.5,ys=1.25,alpha=1.0;
    uint32_t colour=0xFFFFFFu;
    if(excluded==1) fake.on_demand=0;
    if(excluded==2) font->subpixel=1;
    if(excluded==3){font->runtime_owned=1;render->atlas[0].runtime_font_page=1;}
    if(excluded==5) alpha=0.5;
    if(excluded==6) colour=0x112233u;
    if(excluded==7) render->interp=1;
    if(excluded==8) gml_render_gui_begin(render,16,12);
    if(excluded==9){
      int id=gml_surface_create(render,16,12);CHECK(id>0 && gml_surface_set_target(render,id));
      gml_render_set_pending_fill(render,0xFF123456u);gml_render_flush_pending_fill(render);
      CHECK(gml_render_scene_submit_pending(render));
      memcpy(shadow,render->fb,sizeof shadow);submits=fake.submits;
    }
    if(excluded==10) content.bytecode=15;
    if(excluded==11) xs=4.25;
    if(excluded==12) ys=0.5;
    if(excluded==13) render->cam_x=0.25;
    if(excluded==14) render->active_shader=0;
    if(excluded==15) font->sdf_spread=1;
    render->world_transform_active=world;
    render->world_scale_x=4;render->world_scale_y=2;
    /* GUI and numbered targets do not apply the retained room-world map. */
    int mapped_world=world && excluded!=8 && excluded!=9;
    double wx=mapped_world?4.0:1.0,wy=mapped_world?2.0:1.0;
    gml_draw_text_transformed(render,-1.0/wx,-1.0/wy,"A",xs/wx,ys/wy,0,colour,alpha);
    /* R03-TEXT-NEAREST-FUNCTION's paired matrix supersedes these former
     * admission fences. Original positive and out-of-domain controls remain. */
    int admitted=!excluded || (excluded>=3 && excluded<=13 && excluded!=7);
    /* Keep the original clipped attempt and add a visible witness for each
     * newly admitted variant: a minified glyph can otherwise be a no-op. */
    if(admitted && excluded)
      gml_draw_text_transformed(render,4.0/wx,4.0/wy,"A",xs/wx,ys/wy,0,colour,alpha);
    if(admitted){
      GmlRenderJournalMetrics metrics={0};
      if(!render->resident_application_pass || fake.submits!=submits || fake.readbacks)
        fprintf(stderr,"fractional font admission: world=%d resident=%d submits=%d/%d reads=%d\n",
          world,render->resident_application_pass,fake.submits,submits,fake.readbacks);
      CHECK(render->resident_application_pass && fake.submits==submits && !fake.readbacks);
      CHECK(gml_render_scene_submit_pending(render));
      gml_render_resource_journal_metrics(gml_render_scene_target_resource_ensure(render),&metrics);
      if(fake.last_batch_operations<(excluded==9?1u:2u) || fake.readbacks ||
         fake.last_filter!=GML_RENDER_FILTER_NEAREST ||
         metrics.authority!=GML_RENDER_RESOURCE_GPU_RESIDENT || metrics.cpu_replays ||
         metrics.journal_bytes || metrics.retained_mutable_bytes ||
         memcmp(shadow,render->fb,sizeof shadow))
        fprintf(stderr,"fractional font tuple: excluded=%d world=%d operations=%u reads=%d filter=%d authority=%d replays=%llu journal=%llu retained=%llu shadow=%d\n",
          excluded,world,fake.last_batch_operations,fake.readbacks,(int)fake.last_filter,
          (int)metrics.authority,(unsigned long long)metrics.cpu_replays,
          (unsigned long long)metrics.journal_bytes,(unsigned long long)metrics.retained_mutable_bytes,
          !memcmp(shadow,render->fb,sizeof shadow));
      CHECK(fake.last_batch_operations>=(excluded==9?1u:2u) && !fake.readbacks &&
            fake.last_filter==GML_RENDER_FILTER_NEAREST &&
            metrics.authority==GML_RENDER_RESOURCE_GPU_RESIDENT && !metrics.cpu_replays &&
            !metrics.journal_bytes && !metrics.retained_mutable_bytes &&
            !memcmp(shadow,render->fb,sizeof shadow));
    }else CHECK(!render->resident_application_pass);
    render->world_transform_active=0;
    gml_render_free(render);free(render);
  }
  return 1;
}

static int resident_integer_sprite_rounded_noop(void){
  for(int axis=0;axis<4;axis++) for(int inside=0;inside<2;inside++)
  for(int world=0;world<2;world++) for(int scale=2;scale<=4;scale+=2){
    GmlRender *render=calloc(1,sizeof *render);
    FakeDevice fake={.on_demand=1};
    GmlRenderDeviceServices device=service(&fake);
    uint32_t prior[256];
    CHECK(render && !gml_render_init(render,NULL));
    CHECK(gml_render_application_surface_ensure_owned(render,16,16));
    render->tpag=calloc(1,sizeof *render->tpag);
    render->spr=calloc(1,sizeof *render->spr);
    render->bg=calloc(1,sizeof *render->bg);
    CHECK(render->tpag && render->spr && render->bg);
    render->n_tpag=render->n_spr=render->n_bg=1;
    free(render->atlas[0].px);
    render->atlas[0].px=malloc(16);CHECK(render->atlas[0].px);
    memset(render->atlas[0].px,255,16);
    render->atlas[0].w=render->atlas[0].h=2;
    render->tpag[0]=(GmlTpag){.sw=2,.sh=2,.tw=2,.th=2,.bw=2,.bh=2};
    render->spr[0]=(GmlSprite){.w=2,.h=2,.originx=1,.originy=1,.n_frames=1};
    render->spr[0].frame=calloc(1,sizeof(int));CHECK(render->spr[0].frame);
    double left=2,top=2,edge=inside?0.51:0.49;
    if(axis==0) left=16-edge;
    if(axis==1) left=-2*scale+edge;
    if(axis==2) top=16-edge;
    if(axis==3) top=-2*scale+edge;
    CHECK(gml_render_begin_owned_application(render,0,0,0));
    gml_render_set_pending_fill(render,0xFF123456u);
    gml_draw_background(render,0,7,7);
    memcpy(prior,render->fb,sizeof prior);
    render->world_transform_active=world;
    render->world_scale_x=render->world_scale_y=4;
    gml_draw_sprite_ext(render,0,0,(left+scale)/(world?4:1),
      (top+scale)/(world?4:1),scale/(world?4.0:1),scale/(world?4.0:1),0,0xFFFFFFu,1);
    CHECK((memcmp(prior,render->fb,sizeof prior)!=0)==inside);
    render->world_transform_active=0;
    device.observe_on_demand=fake_observe_on_demand;
    gml_render_bind_device(render,&device);
    CHECK(gml_render_begin_resident_owned_application(render,0,0,0));
    gml_render_set_pending_fill(render,0xFF123456u);
    gml_draw_background(render,0,7,7);
    int submits=fake.submits;
    render->world_transform_active=world;
    render->world_scale_x=render->world_scale_y=4;
    gml_draw_sprite_ext(render,0,0,(left+scale)/(world?4:1),
      (top+scale)/(world?4:1),scale/(world?4.0:1),scale/(world?4.0:1),0,0xFFFFFFu,1);
    CHECK(render->resident_application_pass && fake.submits==submits && !fake.readbacks);
    if(inside) CHECK(render->pending_scene_batch && gml_render_scene_submit_pending(render) &&
      fake.submits==submits+1 && fake.last_batch_operations>=1u &&
      fake.last_quantisation==GML_RENDER_QUANTISE_INTEGER_TINT_TRUNC && !fake.readbacks);
    gml_render_free(render);free(render);
  }
  return 1;
}

static int resident_background_and_tile_cpu_barriers(void){
  GmlRender *render=(GmlRender*)calloc(1,sizeof *render);
  FakeDevice fake={0};
  GmlRenderDeviceServices device=service(&fake);
  const uint8_t rgba[16]={
    0xD0u,0x20u,0x10u,0xFFu, 0x10u,0xD0u,0x20u,0xFFu,
    0x20u,0x10u,0xD0u,0xFFu, 0xC0u,0xB0u,0xA0u,0xFFu
  };
  CHECK(render && gml_render_application_surface_ensure_owned(render,4,4));
  render->atlas=(GmlAtlas*)calloc(1,sizeof *render->atlas);
  render->tpag=(GmlTpag*)calloc(1,sizeof *render->tpag);
  render->spr=(GmlSprite*)calloc(1,sizeof *render->spr);
  render->bg=(GmlBg*)calloc(1,sizeof *render->bg);
  CHECK(render->atlas && render->tpag && render->spr && render->bg);
  render->n_atlas=render->n_tpag=render->n_spr=render->n_bg=1;
  render->atlas[0].px=(uint8_t*)malloc(sizeof rgba);
  render->spr[0].frame=(int*)calloc(1,sizeof(int));
  CHECK(render->atlas[0].px && render->spr[0].frame);
  memcpy(render->atlas[0].px,rgba,sizeof rgba);
  render->atlas[0].w=render->atlas[0].h=2;
  render->tpag[0].sw=render->tpag[0].sh=2;
  render->tpag[0].tw=render->tpag[0].th=2;
  render->tpag[0].bw=render->tpag[0].bh=2;
  render->spr[0].w=render->spr[0].h=2;
  render->spr[0].n_frames=1;
  render->active_shader=-1;
  render->color_write_mask=0x0Fu;
  render->alpha=1.0;
  render->interp=1;
  gml_render_bind_device(render,&device);

  CHECK(gml_render_begin_resident_owned_application(render,0.0,0.0,0));
  gml_render_set_pending_fill(render,0xFF000000u);
  gml_draw_sprite_ext(render,0,0,0.0,0.0,1.0,1.0,0.0,0xFFFFFFu,1.0);
  CHECK(render->resident_application_pass);
  gml_draw_background(render,0,1.0,0.0);
  CHECK(!render->resident_application_pass && !render->pending_fill &&
        render->app_surface_owned[0]==0xFFD02010u &&
        render->app_surface_owned[1]==0xFFD02010u &&
        render->app_surface_owned[2]==0xFF10D020u && fake.readbacks==0);

  CHECK(gml_render_begin_resident_owned_application(render,0.0,0.0,0));
  gml_render_set_pending_fill(render,0xFF000000u);
  gml_draw_sprite_ext(render,0,0,0.0,0.0,1.0,1.0,0.0,0xFFFFFFu,1.0);
  CHECK(render->resident_application_pass);
  gml_draw_tile(render,0,0,0,1,1,2.0,0.0);
  CHECK(!render->resident_application_pass && !render->pending_fill &&
        render->app_surface_owned[0]==0xFFD02010u &&
        render->app_surface_owned[2]==0xFFD02010u && fake.readbacks==0);

  /* Every public background shape must finish the queued device prefix before its unchanged
   * software writer. Exact pixels for the plain background and tile are checked above. */
  for(int variant=0;variant<7;variant++){
    CHECK(gml_render_begin_resident_owned_application(render,0.0,0.0,0));
    gml_render_set_pending_fill(render,0xFF000000u);
    gml_draw_sprite_ext(render,0,0,0.0,0.0,1.0,1.0,0.0,0xFFFFFFu,1.0);
    CHECK(render->resident_application_pass);
    switch(variant){
      case 0: gml_draw_background_stretched(render,0,1,0,2,2,0xFFFFFFu,1.0); break;
      case 1: gml_draw_background_part_ext(render,0,0,0,1,1,1,0,1,1,0xFFFFFFu,1.0); break;
      case 2: gml_draw_background_tile(render,0,0,0,1,1,1,0,1,1,0,0,0,0xFFFFFFu,1.0); break;
      case 3: gml_draw_background_ext(render,0,1,0,1,1,0xFFFFFFu,1.0); break;
      case 4: gml_draw_background_tiled(render,0,0,0,1,1); break;
      case 5: gml_draw_background_tiled_ext(render,0,0,0,1,1,0xFFFFFFu,1.0,1,1); break;
      case 6: gml_draw_layer_background_sprite(render,0,0,1,0,1,1,0xFFFFFFu,1.0,0,0); break;
    }
    CHECK(!render->resident_application_pass && !render->pending_fill && fake.readbacks==0);
  }
  /* Nearest, complete atlas backgrounds and tiles share the resident quad preparation.
   * The filtered variants above remain synchronized CPU fallbacks. */
  render->interp=0;
  CHECK(gml_render_begin_resident_owned_application(render,0.0,0.0,0));
  gml_render_set_pending_fill(render,0xFF000000u);
  gml_draw_sprite_tiled_ext(render,0,0,0,0,1,1,0xFFFFFFu,1.0);
  CHECK(render->resident_application_pass && fake.readbacks==0);
  CHECK(gml_render_resident_application_cpu_barrier(render));
  CHECK(!render->resident_application_pass && fake.readbacks==0 &&
        render->app_surface_owned[0]==0xFFD02010u &&
        render->app_surface_owned[1]==0xFF10D020u &&
        render->app_surface_owned[2]==0xFFD02010u &&
        render->app_surface_owned[4]==0xFF2010D0u &&
        render->app_surface_owned[10]==0xFFD02010u &&
        render->app_surface_owned[15]==0xFFC0B0A0u);
  render->interp=1;
  CHECK(gml_render_begin_resident_owned_application(render,0.0,0.0,0));
  gml_render_set_pending_fill(render,0xFF000000u);
  gml_draw_sprite_tiled_ext(render,0,0,0,0,1,1,0xFFFFFFu,1.0);
  CHECK(!render->resident_application_pass && fake.readbacks==0 &&
        render->app_surface_owned[0]==0xFFD02010u &&
        render->app_surface_owned[15]==0xFFC0B0A0u);
  render->interp=0;
  CHECK(gml_render_begin_resident_owned_application(render,0.0,0.0,0));
  render->cam_x=0.5;
  gml_render_set_pending_fill(render,0xFF000000u);
  gml_draw_sprite_tiled_ext(render,0,0,0,0,1,1,0xFFFFFFu,1.0);
  CHECK(!render->resident_application_pass && fake.readbacks==0);
  render->cam_x=0.0;
  CHECK(gml_render_begin_resident_owned_application(render,0.0,0.0,0));
  gml_render_set_pending_fill(render,0xFF000000u);
  gml_draw_background(render,0,1.0,0.0);
  CHECK(render->resident_application_pass && fake.readbacks==0);
  gml_draw_background_tile(render,0,0,0,2,2,0,2,1,1,0,0,0,0xFFFFFFu,1.0);
  CHECK(render->resident_application_pass && fake.readbacks==0);
  gml_draw_tile(render,0,0,0,2,2,2,2);
  CHECK(render->resident_application_pass && fake.readbacks==0);
  gml_draw_tile(render,0,0,1,1,1,3,0);
  CHECK(render->resident_application_pass && fake.readbacks==0);
  gml_draw_background_tile(render,0,1,0,1,1,3,1,1,1,0,0,0,0xFFFFFFu,1.0);
  CHECK(render->resident_application_pass && fake.readbacks==0);
  gml_draw_background_part_ext(render,0,1,0,1,1,0,1,1,1,0xFFFFFFu,1.0);
  CHECK(render->resident_application_pass && fake.readbacks==0);
  CHECK(gml_render_resident_application_cpu_barrier(render));
  CHECK(!render->resident_application_pass && fake.readbacks==0 &&
        render->app_surface_owned[0]==0xFF000000u &&
        render->app_surface_owned[1]==0xFFD02010u &&
        render->app_surface_owned[2]==0xFF10D020u &&
        render->app_surface_owned[3]==0xFF2010D0u &&
        render->app_surface_owned[4]==0xFF10D020u &&
        render->app_surface_owned[7]==0xFF10D020u &&
        render->app_surface_owned[8]==0xFFD02010u &&
        render->app_surface_owned[11]==0xFF10D020u);
  /* Unit tile source selections clip against the packed page, not neighbouring
   * atlas texels. Preserve the queued prefix for empty source/destination crops. */
  for(int variant=0;variant<5;variant++){
    int sx=variant==1?0:1,sy=variant==0?0:1;
    if(variant==3) sx=2;
    CHECK(gml_render_begin_resident_owned_application(render,0.0,0.0,0));
    gml_render_set_pending_fill(render,0xFF000000u);
    gml_draw_background(render,0,2,2);
    CHECK(render->resident_application_pass);
    int submits=fake.submits;
    gml_draw_background_tile(render,0,sx,sy,2,2,variant==4?-1:0,0,
                             1,1,0,0,0,0xFFFFFFu,1.0);
    CHECK(render->resident_application_pass && fake.readbacks==0 &&
          fake.submits==submits);
    CHECK(gml_render_resident_application_cpu_barrier(render));
    CHECK(fake.last_batch_operations==(variant<3?2u:1u));
    CHECK(render->app_surface_owned[10]==0xFFD02010u &&
          render->app_surface_owned[15]==0xFFC0B0A0u && fake.readbacks==0);
    if(variant==0) CHECK(render->app_surface_owned[0]==0xFF10D020u &&
                        render->app_surface_owned[4]==0xFFC0B0A0u &&
                        render->app_surface_owned[1]==0xFF000000u);
    if(variant==1) CHECK(render->app_surface_owned[0]==0xFF2010D0u &&
                        render->app_surface_owned[1]==0xFFC0B0A0u &&
                        render->app_surface_owned[4]==0xFF000000u);
    if(variant==2) CHECK(render->app_surface_owned[0]==0xFFC0B0A0u &&
                        render->app_surface_owned[1]==0xFF000000u &&
                        render->app_surface_owned[4]==0xFF000000u);
    if(variant>=3) CHECK(render->app_surface_owned[0]==0xFF000000u &&
                        render->app_surface_owned[1]==0xFF000000u &&
                        render->app_surface_owned[4]==0xFF000000u);
  }
  /* This source-clipping extension does not admit reflected tile projections. */
  CHECK(gml_render_begin_resident_owned_application(render,0.0,0.0,0));
  gml_render_set_pending_fill(render,0xFF000000u);
  gml_draw_background_tile(render,0,1,0,2,2,2,0,1,1,1,0,0,0xFFFFFFu,1.0);
  CHECK(!render->resident_application_pass && fake.readbacks==0);
  CHECK(gml_render_begin_resident_owned_application(render,0.0,0.0,0));
  gml_render_set_pending_fill(render,0xFF000000u);
  gml_draw_background_ext(render,0,0,0,2,2,0xFFFFFFu,1.0);
  CHECK(render->resident_application_pass && fake.readbacks==0);
  CHECK(gml_render_resident_application_cpu_barrier(render));
  CHECK(render->app_surface_owned[0]==0xFFD02010u &&
        render->app_surface_owned[1]==0xFFD02010u &&
        render->app_surface_owned[2]==0xFF10D020u &&
        render->app_surface_owned[15]==0xFFC0B0A0u);
  {
    GmlWin win={0};
    uint8_t data[64]={0};
    data[4]=1;             /* one room tile */
    data[8]=16;            /* tile record address */
    data[16]=2;            /* destination x */
    data[16+12]=1;         /* source x */
    data[16+16]=1;         /* source y */
    data[16+20]=1;         /* source width */
    data[16+24]=1;         /* source height */
    win.data=data;
    win.size=sizeof data;
    render->win=&win;
    CHECK(gml_render_begin_resident_owned_application(render,0.0,0.0,0));
    gml_render_set_pending_fill(render,0xFF000000u);
    gml_draw_room_tiles(render,4);
    CHECK(render->resident_application_pass && fake.readbacks==0);
    CHECK(gml_render_resident_application_cpu_barrier(render));
    CHECK(render->app_surface_owned[0]==0xFF000000u &&
          render->app_surface_owned[2]==0xFFC0B0A0u &&
          render->app_surface_owned[3]==0xFF000000u);
    render->win=NULL;
  }
  CHECK(gml_render_begin_resident_owned_application(render,0.0,0.0,0));
  gml_render_set_pending_fill(render,0xFF000000u);
  gml_draw_sprite_ext(render,0,0,2,0,-1,1,0,0xFFFFFFu,1.0);
  CHECK(render->resident_application_pass && fake.readbacks==0);
  CHECK(gml_render_resident_application_cpu_barrier(render));
  CHECK(!render->resident_application_pass &&
        render->app_surface_owned[0]==0xFF10D020u &&
        render->app_surface_owned[1]==0xFFD02010u &&
        render->app_surface_owned[2]==0xFF000000u &&
        render->app_surface_owned[4]==0xFFC0B0A0u &&
        render->app_surface_owned[5]==0xFF2010D0u);
  CHECK(gml_render_begin_resident_owned_application(render,0.0,0.0,0));
  gml_render_set_pending_fill(render,0xFF000000u);
  gml_draw_sprite_ext(render,0,0,0,2,1,-1,0,0xFFFFFFu,1.0);
  CHECK(render->resident_application_pass && fake.readbacks==0);
  CHECK(gml_render_resident_application_cpu_barrier(render));
  CHECK(!render->resident_application_pass &&
        render->app_surface_owned[0]==0xFF2010D0u &&
        render->app_surface_owned[1]==0xFFC0B0A0u &&
        render->app_surface_owned[4]==0xFFD02010u &&
        render->app_surface_owned[5]==0xFF10D020u);
  CHECK(gml_render_begin_resident_owned_application(render,0.0,0.0,0));
  gml_render_set_pending_fill(render,0xFF000000u);
  gml_draw_background_ext(render,0,2,0,-1,1,0xFFFFFFu,1.0);
  CHECK(render->resident_application_pass && fake.readbacks==0);
  CHECK(gml_render_resident_application_cpu_barrier(render));
  CHECK(render->app_surface_owned[0]==0xFF10D020u &&
        render->app_surface_owned[1]==0xFFD02010u &&
        render->app_surface_owned[4]==0xFFC0B0A0u &&
        render->app_surface_owned[5]==0xFF2010D0u);
  CHECK(gml_render_begin_resident_owned_application(render,0.0,0.0,0));
  gml_render_set_pending_fill(render,0xFF000000u);
  gml_draw_background_tile(render,0,0,0,2,1,0,0,1,1,1,0,0,0xFFFFFFu,1.0);
  CHECK(render->resident_application_pass && fake.readbacks==0);
  CHECK(gml_render_resident_application_cpu_barrier(render));
  CHECK(render->app_surface_owned[0]==0xFF10D020u &&
        render->app_surface_owned[1]==0xFFD02010u &&
        render->app_surface_owned[4]==0xFF000000u);
  CHECK(gml_render_begin_resident_owned_application(render,0.0,0.0,0));
  gml_render_set_pending_fill(render,0xFF000000u);
  gml_draw_sprite_ext(render,0,0,4,0,-2,2,0,0xFFFFFFu,1.0);
  CHECK(render->resident_application_pass && fake.readbacks==0);
  CHECK(gml_render_resident_application_cpu_barrier(render));
  CHECK(render->app_surface_owned[0]==0xFF10D020u &&
        render->app_surface_owned[1]==0xFF10D020u &&
        render->app_surface_owned[2]==0xFFD02010u &&
        render->app_surface_owned[3]==0xFFD02010u &&
        render->app_surface_owned[8]==0xFFC0B0A0u &&
        render->app_surface_owned[10]==0xFF2010D0u &&
        render->app_surface_owned[15]==0xFF2010D0u);
  CHECK(gml_render_begin_resident_owned_application(render,0.0,0.0,0));
  gml_render_set_pending_fill(render,0xFF000000u);
  gml_draw_sprite_ext(render,0,0,1,0,-1,1,0,0xFFFFFFu,1.0);
  CHECK(!render->resident_application_pass &&
        render->app_surface_owned[0]==0xFFD02010u &&
        render->app_surface_owned[4]==0xFF2010D0u &&
        render->app_surface_owned[1]==0xFF000000u);
  CHECK(gml_render_begin_resident_owned_application(render,0.0,0.0,0));
  gml_render_set_pending_fill(render,0xFF000000u);
  gml_draw_background_tile(render,0,0,0,2,2,0,0,1,1,1,0,0,0xFFFFFFu,1.0);
  CHECK(render->resident_application_pass && fake.readbacks==0);
  CHECK(gml_render_resident_application_cpu_barrier(render));
  CHECK(render->app_surface_owned[0]==0xFF10D020u &&
        render->app_surface_owned[1]==0xFFD02010u &&
        render->app_surface_owned[4]==0xFFC0B0A0u &&
        render->app_surface_owned[5]==0xFF2010D0u);
  CHECK(gml_render_begin_resident_owned_application(render,0.0,0.0,0));
  gml_render_set_pending_fill(render,0xFF000000u);
  gml_draw_background_tiled_ext(render,0,-1,0,1,1,0xFFFFFFu,1.0,1,0);
  CHECK(render->resident_application_pass && fake.readbacks==0);
  CHECK(gml_render_resident_application_cpu_barrier(render));
  CHECK(render->app_surface_owned[0]==0xFF10D020u &&
        render->app_surface_owned[1]==0xFFD02010u &&
        render->app_surface_owned[2]==0xFF10D020u &&
        render->app_surface_owned[3]==0xFFD02010u &&
        render->app_surface_owned[4]==0xFFC0B0A0u &&
        render->app_surface_owned[8]==0xFF000000u);
  CHECK(gml_render_begin_resident_owned_application(render,0.0,0.0,0));
  gml_render_set_pending_fill(render,0xFF000000u);
  gml_draw_background(render,0,-1,0);
  CHECK(render->resident_application_pass && fake.readbacks==0);
  CHECK(gml_render_resident_application_cpu_barrier(render));
  CHECK(render->app_surface_owned[0]==0xFF10D020u &&
        render->app_surface_owned[4]==0xFFC0B0A0u &&
        render->app_surface_owned[1]==0xFF000000u);
  CHECK(gml_render_begin_resident_owned_application(render,0.0,0.0,0));
  gml_render_set_pending_fill(render,0xFF000000u);
  gml_draw_background(render,0,3,3);
  CHECK(render->resident_application_pass && fake.readbacks==0);
  CHECK(gml_render_resident_application_cpu_barrier(render));
  CHECK(render->app_surface_owned[15]==0xFFD02010u &&
        render->app_surface_owned[14]==0xFF000000u);
  {
    GmlWin win={0};
    uint8_t data[64]={0};
    data[4]=1;               /* one room background */
    data[8]=16;              /* background record address */
    data[16]=1;              /* enabled */
    data[16+12]=data[16+13]=data[16+14]=data[16+15]=0xFFu; /* x = -1 */
    data[16+20]=1;           /* horizontal tiling */
    win.data=data;
    win.size=sizeof data;
    render->win=&win;
    CHECK(gml_render_begin_resident_owned_application(render,0.0,0.0,0));
    gml_render_set_pending_fill(render,0xFF000000u);
    gml_draw_room_backgrounds(render,4,0);
    CHECK(render->resident_application_pass && fake.readbacks==0);
    CHECK(gml_render_resident_application_cpu_barrier(render));
    CHECK(render->app_surface_owned[0]==0xFF10D020u &&
          render->app_surface_owned[1]==0xFFD02010u &&
          render->app_surface_owned[2]==0xFF10D020u &&
          render->app_surface_owned[3]==0xFFD02010u &&
          render->app_surface_owned[4]==0xFFC0B0A0u &&
          render->app_surface_owned[5]==0xFF2010D0u &&
          render->app_surface_owned[8]==0xFF000000u);
    render->win=NULL;
  }
  /* The unchanged CPU part writer selects the right atlas column in sprite-local coordinates. */
  CHECK(gml_render_begin_owned_application(render,0.0,0.0,0));
  gml_render_set_pending_fill(render,0xFF000000u);
  gml_draw_sprite_part_ext(render,0,0,1,0,1,2,1,1,1,1,0xFFFFFFu,1.0);
  gml_render_flush_pending_fill(render);
  CHECK(render->app_surface_owned[5]==0xFF10D020u &&
        render->app_surface_owned[9]==0xFFC0B0A0u &&
        render->app_surface_owned[4]==0xFF000000u &&
        render->app_surface_owned[6]==0xFF000000u);
  {
    uint32_t cpu_part[16];
    GmlRenderApplicationWriteView part_view={0};
    memcpy(cpu_part,render->app_surface_owned,sizeof cpu_part);
    CHECK(gml_render_begin_resident_owned_application(render,0.0,0.0,0));
    gml_render_set_pending_fill(render,0xFF000000u);
    gml_draw_sprite_part_ext(render,0,0,1,0,1,2,1,1,1,1,0xFFFFFFu,1.0);
    CHECK(render->resident_application_pass && fake.readbacks==0);
    CHECK(gml_render_application_surface_owned_view(render,&part_view) &&
          !memcmp(part_view.pixels,cpu_part,sizeof cpu_part) && fake.readbacks==0);
    CHECK(gml_render_begin_resident_owned_application(render,0.0,0.0,0));
    gml_render_set_pending_fill(render,0xFF000000u);
    gml_draw_sprite_part_ext(render,0,0,1,0,1,2,1,1,1,1,0xFFFFFFu,0.5);
    CHECK(!render->resident_application_pass && fake.readbacks==0);
  }
  /* Establish the old CPU geometry before qualifying additional entrypoints. */
  uint32_t stretched_cpu[16],layer_cpu[16];
  GmlRenderApplicationWriteView variant_view={0};
  CHECK(gml_render_begin_owned_application(render,0.0,0.0,0));
  gml_render_set_pending_fill(render,0xFF000000u);
  gml_draw_sprite_stretched(render,0,0,0,0,4,4,0xFFFFFFu,1.0);
  gml_render_flush_pending_fill(render);
  CHECK(render->app_surface_owned[0]==0xFFD02010u &&
        render->app_surface_owned[1]==0xFFD02010u &&
        render->app_surface_owned[2]==0xFF10D020u &&
        render->app_surface_owned[8]==0xFF2010D0u &&
        render->app_surface_owned[15]==0xFFC0B0A0u);
  memcpy(stretched_cpu,render->app_surface_owned,sizeof stretched_cpu);
  CHECK(gml_render_begin_owned_application(render,0.0,0.0,0));
  gml_render_set_pending_fill(render,0xFF000000u);
  gml_draw_layer_background_sprite(render,0,0,1,1,1,1,0xFFFFFFu,1.0,0,0);
  gml_render_flush_pending_fill(render);
  CHECK(render->app_surface_owned[5]==0xFFD02010u &&
        render->app_surface_owned[6]==0xFF10D020u &&
        render->app_surface_owned[9]==0xFF2010D0u &&
        render->app_surface_owned[10]==0xFFC0B0A0u &&
        render->app_surface_owned[0]==0xFF000000u);
  memcpy(layer_cpu,render->app_surface_owned,sizeof layer_cpu);
  CHECK(gml_render_begin_resident_owned_application(render,0.0,0.0,0));
  gml_render_set_pending_fill(render,0xFF000000u);
  gml_draw_sprite_stretched(render,0,0,0,0,4,4,0xFFFFFFu,1.0);
  CHECK(render->resident_application_pass && fake.readbacks==0 &&
        gml_render_application_surface_owned_view(render,&variant_view) &&
        !memcmp(variant_view.pixels,stretched_cpu,sizeof stretched_cpu));
  CHECK(gml_render_begin_resident_owned_application(render,0.0,0.0,0));
  gml_render_set_pending_fill(render,0xFF000000u);
  gml_draw_layer_background_sprite(render,0,0,1,1,1,1,0xFFFFFFu,1.0,0,0);
  CHECK(render->resident_application_pass && fake.readbacks==0 &&
        gml_render_application_surface_owned_view(render,&variant_view) &&
        !memcmp(variant_view.pixels,layer_cpu,sizeof layer_cpu));
  /* The former tiled fallback was a dispatch limit, not a pixel expectation. */
  uint32_t repeated_layer_cpu[16];
  CHECK(gml_render_begin_owned_application(render,0.0,0.0,0));
  gml_render_set_pending_fill(render,0xFF000000u);
  gml_draw_layer_background_sprite(render,0,0,1,1,1,1,0xFFFFFFu,1.0,1,0);
  gml_render_flush_pending_fill(render);
  CHECK(render->app_surface_owned[4]==0xFF10D020u &&
        render->app_surface_owned[5]==0xFFD02010u &&
        render->app_surface_owned[6]==0xFF10D020u &&
        render->app_surface_owned[7]==0xFFD02010u);
  memcpy(repeated_layer_cpu,render->app_surface_owned,sizeof repeated_layer_cpu);
  CHECK(gml_render_begin_resident_owned_application(render,0.0,0.0,0));
  gml_render_set_pending_fill(render,0xFF000000u);
  gml_draw_layer_background_sprite(render,0,0,1,1,1,1,0xFFFFFFu,1.0,1,0);
  CHECK(render->resident_application_pass && fake.readbacks==0 &&
        gml_render_application_surface_owned_view(render,&variant_view) &&
        !memcmp(variant_view.pixels,repeated_layer_cpu,sizeof repeated_layer_cpu));
  CHECK(gml_render_begin_resident_owned_application(render,0.0,0.0,0));
  gml_render_set_pending_fill(render,0xFF000000u);
  gml_draw_sprite_stretched(render,0,0,0,0,4,4,0xFFFFFFu,0.5);
  CHECK(!render->resident_application_pass && fake.readbacks==0);
  gml_render_free(render);
  free(render);
  return 1;
}

static int atlas_scene_identity_and_retirement(void){
  GmlRender *first=(GmlRender*)calloc(1,sizeof *first);
  GmlRender *second=(GmlRender*)calloc(1,sizeof *second);
  GmlRenderResourceSource one={0},same={0},two={0},reloaded={0};
  FakeDevice fake={0};
  GmlRenderDeviceServices device=service(&fake);
  CHECK(first && second);
  first->atlas=(GmlAtlas*)calloc(1,sizeof *first->atlas);
  second->atlas=(GmlAtlas*)calloc(1,sizeof *second->atlas);
  CHECK(first->atlas && second->atlas);
  first->n_atlas=second->n_atlas=1;
  first->atlas[0].px=(uint8_t*)calloc(16u,1u);
  second->atlas[0].px=(uint8_t*)calloc(16u,1u);
  CHECK(first->atlas[0].px && second->atlas[0].px);
  first->atlas[0].w=second->atlas[0].w=2;
  first->atlas[0].h=second->atlas[0].h=2;
  gml_render_bind_device(first,&device);
  gml_render_bind_device(second,&device);
  CHECK(gml_render_atlas_scene_source_view(first,0,&one) &&
        gml_render_atlas_scene_source_view(first,0,&same) &&
        gml_render_atlas_scene_source_view(second,0,&two));
  CHECK(one.image.resource_class==GML_RENDER_RESOURCE_ATLAS &&
        one.image.identity==0u && one.image.content_version==1u &&
        one.image.incarnation==same.image.incarnation &&
        one.image.incarnation!=two.image.incarnation &&
        one.borrowed_immutable && one.pixel_format==GML_RENDER_PIXEL_RGBA8888 &&
        one.width==2u && one.height==2u && one.stride_bytes==8u &&
        one.byte_capacity==16u && one.bytes==first->atlas[0].px);
  gml_render_free(first);
  CHECK(fake.retires==1);
  CHECK(gml_render_init(first,NULL)==0);
  CHECK(first->n_atlas>0 && first->atlas && first->atlas[0].px);
  /* Reinitialization owns a fresh page at the same logical index. Keep its allocated default
   * font page intact: replacing it would leak the generation we are testing. */
  first->atlas[0].px[0]=0xA5u;
  gml_render_bind_device(first,&device);
  CHECK(gml_render_atlas_scene_source_view(first,0,&reloaded) &&
        reloaded.image.identity==one.image.identity &&
        reloaded.image.incarnation!=one.image.incarnation &&
        reloaded.image.incarnation!=two.image.incarnation &&
        reloaded.image.content_version==1u &&
        reloaded.bytes==first->atlas[0].px &&
        ((const uint8_t*)reloaded.bytes)[0]==0xA5u);
  gml_render_free(first);
  gml_render_free(second);
  CHECK(fake.retires==3);
  free(first);
  free(second);
  return 1;
}

static int immutable_font_scene_preparation(void){
  GmlRender *render=(GmlRender*)calloc(1,sizeof *render);
  FakeDevice fake={0};
  GmlRenderDeviceServices device=service(&fake);
  GmlRenderResourceSource source={0};
  const uint8_t encoded[]={ 'f','i','o','q',1,0,1,0,0,0,0,0,0xFF,17,39,61,255 };
  CHECK(render);
  render->n_atlas=5;render->n_fonts=5;
  render->atlas=(GmlAtlas*)calloc(5,sizeof *render->atlas);
  CHECK(render->atlas);
  for(int index=0;index<5;index++){
    render->atlas[index].external_blob=(uint8_t*)malloc(sizeof encoded);
    CHECK(render->atlas[index].external_blob);
    memcpy(render->atlas[index].external_blob,encoded,sizeof encoded);
    render->atlas[index].external_size=sizeof encoded;
    render->atlas[index].w=render->atlas[index].h=1;
  }
  for(int font=0;font<5;font++){
    render->fonts[font].real=1;
    render->fonts[font].atlas=font?font-1:0;
  }
  render->fonts[2].runtime_owned=1;
  render->atlas[2].runtime_font_page=1;
  /* Metadata alone refuses excessive speculative work before invoking the decoder. */
  render->atlas[3].w=4096;render->atlas[3].h=4097;
  render->atlas[3].external_blob[4]=0;render->atlas[3].external_blob[5]=16;
  render->atlas[3].external_blob[6]=1;render->atlas[3].external_blob[7]=16;
  fake.max_texture_extent=8192;
  gml_render_prepare_scene_fonts(render);
  CHECK(!render->atlas[0].px && !fake.uploads);
  gml_render_bind_device(render,&device);
  gml_render_prepare_scene_fonts(render);
  CHECK(!render->atlas[0].px && !fake.uploads);
  device.observe_on_demand=fake_observe_on_demand;
  gml_render_bind_device(render,&device);
  fake.forbid_calls=1;
  gml_render_prepare_scene_fonts(render);
  CHECK(!render->atlas[0].px && !fake.uploads && !fake.forbidden_calls);
  fake.forbid_calls=0;fake.on_demand=1;
  gml_render_prepare_scene_fonts(render);
  CHECK(fake.uploads==2 && !fake.submits && !fake.readbacks && !fake.retires);
  CHECK(render->atlas[0].px && render->atlas[0].decode_attempted &&
        !memcmp(render->atlas[0].px,encoded+13,4));
  for(int index=1;index<4;index++)
    CHECK(!render->atlas[index].px && !render->atlas[index].decode_attempted);
  CHECK(render->atlas[4].px && render->atlas[4].decode_attempted &&
        !memcmp(render->atlas[4].px,encoded+13,4));
  /* The unreferenced immutable page spends the remaining budget only after fonts, while
   * runtime-owned, runtime-font and over-budget pages remain undecoded. */
  CHECK(gml_render_atlas_scene_source_view(render,4,&source));
  CHECK(fake.last_upload.image.identity==4u);
  CHECK(fake.last_upload.image.resource_class==source.image.resource_class &&
        fake.last_upload.image.identity==source.image.identity &&
        fake.last_upload.image.incarnation==source.image.incarnation &&
        fake.last_upload.image.content_version==source.image.content_version &&
        fake.last_upload.bytes==source.bytes && fake.last_upload.byte_capacity==4 &&
        fake.last_upload.storage_width==1 && fake.last_upload.storage_height==1 &&
        fake.last_upload.stride_bytes==4 && fake.last_upload.region.x==0 &&
        fake.last_upload.region.y==0 && fake.last_upload.region.width==1 &&
        fake.last_upload.region.height==1 &&
        fake.last_upload.pixel_format==GML_RENDER_PIXEL_RGBA8888);
  fake.fail_upload=1;
  gml_render_prepare_scene_fonts(render);
  fake.fail_upload=0;
  gml_render_prepare_scene_fonts(render);
  CHECK(fake.uploads==6 && fake.last_upload.image.incarnation==source.image.incarnation);
  fake.on_demand=0;fake.forbid_calls=1;
  gml_render_prepare_scene_fonts(render);
  CHECK(fake.uploads==6 && !fake.forbidden_calls);
  fake.forbid_calls=0;
  gml_render_free(render);free(render);
  CHECK(fake.retires==2);
  return 1;
}

static int immutable_sprite_font_scene_preparation(void){
  GmlRender *render=(GmlRender*)calloc(1,sizeof *render);
  FakeDevice fake={.on_demand=1};
  GmlRenderDeviceServices device=service(&fake);
  const uint8_t encoded[]={ 'f','i','o','q',1,0,1,0,0,0,0,0,0xFF,17,39,61,255 };
  CHECK(render);
  render->n_atlas=2;render->n_fonts=3;render->n_spr=1;render->n_tpag=2;
  render->atlas=(GmlAtlas*)calloc(2,sizeof *render->atlas);
  render->spr=(GmlSprite*)calloc(1,sizeof *render->spr);
  render->tpag=(GmlTpag*)calloc(2,sizeof *render->tpag);
  CHECK(render->atlas && render->spr && render->tpag);
  for(int index=0;index<2;index++){
    render->atlas[index].external_blob=(uint8_t*)malloc(sizeof encoded);
    CHECK(render->atlas[index].external_blob);
    memcpy(render->atlas[index].external_blob,encoded,sizeof encoded);
    render->atlas[index].external_size=sizeof encoded;
    render->atlas[index].w=render->atlas[index].h=1;
    render->tpag[index].atlas=index;
  }
  render->atlas[1].runtime_font_page=1;
  render->spr[0].n_frames=3;
  render->spr[0].frame=(int*)malloc(3*sizeof(int));
  CHECK(render->spr[0].frame);
  render->spr[0].frame[0]=0;render->spr[0].frame[1]=1;render->spr[0].frame[2]=-1;
  render->fonts[0].sprite=render->fonts[1].sprite=0;
  render->fonts[2].sprite=-1;
  device.observe_on_demand=fake_observe_on_demand;
  gml_render_bind_device(render,&device);
  gml_render_prepare_scene_fonts(render);
  CHECK(fake.uploads==1 && render->atlas[0].decode_attempted &&
        !render->atlas[1].decode_attempted && !fake.submits && !fake.readbacks);
  gml_render_free(render);free(render);
  CHECK(fake.retires==1);
  return 1;
}

static int immutable_room_scene_preparation(void){
  GmlRender *render=(GmlRender*)calloc(1,sizeof *render);
  FakeDevice fake={0};
  GmlRenderDeviceServices device=service(&fake);
  CHECK(render);
  render->n_atlas=2;render->n_spr=1;render->n_bg=1;render->n_tpag=3;
  render->atlas=(GmlAtlas*)calloc(2,sizeof *render->atlas);
  render->spr=(GmlSprite*)calloc(1,sizeof *render->spr);
  render->bg=(GmlBg*)calloc(1,sizeof *render->bg);
  render->tpag=(GmlTpag*)calloc(3,sizeof *render->tpag);
  CHECK(render->atlas && render->spr && render->bg && render->tpag);
  render->atlas[0].px=(uint8_t*)calloc(16,1);
  CHECK(render->atlas[0].px);
  render->atlas[0].w=render->atlas[0].h=2;
  render->spr[0].n_frames=3;
  render->spr[0].frame=(int*)calloc(3,sizeof(int));
  CHECK(render->spr[0].frame);
  for(int frame=0;frame<3;frame++) render->spr[0].frame[frame]=frame;
  render->tpag[0].atlas=0;render->tpag[1].atlas=1;render->tpag[2].atlas=-1;
  render->bg[0].tpag=0;
  device.observe_on_demand=fake_observe_on_demand;
  gml_render_bind_device(render,&device);
  gml_render_prepare_scene_sprite(render,0);gml_render_prepare_scene_bg(render,0);
  CHECK(!fake.uploads);
  fake.on_demand=1;
  fake.max_texture_extent=1;
  gml_render_prepare_scene_sprite(render,0);gml_render_prepare_scene_bg(render,0);
  CHECK(!fake.uploads && !render->atlas[1].decode_attempted);
  fake.max_texture_extent=2;
  gml_render_prepare_scene_sprite(render,0);gml_render_prepare_scene_bg(render,0);
  CHECK(fake.uploads==2 && fake.last_upload.image.identity==0 &&
        !fake.submits && !fake.readbacks && !render->atlas[1].decode_attempted);
  render->atlas[0].runtime_font_page=1;
  gml_render_prepare_scene_sprite(render,0);gml_render_prepare_scene_bg(render,0);
  CHECK(fake.uploads==2);
  render->atlas[0].runtime_font_page=0;
  gml_render_prepare_scene_sprite(render,-1);gml_render_prepare_scene_sprite(render,1);
  gml_render_prepare_scene_bg(render,-1);gml_render_prepare_scene_bg(render,1);
  CHECK(fake.uploads==2);
  gml_render_free(render);free(render);
  CHECK(fake.retires==1);
  return 1;
}

static int runtime_font_page_scene_retirement(void){
  GmlRender *render=(GmlRender*)calloc(1,sizeof *render);
  GmlRenderResourceSource first={0},second={0};
  FakeDevice fake={0};
  GmlRenderDeviceServices device=service(&fake);
  CHECK(render);
  render->atlas=(GmlAtlas*)calloc(1,sizeof *render->atlas);
  CHECK(render->atlas);
  render->n_atlas=render->n_fonts=1;
  render->fonts[0].real=render->fonts[0].runtime_owned=1;
  render->fonts[0].atlas=0;
  render->atlas[0].runtime_font_page=1;
  render->atlas[0].w=render->atlas[0].h=2;
  render->atlas[0].px=(uint8_t*)calloc(16u,1u);
  CHECK(render->atlas[0].px);
  gml_render_bind_device(render,&device);
  CHECK(gml_render_atlas_scene_source_view(render,0,&first) &&
        !first.borrowed_immutable && first.image.content_version==1u);
  gml_font_delete(render,0);
  CHECK(fake.retires==1 && !render->atlas[0].px);
  render->fonts[0].real=render->fonts[0].runtime_owned=1;
  render->fonts[0].atlas=0;
  render->atlas[0].w=render->atlas[0].h=2;
  render->atlas[0].px=(uint8_t*)calloc(16u,1u);
  CHECK(render->atlas[0].px);
  CHECK(gml_render_atlas_scene_source_view(render,0,&second) &&
        !second.borrowed_immutable &&
        second.image.incarnation!=first.image.incarnation &&
        second.image.content_version==1u);
  gml_render_free(render);
  CHECK(fake.retires==2);
  free(render);
  return 1;
}

static int mutable_scene_identity_across_renderers(void){
  GmlRender *first=(GmlRender*)calloc(1,sizeof *first);
  GmlRender *second=(GmlRender*)calloc(1,sizeof *second);
  GmlRenderImageRef first_app,second_app,first_surface,second_surface;
  FakeDevice fake={0};
  GmlRenderDeviceServices device=service(&fake);
  int first_id,second_id;
  CHECK(first && second);
  CHECK(gml_render_application_surface_ensure_owned(first,4,4) &&
        gml_render_application_surface_ensure_owned(second,4,4));
  first->next_surface_id=second->next_surface_id=1;
  first_id=gml_surface_create(first,4,4);
  second_id=gml_surface_create(second,4,4);
  CHECK(first_id==1 && second_id==1);
  gml_render_bind_device(first,&device);
  gml_render_bind_device(second,&device);
  CHECK(gml_render_application_resource_ensure(first) &&
        gml_render_application_resource_ensure(second) &&
        gml_render_surface_resource_ensure(first,first_id) &&
        gml_render_surface_resource_ensure(second,second_id));
  first_app=gml_render_resource_image_ref(first->app_surface_resource);
  second_app=gml_render_resource_image_ref(second->app_surface_resource);
  first_surface=gml_render_resource_image_ref(first->surface[0].resource);
  second_surface=gml_render_resource_image_ref(second->surface[0].resource);
  CHECK(first_app.resource_class==GML_RENDER_RESOURCE_APPLICATION &&
        first_app.identity==second_app.identity &&
        first_app.incarnation!=second_app.incarnation);
  CHECK(first_surface.resource_class==GML_RENDER_RESOURCE_SURFACE &&
        first_surface.identity==second_surface.identity &&
        first_surface.incarnation!=second_surface.incarnation);
  gml_render_free(first);
  gml_render_free(second);
  free(first);
  free(second);
  return 1;
}

static int surface_observer_materializes_suspended_fill(void){
  GmlRender *render=(GmlRender*)calloc(1,sizeof *render);
  GmlRenderResource *resource;
  GmlRenderImageRef before,after;
  const uint32_t *pixels;
  int id,w=0,h=0;
  CHECK(render);
  render->next_surface_id=1;
  CHECK(gml_render_application_surface_ensure_owned(render,4,4));
  resource=gml_render_application_resource_ensure(render);
  CHECK(resource);
  gml_render_begin(render,render->app_surface_owned,4,4,0.0,0.0);
  id=gml_surface_create(render,2,2);
  CHECK(id>0);
  gml_render_set_pending_fill(render,0xFF315179u);
  before=gml_render_resource_image_ref(resource);
  CHECK(gml_surface_set_target(render,id));
  CHECK(render->target_stack[0].pending_fill);
  pixels=gml_surface_pixels_read(render,0,&w,&h);
  after=gml_render_resource_image_ref(resource);
  CHECK(pixels && w==4 && h==4 && pixels[0]==0xFF315179u &&
        pixels[15]==0xFF315179u && !render->target_stack[0].pending_fill &&
        after.content_version==before.content_version+1u);
  CHECK(gml_surface_pixels_read(render,0,&w,&h)==pixels &&
        gml_render_resource_image_ref(resource).content_version==after.content_version);
  gml_render_free(render);
  free(render);
  return 1;
}

static int resident_content_surface_program_boundary(void){
  GmlRender *render=(GmlRender*)calloc(1,sizeof *render);
  FakeDevice fake={0};
  GmlRenderDeviceServices device=service(&fake);
  GmlRenderJournalMetrics metrics={0};
  GmlPlanShader snapshot={0};
  const uint32_t *pixels=NULL;
  int id,aux_id,large_id,old_submits,old_uploads,old_retires,old_readbacks;
  CHECK(render && gml_render_application_surface_ensure_owned(render,4,4));
  render->next_surface_id=1;
  render->color_write_mask=0x0Fu;
  render->blend_equation=render->blend_equation_alpha=1;
  render->shader_pal=(struct GmlShaderPal*)calloc(1,sizeof *render->shader_pal);
  CHECK(render->shader_pal);
  render->n_shader_pal=1;
  render->shader_pal[0].source_vertex_es=
    "attribute vec3 in_Position; void main(){ gl_Position=vec4(in_Position,1.0); }";
  render->shader_pal[0].source_fragment_es=
    "uniform float u_value; void main(){ gl_FragColor=vec4(u_value,0.5,0.25,0.5); }";
  render->shader_pal[0].generic_uniform_count=1;
  snprintf(render->shader_pal[0].generic_uniform[0].name,
           sizeof render->shader_pal[0].generic_uniform[0].name,"u_value");
  render->shader_pal[0].generic_uniform[0].count=1;
  render->shader_pal[0].generic_uniform[0].value[0]=0.375f;
  render->shader_device_present=1;
  render->shader_device_expected=1;
  id=gml_surface_create(render,2,2);
  CHECK(id>0);
  for(int index=0;index<4;index++) render->surface[id-1].px[index]=0xFF102030u;
  fake.readback_colour=0x80776655u;
  gml_render_bind_device(render,&device);
  CHECK(gml_render_begin_resident_owned_application(render,0.0,0.0,0));
  render->active_shader=0;
  gml_render_set_pending_fill(render,0xFF010203u);
  gml_draw_surface_stretched(render,id,0.0,0.0,4.0,4.0,0xFFFFFFu,1.0);
  CHECK(gml_render_application_surface_owned_read(render,NULL));
  gml_render_resource_journal_metrics(render->app_surface_resource,&metrics);
  CHECK(render->resident_application_pass && fake.last_opcode==GML_RENDER_OP_CONTENT_SHADER_DRAW &&
        fake.last_shader_identity==0u && fake.last_shader_uniform_count==1u &&
        fake.last_shader_value==0.375f && fake.readbacks==1 &&
        metrics.authority==GML_RENDER_RESOURCE_BOTH_CURRENT &&
        metrics.device_checkpoints==1u && render->shaded_draws==1u &&
        render->app_surface_owned[5]==fake.readback_colour);
  gml_render_resource_context_lost(render->app_surface_resource);
  CHECK(gml_render_resource_acquire_cpu_read(render->app_surface_resource,&pixels) &&
        pixels[5]==fake.readback_colour && fake.readbacks==1);
  /* Unbound samplers, fractional placement and disabled content policy remain CPU work. */
  render->shader_pal[0].generic_sampler_count=1;
  snprintf(render->shader_pal[0].generic_sampler[0].name,
           sizeof render->shader_pal[0].generic_sampler[0].name,"u_aux");
  CHECK(gml_render_begin_resident_owned_application(render,0.0,0.0,0));
  render->active_shader=0;
  gml_render_set_pending_fill(render,0xFF010203u);
  old_submits=fake.submits;
  gml_draw_surface_stretched(render,id,0.0,0.0,4.0,4.0,0xFFFFFFu,1.0);
  CHECK(!render->resident_application_pass && fake.submits==old_submits+1 &&
        fake.last_opcode==GML_RENDER_OP_CLEAR && fake.readbacks==1);
  aux_id=gml_surface_create(render,2,2);
  CHECK(aux_id>0);
  for(int index=0;index<4;index++) render->surface[aux_id-1].px[index]=0xFF775533u;
  render->shader_pal[0].generic_sampler[0].texture=
    (int)(GML_TEX_SURF_TAG|(unsigned)aux_id);
  CHECK(gml_render_begin_resident_owned_application(render,0.0,0.0,0));
  render->active_shader=0;
  gml_render_set_pending_fill(render,0xFF010203u);
  old_submits=fake.submits;
  old_uploads=fake.uploads;
  old_retires=fake.retires;
  gml_draw_surface_stretched(render,id,0.0,0.0,4.0,4.0,0xFFFFFFu,1.0);
  CHECK(gml_render_application_surface_owned_read(render,NULL));
  gml_render_resource_journal_metrics(render->app_surface_resource,&metrics);
  CHECK(render->resident_application_pass && fake.submits==old_submits+2 &&
        fake.last_opcode==GML_RENDER_OP_CONTENT_SHADER_DRAW &&
        fake.last_source_count==2u && fake.last_shader_sampler_count==1u &&
        fake.uploads>=old_uploads+1 && fake.retires==old_retires+1 &&
        metrics.authority==GML_RENDER_RESOURCE_BOTH_CURRENT &&
        metrics.device_checkpoints==2u && render->shaded_draws==2u);
  render->shader_pal[0].generic_sampler_count=GML_PLAN_MAX_SAMPLERS+1;
  CHECK(!gml_render_shader_plan_snapshot(render,0,0u,NULL,&snapshot));
  render->shader_pal[0].generic_sampler_count=1;
  render->shader_pal[0].generic_uniform_count=GML_PLAN_MAX_UNIFORMS+1;
  CHECK(!gml_render_shader_plan_snapshot(render,0,0u,NULL,&snapshot));
  render->shader_pal[0].generic_uniform_count=1;
  large_id=gml_surface_create(render,8,8);
  CHECK(large_id>0);
  render->shader_pal[0].generic_sampler[0].texture=
    (int)(GML_TEX_SURF_TAG|(unsigned)large_id);
  fake.max_texture_extent=4u;
  CHECK(gml_render_begin_resident_owned_application(render,0.0,0.0,0));
  render->active_shader=0;
  gml_render_set_pending_fill(render,0xFF010203u);
  old_submits=fake.shader_submits;
  gml_draw_surface_stretched(render,id,0.0,0.0,4.0,4.0,0xFFFFFFu,1.0);
  CHECK(fake.shader_submits==old_submits && render->shaded_draws==2u);
  fake.max_texture_extent=0u;
  /* A sampled target cannot bind its own pixels as an auxiliary image. */
  render->shader_pal[0].generic_sampler[0].texture=
    (int)(GML_TEX_SURF_TAG|(unsigned)id);
  CHECK(gml_render_begin_resident_owned_application(render,0.0,0.0,0));
  CHECK(gml_surface_set_target(render,id));
  render->active_shader=0;
  old_submits=fake.submits;
  gml_draw_surface_stretched(render,aux_id,0.0,0.0,4.0,4.0,0xFFFFFFu,1.0);
  CHECK(fake.submits==old_submits && render->shaded_draws==2u);
  gml_surface_reset_target(render);
  render->shader_pal[0].generic_sampler_count=0;
  CHECK(gml_render_begin_resident_owned_application(render,0.0,0.0,0));
  render->active_shader=0;
  gml_render_set_pending_fill(render,0xFF010203u);
  old_submits=fake.submits;
  old_readbacks=fake.readbacks;
  gml_draw_surface_stretched(render,id,1.5,1.0,2.0,2.0,0xFFFFFFu,1.0);
  CHECK(!render->resident_application_pass && fake.submits==old_submits+1 &&
        fake.last_opcode==GML_RENDER_OP_CLEAR && fake.readbacks==old_readbacks);
  render->shader_device_present=0;
  CHECK(gml_render_begin_resident_owned_application(render,0.0,0.0,0));
  render->active_shader=0;
  gml_render_set_pending_fill(render,0xFF010203u);
  old_submits=fake.submits;
  old_readbacks=fake.readbacks;
  gml_draw_surface_stretched(render,id,0.0,0.0,4.0,4.0,0xFFFFFFu,1.0);
  CHECK(!render->resident_application_pass && fake.submits==old_submits+1 &&
        fake.last_opcode==GML_RENDER_OP_CLEAR && fake.readbacks==old_readbacks);
  render->shader_device_present=1;
  CHECK(gml_render_shader_is_compiled(render,0));
  CHECK(gml_render_begin_resident_owned_application(render,0.0,0.0,0));
  render->active_shader=0;
  gml_render_set_pending_fill(render,0xFF010203u);
  gml_render_flush_pending_fill(render);
  fake.status=GML_RENDER_EXEC_RECOVERABLE_ERROR;
  old_submits=fake.submits;
  gml_draw_surface_stretched(render,id,0.0,0.0,4.0,4.0,0xFFFFFFu,1.0);
  CHECK(fake.submits==old_submits+1 && !render->shader_pal[0].gpu_failed &&
        gml_render_shader_is_compiled(render,0));
  fake.status=0;
  CHECK(gml_render_begin_resident_owned_application(render,0.0,0.0,0));
  render->active_shader=0;
  gml_render_set_pending_fill(render,0xFF010203u);
  gml_render_flush_pending_fill(render);
  fake.status=GML_RENDER_EXEC_RECOVERABLE_ERROR;
  fake.shader_failure=1;
  old_submits=fake.submits;
  gml_draw_surface_stretched(render,id,0.0,0.0,4.0,4.0,0xFFFFFFu,1.0);
  CHECK(fake.submits==old_submits+1 && render->shader_pal[0].gpu_failed &&
        !gml_render_shader_is_compiled(render,0));
  gml_render_free(render);
  free(render);
  return 1;
}

static int resident_content_surface_full_part_boundary(void){
  GmlRender *render=(GmlRender*)calloc(1,sizeof *render);
  FakeDevice fake={0};
  GmlRenderDeviceServices device=service(&fake);
  uint32_t screen[64]={0};
  int source,shader_submits;
  CHECK(render && gml_render_application_surface_ensure_owned(render,4,4));
  render->next_surface_id=1;
  render->color_write_mask=0x0Fu;
  render->blend_equation=render->blend_equation_alpha=1;
  render->shader_pal=(struct GmlShaderPal*)calloc(1,sizeof *render->shader_pal);
  CHECK(render->shader_pal);
  render->n_shader_pal=1;
  render->shader_pal[0].source_vertex_es=
    "attribute vec3 in_Position; void main(){ gl_Position=vec4(in_Position,1.0); }";
  render->shader_pal[0].source_fragment_es=
    "void main(){ gl_FragColor=vec4(0.5,0.25,0.75,1.0); }";
  render->shader_device_present=render->shader_device_expected=1;
  source=gml_surface_create(render,2,2);
  CHECK(source>0);
  for(int index=0;index<4;index++) render->surface[source-1].px[index]=0xFF102030u;
  gml_render_application_surface_publish(render);
  fake.readback_colour=0xFF5879A4u;
  gml_render_bind_device(render,&device);
  CHECK(gml_render_begin_resident_base(render,screen,8,8,0.0,0.0));
  render->active_shader=0;
  gml_render_set_pending_fill(render,0xFF010203u);
  gml_draw_surface_part_ext(render,source,0.0,0.0,2.0,2.0,
                            0.0,0.0,4.0,4.0,0xFFFFFFu,1.0);
  CHECK(render->resident_application_pass && fake.shader_submits==1 &&
        fake.last_opcode==GML_RENDER_OP_CONTENT_SHADER_DRAW);
  CHECK(gml_render_begin_resident_base(render,screen,8,8,0.0,0.0));
  render->active_shader=0;
  gml_render_set_pending_fill(render,0xFF010203u);
  gml_draw_surface_part_ext(render,0,0.0,0.0,4.0,4.0,
                            0.0,0.0,2.0,2.0,0xFFFFFFu,1.0);
  CHECK(render->resident_application_pass && fake.shader_submits==2 &&
        fake.last_opcode==GML_RENDER_OP_CONTENT_SHADER_DRAW);
  CHECK(gml_render_begin_resident_base(render,screen,8,8,0.0,0.0));
  render->active_shader=0;
  shader_submits=fake.shader_submits;
  gml_draw_surface_part_ext(render,source,1.0,0.0,1.0,2.0,
                            0.0,0.0,8.0,4.0,0xFFFFFFu,1.0);
  CHECK(!render->resident_application_pass && fake.shader_submits==shader_submits);
  CHECK(gml_render_begin_resident_base(render,screen,8,8,0.0,0.0));
  render->active_shader=0;
  gml_draw_surface_part_ext(render,source,0.0,0.0,2.0,2.0,
                            0.0,0.0,2.0,4.0,0xFFFFFFu,1.0);
  CHECK(render->resident_application_pass && fake.shader_submits==shader_submits+1 &&
        fake.last_target_region.width==4u && fake.last_target_region.height==8u);
  CHECK(gml_render_begin_resident_base(render,screen,8,8,0.0,0.0));
  render->active_shader=0;
  shader_submits=fake.shader_submits;
  gml_draw_surface_part_ext(render,source,0.0,0.0,2.0,2.0,
                            0.0,0.0,2.25,4.0,0xFFFFFFu,1.0);
  CHECK(!render->resident_application_pass && fake.shader_submits==shader_submits);
  gml_render_free(render);
  free(render);
  return 1;
}

static int stale_auxiliary_surface_shader_fallback(void){
  GmlRender *render=(GmlRender*)calloc(1,sizeof *render);
  FakeDevice fake={0};
  GmlRenderDeviceServices device=service(&fake);
  GmlRenderJournalMetrics metrics={0};
  GmlRenderApplicationReadView view={0};
  int source,aux,shader_submits;
  CHECK(render && gml_render_application_surface_ensure_owned(render,4,4));
  render->next_surface_id=1;
  render->color_write_mask=0x0Fu;
  render->blend_equation=render->blend_equation_alpha=1;
  render->shader_pal=(struct GmlShaderPal*)calloc(1,sizeof *render->shader_pal);
  CHECK(render->shader_pal);
  render->n_shader_pal=1;
  render->shader_pal[0].source_vertex_es=
    "attribute vec3 in_Position; void main(){ gl_Position=vec4(in_Position,1.0); }";
  render->shader_pal[0].source_fragment_es=
    "void main(){ gl_FragColor=vec4(0.5,0.25,0.75,1.0); }";
  render->shader_device_present=render->shader_device_expected=1;
  source=gml_surface_create(render,2,2);
  aux=gml_surface_create(render,4,4);
  CHECK(source>0 && aux>0);
  for(int index=0;index<4;index++) render->surface[source-1].px[index]=0xFF102030u;
  fake.readback_colour=0xFF8040C0u;
  gml_render_bind_device(render,&device);
  CHECK(gml_render_begin_resident_owned_application(render,0.0,0.0,0) &&
        gml_surface_set_target(render,aux));
  render->active_shader=0;
  gml_draw_surface_stretched(render,source,0.0,0.0,4.0,4.0,0xFFFFFFu,1.0);
  gml_render_resource_journal_metrics(render->surface[aux-1].resource,&metrics);
  CHECK(fake.shader_submits==1 && fake.readbacks==0 &&
        metrics.authority==GML_RENDER_RESOURCE_GPU_PENDING_CHECKPOINT);
  gml_surface_reset_target(render);
  render->shader_pal[0].generic_sampler_count=1;
  snprintf(render->shader_pal[0].generic_sampler[0].name,
           sizeof render->shader_pal[0].generic_sampler[0].name,"u_aux");
  render->shader_pal[0].generic_sampler[0].texture=
    (int)(GML_TEX_SURF_TAG|(unsigned)aux);
  gml_render_set_pending_fill(render,0xFF010203u);
  shader_submits=fake.shader_submits;
  gml_draw_surface_stretched(render,source,0.0,0.0,4.0,4.0,0xFFFFFFu,1.0);
  CHECK(fake.shader_submits==shader_submits && render->shaded_draws==1u &&
        gml_render_application_surface_owned_read(render,&view) &&
        view.pixels[5]==0xFF010203u);
  CHECK(gml_render_checkpoint_pending_resources(render));
  gml_render_resource_journal_metrics(render->surface[aux-1].resource,&metrics);
  CHECK(metrics.authority==GML_RENDER_RESOURCE_BOTH_CURRENT &&
        render->surface[aux-1].px[5]==fake.readback_colour);
  gml_render_free(render);
  free(render);
  return 1;
}

static int resident_checkpoint_span_seals_at_observer(void){
  for(int target_kind=0;target_kind<3;target_kind++){
    GmlRender *render=(GmlRender*)calloc(1,sizeof *render);
    FakeDevice fake={0};
    GmlRenderDeviceServices device=service(&fake);
    GmlRenderResource *target;
    GmlRenderJournalMetrics metrics={0};
    const uint32_t *observed=NULL;
    uint32_t screen[16]={0};
    int source,destination=0;
    CHECK(render && gml_render_application_surface_ensure_owned(render,4,4));
    render->next_surface_id=1;
    render->color_write_mask=0x0Fu;
    render->blend_equation=render->blend_equation_alpha=1;
    render->shader_pal=(struct GmlShaderPal*)calloc(1,sizeof *render->shader_pal);
    CHECK(render->shader_pal);
    render->n_shader_pal=1;
    render->shader_pal[0].source_vertex_es=
      "attribute vec3 in_Position; void main(){ gl_Position=vec4(in_Position,1.0); }";
    render->shader_pal[0].source_fragment_es=
      "void main(){ gl_FragColor=vec4(0.5,0.25,0.75,1.0); }";
    render->shader_device_present=render->shader_device_expected=1;
    source=gml_surface_create(render,2,2);
    CHECK(source>0);
    for(int index=0;index<4;index++) render->surface[source-1].px[index]=0xFF102030u;
    fake.readback_colour=0xFF8040C0u;
    gml_render_bind_device(render,&device);
    if(target_kind==1){
      CHECK(gml_render_begin_resident_base(render,screen,4,4,0.0,0.0));
      target=gml_render_scene_target_resource_ensure(render);
    } else {
      CHECK(gml_render_begin_resident_owned_application(render,0.0,0.0,0));
      if(target_kind==2){
        destination=gml_surface_create(render,4,4);
        CHECK(destination>0 && gml_surface_set_target(render,destination));
        target=gml_render_surface_resource_ensure(render,destination);
      } else target=gml_render_scene_target_resource_ensure(render);
    }
    CHECK(target);
    render->active_shader=0;
    gml_draw_surface_stretched(render,source,0.0,0.0,4.0,4.0,0xFFFFFFu,1.0);
    CHECK(fake.shader_submits==1 && render->resident_application_pass && fake.readbacks==0);
    gml_draw_surface_stretched(render,source,0.0,0.0,4.0,4.0,0xFFFFFFu,1.0);
    gml_render_resource_journal_metrics(target,&metrics);
    CHECK(fake.shader_submits==2 && render->resident_application_pass && fake.readbacks==0 &&
          metrics.authority==GML_RENDER_RESOURCE_GPU_PENDING_CHECKPOINT &&
          metrics.device_checkpoints==0u && metrics.gpu_version==metrics.logical_version);
    /* The first real CPU observer seals the complete ordered span, not each draw. The fake
     * verifies authority and call counts only; real shader-produced pixels are separate. */
    CHECK(gml_render_resource_acquire_cpu_read(target,&observed) && observed &&
          observed[5]==fake.readback_colour && fake.readbacks==1);
    CHECK(gml_render_scene_flush(render) && fake.readbacks==1);
    gml_render_resource_journal_metrics(target,&metrics);
    CHECK(metrics.authority==GML_RENDER_RESOURCE_BOTH_CURRENT &&
          metrics.device_checkpoints==1u && metrics.cpu_version==metrics.logical_version);
    gml_render_bind_device(render,NULL);
    fake.forbid_calls=1;
    CHECK(gml_render_resource_acquire_cpu_read(target,&observed) &&
          observed[5]==fake.readback_colour && fake.forbidden_calls==0);
    gml_render_free(render);
    free(render);
  }
  return 1;
}

static int public_content_surface_program_chain(void){
  GmlRender *render=(GmlRender*)calloc(1,sizeof *render);
  GmlRender *restored=(GmlRender*)calloc(1,sizeof *restored);
  FakeDevice fake={0};
  GmlRenderDeviceServices device=service(&fake);
  GmlRenderJournalMetrics metrics={0};
  const uint32_t *pixels=NULL;
  uint8_t *state_bytes=NULL;
  size_t state_size,written=0,used=0;
  int source,intermediate,abandoned,readbacks_before_abandon;
  CHECK(render && restored && gml_render_application_surface_ensure_owned(render,4,4));
  render->next_surface_id=1;
  render->color_write_mask=0x0Fu;
  render->blend_equation=render->blend_equation_alpha=1;
  render->shader_pal=(struct GmlShaderPal*)calloc(1,sizeof *render->shader_pal);
  CHECK(render->shader_pal);
  render->n_shader_pal=1;
  render->shader_pal[0].source_vertex_es=
    "attribute vec3 in_Position; void main(){ gl_Position=vec4(in_Position,1.0); }";
  render->shader_pal[0].source_fragment_es=
    "void main(){ gl_FragColor=vec4(0.5,0.25,0.75,1.0); }";
  render->shader_device_present=render->shader_device_expected=1;
  source=gml_surface_create(render,2,2);
  intermediate=gml_surface_create(render,4,4);
  CHECK(source>0 && intermediate>0);
  for(int index=0;index<4;index++) render->surface[source-1].px[index]=0xFF102030u;
  fake.readback_colour=0xFF5879A4u;
  gml_render_bind_device(render,&device);
  CHECK(gml_render_begin_resident_owned_application(render,0.0,0.0,0));
  CHECK(gml_surface_set_target(render,intermediate));
  render->active_shader=0;
  gml_draw_surface_stretched(render,source,0.0,0.0,4.0,4.0,0xFFFFFFu,1.0);
  CHECK(fake.submits==1 && fake.readbacks==0 && render->shaded_draws==1u);
  gml_render_resource_journal_metrics(render->surface[intermediate-1].resource,&metrics);
  CHECK(metrics.authority==GML_RENDER_RESOURCE_GPU_PENDING_CHECKPOINT &&
        metrics.gpu_version==metrics.logical_version &&
        render->surface[intermediate-1].px[5]==0u);
  gml_surface_reset_target(render);
  CHECK(fake.readbacks==0 && render->target_id==-1);
  gml_draw_surface_stretched(render,intermediate,0.0,0.0,4.0,4.0,
                             0xFFFFFFu,1.0);
  CHECK(gml_render_application_surface_owned_read(render,NULL));
  CHECK(fake.submits==2 && fake.readbacks==1 && render->shaded_draws==2u &&
        render->app_surface_owned[5]==fake.readback_colour);
  gml_render_resource_journal_metrics(render->surface[intermediate-1].resource,&metrics);
  CHECK(metrics.authority==GML_RENDER_RESOURCE_GPU_PENDING_CHECKPOINT);
  CHECK(gml_render_scene_flush(render) && fake.readbacks==2);
  gml_render_resource_journal_metrics(render->surface[intermediate-1].resource,&metrics);
  CHECK(metrics.authority==GML_RENDER_RESOURCE_BOTH_CURRENT &&
        metrics.device_checkpoints==1u);
  gml_render_bind_device(render,NULL);
  CHECK(gml_surface_pixels_read(render,intermediate,NULL,NULL) &&
        gml_render_resource_acquire_cpu_read(
          render->surface[intermediate-1].resource,&pixels) &&
        pixels[5]==fake.readback_colour && fake.readbacks==2);
  /* A state-size query is also a CPU observer: it seals a live numbered result once. */
  gml_render_bind_device(render,&device);
  CHECK(gml_render_begin_resident_owned_application(render,0.0,0.0,0));
  CHECK(gml_surface_set_target(render,intermediate));
  render->active_shader=0;
  gml_draw_surface_stretched(render,source,0.0,0.0,4.0,4.0,0xFFFFFFu,1.0);
  gml_surface_reset_target(render);
  gml_render_resource_journal_metrics(render->surface[intermediate-1].resource,&metrics);
  CHECK(metrics.authority==GML_RENDER_RESOURCE_GPU_PENDING_CHECKPOINT &&
        fake.readbacks==2);
  state_size=gml_render_state_size(render,0);
  CHECK(state_size>0 && fake.readbacks==3);
  state_bytes=(uint8_t*)malloc(state_size);
  CHECK(state_bytes && gml_render_state_save(render,0,state_bytes,state_size,&written) &&
        written==state_size && fake.readbacks==3 &&
        gml_render_state_load(restored,state_bytes,state_size,&used) && used==state_size);
  pixels=gml_surface_pixels_read(restored,intermediate,NULL,NULL);
  CHECK(pixels && pixels[5]==fake.readback_colour);
  free(state_bytes);
  /* A failed deferred seal must never expose the last canonical pixels as the new version. */
  gml_render_bind_device(render,&device);
  CHECK(gml_render_begin_resident_owned_application(render,0.0,0.0,0));
  CHECK(gml_surface_set_target(render,intermediate));
  render->active_shader=0;
  gml_draw_surface_stretched(render,source,0.0,0.0,4.0,4.0,0xFFFFFFu,1.0);
  gml_render_resource_journal_metrics(render->surface[intermediate-1].resource,&metrics);
  CHECK(metrics.authority==GML_RENDER_RESOURCE_GPU_PENDING_CHECKPOINT &&
        render->surface[intermediate-1].px[5]==fake.readback_colour);
  gml_surface_reset_target(render);
  fake.fail_readback=1;
  CHECK(!gml_render_scene_flush(render) &&
        !gml_surface_pixels_read(render,intermediate,NULL,NULL));
  gml_render_resource_journal_metrics(render->surface[intermediate-1].resource,&metrics);
  CHECK(metrics.authority==GML_RENDER_RESOURCE_INVALID &&
        render->surface[intermediate-1].px[5]==fake.readback_colour);
  CHECK(!gml_render_scene_flush(render)); /* A repeated seal must not turn failure into success. */
  gml_render_bind_device(render,NULL);
  CHECK(!gml_surface_pixels_read(render,intermediate,NULL,NULL));
  gml_surface_free(render,intermediate);
  abandoned=gml_surface_create(render,4,4);
  CHECK(abandoned>0);
  fake.fail_readback=0;
  gml_render_bind_device(render,&device);
  CHECK(gml_render_begin_resident_owned_application(render,0.0,0.0,0));
  CHECK(gml_surface_set_target(render,abandoned));
  render->active_shader=0;
  gml_draw_surface_stretched(render,source,0.0,0.0,4.0,4.0,0xFFFFFFu,1.0);
  gml_surface_reset_target(render);
  gml_render_resource_journal_metrics(render->surface[abandoned-1].resource,&metrics);
  CHECK(metrics.authority==GML_RENDER_RESOURCE_GPU_PENDING_CHECKPOINT);
  readbacks_before_abandon=fake.readbacks;
  gml_render_abandon_device(render);
  CHECK(fake.readbacks==readbacks_before_abandon &&
        !gml_surface_pixels_read(render,abandoned,NULL,NULL));
  gml_render_free(render);
  free(render);
  gml_render_free(restored);
  free(restored);
  return 1;
}

static int public_surface_part_qualified_dispatch(void){
  GmlRender *render=(GmlRender*)calloc(1,sizeof *render);
  GmlRenderApplicationWriteView view={0};
  GmlRenderJournalMetrics metrics={0};
  FakeDevice fake={0};
  GmlRenderDeviceServices device=service(&fake);
  uint32_t expected[16];
  const uint32_t authored[4]={0xFFB42834u,0xFF268BB5u,0xFF8CC539u,0xFFD37224u};
  const uint32_t binary[4]={0xFF26B481u,0x00D0A51Fu,0xFF692CB3u,0x00134791u};
  int id,binary_id;
  CHECK(render);
  render->next_surface_id=1;
  render->active_shader=-1;
  render->color_write_mask=0x0Fu;
  render->alphablend=1;
  render->blend_equation=render->blend_equation_alpha=1;
  CHECK(gml_render_application_surface_ensure_owned(render,4,4));
  id=gml_surface_create(render,2,2);
  CHECK(id>0);
  binary_id=gml_surface_create(render,2,2);
  CHECK(binary_id>0);
  memcpy(render->surface[id-1].px,authored,sizeof authored);
  memcpy(render->surface[binary_id-1].px,binary,sizeof binary);
  render->surface[id-1].opaque_known=render->surface[id-1].all_opaque=1;
  render->surface[id-1].all_transparent=0;
  render->surface[binary_id-1].all_transparent=0;
  gml_render_bind_device(render,&device);
  CHECK(gml_render_begin_resident_owned_application(render,0.0,0.0,0));
  gml_render_set_pending_fill(render,0xFF102030u);
  gml_draw_surface_part_ext(render,id,0,0,2,2,0,0,1,1,0xFFFFFFu,1.0);
  gml_draw_surface_part_ext(render,id,1,0,1,2,3,1,1,1,0xFFFFFFu,1.0);
  gml_draw_surface_part_ext(render,id,0,0,1,1,1,2,2,2,0xFFFFFFu,1.0);
  gml_draw_surface_stretched(render,id,2,2,2,2,0xFFFFFFu,1.0);
  gml_draw_surface_stretched(render,binary_id,0,2,2,2,0xFFFFFFu,1.0);
  gml_draw_surface_part_ext(render,binary_id,0,0,1,2,2,0,1,1,0xFFFFFFu,1.0);
  CHECK(render->resident_application_pass && gml_render_scene_flush(render));
  gml_render_resource_journal_metrics(render->app_surface_resource,&metrics);
  CHECK(metrics.journal_operations==7u && !metrics.cpu_replays &&
        fake.submits==7 && !fake.readbacks);
  for(int index=0;index<16;index++) expected[index]=0xFF102030u;
  expected[0]=authored[0]; expected[1]=authored[1]; expected[2]=binary[0];
  expected[4]=authored[2]; expected[5]=authored[3];
  expected[6]=binary[2]; expected[7]=authored[1];
  expected[8]=binary[0]; expected[9]=expected[10]=authored[0];
  expected[11]=authored[1];
  expected[12]=binary[2]; expected[13]=authored[0];
  expected[14]=authored[2]; expected[15]=authored[3];
  gml_surface_free(render,id);
  gml_surface_free(render,binary_id);
  gml_render_bind_device(render,NULL);
  CHECK(gml_render_application_surface_owned_view(render,&view) &&
        view.width==4 && view.height==4 &&
        !memcmp(view.pixels,expected,sizeof expected) && !fake.readbacks);
  gml_render_free(render);
  free(render);
  return 1;
}

/* The transient coarse texel must be retained, not checkpointed or borrowed from its caller. */
static int resident_coarse_veil_exact_recovery(void){
  const uint8_t alphas[]={0u,1u,17u,127u,128u,254u,255u};
  GmlRender *render=(GmlRender*)calloc(1,sizeof *render);
  GmlRenderApplicationWriteView view={0};
  CHECK(render && gml_render_application_surface_ensure_owned(render,4,4));
  render->atlas=(GmlAtlas*)calloc(1,sizeof *render->atlas);
  render->tpag=(GmlTpag*)calloc(1,sizeof *render->tpag);
  render->spr=(GmlSprite*)calloc(1,sizeof *render->spr);
  CHECK(render->atlas && render->tpag && render->spr);
  render->n_atlas=render->n_tpag=render->n_spr=1;
  render->atlas[0].px=(uint8_t*)malloc(8u*8u*4u);
  render->spr[0].frame=(int*)calloc(1,sizeof(int));
  CHECK(render->atlas[0].px && render->spr[0].frame);
  render->atlas[0].w=render->atlas[0].h=8;
  render->tpag[0].sw=render->tpag[0].sh=8;
  render->tpag[0].bw=render->tpag[0].bh=8;
  render->spr[0].w=render->spr[0].h=8;
  render->spr[0].n_frames=1;
  render->color_write_mask=15u;
  render->interp=1;
  for(unsigned alpha=0;alpha<sizeof alphas;alpha++) for(int failure=0;failure<4;failure++){
    FakeDevice fake={0};
    GmlRenderDeviceServices device=service(&fake);
    GmlRenderJournalMetrics metrics={0},before_veil={0};
    const uint32_t operations=alphas[alpha]?3u:1u;
    const size_t retained=alphas[alpha]?8u:0u;
    uint32_t expected[16];
    gml_render_bind_device(render,NULL);
    CHECK(gml_render_begin_owned_application(render,0.0,0.0,0));
    gml_render_set_pending_fill(render,0xFF714A24u);
    for(int pass=0;pass<2;pass++){
      for(int index=0;index<64;index++){
        uint8_t *pixel=render->atlas[0].px+index*4;
        pixel[0]=pass?213u:29u; pixel[1]=pass?19u:203u;
        pixel[2]=pass?97u:41u; pixel[3]=alphas[alpha];
      }
      /* A covering off-target quad is the same flat CPU veil after clipping. */
      gml_draw_sprite_stretched(render,0,0,-1,-1,6,6,0xFFFFFFu,1.0);
    }
    gml_render_flush_pending_fill(render);
    memcpy(expected,render->app_surface_owned,sizeof expected);
    gml_render_bind_device(render,&device);
    CHECK(gml_render_begin_resident_owned_application(render,0.0,0.0,0));
    gml_render_set_pending_fill(render,0xFF714A24u);
    gml_render_flush_pending_fill(render);
    CHECK(fake.submits==1 && fake.readbacks==0);
    gml_render_resource_journal_metrics(render->app_surface_resource,&before_veil);
    if(failure==1) fake.status=GML_RENDER_EXEC_UNSUPPORTED;
    if(failure==2) fake.fail_upload=1;
    if(failure==3){
      fake.status=GML_RENDER_EXEC_RECOVERABLE_ERROR;
      fake.accepted_prefix=1u;
    }
    for(int pass=0;pass<2;pass++){
      for(int index=0;index<64;index++){
        uint8_t *pixel=render->atlas[0].px+index*4;
        pixel[0]=pass?213u:29u; pixel[1]=pass?19u:203u;
        pixel[2]=pass?97u:41u; pixel[3]=alphas[alpha];
      }
      gml_draw_sprite_stretched(render,0,0,-1,-1,6,6,0xFFFFFFu,1.0);
    }
    CHECK(fake.readbacks==0);
    gml_render_resource_journal_metrics(render->app_surface_resource,&metrics);
    if(!failure && (metrics.journal_operations!=operations || metrics.retained_mutable_bytes!=retained ||
       metrics.cpu_replays!=before_veil.cpu_replays ||
       metrics.device_checkpoints!=before_veil.device_checkpoints || !render->resident_application_pass))
      fprintf(stderr,"coarse veil alpha=%u resident=%d operations=%u retained=%zu replay=%llu checkpoint=%llu\n",
        alphas[alpha],render->resident_application_pass,metrics.journal_operations,
        metrics.retained_mutable_bytes,(unsigned long long)metrics.cpu_replays,
        (unsigned long long)metrics.device_checkpoints);
    if(!failure) CHECK(render->resident_application_pass &&
      metrics.journal_operations==operations && metrics.retained_mutable_bytes==retained &&
      metrics.device_checkpoints==before_veil.device_checkpoints &&
      metrics.cpu_replays==before_veil.cpu_replays);
    /* Both reduced RGBA texels are now gone; recovery must use their pinned versions. */
    memset(render->atlas[0].px,0,8u*8u*4u);
    gml_render_bind_device(render,NULL);
    fake.forbid_calls=1;
    CHECK(gml_render_application_surface_owned_view(render,&view) &&
      !memcmp(view.pixels,expected,sizeof expected) && !fake.readbacks &&
      !fake.forbidden_calls);
  }
  gml_render_free(render);
  free(render);
  return 1;
}

/* Identity SCREEN GUI veils keep device bytes until a real observation requests them. */
static int resident_gui_coarse_veil_on_demand(void){
  const uint8_t alphas[]={0u,1u,127u,255u};
  for(unsigned interpolation=0;interpolation<2u;interpolation++)
  for(unsigned witness=0;witness<2u;witness++)
  for(unsigned destination=0;destination<3u;destination++)
  for(unsigned coverage=0;coverage<sizeof alphas;coverage++){
    GmlRender *render=(GmlRender*)calloc(1,sizeof *render);
    FakeDevice fake={.on_demand=witness!=0u,.readback_colour=0xFF456789u};
    GmlRenderDeviceServices device=service(&fake);
    GmlRenderJournalMetrics metrics={0};
    uint32_t screen[16],sentinel[16];
    CHECK(render);
    render->atlas=(GmlAtlas*)calloc(1,sizeof *render->atlas);
    render->tpag=(GmlTpag*)calloc(1,sizeof *render->tpag);
    render->spr=(GmlSprite*)calloc(1,sizeof *render->spr);
    CHECK(render->atlas && render->tpag && render->spr);
    render->n_atlas=render->n_tpag=render->n_spr=1;
    render->atlas[0].px=(uint8_t*)malloc(8u*8u*4u);
    render->spr[0].frame=(int*)calloc(1,sizeof(int));
    CHECK(render->atlas[0].px && render->spr[0].frame);
    render->atlas[0].w=render->atlas[0].h=8;
    render->tpag[0].sw=render->tpag[0].sh=8;
    render->tpag[0].bw=render->tpag[0].bh=8;
    render->spr[0].w=render->spr[0].h=8;
    render->spr[0].n_frames=1;
    render->color_write_mask=15u;
    for(unsigned index=0;index<64u;index++){
      uint8_t *pixel=render->atlas[0].px+index*4u;
      pixel[0]=29u;pixel[1]=203u;pixel[2]=41u;pixel[3]=alphas[coverage];
    }
    for(unsigned index=0;index<16u;index++) screen[index]=sentinel[index]=0xA5123456u;
    device.observe_on_demand=fake_observe_on_demand;
    gml_render_bind_device(render,&device);
    CHECK(gml_render_begin_resident_base(render,screen,4,4,0.0,0.0));
    gml_render_gui_begin(render,4,4);
    render->interp=(int)interpolation;
    gml_render_set_pending_fill(render,destination?0xA5714A24u:0xFF714A24u);
    gml_render_flush_pending_fill(render);
    /* Submission is not an observation: scene_flush would explicitly seal the GPU clear. */
    CHECK(gml_render_scene_submit_pending(render) && !fake.readbacks && !fake.rgb_queries);
    if(destination==2u) render->fb_opaque_known=render->fb_all_opaque=0;
    int submits=fake.submits;
    gml_draw_sprite_stretched(render,0,0,-1,-1,6,6,0xFFFFFFu,1.0);
    CHECK(gml_render_scene_submit_pending(render) && !fake.readbacks && !fake.rgb_queries);
    if(witness){
      CHECK(render->resident_application_pass && !memcmp(screen,sentinel,sizeof screen));
      if(alphas[coverage]){
        CHECK(fake.submits==submits+1 && fake.last_opcode==GML_RENDER_OP_TEXTURED_QUAD &&
              fake.last_target_image.resource_class==GML_RENDER_RESOURCE_SCREEN &&
              fake.last_filter==(destination?GML_RENDER_FILTER_HARDWARE_BILINEAR:
                                            GML_RENDER_FILTER_NEAREST) &&
              fake.last_quad_state.coverage_policy==(destination?
                GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA:0u) &&
              fake.last_quad_state.rgb_equation==GML_RENDER_BLEND_ADD &&
              fake.last_quad_state.alpha_equation==GML_RENDER_BLEND_ADD &&
              render->fb_opaque_known && render->fb_all_opaque);
      }else CHECK(fake.submits==submits);
      gml_render_resource_journal_metrics(render->base_scene_resource,&metrics);
      CHECK(metrics.authority==GML_RENDER_RESOURCE_GPU_RESIDENT &&
            metrics.gpu_version==metrics.logical_version &&
            metrics.cpu_version<metrics.logical_version &&
            !metrics.journal_operations && !metrics.retained_mutable_bytes &&
            !metrics.cpu_replays && !metrics.device_checkpoints);
      /* Even a transparent veil leaves the preceding GPU clear to be observed explicitly. */
      CHECK(gml_render_completed_screen_read(render,screen,4,4) && fake.readbacks==1);
      for(unsigned index=0;index<16u;index++) CHECK(screen[index]==fake.readback_colour);
    }else CHECK(!render->resident_application_pass && fake.submits==submits);
    gml_render_bind_device(render,NULL);
    fake.forbid_calls=1;
    CHECK(gml_render_completed_screen_read(render,screen,4,4) && !fake.forbidden_calls &&
          fake.readbacks==(witness?1:0));
    gml_render_free(render);
    free(render);
  }
  return 1;
}

static int resident_stretched_function_on_demand_impl(int profile){
  int raw_opacity=profile==1,coarse_box=profile>=2,over_limit=profile==3;
  const int widths[]={6,7,3,2,1},heights[]={16,3,4,1,1};
  const double opacities[]={1.0+0x1p-52,1.0+0x1p-51,1.0+0x1p-25,1.0+0x1p-24,
    nextafter(1.0+0x1p-24,2.0),1.25,2.0,16.0,255.0,256.0,1e100};
  for(unsigned classic=0;classic<2u;classic++) for(unsigned kind=0;kind<3u;kind++)
  for(unsigned opacity=0;opacity<(raw_opacity?11u:1u);opacity++)
  for(unsigned reduction=0;reduction<(coarse_box && !over_limit?5u:1u);reduction++){
    double alpha=raw_opacity?opacities[opacity]:.975;
    int physical=coarse_box?64:4,logical=over_limit?65:physical;
    int width=coarse_box?(over_limit?1:widths[reduction]):12;
    int height=coarse_box?(over_limit?1:heights[reduction]):10;
    GmlRender *r=calloc(1,sizeof *r);
    GmlWin content={.bytecode=classic?0:15,.classic_version=classic?701:0};
    FakeDevice fake={.on_demand=1,.readback_colour=0xFF315779u};
    GmlRenderDeviceServices device=service(&fake);
    uint32_t screen[16*12],shadow[16*12];
    GmlRenderJournalMetrics metrics={0};const uint32_t *pixels=NULL;
    CHECK(r && gml_render_init(r,NULL)==0);
    r->win=&content;r->classic=(int)classic;
    /* This fixture replaces the initial default-font atlas, not its glyphs. */
    CHECK(r->n_atlas==1 && r->atlas && r->atlas[0].px);
    free(r->atlas[0].px);free(r->atlas);
    r->atlas=calloc(1,sizeof *r->atlas);r->tpag=calloc(1,sizeof *r->tpag);
    r->spr=calloc(1,sizeof *r->spr);
    CHECK(r->atlas && r->tpag && r->spr);
    r->n_atlas=r->n_tpag=r->n_spr=1;
    r->atlas[0].px=malloc((size_t)physical*physical*4u);r->spr[0].frame=calloc(1,sizeof(int));
    CHECK(r->atlas[0].px && r->spr[0].frame);
    r->atlas[0].w=r->atlas[0].h=physical;
    memset(r->atlas[0].px,191,(size_t)physical*physical*4u);
    r->tpag[0]=(GmlTpag){.sw=physical,.sh=physical,.tw=physical,.th=physical,
      .bw=logical,.bh=logical};
    r->spr[0].w=r->spr[0].h=logical;r->spr[0].n_frames=1;
    for(unsigned p=0;p<16u*12u;p++) screen[p]=0xA5010203u;
    device.observe_on_demand=fake_observe_on_demand;
    gml_render_bind_device(r,&device);
    CHECK(gml_render_begin_resident_base(r,screen,16,12,.25,.75));
    if(kind==1u){
      CHECK(gml_render_application_surface_ensure_owned(r,16,12));
      CHECK(gml_surface_set_target(r,0));
    }
    if(kind==2u){int id=gml_surface_create(r,16,12);CHECK(id>0 && gml_surface_set_target(r,id));}
    gml_render_clear(r,0x684624u,128.0/255.0);gml_render_flush_pending_fill(r);
    CHECK(gml_render_scene_submit_pending(r));
    GmlRenderResource *resource=gml_render_scene_target_resource_ensure(r);
    CHECK(resource);
    memcpy(shadow,r->fb,sizeof shadow);
    int submits=fake.submits,reads=fake.readbacks;
    r->color_write_mask=5u;r->blendmode=2;r->alphablend=classic;
    r->alpha_test_enable=1;r->alpha_test_ref=127u;
    if(raw_opacity) r->alpha=.37;
    for(unsigned draw=0;draw<4u;draw++)
      gml_draw_sprite_stretched(r,0,0,coarse_box?3.25:-.3,coarse_box?2.25:.3,
        width,height,0x93B5D7u,alpha);
    CHECK(fake.submits==submits && gml_render_scene_submit_pending(r));
    if(over_limit){
      CHECK(!r->resident_application_pass && fake.submits==submits &&
            fake.readbacks==reads+1);
      gml_render_free(r);free(r);continue;
    }
    if(raw_opacity){
      CHECK(r->alpha==.37);
    }
    CHECK(r->resident_application_pass && fake.submits==submits+1 && fake.readbacks==reads &&
      fake.last_batch_operations==4u && fake.last_filter==GML_RENDER_FILTER_BOX &&
      fake.last_quantisation==GML_RENDER_QUANTISE_FIXED_STRETCHED &&
      fake.last_quad_state.coverage_policy==GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA &&
      fake.last_quad_state.write_mask==15u && fake.last_quad_state.alpha_test_enabled &&
      fake.last_quad_state.alpha_test_reference==127u &&
      fake.last_quad.span_x[0]==logical && fake.last_quad.span_x[1]==width &&
      fake.last_filtered_draw_alpha==fmin(alpha,255.0) && !memcmp(shadow,r->fb,sizeof shadow));
    gml_render_resource_journal_metrics(resource,&metrics);
    CHECK(metrics.authority==GML_RENDER_RESOURCE_GPU_RESIDENT && !metrics.cpu_replays &&
      !metrics.journal_operations && !metrics.device_checkpoints);
    CHECK(gml_render_resource_acquire_cpu_read(resource,&pixels) && fake.readbacks==reads+1);
    CHECK(pixels[0]==fake.readback_colour);
    gml_render_bind_device(r,NULL);fake.forbid_calls=1;
    CHECK(gml_render_resource_acquire_cpu_read(resource,&pixels) && !fake.forbidden_calls &&
          pixels[0]==fake.readback_colour);
    gml_render_free(r);free(r);
  }
  return 1;
}

static int resident_stretched_function_on_demand(void){
  return resident_stretched_function_on_demand_impl(0);
}

static int resident_stretched_raw_opacity_on_demand(void){
  return resident_stretched_function_on_demand_impl(1);
}

static int resident_stretched_coarse_box_on_demand(void){
  return resident_stretched_function_on_demand_impl(2);
}

static int resident_stretched_coarse_box_limit(void){
  return resident_stretched_function_on_demand_impl(3);
}

static int resident_stretched_unavailable_source(void){
  for(unsigned invalid_crop=0;invalid_crop<2u;invalid_crop++){
    GmlRender *r=calloc(1,sizeof *r);
    GmlWin content={.bytecode=15};
    FakeDevice fake={.on_demand=1,.readback_colour=0xFF315779u};
    GmlRenderDeviceServices device=service(&fake);
    uint32_t screen[16u*12u];
    CHECK(r && gml_render_init(r,NULL)==0);
    r->win=&content;
    r->atlas=calloc(1,sizeof *r->atlas);r->tpag=calloc(1,sizeof *r->tpag);
    r->spr=calloc(1,sizeof *r->spr);
    CHECK(r->atlas && r->tpag && r->spr);
    r->n_atlas=r->n_tpag=r->n_spr=1;
    r->atlas[0].w=r->atlas[0].h=4;
    if(invalid_crop){
      r->atlas[0].px=malloc(4u*4u*4u);CHECK(r->atlas[0].px);
      memset(r->atlas[0].px,255,4u*4u*4u);
    }
    r->tpag[0]=(GmlTpag){.sx=invalid_crop?4:0,.sw=4,.sh=4,.tw=4,.th=4,.bw=4,.bh=4};
    r->spr[0].w=r->spr[0].h=4;r->spr[0].n_frames=1;
    r->spr[0].frame=calloc(1,sizeof(int));CHECK(r->spr[0].frame);
    for(unsigned p=0;p<16u*12u;p++) screen[p]=0xA5010203u;
    device.observe_on_demand=fake_observe_on_demand;gml_render_bind_device(r,&device);
    CHECK(gml_render_begin_resident_base(r,screen,16,12,.25,.75));
    gml_render_clear(r,0x684624u,128.0/255.0);gml_render_flush_pending_fill(r);
    CHECK(gml_render_scene_submit_pending(r));
    int submits=fake.submits;r->interp=1;
    gml_draw_sprite_stretched(r,0,0,1,1,12,10,0xFFFFFFu,16);
    CHECK(!r->resident_application_pass && fake.submits==submits &&
      !r->tpag[0].scene_opaque_checked);
    gml_render_free(r);free(r);
  }
  return 1;
}

static int resident_mapped_gui_stretch_on_demand(void){
  for(unsigned exclusion=0;exclusion<7u;exclusion++){
    GmlRender *render=(GmlRender*)calloc(1,sizeof *render);
    GmlWin content={.bytecode=15};
    FakeDevice fake={.on_demand=exclusion!=1u,.readback_colour=0xFF456789u};
    GmlRenderDeviceServices device=service(&fake);
    GmlRenderJournalMetrics metrics={0};
    uint32_t screen[24u*16u],sentinel[24u*16u];
    CHECK(render);
    render->atlas=(GmlAtlas*)calloc(1,sizeof *render->atlas);
    render->tpag=(GmlTpag*)calloc(1,sizeof *render->tpag);
    render->spr=(GmlSprite*)calloc(1,sizeof *render->spr);
    CHECK(render->atlas && render->tpag && render->spr);
    render->n_atlas=render->n_tpag=render->n_spr=1;
    render->atlas[0].px=(uint8_t*)malloc(8u*8u*4u);
    render->spr[0].frame=(int*)calloc(1,sizeof(int));
    CHECK(render->atlas[0].px && render->spr[0].frame);
    render->atlas[0].w=render->atlas[0].h=8;
    render->tpag[0].sw=render->tpag[0].sh=8;
    render->tpag[0].tw=render->tpag[0].th=8;
    render->tpag[0].bw=render->tpag[0].bh=8;
    render->spr[0].w=render->spr[0].h=8;
    render->spr[0].n_frames=1;
    render->win=&content;render->color_write_mask=15u;render->interp=1;
    for(unsigned index=0;index<64u;index++){
      uint8_t *pixel=render->atlas[0].px+index*4u;
      pixel[0]=29u;pixel[1]=203u;pixel[2]=41u;pixel[3]=127u;
    }
    for(unsigned index=0;index<24u*16u;index++) screen[index]=sentinel[index]=0xA5123456u;
    device.observe_on_demand=fake_observe_on_demand;
    gml_render_bind_device(render,&device);
    CHECK(gml_render_begin_resident_base(render,screen,24,16,-3.0,-2.0));
    gml_render_gui_begin(render,18,12);
    gml_render_gui_set_size(render,36,48);
    gml_render_set_pending_fill(render,0xA5714A24u);
    gml_render_flush_pending_fill(render);
    CHECK(gml_render_scene_submit_pending(render) && !fake.readbacks && !fake.rgb_queries);
    int submits=fake.submits;
    if(exclusion==2u) render->gui_maximise_active=1;
    if(exclusion==3u) render->color_write_mask=7u;
    if(exclusion==4u) render->blendmode=1;
    if(exclusion==5u) render->alpha_test_enable=1;
    if(exclusion==6u){render->tpag[0].tx=1;render->tpag[0].sw=7;}
    gml_draw_sprite_stretched(render,0,0,0.5,1.0,54.98,73.96,0xFFFFFFu,1.0);
    CHECK(gml_render_scene_submit_pending(render));
    if(!exclusion){
      CHECK(render->resident_application_pass && !memcmp(screen,sentinel,sizeof screen) &&
            fake.submits==submits+1 && fake.last_opcode==GML_RENDER_OP_TEXTURED_QUAD &&
            fake.last_target_image.resource_class==GML_RENDER_RESOURCE_SCREEN &&
            fake.last_filter==GML_RENDER_FILTER_HARDWARE_BILINEAR && !fake.last_quantisation &&
            fake.last_quad_state.coverage_policy==GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA &&
            fake.last_quad_state.rgb_equation==GML_RENDER_BLEND_ADD &&
            fake.last_quad.x[0]==3.0 && fake.last_quad.y[0]==2.0 &&
            fake.last_quad.x[1]==30.0 && fake.last_quad.y[2]==20.0 &&
            !fake.readbacks && !fake.rgb_queries);
      gml_render_resource_journal_metrics(render->base_scene_resource,&metrics);
      CHECK(metrics.authority==GML_RENDER_RESOURCE_GPU_RESIDENT &&
            metrics.cpu_version<metrics.logical_version && metrics.gpu_version==metrics.logical_version &&
            !metrics.journal_operations && !metrics.retained_mutable_bytes &&
            !metrics.cpu_replays && !metrics.device_checkpoints);
      CHECK(gml_render_completed_screen_read(render,screen,24,16) && fake.readbacks==1);
    }else if(exclusion==2u || exclusion==3u || exclusion==4u || exclusion==6u){
      /* The general stretch owner now qualifies these former admission limits.
       * Its real paired matrix owns pixels; retain the old witness/cutout refusals. */
      CHECK(render->resident_application_pass && !memcmp(screen,sentinel,sizeof screen) &&
            fake.submits==submits+1 && !fake.readbacks && !fake.rgb_queries &&
            fake.last_filter==GML_RENDER_FILTER_HARDWARE_BILINEAR &&
            fake.last_quantisation==GML_RENDER_QUANTISE_FIXED_STRETCHED &&
            fake.last_quad_state.coverage_policy==GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA &&
            fake.last_quad_state.write_mask==15u);
    }else CHECK(!render->resident_application_pass && fake.submits==submits);
    gml_render_free(render);free(render);
  }
  return 1;
}

static int resident_modern_screen_filtered_sprite(void){
  static const double rotations[]={0,360,-360,720,-720,.0005,-.0005,359.9995,-359.9995};
  for(unsigned angle=0;angle<sizeof rotations/sizeof rotations[0];angle++)
  for(unsigned variant=0;variant<20u;variant++){
    GmlRender *render=(GmlRender*)calloc(1,sizeof *render);
    GmlWin content={.bytecode=variant==18u?15:17};
    FakeDevice fake={.on_demand=variant!=1u};
    GmlRenderDeviceServices device=service(&fake);
    GmlRenderJournalMetrics metrics={0};
    uint32_t screen[24u*16u],sentinel[24u*16u];
    CHECK(render);
    render->atlas=calloc(1,sizeof *render->atlas);
    render->tpag=calloc(1,sizeof *render->tpag);
    render->spr=calloc(1,sizeof *render->spr);
    CHECK(render->atlas && render->tpag && render->spr);
    render->n_atlas=render->n_tpag=render->n_spr=1;
    render->atlas[0].px=malloc(10u*10u*4u);
    render->spr[0].frame=calloc(1,sizeof(int));
    CHECK(render->atlas[0].px && render->spr[0].frame);
    memset(render->atlas[0].px,127,10u*10u*4u);
    render->atlas[0].w=render->atlas[0].h=10;
    render->tpag[0]=(GmlTpag){.sx=1,.sy=1,.sw=8,.sh=8,.tw=8,.th=8,.bw=8,.bh=8};
    render->spr[0].w=render->spr[0].h=8;render->spr[0].n_frames=1;
    render->win=&content;render->color_write_mask=15u;render->interp=1;
    for(unsigned index=0;index<24u*16u;index++) screen[index]=sentinel[index]=0xA5123456u;
    device.observe_on_demand=fake_observe_on_demand;
    gml_render_bind_device(render,&device);
    CHECK(gml_render_begin_resident_base(render,screen,24,16,0.0,0.0));
    gml_render_gui_begin(render,24,16);
    gml_render_gui_set_size(render,48,64);
    if(variant>=11u){
      gml_render_gui_set_size(render,24,16);
      gml_render_gui_set_maximise(render,1,1.0,1.0,variant==12u?1.0:0.0,0.0,24,16);
    }
    gml_render_set_pending_fill(render,0xA5714A24u);
    gml_render_flush_pending_fill(render);
    CHECK(gml_render_scene_submit_pending(render));
    int submits=fake.submits;
    if(variant==2u) render->gui_maximise_active=1;
    if(variant==3u) render->color_write_mask=7u;
    if(variant==4u) render->blendmode=1;
    if(variant==5u) render->alpha_test_enable=1;
    if(variant==6u){render->tpag[0].tx=1;render->tpag[0].sw=7;render->tpag[0].tw=7;}
    if(variant==7u) render->tpag[0].project_authored_edges=1;
    if(variant==8u) render->fog_flat=1;
    if(variant==9u) render->active_shader=0;
    if(variant==14u) render->blendmode=6;
    if(variant==15u) render->active_shader=0;
    if(variant>=16u){
      render->tpag[0].tw=1;
      render->tpag[0].th=variant==17u?3:1;
    }
    if(variant==19u) render->tpag[0].project_authored_edges=1;
    uint64_t version=gml_render_resource_image_ref(render->base_scene_resource).content_version;
    if(variant==10u)
      gml_draw_sprite_ext(render,0,0,8.0,12.0,2.001,4.002,rotations[angle],0xFFFFFFu,1.0);
    else gml_draw_sprite_ext(render,0,0,8.5,13.0,1.2,1.5,rotations[angle],0x93B5D7u,
      variant>=13u && variant<=15u?0.0:1.0/90.0);
    CHECK(gml_render_scene_submit_pending(render));
    if(variant==13u){
      CHECK(render->resident_application_pass && fake.submits==submits &&
            !memcmp(screen,sentinel,sizeof screen) && !fake.readbacks && !fake.rgb_queries &&
            gml_render_resource_image_ref(render->base_scene_resource).content_version==version);
    }else if(!variant || variant==6u || variant==11u || variant==16u || variant==17u || variant==18u){
      CHECK(render->resident_application_pass && !memcmp(screen,sentinel,sizeof screen) &&
            fake.submits==submits+1 && fake.last_opcode==GML_RENDER_OP_TEXTURED_QUAD &&
            fake.last_filter==GML_RENDER_FILTER_HARDWARE_BILINEAR &&
            fake.last_quantisation==(variant==18u?GML_RENDER_QUANTISE_HARDWARE_SPRITE_FIRSTGEN:0u) &&
            fake.last_filtered_draw_alpha==1.0/90.0 &&
            fake.last_quad_state.coverage_policy==GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA &&
            fake.last_quad_state.rgb_equation==GML_RENDER_BLEND_ADD &&
            !fake.readbacks && !fake.rgb_queries);
      gml_render_resource_journal_metrics(render->base_scene_resource,&metrics);
      CHECK(metrics.authority==GML_RENDER_RESOURCE_GPU_RESIDENT &&
            metrics.cpu_version<metrics.logical_version && !metrics.journal_operations &&
            !metrics.cpu_replays && !metrics.device_checkpoints);
    }else if(variant==2u || variant==3u || variant==4u || variant==12u){
      /* Separately paired physical filtering supersedes only these former
       * mask/add admission exclusions, not the old SCREEN recipe. */
      CHECK(render->resident_application_pass && !memcmp(screen,sentinel,sizeof screen) &&
        fake.submits==submits+1 && !fake.readbacks && !fake.rgb_queries &&
        fake.last_filter==GML_RENDER_FILTER_HARDWARE_BILINEAR &&
        fake.last_quantisation==(variant==4u?GML_RENDER_QUANTISE_HARDWARE_SPRITE_FIRSTGEN:
                                            GML_RENDER_QUANTISE_HARDWARE_SPRITE_MODERN) &&
        fake.last_quad_state.write_mask==(variant==3u?7u:15u) &&
        fake.last_filtered_draw_alpha==1.0/90.0);
    }else if(variant==8u){
      /* Flat fog selects the CPU point owner even with filtering requested. */
      CHECK(render->resident_application_pass && !memcmp(screen,sentinel,sizeof screen) &&
            fake.submits==submits+1 && !fake.readbacks && !fake.rgb_queries &&
            fake.last_filter==GML_RENDER_FILTER_NEAREST &&
            fake.last_quantisation==GML_RENDER_QUANTISE_INTEGER_FLAT_RGB &&
            fake.last_filtered_draw_alpha==1.0/90.0);
      gml_render_resource_journal_metrics(render->base_scene_resource,&metrics);
      CHECK(metrics.authority==GML_RENDER_RESOURCE_GPU_RESIDENT &&
            metrics.cpu_version<metrics.logical_version && !metrics.journal_operations &&
            !metrics.cpu_replays && !metrics.device_checkpoints);
    }else CHECK(!render->resident_application_pass && fake.submits==submits);
    gml_render_free(render);free(render);
  }
  return 1;
}

static int resident_maximised_filtered_real_font(void){
  for(unsigned variant=0;variant<17u;variant++){
    GmlRender *render=(GmlRender*)calloc(1,sizeof *render);
    GmlWin content={.bytecode=variant==9u || variant==14u?15:17};
    FakeDevice fake={.on_demand=variant!=1u};
    GmlRenderDeviceServices device=service(&fake);
    GmlRenderJournalMetrics metrics={0};
    uint32_t screen[24u*16u],sentinel[24u*16u];
    CHECK(render);
    render->atlas=calloc(1,sizeof *render->atlas);
    CHECK(render->atlas);
    render->n_atlas=render->n_fonts=1;
    render->atlas[0].px=malloc(16u*16u*4u);
    CHECK(render->atlas[0].px);
    memset(render->atlas[0].px,127,16u*16u*4u);
    render->atlas[0].w=render->atlas[0].h=16;
    GmlFont *font=&render->fonts[0];
    font->real=1;font->sprite=-1;font->atlas=0;
    font->line_height=font->align_height=5;
    font->n_glyphs=1;font->glyphs=calloc(1,sizeof *font->glyphs);
    CHECK(font->glyphs);
    for(unsigned index=0;index<256u;index++) font->glyph_by_char[index]=-1;
    font->glyph_by_char['A']=0;
    font->glyphs[0]=(GmlGlyph){.sx=5,.sy=5,.w=5,.h=5,.shift=6,.ch='A'};
    render->font=0;render->win=&content;render->color_write_mask=15u;render->interp=1;
    for(unsigned index=0;index<24u*16u;index++) screen[index]=sentinel[index]=0xA5123456u;
    device.observe_on_demand=fake_observe_on_demand;
    gml_render_bind_device(render,&device);
    CHECK(gml_render_begin_resident_base(render,screen,24,16,0.0,0.0));
    if(variant<14u || variant==16u){
      gml_render_gui_begin(render,24,16);
      if(variant==16u) gml_render_gui_set_size(render,12,8);
      gml_render_gui_set_maximise(render,1,1.0,1.0,variant==2u?1.0:0.0,0.0,24,16);
    }
    gml_render_set_pending_fill(render,0xA5714A24u);
    gml_render_flush_pending_fill(render);
    CHECK(gml_render_scene_submit_pending(render));
    int submits=fake.submits;
    if(variant==3u) render->color_write_mask=7u;
    if(variant==4u) render->blendmode=1;
    if(variant==5u) render->alpha_test_enable=1;
    if(variant==6u) font->runtime_owned=1;
    if(variant==7u) font->sdf_spread=1;
    if(variant==8u) render->fog_flat=1;
    if(variant==10u) render->active_shader=0;
    if(variant==11u){render->world_transform_active=1;render->world_scale_x=2.0;}
    uint64_t version=gml_render_resource_image_ref(render->base_scene_resource).content_version;
    if(variant>=14u && variant<16u)
      gml_draw_text_transformed(render,8.5,16.0,"A",1.0,1.0,0.0,0u,.3);
    else
      gml_draw_text_transformed(render,8.5,4.25,"A",variant==12u?1.5:.6,.75,0.0,0x93B5D7u,
        variant==13u?0.0:1.0/60.0);
    CHECK(gml_render_scene_submit_pending(render));
    if(variant>=13u && variant<16u){
      CHECK(render->resident_application_pass && fake.submits==submits &&
            !memcmp(screen,sentinel,sizeof screen) && !fake.readbacks && !fake.rgb_queries &&
            gml_render_resource_image_ref(render->base_scene_resource).content_version==version);
    }else if(!variant || variant==16u){
      CHECK(render->resident_application_pass && !memcmp(screen,sentinel,sizeof screen) &&
            fake.submits==submits+1 && fake.last_filter==GML_RENDER_FILTER_HARDWARE_BILINEAR &&
            !fake.last_quantisation && fake.last_filtered_draw_alpha==1.0/60.0 &&
            fake.last_quad_state.coverage_policy==GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA &&
            fake.last_quad_state.rgb_equation==GML_RENDER_BLEND_ADD &&
            !fake.readbacks && !fake.rgb_queries);
      gml_render_resource_journal_metrics(render->base_scene_resource,&metrics);
      CHECK(metrics.authority==GML_RENDER_RESOURCE_GPU_RESIDENT &&
            metrics.cpu_version<metrics.logical_version && !metrics.journal_operations &&
            !metrics.cpu_replays && !metrics.device_checkpoints);
    }else CHECK(!render->resident_application_pass && fake.submits==submits);
    gml_render_free(render);free(render);
  }
  return 1;
}

static int resident_numbered_sprite_channel_masks(void){
  const uint8_t rgba[16]={208,32,16,128,16,208,32,255,32,16,208,64,192,176,160,0};
  for(unsigned mask=0;mask<16u;mask++){
    GmlRender *render=(GmlRender*)calloc(1,sizeof *render);
    GmlWin content={.bytecode=17};
    FakeDevice fake={.on_demand=1};
    GmlRenderDeviceServices device=service(&fake);
    GmlRenderJournalMetrics metrics={0};
    CHECK(render);
    device.observe_on_demand=fake_observe_on_demand;
    render->win=&content;render->active_shader=-1;render->color_write_mask=15u;
    render->alphablend=1;render->blend_equation=render->blend_equation_alpha=1;
    render->next_surface_id=1;
    CHECK(gml_render_application_surface_ensure_owned(render,4,4));
    render->atlas=(GmlAtlas*)calloc(1,sizeof *render->atlas);
    render->tpag=(GmlTpag*)calloc(1,sizeof *render->tpag);
    render->spr=(GmlSprite*)calloc(1,sizeof *render->spr);
    CHECK(render->atlas && render->tpag && render->spr);
    render->n_atlas=render->n_tpag=render->n_spr=1;
    render->atlas[0].px=(uint8_t*)malloc(sizeof rgba);
    render->spr[0].frame=(int*)calloc(1,sizeof(int));
    CHECK(render->atlas[0].px && render->spr[0].frame);
    memcpy(render->atlas[0].px,rgba,sizeof rgba);
    render->atlas[0].w=render->atlas[0].h=2;
    render->tpag[0].sw=render->tpag[0].sh=render->tpag[0].tw=render->tpag[0].th=2;
    render->tpag[0].bw=render->tpag[0].bh=2;
    render->spr[0].w=render->spr[0].h=2;render->spr[0].n_frames=1;
    gml_render_bind_device(render,&device);
    CHECK(gml_render_begin_resident_owned_application(render,0.0,0.0,0));
    int id=gml_surface_create(render,4,4);
    CHECK(id>0 && gml_surface_set_target(render,id));
    gml_render_clear(render,0x332211u,0.5);
    render->color_write_mask=(uint8_t)mask;
    gml_draw_sprite_ext(render,0,0,1.0,1.0,0.5,0.5,0.0,0xA0C0E0u,0.6);
    CHECK(render->resident_application_pass && !render->color_write_scratch &&
          gml_render_scene_submit_pending(render) && !fake.readbacks);
    if(mask){
      CHECK(fake.last_opcode==GML_RENDER_OP_TEXTURED_QUAD && fake.last_write_mask==mask &&
            fake.last_filter==GML_RENDER_FILTER_NEAREST_PHASED &&
            fake.last_quantisation==GML_RENDER_QUANTISE_SPRITE_MODERN_NEAREST &&
            fake.last_filtered_draw_alpha==0.6);
      gml_render_resource_journal_metrics(render->surface[id-1].resource,&metrics);
      CHECK(metrics.authority==GML_RENDER_RESOURCE_GPU_RESIDENT &&
            !metrics.cpu_replays && !metrics.device_checkpoints);
    }
    /* Independent extent rounding touches each edge only in a CPU-skipped texel.
     * These suffixes must not advance storage or submit an empty device region. */
    int submits=fake.submits;
    uint64_t version=gml_render_resource_image_ref(render->surface[id-1].resource).content_version;
    gml_draw_sprite_ext(render,0,0,-1.4,1.0,0.75,0.5,0.0,0xA0C0E0u,0.6);
    gml_draw_sprite_ext(render,0,0,5.0,1.0,-0.75,0.5,0.0,0xA0C0E0u,0.6);
    gml_draw_sprite_ext(render,0,0,1.0,-1.4,0.5,0.75,0.0,0xA0C0E0u,0.6);
    gml_draw_sprite_ext(render,0,0,1.0,5.0,0.5,-0.75,0.0,0xA0C0E0u,0.6);
    CHECK(render->resident_application_pass && gml_render_scene_submit_pending(render) &&
          fake.submits==submits && !fake.readbacks &&
          gml_render_resource_image_ref(render->surface[id-1].resource).content_version==version);
    /* Wholly off-target coloured point sprites skip pixels before edge pruning too.
     * Keep the visible GPU prefix and target version for every channel mask. */
    gml_draw_sprite_ext(render,0,0,-8.0,1.25,1.0,1.0,0.0,0x000000u,1.0);
    gml_draw_sprite_ext(render,0,0,8.0,1.25,-1.0,1.0,0.0,0x000000u,1.0);
    gml_draw_sprite_ext(render,0,0,1.25,-8.0,1.0,1.0,0.0,0x000000u,1.0);
    gml_draw_sprite_ext(render,0,0,1.25,8.0,1.0,-1.0,0.0,0x000000u,1.0);
    CHECK(render->resident_application_pass && gml_render_scene_submit_pending(render) &&
          fake.submits==submits && !fake.readbacks &&
          gml_render_resource_image_ref(render->surface[id-1].resource).content_version==version);
    /* Disposing an unread test-device result is not pixel evidence. */
    gml_render_abandon_device(render);
    gml_render_free(render);free(render);
  }
  return 1;
}

static int resident_opaque_atlas_sprite_gpu(void){
  GmlRender *render=(GmlRender*)calloc(1,sizeof *render);
  GmlRenderApplicationWriteView view={0};
  GmlRenderJournalMetrics metrics={0};
  FakeDevice fake={0};
  GmlRenderDeviceServices device=service(&fake);
  uint32_t gpu_reconstructed[16];
  uint32_t fallback_pixels[16];
  uint32_t sample_plane[16]={0};
  const uint8_t rgba[16]={
    0xD0u,0x20u,0x10u,0xFFu, 0x10u,0xD0u,0x20u,0xFFu,
    0x20u,0x10u,0xD0u,0xFFu, 0xC0u,0xB0u,0xA0u,0xFFu
  };
  CHECK(render);
  CHECK(gml_render_application_surface_ensure_owned(render,4,4));
  render->atlas=(GmlAtlas*)calloc(1,sizeof *render->atlas);
  render->tpag=(GmlTpag*)calloc(1,sizeof *render->tpag);
  render->spr=(GmlSprite*)calloc(1,sizeof *render->spr);
  CHECK(render->atlas && render->tpag && render->spr);
  render->n_atlas=render->n_tpag=render->n_spr=1;
  render->atlas[0].px=(uint8_t*)malloc(sizeof rgba);
  render->spr[0].frame=(int*)calloc(1,sizeof(int));
  CHECK(render->atlas[0].px && render->spr[0].frame);
  memcpy(render->atlas[0].px,rgba,sizeof rgba);
  render->atlas[0].w=render->atlas[0].h=2;
  render->tpag[0].sw=render->tpag[0].sh=2;
  render->tpag[0].tw=render->tpag[0].th=2;
  render->tpag[0].bw=render->tpag[0].bh=2;
  render->spr[0].w=render->spr[0].h=2;
  render->spr[0].n_frames=1;
  render->active_shader=-1;
  render->color_write_mask=0x0Fu;
  gml_render_bind_device(render,&device);
  CHECK(gml_render_begin_resident_owned_application(render,0.0,0.0,0));
  gml_render_set_pending_fill(render,0xFF000000u);
  gml_draw_sprite_ext(render,0,0,1.0,1.0,1.0,1.0,0.0,0xFFFFFFu,1.0);
  gml_render_resource_journal_metrics(render->app_surface_resource,&metrics);
  CHECK(render->resident_application_pass && fake.submits==1 &&
        metrics.journal_operations==1u && metrics.cpu_replays==0u &&
        render->app_surface_owned[5]==0u);
  CHECK(gml_render_application_surface_owned_view(render,&view) &&
        view.pixels[0]==0xFF000000u && view.pixels[5]==0xFFD02010u &&
        view.pixels[6]==0xFF10D020u && view.pixels[9]==0xFF2010D0u &&
        view.pixels[10]==0xFFC0B0A0u && fake.readbacks==0);
  CHECK(fake.submits==2);
  memcpy(gpu_reconstructed,view.pixels,sizeof gpu_reconstructed);
  CHECK(gml_render_begin_owned_application(render,0.0,0.0,0));
  gml_render_set_pending_fill(render,0xFF000000u);
  gml_draw_sprite_ext(render,0,0,1.0,1.0,1.0,1.0,0.0,0xFFFFFFu,1.0);
  gml_render_flush_pending_fill(render);
  CHECK(fake.submits==2 &&
        !memcmp(render->app_surface_owned,gpu_reconstructed,sizeof gpu_reconstructed));
  CHECK(gml_render_begin_resident_owned_application(render,0.0,0.0,0));
  gml_render_set_pending_fill(render,0xFF000000u);
  gml_draw_sprite_ext(render,0,0,1.0,1.0,1.0,1.0,0.0,0xFFFFFFu,0.5);
  CHECK(!render->resident_application_pass && fake.submits==3 &&
        render->app_surface_owned[5]!=0xFF000000u);
  memcpy(fallback_pixels,render->app_surface_owned,sizeof fallback_pixels);
  CHECK(gml_render_begin_owned_application(render,0.0,0.0,0));
  gml_render_set_pending_fill(render,0xFF000000u);
  gml_draw_sprite_ext(render,0,0,1.0,1.0,1.0,1.0,0.0,0xFFFFFFu,0.5);
  gml_render_flush_pending_fill(render);
  CHECK(fake.submits==3 &&
        !memcmp(render->app_surface_owned,fallback_pixels,sizeof fallback_pixels));
  CHECK(gml_render_begin_resident_owned_application(render,0.0,0.0,0));
  gml_render_set_pending_fill(render,0u);
  gml_draw_sprite_ext(render,0,0,1.0,1.0,1.0,1.0,0.0,0xFFFFFFu,1.0);
  CHECK(render->resident_application_pass && fake.submits==4 &&
        !render->fb_all_transparent);
  CHECK(gml_render_application_surface_owned_view(render,&view) &&
        view.pixels[0]==0u && view.pixels[5]==0xFFD02010u);
  CHECK(fake.submits==5);
  render->tpag[0].tx=render->tpag[0].ty=1;
  render->tpag[0].bw=render->tpag[0].bh=4;
  render->spr[0].w=render->spr[0].h=4;
  render->spr[0].originx=render->spr[0].originy=2;
  render->interp=render->texture_filter[0]=1;
  render->world_transform_active=1;
  render->world_scale_x=render->world_scale_y=1.0;
  render->app_phase_y=sample_plane;
  CHECK(gml_render_begin_resident_owned_application(render,0.0,0.0,0));
  gml_render_set_pending_fill(render,0xFF000000u);
  gml_draw_sprite_ext(render,0,0,2.0,2.0,1.0,1.0,0.0,0xFFFFFFu,1.0);
  CHECK(render->resident_application_pass && fake.submits==6);
  CHECK(gml_render_application_surface_owned_view(render,&view) &&
        view.pixels[0]==0xFF000000u && view.pixels[5]==0xFFD02010u &&
        view.pixels[6]==0xFF10D020u && view.pixels[9]==0xFF2010D0u &&
        view.pixels[10]==0xFFC0B0A0u && fake.readbacks==0);
  CHECK(fake.submits==7);
  memcpy(gpu_reconstructed,view.pixels,sizeof gpu_reconstructed);
  CHECK(gml_render_begin_owned_application(render,0.0,0.0,0));
  gml_render_set_pending_fill(render,0xFF000000u);
  gml_draw_sprite_ext(render,0,0,2.0,2.0,1.0,1.0,0.0,0xFFFFFFu,1.0);
  gml_render_flush_pending_fill(render);
  CHECK(fake.submits==7 &&
        !memcmp(render->app_surface_owned,gpu_reconstructed,sizeof gpu_reconstructed));
  render->app_phase_y=NULL;
  CHECK(gml_render_begin_resident_owned_application(render,0.0,0.0,0));
  gml_render_set_pending_fill(render,0xFF123456u);
  gml_draw_sprite_ext(render,0,0,2.0,2.0,1.0,1.0,0.0,0xFFFFFFu,1.0);
  gml_draw_sprite_ext(render,0,0,2.0,2.0,1.0,1.0,0.0,0xFFFFFFu,1.0);
  gml_render_resource_journal_metrics(render->app_surface_resource,&metrics);
  CHECK(fake.submits==8 && metrics.journal_operations==1u);
  fake.status=GML_RENDER_EXEC_RECOVERABLE_ERROR;
  fake.accepted_prefix=1u;
  CHECK(gml_render_scene_flush(render));
  fake.status=0;
  fake.accepted_prefix=0;
  gml_render_resource_journal_metrics(render->app_surface_resource,&metrics);
  CHECK(fake.submits==9 && fake.retires>=1 && fake.readbacks==0 &&
        metrics.cpu_fallbacks>=1u && metrics.journal_operations==0u &&
        render->app_surface_owned[0]==0xFF123456u &&
        render->app_surface_owned[5]==0xFFD02010u &&
        render->app_surface_owned[10]==0xFFC0B0A0u);
  CHECK(gml_render_begin_resident_owned_application(render,0.0,0.0,0));
  gml_render_set_pending_fill(render,0xFF123456u);
  gml_draw_sprite_ext(render,0,0,2.0,2.0,1.0,1.0,0.0,0xFFFFFFu,1.0);
  gml_draw_sprite_ext(render,0,0,2.0,2.0,1.0,1.0,0.0,0xFFFFFFu,1.0);
  CHECK(fake.submits==10);
  gml_render_bind_device(render,NULL);
  CHECK(fake.submits==10 && fake.readbacks==0 &&
        gml_render_application_surface_owned_view(render,&view) &&
        view.pixels[0]==0xFF123456u && view.pixels[5]==0xFFD02010u &&
        view.pixels[10]==0xFFC0B0A0u);
  gml_render_resource_journal_metrics(render->app_surface_resource,&metrics);
  CHECK(metrics.journal_operations==0u && metrics.cpu_fallbacks>=2u);
  gml_render_free(render);
  free(render);
  return 1;
}

static int resident_packaged_sprite_font_glyph(void){
  GmlRender *render=(GmlRender*)calloc(1,sizeof *render);
  GmlRenderApplicationWriteView view={0};
  FakeDevice fake={0};
  GmlRenderDeviceServices device=service(&fake);
  uint32_t completed[16];
  const uint8_t rgba[16]={
    0xD0u,0x20u,0x10u,0xFFu, 0x10u,0xD0u,0x20u,0xFFu,
    0x20u,0x10u,0xD0u,0xFFu, 0xC0u,0xB0u,0xA0u,0xFFu
  };
  CHECK(render && gml_render_application_surface_ensure_owned(render,4,4));
  render->atlas=(GmlAtlas*)calloc(1,sizeof *render->atlas);
  render->tpag=(GmlTpag*)calloc(1,sizeof *render->tpag);
  render->spr=(GmlSprite*)calloc(1,sizeof *render->spr);
  CHECK(render->atlas && render->tpag && render->spr);
  render->n_atlas=render->n_tpag=render->n_spr=render->n_fonts=1;
  render->atlas[0].px=(uint8_t*)malloc(sizeof rgba);
  render->spr[0].frame=(int*)calloc(1,sizeof(int));
  CHECK(render->atlas[0].px && render->spr[0].frame);
  memcpy(render->atlas[0].px,rgba,sizeof rgba);
  render->atlas[0].w=render->atlas[0].h=2;
  render->tpag[0].sw=render->tpag[0].sh=2;
  render->tpag[0].tw=render->tpag[0].th=2;
  render->tpag[0].bw=render->tpag[0].bh=2;
  render->spr[0].w=render->spr[0].h=2;
  render->spr[0].n_frames=1;
  render->fonts[0].sprite=0;
  render->fonts[0].first=65;
  render->font=0;
  render->alpha=1.0;
  render->color=0xFFFFFFu;
  render->alphablend=1;
  render->blend_equation=render->blend_equation_alpha=1;
  render->color_write_mask=0x0Fu;
  render->active_shader=-1;
  gml_render_bind_device(render,&device);
  CHECK(gml_render_begin_resident_owned_application(render,0.0,0.0,0));
  gml_render_set_pending_fill(render,0xFF000000u);
  gml_draw_text(render,1.0,1.0,"A");
  CHECK(render->resident_application_pass);
  CHECK(gml_render_scene_flush(render) && fake.submits==2);
  CHECK(gml_render_application_surface_owned_view(render,&view) &&
        view.pixels[5]==0xFFD02010u && view.pixels[6]==0xFF10D020u &&
        view.pixels[9]==0xFF2010D0u && view.pixels[10]==0xFFC0B0A0u);
  memcpy(completed,view.pixels,sizeof completed);
  CHECK(gml_render_begin_owned_application(render,0.0,0.0,0));
  gml_render_set_pending_fill(render,0xFF000000u);
  gml_draw_text(render,1.0,1.0,"A");
  gml_render_flush_pending_fill(render);
  CHECK(!memcmp(render->app_surface_owned,completed,sizeof completed));
  gml_render_bind_device(render,&device);
  CHECK(gml_render_begin_resident_owned_application(render,0.0,0.0,0));
  gml_render_set_pending_fill(render,0xFF000000u);
  gml_draw_text_transformed(render,0.0,0.0,"A",2.0,2.0,0.0,0xFFFFFFu,1.0);
  CHECK(render->resident_application_pass);
  CHECK(gml_render_scene_flush(render) && fake.submits==4 && fake.readbacks==0);
  CHECK(gml_render_application_surface_owned_view(render,&view));
  CHECK(view.pixels[0]==0xFFD02010u && view.pixels[1]==0xFFD02010u &&
        view.pixels[2]==0xFF10D020u && view.pixels[3]==0xFF10D020u &&
        view.pixels[8]==0xFF2010D0u && view.pixels[9]==0xFF2010D0u &&
        view.pixels[10]==0xFFC0B0A0u && view.pixels[11]==0xFFC0B0A0u);
  memcpy(completed,view.pixels,sizeof completed);
  CHECK(gml_render_begin_owned_application(render,0.0,0.0,0));
  gml_render_set_pending_fill(render,0xFF000000u);
  gml_draw_text_transformed(render,0.0,0.0,"A",2.0,2.0,0.0,0xFFFFFFu,1.0);
  gml_render_flush_pending_fill(render);
  CHECK(!memcmp(render->app_surface_owned,completed,sizeof completed));
  gml_render_bind_device(render,&device);
  fake.readback_colour=0xFF445566u;
  CHECK(gml_render_begin_resident_owned_application(render,0.0,0.0,0));
  gml_render_set_pending_fill(render,0xFF123456u);
  gml_draw_text_transformed(render,1.0,1.0,"A",1.0,1.0,0.0,0x80C040u,1.0);
  CHECK(render->resident_application_pass);
  CHECK(gml_render_scene_flush(render));
  CHECK(fake.submits==6 && fake.readbacks==1);
  CHECK(gml_render_application_surface_owned_view(render,&view) &&
        view.pixels[5]==fake.readback_colour && fake.readbacks==1);
  CHECK(gml_render_begin_owned_application(render,0.0,0.0,0));
  gml_render_set_pending_fill(render,0xFF123456u);
  gml_draw_text_transformed(render,1.0,1.0,"A",1.0,1.0,0.0,0x80C040u,1.0);
  gml_render_flush_pending_fill(render);
  CHECK(render->app_surface_owned[5]==0xFF341808u);
  int previous_submits=fake.submits;
  gml_render_bind_device(render,&device);
  CHECK(gml_render_begin_resident_owned_application(render,0.0,0.0,0));
  gml_render_set_pending_fill(render,0xFF123456u);
  gml_draw_text(render,1.0,1.0,"A");
  CHECK(render->resident_application_pass);
  gml_draw_text_transformed(render,2.0,2.0,"A",1.0,1.0,90.0,0xFFFFFFu,1.0);
  CHECK(!render->resident_application_pass &&
        fake.submits==previous_submits+2 && fake.readbacks==1);
  CHECK(gml_render_application_surface_owned_view(render,&view));
  memcpy(completed,view.pixels,sizeof completed);
  gml_render_bind_device(render,NULL);
  CHECK(gml_render_application_surface_owned_view(render,&view) &&
        !memcmp(view.pixels,completed,sizeof completed));
  CHECK(gml_render_begin_owned_application(render,0.0,0.0,0));
  gml_render_set_pending_fill(render,0xFF123456u);
  gml_draw_text(render,1.0,1.0,"A");
  gml_draw_text_transformed(render,2.0,2.0,"A",1.0,1.0,90.0,0xFFFFFFu,1.0);
  gml_render_flush_pending_fill(render);
  CHECK(!memcmp(render->app_surface_owned,completed,sizeof completed));
  gml_render_free(render);
  free(render);
  return 1;
}

static int resident_filtered_gui_real_font(void){
  enum { WIDTH=24,HEIGHT=18,ATLAS=12 };
  GmlRender *render=(GmlRender*)calloc(1,sizeof *render);
  GmlWin content={0};
  uint32_t reference[WIDTH*HEIGHT],screen[WIDTH*HEIGHT],sentinel[WIDTH*HEIGHT];
  CHECK(render);
  render->atlas=(GmlAtlas*)calloc(1,sizeof *render->atlas);
  CHECK(render->atlas);
  render->n_atlas=render->n_fonts=1;
  render->atlas[0].px=(uint8_t*)calloc(ATLAS*ATLAS,4u);
  CHECK(render->atlas[0].px);
  render->atlas[0].w=render->atlas[0].h=ATLAS;
  for(unsigned y=0;y<5u;y++) for(unsigned x=0;x<5u;x++){
    uint8_t *pixel=render->atlas[0].px+((y+3u)*ATLAS+x+3u)*4u;
    pixel[0]=(uint8_t)(40u+x*35u);
    pixel[1]=(uint8_t)(200u-y*30u);
    pixel[2]=(uint8_t)(60u+(x+y)*15u);
    pixel[3]=(uint8_t)((x+y)%5u*63u);
  }
  GmlFont *font=&render->fonts[0];
  font->real=1; font->sprite=-1; font->atlas=0;
  font->line_height=font->align_height=5;
  font->glyphs=(GmlGlyph*)calloc(1,sizeof *font->glyphs);
  CHECK(font->glyphs);
  for(int index=0;index<256;index++) font->glyph_by_char[index]=-1;
  font->n_glyphs=1;
  font->glyphs[0]=(GmlGlyph){.sx=3,.sy=3,.w=5,.h=5,.shift=5,.ch='A'};
  font->glyph_by_char['A']=0;
  render->font=0;
  render->color_write_mask=0x0Fu;
  render->interp=1;
  render->win=&content;
  /* Distinct behaviors, rather than a Cartesian GUI-state matrix: identity,
   * nonuniform mapping, partial destination alpha, multiline alignment,
   * clipped coverage, world alpha, a mid-run size change, ordered exclusions
   * and a pending application underlay with its exact resident source recipe. */
  for(unsigned modern=0;modern<2u;modern++) for(unsigned variant=0;variant<15u;variant++){
    const uint32_t background=variant==0u || variant==3u?0xFF123456u:
                              variant==2u?0x40123456u:0x00123456u;
    const double x=variant==4u?-0.35:2.125,y=1.75;
    for(unsigned accelerated=0;accelerated<2u;accelerated++){
      FakeDevice fake={0};
      GmlRenderDeviceServices device=service(&fake);
      uint32_t *pixels=accelerated?screen:reference;
      content.bytecode=modern?17:15;
      gml_render_bind_device(render,accelerated?&device:NULL);
      if(variant>=13u){
        CHECK(gml_render_application_surface_ensure_owned(render,WIDTH,HEIGHT));
        if(accelerated) CHECK(gml_render_begin_resident_owned_application(render,0.0,0.0,0));
        else CHECK(gml_render_begin_owned_application(render,0.0,0.0,0));
        gml_render_set_pending_fill(render,0xFF25C080u);
        if(variant==14u)
          gml_draw_text_transformed(render,2.125,1.75,"A",0.75,0.75,0.0,0x80C040u,0.7);
        gml_render_flush_pending_fill(render);
        if(accelerated) CHECK(gml_render_scene_flush(render));
        render->app_surface_opaque=modern && variant==14u?0:1;
      }
      for(unsigned index=0;index<WIDTH*HEIGHT;index++)
        pixels[index]=sentinel[index]=0xA5123456u;
      if(accelerated)
        CHECK(gml_render_begin_resident_base(render,pixels,WIDTH,HEIGHT,0.0,0.0));
      else gml_render_begin(render,pixels,WIDTH,HEIGHT,0.0,0.0);
      render->halign=variant==3u?1:0;
      render->valign=0;
      font->runtime_owned=0;
      font->sdf_spread=0;
      if(variant!=5u){
        gml_render_gui_begin(render,WIDTH,HEIGHT);
        if(variant!=0u) gml_render_gui_set_size(render,10,7);
      }
      const double xs=0.75/render->gui_scale_x;
      const double ys=0.7555555555555555/render->gui_scale_y;
      gml_render_set_pending_fill(render,background);
      if(variant>=13u) gml_render_set_pending_underlay(render,0,0,WIDTH,HEIGHT);
      gml_draw_text_transformed(render,x,y,variant==3u?"AA#A":"A",
                                xs,ys,0.0,0u,0.8);
      if(variant==6u) gml_render_gui_set_size(render,12,9);
      if(variant==8u)
        gml_render_gui_set_maximise(render,1,1.2,1.3,0.25,0.75,WIDTH,HEIGHT);
      if(variant==9u){
        render->world_transform_active=1;
        render->world_scale_x=render->world_scale_y=2.0;
      }
      if(variant==11u) font->runtime_owned=1;
      if(variant==12u) font->sdf_spread=1;
      gml_draw_text_transformed(render,x+0.25,y+0.25,"A",
        0.75/render->gui_scale_x,0.7555555555555555/render->gui_scale_y,
        variant==7u?30.0:0.0,0x80C040u,variant==10u?0x1p-40:0.7);
      if(!accelerated){
        unsigned visible=0;
        gml_render_flush_pending_fill(render);
        for(unsigned index=0;index<WIDTH*HEIGHT;index++)
          if(reference[index]!=background) visible++;
        CHECK(visible>0u);
      } else {
        GmlRenderJournalMetrics before={0},after={0};
        CHECK(render->resident_application_pass==(variant<7u || variant>=13u));
        CHECK(gml_render_scene_flush(render) && fake.submits>0 && fake.readbacks==0);
        gml_render_resource_journal_metrics(render->base_scene_resource,&before);
        if(variant<7u || variant>=13u){
          CHECK(before.authority==GML_RENDER_RESOURCE_GPU_REPLAYABLE &&
                before.journal_operations>=3u && before.retained_mutable_bytes==0u);
          CHECK(!memcmp(screen,sentinel,sizeof screen));
        }
        fake.forbid_calls=1;
        gml_render_abandon_device(render);
        CHECK(gml_render_completed_screen_read(render,screen,WIDTH,HEIGHT));
        if(memcmp(screen,reference,sizeof screen))
          for(unsigned index=0;index<WIDTH*HEIGHT;index++) if(screen[index]!=reference[index]){
            fprintf(stderr,"filtered GUI font: modern=%u variant=%u pixel=(%u,%u) CPU=%08x replay=%08x\n",
                    modern,variant,index%WIDTH,index/WIDTH,reference[index],screen[index]);
            break;
          }
        CHECK(!memcmp(screen,reference,sizeof screen));
        gml_render_resource_journal_metrics(render->base_scene_resource,&after);
        CHECK(fake.readbacks==0 && fake.forbidden_calls==0 &&
              after.cpu_version==after.logical_version &&
              ((variant>=7u && variant<13u) || after.cpu_replays>before.cpu_replays));
      }
      gml_render_gui_end(render);
    }
  }
  gml_render_free(render);
  free(render);
  return 1;
}

static int surface_pending_source_lifetime(int nearest,int destination_mode){
  for(unsigned interpolation=0;interpolation<(nearest?2u:1u);interpolation++)
  for(unsigned scale=0;scale<(destination_mode?3u:1u);scale++)
  for(unsigned variant=0;variant<6u;variant++){
    GmlRender *render=(GmlRender*)calloc(1,sizeof *render);
    GmlWin content={.bytecode=15};
    FakeDevice fake={.on_demand=1};
    GmlRenderDeviceServices device=service(&fake);
    uint32_t pixels[16];
    device.observe_on_demand=fake_observe_on_demand;
    CHECK(render && gml_render_init(render,NULL)==0);
    render->win=&content;
    CHECK(gml_render_application_surface_ensure_owned(render,48,32));
    int source=gml_surface_create(render,4,4);
    CHECK(source>0);
    for(unsigned pixel=0;pixel<16u;pixel++) pixels[pixel]=0x7F1267B9u;
    CHECK(gml_render_surface_mirror_pixels(render,source,pixels,4,4,0));
    gml_render_bind_device(render,&device);
    CHECK(gml_render_begin_resident_owned_application(render,0,0,0));
    render->interp=nearest?(int)interpolation:1;
    render->alphablend=1;
    render->blendmode=destination_mode;
    gml_render_set_pending_fill(render,0x40123456u);
    gml_render_flush_pending_fill(render);
    int submits=fake.submits;
    for(unsigned draw=0;draw<8u;draw++)
      gml_draw_surface_stretched(render,source,3.25,4.125,
                                 scale==1u?3.0:scale==2u?5.0:nearest?4.0:17.3,
                                 scale==1u?2.0:scale==2u?6.0:nearest?4.0:13.1,0x3267B9u,.5);
    if(!render->resident_application_pass || fake.submits!=submits || fake.readbacks)
      fprintf(stderr,"surface lifetime: nearest=%d interp=%u variant=%u pass=%d submits=%d/%d reads=%d\n",
        nearest,interpolation,variant,render->resident_application_pass,fake.submits,submits,fake.readbacks);
    CHECK(render->resident_application_pass && fake.submits==submits && !fake.readbacks);
    if(destination_mode){
      GmlRenderResource *target=gml_render_scene_target_resource_ensure(render);
      GmlRenderImageRef prefix=gml_render_application_scene_next_image_ref(render,target);
      int uploads=fake.uploads;
      double w=scale==1u?3.0:scale==2u?5.0:4.0;
      double h=scale==1u?2.0:scale==2u?6.0:4.0;
      for(unsigned edge=0;edge<4u;edge++){
        double x=edge==0u?48.25:edge==1u?-w+.25:3.25;
        double y=edge==2u?32.125:edge==3u?-h+.125:4.125;
        gml_draw_surface_stretched(render,source,x,y,w,h,0x3267B9u,.5);
        GmlRenderImageRef after=gml_render_application_scene_next_image_ref(render,target);
        CHECK(render->resident_application_pass && prefix.content_version==after.content_version &&
          fake.submits==submits && fake.uploads==uploads && !fake.readbacks);
      }
    }
    if(variant==5u){
      gml_render_free(render);free(render);
      CHECK(fake.submits==submits+1 && fake.last_batch_operations==8u && !fake.readbacks);
      continue;
    }
    if(variant==0u){
      pixels[0]=0xFF2468ACu;
      CHECK(gml_render_surface_mirror_pixels(render,source,pixels,4,4,0));
    } else if(variant==1u) gml_surface_resize(render,source,5,5);
    else if(variant==2u) gml_surface_free(render,source);
    else if(variant==3u) CHECK(gml_surface_set_target(render,source));
    else {
      fake.status=GML_RENDER_EXEC_UNSUPPORTED;fake.prewrite_refusal=1;
      CHECK(!gml_render_scene_submit_pending(render));
      /* The existing pre-write refusal seals the prior destination for ordered fallback. */
      CHECK(gml_surface_exists(render,source) && fake.readbacks==1);
      fake.status=0;
      CHECK(gml_render_scene_submit_pending(render));
    }
    CHECK(fake.submits==submits+(variant==4u?2:1) &&
          fake.last_batch_operations==8u && fake.readbacks==(variant==4u?1:0));
    if(destination_mode){
      CHECK(fake.last_filtered_draw_alpha==.5 &&
        fake.last_filter==(scale && interpolation?GML_RENDER_FILTER_BOX:GML_RENDER_FILTER_NEAREST) &&
        fake.last_quad_state.coverage_policy==0u &&
        fake.last_quad_state.rgb_equation==GML_RENDER_BLEND_EXACT_DESTINATION_ADD &&
        fake.last_quad_state.alpha_equation==GML_RENDER_BLEND_EXACT_DESTINATION_ADD &&
        fake.last_quad_state.rgb_source_factor==(destination_mode==11?
          GML_RENDER_BLEND_DESTINATION_ALPHA:GML_RENDER_BLEND_DESTINATION_COLOUR) &&
        fake.last_quad_state.rgb_destination_factor==(destination_mode==9?
          GML_RENDER_BLEND_SOURCE_COLOUR:GML_RENDER_BLEND_DESTINATION_COLOUR));
    }
    CHECK(gml_render_scene_submit_pending(render) &&
          fake.submits==submits+(variant==4u?2:1));
    if(variant==3u) gml_surface_reset_target(render);
    gml_render_free(render);free(render);
  }
  return 1;
}

static int clipped_destination_surface_after_cpu_suffix(void){
  for(unsigned numbered=0;numbered<2u;numbered++)
  for(unsigned interpolation=0;interpolation<2u;interpolation++)
  for(int mode=9;mode<=11;mode++)
  for(unsigned scale=0;scale<3u;scale++){
    GmlRender *render=calloc(1,sizeof *render);
    GmlWin content={.bytecode=15};
    FakeDevice fake={.on_demand=1,.readback_colour=0x7F1267B9u};
    GmlRenderDeviceServices device=service(&fake);
    GmlRenderJournalMetrics source_before={0},target_before={0},after={0};
    uint32_t before[48*32];
    device.observe_on_demand=fake_observe_on_demand;
    CHECK(render && gml_render_init(render,NULL)==0);
    render->win=&content;
    CHECK(gml_render_application_surface_ensure_owned(render,48,32));
    int source=gml_surface_create(render,4,4);
    int destination=numbered?gml_surface_create(render,48,32):0;
    CHECK(source>0 && (!numbered || destination>0));
    gml_render_bind_device(render,&device);
    CHECK(gml_render_begin_resident_owned_application(render,0,0,0));
    CHECK(gml_surface_set_target(render,source));
    gml_render_set_pending_fill(render,0x7F1267B9u);
    gml_render_flush_pending_fill(render);
    gml_surface_reset_target(render);
    if(numbered) CHECK(gml_surface_set_target(render,destination));
    CHECK(gml_render_resident_application_cpu_barrier(render));
    CHECK(render->resident_scene_pass && !render->resident_application_pass);
    for(unsigned pixel=0;pixel<48u*32u;pixel++) render->fb[pixel]=0x40123456u;
    memcpy(before,render->fb,sizeof before);
    GmlRenderResource *source_resource=gml_render_surface_resource_ensure(render,source);
    GmlRenderResource *target=gml_render_scene_target_resource_ensure(render);
    CHECK(source_resource && target);
    gml_render_resource_journal_metrics(source_resource,&source_before);
    gml_render_resource_journal_metrics(target,&target_before);
    CHECK(source_before.authority==GML_RENDER_RESOURCE_GPU_RESIDENT &&
          target_before.authority==GML_RENDER_RESOURCE_CPU_CURRENT);
    render->interp=(int)interpolation;
    render->alphablend=1;render->blendmode=mode;
    double w=scale==1u?3.0:scale==2u?5.0:4.0;
    double h=scale==1u?2.0:scale==2u?6.0:4.0;
    int reads=fake.readbacks,uploads=fake.uploads,submits=fake.submits;
    fake.forbid_calls=1;
    for(unsigned edge=0;edge<4u;edge++){
      double x=edge==0u?48.25:edge==1u?-w+.25:3.25;
      double y=edge==2u?32.125:edge==3u?-h+.125:4.125;
      gml_draw_surface_stretched(render,source,x,y,w,h,0x3267B9u,.5);
      CHECK(!fake.forbidden_calls && fake.readbacks==reads && fake.uploads==uploads &&
            fake.submits==submits && !render->resident_application_pass &&
            !memcmp(before,render->fb,sizeof before));
      gml_render_resource_journal_metrics(source_resource,&after);
      CHECK(after.authority==source_before.authority &&
            after.logical_version==source_before.logical_version &&
            after.cpu_version==source_before.cpu_version &&
            after.gpu_version==source_before.gpu_version);
      gml_render_resource_journal_metrics(target,&after);
      CHECK(after.authority==target_before.authority &&
            after.logical_version==target_before.logical_version &&
            after.cpu_version==target_before.cpu_version);
    }
    /* One covered column is not a no-op: preserve the established CPU suffix
     * and observe the device-current source exactly once, without GPU admission. */
    fake.forbid_calls=0;
    gml_draw_surface_stretched(render,source,47.25,4.125,w,h,0x3267B9u,.5);
    CHECK(fake.readbacks==reads+1 && fake.submits==submits &&
          !render->resident_application_pass && memcmp(before,render->fb,sizeof before));
    if(numbered) gml_surface_reset_target(render);
    gml_render_free(render);free(render);
  }
  return 1;
}

static int application_surface_pending_source_lifetime(void){
  for(unsigned variant=0;variant<3u;variant++){
    GmlRender *render=calloc(1,sizeof *render);
    GmlWin content={.bytecode=15};
    FakeDevice fake={.on_demand=1};
    GmlRenderDeviceServices device=service(&fake);
    GmlRenderApplicationWriteView view={0};
    uint32_t screen[48*32]={0};
    device.observe_on_demand=fake_observe_on_demand;
    CHECK(render && gml_render_init(render,NULL)==0);
    render->win=&content;
    CHECK(gml_render_application_surface_ensure_owned(render,4,4));
    gml_render_application_surface_publish(render);
    for(unsigned pixel=0;pixel<16u;pixel++) render->app_surface_owned[pixel]=0xFF1267B9u;
    gml_render_bind_device(render,&device);
    CHECK(gml_render_begin_resident_base(render,screen,48,32,0,0));
    gml_render_set_pending_fill(render,0x40123456u);
    gml_render_flush_pending_fill(render);
    int submits=fake.submits;
    for(unsigned draw=0;draw<8u;draw++)
      gml_draw_surface_stretched(render,0,3.25,4.125,4,4,0x3267B9u,.5);
    CHECK(render->resident_application_pass && fake.submits==submits && !fake.readbacks);
    if(variant==0u){
      CHECK(gml_render_application_surface_owned_view(render,&view));
      view.pixels[0]=0xFF2468ACu;
    } else if(variant==1u){
      CHECK(gml_render_application_surface_ensure_owned(render,5,5));
    }
    gml_render_free(render);free(render);
    CHECK(fake.submits==submits+1 && fake.last_batch_operations==8u && !fake.readbacks);
  }
  return 1;
}

/* Content-owned point presentation followed by a filtered alpha surface draw
 * must not acquire SCREEN or the application on the CPU between the draws. */
static int resident_gui_application_composition(void){
  const double geometry[][4]={{0,0,48,32},{3,4,17,13},{3.25,4.125,17.25,13.5},
                              {-4.25,-3.125,17.25,13.5},{2,3,4,3},{0,0,48,32}};
  for(unsigned variant=0;variant<sizeof geometry/sizeof *geometry;variant++){
    GmlRender *render=calloc(1,sizeof *render);
    GmlWin content={.bytecode=15};
    FakeDevice fake={.on_demand=1};
    GmlRenderDeviceServices device=service(&fake);
    uint32_t screen[48*32]={0};
    device.observe_on_demand=fake_observe_on_demand;
    CHECK(render && gml_render_init(render,NULL)==0);
    render->win=&content;
    CHECK(gml_render_application_surface_ensure_owned(render,6,4));
    gml_render_application_surface_publish(render);
    gml_render_application_surface_set_draw_enabled(render,0);
    render->app_surface_opaque=1;
    for(unsigned pixel=0;pixel<24u;pixel++)
      render->app_surface_owned[pixel]=0xFF1267B9u;
    gml_render_bind_device(render,&device);
    CHECK(gml_render_begin_resident_base(render,screen,48,32,0,0));
    gml_render_gui_begin(render,48,32);
    if(variant==5u) gml_render_gui_set_size(render,24,16);
    gml_render_set_pending_fill(render,0x40123456u);
    render->interp=0;render->alphablend=1;
    int submits=fake.submits,reads=fake.readbacks;
    gml_draw_surface_stretched(render,0,geometry[variant][0],geometry[variant][1],
      geometry[variant][2],geometry[variant][3],0xFFFFFFu,1.0);
    CHECK(render->resident_application_pass && !render->pending_presentation &&
          fake.readbacks==reads);
    CHECK(gml_render_scene_submit_pending(render));
    CHECK(fake.submits>submits && fake.last_opcode==GML_RENDER_OP_TEXTURED_QUAD &&
          fake.last_filtered_draw_alpha==1.0 && fake.readbacks==reads);
    if(variant<2u || variant==5u){
      submits=fake.submits;
      render->interp=1;
      for(unsigned draw=0;draw<8u;draw++)
        gml_draw_surface_stretched(render,0,0,0,48,32,0xFFFFFFu,.7);
      CHECK(render->resident_application_pass && fake.submits==submits &&
            fake.readbacks==reads);
      CHECK(gml_render_scene_submit_pending(render) && fake.submits==submits+1 &&
            fake.last_batch_operations==8u &&
            fake.last_filter==GML_RENDER_FILTER_HARDWARE_BILINEAR &&
            fake.last_filtered_draw_alpha==.7 && fake.readbacks==reads);
    }
    gml_render_gui_end(render);
    gml_render_free(render);free(render);
  }
  return 1;
}

static int resident_fullwidth_gui_application(void){
  for(unsigned variant=0;variant<11u;variant++)
  for(unsigned gui=0;gui<2u;gui++) for(unsigned camera=0;camera<2u;camera++){
    GmlRender *render=calloc(1,sizeof *render);
    GmlWin content={.bytecode=15};
    FakeDevice fake={.on_demand=1};
    GmlRenderDeviceServices device=service(&fake);
    uint32_t screen[48*32]={0};
    const int source_width=variant>=8u?48:variant==1u?8:6;
    const int source_height=variant>=8u?32:4;
    device.observe_on_demand=fake_observe_on_demand;
    CHECK(render && gml_render_init(render,NULL)==0);
    render->win=&content;
    CHECK(gml_render_application_surface_ensure_owned(render,source_width,source_height));
    gml_render_application_surface_publish(render);
    gml_render_application_surface_set_draw_enabled(render,0);
    render->app_surface_opaque=1;
    render->aspect_fullwidth=1;
    render->aspect_wide_w=source_width;render->aspect_wide_h=source_height;
    for(int pixel=0;pixel<source_width*source_height;pixel++)
      render->app_surface_owned[pixel]=0xFF1267B9u;
    gml_render_bind_device(render,&device);
    CHECK(gml_render_begin_resident_base(render,screen,48,32,0,0));
    gml_render_gui_begin(render,48,32);
    if(gui) gml_render_gui_set_size(render,24,16);
    render->cam_x=camera?1.25:0.0;render->cam_y=camera?-.75:0.0;
    gml_render_set_pending_fill(render,0x40123456u);
    gml_render_flush_pending_fill(render);
    CHECK(gml_render_scene_submit_pending(render));
    render->interp=variant==5u || variant==8u;render->alphablend=1;
    if(variant==7u) render->color_write_mask=7u;
    const double dw=variant==2u || variant==9u?27:variant==3u?45:36;
    const double dh=variant==2u || variant==9u?24:variant==3u?40:32;
    const double dy=variant==10u?1.75:variant==2u?4:0;
    const int submits=fake.submits,reads=fake.readbacks;
    gml_draw_surface_stretched(render,0,(5.0+render->cam_x)/render->gui_scale_x,
      (dy+render->cam_y)/render->gui_scale_y,dw/render->gui_scale_x,dh/render->gui_scale_y,
      variant==6u?0x123456u:0xFFFFFFu,variant==10u?.002:variant==4u || variant>=8u?.7:1.0);
    if(variant<4u || variant==8u || variant==10u){
      CHECK(render->resident_application_pass && fake.submits==submits &&
            fake.readbacks==reads && !render->pending_presentation);
      const int opaque=variant==0u || variant==3u || variant==8u;
      CHECK(render->fb_opaque_known==opaque && render->fb_all_opaque==opaque);
      CHECK(gml_render_scene_submit_pending(render) && fake.submits==submits+1 &&
            fake.readbacks==reads && fake.last_batch_operations==1u &&
            fake.last_filter==(variant>=8u?GML_RENDER_FILTER_NEAREST:
                                         GML_RENDER_FILTER_HARDWARE_PIXEL_CENTRE_POINT));
      CHECK(render->fb_opaque_known==opaque && render->fb_all_opaque==opaque);
      const double x=variant==2u?6:0,y=variant==10u?1:variant==1u || variant==2u?4:0;
      const double width=variant==2u?36:48,height=variant==1u || variant==2u?24:32;
      CHECK(fake.last_quad.x[0]==x && fake.last_quad.y[0]==y &&
            fake.last_quad.x[1]-fake.last_quad.x[0]==width &&
            fake.last_quad.y[2]-fake.last_quad.y[0]==height);
      if(variant<4u)
        CHECK(fake.last_quad.inverse_anchor[0]==x &&
              fake.last_quad.inverse_anchor[1]==y &&
              fake.last_quad.inverse_x[0]==width && fake.last_quad.inverse_y[1]==height);
    } else CHECK(!render->resident_application_pass && fake.readbacks>reads);
    gml_render_gui_end(render);
    gml_render_free(render);free(render);
  }
  return 1;
}

static int current_cpu_source_view_is_not_observer(void){
  uint32_t pixels[16],*writable=NULL;
  const uint32_t *observed=NULL;
  GmlRenderResourceSource view={0};
  FakeDevice fake={.on_demand=1,.readback_colour=0x7F25C080u};
  GmlRenderDeviceServices device=service(&fake);
  GmlRenderDrawOp operation=clear_op(2u,0x7F25C080u,GML_RENDER_REPLAY_EXACT);
  GmlRenderDrawBatch batch={&operation,1u,NULL,0u};
  GmlRenderResource *resource;
  device.observe_on_demand=fake_observe_on_demand;
  fill(pixels,0xA5123456u);
  resource=gml_render_resource_create(target_ref(1u,1u),4u,4u,
    GML_RENDER_PIXEL_XRGB8888,pixels,(GmlRenderResourceLimits){0});
  CHECK(resource && gml_render_resource_current_cpu_source_view(resource,&view) &&
        view.bytes==pixels && view.image.content_version==1u && !fake.readbacks);
  CHECK(gml_render_resource_acquire_cpu_write(resource,&writable) && writable==pixels);
  CHECK(!gml_render_resource_current_cpu_source_view(resource,&view) &&
        !view.bytes && !view.resident_resource && !fake.readbacks);
  CHECK(gml_render_resource_commit_cpu_write(resource) &&
        gml_render_resource_current_cpu_source_view(resource,&view) &&
        view.image.content_version==2u && view.bytes==pixels);
  operation.target.image=gml_render_resource_image_ref(resource);
  operation.target.image.content_version++;
  CHECK(gml_render_resource_submit_deferred(resource,&device,&batch,NULL,0u)==
        GML_RENDER_RESOURCE_EXEC_DEVICE);
  int submits=fake.submits,uploads=fake.uploads;
  CHECK(!gml_render_resource_current_cpu_source_view(resource,&view) && !view.bytes &&
        !fake.readbacks && fake.submits==submits && fake.uploads==uploads);
  fake.fail_readback=1;
  CHECK(!gml_render_resource_acquire_cpu_read(resource,&observed) && !observed);
  int readbacks=fake.readbacks;
  CHECK(!gml_render_resource_current_cpu_source_view(resource,&view) && !view.bytes &&
        fake.readbacks==readbacks && fake.submits==submits && fake.uploads==uploads);
  fake.fail_readback=0;
  CHECK(gml_render_resource_acquire_cpu_read(resource,&observed) && observed==pixels &&
        gml_render_resource_current_cpu_source_view(resource,&view) && view.bytes==pixels &&
        view.image.content_version==3u);
  readbacks=fake.readbacks;
  gml_render_resource_context_lost(resource);
  CHECK(gml_render_resource_current_cpu_source_view(resource,&view) && view.bytes==pixels &&
        fake.readbacks==readbacks && fake.submits==submits && fake.uploads==uploads);
  gml_render_resource_destroy(resource);
  return 1;
}

static int resident_nearest_surface_cutout_targets(void){
  enum { WIDTH=24,HEIGHT=18,SOURCE_WIDTH=8,SOURCE_HEIGHT=6 };
  for(unsigned modern=0;modern<2u;modern++) for(unsigned destination=0;destination<4u;destination++){
    GmlRender *render=(GmlRender*)calloc(1,sizeof *render);
    GmlWin content={.bytecode=modern?17:15};
    uint32_t screen[WIDTH*HEIGHT];
    FakeDevice fake={.on_demand=1};
    GmlRenderDeviceServices device=service(&fake);
    GmlRenderJournalMetrics metrics={0};
    CHECK(render && gml_render_init(render,NULL)==0);
    render->win=&content;
    for(unsigned pixel=0;pixel<WIDTH*HEIGHT;pixel++) screen[pixel]=0xA5123456u;
    gml_render_begin(render,screen,WIDTH,HEIGHT,0.0,0.0);
    int source=gml_surface_create(render,SOURCE_WIDTH,SOURCE_HEIGHT);
    CHECK(source>0);
    GmlSurface *surface=&render->surface[surface_slot(source)];
    for(unsigned pixel=0;pixel<SOURCE_WIDTH*SOURCE_HEIGHT;pixel++)
      surface->px[pixel]=((pixel%3u==0u?0u:pixel%3u==1u?127u:255u)<<24)|
        ((pixel*31u+17u)&0xFFFFFFu);
    surface->opaque_known=surface->all_opaque=surface->all_transparent=0;
    device.observe_on_demand=fake_observe_on_demand;
    gml_render_bind_device(render,&device);
    CHECK(gml_render_begin_resident_base(render,screen,WIDTH,HEIGHT,0.0,0.0));
    if(destination==1u) gml_render_gui_begin(render,WIDTH,HEIGHT);
    if(destination==2u){
      CHECK(gml_render_application_surface_ensure_owned(render,WIDTH,HEIGHT) &&
        gml_render_begin_resident_owned_application(render,0.0,0.0,0));
    }
    if(destination==3u){
      int target=gml_surface_create(render,WIDTH,HEIGHT);
      CHECK(target>0 && target!=source && gml_surface_set_target(render,target));
    }
    CHECK(render->resident_application_pass);
    gml_render_set_pending_fill(render,0xA5714A24u);
    render->interp=0;
    gml_draw_surface_stretched(render,source,-2.25,1.4,8.0,6.0,0xFFFFFFu,1.0);
    CHECK(render->resident_application_pass && gml_render_scene_submit_pending(render));
    render->alpha_test_enable=1;render->alpha_test_ref=2u;
    int reads=fake.readbacks,submits=fake.submits;
    gml_draw_surface_stretched(render,source,-2.25,1.4,8.0,6.0,0xFFFFFFu,1.0);
    if(!render->resident_application_pass)
      fprintf(stderr,"nearest surface cutout: modern=%u destination=%u ordinary path lost residence\n",
        modern,destination);
    CHECK(render->resident_application_pass && gml_render_scene_submit_pending(render));
    GmlRenderResource *target=gml_render_scene_target_resource_ensure(render);
    CHECK(target);
    gml_render_resource_journal_metrics(target,&metrics);
    CHECK(fake.submits>submits && fake.readbacks==reads &&
      metrics.authority==GML_RENDER_RESOURCE_GPU_RESIDENT && !metrics.cpu_replays &&
      !metrics.journal_operations && metrics.cpu_version<metrics.logical_version);
    gml_render_abandon_device(render);gml_render_free(render);free(render);
  }
  return 1;
}

static int resident_current_cpu_application_underlay(void){
  enum { WIDTH=4,HEIGHT=4 };
  for(unsigned modern=0;modern<2u;modern++) for(unsigned variant=0;variant<6u;variant++){
    uint32_t screen[WIDTH*HEIGHT],sentinel[WIDTH*HEIGHT],*writable=NULL;
    GmlRender *render=(GmlRender*)calloc(1,sizeof *render);
    GmlWin content={.bytecode=modern?17:15};
    FakeDevice fake={.on_demand=variant!=1u};
    GmlRenderDeviceServices device=service(&fake);
    GmlRenderJournalMetrics source_before={0},source_after={0},target={0};
    CHECK(render);
    device.observe_on_demand=variant==2u?NULL:fake_observe_on_demand;
    render->win=&content;render->color_write_mask=15u;
    CHECK(gml_render_application_surface_ensure_owned(render,WIDTH,HEIGHT) &&
          gml_render_begin_owned_application(render,0.0,0.0,0));
    gml_render_set_pending_fill(render,0x7F25C080u);
    gml_render_flush_pending_fill(render);
    CHECK(gml_render_application_resource_ensure(render));
    gml_render_resource_journal_metrics(render->app_surface_resource,&source_before);
    CHECK(source_before.authority==GML_RENDER_RESOURCE_CPU_CURRENT &&
          source_before.cpu_version==source_before.logical_version);
    gml_render_bind_device(render,&device);
    for(unsigned index=0;index<WIDTH*HEIGHT;index++)
      screen[index]=sentinel[index]=0xA5123456u;
    CHECK(gml_render_begin_resident_base(render,screen,WIDTH,HEIGHT,0.0,0.0));
    gml_render_set_pending_fill(render,0x40123456u);
    gml_render_set_pending_underlay(render,0,0,WIDTH,HEIGHT);
    if(variant==4u) render->underlay_w=WIDTH/2;
    if(variant==5u) render->underlay_h=HEIGHT/2;
    if(variant==3u)
      CHECK(gml_render_resource_acquire_cpu_write(render->app_surface_resource,&writable));
    int submits=fake.submits,uploads=fake.uploads;
    if(variant!=0u){
      CHECK(!gml_render_compose_resident_underlay(render) && render->pending_underlay &&
            fake.submits==submits && fake.uploads==uploads && !fake.readbacks);
      if(variant==3u){
        CHECK(gml_render_resource_commit_cpu_write(render->app_surface_resource));
        gml_render_resource_journal_metrics(render->app_surface_resource,&source_before);
      }
    }
    if(variant==0u || variant==3u){
      CHECK(gml_render_compose_resident_underlay(render) && !render->pending_underlay &&
            render->resident_application_pass && gml_render_scene_submit_pending(render));
      gml_render_resource_journal_metrics(render->base_scene_resource,&target);
      fprintf(stderr,"CPU-source underlay control: modern=%u variant=%u submits=%d/%d "
              "uploads=%d/%d reads=%d opcode=%u authority=%u versions=%llu/%llu/%llu "
              "journal=%zu retained=%zu replays=%llu shadow_equal=%d\n",
              modern,variant,fake.submits,submits,fake.uploads,uploads,fake.readbacks,
              fake.last_opcode,target.authority,(unsigned long long)target.cpu_version,
              (unsigned long long)target.gpu_version,(unsigned long long)target.logical_version,
              target.journal_bytes,target.retained_mutable_bytes,
              (unsigned long long)target.cpu_replays,!memcmp(screen,sentinel,sizeof screen));
      CHECK(fake.submits>submits && fake.uploads>uploads && !fake.readbacks &&
            fake.last_opcode==GML_RENDER_OP_TEXTURED_QUAD &&
            target.authority==GML_RENDER_RESOURCE_GPU_RESIDENT &&
            target.gpu_version==target.logical_version &&
            target.cpu_version<target.logical_version && !target.journal_bytes &&
            !target.retained_mutable_bytes && !target.cpu_replays &&
            !memcmp(screen,sentinel,sizeof screen));
    }
    gml_render_resource_journal_metrics(render->app_surface_resource,&source_after);
    CHECK(source_after.authority==GML_RENDER_RESOURCE_CPU_CURRENT &&
          source_after.logical_version==source_before.logical_version &&
          source_after.cpu_version==source_before.cpu_version && !source_after.cpu_replays &&
          !source_after.device_checkpoints && !fake.readbacks);
    fake.forbid_calls=1;
    gml_render_abandon_device(render);
    CHECK(!fake.forbidden_calls);
    gml_render_free(render);free(render);
  }
  return 1;
}

static int resident_unregistered_cpu_application_underlay(void){
  enum { WIDTH=4,HEIGHT=4 };
  for(unsigned modern=0;modern<2u;modern++) for(unsigned witness=0;witness<2u;witness++){
    uint32_t screen[WIDTH*HEIGHT],sentinel[WIDTH*HEIGHT];
    GmlRender *render=(GmlRender*)calloc(1,sizeof *render);
    GmlWin content={.bytecode=modern?17:15};
    FakeDevice fake={.on_demand=witness!=0u};
    GmlRenderDeviceServices device=service(&fake);
    GmlRenderJournalMetrics source={0},target={0};
    CHECK(render);
    device.observe_on_demand=fake_observe_on_demand;
    render->win=&content;render->color_write_mask=15u;
    CHECK(gml_render_application_surface_ensure_owned(render,WIDTH,HEIGHT) &&
          gml_render_begin_owned_application(render,0.0,0.0,0));
    gml_render_set_pending_fill(render,0xFF25C080u);
    gml_render_flush_pending_fill(render);
    render->app_surface_opaque=1;
    CHECK(!render->app_surface_resource);
    for(unsigned index=0;index<WIDTH*HEIGHT;index++)
      screen[index]=sentinel[index]=0xA5123456u;
    gml_render_bind_device(render,&device);
    CHECK(gml_render_begin_resident_base(render,screen,WIDTH,HEIGHT,0.0,0.0));
    gml_render_set_pending_fill(render,0x40123456u);
    gml_render_set_pending_underlay(render,0,0,WIDTH,HEIGHT);
    if(!witness){
      CHECK(!gml_render_compose_resident_underlay(render) && render->pending_underlay &&
            !render->app_surface_resource && !fake.submits && !fake.uploads && !fake.readbacks);
      fake.forbid_calls=1;
      gml_render_abandon_device(render);
      CHECK(!fake.forbidden_calls);
      gml_render_free(render);free(render);
      continue;
    }
    CHECK(gml_render_compose_resident_underlay(render) &&
          !render->pending_underlay && render->resident_application_pass &&
          gml_render_scene_submit_pending(render));
    CHECK(render->app_surface_resource);
    gml_render_resource_journal_metrics(render->app_surface_resource,&source);
    gml_render_resource_journal_metrics(render->base_scene_resource,&target);
    CHECK(source.authority==GML_RENDER_RESOURCE_CPU_CURRENT &&
          source.cpu_version==source.logical_version &&
          target.authority==GML_RENDER_RESOURCE_GPU_RESIDENT &&
          target.gpu_version==target.logical_version &&
          fake.last_opcode==GML_RENDER_OP_TEXTURED_QUAD &&
          fake.uploads>0 && !fake.readbacks && !memcmp(screen,sentinel,sizeof screen));
    /* This fake device proves admission/authority, not actual GL raster output. */
    fake.forbid_calls=1;
    gml_render_abandon_device(render);
    CHECK(!fake.forbidden_calls);
    gml_render_free(render);free(render);
  }
  return 1;
}

static int resident_surface_after_pending_underlay(void){
  enum { WIDTH=8,HEIGHT=8 };
  for(unsigned modern=0;modern<2u;modern++) for(unsigned gpu_source=0;gpu_source<2u;gpu_source++)
  for(unsigned numbered=0;numbered<2u;numbered++) for(unsigned filtered=0;filtered<2u;filtered++)
  for(unsigned excluded=0;excluded<3u;excluded++){
    GmlWin content={.bytecode=modern?17:15};
    GmlRender *render=calloc(1,sizeof *render);
    FakeDevice fake={.on_demand=1};
    GmlRenderDeviceServices device=service(&fake);
    GmlRenderJournalMetrics target={0};
    uint32_t screen[WIDTH*HEIGHT],sentinel[WIDTH*HEIGHT];
    CHECK(render && !gml_render_init(render,NULL));
    device.observe_on_demand=fake_observe_on_demand;
    render->win=&content;render->color_write_mask=15u;
    CHECK(gml_render_application_surface_ensure_owned(render,WIDTH,HEIGHT));
    if(gpu_source){
      gml_render_bind_device(render,&device);
      CHECK(gml_render_begin_resident_owned_application(render,0,0,0));
    } else CHECK(gml_render_begin_owned_application(render,0,0,0));
    gml_render_set_pending_fill(render,0xFF25C080u);
    gml_render_flush_pending_fill(render);
    CHECK(!gpu_source || gml_render_scene_submit_pending(render));
    render->app_surface_opaque=1;
    gml_render_application_surface_set_draw_enabled(render,0);
    gml_render_application_surface_publish(render);
    int surface=0;
    if(numbered){
      surface=gml_surface_create(render,WIDTH,HEIGHT);CHECK(surface>0);
      GmlSurface *source=&render->surface[surface_slot(surface)];
      for(unsigned pixel=0;pixel<WIDTH*HEIGHT;pixel++) source->px[pixel]=0xFF1267B9u;
      source->opaque_known=source->all_opaque=1;source->all_transparent=0;
    }
    gml_render_bind_device(render,&device);
    for(unsigned pixel=0;pixel<WIDTH*HEIGHT;pixel++) screen[pixel]=sentinel[pixel]=0xA5123456u;
    CHECK(gml_render_begin_resident_base(render,screen,WIDTH,HEIGHT,0,0));
    gml_render_gui_begin(render,WIDTH,HEIGHT);
    gml_render_set_pending_fill(render,0x40123456u);
    gml_render_set_pending_underlay(render,0,0,WIDTH,HEIGHT);
    if(excluded==1u) render->cam_x=1.0;
    if(excluded==2u) gml_render_gui_end(render);
    render->interp=(int)filtered;
    int reads=fake.readbacks;
    gml_draw_surface_stretched(render,surface,2,1,WIDTH,HEIGHT,0xFFFFFFu,1.0);
    if(excluded){
      CHECK(!render->resident_application_pass && fake.readbacks>reads);
      gml_render_free(render);free(render);
      continue;
    }
    CHECK(!render->pending_underlay && render->resident_application_pass && fake.readbacks==reads &&
          gml_render_scene_submit_pending(render) && !memcmp(screen,sentinel,sizeof screen));
    gml_render_resource_journal_metrics(render->base_scene_resource,&target);
    CHECK(target.authority==GML_RENDER_RESOURCE_GPU_RESIDENT &&
          target.gpu_version==target.logical_version && target.cpu_version<target.logical_version &&
          !target.journal_operations && !target.cpu_replays && !target.device_checkpoints &&
          !target.retained_mutable_bytes && fake.last_opcode==GML_RENDER_OP_TEXTURED_QUAD &&
          fake.last_quad_state.rgb_equation==GML_RENDER_BLEND_ADD);
    fake.forbid_calls=1;
    gml_render_abandon_device(render);
    CHECK(!fake.forbidden_calls);
    gml_render_free(render);free(render);
  }
  return 1;
}

static int resident_automatic_underlay_exclusions(void){
  enum { WIDTH=4,HEIGHT=4 };
  uint32_t reference[WIDTH*HEIGHT],screen[WIDTH*HEIGHT];
  GmlRenderQuad boundary={0};
  boundary.shape=GML_RENDER_QUAD_AXIS_ALIGNED;
  boundary.x[1]=boundary.x[2]=boundary.y[2]=boundary.y[3]=1.0;
  boundary.sampling=(GmlRenderSamplingPolicy){
    .filter=GML_RENDER_FILTER_BOX,.edge_rule=GML_RENDER_EDGE_CLAMP_SUBIMAGE,
    .quantisation=GML_RENDER_QUANTISE_AUTOMATIC_SURFACE_FIRSTGEN,
    .source_region={0,0,4096u,1u}};
  for(unsigned corner=0;corner<4u;corner++){
    boundary.corner_colour[corner]=0xFFFFFFFFu;
    boundary.u[corner]=corner==0u || corner==3u?0.0:1.0;
    boundary.v[corner]=corner<2u?0.0:1.0;
  }
  CHECK(gml_render_automatic_surface_quad_valid(&boundary,WIDTH,HEIGHT));
  boundary.sampling.source_region.width=4097u;
  CHECK(!gml_render_automatic_surface_quad_valid(&boundary,WIDTH,HEIGHT));
  boundary.sampling.filter=GML_RENDER_FILTER_NEAREST;
  CHECK(gml_render_automatic_surface_quad_valid(&boundary,WIDTH,HEIGHT));
  boundary.sampling.source_region.width=16385u;
  CHECK(!gml_render_automatic_surface_quad_valid(&boundary,WIDTH,HEIGHT));
  boundary.sampling.source_region.width=16384u;
  CHECK(gml_render_automatic_surface_quad_valid(&boundary,WIDTH,HEIGHT));
  CHECK(!gml_render_automatic_surface_quad_valid(&boundary,16385u,HEIGHT));
  boundary.x[0]=boundary.x[3]=-16383.0;
  CHECK(gml_render_automatic_surface_quad_valid(&boundary,WIDTH,HEIGHT));
  boundary.x[0]=boundary.x[3]=-16384.0;
  CHECK(!gml_render_automatic_surface_quad_valid(&boundary,WIDTH,HEIGHT));
  boundary.x[0]=boundary.x[3]=-0.5;
  CHECK(!gml_render_automatic_surface_quad_valid(&boundary,WIDTH,HEIGHT));
  for(unsigned variant=0;variant<8u;variant++) for(unsigned accelerated=0;accelerated<2u;accelerated++){
    GmlRender *render=(GmlRender*)calloc(1,sizeof *render);
    GmlWin content={.bytecode=15};
    FakeDevice fake={0};
    GmlRenderDeviceServices device=service(&fake);
    uint32_t *pixels=accelerated?screen:reference;
    CHECK(render);
    render->win=&content; render->color_write_mask=15u;
    CHECK(gml_render_application_surface_ensure_owned(render,WIDTH,HEIGHT));
    gml_render_bind_device(render,accelerated && variant!=5u?&device:NULL);
    if(accelerated && variant!=5u)
      CHECK(gml_render_begin_resident_owned_application(render,0,0,0));
    else CHECK(gml_render_begin_owned_application(render,0,0,0));
    gml_render_set_pending_fill(render,0xFF25C080u);
    gml_render_flush_pending_fill(render);
    if(accelerated && variant!=5u) CHECK(gml_render_scene_flush(render));
    render->app_surface_opaque=1;
    if(accelerated && variant==5u) gml_render_bind_device(render,&device);
    for(unsigned index=0;index<WIDTH*HEIGHT;index++) pixels[index]=0xA5123456u;
    if(accelerated) CHECK(gml_render_begin_resident_base(render,pixels,WIDTH,HEIGHT,0,0));
    else gml_render_begin(render,pixels,WIDTH,HEIGHT,0,0);
    gml_render_set_pending_fill(render,0x40123456u);
    gml_render_set_pending_underlay(render,0,0,WIDTH,HEIGHT);
    if(variant==0u){
      render->app_presentation_coverage=(uint8_t*)calloc(WIDTH*HEIGHT,1u);
      render->app_presentation_alpha_scratch=(uint8_t*)calloc(WIDTH*HEIGHT,1u);
      CHECK(render->app_presentation_coverage && render->app_presentation_alpha_scratch);
      render->app_presentation_coverage_capacity=WIDTH*HEIGHT;
      for(unsigned index=0;index<WIDTH*HEIGHT;index++)
        render->app_presentation_coverage[index]=(uint8_t)(index%3u*127u);
      render->app_presentation_coverage_active=1;
    }
    if(variant==1u) render->color_write_mask=7u;
    if(variant==2u) render->alpha_test_enable=1;
    if(variant==3u) render->blendmode=6;
    if(variant==4u) render->classic=1;
    if(variant==6u) render->underlay_w=16385;
    if(variant==7u) render->underlay_x=WIDTH;
    if(accelerated){
      GmlRenderJournalMetrics before={0},after={0};
      gml_render_resource_journal_metrics(render->app_surface_resource,&before);
      int submits=fake.submits,uploads=fake.uploads;
      CHECK(!gml_render_compose_resident_underlay(render) && render->pending_underlay);
      gml_render_resource_journal_metrics(render->app_surface_resource,&after);
      CHECK(fake.submits==submits && fake.uploads==uploads && !fake.readbacks &&
            before.logical_version==after.logical_version &&
            before.cpu_version==after.cpu_version);
    }
    gml_render_flush_pending_underlay(render);
    gml_render_flush_pending_fill(render);
    if(accelerated){
      if(render->resident_application_pass || memcmp(screen,reference,sizeof screen))
        fprintf(stderr,"automatic underlay exclusion: variant=%u resident=%d CPU0=%08x selected0=%08x\n",
                variant,render->resident_application_pass,reference[0],screen[0]);
      CHECK(!render->resident_application_pass && !memcmp(screen,reference,sizeof screen));
      fake.forbid_calls=1;
      gml_render_abandon_device(render);
      CHECK(gml_render_completed_screen_read(render,screen,WIDTH,HEIGHT) &&
            !memcmp(screen,reference,sizeof screen) && !fake.readbacks && !fake.forbidden_calls);
    }
    if(variant==4u) render->classic=0;
    gml_render_free(render); free(render);
  }
  return 1;
}

static int resident_nearest_font_pending_underlay(void){
  static const int policies[]={13,15,17};
  static const unsigned cutoffs[]={0u,1u,2u,127u,254u,255u};
  enum { WIDTH=12,HEIGHT=12 };
  for(unsigned policy=0;policy<3u;policy++) for(unsigned scale=1u;scale<=2u;scale++)
  for(unsigned cutoff=0;cutoff<6u;cutoff++){
    GmlWin content={.bytecode=policies[policy]};
    GmlRender *r=calloc(1,sizeof *r);
    FakeDevice fake={.on_demand=1};
    GmlRenderDeviceServices device=service(&fake);
    uint32_t screen[WIDTH*HEIGHT];
    CHECK(r && gml_render_init(r,NULL)==0);
    device.observe_on_demand=fake_observe_on_demand;
    r->win=&content;r->color_write_mask=15u;
    r->atlas=calloc(1,sizeof *r->atlas);CHECK(r->atlas);
    r->n_atlas=1;r->atlas[0].w=r->atlas[0].h=2;
    r->atlas[0].px=malloc(16u);CHECK(r->atlas[0].px);
    const uint8_t glyph[]={40u,90u,160u,0u,90u,160u,40u,255u,
                           160u,40u,90u,128u,40u,160u,90u,2u};
    memcpy(r->atlas[0].px,glyph,sizeof glyph);
    r->n_fonts=1;r->font=0;
    GmlFont *font=&r->fonts[0];
    font->real=1;font->sprite=-1;font->atlas=0;
    font->line_height=font->align_height=2;
    font->glyphs=calloc(1,sizeof *font->glyphs);CHECK(font->glyphs);
    for(unsigned cp=0;cp<256u;cp++) font->glyph_by_char[cp]=-1;
    font->glyph_by_char['A']=0;font->n_glyphs=1;
    font->glyphs[0]=(GmlGlyph){.w=2,.h=2,.shift=2,.ch='A'};
    CHECK(gml_render_application_surface_ensure_owned(r,4,4));
    gml_render_bind_device(r,&device);
    CHECK(gml_render_begin_resident_owned_application(r,0,0,0));
    gml_render_set_pending_fill(r,0xFF25C080u);
    gml_render_flush_pending_fill(r);CHECK(gml_render_scene_submit_pending(r));
    r->app_surface_opaque=1;
    for(unsigned pixel=0;pixel<WIDTH*HEIGHT;pixel++) screen[pixel]=0xA5123456u;
    CHECK(gml_render_begin_resident_base(r,screen,WIDTH,HEIGHT,0,0));
    gml_render_gui_begin(r,WIDTH,HEIGHT);
    gml_render_set_pending_fill(r,0x40123456u);
    gml_render_set_pending_underlay(r,0,0,WIDTH,HEIGHT);
    r->interp=0;r->alpha_test_enable=1;r->alpha_test_ref=cutoffs[cutoff];
    int submits=fake.submits;
    r->cam_x=1;
    CHECK(!gml_render_resident_font_glyph(r,0,0,0,2,2,2,2,scale,scale,0xFFFFFFu,1));
    CHECK(r->pending_underlay && r->pending_fill && fake.submits==submits && !fake.readbacks);
    r->cam_x=0;r->world_transform_active=1;
    CHECK(!gml_render_resident_font_glyph(r,0,0,0,2,2,2,2,scale,scale,0xFFFFFFu,1));
    CHECK(r->pending_underlay && r->pending_fill && fake.submits==submits && !fake.readbacks);
    r->world_transform_active=0;
    if(cutoffs[cutoff]==255u){
      CHECK(!gml_render_resident_font_glyph(r,0,0,0,2,2,2,2,scale,scale,0xFFFFFFu,1));
      CHECK(r->pending_underlay && r->resident_application_pass && !fake.readbacks);
    }else{
      gml_draw_text_transformed(r,2,2,"AA",scale,scale,0,0xFFFFFFu,1);
      CHECK(!r->pending_underlay && r->resident_application_pass && !fake.readbacks &&
            gml_render_scene_submit_pending(r));
      GmlRenderJournalMetrics metrics={0};
      gml_render_resource_journal_metrics(r->base_scene_resource,&metrics);
      CHECK(metrics.authority==GML_RENDER_RESOURCE_GPU_RESIDENT &&
            metrics.gpu_version==metrics.logical_version && metrics.cpu_version<metrics.logical_version &&
            !metrics.cpu_replays && !metrics.journal_operations);
      for(unsigned pixel=0;pixel<WIDTH*HEIGHT;pixel++) CHECK(screen[pixel]==0xA5123456u);
      CHECK(gml_render_completed_screen_read(r,screen,WIDTH,HEIGHT));
    }
    fake.forbid_calls=1;gml_render_abandon_device(r);CHECK(!fake.forbidden_calls);
    gml_render_free(r);free(r);
  }
  return 1;
}

static int resident_packaged_real_font_glyph(int runtime_page){
  GmlRender *render=(GmlRender*)calloc(1,sizeof *render);
  GmlRenderApplicationWriteView view={0};
  FakeDevice fake={0};
  GmlRenderDeviceServices device=service(&fake);
  uint32_t completed[16];
  const uint8_t rgba[16]={
    0xD0u,0x20u,0x10u,0xFFu, 0x10u,0xD0u,0x20u,0u,
    0x20u,0x10u,0xD0u,0xFFu, 0xC0u,0xB0u,0xA0u,0xFFu
  };
  CHECK(render && gml_render_application_surface_ensure_owned(render,4,4));
  render->atlas=(GmlAtlas*)calloc(1,sizeof *render->atlas);
  CHECK(render->atlas);
  render->n_atlas=render->n_fonts=1;
  render->atlas[0].px=(uint8_t*)malloc(sizeof rgba);
  CHECK(render->atlas[0].px);
  memcpy(render->atlas[0].px,rgba,sizeof rgba);
  render->atlas[0].w=render->atlas[0].h=2;
  render->atlas[0].runtime_font_page=runtime_page;
  GmlFont *font=&render->fonts[0];
  font->real=1; font->sprite=-1; font->atlas=0;
  font->runtime_owned=runtime_page;
  font->line_height=font->align_height=2;
  font->glyphs=(GmlGlyph*)calloc(1,sizeof *font->glyphs);
  CHECK(font->glyphs);
  for(int index=0;index<256;index++) font->glyph_by_char[index]=-1;
  font->n_glyphs=1;
  font->glyphs[0]=(GmlGlyph){.sx=0,.sy=0,.w=2,.h=2,.shift=2,.ch='A'};
  font->glyph_by_char['A']=0;
  render->font=0;
  render->alpha=1.0;
  render->color=0xFFFFFFu;
  render->alphablend=1;
  render->blend_equation=render->blend_equation_alpha=1;
  render->color_write_mask=0x0Fu;
  render->active_shader=-1;
  gml_render_bind_device(render,&device);
  fake.readback_colour=0xFF445566u;
  CHECK(gml_render_begin_resident_owned_application(render,0.0,0.0,0));
  gml_render_set_pending_fill(render,0xFF000000u);
  gml_draw_text(render,1.0,1.0,"A");
  CHECK(render->resident_application_pass);
  CHECK(gml_render_scene_flush(render) && fake.submits==2 && fake.readbacks==0);
  CHECK(gml_render_application_surface_owned_view(render,&view) &&
        view.pixels[5]==0xFFD02010u && view.pixels[6]==0xFF000000u &&
        view.pixels[9]==0xFF2010D0u && view.pixels[10]==0xFFC0B0A0u);
  memcpy(completed,view.pixels,sizeof completed);
  CHECK(gml_render_begin_owned_application(render,0.0,0.0,0));
  gml_render_set_pending_fill(render,0xFF000000u);
  gml_draw_text(render,1.0,1.0,"A");
  gml_render_flush_pending_fill(render);
  CHECK(!memcmp(render->app_surface_owned,completed,sizeof completed));
  gml_render_bind_device(render,&device);
  CHECK(gml_render_begin_resident_owned_application(render,0.0,0.0,0));
  gml_render_set_pending_fill(render,0xFF000000u);
  gml_draw_text_transformed(render,0.0,0.0,"A",2.0,2.0,0.0,0xFFFFFFu,1.0);
  CHECK(render->resident_application_pass);
  CHECK(gml_render_scene_flush(render) && fake.submits==4 && fake.readbacks==0);
  CHECK(gml_render_application_surface_owned_view(render,&view));
  CHECK(view.pixels[0]==0xFFD02010u && view.pixels[1]==0xFFD02010u &&
        view.pixels[2]==0xFF000000u && view.pixels[3]==0xFF000000u &&
        view.pixels[8]==0xFF2010D0u && view.pixels[9]==0xFF2010D0u &&
        view.pixels[10]==0xFFC0B0A0u && view.pixels[11]==0xFFC0B0A0u);
  memcpy(completed,view.pixels,sizeof completed);
  CHECK(gml_render_begin_owned_application(render,0.0,0.0,0));
  gml_render_set_pending_fill(render,0xFF000000u);
  gml_draw_text_transformed(render,0.0,0.0,"A",2.0,2.0,0.0,0xFFFFFFu,1.0);
  gml_render_flush_pending_fill(render);
  CHECK(!memcmp(render->app_surface_owned,completed,sizeof completed));
  gml_render_bind_device(render,&device);
  CHECK(gml_render_begin_resident_owned_application(render,0.0,0.0,0));
  gml_render_set_pending_fill(render,0xFF123456u);
  gml_draw_text_transformed(render,1.0,1.0,"A",1.0,1.0,0.0,0x80C040u,1.0);
  CHECK(render->resident_application_pass);
  CHECK(gml_render_scene_flush(render));
  CHECK(fake.submits==6 && fake.readbacks==1);
  CHECK(gml_render_application_surface_owned_view(render,&view) &&
        view.pixels[5]==fake.readback_colour && fake.readbacks==1);
  CHECK(gml_render_begin_owned_application(render,0.0,0.0,0));
  gml_render_set_pending_fill(render,0xFF123456u);
  gml_draw_text_transformed(render,1.0,1.0,"A",1.0,1.0,0.0,0x80C040u,1.0);
  gml_render_flush_pending_fill(render);
  CHECK(render->app_surface_owned[5]==0xFF341808u);
  int previous_submits=fake.submits;
  gml_render_bind_device(render,&device);
  CHECK(gml_render_begin_resident_owned_application(render,0.0,0.0,0));
  gml_render_set_pending_fill(render,0xFF123456u);
  gml_draw_text(render,1.0,1.0,"A");
  CHECK(render->resident_application_pass);
  gml_draw_text_transformed(render,2.0,2.0,"A",1.0,1.0,90.0,0xFFFFFFu,1.0);
  CHECK(!render->resident_application_pass &&
        fake.submits==previous_submits+2 && fake.readbacks==1);
  CHECK(gml_render_application_surface_owned_view(render,&view));
  memcpy(completed,view.pixels,sizeof completed);
  gml_render_bind_device(render,NULL);
  CHECK(gml_render_application_surface_owned_view(render,&view) &&
        !memcmp(view.pixels,completed,sizeof completed));
  CHECK(gml_render_begin_owned_application(render,0.0,0.0,0));
  gml_render_set_pending_fill(render,0xFF123456u);
  gml_draw_text(render,1.0,1.0,"A");
  gml_draw_text_transformed(render,2.0,2.0,"A",1.0,1.0,90.0,0xFFFFFFu,1.0);
  gml_render_flush_pending_fill(render);
  CHECK(!memcmp(render->app_surface_owned,completed,sizeof completed));
  if(!runtime_page){
    GmlWin studio={0};
    GmlRenderJournalMetrics before={0},after={0};
    int prior_submits=fake.submits;
    int prior_readbacks=fake.readbacks;
    studio.bytecode=16;
    render->win=&studio;
    render->interp=1;
    gml_render_bind_device(render,&device);
    CHECK(gml_render_begin_resident_owned_application(render,0.0,0.0,0));
    gml_render_set_pending_fill(render,0xFF123456u);
    gml_draw_text_transformed(render,1.0,1.0,"A",0.5,0.5,0.0,0x80C040u,0.7);
    CHECK(render->resident_application_pass);
    CHECK(gml_render_scene_flush(render) && fake.submits==prior_submits+2 &&
          fake.readbacks==prior_readbacks);
    GmlRenderResource *resource=gml_render_application_resource_ensure(render);
    CHECK(resource);
    gml_render_resource_journal_metrics(resource,&before);
    CHECK(before.authority==GML_RENDER_RESOURCE_GPU_REPLAYABLE &&
          before.journal_operations==2u && before.retained_mutable_bytes==0u);
    fake.forbid_calls=1;
    gml_render_abandon_device(render);
    CHECK(gml_render_application_surface_owned_view(render,&view));
    memcpy(completed,view.pixels,sizeof completed);
    gml_render_resource_journal_metrics(resource,&after);
    CHECK(after.cpu_replays>before.cpu_replays &&
          fake.readbacks==prior_readbacks && fake.forbidden_calls==0);
    CHECK(gml_render_begin_owned_application(render,0.0,0.0,0));
    gml_render_set_pending_fill(render,0xFF123456u);
    gml_draw_text_transformed(render,1.0,1.0,"A",0.5,0.5,0.0,0x80C040u,0.7);
    gml_render_flush_pending_fill(render);
    CHECK(!memcmp(render->app_surface_owned,completed,sizeof completed));
    fake.forbid_calls=0;
    render->interp=0;
    render->win=NULL;
  }
  if(runtime_page){
    uint8_t *replacement;
    render->atlas[0].px[7]=128u;
    render->atlas[0].scene_source_version++;
    gml_render_bind_device(render,&device);
    fake.fail_upload_at=fake.uploads+1;
    CHECK(gml_render_begin_resident_owned_application(render,0.0,0.0,0));
    gml_render_set_pending_fill(render,0xFF000000u);
    gml_draw_text(render,1.0,1.0,"A");
    CHECK(!render->resident_application_pass &&
          fake.uploads==fake.fail_upload_at &&
          gml_render_application_scene_flush(render));
    CHECK(gml_render_application_surface_owned_view(render,&view) &&
          view.pixels[5]==0xFFD02010u);
    memcpy(completed,view.pixels,sizeof completed);
    gml_render_bind_device(render,NULL);
    CHECK(gml_render_begin_owned_application(render,0.0,0.0,0));
    gml_render_set_pending_fill(render,0xFF000000u);
    gml_draw_text(render,1.0,1.0,"A");
    gml_render_flush_pending_fill(render);
    CHECK(!memcmp(render->app_surface_owned,completed,sizeof completed));
    replacement=(uint8_t*)malloc(sizeof rgba);
    CHECK(replacement);
    memcpy(replacement,render->atlas[0].px,sizeof rgba);
    replacement[7]=255u;
    free(render->atlas[0].px);
    render->atlas[0].px=replacement;
    render->atlas[0].scene_source_version++;
    fake.fail_upload_at=0;
    gml_render_bind_device(render,&device);
    CHECK(gml_render_begin_resident_owned_application(render,0.0,0.0,0));
    gml_render_set_pending_fill(render,0xFF000000u);
    gml_draw_text(render,1.0,1.0,"A");
    CHECK(render->resident_application_pass && gml_render_scene_flush(render));
    CHECK(gml_render_application_surface_owned_view(render,&view) &&
          view.pixels[5]==0xFFD02010u && view.pixels[6]==0xFF10D020u);
  }
  gml_render_free(render);
  free(render);
  return 1;
}

static int resident_binary_atlas_sprite_gpu(void){
  for(int variant=0;variant<2;variant++){
    GmlRender *render=(GmlRender*)calloc(1,sizeof *render);
    GmlRenderApplicationWriteView view={0};
    FakeDevice fake={0};
    GmlRenderDeviceServices device=service(&fake);
    uint32_t candidate[16];
    uint8_t rgba[16]={
      0xD0u,0x20u,0x10u,0xFFu, 0x10u,0xD0u,0x20u,0u,
      0x20u,0x10u,0xD0u,0xFFu, 0xC0u,0xB0u,0xA0u,0xFFu
    };
    CHECK(render);
    CHECK(gml_render_application_surface_ensure_owned(render,4,4));
    render->atlas=(GmlAtlas*)calloc(1,sizeof *render->atlas);
    render->tpag=(GmlTpag*)calloc(1,sizeof *render->tpag);
    render->spr=(GmlSprite*)calloc(1,sizeof *render->spr);
    CHECK(render->atlas && render->tpag && render->spr);
    render->n_atlas=render->n_tpag=render->n_spr=1;
    render->atlas[0].px=(uint8_t*)malloc(sizeof rgba);
    render->spr[0].frame=(int*)calloc(1,sizeof(int));
    CHECK(render->atlas[0].px && render->spr[0].frame);
    if(variant) rgba[7]=128u;
    memcpy(render->atlas[0].px,rgba,sizeof rgba);
    render->atlas[0].w=render->atlas[0].h=2;
    render->tpag[0].sw=render->tpag[0].sh=2;
    render->tpag[0].tw=render->tpag[0].th=2;
    render->tpag[0].bw=render->tpag[0].bh=4;
    render->tpag[0].tx=render->tpag[0].ty=1;
    render->spr[0].w=render->spr[0].h=4;
    render->spr[0].originx=render->spr[0].originy=2;
    render->spr[0].n_frames=1;
    render->active_shader=-1;
    render->color_write_mask=0x0Fu;
    render->interp=render->texture_filter[0]=1;
    render->world_transform_active=1;
    render->world_scale_x=render->world_scale_y=1.0;
    gml_render_bind_device(render,&device);
    CHECK(gml_render_begin_resident_owned_application(render,0.0,0.0,0));
    gml_render_set_pending_fill(render,0xFF123456u);
    gml_draw_sprite_ext(render,0,0,2.0,2.0,1.0,1.0,0.0,0xFFFFFFu,1.0);
    CHECK(render->resident_application_pass==!variant && fake.submits==1);
    CHECK(gml_render_application_surface_owned_view(render,&view) &&
          view.pixels[5]==0xFFD02010u &&
          (variant ? view.pixels[6]!=0xFF123456u
                   : view.pixels[6]==0xFF123456u) &&
          view.pixels[9]==0xFF2010D0u && view.pixels[10]==0xFFC0B0A0u &&
          fake.readbacks==0);
    CHECK(fake.submits==(variant?1:2));
    memcpy(candidate,view.pixels,sizeof candidate);
    CHECK(gml_render_begin_owned_application(render,0.0,0.0,0));
    gml_render_set_pending_fill(render,0xFF123456u);
    gml_draw_sprite_ext(render,0,0,2.0,2.0,1.0,1.0,0.0,0xFFFFFFu,1.0);
    gml_render_flush_pending_fill(render);
    CHECK(!memcmp(render->app_surface_owned,candidate,sizeof candidate));
    gml_render_free(render);
    free(render);
  }
  return 1;
}

static int resident_integer_scaled_atlas_sprite_gpu(void){
  GmlRender *render=(GmlRender*)calloc(1,sizeof *render);
  GmlRenderApplicationWriteView view={0};
  FakeDevice fake={0};
  GmlRenderDeviceServices device=service(&fake);
  uint32_t candidate[64];
  const uint8_t rgba[16]={
    0xD0u,0x20u,0x10u,0xFFu, 0x10u,0xD0u,0x20u,0u,
    0x20u,0x10u,0xD0u,0xFFu, 0xC0u,0xB0u,0xA0u,0xFFu
  };
  CHECK(render);
  CHECK(gml_render_application_surface_ensure_owned(render,8,8));
  render->atlas=(GmlAtlas*)calloc(1,sizeof *render->atlas);
  render->tpag=(GmlTpag*)calloc(1,sizeof *render->tpag);
  render->spr=(GmlSprite*)calloc(1,sizeof *render->spr);
  CHECK(render->atlas && render->tpag && render->spr);
  render->n_atlas=render->n_tpag=render->n_spr=1;
  render->atlas[0].px=(uint8_t*)malloc(sizeof rgba);
  render->spr[0].frame=(int*)calloc(1,sizeof(int));
  CHECK(render->atlas[0].px && render->spr[0].frame);
  memcpy(render->atlas[0].px,rgba,sizeof rgba);
  render->atlas[0].w=render->atlas[0].h=2;
  render->tpag[0].sw=render->tpag[0].sh=2;
  render->tpag[0].tw=render->tpag[0].th=2;
  render->tpag[0].bw=render->tpag[0].bh=4;
  render->tpag[0].tx=render->tpag[0].ty=1;
  render->spr[0].w=render->spr[0].h=4;
  render->spr[0].originx=render->spr[0].originy=2;
  render->spr[0].n_frames=1;
  render->active_shader=-1;
  render->color_write_mask=0x0Fu;
  gml_render_bind_device(render,&device);
  CHECK(gml_render_begin_resident_owned_application(render,0.0,0.0,0));
  gml_render_set_pending_fill(render,0xFF123456u);
  gml_draw_sprite_ext(render,0,0,4.0,4.0,2.0,2.0,0.0,0xFFFFFFu,1.0);
  CHECK(render->resident_application_pass);
  CHECK(fake.submits==1);
  CHECK(gml_render_scene_flush(render));
  CHECK(fake.submits==2);
  CHECK(gml_render_application_surface_owned_view(render,&view));
  CHECK(view.pixels[2u+2u*8u]==0xFFD02010u &&
        view.pixels[3u+3u*8u]==0xFFD02010u &&
        view.pixels[4u+2u*8u]==0xFF123456u &&
        view.pixels[2u+4u*8u]==0xFF2010D0u &&
        view.pixels[5u+5u*8u]==0xFFC0B0A0u);
  memcpy(candidate,view.pixels,sizeof candidate);
  CHECK(gml_render_begin_owned_application(render,0.0,0.0,0));
  gml_render_set_pending_fill(render,0xFF123456u);
  gml_draw_sprite_ext(render,0,0,4.0,4.0,2.0,2.0,0.0,0xFFFFFFu,1.0);
  gml_render_flush_pending_fill(render);
  CHECK(!memcmp(render->app_surface_owned,candidate,sizeof candidate));
  render->interp=1;
  CHECK(gml_render_begin_resident_owned_application(render,0.0,0.0,0));
  gml_render_set_pending_fill(render,0xFF123456u);
  gml_draw_sprite_ext(render,0,0,4.0,4.0,2.0,2.0,0.0,0xFFFFFFu,1.0);
  CHECK(!render->resident_application_pass && fake.submits==3);
  gml_render_free(render);
  free(render);
  return 1;
}

static int resident_classic_owned_binary_unit_sprite(void){
  const int generations[]={530,600,700};
  for(unsigned generation=0;generation<3u;generation++)
  for(unsigned exclusion=0;exclusion<16u;exclusion++){
    GmlRender *render=calloc(1,sizeof *render);
    GmlWin content={.classic_version=exclusion==13u?810:generations[generation]};
    FakeDevice fake={.on_demand=exclusion!=1u};
    GmlRenderDeviceServices device=service(&fake);
    uint32_t base[64]={0},shadow[64],phase[8]={0};
    GmlRenderJournalMetrics metrics={0};
    const uint8_t rgba[16]={208,32,16,255,16,208,32,0,
                           32,16,208,255,192,176,160,255};
    CHECK(render && gml_render_init(render,NULL)==0);
    render->classic=1;render->win=&content;
    free(render->atlas[0].px);
    render->atlas[0].px=malloc(sizeof rgba);
    render->atlas[0].w=render->atlas[0].h=2;
    render->tpag=calloc(1,sizeof *render->tpag);
    render->spr=calloc(1,sizeof *render->spr);
    CHECK(render->atlas[0].px && render->tpag && render->spr);
    memcpy(render->atlas[0].px,rgba,sizeof rgba);
    if(exclusion==2u) render->atlas[0].px[7]=128;
    render->n_tpag=render->n_spr=1;
    render->tpag[0]=(GmlTpag){.sw=2,.sh=2,.tw=2,.th=2,.bw=4,.bh=4,.tx=1,.ty=1};
    render->spr[0].w=render->spr[0].h=4;
    render->spr[0].originx=render->spr[0].originy=1;
    render->spr[0].n_frames=1;render->spr[0].frame=calloc(1,sizeof(int));
    CHECK(render->spr[0].frame);
    device.observe_on_demand=fake_observe_on_demand;
    gml_render_begin(render,base,8,8,0,0);
    CHECK(gml_render_application_surface_ensure_owned(render,8,8));
    gml_render_bind_device(render,&device);
    CHECK(gml_render_begin_resident_owned_application(render,0,0,0));
    memcpy(shadow,render->fb,sizeof shadow);
    if(exclusion==3u) render->interp=1;
    if(exclusion==4u) render->color_write_mask=7u;
    if(exclusion==5u) render->gui_pass_active=1;
    if(exclusion==6u) render->alpha_test_enable=1;
    if(exclusion==7u) render->fog_flat=1;
    if(exclusion==8u) render->classic_phase_y=phase;
    if(exclusion==9u){render->world_transform_active=1;render->world_scale_x=2;}
    gml_draw_sprite_ext(render,0,0,2.25,3.75,exclusion==10u?2.0:exclusion==15u?-1.0:1.0,1.0,
      exclusion==11u?0.5:0.0,exclusion==12u?0xABCDEFu:0xFFFFFFu,
      exclusion==14u?0.5:1.0);
    /* The old owned-binary rule is still preferred. Newly paired integer
     * variants strengthen these old scope exclusions into transport guards,
     * including Classic's logical nearest writer with presentation filtering. */
    int integer_extension=exclusion==2u || exclusion==3u || exclusion==5u || exclusion==6u || exclusion==9u || exclusion==10u || exclusion==12u ||
      exclusion==13u || exclusion==14u || exclusion==15u;
    int nearest_extension=exclusion==4u || exclusion==11u;
    int fog_extension=exclusion==7u;
    if(!exclusion || integer_extension || nearest_extension || fog_extension){
      CHECK(render->resident_application_pass && !fake.submits && !fake.readbacks &&
            !memcmp(shadow,render->fb,sizeof shadow));
      CHECK(gml_render_scene_submit_pending(render) && fake.submits==1 &&
            (exclusion==9u || exclusion==10u || exclusion==11u?fake.last_batch_operations>=1u:fake.last_batch_operations==1u) && !fake.readbacks);
      GmlRenderResource *resource=gml_render_scene_target_resource_ensure(render);
      CHECK(resource);
      gml_render_resource_journal_metrics(resource,&metrics);
      CHECK(metrics.authority==GML_RENDER_RESOURCE_GPU_RESIDENT &&
            metrics.cpu_version<metrics.logical_version && !metrics.cpu_replays &&
            !metrics.journal_operations && !metrics.retained_mutable_bytes);
      if(integer_extension) CHECK(fake.last_quantisation==GML_RENDER_QUANTISE_INTEGER_TINT_TRUNC);
      if(fog_extension) CHECK(fake.last_filter==GML_RENDER_FILTER_NEAREST &&
        fake.last_quantisation==GML_RENDER_QUANTISE_INTEGER_FLAT_RGB &&
        !fake.rgb_queries && !metrics.device_checkpoints);
      if(nearest_extension) CHECK(fake.last_filter==GML_RENDER_FILTER_NEAREST &&
        fake.last_quad_state.write_mask==render->color_write_mask && !fake.rgb_queries &&
        !metrics.device_checkpoints && !memcmp(shadow,render->fb,sizeof shadow));
      if(exclusion==6u) CHECK(fake.last_quad_state.alpha_test_enabled==1u &&
                             fake.last_quad_state.alpha_test_reference==0u);
    } else CHECK(!render->resident_application_pass);
    gml_render_abandon_device(render);gml_render_free(render);free(render);
  }
  return 1;
}

static int resident_classic_nonphase_sprite_targets(void){
  for(unsigned scaled=0;scaled<2u;scaled++)
  for(unsigned generation=0;generation<2u;generation++)
  for(unsigned numbered=0;numbered<2u;numbered++)
  for(unsigned source_kind=0;source_kind<3u;source_kind++){
    GmlRender *render=calloc(1,sizeof *render);
    GmlWin content={.classic_version=generation?810:800};
    FakeDevice fake={.on_demand=scaled || source_kind==2u};
    GmlRenderDeviceServices device=service(&fake);
    uint32_t pixels[64],reference[64];
    GmlRenderJournalMetrics metrics={0};
    const uint32_t *observed=NULL;
    const uint8_t rgba[16]={208,32,16,255,16,208,32,255,
                            32,16,208,255,192,176,160,255};
    CHECK(render && gml_render_init(render,NULL)==0);
    render->win=&content;render->classic=1;
    CHECK(render->n_atlas==1);
    free(render->atlas[0].px);
    memset(render->atlas,0,sizeof *render->atlas);
    render->atlas[0].w=render->atlas[0].h=2;
    render->atlas[0].px=malloc(sizeof rgba);
    render->tpag=calloc(1,sizeof *render->tpag);
    render->spr=calloc(1,sizeof *render->spr);
    CHECK(render->atlas[0].px && render->tpag && render->spr);
    memcpy(render->atlas[0].px,rgba,sizeof rgba);
    if(source_kind) render->atlas[0].px[7]=source_kind==1u?0:128;
    render->n_tpag=render->n_spr=1;
    render->tpag[0]=(GmlTpag){.sw=2,.sh=2,.tw=2,.th=2,.bw=4,.bh=4,.tx=1,.ty=1};
    render->spr[0].w=render->spr[0].h=4;
    render->spr[0].originx=render->spr[0].originy=2;
    render->spr[0].n_frames=1;
    render->spr[0].frame=calloc(1,sizeof(int));
    CHECK(render->spr[0].frame);
    for(unsigned i=0;i<64u;i++) pixels[i]=reference[i]=0xFF123456u;
    gml_render_begin(render,reference,8,8,0.0,0.0);
    int id=numbered?gml_surface_create(render,8,8):0;
    if(numbered){
      CHECK(id>0 && gml_surface_set_target(render,id));
      memcpy(render->surface[id-1].px,reference,sizeof reference);
    }
    double xs=scaled?0.7:1.0,ys=scaled?1.3:1.0;
    gml_draw_sprite_ext(render,0,-1,3.25,2.75,xs,ys,0.0,0xFFFFFFu,1.0);
    if(numbered){
      memcpy(reference,render->surface[id-1].px,sizeof reference);
      gml_surface_reset_target(render);
    }
    device.observe_on_demand=fake_observe_on_demand;
    gml_render_application_surface_bind(render,pixels,8,8,1);
    gml_render_bind_device(render,&device);
    CHECK(gml_render_begin_resident_base(render,pixels,8,8,0.0,0.0));
    if(numbered){
      for(unsigned i=0;i<64u;i++) render->surface[id-1].px[i]=0xFF123456u;
      CHECK(gml_surface_set_target(render,id));
    }
    CHECK(render->resident_application_pass);
    render->fb_opaque_known=render->fb_all_opaque=1;
    gml_draw_sprite_ext(render,0,-1,3.25,2.75,xs,ys,0.0,0xFFFFFFu,1.0);
    CHECK(render->resident_application_pass && !fake.submits && !fake.readbacks);
    CHECK(gml_render_scene_submit_pending(render) && fake.submits==1 && !fake.readbacks);
    GmlRenderResource *resource=gml_render_scene_target_resource_ensure(render);
    CHECK(resource);
    gml_render_resource_journal_metrics(resource,&metrics);
    if(source_kind<2u && !scaled){
      CHECK(gml_render_resource_acquire_cpu_read(resource,&observed) &&
            !memcmp(observed,reference,sizeof reference) && !fake.readbacks);
    } else {
      CHECK(metrics.authority==GML_RENDER_RESOURCE_GPU_RESIDENT &&
            metrics.cpu_version<metrics.logical_version && !metrics.cpu_replays &&
            !metrics.journal_operations && !metrics.device_checkpoints);
    }
    gml_render_abandon_device(render);gml_render_free(render);free(render);
  }
  return 1;
}

static uint8_t *runtime_unit_test_rgba(int width,int height,int frames,int binary){
  size_t count=(size_t)width*height*frames;
  uint8_t *bytes=malloc(count*4u);
  if(!bytes) return NULL;
  for(size_t index=0;index<count;index++){
    bytes[index*4u]=(uint8_t)(31u+index*17u);
    bytes[index*4u+1u]=(uint8_t)(43u+index*23u);
    bytes[index*4u+2u]=(uint8_t)(59u+index*29u);
    bytes[index*4u+3u]=binary && index%3u==1u?0u:255u;
  }
  return bytes;
}

static int resident_classic_unit_tiles(void){
  const double positions[5][2]={{2,3},{-1,2},{7,7},{8,2},{2,-2}};
  for(unsigned generation=0;generation<2u;generation++)
  for(unsigned numbered=0;numbered<2u;numbered++)
  for(unsigned binary=0;binary<2u;binary++)
  for(unsigned placement=0;placement<5u;placement++)
  for(unsigned port=0;port<2u;port++){
    GmlRender *render=calloc(1,sizeof *render);
    GmlWin content={.classic_version=generation?810:600};
    FakeDevice fake={.on_demand=1};
    GmlRenderDeviceServices device=service(&fake);
    GmlRenderJournalMetrics metrics={0};
    uint32_t base[64],shadow[64];
    const uint8_t rgba[16]={208,32,16,255,16,208,32,255,
                            32,16,208,255,192,176,160,255};
    CHECK(render && gml_render_init(render,NULL)==0);
    render->classic=1;render->win=&content;
    free(render->atlas[0].px);
    render->atlas[0].px=malloc(sizeof rgba);
    CHECK(render->atlas[0].px);
    memcpy(render->atlas[0].px,rgba,sizeof rgba);
    if(binary) render->atlas[0].px[7]=0;
    render->atlas[0].w=render->atlas[0].h=2;
    render->tpag=calloc(1,sizeof *render->tpag);
    render->bg=calloc(1,sizeof *render->bg);
    CHECK(render->tpag && render->bg);
    render->n_tpag=render->n_bg=1;
    render->tpag[0]=(GmlTpag){.sw=2,.sh=2,.tw=2,.th=2,.bw=2,.bh=2};
    for(unsigned index=0;index<64u;index++) base[index]=0xFF123456u;
    gml_render_begin(render,base,8,8,0.25,-0.25);
    int id=numbered?gml_surface_create(render,8,8):0;
    if(numbered) CHECK(id>0);
    device.observe_on_demand=fake_observe_on_demand;
    gml_render_application_surface_bind(render,base,8,8,1);
    gml_render_bind_device(render,&device);
    CHECK(gml_render_begin_resident_base(render,base,8,8,0.25,-0.25));
    if(numbered) CHECK(gml_surface_set_target(render,id));
    memcpy(shadow,render->fb,sizeof shadow);
    double camx=render->cam_x,camy=render->cam_y;
    gml_draw_background_tile(render,0,0,0,2,2,3+camx,3+camy,1,1,0,0,0,0xFFFFFFu,1);
    double x=positions[placement][0]+camx,y=positions[placement][1]+camy;
    if(port) gml_draw_tile(render,0,0,0,2,2,x,y);
    else gml_draw_background_tile(render,0,0,0,2,2,x,y,1,1,0,0,0,0xFFFFFFu,1);
    /* Only the selected-source port may clip an overrun to an empty source. */
    if(!port) gml_draw_background_tile(render,0,2,0,2,2,1+camx,1+camy,1,1,0,0,0,0xFFFFFFu,1);
    CHECK(render->resident_application_pass && !fake.submits && !fake.readbacks &&
          !memcmp(shadow,render->fb,sizeof shadow));
    CHECK(gml_render_scene_submit_pending(render) && fake.submits==1 &&
          fake.last_batch_operations==(placement>=3u?1u:2u) && !fake.readbacks);
    GmlRenderResource *resource=gml_render_scene_target_resource_ensure(render);
    CHECK(resource);
    gml_render_resource_journal_metrics(resource,&metrics);
    CHECK(metrics.authority==GML_RENDER_RESOURCE_GPU_RESIDENT &&
          metrics.cpu_version<metrics.logical_version && !metrics.cpu_replays &&
          !metrics.journal_operations && !metrics.retained_mutable_bytes);
    gml_render_abandon_device(render);gml_render_free(render);free(render);
  }
  for(unsigned exclusion=0;exclusion<10u;exclusion++){
    GmlRender *render=calloc(1,sizeof *render);
    GmlWin content={.classic_version=600};
    FakeDevice fake={.on_demand=exclusion!=0u};
    GmlRenderDeviceServices device=service(&fake);
    uint32_t base[64]={0};
    CHECK(render && gml_render_init(render,NULL)==0);
    render->classic=1;render->win=&content;
    free(render->atlas[0].px);
    render->atlas[0].px=malloc(16u);
    render->tpag=calloc(1,sizeof *render->tpag);
    render->bg=calloc(1,sizeof *render->bg);
    CHECK(render->atlas[0].px && render->tpag && render->bg);
    memset(render->atlas[0].px,255,16u);
    if(exclusion==1u) render->atlas[0].px[3]=128;
    render->atlas[0].w=render->atlas[0].h=2;
    render->n_tpag=render->n_bg=1;
    render->tpag[0]=(GmlTpag){.sw=2,.sh=2,.tw=2,.th=2,.bw=2,.bh=2};
    device.observe_on_demand=fake_observe_on_demand;
    gml_render_application_surface_bind(render,base,8,8,1);
    gml_render_bind_device(render,&device);
    CHECK(gml_render_begin_resident_base(render,base,8,8,0,0));
    if(exclusion==2u) render->interp=1;
    if(exclusion==3u) render->color_write_mask=7u;
    if(exclusion==9u){render->world_transform_active=1;render->world_scale_x=2;}
    uint32_t shadow[64];memcpy(shadow,render->fb,sizeof shadow);
    gml_draw_background_tile(render,0,0,0,2,2,exclusion==4u?2.5:2.0,2.0,
      exclusion==5u?2.0:1.0,1.0,exclusion==6u,0,0,
      exclusion==7u?0xABCDEFu:0xFFFFFFu,exclusion==8u?0.5:1.0);
    /* The complete nearest TILE owner supersedes the old unit-only scope.
     * Preserve every input and the exclusions outside its paired domain. */
    if(exclusion==0u || exclusion==2u || exclusion==3u)
      CHECK(!render->resident_application_pass);
    else{
      CHECK(render->resident_application_pass && !fake.submits && !fake.readbacks &&
            !memcmp(shadow,render->fb,sizeof shadow));
      CHECK(gml_render_scene_submit_pending(render) && fake.submits==1 &&
            !fake.readbacks && fake.last_quantisation==GML_RENDER_QUANTISE_INTEGER_TINT_TRUNC &&
            fake.last_filter==GML_RENDER_FILTER_NEAREST);
    }
    gml_render_free(render);free(render);
  }
  return 1;
}

static int resident_runtime_binary_unit_sprites(void){
  const double positions[4][2]={{2,3},{-1,2},{7,7},{8,2}};
  for(unsigned generation=0;generation<2u;generation++)
  for(unsigned numbered=0;numbered<2u;numbered++)
  for(unsigned binary=0;binary<2u;binary++)
  for(unsigned placement=0;placement<4u;placement++){
    GmlRender *render=calloc(1,sizeof *render);
    GmlWin content={.classic_version=generation?810:600};
    FakeDevice fake={.on_demand=1};
    GmlRenderDeviceServices device=service(&fake);
    GmlRenderJournalMetrics metrics={0};
    uint32_t base[64],shadow[64];
    CHECK(render && gml_render_init(render,NULL)==0);
    render->classic=1;render->win=&content;
    int sprite=gml_sprite_append_from_rgba_frames(render,
      runtime_unit_test_rgba(3,2,2,(int)binary),3,2,2,1,1,NULL);
    CHECK(sprite>=0 && render->spr[sprite].runtime_scene_binary);
    for(unsigned index=0;index<64u;index++) base[index]=0xFF123456u;
    gml_render_begin(render,base,8,8,0.25,-0.25);
    int id=numbered?gml_surface_create(render,8,8):0;
    if(numbered) CHECK(id>0);
    device.observe_on_demand=fake_observe_on_demand;
    gml_render_application_surface_bind(render,base,8,8,1);
    gml_render_bind_device(render,&device);
    CHECK(gml_render_begin_resident_base(render,base,8,8,0.25,-0.25));
    if(numbered) CHECK(gml_surface_set_target(render,id));
    memcpy(shadow,render->fb,sizeof shadow);
    double camx=render->cam_x,camy=render->cam_y;
    gml_draw_sprite_ext(render,sprite,0,3+camx,3+camy,1,1,0,0xFFFFFFu,1);
    gml_draw_sprite_ext(render,sprite,-1,positions[placement][0]+1+camx,
      positions[placement][1]+1+camy,1,1,0,0xFFFFFFu,1);
    CHECK(render->resident_application_pass && !fake.submits && !fake.readbacks &&
          !memcmp(shadow,render->fb,sizeof shadow));
    CHECK(gml_render_scene_submit_pending(render) && fake.submits==1 &&
          fake.last_batch_operations==(placement==3u?1u:2u) && !fake.readbacks);
    CHECK(fake.last_batch_source_ids[0]==UINT32_C(0x80000000));
    if(placement!=3u) CHECK(fake.last_batch_source_ids[1]==UINT32_C(0x80000001));
    GmlRenderResource *resource=gml_render_scene_target_resource_ensure(render);
    CHECK(resource);
    gml_render_resource_journal_metrics(resource,&metrics);
    CHECK(metrics.authority==GML_RENDER_RESOURCE_GPU_RESIDENT &&
          metrics.cpu_version<metrics.logical_version && !metrics.cpu_replays &&
          !metrics.journal_operations && !metrics.retained_mutable_bytes);
    gml_render_abandon_device(render);gml_render_free(render);free(render);
  }
  for(unsigned exclusion=0;exclusion<9u;exclusion++){
    GmlRender *render=calloc(1,sizeof *render);
    GmlWin content={.classic_version=600};
    FakeDevice fake={.on_demand=exclusion!=0u};
    GmlRenderDeviceServices device=service(&fake);
    uint32_t base[64]={0};
    CHECK(render && gml_render_init(render,NULL)==0);
    render->classic=1;render->win=&content;
    uint8_t *rgba=runtime_unit_test_rgba(3,2,1,0);
    CHECK(rgba);
    if(exclusion==1u) rgba[3]=128;
    int sprite=gml_sprite_append_from_rgba(render,rgba,3,2,0,0,NULL);
    CHECK(sprite>=0);
    if(exclusion==2u) render->spr[sprite].runtime_scene_binary=0;
    device.observe_on_demand=fake_observe_on_demand;
    gml_render_application_surface_bind(render,base,8,8,1);
    gml_render_bind_device(render,&device);
    CHECK(gml_render_begin_resident_base(render,base,8,8,0,0));
    if(exclusion==3u) render->interp=1;
    if(exclusion==4u) render->color_write_mask=7u;
    gml_draw_sprite_ext(render,sprite,0,exclusion==5u?2.5:2.0,2.0,
      exclusion==6u?2.0:1.0,1.0,exclusion==7u?17.0:0.0,
      exclusion==8u?0xABCDEFu:0xFFFFFFu,1.0);
    /* The runtime function now qualifies partial alpha, Classic point filter
     * state, masks, fractional anchors, scale, rotation and tint. Missing
     * on-demand authority and contradictory classification still reject. */
    int admitted=exclusion!=0u && exclusion!=2u;
    CHECK(!!render->resident_application_pass==admitted &&
          !!render->spr[sprite].runtime_scene_used==admitted);
    if(admitted) CHECK(!fake.submits && !fake.readbacks &&
      gml_render_scene_submit_pending(render) && fake.submits==1 && !fake.readbacks);
    gml_render_free(render);free(render);
  }
  return 1;
}

static int runtime_sprite_scene_lifetimes_variant(int partial){
  GmlRender *render=calloc(1,sizeof *render);
  FakeDevice fake={.on_demand=1};
  GmlRenderDeviceServices device=service(&fake);
  GmlWin content={.classic_version=600};
  GmlRenderResourceSource one={0},same={0},other={0},renewed={0};
  uint32_t base[64]={0};
  CHECK(render && gml_render_init(render,NULL)==0);
  render->classic=1;render->win=&content;
  uint8_t *initial=runtime_unit_test_rgba(3,2,2,1);
  CHECK(initial);
  if(partial) initial[3]=127u;
  int sprite=gml_sprite_append_from_rgba_frames(render,initial,3,2,2,0,0,NULL);
  int second=gml_sprite_append_from_rgba(render,
    runtime_unit_test_rgba(2,3,1,0),2,3,0,0,NULL);
  CHECK(sprite>=0 && second>=0);
  /* Replacement targets a base asset; extra assets exercise delete/reuse below. */
  render->base_n_spr=1;
  render->spr[sprite].runtime_extra=0;
  gml_render_bind_device(render,&device);
  size_t size=gml_render_state_size(render,0),written=0,again_written=0;
  uint8_t *state=malloc(size),*again=malloc(size);
  CHECK(state && again && gml_render_state_save(render,0,state,size,&written) && written==size);
  CHECK(gml_render_runtime_sprite_scene_source_view(render,&render->spr[sprite],0,&one) &&
        gml_render_runtime_sprite_scene_source_view(render,&render->spr[sprite],-2,&same) &&
        gml_render_runtime_sprite_scene_source_view(render,&render->spr[second],0,&other));
  CHECK(one.bytes==render->spr[sprite].runtime_rgba && one.borrowed_immutable &&
        one.width==3u && one.height==2u && one.stride_bytes==12u && one.byte_capacity==24u &&
        one.image.identity==UINT32_C(0x80000000) && one.image.incarnation &&
        one.image.incarnation==same.image.incarnation &&
        one.image.incarnation!=other.image.incarnation);
  CHECK(gml_render_state_size(render,0)==size &&
        gml_render_state_save(render,0,again,size,&again_written) && again_written==size &&
        !memcmp(state,again,size));
  device.observe_on_demand=fake_observe_on_demand;
  gml_render_bind_device(render,&device);
  gml_render_application_surface_bind(render,base,8,8,1);
  CHECK(gml_render_begin_resident_base(render,base,8,8,0,0));
  gml_draw_sprite_ext(render,sprite,1,2,2,1,1,0,0xFFFFFFu,1);
  CHECK(render->resident_application_pass && !fake.submits);
  uint8_t *old=render->spr[sprite].runtime_rgba;
  uint8_t *replacement=runtime_unit_test_rgba(4,1,1,0);
  CHECK(replacement);
  fake.fail_upload=1;
  CHECK(!gml_sprite_replace_from_rgba(render,sprite,replacement,4,1,0,0) &&
        render->spr[sprite].runtime_rgba==old && !fake.retires &&
        render->spr[sprite].runtime_scene_incarnation==one.image.incarnation);
  fake.fail_upload=0;
  CHECK(gml_sprite_replace_from_rgba(render,sprite,replacement,4,1,0,0) &&
        fake.submits==1 && fake.retires==2 && !fake.readbacks &&
        fake.last_upload_rgba[0]==(uint8_t)(31u+6u*17u) &&
        fake.last_retired_image.incarnation==one.image.incarnation &&
        !render->spr[sprite].runtime_scene_used);
  CHECK(gml_render_runtime_sprite_scene_source_view(render,&render->spr[sprite],0,&renewed) &&
        renewed.image.incarnation!=one.image.incarnation && renewed.width==4u && renewed.height==1u);
  gml_draw_sprite_ext(render,sprite,0,2,2,1,1,0,0xFFFFFFu,1);
  CHECK(gml_sprite_assign(render,sprite,second) && fake.submits==2 && fake.retires==3 &&
        render->spr[sprite].w==2 && render->spr[sprite].h==3);
  gml_draw_sprite_ext(render,sprite,0,2,2,1,1,0,0xFFFFFFu,1);
  CHECK(gml_sprite_set_alpha_from_sprite(render,sprite,second) && fake.submits==3 &&
        fake.retires==4 && !render->spr[sprite].runtime_scene_binary && !fake.readbacks);
  CHECK(gml_sprite_assign(render,sprite,second));
  gml_draw_sprite_ext(render,sprite,0,2,2,1,1,0,0xFFFFFFu,1);
  size_t used=0;
  CHECK(gml_render_state_load(render,state,size,&used) && used==size && fake.submits==4 &&
        render->spr[sprite].w==3 && render->spr[sprite].h==2 &&
        render->spr[sprite].n_frames==2 &&
        !!render->spr[sprite].runtime_scene_binary==!partial &&
        !render->spr[sprite].runtime_scene_used && !fake.readbacks);
  CHECK(!render->resident_application_pass &&
        gml_render_begin_resident_base(render,base,8,8,0,0));
  gml_draw_sprite_ext(render,sprite,0,2,2,1,1,0,0xFFFFFFu,1);
  CHECK(render->resident_application_pass);
  gml_render_clear_runtime_sprites(render);
  CHECK(render->n_spr==1 && !render->spr[sprite].runtime_rgba &&
        fake.submits==5 && !fake.readbacks);
  int reused=gml_sprite_append_from_rgba(render,runtime_unit_test_rgba(2,2,1,0),2,2,0,0,NULL);
  CHECK(reused==second &&
        gml_render_runtime_sprite_scene_source_view(render,&render->spr[reused],0,&renewed) &&
        renewed.image.incarnation!=one.image.incarnation);
  gml_draw_sprite_ext(render,reused,0,2,2,1,1,0,0xFFFFFFu,1);
  gml_sprite_delete(render,reused);
  CHECK(fake.submits==6 && !render->spr[reused].runtime_rgba);
  int reused_again=gml_sprite_append_from_rgba(render,runtime_unit_test_rgba(2,2,1,0),2,2,0,0,NULL);
  CHECK(reused_again==reused);
  gml_draw_sprite_ext(render,reused_again,0,2,2,1,1,0,0xFFFFFFu,1);
  int before=fake.submits;
  gml_render_free(render);
  CHECK(fake.submits==before+1 && !fake.readbacks);
  free(render);free(state);free(again);
  return 1;
}

static int runtime_sprite_scene_lifetimes(void){
  return runtime_sprite_scene_lifetimes_variant(0) &&
    runtime_sprite_scene_lifetimes_variant(1);
}

static int resident_classic_clipped_sprite_noops(void){
  const double anchors[12][2]={{8.0,1.0},{1.0,8.0},{-2.0,1.0},{1.0,-2.0},
    {7.5,1.0},{1.0,7.5},{-2.5,1.0},{1.0,-2.5},
    {7.499,1.0},{1.0,7.499},{-1.5,1.0},{1.0,-1.5}};
  for(unsigned generation=0;generation<2u;generation++)
  for(unsigned numbered=0;numbered<2u;numbered++)
  for(unsigned source_kind=0;source_kind<3u;source_kind++)
  for(unsigned signs=0;signs<4u;signs++)
  for(unsigned placement=0;placement<(!signs && source_kind==2u?8u:12u);placement++){
    GmlRender *render=calloc(1,sizeof *render);
    GmlWin content={.classic_version=generation?810:800};
    FakeDevice fake={0};
    GmlRenderDeviceServices device=service(&fake);
    uint32_t base[64],reference[64];
    const uint32_t *observed=NULL;
    uint8_t rgba[32]={208,32,16,255,16,208,32,255,32,16,208,255,192,176,160,255,
                      16,208,32,255,208,32,16,255,192,176,160,255,32,16,208,255};
    CHECK(render && gml_render_init(render,NULL)==0);
    render->win=&content;render->classic=1;
    CHECK(render->n_atlas==1);
    free(render->atlas[0].px);
    memset(render->atlas,0,sizeof *render->atlas);
    render->atlas[0].w=4;render->atlas[0].h=2;
    render->atlas[0].px=malloc(sizeof rgba);
    render->tpag=calloc(2,sizeof *render->tpag);
    render->spr=calloc(2,sizeof *render->spr);
    CHECK(render->atlas[0].px && render->tpag && render->spr);
    if(source_kind) rgba[11]=source_kind==1u?0:128;
    memcpy(render->atlas[0].px,rgba,sizeof rgba);
    render->n_tpag=render->n_spr=2;
    render->tpag[0]=(GmlTpag){.sw=2,.sh=2,.tw=2,.th=2,.bw=2,.bh=2};
    render->tpag[1]=(GmlTpag){.sx=2,.sw=2,.sh=2,.tw=2,.th=2,.bw=4,.bh=4,.tx=1,.ty=1};
    for(unsigned index=0;index<2u;index++){
      render->spr[index].w=render->spr[index].h=index?4:2;
      render->spr[index].originx=render->spr[index].originy=index?2:0;
      render->spr[index].n_frames=1;
      render->spr[index].frame=calloc(1,sizeof(int));
      CHECK(render->spr[index].frame);
      render->spr[index].frame[0]=(int)index;
    }
    uint32_t initial=numbered?0x40123456u:0xFF123456u;
    for(unsigned index=0;index<64u;index++) base[index]=reference[index]=initial;
    gml_render_begin(render,reference,8,8,0.25,-0.25);
    int id=numbered?gml_surface_create(render,8,8):0;
    if(numbered){
      CHECK(id>0 && gml_surface_set_target(render,id));
      memcpy(render->surface[id-1].px,reference,sizeof reference);
    }
    double camx=render->cam_x,camy=render->cam_y;
    double xs=signs&1u?-1.0:1.0,ys=signs&2u?-1.0:1.0;
    double x=anchors[placement][0]+xs+(xs<0.0?2.0:0.0)+camx;
    double y=anchors[placement][1]+ys+(ys<0.0?2.0:0.0)+camy;
    gml_draw_sprite_ext(render,0,0,2.0+camx,2.0+camy,1.0,1.0,0.0,0xFFFFFFu,1.0);
    gml_draw_sprite_ext(render,1,0,x,y,xs,ys,0.0,0xFFFFFFu,1.0);
    gml_draw_sprite_ext(render,0,0,4.0+camx,4.0+camy,1.0,1.0,0.0,0xFFFFFFu,1.0);
    if(numbered){
      memcpy(reference,render->surface[id-1].px,sizeof reference);
      gml_surface_reset_target(render);
    }
    gml_render_application_surface_bind(render,base,8,8,1);
    gml_render_bind_device(render,&device);
    CHECK(gml_render_begin_resident_base(render,base,8,8,0.25,-0.25));
    if(numbered){
      for(unsigned index=0;index<64u;index++) render->surface[id-1].px[index]=initial;
      CHECK(gml_surface_set_target(render,id));
    }
    CHECK(render->cam_x==camx && render->cam_y==camy);
    gml_draw_sprite_ext(render,0,0,2.0+camx,2.0+camy,1.0,1.0,0.0,0xFFFFFFu,1.0);
    CHECK(render->resident_application_pass && !fake.submits && !fake.readbacks);
    gml_draw_sprite_ext(render,1,0,x,y,xs,ys,0.0,0xFFFFFFu,1.0);
    if(signs && placement>=8u){
      /* Adjacent reflected coverage is visible, so it retains the CPU writer. */
      CHECK(!render->resident_application_pass && fake.submits==1 && !fake.readbacks);
      gml_draw_sprite_ext(render,0,0,4.0+camx,4.0+camy,1.0,1.0,0.0,0xFFFFFFu,1.0);
      CHECK(!memcmp(render->fb,reference,sizeof reference));
      gml_render_abandon_device(render);gml_render_free(render);free(render);
      continue;
    }
    CHECK(render->resident_application_pass && !fake.readbacks);
    if(placement<8u) CHECK(!fake.submits);
    gml_draw_sprite_ext(render,0,0,4.0+camx,4.0+camy,1.0,1.0,0.0,0xFFFFFFu,1.0);
    CHECK(gml_render_scene_submit_pending(render) && fake.submits>0 && !fake.readbacks);
    if(placement<8u) CHECK(fake.submits==1 && fake.last_batch_operations==2u);
    GmlRenderResource *resource=gml_render_scene_target_resource_ensure(render);
    CHECK(resource && gml_render_resource_acquire_cpu_read(resource,&observed) &&
          !memcmp(observed,reference,sizeof reference) && !fake.readbacks);
    gml_render_abandon_device(render);gml_render_free(render);free(render);
  }
  return 1;
}

static int resident_classic_rotated_sprite_noops(void){
  const double rotations[4]={90.0,180.0,270.0,17.0};
  const double anchors[5][2]={{-30.0,4.0},{38.0,4.0},
                             {4.0,-30.0},{4.0,38.0},{4.0,4.0}};
  for(unsigned generation=0;generation<2u;generation++)
  for(unsigned numbered=0;numbered<2u;numbered++)
  for(unsigned source_kind=0;source_kind<3u;source_kind++)
  for(unsigned angle=0;angle<4u;angle++)
  for(unsigned placement=0;placement<5u;placement++)
  for(unsigned reduced=0;reduced<2u;reduced++){
    GmlRender *render=calloc(1,sizeof *render);
    GmlWin content={.classic_version=generation?810:600};
    FakeDevice fake={0};
    GmlRenderDeviceServices device=service(&fake);
    uint32_t base[64],reference[64],prefix[64];
    const uint32_t *observed=NULL;
    uint8_t rgba[32]={208,32,16,255,16,208,32,255,32,16,208,255,192,176,160,255,
                      16,208,32,255,208,32,16,255,192,176,160,255,32,16,208,255};
    CHECK(render && gml_render_init(render,NULL)==0);
    render->win=&content;render->classic=1;
    free(render->atlas[0].px);
    memset(render->atlas,0,sizeof *render->atlas);
    render->atlas[0].w=4;render->atlas[0].h=2;
    render->atlas[0].px=malloc(sizeof rgba);
    render->tpag=calloc(2,sizeof *render->tpag);
    render->spr=calloc(2,sizeof *render->spr);
    CHECK(render->atlas[0].px && render->tpag && render->spr);
    if(source_kind) rgba[11]=source_kind==1u?0:128;
    memcpy(render->atlas[0].px,rgba,sizeof rgba);
    render->n_tpag=render->n_spr=2;
    render->tpag[0]=(GmlTpag){.sw=2,.sh=2,.tw=2,.th=2,.bw=2,.bh=2};
    render->tpag[1]=(GmlTpag){.sx=2,.sw=2,.sh=2,.tw=2,.th=2,.bw=4,.bh=4,.tx=1,.ty=1};
    for(unsigned index=0;index<2u;index++){
      render->spr[index].w=render->spr[index].h=index?4:2;
      render->spr[index].originx=render->spr[index].originy=index?2:0;
      render->spr[index].n_frames=1;
      render->spr[index].frame=calloc(1,sizeof(int));
      CHECK(render->spr[index].frame);
      render->spr[index].frame[0]=(int)index;
    }
    uint32_t initial=numbered?0x40123456u:0xFF123456u;
    for(unsigned index=0;index<64u;index++) base[index]=reference[index]=initial;
    gml_render_begin(render,reference,8,8,0.25,-0.25);
    int id=numbered?gml_surface_create(render,8,8):0;
    if(numbered){
      CHECK(id>0 && gml_surface_set_target(render,id));
      memcpy(render->surface[id-1].px,reference,sizeof reference);
    }
    double camx=render->cam_x,camy=render->cam_y;
    double xs=reduced && placement<4u?0.1506:1.0;
    double ys=reduced && placement<4u?0.2014:1.0;
    double alpha=reduced?0.5:1.0;
    double x=anchors[placement][0]+camx,y=anchors[placement][1]+camy;
    gml_draw_sprite_ext(render,0,0,2.0+camx,2.0+camy,1.0,1.0,0.0,0xFFFFFFu,1.0);
    memcpy(prefix,render->fb,sizeof prefix);
    gml_draw_sprite_ext(render,1,0,x,y,xs,ys,rotations[angle],0xFFFFFFu,alpha);
    CHECK((memcmp(prefix,render->fb,sizeof prefix)==0)==(placement<4u));
    gml_draw_sprite_ext(render,0,0,5.0+camx,5.0+camy,1.0,1.0,0.0,0xFFFFFFu,1.0);
    if(numbered){
      memcpy(reference,render->surface[id-1].px,sizeof reference);
      gml_surface_reset_target(render);
    }
    gml_render_application_surface_bind(render,base,8,8,1);
    gml_render_bind_device(render,&device);
    CHECK(gml_render_begin_resident_base(render,base,8,8,0.25,-0.25));
    if(numbered){
      for(unsigned index=0;index<64u;index++) render->surface[id-1].px[index]=initial;
      CHECK(gml_surface_set_target(render,id));
    }
    gml_draw_sprite_ext(render,0,0,2.0+camx,2.0+camy,1.0,1.0,0.0,0xFFFFFFu,1.0);
    CHECK(render->resident_application_pass && !fake.submits && !fake.readbacks);
    gml_draw_sprite_ext(render,1,0,x,y,xs,ys,rotations[angle],0xFFFFFFu,alpha);
    if(placement==4u){
      CHECK(!render->resident_application_pass && fake.submits==1 && !fake.readbacks);
      gml_draw_sprite_ext(render,0,0,5.0+camx,5.0+camy,1.0,1.0,0.0,0xFFFFFFu,1.0);
      CHECK(!memcmp(render->fb,reference,sizeof reference));
    } else {
      CHECK(render->resident_application_pass && !fake.submits && !fake.readbacks);
      gml_draw_sprite_ext(render,0,0,5.0+camx,5.0+camy,1.0,1.0,0.0,0xFFFFFFu,1.0);
      CHECK(gml_render_scene_submit_pending(render) && fake.submits==1 &&
            fake.last_batch_operations==2u && !fake.readbacks);
      GmlRenderResource *resource=gml_render_scene_target_resource_ensure(render);
      CHECK(resource && gml_render_resource_acquire_cpu_read(resource,&observed) &&
            !memcmp(observed,reference,sizeof reference) && !fake.readbacks);
    }
    gml_render_abandon_device(render);gml_render_free(render);free(render);
  }
  return 1;
}

static int resident_outside_sprite_geometry(void){
  const int generations[]={530,700,800,13,15,17};
  const double angles[]={0,.0005,84,91.5,359.9995,360,720.0005};
  const double scales[][2]={{1,1},{2,3},{.1875,.234375},{64,.0625}};
  GmlRender *r=calloc(1,sizeof *r);GmlWin content={0};
  GmlSprite sprite={.w=13,.h=19,.originx=6,.originy=9};
  GmlTpag page={.sw=8,.sh=15,.tw=8,.th=15,.tx=3,.ty=2};
  CHECK(r);r->win=&content;r->fbw=48;r->fbh=40;r->cam_x=-3;r->cam_y=2;
  /* No framebuffer, atlas, frame bytes or device exist in this geometry check. */
  for(unsigned generation=0;generation<6u;generation++)
  for(unsigned sign=0;sign<4u;sign++)
  for(unsigned scale=0;scale<4u;scale++)
  for(unsigned angle=0;angle<7u;angle++){
    content.classic_version=generations[generation]>100?generations[generation]:0;
    content.bytecode=content.classic_version?0:generations[generation];
    r->classic=content.classic_version!=0;
    double xs=scales[scale][0]*(sign&1u?-1:1),ys=scales[scale][1]*(sign&2u?-1:1);
    double radius=8+4*(sprite.w+sprite.h+32)*fmax(fabs(xs),fabs(ys));
    for(unsigned side=0;side<4u;side++){
      double x=r->cam_x+(side==0u?-radius:side==1u?r->fbw+radius:r->fbw*.5);
      double y=r->cam_y+(side==2u?-radius:side==3u?r->fbh+radius:r->fbh*.5);
      CHECK(gml_render_sprite_source_is_outside(r,&sprite,&page,x,y,xs,ys,angles[angle]));
    }
    CHECK(!gml_render_sprite_source_is_outside(r,&sprite,&page,
      r->cam_x+24,r->cam_y+20,xs,ys,angles[angle]));
  }
  GmlTpag expanded={.tx=-80,.ty=-70,.sw=160,.sh=140,.tw=180,.th=150};
  CHECK(!gml_render_sprite_source_is_outside(r,&sprite,&expanded,80,20,1,1,0));
  CHECK(!gml_render_sprite_source_is_outside(r,&sprite,&page,NAN,200,1,1,0));
  CHECK(!gml_render_sprite_source_is_outside(r,&sprite,&page,200,200,0,1,0));
  CHECK(!gml_render_sprite_source_is_outside(r,&sprite,&page,200,200,1,1,INFINITY));
  CHECK(!gml_render_sprite_source_is_outside(r,&sprite,&page,-11,20,1,1,0));
  /* The conservative rim deliberately keeps a crop touching the edge visible. */
  CHECK(!gml_render_sprite_source_is_outside(r,&sprite,&page,-8,20,1,1,.0005));
  free(r);return 1;
}

static int resident_empty_sprite_preserves_queued_prefix(int zero_scale){
  const int generations[]={530,700,800,13,15,17};
  enum { W=24,H=20 };
  for(unsigned generation=0;generation<6u;generation++)
  for(unsigned target=0;target<4u;target++)
  for(unsigned mapping=0;mapping<3u;mapping++){
    GmlRender *r=calloc(1,sizeof *r);GmlWin content={0};
    FakeDevice fake={.on_demand=1};GmlRenderDeviceServices device=service(&fake);
    uint32_t base[W*H]={0};GmlRenderJournalMetrics before={0},after={0};
    CHECK(r && !gml_render_init(r,NULL));
    content.classic_version=generations[generation]>100?generations[generation]:0;
    content.bytecode=content.classic_version?0:generations[generation];
    r->win=&content;r->classic=content.classic_version!=0;
    r->tpag=calloc(1,sizeof *r->tpag);r->spr=calloc(1,sizeof *r->spr);
    CHECK(r->tpag && r->spr);r->n_tpag=r->n_spr=1;
    r->tpag[0]=(GmlTpag){.sw=2,.sh=2,.tw=2,.th=2,.bw=4,.bh=4,.tx=1,.ty=1};
    r->spr[0]=(GmlSprite){.w=4,.h=4,.originx=2,.originy=2,.n_frames=1};
    r->spr[0].frame=calloc(1,sizeof(int));CHECK(r->spr[0].frame);
    device.observe_on_demand=fake_observe_on_demand;gml_render_bind_device(r,&device);
    if(target==1u || target==2u){
      CHECK(gml_render_application_surface_ensure_owned(r,W,H));
      CHECK(gml_render_begin_resident_owned_application(r,-3,2,0));
    }else{
      gml_render_application_surface_bind(r,base,W,H,1);
      CHECK(gml_render_begin_resident_base(r,base,W,H,-3,2));
    }
    if(target==2u) CHECK(gml_surface_set_target(r,0));
    if(target==3u){int id=gml_surface_create(r,W,H);CHECK(id>0 && gml_surface_set_target(r,id));}
    if(mapping==1u){gml_render_gui_begin(r,W,H);gml_render_gui_set_size(r,W/2,H/2);}
    if(mapping==2u){r->world_transform_active=1;r->world_scale_x=2;r->world_scale_y=3;}
    gml_render_set_pending_fill(r,0xD3273859u);
    GmlRenderResource *resource=gml_render_scene_target_resource_ensure(r);CHECK(resource);
    gml_render_resource_journal_metrics(resource,&before);
    int submits=fake.submits,reads=fake.readbacks;double xs=.1875,ys=.234375;
    double x=r->cam_x+W*.5,y=r->cam_y+200;
    double mx=1,my=1;gml_render_draw_map_scale(r,&mx,&my);
    CHECK(mx>0 && my>0);
    if(zero_scale){
      const double rotations[]={0,.0005,84,91.51002717488725};
      const double axis_rotations[]={0,.0005,360,-360};
      for(unsigned mask=0;mask<16u;mask++)
      for(unsigned axis=0;axis<3u;axis++)
      for(unsigned rotation=0;rotation<4u;rotation++)
      for(unsigned filter=0;filter<2u;filter++){
        r->interp=filter;r->fog_flat=(mask+axis+rotation)%2u;
        r->color_write_mask=(uint8_t)mask;r->blendmode=(mask+axis)%8u;
        r->alphablend=(mask+rotation)%2u;r->alpha_test_enable=1;r->alpha_test_ref=128;
        double zx=axis!=1u?0:-.1875,zy=axis!=0u?0:.234375;
        if(zero_scale==2){
          zx=axis!=1u?0x1p-52:-.1875;zy=axis!=0u?-0x1p-52:.234375;
        }
        gml_draw_sprite_ext(r,0,0,x/mx,(r->cam_y+H*.5)/my,zx/mx,zy/my,
                            zero_scale==2?axis_rotations[rotation]:rotations[rotation],0x70B0D0u,.37);
        CHECK(r->resident_application_pass && fake.submits==submits && fake.readbacks==reads);
        gml_render_resource_journal_metrics(resource,&after);
        CHECK(after.logical_version==before.logical_version && !after.cpu_replays);
      }
      if(zero_scale==2){
        double px=x/mx,py=(r->cam_y+H*.5)/my;
        CHECK(gml_render_resident_sprite_outside(r,0,0,px,py,0x1p-52/mx,1/my,0));
        r->active_shader=0;
        CHECK(!gml_render_resident_sprite_outside(r,0,0,px,py,0x1p-52/mx,1/my,0));
        r->active_shader=-1;r->spr[0].ns_enabled=1;
        CHECK(!gml_render_resident_sprite_outside(r,0,0,px,py,0x1p-52/mx,1/my,0));
        r->spr[0].ns_enabled=0;r->pending_underlay=1;
        CHECK(!gml_render_resident_sprite_outside(r,0,0,px,py,0x1p-52/mx,1/my,0));
        r->pending_underlay=0;
        CHECK(!gml_render_resident_sprite_outside(r,0,0,px,py,0x1p-52/mx,1/my,INFINITY));
      }
      r->fog_flat=0;r->color_write_mask=5;r->active_shader=0;
      CHECK(!gml_render_resident_masked_sprite(r,0,0,x,y,0,1,84,0xFFFFFFu,1));
      r->active_shader=-1;r->spr[0].ns_enabled=1;
      CHECK(!gml_render_resident_masked_sprite(r,0,0,x,y,0,1,84,0xFFFFFFu,1));
      r->spr[0].ns_enabled=0;
      CHECK(!gml_render_resident_masked_sprite(r,0,0,x,y,0,1,INFINITY,0xFFFFFFu,1));
      gml_render_free(r);free(r);continue;
    }
    for(unsigned state=0;state<4u;state++){
      r->interp=state&1u;r->fog_flat=state==3u;r->color_write_mask=state>=2u?5:15;
      r->blendmode=2;r->alpha_test_enable=1;r->alpha_test_ref=128;
      gml_draw_sprite_ext(r,0,0,x/mx,y/my,xs/mx,ys/my,91.5,0x70B0D0u,.37);
      CHECK(r->resident_application_pass && fake.submits==submits && fake.readbacks==reads);
      gml_render_resource_journal_metrics(resource,&after);
      CHECK(after.logical_version==before.logical_version && !after.cpu_replays);
    }
    r->fog_flat=0;r->color_write_mask=15;
    CHECK(gml_render_resident_sprite_outside(r,0,0,x/mx,y/my,xs/mx,ys/my,91.5));
    r->active_shader=0;
    CHECK(!gml_render_resident_sprite_outside(r,0,0,x/mx,y/my,xs/mx,ys/my,91.5));
    r->active_shader=-1;r->spr[0].ns_enabled=1;
    CHECK(!gml_render_resident_sprite_outside(r,0,0,x/mx,y/my,xs/mx,ys/my,91.5));
    uint32_t phase=0;r->spr[0].ns_enabled=0;r->classic_phase_y=&phase;
    /* The borrowed root plane cannot receive pixels from a distinct positive
     * numbered target, even when an outside no-op preserves its prefix. */
    int phase_noop=gml_render_resident_sprite_outside(r,0,0,x/mx,y/my,xs/mx,ys/my,91.5);
    CHECK(phase_noop==(target==3u));
    gml_render_resource_journal_metrics(resource,&after);
    CHECK(after.logical_version==before.logical_version && !after.cpu_replays &&
          fake.submits==submits && fake.readbacks==reads && phase==0u);
    r->classic_phase_y=NULL;gml_render_free(r);free(r);
  }
  return 1;
}

static int resident_outside_sprite_preserves_queued_prefix(void){
  return resident_empty_sprite_preserves_queued_prefix(0);
}

static int resident_vertical_phase_binding_scope(void){
  GmlRender *r=calloc(1,sizeof *r);
  uint32_t root=0,numbered=0,phase=0;
  CHECK(r && !gml_render_classic_vertical_phase_blocks_scene(NULL));
  r->fb=r->base_fb=&root;r->target_id=-1;
  CHECK(!gml_render_classic_vertical_phase_blocks_scene(r));
  r->classic_phase_y=&phase;
  CHECK(gml_render_classic_vertical_phase_blocks_scene(r));
  r->target_id=0;r->target_sp=1;r->fb=&numbered;
  CHECK(gml_render_classic_vertical_phase_blocks_scene(r));
  r->target_id=1;r->target_sp=0;
  CHECK(gml_render_classic_vertical_phase_blocks_scene(r));
  r->target_sp=1;r->fb=&root;
  CHECK(gml_render_classic_vertical_phase_blocks_scene(r));
  r->fb=&numbered;
  CHECK(!gml_render_classic_vertical_phase_blocks_scene(r));
  CHECK(!root && !numbered && !phase);
  free(r);return 1;
}

static int resident_zero_scale_sprite_preserves_queued_prefix(void){
  return resident_empty_sprite_preserves_queued_prefix(1);
}

static int resident_rounded_empty_axis_preserves_queued_prefix(void){
  return resident_empty_sprite_preserves_queued_prefix(2);
}

static int resident_rounded_empty_axis_geometry(void){
  GmlWin content={.bytecode=17,.has_room_layers=1};
  GmlRender render={.win=&content,.fbw=48,.fbh=40,.target_id=-1,.alphablend=1};
  GmlSprite sprite={.w=8,.h=16};GmlTpag page={.sw=8,.sh=16};
  for(unsigned signs=0;signs<4u;signs++){
    double xs=(signs&1u?-1.0:1.0)*0x1p-52;
    double ys=(signs&2u?-1.0:1.0)*0x1p-52;
    CHECK(gml_render_sprite_axis_is_empty(&render,&sprite,&page,12.25,10.25,xs,ys,0));
    CHECK(!gml_render_sprite_axis_is_empty(&render,&sprite,&page,12.25,10.25,xs,ys,17));
  }
  CHECK(gml_render_sprite_axis_is_empty(&render,&sprite,&page,12.25,10.25,.25/8,.25/16,0));
  page.project_authored_edges=1;
  CHECK(!gml_render_sprite_axis_is_empty(&render,&sprite,&page,12.25,10.25,.25/8,.25/16,0));
  page.project_authored_edges=0;
  CHECK(!gml_render_sprite_axis_is_empty(&render,&sprite,&page,12,10,.5/8,.5/16,0));
  CHECK(gml_render_sprite_axis_is_empty(&render,&sprite,&page,12,10,
    nextafter(.5/8,0.0),nextafter(.5/16,0.0),0));
  CHECK(!gml_render_sprite_axis_is_empty(&render,&sprite,&page,NAN,10,0x1p-52,1,0));
  CHECK(!gml_render_sprite_axis_is_empty(&render,&sprite,&page,12,10,0x1p-52,INFINITY,0));
  CHECK(!gml_render_sprite_axis_is_empty(&render,&sprite,&page,12,10,0x1p-52,1,NAN));
  page.sw=page.sh=4096;page.project_authored_edges=1;
  CHECK(!gml_render_sprite_axis_is_empty(&render,&sprite,&page,12,9.5,64,.75/4096,0));
  render.interp=1;
  CHECK(gml_render_sprite_axis_is_empty(&render,&sprite,&page,12,9.5,64,.75/4096,0));
  return 1;
}

static int resident_classic_depth_free_sprites(void){
  enum { WIDTH=64,HEIGHT=48 };
  for(unsigned target=0;target<3;target++)
  for(unsigned trim=0;trim<2;trim++)
  for(unsigned scale=1;scale<=4;scale++)
  for(unsigned culling=0;culling<2;culling++)
  for(unsigned zwrite=0;zwrite<2;zwrite++){
    GmlRender *r=calloc(1,sizeof *r);
    GmlSoftware3D *graphics=gml_software3d_create();
    FakeDevice fake={0};
    GmlRenderDeviceServices device=service(&fake);
    uint32_t base[WIDTH*HEIGHT],reference[WIDTH*HEIGHT];
    const uint32_t *pixels=NULL;
    CHECK(r && graphics && gml_render_init(r,NULL)==0);
    gml_software3d_reset(graphics);
    gml_software3d_set_classic(graphics,1);
    gml_render_bind_software3d(r,graphics);r->classic=1;
    free(r->atlas[0].px);memset(r->atlas,0,sizeof *r->atlas);
    r->atlas[0].w=7;r->atlas[0].h=5;r->atlas[0].px=malloc(7*5*4);
    r->tpag=calloc(1,sizeof *r->tpag);r->spr=calloc(1,sizeof *r->spr);
    CHECK(r->atlas[0].px && r->tpag && r->spr);
    for(unsigned i=0;i<7*5;i++){
      uint8_t *p=r->atlas[0].px+i*4;
      p[0]=(i*43+7)&255;p[1]=(i*17+31)&255;p[2]=(i*61+11)&255;p[3]=255;
    }
    r->n_tpag=r->n_spr=1;
    r->tpag[0]=(GmlTpag){.sx=1,.sy=1,.sw=5,.sh=3,.tw=5,.th=3,
      .tx=trim,.ty=trim,.bw=5+trim*2,.bh=3+trim*2};
    r->spr[0]=(GmlSprite){.w=5+trim*2,.h=3+trim*2,.originx=2,.originy=1,
      .n_frames=1,.frame=calloc(1,sizeof(int))};
    CHECK(r->spr[0].frame);
    if(target==1) CHECK(gml_render_application_surface_ensure_owned(r,WIDTH,HEIGHT) &&
                       gml_render_begin_owned_application(r,0,0,0));
    else gml_render_begin(r,base,WIDTH,HEIGHT,0,0);
    int id=target==2?gml_surface_create(r,WIDTH,HEIGHT):0;
    if(target==2) CHECK(id>0 && gml_surface_set_target(r,id));
    gml_software3d_begin(r,0,0,WIDTH,HEIGHT,0);
    GmlSoftware3DControl control={.ortho=1,.ortho_w=(double)WIDTH/scale,
      .ortho_h=(double)HEIGHT/scale,.culling=(int)culling,.zwrite=(int)zwrite};
    gml_software3d_control_update(graphics,&control,GML_SOFTWARE3D_CONTROL_ORTHOGRAPHIC |
      GML_SOFTWARE3D_CONTROL_HIDDEN | GML_SOFTWARE3D_CONTROL_CULLING |
      GML_SOFTWARE3D_CONTROL_ZWRITE | GML_SOFTWARE3D_CONTROL_FOG |
      GML_SOFTWARE3D_CONTROL_LIGHTING);
    gml_render_set_pending_fill(r,0xA5123456u);
    gml_draw_sprite_ext(r,0,0,3,2,1,1,0,0xFFFFFFu,1);
    gml_draw_sprite_ext(r,0,0,7,6,1,1,0,0xFFFFFFu,1);
    gml_render_flush_pending_fill(r);memcpy(reference,r->fb,sizeof reference);
    if(target==2) gml_surface_reset_target(r);
    gml_render_bind_device(r,&device);
    if(target==1) CHECK(gml_render_begin_resident_owned_application(r,0,0,0));
    else{
      gml_render_application_surface_bind(r,base,WIDTH,HEIGHT,1);
      CHECK(gml_render_begin_resident_base(r,base,WIDTH,HEIGHT,0,0));
      if(target==2) CHECK(gml_surface_set_target(r,id));
    }
    gml_render_set_pending_fill(r,0xA5123456u);
    gml_draw_sprite_ext(r,0,0,3,2,1,1,0,0xFFFFFFu,1);
    gml_draw_sprite_ext(r,0,0,7,6,1,1,0,0xFFFFFFu,1);
    CHECK(r->resident_application_pass && !fake.readbacks);
    CHECK(gml_render_scene_submit_pending(r) && fake.last_batch_operations==2);
    GmlRenderResource *resource=gml_render_scene_target_resource_ensure(r);
    CHECK(resource && gml_render_resource_acquire_cpu_read(resource,&pixels) &&
          !memcmp(pixels,reference,sizeof reference) && !fake.readbacks);
    double rectangle[4]={11,22,33,44};
    const double sentinel[4]={11,22,33,44};
    CHECK(!gml_software3d_project_sprite_axis(r,0,-1,3,2,rectangle) &&
          !gml_software3d_project_sprite_axis(r,0,1024,3,2,rectangle) &&
          !gml_software3d_project_sprite_axis(r,65536,0,3,2,rectangle) &&
          !memcmp(rectangle,sentinel,sizeof rectangle));
    control.hidden=1;
    gml_software3d_control_update(graphics,&control,GML_SOFTWARE3D_CONTROL_HIDDEN);
    CHECK(!gml_software3d_project_sprite_axis(r,0,0,3,2,rectangle) &&
          !memcmp(rectangle,sentinel,sizeof rectangle));
    control.hidden=0;control.fog=1;
    gml_software3d_control_update(graphics,&control,GML_SOFTWARE3D_CONTROL_HIDDEN |
                                                     GML_SOFTWARE3D_CONTROL_FOG);
    CHECK(!gml_software3d_project_sprite_axis(r,0,0,3,2,rectangle));
    control.fog=0;control.lighting=1;
    gml_software3d_control_update(graphics,&control,GML_SOFTWARE3D_CONTROL_FOG |
                                                     GML_SOFTWARE3D_CONTROL_LIGHTING);
    CHECK(!gml_software3d_project_sprite_axis(r,0,0,3,2,rectangle));
    control.lighting=0;control.ortho_angle=15;
    gml_software3d_control_update(graphics,&control,GML_SOFTWARE3D_CONTROL_LIGHTING |
                                                     GML_SOFTWARE3D_CONTROL_ORTHOGRAPHIC);
    CHECK(!gml_software3d_project_sprite_axis(r,0,0,3,2,rectangle));
    control.ortho_angle=0;
    gml_software3d_control_update(graphics,&control,GML_SOFTWARE3D_CONTROL_ORTHOGRAPHIC);
    r->interp=1;
    CHECK(!gml_software3d_project_sprite_axis(r,0,0,3,2,rectangle));
    r->interp=0;
    double matrix[16];gml_software3d_matrix_identity(matrix);matrix[12]=1;
    CHECK(gml_software3d_matrix_set(r,GML_SOFTWARE3D_MATRIX_TRANSFORM,matrix) &&
          !gml_software3d_project_sprite_axis(r,0,0,3,2,rectangle));
    gml_render_abandon_device(r);gml_render_free(r);free(r);
    gml_software3d_destroy(graphics);
  }
  return 1;
}

static int resident_classic_depth_free_glyphs(void){
  enum { WIDTH=64,HEIGHT=64 };
  for(unsigned generation=0;generation<2;generation++)
  for(unsigned target=0;target<3;target++)
  for(unsigned binary=0;binary<2;binary++)
  for(unsigned scale=1;scale<=4;scale++)
  for(unsigned culling=0;culling<2;culling++)
  for(unsigned zwrite=0;zwrite<2;zwrite++){
    GmlRender *r=calloc(1,sizeof *r);
    GmlWin content={.classic_version=generation?810:800};
    GmlSoftware3D *graphics=gml_software3d_create();
    FakeDevice fake={0};
    GmlRenderDeviceServices device=service(&fake);
    uint32_t base[WIDTH*HEIGHT],reference[WIDTH*HEIGHT];
    const uint32_t *pixels=NULL;
    CHECK(r && graphics && gml_render_init(r,NULL)==0);
    r->win=&content;r->classic=1;
    gml_software3d_reset(graphics);gml_software3d_set_classic(graphics,1);
    gml_render_bind_software3d(r,graphics);
    free(r->atlas[0].px);memset(r->atlas,0,sizeof *r->atlas);
    r->atlas[0].w=7;r->atlas[0].h=5;r->atlas[0].px=malloc(7*5*4);
    CHECK(r->atlas[0].px);
    for(unsigned i=0;i<7*5;i++){
      uint8_t *p=r->atlas[0].px+i*4;
      p[0]=(i*43+7)&255;p[1]=(i*17+31)&255;p[2]=(i*61+11)&255;
      p[3]=binary && i%3==0?0:255;
    }
    r->n_fonts=1;r->font=0;r->halign=1;
    GmlFont *font=&r->fonts[0];
    font->real=1;font->sprite=-1;font->atlas=0;
    font->line_height=font->align_height=3;
    font->glyphs=calloc(1,sizeof *font->glyphs);CHECK(font->glyphs);
    for(int i=0;i<256;i++) font->glyph_by_char[i]=-1;
    font->n_glyphs=1;font->glyph_by_char['A']=0;
    font->glyphs[0]=(GmlGlyph){.sx=1,.sy=1,.w=5,.h=3,.shift=5,.ch='A'};
    if(target==1) CHECK(gml_render_application_surface_ensure_owned(r,WIDTH,HEIGHT) &&
                       gml_render_begin_owned_application(r,0,0,0));
    else gml_render_begin(r,base,WIDTH,HEIGHT,0,0);
    int id=target==2?gml_surface_create(r,WIDTH,HEIGHT):0;
    if(target==2) CHECK(id>0 && gml_surface_set_target(r,id));
    gml_software3d_begin(r,0,0,WIDTH,HEIGHT,0);
    GmlSoftware3DControl control={.ortho=1,.ortho_w=(double)WIDTH/scale,
      .ortho_h=(double)HEIGHT/scale,.culling=(int)culling,.zwrite=(int)zwrite};
    gml_software3d_control_update(graphics,&control,GML_SOFTWARE3D_CONTROL_ORTHOGRAPHIC |
      GML_SOFTWARE3D_CONTROL_HIDDEN | GML_SOFTWARE3D_CONTROL_CULLING |
      GML_SOFTWARE3D_CONTROL_ZWRITE | GML_SOFTWARE3D_CONTROL_FOG |
      GML_SOFTWARE3D_CONTROL_LIGHTING);
    gml_render_set_pending_fill(r,0xA5123456u);
    gml_draw_text_ext_transformed(r,8,4,"AA",4,100,1,1,0,0xFFFFFFu,1);
    gml_draw_text_ext_transformed(r,9,5,"AA",4,100,1,1,0,0xFFFFFFu,1);
    gml_render_flush_pending_fill(r);memcpy(reference,r->fb,sizeof reference);
    if(target==2) gml_surface_reset_target(r);
    gml_render_bind_device(r,&device);
    if(target==1) CHECK(gml_render_begin_resident_owned_application(r,0,0,0));
    else{
      gml_render_application_surface_bind(r,base,WIDTH,HEIGHT,1);
      CHECK(gml_render_begin_resident_base(r,base,WIDTH,HEIGHT,0,0));
      if(target==2) CHECK(gml_surface_set_target(r,id));
    }
    gml_render_set_pending_fill(r,0xA5123456u);
    gml_draw_text_ext_transformed(r,8,4,"AA",4,100,1,1,0,0xFFFFFFu,1);
    gml_draw_text_ext_transformed(r,9,5,"AA",4,100,1,1,0,0xFFFFFFu,1);
    CHECK(r->resident_application_pass && !fake.readbacks);
    CHECK(gml_render_scene_submit_pending(r) && fake.last_batch_operations==4);
    GmlRenderResource *resource=gml_render_scene_target_resource_ensure(r);
    CHECK(resource && gml_render_resource_acquire_cpu_read(resource,&pixels) &&
          !memcmp(pixels,reference,sizeof reference) && !fake.readbacks);
    double rectangle[4]={11,22,33,44};
    const double sentinel[4]={11,22,33,44};
    CHECK(!gml_software3d_project_atlas_part_axis(r,0,3,4,4,rectangle) &&
          !gml_software3d_project_atlas_part_axis(r,5,256,4,4,rectangle) &&
          !gml_software3d_project_atlas_part_axis(r,5,3,4.125,4,rectangle) &&
          !memcmp(rectangle,sentinel,sizeof rectangle));
    control.hidden=1;
    gml_software3d_control_update(graphics,&control,GML_SOFTWARE3D_CONTROL_HIDDEN);
    CHECK(!gml_software3d_project_atlas_part_axis(r,5,3,4,4,rectangle));
    control.hidden=0;control.fog=1;
    gml_software3d_control_update(graphics,&control,GML_SOFTWARE3D_CONTROL_HIDDEN |
                                                     GML_SOFTWARE3D_CONTROL_FOG);
    CHECK(!gml_software3d_project_atlas_part_axis(r,5,3,4,4,rectangle));
    control.fog=0;control.lighting=1;
    gml_software3d_control_update(graphics,&control,GML_SOFTWARE3D_CONTROL_FOG |
                                                     GML_SOFTWARE3D_CONTROL_LIGHTING);
    CHECK(!gml_software3d_project_atlas_part_axis(r,5,3,4,4,rectangle));
    control.lighting=0;
    gml_software3d_control_update(graphics,&control,GML_SOFTWARE3D_CONTROL_LIGHTING);
    r->interp=1;
    CHECK(!gml_software3d_project_atlas_part_axis(r,5,3,4,4,rectangle));
    r->interp=0;
    double matrix[16];gml_software3d_matrix_identity(matrix);matrix[12]=1;
    CHECK(gml_software3d_matrix_set(r,GML_SOFTWARE3D_MATRIX_TRANSFORM,matrix) &&
          !gml_software3d_project_atlas_part_axis(r,5,3,4,4,rectangle) &&
          !memcmp(rectangle,sentinel,sizeof rectangle));
    gml_render_abandon_device(r);gml_render_free(r);free(r);
    gml_software3d_destroy(graphics);
  }
  return 1;
}

static int resident_studio_unit_background_noops(void){
  enum { WIDTH=16,HEIGHT=12 };
  const double anchors[12][2]={{WIDTH,4},{4,HEIGHT},{-2,4},{4,-2},
    {WIDTH+2,4},{4,HEIGHT+2},{-4,4},{4,-4},
    {WIDTH-1,4},{4,HEIGHT-1},{-1,4},{4,-1}};
  for(unsigned generation=0;generation<2u;generation++)
  for(unsigned target=0;target<3u;target++)
  for(unsigned trimmed=0;trimmed<2u;trimmed++)
  for(unsigned binary=0;binary<2u;binary++)
  for(unsigned placement=0;placement<12u;placement++){
    GmlRender *render=calloc(1,sizeof *render);
    GmlWin content={.bytecode=generation?17:15};
    FakeDevice fake={0};
    GmlRenderDeviceServices device=service(&fake);
    uint32_t base[WIDTH*HEIGHT],reference[WIDTH*HEIGHT],prefix[WIDTH*HEIGHT];
    const uint32_t *observed=NULL;
    uint8_t rgba[32]={208,32,16,255,16,208,32,255,32,16,208,255,192,176,160,255,
                      16,208,32,255,208,32,16,255,192,176,160,255,32,16,208,255};
    CHECK(render && gml_render_init(render,NULL)==0);
    render->win=&content;
    free(render->atlas[0].px);
    memset(render->atlas,0,sizeof *render->atlas);
    render->atlas[0].w=4;render->atlas[0].h=2;
    render->atlas[0].px=malloc(sizeof rgba);
    render->tpag=calloc(2,sizeof *render->tpag);
    render->bg=calloc(2,sizeof *render->bg);
    CHECK(render->atlas[0].px && render->tpag && render->bg);
    if(binary) rgba[11]=0;
    memcpy(render->atlas[0].px,rgba,sizeof rgba);
    render->n_tpag=render->n_bg=2;
    render->tpag[0]=(GmlTpag){.sw=2,.sh=2,.tw=2,.th=2,.bw=2,.bh=2};
    render->tpag[1]=(GmlTpag){.sx=2,.sw=2,.sh=2,.tw=2,.th=2,
      .bw=trimmed?4:2,.bh=trimmed?4:2,.tx=(int)trimmed,.ty=(int)trimmed};
    render->bg[1].tpag=1;
    for(unsigned pixel=0;pixel<WIDTH*HEIGHT;pixel++) base[pixel]=0xA5ABCDEFu;
    if(target==1u){
      CHECK(gml_render_application_surface_ensure_owned(render,WIDTH,HEIGHT) &&
            gml_render_begin_owned_application(render,0.25,-0.25,0));
    }else gml_render_begin(render,base,WIDTH,HEIGHT,0.25,-0.25);
    int id=target==2u?gml_surface_create(render,WIDTH,HEIGHT):0;
    if(target==2u) CHECK(id>0 && gml_surface_set_target(render,id));
    double camx=render->cam_x,camy=render->cam_y;
    double x=anchors[placement][0]-trimmed+camx;
    double y=anchors[placement][1]-trimmed+camy;
    gml_render_set_pending_fill(render,0xFF123456u);
    gml_draw_background_tiled(render,0,2+camx,2+camy,0,0);
    memcpy(prefix,render->fb,sizeof prefix);
    gml_draw_background_tiled(render,1,x,y,0,0);
    CHECK((memcmp(prefix,render->fb,sizeof prefix)!=0)==(placement>=8u));
    gml_draw_background_tiled(render,0,8+camx,7+camy,0,0);
    memcpy(reference,render->fb,sizeof reference);
    if(target==2u) gml_surface_reset_target(render);
    gml_render_bind_device(render,&device);
    if(target==1u){
      CHECK(gml_render_begin_resident_owned_application(render,0.25,-0.25,0));
    }else{
      gml_render_application_surface_bind(render,base,WIDTH,HEIGHT,1);
      CHECK(gml_render_begin_resident_base(render,base,WIDTH,HEIGHT,0.25,-0.25));
      if(target==2u) CHECK(gml_surface_set_target(render,id));
    }
    CHECK(render->cam_x==camx && render->cam_y==camy);
    gml_render_set_pending_fill(render,0xFF123456u);
    gml_draw_background_tiled(render,0,2+camx,2+camy,0,0);
    GmlRenderResource *resource=gml_render_scene_target_resource_ensure(render);
    GmlRenderJournalMetrics before={0},after={0};
    CHECK(resource);
    gml_render_resource_journal_metrics(resource,&before);
    int submits=fake.submits,uploads=fake.uploads;
    gml_draw_background_tiled(render,1,x,y,0,0);
    gml_render_resource_journal_metrics(resource,&after);
    CHECK(render->resident_application_pass && !fake.readbacks && fake.submits==submits);
    if(placement<8u) CHECK(fake.uploads==uploads &&
      after.logical_version==before.logical_version && after.authority==before.authority &&
      after.journal_operations==before.journal_operations);
    gml_draw_background_tiled(render,0,8+camx,7+camy,0,0);
    CHECK(gml_render_scene_submit_pending(render) && fake.submits==submits+1 &&
          fake.last_batch_operations==(placement<8u?2u:3u) && !fake.readbacks);
    CHECK(gml_render_resource_acquire_cpu_read(resource,&observed) &&
          !memcmp(observed,reference,sizeof reference) && !fake.readbacks);
    gml_render_abandon_device(render);gml_render_free(render);free(render);
  }
  return 1;
}

static int resident_classic_unit_background_targets(void){
  const double positions[4][2]={{0.0,0.0},{0.5,-0.5},{-1.25,2.75},{-24.5,24.5}};
  for(unsigned generation=0;generation<2u;generation++)
  for(unsigned numbered=0;numbered<2u;numbered++)
  for(unsigned binary=0;binary<2u;binary++)
  for(unsigned excluded=0;excluded<6u;excluded++)
  for(unsigned placement=0;placement<(excluded?1u:4u);placement++)
  for(unsigned axes=0;axes<(excluded?1u:4u);axes++){
    GmlRender *render=calloc(1,sizeof *render);
    GmlWin content={.classic_version=generation?810:800};
    FakeDevice fake={0};
    GmlRenderDeviceServices device=service(&fake);
    uint32_t base[64]={0},reference[64];
    const uint32_t *observed=NULL;
    uint8_t rgba[16]={208,32,16,255,16,208,32,255,32,16,208,255,192,176,160,255};
    uint32_t clear=numbered?0x40123456u:0xFF123456u;
    double xs=excluded==2u?1.5:1.0,alpha=excluded==4u?0.5:1.0;
    uint32_t colour=excluded==3u?0x80C0E0u:0xFFFFFFu;
    CHECK(render && gml_render_init(render,NULL)==0);
    render->win=&content;render->classic=1;render->interp=excluded==1u;
    free(render->atlas[0].px);
    memset(render->atlas,0,sizeof *render->atlas);
    render->atlas[0].w=render->atlas[0].h=2;
    render->atlas[0].px=malloc(sizeof rgba);
    render->tpag=calloc(1,sizeof *render->tpag);
    render->bg=calloc(1,sizeof *render->bg);
    CHECK(render->atlas[0].px && render->tpag && render->bg);
    if(binary) rgba[7]=0;
    if(excluded==5u) rgba[7]=128;
    memcpy(render->atlas[0].px,rgba,sizeof rgba);
    render->n_tpag=render->n_bg=1;
    render->tpag[0]=(GmlTpag){.sw=2,.sh=2,.tw=2,.th=2,.bw=4,.bh=4,.tx=1,.ty=1};
    gml_render_begin(render,base,8,8,0.25,0.75);
    int id=numbered?gml_surface_create(render,8,8):0;
    if(numbered) CHECK(id>0 && gml_surface_set_target(render,id));
    gml_render_set_pending_fill(render,clear);
    gml_draw_background_tiled_ext(render,0,positions[placement][0],positions[placement][1],
                                  xs,1.0,colour,alpha,axes&1u,axes&2u);
    gml_render_flush_pending_fill(render);
    memcpy(reference,render->fb,sizeof reference);
    if(numbered) gml_surface_reset_target(render);
    gml_render_application_surface_bind(render,base,8,8,1);
    gml_render_bind_device(render,&device);
    CHECK(gml_render_begin_resident_base(render,base,8,8,0.25,0.75));
    if(numbered) CHECK(gml_surface_set_target(render,id));
    gml_render_set_pending_fill(render,clear);
    gml_draw_background_tiled_ext(render,0,positions[placement][0],positions[placement][1],
                                  xs,1.0,colour,alpha,axes&1u,axes&2u);
    CHECK(render->resident_application_pass==!excluded && !fake.readbacks);
    if(excluded){
      CHECK(!memcmp(render->fb,reference,sizeof reference));
    }else{
      CHECK(gml_render_scene_submit_pending(render));
      CHECK(fake.submits<=2);
      GmlRenderResource *resource=gml_render_scene_target_resource_ensure(render);
      CHECK(resource && gml_render_resource_acquire_cpu_read(resource,&observed) &&
            !memcmp(observed,reference,sizeof reference) && !fake.readbacks);
    }
    gml_render_free(render);free(render);
  }
  return 1;
}

static int resident_classic_scaled_background_scope(void){
  const int revisions[4]={530,600,700,810};
  for(unsigned policy=0;policy<4u;policy++)
  for(unsigned numbered=0;numbered<2u;numbered++)
  for(unsigned port=0;port<2u;port++)
  for(unsigned variant=0;variant<11u;variant++){
    GmlRender *render=calloc(1,sizeof *render);
    GmlWin content={.classic_version=revisions[policy]};
    uint32_t clear=numbered?0x40123456u:0xFF123456u;
    /* The selected-background function supersedes the old cheap-owner scope.
     * Missing on-demand authority still rejects before the unchanged CPU writer. */
    int rejected=variant==10u;
    FakeDevice fake={.on_demand=!rejected,.readback_colour=clear};
    GmlRenderDeviceServices device=service(&fake);
    uint32_t base[64]={0},reference[64];
    uint8_t rgba[16]={208,32,16,255,16,208,32,255,32,16,208,255,192,176,160,255};
    double xs=variant==6u?-1.5:variant==7u?4.25:1.5;
    double alpha=variant==4u?0.5:1.0;
    uint32_t colour=variant==5u?0x80C0E0u:0xFFFFFFu;
    CHECK(render && gml_render_init(render,NULL)==0);
    render->win=&content;render->classic=1;
    free(render->atlas[0].px);
    memset(render->atlas,0,sizeof *render->atlas);
    render->atlas[0].w=render->atlas[0].h=2;
    render->atlas[0].px=malloc(sizeof rgba);
    render->tpag=calloc(1,sizeof *render->tpag);
    render->bg=calloc(1,sizeof *render->bg);
    CHECK(render->atlas[0].px && render->tpag && render->bg);
    if(variant==3u) rgba[7]=128u;
    memcpy(render->atlas[0].px,rgba,sizeof rgba);
    render->n_tpag=render->n_bg=1;
    render->tpag[0]=(GmlTpag){.sw=2,.sh=2,.tw=2,.th=2,
      .bw=variant==2u?4:2,.bh=2,.tx=variant==2u?1:0};
    device.observe_on_demand=fake_observe_on_demand;
    int id=0;
    for(unsigned gpu=0;gpu<2u;gpu++){
      if(gpu){
        if(numbered) gml_surface_reset_target(render);
        gml_render_application_surface_bind(render,base,8,8,1);
        gml_render_bind_device(render,&device);
        CHECK(gml_render_begin_resident_base(render,base,8,8,0.25,0.75));
        if(numbered) CHECK(gml_surface_set_target(render,id));
      }else{
        gml_render_begin(render,base,8,8,0.25,0.75);
        if(numbered){
          id=gml_surface_create(render,8,8);
          CHECK(id>0 && gml_surface_set_target(render,id));
        }
      }
      render->gui_pass_active=variant==8u;
      render->world_transform_active=variant==9u;
      render->world_scale_x=variant==9u?2.0:1.0;render->world_scale_y=1.0;
      gml_render_set_pending_fill(render,clear);
      double width=variant==1u?1.0:2.0;
      if(port && variant!=1u)
        gml_draw_background_stretched(render,0,2.5,2.5,
          render->tpag[0].bw*xs,1.5,colour,alpha);
      else
        gml_draw_background_part_ext(render,0,0,0,width,2,2.5,2.5,xs,0.75,colour,alpha);
      if(!gpu){
        gml_render_flush_pending_fill(render);
        memcpy(reference,render->fb,sizeof reference);
      }else{
        /* Rejected variants may observe the pending GPU clear before CPU drawing. */
        if(render->resident_application_pass!=!rejected || (!rejected && fake.readbacks))
          fprintf(stderr,"Classic scaled background scope: policy=%u numbered=%u port=%u variant=%u resident=%d reads=%d\n",
                  policy,numbered,port,variant,render->resident_application_pass,fake.readbacks);
        CHECK(render->resident_application_pass==!rejected && (rejected || !fake.readbacks));
        if(rejected){
          if(memcmp(reference,render->fb,sizeof reference))
            for(unsigned pixel=0;pixel<64u;pixel++)
              if(reference[pixel]!=render->fb[pixel]){
                fprintf(stderr,"Classic scaled background fallback: policy=%u numbered=%u port=%u variant=%u pixel=%u CPU=%08x fallback=%08x\n",
                        policy,numbered,port,variant,pixel,reference[pixel],render->fb[pixel]);
                break;
              }
          CHECK(!memcmp(reference,render->fb,sizeof reference));
        }
        else{
          CHECK(gml_render_scene_submit_pending(render));
          GmlRenderResource *resource=gml_render_scene_target_resource_ensure(render);
          GmlRenderJournalMetrics metrics={0};
          CHECK(resource);
          gml_render_resource_journal_metrics(resource,&metrics);
          CHECK(metrics.authority==GML_RENDER_RESOURCE_GPU_RESIDENT &&
            !metrics.cpu_replays && !metrics.journal_bytes && !metrics.retained_mutable_bytes);
        }
      }
    }
    gml_render_free(render);free(render);
  }
  return 1;
}

static int resident_classic_retained_numbered_target(void){
  for(unsigned generation=0;generation<2u;generation++){
    GmlRender *render=calloc(1,sizeof *render);
    GmlWin content={.classic_version=generation?810:800};
    FakeDevice fake={0};
    GmlRenderDeviceServices device=service(&fake);
    uint32_t old_base[64]={0},new_base[126]={0};
    GmlRenderJournalMetrics before={0},after={0};
    CHECK(render && gml_render_init(render,NULL)==0);
    render->win=&content;render->classic=1;
    gml_render_bind_device(render,&device);
    CHECK(gml_render_begin_resident_base(render,old_base,8,8,0.0,0.0));
    int first=gml_surface_create(render,4,3),second=gml_surface_create(render,2,2);
    CHECK(first>0 && second>0 && gml_surface_set_target(render,first) &&
          render->resident_application_pass);
    gml_render_set_pending_fill(render,0xFF315779u);
    gml_render_flush_pending_fill(render);
    CHECK(fake.submits==1 && !fake.readbacks);
    GmlRenderResource *resource=render->surface[first-1].resource;
    CHECK(resource);
    gml_render_resource_journal_metrics(resource,&before);
    /* A new pass retains the actual Step target, not the application surface or old base. */
    CHECK(gml_render_begin_resident_base_retaining_target(
            render,new_base,14,9,2.0,3.0) && render->resident_application_pass &&
          render->resident_scene_pass && render->target_id==first && render->target_sp==1 &&
          render->fb==render->surface[first-1].px && render->fbw==4 && render->fbh==3 &&
          render->base_fb==new_base && render->base_fbw==14 && render->base_fbh==9);
    gml_render_resource_journal_metrics(resource,&after);
    CHECK(after.logical_version==before.logical_version &&
          after.cpu_version==before.cpu_version && after.gpu_version==before.gpu_version &&
          after.cpu_replays==before.cpu_replays && !fake.readbacks);
    /* Classic assignment replaces its target. It must neither nest nor observe the new one. */
    CHECK(gml_surface_set_target(render,second) && render->target_sp==1 &&
          render->target_id==second && render->resident_application_pass && !fake.readbacks);
    gml_render_set_pending_fill(render,0xFFABCDEFu);
    gml_render_flush_pending_fill(render);
    CHECK(fake.submits==2 && !fake.readbacks);
    CHECK(gml_surface_set_target(render,first) && render->target_sp==1 &&
          render->resident_application_pass && !fake.readbacks);
    gml_render_resource_journal_metrics(resource,&after);
    CHECK(after.cpu_version==before.cpu_version && after.cpu_replays==before.cpu_replays &&
          after.logical_version==before.logical_version);
    gml_surface_reset_target(render);
    CHECK(render->target_sp==0 && render->target_id==-1 && render->fb==new_base &&
          render->fbw==14 && render->fbh==9 && render->cam_x==2.0 && render->cam_y==3.0 &&
          render->resident_application_pass && !fake.readbacks);
    CHECK(gml_surface_pixels_read(render,first,NULL,NULL)[0]==0xFF315779u &&
          gml_surface_pixels_read(render,second,NULL,NULL)[0]==0xFFABCDEFu && !fake.readbacks);
    gml_render_free(render);free(render);
  }
  return 1;
}

static int resident_firstgen_nearest_magnification_authority(void){
  for(unsigned variant=0;variant<6u;variant++){
    GmlRender *render=calloc(1,sizeof *render);
    GmlWin content={.bytecode=15};
    FakeDevice fake={.on_demand=variant!=1u};
    GmlRenderDeviceServices device=service(&fake);
    device.observe_on_demand=fake_observe_on_demand;
    GmlRenderJournalMetrics metrics={0};
    CHECK(render && gml_render_init(render,NULL)==0 &&
          gml_render_application_surface_ensure_owned(render,16,16));
    render->win=&content;
    CHECK(render->atlas && render->n_atlas==1);
    free(render->atlas[0].px);
    memset(&render->atlas[0],0,sizeof render->atlas[0]);
    render->tpag=calloc(1,sizeof *render->tpag);
    render->spr=calloc(1,sizeof *render->spr);
    CHECK(render->atlas && render->tpag && render->spr);
    render->n_atlas=render->n_tpag=render->n_spr=1;
    unsigned atlas_width=variant>=4u?12u:8u,atlas_height=variant>=4u?10u:8u;
    render->atlas[0].px=malloc(atlas_width*atlas_height*4u);
    render->spr[0].frame=calloc(1,sizeof(int));
    CHECK(render->atlas[0].px && render->spr[0].frame);
    render->atlas[0].w=(int)atlas_width;render->atlas[0].h=(int)atlas_height;
    for(unsigned index=0;index<atlas_width*atlas_height;index++){
      uint8_t *pixel=render->atlas[0].px+index*4u;
      pixel[0]=208;pixel[1]=32;pixel[2]=16;pixel[3]=128;
    }
    render->tpag[0]=(GmlTpag){.sx=2,.sy=2,.sw=4,.sh=4,.tw=4,.th=4,.bw=4,.bh=4};
    render->spr[0].w=render->spr[0].h=4;render->spr[0].n_frames=1;
    if(variant>=4u){
      render->tpag[0]=(GmlTpag){.sx=2,.sy=2,.sw=8,.sh=6,.tw=8,.th=6,.bw=8,.bh=6};
      render->spr[0].w=8;render->spr[0].h=6;
    }
    render->active_shader=-1;render->color_write_mask=15u;
    gml_render_bind_device(render,&device);
    CHECK(gml_render_begin_resident_owned_application(render,0.0,0.0,0));
    gml_render_set_pending_fill(render,0xFF714A24u);
    gml_render_flush_pending_fill(render);
    CHECK(gml_render_scene_submit_pending(render));
    int submits=fake.submits;
    double scale=variant==2u?5.0:4.0/3.0;
    if(variant>=4u)
      gml_draw_sprite_ext(render,0,0,0.5,0.5,1.0,4.0/3.0,0.0,
                          variant==4u?0xFFFFFFu:0xA8C4E0u,variant==4u?1.0:0.61);
    else
      gml_draw_sprite_ext(render,0,0,0.25,0.25,scale,1.0,variant==3u?10.0:0.0,0xA8C4E0u,0.61);
    if(variant==2u){
      /* The function matrix qualifies this whole-scale fractional refusal;
       * retain explicit device ownership, grouping and zero ordinary reads. */
      CHECK(render->resident_application_pass && fake.submits==submits &&
            gml_render_scene_submit_pending(render) && fake.submits==submits+1 &&
            fake.last_filter==GML_RENDER_FILTER_NEAREST &&
            fake.last_quantisation==GML_RENDER_QUANTISE_INTEGER_TINT_TRUNC &&
            !fake.readbacks && !fake.rgb_queries);
      gml_render_resource_journal_metrics(render->app_surface_resource,&metrics);
      CHECK(metrics.authority==GML_RENDER_RESOURCE_GPU_RESIDENT &&
            !metrics.cpu_replays && !metrics.device_checkpoints && !metrics.journal_bytes);
    }else if(variant==3u){
      CHECK(nearest_sprite_extension_transport(render,&fake,submits));
      CHECK(fake.last_quantisation==GML_RENDER_QUANTISE_INTEGER_TINT_TRUNC);
    }else if(variant>0u && variant<4u){
      CHECK(!render->resident_application_pass);
    }else if(variant==0u){
      gml_draw_sprite_ext(render,0,0,2.25,1.25,scale,1.0,0.0,0xA8C4E0u,0.61);
      CHECK(render->resident_application_pass && fake.submits==submits);
      CHECK(gml_render_scene_submit_pending(render) && fake.submits==submits+1 &&
            fake.last_batch_operations==2u && fake.last_filter==GML_RENDER_FILTER_NEAREST_PHASED &&
            fake.last_quad_state.rgb_equation==GML_RENDER_BLEND_ADD &&
            fake.last_quad_state.alpha_equation==GML_RENDER_BLEND_ADD &&
            !fake.readbacks && !fake.rgb_queries);
      gml_render_resource_journal_metrics(render->app_surface_resource,&metrics);
      CHECK(metrics.authority==GML_RENDER_RESOURCE_GPU_RESIDENT &&
            metrics.cpu_version<metrics.logical_version && metrics.gpu_version==metrics.logical_version &&
            !metrics.journal_operations && !metrics.cpu_replays && !metrics.device_checkpoints);
    }else{
      CHECK(render->resident_application_pass && fake.submits==submits &&
            gml_render_scene_submit_pending(render) && fake.submits==submits+1 &&
            fake.last_batch_operations==1u && !fake.readbacks && !fake.rgb_queries);
      const GmlRenderRect *crop=&fake.last_quad_state.samplers[0].source_region;
      CHECK(crop->x==2 && crop->y==2 && crop->width==8u &&
            crop->height==(variant==4u?7u:6u) &&
            fake.last_quad_state.rgb_source_factor==GML_RENDER_BLEND_SOURCE_ALPHA &&
            fake.last_quad_state.rgb_destination_factor==GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA);
      gml_render_resource_journal_metrics(render->app_surface_resource,&metrics);
      CHECK(metrics.authority==GML_RENDER_RESOURCE_GPU_RESIDENT &&
            !metrics.journal_operations && !metrics.cpu_replays && !metrics.device_checkpoints);
    }
    /* This fake device establishes authority and batching, not actual device pixels. */
    gml_render_abandon_device(render);gml_render_free(render);free(render);
  }
  return 1;
}

static int resident_modern_screen_point_sprite(void){
  for(unsigned variant=0;variant<20u;variant++){
    GmlRender *r=calloc(1,sizeof *r);
    GmlWin content={.bytecode=variant>=14u?16:17};
    FakeDevice fake={.on_demand=variant!=5u && variant!=15u};
    GmlRenderDeviceServices device=service(&fake);
    GmlRenderJournalMetrics metrics={0};
    CHECK(r && gml_render_init(r,NULL)==0 &&
          gml_render_application_surface_ensure_owned(r,64,48));
    r->win=&content;
    free(r->atlas[0].px);memset(&r->atlas[0],0,sizeof r->atlas[0]);
    r->atlas[0].px=malloc(10u*8u*4u);
    r->tpag=calloc(1,sizeof *r->tpag);r->spr=calloc(1,sizeof *r->spr);
    CHECK(r->atlas[0].px && r->tpag && r->spr);
    r->atlas[0].w=10;r->atlas[0].h=8;r->n_tpag=r->n_spr=1;
    for(unsigned p=0;p<80u;p++){
      uint8_t *pixel=r->atlas[0].px+p*4u;
      pixel[0]=208;pixel[1]=32;pixel[2]=16;pixel[3]=variant==12u?255u:128u;
    }
    r->tpag[0]=(GmlTpag){.sx=3,.sy=2,.sw=4,.sh=3,.tw=4,.th=3,.bw=4,.bh=3};
    r->spr[0].w=4;r->spr[0].h=3;r->spr[0].n_frames=1;
    r->spr[0].originx=r->spr[0].originy=1;
    r->spr[0].frame=calloc(1,sizeof(int));CHECK(r->spr[0].frame);
    device.observe_on_demand=fake_observe_on_demand;
    gml_render_bind_device(r,&device);
    CHECK(gml_render_begin_resident_owned_application(r,0,0,0));
    if(variant==6u || variant==17u) gml_render_gui_begin(r,64,48);
    if(variant==7u) r->cam_x=0.5;
    if(variant==8u) r->blend_equation=2;
    if(variant==9u) r->fog_flat=1;
    if(variant==10u) r->color_write_mask=7u;
    if(variant==11u) r->tpag[0].project_authored_edges=1;
    double xs=variant==1u || variant==3u?-5.9:5.9;
    double ys=variant==2u || variant==3u?-5.9:5.9;
    if(variant==4u) xs=ys=0.7;
    if(variant==12u) xs=ys=2.0;
    if(variant>=14u) xs=-5.9;
    if(variant==16u){ xs=-2.0;ys=2.0; }
    if(variant==18u) ys=0.0;
    if(variant==19u) ys=-0x1p-50;
    gml_render_set_pending_fill(r,0x40123456u);
    if(variant>=18u) gml_render_flush_pending_fill(r);
    uint32_t shadow[64*48];
    memcpy(shadow,r->fb,sizeof shadow);
    int submits=fake.submits;
    gml_draw_sprite_ext(r,0,0,20.25,15.25,xs,ys,0,
                        variant==12u?0xA8C4E0u:0u,
                        variant==13u?0.00001:variant==12u?1.0:0.05);
    int nearest_extension=variant==6u || variant==7u || variant==9u || variant==10u ||
                          variant==13u || variant==17u;
    CHECK(r->resident_application_pass==(variant<5u || variant==12u || variant==14u ||
                                        variant==16u || variant>=18u || nearest_extension));
    if(variant>=18u){
      CHECK(gml_render_scene_submit_pending(r) && !fake.readbacks && !fake.rgb_queries &&
            fake.last_opcode!=GML_RENDER_OP_TEXTURED_QUAD);
    }else if(nearest_extension){
      /* The pending clear is a setup write, not a per-sprite flush. */
      CHECK(nearest_sprite_extension_transport(r,&fake,submits+1));
      CHECK(!memcmp(shadow,r->fb,sizeof shadow));
    }else if(variant==12u){
      CHECK(gml_render_scene_submit_pending(r) && !fake.readbacks && !fake.rgb_queries &&
            fake.last_filter==GML_RENDER_FILTER_NEAREST &&
            fake.last_quantisation==GML_RENDER_QUANTISE_INTEGER_TINT_ROUND);
      gml_render_resource_journal_metrics(r->app_surface_resource,&metrics);
      CHECK(metrics.authority==GML_RENDER_RESOURCE_GPU_RESIDENT &&
            !metrics.cpu_replays && !metrics.device_checkpoints && !metrics.journal_bytes);
    } else if(variant<5u || variant==14u || variant==16u){
      CHECK(gml_render_scene_submit_pending(r) && !fake.readbacks && !fake.rgb_queries &&
            fake.last_filter==(variant==16u?GML_RENDER_FILTER_NEAREST:
                             variant>=14u?GML_RENDER_FILTER_NEAREST_PHASED:
                                            GML_RENDER_FILTER_NEAREST_PHASED_OPAQUE_ALPHA) &&
            fake.last_quantisation==(variant==16u?GML_RENDER_QUANTISE_INTEGER_TINT_TRUNC:0u) &&
            fake.last_quad_state.rgb_equation==GML_RENDER_BLEND_ADD &&
            fake.last_quad_state.alpha_equation==GML_RENDER_BLEND_ADD);
      if(variant>=14u) CHECK(fake.last_quad_state.alpha_source_factor==GML_RENDER_BLEND_SOURCE_ALPHA);
      gml_render_resource_journal_metrics(r->app_surface_resource,&metrics);
      CHECK(metrics.authority==GML_RENDER_RESOURCE_GPU_RESIDENT &&
            !metrics.cpu_replays && !metrics.device_checkpoints && !metrics.journal_bytes);
    }
    gml_render_free(r);free(r);
  }
  return 1;
}

static int resident_cached_rotation_trim_tie_metadata(void){
  enum { W=64,H=48 };
  GmlRender *r=calloc(1,sizeof *r);
  GmlWin content={.bytecode=13};
  uint32_t screen[W*H];
  GmlRenderRotatedCoverageRegion regions[2048];
  uint32_t count=0,matched=0;
  CHECK(r && gml_render_init(r,NULL)==0);
  r->win=&content;
  free(r->atlas[0].px);
  r->atlas[0].px=malloc(20u*20u*4u);
  CHECK(r->atlas[0].px);
  r->atlas[0].w=r->atlas[0].h=20;
  for(unsigned pixel=0;pixel<400u;pixel++){
    uint8_t *rgba=r->atlas[0].px+4u*pixel;
    rgba[0]=(uint8_t)(pixel*31u+19u);rgba[1]=(uint8_t)(pixel*17u+53u);
    rgba[2]=(uint8_t)(pixel*13u+101u);rgba[3]=255u;
  }
  r->spr=calloc(1,sizeof *r->spr);r->tpag=calloc(1,sizeof *r->tpag);
  CHECK(r->spr && r->tpag);
  r->n_spr=r->n_tpag=1;
  r->spr[0]=(GmlSprite){.w=13,.h=19,.originx=6,.originy=9,.n_frames=1};
  r->spr[0].frame=calloc(1,sizeof *r->spr[0].frame);
  CHECK(r->spr[0].frame);
  r->tpag[0]=(GmlTpag){.sx=4,.sy=4,.sw=8,.sh=15,.tw=8,.th=15,
                      .tx=3,.ty=2,.bw=13,.bh=19};
  gml_render_begin(r,screen,W,H,0,0);gml_render_gui_begin(r,W,H);
  r->interp=0;
  gml_render_set_pending_fill(r,0xFF25C080u);
  gml_draw_sprite_ext(r,0,0,6,13,2,2,45,0xFFFFFFu,1);
  gml_render_flush_pending_fill(r);
  CHECK(screen[7u*W]==0xFFC1E7CFu);
  CHECK(gml_render_rotated_nearest_texel_regions(r,&r->spr[0],&r->tpag[0],
          6,13,2,2,45,0xFFFFFFu,regions,2048u,&count));
  for(uint32_t index=0;index<count;index++){
    const GmlRenderRotatedCoverageRegion *region=&regions[index];
    if(region->region.x<=0 && (int64_t)region->region.x+region->region.width>0 &&
       region->region.y<=7 && (int64_t)region->region.y+region->region.height>7){
      CHECK(region->source_x==2 && region->source_y==3 &&
            region->sample_key==screen[7u*W]);
      matched++;
    }
  }
  CHECK(matched==1u);
  gml_render_free(r);free(r);
  return 1;
}

/* Fake-device authority/limits only; public paired scenes own all pixel claims. */
static int resident_tiny_nearest_sprite_scope(void){
  enum { W=24,H=20 };
  const int generations[]={530,700,800,13,15,17};
  const double scales[]={.12,.01,0x1p-14,0x1p-15};
  unsigned visible[2]={0};
  for(unsigned generation=0;generation<6u;generation++)
  for(unsigned runtime=0;runtime<2u;runtime++)
  for(unsigned target_kind=0;target_kind<4u;target_kind++)
  for(unsigned gui=0;gui<2u;gui++)
  for(unsigned rotation=0;rotation<2u;rotation++)
  for(unsigned scale=0;scale<4u;scale++){
    /* Boundary refusal selects the common Classic owner; existing Studio
     * preferred recipes retain their separately qualified no-op policies. */
    if(scale==3u && (generation || target_kind || gui)) continue;
    unsigned extent=scale==0u?400u:4u;
    size_t source_bytes=(size_t)extent*extent*4u;
    GmlRender *r=calloc(1,sizeof *r);GmlWin content={0};
    FakeDevice fake={.on_demand=1};GmlRenderDeviceServices device=service(&fake);
    uint32_t screen[W*H]={0},shadow[W*H];GmlRenderJournalMetrics metrics={0};
    CHECK(r && !gml_render_init(r,NULL));
    content.classic_version=generations[generation]>100?generations[generation]:0;
    content.bytecode=content.classic_version?0:generations[generation];
    r->win=&content;r->classic=content.classic_version!=0;
    free(r->atlas[0].px);r->atlas[0].px=malloc(source_bytes);
    r->spr=calloc(1,sizeof *r->spr);r->tpag=calloc(1,sizeof *r->tpag);
    CHECK(r->atlas[0].px && r->spr && r->tpag);
    memset(r->atlas[0].px,255,source_bytes);r->atlas[0].w=r->atlas[0].h=(int)extent;
    r->n_spr=r->n_tpag=1;
    r->spr[0]=(GmlSprite){.w=(int)extent,.h=(int)extent,
      .originx=(int)extent/2,.originy=(int)extent/2,.n_frames=1};
    r->spr[0].frame=calloc(1,sizeof *r->spr[0].frame);CHECK(r->spr[0].frame);
    r->tpag[0]=(GmlTpag){.sw=(int)extent,.sh=(int)extent,.tw=(int)extent,
      .th=(int)extent,.bw=(int)extent,.bh=(int)extent};
    if(runtime){
      uint8_t *pixels=malloc(source_bytes);CHECK(pixels);memset(pixels,255,source_bytes);
      CHECK(gml_sprite_replace_from_rgba(r,0,pixels,(int)extent,(int)extent,(int)extent/2,(int)extent/2));
    }
    device.observe_on_demand=fake_observe_on_demand;gml_render_bind_device(r,&device);
    if(target_kind==1u || target_kind==2u){
      CHECK(gml_render_application_surface_ensure_owned(r,W,H));
      CHECK(gml_render_begin_resident_owned_application(r,0,0,0));
    }else{
      gml_render_application_surface_bind(r,screen,W,H,1);
      CHECK(gml_render_begin_resident_base(r,screen,W,H,0,0));
    }
    if(target_kind==2u) CHECK(gml_surface_set_target(r,0));
    if(target_kind==3u){int id=gml_surface_create(r,W,H);CHECK(id>0 && gml_surface_set_target(r,id));}
    if(gui){gml_render_gui_begin(r,W,H);gml_render_gui_set_size(r,W/2,H/2);}
    gml_render_set_pending_fill(r,0x40273859u);gml_render_flush_pending_fill(r);
    CHECK(gml_render_scene_submit_pending(r));
    memcpy(shadow,r->fb,sizeof shadow);int submitted=fake.submits;
    r->color_write_mask=5u;
    double map=gml_render_gui_transform_active(r)?2.0:1.0;
    double x=8.5/map,y=6.5/map,xs=-scales[scale]/map,ys=scales[scale]/map;
    if(scale==3u){
      CHECK(!gml_render_resident_masked_sprite(r,0,0,x,y,xs,ys,rotation?73.0:0.0,0x70B0D0u,1.0));
    }else for(unsigned draw=0;draw<8u;draw++)
      gml_draw_sprite_ext(r,0,0,x,y,xs,ys,rotation?73.0:0.0,0x70B0D0u,1.0);
    CHECK(r->resident_application_pass && gml_render_scene_submit_pending(r) &&
      !fake.readbacks && !fake.rgb_queries && !memcmp(shadow,r->fb,sizeof shadow));
    if(scale==3u) CHECK(fake.submits==submitted);
    else visible[runtime]+=(unsigned)(fake.submits>submitted);
    GmlRenderResource *resource=gml_render_scene_target_resource_ensure(r);CHECK(resource);
    gml_render_resource_journal_metrics(resource,&metrics);
    CHECK(metrics.authority==GML_RENDER_RESOURCE_GPU_RESIDENT && !metrics.cpu_replays && !metrics.journal_bytes);
    gml_render_abandon_device(r);gml_render_free(r);free(r);
  }
  CHECK(visible[0] && visible[1]);return 1;
}

/* Admission/authority only; the real-context large-unit matrix owns pixels. */
static int resident_large_unit_sprite_scope(void){
  enum { W=16,H=12,A=416 };
  for(unsigned generation=0;generation<2u;generation++) for(unsigned variant=0;variant<9u;variant++){
    GmlRender *r=calloc(1,sizeof *r);
    GmlWin content={.bytecode=generation?17:13};
    FakeDevice fake={.on_demand=variant!=4u};
    GmlRenderDeviceServices device=service(&fake);
    GmlRenderJournalMetrics metrics={0};uint32_t screen[W*H]={0},shadow[W*H];
    CHECK(r && !gml_render_init(r,NULL));r->win=&content;
    free(r->atlas[0].px);r->atlas[0].px=malloc(A*A*4u);
    r->spr=calloc(1,sizeof *r->spr);r->tpag=calloc(1,sizeof *r->tpag);
    CHECK(r->atlas[0].px && r->spr && r->tpag);
    memset(r->atlas[0].px,255,A*A*4u);r->atlas[0].w=r->atlas[0].h=A;
    r->n_spr=r->n_tpag=1;
    r->spr[0]=(GmlSprite){.w=400,.h=400,.originx=200,.originy=200,.n_frames=1};
    r->spr[0].frame=calloc(1,sizeof *r->spr[0].frame);CHECK(r->spr[0].frame);
    r->tpag[0]=(GmlTpag){.sx=4,.sy=4,.sw=400,.sh=400,.tw=400,.th=400,.bw=400,.bh=400};
    device.observe_on_demand=fake_observe_on_demand;gml_render_bind_device(r,&device);
    CHECK(gml_render_begin_resident_base(r,screen,W,H,0,0));
    CHECK(!gml_render_target_preserves_alpha(r) && r->fb!=r->app_surface);
    gml_render_set_pending_fill(r,0x40273859u);gml_render_flush_pending_fill(r);
    CHECK(gml_render_scene_submit_pending(r));
    memcpy(shadow,r->fb,sizeof shadow);int submitted=fake.submits;
    GmlRenderJournalMetrics before={0};
    gml_render_resource_journal_metrics(r->base_scene_resource,&before);
    r->color_write_mask=7u;
    double x=variant==7u?8.0:variant==2u?202.25:202.0;
    double y=variant==7u?6.0:202.0;
    double xs=variant==1u?-1.0:variant==3u?2.0:1.0,ys=variant==1u?-1.0:1.0;
    if(variant==5u) r->active_shader=0;
    if(variant==6u) r->blend_equation=2;
    /* The paired nearest-region controls additionally qualify bounded
     * visible rotation; the old unit-only scope left it unqualified. */
    int accepted=variant<2u || variant==7u || variant==8u;
    int actual=gml_render_resident_masked_sprite(r,0,0,x,y,xs,ys,variant==7u?45:0,
                                                0x70B0D0u,variant==8u?0.00001:1.0);
    if(actual!=accepted) fprintf(stderr,"large unit admission: bytecode=%d variant=%u actual=%d expected=%d\n",
                                 content.bytecode,variant,actual,accepted);
    CHECK(actual==accepted);
    CHECK(r->resident_application_pass && gml_render_scene_submit_pending(r) &&
          !fake.readbacks && !fake.rgb_queries && !memcmp(shadow,r->fb,sizeof shadow));
    CHECK(fake.submits==submitted+(variant<2u || variant==7u?1:0));
    if(variant<2u){
      if(fake.last_quad.x[0]!=(variant?1.0:2.0) || fake.last_quad.y[0]!=(variant?1.0:2.0))
        fprintf(stderr,"large unit metadata: bytecode=%d variant=%u quad=%.17g,%.17g..%.17g,%.17g filter=%u quantisation=%u\n",
          content.bytecode,variant,fake.last_quad.x[0],fake.last_quad.y[0],fake.last_quad.x[2],fake.last_quad.y[2],
          fake.last_filter,fake.last_quantisation);
      CHECK(fake.last_quad.x[0]==(variant?1.0:2.0) && fake.last_quad.y[0]==(variant?1.0:2.0));
    }
    gml_render_resource_journal_metrics(r->base_scene_resource,&metrics);
    if(variant<2u || variant==7u) CHECK(metrics.authority==GML_RENDER_RESOURCE_GPU_RESIDENT &&
                        !metrics.cpu_replays && !metrics.journal_bytes);
    else CHECK(metrics.authority==before.authority &&
               metrics.logical_version==before.logical_version &&
               metrics.cpu_version==before.cpu_version && metrics.gpu_version==before.gpu_version &&
               metrics.cpu_replays==before.cpu_replays && metrics.journal_bytes==before.journal_bytes);
    gml_render_abandon_device(r);gml_render_free(r);free(r);
  }
  return 1;
}

static int resident_large_screen_axis_row_metadata(void){
  for(unsigned variant=0;variant<11u;variant++){
    GmlRender *r=calloc(1,sizeof *r);
    GmlWin content={.bytecode=variant==4u?16:17};
    GmlSprite sprite={.w=316,.h=104,.originx=158,.originy=51};
    GmlTpag page={.sx=3,.sy=2,.sw=316,.sh=104,.tw=316,.th=104,.bw=316,.bh=104};
    GmlRenderRotatedCoverageRegion rows[720];
    uint32_t count=0;
    CHECK(r && gml_render_init(r,NULL)==0 &&
          gml_render_application_surface_ensure_owned(r,1280,720));
    r->win=&content;
    free(r->atlas[0].px);
    r->atlas[0].px=malloc(336u*128u*4u);
    CHECK(r->atlas[0].px);
    memset(r->atlas[0].px,255,336u*128u*4u);
    r->atlas[0].w=336;r->atlas[0].h=128;
    CHECK(gml_render_begin_owned_application(r,0,0,0));
    r->resident_application_pass=variant!=1u;
    if(variant==2u) r->target_id=1;
    if(variant==5u) r->interp=1;
    if(variant==7u) r->gui_pass_active=1;
    if(variant==8u) r->target_sp=1;
    double xs=variant==6u?-3.004694835680751:3.004694835680751;
    double rotation=variant==3u?0.5:0.0;
    int accepted;
    if(variant==9u)
      accepted=gml_render_rotated_nearest_regions(r,&sprite,&page,640,330,xs,3,rotation,
                                                  rows,720,&count);
    else if(variant==10u)
      accepted=gml_render_rotated_nearest_texel_regions(r,&sprite,&page,640,330,xs,3,rotation,
                                                       0xFFFFFFu,rows,720,&count);
    else
      accepted=gml_render_rotated_nearest_row_regions(r,&sprite,&page,640,330,xs,3,rotation,
                                                      rows,720,&count);
    CHECK(accepted==(variant==0u));
    if(variant==0u){
      CHECK(count>300u && count<=720u);
      for(uint32_t row=0;row<count;row++){
        CHECK(rows[row].region.height==1u && rows[row].region.width<=1280u &&
              rows[row].region.x>=0 && rows[row].region.y>=0 && rows[row].region.y<720);
        if(row) CHECK(rows[row].region.y==rows[row-1u].region.y+1);
      }
    } else CHECK(!count);
    r->resident_application_pass=0;r->target_id=-1;r->target_sp=0;
    gml_render_free(r);free(r);
  }
  return 1;
}

/* Prepared large-axis ties must not turn one coherent span into a CPU suffix.
 * The real-device comparator owns pixels; this fixture owns authority/order. */
static int resident_large_screen_coherent_phase(void){
  enum { W=1280,H=720,AW=1312,AH=736 };
  const double phases[]={-.50025,-.49975,-.5,-.75,-.50025};
  for(unsigned variant=0;variant<5u;variant++){
    GmlRender *r=calloc(1,sizeof *r);GmlWin content={.bytecode=17};
    FakeDevice fake={.on_demand=1};GmlRenderDeviceServices device=service(&fake);
    device.observe_on_demand=fake_observe_on_demand;
    size_t bytes=(size_t)W*H*sizeof(uint32_t);
    uint32_t *storage=calloc(2u,bytes),*screen=storage,*shadow=storage?storage+(size_t)W*H:NULL;
    CHECK(r && storage && !gml_render_init(r,NULL));r->win=&content;
    free(r->atlas[0].px);r->atlas[0].px=malloc((size_t)AW*AH*4u);
    r->spr=calloc(1,sizeof *r->spr);r->tpag=calloc(1,sizeof *r->tpag);
    CHECK(r->atlas[0].px && r->spr && r->tpag);
    memset(r->atlas[0].px,255,(size_t)AW*AH*4u);r->atlas[0].w=AW;r->atlas[0].h=AH;
    r->n_spr=r->n_tpag=1;
    r->spr[0]=(GmlSprite){.w=W,.h=H,.n_frames=1};
    r->spr[0].frame=calloc(1,sizeof *r->spr[0].frame);CHECK(r->spr[0].frame);
    r->tpag[0]=(GmlTpag){.sx=2,.sy=2,.sw=W,.sh=H,.tw=W,.th=H,.bw=W,.bh=H};
    gml_render_bind_device(r,&device);
    CHECK(gml_render_begin_resident_base(r,screen,W,H,0,0));
    gml_render_world_set_logical_extent(r,W,H);
    gml_render_set_pending_fill(r,0x40273859u);gml_render_flush_pending_fill(r);
    CHECK(gml_render_scene_submit_pending(r));
    memcpy(shadow,r->fb,bytes);int submitted=fake.submits;
    GmlRenderJournalMetrics before={0},after={0};
    gml_render_resource_journal_metrics(r->base_scene_resource,&before);
    for(unsigned draw=0;draw<8u;draw++)
      gml_draw_sprite_ext(r,0,0,-.75,phases[variant],1,1,0,0xFFFFFFu,variant==4u?0.0:.4);
    CHECK(r->resident_application_pass && gml_render_scene_submit_pending(r) &&
      fake.submits==submitted+(variant!=4u) && !fake.readbacks && !fake.rgb_queries &&
      !memcmp(shadow,r->fb,bytes));
    gml_render_resource_journal_metrics(r->base_scene_resource,&after);
    CHECK(after.authority==GML_RENDER_RESOURCE_GPU_RESIDENT && !after.cpu_replays &&
      !after.journal_bytes && after.logical_version==before.logical_version+(variant==4u?0u:8u));
    gml_render_abandon_device(r);gml_render_free(r);free(r);free(storage);
  }
  return 1;
}

static int resident_gui_replacement_sprite(void){
  for(unsigned variant=0;variant<14u;variant++){
    GmlRender *render=calloc(1,sizeof *render);
    GmlWin content={.bytecode=17};
    FakeDevice fake={.on_demand=1};
    GmlRenderDeviceServices device=service(&fake);
    GmlRenderJournalMetrics metrics={0};
    uint32_t screen[64]={0};
    const uint8_t rgba[16]={208,32,16,0,16,208,32,1,
                            32,16,208,128,192,176,160,255};
    CHECK(render && gml_render_init(render,NULL)==0);
    render->win=&content;
    free(render->atlas[0].px);
    memset(&render->atlas[0],0,sizeof render->atlas[0]);
    render->atlas[0].px=malloc(sizeof rgba);
    render->tpag=calloc(1,sizeof *render->tpag);
    render->spr=calloc(1,sizeof *render->spr);
    CHECK(render->atlas[0].px && render->tpag && render->spr);
    memcpy(render->atlas[0].px,rgba,sizeof rgba);
    render->atlas[0].w=render->atlas[0].h=2;
    render->n_tpag=render->n_spr=1;
    render->tpag[0]=(GmlTpag){.sw=2,.sh=2,.tw=2,.th=2,.bw=2,.bh=2};
    render->spr[0].w=render->spr[0].h=2;
    render->spr[0].n_frames=1;
    render->spr[0].frame=calloc(1,sizeof(int));
    CHECK(render->spr[0].frame);
    device.observe_on_demand=fake_observe_on_demand;
    gml_render_bind_device(render,&device);
    CHECK(gml_render_begin_resident_base(render,screen,8,8,0,0));
    gml_render_gui_begin(render,8,8);
    render->interp=0;
    render->blendmode=6;
    double x=1.0,xs=1.0,alpha=1.0;
    uint32_t colour=0xFFFFFFu;
    if(variant==1u) x=-1.0; /* Unit crop clipped at the SCREEN edge. */
    if(variant==2u) render->interp=1;
    if(variant==3u) x+=0.25;
    if(variant==4u) xs=2.0;
    if(variant==5u) xs=-1.0;
    if(variant==6u) colour=0x80C0E0u;
    if(variant==7u) alpha=0.5;
    if(variant==8u) render->color_write_mask=7u;
    if(variant==9u) fake.on_demand=0;
    if(variant==10u) render->gui_scale_x=2.0;
    if(variant==11u) render->blend_equation=2;
    if(variant==12u){
      render->tpag[0].bw=render->tpag[0].bh=4;
      render->tpag[0].tx=render->tpag[0].ty=1;
      render->spr[0].w=render->spr[0].h=4;
    }
    gml_render_set_pending_fill(render,0x40123456u);
    if(variant==13u)
      gml_draw_sprite_part_ext(render,0,0,0,0,2,2,x,1.0,xs,1.0,colour,alpha);
    else
      gml_draw_sprite_ext(render,0,0,x,1.0,xs,1.0,0.0,colour,alpha);
    int admitted=variant!=2u && variant!=9u && variant!=11u && variant!=13u;
    CHECK(render->resident_application_pass==admitted);
    if(admitted){
      CHECK(gml_render_scene_submit_pending(render) && !fake.readbacks);
      if(variant<2u) CHECK(fake.last_filter==GML_RENDER_FILTER_NEAREST_PHASED_COPY);
      else CHECK(fake.last_filter==GML_RENDER_FILTER_NEAREST &&
                 fake.last_quantisation==GML_RENDER_QUANTISE_INTEGER_TINT_TRUNC &&
                 fake.last_quad_state.coverage_policy==GML_RENDER_COVERAGE_REPLACE_ZERO_ALPHA);
      CHECK(!fake.last_quad_state.rgb_source_factor &&
            !fake.last_quad_state.alpha_source_factor);
      gml_render_resource_journal_metrics(render->base_scene_resource,&metrics);
      CHECK(metrics.authority==GML_RENDER_RESOURCE_GPU_RESIDENT &&
            !metrics.cpu_replays && !metrics.device_checkpoints && !metrics.journal_bytes);
    }
    gml_render_free(render);free(render);
  }
  return 1;
}

static int resident_integer_world_sprite(void){
  enum { WIDTH=24,HEIGHT=24,POSITIVES=40,VARIANTS=45 };
  const double world[5][2]={{2,2},{4,4},{2,3},{0.5,0.5},{-2,-3}};
  for(unsigned modern=0;modern<2u;modern++)
  for(unsigned variant=0;variant<VARIANTS;variant++){
    GmlRender *render=(GmlRender*)calloc(1,sizeof *render);
    GmlWin content={0};
    GmlRenderApplicationWriteView view={0};
    FakeDevice fake={0};
    GmlRenderDeviceServices device=service(&fake);
    uint32_t candidate[WIDTH*HEIGHT];
    uint8_t rgba[16]={208,32,16,255,16,208,32,255,
                      32,16,208,255,192,176,160,255};
    unsigned mapping=variant<POSITIVES?variant/8u:0u;
    unsigned signs=variant<POSITIVES?(variant/2u)%4u:0u;
    double xs=mapping==3u?4.0:1.0,ys=xs;
    double cam=0.5;
    CHECK(render && gml_render_application_surface_ensure_owned(render,WIDTH,HEIGHT));
    content.bytecode=modern?17:15;
    render->win=&content;
    render->atlas=(GmlAtlas*)calloc(1,sizeof *render->atlas);
    render->tpag=(GmlTpag*)calloc(1,sizeof *render->tpag);
    render->spr=(GmlSprite*)calloc(1,sizeof *render->spr);
    CHECK(render->atlas && render->tpag && render->spr);
    render->n_atlas=render->n_tpag=render->n_spr=1;
    render->atlas[0].px=(uint8_t*)malloc(sizeof rgba);
    render->spr[0].frame=(int*)calloc(1,sizeof(int));
    CHECK(render->atlas[0].px && render->spr[0].frame);
    if(variant%2u) rgba[7]=0;
    memcpy(render->atlas[0].px,rgba,sizeof rgba);
    render->atlas[0].w=render->atlas[0].h=2;
    render->tpag[0].sw=render->tpag[0].sh=2;
    render->tpag[0].tw=render->tpag[0].th=2;
    render->tpag[0].bw=render->tpag[0].bh=4;
    render->tpag[0].tx=render->tpag[0].ty=1;
    render->spr[0].w=render->spr[0].h=4;
    render->spr[0].originx=render->spr[0].originy=2;
    render->spr[0].n_frames=1;
    render->active_shader=-1;
    render->color_write_mask=15u;
    if(signs&1u) xs=-xs;
    if(signs&2u) ys=-ys;
    double projected_xs=xs*world[mapping][0],projected_ys=ys*world[mapping][1];
    double x=(6.0+projected_xs+(projected_xs<0.0?-2.0*projected_xs:0.0)+cam)/world[mapping][0];
    double y=(6.0+projected_ys+(projected_ys<0.0?-2.0*projected_ys:0.0)+cam)/world[mapping][1];
    double second_x=x+1.0/world[mapping][0];
    gml_render_bind_device(render,&device);
    CHECK(gml_render_begin_resident_owned_application(render,cam,cam,0));
    render->world_transform_active=1;
    render->world_scale_x=world[mapping][0];render->world_scale_y=world[mapping][1];
    gml_render_set_pending_fill(render,0x80123456u);
    gml_draw_sprite_ext(render,0,0,x,y,xs,ys,0.0,0xFFFFFFu,1.0);
    CHECK(render->resident_application_pass);
    unsigned exclusion=variant-POSITIVES;
    if(variant==POSITIVES) render->interp=1;
    if(variant==POSITIVES+1u) second_x+=0.125;
    if(variant==POSITIVES+2u) xs=0.75;
    if(variant==POSITIVES+3u) render->color_write_mask=7u;
    if(variant==POSITIVES+4u) second_x=(-1.0+projected_xs+cam)/world[mapping][0];
    gml_draw_sprite_ext(render,0,0,second_x,y,xs,ys,0.0,0xFFFFFFu,1.0);
    CHECK(render->resident_application_pass==(variant<POSITIVES));
    CHECK(gml_render_scene_flush(render) && fake.submits==2 && fake.readbacks==0);
    CHECK(variant>=POSITIVES || fake.last_batch_operations==2u);
    CHECK(gml_render_application_surface_owned_view(render,&view));
    memcpy(candidate,view.pixels,sizeof candidate);
    render->interp=0;render->color_write_mask=15u;
    xs=projected_xs/world[mapping][0];
    CHECK(gml_render_begin_owned_application(render,cam,cam,0));
    render->world_transform_active=1;
    render->world_scale_x=world[mapping][0];render->world_scale_y=world[mapping][1];
    gml_render_set_pending_fill(render,0x80123456u);
    gml_draw_sprite_ext(render,0,0,x,y,xs,ys,0.0,0xFFFFFFu,1.0);
    if(variant>=POSITIVES){
      if(exclusion==0u) render->interp=1;
      if(exclusion==2u) xs=0.75;
      if(exclusion==3u) render->color_write_mask=7u;
    }
    gml_draw_sprite_ext(render,0,0,second_x,y,xs,ys,0.0,0xFFFFFFu,1.0);
    gml_render_flush_pending_fill(render);
    CHECK(!memcmp(candidate,render->app_surface_owned,sizeof candidate));
    gml_render_free(render);
    free(render);
  }
  return 1;
}

static int resident_opaque_sprite_complete_application(void){
  for(unsigned variant=0;variant<7u;variant++){
    GmlRender *render=calloc(1,sizeof *render);
    GmlWin content={.bytecode=15};
    FakeDevice fake={.on_demand=1};
    GmlRenderDeviceServices device=service(&fake);
    GmlRenderJournalMetrics metrics={0};
    uint8_t rgba[16]={208,32,16,255,16,208,32,255,
                      32,16,208,255,192,176,160,255};
    int extent=variant==1u || variant>=3u?4:2;
    double x=variant==2u?2.0:variant>=3u?1.0:0.0;
    double y=variant>=3u?1.0:0.0;
    double xs=variant==1u?2.0:variant==2u?-1.0:1.0;
    double ys=variant==1u?2.0:1.0;
    double alpha=variant==6u?.75:1.0;
    CHECK(render && gml_render_init(render,NULL)==0);
    render->win=&content;
    CHECK(gml_render_application_surface_ensure_owned(render,extent,extent));
    CHECK(render->atlas && render->n_atlas==1);
    free(render->atlas[0].px);
    memset(&render->atlas[0],0,sizeof render->atlas[0]);
    render->tpag=calloc(1,sizeof *render->tpag);
    render->spr=calloc(1,sizeof *render->spr);
    CHECK(render->atlas && render->tpag && render->spr);
    render->n_atlas=render->n_tpag=render->n_spr=1;
    render->atlas[0].px=malloc(sizeof rgba);
    render->spr[0].frame=calloc(1,sizeof(int));
    CHECK(render->atlas[0].px && render->spr[0].frame);
    if(variant==4u) rgba[7]=0;
    if(variant==5u) rgba[7]=128;
    memcpy(render->atlas[0].px,rgba,sizeof rgba);
    render->atlas[0].w=render->atlas[0].h=2;
    render->tpag[0]=(GmlTpag){.sw=2,.sh=2,.tw=2,.th=2,.bw=2,.bh=2};
    render->spr[0].w=render->spr[0].h=2;
    render->spr[0].n_frames=1;
    device.observe_on_demand=fake_observe_on_demand;
    gml_render_bind_device(render,&device);
    CHECK(gml_render_begin_resident_owned_application(render,0,0,0));
    render->interp=0;
    gml_render_set_pending_fill(render,0x40123456u);
    gml_draw_sprite_ext(render,0,0,x,y,xs,ys,0,0xFFFFFFu,alpha);
    CHECK(render->resident_application_pass && fake.readbacks==0);
    CHECK(render->app_surface_opaque==(variant<3u));
    if(variant<3u) CHECK(render->fb_opaque_known && render->fb_all_opaque);
    CHECK(gml_render_scene_submit_pending(render) && fake.readbacks==0);
    CHECK(render->app_surface_opaque==(variant<3u));
    gml_render_resource_journal_metrics(render->app_surface_resource,&metrics);
    CHECK(metrics.gpu_version==metrics.logical_version &&
          metrics.cpu_version<metrics.logical_version && metrics.cpu_replays==0 &&
          metrics.device_checkpoints==0);
    gml_render_free(render);free(render);
  }
  return 1;
}

/* Admission/authority only. The owning real-context control supplies D2 pixels. */
static int resident_sprite_function(int geometry){
  enum { W=24,H=20 };
  const int legacy_generations[]={700,800,15,17},nearest_generations[]={700,530,800,13,15,17};
  const int *generations=geometry?nearest_generations:legacy_generations;
  const double alphas[]={1.0,0.0,0.4},angles[]={0,90,45,-0.5,359.9995};
  const double scales[][2]={{.75,1.5},{1.5,.75},{2,3}};
  for(unsigned draw_state=0;draw_state<4u;draw_state++)
  for(unsigned angle=0;angle<(geometry?5u:1u);angle++)
  for(unsigned scale=0;scale<(geometry?3u:1u);scale++)
  for(unsigned fractional=geometry?1u:0u;fractional<2u;fractional++)
  for(unsigned generation=0;generation<(geometry?6u:4u);generation++)
  for(unsigned interpolation=0;interpolation<(geometry==2 || generations[generation]>100?2u:1u);interpolation++)
  for(unsigned target_kind=0;target_kind<4u;target_kind++)
  for(unsigned gui=0;gui<2u;gui++) for(unsigned signs=0;signs<4u;signs++)
  for(unsigned disabled=0;disabled<2u;disabled++) for(unsigned tint=0;tint<2u;tint++)
  for(unsigned mask=0;mask<(interpolation?4u:1u);mask++)
  for(unsigned a=0;a<3u;a++){
    if(geometry==2 && ((generation!=3u && generation!=4u) ||
                      !interpolation || !angle || angle==4u)) continue;
    GmlRender *render=calloc(1,sizeof *render);GmlWin content={0};
    FakeDevice fake={.on_demand=1,.readback_colour=0x7F25C080u};
    GmlRenderDeviceServices device=service(&fake);GmlRenderJournalMetrics metrics={0};
    uint32_t screen[W*H],shadow[W*H];const uint32_t *pixels=NULL;
    CHECK(render && !gml_render_init(render,NULL));
    content.classic_version=generations[generation]>100?generations[generation]:0;
    content.bytecode=content.classic_version?0:generations[generation];
    render->win=&content;render->classic=content.classic_version!=0;
    uint8_t rgba[16]={208,32,16,255,16,208,32,0,32,16,208,128,192,176,160,64};
    free(render->atlas[0].px);render->atlas[0].px=malloc(sizeof rgba);
    render->atlas[0].w=render->atlas[0].h=2;
    render->tpag=calloc(1,sizeof *render->tpag);render->spr=calloc(1,sizeof *render->spr);
    CHECK(render->atlas[0].px && render->tpag && render->spr);
    memcpy(render->atlas[0].px,rgba,sizeof rgba);render->n_tpag=render->n_spr=1;
    render->tpag[0]=(GmlTpag){.sw=2,.sh=2,.tw=2,.th=2,.bw=4,.bh=4,.tx=1,.ty=1};
    render->spr[0]=(GmlSprite){.w=4,.h=4,.originx=2,.originy=2,.n_frames=1};
    render->spr[0].frame=calloc(1,sizeof(int));CHECK(render->spr[0].frame);
    device.observe_on_demand=fake_observe_on_demand;gml_render_bind_device(render,&device);
    for(unsigned p=0;p<W*H;p++) screen[p]=0xA5ABCDEFu;
    if(target_kind==1u || target_kind==2u){
      CHECK(gml_render_application_surface_ensure_owned(render,W,H));
      CHECK(gml_render_begin_resident_owned_application(render,0,0,0));
    }else{
      gml_render_application_surface_bind(render,screen,W,H,1);
      CHECK(gml_render_begin_resident_base(render,screen,W,H,0,0));
    }
    if(target_kind==2u) CHECK(gml_surface_set_target(render,0));
    if(target_kind==3u){int id=gml_surface_create(render,W,H);CHECK(id>0 && gml_surface_set_target(render,id));}
    if(gui){gml_render_gui_begin(render,W,H);gml_render_gui_set_size(render,W/2,H/2);}
    render->interp=(int)interpolation;
    render->alphablend=!disabled;
    render->blendmode=draw_state>=2u?2:0;
    render->alpha_test_enable=(int)(draw_state&1u);
    render->alpha_test_ref=(uint8_t)(generation*67u+signs*41u+a*17u);
    gml_render_set_pending_fill(render,0x40273859u);
    gml_render_flush_pending_fill(render);CHECK(gml_render_scene_submit_pending(render));
    const uint8_t write_masks[]={15,0,5,10};
    render->color_write_mask=write_masks[mask];
    memcpy(shadow,render->fb,sizeof shadow);int submitted=fake.submits;
    double xs=(fractional?4.0:2.0)*(signs&1u?-1.0:1.0);
    double ys=(fractional?4.0:1.0)*(signs&2u?-1.0:1.0);
    double mapping=gml_render_gui_transform_active(render)?2.0:1.0;
    if(geometry){
      xs=scales[scale][0]*(signs&1u?-1.0:1.0)/mapping;
      ys=scales[scale][1]*(signs&2u?-1.0:1.0)/mapping;
    }
    double dx=xs*mapping,dy=ys*mapping;
    double x=(6.0+(fractional?0.018:0.0)+(dx<0.0?-2.0*dx:0.0)+dx)/mapping;
    double y=(5.0+(fractional?0.07:0.0)+(dy<0.0?-2.0*dy:0.0)+dy)/mapping;
    for(unsigned draw=0;draw<8u;draw++)
      gml_draw_sprite_ext(render,0,0,x,y,xs,ys,geometry?angles[angle]:0,tint?0x70B0D0u:0xFFFFFFu,alphas[a]);
    CHECK(render->resident_application_pass && gml_render_scene_submit_pending(render));
    int retained_rotation=geometry && fake.last_quantisation==GML_RENDER_QUANTISE_SPRITE_MODERN_ROTATED_NEAREST;
    int expected_submits=a==1u || !write_masks[mask]?0:retained_rotation?8:1;
    if(geometry && fake.submits!=submitted+expected_submits)
      fprintf(stderr,"nearest sprite metadata: generation=%d interp=%u target=%u gui=%u state=%u angle=%g scale=%u signs=%u disabled=%u tint=%u alpha=%g submits=%d/%d packets=%u\n",
        generations[generation],interpolation,target_kind,gui,draw_state,angles[angle],scale,signs,disabled,tint,
        alphas[a],fake.submits,submitted,fake.last_batch_operations);
    CHECK(fake.submits==submitted+expected_submits && !fake.readbacks && !fake.rgb_queries &&
          !memcmp(shadow,render->fb,sizeof shadow));
    GmlRenderResource *resource=gml_render_scene_target_resource_ensure(render);CHECK(resource);
    gml_render_resource_journal_metrics(resource,&metrics);
    CHECK(metrics.authority==GML_RENDER_RESOURCE_GPU_RESIDENT && !metrics.cpu_replays && !metrics.journal_bytes);
    if(a!=1u && write_masks[mask]){
      CHECK((retained_rotation?fake.last_batch_operations>=1u:fake.last_batch_operations>=8u) &&
        fake.last_filter==(retained_rotation?GML_RENDER_FILTER_NEAREST_PHASED:GML_RENDER_FILTER_NEAREST));
      if(fake.last_quantisation==GML_RENDER_QUANTISE_SPRITE_MODERN_NEAREST ||
         (geometry && fake.last_quantisation==GML_RENDER_QUANTISE_SPRITE_MODERN_ROTATED_NEAREST)){
        /* The inherited predecessor path is not replaced by this extension. */
        CHECK(!draw_state && generations[generation]==17 && target_kind==3u && !gui && !disabled &&
              fake.last_filtered_draw_alpha==alphas[a]);
      }else if(fake.last_quantisation) CHECK(
        fake.last_quantisation==((generations[generation]==17 && (!geometry || angle==0u || angle==4u))?
          GML_RENDER_QUANTISE_INTEGER_TINT_ROUND:GML_RENDER_QUANTISE_INTEGER_TINT_TRUNC) &&
        fake.last_filtered_draw_alpha==alphas[a]);
    }
    if(!geometry && draw_state==2u && !fractional && generation==2u && target_kind==3u &&
       !gui && !signs && disabled && tint && !a){
      int calls=fake.submits;
      for(unsigned reference=0;reference<256u;reference++){
        render->alpha_test_ref=(uint8_t)reference;
        gml_draw_sprite_ext(render,0,0,x,y,xs,ys,0,0x70B0D0u,1.0);
      }
      CHECK(fake.submits==calls && render->alpha_test_ref==255u &&
            gml_render_scene_submit_pending(render) && fake.submits==calls+1 &&
            fake.last_batch_operations==256u && !fake.last_quad_state.alpha_test_enabled &&
            !fake.last_quad_state.alpha_test_reference && !fake.readbacks && !fake.rgb_queries);
    }
    CHECK(gml_render_resource_acquire_cpu_read(resource,&pixels) && fake.readbacks==1 &&
      pixels[0]==fake.readback_colour && pixels[W*H-1u]==fake.readback_colour);
    fake.forbid_calls=1;
    CHECK(gml_render_resource_acquire_cpu_read(resource,&pixels) && !fake.forbidden_calls &&
      pixels[0]==fake.readback_colour && pixels[W*H-1u]==fake.readback_colour);
    fake.forbid_calls=0;gml_render_free(render);free(render);
  }
  return 1;
}

/* Authority only; the real-context function matrix owns all RGBA comparisons. */
static int resident_flat_fog_sprite_function(void){
  enum { W=24,H=20 };
  const int generations[]={530,700,800,13,15,17};
  for(unsigned generation=0;generation<6u;generation++)
  for(unsigned kind=0;kind<4u;kind++) for(unsigned gui=0;gui<2u;gui++)
  for(unsigned interpolation=0;interpolation<2u;interpolation++){
    GmlRender *r=calloc(1,sizeof *r);GmlWin content={0};
    FakeDevice fake={.on_demand=1,.readback_colour=0x7F25C080u};
    GmlRenderDeviceServices device=service(&fake);uint32_t screen[W*H]={0},shadow[W*H];
    CHECK(r && !gml_render_init(r,NULL));
    content.classic_version=generations[generation]>100?generations[generation]:0;
    content.bytecode=content.classic_version?0:generations[generation];
    r->win=&content;r->classic=content.classic_version!=0;
    const uint8_t rgba[]={208,32,16,255,16,208,32,0,32,16,208,128,192,176,160,64};
    free(r->atlas[0].px);r->atlas[0].px=malloc(sizeof rgba);
    r->tpag=calloc(1,sizeof *r->tpag);r->spr=calloc(1,sizeof *r->spr);
    CHECK(r->atlas[0].px && r->tpag && r->spr);
    memcpy(r->atlas[0].px,rgba,sizeof rgba);r->atlas[0].w=r->atlas[0].h=2;
    r->n_tpag=r->n_spr=1;
    r->tpag[0]=(GmlTpag){.sw=2,.sh=2,.tw=2,.th=2,.bw=4,.bh=4,.tx=1,.ty=1};
    r->spr[0]=(GmlSprite){.w=4,.h=4,.originx=2,.originy=2,.n_frames=1};
    r->spr[0].frame=calloc(1,sizeof(int));CHECK(r->spr[0].frame);
    device.observe_on_demand=fake_observe_on_demand;gml_render_bind_device(r,&device);
    if(kind==1u || kind==2u){
      CHECK(gml_render_application_surface_ensure_owned(r,W,H));
      CHECK(gml_render_begin_resident_owned_application(r,0,0,0));
    }else{
      gml_render_application_surface_bind(r,screen,W,H,1);
      CHECK(gml_render_begin_resident_base(r,screen,W,H,0,0));
    }
    if(kind==2u) CHECK(gml_surface_set_target(r,0));
    if(kind==3u){int id=gml_surface_create(r,W,H);CHECK(id>0 && gml_surface_set_target(r,id));}
    if(gui){gml_render_gui_begin(r,W,H);gml_render_gui_set_size(r,W/2,H/2);}
    gml_render_set_pending_fill(r,0x40273859u);gml_render_flush_pending_fill(r);
    CHECK(gml_render_scene_submit_pending(r));memcpy(shadow,r->fb,sizeof shadow);
    int submitted=fake.submits;
    r->interp=(int)interpolation;r->fog_flat=1;r->fog_flat_rgb=0x315A9Cu;
    double map_x=1,map_y=1;gml_render_draw_map_scale(r,&map_x,&map_y);
    for(unsigned draw=0;draw<8u;draw++)
      gml_draw_sprite_ext(r,0,0,12/map_x,12/map_y,-1/map_x,1/map_y,0,0x103070u,1);
    CHECK(r->resident_application_pass && !fake.readbacks && !fake.rgb_queries);
    CHECK(gml_render_scene_submit_pending(r) && fake.submits==submitted+1);
    CHECK(fake.last_batch_operations>=8u && fake.last_quad.corner_colour[0]==0xFF315A9Cu);
    CHECK(!memcmp(shadow,r->fb,sizeof shadow) && r->fog_flat && r->interp==(int)interpolation);
    gml_render_free(r);free(r);
  }
  return 1;
}

static int resident_integer_sprite_function(void){return resident_sprite_function(0);}
static int resident_nearest_sprite_function(void){return resident_sprite_function(1);}
static int resident_first_generation_rotated_point_sprite_function(void){return resident_sprite_function(2);}

/* Authority and ownership only; the paired device matrix owns tiled pixels. */
static int resident_tiled_sprite_function_impl(int underlay){
  enum { W=24,H=20 };
  const int generations[]={530,700,800,13,15,17};
  for(unsigned generation=0;generation<6u;generation++)
  for(unsigned kind=0;kind<4u;kind++) for(unsigned gui=0;gui<2u;gui++)
  for(unsigned interpolation=0;interpolation<2u;interpolation++)
  for(unsigned state=0;state<4u;state++){
    if(underlay && (kind || !gui || state!=3u)) continue;
    GmlRender *r=calloc(1,sizeof *r);GmlWin content={0};
    FakeDevice fake={.on_demand=1,.readback_colour=0x7F25C080u};
    GmlRenderDeviceServices device=service(&fake);GmlRenderJournalMetrics metrics={0};
    uint32_t screen[W*H],shadow[W*H];const uint32_t *pixels=NULL;
    CHECK(r && !gml_render_init(r,NULL));
    content.classic_version=generations[generation]>100?generations[generation]:0;
    content.bytecode=content.classic_version?0:generations[generation];
    r->win=&content;r->classic=content.classic_version!=0;
    const uint8_t rgba[]={208,32,16,255,16,208,32,255,32,16,208,255,192,176,160,255};
    free(r->atlas[0].px);r->atlas[0].px=malloc(sizeof rgba);
    r->tpag=calloc(1,sizeof *r->tpag);r->spr=calloc(1,sizeof *r->spr);
    CHECK(r->atlas[0].px && r->tpag && r->spr);
    memcpy(r->atlas[0].px,rgba,sizeof rgba);r->atlas[0].w=r->atlas[0].h=2;
    r->n_tpag=r->n_spr=1;
    r->tpag[0]=(GmlTpag){.sw=2,.sh=2,.tw=2,.th=2,.bw=4,.bh=4,.tx=1,.ty=1};
    r->spr[0]=(GmlSprite){.w=4,.h=4,.originx=7,.originy=-3,.n_frames=1};
    r->spr[0].frame=calloc(1,sizeof(int));CHECK(r->spr[0].frame);
    device.observe_on_demand=fake_observe_on_demand;gml_render_bind_device(r,&device);
    for(unsigned p=0;p<W*H;p++) screen[p]=0xA5ABCDEFu;
    if(kind==1u || kind==2u){
      CHECK(gml_render_application_surface_ensure_owned(r,W,H));
      CHECK(gml_render_begin_resident_owned_application(r,-3.5,2.25,0));
    }else{
      gml_render_application_surface_bind(r,screen,W,H,1);
      CHECK(gml_render_begin_resident_base(r,screen,W,H,-3.5,2.25));
    }
    if(kind==2u) CHECK(gml_surface_set_target(r,0));
    if(kind==3u){int id=gml_surface_create(r,W,H);CHECK(id>0 && gml_surface_set_target(r,id));}
    if(gui){gml_render_gui_begin(r,W,H);gml_render_gui_set_size(r,W/2,H/2);}
    r->interp=(int)interpolation;r->alphablend=state!=2u;
    r->blendmode=state<3u?(int)state:0;r->color_write_mask=state==1u?5u:15u;
    gml_render_set_pending_fill(r,0x40273859u);gml_render_flush_pending_fill(r);
    CHECK(gml_render_scene_submit_pending(r));
    GmlRenderResource *target=gml_render_scene_target_resource_ensure(r);CHECK(target);
    uint64_t version=gml_render_resource_image_ref(target).content_version;
    memcpy(shadow,r->fb,sizeof shadow);int submitted=fake.submits;
    double mapping=gml_render_gui_transform_active(r)?2.0:1.0;
    double opacity=state==3u?0.0:state?0.4:1.0;
    if(underlay){
      gml_render_set_pending_fill(r,0x7F315779u);
      gml_render_set_pending_underlay(r,0,0,W,H);
      if(underlay>=2) opacity=0.4;
      if(underlay==2) r->color_write_mask=0u;
    }
    for(unsigned draw=0;draw<8u;draw++)
      gml_draw_sprite_tiled_ext(r,0,0,-3.25,.5,underlay==3?0.0:-2.0/mapping,3.0/mapping,
        state?0x70B0D0u:0xFFFFFFu,opacity);
    CHECK(r->resident_application_pass && fake.submits==submitted &&
      gml_render_resource_image_ref(target).content_version==version);
    if(underlay){
      CHECK(r->pending_fill && r->pending_fill_color==0x7F315779u &&
        r->pending_underlay && r->underlay_w==W && r->underlay_h==H &&
        !fake.readbacks && !fake.rgb_queries && !memcmp(shadow,r->fb,sizeof shadow));
      gml_render_abandon_device(r);gml_render_free(r);free(r);continue;
    }
    CHECK(gml_render_scene_submit_pending(r) && fake.submits==submitted+(state!=3u) &&
      !fake.readbacks && !fake.rgb_queries && !memcmp(shadow,r->fb,sizeof shadow));
    gml_render_resource_journal_metrics(target,&metrics);
    CHECK(metrics.authority==GML_RENDER_RESOURCE_GPU_RESIDENT && !metrics.cpu_replays &&
      !metrics.journal_bytes && (state==3u || fake.last_batch_operations>=8u));
    CHECK(gml_render_resource_acquire_cpu_read(target,&pixels) && fake.readbacks==1 &&
      pixels[0]==fake.readback_colour && pixels[W*H-1u]==fake.readback_colour);
    fake.forbid_calls=1;
    CHECK(gml_render_resource_acquire_cpu_read(target,&pixels) && !fake.forbidden_calls);
    fake.forbid_calls=0;gml_render_free(r);free(r);
  }
  return 1;
}

static int resident_tiled_sprite_function(void){return resident_tiled_sprite_function_impl(0);}
static int resident_tiled_sprite_underlay_noops(void){
  for(int variant=1;variant<=3;variant++) if(!resident_tiled_sprite_function_impl(variant)) return 0;
  return 1;
}

static int passthrough_scope_executor(void *context,const GmlRenderShaderRequest *request){
  (void)context;(void)request;return 0;
}

/* Admission/authority only; real paired controls own pixels and shader policy. */
static int resident_cpu_passthrough_sprite_scope(void){
  enum { W=24,H=20 };
  const int generations[]={530,700,800,13,15,17};
  for(unsigned generation=0;generation<6u;generation++)
  for(unsigned kind=0;kind<4u;kind++) for(unsigned interpolation=0;interpolation<2u;interpolation++)
  for(unsigned mask=0;mask<2u;mask++) for(unsigned program=0;program<21u;program++){
    if(program && !mask) continue;
    GmlRender *r=calloc(1,sizeof *r);GmlWin content={0};
    FakeDevice fake={.on_demand=1};GmlRenderDeviceServices device=service(&fake);
    uint32_t screen[W*H]={0},shadow[W*H];
    CHECK(r && !gml_render_init(r,NULL));
    content.classic_version=generations[generation]>100?generations[generation]:0;
    content.bytecode=content.classic_version?0:generations[generation];
    r->win=&content;r->classic=content.classic_version!=0;
    const uint8_t rgba[]={208,32,16,255,16,208,32,0,32,16,208,128,192,176,160,64};
    free(r->atlas[0].px);r->atlas[0].px=malloc(sizeof rgba);
    r->tpag=calloc(1,sizeof *r->tpag);r->spr=calloc(1,sizeof *r->spr);
    r->shader_pal=calloc(1,sizeof *r->shader_pal);
    CHECK(r->atlas[0].px && r->tpag && r->spr && r->shader_pal);
    memcpy(r->atlas[0].px,rgba,sizeof rgba);r->atlas[0].w=r->atlas[0].h=2;
    r->n_spr=r->n_tpag=r->n_shader_pal=1;
    r->tpag[0]=(GmlTpag){.sw=2,.sh=2,.tw=2,.th=2,.bw=4,.bh=4,.tx=1,.ty=1};
    r->spr[0]=(GmlSprite){.w=4,.h=4,.originx=2,.originy=2,.n_frames=1};
    r->spr[0].frame=calloc(1,sizeof(int));CHECK(r->spr[0].frame);
    device.observe_on_demand=fake_observe_on_demand;gml_render_bind_device(r,&device);
    if(kind==1u || kind==2u){
      CHECK(gml_render_application_surface_ensure_owned(r,W,H));
      CHECK(gml_render_begin_resident_owned_application(r,0,0,0));
    }else{
      gml_render_application_surface_bind(r,screen,W,H,1);
      CHECK(gml_render_begin_resident_base(r,screen,W,H,0,0));
    }
    if(kind==2u) CHECK(gml_surface_set_target(r,0));
    if(kind==3u){int id=gml_surface_create(r,W,H);CHECK(id>0 && gml_surface_set_target(r,id));}
    gml_render_set_pending_fill(r,0x40273859u);gml_render_flush_pending_fill(r);
    CHECK(gml_render_scene_submit_pending(r));
    r->active_shader=program==20u?1:0;r->interp=(int)interpolation;r->color_write_mask=mask?5u:15u;
    struct GmlShaderPal *p=&r->shader_pal[0];
    int *effects[]={&p->threshold_palette,&p->indexed_brightness,&p->channel_mask,&p->channel_alpha_key,
      &p->alpha_discard,&p->ordered_dither,&p->quantise4,&p->solid_alpha_mask,&p->solid_blur_alpha,
      &p->lut,&p->lut_indexed,&p->grid,&p->dual_sample,&p->radial_wave,&p->uv_wave_mode,
      &p->grayscale,&p->vertex_colour_blend,&p->vertex_colour_blend_alpha_only};
    if(program>0u && program<=18u) *effects[program-1u]=1;
    if(program==19u){
      p->source_vertex_es="attribute vec3 in_Position;void main(){gl_Position=vec4(in_Position,1.0);}";
      p->source_fragment_es="precision mediump float;void main(){gl_FragColor=vec4(1.0);}";
      gml_render_set_shader_executor(r,passthrough_scope_executor,NULL);
    }
    memcpy(shadow,r->fb,sizeof shadow);int submitted=fake.submits;
    int refused=program;
    if(refused){
      /* Test the actual masked entry,not its full-mask early refusal. */
      r->color_write_mask=5u;
      CHECK(!gml_render_resident_masked_sprite(r,0,0,12,10,1,1,0,0xFFFFFFu,0.4));
      CHECK(r->resident_application_pass && fake.submits==submitted);
    }else{
      for(unsigned draw=0;draw<8u;draw++) gml_draw_sprite_ext(r,0,0,12,10,1,1,0,0xFFFFFFu,0.4);
      CHECK(r->resident_application_pass && gml_render_scene_submit_pending(r));
      CHECK(fake.submits==submitted+1 && fake.last_batch_operations>=8u);
    }
    CHECK(r->active_shader==(program==20u?1:0) && r->interp==(int)interpolation &&
      !fake.readbacks && !fake.rgb_queries && !memcmp(shadow,r->fb,sizeof shadow));
    gml_render_abandon_device(r);gml_render_free(r);free(r);
  }
  return 1;
}

/* Authority and transport only; the paired device generator owns D2 pixels. */
static int resident_physical_filtered_sprite_function_impl(int matched,int modern){
  enum { W=24,H=20,A=6 };
  const int generations[]={13,15,17};
  for(unsigned generation=modern?2u:0u;generation<(modern?3u:2u);generation++)
  for(unsigned state=0;state<6u;state++)
  for(unsigned kind=0;kind<4u;kind++) for(unsigned gui=0;gui<2u;gui++)
  for(unsigned layout=0;layout<2u;layout++) for(unsigned source=0;source<3u;source++){
    GmlRender *render=calloc(1,sizeof *render);GmlWin content={.bytecode=generations[generation]};
    FakeDevice fake={.on_demand=1,.readback_colour=0x7F25C080u};
    GmlRenderDeviceServices device=service(&fake);GmlRenderJournalMetrics metrics={0};
    uint32_t screen[W*H],shadow[W*H];const uint32_t *pixels=NULL;
    CHECK(render && !gml_render_init(render,NULL));render->win=&content;
    free(render->atlas[0].px);render->atlas[0].px=malloc(A*A*4u);
    render->atlas[0].w=render->atlas[0].h=A;
    render->tpag=calloc(1,sizeof *render->tpag);render->spr=calloc(1,sizeof *render->spr);
    CHECK(render->atlas[0].px && render->tpag && render->spr);
    render->n_tpag=render->n_spr=1;
    for(unsigned p=0;p<A*A;p++){
      uint8_t *pixel=render->atlas[0].px+4u*p;
      pixel[0]=(uint8_t)(p*7u);pixel[1]=(uint8_t)(p*11u);pixel[2]=(uint8_t)(p*17u);
      pixel[3]=(uint8_t)(source==0u?255u:source==1u?(p%2u?255u:0u):32u+p*5u);
    }
    render->tpag[0]=(GmlTpag){.sx=1,.sy=1,.sw=2,.sh=2,.tw=matched?2:1,.th=matched?2:1,
      .bw=layout?4:2,.bh=layout?4:2,.tx=layout?1:0,.ty=layout?1:0};
    render->spr[0]=(GmlSprite){.w=layout?4:2,.h=layout?4:2,
      .originx=1,.originy=1,.n_frames=1};
    render->spr[0].frame=calloc(1,sizeof(int));CHECK(render->spr[0].frame);
    device.observe_on_demand=fake_observe_on_demand;gml_render_bind_device(render,&device);
    for(unsigned p=0;p<W*H;p++) screen[p]=0xA5ABCDEFu;
    if(kind==1u || kind==2u){
      CHECK(gml_render_application_surface_ensure_owned(render,W,H));
      CHECK(gml_render_begin_resident_owned_application(render,0,0,0));
    }else{
      gml_render_application_surface_bind(render,screen,W,H,1);
      CHECK(gml_render_begin_resident_base(render,screen,W,H,0,0));
    }
    if(kind==2u) CHECK(gml_surface_set_target(render,0));
    if(kind==3u){int id=gml_surface_create(render,W,H);CHECK(id>0 && gml_surface_set_target(render,id));}
    if(gui){gml_render_gui_begin(render,W,H);gml_render_gui_set_size(render,W/2,H/2);}
    if(modern && gui) gml_render_gui_set_maximise(render,1,2,2,.25,-.5,W,H);
    render->interp=1;render->alphablend=(state+source)%2u==0u;
    render->blendmode=state/2u;render->alpha_test_enable=(int)(state&1u);
    render->alpha_test_ref=(uint8_t)(source*73u+kind*41u);
    gml_render_set_pending_fill(render,0x40273859u);
    gml_render_flush_pending_fill(render);CHECK(gml_render_scene_submit_pending(render));
    unsigned mask=(generation+state*3u+source*5u+kind*7u+gui*11u+layout*13u)%16u;
    if(modern) mask=1u+mask%14u;
    render->color_write_mask=(uint8_t)mask;memcpy(shadow,render->fb,sizeof shadow);
    double mapping=gml_render_gui_transform_active(render)?2.0:1.0;
    unsigned signs=(state+source+kind+gui+layout)%4u;
    double xs=(signs&1u?-1.5:.75)/mapping,ys=(signs&2u?-.75:1.5)/mapping;
    int precision_refused=modern==2 && (layout || source) &&
      ((!render->alphablend && render->blendmode!=2) ||
       (render->alphablend && (mask&8u) &&
        (render->blendmode==2?gml_render_target_preserves_alpha(render):
          !gml_render_target_preserves_alpha(render) && (!render->fb_opaque_known || !render->fb_all_opaque))));
    unsigned before=(unsigned)fake.submits;
    for(unsigned draw=0;draw<8u;draw++)
      gml_draw_sprite_ext(render,0,0,10.25/mapping,9.75/mapping,xs,ys,modern==2?31.0:0.0,0x70B0D0u,.4);
    if(mask && (precision_refused || ((state&1u) && (layout || source ||
       (modern && state/2u==0u && render->alpha_test_ref))))){
      CHECK(!render->resident_application_pass && fake.readbacks==1 &&
        (unsigned)fake.submits==before && render->interp==1 &&
        render->alpha_test_enable==(int)(state&1u));
      gml_render_free(render);free(render);continue;
    }
    CHECK(render->resident_application_pass && gml_render_scene_submit_pending(render));
    CHECK((unsigned)fake.submits==before+(mask?1u:0u) && !fake.readbacks && !fake.rgb_queries &&
      !memcmp(shadow,render->fb,sizeof shadow) && render->interp==1 &&
      render->alpha_test_enable==(int)(state&1u));
    if(mask) CHECK((modern==2?fake.last_batch_operations>=8u && fake.last_batch_operations%8u==0u:
                            fake.last_batch_operations==8u) && fake.last_filter==GML_RENDER_FILTER_HARDWARE_BILINEAR &&
      fake.last_quantisation==(modern && state/2u==0u
        ?GML_RENDER_QUANTISE_HARDWARE_SPRITE_MODERN:GML_RENDER_QUANTISE_HARDWARE_SPRITE_FIRSTGEN) &&
      fake.last_quad_state.write_mask==mask &&
      fake.last_quad_state.alpha_test_enabled==(state&1u));
    GmlRenderResource *target=gml_render_scene_target_resource_ensure(render);CHECK(target);
    gml_render_resource_journal_metrics(target,&metrics);
    CHECK(metrics.authority==GML_RENDER_RESOURCE_GPU_RESIDENT && !metrics.cpu_replays &&
      !metrics.device_checkpoints && !metrics.journal_bytes);
    CHECK(gml_render_resource_acquire_cpu_read(target,&pixels) && fake.readbacks==1 &&
      pixels[0]==fake.readback_colour && pixels[W*H-1u]==fake.readback_colour);
    fake.forbid_calls=1;
    CHECK(gml_render_resource_acquire_cpu_read(target,&pixels) && !fake.forbidden_calls &&
      pixels[0]==fake.readback_colour);
    fake.forbid_calls=0;gml_render_free(render);free(render);
  }
  return 1;
}

static int resident_physical_filtered_sprite_function(void){
  return resident_physical_filtered_sprite_function_impl(0,0);
}

static int resident_matched_filtered_sprite_function(void){
  return resident_physical_filtered_sprite_function_impl(1,0);
}

static int resident_modern_physical_filtered_sprite_function(void){
  return resident_physical_filtered_sprite_function_impl(1,1);
}

static int resident_modern_rotated_filtered_sprite_function(void){
  return resident_physical_filtered_sprite_function_impl(1,2);
}

static int resident_classic_large_point_sprite_function(void){
  enum { W=24,H=20,SOURCE=400 };
  const int generations[]={530,700,800};
  const uint8_t masks[]={15,0,5,10};
  for(unsigned generation=0;generation<3u;generation++)
  for(unsigned interpolation=0;interpolation<2u;interpolation++)
  for(unsigned target_kind=0;target_kind<4u;target_kind++)
  for(unsigned gui=0;gui<2u;gui++)
  for(unsigned variant=0;variant<12u;variant++){
    GmlRender *render=calloc(1,sizeof *render);GmlWin content={.classic_version=generations[generation]};
    FakeDevice fake={.on_demand=1};GmlRenderDeviceServices device=service(&fake);
    GmlRenderJournalMetrics metrics={0};uint32_t screen[W*H],shadow[W*H];
    CHECK(render && !gml_render_init(render,NULL));
    CHECK(gml_render_application_surface_ensure_owned(render,W,H));
    render->win=&content;render->classic=1;
    free(render->atlas[0].px);render->atlas[0].px=malloc(SOURCE*SOURCE*4u);
    render->atlas[0].w=render->atlas[0].h=SOURCE;
    render->tpag=calloc(1,sizeof *render->tpag);render->spr=calloc(1,sizeof *render->spr);
    CHECK(render->atlas[0].px && render->tpag && render->spr);
    render->n_tpag=render->n_spr=1;
    render->tpag[0]=(GmlTpag){.sw=SOURCE,.sh=SOURCE,.tw=SOURCE,.th=SOURCE,.bw=SOURCE,.bh=SOURCE};
    render->spr[0]=(GmlSprite){.w=SOURCE,.h=SOURCE,.originx=SOURCE/2,.originy=SOURCE/2,.n_frames=1};
    render->spr[0].frame=calloc(1,sizeof(int));CHECK(render->spr[0].frame);
    for(unsigned p=0;p<SOURCE*SOURCE;p++){
      uint8_t *pixel=render->atlas[0].px+4u*p;
      pixel[0]=(uint8_t)(p*3u);pixel[1]=(uint8_t)(p*5u);pixel[2]=(uint8_t)(p*7u);
      pixel[3]=(uint8_t)(variant%3u==0u?255u:variant%3u==1u?(p&1u?255u:0u):(p%254u+1u));
    }
    device.observe_on_demand=fake_observe_on_demand;
    gml_render_bind_device(render,&device);
    if(target_kind==1u || target_kind==2u)
      CHECK(gml_render_begin_resident_owned_application(render,0,0,0));
    else{
      gml_render_application_surface_bind(render,screen,W,H,1);
      CHECK(gml_render_begin_resident_base(render,screen,W,H,0,0));
    }
    if(target_kind==2u) CHECK(gml_surface_set_target(render,0));
    if(target_kind==3u){int id=gml_surface_create(render,W,H);CHECK(id>0 && gml_surface_set_target(render,id));}
    if(gui){gml_render_gui_begin(render,W,H);gml_render_gui_set_size(render,W/2,H/2);}
    gml_render_set_pending_fill(render,0x40273859u);gml_render_flush_pending_fill(render);
    CHECK(gml_render_scene_submit_pending(render));memcpy(shadow,render->fb,sizeof shadow);
    render->interp=(int)interpolation;render->alphablend=variant<6u;
    render->blendmode=(int)(variant%3u);render->color_write_mask=masks[variant%4u];
    render->alpha_test_enable=variant&1u;render->alpha_test_ref=(uint8_t)(variant*23u);
    double mapping=gml_render_gui_transform_active(render)?2.0:1.0;
    double xs=(variant&2u?-1.0:1.0)/mapping,ys=(variant&4u?-1.0:1.0)/mapping;
    double alpha=variant%4u==0u?1.0:variant%4u==1u?0.0:variant%4u==2u?.4:1e-5;
    for(unsigned draw=0;draw<8u;draw++)
      gml_draw_sprite_ext(render,0,0,(W/2.0)/mapping,(H/2.0)/mapping,xs,ys,
        variant&8u?45.0:0.0,variant&1u?0x70B0D0u:0xFFFFFFu,alpha);
    if(!render->resident_application_pass || fake.readbacks)
      fprintf(stderr,"Classic large sprite: generation=%d interpolation=%u target=%u gui=%u "
        "variant=%u resident=%d reads=%d\n",generations[generation],interpolation,target_kind,gui,
        variant,render->resident_application_pass,fake.readbacks);
    CHECK(render->resident_application_pass && gml_render_scene_submit_pending(render) && !fake.readbacks);
    GmlRenderResource *target=gml_render_scene_target_resource_ensure(render);CHECK(target);
    gml_render_resource_journal_metrics(target,&metrics);
    CHECK(metrics.gpu_version==metrics.logical_version && metrics.cpu_version<metrics.logical_version &&
          !metrics.cpu_replays && !metrics.device_checkpoints && !memcmp(shadow,render->fb,sizeof shadow));
    gml_render_free(render);free(render);
  }
  return 1;
}

/* Capacity rescue owns only bounded geometry and immutable atlas authority.
 * Complete RGBA, batching and detached bytes are paired on the real device. */
static int resident_external_base_mask(void){
  enum { W=8,H=6 };
  const int generations[]={530,700,800,13,15,17};
  for(unsigned generation=0;generation<6u;generation++)
  for(unsigned owner=0;owner<4u;owner++) for(unsigned gui=0;gui<2u;gui++){
    GmlRender *r=calloc(1,sizeof *r);GmlWin content={0};
    FakeDevice fake={.on_demand=1,.readback_colour=0xFF182838u};
    GmlRenderDeviceServices device=service(&fake);
    uint32_t root[W*H],screen[W*H],shadow[W*H];
    CHECK(r && !gml_render_init(r,NULL));
    content.classic_version=generations[generation]>100?generations[generation]:0;
    content.bytecode=generations[generation]>100?0:generations[generation];
    r->win=&content;r->classic=content.classic_version!=0;
    for(unsigned p=0;p<W*H;p++)root[p]=0x73513927u+p;
    memcpy(shadow,root,sizeof root);device.observe_on_demand=fake_observe_on_demand;
    gml_render_bind_device(r,&device);gml_render_set_scene_device(r,1);
    if(owner==1u || owner==2u){
      CHECK(gml_render_application_surface_ensure_owned(r,W,H));
      CHECK(gml_render_begin_resident_owned_application(r,-3,2,0));
    }else{
      gml_render_application_surface_bind(r,screen,W,H,1);
      CHECK(gml_render_begin_resident_base(r,screen,W,H,-3,2));
    }
    if(owner==2u)CHECK(gml_surface_set_target(r,0));
    if(owner==3u){int id=gml_surface_create(r,W,H);CHECK(id>0 && gml_surface_set_target(r,id));}
    if(gui){gml_render_gui_begin(r,W,H);gml_render_gui_set_size(r,W/2,H/2);}
    gml_render_set_pending_fill(r,0xD3273859u);gml_render_flush_pending_fill(r);
    CHECK(gml_render_scene_submit_pending(r));
    uint32_t *bound=r->fb;int target=r->target_id,depth=r->target_sp;
    int resident=r->resident_application_pass;double camera_x=r->cam_x,camera_y=r->cam_y;
    CHECK(gml_render_completed_screen_mask(r,root,W,H,2,1,4,3));
    CHECK(!fake.readbacks && !fake.rgb_queries && !memcmp(root,shadow,sizeof root) &&
      r->fb==bound && r->target_id==target && r->target_sp==depth &&
      r->resident_application_pass==resident && r->cam_x==camera_x && r->cam_y==camera_y &&
      fake.last_batch_operations==4u && fake.last_opcode==GML_RENDER_OP_CLEAR &&
      fake.last_target_region.x==6 && fake.last_target_region.y==1 &&
      fake.last_target_region.width==2u && fake.last_target_region.height==3u);
    GmlRenderResource *resource=gml_render_base_resource_lookup(r,root,NULL,NULL);
    GmlRenderJournalMetrics metrics={0};CHECK(resource);
    gml_render_resource_journal_metrics(resource,&metrics);
    CHECK(metrics.gpu_version==metrics.logical_version && metrics.cpu_version<metrics.logical_version &&
      !metrics.cpu_replays && !metrics.device_checkpoints && !metrics.journal_bytes);
    CHECK(gml_render_completed_screen_mask(r,root,W,H,0,0,W,H));
    int submits=fake.submits;
    r->resident_application_pass=0;
    CHECK(!gml_render_completed_screen_mask(r,root,W,H,1,1,2,2) && fake.submits==submits);
    r->resident_application_pass=resident;
    CHECK(!gml_render_completed_screen_mask(r,NULL,W,H,0,0,1,1) &&
      !gml_render_completed_screen_mask(r,root,4097,H,0,0,1,1) && fake.submits==submits);
    if(owner==1u || owner==2u)
      CHECK(!gml_render_completed_screen_mask(r,r->app_surface_owned,W,H,1,1,2,2));
    if(owner==3u)CHECK(!gml_render_completed_screen_mask(r,r->fb,W,H,1,1,2,2));
    CHECK(gml_render_completed_screen_read(r,root,W,H) && fake.readbacks==1);
    CHECK(root[0]==fake.readback_colour); /* Fake observes authority,not device raster pixels. */
    CHECK(gml_render_completed_screen_write(r,root,W,H));root[0]=0xFF102030u;
    CHECK(gml_render_completed_screen_mask(r,root,W,H,-10,-10,1,1));
    CHECK(fake.last_batch_operations==1u && fake.last_target_region.width==W &&
      fake.last_target_region.height==H && root[0]==0xFF102030u);
    CHECK(gml_render_masked_bases_observe(r) && fake.readbacks==2 && root[0]==fake.readback_colour);
    gml_render_free(r);free(r);
  }
  return 1;
}

static int resident_nearest_geometry_capacity(void){
  enum { W=320,H=192,AW=512,AH=416 };
  const int generations[]={530,700,800,13,15,17};
  const double geometry[][3]={{-.8,-.8,80},{2.3,1.7,204},
    {.375-0x1p-46,.375-0x1p-46,0}};
  unsigned row_controls=0;
  for(unsigned generation=0;generation<6u;generation++)
  for(unsigned kind=0;kind<4u;kind++) for(unsigned gui=0;gui<2u;gui++)
  for(unsigned shape=0;shape<3u;shape++) for(unsigned state=0;state<6u;state++){
    GmlRender *r=calloc(1,sizeof *r);GmlWin content={0};
    FakeDevice fake={.on_demand=1};GmlRenderDeviceServices device=service(&fake);
    uint32_t screen[W*H],shadow[W*H];GmlRenderJournalMetrics metrics={0};
    CHECK(r && !gml_render_init(r,NULL));
    content.classic_version=generations[generation]>100?generations[generation]:0;
    content.bytecode=generations[generation]>100?0:generations[generation];
    r->win=&content;r->classic=content.classic_version!=0;
    free(r->atlas[0].px);r->atlas[0].px=malloc(AW*AH*4u);
    r->atlas[0].w=AW;r->atlas[0].h=AH;
    r->tpag=calloc(1,sizeof *r->tpag);r->spr=calloc(1,sizeof *r->spr);
    CHECK(r->atlas[0].px && r->tpag && r->spr);r->n_tpag=r->n_spr=1;
    int width=shape==2u?480:400,height=shape==2u?284:400;
    r->tpag[0]=(GmlTpag){.sx=4,.sy=4,.sw=width,.sh=height,.tw=width,.th=height,
      .bw=width,.bh=shape==2u?320:400,.ty=shape==2u?20:0};
    r->spr[0]=(GmlSprite){.w=width,.h=shape==2u?320:400,.originx=width/2,
      .originy=shape==2u?140:200,.n_frames=1};
    r->spr[0].frame=calloc(1,sizeof(int));CHECK(r->spr[0].frame);
    for(unsigned p=0;p<AW*AH;p++){
      uint8_t *pixel=r->atlas[0].px+4u*p;
      pixel[0]=(uint8_t)(p*3u);pixel[1]=(uint8_t)(p*5u);pixel[2]=(uint8_t)(p*7u);
      pixel[3]=(uint8_t)(state%3u==0u?255u:state%3u==1u?(p&1u?255u:0u):(p%254u+1u));
    }
    device.observe_on_demand=fake_observe_on_demand;gml_render_bind_device(r,&device);
    if(kind==1u || kind==2u){
      CHECK(gml_render_application_surface_ensure_owned(r,W,H));
      CHECK(gml_render_begin_resident_owned_application(r,-3,2,0));
    }else{
      gml_render_application_surface_bind(r,screen,W,H,1);
      CHECK(gml_render_begin_resident_base(r,screen,W,H,-3,2));
    }
    if(kind==2u)CHECK(gml_surface_set_target(r,0));
    if(kind==3u){int id=gml_surface_create(r,W,H);CHECK(id>0 && gml_surface_set_target(r,id));}
    if(gui){gml_render_gui_begin(r,W,H);gml_render_gui_set_size(r,W/2,H/2);}
    gml_render_set_pending_fill(r,0xD3273859u);gml_render_flush_pending_fill(r);
    CHECK(gml_render_scene_submit_pending(r));memcpy(shadow,r->fb,sizeof shadow);
    r->alphablend=state<3u;r->blendmode=(int)(state%3u);
    r->alpha_test_enable=(int)(state&1u);r->alpha_test_ref=(uint8_t)(state*31u);
    r->color_write_mask=state&1u?5u:15u;
    double mx=1,my=1;gml_render_draw_map_scale(r,&mx,&my);
    double x=r->cam_x+W*.5,y=r->cam_y+(shape==2u?64:H*.5);
    gml_draw_sprite_ext(r,0,0,x/mx,y/my,geometry[shape][0]/mx,geometry[shape][1]/my,
      geometry[shape][2],state&1u?0x70B0D0u:0xFFFFFFu,state%3u?.37:1);
    CHECK(r->resident_application_pass && gml_render_scene_submit_pending(r) &&
      !fake.readbacks && !fake.rgb_queries && !memcmp(shadow,r->fb,sizeof shadow));
    row_controls+=(unsigned)(fake.last_quantisation==GML_RENDER_QUANTISE_INTEGER_ROW_TINT_TRUNC);
    GmlRenderResource *target=gml_render_scene_target_resource_ensure(r);CHECK(target);
    gml_render_resource_journal_metrics(target,&metrics);
    CHECK(metrics.gpu_version==metrics.logical_version && metrics.cpu_version<metrics.logical_version &&
      !metrics.cpu_replays && !metrics.device_checkpoints && !metrics.journal_bytes);
    if(!generation && !kind && !gui && !shape && !state){
      for(unsigned exclusion=0;exclusion<3u;exclusion++){
        r->active_shader=exclusion==0u?0:-1;r->fog_flat=exclusion==1u;
        double scale=exclusion==2u?0x1p-15:.8;
        CHECK(!gml_render_resident_masked_sprite(r,0,0,x,y,scale,scale,80,0xFFFFFFu,1));
        CHECK(r->resident_application_pass && !fake.readbacks && !memcmp(shadow,r->fb,sizeof shadow));
      }
    }
    gml_render_free(r);free(r);
  }
  CHECK(row_controls>0u);return 1;
}

static int resident_background_function(int fractional){
  enum { W=24,H=20 };
  const int generations[]={530,700,800,13,15,17};
  const double alphas[]={1.0,0.0,0.37};
  int stored_extents=fractional==3;
  if(stored_extents) fractional=0;
  for(unsigned interpolation=0;interpolation<2u;interpolation++)
  for(unsigned generation=0;generation<6u;generation++)
  for(unsigned target_kind=0;target_kind<4u;target_kind++)
  for(unsigned gui=0;gui<2u;gui++) for(unsigned repeat=0;repeat<(fractional==2?8u:4u);repeat++)
  for(unsigned draw_state=0;draw_state<4u;draw_state++)
  for(unsigned disabled=0;disabled<2u;disabled++) for(unsigned tint=0;tint<2u;tint++)
  for(unsigned a=0;a<3u;a++){
    if(interpolation && (fractional || generations[generation]<100)) continue;
    if(stored_extents && (tint!=(generation+target_kind+gui+repeat+draw_state+disabled)%2u ||
       a!=(target_kind+gui+repeat+draw_state+disabled)%3u)) continue;
    GmlRender *render=calloc(1,sizeof *render);GmlWin content={0};
    FakeDevice fake={.on_demand=1,.readback_colour=0x7F25C080u};
    GmlRenderDeviceServices device=service(&fake);GmlRenderJournalMetrics metrics={0};
    uint32_t screen[W*H],shadow[W*H];const uint32_t *pixels=NULL;
    CHECK(render && !gml_render_init(render,NULL));
    content.classic_version=generations[generation]>100?generations[generation]:0;
    content.bytecode=content.classic_version?0:generations[generation];
    render->win=&content;render->classic=content.classic_version!=0;
    render->interp=(int)interpolation;
    uint8_t rgba[16]={208,32,16,255,16,208,32,0,32,16,208,128,192,176,160,64};
    free(render->atlas[0].px);render->atlas[0].px=malloc(sizeof rgba);
    render->atlas[0].w=render->atlas[0].h=2;
    render->atlas[0].blob=1u;
    render->tpag=calloc(1,sizeof *render->tpag);render->bg=calloc(1,sizeof *render->bg);
    CHECK(render->atlas[0].px && render->tpag && render->bg);
    memcpy(render->atlas[0].px,rgba,sizeof rgba);render->n_tpag=render->n_bg=1;
    render->tpag[0]=(GmlTpag){.sw=2,.sh=2,.tw=2,.th=2,.bw=4,.bh=4,.tx=1,.ty=1};
    if(stored_extents){
      unsigned variation=(generation+target_kind+gui+repeat+draw_state+disabled)%5u;
      render->tpag[0].tw=variation==0u || variation==2u?0:variation==3u?5:variation==4u?1:2;
      render->tpag[0].th=variation==1u || variation==2u?0:variation>=3u?4:2;
      render->atlas[0].blob=(generation+target_kind+gui+repeat+draw_state+disabled)&1u;
    }
    int original_stored_width=render->tpag[0].tw,original_stored_height=render->tpag[0].th;
    render->bg[0].tpag=0;
    device.observe_on_demand=fake_observe_on_demand;gml_render_bind_device(render,&device);
    for(unsigned p=0;p<W*H;p++) screen[p]=0xA5ABCDEFu;
    if(target_kind==1u || target_kind==2u){
      CHECK(gml_render_application_surface_ensure_owned(render,W,H));
      CHECK(gml_render_begin_resident_owned_application(render,0,0,0));
    }else{
      gml_render_application_surface_bind(render,screen,W,H,1);
      CHECK(gml_render_begin_resident_base(render,screen,W,H,0,0));
    }
    if(target_kind==2u) CHECK(gml_surface_set_target(render,0));
    if(target_kind==3u){int id=gml_surface_create(render,W,H);CHECK(id>0 && gml_surface_set_target(render,id));}
    if(gui){gml_render_gui_begin(render,W,H);gml_render_gui_set_size(render,W/2,H/2);}
    render->alphablend=!disabled;render->blendmode=draw_state>=2u?2:0;
    render->alpha_test_enable=(int)(draw_state&1u);render->alpha_test_ref=127u;
    gml_render_set_pending_fill(render,0x40273859u);
    gml_render_flush_pending_fill(render);CHECK(gml_render_scene_submit_pending(render));
    memcpy(shadow,render->fb,sizeof shadow);int submitted=fake.submits;
    if(!a && tint && !draw_state && !disabled && !repeat){
      /* An excluded state must refuse before submission or observation. */
      GmlTpag tile_view=render->tpag[0];tile_view.project_authored_edges=1;
#define BG_REJECT(set,restore) do { set; \
      CHECK(!(fractional==2 \
        ?gml_render_resident_nearest_tile_page(render,&tile_view,6,5,2,1,0x70B0D0u,.37) \
        :gml_render_resident_integer_background_page(render,0,6,5,2,1,0x70B0D0u,.37))); \
      restore; CHECK(fake.submits==submitted && !fake.readbacks); \
      } while(0)
      if(!render->classic || fractional==2){
        BG_REJECT(render->interp=1,render->interp=(int)interpolation);
      }else{
        /* The Classic logical writer stays nearest even when presentation
         * requests interpolation. Keep a positive packet/shadow discriminator,
         * not the obsolete unsupported-sampler assertion. */
        render->interp=1;
        CHECK(gml_render_resident_integer_background_page(render,0,6,5,2,1,0x70B0D0u,.37));
        CHECK(render->interp==1 && fake.submits==submitted && !fake.readbacks &&
              !memcmp(shadow,render->fb,sizeof shadow));
        render->interp=(int)interpolation;
      }
      BG_REJECT(render->fog_flat=1,render->fog_flat=0);
      BG_REJECT(render->active_shader=0,render->active_shader=-1);
      BG_REJECT(render->color_write_mask=7u,render->color_write_mask=15u);
      BG_REJECT(render->blendmode=1,render->blendmode=0);
      BG_REJECT(render->blend_equation=2,render->blend_equation=1);
      BG_REJECT(render->blend_equation_alpha=2,render->blend_equation_alpha=1);
      if(target_kind==3u){
        /* An allocated root plane is inert on the numbered allocation. Keep
         * the active-root/application and interpolation-plane fences below. */
        render->classic_phase_y=screen;
        CHECK(fractional==2
          ?gml_render_resident_nearest_tile_page(render,&tile_view,6,5,2,1,0x70B0D0u,.37)
          :gml_render_resident_integer_background_page(render,0,6,5,2,1,0x70B0D0u,.37));
        render->classic_phase_y=NULL;
        CHECK(fake.submits==submitted && !fake.readbacks && !memcmp(shadow,render->fb,sizeof shadow));
        /* This positive setup queues a real draw, unlike the rejection cases.
         * Submit it separately so the later eight-call batching proof starts
         * with exactly its original empty queue and unchanged strict counts. */
        CHECK(gml_render_scene_submit_pending(render) && fake.submits==submitted+1 &&
              !fake.readbacks && !fake.rgb_queries && !memcmp(shadow,render->fb,sizeof shadow));
        submitted=fake.submits;
      }else BG_REJECT(render->classic_phase_y=screen,render->classic_phase_y=NULL);
      BG_REJECT(render->classic_interp_phase[0]=screen,render->classic_interp_phase[0]=NULL);
      BG_REJECT(render->app_interp_phase[0]=screen,render->app_interp_phase[0]=NULL);
      BG_REJECT(render->atlas[0].runtime_font_page=1,render->atlas[0].runtime_font_page=0);
#undef BG_REJECT
      if(fractional==2){
        CHECK(!gml_render_resident_nearest_tile_page(render,&tile_view,NAN,5,2,1,0xFFFFFFu,0.0));
        CHECK(!gml_render_resident_nearest_tile_page(render,&tile_view,6,5,NAN,1,0xFFFFFFu,0.0));
        CHECK(fake.submits==submitted && !fake.readbacks);
      }
      CHECK(!gml_render_resident_integer_background_page(render,0,6,5,.5,1,0x70B0D0u,.37));
      CHECK(!gml_render_resident_integer_background_page(render,0,6,5,16385,1,0x70B0D0u,.37));
      CHECK(!gml_render_resident_integer_background_page(render,0,6,5,8193,1,0x70B0D0u,.37));
      CHECK(!gml_render_resident_integer_background_page(render,0,6,5,1.5,1,0x70B0D0u,.37));
      if(!fractional && !stored_extents){
        /* Keep every former scale boundary and the projected-extent limit,
         * independently in either axis of this two-texel source. */
        const unsigned wider[]={65,207,256,4096,8192};
        for(unsigned index=0;index<56u+sizeof wider/sizeof wider[0];index++)
        for(unsigned axis=0;axis<2u;axis++){
          unsigned scale=index<56u?index+9u:wider[index-56u];
          CHECK(gml_render_resident_integer_background_page(render,0,6,5,
            axis?1.0:scale,axis?scale:1.0,0x70B0D0u,.37));
          CHECK(gml_render_scene_submit_pending(render));
          CHECK(!fake.readbacks && !memcmp(shadow,render->fb,sizeof shadow));
        }
        submitted=fake.submits;
      }
      /* The public tiled CPU owner treats zero/negative periods as no-ops. */
      for(unsigned axis=0;axis<4u;axis++)
        gml_draw_background_tiled_ext(render,0,6,5,axis<2u?(axis?-1:0):1,
          axis>=2u?(axis==3u?-1:0):1,0x70B0D0u,.37,1,1);
      CHECK(fake.submits==submitted && !fake.readbacks && render->resident_application_pass &&
            !memcmp(shadow,render->fb,sizeof shadow));
    }
    if(fractional==2 && generation==5u && !target_kind && !gui && !repeat &&
       (a==2u || a==0u) && !tint && !draw_state){
      /* Detached partial-opacity and reflected full-opacity white unit crops
       * miss the cached copy. Both sample row one at the authored half tie. */
      gml_draw_background_tile(render,0,1,1,2,2,W-0.75,H-1.5,1,1,
                               a==0u,0,0,0xFFFFFFu,alphas[a]);
      CHECK(render->resident_application_pass && gml_render_scene_submit_pending(render));
      const GmlRenderQuad *q=&fake.last_quad;
      CHECK(q->y[2]>q->y[0] && q->y[0]<=H-0.5 && q->y[2]>H-0.5);
      double sample_y=q->v[0]+(q->v[2]-q->v[0])*(H-0.5-q->y[0])/(q->y[2]-q->y[0]);
      CHECK(floor(sample_y*render->atlas[0].h)==1.0 &&
            !fake.readbacks && !memcmp(shadow,render->fb,sizeof shadow));
      submitted=fake.submits;
    }
    if(fractional==2 && repeat==4u && !a && !tint && draw_state==1u){
      double physical_unit=gml_render_gui_transform_active(render)?0.5:1.0;
      gml_draw_background_tile(render,0,1,1,2,2,6.018,5.07,physical_unit,physical_unit,
                               0,0,1,0xFFFFFFu,1.0);
      CHECK(render->resident_application_pass && gml_render_scene_submit_pending(render));
      CHECK(!fake.last_quad_state.alpha_test_enabled &&
            !fake.last_quad_state.alpha_test_reference &&
            render->alpha_test_enable==1 && render->alpha_test_ref==127u &&
            !fake.readbacks && !memcmp(shadow,render->fb,sizeof shadow));
      submitted=fake.submits;
    }
    for(unsigned draw=0;draw<8u;draw++){
      if(fractional==1 && !repeat)
        gml_draw_background_ext(render,0,6.018,5.07,1.25,.75,
          tint?0x70B0D0u:0xFFFFFFu,alphas[a]);
      if(fractional==2)
        gml_draw_background_tile(render,0,1,1,repeat>=4u?1:2,2,6.018,5.07,1.25,.75,
          (int)(repeat&1u),(int)((repeat>>1u)&1u),(int)(repeat>>2u),
          tint?0x70B0D0u:0xFFFFFFu,alphas[a]);
      else
        gml_draw_background_tiled_ext(render,0,6.018,5.07,fractional?1.25:2.0,fractional?.75:1.0,
          tint?0x70B0D0u:0xFFFFFFu,alphas[a],(int)(repeat&1u),(int)(repeat>>1u));
    }
    CHECK(render->resident_application_pass && gml_render_scene_submit_pending(render));
    int noop=a==1u;
    CHECK(fake.submits==submitted+(noop?0:1) && !fake.readbacks && !fake.rgb_queries &&
          !memcmp(shadow,render->fb,sizeof shadow));
    GmlRenderResource *resource=gml_render_scene_target_resource_ensure(render);CHECK(resource);
    gml_render_resource_journal_metrics(resource,&metrics);
    CHECK(metrics.authority==GML_RENDER_RESOURCE_GPU_RESIDENT && !metrics.cpu_replays && !metrics.journal_bytes);
    if(!noop){
      CHECK(fake.last_batch_operations>=8u && fake.last_filter==GML_RENDER_FILTER_NEAREST);
      if(fake.last_quantisation){
        int cardinal=fractional==2 && repeat>=4u;
        CHECK(fake.last_quantisation==((generation==5u && !cardinal)?GML_RENDER_QUANTISE_INTEGER_TINT_ROUND:
                                                   GML_RENDER_QUANTISE_INTEGER_TINT_TRUNC));
        double expected=fractional!=2 && render->atlas[0].blob && generations[generation]==13 && a==2u
          ?floor(alphas[a]*255.0)/255.0:alphas[a];
        int packed_empty_cutoff=fractional!=2 && render->atlas[0].blob &&
          generations[generation]==13 && a==2u && !draw_state;
        int effective_cutout=!cardinal || !disabled;
        int enabled=(effective_cutout && (draw_state&1u)) || packed_empty_cutoff ||
          (cardinal && (draw_state&2u));
        unsigned reference=effective_cutout && (draw_state&1u)?127u:packed_empty_cutoff?1u:0u;
        CHECK(fake.last_filtered_draw_alpha==expected &&
              fake.last_quad_state.alpha_test_enabled==!!enabled &&
              fake.last_quad_state.alpha_test_reference==reference &&
              render->alpha_test_enable==(int)(draw_state&1u) && render->alpha_test_ref==127u);
      }else CHECK(!a && !tint && !draw_state && !disabled);
    }
    CHECK(gml_render_resource_acquire_cpu_read(resource,&pixels) && fake.readbacks==1 &&
      pixels[0]==fake.readback_colour && pixels[W*H-1u]==fake.readback_colour);
    fake.forbid_calls=1;
    CHECK(gml_render_resource_acquire_cpu_read(resource,&pixels) && !fake.forbidden_calls &&
      pixels[0]==fake.readback_colour && pixels[W*H-1u]==fake.readback_colour);
    CHECK(render->tpag[0].tw==original_stored_width && render->tpag[0].th==original_stored_height);
    fake.forbid_calls=0;gml_render_free(render);free(render);
  }
  return 1;
}

/* Admission/authority proof only. Real-context public-string controls own D2. */
static int resident_nearest_text_function(int outside){
  enum {W=24,H=20};
  const int generations[]={530,700,800,13,15,17};
  const double alphas[]={1,0,.37};
  const double scales[][2]={{1,1},{2,3},{-2,-3},{.75,1.5}};
  int runtime=outside==3;
  if(runtime) outside=0;
  for(unsigned generation=0;generation<6u;generation++)
  for(unsigned interpolation=0;interpolation<(generations[generation]>100?2u:1u);interpolation++)
  for(unsigned target_kind=0;target_kind<4u;target_kind++)
  for(unsigned gui=0;gui<2u;gui++) for(unsigned font_kind=0;font_kind<(runtime?1u:2u);font_kind++)
  for(unsigned draw_state=0;draw_state<4u;draw_state++)
  for(unsigned disabled=0;disabled<2u;disabled++) for(unsigned a=0;a<3u;a++)
  for(unsigned scale=0;scale<4u;scale++){
    GmlRender *r=calloc(1,sizeof *r);GmlWin content={0};
    FakeDevice fake={.on_demand=1,.readback_colour=0x7F25C080u};
    GmlRenderDeviceServices device=service(&fake);GmlRenderJournalMetrics metrics={0};
    uint32_t screen[W*H],shadow[W*H];const uint32_t *pixels=NULL;
    CHECK(r && !gml_render_init(r,NULL));
    content.classic_version=generations[generation]>100?generations[generation]:0;
    content.bytecode=content.classic_version?0:generations[generation];
    content.has_room_layers=content.bytecode==17;
    r->win=&content;r->classic=content.classic_version!=0;
    const uint8_t rgba[16]={208,32,16,255,16,208,32,0,32,16,208,128,192,176,160,64};
    free(r->atlas[0].px);r->atlas[0].px=malloc(sizeof rgba);r->atlas[0].w=r->atlas[0].h=2;
    r->tpag=calloc(1,sizeof *r->tpag);r->spr=calloc(1,sizeof *r->spr);
    CHECK(r->atlas[0].px && r->tpag && r->spr);
    memcpy(r->atlas[0].px,rgba,sizeof rgba);r->n_tpag=r->n_spr=r->n_fonts=1;
    r->tpag[0]=(GmlTpag){.sw=2,.sh=2,.tw=2,.th=2,.bw=4,.bh=4,.tx=1,.ty=1};
    r->spr[0]=(GmlSprite){.w=4,.h=4,.n_frames=1,.frame=calloc(1,sizeof(int))};
    CHECK(r->spr[0].frame);
    GmlFont *font=&r->fonts[0];font->sprite=0;font->first='A';
    if(!font_kind){
      font->real=1;font->sprite=-1;font->atlas=0;font->line_height=font->align_height=2;
      font->glyphs=calloc(1,sizeof *font->glyphs);CHECK(font->glyphs);font->n_glyphs=1;
      font->glyphs[0]=(GmlGlyph){.w=2,.h=2,.shift=3,.ch='A'};
      for(unsigned cp=0;cp<256u;cp++) font->glyph_by_char[cp]=-1;
      font->glyph_by_char['A']=0;
      if(runtime){font->runtime_owned=1;r->atlas[0].runtime_font_page=1;}
    }
    r->font=0;device.observe_on_demand=fake_observe_on_demand;gml_render_bind_device(r,&device);
    for(unsigned p=0;p<W*H;p++) screen[p]=0xA5ABCDEFu;
    if(target_kind==1u || target_kind==2u){
      CHECK(gml_render_application_surface_ensure_owned(r,W,H));
      CHECK(gml_render_begin_resident_owned_application(r,0,0,0));
    }else{
      gml_render_application_surface_bind(r,screen,W,H,1);
      CHECK(gml_render_begin_resident_base(r,screen,W,H,0,0));
    }
    if(target_kind==2u) CHECK(gml_surface_set_target(r,0));
    if(target_kind==3u){int id=gml_surface_create(r,W,H);CHECK(id>0 && gml_surface_set_target(r,id));}
    if(gui){gml_render_gui_begin(r,W,H);gml_render_gui_set_size(r,W/2,H/2);}
    r->interp=outside?1:(int)interpolation;
    r->alphablend=!disabled;r->blendmode=draw_state>=2u?2:0;
    r->alpha_test_enable=(int)(draw_state&1u);r->alpha_test_ref=127;
    gml_render_set_pending_fill(r,0x40273859u);gml_render_flush_pending_fill(r);
    CHECK(gml_render_scene_submit_pending(r));memcpy(shadow,r->fb,sizeof shadow);
    if(outside==2){
      /* An empty glyph must not consume an automatic composition's intent.
       * Real pixel/order qualification belongs to the device underlay control. */
      gml_render_set_pending_fill(r,0x60123456u);
      gml_render_set_pending_underlay(r,0,0,W,H);
    }
    int submitted=fake.submits;
    uint64_t target_version=gml_render_resource_image_ref(gml_render_scene_target_resource_ensure(r)).content_version;
    double mx=1,my=1;gml_render_draw_map_scale(r,&mx,&my);
    for(unsigned draw=0;draw<8u;draw++){
      if(runtime && draw==1u){
        /* A completed glyph must not retain the old pointer across growth.
         * Model the existing font rasterizer's mutation/version boundary. */
        GmlAtlas *atlas=&r->atlas[0];
        CHECK(gml_render_application_scene_flush(r));
        uint8_t *grown=calloc(1,32u);CHECK(grown);
        memcpy(grown,atlas->px,16u);grown[3]=96u;
        free(atlas->px);atlas->px=grown;atlas->h=4;
        if(atlas->scene_source_version) atlas->scene_source_version++;
      }
      gml_draw_text_transformed(r,(outside?200.0:12.125)/mx,(outside?200.0:10.25)/my,
        "A",scales[scale][0]/mx,scales[scale][1]/my,0,0x70B0D0u,alphas[a]);
    }
    CHECK(r->resident_application_pass && gml_render_scene_submit_pending(r));
    if(outside==2) CHECK(r->pending_fill && r->pending_fill_color==0x60123456u &&
                        r->pending_underlay);
    CHECK(fake.submits==submitted+(outside || a==1u?0:runtime?2:1) && !fake.readbacks && !fake.rgb_queries &&
          !memcmp(shadow,r->fb,sizeof shadow));
    GmlRenderResource *resource=gml_render_scene_target_resource_ensure(r);CHECK(resource);
    if(outside) CHECK(gml_render_resource_image_ref(resource).content_version==target_version &&
      r->interp==1 && r->alpha_test_enable==(int)(draw_state&1u) && r->alpha_test_ref==127);
    gml_render_resource_journal_metrics(resource,&metrics);
    CHECK(metrics.authority==GML_RENDER_RESOURCE_GPU_RESIDENT && !metrics.cpu_replays && !metrics.journal_bytes);
    if(a!=1u && !outside){
      CHECK(fake.last_batch_operations>=(runtime?7u:8u) && fake.last_filter==GML_RENDER_FILTER_NEAREST &&
            r->alpha_test_enable==(int)(draw_state&1u) && r->alpha_test_ref==127 &&
            r->interp==(int)interpolation);
    }
    if(runtime && a!=1u){
      uint64_t incarnation=r->atlas[0].scene_source_incarnation;
      int submitted_before_delete=fake.submits;
      gml_draw_text_transformed(r,12.125/mx,10.25/my,"A",
        scales[scale][0]/mx,scales[scale][1]/my,0,0x70B0D0u,alphas[a]);
      CHECK(r->resident_application_pass && fake.submits==submitted_before_delete);
      gml_font_delete(r,0);
      CHECK(!gml_render_font_exists(r,0) && !r->atlas[0].px &&
            fake.submits==submitted_before_delete+1 && !fake.readbacks &&
            fake.last_retired_image.incarnation==incarnation);
    }
    CHECK(gml_render_resource_acquire_cpu_read(resource,&pixels) && fake.readbacks==1 &&
          pixels[0]==fake.readback_colour && pixels[W*H-1u]==fake.readback_colour);
    fake.forbid_calls=1;
    CHECK(gml_render_resource_acquire_cpu_read(resource,&pixels) && !fake.forbidden_calls &&
          pixels[0]==fake.readback_colour && pixels[W*H-1u]==fake.readback_colour);
    fake.forbid_calls=0;gml_render_free(r);free(r);
  }
  return 1;
}

static int resident_rotated_text_function(void){
  enum { W=24,H=20 };
  const int generations[]={530,700,800,13,15,17};
  const double rotations[]={-1,45,90};
  const double alphas[]={1,0,.37};
  const double scales[][2]={{1,1},{2,3},{-2,-3},{.75,1.5}};
  for(unsigned generation=0;generation<6u;generation++)
  for(unsigned interpolation=0;interpolation<(generations[generation]>100?2u:1u);interpolation++)
  for(unsigned target_kind=0;target_kind<4u;target_kind++)
  for(unsigned gui=0;gui<2u;gui++) for(unsigned font_kind=0;font_kind<2u;font_kind++)
  for(unsigned state=0;state<6u;state++) for(unsigned angle=0;angle<3u;angle++){
    GmlRender *r=calloc(1,sizeof *r);GmlWin content={0};
    FakeDevice fake={.on_demand=1};GmlRenderDeviceServices device=service(&fake);
    GmlRenderJournalMetrics metrics={0};uint32_t screen[W*H],shadow[W*H];
    CHECK(r && !gml_render_init(r,NULL));
    content.classic_version=generations[generation]>100?generations[generation]:0;
    content.bytecode=content.classic_version?0:generations[generation];
    content.has_room_layers=content.bytecode==17;r->win=&content;r->classic=content.classic_version!=0;
    const uint8_t rgba[16]={208,32,16,255,16,208,32,0,32,16,208,128,192,176,160,64};
    free(r->atlas[0].px);r->atlas[0].px=malloc(sizeof rgba);r->atlas[0].w=r->atlas[0].h=2;
    r->tpag=calloc(1,sizeof *r->tpag);r->spr=calloc(1,sizeof *r->spr);
    CHECK(r->atlas[0].px && r->tpag && r->spr);
    memcpy(r->atlas[0].px,rgba,sizeof rgba);r->n_tpag=r->n_spr=r->n_fonts=1;
    r->tpag[0]=(GmlTpag){.sw=2,.sh=2,.tw=2,.th=2,.bw=4,.bh=4,.tx=1,.ty=1};
    r->spr[0]=(GmlSprite){.w=4,.h=4,.n_frames=1,.frame=calloc(1,sizeof(int))};CHECK(r->spr[0].frame);
    GmlFont *font=&r->fonts[0];font->sprite=0;font->first='A';
    if(!font_kind){
      font->real=1;font->sprite=-1;font->atlas=0;font->line_height=font->align_height=2;
      font->glyphs=calloc(1,sizeof *font->glyphs);CHECK(font->glyphs);font->n_glyphs=1;
      font->glyphs[0]=(GmlGlyph){.w=2,.h=2,.shift=3,.ch='A'};
      for(unsigned cp=0;cp<256u;cp++) font->glyph_by_char[cp]=-1;
      font->glyph_by_char['A']=0;
    }
    r->font=0;device.observe_on_demand=fake_observe_on_demand;gml_render_bind_device(r,&device);
    if(target_kind==1u || target_kind==2u){
      CHECK(gml_render_application_surface_ensure_owned(r,W,H));
      CHECK(gml_render_begin_resident_owned_application(r,0,0,0));
    }else{
      gml_render_application_surface_bind(r,screen,W,H,1);
      CHECK(gml_render_begin_resident_base(r,screen,W,H,0,0));
    }
    if(target_kind==2u) CHECK(gml_surface_set_target(r,0));
    if(target_kind==3u){int id=gml_surface_create(r,W,H);CHECK(id>0 && gml_surface_set_target(r,id));}
    if(gui){gml_render_gui_begin(r,W,H);gml_render_gui_set_size(r,W/2,H/2);}
    r->interp=(int)interpolation;r->alphablend=!(state&1u);r->blendmode=(int)(state/2u);
    r->color_write_mask=(uint8_t)((state*3u+angle+target_kind)&15u);
    r->alpha_test_enable=(state+angle)&1u;r->alpha_test_ref=(uint8_t)(state*31u+angle*17u);
    gml_render_set_pending_fill(r,0x40273859u);gml_render_flush_pending_fill(r);
    CHECK(gml_render_scene_submit_pending(r));memcpy(shadow,r->fb,sizeof shadow);
    double mx=1,my=1;gml_render_draw_map_scale(r,&mx,&my);
    unsigned scale=(state+angle)%4u,a=(state+angle)%3u;
    if(anygm_policy_has_modern_layer_semantics(r->win) && r->alphablend &&
       r->blendmode==0 && !gml_render_target_preserves_alpha(r)){
      CHECK(!gml_render_resident_rotated_font_glyph(r,&r->spr[0],&r->tpag[0],
        12.125,10.25,scales[scale][0],scales[scale][1],rotations[angle],0x70B0D0u,alphas[a]));
      CHECK(r->resident_application_pass && !fake.readbacks && !fake.rgb_queries &&
            !memcmp(shadow,r->fb,sizeof shadow));
      gml_render_free(r);free(r);continue;
    }
    for(unsigned draw=0;draw<8u;draw++)
      gml_draw_text_transformed(r,12.125/mx,10.25/my,"A",scales[scale][0]/mx,
        scales[scale][1]/my,rotations[angle],0x70B0D0u,alphas[a]);
    if(!r->resident_application_pass) fprintf(stderr,"rotated text admission: generation=%d interp=%u target=%u gui=%u font=%u state=%u angle=%u\n",
      generations[generation],interpolation,target_kind,gui,font_kind,state,angle);
    CHECK(r->resident_application_pass && gml_render_scene_submit_pending(r) && !fake.readbacks && !fake.rgb_queries);
    GmlRenderResource *target=gml_render_scene_target_resource_ensure(r);CHECK(target);
    gml_render_resource_journal_metrics(target,&metrics);
    CHECK(metrics.authority==GML_RENDER_RESOURCE_GPU_RESIDENT && !metrics.cpu_replays &&
          !metrics.journal_bytes && !memcmp(shadow,r->fb,sizeof shadow));
    gml_render_free(r);free(r);
  }
  return 1;
}

static int resident_integer_background_function(void){
  return resident_background_function(0);
}

static int resident_fractional_background_function(void){
  return resident_background_function(1);
}

static int resident_background_stored_extents(void){
  return resident_background_function(3);
}

static int resident_nearest_tile_function(void){
  return resident_background_function(2);
}

/* Admission and authority only; the public paired generator owns RGBA. */
static int resident_nearest_crop_edges(unsigned port){
  enum { W=160,H=180,AW=264,AH=328 };
  const int generations[]={530,700,800,13,15,17};
  const double alphas[]={1,0,.37};
  for(unsigned generation=0;generation<6u;generation++)
  for(unsigned target_kind=0;target_kind<4u;target_kind++)
  for(unsigned gui=0;gui<2u;gui++) for(unsigned signs=0;signs<4u;signs++)
  for(unsigned state=0;state<4u;state++) for(unsigned disabled=0;disabled<2u;disabled++)
  for(unsigned a=0;a<3u;a++){
    GmlRender *r=calloc(1,sizeof *r);GmlWin content={0};
    FakeDevice fake={.on_demand=1,.readback_colour=0x7F25C080u};
    GmlRenderDeviceServices device=service(&fake);GmlRenderJournalMetrics metrics={0};
    uint32_t screen[W*H],shadow[W*H];const uint32_t *pixels=NULL;
    CHECK(r && !gml_render_init(r,NULL));
    content.classic_version=generations[generation]>100?generations[generation]:0;
    content.bytecode=content.classic_version?0:generations[generation];
    content.has_room_layers=content.bytecode==17;
    r->win=&content;r->classic=content.classic_version!=0;
    free(r->atlas[0].px);r->atlas[0].px=malloc(AW*AH*4u);
    r->tpag=calloc(1,sizeof *r->tpag);r->bg=calloc(1,sizeof *r->bg);
    CHECK(r->atlas[0].px && r->tpag && r->bg);
    r->atlas[0].w=AW;r->atlas[0].h=AH;r->n_tpag=r->n_bg=1;
    for(unsigned p=0;p<AW*AH;p++){
      r->atlas[0].px[p*4u]=(uint8_t)(p*31u+19u);
      r->atlas[0].px[p*4u+1u]=(uint8_t)(p*17u+53u);
      r->atlas[0].px[p*4u+2u]=(uint8_t)(p*13u+101u);
      r->atlas[0].px[p*4u+3u]=(uint8_t)((p*41u+7u)%255u);
    }
    r->tpag[0]=(GmlTpag){.sx=4,.sy=4,.sw=256,.sh=320,.tw=256,.th=320,.bw=256,.bh=320};
    if(port==3u){
      r->spr=calloc(1,sizeof *r->spr);CHECK(r->spr);
      r->n_spr=1;r->spr[0]=(GmlSprite){.w=256,.h=320,.n_frames=1};
      r->spr[0].frame=calloc(1,sizeof *r->spr[0].frame);CHECK(r->spr[0].frame);
    }
    device.observe_on_demand=fake_observe_on_demand;gml_render_bind_device(r,&device);
    for(unsigned p=0;p<W*H;p++) screen[p]=0xA5ABCDEFu;
    if(target_kind==1u || target_kind==2u){
      CHECK(gml_render_application_surface_ensure_owned(r,W,H));
      CHECK(gml_render_begin_resident_owned_application(r,0,0,0));
    }else{
      gml_render_application_surface_bind(r,screen,W,H,1);
      CHECK(gml_render_begin_resident_base(r,screen,W,H,0,0));
    }
    if(target_kind==2u) CHECK(gml_surface_set_target(r,0));
    if(target_kind==3u){int id=gml_surface_create(r,W,H);CHECK(id>0 && gml_surface_set_target(r,id));}
    if(gui){gml_render_gui_begin(r,W,H);gml_render_gui_set_size(r,W/2,H/2);}
    r->alphablend=!disabled;r->blendmode=state>=2u?2:0;
    r->alpha_test_enable=(int)(state&1u);r->alpha_test_ref=2u;
    gml_render_set_pending_fill(r,0xFF273859u);gml_render_flush_pending_fill(r);
    CHECK(gml_render_scene_submit_pending(r));memcpy(shadow,r->fb,sizeof shadow);
    int submitted=fake.submits;double mapping=gml_render_gui_transform_active(r)?2.0:1.0;
    for(unsigned draw=0;draw<8u;draw++){
      if(port==3u)
        gml_draw_sprite_ext(r,0,0,12.115987/mapping,8.3568773/mapping,
          (signs&1u?-.533926585:.533926585)/mapping,
          (signs&2u?-.533596838:.533596838)/mapping,0,0x70B0D0u,alphas[a]);
      else if(port==1u)
        gml_draw_background_tile(r,0,0,0,256,320,12.115987/mapping,8.3568773/mapping,
          .533926585/mapping,.533596838/mapping,(int)(signs&1u),(int)(signs>>1u),0,
          0x70B0D0u,alphas[a]);
      else if(port==2u)
        gml_draw_background_part_ext(r,0,0,0,256,320,12.115987/mapping,8.3568773/mapping,
          (signs&1u?-.533926585:.533926585)/mapping,
          (signs&2u?-.533596838:.533596838)/mapping,0x70B0D0u,alphas[a]);
      else
        gml_draw_background_tiled_ext(r,0,12.115987/mapping,8.3568773/mapping,
          .533926585/mapping,.533596838/mapping,0x70B0D0u,alphas[a],
          (int)(signs&1u),(int)(signs>>1u));
    }
    CHECK(r->resident_application_pass && gml_render_scene_submit_pending(r));
    CHECK(a==1u?fake.submits==submitted:fake.submits>submitted);
    CHECK(!fake.readbacks && !fake.rgb_queries && !memcmp(shadow,r->fb,sizeof shadow));
    GmlRenderResource *resource=gml_render_scene_target_resource_ensure(r);CHECK(resource);
    gml_render_resource_journal_metrics(resource,&metrics);
    CHECK(metrics.authority==GML_RENDER_RESOURCE_GPU_RESIDENT && !metrics.cpu_replays && !metrics.journal_bytes);
    CHECK(gml_render_resource_acquire_cpu_read(resource,&pixels) && fake.readbacks==1 &&
          pixels[0]==fake.readback_colour && pixels[W*H-1u]==fake.readback_colour);
    fake.forbid_calls=1;
    CHECK(gml_render_resource_acquire_cpu_read(resource,&pixels) && !fake.forbidden_calls);
    fake.forbid_calls=0;gml_render_free(r);free(r);
  }
  return 1;
}

static int resident_nearest_tile_crop_edges(void){
  return resident_nearest_crop_edges(1u);
}

static int resident_nearest_background_crop_edges(void){
  return resident_nearest_crop_edges(0u);
}

static int resident_selected_background_crop_edges(void){
  return resident_nearest_crop_edges(2u);
}

static int resident_nearest_sprite_crop_edges(void){
  return resident_nearest_crop_edges(3u);
}

static int resident_selected_background_function(void){
  enum { W=48,H=40 };
  static const int generations[]={530,700,800,13,15,17};
  static const double alphas[]={1.0,0.0,.37};
  static const uint8_t rgba[]={208,32,16,128,16,208,32,255,32,16,208,0,192,176,160,64};
  for(unsigned generation=0;generation<6u;generation++)
  for(unsigned target_kind=0;target_kind<4u;target_kind++)
  for(unsigned gui=0;gui<2u;gui++) for(unsigned draw_state=0;draw_state<4u;draw_state++)
  for(unsigned a=0;a<3u;a++) for(unsigned tint=0;tint<2u;tint++){
    AnygmContentFacts facts={.schema_version=ANYGM_COMPATIBILITY_SCHEMA};
    AnygmCompatibilityProfile profile={0};GmlWin content={0};
    if(generations[generation]>100) facts.classic_revision=generations[generation];
    else{facts.bytecode_revision=generations[generation];facts.has_room_layers=generations[generation]==17;}
    CHECK(anygm_compatibility_resolve(&facts,&profile,NULL,0));
    content.compatibility=&profile;content.classic_version=facts.classic_revision;
    content.bytecode=facts.bytecode_revision;content.has_room_layers=facts.has_room_layers;
    GmlRender *render=calloc(1,sizeof *render);
    FakeDevice fake={.on_demand=1,.readback_colour=0x80446688u};
    GmlRenderDeviceServices device=service(&fake);uint32_t screen[W*H],shadow[W*H];
    CHECK(render && gml_render_init(render,NULL)==0);render->win=&content;
    render->classic=facts.classic_revision!=0;
    render->atlas=calloc(1,sizeof *render->atlas);render->tpag=calloc(1,sizeof *render->tpag);
    render->bg=calloc(1,sizeof *render->bg);CHECK(render->atlas && render->tpag && render->bg);
    render->atlas[0].px=malloc(sizeof rgba);CHECK(render->atlas[0].px);
    memcpy(render->atlas[0].px,rgba,sizeof rgba);render->atlas[0].w=render->atlas[0].h=2;
    render->atlas[0].blob=1u;render->n_atlas=render->n_tpag=render->n_bg=1;
    render->tpag[0]=(GmlTpag){.sw=2,.sh=2,.tw=2,.th=2,.bw=4,.bh=4,.tx=1,.ty=1};
    device.observe_on_demand=fake_observe_on_demand;gml_render_bind_device(render,&device);
    for(unsigned p=0;p<W*H;p++) screen[p]=0xA5ABCDEFu;
    if(target_kind==1u || target_kind==2u){
      CHECK(gml_render_application_surface_ensure_owned(render,W,H));
      CHECK(gml_render_begin_resident_owned_application(render,0,0,0));
    }else{
      gml_render_application_surface_bind(render,screen,W,H,1);
      CHECK(gml_render_begin_resident_base(render,screen,W,H,0,0));
    }
    if(target_kind==2u) CHECK(gml_surface_set_target(render,0));
    if(target_kind==3u){int id=gml_surface_create(render,W,H);CHECK(id>0 && gml_surface_set_target(render,id));}
    if(gui){gml_render_gui_begin(render,W,H);gml_render_gui_set_size(render,W/2,H/2);}
    render->blendmode=draw_state>=2u?2:0;
    render->alpha_test_enable=(int)(draw_state&1u);render->alpha_test_ref=127u;
    gml_render_set_pending_fill(render,0x40273859u);gml_render_flush_pending_fill(render);
    CHECK(gml_render_scene_submit_pending(render));memcpy(shadow,render->fb,sizeof shadow);
    int submitted=fake.submits;double mapping=gui?2.0:1.0;
    for(unsigned draw=0;draw<8u;draw++)
      gml_draw_background_stretched(render,0,-3.0/mapping,1.0/mapping,
        60.0/mapping,33.75/mapping,tint?0x70B0D0u:0xFFFFFFu,alphas[a]);
    CHECK(render->resident_application_pass && gml_render_scene_submit_pending(render));
    CHECK(!fake.readbacks && !fake.rgb_queries && !memcmp(shadow,render->fb,sizeof shadow));
    CHECK(a==1u?fake.submits==submitted:fake.submits>submitted);
    GmlRenderResource *resource=gml_render_scene_target_resource_ensure(render);
    GmlRenderJournalMetrics metrics={0};const uint32_t *pixels=NULL;CHECK(resource);
    gml_render_resource_journal_metrics(resource,&metrics);
    CHECK(metrics.authority==GML_RENDER_RESOURCE_GPU_RESIDENT && !metrics.cpu_replays && !metrics.journal_bytes);
    CHECK(render->alpha_test_enable==(int)(draw_state&1u) && render->alpha_test_ref==127u);
    CHECK(gml_render_resource_acquire_cpu_read(resource,&pixels) && fake.readbacks==1 &&
      pixels[0]==fake.readback_colour && pixels[W*H-1u]==fake.readback_colour);
    fake.forbid_calls=1;
    CHECK(gml_render_resource_acquire_cpu_read(resource,&pixels) && !fake.forbidden_calls &&
      pixels[0]==fake.readback_colour && pixels[W*H-1u]==fake.readback_colour);
    fake.forbid_calls=0;gml_render_free(render);free(render);
  }
  return 1;
}

static int resident_integer_world_sourceover_pages(int packed_pages){
  enum { WIDTH=24,HEIGHT=24 };
  const double world[5][2]={{2,2},{4,4},{2,3},{0.5,0.5},{-2,-3}};
  const double alpha[3]={0.25,0.75,1.0};
  for(unsigned modern=0;modern<2u;modern++)
  for(unsigned stored=packed_pages?1u:0u;stored<(packed_pages?4u:1u);stored++)
  for(unsigned mapping=0;mapping<5u;mapping++)
  for(unsigned signs=0;signs<4u;signs++)
  for(unsigned source_alpha=0;source_alpha<3u;source_alpha++)
  for(unsigned opacity=0;opacity<3u;opacity++){
    GmlRender *render=(GmlRender*)calloc(1,sizeof *render);
    GmlWin content={.bytecode=modern?17:15};
    FakeDevice fake={.on_demand=1,.readback_colour=0x7F25C080u};
    GmlRenderDeviceServices device=service(&fake);
    GmlRenderApplicationWriteView view={0};
    GmlRenderJournalMetrics metrics={0};
    uint8_t rgba[16]={208,32,16,255,16,208,32,255,
                      32,16,208,255,192,176,160,255};
    double xs=mapping==3u?4.0:1.0,ys=xs,cam=0.5;
    CHECK(render && gml_render_application_surface_ensure_owned(render,WIDTH,HEIGHT));
    render->win=&content;
    render->atlas=(GmlAtlas*)calloc(1,sizeof *render->atlas);
    render->tpag=(GmlTpag*)calloc(1,sizeof *render->tpag);
    render->spr=(GmlSprite*)calloc(1,sizeof *render->spr);
    CHECK(render->atlas && render->tpag && render->spr);
    render->n_atlas=render->n_tpag=render->n_spr=1;
    render->atlas[0].px=(uint8_t*)malloc(sizeof rgba);
    render->spr[0].frame=(int*)calloc(1,sizeof(int));
    CHECK(render->atlas[0].px && render->spr[0].frame);
    if(source_alpha==1u) rgba[7]=0;
    if(source_alpha==2u){ rgba[3]=64;rgba[7]=128;rgba[11]=192; }
    memcpy(render->atlas[0].px,rgba,sizeof rgba);
    render->atlas[0].w=render->atlas[0].h=2;
    render->tpag[0].sw=render->tpag[0].sh=2;
    render->tpag[0].tw=stored==0u?2:stored==3u?4:1;
    render->tpag[0].th=stored==0u?2:stored==2u?3:stored==3u?4:1;
    render->tpag[0].bw=render->tpag[0].bh=4;
    render->tpag[0].tx=render->tpag[0].ty=1;
    render->spr[0].w=render->spr[0].h=4;
    render->spr[0].originx=render->spr[0].originy=2;
    render->spr[0].n_frames=1;
    render->active_shader=-1;
    render->color_write_mask=15u;
    device.observe_on_demand=fake_observe_on_demand;
    if(signs&1u) xs=-xs;
    if(signs&2u) ys=-ys;
    double physical_xs=xs*world[mapping][0],physical_ys=ys*world[mapping][1];
    double x=(6.0+physical_xs+(physical_xs<0.0?-2.0*physical_xs:0.0)+cam)/world[mapping][0];
    double y=(6.0+physical_ys+(physical_ys<0.0?-2.0*physical_ys:0.0)+cam)/world[mapping][1];
    gml_render_bind_device(render,&device);
    CHECK(gml_render_begin_resident_owned_application(render,cam,cam,0));
    render->world_transform_active=1;
    render->world_scale_x=world[mapping][0];render->world_scale_y=world[mapping][1];
    gml_render_set_pending_fill(render,modern?0xFF123456u:0x80123456u);
    if(stored){
      /* Only complete ordinary sprite dispatch bypasses stored-layout scaling. */
      for(unsigned family=GML_RENDER_SCENE_SPRITE;family<=GML_RENDER_SCENE_TEXT;family++)
        CHECK(!gml_render_resident_atlas_crop(render,0,0,0,2,2,6,6,
                  physical_xs,physical_ys,(GmlRenderSceneFamily)family));
      CHECK(render->resident_application_pass && fake.readbacks==0);
    }
    for(unsigned draw=0;draw<8u;draw++){
      gml_draw_sprite_ext(render,0,0,x+(draw%2u)/world[mapping][0],y,
                         xs,ys,0.0,0xFFFFFFu,alpha[opacity]);
      if(!render->resident_application_pass || fake.readbacks)
        fprintf(stderr,"integer sourceover admission: modern=%u mapping=%u signs=%u source=%u opacity=%u draw=%u resident=%d reads=%d\n",
                modern,mapping,signs,source_alpha,opacity,draw,
                render->resident_application_pass,fake.readbacks);
      CHECK(render->resident_application_pass && fake.readbacks==0);
    }
    CHECK(gml_render_scene_submit_pending(render) && fake.submits==2 &&
          fake.last_batch_operations==8u && fake.readbacks==0);
    gml_render_resource_journal_metrics(render->app_surface_resource,&metrics);
    if(metrics.authority!=GML_RENDER_RESOURCE_GPU_RESIDENT || metrics.cpu_replays ||
       metrics.journal_bytes || metrics.retained_mutable_bytes)
      fprintf(stderr,"integer sourceover authority: modern=%u mapping=%u signs=%u source=%u opacity=%u authority=%u replays=%llu journal=%zu retained=%zu\n",
              modern,mapping,signs,source_alpha,opacity,(unsigned)metrics.authority,
              (unsigned long long)metrics.cpu_replays,metrics.journal_bytes,metrics.retained_mutable_bytes);
    CHECK(metrics.authority==GML_RENDER_RESOURCE_GPU_RESIDENT &&
          metrics.cpu_replays==0 && metrics.journal_bytes==0 &&
          metrics.retained_mutable_bytes==0);
    CHECK(fake.readbacks==0);
    CHECK(render->tpag[0].tw==(stored==0u?2:stored==3u?4:1) &&
          render->tpag[0].th==(stored==0u?2:stored==2u?3:stored==3u?4:1) &&
          render->tpag[0].scene_opaque_checked);
    /* A real pixel observation, not each sprite, materialises the mocked device bytes. */
    CHECK(gml_render_application_surface_owned_view(render,&view));
    CHECK(fake.readbacks==1 && view.pixels[0]==fake.readback_colour &&
          view.pixels[WIDTH*HEIGHT-1u]==fake.readback_colour);
    gml_render_free(render);
    free(render);
  }
  return 1;
}

static int resident_identity_gui_unit_sourceover(void){
  for(unsigned modern=0;modern<2u;modern++)
  for(unsigned text=0;text<2u;text++)
  for(unsigned opacity=0;opacity<3u;opacity++){
    GmlRender *r=calloc(1,sizeof *r);
    GmlWin content={.bytecode=modern?17:15};
    FakeDevice fake={.on_demand=1,.readback_colour=0xFFABCDEFu};
    GmlRenderDeviceServices device=service(&fake);
    GmlRenderJournalMetrics metrics={0};
    uint32_t screen[64]={0};
    CHECK(r && gml_render_init(r,NULL)==0);
    r->win=&content;r->interp=0;
    free(r->atlas[0].px);
    r->atlas[0].w=r->atlas[0].h=2;
    r->atlas[0].px=malloc(16);
    CHECK(r->atlas[0].px);
    const uint8_t rgba[16]={208,32,16,0,16,208,32,1,
                            32,16,208,128,192,176,160,255};
    memcpy(r->atlas[0].px,rgba,sizeof rgba);
    r->tpag=calloc(1,sizeof *r->tpag);
    r->spr=calloc(1,sizeof *r->spr);
    CHECK(r->tpag && r->spr);
    r->n_tpag=r->n_spr=1;
    r->tpag[0]=(GmlTpag){.atlas=0,.sw=2,.sh=2,.tw=2,.th=2,.bw=2,.bh=2};
    r->spr[0].w=r->spr[0].h=2;r->spr[0].n_frames=1;
    r->spr[0].frame=calloc(1,sizeof(int));CHECK(r->spr[0].frame);
    device.observe_on_demand=fake_observe_on_demand;
    gml_render_bind_device(r,&device);
    CHECK(gml_render_begin_resident_base(r,screen,8,8,0,0));
    gml_render_gui_begin(r,8,8);
    gml_render_set_pending_fill(r,(opacity==0u?0u:opacity==1u?0x40000000u:0xFF000000u)|0x123456u);
    gml_render_flush_pending_fill(r);
    r->fb_opaque_known=r->fb_all_opaque=0;
    for(unsigned draw=0;draw<8u;draw++){
      if(text) CHECK(gml_render_resident_font_glyph(r,0,0,0,2,2,2,2,1,1,0xFFFFFFu,1));
      else gml_draw_sprite(r,0,0,2,2);
      CHECK(r->resident_application_pass && fake.readbacks==0);
    }
    CHECK(gml_render_scene_submit_pending(r) && fake.last_batch_operations==8u &&
          fake.last_filter==GML_RENDER_FILTER_NEAREST &&
          fake.last_quad_state.coverage_policy==GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA &&
          fake.last_filtered_draw_alpha==1.0 && fake.readbacks==0);
    gml_render_resource_journal_metrics(r->base_scene_resource,&metrics);
    CHECK(metrics.authority==GML_RENDER_RESOURCE_GPU_RESIDENT && !metrics.cpu_replays &&
          !metrics.journal_bytes && !metrics.retained_mutable_bytes);
    /* R03-TEXT-NEAREST-FUNCTION pairs the former modern tint, partial-alpha
     * and enlarged GUI admission fences without changing any pixel limit. */
    {
      const uint32_t tints[4]={0x123456u,0x53A2EDu,0xFE0107u,0x314F89u};
      for(unsigned tint=0;tint<4u;tint++)
        CHECK(gml_render_resident_font_glyph(r,0,0,0,2,2,2,2,1,1,tints[tint],1));
      CHECK(gml_render_scene_submit_pending(r) && fake.last_batch_operations==4u &&
            fake.last_quad_state.coverage_policy==GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA &&
            fake.last_filter==GML_RENDER_FILTER_NEAREST && fake.readbacks==0);
      fake.on_demand=0;
      CHECK(!gml_render_resident_font_glyph(r,0,0,0,2,2,2,2,1,1,0x123456u,1));
      fake.on_demand=1;
      r->fog_flat=1;
      CHECK(!gml_render_resident_font_glyph(r,0,0,0,2,2,2,2,1,1,0x123456u,1));
      r->fog_flat=0;
    }
    CHECK(gml_render_resident_font_glyph(r,0,0,0,2,2,2,2,1,1,0xFFFFFFu,0.5));
    CHECK(gml_render_resident_font_glyph(r,0,0,0,2,2,2,2,2,1,0xFFFFFFu,1));
    CHECK(gml_render_scene_submit_pending(r) && fake.last_batch_operations>=2u &&
          fake.last_filter==GML_RENDER_FILTER_NEAREST && !fake.readbacks && !fake.rgb_queries);
    r->gui_maximise_active=1;
    CHECK(!gml_render_resident_font_glyph(r,0,0,0,2,2,2,2,1,1,0xFFFFFFu,1));
    r->gui_maximise_active=0;
    CHECK(r->resident_application_pass && fake.readbacks==0);
    CHECK(gml_render_completed_screen_read(r,screen,8,8) && fake.readbacks==1 &&
          screen[0]==fake.readback_colour && screen[63]==fake.readback_colour);
    gml_render_free(r);free(r);
  }
  return 1;
}

static int resident_integer_world_sourceover_sprite(void){
  return resident_integer_world_sourceover_pages(0);
}

static int resident_firstgen_gui_half_point(void){
  const int extents[4][2]={{4,4},{5,3},{2,2},{1,1}};
  const double alpha[4]={0x1p-16,0.0137,0.75,1.0};
  for(unsigned excluded=0;excluded<10u;excluded++)
  for(unsigned size=0;size<4u;size++)
  for(unsigned opacity=0;opacity<4u;opacity++)
  for(unsigned placement=0;placement<3u;placement++){
    if(excluded && (size || opacity || placement)) continue;
    GmlRender *r=calloc(1,sizeof *r);
    GmlWin content={.bytecode=15};
    FakeDevice fake={.on_demand=1,.readback_colour=0xFFABCDEFu};
    GmlRenderDeviceServices device=service(&fake);
    GmlRenderJournalMetrics metrics={0};
    uint32_t screen[64]={0};
    int sw=extents[size][0],sh=extents[size][1];
    double left=placement==0u?2.49:placement==1u?-0.51:6.49;
    CHECK(r && gml_render_init(r,NULL)==0);
    r->win=&content;r->interp=0;
    free(r->atlas[0].px);
    r->atlas[0].px=malloc(8*6*4);CHECK(r->atlas[0].px);
    r->atlas[0].w=8;r->atlas[0].h=6;
    for(unsigned i=0;i<8*6u;i++){
      uint8_t *p=r->atlas[0].px+i*4;
      p[0]=208;p[1]=32;p[2]=16;p[3]=(i%3u)?128:0;
    }
    r->tpag=calloc(1,sizeof *r->tpag);r->spr=calloc(1,sizeof *r->spr);
    CHECK(r->tpag && r->spr);r->n_tpag=r->n_spr=1;
    r->tpag[0]=(GmlTpag){.atlas=0,.sx=8-sw,.sy=6-sh,.sw=sw,.sh=sh,
      .tw=sw,.th=sh,.tx=1,.ty=1,.bw=sw+2,.bh=sh+2};
    r->spr[0].w=sw+2;r->spr[0].h=sh+2;r->spr[0].n_frames=1;
    r->spr[0].originx=r->spr[0].originy=2;
    r->spr[0].frame=calloc(1,sizeof(int));CHECK(r->spr[0].frame);
    device.observe_on_demand=fake_observe_on_demand;
    gml_render_bind_device(r,&device);
    CHECK(gml_render_begin_resident_base(r,screen,8,8,0,0));
    gml_render_gui_begin(r,8,8);gml_render_set_pending_fill(r,0x40123456u);
    gml_render_flush_pending_fill(r);r->fb_opaque_known=r->fb_all_opaque=0;
    if(excluded==1u) fake.on_demand=0;
    if(excluded==2u) r->fog_flat=1;
    if(excluded==3u) r->tpag[0].project_authored_edges=1;
    if(excluded==4u) r->tpag[0].tw++;
    if(excluded==5u) r->world_transform_active=1;
    if(excluded==9u) r->active_shader=0;
    for(unsigned draw=0;draw<8u;draw++){
      gml_draw_sprite_ext(r,0,0,left+0.5,2.49+0.5,0.5,excluded==7u?0.6:0.5,0,
          excluded==8u?0x123456u:0xFFFFFFu,excluded==6u?0x1p-65:alpha[opacity]);
      if(!excluded) CHECK(r->resident_application_pass && !fake.readbacks);
    }
    if(excluded==2u || excluded==4u || excluded==5u || excluded==7u || excluded==8u){
      CHECK(nearest_sprite_extension_transport(r,&fake,1));
      if(excluded==2u) CHECK(fake.last_quantisation==GML_RENDER_QUANTISE_INTEGER_FLAT_RGB);
      /* A corrected nearest boundary can split a public draw into strips;
       * all eight compatible calls must still fit one pending submission. */
      if(fake.last_batch_operations!=8u)
        fprintf(stderr,"nearest GUI extension: case=%u packets=%u submits=%d\n",
                excluded,fake.last_batch_operations,fake.submits);
      CHECK(fake.last_batch_operations>=8u);
      for(unsigned pixel=0;pixel<64u;pixel++) CHECK(screen[pixel]==0u);
      gml_render_free(r);free(r);continue;
    }
    if(excluded){
      CHECK(!r->resident_application_pass && fake.submits==1 &&
        fake.last_opcode==GML_RENDER_OP_CLEAR && !r->pending_scene_batch);
      gml_render_free(r);free(r);continue;
    }
    CHECK(gml_render_scene_submit_pending(r) && !fake.readbacks);
    if(size<3u && floor(left+0.5)+sw/2>0.0){
      CHECK(fake.last_batch_operations==8u &&
        fake.last_quad_state.coverage_policy==GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA &&
        fake.last_filter==GML_RENDER_FILTER_NEAREST && !fake.last_quantisation &&
        fake.last_filtered_draw_alpha==alpha[opacity] &&
        fake.last_quad.x[0]==floor(left+0.5) && fake.last_quad.y[0]==2.0 &&
        fake.last_quad.x[1]-fake.last_quad.x[0]==sw/2 &&
        fake.last_quad.y[2]-fake.last_quad.y[0]==sh/2 &&
        fake.last_quad.sampling.source_region.width==(unsigned)(sw/2)*2u &&
        fake.last_quad.sampling.source_region.height==(unsigned)(sh/2)*2u);
      gml_render_resource_journal_metrics(r->base_scene_resource,&metrics);
      CHECK(metrics.authority==GML_RENDER_RESOURCE_GPU_RESIDENT && !metrics.cpu_replays &&
        !metrics.journal_bytes && !metrics.retained_mutable_bytes);
    } else CHECK(fake.submits==1 && !r->pending_scene_batch);
    gml_render_free(r);free(r);
  }
  return 1;
}

static int resident_integer_world_packed_sprite(void){
  return resident_integer_world_sourceover_pages(1);
}

static int resident_integer_sourceover_exclusions(void){
  for(unsigned packed=0;packed<2u;packed++)
  for(unsigned variant=0;variant<6u;variant++){
    GmlRender *render=(GmlRender*)calloc(1,sizeof *render);
    GmlWin content={.bytecode=variant==5u?17:15};
    FakeDevice fake={.on_demand=1,.readback_colour=0x80123456u};
    GmlRenderDeviceServices device=service(&fake);
    const uint8_t rgba[16]={208,32,16,128,16,208,32,255,
                           32,16,208,64,192,176,160,192};
    CHECK(render && gml_render_application_surface_ensure_owned(render,24,24));
    render->win=&content;
    render->atlas=(GmlAtlas*)calloc(1,sizeof *render->atlas);
    render->tpag=(GmlTpag*)calloc(1,sizeof *render->tpag);
    render->spr=(GmlSprite*)calloc(1,sizeof *render->spr);
    CHECK(render->atlas && render->tpag && render->spr);
    render->n_atlas=render->n_tpag=render->n_spr=1;
    render->atlas[0].px=(uint8_t*)malloc(sizeof rgba);
    render->spr[0].frame=(int*)calloc(1,sizeof(int));
    CHECK(render->atlas[0].px && render->spr[0].frame);
    memcpy(render->atlas[0].px,rgba,sizeof rgba);
    render->atlas[0].w=render->atlas[0].h=2;
    render->tpag[0].sw=render->tpag[0].sh=2;
    render->tpag[0].tw=render->tpag[0].th=packed?1:2;
    render->tpag[0].bw=render->tpag[0].bh=2;
    render->spr[0].w=render->spr[0].h=2;render->spr[0].n_frames=1;
    render->active_shader=-1;render->color_write_mask=15u;
    device.observe_on_demand=variant==0u?NULL:fake_observe_on_demand;
    gml_render_bind_device(render,&device);
    CHECK(gml_render_begin_resident_owned_application(render,0,0,0));
    render->world_transform_active=1;render->world_scale_x=render->world_scale_y=2;
    gml_render_set_pending_fill(render,0x80123456u);
    if(variant==1u) render->alpha_test_enable=1;
    if(variant==2u) render->color_write_mask=7u;
    double x=variant==3u?3.125:variant==4u?-0.5:3.0;
    uint32_t shadow[24*24];
    memcpy(shadow,render->fb,sizeof shadow);
    gml_draw_sprite_ext(render,0,0,x,3.0,1,1,0,0xFFFFFFu,0.75);
    if(variant==2u){
      CHECK(nearest_sprite_extension_transport(render,&fake,1));
      CHECK(!memcmp(shadow,render->fb,sizeof shadow));
      gml_render_free(render);free(render);continue;
    }
    if(variant==1u || variant==3u || variant==4u){
      /* Clipped and fractional whole-scale source-over share the paired rule. */
      CHECK(render->resident_application_pass && render->pending_scene_batch &&
            gml_render_scene_submit_pending(render) && fake.submits==2 &&
            fake.last_batch_operations==(variant==3u?2u:1u) && !fake.readbacks && !fake.rgb_queries);
      CHECK(
            fake.last_filter==GML_RENDER_FILTER_NEAREST &&
            fake.last_quantisation==GML_RENDER_QUANTISE_INTEGER_TINT_TRUNC);
      if(variant==1u) CHECK(fake.last_quad_state.alpha_test_enabled==1u &&
                           fake.last_quad_state.alpha_test_reference==0u);
      gml_render_free(render);free(render);
      continue;
    }
    if(variant==5u){
      /* The preferred crop may reject unknown-alpha modern SCREEN. Its new
       * whole-texel extension precedes the separately qualified point rule. */
      CHECK(render->resident_application_pass && render->pending_scene_batch &&
            gml_render_scene_submit_pending(render) && fake.submits==2 &&
            fake.last_batch_operations==1u && !fake.readbacks && !fake.rgb_queries &&
            fake.last_filter==GML_RENDER_FILTER_NEAREST &&
            fake.last_quantisation==GML_RENDER_QUANTISE_INTEGER_TINT_ROUND &&
            fake.last_quad_state.rgb_equation==GML_RENDER_BLEND_ADD &&
            fake.last_quad_state.coverage_policy==GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA);
      gml_render_free(render);free(render);
      continue;
    }
    if(render->resident_application_pass || render->pending_scene_batch ||
       fake.readbacks!=(variant==0u?0:1) || fake.submits!=1)
      fprintf(stderr,"integer sourceover exclusion: variant=%u resident=%d pending=%d reads=%d submits=%d\n",
              variant,render->resident_application_pass,render->pending_scene_batch!=NULL,
              fake.readbacks,fake.submits);
    CHECK(!render->resident_application_pass && !render->pending_scene_batch &&
          fake.readbacks==(variant==0u?0:1) && fake.submits==1);
    gml_render_free(render);free(render);
  }
  return 1;
}

static int state_without_context(void){
  GmlRender *original=(GmlRender*)calloc(1,sizeof *original);
  GmlRender *restored=(GmlRender*)calloc(1,sizeof *restored);
  GmlRenderDrawOp operation;
  GmlRenderDrawBatch batch;
  GmlRenderDeviceServices device;
  FakeDevice fake={0};
  GmlRenderImageRef ref;
  uint8_t *bytes;
  size_t size,written=0,used=0;
  const uint32_t *pixels;
  int id,w=0,h=0;
  CHECK(original && restored);
  original->next_surface_id=1;
  id=gml_surface_create(original,4,4);
  CHECK(id>0);
  device=service(&fake);
  gml_render_bind_device(original,&device);
  ref=gml_render_resource_image_ref(gml_render_surface_resource_ensure(original,id));
  CHECK(ref.incarnation);
  operation=clear_op(2u,0x4088AABBu,GML_RENDER_REPLAY_EXACT);
  operation.target.image=ref;
  operation.target.image.content_version=2u;
  operation.target.pixel_format=GML_RENDER_PIXEL_ARGB8888;
  batch=(GmlRenderDrawBatch){&operation,1u,NULL,0u};
  CHECK(gml_render_surface_scene_submit(original,id,&batch,NULL,0u)==
        GML_RENDER_RESOURCE_EXEC_DEVICE);
  fake.forbid_calls=1;
  size=gml_render_state_size(original,0);
  CHECK(size>0 && fake.readbacks==0);
  bytes=(uint8_t*)malloc(size);
  CHECK(bytes);
  CHECK(gml_render_state_save(original,0,bytes,size,&written) && written==size);
  CHECK(gml_render_state_load(restored,bytes,size,&used) && used==size);
  pixels=gml_surface_pixels_read(restored,id,&w,&h);
  CHECK(pixels && w==4 && h==4);
  CHECK(pixels[0]==0x4088AABBu && pixels[15]==0x4088AABBu);
  CHECK(!restored->surface[id-1].resource && fake.readbacks==0 &&
        fake.forbidden_calls==0);
  fake.forbid_calls=0;
  gml_render_bind_device(original,NULL);
  free(bytes);
  gml_surface_free(original,id);
  gml_surface_free(restored,id);
  free(original);
  free(restored);
  return 1;
}

static int resident_authority_avoids_replay_retention(void){
  uint32_t pixels[16],source_pixels[4]={0x80FF0000u,0x40FF0000u,
                                       0xFFFF0000u,0x00FF0000u};
  GmlRenderImageRef ref=source_ref();
  GmlRenderDrawOp operation=quad_op(2u,&ref);
  GmlRenderDrawStateSnapshot state=quad_state(1);
  GmlRenderDrawBatch batch={&operation,1u,&state,1u};
  GmlRenderResourceSource source=source_view(source_pixels,0);
  GmlRenderJournalMetrics metrics={0};
  FakeDevice fake={0};
  GmlRenderDeviceServices device=service(&fake);
  const uint32_t *observed=NULL;
  GmlRenderResource *resource;
  fake.on_demand=1;
  device.observe_on_demand=fake_observe_on_demand;
  fill(pixels,0xFF0000FFu);
  /* This budget cannot retain even one operation or source. It must not force a
   * CPU replay, consolidation or read in the witnessed resident path. */
  resource=gml_render_resource_create(target_ref(1u,1u),4u,4u,
    GML_RENDER_PIXEL_XRGB8888,pixels,(GmlRenderResourceLimits){1u,1u,0u});
  CHECK(resource);
  for(unsigned draw=0;draw<64u;draw++){
    operation.target.image=gml_render_resource_image_ref(resource);
    operation.target.image.content_version++;
    CHECK(gml_render_resource_submit(resource,&device,&batch,&source,1u)==
          GML_RENDER_RESOURCE_EXEC_DEVICE);
    source_pixels[0]++;ref.content_version++;source.image=ref;
    gml_render_resource_journal_metrics(resource,&metrics);
    CHECK(metrics.authority==GML_RENDER_RESOURCE_GPU_RESIDENT &&
          metrics.logical_version==draw+2u && metrics.cpu_version==1u &&
          metrics.gpu_version==metrics.logical_version && !metrics.cpu_replays &&
          !metrics.device_checkpoints && !metrics.consolidations && !metrics.cpu_fallbacks &&
          !metrics.journal_operations && !metrics.retained_versions && !metrics.journal_bytes &&
          !metrics.retained_mutable_bytes && !metrics.cached_replay_bytes &&
          !metrics.checkpoint_allocations && !fake.readbacks && pixels[5]==0xFF0000FFu);
  }
  fake.readback_colour=0xFF456789u;
  CHECK(gml_render_resource_acquire_cpu_read(resource,&observed) &&
        observed==pixels && pixels[0]==fake.readback_colour &&
        pixels[15]==fake.readback_colour && fake.readbacks==1);
  gml_render_resource_journal_metrics(resource,&metrics);
  CHECK(metrics.authority==GML_RENDER_RESOURCE_BOTH_CURRENT &&
        metrics.cpu_version==65u && !metrics.cpu_replays && metrics.device_checkpoints==1u);
  gml_render_resource_context_lost(resource);
  CHECK(gml_render_resource_acquire_cpu_read(resource,&observed) && fake.readbacks==1);
  gml_render_resource_destroy(resource);
  /* An absent or negative proof retains the established journal and source-pin contract. */
  fake.on_demand=0;
  operation=quad_op(2u,&ref);
  resource=gml_render_resource_create(target_ref(1u,1u),4u,4u,
    GML_RENDER_PIXEL_XRGB8888,pixels,(GmlRenderResourceLimits){0});
  CHECK(resource && gml_render_resource_submit(resource,&device,&batch,&source,1u)==
        GML_RENDER_RESOURCE_EXEC_DEVICE);
  gml_render_resource_journal_metrics(resource,&metrics);
  CHECK(metrics.authority==GML_RENDER_RESOURCE_GPU_REPLAYABLE &&
        metrics.journal_operations==1u && metrics.retained_versions==1u &&
        metrics.retained_mutable_bytes==sizeof source_pixels && fake.readbacks==1);
  gml_render_resource_destroy(resource);
  return 1;
}

static int resident_observation_retry_and_fallback(void){
  uint32_t pixels[16],prior[16];
  GmlRenderDrawOp operation=clear_op(2u,0xFF112233u,GML_RENDER_REPLAY_EXACT);
  GmlRenderDrawBatch batch={&operation,1u,NULL,0u};
  GmlRenderJournalMetrics metrics={0};
  FakeDevice fake={0};
  GmlRenderDeviceServices device=service(&fake);
  const uint32_t *observed=(const uint32_t*)1;
  uint32_t *writable=(uint32_t*)1;
  GmlRenderResource *resource;
  fake.on_demand=1;
  device.observe_on_demand=fake_observe_on_demand;
  fill(pixels,0xFFABCDEFu);memcpy(prior,pixels,sizeof prior);
  resource=gml_render_resource_create(target_ref(1u,1u),4u,4u,
    GML_RENDER_PIXEL_XRGB8888,pixels,(GmlRenderResourceLimits){0});
  CHECK(resource && gml_render_resource_submit_deferred(resource,&device,&batch,NULL,0u)==
        GML_RENDER_RESOURCE_EXEC_DEVICE);
  fake.readback_colour=0xFF56789Au;fake.fail_partial_readback=1;
  CHECK(!gml_render_resource_acquire_cpu_write(resource,&writable) && !writable &&
        !memcmp(pixels,prior,sizeof prior));
  gml_render_resource_journal_metrics(resource,&metrics);
  CHECK(metrics.authority==GML_RENDER_RESOURCE_GPU_RESIDENT && metrics.cpu_version==1u &&
        metrics.logical_version==2u && !metrics.cpu_replays && !metrics.device_checkpoints);
  fake.fail_partial_readback=0;
  CHECK(gml_render_resource_acquire_cpu_read(resource,&observed) && observed==pixels &&
        pixels[0]==fake.readback_colour && pixels[15]==fake.readback_colour);
  operation.target.image.content_version=3u;
  fake.status=GML_RENDER_EXEC_UNSUPPORTED;fake.prewrite_refusal=1;
  CHECK(gml_render_resource_submit_deferred(resource,&device,&batch,NULL,0u)==
        GML_RENDER_RESOURCE_EXEC_CPU && pixels[0]==0xFF112233u);
  fake.status=0;operation.target.image.content_version=4u;
  CHECK(gml_render_resource_submit_deferred(resource,&device,&batch,NULL,0u)==
        GML_RENDER_RESOURCE_EXEC_DEVICE);
  operation=flat_rect_op(5u,(GmlRenderRect){1,1,2u,2u},0xFF2468ACu);
  fake.status=GML_RENDER_EXEC_UNSUPPORTED;
  CHECK(gml_render_resource_submit_deferred(resource,&device,&batch,NULL,0u)==
        GML_RENDER_RESOURCE_EXEC_CPU && pixels[0]==fake.readback_colour &&
        pixels[5]==0xFF2468ACu && pixels[15]==fake.readback_colour);
  gml_render_resource_journal_metrics(resource,&metrics);
  CHECK(metrics.cpu_version==5u && metrics.cpu_fallbacks==2u && !metrics.cpu_replays &&
        metrics.device_checkpoints==2u && fake.readbacks==3);
  fake.status=0;operation=clear_op(6u,0xFF112233u,GML_RENDER_REPLAY_EXACT);
  CHECK(gml_render_resource_submit_deferred(resource,&device,&batch,NULL,0u)==
        GML_RENDER_RESOURCE_EXEC_DEVICE);
  memcpy(prior,pixels,sizeof prior);
  operation.target.image.content_version=7u;
  fake.status=GML_RENDER_EXEC_RECOVERABLE_ERROR;fake.prewrite_refusal=0;
  CHECK(gml_render_resource_submit_deferred(resource,&device,&batch,NULL,0u)==
        GML_RENDER_RESOURCE_EXEC_FAILED && !memcmp(pixels,prior,sizeof prior));
  observed=(const uint32_t*)1;
  CHECK(!gml_render_resource_acquire_cpu_read(resource,&observed) && !observed);
  gml_render_resource_journal_metrics(resource,&metrics);
  CHECK(metrics.authority==GML_RENDER_RESOURCE_INVALID && metrics.cpu_fallbacks==2u &&
        metrics.logical_version==6u && fake.readbacks==3);
  gml_render_resource_destroy(resource);
  return 1;
}

static int resident_source_copy_and_loss(void){
  uint32_t source_pixels[16],pixels[16];
  GmlRenderDrawOp operation=clear_op(2u,0xFF112233u,GML_RENDER_REPLAY_EXACT);
  GmlRenderDrawBatch batch={&operation,1u,NULL,0u};
  GmlRenderResourceSource source={0};
  GmlRenderDrawStateSnapshot state={0};
  GmlRenderImageRef ref={0};
  GmlRenderJournalMetrics metrics={0};
  FakeDevice fake={0};
  GmlRenderDeviceServices device=service(&fake);
  GmlRenderResource *source_resource,*resource;
  const uint32_t *observed=(const uint32_t*)1;
  fake.on_demand=1;device.observe_on_demand=fake_observe_on_demand;
  fill(source_pixels,0xFF000000u);fill(pixels,0xFFABCDEFu);
  source_resource=gml_render_resource_create(target_ref(1u,1u),4u,4u,
    GML_RENDER_PIXEL_XRGB8888,source_pixels,(GmlRenderResourceLimits){0});
  CHECK(source_resource &&
        gml_render_resource_submit_deferred(source_resource,&device,&batch,NULL,0u)==
        GML_RENDER_RESOURCE_EXEC_DEVICE &&
        gml_render_resource_device_source_view(source_resource,&device,&source) && !source.bytes);
  ref=source.image;
  GmlRenderImageRef destination=target_ref(1u,1u);destination.identity=2u;
  resource=gml_render_resource_create(destination,4u,4u,
    GML_RENDER_PIXEL_XRGB8888,pixels,(GmlRenderResourceLimits){0});
  CHECK(resource);
  operation.opcode=GML_RENDER_OP_COPY_IMAGE;
  operation.target.image=destination;operation.target.image.content_version++;
  operation.source_count=1u;operation.sources=&ref;
  operation.payload.copy=(GmlRenderCopyRegion){
    {0,0,4u,4u},{0,0,4u,4u},0u};
  batch.states=&state;batch.state_count=1u;
  unsigned uploads=(unsigned)fake.uploads;
  CHECK(gml_render_resource_submit_deferred(resource,&device,&batch,&source,1u)==
        GML_RENDER_RESOURCE_EXEC_DEVICE && fake.readbacks==0 &&
        (unsigned)fake.uploads==uploads+1u);
  gml_render_resource_journal_metrics(resource,&metrics);
  CHECK(metrics.authority==GML_RENDER_RESOURCE_GPU_RESIDENT && !metrics.journal_operations &&
        !metrics.journal_bytes && !metrics.retained_versions && !metrics.cached_replay_bytes);
  /* A real loss cannot reconstruct either device-only image or issue a last read. */
  fake.forbid_calls=1;
  gml_render_resource_context_lost(source_resource);
  gml_render_resource_context_lost(resource);
  CHECK(!gml_render_resource_acquire_cpu_read(resource,&observed) && !observed &&
        !gml_render_resource_device_source_view(source_resource,&device,&source) &&
        !fake.forbidden_calls && !fake.readbacks && pixels[0]==0xFFABCDEFu);
  gml_render_resource_destroy(resource);gml_render_resource_destroy(source_resource);
  return 1;
}

static int device_observation_preserves_actual_replayable_pixels(void){
  uint32_t pixels[16],before[16];
  FakeDevice fake={0},foreign={0};
  GmlRenderDeviceServices device=service(&fake),other=service(&foreign);
  GmlRenderDrawOp operation=clear_op(2u,0x40556677u,GML_RENDER_REPLAY_EXACT);
  GmlRenderDrawBatch batch={&operation,1u,NULL,0u};
  GmlRenderJournalMetrics original={0},observed={0};
  GmlRenderResource *resource;
  const uint32_t *current=NULL;
  fill(pixels,0x80112233u);
  memcpy(before,pixels,sizeof before);
  resource=gml_render_resource_create(target_ref(1u,1u),4u,4u,
    GML_RENDER_PIXEL_ARGB8888,pixels,(GmlRenderResourceLimits){0});
  CHECK(resource);
  operation.target.pixel_format=GML_RENDER_PIXEL_ARGB8888;
  /* A synthetic device response deliberately differs from its replay recipe.
   * This tests the observation contract, not the fidelity of a real clear. */
  fake.readback_colour=0x41566778u;
  CHECK(gml_render_resource_submit_deferred(resource,&device,&batch,NULL,0u)==
        GML_RENDER_RESOURCE_EXEC_DEVICE && !fake.readbacks);
  gml_render_resource_journal_metrics(resource,&original);
  CHECK(original.authority==GML_RENDER_RESOURCE_GPU_REPLAYABLE &&
        original.journal_operations==1u && original.cpu_version==1u);
  fake.forbid_calls=foreign.forbid_calls=1;
  CHECK(!gml_render_resource_observe_device(resource,NULL) &&
        !gml_render_resource_observe_device(resource,&other));
  gml_render_resource_journal_metrics(resource,&observed);
  CHECK(!memcmp(&original,&observed,sizeof original) &&
        !memcmp(pixels,before,sizeof before) &&
        !fake.forbidden_calls && !foreign.forbidden_calls);
  fake.forbid_calls=0;
  fake.fail_partial_readback=1;
  CHECK(!gml_render_resource_observe_device(resource,&device) && fake.readbacks==1);
  gml_render_resource_journal_metrics(resource,&observed);
  CHECK(observed.authority==original.authority &&
        observed.cpu_version==original.cpu_version &&
        observed.logical_version==original.logical_version &&
        observed.journal_operations==original.journal_operations &&
        !memcmp(pixels,before,sizeof before));
  fake.fail_partial_readback=0;
  CHECK(gml_render_resource_observe_device(resource,&device) && fake.readbacks==2);
  gml_render_resource_journal_metrics(resource,&observed);
  CHECK(observed.cpu_version==2u && observed.gpu_version==2u &&
        observed.authority==GML_RENDER_RESOURCE_BOTH_CURRENT &&
        observed.cpu_replays==0u && observed.device_checkpoints==1u &&
        !observed.journal_operations);
  for(unsigned index=0;index<16u;index++) CHECK(pixels[index]==fake.readback_colour);
  fake.forbid_calls=1;
  CHECK(gml_render_resource_observe_device(resource,NULL) &&
        gml_render_resource_acquire_cpu_read(resource,&current) && current==pixels &&
        !fake.forbidden_calls && fake.readbacks==2);
  gml_render_resource_context_lost(resource);
  CHECK(gml_render_resource_acquire_cpu_read(resource,&current) &&
        pixels[0]==fake.readback_colour && pixels[15]==fake.readback_colour &&
        !fake.forbidden_calls);
  gml_render_resource_destroy(resource);
  return 1;
}

static int metadata_state_capacity_preserves_pending_pixels(void){
  GmlRender *original=(GmlRender*)calloc(1,sizeof *original);
  GmlRender *restored=(GmlRender*)calloc(1,sizeof *restored);
  FakeDevice fake={0};
  GmlRenderDeviceServices device=service(&fake);
  GmlRenderResource *resource;
  GmlRenderJournalMetrics before={0},after={0};
  GmlRenderDrawOp operation;
  GmlRenderDrawBatch batch;
  uint8_t *state=NULL,*again=NULL;
  size_t bound=0,derived_bound=0,written=0,used=0,again_written=0;
  int id;
  CHECK(original && restored);
  original->next_surface_id=1;
  original->circle_precision=4;
  id=gml_surface_create(original,4,4);
  CHECK(id>0);
  gml_render_bind_device(original,&device);
  resource=gml_render_surface_resource_ensure(original,id);
  CHECK(resource);
  operation=clear_op(2u,0xFF010203u,GML_RENDER_DEVICE_CHECKPOINT);
  operation.target.image=gml_render_resource_image_ref(resource);
  operation.target.image.content_version=2u;
  operation.target.pixel_format=GML_RENDER_PIXEL_ARGB8888;
  batch=(GmlRenderDrawBatch){&operation,1u,NULL,0u};
  fake.readback_colour=0x40335577u;
  CHECK(gml_render_resource_submit_deferred(resource,&device,&batch,NULL,0u)==
        GML_RENDER_RESOURCE_EXEC_DEVICE && !fake.readbacks);
  gml_render_resource_journal_metrics(resource,&before);
  CHECK(before.authority==GML_RENDER_RESOURCE_GPU_PENDING_CHECKPOINT &&
        before.cpu_version!=before.logical_version && !original->surface[id-1].rle);
  fake.forbid_calls=1;
  bound=gml_render_state_capacity(original,0);
  derived_bound=gml_render_state_capacity(original,id);
  CHECK(bound>derived_bound && bound-derived_bound==16u*8u-8u &&
        gml_render_state_capacity(original,0)==bound);
  gml_render_resource_journal_metrics(resource,&after);
  CHECK(!fake.forbidden_calls && !fake.readbacks && !original->surface[id-1].rle &&
        original->surface[id-1].px[0]==0u && original->surface[id-1].px[15]==0u &&
        !memcmp(&before,&after,sizeof before));
  fake.forbid_calls=0;
  state=(uint8_t*)malloc(bound);
  again=(uint8_t*)malloc(bound);
  CHECK(state && again);
  CHECK(gml_render_state_save(original,0,state,bound,&written) &&
        written>0u && written<=bound && fake.readbacks==1);
  CHECK(gml_render_state_load(restored,state,written,&used) && used==written &&
        restored->surface[id-1].px[0]==fake.readback_colour &&
        restored->surface[id-1].px[15]==fake.readback_colour);
  CHECK(gml_render_state_capacity(original,0)==bound);
  CHECK(gml_render_state_capacity(restored,0)==bound);
  CHECK(gml_render_state_save(restored,0,again,bound,&again_written));
  CHECK(again_written==written);
  if(memcmp(state,again,written))
    for(size_t index=0;index<written;index++) if(state[index]!=again[index]){
      fprintf(stderr,"pending metadata state first difference at %zu: %u/%u\n",
              index,state[index],again[index]);
      break;
    }
  CHECK(!memcmp(state,again,written));
  free(state); free(again);
  gml_render_free(original); gml_render_free(restored);
  free(original); free(restored);
  return 1;
}

static int metadata_state_capacity_skips_pixel_compression(void){
  for(int detailed=0;detailed<2;detailed++){
    GmlRender *render=(GmlRender*)calloc(1,sizeof *render);
    uint8_t *rgba=(uint8_t*)calloc(1,32u*32u*4u),*state=NULL;
    size_t bound=0,written=0;
    int id,sprite;
    CHECK(render && rgba);
    render->next_surface_id=1;
    render->font=-1;
    id=gml_surface_create(render,4,4);
    CHECK(id>0);
    for(unsigned index=0;index<16u;index++)
      render->surface[id-1].px[index]=detailed?0xFF000000u+index:0u;
    sprite=gml_sprite_append_from_rgba_frames(render,rgba,32,32,1,0,0,NULL);
    CHECK(sprite==0 && !render->spr[sprite].runtime_state_cached &&
          !render->surface[id-1].rle);
    bound=gml_render_state_capacity(render,0);
    CHECK(bound>0u && gml_render_state_capacity(render,0)==bound &&
          !render->spr[sprite].runtime_state_cached &&
          !render->spr[sprite].runtime_state_data && !render->surface[id-1].rle);
    state=(uint8_t*)malloc(bound);
    CHECK(state && gml_render_state_save(render,0,state,bound,&written) &&
          written<=bound && render->spr[sprite].runtime_state_cached &&
          render->spr[sprite].runtime_state_data && render->surface[id-1].rle &&
          gml_render_state_capacity(render,0)==bound &&
          render->surface[id-1].rle_len==(detailed?132u:12u));
    free(state);
    gml_render_free(render); free(render);
  }
  return 1;
}

static int zero_base_runtime_sprite_state(void){
  GmlRender *render=(GmlRender*)calloc(1,sizeof *render);
  uint8_t *rgba=(uint8_t*)malloc(16u);
  uint8_t expected[16];
  uint8_t *bytes=NULL;
  size_t size=0,written=0,used=0;
  int sprite;
  CHECK(render && rgba);
  render->next_surface_id=1;
  render->font=-1;
  render->base_n_spr=0;
  for(size_t i=0;i<16u;i++) expected[i]=rgba[i]=(uint8_t)(i*7u);
  sprite=gml_sprite_append_from_rgba_frames(render,rgba,2,2,1,0,0,NULL);
  CHECK(sprite==0 && render->n_spr==1 && render->spr[0].runtime_extra);
  size=gml_render_state_size(render,0);
  CHECK(size>0);
  bytes=(uint8_t*)malloc(size);
  CHECK(bytes && gml_render_state_save(render,0,bytes,size,&written) && written==size);
  CHECK(gml_render_state_load(render,bytes,size,&used) && used==size &&
        render->n_spr==1 && render->spr[0].runtime_rgba &&
        !memcmp(render->spr[0].runtime_rgba,expected,16u));
  gml_sprite_delete(render,sprite);
  CHECK(render->n_spr==0);
  rgba=(uint8_t*)malloc(16u);
  CHECK(rgba);
  memcpy(rgba,expected,16u);
  CHECK(gml_sprite_append_from_rgba_frames(render,rgba,2,2,1,0,0,NULL)==0);
  gml_render_clear_runtime_sprites(render);
  CHECK(render->n_spr==0);
  free(bytes);
  gml_render_free(render);
  free(render);
  return 1;
}

static int surface_draw_sources_are_read_only(void){
  GmlRender *render=(GmlRender*)calloc(1,sizeof *render);
  uint32_t frame[16]={0};
  GmlRenderDeviceServices device;
  GmlRenderDrawOp operation;
  GmlRenderDrawBatch batch;
  GmlRenderResource *resource;
  GmlRenderJournalMetrics before={0},after={0};
  GmlRenderResourceSource source={0};
  const uint32_t *plane=NULL;
  FakeDevice fake={0};
  int id,sprite,plane_width=0,plane_height=0;
  CHECK(render);
  render->next_surface_id=1;
  render->active_shader=-1;
  render->color_write_mask=0x0Fu;
  render->alphablend=1;
  render->blend_equation=render->blend_equation_alpha=1;
  gml_render_begin(render,frame,4,4,0.0,0.0);
  id=gml_surface_create(render,4,4);
  CHECK(id>0);
  device=service(&fake);
  gml_render_bind_device(render,&device);
  resource=gml_render_surface_resource_ensure(render,id);
  CHECK(resource);
  operation=clear_op(2u,0xFFB42834u,GML_RENDER_REPLAY_EXACT);
  operation.target.image=gml_render_resource_image_ref(resource);
  operation.target.image.content_version=2u;
  operation.target.pixel_format=GML_RENDER_PIXEL_ARGB8888;
  batch=(GmlRenderDrawBatch){&operation,1u,NULL,0u};
  CHECK(gml_render_surface_scene_submit(render,id,&batch,NULL,0u)==
        GML_RENDER_RESOURCE_EXEC_DEVICE);
  render->surface[id-1].opaque_known=1;
  render->surface[id-1].all_opaque=1;
  render->surface[id-1].all_transparent=0;
  gml_render_resource_journal_metrics(resource,&before);
  CHECK(before.logical_version==2u && before.gpu_version==2u);

  render->interp=1;
  gml_draw_surface_part_ext(render,id,0,0,2,2,1,1,1.5,1.5,0xFFFFFFu,1.0);
  CHECK(frame[5]!=0u);
  gml_render_resource_journal_metrics(resource,&after);
  CHECK(after.logical_version==before.logical_version && after.gpu_version==before.gpu_version);

  memset(frame,0,sizeof frame);
  gml_draw_surface_stretched(render,id,1,1,3,3,0xFFFFFFu,1.0);
  CHECK(frame[5]!=0u);
  gml_render_resource_journal_metrics(resource,&after);
  CHECK(after.logical_version==before.logical_version && after.gpu_version==before.gpu_version);

  memset(frame,0,sizeof frame);
  gml_draw_surface_ext(render,id,1,3,1,1,90,0xFFFFFFu,1.0);
  CHECK(frame[1]!=0u || frame[5]!=0u);
  gml_render_resource_journal_metrics(resource,&after);
  CHECK(after.logical_version==before.logical_version && after.gpu_version==before.gpu_version);

  memset(frame,0,sizeof frame);
  gml_draw_surface_tiled_ext(render,id,0,0,1,1,0xFFFFFFu,1.0);
  CHECK(frame[0]!=0u);
  gml_render_resource_journal_metrics(resource,&after);
  CHECK(after.logical_version==before.logical_version && after.gpu_version==before.gpu_version);

  sprite=gml_sprite_create_from_surface(render,id,0,0,2,2,0,0,0,0);
  CHECK(sprite>=0 && render->spr[sprite].runtime_rgba &&
        render->spr[sprite].runtime_rgba[0]==0xB4u &&
        render->spr[sprite].runtime_rgba[1]==0x28u &&
        render->spr[sprite].runtime_rgba[2]==0x34u &&
        render->spr[sprite].runtime_rgba[3]==0xFFu);
  gml_render_resource_journal_metrics(resource,&after);
  CHECK(after.logical_version==before.logical_version && after.gpu_version==before.gpu_version &&
        fake.readbacks==0 && gml_render_resource_source_view(resource,&source) &&
        source.image.content_version==before.logical_version &&
        source.resident_resource==resource);
  CHECK(gml_render_surface_plane(render,id,&plane_width,&plane_height,&plane) &&
        plane && plane_width==4 && plane_height==4 && plane[0]==0xFFB42834u);
  gml_render_resource_journal_metrics(resource,&after);
  CHECK(after.logical_version==before.logical_version && after.gpu_version==before.gpu_version);
  gml_render_free(render);
  free(render);
  return 1;
}

static int target_pixel_observes_resident_result(void){
  GmlRender *render=(GmlRender*)calloc(1,sizeof *render);
  GmlVM vm={0};
  GmlVal create_args[]={vreal(64),vreal(0),vreal(1)};
  GmlVal buffer;
  uint32_t base[16]={0};
  GmlRenderDeviceServices device;
  GmlRenderDrawOp operation;
  GmlRenderDrawBatch batch;
  GmlRenderResource *resource;
  GmlRenderApplicationReadView read_view={0};
  GmlRenderJournalMetrics before={0},after={0};
  FakeDevice fake={0};
  uint32_t pixel=0;
  const uint32_t *mirror_pixels,*observed_pixels;
  int id,mirror_id,sprite,w=0,h=0;
  CHECK(render);
  render->next_surface_id=1;
  render->active_shader=-1;
  render->color_write_mask=0x0Fu;
  CHECK(gml_render_application_surface_ensure_owned(render,4,4));
  device=service(&fake);
  gml_render_bind_device(render,&device);
  CHECK(gml_render_begin_resident_owned_application(render,0.0,0.0,0));
  vm.render=render;
  buffer=gml_builtin_call(&vm,"buffer_create",create_args,3);
  CHECK(buffer.t==V_REAL);
  gml_render_set_pending_fill(render,0xFF2468ACu);
  gml_render_flush_pending_fill(render);
  CHECK(gml_render_scene_flush(render) && render->resident_application_pass);
  resource=render->app_surface_resource;
  CHECK(resource && render->app_surface_owned[5]==0u);
  gml_render_resource_journal_metrics(resource,&before);
  CHECK(before.logical_version==before.gpu_version && before.logical_version>1u);
  CHECK(gml_render_application_surface_owned_read(render,&read_view) &&
        read_view.width==4 && read_view.height==4 &&
        read_view.pixels==render->app_surface_owned &&
        read_view.pixels[5]==0xFF2468ACu);
  gml_render_resource_journal_metrics(resource,&after);
  CHECK(after.logical_version==before.logical_version &&
        after.gpu_version==before.gpu_version &&
        render->resident_application_pass && fake.readbacks==0);
  observed_pixels=gml_surface_pixels_read(render,0,&w,&h);
  CHECK(observed_pixels && w==4 && h==4 &&
        observed_pixels[5]==0xFF2468ACu);
  sprite=gml_sprite_create_from_surface(render,0,1,1,1,1,0,0,0,0);
  CHECK(sprite>=0 && render->spr[sprite].runtime_rgba &&
        render->spr[sprite].runtime_rgba[0]==0x24u &&
        render->spr[sprite].runtime_rgba[1]==0x68u &&
        render->spr[sprite].runtime_rgba[2]==0xACu &&
        render->spr[sprite].runtime_rgba[3]==0xFFu);
  gml_render_resource_journal_metrics(resource,&after);
  CHECK(after.logical_version==before.logical_version &&
        after.gpu_version==before.gpu_version && fake.readbacks==0);
  {
    GmlVal pixel_args[]={vreal(0),vreal(1),vreal(1)};
    GmlVal export_args[]={buffer,vreal(0),vreal(0)};
    GmlVal peek_args[]={buffer,vreal(20),vreal(1)};
    GmlVal colour=gml_builtin_call(&vm,"surface_getpixel",pixel_args,3);
    CHECK(colour.t==V_REAL && colour.d==0xAC6824u);
    (void)gml_builtin_call(&vm,"buffer_get_surface",export_args,3);
    CHECK(gml_builtin_call(&vm,"buffer_peek",peek_args,3).d==0x24u);
    peek_args[1]=vreal(21);
    CHECK(gml_builtin_call(&vm,"buffer_peek",peek_args,3).d==0x68u);
    peek_args[1]=vreal(22);
    CHECK(gml_builtin_call(&vm,"buffer_peek",peek_args,3).d==0xACu);
    peek_args[1]=vreal(23);
    CHECK(gml_builtin_call(&vm,"buffer_peek",peek_args,3).d==0xFFu);
    gml_render_resource_journal_metrics(resource,&after);
    CHECK(after.logical_version==before.logical_version &&
          after.gpu_version==before.gpu_version && fake.readbacks==0);
  }
  mirror_id=gml_surface_create(render,4,4);
  CHECK(mirror_id>0 &&
        gml_render_surface_mirror_pixels(render,mirror_id,read_view.pixels,4,4,1));
  mirror_pixels=gml_surface_pixels_read(render,mirror_id,NULL,NULL);
  CHECK(mirror_pixels && mirror_pixels[5]==0xFF2468ACu);
  gml_render_resource_journal_metrics(resource,&after);
  CHECK(after.logical_version==before.logical_version &&
        after.gpu_version==before.gpu_version);
  CHECK(gml_render_target_pixel(render,1,1,&pixel) && pixel==0xFF2468ACu);
  gml_render_resource_journal_metrics(resource,&after);
  CHECK(after.logical_version==before.logical_version &&
        after.gpu_version==before.gpu_version &&
        render->resident_application_pass && fake.readbacks==0);

  gml_render_begin(render,base,4,4,0.0,0.0);
  id=gml_surface_create(render,4,4);
  CHECK(id>0 && gml_surface_set_target(render,id));
  resource=gml_render_surface_resource_ensure(render,id);
  CHECK(resource);
  operation=clear_op(2u,0xFFB42834u,GML_RENDER_REPLAY_EXACT);
  operation.target.image=gml_render_resource_image_ref(resource);
  operation.target.image.content_version++;
  operation.target.pixel_format=GML_RENDER_PIXEL_ARGB8888;
  batch=(GmlRenderDrawBatch){&operation,1u,NULL,0u};
  CHECK(gml_render_surface_scene_submit(render,id,&batch,NULL,0u)==
        GML_RENDER_RESOURCE_EXEC_DEVICE);
  gml_render_resource_journal_metrics(resource,&before);
  CHECK(before.logical_version==before.gpu_version &&
        render->surface[id-1].px[5]==0u);
  CHECK(gml_render_target_pixel(render,1,1,&pixel) && pixel==0xFFB42834u);
  gml_render_resource_journal_metrics(resource,&after);
  CHECK(after.logical_version==before.logical_version &&
        after.gpu_version==before.gpu_version && fake.readbacks==0);
  {
    GmlVal pixel_args[]={vreal(id),vreal(1),vreal(1)};
    GmlVal export_args[]={buffer,vreal(id),vreal(0)};
    GmlVal peek_args[]={buffer,vreal(20),vreal(1)};
    GmlVal colour=gml_builtin_call(&vm,"surface_getpixel",pixel_args,3);
    CHECK(colour.t==V_REAL && colour.d==0x3428B4u);
    (void)gml_builtin_call(&vm,"buffer_get_surface",export_args,3);
    CHECK(gml_builtin_call(&vm,"buffer_peek",peek_args,3).d==0xB4u);
    peek_args[1]=vreal(21);
    CHECK(gml_builtin_call(&vm,"buffer_peek",peek_args,3).d==0x28u);
    peek_args[1]=vreal(22);
    CHECK(gml_builtin_call(&vm,"buffer_peek",peek_args,3).d==0x34u);
    peek_args[1]=vreal(23);
    CHECK(gml_builtin_call(&vm,"buffer_peek",peek_args,3).d==0xFFu);
    gml_render_resource_journal_metrics(resource,&after);
    CHECK(after.logical_version==before.logical_version &&
          after.gpu_version==before.gpu_version && fake.readbacks==0);
  }
  observed_pixels=gml_surface_pixels_read(render,id,&w,&h);
  CHECK(observed_pixels && w==4 && h==4 &&
        observed_pixels[5]==0xFFB42834u);
  sprite=gml_sprite_create_from_surface(render,id,1,1,1,1,0,0,0,0);
  CHECK(sprite>=0 && render->spr[sprite].runtime_rgba &&
        render->spr[sprite].runtime_rgba[0]==0xB4u &&
        render->spr[sprite].runtime_rgba[1]==0x28u &&
        render->spr[sprite].runtime_rgba[2]==0x34u &&
        render->spr[sprite].runtime_rgba[3]==0xFFu);
  gml_render_resource_journal_metrics(resource,&after);
  CHECK(after.logical_version==before.logical_version &&
        after.gpu_version==before.gpu_version && fake.readbacks==0);
  gml_render_set_pending_fill(render,0xFF3975B1u);
  CHECK(gml_render_target_pixel(render,1,1,&pixel) && pixel==0xFF3975B1u);
  gml_render_resource_journal_metrics(resource,&after);
  CHECK(after.logical_version>before.logical_version &&
        after.cpu_version==after.logical_version &&
        after.gpu_version==before.gpu_version && fake.readbacks==0);
  gml_surface_reset_target(render);
  base[5]=0xFF654321u;
  CHECK(gml_render_target_pixel(render,1,1,&pixel) && pixel==0xFF654321u);
  vm.render=NULL;
  gml_vm_free(&vm);
  gml_render_free(render);
  free(render);
  return 1;
}

int main(void){
  int passed=0;
  passed+=dispatch_retained_replay();
  passed+=dispatch_replay_owner_move();
  passed+=journal_loss_and_mutation();
  passed+=argb_surface_replay();
  passed+=additive_background_exact_replay();
  passed+=pressure_and_fallback();
  passed+=upload_refusal_recovers_cpu();
  passed+=journal_saturation_stays_bounded();
  passed+=recreate_device_before_reader();
  passed+=resident_target_copy();
  passed+=resident_source_recipe_versions();
  passed+=resident_source_recipe_chain();
  passed+=partial_error_and_write_lease();
  passed+=checkpoint_and_failure();
  passed+=pending_shader_source_cross_target();
  passed+=public_checkpoint_readback_refusal();
  passed+=queued_checkpoint_flush_refusal_preserves_target();
  passed+=queued_quad_groups_are_atomic();
  passed+=queued_multicell_preparation_is_transactional();
  passed+=paired_root_queue_groups_and_versions();
  passed+=paired_root_preparation_rolls_back_both_planes();
  passed+=paired_root_clear_versions_and_coverage();
  passed+=paired_root_clear_refusal_preserves_order();
  passed+=paired_root_clear_scope_is_explicit();
  passed+=paired_root_public_clear_keeps_device_authority();
  passed+=paired_root_public_sprite_and_base_only_glyph();
  passed+=paired_root_numbered_ports_keep_peer_inert();
  passed+=paired_root_atlas_ports_are_atomic();
  passed+=paired_root_atlas_refuses_unqualified_ports();
  passed+=paired_root_public_axis_atlas_ports();
  passed+=paired_root_composition_keeps_both_sources_resident();
  passed+=paired_root_composition_refuses_unsupported_state();
  passed+=paired_root_primitives_change_only_the_base();
  passed+=paired_root_full_rectangle_clears_both_samples();
  passed+=paired_root_storage_retirement_is_transactional();
  passed+=paired_root_physical_clear_keeps_sources_independent();
  passed+=paired_root_second_plane_refusal_keeps_only_undelivered_work();
  passed+=paired_root_binding_and_loss_boundaries();
  passed+=paired_root_cpu_boundary_and_unfinished_teardown();
  passed+=paired_root_capacity_does_not_deliver_half_a_draw();
  passed+=paired_root_preparation_keeps_each_image_budget();
  passed+=classic_phase_geometry_is_pure_and_refusable();
  passed+=queued_immutable_checkpoint_spans_sources_and_states();
  passed+=queued_filtered_font_spans();
  passed+=rejection_and_isolation();
  passed+=persistent_surface_lifecycle();
  passed+=retained_target_stays_resident();
  passed+=resident_numbered_target_nested_cpu_boundary();
  passed+=resident_numbered_target_queued_atlas_observation();
  passed+=volatile_loss_discards_queued_nested_target();
  passed+=volatile_loss_recreates_only_unobserved_application();
  passed+=resident_numbered_target_rebind_retention_and_reuse();
  passed+=resident_empty_reset_preserves_base_owner();
  passed+=resident_empty_reset_does_not_admit_a_different_owner();
  passed+=resident_target_alias_resumes_cpu_parent();
  passed+=numbered_surface_resize_replays_and_retires();
  passed+=numbered_surface_free_bound_resource();
  passed+=owned_application_resize_and_context_loss();
  passed+=resident_base_linear_history();
  passed+=resident_base_history_failures();
  passed+=resident_surface_resize_storage();
  passed+=resident_surface_resize_refusal();
  passed+=resident_surface_resize_state();
  passed+=resident_application_pending_clear();
  passed+=owned_application_clear_without_cpu_view();
  passed+=independent_persistent_target_clear();
  passed+=independent_numbered_target_admission();
  passed+=resident_pending_clear_after_queued_draw();
  passed+=first_generation_underlay_resource_borrow();
  passed+=resident_layer_filter_capture_barrier();
  passed+=resident_inline_layer_effect_cpu_bridge();
  passed+=resident_layer_effect_continuation_authority();
  passed+=resident_numbered_layer_effect_target();
  passed+=resident_flat_primitive_public_order();
  passed+=resident_classic_identity_filtered_underlay();
  passed+=resident_classic_rectangle_scope();
  passed+=resident_rectangle_function_matrix();
  passed+=resident_circle_pixel_packets();
  passed+=resident_classic_owned_rectangle_scope();
  passed+=resident_rectangle_outline_coalesced();
  passed+=resident_studio_gui_rectangle_scope();
  passed+=resident_classic_alpha_rectangle_scope();
  passed+=resident_studio_alpha_rectangle_scope();
  passed+=resident_firstgen_gui_alpha_rectangle_scope();
  passed+=resident_classic_inverse_rectangle_scope();
  passed+=resident_viewless_base_canvas_order();
  passed+=completed_screen_reader_and_writer();
  passed+=conditional_screen_recovery_authority();
  passed+=conditional_screen_recovery_fullwidth_authority();
  passed+=completed_screen_deferred_upscale();
  passed+=resident_borrowed_surface_zero_screen_shader();
  passed+=surface_metadata_preserves_device_authority();
  passed+=resident_centered_full_source_shader();
  passed+=resident_threshold_palette_surface();
  passed+=resident_lut_surface();
  passed+=resident_identity_gui_filtered_atlas();
  passed+=resident_zero_alpha_layer_fill_noop();
  passed+=resident_rectangle_fill_noop();
  passed+=resident_line_alpha_noop();
  passed+=resident_circle_alpha_noop();
  passed+=resident_unit_world_primitive_order();
  passed+=resident_gradient_primitive_cpu_boundary();
  passed+=resident_particle_square_public_order();
  passed+=resident_particle_line_public_order();
  passed+=particle_line_recovery_uses_cpu_raster();
  passed+=resident_software3d_primitive_cpu_bridge();
  passed+=resident_filled_span_public_order();
  passed+=resident_line_coverage_public_order();
  passed+=resident_line_function_transaction();
  passed+=resident_borrowed_application_screen_handoff();
  passed+=resident_outline_coverage_public_order();
  passed+=resident_atlas_uv_refusal_preserves_queued_prefix();
  passed+=resident_application_sprite_cpu_fallback();
  passed+=resident_empty_sprite_preserves_queued_draws();
  passed+=resident_black_additive_sprite_noop();
  passed+=resident_empty_sprite_part_preserves_pending_work();
  passed+=resident_zero_alpha_sprite_part_noop();
  passed+=resident_offscreen_sprite_part_preserves_queued_draws();
  passed+=resident_filtered_gui_offscreen_sprite_noop();
  passed+=resident_filtered_application_offscreen_sprite_noop();
  passed+=resident_clipped_filtered_background_part_noop();
  passed+=resident_background_and_tile_cpu_barriers();
  passed+=resident_integer_tile_clipping(0);
  passed+=resident_integer_tile_clipping(1);
  passed+=resident_integer_endpoint_sprite_part();
  passed+=resident_endpoint_source_sprite_part();
  passed+=resident_fractional_screen_font();
  passed+=resident_screen_binary_rotation();
  passed+=resident_trimmed_unit_layer();
  passed+=resident_modern_unit_sprite_part();
  passed+=resident_integer_sprite_rounded_noop();
  passed+=atlas_scene_identity_and_retirement();
  passed+=immutable_font_scene_preparation();
  passed+=immutable_sprite_font_scene_preparation();
  passed+=immutable_room_scene_preparation();
  passed+=runtime_font_page_scene_retirement();
  passed+=mutable_scene_identity_across_renderers();
  passed+=surface_observer_materializes_suspended_fill();
  passed+=resident_content_surface_program_boundary();
  passed+=resident_content_surface_full_part_boundary();
  passed+=stale_auxiliary_surface_shader_fallback();
  passed+=public_content_surface_program_chain();
  passed+=resident_checkpoint_span_seals_at_observer();
  passed+=checkpoint_span_mixed_suffix_and_faults();
  passed+=bounded_checkpoint_staging_lifecycle();
  passed+=incremental_checkpoint_publication();
  passed+=public_surface_part_qualified_dispatch();
  passed+=resident_opaque_atlas_sprite_gpu();
  passed+=resident_numbered_sprite_channel_masks();
  passed+=resident_coarse_veil_exact_recovery();
  passed+=resident_gui_coarse_veil_on_demand();
  passed+=resident_stretched_function_on_demand();
  passed+=resident_stretched_coarse_box_on_demand();
  passed+=resident_stretched_coarse_box_limit();
  passed+=resident_stretched_raw_opacity_on_demand();
  passed+=resident_stretched_unavailable_source();
  passed+=resident_mapped_gui_stretch_on_demand();
  passed+=resident_modern_screen_filtered_sprite();
  passed+=resident_maximised_filtered_real_font();
  passed+=resident_packaged_sprite_font_glyph();
  passed+=resident_filtered_gui_real_font();
  passed+=surface_pending_source_lifetime(0,0);
  passed+=surface_pending_source_lifetime(1,0);
  passed+=surface_pending_source_lifetime(1,9);
  passed+=surface_pending_source_lifetime(1,10);
  passed+=surface_pending_source_lifetime(1,11);
  passed+=clipped_destination_surface_after_cpu_suffix();
  passed+=application_surface_pending_source_lifetime();
  passed+=resident_gui_application_composition();
  passed+=resident_fullwidth_gui_application();
  passed+=current_cpu_source_view_is_not_observer();
  passed+=resident_nearest_surface_cutout_targets();
  passed+=resident_current_cpu_application_underlay();
  passed+=resident_unregistered_cpu_application_underlay();
  passed+=resident_surface_after_pending_underlay();
  passed+=resident_automatic_underlay_exclusions();
  passed+=resident_nearest_font_pending_underlay();
  passed+=resident_packaged_real_font_glyph(0);
  passed+=resident_packaged_real_font_glyph(1);
  passed+=resident_binary_atlas_sprite_gpu();
  passed+=resident_integer_scaled_atlas_sprite_gpu();
  passed+=resident_classic_nonphase_sprite_targets();
  passed+=resident_integer_sprite_function();
  passed+=resident_nearest_sprite_function();
  passed+=resident_flat_fog_sprite_function();
  passed+=resident_first_generation_rotated_point_sprite_function();
  passed+=resident_tiled_sprite_function();
  passed+=resident_tiled_sprite_underlay_noops();
  passed+=resident_physical_filtered_sprite_function();
  passed+=resident_matched_filtered_sprite_function();
  passed+=resident_modern_physical_filtered_sprite_function();
  passed+=resident_modern_rotated_filtered_sprite_function();
  passed+=resident_cpu_passthrough_sprite_scope();
  passed+=resident_classic_large_point_sprite_function();
  passed+=resident_nearest_geometry_capacity();
  passed+=resident_external_base_mask();
  passed+=resident_integer_background_function();
  passed+=resident_fractional_background_function();
  passed+=resident_background_stored_extents();
  passed+=resident_nearest_tile_function();
  passed+=resident_nearest_tile_crop_edges();
  passed+=resident_nearest_background_crop_edges();
  passed+=resident_nearest_sprite_crop_edges();
  passed+=resident_selected_background_crop_edges();
  passed+=resident_nearest_text_function(0);
  passed+=resident_nearest_text_function(1);
  passed+=resident_nearest_text_function(2);
  passed+=resident_nearest_text_function(3);
  passed+=resident_rotated_text_function();
  passed+=resident_selected_background_function();
  passed+=resident_classic_owned_binary_unit_sprite();
  passed+=resident_runtime_binary_unit_sprites();
  passed+=resident_classic_unit_tiles();
  passed+=runtime_sprite_scene_lifetimes();
  passed+=resident_classic_clipped_sprite_noops();
  passed+=resident_classic_rotated_sprite_noops();
  passed+=resident_outside_sprite_geometry();
  passed+=resident_vertical_phase_binding_scope();
  passed+=resident_outside_sprite_preserves_queued_prefix();
  passed+=resident_zero_scale_sprite_preserves_queued_prefix();
  passed+=resident_rounded_empty_axis_preserves_queued_prefix();
  passed+=resident_rounded_empty_axis_geometry();
  passed+=resident_studio_unit_background_noops();
  passed+=resident_classic_depth_free_sprites();
  passed+=resident_classic_depth_free_glyphs();
  passed+=resident_classic_unit_background_targets();
  passed+=resident_classic_scaled_background_scope();
  passed+=resident_classic_retained_numbered_target();
  passed+=resident_firstgen_nearest_magnification_authority();
  passed+=resident_integer_world_sprite();
  passed+=resident_gui_replacement_sprite();
  passed+=resident_modern_screen_point_sprite();
  passed+=resident_large_screen_axis_row_metadata();
  passed+=resident_large_screen_coherent_phase();
  passed+=resident_cached_rotation_trim_tie_metadata();
  passed+=resident_large_unit_sprite_scope();
  passed+=resident_tiny_nearest_sprite_scope();
  passed+=resident_opaque_sprite_complete_application();
  passed+=resident_integer_world_sourceover_sprite();
  passed+=resident_identity_gui_unit_sourceover();
  passed+=resident_firstgen_gui_half_point();
  passed+=resident_integer_world_packed_sprite();
  passed+=resident_integer_sourceover_exclusions();
  passed+=state_without_context();
  passed+=device_observation_preserves_actual_replayable_pixels();
  passed+=resident_authority_avoids_replay_retention();
  passed+=resident_observation_retry_and_fallback();
  passed+=resident_source_copy_and_loss();
  passed+=metadata_state_capacity_preserves_pending_pixels();
  passed+=metadata_state_capacity_skips_pixel_compression();
  passed+=zero_base_runtime_sprite_state();
  passed+=surface_draw_sources_are_read_only();
  passed+=target_pixel_observes_resident_result();
  passed+=nearest_subrectangle_mirrors();
  passed+=source_free_primitive_rect_replay();
  passed+=stretched_float_exact_replay();
  passed+=filtered_font_exact_recovery();
  printf("render resource cases: passed=%d failed=%d\n",passed,257-passed);
  return passed==257?EXIT_SUCCESS:EXIT_FAILURE;
}
