/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Shared sprite source and subrectangle preparation. */
#include "gml_render.h"
#include "gml_render_internal.h"
#include "gml_render_sprites_internal.h"
#include "gml_render_surfaces_internal.h"
#include "gml_render_cpu_blit.h"
#include "gml_software3d.h"
#include "anygm_host.h"

#include <math.h>
#include <limits.h>
#include <stddef.h>
#include <stdlib.h>

/* Coarse-mip (flat area-average) texel of a straight-RGBA atlas sub-region, built by successive
 * 2x2 box reduction to 1x1 — i.e. the top mip level of the texture page.
 *
 * Why: GameMaker enables mipmapping on interpolated ("texture_set_interpolation(true)") sprite
 * pages. When such a sprite is heavily minified to fill the screen,
 * the GPU's LOD lands on the coarsest mip, so every output pixel samples the same flat average
 * texel — the overlay becomes a uniform veil, NOT a per-pixel down-filtered copy of the texture.
 * A per-pixel box-average (the generic filtered-downscale path below) instead reproduces the fine
 * scanline structure, which is both visibly and numerically wrong (systematically darker where the
 * sprite's bright, low-alpha rows should dominate). Reproduce the flat veil by compositing this
 * single coarse texel across the whole rect. Reduction is done in float (no per-level 8-bit
 * rounding) to stay as close as possible to the GPU's mip value. */
void sprite_region_coarse_texel(GmlAtlas *a, int sx, int sy, int sw, int sh, double out[4]){
  out[0]=out[1]=out[2]=out[3]=0;
  if(!a || !a->px || sw<=0 || sh<=0) return;
  /* Clamp source coordinates before reading atlas pixels. */
  if(a->w<=0 || a->h<=0) return;
  if(sx<0){ sw+=sx; sx=0; }
  if(sy<0){ sh+=sy; sy=0; }
  if(sx>=a->w || sy>=a->h) return;
  if(sx+sw>a->w) sw=a->w-sx;
  if(sy+sh>a->h) sh=a->h-sy;
  if(sw<=0 || sh<=0) return;
  int w=sw, h=sh;
  float *buf=(float*)malloc((size_t)w*(size_t)h*4u*sizeof(float));
  if(!buf) return;
  for(int y=0;y<h;y++){
    const uint8_t *sp=a->px + ((size_t)(sy+y)*a->w + sx)*4;
    float *d=buf+(size_t)y*w*4;
    for(int x=0;x<w;x++,sp+=4,d+=4){ d[0]=sp[0]; d[1]=sp[1]; d[2]=sp[2]; d[3]=sp[3]; }
  }
  while(w>1 || h>1){                                   /* one mip step: halve each axis (floor) */
    int nw = w>1 ? w/2 : 1, nh = h>1 ? h/2 : 1;
    for(int y=0;y<nh;y++){
      int y0=(h>1)?2*y:0, y1=(h>1)?2*y+1:0;
      for(int x=0;x<nw;x++){
        int x0=(w>1)?2*x:0, x1=(w>1)?2*x+1:0;
        const float *p00=buf+((size_t)y0*w+x0)*4, *p01=buf+((size_t)y0*w+x1)*4;
        const float *p10=buf+((size_t)y1*w+x0)*4, *p11=buf+((size_t)y1*w+x1)*4;
        float *d=buf+((size_t)y*nw+x)*4;              /* write compacts behind the read cursor */
        for(int c=0;c<4;c++) d[c]=(p00[c]+p01[c]+p10[c]+p11[c])*0.25f;
      }
    }
    w=nw; h=nh;
  }
  out[0]=buf[0]; out[1]=buf[1]; out[2]=buf[2]; out[3]=buf[3];
  free(buf);
}

static void resident_classify_page(GmlTpag *page,const GmlAtlas *atlas){
  if(page->scene_opaque_checked) return;
  page->scene_fully_opaque=1;
  page->scene_binary_alpha=1;
  page->scene_ax0=page->sw; page->scene_ay0=page->sh;
  page->scene_ax1=-1; page->scene_ay1=-1;
  for(int row=0;row<page->sh;row++)
    for(int column=0;column<page->sw;column++){
      uint8_t coverage=atlas->px[((size_t)(page->sy+row)*atlas->w+page->sx+column)*4u+3u];
      if(coverage!=255u) page->scene_fully_opaque=0;
      if(coverage!=0u && coverage!=255u) page->scene_binary_alpha=0;
      if(coverage){
        if(column<page->scene_ax0) page->scene_ax0=column;
        if(row<page->scene_ay0) page->scene_ay0=row;
        if(column>page->scene_ax1) page->scene_ax1=column;
        if(row>page->scene_ay1) page->scene_ay1=row;
      }
    }
  page->scene_opaque_checked=1;
}

static uint32_t resident_page_endpoint_rgb_mask(GmlTpag *page,const GmlAtlas *atlas){
  if(page->scene_endpoint_rgb_checked) return page->scene_endpoint_rgb_mask;
  uint32_t mask=7u;
  for(int row=0;row<page->sh && mask;row++)
    for(int column=0;column<page->sw && mask;column++){
      const uint8_t *pixel=atlas->px+((size_t)(page->sy+row)*atlas->w+page->sx+column)*4u;
      if(!pixel[3]) continue;
      for(unsigned channel=0;channel<3u;channel++)
        if(pixel[channel]!=0u && pixel[channel]!=255u) mask&=~(1u<<channel);
    }
  page->scene_endpoint_rgb_mask=mask;
  page->scene_endpoint_rgb_checked=1;
  return mask;
}

int gml_render_sprite_log_enabled(const GmlRender *render){
  return render_setting(render,"GML_LOG_SPRITE")!=NULL;
}

int gml_render_runtime_frame_index(GmlSprite *s, int frame){
  if(!s || s->n_frames<=0) return 0;
  return ((frame%s->n_frames)+s->n_frames)%s->n_frames;
}
const uint8_t *runtime_frame_rgba(GmlSprite *s, int frame){
  if(!s->runtime_rgba || s->w<=0 || s->h<=0 || s->n_frames<=0) return NULL;
  int sub=gml_render_runtime_frame_index(s,frame);
  return s->runtime_rgba + (size_t)sub*s->w*s->h*4;
}

/* An integer-aligned atlas crop at an integer scale has exact nearest texel centres. Unit scale
 * also has identity sampling when interpolation is enabled. Authored trim offsets move the
 * visible crop, not its texels. Binary-alpha crops use endpoint source-over; ordinary sprites
 * with filtering, fractional scale or programs retain their separately qualified paths. Partial-alpha Studio
 * sprites use fixed source-over only with witnessed on-demand device authority.
 * The separate TEXT family admits unit-scale intermediate source alpha with a checkpoint. */
static int resident_atlas_crop_from_page(GmlRender *r,GmlTpag *page,
                                         int crop_x,int crop_y,int crop_width,int crop_height,
                                         double left,double top,double xs,double ys,
                                         GmlRenderSceneFamily family,uint32_t colour,
                                         double draw_alpha,int physical_sprite_page,
                                         const GmlRenderResourceSource *source_override);
static void modern_point_axis_prune(double leading,double scale,double phase_offset,
                                    int source_extent,double *start,double *extent);
static int resident_modern_point_regions(GmlRender *r,GmlTpag *page,
  double left,double top,double xs,double ys,double phase_x,double phase_y,
  const GmlRenderDrawOp *operation,const GmlRenderDrawStateSnapshot *state,
  const GmlRenderResourceSource *source,const GmlRenderRotatedCoverageRegion *maps,uint32_t map_count);
static int resident_identity_screen_gui(const GmlRender *r){
  return r && r->gui_pass_active && r->base_scene_resource &&
         r->fb==r->base_scene_resource_pixels &&
         r->fbw==r->base_scene_resource_width &&
         r->fbh==r->base_scene_resource_height &&
         r->gui_base_logical_w==r->fbw && r->gui_base_logical_h==r->fbh &&
         r->gui_logical_w==r->fbw && r->gui_logical_h==r->fbh &&
         r->gui_scale_x==1.0 && r->gui_scale_y==1.0 &&
         !r->gui_maximise_active &&
         r->gui_maximise_xoffset==0.0 && r->gui_maximise_yoffset==0.0 &&
         r->cam_x==0.0 && r->cam_y==0.0;
}
/* R03-P9-FILTERED-REAL-TEXT: shared text has already mapped its point and scale.
 * An exact glyph therefore consumes physical coordinates, including nonuniform
 * logical GUI scaling, without applying that transform again in the executor. */
static int resident_mapped_screen_gui_font(const GmlRender *r){
  return r && r->gui_pass_active && r->base_scene_resource &&
         r->fb==r->base_scene_resource_pixels &&
         r->fbw==r->base_scene_resource_width &&
         r->fbh==r->base_scene_resource_height &&
         r->gui_base_logical_w==r->fbw && r->gui_base_logical_h==r->fbh &&
         r->gui_logical_w>0 && r->gui_logical_h>0 &&
         isfinite(r->gui_scale_x) && r->gui_scale_x>0.0 &&
         isfinite(r->gui_scale_y) && r->gui_scale_y>0.0 &&
         !r->gui_maximise_active &&
         r->gui_maximise_xoffset==0.0 && r->gui_maximise_yoffset==0.0 &&
         r->target_sp==0 && r->target_id<0 &&
         !r->world_transform_active && r->cam_x==0.0 && r->cam_y==0.0;
}
/* R03-SPR-GUI-REPLACE: ONE/ZERO copies even zero-alpha source RGB. The
 * identity SCREEN leaf stamps opaque alpha; it needs neither a predecessor
 * snapshot nor a source-over approximation. Keep other replacement states out. */
static int resident_identity_gui_replacement(const GmlRender *r,double xs,double ys,
                                            uint32_t colour,double alpha){
  return r && !r->classic && r->blendmode==6 && !r->interp && !r->fog_flat &&
         r->target_id<0 && r->target_sp==0 && resident_identity_screen_gui(r) &&
         xs==1.0 && ys==1.0 && colour==0xFFFFFFu && alpha==1.0 &&
         r->device.context && r->device.observe_on_demand &&
         r->device.observe_on_demand(r->device.context);
}
static int resident_classic_rotated_sprite_noop(GmlRender *r,GmlSprite *sprite,int subimg,
                                               double x,double y,double xs,double ys,double rotation){
  if(!r || !r->classic || !r->resident_application_pass || !sprite ||
     sprite->runtime_rgba || sprite->spine || sprite->ns_enabled ||
     sprite->n_frames<=0 || !sprite->frame || sprite->w<=0 || sprite->h<=0 ||
     r->interp || r->gui_pass_active || r->fog_flat || gml_d3_is_active(r) ||
     r->active_shader>=0 || r->alpha_test_enable || !r->alphablend || r->blendmode ||
     r->blend_equation!=1 || r->blend_equation_alpha!=1 || r->color_write_mask!=15u ||
     r->classic_phase_y || r->rotated_batch_count || r->rotated_batch_building ||
     r->pending_underlay || r->pending_presentation ||
     (r->world_transform_active && (r->world_scale_x!=1.0 || r->world_scale_y!=1.0)) ||
     !gml_render_scene_target_matches(r) ||
     !isfinite(x) || !isfinite(y) || fabs(x)>65536.0 || fabs(y)>65536.0 ||
     !isfinite(xs) || !isfinite(ys) || xs<=0.0 || ys<=0.0 || xs>4.0 || ys>4.0 ||
     !isfinite(rotation) || rotation==0.0) return 0;
  for(unsigned index=0;index<3u;index++)
    if(r->classic_interp_phase[index] || r->app_interp_phase[index]) return 0;
  int sub=gml_render_runtime_frame_index(sprite,subimg),page_index=sprite->frame[sub];
  if(page_index<0 || page_index>=r->n_tpag) return 0;
  GmlTpag *page=&r->tpag[page_index];
  if(page->tx<0 || page->ty<0 || page->sw<=0 || page->sh<=0 ||
     page->sw>4096 || page->sh>4096 ||
     (uint64_t)(uint32_t)page->tx+(uint32_t)page->sw>(uint32_t)sprite->w ||
     (uint64_t)(uint32_t)page->ty+(uint32_t)page->sh>(uint32_t)sprite->h) return 0;
  /* R03-SPR-CLASSIC-ROTATED-NOOP: the unchanged CPU rotation owns the
   * conservative bounds, including trim, origin, camera and its safety rim.
   * An empty result preserves the queued prefix without sampling a target,
   * executing a shader or claiming a visible GPU rotation. */
  return gml_render_rotated_sprite_is_clipped(r,sprite,page,x,y,xs,ys,rotation);
}

static int resident_integer_sprite(GmlRender *r,GmlSprite *sprite,int subimg,
                                  double x,double y,double xs,double ys,double rotation,
                                  uint32_t blend,double alpha){
  GmlTpag *page;
  double left,top;
  int sub,page_index;
  int replace=resident_identity_gui_replacement(r,xs,ys,blend,alpha);
  /* R03-SPR-INTEGER-FUNCTION: public point/scale mapping has already happened.
   * Whole physical texels need no phase emulation, including both reflections,
   * trim, mapped GUI and uniform tint on any ordinary target. */
  int normal_integer=r && !r->interp && !r->fog_flat && !gml_d3_is_active(r) &&
    isfinite(xs) && isfinite(ys) && fabs(xs)>=1.0 && fabs(xs)<=8.0 &&
    fabs(ys)>=1.0 && fabs(ys)<=8.0 && xs==floor(xs) && ys==floor(ys) &&
    rotation==0.0 && !r->blendmode && r->device.context && r->device.observe_on_demand &&
    r->device.observe_on_demand(r->device.context);
  if(!r || !r->resident_application_pass || !sprite || sprite->runtime_rgba ||
     sprite->spine || sprite->ns_enabled || sprite->n_frames<=0 ||
     sprite->w<=0 || sprite->h<=0 ||
     (r->classic && !normal_integer && (gml_d3_is_active(r) ||
                     r->interp ||
                     ((fabs(xs)!=1.0 || fabs(ys)!=1.0) &&
                      (xs<=0.0 || ys<=0.0 || xs>4.0 || ys>4.0)) ||
                     (r->world_transform_active &&
                      (r->world_scale_x!=1.0 || r->world_scale_y!=1.0)))) ||
     (r->gui_pass_active && !normal_integer && !resident_identity_screen_gui(r)) ||
     r->active_shader>=0 || r->alpha_test_enable || (!r->alphablend && !normal_integer) ||
     (r->blendmode && !replace) || r->blend_equation!=1 || r->blend_equation_alpha!=1 ||
     r->color_write_mask!=0x0Fu || r->classic_phase_y ||
     r->rotated_batch_count || r->rotated_batch_building ||
     r->pending_underlay || r->pending_presentation ||
     !gml_render_scene_target_matches(r) ||
     !isfinite(xs) || !isfinite(ys) || xs==0.0 || ys==0.0 ||
     rotation!=0.0 || (blend!=0xFFFFFFu && !normal_integer) ||
     !isfinite(alpha) || alpha<0.0 || alpha>1.0 || (alpha==0.0 && !normal_integer) ||
     (r->interp && (xs!=1.0 || ys!=1.0)))
    return 0;
  /* A plain sprite reads only the base image; the other texture-stage filter flags do not
   * participate in the CPU blit, and the integer-aligned unit-scale base sample is exact. */
  for(int index=0;index<3;index++)
    if(r->classic_interp_phase[index] || r->app_interp_phase[index]) return 0;
  if(normal_integer && alpha==0.0) return 1;
  sub=gml_render_runtime_frame_index(sprite,subimg);
  if(!sprite->frame) return 0;
  page_index=sprite->frame[sub];
  if(page_index<0 || page_index>=r->n_tpag) return 0;
  page=&r->tpag[page_index];
  /* Replacement writes authored transparent margins as opaque black on SCREEN.
   * A cropped quad alone cannot implement that additional logical coverage. */
  if(replace && (page->tx || page->ty || page->bw!=page->sw || page->bh!=page->sh ||
                 page->tw!=page->sw || page->th!=page->sh)) return 0;
  /* Ordinary Studio nearest blits project and sample the physical sw/sh crop.
   * Stored tw/th does not rescale that leaf. Keep the original page identity
   * and cached alpha classification; other callers retain their layout guard. */
  if(page->sw<=0 || page->sh<=0 || page->sx<0 || page->sy<0 ||
     page->tx<0 || page->ty<0 ||
     (uint64_t)(uint32_t)page->tx+(uint32_t)page->sw>(uint32_t)sprite->w ||
     (uint64_t)(uint32_t)page->ty+(uint32_t)page->sh>(uint32_t)sprite->h ||
     page->tw<=0 || page->th<=0 ||
     ((r->classic || r->interp) &&
      (page->tw!=page->sw || page->th!=page->sh)) ||
     page->bw!=sprite->w || page->bh!=sprite->h ||
     page->atlas<0 || page->atlas>=r->n_atlas) return 0;
  left=x-(double)sprite->originx*xs+(double)page->tx*xs-r->cam_x;
  top=y-(double)sprite->originy*ys+(double)page->ty*ys-r->cam_y;
  if(r->classic && fabs(xs)==1.0 && fabs(ys)==1.0 &&
     isfinite(left) && isfinite(top) &&
     fabs(left)<1000000.0 && fabs(top)<1000000.0){
    /* The established early-classic unit blit rounds the physical crop origin half up.
     * Keep texelFetch sampling exact after projecting that same coverage. */
    left=floor(left+0.5);
    top=floor(top+0.5);
    /* R03-SPR-CLASSIC-SIGNED-NOOP: both unit signs have exact half-open
     * coverage. A clipped reflected crop must not drain a queued GPU prefix.
     * Visible reflected sprites need the paired integer-function rule below. */
    double right=left+(double)page->sw*xs;
    double bottom=top+(double)page->sh*ys;
    if(fmin(left,right)>=(double)r->fbw || fmin(top,bottom)>=(double)r->fbh ||
       fmax(left,right)<=0.0 || fmax(top,bottom)<=0.0)
      return 1;
  }
  if(r->classic && !normal_integer && (xs<0.0 || ys<0.0)) return 0;
  /* R03-SPR-INTEGER-ROUNDED-NOOP: positive whole nearest crops use the
   * unchanged CPU writer's half-up anchor and exact integral extent. A
   * subpixel sliver outside that rounded rectangle cannot touch the target.
   * Integral cameras exclude the modern world's separate half-camera tie. */
  if(!r->classic && !r->interp && !r->gui_pass_active && !r->fog_flat &&
     !gml_d3_is_active(r) && alpha==1.0 && xs>=1.0 && xs<=8.0 && ys>=1.0 && ys<=8.0 &&
     xs==floor(xs) && ys==floor(ys) &&
     r->cam_x==floor(r->cam_x) && r->cam_y==floor(r->cam_y) &&
     isfinite(left) && isfinite(top) && fabs(left)<=65536.0 && fabs(top)<=65536.0 &&
     (double)page->sw*xs<=65536.0 && (double)page->sh*ys<=65536.0){
    double rounded_left=floor(left+0.5),rounded_top=floor(top+0.5);
    if(rounded_left>=r->fbw || rounded_top>=r->fbh ||
       rounded_left+(double)page->sw*xs<=0.0 ||
       rounded_top+(double)page->sh*ys<=0.0) return 1;
  }
  /* R03-SPR-NEAREST: public dispatch has already projected point and scale.
   * The crop validates final integral coverage; world state is not a second
   * transform or an independent reason to reject an exact nearest sprite. */
  /* A fully offscreen crop cannot write the application surface. Keep the
   * margin conservative for fractional coordinates and mirrored draws. */
  {
    double right=left+(double)page->sw*xs;
    double bottom=top+(double)page->sh*ys;
    if(isfinite(left) && isfinite(top) && isfinite(right) && isfinite(bottom) &&
       (fmax(left,right)<=-1.0 || fmin(left,right)>=(double)r->fbw+1.0 ||
        fmax(top,bottom)<=-1.0 || fmin(top,bottom)>=(double)r->fbh+1.0))
      return 1;
  }
  /* Retain already qualified, cheaper recipes before extending admission.
   * Integral Classic magnification does not need the fractional point-region
   * partitioner; the plain whole-texel quad preserves compatible batching. */
  if(!(normal_integer && r->classic && (fabs(xs)!=1.0 || fabs(ys)!=1.0) &&
       left==floor(left) && top==floor(top)) &&
     resident_atlas_crop_from_page(r,page,0,0,page->sw,page->sh,
                                       left,top,xs,ys,GML_RENDER_SCENE_SPRITE,blend,alpha,
                                       r->classic && (xs!=1.0 || ys!=1.0)?3:
                                       (!r->classic && !r->interp),NULL)) return 1;
  return normal_integer && resident_atlas_crop_from_page(r,page,0,0,page->sw,page->sh,
       left,top,xs,ys,GML_RENDER_SCENE_SPRITE,blend,alpha,7,NULL);
}

static int resident_atlas_crop_from_page(GmlRender *r,GmlTpag *page,
                                         int crop_x,int crop_y,int crop_width,int crop_height,
                                         double left,double top,double xs,double ys,
                                         GmlRenderSceneFamily family,uint32_t colour,
                                         double draw_alpha,int physical_sprite_page,
                                         const GmlRenderResourceSource *source_override){
  GmlAtlas *atlas;
  GmlAtlas runtime_atlas;
  GmlRenderResourceSource source={0};
  GmlRenderResource *target;
  GmlRenderDrawOp operation={0};
  GmlRenderDrawStateSnapshot state={0};
  GmlRenderImageRef source_ref,image;
  GmlRenderRect region;
  double scaled_width,scaled_height,source_left,source_right,source_top,source_bottom;
  int checkpoint_alpha_background,checkpoint_alpha_text,checkpoint_alpha_sprite;
  int checkpoint_alpha_gui,checkpoint_integer_tile;
  if(source_override && (!r || !page || !r->classic || r->interp ||
     family!=GML_RENDER_SCENE_SPRITE || physical_sprite_page!=4 ||
     xs!=1.0 || ys!=1.0 || colour!=0xFFFFFFu || draw_alpha!=1.0 ||
     !page->scene_opaque_checked || !page->scene_binary_alpha ||
     !source_override->borrowed_immutable || !source_override->bytes ||
     source_override->image.resource_class!=GML_RENDER_RESOURCE_ATLAS ||
     !(source_override->image.identity&UINT32_C(0x80000000)) ||
     !source_override->image.incarnation || source_override->image.content_version!=1u ||
     source_override->pixel_format!=GML_RENDER_PIXEL_RGBA8888 ||
     source_override->width!=(uint32_t)page->sw ||
     source_override->height!=(uint32_t)page->sh ||
     source_override->width>4096u || source_override->height>4096u ||
     source_override->stride_bytes!=source_override->width*4u ||
     source_override->byte_capacity<(uint64_t)source_override->width*source_override->height*4u))
    return 0;
  /* Mode 2 is only a separately proved depth-free orthographic crop. */
  int orthographic=physical_sprite_page==2;
  /* R03-SPR-CLASSIC-SCALED-NEAREST: preserve the CPU point phase and rounded
   * coverage through the existing bounded UV-region producer and hardware
   * nearest sampler. This is not a new float shader or a CPU pixel writer. */
  int classic_point=physical_sprite_page==3 && r && r->classic &&
    (family==GML_RENDER_SCENE_SPRITE || family==GML_RENDER_SCENE_BACKGROUND) &&
    xs>0.0 && ys>0.0 && xs<=4.0 && ys<=4.0;
  /* R03-TILE-INTEGER-CLIP: the public tile port already resolves world mapping.
   * Integral magnification keeps each destination centre inside one nearest
   * texel; the target viewport clips even a partly visible enlarged texel. */
  int integer_tile=r && !r->classic && !r->interp &&
    family==GML_RENDER_SCENE_TILE && xs>=1.0 && xs<=8.0 && ys>=1.0 && ys<=8.0 &&
    xs==floor(xs) && ys==floor(ys) && isfinite(left) && isfinite(top) &&
    floor(left)==left && floor(top)==top && fabs(left)<=65536.0 && fabs(top)<=65536.0 &&
    (double)crop_width*xs<=65536.0 && (double)crop_height*ys<=65536.0;
  /* R03-TILE-CLASSIC-UNIT: the three public tile owners already resolved
   * their distinct source selection. Qualify only contained positive-unit
   * packed crops and integral coverage; keep phase/filter/transform debt. */
  int classic_tile=r && page && r->classic && !r->interp &&
    family==GML_RENDER_SCENE_TILE && xs==1.0 && ys==1.0 &&
    isfinite(left) && isfinite(top) && floor(left)==left && floor(top)==top &&
    fabs(left)<=65536.0 && fabs(top)<=65536.0 && crop_width<=65536 && crop_height<=65536 &&
    !page->tx && !page->ty && page->bw==page->sw && page->bh==page->sh &&
    !r->gui_pass_active && !r->fog_flat && !gml_d3_is_active(r) &&
    r->device.context && r->device.observe_on_demand &&
    r->device.observe_on_demand(r->device.context);
  /* R03-TEXT-MAPPED-INTEGER: the text owner has already mapped and snapped
   * the glyph. Binary source coverage remains exact at whole magnification,
   * including target clipping; no second world transform belongs here. */
  int integer_text=r && !r->classic && !r->interp && !r->gui_pass_active &&
    !r->fog_flat && !gml_d3_is_active(r) &&
    family==GML_RENDER_SCENE_TEXT && draw_alpha==1.0 &&
    ((colour&0xFFFFFFu)==0u || (colour&0xFFFFFFu)==0xFFFFFFu) &&
    xs>=1.0 && xs<=4.0 && ys>=1.0 && ys<=4.0 &&
    xs==floor(xs) && ys==floor(ys) && isfinite(left) && isfinite(top) &&
    floor(left)==left && floor(top)==top && fabs(left)<=65536.0 && fabs(top)<=65536.0 &&
    (double)crop_width*xs<=65536.0 && (double)crop_height*ys<=65536.0 &&
    r->device.context && r->device.observe_on_demand &&
    r->device.observe_on_demand(r->device.context);
  /* R03-SPR-PART-INTEGER-ENDPOINT: the selected-part owner resolves physical
   * trim and mapping once. Whole positive nearest texels and endpoint RGB
   * multiplication stay exact without a predecessor image or float emulation. */
  int integer_sprite_part=physical_sprite_page==5;
  int integer_sprite=physical_sprite_page==7;
  if(integer_sprite &&
     (!r || !page || r->interp || r->fog_flat || gml_d3_is_active(r) ||
      family!=GML_RENDER_SCENE_SPRITE || xs!=floor(xs) || ys!=floor(ys) ||
      fabs(xs)<1.0 || fabs(xs)>8.0 || fabs(ys)<1.0 || fabs(ys)>8.0 ||
      !isfinite(left) || !isfinite(top) || left!=floor(left) || top!=floor(top) ||
      fabs(left)>65536.0 || fabs(top)>65536.0 || page->sw>4096 || page->sh>4096 ||
      (double)crop_width*fabs(xs)>16384.0 || (double)crop_height*fabs(ys)>16384.0 ||
      (!gml_render_target_preserves_alpha(r) &&
       (double)crop_width*fabs(xs)*(double)crop_height*fabs(ys)>=131072.0) ||
      !r->device.context || !r->device.observe_on_demand ||
      !r->device.observe_on_demand(r->device.context))) return 0;
  if(integer_sprite_part &&
     (!r || r->classic || r->interp || r->gui_pass_active || r->fog_flat ||
      gml_d3_is_active(r) || !anygm_policy_has_modern_layer_semantics(r->win) ||
      r->target_id<=0 || r->target_sp<=0 || family!=GML_RENDER_SCENE_SPRITE ||
      draw_alpha!=1.0 || xs<1.0 || xs>4.0 || ys<1.0 || ys>4.0 ||
      xs!=floor(xs) || ys!=floor(ys) || !isfinite(left) || !isfinite(top) ||
      left!=floor(left) || top!=floor(top) || fabs(left)>65536.0 || fabs(top)>65536.0 ||
      (double)crop_width*xs>65536.0 || (double)crop_height*ys>65536.0 ||
      !r->device.context || !r->device.observe_on_demand ||
      !r->device.observe_on_demand(r->device.context))) return 0;
  /* Packaged point glyphs on modern SCREEN already have resolved, snapped
   * anchors. Fractional enlargement changes sampling, not the text layout. */
  int fractional_text=r && !r->classic && !r->interp && !r->gui_pass_active &&
    !r->fog_flat && !gml_d3_is_active(r) && r->target_id<0 && r->target_sp==0 &&
    anygm_policy_has_modern_layer_semantics(r->win) &&
    family==GML_RENDER_SCENE_TEXT && draw_alpha==1.0 &&
    ((colour&0xFFFFFFu)==0u || (colour&0xFFFFFFu)==0xFFFFFFu) &&
    isfinite(xs) && isfinite(ys) && xs>=1.0 && xs<=4.0 && ys>=1.0 && ys<=4.0 &&
    (xs!=floor(xs) || ys!=floor(ys)) &&
    isfinite(left) && isfinite(top) && floor(left)==left && floor(top)==top &&
    fabs(left)<=65536.0 && fabs(top)<=65536.0 &&
    (double)crop_width*xs<=4096.0 && (double)crop_height*ys<=4096.0 &&
    r->device.context && r->device.observe_on_demand &&
    r->device.observe_on_demand(r->device.context);
  double point_left=left,point_top=top;
  int replace=physical_sprite_page && family==GML_RENDER_SCENE_SPRITE &&
    resident_identity_gui_replacement(r,xs,ys,colour,draw_alpha);
  /* R03-GUI-UNIT-SOURCEOVER: SCREEN stamps covered alpha rather than
   * preserving destination alpha. Keep this leaf to identity point samples,
   * white RGB and full draw alpha; zero source texels still write nothing. */
  int gui_crop=r && !r->classic && !r->interp && !replace &&
    r->target_id<0 && r->target_sp==0 && resident_identity_screen_gui(r) &&
    (family==GML_RENDER_SCENE_SPRITE || family==GML_RENDER_SCENE_TEXT) &&
    xs==1.0 && ys==1.0 && (colour&0xFFFFFFu)==0xFFFFFFu && draw_alpha==1.0;
  /* R03-TEXT-FIRSTGEN-GUI-TINTED-POINT: the existing CPU glyph truncates
   * integer RGB tint before source-over. The point/covered-alpha executor
   * already uses that same source quantisation; only admission is new. */
  int gui_tinted_text=r && !r->classic && !r->interp && !r->fog_flat &&
    !gml_d3_is_active(r) && r->target_id<0 && r->target_sp==0 &&
    resident_identity_screen_gui(r) &&
    anygm_policy_uses_first_generation_studio(r->win) &&
    !anygm_policy_has_modern_layer_semantics(r->win) &&
    family==GML_RENDER_SCENE_TEXT && xs==1.0 && ys==1.0 && draw_alpha==1.0 &&
    (colour&0xFFFFFFu)!=0xFFFFFFu && r->device.context &&
    r->device.observe_on_demand && r->device.observe_on_demand(r->device.context);
  /* R03-SPR-FIRSTGEN-GUI-HALF-POINT: the unchanged SCREEN point leaf
   * selects source texels 2j+1 after snapping coverage. Odd trailing extents
   * sample outside and do not write. Keep this exact reciprocal phase narrow. */
  int gui_half=physical_sprite_page==6 && r && page && !r->classic &&
    !r->interp && !r->fog_flat && !gml_d3_is_active(r) &&
    r->target_id<0 && !r->target_sp && resident_identity_screen_gui(r) &&
    anygm_policy_uses_first_generation_studio(r->win) &&
    !anygm_policy_has_modern_layer_semantics(r->win) &&
    family==GML_RENDER_SCENE_SPRITE && xs==0.5 && ys==0.5 &&
    !r->world_transform_active && draw_alpha>=0x1p-64 &&
    !gml_render_target_is_first_generation_application_surface(r) &&
    (colour&0xFFFFFFu)==0xFFFFFFu && !page->project_authored_edges &&
    page->tw==page->sw && page->th==page->sh &&
    page->sw<=4096 && page->sh<=4096 && isfinite(left) && isfinite(top) &&
    fabs(left)<=65536.0 && fabs(top)<=65536.0 &&
    r->device.context && r->device.observe_on_demand &&
    r->device.observe_on_demand(r->device.context);
  if(physical_sprite_page==6 && !gui_half) return 0;
  gui_crop=gui_crop || gui_tinted_text || gui_half;
  if(replace && page && (page->tx || page->ty || page->bw!=page->sw ||
                         page->bh!=page->sh || page->tw!=page->sw || page->th!=page->sh))
    return 0;
  /* R03-TEXT-PACKAGED-PARTIAL-ALPHA: the shared glyph crop keeps the CPU
   * source-over policy's target-alpha boundary and checkpoints the GPU result. */
  if(!r || !page || !r->resident_application_pass ||
     (r->classic && ((family!=GML_RENDER_SCENE_SPRITE &&
                      family!=GML_RENDER_SCENE_BACKGROUND &&
                      family!=GML_RENDER_SCENE_TEXT && !classic_tile) ||
                     (family==GML_RENDER_SCENE_TEXT && !orthographic &&
                      anygm_policy_classic_modern_presentation(r->win)) ||
                     r->interp || (!orthographic && !classic_point && !integer_sprite &&
                                   (xs!=1.0 || ys!=1.0)))) ||
     (r->world_transform_active &&
      (r->world_scale_x!=1.0 || r->world_scale_y!=1.0) &&
      (!integer_sprite && (r->classic || (family!=GML_RENDER_SCENE_SPRITE && !integer_tile && !integer_text && !fractional_text) || r->interp))) ||
     (r->gui_pass_active && !integer_sprite &&
      ((family!=GML_RENDER_SCENE_SPRITE && !gui_crop) ||
       !resident_identity_screen_gui(r))) ||
     r->active_shader>=0 || r->alpha_test_enable ||
     (!r->alphablend && !integer_sprite) || (r->blendmode && !replace) || r->blend_equation!=1 ||
     r->blend_equation_alpha!=1 || r->color_write_mask!=0x0Fu ||
     r->classic_phase_y || r->rotated_batch_count || r->rotated_batch_building ||
     r->pending_underlay || r->pending_presentation ||
     !gml_render_scene_target_matches(r) ||
     !isfinite(xs) || !isfinite(ys) || xs==0.0 || ys==0.0 ||
     !isfinite(draw_alpha) || draw_alpha<=0.0 || draw_alpha>1.0 ||
     (family!=GML_RENDER_SCENE_TEXT && family!=GML_RENDER_SCENE_SPRITE &&
      draw_alpha!=1.0) ||
     (family==GML_RENDER_SCENE_TEXT && draw_alpha<1.0 &&
      !gml_render_target_preserves_alpha(r) &&
      (!r->fb_opaque_known || !r->fb_all_opaque)) ||
     (r->interp && (xs!=1.0 || ys!=1.0))) return 0;
  for(int index=0;index<3;index++)
    if(r->classic_interp_phase[index] || r->app_interp_phase[index]) return 0;
  if(classic_point && (r->fog_flat || r->gui_pass_active ||
                      !r->device.context || !r->device.observe_on_demand ||
                      !r->device.observe_on_demand(r->device.context))) return 0;
  if(page->sw<=0 || page->sh<=0 || page->sx<0 || page->sy<0 ||
     (!source_override && (page->atlas<0 || page->atlas>=r->n_atlas)) ||
     ((!physical_sprite_page || family!=GML_RENDER_SCENE_SPRITE ||
       r->classic || r->interp) &&
      (page->tw!=page->sw || page->th!=page->sh)) ||
     crop_x<0 || crop_y<0 || crop_width<=0 || crop_height<=0 ||
     (uint64_t)(uint32_t)crop_x+(uint32_t)crop_width>(uint32_t)page->sw ||
     (uint64_t)(uint32_t)crop_y+(uint32_t)crop_height>(uint32_t)page->sh) return 0;
  /* Layout and glyph demand have already happened. A whole-texel glyph outside
   * the half-open target cannot write, even when its tint/source would otherwise
   * need an ordered CPU fallback. Leave a queued GPU prefix resident. */
  if(family==GML_RENDER_SCENE_TEXT && xs==1.0 && ys==1.0 &&
     isfinite(left) && isfinite(top) && floor(left)==left && floor(top)==top &&
     (left>=r->fbw || top>=r->fbh ||
      left+(double)crop_width<=0.0 || top+(double)crop_height<=0.0)) return 1;
  if(source_override){
    source=*source_override;
    runtime_atlas=(GmlAtlas){.px=(uint8_t*)source.bytes,
      .w=(int)source.width,.h=(int)source.height};
    atlas=&runtime_atlas;
  } else {
    atlas=&r->atlas[page->atlas];
    if(!gml_render_atlas_scene_source_view(r,page->atlas,&source)) return 0;
  }
  if(((integer_tile || classic_tile || integer_text) && (source.width>16384u || source.height>16384u)) ||
     (uint64_t)(uint32_t)page->sx+(uint32_t)page->sw>source.width ||
     (uint64_t)(uint32_t)page->sy+(uint32_t)page->sh>source.height) return 0;
  uint32_t required_endpoint_rgb=0u;
  if(integer_sprite_part){
    for(unsigned channel=0;channel<3u;channel++){
      unsigned factor=(colour>>(channel*8u))&255u;
      if(factor!=0u && factor!=255u) required_endpoint_rgb|=1u<<channel;
    }
    /* Reject the newly admitted work before either cached page scan. */
    if(required_endpoint_rgb &&
       (uint64_t)(uint32_t)page->sw*(uint32_t)page->sh>65536u) return 0;
  }
  resident_classify_page(page,atlas);
  if(integer_sprite && (atlas->runtime_font_page || !source.borrowed_immutable ||
     source.width>16384u || source.height>16384u)) return 0;
  if(integer_sprite_part && (!page->scene_binary_alpha || atlas->runtime_font_page ||
     !source.borrowed_immutable || source.width>16384u || source.height>16384u)) return 0;
  /* R03-SPR-PART-ENDPOINT-SOURCE-TINT: tint truncation is exact when either
   * factor is an endpoint. Prove only the required visible source channels,
   * once per immutable small page, leaving the endpoint-tint packet unchanged. */
  if(required_endpoint_rgb){
    uint32_t proven=resident_page_endpoint_rgb_mask(page,atlas);
    if((proven&required_endpoint_rgb)!=required_endpoint_rgb) return 0;
    operation.payload.quad.source_endpoint_rgb_mask=proven;
  }
  /* Do not extend the partial-source-alpha contract with this exact variant. */
  if(integer_text && (!page->scene_binary_alpha || atlas->runtime_font_page) && r->world_transform_active &&
     (r->world_scale_x!=1.0 || r->world_scale_y!=1.0)) return 0;
  integer_text=integer_text && page->scene_binary_alpha && !atlas->runtime_font_page;
  if(fractional_text && (!page->scene_binary_alpha || atlas->runtime_font_page ||
     source.width>16384u || source.height>16384u || !source.borrowed_immutable)) return 0;
  if(r->classic && family==GML_RENDER_SCENE_BACKGROUND){
    /* Classic point backgrounds share the unchanged CPU writer's half-up crop
     * anchor. Logical trim and repeat periods remain with the background owner. */
    if(!page->scene_binary_alpha || !isfinite(left) || !isfinite(top) ||
       fabs(left)>=1000000.0 || fabs(top)>=1000000.0) return 0;
    left=floor(left+0.5);
    top=floor(top+0.5);
    if(left>=r->fbw || top>=r->fbh ||
       left+(double)crop_width*xs<=0.0 || top+(double)crop_height*ys<=0.0) return 1;
  }
  /* R03-SPR-CLASSIC-OWNED-BINARY: endpoint alpha has exact source-over on
   * the owned application too. Keep this extension to packaged positive-unit
   * point samples; partial alpha and special states retain their old scope. */
  int classic_owned_binary=r->classic &&
    !anygm_policy_classic_modern_presentation(r->win) &&
    family==GML_RENDER_SCENE_SPRITE && !physical_sprite_page && !source_override &&
    source.borrowed_immutable && !atlas->runtime_font_page && page->scene_binary_alpha &&
    xs==1.0 && ys==1.0 && colour==0xFFFFFFu && draw_alpha==1.0 &&
    !r->gui_pass_active && !r->fog_flat && !gml_d3_is_active(r) &&
    r->target_id<0 && !r->target_sp && r->fb==r->app_surface &&
    r->fb==r->app_surface_owned && r->device.context && r->device.observe_on_demand &&
    r->device.observe_on_demand(r->device.context);
  if(r->classic && !integer_sprite &&
     ((family==GML_RENDER_SCENE_TEXT && !page->scene_binary_alpha) ||
      (family==GML_RENDER_SCENE_SPRITE && !page->scene_fully_opaque &&
       !(r->target_id>0 && r->target_sp>0) &&
       (r->fb!=r->app_surface ||
        (r->fb!=r->base_scene_resource_pixels && !classic_owned_binary))))) return 0;
  colour&=0xFFFFFFu;
  if(colour!=0xFFFFFFu &&
     !integer_sprite && !integer_sprite_part && !gui_tinted_text &&
     (family!=GML_RENDER_SCENE_TEXT || !page->scene_binary_alpha)) return 0;
  checkpoint_alpha_gui=gui_crop &&
    (family==GML_RENDER_SCENE_TEXT || !page->scene_binary_alpha || gui_half);
  if(checkpoint_alpha_gui &&
     (!r->device.context || !r->device.observe_on_demand ||
      !r->device.observe_on_demand(r->device.context))) return 0;
  checkpoint_alpha_background=!page->scene_binary_alpha &&
    family==GML_RENDER_SCENE_BACKGROUND && !r->interp &&
    anygm_policy_uses_first_generation_studio(r->win) &&
    crop_x==0 && crop_y==0 && crop_width==page->sw && crop_height==page->sh &&
    !page->tx && !page->ty && page->bw==page->sw && page->bh==page->sh &&
    xs==1.0 && ys==1.0;
  checkpoint_alpha_text=!page->scene_binary_alpha &&
    family==GML_RENDER_SCENE_TEXT && !r->interp &&
    (draw_alpha<1.0 || (!gml_render_target_preserves_alpha(r) &&
                        r->fb_opaque_known && r->fb_all_opaque)) &&
    crop_x==0 && crop_y==0 && crop_width==page->sw && crop_height==page->sh &&
    !page->tx && !page->ty && page->bw==page->sw && page->bh==page->sh &&
    xs==1.0 && ys==1.0;
  /* R03-SPR-INTEGER-SOURCEOVER: integral nearest coverage needs no float
   * emulation or destination snapshot. A witnessed device owns partial-alpha
   * results until a genuine pixel/state observation requests canonical bytes. */
  checkpoint_alpha_sprite=!integer_sprite && !replace && family==GML_RENDER_SCENE_SPRITE &&
    !r->interp &&
    ((!r->classic && r->target_sp==0 && r->fb==r->app_surface_owned) ||
     (r->classic && r->target_id>0 && r->target_sp>0) ||
     (r->classic && r->target_sp==0 &&
      !r->gui_pass_active && r->fb==r->app_surface &&
      r->fb==r->base_scene_resource_pixels)) &&
    (draw_alpha<1.0 || !page->scene_binary_alpha);
  if(checkpoint_alpha_sprite &&
     (!r->device.context || !r->device.observe_on_demand ||
      !r->device.observe_on_demand(r->device.context) ||
      (!gml_render_target_preserves_alpha(r) &&
       (!r->fb_opaque_known || !r->fb_all_opaque)))) return 0;
  /* The CPU full-target unit sheet also prepares an application presentation
   * mask. Keep that preparation on its existing leaf until separately qualified. */
  if(checkpoint_alpha_sprite && anygm_policy_uses_first_generation_studio(r->win) &&
     r->app_surface && r->fb==r->app_surface && left==0.0 && top==0.0 &&
     xs==1.0 && ys==1.0 && crop_width==r->fbw && crop_height==r->fbh)
    return 0;
  if(family==GML_RENDER_SCENE_SPRITE && draw_alpha<1.0 &&
     !integer_sprite && !checkpoint_alpha_sprite && !gui_half) return 0;
  if(!replace && !page->scene_binary_alpha && !checkpoint_alpha_background &&
     !integer_sprite && !checkpoint_alpha_text && !checkpoint_alpha_sprite && !checkpoint_alpha_gui) return 0;
  scaled_width=(double)crop_width*fabs(xs);
  scaled_height=(double)crop_height*fabs(ys);
  if(gui_half){
    if(source.width>16384u || source.height>16384u || atlas->runtime_font_page ||
       !source.borrowed_immutable) return 0;
    crop_width=(crop_width/2)*2;crop_height=(crop_height/2)*2;
    scaled_width=crop_width*0.5;scaled_height=crop_height*0.5;
    left=floor(left+0.5);top=floor(top+0.5);
    if(!crop_width || !crop_height || left>=r->fbw || top>=r->fbh ||
       left+scaled_width<=0.0 || top+scaled_height<=0.0) return 1;
  }
  if(classic_point || fractional_text){
    if(!isfinite(left) || !isfinite(top) || fabs(left)>=1000000.0 ||
       fabs(top)>=1000000.0 || crop_width>4096 || crop_height>4096 ||
       scaled_width>4096.0 || scaled_height>4096.0 ||
       source.width>16384u || source.height>16384u) return 0;
    left=floor(left+0.5);top=floor(top+0.5);
    scaled_width=floor(scaled_width+0.5);scaled_height=floor(scaled_height+0.5);
    if(fabs(1.0/xs-nearbyint(1.0/xs))<1e-9) point_left=left;
    if(fabs(1.0/ys-nearbyint(1.0/ys))<1e-9) point_top=top;
    modern_point_axis_prune(point_left,xs,0.0,crop_width,&left,&scaled_width);
    modern_point_axis_prune(point_top,ys,0.0,crop_height,&top,&scaled_height);
    if(scaled_width<=0.0 || scaled_height<=0.0 || left>=r->fbw || top>=r->fbh ||
       left+scaled_width<=0.0 || top+scaled_height<=0.0) return 1;
  }
  if(xs<0.0) left-=scaled_width;
  if(ys<0.0) top-=scaled_height;
  if(!isfinite(left) || !isfinite(top) ||
     !isfinite(scaled_width) || !isfinite(scaled_height) ||
     floor(left)!=left || floor(top)!=top ||
     fabs(xs)>INT_MAX || fabs(ys)>INT_MAX ||
     (!classic_point && !fractional_text && !gui_half &&
      (xs!=(double)(int)xs || ys!=(double)(int)ys))) return 0;
  if(integer_sprite && (left>=r->fbw || top>=r->fbh ||
     left+scaled_width<=0.0 || top+scaled_height<=0.0)) return 1;
  /* R03-BG-UNIT-CLIPPED-NOOP: a trimmed repeat cell can have no physical
   * coverage even while its logical cell intersects the target. Keep the
   * qualified unit-nearest prefix queued instead of handing an empty draw
   * to the CPU. The source, state and whole-texel guards above still apply. */
  if(((family==GML_RENDER_SCENE_BACKGROUND && xs==1.0 && ys==1.0) || integer_tile || classic_tile || integer_text || integer_sprite_part) &&
     page->scene_binary_alpha &&
     (left>=r->fbw || top>=r->fbh ||
      left+scaled_width<=0.0 || top+scaled_height<=0.0)) return 1;
  if(left>=r->fbw || top>=r->fbh) return 0;
  if(!integer_sprite && ((xs<0.0 && (left<0.0 || left+scaled_width>r->fbw)) ||
     (ys<0.0 && (top<0.0 || top+scaled_height>r->fbh)))) return 0;
  /* Unit crops may shorten the source. Qualified positive integer TILE quads
   * instead keep their original UV phase and let the target clip their coverage. */
  if(!integer_sprite && !classic_point && !fractional_text && xs==1.0){
    if(left<0.0){
      if(left<=-scaled_width) return 0;
      int shift=(int)-left;
      crop_x+=shift;
      crop_width-=shift;
      left=0.0;
    }
    if(left+crop_width>r->fbw) crop_width=(int)(r->fbw-left);
    scaled_width=(double)crop_width;
  }
  if(!integer_sprite && !classic_point && !fractional_text && ys==1.0){
    if(top<0.0){
      if(top<=-scaled_height) return 0;
      int shift=(int)-top;
      crop_y+=shift;
      crop_height-=shift;
      top=0.0;
    }
    if(top+crop_height>r->fbh) crop_height=(int)(r->fbh-top);
    scaled_height=(double)crop_height;
  }
  if(crop_width<=0 || crop_height<=0 ||
     (!integer_sprite && !classic_point && !fractional_text && !gui_half && !integer_tile && !integer_text && !integer_sprite_part && (left<0.0 || top<0.0 ||
                        left+scaled_width>r->fbw || top+scaled_height>r->fbh)) ||
     !isfinite(scaled_width) || !isfinite(scaled_height)) return 0;
  /* Clipped enlarged texels are exact on device, but are not an eligible CPU
   * replay packet. Retain their actual pixels through the existing on-demand
   * observation contract, without an ordinary-frame read or new replay leaf. */
  checkpoint_integer_tile=(integer_tile || integer_text) && (left<0.0 || top<0.0 ||
    left+scaled_width>r->fbw || top+scaled_height>r->fbh);
  if(checkpoint_integer_tile && (!r->device.context || !r->device.observe_on_demand ||
     !r->device.observe_on_demand(r->device.context))) return 0;
  gml_render_flush_pending_fill(r);
  if(r->pending_fill || !(target=gml_render_scene_target_resource_ensure(r))) return 0;
  image=gml_render_application_scene_next_image_ref(r,target);
  if(image.content_version==UINT64_MAX) return 0;
  region=(GmlRenderRect){page->sx+crop_x,page->sy+crop_y,
                         (uint32_t)crop_width,(uint32_t)crop_height};
  source_ref=source.image;
  operation.opcode=GML_RENDER_OP_TEXTURED_QUAD;
  operation.reconstruction_mode=integer_sprite || source_override || classic_tile || integer_text || integer_sprite_part || fractional_text || checkpoint_integer_tile || classic_point || replace || checkpoint_alpha_background || checkpoint_alpha_text ||
    checkpoint_alpha_sprite || checkpoint_alpha_gui ||
    colour!=0xFFFFFFu || draw_alpha<1.0
    ?GML_RENDER_DEVICE_CHECKPOINT:GML_RENDER_REPLAY_EXACT;
  operation.scene_family=family;
  operation.source_count=1u;
  operation.sources=&source_ref;
  operation.target.image=image;
  operation.target.image.content_version++;
  operation.target.storage_version=1u;
  operation.target.width=(uint32_t)r->fbw;
  operation.target.height=(uint32_t)r->fbh;
  operation.target.pixel_format=GML_RENDER_PIXEL_ARGB8888;
  operation.target.region=(GmlRenderRect){0,0,(uint32_t)r->fbw,(uint32_t)r->fbh};
  operation.payload.quad.shape=GML_RENDER_QUAD_AXIS_ALIGNED;
  operation.payload.quad.sampling.filter=replace
    ?GML_RENDER_FILTER_NEAREST_PHASED_COPY:GML_RENDER_FILTER_NEAREST;
  operation.payload.quad.sampling.edge_rule=GML_RENDER_EDGE_CLAMP_SUBIMAGE;
  operation.payload.quad.sampling.source_region=region;
  if(integer_sprite){
    operation.payload.quad.sampling.quantisation=anygm_policy_has_modern_layer_semantics(r->win)
      ?GML_RENDER_QUANTISE_INTEGER_TINT_ROUND:GML_RENDER_QUANTISE_INTEGER_TINT_TRUNC;
    operation.payload.quad.filtered_draw_alpha=draw_alpha;
  }
  if(checkpoint_alpha_gui) operation.payload.quad.filtered_draw_alpha=draw_alpha;
  /* GML colours are BBGGRR; neutral quad colours are AARRGGBB. */
  uint32_t rgba=((colour&0xFFu)<<16)|(colour&0xFF00u)|
                ((colour>>16)&0xFFu);
  rgba|=((uint32_t)lround(draw_alpha*255.0))<<24;
  for(int corner=0;corner<4;corner++) operation.payload.quad.corner_colour[corner]=rgba;
  operation.payload.quad.x[0]=operation.payload.quad.x[3]=left;
  operation.payload.quad.x[1]=operation.payload.quad.x[2]=left+scaled_width;
  operation.payload.quad.y[0]=operation.payload.quad.y[1]=top;
  operation.payload.quad.y[2]=operation.payload.quad.y[3]=top+scaled_height;
  source_left=(double)region.x/source.width;
  source_right=((double)region.x+region.width)/source.width;
  source_top=(double)region.y/source.height;
  source_bottom=((double)region.y+region.height)/source.height;
  if(gui_half){
    source_left+=0.5/source.width;source_right+=0.5/source.width;
    source_top+=0.5/source.height;source_bottom+=0.5/source.height;
  }
  operation.payload.quad.u[0]=operation.payload.quad.u[3]=xs<0.0?source_right:source_left;
  operation.payload.quad.u[1]=operation.payload.quad.u[2]=xs<0.0?source_left:source_right;
  operation.payload.quad.v[0]=operation.payload.quad.v[1]=ys<0.0?source_bottom:source_top;
  operation.payload.quad.v[2]=operation.payload.quad.v[3]=ys<0.0?source_top:source_bottom;
  state.write_mask=15u;
  state.colour=0xFFFFFFFFu;
  state.alpha=255u;
  if(!replace && (!integer_sprite || r->alphablend) &&
     (!page->scene_fully_opaque || draw_alpha<1.0 || checkpoint_alpha_gui)){
    state.rgb_source_factor=GML_RENDER_BLEND_SOURCE_ALPHA;
    state.rgb_destination_factor=GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA;
    state.alpha_source_factor=(integer_sprite && gml_render_target_preserves_alpha(r)) || checkpoint_alpha_background ||
      (checkpoint_alpha_sprite && gml_render_target_preserves_alpha(r)) ||
      (family==GML_RENDER_SCENE_TEXT && draw_alpha<1.0 &&
       gml_render_target_preserves_alpha(r))
      ?GML_RENDER_BLEND_SOURCE_ALPHA:GML_RENDER_BLEND_ONE;
    state.alpha_destination_factor=GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA;
    state.rgb_equation=GML_RENDER_BLEND_ADD;
    state.alpha_equation=GML_RENDER_BLEND_ADD;
  }
  state.sampler_count=1u;
  if(checkpoint_alpha_sprite && r->classic && !gml_render_target_preserves_alpha(r))
    state.coverage_policy=GML_RENDER_COVERAGE_OPAQUE_TARGET;
  if(checkpoint_alpha_gui)
    state.coverage_policy=GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA;
  if(integer_sprite && r->alphablend && !gml_render_target_preserves_alpha(r) &&
     (draw_alpha<1.0 || !page->scene_binary_alpha))
    state.coverage_policy=GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA;
  state.samplers[0]=operation.payload.quad.sampling;
  if(classic_point || fractional_text){
    if(!resident_modern_point_regions(r,page,point_left,point_top,xs,ys,0.0,0.0,
                                     &operation,&state,&source,NULL,0u)) return 0;
  } else if(!gml_render_application_scene_enqueue(r,&operation,&state,&source)) return 0;
  r->fb_all_transparent=0;
  if(((family==GML_RENDER_SCENE_TEXT && draw_alpha<1.0) || checkpoint_alpha_sprite ||
      (integer_sprite && r->alphablend && (draw_alpha<1.0 || !page->scene_binary_alpha))) &&
     gml_render_target_preserves_alpha(r)){
    r->fb_opaque_known=0;
    r->fb_all_opaque=0;
    if(r->app_surface && r->fb==r->app_surface) r->app_surface_opaque=0;
  }
  /* Enqueue conservatively invalidates application opacity. A complete nearest
   * replacement from an opaque page proves every target alpha without reading it. */
  if((replace || page->scene_fully_opaque) && draw_alpha==1.0 && left==0.0 && top==0.0 &&
     scaled_width==r->fbw && scaled_height==r->fbh){
    r->fb_opaque_known=r->fb_all_opaque=1;
    if(r->app_surface && r->fb==r->app_surface) r->app_surface_opaque=1;
  }
  return 1;
}

/* R03-SPR-RUNTIME-BINARY-UNIT: the existing crop packet consumes the actual
 * immutable-between-mutations frame, never its stale packaged texture page. */
static int resident_runtime_unit_sprite(GmlRender *r,GmlSprite *sprite,int subimg,
                                        double x,double y,double xs,double ys,double rotation,
                                        uint32_t colour,double alpha){
  if(!r || !r->classic || !r->resident_application_pass || !sprite ||
     !sprite->runtime_rgba || !sprite->runtime_scene_binary ||
     sprite->spine || sprite->ns_enabled || r->interp || r->gui_pass_active ||
     r->fog_flat || gml_d3_is_active(r) || r->active_shader>=0 ||
     r->alpha_test_enable || !r->alphablend || r->blendmode ||
     r->blend_equation!=1 || r->blend_equation_alpha!=1 || r->color_write_mask!=15u ||
     r->classic_phase_y || r->rotated_batch_count || r->rotated_batch_building ||
     r->pending_underlay || r->pending_presentation ||
     (r->world_transform_active && (r->world_scale_x!=1.0 || r->world_scale_y!=1.0)) ||
     !gml_render_scene_target_matches(r) || xs!=1.0 || ys!=1.0 || rotation!=0.0 ||
     (colour&0xFFFFFFu)!=0xFFFFFFu || alpha!=1.0 ||
     !isfinite(x) || !isfinite(y) || fabs(x)>65536.0 || fabs(y)>65536.0 ||
     !isfinite(r->cam_x) || !isfinite(r->cam_y) ||
     !r->device.context || !r->device.observe_on_demand ||
     !r->device.observe_on_demand(r->device.context) ||
     sprite->w<=0 || sprite->h<=0 || sprite->w>4096 || sprite->h>4096 ||
     sprite->n_frames<=0 || sprite->n_frames>4096) return 0;
  for(unsigned index=0;index<3u;index++)
    if(r->classic_interp_phase[index] || r->app_interp_phase[index]) return 0;
  double left=(x-r->cam_x)-sprite->originx,top=(y-r->cam_y)-sprite->originy;
  if(!isfinite(left) || !isfinite(top) || fabs(left)>65536.0 || fabs(top)>65536.0 ||
     floor(left)!=left || floor(top)!=top) return 0;
  if(left>=r->fbw || top>=r->fbh || left+sprite->w<=0.0 || top+sprite->h<=0.0)
    return 1;
  GmlRenderResourceSource source={0};
  if(!gml_render_runtime_sprite_scene_source_view(r,sprite,subimg,&source)) return 0;
  GmlTpag page={.atlas=-1,.sw=sprite->w,.sh=sprite->h,.tw=sprite->w,.th=sprite->h,
    .bw=sprite->w,.bh=sprite->h,.scene_opaque_checked=1,.scene_binary_alpha=1,
    .scene_fully_opaque=sprite->runtime_opaque};
  return resident_atlas_crop_from_page(r,&page,0,0,page.sw,page.sh,left,top,1.0,1.0,
    GML_RENDER_SCENE_SPRITE,0xFFFFFFu,1.0,4,&source);
}

/* R03-SPR-CLASSIC-ORTHO: active fixed-function state need not imply depth work.
 * Keep the existing camera/trim producer and the ordinary opaque nearest batch. */
static int resident_depth_free_ortho_sprite(GmlRender *r,int sprite,int subimg,
                                            double x,double y,double xs,double ys,
                                            double rotation,uint32_t colour,double alpha){
  if(!r || !r->classic || !gml_d3_is_active(r) || r->gui_pass_active ||
     r->world_transform_active || xs!=1.0 || ys!=1.0 || rotation!=0.0 ||
     colour!=0xFFFFFFu || alpha!=1.0 || sprite<0 || sprite>=r->n_spr ||
     sprite>0xFFFF || subimg<0 || subimg>0x3FF) return 0;
  GmlSprite *s=&r->spr[sprite];
  if(s->runtime_rgba || s->spine || s->ns_enabled || s->n_frames<=0 ||
     s->w<=0 || s->h<=0 || !s->frame) return 0;
  int page_index=s->frame[gml_render_runtime_frame_index(s,subimg)];
  if(page_index<0 || page_index>=r->n_tpag) return 0;
  GmlTpag *page=&r->tpag[page_index];
  if(page->atlas<0 || page->atlas>=r->n_atlas || page->sw<=0 || page->sh<=0 ||
     page->sx<0 || page->sy<0 || page->tx<0 || page->ty<0 ||
     page->tw!=page->sw || page->th!=page->sh || page->bw!=s->w || page->bh!=s->h ||
     (uint64_t)(uint32_t)page->tx+(uint32_t)page->sw>(uint32_t)s->w ||
     (uint64_t)(uint32_t)page->ty+(uint32_t)page->sh>(uint32_t)s->h) return 0;
  GmlAtlas *atlas=&r->atlas[page->atlas];
  if(!atlas->px || (uint64_t)(uint32_t)page->sx+(uint32_t)page->sw>(uint32_t)atlas->w ||
     (uint64_t)(uint32_t)page->sy+(uint32_t)page->sh>(uint32_t)atlas->h) return 0;
  resident_classify_page(page,atlas);
  double rectangle[4];
  if(!page->scene_fully_opaque ||
     !gml_software3d_project_sprite_axis(r,sprite,subimg,x,y,rectangle) ||
     !gml_software3d_prepare_resident_sprite(r)) return 0;
  return resident_atlas_crop_from_page(r,page,0,0,page->sw,page->sh,
    rectangle[0],rectangle[1],rectangle[2],rectangle[3],
    GML_RENDER_SCENE_SPRITE,colour,alpha,2,NULL);
}

int gml_render_resident_atlas_crop(GmlRender *r,int page_index,
                                  int crop_x,int crop_y,int crop_width,int crop_height,
                                  double left,double top,double xs,double ys,
                                  GmlRenderSceneFamily family){
  if(!r || page_index<0 || page_index>=r->n_tpag) return 0;
  GmlTpag *page=&r->tpag[page_index];
  /* R03-BG-CLASSIC-SCALED-NEAREST: full untrimmed point backgrounds share
   * the existing sprite phase/coverage producer, not the filtered shader. */
  int classic_scaled=r->classic && family==GML_RENDER_SCENE_BACKGROUND &&
    !page->tx && !page->ty && page->bw==page->sw && page->bh==page->sh &&
    crop_x==0 && crop_y==0 && crop_width==page->sw && crop_height==page->sh &&
    xs>0.0 && xs<=4.0 && ys>0.0 && ys<=4.0 && (xs!=1.0 || ys!=1.0);
  return resident_atlas_crop_from_page(r,&r->tpag[page_index],
    crop_x,crop_y,crop_width,crop_height,left,top,xs,ys,family,0xFFFFFFu,1.0,
    classic_scaled?3:0,NULL);
}

/* R03-TEXT-CLASSIC-ORTHO: font layout stays with the text owner; the existing
 * CPU atlas-part vertices and camera produce the final whole-texel rectangle. */
int gml_render_resident_ortho_font_glyph(GmlRender *r,int atlas,int source_x,int source_y,
                                         int width,int height,double x,double y){
  if(!r || !r->classic || !gml_d3_is_active(r) || r->gui_pass_active ||
     r->world_transform_active || atlas<0 || atlas>=r->n_atlas ||
     r->atlas[atlas].runtime_font_page || width<=0 || height<=0) return 0;
  double rectangle[4];
  if(!gml_software3d_project_atlas_part_axis(r,width,height,x,y,rectangle) ||
     !gml_software3d_prepare_resident_sprite(r)) return 0;
  GmlTpag page={.atlas=atlas,.sx=source_x,.sy=source_y,.sw=width,.sh=height,
                .tw=width,.th=height,.bw=width,.bh=height};
  return resident_atlas_crop_from_page(r,&page,0,0,width,height,
    rectangle[0],rectangle[1],rectangle[2],rectangle[3],
    GML_RENDER_SCENE_TEXT,0xFFFFFFu,1,2,NULL);
}

int gml_render_resident_font_glyph(GmlRender *r,int atlas,int source_x,int source_y,
                                   int width,int height,double left,double top,
                                   double xs,double ys,uint32_t colour,double alpha){
  GmlTpag page={.atlas=atlas,.sx=source_x,.sy=source_y,.sw=width,.sh=height,
                .tw=width,.th=height,.bw=width,.bh=height};
  return resident_atlas_crop_from_page(r,&page,0,0,width,height,left,top,xs,ys,
                                       GML_RENDER_SCENE_TEXT,colour,alpha,0,NULL);
}

int gml_render_resident_atlas_page(GmlRender *r,int page_index,
                                  double left,double top,double xs,double ys,
                                  GmlRenderSceneFamily family){
  if(!r || page_index<0 || page_index>=r->n_tpag) return 0;
  GmlTpag *page=&r->tpag[page_index];
  return gml_render_resident_atlas_crop(r,page_index,0,0,page->sw,page->sh,
                                        left,top,xs,ys,family);
}

/* Axis draws retain the first-generation filtered CPU blit's snapped coverage and fractional
 * texture phase. Rotated sprites carry their CPU inverse map with policy-specific sampling. */
/* Nearest mode 1 is the established snapped sprite rule; mode 2 is the
 * first-generation fractional-origin background-part rule. */
/* R03-SPR-MODERN-NEAREST: a double-precision CPU sample just below an integral
 * texel boundary must not be promoted by the shader's float point-phase tie.
 * New SCREEN/application rules resolve conflicting directions into bounded
 * disjoint geometry; other rules keep their existing CPU exclusion. */
static int modern_nearest_axis_bias(double leading,double snapped,double scale,
                                    double phase_offset,double extent,int source_extent,
                                    int ordinary_interval,double *bias){
  int near_below=0,near_at_or_above=0;
  /* Reflections consume positive loop indices from anchor-1, not a
   * fractional leading edge. Pruning may remove indices at either end. */
  double phase=scale<0.0?floor(leading+0.5)-(snapped+extent)+0.5:
                         snapped+0.5-leading+phase_offset;
  double magnitude=fabs(scale);
  if(extent>4096.0 || source_extent>4096) return 0;
  for(int pixel=0;pixel<(int)extent;pixel++){
    double sample=((double)pixel+phase)/magnitude;
    double edge=nearbyint(sample);
    if(!isfinite(sample) || sample<0.0 || sample>=source_extent) return 0;
    if(fabs(sample-edge)<(ordinary_interval?0.01:0.00001)){
      if(sample<edge) near_below=1;
      else near_at_or_above=1;
    }
  }
  if(near_below && near_at_or_above) return 0;
  /* Wide visible strips need a margin larger than normalised float UV error.
   * Every other boundary is at least .01 away, so .001 cannot change its texel.
   * Conflicting directions still require disjoint constant-centre strips. */
  *bias=ordinary_interval?(near_below?0.001:near_at_or_above?-0.001:0.0):
    near_below?0.00001:0.0;
  return 1;
}

/* Use the point writer's original loop index and phase even when clipping
 * removes a rounded edge. Reflections iterate from anchor-1 toward the left. */
static void modern_point_axis_prune(double leading,double scale,double phase_offset,int source_extent,
                                    double *start,double *extent){
  int anchor=(int)floor(leading+0.5),reflected=scale<0.0;
  double phase=reflected?0.5:anchor+0.5-leading+phase_offset;
  int first=0,last=(int)*extent;
  while(first<last){
    int pixel=reflected?anchor-1-(int)*start-first:(int)*start+first-anchor;
    int sample=gml_render_point_axis_index(pixel,phase,fabs(scale));
    if(sample>=0 && sample<source_extent) break;
    first++;
  }
  while(last>first){
    int pixel=reflected?anchor-1-(int)*start-(last-1):(int)*start+(last-1)-anchor;
    int sample=gml_render_point_axis_index(pixel,phase,fabs(scale));
    if(sample>=0 && sample<source_extent) break;
    last--;
  }
  *start+=first;
  *extent=last-first;
}

typedef struct ModernPointRegion { double start,end; int texel; } ModernPointRegion;
enum { POINT_AXIS_REGION_LIMIT=512 };

/* Only near-boundary samples need a constant texel-centre strip. Ordinary intervals retain
 * hardware point interpolation. This is bounded geometry preparation, not float emulation
 * in the shader or a destination-dependent draw. Clip before collecting visible strips. */
static uint32_t modern_point_axis_regions(double leading,double scale,double phase_offset,int source_extent,
  double start,double end,int constant_texels,ModernPointRegion regions[POINT_AXIS_REGION_LIMIT]){
  int anchor=(int)floor(leading+0.5);
  double phase=scale<0.0?0.5:anchor+0.5-leading+phase_offset;
  uint32_t count=0;
  for(int coordinate=(int)start;coordinate<(int)end;coordinate++){
    int pixel=scale<0.0?anchor-1-coordinate:coordinate-anchor;
    double sample=((double)pixel+phase)/fabs(scale);
    if(!isfinite(sample) || sample<0.0 || sample>=source_extent) return 0;
    int texel=constant_texels || fabs(sample-nearbyint(sample))<0.01?(int)floor(sample):-1;
    if(count && regions[count-1u].texel==texel) regions[count-1u].end=coordinate+1.0;
    else {
      if(count==POINT_AXIS_REGION_LIMIT) return 0;
      regions[count++]=(ModernPointRegion){coordinate,coordinate+1.0,texel};
    }
  }
  return count;
}

/* The existing large-axis CPU kernel selects a fixed-point inverse map, not the small
 * point writer's fractional leading edge. Reuse its prepared rows as geometry only. */
static uint32_t modern_mapped_axis_regions(const GmlRenderRotatedCoverageRegion *maps,
  uint32_t rows,int horizontal,int source_extent,ModernPointRegion regions[POINT_AXIS_REGION_LIMIT]){
  uint32_t count=0,length=horizontal?maps[0].region.width:rows;
  int start=horizontal?maps[0].region.x:maps[0].region.y;
  if(length>4096u) return 0;
  for(uint32_t pixel=0;pixel<length;pixel++){
    int64_t fixed=horizontal?maps[0].fixed_x+maps[0].delta_x*(int64_t)pixel:maps[pixel].fixed_y;
    double sample=(double)fixed/1048576.0;
    if(!isfinite(sample) || sample<0.0 || sample>=source_extent) return 0;
    int texel=fabs(sample-nearbyint(sample))<0.01?(int)floor(sample):-1;
    double coordinate=(double)start+pixel;
    if(count && regions[count-1u].texel==texel) regions[count-1u].end=coordinate+1.0;
    else {
      if(count==POINT_AXIS_REGION_LIMIT) return 0;
      regions[count++]=(ModernPointRegion){coordinate,coordinate+1.0,texel};
    }
  }
  return count;
}

static int resident_modern_point_regions(GmlRender *r,GmlTpag *page,
  double left,double top,double xs,double ys,double phase_x,double phase_y,const GmlRenderDrawOp *operation,
  const GmlRenderDrawStateSnapshot *state,const GmlRenderResourceSource *source,
  const GmlRenderRotatedCoverageRegion *maps,uint32_t map_count){
  ModernPointRegion columns[POINT_AXIS_REGION_LIMIT],rows[POINT_AXIS_REGION_LIMIT];
  GmlRenderRotatedCoverageRegion basis={0};
  const GmlRenderQuad *original=&operation->payload.quad;
  uint32_t nx,ny;
  double bias_x=0.0,bias_y=0.0;
  if(maps){
    if(!map_count) return 0;
    for(uint32_t row=0;row<map_count;row++)
      if(maps[row].region.x!=maps[0].region.x || maps[row].region.width!=maps[0].region.width ||
         maps[row].region.y!=(int64_t)maps[0].region.y+row || maps[row].region.height!=1u ||
         maps[row].fixed_x!=maps[0].fixed_x || maps[row].delta_x!=maps[0].delta_x ||
         maps[row].delta_y!=0) return 0;
    /* Prepared rows include the CPU geometry's padded border. Drop only samples outside
     * the physical crop, as the original kernel does before reading any source bytes. */
    while(map_count && (maps[0].fixed_y<0 ||
      maps[0].fixed_y>=(int64_t)page->sh*1048576)){ maps++;map_count--; }
    while(map_count && (maps[map_count-1u].fixed_y<0 ||
      maps[map_count-1u].fixed_y>=(int64_t)page->sh*1048576)) map_count--;
    if(!map_count) return 1;
    basis=maps[0];
    uint32_t first=0,last=basis.region.width;
    while(first<last){
      int64_t sample=basis.fixed_x+basis.delta_x*(int64_t)first;
      if(sample>=0 && sample<(int64_t)page->sw*1048576) break;
      first++;
    }
    while(last>first){
      int64_t sample=basis.fixed_x+basis.delta_x*(int64_t)(last-1u);
      if(sample>=0 && sample<(int64_t)page->sw*1048576) break;
      last--;
    }
    if(first==last) return 1;
    basis.region.x+=(int)first;basis.region.width=last-first;
    basis.fixed_x+=basis.delta_x*(int64_t)first;
    nx=modern_mapped_axis_regions(&basis,1u,1,page->sw,columns);
    ny=modern_mapped_axis_regions(maps,map_count,0,page->sh,rows);
  } else {
    double x0=fmax(0.0,original->x[0]),x1=fmin(r->fbw,original->x[1]);
    double y0=fmax(0.0,original->y[0]),y1=fmin(r->fbh,original->y[2]);
    /* A conflicting axis must not split the other, already representable axis.
     * Preserve its one-quad phase correction instead of a Cartesian strip explosion. */
    double anchor_x=xs<0.0?floor(left+0.5):left-phase_x;
    double anchor_y=ys<0.0?floor(top+0.5):top-phase_y;
    int stable_x=modern_nearest_axis_bias(left,x0,xs,phase_x,x1-x0,page->sw,1,&bias_x);
    int stable_y=modern_nearest_axis_bias(top,y0,ys,phase_y,y1-y0,page->sh,1,&bias_y);
    double u0=page->sx+(x0-anchor_x)/xs-bias_x,u1=page->sx+(x1-anchor_x)/xs-bias_x;
    double v0=page->sy+(y0-anchor_y)/ys-bias_y,v1=page->sy+(y1-anchor_y)/ys-bias_y;
    int constant_x=fmin(u0,u1)<0.0 || fmax(u0,u1)>source->width;
    int constant_y=fmin(v0,v1)<0.0 || fmax(v0,v1)>source->height;
    /* Direct hardware nearest must keep every vertex inside its physical
     * crop. Constant-centre regions fence a phase correction at that edge;
     * the existing phased shader policies keep their original atlas bounds. */
    if(original->sampling.filter==GML_RENDER_FILTER_NEAREST){
      constant_x|=fmin(u0,u1)<page->sx || fmax(u0,u1)>page->sx+page->sw;
      constant_y|=fmin(v0,v1)<page->sy || fmax(v0,v1)>page->sy+page->sh;
    }
    if(stable_x && !constant_x){
      nx=1u;columns[0]=(ModernPointRegion){x0,x1,-1};
    } else nx=modern_point_axis_regions(left,xs,phase_x,page->sw,x0,x1,constant_x,columns);
    if(stable_y && !constant_y){
      ny=1u;rows[0]=(ModernPointRegion){y0,y1,-1};
    } else ny=modern_point_axis_regions(top,ys,phase_y,page->sh,y0,y1,constant_y,rows);
  }
  uint32_t count=nx*ny;
  if(!count || count>2048u || !source->borrowed_immutable ||
     operation->target.image.content_version>UINT64_MAX-(count-1u)) return 0;
  GmlRenderDrawOp *operations=malloc((size_t)count*sizeof *operations);
  if(!operations) return 0;
  double anchor_x=xs<0.0?floor(left+0.5):left-phase_x;
  double anchor_y=ys<0.0?floor(top+0.5):top-phase_y;
  int accepted=0;
  for(uint32_t y=0,index=0;y<ny;y++) for(uint32_t x=0;x<nx;x++,index++){
    operations[index]=*operation;
    operations[index].target.image.content_version+=index;
    GmlRenderQuad *quad=&operations[index].payload.quad;
    quad->x[0]=quad->x[3]=columns[x].start;quad->x[1]=quad->x[2]=columns[x].end;
    quad->y[0]=quad->y[1]=rows[y].start;quad->y[2]=quad->y[3]=rows[y].end;
    for(unsigned corner=0;corner<4u;corner++){
      double sx=columns[x].texel<0?(quad->x[corner]-anchor_x)/xs-bias_x:columns[x].texel+0.5;
      double sy=rows[y].texel<0?(quad->y[corner]-anchor_y)/ys-bias_y:rows[y].texel+0.5;
      if(maps){
        if(columns[x].texel<0) sx=(double)basis.fixed_x/1048576.0+
          (quad->x[corner]-basis.region.x-0.5)*(double)basis.delta_x/1048576.0;
        if(rows[y].texel<0) sy=(double)maps[0].fixed_y/1048576.0+
          (quad->y[corner]-maps[0].region.y-0.5)/ys;
      }
      quad->u[corner]=(page->sx+sx)/source->width;
      quad->v[corner]=(page->sy+sy)/source->height;
      if(!isfinite(quad->u[corner]) || !isfinite(quad->v[corner]) ||
         quad->u[corner]<0.0 || quad->u[corner]>1.0 ||
         quad->v[corner]<0.0 || quad->v[corner]>1.0) goto done;
    }
  }
  accepted=gml_render_application_scene_enqueue_quads(r,operations,count,state,source);
done:
  free(operations);
  return accepted;
}

static int resident_rotated_packet_regions(GmlRender *r,GmlSprite *sprite,GmlTpag *page,
                                            double left,double top,double xs,double ys,
                                            double rotation,const GmlRenderDrawOp *operation,
                                            const GmlRenderDrawStateSnapshot *state,
                                            const GmlRenderResourceSource *source){
  enum { REGION_LIMIT=8192 };
  GmlRenderRotatedCoverageRegion *regions=(GmlRenderRotatedCoverageRegion*)
    malloc(REGION_LIMIT*sizeof *regions);
  GmlRenderDrawOp *operations=NULL;
  GmlRenderDrawStateSnapshot states[2]={*state,*state};
  uint32_t count=0;
  int accepted=0;
  if(!regions || !gml_render_rotated_nearest_regions(r,sprite,page,left+r->cam_x,
       top+r->cam_y,xs,ys,rotation,regions,REGION_LIMIT,&count)) goto done;
  if(!count){ accepted=1; goto done; }
  operations=(GmlRenderDrawOp*)malloc((size_t)count*sizeof *operations);
  if(!operations || operation->target.image.content_version>UINT64_MAX-(count-1u)) goto done;
  states[1].coverage_policy=GML_RENDER_COVERAGE_ROTATED_VECTOR_ALPHA;
  states[1].rgb_source_factor=GML_RENDER_BLEND_SOURCE_ALPHA;
  states[1].rgb_destination_factor=GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA;
  states[1].alpha_source_factor=GML_RENDER_BLEND_ONE;
  states[1].alpha_destination_factor=GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA;
  states[1].rgb_equation=states[1].alpha_equation=GML_RENDER_BLEND_ADD;
  for(uint32_t index=0;index<count;index++){
    const GmlRenderRect *region=&regions[index].region;
    operations[index]=*operation;
    operations[index].state_index=regions[index].vector_alpha;
    operations[index].target.image.content_version+=index;
    GmlRenderQuad *quad=&operations[index].payload.quad;
    quad->x[0]=quad->x[3]=(double)region->x;
    quad->x[1]=quad->x[2]=(double)region->x+region->width;
    quad->y[0]=quad->y[1]=(double)region->y;
    quad->y[2]=quad->y[3]=(double)region->y+region->height;
  }
  GmlRenderDrawBatch batch={operations,count,states,2u};
  GmlRenderResourceExecution result=gml_render_application_scene_submit(r,&batch,source,1u);
  if(result==GML_RENDER_RESOURCE_EXEC_CPU) r->resident_application_pass=0;
  accepted=result!=GML_RENDER_RESOURCE_EXEC_FAILED;
done:
  free(operations);
  free(regions);
  return accepted;
}

static int resident_filtered_atlas_page(GmlRender *r,int page_index,
                                      double left,double top,double xs,double ys,
                                      GmlRenderSceneFamily family,uint32_t colour,double alpha,
                                      GmlTpag *part_view,GmlSprite *rotated_sprite,
                                      double rotation,
                                      const uint32_t corner_colours[4],
                                      int opaque_target_alpha,
                                      int transparent_stretched_trim,
                                      int box_stretched,int positioned_axis,
                                      int nearest_mode){
  if(r && r->pending_underlay && !gml_render_compose_resident_underlay(r)) return 0;
  GmlTpag *page;
  GmlAtlas *atlas;
  GmlRenderResourceSource source={0};
  GmlRenderResource *target;
  GmlRenderDrawOp operation={0};
  GmlRenderDrawStateSnapshot state={0};
  GmlRenderImageRef source_ref,image;
  GmlRenderRect region;
  double x0,y0,width,height,cosine=1.0,sine=0.0;
  double nearest_bias_x=0.0,nearest_bias_y=0.0;
  int split_point=0;
  uint32_t nearest_contiguous_x=0u,nearest_contiguous_y=0u;
  double nearest_anchor_x=left,nearest_anchor_y=top;
  double positioned_x_tie=0.0,positioned_y_tie=0.0;
  double span_x[4]={0},span_y[4]={0};
  int modern_rotation=rotated_sprite && r && r->win &&
    anygm_policy_has_modern_layer_semantics(r->win);
  int additive_background=r && r->blendmode==1 &&
    family==GML_RENDER_SCENE_BACKGROUND && nearest_mode==5 &&
    !r->interp && fabs(xs)==1.0 && fabs(ys)==1.0 &&
    (colour&0xFFFFFFu)==0xFFFFFFu && alpha>0.0 && alpha<=1.0;
  /* R03-P9-FIXED-BACKGROUND: retain the CPU leaf's source quantisation policy
   * while hardware weights ordered destinations without a per-quad snapshot. */
  int hardware_background=r && r->interp && !r->blendmode && alpha==1.0 &&
    !rotated_sprite && !corner_colours && !opaque_target_alpha &&
    !transparent_stretched_trim && !box_stretched && !positioned_axis &&
    !nearest_mode && !r->gui_pass_active && part_view &&
    family==GML_RENDER_SCENE_BACKGROUND &&
    r->device.context && r->device.observe_on_demand &&
    r->device.observe_on_demand(r->device.context);
  /* R03-P9-FILTERED-REAL-TEXT: a demanded real glyph is a physical atlas crop,
   * not a TPAG entry. It uses the same filtered source/phase preparation as a sprite. */
  int filtered_real_text=family==GML_RENDER_SCENE_TEXT && page_index==-1 &&
    part_view && r && r->interp && !rotated_sprite && !corner_colours &&
    xs>0.0 && ys>0.0 && xs<=1.0 && ys<=1.0 &&
    part_view->sw<=256 && part_view->sh<=256;
  int mapped_gui_font=filtered_real_text && resident_mapped_screen_gui_font(r);
  /* R03-P9-GUI-FILTERED-ATLAS: an identity physical-screen GUI axis draw uses
   * the same resolved CPU sampling coordinates as the existing world rule.
   * Mapped sprites below retain their separate admission and sampling rules. */
  int identity_gui_atlas=r && resident_identity_screen_gui(r) && r->interp &&
    !rotated_sprite && !corner_colours && !opaque_target_alpha &&
    !transparent_stretched_trim && !box_stretched && !positioned_axis &&
    !filtered_real_text && !nearest_mode;
  /* R03-P9-GUI-MAPPED-SPRITE: shared sprite entries already map their point/scale or stretched extent.
   * Reuse the owned mapped-screen predicate; sampling and trim keep their
   * existing CPU-rule branches below rather than mapping the quad twice. */
  int mapped_gui_sprite=family==GML_RENDER_SCENE_SPRITE && r && r->interp &&
    resident_mapped_screen_gui_font(r) && !rotated_sprite && !corner_colours &&
    !positioned_axis && !filtered_real_text && !nearest_mode;
  /* R03-P9-GUI-ROTATED-SPRITE: first-generation rotation remains nearest,
   * including when interpolation is enabled. The resolved point/scale is
   * already in SCREEN coordinates; only destination coverage differs from
   * the existing application-target rotation. */
  int mapped_gui_rotation=family==GML_RENDER_SCENE_SPRITE && r && r->interp &&
    resident_mapped_screen_gui_font(r) && rotated_sprite && !corner_colours &&
    !positioned_axis && !filtered_real_text && !nearest_mode && r->win &&
    anygm_policy_uses_first_generation_studio(r->win);
  int span_active=0;
  int stretched_float=0;
  int stretched_replay_exact=0;
  int stretched_magnification=family==GML_RENDER_SCENE_SPRITE && opaque_target_alpha &&
    !rotated_sprite && !corner_colours && !positioned_axis && !nearest_mode &&
    !box_stretched && xs>1.0 && ys>1.0;
  int exact_filtered_font=0;
  int exact_filtered_sprite=0;
  int modern_numbered_sprite=r && r->target_id>0 && r->color_write_mask<=15u &&
    family==GML_RENDER_SCENE_SPRITE && (nearest_mode==3 || nearest_mode==6) &&
    !r->interp && !r->fog_flat && !r->gui_pass_active &&
    anygm_policy_has_modern_layer_semantics(r->win) &&
    gml_render_target_preserves_alpha(r);
  int masked_numbered_sprite=modern_numbered_sprite && r->color_write_mask<15u;
  /* R03-SPR-MODERN-SCREEN-POINT: fixed source-over keeps signed point
   * sampling resident without the numbered target's predecessor recipe. */
  int modern_screen_sprite=r && r->target_id<0 && r->target_sp==0 &&
    family==GML_RENDER_SCENE_SPRITE && (nearest_mode==3 || nearest_mode==6) &&
    !r->interp && !r->fog_flat && !r->gui_pass_active &&
    anygm_policy_has_modern_layer_semantics(r->win) &&
    !gml_render_target_preserves_alpha(r) &&
    r->device.context && r->device.observe_on_demand &&
    r->device.observe_on_demand(r->device.context);
  /* R03-SPR-MODERN-SCREEN-FILTERED: ordinary modern axis sprites keep
   * floating source colour through hardware filtering and fixed source-over.
   * Packed trim and reflected sampling remain separate qualification slices. */
  int modern_screen_filtered=r && r->target_id<0 && r->target_sp==0 &&
    family==GML_RENDER_SCENE_SPRITE && r->interp &&
    !rotated_sprite && !corner_colours && !part_view && !opaque_target_alpha &&
    !transparent_stretched_trim && !box_stretched && !positioned_axis && !nearest_mode &&
    !r->fog_flat && xs>0.0 && ys>0.0 && alpha>0.0 &&
    anygm_policy_has_modern_layer_semantics(r->win) &&
    !gml_render_target_preserves_alpha(r) &&
    r->device.context && r->device.observe_on_demand &&
    r->device.observe_on_demand(r->device.context);
  /* A maximised GUI can still resolve to the same identity mapping. Admit
   * that flag-only variant, not arbitrary maximised scales or offsets. */
  int modern_filtered_identity_maximise=r && r->gui_pass_active &&
    r->gui_maximise_active && !r->world_transform_active && r->base_scene_resource &&
    r->fb==r->base_scene_resource_pixels &&
    r->fbw==r->base_scene_resource_width && r->fbh==r->base_scene_resource_height &&
    r->gui_base_logical_w==r->fbw && r->gui_base_logical_h==r->fbh &&
    r->gui_logical_w==r->fbw && r->gui_logical_h==r->fbh &&
    r->gui_scale_x==1.0 && r->gui_scale_y==1.0 &&
    r->gui_maximise_xoffset==0.0 && r->gui_maximise_yoffset==0.0 &&
    r->cam_x==0.0 && r->cam_y==0.0;
  /* R03-TEXT-MODERN-MAXIMISED-FILTERED: this previously excluded identity
   * GUI glyph slice shares hardware filtering and batched alpha coverage.
   * Existing world and non-maximised GUI exact font recipes stay unchanged. */
  int modern_screen_filtered_text=filtered_real_text && modern_filtered_identity_maximise &&
    r->target_id<0 && r->target_sp==0 && !r->fog_flat && alpha>=0.0 &&
    anygm_policy_has_modern_layer_semantics(r->win) &&
    !gml_render_target_preserves_alpha(r) && r->device.context &&
    r->device.observe_on_demand && r->device.observe_on_demand(r->device.context);
  int hardware_screen_filtered=modern_screen_filtered || modern_screen_filtered_text;
  modern_filtered_identity_maximise=modern_filtered_identity_maximise && hardware_screen_filtered;
  /* R03-SPR-FIRSTGEN-SIGNED-POINT: retain first-generation alpha and edge coverage.
   * Contiguous-white specialisations keep their separate established recipes. */
  int firstgen_signed_point=r && nearest_mode==4 &&
    family==GML_RENDER_SCENE_SPRITE && !rotated_sprite && !corner_colours && !part_view &&
    gml_render_target_is_first_generation_application_surface(r) &&
    (xs<0.0 || ys<0.0 || ((xs>4.0 || ys>4.0) &&
     fabs(xs-nearbyint(xs))>1e-9 && fabs(ys-nearbyint(ys))>1e-9)) &&
    r->device.context && r->device.observe_on_demand &&
    r->device.observe_on_demand(r->device.context);
  double point_phase_x=firstgen_signed_point && xs>0.0 &&
    fabs(1.0/xs-nearbyint(1.0/xs))<1e-9?left-floor(left+0.5):
    firstgen_signed_point && xs>1.0 && fabs(xs-nearbyint(xs))<1e-9?0.5-1e-9:0.0;
  double point_phase_y=firstgen_signed_point && ys>0.0 &&
    fabs(1.0/ys-nearbyint(1.0/ys))<1e-9?top-floor(top+0.5):
    firstgen_signed_point && ys>1.0 && fabs(ys-nearbyint(ys))<1e-9?0.5-1e-9:0.0;
  int nearest_preserved_alpha=r && (nearest_mode==3 || nearest_mode==6) &&
    gml_render_target_preserves_alpha(r);
  int margin_left=0,margin_top=0,margin_right=0,margin_bottom=0;
  uint32_t margin_mask=0u;
  if(!r || !r->resident_application_pass ||
     (!r->interp && !box_stretched && !nearest_mode) || !r->win ||
     !(anygm_policy_uses_first_generation_studio(r->win) ||
       (filtered_real_text && anygm_policy_has_modern_layer_semantics(r->win)) ||
       ((nearest_mode==3 || nearest_mode==6) &&
        anygm_policy_has_modern_layer_semantics(r->win)) ||
       ((rotated_sprite || opaque_target_alpha || modern_screen_filtered) &&
        anygm_policy_has_modern_layer_semantics(r->win))) ||
     (!filtered_real_text && (page_index<0 || page_index>=r->n_tpag)) || r->classic ||
     (r->gui_pass_active && !identity_gui_atlas && !mapped_gui_font &&
      !mapped_gui_sprite && !mapped_gui_rotation && !modern_filtered_identity_maximise) ||
     r->active_shader>=0 || r->alpha_test_enable ||
     !r->alphablend || (r->blendmode && !additive_background) ||
     r->blend_equation!=1 ||
     r->blend_equation_alpha!=1 ||
     (r->color_write_mask!=0x0fu && !masked_numbered_sprite) ||
     (rotated_sprite && anygm_policy_has_modern_layer_semantics(r->win) &&
      (!r->fb_opaque_known || !r->fb_all_opaque)) ||
     r->classic_phase_y || r->rotated_batch_count || r->rotated_batch_building ||
     r->pending_underlay || r->pending_presentation ||
     !gml_render_scene_target_matches(r) ||
     !isfinite(left) || !isfinite(top) || !isfinite(xs) || !isfinite(ys) ||
     (rotated_sprite && !isfinite(rotation)) ||
     !isfinite(alpha) || alpha<0.0 || alpha>1.0 ||
     (opaque_target_alpha && !filtered_real_text && !stretched_magnification &&
      (!r->fb_opaque_known || !r->fb_all_opaque)) ||
     ((transparent_stretched_trim || box_stretched) &&
      (family!=GML_RENDER_SCENE_SPRITE || rotated_sprite || xs<=0.0 || ys<=0.0)) ||
     xs==0.0 || ys==0.0 || fabs(xs)>1000000.0 || fabs(ys)>1000000.0 ||
     (nearest_mode==1 && (r->interp || family!=GML_RENDER_SCENE_SPRITE ||
       rotated_sprite || corner_colours || fabs(xs)!=1.0 || fabs(ys)!=1.0 ||
       (part_view && (part_view->sw<=1 || part_view->sh<=1)) ||
       floor(left)!=left || floor(top)!=top)) ||
     (nearest_mode==3 && (r->interp || family!=GML_RENDER_SCENE_SPRITE ||
       rotated_sprite || corner_colours || part_view ||
       !r->fb_opaque_known || !r->fb_all_opaque ||
       (!modern_numbered_sprite && ((!modern_screen_sprite &&
       (xs<=0.0 || ys<=0.0 || xs>1.0 || ys>1.0)) ||
       fabs(fabs(r->cam_x-nearbyint(r->cam_x))-0.5)<1e-9 ||
       fabs(fabs(r->cam_y-nearbyint(r->cam_y))-0.5)<1e-9)))) ||
     (nearest_mode==6 && (r->interp || family!=GML_RENDER_SCENE_SPRITE ||
       rotated_sprite || corner_colours || part_view ||
       (!modern_numbered_sprite && ((!modern_screen_sprite &&
       (xs<=0.0 || ys<=0.0 || xs>1.0 || ys>1.0)) ||
       fabs(fabs(r->cam_x-nearbyint(r->cam_x))-0.5)<1e-9 ||
       fabs(fabs(r->cam_y-nearbyint(r->cam_y))-0.5)<1e-9)))) ||
     (nearest_mode==4 && (r->interp || family!=GML_RENDER_SCENE_SPRITE ||
       rotated_sprite || corner_colours || part_view ||
       (!firstgen_signed_point && (xs<=0.0 || ys<=0.0 || xs>4.0 || ys>4.0)) ||
       ((xs>1.0 || ys>1.0) && (!r->device.context ||
        !r->device.observe_on_demand || !r->device.observe_on_demand(r->device.context))) ||
       (!firstgen_signed_point &&
        ((xs!=1.0 && fabs(1.0/xs-nearbyint(1.0/xs))<1e-9) ||
         (ys!=1.0 && fabs(1.0/ys-nearbyint(1.0/ys))<1e-9))))) ||
     (nearest_mode==5 && (r->interp || family!=GML_RENDER_SCENE_BACKGROUND ||
       rotated_sprite || corner_colours || !part_view ||
       ((!additive_background) && (xs!=1.0 || ys!=1.0)) ||
       (alpha!=1.0 && !additive_background))) ||
     (nearest_mode==2 && (r->interp || family!=GML_RENDER_SCENE_BACKGROUND ||
       !part_view || rotated_sprite || corner_colours || xs==0.0 || ys==0.0 ||
       fabs(xs)>4.0 || fabs(ys)>4.0 ||
       (fabs(xs)!=1.0 && (fabs(1.0/xs-nearbyint(1.0/xs))<1e-9 ||
                          fabs(xs-nearbyint(xs))<1e-9)) ||
       (fabs(ys)!=1.0 && (fabs(1.0/ys-nearbyint(1.0/ys))<1e-9 ||
                          fabs(ys-nearbyint(ys))<1e-9))))) return 0;
  for(int index=0;index<3;index++)
    if(r->classic_interp_phase[index] || r->app_interp_phase[index]) return 0;
  if(masked_numbered_sprite && !r->color_write_mask) return 1;
  /* blit_one skips every destination at zero draw alpha. Preserve the queued
   * prefix without a write, shader call or source snapshot in this ordinary state. */
  if(alpha==0.0) return 1;
  if(part_view && !filtered_real_text &&
     part_view->atlas!=r->tpag[page_index].atlas) return 0;
  page=part_view?part_view:&r->tpag[page_index];
  if((modern_numbered_sprite || modern_screen_sprite) && page->project_authored_edges) return 0;
  if(modern_screen_sprite && alpha<1.0/255.0) return 0;
  if(firstgen_signed_point && (alpha<1.0/255.0 || page->project_authored_edges)) return 0;
  /* Ordinary axis CPU sampling uses physical sw/sh, not stored tw/th. Keep
   * the qualified nearest rules and modern SCREEN hardware filtering on that
   * physical crop; other filtered families retain their layout guard. */
  if(page->sw<=0 || page->sh<=0 || page->sx<0 || page->sy<0 ||
     ((page->tw!=page->sw || page->th!=page->sh) &&
      nearest_mode!=1 && nearest_mode!=3 && nearest_mode!=4 && nearest_mode!=6 &&
      !modern_screen_filtered) ||
     page->atlas<0 || page->atlas>=r->n_atlas) return 0;
  if(modern_screen_filtered && page->project_authored_edges) return 0;
  /* blit_one can select its near-unit white exact-copy leaf instead of
   * filtering. Do not approximate that separate sampling specialisation. */
  if(modern_screen_filtered && alpha==1.0 && (colour&0xFFFFFFu)==0xFFFFFFu &&
     fabs(xs-1.0)<0.001 && fabs(ys-1.0)<0.001 && (xs!=1.0 || ys!=1.0)) return 0;
  if(family==GML_RENDER_SCENE_SPRITE ||
     ((family==GML_RENDER_SCENE_BACKGROUND || family==GML_RENDER_SCENE_TILE) &&
      part_view)){
    if((page->tx<0 || page->ty<0) && !(corner_colours && part_view)) return 0;
    if(page->bw<=0 || page->bh<=0 ||
       (int64_t)page->tx+page->sw>page->bw ||
       (int64_t)page->ty+page->sh>page->bh) return 0;
    margin_left=page->tx>0;
    margin_top=page->ty>0;
    margin_right=(int64_t)page->tx+page->sw<page->bw;
    margin_bottom=(int64_t)page->ty+page->sh<page->bh;
  } else if(page->tx || page->ty || page->bw!=page->sw || page->bh!=page->sh)
    return 0;
  atlas=&r->atlas[page->atlas];
  if(!gml_render_atlas_scene_source_view(r,page->atlas,&source) ||
     (uint64_t)(uint32_t)page->sx+(uint32_t)page->sw>source.width ||
     (uint64_t)(uint32_t)page->sy+(uint32_t)page->sh>source.height) return 0;
  if(hardware_screen_filtered && !source.borrowed_immutable) return 0;
  /* The full logical quad can extend beyond the visible page, but its prepared UVs must remain
   * inside the atlas even though transparent taps themselves never fetch those neighbours. */
  if(transparent_stretched_trim && !box_stretched && !stretched_magnification &&
     ((uint32_t)page->sx<(uint32_t)page->tx ||
      (uint32_t)page->sy<(uint32_t)page->ty ||
      (uint64_t)(uint32_t)page->sx+(uint32_t)page->bw-(uint32_t)page->tx>source.width ||
      (uint64_t)(uint32_t)page->sy+(uint32_t)page->bh-(uint32_t)page->ty>source.height))
    return 0;
  /* The CPU filtered sprite can read one physical atlas texel outside a packed crop. The
   * logical-margin edge rule decides that per tap without widening the visible quad or UVs. */
  if(!transparent_stretched_trim && !box_stretched &&
     ((margin_left && page->sx==0) || (margin_top && page->sy==0) ||
      (margin_right && (uint64_t)(uint32_t)page->sx+(uint32_t)page->sw>=source.width) ||
      (margin_bottom && (uint64_t)(uint32_t)page->sy+(uint32_t)page->sh>=source.height)))
    return 0;
  if(margin_left) margin_mask|=GML_RENDER_MARGIN_LEFT;
  if(margin_top) margin_mask|=GML_RENDER_MARGIN_TOP;
  if(margin_right) margin_mask|=GML_RENDER_MARGIN_RIGHT;
  if(margin_bottom) margin_mask|=GML_RENDER_MARGIN_BOTTOM;
  resident_classify_page(page,atlas);
  /* The established shader truncates tint; modern opaque CPU pixels round it.
   * Do not admit new exact integer variants with non-endpoint tint channels
   * through a tolerance rule. Existing positive unit/reduction paths remain. */
  if(modern_screen_sprite && (xs<0.0 || ys<0.0 || xs>1.0 || ys>1.0) &&
     page->scene_binary_alpha && alpha==1.0 &&
     fabs(xs-nearbyint(xs))<1e-9 && fabs(ys-nearbyint(ys))<1e-9){
    for(unsigned shift=0;shift<24u;shift+=8u){
      unsigned channel=(colour>>shift)&255u;
      if(channel!=0u && channel!=255u) return 0;
    }
  }
  hardware_background=hardware_background && !page->scene_fully_opaque;
  int hardware_combined=hardware_background &&
    gml_render_target_is_first_generation_application_surface(r) &&
    (colour&0xFFFFFFu)==0xFFFFFFu && xs>0.0 && ys>0.0 &&
    fabs(xs-1.0)<0.001 && fabs(ys-1.0)<0.001 &&
    fabs(left-nearbyint(left))<1e-9 && fabs(top-nearbyint(top))<1e-9;
  if(hardware_combined){
    /* The CPU unit-white leaf indexes source texels from the snapped origin,
     * even when its accepted authored scale is only approximately one. */
    left=nearbyint(left); top=nearbyint(top); xs=ys=1.0;
  }
  /* R03-P9-STRETCHED-COVERED-ALPHA: magnification's original CPU band
   * keeps float channels and alpha before blending, including logical trim.
   * Opaque-white pages retain the older exact retained-source recipe. */
  stretched_float=stretched_magnification;
  stretched_replay_exact=stretched_float && !transparent_stretched_trim &&
    page->scene_fully_opaque && (colour&0xFFFFFFu)==0xFFFFFFu && alpha==1.0;
  if(rotated_sprite){
    double minx=1e30,miny=1e30,maxx=-1e30,maxy=-1e30;
    int source_x0=corner_colours && part_view?-page->tx:
      modern_rotation && !corner_colours?page->scene_ax0:0;
    int source_y0=corner_colours && part_view?-page->ty:
      modern_rotation && !corner_colours?page->scene_ay0:0;
    int source_x1=corner_colours && part_view?rotated_sprite->w-page->tx:
      modern_rotation && !corner_colours?page->scene_ax1+1:page->sw;
    int source_y1=corner_colours && part_view?rotated_sprite->h-page->ty:
      modern_rotation && !corner_colours?page->scene_ay1+1:page->sh;
    if(source_x1<=source_x0 || source_y1<=source_y0) return 0;
    render_rotation_sincos(rotation,&cosine,&sine);
    render_modern_cardinal_anchor(r,rotation,xs,ys,cosine,sine,&left,&top);
    /* First-generation nearest rotation advances a fixed-point coordinate from the beginning
     * of each CPU row. At an exactly centred cardinal texel edge, that rounded increment can
     * select the preceding texel while a GPU's independent inverse map selects the next one.
     * Keep only these boundary-coincident partial draws on the established CPU path. */
    if(part_view && !modern_rotation &&
       fabs(rotation-nearbyint(rotation/90.0)*90.0)<1e-10){
      if(page->sw>256 || page->sh>256) return 0;
      for(int axis=0;axis<2;axis++){
        int extent=axis?page->sh:page->sw;
        for(int edge=0;edge<=extent;edge++){
          double source_axis=(double)edge+(axis?page->ty:page->tx)-
                             (axis?rotated_sprite->originy:rotated_sprite->originx);
          double screen_axis=axis
            ? (fabs(sine)>0.5?left+source_axis*ys*sine:top+source_axis*ys*cosine)
            : (fabs(cosine)>0.5?left+source_axis*xs*cosine:top-source_axis*xs*sine);
          double screen_extent=(axis ? fabs(sine)>0.5 : fabs(cosine)>0.5)
            ? (double)r->fbw : (double)r->fbh;
          if(screen_axis>=-1.0 && screen_axis<=screen_extent+1.0 &&
             fabs(screen_axis-nearbyint(screen_axis-0.5)-0.5)<0.00002)
            return 0;
        }
      }
    }
    for(int corner=0;corner<4;corner++){
      double sx=page->tx+((corner==1 || corner==2)?source_x1:source_x0);
      double sy=page->ty+((corner>=2)?source_y1:source_y0);
      double px=(sx-rotated_sprite->originx)*xs;
      double py=(sy-rotated_sprite->originy)*ys;
      double dx=left+px*cosine+py*sine;
      double dy=top-px*sine+py*cosine;
      span_x[corner]=dx; span_y[corner]=dy;
      minx=fmin(minx,dx); maxx=fmax(maxx,dx);
      miny=fmin(miny,dy); maxy=fmax(maxy,dy);
    }
    x0=fmax(0.0,floor(minx)-1.0);
    y0=fmax(0.0,floor(miny)-1.0);
    width=fmin((double)r->fbw,ceil(maxx)+1.0)-x0;
    height=fmin((double)r->fbh,ceil(maxy)+1.0)-y0;
  } else if(transparent_stretched_trim || box_stretched || stretched_float){
    x0=left; y0=top;
    width=box_stretched || stretched_float
      ?nearbyint((double)page->bw*xs):(double)page->bw*xs;
    height=box_stretched || stretched_float
      ?nearbyint((double)page->bh*ys):(double)page->bh*ys;
  } else {
    /* The positioned axis CPU leaf delegates to blit_one. Its modern world-target half-camera
     * ties select the preceding pixel; other filtered quads keep their established rounding. */
    if((positioned_axis || modern_screen_filtered) && anygm_policy_has_modern_layer_semantics(r->win) &&
       !r->gui_pass_active && r->target_sp==0 && r->target_id<0){
      if(fabs(fabs(r->cam_x-nearbyint(r->cam_x))-0.5)<1e-9)
        positioned_x_tie=-1e-9;
      if(fabs(fabs(r->cam_y-nearbyint(r->cam_y))-0.5)<1e-9)
        positioned_y_tie=-1e-9;
    }
    x0=floor(left+0.5+positioned_x_tie);
    y0=floor(top+0.5+positioned_y_tie);
    /* blit_one rounds screen extents independently unless the authored tile
     * requests edge projection; application-surface coverage keeps its old rule. */
    width=(identity_gui_atlas || mapped_gui_sprite || modern_screen_filtered ||
      (hardware_background && !gml_render_target_is_first_generation_application_surface(r))) &&
      !page->project_authored_edges
      ?floor((double)page->sw*fabs(xs)+0.5)
      :filtered_real_text && !gml_render_target_is_first_generation_application_surface(r)
      ?floor((double)page->sw*fabs(xs)+0.5)
      :(positioned_axis || filtered_real_text || nearest_mode==3 || nearest_mode==6) &&
      anygm_policy_has_modern_layer_semantics(r->win)
      ?floor((double)page->sw*fabs(xs)+0.5)
      :xs<0.0
      ? (fabs(xs-nearbyint(xs))>1e-9
         ? x0-floor(left+(double)page->sw*xs+0.5)
         : floor((double)page->sw*-xs+0.5))
      : fabs(xs-nearbyint(xs))>1e-9
        ? floor(left+(double)page->sw*xs+0.5)-x0
        : floor((double)page->sw*xs+0.5);
    height=(identity_gui_atlas || mapped_gui_sprite || modern_screen_filtered ||
      (hardware_background && !gml_render_target_is_first_generation_application_surface(r))) &&
      !page->project_authored_edges
      ?floor((double)page->sh*fabs(ys)+0.5)
      :filtered_real_text && !gml_render_target_is_first_generation_application_surface(r)
      ?floor((double)page->sh*fabs(ys)+0.5)
      :(positioned_axis || filtered_real_text || nearest_mode==3 || nearest_mode==6) &&
      anygm_policy_has_modern_layer_semantics(r->win)
      ?floor((double)page->sh*fabs(ys)+0.5)
      :ys<0.0
      ? (fabs(ys-nearbyint(ys))>1e-9
         ? y0-floor(top+(double)page->sh*ys+0.5)
         : floor((double)page->sh*-ys+0.5))
      : fabs(ys-nearbyint(ys))>1e-9
        ? floor(top+(double)page->sh*ys+0.5)-y0
        : floor((double)page->sh*ys+0.5);
    if(xs<0.0) x0-=width;
    if(ys<0.0) y0-=height;
  }
  if((modern_screen_sprite || hardware_screen_filtered || firstgen_signed_point) &&
     isfinite(width) && isfinite(height) && (width==0.0 || height==0.0)) return 1;
  if(!isfinite(width) || !isfinite(height) || width<1.0 || height<1.0 ||
     x0+width>10000000.0 || y0+height>10000000.0) return 0;
  /* Larger CPU rotations can select their separate 8-bit/culling kernel.
   * This rule retains the existing 16-bit nearest coverage, not that policy. */
  if(mapped_gui_rotation && width*height>=262144.0) return 0;
  if(x0<=-width || y0<=-height || x0>=r->fbw || y0>=r->fbh){
    /* Filtered background parts on first-generation application surfaces share
     * this projected half-open coverage with their CPU leaf at fractional scales.
     * Unit-sign parts retain their qualified coverage on other targets too. */
    /* The qualified numbered point leaf cannot expand this rounded coverage.
     * A wholly clipped sprite is a no-op before its later sample-edge pruning. */
    /* R03-SPR-GUI-FILTERED-OFFSCREEN-NOOP: these axis GUI slices already use
     * blit_one's independently rounded half-open SCREEN rectangle above.
     * A fully clipped sprite writes nothing, even with partial source alpha. */
    if((family==GML_RENDER_SCENE_SPRITE && (identity_gui_atlas || mapped_gui_sprite) &&
        !part_view && !opaque_target_alpha && !transparent_stretched_trim && !box_stretched &&
        anygm_policy_uses_first_generation_studio(r->win)) ||
       modern_numbered_sprite || modern_screen_sprite || hardware_screen_filtered || firstgen_signed_point ||
       (nearest_mode==4 && !rotated_sprite && xs==1.0 && ys==1.0) ||
       additive_background ||
       (family==GML_RENDER_SCENE_BACKGROUND && part_view && !rotated_sprite &&
        !nearest_mode && ((fabs(xs)==1.0 && fabs(ys)==1.0) ||
          gml_render_target_is_first_generation_application_surface(r)))) return 1;
    return 0;
  }
  if(!page->scene_fully_opaque && !hardware_background && family!=GML_RENDER_SCENE_SPRITE &&
     !filtered_real_text && !identity_gui_atlas &&
     nearest_mode!=2 && nearest_mode!=5) return 0;
  if(stretched_float){
    if(width>1000000.0 || height>1000000.0 || page->bw>16384 || page->bh>16384 ||
       floor(x0)!=x0 || floor(y0)!=y0 || alpha<0x1p-64) return 0;
    uint32_t first_x=(uint32_t)fmax(0.0,-x0),first_y=(uint32_t)fmax(0.0,-y0);
    uint32_t last_x=(uint32_t)fmin(width,(double)r->fbw-x0);
    uint32_t last_y=(uint32_t)fmin(height,(double)r->fbh-y0);
    if(!gml_render_stretched_float_visible_phase_exact((uint32_t)page->bw,(uint32_t)width,
                                               first_x,last_x-first_x) ||
       !gml_render_stretched_float_visible_phase_exact((uint32_t)page->bh,(uint32_t)height,
                                               first_y,last_y-first_y)) return 0;
    stretched_replay_exact=stretched_replay_exact && width<=16384.0 && height<=16384.0 &&
      gml_render_stretched_float_phase_exact((uint32_t)page->bw,(uint32_t)width) &&
      gml_render_stretched_float_phase_exact((uint32_t)page->bh,(uint32_t)height);
  }
  if(nearest_mode==4 && !firstgen_signed_point && (xs>1.0 || ys>1.0)){
    if(page->project_authored_edges || width>4096.0 || height>4096.0) return 0;
    /* The white/full-alpha point leaf can consume one contiguous physical
     * atlas texel at its far edge. Other draws leave that sample unwritten. */
    double phase_x=xs==1.0?0.5:x0+0.5-left;
    double phase_y=ys==1.0?0.5:y0+0.5-top;
    if(xs>1.0 && fabs(xs-nearbyint(xs))<1e-9) phase_x+=0.5-1e-9;
    if(ys>1.0 && fabs(ys-nearbyint(ys))<1e-9) phase_y+=0.5-1e-9;
    int last_x=gml_render_point_axis_index((int)width-1,phase_x,xs);
    int last_y=gml_render_point_axis_index((int)height-1,phase_y,ys);
    int white_point=alpha==1.0 && (colour&0xFFFFFFu)==0xFFFFFFu && !r->fog_flat;
    nearest_contiguous_x=white_point && xs!=1.0 && last_x==page->sw &&
      (uint64_t)(uint32_t)page->sx+(uint32_t)page->sw<source.width;
    nearest_contiguous_y=white_point && ys!=1.0 && last_y==page->sh &&
      (uint64_t)(uint32_t)page->sy+(uint32_t)page->sh<source.height;
    if(last_x>=page->sw && !nearest_contiguous_x) width-=1.0;
    if(last_y>=page->sh && !nearest_contiguous_y) height-=1.0;
    if(width==0.0 || height==0.0) return 1;
  }
  if(modern_numbered_sprite || modern_screen_sprite || firstgen_signed_point){
    /* Both point leaves leave out-of-crop rounded edge samples unwritten.
     * The numbered executor prepares indices; SCREEN retains the bounded
     * single-axis phase guard and fixed source-over below. */
    if(width>4096.0 || height>4096.0 || page->sw>4096 || page->sh>4096) return 0;
    if(firstgen_signed_point && alpha==1.0 && (colour&0xFFFFFFu)==0xFFFFFFu){
      int anchor_x=(int)floor(left+0.5),anchor_y=(int)floor(top+0.5);
      if((xs>0.0 && gml_render_point_axis_index((int)(x0+width)-1-anchor_x,
           anchor_x+0.5-left+point_phase_x,xs)>=page->sw) ||
         (ys>0.0 && gml_render_point_axis_index((int)(y0+height)-1-anchor_y,
           anchor_y+0.5-top+point_phase_y,ys)>=page->sh)) return 0;
    }
    modern_point_axis_prune(left,xs,point_phase_x,page->sw,&x0,&width);
    modern_point_axis_prune(top,ys,point_phase_y,page->sh,&y0,&height);
    if(width==0.0 || height==0.0 || x0+width<=0.0 || y0+height<=0.0 ||
       x0>=r->fbw || y0>=r->fbh) return 1;
  }
  if(!modern_numbered_sprite && (nearest_mode==3 || nearest_mode==6 || firstgen_signed_point) &&
     (!modern_nearest_axis_bias(left,x0,xs,point_phase_x,width,page->sw,0,&nearest_bias_x) ||
      !modern_nearest_axis_bias(top,y0,ys,point_phase_y,height,page->sh,0,&nearest_bias_y))){
    if(!modern_screen_sprite && !firstgen_signed_point) return 0;
    split_point=1;
  }
  if(rotated_sprite){
    double qarea=fabs(span_x[0]*span_y[1]-span_x[1]*span_y[0] +
                      span_x[1]*span_y[2]-span_x[2]*span_y[1] +
                      span_x[2]*span_y[3]-span_x[3]*span_y[2] +
                      span_x[3]*span_y[0]-span_x[0]*span_y[3])*0.5;
    span_active=qarea>0.0 && qarea<width*height*0.85;
  }
  gml_render_flush_pending_fill(r);
  if(r->pending_fill || !(target=gml_render_scene_target_resource_ensure(r))) return 0;
  image=gml_render_application_scene_next_image_ref(r,target);
  if(image.content_version==UINT64_MAX) return 0;
  region=(GmlRenderRect){page->sx,page->sy,(uint32_t)page->sw+nearest_contiguous_x,
                       (uint32_t)page->sh+nearest_contiguous_y};
  source_ref=source.image;
  operation.opcode=GML_RENDER_OP_TEXTURED_QUAD;
  operation.reconstruction_mode=additive_background || stretched_replay_exact
    ?GML_RENDER_REPLAY_EXACT:GML_RENDER_DEVICE_CHECKPOINT;
  operation.scene_family=family;
  operation.source_count=1u;
  operation.sources=&source_ref;
  operation.target.image=image;
  operation.target.image.content_version++;
  operation.target.storage_version=1u;
  operation.target.width=(uint32_t)r->fbw;
  operation.target.height=(uint32_t)r->fbh;
  operation.target.pixel_format=GML_RENDER_PIXEL_ARGB8888;
  operation.target.region=(GmlRenderRect){0,0,(uint32_t)r->fbw,(uint32_t)r->fbh};
  operation.payload.quad.shape=rotated_sprite
    ?(fabs(rotation-nearbyint(rotation/90.0)*90.0)<1e-10
      ?GML_RENDER_QUAD_CARDINAL:GML_RENDER_QUAD_GENERAL)
    :GML_RENDER_QUAD_AXIS_ALIGNED;
  operation.payload.quad.sampling.filter=hardware_background || hardware_screen_filtered?GML_RENDER_FILTER_HARDWARE_BILINEAR:
    box_stretched?GML_RENDER_FILTER_BOX:
    nearest_mode==5?(additive_background?GML_RENDER_FILTER_NEAREST:
                     GML_RENDER_FILTER_NEAREST_PHASED_COPY):
    nearest_mode==6 && !nearest_preserved_alpha?GML_RENDER_FILTER_NEAREST_PHASED_OPAQUE_ALPHA:
    (nearest_mode==2 || nearest_mode==3 || nearest_mode==4 || nearest_mode==5 || nearest_mode==6)
      ?GML_RENDER_FILTER_NEAREST_PHASED:
    rotated_sprite && !anygm_policy_has_modern_layer_semantics(r->win) &&
    !(corner_colours && fabs(fmod(rotation,360.0))<0.001)
      ?GML_RENDER_FILTER_NEAREST:GML_RENDER_FILTER_BILINEAR;
  operation.payload.quad.sampling.edge_rule=transparent_stretched_trim || box_stretched
    ?GML_RENDER_EDGE_TRANSPARENT
    :(margin_mask && (operation.payload.quad.sampling.filter==GML_RENDER_FILTER_BILINEAR ||
                     hardware_background || modern_screen_filtered)
      ?GML_RENDER_EDGE_LOGICAL_TPAG:GML_RENDER_EDGE_CLAMP_SUBIMAGE);
  operation.payload.quad.sampling.source_region=region;
  operation.payload.quad.sampling.quantisation=stretched_float
    ?GML_RENDER_QUANTISE_STRETCHED_FLOAT:
    hardware_combined?GML_RENDER_QUANTISE_HARDWARE_COMBINED:
    modern_numbered_sprite?GML_RENDER_QUANTISE_SPRITE_MODERN_NEAREST:0u;
  if(modern_numbered_sprite) operation.payload.quad.filtered_draw_alpha=alpha;
  operation.payload.quad.sampling.logical_margin_mask=box_stretched?0u:
    (operation.payload.quad.sampling.filter==GML_RENDER_FILTER_BILINEAR ||
     hardware_background || modern_screen_filtered)
      ?(corner_colours && part_view && margin_mask
          ?GML_RENDER_MARGIN_VISIBLE_CENTRE:margin_mask):0u;
  uint32_t draw_alpha=additive_background?255u:(uint32_t)lround(alpha*255.0);
  /* GML's blend is BBGGRR; each neutral quad vertex carries AARRGGBB. */
  for(int corner=0;corner<4;corner++){
    uint32_t tint=corner_colours?corner_colours[corner]:colour;
    uint32_t rgba=((tint&0xFFu)<<16)|(tint&0xFF00u)|((tint>>16)&0xFFu);
    operation.payload.quad.corner_colour[corner]=(draw_alpha<<24)|rgba;
  }
  operation.payload.quad.x[0]=operation.payload.quad.x[3]=x0;
  operation.payload.quad.x[1]=operation.payload.quad.x[2]=x0+width;
  operation.payload.quad.y[0]=operation.payload.quad.y[1]=y0;
  operation.payload.quad.y[2]=operation.payload.quad.y[3]=y0+height;
  if(modern_numbered_sprite){
    GmlRenderQuad *quad=&operation.payload.quad;
    quad->inverse_anchor[0]=floor(left+0.5);
    quad->inverse_anchor[1]=floor(top+0.5);
    quad->inverse_x[0]=xs;
    quad->inverse_y[1]=ys;
    quad->inverse_x[1]=xs<0.0?0.5:quad->inverse_anchor[0]+0.5-left;
    quad->inverse_y[0]=ys<0.0?0.5:quad->inverse_anchor[1]+0.5-top;
  }
  if(rotated_sprite){
    GmlRenderQuad *quad=&operation.payload.quad;
    if(corner_colours){
      for(int corner=0;corner<4;corner++){
        quad->x[corner]=span_x[corner];
        quad->y[corner]=span_y[corner];
      }
    }
    quad->inverse_anchor[0]=left;
    quad->inverse_anchor[1]=top;
    quad->inverse_x[0]=cosine/xs;
    quad->inverse_x[1]=sine/ys;
    quad->inverse_y[0]=-sine/xs;
    quad->inverse_y[1]=cosine/ys;
    double sample_bias=quad->sampling.filter==GML_RENDER_FILTER_BILINEAR?-0.5:0.0;
    quad->inverse_offset[0]=(double)rotated_sprite->originx-page->tx+sample_bias;
    quad->inverse_offset[1]=(double)rotated_sprite->originy-page->ty+sample_bias;
    if(span_active && !corner_colours){
      for(int corner=0;corner<4;corner++){
        quad->span_x[corner]=span_x[corner];
        quad->span_y[corner]=span_y[corner];
      }
      quad->span_active=1u;
    }
  } else if(box_stretched || stretched_float){
    GmlRenderQuad *quad=&operation.payload.quad;
    quad->u[0]=quad->u[3]=(double)page->sx/source.width;
    quad->u[1]=quad->u[2]=((double)page->sx+page->sw)/source.width;
    quad->v[0]=quad->v[1]=(double)page->sy/source.height;
    quad->v[2]=quad->v[3]=((double)page->sy+page->sh)/source.height;
    /* Axis-aligned stretched quads carry the exact integer logical source,
     * destination and trim dimensions used by the established CPU leaf. */
    quad->span_x[0]=(double)page->bw;
    quad->span_x[1]=width;
    quad->span_x[2]=(double)page->tx;
    quad->span_x[3]=(double)page->sw;
    quad->span_y[0]=(double)page->bh;
    quad->span_y[1]=height;
    quad->span_y[2]=(double)page->ty;
    quad->span_y[3]=(double)page->sh;
  } else {
    double sample_left=left+(transparent_stretched_trim?(double)page->tx*xs:0.0);
    double sample_top=top+(transparent_stretched_trim?(double)page->ty*ys:0.0);
    if(nearest_mode==4 && !firstgen_signed_point && (xs>1.0 || ys>1.0)){
      if(xs==1.0) sample_left=floor(left+0.5);
      else if(xs>1.0 && fabs(xs-nearbyint(xs))<1e-9){
        sample_left-=0.5;nearest_bias_x=0.00001;
      }
      if(ys==1.0) sample_top=floor(top+0.5);
      else if(ys>1.0 && fabs(ys-nearbyint(ys))<1e-9){
        sample_top-=0.5;nearest_bias_y=0.00001;
      }
    }
    if(modern_numbered_sprite){
      sample_left=nearest_anchor_x; sample_top=nearest_anchor_y;
    }
    if(modern_screen_sprite || firstgen_signed_point){
      if(xs<0.0) sample_left=floor(left+0.5);
      else sample_left-=point_phase_x;
      if(ys<0.0) sample_top=floor(top+0.5);
      else sample_top-=point_phase_y;
    }
    /* The first-generation nearest CPU leaf samples a reflected background part from its
     * snapped half-open anchor, while an unreflected axis retains the fractional draw phase. */
    if(nearest_mode==2 && xs<0.0) sample_left=floor(left+0.5);
    if(nearest_mode==2 && ys<0.0) sample_top=floor(top+0.5);
    /* Unlike a selected part, the CPU's unit-scale complete-page point leaf
     * indexes texels from the snapped destination origin. */
    if(nearest_mode==5){
      sample_left=xs<0.0?x0+width:x0;
      sample_top=ys<0.0?y0+height:y0;
    }
    operation.payload.quad.u[0]=operation.payload.quad.u[3]=
      ((double)page->sx+(x0-sample_left)/xs-nearest_bias_x)/source.width;
    operation.payload.quad.u[1]=operation.payload.quad.u[2]=
      ((double)page->sx+(x0+width-sample_left)/xs-nearest_bias_x)/source.width;
    operation.payload.quad.v[0]=operation.payload.quad.v[1]=
      ((double)page->sy+(y0-sample_top)/ys-nearest_bias_y)/source.height;
    operation.payload.quad.v[2]=operation.payload.quad.v[3]=
      ((double)page->sy+(y0+height-sample_top)/ys-nearest_bias_y)/source.height;
  }
  /* A fractional glyph origin may project the prepared inverse sample outside
   * an atlas whose crop touches its physical edge. Reject before submission so
   * the unchanged CPU sampler handles that draw in order. */
  if(filtered_real_text)
    for(int corner=0;corner<4;corner++)
      if(!isfinite(operation.payload.quad.u[corner]) ||
         !isfinite(operation.payload.quad.v[corner]) ||
         operation.payload.quad.u[corner]<0.0 ||
         operation.payload.quad.u[corner]>1.0 ||
         operation.payload.quad.v[corner]<0.0 ||
         operation.payload.quad.v[corner]>1.0) return 0;
  if(filtered_real_text && !modern_screen_filtered_text){
    GmlRenderQuad *quad=&operation.payload.quad;
    quad->sampling.quantisation=anygm_policy_has_modern_layer_semantics(r->win)
      ?GML_RENDER_QUANTISE_FONT_MODERN
      :gml_render_target_is_first_generation_application_surface(r)
        ?GML_RENDER_QUANTISE_FONT_FIRSTGEN:GML_RENDER_QUANTISE_FONT_FIRSTGEN_INDEPENDENT;
    quad->inverse_anchor[0]=left; quad->inverse_anchor[1]=top;
    quad->inverse_x[0]=xs; quad->inverse_y[1]=ys;
    quad->filtered_draw_alpha=alpha;
    for(unsigned corner=0;corner<4u;corner++)
      quad->corner_colour[corner]|=0xFF000000u;
    exact_filtered_font=gml_render_filtered_font_quad_valid(quad,
      (uint32_t)r->fbw,(uint32_t)r->fbh) && source.borrowed_immutable &&
      !(anygm_policy_has_modern_layer_semantics(r->win) &&
        (fabs(fabs(r->cam_x-nearbyint(r->cam_x))-0.5)<1e-9 ||
         fabs(fabs(r->cam_y-nearbyint(r->cam_y))-0.5)<1e-9));
    /* The new GUI/nonopaque-target admission is only the exact font rule.
     * The older checkpointed approximation remains bounded to its qualified
     * world/opaque-target domain, never substituted for transparent SCREENs. */
    if(!exact_filtered_font && (mapped_gui_font ||
       (opaque_target_alpha && (!r->fb_opaque_known || !r->fb_all_opaque)))) return 0;
    if(exact_filtered_font) operation.reconstruction_mode=GML_RENDER_REPLAY_EXACT;
    else {
      quad->sampling.quantisation=0u;
      for(unsigned corner=0;corner<4u;corner++)
        quad->corner_colour[corner]=(draw_alpha<<24)|(quad->corner_colour[corner]&0xFFFFFFu);
    }
  }
  /* R03-P9-GUI-MAPPED-SPRITE: the ordinary first-generation CPU sampler
   * retains binary64 phase/channel arithmetic, unlike stretched float bands.
   * Reuse the existing exact filtered arithmetic, with actual SPRITE metadata
   * and a checkpoint until this ordinary recipe has its own exact replay. */
  int ordinary_white_copy=xs>0.0 && ys>0.0 && fabs(xs-1.0)<0.001 &&
    fabs(ys-1.0)<0.001 && fabs(left-nearbyint(left))<1e-9 &&
    fabs(top-nearbyint(top))<1e-9 && alpha==1.0 && (colour&0xFFFFFFu)==0xFFFFFFu;
  if(mapped_gui_sprite && !modern_screen_filtered && !opaque_target_alpha && !transparent_stretched_trim &&
     !box_stretched && !page->project_authored_edges && source.borrowed_immutable &&
     !ordinary_white_copy){
    GmlRenderQuad *quad=&operation.payload.quad;
    quad->sampling.quantisation=GML_RENDER_QUANTISE_SPRITE_FIRSTGEN_INDEPENDENT;
    quad->inverse_anchor[0]=left; quad->inverse_anchor[1]=top;
    quad->inverse_x[0]=xs; quad->inverse_y[1]=ys;
    quad->filtered_draw_alpha=alpha;
    for(unsigned corner=0;corner<4u;corner++) quad->corner_colour[corner]|=0xFF000000u;
    exact_filtered_sprite=gml_render_filtered_font_quad_valid(quad,
      (uint32_t)r->fbw,(uint32_t)r->fbh);
    if(!exact_filtered_sprite){
      quad->sampling.quantisation=0u;
      for(unsigned corner=0;corner<4u;corner++)
        quad->corner_colour[corner]=(draw_alpha<<24)|(quad->corner_colour[corner]&0xFFFFFFu);
    }
  }
  state.write_mask=masked_numbered_sprite?r->color_write_mask:15u;
  state.colour=0xFFFFFFFFu;
  state.alpha=255u;
  if((identity_gui_atlas || (mapped_gui_sprite && !opaque_target_alpha) ||
     mapped_gui_rotation || hardware_screen_filtered) && !exact_filtered_sprite){
    state.coverage_policy=GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA;
    operation.payload.quad.filtered_draw_alpha=alpha;
  }
  if(hardware_screen_filtered){
    /* RGB and alpha coverage are independent batched passes: a positive
     * filtered source stamps SCREEN alpha, including on a partial target. */
    operation.payload.quad.inverse_anchor[0]=left;
    operation.payload.quad.inverse_anchor[1]=top;
    operation.payload.quad.inverse_x[0]=xs;
    operation.payload.quad.inverse_y[1]=ys;
    state.rgb_source_factor=GML_RENDER_BLEND_SOURCE_ALPHA;
    state.rgb_destination_factor=GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA;
    state.alpha_source_factor=GML_RENDER_BLEND_ONE;
    state.alpha_destination_factor=GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA;
    state.rgb_equation=state.alpha_equation=GML_RENDER_BLEND_ADD;
  } else if(hardware_background){
    operation.payload.quad.filtered_draw_alpha=alpha;
    /* The unit-white application leaf rounds the combined sum; ordinary
     * filtered coverage first rounds its source term. Both use fixed blending. */
    state.rgb_source_factor=hardware_combined?GML_RENDER_BLEND_SOURCE_ALPHA:GML_RENDER_BLEND_ONE;
    state.rgb_destination_factor=GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA;
    state.alpha_source_factor=GML_RENDER_BLEND_SOURCE_ALPHA;
    state.alpha_destination_factor=GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA;
    state.rgb_equation=state.alpha_equation=GML_RENDER_BLEND_ADD;
  } else if(stretched_float && !stretched_replay_exact){
    operation.payload.quad.filtered_draw_alpha=alpha;
    for(unsigned corner=0;corner<4u;corner++)
      operation.payload.quad.corner_colour[corner]|=0xFF000000u;
    state.rgb_source_factor=GML_RENDER_BLEND_SOURCE_ALPHA;
    state.rgb_destination_factor=GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA;
    state.alpha_source_factor=GML_RENDER_BLEND_ONE;
    state.alpha_destination_factor=GML_RENDER_BLEND_ZERO;
    state.rgb_equation=state.alpha_equation=GML_RENDER_BLEND_EXACT_STRETCHED_FLOAT;
  } else if(exact_filtered_font || exact_filtered_sprite){
    state.rgb_source_factor=GML_RENDER_BLEND_SOURCE_ALPHA;
    state.rgb_destination_factor=GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA;
    state.alpha_source_factor=opaque_target_alpha || exact_filtered_sprite
      ?GML_RENDER_BLEND_ONE:GML_RENDER_BLEND_SOURCE_ALPHA;
    state.alpha_destination_factor=GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA;
    state.rgb_equation=GML_RENDER_BLEND_EXACT_FILTERED_FONT;
    state.alpha_equation=GML_RENDER_BLEND_EXACT_FILTERED_FONT;
  } else if(additive_background){
    state.rgb_source_factor=GML_RENDER_BLEND_SOURCE_ALPHA;
    state.rgb_destination_factor=GML_RENDER_BLEND_ONE;
    state.alpha_source_factor=GML_RENDER_BLEND_ONE;
    state.alpha_destination_factor=GML_RENDER_BLEND_ZERO;
    state.rgb_equation=GML_RENDER_BLEND_EXACT8_ADDITIVE;
    state.alpha_equation=GML_RENDER_BLEND_EXACT8_ADDITIVE;
    state.additive_draw_alpha=alpha;
  } else if(nearest_mode!=5 && (modern_numbered_sprite || modern_screen_sprite || firstgen_signed_point ||
     !page->scene_fully_opaque || alpha<1.0 ||
     nearest_contiguous_x || nearest_contiguous_y ||
     margin_left || margin_top || margin_right || margin_bottom)){
    state.rgb_source_factor=GML_RENDER_BLEND_SOURCE_ALPHA;
    state.rgb_destination_factor=GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA;
    state.alpha_source_factor=nearest_preserved_alpha?GML_RENDER_BLEND_SOURCE_ALPHA:
      opaque_target_alpha || nearest_mode==3 ||
      nearest_mode==6 ||
      (rotated_sprite && anygm_policy_has_modern_layer_semantics(r->win))
        ?GML_RENDER_BLEND_ONE:GML_RENDER_BLEND_SOURCE_ALPHA;
    state.alpha_destination_factor=GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA;
    state.rgb_equation=GML_RENDER_BLEND_ADD;
    state.alpha_equation=GML_RENDER_BLEND_ADD;
  }
  state.sampler_count=1u;
  state.samplers[0]=operation.payload.quad.sampling;
  /* A reduced axis can have valid centre samples while its extrapolated vertices
   * lie outside the atlas. Resolve just that bounded axis to constant texel strips. */
  const GmlRenderQuad *quad=&operation.payload.quad;
  if(quad->shape==GML_RENDER_QUAD_AXIS_ALIGNED) for(unsigned corner=0;corner<4u;corner++){
    if(!isfinite(quad->u[corner]) || !isfinite(quad->v[corner]) ||
       quad->u[corner]<0.0 || quad->u[corner]>1.0 ||
       quad->v[corner]<0.0 || quad->v[corner]>1.0){
      if(!modern_screen_sprite && !firstgen_signed_point) return 0;
      split_point=1;
    }
  }
  if(split_point){
    if(!resident_modern_point_regions(r,page,left,top,xs,ys,point_phase_x,point_phase_y,
                                     &operation,&state,&source,NULL,0u)) return 0;
    r->fb_all_transparent=0;
    return 1;
  }
  if(mapped_gui_rotation && (!r->fb_opaque_known || !r->fb_all_opaque)){
    if(!resident_rotated_packet_regions(r,rotated_sprite,page,left,top,xs,ys,rotation,
                                        &operation,&state,&source)) return 0;
    r->fb_all_transparent=0;
    return 1;
  }
  if(!gml_render_application_scene_enqueue(r,&operation,&state,&source)) return 0;
  r->fb_all_transparent=0;
  return 1;
}

int gml_render_resident_filtered_atlas_page(GmlRender *r,int page_index,
                                           double left,double top,double xs,double ys,
                                           GmlRenderSceneFamily family,uint32_t colour,double alpha,
                                           GmlTpag *part_view,int nearest_rule){
  int mode=nearest_rule==GML_RENDER_NEAREST_PART_PHASE_RULE?2:
    nearest_rule==GML_RENDER_NEAREST_DIRECT_SNAP_RULE?5:0;
  return resident_filtered_atlas_page(r,page_index,left,top,xs,ys,family,colour,alpha,
                                     part_view,NULL,0.0,NULL,0,0,0,0,mode);
}

int gml_render_resident_filtered_font_glyph(GmlRender *r,int atlas,
                                            int source_x,int source_y,int width,int height,
                                            double left,double top,double xs,double ys,
                                            uint32_t colour,double alpha){
  GmlTpag glyph={.atlas=atlas,.sx=source_x,.sy=source_y,.sw=width,.sh=height,
                 .tw=width,.th=height,.bw=width,.bh=height};
  int opaque_target_alpha=!gml_render_target_preserves_alpha(r);
  return resident_filtered_atlas_page(r,-1,left,top,xs,ys,GML_RENDER_SCENE_TEXT,
                                      colour,alpha,&glyph,NULL,0.0,NULL,
                                      opaque_target_alpha,0,0,0,0);
}

/* Shared source-index regions retain the CPU's rotated coverage. Constant
 * texel centres need only hardware nearest; no destination or float replay. */
static int resident_modern_screen_binary_rotation(GmlRender *r,GmlSprite *sprite,GmlTpag *page,
                                                  double x,double y,double xs,double ys,double rotation,
                                                  uint32_t colour,double alpha){
  enum { REGION_LIMIT=2048 };
  GmlRenderResourceSource source={0};
  GmlRenderRotatedCoverageRegion *regions=NULL;
  GmlRenderDrawOp *operations=NULL;
  GmlRenderDrawStateSnapshot state={0};
  GmlRenderResource *target;
  uint32_t count=0;
  int accepted=0;
  const GmlRenderDeviceServices *device=gml_render_device(r);
  if(!device || !device->observe_on_demand || !device->observe_on_demand(device->context) ||
     !r->resident_application_pass || r->target_id>=0 || r->target_sp || r->classic || r->interp ||
     !anygm_policy_has_modern_layer_semantics(r->win) || gml_render_target_preserves_alpha(r) ||
     alpha!=1.0 || (colour&0xFFFFFFu)!=0xFFFFFFu ||
     r->fog_flat || r->gui_pass_active || r->active_shader>=0 || r->alpha_test_enable ||
     !r->alphablend || r->blendmode || r->blend_equation!=1 || r->blend_equation_alpha!=1 ||
     r->color_write_mask!=15u || r->classic_phase_y || r->rotated_batch_count ||
     r->rotated_batch_building || r->pending_underlay || r->pending_presentation ||
     !gml_render_scene_target_matches(r) || !isfinite(rotation) || fabs(rotation)>1000000.0 ||
     page->project_authored_edges || page->sw<=0 || page->sh<=0 || page->sw>4096 || page->sh>4096 ||
     page->sx<0 || page->sy<0 || page->tx<0 || page->ty<0 ||
     (int64_t)page->tx+page->sw>page->bw || (int64_t)page->ty+page->sh>page->bh ||
     !gml_render_atlas_scene_source_view(r,page->atlas,&source) || !source.borrowed_immutable ||
     source.width>16384u || source.height>16384u ||
     (uint64_t)(uint32_t)page->sx+(uint32_t)page->sw>source.width ||
     (uint64_t)(uint32_t)page->sy+(uint32_t)page->sh>source.height) return 0;
  for(int index=0;index<3;index++)
    if(r->classic_interp_phase[index] || r->app_interp_phase[index]) return 0;
  resident_classify_page(page,&r->atlas[page->atlas]);
  if(!page->scene_binary_alpha) return 0;
  regions=malloc(REGION_LIMIT*sizeof *regions);
  if(!regions || !gml_render_rotated_nearest_texel_regions(r,sprite,page,x,y,xs,ys,rotation,
       colour,regions,REGION_LIMIT,&count)) goto done;
  if(!count){ accepted=1;goto done; }
  operations=calloc(count,sizeof *operations);
  if(!operations) goto done;
  gml_render_flush_pending_fill(r);
  if(r->pending_fill || !(target=gml_render_scene_target_resource_ensure(r))) goto done;
  GmlRenderImageRef image=gml_render_application_scene_next_image_ref(r,target);
  if(image.content_version>UINT64_MAX-count) goto done;
  state.write_mask=15u;state.colour=0xFFFFFFFFu;state.alpha=255u;
  /* The shared regions omit alpha-zero texels; the binary certificate makes
   * every remaining texel fully opaque. Endpoint source-over is replacement. */
  state.sampler_count=1u;
  uint32_t tint=((colour&255u)<<16)|(colour&0xFF00u)|((colour>>16)&255u);
  for(uint32_t index=0;index<count;index++){
    const GmlRenderRect *rect=&regions[index].region;
    GmlRenderDrawOp *op=&operations[index];
    op->opcode=GML_RENDER_OP_TEXTURED_QUAD;op->reconstruction_mode=GML_RENDER_DEVICE_CHECKPOINT;
    op->scene_family=GML_RENDER_SCENE_SPRITE;op->source_count=1u;op->sources=&source.image;
    op->target.image=image;op->target.image.content_version+=index+1u;
    op->target.storage_version=1u;op->target.width=(uint32_t)r->fbw;op->target.height=(uint32_t)r->fbh;
    op->target.pixel_format=GML_RENDER_PIXEL_ARGB8888;
    op->target.region=(GmlRenderRect){0,0,(uint32_t)r->fbw,(uint32_t)r->fbh};
    GmlRenderQuad *quad=&op->payload.quad;
    quad->shape=GML_RENDER_QUAD_AXIS_ALIGNED;
    quad->sampling.filter=GML_RENDER_FILTER_NEAREST;
    quad->sampling.edge_rule=GML_RENDER_EDGE_CLAMP_SUBIMAGE;
    quad->sampling.source_region=(GmlRenderRect){page->sx,page->sy,(uint32_t)page->sw,(uint32_t)page->sh};
    quad->x[0]=quad->x[3]=rect->x;quad->x[1]=quad->x[2]=(double)rect->x+rect->width;
    quad->y[0]=quad->y[1]=rect->y;quad->y[2]=quad->y[3]=(double)rect->y+rect->height;
    for(unsigned corner=0;corner<4u;corner++){
      quad->corner_colour[corner]=0xFF000000u|tint;
      quad->u[corner]=((double)page->sx+regions[index].source_x+0.5)/source.width;
      quad->v[corner]=((double)page->sy+regions[index].source_y+0.5)/source.height;
    }
  }
  state.samplers[0]=operations[0].payload.quad.sampling;
  accepted=gml_render_application_scene_enqueue_quads(r,operations,count,&state,&source);
  if(accepted){ r->fb_all_transparent=0;r->fb_opaque_known=0; }
done:
  free(operations);free(regions);return accepted;
}

static int resident_modern_nearest_rotation(GmlRender *r,GmlSprite *sprite,GmlTpag *page,
                                            double x,double y,double xs,double ys,double rotation,
                                            uint32_t colour,double alpha){
  enum { REGION_LIMIT=8192 };
  GmlRenderResourceSource source={0};
  GmlRenderRotatedCoverageRegion *regions=NULL;
  GmlRenderDrawOp *operations=NULL;
  GmlRenderDrawStateSnapshot state={0};
  GmlRenderResource *target;
  uint32_t count=0;
  int accepted=0,rows=0;
  int screen_axis=r && rotation==0.0 && r->target_id<0 && r->target_sp==0 &&
    !gml_render_target_preserves_alpha(r);
  uint32_t capacity=r->fbh>0 && r->fbh<REGION_LIMIT?(uint32_t)r->fbh:REGION_LIMIT;
  const GmlRenderDeviceServices *device=gml_render_device(r);
  if(!device || !device->observe_on_demand || !device->observe_on_demand(device->context))
    return 0;
  if(!r->resident_application_pass || r->interp || (!screen_axis && r->target_id<=0) ||
     (screen_axis && (r->classic || alpha<1.0/255.0 || page->sw>4096 || page->sh>4096)) ||
     !anygm_policy_has_modern_layer_semantics(r->win) ||
     (!screen_axis && !gml_render_target_preserves_alpha(r)) ||
     r->fog_flat || r->gui_pass_active || r->active_shader>=0 || r->alpha_test_enable ||
     !r->alphablend || r->blendmode || r->blend_equation!=1 || r->blend_equation_alpha!=1 ||
     fabs(rotation)>1000000.0 ||
     r->color_write_mask!=15u || r->classic_phase_y || r->rotated_batch_count ||
     r->rotated_batch_building || r->pending_underlay || r->pending_presentation ||
     !gml_render_scene_target_matches(r) || page->project_authored_edges ||
     page->sw<=0 || page->sh<=0 || page->sx<0 || page->sy<0 ||
     page->tx<0 || page->ty<0 || (int64_t)page->tx+page->sw>page->bw ||
     (int64_t)page->ty+page->sh>page->bh ||
     !gml_render_atlas_scene_source_view(r,page->atlas,&source) ||
     source.width>16384u || source.height>16384u ||
     (uint64_t)(uint32_t)page->sx+(uint32_t)page->sw>source.width ||
     (uint64_t)(uint32_t)page->sy+(uint32_t)page->sh>source.height) return 0;
  for(int index=0;index<3;index++)
    if(r->classic_interp_phase[index] || r->app_interp_phase[index]) return 0;
  regions=malloc(capacity*sizeof *regions);
  if(!regions) goto done;
  rows=r->fbw<=16384 && r->fbh<=16384 &&
    gml_render_rotated_nearest_row_regions(r,sprite,page,x,y,xs,ys,rotation,
                                          regions,capacity,&count);
  if(!rows){
    if(screen_axis) goto done;
    GmlRenderRotatedCoverageRegion *expanded=realloc(regions,REGION_LIMIT*sizeof *regions);
    if(!expanded) goto done;
    regions=expanded;
    if(!gml_render_rotated_nearest_texel_regions(r,sprite,page,x,y,xs,ys,
         rotation,colour,regions,REGION_LIMIT,&count)) goto done;
  }
  if(!count){accepted=1;goto done;}
  operations=calloc(count,sizeof *operations);
  if(!operations) goto done;
  gml_render_flush_pending_fill(r);
  if(r->pending_fill || !(target=gml_render_scene_target_resource_ensure(r))) goto done;
  GmlRenderImageRef image=gml_render_application_scene_next_image_ref(r,target);
  if(image.content_version>UINT64_MAX-count) goto done;
  state.write_mask=15u;state.colour=0xFFFFFFFFu;state.alpha=255u;
  state.rgb_source_factor=state.alpha_source_factor=GML_RENDER_BLEND_SOURCE_ALPHA;
  if(screen_axis) state.alpha_source_factor=GML_RENDER_BLEND_ONE;
  state.rgb_destination_factor=state.alpha_destination_factor=GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA;
  state.rgb_equation=state.alpha_equation=GML_RENDER_BLEND_ADD;
  state.sampler_count=1u;
  uint32_t tint=((colour&255u)<<16)|(colour&0xFF00u)|((colour>>16)&255u);
  for(uint32_t index=0;index<count;index++){
    const GmlRenderRect *rect=&regions[index].region;
    GmlRenderDrawOp *op=&operations[index];
    op->opcode=GML_RENDER_OP_TEXTURED_QUAD;op->reconstruction_mode=GML_RENDER_DEVICE_CHECKPOINT;
    op->scene_family=GML_RENDER_SCENE_SPRITE;op->source_count=1u;op->sources=&source.image;
    op->target.image=image;op->target.image.content_version+=index+1u;
    op->target.storage_version=1u;op->target.width=(uint32_t)r->fbw;op->target.height=(uint32_t)r->fbh;
    op->target.pixel_format=GML_RENDER_PIXEL_ARGB8888;
    op->target.region=(GmlRenderRect){0,0,(uint32_t)r->fbw,(uint32_t)r->fbh};
    GmlRenderQuad *quad=&op->payload.quad;
    quad->shape=GML_RENDER_QUAD_AXIS_ALIGNED;
    quad->sampling.filter=GML_RENDER_FILTER_NEAREST_PHASED;
    quad->sampling.edge_rule=GML_RENDER_EDGE_CLAMP_SUBIMAGE;
    quad->sampling.quantisation=GML_RENDER_QUANTISE_SPRITE_MODERN_ROTATED_NEAREST;
    quad->sampling.source_region=(GmlRenderRect){page->sx,page->sy,(uint32_t)page->sw,(uint32_t)page->sh};
    quad->filtered_draw_alpha=alpha;
    if(rows && !screen_axis){
      quad->span_active=GML_RENDER_SPAN_FIXED20_ROW;
      const int64_t fixed[4]={regions[index].fixed_x,regions[index].delta_x,
                              regions[index].fixed_y,regions[index].delta_y};
      for(unsigned axis=0;axis<2u;axis++){
        double *span=axis?quad->span_y:quad->span_x;
        for(unsigned component=0;component<2u;component++){
          int64_t value=fixed[axis*2u+component];
          int64_t high=(int64_t)floor((double)value/65536.0);
          span[component*2u]=(double)high;
          span[component*2u+1u]=(double)(value-high*65536);
        }
      }
    }
    quad->x[0]=quad->x[3]=rect->x;quad->x[1]=quad->x[2]=(double)rect->x+rect->width;
    quad->y[0]=quad->y[1]=rect->y;quad->y[2]=quad->y[3]=(double)rect->y+rect->height;
    for(unsigned corner=0;corner<4u;corner++){
      quad->corner_colour[corner]=((uint32_t)lround(alpha*255.0)<<24)|tint;
      quad->u[corner]=rows?0.0:((double)page->sx+regions[index].source_x+0.5)/source.width;
      quad->v[corner]=rows?0.0:((double)page->sy+regions[index].source_y+0.5)/source.height;
    }
  }
  state.samplers[0]=operations[0].payload.quad.sampling;
  if(screen_axis){
    operations[0].payload.quad.sampling.quantisation=0u;
    operations[0].payload.quad.sampling.filter=GML_RENDER_FILTER_NEAREST_PHASED_OPAQUE_ALPHA;
    state.samplers[0]=operations[0].payload.quad.sampling;
    accepted=resident_modern_point_regions(r,page,0,0,xs,ys,0,0,operations,&state,&source,regions,count);
    if(accepted){r->fb_all_transparent=0;r->fb_opaque_known=0;}
    goto done;
  }
  GmlRenderDrawBatch batch={operations,count,&state,1u};
  GmlRenderResourceExecution result=gml_render_application_scene_submit(r,&batch,&source,1u);
  if(result==GML_RENDER_RESOURCE_EXEC_CPU) r->resident_application_pass=0;
  accepted=result!=GML_RENDER_RESOURCE_EXEC_FAILED;
  if(accepted){r->fb_all_transparent=0;r->fb_opaque_known=0;}
done:
  free(operations);free(regions);return accepted;
}

static int resident_filtered_sprite(GmlRender *r,GmlSprite *sprite,int subimg,
                                    double x,double y,double xs,double ys,double rotation,
                                    uint32_t blend,double alpha){
  GmlTpag *page;
  int sub,page_index;
  double normalized_rotation;
  if(!r || !sprite || sprite->runtime_rgba || sprite->spine || sprite->ns_enabled ||
     sprite->n_frames<=0 || sprite->w<=0 || sprite->h<=0 ||
     gml_d3_is_active(r) || !isfinite(rotation) ||
     !isfinite(alpha) || alpha<=0.0 || alpha>1.0 ||
     !isfinite(xs) || !isfinite(ys) || xs==0.0 || ys==0.0 ||
     !sprite->frame) return 0;
  sub=gml_render_runtime_frame_index(sprite,subimg);
  page_index=sprite->frame[sub];
  if(page_index<0 || page_index>=r->n_tpag) return 0;
  page=&r->tpag[page_index];
  if(page->bw!=sprite->w || page->bh!=sprite->h) return 0;
  normalized_rotation=fmod(rotation,360.0);
  if(normalized_rotation<0.0) normalized_rotation+=360.0;
  /* The CPU entry treats a narrow near-zero interval as an axis draw. */
  if(rotation!=0.0 && (normalized_rotation<0.001 ||
                       fabs(normalized_rotation-360.0)<0.001)) return 0;
  /* Retain the CPU's two rounded products and ordered additions. Folding
   * (origin - trim) changes original-double phases at channel half-code ties. */
  double left=x-(double)sprite->originx*xs+(double)page->tx*xs-r->cam_x;
  double top=y-(double)sprite->originy*ys+(double)page->ty*ys-r->cam_y;
  if(!r->interp){
    if(rotation==0.0 && xs==0.5 && ys==0.5 &&
       resident_identity_screen_gui(r) &&
       anygm_policy_uses_first_generation_studio(r->win) &&
       !anygm_policy_has_modern_layer_semantics(r->win)){
      if(r->pending_underlay && !gml_render_compose_resident_underlay(r)) return 0;
      return resident_atlas_crop_from_page(r,page,0,0,page->sw,page->sh,
        left,top,xs,ys,GML_RENDER_SCENE_SPRITE,blend,alpha,6,NULL);
    }
    /* Match the CPU entry's large-axis kernel selection before the small point leaf. */
    if(rotation==0.0 && anygm_policy_has_modern_layer_semantics(r->win) &&
       r->target_id<0 && r->target_sp==0 && !gml_render_target_preserves_alpha(r) &&
       fabs((double)page->sw*xs*(double)page->sh*ys)>=131072.0)
      return resident_modern_nearest_rotation(r,sprite,page,x,y,xs,ys,0.0,blend,alpha);
    if(rotation!=0.0 && anygm_policy_has_modern_layer_semantics(r->win)){
      if(r->target_id<0 && r->target_sp==0)
        return resident_modern_screen_binary_rotation(r,sprite,page,x,y,xs,ys,rotation,blend,alpha);
      return resident_modern_nearest_rotation(r,sprite,page,x,y,xs,ys,rotation,blend,alpha);
    }
    /* R03-SPR-FIRSTGEN-NEAREST-REDUCE / R03-SPR-FIRSTGEN-NEAREST-MAG:
     * retain independent authored far-edge coverage and fractional point phase.
     * Enlarged device results additionally require witnessed on-demand observation. */
    if(rotation==0.0 && anygm_policy_uses_first_generation_studio(r->win) &&
       ((xs>0.0 && ys>0.0 && xs<=4.0 && ys<=4.0) ||
        (gml_render_target_is_first_generation_application_surface(r) &&
         r->device.context && r->device.observe_on_demand &&
         r->device.observe_on_demand(r->device.context)))){
      if(r->app_surface && r->fb==r->app_surface)
        return resident_filtered_atlas_page(r,page_index,left,top,xs,ys,
          GML_RENDER_SCENE_SPRITE,blend,alpha,NULL,NULL,0.0,NULL,0,0,0,0,4);
      /* A view scratch is not the first-generation application surface. Its
       * CPU point leaf snaps a unit sprite before sampling, without the
       * application surface's fractional source phase. */
      if(xs==1.0 && ys==1.0)
        return resident_filtered_atlas_page(r,page_index,
          floor(left+0.5),floor(top+0.5),xs,ys,
          GML_RENDER_SCENE_SPRITE,blend,alpha,NULL,NULL,0.0,NULL,0,0,0,0,1);
      return 0;
    }
    if(rotation==0.0 && anygm_policy_has_modern_layer_semantics(r->win) &&
       ((r->target_id>0 && !r->fog_flat && !r->gui_pass_active) ||
        (xs>0.0 && ys>0.0 && xs<=1.0 && ys<=1.0) ||
        (r->target_id<0 && r->target_sp==0 && !r->fog_flat && !r->gui_pass_active &&
         !gml_render_target_preserves_alpha(r) &&
         r->device.context && r->device.observe_on_demand &&
         r->device.observe_on_demand(r->device.context))))
      return resident_filtered_atlas_page(r,page_index,
        left,top,
        xs,ys,GML_RENDER_SCENE_SPRITE,blend,alpha,NULL,NULL,0.0,NULL,
        0,0,0,0,r->fb_opaque_known && r->fb_all_opaque?3:6);
    if(rotation!=0.0 || fabs(xs)!=1.0 || fabs(ys)!=1.0 ||
       !isfinite(left) || !isfinite(top) ||
       fabs(left)>1000000.0 || fabs(top)>1000000.0) return 0;
    /* The nearest CPU leaf rounds its destination origin. At unit scale each snapped
     * destination centre maps exactly to one source texel in the shared sampled-quad
     * helper, while its existing checkpoint preserves tint and intermediate alpha. */
    return resident_filtered_atlas_page(r,page_index,
      floor(left+0.5),floor(top+0.5),xs,ys,
      GML_RENDER_SCENE_SPRITE,blend,alpha,NULL,NULL,0.0,NULL,0,0,0,0,1);
  }
  if(rotation!=0.0)
    return resident_filtered_atlas_page(r,page_index,x-r->cam_x,y-r->cam_y,xs,ys,
                                       GML_RENDER_SCENE_SPRITE,blend,alpha,NULL,sprite,
                                       normalized_rotation,NULL,0,0,0,0,0);
  return gml_render_resident_filtered_atlas_page(
      r,page_index,left,top,
      xs,ys,GML_RENDER_SCENE_SPRITE,blend,alpha,NULL,0);
}

void gml_draw_sprite(GmlRender *r, int sprite, int subimg, double x, double y){
  gml_draw_sprite_ext(r,sprite,subimg,x,y,1,1,0,0xFFFFFF,
                      r && !r->classic ? r->alpha : 1.0);
}

int gml_render_resident_masked_sprite(GmlRender *r,int sprite,int subimg,
                                      double x,double y,double xs,double ys,double rotation,
                                      uint32_t colour,double alpha){
  /* Try only the established modern point sampler on numbered targets. The CPU wrapper
   * keeps its snapshot/merge path for fog, filtering, rotation and every excluded state. */
  if(!r || !r->resident_application_pass || r->target_id<=0 || r->fog_flat ||
     r->color_write_mask>=15u || r->interp || r->gui_pass_active ||
     !anygm_policy_has_modern_layer_semantics(r->win) || sprite<0 || sprite>=r->n_spr)
    return 0;
  gml_render_draw_map_point(r,&x,&y);
  gml_render_draw_map_scale(r,&xs,&ys);
  double resident_alpha=isfinite(alpha) && alpha>1.0?1.0:alpha;
  return resident_filtered_sprite(r,&r->spr[sprite],subimg,x,y,xs,ys,rotation,
                                  colour,resident_alpha);
}

void gml_render_sprite_ext_unmasked(GmlRender *r, int sprite, int subimg, double x, double y,
                                     double xs, double ys, double rot, uint32_t blend, double alpha){
  gml_render_draw_map_point(r,&x,&y);
  gml_render_draw_map_scale(r,&xs,&ys);
  if(r && r->resident_application_pass){
    /* Invalid and frameless ordinary sprites have no raster work to order
     * after the queued prefix. Spine and active 3D retain their earlier CPU
     * preparation, which does not share the ordinary frame-count rule. */
    if(!gml_d3_is_active(r) &&
       (sprite<0 || sprite>=r->n_spr ||
        (!r->spr[sprite].spine && r->spr[sprite].n_frames<=0))) return;
    /* The ordinary CPU axis writer returns before sampling a zero scale.
     * Do not materialise a queued prefix just to execute that same no-op. */
    if(sprite>=0 && sprite<r->n_spr && rot==0.0 &&
       isfinite(xs) && isfinite(ys) && (xs==0.0 || ys==0.0) &&
       !r->spr[sprite].spine && !r->spr[sprite].ns_enabled &&
       r->active_shader<0 && !gml_d3_is_active(r)) return;
    /* R03-SPR-BLACK-ADDITIVE-NOOP: black tint contributes zero RGB. The
     * unchanged modern SCREEN CPU leaf stamps written alpha opaque, so a
     * certified opaque destination is unchanged with either sampler. Keep
     * unknown/nonopaque targets and special preparation on their old paths. */
    if(sprite>=0 && sprite<r->n_spr && r->win && !r->classic &&
       anygm_policy_has_modern_layer_semantics(r->win) &&
       !r->gui_pass_active && r->target_id==-1 && !r->target_sp &&
       r->fb_opaque_known && r->fb_all_opaque &&
       !gml_render_target_preserves_alpha(r) &&
       r->alphablend && r->blendmode==1 &&
       r->blend_equation==1 && r->blend_equation_alpha==1 &&
       r->color_write_mask==15u && !r->fog_flat && !r->alpha_test_enable &&
       r->active_shader<0 && !r->classic_phase_y &&
       !r->classic_interp_phase[0] && !r->classic_interp_phase[1] &&
       !r->classic_interp_phase[2] && !r->app_interp_phase[0] &&
       !r->app_interp_phase[1] && !r->app_interp_phase[2] &&
       !r->pending_underlay && !r->pending_presentation &&
       !r->rotated_batch_count && !r->rotated_batch_building &&
       rot==0.0 && isfinite(x) && isfinite(y) &&
       isfinite(xs) && isfinite(ys) && isfinite(alpha) && alpha>0.0 &&
       (blend&0xFFFFFFu)==0u && !r->spr[sprite].runtime_rgba &&
       !r->spr[sprite].spine && !r->spr[sprite].ns_enabled &&
       !gml_d3_is_active(r) && gml_render_scene_target_matches(r)) return;
    /* R03-P9-SPRITE-ALPHA-BOUNDARY: ordinary finite alpha is clamped by the
     * existing CPU leaves. Normalize only the GPU argument; CPU fallback and
     * specialised preparation retain the original caller value and order. */
    double resident_alpha=isfinite(alpha) && alpha>1.0?1.0:alpha;
    if(isfinite(alpha) && alpha<=0.0 &&
       !(r->alphablend && (r->blendmode==6 ||
                          gml_render_blend_uses_destination_colour(r->blendmode))) &&
       sprite>=0 && sprite<r->n_spr &&
       r->active_shader<0 &&
       !r->spr[sprite].spine && !r->spr[sprite].ns_enabled &&
       !gml_d3_is_active(r) && gml_render_scene_target_matches(r))
      return;
    if(sprite>=0 && sprite<r->n_spr &&
       resident_depth_free_ortho_sprite(r,sprite,subimg,x,y,xs,ys,rot,blend,resident_alpha)) return;
    if(r->classic && sprite>=0 && sprite<r->n_spr && r->spr[sprite].runtime_rgba &&
       resident_runtime_unit_sprite(r,&r->spr[sprite],subimg,x,y,xs,ys,rot,blend,resident_alpha)) return;
    if(r->classic && rot!=0.0 && sprite>=0 && sprite<r->n_spr &&
       isfinite(alpha) && alpha>0.0 &&
       resident_classic_rotated_sprite_noop(r,&r->spr[sprite],subimg,x,y,xs,ys,rot)) return;
    if(sprite>=0 && sprite<r->n_spr &&
       resident_integer_sprite(r,&r->spr[sprite],subimg,x,y,xs,ys,rot,blend,resident_alpha)) return;
    if(sprite>=0 && sprite<r->n_spr &&
       resident_filtered_sprite(r,&r->spr[sprite],subimg,x,y,xs,ys,rot,blend,resident_alpha)) return;
    if(!gml_render_resident_application_cpu_barrier(r)) return;
  }
  if(r && sprite>=0 && sprite<r->n_spr &&
     gml_render_cpu_draw_spine_sprite(r,&r->spr[sprite],x,y,xs,ys,rot,blend,alpha)) return;
  if(gml_d3_draw_sprite_2d(r,sprite,subimg,x,y,xs,ys,rot,blend,alpha)) return;
  if(sprite<0||sprite>=r->n_spr) return;
  if(alpha<=0 && !(r->alphablend && (r->blendmode==6 ||
     gml_render_blend_uses_destination_colour(r->blendmode)))) return;
  GmlSprite *s=&r->spr[sprite]; if(s->n_frames<=0) return;
  /* A sprite drawn through a program this renderer does not execute goes to the same executor as
   * a surface, evaluated at the destination's size. Unrotated, unflipped draws only: the composed
   * plane is axis-aligned, and a rotated or mirrored draw keeps today's plain path. */
  /* A rotated draw composes through the ordinary sprite blit, which already has the pivot. Only a
   * program whose answer is a property of the texels can be kept for it; one that reads its place
   * on the target is evaluated where it lands, and a rotated one of those keeps the plain path. */
  if(rot!=0.0 && xs!=0 && ys!=0 && r->active_shader>=0 && r->shader_executor &&
     gml_render_shader_content_candidate(r,r->active_shader) &&
     r->shader_pal && !r->shader_pal[r->active_shader].position_dependent){
    int sub=((subimg%s->n_frames)+s->n_frames)%s->n_frames;
    int atlas=0,ax=0,ay=0,aw=0,ah=0;
    if(gml_render_sprite_frame_rect(r,sprite,sub,&atlas,&ax,&ay,&aw,&ah)){
      const uint32_t *shaded=gml_render_shaded_atlas_rect(r,atlas,ax,ay,aw,ah);
      if(shaded && gml_render_compose_shaded_rotated(r,s,shaded,aw,ah,x,y,xs,ys,rot,
                                                     s->originx,s->originy,blend,alpha)) return;
    }
  }
  /* Unrotated draws, mirrored or not: the composition takes a negative extent for a flip. */
  if(rot==0.0 && xs!=0 && ys!=0 && r->active_shader>=0 && r->shader_executor &&
     gml_render_shader_content_candidate(r,r->active_shader)){
    int sub=((subimg%s->n_frames)+s->n_frames)%s->n_frames;
    if(gml_render_shade_target_sprite(r,sprite,sub,
                                      x-s->originx*xs,y-s->originy*ys,
                                      s->w*xs,s->h*ys,blend,alpha)) return;
  }
  gml_render_cpu_blit_sprite_ext_resolved(r,s,sprite,subimg,x,y,xs,ys,rot,blend,alpha);
}

/* R03-SPR-COARSE-MIP: the CPU leaf already reduces a full-target minified page to one
 * straight-RGBA texel. Share that exact preparation, then let the device compose the veil.
 * Identity SCREEN GUI uses fixed blending and witnessed on-demand device authority; the
 * established world rule retains its exact blend/replay. Other GUI and special states stay CPU. */
static int resident_coarse_mip_veil(GmlRender *r,int sprite,int frame,
                                   double dx,double dy,double dw,double dh,
                                   uint32_t blend,double alpha){
  GmlSprite *record;
  GmlTpag *page;
  GmlAtlas *atlas;
  GmlRenderResource *target;
  GmlRenderResourceSource source={0};
  GmlRenderDrawOp operation={0};
  GmlRenderDrawStateSnapshot state={0};
  GmlRenderImageRef source_ref,image;
  GmlRenderDrawBatch batch;
  const GmlRenderDeviceServices *device;
  GmlRenderResourceExecution result;
  double mapped_x=dx,mapped_y=dy,mapped_w=dw,mapped_h=dh;
  double texel[4];
  uint8_t pixel[4];
  int x0,y0,width,height,sub,page_index;
  if(!r || !r->resident_application_pass || !(device=gml_render_device(r)) ||
     !device->retire || (!r->interp && !r->gui_pass_active) || r->classic ||
     (r->gui_pass_active &&
      (!resident_identity_screen_gui(r) || r->world_transform_active ||
       !device->context || !device->observe_on_demand ||
       !device->observe_on_demand(device->context))) ||
     r->active_shader>=0 || r->alpha_test_enable || !r->alphablend ||
     r->blendmode || r->blend_equation!=1 || r->blend_equation_alpha!=1 ||
     r->color_write_mask!=0x0Fu || r->classic_phase_y ||
     r->rotated_batch_count || r->rotated_batch_building ||
     r->pending_underlay || r->pending_presentation ||
     r->target_sp || r->target_id!=-1 ||
     !gml_render_scene_target_matches(r) ||
     (!r->gui_pass_active && (!r->fb_opaque_known || !r->fb_all_opaque)) ||
     (r->world_transform_active &&
      (!isfinite(r->world_scale_x) || !isfinite(r->world_scale_y) ||
       r->world_scale_x<=0.0 || r->world_scale_y<=0.0)) ||
     sprite<0 || sprite>=r->n_spr || blend!=0xFFFFFFu || alpha!=1.0 ||
     gml_d3_is_active(r)) return 0;
  for(int index=0;index<3;index++)
    if(r->classic_interp_phase[index] || r->app_interp_phase[index]) return 0;
  record=&r->spr[sprite];
  if(record->runtime_rgba || record->spine || record->ns_enabled ||
     record->n_frames<=0 || !record->frame || record->w<=0 || record->h<=0)
    return 0;
  sub=gml_render_runtime_frame_index(record,frame);
  page_index=record->frame[sub];
  if(page_index<0 || page_index>=r->n_tpag) return 0;
  page=&r->tpag[page_index];
  if(page->atlas<0 || page->atlas>=r->n_atlas || page->sw<=0 || page->sh<=0 ||
     page->sx<0 || page->sy<0) return 0;
  atlas=&r->atlas[page->atlas];
  if(!atlas_pixels(r,page->atlas) || atlas->w<=0 || atlas->h<=0 ||
     (uint64_t)(uint32_t)page->sx+(uint32_t)page->sw>(uint32_t)atlas->w ||
     (uint64_t)(uint32_t)page->sy+(uint32_t)page->sh>(uint32_t)atlas->h)
    return 0;
  gml_render_draw_map_point(r,&mapped_x,&mapped_y);
  gml_render_draw_map_scale(r,&mapped_w,&mapped_h);
  mapped_x-=r->cam_x;
  mapped_y-=r->cam_y;
  if(!isfinite(mapped_x) || !isfinite(mapped_y) || !isfinite(mapped_w) ||
     !isfinite(mapped_h) || fabs(mapped_x)>1000000.0 ||
     fabs(mapped_y)>1000000.0 || mapped_w<=0.0 || mapped_h<=0.0 ||
     mapped_w>1000000.0 || mapped_h>1000000.0) return 0;
  x0=(int)floor(mapped_x);
  y0=(int)floor(mapped_y);
  width=(int)lround(mapped_w);
  height=(int)lround(mapped_h);
  if(width<=0 || height<=0 ||
     !(record->w>width || record->h>height) ||
     x0>0 || y0>0 || (int64_t)x0+width<r->fbw ||
     (int64_t)y0+height<r->fbh) return 0;
  sprite_region_coarse_texel(atlas,page->sx,page->sy,page->sw,page->sh,texel);
  for(int channel=0;channel<4;channel++) pixel[channel]=(uint8_t)floor(texel[channel]+0.5);
  if(pixel[3]==0u) return 1;
  gml_render_flush_pending_fill(r);
  if(r->pending_fill || !(target=gml_render_scene_target_resource_ensure(r))) return 0;
  image=gml_render_application_scene_next_image_ref(r,target);
  if(image.content_version==UINT64_MAX) return 0;
  source.image=(GmlRenderImageRef){GML_RENDER_RESOURCE_SCRATCH,0u,
    gml_render_scene_incarnation_allocate(),1u};
  if(!source.image.incarnation) return 0;
  source.width=source.height=1u;
  source.pixel_format=GML_RENDER_PIXEL_RGBA8888;
  source.stride_bytes=4u;
  source.byte_capacity=sizeof pixel;
  source.bytes=pixel;
  source_ref=source.image;
  operation.opcode=GML_RENDER_OP_TEXTURED_QUAD;
  operation.scene_family=GML_RENDER_SCENE_SPRITE;
  operation.reconstruction_mode=r->gui_pass_active
    ?GML_RENDER_DEVICE_CHECKPOINT:GML_RENDER_REPLAY_EXACT;
  operation.source_count=1u;
  operation.sources=&source_ref;
  operation.target.image=image;
  operation.target.image.content_version++;
  operation.target.storage_version=1u;
  operation.target.width=(uint32_t)r->fbw;
  operation.target.height=(uint32_t)r->fbh;
  operation.target.pixel_format=GML_RENDER_PIXEL_ARGB8888;
  operation.target.region=(GmlRenderRect){0,0,(uint32_t)r->fbw,(uint32_t)r->fbh};
  operation.payload.quad.shape=GML_RENDER_QUAD_AXIS_ALIGNED;
  int stamp_alpha=r->gui_pass_active && (!r->fb_opaque_known || !r->fb_all_opaque);
  operation.payload.quad.sampling.filter=stamp_alpha
    ?GML_RENDER_FILTER_HARDWARE_BILINEAR:GML_RENDER_FILTER_NEAREST;
  operation.payload.quad.filtered_draw_alpha=1.0;
  operation.payload.quad.sampling.edge_rule=GML_RENDER_EDGE_CLAMP_SUBIMAGE;
  operation.payload.quad.sampling.source_region=(GmlRenderRect){0,0,1u,1u};
  for(int corner=0;corner<4;corner++) operation.payload.quad.corner_colour[corner]=0xFFFFFFFFu;
  /* The sampled texel is constant and the qualified quad covers the target completely.
   * Clip its resolved coverage here; replay and the device then share the same finite span. */
  operation.payload.quad.x[0]=operation.payload.quad.x[3]=0.0;
  operation.payload.quad.x[1]=operation.payload.quad.x[2]=r->fbw;
  operation.payload.quad.y[0]=operation.payload.quad.y[1]=0.0;
  operation.payload.quad.y[2]=operation.payload.quad.y[3]=r->fbh;
  operation.payload.quad.u[0]=operation.payload.quad.u[3]=0.0;
  operation.payload.quad.u[1]=operation.payload.quad.u[2]=1.0;
  operation.payload.quad.v[0]=operation.payload.quad.v[1]=0.0;
  operation.payload.quad.v[2]=operation.payload.quad.v[3]=1.0;
  state.write_mask=15u;
  state.colour=0xFFFFFFFFu;
  state.alpha=255u;
  state.coverage_policy=stamp_alpha?GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA:0u;
  state.rgb_source_factor=GML_RENDER_BLEND_SOURCE_ALPHA;
  state.rgb_destination_factor=GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA;
  state.alpha_source_factor=GML_RENDER_BLEND_ONE;
  state.alpha_destination_factor=GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA;
  state.rgb_equation=state.alpha_equation=r->gui_pass_active
    ?GML_RENDER_BLEND_ADD:GML_RENDER_BLEND_EXACT8_ROUND_VEIL;
  state.sampler_count=1u;
  state.samplers[0]=operation.payload.quad.sampling;
  batch=(GmlRenderDrawBatch){&operation,1u,&state,1u};
  result=gml_render_application_scene_submit(r,&batch,&source,1u);
  device->retire(device->context,source.image);
  if(result==GML_RENDER_RESOURCE_EXEC_CPU) r->resident_application_pass=0;
  if(result!=GML_RENDER_RESOURCE_EXEC_FAILED){
    r->fb_all_transparent=0;
    r->fb_opaque_known=r->fb_all_opaque=1;
  }
  return result!=GML_RENDER_RESOURCE_EXEC_FAILED;
}

/* R03-SPR-GUI-MAPPED-STRETCH: the public entry owns GUI mapping and camera
 * subtraction. Newly admitted centred/mapped magnification uses hardware
 * filtering/source-over, not the existing identity GUI's exact float recipe. */
static int resident_mapped_gui_stretch(GmlRender *r,int sprite,int frame,
                                      double dx,double dy,double dw,double dh,
                                      uint32_t blend,double alpha){
  const GmlRenderDeviceServices *device;
  GmlSprite *record;
  GmlTpag *page;
  GmlRenderResourceSource source={0};
  GmlRenderResource *target;
  GmlRenderDrawOp operation={0};
  GmlRenderDrawStateSnapshot state={0};
  GmlRenderImageRef image,source_ref;
  double left,top;
  int width,height,page_index;
  if(!r || !r->resident_application_pass || !r->gui_pass_active ||
     resident_mapped_screen_gui_font(r) || r->classic || !r->win ||
     r->target_id!=-1 || r->target_sp || r->world_transform_active ||
     r->gui_maximise_active || r->gui_maximise_xoffset!=0.0 ||
     r->gui_maximise_yoffset!=0.0 ||
     r->gui_base_logical_w<=0 || r->gui_base_logical_h<=0 ||
     r->gui_logical_w<=0 || r->gui_logical_h<=0 ||
     !isfinite(r->gui_scale_x) || r->gui_scale_x<=0.0 ||
     !isfinite(r->gui_scale_y) || r->gui_scale_y<=0.0 ||
     !r->interp || r->active_shader>=0 || r->fog_flat || r->alpha_test_enable ||
     !r->alphablend || r->blendmode || r->blend_equation!=1 ||
     r->blend_equation_alpha!=1 || r->color_write_mask!=15u ||
     r->classic_phase_y || r->rotated_batch_count || r->rotated_batch_building ||
     r->pending_underlay || r->pending_presentation ||
     !gml_render_scene_target_matches(r) || !r->base_scene_resource ||
     r->fb!=r->base_scene_resource_pixels ||
     !(device=gml_render_device(r)) || !device->context || !device->observe_on_demand ||
     !device->observe_on_demand(device->context) || gml_d3_is_active(r) ||
     sprite<0 || sprite>=r->n_spr || blend!=0xFFFFFFu || alpha!=1.0) return 0;
  for(int index=0;index<3;index++)
    if(r->classic_interp_phase[index] || r->app_interp_phase[index]) return 0;
  record=&r->spr[sprite];
  if(record->runtime_rgba || record->spine || record->ns_enabled ||
     record->w<=0 || record->h<=0 || record->n_frames<=0 || !record->frame) return 0;
  page_index=record->frame[gml_render_runtime_frame_index(record,frame)];
  if(page_index<0 || page_index>=r->n_tpag) return 0;
  page=&r->tpag[page_index];
  if(page->tx || page->ty || page->sw!=record->w || page->sh!=record->h ||
     page->tw!=page->sw || page->th!=page->sh ||
     page->bw!=page->sw || page->bh!=page->sh || page->sx<0 || page->sy<0 ||
     page->atlas<0 || page->atlas>=r->n_atlas) return 0;
  gml_render_draw_map_point(r,&dx,&dy);
  gml_render_draw_map_scale(r,&dw,&dh);
  dx-=r->cam_x;dy-=r->cam_y;
  if(!isfinite(dx) || !isfinite(dy) || fabs(dx)>16384.0 || fabs(dy)>16384.0 ||
     !isfinite(dw) || !isfinite(dh) || dw<=0.0 || dh<=0.0 ||
     dw>16384.0 || dh>16384.0) return 0;
  left=floor(dx);top=floor(dy);
  width=(int)lround(dw);height=(int)lround(dh);
  if(width<=record->w || height<=record->h ||
     !gml_render_atlas_scene_source_view(r,page->atlas,&source) ||
     (uint64_t)(uint32_t)page->sx+(uint32_t)page->sw>source.width ||
     (uint64_t)(uint32_t)page->sy+(uint32_t)page->sh>source.height) return 0;
  if(left>=r->fbw || top>=r->fbh || left+width<=0.0 || top+height<=0.0) return 1;
  gml_render_flush_pending_fill(r);
  if(r->pending_fill || !(target=gml_render_scene_target_resource_ensure(r))) return 0;
  image=gml_render_application_scene_next_image_ref(r,target);
  if(image.content_version==UINT64_MAX) return 0;
  source_ref=source.image;
  operation.opcode=GML_RENDER_OP_TEXTURED_QUAD;
  operation.scene_family=GML_RENDER_SCENE_SPRITE;
  operation.reconstruction_mode=GML_RENDER_DEVICE_CHECKPOINT;
  operation.source_count=1u;operation.sources=&source_ref;
  operation.target.image=image;operation.target.image.content_version++;
  operation.target.storage_version=1u;
  operation.target.width=(uint32_t)r->fbw;operation.target.height=(uint32_t)r->fbh;
  operation.target.pixel_format=GML_RENDER_PIXEL_ARGB8888;
  operation.target.region=(GmlRenderRect){0,0,(uint32_t)r->fbw,(uint32_t)r->fbh};
  GmlRenderQuad *quad=&operation.payload.quad;
  quad->shape=GML_RENDER_QUAD_AXIS_ALIGNED;
  quad->sampling.filter=GML_RENDER_FILTER_HARDWARE_BILINEAR;
  quad->sampling.edge_rule=GML_RENDER_EDGE_CLAMP_SUBIMAGE;
  quad->sampling.source_region=(GmlRenderRect){page->sx,page->sy,(uint32_t)page->sw,(uint32_t)page->sh};
  quad->filtered_draw_alpha=1.0;
  for(unsigned corner=0;corner<4u;corner++){
    quad->x[corner]=left+((corner==1u || corner==2u)?width:0);
    quad->y[corner]=top+(corner>=2u?height:0);
    quad->u[corner]=((double)page->sx+((corner==1u || corner==2u)?page->sw:0))/source.width;
    quad->v[corner]=((double)page->sy+(corner>=2u?page->sh:0))/source.height;
    quad->corner_colour[corner]=0xFFFFFFFFu;
  }
  state.write_mask=15u;state.colour=0xFFFFFFFFu;state.alpha=255u;
  state.coverage_policy=GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA;
  state.rgb_source_factor=GML_RENDER_BLEND_SOURCE_ALPHA;
  state.rgb_destination_factor=GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA;
  state.alpha_source_factor=GML_RENDER_BLEND_ONE;
  state.alpha_destination_factor=GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA;
  state.rgb_equation=state.alpha_equation=GML_RENDER_BLEND_ADD;
  state.sampler_count=1u;state.samplers[0]=quad->sampling;
  if(!gml_render_application_scene_enqueue(r,&operation,&state,&source)) return 0;
  r->fb_all_transparent=0;
  return 1;
}

void gml_draw_sprite_stretched(GmlRender *r, int sprite, int frame, double dx, double dy, double dw, double dh, uint32_t blend, double alpha){
  /* The ordinary stretched CPU blit contributes no pixels at zero alpha. Do not materialise
   * preceding device work for that no-op; special draw states retain their existing barrier. */
  if(r && r->resident_application_pass && alpha==0.0 &&
     sprite>=0 && sprite<r->n_spr && !r->classic && !r->gui_pass_active &&
     r->active_shader<0 && !r->alpha_test_enable && r->alphablend &&
     r->blendmode==0 && r->blend_equation==1 && r->blend_equation_alpha==1 &&
     r->color_write_mask==0x0Fu && !r->pending_underlay &&
     !r->pending_presentation && !r->rotated_batch_count &&
     !r->rotated_batch_building && !r->classic_phase_y &&
     !r->spr[sprite].spine && !r->spr[sprite].ns_enabled &&
     !r->spr[sprite].runtime_rgba && r->spr[sprite].n_frames>0 &&
     r->spr[sprite].frame &&
     !gml_d3_is_active(r) && gml_render_scene_target_matches(r))
    return;
  if(resident_coarse_mip_veil(r,sprite,frame,dx,dy,dw,dh,blend,alpha)) return;
  if(resident_mapped_gui_stretch(r,sprite,frame,dx,dy,dw,dh,blend,alpha)) return;
  if(r && r->resident_application_pass && !r->interp &&
     blend==0xFFFFFFu && alpha==1.0 &&
     sprite>=0 && sprite<r->n_spr && !gml_d3_is_active(r)){
    GmlSprite *resident_sprite=&r->spr[sprite];
    if(!resident_sprite->runtime_rgba && !resident_sprite->ns_enabled &&
       resident_sprite->n_frames>0 && resident_sprite->frame &&
       resident_sprite->w>0 && resident_sprite->h>0){
      int sub=((frame%resident_sprite->n_frames)+resident_sprite->n_frames)%
              resident_sprite->n_frames;
      int page_index=resident_sprite->frame[sub];
      if(page_index>=0 && page_index<r->n_tpag){
        GmlTpag *page=&r->tpag[page_index];
        if(page->tx==0 && page->ty==0 &&
           page->sw==resident_sprite->w && page->sh==resident_sprite->h &&
           page->bw==page->sw && page->bh==page->sh){
          double mapped_x=dx,mapped_y=dy,mapped_w=dw,mapped_h=dh;
          gml_render_draw_map_point(r,&mapped_x,&mapped_y);
          gml_render_draw_map_scale(r,&mapped_w,&mapped_h);
          if(mapped_w>=resident_sprite->w && mapped_h>=resident_sprite->h &&
             gml_render_resident_atlas_page(r,page_index,
               mapped_x-r->cam_x,mapped_y-r->cam_y,
               mapped_w/resident_sprite->w,mapped_h/resident_sprite->h,
               GML_RENDER_SCENE_SPRITE)) return;
        }
      }
    }
  }
  /* Ordinary magnification and bounded box stretches use the CPU leaf's floored destination
   * origin and rounded extent. The full-target coarse-mip effect and nine-slice stay separate. */
  if(r && r->resident_application_pass &&
     isfinite(alpha) && alpha>0.0 && alpha<=1.0 &&
     sprite>=0 && sprite<r->n_spr &&
     !gml_d3_is_active(r)){
    GmlSprite *resident_sprite=&r->spr[sprite];
    if(!resident_sprite->runtime_rgba && !resident_sprite->spine &&
       !resident_sprite->ns_enabled && resident_sprite->n_frames>0 &&
       resident_sprite->frame && resident_sprite->w>0 && resident_sprite->h>0){
      int sub=gml_render_runtime_frame_index(resident_sprite,frame);
      int page_index=resident_sprite->frame[sub];
      if(page_index>=0 && page_index<r->n_tpag){
        GmlTpag *page=&r->tpag[page_index];
        double mapped_x=dx,mapped_y=dy,mapped_w=dw,mapped_h=dh;
        gml_render_draw_map_point(r,&mapped_x,&mapped_y);
        gml_render_draw_map_scale(r,&mapped_w,&mapped_h);
        if(isfinite(mapped_x) && isfinite(mapped_y) &&
           isfinite(mapped_w) && isfinite(mapped_h) &&
           mapped_w>0.0 && mapped_h>0.0 &&
           mapped_w<1000000.0 && mapped_h<1000000.0 &&
           fabs(mapped_x)<1000000.0 && fabs(mapped_y)<1000000.0 &&
           page->tx>=0 && page->ty>=0 &&
           page->bw==resident_sprite->w && page->bh==resident_sprite->h &&
           page->sw>0 && page->sh>0 &&
           (uint64_t)(uint32_t)page->tx+(uint32_t)page->sw<=(uint32_t)page->bw &&
           (uint64_t)(uint32_t)page->ty+(uint32_t)page->sh<=(uint32_t)page->bh){
          int width=(int)lround(mapped_w),height=(int)lround(mapped_h);
          double left=floor(mapped_x-r->cam_x),top=floor(mapped_y-r->cam_y);
          if(r->interp && width>resident_sprite->w && height>resident_sprite->h &&
             resident_filtered_atlas_page(r,page_index,
               left,top,
               (double)width/resident_sprite->w,
               (double)height/resident_sprite->h,
               GML_RENDER_SCENE_SPRITE,blend,alpha,NULL,NULL,0.0,NULL,1,
               page->tx || page->ty || page->sw!=page->bw || page->sh!=page->bh,0,0,0)) return;
          if(width>0 && height>0 && (width!=resident_sprite->w || height!=resident_sprite->h) &&
             (width<=resident_sprite->w || height<=resident_sprite->h) &&
             resident_sprite->w<=4096 && resident_sprite->h<=4096 &&
             width<=4096 && height<=4096 &&
             ((resident_sprite->w+width-1)/width)<=8 &&
             ((resident_sprite->h+height-1)/height)<=8 &&
             !((resident_sprite->w>width || resident_sprite->h>height) &&
               left<=0.0 && top<=0.0 && left+width>=r->fbw && top+height>=r->fbh) &&
             resident_filtered_atlas_page(r,page_index,left,top,
               (double)width/resident_sprite->w,
               (double)height/resident_sprite->h,
               GML_RENDER_SCENE_SPRITE,blend,alpha,NULL,NULL,0.0,NULL,1,0,1,0,0)) return;
        }
      }
    }
  }
  if(!gml_render_resident_application_cpu_barrier(r)) return;
  gml_render_draw_map_point(r,&dx,&dy);
  gml_render_draw_map_scale(r,&dw,&dh);
  if(sprite<0||sprite>=r->n_spr) return;
  GmlSprite *s=&r->spr[sprite]; if(s->n_frames<=0) return;
  if(s->w>0 && s->h>0){
    double xs=dw/s->w,ys=dh/s->h;
    if(gml_d3_draw_sprite_2d(r,sprite,frame,dx+s->originx*xs,dy+s->originy*ys,xs,ys,0,blend,alpha)) return;
  }
  /* A stretched sprite drawn through a program this renderer does not execute reaches the same
   * executor as any other sprite draw; the region is the whole frame and the extent is the one
   * asked for, so a flip is carried as a negative extent exactly as elsewhere. */
  if(dw!=0.0 && dh!=0.0 && r->active_shader>=0 && r->shader_executor &&
     gml_render_shader_content_candidate(r,r->active_shader)){
    int sub=((frame%s->n_frames)+s->n_frames)%s->n_frames;
    if(gml_render_shade_target_sprite(r,sprite,sub,dx,dy,dw,dh,blend,alpha)) return;
  }
  gml_render_cpu_blit_sprite_stretched_resolved(r,s,frame,dx,dy,dw,dh,blend,alpha);
}

/* The ordinary rectangular form of draw_sprite_pos uses the established atlas-cell blitter.
 * Its mapped physical crop can therefore share the resident nearest-cell preparation. Other
 * positioned forms retain their two-triangle CPU raster until separately qualified. */
static int resident_positioned_axis_sprite(GmlRender *r,int sprite,int subimg,
                                            const double x[4],const double y[4],double alpha){
  GmlRenderSpritePosGeometry geometry;
  double mapped_x[4],mapped_y[4];
  if(!r || !r->resident_application_pass || r->classic ||
     gml_d3_is_active(r) || !isfinite(alpha) || alpha<=0.0 || alpha>1.0 ||
     sprite<0 || sprite>=r->n_spr) return 0;
  GmlSprite *source=&r->spr[sprite];
  if(source->runtime_rgba || source->n_frames<=0 || !source->frame) return 0;
  int frame=gml_render_runtime_frame_index(source,subimg);
  int page_index=source->frame[frame];
  if(page_index<0 || page_index>=r->n_tpag) return 0;
  GmlTpag *page=&r->tpag[page_index];
  if(page->sw<=0 || page->sh<=0) return 0;
  for(int corner=0;corner<4;corner++){
    if(!isfinite(x[corner]) || !isfinite(y[corner])) return 0;
    mapped_x[corner]=x[corner]; mapped_y[corner]=y[corner];
    gml_render_draw_map_point(r,&mapped_x[corner],&mapped_y[corner]);
    if(!isfinite(mapped_x[corner]) || !isfinite(mapped_y[corner])) return 0;
  }
  if(gml_render_sprite_pos_geometry(r,mapped_x,mapped_y,&geometry)!=1) return 0;
  if(r->interp)
    return resident_filtered_atlas_page(r,page_index,geometry.qx[0],geometry.qy[0],
      (geometry.qx[1]-geometry.qx[0])/page->sw,
      (geometry.qy[3]-geometry.qy[0])/page->sh,GML_RENDER_SCENE_SPRITE,
      0xFFFFFFu,alpha,NULL,NULL,0.0,NULL,
      r->win && anygm_policy_has_modern_layer_semantics(r->win),0,0,1,0);
  if(alpha!=1.0) return 0;
  return gml_render_resident_atlas_page(r,page_index,geometry.qx[0],geometry.qy[0],
    (geometry.qx[1]-geometry.qx[0])/page->sw,
    (geometry.qy[3]-geometry.qy[0])/page->sh,GML_RENDER_SCENE_SPRITE);
}

/* The CPU's two-triangle barycentric UVs reduce to one affine inverse for a
 * parallelogram. A convex warped quad carries those UVs at its four corners. */
static int resident_positioned_quad_sprite(GmlRender *r,int sprite,int subimg,
                                              const double x[4],const double y[4],double alpha){
  GmlRenderSpritePosGeometry geometry;
  GmlRenderResourceSource source={0};
  GmlRenderResource *target;
  GmlRenderDrawOp operation={0};
  GmlRenderDrawStateSnapshot state={0};
  GmlRenderImageRef image;
  GmlTpag *page;
  GmlAtlas *atlas;
  double mapped_x[4],mapped_y[4],ax,ay,bx,by,det;
  int page_index,affine;
  if(!r || !r->resident_application_pass || !r->win || r->classic ||
     !(anygm_policy_uses_first_generation_studio(r->win) ||
       anygm_policy_has_modern_layer_semantics(r->win)) ||
     !r->alphablend || r->blendmode || r->blend_equation!=1 ||
     r->blend_equation_alpha!=1 || r->color_write_mask!=0x0fu ||
     r->gui_pass_active || r->active_shader>=0 || r->alpha_test_enable ||
     r->classic_phase_y || r->rotated_batch_count || r->rotated_batch_building ||
     r->pending_underlay || r->pending_presentation || gml_d3_is_active(r) ||
     !gml_render_scene_target_matches(r) ||
     !isfinite(alpha) || alpha<=0.0 || alpha>1.0 ||
     sprite<0 || sprite>=r->n_spr) return 0;
  for(int index=0;index<3;index++)
    if(r->classic_interp_phase[index] || r->app_interp_phase[index]) return 0;
  GmlSprite *sprite_source=&r->spr[sprite];
  if(sprite_source->runtime_rgba || sprite_source->spine ||
     sprite_source->ns_enabled || sprite_source->n_frames<=0 ||
     !sprite_source->frame) return 0;
  page_index=sprite_source->frame[gml_render_runtime_frame_index(sprite_source,subimg)];
  if(page_index<0 || page_index>=r->n_tpag) return 0;
  page=&r->tpag[page_index];
  if(page->sw<=0 || page->sh<=0 || page->sx<0 || page->sy<0 ||
     page->tw!=page->sw || page->th!=page->sh ||
     page->atlas<0 || page->atlas>=r->n_atlas) return 0;
  atlas=&r->atlas[page->atlas];
  if(!gml_render_atlas_scene_source_view(r,page->atlas,&source) ||
     (uint64_t)(uint32_t)page->sx+(uint32_t)page->sw>source.width ||
     (uint64_t)(uint32_t)page->sy+(uint32_t)page->sh>source.height) return 0;
  resident_classify_page(page,atlas);
  if(anygm_policy_has_modern_layer_semantics(r->win) &&
     (!r->fb_opaque_known || !r->fb_all_opaque)) return 0;
  for(int corner=0;corner<4;corner++){
    if(!isfinite(x[corner]) || !isfinite(y[corner])) return 0;
    mapped_x[corner]=x[corner]; mapped_y[corner]=y[corner];
    gml_render_draw_map_point(r,&mapped_x[corner],&mapped_y[corner]);
    if(!isfinite(mapped_x[corner]) || !isfinite(mapped_y[corner])) return 0;
  }
  if(gml_render_sprite_pos_geometry(r,mapped_x,mapped_y,&geometry)!=2) return 0;
  affine=fabs(geometry.qx[2]-(geometry.qx[1]+geometry.qx[3]-geometry.qx[0]))<=1e-9 &&
         fabs(geometry.qy[2]-(geometry.qy[1]+geometry.qy[3]-geometry.qy[0]))<=1e-9;
  for(int corner=0;corner<4;corner++){
    if(!affine && (geometry.qx[corner]!=floor(geometry.qx[corner]) ||
                   geometry.qy[corner]!=floor(geometry.qy[corner]))) return 0;
    int next=(corner+1)&3,after=(corner+2)&3;
    double edge_x=geometry.qx[next]-geometry.qx[corner];
    double edge_y=geometry.qy[next]-geometry.qy[corner];
    double turn_x=geometry.qx[after]-geometry.qx[next];
    double turn_y=geometry.qy[after]-geometry.qy[next];
    if(edge_x*turn_y-edge_y*turn_x<=1e-9) return 0;
  }
  ax=geometry.qx[1]-geometry.qx[0]; ay=geometry.qy[1]-geometry.qy[0];
  bx=geometry.qx[3]-geometry.qx[0]; by=geometry.qy[3]-geometry.qy[0];
  /* A nearest, freely rotated affine edge can land exactly on a source-texel
   * boundary where the CPU barycentric double and GPU inverse float pick
   * opposite sides. Cardinal and axis-led shear cases are separately tested. */
  if(affine && !r->interp && fabs(ax)>1e-9 && fabs(ay)>1e-9) return 0;
  if(!affine && !r->interp && (fabs(ay)>1e-9 || fabs(bx)>1e-9)) return 0;
  det=ax*by-ay*bx;
  if(!isfinite(det) || det<=1e-9 || det>100000000.0) return 0;
  gml_render_flush_pending_fill(r);
  if(r->pending_fill || !(target=gml_render_scene_target_resource_ensure(r))) return 0;
  image=gml_render_application_scene_next_image_ref(r,target);
  if(image.content_version==UINT64_MAX) return 0;
  operation.opcode=GML_RENDER_OP_TEXTURED_QUAD;
  operation.reconstruction_mode=GML_RENDER_DEVICE_CHECKPOINT;
  operation.scene_family=GML_RENDER_SCENE_SPRITE;
  operation.source_count=1u;
  operation.sources=&source.image;
  operation.target.image=image;
  operation.target.image.content_version++;
  operation.target.storage_version=1u;
  operation.target.width=(uint32_t)r->fbw;
  operation.target.height=(uint32_t)r->fbh;
  operation.target.pixel_format=GML_RENDER_PIXEL_ARGB8888;
  operation.target.region=(GmlRenderRect){0,0,(uint32_t)r->fbw,(uint32_t)r->fbh};
  GmlRenderQuad *quad=&operation.payload.quad;
  quad->shape=affine?GML_RENDER_QUAD_GENERAL:GML_RENDER_QUAD_WARPED;
  quad->sampling.filter=r->interp?GML_RENDER_FILTER_BILINEAR:GML_RENDER_FILTER_NEAREST;
  quad->sampling.edge_rule=GML_RENDER_EDGE_CLAMP_SUBIMAGE;
  quad->sampling.source_region=(GmlRenderRect){page->sx,page->sy,
                                             (uint32_t)page->sw,(uint32_t)page->sh};
  for(int corner=0;corner<4;corner++){
    quad->x[corner]=geometry.qx[corner];
    quad->y[corner]=geometry.qy[corner];
    quad->corner_colour[corner]=((uint32_t)lround(alpha*255.0)<<24)|0xFFFFFFu;
  }
  if(!affine){
    quad->u[0]=quad->u[3]=0.0;
    quad->u[1]=quad->u[2]=(double)page->sw;
    quad->v[0]=quad->v[1]=0.0;
    quad->v[2]=quad->v[3]=(double)page->sh;
  }
  quad->inverse_anchor[0]=geometry.qx[0];
  quad->inverse_anchor[1]=geometry.qy[0];
  quad->inverse_x[0]=by*page->sw/det;
  quad->inverse_y[0]=-bx*page->sw/det;
  quad->inverse_x[1]=-ay*page->sh/det;
  quad->inverse_y[1]=ax*page->sh/det;
  quad->inverse_offset[0]=quad->inverse_offset[1]=r->interp?-0.5:0.0;
  state.write_mask=15u;
  state.colour=0xFFFFFFFFu;
  state.alpha=255u;
  if(!page->scene_fully_opaque || alpha<1.0){
    state.rgb_source_factor=GML_RENDER_BLEND_SOURCE_ALPHA;
    state.rgb_destination_factor=GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA;
    state.alpha_source_factor=anygm_policy_has_modern_layer_semantics(r->win)
      ?GML_RENDER_BLEND_ONE:GML_RENDER_BLEND_SOURCE_ALPHA;
    state.alpha_destination_factor=GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA;
    state.rgb_equation=GML_RENDER_BLEND_ADD;
    state.alpha_equation=GML_RENDER_BLEND_ADD;
  }
  state.sampler_count=1u;
  state.samplers[0]=quad->sampling;
  if(!gml_render_application_scene_enqueue(r,&operation,&state,&source)) return 0;
  r->fb_all_transparent=0;
  return 1;
}

void gml_draw_sprite_pos(GmlRender *r,int sprite,int subimg,
                         const double x[4],const double y[4],double alpha){
  if(resident_positioned_axis_sprite(r,sprite,subimg,x,y,alpha)) return;
  if(resident_positioned_quad_sprite(r,sprite,subimg,x,y,alpha)) return;
  if(!gml_render_resident_application_cpu_barrier(r)) return;
  double mapped_x[4],mapped_y[4];
  for(int i=0;i<4;i++){
    mapped_x[i]=x[i]; mapped_y[i]=y[i];
    gml_render_draw_map_point(r,&mapped_x[i],&mapped_y[i]);
  }
  x=mapped_x; y=mapped_y;
  if(gml_d3_draw_sprite_pos_2d(r,sprite,subimg,x,y,alpha)) return;
  if(render_setting(r,"GML_LOG_SPRITE_POS")){
    if(r && r->sprite_position_log_count++<16) anygm_host_logf(r && r->win ? r->win->host : NULL,ANYGM_LOG_DEBUG,"[sprite-pos] sprite=%d sub=%d alpha=%.3f p=(%.2f,%.2f)(%.2f,%.2f)(%.2f,%.2f)(%.2f,%.2f)\n",
      sprite,subimg,alpha,x[0],y[0],x[1],y[1],x[2],y[2],x[3],y[3]);
  }
  if(!r||!r->fb||sprite<0||sprite>=r->n_spr||
     (alpha<=0 && !(r->alphablend &&
       gml_render_blend_uses_destination_colour(r->blendmode)))) return;
  if(alpha>1) alpha=1;
  for(int i=0;i<4;i++) if(!isfinite(x[i])||!isfinite(y[i])) return;
  gml_render_cpu_blit_sprite_pos_resolved(r,sprite,subimg,x,y,alpha);
}

int gml_render_sprite_pos_geometry(GmlRender *r, const double x[4], const double y[4],
                                    GmlRenderSpritePosGeometry *geometry){
  double *qx=geometry->qx,*qy=geometry->qy,shift=r->classic?-.5:0;
  for(int i=0;i<4;i++){ qx[i]=x[i]-r->cam_x+shift; qy[i]=y[i]-r->cam_y+shift; }
  /* Most calls are ordinary rectangles. Preserve the established axis blitters for this common
   * case and reserve barycentric work for genuinely distorted quads. */
  if(!r->classic && qx[1]>qx[0] && qy[3]>qy[0] &&
     fabs(qy[0]-qy[1])<1e-9 && fabs(qx[1]-qx[2])<1e-9 &&
     fabs(qy[2]-qy[3])<1e-9 && fabs(qx[3]-qx[0])<1e-9){
    return 1;
  }

  double minx=qx[0],maxx=qx[0],miny=qy[0],maxy=qy[0];
  for(int i=1;i<4;i++){
    if(qx[i]<minx) minx=qx[i];
    if(qx[i]>maxx) maxx=qx[i];
    if(qy[i]<miny) miny=qy[i];
    if(qy[i]>maxy) maxy=qy[i];
  }
  int x0=(int)floor(minx),x1=(int)ceil(maxx)-1,y0=(int)floor(miny),y1=(int)ceil(maxy)-1;
  if(x0<0) x0=0;
  if(y0<0) y0=0;
  if(x1>=r->fbw) x1=r->fbw-1;
  if(y1>=r->fbh) y1=r->fbh-1;
  if(x1<x0||y1<y0) return 0;
  static const double uvx[4]={0,1,1,0},uvy[4]={0,0,1,1};
  const int tri[2][3]={{0,1,2},{0,2,3}};
  double (*tx)[3]=geometry->tx,(*ty)[3]=geometry->ty;
  double (*tu)[3]=geometry->tu,(*tv)[3]=geometry->tv;
  for(int part=0;part<2;part++) for(int k=0;k<3;k++){
    int index=tri[part][k];
    tx[part][k]=qx[index]; ty[part][k]=qy[index];
    tu[part][k]=uvx[index]; tv[part][k]=uvy[index];
  }
  geometry->x0=x0; geometry->x1=x1; geometry->y0=y0; geometry->y1=y1;
  return 2;
}

/* one nine-slice band: tile (repeat/mirror) or stretch the source region (full-frame logical
 * coords) into the destination span. Tiling anchors at the band's top-left and crops the last
 * tile, according to the nine-slice contract. Mirror and blank-repeat modes
 * fall back to plain repeat. */
static void ns_band(GmlRender *r, GmlTpag *t, int mode,
                    int sx, int sy, int sw, int sh,
                    double dx, double dy, int tw, int th,
                    uint32_t blend, double alpha){
  if(sw<=0 || sh<=0 || tw<=0 || th<=0) return;
  if(mode==4) return;                                   /* hide */
  if(mode==0){                                          /* stretch */
    gml_render_cpu_blit_tpag_part(r,t,sx,sy,sw,sh,dx,dy,(double)tw/sw,(double)th/sh,blend,alpha);
    return;
  }
  for(int oy=0; oy<th; oy+=sh){                          /* repeat (also mirror/blankrepeat) */
    int ch=sh<th-oy? sh : th-oy;
    for(int ox=0; ox<tw; ox+=sw){
      int cw=sw<tw-ox? sw : tw-ox;
      gml_render_cpu_blit_tpag_part(r,t,sx,sy,cw,ch,dx+ox,dy+oy,1,1,blend,alpha);
    }
  }
}
/* draw a nine-slice sprite frame into the W x H target box at (dx0,dy0) (camera applied).
 * Corners stay native-size; edges and center follow their tile modes. */
void gml_render_sprite_nineslice(GmlRender *r, GmlSprite *s, GmlTpag *t,
                                  double dx0, double dy0, int W, int H,
                                  uint32_t blend, double alpha){
  int sw=s->w, sh=s->h;
  int l=s->ns_l, rt=s->ns_r, tp=s->ns_t, bt=s->ns_b;
  int cw=sw-l-rt, chh=sh-tp-bt;      /* source center band sizes */
  int tw=W-l-rt, th=H-tp-bt;         /* target center band sizes */
  /* corners (never scaled) */
  gml_render_cpu_blit_tpag_part(r,t,0,0,l,tp,          dx0,        dy0,        1,1,blend,alpha);
  gml_render_cpu_blit_tpag_part(r,t,sw-rt,0,rt,tp,     dx0+W-rt,   dy0,        1,1,blend,alpha);
  gml_render_cpu_blit_tpag_part(r,t,0,sh-bt,l,bt,      dx0,        dy0+H-bt,   1,1,blend,alpha);
  gml_render_cpu_blit_tpag_part(r,t,sw-rt,sh-bt,rt,bt, dx0+W-rt,   dy0+H-bt,   1,1,blend,alpha);
  /* edges: top(1), bottom(3), left(0), right(2) — tile mode indices per the SPRT record */
  ns_band(r,t,s->ns_tile[1], l,0,cw,tp,      dx0+l,      dy0,        tw,tp, blend,alpha);
  ns_band(r,t,s->ns_tile[3], l,sh-bt,cw,bt,  dx0+l,      dy0+H-bt,   tw,bt, blend,alpha);
  ns_band(r,t,s->ns_tile[0], 0,tp,l,chh,     dx0,        dy0+tp,     l,th,  blend,alpha);
  ns_band(r,t,s->ns_tile[2], sw-rt,tp,rt,chh,dx0+W-rt,   dy0+tp,     rt,th, blend,alpha);
  /* center */
  ns_band(r,t,s->ns_tile[4], l,tp,cw,chh,    dx0+l,      dy0+tp,     tw,th, blend,alpha);
}

/* R03-SPR-MODERN-UNIT-PART: consume the shared selection and mapping once.
 * Integral clipped texels use natural nearest sampling and endpoint blending. */
static int resident_modern_unit_sprite_part(GmlRender *r,GmlTpag *page,const GmlTpag *view,
  double sx,double sy,double sw,double sh,double left,double top,double xs,double ys,
  uint32_t colour,double alpha){
  GmlRenderResourceSource source={0};
  GmlRenderDrawStateSnapshot state={0};
  GmlRenderDrawOp operation={0};
  const GmlRenderDeviceServices *device=gml_render_device(r);
  if(!r || !page || !view || !device || !device->observe_on_demand ||
     !device->observe_on_demand(device->context) || !r->resident_application_pass ||
     r->classic || r->interp || !anygm_policy_has_modern_layer_semantics(r->win) ||
     r->target_id>0 || r->target_sp || r->gui_pass_active || r->fog_flat ||
     gml_d3_is_active(r) || r->active_shader>=0 || r->alpha_test_enable ||
     !r->alphablend || r->blendmode || r->blend_equation!=1 ||
     r->blend_equation_alpha!=1 || r->color_write_mask!=15u || r->classic_phase_y ||
     r->rotated_batch_count || r->rotated_batch_building || r->pending_underlay ||
     r->pending_presentation || !gml_render_scene_target_matches(r) ||
     xs!=1.0 || ys!=1.0 || colour!=0xFFFFFFu || alpha!=1.0 ||
     floor(sx)!=sx || floor(sy)!=sy || floor(sw)!=sw || floor(sh)!=sh ||
     !isfinite(left) || !isfinite(top) || fabs(left)>65536.0 || fabs(top)>65536.0 ||
     floor(left)!=left || floor(top)!=top ||
     !isfinite(r->cam_x) || !isfinite(r->cam_y) ||
     floor(r->cam_x)!=r->cam_x || floor(r->cam_y)!=r->cam_y ||
     r->fbw<=0 || r->fbh<=0 || r->fbw>16384 || r->fbh>16384 ||
     page->project_authored_edges || page->sw<=0 || page->sh<=0 ||
     page->bw>4096 || page->bh>4096 || page->sx<0 || page->sy<0 ||
     (uint64_t)(uint32_t)page->sw*(uint32_t)page->sh>65536u ||
     view->sw<=0 || view->sh<=0 || view->atlas!=page->atlas ||
     view->sx<page->sx || view->sy<page->sy ||
     (int64_t)view->sx+view->sw>(int64_t)page->sx+page->sw ||
     (int64_t)view->sy+view->sh>(int64_t)page->sy+page->sh ||
     !gml_render_atlas_scene_source_view(r,page->atlas,&source) || !source.borrowed_immutable ||
     source.width>16384u || source.height>16384u || r->atlas[page->atlas].runtime_font_page ||
     (uint64_t)(uint32_t)page->sx+(uint32_t)page->sw>source.width ||
     (uint64_t)(uint32_t)page->sy+(uint32_t)page->sh>source.height) return 0;
  for(unsigned index=0;index<3u;index++)
    if(r->classic_interp_phase[index] || r->app_interp_phase[index]) return 0;
  resident_classify_page(page,&r->atlas[page->atlas]);
  if(!page->scene_binary_alpha) return 0;
  double right=left+view->sw,bottom=top+view->sh;
  if(left>=r->fbw || top>=r->fbh || right<=0.0 || bottom<=0.0) return 1;
  double clipped_left=fmax(left,0.0),clipped_top=fmax(top,0.0);
  double clipped_right=fmin(right,r->fbw),clipped_bottom=fmin(bottom,r->fbh);
  gml_render_flush_pending_fill(r);
  GmlRenderResource *target=gml_render_scene_target_resource_ensure(r);
  if(r->pending_fill || !target) return 0;
  GmlRenderImageRef image=gml_render_application_scene_next_image_ref(r,target);
  if(image.content_version==UINT64_MAX) return 0;
  operation.opcode=GML_RENDER_OP_TEXTURED_QUAD;
  operation.reconstruction_mode=GML_RENDER_DEVICE_CHECKPOINT;
  operation.scene_family=GML_RENDER_SCENE_SPRITE;
  operation.source_count=1u;operation.sources=&source.image;
  operation.target.image=image;operation.target.image.content_version++;
  operation.target.storage_version=1u;
  operation.target.width=(uint32_t)r->fbw;operation.target.height=(uint32_t)r->fbh;
  operation.target.pixel_format=GML_RENDER_PIXEL_ARGB8888;
  operation.target.region=(GmlRenderRect){0,0,(uint32_t)r->fbw,(uint32_t)r->fbh};
  GmlRenderQuad *quad=&operation.payload.quad;
  quad->shape=GML_RENDER_QUAD_AXIS_ALIGNED;quad->sampling.filter=GML_RENDER_FILTER_NEAREST;
  quad->sampling.edge_rule=GML_RENDER_EDGE_CLAMP_SUBIMAGE;
  quad->sampling.source_region=(GmlRenderRect){view->sx,view->sy,(uint32_t)view->sw,(uint32_t)view->sh};
  quad->x[0]=quad->x[3]=clipped_left;quad->x[1]=quad->x[2]=clipped_right;
  quad->y[0]=quad->y[1]=clipped_top;quad->y[2]=quad->y[3]=clipped_bottom;
  for(unsigned corner=0;corner<4u;corner++){
    quad->corner_colour[corner]=0xFFFFFFFFu;
    quad->u[corner]=((double)view->sx+((corner==1u || corner==2u)?clipped_right:clipped_left)-left)/source.width;
    quad->v[corner]=((double)view->sy+(corner>=2u?clipped_bottom:clipped_top)-top)/source.height;
  }
  state.write_mask=15u;state.colour=0xFFFFFFFFu;state.alpha=255u;state.sampler_count=1u;
  state.samplers[0]=quad->sampling;
  if(!page->scene_fully_opaque){
    state.rgb_source_factor=GML_RENDER_BLEND_SOURCE_ALPHA;
    state.rgb_destination_factor=GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA;
    state.alpha_source_factor=GML_RENDER_BLEND_ONE;
    state.alpha_destination_factor=GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA;
    state.rgb_equation=state.alpha_equation=GML_RENDER_BLEND_ADD;
  }
  if(!gml_render_application_scene_enqueue(r,&operation,&state,&source)) return 0;
  r->fb_all_transparent=0;r->fb_opaque_known=0;
  return 1;
}

void gml_draw_sprite_part_ext(GmlRender *r, int sprite, int subimg, double sx, double sy,
                              double sw, double sh, double x, double y,
                              double xs, double ys, uint32_t blend, double alpha){
  /* The ordinary CPU part leaf writes nothing at finite nonpositive alpha. Keep a
   * queued prefix resident, but do not bypass specialised shader or blend semantics. */
  if(r && r->resident_application_pass && isfinite(alpha) && alpha<=0.0 &&
     !(r->alphablend && (r->blendmode==6 ||
                        gml_render_blend_uses_destination_colour(r->blendmode))) &&
     sprite>=0 && sprite<r->n_spr &&
     (!r->gui_pass_active || resident_mapped_screen_gui_font(r)) &&
     r->active_shader<0 && !r->spr[sprite].spine && !r->spr[sprite].ns_enabled &&
     !gml_d3_is_active(r) && gml_render_scene_target_matches(r)) return;
  if(r && r->resident_application_pass &&
     isfinite(alpha) && alpha>0.0 && alpha<=1.0 && sprite>=0 && sprite<r->n_spr &&
     isfinite(sx) && isfinite(sy) && isfinite(sw) && isfinite(sh) &&
     sx>=0.0 && sy>=0.0 && sw>0.0 && sh>0.0 &&
     sx<=(double)INT_MAX && sy<=(double)INT_MAX &&
     sw<=(double)INT_MAX && sh<=(double)INT_MAX &&
     !gml_d3_is_active(r)){
    GmlSprite *resident_sprite=&r->spr[sprite];
    if(!resident_sprite->runtime_rgba && !resident_sprite->spine &&
       !resident_sprite->ns_enabled && resident_sprite->n_frames>0 &&
       resident_sprite->frame){
      int sub=gml_render_runtime_frame_index(resident_sprite,subimg);
      int page_index=resident_sprite->frame[sub];
      if(page_index>=0 && page_index<r->n_tpag){
        GmlTpag *page=&r->tpag[page_index],view;
        int source_x=0,source_y=0;
        double overlap_x0=0.0,overlap_y0=0.0,overlap_x1=0.0,overlap_y1=0.0;
        if(page->sw>0 && page->sh>0 && page->tx>=0 && page->ty>=0 &&
           page->tw==page->sw && page->th==page->sh &&
           page->bw==resident_sprite->w && page->bh==resident_sprite->h &&
           page->bw>0 && page->bh>0 &&
           (uint64_t)(uint32_t)page->tx+(uint32_t)page->sw<=(uint32_t)page->bw &&
           (uint64_t)(uint32_t)page->ty+(uint32_t)page->sh<=(uint32_t)page->bh &&
           gml_render_tpag_part_view(page,sx,sy,sw,sh,&view,&source_x,&source_y,
                                      &overlap_x0,&overlap_y0,
                                      &overlap_x1,&overlap_y1)){
          double mapped_x=x,mapped_y=y,mapped_xs=xs,mapped_ys=ys;
          double anchor_x=source_x,anchor_y=source_y;
          gml_render_draw_map_point(r,&mapped_x,&mapped_y);
          gml_render_draw_map_scale(r,&mapped_xs,&mapped_ys);
          double draw_xscale=mapped_xs,draw_yscale=mapped_ys;
          if(view.sw==1){
            anchor_x=overlap_x0;
            mapped_xs*=(overlap_x1-overlap_x0);
          }
          if(view.sh==1){
            anchor_y=overlap_y0;
            mapped_ys*=(overlap_y1-overlap_y0);
          }
          view.tw=view.sw;
          view.th=view.sh;
          /* The ephemeral crop need not rescan its pixels on every draw. Source-over is valid
           * whether the crop is fully opaque or not, and CPU still owns its original scan. */
          view.scene_opaque_checked=1;
          view.scene_fully_opaque=0;
          double left=mapped_x+(anchor_x-sx)*draw_xscale-r->cam_x;
          double top=mapped_y+(anchor_y-sy)*draw_yscale-r->cam_y;
          if(resident_modern_unit_sprite_part(r,page,&view,sx,sy,sw,sh,left,top,
               mapped_xs,mapped_ys,blend,alpha)) return;
          if(!r->interp && anygm_policy_has_modern_layer_semantics(r->win) &&
             r->target_id>0 && r->target_sp>0 &&
             floor(sx)==sx && floor(sy)==sy && floor(sw)==sw && floor(sh)==sh &&
             resident_atlas_crop_from_page(r,page,view.sx-page->sx,view.sy-page->sy,
               view.sw,view.sh,left,top,draw_xscale,draw_yscale,
               GML_RENDER_SCENE_SPRITE,blend,alpha,5,NULL)) return;
          /* R03-SPR-PART-OFFSCREEN-NOOP: the shared physical selection also
           * bounds single-column/row crops. A whole-pixel separation from the
           * target cannot write through the unchanged positive-unit CPU leaf. */
          if(!r->classic && !r->interp && !r->gui_pass_active && !r->fog_flat &&
             r->target_id<=0 && r->target_sp==0 &&
             anygm_policy_uses_first_generation_studio(r->win) &&
             r->active_shader<0 && !r->alpha_test_enable && r->alphablend &&
             !r->blendmode && r->blend_equation==1 && r->blend_equation_alpha==1 &&
             r->color_write_mask==15u && gml_render_scene_target_matches(r) &&
             blend==0xFFFFFFu && alpha==1.0 &&
             floor(sx)==sx && floor(sy)==sy && floor(sw)==sw && floor(sh)==sh &&
             mapped_xs==1.0 && mapped_ys==1.0 &&
             r->cam_x==floor(r->cam_x) && r->cam_y==floor(r->cam_y) &&
             isfinite(left) && isfinite(top) &&
             fabs(left)<=65536.0 && fabs(top)<=65536.0 &&
             view.sw<=65536 && view.sh<=65536 &&
             (left+(double)view.sw<=-1.0 || left>=(double)r->fbw+1.0 ||
              top+(double)view.sh<=-1.0 || top>=(double)r->fbh+1.0)) return;
          if(r->interp){
            if(gml_render_resident_filtered_atlas_page(
                 r,page_index,left,top,mapped_xs,mapped_ys,
                 GML_RENDER_SCENE_SPRITE,blend,alpha,&view,0)) return;
          } else if(floor(sx)==sx && floor(sy)==sy &&
                    floor(sw)==sw && floor(sh)==sh &&
                    fabs(mapped_xs)==1.0 && fabs(mapped_ys)==1.0 &&
                    view.sw>1 && view.sh>1 &&
                    isfinite(left) && isfinite(top) &&
                    fabs(left)<1000000.0 && fabs(top)<1000000.0 &&
                    resident_filtered_atlas_page(r,page_index,
                      floor(left+0.5),floor(top+0.5),mapped_xs,mapped_ys,
                      GML_RENDER_SCENE_SPRITE,blend,alpha,
                      &view,NULL,0.0,NULL,0,0,0,0,1)) return;
        }
      }
    }
  }
  if(r && r->resident_application_pass && !r->interp &&
     blend==0xFFFFFFu && alpha==1.0 &&
     sprite>=0 && sprite<r->n_spr &&
     isfinite(sx) && isfinite(sy) && isfinite(sw) && isfinite(sh) &&
     sx>=0.0 && sy>=0.0 && sw>0.0 && sh>0.0 &&
     sx<=(double)INT_MAX && sy<=(double)INT_MAX &&
     sw<=(double)INT_MAX && sh<=(double)INT_MAX &&
     floor(sx)==sx && floor(sy)==sy && floor(sw)==sw && floor(sh)==sh &&
     !gml_d3_is_active(r)){
    GmlSprite *resident_sprite=&r->spr[sprite];
    if(!resident_sprite->runtime_rgba && resident_sprite->n_frames>0 &&
       resident_sprite->frame){
      int sub=((subimg%resident_sprite->n_frames)+resident_sprite->n_frames)%
              resident_sprite->n_frames;
      int page_index=resident_sprite->frame[sub];
      if(page_index>=0 && page_index<r->n_tpag){
        GmlTpag *page=&r->tpag[page_index];
        if(page->tx==0 && page->ty==0 &&
           page->sw==resident_sprite->w && page->sh==resident_sprite->h &&
           page->bw==page->sw && page->bh==page->sh){
          double mapped_x=x,mapped_y=y,mapped_xs=xs,mapped_ys=ys;
          gml_render_draw_map_point(r,&mapped_x,&mapped_y);
          gml_render_draw_map_scale(r,&mapped_xs,&mapped_ys);
          if(gml_render_resident_atlas_crop(r,page_index,(int)sx,(int)sy,(int)sw,(int)sh,
                                            mapped_x-r->cam_x,mapped_y-r->cam_y,
                                            mapped_xs,mapped_ys,GML_RENDER_SCENE_SPRITE)) return;
        }
      }
    }
  }
  if(!gml_render_resident_application_cpu_barrier(r)) return;
  gml_render_draw_map_point(r,&x,&y);
  gml_render_draw_map_scale(r,&xs,&ys);
  if(sprite<0||sprite>=r->n_spr) return;
  if(gml_d3_draw_sprite_part_2d(r,sprite,subimg,sx,sy,sw,sh,x,y,xs,ys,blend,alpha)) return;
  GmlSprite *s=&r->spr[sprite]; if(s->n_frames<=0) return;
  /* A region of a sprite drawn through a program this renderer does not execute reaches the same
   * executor; unflipped only, because the composed plane is axis-aligned. */
  if(xs!=0 && ys!=0 && r->active_shader>=0 && r->shader_executor &&
     gml_render_shader_content_candidate(r,r->active_shader)){
    int sub=((subimg%s->n_frames)+s->n_frames)%s->n_frames;
    if(gml_render_shade_target_sprite_part(r,sprite,sub,sx,sy,sw,sh,
                                           x-r->cam_x,y-r->cam_y,sw*xs,sh*ys,blend,alpha)) return;
  }
  if(s->runtime_rgba){
    const uint8_t *fr=runtime_frame_rgba(s,subimg); if(!fr) return;
    gml_render_cpu_blit_rgba_region(r,fr,s->w,s->h,sx,sy,sw,sh,x-r->cam_x,y-r->cam_y,sw*xs,sh*ys,blend,alpha);
    return;
  }
  int sub=((subimg%s->n_frames)+s->n_frames)%s->n_frames;
  int ti=s->frame[sub]; if(ti<0||ti>=r->n_tpag) return;
  gml_render_cpu_blit_tpag_part(r,&r->tpag[ti],sx,sy,sw,sh,x-r->cam_x,y-r->cam_y,xs,ys,blend,alpha);
}

void gml_draw_sprite_general(GmlRender *r, int sprite, int subimg,
                             double sx, double sy, double sw, double sh,
                             double x, double y, double xs, double ys, double rotation,
                             const uint32_t corner_colour[4], double alpha){
  if(!r || !corner_colour) return;
  if(corner_colour[0]!=corner_colour[1] ||
     corner_colour[0]!=corner_colour[2] ||
     corner_colour[0]!=corner_colour[3]){
    if(sprite<0 || sprite>=r->n_spr) return;
    if(gml_d3_is_active(r) || r->spr[sprite].spine || r->spr[sprite].ns_enabled){
      gml_draw_sprite_part_ext(r,sprite,subimg,sx,sy,sw,sh,x,y,xs,ys,
                               corner_colour[0],alpha);
      return;
    }
    double mapped_x=x,mapped_y=y,mapped_xs=xs,mapped_ys=ys;
    gml_render_draw_map_point(r,&mapped_x,&mapped_y);
    gml_render_draw_map_scale(r,&mapped_xs,&mapped_ys);
    if(r->resident_application_pass && r->interp &&
       isfinite(sx) && isfinite(sy) && isfinite(sw) && isfinite(sh) &&
       isfinite(mapped_x) && isfinite(mapped_y) &&
       isfinite(mapped_xs) && isfinite(mapped_ys) &&
       isfinite(rotation) && isfinite(alpha) &&
       sx>=0.0 && sy>=0.0 && sw>0.0 && sh>0.0 &&
       sx<=INT_MAX && sy<=INT_MAX && sw<=INT_MAX && sh<=INT_MAX &&
       floor(sx)==sx && floor(sy)==sy && floor(sw)==sw && floor(sh)==sh){
      GmlSprite *record=&r->spr[sprite];
      if(!record->runtime_rgba && record->frame && record->n_frames>0){
        int frame=gml_render_runtime_frame_index(record,subimg);
        int page_index=record->frame[frame];
        if(page_index>=0 && page_index<r->n_tpag){
          GmlTpag view;
          int source_x=0,source_y=0;
          GmlTpag *page=&r->tpag[page_index];
          if(gml_render_tpag_part_view(page,sx,sy,sw,sh,
                                        &view,&source_x,&source_y,NULL,NULL,NULL,NULL)){
            GmlSprite part=*record;
            /* The general CPU leaf samples the entire physical atlas page, even when
             * the authored source selection ends earlier. Keep that sampler extent separate
             * from the selected rectangle which owns geometry and vertex-colour interpolation. */
            int64_t physical_x_end=(int64_t)page->tx+page->sw-(int64_t)sx;
            int64_t physical_y_end=(int64_t)page->ty+page->sh-(int64_t)sy;
            if(physical_x_end>INT_MAX || physical_y_end>INT_MAX ||
               (int64_t)page->tx-(int64_t)sx<INT_MIN ||
               (int64_t)page->ty-(int64_t)sy<INT_MIN) goto general_cpu;
            view.sx=page->sx;
            view.sy=page->sy;
            view.sw=page->sw;
            view.sh=page->sh;
            view.tx=page->tx-(int)sx;
            view.ty=page->ty-(int)sy;
            part.w=(int)sw;
            part.h=(int)sh;
            view.bw=physical_x_end>part.w?(int)physical_x_end:part.w;
            view.bh=physical_y_end>part.h?(int)physical_y_end:part.h;
            view.tw=view.sw;
            view.th=view.sh;
            part.originx=part.originy=0;
            view.scene_opaque_checked=0;
            if(resident_filtered_atlas_page(r,page_index,
                 mapped_x-r->cam_x,mapped_y-r->cam_y,mapped_xs,mapped_ys,
                 GML_RENDER_SCENE_SPRITE,corner_colour[0],alpha,
                 &view,&part,rotation,corner_colour,0,0,0,0,0)) return;
          }
        }
      }
    }
general_cpu:
    if(!gml_render_resident_application_cpu_barrier(r)) return;
    gml_render_cpu_blit_sprite_general_coloured(r,&r->spr[sprite],subimg,
                                                 sx,sy,sw,sh,mapped_x,mapped_y,
                                                 mapped_xs,mapped_ys,rotation,
                                                 corner_colour,alpha);
    return;
  }
  /* The selected region's rotation cannot use the axis part blit because it has no rotation
   * argument. Uniform corner colour retains the established part blit for an axis draw. */
  if(!isfinite(rotation) || fabs(fmod(rotation,360.0))<0.001){
    gml_draw_sprite_part_ext(r,sprite,subimg,sx,sy,sw,sh,x,y,xs,ys,
                             corner_colour[0],alpha);
    return;
  }
  if(sprite<0 || sprite>=r->n_spr || !isfinite(sx) || !isfinite(sy) ||
     !isfinite(sw) || !isfinite(sh) || !isfinite(x) || !isfinite(y) ||
     !isfinite(xs) || !isfinite(ys) || !isfinite(alpha) ||
     sx<0.0 || sy<0.0 || sw<=0.0 || sh<=0.0 || xs==0.0 || ys==0.0 ||
     sx>INT_MAX || sy>INT_MAX || sw>INT_MAX || sh>INT_MAX ||
     (alpha<=0.0 && !(r->alphablend &&
       gml_render_blend_uses_destination_colour(r->blendmode)))) return;
  if(gml_d3_is_active(r) || r->spr[sprite].spine || r->spr[sprite].ns_enabled){
    gml_draw_sprite_part_ext(r,sprite,subimg,sx,sy,sw,sh,x,y,xs,ys,
                             corner_colour[0],alpha);
    return;
  }
  double mapped_x=x,mapped_y=y,mapped_xs=xs,mapped_ys=ys;
  gml_render_draw_map_point(r,&mapped_x,&mapped_y);
  gml_render_draw_map_scale(r,&mapped_xs,&mapped_ys);
  if(r->resident_application_pass && r->interp &&
     corner_colour[0]==corner_colour[1] &&
     corner_colour[0]==corner_colour[2] &&
     corner_colour[0]==corner_colour[3] &&
     floor(sx)==sx && floor(sy)==sy && floor(sw)==sw && floor(sh)==sh){
    GmlSprite *sprite_record=&r->spr[sprite];
    if(!sprite_record->runtime_rgba && sprite_record->frame &&
       sprite_record->n_frames>0){
      int sub=gml_render_runtime_frame_index(sprite_record,subimg);
      int page_index=sprite_record->frame[sub];
      if(page_index>=0 && page_index<r->n_tpag){
        GmlTpag view;
        int source_x=0,source_y=0;
        if(gml_render_tpag_part_view(&r->tpag[page_index],sx,sy,sw,sh,
                                      &view,&source_x,&source_y,NULL,NULL,NULL,NULL)){
          GmlSprite part=*sprite_record;
          part.w=view.bw=view.tw=view.sw;
          part.h=view.bh=view.th=view.sh;
          part.originx=part.originy=0;
          view.scene_opaque_checked=0;
          double cosine,sine;
          render_rotation_sincos(rotation,&cosine,&sine);
          double offset_x=(source_x-sx)*mapped_xs;
          double offset_y=(source_y-sy)*mapped_ys;
          if(resident_filtered_atlas_page(r,page_index,
               mapped_x+offset_x*cosine+offset_y*sine-r->cam_x,
               mapped_y-offset_x*sine+offset_y*cosine-r->cam_y,
               mapped_xs,mapped_ys,GML_RENDER_SCENE_SPRITE,
               corner_colour[0],alpha,&view,&part,rotation,NULL,0,0,0,0,0)) return;
        }
      }
    }
  }
  if(!gml_render_resident_application_cpu_barrier(r)) return;
  gml_render_cpu_blit_sprite_general_rotated(r,&r->spr[sprite],subimg,
                                             sx,sy,sw,sh,mapped_x,mapped_y,
                                             mapped_xs,mapped_ys,rotation,
                                             corner_colour[0],alpha);
}

/* A complete, untrimmed nearest atlas cell can repeat with the CPU tile phase.
 * Unit-scale cells clipped by the application edge select the same physical texels;
 * trimmed, filtered and very dense repetitions retain the original CPU loop. */
int gml_render_resident_tiled_sprite(GmlRender *r,int sprite,int subimg,
                                     double x,double y,double xs,double ys,
                                     uint32_t blend,double alpha){
  if(!r || !r->resident_application_pass || r->interp ||
     sprite<0 || sprite>=r->n_spr || !isfinite(x) || !isfinite(y) ||
     !isfinite(xs) || !isfinite(ys) || xs!=1.0 || ys!=1.0 ||
     blend!=0xFFFFFFu || alpha!=1.0 || gml_d3_is_active(r)) return 0;
  gml_render_draw_map_point(r,&x,&y);
  gml_render_draw_map_scale(r,&xs,&ys);
  if(xs!=1.0 || ys!=1.0 || !isfinite(x) || !isfinite(y) ||
     !isfinite(r->cam_x) || !isfinite(r->cam_y) ||
     floor(r->cam_x)!=r->cam_x || floor(r->cam_y)!=r->cam_y) return 0;
  GmlSprite *s=&r->spr[sprite];
  if(s->runtime_rgba || s->n_frames<=0 || !s->frame || s->w<=0 || s->h<=0 ||
     r->fbw<=0 || r->fbh<=0) return 0;
  int sub=((subimg%s->n_frames)+s->n_frames)%s->n_frames;
  int page_index=s->frame[sub];
  if(page_index<0 || page_index>=r->n_tpag) return 0;
  GmlTpag *page=&r->tpag[page_index];
  if(page->sw!=s->w || page->sh!=s->h || page->tx || page->ty ||
     page->bw!=s->w || page->bh!=s->h) return 0;
  double ax=floor(x-r->cam_x), ay=floor(y-r->cam_y);
  if(!isfinite(ax) || !isfinite(ay)) return 0;
  double x0=fmod(ax,(double)s->w), y0=fmod(ay,(double)s->h);
  if(x0>0.0) x0-=s->w;
  if(y0>0.0) y0-=s->h;
  int start_x=(int)x0, start_y=(int)y0;
  uint64_t span_x=(uint64_t)(uint32_t)r->fbw+(uint32_t)(-start_x);
  uint64_t span_y=(uint64_t)(uint32_t)r->fbh+(uint32_t)(-start_y);
  uint64_t count_x=(span_x+(uint32_t)s->w-1u)/(uint32_t)s->w;
  uint64_t count_y=(span_y+(uint32_t)s->h-1u)/(uint32_t)s->h;
  if(count_x*count_y>256u) return 0;
  for(double yy=y0;yy<r->fbh;yy+=s->h)
    for(double xx=x0;xx<r->fbw;xx+=s->w)
      if(!gml_render_resident_atlas_page(r,page_index,xx,yy,1.0,1.0,
                                         GML_RENDER_SCENE_SPRITE)) return 0;
  return 1;
}

/* draw_sprite_tiled_ext: repeat a sprite frame to fill the screen (both axes),
 * anchored at (x,y). Used by GML effects and tiled background scripts. */
void gml_render_sprite_tiled_ext_unmasked(GmlRender *r, int sprite, int subimg,
                                           double x, double y, double xs, double ys,
                                           uint32_t blend, double alpha){
  gml_render_draw_map_point(r,&x,&y);
  gml_render_draw_map_scale(r,&xs,&ys);
  if(sprite<0||sprite>=r->n_spr) return;
  GmlSprite *s=&r->spr[sprite]; if(s->n_frames<=0) return;
  if(s->runtime_rgba){
    int sub=gml_render_runtime_frame_index(s,subimg);
    const uint8_t *fr=runtime_frame_rgba(s,subimg); if(!fr) return;
    const int *rmin=s->runtime_row_min?s->runtime_row_min+(size_t)sub*s->h:NULL;
    const int *rmax=s->runtime_row_max?s->runtime_row_max+(size_t)sub*s->h:NULL;
    double bw=s->w*fabs(xs), bh=s->h*fabs(ys); if(bw<=0||bh<=0) return;
    /* Tiled sprite coordinates anchor the logical cell itself.  Unlike draw_sprite(), the
     * sprite origin must not shift the repeat phase: an x equal to one full cell width starts
     * the next cell at the view origin.  The target offsets below still place trimmed pixels
     * inside each full sprite-sized cell. */
    double ax=floor(x-r->cam_x);
    double ay=floor(y-r->cam_y);
    double x0=fmod(ax,bw); if(x0>0) x0-=bw;
    double y0=fmod(ay,bh); if(y0>0) y0-=bh;
    if(gml_d3_is_active(r)){
      for(double yy=y0; yy<r->fbh; yy+=bh) for(double xx=x0; xx<r->fbw; xx+=bw)
        gml_d3_draw_sprite_2d(r,sprite,subimg,xx+r->cam_x+s->originx*xs,
          yy+r->cam_y+s->originy*ys,xs,ys,0,blend,alpha);
      return;
    }
    for(double yy=y0; yy<r->fbh; yy+=bh)
      for(double xx=x0; xx<r->fbw; xx+=bw)
        blit_rgba_sprite(r,s,fr,s->w,s->h,xx,yy,xs,ys,0,0,0,blend,alpha,0,rmin,rmax,s->runtime_opaque);
    return;
  }
  int sub=((subimg%s->n_frames)+s->n_frames)%s->n_frames;
  int ti=s->frame[sub]; if(ti<0||ti>=r->n_tpag) return;
  GmlTpag *t=&r->tpag[ti];
  /* The tiling PERIOD is the sprite's FULL cell (s->w/s->h), not the trimmed TPAG source, and
   * the trimmed content sits at the TPAG target offset within each cell. Tiling the trimmed crop
   * instead of the logical cell repeats transparent padding incorrectly and can flood the target. */
  double bw=s->w*fabs(xs), bh=s->h*fabs(ys); if(bw<=0||bh<=0) return;
  double ax=floor(x-r->cam_x);
  double ay=floor(y-r->cam_y);
  /* The first-generation profile uses a half-pixel correction for tiled sprite cells when the
   * camera is fractional: integer camera positions retain the authored phase, while a non-integer
   * position selects the following cell sample. Snapping only after the subtraction loses that
   * distinction and moves a screen-space mask one column relative to ordinary world sprites. */
  if(r->win && anygm_policy_uses_first_generation_studio(r->win) &&
     fabs(r->cam_x-nearbyint(r->cam_x))>1e-9)
    ax+=1.0;
  double x0=fmod(ax,bw); if(x0>0) x0-=bw;
  double y0=fmod(ay,bh); if(y0>0) y0-=bh;
  if(gml_d3_is_active(r)){
    for(double yy=y0; yy<r->fbh; yy+=bh) for(double xx=x0; xx<r->fbw; xx+=bw)
      gml_d3_draw_sprite_2d(r,sprite,subimg,xx+r->cam_x+s->originx*xs,
        yy+r->cam_y+s->originy*ys,xs,ys,0,blend,alpha);
    return;
  }
  if(gml_render_sprite_log_enabled(r))
    anygm_host_logf(r && r->win ? r->win->host : NULL,ANYGM_LOG_DEBUG,"[spr-tiled] spr=%d sub=%d cell=%.0fx%.0f anchor=(%.0f,%.0f) start=(%.0f,%.0f) t=(%d,%d %dx%d) a=%.2f\n",
      sprite,sub,bw,bh,ax,ay,x0,y0,t->tx,t->ty,t->sw,t->sh,alpha);
  for(double yy=y0; yy<r->fbh; yy+=bh)
    for(double xx=x0; xx<r->fbw; xx+=bw)
	      gml_render_cpu_blit_with_phase(r,t,xx+t->tx*xs,yy+t->ty*ys,xs,ys,blend,alpha,0,0);
}

/* R03-LAYER-TRIMMED-UNIT: prepare the entire immutable cell run before enqueue.
 * Logical repeat periods and physical trim are independent; clipped cells do
 * not force a prefix flush or acquire destination pixels. */
static int resident_modern_trimmed_unit_layer(GmlRender *r,GmlSprite *sprite,int subimg,
  double x,double y,double xs,double ys,uint32_t colour,double alpha,int htiled,int vtiled){
  enum { CELL_LIMIT=256 };
  GmlRenderResourceSource source={0};
  GmlRenderDrawStateSnapshot state={0};
  GmlRenderDrawOp *operations=NULL;
  GmlRenderResource *target;
  const GmlRenderDeviceServices *device=gml_render_device(r);
  int accepted=0;
  uint32_t count=0;
  if(!r || !sprite || !device || !device->observe_on_demand ||
     !device->observe_on_demand(device->context) || !r->resident_application_pass ||
     r->classic || r->interp || !anygm_policy_has_modern_layer_semantics(r->win) ||
     r->gui_pass_active || r->fog_flat || gml_d3_is_active(r) || r->active_shader>=0 ||
     r->alpha_test_enable || !r->alphablend || r->blendmode || r->blend_equation!=1 ||
     r->blend_equation_alpha!=1 || r->color_write_mask!=15u || r->classic_phase_y ||
     r->rotated_batch_count || r->rotated_batch_building || r->pending_underlay ||
     r->pending_presentation || !gml_render_scene_target_matches(r) ||
     xs!=1.0 || ys!=1.0 || colour!=0xFFFFFFu || alpha!=1.0 ||
     !isfinite(x) || !isfinite(y) || !isfinite(r->cam_x) || !isfinite(r->cam_y) ||
     sprite->runtime_rgba || sprite->spine || sprite->ns_enabled || !sprite->frame ||
     sprite->n_frames<=0 || sprite->w<=0 || sprite->h<=0 ||
     sprite->w>4096 || sprite->h>4096 || r->fbw<=0 || r->fbh<=0 ||
     r->fbw>16384 || r->fbh>16384) return 0;
  for(int index=0;index<3;index++)
    if(r->classic_interp_phase[index] || r->app_interp_phase[index]) return 0;
  int page_index=sprite->frame[gml_render_runtime_frame_index(sprite,subimg)];
  if(page_index<0 || page_index>=r->n_tpag) return 0;
  GmlTpag *page=&r->tpag[page_index];
  if((!page->tx && !page->ty) || page->tx<0 || page->ty<0 || page->project_authored_edges ||
     page->bw!=sprite->w || page->bh!=sprite->h || page->sw<=0 || page->sh<=0 ||
     page->tw!=page->sw || page->th!=page->sh || page->sx<0 || page->sy<0 ||
     (int64_t)page->tx+page->sw>sprite->w || (int64_t)page->ty+page->sh>sprite->h ||
     (uint64_t)(uint32_t)page->sw*(uint32_t)page->sh>65536u ||
     !gml_render_atlas_scene_source_view(r,page->atlas,&source) || !source.borrowed_immutable ||
     source.width>16384u || source.height>16384u || r->atlas[page->atlas].runtime_font_page ||
     (uint64_t)(uint32_t)page->sx+(uint32_t)page->sw>source.width ||
     (uint64_t)(uint32_t)page->sy+(uint32_t)page->sh>source.height) return 0;
  double ax=round(x-r->cam_x),ay=round(y-r->cam_y),x0=ax,y0=ay;
  if(!isfinite(ax) || !isfinite(ay) || fabs(ax)>65536.0 || fabs(ay)>65536.0) return 0;
  if(htiled){x0=fmod(ax,sprite->w);if(x0>0.0) x0-=sprite->w;}
  if(vtiled){y0=fmod(ay,sprite->h);if(y0>0.0) y0-=sprite->h;}
  double xe=htiled?r->fbw:x0+sprite->w,ye=vtiled?r->fbh:y0+sprite->h;
  double nx=htiled?ceil((xe-x0)/sprite->w):1.0;
  double ny=vtiled?ceil((ye-y0)/sprite->h):1.0;
  if(nx<1.0 || ny<1.0 || nx>CELL_LIMIT || ny>CELL_LIMIT || nx*ny>CELL_LIMIT) return 0;
  resident_classify_page(page,&r->atlas[page->atlas]);
  if(!page->scene_binary_alpha) return 0;
  operations=calloc((size_t)(nx*ny),sizeof *operations);
  if(!operations) return 0;
  for(double yy=y0;yy<ye;yy+=sprite->h) for(double xx=x0;xx<xe;xx+=sprite->w){
    double left=xx+page->tx,top=yy+page->ty;
    double right=left+page->sw,bottom=top+page->sh;
    if(left>=r->fbw || top>=r->fbh || right<=0.0 || bottom<=0.0) continue;
    double clipped_left=fmax(left,0.0),clipped_top=fmax(top,0.0);
    double clipped_right=fmin(right,r->fbw),clipped_bottom=fmin(bottom,r->fbh);
    GmlRenderDrawOp *op=&operations[count++];
    op->opcode=GML_RENDER_OP_TEXTURED_QUAD;op->reconstruction_mode=GML_RENDER_DEVICE_CHECKPOINT;
    op->scene_family=GML_RENDER_SCENE_SPRITE;op->source_count=1u;op->sources=&source.image;
    op->target.storage_version=1u;op->target.width=(uint32_t)r->fbw;op->target.height=(uint32_t)r->fbh;
    op->target.pixel_format=GML_RENDER_PIXEL_ARGB8888;
    op->target.region=(GmlRenderRect){0,0,(uint32_t)r->fbw,(uint32_t)r->fbh};
    GmlRenderQuad *quad=&op->payload.quad;
    quad->shape=GML_RENDER_QUAD_AXIS_ALIGNED;quad->sampling.filter=GML_RENDER_FILTER_NEAREST;
    quad->sampling.edge_rule=GML_RENDER_EDGE_CLAMP_SUBIMAGE;
    quad->sampling.source_region=(GmlRenderRect){page->sx,page->sy,(uint32_t)page->sw,(uint32_t)page->sh};
    quad->x[0]=quad->x[3]=clipped_left;quad->x[1]=quad->x[2]=clipped_right;
    quad->y[0]=quad->y[1]=clipped_top;quad->y[2]=quad->y[3]=clipped_bottom;
    for(unsigned corner=0;corner<4u;corner++){
      quad->corner_colour[corner]=0xFFFFFFFFu;
      quad->u[corner]=((double)page->sx+((corner==1u || corner==2u)?clipped_right:clipped_left)-left)/source.width;
      quad->v[corner]=((double)page->sy+(corner>=2u?clipped_bottom:clipped_top)-top)/source.height;
    }
  }
  if(!count){accepted=1;goto done;}
  gml_render_flush_pending_fill(r);
  if(r->pending_fill || !(target=gml_render_scene_target_resource_ensure(r))) goto done;
  GmlRenderImageRef image=gml_render_application_scene_next_image_ref(r,target);
  if(image.content_version>UINT64_MAX-count) goto done;
  for(uint32_t index=0;index<count;index++){
    operations[index].target.image=image;
    operations[index].target.image.content_version+=index+1u;
  }
  state.write_mask=15u;state.colour=0xFFFFFFFFu;state.alpha=255u;state.sampler_count=1u;
  state.samplers[0]=operations[0].payload.quad.sampling;
  if(!page->scene_fully_opaque){
    state.rgb_source_factor=GML_RENDER_BLEND_SOURCE_ALPHA;
    state.rgb_destination_factor=GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA;
    state.alpha_source_factor=GML_RENDER_BLEND_ONE;
    state.alpha_destination_factor=GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA;
    state.rgb_equation=state.alpha_equation=GML_RENDER_BLEND_ADD;
  }
  accepted=gml_render_application_scene_enqueue_quads(r,operations,count,&state,&source);
  if(accepted){r->fb_all_transparent=0;r->fb_opaque_known=0;}
done:
  free(operations);return accepted;
}

/* Background layers are not sprite instances.  Their x/y is the top-left of the logical sprite
 * cell, irrespective of the sprite's authored origin, and horizontal/vertical tiling are separate
 * switches.  Routing them through draw_sprite_tiled_ext used the instance origin and repeated on
 * both axes, which displaced parallax art and introduced phantom copies on the untiled axis. */
void gml_draw_layer_background_sprite(GmlRender *r, int sprite, int subimg, double x, double y,
                                      double xs, double ys, uint32_t blend, double alpha,
                                      int htiled, int vtiled){
  gml_render_draw_map_point(r,&x,&y);
  gml_render_draw_map_scale(r,&xs,&ys);
  if(!r || sprite<0 || sprite>=r->n_spr ||
     (alpha<=0 && !(r->alphablend &&
       gml_render_blend_uses_destination_colour(r->blendmode)))) return;
  GmlSprite *s=&r->spr[sprite]; if(s->n_frames<=0) return;
  double bw=s->w*fabs(xs), bh=s->h*fabs(ys); if(bw<=0 || bh<=0) return;
  if(resident_modern_trimmed_unit_layer(r,s,subimg,x,y,xs,ys,blend,alpha,htiled,vtiled)) return;
  if(r->resident_application_pass && !r->interp &&
     ((!htiled && !vtiled) || (xs==1.0 && ys==1.0)) &&
     xs>0.0 && ys>0.0 && blend==0xFFFFFFu && alpha==1.0 &&
     !s->runtime_rgba && s->frame &&
     !gml_d3_is_active(r)){
    int sub=((subimg%s->n_frames)+s->n_frames)%s->n_frames;
    int page_index=s->frame[sub];
    if(page_index>=0 && page_index<r->n_tpag){
      GmlTpag *page=&r->tpag[page_index];
      if(page->tx==0 && page->ty==0 &&
         page->sw==s->w && page->sh==s->h &&
         page->bw==page->sw && page->bh==page->sh){
        if(!htiled && !vtiled){
          if(gml_render_resident_atlas_page(r,page_index,
               round(x-r->cam_x),round(y-r->cam_y),xs,ys,
               GML_RENDER_SCENE_SPRITE)) return;
        } else {
          double ax=round(x-r->cam_x), ay=round(y-r->cam_y);
          double x0=ax, y0=ay;
          if(htiled){ x0=fmod(ax,bw); if(x0>0.0) x0-=bw; }
          if(vtiled){ y0=fmod(ay,bh); if(y0>0.0) y0-=bh; }
          double xend=htiled?r->fbw:x0+bw;
          double yend=vtiled?r->fbh:y0+bh;
          if(isfinite(x0) && isfinite(y0) && isfinite(xend) && isfinite(yend) &&
             xend>x0 && yend>y0){
            double count_x=htiled?ceil((xend-x0)/bw):1.0;
            double count_y=vtiled?ceil((yend-y0)/bh):1.0;
            /* Isolate the bounded run so queue-capacity flush cannot split its cells. */
            if(count_x<=256.0 && count_y<=256.0 && count_x*count_y<=256.0 &&
               gml_render_application_scene_flush(r)){
              int accepted=1;
              for(double yy=y0;yy<yend && accepted;yy+=bh)
                for(double xx=x0;xx<xend;xx+=bw)
                  if(!gml_render_resident_atlas_page(r,page_index,xx,yy,1.0,1.0,
                                                     GML_RENDER_SCENE_SPRITE)){
                    accepted=0;
                    break;
                  }
              if(accepted) return;
            }
          }
        }
      }
    }
  }
  if(!gml_render_resident_application_cpu_barrier(r)) return;
  /* Snap camera-relative background-layer vertices to the nearest output pixel. */
  double ax=round(x-r->cam_x), ay=round(y-r->cam_y);
  double x0=ax, y0=ay;
  if(htiled){ x0=fmod(ax,bw); if(x0>0) x0-=bw; }
  if(vtiled){ y0=fmod(ay,bh); if(y0>0) y0-=bh; }
  double xend=htiled ? r->fbw : x0+bw;
  double yend=vtiled ? r->fbh : y0+bh;
  if(s->runtime_rgba){
    int sub=gml_render_runtime_frame_index(s,subimg);
    const uint8_t *fr=runtime_frame_rgba(s,subimg); if(!fr) return;
    const int *rmin=s->runtime_row_min?s->runtime_row_min+(size_t)sub*s->h:NULL;
    const int *rmax=s->runtime_row_max?s->runtime_row_max+(size_t)sub*s->h:NULL;
    if(gml_d3_is_active(r)){
      for(double yy=y0;yy<yend;yy+=bh) for(double xx=x0;xx<xend;xx+=bw)
        gml_d3_draw_sprite_2d(r,sprite,subimg,xx+r->cam_x+s->originx*xs,
          yy+r->cam_y+s->originy*ys,xs,ys,0,blend,alpha);
      return;
    }
    for(double yy=y0;yy<yend;yy+=bh) for(double xx=x0;xx<xend;xx+=bw)
      blit_rgba_sprite(r,s,fr,s->w,s->h,xx,yy,xs,ys,0,0,0,blend,alpha,0,
                       rmin,rmax,s->runtime_opaque);
    return;
  }
  int sub=((subimg%s->n_frames)+s->n_frames)%s->n_frames;
  int ti=s->frame[sub]; if(ti<0 || ti>=r->n_tpag) return;
  GmlTpag *t=&r->tpag[ti];
  if(gml_render_sprite_log_enabled(r))
    anygm_host_logf(r && r->win ? r->win->host : NULL,ANYGM_LOG_DEBUG,"[layer-bg-spr] %s subimg=%d sub=%d tpag=%d src=%d,%d %dx%d dst=%d,%d base=%dx%d atlas=%d pos=%.3f,%.3f scale=%.3f/%.3f tiled=%d/%d alpha=%.3f\n",
      s->name?s->name:"?",subimg,sub,ti,t->sx,t->sy,t->sw,t->sh,t->tx,t->ty,t->bw,t->bh,
      t->atlas,x,y,xs,ys,htiled,vtiled,alpha);
  if(gml_d3_is_active(r)){
    for(double yy=y0;yy<yend;yy+=bh) for(double xx=x0;xx<xend;xx+=bw)
      gml_d3_draw_sprite_2d(r,sprite,subimg,xx+r->cam_x+s->originx*xs,
        yy+r->cam_y+s->originy*ys,xs,ys,0,blend,alpha);
    return;
  }
  for(double yy=y0;yy<yend;yy+=bh) for(double xx=x0;xx<xend;xx+=bw)
    gml_render_cpu_blit_with_phase(r,t,xx+t->tx*xs,yy+t->ty*ys,xs,ys,blend,alpha,0,0);
}
