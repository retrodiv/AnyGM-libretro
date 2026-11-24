/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef GML_GPU_OPENGL_LAYER_PASS_H
#define GML_GPU_OPENGL_LAYER_PASS_H

/* Whole-image, byte-quantized box axes. Reduced block-prefix planes keep both
 * storage and per-fragment work bounded independently of the kernel radius. */
static uint64_t layer_pass_image_bytes(const GlEffectSampleImage *image){
  return image->texture?(uint64_t)image->width*image->height*
    (image->format==GL_RGBA32F?16u:4u):0u;
}

static uint64_t layer_pass_retained_bytes(const GlLayerPass *pass,unsigned first){
  uint64_t bytes=0;
  for(unsigned i=first;i<6u;i++)bytes+=layer_pass_image_bytes(&pass->images[i]);
  return bytes;
}

static uint64_t layer_glow_retained_bytes(const GlLayerGlow *glow){
  uint64_t bytes=0;
  for(unsigned i=0;i<4u;i++)bytes+=layer_pass_image_bytes(&glow->images[i]);
  return bytes;
}

static int layer_pass_supported(const GmlGpuBackend *backend,uint32_t width,
                                uint32_t height,double radius){
  if(!width || !height || width>65536u || height>65536u ||
     (uint64_t)width*height>16u*1024u*1024u ||
     width>(uint32_t)backend->scene.max_texture_size ||
     height>(uint32_t)backend->scene.max_texture_size ||
     !isfinite(radius) || fabs(radius)>1000000.0 || backend->scene.max_texture_units<2)return 0;
  if(floorf(fabsf((float)radius)+0.5f)==0.0f)return 1;
  if(backend->scene.layer_pass.failed)return 0;
  if(floorf(fabsf((float)radius)+0.5f)<=7.0f)return 1;
  if(backend->api==GML_GPU_API_OPENGLES3 &&
     !layer_effect_extension((const char*)backend->gl.GetString(GL_EXTENSIONS),
       "GL_EXT_color_buffer_float"))return 0;
  uint64_t pixels=(uint64_t)width*height;
  uint64_t blocks=((uint64_t)(width+7u)/8u)*height+
    (uint64_t)width*((height+7u)/8u);
  return pixels*8u+blocks*32u<=512u*1024u*1024u;
}

static int layer_pass_image(GmlGpuBackend *backend,GlEffectSampleImage *image,
                            uint32_t width,uint32_t height,GLenum format){
  if(!image->texture)backend->gl.GenTextures(1,&image->texture);
  if(!image->framebuffer)backend->gl.GenFramebuffers(1,&image->framebuffer);
  if(!image->texture || !image->framebuffer)return 0;
  backend->gl.ActiveTexture(GL_TEXTURE0);
  backend->gl.BindTexture(GL_TEXTURE_2D,image->texture);
  if(image->width!=width || image->height!=height || image->format!=format){
    image->valid=0;
    backend->gl.TexImage2D(GL_TEXTURE_2D,0,(GLint)format,(GLsizei)width,(GLsizei)height,
      0,GL_RGBA,format==GL_RGBA32F?GL_FLOAT:GL_UNSIGNED_BYTE,NULL);
    if(backend->gl.GetError()!=GL_NO_ERROR){
      gml_gpu_opengl_effect_images_release(backend,image,1u);return 0;
    }
    image->width=width;image->height=height;image->format=format;
  }
  backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
  backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
  backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
  backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
  backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_BASE_LEVEL,0);
  backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAX_LEVEL,0);
  backend->gl.BindFramebuffer(GL_FRAMEBUFFER,image->framebuffer);
  backend->gl.FramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,image->texture,0);
  return backend->gl.CheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE &&
    backend->gl.GetError()==GL_NO_ERROR;
}

static int layer_pass_draw(GmlGpuBackend *backend,GlEffectSampleImage *target,
                           GLuint source,GLuint prefix,const GLint size[2],
                           GLint axis,GLint mode,GLint step,GLint radius,GLint flip){
  GlLayerPass *pass=&backend->scene.layer_pass;
  backend->gl.BindFramebuffer(GL_FRAMEBUFFER,target->framebuffer);
  backend->gl.Viewport(0,0,(GLsizei)target->width,(GLsizei)target->height);
  backend->gl.Scissor(0,0,(GLsizei)target->width,(GLsizei)target->height);
  backend->gl.Enable(GL_SCISSOR_TEST);
  backend->gl.Disable(GL_BLEND);backend->gl.Disable(GL_DEPTH_TEST);
  backend->gl.Disable(GL_CULL_FACE);backend->gl.Disable(GL_DITHER);
  backend->gl.ColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
  backend->gl.ActiveTexture(GL_TEXTURE0);backend->gl.BindTexture(GL_TEXTURE_2D,source);
  backend->gl.ActiveTexture(GL_TEXTURE1);backend->gl.BindTexture(GL_TEXTURE_2D,prefix?prefix:source);
  backend->gl.UseProgram(pass->program);
  backend->gl.Uniform1i(pass->source,0);backend->gl.Uniform1i(pass->prefix,1);
  backend->gl.Uniform2iv(pass->size,1,size);
  backend->gl.Uniform1i(pass->axis,axis);backend->gl.Uniform1i(pass->mode,mode);
  backend->gl.Uniform1i(pass->step,step);backend->gl.Uniform1i(pass->radius,radius);
  backend->gl.Uniform1i(pass->flip,flip);
  backend->gl.BindVertexArray(backend->vertex_array);
  backend->gl.DrawArrays(GL_TRIANGLES,0,3);
  return backend->gl.GetError()==GL_NO_ERROR;
}

/* No parent write occurs here. Failure leaves only disposable images changed. */
static int layer_pass_blur(GmlGpuBackend *backend,GlSceneImage *source,double radius,
                           unsigned pairs,GlSceneImage *result,char *error,size_t capacity){
  GlLayerPass *pass=&backend->scene.layer_pass;
  uint32_t w=source->width,h=source->height;
  if(!layer_pass_supported(backend,w,h,radius) || !pairs || pairs>3u)return 0;
  int r=(int)floorf(fabsf((float)radius)+0.5f);
  if(!r){*result=*source;return 1;}
  uint64_t pixels=(uint64_t)w*h;
  uint64_t blocks=(uint64_t)((w+7u)/8u)*h+(uint64_t)w*((h+7u)/8u);
  uint64_t bytes=pixels*8u+(r<=7?layer_pass_retained_bytes(pass,2u):blocks*32u);
  uint64_t sampled=layer_pass_image_bytes(&backend->scene.effect_sampler)+
    layer_pass_image_bytes(&backend->scene.effect_capture);
  uint64_t glow_bytes=layer_glow_retained_bytes(&backend->scene.layer_glow);
  if(!backend->scene.layer_glow.active && bytes+sampled+glow_bytes>512u*1024u*1024u){
    gml_gpu_opengl_effect_images_release(backend,backend->scene.layer_glow.images,4u);
    glow_bytes=0;
  }
  /* Keep warmed images across ordinary family changes. Evict only when the
   * new allocation plan would exceed the combined private-image budget. */
  if(r<=7 && bytes+sampled+glow_bytes>512u*1024u*1024u){
    gml_gpu_opengl_effect_images_release(backend,&pass->images[2],4u);
    bytes=pixels*8u;
  }
  if(bytes+sampled+glow_bytes>512u*1024u*1024u){
    gml_gpu_opengl_effect_images_release(backend,&backend->scene.effect_sampler,1u);
    gml_gpu_opengl_effect_images_release(backend,&backend->scene.effect_capture,1u);
  }
  if(bytes+glow_bytes>512u*1024u*1024u)return 0;
  if(!gml_gpu_opengl_layer_pass_program(backend,error,capacity))return 0;
  for(unsigned i=0;i<(r<=7?2u:6u);i++){
    uint32_t width=i<2u?w:i<4u?(w+7u)/8u:w;
    uint32_t height=i<4u?h:(h+7u)/8u;
    if(!layer_pass_image(backend,&pass->images[i],width,height,i<2u?GL_RGBA8:GL_RGBA32F))return 0;
  }
  GLint size[2]={(GLint)w,(GLint)h};GLuint input=source->texture;
  GLint flip=source->rendered_origin?0:1;
  for(unsigned pair=0;pair<pairs;pair++)for(unsigned axis=0;axis<2u;axis++){
    if(r<=7){
      if(!layer_pass_draw(backend,&pass->images[axis],input,0,size,(GLint)axis,3,0,r,flip))return 0;
      input=pass->images[axis].texture;flip=0;continue;
    }
    GlEffectSampleImage *prefix=&pass->images[axis?4:2];
    unsigned current=0,count=(axis?h+7u:w+7u)/8u;
    if(!layer_pass_draw(backend,&prefix[current],input,0,size,(GLint)axis,0,0,r,flip))return 0;
    for(unsigned step=1;step<count;step*=2u){
      unsigned next=1u-current;
      if(!layer_pass_draw(backend,&prefix[next],prefix[current].texture,0,size,
                           (GLint)axis,1,(GLint)step,r,0))return 0;
      current=next;
    }
    if(!layer_pass_draw(backend,&pass->images[axis],input,prefix[current].texture,
                         size,(GLint)axis,2,0,r,flip))return 0;
    input=pass->images[axis].texture;flip=0;
  }
  *result=(GlSceneImage){.texture=pass->images[1].texture,
    .framebuffer=pass->images[1].framebuffer,.width=w,.height=h,
    .pixel_format=GML_RENDER_PIXEL_ARGB8888,.rendered_origin=1,.valid=1};
  return 1;
}
#endif
