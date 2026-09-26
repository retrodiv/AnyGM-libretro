/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Shared surface identity, lifetime, target stack, and read policy. */
#include "gml_render_surfaces_internal.h"
#include "gml_render_cpu_surfaces.h"
#include "gml_render.h"
#include "gml_render_internal.h"
#include "anygm_host.h"

#include <stdint.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Colour and coverage counted apart. A surface can be fully authored and hold no colour at all:
 * a scanline or shadow mask is black with an alpha ramp, so a count of non-black RGB reads zero
 * over a mask that drew perfectly, and reading that zero as "nothing was drawn" is a wrong answer
 * an instrument handed out rather than a defect in the renderer. */
static void surface_coverage_counts(const uint32_t *px,int w,int h,int *colour,int *covered){
  int c=0,a=0;
  for(size_t i=0;i<(size_t)w*(size_t)h;i++){
    if(px[i]&0x00FFFFFFu) c++;
    if(px[i]&0xFF000000u) a++;
  }
  *colour=c; *covered=a;
}
int surface_slot(int id){ return id>0 && id<=GML_MAX_SURFACES ? id-1 : -1; }

enum { GML_SCENE_BATCH_LIMIT=2048 };
struct GmlRenderPendingSceneBatch {
  GmlRenderDrawOp operations[GML_SCENE_BATCH_LIMIT];
  GmlRenderDrawStateSnapshot state;
  GmlRenderResourceSource source;
  GmlRenderImageRef source_ref;
  uint32_t count;
};

static int same_scene_source(GmlRenderImageRef a,GmlRenderImageRef b){
  return a.resource_class==b.resource_class && a.identity==b.identity &&
         a.incarnation==b.incarnation && a.content_version==b.content_version;
}

GmlRenderImageRef gml_render_application_scene_next_image_ref(
  GmlRender *r,GmlRenderResource *resource){
  GmlRenderImageRef image=gml_render_resource_image_ref(resource);
  uint32_t pending=r && r->pending_scene_batch?r->pending_scene_batch->count:0u;
  if(image.content_version>UINT64_MAX-pending) image.content_version=UINT64_MAX;
  else image.content_version+=pending;
  return image;
}

int gml_render_application_scene_flush(GmlRender *r){
  GmlRenderPendingSceneBatch *pending=r?r->pending_scene_batch:NULL;
  GmlRenderDrawBatch batch;
  GmlRenderResourceExecution result;
  if(!pending || !pending->count) return 1;
  if(!r->app_surface_resource) return 0;
  batch=(GmlRenderDrawBatch){pending->operations,pending->count,&pending->state,1u};
  result=gml_render_resource_submit(r->app_surface_resource,gml_render_device(r),
                                    &batch,&pending->source,1u);
  pending->count=0;
  return result!=GML_RENDER_RESOURCE_EXEC_FAILED;
}

void gml_render_application_scene_queue_dispose(GmlRender *r){
  if(!r) return;
  free(r->pending_scene_batch);
  r->pending_scene_batch=NULL;
}

int gml_render_application_scene_enqueue(GmlRender *r,const GmlRenderDrawOp *operation,
                                          const GmlRenderDrawStateSnapshot *state,
                                          const GmlRenderResourceSource *source){
  GmlRenderPendingSceneBatch *pending;
  GmlRenderImageRef next;
  if(!r || !r->resident_application_pass || !gml_render_device(r) ||
     !r->app_surface_resource || !operation || !state || !source ||
     !source->borrowed_immutable || !operation->sources ||
     operation->source_count!=1u || operation->opcode!=GML_RENDER_OP_TEXTURED_QUAD ||
     (operation->reconstruction_mode!=GML_RENDER_REPLAY_EXACT &&
      operation->reconstruction_mode!=GML_RENDER_DEVICE_CHECKPOINT) ||
     !same_scene_source(operation->sources[0],source->image)) return 0;
  pending=r->pending_scene_batch;
  if(pending && pending->count &&
     (pending->count==GML_SCENE_BATCH_LIMIT ||
      !same_scene_source(pending->source_ref,source->image) ||
      pending->source.bytes!=source->bytes ||
      memcmp(&pending->state,state,sizeof *state))){
    if(!gml_render_application_scene_flush(r)) return 0;
  }
  if(!pending){
    pending=(GmlRenderPendingSceneBatch*)calloc(1,sizeof *pending);
    if(!pending) return 0;
    r->pending_scene_batch=pending;
  }
  next=gml_render_application_scene_next_image_ref(r,r->app_surface_resource);
  if(next.content_version==UINT64_MAX ||
     operation->target.image.content_version!=next.content_version+1u ||
     operation->target.image.resource_class!=next.resource_class ||
     operation->target.image.identity!=next.identity ||
     operation->target.image.incarnation!=next.incarnation) return 0;
  if(!pending->count){
    pending->state=*state;
    pending->source=*source;
    pending->source_ref=source->image;
  }
  pending->operations[pending->count]=*operation;
  pending->operations[pending->count].sources=&pending->source_ref;
  pending->operations[pending->count].state_index=0;
  pending->count++;
  r->app_surface_opaque=0;
  r->app_presentation_coverage_active=0;
  return 1;
}

GmlRenderResource *gml_render_surface_resource_ensure(GmlRender *r,int id){
  int slot=surface_slot(id);
  GmlSurface *surface;
  GmlRenderImageRef image;
  if(!r || slot<0) return NULL;
  surface=&r->surface[slot];
  if(!surface->live || !surface->px || surface->w<=0 || surface->h<=0) return NULL;
  if(surface->resource) return surface->resource;
  if(r->next_surface_resource_incarnation==UINT64_MAX) return NULL;
  image=(GmlRenderImageRef){GML_RENDER_RESOURCE_SURFACE,(uint32_t)id,
    ++r->next_surface_resource_incarnation,1u};
  surface->resource=gml_render_resource_create(image,(uint32_t)surface->w,
    (uint32_t)surface->h,GML_RENDER_PIXEL_ARGB8888,surface->px,
    (GmlRenderResourceLimits){0});
  return surface->resource;
}

void gml_render_surface_resource_dispose(GmlRender *r,int id){
  int slot=surface_slot(id);
  GmlRenderResource *resource;
  const GmlRenderDeviceServices *device;
  if(!r || slot<0 || !(resource=r->surface[slot].resource)) return;
  device=gml_render_device(r);
  if(device && device->retire)
    device->retire(device->context,gml_render_resource_image_ref(resource));
  gml_render_resource_destroy(resource);
  r->surface[slot].resource=NULL;
}

GmlRenderResourceExecution gml_render_surface_scene_submit(
  GmlRender *r,int id,const GmlRenderDrawBatch *batch,
  const GmlRenderResourceSource *sources,uint32_t source_count){
  GmlRenderResource *resource=gml_render_surface_resource_ensure(r,id);
  GmlRenderResourceExecution result;
  int slot=surface_slot(id);
  if(!resource) return GML_RENDER_RESOURCE_EXEC_FAILED;
  result=gml_render_resource_submit(resource,gml_render_device(r),batch,sources,source_count);
  if(result!=GML_RENDER_RESOURCE_EXEC_FAILED){
    r->surface[slot].dirty=1;
    r->surface[slot].opaque_known=0;
    r->surface[slot].all_opaque=0;
    r->surface[slot].all_transparent=0;
  }
  return result;
}
GmlRenderResource *gml_render_application_resource_ensure(GmlRender *r){
  GmlRenderImageRef image;
  if(!r || !r->app_surface_owned || r->app_surface!=r->app_surface_owned ||
     r->app_w<=0 || r->app_h<=0) return NULL;
  if(r->app_surface_resource)
    return r->app_surface_resource_width==r->app_w &&
           r->app_surface_resource_height==r->app_h?r->app_surface_resource:NULL;
  if(r->next_surface_resource_incarnation==UINT64_MAX) return NULL;
  image=(GmlRenderImageRef){GML_RENDER_RESOURCE_APPLICATION,0u,
    ++r->next_surface_resource_incarnation,1u};
  r->app_surface_resource=gml_render_resource_create(image,(uint32_t)r->app_w,
    (uint32_t)r->app_h,GML_RENDER_PIXEL_ARGB8888,r->app_surface_owned,
    (GmlRenderResourceLimits){0});
  if(r->app_surface_resource){
    r->app_surface_resource_width=r->app_w;
    r->app_surface_resource_height=r->app_h;
  }
  return r->app_surface_resource;
}
void gml_render_application_resource_dispose(GmlRender *r){
  GmlRenderResource *resource;
  const GmlRenderDeviceServices *device;
  if(!r || !(resource=r->app_surface_resource)) return;
  (void)gml_render_application_scene_flush(r);
  gml_render_application_scene_queue_dispose(r);
  device=gml_render_device(r);
  if(device && device->retire)
    device->retire(device->context,gml_render_resource_image_ref(resource));
  gml_render_resource_destroy(resource);
  r->app_surface_resource=NULL;
  r->app_surface_resource_width=r->app_surface_resource_height=0;
}
GmlRenderResourceExecution gml_render_application_scene_submit(
  GmlRender *r,const GmlRenderDrawBatch *batch,
  const GmlRenderResourceSource *sources,uint32_t source_count){
  GmlRenderResource *resource=gml_render_application_resource_ensure(r);
  GmlRenderResourceExecution result;
  if(!resource || !gml_render_application_scene_flush(r))
    return GML_RENDER_RESOURCE_EXEC_FAILED;
  result=gml_render_resource_submit(resource,gml_render_device(r),batch,sources,source_count);
  if(result!=GML_RENDER_RESOURCE_EXEC_FAILED){
    r->app_surface_opaque=0;
    r->app_presentation_coverage_active=0;
  }
  return result;
}
int surface_known_opaque(GmlRender *r, int id){
  if(!r) return 0;
  if(id==0) return r->app_surface_opaque;
  int i=surface_slot(id);
  return i>=0 && r->surface[i].live && r->surface[i].opaque_known && r->surface[i].all_opaque;
}
int surface_known_transparent(GmlRender *r, int id){
  if(!r || id==0) return 0;
  int i=surface_slot(id);
  return i>=0 && r->surface[i].live && r->surface[i].all_transparent;
}
static int surface_alpha_all_zero(const uint32_t *px, size_t n){
  if(!px) return 0;
  for(size_t i=0;i<n;i++) if(px[i]>>24) return 0;
  return 1;
}
static void surface_store_target_coverage(GmlRender *r){
  if(!r) return;
  int i=surface_slot(r->target_id);
  if(i>=0 && r->surface[i].live){
    /* No-op draws and readbacks can conservatively dirty this bit; verify cleared targets
     * at target close so later composites can skip surfaces with no covered pixels. */
    if(!r->fb_all_transparent && r->fb_opaque_known && !r->fb_all_opaque &&
       r->fb && r->fbw>0 && r->fbh>0 &&
       surface_alpha_all_zero(r->fb,(size_t)r->fbw*(size_t)r->fbh)){
      r->fb_all_transparent=1;
    }
    r->surface[i].opaque_known=r->fb_opaque_known;
    r->surface[i].all_opaque=r->fb_all_opaque;
    r->surface[i].all_transparent=r->fb_all_transparent;
  }
}

int gml_surface_exists(GmlRender *r, int id){
  if(id==0 && (!r || !r->app_surface_published)) return 0;
  int w=0,h=0; return surface_pixels(r,id,&w,&h)!=NULL && w>0 && h>0;
}
void gml_render_application_surface_publish(GmlRender *r){
  if(r) r->app_surface_published=1;
}
int gml_surface_width(GmlRender *r, int id){
  int w=0,h=0; return surface_pixels(r,id,&w,&h)?w:0;
}
int gml_surface_height(GmlRender *r, int id){
  int w=0,h=0; return surface_pixels(r,id,&w,&h)?h:0;
}

/* A screen-stage compositor can choose either logical application-surface coordinates or explicit
 * window-pixel coordinates. The latter is observable when surface 0 covers exactly the current
 * target extent. Those dimensions are already physical: applying the logical-to-window GUI
 * transform again magnifies the image by the presentation scale and clips the trailing region. */
int gml_render_surface_draw_targets_screen_raster(const GmlRender *r,int surf,
                                                   double width,double height){
  return r && surf==0 && gml_render_gui_transform_active(r) &&
         r->target_sp==0 && r->fbw>0 && r->fbh>0 &&
         gml_render_gui_logical_width(r)<=(double)r->fbw+0.001 &&
         gml_render_gui_logical_height(r)<=(double)r->fbh+0.001 &&
         fabs(fabs(width)-(double)r->fbw)<0.001 &&
         fabs(fabs(height)-(double)r->fbh)<0.001;
}

void gml_draw_surface_stretched(GmlRender *r,int surf,double dx,double dy,
                                double dw,double dh,uint32_t blend,double alpha){
  if(!gml_render_resident_application_cpu_barrier(r)) return;
  /* A surface drawn onto the base canvas, rather than into another surface, is content compositing
   * its own screen. A bare refresh means only presentation, and a newly created transparent scratch
   * surface writes no pixels. An authored surface can still count after a transparent clear. */
  int slot=surface_slot(surf);
  if(r && r->target_id<0 && alpha>0.0 && slot>=0 &&
     (!surface_known_transparent(r,surf) ||
      (r->content_authored_surfaces&(UINT64_C(1)<<slot))))
    r->content_composited_screen=1;
  gml_render_cpu_draw_surface_stretched_impl(r,surf,dx,dy,dw,dh,blend,alpha,1);
}

void gml_draw_surface_part_ext(GmlRender *r,int surf,double sx,double sy,double sw,double sh,
                               double dx,double dy,double xs,double ys,uint32_t blend,double alpha){
  if(!gml_render_resident_application_cpu_barrier(r)) return;
  int explicit_target_raster=gml_render_surface_draw_targets_screen_raster(r,surf,sw*xs,sh*ys);
  if(!explicit_target_raster){
    gml_render_draw_map_point(r,&dx,&dy);
    gml_render_draw_map_scale(r,&xs,&ys);
  }
  gml_render_cpu_draw_surface_part_mapped(r,surf,sx,sy,sw,sh,dx,dy,xs,ys,blend,alpha);
}
void gml_draw_surface_ext(GmlRender *r,int surf,double x,double y,
                          double xs,double ys,double rot,uint32_t blend,double alpha){
  double rr=fmod(rot,360.0); if(rr<0) rr+=360.0;
  if(fabs(rr)<0.001 || fabs(rr-360.0)<0.001){
    int sw=gml_surface_width(r,surf),sh=gml_surface_height(r,surf);
    if(sw>0&&sh>0) gml_draw_surface_stretched(r,surf,x,y,sw*xs,sh*ys,blend,alpha);
    return;
  }
  if(!gml_render_resident_application_cpu_barrier(r)) return;
  gml_render_draw_map_point(r,&x,&y);
  gml_render_draw_map_scale(r,&xs,&ys);
  gml_render_cpu_draw_surface_ext_mapped(r,surf,x,y,xs,ys,rr,blend,alpha);
}
const uint32_t *gml_surface_pixels_read(GmlRender *r, int id, int *w, int *h){
  if(!r) return NULL;
  if(id==r->target_id) gml_render_maybe_prepare_draw(r);
  return surface_pixels(r,id,w,h);
}

int gml_surface_create(GmlRender *r, int w, int h){
  if(w<=0||h<=0||w>4096||h>4096) return -1;
  for(int k=0;k<GML_MAX_SURFACES;k++){
    int i=(r->next_surface_id+k-1)%GML_MAX_SURFACES;
    if(!r->surface[i].live){
      uint32_t *px=calloc((size_t)w*h,sizeof(uint32_t));
      if(!px) return -1;
      { uint8_t *rle=r->surface[i].rle; size_t cap=r->surface[i].rle_cap;   /* keep cache alloc across reuse */
        r->surface[i]=(GmlSurface){px,w,h,1,1,1,0,1,rle,0,cap,NULL}; }
      r->next_surface_id=i+2; if(r->next_surface_id>GML_MAX_SURFACES) r->next_surface_id=1;
      return i+1;
    }
  }
  return -1;
}
void gml_surface_free(GmlRender *r, int id){
  int i=surface_slot(id); if(i<0) return;
  gml_render_surface_resource_dispose(r,id);
  free(r->surface[i].px); free(r->surface[i].rle); memset(&r->surface[i],0,sizeof(r->surface[i]));
}
static int application_surface_resize(GmlRender *r,int w,int h){
  if(!r || w<=0 || h<=0 || w>4096 || h>4096 ||
     (size_t)w>SIZE_MAX/(size_t)h || (size_t)w*(size_t)h>SIZE_MAX/sizeof(uint32_t))
    return 0;
  if(!gml_render_application_scene_flush(r)) return 0;
  if(r->app_surface_owned && r->app_w==w && r->app_h==h &&
     (!r->app_surface_resource ||
      (r->app_surface_resource_width==w && r->app_surface_resource_height==h))) return 1;
  uint32_t *old=r->app_surface;
  int oldw=r->app_w, oldh=r->app_h;
  if(old==r->app_surface_owned && r->app_surface_resource){
    oldw=r->app_surface_resource_width;
    oldh=r->app_surface_resource_height;
    if(!gml_render_resource_acquire_cpu_read(r->app_surface_resource,NULL)) return 0;
  }
  uint32_t *px=calloc((size_t)w*(size_t)h,sizeof(uint32_t));
  if(!px) return 0;
  int cw=oldw<w?oldw:w, ch=oldh<h?oldh:h;
  if(old && cw>0 && ch>0)
    for(int y=0;y<ch;y++) memcpy(px+(size_t)y*w,old+(size_t)y*oldw,(size_t)cw*sizeof(uint32_t));
  uint32_t *owned=r->app_surface_owned;
  /* A game can resize surface 0 while it is the current/nested target. Keep every borrowed
   * target reference coherent before releasing the previous owned allocation. */
  if(r->fb==old){ r->fb=px; r->fbw=w; r->fbh=h; }
  if(r->base_fb==old){ r->base_fb=px; r->base_fbw=w; r->base_fbh=h; }
  for(int i=0;i<r->target_sp;i++) if(r->target_stack[i].fb==old){
    r->target_stack[i].fb=px; r->target_stack[i].w=w; r->target_stack[i].h=h;
  }
  r->app_surface_owned=px;
  r->app_surface=px; r->app_w=w; r->app_h=h;
  r->app_surface_opaque=0;
  r->app_presentation_coverage_active=0;
  gml_render_application_resource_dispose(r);
  free(owned);
  return 1;
}
int gml_render_application_surface_ensure_owned(GmlRender *r,int w,int h){
  return application_surface_resize(r,w,h);
}
void gml_surface_resize(GmlRender *r, int id, int w, int h){
  if(!r || w<=0 || h<=0 || w>4096 || h>4096) return;
  if(id==0){
    /* Surface zero follows the same explicit resize request in every generation. */
    if(!r->win) return;
    if(render_setting(r,"GML_LOG_SURF")) anygm_host_logf(r && r->win ? r->win->host : NULL,ANYGM_LOG_DEBUG,"[surf] resize application_surface %dx%d (was %dx%d)\n",w,h,r->app_w,r->app_h);
    (void)application_surface_resize(r,w,h);
    return;
  }
  int i=surface_slot(id); if(i<0 || !r->surface[i].live || w<=0 || h<=0 || w>4096 || h>4096) return;
  uint32_t *px=calloc((size_t)w*h,sizeof(uint32_t));
  if(!px) return;
  if(r->surface[i].resource &&
     !gml_render_resource_acquire_cpu_read(r->surface[i].resource,NULL)){
    free(px);
    return;
  }
  int was_transparent=r->surface[i].all_transparent;
  int cw=w<r->surface[i].w?w:r->surface[i].w, ch=h<r->surface[i].h?h:r->surface[i].h;
  for(int y=0;y<ch;y++) memcpy(px+(size_t)y*w,r->surface[i].px+(size_t)y*r->surface[i].w,(size_t)cw*sizeof(uint32_t));
  gml_render_surface_resource_dispose(r,id);
  free(r->surface[i].px); r->surface[i].px=px; r->surface[i].w=w; r->surface[i].h=h;
  r->surface[i].dirty=1;
  r->surface[i].opaque_known=0;
  r->surface[i].all_opaque=0;
  r->surface[i].all_transparent=was_transparent;
}
int gml_surface_set_target(GmlRender *r, int id){
  if(!gml_render_application_scene_flush(r)) return 0;
  int w=0,h=0; uint32_t *px=surface_pixels(r,id,&w,&h);
  if(!px || r->target_sp>=GML_SURFACE_STACK) return 0;
  /* Classic target assignment replaces the current target; one reset returns to the base. Other runtime families retain nested targets. */
  if(r->classic && r->target_id>=0){
    gml_surface_reset_target(r);
    px=surface_pixels(r,id,&w,&h);
    if(!px || r->target_sp>=GML_SURFACE_STACK) return 0;
  }
  int si=surface_slot(id);
  { if(si>=0) r->surface[si].dirty=1; }   /* about to be drawn into */
  r->target_stack[r->target_sp++]=(typeof(r->target_stack[0])){
    r->fb,r->fbw,r->fbh,r->cam_x,r->cam_y,r->projection_cam_x,r->projection_cam_y,
    r->target_id,r->fb_opaque_known,r->fb_all_opaque,r->fb_all_transparent,
    r->pending_underlay,r->underlay_x,r->underlay_y,r->underlay_w,r->underlay_h,
    r->pending_fill,r->pending_fill_color
  };
  if(render_setting(r,"GML_LOG_SURF")){
    /* Count existing RGB and alpha before this surface becomes the active target. */
    int lit_=0,cov_=0;
    surface_coverage_counts(px,w,h,&lit_,&cov_);
    anygm_host_logf(r->win?r->win->host:NULL,ANYGM_LOG_DEBUG,
      "[surf] set_target %d px=%p lit_on_entry=%d covered_on_entry=%d of %d\n",
      id,(const void *)px,lit_,cov_,w*h);
  }
  r->fb=px; r->fbw=w; r->fbh=h;
  r->target_id=id;
  r->fb_opaque_known=(si>=0)?r->surface[si].opaque_known:0;
  r->fb_all_opaque=(si>=0)?r->surface[si].all_opaque:0;
  r->fb_all_transparent=(si>=0)?r->surface[si].all_transparent:0;
  r->pending_underlay=0;
  r->underlay_x=r->underlay_y=r->underlay_w=r->underlay_h=0;
  r->pending_fill=0;
  r->pending_fill_color=0;
  /* GM resets the projection to the surface's own coordinates while a surface target is
   * active: draws inside it must NOT be shifted by the room camera/GUI offset. */
  r->projection_cam_x=0; r->projection_cam_y=0;
  r->cam_x=0; r->cam_y=0;
  if(id==r->surface_canvas && r->surface_canvas_width>0 && w>=r->surface_canvas_width)
    r->projection_cam_x=r->cam_x=-((w-r->surface_canvas_width)/2);
  gml_d3_sync_render_camera(r);
  if(render_setting(r,"GML_LOG_SURF")){
    /* Count both channels again at function exit, paired with the entry counts. */
    int lit_=0,cov_=0;
    surface_coverage_counts(px,w,h,&lit_,&cov_);
    anygm_host_logf(r->win?r->win->host:NULL,ANYGM_LOG_DEBUG,
      "[surf] set_target %d lit_on_exit=%d covered_on_exit=%d of %d\n",id,lit_,cov_,w*h);
  }
  return 1;
}
void gml_surface_reset_target(GmlRender *r){
  if(!gml_render_application_scene_flush(r)) return;
  /* Count colour coverage on both sides of the pending-draw flush while the
   * active surface is still the render target. */
  int dbg_=r && render_setting(r,"GML_LOG_SURF")!=NULL && r->fb && r->fbw>0 && r->fbh>0;
  int released_=r?r->target_id:-1;   /* captured before the stack pop restores the previous one */
  int before_=0,before_cov_=0;
  if(dbg_) surface_coverage_counts(r->fb,r->fbw,r->fbh,&before_,&before_cov_);
  if(r && surface_slot(r->target_id)>=0){
    /* Remember that content really authored this surface before a deferred transparent clear is
     * resolved. A later screen blit of that now-transparent surface is still a compositor action;
     * this is distinct from blitting a scratch surface that received no draw at all. */
    if(!r->fb_all_transparent)
      r->content_authored_surfaces|=UINT64_C(1)<<(r->target_id-1);
    gml_render_flush_pending_underlay(r);
    gml_render_flush_pending_fill(r);
    surface_store_target_coverage(r);
  }
  if(dbg_){
    int after_=0,after_cov_=0;
    surface_coverage_counts(r->fb,r->fbw,r->fbh,&after_,&after_cov_);
    anygm_host_logf(r->win?r->win->host:NULL,ANYGM_LOG_DEBUG,
      "[surf] reset_target released=%d px=%p lit_before=%d lit_after=%d"
      " covered_before=%d covered_after=%d of %d\n",
      released_,(const void *)r->fb,before_,after_,before_cov_,after_cov_,r->fbw*r->fbh);
  }
  if(r->target_sp>0){
    typeof(r->target_stack[0]) t=r->target_stack[--r->target_sp];
    r->fb=t.fb; r->fbw=t.w; r->fbh=t.h;
    r->projection_cam_x=t.projection_cx; r->projection_cam_y=t.projection_cy;
    r->cam_x=t.cx; r->cam_y=t.cy;
    gml_d3_sync_render_camera(r);
    r->target_id=t.target_id;
    r->fb_opaque_known=t.opaque_known;
    r->fb_all_opaque=t.all_opaque;
    r->fb_all_transparent=t.all_transparent;
    r->pending_underlay=t.pending_underlay;
    r->underlay_x=t.underlay_x;
    r->underlay_y=t.underlay_y;
    r->underlay_w=t.underlay_w;
    r->underlay_h=t.underlay_h;
    r->pending_fill=t.pending_fill;
    r->pending_fill_color=t.fill_color;
  } else if(r->base_fb){
    r->fb=r->base_fb; r->fbw=r->base_fbw; r->fbh=r->base_fbh;
    r->target_id=-1;
    r->fb_opaque_known=0;
    r->fb_all_opaque=0;
    r->fb_all_transparent=0;
    r->pending_underlay=0;
    r->underlay_x=r->underlay_y=r->underlay_w=r->underlay_h=0;
    r->pending_fill=0;
    r->pending_fill_color=0;
  } else {
    r->target_id=-1;
    r->fb_opaque_known=0;
    r->fb_all_opaque=0;
    r->fb_all_transparent=0;
    r->pending_underlay=0;
    r->underlay_x=r->underlay_y=r->underlay_w=r->underlay_h=0;
    r->pending_fill=0;
    r->pending_fill_color=0;
  }
}
int gml_surface_get_target(GmlRender *r){
  return r ? r->target_id : -1;
}
/* Retain a bound program target across a new pass while replacing the base buffer that a later target reset returns to. With no target, use ordinary pass initialization. */
static void begin_retaining_target(GmlRender *r, uint32_t *fb, int w, int h,
                                   double cx, double cy,int resident_selection){
  if(!r || r->target_id<0 || r->target_sp<=0){ gml_render_begin(r,fb,w,h,cx,cy); return; }
  int retained=r->target_id;
  int depth=r->target_sp;
  /* The coverage of a bound target is tracked live and only written back to the surface when the
   * target closes, so it has to cross this call rather than be re-read: a surface the draw phase
   * has just filled still reads transparent in its own record, and a later composite of it would
   * be skipped as empty. */
  int retained_opaque_known=r->fb_opaque_known;
  int retained_all_opaque=r->fb_all_opaque;
  int retained_all_transparent=r->fb_all_transparent;
  typeof(r->target_stack[0]) saved[GML_SURFACE_STACK];
  memcpy(saved,r->target_stack,sizeof(saved));
  gml_render_begin(r,fb,w,h,cx,cy);
  int sw=0,sh=0;
  int si=surface_slot(retained);
  uint32_t *px=NULL;
  if(resident_selection && retained==0 && r->app_surface_resource &&
     r->app_surface==r->app_surface_owned){
    px=r->app_surface_owned;
    sw=r->app_surface_resource_width;
    sh=r->app_surface_resource_height;
  } else if(resident_selection && si>=0 && r->surface[si].live &&
            r->surface[si].resource){
    /* Retaining a target only needs its stable allocation and extent. The CPU accessor would
     * replay a resident journal and mark the GPU copy stale before this pass draws anything. */
    px=r->surface[si].px;
    sw=r->surface[si].w;
    sh=r->surface[si].h;
  } else
    px=surface_pixels(r,retained,&sw,&sh);
  if(!px) return;   /* the surface was freed under the binding: the pass owns the base buffer */
  memcpy(r->target_stack,saved,sizeof(r->target_stack));
  r->target_sp=depth;
  /* The bottom entry described the base of the pass that pushed it, which no longer exists. */
  r->target_stack[0].fb=fb;
  r->target_stack[0].w=w;
  r->target_stack[0].h=h;
  r->target_stack[0].cx=cx;
  r->target_stack[0].cy=cy;
  r->target_stack[0].projection_cx=cx;
  r->target_stack[0].projection_cy=cy;
  r->target_stack[0].target_id=-1;
  r->target_stack[0].opaque_known=0;
  r->target_stack[0].all_opaque=0;
  r->target_stack[0].all_transparent=0;
  r->target_stack[0].pending_underlay=0;
  r->target_stack[0].underlay_x=r->target_stack[0].underlay_y=0;
  r->target_stack[0].underlay_w=r->target_stack[0].underlay_h=0;
  r->target_stack[0].pending_fill=0;
  r->target_stack[0].fill_color=0;
  r->fb=px; r->fbw=sw; r->fbh=sh;
  r->target_id=retained;
  r->fb_opaque_known=retained_opaque_known;
  r->fb_all_opaque=retained_all_opaque;
  r->fb_all_transparent=retained_all_transparent;
  if(si>=0) r->surface[si].dirty=1;
}
void gml_render_begin_retaining_target(GmlRender *r, uint32_t *fb, int w, int h,
                                       double cx, double cy){
  begin_retaining_target(r,fb,w,h,cx,cy,0);
}
void gml_render_begin_retaining_resident_target(GmlRender *r,uint32_t *fb,int w,int h,
                                                double cx,double cy){
  begin_retaining_target(r,fb,w,h,cx,cy,1);
}
