/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef GML_RENDER_SAMPLE_SETUP_H
#define GML_RENDER_SAMPLE_SETUP_H

#include <math.h>

/* Non-reentrant backend preparation of repeated coordinates and filter weights.
 * This leaf has no renderer context and never reads or writes pixels. */
typedef struct GmlRenderSampleSetup {
  int x0,y0,x1,y1,ix,iy;
  double weight[4];
} GmlRenderSampleSetup;

static inline int gml_render_cpu_backend_sample_setup_repeat(double u,double v,int width,int height,
                                                 int interpolate,GmlRenderSampleSetup *out){
  if(!out || width<=0 || height<=0 || !isfinite(u) || !isfinite(v))return 0;
  u-=floor(u);v-=floor(v);
  double fx=u*width-0.5,fy=v*height-0.5;
  int x0=(int)floor(fx),y0=(int)floor(fy);
  double ax=fx-floor(fx),ay=fy-floor(fy);
  x0%=width;y0%=height;
  if(x0<0)x0+=width;
  if(y0<0)y0+=height;
  if(!interpolate){
    x0=(x0+(ax>=0.5))%width;
    y0=(y0+(ay>=0.5))%height;
  }
  out->x0=x0;out->y0=y0;out->x1=(x0+1)%width;out->y1=(y0+1)%height;
  enum {LEVELS=1<<8};
  int ix=(int)floor(ax*LEVELS+.5),iy=(int)floor(ay*LEVELS+.5);
  if(ix<0)ix=0;else if(ix>LEVELS)ix=LEVELS;
  if(iy<0)iy=0;else if(iy>LEVELS)iy=LEVELS;
  out->ix=ix;out->iy=iy;
  out->weight[0]=(1-ax)*(1-ay);out->weight[1]=ax*(1-ay);
  out->weight[2]=(1-ax)*ay;out->weight[3]=ax*ay;
  return 1;
}
#endif
