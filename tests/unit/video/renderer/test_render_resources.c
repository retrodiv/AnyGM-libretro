/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Resource authority tests. The fake device verifies transactions and lifetimes; exact GL pixels
 * are checked separately by the real-context scene probe. */
#include "gml_render_resources.h"
#include "gml_render_internal.h"
#include "gml_render_state.h"
#include "gml_render_surfaces_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if(!(condition)){ \
  fprintf(stderr,"render resources: %s:%d: %s\n",__FILE__,__LINE__,#condition); \
  return 0; \
} } while(0)

typedef struct FakeDevice {
  int uploads,submits,readbacks,retires;
  int fail_upload,fail_readback;
  uint32_t status,accepted_prefix;
  uint32_t readback_colour;
} FakeDevice;

static int fake_upload(void *context,const GmlRenderImageUpload *upload){
  FakeDevice *fake=(FakeDevice*)context;
  fake->uploads++;
  return !fake->fail_upload && upload && upload->bytes && upload->byte_capacity;
}

static GmlRenderExecutionResult fake_submit(void *context,const GmlRenderDrawBatch *batch){
  FakeDevice *fake=(FakeDevice*)context;
  GmlRenderExecutionResult result={0};
  fake->submits++;
  result.status=fake->status?fake->status:GML_RENDER_EXEC_ACCEPTED;
  result.accepted_prefix=fake->accepted_prefix?fake->accepted_prefix:
    (result.status==GML_RENDER_EXEC_ACCEPTED?batch->operation_count:0u);
  result.all_destinations_uncertain=result.status!=GML_RENDER_EXEC_ACCEPTED;
  return result;
}

static int fake_readback(void *context,const GmlRenderImageReadback *readback){
  FakeDevice *fake=(FakeDevice*)context;
  uint32_t *pixels;
  fake->readbacks++;
  if(fake->fail_readback || !readback || !readback->bytes ||
     readback->byte_capacity<(size_t)readback->storage_width*readback->storage_height*4u)
    return 0;
  pixels=(uint32_t*)readback->bytes;
  for(uint32_t index=0;index<readback->storage_width*readback->storage_height;index++)
    pixels[index]=fake->readback_colour;
  return 1;
}

static void fake_retire(void *context,GmlRenderImageRef image){
  FakeDevice *fake=(FakeDevice*)context;
  (void)image;
  fake->retires++;
}

static int fake_capabilities(void *context,GmlRenderDeviceCaps *out_caps){
  (void)context;
  if(!out_caps) return 0;
  memset(out_caps,0,sizeof *out_caps);
  out_caps->max_texture_extent=4096u;
  return 1;
}

static int fake_barrier(void *context){
  (void)context;
  return 1;
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
    (GmlRenderResourceLimits){metrics.journal_bytes,1u});
  CHECK(limited);
  CHECK(gml_render_resource_submit(limited,&device,&batch,NULL,0u)==
        GML_RENDER_RESOURCE_EXEC_DEVICE);
  batch.operations=&operations[1];
  CHECK(gml_render_resource_submit(limited,&device,&batch,NULL,0u)==
        GML_RENDER_RESOURCE_EXEC_DEVICE);
  gml_render_resource_journal_metrics(limited,&metrics);
  CHECK(metrics.consolidations==1u && metrics.cpu_replays==1u);
  CHECK(metrics.journal_operations==1u);
  CHECK(gml_render_resource_acquire_cpu_read(limited,&observed));
  CHECK(pixels[0]==0xFF445566u && pixels[15]==0xFF445566u);
  gml_render_resource_destroy(limited);
  /* One descriptor cannot fit. The batch must execute once on CPU, before device work. */
  fill(pixels,0xFF000000u);
  limited=gml_render_resource_create(target_ref(1u,1u),4u,4u,
    GML_RENDER_PIXEL_XRGB8888,pixels,(GmlRenderResourceLimits){1u,1u});
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
      GML_RENDER_PIXEL_XRGB8888,pixels,(GmlRenderResourceLimits){1024u,8u});
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
  gml_render_resource_context_lost(destination);
  CHECK(gml_render_resource_acquire_cpu_read(destination,NULL));
  CHECK(destination_pixels[0]==0x40010203u && destination_pixels[15]==0x40010203u);
  CHECK(fake.readbacks==0 && fake.submits==2);
  CHECK(other_fake.submits==1);
  gml_render_resource_destroy(source);
  gml_render_resource_destroy(destination);
  gml_render_resource_destroy(other_destination);
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
  gml_surface_copy(render,second,0,0,first);
  pixels=gml_surface_pixels_read(render,second,&w,&h);
  CHECK(pixels && pixels[0]==0x40224466u && pixels[15]==0x40224466u);
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

static int atlas_scene_identity_and_retirement(void){
  GmlRender *first=(GmlRender*)calloc(1,sizeof *first);
  GmlRender *second=(GmlRender*)calloc(1,sizeof *second);
  GmlRenderResourceSource one={0},same={0},two={0};
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
  first->atlas[0].runtime_font_page=1;
  CHECK(!gml_render_atlas_scene_source_view(first,0,&same) && !same.bytes);
  gml_render_free(first);
  gml_render_free(second);
  CHECK(fake.retires==2);
  free(first);
  free(second);
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
  CHECK(render->resident_application_pass && fake.submits==2 &&
        metrics.journal_operations==2u && metrics.cpu_replays==0u &&
        render->app_surface_owned[5]==0u);
  CHECK(gml_render_application_surface_owned_view(render,&view) &&
        view.pixels[0]==0xFF000000u && view.pixels[5]==0xFFD02010u &&
        view.pixels[6]==0xFF10D020u && view.pixels[9]==0xFF2010D0u &&
        view.pixels[10]==0xFFC0B0A0u && fake.readbacks==0);
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
  CHECK(render->resident_application_pass && fake.submits==5 &&
        !render->fb_all_transparent);
  CHECK(gml_render_application_surface_owned_view(render,&view) &&
        view.pixels[0]==0u && view.pixels[5]==0xFFD02010u);
  gml_render_free(render);
  free(render);
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
  gml_render_bind_device(original,NULL);
  size=gml_render_state_size(original,0);
  CHECK(size>0 && fake.readbacks==0);
  bytes=(uint8_t*)malloc(size);
  CHECK(bytes);
  CHECK(gml_render_state_save(original,0,bytes,size,&written) && written==size);
  CHECK(gml_render_state_load(restored,bytes,size,&used) && used==size);
  pixels=gml_surface_pixels_read(restored,id,&w,&h);
  CHECK(pixels && w==4 && h==4);
  CHECK(pixels[0]==0x4088AABBu && pixels[15]==0x4088AABBu);
  CHECK(!restored->surface[id-1].resource && fake.readbacks==0);
  free(bytes);
  gml_surface_free(original,id);
  gml_surface_free(restored,id);
  free(original);
  free(restored);
  return 1;
}

int main(void){
  int passed=0;
  passed+=journal_loss_and_mutation();
  passed+=argb_surface_replay();
  passed+=pressure_and_fallback();
  passed+=recreate_device_before_reader();
  passed+=resident_target_copy();
  passed+=partial_error_and_write_lease();
  passed+=checkpoint_and_failure();
  passed+=rejection_and_isolation();
  passed+=persistent_surface_lifecycle();
  passed+=retained_target_stays_resident();
  passed+=owned_application_resize_and_context_loss();
  passed+=resident_application_pending_clear();
  passed+=resident_application_sprite_cpu_fallback();
  passed+=atlas_scene_identity_and_retirement();
  passed+=resident_opaque_atlas_sprite_gpu();
  passed+=state_without_context();
  passed+=nearest_subrectangle_mirrors();
  printf("render resource cases: passed=%d failed=%d\n",passed,17-passed);
  return passed==17?EXIT_SUCCESS:EXIT_FAILURE;
}
