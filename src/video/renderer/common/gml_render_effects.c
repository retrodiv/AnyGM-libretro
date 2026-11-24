/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Room-layer effect capture and pass-level CPU execution request. */
#include "gml_render.h"
#include "gml_render_internal.h"
#include "gml_render_cpu_effects.h"
#include "gml_render_surfaces_internal.h"
#include "anygm_host.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>


static int layer_filter_reserve(GmlRender *r,size_t count){
  if(!r || count==0 || count>67108864u) return 0;
  if(count<=r->layer_filter_capacity && r->layer_filter_src &&
     r->layer_filter_work && r->layer_filter_aux) return 1;
  uint32_t *src=malloc(count*sizeof(*src));
  uint32_t *work=malloc(count*sizeof(*work));
  uint32_t *aux=malloc(count*sizeof(*aux));
  if(!src || !work || !aux){ free(src); free(work); free(aux); return 0; }
  gml_render_layer_capture_dispose(r);
  free(r->layer_filter_src); free(r->layer_filter_work); free(r->layer_filter_aux);
  r->layer_filter_src=src; r->layer_filter_work=work; r->layer_filter_aux=aux;
  r->layer_filter_capacity=count;
  return 1;
}

/* R03-P7-LAYER-EFFECT-SUBMIT: captures provide a resident source or a CPU checkpoint;
 * inline effects consume the immutable snapshot of their current device target. */
static int layer_effect_submit_gpu(GmlRender *r,const uint32_t *source_pixels,
                                   GmlRenderResource *source_resource,
                                   const GmlRenderResourceSource *auxiliary,
                                   const GmlRenderLayerEffect *effect,
                                   int width,int height){
  GmlRenderResource *target;
  GmlRenderResourceSource source={0},sources[2]={0};
  GmlRenderImageRef refs[2]={0};
  GmlRenderImageRef source_ref={0},image;
  GmlRenderDrawOp operation={0};
  GmlRenderDrawStateSnapshot state={0};
  GmlRenderDrawBatch batch;
  GmlRenderResourceExecution result;
  GmlRenderDeviceCaps caps={0};
  GmlRenderJournalMetrics metrics={0};
  const GmlRenderDeviceServices *device=gml_render_device(r);
  size_t bytes;
  int application,in_place=effect && gml_render_layer_effect_is_inline(effect->kind);
  if(!device || !device->capabilities ||
     !device->capabilities(device->context,&caps) || !caps.max_texture_extent ||
     width<=0 || height<=0 || (uint32_t)width>caps.max_texture_extent ||
     (uint32_t)height>caps.max_texture_extent ||
     (size_t)width>SIZE_MAX/4u/(size_t)height ||
     (size_t)width*(size_t)height>16u*1024u*1024u ||
     (!in_place && !source_pixels && !source_resource) || !r->fb || !effect || !isfinite(effect->intensity)) return 0;
  /* The shared scene matcher also accepts numbered targets. After capture its
   * resident flag is cleared, so the application submitter would select the
   * owned application resource instead of this restored numbered destination. */
  application=r->target_id<=0 && gml_render_scene_target_matches(r);
  if(!application && !(r->target_id>0 && r->target_id<=GML_MAX_SURFACES &&
     r->surface[r->target_id-1].live &&
     r->fb==r->surface[r->target_id-1].px &&
     r->fbw==r->surface[r->target_id-1].w &&
     r->fbh==r->surface[r->target_id-1].h)) return 0;
  target=application?gml_render_scene_target_resource_ensure(r):
    gml_render_surface_resource_ensure(r,r->target_id);
  if(!target) return 0;
  image=gml_render_resource_image_ref(target);
  if(image.content_version==UINT64_MAX) return 0;
  if(!in_place && source_resource){
    if(!gml_render_resource_prepare_device_source(source_resource,device,&source))return 0;
    source_ref=source.image;
  }else if(!in_place){
    source_ref=(GmlRenderImageRef){GML_RENDER_RESOURCE_SCRATCH,1u,
      gml_render_scene_incarnation_allocate(),1u};
    if(!source_ref.incarnation) return 0;
    bytes=(size_t)width*(size_t)height*4u;
    source.image=source_ref;
    source.width=(uint32_t)width;
    source.height=(uint32_t)height;
    source.pixel_format=GML_RENDER_PIXEL_ARGB8888;
    source.stride_bytes=(uint32_t)width*4u;
    source.byte_capacity=bytes;
    source.bytes=source_pixels;
  }
  operation.opcode=GML_RENDER_OP_LAYER_EFFECT;
  operation.scene_family=GML_RENDER_SCENE_EFFECT;
  operation.reconstruction_mode=GML_RENDER_DEVICE_CHECKPOINT;
  sources[0]=source;refs[0]=source_ref;
  if(auxiliary){sources[1]=*auxiliary;refs[1]=auxiliary->image;}
  operation.source_count=in_place?0u:auxiliary?2u:1u;
  operation.sources=in_place?NULL:refs;
  operation.target.image=image;
  operation.target.image.content_version++;
  operation.target.storage_version=1u;
  operation.target.width=(uint32_t)width;
  operation.target.height=(uint32_t)height;
  operation.target.pixel_format=GML_RENDER_PIXEL_ARGB8888;
  operation.target.region=(GmlRenderRect){0,0,(uint32_t)width,(uint32_t)height};
  operation.payload.effect=*effect;
  state.write_mask=15u;
  state.colour=0xFFFFFFFFu;
  state.alpha=255u;
  if(!in_place){
    state.sampler_count=auxiliary?2u:1u;
    state.samplers[0].filter=GML_RENDER_FILTER_NEAREST;
    state.samplers[0].edge_rule=GML_RENDER_EDGE_CLAMP_SUBIMAGE;
    state.samplers[0].source_region=operation.target.region;
    if(auxiliary){
      state.samplers[1].filter=GML_RENDER_FILTER_BILINEAR;
      state.samplers[1].edge_rule=GML_RENDER_EDGE_REPEAT_SUBIMAGE;
      state.samplers[1].source_region=effect->sampler.source_region;
    }
  }
  batch=(GmlRenderDrawBatch){&operation,1u,&state,1u};
  if(in_place || source_resource){
    /* Raw target effects ignore the content draw preset, palette and shader.
     * The packet describes their complete effective state and selected owner. */
    result=gml_render_resource_submit_deferred(target,device,&batch,
      in_place?NULL:sources,operation.source_count);
    gml_render_dispatch_batch_record(r,&batch,result);
    if(result==GML_RENDER_RESOURCE_EXEC_DEVICE && r->fb==r->app_surface_owned){
      r->app_surface_opaque=0;r->app_presentation_coverage_active=0;
    }
  }else{
    result=application?gml_render_application_scene_submit(r,&batch,sources,operation.source_count):
      gml_render_surface_scene_submit(r,r->target_id,&batch,sources,operation.source_count);
    if(device->retire) device->retire(device->context,source_ref);
  }
  if(result==GML_RENDER_RESOURCE_EXEC_DEVICE){
    /* The source capture crossed the CPU barrier, but the accepted effect
     * makes this target device-current again. Re-enter the
     * resident draw contract so a later CPU-only draw crosses its write
     * barrier and invalidates that device version. */
    if(gml_render_scene_target_matches(r)) r->resident_application_pass=1;
    if(!in_place)r->fb_opaque_known=0;
    else if(effect->kind==GML_RENDER_LAYER_EFFECT_INLINE_TINT && (effect->colour>>24)<255u)
      r->fb_opaque_known=r->fb_all_opaque=0;
    r->fb_all_transparent=0;
    return 1;
  }
  gml_render_resource_journal_metrics(target,&metrics);
  return metrics.authority==GML_RENDER_RESOURCE_INVALID?-1:0;
}

/* Resolve only immutable asset bytes here. The device prepares logical trim
 * and repeat storage; no scene pixels are evaluated or copied on the CPU. */
static int layer_effect_sampler_source(GmlRender *r,int id,GmlRenderLayerSampler *sampler,
                                       GmlRenderResourceSource *source){
  if(id<0 || id>=r->n_spr)return 0;
  GmlSprite *sprite=&r->spr[id];
  if(sprite->w<=0 || sprite->h<=0 || sprite->w>4096 || sprite->h>4096 || sprite->n_frames<=0)return 0;
  sampler->width=(uint32_t)sprite->w;sampler->height=(uint32_t)sprite->h;
  if(sprite->runtime_rgba){
    sampler->source_region=(GmlRenderRect){0,0,sampler->width,sampler->height};
    return gml_render_runtime_sprite_scene_source_view(r,sprite,0,source);
  }
  if(!sprite->frame || sprite->frame[0]<0 || sprite->frame[0]>=r->n_tpag)return 0;
  GmlTpag *page=&r->tpag[sprite->frame[0]];
  if(page->sw<=0 || page->sh<=0 || page->sx<0 || page->sy<0 ||
     !gml_render_atlas_scene_source_view(r,page->atlas,source))return 0;
  int64_t x=page->tx,y=page->ty,w=page->sw,h=page->sh,sx=page->sx,sy=page->sy;
  if(x<0){sx-=x;w+=x;x=0;}if(y<0){sy-=y;h+=y;y=0;}
  if(x+w>sprite->w)w=sprite->w-x;
  if(y+h>sprite->h)h=sprite->h-y;
  if(w<=0 || h<=0 || sx+w>source->width || sy+h>source->height)return 0;
  sampler->source_region=(GmlRenderRect){(int32_t)sx,(int32_t)sy,(uint32_t)w,(uint32_t)h};
  sampler->offset_x=(int32_t)x;sampler->offset_y=(int32_t)y;return 1;
}

static int layer_cloud_effect(GmlRender *r,const GmlLayerFilter *filter,
                               double time,double camx,double camy,
                               GmlRenderLayerEffect *effect,GmlRenderResourceSource *source){
  const typeof(filter->u.clouds) *c=&filter->u.clouds;
  effect->kind=GML_RENDER_LAYER_EFFECT_CLOUDS;effect->intensity=1.0;
  effect->colour=c->light_colour;effect->shade_colour=c->shade_colour;
  effect->clouds=(GmlRenderLayerClouds){time,{camx,camy},c->scale,
    {c->velocity[0],c->velocity[1]},c->turbulence,c->waves,
    {c->shape[0],c->shape[1]},c->level,c->density,
    c->fade,{c->shade_offset[0],c->shade_offset[1]},c->shade_fade};
  double values[16];gml_render_layer_cloud_parameters(&effect->clouds,values);
  for(unsigned i=0;i<16u;i++)if(!isfinite(values[i]) || fabs(values[i])>1000000.0)return 0;
  if(!layer_effect_sampler_source(r,filter->sampler_sprite,&effect->sampler,source))return 0;
  const GmlRenderDeviceServices *device=gml_render_device(r);GmlRenderDeviceCaps caps={0};
  return device && device->capabilities && device->capabilities(device->context,&caps) &&
    effect->sampler.width<=caps.max_texture_extent && effect->sampler.height<=caps.max_texture_extent;
}

/* The filter-specific qualification stays at the captured-pass boundary. */
static int layer_filter_try_gpu(GmlRender *r,const GmlLayerFilter *filter,
                                int width,int height,double time,double camx,double camy){
  GmlRenderLayerEffect effect={0};
  GmlRenderResourceSource auxiliary={0};
  if(!r->layer_filter_gpu_candidate || !r->layer_filter_src ||
     (filter->kind!=GML_LAYER_FILTER_TINT &&
      filter->kind!=GML_LAYER_FILTER_COLOURISE &&
      filter->kind!=GML_LAYER_FILTER_ZOOM_BLUR &&
      filter->kind!=GML_LAYER_FILTER_CLOUDS)) return 0;
  if(filter->kind==GML_LAYER_FILTER_CLOUDS){
    if(!r->layer_filter_gpu_capture || !layer_cloud_effect(r,filter,time,camx,camy,&effect,&auxiliary))return 0;
    return layer_effect_submit_gpu(r,r->layer_filter_src,r->layer_filter_resource,
      &auxiliary,&effect,width,height);
  }
  if(filter->kind==GML_LAYER_FILTER_ZOOM_BLUR &&
     (!isfinite(filter->u.zoom_blur.centre[0]) ||
      !isfinite(filter->u.zoom_blur.centre[1]) ||
      !isfinite(filter->u.zoom_blur.intensity) ||
      !isfinite(filter->u.zoom_blur.focus_radius) ||
      filter->u.zoom_blur.centre[0]<0.0 || filter->u.zoom_blur.centre[0]>1.0 ||
      filter->u.zoom_blur.centre[1]<0.0 || filter->u.zoom_blur.centre[1]>1.0 ||
      filter->u.zoom_blur.intensity<0.0 || filter->u.zoom_blur.intensity>1.0 ||
      filter->u.zoom_blur.focus_radius<0.0 ||
      filter->u.zoom_blur.focus_radius>8192.0 ||
      (r->interp!=0 && r->interp!=1))) return 0;
  effect.kind=filter->kind==GML_LAYER_FILTER_TINT?
    GML_RENDER_LAYER_EFFECT_TINT:filter->kind==GML_LAYER_FILTER_COLOURISE?
    GML_RENDER_LAYER_EFFECT_COLOURISE:GML_RENDER_LAYER_EFFECT_ZOOM_BLUR;
  effect.colour=filter->kind==GML_LAYER_FILTER_TINT?
    filter->u.tint.colour:filter->kind==GML_LAYER_FILTER_COLOURISE?
    filter->u.colourise.tint_colour:0xFFFFFFFFu;
  effect.intensity=filter->kind==GML_LAYER_FILTER_TINT?
    1.0:filter->kind==GML_LAYER_FILTER_COLOURISE?
    filter->u.colourise.intensity:filter->u.zoom_blur.intensity;
  if(filter->kind==GML_LAYER_FILTER_ZOOM_BLUR){
    effect.centre[0]=filter->u.zoom_blur.centre[0];
    effect.centre[1]=filter->u.zoom_blur.centre[1];
    effect.focus_radius=filter->u.zoom_blur.focus_radius;
    effect.linear=(uint32_t)r->interp;
  }
  return layer_effect_submit_gpu(r,r->layer_filter_src,
    r->layer_filter_gpu_capture?r->layer_filter_resource:NULL,NULL,&effect,width,height);
}

/* Both inline kernels consume the same immutable device snapshot. A missing
 * capability or refused packet retains the established CPU authority barrier. */
static int layer_inline_try_gpu(GmlRender *r,const GmlRenderLayerEffect *effect){
  const GmlRenderDeviceServices *device=r?gml_render_device(r):NULL;
  GmlRenderDeviceCaps caps={0};
  if(!r || !r->resident_application_pass || !device || !device->capabilities ||
     !device->capabilities(device->context,&caps) ||
     !(caps.supported_operations&(UINT64_C(1)<<GML_RENDER_OP_LAYER_EFFECT)) ||
     !gml_render_scene_target_matches(r) || r->layer_filter_active || r->pending_presentation)
    return 0;
  gml_render_prepare_draw(r);
  if(!r->resident_application_pass || !gml_render_scene_target_matches(r) ||
     !gml_render_scene_submit_pending(r)) return 0;
  return layer_effect_submit_gpu(r,NULL,NULL,NULL,effect,r->fbw,r->fbh);
}

void gml_render_layer_tint(GmlRender *r,uint32_t rgba){
  if(rgba==0xFFFFFFFFu)return;
  GmlRenderLayerEffect effect={.kind=GML_RENDER_LAYER_EFFECT_INLINE_TINT,
    .colour=rgba,.intensity=1.0};
  if(layer_inline_try_gpu(r,&effect)!=0)return;
  gml_render_cpu_layer_tint(r,rgba);
}

void gml_render_layer_rgb_noise(GmlRender *r,uint32_t sampler_tpag_ptr,
                                double intensity,double animation,uint32_t rgb){
  if(isfinite(intensity) && intensity>0.0 && intensity<=1.0 &&
     isfinite(animation) && isfinite((double)(float)animation) &&
     (double)(float)animation*4096.0>=INT32_MIN &&
     (double)(float)animation*4096.0<=INT32_MAX){
    GmlRenderLayerEffect effect={.kind=GML_RENDER_LAYER_EFFECT_INLINE_RGB_NOISE,
      .colour=rgb,.intensity=intensity,
      .phase_seed=(int32_t)llround((double)(float)animation*4096.0)};
    if(layer_inline_try_gpu(r,&effect)!=0)return;
  }
  gml_render_cpu_layer_rgb_noise(r,sampler_tpag_ptr,intensity,animation,rgb);
}

int gml_render_layer_filter_begin(GmlRender *r,const GmlLayerFilter *filter){
  if(!r || !filter || filter->kind==GML_LAYER_FILTER_NONE || !r->fb ||
     r->fbw<=0 || r->fbh<=0 || r->layer_filter_active || r->target_sp>=GML_SURFACE_STACK) return 0;
  size_t count=(size_t)r->fbw*(size_t)r->fbh;
  if(!layer_filter_reserve(r,count)) return 0;
  int gpu_candidate=r->resident_application_pass && gml_render_device(r) &&
    !r->gui_pass_active && !r->classic && r->active_shader<0;
  gml_render_prepare_draw(r);
  GmlRenderLayerEffect cloud={0};GmlRenderResourceSource sampler={0};
  int cloud_candidate=r->scene_device_enabled && r->resident_application_pass &&
    filter->kind==GML_LAYER_FILTER_CLOUDS &&
    layer_cloud_effect(r,filter,0,r->cam_x,r->cam_y,&cloud,&sampler);
  int native_capture=(cloud_candidate || filter->kind==GML_LAYER_FILTER_TINT ||
    filter->kind==GML_LAYER_FILTER_COLOURISE || filter->kind==GML_LAYER_FILTER_ZOOM_BLUR) &&
    !r->classic_phase_scene_target &&
    gml_render_layer_capture_prepare(r,r->fbw,r->fbh,filter->affects_below);
  /* The CPU capture and final composite must observe the current target, even
   * when the preceding scene/clear has only been submitted to the device. */
  if(!native_capture && !gml_render_cpu_draw_barrier(r)) return 0;
  if(!native_capture && r->target_id>0){
    int width=0,height=0;
    if(surface_pixels(r,r->target_id,&width,&height)!=r->fb ||
       width!=r->fbw || height!=r->fbh) return 0;
  }
  int captured_known=r->fb_opaque_known,captured_opaque=r->fb_all_opaque;
  int captured_transparent=r->fb_all_transparent;
  if(!native_capture){
    if(filter->affects_below) memcpy(r->layer_filter_src,r->fb,count*sizeof(uint32_t));
    else memset(r->layer_filter_src,0,count*sizeof(uint32_t));
  }
  r->target_stack[r->target_sp++]=(typeof(r->target_stack[0])){
    r->fb,r->fbw,r->fbh,r->cam_x,r->cam_y,r->projection_cam_x,r->projection_cam_y,
    r->target_id,r->fb_opaque_known,r->fb_all_opaque,r->fb_all_transparent,
    r->pending_underlay,r->underlay_x,r->underlay_y,r->underlay_w,r->underlay_h,
    r->pending_fill,r->pending_fill_color,r->resident_application_pass
  };
  r->fb=r->layer_filter_src; r->target_id=-2;
  r->fb_opaque_known=filter->affects_below?captured_known:1;
  r->fb_all_opaque=filter->affects_below?captured_opaque:0;
  r->fb_all_transparent=filter->affects_below?captured_transparent:1;
  r->pending_underlay=0; r->underlay_x=r->underlay_y=r->underlay_w=r->underlay_h=0;
  r->pending_fill=0; r->pending_fill_color=0; r->layer_filter_active=1;
  r->layer_filter_gpu_candidate=gpu_candidate || native_capture;
  r->layer_filter_gpu_capture=native_capture;
  return 1;
}

void gml_render_layer_filter_end(GmlRender *r,const GmlLayerFilter *filter,double time_seconds){
  if(!r || !filter || !r->layer_filter_active) return;
  int width=r->fbw,height=r->fbh;
  double camera_x=r->cam_x,camera_y=r->cam_y;
  int source_empty=r->fb_all_transparent;
  int native_capture=r->layer_filter_gpu_capture;
  int submitted=!native_capture || gml_render_scene_submit_pending(r);
  int parent_resident=r->target_sp>0?r->target_stack[r->target_sp-1].resident_pass:0;
  gml_render_cpu_layer_filter_restore_target(r);
  if(native_capture)r->resident_application_pass=parent_resident;
  r->layer_filter_active=0;
  if(submitted && !source_empty && r->fb && r->fbw==width && r->fbh==height){
    int gpu=layer_filter_try_gpu(r,filter,width,height,time_seconds,camera_x,camera_y);
    r->layer_filter_gpu_candidate=0;
    if(gpu>0 && render_setting(r,"GML_LOG_LAYER_EFFECT") &&
       (r->frame<4 || (r->frame%60)==0))
      anygm_host_logf(r->win?r->win->host:NULL,ANYGM_LOG_DEBUG,
        "[layer-filter] f%ld kind=%d empty=0 size=%dx%d cam=%.3f,%.3f time=%.6f linear=%d backend=gpu capture_bytes=%zu checkpoint_bytes=%zu\n",
        r->frame,filter->kind,width,height,camera_x,camera_y,time_seconds,r->interp,
        (size_t)width*(size_t)height*4u,(size_t)width*(size_t)height*4u);
    if(gpu!=0){r->layer_filter_gpu_capture=0;return;}
  }
  r->layer_filter_gpu_candidate=0;
  r->layer_filter_gpu_capture=0;
  if(native_capture){
    if(!submitted || source_empty)return;
    if(!gml_render_cpu_draw_barrier(r) ||
       !gml_render_resource_acquire_cpu_read(r->layer_filter_resource,NULL))return;
  }
  gml_render_cpu_layer_filter_end(r,filter,time_seconds,width,height,
                                  camera_x,camera_y,source_empty);
}

void gml_render_layer_capture_abandon(GmlRender *r){
  if(!r || !r->layer_filter_gpu_capture)return;
  /* The scene-loss owner discarded pending packets before abandoning this borrow. */
  r->pending_fill=r->pending_underlay=0;
  if(r->layer_filter_active && r->target_id==-2 && r->fb==r->layer_filter_src)
    gml_render_cpu_layer_filter_restore_target(r);
  r->layer_filter_active=r->layer_filter_gpu_candidate=r->layer_filter_gpu_capture=0;
}
