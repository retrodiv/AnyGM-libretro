/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Header-local hot helpers shared by sprite and surface composition kernels. */
#ifndef GML_RENDER_BLIT_INTERNAL_H
#define GML_RENDER_BLIT_INTERNAL_H

#include "gml_render_sampling_internal.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(__SSE2__)
#include <emmintrin.h>
#endif
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#endif


/* Modern format semantics rasterize a cardinally rotated textured rectangle as a half-open cell. When a
 * source basis points toward a negative destination axis, its outer integer edge belongs to the
 * preceding pixel.  Applying that one-pixel anchor correction also makes the four 90-degree
 * quadrants meet without doubled/separated seams.  Arbitrary-angle coverage keeps the ordinary
 * pixel-centre inverse map below. */
static inline void render_modern_cardinal_anchor(const GmlRender *r,double degrees,
                                                  double xs,double ys,double cosine,double sine,
                                                  double *x,double *y){
  double quadrant=nearbyint(degrees/90.0);
  if(!r || r->classic || fabs(degrees-quadrant*90.0)>=1e-10) return;
  if(xs*cosine < -1e-12 || ys*sine < -1e-12) *x-=1.0;
  if(-xs*sine < -1e-12 || ys*cosine < -1e-12) *y-=1.0;
}

static inline uint32_t blend_fast8_cached(uint32_t dst, uint32_t srb, uint32_t sg, uint32_t ia){
  uint32_t rb=((srb+(dst&0x00FF00FFu)*ia)>>8)&0x00FF00FFu;
  uint32_t g=((sg+(dst&0x0000FF00u)*ia)>>8)&0x0000FF00u;
  return 0xFF000000u|rb|g;
}

static inline int row_all_opaque32(const uint32_t *sp, int run){
  if(run<=0) return 1;
#if defined(__SSE2__)
  __m128i mask=_mm_set1_epi32((int)0xFF000000u);
  while(run>=4){
    __m128i v=_mm_loadu_si128((const __m128i*)sp);
    __m128i a=_mm_and_si128(v,mask);
    __m128i c=_mm_cmpeq_epi32(a,mask);
    if(_mm_movemask_epi8(c)!=0xFFFF) return 0;
    sp+=4;
    run-=4;
  }
#elif defined(__aarch64__) && (defined(__ARM_NEON) || defined(__ARM_NEON__))
  uint32x4_t mask=vdupq_n_u32(0xFF000000u);
  while(run>=4){
    uint32x4_t v=vld1q_u32(sp);
    uint32x4_t c=vceqq_u32(vandq_u32(v,mask),mask);
    uint64x2_t q=vreinterpretq_u64_u32(c);
    if(vgetq_lane_u64(q,0)!=(uint64_t)~0ull || vgetq_lane_u64(q,1)!=(uint64_t)~0ull) return 0;
    sp+=4;
    run-=4;
  }
#endif
  for(int k=0; k<run; k++) if((sp[k]>>24)!=255u) return 0;
  return 1;
}

static inline void blend_fast8_src_run(uint32_t *dp, const uint32_t *sp, int run, uint32_t af){
  if(run<=0) return;
  if(af>=256u){ memcpy(dp,sp,(size_t)run*sizeof(uint32_t)); return; }
  if(!af) return;
  uint32_t ia=256u-af;
#if defined(__SSE2__)
  if(run>=4){
    __m128i zero=_mm_setzero_si128();
    __m128i valpha=_mm_set1_epi32((int)0xFF000000u);
    __m128i vaf=_mm_set1_epi16((short)af);
    __m128i via=_mm_set1_epi16((short)ia);
    while(run>=4){
      __m128i src=_mm_loadu_si128((const __m128i*)sp);
      __m128i dst=_mm_loadu_si128((const __m128i*)dp);
      __m128i slo=_mm_unpacklo_epi8(src,zero);
      __m128i shi=_mm_unpackhi_epi8(src,zero);
      __m128i dlo=_mm_unpacklo_epi8(dst,zero);
      __m128i dhi=_mm_unpackhi_epi8(dst,zero);
      slo=_mm_add_epi16(_mm_mullo_epi16(slo,vaf),_mm_mullo_epi16(dlo,via));
      shi=_mm_add_epi16(_mm_mullo_epi16(shi,vaf),_mm_mullo_epi16(dhi,via));
      slo=_mm_srli_epi16(slo,8);
      shi=_mm_srli_epi16(shi,8);
      _mm_storeu_si128((__m128i*)dp,_mm_or_si128(_mm_packus_epi16(slo,shi),valpha));
      sp+=4;
      dp+=4;
      run-=4;
    }
  }
#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
  if(run>=4){
    uint8x16_t valpha8=vreinterpretq_u8_u32(vdupq_n_u32(0xFF000000u));
    uint16x8_t vaf=vdupq_n_u16((uint16_t)af);
    uint16x8_t via=vdupq_n_u16((uint16_t)ia);
    while(run>=4){
      uint8x16_t src=vld1q_u8((const uint8_t*)sp);
      uint8x16_t dst=vld1q_u8((const uint8_t*)dp);
      uint16x8_t lo=vaddq_u16(vmulq_u16(vmovl_u8(vget_low_u8(src)),vaf),
                              vmulq_u16(vmovl_u8(vget_low_u8(dst)),via));
      uint16x8_t hi=vaddq_u16(vmulq_u16(vmovl_u8(vget_high_u8(src)),vaf),
                              vmulq_u16(vmovl_u8(vget_high_u8(dst)),via));
      lo=vshrq_n_u16(lo,8);
      hi=vshrq_n_u16(hi,8);
      vst1q_u8((uint8_t*)dp,vorrq_u8(vcombine_u8(vmovn_u16(lo),vmovn_u16(hi)),valpha8));
      sp+=4;
      dp+=4;
      run-=4;
    }
  }
#endif
  for(int k=0;k<run;k++){
    uint32_t src=sp[k];
    uint32_t srb=(src & 0x00FF00FFu)*af;
    uint32_t sg=(src & 0x0000FF00u)*af;
    dp[k]=blend_fast8_cached(dp[k],srb,sg,ia);
  }
}

/* --- threaded bodies of the two interpolated-magnify blits (see draw_surface_region / draw_sprite_
 * stretched). Per-column/row taps are precomputed by the caller; each band writes disjoint rows. --- */
typedef struct { GmlRender *r; const uint32_t *src; const int *cxa,*cxb; const float *cfx;
  int sw,sh,W,H,x0,y0; double sy0d,shd,sy_phase,sy_bias;
  float fa,fbRr,fbGg,fbBb; int noblend; } SurfBiCtx;
static inline void surf_bi_band(void *p, int py0, int py1, int slot){
  (void)slot;
  SurfBiCtx *c=(SurfBiCtx*)p; GmlRender *r=c->r; const int W=c->W, sw=c->sw, sh=c->sh;
  for(int py=py0; py<py1; py++){ int ty_=c->y0+py; if(ty_<0||ty_>=r->fbh) continue;
    double fsy=c->sy0d+(py+c->sy_phase)*c->shd/c->H+c->sy_bias;
    int sya=(int)floor(fsy); float fy=(float)(fsy-sya);
    int syb=sya+1; if(sya<0)sya=0; else if(sya>sh-1)sya=sh-1; if(syb<0)syb=0; else if(syb>sh-1)syb=sh-1;
    const uint32_t *rowa=c->src+(size_t)sya*sw, *rowb=c->src+(size_t)syb*sw;
    uint32_t *dprow=r->fb+(size_t)ty_*r->fbw; float wy0=1.0f-fy;
    for(int px=0; px<W; px++){ int tx_=c->x0+px; if(tx_<0||tx_>=r->fbw) continue;
      int sxa=c->cxa[px], sxb=c->cxb[px]; float fx=c->cfx[px], wx0=1.0f-fx;
      uint32_t p00=rowa[sxa],p01=rowa[sxb],p10=rowb[sxa],p11=rowb[sxb];
      float w00=wx0*wy0,w01=fx*wy0,w10=wx0*fy,w11=fx*fy;
      float Av=(p00>>24)*w00+(p01>>24)*w01+(p10>>24)*w10+(p11>>24)*w11;
      if(shader_discards_alpha_value(r,Av)) continue;
      float ea=(Av*(1.0f/255.0f))*c->fa; if(ea<=0.0f) continue;
      float Rv=((p00>>16)&0xFF)*w00+((p01>>16)&0xFF)*w01+((p10>>16)&0xFF)*w10+((p11>>16)&0xFF)*w11;
      float Gv=((p00>>8)&0xFF)*w00+((p01>>8)&0xFF)*w01+((p10>>8)&0xFF)*w10+((p11>>8)&0xFF)*w11;
      float Bv=(p00&0xFF)*w00+(p01&0xFF)*w01+(p10&0xFF)*w10+(p11&0xFF)*w11;
      float sr=Rv*c->fbRr, sg=Gv*c->fbGg, sb=Bv*c->fbBb; uint32_t *dp=&dprow[tx_];
      if(c->noblend || ea>=1.0f){ *dp=0xFF000000u|((uint32_t)(sr+0.5f)<<16)|((uint32_t)(sg+0.5f)<<8)|(uint32_t)(sb+0.5f); }
      else { float ia=1.0f-ea; int dr=(*dp>>16)&0xFF,dg=(*dp>>8)&0xFF,db=*dp&0xFF;
        *dp=0xFF000000u|((int)(sr*ea+dr*ia+0.5f)<<16)|((int)(sg*ea+dg*ia+0.5f)<<8)|(int)(sb*ea+db*ia+0.5f); }
    }
  }
}


#endif
