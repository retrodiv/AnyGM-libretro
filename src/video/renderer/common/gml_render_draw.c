/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Neutral device binding. Operation preparation and barriers grow here as scene families land. */
#include "gml_render_internal.h"
#include "gml_render_surfaces_internal.h"

#include <string.h>

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
