/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* CPU shader capture, host handoff, and shaded-plane composition. */
#include "gml_render_cpu_shader_pass.h"
#include "gml_render_cpu_backend.h"
#include "gml_render_internal.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* draw_surface_stretched[_ext]: blit a runtime surface into the current target, box-averaged. */
/* A surface drawn through a program this renderer does not execute, somewhere other than the
 * frame's terminal presentation: the host runs the program over the surface at the destination's
 * size, and the result is composed here as a surface of that size, with the blend, alpha and
 * flips the draw asked for. Returns 0 when the host could not, and the draw proceeds unshaded. */
/* Run the content's active program over a destination through the host's executor and compose the
 * result at the current target with the given blend, alpha and flips. `source` is the picture the
 * program samples (its region names the part shown); a procedural program ignores it. `dx,dy` are
 * mapped draw coordinates: the request subtracts the camera to locate the target rectangle,
 * while the position attribute retains the draw coordinates before mapping. Returns 0 when the
 * host cannot. An explicit geometry names raster-space/internal presentation draws. */
static int shade_source_to_target(GmlRender *r,const uint32_t *source,int sw,int sh,
                                   double rx,double ry,double rw,double rh,uint32_t identity,
                                   double dx,double dy,double dw,double dh,
                                   uint32_t blend,double alpha,
                                   const GmlRenderShaderGeometry *geometry){
  enum { SHADED_MAX_EXTENT=4096, SHADED_MAX_PIXELS=16u<<20 };
  GmlRenderShaderRequest request;
  int width=(int)lround(fabs(dw)),height=(int)lround(fabs(dh));
  size_t pixels;
  int saved;
  if(!source || sw<=0 || sh<=0) return 0;
  if(width<=0 || height<=0 || width>SHADED_MAX_EXTENT || height>SHADED_MAX_EXTENT) return 0;
  pixels=(size_t)width*(size_t)height;
  if(pixels>SHADED_MAX_PIXELS) return 0;
  if(pixels>r->shaded_plane_capacity){
    uint32_t *grown=(uint32_t*)realloc(r->shaded_plane,pixels*sizeof *grown);
    if(!grown) return 0;
    r->shaded_plane=grown;
    r->shaded_plane_capacity=pixels;
  }
  memset(&request,0,sizeof request);
  request.shader=r->active_shader;
  request.source=source;
  request.source_width=sw;
  request.source_height=sh;
  request.source_pitch=sw;
  request.region_x=(int)lround(rx);
  request.region_y=(int)lround(ry);
  request.region_width=(int)lround(rw);
  request.region_height=(int)lround(rh);
  if(request.region_x<0 || request.region_y<0 ||
     request.region_width<=0 || request.region_height<=0 ||
     request.region_x+request.region_width>sw || request.region_y+request.region_height>sh)
    return 0;
  request.source_identity=identity;
  request.serial=++r->shaded_requests;
  request.width=width;
  request.height=height;
  /* Where this draw lands on the current render target, so a fragment reading its own place gets
   * the answer it would get drawing there. */
  request.target_width=r->fbw;
  request.target_height=r->fbh;
  request.dest_x=(int)lround(dx-r->cam_x);
  request.dest_y=(int)lround(dy-r->cam_y);
  request.geometry=geometry?*geometry:gml_render_shader_geometry(r,dx,dy,dw,dh,1);
  if(!request.geometry.valid) return 0;
  request.linear=r->interp?1:0;
  if(r->shade_vertex_colour_valid)
    memcpy(request.vertex_colour,r->shade_vertex_colour,sizeof request.vertex_colour);
  request.output=r->shaded_plane;
  if(!r->shader_executor(r->shader_executor_context,&request)) return 0;
  r->shaded_plane_borrowed=NULL;
  r->shaded_plane_width=width;
  r->shaded_plane_height=height;
  r->shaded_draws++;
  saved=r->active_shader;
  r->active_shader=-1;
  draw_surface_region(r,GML_RENDER_SHADED_SURFACE,0,0,width,height,dx,dy,
                      dw<0?-(double)width:(double)width,dh<0?-(double)height:(double)height,
                      blend,alpha);
  r->active_shader=saved;
  return 1;
}

int gml_render_cpu_shader_pass_surface(GmlRender *r,int surf,const uint32_t *spx,int sw,int sh,
                                        double rx,double ry,double rw,double rh,
                                        double dx,double dy,double dw,double dh,
                                        uint32_t blend,double alpha,
                                        const GmlRenderShaderGeometry *geometry){
  return shade_source_to_target(r,spx,sw,sh,rx,ry,rw,rh,
                                (uint32_t)(surf<0?0x7FFFFFFF:surf),dx,dy,dw,dh,blend,alpha,geometry);
}

int gml_render_shade_target_rect(GmlRender *r,int x1,int y1,int x2,int y2,uint32_t colour,double alpha){
  uint32_t source;
  if(!r || r->active_shader<0 || !r->shader_executor ||
     !gml_render_shader_content_candidate(r,r->active_shader)) return 0;
  if(x1>x2){ int t=x1; x1=x2; x2=t; }
  if(y1>y2){ int t=y1; y1=y2; y2=t; }
  if(x2<=x1 || y2<=y1) return 0;
  /* A one-texel source of the primitive's colour: a program that samples gm_BaseTexture reads the
   * flat colour a rectangle carries, and a procedural one ignores it. */
  source=gml_render_cpu_backend_color_to_xrgb(colour)|0xFF000000u;
  return shade_source_to_target(r,&source,1,1,0,0,1,1,0xFFFFFFFFu,
                                (double)x1+r->cam_x,(double)y1+r->cam_y,
                                (double)(x2-x1),(double)(y2-y1),0xFFFFFFu,alpha,NULL);
}

/* The program's answer for one atlas rectangle, evaluated on the device once and kept. A sprite
 * frame and a font glyph are the same thing here — a rectangle of a texture page — so both reuse
 * one answer, which is what turns a device round trip per draw into one per distinct rectangle. */
const uint32_t *gml_render_shaded_atlas_rect(GmlRender *r,int atlas,int sx,int sy,int w,int h){
  int shader,spare=-1;
  const uint32_t *px=NULL;
  uint64_t fingerprint;
  uint32_t oldest=0xFFFFFFFFu;
  GmlRenderShaderRequest request;
  if(!r || r->active_shader<0 || !r->shader_executor || w<=0 || h<=0) return NULL;
  if(!gml_render_shader_content_candidate(r,r->active_shader)) return NULL;
  /* A program whose answer moves with the draw cannot be kept for a rectangle. */
  if(r->shader_pal && r->shader_pal[r->active_shader].position_dependent) return NULL;
  shader=r->active_shader;
  fingerprint=gml_render_shader_uniform_fingerprint(r,shader);
  for(int i=0;i<GML_SHADED_FRAME_CACHE;i++){
    if(r->shaded_frame[i].px && r->shaded_frame[i].atlas==atlas && r->shaded_frame[i].sx==sx &&
       r->shaded_frame[i].sy==sy && r->shaded_frame[i].w==w && r->shaded_frame[i].h==h &&
       r->shaded_frame[i].shader==shader && r->shaded_frame[i].fingerprint==fingerprint){
      r->shaded_frame[i].last_used=++r->shaded_frame_clock;
      return r->shaded_frame[i].px;
    }
  }
  if(!gml_render_atlas_rect_plane(r,atlas,sx,sy,w,h,&px) || !px) return NULL;
  for(int i=0;i<GML_SHADED_FRAME_CACHE;i++){
    if(!r->shaded_frame[i].px){ spare=i; break; }
    if(r->shaded_frame[i].last_used<oldest){ oldest=r->shaded_frame[i].last_used; spare=i; }
  }
  if(spare<0) return NULL;
  if(r->shaded_frame[spare].w*r->shaded_frame[spare].h!=w*h || !r->shaded_frame[spare].px){
    uint32_t *grown=(uint32_t*)realloc(r->shaded_frame[spare].px,(size_t)w*h*sizeof *grown);
    if(!grown) return NULL;
    r->shaded_frame[spare].px=grown;
  }
  memset(&request,0,sizeof request);
  request.shader=shader;
  request.source=px;
  request.source_width=w;
  request.source_height=h;
  request.source_pitch=w;
  request.region_width=w;
  request.region_height=h;
  request.source_identity=((uint32_t)atlas<<20)^((uint32_t)sx<<10)^(uint32_t)sy;
  request.serial=++r->shaded_requests;
  request.width=w;
  request.height=h;
  request.output=r->shaded_frame[spare].px;
  if(!r->shader_executor(r->shader_executor_context,&request)) return NULL;
  r->shaded_frame[spare].atlas=atlas;
  r->shaded_frame[spare].sx=sx;
  r->shaded_frame[spare].sy=sy;
  r->shaded_frame[spare].shader=shader;
  r->shaded_frame[spare].fingerprint=fingerprint;
  r->shaded_frame[spare].w=w;
  r->shaded_frame[spare].h=h;
  r->shaded_frame[spare].last_used=++r->shaded_frame_clock;
  return r->shaded_frame[spare].px;
}

/* Compose an already-shaded plane at the target, with the draw's own blend, alpha and flips. */
int gml_render_compose_shaded_plane(GmlRender *r,const uint32_t *plane,int w,int h,
                                    double rx,double ry,double rw,double rh,
                                    double dx,double dy,double dw,double dh,
                                    uint32_t blend,double alpha){
  int saved;
  if(!r || !plane || w<=0 || h<=0) return 0;
  if(rx<0){ rw+=rx; rx=0; }
  if(ry<0){ rh+=ry; ry=0; }
  if(rw<=0 || rh<=0 || rx>=w || ry>=h) return 0;
  if(rx+rw>w) rw=w-rx;
  if(ry+rh>h) rh=h-ry;
  saved=r->active_shader;
  r->active_shader=-1;
  r->shaded_plane_borrowed=plane;
  r->shaded_plane_width=w;
  r->shaded_plane_height=h;
  draw_surface_region(r,GML_RENDER_SHADED_SURFACE,rx,ry,rw,rh,dx,dy,dw,dh,blend,alpha);
  r->shaded_plane_borrowed=NULL;
  r->active_shader=saved;
  r->shaded_draws++;
  return 1;
}

int gml_render_shaded_run_wanted(GmlRender *r){
  if(!r || r->run_plane_active) return 0;
  if(r->active_shader<0 || !r->shader_executor) return 0;
  if(!gml_render_shader_content_candidate(r,r->active_shader)) return 0;
  if(!r->shader_pal || !r->shader_pal[r->active_shader].position_dependent) return 0;
  return r->fb && r->fbw>0 && r->fbh>0;
}

int gml_render_shaded_run_begin(GmlRender *r,uint32_t **saved_fb,int *saved_shader){
  size_t count;
  if(!gml_render_shaded_run_wanted(r)) return 0;
  count=(size_t)r->fbw*(size_t)r->fbh;
  if(count>r->run_plane_capacity){
    uint32_t *grown=(uint32_t*)realloc(r->run_plane,count*sizeof *grown);
    if(!grown) return 0;
    r->run_plane=grown;
    r->run_plane_capacity=count;
  }
  memset(r->run_plane,0,count*sizeof *r->run_plane);
  gml_render_maybe_prepare_draw(r);
  *saved_fb=r->fb;
  *saved_shader=r->active_shader;
  r->fb=r->run_plane;
  r->active_shader=-1;      /* the gathering is plain; the program runs once over the result */
  r->run_plane_active=1;
  return 1;
}

void gml_render_shaded_run_end(GmlRender *r,uint32_t *saved_fb,int saved_shader,
                               int x0,int y0,int x1,int y1,uint32_t blend,double alpha){
  /* Gathering omits colour; the program applies vertex colour after evaluating the
   * sampled image, which matters when its result saturates. */
  uint32_t xrgb=gml_render_cpu_backend_color_to_xrgb(blend);
  float colour[4]={((xrgb>>16)&0xFFu)/255.0f,((xrgb>>8)&0xFFu)/255.0f,(xrgb&0xFFu)/255.0f,
                   (float)(alpha>1.0?1.0:(alpha<0.0?0.0:alpha))};
  const uint32_t *plane=r->run_plane;
  int w,h;
  r->fb=saved_fb;
  r->active_shader=saved_shader;
  r->run_plane_active=0;
  if(x0<0) x0=0;
  if(y0<0) y0=0;
  if(x1>r->fbw) x1=r->fbw;
  if(y1>r->fbh) y1=r->fbh;
  w=x1-x0; h=y1-y0;
  if(w<=0 || h<=0) return;
  /* One pass over what the run put down, at the place it put it, so a fragment reading its own
   * position gets the answer it would get drawing each piece there. */
  r->shade_vertex_colour_valid=1;
  memcpy(r->shade_vertex_colour,colour,sizeof colour);
  if(!shade_source_to_target(r,plane,r->fbw,r->fbh,x0,y0,w,h,0xFFFFFFFEu,
                             (double)x0+r->cam_x,(double)y0+r->cam_y,(double)w,(double)h,
                             0xFFFFFFu,1.0,NULL)){
    r->shade_vertex_colour_valid=0;
    /* The device could not: put the gathered picture down unshaded rather than lose it. */
    int saved=r->active_shader;
    r->active_shader=-1;
    r->shaded_plane_borrowed=plane;
    r->shaded_plane_width=r->fbw;
    r->shaded_plane_height=r->fbh;
    draw_surface_region(r,GML_RENDER_SHADED_SURFACE,x0,y0,w,h,
                        (double)x0+r->cam_x,(double)y0+r->cam_y,(double)w,(double)h,blend,alpha);
    r->shaded_plane_borrowed=NULL;
    r->active_shader=saved;
  }
  r->shade_vertex_colour_valid=0;
}

/* Draw one atlas rectangle through the content's program at a destination. A program whose answer
 * is a property of the texels is evaluated once for the rectangle and kept; one whose answer moves
 * with the draw is evaluated where it lands, which costs a device round trip per draw and is the
 * only way it can be right. Returns 0 when nothing can run it. */
int gml_render_shade_atlas_rect_at(GmlRender *r,int atlas,int sx,int sy,int w,int h,
                                   double dx,double dy,double dw,double dh,
                                   uint32_t blend,double alpha){
  if(!r || r->active_shader<0 || !r->shader_executor || w<=0 || h<=0) return 0;
  if(!gml_render_shader_content_candidate(r,r->active_shader)) return 0;
  if(r->shader_pal && r->shader_pal[r->active_shader].position_dependent){
    const uint32_t *px=NULL;
    if(!gml_render_atlas_rect_plane(r,atlas,sx,sy,w,h,&px) || !px) return 0;
    return shade_source_to_target(r,px,w,h,0,0,w,h,
                                  ((uint32_t)atlas<<20)^((uint32_t)sx<<10)^(uint32_t)sy,
                                  dx,dy,dw,dh,blend,alpha,NULL);
  }
  {
    const uint32_t *shaded=gml_render_shaded_atlas_rect(r,atlas,sx,sy,w,h);
    if(!shaded) return 0;
    return gml_render_compose_shaded_plane(r,shaded,w,h,0,0,w,h,dx,dy,dw,dh,blend,alpha);
  }
}

/* Compose an already-shaded plane as a rotated sprite. The ordinary sprite blit already has the
 * pivot, the filtering and the edge rules, and it reads the byte order a decoded page uses, so the
 * plane is handed over in that order rather than a second rotation being written here. */
int gml_render_compose_shaded_rotated(GmlRender *r,GmlSprite *owner,const uint32_t *plane,
                                      int w,int h,double x,double y,double xs,double ys,
                                      double rot,int origin_x,int origin_y,
                                      uint32_t blend,double alpha){
  size_t count;
  if(!r || !plane || w<=0 || h<=0) return 0;
  count=(size_t)w*(size_t)h;
  if(count>SIZE_MAX/4u) return 0;
  if(count*4u>r->shaded_bytes_capacity){
    uint8_t *grown=(uint8_t*)realloc(r->shaded_bytes,count*4u);
    if(!grown) return 0;
    r->shaded_bytes=grown;
    r->shaded_bytes_capacity=count*4u;
  }
  for(size_t i=0;i<count;i++){
    uint32_t px=plane[i];
    r->shaded_bytes[i*4u+0]=(uint8_t)(px>>16);
    r->shaded_bytes[i*4u+1]=(uint8_t)(px>>8);
    r->shaded_bytes[i*4u+2]=(uint8_t)px;
    r->shaded_bytes[i*4u+3]=(uint8_t)(px>>24);
  }
  {
    int saved=r->active_shader;
    r->active_shader=-1;
    blit_rgba_sprite(r,owner,r->shaded_bytes,w,h,x,y,xs,ys,rot,origin_x,origin_y,blend,alpha,1,
                     NULL,NULL,0);
    r->active_shader=saved;
  }
  r->shaded_draws++;
  return 1;
}

int gml_render_shade_target_sprite_part(GmlRender *r,int sprite,int frame,
                                        double rx,double ry,double rw,double rh,
                                        double dx,double dy,double dw,double dh,
                                        uint32_t blend,double alpha){
  int atlas=0,sx=0,sy=0,w=0,h=0;
  const uint32_t *shaded;
  if(!r || r->active_shader<0 || !r->shader_executor ||
     !gml_render_shader_content_candidate(r,r->active_shader)) return 0;
  /* A sprite whose frame is a texture-page rectangle shares the cache with every other rectangle;
   * one built at runtime has no page, and keeps the per-frame path below. */
  {
    /* A stored frame is cropped: the region a draw names is in the sprite's own space, and the
     * page holds only the part that carried colour, placed at the crop offset. Translating by that
     * offset — and clipping the destination to the part the page actually has — is what makes a
     * region of a cropped frame land where the plain blit puts it. */
    int tx=0,ty=0;
    if(gml_render_sprite_frame_rect_full(r,sprite,frame,&atlas,&sx,&sy,&w,&h,&tx,&ty)){
      double scale_x=rw>0?dw/rw:0.0, scale_y=rh>0?dh/rh:0.0;
      double lx0=rx>tx?rx:tx, ly0=ry>ty?ry:ty;
      double lx1=rx+rw<tx+w?rx+rw:tx+w, ly1=ry+rh<ty+h?ry+rh:ty+h;
      /* The region names part of the sprite that carried no colour, so the stored page holds
       * nothing for it: there is nothing to shade and nothing to draw, and saying so is the
       * answer rather than handing the draw to a path that would also draw nothing. */
      if(lx1<=lx0 || ly1<=ly0){ r->shaded_draws++; return 1; }
      dx+=(lx0-rx)*scale_x;
      dy+=(ly0-ry)*scale_y;
      dw=(lx1-lx0)*scale_x;
      dh=(ly1-ly0)*scale_y;
      rx=lx0-tx; ry=ly0-ty; rw=lx1-lx0; rh=ly1-ly0;
    }
  }
  if(gml_render_sprite_frame_rect(r,sprite,frame,&atlas,&sx,&sy,&w,&h)){
    if(r->shader_pal && r->shader_pal[r->active_shader].position_dependent){
      const uint32_t *px=NULL;
      if(!gml_render_atlas_rect_plane(r,atlas,sx,sy,w,h,&px) || !px) return 0;
      if(rx<0){ rw+=rx; rx=0; }
      if(ry<0){ rh+=ry; ry=0; }
      if(rw<=0 || rh<=0 || rx>=w || ry>=h) return 0;
      if(rx+rw>w) rw=w-rx;
      if(ry+rh>h) rh=h-ry;
      return shade_source_to_target(r,px,w,h,rx,ry,rw,rh,
                                    ((uint32_t)atlas<<20)^((uint32_t)sx<<10)^(uint32_t)sy,
                                    dx,dy,dw,dh,blend,alpha,NULL);
    }
    shaded=gml_render_shaded_atlas_rect(r,atlas,sx,sy,w,h);
    if(!shaded){ return 0; }
    if(!gml_render_compose_shaded_plane(r,shaded,w,h,rx,ry,rw,rh,dx,dy,dw,dh,blend,alpha)){
      return 0; }
    return 1;
  }
  {
    const uint32_t *px=NULL;
    if(!gml_render_sprite_frame_plane(r,sprite,frame,&w,&h,&px) || !px || w<=0 || h<=0){
      return 0; }
    if(rx<0){ rw+=rx; rx=0; }
    if(ry<0){ rh+=ry; ry=0; }
    if(rw<=0 || rh<=0 || rx>=w || ry>=h) return 0;
    if(rx+rw>w) rw=w-rx;
    if(ry+rh>h) rh=h-ry;
    return shade_source_to_target(r,px,w,h,rx,ry,rw,rh,
                                  ((uint32_t)sprite<<10)|((uint32_t)frame&0x3FFu),
                                  dx,dy,dw,dh,blend,alpha,NULL);
  }
}

int gml_render_shade_target_sprite(GmlRender *r,int sprite,int frame,
                                    double dx,double dy,double dw,double dh,
                                    uint32_t blend,double alpha){
  int w=0,h=0;
  const uint32_t *px=NULL;
  if(!r || r->active_shader<0) return 0;
  if(!gml_render_sprite_frame_plane(r,sprite,frame,&w,&h,&px) || !px || w<=0 || h<=0) return 0;
  return gml_render_shade_target_sprite_part(r,sprite,frame,0,0,w,h,dx,dy,dw,dh,blend,alpha);
}
