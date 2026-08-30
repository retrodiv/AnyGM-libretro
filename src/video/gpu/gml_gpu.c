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
  gpu->backend=gml_gpu_gl_create();
  if(!gpu->backend){ free(gpu); return NULL; }
  return gpu;
}

void gml_gpu_destroy(GmlGpu *gpu,int context_is_current){
  if(!gpu) return;
  gml_gpu_gl_destroy(gpu->backend,context_is_current && gpu->context_active);
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
  if(gpu->context_active) gml_gpu_gl_forget(gpu->backend);
  gpu->context=*context;
  gpu->context_active=0;
  gpu->error[0]='\0';
  if(!gml_gpu_gl_reset(gpu->backend,&gpu->context,gpu->error,sizeof gpu->error)){
    gpu->counters.program_failures++;
    gml_gpu_gl_forget(gpu->backend);
    memset(&gpu->context,0,sizeof gpu->context);
    return 0;
  }
  gpu->context_active=1;
  gpu->counters.context_resets++;
  return 1;
}

void gml_gpu_context_lost(GmlGpu *gpu){
  if(!gpu || !gpu->context_active) return;
  gml_gpu_gl_forget(gpu->backend);
  gpu->context_active=0;
  gpu->counters.context_losses++;
  memset(&gpu->context,0,sizeof gpu->context);
}

int gml_gpu_context_active(const GmlGpu *gpu){
  return gpu && gpu->context_active;
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
  if(!gml_gpu_gl_execute(gpu->backend,&gpu->context,plan,&gpu->counters,
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

const char *gml_gpu_last_error(const GmlGpu *gpu){
  if(!gpu || !gpu->error[0]) return NULL;
  return gpu->error;
}
