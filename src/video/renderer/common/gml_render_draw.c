/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Neutral device binding. Operation preparation and barriers grow here as scene families land. */
#include "gml_render_internal.h"
#include "gml_render_surfaces_internal.h"

#include <string.h>

void gml_render_bind_device(GmlRender *render,const GmlRenderDeviceServices *services){
  if(!render) return;
  if(!services || !services->context || !services->capabilities || !services->submit ||
     !services->upload || !services->readback || !services->retire || !services->flush ||
     !services->wait){
    /* The old device may already be gone. Reconstruct queued exact work on the CPU before
     * releasing the device identity; no borrowed atlas pointer survives renderer teardown. */
    render->device_bound=0;
    (void)gml_render_application_scene_flush(render);
    for(int index=0;index<GML_MAX_SURFACES;index++)
      gml_render_resource_context_lost(render->surface[index].resource);
    gml_render_resource_context_lost(render->app_surface_resource);
    memset(&render->device,0,sizeof render->device);
    render->device_bound=0;
    return;
  }
  if(!gml_render_application_scene_flush(render)) return;
  render->device=*services;
  render->device_bound=1;
}

const GmlRenderDeviceServices *gml_render_device(const GmlRender *render){
  return render && render->device_bound?&render->device:NULL;
}
