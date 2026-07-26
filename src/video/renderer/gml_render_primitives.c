/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Renderer-owned fixed-function primitive kernels and typed operations. */
#include <math.h>
#include <stdlib.h>

#include "gml_render.h"
#include "gml_render_backend.h"
#include "gml_render_internal.h"

static void draw_rect_prim_alpha(GmlRender *R, int x1, int y1, int x2, int y2, uint32_t gmcol, int outline, double alpha){
  if(!R) return;
  if(x1>x2){ int t=x1; x1=x2; x2=t; }
  if(y1>y2){ int t=y1; y1=y2; y2=t; }
  if(x2<0||y2<0||x1>=R->fbw||y1>=R->fbh) return;
  if(x1<0) x1=0;
  if(y1<0) y1=0;
  if(x2>=R->fbw) x2=R->fbw-1;
  if(y2>=R->fbh) y2=R->fbh-1;
  if(alpha>1) alpha=1; else if(alpha<0) alpha=0;
  if(!outline && alpha<=0) return;
  /* An untextured fragment shader receives the primitive coordinates and produces its own colour;
   * draw_set_color/alpha need not affect it when the shader source does not consume vertex colour. */
  if(!outline && gml_render_shader_fill_rect(R,x1,y1,x2+1,y2+1)) return;
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
    if(outline && y>y1 && y<y2 && x>x1 && x<x2) continue;
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
  if(x1<0) x1=0;
  if(y1<0) y1=0;
  if(x2>=R->fbw) x2=R->fbw-1;
  if(y2>=R->fbh) y2=R->fbh-1;
  double alpha=R->alpha; if(alpha>1) alpha=1; else if(alpha<0) alpha=0;
  if(alpha<=0) return;
  if(alpha>=1.0 || !R->alphablend) gml_render_maybe_prepare_opaque_rect(R,x1,y1,x2+1,y2+1);
  else gml_render_maybe_prepare_draw(R);
  uint32_t tl=gml_render_backend_color_to_xrgb(c1), tr=gml_render_backend_color_to_xrgb(c2), br=gml_render_backend_color_to_xrgb(c3), bl=gml_render_backend_color_to_xrgb(c4);
  int wden=x2-x1, hden=y2-y1, n=x2-x1+1;
  if(tl==tr && bl==br){
    for(int y=y1;y<=y2;y++){
      uint32_t rowc=gml_render_backend_lerp_xrgb(tl,bl,y-y1,hden);
      gml_render_backend_draw_xrgb_alpha(R,R->fb+(size_t)y*R->fbw+x1,n,rowc,alpha);
    }
    return;
  }
  for(int y=y1;y<=y2;y++){
    uint32_t lc=gml_render_backend_lerp_xrgb(tl,bl,y-y1,hden);
    uint32_t rc=gml_render_backend_lerp_xrgb(tr,br,y-y1,hden);
    uint32_t *row=R->fb+(size_t)y*R->fbw+x1;
    if(lc==rc){ gml_render_backend_draw_xrgb_alpha(R,row,n,lc,alpha); continue; }
    for(int x=0;x<n;x++){
      uint32_t c=gml_render_backend_lerp_xrgb(lc,rc,x,wden);
      gml_render_backend_draw_xrgb_alpha(R,row+x,1,c,alpha);
    }
  }
}
static void draw_line_prim(GmlRender *R, int x0, int y0, int x1, int y1, uint32_t gmcol, int width){
  if(width<1) width=1;
  int dx=abs(x1-x0), sx=x0<x1?1:-1;
  int dy=-abs(y1-y0), sy=y0<y1?1:-1;
  int err=dx+dy;
  for(;;){
    int r=width/2;
    draw_rect_prim(R,x0-r,y0-r,x0-r+width-1,y0-r+width-1,gmcol,0);
    if(x0==x1 && y0==y1) break;
    int e2=2*err;
    if(e2>=dy){ err+=dy; x0+=sx; }
    if(e2<=dx){ err+=dx; y0+=sy; }
  }
}


static void draw_circle_prim(GmlRender *R, int cx, int cy, int rx, int ry, uint32_t gmcol, int outline){
  if(!R||rx<0||ry<0) return;
  if(rx==0||ry==0){ gml_render_backend_draw_pixel(R,cx,cy,gmcol); return; }
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
    long long raw_y0=(long long)cy-ry,raw_y1=(long long)cy+ry;
    if(raw_y1<0 || raw_y0>=R->fbh) return;
    int y0=raw_y0<0?0:(int)raw_y0;
    int y1=raw_y1>=R->fbh?R->fbh-1:(int)raw_y1;
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

/* Colour variants interpolate from the first colour at the centre to the second at the perimeter. */
static uint32_t gm_color_lerp_fan(uint32_t inner,uint32_t outer,double amount){
  if(amount<=0.0) return inner;
  if(amount>=1.0) return outer;
  int ir=inner&255,ig=(inner>>8)&255,ib=(inner>>16)&255;
  int or_=outer&255,og=(outer>>8)&255,ob=(outer>>16)&255;
  int r=(int)(ir+(or_-ir)*amount);
  int g=(int)(ig+(og-ig)*amount);
  int b=(int)(ib+(ob-ib)*amount);
  return (uint32_t)r|((uint32_t)g<<8)|((uint32_t)b<<16);
}
static void draw_px_fan(GmlRender *R,int x,int y,uint32_t inner,uint32_t outer,double amount){
  if(R && R->alphablend && R->blendmode==2 &&
     R->blend_equation==1 && R->blend_equation_alpha==1 &&
     x>=0 && y>=0 && x<R->fbw && y<R->fbh && R->alpha>0.0){
    if(amount<0.0) amount=0.0; else if(amount>1.0) amount=1.0;
    if(R->pending_underlay || R->pending_fill) gml_render_prepare_draw(R);
    R->fb_all_transparent=0;
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
static void draw_circle_colour_prim(GmlRender *R,int cx,int cy,int rx,int ry,
                                    uint32_t inner,uint32_t outer,int outline){
  if(!R||rx<0||ry<0) return;
  if(inner==outer){ draw_circle_prim(R,cx,cy,rx,ry,inner,outline); return; }
  if(rx==0||ry==0){ gml_render_backend_draw_pixel(R,cx,cy,inner); return; }
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
  long long raw_y0=(long long)cy-ry,raw_y1=(long long)cy+ry;
  if(raw_y1<0 || raw_y0>=R->fbh) return;
  int y0=raw_y0<0?0:(int)raw_y0;
  int y1=raw_y1>=R->fbh?R->fbh-1:(int)raw_y1;
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
  draw_rect_prim(render,x1,y1,x2,y2,color,outline);
}

void gml_render_primitive_rectangle_color(GmlRender *render,
                                          int x1,int y1,int x2,int y2,
                                          uint32_t color1,uint32_t color2,
                                          uint32_t color3,uint32_t color4,
                                          int outline){
  draw_rect_colour_prim(render,x1,y1,x2,y2,color1,color2,color3,color4,outline);
}

void gml_render_primitive_line(GmlRender *render,
                               int x1,int y1,int x2,int y2,
                               uint32_t color,int width){
  draw_line_prim(render,x1,y1,x2,y2,color,width);
}

void gml_render_primitive_circle(GmlRender *render,
                                 int center_x,int center_y,int radius_x,int radius_y,
                                 uint32_t color,int outline){
  draw_circle_prim(render,center_x,center_y,radius_x,radius_y,color,outline);
}

void gml_render_primitive_circle_color(GmlRender *render,
                                       int center_x,int center_y,int radius_x,int radius_y,
                                       uint32_t inner,uint32_t outer,int outline){
  draw_circle_colour_prim(render,center_x,center_y,radius_x,radius_y,inner,outer,outline);
}

void gml_render_primitive_triangle_alpha(GmlRender *render,
                                         double x1,double y1,double x2,double y2,
                                         double x3,double y3,uint32_t color,
                                         double alpha,int outline){
  prim_tri_fill_ex(render,x1,y1,x2,y2,x3,y3,color,alpha,outline);
}
