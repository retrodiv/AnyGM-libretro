/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* The API-neutral GPU facade: context lifetime, eligibility, counters, and the one place a plan
 * crosses into a backend. Nothing here knows what a graphics API is. */
#include "gml_gpu_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

GmlGpu *gml_gpu_create(void){
  GmlGpu *gpu=(GmlGpu*)calloc(1,sizeof *gpu);
  if(!gpu) return NULL;
  gpu->backend=gml_gpu_opengl_create();
  if(!gpu->backend){ free(gpu); return NULL; }
  return gpu;
}

void gml_gpu_destroy(GmlGpu *gpu,int context_is_current){
  if(!gpu) return;
  gml_gpu_opengl_destroy(gpu->backend,context_is_current && gpu->context_active);
  free(gpu);
}

int gml_gpu_context_reset(GmlGpu *gpu,const GmlGpuContext *context){
  if(!gpu) return 0;
  if(!context || !context->get_proc_address || !context->get_current_framebuffer ||
     (context->api!=GML_GPU_API_OPENGL_CORE && context->api!=GML_GPU_API_OPENGLES3)){
    snprintf(gpu->error,sizeof gpu->error,"graphics context is incomplete");
    return 0;
  }
  /* A reset with no destroy before it means the previous context is already gone. Deleting into
   * the new one would name objects that never existed there, so forget them first. */
  if(gpu->context_active) gml_gpu_opengl_forget(gpu->backend);
  gpu->context=*context;
  gpu->context_active=0;
  gpu->error[0]='\0';
  if(!gml_gpu_opengl_reset(gpu->backend,&gpu->context,gpu->error,sizeof gpu->error)){
    gpu->counters.program_failures++;
    gml_gpu_opengl_forget(gpu->backend);
    memset(&gpu->context,0,sizeof gpu->context);
    return 0;
  }
  gpu->context_active=1;
  gpu->counters.context_resets++;
  return 1;
}

void gml_gpu_context_lost(GmlGpu *gpu){
  if(!gpu || !gpu->context_active) return;
  gml_gpu_opengl_forget(gpu->backend);
  gpu->context_active=0;
  gpu->counters.context_losses++;
  memset(&gpu->context,0,sizeof gpu->context);
}

int gml_gpu_context_active(const GmlGpu *gpu){
  return gpu && gpu->context_active;
}

static int scene_capabilities(void *context,GmlRenderDeviceCaps *out_caps){
  GmlGpu *gpu=(GmlGpu*)context;
  if(!gpu || !out_caps || !gpu->context_active) return 0;
  return gml_gpu_opengl_scene_capabilities(gpu->backend,out_caps);
}

static GmlRenderExecutionResult scene_submit(void *context,const GmlRenderDrawBatch *batch){
  GmlRenderExecutionResult result;
  GmlGpu *gpu=(GmlGpu*)context;
  memset(&result,0,sizeof result);
  result.status=GML_RENDER_EXEC_UNSUPPORTED;
  if(gpu && gpu->context_active && gpu->context.get_current_framebuffer){
    gpu->counters.scene_batches_offered++;
    result=gml_gpu_opengl_scene_submit(gpu->backend,batch,
      gpu->context.get_current_framebuffer(gpu->context.userdata),
      gpu->error,sizeof gpu->error);
    if(batch && result.status==GML_RENDER_EXEC_ACCEPTED &&
       result.accepted_prefix==batch->operation_count){
      gpu->counters.scene_batches_accepted++;
      for(uint32_t index=0;index<batch->operation_count;index++){
        uint32_t opcode=batch->operations[index].opcode;
        if(opcode<GML_RENDER_OPERATION_COUNT)
          gpu->counters.scene_operations[opcode]++;
        if((opcode==GML_RENDER_OP_TEXTURED_QUAD ||
            opcode==GML_RENDER_OP_COLOURED_PRIMITIVE ||
            opcode==GML_RENDER_OP_LAYER_EFFECT ||
            opcode==GML_RENDER_OP_CONTENT_SHADER_DRAW) &&
           batch->operations[index].scene_family<GML_RENDER_SCENE_FAMILY_COUNT)
          gpu->counters.scene_families[batch->operations[index].scene_family]++;
      }
    } else {
      gpu->counters.scene_batches_refused++;
      gpu->counters.scene_partial_prefix+=result.accepted_prefix;
    }
    return result;
  }
  return result;
}

static int scene_upload(void *context,const GmlRenderImageUpload *transfer){
  GmlGpu *gpu=(GmlGpu*)context;
  int did_upload=0;
  if(!gpu || !gpu->context_active ||
     !gml_gpu_opengl_scene_upload(gpu->backend,transfer,&did_upload)) return 0;
  if(did_upload){
    gpu->counters.scene_uploads++;
    gpu->counters.scene_uploaded_bytes+=(uint64_t)transfer->region.width*
                                        transfer->region.height*4u;
  }
  return 1;
}

static int scene_readback(void *context,const GmlRenderImageReadback *transfer){
  GmlGpu *gpu=(GmlGpu*)context;
  if(!gpu || !gpu->context_active ||
     !gml_gpu_opengl_scene_readback(gpu->backend,transfer)) return 0;
  gpu->counters.scene_readbacks++;
  gpu->counters.scene_readback_bytes+=(uint64_t)transfer->region.width*
                                      transfer->region.height*4u;
  return 1;
}

static void scene_retire(void *context,GmlRenderImageRef image){
  GmlGpu *gpu=(GmlGpu*)context;
  if(gpu && gpu->context_active) gml_gpu_opengl_scene_retire(gpu->backend,image);
}

static int scene_flush(void *context){
  GmlGpu *gpu=(GmlGpu*)context;
  return gpu && gpu->context_active && gml_gpu_opengl_scene_flush(gpu->backend);
}

static int scene_wait(void *context){
  GmlGpu *gpu=(GmlGpu*)context;
  return gpu && gpu->context_active && gml_gpu_opengl_scene_wait(gpu->backend);
}

static int scene_content_program_failed(void *context,const GmlPlanShader *shader){
  GmlGpu *gpu=(GmlGpu*)context;
  return gpu && gpu->context_active &&
    gml_gpu_opengl_content_program_failed(gpu->backend,shader);
}
static int scene_has_rgb(void *context,GmlRenderImageRef image,int *has_rgb){
  GmlGpu *gpu=(GmlGpu*)context;
  if(!gpu || !gpu->context_active) return 0;
  if(!gml_gpu_opengl_scene_has_rgb(gpu->backend,image,has_rgb)){
    gpu->counters.scene_rgb_query_refusals++;
    return 0;
  }
  gpu->counters.scene_rgb_queries++;
  return 1;
}

void gml_gpu_device_services(GmlGpu *gpu,GmlRenderDeviceServices *out_services){
  if(!out_services) return;
  memset(out_services,0,sizeof *out_services);
  if(!gpu || !gpu->context_active) return;
  out_services->context=gpu;
  out_services->capabilities=scene_capabilities;
  out_services->submit=scene_submit;
  out_services->upload=scene_upload;
  out_services->readback=scene_readback;
  out_services->retire=scene_retire;
  out_services->flush=scene_flush;
  out_services->wait=scene_wait;
  out_services->content_program_failed=scene_content_program_failed;
  out_services->has_rgb=scene_has_rgb;
}

int gml_gpu_execute_plan(GmlGpu *gpu,GmlRenderPlan *plan){
  if(!gpu || !plan) return 0;
  gpu->counters.frames_offered++;
  if(!gpu->context_active){
    plan->fallback_reason=GML_PLAN_FALLBACK_CONTEXT_UNAVAILABLE;
    gpu->counters.fallbacks[GML_PLAN_FALLBACK_CONTEXT_UNAVAILABLE]++;
    gpu->counters.passes_replayed++;
    return 0;
  }
  if(!gml_render_plan_gpu_eligible(plan)){
    uint32_t reason=plan->fallback_reason;
    if(reason>=GML_PLAN_FALLBACK_COUNT) reason=GML_PLAN_FALLBACK_UNSUPPORTED_OPERATION;
    gpu->counters.fallbacks[reason]++;
    gpu->counters.passes_replayed++;
    return 0;
  }
  plan->fallback_reason=GML_PLAN_FALLBACK_NONE;
  if(!gml_gpu_opengl_execute(gpu->backend,&gpu->context,plan,&gpu->counters,
                         gpu->error,sizeof gpu->error)){
    if(plan->fallback_reason!=GML_PLAN_FALLBACK_SHADER_FAILURE)
      plan->fallback_reason=GML_PLAN_FALLBACK_RESOURCE_UPLOAD_FAILURE;
    else gpu->counters.program_failures++;
    gpu->counters.fallbacks[plan->fallback_reason]++;
    gpu->counters.passes_replayed++;
    return 0;
  }
  gpu->counters.passes_accepted++;
  for(uint32_t index=0;index<plan->operation_count;index++)
    if(plan->operations[index].opcode==GML_PLAN_OP_PRESENT_CPU_FRAME){
      /* Counted apart from an accelerated pass on purpose: carrying a complete software frame to
       * the device is transport, and a diagnostic that blurred the two would report a session
       * that never accelerated anything as a success. */
      gpu->counters.cpu_upload_frames++;
      break;
    }
  return 1;
}

void gml_gpu_counters(const GmlGpu *gpu,GmlGpuCounters *counters){
  if(!counters) return;
  if(!gpu){ memset(counters,0,sizeof *counters); return; }
  *counters=gpu->counters;
}

void gml_gpu_set_readback_pipelined(GmlGpu *gpu,int pipelined){
  if(gpu && gpu->backend) gml_gpu_opengl_set_readback_pipelined(gpu->backend,pipelined);
}

const char *gml_gpu_last_error(const GmlGpu *gpu){
  if(!gpu || !gpu->error[0]) return NULL;
  return gpu->error;
}
