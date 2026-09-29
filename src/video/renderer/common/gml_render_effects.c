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
  free(r->layer_filter_src); free(r->layer_filter_work); free(r->layer_filter_aux);
  r->layer_filter_src=src; r->layer_filter_work=work; r->layer_filter_aux=aux;
  r->layer_filter_capacity=count;
  return 1;
}

/* R03-P7-LAYER-EFFECT-SUBMIT: the captured filter and the inline noise both
 * submit their CPU-current source through the same device checkpoint. */
static int layer_effect_submit_gpu(GmlRender *r,const uint32_t *source_pixels,
                                   const GmlRenderLayerEffect *effect,
                                   int width,int height){
  GmlRenderResource *target;
  GmlRenderResourceSource source={0};
  GmlRenderImageRef source_ref,image;
  GmlRenderDrawOp operation={0};
  GmlRenderDrawStateSnapshot state={0};
  GmlRenderDrawBatch batch;
  GmlRenderResourceExecution result;
  GmlRenderDeviceCaps caps={0};
  GmlRenderJournalMetrics metrics={0};
  const GmlRenderDeviceServices *device=gml_render_device(r);
  size_t bytes;
  int application;
  if(!device || !device->capabilities ||
     !device->capabilities(device->context,&caps) || !caps.max_texture_extent ||
     width<=0 || height<=0 || (uint32_t)width>caps.max_texture_extent ||
     (uint32_t)height>caps.max_texture_extent ||
     (size_t)width>SIZE_MAX/4u/(size_t)height ||
     (size_t)width*(size_t)height>16u*1024u*1024u ||
     !source_pixels || !r->fb || !effect || !isfinite(effect->intensity)) return 0;
  application=r->target_id==-1 && r->target_sp==0 &&
    r->fb==r->app_surface_owned && r->fbw==r->app_w && r->fbh==r->app_h;
  if(!application && !(r->target_id>0 && r->target_id<=GML_MAX_SURFACES &&
     r->surface[r->target_id-1].live &&
     r->fb==r->surface[r->target_id-1].px &&
     r->fbw==r->surface[r->target_id-1].w &&
     r->fbh==r->surface[r->target_id-1].h)) return 0;
  target=application?gml_render_application_resource_ensure(r):
    gml_render_surface_resource_ensure(r,r->target_id);
  if(!target) return 0;
  image=gml_render_resource_image_ref(target);
  if(image.content_version==UINT64_MAX) return 0;
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
  operation.opcode=GML_RENDER_OP_LAYER_EFFECT;
  operation.scene_family=GML_RENDER_SCENE_EFFECT;
  operation.reconstruction_mode=GML_RENDER_DEVICE_CHECKPOINT;
  operation.source_count=1u;
  operation.sources=&source_ref;
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
  state.sampler_count=1u;
  state.samplers[0].filter=GML_RENDER_FILTER_NEAREST;
  state.samplers[0].edge_rule=GML_RENDER_EDGE_CLAMP_SUBIMAGE;
  state.samplers[0].source_region=operation.target.region;
  batch=(GmlRenderDrawBatch){&operation,1u,&state,1u};
  result=application?gml_render_application_scene_submit(r,&batch,&source,1u):
    gml_render_surface_scene_submit(r,r->target_id,&batch,&source,1u);
  if(device->retire) device->retire(device->context,source_ref);
  if(result==GML_RENDER_RESOURCE_EXEC_DEVICE){
    r->fb_opaque_known=0;
    r->fb_all_transparent=0;
    return 1;
  }
  gml_render_resource_journal_metrics(target,&metrics);
  return metrics.authority==GML_RENDER_RESOURCE_INVALID?-1:0;
}

/* The filter-specific qualification stays at the captured-pass boundary. */
static int layer_filter_try_gpu(GmlRender *r,const GmlLayerFilter *filter,
                                int width,int height){
  GmlRenderLayerEffect effect={0};
  if(!r->layer_filter_gpu_candidate || !r->layer_filter_src ||
     (filter->kind!=GML_LAYER_FILTER_TINT &&
      filter->kind!=GML_LAYER_FILTER_COLOURISE &&
      filter->kind!=GML_LAYER_FILTER_ZOOM_BLUR)) return 0;
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
  return layer_effect_submit_gpu(r,r->layer_filter_src,&effect,width,height);
}

/* R03-P7-LAYER-RGB-NOISE: the inline effect reads the current target. Keep
 * the characterised CPU cache/kernel intact for unqualified and failed paths. */
void gml_render_layer_rgb_noise(GmlRender *r,uint32_t sampler_tpag_ptr,
                                double intensity,double animation,uint32_t rgb){
  int gpu_candidate=r && r->fb && r->fbw>0 && r->fbh>0 &&
    r->resident_application_pass && gml_render_device(r) &&
    r->target_id==-1 && r->target_sp==0 &&
    r->fb==r->app_surface_owned && r->fbw==r->app_w && r->fbh==r->app_h &&
    !r->layer_filter_active && !r->gui_pass_active && !r->classic &&
    r->active_shader<0 && isfinite(intensity) && intensity>0.0 && intensity<=1.0 &&
    isfinite(animation) &&
    isfinite((double)(float)animation) &&
    (double)(float)animation*4096.0>=INT32_MIN &&
    (double)(float)animation*4096.0<=INT32_MAX;
  if(gpu_candidate){
    GmlRenderLayerEffect effect={0};
    int opaque_known,all_opaque;
    gml_render_prepare_draw(r);
    if(!gml_render_resident_application_cpu_barrier(r)) return;
    opaque_known=r->fb_opaque_known;
    all_opaque=r->fb_all_opaque;
    effect.kind=GML_RENDER_LAYER_EFFECT_RGB_NOISE;
    effect.colour=rgb;
    effect.intensity=intensity;
    effect.phase_seed=(int32_t)llround((double)(float)animation*4096.0);
    int result=layer_effect_submit_gpu(r,r->fb,&effect,r->fbw,r->fbh);
    if(result>0){
      r->fb_opaque_known=opaque_known;
      r->fb_all_opaque=all_opaque;
      return;
    }
    if(result<0) return;
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
  /* The CPU capture and final composite must observe the current target, even
   * when the preceding scene/clear has only been submitted to the device. */
  if(!gml_render_resident_application_cpu_barrier(r)) return 0;
  if(r->target_id>0){
    int width=0,height=0;
    if(surface_pixels(r,r->target_id,&width,&height)!=r->fb ||
       width!=r->fbw || height!=r->fbh) return 0;
  }
  int captured_known=r->fb_opaque_known,captured_opaque=r->fb_all_opaque;
  int captured_transparent=r->fb_all_transparent;
  if(filter->affects_below) memcpy(r->layer_filter_src,r->fb,count*sizeof(uint32_t));
  else memset(r->layer_filter_src,0,count*sizeof(uint32_t));
  r->target_stack[r->target_sp++]=(typeof(r->target_stack[0])){
    r->fb,r->fbw,r->fbh,r->cam_x,r->cam_y,r->projection_cam_x,r->projection_cam_y,
    r->target_id,r->fb_opaque_known,r->fb_all_opaque,r->fb_all_transparent,
    r->pending_underlay,r->underlay_x,r->underlay_y,r->underlay_w,r->underlay_h,
    r->pending_fill,r->pending_fill_color
  };
  r->fb=r->layer_filter_src; r->target_id=-2;
  r->fb_opaque_known=filter->affects_below?captured_known:1;
  r->fb_all_opaque=filter->affects_below?captured_opaque:0;
  r->fb_all_transparent=filter->affects_below?captured_transparent:1;
  r->pending_underlay=0; r->underlay_x=r->underlay_y=r->underlay_w=r->underlay_h=0;
  r->pending_fill=0; r->pending_fill_color=0; r->layer_filter_active=1;
  r->layer_filter_gpu_candidate=gpu_candidate;
  return 1;
}

void gml_render_layer_filter_end(GmlRender *r,const GmlLayerFilter *filter,double time_seconds){
  if(!r || !filter || !r->layer_filter_active) return;
  int width=r->fbw,height=r->fbh;
  double camera_x=r->cam_x,camera_y=r->cam_y;
  int source_empty=r->fb_all_transparent;
  gml_render_cpu_layer_filter_restore_target(r);
  r->layer_filter_active=0;
  if(!source_empty && r->fb && r->fbw==width && r->fbh==height){
    int gpu=layer_filter_try_gpu(r,filter,width,height);
    r->layer_filter_gpu_candidate=0;
    if(gpu>0 && render_setting(r,"GML_LOG_LAYER_EFFECT") &&
       (r->frame<4 || (r->frame%60)==0))
      anygm_host_logf(r->win?r->win->host:NULL,ANYGM_LOG_DEBUG,
        "[layer-filter] f%ld kind=%d empty=0 size=%dx%d cam=%.3f,%.3f time=%.6f linear=%d backend=gpu capture_bytes=%zu checkpoint_bytes=%zu\n",
        r->frame,filter->kind,width,height,camera_x,camera_y,time_seconds,r->interp,
        (size_t)width*(size_t)height*4u,(size_t)width*(size_t)height*4u);
    if(gpu!=0) return;
  }
  r->layer_filter_gpu_candidate=0;
  gml_render_cpu_layer_filter_end(r,filter,time_seconds,width,height,
                                  camera_x,camera_y,source_empty);
}
