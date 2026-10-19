/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Shared primitive geometry and the existing rectangle shader-preparation decision. */
#include "gml_render.h"
#include "gml_render_internal.h"
#include "gml_render_cpu_primitives.h"
#include "gml_render_cpu_backend.h"
#include "gml_render_surfaces_internal.h"
#include "gml_render_shaders.h"
#include "gml_render_texture.h"

#include <math.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* Opaque flat coverage has the same half-open pixel region for point-sized and filled
 * rectangles. Typed entries already receive raster coordinates, so the current world
 * scale must not reject or remap them. Keep unqualified shapes in their CPU raster body. */
static int resident_flat_eligible_rule(GmlRender *render,int filled_rectangle,int partial_alpha,
                                        int blendmode){
  return render && render->resident_application_pass && gml_render_device(render) &&
    gml_render_scene_target_matches(render) &&
    /* Filled Studio rectangles already carry the builtin's resolved GUI raster bounds. */
    (!render->gui_pass_active || (filled_rectangle && !render->classic)) &&
    (!render->classic || (filled_rectangle &&
      !anygm_policy_classic_modern_presentation(render->win) &&
      render->fb==render->app_surface &&
      (render->fb==render->base_scene_resource_pixels ||
       /* Opaque flat writes also have exact alpha on the owned application.
        * Partial/inverse rectangles retain their borrowed opaque SCREEN scope. */
       (render->fb==render->app_surface_owned && render->alpha==1.0 && !blendmode)))) &&
    !render->classic_phase_y && !render->classic_interp_phase[0] &&
    !render->classic_interp_phase[1] && !render->classic_interp_phase[2] &&
    render->active_shader<0 &&
    (partial_alpha ? isfinite(render->alpha) && render->alpha>0.0 && render->alpha<1.0
                   : render->alpha==1.0) && render->blendmode==blendmode &&
    render->blend_equation==1 && render->blend_equation_alpha==1 &&
    render->color_write_mask==15u && !render->alpha_test_enable &&
    !render->pending_presentation && !render->rotated_batch_count &&
    !render->rotated_batch_building;
}

static int resident_flat_eligible(GmlRender *render){
  return resident_flat_eligible_rule(render,0,0,0);
}

int gml_render_try_particle_line(GmlRender *r,double x,double y,
                                 double xs,double ys,double angle,
                                 uint32_t colour,double alpha){
  const GmlRenderDeviceServices *device=gml_render_device(r);
  if(!r || !r->resident_application_pass || !device || !device->observe_on_demand ||
     !device->observe_on_demand(device->context) || !gml_render_scene_target_matches(r) ||
     r->active_shader>=0 || r->color_write_mask!=15u || r->alpha_test_enable ||
     r->blendmode || r->blend_equation!=1 || r->blend_equation_alpha!=1 ||
     r->pending_presentation || r->rotated_batch_count || r->rotated_batch_building ||
     (r->target_sp==0 && r->fb==r->base_fb &&
      (r->classic_interp_phase[0] || r->classic_interp_phase[1] || r->classic_interp_phase[2])) ||
     !isfinite(x) || !isfinite(y) || !isfinite(xs) || !isfinite(ys) ||
     !isfinite(angle) || !isfinite(alpha) || alpha<=0.0 || alpha>1.0 ||
     fabs(xs)<1.0/128.0 || fabs(ys)<1.0/128.0 || fabs(xs)>128.0 || fabs(ys)>128.0 ||
     fabs(x)>16384.0 || fabs(y)>16384.0 || fabs(angle)>360.0) return 0;
  double radians=angle*3.14159265358979323846/180.0;
  double co=cos(radians),si=sin(radians);
  double ex=fabs(co)*29.0*fabs(xs)+fabs(si)*6.0*fabs(ys)+1.0;
  double ey=fabs(si)*29.0*fabs(xs)+fabs(co)*6.0*fabs(ys)+1.0;
  double x0=fmax(0.0,floor(x-ex)),x1=fmin((double)r->fbw-1.0,ceil(x+ex));
  double y0=fmax(0.0,floor(y-ey)),y1=fmin((double)r->fbh-1.0,ceil(y+ey));
  if(x1<x0 || y1<y0) return 1;
  if(r->pending_underlay && !gml_render_compose_resident_underlay(r)) return 0;
  gml_render_flush_pending_fill(r);
  GmlRenderResource *target=gml_render_scene_target_resource_ensure(r);
  if(r->pending_fill || !target) return 0;
  GmlRenderImageRef next=gml_render_application_scene_next_image_ref(r,target);
  if(next.content_version==UINT64_MAX) return 0;
  GmlRenderDrawOp operation={0};
  operation.opcode=GML_RENDER_OP_COLOURED_PRIMITIVE;
  operation.scene_family=GML_RENDER_SCENE_PRIMITIVE;
  operation.reconstruction_mode=GML_RENDER_DEVICE_CHECKPOINT;
  operation.target=(GmlRenderTargetRef){.image=next,.storage_version=1u,
    .width=(uint32_t)r->fbw,.height=(uint32_t)r->fbh,
    .pixel_format=GML_RENDER_PIXEL_ARGB8888,
    .region={0,0,(uint32_t)r->fbw,(uint32_t)r->fbh}};
  operation.target.image.content_version++;
  operation.payload.primitive=(GmlRenderPrimitiveRect){
    .region={(int32_t)x0,(int32_t)y0,(uint32_t)(x1-x0+1.0),(uint32_t)(y1-y0+1.0)},
    .colour=gml_render_cpu_backend_color_to_xrgb(colour),
    .kind=GML_RENDER_PRIMITIVE_PARTICLE_LINE,.centre_x=x,.centre_y=y,
    .scale_x=xs,.scale_y=ys,.angle=angle,.alpha=alpha,
    .interpolate=r->interp?1u:0u,.alpha_blend=r->alphablend?1u:0u};
  if(!gml_render_application_scene_enqueue_primitive(r,&operation)) return 0;
  r->fb_all_transparent=0;
  r->fb_opaque_known=r->fb_all_opaque=0;
  return 1;
}

static int resident_opaque_rect(GmlRender *render,int x1,int y1,int x2,int y2,
                                uint32_t color,int classic_rectangle){
  GmlRenderResource *resource;
  GmlRenderImageRef image;
  GmlRenderDrawOp operation={0};
  GmlRenderDrawBatch batch;
  GmlRenderResourceExecution result;
  /* This entry receives the existing CPU rectangle's inclusive raster bounds. Early
   * classic filled rectangles share that exact coverage; other shapes keep their gate. */
  if(!resident_flat_eligible_rule(render,classic_rectangle,0,0)) return 0;
  if(x1>x2){int swap=x1;x1=x2;x2=swap;}
  if(y1>y2){int swap=y1;y1=y2;y2=swap;}
  if(x2<0 || y2<0 || x1>=render->fbw || y1>=render->fbh) return 0;
  if(x1<0) x1=0;
  if(y1<0) y1=0;
  if(x2>=render->fbw) x2=render->fbw-1;
  if(y2>=render->fbh) y2=render->fbh-1;
  gml_render_maybe_prepare_opaque_rect(render,x1,y1,x2+1,y2+1);
  if(render->pending_fill || render->pending_underlay ||
     !render->resident_application_pass) return 0;
  resource=gml_render_scene_target_resource_ensure(render);
  if(!resource) return 0;
  image=gml_render_application_scene_next_image_ref(render,resource);
  if(image.content_version==UINT64_MAX) return 0;
  operation.opcode=GML_RENDER_OP_COLOURED_PRIMITIVE;
  operation.scene_family=GML_RENDER_SCENE_PRIMITIVE;
  operation.reconstruction_mode=GML_RENDER_REPLAY_EXACT;
  operation.target.image=image;
  operation.target.image.content_version++;
  operation.target.storage_version=1u;
  operation.target.width=(uint32_t)render->fbw;
  operation.target.height=(uint32_t)render->fbh;
  operation.target.pixel_format=GML_RENDER_PIXEL_ARGB8888;
  operation.target.region=(GmlRenderRect){0,0,(uint32_t)render->fbw,(uint32_t)render->fbh};
  operation.payload.primitive.region=(GmlRenderRect){x1,y1,(uint32_t)(x2-x1+1),
                                                      (uint32_t)(y2-y1+1)};
  operation.payload.primitive.colour=gml_render_cpu_backend_color_to_xrgb(color);
  batch=(GmlRenderDrawBatch){&operation,1u,NULL,0u};
  result=gml_render_application_scene_submit(render,&batch,NULL,0u);
  if(result==GML_RENDER_RESOURCE_EXEC_CPU) render->resident_application_pass=0;
  return result!=GML_RENDER_RESOURCE_EXEC_FAILED;
}

int gml_render_try_opaque_primitive_rect(GmlRender *render,int x1,int y1,int x2,int y2,
                                         uint32_t color){
  return resident_opaque_rect(render,x1,y1,x2,y2,color,0);
}

static int resident_classic_alpha_rect(GmlRender *render,int x1,int y1,int x2,int y2,
                                       uint32_t color){
  GmlRenderDrawOp operation={0};
  const GmlRenderDeviceServices *device=gml_render_device(render);
  int inverse=render && render->blendmode==2;
  /* Only the borrowed opaque classic raster has qualified target-alpha semantics.
   * The CPU receives these same inclusive, already transformed integer bounds. */
  if(!render || !render->classic || !render->alphablend ||
     render->target_id>=0 || render->target_sp ||
     !resident_flat_eligible_rule(render,1,!inverse,inverse?2:0) ||
     !render->fb_opaque_known || !render->fb_all_opaque ||
     !device || !device->observe_on_demand ||
     !device->observe_on_demand(device->context)) return 0;
  if(x1>x2){int swap=x1;x1=x2;x2=swap;}
  if(y1>y2){int swap=y1;y1=y2;y2=swap;}
  if(x2<0 || y2<0 || x1>=render->fbw || y1>=render->fbh) return 1;
  if(x1<0) x1=0;
  if(y1<0) y1=0;
  if(x2>=render->fbw) x2=render->fbw-1;
  if(y2>=render->fbh) y2=render->fbh-1;
  gml_render_flush_pending_fill(render);
  GmlRenderResource *resource=gml_render_scene_target_resource_ensure(render);
  if(render->pending_fill || !resource) return 0;
  GmlRenderImageRef image=gml_render_application_scene_next_image_ref(render,resource);
  if(image.content_version==UINT64_MAX) return 0;
  operation.opcode=GML_RENDER_OP_COLOURED_PRIMITIVE;
  operation.scene_family=GML_RENDER_SCENE_PRIMITIVE;
  operation.reconstruction_mode=GML_RENDER_DEVICE_CHECKPOINT;
  operation.target.image=image;
  operation.target.image.content_version++;
  operation.target.storage_version=1u;
  operation.target.width=(uint32_t)render->fbw;
  operation.target.height=(uint32_t)render->fbh;
  operation.target.pixel_format=GML_RENDER_PIXEL_ARGB8888;
  operation.target.region=(GmlRenderRect){0,0,(uint32_t)render->fbw,(uint32_t)render->fbh};
  operation.payload.primitive=(GmlRenderPrimitiveRect){
    .region={x1,y1,(uint32_t)(x2-x1+1),(uint32_t)(y2-y1+1)},
    .colour=gml_render_cpu_backend_color_to_xrgb(color),
    .kind=inverse?GML_RENDER_PRIMITIVE_CLASSIC_INVERSE_RECT:
                  GML_RENDER_PRIMITIVE_CLASSIC_ALPHA_RECT,
    .alpha=render->alpha,.alpha_blend=1u};
  return gml_render_application_scene_enqueue_primitive(render,&operation);
}

typedef struct {
  GmlRenderRect region;
  uint32_t colour;
} GmlFlatRegion;

/* R03-PRIM-STUDIO-ALPHA-RECT: the existing CPU filled rectangle uses a constant
 * source and inclusive raster bounds. Device checkpoints retain its opaque
 * written alpha; RGB uses fixed source-over within the tolerance contract. */
static int resident_studio_alpha_rect(GmlRender *render,int x1,int y1,int x2,int y2,
                                      uint32_t colour){
  const GmlRenderDeviceServices *device=gml_render_device(render);
  if(!render || render->classic || !render->win ||
     !anygm_policy_uses_first_generation_studio(render->win) ||
     anygm_policy_has_modern_layer_semantics(render->win) ||
     /* Only initialized identity GUI joins the existing resolved-raster path. */
     (render->gui_pass_active &&
      (render->gui_base_logical_w!=render->fbw || render->gui_base_logical_h!=render->fbh ||
       render->gui_logical_w!=render->fbw || render->gui_logical_h!=render->fbh ||
       render->gui_scale_x!=1.0 || render->gui_scale_y!=1.0 ||
       render->gui_maximise_active || render->gui_maximise_xoffset!=0.0 ||
       render->gui_maximise_yoffset!=0.0 || render->cam_x!=0.0 || render->cam_y!=0.0)) ||
     render->target_id>=0 || render->target_sp ||
     render->pending_underlay || !render->alphablend ||
     render->alpha<1.0/256.0 ||
     !resident_flat_eligible_rule(render,1,1,0) ||
     !device || !device->observe_on_demand ||
     !device->observe_on_demand(device->context)) return 0;
  if(x1>x2){int swap=x1;x1=x2;x2=swap;}
  if(y1>y2){int swap=y1;y1=y2;y2=swap;}
  if(x2<0 || y2<0 || x1>=render->fbw || y1>=render->fbh) return 0;
  if(x1<0) x1=0;
  if(y1<0) y1=0;
  if(x2>=render->fbw) x2=render->fbw-1;
  if(y2>=render->fbh) y2=render->fbh-1;
  gml_render_flush_pending_fill(render);
  GmlRenderResource *resource=gml_render_scene_target_resource_ensure(render);
  if(render->pending_fill || !resource) return 0;
  GmlRenderImageRef image=gml_render_application_scene_next_image_ref(render,resource);
  if(image.content_version==UINT64_MAX) return 0;
  GmlRenderDrawOp operation={0};
  operation.opcode=GML_RENDER_OP_COLOURED_PRIMITIVE;
  operation.scene_family=GML_RENDER_SCENE_PRIMITIVE;
  operation.reconstruction_mode=GML_RENDER_DEVICE_CHECKPOINT;
  operation.target=(GmlRenderTargetRef){.image=image,.storage_version=1u,
    .width=(uint32_t)render->fbw,.height=(uint32_t)render->fbh,
    .pixel_format=GML_RENDER_PIXEL_ARGB8888,
    .region={0,0,(uint32_t)render->fbw,(uint32_t)render->fbh}};
  operation.target.image.content_version++;
  operation.payload.primitive=(GmlRenderPrimitiveRect){
    .region={x1,y1,(uint32_t)(x2-x1+1),(uint32_t)(y2-y1+1)},
    .colour=gml_render_cpu_backend_color_to_xrgb(colour),
    .kind=GML_RENDER_PRIMITIVE_STUDIO_ALPHA_RECT,
    .alpha=render->alpha,.alpha_blend=1u};
  if(!gml_render_application_scene_enqueue_primitive(render,&operation)) return 0;
  render->fb_all_transparent=0;
  int opaque=(render->fb_opaque_known && render->fb_all_opaque) ||
    (x1==0 && y1==0 && x2==render->fbw-1 && y2==render->fbh-1);
  render->fb_opaque_known=render->fb_all_opaque=
    opaque;
  return 1;
}

/* R03-PRIM-RECTANGLE-MATRIX: all rectangle ports already carry CPU raster bounds.
 * Keep the cheaper qualified flat paths, and batch ordinary rectangle presets
 * without a predecessor snapshot or CPU pixel transfer. Raw cutout does not
 * participate in these unchanged CPU rectangle writers. */
static int resident_primitive_state_eligible_rule(GmlRender *r,int vertex){
  const GmlRenderDeviceServices *device=gml_render_device(r);
  if(!r || !r->resident_application_pass || !device || !device->observe_on_demand ||
     !device->observe_on_demand(device->context) || !gml_render_scene_target_matches(r) ||
     (r->active_shader>=0 && (!vertex || !gml_render_shader_cpu_passthrough(r))) || r->blendmode<0 ||
     r->blendmode>GML_RENDER_BLEND_DEST_ALPHA_DEST_COLOUR || r->blend_equation!=1 ||
     r->blend_equation_alpha!=1 || r->color_write_mask>15u ||
     r->pending_presentation || r->rotated_batch_count || r->rotated_batch_building ||
     (gml_render_classic_vertical_phase_blocks_scene(r) &&
       (!r->classic_phase_resource || !gml_render_classic_phase_resource_bind(r))) ||
     r->classic_interp_phase[0] ||
     r->classic_interp_phase[1] || r->classic_interp_phase[2] || !isfinite(r->alpha)) return 0;
  return 1;
}

static int resident_primitive_state_eligible(GmlRender *r){
  return resident_primitive_state_eligible_rule(r,0);
}

static int resident_rectangle_state_eligible(GmlRender *r){
  return resident_primitive_state_eligible(r) && r->color_write_mask==15u;
}

int gml_render_cpu_backend_vertex_spans(GmlRender *r,
  const GmlRenderCpuBackendVertexSpan *spans,uint32_t count){
  if(!resident_primitive_state_eligible(r) || r->classic_phase_scene_target ||
     r->app_interp_phase[0] || r->app_interp_phase[1] || r->app_interp_phase[2] ||
     count>8192u || (count && !spans)) return 0;
  if(!count) return spans==NULL;
  unsigned blend=(unsigned)r->blendmode;
  if(blend==3u || blend==6u) blend=2u;
  int destination=r->alphablend && gml_render_blend_uses_destination_colour((int)blend);
  unsigned mask=r->alphablend && (destination || blend==4u || blend==7u || blend==8u)?
    r->color_write_mask:15u;
  /* The CPU maximum preset preserves only explicitly bound surface alpha;
   * ordinary pixel presets also preserve the first-generation application. */
  int preserve=r->alphablend && blend==4u?r->target_sp>0:gml_render_target_preserves_alpha(r);
  GmlRenderDrawOp *operations=calloc(count,sizeof *operations);
  if(!operations) return 0;
  int result=0;
  for(uint32_t i=0;i<count;i++){
    const GmlRenderCpuBackendVertexSpan *span=&spans[i];
    if(span->x<0 || span->y<0 || !span->count || span->y>=r->fbh ||
       (uint64_t)(unsigned)span->x+span->count>(unsigned)r->fbw) goto vertex_done;
    GmlRenderPrimitiveRect *p=&operations[i].payload.primitive;
    p->region=p->authored_region=(GmlRenderRect){span->x,span->y,span->count,1u};
    p->kind=GML_RENDER_PRIMITIVE_RECTANGLE;
    p->colour=0xFFFFFFFFu;
    for(unsigned c=0;c<4u;c++) p->corner_colour[c]=p->colour;
    p->alpha=1.0;p->alpha_blend=r->alphablend?1u:0u;p->pixel_write_mask=mask;
    p->rectangle_flags=GML_RENDER_RECTANGLE_OUTLINE|GML_RENDER_RECTANGLE_PIXEL_SPAN|
      GML_RENDER_RECTANGLE_VERTEX_SPAN|(r->classic?GML_RENDER_RECTANGLE_CLASSIC:0u)|
      (preserve?GML_RENDER_RECTANGLE_TARGET_ALPHA:0u)|
      (blend<<GML_RENDER_RECTANGLE_BLEND_SHIFT);
    memcpy(p->vertex_first,span->first,sizeof p->vertex_first);
    memcpy(p->vertex_step,span->step,sizeof p->vertex_step);
    for(unsigned c=0;c<4u;c++){
      double last=span->first[c]+(span->count-1u)*span->step[c];
      double maximum=c==3u?1.0:255.0;
      if(!isfinite(span->first[c]) || !isfinite(span->step[c]) || !isfinite(last) ||
         span->first[c]<-1e-5 || span->first[c]>maximum+1e-5 ||
         last<-1e-5 || last>maximum+1e-5) goto vertex_done;
    }
  }
  if(r->pending_underlay && !gml_render_compose_resident_underlay(r)) goto vertex_done;
  gml_render_flush_pending_fill(r);
  GmlRenderResource *target=gml_render_scene_target_resource_ensure(r);
  if(r->pending_fill || !target) goto vertex_done;
  GmlRenderImageRef next=gml_render_application_scene_next_image_ref(r,target);
  if(next.content_version>UINT64_MAX-count) goto vertex_done;
  for(uint32_t i=0;i<count;i++){
    GmlRenderDrawOp *op=&operations[i];
    op->opcode=GML_RENDER_OP_COLOURED_PRIMITIVE;op->scene_family=GML_RENDER_SCENE_PRIMITIVE;
    op->reconstruction_mode=GML_RENDER_DEVICE_CHECKPOINT;
    op->target=(GmlRenderTargetRef){next,1u,(uint32_t)r->fbw,(uint32_t)r->fbh,
      GML_RENDER_PIXEL_ARGB8888,{0,0,(uint32_t)r->fbw,(uint32_t)r->fbh}};
    op->target.image.content_version+=i+1u;
  }
  result=gml_render_application_scene_enqueue_primitives(r,operations,count);
  if(result){r->fb_all_transparent=0;r->fb_opaque_known=r->fb_all_opaque=0;}
vertex_done:
  free(operations);
  return result;
}

static int vertex_texture_source(GmlRender *r,int texture,GmlRenderResourceSource *source,int query){
  uint32_t encoded=(uint32_t)texture,kind=encoded&GML_TEX_KIND_MASK;
  if(texture==-1) return 0;
  if(kind==GML_TEX_SURF_TAG){
    int id=(int)(encoded&65535u),slot=id?surface_slot(id):-1;
    if(id && (slot<0 || !r->surface[slot].px)) return 0;
    const uint32_t *pixels=id?r->surface[slot].px:r->app_surface;
    if(!pixels || pixels==r->fb) return 0;
    if(query){
      source->width=(uint32_t)(id?r->surface[slot].w:r->app_w);
      source->height=(uint32_t)(id?r->surface[slot].h:r->app_h);
      return source->width && source->height;
    }
    if(!gml_render_application_scene_flush(r)) return 0;
    GmlRenderResource *resource=gml_render_surface_resource_ensure(r,id);
    /* Preserve the source owner's device/version receipt after its first
     * upload; a borrowed CPU view would upload the same mutable image again. */
    return resource && gml_render_resource_prepare_device_source(resource,gml_render_device(r),source);
  }
  /* The established resolver lends immutable atlas metadata without observing
   * a device image. Runtime RGBA sprites retain their separate mutable owner. */
  GmlRenderCpuBackendTextureView view;
  if(!gml_render_cpu_backend_texture_view(r,texture,1,&view) || view.runtime ||
     view.atlas_index<0 || view.pixel_kind!=GML_RENDER_CPU_BACKEND_PIXELS_RGBA) return 0;
  if(query){source->width=(uint32_t)view.width;source->height=(uint32_t)view.height;return 1;}
  return gml_render_atlas_scene_source_view(r,view.atlas_index,source);
}

int gml_render_cpu_backend_vertex_texture_extent(GmlRender *r,int texture,int *width,int *height){
  GmlRenderResourceSource source={0};
  if(width) *width=0;
  if(height) *height=0;
  if(!width || !height || !resident_primitive_state_eligible_rule(r,1) ||
     !vertex_texture_source(r,texture,&source,1) || source.width>16384u ||
     source.height>16384u) return 0;
  *width=(int)source.width;*height=(int)source.height;return 1;
}

int gml_render_cpu_backend_textured_vertex_spans(GmlRender *r,int texture,
  const GmlRenderCpuBackendVertexSpan *spans,uint32_t count){
  if(!resident_primitive_state_eligible_rule(r,1) || r->classic_phase_scene_target ||
     r->app_interp_phase[0] || r->app_interp_phase[1] || r->app_interp_phase[2] ||
     count>65536u || (count && !spans)) return 0;
  GmlRenderResourceSource source={0};
  if(!vertex_texture_source(r,texture,&source,!count)) return 0;
  if(!count) return spans==NULL;
  unsigned blend=(unsigned)r->blendmode;
  if(blend==3u || blend==6u) blend=2u;
  int destination=r->alphablend && gml_render_blend_uses_destination_colour((int)blend);
  unsigned mask=r->alphablend && (destination || blend==4u || blend==7u || blend==8u)?
    r->color_write_mask:15u;
  int preserve=r->alphablend && blend==4u?r->target_sp>0:gml_render_target_preserves_alpha(r);
  GmlRenderDrawOp *operations=calloc(count,sizeof *operations);
  if(!operations) return 0;
  int result=0;
  GmlRenderResource *target=gml_render_scene_target_resource_ensure(r);
  if(!target) goto textured_done;
  GmlRenderImageRef next=gml_render_application_scene_next_image_ref(r,target);
  if(next.content_version>UINT64_MAX-count ||
     (next.resource_class==source.image.resource_class && next.identity==source.image.identity &&
      next.incarnation==source.image.incarnation)) goto textured_done;
  for(uint32_t i=0;i<count;i++){
    const GmlRenderCpuBackendVertexSpan *span=&spans[i];
    GmlRenderDrawOp *op=&operations[i];
    op->opcode=GML_RENDER_OP_COLOURED_PRIMITIVE;op->scene_family=GML_RENDER_SCENE_PRIMITIVE;
    op->reconstruction_mode=GML_RENDER_DEVICE_CHECKPOINT;
    op->source_count=1u;op->sources=&source.image;
    op->target=(GmlRenderTargetRef){next,1u,(uint32_t)r->fbw,(uint32_t)r->fbh,
      GML_RENDER_PIXEL_ARGB8888,{0,0,(uint32_t)r->fbw,(uint32_t)r->fbh}};
    op->target.image.content_version+=i+1u;
    GmlRenderPrimitiveRect *p=&op->payload.primitive;
    p->region=p->authored_region=(GmlRenderRect){span->x,span->y,span->count,1u};
    p->kind=GML_RENDER_PRIMITIVE_TEXTURED_VERTEX_ROW;p->colour=0xFFFFFFFFu;
    for(unsigned c=0;c<4u;c++) p->corner_colour[c]=p->colour;
    p->alpha=1.0;p->alpha_blend=r->alphablend?1u:0u;p->pixel_write_mask=mask;
    p->interpolate=r->interp?1u:0u;
    p->centre_x=span->texcoord_first[0];p->centre_y=span->texcoord_first[1];
    p->scale_x=span->texcoord_step[0];p->scale_y=span->texcoord_step[1];
    p->rectangle_flags=GML_RENDER_RECTANGLE_OUTLINE|GML_RENDER_RECTANGLE_PIXEL_SPAN|
      GML_RENDER_RECTANGLE_VERTEX_SPAN|(r->classic?GML_RENDER_RECTANGLE_CLASSIC:0u)|
      (preserve?GML_RENDER_RECTANGLE_TARGET_ALPHA:0u)|
      (blend<<GML_RENDER_RECTANGLE_BLEND_SHIFT);
    memcpy(p->vertex_first,span->first,sizeof p->vertex_first);
    memcpy(p->vertex_step,span->step,sizeof p->vertex_step);
    if(!gml_render_textured_vertex_row_valid(op)) goto textured_done;
  }
  if(r->pending_underlay && !gml_render_compose_resident_underlay(r)) goto textured_done;
  gml_render_flush_pending_fill(r);
  if(r->pending_fill) goto textured_done;
  /* A seed clear may advance the target while rows are prepared. */
  next=gml_render_application_scene_next_image_ref(r,target);
  if(next.content_version>UINT64_MAX-count) goto textured_done;
  for(uint32_t i=0;i<count;i++) operations[i].target.image.content_version=next.content_version+i+1u;
  result=gml_render_application_scene_enqueue_textured_primitives(r,operations,count,&source);
  if(result){r->fb_all_transparent=0;r->fb_opaque_known=r->fb_all_opaque=0;}
textured_done:
  free(operations);return result;
}

/* Filled line boxes use this same CPU rectangle policy, including target alpha. */
static void resident_rectangle_payload(GmlRender *r,GmlRenderPrimitiveRect *p,
  GmlRenderRect authored,GmlRenderRect visible,const uint32_t colours[4],
  int outline,int gradient,unsigned blend,double alpha){
  int destination=r->alphablend && gml_render_blend_uses_destination_colour(r->blendmode);
  p->region=visible;p->authored_region=authored;p->kind=GML_RENDER_PRIMITIVE_RECTANGLE;
  p->alpha=alpha;p->alpha_blend=r->alphablend?1u:0u;
  int pixel_alpha=outline || (!gradient && r->blendmode!=0) || destination;
  int target_alpha=(r->alphablend && (blend==4u || (blend==1u && !outline)))?
    r->target_sp>0:pixel_alpha?gml_render_target_preserves_alpha(r):r->target_sp>0;
  p->rectangle_flags=(outline?GML_RENDER_RECTANGLE_OUTLINE:0u)|
    (r->classic?GML_RENDER_RECTANGLE_CLASSIC:0u)|
    (target_alpha?GML_RENDER_RECTANGLE_TARGET_ALPHA:0u)|
    (gradient?GML_RENDER_RECTANGLE_GRADIENT:0u)|
    (blend<<GML_RENDER_RECTANGLE_BLEND_SHIFT);
  for(unsigned c=0;c<4u;c++)
    p->corner_colour[c]=gml_render_cpu_backend_color_to_xrgb(colours[c]);
  p->colour=p->corner_colour[0];
}

static int resident_rectangle_function(GmlRender *r,int x1,int y1,int x2,int y2,
                                       uint32_t c1,uint32_t c2,uint32_t c3,uint32_t c4,
                                       int outline){
  if(!resident_rectangle_state_eligible(r)) return 0;
  double alpha=fmax(0.0,fmin(1.0,r->alpha));
  int destination=r->alphablend && gml_render_blend_uses_destination_colour(r->blendmode);
  if(alpha<=0.0 && !destination) return 1;
  if(x1>x2){int swap=x1;x1=x2;x2=swap;}
  if(y1>y2){int swap=y1;y1=y2;y2=swap;}
  if(x2<0 || y2<0 || x1>=r->fbw || y1>=r->fbh) return 1;
  if(x1 < -32768 || y1 < -32768 || x2>32767 || y2>32767) return 0;
  int gradient=!outline && (c1!=c2 || c1!=c3 || c1!=c4);
  /* Filled gradients use the scalar row writer: ordinary presets are ignored,
   * while destination-colour pairs keep their distinct CPU executor. */
  unsigned blend=gradient && !destination?0u:(unsigned)r->blendmode;
  /* The primitive pixel writer's remaining internal codes share inverse colour. */
  if(blend==3u || blend==6u) blend=2u;
  /* The existing filled scalar writer returns at an alpha below its first
   * eight-bit factor. Classic uniform floats and outlines do not use that gate. */
  if(r->alphablend && !blend && alpha<1.0/256.0 && !outline && (!r->classic || gradient)) return 1;
  GmlRenderRect authored={x1,y1,(uint32_t)(x2-x1+1),(uint32_t)(y2-y1+1)};
  int left=x1<0?0:x1,top=y1<0?0:y1;
  int right=x2>=r->fbw?r->fbw-1:x2,bottom=y2>=r->fbh?r->fbh-1:y2;
  if(outline && x1<left && x2>right && y1<top && y2>bottom) return 1;
  /* The CPU uniform full-target leaf calls set_pending_fill, which also
   * replaces its vertical samples. Partial rectangles and gradients do not. */
  if(r->classic_phase_y && !r->target_sp && r->fb==r->base_fb &&
     !outline && !gradient && !r->blendmode &&
     (alpha>=1.0 || !r->alphablend) &&
     left==0 && top==0 && right==r->fbw-1 && bottom==r->fbh-1){
    gml_render_set_pending_fill(r,gml_render_cpu_backend_color_to_xrgb(c1));
    return 1;
  }
  if(r->pending_underlay && !gml_render_compose_resident_underlay(r)) return 0;
  gml_render_flush_pending_fill(r);
  GmlRenderResource *target=gml_render_scene_target_resource_ensure(r);
  if(r->pending_fill || !target) return 0;
  GmlRenderImageRef next=gml_render_application_scene_next_image_ref(r,target);
  if(next.content_version==UINT64_MAX) return 0;
  GmlRenderDrawOp operation={0};
  operation.opcode=GML_RENDER_OP_COLOURED_PRIMITIVE;
  operation.scene_family=GML_RENDER_SCENE_PRIMITIVE;
  operation.reconstruction_mode=GML_RENDER_DEVICE_CHECKPOINT;
  operation.target=(GmlRenderTargetRef){.image=next,.storage_version=1u,
    .width=(uint32_t)r->fbw,.height=(uint32_t)r->fbh,
    .pixel_format=GML_RENDER_PIXEL_ARGB8888,
    .region={0,0,(uint32_t)r->fbw,(uint32_t)r->fbh}};
  operation.target.image.content_version++;
  GmlRenderPrimitiveRect *p=&operation.payload.primitive;
  uint32_t colours[4]={c1,c2,c3,c4};
  resident_rectangle_payload(r,p,authored,
    (GmlRenderRect){left,top,(uint32_t)(right-left+1),(uint32_t)(bottom-top+1)},
    colours,outline,gradient,blend,alpha);
  if(!gml_render_application_scene_enqueue_primitive(r,&operation)) return 0;
  r->fb_all_transparent=0;
  int opaque_write=!(p->rectangle_flags&GML_RENDER_RECTANGLE_TARGET_ALPHA) || (!blend &&
    (alpha>=1.0 || (!r->alphablend && !outline && !gradient)));
  int full_cover=!outline && left==0 && top==0 && right==r->fbw-1 && bottom==r->fbh-1;
  int opaque=opaque_write && (full_cover || (r->fb_opaque_known && r->fb_all_opaque));
  r->fb_opaque_known=r->fb_all_opaque=opaque;
  return 1;
}

typedef struct {
  GmlRender *render;
  GmlFlatRegion *regions;
  uint32_t count;
  uint32_t capacity;
  uint32_t uniform_colour;
  int overflow;
} GmlFlatSpans;

static void collect_flat_span(void *context,int x0,int x1,int y){
  GmlFlatSpans *spans=(GmlFlatSpans*)context;
  if(spans->count>=spans->capacity){spans->overflow=1;return;}
  spans->regions[spans->count++]=(GmlFlatRegion){
    {x0,y,(uint32_t)(x1-x0+1),1u},spans->uniform_colour};
}

static void collect_line_box(void *context,int x1,int y1,int x2,int y2,
                             uint32_t colour){
  GmlFlatSpans *spans=(GmlFlatSpans*)context;
  GmlRender *render=spans->render;
  if(x2<0 || y2<0 || x1>=render->fbw || y1>=render->fbh) return;
  if(x1<0) x1=0;
  if(y1<0) y1=0;
  if(x2>=render->fbw) x2=render->fbw-1;
  if(y2>=render->fbh) y2=render->fbh-1;
  if(spans->count>=spans->capacity){spans->overflow=1;return;}
  spans->regions[spans->count++]=(GmlFlatRegion){
    {x1,y1,(uint32_t)(x2-x1+1),(uint32_t)(y2-y1+1)},colour};
}

static int resident_flat_spans(GmlRender *render,GmlFlatSpans *spans){
  GmlRenderResource *resource;
  GmlRenderImageRef image;
  GmlRenderDrawOp *operations;
  GmlRenderDrawBatch batch;
  GmlRenderResourceExecution result;
  if(spans->overflow) return 0;
  if(!spans->count) return 1;
  /* A shape's bounding box is not an opaque replacement for its untouched pixels. */
  gml_render_flush_pending_fill(render);
  if(render->pending_fill || render->pending_underlay ||
     !render->resident_application_pass) return 0;
  resource=gml_render_scene_target_resource_ensure(render);
  if(!resource) return 0;
  image=gml_render_application_scene_next_image_ref(render,resource);
  if(image.content_version>UINT64_MAX-spans->count) return 0;
  operations=(GmlRenderDrawOp*)calloc(spans->count,sizeof *operations);
  if(!operations) return 0;
  for(uint32_t i=0;i<spans->count;i++){
    GmlRenderDrawOp *operation=&operations[i];
    operation->opcode=GML_RENDER_OP_COLOURED_PRIMITIVE;
    operation->scene_family=GML_RENDER_SCENE_PRIMITIVE;
    operation->reconstruction_mode=GML_RENDER_REPLAY_EXACT;
    operation->target.image=image;
    operation->target.image.content_version+=i+1u;
    operation->target.storage_version=1u;
    operation->target.width=(uint32_t)render->fbw;
    operation->target.height=(uint32_t)render->fbh;
    operation->target.pixel_format=GML_RENDER_PIXEL_ARGB8888;
    operation->target.region=(GmlRenderRect){0,0,(uint32_t)render->fbw,(uint32_t)render->fbh};
    operation->payload.primitive.region=spans->regions[i].region;
    operation->payload.primitive.colour=gml_render_cpu_backend_color_to_xrgb(
      spans->regions[i].colour);
  }
  batch=(GmlRenderDrawBatch){operations,spans->count,NULL,0u};
  result=gml_render_application_scene_submit(render,&batch,NULL,0u);
  free(operations);
  if(result==GML_RENDER_RESOURCE_EXEC_CPU) render->resident_application_pass=0;
  return result!=GML_RENDER_RESOURCE_EXEC_FAILED;
}

static int resident_filled_circle(GmlRender *render,double cx,double cy,
                                  double rx,double ry,uint32_t color){
  GmlFlatSpans spans={0};
  int covered,result;
  if(!resident_flat_eligible(render) || !isfinite(cx) || !isfinite(cy) ||
     !isfinite(rx) || !isfinite(ry) || rx<0.0 || ry<0.0) return 0;
  spans.render=render;
  spans.capacity=(uint32_t)(render->fbh<8192?render->fbh:8192);
  spans.uniform_colour=color;
  spans.regions=(GmlFlatRegion*)malloc(spans.capacity*sizeof *spans.regions);
  if(!spans.regions) return 0;
  covered=gml_render_circle_filled_spans(render,cx,cy,rx,ry,collect_flat_span,&spans);
  result=covered && resident_flat_spans(render,&spans);
  free(spans.regions);
  return result;
}

static int resident_filled_triangle(GmlRender *render,
                                    double x1,double y1,double x2,double y2,
                                    double x3,double y3,uint32_t color,
                                    double alpha,int outline){
  GmlFlatSpans spans={0};
  int covered,result;
  if(!resident_flat_eligible(render)) return 0;
  spans.render=render;
  spans.capacity=(uint32_t)(render->fbh<8192?render->fbh:8192);
  spans.uniform_colour=color;
  spans.regions=(GmlFlatRegion*)malloc(spans.capacity*sizeof *spans.regions);
  if(!spans.regions) return 0;
  covered=gml_render_triangle_opaque_spans(render,x1,y1,x2,y2,x3,y3,
                                           alpha,outline,collect_flat_span,&spans);
  result=covered && resident_flat_spans(render,&spans);
  free(spans.regions);
  return result;
}

static int resident_outline_triangle(GmlRender *render,
                                     double x1,double y1,double x2,double y2,
                                     double x3,double y3,uint32_t color,double alpha){
  GmlFlatSpans spans={0};
  int covered,result;
  if(!resident_flat_eligible(render) || !isfinite(alpha) || alpha<1.0 ||
     !isfinite(x1) || !isfinite(y1) || !isfinite(x2) || !isfinite(y2) ||
     !isfinite(x3) || !isfinite(y3)) return 0;
  double minx=fmin(x1,fmin(x2,x3)),maxx=fmax(x1,fmax(x2,x3));
  double miny=fmin(y1,fmin(y2,y3)),maxy=fmax(y1,fmax(y2,y3));
  if(minx< -8192.0 || miny< -8192.0 ||
     maxx>=(double)render->fbw+8192.0 ||
     maxy>=(double)render->fbh+8192.0 ||
     (maxx-minx+1.0)*(maxy-miny+1.0)>262144.0) return 0;
  spans.render=render;
  spans.capacity=8192u;
  spans.regions=(GmlFlatRegion*)malloc(spans.capacity*sizeof *spans.regions);
  if(!spans.regions) return 0;
  covered=gml_render_triangle_barycentric_pixels(render,x1,y1,x2,y2,x3,y3,
                                                  alpha,1,color,collect_line_box,&spans);
  result=covered && resident_flat_spans(render,&spans);
  free(spans.regions);
  return result;
}

static int resident_line_bounds(GmlRender *render,double x1,double y1,
                                double x2,double y2,int width){
  double right=(double)render->fbw+8192.0,bottom=(double)render->fbh+8192.0;
  return width<=4096 && x1>=-8192.0 && x1<right && x2>=-8192.0 && x2<right &&
    y1>=-8192.0 && y1<bottom && y2>=-8192.0 && y2<bottom;
}

typedef struct {
  GmlFlatSpans spans;
  uint32_t pixels;
  int top,bottom;
} GmlRectangleOutlineSpans;

static void collect_rectangle_outline_pixel(void *context,int x1,int y1,int x2,int y2,
                                             uint32_t colour){
  GmlRectangleOutlineSpans *outline=(GmlRectangleOutlineSpans*)context;
  GmlFlatSpans *spans=&outline->spans;
  /* Keep the original pixel-work limit even when storage needs only four regions.
   * The unchanged collector visits each geometric edge pixel once in raster order. */
  if(spans->overflow) return;
  if(x1!=x2 || y1!=y2 || outline->pixels>=8192u){spans->overflow=1;return;}
  outline->pixels++;
  for(uint32_t i=0;i<spans->count;i++){
    GmlFlatRegion *run=&spans->regions[i];
    if(run->colour!=colour) continue;
    /* Adjacent left/right pixels in a two-column middle row are separate edges,
     * not a new horizontal run on every row. */
    if((y1==outline->top || y1==outline->bottom) &&
       run->region.height==1u && run->region.y==y1 &&
       (int64_t)run->region.x+run->region.width==x1){
      run->region.width++;
      return;
    }
    if(run->region.width==1u && run->region.x==x1 &&
       (int64_t)run->region.y+run->region.height==y1){
      run->region.height++;
      return;
    }
  }
  collect_line_box(spans,x1,y1,x2,y2,colour);
}

static int resident_outline_rectangle(GmlRender *render,int x1,int y1,int x2,int y2,
                                      uint32_t color){
  GmlFlatRegion regions[4];
  GmlRectangleOutlineSpans outline={0};
  int covered,result;
  /* The shared CPU collector preserves the geometric edges after clipping.
   * Only this rectangle outline may reuse the early-Classic raster gate. */
  if(!resident_flat_eligible_rule(render,render && render->classic,0,0)) return 0;
  outline.spans.render=render;
  outline.spans.capacity=4u;
  outline.spans.regions=regions;
  outline.top=y1<y2?y1:y2;
  outline.bottom=y1>y2?y1:y2;
  covered=gml_render_rectangle_outline_pixels(render,x1,y1,x2,y2,color,
                                               collect_rectangle_outline_pixel,&outline);
  result=covered && resident_flat_spans(render,&outline.spans);
  return result;
}

static void collect_outline_segment(void *context,int x1,int y1,int x2,int y2,
                                    uint32_t color){
  GmlFlatSpans *spans=(GmlFlatSpans*)context;
  (void)gml_render_line_integer_boxes(spans->render,x1,y1,x2,y2,
                                       color,color,1,collect_line_box,spans);
}

static int resident_outline_circle(GmlRender *render,double cx,double cy,
                                   double rx,double ry,uint32_t color,int coloured){
  GmlFlatSpans spans={0};
  int covered,result;
  if(!resident_flat_eligible(render) || !isfinite(cx) || !isfinite(cy) ||
     !isfinite(rx) || !isfinite(ry) || !(rx>0.0) || !(ry>0.0) ||
     cx-rx < -8192.0 || cy-ry < -8192.0 ||
     cx+rx >= (double)render->fbw+8192.0 ||
     cy+ry >= (double)render->fbh+8192.0 ||
     cx-rx <= (double)INT_MIN+2.0 || cy-ry <= (double)INT_MIN+2.0 ||
     cx+rx >= (double)INT_MAX-2.0 || cy+ry >= (double)INT_MAX-2.0)
    return 0;
  spans.render=render;
  spans.capacity=8192u;
  spans.regions=(GmlFlatRegion*)malloc(spans.capacity*sizeof *spans.regions);
  if(!spans.regions) return 0;
  covered=gml_render_circle_outline_segments(render,cx,cy,rx,ry,color,coloured,
                                              collect_outline_segment,&spans);
  result=covered && resident_flat_spans(render,&spans);
  free(spans.regions);
  return result;
}

static int resident_line_integer(GmlRender *render,int x1,int y1,int x2,int y2,
                                 uint32_t colour1,uint32_t colour2,int width){
  GmlFlatSpans spans={0};
  int covered,result;
  if(!resident_flat_eligible(render) ||
     !resident_line_bounds(render,x1,y1,x2,y2,width)) return 0;
  spans.render=render;
  int64_t dx=(int64_t)x2-x1,dy=(int64_t)y2-y1;
  if(dx<0) dx=-dx;
  if(dy<0) dy=-dy;
  int64_t steps=(dx>dy?dx:dy)+1;
  spans.capacity=(uint32_t)(steps<8192?steps:8192);
  spans.regions=(GmlFlatRegion*)malloc(spans.capacity*sizeof *spans.regions);
  if(!spans.regions) return 0;
  covered=gml_render_line_integer_boxes(render,x1,y1,x2,y2,
                                         colour1,colour2,width,collect_line_box,&spans);
  result=covered && resident_flat_spans(render,&spans);
  free(spans.regions);
  return result;
}

static int resident_line_subpixel(GmlRender *render,double x1,double y1,
                                  double x2,double y2,uint32_t colour1,
                                  uint32_t colour2,int width){
  GmlFlatSpans spans={0};
  int covered,result;
  if(!resident_flat_eligible(render) || !isfinite(x1) || !isfinite(y1) ||
     !isfinite(x2) || !isfinite(y2) ||
     !resident_line_bounds(render,x1,y1,x2,y2,width)) return 0;
  spans.render=render;
  double candidate_count=(ceil(fmax(fabs(x2-x1),fabs(y2-y1)))+3.0)*5.0;
  spans.capacity=(uint32_t)(candidate_count<8192.0?candidate_count:8192.0);
  spans.regions=(GmlFlatRegion*)malloc(spans.capacity*sizeof *spans.regions);
  if(!spans.regions) return 0;
  covered=gml_render_line_subpixel_boxes(render,x1,y1,x2,y2,
                                          colour1,colour2,width,collect_line_box,&spans);
  result=covered && resident_flat_spans(render,&spans);
  free(spans.regions);
  return result;
}

static int resident_axis_line(GmlRender *render,int x1,int y1,int x2,int y2,
                              uint32_t color,int width){
  int64_t left,right,top,bottom;
  if(x1!=x2 && y1!=y2) return 0;
  if(width<1) width=1;
  left=(int64_t)(x1<x2?x1:x2)-width/2;
  right=(int64_t)(x1>x2?x1:x2)-width/2+width-1;
  top=(int64_t)(y1<y2?y1:y2)-width/2;
  bottom=(int64_t)(y1>y2?y1:y2)-width/2+width-1;
  if(left<INT_MIN || right>INT_MAX || top<INT_MIN || bottom>INT_MAX) return 0;
  return gml_render_try_opaque_primitive_rect(render,(int)left,(int)top,
                                               (int)right,(int)bottom,color);
}

/* R03-PRIM-LINE-FUNCTION: retain the CPU walk and its ordered, overlapping boxes.
 * Only adjacent disjoint boxes with identical colour may become one rectangle. */
static int resident_line_function(GmlRender *r,double x1,double y1,double x2,double y2,
                                  uint32_t colour1,uint32_t colour2,int width,int subpixel){
  if(!resident_rectangle_state_eligible(r) ||
     !isfinite(x1) || !isfinite(y1) || !isfinite(x2) || !isfinite(y2) ||
     !resident_line_bounds(r,x1,y1,x2,y2,width) ||
     fabs(x1)>16384.0 || fabs(y1)>16384.0 || fabs(x2)>16384.0 || fabs(y2)>16384.0 ||
     r->app_interp_phase[0] || r->app_interp_phase[1] || r->app_interp_phase[2]) return 0;
  double alpha=fmax(0.0,fmin(1.0,r->alpha));
  int destination=r->alphablend && gml_render_blend_uses_destination_colour(r->blendmode);
  if(alpha<=0.0 && !destination) return 1;
  unsigned blend=(unsigned)r->blendmode;
  if(blend==3u || blend==6u) blend=2u;
  if(r->alphablend && !blend && !r->classic && alpha<1.0/256.0) return 1;
  double candidates=ceil(fmax(fabs(x2-x1),fabs(y2-y1)))+(subpixel?3.0:1.0);
  if(subpixel) candidates*=5.0;
  GmlFlatSpans spans={.render=r,.capacity=(uint32_t)fmin(candidates,8192.0)};
  spans.regions=(GmlFlatRegion*)malloc(spans.capacity*sizeof *spans.regions);
  if(!spans.regions) return 0;
  int covered=subpixel?
    gml_render_line_subpixel_boxes(r,x1,y1,x2,y2,colour1,colour2,width,collect_line_box,&spans):
    gml_render_line_integer_boxes(r,(int)x1,(int)y1,(int)x2,(int)y2,
      colour1,colour2,width,collect_line_box,&spans);
  int result=0;
  GmlRenderDrawOp *operations=NULL;
  if(!covered || spans.overflow) goto line_done;
  if(!spans.count){result=1;goto line_done;}
  /* Repeated low-opacity source-over can exceed D2 because the CPU quantises
   * after every box. Bound repeated coverage before accepting any part of it.
   * An integer walk emits one box per major step; a diamond walk may emit two. */
  unsigned thickness=width>1?(unsigned)width:1u;
  unsigned overlap_bound=thickness*(subpixel && thickness>1u?2u:1u);
  if(overlap_bound>spans.count) overlap_bound=spans.count;
  if(r->alphablend && !blend && alpha<0.5 && overlap_bound>2u) goto line_done;
  uint32_t count=0;
  for(uint32_t i=0;i<spans.count;i++){
    GmlFlatRegion current=spans.regions[i];
    if(count && current.colour==spans.regions[count-1u].colour){
      GmlRenderRect *previous=&spans.regions[count-1u].region;
      if(previous->y==current.region.y && previous->height==current.region.height &&
         ((int64_t)previous->x+previous->width==current.region.x ||
          (int64_t)current.region.x+current.region.width==previous->x)){
        if(current.region.x<previous->x) previous->x=current.region.x;
        previous->width+=current.region.width;continue;
      }
      if(previous->x==current.region.x && previous->width==current.region.width &&
         ((int64_t)previous->y+previous->height==current.region.y ||
          (int64_t)current.region.y+current.region.height==previous->y)){
        if(current.region.y<previous->y) previous->y=current.region.y;
        previous->height+=current.region.height;continue;
      }
    }
    spans.regions[count++]=current;
  }
  operations=(GmlRenderDrawOp*)calloc(count,sizeof *operations);
  if(!operations) goto line_done;
  if(r->pending_underlay && !gml_render_compose_resident_underlay(r)) goto line_done;
  gml_render_flush_pending_fill(r);
  GmlRenderResource *target=gml_render_scene_target_resource_ensure(r);
  if(r->pending_fill || !target) goto line_done;
  GmlRenderImageRef next=gml_render_application_scene_next_image_ref(r,target);
  if(next.content_version>UINT64_MAX-count) goto line_done;
  for(uint32_t i=0;i<count;i++){
    GmlRenderDrawOp *op=&operations[i];
    op->opcode=GML_RENDER_OP_COLOURED_PRIMITIVE;
    op->scene_family=GML_RENDER_SCENE_PRIMITIVE;
    op->reconstruction_mode=GML_RENDER_DEVICE_CHECKPOINT;
    op->target=(GmlRenderTargetRef){next,1u,(uint32_t)r->fbw,(uint32_t)r->fbh,
      GML_RENDER_PIXEL_ARGB8888,{0,0,(uint32_t)r->fbw,(uint32_t)r->fbh}};
    op->target.image.content_version+=i+1u;
    uint32_t colour=spans.regions[i].colour,colours[4]={colour,colour,colour,colour};
    resident_rectangle_payload(r,&op->payload.primitive,spans.regions[i].region,
      spans.regions[i].region,colours,0,0,blend,alpha);
  }
  result=gml_render_application_scene_enqueue_primitives(r,operations,count);
  if(result){
    int opaque_write=!(operations[0].payload.primitive.rectangle_flags&GML_RENDER_RECTANGLE_TARGET_ALPHA) ||
      (!blend && (alpha>=1.0 || !r->alphablend));
    int opaque=opaque_write && r->fb_opaque_known && r->fb_all_opaque;
    r->fb_all_transparent=0;r->fb_opaque_known=r->fb_all_opaque=opaque;
  }
line_done:
  free(operations);free(spans.regions);
  return result;
}

static int compare_circle_pixel(const void *left,const void *right){
  uint32_t a=*(const uint32_t*)left,b=*(const uint32_t*)right;
  return (a>b)-(a<b);
}

/* R03-PRIM-CIRCLE-FUNCTION: share the CPU polygon and segment coverage, not its
 * framebuffer. Thin rectangles select the pixel writer's alpha policy. Keep
 * repeated outline pixels in order, including coincident polygon vertices. */
static int resident_circle_function(GmlRender *r,double cx,double cy,double rx,double ry,
                                    uint32_t inner,uint32_t outer,int outline){
  if(!resident_primitive_state_eligible(r) ||
     !isfinite(cx) || !isfinite(cy) || !isfinite(rx) || !isfinite(ry) ||
     cx-fabs(rx)<-32766.0 || cy-fabs(ry)<-32766.0 ||
     cx+fabs(rx)>32766.0 || cy+fabs(ry)>32766.0 ||
     r->app_interp_phase[0] || r->app_interp_phase[1] || r->app_interp_phase[2]) return 0;
  if(rx<0.0 || ry<0.0) return 1;
  double alpha=fmax(0.0,fmin(1.0,r->alpha));
  int destination=r->alphablend && gml_render_blend_uses_destination_colour(r->blendmode);
  if(alpha<=0.0 && !destination) return 1;
  int point=rx==0.0 || ry==0.0;
  /* Outlines call the existing one-pixel filled-rectangle line walker. Its
   * ordinary/additive alpha differs from the filled circle's pixel writer. */
  int flat_outline=outline && !point &&
    (!r->blendmode || (r->alphablend && r->blendmode==1));
  if(flat_outline && r->alphablend && !r->blendmode && !r->classic && alpha<1.0/256.0) return 1;
  /* The nonuniform filled fan retains its separate CPU rounding debt. */
  if(!outline && !point && inner!=outer) return 0;
  unsigned blend=(unsigned)r->blendmode;
  if(blend==3u || blend==6u) blend=2u;
  GmlFlatSpans spans={.render=r,.capacity=8192u,
    .uniform_colour=outline && !point && inner!=outer?outer:inner};
  spans.regions=(GmlFlatRegion*)malloc(spans.capacity*sizeof *spans.regions);
  if(!spans.regions) return 0;
  int result=0,covered;
  GmlRenderDrawOp *operations=NULL;
  if(outline && !point)
    covered=gml_render_circle_outline_segments(r,cx,cy,rx,ry,spans.uniform_colour,
      inner!=outer,collect_outline_segment,&spans);
  else covered=gml_render_circle_filled_spans(r,cx,cy,rx,ry,collect_flat_span,&spans);
  if(!covered || spans.overflow) goto circle_done;
  if(!spans.count){result=1;goto circle_done;}
  if(outline && !point && r->alphablend && !blend && alpha<0.5){
    uint32_t *pixels=(uint32_t*)malloc(spans.count*sizeof *pixels);
    if(!pixels) goto circle_done;
    for(uint32_t i=0;i<spans.count;i++)
      pixels[i]=(uint32_t)spans.regions[i].region.y*(uint32_t)r->fbw+
        (uint32_t)spans.regions[i].region.x;
    qsort(pixels,spans.count,sizeof *pixels,compare_circle_pixel);
    unsigned repeats=1u,maximum=1u;
    for(uint32_t i=1;i<spans.count;i++){
      repeats=pixels[i]==pixels[i-1u]?repeats+1u:1u;
      if(repeats>maximum) maximum=repeats;
    }
    free(pixels);
    if(maximum>2u) goto circle_done;
  }
  uint32_t count=0;
  for(uint32_t i=0;i<spans.count;i++){
    GmlFlatRegion current=spans.regions[i];
    if(count && current.colour==spans.regions[count-1u].colour){
      GmlRenderRect *previous=&spans.regions[count-1u].region;
      if(previous->height==1u && current.region.height==1u && previous->y==current.region.y &&
         ((int64_t)previous->x+previous->width==current.region.x ||
          (int64_t)current.region.x+current.region.width==previous->x)){
        if(current.region.x<previous->x) previous->x=current.region.x;
        previous->width+=current.region.width;continue;
      }
      if(previous->width==1u && current.region.width==1u && previous->x==current.region.x &&
         ((int64_t)previous->y+previous->height==current.region.y ||
          (int64_t)current.region.y+current.region.height==previous->y)){
        if(current.region.y<previous->y) previous->y=current.region.y;
        previous->height+=current.region.height;continue;
      }
    }
    spans.regions[count++]=current;
  }
  operations=(GmlRenderDrawOp*)calloc(count,sizeof *operations);
  if(!operations) goto circle_done;
  if(r->pending_underlay && !gml_render_compose_resident_underlay(r)) goto circle_done;
  gml_render_flush_pending_fill(r);
  GmlRenderResource *target=gml_render_scene_target_resource_ensure(r);
  if(r->pending_fill || !target) goto circle_done;
  GmlRenderImageRef next=gml_render_application_scene_next_image_ref(r,target);
  if(next.content_version>UINT64_MAX-count) goto circle_done;
  /* Ordinary pixel presets ignore the raw mask. Destination-dependent presets
   * use the same four-channel mask as their existing CPU pixel executor. */
  unsigned mask=r->alphablend && (destination || blend==4u || blend==7u || blend==8u)?
    r->color_write_mask:15u;
  for(uint32_t i=0;i<count;i++){
    GmlRenderDrawOp *op=&operations[i];
    op->opcode=GML_RENDER_OP_COLOURED_PRIMITIVE;op->scene_family=GML_RENDER_SCENE_PRIMITIVE;
    op->reconstruction_mode=GML_RENDER_DEVICE_CHECKPOINT;
    op->target=(GmlRenderTargetRef){next,1u,(uint32_t)r->fbw,(uint32_t)r->fbh,
      GML_RENDER_PIXEL_ARGB8888,{0,0,(uint32_t)r->fbw,(uint32_t)r->fbh}};
    op->target.image.content_version+=i+1u;
    uint32_t colour=spans.regions[i].colour,colours[4]={colour,colour,colour,colour};
    resident_rectangle_payload(r,&op->payload.primitive,spans.regions[i].region,
      spans.regions[i].region,colours,!flat_outline,0,blend,alpha);
    if(!flat_outline){
      op->payload.primitive.rectangle_flags|=GML_RENDER_RECTANGLE_PIXEL_SPAN;
      op->payload.primitive.pixel_write_mask=mask;
    }
  }
  result=gml_render_application_scene_enqueue_primitives(r,operations,count);
  if(result){
    unsigned flags=operations[0].payload.primitive.rectangle_flags;
    int opaque_write=!(flags&GML_RENDER_RECTANGLE_TARGET_ALPHA) ||
      (r->alphablend && (blend==1u || blend==5u)) ||
      ((!blend || !r->alphablend || blend==4u) && alpha>=1.0) || !(mask&8u);
    int opaque=opaque_write && r->fb_opaque_known && r->fb_all_opaque;
    r->fb_all_transparent=0;r->fb_opaque_known=r->fb_all_opaque=opaque;
  }
circle_done:
  free(operations);free(spans.regions);
  return result;
}

void gml_render_primitive_point(GmlRender *render,int x,int y,uint32_t color){
  if(gml_render_try_opaque_primitive_rect(render,x,y,x,y,color)) return;
  gml_render_cpu_point(render,x,y,color);
}

static int resident_primitive_alpha_noop(GmlRender *render){
  /* CPU line boxes and circle pixels reject this alpha before preparing a draw.
   * Keep pending work and stale CPU pixels untouched; enabled destination-colour
   * blends may still write at alpha zero. Only the qualified ports use this rule. */
  return render && render->resident_application_pass && render->alpha<=0.0 &&
    !(render->alphablend && gml_render_blend_uses_destination_colour(render->blendmode));
}

void gml_render_primitive_line(GmlRender *render,int x1,int y1,int x2,int y2,
                               uint32_t color,int width){
  if(resident_primitive_alpha_noop(render)) return;
  if(resident_line_function(render,x1,y1,x2,y2,color,color,width,0)) return;
  if(resident_axis_line(render,x1,y1,x2,y2,color,width)) return;
  if(resident_line_integer(render,x1,y1,x2,y2,color,color,width)) return;
  gml_render_cpu_line(render,x1,y1,x2,y2,color,width);
}

void gml_render_primitive_line_color(GmlRender *render,int x1,int y1,int x2,int y2,
                                     uint32_t color1,uint32_t color2,int width){
  if(resident_primitive_alpha_noop(render)) return;
  if(resident_line_function(render,x1,y1,x2,y2,color1,color2,width,0)) return;
  if(color1==color2 && resident_axis_line(render,x1,y1,x2,y2,color1,width)) return;
  if(resident_line_integer(render,x1,y1,x2,y2,color1,color2,width)) return;
  gml_render_cpu_line_color(render,x1,y1,x2,y2,color1,color2,width);
}

void gml_render_primitive_line_color_subpixel(GmlRender *render,
                                              double x1,double y1,
                                              double x2,double y2,
                                              uint32_t color1,uint32_t color2,
                                              int width){
  if(resident_primitive_alpha_noop(render)) return;
  if(resident_line_function(render,x1,y1,x2,y2,color1,color2,width,1)) return;
  if(resident_line_subpixel(render,x1,y1,x2,y2,color1,color2,width)) return;
  gml_render_cpu_line_color_subpixel(render,x1,y1,x2,y2,color1,color2,width);
}

void gml_render_primitive_rectangle(GmlRender *render,
                                    int x1,int y1,int x2,int y2,
                                    uint32_t color,int outline){
  if(outline && resident_outline_rectangle(render,x1,y1,x2,y2,color)) return;
  if(!outline && gml_render_shade_target_rect(render,x1,y1,x2,y2,color,render?render->alpha:1.0))
    return;
  if(!outline && resident_opaque_rect(render,x1,y1,x2,y2,color,1)) return;
  if(!outline && resident_classic_alpha_rect(render,x1,y1,x2,y2,color)) return;
  if(!outline && resident_studio_alpha_rect(render,x1,y1,x2,y2,color)) return;
  if(resident_rectangle_function(render,x1,y1,x2,y2,color,color,color,color,outline)) return;
  gml_render_cpu_rectangle(render,x1,y1,x2,y2,color,outline);
}

void gml_render_primitive_rectangle_color(GmlRender *render,
                                          int x1,int y1,int x2,int y2,
                                          uint32_t color1,uint32_t color2,
                                          uint32_t color3,uint32_t color4,
                                          int outline){
  if(outline && resident_outline_rectangle(render,x1,y1,x2,y2,color1)) return;
  if(!outline && color1==color2 && color2==color3 && color3==color4 &&
     gml_render_shade_target_rect(render,x1,y1,x2,y2,color1,render?render->alpha:1.0)) return;
  if(!outline && color1==color2 && color2==color3 && color3==color4 &&
     resident_opaque_rect(render,x1,y1,x2,y2,color1,1)) return;
  if(!outline && color1==color2 && color2==color3 && color3==color4 &&
     resident_classic_alpha_rect(render,x1,y1,x2,y2,color1)) return;
  if(!outline && color1==color2 && color2==color3 && color3==color4 &&
     resident_studio_alpha_rect(render,x1,y1,x2,y2,color1)) return;
  if(resident_rectangle_function(render,x1,y1,x2,y2,color1,color2,color3,color4,outline)) return;
  gml_render_cpu_rectangle_color(render,x1,y1,x2,y2,color1,color2,color3,color4,outline);
}

void gml_render_primitive_circle(GmlRender *render,
                                 int center_x,int center_y,int radius_x,int radius_y,
                                 uint32_t color,int outline){
  if(resident_primitive_alpha_noop(render)) return;
  if(outline && resident_outline_circle(render,(double)center_x,(double)center_y,
                                        (double)radius_x,(double)radius_y,color,0)) return;
  if(!outline && resident_filled_circle(render,(double)center_x,(double)center_y,
                                         (double)radius_x,(double)radius_y,color)) return;
  if(resident_circle_function(render,center_x,center_y,radius_x,radius_y,color,color,outline)) return;
  gml_render_cpu_circle(render,(double)center_x,(double)center_y,
                        (double)radius_x,(double)radius_y,color,outline);
}

void gml_render_primitive_circle_subpixel(GmlRender *render,
                                          double center_x,double center_y,
                                          double radius_x,double radius_y,
                                          uint32_t color,int outline){
  if(resident_primitive_alpha_noop(render)) return;
  if(outline && resident_outline_circle(render,center_x,center_y,
                                        radius_x,radius_y,color,0)) return;
  if(!outline && resident_filled_circle(render,center_x,center_y,radius_x,radius_y,color)) return;
  if(resident_circle_function(render,center_x,center_y,radius_x,radius_y,color,color,outline)) return;
  gml_render_cpu_circle(render,center_x,center_y,radius_x,radius_y,color,outline);
}

void gml_render_primitive_circle_color(GmlRender *render,
                                       int center_x,int center_y,int radius_x,int radius_y,
                                       uint32_t inner,uint32_t outer,int outline){
  if(resident_primitive_alpha_noop(render)) return;
  if(outline && resident_outline_circle(render,(double)center_x,(double)center_y,
                                        (double)radius_x,(double)radius_y,
                                        inner==outer?inner:outer,inner!=outer)) return;
  if(!outline && inner==outer &&
     resident_filled_circle(render,(double)center_x,(double)center_y,
                            (double)radius_x,(double)radius_y,inner)) return;
  if(resident_circle_function(render,center_x,center_y,radius_x,radius_y,inner,outer,outline)) return;
  gml_render_cpu_circle_color(render,(double)center_x,(double)center_y,
                              (double)radius_x,(double)radius_y,inner,outer,outline);
}

void gml_render_primitive_circle_color_subpixel(GmlRender *render,
                                                double center_x,double center_y,
                                                double radius_x,double radius_y,
                                                uint32_t inner,uint32_t outer,int outline){
  if(resident_primitive_alpha_noop(render)) return;
  if(outline && resident_outline_circle(render,center_x,center_y,radius_x,radius_y,
                                        inner==outer?inner:outer,inner!=outer)) return;
  if(!outline && inner==outer &&
     resident_filled_circle(render,center_x,center_y,radius_x,radius_y,inner)) return;
  if(resident_circle_function(render,center_x,center_y,radius_x,radius_y,inner,outer,outline)) return;
  gml_render_cpu_circle_color(render,center_x,center_y,radius_x,radius_y,
                              inner,outer,outline);
}

void gml_render_primitive_triangle_alpha(GmlRender *render,
                                         double x1,double y1,double x2,double y2,
                                         double x3,double y3,uint32_t color,
                                         double alpha,int outline){
  if(outline && resident_outline_triangle(render,x1,y1,x2,y2,x3,y3,color,alpha)) return;
  if(resident_filled_triangle(render,x1,y1,x2,y2,x3,y3,color,alpha,outline)) return;
  gml_render_cpu_triangle_alpha(render,x1,y1,x2,y2,x3,y3,color,alpha,outline);
}

void gml_render_circle_geometry(const GmlRender *render,
                                double x,double y,double radius,
                                double *center_x,double *center_y,
                                double *radius_x,double *radius_y){
  double mapped_x=x+GML_RENDER_CIRCLE_CENTER_BIAS;
  double mapped_y=y+GML_RENDER_CIRCLE_CENTER_BIAS;
  double mapped_rx=radius,mapped_ry=radius;
  GmlRenderTargetMetrics metrics={0};
  gml_render_draw_map_point(render,&mapped_x,&mapped_y);
  gml_render_draw_map_scale(render,&mapped_rx,&mapped_ry);
  (void)gml_render_target_metrics(render,&metrics);
  if(center_x) *center_x=mapped_x-metrics.camera_x;
  if(center_y) *center_y=mapped_y-metrics.camera_y;
  if(radius_x) *radius_x=fabs(mapped_rx);
  if(radius_y) *radius_y=fabs(mapped_ry);
}
