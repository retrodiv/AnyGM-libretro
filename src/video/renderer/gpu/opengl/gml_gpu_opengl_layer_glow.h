/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef GML_GPU_OPENGL_LAYER_GLOW_H
#define GML_GPU_OPENGL_LAYER_GLOW_H

static unsigned layer_glow_quality(double quality){
  double rounded=floor(quality+0.5);
  return rounded<1.0?1u:rounded>16.0?16u:(unsigned)rounded;
}

static int layer_glow_supported(const GmlGpuBackend *backend,uint32_t w,uint32_t h,
                                const GmlRenderLayerEffect *effect){
  const double values[]={effect->glow.radius,effect->glow.quality,effect->glow.gamma,
    effect->glow.alpha,effect->intensity};
  for(unsigned i=0;i<5u;i++)if(!isfinite(values[i]) || fabs(values[i])>1000000.0)return 0;
  if(backend->scene.layer_glow.failed || backend->scene.max_texture_units<4 ||
     !backend->gl.BlitFramebuffer || effect->glow.preserve_alpha>1u ||
     !layer_pass_supported(backend,w,h,0.0))return 0;
  if(effect->intensity>0.0){
    unsigned quality=layer_glow_quality(effect->glow.quality);
    for(unsigned step=1;step<=quality;step++){
      float reach=fabsf((float)effect->glow.radius)*(float)step/(float)quality*0.5f;
      float radius=fmaxf(1.0f,floorf(reach+0.5f));
      if(!layer_pass_supported(backend,w,h,radius))return 0;
    }
  }
  return 1;
}

static int layer_glow_draw(GmlGpuBackend *backend,GlEffectSampleImage *target,
                           unsigned w,unsigned h,GLuint source,GLuint halo,
                           GLuint parent,GLuint peaks,const GLint size[2],GLint mode,
                           GLint flip,GLint halo_flip,const GmlRenderLayerEffect *effect){
  GlLayerGlow *glow=&backend->scene.layer_glow;
  const GLuint textures[]={source,halo?halo:source,parent?parent:source,peaks?peaks:source};
  backend->gl.BindFramebuffer(GL_FRAMEBUFFER,target->framebuffer);
  backend->gl.Viewport(0,0,(GLsizei)w,(GLsizei)h);
  backend->gl.Scissor(0,0,(GLsizei)w,(GLsizei)h);
  backend->gl.Enable(GL_SCISSOR_TEST);backend->gl.Disable(GL_BLEND);
  backend->gl.Disable(GL_DEPTH_TEST);backend->gl.Disable(GL_CULL_FACE);backend->gl.Disable(GL_DITHER);
  backend->gl.ColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
  for(unsigned i=0;i<4u;i++){
    backend->gl.ActiveTexture(GL_TEXTURE0+i);backend->gl.BindTexture(GL_TEXTURE_2D,textures[i]);
  }
  backend->gl.UseProgram(glow->program);
  backend->gl.Uniform1i(glow->source,0);backend->gl.Uniform1i(glow->halo,1);
  backend->gl.Uniform1i(glow->parent,2);backend->gl.Uniform1i(glow->peaks,3);
  backend->gl.Uniform2iv(glow->size,1,size);backend->gl.Uniform1i(glow->mode,mode);
  backend->gl.Uniform1i(glow->source_flip,flip);backend->gl.Uniform1i(glow->halo_flip,halo_flip);
  backend->gl.Uniform1i(glow->preserve,(GLint)effect->glow.preserve_alpha);
  GLfloat values[]={(float)effect->glow.alpha,(float)effect->glow.gamma,
    (float)(effect->intensity/sqrt((double)layer_glow_quality(effect->glow.quality)))};
  backend->gl.Uniform1fv(glow->alpha,1,&values[0]);
  backend->gl.Uniform1fv(glow->gamma,1,&values[1]);
  backend->gl.Uniform1fv(glow->scale,1,&values[2]);
  backend->gl.BindVertexArray(backend->vertex_array);
  backend->gl.DrawArrays(GL_TRIANGLES,0,3);
  return backend->gl.GetError()==GL_NO_ERROR;
}

/* All ordered writes stay private until one final whole-image publication. */
static int layer_glow_execute(GmlGpuBackend *backend,GlSceneImage *target,GlSceneImage *source,
                              const GmlRenderLayerEffect *effect,char *error,size_t capacity){
  GlLayerGlow *glow=&backend->scene.layer_glow;
  unsigned w=source->width,h=source->height;
  if(!layer_glow_supported(backend,w,h,effect) ||
     !gml_gpu_opengl_layer_glow_program(backend,error,capacity))return 0;
  uint64_t bytes=(uint64_t)w*h*8u+(uint64_t)((w+3u)/4u)*((h+3u)/4u)*8u;
  uint64_t other=layer_pass_retained_bytes(&backend->scene.layer_pass,0u)+
    layer_pass_image_bytes(&backend->scene.effect_sampler)+layer_pass_image_bytes(&backend->scene.effect_capture);
  if(bytes+other>512u*1024u*1024u){
    gml_gpu_opengl_effect_images_release(backend,&backend->scene.effect_sampler,1u);
    gml_gpu_opengl_effect_images_release(backend,&backend->scene.effect_capture,1u);
    if(bytes+layer_pass_retained_bytes(&backend->scene.layer_pass,0u)>512u*1024u*1024u)
      gml_gpu_opengl_effect_images_release(backend,backend->scene.layer_pass.images,6u);
  }
  for(unsigned i=0;i<4u;i++)
    if(!layer_pass_image(backend,&glow->images[i],i<2u?w:(w+3u)/4u,i<2u?h:(h+3u)/4u,GL_RGBA8))return 0;
  GLint size[2]={(GLint)w,(GLint)h};unsigned accumulator=0;
  glow->active=1;int result=0;
  if(!layer_glow_draw(backend,&glow->images[0],w,h,source->texture,0,target->texture,0,
       size,2,source->rendered_origin?0:1,0,effect))goto glow_done;
  unsigned quality=layer_glow_quality(effect->glow.quality);
  for(unsigned step=1;effect->intensity>0.0 && step<=quality;step++){
    float reach=fabsf((float)effect->glow.radius)*(float)step/(float)quality*0.5f;
    float radius=fmaxf(1.0f,floorf(reach+0.5f));GlSceneImage blurred={0};
    if(!layer_pass_blur(backend,source,radius,2u,&blurred,error,capacity))goto glow_done;
    unsigned rw=(w+3u)/4u,rh=(h+3u)/4u,current=2;
    if(!layer_glow_draw(backend,&glow->images[current],rw,rh,source->texture,blurred.texture,
         0,0,size,0,source->rendered_origin?0:1,blurred.rendered_origin?0:1,effect))goto glow_done;
    while(rw>1u || rh>1u){
      GLint previous[2]={(GLint)rw,(GLint)rh};unsigned next=5u-current;
      rw=(rw+3u)/4u;rh=(rh+3u)/4u;
      if(!layer_glow_draw(backend,&glow->images[next],rw,rh,glow->images[current].texture,
           0,0,0,previous,1,0,0,effect))goto glow_done;
      current=next;
    }
    unsigned next=1u-accumulator;
    if(!layer_glow_draw(backend,&glow->images[next],w,h,source->texture,blurred.texture,
         glow->images[accumulator].texture,glow->images[current].texture,size,3,
         source->rendered_origin?0:1,blurred.rendered_origin?0:1,effect))goto glow_done;
    accumulator=next;
  }
  backend->gl.BindFramebuffer(GL_READ_FRAMEBUFFER,glow->images[accumulator].framebuffer);
  backend->gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER,target->framebuffer);
  backend->gl.Scissor(0,0,(GLsizei)w,(GLsizei)h);
  target->alpha_opaque=target->alpha_nonzero=target->alpha_bounds_known=0;
  backend->gl.BlitFramebuffer(0,0,(GLint)w,(GLint)h,0,0,(GLint)w,(GLint)h,GL_COLOR_BUFFER_BIT,GL_NEAREST);
  result=backend->gl.GetError()==GL_NO_ERROR?1:-1;
glow_done:
  glow->active=0;return result;
}
#endif
