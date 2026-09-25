/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Neutral device binding. Operation preparation and barriers grow here as scene families land. */
#include "gml_render_internal.h"

#include <string.h>

void gml_render_bind_device(GmlRender *render,const GmlRenderDeviceServices *services){
  if(!render) return;
  if(!services || !services->context || !services->capabilities || !services->submit ||
     !services->upload || !services->readback || !services->retire || !services->flush ||
     !services->wait){
    memset(&render->device,0,sizeof render->device);
    render->device_bound=0;
    return;
  }
  render->device=*services;
  render->device_bound=1;
}

const GmlRenderDeviceServices *gml_render_device(const GmlRender *render){
  return render && render->device_bound?&render->device:NULL;
}
