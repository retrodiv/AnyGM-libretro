/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Replay of the first exactly qualified resolved scene variants. Existing CPU channel/fill
 * kernels remain the arithmetic owner; this unit borrows the checkpoint and retained source. */
#include "gml_render_cpu_replay.h"
#include "gml_render_cpu_backend.h"
#include "gml_render_internal.h"
#include "gml_render_cpu_blit_internal.h"
#include "gml_render_cpu_blit.h"
#include "gml_render_cpu_surfaces.h"
#include "gml_render_cpu_primitives.h"
#include "gml_particle.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static int same_ref(GmlRenderImageRef a,GmlRenderImageRef b){
  return a.resource_class==b.resource_class && a.identity==b.identity &&
         a.incarnation==b.incarnation && a.content_version==b.content_version;
}

static int full_rect(GmlRenderRect rect,uint32_t width,uint32_t height){
  return rect.x==0 && rect.y==0 && rect.width==width && rect.height==height;
}

static int source_rect(GmlRenderRect rect,uint32_t width,uint32_t height){
  return rect.x>=0 && rect.y>=0 && rect.width && rect.height &&
         (uint64_t)(uint32_t)rect.x+rect.width<=width &&
         (uint64_t)(uint32_t)rect.y+rect.height<=height;
}

static int same_rect(GmlRenderRect a,GmlRenderRect b){
  return a.x==b.x && a.y==b.y && a.width==b.width && a.height==b.height;
}

static int coordinate_matches(double actual,double expected){
  return isfinite(actual) && fabs(actual-expected)<=1e-12;
}

static int source_valid(const GmlRenderResourceSource *source){
  uint64_t required;
  if(!source || !source->width || !source->height ||
     source->width>INT_MAX || source->height>INT_MAX ||
     source->width>UINT32_MAX/4u || source->stride_bytes<source->width*4u ||
     (source->pixel_format!=GML_RENDER_PIXEL_XRGB8888 &&
      source->pixel_format!=GML_RENDER_PIXEL_ARGB8888 &&
      source->pixel_format!=GML_RENDER_PIXEL_RGBA8888)) return 0;
  if(!source->bytes)
    return source->resident_resource && !source->byte_capacity &&
           !source->borrowed_immutable && source->stride_bytes==source->width*4u;
  required=(uint64_t)(source->height-1u)*source->stride_bytes+(uint64_t)source->width*4u;
  return required<=source->byte_capacity;
}

static int blend_rule(const GmlRenderDrawStateSnapshot *state){
  if(state->rgb_source_factor==GML_RENDER_BLEND_SOURCE_ALPHA &&
     state->rgb_destination_factor==GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA &&
     state->alpha_source_factor==GML_RENDER_BLEND_ONE &&
     state->alpha_destination_factor==GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA &&
     state->rgb_equation==GML_RENDER_BLEND_EXACT_AUTOMATIC_SURFACE &&
     state->alpha_equation==GML_RENDER_BLEND_EXACT_AUTOMATIC_SURFACE) return 6;
  if(state->rgb_source_factor==GML_RENDER_BLEND_SOURCE_ALPHA &&
     state->rgb_destination_factor==GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA &&
     (state->alpha_source_factor==GML_RENDER_BLEND_ONE ||
      state->alpha_source_factor==GML_RENDER_BLEND_SOURCE_ALPHA) &&
     state->alpha_destination_factor==GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA &&
     state->rgb_equation==GML_RENDER_BLEND_EXACT_FILTERED_FONT &&
     state->alpha_equation==GML_RENDER_BLEND_EXACT_FILTERED_FONT) return 5;
  if(!state->rgb_source_factor && !state->rgb_destination_factor &&
     !state->alpha_source_factor && !state->alpha_destination_factor &&
     !state->rgb_equation && !state->alpha_equation) return 0;
  if(state->rgb_source_factor==GML_RENDER_BLEND_SOURCE_ALPHA &&
     state->rgb_destination_factor==GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA &&
     state->alpha_source_factor==GML_RENDER_BLEND_ONE &&
     state->alpha_destination_factor==GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA){
    if(state->rgb_equation==GML_RENDER_BLEND_EXACT8_TRUNC &&
       state->alpha_equation==GML_RENDER_BLEND_EXACT8_TRUNC) return 1;
    if(state->rgb_equation==GML_RENDER_BLEND_ADD &&
       state->alpha_equation==GML_RENDER_BLEND_ADD) return 2;
    if(state->rgb_equation==GML_RENDER_BLEND_EXACT8_ROUND_VEIL &&
       state->alpha_equation==GML_RENDER_BLEND_EXACT8_ROUND_VEIL) return 4;
  }
  if(state->rgb_source_factor==GML_RENDER_BLEND_SOURCE_ALPHA &&
     state->rgb_destination_factor==GML_RENDER_BLEND_ONE &&
     state->alpha_source_factor==GML_RENDER_BLEND_ONE &&
     state->alpha_destination_factor==GML_RENDER_BLEND_ZERO &&
     state->rgb_equation==GML_RENDER_BLEND_EXACT8_ADDITIVE &&
     state->alpha_equation==GML_RENDER_BLEND_EXACT8_ADDITIVE &&
     isfinite(state->additive_draw_alpha) &&
     state->additive_draw_alpha>0.0 && state->additive_draw_alpha<=1.0) return 3;
  return -1;
}

static int stretched_float_eligible(const GmlRenderDrawOp *operation,
                                     const GmlRenderDrawStateSnapshot *state,
                                     const GmlRenderResourceSource *source){
  const GmlRenderQuad *quad=&operation->payload.quad;
  GmlRenderRect region=quad->sampling.source_region;
  if(operation->scene_family!=GML_RENDER_SCENE_SPRITE || blend_rule(state)!=0 ||
     operation->target.pixel_format!=GML_RENDER_PIXEL_ARGB8888 ||
     source->pixel_format!=GML_RENDER_PIXEL_RGBA8888 ||
     source->stride_bytes!=source->width*4u ||
     quad->shape!=GML_RENDER_QUAD_AXIS_ALIGNED || quad->span_active ||
     quad->sampling.filter!=GML_RENDER_FILTER_BILINEAR ||
     quad->sampling.edge_rule!=GML_RENDER_EDGE_CLAMP_SUBIMAGE ||
     quad->sampling.logical_margin_mask ||
     !source_rect(region,source->width,source->height) ||
     state->samplers[0].quantisation!=quad->sampling.quantisation ||
     state->samplers[0].filter!=quad->sampling.filter ||
     state->samplers[0].edge_rule!=quad->sampling.edge_rule ||
     state->samplers[0].logical_margin_mask ||
     !same_rect(state->samplers[0].source_region,region)) return 0;
  for(unsigned axis=0;axis<2u;axis++){
    const double *span=axis?quad->span_y:quad->span_x;
    double first=axis?quad->y[0]:quad->x[0];
    double last=axis?quad->y[2]:quad->x[1];
    double physical=axis?region.height:region.width;
    for(unsigned field=0;field<4u;field++)
      if(!isfinite(span[field]) || floor(span[field])!=span[field]) return 0;
    if(!isfinite(first) || !isfinite(last) || last>(double)INT_MAX || floor(first)!=first ||
       first<=-span[1] || first>=
         (double)(axis?operation->target.height:operation->target.width) ||
       span[0]!=physical || span[0]<1.0 || span[0]>16384.0 ||
       span[1]<=span[0] || span[1]>16384.0 || last-first!=span[1] ||
       span[2]!=0.0 || span[3]!=span[0]) return 0;
    if(!gml_render_stretched_float_phase_exact((uint32_t)span[0],(uint32_t)span[1]))
      return 0;
  }
  if(quad->x[3]!=quad->x[0] || quad->x[2]!=quad->x[1] ||
     quad->y[1]!=quad->y[0] || quad->y[3]!=quad->y[2]) return 0;
  double u0=(double)region.x/source->width;
  double u1=((double)region.x+region.width)/source->width;
  double v0=(double)region.y/source->height;
  double v1=((double)region.y+region.height)/source->height;
  if(!coordinate_matches(quad->u[0],u0) || !coordinate_matches(quad->u[3],u0) ||
     !coordinate_matches(quad->u[1],u1) || !coordinate_matches(quad->u[2],u1) ||
     !coordinate_matches(quad->v[0],v0) || !coordinate_matches(quad->v[1],v0) ||
     !coordinate_matches(quad->v[2],v1) || !coordinate_matches(quad->v[3],v1)) return 0;
  for(unsigned corner=0;corner<4u;corner++)
    if(quad->corner_colour[corner]!=0xFFFFFFFFu) return 0;
  for(uint32_t y=0;y<region.height;y++) for(uint32_t x=0;x<region.width;x++){
    const uint8_t *pixel=(const uint8_t*)source->bytes+
      (size_t)((uint32_t)region.y+y)*source->stride_bytes+
      ((uint32_t)region.x+x)*4u;
    if(pixel[3]!=255u) return 0;
  }
  return 1;
}

int gml_render_cpu_replay_eligible(const GmlRenderDrawOp *operation,
                                    const GmlRenderDrawStateSnapshot *state,
                                    const GmlRenderResourceSource *source){
  const GmlRenderTargetRef *target;
  const GmlRenderQuad *quad;
  GmlRenderRect region;
  double u0,u1,v0,v1;
  double x0,x1,y0,y1;
  if(!operation || operation->reconstruction_mode!=GML_RENDER_REPLAY_EXACT) return 0;
  target=&operation->target;
  if(!target->width || !target->height || target->width>INT_MAX || target->height>INT_MAX ||
     (uint64_t)target->width*target->height>INT_MAX ||
     (target->pixel_format!=GML_RENDER_PIXEL_XRGB8888 &&
      target->pixel_format!=GML_RENDER_PIXEL_ARGB8888) ||
     !full_rect(target->region,target->width,target->height)) return 0;
  if(operation->opcode==GML_RENDER_OP_CLEAR)
    return operation->source_count==0u;
  if(operation->opcode==GML_RENDER_OP_COLOURED_PRIMITIVE)
    return operation->source_count==0u && !operation->sources && !source &&
      operation->payload.primitive.kind==GML_RENDER_PRIMITIVE_FLAT &&
      (operation->payload.primitive.colour>>24)==255u &&
      source_rect(operation->payload.primitive.region,target->width,target->height);
  if(operation->opcode!=GML_RENDER_OP_TEXTURED_QUAD || !state || !source_valid(source) ||
     operation->source_count!=1u || !operation->sources ||
     !same_ref(operation->sources[0],source->image) ||
     (operation->sources[0].resource_class==target->image.resource_class &&
      operation->sources[0].identity==target->image.identity &&
      operation->sources[0].incarnation==target->image.incarnation) ||
     state->write_mask!=15u || state->colour!=0xFFFFFFFFu || state->alpha!=255u ||
     state->alpha_test_enabled || state->coverage_policy ||
     state->material_identity || state->material_version ||
     state->sampler_count!=1u || blend_rule(state)<0 ||
     (blend_rule(state)==1 && target->pixel_format!=GML_RENDER_PIXEL_XRGB8888) ||
     (blend_rule(state)==3 && (target->pixel_format!=GML_RENDER_PIXEL_ARGB8888 ||
                              operation->scene_family!=GML_RENDER_SCENE_BACKGROUND)) ||
     (blend_rule(state)==4 && (target->pixel_format!=GML_RENDER_PIXEL_ARGB8888 ||
                              operation->scene_family!=GML_RENDER_SCENE_SPRITE ||
                              source->width!=1u || source->height!=1u)) ||
     !((state->clip.width==0u && state->clip.height==0u) ||
       full_rect(state->clip,target->width,target->height))) return 0;
  quad=&operation->payload.quad;
  region=quad->sampling.source_region;
  if(blend_rule(state)==6)
    return operation->scene_family==GML_RENDER_SCENE_SURFACE &&
      target->image.resource_class==GML_RENDER_RESOURCE_SCREEN &&
      source->image.resource_class==GML_RENDER_RESOURCE_APPLICATION &&
      target->pixel_format==GML_RENDER_PIXEL_ARGB8888 &&
      source->pixel_format==GML_RENDER_PIXEL_ARGB8888 &&
      region.width==source->width && region.height==source->height &&
      !memcmp(&state->samplers[0],&quad->sampling,sizeof quad->sampling) &&
      gml_render_automatic_surface_quad_valid(quad,target->width,target->height);
  /* Byte-free source geometry is eligible only for sample-independent replacement.
   * The resource owner separately proves and retains its complete exact source recipe. */
  if(!source->bytes && (blend_rule(state)!=0 ||
                       quad->sampling.quantisation ||
                       quad->sampling.filter!=GML_RENDER_FILTER_NEAREST)) return 0;
  if(blend_rule(state)==5){
    if(operation->scene_family!=GML_RENDER_SCENE_TEXT ||
       source->pixel_format!=GML_RENDER_PIXEL_RGBA8888 ||
       source->stride_bytes!=source->width*4u ||
       source->image.resource_class!=GML_RENDER_RESOURCE_ATLAS ||
       !source->borrowed_immutable ||
       !source_rect(region,source->width,source->height) ||
       state->samplers[0].quantisation!=quad->sampling.quantisation ||
       state->samplers[0].filter!=quad->sampling.filter ||
       state->samplers[0].edge_rule!=quad->sampling.edge_rule ||
       state->samplers[0].logical_margin_mask ||
       !same_rect(state->samplers[0].source_region,region)) return 0;
    return gml_render_filtered_font_quad_valid(quad,target->width,target->height);
  }
  if(quad->sampling.quantisation==GML_RENDER_QUANTISE_STRETCHED_FLOAT)
    return stretched_float_eligible(operation,state,source);
  if(blend_rule(state)==4 &&
     (quad->x[0]!=0.0 || quad->y[0]!=0.0 ||
      quad->x[1]!=(double)target->width || quad->y[2]!=(double)target->height)) return 0;
  if(quad->shape!=GML_RENDER_QUAD_AXIS_ALIGNED ||
     quad->sampling.filter!=GML_RENDER_FILTER_NEAREST ||
     quad->sampling.edge_rule!=GML_RENDER_EDGE_CLAMP_SUBIMAGE ||
     quad->sampling.quantisation || quad->sampling.logical_margin_mask ||
     !source_rect(region,source->width,source->height) ||
     state->samplers[0].filter!=quad->sampling.filter ||
     state->samplers[0].edge_rule!=quad->sampling.edge_rule ||
     state->samplers[0].logical_margin_mask!=quad->sampling.logical_margin_mask ||
     !same_rect(state->samplers[0].source_region,region)) return 0;
  u0=(double)region.x/source->width;
  u1=(double)((uint32_t)region.x+region.width)/source->width;
  v0=(double)region.y/source->height;
  v1=(double)((uint32_t)region.y+region.height)/source->height;
  x0=quad->x[0];x1=quad->x[1];y0=quad->y[0];y1=quad->y[2];
  if(!isfinite(x0) || !isfinite(x1) || !isfinite(y0) || !isfinite(y1) ||
     x0<(double)INT_MIN || y0<(double)INT_MIN ||
     x1>(double)INT_MAX || y1>(double)INT_MAX ||
     x1-x0>(double)UINT32_MAX || y1-y0>(double)UINT32_MAX ||
     (blend_rule(state)!=3 &&
       (x0<0.0 || y0<0.0 || x1>(double)target->width || y1>(double)target->height)) ||
     (blend_rule(state)==3 &&
       (x1<=0.0 || y1<=0.0 || x0>=(double)target->width ||
        y0>=(double)target->height)) ||
     x0!=(double)(int)x0 || x1!=(double)(int)x1 ||
     y0!=(double)(int)y0 || y1!=(double)(int)y1 ||
     x1<=x0 || y1<=y0 ||
     x1-x0!=(double)(uint32_t)(x1-x0) ||
     y1-y0!=(double)(uint32_t)(y1-y0) ||
     (uint32_t)(x1-x0)<region.width || (uint32_t)(y1-y0)<region.height ||
     (uint32_t)(x1-x0)%region.width || (uint32_t)(y1-y0)%region.height ||
     quad->x[3]!=x0 || quad->x[2]!=x1 || quad->y[1]!=y0 || quad->y[3]!=y1 ||
     !coordinate_matches(quad->u[0],quad->u[3]) ||
     !coordinate_matches(quad->u[1],quad->u[2]) ||
     !coordinate_matches(quad->v[0],quad->v[1]) ||
     !coordinate_matches(quad->v[2],quad->v[3]) ||
     !((coordinate_matches(quad->u[0],u0) && coordinate_matches(quad->u[1],u1)) ||
       (coordinate_matches(quad->u[0],u1) && coordinate_matches(quad->u[1],u0))) ||
     !((coordinate_matches(quad->v[0],v0) && coordinate_matches(quad->v[2],v1)) ||
       (coordinate_matches(quad->v[0],v1) && coordinate_matches(quad->v[2],v0))))
    return 0;
  for(uint32_t corner=0;corner<4u;corner++)
    if(quad->corner_colour[corner]!=0xFFFFFFFFu) return 0;
  if(blend_rule(state)==2 && source->pixel_format!=GML_RENDER_PIXEL_XRGB8888)
    for(uint32_t y=0;y<region.height;y++) for(uint32_t x=0;x<region.width;x++){
      const uint8_t *bytes=(const uint8_t*)source->bytes+
        (size_t)((uint32_t)region.y+y)*source->stride_bytes+
        (size_t)((uint32_t)region.x+x)*4u;
      uint32_t packed;
      uint8_t coverage;
      memcpy(&packed,bytes,sizeof packed);
      coverage=source->pixel_format==GML_RENDER_PIXEL_RGBA8888
        ?bytes[3]:(uint8_t)(packed>>24);
      if(coverage!=0u && coverage!=255u) return 0;
    }
  return 1;
}

static uint32_t source_pixel(const GmlRenderResourceSource *source,uint32_t x,uint32_t y){
  const uint8_t *bytes=(const uint8_t*)source->bytes+(size_t)y*source->stride_bytes+(size_t)x*4u;
  uint32_t pixel;
  if(source->pixel_format==GML_RENDER_PIXEL_RGBA8888)
    return ((uint32_t)bytes[3]<<24)|((uint32_t)bytes[0]<<16)|
           ((uint32_t)bytes[1]<<8)|bytes[2];
  memcpy(&pixel,bytes,sizeof pixel);
  if(source->pixel_format==GML_RENDER_PIXEL_XRGB8888) pixel|=0xFF000000u;
  return pixel;
}

int gml_render_cpu_replay_one(uint32_t *target,uint32_t width,uint32_t height,
                               const GmlRenderDrawOp *operation,
                               const GmlRenderDrawStateSnapshot *state,
                               const GmlRenderResourceSource *source){
  if(!target || !operation || operation->target.width!=width ||
     operation->target.height!=height ||
     (operation->source_count && (!source || !source->bytes)) ||
     (!gml_render_particle_line_valid(operation) &&
      !gml_render_classic_alpha_rect_valid(operation) &&
      !gml_render_studio_alpha_rect_valid(operation) &&
      !gml_render_classic_inverse_rect_valid(operation) &&
      !gml_render_rectangle_valid(operation) &&
      !gml_render_cpu_replay_eligible(operation,state,source))) return 0;
  if(operation->opcode==GML_RENDER_OP_CLEAR){
    gml_render_cpu_backend_fill_xrgb(target,(int)((size_t)width*height),
      operation->target.pixel_format==GML_RENDER_PIXEL_XRGB8888
        ?operation->payload.clear_colour|0xFF000000u:operation->payload.clear_colour);
    return 1;
  }
  if(operation->opcode==GML_RENDER_OP_COLOURED_PRIMITIVE){
    if(operation->payload.primitive.kind==GML_RENDER_PRIMITIVE_RECTANGLE){
      const GmlRenderPrimitiveRect *p=&operation->payload.primitive;
      GmlRender *render=(GmlRender*)calloc(1,sizeof *render);
      if(!render) return 0;
      render->fb=render->base_fb=target;
      render->fbw=(int)width; render->fbh=(int)height;
      render->classic=(p->rectangle_flags&GML_RENDER_RECTANGLE_CLASSIC)!=0u;
      render->target_sp=(p->rectangle_flags&GML_RENDER_RECTANGLE_TARGET_ALPHA)?1:0;
      render->alphablend=(int)p->alpha_blend; render->active_shader=-1;
      render->blendmode=(int)((p->rectangle_flags&GML_RENDER_RECTANGLE_BLEND_MASK)>>
                              GML_RENDER_RECTANGLE_BLEND_SHIFT);
      render->blend_equation=render->blend_equation_alpha=1;
      render->alpha=p->alpha; render->color_write_mask=15u;
      uint32_t colours[4];
      for(unsigned c=0;c<4u;c++){
        uint32_t xrgb=p->corner_colour[c];
        colours[c]=((xrgb&255u)<<16)|(xrgb&0xFF00u)|((xrgb>>16)&255u);
      }
      gml_render_cpu_rectangle_color(render,p->authored_region.x,p->authored_region.y,
        p->authored_region.x+(int)p->authored_region.width-1,
        p->authored_region.y+(int)p->authored_region.height-1,
        colours[0],colours[1],colours[2],colours[3],
        (p->rectangle_flags&GML_RENDER_RECTANGLE_OUTLINE)!=0u);
      gml_render_flush_pending_fill(render);
      free(render);
      return 1;
    }
    if(operation->payload.primitive.kind==GML_RENDER_PRIMITIVE_CLASSIC_ALPHA_RECT ||
       operation->payload.primitive.kind==GML_RENDER_PRIMITIVE_STUDIO_ALPHA_RECT ||
       operation->payload.primitive.kind==GML_RENDER_PRIMITIVE_CLASSIC_INVERSE_RECT){
      const GmlRenderPrimitiveRect *p=&operation->payload.primitive;
      GmlRender *render=(GmlRender*)calloc(1,sizeof *render);
      if(!render) return 0;
      render->fb=render->base_fb=target;
      render->fbw=(int)width; render->fbh=(int)height;
      render->classic=p->kind!=GML_RENDER_PRIMITIVE_STUDIO_ALPHA_RECT;
      render->alphablend=1; render->active_shader=-1;
      render->blendmode=p->kind==GML_RENDER_PRIMITIVE_CLASSIC_INVERSE_RECT?2:0;
      render->blend_equation=render->blend_equation_alpha=1;
      render->alpha=p->alpha; render->color_write_mask=15u;
      uint32_t colour=((p->colour&255u)<<16)|(p->colour&0xFF00u)|((p->colour>>16)&255u);
      gml_render_cpu_rectangle(render,p->region.x,p->region.y,
        p->region.x+(int)p->region.width-1,p->region.y+(int)p->region.height-1,colour,0);
      free(render);
      return 1;
    }
    if(operation->payload.primitive.kind==GML_RENDER_PRIMITIVE_PARTICLE_LINE){
      const GmlRenderPrimitiveRect *p=&operation->payload.primitive;
      uint32_t colour=((p->colour&255u)<<16)|(p->colour&0xFF00u)|((p->colour>>16)&255u);
      gml_particle_raster_line(target,(int)width,(int)height,p->centre_x,p->centre_y,
        p->scale_x,p->scale_y,p->angle,colour,p->alpha,p->interpolate,p->alpha_blend);
      return 1;
    }
    const GmlRenderRect *region=&operation->payload.primitive.region;
    uint32_t colour=operation->payload.primitive.colour;
    for(uint32_t y=0;y<region->height;y++)
      gml_render_cpu_backend_fill_xrgb(
        target+(size_t)((uint32_t)region->y+y)*width+(uint32_t)region->x,
        (int)region->width,colour);
    return 1;
  }
  if(blend_rule(state)==6){
    const GmlRenderQuad *q=&operation->payload.quad;
    GmlWin content={0};
    GmlRender *render=(GmlRender*)calloc(1,sizeof *render);
    if(!render) return 0;
    content.bytecode=q->sampling.quantisation==GML_RENDER_QUANTISE_AUTOMATIC_SURFACE_FIRSTGEN?15:17;
    render->win=&content;
    render->fb=render->base_fb=target;
    render->fbw=(int)width; render->fbh=(int)height;
    render->app_surface=(uint32_t*)source->bytes;
    render->app_w=(int)source->width; render->app_h=(int)source->height;
    render->app_surface_opaque=(int)q->source_opaque;
    render->active_shader=-1; render->alphablend=1; render->color_write_mask=15u;
    render->interp=q->sampling.filter==GML_RENDER_FILTER_BOX;
    draw_surface_region(render,0,0,0,source->width,source->height,
      q->x[0],q->y[0],q->x[1]-q->x[0],q->y[2]-q->y[0],0xFFFFFFu,1.0);
    free(render);
    return 1;
  }
  if(blend_rule(state)==5){
    const GmlRenderQuad *q=&operation->payload.quad;
    GmlRenderRect region=q->sampling.source_region;
    GmlAtlas atlas={0};
    GmlTpag page={0};
    GmlWin content={0};
    GmlRender *render=(GmlRender*)calloc(1,sizeof *render);
    if(!render) return 0;
    content.bytecode=q->sampling.quantisation==GML_RENDER_QUANTISE_FONT_MODERN?17:16;
    atlas.px=(uint8_t*)source->bytes;
    atlas.w=(int)source->width; atlas.h=(int)source->height;
    page.sx=region.x; page.sy=region.y;
    page.sw=page.tw=page.bw=(int)region.width;
    page.sh=page.th=page.bh=(int)region.height;
    render->win=&content;
    render->fb=target; render->fbw=(int)width; render->fbh=(int)height;
    render->active_shader=-1;
    render->alphablend=render->interp=1;
    render->target_sp=state->alpha_source_factor==GML_RENDER_BLEND_SOURCE_ALPHA?1:0;
    render->color_write_mask=15u;
    int x0=(int)q->x[0],y0=(int)q->y[0];
    int xx0=x0<0?-x0:0,yy0=y0<0?-y0:0;
    int xx1=(int)fmin(q->x[1],width)-x0;
    int yy1=(int)fmin(q->y[2],height)-y0;
    uint32_t rgba=q->corner_colour[0];
    uint32_t blend=((rgba>>16)&255u)|(rgba&0xFF00u)|((rgba&255u)<<16);
    gml_render_cpu_filtered_tpag_band(render,&atlas,&page,
      0,0,x0,y0,xx0,xx1,yy0,yy1,
      q->inverse_anchor[0],q->inverse_anchor[1],q->inverse_x[0],q->inverse_y[1],
      blend,q->filtered_draw_alpha,NULL,NULL,NULL,
      (unsigned long long)(xx1-xx0)*(unsigned long long)(yy1-yy0));
    free(render);
    return 1;
  }
  if(operation->payload.quad.sampling.quantisation==GML_RENDER_QUANTISE_STRETCHED_FLOAT){
    const GmlRenderQuad *quad=&operation->payload.quad;
    GmlRenderRect region=quad->sampling.source_region;
    GmlAtlas atlas={0};
    GmlTpag page={0};
    GmlRender *render=(GmlRender*)calloc(1,sizeof *render);
    if(!render) return 0;
    atlas.px=(uint8_t*)source->bytes;
    atlas.w=(int)source->width; atlas.h=(int)source->height;
    page.sx=region.x; page.sy=region.y;
    page.sw=page.bw=(int)region.width; page.sh=page.bh=(int)region.height;
    render->fb=target; render->fbw=(int)width; render->fbh=(int)height;
    render->active_shader=-1;
    render->alphablend=render->interp=1;
    render->color_write_mask=15u;
    int applied=gml_render_cpu_bilinear_sprite_stretch(render,&atlas,&page,
      (int)quad->x[0],(int)quad->y[0],(int)quad->span_x[1],(int)quad->span_y[1],
      (int)region.width,(int)region.height,255,255,255,1.0);
    free(render);
    return applied;
  }
  {
    const GmlRenderQuad *quad=&operation->payload.quad;
    GmlRenderRect region=quad->sampling.source_region;
    int64_t left=(int64_t)quad->x[0],top=(int64_t)quad->y[0];
    uint32_t draw_width=(uint32_t)(quad->x[1]-quad->x[0]);
    uint32_t draw_height=(uint32_t)(quad->y[2]-quad->y[0]);
    uint32_t scale_x=draw_width/region.width,scale_y=draw_height/region.height;
    int mirror_x=quad->u[0]>quad->u[1],mirror_y=quad->v[0]>quad->v[2];
    int exact=blend_rule(state)==1;
    int binary=blend_rule(state)==2;
    int additive=blend_rule(state)==3;
    int veil=blend_rule(state)==4;
    uint32_t first_x=additive && left<0?(uint32_t)-left:0u;
    uint32_t first_y=additive && top<0?(uint32_t)-top:0u;
    uint32_t last_x=additive && left+(int64_t)draw_width>width
      ?(uint32_t)((int64_t)width-left):draw_width;
    uint32_t last_y=additive && top+(int64_t)draw_height>height
      ?(uint32_t)((int64_t)height-top):draw_height;
    for(uint32_t y=first_y;y<last_y;y++) for(uint32_t x=first_x;x<last_x;x++){
      uint32_t sample_x=x/scale_x,sample_y=y/scale_y;
      uint32_t source_x=(uint32_t)region.x+
        (mirror_x?region.width-1u-sample_x:sample_x);
      uint32_t source_y=(uint32_t)region.y+
        (mirror_y?region.height-1u-sample_y:sample_y);
      uint32_t pixel=source_pixel(source,source_x,source_y);
      uint32_t *destination=&target[(size_t)(top+y)*width+(size_t)(left+x)];
      if(veil){
        if((pixel>>24)==0u) continue;
        double sa=(pixel>>24)/255.0;
        *destination=gml_render_cpu_coarse_veil_pixel(*destination,
          (pixel>>16)&255u,(pixel>>8)&255u,pixel&255u,sa,1.0-sa);
      } else if(additive){
        if((pixel>>24)==0u) continue;
        double sa=((pixel>>24)/255.0)*state->additive_draw_alpha;
        *destination=gml_render_cpu_additive_pixel(*destination,
          (int)((pixel>>16)&255u),(int)((pixel>>8)&255u),(int)(pixel&255u),sa);
      } else if(exact){
        uint32_t factor=pixel>>24;
        uint32_t inverse;
        if(factor==255u) factor=256u;
        inverse=256u-factor;
        *destination=blend_fast8_cached(*destination,
          (pixel&0x00FF00FFu)*factor,(pixel&0x0000FF00u)*factor,inverse,0);
      } else if(!binary || (pixel>>24)==255u)
        *destination=operation->target.pixel_format==GML_RENDER_PIXEL_XRGB8888
        ?pixel|0xFF000000u:pixel;
    }
  }
  return 1;
}
