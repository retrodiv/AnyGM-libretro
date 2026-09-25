/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Existing OpenGL hybrid-plan executor. */
#include "gml_gpu_opengl_internal.h"

#include <string.h>

static GmlPlanAxis identity_axis(uint32_t extent){
  GmlPlanAxis axis;
  memset(&axis,0,sizeof axis);
  axis.rule=GML_PLAN_AXIS_PIXEL_CENTRE;
  axis.source_extent=extent;
  axis.destination_extent=extent;
  axis.destination_first=0;
  axis.origin=0.0;
  axis.extent=(double)extent;
  return axis;
}

int gml_gpu_opengl_execute(GmlGpuBackend *backend,const GmlGpuContext *context,
                       GmlRenderPlan *plan,GmlGpuCounters *counters,
                       char *error,size_t error_capacity){
  uintptr_t framebuffer;
  int failed=0;
  if(!backend || !backend->loaded || !context || !plan) return 0;
  framebuffer=context->get_current_framebuffer(context->userdata);
  if(plan->target==GML_PLAN_TARGET_READBACK){
    if(!gml_gpu_opengl_bind_readback_target(backend,plan->target_width,plan->target_height,error,error_capacity))
      return 0;
  } else {
    /* Handle zero is a valid target: some frontends hand over the default framebuffer. */
    backend->gl.BindFramebuffer(GL_FRAMEBUFFER,(GLuint)framebuffer);
  }

  /* The frontend and this core share one context, so every piece of state a pass depends on is set
   * rather than assumed, and nothing is left enabled that the frontend did not enable itself. */
  backend->gl.Disable(GL_BLEND);
  backend->gl.Disable(GL_DEPTH_TEST);
  backend->gl.Disable(GL_STENCIL_TEST);
  backend->gl.Disable(GL_CULL_FACE);
  backend->gl.Disable(GL_DITHER);
  /* Exact 8-bit output: an sRGB-encoding write would change every channel value. Desktop OpenGL
   * exposes the switch; OpenGL ES 3.0 has no default-framebuffer sRGB enable to turn off. */
  if(backend->api==GML_GPU_API_OPENGL_CORE) backend->gl.Disable(GL_FRAMEBUFFER_SRGB);
  backend->gl.ColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
  backend->gl.DepthMask(GL_FALSE);
  backend->gl.Enable(GL_SCISSOR_TEST);
  backend->gl.BindVertexArray(backend->vertex_array);
  backend->gl.UseProgram(backend->program);
  backend->gl.Uniform1i(backend->u_source,0);
  backend->gl.Uniform1i(backend->u_map_x,1);
  backend->gl.Uniform1i(backend->u_map_y,2);
  backend->gl.Uniform1i(backend->u_target_height,(GLint)plan->target_height);

  for(uint32_t index=0;index<plan->operation_count;index++){
    const GmlPlanOp *op=&plan->operations[index];
    if(op->opcode==GML_PLAN_OP_SHADER_DRAW){
      if(!gml_gpu_opengl_execute_shader_draw(backend,plan,op,counters,error,error_capacity)){ failed=1; break; }
      continue;
    }
    if(op->opcode==GML_PLAN_OP_CLEAR_XRGB){
      gml_gpu_opengl_set_rect(backend,&op->destination,plan->target_height);
      /* Only fully black is executed here. Zero and one convert to a byte exactly on every
       * implementation; an arbitrary float colour does not, and the plan is refused rather than
       * accepting a value that two drivers would round differently. */
      backend->gl.ClearColor(0.0f,0.0f,0.0f,1.0f);
      backend->gl.Clear(GL_COLOR_BUFFER_BIT);
      continue;
    }
    {
      const GmlPlanImage *image=&plan->images[op->source];
      GmlPlanAxis axis_x=op->axis_x;
      GmlPlanAxis axis_y=op->axis_y;
      GLuint source_name=0,map_x=0,map_y=0;
      if(op->opcode==GML_PLAN_OP_PRESENT_CPU_FRAME){
        axis_x=identity_axis(image->width);
        axis_y=identity_axis(image->height);
      }
      if(!gml_gpu_opengl_upload_source(backend,image,counters,&source_name,op->linear?1:0)){
        gml_gpu_opengl_record_error(error,error_capacity,"graphics source upload failed");
        failed=1;
        break;
      }
      if(!gml_gpu_opengl_upload_axis(backend,index*2u,&axis_x,op->destination.width,counters,&map_x) ||
         !gml_gpu_opengl_upload_axis(backend,index*2u+1u,&axis_y,op->destination.height,counters,&map_y)){
        gml_gpu_opengl_record_error(error,error_capacity,"graphics axis map upload failed");
        failed=1;
        break;
      }
      gml_gpu_opengl_set_rect(backend,&op->destination,plan->target_height);
      backend->gl.Uniform1i(backend->u_dest_x,op->destination.x);
      backend->gl.Uniform1i(backend->u_dest_y,op->destination.y);
      backend->gl.ActiveTexture(GL_TEXTURE0);
      backend->gl.BindTexture(GL_TEXTURE_2D,source_name);
      backend->gl.ActiveTexture(GL_TEXTURE1);
      backend->gl.BindTexture(GL_TEXTURE_2D,map_x);
      backend->gl.ActiveTexture(GL_TEXTURE2);
      backend->gl.BindTexture(GL_TEXTURE_2D,map_y);
      backend->gl.DrawArrays(GL_TRIANGLES,0,3);
      if(counters) counters->draw_calls++;
    }
  }

  backend->gl.ActiveTexture(GL_TEXTURE2);
  backend->gl.BindTexture(GL_TEXTURE_2D,0);
  backend->gl.ActiveTexture(GL_TEXTURE1);
  backend->gl.BindTexture(GL_TEXTURE_2D,0);
  backend->gl.ActiveTexture(GL_TEXTURE0);
  backend->gl.BindTexture(GL_TEXTURE_2D,0);
  backend->gl.UseProgram(0);
  backend->gl.BindVertexArray(0);
  backend->gl.Disable(GL_SCISSOR_TEST);
  {
    GLenum status=backend->gl.GetError();
    if(status!=GL_NO_ERROR){
      gml_gpu_opengl_record_error(error,error_capacity,"graphics execution reported error 0x%04X",
                   (unsigned)status);
      while(backend->gl.GetError()!=GL_NO_ERROR){}
      failed=1;
    }
  }
  /* A pass that stopped part way has written something the plan does not describe. Report the
   * failure so the caller replays the complete pass, which overwrites it. */
  if(!failed && plan->target==GML_PLAN_TARGET_READBACK){
    if(!(backend->readback_pipelined
           ?gml_gpu_opengl_read_back_target_pipelined(backend,plan,error,error_capacity)
           :gml_gpu_opengl_read_back_target(backend,plan,error,error_capacity))) failed=1;
    /* The frontend's own framebuffer is what a pass leaves bound. */
    backend->gl.BindFramebuffer(GL_FRAMEBUFFER,(GLuint)framebuffer);
  }
  return failed?0:1;
}
