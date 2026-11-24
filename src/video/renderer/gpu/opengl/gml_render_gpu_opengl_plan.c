/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Existing OpenGL hybrid-plan executor. */
#include "gml_gpu_opengl_internal.h"

#include <string.h>

/* A frontend may lend a much larger texture than the published native picture.
 * On tiled devices a whole-attachment clear then visits that unpublished padding.
 * An unused second attachment bounds the framebuffer's effective extent, while
 * preserving the original colour attachment, clear, draw and completed-frame copy.
 * Keep one cleared pixel beyond the picture for the frontend's linear filtering. */
static int begin_host_extent(GmlGpuBackend *backend,uintptr_t framebuffer,
                             const GmlRenderPlan *plan){
  GmlGlApi *gl=&backend->gl;
  uint64_t width=(uint64_t)plan->target_width+1u,height=(uint64_t)plan->target_height+1u;
  if(!framebuffer || plan->target!=GML_PLAN_TARGET_HOST_FRAMEBUFFER ||
     plan->operation_count!=2u || plan->operations[0].opcode!=GML_PLAN_OP_CLEAR_XRGB ||
     (plan->operations[1].opcode!=GML_PLAN_OP_BLIT_OPAQUE_NEAREST &&
      plan->operations[1].opcode!=GML_PLAN_OP_BLIT_OPAQUE_SHARP_BILINEAR) ||
     !gl->InvalidateFramebuffer || !gl->ClearBufferfv || !gl->GetIntegerv ||
     !gl->GetFramebufferAttachmentParameteriv || !gl->GetTexLevelParameteriv ||
     !gl->GenRenderbuffers || !gl->BindRenderbuffer || !gl->RenderbufferStorage ||
     !gl->FramebufferRenderbuffer || !gl->DeleteRenderbuffers ||
     !plan->target_width || !plan->target_height || width>(4u*1024u*1024u)/height) return 0;
  const GmlPlanRect *clear=&plan->operations[0].destination;
  if(clear->x || clear->y || clear->width!=plan->target_width ||
     clear->height!=plan->target_height) return 0;
  /* Do not absorb a failure from an earlier operation into an optional refusal. */
  if(gl->GetError()!=GL_NO_ERROR) return -1;
  GLint type=0,draw=0,name=0,level=0,previous_texture=0;
  GLint host_width=0,host_height=0,previous_renderbuffer=0;
  gl->GetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT1,
                                         GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE,&type);
  gl->GetIntegerv(GL_DRAW_BUFFER1,&draw);
  if(gl->GetError()!=GL_NO_ERROR) goto refused;
  if(type || draw) return 0;
  gl->GetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,
                                         GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE,&type);
  if(gl->GetError()!=GL_NO_ERROR) goto refused;
  if(type!=GL_TEXTURE) return 0;
  gl->GetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,
                                         GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME,&name);
  gl->GetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,
                                         GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL,&level);
  if(gl->GetError()!=GL_NO_ERROR) goto refused;
  /* An array, cube or multisample attachment cannot be rebound as a 2D texture;
   * restore the borrowed binding and retain the ordinary path in that case. */
  gl->GetIntegerv(GL_TEXTURE_BINDING_2D,&previous_texture);
  gl->BindTexture(GL_TEXTURE_2D,(GLuint)name);
  if(gl->GetError()==GL_NO_ERROR){
    gl->GetTexLevelParameteriv(GL_TEXTURE_2D,level,GL_TEXTURE_WIDTH,&host_width);
    gl->GetTexLevelParameteriv(GL_TEXTURE_2D,level,GL_TEXTURE_HEIGHT,&host_height);
  }
  gl->BindTexture(GL_TEXTURE_2D,(GLuint)previous_texture);
  if(gl->GetError()!=GL_NO_ERROR) goto refused;
  if(host_width<(int64_t)plan->target_width || host_height<(int64_t)plan->target_height)
    return 0;
  uint64_t area=(uint64_t)plan->target_width*plan->target_height;
  if((uint64_t)host_width*(uint64_t)host_height<=area+area/4u) return 0;
  if(!backend->host_extent_renderbuffer || backend->host_extent_width!=width ||
     backend->host_extent_height!=height){
    GLuint next=0;
    gl->GetIntegerv(GL_RENDERBUFFER_BINDING,&previous_renderbuffer);
    gl->GenRenderbuffers(1,&next);
    if(!next) goto refused;
    gl->BindRenderbuffer(GL_RENDERBUFFER,next);
    gl->RenderbufferStorage(GL_RENDERBUFFER,GL_R8,(GLsizei)width,(GLsizei)height);
    gl->BindRenderbuffer(GL_RENDERBUFFER,(GLuint)previous_renderbuffer);
    if(gl->GetError()!=GL_NO_ERROR){gl->DeleteRenderbuffers(1,&next);goto refused;}
    if(backend->host_extent_renderbuffer)
      gl->DeleteRenderbuffers(1,&backend->host_extent_renderbuffer);
    backend->host_extent_renderbuffer=next;
    backend->host_extent_width=(uint32_t)width;
    backend->host_extent_height=(uint32_t)height;
  }
  gl->FramebufferRenderbuffer(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT1,GL_RENDERBUFFER,
                              backend->host_extent_renderbuffer);
  if(gl->CheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE &&
     gl->GetError()==GL_NO_ERROR){backend->host_extent_passes++;return 1;}
  gl->FramebufferRenderbuffer(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT1,GL_RENDERBUFFER,0);
refused:
  /* Only the optional setup ran since the clean error check above. Its failure
   * changes no pixels and leaves the existing executor available. */
  while(gl->GetError()!=GL_NO_ERROR){}
  return 0;
}

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
  backend->gl.Uniform1i(backend->u_rotation,0);
  backend->gl.Uniform1i(backend->u_area_filter,0);
  backend->gl.Uniform1i(backend->u_map_x,1);
  backend->gl.Uniform1i(backend->u_map_y,2);
  backend->gl.Uniform1i(backend->u_target_height,(GLint)target_height);
}

static void emergency_clear(GmlGlApi *gl,uint32_t color){
  const GLfloat rgb[]={
    (GLfloat)((color>>16)&255u)/255.0f,(GLfloat)((color>>8)&255u)/255.0f,
    (GLfloat)(color&255u)/255.0f,1.0f
  };
  gl->ClearBufferfv(GL_COLOR,0,rgb);
}

int gml_gpu_opengl_present_emergency_frame(GmlGpuBackend *backend,const GmlGpuContext *context,
                                          const uint32_t *pixels,uint32_t width,uint32_t height){
  GmlGlApi *gl=&backend->gl;
  if(!gl->ClearBufferfv || !gl->GetIntegerv || !gl->IsEnabled) return 0;
  GLint previous_fbo,previous_scissor[4],previous_mask[4];
  gl->GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&previous_fbo);
  gl->GetIntegerv(GL_SCISSOR_BOX,previous_scissor);
  gl->GetIntegerv(GL_COLOR_WRITEMASK,previous_mask);
  GLboolean scissor=gl->IsEnabled(GL_SCISSOR_TEST);
  GLboolean dither=gl->IsEnabled(GL_DITHER);
  GLboolean srgb=backend->api==GML_GPU_API_OPENGL_CORE && gl->IsEnabled(GL_FRAMEBUFFER_SRGB);

  gl->BindFramebuffer(GL_DRAW_FRAMEBUFFER,(GLuint)context->get_current_framebuffer(context->userdata));
  gl->Enable(GL_SCISSOR_TEST);
  gl->Disable(GL_DITHER);
  if(srgb) gl->Disable(GL_FRAMEBUFFER_SRGB);
  gl->ColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
  /* A terminal notice uses a few solid colours on a uniform background. Row runs use only the
   * framebuffer already lent by the host: no new program, texture, scratch plane or readback. */
  uint32_t background=pixels[0];
  gl->Scissor(0,0,(GLsizei)width,(GLsizei)height);
  emergency_clear(gl,background);
  for(uint32_t y=0;y<height;y++){
    const uint32_t *row=pixels+(size_t)y*width;
    for(uint32_t x=0;x<width;){
      uint32_t color=row[x],end=x+1;
      while(end<width && row[end]==color) end++;
      if(color!=background){
        gl->Scissor((GLint)x,(GLint)(height-y-1),(GLsizei)(end-x),1);
        emergency_clear(gl,color);
      }
      x=end;
    }
  }

  gl->BindFramebuffer(GL_DRAW_FRAMEBUFFER,(GLuint)previous_fbo);
  gl->Scissor(previous_scissor[0],previous_scissor[1],previous_scissor[2],previous_scissor[3]);
  gl->ColorMask(previous_mask[0]!=0,previous_mask[1]!=0,previous_mask[2]!=0,previous_mask[3]!=0);
  if(!scissor) gl->Disable(GL_SCISSOR_TEST);
  if(dither) gl->Enable(GL_DITHER);
  if(srgb) gl->Enable(GL_FRAMEBUFFER_SRGB);
  return gl->GetError()==GL_NO_ERROR;
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
  int bounded_host=begin_host_extent(backend,framebuffer,plan);
  if(bounded_host<0){
    gml_gpu_opengl_record_error(error,error_capacity,"graphics extent setup found an earlier error");
    return 0;
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
      int sharp=op->opcode==GML_PLAN_OP_BLIT_OPAQUE_SHARP_BILINEAR;
      int filtered=sharp || op->opcode==GML_PLAN_OP_BLIT_OPAQUE_BOX;
      int area=op->opcode==GML_PLAN_OP_BLIT_OPAQUE_AREA;
      GLint filter_destination[2]={filtered || area?(GLint)op->destination.width:0,
                                   filtered || area?(GLint)op->destination.height:0};
      if(op->opcode==GML_PLAN_OP_PRESENT_CPU_FRAME){
        axis_x=identity_axis(image->width);
        axis_y=identity_axis(image->height);
      }
      int identity=op->opcode==GML_PLAN_OP_BLIT_OPAQUE_NEAREST &&
        op->destination.width==image->width && op->destination.height==image->height &&
        axis_is_identity(&axis_x,image->width) && axis_is_identity(&axis_y,image->height);
      /* Keep the existing map cache and bindings; only the shader lookup is redundant.
       * Zero selects maps, positive values are box extents, and this tag selects identity. */
      if(identity)filter_destination[0]=filter_destination[1]=GL_PLAN_IDENTITY_AXES;
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
      } else if(image->device_completed){
        if(image->device_completed!=1u || !backend->completed_valid ||
           backend->completed_width!=image->width || backend->completed_height!=image->height ||
           backend->completed_generation!=image->content_generation){
          gml_gpu_opengl_record_error(error,error_capacity,
                                      "completed graphics source unavailable");
          failed=1;
          break;
        }
        source_name=backend->completed_texture;
        resident_source=1;
      } else if(!gml_gpu_opengl_upload_source(backend,image,counters,&source_name,op->linear?1:0)){
        gml_gpu_opengl_record_error(error,error_capacity,"graphics source upload failed");
        failed=1;
        break;
      }
      if(!filtered && !area &&
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
      backend->gl.Uniform1i(backend->u_sharp_scale,
                            sharp?(GLint)gml_plan_sharp_scale(&op->destination,image):0);
      backend->gl.Uniform2iv(backend->u_filter_destination,1,filter_destination);
      backend->gl.Uniform1i(backend->u_area_filter,area);
      backend->gl.ActiveTexture(GL_TEXTURE0);
      backend->gl.BindTexture(GL_TEXTURE_2D,source_name);
      if(filtered){
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
      if(filtered){
        backend->gl.ActiveTexture(GL_TEXTURE0);
        backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
        backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
      }
    }
  }

  if(bounded_host){
    backend->gl.BindFramebuffer(GL_FRAMEBUFFER,(GLuint)framebuffer);
    backend->gl.FramebufferRenderbuffer(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT1,GL_RENDERBUFFER,0);
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
int gml_render_gpu_opengl_present_transform(GmlGpuBackend *backend,uintptr_t framebuffer,
  uint32_t width,uint32_t height,uint64_t generation,int dx,int dy,unsigned rotation,int scratch,
  GmlGpuCounters *counters,char *error,size_t error_capacity){
  if(!backend || !backend->loaded || !backend->program || !width || !height ||
     width>GML_PLAN_MAX_EXTENT || height>GML_PLAN_MAX_EXTENT ||
     rotation>3u || (scratch!=0 && scratch!=1)) return 0;
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
  if(rotation){
    if(rotation&1u){ whole.width=height; whole.height=width; }
    written=whole;
  }else{
    GmlPlanAxis x=identity_axis(width),y=identity_axis(height);
    x.destination_first=dx<0?-dx:0;
    y.destination_first=dy<0?-dy:0;
    if(!gml_gpu_opengl_upload_axis(backend,0u,&x,written.width,counters,&map_x) ||
       !gml_gpu_opengl_upload_axis(backend,1u,&y,written.height,counters,&map_y)) return 0;
  }
  backend->gl.BindFramebuffer(GL_FRAMEBUFFER,(GLuint)framebuffer);
  begin_plan_state(backend,whole.height);
  if(!rotation){
    gml_gpu_opengl_set_rect(backend,&whole,whole.height);
    backend->gl.ClearColor(0.0f,0.0f,0.0f,1.0f);
    backend->gl.Clear(GL_COLOR_BUFFER_BIT);
  }
  gml_gpu_opengl_set_rect(backend,&written,whole.height);
  backend->gl.Uniform1i(backend->u_dest_x,rotation?dx:written.x);
  backend->gl.Uniform1i(backend->u_dest_y,rotation?dy:written.y);
  backend->gl.Uniform1i(backend->u_rotation,(GLint)rotation);
  backend->gl.Uniform1i(backend->u_source_height,(GLint)height);
  backend->gl.Uniform1i(backend->u_resident_source,1);
  backend->gl.Uniform1i(backend->u_sharp_scale,0);
  const GLint no_box[2]={0,0};
  backend->gl.Uniform2iv(backend->u_filter_destination,1,no_box);
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
    unsigned draws=1u;
    if(backend->api==GML_GPU_API_OPENGLES3)
      draws+=gml_gpu_opengl_prepare_fixed_scene_draws(backend,source_texture);
    if(counters) counters->draw_calls+=draws;
    prepared=gml_gpu_opengl_scene_wait(backend);
  }
  /* The ordinary bounded mirror cache owns the tiny source until eviction or context loss.
   * It retains only identity/generation, never the borrowed CPU pointer. */
  backend->gl.BindFramebuffer(GL_FRAMEBUFFER,(GLuint)frontend);
  backend->presentation_prepared=prepared;
  return prepared;
}
