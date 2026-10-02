/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Neutral device binding. Operation preparation and barriers grow here as scene families land. */
#include "gml_render_internal.h"
#include "gml_render_surfaces_internal.h"

#include <math.h>
#include <string.h>

int gml_render_stretched_float_phase_exact(uint32_t logical,uint32_t destination){
  if(!logical || logical>=destination || destination>16384u) return 0;
  const uint32_t denominator=destination*2u;
  for(uint32_t pixel=0;pixel<destination;pixel++){
    /* The preserved CPU band divides in double before subtracting its integer
     * tap. Near a float rounding boundary that cancellation need not match a
     * correctly rounded rational phase. Qualify the whole axis before writes;
     * this is an admission guard, not another sampling kernel. */
    double phase=(pixel+0.5)*logical/(double)destination-0.5;
    uint32_t numerator=(pixel*2u+1u)*logical+destination;
    if((float)(phase-floor(phase))!=
       (float)((numerator%denominator)/(double)denominator)) return 0;
  }
  return 1;
}

int gml_render_filtered_font_quad_valid(const GmlRenderQuad *q,
                                        uint32_t width,uint32_t height){
  if(!q || !width || !height || width>16384u || height>16384u ||
     (q->sampling.quantisation!=GML_RENDER_QUANTISE_FONT_FIRSTGEN &&
      q->sampling.quantisation!=GML_RENDER_QUANTISE_FONT_MODERN &&
      q->sampling.quantisation!=GML_RENDER_QUANTISE_FONT_FIRSTGEN_INDEPENDENT) ||
     q->shape!=GML_RENDER_QUAD_AXIS_ALIGNED || q->span_active ||
     q->sampling.filter!=GML_RENDER_FILTER_BILINEAR ||
     q->sampling.edge_rule!=GML_RENDER_EDGE_CLAMP_SUBIMAGE ||
     q->sampling.logical_margin_mask ||
     !q->sampling.source_region.width || q->sampling.source_region.width>256u ||
     !q->sampling.source_region.height || q->sampling.source_region.height>256u ||
     !isfinite(q->filtered_draw_alpha) || q->filtered_draw_alpha<0x1p-32 ||
     q->filtered_draw_alpha>1.0 || q->inverse_x[1]!=0.0 || q->inverse_y[0]!=0.0 ||
     q->inverse_offset[0]!=0.0 || q->inverse_offset[1]!=0.0) return 0;
  for(unsigned axis=0;axis<2u;axis++){
    double origin=q->inverse_anchor[axis];
    double scale=axis?q->inverse_y[1]:q->inverse_x[0];
    double first=axis?q->y[0]:q->x[0];
    double last=axis?q->y[2]:q->x[1];
    double extent=axis?q->sampling.source_region.height:q->sampling.source_region.width;
    if(!isfinite(origin) || fpclassify(origin)==FP_SUBNORMAL || fabs(origin)>1048576.0 ||
       !isfinite(scale) || scale<0x1p-16 || scale>1.0 ||
       first!=floor(origin+0.5) || last!=floor(last) ||
       last<=first || last<=0.0 || first>=(double)(axis?height:width)) return 0;
    double expected=q->sampling.quantisation==GML_RENDER_QUANTISE_FONT_FIRSTGEN &&
      fabs(scale-nearbyint(scale))>1e-9
        ? floor(origin+extent*scale+0.5)-first : floor(extent*scale+0.5);
    if(last-first!=expected) return 0;
  }
  if(q->x[3]!=q->x[0] || q->x[2]!=q->x[1] ||
     q->y[1]!=q->y[0] || q->y[3]!=q->y[2]) return 0;
  for(unsigned corner=0;corner<4u;corner++)
    if(q->corner_colour[corner]!=q->corner_colour[0] ||
       (q->corner_colour[corner]>>24)!=255u) return 0;
  return 1;
}

int gml_render_automatic_surface_quad_valid(const GmlRenderQuad *q,
                                             uint32_t width,uint32_t height){
  if(!q || !width || !height || width>16384u || height>16384u ||
     (q->sampling.quantisation!=GML_RENDER_QUANTISE_AUTOMATIC_SURFACE_FIRSTGEN &&
      q->sampling.quantisation!=GML_RENDER_QUANTISE_AUTOMATIC_SURFACE_MODERN) ||
     q->shape!=GML_RENDER_QUAD_AXIS_ALIGNED || q->span_active || q->source_opaque>1u ||
     (q->sampling.filter!=GML_RENDER_FILTER_NEAREST &&
      q->sampling.filter!=GML_RENDER_FILTER_BOX) ||
     q->sampling.edge_rule!=GML_RENDER_EDGE_CLAMP_SUBIMAGE ||
     q->sampling.logical_margin_mask || q->sampling.source_region.x ||
     q->sampling.source_region.y || !q->sampling.source_region.width ||
     !q->sampling.source_region.height || q->sampling.source_region.width>16384u ||
     q->sampling.source_region.height>16384u) return 0;
  uint32_t samples=1u;
  for(unsigned axis=0;axis<2u;axis++){
    double first=axis?q->y[0]:q->x[0],last=axis?q->y[2]:q->x[1];
    uint32_t source=axis?q->sampling.source_region.height:q->sampling.source_region.width;
    if(!isfinite(first) || !isfinite(last) || floor(first)!=first || floor(last)!=last ||
       first<-16384.0 || first>=16384.0 || last>32768.0 || last<=0.0 ||
       first>=(double)(axis?height:width) || last<=first || last-first>16384.0) return 0;
    uint32_t extent=(uint32_t)(last-first);
    samples*=(source+extent-1u)/extent;
  }
  if(q->sampling.filter==GML_RENDER_FILTER_BOX && samples>4096u) return 0;
  if(q->x[3]!=q->x[0] || q->x[2]!=q->x[1] ||
     q->y[1]!=q->y[0] || q->y[3]!=q->y[2]) return 0;
  for(unsigned corner=0;corner<4u;corner++)
    if(q->corner_colour[corner]!=0xFFFFFFFFu ||
       q->u[corner]!=(corner==0u || corner==3u?0.0:1.0) ||
       q->v[corner]!=(corner<2u?0.0:1.0)) return 0;
  return 1;
}

static void render_unbind_device(GmlRender *render,int context_current){
  if(!render) return;
  if(context_current) (void)gml_render_checkpoint_pending_resources(render);
  /* A queued exact batch can replay through the CPU after the device binding is removed. A
   * non-reconstructible pending result instead becomes invalid when its context is already lost. */
  render->device_bound=0;
  (void)gml_render_application_scene_flush(render);
  for(int index=0;index<GML_MAX_SURFACES;index++)
    gml_render_resource_context_lost(render->surface[index].resource);
  gml_render_resource_context_lost(render->app_surface_resource);
  gml_render_resource_context_lost(render->base_scene_resource);
  memset(&render->device,0,sizeof render->device);
}

void gml_render_abandon_device(GmlRender *render){
  render_unbind_device(render,0);
}

void gml_render_bind_device(GmlRender *render,const GmlRenderDeviceServices *services){
  if(!render) return;
  if(!services || !services->context || !services->capabilities || !services->submit ||
     !services->upload || !services->readback || !services->retire || !services->flush ||
     !services->wait){
    render_unbind_device(render,1);
    return;
  }
  if(!gml_render_application_scene_flush(render)) return;
  if(!gml_render_checkpoint_pending_resources(render)) return;
  render->device=*services;
  render->device_bound=1;
}

const GmlRenderDeviceServices *gml_render_device(const GmlRender *render){
  return render && render->device_bound?&render->device:NULL;
}
