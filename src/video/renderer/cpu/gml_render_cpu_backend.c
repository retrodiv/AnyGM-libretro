/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Borrowed CPU views and coarse coordination for the non-reentrant software leaves. */
#include "gml_render_cpu_backend.h"
#include "gml_render_internal.h"
#include "gml_render_surfaces_internal.h"
#include "gml_render_cpu_dispatch_internal.h"
#include "gml_render_cpu_pixel_internal.h"
#include "anygm_host.h"

#include <string.h>

void gml_render_cpu_backend_dispatch_begin(GmlRender *render,
                                           GmlRenderCpuBackendDispatchScope *scope,
                                           unsigned family,int depth_dependent){
  if(!scope) return;
  gml_render_dispatch_cpu_scope_begin(render,scope,family);
  if(scope->active && depth_dependent)
    scope->exception=GML_RENDER_DISPATCH_EXCEPTION_DEPTH;
}

void gml_render_cpu_backend_dispatch_end(GmlRender *render,
                                         GmlRenderCpuBackendDispatchScope *scope){
  if(scope) gml_render_dispatch_cpu_scope_end(render,scope);
}

static void backend_texture_view_reset(GmlRenderCpuBackendTextureView *view){
  if(view) memset(view,0,sizeof(*view));
}

static void backend_texture_view_page(GmlRenderCpuBackendTextureView *view,
                                      const GmlTpag *page,const GmlAtlas *atlas,
                                      int full_atlas){
  if(!view) return;
  view->pixel_kind=GML_RENDER_CPU_BACKEND_PIXELS_RGBA;
  view->trim_x=page->tx; view->trim_y=page->ty;
  view->width=full_atlas?atlas->w:page->sw;
  view->height=full_atlas?atlas->h:page->sh;
  view->stride=atlas->w;
  view->source_x=full_atlas?0:page->sx;
  view->source_y=full_atlas?0:page->sy;
  view->full_width=atlas->w; view->full_height=atlas->h;
  view->atlas_index=page->atlas;
  view->rgba=atlas->px;
}

int gml_render_cpu_backend_texture_view(GmlRender *R,int handle,int full_atlas,
                                    GmlRenderCpuBackendTextureView *view){
  backend_texture_view_reset(view);
  if(!R) return 0;
  uint32_t encoded=(uint32_t)handle,kind=encoded&GML_TEX_KIND_MASK;
  if(kind==GML_TEX_ATLAS_TAG)
    return gml_render_cpu_backend_atlas_view(R,(int)(encoded&0x00FFFFFFu),view);
  if(kind==GML_TEX_SPR_TAG){
    int spr=(int)((encoded>>10)&0xFFFF),img=(int)(encoded&0x3FF);
    gml_render_warm_sprite(R,spr);
    if(spr<0 || spr>=R->n_spr) return 0;
    GmlSprite *sprite=&R->spr[spr];
    if(view){
      view->resource_index=spr;
      view->logical_width=sprite->w; view->logical_height=sprite->h;
      view->origin_x=sprite->originx; view->origin_y=sprite->originy;
    }
    if(sprite->runtime_rgba){
      int frames=sprite->n_frames>0?sprite->n_frames:1;
      int sub=((img%frames)+frames)%frames;
      if(view){
        view->pixel_kind=GML_RENDER_CPU_BACKEND_PIXELS_RGBA;
        view->runtime=1;
        view->width=view->full_width=sprite->w;
        view->height=view->full_height=sprite->h;
        view->stride=sprite->w;
        if(sprite->w>0 && sprite->h>0)
          view->rgba=sprite->runtime_rgba+(size_t)sub*sprite->w*sprite->h*4;
      }
      return 1;
    }
    if(sprite->n_frames<=0 || !sprite->frame) return 0;
    int sub=((img%sprite->n_frames)+sprite->n_frames)%sprite->n_frames;
    int page_index=sprite->frame[sub];
    if(page_index<0 || page_index>=R->n_tpag) return 0;
    GmlTpag *page=&R->tpag[page_index];
    if(page->atlas<0 || page->atlas>=R->n_atlas) return 0;
    GmlAtlas *atlas=&R->atlas[page->atlas];
    if(!atlas->px || atlas->w<=0 || atlas->h<=0) return 0;
    if(view){
      view->page_index=page_index;
      backend_texture_view_page(view,page,atlas,full_atlas);
    }
    return 1;
  }
  if(kind==GML_TEX_SURF_TAG){
    int surface=(int)(encoded&0xFFFF),width=0,height=0;
    const uint32_t *pixels=gml_surface_pixels_read(R,surface,&width,&height);
    if(!pixels || width<=0 || height<=0) return 0;
    if(view){
      view->pixel_kind=GML_RENDER_CPU_BACKEND_PIXELS_XRGB;
      view->logical_width=view->width=view->full_width=width;
      view->logical_height=view->height=view->full_height=height;
      view->stride=width; view->resource_index=surface; view->xrgb=pixels;
    }
    return 1;
  }
  if(kind==GML_TEX_FONT_TAG){
    int font=(int)(encoded&0x00FFFFFFu);
    if(font<0 || font>=R->n_fonts || !R->fonts[font].real) return 0;
    int atlas=R->fonts[font].atlas;
    int ok=gml_render_cpu_backend_atlas_view(R,atlas,view);
    if(ok && view) view->resource_index=font;
    return ok;
  }
  if(kind!=GML_TEX_BG_TAG){
    if(anygm_host_development_setting(R->win?R->win->host:NULL,"GML_LOG_D3D"))
      anygm_host_logf(R->win?R->win->host:NULL,ANYGM_LOG_DEBUG,
                      "[d3d] invalid texture handle %d\n",handle);
    return 0;
  }
  int background=(int)(encoded&0x00FFFFFF);
  gml_render_warm_bg(R,background);
  if(background<0 || background>=R->n_bg) return 0;
  int page_index=R->bg[background].tpag;
  if(page_index<0 || page_index>=R->n_tpag) return 0;
  GmlTpag *page=&R->tpag[page_index];
  if(page->atlas<0 || page->atlas>=R->n_atlas) return 0;
  GmlAtlas *atlas=&R->atlas[page->atlas];
  if(!atlas->px || page->sw<=0 || page->sh<=0) return 0;
  if(view){
    view->resource_index=background; view->page_index=page_index;
    view->logical_width=page->bw?page->bw:page->sw;
    view->logical_height=page->bh?page->bh:page->sh;
    backend_texture_view_page(view,page,atlas,full_atlas);
  }
  return 1;
}

int gml_render_cpu_backend_atlas_view(GmlRender *R,int atlas_index,
                                  GmlRenderCpuBackendTextureView *view){
  backend_texture_view_reset(view);
  if(!R || atlas_index<0 || atlas_index>=R->n_atlas ||
     !gml_render_warm_atlas(R,atlas_index)) return 0;
  GmlAtlas *atlas=&R->atlas[atlas_index];
  if(!atlas->px || atlas->w<=0 || atlas->h<=0) return 0;
  if(view){
    view->pixel_kind=GML_RENDER_CPU_BACKEND_PIXELS_RGBA;
    view->logical_width=view->width=view->full_width=atlas->w;
    view->logical_height=view->height=view->full_height=atlas->h;
    view->stride=atlas->w; view->atlas_index=atlas_index; view->rgba=atlas->px;
  }
  return 1;
}

GmlSoftware3D *gml_render_cpu_backend_software3d(GmlRender *r){
  return r?r->software3d:NULL;
}

void gml_render_cpu_backend_set_flat_fog(GmlRender *r,int enabled,uint32_t colour){
  if(!r) return;
  r->fog_flat=enabled?1:0;
  /* Content colours are BGR; framebuffer colours are RGB. */
  r->fog_flat_rgb=((colour&0xFFu)<<16)|(colour&0xFF00u)|((colour>>16)&0xFFu);
}

int gml_render_cpu_backend_draw_view(GmlRender *r,GmlRenderCpuBackendDrawView *view){
  if(view) memset(view,0,sizeof(*view));
  if(!r || !view) return 0;
  gml_render_flush_rotated_batch(r);
  view->host=r->win?r->win->host:NULL;
  view->pixels=r->fb;
  if(r->target_sp==0 && r->fb==r->base_fb){
    view->interpolation_pixels[0]=r->classic_interp_phase[0];
    view->interpolation_pixels[1]=r->classic_interp_phase[1];
    view->interpolation_pixels[2]=r->classic_interp_phase[2];
  }
  view->frame=r->frame;
  view->camera_x=r->cam_x; view->camera_y=r->cam_y;
  int world_coordinates=r->world_transform_active && !r->gui_pass_active &&
    (r->target_sp==0 || r->target_id==-2) && r->target_id<0;
  view->coordinate_scale_x=world_coordinates?r->world_scale_x:1.0;
  view->coordinate_scale_y=world_coordinates?r->world_scale_y:1.0;
  view->alpha=r->alpha;
  view->width=r->fbw; view->height=r->fbh;
  view->interpolate=r->interp;
  view->alpha_blend=r->alphablend; view->blend_mode=r->blendmode;
  view->circle_precision=r->circle_precision;
  view->zero_alpha_writes=destination_colour_blend_active(r);
  if(r->dispatch_cpu_depth) view->dispatch_written=&r->dispatch_cpu_written;
  return 1;
}
void gml_render_cpu_backend_prepare_draw(GmlRender *r){
  gml_render_maybe_prepare_draw(r);
}
int gml_render_cpu_backend_prepare_draw_view(GmlRender *r,GmlRenderCpuBackendDrawView *view){
  if(!gml_render_resident_application_cpu_barrier(r)) return 0;
  gml_render_cpu_backend_prepare_draw(r);
  return gml_render_cpu_backend_draw_view(r,view);
}
void gml_render_cpu_backend_sync_camera(GmlRender *r,double translation_x,double translation_y){
  if(!r) return;
  if(r->world_transform_active && !r->gui_pass_active &&
     (r->target_sp==0 || r->target_id==-2) && r->target_id<0){
    translation_x*=r->world_scale_x;
    translation_y*=r->world_scale_y;
  }
  r->cam_x=r->projection_cam_x-translation_x;
  r->cam_y=r->projection_cam_y-translation_y;
}

void gml_render_cpu_backend_draw_map_point(const GmlRender *r,double *x,double *y){
  gml_render_draw_map_point(r,x,y);
}
