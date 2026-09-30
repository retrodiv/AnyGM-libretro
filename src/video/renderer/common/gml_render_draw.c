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
