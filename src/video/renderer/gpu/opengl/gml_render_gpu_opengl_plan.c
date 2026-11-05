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

enum { GL_PLAN_IDENTITY_AXES=-1 };

static int axis_is_identity(const GmlPlanAxis *axis,uint32_t extent){
  if(axis->source_extent!=extent || axis->destination_extent!=extent ||
     axis->destination_first) return 0;
  switch(axis->rule){
    case GML_PLAN_AXIS_ACCUMULATOR:
    case GML_PLAN_AXIS_LEADING_EDGE: return 1;
    case GML_PLAN_AXIS_PIXEL_CENTRE: return axis->origin==0.0 && axis->extent==(double)extent;
    case GML_PLAN_AXIS_COMPOSED_NEAREST:
    case GML_PLAN_AXIS_COMPOSED_LEADING_NEAREST: return axis->intermediate_extent==extent;
    default: return 0;
  }
}

static void begin_plan_state(GmlGpuBackend *backend,uint32_t target_height){
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
  backend->gl.Uniform1i(backend->u_rgb_query,0);
  backend->gl.Uniform1i(backend->u_map_x,1);
  backend->gl.Uniform1i(backend->u_map_y,2);
  backend->gl.Uniform1i(backend->u_target_height,(GLint)target_height);
}

int gml_gpu_opengl_execute(GmlGpuBackend *backend,const GmlGpuContext *context,
                       GmlRenderPlan *plan,GmlGpuCounters *counters,
                       char *error,size_t error_capacity){
  uintptr_t framebuffer;
  int failed=0,discarded_host_colour=0;
  uint64_t identity_draws=0;
  if(!backend || !backend->loaded || !context || !plan) return 0;
  backend->readback_present_valid=0;
  framebuffer=context->get_current_framebuffer(context->userdata);
  if(plan->target==GML_PLAN_TARGET_READBACK || plan->target==GML_PLAN_TARGET_DEVICE_PRESENTATION){
    if(!gml_gpu_opengl_bind_readback_target(backend,plan->target_width,plan->target_height,error,error_capacity))
      return 0;
  } else {
    /* Handle zero is a valid target: some frontends hand over the default framebuffer. */
    backend->gl.BindFramebuffer(GL_FRAMEBUFFER,(GLuint)framebuffer);
  }
  /* A full initial clear overwrites every published pixel. Discard the obsolete attachment
   * before it, so tiled devices need not load a larger frontend allocation's previous frame.
   * This does not discard the completed-frame texture or change announced geometry. */
  if(plan->target==GML_PLAN_TARGET_HOST_FRAMEBUFFER && backend->gl.InvalidateFramebuffer &&
     plan->operation_count && plan->operations[0].opcode==GML_PLAN_OP_CLEAR_XRGB){
    const GmlPlanRect *rect=&plan->operations[0].destination;
    if(rect->x==0 && rect->y==0 && rect->width==plan->target_width &&
       rect->height==plan->target_height){
      GLenum attachment=framebuffer?GL_COLOR_ATTACHMENT0:GL_COLOR;
      backend->gl.InvalidateFramebuffer(GL_FRAMEBUFFER,1,&attachment);
      discarded_host_colour=framebuffer!=0u;
    }
  }
  begin_plan_state(backend,plan->target_height);

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
      if(index==0u && discarded_host_colour && backend->gl.ClearBufferfv){
        /* Padding outside the published frame was discarded above. Initialize that obsolete
         * storage with the same clear value as the frame, allowing a whole-attachment fast clear.
         * Only draw buffer zero is initialized; later draws retain their original placement. */
        static const GLfloat black[4]={0.0f,0.0f,0.0f,1.0f};
        backend->gl.Disable(GL_SCISSOR_TEST);
        backend->gl.ClearBufferfv(GL_COLOR,0,black);
        backend->gl.Enable(GL_SCISSOR_TEST);
      }else backend->gl.Clear(GL_COLOR_BUFFER_BIT);
      continue;
    }
    {
      const GmlPlanImage *image=&plan->images[op->source];
      GmlPlanAxis axis_x=op->axis_x;
      GmlPlanAxis axis_y=op->axis_y;
      GLuint source_name=0,map_x=0,map_y=0;
      int resident_source=0;
      int box=op->opcode==GML_PLAN_OP_BLIT_OPAQUE_BOX;
      GLint box_destination[2]={box?(GLint)op->destination.width:0,
                                box?(GLint)op->destination.height:0};
      if(op->opcode==GML_PLAN_OP_PRESENT_CPU_FRAME){
        axis_x=identity_axis(image->width);
        axis_y=identity_axis(image->height);
      }
      int identity=op->opcode==GML_PLAN_OP_BLIT_OPAQUE_NEAREST &&
        op->destination.width==image->width && op->destination.height==image->height &&
        axis_is_identity(&axis_x,image->width) && axis_is_identity(&axis_y,image->height);
      /* Keep the existing map cache and bindings; only the shader lookup is redundant.
       * Zero selects maps, positive values are box extents, and this tag selects identity. */
      if(identity)box_destination[0]=box_destination[1]=GL_PLAN_IDENTITY_AXES;
      if(image->resident_image.resource_class){
        GlSceneImage *scene=gml_gpu_opengl_scene_image(backend,image->resident_image);
        if(!scene || scene->width!=image->width || scene->height!=image->height ||
           scene->pixel_format!=GML_RENDER_PIXEL_ARGB8888 || !scene->rendered_origin){
          gml_gpu_opengl_record_error(error,error_capacity,"resident graphics source unavailable");
          failed=1;
          break;
        }
        source_name=scene->texture;
        resident_source=1;
      } else if(!gml_gpu_opengl_upload_source(backend,image,counters,&source_name,op->linear?1:0)){
        gml_gpu_opengl_record_error(error,error_capacity,"graphics source upload failed");
        failed=1;
        break;
      }
      if(!box &&
         (!gml_gpu_opengl_upload_axis(backend,index*2u,&axis_x,op->destination.width,counters,&map_x) ||
          !gml_gpu_opengl_upload_axis(backend,index*2u+1u,&axis_y,op->destination.height,counters,&map_y))){
        gml_gpu_opengl_record_error(error,error_capacity,"graphics axis map upload failed");
        failed=1;
        break;
      }
      gml_gpu_opengl_set_rect(backend,&op->destination,plan->target_height);
      backend->gl.Uniform1i(backend->u_dest_x,op->destination.x);
      backend->gl.Uniform1i(backend->u_dest_y,op->destination.y);
      backend->gl.Uniform1i(backend->u_source_height,(GLint)image->height);
      backend->gl.Uniform1i(backend->u_resident_source,resident_source);
      backend->gl.Uniform2iv(backend->u_box_destination,1,box_destination);
      backend->gl.ActiveTexture(GL_TEXTURE0);
      backend->gl.BindTexture(GL_TEXTURE_2D,source_name);
      if(box){
        backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
        backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
      }
      backend->gl.ActiveTexture(GL_TEXTURE1);
      backend->gl.BindTexture(GL_TEXTURE_2D,map_x);
      backend->gl.ActiveTexture(GL_TEXTURE2);
      backend->gl.BindTexture(GL_TEXTURE_2D,map_y);
      backend->gl.DrawArrays(GL_TRIANGLES,0,3);
      if(identity)identity_draws++;
      if(counters) counters->draw_calls++;
      if(box){
        backend->gl.ActiveTexture(GL_TEXTURE0);
        backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
        backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
      }
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
  if(plan->target==GML_PLAN_TARGET_DEVICE_PRESENTATION){
    backend->readback_present_valid=!failed;
    backend->gl.BindFramebuffer(GL_FRAMEBUFFER,(GLuint)framebuffer);
  }
  if(!failed)backend->identity_map_draws+=identity_draws;
  return failed?0:1;
}

/* R03-PRESENT-INTEGER-SHIFT: an immutable complete picture is sampled at integer
 * indices. The canonical cache is never a target of this delivery-only pass. */
int gml_render_gpu_opengl_present_shift(GmlGpuBackend *backend,uintptr_t framebuffer,
  uint32_t width,uint32_t height,uint64_t generation,int dx,int dy,int scratch,
  GmlGpuCounters *counters,char *error,size_t error_capacity){
  if(!backend || !backend->loaded || !backend->program || !width || !height ||
     width>GML_PLAN_MAX_EXTENT || height>GML_PLAN_MAX_EXTENT ||
     (scratch!=0 && scratch!=1)) return 0;
  GLuint source=0,map_x=0,map_y=0;
  if(scratch){
    if(!backend->readback_present_valid || backend->readback_width!=width ||
       backend->readback_height!=height) return 0;
    source=backend->readback_texture;
  }else{
    if(!backend->completed_valid || backend->completed_width!=width ||
       backend->completed_height!=height || backend->completed_generation!=generation) return 0;
    source=backend->completed_texture;
  }
  if(!source) return 0;
  /* The CPU owner leaves the picture unshifted when either axis is outside it. */
  if((int64_t)dx>=width || (int64_t)dx<=-(int64_t)width ||
     (int64_t)dy>=height || (int64_t)dy<=-(int64_t)height) dx=dy=0;
  uint32_t ax=(uint32_t)(dx<0?-dx:dx),ay=(uint32_t)(dy<0?-dy:dy);
  GmlPlanRect whole={0,0,width,height};
  GmlPlanRect written={dx>0?dx:0,dy>0?dy:0,width-ax,height-ay};
  GmlPlanAxis x=identity_axis(width),y=identity_axis(height);
  x.destination_first=dx<0?-dx:0;y.destination_first=dy<0?-dy:0;
  if(!gml_gpu_opengl_upload_axis(backend,0u,&x,written.width,counters,&map_x) ||
     !gml_gpu_opengl_upload_axis(backend,1u,&y,written.height,counters,&map_y)) return 0;
  backend->gl.BindFramebuffer(GL_FRAMEBUFFER,(GLuint)framebuffer);
  begin_plan_state(backend,height);
  gml_gpu_opengl_set_rect(backend,&whole,height);
  backend->gl.ClearColor(0.0f,0.0f,0.0f,1.0f);
  backend->gl.Clear(GL_COLOR_BUFFER_BIT);
  gml_gpu_opengl_set_rect(backend,&written,height);
  backend->gl.Uniform1i(backend->u_dest_x,written.x);
  backend->gl.Uniform1i(backend->u_dest_y,written.y);
  backend->gl.Uniform1i(backend->u_source_height,(GLint)height);
  backend->gl.Uniform1i(backend->u_resident_source,1);
  const GLint no_box[2]={0,0};
  backend->gl.Uniform2iv(backend->u_box_destination,1,no_box);
  backend->gl.ActiveTexture(GL_TEXTURE0);backend->gl.BindTexture(GL_TEXTURE_2D,source);
  backend->gl.ActiveTexture(GL_TEXTURE1);backend->gl.BindTexture(GL_TEXTURE_2D,map_x);
  backend->gl.ActiveTexture(GL_TEXTURE2);backend->gl.BindTexture(GL_TEXTURE_2D,map_y);
  backend->gl.DrawArrays(GL_TRIANGLES,0,3);
  if(counters)counters->draw_calls++;
  backend->gl.BindTexture(GL_TEXTURE_2D,0);
  backend->gl.ActiveTexture(GL_TEXTURE1);backend->gl.BindTexture(GL_TEXTURE_2D,0);
  backend->gl.ActiveTexture(GL_TEXTURE0);backend->gl.BindTexture(GL_TEXTURE_2D,0);
  backend->gl.UseProgram(0);backend->gl.BindVertexArray(0);
  backend->gl.Disable(GL_SCISSOR_TEST);
  if(scratch)backend->readback_present_valid=0;
  GLenum status=backend->gl.GetError();
  if(status!=GL_NO_ERROR){
    gml_gpu_opengl_record_error(error,error_capacity,"integer presentation failed 0x%04X",(unsigned)status);
    while(backend->gl.GetError()!=GL_NO_ERROR){}
    return 0;
  }
  return 1;
}

static uintptr_t preparation_framebuffer(void *userdata){
  return *(const uintptr_t*)userdata;
}

int gml_gpu_opengl_prepare_presentation(GmlGpuBackend *backend,const GmlGpuContext *context,
                                        GmlGpuCounters *counters,char *error,size_t error_capacity){
  const uint32_t pixel=0xFF102030u;
  GmlRenderPlan plan={0};
  uintptr_t frontend,private_target;
  GmlGpuContext private_context;
  GLuint source_texture=0;
  int prepared;
  if(!backend || !backend->loaded || !context || !context->get_current_framebuffer) return 0;
  if(backend->presentation_prepared) return 1;
  frontend=context->get_current_framebuffer(context->userdata);
  backend->gl.BindBuffer(GL_PIXEL_UNPACK_BUFFER,0);
  if(!gml_gpu_opengl_bind_readback_target(backend,1u,1u,error,error_capacity)){
    backend->gl.BindFramebuffer(GL_FRAMEBUFFER,(GLuint)frontend);
    return 0;
  }
  private_target=backend->readback_framebuffer;
  private_context=*context;
  private_context.userdata=&private_target;
  private_context.get_current_framebuffer=preparation_framebuffer;
  plan.target=GML_PLAN_TARGET_HOST_FRAMEBUFFER;
  plan.target_width=plan.target_height=1u;
  plan.image_count=plan.operation_count=1u;
  /* NONE cannot alias a real plan image. No borrowed address survives the synchronous upload. */
  plan.images[0]=(GmlPlanImage){.image_class=GML_PLAN_IMAGE_NONE,.width=1u,.height=1u,
    .pitch_pixels=1u,.opaque=1u,.cpu_pixels=&pixel};
  plan.operations[0]=(GmlPlanOp){.opcode=GML_PLAN_OP_PRESENT_CPU_FRAME,
    .destination={0,0,1u,1u},.alpha_write=255u};
  /* Linking alone may leave expensive driver compilation until the first draw. Execute the
   * preserved terminal and automatic-composition writers privately, and retire their work during
   * preparation, never per frame.
   * HOST_FRAMEBUFFER selects execution without the READBACK target's CPU observation. */
  prepared=gml_gpu_opengl_execute(backend,&private_context,&plan,counters,error,error_capacity) &&
           gml_gpu_opengl_upload_source(backend,&plan.images[0],counters,&source_texture,0) &&
           gml_gpu_opengl_prepare_automatic_draw(backend,source_texture,error,error_capacity);
  if(prepared){
    if(counters) counters->draw_calls++;
    prepared=gml_gpu_opengl_scene_wait(backend);
  }
  /* The ordinary bounded mirror cache owns the tiny source until eviction or context loss.
   * It retains only identity/generation, never the borrowed CPU pointer. */
  backend->gl.BindFramebuffer(GL_FRAMEBUFFER,(GLuint)frontend);
  backend->presentation_prepared=prepared;
  return prepared;
}
