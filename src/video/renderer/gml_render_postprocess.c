/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Recognized display post-processes and their complete software pixel kernels. The one family
 * kept here is the two-sample channel offset, whose parameters are read from the content and whose
 * operation - two lookups, per-channel gains, a sum - carries no expression of its own. */
#include "gml_render.h"
#include "gml_render_internal.h"
#include "gml_render_pixel_internal.h"
#include "gml_render_backend.h"

#include "anygm_host.h"
#include "gml_thread.h"

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static inline int dual_u8(float v){
  int n=(int)floorf(v*255.0f+0.5f);
  return n<0?0:(n>255?255:n);
}
static inline void dual_texel(const uint32_t *src,int sw,int sh,double x,double y,int interp,float out[4]){
  if(!interp){
    int ix=(int)floor(x),iy=(int)floor(y);
    if(ix<0)ix=0; else if(ix>=sw)ix=sw-1;
    if(iy<0)iy=0; else if(iy>=sh)iy=sh-1;
    uint32_t v=src[(size_t)iy*sw+ix];
    out[0]=((v>>16)&255)*(1.0f/255.0f); out[1]=((v>>8)&255)*(1.0f/255.0f);
    out[2]=(v&255)*(1.0f/255.0f); out[3]=(v>>24)*(1.0f/255.0f);
    return;
  }
  /* Texture coordinates refer to texel centres at n+0.5. Clamp-to-edge matches GameMaker's
   * default non-repeating surface sampler. */
  double fx=x-0.5,fy=y-0.5; int x0=(int)floor(fx),y0=(int)floor(fy);
  float tx=(float)(fx-x0),ty=(float)(fy-y0);
  int x1=x0+1,y1=y0+1;
  if(x0<0)x0=0; else if(x0>=sw)x0=sw-1; if(x1<0)x1=0; else if(x1>=sw)x1=sw-1;
  if(y0<0)y0=0; else if(y0>=sh)y0=sh-1; if(y1<0)y1=0; else if(y1>=sh)y1=sh-1;
  uint32_t p[4]={src[(size_t)y0*sw+x0],src[(size_t)y0*sw+x1],
                 src[(size_t)y1*sw+x0],src[(size_t)y1*sw+x1]};
  float w[4]={(1-tx)*(1-ty),tx*(1-ty),(1-tx)*ty,tx*ty};
  for(int c=0;c<4;c++) out[c]=0;
  for(int i=0;i<4;i++){
    out[0]+=((p[i]>>16)&255)*(1.0f/255.0f)*w[i];
    out[1]+=((p[i]>>8)&255)*(1.0f/255.0f)*w[i];
    out[2]+=(p[i]&255)*(1.0f/255.0f)*w[i];
    out[3]+=(p[i]>>24)*(1.0f/255.0f)*w[i];
  }
}
typedef struct {
  GmlRender *r; const uint32_t *src; int sw,sh,x0,y0,W,H,sx,sy,shift,axis;
  float base[3][256], shifted[3][256];
} DualFastCtx;
static void dual_fast_band(void *p,int py0,int py1,int slot){
  (void)slot; DualFastCtx *c=(DualFastCtx*)p;
  for(int py=py0;py<py1;py++){
    int ty=c->y0+py; if(ty<0||ty>=c->r->fbh) continue;
    int ay=c->sy+py; if(ay<0)ay=0; else if(ay>=c->sh)ay=c->sh-1;
    int by=ay;
    if(c->axis){ by=c->sy+py+c->shift; if(by<0)by=0; else if(by>=c->sh)by=c->sh-1; }
    const uint32_t *arow=c->src+(size_t)ay*c->sw,*brow=c->src+(size_t)by*c->sw;
    uint32_t *drow=c->r->fb+(size_t)ty*c->r->fbw;
    for(int px=0;px<c->W;px++){
      int tx=c->x0+px; if(tx<0||tx>=c->r->fbw) continue;
      int ax=c->sx+px; if(ax<0)ax=0; else if(ax>=c->sw)ax=c->sw-1;
      int bx=ax;
      if(!c->axis){ bx=c->sx+px+c->shift; if(bx<0)bx=0; else if(bx>=c->sw)bx=c->sw-1; }
      uint32_t a=arow[ax],b=brow[bx];
      int rr=(int)(c->base[0][(a>>16)&255]+c->shifted[0][(b>>16)&255]+0.5f);
      int gg=(int)(c->base[1][(a>>8)&255]+c->shifted[1][(b>>8)&255]+0.5f);
      int bb=(int)(c->base[2][a&255]+c->shifted[2][b&255]+0.5f);
      if(rr>255) rr=255;
      if(gg>255) gg=255;
      if(bb>255) bb=255;
      drow[tx]=0xFF000000u|((uint32_t)rr<<16)|((uint32_t)gg<<8)|(uint32_t)bb;
    }
  }
}
/* Software execution of the structurally parsed two-sample offset fragment. Unlike a brightness
 * approximation, this preserves the second lookup, its channel isolation and sub-texel offsets. */
void draw_surface_dual_sample(GmlRender *r,const struct GmlShaderPal *sp,int surf,
                                     double sx,double sy,double swd,double shd,
                                     double dx,double dy,double dw,double dh,uint32_t blend,double alpha){
  int sw=0,sh=0; uint32_t *src=surface_pixels(r,surf,&sw,&sh);
  if(!r||!r->fb||!src||sw<=0||sh<=0||swd<=0||shd<=0||alpha<=0) return;
  if(alpha>1) alpha=1;
  dx-=r->cam_x; dy-=r->cam_y;
  int x0=(int)floor(dx),y0=(int)floor(dy),W=(int)lround(dw),H=(int)lround(dh);
  if(W==0||H==0) return;
  int flipx=W<0,flipy=H<0; if(W<0)W=-W; if(H<0)H=-H;
  uint32_t *copy=NULL;
  if(src==r->fb){
    gml_render_maybe_prepare_draw(r);
    copy=malloc((size_t)sw*sh*sizeof(*copy)); if(!copy)return;
    memcpy(copy,src,(size_t)sw*sh*sizeof(*copy)); src=copy;
  }
  gml_render_maybe_prepare_draw(r);
  float tint[4]={ (blend&255)*(1.0f/255.0f),((blend>>8)&255)*(1.0f/255.0f),
                  ((blend>>16)&255)*(1.0f/255.0f),(float)alpha };
  double shift=(double)sp->dual_sign*sp->dual_value[0]*sp->dual_value[1]*
               (sp->dual_axis?sh:sw);
  /* Full-resolution post-processes are by far the common case. Avoid per-pixel coordinate math,
   * float normalization and blending when the source is known opaque and the shader result is
   * likewise opaque. Row bands keep a 1080p software pass comfortably real-time. */
  int isx=(int)lround(sx),isy=(int)lround(sy),ishift=(int)lround(shift);
  int nonnegative=1;
  for(int c=0;c<3;c++) if(sp->dual_base_gain[c]<0 || sp->dual_shift_gain[c]<0) nonnegative=0;
  if(!r->interp&&!flipx&&!flipy && W==(int)lround(swd) && H==(int)lround(shd) &&
     fabs(sx-isx)<0.0001 && fabs(sy-isy)<0.0001 && fabs(shift-ishift)<0.0001 &&
     (blend&0xFFFFFFu)==0xFFFFFFu && alpha>=1.0 && r->blendmode==0 &&
     r->color_write_mask==0x0F && surface_known_opaque(r,surf) && nonnegative &&
     sp->dual_base_gain[3]+sp->dual_shift_gain[3]>=1.0f){
    DualFastCtx c={.r=r,.src=src,.sw=sw,.sh=sh,.x0=x0,.y0=y0,.W=W,.H=H,
                   .sx=isx,.sy=isy,.shift=ishift,.axis=sp->dual_axis};
    for(int ch=0;ch<3;ch++) for(int v=0;v<256;v++){
      c.base[ch][v]=v*sp->dual_base_gain[ch];
      c.shifted[ch][v]=v*sp->dual_shift_gain[ch];
    }
    if(render_setting(r,"GML_LOG_SHADER") && r->dual_shader_fast_log_count++==0)
      anygm_host_logf(r && r->win ? r->win->host : NULL,ANYGM_LOG_DEBUG,"[shader] dual fast path %dx%d integer-shift=%d\n",W,H,ishift);
    gml_run_row_bands(r,H,dual_fast_band,&c);
    if(x0<=0&&y0<=0&&x0+W>=r->fbw&&y0+H>=r->fbh){
      r->fb_opaque_known=1; r->fb_all_opaque=1; r->fb_all_transparent=0;
    } else r->fb_opaque_known=0;
    free(copy); return;
  }
  for(int py=0;py<H;py++){
    int ty=y0+py; if(ty<0||ty>=r->fbh) continue;
    int dpy=flipy?(H-1-py):py;
    double fy=sy+((dpy+0.5)*shd)/H;
    for(int px=0;px<W;px++){
      int tx=x0+px; if(tx<0||tx>=r->fbw) continue;
      int dpx=flipx?(W-1-px):px;
      double fx=sx+((dpx+0.5)*swd)/W;
      float a[4],b[4];
      dual_texel(src,sw,sh,fx,fy,r->interp,a);
      dual_texel(src,sw,sh,fx+(sp->dual_axis?0:shift),fy+(sp->dual_axis?shift:0),r->interp,b);
      float fc[4];
      for(int c=0;c<4;c++) fc[c]=a[c]*tint[c]*sp->dual_base_gain[c]+b[c]*sp->dual_shift_gain[c];
      int sr=dual_u8(fc[0]),sg=dual_u8(fc[1]),sb=dual_u8(fc[2]),sa=dual_u8(fc[3]);
      uint32_t *dp=&r->fb[(size_t)ty*r->fbw+tx],old=*dp,out=old;
      if(r->blendmode==3){
        out=blend_multiply_pixel(r,old,sr,sg,sb);
      } else if(r->blendmode==4){
        out=blend_max_preset_pixel(r,old,sr,sg,sb,(unsigned)sa);
      } else if(r->blendmode==1 || r->blendmode==2){
        int add=r->blendmode==1,dr=(old>>16)&255,dg=(old>>8)&255,db=old&255;
        int rr,gg,bb,oa;
        if(add){
          rr=dr+sr*sa/255; gg=dg+sg*sa/255; bb=db+sb*sa/255;
          if(rr>255) rr=255;
          if(gg>255) gg=255;
          if(bb>255) bb=255;
          oa=r->target_sp==0?255:(int)(old>>24)+sa; if(oa>255)oa=255;
        } else {
          rr=(int)gml_blend_inv_source_u8((unsigned)dr,(unsigned)sr);
          gg=(int)gml_blend_inv_source_u8((unsigned)dg,(unsigned)sg);
          bb=(int)gml_blend_inv_source_u8((unsigned)db,(unsigned)sb);
          oa=r->target_sp==0?255:(int)gml_blend_inv_source_u8(old>>24,(unsigned)sa);
        }
        out=((uint32_t)oa<<24)|((uint32_t)rr<<16)|((uint32_t)gg<<8)|(uint32_t)bb;
      } else if(!r->alphablend || sa>=255){
        out=((uint32_t)(r->target_sp==0?255:sa)<<24)|((uint32_t)sr<<16)|((uint32_t)sg<<8)|(uint32_t)sb;
      } else if(sa){
        int ia=255-sa,dr=(old>>16)&255,dg=(old>>8)&255,db=old&255,da=old>>24;
        int rr=(sr*sa+dr*ia+127)/255,gg=(sg*sa+dg*ia+127)/255,bb=(sb*sa+db*ia+127)/255;
        int oa=r->target_sp==0?255:sa+(da*ia+127)/255; if(oa>255)oa=255;
        out=((uint32_t)oa<<24)|((uint32_t)rr<<16)|((uint32_t)gg<<8)|(uint32_t)bb;
      }
      *dp=color_write_merge(r,old,out);
    }
  }
  r->fb_opaque_known=0;
  free(copy);
}
