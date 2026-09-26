/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Shared sprite source and subrectangle preparation. */
#include "gml_render.h"
#include "gml_render_internal.h"
#include "gml_render_sprites_internal.h"
#include "gml_render_surfaces_internal.h"
#include "gml_render_cpu_blit.h"
#include "anygm_host.h"

#include <math.h>
#include <stddef.h>

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

/* A unit-scale, integer-aligned atlas crop has identity sampling even when the sampler is set
 * to interpolate. Authored trim offsets move the visible crop, not its texels. Binary-alpha
 * crops use endpoint source-over; intermediate-alpha, scaled, fractional and programmed cases
 * remain with the established CPU blit. */
static int resident_identity_sprite(GmlRender *r,GmlSprite *sprite,int subimg,
                                  double x,double y,double xs,double ys,double rotation,
                                  uint32_t blend,double alpha){
  GmlTpag *page;
  GmlAtlas *atlas;
  GmlRenderResourceSource source={0};
  GmlRenderResource *target;
  GmlRenderDrawOp operation={0};
  GmlRenderDrawStateSnapshot state={0};
  GmlRenderDrawBatch batch;
  GmlRenderImageRef source_ref,image;
  GmlRenderRect region;
  double left,top;
  int sub,page_index;
  if(!r || !r->resident_application_pass || !sprite || sprite->runtime_rgba ||
     sprite->spine || sprite->ns_enabled || sprite->n_frames<=0 ||
     sprite->w<=0 || sprite->h<=0 ||
     r->classic || (r->world_transform_active &&
                    (r->world_scale_x!=1.0 || r->world_scale_y!=1.0)) ||
     r->gui_pass_active ||
     r->active_shader>=0 || r->alpha_test_enable || !r->alphablend ||
     r->blendmode || r->blend_equation!=1 || r->blend_equation_alpha!=1 ||
     r->color_write_mask!=0x0Fu || r->classic_phase_y ||
     r->rotated_batch_count || r->rotated_batch_building ||
     r->pending_underlay || r->pending_presentation ||
     r->fb!=r->app_surface_owned || r->fbw!=r->app_w || r->fbh!=r->app_h ||
     xs!=1.0 || ys!=1.0 || rotation!=0.0 || blend!=0xFFFFFFu || alpha!=1.0)
    return 0;
  /* A plain sprite reads only the base image; the other texture-stage filter flags do not
   * participate in the CPU blit, and the integer-aligned unit-scale base sample is exact. */
  for(int index=0;index<3;index++)
    if(r->classic_interp_phase[index] || r->app_interp_phase[index]) return 0;
  sub=gml_render_runtime_frame_index(sprite,subimg);
  if(!sprite->frame) return 0;
  page_index=sprite->frame[sub];
  if(page_index<0 || page_index>=r->n_tpag) return 0;
  page=&r->tpag[page_index];
  if(page->sw<=0 || page->sh<=0 || page->sx<0 || page->sy<0 ||
     page->tx<0 || page->ty<0 ||
     (uint64_t)(uint32_t)page->tx+(uint32_t)page->sw>(uint32_t)sprite->w ||
     (uint64_t)(uint32_t)page->ty+(uint32_t)page->sh>(uint32_t)sprite->h ||
     page->tw!=page->sw || page->th!=page->sh ||
     page->bw!=sprite->w || page->bh!=sprite->h ||
     page->atlas<0 || page->atlas>=r->n_atlas) return 0;
  atlas=&r->atlas[page->atlas];
  if(!gml_render_atlas_scene_source_view(r,page->atlas,&source) ||
     (uint64_t)(uint32_t)page->sx+(uint32_t)page->sw>source.width ||
     (uint64_t)(uint32_t)page->sy+(uint32_t)page->sh>source.height) return 0;
  if(!page->scene_opaque_checked){
    page->scene_fully_opaque=1;
    page->scene_binary_alpha=1;
    for(int row=0;row<page->sh;row++)
      for(int column=0;column<page->sw;column++){
        uint8_t coverage=atlas->px[((size_t)(page->sy+row)*atlas->w+page->sx+column)*4u+3u];
        if(coverage!=255u) page->scene_fully_opaque=0;
        if(coverage!=0u && coverage!=255u) page->scene_binary_alpha=0;
      }
    page->scene_opaque_checked=1;
  }
  if(!page->scene_binary_alpha) return 0;
  left=x-(double)sprite->originx+(double)page->tx-r->cam_x;
  top=y-(double)sprite->originy+(double)page->ty-r->cam_y;
  if(!isfinite(left) || !isfinite(top) || left<0.0 || top<0.0 ||
     left+page->sw>r->fbw || top+page->sh>r->fbh ||
     left!=(double)(int)left || top!=(double)(int)top) return 0;
  gml_render_flush_pending_fill(r);
  if(r->pending_fill || !(target=gml_render_application_resource_ensure(r))) return 0;
  image=gml_render_resource_image_ref(target);
  if(image.content_version==UINT64_MAX) return 0;
  region=(GmlRenderRect){page->sx,page->sy,(uint32_t)page->sw,(uint32_t)page->sh};
  source_ref=source.image;
  operation.opcode=GML_RENDER_OP_TEXTURED_QUAD;
  operation.reconstruction_mode=GML_RENDER_REPLAY_EXACT;
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
  operation.payload.quad.sampling.filter=GML_RENDER_FILTER_NEAREST;
  operation.payload.quad.sampling.edge_rule=GML_RENDER_EDGE_CLAMP_SUBIMAGE;
  operation.payload.quad.sampling.source_region=region;
  for(int corner=0;corner<4;corner++) operation.payload.quad.corner_colour[corner]=0xFFFFFFFFu;
  operation.payload.quad.x[0]=operation.payload.quad.x[3]=left;
  operation.payload.quad.x[1]=operation.payload.quad.x[2]=left+page->sw;
  operation.payload.quad.y[0]=operation.payload.quad.y[1]=top;
  operation.payload.quad.y[2]=operation.payload.quad.y[3]=top+page->sh;
  operation.payload.quad.u[0]=operation.payload.quad.u[3]=(double)page->sx/source.width;
  operation.payload.quad.u[1]=operation.payload.quad.u[2]=(double)(page->sx+page->sw)/source.width;
  operation.payload.quad.v[0]=operation.payload.quad.v[1]=(double)page->sy/source.height;
  operation.payload.quad.v[2]=operation.payload.quad.v[3]=(double)(page->sy+page->sh)/source.height;
  state.write_mask=15u;
  state.colour=0xFFFFFFFFu;
  state.alpha=255u;
  if(!page->scene_fully_opaque){
    state.rgb_source_factor=GML_RENDER_BLEND_SOURCE_ALPHA;
    state.rgb_destination_factor=GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA;
    state.alpha_source_factor=GML_RENDER_BLEND_ONE;
    state.alpha_destination_factor=GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA;
    state.rgb_equation=GML_RENDER_BLEND_ADD;
    state.alpha_equation=GML_RENDER_BLEND_ADD;
  }
  state.sampler_count=1u;
  state.samplers[0]=operation.payload.quad.sampling;
  batch=(GmlRenderDrawBatch){&operation,1u,&state,1u};
  if(gml_render_application_scene_submit(r,&batch,&source,1u)==
     GML_RENDER_RESOURCE_EXEC_FAILED) return 0;
  r->fb_all_transparent=0;
  return 1;
}

void gml_draw_sprite(GmlRender *r, int sprite, int subimg, double x, double y){
  gml_draw_sprite_ext(r,sprite,subimg,x,y,1,1,0,0xFFFFFF,
                      r && !r->classic ? r->alpha : 1.0);
}

void gml_render_sprite_ext_unmasked(GmlRender *r, int sprite, int subimg, double x, double y,
                                     double xs, double ys, double rot, uint32_t blend, double alpha){
  gml_render_draw_map_point(r,&x,&y);
  gml_render_draw_map_scale(r,&xs,&ys);
  if(r && r->resident_application_pass){
    if(sprite>=0 && sprite<r->n_spr &&
       resident_identity_sprite(r,&r->spr[sprite],subimg,x,y,xs,ys,rot,blend,alpha)) return;
    if(!gml_render_resident_application_cpu_barrier(r)) return;
  }
  if(r && sprite>=0 && sprite<r->n_spr &&
     gml_render_cpu_draw_spine_sprite(r,&r->spr[sprite],x,y,xs,ys,rot,blend,alpha)) return;
  if(gml_d3_draw_sprite_2d(r,sprite,subimg,x,y,xs,ys,rot,blend,alpha)) return;
  if(sprite<0||sprite>=r->n_spr) return;
  if(alpha<=0) return;
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

void gml_draw_sprite_stretched(GmlRender *r, int sprite, int frame, double dx, double dy, double dw, double dh, uint32_t blend, double alpha){
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

void gml_draw_sprite_pos(GmlRender *r,int sprite,int subimg,
                         const double x[4],const double y[4],double alpha){
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
  if(!r||!r->fb||sprite<0||sprite>=r->n_spr||alpha<=0) return;
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

void gml_draw_sprite_part_ext(GmlRender *r, int sprite, int subimg, double sx, double sy,
                              double sw, double sh, double x, double y,
                              double xs, double ys, uint32_t blend, double alpha){
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

/* Background layers are not sprite instances.  Their x/y is the top-left of the logical sprite
 * cell, irrespective of the sprite's authored origin, and horizontal/vertical tiling are separate
 * switches.  Routing them through draw_sprite_tiled_ext used the instance origin and repeated on
 * both axes, which displaced parallax art and introduced phantom copies on the untiled axis. */
void gml_draw_layer_background_sprite(GmlRender *r, int sprite, int subimg, double x, double y,
                                      double xs, double ys, uint32_t blend, double alpha,
                                      int htiled, int vtiled){
  gml_render_draw_map_point(r,&x,&y);
  gml_render_draw_map_scale(r,&xs,&ys);
  if(!r || sprite<0 || sprite>=r->n_spr || alpha<=0) return;
  GmlSprite *s=&r->spr[sprite]; if(s->n_frames<=0) return;
  double bw=s->w*fabs(xs), bh=s->h*fabs(ys); if(bw<=0 || bh<=0) return;
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
