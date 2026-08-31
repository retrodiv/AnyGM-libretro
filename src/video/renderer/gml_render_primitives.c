/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Renderer-owned fixed-function primitive kernels and typed operations. */
#include <math.h>
#include <stdlib.h>
#if defined(__SSE2__)
#include <emmintrin.h>
#endif

#include <stdio.h>
#include "gml_render.h"
#include "gml_render_backend.h"
#include "gml_render_internal.h"

static void draw_additive_span(GmlRender *render,uint32_t *pixels,int count,
                               uint32_t color,double alpha){
  uint32_t source=gml_render_backend_color_to_xrgb(color);
  int add_red=(int)(((source>>16)&255u)*alpha);
  int add_green=(int)(((source>>8)&255u)*alpha);
  int add_blue=(int)((source&255u)*alpha);
  int add_alpha=(int)(255.0*alpha);
#if defined(__SSE2__)
  uint32_t packed=(render->target_sp>0?(uint32_t)add_alpha<<24:0)|
                  ((uint32_t)add_red<<16)|((uint32_t)add_green<<8)|
                  (uint32_t)add_blue;
  __m128i increment=_mm_set1_epi32((int)packed);
  __m128i framebuffer_alpha=_mm_set1_epi32((int)UINT32_C(0xFF000000));
  while(count>=4){
    __m128i destination=_mm_loadu_si128((const __m128i*)pixels);
    __m128i result=_mm_adds_epu8(destination,increment);
    if(render->target_sp<=0) result=_mm_or_si128(result,framebuffer_alpha);
    _mm_storeu_si128((__m128i*)pixels,result);
    pixels+=4;
    count-=4;
  }
#endif
  for(int index=0;index<count;index++){
    uint32_t destination=pixels[index];
    int red=(int)((destination>>16)&255u)+add_red;
    int green=(int)((destination>>8)&255u)+add_green;
    int blue=(int)(destination&255u)+add_blue;
    int coverage=render->target_sp>0
      ? (int)(destination>>24)+add_alpha : 255;
    if(red>255) red=255;
    if(green>255) green=255;
    if(blue>255) blue=255;
    if(coverage>255) coverage=255;
    pixels[index]=((uint32_t)coverage<<24)|((uint32_t)red<<16)|
                  ((uint32_t)green<<8)|(uint32_t)blue;
  }
}

static void draw_opaque_gradient_row(uint32_t *pixels,int count,
                                     uint32_t left,uint32_t right){
  int denominator=count-1;
  if(denominator<=0){
    if(count>0) pixels[0]=left;
    return;
  }
  int red=(int)((left>>16)&255u);
  int green=(int)((left>>8)&255u);
  int blue=(int)(left&255u);
  int red_delta=(int)((right>>16)&255u)-red;
  int green_delta=(int)((right>>8)&255u)-green;
  int blue_delta=(int)(right&255u)-blue;
  int red_sign=red_delta<0?-1:1;
  int green_sign=green_delta<0?-1:1;
  int blue_sign=blue_delta<0?-1:1;
  int red_amount=abs(red_delta),green_amount=abs(green_delta),blue_amount=abs(blue_delta);
  int red_error=0,green_error=0,blue_error=0;
  for(int index=0;index<count;index++){
    pixels[index]=UINT32_C(0xFF000000)|((uint32_t)red<<16)|
                  ((uint32_t)green<<8)|(uint32_t)blue;
    red_error+=red_amount;
    green_error+=green_amount;
    blue_error+=blue_amount;
    if(red_error>=denominator){
      int increment=red_amount<=denominator?1:red_error/denominator;
      red_error-=increment*denominator;
      red+=red_sign*increment;
    }
    if(green_error>=denominator){
      int increment=green_amount<=denominator?1:green_error/denominator;
      green_error-=increment*denominator;
      green+=green_sign*increment;
    }
    if(blue_error>=denominator){
      int increment=blue_amount<=denominator?1:blue_error/denominator;
      blue_error-=increment*denominator;
      blue+=blue_sign*increment;
    }
  }
}

typedef struct {
  GmlRender *render;
  int x,y,width;
  uint32_t color;
  double alpha;
} GmlAdditiveRows;
static void draw_additive_rows(void *context,int row_start,int row_end,int slot){
  (void)slot;
  GmlAdditiveRows *rows=(GmlAdditiveRows*)context;
  for(int row=row_start;row<row_end;row++)
    draw_additive_span(rows->render,
      rows->render->fb+(size_t)(rows->y+row)*rows->render->fbw+rows->x,
      rows->width,rows->color,rows->alpha);
}

static void draw_rect_prim_alpha(GmlRender *R, int x1, int y1, int x2, int y2, uint32_t gmcol, int outline, double alpha){
  if(!R) return;
  if(x1>x2){ int t=x1; x1=x2; x2=t; }
  if(y1>y2){ int t=y1; y1=y2; y2=t; }
  int geometric_x1=x1,geometric_y1=y1,geometric_x2=x2,geometric_y2=y2;
  if(x2<0||y2<0||x1>=R->fbw||y1>=R->fbh) return;
  if(x1<0) x1=0;
  if(y1<0) y1=0;
  if(x2>=R->fbw) x2=R->fbw-1;
  if(y2>=R->fbh) y2=R->fbh-1;
  if(alpha>1) alpha=1; else if(alpha<0) alpha=0;
  if(!outline && alpha<=0) return;
  /* An untextured fragment shader receives the primitive coordinates and produces its own colour;
   * draw_set_color/alpha need not affect it when the shader source does not consume vertex colour. */
  if(!outline && R->blendmode==0 && (alpha>=1 || !R->alphablend)){   /* opaque filled rect: fast per-row fill */
    gml_render_maybe_prepare_opaque_rect(R,x1,y1,x2+1,y2+1);
    uint32_t src=gml_render_backend_color_to_xrgb(gmcol);
    if(x1==0 && y1==0 && x2==R->fbw-1 && y2==R->fbh-1){
      gml_render_set_pending_fill(R,src);
      return;
    }
    for(int y=y1;y<=y2;y++) gml_render_backend_fill_xrgb(R->fb+(size_t)y*R->fbw+x1,x2-x1+1,src);
    return;
  }
  if(!outline){
    gml_render_maybe_prepare_draw(R);
    if(R->blendmode!=0){
      if(R->alphablend && R->blendmode==1 &&
         R->blend_equation==1 && R->blend_equation_alpha==1){
        R->fb_all_transparent=0;
        GmlAdditiveRows rows={R,x1,y1,x2-x1+1,gmcol,alpha};
        if((size_t)rows.width*(size_t)(y2-y1+1)>=262144u)
          gml_run_row_bands(R,y2-y1+1,draw_additive_rows,&rows);
        else
          draw_additive_rows(&rows,0,y2-y1+1,0);
        return;
      }
      for(int y=y1;y<=y2;y++) for(int x=x1;x<=x2;x++) gml_render_backend_draw_pixel_alpha(R,x,y,gmcol,alpha);
      return;
    }
    uint32_t src=gml_render_backend_color_to_xrgb(gmcol);
    if(R->classic && gml_render_backend_flat_blend_uses_float_alpha(alpha)){
      GmlRenderBackendFlatBlend blend;
      gml_render_backend_flat_blend_init(&blend,src,alpha);
      for(int y=y1;y<=y2;y++)
        gml_render_backend_flat_blend_run(R,&R->fb[(size_t)y*R->fbw+x1],x2-x1+1,&blend);
      return;
    }
    for(int y=y1;y<=y2;y++)
      gml_render_backend_draw_xrgb_alpha(R,&R->fb[(size_t)y*R->fbw+x1],x2-x1+1,src,alpha);
    return;
  }
  for(int y=y1;y<=y2;y++) for(int x=x1;x<=x2;x++){
    /* Clip the geometric edges instead of turning each clipped boundary into a new edge. */
    if(y!=geometric_y1 && y!=geometric_y2 &&
       x!=geometric_x1 && x!=geometric_x2) continue;
    gml_render_backend_draw_pixel_alpha(R,x,y,gmcol,alpha);
  }
}
static void draw_rect_prim(GmlRender *R, int x1, int y1, int x2, int y2, uint32_t gmcol, int outline){
  draw_rect_prim_alpha(R,x1,y1,x2,y2,gmcol,outline,R?R->alpha:1);
}
void gml_render_clear(GmlRender *R,uint32_t color,double alpha){
  if(!R) return;
  if(alpha<0) alpha=0;
  if(alpha>1) alpha=1;
  uint32_t source=gml_render_backend_color_to_xrgb(color);
  source=(source&UINT32_C(0x00FFFFFF))|
    ((uint32_t)lround(alpha*255.0)<<24);
  gml_render_set_pending_fill(R,source);
}
/* Full-screen color fill for spriteless GMS2 background layers (screen space, at layer depth). */
void gml_draw_layer_color_fill(GmlRender *R, uint32_t gmcol, double alpha){
  if(!R) return;
  draw_rect_prim_alpha(R,0,0,R->fbw-1,R->fbh-1,gmcol,0,alpha);
}

typedef struct {
  GmlRender *render;
  int x,y,width,height_denominator;
  uint32_t top_left,top_right,bottom_right,bottom_left;
  /* The gradient belongs to the authored rectangle, not to its visible part. When the rectangle
   * ran off the framebuffer, these carry the visible region's position inside the authored one,
   * so every colour is still evaluated in authored space. */
  int x_offset,y_offset,width_denominator;
} GmlGradientRows;
static void draw_gradient_rows(void *context,int row_start,int row_end,int slot){
  (void)slot;
  GmlGradientRows *rows=(GmlGradientRows*)context;
  for(int row=row_start;row<row_end;row++){
    uint32_t left=gml_render_backend_lerp_xrgb(
      rows->top_left,rows->bottom_left,rows->y_offset+row,rows->height_denominator);
    uint32_t right=gml_render_backend_lerp_xrgb(
      rows->top_right,rows->bottom_right,rows->y_offset+row,rows->height_denominator);
    if(rows->x_offset>0 || rows->x_offset+rows->width-1<rows->width_denominator){
      uint32_t clipped_left=gml_render_backend_lerp_xrgb(
        left,right,rows->x_offset,rows->width_denominator);
      right=gml_render_backend_lerp_xrgb(
        left,right,rows->x_offset+rows->width-1,rows->width_denominator);
      left=clipped_left;
    }
    uint32_t *pixels=rows->render->fb+
      (size_t)(rows->y+row)*rows->render->fbw+rows->x;
    if(left==right) gml_render_backend_fill_xrgb(pixels,rows->width,left);
    else draw_opaque_gradient_row(pixels,rows->width,left,right);
  }
}

static void draw_rect_colour_prim(GmlRender *R, int x1, int y1, int x2, int y2,
                                  uint32_t c1, uint32_t c2, uint32_t c3, uint32_t c4,
                                  int outline){
  if(!R) return;
  if(c1==c2 && c1==c3 && c1==c4){
    draw_rect_prim(R,x1,y1,x2,y2,c1,outline);
    return;
  }
  if(outline){
    draw_rect_prim(R,x1,y1,x2,y2,c1,outline);
    return;
  }
  if(x1>x2){ int t=x1; x1=x2; x2=t; }
  if(y1>y2){ int t=y1; y1=y2; y2=t; }
  if(x2<0||y2<0||x1>=R->fbw||y1>=R->fbh) return;
  /* The four colours span the authored rectangle. Clipping selects which pixels are drawn,
   * not where the gradient's endpoints sit. */
  int ox1=x1, oy1=y1;
  int owden=x2-x1, ohden=y2-y1;
  if(x1<0) x1=0;
  if(y1<0) y1=0;
  if(x2>=R->fbw) x2=R->fbw-1;
  if(y2>=R->fbh) y2=R->fbh-1;
  int x_off=x1-ox1, y_off=y1-oy1;
  double alpha=R->alpha; if(alpha>1) alpha=1; else if(alpha<0) alpha=0;
  if(alpha<=0) return;
  if(alpha>=1.0 || !R->alphablend) gml_render_maybe_prepare_opaque_rect(R,x1,y1,x2+1,y2+1);
  else gml_render_maybe_prepare_draw(R);
  uint32_t tl=gml_render_backend_color_to_xrgb(c1), tr=gml_render_backend_color_to_xrgb(c2), br=gml_render_backend_color_to_xrgb(c3), bl=gml_render_backend_color_to_xrgb(c4);
  int wden=owden, hden=ohden, n=x2-x1+1;
  if(alpha>=1.0){
    GmlGradientRows rows={R,x1,y1,n,hden,tl,tr,br,bl,x_off,y_off,wden};
    if((size_t)n*(size_t)(y2-y1+1)>=262144u)
      gml_run_row_bands(R,y2-y1+1,draw_gradient_rows,&rows);
    else
      draw_gradient_rows(&rows,0,y2-y1+1,0);
    return;
  }
  if(tl==tr && bl==br){
    for(int y=y1;y<=y2;y++){
      uint32_t rowc=gml_render_backend_lerp_xrgb(tl,bl,y_off+(y-y1),hden);
      gml_render_backend_draw_xrgb_alpha(R,R->fb+(size_t)y*R->fbw+x1,n,rowc,alpha);
    }
    return;
  }
  for(int y=y1;y<=y2;y++){
    uint32_t lc=gml_render_backend_lerp_xrgb(tl,bl,y_off+(y-y1),hden);
    uint32_t rc=gml_render_backend_lerp_xrgb(tr,br,y_off+(y-y1),hden);
    uint32_t *row=R->fb+(size_t)y*R->fbw+x1;
    if(lc==rc){ gml_render_backend_draw_xrgb_alpha(R,row,n,lc,alpha); continue; }
    for(int x=0;x<n;x++){
      uint32_t c=gml_render_backend_lerp_xrgb(lc,rc,x_off+x,wden);
      gml_render_backend_draw_xrgb_alpha(R,row+x,1,c,alpha);
    }
  }
}
static uint32_t gm_color_lerp_steps(uint32_t first,uint32_t second,
                                    int numerator,int denominator){
  if(denominator<=0 || numerator<=0) return first;
  if(numerator>=denominator) return second;
  int first_red=first&255,first_green=(first>>8)&255,first_blue=(first>>16)&255;
  int second_red=second&255,second_green=(second>>8)&255,second_blue=(second>>16)&255;
  int red=first_red+(second_red-first_red)*numerator/denominator;
  int green=first_green+(second_green-first_green)*numerator/denominator;
  int blue=first_blue+(second_blue-first_blue)*numerator/denominator;
  return (uint32_t)red|((uint32_t)green<<8)|((uint32_t)blue<<16);
}

static uint32_t gm_color_lerp_amount(uint32_t first,uint32_t second,double amount){
  if(amount<=0.0) return first;
  if(amount>=1.0) return second;
  int first_red=first&255,first_green=(first>>8)&255,first_blue=(first>>16)&255;
  int second_red=second&255,second_green=(second>>8)&255,second_blue=(second>>16)&255;
  int red=(int)(first_red+(second_red-first_red)*amount);
  int green=(int)(first_green+(second_green-first_green)*amount);
  int blue=(int)(first_blue+(second_blue-first_blue)*amount);
  return (uint32_t)red|((uint32_t)green<<8)|((uint32_t)blue<<16);
}

static void draw_line_colour_prim(GmlRender *R, int x0, int y0, int x1, int y1,
                                  uint32_t first,uint32_t second,int width){
  if(width<1) width=1;
  int dx=abs(x1-x0), sx=x0<x1?1:-1;
  int dy=-abs(y1-y0), sy=y0<y1?1:-1;
  int denominator=dx>-dy?dx:-dy,step=0;
  int err=dx+dy;
  for(;;){
    int r=width/2;
    uint32_t color=gm_color_lerp_steps(first,second,step,denominator);
    draw_rect_prim(R,x0-r,y0-r,x0-r+width-1,y0-r+width-1,color,0);
    if(x0==x1 && y0==y1) break;
    int e2=2*err;
    if(e2>=dy){ err+=dy; x0+=sx; }
    if(e2<=dx){ err+=dx; y0+=sy; }
    step++;
  }
}
static void draw_line_prim(GmlRender *R,int x0,int y0,int x1,int y1,
                           uint32_t color,int width){
  draw_line_colour_prim(R,x0,y0,x1,y1,color,color,width);
}

static int line_open_interval_axis(double start,double delta,
                                   double lower,double upper,
                                   double *interval_start,double *interval_end){
  if(delta==0.0) return start>lower && start<upper;
  double first=(lower-start)/delta,second=(upper-start)/delta;
  if(first>second){ double temporary=first; first=second; second=temporary; }
  if(first>*interval_start) *interval_start=first;
  if(second<*interval_end) *interval_end=second;
  return *interval_start<*interval_end;
}

static int line_crosses_pixel_diamond(double x0,double y0,double x1,double y1,
                                      int pixel_x,int pixel_y){
  double center_u=pixel_x+pixel_y+1.0;
  double center_v=pixel_x-pixel_y;
  double start_u=x0+y0,start_v=x0-y0;
  double delta_u=(x1+y1)-start_u,delta_v=(x1-y1)-start_v;
  double interval_start=0.0,interval_end=1.0;
  if(!line_open_interval_axis(start_u,delta_u,center_u-0.5,center_u+0.5,
                              &interval_start,&interval_end) ||
     !line_open_interval_axis(start_v,delta_v,center_v-0.5,center_v+0.5,
                              &interval_start,&interval_end))
    return 0;
  /* Diamond-exit line rules omit the fragment containing the final endpoint. */
  return fabs(x1-(pixel_x+0.5))+fabs(y1-(pixel_y+0.5))>=0.5;
}

static void draw_line_colour_subpixel_prim(GmlRender *R,
                                           double x0,double y0,double x1,double y1,
                                           uint32_t first,uint32_t second,int width){
  if(!R || !isfinite(x0) || !isfinite(y0) || !isfinite(x1) || !isfinite(y1)) return;
  if(width<1) width=1;
  double color_dx=x1-x0,color_dy=y1-y0;
  double color_denominator=color_dx*color_dx+color_dy*color_dy;
  if(color_denominator<=0.0) return;

  /* Resolve exact diamond-boundary ties deterministically. The y perturbation is reflected for
   * the renderer's top-down framebuffer coordinates. */
  const double perturbation=1e-7;
  double raster_x0=x0-perturbation,raster_y0=y0+perturbation*perturbation;
  double raster_x1=x1-perturbation,raster_y1=y1+perturbation*perturbation;
  double raster_dx=raster_x1-raster_x0,raster_dy=raster_y1-raster_y0;
  int x_major=fabs(raster_dx)>=fabs(raster_dy);
  double minimum=x_major?fmin(raster_x0,raster_x1):fmin(raster_y0,raster_y1);
  double maximum=x_major?fmax(raster_x0,raster_x1):fmax(raster_y0,raster_y1);
  int major_start=(int)floor(minimum)-1;
  int major_end=(int)floor(maximum)+1;
  for(int major=major_start;major<=major_end;major++){
    double parameter;
    if(x_major){
      parameter=(major+0.5-raster_x0)/raster_dx;
    } else {
      parameter=(major+0.5-raster_y0)/raster_dy;
    }
    if(parameter<0.0) parameter=0.0;
    else if(parameter>1.0) parameter=1.0;
    double minor=x_major?raster_y0+parameter*raster_dy
                        :raster_x0+parameter*raster_dx;
    int minor_center=(int)floor(minor);
    for(int candidate=minor_center-2;candidate<=minor_center+2;candidate++){
      int pixel_x=x_major?major:candidate;
      int pixel_y=x_major?candidate:major;
      if(!line_crosses_pixel_diamond(raster_x0,raster_y0,raster_x1,raster_y1,
                                     pixel_x,pixel_y))
        continue;
      double fragment_x=pixel_x+0.5,fragment_y=pixel_y+0.5;
      double color_parameter=((fragment_x-x0)*color_dx+(fragment_y-y0)*color_dy)/
                             color_denominator;
      uint32_t color=gm_color_lerp_amount(first,second,color_parameter);
      int radius=width/2;
      draw_rect_prim(R,pixel_x-radius,pixel_y-radius,
                     pixel_x-radius+width-1,pixel_y-radius+width-1,color,0);
    }
  }
}


/* The continuous form. The mapped centre and radii of a circle are generally fractional, and
 * truncating them before rasterization loses up to a whole destination pixel of extent, so the
 * geometry is carried as-is and only the covered pixel set is quantized. The integer entry point
 * below forwards to this one, so both share a single rasterization kernel. */
static void draw_circle_prim_ex(GmlRender *R, double cx, double cy, double rx, double ry,
                                uint32_t gmcol, int outline){
  if(!R||rx<0.0||ry<0.0) return;
  if(!(rx>0.0)||!(ry>0.0)){
    gml_render_backend_draw_pixel(R,(int)floor(cx),(int)floor(cy),gmcol);
    return;
  }
  int n=R->circle_precision;
  if(n<4) n=4;
  if(n>64) n=64;
  n=(n/4)*4; if(n<4) n=4;
  double vx[64], vy[64];
  for(int i=0;i<n;i++){
    double a=(i*2.0*M_PI)/n;
    vx[i]=cx+cos(a)*rx;
    vy[i]=cy-sin(a)*ry;
  }
  if(outline){
    for(int i=0;i<n;i++){
      int j=(i+1)%n;
      draw_line_prim(R,(int)lround(vx[i]),(int)lround(vy[i]),(int)lround(vx[j]),(int)lround(vy[j]),gmcol,1);
    }
  } else {
    /* The regular polygon is convex, so each scanline has at most one filled span. This is
     * pixel-identical to point_in_poly(x+0.5,y+0.5) but avoids testing every edge per pixel. */
    double span_y0=floor(cy-ry),span_y1=ceil(cy+ry);
    if(span_y1<0.0 || span_y0>=(double)R->fbh) return;
    int y0=span_y0<0.0?0:(int)span_y0;
    int y1=span_y1>=(double)R->fbh?R->fbh-1:(int)span_y1;
    for(int y=y0;y<=y1;y++){
      double py=y+0.5, xl=1e30, xr=-1e30;
      for(int i=0,j=n-1;i<n;j=i++){
        if((vy[i]>py)==(vy[j]>py)) continue;
        double xi=(vx[j]-vx[i])*(py-vy[i])/(vy[j]-vy[i])+vx[i];
        if(xi<xl) xl=xi;
        if(xi>xr) xr=xi;
      }
      if(xr<xl) continue;
      double first=ceil(xl-0.5),last=ceil(xr-0.5)-1.0;
      if(last<0.0 || first>=(double)R->fbw) continue;
      int x0=first<0.0?0:(int)first;
      int x1=last>=(double)R->fbw?R->fbw-1:(int)last;
      for(int x=x0;x<=x1;x++) gml_render_backend_draw_pixel(R,x,y,gmcol);
    }
  }
}

static void draw_circle_prim(GmlRender *R, int cx, int cy, int rx, int ry, uint32_t gmcol, int outline){
  if(!R||rx<0||ry<0) return;
  draw_circle_prim_ex(R,(double)cx,(double)cy,(double)rx,(double)ry,gmcol,outline);
}

/* Colour variants interpolate from the first colour at the centre to the second at the perimeter. */
static uint32_t gm_color_lerp_fan(uint32_t inner,uint32_t outer,double amount){
  return gm_color_lerp_amount(inner,outer,amount);
}
static void draw_px_fan(GmlRender *R,int x,int y,uint32_t inner,uint32_t outer,double amount){
  if(R && R->alphablend && R->blendmode==2 &&
     R->blend_equation==1 && R->blend_equation_alpha==1 &&
     x>=0 && y>=0 && x<R->fbw && y<R->fbh && R->alpha>0.0){
    if(amount<0.0) amount=0.0; else if(amount>1.0) amount=1.0;
    if(R->pending_underlay || R->pending_fill) gml_render_prepare_draw(R);
    R->fb_all_transparent=0;
    if(R->target_sp>0){
      R->fb_opaque_known=0;
      R->fb_all_opaque=0;
    }
    /* GPU vertex colours remain continuous until blending.  Quantising the fan colour to an
     * intermediate byte first creates visible one-step rings and changes bm_inv_src_colour at
     * polygon-sector boundaries. */
    double sr=(inner&255)+((int)(outer&255)-(int)(inner&255))*amount;
    double sg=((inner>>8)&255)+((int)((outer>>8)&255)-(int)((inner>>8)&255))*amount;
    double sb=((inner>>16)&255)+((int)((outer>>16)&255)-(int)((inner>>16)&255))*amount;
    uint32_t *dp=&R->fb[(size_t)y*R->fbw+x];
    unsigned dr=(*dp>>16)&255,dg=(*dp>>8)&255,db=*dp&255;
    unsigned rr=(unsigned)lround(dr*(1.0-sr/255.0));
    unsigned gg=(unsigned)lround(dg*(1.0-sg/255.0));
    unsigned bb=(unsigned)lround(db*(1.0-sb/255.0));
    unsigned coverage=R->target_sp>0?*dp>>24:255;
    if(R->target_sp>0){
      double alpha=R->alpha>1.0?1.0:R->alpha;
      coverage=gml_blend_inv_source_u8(coverage,(unsigned)lround(alpha*255.0));
    }
    *dp=(coverage<<24)|(rr<<16)|(gg<<8)|bb;
    return;
  }
  gml_render_backend_draw_pixel(R,x,y,gm_color_lerp_fan(inner,outer,amount));
}
static void draw_circle_colour_prim_ex(GmlRender *R,double cx,double cy,double rx,double ry,
                                       uint32_t inner,uint32_t outer,int outline){
  if(!R||rx<0.0||ry<0.0) return;
  if(inner==outer){ draw_circle_prim_ex(R,cx,cy,rx,ry,inner,outline); return; }
  if(!(rx>0.0)||!(ry>0.0)){
    gml_render_backend_draw_pixel(R,(int)floor(cx),(int)floor(cy),inner);
    return;
  }
  int n=R->circle_precision;
  if(n<4) n=4;
  if(n>64) n=64;
  n=(n/4)*4; if(n<4) n=4;
  double vx[64],vy[64],edge_nx[64],edge_ny[64];
  double step=2.0*M_PI/n,apothem=cos(M_PI/n);
  for(int i=0;i<n;i++){
    double a=i*step,mid=(i+.5)*step;
    vx[i]=cx+cos(a)*rx;
    vy[i]=cy-sin(a)*ry;
    edge_nx[i]=cos(mid);
    edge_ny[i]=-sin(mid);
  }
  if(outline){
    for(int i=0;i<n;i++){
      int j=(i+1)%n;
      draw_line_prim(R,(int)lround(vx[i]),(int)lround(vy[i]),
                     (int)lround(vx[j]),(int)lround(vy[j]),outer,1);
    }
    return;
  }
  double span_y0=floor(cy-ry),span_y1=ceil(cy+ry);
  if(span_y1<0.0 || span_y0>=(double)R->fbh) return;
  int y0=span_y0<0.0?0:(int)span_y0;
  int y1=span_y1>=(double)R->fbh?R->fbh-1:(int)span_y1;
  for(int y=y0;y<=y1;y++){
    double py=y+0.5,xl=1e30,xr=-1e30;
    for(int i=0,j=n-1;i<n;j=i++){
      if((vy[i]>py)==(vy[j]>py)) continue;
      double xi=(vx[j]-vx[i])*(py-vy[i])/(vy[j]-vy[i])+vx[i];
      if(xi<xl) xl=xi;
      if(xi>xr) xr=xi;
    }
    if(xr<xl) continue;
    double first=ceil(xl-0.5),last=ceil(xr-0.5)-1.0;
    if(last<0.0 || first>=(double)R->fbw) continue;
    int x0=first<0.0?0:(int)first;
    int x1=last>=(double)R->fbw?R->fbw-1:(int)last;
    double ny=(py-cy)/ry;
    double nx=(x0+0.5-cx)/rx;
    int edge=0,direction=ny>=0.0?1:-1;
    double edge_dot=nx*edge_nx[0]+ny*edge_ny[0];
    for(int i=1;i<n;i++){
      double dot=nx*edge_nx[i]+ny*edge_ny[i];
      if(dot>edge_dot){ edge=i; edge_dot=dot; }
    }
    for(int x=x0;x<=x1;x++){
      /* A regular fan is the intersection of its edge half-planes.  Its exact barycentric
       * centre-to-rim amount is max(dot(point,edge_normal))/apothem.  As x increases on one
       * scanline the winning normal advances monotonically, making this exact interpolation
       * O(pixels+edges) rather than testing every triangle for every pixel. */
      if(fabs(ny)<1e-15) edge_dot=fabs(nx)*apothem;
      else for(int advanced=0;advanced<n;advanced++){
        int next=(edge+direction+n)%n;
        double next_dot=nx*edge_nx[next]+ny*edge_ny[next];
        if(next_dot<=edge_dot+1e-15) break;
        edge=next; edge_dot=next_dot;
      }
      draw_px_fan(R,x,y,inner,outer,edge_dot/apothem);
      nx+=1.0/rx;
      edge_dot=nx*edge_nx[edge]+ny*edge_ny[edge];
    }
  }
}

static void draw_circle_colour_prim(GmlRender *R,int cx,int cy,int rx,int ry,
                                    uint32_t inner,uint32_t outer,int outline){
  if(!R||rx<0||ry<0) return;
  draw_circle_colour_prim_ex(R,(double)cx,(double)cy,(double)rx,(double)ry,inner,outer,outline);
}

typedef struct {
  GmlRender *render;
  int min_x,min_y,max_x;
  double denominator,a_step,b_step,a_row,b_row,a_row_step,b_row_step;
  uint32_t source;
} GmlOpaqueTriangleRows;
static void draw_opaque_triangle_rows(void *context,int row_start,int row_end,int slot){
  (void)slot;
  GmlOpaqueTriangleRows *rows=(GmlOpaqueTriangleRows*)context;
  double sign=rows->denominator>0.0?1.0:-1.0;
  int width=rows->max_x-rows->min_x+1;
  for(int row=row_start;row<row_end;row++){
    double a_row=rows->a_row+rows->a_row_step*row;
    double b_row=rows->b_row+rows->b_row_step*row;
    double value[3]={
      sign*a_row,
      sign*b_row,
      sign*(rows->denominator-a_row-b_row)
    };
    double step[3]={
      sign*rows->a_step,
      sign*rows->b_step,
      sign*(-rows->a_step-rows->b_step)
    };
    int first=0,last=width-1;
    for(int edge=0;edge<3 && first<=last;edge++){
      if(step[edge]>0.0){
        int bound=(int)ceil(-value[edge]/step[edge]);
        if(bound>first) first=bound;
      } else if(step[edge]<0.0){
        int bound=(int)floor(value[edge]/-step[edge]);
        if(bound<last) last=bound;
      } else if(value[edge]<0.0) first=last+1;
    }
    if(first<0) first=0;
    if(last>=width) last=width-1;
    while(first<=last &&
          (value[0]+step[0]*first<0.0 ||
           value[1]+step[1]*first<0.0 ||
           value[2]+step[2]*first<0.0)) first++;
    while(first<=last &&
          (value[0]+step[0]*last<0.0 ||
           value[1]+step[1]*last<0.0 ||
           value[2]+step[2]*last<0.0)) last--;
    if(first<=last)
      gml_render_backend_fill_xrgb(
        rows->render->fb+(size_t)(rows->min_y+row)*rows->render->fbw+
          rows->min_x+first,
        last-first+1,rows->source);
  }
}

/* Instance-owned immediate-mode primitive buffer used between begin/end. */
static void prim_tri_fill_ex(GmlRender *R, double X1,double Y1,double X2,double Y2,double X3,double Y3,
                             uint32_t col, double alpha, int outline){
  if(!R||!R->fb||R->fbw<=0||R->fbh<=0) return;
  if(!isfinite(X1)||!isfinite(Y1)||!isfinite(X2)||!isfinite(Y2)||!isfinite(X3)||!isfinite(Y3)) return;
  if(!isfinite(alpha)) return;
  if(alpha>1) alpha=1; else if(alpha<0) alpha=0;
  if(alpha<=0) return;
  double den=(Y2-Y3)*(X1-X3)+(X3-X2)*(Y1-Y3);
  if(!isfinite(den)||fabs(den)<1e-9) return;
  double minxd=floor(fmin(X1,fmin(X2,X3))), maxxd=floor(fmax(X1,fmax(X2,X3)));
  double minyd=floor(fmin(Y1,fmin(Y2,Y3))), maxyd=floor(fmax(Y1,fmax(Y2,Y3)));
  if(maxxd<0.0||maxyd<0.0||minxd>(double)(R->fbw-1)||minyd>(double)(R->fbh-1)) return;
  int minx=minxd<0.0?0:(int)minxd, maxx=maxxd>=(double)R->fbw?R->fbw-1:(int)maxxd;
  int miny=minyd<0.0?0:(int)minyd, maxy=maxyd>=(double)R->fbh?R->fbh-1:(int)maxyd;
  int opaque_span=!outline && alpha>=1.0 && R->blendmode==0 &&
    R->blend_equation==1 && R->blend_equation_alpha==1 &&
    R->color_write_mask==15 &&
    fabs(X1-nearbyint(X1))<1e-9 && fabs(Y1-nearbyint(Y1))<1e-9 &&
    fabs(X2-nearbyint(X2))<1e-9 && fabs(Y2-nearbyint(Y2))<1e-9 &&
    fabs(X3-nearbyint(X3))<1e-9 && fabs(Y3-nearbyint(Y3))<1e-9;
  if(opaque_span){
    GmlOpaqueTriangleRows rows={
      R,minx,miny,maxx,den,
      Y2-Y3,Y3-Y1,
      (Y2-Y3)*(minx-X3)+(X3-X2)*(miny-Y3),
      (Y3-Y1)*(minx-X3)+(X1-X3)*(miny-Y3),
      X3-X2,X1-X3,
      gml_render_backend_color_to_xrgb(col)
    };
    gml_render_maybe_prepare_draw(R);
    R->fb_all_transparent=0;
    if((size_t)(maxx-minx+1)*(size_t)(maxy-miny+1)>=262144u)
      gml_run_row_bands(R,maxy-miny+1,draw_opaque_triangle_rows,&rows);
    else
      draw_opaque_triangle_rows(&rows,0,maxy-miny+1,0);
    return;
  }
  for(int yy=miny;yy<=maxy;yy++) for(int xx=minx;xx<=maxx;xx++){
    double a0=((Y2-Y3)*(xx-X3)+(X3-X2)*(yy-Y3))/den;
    double b0=((Y3-Y1)*(xx-X3)+(X1-X3)*(yy-Y3))/den;
    double c0=1-a0-b0;
    if(outline && a0>0.04&&b0>0.04&&c0>0.04) continue;
    if(a0>=0&&b0>=0&&c0>=0) gml_render_backend_draw_pixel_alpha(R,xx,yy,col,alpha);
  }
}


void gml_render_primitive_point(GmlRender *render,
                                int x,int y,uint32_t color){
  gml_render_maybe_prepare_draw(render);
  gml_render_backend_draw_pixel(render,x,y,color);
}


void gml_render_primitive_rectangle(GmlRender *render,
                                    int x1,int y1,int x2,int y2,
                                    uint32_t color,int outline){
  if(!outline && gml_render_shade_target_rect(render,x1,y1,x2,y2,color,render?render->alpha:1.0))
    return;
  draw_rect_prim(render,x1,y1,x2,y2,color,outline);
}

void gml_render_primitive_rectangle_color(GmlRender *render,
                                          int x1,int y1,int x2,int y2,
                                          uint32_t color1,uint32_t color2,
                                          uint32_t color3,uint32_t color4,
                                          int outline){
  if(!outline && color1==color2 && color2==color3 && color3==color4 &&
     gml_render_shade_target_rect(render,x1,y1,x2,y2,color1,render?render->alpha:1.0)) return;
  draw_rect_colour_prim(render,x1,y1,x2,y2,color1,color2,color3,color4,outline);
}

void gml_render_primitive_line(GmlRender *render,
                               int x1,int y1,int x2,int y2,
                               uint32_t color,int width){
  draw_line_colour_prim(render,x1,y1,x2,y2,color,color,width);
}

void gml_render_primitive_line_color(GmlRender *render,
                                     int x1,int y1,int x2,int y2,
                                     uint32_t color1,uint32_t color2,
                                     int width){
  draw_line_colour_prim(render,x1,y1,x2,y2,color1,color2,width);
}

void gml_render_primitive_line_color_subpixel(GmlRender *render,
                                              double x1,double y1,
                                              double x2,double y2,
                                              uint32_t color1,uint32_t color2,
                                              int width){
  draw_line_colour_subpixel_prim(render,x1,y1,x2,y2,color1,color2,width);
}

void gml_render_primitive_circle(GmlRender *render,
                                 int center_x,int center_y,int radius_x,int radius_y,
                                 uint32_t color,int outline){
  draw_circle_prim(render,center_x,center_y,radius_x,radius_y,color,outline);
}

void gml_render_primitive_circle_subpixel(GmlRender *render,
                                          double center_x,double center_y,
                                          double radius_x,double radius_y,
                                          uint32_t color,int outline){
  draw_circle_prim_ex(render,center_x,center_y,radius_x,radius_y,color,outline);
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

void gml_render_primitive_circle_color(GmlRender *render,
                                       int center_x,int center_y,int radius_x,int radius_y,
                                       uint32_t inner,uint32_t outer,int outline){
  draw_circle_colour_prim(render,center_x,center_y,radius_x,radius_y,inner,outer,outline);
}

void gml_render_primitive_circle_color_subpixel(GmlRender *render,
                                                double center_x,double center_y,
                                                double radius_x,double radius_y,
                                                uint32_t inner,uint32_t outer,int outline){
  draw_circle_colour_prim_ex(render,center_x,center_y,radius_x,radius_y,inner,outer,outline);
}

void gml_render_primitive_triangle_alpha(GmlRender *render,
                                         double x1,double y1,double x2,double y2,
                                         double x3,double y3,uint32_t color,
                                         double alpha,int outline){
  prim_tri_fill_ex(render,x1,y1,x2,y2,x3,y3,color,alpha,outline);
}
