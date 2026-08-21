/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Cohesive software blit and pixel kernels. */
#include "gml_render_backend.h"
#include "gml_render_blit_internal.h"
#include "gml_render_internal.h"
#include "gml_render_pixel_internal.h"
#include "gml_render_sampling_internal.h"
#include "anygm_compatibility.h"
#include "anygm_host.h"
#include "gml_thread.h"

#include <ctype.h>
#include <limits.h>
#include <stdatomic.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__SSE2__)
#include <emmintrin.h>
#endif
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#endif

#define RFP_SHIFT 20
#define RFP_ONE ((int64_t)1 << RFP_SHIFT)
#define GML_INTERP_DRAW_CACHE_BUDGET (64u*1024u*1024u)
#define GML_INTERP_DRAW_CACHE_MAX_PIXELS (8u*1024u*1024u)
#define GML_INTERP_DRAW_CACHE_MAX_RUNS 262144
#define GML_INTERP_DRAW_CACHE_MIN_PIXELS 16384u
#if defined(__GNUC__) && !defined(__clang__)
#define GML_HOT_RENDER __attribute__((hot,optimize("O3")))
#else
#define GML_HOT_RENDER
#endif

static int log_spr_enabled(const GmlRender *render){
  return render_setting(render,"GML_LOG_SPRITE")!=NULL;
}
static int log_axis_cache_enabled(void){ return 0; }
static int sprof_enabled(void){ return 0; }
static void sprof_add(int sprite,const char *name,double milliseconds){
  (void)sprite;
  (void)name;
  (void)milliseconds;
}

uint32_t gml_render_backend_color_to_xrgb(uint32_t c){
  /* alpha byte 0xFF = drawn/opaque: surfaces track per-pixel coverage in the high byte so
   * draw_clear_alpha(c,0) ("clear to transparent") is representable — see gml_render.h. */
  return 0xFF000000u | ((c&0xff)<<16) | (c&0xff00) | ((c>>16)&0xff);
}
uint32_t gml_render_backend_lerp_xrgb(uint32_t a, uint32_t b, int num, int den){
  if(den<=0 || num<=0) return a;
  if(num>=den) return b;
  int ar=(a>>16)&0xff, ag=(a>>8)&0xff, ab=a&0xff;
  int br=(b>>16)&0xff, bg=(b>>8)&0xff, bb=b&0xff;
  int r=ar + (br-ar)*num/den;
  int g=ag + (bg-ag)*num/den;
  int bl=ab + (bb-ab)*num/den;
  return 0xFF000000u|((uint32_t)r<<16)|((uint32_t)g<<8)|(uint32_t)bl;
}
#if defined(__GNUC__) || defined(__clang__)
typedef uint64_t GmlRenderU64Alias __attribute__((__may_alias__));
#endif
void gml_render_backend_fill_xrgb(uint32_t *dp, int n, uint32_t src){
  if(n<=0) return;
#if defined(__SSE2__)
  /* A fill the size of a virtual monitor is larger than any cache, and an ordinary store has to
   * fetch each line before overwriting all of it -- twice the memory traffic the fill needs. A
   * non-temporal store writes straight out. The values stored are unchanged. */
  if(n>=32768){
    __m128i v=_mm_set1_epi32((int)src);
    while(n>0 && ((uintptr_t)dp & 15u)){ *dp++=src; n--; }
    while(n>=16){
      _mm_stream_si128((__m128i*)dp,v);
      _mm_stream_si128((__m128i*)(dp+4),v);
      _mm_stream_si128((__m128i*)(dp+8),v);
      _mm_stream_si128((__m128i*)(dp+12),v);
      dp+=16;
      n-=16;
    }
    _mm_sfence();
  }
  if(n>=4){
    __m128i v=_mm_set1_epi32((int)src);
    while(n>=4){
      _mm_storeu_si128((__m128i*)dp,v);
      dp+=4;
      n-=4;
    }
  }
#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
  if(n>=4){
    uint32x4_t v=vdupq_n_u32(src);
    while(n>=4){
      vst1q_u32(dp,v);
      dp+=4;
      n-=4;
    }
  }
#endif
#if defined(__GNUC__) || defined(__clang__)
  if(n>=4){
    if(((uintptr_t)dp & 7u) != 0){ *dp++=src; n--; }
    GmlRenderU64Alias *p=(GmlRenderU64Alias*)dp;
    int pairs=n/2;
    uint64_t v=(uint64_t)src | ((uint64_t)src<<32);
    for(int i=0;i<pairs;i++) p[i]=v;
    dp += pairs*2;
    n -= pairs*2;
  }
#endif
  for(int i=0;i<n;i++) dp[i]=src;
}
void gml_render_backend_flat_blend_init(GmlRenderBackendFlatBlend *blend,uint32_t src,double alpha){
  double ia=1.0-alpha;
  int sr=(src>>16)&0xff,sg=(src>>8)&0xff,sb=src&0xff;
  for(int d=0;d<256;d++){
    blend->red[d]=(uint8_t)(sr*alpha+d*ia+0.5);
    blend->green[d]=(uint8_t)(sg*alpha+d*ia+0.5);
    blend->blue[d]=(uint8_t)(sb*alpha+d*ia+0.5);
  }
  blend->surface_alpha=(uint32_t)lround(alpha*255.0);
}
void gml_render_backend_flat_blend_run(GmlRender *R,uint32_t *dp,int n,const GmlRenderBackendFlatBlend *blend){
  for(int i=0;i<n;i++){
    uint32_t dv=dp[i],oa=0xFF;
    if(R->target_sp>0){
      uint32_t sa=blend->surface_alpha,da=dv>>24;
      oa=(sa*sa+da*(255u-sa)+127u)/255u;
    }
    dp[i]=(oa<<24)|((uint32_t)blend->red[(dv>>16)&0xff]<<16)|
          ((uint32_t)blend->green[(dv>>8)&0xff]<<8)|blend->blue[dv&0xff];
  }
}
int gml_render_backend_flat_blend_uses_float_alpha(double alpha){
  /* Exact N/256 alpha values use the classic tie behavior and retain the truncating
   * 8-bit fast path. Other values (notably 0.4 and 0.8) retain their requested
   * fractional weight and round the final channels. */
  double scaled=alpha*256.0;
  return fabs(scaled-floor(scaled+0.5))>1e-12;
}
void gml_render_backend_draw_xrgb_alpha(GmlRender *R, uint32_t *dp, int n, uint32_t src, double alpha){
  if(n<=0) return;
  if(!R->alphablend || alpha>=1.0){
    uint32_t out=src;
    if(R->target_sp>0 && !R->alphablend){
      uint32_t sa=(uint32_t)lround(alpha*255.0);
      out=(src&0x00FFFFFFu)|(sa<<24);
    }
    gml_render_backend_fill_xrgb(dp,n,out);
    return;
  }
  uint32_t af=(uint32_t)(alpha*256.0);
  if(af>=256u){ gml_render_backend_fill_xrgb(dp,n,src); return; }
  if(!af) return;
  if(R->classic && gml_render_backend_flat_blend_uses_float_alpha(alpha)){
    /* Classic compatibility keeps the requested draw alpha as a float and rounds the
     * resulting colour channels to the nearest 8-bit value. Quantizing alpha to 1/256 first
     * produces different channel values for translucent primitives such as alpha 0.4. The
     * source colour is constant across this run, so small channel lookup tables preserve that
     * arithmetic without putting floating-point work in the per-pixel loop. */
    GmlRenderBackendFlatBlend blend;
    gml_render_backend_flat_blend_init(&blend,src,alpha);
    gml_render_backend_flat_blend_run(R,dp,n,&blend);
    return;
  }
  uint32_t ia=256u-af;
  uint32_t srb=(src&0x00FF00FFu)*af;
  uint32_t sg=(src&0x0000FF00u)*af;
  uint32_t sa=(uint32_t)lround(alpha*255.0);
  for(int i=0;i<n;i++){
    uint32_t dv=dp[i];
    uint32_t rb=((srb+(dv&0x00FF00FFu)*ia)>>8)&0x00FF00FFu;
    uint32_t g=((sg+(dv&0x0000FF00u)*ia)>>8)&0x0000FF00u;
    uint32_t oa=0xFF;
    if(R->target_sp>0){
      uint32_t da=dv>>24;
      oa=(sa*sa+da*(255u-sa)+127u)/255u;
    }
    dp[i]=(oa<<24)|rb|g;
  }
}
void gml_render_backend_draw_pixel_alpha(GmlRender *R, int x, int y, uint32_t gmcol, double alpha){
  if(!R||x<0||y<0||x>=R->fbw||y>=R->fbh) return;
  if(alpha>1) alpha=1; else if(alpha<0) alpha=0;
  if(alpha<=0) return;
  /* Pixel-at-a-time primitives (outlines, lines, circles and primitive batches) can be the
   * first draw after the frame's deferred clear.  They must materialize that underlay before
   * touching a pixel; otherwise the end-of-frame flush paints over the completed primitive.
   * Keep the check here so every such path has the same ordering semantics. */
  if(R->pending_underlay || R->pending_fill) gml_render_prepare_draw(R);
  R->fb_all_transparent=0;
  /* The subtract preset also applies inverse source alpha to offscreen coverage. A surface
   * previously filled edge to edge therefore stops being provably opaque as soon as one of
   * these fragments lands on it. Keep the coverage certificate conservative so a later
   * surface composite does not take the opaque-copy fast path through punched-out pixels. */
  if(R->target_sp>0 && R->alphablend && R->blendmode==2){
    R->fb_opaque_known=0;
    R->fb_all_opaque=0;
  }
  uint32_t src=gml_render_backend_color_to_xrgb(gmcol), *dp=&R->fb[(size_t)y*R->fbw+x];
  if(R->alphablend && (R->blend_equation!=1 || R->blend_equation_alpha!=1)){
    int sc[4]={(src>>16)&255,(src>>8)&255,src&255,(int)lround(alpha*255.0)};
    int dc[4]={(*dp>>16)&255,(*dp>>8)&255,*dp&255,R->target_sp>0?(int)(*dp>>24):255};
    double sf=alpha, df=1.0-alpha;
    if(R->blendmode==1 || R->blendmode==2 || R->blendmode==5) df=1.0;
    int oc[4];
    for(int k=0;k<4;k++){
      int eq=k==3?R->blend_equation_alpha:R->blend_equation;
      if(R->blendmode==4) df=1.0-sc[k]/255.0;
      if(R->blendmode==5) sf=sc[k]/255.0;   /* bm_src_colour scales the source by itself */
      double v;
      if(eq==2) v=sc[k]>dc[k]?sc[k]:dc[k];
      else if(eq==5) v=sc[k]<dc[k]?sc[k]:dc[k];
      else if(eq==3) v=dc[k]*df-sc[k]*sf;
      else if(eq==4) v=sc[k]*sf-dc[k]*df;
      else v=sc[k]*sf+dc[k]*df;
      if(v<0) v=0; else if(v>255) v=255;
      oc[k]=(int)lround(v);
    }
    uint32_t out=((uint32_t)oc[3]<<24)|((uint32_t)oc[0]<<16)|((uint32_t)oc[1]<<8)|(uint32_t)oc[2];
    unsigned m=R->color_write_mask;
    uint32_t bits=(m&1?0x00FF0000u:0)|(m&2?0x0000FF00u:0)|(m&4?0x000000FFu:0)|(m&8?0xFF000000u:0);
    *dp=(*dp&~bits)|(out&bits);
    return;
  }
  if(R->alphablend && R->blendmode!=0){
    int sr=(src>>16)&0xff, sg=(src>>8)&0xff, sb=src&0xff;
    int dr=(*dp>>16)&0xff, dg=(*dp>>8)&0xff, db=*dp&0xff;
    int or_,og,ob;
    uint32_t oc=R->target_sp>0 ? *dp>>24 : 0xff;
    if(R->blendmode==4){
      unsigned source_alpha=(unsigned)lround(alpha*255.0);
      *dp=color_write_merge(R,*dp,
        blend_max_preset_pixel(R,*dp,sr,sg,sb,source_alpha));
      return;
    } else if(R->blendmode==1){
      or_=dr+(int)(sr*alpha); og=dg+(int)(sg*alpha); ob=db+(int)(sb*alpha);
      if(or_>255) or_=255;
      if(og>255) og=255;
      if(ob>255) ob=255;
      if(R->target_sp>0){ uint32_t a=oc+(uint32_t)(255*alpha); oc=a>255?255:a; }
    } else if(R->blendmode==5){
      /* Add source channels scaled by their own values. A black source
       * leaves destination colour unchanged while increasing coverage. */
      unsigned source_alpha=(unsigned)lround(alpha*255.0);
      or_=dr+(int)(((unsigned)sr*(unsigned)sr+127u)/255u);
      og=dg+(int)(((unsigned)sg*(unsigned)sg+127u)/255u);
      ob=db+(int)(((unsigned)sb*(unsigned)sb+127u)/255u);
      if(or_>255) or_=255;
      if(og>255) og=255;
      if(ob>255) ob=255;
      if(R->target_sp>0){
        uint32_t a=oc+(uint32_t)((source_alpha*source_alpha+127u)/255u);
        oc=a>255?255:a;
      }
    } else {
      /* bm_subtract is the preset (bm_zero, bm_inv_src_colour): RGB is
       * destination*(1-source RGB), while coverage uses inverse source alpha. */
      or_=(int)gml_blend_inv_source_u8((unsigned)dr,(unsigned)sr);
      og=(int)gml_blend_inv_source_u8((unsigned)dg,(unsigned)sg);
      ob=(int)gml_blend_inv_source_u8((unsigned)db,(unsigned)sb);
      if(R->target_sp>0){
        unsigned sa=(unsigned)lround(alpha*255.0);
        oc=gml_blend_inv_source_u8(oc,sa);
      }
    }
    *dp=(oc<<24)|((uint32_t)or_<<16)|((uint32_t)og<<8)|(uint32_t)ob;
    return;
  }
  if(!R->alphablend || alpha>=1){
    if(R->target_sp>0 && !R->alphablend){
      uint32_t sa=(uint32_t)lround(alpha*255.0);
      src=(src&0x00FFFFFFu)|(sa<<24);
    }
    *dp=src; return;
  }
  int sr=(src>>16)&0xff, sg=(src>>8)&0xff, sb=src&0xff;
  int dr=(*dp>>16)&0xff, dg=(*dp>>8)&0xff, db=*dp&0xff;
  int or_=(int)(sr*alpha+dr*(1-alpha)); if(or_>255) or_=255; else if(or_<0) or_=0;
  int og=(int)(sg*alpha+dg*(1-alpha)); if(og>255) og=255; else if(og<0) og=0;
  int ob=(int)(sb*alpha+db*(1-alpha)); if(ob>255) ob=255; else if(ob<0) ob=0;
  uint32_t oa=0xFF;
  if(R->target_sp>0){
    uint32_t sa=(uint32_t)lround(alpha*255.0), da=*dp>>24;
    oa=(sa*sa+da*(255u-sa)+127u)/255u;
  }
  *dp=(oa<<24)|(or_<<16)|(og<<8)|ob;
}
void gml_render_backend_draw_pixel(GmlRender *R, int x, int y, uint32_t gmcol){
  gml_render_backend_draw_pixel_alpha(R,x,y,gmcol,R?R->alpha:1);
}

static inline int floor_fixed20(int64_t v){
#if defined(__GNUC__) || defined(__clang__)
  return (int)(v>>RFP_SHIFT);
#else
  return v>=0 ? (int)(v>>RFP_SHIFT) : -(int)((-v + RFP_ONE - 1) >> RFP_SHIFT);
#endif
}
static inline int ceil_div_pos_i64(int64_t num, int64_t den){
  if(num<=0) return 0;
  if(den<=0) return 1;
  if(num<=0x7fffffffll && den<=0xffffffffll && num<=0x100000000ll-den)
    return (int)(((uint32_t)num + (uint32_t)den - 1u) / (uint32_t)den);
  return (int)(((num - 1) / den) + 1);
}
static inline int fixed20_run_to_change(int64_t fp, int64_t step, int cell, int maxrun){
  if(maxrun<=1 || step==0) return maxrun;
  int64_t n;
  if(step>0){
    int64_t edge=((int64_t)cell+1) * RFP_ONE;
    if(fp>=edge) return 1;
    n=ceil_div_pos_i64(edge - fp, step);
  } else {
    int64_t neg=-step;
    int64_t edge=(int64_t)cell * RFP_ONE;
    if(fp<edge) return 1;
    n=ceil_div_pos_i64(fp - edge + 1, neg);
  }
  if(n<1) return 1;
  if(n>maxrun) return maxrun;
  return (int)n;
}
static inline int fixed20_run_until_at_least(int64_t fp, int64_t step, int target, int maxrun){
  if(maxrun<=1 || step<=0) return maxrun;
  int64_t edge=(int64_t)target * RFP_ONE;
  if(fp>=edge) return 1;
  int64_t n=ceil_div_pos_i64(edge - fp, step);
  if(n<1) return 1;
  if(n>maxrun) return maxrun;
  return (int)n;
}
static inline int fixed20_run_until_at_most(int64_t fp, int64_t step, int target, int maxrun){
  if(maxrun<=1 || step>=0) return maxrun;
  int64_t neg=-step;
  int64_t edge=(int64_t)(target + 1) * RFP_ONE;
  if(fp<edge) return 1;
  int64_t num=fp - edge;
  int64_t n=(num<=0x7ffffffell && neg<=0x7fffffffll)
    ? (int64_t)((uint32_t)num / (uint32_t)neg + 1u)
    : (num / neg + 1);
  if(n<1) return 1;
  if(n>maxrun) return maxrun;
  return (int)n;
}
static inline int rotated_quad_row_span(const double qx[4], const double qy[4], double y,
                                        int clip0, int clip1, int *out0, int *out1){
  double xs[4];
  int n=0;
  for(int i=0;i<4;i++){
    int j=(i+1)&3;
    double y0=qy[i], y1=qy[j];
    if((y0<=y && y1>y) || (y1<=y && y0>y)){
      double den=y1-y0;
      if(den!=0.0 && n<4){
        double t=(y-y0)/den;
        xs[n++]=qx[i] + t*(qx[j]-qx[i]);
      }
    }
  }
  if(n<2) return 0;
  double mn=xs[0], mx=xs[0];
  for(int i=1;i<n;i++){
    if(xs[i]<mn) mn=xs[i];
    if(xs[i]>mx) mx=xs[i];
  }
  int x0=(int)floor(mn)-1;
  int x1=(int)ceil(mx)+1;
  if(x0<clip0) x0=clip0;
  if(x1>clip1) x1=clip1;
  if(x1<=x0) return 0;
  *out0=x0;
  *out1=x1;
  return 1;
}
static inline int alpha_span_skip_run(const GmlTpag *t, int ix, int iy, int64_t lx_fp,
                                      int64_t ly_fp, int64_t dlx_fp, int64_t dly_fp,
                                      int maxrun){
  if(!t || !t->alpha_row_min || iy<0 || iy>=t->sh || maxrun<=0) return 0;
  int mn=t->alpha_row_min[iy], mx=t->alpha_row_max[iy];
  if(mx<mn){
    int run=fixed20_run_to_change(ly_fp,dly_fp,iy,maxrun);
    return run<1 ? 1 : run;
  }
  if(ix<mn){
    int run=fixed20_run_to_change(ly_fp,dly_fp,iy,maxrun);
    if(dlx_fp>0){
      int xr=fixed20_run_until_at_least(lx_fp,dlx_fp,mn,maxrun);
      if(xr<run) run=xr;
    }
    return run<1 ? 1 : run;
  }
  if(ix>mx){
    int run=fixed20_run_to_change(ly_fp,dly_fp,iy,maxrun);
    if(dlx_fp<0){
      int xr=fixed20_run_until_at_most(lx_fp,dlx_fp,mx,maxrun);
      if(xr<run) run=xr;
    }
    return run<1 ? 1 : run;
  }
  return 0;
}
static inline int alpha_qspan_skip_run(const GmlTpag *t, const uint16_t *row_min, const uint16_t *row_max,
                                       int ix, int iy, int64_t lx_fp, int64_t ly_fp,
                                       int64_t dlx_fp, int64_t dly_fp, int maxrun){
  if(!t || !row_min || !row_max || iy<0 || iy>=t->sh || maxrun<=0) return 0;
  uint16_t qmn=row_min[iy];
  if(qmn==UINT16_MAX){
    int run=fixed20_run_to_change(ly_fp,dly_fp,iy,maxrun);
    return run<1 ? 1 : run;
  }
  int mn=(int)qmn, mx=(int)row_max[iy];
  if(ix<mn){
    int run=fixed20_run_to_change(ly_fp,dly_fp,iy,maxrun);
    if(dlx_fp>0){
      int xr=fixed20_run_until_at_least(lx_fp,dlx_fp,mn,maxrun);
      if(xr<run) run=xr;
    }
    return run<1 ? 1 : run;
  }
  if(ix>mx){
    int run=fixed20_run_to_change(ly_fp,dly_fp,iy,maxrun);
    if(dlx_fp<0){
      int xr=fixed20_run_until_at_most(lx_fp,dlx_fp,mx,maxrun);
      if(xr<run) run=xr;
    }
    return run<1 ? 1 : run;
  }
  return 0;
}
static inline void fill_u32_run(uint32_t *dp, int run, uint32_t src){
  if(run<=0) return;
#if defined(__SSE2__)
  if(run>=4){
    __m128i v=_mm_set1_epi32((int)src);
    while(run>=4){
      _mm_storeu_si128((__m128i*)dp,v);
      dp+=4;
      run-=4;
    }
  }
#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
  if(run>=4){
    uint32x4_t v=vdupq_n_u32(src);
    while(run>=4){
      vst1q_u32(dp,v);
      dp+=4;
      run-=4;
    }
  }
#endif
  for(int k=0;k<run;k++) dp[k]=src;
}
#if defined(__GNUC__) || defined(__clang__)
typedef uint64_t GmlU64Alias __attribute__((__may_alias__));
#endif
static inline void blend_fast8_run(uint32_t *dp, int run, uint32_t src, uint32_t af){
  if(run<=0) return;
  if(af>=256u){ fill_u32_run(dp,run,src); return; }
  if(!af) return;
  uint32_t ia=256u-af;
#if defined(__SSE2__)
  if(run>=4){
    __m128i zero=_mm_setzero_si128();
    __m128i vsrc=_mm_set1_epi32((int)src);
    __m128i valpha=_mm_set1_epi32((int)0xFF000000u);
    __m128i vaf=_mm_set1_epi16((short)af);
    __m128i via=_mm_set1_epi16((short)ia);
    __m128i src_lo=_mm_unpacklo_epi8(vsrc,zero);
    __m128i src_hi=_mm_unpackhi_epi8(vsrc,zero);
    src_lo=_mm_mullo_epi16(src_lo,vaf);
    src_hi=_mm_mullo_epi16(src_hi,vaf);
    while(run>=4){
      __m128i dst=_mm_loadu_si128((const __m128i*)dp);
      __m128i lo=_mm_unpacklo_epi8(dst,zero);
      __m128i hi=_mm_unpackhi_epi8(dst,zero);
      lo=_mm_add_epi16(src_lo,_mm_mullo_epi16(lo,via));
      hi=_mm_add_epi16(src_hi,_mm_mullo_epi16(hi,via));
      lo=_mm_srli_epi16(lo,8);
      hi=_mm_srli_epi16(hi,8);
      _mm_storeu_si128((__m128i*)dp,_mm_or_si128(_mm_packus_epi16(lo,hi),valpha));
      dp+=4;
      run-=4;
    }
  }
#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
  if(run>=4){
    uint8x16_t vsrc8=vreinterpretq_u8_u32(vdupq_n_u32(src));
    uint8x16_t valpha8=vreinterpretq_u8_u32(vdupq_n_u32(0xFF000000u));
    uint16x8_t vaf=vdupq_n_u16((uint16_t)af);
    uint16x8_t via=vdupq_n_u16((uint16_t)ia);
    uint16x8_t src_lo=vmulq_u16(vmovl_u8(vget_low_u8(vsrc8)),vaf);
    uint16x8_t src_hi=vmulq_u16(vmovl_u8(vget_high_u8(vsrc8)),vaf);
    while(run>=4){
      uint8x16_t dst=vld1q_u8((const uint8_t*)dp);
      uint16x8_t lo=vaddq_u16(src_lo,vmulq_u16(vmovl_u8(vget_low_u8(dst)),via));
      uint16x8_t hi=vaddq_u16(src_hi,vmulq_u16(vmovl_u8(vget_high_u8(dst)),via));
      lo=vshrq_n_u16(lo,8);
      hi=vshrq_n_u16(hi,8);
      vst1q_u8((uint8_t*)dp,vorrq_u8(vcombine_u8(vmovn_u16(lo),vmovn_u16(hi)),valpha8));
      dp+=4;
      run-=4;
    }
  }
#endif
  uint32_t srb=(src & 0x00FF00FFu)*af;
  uint32_t sg=(src & 0x0000FF00u)*af;
#if defined(__GNUC__) || defined(__clang__)
  if(run>=4){
    if(((uintptr_t)dp & 7u) != 0){
      *dp=blend_fast8_cached(*dp,srb,sg,ia);
      dp++;
      run--;
    }
    GmlU64Alias *p=(GmlU64Alias*)dp;
    int pairs=run/2;
    uint64_t srb64=(uint64_t)srb | ((uint64_t)srb<<32);
    uint64_t sg64=(uint64_t)sg | ((uint64_t)sg<<32);
    for(int i=0;i<pairs;i++){
      uint64_t dv=p[i];
      uint64_t rb=((srb64+(dv&0x00FF00FF00FF00FFull)*ia)>>8)&0x00FF00FF00FF00FFull;
      uint64_t g=((sg64+(dv&0x0000FF000000FF00ull)*ia)>>8)&0x0000FF000000FF00ull;
      p[i]=0xFF000000FF000000ull|rb|g;
    }
    dp += pairs*2;
    run -= pairs*2;
  }
#endif
  for(int k=0;k<run;k++) dp[k]=blend_fast8_cached(dp[k],srb,sg,ia);
}
static int tpag_alpha_bounds(GmlRender *r, GmlTpag *t, GmlAtlas *a,
                             int *x0, int *y0, int *x1, int *y1){
  (void)r;
  if(!t || !a || !a->px || t->sw<=0 || t->sh<=0) return 0;
  if(!t->alpha_scanned){
    int minx=t->sw, miny=t->sh, maxx=-1, maxy=-1;
    int maxa=0,partial=0;
    int log_alpha=render_setting(r,"GML_LOG_TPAG_ALPHA")!=NULL;
    int real_tpag = rprof_tpag_id(r,t)>=0;
    if(real_tpag && !t->alpha_row_min && !t->alpha_row_max){
      t->alpha_row_min=malloc((size_t)t->sh*sizeof(int));
      t->alpha_row_max=malloc((size_t)t->sh*sizeof(int));
      if(t->alpha_row_min && t->alpha_row_max){
        for(int yy=0; yy<t->sh; yy++){ t->alpha_row_min[yy]=t->sw; t->alpha_row_max[yy]=-1; }
      } else {
        free(t->alpha_row_min); free(t->alpha_row_max);
        t->alpha_row_min=NULL; t->alpha_row_max=NULL;
      }
    }
    unsigned long nz=0;
    for(int yy=0; yy<t->sh; yy++){
      int sy=t->sy+yy;
      if(sy<0 || sy>=a->h) continue;
      const uint8_t *row=a->px+(size_t)sy*a->w*4;
      for(int xx=0; xx<t->sw; xx++){
        int sx=t->sx+xx;
        if(sx<0 || sx>=a->w) continue;
        const uint8_t *sp=row+(size_t)sx*4;
        if(!sp[3]) continue;
        if(sp[3]>maxa) maxa=sp[3];
        if(sp[3]<255) partial=1;
        if(log_alpha) nz++;
        if(xx<minx) minx=xx;
        if(xx>maxx) maxx=xx;
        if(yy<miny) miny=yy;
        if(yy>maxy) maxy=yy;
        if(t->alpha_row_min){
          if(xx<t->alpha_row_min[yy]) t->alpha_row_min[yy]=xx;
          if(xx>t->alpha_row_max[yy]) t->alpha_row_max[yy]=xx;
        }
      }
    }
    t->ax0=minx; t->ay0=miny; t->ax1=maxx; t->ay1=maxy; t->alpha_max=maxa;
    t->alpha_partial=partial;
    t->alpha_scanned=1;
    if(log_alpha){
      int id=rprof_tpag_id(r,t);
      unsigned long area=(unsigned long)(t->sw>0?t->sw:0)*(unsigned long)(t->sh>0?t->sh:0);
      anygm_host_logf(r && r->win ? r->win->host : NULL,ANYGM_LOG_DEBUG,"[tpag-alpha] id=%d atlas=%d src=%d,%d %dx%d nz=%lu/%lu amax=%d bbox=%d,%d-%d,%d\n",
              id,t->atlas,t->sx,t->sy,t->sw,t->sh,nz,area,t->alpha_max,t->ax0,t->ay0,t->ax1,t->ay1);
    }
  }
  if(t->ax1<t->ax0 || t->ay1<t->ay0) return 0;
  if(x0) *x0=t->ax0;
  if(y0) *y0=t->ay0;
  if(x1) *x1=t->ax1;
  if(y1) *y1=t->ay1;
  return 1;
}
static uint32_t *tpag_argb_cache(GmlRender *r, GmlTpag *t, GmlAtlas *a){
  if(!t || !a || !a->px || t->sw<=0 || t->sh<=0) return NULL;
  if(t->argb_cache) return t->argb_cache;
  if(!r || !r->tpag || r->n_tpag<=0) return NULL;
  uintptr_t p=(uintptr_t)t, b=(uintptr_t)r->tpag, e=b+(uintptr_t)r->n_tpag*sizeof(GmlTpag);
  if(p<b || p>=e) return NULL;
  size_t n=(size_t)t->sw*(size_t)t->sh;
  if(n==0 || n>16777216u) return NULL;
  uint32_t *cache=malloc(n*sizeof(uint32_t));
  if(!cache) return NULL;
  for(int yy=0; yy<t->sh; yy++){
    int sy=t->sy+yy;
    uint32_t *cp=cache+(size_t)yy*t->sw;
    if(sy<0 || sy>=a->h){ memset(cp,0,(size_t)t->sw*sizeof(uint32_t)); continue; }
    for(int xx=0; xx<t->sw; xx++){
      int sx=t->sx+xx;
      if(sx<0 || sx>=a->w){ cp[xx]=0; continue; }
      const uint8_t *sp=a->px+((size_t)sy*a->w+sx)*4;
      cp[xx]=((uint32_t)sp[3]<<24)|((uint32_t)sp[0]<<16)|((uint32_t)sp[1]<<8)|(uint32_t)sp[2];
    }
  }
  t->argb_cache=cache;
  return cache;
}
static uint8_t *tpag_alpha8_cache(GmlRender *r, GmlTpag *t, GmlAtlas *a){
  if(!t || !a || !a->px || t->sw<=0 || t->sh<=0) return NULL;
  if(t->alpha8_cache) return t->alpha8_cache;
  if(!r || !r->tpag || r->n_tpag<=0) return NULL;
  uintptr_t pointer=(uintptr_t)t,begin=(uintptr_t)r->tpag;
  uintptr_t end=begin+(uintptr_t)r->n_tpag*sizeof(GmlTpag);
  if(pointer<begin || pointer>=end) return NULL;
  size_t count=(size_t)t->sw*(size_t)t->sh;
  if(!count || count>16777216u) return NULL;
  uint8_t *cache=malloc(count);
  if(!cache) return NULL;
  for(int y=0;y<t->sh;y++){
    int source_y=t->sy+y;
    uint8_t *destination=cache+(size_t)y*t->sw;
    if(source_y<0 || source_y>=a->h){
      memset(destination,0,(size_t)t->sw);
      continue;
    }
    for(int x=0;x<t->sw;x++){
      int source_x=t->sx+x;
      destination[x]=source_x>=0 && source_x<a->w
        ? a->px[((size_t)source_y*a->w+source_x)*4u+3u] : 0;
    }
  }
  t->alpha8_cache=cache;
  return cache;
}
static float atlas_alpha_sample(const GmlAtlas *atlas,float x,float y,int linear){
  if(!atlas || !atlas->px || atlas->w<=0 || atlas->h<=0) return 0.0f;
  if(!linear){
    int ix=(int)floorf(x+0.5f),iy=(int)floorf(y+0.5f);
    if(ix<0)ix=0;else if(ix>=atlas->w)ix=atlas->w-1;
    if(iy<0)iy=0;else if(iy>=atlas->h)iy=atlas->h-1;
    return atlas->px[((size_t)iy*atlas->w+ix)*4u+3u]*(1.0f/255.0f);
  }
  int x0=(int)floorf(x),y0=(int)floorf(y);
  float fx=x-x0,fy=y-y0;
  int x1=x0+1,y1=y0+1;
  if(x0<0)x0=0;else if(x0>=atlas->w)x0=atlas->w-1;
  if(x1<0)x1=0;else if(x1>=atlas->w)x1=atlas->w-1;
  if(y0<0)y0=0;else if(y0>=atlas->h)y0=atlas->h-1;
  if(y1<0)y1=0;else if(y1>=atlas->h)y1=atlas->h-1;
  float a00=atlas->px[((size_t)y0*atlas->w+x0)*4u+3u];
  float a10=atlas->px[((size_t)y0*atlas->w+x1)*4u+3u];
  float a01=atlas->px[((size_t)y1*atlas->w+x0)*4u+3u];
  float a11=atlas->px[((size_t)y1*atlas->w+x1)*4u+3u];
  return ((a00+(a10-a00)*fx)*(1.0f-fy)+(a01+(a11-a01)*fx)*fy)*
         (1.0f/255.0f);
}
static uint32_t *tpag_solid_blur_alpha_cache(
    GmlRender *r,GmlTpag *t,GmlAtlas *atlas,const struct GmlShaderPal *shader){
  if(!r || !t || !atlas || !shader || !shader->solid_blur_alpha ||
     t->sw<=0 || t->sh<=0 || rprof_tpag_id(r,t)<0) return NULL;
  if(t->solid_blur_alpha_cache &&
     t->solid_blur_alpha_shader==r->active_shader &&
     t->solid_blur_alpha_interp==r->interp) return t->solid_blur_alpha_cache;
  size_t count=(size_t)t->sw*(size_t)t->sh;
  if(!count || count>16777216u) return NULL;
  uint32_t *cache=t->solid_blur_alpha_cache;
  if(!cache){
    cache=malloc(count*sizeof(*cache));
    if(!cache) return NULL;
    t->solid_blur_alpha_cache=cache;
  }
  for(int y=0;y<t->sh;y++) for(int x=0;x<t->sw;x++){
    float atlas_x=(float)(t->sx+x),atlas_y=(float)(t->sy+y);
    float alpha=0.0f;
    for(int tap=0;tap<shader->solid_blur_alpha_x_count;tap++)
      alpha+=atlas_alpha_sample(
        atlas,atlas_x+shader->solid_blur_alpha_x_offset[tap]*
                        shader->solid_blur_alpha_step_x*(float)atlas->w,
        atlas_y,r->interp)*shader->solid_blur_alpha_x_weight[tap];
    for(int tap=0;tap<shader->solid_blur_alpha_y_count;tap++){
      float sample=atlas_alpha_sample(
        atlas,atlas_x,
        atlas_y+shader->solid_blur_alpha_y_offset[tap]*
                shader->solid_blur_alpha_step_y*(float)atlas->h,r->interp);
      alpha+=sample*shader->solid_blur_alpha_y_weight[tap]*alpha;
    }
    if(alpha<0.0f)alpha=0.0f;else if(alpha>1.0f)alpha=1.0f;
    cache[(size_t)y*t->sw+x]=(uint32_t)floorf(alpha*255.0f+0.5f)<<24;
  }
  t->solid_blur_alpha_shader=r->active_shader;
  t->solid_blur_alpha_interp=r->interp;
  return cache;
}
static int tpag_alpha_runs(GmlRender *r, GmlTpag *t, GmlAtlas *a,
                           const GmlTpagAlphaRun **runs, int *count){
  if(runs) *runs=NULL;
  if(count) *count=0;
  if(!r || !t || !a || !a->px || t->sw<=0 || t->sh<=0 || t->sw>UINT16_MAX || t->sh>UINT16_MAX) return 0;
  if(t->alpha_runs_built){
    if(!t->alpha_runs || t->alpha_run_count<=0) return 0;
    if(runs) *runs=t->alpha_runs;
    if(count) *count=t->alpha_run_count;
    return 1;
  }
  t->alpha_runs_built=1;
  int bx0=0, by0=0, bx1=t->sw-1, by1=t->sh-1;
  if(!tpag_alpha_bounds(r,t,a,&bx0,&by0,&bx1,&by1) || !t->alpha_row_min || !t->alpha_row_max) return 0;
  uint32_t *cache=tpag_argb_cache(r,t,a);
  if(!cache) return 0;
  int cap=0, n=0;
  GmlTpagAlphaRun *out=NULL;
  for(int yy=0; yy<t->sh; yy++){
    int mn=t->alpha_row_min[yy], mx=t->alpha_row_max[yy];
    if(mx<mn) continue;
    const uint32_t *row=cache+(size_t)yy*t->sw;
    for(int xx=mn; xx<=mx; ){
      uint32_t aa=row[xx]>>24;
      int run=1;
      while(xx+run<=mx && (row[xx+run]>>24)==aa && run<UINT16_MAX) run++;
      if(aa){
        if(n>=cap){
          if(cap>=262144){ free(out); return 0; }
          int nc=cap?cap*2:1024;
          if(nc>262144) nc=262144;
          GmlTpagAlphaRun *nr=realloc(out,(size_t)nc*sizeof(*out));
          if(!nr){ free(out); return 0; }
          out=nr; cap=nc;
        }
        out[n++]=(GmlTpagAlphaRun){(uint16_t)yy,(uint16_t)xx,(uint16_t)run,(uint8_t)aa};
      }
      xx+=run;
    }
  }
  if(!n){ free(out); return 0; }
  t->alpha_runs=out;
  t->alpha_run_count=n;
  if(runs) *runs=t->alpha_runs;
  if(count) *count=t->alpha_run_count;
  return 1;
}
static const uint32_t *tpag_fully_opaque_argb(
    GmlRender *r,GmlTpag *t,GmlAtlas *atlas){
  const GmlTpagAlphaRun *runs=NULL;
  int count=0;
  if(!tpag_alpha_runs(r,t,atlas,&runs,&count) || count!=t->sh) return NULL;
  for(int row=0;row<t->sh;row++){
    const GmlTpagAlphaRun *run=&runs[row];
    if(run->y!=(uint16_t)row || run->x!=0 || run->len!=(uint16_t)t->sw ||
       run->alpha!=255) return NULL;
  }
  return t->argb_cache;
}
static uint32_t *tpag_fast8_draw_cache(GmlTpag *t, GmlAtlas *a, uint32_t blend, double alpha,
                                       int alpha_floor, int *copy_255){
  if(copy_255) *copy_255=0;
  if(!t || !a || !a->px || t->sw<=0 || t->sh<=0) return NULL;
  size_t n=(size_t)t->sw*(size_t)t->sh;
  if(n==0 || n>262144u) return NULL;
  blend &= 0xFFFFFFu;
  if(alpha_floor<0) alpha_floor=0;
  if(alpha_floor>255) alpha_floor=255;
  if(t->fast8_draw_cache_valid && t->fast8_draw_cache &&
     t->fast8_draw_blend_key==blend && t->fast8_draw_alpha_key==alpha &&
     t->fast8_draw_alpha_floor_key==alpha_floor){
    if(copy_255) *copy_255=t->fast8_draw_cache_copy_255;
    return t->fast8_draw_cache;
  }
  if(t->fast8_draw_pending_blend_key==blend && t->fast8_draw_pending_alpha_key==alpha &&
     t->fast8_draw_pending_alpha_floor_key==alpha_floor){
    t->fast8_draw_pending_count++;
  } else {
    t->fast8_draw_pending_blend_key=blend;
    t->fast8_draw_pending_alpha_key=alpha;
    t->fast8_draw_pending_alpha_floor_key=alpha_floor;
    t->fast8_draw_pending_count=1;
  }
  if(t->fast8_draw_pending_count<8) return NULL;
  uint32_t *cache=t->fast8_draw_cache;
  if(!cache){
    cache=malloc(n*sizeof(uint32_t));
    if(!cache) return NULL;
    t->fast8_draw_cache=cache;
  }
  int white=(blend==0xFFFFFFu);
  int bR=blend&0xFF, bG=(blend>>8)&0xFF, bB=(blend>>16)&0xFF;
  for(int yy=0; yy<t->sh; yy++){
    int sy=t->sy+yy;
    uint32_t *cp=cache+(size_t)yy*t->sw;
    if(sy<0 || sy>=a->h){ memset(cp,0,(size_t)t->sw*sizeof(uint32_t)); continue; }
    for(int xx=0; xx<t->sw; xx++){
      int sx=t->sx+xx;
      if(sx<0 || sx>=a->w){ cp[xx]=0; continue; }
      const uint8_t *sp=a->px+((size_t)sy*a->w+sx)*4;
      double sa=(sp[3]/255.0)*alpha;
      uint32_t af=sa>=1.0 ? 256u : (uint32_t)(sa*256.0);
      if(af<=(uint32_t)alpha_floor){ cp[xx]=0; continue; }
      int sr=white?sp[0]:sp[0]*bR/255;
      int sg=white?sp[1]:sp[1]*bG/255;
      int sb=white?sp[2]:sp[2]*bB/255;
      cp[xx]=((af>=256u?255u:af)<<24)|((uint32_t)sr<<16)|((uint32_t)sg<<8)|(uint32_t)sb;
    }
  }
  t->fast8_draw_blend_key=blend;
  t->fast8_draw_alpha_key=alpha;
  t->fast8_draw_alpha_floor_key=alpha_floor;
  t->fast8_draw_cache_valid=1;
  t->fast8_draw_cache_copy_255=alpha>=1.0;
  if(copy_255) *copy_255=t->fast8_draw_cache_copy_255;
  return cache;
}
enum { GML_BLEND_STUDIO1=0, GML_BLEND_STUDIO2=1, GML_BLEND_CLASSIC=2 };
static inline int gml_blend_family(const GmlRender *r){
  /* Loaded content owns compatibility policy. A renderer can also be exercised as an isolated
   * component before content is attached; preserve its explicit mode in that narrow case. */
  if(!r || !r->win) return r&&r->classic?GML_BLEND_CLASSIC:GML_BLEND_STUDIO1;
  AnygmBlendPolicy policy=anygm_policy_blend(r?r->win:NULL);
  if(policy==ANYGM_BLEND_CLASSIC) return GML_BLEND_CLASSIC;
  return policy==ANYGM_BLEND_STUDIO_SECOND?GML_BLEND_STUDIO2:GML_BLEND_STUDIO1;
}
static inline int gml_render_target_is_first_generation_application_surface(
    const GmlRender *render){
  return render && render->win &&
    anygm_policy_uses_first_generation_studio(render->win) &&
    render->app_surface && render->fb==render->app_surface;
}
static inline uint32_t gml_sprite_target_alpha(const GmlRender *r,uint32_t dst,
                                               unsigned source_alpha){
  if(!gml_render_target_preserves_alpha(r)) return 0xFF000000u;
  if(source_alpha>255u) source_alpha=255u;
  unsigned destination_alpha=dst>>24;
  unsigned inverse=255u-source_alpha;
  unsigned output=(source_alpha*source_alpha+destination_alpha*inverse+127u)/255u;
  if(output>255u) output=255u;
  return output<<24;
}
static inline uint32_t gml_sprite_target_inverse_alpha(const GmlRender *r,uint32_t dst,
                                                       double source_alpha){
  if(!gml_render_target_preserves_alpha(r)) return 0xFF000000u;
  if(source_alpha<0.0) source_alpha=0.0;
  else if(source_alpha>255.0) source_alpha=255.0;
  unsigned destination_alpha=dst>>24;
  unsigned output=(unsigned)lround((double)destination_alpha*(1.0-source_alpha/255.0));
  if(output>255u) output=255u;
  return output<<24;
}
static inline void gml_sprite_target_may_change_alpha(GmlRender *r){
  if(!gml_render_target_preserves_alpha(r)) return;
  r->fb_opaque_known=0;
  r->fb_all_opaque=0;
  if(r->app_surface && r->fb==r->app_surface) r->app_surface_opaque=0;
}
static inline void blend_argb_src_over_exact(GmlRender *r,uint32_t *dp,const uint32_t *sp,
                                             int run,uint32_t aa,int family){
  if(run<=0 || !aa) return;
  if(aa>=255u){ memcpy(dp,sp,(size_t)run*sizeof(uint32_t)); return; }
  uint32_t ia=255u-aa;
  for(int k=0; k<run; k++){
    uint32_t src=sp[k], dst=dp[k];
    uint32_t sr=(src>>16)&0xFFu, sg=(src>>8)&0xFFu, sb=src&0xFFu;
    uint32_t dr=(dst>>16)&0xFFu, dg=(dst>>8)&0xFFu, db=dst&0xFFu;
    if(family==GML_BLEND_CLASSIC){
      uint32_t rr=(sr*aa+127u)/255u+(dr*ia+127u)/255u;
      uint32_t rg=(sg*aa+127u)/255u+(dg*ia+127u)/255u;
      uint32_t rb=(sb*aa+127u)/255u+(db*ia+127u)/255u;
      if(rr>255u) rr=255u;
      if(rg>255u) rg=255u;
      if(rb>255u) rb=255u;
      dp[k]=gml_sprite_target_alpha(r,dst,aa)|(rr<<16)|(rg<<8)|rb;
    } else if(family==GML_BLEND_STUDIO2) {
      /* The Studio 2 format uses a UNORM target: the complete source-over sum is rounded to the
       * nearest representable channel. Classic rounds the terms independently above, while
       * Studio 1 truncates the combined result below. */
      dp[k]=gml_sprite_target_alpha(r,dst,aa)|(((sr*aa+dr*ia+127u)/255u)<<16)|
            (((sg*aa+dg*ia+127u)/255u)<<8)|((sb*aa+db*ia+127u)/255u);
    } else {
      dp[k]=gml_sprite_target_alpha(r,dst,aa)|(((sr*aa+dr*ia)/255u)<<16)|
            (((sg*aa+dg*ia)/255u)<<8)|((sb*aa+db*ia)/255u);
    }
  }
}
static inline void blend_solid_fast8_4(uint32_t *destination, uint32_t source,
                                       const uint32_t alpha[4]){
#if defined(__SSE2__)
  __m128i zero=_mm_setzero_si128();
  __m128i src=_mm_set1_epi32((int)source);
  __m128i dst=_mm_loadu_si128((const __m128i*)destination);
  __m128i src_lo=_mm_unpacklo_epi8(src,zero);
  __m128i src_hi=_mm_unpackhi_epi8(src,zero);
  __m128i dst_lo=_mm_unpacklo_epi8(dst,zero);
  __m128i dst_hi=_mm_unpackhi_epi8(dst,zero);
  __m128i alpha_lo=_mm_set_epi16(
    (short)alpha[1],(short)alpha[1],(short)alpha[1],(short)alpha[1],
    (short)alpha[0],(short)alpha[0],(short)alpha[0],(short)alpha[0]);
  __m128i alpha_hi=_mm_set_epi16(
    (short)alpha[3],(short)alpha[3],(short)alpha[3],(short)alpha[3],
    (short)alpha[2],(short)alpha[2],(short)alpha[2],(short)alpha[2]);
  __m128i inverse_lo=_mm_sub_epi16(_mm_set1_epi16(256),alpha_lo);
  __m128i inverse_hi=_mm_sub_epi16(_mm_set1_epi16(256),alpha_hi);
  __m128i out_lo=_mm_add_epi16(_mm_mullo_epi16(src_lo,alpha_lo),
                               _mm_mullo_epi16(dst_lo,inverse_lo));
  __m128i out_hi=_mm_add_epi16(_mm_mullo_epi16(src_hi,alpha_hi),
                               _mm_mullo_epi16(dst_hi,inverse_hi));
  out_lo=_mm_srli_epi16(out_lo,8);
  out_hi=_mm_srli_epi16(out_hi,8);
  __m128i packed=_mm_packus_epi16(out_lo,out_hi);
  packed=_mm_or_si128(packed,_mm_set1_epi32((int)0xFF000000u));
  __m128i active=_mm_set_epi32(
    alpha[3]?-1:0,alpha[2]?-1:0,alpha[1]?-1:0,alpha[0]?-1:0);
  _mm_storeu_si128(
    (__m128i*)destination,
    _mm_or_si128(_mm_and_si128(packed,active),_mm_andnot_si128(active,dst)));
#else
  for(int i=0;i<4;i++) blend_fast8_run(destination+i,1,source,alpha[i]);
#endif
}
static inline void blend_additive_rgb_run(uint32_t *destination,int count,uint32_t increment){
  if(!destination || count<=0) return;
#if defined(__SSE2__)
  __m128i add=_mm_set1_epi32((int)(increment&0x00FFFFFFu));
  __m128i opaque=_mm_set1_epi32((int)0xFF000000u);
  while(count>=4){
    __m128i value=_mm_loadu_si128((const __m128i*)destination);
    value=_mm_adds_epu8(value,add);
    _mm_storeu_si128((__m128i*)destination,_mm_or_si128(value,opaque));
    destination+=4;
    count-=4;
  }
#endif
  for(int i=0;i<count;i++){
    uint32_t value=destination[i];
    unsigned red=((value>>16)&255u)+((increment>>16)&255u);
    unsigned green=((value>>8)&255u)+((increment>>8)&255u);
    unsigned blue=(value&255u)+(increment&255u);
    if(red>255u) red=255u;
    if(green>255u) green=255u;
    if(blue>255u) blue=255u;
    destination[i]=0xFF000000u|(red<<16)|(green<<8)|blue;
  }
}
static inline void blend_solid_trunc255_4(
    uint32_t *destination,uint32_t source,const uint32_t alpha[4]){
#if defined(__SSE2__)
  __m128i zero=_mm_setzero_si128();
  __m128i src=_mm_set1_epi32((int)(source&0x00FFFFFFu));
  __m128i dst=_mm_loadu_si128((const __m128i*)destination);
  __m128i src_lo=_mm_unpacklo_epi8(src,zero);
  __m128i src_hi=_mm_unpackhi_epi8(src,zero);
  __m128i dst_lo=_mm_unpacklo_epi8(dst,zero);
  __m128i dst_hi=_mm_unpackhi_epi8(dst,zero);
  __m128i alpha_lo=_mm_set_epi16(
    (short)alpha[1],(short)alpha[1],(short)alpha[1],(short)alpha[1],
    (short)alpha[0],(short)alpha[0],(short)alpha[0],(short)alpha[0]);
  __m128i alpha_hi=_mm_set_epi16(
    (short)alpha[3],(short)alpha[3],(short)alpha[3],(short)alpha[3],
    (short)alpha[2],(short)alpha[2],(short)alpha[2],(short)alpha[2]);
  __m128i inverse_lo=_mm_sub_epi16(_mm_set1_epi16(255),alpha_lo);
  __m128i inverse_hi=_mm_sub_epi16(_mm_set1_epi16(255),alpha_hi);
  __m128i sum_lo=_mm_add_epi16(_mm_mullo_epi16(src_lo,alpha_lo),
                               _mm_mullo_epi16(dst_lo,inverse_lo));
  __m128i sum_hi=_mm_add_epi16(_mm_mullo_epi16(src_hi,alpha_hi),
                               _mm_mullo_epi16(dst_hi,inverse_hi));
  __m128i one=_mm_set1_epi16(1);
  sum_lo=_mm_add_epi16(sum_lo,one);
  sum_hi=_mm_add_epi16(sum_hi,one);
  sum_lo=_mm_srli_epi16(_mm_add_epi16(sum_lo,_mm_srli_epi16(sum_lo,8)),8);
  sum_hi=_mm_srli_epi16(_mm_add_epi16(sum_hi,_mm_srli_epi16(sum_hi,8)),8);
  __m128i packed=_mm_packus_epi16(sum_lo,sum_hi);
  __m128i active=_mm_set_epi32(
    alpha[3]?-1:0,alpha[2]?-1:0,alpha[1]?-1:0,alpha[0]?-1:0);
  packed=_mm_or_si128(
    _mm_or_si128(_mm_and_si128(packed,active),
                 _mm_andnot_si128(active,dst)),
    _mm_and_si128(active,_mm_set1_epi32((int)0xFF000000u)));
  _mm_storeu_si128((__m128i*)destination,packed);
#else
  for(int i=0;i<4;i++){
    unsigned a=alpha[i];
    if(!a) continue;
    unsigned inverse=255u-a;
    uint32_t value=destination[i];
    unsigned red=(((source>>16)&255u)*a+((value>>16)&255u)*inverse)/255u;
    unsigned green=(((source>>8)&255u)*a+((value>>8)&255u)*inverse)/255u;
    unsigned blue=((source&255u)*a+(value&255u)*inverse)/255u;
    destination[i]=0xFF000000u|(red<<16)|(green<<8)|blue;
  }
#endif
}
static inline void blend_pixels_fast8_4(uint32_t *destination, const uint32_t source[4],
                                        const uint32_t alpha[4]){
#if defined(__SSE2__)
  __m128i zero=_mm_setzero_si128();
  __m128i src=_mm_loadu_si128((const __m128i*)source);
  __m128i dst=_mm_loadu_si128((const __m128i*)destination);
  __m128i src_lo=_mm_unpacklo_epi8(src,zero);
  __m128i src_hi=_mm_unpackhi_epi8(src,zero);
  __m128i dst_lo=_mm_unpacklo_epi8(dst,zero);
  __m128i dst_hi=_mm_unpackhi_epi8(dst,zero);
  __m128i alpha_lo=_mm_set_epi16(
    (short)alpha[1],(short)alpha[1],(short)alpha[1],(short)alpha[1],
    (short)alpha[0],(short)alpha[0],(short)alpha[0],(short)alpha[0]);
  __m128i alpha_hi=_mm_set_epi16(
    (short)alpha[3],(short)alpha[3],(short)alpha[3],(short)alpha[3],
    (short)alpha[2],(short)alpha[2],(short)alpha[2],(short)alpha[2]);
  __m128i inverse_lo=_mm_sub_epi16(_mm_set1_epi16(256),alpha_lo);
  __m128i inverse_hi=_mm_sub_epi16(_mm_set1_epi16(256),alpha_hi);
  __m128i out_lo=_mm_add_epi16(_mm_mullo_epi16(src_lo,alpha_lo),
                               _mm_mullo_epi16(dst_lo,inverse_lo));
  __m128i out_hi=_mm_add_epi16(_mm_mullo_epi16(src_hi,alpha_hi),
                               _mm_mullo_epi16(dst_hi,inverse_hi));
  out_lo=_mm_srli_epi16(out_lo,8);
  out_hi=_mm_srli_epi16(out_hi,8);
  __m128i packed=_mm_packus_epi16(out_lo,out_hi);
  packed=_mm_or_si128(packed,_mm_set1_epi32((int)0xFF000000u));
  __m128i active=_mm_set_epi32(
    alpha[3]?-1:0,alpha[2]?-1:0,alpha[1]?-1:0,alpha[0]?-1:0);
  _mm_storeu_si128(
    (__m128i*)destination,
    _mm_or_si128(_mm_and_si128(packed,active),_mm_andnot_si128(active,dst)));
#else
  for(int i=0;i<4;i++) blend_fast8_run(destination+i,1,source[i],alpha[i]);
#endif
}
static inline void blend_pixels_exact16_4(
    uint32_t *destination,const uint32_t source[4],const uint32_t alpha[4]){
#if defined(__SSE2__)
  __m128i zero=_mm_setzero_si128();
  __m128i sources=_mm_loadu_si128((const __m128i*)source);
  __m128i destinations=_mm_loadu_si128((const __m128i*)destination);
  __m128i source_lo=_mm_unpacklo_epi8(sources,zero);
  __m128i source_hi=_mm_unpackhi_epi8(sources,zero);
  __m128i destination_lo=_mm_unpacklo_epi8(destinations,zero);
  __m128i destination_hi=_mm_unpackhi_epi8(destinations,zero);
  __m128i alpha_lo=_mm_set_epi16(
    (short)alpha[1],(short)alpha[1],(short)alpha[1],(short)alpha[1],
    (short)alpha[0],(short)alpha[0],(short)alpha[0],(short)alpha[0]);
  __m128i alpha_hi=_mm_set_epi16(
    (short)alpha[3],(short)alpha[3],(short)alpha[3],(short)alpha[3],
    (short)alpha[2],(short)alpha[2],(short)alpha[2],(short)alpha[2]);
  __m128i delta_lo=_mm_sub_epi16(source_lo,destination_lo);
  __m128i delta_hi=_mm_sub_epi16(source_hi,destination_hi);
  /* Treat the 16-bit factors as unsigned while retaining SSE2's signed high multiply.
   * A factor with its high bit set is (signed_factor + 65536), so add one delta after
   * the high product. This is exactly floor((source-destination)*factor/65536). */
  __m128i quotient_lo=_mm_add_epi16(
    _mm_mulhi_epi16(delta_lo,alpha_lo),
    _mm_and_si128(delta_lo,_mm_srai_epi16(alpha_lo,15)));
  __m128i quotient_hi=_mm_add_epi16(
    _mm_mulhi_epi16(delta_hi,alpha_hi),
    _mm_and_si128(delta_hi,_mm_srai_epi16(alpha_hi,15)));
  __m128i blended=_mm_packus_epi16(
    _mm_add_epi16(destination_lo,quotient_lo),
    _mm_add_epi16(destination_hi,quotient_hi));
  __m128i full=_mm_set_epi32(
    alpha[3]>=65536u?-1:0,alpha[2]>=65536u?-1:0,
    alpha[1]>=65536u?-1:0,alpha[0]>=65536u?-1:0);
  blended=_mm_or_si128(_mm_and_si128(sources,full),_mm_andnot_si128(full,blended));
  _mm_storeu_si128((__m128i*)destination,blended);
#else
  for(int i=0;i<4;i++){
    uint32_t factor=alpha[i];
    if(!factor) continue;
    if(factor>=65536u){
      destination[i]=source[i];
      continue;
    }
    uint32_t inverse=65536u-factor;
    uint32_t current=destination[i];
    uint32_t red=((source[i]>>16)&255u)*factor+
                 ((current>>16)&255u)*inverse;
    uint32_t green=((source[i]>>8)&255u)*factor+
                   ((current>>8)&255u)*inverse;
    uint32_t blue=(source[i]&255u)*factor+(current&255u)*inverse;
    destination[i]=0xFF000000u|((red>>16)<<16)|((green>>16)<<8)|(blue>>16);
  }
#endif
}
static inline void blend_pixels_exact16_round_4(
    uint32_t *destination,const uint32_t source[4],const uint32_t alpha[4]){
  for(int i=0;i<4;i++){
    uint32_t factor=alpha[i];
    if(!factor) continue;
    if(factor>=65536u){
      destination[i]=source[i];
      continue;
    }
    uint32_t inverse=65536u-factor;
    uint32_t current=destination[i];
    uint32_t red=((source[i]>>16)&255u)*factor+
                 ((current>>16)&255u)*inverse+32768u;
    uint32_t green=((source[i]>>8)&255u)*factor+
                   ((current>>8)&255u)*inverse+32768u;
    uint32_t blue=(source[i]&255u)*factor+(current&255u)*inverse+32768u;
    destination[i]=0xFF000000u|((red>>16)<<16)|((green>>16)<<8)|(blue>>16);
  }
}
static inline void copy_argb_force_opaque(uint32_t *dp, const uint32_t *sp, int run){
  if(run<=0) return;
  for(int k=0; k<run; k++) dp[k]=0xFF000000u|(sp[k]&0x00FFFFFFu);
}
static inline void copy_argb_force_opaque_reverse(uint32_t *dp, const uint32_t *sp, int run){
  if(run<=0) return;
  for(int k=0; k<run; k++) dp[-k]=0xFF000000u|(sp[k]&0x00FFFFFFu);
}
static inline void blend_argb_src_over_double(GmlRender *r,uint32_t *dp,const uint32_t *sp,
                                              int run,uint32_t aa){
  if(run<=0 || !aa) return;
  if(aa>=255u){ memcpy(dp,sp,(size_t)run*sizeof(uint32_t)); return; }
  double sa=aa/255.0, ia=1.0-sa;
  for(int k=0; k<run; k++){
    uint32_t src=sp[k], dst=dp[k];
    int sr=(src>>16)&0xFF, sg=(src>>8)&0xFF, sb=src&0xFF;
    int dr=(dst>>16)&0xFF, dg=(dst>>8)&0xFF, db=dst&0xFF;
    int or_=(int)(sr*sa+dr*ia); if(or_>255) or_=255; else if(or_<0) or_=0;
    int og=(int)(sg*sa+dg*ia); if(og>255) og=255; else if(og<0) og=0;
    int ob=(int)(sb*sa+db*ia); if(ob>255) ob=255; else if(ob<0) ob=0;
    dp[k]=gml_sprite_target_alpha(r,dst,aa)|((uint32_t)or_<<16)|
          ((uint32_t)og<<8)|(uint32_t)ob;
  }
}
static inline void blend_argb_src_over_double_reverse(GmlRender *r,uint32_t *dp,
                                                      const uint32_t *sp,int run,uint32_t aa){
  if(run<=0 || !aa) return;
  if(aa>=255u){ for(int k=0; k<run; k++) dp[-k]=sp[k]; return; }
  double sa=aa/255.0, ia=1.0-sa;
  for(int k=0; k<run; k++){
    uint32_t src=sp[k], dst=dp[-k];
    int sr=(src>>16)&0xFF, sg=(src>>8)&0xFF, sb=src&0xFF;
    int dr=(dst>>16)&0xFF, dg=(dst>>8)&0xFF, db=dst&0xFF;
    int or_=(int)(sr*sa+dr*ia); if(or_>255) or_=255; else if(or_<0) or_=0;
    int og=(int)(sg*sa+dg*ia); if(og>255) og=255; else if(og<0) og=0;
    int ob=(int)(sb*sa+db*ia); if(ob>255) ob=255; else if(ob<0) ob=0;
    dp[-k]=gml_sprite_target_alpha(r,dst,aa)|((uint32_t)or_<<16)|
           ((uint32_t)og<<8)|(uint32_t)ob;
  }
}
static inline void blend_argb_src_over_draw_alpha(GmlRender *r,uint32_t *dp,const uint32_t *sp,
                                                  int run,uint32_t aa,double alpha,int family){
  if(run<=0 || !aa || alpha<=0.0) return;
  if(family==GML_BLEND_CLASSIC){
    uint32_t effective=(uint32_t)(aa*alpha+0.5);
    if(effective>255u) effective=255u;
    uint32_t inverse=255u-effective;
    for(int k=0; k<run; k++){
      uint32_t src=sp[k], dst=dp[k];
      uint32_t sr=(src>>16)&0xFFu, sg=(src>>8)&0xFFu, sb=src&0xFFu;
      uint32_t dr=(dst>>16)&0xFFu, dg=(dst>>8)&0xFFu, db=dst&0xFFu;
      uint32_t rr=(sr*effective+127u)/255u+(dr*inverse+127u)/255u;
      uint32_t rg=(sg*effective+127u)/255u+(dg*inverse+127u)/255u;
      uint32_t rb=(sb*effective+127u)/255u+(db*inverse+127u)/255u;
      if(rr>255u) rr=255u;
      if(rg>255u) rg=255u;
      if(rb>255u) rb=255u;
      dp[k]=gml_sprite_target_alpha(r,dst,effective)|(rr<<16)|(rg<<8)|rb;
    }
    return;
  }
  if(family==GML_BLEND_STUDIO1){
    double sa=(aa/255.0)*alpha, ia=1.0-sa;
    double bias=gml_render_target_is_first_generation_application_surface(r)?0.5:0.0;
    for(int k=0; k<run; k++){
      uint32_t src=sp[k], dst=dp[k];
      int sr=(src>>16)&0xFF, sg=(src>>8)&0xFF, sb=src&0xFF;
      int dr=(dst>>16)&0xFF, dg=(dst>>8)&0xFF, db=dst&0xFF;
      int or_=(int)(sr*sa+dr*ia+bias); if(or_>255) or_=255; else if(or_<0) or_=0;
      int og=(int)(sg*sa+dg*ia+bias); if(og>255) og=255; else if(og<0) og=0;
      int ob=(int)(sb*sa+db*ia+bias); if(ob>255) ob=255; else if(ob<0) ob=0;
      uint32_t effective=(uint32_t)lround((double)aa*alpha);
      if(effective>255u) effective=255u;
      dp[k]=gml_sprite_target_alpha(r,dst,effective)|((uint32_t)or_<<16)|
            ((uint32_t)og<<8)|(uint32_t)ob;
    }
    return;
  }
  uint32_t effective=(uint32_t)(aa*alpha+0.5);
  if(effective>255u) effective=255u;
  uint32_t inverse=255u-effective;
  for(int k=0; k<run; k++){
    uint32_t src=sp[k], dst=dp[k];
    uint32_t sr=(src>>16)&0xFFu, sg=(src>>8)&0xFFu, sb=src&0xFFu;
    uint32_t dr=(dst>>16)&0xFFu, dg=(dst>>8)&0xFFu, db=dst&0xFFu;
    uint32_t rr=(sr*effective+dr*inverse+127u)/255u;
    uint32_t rg=(sg*effective+dg*inverse+127u)/255u;
    uint32_t rb=(sb*effective+db*inverse+127u)/255u;
    dp[k]=gml_sprite_target_alpha(r,dst,effective)|(rr<<16)|(rg<<8)|rb;
  }
}
static int blit_tpag_scale1_white_exact(GmlRender *r, GmlTpag *t, GmlAtlas *a,
                                        int x0, int y0, int xx0, int xx1, int yy0, int yy1){
  if(!r || !t || !a || !a->px || !r->fb || xx1<=xx0 || yy1<=yy0) return 0;
  int abx0=0, aby0=0, abx1=t->sw-1, aby1=t->sh-1;
  if(!tpag_alpha_bounds(r,t,a,&abx0,&aby0,&abx1,&aby1)) return 1;
  if(!t->alpha_row_min || !t->alpha_row_max) return 0;
  uint32_t *cache=tpag_argb_cache(r,t,a);
  if(!cache) return 0;
  const GmlTpagAlphaRun *runs=NULL;
  int run_count=0;
  if(tpag_alpha_runs(r,t,a,&runs,&run_count)){
    for(int ri=0; ri<run_count; ri++){
      const GmlTpagAlphaRun *ar=&runs[ri];
      int yy=(int)ar->y;
      if(yy<yy0 || yy>=yy1) continue;
      int sx0=(int)ar->x;
      int sx1=sx0+(int)ar->len;
      if(sx0<xx0) sx0=xx0;
      if(sx1>xx1) sx1=xx1;
      if(sx1<=sx0) continue;
      uint32_t *dp=r->fb+(size_t)(y0+yy)*r->fbw+x0+sx0;
      const uint32_t *sp=cache+(size_t)yy*t->sw+sx0;
      int n=sx1-sx0;
      if(!r->alphablend) copy_argb_force_opaque(dp,sp,n);
      else if(ar->alpha==255u) memcpy(dp,sp,(size_t)n*sizeof(uint32_t));
      else blend_argb_src_over_exact(r,dp,sp,n,(uint32_t)ar->alpha,gml_blend_family(r));
    }
    return 1;
  }
  for(int yy=yy0; yy<yy1; yy++){
    int mn=t->alpha_row_min[yy], mx=t->alpha_row_max[yy];
    if(mx<mn) continue;
    int sx0=xx0>mn?xx0:mn;
    int sx1=(xx1-1)<mx?(xx1-1):mx;
    if(sx1<sx0) continue;
    uint32_t *dp=r->fb+(size_t)(y0+yy)*r->fbw+x0+sx0;
    const uint32_t *sp=cache+(size_t)yy*t->sw+sx0;
    int n=sx1-sx0+1;
    if(!r->alphablend){
      for(int i=0; i<n; ){
        while(i<n && !(sp[i]>>24)) i++;
        int j=i;
        while(j<n && (sp[j]>>24)) j++;
        if(j>i) copy_argb_force_opaque(dp+i,sp+i,j-i);
        i=j;
      }
      continue;
    }
    for(int i=0; i<n; ){
      uint32_t aa=sp[i]>>24;
      int run=1;
      while(i+run<n && (sp[i+run]>>24)==aa) run++;
      blend_argb_src_over_exact(r,dp+i,sp+i,run,aa,gml_blend_family(r));
      i+=run;
    }
  }
  return 1;
}
static int blit_tpag_scale1_white_draw_alpha(GmlRender *r, GmlTpag *t, GmlAtlas *a,
                                             int x0, int y0, int xx0, int xx1, int yy0, int yy1,
                                             double alpha){
  if(!r || !t || !a || !a->px || !r->fb || xx1<=xx0 || yy1<=yy0 || alpha<=0.0 || alpha>=1.0) return 0;
  int abx0=0, aby0=0, abx1=t->sw-1, aby1=t->sh-1;
  if(!tpag_alpha_bounds(r,t,a,&abx0,&aby0,&abx1,&aby1)) return 1;
  uint32_t *cache=tpag_argb_cache(r,t,a);
  if(!cache) return 0;
  const GmlTpagAlphaRun *runs=NULL;
  int run_count=0;
  if(!tpag_alpha_runs(r,t,a,&runs,&run_count)) return 0;
  for(int ri=0; ri<run_count; ri++){
    const GmlTpagAlphaRun *ar=&runs[ri];
    int yy=(int)ar->y;
    if(yy<yy0 || yy>=yy1) continue;
    int sx0=(int)ar->x;
    int sx1=sx0+(int)ar->len;
    if(sx0<xx0) sx0=xx0;
    if(sx1>xx1) sx1=xx1;
    if(sx1<=sx0) continue;
    uint32_t *dp=r->fb+(size_t)(y0+yy)*r->fbw+x0+sx0;
    const uint32_t *sp=cache+(size_t)yy*t->sw+sx0;
    int n=sx1-sx0;
    if(!r->alphablend) copy_argb_force_opaque(dp,sp,n);
    else blend_argb_src_over_draw_alpha(r,dp,sp,n,(uint32_t)ar->alpha,alpha,
                                        gml_blend_family(r));
  }
  return 1;
}
static int blit_tpag_scale1_white_exact_flipped(GmlRender *r, GmlTpag *t, GmlAtlas *a,
                                                int x0, int y0, int xx0, int xx1, int yy0, int yy1,
                                                int flipx, int flipy){
  if(!r || !t || !a || !a->px || !r->fb || xx1<=xx0 || yy1<=yy0 || (!flipx && !flipy)) return 0;
  int abx0=0, aby0=0, abx1=t->sw-1, aby1=t->sh-1;
  if(!tpag_alpha_bounds(r,t,a,&abx0,&aby0,&abx1,&aby1)) return 1;
  uint32_t *cache=tpag_argb_cache(r,t,a);
  if(!cache) return 0;
  const GmlTpagAlphaRun *runs=NULL;
  int run_count=0;
  if(!tpag_alpha_runs(r,t,a,&runs,&run_count)) return 0;
  for(int ri=0; ri<run_count; ri++){
    const GmlTpagAlphaRun *ar=&runs[ri];
    int yy=(int)ar->y;
    if(yy<yy0 || yy>=yy1) continue;
    int sx0=(int)ar->x;
    int sx1=sx0+(int)ar->len;
    if(sx0<xx0) sx0=xx0;
    if(sx1>xx1) sx1=xx1;
    if(sx1<=sx0) continue;
    int py=flipy ? (y0-yy) : (y0+yy);
    if(py<0 || py>=r->fbh) continue;
    const uint32_t *sp=cache+(size_t)yy*t->sw+sx0;
    int n=sx1-sx0;
    if(!flipx){
      int px=x0+sx0;
      if(px<0 || px+n>r->fbw) continue;
      uint32_t *dp=r->fb+(size_t)py*r->fbw+px;
      if(!r->alphablend) copy_argb_force_opaque(dp,sp,n);
      else blend_argb_src_over_double(r,dp,sp,n,(uint32_t)ar->alpha);
    } else {
      int px=x0-sx0;
      if(px-(n-1)<0 || px>=r->fbw) continue;
      uint32_t *dp=r->fb+(size_t)py*r->fbw+px;
      if(!r->alphablend) copy_argb_force_opaque_reverse(dp,sp,n);
      else blend_argb_src_over_double_reverse(r,dp,sp,n,(uint32_t)ar->alpha);
    }
  }
  return 1;
}
static int blit_tpag_scale1_white_exact_rotated_tile(
    GmlRender *r,GmlTpag *t,GmlAtlas *a,
    int source_x,int source_y,int source_width,int source_height,
    double destination_x,double destination_y,int mirror,int flip){
  if(!r || !t || !a || !a->px || !r->fb ||
     source_width<=0 || source_height<=0) return 0;
  int local_x=source_x-t->tx;
  int local_y=source_y-t->ty;
  if(local_x<0 || local_y<0 ||
     local_x+source_width>t->sw || local_y+source_height>t->sh) return 0;
  uint32_t *cache=tpag_argb_cache(r,t,a);
  if(!cache) return 0;
  int output_width=source_height;
  int output_height=source_width;
  int x0=(int)ceil(destination_x-0.5);
  int y0=(int)ceil(destination_y-0.5);
  int x1=(int)ceil(destination_x+output_width-0.5);
  int y1=(int)ceil(destination_y+output_height-0.5);
  if(x0<0) x0=0;
  if(y0<0) y0=0;
  if(x1>r->fbw) x1=r->fbw;
  if(y1>r->fbh) y1=r->fbh;
  if(x1<=x0 || y1<=y0) return 1;
  gml_render_maybe_prepare_draw(r);
  int family=gml_blend_family(r);
  for(int destination_row=y0;destination_row<y1;destination_row++){
    int output_y=(int)floor(destination_row+0.5-destination_y);
    if(output_y<0 || output_y>=source_width) continue;
    int tile_x=mirror?source_width-1-output_y:output_y;
    uint32_t *destination=
      r->fb+(size_t)destination_row*r->fbw+x0;
    for(int destination_column=x0;destination_column<x1;
        destination_column++,destination++){
      int output_x=(int)floor(destination_column+0.5-destination_x);
      if(output_x<0 || output_x>=source_height) continue;
      int pre_flip_y=source_height-1-output_x;
      int tile_y=flip?source_height-1-pre_flip_y:pre_flip_y;
      uint32_t source=
        cache[(size_t)(local_y+tile_y)*t->sw+local_x+tile_x];
      uint32_t source_alpha=source>>24;
      if(!source_alpha) continue;
      if(!r->alphablend){
        *destination=0xFF000000u|(source&0x00FFFFFFu);
      } else if(source_alpha>=255u){
        *destination=source;
      } else {
        blend_argb_src_over_exact(
          r,destination,&source,1,source_alpha,family);
      }
    }
  }
  return 1;
}
static int tpag_alpha_qrows(GmlRender *r, GmlTpag *t, GmlAtlas *a, int min_alpha,
                            const uint16_t **row_min, const uint16_t **row_max){
  if(row_min) *row_min=NULL;
  if(row_max) *row_max=NULL;
  if(!r || !t || !a || !a->px || min_alpha<=1 || min_alpha>255 || t->sw<=0 || t->sh<=0) return 0;
  if(rprof_tpag_id(r,t)<0 || t->sw>UINT16_MAX-1) return 0;
  if(!t->alpha_qrow_min || !t->alpha_qrow_max || !t->alpha_qrow_built){
    size_t n=(size_t)256*(size_t)t->sh;
    uint16_t *mn=malloc(n*sizeof(uint16_t));
    uint16_t *mx=malloc(n*sizeof(uint16_t));
    uint8_t *built=calloc(256,1);
    if(!mn || !mx || !built){ free(mn); free(mx); free(built); return 0; }
    t->alpha_qrow_min=mn;
    t->alpha_qrow_max=mx;
    t->alpha_qrow_built=built;
  }
  if(!t->alpha_qrow_built[min_alpha]){
    uint16_t *mn=t->alpha_qrow_min+(size_t)min_alpha*(size_t)t->sh;
    uint16_t *mx=t->alpha_qrow_max+(size_t)min_alpha*(size_t)t->sh;
    for(int yy=0; yy<t->sh; yy++){ mn[yy]=UINT16_MAX; mx[yy]=0; }
    for(int yy=0; yy<t->sh; yy++){
      int sy=t->sy+yy;
      if(sy<0 || sy>=a->h) continue;
      const uint8_t *sp=a->px+((size_t)sy*a->w)*4;
      for(int xx=0; xx<t->sw; xx++){
        int sx=t->sx+xx;
        if(sx<0 || sx>=a->w) continue;
        int aa=sp[(size_t)sx*4+3];
        if(aa>=min_alpha){
          if(xx<(int)mn[yy]) mn[yy]=(uint16_t)xx;
          if(xx>(int)mx[yy]) mx[yy]=(uint16_t)xx;
        }
      }
    }
    t->alpha_qrow_built[min_alpha]=1;
  }
  if(!t->alpha_qrow_min || !t->alpha_qrow_max) return 0;
  if(row_min) *row_min=t->alpha_qrow_min+(size_t)min_alpha*(size_t)t->sh;
  if(row_max) *row_max=t->alpha_qrow_max+(size_t)min_alpha*(size_t)t->sh;
  return 1;
}

typedef struct { GmlRender *r; const uint8_t *ap; const ptrdiff_t *colA,*colB,*rowA,*rowB;
  const float *cfx,*cfy; int W,x0,y0; float fa,fbRr,fbGg,fbBb; int noblend; } SprBiCtx;
static void spr_bi_band(void *p, int py0, int py1, int slot){
  (void)slot;
  SprBiCtx *c=(SprBiCtx*)p; GmlRender *r=c->r; const int W=c->W; const uint8_t *ap=c->ap;
  for(int py=py0; py<py1; py++){ int ty_=c->y0+py; if(ty_<0||ty_>=r->fbh) continue;
    ptrdiff_t ra=c->rowA[py], rb=c->rowB[py]; float fy=c->cfy[py], wy0=1.0f-fy;
    uint32_t *dprow=r->fb+(size_t)ty_*r->fbw;
    for(int px=0; px<W; px++){ int tx_=c->x0+px; if(tx_<0||tx_>=r->fbw) continue;
      ptrdiff_t ca=c->colA[px], cb=c->colB[px]; float fx=c->cfx[px], wx0=1.0f-fx;
      float w00=wx0*wy0,w01=fx*wy0,w10=wx0*fy,w11=fx*fy;
      const uint8_t *p00=(ra>=0&&ca>=0)?ap+ra+ca:NULL, *p01=(ra>=0&&cb>=0)?ap+ra+cb:NULL;
      const uint8_t *p10=(rb>=0&&ca>=0)?ap+rb+ca:NULL, *p11=(rb>=0&&cb>=0)?ap+rb+cb:NULL;
      float A0=p00?p00[3]:0,A1=p01?p01[3]:0,A2=p10?p10[3]:0,A3=p11?p11[3]:0;
      float Av=A0*w00+A1*w01+A2*w10+A3*w11;
      if(shader_discards_alpha_value(r,Av)) continue;
      float R0=p00?p00[0]:0,R1=p01?p01[0]:0,R2=p10?p10[0]:0,R3=p11?p11[0]:0;
      float G0=p00?p00[1]:0,G1=p01?p01[1]:0,G2=p10?p10[1]:0,G3=p11?p11[1]:0;
      float B0=p00?p00[2]:0,B1=p01?p01[2]:0,B2=p10?p10[2]:0,B3=p11?p11[2]:0;
      float sample_r=R0*w00+R1*w01+R2*w10+R3*w11;
      float sample_g=G0*w00+G1*w01+G2*w10+G3*w11;
      float sample_b=B0*w00+B1*w01+B2*w10+B3*w11;
      if(mapped_texture_active(r)){
        uint32_t sampled=mapped_texture_pixel(r,
          ((uint32_t)(Av+0.5f)<<24)|((uint32_t)(sample_r+0.5f)<<16)|
          ((uint32_t)(sample_g+0.5f)<<8)|(uint32_t)(sample_b+0.5f));
        Av=(float)(sampled>>24);
        sample_r=(float)((sampled>>16)&255);
        sample_g=(float)((sampled>>8)&255);
        sample_b=(float)(sampled&255);
      }
      float oa=(Av*(1.0f/255.0f))*c->fa; if(oa<=0.0f) continue;
      float sR=sample_r*c->fbRr, sG=sample_g*c->fbGg, sB=sample_b*c->fbBb;
      uint32_t *dp=&dprow[tx_];
      if(c->noblend || oa>=1.0f){ *dp=0xFF000000u|((int)(sR+0.5f)<<16)|((int)(sG+0.5f)<<8)|(int)(sB+0.5f); }
      else { float ia2=1.0f-oa; int dr=(*dp>>16)&0xFF,dg=(*dp>>8)&0xFF,db=*dp&0xFF;
        *dp=0xFF000000u|((int)(sR*oa+dr*ia2+0.5f)<<16)|((int)(sG*oa+dg*ia2+0.5f)<<8)|(int)(sB*oa+db*ia2+0.5f); }
    }
  }
}
void gml_render_sprite_cache_free(GmlSprite *s){
  if(!s) return;
  free(s->runtime_axis_cache_px);
  free(s->runtime_axis_cache_alpha);
  free(s->runtime_axis_cache_row_min);
  free(s->runtime_axis_cache_row_max);
  free(s->runtime_axis_cache_runs);
  s->runtime_axis_cache_px=NULL;
  s->runtime_axis_cache_alpha=NULL;
  s->runtime_axis_cache_row_min=NULL;
  s->runtime_axis_cache_row_max=NULL;
  s->runtime_axis_cache_runs=NULL;
  s->runtime_axis_cache_run_count=0;
  s->runtime_axis_cache_valid=0;
  s->runtime_axis_cache_copy_255=0;
  s->runtime_axis_cache_uniform_alpha=0;
  s->runtime_axis_cache_full_rect=0;
  s->runtime_axis_pending_count=0;
}
static int runtime_frame_index(GmlSprite *s, int frame){
  if(!s || s->n_frames<=0) return 0;
  return ((frame%s->n_frames)+s->n_frames)%s->n_frames;
}
const uint8_t *runtime_frame_rgba(GmlSprite *s, int frame){
  if(!s->runtime_rgba || s->w<=0 || s->h<=0 || s->n_frames<=0) return NULL;
  int sub=runtime_frame_index(s,frame);
  return s->runtime_rgba + (size_t)sub*s->w*s->h*4;
}

static int runtime_axis_key_eq(const GmlRuntimeAxisKey *a, const GmlRuntimeAxisKey *b){
  return a->src==b->src && a->sw==b->sw && a->sh==b->sh &&
         a->fbw==b->fbw && a->fbh==b->fbh && a->x0==b->x0 && a->y0==b->y0 &&
         a->w==b->w && a->h==b->h && a->originx==b->originx && a->originy==b->originy &&
         a->blend==b->blend && a->ax==b->ax && a->ay==b->ay && a->xs==b->xs &&
         a->ys==b->ys && a->alpha==b->alpha;
}
static void GML_HOT_RENDER runtime_axis_cache_copy(GmlRender *r, GmlSprite *s){
  GmlRuntimeAxisKey *k=&s->runtime_axis_cache_key;
  if(s->runtime_axis_cache_full_rect && !s->runtime_axis_cache_alpha &&
     k->x0==0 && k->w==r->fbw && k->y0>=0 && k->y0+k->h<=r->fbh){
    uint32_t *dp=r->fb+(size_t)k->y0*r->fbw;
    uint32_t *sp=s->runtime_axis_cache_px;
    int n=k->w*k->h;
    if(s->runtime_axis_cache_uniform_alpha && !s->runtime_axis_cache_copy_255)
      blend_fast8_src_run(dp,sp,n,(uint32_t)s->runtime_axis_cache_uniform_alpha);
    else if(!s->runtime_axis_cache_alpha)
      memcpy(dp,sp,(size_t)n*sizeof(uint32_t));
    return;
  }
  if(s->runtime_axis_cache_runs && s->runtime_axis_cache_run_count>0){
    for(int i=0; i<s->runtime_axis_cache_run_count; i++){
      const GmlRuntimeAxisRun *run=&s->runtime_axis_cache_runs[i];
      uint32_t *dp=r->fb+(size_t)(k->y0+run->y)*r->fbw+k->x0+run->x;
      uint32_t *sp=s->runtime_axis_cache_px+(size_t)run->y*k->w+run->x;
      uint32_t af=run->alpha;
      if(s->runtime_axis_cache_copy_255 && af==255u) memcpy(dp,sp,(size_t)run->len*sizeof(uint32_t));
      else blend_fast8_src_run(dp,sp,run->len,af);
    }
    return;
  }
  for(int yy=0; yy<k->h; yy++){
    int mn=s->runtime_axis_cache_row_min[yy], mx=s->runtime_axis_cache_row_max[yy];
    if(mx<mn) continue;
    uint32_t *dp=r->fb+(size_t)(k->y0+yy)*r->fbw+k->x0+mn;
    uint32_t *sp=s->runtime_axis_cache_px+(size_t)yy*k->w+mn;
    uint8_t *ap=s->runtime_axis_cache_alpha ? s->runtime_axis_cache_alpha+(size_t)yy*k->w+mn : NULL;
    if(!ap){
      int run=mx-mn+1;
      if(s->runtime_axis_cache_uniform_alpha && !s->runtime_axis_cache_copy_255)
        blend_fast8_src_run(dp,sp,run,(uint32_t)s->runtime_axis_cache_uniform_alpha);
      else
        memcpy(dp,sp,(size_t)run*sizeof(uint32_t));
      continue;
    }
    for(int xx=mn; xx<=mx; ){
      uint32_t af=*ap;
      int run=1;
      while(xx+run<=mx && ap[run]==af) run++;
      if(!af){ dp+=run; sp+=run; ap+=run; xx+=run; continue; }
      if(s->runtime_axis_cache_copy_255 && af==255u){
        memcpy(dp,sp,(size_t)run*sizeof(uint32_t));
      } else {
        blend_fast8_src_run(dp,sp,run,af);
      }
      dp+=run;
      sp+=run;
      ap+=run;
      xx+=run;
    }
  }
}
static int runtime_axis_cache_add_run(GmlRuntimeAxisRun **runs, int *count, int *cap,
                                      int y, int x, int len, uint32_t alpha){
  if(!runs || !count || !cap || len<=0 || !alpha || alpha>255u) return 1;
  if(*count>0){
    GmlRuntimeAxisRun *last=&(*runs)[*count-1];
    if(last->y==y && last->x+last->len==x && last->alpha==(uint8_t)alpha){
      last->len+=len;
      return 1;
    }
  }
  if(*count>=*cap){
    if(*cap>=131072) return 0;
    int ncap=*cap ? *cap*2 : 1024;
    if(ncap>131072) ncap=131072;
    GmlRuntimeAxisRun *nr=realloc(*runs,(size_t)ncap*sizeof(**runs));
    if(!nr) return 0;
    *runs=nr;
    *cap=ncap;
  }
  (*runs)[*count]=(GmlRuntimeAxisRun){y,x,len,(uint8_t)alpha};
  (*count)++;
  return 1;
}
static int runtime_axis_cache_build(GmlRender *r, GmlSprite *s, const GmlRuntimeAxisKey *k){
  if(!s || !k || !k->src || k->w<=0 || k->h<=0) return 0;
  size_t n=(size_t)k->w*(size_t)k->h;
  if(n==0 || n>1048576u) return 0;
  int full_copy=s->runtime_opaque && k->alpha>=1.0;
  int uniform_alpha=0;
  if(s->runtime_opaque && !full_copy){
    double sa=k->alpha;
    uint32_t af=sa>=1.0 ? 256u : (uint32_t)(sa*256.0);
    if(!af) return 0;
    uniform_alpha=(int)(af>=256u ? 255u : af);
  }
  uint32_t *px=malloc(n*sizeof(uint32_t));
  uint8_t *am=(full_copy || uniform_alpha) ? NULL : malloc(n);
  int *row_min=malloc((size_t)k->h*sizeof(int));
  int *row_max=malloc((size_t)k->h*sizeof(int));
  GmlRuntimeAxisRun *runs=NULL;
  int run_count=0, run_cap=0, runs_ok=(!full_copy && !uniform_alpha);
  if(!px || ((!full_copy && !uniform_alpha) && !am) || !row_min || !row_max){
    free(px); free(am); free(row_min); free(row_max); return 0;
  }
  int white=((k->blend & 0xFFFFFF)==0xFFFFFF);
  int bR=k->blend&0xFF, bG=(k->blend>>8)&0xFF, bB=(k->blend>>16)&0xFF;
  double invxs=1.0/k->xs, invys=1.0/k->ys;
  int64_t dlx_fp=(int64_t)llround(invxs*(double)RFP_ONE);
  for(int yy=0; yy<k->h; yy++){
    row_min[yy]=k->w;
    row_max[yy]=-1;
    uint32_t *out=px+(size_t)yy*k->w;
    int py=k->y0+yy;
    int64_t ly_fp=(int64_t)floor(((py+0.5-k->ay)*invys + k->originy)*(double)RFP_ONE);
    int ly=floor_fixed20(ly_fp);
    uint8_t *aout=am ? am+(size_t)yy*k->w : NULL;
    if(ly<0 || ly>=k->sh){
      memset(out,0,(size_t)k->w*sizeof(uint32_t));
      if(aout) memset(aout,0,(size_t)k->w);
      continue;
    }
    const uint8_t *srow=k->src+(size_t)ly*k->sw*4;
    int64_t lx_fp=(int64_t)floor(((k->x0+0.5-k->ax)*invxs + k->originx)*(double)RFP_ONE);
    for(int xx=0; xx<k->w; xx++, lx_fp+=dlx_fp){
      int lx=floor_fixed20(lx_fp);
      if(lx<0 || lx>=k->sw){ out[xx]=0; if(aout) aout[xx]=0; continue; }
      const uint8_t *sp=srow+(size_t)lx*4;
      double sa=(sp[3]/255.0)*k->alpha;
      uint32_t af=sa>=1.0 ? 256u : (uint32_t)(sa*256.0);
      if(!af){ out[xx]=0; if(aout) aout[xx]=0; continue; }
      int sr=white?sp[0]:sp[0]*bR/255;
      int sg=white?sp[1]:sp[1]*bG/255;
      int sb=white?sp[2]:sp[2]*bB/255;
      out[xx]=0xFF000000u|((uint32_t)sr<<16)|((uint32_t)sg<<8)|(uint32_t)sb;
      uint32_t a8=af>=256u ? 255u : af;
      if(aout) aout[xx]=(uint8_t)a8;
      if(runs_ok && !runtime_axis_cache_add_run(&runs,&run_count,&run_cap,yy,xx,1,a8)){
        free(runs);
        runs=NULL;
        run_count=0;
        run_cap=0;
        runs_ok=0;
      }
      if(xx<row_min[yy]) row_min[yy]=xx;
      if(xx>row_max[yy]) row_max[yy]=xx;
    }
  }
  if(!runs_ok){
    free(runs);
    runs=NULL;
    run_count=0;
  }
  int full_rect=1;
  for(int yy=0; yy<k->h; yy++){
    if(row_min[yy]!=0 || row_max[yy]!=k->w-1){
      full_rect=0;
      break;
    }
  }
  gml_render_sprite_cache_free(s);
  s->runtime_axis_cache_px=px;
  s->runtime_axis_cache_alpha=am;
  s->runtime_axis_cache_row_min=row_min;
  s->runtime_axis_cache_row_max=row_max;
  s->runtime_axis_cache_runs=runs;
  s->runtime_axis_cache_run_count=run_count;
  s->runtime_axis_cache_key=*k;
  s->runtime_axis_cache_valid=1;
  s->runtime_axis_cache_copy_255=full_copy || k->alpha>=1.0;
  s->runtime_axis_cache_uniform_alpha=uniform_alpha;
  s->runtime_axis_cache_full_rect=full_rect;
  if(log_axis_cache_enabled()){
    if(r->axis_cache_log_count<80){
      int drawn=0;
      for(int yy=0; yy<k->h; yy++){
        int mn=row_min[yy], mx=row_max[yy];
        if(mx>=mn) drawn+=mx-mn+1;
      }
      anygm_host_logf(r && r->win ? r->win->host : NULL,ANYGM_LOG_DEBUG,"[axis-cache] sprite=%s out=%dx%d drawn_span=%d full=%d uniform=%d rect=%d runs=%d fallback=%d alpha=%.3f blend=%06x\n",
              s->name?s->name:"?",k->w,k->h,drawn,full_copy,uniform_alpha,full_rect,run_count,
              (!full_copy && !uniform_alpha && !runs),k->alpha,(unsigned)(k->blend&0xFFFFFF));
      r->axis_cache_log_count++;
    }
  }
  return 1;
}
static int runtime_axis_cache_try(GmlRender *r, GmlSprite *s, const GmlRuntimeAxisKey *k){
  if(!r || !s || !k) return 0;
  if(s->runtime_axis_cache_valid && runtime_axis_key_eq(&s->runtime_axis_cache_key,k)){
    runtime_axis_cache_copy(r,s);
    return 1;
  }
  if(runtime_axis_key_eq(&s->runtime_axis_pending_key,k)){
    s->runtime_axis_pending_count++;
  } else {
    s->runtime_axis_pending_key=*k;
    s->runtime_axis_pending_count=1;
  }
  if(s->runtime_axis_pending_count<2) return 0;
  if(!runtime_axis_cache_build(r,s,k)) return 0;
  runtime_axis_cache_copy(r,s);
  return 1;
}
void blit(GmlRender *r, GmlTpag *t, double dx, double dy, double xs, double ys,
                 uint32_t blend, double alpha);
static void blit_background_phase(GmlRender *r, GmlTpag *t, double dx, double dy,
                                  double xs, double ys, uint32_t blend, double alpha);
/* One band of the point-sampled scaled sprite kernel. Every row derives its source line and its
 * destination row from the row index alone and writes only that row, so bands are independent and
 * each row samples exactly the texel it would have sampled on its own. */
typedef struct AxisSpriteBand {
  GmlRender *r;
  const uint8_t *src;
  const int *row_min,*row_max;
  const uint32_t *a8_lut,*a16_lut;
  double ax,ay,xs,invxs,invys,alpha;
  int64_t dlx_fp;
  int sw,sh,x0,x1,y0,originx,originy,bR,bG,bB,white,fast8_blend;
  uint32_t round_bias;
} AxisSpriteBand;

static void axis_sprite_band(void *context,int row_start,int row_end,int slot){
  const AxisSpriteBand *b=(const AxisSpriteBand*)context;
  GmlRender *r=b->r;
  const uint8_t *src=b->src;
  const int *row_min=b->row_min,*row_max=b->row_max;
  const uint32_t *a8_lut=b->a8_lut,*a16_lut=b->a16_lut;
  const double ax=b->ax,ay=b->ay,xs=b->xs,invxs=b->invxs,invys=b->invys,alpha=b->alpha;
  const int64_t dlx_fp=b->dlx_fp;
  const int sw=b->sw,sh=b->sh,x0=b->x0,x1=b->x1,originx=b->originx,originy=b->originy;
  const int y0=b->y0;
  const int bR=b->bR,bG=b->bG,bB=b->bB,white=b->white,fast8_blend=b->fast8_blend;
  const uint32_t round_bias=b->round_bias;
  (void)slot; (void)invxs;
  for(int py=y0+row_start; py<y0+row_end; py++){
    int ly=(int)floor((py+0.5-ay)*invys + originy);
    if(ly<0 || ly>=sh) continue;
    int rmin=row_min?row_min[ly]:0, rmax=row_max?row_max[ly]:sw-1;
    if(rmax<rmin) continue;
    int px0=x0, px1=x1;
    int span0=(int)ceil(ax + (rmin-originx)*xs - 0.5);
    int span1=(int)ceil(ax + (rmax+1-originx)*xs - 0.5);
    if(px0<span0) px0=span0;
    if(px1>span1) px1=span1;
    if(px0<0) px0=0;
    if(px1>r->fbw) px1=r->fbw;
    if(px1<=px0) continue;
    const uint8_t *srow=src+(size_t)ly*sw*4;
    uint32_t *row=&r->fb[(size_t)py*r->fbw];
    double lx0=(px0+0.5-ax)*invxs + originx;
    int64_t lx_fp=(int64_t)floor(lx0*(double)RFP_ONE);
    for(int px=px0; px<px1; ){
      int lx=floor_fixed20(lx_fp);
      int maxrun=px1-px;
      int run=fixed20_run_to_change(lx_fp,dlx_fp,lx,maxrun);
      if(run<1) run=1;
      if(lx>=0 && lx<sw){
        const uint8_t *sp=srow+(size_t)lx*4;
        int aa=sp[3];
        if(aa){
          int sr=white?sp[0]:sp[0]*bR/255, sg=white?sp[1]:sp[1]*bG/255, sb=white?sp[2]:sp[2]*bB/255;
          uint32_t srcpx=0xFF000000u|((uint32_t)sr<<16)|((uint32_t)sg<<8)|(uint32_t)sb;
          uint32_t *dp=&row[px];
          if(!r->alphablend){
            fill_u32_run(dp,run,srcpx);
          } else {
            if(fast8_blend){
              uint32_t af=a8_lut[aa];
              blend_fast8_run(dp,run,srcpx,af);
            } else {
              uint32_t af=a16_lut[aa];
              if(af>=65536u){
                fill_u32_run(dp,run,srcpx);
              } else if(af){
                uint32_t ia=65536u-af;
                uint32_t srp=(uint32_t)sr*af, sgp=(uint32_t)sg*af, sbp=(uint32_t)sb*af;
                for(int k=0;k<run;k++){
                  uint32_t dv=dp[k];
                  int dr=(dv>>16)&0xFF, dg=(dv>>8)&0xFF, db=dv&0xFF;
                  unsigned source_alpha=(unsigned)lround((double)aa*alpha);
                  dp[k]=gml_sprite_target_alpha(r,dv,source_alpha)|
                      (((srp+dr*ia+round_bias)>>16)<<16)|
                      (((sgp+dg*ia+round_bias)>>16)<<8)|((sbp+db*ia+round_bias)>>16);
                }
              }
            }
          }
        }
      }
      lx_fp += dlx_fp * (int64_t)run;
      px += run;
    }
    }
}

static int GML_HOT_RENDER blit_rgba_sprite_axis(GmlRender *r, GmlSprite *owner, const uint8_t *src, int sw, int sh, double x, double y,
                                                double xs, double ys, int originx, int originy,
                                                uint32_t blend, double alpha, int world_space,
                                                const int *row_min, const int *row_max, int opaque){
  if(!src || sw<=0 || sh<=0 || !r->fb || xs<=0 || ys<=0 || r->blendmode!=0) return 0;
  if(alpha>1) alpha=1; else if(alpha<0) alpha=0;
  if(alpha<=0) return 1;
  double ax=x-(world_space?r->cam_x:0), ay=y-(world_space?r->cam_y:0);
  double minx=ax-originx*xs, maxx=ax+(sw-originx)*xs;
  double miny=ay-originy*ys, maxy=ay+(sh-originy)*ys;
  int x0=(int)floor(minx)-1, x1=(int)ceil(maxx)+1;
  int y0=(int)floor(miny)-1, y1=(int)ceil(maxy)+1;
  if(x0<0) x0=0;
  if(y0<0) y0=0;
  if(x1>r->fbw) x1=r->fbw;
  if(y1>r->fbh) y1=r->fbh;
  if(x1<=x0 || y1<=y0) return 1;
  int bR=blend&0xFF, bG=(blend>>8)&0xFF, bB=(blend>>16)&0xFF;
  int white=((blend & 0xFFFFFF)==0xFFFFFF);
  unsigned long long vispix=(unsigned long long)(x1-x0)*(unsigned long long)(y1-y0);
  if(opaque && alpha>=1.0 && r->blendmode==0) gml_render_maybe_prepare_opaque_rect(r,x0,y0,x1,y1);
  else gml_render_maybe_prepare_draw(r);
  if((alpha<1.0 || r->blendmode==2 || !opaque) && r->alphablend)
    gml_sprite_target_may_change_alpha(r);
  /* The global texture filter reaches runtime RGBA sprites as well as atlas sprites.
   * Sampling identity holds at an integer-aligned 1:1 draw, so that case keeps the exact
   * fast paths; classic partial-row bounds stay with the point-sampled kernels below. */
  if(r->interp && r->win && anygm_policy_has_modern_layer_semantics(r->win) &&
     !row_min && !row_max &&
     !(fabs(xs-1.0)<0.001 && fabs(ys-1.0)<0.001 &&
       fabs(minx-nearbyint(minx))<1e-9 && fabs(miny-nearbyint(miny))<1e-9)){
    int W=x1-x0, H=y1-y0;
    int *cxa=malloc((size_t)W*sizeof(*cxa));
    int *cxb=malloc((size_t)W*sizeof(*cxb));
    float *cfx=malloc((size_t)W*sizeof(*cfx));
    if(cxa && cxb && cfx){
      for(int px=0; px<W; px++){
        double fsx=((double)(x0+px)+0.5-ax)/xs+(double)originx-0.5;
        int sxa=(int)floor(fsx); float fx=(float)(fsx-sxa);
        int sxb=sxa+1;
        if(sxa<0)sxa=0; else if(sxa>sw-1)sxa=sw-1;
        if(sxb<0)sxb=0; else if(sxb>sw-1)sxb=sw-1;
        cxa[px]=sxa; cxb[px]=sxb; cfx[px]=fx;
      }
      RgbaBiCtx ctx={ r,src,cxa,cxb,cfx,sw,sh,W,x0,y0,
        ((double)y0+0.5-ay)/ys+(double)originy-0.5, 1.0/ys,
        (float)alpha,(float)bR*(1.0f/255.0f),(float)bG*(1.0f/255.0f),(float)bB*(1.0f/255.0f),
        opaque && alpha>=1.0 && !r->alphablend };
      gml_run_row_bands(r,H,rgba_bi_band,&ctx);
      free(cxa); free(cxb); free(cfx);
      return 1;
    }
    free(cxa); free(cxb); free(cfx);
  }
  if(owner && !gml_render_target_preserves_alpha(r) && r->alphablend && alpha>0 &&
     xs<1.5 && ys<1.5 && vispix>=262144ull){
    GmlRuntimeAxisKey key={
      src,sw,sh,r->fbw,r->fbh,x0,y0,x1-x0,y1-y0,originx,originy,
      ax,ay,xs,ys,alpha,blend
    };
    if(runtime_axis_cache_try(r,owner,&key)) return 1;
  }
  uint32_t a16_lut[256];
  uint32_t a8_lut[256];
  /* Large runtime RGBA sprites are used for full-screen/background layers.
   * Blend them at framebuffer precision; small sprites keep the exact 16-bit path. */
  int fast8_blend = (!gml_render_target_preserves_alpha(r) && r->alphablend &&
                     r->blendmode==0 && vispix>=262144ull);
  if(fast8_blend && alpha < (1.0/256.0)) return 1;
  for(int i=0;i<256;i++){
    double sa=(i/255.0)*alpha;
    a16_lut[i]=sa>=1.0 ? 65536u : (uint32_t)(sa*65536.0);
    a8_lut[i]=sa>=1.0 ? 256u : (uint32_t)(sa*256.0);
  }
  uint32_t round_bias=r->classic?32768u:0u;
  if(fabs(xs-1.0)<0.001 && fabs(ys-1.0)<0.001){
    for(int py=y0; py<y1; py++){
      int ly=(int)floor(py+0.5-ay+originy);
      if(ly<0 || ly>=sh) continue;
      int rmin=row_min?row_min[ly]:0, rmax=row_max?row_max[ly]:sw-1;
      if(rmax<rmin) continue;
      int lx=(int)floor(x0+0.5-ax+originx);
      int px0=x0, px1=x1;
      if(lx<0){ px0+=-lx; lx=0; }
      if(lx<rmin){ px0+=rmin-lx; lx=rmin; }
      if(lx+(px1-px0)>sw) px1=px0+(sw-lx);
      if(lx+(px1-px0)-1>rmax) px1=px0+(rmax-lx+1);
      if(px1<=px0) continue;
      const uint8_t *sp=src+((size_t)ly*sw+lx)*4;
      uint32_t *dp=r->fb+(size_t)py*r->fbw+px0;
      if(opaque && alpha>=1.0 && (!r->alphablend || fast8_blend)){
        if(white){
          for(int px=px0; px<px1; px++, sp+=4, dp++)
            *dp=0xFF000000u|((uint32_t)sp[0]<<16)|((uint32_t)sp[1]<<8)|(uint32_t)sp[2];
        } else {
          for(int px=px0; px<px1; px++, sp+=4, dp++){
            int sr=sp[0]*bR/255, sg=sp[1]*bG/255, sb=sp[2]*bB/255;
            *dp=0xFF000000u|((uint32_t)sr<<16)|((uint32_t)sg<<8)|(uint32_t)sb;
          }
        }
        continue;
      }
      for(int px=px0; px<px1; px++, sp+=4, dp++){
        int aa=sp[3];
        if(!aa) continue;
        int sr=white?sp[0]:sp[0]*bR/255, sg=white?sp[1]:sp[1]*bG/255, sb=white?sp[2]:sp[2]*bB/255;
        uint32_t srcpx=0xFF000000u|((uint32_t)sr<<16)|((uint32_t)sg<<8)|(uint32_t)sb;
        if(!r->alphablend){ *dp=srcpx; continue; }
        if(fast8_blend){
          uint32_t af=a8_lut[aa];
          if(af>=256u){ *dp=srcpx; continue; }
          if(!af) continue;
          uint32_t ia=256u-af;
          uint32_t srb=(srcpx & 0x00FF00FFu)*af;
          uint32_t sgc=(srcpx & 0x0000FF00u)*af;
          *dp=blend_fast8_cached(*dp,srb,sgc,ia);
        } else {
          uint32_t af=a16_lut[aa];
          if(af>=65536u){ *dp=srcpx; continue; }
          if(!af) continue;
          uint32_t ia=65536u-af;
          uint32_t srp=(uint32_t)sr*af, sgp=(uint32_t)sg*af, sbp=(uint32_t)sb*af;
          uint32_t dv=*dp;
          int dr=(dv>>16)&0xFF, dg=(dv>>8)&0xFF, db=dv&0xFF;
          unsigned source_alpha=(unsigned)lround((double)aa*alpha);
          *dp=gml_sprite_target_alpha(r,dv,source_alpha)|
              (((srp+dr*ia+round_bias)>>16)<<16)|
              (((sgp+dg*ia+round_bias)>>16)<<8)|((sbp+db*ia+round_bias)>>16);
        }
      }
    }
    return 1;
  }
  double invxs=1.0/xs, invys=1.0/ys;
  int64_t dlx_fp=(int64_t)llround(invxs*(double)RFP_ONE);
  if(xs<1.5 && ys<1.5){
    for(int py=y0; py<y1; py++){
      int64_t ly_fp=(int64_t)floor(((py+0.5-ay)*invys + originy)*(double)RFP_ONE);
      int ly=floor_fixed20(ly_fp);
      if(ly<0 || ly>=sh) continue;
      int rmin=row_min?row_min[ly]:0, rmax=row_max?row_max[ly]:sw-1;
      if(rmax<rmin) continue;
      int px0=x0, px1=x1;
      int span0=(int)ceil(ax + (rmin-originx)*xs - 0.5);
      int span1=(int)ceil(ax + (rmax+1-originx)*xs - 0.5);
      if(px0<span0) px0=span0;
      if(px1>span1) px1=span1;
      if(px0<0) px0=0;
      if(px1>r->fbw) px1=r->fbw;
      if(px1<=px0) continue;
      const uint8_t *srow=src+(size_t)ly*sw*4;
      uint32_t *row=&r->fb[(size_t)py*r->fbw];
      int64_t lx_fp=(int64_t)floor(((px0+0.5-ax)*invxs + originx)*(double)RFP_ONE);
      if(opaque && alpha>=1.0 && (!r->alphablend || fast8_blend)){
        if(white){
          for(int px=px0; px<px1; px++, lx_fp+=dlx_fp){
            int lx=floor_fixed20(lx_fp);
            if(lx<0 || lx>=sw) continue;
            const uint8_t *sp=srow+(size_t)lx*4;
            row[px]=0xFF000000u|((uint32_t)sp[0]<<16)|((uint32_t)sp[1]<<8)|(uint32_t)sp[2];
          }
        } else {
          for(int px=px0; px<px1; px++, lx_fp+=dlx_fp){
            int lx=floor_fixed20(lx_fp);
            if(lx<0 || lx>=sw) continue;
            const uint8_t *sp=srow+(size_t)lx*4;
            int sr=sp[0]*bR/255, sg=sp[1]*bG/255, sb=sp[2]*bB/255;
            row[px]=0xFF000000u|((uint32_t)sr<<16)|((uint32_t)sg<<8)|(uint32_t)sb;
          }
        }
        continue;
      }
      for(int px=px0; px<px1; px++, lx_fp+=dlx_fp){
        int lx=floor_fixed20(lx_fp);
        if(lx<0 || lx>=sw) continue;
        const uint8_t *sp=srow+(size_t)lx*4;
        int aa=sp[3];
        if(!aa) continue;
        int sr=white?sp[0]:sp[0]*bR/255, sg=white?sp[1]:sp[1]*bG/255, sb=white?sp[2]:sp[2]*bB/255;
        uint32_t srcpx=0xFF000000u|((uint32_t)sr<<16)|((uint32_t)sg<<8)|(uint32_t)sb;
        uint32_t *dp=&row[px];
        if(!r->alphablend){ *dp=srcpx; continue; }
        if(fast8_blend){
          uint32_t af=a8_lut[aa];
          if(af>=256u){ *dp=srcpx; continue; }
          if(!af) continue;
          uint32_t ia=256u-af;
          uint32_t srb=(srcpx & 0x00FF00FFu)*af;
          uint32_t sgc=(srcpx & 0x0000FF00u)*af;
          *dp=blend_fast8_cached(*dp,srb,sgc,ia);
        } else {
          uint32_t af=a16_lut[aa];
          if(af>=65536u){ *dp=srcpx; continue; }
          if(!af) continue;
          uint32_t ia=65536u-af;
          uint32_t srp=(uint32_t)sr*af, sgp=(uint32_t)sg*af, sbp=(uint32_t)sb*af;
          uint32_t dv=*dp;
          int dr=(dv>>16)&0xFF, dg=(dv>>8)&0xFF, db=dv&0xFF;
          unsigned source_alpha=(unsigned)lround((double)aa*alpha);
          *dp=gml_sprite_target_alpha(r,dv,source_alpha)|
              (((srp+dr*ia+round_bias)>>16)<<16)|
              (((sgp+dg*ia+round_bias)>>16)<<8)|((sbp+db*ia+round_bias)>>16);
        }
      }
    }
    return 1;
  }
  if(vispix>=262144ull){
    AxisSpriteBand band={r,src,row_min,row_max,a8_lut,a16_lut,ax,ay,xs,invxs,invys,alpha,dlx_fp,
                         sw,sh,x0,x1,y0,originx,originy,bR,bG,bB,white,fast8_blend,round_bias};
    gml_run_row_bands(r,y1-y0,axis_sprite_band,&band);
    return 1;
  }
  for(int py=y0; py<y1; py++){
    int ly=(int)floor((py+0.5-ay)*invys + originy);
    if(ly<0 || ly>=sh) continue;
    int rmin=row_min?row_min[ly]:0, rmax=row_max?row_max[ly]:sw-1;
    if(rmax<rmin) continue;
    int px0=x0, px1=x1;
    int span0=(int)ceil(ax + (rmin-originx)*xs - 0.5);
    int span1=(int)ceil(ax + (rmax+1-originx)*xs - 0.5);
    if(px0<span0) px0=span0;
    if(px1>span1) px1=span1;
    if(px0<0) px0=0;
    if(px1>r->fbw) px1=r->fbw;
    if(px1<=px0) continue;
    const uint8_t *srow=src+(size_t)ly*sw*4;
    uint32_t *row=&r->fb[(size_t)py*r->fbw];
    double lx0=(px0+0.5-ax)*invxs + originx;
    int64_t lx_fp=(int64_t)floor(lx0*(double)RFP_ONE);
    for(int px=px0; px<px1; ){
      int lx=floor_fixed20(lx_fp);
      int maxrun=px1-px;
      int run=fixed20_run_to_change(lx_fp,dlx_fp,lx,maxrun);
      if(run<1) run=1;
      if(lx>=0 && lx<sw){
        const uint8_t *sp=srow+(size_t)lx*4;
        int aa=sp[3];
        if(aa){
          int sr=white?sp[0]:sp[0]*bR/255, sg=white?sp[1]:sp[1]*bG/255, sb=white?sp[2]:sp[2]*bB/255;
          uint32_t srcpx=0xFF000000u|((uint32_t)sr<<16)|((uint32_t)sg<<8)|(uint32_t)sb;
          uint32_t *dp=&row[px];
          if(!r->alphablend){
            fill_u32_run(dp,run,srcpx);
          } else {
            if(fast8_blend){
              uint32_t af=a8_lut[aa];
              blend_fast8_run(dp,run,srcpx,af);
            } else {
              uint32_t af=a16_lut[aa];
              if(af>=65536u){
                fill_u32_run(dp,run,srcpx);
              } else if(af){
                uint32_t ia=65536u-af;
                uint32_t srp=(uint32_t)sr*af, sgp=(uint32_t)sg*af, sbp=(uint32_t)sb*af;
                for(int k=0;k<run;k++){
                  uint32_t dv=dp[k];
                  int dr=(dv>>16)&0xFF, dg=(dv>>8)&0xFF, db=dv&0xFF;
                  unsigned source_alpha=(unsigned)lround((double)aa*alpha);
                  dp[k]=gml_sprite_target_alpha(r,dv,source_alpha)|
                      (((srp+dr*ia+round_bias)>>16)<<16)|
                      (((sgp+dg*ia+round_bias)>>16)<<8)|((sbp+db*ia+round_bias)>>16);
                }
              }
            }
          }
        }
      }
      lx_fp += dlx_fp * (int64_t)run;
      px += run;
    }
  }
  return 1;
}
void blit_rgba_sprite(GmlRender *r, GmlSprite *owner, const uint8_t *src, int sw, int sh, double x, double y,
                             double xs, double ys, double rot, int originx, int originy,
                             uint32_t blend, double alpha, int world_space,
                             const int *row_min, const int *row_max, int opaque){
  if(!src || sw<=0 || sh<=0 || !r->fb) return;
  if(alpha>1) alpha=1; else if(alpha<0) alpha=0;
  if(xs==0||ys==0) return;
  int bR=blend&0xFF, bG=(blend>>8)&0xFF, bB=(blend>>16)&0xFF;
  double rr=fmod(rot,360.0); if(rr<0) rr+=360.0;
  const struct GmlShaderPal *wave=radial_wave_active(r);
  const struct GmlShaderPal *uvwave=uv_wave_active(r);
  int mapped_shader=mapped_texture_active(r) || shader_alpha_test_requires_filter(r) ||
                    wave!=NULL || uvwave!=NULL;
  if(!mapped_shader && fabs(rr)<0.001 &&
     blit_rgba_sprite_axis(r,owner,src,sw,sh,x,y,xs,ys,originx,originy,blend,alpha,
                           world_space,row_min,row_max,opaque)) return;
  double c,sn; render_rotation_sincos(rr,&c,&sn);
  double ax=x-(world_space?r->cam_x:0), ay=y-(world_space?r->cam_y:0);
  render_modern_cardinal_anchor(r,rr,xs,ys,c,sn,&ax,&ay);
  double minx=1e30,miny=1e30,maxx=-1e30,maxy=-1e30;
  double corners[4][2]={{0,0},{sw,0},{0,sh},{sw,sh}};
  for(int i=0;i<4;i++){
    double px=(corners[i][0]-originx)*xs, py=(corners[i][1]-originy)*ys;
    double dx=ax + px*c + py*sn, dy=ay - px*sn + py*c;
    if(dx<minx) minx=dx;
    if(dx>maxx) maxx=dx;
    if(dy<miny) miny=dy;
    if(dy>maxy) maxy=dy;
  }
  int x0=(int)floor(minx)-1, x1=(int)ceil(maxx)+1;
  int y0=(int)floor(miny)-1, y1=(int)ceil(maxy)+1;
  if(x0<0) x0=0;
  if(y0<0) y0=0;
  if(x1>r->fbw) x1=r->fbw;
  if(y1>r->fbh) y1=r->fbh;
  if(x1<=x0 || y1<=y0) return;
  gml_render_maybe_prepare_draw(r);
  if((alpha<1.0 || r->blendmode==2 || !opaque) && r->alphablend)
    gml_sprite_target_may_change_alpha(r);
  /* The global texture filter reaches rotated and flipped runtime sprites too; the wave
   * samplers keep their own indexed fetches. */
  int rot_interp=r->interp && r->win && anygm_policy_has_modern_layer_semantics(r->win) &&
                 !wave && !uvwave;
  for(int py=y0; py<y1; py++) for(int px=x0; px<x1; px++){
    double rx=px+0.5-ax, ry=py+0.5-ay;
    double sxr=rx*c - ry*sn, syr=rx*sn + ry*c;
    int lx=(int)floor(sxr/xs + originx), ly=(int)floor(syr/ys + originy);
    uint8_t lerped[4];
    const uint8_t *sp;
    if(rot_interp){
      double u=sxr/xs + originx - 0.5, v=syr/ys + originy - 0.5;
      if(u<-1.0 || v<-1.0 || u>=(double)sw || v>=(double)sh) continue;
      int ua=(int)floor(u), va=(int)floor(v);
      double fx=u-ua, fy=v-va;
      int ub=ua+1, vb=va+1;
      if(ua<0)ua=0; else if(ua>sw-1)ua=sw-1;
      if(ub<0)ub=0; else if(ub>sw-1)ub=sw-1;
      if(va<0)va=0; else if(va>sh-1)va=sh-1;
      if(vb<0)vb=0; else if(vb>sh-1)vb=sh-1;
      const uint8_t *p00=src+((size_t)va*sw+ua)*4u, *p01=src+((size_t)va*sw+ub)*4u;
      const uint8_t *p10=src+((size_t)vb*sw+ua)*4u, *p11=src+((size_t)vb*sw+ub)*4u;
      double w00=(1.0-fx)*(1.0-fy), w01=fx*(1.0-fy), w10=(1.0-fx)*fy, w11=fx*fy;
      for(int ch=0;ch<4;ch++)
        lerped[ch]=(uint8_t)(p00[ch]*w00+p01[ch]*w01+p10[ch]*w10+p11[ch]*w11+0.5);
      sp=lerped;
    } else {
      if(lx<0||ly<0||lx>=sw||ly>=sh) continue;
      int source_index=wave ? radial_wave_sample_index(wave,sw,sh,lx,ly) :
        (uvwave ? uv_wave_sample_index(uvwave,sw,sh,lx,ly,
           world_space ? (double)py+0.5+r->cam_y : (double)py+0.5) : ly*sw+lx);
      sp=src+(size_t)source_index*4u;
    }
      /* The fixed-function subtract preset is (zero, inverse source colour): RGB depends on the
       * sampled colour, not on source alpha.  Treating ordinary zero coverage as a discarded
       * fragment therefore makes a transparent black mask a no-op instead of copying the
       * destination.  An explicit alpha test or shader discard still wins, as on hardware. */
      if(shader_discards_alpha(r,sp[3]) &&
         !(r->blendmode==2 && !shader_alpha_test_active(r))) continue;
      /* An ordered-dither pass keeps or drops the whole texel by its position. The fragment reads
       * the interpolated object-space vertex position, which for this blit is the destination pixel
       * carried back into world space the same way the wave sampler above does it. */
      if(shader_ordered_dither_drops(r,
           world_space ? (double)px+0.5+r->cam_x : (double)px+0.5,
           world_space ? (double)py+0.5+r->cam_y : (double)py+0.5)) continue;
      uint32_t sampled=((uint32_t)sp[3]<<24)|((uint32_t)sp[0]<<16)|
                       ((uint32_t)sp[1]<<8)|(uint32_t)sp[2];
      if(mapped_texture_active(r)) sampled=mapped_texture_pixel(r,sampled);
      int sample_a=(int)(sampled>>24);
      double sa=(sample_a/255.0)*alpha;
      if(sa<=0 && r->blendmode!=2) continue;
      uint32_t *dp=&r->fb[(size_t)py*r->fbw+px];
      int sr=((sampled>>16)&255)*bR/255;
      int sg=((sampled>>8)&255)*bG/255;
      int sb=(sampled&255)*bB/255;
      if(!r->alphablend){ *dp=0xFF000000u|(sr<<16)|(sg<<8)|sb; continue; }
      uint32_t destination=*dp;
      int dr=(destination>>16)&0xFF, dg=(destination>>8)&0xFF, db=destination&0xFF;
      if(r->blendmode==1){ int ar=dr+(int)(sr*sa); if(ar>255)ar=255; int ag=dg+(int)(sg*sa); if(ag>255)ag=255; int ab=db+(int)(sb*sa); if(ab>255)ab=255; *dp=0xFF000000u|(ar<<16)|(ag<<8)|ab; continue; }
      if(r->blendmode==2){
        unsigned ar=gml_blend_inv_source_u8((unsigned)dr,(unsigned)sr);
        unsigned ag=gml_blend_inv_source_u8((unsigned)dg,(unsigned)sg);
        unsigned ab=gml_blend_inv_source_u8((unsigned)db,(unsigned)sb);
        *dp=0xFF000000u|(ar<<16)|(ag<<8)|ab; continue;
      }
      if(r->blendmode==4){
        unsigned source_alpha=(unsigned)lround((double)sample_a*alpha);
        *dp=color_write_merge(r,*dp,
          blend_max_preset_pixel(r,*dp,sr,sg,sb,source_alpha));
        continue;
      }
      int or_=(int)(sr*sa+dr*(1-sa)); if(or_>255) or_=255; else if(or_<0) or_=0;
      int og=(int)(sg*sa+dg*(1-sa)); if(og>255) og=255; else if(og<0) og=0;
      int ob=(int)(sb*sa+db*(1-sa)); if(ob>255) ob=255; else if(ob<0) ob=0;
    unsigned source_alpha=(unsigned)lround((double)sample_a*alpha);
    *dp=gml_sprite_target_alpha(r,destination,source_alpha)|(or_<<16)|(og<<8)|ob;
  }
}
static void blit_rgba_region(GmlRender *r, const uint8_t *src, int iw, int ih,
                             double sx0d, double sy0d, double swd, double shd,
                             double dx, double dy, double dw, double dh,
                             uint32_t blend, double alpha){
  if(!src || iw<=0 || ih<=0 || !r->fb) return;
  if(alpha>1) alpha=1; else if(alpha<0) alpha=0;
  int x0=(int)floor(dx), y0=(int)floor(dy), W=(int)lround(dw), H=(int)lround(dh);
  if(swd<=0||shd<=0||W==0||H==0) return;
  int flipx=W<0, flipy=H<0; if(W<0) W=-W; if(H<0) H=-H;
  if(alpha<=0) return;
  int bR=blend&0xFF, bG=(blend>>8)&0xFF, bB=(blend>>16)&0xFF;
  if(!flipx && !flipy && W==iw && H==ih &&
     fabs(sx0d) < 0.001 && fabs(sy0d) < 0.001 &&
     fabs(swd - iw) < 0.001 && fabs(shd - ih) < 0.001){
    int cx0=x0<0?0:x0, cy0=y0<0?0:y0;
    int cx1=x0+W; if(cx1>r->fbw) cx1=r->fbw;
    int cy1=y0+H; if(cy1>r->fbh) cy1=r->fbh;
    int cw=cx1-cx0, ch=cy1-cy0;
    if(cw<=0 || ch<=0) return;
    int sx_start=cx0-x0, sy_start=cy0-y0;
    gml_render_maybe_prepare_draw(r);
    for(int yy=0; yy<ch; yy++){
      const uint8_t *sp=src+((size_t)(sy_start+yy)*iw+sx_start)*4;
      uint32_t *dp=r->fb+(size_t)(cy0+yy)*r->fbw+cx0;
      for(int xx=0; xx<cw; xx++, sp+=4){
        if(shader_discards_alpha(r,sp[3])) continue;
        uint32_t sampled=((uint32_t)sp[3]<<24)|((uint32_t)sp[0]<<16)|
                         ((uint32_t)sp[1]<<8)|(uint32_t)sp[2];
        if(mapped_texture_active(r)) sampled=mapped_texture_pixel(r,sampled);
        int sample_a=(int)(sampled>>24);
        double sa=(sample_a/255.0)*alpha;
        if(sa<=0) continue;
        int sr=((sampled>>16)&255)*bR/255;
        int sg=((sampled>>8)&255)*bG/255;
        int sb=(sampled&255)*bB/255;
        if(!r->alphablend || sa>=1.0) dp[xx]=0xFF000000u|(sr<<16)|(sg<<8)|sb;
        else {
          uint32_t dv=dp[xx];
          double ia=1.0-sa;
          int dr=(dv>>16)&0xFF, dg=(dv>>8)&0xFF, db=dv&0xFF;
          unsigned source_alpha=(unsigned)lround((double)sample_a*alpha);
          dp[xx]=gml_sprite_target_alpha(r,dv,source_alpha)|
                 ((int)(sr*sa+dr*ia)<<16)|((int)(sg*sa+dg*ia)<<8)|
                 (int)(sb*sa+db*ia);
        }
      }
    }
    return;
  }
  gml_render_maybe_prepare_draw(r);
  for(int py=0; py<H; py++){ int ty_=y0+py; if(ty_<0||ty_>=r->fbh) continue;
    int dpy=flipy?(H-1-py):py;
    int sy0=(int)floor(sy0d + (dpy*shd)/H), sy1=(int)floor(sy0d + ((dpy+1)*shd)/H);
    if(sy1<=sy0) sy1=sy0+1;
    if(sy0<0) sy0=0;
    if(sy1>ih) sy1=ih;
    for(int px=0; px<W; px++){ int tx_=x0+px; if(tx_<0||tx_>=r->fbw) continue;
      int dpx=flipx?(W-1-px):px;
      int sx0=(int)floor(sx0d + (dpx*swd)/W), sx1=(int)floor(sx0d + ((dpx+1)*swd)/W);
      if(sx1<=sx0) sx1=sx0+1;
      if(sx0<0) sx0=0;
      if(sx1>iw) sx1=iw;
      int R=0,G=0,B=0,A=0,n=0;
      for(int sy=sy0;sy<sy1;sy++) for(int sx=sx0;sx<sx1;sx++){
        const uint8_t *sp=src+((size_t)sy*iw+sx)*4;
        R+=sp[0]; G+=sp[1]; B+=sp[2]; A+=sp[3]; n++;
      }
      if(!n) continue;
      double sample_alpha=A/(double)n;
      if(shader_discards_alpha_value(r,sample_alpha)) continue;
      int sample_a=(int)floor(sample_alpha+0.5);
      int sample_r=(int)floor(R/(double)n+0.5);
      int sample_g=(int)floor(G/(double)n+0.5);
      int sample_b=(int)floor(B/(double)n+0.5);
      if(mapped_texture_active(r)){
        uint32_t sampled=mapped_texture_pixel(r,((uint32_t)sample_a<<24)|
          ((uint32_t)sample_r<<16)|((uint32_t)sample_g<<8)|(uint32_t)sample_b);
        sample_a=(int)(sampled>>24);
        sample_r=(sampled>>16)&255;
        sample_g=(sampled>>8)&255;
        sample_b=sampled&255;
      }
      double oa=(sample_a/255.0)*alpha; if(oa<=0) continue;
      uint32_t *dp=&r->fb[(size_t)ty_*r->fbw+tx_];
      int sr=sample_r*bR/255, sg=sample_g*bG/255, sb=sample_b*bB/255;
      if(!r->alphablend){ *dp=0xFF000000u|(sr<<16)|(sg<<8)|sb; continue; }
      uint32_t destination=*dp;
      int dr=(destination>>16)&0xFF, dg=(destination>>8)&0xFF, db=destination&0xFF;
      unsigned source_alpha=(unsigned)lround((double)sample_a*alpha);
      *dp=gml_sprite_target_alpha(r,destination,source_alpha)|
          ((int)(sr*oa+dr*(1-oa))<<16)|((int)(sg*oa+dg*(1-oa))<<8)|
          (int)(sb*oa+db*(1-oa));
    }
  }
}
static int tpag_part_view(const GmlTpag *t,
                          double sx,double sy,double sw,double sh,
                          GmlTpag *view,int *source_x,int *source_y,
                          double *overlap_x0,double *overlap_y0,
                          double *overlap_x1,double *overlap_y1){
  if(!t || !view || sw<=0 || sh<=0) return 0;
  double ix0d=fmax(sx,(double)t->tx), iy0d=fmax(sy,(double)t->ty);
  double ix1d=fmin(sx+sw,(double)(t->tx+t->sw)), iy1d=fmin(sy+sh,(double)(t->ty+t->sh));
  int ix0=(int)floor(ix0d), iy0=(int)floor(iy0d);
  int ix1=(int)ceil(ix1d), iy1=(int)ceil(iy1d);
  if(ix1<=ix0 || iy1<=iy0) return 0;
  /* The continuous overlap, before the outward snap, so callers can keep the selected extent. */
  if(overlap_x0) *overlap_x0=ix0d;
  if(overlap_y0) *overlap_y0=iy0d;
  if(overlap_x1) *overlap_x1=ix1d;
  if(overlap_y1) *overlap_y1=iy1d;
  *view=*t;
  view->sx=t->sx + (ix0 - t->tx);
  view->sy=t->sy + (iy0 - t->ty);
  view->sw=ix1-ix0;
  view->sh=iy1-iy0;
  view->tx=0; view->ty=0;
  /* the copy dies on return, so any alpha scan on it is thrown-away work redone EVERY call
   * (a big tiled background part re-scanned its full sub-rect per draw); and the pointer
   * caches copied from the parent describe the PARENT's geometry, not this sub-rect.
   * Mark it pre-scanned with conservative full-rect bounds and detach the caches. */
  view->alpha_scanned=1;
  view->ax0=0; view->ay0=0; view->ax1=view->sw-1; view->ay1=view->sh-1;
  view->alpha_max=t->alpha_scanned ? t->alpha_max : 255;
  view->alpha_partial=t->alpha_scanned ? t->alpha_partial : 1;
  view->alpha_row_min=view->alpha_row_max=NULL;
  view->alpha_qrow_min=view->alpha_qrow_max=NULL; view->alpha_qrow_built=NULL;
  view->alpha_runs=NULL; view->alpha_run_count=0; view->alpha_runs_built=0;
  view->alpha8_cache=NULL;
  view->argb_cache=NULL;
  view->solid_blur_alpha_cache=NULL; view->solid_blur_alpha_shader=-1;
  view->interp_phase_cache[0]=view->interp_phase_cache[1]=view->interp_phase_cache[2]=NULL;
  view->fast8_draw_cache=NULL; view->fast8_draw_cache_valid=0; view->fast8_draw_pending_count=0;
  view->interp_draw_cache=NULL; view->interp_draw_runs=NULL;
  view->interp_draw_cache_bytes=0; view->interp_draw_run_count=0;
  view->interp_draw_cache_valid=0; view->interp_draw_pending_count=0;
  if(source_x) *source_x=ix0;
  if(source_y) *source_y=iy0;
  return 1;
}
/* The selected source rectangle is snapped outwards to whole texels, because a texel is the
 * smallest thing the sampler can address. Along an axis where the continuous selection lies inside
 * a single texel that snapping also inflates the DESTINATION extent, from `selected*scale` up to a
 * full `1*scale`. At a magnifying scale that surplus is a visible band. Keep the one selected texel,
 * but scale its one-cell view by the exact continuous overlap and anchor it at the continuous
 * clipped edge, so the destination extent stays `selected*scale` and the covered pixels remain
 * those whose centres fall inside it. Multi-texel selections address whole texels already and are
 * left exactly as they were. */
static void blit_tpag_part_with_phase(GmlRender *r, GmlTpag *t,
                                      double sx, double sy, double sw, double sh,
                                      double dx, double dy, double xs, double ys,
                                      uint32_t blend, double alpha, int advance_y){
  if(!t || sw<=0 || sh<=0 || xs==0 || ys==0) return;
  GmlTpag tt;
  int ix0=0,iy0=0;
  double overlap_x0=0.0,overlap_y0=0.0,overlap_x1=0.0,overlap_y1=0.0;
  if(!tpag_part_view(t,sx,sy,sw,sh,&tt,&ix0,&iy0,
                     &overlap_x0,&overlap_y0,&overlap_x1,&overlap_y1)) return;
  double anchor_x=ix0,anchor_y=iy0;
  double cell_xs=xs,cell_ys=ys;
  if(tt.sw==1){ anchor_x=overlap_x0; cell_xs=(overlap_x1-overlap_x0)*xs; }
  if(tt.sh==1){ anchor_y=overlap_y0; cell_ys=(overlap_y1-overlap_y0)*ys; }
  if(cell_xs==0.0 || cell_ys==0.0) return;
  if(advance_y)
    blit_background_phase(r,&tt,dx+(anchor_x-sx)*xs,dy+(anchor_y-sy)*ys,
                          cell_xs,cell_ys,blend,alpha);
  else
    blit(r,&tt,dx+(anchor_x-sx)*xs,dy+(anchor_y-sy)*ys,cell_xs,cell_ys,blend,alpha);
}
static void blit_tpag_part(GmlRender *r, GmlTpag *t, double sx, double sy, double sw, double sh,
                           double dx, double dy, double xs, double ys, uint32_t blend, double alpha){
  blit_tpag_part_with_phase(r,t,sx,sy,sw,sh,dx,dy,xs,ys,blend,alpha,0);
}
static void blit_tpag_part_background(GmlRender *r, GmlTpag *t,
                                      double sx, double sy, double sw, double sh,
                                      double dx, double dy, double xs, double ys,
                                      uint32_t blend, double alpha){
  blit_tpag_part_with_phase(r,t,sx,sy,sw,sh,dx,dy,xs,ys,blend,alpha,1);
}
/* alpha (0-255) of a sprite frame at SPRITE-LOCAL pixel (lx,ly) in [0,w)x[0,h); 0 outside the
 * trimmed image. Used for per-pixel (precise) collision masks. */
int gml_sprite_alpha(GmlRender *r, int sprite, int frame, int lx, int ly){
  if(sprite<0||sprite>=r->n_spr) return 0;
  GmlSprite *s=&r->spr[sprite]; if(s->n_frames<=0) return 0;
  if(s->runtime_rgba){
    const uint8_t *fr=runtime_frame_rgba(s,frame); if(!fr) return 0;
    if(lx<0||ly<0||lx>=s->w||ly>=s->h) return 0;
    return fr[((size_t)ly*s->w+lx)*4+3];
  }
  int sub=((frame%s->n_frames)+s->n_frames)%s->n_frames;
  int ti=s->frame[sub]; if(ti<0||ti>=r->n_tpag) return 0;
  GmlTpag *t=&r->tpag[ti];
  int ix=lx - t->tx, iy=ly - t->ty;                 /* into the trimmed sub-image */
  if(ix<0||iy<0||ix>=t->sw||iy>=t->sh) return 0;
  if(t->atlas<0||t->atlas>=r->n_atlas) return 0;
  GmlAtlas *a=&r->atlas[t->atlas]; if(!atlas_pixels(r,t->atlas)) return 0;
  int ax=t->sx+ix, ay=t->sy+iy;
  if(ax<0||ay<0||ax>=a->w||ay>=a->h) return 0;
  return a->px[((size_t)ay*a->w+ax)*4+3];
}
static void blit_interp_sample(GmlRender *r, uint32_t *dp, const GmlAtlas *atlas,
                               const GmlTpag *t, int ua, int ub, int va, int vb,
                               double fx, double fy, int logical_margin,
                               int transparent_tap, int bR, int bG, int bB,
                               uint32_t blend, double alpha);
typedef struct {
  GmlRender *r;
  const GmlAtlas *atlas;
  const GmlTpag *tpag;
  const struct GmlShaderPal *solid_mask;
  const uint32_t *solid_mask_argb;
  const struct GmlShaderPal *solid_blur;
  const uint32_t *solid_blur_alpha;
  const int *source_a;
  const double *fraction_x;
  int flip_x, flip_y, x0, y0, source_y0, source_x0, source_x1;
  int logical_margin, blend_r, blend_g, blend_b;
  int solid_red, solid_green, solid_blue;
  double destination_x, destination_y, abs_xscale, abs_yscale, alpha;
  uint32_t blend;
} GmlInterpBlitBand;
static void interp_blit_band_rows(void *context, int row_start, int row_end, int slot);
static int tpag_interp_draw_cache_try(GmlRender *render,GmlTpag *tpag,
                                      const GmlInterpBlitBand *band,int row_count);
typedef struct {
  GmlRender *render;
  const GmlTpag *tpag;
  const uint32_t *source;
  const int *source_x;
  int destination_x,destination_y,source_x0,source_x1,source_y0;
  double abs_yscale,sample_y;
  uint32_t solid_rgb;
  uint8_t mapped_alpha[256];
} GmlNearestSolidMaskBand;
static void nearest_solid_mask_band_rows(
    void *context,int row_start,int row_end,int slot){
  GmlNearestSolidMaskBand *band=(GmlNearestSolidMaskBand*)context;
  (void)slot;
  for(int row=row_start;row<row_end;row++){
    int yy=band->source_y0+row;
    int source_y=(int)((yy+band->sample_y)/band->abs_yscale);
    if(source_y<0 || source_y>=band->tpag->sh) continue;
    const uint32_t *source_row=band->source+(size_t)source_y*band->tpag->sw;
    uint32_t *destination_row=
      band->render->fb+(size_t)(band->destination_y+yy)*band->render->fbw;
    int xx=band->source_x0;
    for(;xx+3<band->source_x1;xx+=4){
      uint32_t alpha[4];
      for(int lane=0;lane<4;lane++){
        int source_x=band->source_x[xx+lane-band->source_x0];
        alpha[lane]=(source_x>=0 && source_x<band->tpag->sw)
          ? band->mapped_alpha[source_row[source_x]>>24] : 0;
      }
      blend_solid_trunc255_4(
        destination_row+band->destination_x+xx,band->solid_rgb,alpha);
    }
    for(;xx<band->source_x1;xx++){
      int source_x=band->source_x[xx-band->source_x0];
      if(source_x<0 || source_x>=band->tpag->sw) continue;
      unsigned alpha=band->mapped_alpha[source_row[source_x]>>24];
      if(!alpha) continue;
      uint32_t *destination=destination_row+band->destination_x+xx;
      uint32_t value=*destination;
      unsigned inverse=255u-alpha;
      unsigned red=(((band->solid_rgb>>16)&255u)*alpha+
                    ((value>>16)&255u)*inverse)/255u;
      unsigned green=(((band->solid_rgb>>8)&255u)*alpha+
                      ((value>>8)&255u)*inverse)/255u;
      unsigned blue=((band->solid_rgb&255u)*alpha+
                     (value&255u)*inverse)/255u;
      *destination=0xFF000000u|(red<<16)|(green<<8)|blue;
    }
  }
}
/* blit one TPAG sub-rect; nearest-neighbour scale; per-pixel alpha; blend multiply.
 * Handles negative xscale/yscale (horizontal/vertical mirror): the caller's (dx,dy) is the anchor
 * edge for the scale sign, and we walk the destination outward (right/down for +, left/up for −)
 * while sampling the source left-to-right/top-to-bottom. rotation not yet handled (rot ignored). */
/* One band of blit_one's general point-sampled loop. Every row derives its source line and its
 * destination row from the row index alone and writes only that row, so bands are independent; the
 * per-column source table, the wave map and the shader state are all built before the bands run and
 * only read inside them. The prior pixel loop remains whole inside this callback. */
typedef struct GmlBlitOneBand {
  GmlRender *r;
  GmlAtlas *a;
  GmlTpag *t;
  const int *lxtab;
  const int *wave_map;
  const uint32_t *solid_blur_alpha;
  const struct GmlShaderPal *solid_blur;
  int fastcase;
  int flipx,flipy;
  int x0,y0;
  int xx0,xx1,yy0;
  int sx_max;
  int first_generation_edge_phase_x,first_generation_edge_phase_y;
  int reciprocal_x,reciprocal_y;
  double sample_x,sample_y;
  double axs,ays;
  double alpha;
  int bR,bG,bB;
} GmlBlitOneBand;

typedef struct GmlNearestOpaqueAlphaBand {
  GmlRender *render;
  const GmlTpag *tpag;
  const uint32_t *source;
  const int *source_x;
  int flip_x,flip_y;
  int anchor_x,anchor_y;
  int output_x0,output_x1,output_y0;
  double sample_y,scale_y;
  double source_weight,destination_weight;
  unsigned source_alpha;
  int preserve_alpha;
} GmlNearestOpaqueAlphaBand;

static void nearest_opaque_alpha_band_rows(
    void *context,int row_start,int row_end,int slot){
  GmlNearestOpaqueAlphaBand *band=(GmlNearestOpaqueAlphaBand*)context;
  GmlRender *render=band->render;
  const GmlTpag *tpag=band->tpag;
  const uint32_t *source=band->source;
  const int *source_x=band->source_x;
  const int flip_x=band->flip_x,flip_y=band->flip_y;
  const int anchor_x=band->anchor_x,anchor_y=band->anchor_y;
  const int output_x0=band->output_x0,output_x1=band->output_x1;
  const int output_y0=band->output_y0;
  const double sample_y=band->sample_y,scale_y=band->scale_y;
  const double source_weight=band->source_weight;
  const double destination_weight=band->destination_weight;
  const unsigned source_alpha=band->source_alpha;
  const int preserve_alpha=band->preserve_alpha;
  const unsigned source_alpha_square=source_alpha*source_alpha+127u;
  const unsigned inverse_alpha=255u-source_alpha;
  (void)slot;
  for(int row=row_start;row<row_end;row++){
    int output_y=output_y0+row;
    int local_y=(int)((output_y+sample_y)/scale_y);
    if(local_y<0 || local_y>=tpag->sh) continue;
    int destination_y=flip_y?anchor_y-output_y:anchor_y+output_y;
    const uint32_t *source_row=source+(size_t)local_y*tpag->sw;
    uint32_t *destination=render->fb+(size_t)destination_y*render->fbw+
      (flip_x?anchor_x-output_x0:anchor_x+output_x0);
    int destination_step=flip_x?-1:1;
    for(int output_x=output_x0;output_x<output_x1;
        output_x++,destination+=destination_step){
      int local_x=source_x[output_x-output_x0];
      if(local_x<0 || local_x>=tpag->sw) continue;
      uint32_t source_pixel=source_row[local_x];
      uint32_t destination_pixel=*destination;
      int source_red=(source_pixel>>16)&255;
      int source_green=(source_pixel>>8)&255;
      int source_blue=source_pixel&255;
      int destination_red=(destination_pixel>>16)&255;
      int destination_green=(destination_pixel>>8)&255;
      int destination_blue=destination_pixel&255;
      int output_red=(int)(source_red*source_weight+
                           destination_red*destination_weight+0.5);
      int output_green=(int)(source_green*source_weight+
                             destination_green*destination_weight+0.5);
      int output_blue=(int)(source_blue*source_weight+
                            destination_blue*destination_weight+0.5);
      uint32_t output_alpha=UINT32_C(0xff000000);
      if(preserve_alpha){
        unsigned destination_alpha=destination_pixel>>24;
        output_alpha=((source_alpha_square+destination_alpha*inverse_alpha)/255u)<<24;
      }
      *destination=output_alpha|((uint32_t)output_red<<16)|
                   ((uint32_t)output_green<<8)|(uint32_t)output_blue;
    }
  }
}

static void blit_one_band_rows(void *context,int row_start,int row_end,int slot){
  GmlBlitOneBand *b=(GmlBlitOneBand*)context;
  GmlRender *r=b->r;
  GmlAtlas *a=b->a;
  GmlTpag *t=b->t;
  /* Locals, not b-> reads, inside the pixel loops: a framebuffer store may legally alias the
   * context's integer fields, and a compiler that cannot prove otherwise reloads them per pixel.
   * The neighbouring band callbacks hoist for the same reason. */
  const int *lxtab=b->lxtab;
  const int *wave_map=b->wave_map;
  const uint32_t *solid_blur_alpha=b->solid_blur_alpha;
  const struct GmlShaderPal *solid_blur=b->solid_blur;
  const int fastcase=b->fastcase;
  const int flipx=b->flipx,flipy=b->flipy;
  const int x0=b->x0,y0=b->y0;
  const int xx0=b->xx0,xx1=b->xx1,yy0=b->yy0;
  const int sx_max=b->sx_max;
  const int first_generation_edge_phase_x=b->first_generation_edge_phase_x;
  const int first_generation_edge_phase_y=b->first_generation_edge_phase_y;
  const int reciprocal_x=b->reciprocal_x,reciprocal_y=b->reciprocal_y;
  const double sample_x=b->sample_x,sample_y=b->sample_y;
  const double axs=b->axs,ays=b->ays;
  const double alpha=b->alpha;
  const int bR=b->bR,bG=b->bG,bB=b->bB;
  (void)slot;
  for(int row=row_start; row<row_end; row++){
    int yy=yy0+row;
    int py = flipy ? (y0-yy) : (y0+yy);
    int ly=(int)((yy+sample_y)/ays);
    int contiguous_y=fastcase && first_generation_edge_phase_y && ly==t->sh &&
                     t->sy+ly>=0 && t->sy+ly<a->h;
    if((ly<0||ly>=t->sh) && !contiguous_y) continue;
    int sy=t->sy+ly;
    /* Clamp the source row to atlas bounds rather than skipping the draw. */
    if(a->h>0){ if(sy<0) sy=0; else if(sy>=a->h) sy=a->h-1; }
    if(fastcase){
      const uint8_t *srow=a->px + ((size_t)sy*a->w + t->sx)*4;
      uint32_t *drow=&r->fb[(size_t)py*r->fbw];
      for(int xx=xx0; xx<xx1; xx++){
        int lx=lxtab[xx-xx0];
        int contiguous_x=first_generation_edge_phase_x && lx==t->sw &&
                         t->sx+lx>=0 && t->sx+lx<a->w;
        if((lx<0||lx>=t->sw) && !contiguous_x) continue;
        const uint8_t *sp=srow + (size_t)lx*4;
        int aa=sp[3]; if(!aa) continue;
        int px = flipx ? (x0-xx) : (x0+xx);
        uint32_t *dp=&drow[px];
        if(aa==255){ *dp=0xFF000000u|((uint32_t)sp[0]<<16)|((uint32_t)sp[1]<<8)|sp[2]; continue; }
        double sa=(aa/255.0)*alpha;
        uint32_t destination=*dp;
        int dr=(destination>>16)&0xFF, dg=(destination>>8)&0xFF, db=destination&0xFF;
        int family=gml_blend_family(r);
        int or_,og,ob;
        if(family==GML_BLEND_CLASSIC){
          or_=(int)(sp[0]*sa+0.5)+(int)(dr*(1-sa)+0.5);
          og=(int)(sp[1]*sa+0.5)+(int)(dg*(1-sa)+0.5);
          ob=(int)(sp[2]*sa+0.5)+(int)(db*(1-sa)+0.5);
        } else {
          double bias=family==GML_BLEND_STUDIO2?0.5:0.0;
          or_=(int)(sp[0]*sa+dr*(1-sa)+bias);
          og=(int)(sp[1]*sa+dg*(1-sa)+bias);
          ob=(int)(sp[2]*sa+db*(1-sa)+bias);
        }
        if(or_>255) or_=255; else if(or_<0) or_=0;
        if(og>255) og=255; else if(og<0) og=0;
        if(ob>255) ob=255; else if(ob<0) ob=0;
        unsigned source_alpha=(unsigned)lround((double)aa*alpha);
        *dp=gml_sprite_target_alpha(r,destination,source_alpha)|(or_<<16)|(og<<8)|ob;
      }
      continue;
    }
    for(int xx=xx0; xx<xx1; xx++){
      int px = flipx ? (x0-xx) : (x0+xx);
      int lx=lxtab? lxtab[xx-xx0] : (int)((xx+sample_x)/axs);
      if(lx<0||lx>=t->sw) continue;
      int sx=t->sx+lx;
      if(sx<0) sx=0; else if(sx>sx_max) sx=sx_max;      /* clamp, never sample past the texture */
      const uint8_t *sp=wave_map
        ? a->px+(size_t)wave_map[(size_t)ly*t->sw+lx]*4u
        : a->px+((size_t)sy*a->w+sx)*4u;
      if(shader_discards_alpha(r,sp[3]) &&
         !(r->blendmode==2 && !shader_alpha_test_active(r))) continue;
      uint32_t sampled=((uint32_t)sp[3]<<24)|((uint32_t)sp[0]<<16)|
                       ((uint32_t)sp[1]<<8)|(uint32_t)sp[2];
      if(solid_blur_alpha)
        sampled=(solid_blur_alpha[(size_t)ly*t->sw+lx]&0xFF000000u)|
                solid_blur->solid_blur_alpha_rgb;
      else if(mapped_texture_active(r))
        sampled=mapped_texture_pixel(r,sampled);
      int sample_a=(int)(sampled>>24);
      double sa=(sample_a/255.0)*alpha;
      /* (bm_one, bm_zero) writes the source fragment wherever the quad lands: the texel's own
       * colour and its own coverage, with the destination contributing nothing. A transparent texel
       * is therefore not a discarded fragment here - it replaces what was underneath with the
       * transparency it carries - and a partially covered one lands at the coverage it was authored
       * with rather than at the coverage a source-over pass would have left. */
      int replace=r->blendmode==6;
      if(sa<=0 && r->blendmode!=2 && !replace) continue;
      uint32_t *dp=&r->fb[(size_t)py*r->fbw+px];
      if(replace){
        unsigned source_alpha=(unsigned)lround((double)sample_a*alpha);
        *dp=(gml_render_target_preserves_alpha(r)?(uint32_t)source_alpha<<24:0xFF000000u)|
            (uint32_t)((((sampled>>16)&255)*bR/255)<<16)|
            (uint32_t)((((sampled>>8)&255)*bG/255)<<8)|
            (uint32_t)((sampled&255)*bB/255);
        continue;
      }
      int family=gml_blend_family(r);
      int tint_bias=family==GML_BLEND_STUDIO2?127:0;
      /* A solid-blur fragment's colour is its uniform alone: v_vColour never reaches it. */
      int sr=solid_blur_alpha?(int)((sampled>>16)&255)
            :(int)((((sampled>>16)&255)*bR+tint_bias)/255);
      int sg=solid_blur_alpha?(int)((sampled>>8)&255)
            :(int)((((sampled>>8)&255)*bG+tint_bias)/255);
      int sb=solid_blur_alpha?(int)(sampled&255)
            :(int)(((sampled&255)*bB+tint_bias)/255);
      if(!r->alphablend){ *dp=0xFF000000u|(sr<<16)|(sg<<8)|sb; continue; }
      uint32_t destination=*dp;
      int dr=(destination>>16)&0xFF, dg=(destination>>8)&0xFF, db=destination&0xFF;
      if(r->blendmode==1){   /* bm_add: dst += src*srcAlpha (glow) */
        int ar=dr+(int)(sr*sa); if(ar>255)ar=255; int ag=dg+(int)(sg*sa); if(ag>255)ag=255;
        int ab=db+(int)(sb*sa); if(ab>255)ab=255; *dp=0xFF000000u|(ar<<16)|(ag<<8)|ab; continue; }
      if(r->blendmode==2){   /* (bm_zero,bm_inv_src_colour), including inverse source alpha. */
        unsigned ar=gml_blend_inv_source_u8((unsigned)dr,(unsigned)sr);
        unsigned ag=gml_blend_inv_source_u8((unsigned)dg,(unsigned)sg);
        unsigned ab=gml_blend_inv_source_u8((unsigned)db,(unsigned)sb);
        uint32_t coverage=gml_sprite_target_inverse_alpha(r,*dp,(double)sample_a*alpha);
        *dp=coverage|(ar<<16)|(ag<<8)|ab; continue; }
      if(r->blendmode==4){
        unsigned source_alpha=(unsigned)lround((double)sample_a*alpha);
        *dp=color_write_merge(r,*dp,
          blend_max_preset_pixel(r,*dp,sr,sg,sb,source_alpha));
        continue;
      }
      /* clamp each channel to [0,255]: a blend>255 or a (legitimately clamped) alpha can still push
       * sr*sa over 255, and packing an out-of-range byte would corrupt the neighbouring channel. */
      int or_,og,ob;
      if(family==GML_BLEND_CLASSIC){
        if(!reciprocal_x || !reciprocal_y){
          or_=(int)(sr*sa+0.5)+(int)(dr*(1-sa)+0.5);
          og=(int)(sg*sa+0.5)+(int)(dg*(1-sa)+0.5);
          ob=(int)(sb*sa+0.5)+(int)(db*(1-sa)+0.5);
        } else {
          or_=(int)(sr*sa+dr*(1-sa)+0.5);
          og=(int)(sg*sa+dg*(1-sa)+0.5);
          ob=(int)(sb*sa+db*(1-sa)+0.5);
        }
      } else {
        double bias=(family==GML_BLEND_STUDIO2 ||
                     gml_render_target_is_first_generation_application_surface(r))?0.5:0.0;
        or_=(int)(sr*sa+dr*(1-sa)+bias);
        og=(int)(sg*sa+dg*(1-sa)+bias);
        ob=(int)(sb*sa+db*(1-sa)+bias);
      }
      if(or_>255) or_=255; else if(or_<0) or_=0;
      if(og>255) og=255; else if(og<0) og=0;
      if(ob>255) ob=255; else if(ob<0) ob=0;
      unsigned source_alpha=(unsigned)lround((double)sample_a*alpha);
      *dp=gml_sprite_target_alpha(r,destination,source_alpha)|(or_<<16)|(og<<8)|ob;
    }
  }
}

/* Alpha-bound packing retains only covered texels plus authored dimensions and offsets. Replacement blending writes the complete authored quad, including cropped transparent margins; other supported blend presets treat those margins as identity. Axis-aligned texture-page paths share this margin operation. Rotated margins require a separate geometric raster path and are unchanged here. */
static void gml_render_write_authored_margin(GmlRender *r, const GmlTpag *t,
                                             double dx, double dy, double axs, double ays){
  if(!r || !t || !r->fb) return;
  if(r->blendmode!=6 || !r->alphablend) return;
  if(!(t->tx>0 || t->ty>0 || t->sw<t->bw || t->sh<t->bh)) return;   /* nothing was cropped away */
  int bx0=(int)floor(dx-(double)t->tx*axs+0.5);
  int by0=(int)floor(dy-(double)t->ty*ays+0.5);
  int bx1=bx0+(int)lround((double)t->bw*axs);
  int by1=by0+(int)lround((double)t->bh*ays);
  if(bx0<0) bx0=0;
  if(by0<0) by0=0;
  if(bx1>r->fbw) bx1=r->fbw;
  if(by1>r->fbh) by1=r->fbh;
  if(bx1<=bx0 || by1<=by0) return;
  /* The empty source fragment: no coverage where the target keeps alpha, and the black it carries
   * where the target has none, which is what the pair writes on a display. */
  uint32_t empty=gml_render_target_preserves_alpha(r)?0x00000000u:0xFF000000u;
  for(int yy=by0; yy<by1; yy++){
    uint32_t *row=r->fb+(size_t)yy*r->fbw;
    for(int xx=bx0; xx<bx1; xx++) row[xx]=empty;
  }
  r->fb_opaque_known=0;
  r->fb_all_opaque=0;
  r->fb_all_transparent=0;
}
static void blit_one(GmlRender *r, GmlTpag *t, double dx, double dy, double xs, double ys,
                     uint32_t blend, double alpha){
  if(!r || !t || !r->fb || r->fbw<=0 || r->fbh<=0) return;
  if(alpha>1) alpha=1; else if(alpha<0) alpha=0;   /* GM clamps draw alpha to [0,1] */
  if(alpha<=0) return;
  double axs=fabs(xs), ays=fabs(ys); if(axs<=0||ays<=0) return;
  int bR=blend&0xFF, bG=(blend>>8)&0xFF, bB=(blend>>16)&0xFF;  /* GM blend = BBGGRR */
  /* Sprite quads normally snap fractional positions by rounding half up. On modern
   * world targets a half-integer camera offset biases exact ties toward the preceding
   * output pixel; other targets retain ordinary half-up rounding. */
  int modern_world_target=r->win &&
    anygm_policy_has_modern_layer_semantics(r->win) &&
    !r->gui_pass_active && r->target_sp==0 && r->target_id<0;
  double x_camera_half=fabs(r->cam_x-nearbyint(r->cam_x));
  double y_camera_half=fabs(r->cam_y-nearbyint(r->cam_y));
  double x_tie=(modern_world_target && fabs(x_camera_half-0.5)<1e-9)?-1e-9:0.0;
  double y_tie=(modern_world_target && fabs(y_camera_half-0.5)<1e-9)?-1e-9:0.0;
  int x0=(int)floor(dx+0.5+x_tie), y0=(int)floor(dy+0.5+y_tie);
  int w=(int)lround(t->sw*axs), h=(int)lround(t->sh*ays);
  /* First-generation application surfaces project each authored edge separately.  Preserve that
   * accumulated fractional coverage instead of rounding every independent quad to the same size. */
  if(gml_render_target_is_first_generation_application_surface(r)){
    if(xs>0.0 && fabs(axs-nearbyint(axs))>1e-9)
      w=(int)floor(dx+t->sw*axs+0.5)-x0;
    if(ys>0.0 && fabs(ays-nearbyint(ays))>1e-9)
      h=(int)floor(dy+t->sh*ays+0.5)-y0;
  }
  if(w<=0 || h<=0) return;
  /* Report blits whose destination rectangle overlaps an explicitly selected
   * area. This is diagnostic only and does not alter rasterization. */
  {
    const char *want=render_setting(r,"GML_LOG_DRAW_RECT");
    if(want){
      int rx0=0,ry0=0,rx1=0,ry1=0;
      if(sscanf(want,"%d,%d,%d,%d",&rx0,&ry0,&rx1,&ry1)==4 &&
         x0<=rx1 && x0+w>=rx0 && y0<=ry1 && y0+h>=ry0)
        anygm_host_logf(r->win?r->win->host:NULL,ANYGM_LOG_DEBUG,
          "[drawrect] f%ld tpag=%d atlas=%d src=%d,%d,%dx%d dst=%d,%d,%dx%d blend=%06x alpha=%.4f\n",
          r->frame,rprof_tpag_id(r,t),t->atlas,t->sx,t->sy,t->sw,t->sh,
          x0,y0,w,h,(unsigned)blend,alpha);
      /* Count coloured source pixels and destination pixels before this blit.
       * A single corner sample need not represent either rectangle. */
      {
        const GmlAtlas *a_=(t->atlas>=0 && t->atlas<r->n_atlas)?&r->atlas[t->atlas]:NULL;
        const uint8_t *ap_=a_?a_->px:NULL;
        int src_lit_=0, src_seen_=0;
        if(ap_) for(int yy_=0; yy_<t->sh; yy_++) for(int xx_=0; xx_<t->sw; xx_++){
          int sx_=t->sx+xx_, sy_=t->sy+yy_;
          if(sx_<0||sy_<0||sx_>=a_->w||sy_>=a_->h) continue;
          const uint8_t *q_=ap_+(((size_t)sy_*(size_t)a_->w)+(size_t)sx_)*4u;
          src_seen_++;
          if(q_[3] && (q_[0]|q_[1]|q_[2])) src_lit_++;
        }
        int dst_lit_=0, dst_seen_=0;
        if(r->fb) for(int yy_=0; yy_<h; yy_++) for(int xx_=0; xx_<w; xx_++){
          int dx_=x0+xx_, dy_=y0+yy_;
          if(dx_<0||dy_<0||dx_>=r->fbw||dy_>=r->fbh) continue;
          dst_seen_++;
          if(r->fb[(size_t)dy_*(size_t)r->fbw+(size_t)dx_]&0x00FFFFFFu) dst_lit_++;
        }
        anygm_host_logf(r->win?r->win->host:NULL,ANYGM_LOG_DEBUG,
          "[drawrect]   src_lit=%d/%d dst_lit_before=%d/%d target=%d fb=%dx%d atlas=%s\n",
          src_lit_,src_seen_,dst_lit_,dst_seen_,r->target_id,r->fbw,r->fbh,ap_?"yes":"no");
      }
    }
  }
  int flipx=(xs<0), flipy=(ys<0);
  /* A negatively-scaled GPU quad is half-open at its anchor edge: with a
   * two-pixel sprite anchored at x=20 it covers destination pixels 18 and
   * 19, not 19 and 20.  This is shared by classic and Studio semantics;
   * keeping the adjustment classic-only displaced every mirrored Studio
   * sprite by one logical pixel (most visibly vertically mirrored arenas). */
  if(flipx) x0--;
  if(flipy) y0--;
  int xx0=0, xx1=w, yy0=0, yy1=h;
  if(!flipx){
    if(x0<0) xx0=-x0;
    if(x0+w>r->fbw) xx1=r->fbw-x0;
  } else {
    int s=x0-(r->fbw-1), e=x0+1;
    if(s>xx0) xx0=s;
    if(e<xx1) xx1=e;
  }
  if(!flipy){
    if(y0<0) yy0=-y0;
    if(y0+h>r->fbh) yy1=r->fbh-y0;
  } else {
    int s=y0-(r->fbh-1), e=y0+1;
    if(s>yy0) yy0=s;
    if(e<yy1) yy1=e;
  }
  if(xx1<=xx0 || yy1<=yy0) return;
  if(t->atlas<0 || t->atlas>=r->n_atlas) return;
  GmlAtlas *a=&r->atlas[t->atlas]; if(!atlas_pixels(r,t->atlas)) return;
  const struct GmlShaderPal *wave=radial_wave_active(r);
  const struct GmlShaderPal *uvwave=uv_wave_active(r);
  const struct GmlShaderPal *solid_mask=solid_alpha_mask_active(r);
  const struct GmlShaderPal *solid_blur=solid_blur_alpha_active(r);
  uint32_t *solid_blur_alpha=solid_blur
    ? tpag_solid_blur_alpha_cache(r,t,a,solid_blur) : NULL;
  int mapped_shader=mapped_texture_active(r) || solid_blur_alpha ||
                    shader_alpha_test_requires_filter(r) || wave!=NULL || uvwave!=NULL;
  if(!t->alpha_scanned) (void)tpag_alpha_bounds(r,t,a,NULL,NULL,NULL,NULL);
  gml_render_maybe_prepare_draw(r);
  gml_render_write_authored_margin(r,t,dx,dy,axs,ays);
  /* SRCALPHA/INVSRCALPHA also blends the destination alpha channel. A partially covered texel
   * therefore makes an opaque render target non-opaque even when the draw alpha is one. Keep the
   * coverage certificate honest so a later surface composite does not take an opaque-copy path.
   * First-generation application surfaces present opaque independently of texel coverage. */
  if((alpha<1.0 || r->blendmode==2 ||
      (t->alpha_partial && !gml_render_target_is_first_generation_application_surface(r)) ||
      (r->interp && t->alpha_max>0) || mapped_shader) && r->alphablend)
    gml_sprite_target_may_change_alpha(r);
  unsigned long long vispix=(unsigned long long)(xx1-xx0)*(unsigned long long)(yy1-yy0);
  /* Hardware filtering samples the four neighbouring texels at the destination pixel centre.
   * Preserve the quad's fractional origin in the inverse map: snapping before sampling shifts a
   * heavily minified texture by several source texels and is visible in presentation overlays. */
  /* At unit scale, an integer-aligned Studio white quad maps every destination pixel centre
   * exactly onto one source texel centre. With full draw alpha and ordinary source-over blending,
   * the cached exact-copy kernel also preserves the filtered path's UNORM result. Keep tinted,
   * draw-alpha and special-blend cases on the general filtered path because their quantization
   * can differ even when the selected source texel is the same. */
  int studio_texture_filtering=
    r->win && (anygm_policy_uses_first_generation_studio(r->win) ||
               anygm_policy_has_modern_layer_semantics(r->win));
  int exact_studio_white_copy=
    studio_texture_filtering && !r->font_sdf_active &&
    !flipx && !flipy &&
    fabs(axs-1.0)<0.001 && fabs(ays-1.0)<0.001 &&
    fabs(dx-nearbyint(dx))<1e-9 && fabs(dy-nearbyint(dy))<1e-9 &&
    alpha>=1.0 && (blend&0xFFFFFFu)==0xFFFFFFu &&
    r->blendmode==0 && !mapped_shader;
  if(r->interp && studio_texture_filtering &&
     !wave && !uvwave && !exact_studio_white_copy){
    int columns=xx1-xx0;
    int *source_a=columns>0?malloc((size_t)columns*sizeof(*source_a)):NULL;
    double *fraction_x=columns>0?malloc((size_t)columns*sizeof(*fraction_x)):NULL;
    if(source_a && fraction_x) for(int column=0;column<columns;column++){
      int xx=xx0+column;
      int px=flipx?x0-xx:x0+xx;
      double source_x=(flipx
        ? dx-((double)px+0.5)
        : ((double)px+0.5)-dx)/axs-0.5;
      source_a[column]=(int)floor(source_x);
      fraction_x[column]=source_x-source_a[column];
    }
    const struct GmlShaderPal *interp_solid_mask=
      r->blendmode==0?solid_mask:NULL;
    int logical_margin=
      t->tx>0 || t->ty>0 || t->tx+t->sw<t->bw || t->ty+t->sh<t->bh;
    uint32_t solid_rgb=interp_solid_mask
      ? interp_solid_mask->solid_alpha_mask_rgb
      : (solid_blur_alpha?solid_blur->solid_blur_alpha_rgb:0);
    /* Recognized solid fragments never read v_vColour; the uniform is the whole tint. */
    int solid_red=(solid_rgb>>16)&255;
    int solid_green=(solid_rgb>>8)&255;
    int solid_blue=solid_rgb&255;
    GmlInterpBlitBand band={
      r,a,t,interp_solid_mask,
      interp_solid_mask&&!logical_margin?tpag_argb_cache(r,t,a):NULL,
      r->blendmode==0&&solid_blur_alpha?solid_blur:NULL,
      r->blendmode==0?solid_blur_alpha:NULL,
      source_a,fraction_x,
      flipx,flipy,x0,y0,yy0,xx0,xx1,
      logical_margin,
      bR,bG,bB,solid_red,solid_green,solid_blue,
      dx,dy,axs,ays,alpha,blend
    };
    if(tpag_interp_draw_cache_try(r,t,&band,yy1-yy0)){
      free(source_a);
      free(fraction_x);
      return;
    }
    if(vispix>=262144ull) gml_run_row_bands(r,yy1-yy0,interp_blit_band_rows,&band);
    else interp_blit_band_rows(&band,0,yy1-yy0,0);
    free(source_a);
    free(fraction_x);
    return;
  }
  if(!flipx && !flipy && fabs(axs-1.0)<0.001 && fabs(ays-1.0)<0.001 &&
     alpha>=1.0 && (blend & 0xFFFFFF)==0xFFFFFF && r->blendmode==0 && !mapped_shader){   /* fast opaque copy — never in add mode */
    int cx0=x0<0?0:x0, cy0=y0<0?0:y0;
    int cx1=x0+t->sw; if(cx1>r->fbw) cx1=r->fbw;
    int cy1=y0+t->sh; if(cy1>r->fbh) cy1=r->fbh;
    int cw=cx1-cx0, ch=cy1-cy0;
    if(cw<=0 || ch<=0) return;
    if(!blit_tpag_scale1_white_exact(r,t,a,x0,y0,cx0-x0,cx1-x0,cy0-y0,cy1-y0)){
      int sx0=t->sx + (cx0-x0), sy0=t->sy + (cy0-y0);
      for(int yy=0; yy<ch; yy++){
        const uint8_t *sp=a->px+((size_t)(sy0+yy)*a->w+sx0)*4;
        uint32_t *dp=r->fb+(size_t)(cy0+yy)*r->fbw+cx0;
        for(int xx=0; xx<cw; xx++, sp+=4){
          int aa=sp[3];
          if(aa<=0) continue;
          int sr=sp[0], sg=sp[1], sb=sp[2];
          if(!r->alphablend || aa>=255) dp[xx]=0xFF000000u|(sr<<16)|(sg<<8)|sb;
          else {
            uint32_t dv=dp[xx];
            int ia=255-aa;
            int dr=(dv>>16)&0xFF, dg=(dv>>8)&0xFF, db=dv&0xFF;
            int family=gml_blend_family(r);
            if(family==GML_BLEND_CLASSIC){
              /* Fixed-function GM8 blending rounds the source and destination
               * products independently before adding them.  Rounding only the
               * combined numerator loses a channel at nearly-opaque texels. */
              int rr=(sr*aa+127)/255 + (dr*ia+127)/255;
              int rg=(sg*aa+127)/255 + (dg*ia+127)/255;
              int rb=(sb*aa+127)/255 + (db*ia+127)/255;
              if(rr>255) rr=255;
              if(rg>255) rg=255;
              if(rb>255) rb=255;
              dp[xx]=gml_sprite_target_alpha(r,dv,(unsigned)aa)|
                     ((uint32_t)rr<<16)|((uint32_t)rg<<8)|(uint32_t)rb;
            } else if(family==GML_BLEND_STUDIO2) {
              dp[xx]=gml_sprite_target_alpha(r,dv,(unsigned)aa)|
                     (((sr*aa+dr*ia+127)/255)<<16)|
                     (((sg*aa+dg*ia+127)/255)<<8)|((sb*aa+db*ia+127)/255);
            } else {
              dp[xx]=gml_sprite_target_alpha(r,dv,(unsigned)aa)|
                     (((sr*aa+dr*ia)/255)<<16)|
                     (((sg*aa+dg*ia)/255)<<8)|((sb*aa+db*ia)/255);
            }
          }
        }
      }
    }
    return;
  }
  if(!flipx && !flipy && fabs(axs-1.0)<0.001 && fabs(ays-1.0)<0.001 &&
     alpha<1.0 && (blend & 0xFFFFFF)==0xFFFFFF && r->blendmode==0 && !mapped_shader){
    int cx0=x0<0?0:x0, cy0=y0<0?0:y0;
    int cx1=x0+t->sw; if(cx1>r->fbw) cx1=r->fbw;
    int cy1=y0+t->sh; if(cy1>r->fbh) cy1=r->fbh;
    int cw=cx1-cx0, ch=cy1-cy0;
    if(cw<=0 || ch<=0) return;
    if(blit_tpag_scale1_white_draw_alpha(
         r,t,a,x0,y0,cx0-x0,cx1-x0,cy0-y0,cy1-y0,alpha)) return;
  }
  if((flipx || flipy) && fabs(axs-1.0)<0.001 && fabs(ays-1.0)<0.001 &&
     alpha>=1.0 && (blend & 0xFFFFFF)==0xFFFFFF && r->blendmode==0 && !mapped_shader){
    if(blit_tpag_scale1_white_exact_flipped(
         r,t,a,x0,y0,xx0,xx1,yy0,yy1,flipx,flipy)) return;
  }
  /* per-column source-x table: hoists the per-pixel division out of the row loop
   * (same expression, so the sampled columns are bit-identical) */
  int lxbuf[2048];
  int span=xx1-xx0;
  int *lxtab=(span<=(int)(sizeof lxbuf/sizeof *lxbuf))? lxbuf : malloc((size_t)span*sizeof(int));
  /* Raster coverage snapping does not snap the quad's texture phase. Point sampling still
   * measures destination-pixel centres from the fractional leading edge. Classic keeps its
   * narrower reciprocal-scale rule; Studio retains that phase for scaled point sampling. */
  double inv_x=1.0/axs, inv_y=1.0/ays;
  int reciprocal_x=fabs(inv_x-nearbyint(inv_x))<1e-9;
  int reciprocal_y=fabs(inv_y-nearbyint(inv_y))<1e-9;
  int studio_point_phase=r->win && anygm_policy_has_modern_layer_semantics(r->win);
  int first_generation_edge_phase_x=
    gml_render_target_is_first_generation_application_surface(r) && !flipx && !reciprocal_x;
  int first_generation_edge_phase_y=
    gml_render_target_is_first_generation_application_surface(r) && !flipy && !reciprocal_y;
  double sample_x=!flipx && (studio_point_phase || (r->classic&&!reciprocal_x))
    ? x0+0.5-dx : 0.5;
  double sample_y=!flipy && (studio_point_phase || (r->classic&&!reciprocal_y))
    ? y0+0.5-dy : 0.5;
  /* First-generation application surfaces keep the same authored-space sampling phase across
   * independently submitted tiles; resetting to half a texel selects the neighbouring edge row.
   * Coverage rounding can put the final pixel just beyond the selected subrectangle.  The hardware
   * samples the contiguous atlas texel there; atlas padding is what normally supplies the edge. */
  if(first_generation_edge_phase_x) sample_x=x0+1.0-dx-1e-9;
  if(first_generation_edge_phase_y) sample_y=y0+1.0-dy-1e-9;
  if(lxtab) for(int xx=xx0;xx<xx1;xx++)
    lxtab[xx-xx0]=(int)((xx+sample_x)/axs);
  /* The displacement is evaluated in texture coordinates, not output coordinates. Scaled pixel
   * art repeats each source texel many times, so precompute one warped atlas index per logical
   * source pixel instead of performing hypot/sin for every enlarged destination pixel. */
  int *wave_map=NULL;
  if((wave || uvwave) && t->sw>0 && t->sh>0 &&
     (size_t)t->sw<=SIZE_MAX/(size_t)t->sh/sizeof(int)){
    size_t count=(size_t)t->sw*(size_t)t->sh;
    wave_map=malloc(count*sizeof(*wave_map));
    if(wave_map) for(int local_y=0;local_y<t->sh;local_y++){
      double position_y=dy+r->cam_y+(flipy?-(local_y+0.5)*ays:(local_y+0.5)*ays);
      for(int local_x=0;local_x<t->sw;local_x++)
        wave_map[(size_t)local_y*t->sw+local_x]=wave
          ? radial_wave_sample_index(wave,a->w,a->h,t->sx+local_x,t->sy+local_y)
          : uv_wave_sample_index(uvwave,a->w,a->h,t->sx+local_x,t->sy+local_y,position_y);
    }
  }
  if(lxtab && vispix>=16384ull && !r->interp && r->active_shader<0 && !mapped_shader &&
     r->alphablend && r->blendmode==0 && r->blend_equation==1 &&
     r->blend_equation_alpha==1 && r->color_write_mask==0x0F &&
     alpha<1.0 && (blend&0xFFFFFFu)==0xFFFFFFu &&
     gml_blend_family(r)==GML_BLEND_STUDIO2){
    const uint32_t *source=tpag_fully_opaque_argb(r,t,a);
    if(source){
      GmlNearestOpaqueAlphaBand band={
        .render=r,.tpag=t,.source=source,.source_x=lxtab,
        .flip_x=flipx,.flip_y=flipy,
        .anchor_x=x0,.anchor_y=y0,
        .output_x0=xx0,.output_x1=xx1,.output_y0=yy0,
        .sample_y=sample_y,.scale_y=ays,
        .source_weight=alpha,.destination_weight=1.0-alpha,
        .source_alpha=(unsigned)lround(255.0*alpha),
        .preserve_alpha=gml_render_target_preserves_alpha(r)
      };
      if(vispix>=262144ull)
        gml_run_row_bands(r,yy1-yy0,nearest_opaque_alpha_band_rows,&band);
      else
        nearest_opaque_alpha_band_rows(&band,0,yy1-yy0,0);
      free(wave_map);
      if(lxtab!=lxbuf) free(lxtab);
      return;
    }
  }
  if(lxtab && !flipx && !flipy && solid_mask && !solid_blur_alpha &&
     alpha>=1.0 && r->alphablend && r->blendmode==0 &&
     !gml_render_target_preserves_alpha(r) &&
     !r->classic && r->win && anygm_policy_has_modern_layer_semantics(r->win)){
    uint32_t *source=tpag_argb_cache(r,t,a);
    if(source){
      /* The recognized fragment is gl_FragColor=vec4(uniform.rgb, alpha), and the structural
       * parser rejects any use of v_vColour. The uniform is therefore the whole tint. */
      uint32_t mask_rgb=solid_mask->solid_alpha_mask_rgb;
      int red=(mask_rgb>>16)&255u;
      int green=(mask_rgb>>8)&255u;
      int blue=mask_rgb&255u;
      GmlNearestSolidMaskBand band={
        .render=r,.tpag=t,.source=source,.source_x=lxtab,
        .destination_x=x0,.destination_y=y0,
        .source_x0=xx0,.source_x1=xx1,.source_y0=yy0,
        .abs_yscale=ays,.sample_y=sample_y,
        .solid_rgb=((uint32_t)red<<16)|((uint32_t)green<<8)|(uint32_t)blue
      };
      for(unsigned raw=0;raw<256;raw++){
        int mapped=(int)raw;
        if(shader_discards_alpha(r,raw) ||
           (solid_mask->solid_alpha_mask_inclusive
             ? mapped<=solid_mask->solid_alpha_mask_cutoff_step
             : mapped< solid_mask->solid_alpha_mask_cutoff_step)) mapped=0;
        band.mapped_alpha[raw]=(uint8_t)mapped;
      }
      if(vispix>=262144ull)
        gml_run_row_bands(r,yy1-yy0,nearest_solid_mask_band_rows,&band);
      else
        nearest_solid_mask_band_rows(&band,0,yy1-yy0,0);
      free(wave_map);
      if(lxtab!=lxbuf) free(lxtab);
      return;
    }
  }
  /* A magnified nearest-neighbour texel covers a contiguous destination run. Additive blending
   * applies the same saturated RGB increment to every pixel in that run, so calculate the shader-
   * free sample once and use packed byte saturation for the repeated destination pixels. */
  if(lxtab && !flipx && !flipy && !mapped_shader && r->alphablend &&
     r->blendmode==1 && !gml_render_target_preserves_alpha(r)){
    int tint_bias=gml_blend_family(r)==GML_BLEND_STUDIO2?127:0;
    for(int yy=yy0;yy<yy1;yy++){
      int py=y0+yy;
      int ly=(int)((yy+sample_y)/ays);
      if(ly<0 || ly>=t->sh) continue;
      const uint8_t *source_row=a->px+((size_t)(t->sy+ly)*a->w+t->sx)*4u;
      uint32_t *destination_row=r->fb+(size_t)py*r->fbw+x0;
      for(int xx=xx0;xx<xx1;){
        int lx=lxtab[xx-xx0];
        int end=xx+1;
        while(end<xx1 && lxtab[end-xx0]==lx) end++;
        if(lx>=0 && lx<t->sw){
          const uint8_t *sample=source_row+(size_t)lx*4u;
          if(sample[3]){
            double source_alpha=(sample[3]/255.0)*alpha;
            int red=(sample[0]*bR+tint_bias)/255;
            int green=(sample[1]*bG+tint_bias)/255;
            int blue=(sample[2]*bB+tint_bias)/255;
            uint32_t increment=
              ((uint32_t)(int)(red*source_alpha)<<16)|
              ((uint32_t)(int)(green*source_alpha)<<8)|
              (uint32_t)(int)(blue*source_alpha);
            blend_additive_rgb_run(destination_row+xx,end-xx,increment);
          }
        }
        xx=end;
      }
    }
    free(wave_map);
    if(lxtab!=lxbuf) free(lxtab);
    return;
  }
  /* dominant case (plain scaled sprite/background: white blend, full alpha, normal mode):
   * opaque pixels are a straight store and transparent ones a skip — identical output to the
   * generic math below ((int)(s*1.0+d*0.0)==s), only edge pixels take the double blend. */
  int fastcase = lxtab && !mapped_shader && (blend&0xFFFFFF)==0xFFFFFF && alpha>=1.0 && r->blendmode==0 && r->alphablend;
  /* Record which raster branch is selected when rectangle diagnostics overlap. */
  {
    const char *want_=render_setting(r,"GML_LOG_DRAW_RECT");
    int rx0_=0,ry0_=0,rx1_=0,ry1_=0;
    if(want_ && sscanf(want_,"%d,%d,%d,%d",&rx0_,&ry0_,&rx1_,&ry1_)==4 &&
       x0<=rx1_ && x0+w>=rx0_ && y0<=ry1_ && y0+h>=ry0_)
      anygm_host_logf(r->win?r->win->host:NULL,ANYGM_LOG_DEBUG,
        "[drawrect]   branch fastcase=%d lxtab=%d mapped_shader=%d blend_white=%d alpha=%.3f bm=%d ab=%d target_sp=%d\n",
        fastcase,lxtab?1:0,mapped_shader?1:0,((blend&0xFFFFFF)==0xFFFFFF)?1:0,alpha,r->blendmode,r->alphablend,r->target_sp);
  }
  {
    /* The band body is the loop this function always ran; see blit_one_band_rows. The clamp
     * comment that lived on the loop's source-row computation moved with it. */
    GmlBlitOneBand band={
      r,a,t,lxtab,wave_map,
      solid_blur_alpha,solid_blur,
      fastcase,flipx,flipy,x0,y0,xx0,xx1,yy0,
      a->w>0?a->w-1:0,
      first_generation_edge_phase_x,first_generation_edge_phase_y,
      reciprocal_x,reciprocal_y,
      sample_x,sample_y,axs,ays,alpha,bR,bG,bB
    };
    if(vispix>=262144ull) gml_run_row_bands(r,yy1-yy0,blit_one_band_rows,&band);
    else blit_one_band_rows(&band,0,yy1-yy0,0);
  }
  free(wave_map);
  if(lxtab && lxtab!=lxbuf) free(lxtab);
  /* Pair the earlier destination count with one taken after this blit. */
  {
    const char *want_=render_setting(r,"GML_LOG_DRAW_RECT");
    int rx0_=0,ry0_=0,rx1_=0,ry1_=0;
    if(want_ && sscanf(want_,"%d,%d,%d,%d",&rx0_,&ry0_,&rx1_,&ry1_)==4 &&
       x0<=rx1_ && x0+w>=rx0_ && y0<=ry1_ && y0+h>=ry0_ && r->fb){
      int after_=0, seen_=0;
      for(int yy_=0; yy_<h; yy_++) for(int xx_=0; xx_<w; xx_++){
        int dx_=x0+xx_, dy_=y0+yy_;
        if(dx_<0||dy_<0||dx_>=r->fbw||dy_>=r->fbh) continue;
        seen_++;
        if(r->fb[(size_t)dy_*(size_t)r->fbw+(size_t)dx_]&0x00FFFFFFu) after_++;
      }
      anygm_host_logf(r->win?r->win->host:NULL,ANYGM_LOG_DEBUG,
        "[drawrect]   dst_lit_after=%d/%d\n",after_,seen_);
    }
  }
}
static void blit_phase_plane(GmlRender *r, uint32_t *plane, GmlTpag *t,
                             double dx, double dy, double xs, double ys,
                             uint32_t blend, double alpha){
  uint32_t *saved_fb=r->fb;
  int saved_known=r->fb_opaque_known;
  int saved_opaque=r->fb_all_opaque;
  int saved_transparent=r->fb_all_transparent;
  r->fb=plane;
  r->fb_opaque_known=0;
  r->fb_all_opaque=0;
  r->fb_all_transparent=0;
  blit_one(r,t,dx,dy,xs,ys,blend,alpha);
  r->fb=saved_fb;
  r->fb_opaque_known=saved_known;
  r->fb_all_opaque=saved_opaque;
  r->fb_all_transparent=saved_transparent;
}
static uint32_t *interp_phase_pixels(GmlRender *r, GmlTpag *t, GmlAtlas *atlas,
                                     int phase_x, int phase_y,
                                     int projected_x, int projected_y,
                                     int dest_x0, int dest_y0,
                                     double edge_x0, double edge_x1,
                                     double edge_y0, double edge_y1){
  enum { INTERP_SUBRECT_MAX_ENTRIES=2048, INTERP_SUBRECT_MAX_BYTES=16*1024*1024 };
  if(!r || !t) return NULL;
  int slot=phase_x?(phase_y?2:0):1;
  int active_projected_x=phase_x && projected_x;
  int active_projected_y=phase_y && projected_y;
  int active_projected=active_projected_x || active_projected_y;
  size_t count=(size_t)t->sw*(size_t)t->sh;
  if(count==0 || count>SIZE_MAX/sizeof(uint32_t)) return NULL;
  size_t bytes=count*sizeof(uint32_t);
  uint32_t **cache_slot=NULL;
  int canonical=rprof_tpag_id(r,t)>=0;
  int direct_cache=canonical && !projected_x && !projected_y;
  if(direct_cache){
    cache_slot=&t->interp_phase_cache[slot];
  } else {
    if(bytes>INTERP_SUBRECT_MAX_BYTES) return NULL;
    GmlInterpSubrectCache *entry=NULL;
    for(int i=0;i<r->interp_subrect_count;i++){
      GmlInterpSubrectCache *candidate=&r->interp_subrect_cache[i];
      if(candidate->atlas==t->atlas && candidate->sx==t->sx && candidate->sy==t->sy &&
         candidate->sw==t->sw && candidate->sh==t->sh &&
         candidate->projected_x==projected_x && candidate->projected_y==projected_y &&
         candidate->dest_x0==dest_x0 && candidate->dest_y0==dest_y0 &&
         candidate->edge_x0==edge_x0 && candidate->edge_x1==edge_x1 &&
         candidate->edge_y0==edge_y0 && candidate->edge_y1==edge_y1){
        entry=candidate; break;
      }
    }
    if(!entry){
      if(r->interp_subrect_bytes>INTERP_SUBRECT_MAX_BYTES-bytes) return NULL;
      if(r->interp_subrect_count>=INTERP_SUBRECT_MAX_ENTRIES) return NULL;
      if(r->interp_subrect_count>=r->interp_subrect_capacity){
        int capacity=r->interp_subrect_capacity?r->interp_subrect_capacity*2:32;
        if(capacity>INTERP_SUBRECT_MAX_ENTRIES) capacity=INTERP_SUBRECT_MAX_ENTRIES;
        GmlInterpSubrectCache *grown=realloc(r->interp_subrect_cache,
                                             (size_t)capacity*sizeof(*grown));
        if(!grown) return NULL;
        r->interp_subrect_cache=grown;
        r->interp_subrect_capacity=capacity;
      }
      entry=&r->interp_subrect_cache[r->interp_subrect_count++];
      memset(entry,0,sizeof(*entry));
      entry->atlas=t->atlas; entry->sx=t->sx; entry->sy=t->sy;
      entry->sw=t->sw; entry->sh=t->sh;
      entry->projected_x=projected_x; entry->projected_y=projected_y;
      entry->dest_x0=dest_x0; entry->dest_y0=dest_y0;
      entry->edge_x0=edge_x0; entry->edge_x1=edge_x1;
      entry->edge_y0=edge_y0; entry->edge_y1=edge_y1;
    }
    cache_slot=&entry->phase[slot];
  }
  if(*cache_slot) return *cache_slot;
  if(!direct_cache &&
     r->interp_subrect_bytes>INTERP_SUBRECT_MAX_BYTES-bytes) return NULL;
  uint32_t *cache=malloc(count*sizeof(uint32_t));
  if(!cache) return NULL;
  int taps=(phase_x?2:1)*(phase_y?2:1);
  for(int y=0;y<t->sh;y++){
    int ya=y,yb=y; double fy=0.0;
    if(phase_y){
      if(active_projected_y && fabs(edge_y1-edge_y0)>1e-12){
        double sample=2.0*(double)(dest_y0+y)-0.5;
        double v=(sample-edge_y0)*(double)t->sh/(edge_y1-edge_y0)-0.5;
        ya=(int)floor(v); yb=ya+1; fy=v-(double)ya;
      } else { ya=y-1; yb=y; fy=0.5; }
      if(ya<0) ya=0; else if(ya>=t->sh) ya=t->sh-1;
      if(yb<0) yb=0; else if(yb>=t->sh) yb=t->sh-1;
    }
    for(int x=0;x<t->sw;x++){
      int xa=x,xb=x; double fx=0.0;
      if(phase_x){
        if(active_projected_x && fabs(edge_x1-edge_x0)>1e-12){
          double sample=2.0*(double)(dest_x0+x)-0.5;
          double u=(sample-edge_x0)*(double)t->sw/(edge_x1-edge_x0)-0.5;
          xa=(int)floor(u); xb=xa+1; fx=u-(double)xa;
        } else { xa=x-1; xb=x; fx=0.5; }
        if(xa<0) xa=0; else if(xa>=t->sw) xa=t->sw-1;
        if(xb<0) xb=0; else if(xb>=t->sw) xb=t->sw-1;
      }
      const uint8_t *p00=atlas->px+((size_t)(t->sy+ya)*atlas->w+t->sx+xa)*4;
      const uint8_t *p01=atlas->px+((size_t)(t->sy+ya)*atlas->w+t->sx+xb)*4;
      const uint8_t *p10=atlas->px+((size_t)(t->sy+yb)*atlas->w+t->sx+xa)*4;
      const uint8_t *p11=atlas->px+((size_t)(t->sy+yb)*atlas->w+t->sx+xb)*4;
      int sr,sg,sb,aa;
      if(!active_projected){
        sr=p00[0];sg=p00[1];sb=p00[2];aa=p00[3];
        if(phase_x){ sr+=p01[0]; sg+=p01[1]; sb+=p01[2]; aa+=p01[3]; }
        if(phase_y){
          sr+=p10[0]; sg+=p10[1]; sb+=p10[2]; aa+=p10[3];
          if(phase_x){ sr+=p11[0]; sg+=p11[1]; sb+=p11[2]; aa+=p11[3]; }
        }
        sr=(sr+taps/2)/taps; sg=(sg+taps/2)/taps;
        sb=(sb+taps/2)/taps; aa=(aa+taps/2)/taps;
      } else {
        double ix=1.0-fx,iy=1.0-fy;
        sr=(int)(p00[0]*ix*iy+p01[0]*fx*iy+p10[0]*ix*fy+p11[0]*fx*fy+0.5);
        sg=(int)(p00[1]*ix*iy+p01[1]*fx*iy+p10[1]*ix*fy+p11[1]*fx*fy+0.5);
        sb=(int)(p00[2]*ix*iy+p01[2]*fx*iy+p10[2]*ix*fy+p11[2]*fx*fy+0.5);
        aa=(int)(p00[3]*ix*iy+p01[3]*fx*iy+p10[3]*ix*fy+p11[3]*fx*fy+0.5);
      }
      cache[(size_t)y*t->sw+x]=((uint32_t)aa<<24)|((uint32_t)sr<<16)|
                                      ((uint32_t)sg<<8)|(uint32_t)sb;
    }
  }
  *cache_slot=cache;
  if(!direct_cache) r->interp_subrect_bytes+=bytes;
  return cache;
}
static double classic_interp_projected_edge(double relative, double camera, int logical_size){
  float logical=(float)logical_size;
  float world=(float)(relative+camera+(double)logical_size*0.5);
  float vertex=world-0.5f;
  float precision_view=(float)camera+logical*0.5f;
  float centre=-(precision_view+logical*0.5f);
  float projection=-2.0f/logical;
  float combined_translation=centre*projection;
  float matrix_scale=-projection;
  float matrix_translation=-combined_translation+
                           (float)(1.0/(2.0*(double)logical_size));
  float corrected=matrix_scale*vertex+matrix_translation;
  return ((double)corrected+1.0)*(double)logical_size;
}
static int classic_interp_projected_axis(double camera, int logical_size){
  if(logical_size<=0) return 0;
  double edge0=classic_interp_projected_edge(0.0,camera,logical_size);
  double edge1=classic_interp_projected_edge((double)logical_size,camera,logical_size);
  return edge0 < -0.5 || edge1-edge0 < 2.0*(double)logical_size;
}
static const uint8_t *interp_tpag_sample(const GmlAtlas *atlas, const GmlTpag *t,
                                         int x, int y, int logical_margin){
  static const uint8_t transparent[4]={0,0,0,0};
  if(logical_margin){
    int right=t->bw-t->tx-t->sw;
    int bottom=t->bh-t->ty-t->sh;
    if((x<0 && t->tx>0) || (x>=t->sw && right>0) ||
       (y<0 && t->ty>0) || (y>=t->sh && bottom>0)){
      int ax=t->sx+x, ay=t->sy+y;
      if(ax>=0 && ax<atlas->w && ay>=0 && ay<atlas->h)
        return atlas->px+((size_t)ay*atlas->w+ax)*4;
      return transparent;
    }
  }
  if(x<0) x=0; else if(x>=t->sw) x=t->sw-1;
  if(y<0) y=0; else if(y>=t->sh) y=t->sh-1;
  return atlas->px+((size_t)(t->sy+y)*atlas->w+t->sx+x)*4;
}
static inline void interp_tpag_filtered_sample(
    const GmlAtlas *atlas,const GmlTpag *tpag,
    int ua,int ub,int va,int vb,double fx,double fy,int logical_margin,
    double *red,double *green,double *blue,double *alpha){
  const uint8_t *p00=interp_tpag_sample(atlas,tpag,ua,va,logical_margin);
  const uint8_t *p01=interp_tpag_sample(atlas,tpag,ub,va,logical_margin);
  const uint8_t *p10=interp_tpag_sample(atlas,tpag,ua,vb,logical_margin);
  const uint8_t *p11=interp_tpag_sample(atlas,tpag,ub,vb,logical_margin);
  double inverse_x=1.0-fx,inverse_y=1.0-fy;
  double weight00=inverse_x*inverse_y;
  double weight01=fx*inverse_y;
  double weight10=inverse_x*fy;
  double weight11=fx*fy;
  *red=p00[0]*weight00+p01[0]*weight01+p10[0]*weight10+p11[0]*weight11;
  *green=p00[1]*weight00+p01[1]*weight01+p10[1]*weight10+p11[1]*weight11;
  *blue=p00[2]*weight00+p01[2]*weight01+p10[2]*weight10+p11[2]*weight11;
  *alpha=p00[3]*weight00+p01[3]*weight01+p10[3]*weight10+p11[3]*weight11;
}

static int tpag_interp_draw_key_equal(
    const GmlTpagInterpKey *left,const GmlTpagInterpKey *right){
  return left->framebuffer_width==right->framebuffer_width &&
         left->framebuffer_height==right->framebuffer_height &&
         left->destination_x==right->destination_x &&
         left->destination_y==right->destination_y &&
         left->width==right->width && left->height==right->height &&
         left->source_x==right->source_x && left->source_y==right->source_y &&
         left->draw_x==right->draw_x && left->draw_y==right->draw_y &&
         left->scale_x==right->scale_x && left->scale_y==right->scale_y;
}

static void tpag_interp_draw_cache_release(GmlRender *render,GmlTpag *tpag){
  if(!tpag) return;
  free(tpag->interp_draw_cache);
  free(tpag->interp_draw_runs);
  if(render){
    if(tpag->interp_draw_cache_bytes<=render->interp_draw_cache_bytes)
      render->interp_draw_cache_bytes-=tpag->interp_draw_cache_bytes;
    else
      render->interp_draw_cache_bytes=0;
  }
  tpag->interp_draw_cache=NULL;
  tpag->interp_draw_runs=NULL;
  tpag->interp_draw_run_count=0;
  tpag->interp_draw_cache_bytes=0;
  tpag->interp_draw_cache_valid=0;
}

static int tpag_interp_draw_cache_reserve(
    GmlRender *render,const GmlTpag *keep,size_t bytes){
  if(!render || bytes>GML_INTERP_DRAW_CACHE_BUDGET) return 0;
  while(render->interp_draw_cache_bytes>
        GML_INTERP_DRAW_CACHE_BUDGET-bytes){
    GmlTpag *oldest=NULL;
    for(int index=0;index<render->n_tpag;index++){
      GmlTpag *candidate=&render->tpag[index];
      if(candidate==keep || !candidate->interp_draw_cache_valid ||
         !candidate->interp_draw_cache_bytes) continue;
      if(!oldest || candidate->interp_draw_last_frame<oldest->interp_draw_last_frame)
        oldest=candidate;
    }
    if(!oldest) return 0;
    tpag_interp_draw_cache_release(render,oldest);
  }
  return 1;
}

static int tpag_interp_draw_cache_add_run(
    GmlTpagInterpRun **runs,int *count,int *capacity,int limit,
    int y,int x,int opaque){
  if(!runs || !count || !capacity) return 0;
  if(*count>0){
    GmlTpagInterpRun *last=&(*runs)[*count-1];
    if(last->y==y && last->x+last->len==x && last->opaque==(uint8_t)opaque){
      last->len++;
      return 1;
    }
  }
  if(*count>=limit) return 0;
  if(*count>=*capacity){
    int grown=*capacity ? *capacity*2 : 1024;
    if(grown>limit) grown=limit;
    GmlTpagInterpRun *replacement=
      realloc(*runs,(size_t)grown*sizeof(**runs));
    if(!replacement) return 0;
    *runs=replacement;
    *capacity=grown;
  }
  (*runs)[*count]=(GmlTpagInterpRun){y,x,1,(uint8_t)opaque};
  (*count)++;
  return 1;
}

static void GML_HOT_RENDER tpag_interp_draw_cache_replay(
    GmlRender *render,const GmlTpag *tpag){
  const GmlTpagInterpKey *key=&tpag->interp_draw_key;
  for(int index=0;index<tpag->interp_draw_run_count;index++){
    const GmlTpagInterpRun *run=&tpag->interp_draw_runs[index];
    uint32_t *destination=render->fb+
      (size_t)(key->destination_y+run->y)*render->fbw+
      key->destination_x+run->x;
    const uint32_t *source=tpag->interp_draw_cache+
      (size_t)run->y*key->width+run->x;
    if(run->opaque){
      memcpy(destination,source,(size_t)run->len*sizeof(*destination));
      continue;
    }
    for(int pixel=0;pixel<run->len;pixel++){
      uint32_t source_pixel=source[pixel];
      uint32_t destination_pixel=destination[pixel];
      unsigned source_alpha=source_pixel>>24;
      unsigned inverse_alpha=255u-source_alpha;
      unsigned red=(((source_pixel>>16)&255u)*source_alpha+127u)/255u+
                   (((destination_pixel>>16)&255u)*inverse_alpha+127u)/255u;
      unsigned green=(((source_pixel>>8)&255u)*source_alpha+127u)/255u+
                     (((destination_pixel>>8)&255u)*inverse_alpha+127u)/255u;
      unsigned blue=((source_pixel&255u)*source_alpha+127u)/255u+
                    ((destination_pixel&255u)*inverse_alpha+127u)/255u;
      if(red>255u) red=255u;
      if(green>255u) green=255u;
      if(blue>255u) blue=255u;
      destination[pixel]=UINT32_C(0xFF000000)|(red<<16)|(green<<8)|blue;
    }
  }
}

static int tpag_interp_draw_cache_build(
    GmlRender *render,GmlTpag *tpag,const GmlInterpBlitBand *band,
    const GmlTpagInterpKey *key){
  if(!band->source_a || !band->fraction_x || key->width<=0 || key->height<=0) return 0;
  size_t pixel_count=(size_t)key->width*(size_t)key->height;
  if(pixel_count==0 || pixel_count>GML_INTERP_DRAW_CACHE_MAX_PIXELS ||
     pixel_count>SIZE_MAX/sizeof(uint32_t)) return 0;
  size_t pixel_bytes=pixel_count*sizeof(uint32_t);
  size_t maximum_runs=pixel_count;
  if(maximum_runs>GML_INTERP_DRAW_CACHE_MAX_RUNS)
    maximum_runs=GML_INTERP_DRAW_CACHE_MAX_RUNS;
  size_t maximum_bytes=pixel_bytes+maximum_runs*sizeof(GmlTpagInterpRun);
  if(maximum_bytes<pixel_bytes ||
     !tpag_interp_draw_cache_reserve(render,tpag,maximum_bytes)) return 0;
  size_t run_budget=GML_INTERP_DRAW_CACHE_BUDGET-
                    render->interp_draw_cache_bytes-pixel_bytes;
  int run_limit=(int)(run_budget/sizeof(GmlTpagInterpRun));
  if(run_limit>GML_INTERP_DRAW_CACHE_MAX_RUNS)
    run_limit=GML_INTERP_DRAW_CACHE_MAX_RUNS;
  if(run_limit<=0) return 0;
  uint32_t *pixels=malloc(pixel_bytes);
  GmlTpagInterpRun *runs=NULL;
  int run_count=0,run_capacity=0;
  if(!pixels) return 0;
  for(int row=0;row<key->height;row++){
    int destination_y=key->destination_y+row;
    double source_y=
      (((double)destination_y+0.5)-band->destination_y)/band->abs_yscale-0.5;
    int source_a_y=(int)floor(source_y);
    int source_b_y=source_a_y+1;
    double fraction_y=source_y-source_a_y;
    uint32_t *output=pixels+(size_t)row*key->width;
    for(int column=0;column<key->width;column++){
      int source_a_x=band->source_a[column];
      int source_b_x=source_a_x+1;
      double filtered_red,filtered_green,filtered_blue,filtered_alpha;
      interp_tpag_filtered_sample(
        band->atlas,band->tpag,source_a_x,source_b_x,source_a_y,source_b_y,
        band->fraction_x[column],fraction_y,band->logical_margin,
        &filtered_red,&filtered_green,&filtered_blue,&filtered_alpha);
      int red=(int)(filtered_red+0.5);
      int green=(int)(filtered_green+0.5);
      int blue=(int)(filtered_blue+0.5);
      int alpha=(int)(filtered_alpha+0.5);
      output[column]=((uint32_t)alpha<<24)|((uint32_t)red<<16)|
                     ((uint32_t)green<<8)|(uint32_t)blue;
      if(alpha>0 && !tpag_interp_draw_cache_add_run(
           &runs,&run_count,&run_capacity,run_limit,row,column,alpha>=255)){
        free(pixels);
        free(runs);
        return 0;
      }
    }
  }
  size_t run_bytes=(size_t)run_capacity*sizeof(*runs);
  if(run_bytes>run_budget){
    free(pixels);
    free(runs);
    return 0;
  }
  tpag->interp_draw_cache=pixels;
  tpag->interp_draw_runs=runs;
  tpag->interp_draw_run_count=run_count;
  tpag->interp_draw_key=*key;
  tpag->interp_draw_cache_bytes=pixel_bytes+run_bytes;
  tpag->interp_draw_last_frame=render->frame;
  tpag->interp_draw_cache_valid=1;
  render->interp_draw_cache_bytes+=tpag->interp_draw_cache_bytes;
  return 1;
}

static int tpag_interp_draw_cache_try(
    GmlRender *render,GmlTpag *tpag,const GmlInterpBlitBand *band,int row_count){
  if(!render || !tpag || !band || row_count<=0 ||
     !render->win || !anygm_policy_uses_first_generation_studio(render->win) ||
     rprof_tpag_id(render,tpag)<0 || band->flip_x || band->flip_y ||
     band->solid_mask || band->solid_blur ||
     mapped_texture_active(render) || shader_alpha_test_requires_filter(render) ||
     render->blendmode!=0 || !render->alphablend ||
     gml_render_target_preserves_alpha(render) || render->color_write_mask!=0x0F ||
     band->alpha<1.0 || (band->blend&0xFFFFFFu)!=0xFFFFFFu)
    return 0;
  GmlTpagInterpKey key={
    render->fbw,render->fbh,
    band->x0+band->source_x0,band->y0+band->source_y0,
    band->source_x1-band->source_x0,row_count,
    band->source_x0,band->source_y0,
    band->destination_x,band->destination_y,band->abs_xscale,band->abs_yscale
  };
  size_t visible_pixels=(size_t)key.width*(size_t)key.height;
  if(visible_pixels<GML_INTERP_DRAW_CACHE_MIN_PIXELS) return 0;
  if(tpag->interp_draw_cache_valid &&
     tpag_interp_draw_key_equal(&tpag->interp_draw_key,&key)){
    tpag->interp_draw_last_frame=render->frame;
    tpag_interp_draw_cache_replay(render,tpag);
    return 1;
  }
  if(tpag_interp_draw_key_equal(&tpag->interp_draw_pending_key,&key)){
    if(tpag->interp_draw_pending_count<0) return 0;
    tpag->interp_draw_pending_count++;
  } else {
    tpag->interp_draw_pending_key=key;
    tpag->interp_draw_pending_count=1;
  }
  if(tpag->interp_draw_pending_count<2) return 0;
  tpag_interp_draw_cache_release(render,tpag);
  if(!tpag_interp_draw_cache_build(render,tpag,band,&key)){
    tpag->interp_draw_pending_count=-1;
    return 0;
  }
  tpag_interp_draw_cache_replay(render,tpag);
  return 1;
}

static void blit_interp_pretinted_constant_alpha_sample(
    GmlRender *r,uint32_t *destination,int red,int green,int blue,
    int alpha,double draw_alpha){
  if(alpha<=0) return;
  if(!r->alphablend){
    *destination=0xFF000000u|((uint32_t)red<<16)|((uint32_t)green<<8)|(uint32_t)blue;
    return;
  }
  if(alpha>=255 && draw_alpha>=1.0){
    *destination=0xFF000000u|((uint32_t)red<<16)|((uint32_t)green<<8)|(uint32_t)blue;
    return;
  }
  if(draw_alpha>=1.0){
    int inverse_alpha=255-alpha;
    int destination_red=(*destination>>16)&255;
    int destination_green=(*destination>>8)&255;
    int destination_blue=*destination&255;
    int out_red=(red*alpha+127)/255+
                (destination_red*inverse_alpha+127)/255;
    int out_green=(green*alpha+127)/255+
                  (destination_green*inverse_alpha+127)/255;
    int out_blue=(blue*alpha+127)/255+
                 (destination_blue*inverse_alpha+127)/255;
    if(out_red>255) out_red=255;
    if(out_green>255) out_green=255;
    if(out_blue>255) out_blue=255;
    uint32_t coverage=r->target_sp>0
      ? gml_sprite_target_alpha(r,*destination,(unsigned)alpha)
      : UINT32_C(0xFF000000);
    *destination=coverage|((uint32_t)out_red<<16)|
                 ((uint32_t)out_green<<8)|(uint32_t)out_blue;
    return;
  }
  double source_alpha=(alpha/255.0)*draw_alpha;
  int destination_red=(*destination>>16)&255;
  int destination_green=(*destination>>8)&255;
  int destination_blue=*destination&255;
  int out_red=(int)(red*source_alpha+0.5)+
              (int)(destination_red*(1.0-source_alpha)+0.5);
  int out_green=(int)(green*source_alpha+0.5)+
                (int)(destination_green*(1.0-source_alpha)+0.5);
  int out_blue=(int)(blue*source_alpha+0.5)+
               (int)(destination_blue*(1.0-source_alpha)+0.5);
  if(out_red>255) out_red=255;
  if(out_green>255) out_green=255;
  if(out_blue>255) out_blue=255;
  unsigned target_alpha=(unsigned)lround((double)alpha*draw_alpha);
  *destination=gml_sprite_target_alpha(r,*destination,target_alpha)|
               ((uint32_t)out_red<<16)|((uint32_t)out_green<<8)|(uint32_t)out_blue;
}
static void blit_interp_constant_alpha_sample(
    GmlRender *r,uint32_t *destination,uint32_t rgb,int alpha,
    int blend_r,int blend_g,int blend_b,double draw_alpha){
  int tint_bias=gml_blend_family(r)==GML_BLEND_STUDIO2?127:0;
  int red=(((rgb>>16)&255)*blend_r+tint_bias)/255;
  int green=(((rgb>>8)&255)*blend_g+tint_bias)/255;
  int blue=((rgb&255)*blend_b+tint_bias)/255;
  blit_interp_pretinted_constant_alpha_sample(
    r,destination,red,green,blue,alpha,draw_alpha);
}
static void blit_interp_solid_mask_sample(
    GmlRender *r, const struct GmlShaderPal *mask, uint32_t *destination,
    const GmlAtlas *atlas, const GmlTpag *tpag,
    int ua, int ub, int va, int vb, double fx, double fy, int logical_margin,
    int blend_r, int blend_g, int blend_b, double draw_alpha){
  const uint8_t *p00=interp_tpag_sample(atlas,tpag,ua,va,logical_margin);
  const uint8_t *p01=interp_tpag_sample(atlas,tpag,ub,va,logical_margin);
  const uint8_t *p10=interp_tpag_sample(atlas,tpag,ua,vb,logical_margin);
  const uint8_t *p11=interp_tpag_sample(atlas,tpag,ub,vb,logical_margin);
  double filtered_alpha;
  if(p00[3]==p01[3] && p00[3]==p10[3] && p00[3]==p11[3])
    filtered_alpha=p00[3];
  else {
    double inverse_x=1.0-fx, inverse_y=1.0-fy;
    double top=p00[3]*inverse_x+p01[3]*fx;
    double bottom=p10[3]*inverse_x+p11[3]*fx;
    filtered_alpha=top*inverse_y+bottom*fy;
  }
  if(shader_discards_alpha_value(r,filtered_alpha)) return;
  int alpha=(int)(filtered_alpha+0.5);
  if(mask->solid_alpha_mask_inclusive
       ? alpha<=mask->solid_alpha_mask_cutoff_step
       : alpha< mask->solid_alpha_mask_cutoff_step) alpha=0;
  /* Recognized solid fragments never read v_vColour; the uniform is the whole tint. */
  (void)blend_r; (void)blend_g; (void)blend_b;
  blit_interp_constant_alpha_sample(
    r,destination,mask->solid_alpha_mask_rgb,alpha,
    255,255,255,draw_alpha);
}
static uint32_t solid_blur_cache_sample(
    const uint32_t *alpha,const GmlTpag *tpag,int x,int y){
  if(!alpha || !tpag || tpag->sw<=0 || tpag->sh<=0) return 0;
  if(x<0)x=0;else if(x>=tpag->sw)x=tpag->sw-1;
  if(y<0)y=0;else if(y>=tpag->sh)y=tpag->sh-1;
  return alpha[(size_t)y*tpag->sw+x]>>24;
}
static inline int solid_mask_cached_alpha(
    GmlRender *r,const struct GmlShaderPal *mask,const uint32_t *argb,
    const GmlTpag *tpag,int ua,int ub,int va,int vb,double fx,double fy){
  unsigned alpha00=solid_blur_cache_sample(argb,tpag,ua,va);
  unsigned alpha01=solid_blur_cache_sample(argb,tpag,ub,va);
  unsigned alpha10=solid_blur_cache_sample(argb,tpag,ua,vb);
  unsigned alpha11=solid_blur_cache_sample(argb,tpag,ub,vb);
  double filtered_alpha;
  if(alpha00==alpha01 && alpha00==alpha10 && alpha00==alpha11)
    filtered_alpha=alpha00;
  else {
    double inverse_x=1.0-fx,inverse_y=1.0-fy;
    double top=alpha00*inverse_x+alpha01*fx;
    double bottom=alpha10*inverse_x+alpha11*fx;
    filtered_alpha=top*inverse_y+bottom*fy;
  }
  if(shader_discards_alpha_value(r,filtered_alpha)) return 0;
  int alpha=(int)floor(filtered_alpha+0.5);
  if(mask->solid_alpha_mask_inclusive
       ? alpha<=mask->solid_alpha_mask_cutoff_step
       : alpha<mask->solid_alpha_mask_cutoff_step) return 0;
  return alpha;
}
static void blit_interp_solid_mask_cached_sample(
    GmlRender *r,const struct GmlShaderPal *mask,const uint32_t *argb,
    uint32_t *destination,const GmlTpag *tpag,
    int ua,int ub,int va,int vb,double fx,double fy,
    int red,int green,int blue,double draw_alpha){
  int alpha=solid_mask_cached_alpha(
    r,mask,argb,tpag,ua,ub,va,vb,fx,fy);
  if(!alpha) return;
  blit_interp_pretinted_constant_alpha_sample(
    r,destination,red,green,blue,alpha,draw_alpha);
}
static void blit_interp_solid_blur_sample(
    GmlRender *r,const uint32_t *alpha,
    uint32_t *destination,const GmlTpag *tpag,
    int ua,int ub,int va,int vb,double fx,double fy,
    int red,int green,int blue,double draw_alpha){
  double inverse_x=1.0-fx,inverse_y=1.0-fy;
  double filtered_alpha=
    solid_blur_cache_sample(alpha,tpag,ua,va)*inverse_x*inverse_y+
    solid_blur_cache_sample(alpha,tpag,ub,va)*fx*inverse_y+
    solid_blur_cache_sample(alpha,tpag,ua,vb)*inverse_x*fy+
    solid_blur_cache_sample(alpha,tpag,ub,vb)*fx*fy;
  int sample_alpha=(int)floor(filtered_alpha+0.5);
  blit_interp_pretinted_constant_alpha_sample(
    r,destination,red,green,blue,sample_alpha,draw_alpha);
}
static void blit_interp_sample(GmlRender *r, uint32_t *dp, const GmlAtlas *atlas,
                               const GmlTpag *t, int ua, int ub, int va, int vb,
                               double fx, double fy, int logical_margin,
                               int transparent_tap, int bR, int bG, int bB,
                               uint32_t blend, double alpha){
  double fsr,fsg,fsb,faa;
  interp_tpag_filtered_sample(
    atlas,t,ua,ub,va,vb,fx,fy,logical_margin,&fsr,&fsg,&fsb,&faa);
  if(r->font_sdf_active){
    double low=128.0-r->font_sdf_width*0.5;
    double amount=(faa-low)/r->font_sdf_width;
    if(amount<=0.0) faa=0.0;
    else if(amount>=1.0) faa=255.0;
    else faa=(amount*amount*(3.0-2.0*amount))*255.0;
    fsr=fsg=fsb=255.0;
  }
  if(shader_discards_alpha_value(r,faa) &&
     !(r->blendmode==2 && !shader_alpha_test_active(r))) return;
  /* A Studio 2 shader receives the filtered sample as floating-point colour. Keep that precision
   * through vertex-colour modulation and ordinary source-alpha blending; quantizing the sample
   * first produces systematic one-channel errors on minified artwork. */
  if(r->win && anygm_policy_has_modern_layer_semantics(r->win) && !mapped_texture_active(r) && r->blendmode==0){
    double tr=fsr*(double)bR/255.0, tg=fsg*(double)bG/255.0, tb=fsb*(double)bB/255.0;
    double sa=faa*alpha/255.0;
    if(sa<=0.0) return;
    int rr,rg,rb;
    if(!r->alphablend || sa>=1.0){
      rr=(int)floor(tr+0.5); rg=(int)floor(tg+0.5); rb=(int)floor(tb+0.5);
    } else {
      int dr=(*dp>>16)&0xFF, dg=(*dp>>8)&0xFF, db=*dp&0xFF;
      rr=(int)floor(tr*sa+dr*(1.0-sa)+0.5);
      rg=(int)floor(tg*sa+dg*(1.0-sa)+0.5);
      rb=(int)floor(tb*sa+db*(1.0-sa)+0.5);
    }
    if(rr<0) rr=0; else if(rr>255) rr=255;
    if(rg<0) rg=0; else if(rg>255) rg=255;
    if(rb<0) rb=0; else if(rb>255) rb=255;
    unsigned source_alpha=(unsigned)lround(faa*alpha);
    *dp=gml_sprite_target_alpha(r,*dp,source_alpha)|((uint32_t)rr<<16)|
        ((uint32_t)rg<<8)|(uint32_t)rb;
    return;
  }
  int sr=(int)(fsr+0.5), sg=(int)(fsg+0.5), sb=(int)(fsb+0.5);
  int aa=(int)(faa+0.5);
  if(aa<=0 && r->blendmode!=2) return;
  if(mapped_texture_active(r)){
    uint32_t sampled=mapped_texture_pixel(r,((uint32_t)aa<<24)|((uint32_t)sr<<16)|
                                             ((uint32_t)sg<<8)|(uint32_t)sb);
    aa=(int)(sampled>>24); sr=(sampled>>16)&255; sg=(sampled>>8)&255; sb=sampled&255;
    if(aa<=0 && r->blendmode!=2) return;
  }
  int tint_bias=gml_blend_family(r)==GML_BLEND_STUDIO2?127:0;
  sr=(sr*bR+tint_bias)/255; sg=(sg*bG+tint_bias)/255; sb=(sb*bB+tint_bias)/255;
  int precise_margin=transparent_tap && alpha>=1.0 &&
    (blend&0xFFFFFFu)==0xFFFFFFu && r->blendmode==0 && r->alphablend;
  double sa=((precise_margin?faa:(double)aa)/255.0)*alpha;
  if(!r->alphablend){
    *dp=0xFF000000u|((uint32_t)sr<<16)|((uint32_t)sg<<8)|(uint32_t)sb;
    return;
  }
  int dr=(*dp>>16)&0xFF, dg=(*dp>>8)&0xFF, db=*dp&0xFF;
  int rr,rg,rb;
  if(r->blendmode==1){
    rr=dr+(int)(sr*sa); rg=dg+(int)(sg*sa); rb=db+(int)(sb*sa);
  } else if(r->blendmode==2){
    rr=(int)gml_blend_inv_source_u8((unsigned)dr,(unsigned)sr);
    rg=(int)gml_blend_inv_source_u8((unsigned)dg,(unsigned)sg);
    rb=(int)gml_blend_inv_source_u8((unsigned)db,(unsigned)sb);
  } else if(r->blendmode==4){
    unsigned source_alpha=(unsigned)lround((precise_margin?faa:(double)aa)*alpha);
    uint32_t out=blend_max_preset_pixel(r,*dp,sr,sg,sb,source_alpha);
    *dp=color_write_merge(r,*dp,out);
    return;
  } else if(sa>=1.0){
    rr=sr; rg=sg; rb=sb;
  } else if(precise_margin){
    rr=(int)(fsr*sa+0.5)+(int)(dr*(1.0-sa)+0.5);
    rg=(int)(fsg*sa+0.5)+(int)(dg*(1.0-sa)+0.5);
    rb=(int)(fsb*sa+0.5)+(int)(db*(1.0-sa)+0.5);
  } else {
    rr=(int)(sr*sa+0.5)+(int)(dr*(1.0-sa)+0.5);
    rg=(int)(sg*sa+0.5)+(int)(dg*(1.0-sa)+0.5);
    rb=(int)(sb*sa+0.5)+(int)(db*(1.0-sa)+0.5);
  }
  if(rr<0) rr=0; else if(rr>255) rr=255;
  if(rg<0) rg=0; else if(rg>255) rg=255;
  if(rb<0) rb=0; else if(rb>255) rb=255;
  unsigned source_alpha=(unsigned)lround((precise_margin?faa:(double)aa)*alpha);
  *dp=gml_sprite_target_alpha(r,*dp,source_alpha)|((uint32_t)rr<<16)|
      ((uint32_t)rg<<8)|(uint32_t)rb;
}
static void interp_blit_band_rows(void *context, int row_start, int row_end, int slot){
  GmlInterpBlitBand *band=(GmlInterpBlitBand*)context;
  (void)slot;
  for(int row_index=row_start;row_index<row_end;row_index++){
    int yy=band->source_y0+row_index;
    int py=band->flip_y ? band->y0-yy : band->y0+yy;
    double source_y=(band->flip_y
      ? band->destination_y-((double)py+0.5)
      : ((double)py+0.5)-band->destination_y)/band->abs_yscale-0.5;
    int va=(int)floor(source_y), vb=va+1;
    double fy=source_y-va;
    uint32_t *destination_row=band->r->fb+(size_t)py*band->r->fbw;
    for(int xx=band->source_x0;xx<band->source_x1;xx++){
      int px=band->flip_x ? band->x0-xx : band->x0+xx;
      int column=xx-band->source_x0;
      int ua;
      double fx;
      if(band->source_a && band->fraction_x){
        ua=band->source_a[column];
        fx=band->fraction_x[column];
      } else {
        double source_x=(band->flip_x
          ? band->destination_x-((double)px+0.5)
          : ((double)px+0.5)-band->destination_x)/band->abs_xscale-0.5;
        ua=(int)floor(source_x);
        fx=source_x-ua;
      }
      int ub=ua+1;
      if(band->solid_blur && band->solid_blur_alpha)
        blit_interp_solid_blur_sample(
          band->r,band->solid_blur_alpha,&destination_row[px],
          band->tpag,ua,ub,va,vb,fx,fy,
          band->solid_red,band->solid_green,band->solid_blue,band->alpha);
      else if(band->solid_mask && band->solid_mask_argb)
        blit_interp_solid_mask_cached_sample(
          band->r,band->solid_mask,band->solid_mask_argb,&destination_row[px],
          band->tpag,ua,ub,va,vb,fx,fy,
          band->solid_red,band->solid_green,band->solid_blue,band->alpha);
      else if(band->solid_mask)
        blit_interp_solid_mask_sample(
          band->r,band->solid_mask,&destination_row[px],band->atlas,band->tpag,
          ua,ub,va,vb,fx,fy,band->logical_margin,
          band->blend_r,band->blend_g,band->blend_b,band->alpha);
      else
        blit_interp_sample(band->r,&destination_row[px],band->atlas,band->tpag,
                           ua,ub,va,vb,fx,fy,band->logical_margin,0,
                           band->blend_r,band->blend_g,band->blend_b,
                           band->blend,band->alpha);
    }
  }
}
static void blit_interp_phase_plane(GmlRender *r, uint32_t *plane, GmlTpag *t,
                                    double dx, double dy, double xs, double ys,
                                    uint32_t blend, double alpha, int phase_x, int phase_y){
  if(!r || !plane || !t || !r->base_fb || t->sw<=0 || t->sh<=0 || alpha<=0.0) return;
  if(xs==0.0 || ys==0.0) return;
  if(t->atlas<0 || t->atlas>=r->n_atlas) return;
  GmlAtlas *atlas=&r->atlas[t->atlas];
  if(!atlas_pixels(r,t->atlas)) return;
  double axs=fabs(xs), ays=fabs(ys);
  int flipx=xs<0.0, flipy=ys<0.0;
  int x0=(int)floor(dx+0.5), y0=(int)floor(dy+0.5);
  if(r->classic && flipx) x0--;
  if(r->classic && flipy) y0--;
  int w=(int)lround(t->sw*axs), h=(int)lround(t->sh*ays);
  if(w<=0 || h<=0) return;
  int canonical=rprof_tpag_id(r,t)>=0;
  int right_pad=t->bw-t->tx-t->sw, bottom_pad=t->bh-t->ty-t->sh;
  int logical_margin=canonical && !flipx && !flipy &&
    (t->tx>0 || t->ty>0 || right_pad>0 || bottom_pad>0);
  int px0=flipx?x0-w+1:x0-(phase_x?1:0);
  int py0=flipy?y0-h+1:y0-(phase_y?1:0);
  int px1=flipx?x0+1:px0+w;
  int py1=flipy?y0+1:py0+h;
  /* A packed TPAG omits transparent rows and columns from the sprite's logical cell.  The
   * classic GPU still filters against those omitted transparent texels at the crop boundary.
   * Keep one trailing half-sample so the phase plane can represent that coverage instead of
   * exposing the already-opaque layer underneath for a whole output pixel. */
  if(logical_margin && phase_x && right_pad>0) px1++;
  if(logical_margin && phase_y && bottom_pad>0) py1++;
  if(px0<0) px0=0;
  if(py0<0) py0=0;
  if(px1>r->base_fbw) px1=r->base_fbw;
  if(py1>r->base_fbh) py1=r->base_fbh;
  if(px0>=px1 || py0>=py1) return;
  if(alpha>1.0) alpha=1.0;
  int project_view_x=classic_interp_projected_axis(r->cam_x,r->base_fbw);
  int project_view_y=classic_interp_projected_axis(r->cam_y,r->base_fbh);
  int projected_x=phase_x && project_view_x;
  int projected_y=phase_y && project_view_y;
  int bR=blend&0xFF, bG=(blend>>8)&0xFF, bB=(blend>>16)&0xFF;
  double edge_x0=0.0,edge_x1=0.0,edge_y0=0.0,edge_y1=0.0;
  if(project_view_x){
    edge_x0=classic_interp_projected_edge(dx,r->cam_x,r->base_fbw);
    edge_x1=classic_interp_projected_edge(dx+t->sw*axs,r->cam_x,r->base_fbw);
  }
  if(project_view_y){
    edge_y0=classic_interp_projected_edge(dy,r->cam_y,r->base_fbh);
    edge_y1=classic_interp_projected_edge(dy+t->sh*ays,r->cam_y,r->base_fbh);
  }
  if((!logical_margin || (!projected_x && !projected_y)) &&
     (!projected_x || !flipx) && (!projected_y || !flipy) &&
     fabs(axs-1.0)<0.001 && fabs(ays-1.0)<0.001 &&
     alpha>=1.0 && (blend&0xFFFFFFu)==0xFFFFFFu &&
     r->blendmode==0 && r->alphablend){
    uint32_t *cache=interp_phase_pixels(r,t,atlas,phase_x,phase_y,
                                        project_view_x,project_view_y,x0,y0,
                                        edge_x0,edge_x1,edge_y0,edge_y1);
    if(cache){
      int origin_x=x0-(phase_x?1:0), origin_y=y0-(phase_y?1:0);
      for(int py=py0;py<py1;py++){
        int sy=flipy?y0-py:py-origin_y;
        uint32_t *drow=plane+(size_t)py*r->base_fbw;
        for(int px=px0;px<px1;px++){
          int sx=flipx?x0-px:px-origin_x;
          int margin_edge=logical_margin &&
            ((phase_x && ((sx==0 && t->tx>0) || (sx==t->sw && right_pad>0))) ||
             (!phase_x && sx==t->sw-1 && right_pad>0) ||
             (phase_y && ((sy==0 && t->ty>0) || (sy==t->sh && bottom_pad>0))) ||
             (!phase_y && sy==t->sh-1 && bottom_pad>0));
          if(margin_edge){
            double u=phase_x?(double)sx-0.5:(double)sx;
            double v=phase_y?(double)sy-0.5:(double)sy;
            int ua=(int)floor(u), ub=ua+1, va=(int)floor(v), vb=va+1;
            int transparent_tap=
              (((ua<0 || ub<0) && t->tx>0) ||
               ((ua>=t->sw || ub>=t->sw) && right_pad>0) ||
               ((va<0 || vb<0) && t->ty>0) ||
               ((va>=t->sh || vb>=t->sh) && bottom_pad>0));
            blit_interp_sample(r,&drow[px],atlas,t,ua,ub,va,vb,
                               u-(double)ua,v-(double)va,1,transparent_tap,
                               bR,bG,bB,blend,alpha);
            continue;
          }
          uint32_t packed=cache[(size_t)sy*t->sw+sx];
          int aa=(int)(packed>>24);
          if(!aa) continue;
          uint32_t *dp=&drow[px];
          if(aa==255){ *dp=packed|0xFF000000u; continue; }
          int sr=(packed>>16)&0xFF,sg=(packed>>8)&0xFF,sb=packed&0xFF;
          int ia=255-aa,dr=(*dp>>16)&0xFF,dg=(*dp>>8)&0xFF,db=*dp&0xFF;
          int rr=(sr*aa+127)/255+(dr*ia+127)/255;
          int rg=(sg*aa+127)/255+(dg*ia+127)/255;
          int rb=(sb*aa+127)/255+(db*ia+127)/255;
          if(rr>255) rr=255;
          if(rg>255) rg=255;
          if(rb>255) rb=255;
          *dp=0xFF000000u|((uint32_t)rr<<16)|((uint32_t)rg<<8)|(uint32_t)rb;
        }
      }
      return;
    }
  }
  for(int py=py0;py<py1;py++){
    double sample_y=(double)py+(phase_y?0.5:0.0);
    double v=(flipy?(double)y0-sample_y:sample_y-(double)y0)/ays;
    if(projected_y && !flipy && fabs(edge_y1-edge_y0)>1e-12){
      double physical_sample=2.0*(double)(py+1)-0.5;
      v=(physical_sample-edge_y0)*(double)t->sh/(edge_y1-edge_y0)-0.5;
    }
    int va=(int)floor(v), vb=va+1;
    double fy=v-(double)va;
    if(!logical_margin){
      if(va<0) va=0; else if(va>=t->sh) va=t->sh-1;
      if(vb<0) vb=0; else if(vb>=t->sh) vb=t->sh-1;
    }
    uint32_t *drow=plane+(size_t)py*r->base_fbw;
    for(int px=px0;px<px1;px++){
      double sample_x=(double)px+(phase_x?0.5:0.0);
      double u=(flipx?(double)x0-sample_x:sample_x-(double)x0)/axs;
      if(projected_x && !flipx && fabs(edge_x1-edge_x0)>1e-12){
        double physical_sample=2.0*(double)(px+1)-0.5;
        u=(physical_sample-edge_x0)*(double)t->sw/(edge_x1-edge_x0)-0.5;
      }
      int ua=(int)floor(u), ub=ua+1;
      double fx=u-(double)ua;
      int transparent_tap=logical_margin &&
        (((ua<0 || ub<0) && t->tx>0) ||
         ((ua>=t->sw || ub>=t->sw) && right_pad>0) ||
         ((va<0 || vb<0) && t->ty>0) ||
         ((va>=t->sh || vb>=t->sh) && bottom_pad>0));
      blit_interp_sample(r,&drow[px],atlas,t,ua,ub,va,vb,fx,fy,
                         logical_margin,transparent_tap,bR,bG,bB,blend,alpha);
    }
  }
}
static GmlTpag phase_tpag_rows(const GmlTpag *src, int row, int count){
  GmlTpag t=*src;
  t.sy+=row;
  t.sh=count;
  t.ty+=row;
  /* This short-lived view must not inherit geometry-dependent caches from the full item. */
  t.alpha_scanned=1;
  t.ax0=0; t.ay0=0; t.ax1=t.sw-1; t.ay1=count-1;
  t.alpha_row_min=t.alpha_row_max=NULL;
  t.alpha_qrow_min=t.alpha_qrow_max=NULL; t.alpha_qrow_built=NULL;
  t.alpha_runs=NULL; t.alpha_run_count=0; t.alpha_runs_built=0;
  t.alpha8_cache=NULL;
  t.argb_cache=NULL;
  t.solid_blur_alpha_cache=NULL; t.solid_blur_alpha_shader=-1;
  t.interp_phase_cache[0]=t.interp_phase_cache[1]=t.interp_phase_cache[2]=NULL;
  t.fast8_draw_cache=NULL; t.fast8_draw_cache_valid=0; t.fast8_draw_pending_count=0;
  t.interp_draw_cache=NULL; t.interp_draw_runs=NULL;
  t.interp_draw_cache_bytes=0; t.interp_draw_run_count=0;
  t.interp_draw_cache_valid=0; t.interp_draw_pending_count=0;
  return t;
}
static void blit_phase_plane_previous(GmlRender *r, uint32_t *plane, GmlTpag *t,
                                      double dx, double dy, double xs, double ys,
                                      uint32_t blend, double alpha);
static double classic_phase_edge_y(const GmlRender *r, double world_y){
  /* Recreate the precision of the classic fixed-function-style vertex path.  Vertex positions,
   * camera translation, projection and the half-pixel correction are each rounded to float before
   * the 2x viewport maps normalized coordinates back to pixels. */
  float logical_h=(float)r->fbh;
  float vertex=(float)(world_y-0.5);
  float precision_view=(float)r->cam_y+logical_h*0.5f;
  float centre=-(precision_view+logical_h*0.5f);
  float projection=-2.0f/logical_h;
  /* The renderer multiplies view * projection * correction on the CPU, then the shader applies
   * that already-rounded matrix to each vertex.  Keeping these temporaries separate prevents a
   * mathematically equivalent, but observably different, reassociation. */
  float combined_translation=centre*projection;
  float matrix_scale=-projection;
  float matrix_translation=-combined_translation+
                           (float)(1.0/(2.0*(double)r->fbh));
  float corrected=matrix_scale*vertex+matrix_translation;
  return ((double)corrected+1.0)*(double)r->fbh;
}
static double classic_phase_edge_y_sequential(const GmlRender *r, double world_y){
  /* Keep the vertex-shader stages separate.  This is the other observable rounding path of the
   * classic transform: vertex and view translation are combined before the projection scale. */
  float logical_h=(float)r->fbh;
  float vertex=(float)(world_y-0.5);
  float centre=-((float)r->cam_y+logical_h*0.5f);
  float viewed=vertex+centre;
  float projected=viewed*(-2.0f/logical_h);
  float corrected=-projected+(float)(1.0/(2.0*(double)r->fbh));
  return ((double)corrected+1.0)*(double)r->fbh;
}
static int classic_phase_ordinary_mode(const GmlRender *r){
  /* Classify the active float transform, not the project.  A neutral origin that rounds before
   * the half pixel needs the sequential/reversed path.  If both neutral edges acquire the same
   * error, the projected span is closed and coverage belongs to the preceding texel.  Otherwise
   * the ordinary next-texel tie is correct unless camera translation crosses the neutral tie. */
  float logical_h=(float)r->fbh;
  float vertex0=logical_h*0.5f-0.5f;
  float vertex1=vertex0+logical_h;
  float centre=-logical_h;
  float projection=-2.0f/logical_h;
  float combined_translation=centre*projection;
  float matrix_translation=-combined_translation+
                           (float)(1.0/(2.0*(double)r->fbh));
  float corrected0=(-projection)*vertex0+matrix_translation;
  float corrected1=(-projection)*vertex1+matrix_translation;
  double edge0=((double)corrected0+1.0)*(double)r->fbh;
  double edge1=((double)corrected1+1.0)*(double)r->fbh;
  if(edge0<-0.5) return 3;
  if(fabs((edge1-edge0)-2.0*(double)r->fbh)<1e-7) return 0;

  float camera=(float)r->cam_y;
  float actual_vertex=(float)(r->cam_y-0.5);
  float actual_centre=-(camera+logical_h*0.5f);
  float actual_translation=-(actual_centre*projection)+
                           (float)(1.0/(2.0*(double)r->fbh));
  float actual_corrected=(-projection)*actual_vertex+actual_translation;
  double actual_edge=((double)actual_corrected+1.0)*(double)r->fbh;
  float neutral_centre=-logical_h*0.5f;
  float neutral_translation=-(neutral_centre*projection)+
                            (float)(1.0/(2.0*(double)r->fbh));
  float neutral_corrected=(-projection)*-0.5f+neutral_translation;
  double neutral_edge=((double)neutral_corrected+1.0)*(double)r->fbh;
  return (actual_edge<-0.5)!=(neutral_edge<-0.5)?3:1;
}
static void blit_phase_plane_classic_y(GmlRender *r, uint32_t *plane, GmlTpag *t,
                                       double dx, double dy, double xs, double ys,
                                       uint32_t blend, double alpha,
                                       int sequential, int reverse_tie){
  int full_h=t&&t->bh>0?t->bh:(t?t->sh:0);
  int trim_y=t?t->ty:0;
  double full_top=dy-(double)trim_y;
  int top=(int)floor(full_top+0.5);
  if(!t || t->sh<=0 || full_h<=0 || trim_y<0 || trim_y+t->sh>full_h ||
     fabs(ys-1.0)>0.001 || fabs(full_top-(double)top)>0.001){
    blit_phase_plane_previous(r,plane,t,dx,dy,xs,ys,blend,alpha);
    return;
  }
  double world_top=full_top+r->cam_y+(sequential?0.0:(double)r->fbh*0.5);
  double edge0=sequential?classic_phase_edge_y_sequential(r,world_top):
                          classic_phase_edge_y(r,world_top);
  double edge1=sequential?classic_phase_edge_y_sequential(r,world_top+(double)full_h):
                          classic_phase_edge_y(r,world_top+(double)full_h);
  double span=edge1-edge0;
  if(fabs(span)<0.001){
    blit_phase_plane_previous(r,plane,t,dx,dy,xs,ys,blend,alpha);
    return;
  }
  int run_q=-1, run_dest=0, run_count=0, previous_q=-2, previous_dest=0;
  for(int k=0;k<full_h;k++){
    int q;
    if(k==0){
      q=0;
    } else {
      double sample=(double)(2*(top+k))-0.5;
      double coord=(sample-edge0)*(double)full_h/span;
      q=reverse_tie?(coord>=(double)k?k-1:k):(coord<(double)k?k-1:k);
    }
    int dest=top+k-1;
    int visible=q>=trim_y && q<trim_y+t->sh;
    if(!visible || (run_count>0 && (q!=previous_q+1 || dest!=previous_dest+1))){
      if(run_count>0){
        GmlTpag rows=phase_tpag_rows(t,run_q-trim_y,run_count);
        blit_phase_plane(r,plane,&rows,dx,(double)run_dest,xs,ys,blend,alpha);
        run_count=0;
      }
    }
    if(visible){
      if(run_count==0){ run_q=q; run_dest=dest; }
      run_count++;
      previous_q=q; previous_dest=dest;
    }
  }
  if(run_count>0){
    GmlTpag rows=phase_tpag_rows(t,run_q-trim_y,run_count);
    blit_phase_plane(r,plane,&rows,dx,(double)run_dest,xs,ys,blend,alpha);
  }
}
/* Every odd output sample of this transform lands exactly on a texel boundary, and coverage there
 * belongs to the preceding texel: output row 2k+1 samples the edge of logical row k+1 and takes
 * row k, which is the row the even sample beside it already took. The plane of odd samples is
 * therefore the base plane, and a quad occupies output rows [2y, 2y+2h).
 *
 * It used to repeat the quad's first row one output row above it and drop its last row instead,
 * which is the following-texel tie at both ends and leaves every quad an output row short at the
 * bottom. Nothing shows that where the row below the quad is empty; where one layer covers
 * another, the layer underneath stays visible along the whole bottom edge. */
static void blit_phase_plane_previous(GmlRender *r, uint32_t *plane, GmlTpag *t,
                                      double dx, double dy, double xs, double ys,
                                      uint32_t blend, double alpha){
  if(!t || t->sh<=0) return;
  blit_phase_plane(r,plane,t,dx,dy,xs,ys,blend,alpha);
}
static void blit_with_phase(GmlRender *r, GmlTpag *t, double dx, double dy,
                            double xs, double ys, uint32_t blend, double alpha,
                            int phase_mode){
  int phase=r && r->target_sp==0 && r->fb==r->base_fb && r->classic_phase_y;
  int interp_phase=r && r->target_sp==0 && r->fb==r->base_fb &&
                   r->classic_interp_phase[0] && r->classic_interp_phase[1] &&
                   r->classic_interp_phase[2];
  blit_one(r,t,dx,dy,xs,ys,blend,alpha);
  if(interp_phase){
    blit_interp_phase_plane(r,r->classic_interp_phase[0],t,dx,dy,xs,ys,blend,alpha,1,0);
    blit_interp_phase_plane(r,r->classic_interp_phase[1],t,dx,dy,xs,ys,blend,alpha,0,1);
    blit_interp_phase_plane(r,r->classic_interp_phase[2],t,dx,dy,xs,ys,blend,alpha,1,1);
  }
  if(!phase) return;
  if(phase_mode==4) phase_mode=classic_phase_ordinary_mode(r);
  /* At exact 2x, odd output samples lie half a logical pixel beyond a centre.  Replaying the
   * textured draw one logical cell earlier gives those tie samples their own alpha composition,
   * rather than shifting the already-composited application surface. */
  if(phase_mode==1)
    blit_phase_plane(r,r->classic_phase_y,t,dx,dy-1.0,xs,ys,blend,alpha);
  else if(phase_mode==2 || phase_mode==3)
    blit_phase_plane_classic_y(r,r->classic_phase_y,t,dx,dy,xs,ys,blend,alpha,
                               phase_mode==3,phase_mode==3);
  else
    blit_phase_plane_previous(r,r->classic_phase_y,t,dx,dy,xs,ys,blend,alpha);
}
void blit(GmlRender *r, GmlTpag *t, double dx, double dy, double xs, double ys,
                 uint32_t blend, double alpha){
  blit_with_phase(r,t,dx,dy,xs,ys,blend,alpha,4);
}
/* A room's background layer is rasterized by the same transform as everything drawn over it, so
 * it answers to the same tie. Pinning the following-texel replay here instead left every layer
 * half a logical row above the sprites sharing its frame. */
static void blit_background_phase(GmlRender *r, GmlTpag *t, double dx, double dy,
                                  double xs, double ys, uint32_t blend, double alpha){
  blit_with_phase(r,t,dx,dy,xs,ys,blend,alpha,4);
}
typedef struct {
  GmlRender *r;
  uint32_t *framebuffer;
  int framebuffer_width;
  GmlTpag *tpag;
  const uint8_t *alpha8;
  const uint32_t *argb;
  const uint16_t *qrow_min, *qrow_max;
  double qx[4], qy[4];
  double ax, ay, cosine, sine, inv_xscale, inv_yscale;
  double local_x_offset, local_y_offset;
  int x0, x1, y0, y1, use_quad_span, vector_pixels;
  int skip_transparent_spans;
  int64_t delta_x, delta_y;
  uint32_t alpha_lut[256];
  uint32_t source;
} GmlSolidMaskRotatedBand;
static void GML_HOT_RENDER solid_mask_alpha8_vector_rows(
    void *context,int row_start,int row_end,int slot){
  GmlSolidMaskRotatedBand *band=(GmlSolidMaskRotatedBand*)context;
  GmlTpag *t=band->tpag;
  (void)slot;
  for(int row_index=row_start;row_index<row_end;row_index++){
    int py=band->y0+row_index;
    int rx0=band->x0,rx1=band->x1;
    if(band->use_quad_span &&
       !rotated_quad_row_span(band->qx,band->qy,py+0.5,band->x0,band->x1,&rx0,&rx1))
      continue;
    uint32_t *row=&band->framebuffer[(size_t)py*band->framebuffer_width];
    double ry=py+0.5-band->ay,rx=rx0+0.5-band->ax;
    double local_x=(rx*band->cosine-ry*band->sine)*band->inv_xscale+
                   band->local_x_offset;
    double local_y=(rx*band->sine+ry*band->cosine)*band->inv_yscale+
                   band->local_y_offset;
    int64_t lx_fp=(int64_t)floor(local_x*(double)RFP_ONE);
    int64_t ly_fp=(int64_t)floor(local_y*(double)RFP_ONE);
    for(int px=rx0;px<rx1;){
      int source_x=floor_fixed20(lx_fp),source_y=floor_fixed20(ly_fp);
      int maxrun=rx1-px;
      if(maxrun>=4){
        uint32_t factors[4]={0,0,0,0};
        if(source_x>=0 && source_y>=0 && source_x<t->sw && source_y<t->sh)
          factors[0]=band->alpha_lut[
            band->alpha8[(size_t)source_y*t->sw+source_x]];
        for(int i=1;i<4;i++){
          int sample_x=floor_fixed20(lx_fp+band->delta_x*(int64_t)i);
          int sample_y=floor_fixed20(ly_fp+band->delta_y*(int64_t)i);
          if(sample_x>=0 && sample_y>=0 && sample_x<t->sw && sample_y<t->sh)
            factors[i]=band->alpha_lut[
              band->alpha8[(size_t)sample_y*t->sw+sample_x]];
        }
        if(factors[0]>=256u && factors[1]>=256u &&
           factors[2]>=256u && factors[3]>=256u)
          fill_u32_run(&row[px],4,band->source);
        else if(factors[0]||factors[1]||factors[2]||factors[3])
          blend_solid_fast8_4(&row[px],band->source,factors);
        lx_fp+=band->delta_x*4;
        ly_fp+=band->delta_y*4;
        px+=4;
        continue;
      }
      int run_x=fixed20_run_to_change(lx_fp,band->delta_x,source_x,maxrun);
      int run_y=fixed20_run_to_change(ly_fp,band->delta_y,source_y,maxrun);
      int run=run_x<run_y?run_x:run_y;
      if(run<1) run=1;
      if(source_x>=0 && source_y>=0 && source_x<t->sw && source_y<t->sh){
        uint32_t alpha=band->alpha_lut[
          band->alpha8[(size_t)source_y*t->sw+source_x]];
        if(alpha) blend_fast8_run(&row[px],run,band->source,alpha);
      }
      lx_fp+=band->delta_x*(int64_t)run;
      ly_fp+=band->delta_y*(int64_t)run;
      px+=run;
    }
  }
}
static void GML_HOT_RENDER solid_mask_rotated_band_rows(
    void *context, int row_start, int row_end, int slot){
  GmlSolidMaskRotatedBand *band=(GmlSolidMaskRotatedBand*)context;
  GmlTpag *t=band->tpag;
  (void)slot;
  for(int row_index=row_start;row_index<row_end;row_index++){
    int py=band->y0+row_index;
    int rx0=band->x0, rx1=band->x1;
    if(band->use_quad_span &&
       !rotated_quad_row_span(band->qx,band->qy,py+0.5,band->x0,band->x1,&rx0,&rx1))
      continue;
    uint32_t *row=&band->framebuffer[(size_t)py*band->framebuffer_width];
    double ry=py+0.5-band->ay, rx=rx0+0.5-band->ax;
    double local_x=(rx*band->cosine-ry*band->sine)*band->inv_xscale+
                   band->local_x_offset;
    double local_y=(rx*band->sine+ry*band->cosine)*band->inv_yscale+
                   band->local_y_offset;
    int64_t lx_fp=(int64_t)floor(local_x*(double)RFP_ONE);
    int64_t ly_fp=(int64_t)floor(local_y*(double)RFP_ONE);
    for(int px=rx0;px<rx1;){
      int ix=floor_fixed20(lx_fp), iy=floor_fixed20(ly_fp);
      int maxrun=rx1-px;
      int skip=0;
      if(band->skip_transparent_spans)
        skip=band->qrow_min
          ? alpha_qspan_skip_run(t,band->qrow_min,band->qrow_max,ix,iy,lx_fp,ly_fp,
                                 band->delta_x,band->delta_y,maxrun)
          : alpha_span_skip_run(
              t,ix,iy,lx_fp,ly_fp,band->delta_x,band->delta_y,maxrun);
      if(skip>0){
        lx_fp+=band->delta_x*(int64_t)skip;
        ly_fp+=band->delta_y*(int64_t)skip;
        px+=skip;
        continue;
      }
      if(band->vector_pixels && maxrun>=4){
        uint32_t factors[4]={0,0,0,0};
        for(int i=0;i<4;i++){
          int source_x=floor_fixed20(lx_fp+band->delta_x*(int64_t)i);
          int source_y=floor_fixed20(ly_fp+band->delta_y*(int64_t)i);
          if(source_x>=0&&source_y>=0&&source_x<t->sw&&source_y<t->sh){
            size_t source_index=(size_t)source_y*t->sw+source_x;
            uint32_t source_alpha=band->alpha8
              ? band->alpha8[source_index] : band->argb[source_index]>>24;
            factors[i]=band->alpha_lut[source_alpha];
          }
        }
        if(factors[0]||factors[1]||factors[2]||factors[3])
          blend_solid_fast8_4(&row[px],band->source,factors);
        lx_fp+=band->delta_x*4;
        ly_fp+=band->delta_y*4;
        px+=4;
        continue;
      }
      int runx=fixed20_run_to_change(lx_fp,band->delta_x,ix,maxrun);
      int runy=fixed20_run_to_change(ly_fp,band->delta_y,iy,maxrun);
      int run=runx<runy?runx:runy;
      if(run<1) run=1;
      if(ix>=0&&iy>=0&&ix<t->sw&&iy<t->sh){
        size_t source_index=(size_t)iy*t->sw+ix;
        uint32_t source_alpha=band->alpha8
          ? band->alpha8[source_index] : band->argb[source_index]>>24;
        uint32_t alpha=band->alpha_lut[source_alpha];
        if(alpha) blend_fast8_run(&row[px],run,band->source,alpha);
      }
      lx_fp+=band->delta_x*(int64_t)run;
      ly_fp+=band->delta_y*(int64_t)run;
      px+=run;
    }
  }
}
typedef struct {
  GmlRender *r;
  uint32_t *framebuffer;
  int framebuffer_width;
  GmlTpag *tpag;
  const uint32_t *draw_cache, *argb;
  const uint16_t *qrow_min, *qrow_max;
  double qx[4], qy[4];
  double ax, ay, cosine, sine, inv_xscale, inv_yscale;
  double local_x_offset, local_y_offset;
  int x0, x1, y0, y1, use_quad_span, white, copy_255, vector_pixels;
  int blend_r, blend_g, blend_b;
  int64_t delta_x, delta_y;
  uint32_t alpha_lut[256];
} GmlCachedRotatedBand;
static void GML_HOT_RENDER cached_rotated_band_rows(
    void *context, int row_start, int row_end, int slot){
  GmlCachedRotatedBand *band=(GmlCachedRotatedBand*)context;
  GmlTpag *t=band->tpag;
  (void)slot;
  for(int row_index=row_start;row_index<row_end;row_index++){
    int py=band->y0+row_index;
    int rx0=band->x0, rx1=band->x1;
    if(band->use_quad_span &&
       !rotated_quad_row_span(band->qx,band->qy,py+0.5,band->x0,band->x1,&rx0,&rx1))
      continue;
    uint32_t *row=&band->framebuffer[(size_t)py*band->framebuffer_width];
    double ry=py+0.5-band->ay, rx=rx0+0.5-band->ax;
    double local_x=(rx*band->cosine-ry*band->sine)*band->inv_xscale+
                   band->local_x_offset;
    double local_y=(rx*band->sine+ry*band->cosine)*band->inv_yscale+
                   band->local_y_offset;
    int64_t lx_fp=(int64_t)floor(local_x*(double)RFP_ONE);
    int64_t ly_fp=(int64_t)floor(local_y*(double)RFP_ONE);
    for(int px=rx0;px<rx1;){
      int ix=floor_fixed20(lx_fp), iy=floor_fixed20(ly_fp);
      int maxrun=rx1-px;
      int skip=band->qrow_min
        ? alpha_qspan_skip_run(t,band->qrow_min,band->qrow_max,ix,iy,lx_fp,ly_fp,
                               band->delta_x,band->delta_y,maxrun)
        : alpha_span_skip_run(t,ix,iy,lx_fp,ly_fp,band->delta_x,band->delta_y,maxrun);
      if(skip>0){
        lx_fp+=band->delta_x*(int64_t)skip;
        ly_fp+=band->delta_y*(int64_t)skip;
        px+=skip;
        continue;
      }
      if(band->vector_pixels && maxrun>=4){
        uint32_t sources[4]={0xFF000000u,0xFF000000u,0xFF000000u,0xFF000000u};
        uint32_t factors[4]={0,0,0,0};
        for(int i=0;i<4;i++){
          int source_x=floor_fixed20(lx_fp+band->delta_x*(int64_t)i);
          int source_y=floor_fixed20(ly_fp+band->delta_y*(int64_t)i);
          if(source_x<0||source_y<0||source_x>=t->sw||source_y>=t->sh) continue;
          size_t source_index=(size_t)source_y*t->sw+source_x;
          uint32_t packed=band->draw_cache
            ? band->draw_cache[source_index] : band->argb[source_index];
          factors[i]=band->draw_cache ? packed>>24 : band->alpha_lut[packed>>24];
          if(band->draw_cache && band->copy_255 && factors[i]==255u) factors[i]=256u;
          if(band->draw_cache || band->white){
            sources[i]=0xFF000000u|(packed&0x00FFFFFFu);
          } else {
            int red=((packed>>16)&255)*band->blend_r/255;
            int green=((packed>>8)&255)*band->blend_g/255;
            int blue=(packed&255)*band->blend_b/255;
            sources[i]=0xFF000000u|((uint32_t)red<<16)|((uint32_t)green<<8)|
                       (uint32_t)blue;
          }
        }
        if(factors[0]||factors[1]||factors[2]||factors[3])
          blend_pixels_fast8_4(&row[px],sources,factors);
        lx_fp+=band->delta_x*4;
        ly_fp+=band->delta_y*4;
        px+=4;
        continue;
      }
      int runx=fixed20_run_to_change(lx_fp,band->delta_x,ix,maxrun);
      int runy=fixed20_run_to_change(ly_fp,band->delta_y,iy,maxrun);
      int run=runx<runy?runx:runy;
      if(run<1) run=1;
      if(ix>=0&&iy>=0&&ix<t->sw&&iy<t->sh){
        size_t source_index=(size_t)iy*t->sw+ix;
        uint32_t packed=band->draw_cache
          ? band->draw_cache[source_index] : band->argb[source_index];
        uint32_t alpha=band->draw_cache ? packed>>24 : band->alpha_lut[packed>>24];
        if(alpha){
          uint32_t source;
          if(band->draw_cache || band->white){
            source=0xFF000000u|(packed&0x00FFFFFFu);
          } else {
            int red=((packed>>16)&255)*band->blend_r/255;
            int green=((packed>>8)&255)*band->blend_g/255;
            int blue=(packed&255)*band->blend_b/255;
            source=0xFF000000u|((uint32_t)red<<16)|((uint32_t)green<<8)|(uint32_t)blue;
          }
          if(band->draw_cache && band->copy_255 && alpha==255u)
            fill_u32_run(&row[px],run,source);
          else
            blend_fast8_run(&row[px],run,source,alpha);
        }
      }
      lx_fp+=band->delta_x*(int64_t)run;
      ly_fp+=band->delta_y*(int64_t)run;
      px+=run;
    }
  }
}
typedef struct {
  uint32_t *framebuffer;
  int framebuffer_width;
  GmlTpag *tpag;
  const uint32_t *argb;
  double qx[4], qy[4];
  double ax, ay, cosine, sine, inv_xscale, inv_yscale;
  double local_x_offset, local_y_offset;
  int x0, x1, y0, y1, use_quad_span, white, vector_pixels, round_blend;
  int blend_r, blend_g, blend_b;
  int64_t delta_x, delta_y;
  uint32_t alpha_lut[256];
} GmlExactRotatedBand;
static void GML_HOT_RENDER exact_rotated_band_rows(
    void *context, int row_start, int row_end, int slot){
  GmlExactRotatedBand *band=(GmlExactRotatedBand*)context;
  GmlTpag *tpag=band->tpag;
  (void)slot;
  for(int row_index=row_start;row_index<row_end;row_index++){
    int py=band->y0+row_index;
    int rx0=band->x0, rx1=band->x1;
    if(band->use_quad_span &&
       !rotated_quad_row_span(band->qx,band->qy,py+0.5,band->x0,band->x1,&rx0,&rx1))
      continue;
    uint32_t *row=&band->framebuffer[(size_t)py*band->framebuffer_width];
    double ry=py+0.5-band->ay, rx=rx0+0.5-band->ax;
    double local_x=(rx*band->cosine-ry*band->sine)*band->inv_xscale+
                   band->local_x_offset;
    double local_y=(rx*band->sine+ry*band->cosine)*band->inv_yscale+
                   band->local_y_offset;
    int64_t lx_fp=(int64_t)floor(local_x*(double)RFP_ONE);
    int64_t ly_fp=(int64_t)floor(local_y*(double)RFP_ONE);
    for(int px=rx0;px<rx1;){
      int source_x=floor_fixed20(lx_fp), source_y=floor_fixed20(ly_fp);
      int maxrun=rx1-px;
      int skip=alpha_span_skip_run(
        tpag,source_x,source_y,lx_fp,ly_fp,band->delta_x,band->delta_y,maxrun);
      if(skip>0){
        lx_fp+=band->delta_x*(int64_t)skip;
        ly_fp+=band->delta_y*(int64_t)skip;
        px+=skip;
        continue;
      }
      if(band->vector_pixels && maxrun>=4){
        uint32_t sources[4]={0xFF000000u,0xFF000000u,0xFF000000u,0xFF000000u};
        uint32_t factors[4]={0,0,0,0};
        for(int i=0;i<4;i++){
          int sample_x=floor_fixed20(lx_fp+band->delta_x*(int64_t)i);
          int sample_y=floor_fixed20(ly_fp+band->delta_y*(int64_t)i);
          if(sample_x<0||sample_y<0||sample_x>=tpag->sw||sample_y>=tpag->sh)
            continue;
          uint32_t packed=band->argb[(size_t)sample_y*tpag->sw+sample_x];
          factors[i]=band->alpha_lut[packed>>24];
          int red=(packed>>16)&255;
          int green=(packed>>8)&255;
          int blue=packed&255;
          if(!band->white){
            red=red*band->blend_r/255;
            green=green*band->blend_g/255;
            blue=blue*band->blend_b/255;
          }
          sources[i]=0xFF000000u|((uint32_t)red<<16)|((uint32_t)green<<8)|
                     (uint32_t)blue;
        }
        if(factors[0]||factors[1]||factors[2]||factors[3]){
          if(band->round_blend)
            blend_pixels_exact16_round_4(&row[px],sources,factors);
          else
            blend_pixels_exact16_4(&row[px],sources,factors);
        }
        lx_fp+=band->delta_x*4;
        ly_fp+=band->delta_y*4;
        px+=4;
        continue;
      }
      int run_x=fixed20_run_to_change(lx_fp,band->delta_x,source_x,maxrun);
      int run_y=fixed20_run_to_change(ly_fp,band->delta_y,source_y,maxrun);
      int run=run_x<run_y?run_x:run_y;
      if(run<1) run=1;
      if(source_x>=0&&source_y>=0&&source_x<tpag->sw&&source_y<tpag->sh){
        uint32_t packed=band->argb[(size_t)source_y*tpag->sw+source_x];
        uint32_t source_alpha=packed>>24;
        uint32_t alpha=band->alpha_lut[source_alpha];
        if(alpha){
          int red=(packed>>16)&255;
          int green=(packed>>8)&255;
          int blue=packed&255;
          if(!band->white){
            red=red*band->blend_r/255;
            green=green*band->blend_g/255;
            blue=blue*band->blend_b/255;
          }
          uint32_t source=0xFF000000u|((uint32_t)red<<16)|((uint32_t)green<<8)|
                          (uint32_t)blue;
          uint32_t *destination=&row[px];
          if(alpha>=65536u){
            fill_u32_run(destination,run,source);
          } else {
            uint32_t inverse=65536u-alpha;
            uint32_t red_product=(uint32_t)red*alpha;
            uint32_t green_product=(uint32_t)green*alpha;
            uint32_t blue_product=(uint32_t)blue*alpha;
            for(int i=0;i<run;i++){
              uint32_t current=destination[i];
              int current_red=(current>>16)&255;
              int current_green=(current>>8)&255;
              int current_blue=current&255;
              uint32_t bias=band->round_blend?32768u:0u;
              destination[i]=0xFF000000u|
                (((red_product+(uint32_t)current_red*inverse+bias)>>16)<<16)|
                (((green_product+(uint32_t)current_green*inverse+bias)>>16)<<8)|
                ((blue_product+(uint32_t)current_blue*inverse+bias)>>16);
            }
          }
        }
      }
      lx_fp+=band->delta_x*(int64_t)run;
      ly_fp+=band->delta_y*(int64_t)run;
      px+=run;
    }
  }
}
typedef struct {
  int kind;
  union {
    GmlSolidMaskRotatedBand solid;
    GmlCachedRotatedBand cached;
    GmlExactRotatedBand exact;
  } draw;
} GmlRotatedBatchItem;
typedef struct {
  GmlRotatedBatchItem *item;
  int count;
  atomic_int next_row;
  int row_count;
  int chunk_rows;
} GmlRotatedBatchRows;
static void GML_HOT_RENDER rotated_batch_rows_range(
    GmlRotatedBatchRows *batch,int row_start,int row_end,int slot){
  for(int i=0;i<batch->count;i++){
    GmlRotatedBatchItem *item=&batch->item[i];
    int y0=item->kind==1?item->draw.solid.y0:
           item->kind==2?item->draw.cached.y0:item->draw.exact.y0;
    int y1=item->kind==1?item->draw.solid.y1:
           item->kind==2?item->draw.cached.y1:item->draw.exact.y1;
    int start=row_start>y0?row_start:y0;
    int end=row_end<y1?row_end:y1;
    if(end<=start) continue;
    if(item->kind==1){
      GmlSolidMaskRotatedBand *solid=&item->draw.solid;
      if(solid->alpha8 && solid->vector_pixels && solid->skip_transparent_spans)
        solid_mask_alpha8_vector_rows(solid,start-y0,end-y0,slot);
      else
        solid_mask_rotated_band_rows(solid,start-y0,end-y0,slot);
    }
    else if(item->kind==2)
      cached_rotated_band_rows(&item->draw.cached,start-y0,end-y0,slot);
    else
      exact_rotated_band_rows(&item->draw.exact,start-y0,end-y0,slot);
  }
}
static void GML_HOT_RENDER rotated_batch_rows(
    void *context,int row_start,int row_end,int slot){
  GmlRotatedBatchRows *batch=(GmlRotatedBatchRows*)context;
  if(batch->chunk_rows<=0){
    rotated_batch_rows_range(batch,row_start,row_end,slot);
    return;
  }
  for(;;){
    int start=atomic_fetch_add_explicit(
      &batch->next_row,batch->chunk_rows,memory_order_relaxed);
    if(start>=batch->row_count) return;
    int end=start+batch->chunk_rows;
    if(end>batch->row_count) end=batch->row_count;
    rotated_batch_rows_range(batch,start,end,slot);
  }
}
void gml_render_flush_rotated_batch(GmlRender *r){
  if(!r || r->rotated_batch_count<=0) return;
  GmlRotatedBatchItem *item=(GmlRotatedBatchItem*)r->rotated_batch;
  int count=r->rotated_batch_count;
  int max_y=0;
  unsigned long long bounds_area=0;
  for(int i=0;i<count;i++){
    int kind=item[i].kind;
    int x0=kind==1?item[i].draw.solid.x0:
           kind==2?item[i].draw.cached.x0:item[i].draw.exact.x0;
    int x1=kind==1?item[i].draw.solid.x1:
           kind==2?item[i].draw.cached.x1:item[i].draw.exact.x1;
    int y0=kind==1?item[i].draw.solid.y0:
           kind==2?item[i].draw.cached.y0:item[i].draw.exact.y0;
    int y1=item[i].kind==1?item[i].draw.solid.y1:
           item[i].kind==2?item[i].draw.cached.y1:item[i].draw.exact.y1;
    if(x1>x0 && y1>y0){
      bounds_area+=(unsigned long long)(x1-x0)*(unsigned long long)(y1-y0);
    }
    if(y1>max_y) max_y=y1;
  }
  r->rotated_batch_count=0;
  if(max_y<=0) return;
  GmlRotatedBatchRows rows;
  memset(&rows,0,sizeof rows);
  rows.item=item;
  rows.count=count;
  int thread_count=1;
  if(max_y>=128){
    if(bounds_area>=1000000ull) thread_count=8;
    else if(bounds_area>=200000ull) thread_count=4;
  }
  if(thread_count>1){
    atomic_init(&rows.next_row,0);
    rows.row_count=max_y;
    rows.chunk_rows=8;
  }
  gml_run_row_bands_n(r,thread_count>1?thread_count:max_y,thread_count,
                      rotated_batch_rows,&rows);
}
static GmlRotatedBatchItem *rotated_batch_append(GmlRender *r,int kind){
  if(!r) return NULL;
  if(r->rotated_batch_count>=r->rotated_batch_capacity){
    int capacity=r->rotated_batch_capacity?r->rotated_batch_capacity*2:32;
    if(capacity>4096) capacity=4096;
    if(capacity<=r->rotated_batch_count) return NULL;
    void *grown=realloc(r->rotated_batch,(size_t)capacity*sizeof(GmlRotatedBatchItem));
    if(!grown) return NULL;
    r->rotated_batch=grown;
    r->rotated_batch_capacity=capacity;
  }
  GmlRotatedBatchItem *item=
    &((GmlRotatedBatchItem*)r->rotated_batch)[r->rotated_batch_count++];
  memset(item,0,sizeof *item);
  item->kind=kind;
  return item;
}
/* One filtered rotated draw: per destination pixel the sprite-local position is mapped back to
 * tpag texels and sampled through the same four-tap kernel the axis-aligned filtered blit uses,
 * so a rotating filtered sprite and a still one quantize identically. */
typedef struct GmlRotInterpBand {
  GmlRender *r; GmlAtlas *atlas; GmlTpag *tpag; GmlSprite *sprite;
  double ax,ay,cosine,sine,inv_xscale,inv_yscale;
  int x0,x1,y0;
  int logical_margin;
  int bR,bG,bB; double alpha; uint32_t blend;
  double qx[4], qy[4]; int use_quad_span;
} GmlRotInterpBand;
static void rot_interp_band_rows(void *context, int row_start, int row_end, int slot){
  GmlRotInterpBand *band=(GmlRotInterpBand*)context;
  (void)slot;
  GmlRender *r=band->r; GmlTpag *t=band->tpag;
  for(int row_index=row_start;row_index<row_end;row_index++){
    int py=band->y0+row_index;
    int rx0=band->x0, rx1=band->x1;
    if(band->use_quad_span &&
       !rotated_quad_row_span(band->qx,band->qy,py+0.5,band->x0,band->x1,&rx0,&rx1))
      continue;
    uint32_t *row=r->fb+(size_t)py*r->fbw;
    double ry=py+0.5-band->ay;
    for(int px=rx0;px<rx1;px++){
      double rx=px+0.5-band->ax;
      double local_x=(rx*band->cosine-ry*band->sine)*band->inv_xscale+band->sprite->originx;
      double local_y=(rx*band->sine+ry*band->cosine)*band->inv_yscale+band->sprite->originy;
      double u=local_x-t->tx-0.5, v=local_y-t->ty-0.5;
      if(u<-1.0 || v<-1.0 || u>=(double)t->sw || v>=(double)t->sh) continue;
      int ua=(int)floor(u), va=(int)floor(v);
      blit_interp_sample(r,&row[px],band->atlas,t,ua,ua+1,va,va+1,
                         u-ua,v-va,band->logical_margin,0,
                         band->bR,band->bG,band->bB,band->blend,band->alpha);
    }
  }
}

void GML_HOT_RENDER blit_rotated(GmlRender *r, GmlSprite *spr, GmlTpag *t, double x, double y,
                         double xs, double ys, double rot, uint32_t blend, double alpha){
  if(t->atlas<0 || t->atlas>=r->n_atlas) return;
  GmlAtlas *a=&r->atlas[t->atlas]; if(!atlas_pixels(r,t->atlas)) return;
  const struct GmlShaderPal *solid_blur=solid_blur_alpha_active(r);
  uint32_t *solid_blur_alpha=solid_blur
    ? tpag_solid_blur_alpha_cache(r,t,a,solid_blur) : NULL;
  int mapped_shader=mapped_texture_active(r) || solid_blur_alpha;
  const struct GmlShaderPal *batchable_solid_mask=solid_alpha_mask_active(r);
  const struct GmlShaderPal *batchable_constant_alpha=
    batchable_solid_mask?batchable_solid_mask:(solid_blur_alpha?solid_blur:NULL);
  int batchable_rotation=!r->classic && !gml_render_target_preserves_alpha(r) &&
                         r->alphablend && r->blendmode==0;
  if(alpha>1) alpha=1; else if(alpha<0) alpha=0;
  if(alpha<=0) return;
  if(xs==0||ys==0) return;
  int bR=blend&0xFF, bG=(blend>>8)&0xFF, bB=(blend>>16)&0xFF;
  double c,sn; render_rotation_sincos(rot,&c,&sn);
  double invxs=1.0/xs, invys=1.0/ys;
  double ax=x-r->cam_x, ay=y-r->cam_y;
  render_modern_cardinal_anchor(r,rot,xs,ys,c,sn,&ax,&ay);
  int abx0=0, aby0=0, abx1=t->sw-1, aby1=t->sh-1;
  if(!solid_blur_alpha &&
     !tpag_alpha_bounds(r,t,a,&abx0,&aby0,&abx1,&aby1)) return;
  double minx=1e30,miny=1e30,maxx=-1e30,maxy=-1e30;
  double qx[4], qy[4];
  double sx0=t->tx+abx0, sy0=t->ty+aby0, sx1=t->tx+abx1+1, sy1=t->ty+aby1+1;
  double corners[4][2]={{sx0,sy0},{sx1,sy0},{sx1,sy1},{sx0,sy1}};
  for(int i=0;i<4;i++){
    double px=(corners[i][0]-spr->originx)*xs, py=(corners[i][1]-spr->originy)*ys;
    double dx=ax + px*c + py*sn, dy=ay - px*sn + py*c;
    qx[i]=dx;
    qy[i]=dy;
    if(dx<minx) minx=dx;
    if(dx>maxx) maxx=dx;
    if(dy<miny) miny=dy;
    if(dy>maxy) maxy=dy;
  }
  int x0=(int)floor(minx)-1, x1=(int)ceil(maxx)+1;
  int y0=(int)floor(miny)-1, y1=(int)ceil(maxy)+1;
  if(x0<0) x0=0;
  if(y0<0) y0=0;
  if(x1>r->fbw) x1=r->fbw;
  if(y1>r->fbh) y1=r->fbh;
  if(x1<=x0 || y1<=y0) return;
  /* Axis-aligned blits materialize a deferred clear before touching the framebuffer. Rotated
   * sprites must do the same: otherwise they are rendered first and then erased when the next
   * axis-aligned draw flushes that pending clear. */
  if(batchable_rotation) r->rotated_batch_building++;
  gml_render_maybe_prepare_draw(r);
  if(batchable_rotation) r->rotated_batch_building--;
  if((alpha<1.0 || r->blendmode==2 || t->alpha_partial || r->interp ||
      mapped_shader) && r->alphablend)
    gml_sprite_target_may_change_alpha(r);
  unsigned long long vispix=(unsigned long long)(x1-x0)*(unsigned long long)(y1-y0);
  /* The global texture filter reaches rotated draws. Recognized-shader masks and
   * mapped textures keep their structural kernels; a plain filtered rotated sprite samples the
   * same four-tap path the axis-aligned filtered blit uses. */
  if(r->interp && r->win && anygm_policy_has_modern_layer_semantics(r->win) &&
     !mapped_shader && !batchable_constant_alpha){
    /* This filtered path paints now, while constant-alpha rotated draws queue in the rotated
     * batch. Anything already queued was issued earlier and must reach the framebuffer first,
     * or a background layer flushed later paints over this sprite. */
    gml_render_flush_rotated_batch(r);
    GmlRotInterpBand band={
      r,a,t,spr,ax,ay,c,sn,invxs,invys,x0,x1,y0,
      t->tx>0 || t->ty>0 || t->tx+t->sw<t->bw || t->ty+t->sh<t->bh,
      bR,bG,bB,alpha,blend,{0},{0},0};
    memcpy(band.qx,qx,sizeof qx); memcpy(band.qy,qy,sizeof qy);
    double qarea_interp=fabs(qx[0]*qy[1]-qx[1]*qy[0] + qx[1]*qy[2]-qx[2]*qy[1] +
                             qx[2]*qy[3]-qx[3]*qy[2] + qx[3]*qy[0]-qx[0]*qy[3]) * 0.5;
    band.use_quad_span=(qarea_interp>0.0 && qarea_interp < (double)vispix * 0.85);
    if(vispix>=262144ull) gml_run_row_bands(r,y1-y0,rot_interp_band_rows,&band);
    else rot_interp_band_rows(&band,0,y1-y0,0);
    return;
  }
  double qarea=fabs(qx[0]*qy[1]-qx[1]*qy[0] + qx[1]*qy[2]-qx[2]*qy[1] +
                    qx[2]*qy[3]-qx[3]*qy[2] + qx[3]*qy[0]-qx[0]*qy[3]) * 0.5;
  int use_quad_span=(qarea>0.0 && qarea < (double)vispix * 0.85);
  double sa_lut[256], ia_lut[256];
  uint32_t a16_lut[256];
  uint32_t a8_lut[256];
  /* Large rotated alpha quads dominate software render cost. For those, blend at
   * framebuffer precision in packed 8-bit lanes; smaller draws keep the 16-bit path. A
   * structurally recognized constant-colour mask has no source-RGB quantization to preserve, so
   * it can enter the same kernel at a smaller area and join adjacent masks in one row dispatch. */
  int fast8_blend = ((gml_blend_family(r)!=GML_BLEND_STUDIO2 || batchable_constant_alpha) &&
                     !gml_render_target_preserves_alpha(r) && r->alphablend &&
                     r->blendmode==0 &&
                     vispix>=(batchable_constant_alpha?4096ull:262144ull));
  int fast8_alpha_floor=fast8_blend ? r->fast_alpha_cull : 0;
  if(fast8_blend && !solid_blur_alpha &&
     (uint32_t)((t->alpha_max/255.0)*alpha*256.0) <=
       (uint32_t)fast8_alpha_floor) return;
  if(fast8_blend){
    for(int i=0;i<256;i++){
      int mapped_alpha=i;
      if(shader_discards_alpha(r,(unsigned)i)) mapped_alpha=0;
      else if(mapped_shader)
        mapped_alpha=(int)(mapped_texture_pixel(r,(uint32_t)i<<24)>>24);
      double sa=(mapped_alpha/255.0)*alpha;
      uint32_t af=sa>=1.0 ? 256u : (uint32_t)(sa*256.0);
      a8_lut[i]=af<=(uint32_t)fast8_alpha_floor ? 0 : af;
    }
  } else {
    for(int i=0;i<256;i++){
      int mapped_alpha=i;
      if(shader_discards_alpha(r,(unsigned)i)) mapped_alpha=0;
      else if(mapped_shader)
        mapped_alpha=(int)(mapped_texture_pixel(r,(uint32_t)i<<24)>>24);
      double sa=(mapped_alpha/255.0)*alpha;
      sa_lut[i]=sa; ia_lut[i]=1.0-sa;
      a16_lut[i]=sa>=1.0 ? 65536u : (uint32_t)(sa*65536.0);
      a8_lut[i]=sa>=1.0 ? 256u : (uint32_t)(sa*256.0);
    }
  }
  int min_fast8_alpha=0;
  const uint16_t *qrow_min=NULL, *qrow_max=NULL;
  if(fast8_blend){
    for(int i=1;i<256;i++) if(a8_lut[i]){ min_fast8_alpha=i; break; }
    if(!min_fast8_alpha) return;
    if(min_fast8_alpha>1 && !solid_blur_alpha)
      tpag_alpha_qrows(r,t,a,min_fast8_alpha,&qrow_min,&qrow_max);
    if(qrow_min && qrow_max){
      int qx0=t->sw, qy0=t->sh, qx1=-1, qy1=-1;
      for(int yy=0; yy<t->sh; yy++){
        int mn=(int)qrow_min[yy], mx=(int)qrow_max[yy];
        if(mn==UINT16_MAX || mx<mn) continue;
        if(mn<qx0) qx0=mn;
        if(mx>qx1) qx1=mx;
        if(yy<qy0) qy0=yy;
        if(yy>qy1) qy1=yy;
      }
      if(qx1<qx0 || qy1<qy0) return;
      if(qx0!=abx0 || qy0!=aby0 || qx1!=abx1 || qy1!=aby1){
        abx0=qx0; aby0=qy0; abx1=qx1; aby1=qy1;
        minx=1e30; miny=1e30; maxx=-1e30; maxy=-1e30;
        sx0=t->tx+abx0; sy0=t->ty+aby0; sx1=t->tx+abx1+1; sy1=t->ty+aby1+1;
        double qc[4][2]={{sx0,sy0},{sx1,sy0},{sx1,sy1},{sx0,sy1}};
        for(int i=0;i<4;i++){
          double px=(qc[i][0]-spr->originx)*xs, py=(qc[i][1]-spr->originy)*ys;
          double dx=ax + px*c + py*sn, dy=ay - px*sn + py*c;
          qx[i]=dx;
          qy[i]=dy;
          if(dx<minx) minx=dx;
          if(dx>maxx) maxx=dx;
          if(dy<miny) miny=dy;
          if(dy>maxy) maxy=dy;
        }
        x0=(int)floor(minx)-1; x1=(int)ceil(maxx)+1;
        y0=(int)floor(miny)-1; y1=(int)ceil(maxy)+1;
        if(x0<0) x0=0;
        if(y0<0) y0=0;
        if(x1>r->fbw) x1=r->fbw;
        if(y1>r->fbh) y1=r->fbh;
        if(x1<=x0 || y1<=y0) return;
        vispix=(unsigned long long)(x1-x0)*(unsigned long long)(y1-y0);
        qarea=fabs(qx[0]*qy[1]-qx[1]*qy[0] + qx[1]*qy[2]-qx[2]*qy[1] +
                   qx[2]*qy[3]-qx[3]*qy[2] + qx[3]*qy[0]-qx[0]*qy[3]) * 0.5;
        use_quad_span=(qarea>0.0 && qarea < (double)vispix * 0.85);
      }
    }
  }
  double dlx=c*invxs, dly=sn*invys;
  int64_t dlx_fp=(int64_t)llround(dlx*(double)RFP_ONE);
  int64_t dly_fp=(int64_t)llround(dly*(double)RFP_ONE);
  int white=((blend & 0xFFFFFF)==0xFFFFFF);
  const uint8_t *tpag_base=NULL;
  if(t->sx>=0 && t->sy>=0 && t->sx+t->sw<=a->w && t->sy+t->sh<=a->h)
    tpag_base=a->px+((size_t)t->sy*a->w+t->sx)*4;
  int fast8_cache_copy_255=0;
  size_t srcpix=(size_t)(t->sw>0?t->sw:0)*(size_t)(t->sh>0?t->sh:0);
  int use_draw_cache=fast8_blend && !mapped_shader && !shader_alpha_test_requires_filter(r) &&
                     rprof_tpag_id(r,t)>=0 && srcpix>0 && srcpix*4u<=vispix;
  uint32_t *draw_cache=use_draw_cache ? tpag_fast8_draw_cache(t,a,blend,alpha,fast8_alpha_floor,&fast8_cache_copy_255) : NULL;
  const struct GmlShaderPal *solid_mask=batchable_solid_mask;
  uint8_t *solid_mask_alpha=fast8_blend && solid_mask
    ? tpag_alpha8_cache(r,t,a) : NULL;
  uint32_t *argb_cache=(fast8_blend && !draw_cache && (!solid_mask || !solid_mask_alpha))
    ? (solid_blur_alpha?solid_blur_alpha:tpag_argb_cache(r,t,a)) : NULL;
  const struct GmlShaderPal *constant_alpha=
    solid_mask?solid_mask:(solid_blur_alpha?solid_blur:NULL);
  if(fast8_blend && (solid_mask_alpha || argb_cache) && constant_alpha){
    GmlSolidMaskRotatedBand band;
    memset(&band,0,sizeof band);
    band.r=r;
    band.framebuffer=r->fb;
    band.framebuffer_width=r->fbw;
    band.tpag=t;
    band.alpha8=solid_mask_alpha;
    band.argb=argb_cache;
    band.qrow_min=qrow_min;
    band.qrow_max=qrow_max;
    memcpy(band.qx,qx,sizeof qx);
    memcpy(band.qy,qy,sizeof qy);
    band.ax=ax;
    band.ay=ay;
    band.cosine=c;
    band.sine=sn;
    band.inv_xscale=invxs;
    band.inv_yscale=invys;
    band.local_x_offset=spr->originx-t->tx;
    band.local_y_offset=spr->originy-t->ty;
    band.x0=x0;
    band.x1=x1;
    band.y0=y0;
    band.y1=y1;
    band.use_quad_span=use_quad_span;
    band.vector_pixels=fabs(dlx)>=0.125 || fabs(dly)>=0.125;
    band.skip_transparent_spans=solid_mask!=NULL;
    band.delta_x=dlx_fp;
    band.delta_y=dly_fp;
    memcpy(band.alpha_lut,a8_lut,sizeof a8_lut);
    uint32_t constant_rgb=solid_mask
      ? solid_mask->solid_alpha_mask_rgb : solid_blur->solid_blur_alpha_rgb;
    /* Recognized solid fragments never read v_vColour; the uniform is the whole tint. */
    int solid_r=(constant_rgb>>16)&255;
    int solid_g=(constant_rgb>>8)&255;
    int solid_b=constant_rgb&255;
    band.source=0xFF000000u|((uint32_t)solid_r<<16)|((uint32_t)solid_g<<8)|
                (uint32_t)solid_b;
    GmlRotatedBatchItem *item=rotated_batch_append(r,1);
    if(item){
      item->draw.solid=band;
      return;
    }
    gml_render_flush_rotated_batch(r);
    gml_run_row_bands(
      r,y1-y0,
      band.alpha8 && band.vector_pixels && band.skip_transparent_spans
        ? solid_mask_alpha8_vector_rows : solid_mask_rotated_band_rows,
      &band);
    return;
  }
  if(fast8_blend && (draw_cache || (argb_cache && !mapped_shader))){
    GmlCachedRotatedBand band;
    memset(&band,0,sizeof band);
    const uint32_t *queued_argb=argb_cache?argb_cache:tpag_argb_cache(r,t,a);
    if(!queued_argb){
      gml_render_flush_rotated_batch(r);
      goto cached_rotated_fallback;
    }
    band.r=r;
    band.framebuffer=r->fb;
    band.framebuffer_width=r->fbw;
    band.tpag=t;
    /* The per-draw cache is reusable storage whose blend/alpha key can change before a deferred
     * batch is flushed. Keep the immutable ARGB source plus this command's LUT and tint instead. */
    band.draw_cache=NULL;
    band.argb=queued_argb;
    band.qrow_min=qrow_min;
    band.qrow_max=qrow_max;
    memcpy(band.qx,qx,sizeof qx);
    memcpy(band.qy,qy,sizeof qy);
    band.ax=ax;
    band.ay=ay;
    band.cosine=c;
    band.sine=sn;
    band.inv_xscale=invxs;
    band.inv_yscale=invys;
    band.local_x_offset=spr->originx-t->tx;
    band.local_y_offset=spr->originy-t->ty;
    band.x0=x0;
    band.x1=x1;
    band.y0=y0;
    band.y1=y1;
    band.use_quad_span=use_quad_span;
    band.white=white;
    band.copy_255=0;
    band.vector_pixels=fabs(dlx)>=0.125 || fabs(dly)>=0.125;
    band.blend_r=bR;
    band.blend_g=bG;
    band.blend_b=bB;
    band.delta_x=dlx_fp;
    band.delta_y=dly_fp;
    memcpy(band.alpha_lut,a8_lut,sizeof a8_lut);
    GmlRotatedBatchItem *item=rotated_batch_append(r,2);
    if(item){
      item->draw.cached=band;
      return;
    }
    gml_render_flush_rotated_batch(r);
    gml_run_row_bands(r,y1-y0,cached_rotated_band_rows,&band);
    return;
  }
  if(!fast8_blend && batchable_rotation && !mapped_shader){
    const uint32_t *immutable_argb=tpag_argb_cache(r,t,a);
    if(immutable_argb){
      GmlExactRotatedBand band;
      memset(&band,0,sizeof band);
      band.framebuffer=r->fb;
      band.framebuffer_width=r->fbw;
      band.tpag=t;
      band.argb=immutable_argb;
      memcpy(band.qx,qx,sizeof qx);
      memcpy(band.qy,qy,sizeof qy);
      band.ax=ax;
      band.ay=ay;
      band.cosine=c;
      band.sine=sn;
      band.inv_xscale=invxs;
      band.inv_yscale=invys;
      band.local_x_offset=spr->originx-t->tx;
      band.local_y_offset=spr->originy-t->ty;
      band.x0=x0;
      band.x1=x1;
      band.y0=y0;
      band.y1=y1;
      band.use_quad_span=use_quad_span;
      band.white=white;
      band.vector_pixels=fabs(dlx)>=0.125 || fabs(dly)>=0.125;
      band.round_blend=gml_blend_family(r)==GML_BLEND_STUDIO2;
      band.blend_r=bR;
      band.blend_g=bG;
      band.blend_b=bB;
      band.delta_x=dlx_fp;
      band.delta_y=dly_fp;
      memcpy(band.alpha_lut,a16_lut,sizeof a16_lut);
      GmlRotatedBatchItem *item=rotated_batch_append(r,3);
      if(item){
        item->draw.exact=band;
        return;
      }
    }
  }
cached_rotated_fallback:
  gml_render_flush_rotated_batch(r);
  if(fast8_blend && (draw_cache || argb_cache)){
    for(int py=y0; py<y1; py++){
      int rx0=x0, rx1=x1;
      if(use_quad_span && !rotated_quad_row_span(qx,qy,py+0.5,x0,x1,&rx0,&rx1)) continue;
      uint32_t *row=&r->fb[(size_t)py*r->fbw];
      double ry=py+0.5-ay, rx=rx0+0.5-ax;
      double lx0=(rx*c - ry*sn)*invxs + spr->originx - t->tx;
      double ly0=(rx*sn + ry*c)*invys + spr->originy - t->ty;
      int64_t lx_fp=(int64_t)floor(lx0*(double)RFP_ONE);
      int64_t ly_fp=(int64_t)floor(ly0*(double)RFP_ONE);
      int ix=floor_fixed20(lx_fp), iy=floor_fixed20(ly_fp);
      int rem=rx1-rx0;
      int runx=fixed20_run_to_change(lx_fp,dlx_fp,ix,rem);
      int runy=fixed20_run_to_change(ly_fp,dly_fp,iy,rem);
      for(int px=rx0; px<rx1; ){
        int maxrun=rx1-px;
        int skip=qrow_min ? alpha_qspan_skip_run(t,qrow_min,qrow_max,ix,iy,lx_fp,ly_fp,dlx_fp,dly_fp,maxrun)
                          : alpha_span_skip_run(t,ix,iy,lx_fp,ly_fp,dlx_fp,dly_fp,maxrun);
        if(skip>0){
          lx_fp += dlx_fp * (int64_t)skip;
          ly_fp += dly_fp * (int64_t)skip;
          px += skip;
          if(px>=rx1) break;
          ix=floor_fixed20(lx_fp);
          iy=floor_fixed20(ly_fp);
          rem=rx1-px;
          runx=fixed20_run_to_change(lx_fp,dlx_fp,ix,rem);
          runy=fixed20_run_to_change(ly_fp,dly_fp,iy,rem);
          continue;
        }
        int run=runx<runy?runx:runy;
        if(run>maxrun) run=maxrun;
        if(run<1) run=1;
        if(ix>=0&&iy>=0&&ix<t->sw&&iy<t->sh){
          size_t si=(size_t)iy*t->sw+ix;
          uint32_t *dp=&row[px];
          if(draw_cache){
            uint32_t packed=draw_cache[si];
            uint32_t af=packed>>24;
            if(af){
              uint32_t src=0xFF000000u|(packed&0x00FFFFFFu);
              if(fast8_cache_copy_255 && af==255u) fill_u32_run(dp,run,src);
              else blend_fast8_run(dp,run,src,af);
            }
          } else {
            uint32_t packed=argb_cache[si];
            if(mapped_shader) packed=mapped_texture_pixel(r,packed);
            int aa=(int)(packed>>24);
            if(aa){
            uint32_t af=a8_lut[aa];
            if(af){
              uint32_t src;
              if(white){
                src=0xFF000000u|(packed&0x00FFFFFFu);
              } else {
                int sr=((packed>>16)&0xFF)*bR/255;
                int sg=((packed>>8)&0xFF)*bG/255;
                int sb=(packed&0xFF)*bB/255;
                src=0xFF000000u|((uint32_t)sr<<16)|((uint32_t)sg<<8)|(uint32_t)sb;
              }
              blend_fast8_run(dp,run,src,af);
            }
          }
          }
        }
        int hitx=(run>=runx);
        int hity=(run>=runy);
        lx_fp += dlx_fp * (int64_t)run;
        ly_fp += dly_fp * (int64_t)run;
        px += run;
        if(px>=rx1) break;
        rem=rx1-px;
        if(hitx){
          ix=floor_fixed20(lx_fp);
          runx=fixed20_run_to_change(lx_fp,dlx_fp,ix,rem);
        } else {
          runx-=run;
        }
        if(hity){
          iy=floor_fixed20(ly_fp);
          runy=fixed20_run_to_change(ly_fp,dly_fp,iy,rem);
        } else {
          runy-=run;
        }
      }
    }
    return;
  }
  if(fast8_blend && tpag_base){
    for(int py=y0; py<y1; py++){
      int rx0=x0, rx1=x1;
      if(use_quad_span && !rotated_quad_row_span(qx,qy,py+0.5,x0,x1,&rx0,&rx1)) continue;
      uint32_t *row=&r->fb[(size_t)py*r->fbw];
      double ry=py+0.5-ay, rx=rx0+0.5-ax;
      double lx0=(rx*c - ry*sn)*invxs + spr->originx - t->tx;
      double ly0=(rx*sn + ry*c)*invys + spr->originy - t->ty;
      int64_t lx_fp=(int64_t)floor(lx0*(double)RFP_ONE);
      int64_t ly_fp=(int64_t)floor(ly0*(double)RFP_ONE);
      for(int px=rx0; px<rx1; ){
        int ix=floor_fixed20(lx_fp), iy=floor_fixed20(ly_fp);
        int maxrun=rx1-px;
        int skip=qrow_min ? alpha_qspan_skip_run(t,qrow_min,qrow_max,ix,iy,lx_fp,ly_fp,dlx_fp,dly_fp,maxrun)
                          : alpha_span_skip_run(t,ix,iy,lx_fp,ly_fp,dlx_fp,dly_fp,maxrun);
        if(skip>0){
          lx_fp += dlx_fp * (int64_t)skip;
          ly_fp += dly_fp * (int64_t)skip;
          px += skip;
          continue;
        }
        int runx=fixed20_run_to_change(lx_fp,dlx_fp,ix,maxrun);
        int runy=fixed20_run_to_change(ly_fp,dly_fp,iy,maxrun);
        int run=runx<runy?runx:runy;
        if(run<1) run=1;
        if(ix>=0&&iy>=0&&ix<t->sw&&iy<t->sh){
          const uint8_t *sp=tpag_base+((size_t)iy*a->w+ix)*4;
          uint32_t packed=((uint32_t)sp[3]<<24)|((uint32_t)sp[0]<<16)|
                          ((uint32_t)sp[1]<<8)|(uint32_t)sp[2];
          if(mapped_shader) packed=mapped_texture_pixel(r,packed);
          int aa=(int)(packed>>24);
          if(aa){
            uint32_t af=a8_lut[aa];
            if(af){
              int mapped_r=(packed>>16)&255,mapped_g=(packed>>8)&255,mapped_b=packed&255;
              int sr=white?mapped_r:mapped_r*bR/255;
              int sg=white?mapped_g:mapped_g*bG/255;
              int sb=white?mapped_b:mapped_b*bB/255;
              uint32_t src=0xFF000000u|((uint32_t)sr<<16)|((uint32_t)sg<<8)|(uint32_t)sb;
              uint32_t *dp=&row[px];
              blend_fast8_run(dp,run,src,af);
            }
          }
        }
        lx_fp += dlx_fp * (int64_t)run;
        ly_fp += dly_fp * (int64_t)run;
        px += run;
      }
    }
    return;
  }
  if(r->blendmode==0){
    for(int py=y0; py<y1; py++){
      int rx0=x0, rx1=x1;
      if(use_quad_span && !rotated_quad_row_span(qx,qy,py+0.5,x0,x1,&rx0,&rx1)) continue;
      uint32_t *row=&r->fb[(size_t)py*r->fbw];
      double ry=py+0.5-ay, rx=rx0+0.5-ax;
      double lx0=(rx*c - ry*sn)*invxs + spr->originx - t->tx;
      double ly0=(rx*sn + ry*c)*invys + spr->originy - t->ty;
      int64_t lx_fp=(int64_t)floor(lx0*(double)RFP_ONE);
      int64_t ly_fp=(int64_t)floor(ly0*(double)RFP_ONE);
      for(int px=rx0; px<rx1; ){
        int ix=floor_fixed20(lx_fp), iy=floor_fixed20(ly_fp);
        int maxrun=rx1-px;
        int skip=qrow_min ? alpha_qspan_skip_run(t,qrow_min,qrow_max,ix,iy,lx_fp,ly_fp,dlx_fp,dly_fp,maxrun)
                          : alpha_span_skip_run(t,ix,iy,lx_fp,ly_fp,dlx_fp,dly_fp,maxrun);
        if(skip>0){
          lx_fp += dlx_fp * (int64_t)skip;
          ly_fp += dly_fp * (int64_t)skip;
          px += skip;
          continue;
        }
        int runx=fixed20_run_to_change(lx_fp,dlx_fp,ix,maxrun);
        int runy=fixed20_run_to_change(ly_fp,dly_fp,iy,maxrun);
        int run=runx<runy?runx:runy;
        if(run<1) run=1;
        if(ix>=0&&iy>=0&&ix<t->sw&&iy<t->sh){
          const uint8_t *sp=tpag_base ? tpag_base+((size_t)iy*a->w+ix)*4 : NULL;
          if(!sp){
            int sx=t->sx+ix, sy=t->sy+iy;
            if(sx>=0&&sy>=0&&sx<a->w&&sy<a->h) sp=a->px + ((size_t)sy*a->w+sx)*4;
          }
          if(sp){
            uint32_t packed=((uint32_t)sp[3]<<24)|((uint32_t)sp[0]<<16)|
                            ((uint32_t)sp[1]<<8)|(uint32_t)sp[2];
            if(mapped_shader) packed=mapped_texture_pixel(r,packed);
            int aa=(int)(packed>>24);
            if(aa){
              int mapped_r=(packed>>16)&255,mapped_g=(packed>>8)&255,mapped_b=packed&255;
              int sr=white?mapped_r:mapped_r*bR/255;
              int sg=white?mapped_g:mapped_g*bG/255;
              int sb=white?mapped_b:mapped_b*bB/255;
              uint32_t src=0xFF000000u|((uint32_t)sr<<16)|((uint32_t)sg<<8)|(uint32_t)sb;
              uint32_t *dp=&row[px];
              if(!r->alphablend){
                fill_u32_run(dp,run,src);
              } else {
                if(fast8_blend){
                  uint32_t af=a8_lut[aa];
                  if(af) blend_fast8_run(dp,run,src,af);
                } else {
                  uint32_t af=a16_lut[aa];
                  if(af>=65536u){
                    fill_u32_run(dp,run,src);
                  } else if(af){
                    uint32_t ia=65536u-af;
                    uint32_t srp=(uint32_t)sr*af, sgp=(uint32_t)sg*af, sbp=(uint32_t)sb*af;
                    for(int k=0;k<run;k++){
                      uint32_t dv=dp[k];
                      int dr=(dv>>16)&0xFF, dg=(dv>>8)&0xFF, db=dv&0xFF;
                      unsigned source_alpha=(unsigned)lround((double)aa*alpha);
                      dp[k]=gml_sprite_target_alpha(r,dv,source_alpha)|
                            (((srp+dr*ia)>>16)<<16)|(((sgp+dg*ia)>>16)<<8)|
                            ((sbp+db*ia)>>16);
                    }
                  }
                }
              }
            }
          }
        }
        lx_fp += dlx_fp * (int64_t)run;
        ly_fp += dly_fp * (int64_t)run;
        px += run;
      }
    }
    return;
  }
  for(int py=y0; py<y1; py++){
    int rx0=x0, rx1=x1;
    if(use_quad_span && !rotated_quad_row_span(qx,qy,py+0.5,x0,x1,&rx0,&rx1)) continue;
    uint32_t *row=&r->fb[(size_t)py*r->fbw];
    double ry=py+0.5-ay, rx=rx0+0.5-ax;
    double lx0=(rx*c - ry*sn)*invxs + spr->originx - t->tx;
    double ly0=(rx*sn + ry*c)*invys + spr->originy - t->ty;
    int64_t lx_fp=(int64_t)floor(lx0*(double)RFP_ONE);
    int64_t ly_fp=(int64_t)floor(ly0*(double)RFP_ONE);
    for(int px=rx0; px<rx1; px++, lx_fp+=dlx_fp, ly_fp+=dly_fp){
      int ix=floor_fixed20(lx_fp), iy=floor_fixed20(ly_fp);
      if(ix<0||iy<0||ix>=t->sw||iy>=t->sh) continue;
      int sx=t->sx+ix, sy=t->sy+iy;
      if(sx<0||sy<0||sx>=a->w||sy>=a->h) continue;
      uint8_t *sp=a->px + ((size_t)sy*a->w+sx)*4;
      uint32_t packed=((uint32_t)sp[3]<<24)|((uint32_t)sp[0]<<16)|
                      ((uint32_t)sp[1]<<8)|(uint32_t)sp[2];
      if(mapped_shader) packed=mapped_texture_pixel(r,packed);
      int aa=(int)(packed>>24); if(!aa) continue;
      int mapped_r=(packed>>16)&255,mapped_g=(packed>>8)&255,mapped_b=packed&255;
      uint32_t *dp=&row[px];
      if(!r->alphablend){
        int sr=white?mapped_r:mapped_r*bR/255;
        int sg=white?mapped_g:mapped_g*bG/255;
        int sb=white?mapped_b:mapped_b*bB/255;
        *dp=0xFF000000u|(sr<<16)|(sg<<8)|sb; continue;
      }
      uint32_t destination=*dp;
      int dr=(destination>>16)&0xFF, dg=(destination>>8)&0xFF, db=destination&0xFF;
      if(r->blendmode==0 && white){
        uint32_t af=a16_lut[aa]; if(!af) continue;
        if(af>=65536u){
          *dp=0xFF000000u|((uint32_t)mapped_r<<16)|((uint32_t)mapped_g<<8)|
              (uint32_t)mapped_b;
          continue;
        }
        uint32_t ia=65536u-af;
        unsigned source_alpha=(unsigned)lround((double)aa*alpha);
        *dp=gml_sprite_target_alpha(r,destination,source_alpha)|
            (((mapped_r*af+dr*ia)>>16)<<16)|(((mapped_g*af+dg*ia)>>16)<<8)|
            ((mapped_b*af+db*ia)>>16);
        continue;
      }
      int sr=mapped_r*bR/255, sg=mapped_g*bG/255, sb=mapped_b*bB/255;
      if(r->blendmode==0){
        uint32_t af=a16_lut[aa]; if(!af) continue;
        if(af>=65536u){ *dp=0xFF000000u|(sr<<16)|(sg<<8)|sb; continue; }
        uint32_t ia=65536u-af;
        unsigned source_alpha=(unsigned)lround((double)aa*alpha);
        *dp=gml_sprite_target_alpha(r,destination,source_alpha)|
            (((sr*af+dr*ia)>>16)<<16)|(((sg*af+dg*ia)>>16)<<8)|
            ((sb*af+db*ia)>>16);
        continue;
      }
      double sa=sa_lut[aa]; if(sa<=0) continue;
      if(r->blendmode==1){   /* bm_add: dst += src*srcAlpha */
        int ar=dr+(int)(sr*sa); if(ar>255)ar=255; int ag=dg+(int)(sg*sa); if(ag>255)ag=255;
        int ab=db+(int)(sb*sa); if(ab>255)ab=255; *dp=0xFF000000u|(ar<<16)|(ag<<8)|ab; continue; }
      if(r->blendmode==2){   /* (bm_zero,bm_inv_src_colour), including inverse source alpha. */
        unsigned ar=gml_blend_inv_source_u8((unsigned)dr,(unsigned)sr);
        unsigned ag=gml_blend_inv_source_u8((unsigned)dg,(unsigned)sg);
        unsigned ab=gml_blend_inv_source_u8((unsigned)db,(unsigned)sb);
        uint32_t coverage=gml_sprite_target_inverse_alpha(r,*dp,(double)aa*alpha);
        *dp=coverage|(ar<<16)|(ag<<8)|ab; continue; }
      if(r->blendmode==4){
        unsigned source_alpha=(unsigned)lround((double)aa*alpha);
        *dp=color_write_merge(r,*dp,
          blend_max_preset_pixel(r,*dp,sr,sg,sb,source_alpha));
        continue;
      }
      double ia=ia_lut[aa];
      int or_=(int)(sr*sa+dr*ia); if(or_>255) or_=255; else if(or_<0) or_=0;
      int og=(int)(sg*sa+dg*ia); if(og>255) og=255; else if(og<0) og=0;
      int ob=(int)(sb*sa+db*ia); if(ob>255) ob=255; else if(ob<0) ob=0;
      unsigned source_alpha=(unsigned)lround((double)aa*alpha);
      *dp=gml_sprite_target_alpha(r,destination,source_alpha)|(or_<<16)|(og<<8)|ob;
    }
  }
}

static void blit_rotated_plane(GmlRender *r, uint32_t *plane, GmlSprite *spr, GmlTpag *t,
                               double x, double y, double xs, double ys, double rot,
                               uint32_t blend, double alpha){
  uint32_t *saved_fb=r->fb;
  int saved_known=r->fb_opaque_known;
  int saved_opaque=r->fb_all_opaque;
  int saved_transparent=r->fb_all_transparent;
  r->fb=plane;
  r->fb_opaque_known=0;
  r->fb_all_opaque=0;
  r->fb_all_transparent=0;
  blit_rotated(r,spr,t,x,y,xs,ys,rot,blend,alpha);
  r->fb=saved_fb;
  r->fb_opaque_known=saved_known;
  r->fb_all_opaque=saved_opaque;
  r->fb_all_transparent=saved_transparent;
}

static void blit_rotated_with_phase(GmlRender *r, GmlSprite *spr, GmlTpag *t,
                                    double x, double y, double xs, double ys, double rot,
                                    uint32_t blend, double alpha){
  int phase=r && r->target_sp==0 && r->fb==r->base_fb && r->classic_phase_y;
  int interp_phase=r && r->target_sp==0 && r->fb==r->base_fb &&
                   r->classic_interp_phase[0] && r->classic_interp_phase[1] &&
                   r->classic_interp_phase[2];
  blit_rotated(r,spr,t,x,y,xs,ys,rot,blend,alpha);
  /* Exact 2x classic presentation selects auxiliary samples after the application surface has
   * been composed.  Keep rotated draws in those planes too, so an otherwise valid base-plane
   * raster cannot disappear from the selected output rows or interpolation quadrants. */
  if(interp_phase)
    for(int q=0;q<3;q++)
      blit_rotated_plane(r,r->classic_interp_phase[q],spr,t,x,y,xs,ys,rot,blend,alpha);
  if(phase)
    blit_rotated_plane(r,r->classic_phase_y,spr,t,x,y,xs,ys,rot,blend,alpha);
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
    blit_tpag_part(r,t,sx,sy,sw,sh,dx,dy,(double)tw/sw,(double)th/sh,blend,alpha);
    return;
  }
  for(int oy=0; oy<th; oy+=sh){                          /* repeat (also mirror/blankrepeat) */
    int ch=sh<th-oy? sh : th-oy;
    for(int ox=0; ox<tw; ox+=sw){
      int cw=sw<tw-ox? sw : tw-ox;
      blit_tpag_part(r,t,sx,sy,cw,ch,dx+ox,dy+oy,1,1,blend,alpha);
    }
  }
}
/* draw a nine-slice sprite frame into the W x H target box at (dx0,dy0) (camera applied).
 * Corners stay native-size; edges and center follow their tile modes. */
static void draw_sprite_nineslice(GmlRender *r, GmlSprite *s, GmlTpag *t,
                                  double dx0, double dy0, int W, int H,
                                  uint32_t blend, double alpha){
  int sw=s->w, sh=s->h;
  int l=s->ns_l, rt=s->ns_r, tp=s->ns_t, bt=s->ns_b;
  int cw=sw-l-rt, chh=sh-tp-bt;      /* source center band sizes */
  int tw=W-l-rt, th=H-tp-bt;         /* target center band sizes */
  /* corners (never scaled) */
  blit_tpag_part(r,t,0,0,l,tp,          dx0,        dy0,        1,1,blend,alpha);
  blit_tpag_part(r,t,sw-rt,0,rt,tp,     dx0+W-rt,   dy0,        1,1,blend,alpha);
  blit_tpag_part(r,t,0,sh-bt,l,bt,      dx0,        dy0+H-bt,   1,1,blend,alpha);
  blit_tpag_part(r,t,sw-rt,sh-bt,rt,bt, dx0+W-rt,   dy0+H-bt,   1,1,blend,alpha);
  /* edges: top(1), bottom(3), left(0), right(2) — tile mode indices per the SPRT record */
  ns_band(r,t,s->ns_tile[1], l,0,cw,tp,      dx0+l,      dy0,        tw,tp, blend,alpha);
  ns_band(r,t,s->ns_tile[3], l,sh-bt,cw,bt,  dx0+l,      dy0+H-bt,   tw,bt, blend,alpha);
  ns_band(r,t,s->ns_tile[0], 0,tp,l,chh,     dx0,        dy0+tp,     l,th,  blend,alpha);
  ns_band(r,t,s->ns_tile[2], sw-rt,tp,rt,chh,dx0+W-rt,   dy0+tp,     rt,th, blend,alpha);
  /* center */
  ns_band(r,t,s->ns_tile[4], l,tp,cw,chh,    dx0+l,      dy0+tp,     tw,th, blend,alpha);
}

static int sprite_pos_triangle(double px,double py,
                               const double x[3],const double y[3],
                               const double u[3],const double v[3],
                               double *out_u,double *out_v){
  double den=(y[1]-y[2])*(x[0]-x[2])+(x[2]-x[1])*(y[0]-y[2]);
  if(!isfinite(den)||fabs(den)<1e-12) return 0;
  double a=((y[1]-y[2])*(px-x[2])+(x[2]-x[1])*(py-y[2]))/den;
  double b=((y[2]-y[0])*(px-x[2])+(x[0]-x[2])*(py-y[2]))/den;
  double c=1.0-a-b;
  if(a < -1e-9 || b < -1e-9 || c < -1e-9) return 0;
  *out_u=a*u[0]+b*u[1]+c*u[2];
  *out_v=a*v[0]+b*v[1]+c*v[2];
  return 1;
}

static void sprite_pos_sample(const uint8_t *rgba,int stride,int ox,int oy,int sw,int sh,
                              double u,double v,int interpolate,double out[4]){
  if(u<0) u=0; else if(u>1) u=1;
  if(v<0) v=0; else if(v>1) v=1;
  if(!interpolate){
    int sx=(int)floor(u*sw),sy=(int)floor(v*sh);
    if(sx>=sw) sx=sw-1;
    if(sy>=sh) sy=sh-1;
    const uint8_t *p=rgba+((size_t)(oy+sy)*stride+ox+sx)*4;
    for(int channel=0;channel<4;channel++) out[channel]=p[channel];
    return;
  }
  double fx=u*sw-.5,fy=v*sh-.5;
  int x0=(int)floor(fx),y0=(int)floor(fy);
  double ax=fx-x0,ay=fy-y0;
  int x1=x0+1,y1=y0+1;
  if(x0<0) x0=0; else if(x0>=sw) x0=sw-1;
  if(x1<0) x1=0; else if(x1>=sw) x1=sw-1;
  if(y0<0) y0=0; else if(y0>=sh) y0=sh-1;
  if(y1<0) y1=0; else if(y1>=sh) y1=sh-1;
  const uint8_t *p00=rgba+((size_t)(oy+y0)*stride+ox+x0)*4;
  const uint8_t *p10=rgba+((size_t)(oy+y0)*stride+ox+x1)*4;
  const uint8_t *p01=rgba+((size_t)(oy+y1)*stride+ox+x0)*4;
  const uint8_t *p11=rgba+((size_t)(oy+y1)*stride+ox+x1)*4;
  for(int channel=0;channel<4;channel++)
    out[channel]=p00[channel]*(1-ax)*(1-ay)+p10[channel]*ax*(1-ay)+
                 p01[channel]*(1-ax)*ay+p11[channel]*ax*ay;
}

static void sprite_pos_pixel(GmlRender *r,int x,int y,const double sample[4],double alpha){
  double sa=sample[3]*alpha/255.0;
  if(sa<=0) return;
  int sr=(int)lround(sample[0]),sg=(int)lround(sample[1]),sb=(int)lround(sample[2]);
  uint32_t *dst=&r->fb[(size_t)y*r->fbw+x];
  if(!r->alphablend){ *dst=0xFF000000u|((uint32_t)sr<<16)|((uint32_t)sg<<8)|(uint32_t)sb; return; }
  int dr=(*dst>>16)&255,dg=(*dst>>8)&255,db=*dst&255;
  if(r->blendmode==1){
    int rr=dr+(int)(sr*sa),gg=dg+(int)(sg*sa),bb=db+(int)(sb*sa);
    if(rr>255) rr=255;
    if(gg>255) gg=255;
    if(bb>255) bb=255;
    *dst=0xFF000000u|((uint32_t)rr<<16)|((uint32_t)gg<<8)|(uint32_t)bb;
    return;
  }
  if(r->blendmode==2){
    unsigned rr=gml_blend_inv_source_u8((unsigned)dr,(unsigned)sr);
    unsigned gg=gml_blend_inv_source_u8((unsigned)dg,(unsigned)sg);
    unsigned bb=gml_blend_inv_source_u8((unsigned)db,(unsigned)sb);
    uint32_t coverage=gml_sprite_target_inverse_alpha(r,*dst,sample[3]*alpha);
    *dst=coverage|(rr<<16)|(gg<<8)|bb;
    return;
  }
  if(r->blendmode==4){
    unsigned source_alpha=(unsigned)lround(sample[3]*alpha);
    *dst=color_write_merge(r,*dst,
      blend_max_preset_pixel(r,*dst,sr,sg,sb,source_alpha));
    return;
  }
  if(sa>1) sa=1;
  double inv=1-sa;
  int rr=(int)(sr*sa+dr*inv),gg=(int)(sg*sa+dg*inv),bb=(int)(sb*sa+db*inv);
  unsigned source_alpha=(unsigned)lround(sample[3]*alpha);
  *dst=gml_sprite_target_alpha(r,*dst,source_alpha)|((uint32_t)rr<<16)|
       ((uint32_t)gg<<8)|(uint32_t)bb;
}

void gml_draw_sprite_pos(GmlRender *r,int sprite,int subimg,
                         const double x[4],const double y[4],double alpha){
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
  GmlSprite *s=&r->spr[sprite]; if(s->n_frames<=0) return;
  const uint8_t *rgba=NULL; int stride=0,ox=0,oy=0,sw=0,sh=0;
  GmlTpag *page=NULL;
  if(s->runtime_rgba){
    rgba=runtime_frame_rgba(s,subimg); stride=sw=s->w; sh=s->h;
  } else {
    int frame=((subimg%s->n_frames)+s->n_frames)%s->n_frames;
    int page_id=s->frame[frame]; if(page_id<0||page_id>=r->n_tpag) return;
    page=&r->tpag[page_id];
    if(page->atlas<0||page->atlas>=r->n_atlas||!atlas_pixels(r,page->atlas)) return;
    GmlAtlas *atlas=&r->atlas[page->atlas];
    rgba=atlas->px; stride=atlas->w; ox=page->sx; oy=page->sy; sw=page->sw; sh=page->sh;
  }
  if(!rgba||stride<=0||sw<=0||sh<=0) return;

  double qx[4],qy[4],shift=r->classic?-.5:0;
  for(int i=0;i<4;i++){ qx[i]=x[i]-r->cam_x+shift; qy[i]=y[i]-r->cam_y+shift; }
  /* Most calls are ordinary rectangles. Preserve the established axis blitters for this common
   * case and reserve barycentric work for genuinely distorted quads. */
  if(!r->classic && qx[1]>qx[0] && qy[3]>qy[0] &&
     fabs(qy[0]-qy[1])<1e-9 && fabs(qx[1]-qx[2])<1e-9 &&
     fabs(qy[2]-qy[3])<1e-9 && fabs(qx[3]-qx[0])<1e-9){
    if(page) blit(r,page,qx[0],qy[0],(qx[1]-qx[0])/sw,(qy[3]-qy[0])/sh,0xFFFFFF,alpha);
    else blit_rgba_region(r,rgba,sw,sh,0,0,sw,sh,qx[0],qy[0],qx[1]-qx[0],qy[3]-qy[0],0xFFFFFF,alpha);
    return;
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
  if(x1<x0||y1<y0) return;
  static const double uvx[4]={0,1,1,0},uvy[4]={0,0,1,1};
  const int tri[2][3]={{0,1,2},{0,2,3}};
  double tx[2][3],ty[2][3],tu[2][3],tv[2][3];
  for(int part=0;part<2;part++) for(int k=0;k<3;k++){
    int index=tri[part][k];
    tx[part][k]=qx[index]; ty[part][k]=qy[index];
    tu[part][k]=uvx[index]; tv[part][k]=uvy[index];
  }
  gml_render_maybe_prepare_draw(r);
  for(int py=y0;py<=y1;py++) for(int px=x0;px<=x1;px++){
    double u=0,v=0; int hit=0;
    for(int part=0;part<2&&!hit;part++)
      hit=sprite_pos_triangle(px+.5,py+.5,tx[part],ty[part],tu[part],tv[part],&u,&v);
    if(hit){ double sample[4]; sprite_pos_sample(rgba,stride,ox,oy,sw,sh,u,v,r->interp,sample); sprite_pos_pixel(r,px,py,sample,alpha); }
  }
}

static void draw_spine_item(GmlRender *r,const GmlSpineDrawItem *item,
                            uint32_t blend,double alpha){
  if(!r || !item || item->texture_page<0 || item->texture_page>=r->n_tpag) return;
  GmlTpag *page=&r->tpag[item->texture_page];
  if(page->atlas<0 || page->atlas>=r->n_atlas || !atlas_pixels(r,page->atlas)) return;
  GmlAtlas *atlas=&r->atlas[page->atlas];
  if(!atlas->px || atlas->w<=0 || atlas->h<=0) return;
  double qx[4],qy[4],minx=0,maxx=0,miny=0,maxy=0;
  for(int i=0;i<4;i++){
    qx[i]=item->x[i]-r->cam_x;
    qy[i]=item->y[i]-r->cam_y;
    if(!isfinite(qx[i]) || !isfinite(qy[i])) return;
    if(!i) minx=maxx=qx[i],miny=maxy=qy[i];
    else {
      if(qx[i]<minx) minx=qx[i];
      if(qx[i]>maxx) maxx=qx[i];
      if(qy[i]<miny) miny=qy[i];
      if(qy[i]>maxy) maxy=qy[i];
    }
  }
  int x0=(int)floor(minx),x1=(int)ceil(maxx)-1;
  int y0=(int)floor(miny),y1=(int)ceil(maxy)-1;
  if(x0<0) x0=0;
  if(y0<0) y0=0;
  if(x1>=r->fbw) x1=r->fbw-1;
  if(y1>=r->fbh) y1=r->fbh-1;
  if(x1<x0 || y1<y0) return;
  const int triangle[2][3]={{0,1,2},{0,2,3}};
  double tx[2][3],ty[2][3],tu[2][3],tv[2][3];
  for(int part=0;part<2;part++) for(int k=0;k<3;k++){
    int index=triangle[part][k];
    tx[part][k]=qx[index]; ty[part][k]=qy[index];
    tu[part][k]=item->u[index]; tv[part][k]=item->v[index];
  }
  unsigned br=blend&255u,bg=(blend>>8)&255u,bb=(blend>>16)&255u;
  unsigned ir=item->colour&255u,ig=(item->colour>>8)&255u,ib=(item->colour>>16)&255u;
  double effective_alpha=alpha*item->alpha;
  gml_render_maybe_prepare_draw(r);
  for(int py=y0;py<=y1;py++) for(int px=x0;px<=x1;px++){
    double u=0,v=0; int hit=0;
    for(int part=0;part<2&&!hit;part++)
      hit=sprite_pos_triangle(px+.5,py+.5,tx[part],ty[part],tu[part],tv[part],&u,&v);
    if(hit){
      double sample[4];
      sprite_pos_sample(atlas->px,atlas->w,0,0,atlas->w,atlas->h,
                        u,v,r->interp,sample);
      sample[0]*=(double)(br*ir)/(255.0*255.0);
      sample[1]*=(double)(bg*ig)/(255.0*255.0);
      sample[2]*=(double)(bb*ib)/(255.0*255.0);
      sprite_pos_pixel(r,px,py,sample,effective_alpha);
    }
  }
}

static int draw_spine_sprite(GmlRender *r,GmlSprite *sprite,double x,double y,
                             double xs,double ys,double rotation,
                             uint32_t blend,double alpha){
  if(!r || !sprite || !sprite->spine) return 0;
  int page_id=sprite->spine->texture_page;
  if(page_id<0 || page_id>=r->n_tpag) return 1;
  int atlas_id=r->tpag[page_id].atlas;
  if(atlas_id<0 || atlas_id>=r->n_atlas || !atlas_pixels(r,atlas_id)) return 1;
  GmlSpineDrawItem items[256];
  int count=gml_render_spine_build_items(
    r,sprite,x,y,xs,ys,rotation,items,256);
  gml_render_flush_rotated_batch(r);
  for(int i=0;i<count;i++) draw_spine_item(r,&items[i],blend,alpha);
  return 1;
}

typedef struct {
  uint32_t *framebuffer;
  size_t pixel_count;
  uint8_t mask;
  int active;
} GmlMaskedSpriteDraw;

static GmlMaskedSpriteDraw masked_sprite_draw_begin(GmlRender *r){
  GmlMaskedSpriteDraw draw={0};
  if(!r || !r->fb || r->fbw<=0 || r->fbh<=0 || r->color_write_mask==0x0F ||
     gml_d3_is_active(r)) return draw;
  gml_render_flush_rotated_batch(r);
  if(r->pending_underlay || r->pending_fill) gml_render_prepare_draw(r);
  size_t pixels=(size_t)r->fbw*(size_t)r->fbh;
  if(pixels>SIZE_MAX/sizeof(uint32_t)) return draw;
  if(r->color_write_scratch_capacity<pixels){
    uint32_t *grown=realloc(r->color_write_scratch,pixels*sizeof(uint32_t));
    if(!grown) return draw;
    r->color_write_scratch=grown;
    r->color_write_scratch_capacity=pixels;
  }
  memcpy(r->color_write_scratch,r->fb,pixels*sizeof(uint32_t));
  draw.framebuffer=r->fb;
  draw.pixel_count=pixels;
  draw.mask=r->color_write_mask;
  draw.active=1;
  r->color_write_mask=0x0F;
  return draw;
}

static void masked_sprite_draw_end(GmlRender *r,GmlMaskedSpriteDraw draw){
  if(!r || !draw.active) return;
  gml_render_flush_rotated_batch(r);
  r->color_write_mask=draw.mask;
  if(r->fb==draw.framebuffer){
    for(size_t index=0;index<draw.pixel_count;index++)
      r->fb[index]=color_write_merge(r,r->color_write_scratch[index],r->fb[index]);
    r->fb_opaque_known=0;
    r->fb_all_opaque=0;
    r->fb_all_transparent=0;
  }
}

static void draw_sprite_ext_unmasked(GmlRender *r, int sprite, int subimg, double x, double y,
                                     double xs, double ys, double rot, uint32_t blend, double alpha){
  gml_render_draw_map_point(r,&x,&y);
  gml_render_draw_map_scale(r,&xs,&ys);
  if(r && sprite>=0 && sprite<r->n_spr &&
     draw_spine_sprite(r,&r->spr[sprite],x,y,xs,ys,rot,blend,alpha)) return;
  if(gml_d3_draw_sprite_2d(r,sprite,subimg,x,y,xs,ys,rot,blend,alpha)) return;
  if(sprite<0||sprite>=r->n_spr) return;
  if(alpha<=0) return;
  GmlSprite *s=&r->spr[sprite]; if(s->n_frames<=0) return;
  int sprof=sprof_enabled();
  double sprof_t0=sprof?rprof_now():0.0;
  if(s->runtime_rgba){
    int sub=runtime_frame_index(s,subimg);
    if(log_spr_enabled(r))
      anygm_host_logf(r && r->win ? r->win->host : NULL,ANYGM_LOG_DEBUG,"[spr] %s subimg=%d sub=%d runtime=1 w,h=%d,%d x=%.0f y=%.0f xs=%.3f ys=%.3f rot=%.3f blend=%06X a=%.3f\n",
        s->name?s->name:"?",subimg,sub,s->w,s->h,x,y,xs,ys,rot,(unsigned)(blend&0xffffff),alpha);
    const uint8_t *fr=runtime_frame_rgba(s,subimg); if(!fr) return;
    const int *rmin=s->runtime_row_min?s->runtime_row_min+(size_t)sub*s->h:NULL;
    const int *rmax=s->runtime_row_max?s->runtime_row_max+(size_t)sub*s->h:NULL;
    blit_rgba_sprite(r,s,fr,s->w,s->h,x,y,xs,ys,rot,s->originx,s->originy,blend,alpha,1,rmin,rmax,s->runtime_opaque);
    if(sprof) sprof_add(sprite,s->name,(rprof_now()-sprof_t0)*1000.0);
    return;
  }
  int sub = s->n_frames? ((subimg%s->n_frames)+s->n_frames)%s->n_frames : 0;
  int ti=s->frame[sub]; if(ti<0||ti>=r->n_tpag){ if(sprof) sprof_add(sprite,s->name,(rprof_now()-sprof_t0)*1000.0); return; }
  GmlTpag *t=&r->tpag[ti];
  if(log_spr_enabled(r)){
    GmlAtlas *atlas=t->atlas>=0&&t->atlas<r->n_atlas?&r->atlas[t->atlas]:NULL;
    int bx0=0,by0=0,bx1=-1,by1=-1;
    int has_pixels=atlas&&atlas_pixels(r,t->atlas)!=NULL;
    int has_alpha=has_pixels&&tpag_alpha_bounds(r,t,atlas,&bx0,&by0,&bx1,&by1);
    anygm_host_logf(r && r->win ? r->win->host : NULL,ANYGM_LOG_DEBUG,"[spr] %s subimg=%d sub=%d tpag=%d src=%d,%d %dx%d dst=%d,%d base=%dx%d origin=%d,%d atlas=%d pixels=%d alpha=%d bbox=%d,%d-%d,%d x=%.0f y=%.0f xs=%.3f ys=%.3f rot=%.3f blend=%06X a=%.3f\n",
      s->name?s->name:"?",subimg,sub,ti,t->sx,t->sy,t->sw,t->sh,t->tx,t->ty,t->bw,t->bh,
      s->originx,s->originy,t->atlas,has_pixels,has_alpha,bx0,by0,bx1,by1,
      x,y,xs,ys,rot,(unsigned)(blend&0xffffff),alpha);
  }
  /* draw at (x - origin)*scale + target offset, minus camera */
  double dx = x - s->originx*xs + t->tx*xs - r->cam_x;
  double dy = y - s->originy*ys + t->ty*ys - r->cam_y;
  double rr=fmod(rot,360.0); if(rr<0) rr+=360.0;
  int unrot = fabs(rr)<0.001 || fabs(rr-360.0)<0.001;
  /* Newer nine-slice draws keep border bands at native size while stretching the center.
   * Negative or rotated draws keep the plain path because GM composes those separately. */
  if(s->ns_enabled && unrot && xs>0 && ys>0 && (fabs(xs-1.0)>1e-9 || fabs(ys-1.0)>1e-9)){
    int W=(int)lround(s->w*xs), H=(int)lround(s->h*ys);
    if(W>s->ns_l+s->ns_r && H>s->ns_t+s->ns_b){
      draw_sprite_nineslice(r,s,t, x - s->originx*xs - r->cam_x, y - s->originy*ys - r->cam_y,
                            W,H,blend,alpha);
      if(sprof) sprof_add(sprite,s->name,(rprof_now()-sprof_t0)*1000.0);
      return;
    }
  }
  if(unrot){
    int can_batch_axis=
      !gml_render_target_preserves_alpha(r) && !r->classic && !r->interp && r->alphablend &&
      r->blendmode==0 && !r->classic_phase_y &&
      !r->classic_interp_phase[0] && !r->classic_interp_phase[1] &&
      !r->classic_interp_phase[2] &&
      fabs((double)t->sw*xs*(double)t->sh*ys)>=131072.0;
    if(can_batch_axis){
      blit_rotated(r,s,t,x,y,xs,ys,0.0,blend,alpha);
      if(sprof) sprof_add(sprite,s->name,(rprof_now()-sprof_t0)*1000.0);
      return;
    }
    /* A viewport-spanning sprite behaves as a composited screen layer: both of its vertical
     * boundaries lie outside the sampled area, so half-step coverage belongs to the preceding
     * texel just as it does for an already-open quad.  Classify from projected geometry only;
     * ordinary actors and effects continue through the transform-derived path. */
    double projected_w=fabs((double)s->w*xs);
    double projected_h=fabs((double)s->h*ys);
    int spans_viewport=projected_w>(double)r->fbw+0.001 &&
                       projected_h>(double)r->fbh+0.001;
    blit_with_phase(r,t,dx,dy,xs,ys,blend,alpha,spans_viewport?0:4);
  }
  else blit_rotated_with_phase(r,s,t,x,y,xs,ys,rr,blend,alpha);
  if(sprof) sprof_add(sprite,s->name,(rprof_now()-sprof_t0)*1000.0);
}

void gml_draw_sprite_ext(GmlRender *r, int sprite, int subimg, double x, double y,
                         double xs, double ys, double rot, uint32_t blend, double alpha){
  GmlMaskedSpriteDraw masked=masked_sprite_draw_begin(r);
  draw_sprite_ext_unmasked(r,sprite,subimg,x,y,xs,ys,rot,blend,alpha);
  masked_sprite_draw_end(r,masked);
}

void gml_draw_sprite(GmlRender *r, int sprite, int subimg, double x, double y){
  gml_draw_sprite_ext(r,sprite,subimg,x,y,1,1,0,0xFFFFFF,
                      r && !r->classic ? r->alpha : 1.0);
}
/* draw_sprite_tiled_ext: repeat a sprite frame to fill the screen (both axes),
 * anchored at (x,y). Used by GML effects and tiled background scripts. */
static void draw_sprite_tiled_ext_unmasked(GmlRender *r, int sprite, int subimg,
                                           double x, double y, double xs, double ys,
                                           uint32_t blend, double alpha){
  gml_render_draw_map_point(r,&x,&y);
  gml_render_draw_map_scale(r,&xs,&ys);
  if(sprite<0||sprite>=r->n_spr) return;
  GmlSprite *s=&r->spr[sprite]; if(s->n_frames<=0) return;
  if(s->runtime_rgba){
    int sub=runtime_frame_index(s,subimg);
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
  if(log_spr_enabled(r))
    anygm_host_logf(r && r->win ? r->win->host : NULL,ANYGM_LOG_DEBUG,"[spr-tiled] spr=%d sub=%d cell=%.0fx%.0f anchor=(%.0f,%.0f) start=(%.0f,%.0f) t=(%d,%d %dx%d) a=%.2f\n",
      sprite,sub,bw,bh,ax,ay,x0,y0,t->tx,t->ty,t->sw,t->sh,alpha);
  for(double yy=y0; yy<r->fbh; yy+=bh)
    for(double xx=x0; xx<r->fbw; xx+=bw)
	      blit_with_phase(r,t,xx+t->tx*xs,yy+t->ty*ys,xs,ys,blend,alpha,0);
}

void gml_draw_sprite_tiled_ext(GmlRender *r, int sprite, int subimg, double x, double y,
                               double xs, double ys, uint32_t blend, double alpha){
  GmlMaskedSpriteDraw masked=masked_sprite_draw_begin(r);
  draw_sprite_tiled_ext_unmasked(r,sprite,subimg,x,y,xs,ys,blend,alpha);
  masked_sprite_draw_end(r,masked);
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
    int sub=runtime_frame_index(s,subimg);
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
  if(log_spr_enabled(r))
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
    blit_with_phase(r,t,xx+t->tx*xs,yy+t->ty*ys,xs,ys,blend,alpha,0);
}
void gml_draw_sprite_part_ext(GmlRender *r, int sprite, int subimg, double sx, double sy,
                              double sw, double sh, double x, double y,
                              double xs, double ys, uint32_t blend, double alpha){
  gml_render_draw_map_point(r,&x,&y);
  gml_render_draw_map_scale(r,&xs,&ys);
  if(sprite<0||sprite>=r->n_spr) return;
  if(gml_d3_draw_sprite_part_2d(r,sprite,subimg,sx,sy,sw,sh,x,y,xs,ys,blend,alpha)) return;
  GmlSprite *s=&r->spr[sprite]; if(s->n_frames<=0) return;
  if(s->runtime_rgba){
    const uint8_t *fr=runtime_frame_rgba(s,subimg); if(!fr) return;
    blit_rgba_region(r,fr,s->w,s->h,sx,sy,sw,sh,x-r->cam_x,y-r->cam_y,sw*xs,sh*ys,blend,alpha);
    return;
  }
  int sub=((subimg%s->n_frames)+s->n_frames)%s->n_frames;
  int ti=s->frame[sub]; if(ti<0||ti>=r->n_tpag) return;
  blit_tpag_part(r,&r->tpag[ti],sx,sy,sw,sh,x-r->cam_x,y-r->cam_y,xs,ys,blend,alpha);
}

void gml_draw_background(GmlRender *r, int bg, double x, double y){
  gml_render_draw_map_point(r,&x,&y);
  if(bg<0||bg>=r->n_bg) return;
  int ti=r->bg[bg].tpag; if(ti<0||ti>=r->n_tpag) return;
  if(gml_d3_draw_background_2d(r,bg,x,y,1,1,0xFFFFFF,r->alpha)) return;
  GmlTpag *t=&r->tpag[ti];
  blit_background_phase(r,t, x - r->cam_x + t->tx, y - r->cam_y + t->ty, 1,1, 0xFFFFFF, r->alpha);
}
void gml_draw_background_part_ext(GmlRender *r, int bg, double sx, double sy, double sw, double sh,
                                  double x, double y, double xs, double ys, uint32_t color, double alpha){
  gml_render_draw_map_point(r,&x,&y);
  gml_render_draw_map_scale(r,&xs,&ys);
  if(bg<0||bg>=r->n_bg) return;
  int ti=r->bg[bg].tpag; if(ti<0||ti>=r->n_tpag) return;
  if(gml_d3_draw_background_part_2d(r,bg,sx,sy,sw,sh,x,y,xs,ys,color,alpha)) return;
  blit_tpag_part_background(r,&r->tpag[ti],sx,sy,sw,sh,x-r->cam_x,y-r->cam_y,xs,ys,color,alpha);
}
void gml_draw_background_tile(GmlRender *r,int bg,
                              double sx,double sy,double sw,double sh,
                              double x,double y,double xs,double ys,
                              int mirror,int flip,int rotate,
                              uint32_t color,double alpha){
  if(!rotate){
    if(mirror){ x+=sw*xs; xs=-xs; }
    if(flip){ y+=sh*ys; ys=-ys; }
    gml_draw_background_part_ext(r,bg,sx,sy,sw,sh,x,y,xs,ys,color,alpha);
    return;
  }
  gml_render_draw_map_point(r,&x,&y);
  gml_render_draw_map_scale(r,&xs,&ys);
  if(!r || bg<0 || bg>=r->n_bg || sw<=0 || sh<=0 || xs==0 || ys==0) return;
  int ti=r->bg[bg].tpag; if(ti<0 || ti>=r->n_tpag) return;
  GmlTpag *page=&r->tpag[ti];
  int source_x=(int)lround(sx),source_y=(int)lround(sy);
  int source_width=(int)lround(sw),source_height=(int)lround(sh);
  double screen_x=x-r->cam_x,screen_y=y-r->cam_y;
  if(fabs(xs-1.0)<0.001 && fabs(ys-1.0)<0.001 &&
     fabs(sx-source_x)<1e-9 && fabs(sy-source_y)<1e-9 &&
     fabs(sw-source_width)<1e-9 && fabs(sh-source_height)<1e-9 &&
     alpha>=1.0 && (color&0xFFFFFFu)==0xFFFFFFu &&
     r->blendmode==0 && r->active_shader<0 && !gml_d3_is_active(r) &&
     page->atlas>=0 && page->atlas<r->n_atlas &&
     atlas_pixels(r,page->atlas) &&
     blit_tpag_scale1_white_exact_rotated_tile(
       r,page,&r->atlas[page->atlas],
       source_x,source_y,source_width,source_height,
       screen_x,screen_y,mirror,flip)) return;
  GmlTpag view;
  int clipped_source_x=0,clipped_source_y=0;
  if(!tpag_part_view(page,sx,sy,sw,sh,
                     &view,&clipped_source_x,&clipped_source_y,
                     NULL,NULL,NULL,NULL)) return;
  view.tx=(int)lround(clipped_source_x-sx);
  view.ty=(int)lround(clipped_source_y-sy);
  view.bw=(int)ceil(sw);
  view.bh=(int)ceil(sh);

  double tile_xscale=mirror?-xs:xs;
  double tile_yscale=flip?-ys:ys;
  double cosine=0.0,sine=0.0;
  const double degrees=-90.0;
  render_rotation_sincos(degrees,&cosine,&sine);
  double min_x=1e30,min_y=1e30;
  const double corner_x[4]={0.0,sw,sw,0.0};
  const double corner_y[4]={0.0,0.0,sh,sh};
  for(int i=0;i<4;i++){
    double local_x=corner_x[i]*tile_xscale;
    double local_y=corner_y[i]*tile_yscale;
    double transformed_x=local_x*cosine+local_y*sine;
    double transformed_y=-local_x*sine+local_y*cosine;
    if(transformed_x<min_x) min_x=transformed_x;
    if(transformed_y<min_y) min_y=transformed_y;
  }
  double draw_x=x-min_x,draw_y=y-min_y;
  /* blit_rotated applies the shared half-open cardinal anchor. The tile's public position is the
   * transformed bounding-box corner, so compensate before entering that origin-based kernel. */
  if(tile_xscale*cosine < -1e-12 || tile_yscale*sine < -1e-12) draw_x+=1.0;
  if(-tile_xscale*sine < -1e-12 || tile_yscale*cosine < -1e-12) draw_y+=1.0;
  GmlSprite sprite;
  memset(&sprite,0,sizeof sprite);
  sprite.w=view.bw; sprite.h=view.bh;
  int queued_before=r->rotated_batch_count;
  blit_rotated(r,&sprite,&view,draw_x,draw_y,
               tile_xscale,tile_yscale,degrees,color,alpha);
  if(r->rotated_batch_count>queued_before) gml_render_flush_rotated_batch(r);
}
void gml_draw_background_stretched(GmlRender *r, int bg, double x, double y, double w, double h, uint32_t color, double alpha){
  if(bg<0||bg>=r->n_bg) return;
  int ti=r->bg[bg].tpag; if(ti<0||ti>=r->n_tpag) return;
  GmlTpag *t=&r->tpag[ti];
  int bw=t->bw?t->bw:t->sw, bh=t->bh?t->bh:t->sh;
  if(bw<=0 || bh<=0) return;
  gml_draw_background_part_ext(r,bg,0,0,bw,bh,x,y,w/bw,h/bh,color,alpha);
}
/* draw_background_tiled: repeat the background image along the layer's tiled axes, anchored at (x,y).
 * htiled/vtiled come from the room layer (background_htiled[]/vtiled[]): a layer that only tiles
 * horizontally (e.g. a ground strip) must draw a single row, not fill the screen vertically. */
static void do_bg_tiled_ext(GmlRender *r, int bg, double x, double y, double xs, double ys,
                            uint32_t color, double alpha, int htiled, int vtiled){
  gml_render_draw_map_point(r,&x,&y);
  gml_render_draw_map_scale(r,&xs,&ys);
  if(bg<0||bg>=r->n_bg) return;
  { const char*sb=render_setting(r,"GML_SKIP_BG"); if(sb&&atoi(sb)==bg) return; }
  int ti=r->bg[bg].tpag; if(ti<0||ti>=r->n_tpag) return;
  GmlTpag *t=&r->tpag[ti];
  double logical_w=(t->bw?t->bw:t->sw)*xs;
  double logical_h=(t->bh?t->bh:t->sh)*ys;
  if(logical_w<=0 || logical_h<=0) return;
  if(render_setting(r,"GML_LOG_BG"))
    anygm_host_logf(r && r->win ? r->win->host : NULL,ANYGM_LOG_DEBUG,"[bg-tiled] def=%d tpag=%d src=%d,%d %dx%d dst=%d,%d base=%dx%d atlas=%d pos=(%.3f,%.3f) scale=(%.3f,%.3f) tiled=(%d,%d) alpha=%.3f\n",
      bg,ti,t->sx,t->sy,t->sw,t->sh,t->tx,t->ty,t->bw,t->bh,t->atlas,
      x,y,xs,ys,htiled,vtiled,alpha);
  if(r->classic){
    /* Keep the anchor where the content put it and let the blit apply its nearest-pixel snapping.
     * Truncating here would make the layer follow a different rule from other draws: a parallax
     * position derived from a view that oscillates by a fraction of a pixel can sit on an integer
     * boundary and flip a whole pixel while the world remains still. The tiling period is
     * unchanged, so every repeat keeps one phase. */
    double anchor_x=x-r->cam_x, anchor_y=y-r->cam_y;
    double x0,xend,y0,yend;
    if(htiled){ x0=fmod(anchor_x,logical_w); if(x0>0) x0-=logical_w; xend=r->fbw; }
    else { x0=anchor_x; xend=anchor_x+1; }
    if(vtiled){ y0=fmod(anchor_y,logical_h); if(y0>0) y0-=logical_h; yend=r->fbh; }
    else { y0=anchor_y; yend=anchor_y+1; }
    if(render_setting(r,"GML_LOG_BG"))
      anygm_host_logf(r && r->win ? r->win->host : NULL,ANYGM_LOG_DEBUG,
        "[bg-anchor] def=%d cam=(%.3f,%.3f) anchor=(%.3f,%.3f) start=(%.3f,%.3f)\n",
        bg,r->cam_x,r->cam_y,anchor_x,anchor_y,x0,y0);
    if(gml_d3_is_active(r)){
      for(double yy=y0; yy<yend; yy+=logical_h) for(double xx=x0; xx<xend; xx+=logical_w)
        gml_d3_draw_background_2d(r,bg,xx+r->cam_x,yy+r->cam_y,xs,ys,color,alpha);
      return;
    }
    for(double yy=y0; yy<yend; yy+=logical_h)
      for(double xx=x0; xx<xend; xx+=logical_w){
        if(vtiled && logical_h<r->fbh)
          blit_with_phase(r,t,xx+t->tx*xs,yy+t->ty*ys,xs,ys,color,alpha,2);
        else
          blit_background_phase(r,t,xx+t->tx*xs,yy+t->ty*ys,xs,ys,color,alpha);
    }
    return;
  }
  /* Repeat the complete logical background cell. Texture pages omit transparent margins, but
   * those margins remain part of both the tiling period and its authored anchor. */
  double anchor_x=floor(x-r->cam_x),anchor_y=floor(y-r->cam_y);
  double x0,xend,y0,yend;
  if(htiled){ x0=fmod(anchor_x,logical_w); if(x0>0) x0-=logical_w; xend=r->fbw; }
  else { x0=anchor_x; xend=anchor_x+1; }
  if(vtiled){ y0=fmod(anchor_y,logical_h); if(y0>0) y0-=logical_h; yend=r->fbh; }
  else { y0=anchor_y; yend=anchor_y+1; }
  if(gml_d3_is_active(r)){
    for(double yy=y0; yy<yend; yy+=logical_h)
      for(double xx=x0; xx<xend; xx+=logical_w)
        gml_d3_draw_background_2d(r,bg,xx+r->cam_x,yy+r->cam_y,xs,ys,color,alpha);
    return;
  }
  for(double yy=y0; yy<yend; yy+=logical_h)
    for(double xx=x0; xx<xend; xx+=logical_w)
      blit_background_phase(r,t,xx+t->tx*xs,yy+t->ty*ys,xs,ys,color,alpha);
}
/* Paint a background layer immediately in the order requested by GML. */
static void bg_emit(GmlRender *r, int bgdef, double x, double y, double xs, double ys,
                    uint32_t color, double alpha, int htiled, int vtiled){
  do_bg_tiled_ext(r,bgdef,x,y,xs,ys,color,alpha,htiled,vtiled);
}
void gml_draw_background_tiled(GmlRender *r, int bg, double x, double y, int htiled, int vtiled){
  bg_emit(r,bg,x,y,1,1,0xFFFFFF,r?r->alpha:1,htiled,vtiled);
}
/* draw_background[_tiled]_ext: as above + xscale/yscale + blend colour + alpha. */
void gml_draw_background_ext(GmlRender *r, int bg, double x, double y, double xs, double ys,
                            uint32_t color, double alpha){
  gml_render_draw_map_point(r,&x,&y);
  gml_render_draw_map_scale(r,&xs,&ys);
  if(bg<0||bg>=r->n_bg) return;
  int ti=r->bg[bg].tpag; if(ti<0||ti>=r->n_tpag) return;
  if(gml_d3_draw_background_2d(r,bg,x,y,xs,ys,color,alpha)) return;
  GmlTpag *t=&r->tpag[ti];
  blit_background_phase(r,t, x - r->cam_x + t->tx*xs, y - r->cam_y + t->ty*ys, xs,ys, color, alpha);
}
void gml_draw_background_tiled_ext(GmlRender *r, int bg, double x, double y, double xs, double ys,
                                  uint32_t color, double alpha, int htiled, int vtiled){
  bg_emit(r,bg,x,y,xs,ys,color,alpha,htiled,vtiled);
}

/* draw a room's background layers (from the room Background pointer-list).
 * want_fg: 0 = backgrounds (behind instances), 1 = foregrounds (in front). */
void gml_draw_room_backgrounds(GmlRender *r, uint32_t bg_ptr, int want_fg){
  if(!bg_ptr) return;
  const uint8_t *d=r->win->data; uint32_t cnt=u32(d,bg_ptr);
  for(uint32_t i=0;i<cnt;i++){
    uint32_t p=u32(d,bg_ptr+4+i*4);
    int enabled=(int)u32(d,p), fg=(int)u32(d,p+4), bgdef=(int)u32(d,p+8);
    int x=(int)u32(d,p+12), y=(int)u32(d,p+16), tilex=(int)u32(d,p+20), tiley=(int)u32(d,p+24);
    if(!enabled || fg!=want_fg) continue;
    if(bgdef<0||bgdef>=r->n_bg) continue;
    int ti=r->bg[bgdef].tpag; if(ti<0||ti>=r->n_tpag) continue;
    GmlTpag *t=&r->tpag[ti]; int bw=t->sw, bh=t->sh; if(bw<=0||bh<=0) continue;
    if(render_setting(r,"GML_LOG_BG"))
      anygm_host_logf(r && r->win ? r->win->host : NULL,ANYGM_LOG_DEBUG,"[bg-room] layer=%u def=%d tpag=%d src=%d,%d %dx%d dst=%d,%d base=%dx%d atlas=%d pos=(%d,%d) tiled=(%d,%d) fg=%d\n",
        i,bgdef,ti,t->sx,t->sy,t->sw,t->sh,t->tx,t->ty,t->bw,t->bh,t->atlas,
        x,y,tilex,tiley,fg);
    int x0=x, y0=y;
    if(tilex){ x0 = x % bw; if(x0>0) x0-=bw; }
    if(tiley){ y0 = y % bh; if(y0>0) y0-=bh; }
    for(int yy=y0; yy < (tiley? r->fbh : y0+1); yy+=bh){
      for(int xx=x0; xx < (tilex? r->fbw : x0+1); xx+=bw){
        blit_background_phase(r,t, xx, yy, 1,1, 0xFFFFFF, 1);
      }
      if(!tilex) {}
    }
  }
}

/* a room's tile layer: each tile blits a sub-rect (src_x,src_y,w,h) of a background/tileset
 * image to a room position, drawn back-to-front by tile depth. The room tile list is a
 * pointer-list (count + pointers), each record: x,y,bgdef,srcx,srcy,w,h,depth,id,sx,sy,color. */
typedef struct { int x,y,def,sx,sy,w,h,depth; } GmlTileRec;
static int cmp_tile_depth(const void *a, const void *b){
  const GmlTileRec *ta=a,*tb=b;
  return ta->depth>tb->depth? -1 : (ta->depth<tb->depth? 1 : 0);   /* high depth first (behind) */
}
/* blit a single tile: a sub-rect (sx,sy,w,h) of background/tileset `def` at room (x,y). */
void gml_draw_tile(GmlRender *r, int def, int sx, int sy, int w, int h, double x, double y){
  gml_render_draw_map_point(r,&x,&y);
  double draw_xscale=1.0,draw_yscale=1.0;
  gml_render_draw_map_scale(r,&draw_xscale,&draw_yscale);
  if(def<0||def>=r->n_bg) return;
  int ti=r->bg[def].tpag; if(ti<0||ti>=r->n_tpag) return;
  if(gml_d3_draw_background_part_2d(r,def,sx,sy,w,h,x,y,draw_xscale,draw_yscale,0xFFFFFF,1)) return;
  GmlTpag *bt=&r->tpag[ti]; GmlTpag tt=*bt;
  tt.sx=bt->sx+sx; tt.sy=bt->sy+sy; tt.sw=w; tt.sh=h;
  tt.interp_phase_cache[0]=tt.interp_phase_cache[1]=tt.interp_phase_cache[2]=NULL;
  /* A sub-rectangle borrows atlas pixels but not the page's geometry-dependent
   * caches. Detach the row spans and pixel caches before drawing this view. */
  tt.alpha_scanned=0; tt.ax0=tt.ay0=0; tt.ax1=tt.ay1=-1; tt.alpha_max=0;
  tt.alpha_row_min=tt.alpha_row_max=NULL;
  tt.alpha_qrow_min=tt.alpha_qrow_max=NULL; tt.alpha_qrow_built=NULL;
  tt.alpha_runs=NULL; tt.alpha_run_count=0; tt.alpha_runs_built=0;
  tt.alpha8_cache=NULL;
  tt.argb_cache=NULL;
  tt.solid_blur_alpha_cache=NULL; tt.solid_blur_alpha_shader=-1;
  tt.fast8_draw_cache=NULL; tt.fast8_draw_cache_valid=0; tt.fast8_draw_pending_count=0;
  tt.interp_draw_cache=NULL; tt.interp_draw_runs=NULL;
  tt.interp_draw_cache_bytes=0; tt.interp_draw_run_count=0;
  tt.interp_draw_cache_valid=0; tt.interp_draw_pending_count=0;

  blit_background_phase(r,&tt, x - r->cam_x, y - r->cam_y,
                        draw_xscale,draw_yscale,0xFFFFFF,1);
}
void gml_draw_room_tiles(GmlRender *r, uint32_t tile_ptr){
  if(!tile_ptr) return;
  const uint8_t *d=r->win->data; uint32_t cnt=u32(d,tile_ptr);
  if(cnt==0 || cnt>100000) return;
  if(render_setting(r,"GML_LOG_BG")) anygm_host_logf(r && r->win ? r->win->host : NULL,ANYGM_LOG_DEBUG,"[tile-room] count=%u\n",cnt);
  GmlTileRec *tiles=malloc((size_t)cnt*sizeof(GmlTileRec)); if(!tiles) return; int m=0;
  for(uint32_t i=0;i<cnt;i++){
    uint32_t p=u32(d,tile_ptr+4+i*4);
    GmlTileRec t;
    t.x=(int)u32(d,p);    t.y=(int)u32(d,p+4);  t.def=(int)u32(d,p+8);
    t.sx=(int)u32(d,p+12);t.sy=(int)u32(d,p+16);t.w=(int)u32(d,p+20);
    t.h=(int)u32(d,p+24); t.depth=(int)u32(d,p+28);
    tiles[m++]=t;
    if(render_setting(r,"GML_LOG_BG"))
      anygm_host_logf(r && r->win ? r->win->host : NULL,ANYGM_LOG_DEBUG,"[tile] index=%u def=%d source=(%d,%d %dx%d) position=(%d,%d) depth=%d\n",
        i,t.def,t.sx,t.sy,t.w,t.h,t.x,t.y,t.depth);
  }
  qsort(tiles,m,sizeof(GmlTileRec),cmp_tile_depth);
  for(int i=0;i<m;i++){
    GmlTileRec *t=&tiles[i];
    if(t->def<0 || t->def>=r->n_bg) continue;
    int ti=r->bg[t->def].tpag; if(ti<0 || ti>=r->n_tpag) continue;
    GmlTpag *bt=&r->tpag[ti];
    GmlTpag tt=*bt;                                  /* sub-rect of the tileset image */
    tt.sx=bt->sx + t->sx; tt.sy=bt->sy + t->sy; tt.sw=t->w; tt.sh=t->h;
    tt.interp_phase_cache[0]=tt.interp_phase_cache[1]=tt.interp_phase_cache[2]=NULL;
    /* A sub-rectangle borrows atlas pixels but not the page's geometry-dependent
     * caches. Detach the row spans and pixel caches before drawing this view. */
    tt.alpha_scanned=0; tt.ax0=tt.ay0=0; tt.ax1=tt.ay1=-1; tt.alpha_max=0;
    tt.alpha_row_min=tt.alpha_row_max=NULL;
    tt.alpha_qrow_min=tt.alpha_qrow_max=NULL; tt.alpha_qrow_built=NULL;
    tt.alpha_runs=NULL; tt.alpha_run_count=0; tt.alpha_runs_built=0;
    tt.alpha8_cache=NULL;
    tt.argb_cache=NULL;
    tt.solid_blur_alpha_cache=NULL; tt.solid_blur_alpha_shader=-1;
    tt.fast8_draw_cache=NULL; tt.fast8_draw_cache_valid=0; tt.fast8_draw_pending_count=0;
    tt.interp_draw_cache=NULL; tt.interp_draw_runs=NULL;
    tt.interp_draw_cache_bytes=0; tt.interp_draw_run_count=0;
    tt.interp_draw_cache_valid=0; tt.interp_draw_pending_count=0;

    blit_background_phase(r,&tt, t->x - r->cam_x, t->y - r->cam_y, 1,1, 0xFFFFFF, 1);
  }
  free(tiles);
}

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
static void sprite_region_coarse_texel(GmlAtlas *a, int sx, int sy, int sw, int sh, double out[4]){
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

/* draw_sprite_stretched: blit a sprite frame stretched into the screen rect (dx,dy,dw,dh), box-
 * averaging the source per dest pixel (matches the GPU's filtered down-stretch). Screen-space. */
typedef struct {
  GmlRender *r; GmlAtlas *a; GmlTpag *t;
  int x0,y0,W,H,sw,sh,bR,bG,bB;
  double alpha;
} SprStretchGeneralBand;

static void spr_stretch_general_band(void *context,int row_start,int row_end,int slot){
  const SprStretchGeneralBand *band=(const SprStretchGeneralBand*)context;
  (void)slot;
  GmlRender *r=band->r;
  GmlAtlas *a=band->a;
  GmlTpag *t=band->t;
  const int x0=band->x0, y0=band->y0, W=band->W, H=band->H;
  const int sw=band->sw, sh=band->sh;
  const int bR=band->bR, bG=band->bG, bB=band->bB;
  const double alpha=band->alpha;
  for(int py=row_start;py<row_end;py++){ int ty_=y0+py; if(ty_<0||ty_>=r->fbh) continue;
    int sy0=(py*sh)/H, sy1=((py+1)*sh)/H; if(sy1<=sy0) sy1=sy0+1;
    for(int px=0;px<W;px++){ int tx_=x0+px; if(tx_<0||tx_>=r->fbw) continue;
      int sx0=(px*sw)/W, sx1=((px+1)*sw)/W; if(sx1<=sx0) sx1=sx0+1;
      int R=0,G=0,B=0,A=0,n=0;
      for(int sy=sy0;sy<sy1;sy++){ int iy=sy-t->ty; if(iy<0||iy>=t->sh) continue;
        for(int sx=sx0;sx<sx1;sx++){ int ix=sx-t->tx; if(ix<0||ix>=t->sw) continue;
          uint8_t *sp=a->px+((size_t)(t->sy+iy)*a->w+(t->sx+ix))*4;
          R+=sp[0]; G+=sp[1]; B+=sp[2]; A+=sp[3]; n++; } }
      if(!n) continue;
      /* Filtered (interpolation=true) downscale matches the GPU: average the covered texels in
       * FLOAT and convert float->8bit round-to-nearest. For an integer NxM box each covered texel
       * carries equal weight, which is exactly GL's bilinear result when the destination straddles
       * texel centres (e.g. an exact 2x downscale samples at frac=0.5 → 0.25 per texel = 2x2 mean).
       * The old code truncated the channel average (int cast) and truncated the blend, which
       * systematically DARKENED a bright overlay; keep full float precision and round at the end. */
      double fA=A/(double)n;
      if(shader_discards_alpha_value(r,fA)) continue;
      if(shader_ordered_dither_drops(r,(double)tx_+0.5+r->cam_x,
                                       (double)ty_+0.5+r->cam_y)) continue;
      int sample_a=(int)floor(fA+0.5);
      int sample_r=(int)floor(R/(double)n+0.5);
      int sample_g=(int)floor(G/(double)n+0.5);
      int sample_b=(int)floor(B/(double)n+0.5);
      if(mapped_texture_active(r)){
        uint32_t sampled=mapped_texture_pixel(r,((uint32_t)sample_a<<24)|
          ((uint32_t)sample_r<<16)|((uint32_t)sample_g<<8)|(uint32_t)sample_b);
        sample_a=(int)(sampled>>24);
        sample_r=(sampled>>16)&255;
        sample_g=(sampled>>8)&255;
        sample_b=sampled&255;
      }
      double oa=(sample_a/255.0)*alpha; if(oa<=0) continue;
      double srcR=sample_r*bR/255.0;
      double srcG=sample_g*bG/255.0;
      double srcB=sample_b*bB/255.0;
      uint32_t *dp=&r->fb[(size_t)ty_*r->fbw+tx_];
      if(!r->alphablend || oa>=1.0){
        int sr=(int)(srcR+0.5), sg=(int)(srcG+0.5), sb=(int)(srcB+0.5);
        *dp=0xFF000000u|(sr<<16)|(sg<<8)|sb; continue;
      }
      double ia=1.0-oa;
      int dr=(*dp>>16)&0xFF, dg=(*dp>>8)&0xFF, db=*dp&0xFF;
      *dp=0xFF000000u|((int)(srcR*oa+dr*ia+0.5)<<16)|
          ((int)(srcG*oa+dg*ia+0.5)<<8)|(int)(srcB*oa+db*ia+0.5);
    }
  }
}

void gml_draw_sprite_stretched(GmlRender *r, int sprite, int frame, double dx, double dy, double dw, double dh, uint32_t blend, double alpha){
  gml_render_draw_map_point(r,&dx,&dy);
  gml_render_draw_map_scale(r,&dw,&dh);
  if(sprite<0||sprite>=r->n_spr) return;
  GmlSprite *s=&r->spr[sprite]; if(s->n_frames<=0) return;
  if(s->w>0 && s->h>0){
    double xs=dw/s->w,ys=dh/s->h;
    if(gml_d3_draw_sprite_2d(r,sprite,frame,dx+s->originx*xs,dy+s->originy*ys,xs,ys,0,blend,alpha)) return;
  }
  dx-=r->cam_x; dy-=r->cam_y;
  if(s->runtime_rgba){
    const uint8_t *fr=runtime_frame_rgba(s,frame); if(!fr) return;
    blit_rgba_region(r,fr,s->w,s->h,0,0,s->w,s->h,dx,dy,dw,dh,blend,alpha);
    return;
  }
  int sub=((frame%s->n_frames)+s->n_frames)%s->n_frames;
  int ti=s->frame[sub]; if(ti<0||ti>=r->n_tpag) return;
  GmlTpag *t=&r->tpag[ti]; if(t->atlas<0||t->atlas>=r->n_atlas) return;
  GmlAtlas *a=&r->atlas[t->atlas]; if(!atlas_pixels(r,t->atlas)) return;
  int sw = s->w>0? s->w : t->sw, sh = s->h>0? s->h : t->sh;
  int x0=(int)floor(dx), y0=(int)floor(dy), W=(int)lround(dw), H=(int)lround(dh);
  if(sw<=0||sh<=0||W<=0||H<=0) return;
  /* nine-slice applies to stretched draws too (same fixed-border layout) */
  if(s->ns_enabled && (W!=sw || H!=sh) && W>s->ns_l+s->ns_r && H>s->ns_t+s->ns_b){
    draw_sprite_nineslice(r,s,t,dx,dy,W,H,blend,alpha);
    return;
  }
  int bR=blend&0xFF, bG=(blend>>8)&0xFF, bB=(blend>>16)&0xFF;
  if(W==sw && H==sh){
    int cx0=x0<0?0:x0, cy0=y0<0?0:y0;
    int cx1=x0+W; if(cx1>r->fbw) cx1=r->fbw;
    int cy1=y0+H; if(cy1>r->fbh) cy1=r->fbh;
    int cw=cx1-cx0, ch=cy1-cy0;
    if(cw<=0 || ch<=0) return;
    int lx0=cx0-x0, ly0=cy0-y0;
    gml_render_maybe_prepare_draw(r);
    for(int yy=0; yy<ch; yy++){
      int iy=ly0+yy-t->ty;
      if(iy<0||iy>=t->sh) continue;
      uint32_t *dp=r->fb+(size_t)(cy0+yy)*r->fbw+cx0;
      for(int xx=0; xx<cw; xx++){
        int ix=lx0+xx-t->tx;
        if(ix<0||ix>=t->sw) continue;
        uint8_t *sp=a->px + ((size_t)(t->sy+iy)*a->w + (t->sx+ix))*4;
        if(shader_discards_alpha(r,sp[3])) continue;
        /* The destination is in framebuffer space and the camera was subtracted from the
         * destination origin above, so adding it back gives the object-space position the
         * fragment interpolates. */
        if(shader_ordered_dither_drops(r,(double)(cx0+xx)+0.5+r->cam_x,
                                         (double)(cy0+yy)+0.5+r->cam_y)) continue;
        uint32_t sampled=((uint32_t)sp[3]<<24)|((uint32_t)sp[0]<<16)|
                         ((uint32_t)sp[1]<<8)|(uint32_t)sp[2];
        if(mapped_texture_active(r)) sampled=mapped_texture_pixel(r,sampled);
        int sample_a=(int)(sampled>>24);
        double oa=(sample_a/255.0)*alpha;
        if(oa<=0) continue;
        int sr=((sampled>>16)&255)*bR/255;
        int sg=((sampled>>8)&255)*bG/255;
        int sb=(sampled&255)*bB/255;
        if(!r->alphablend){ dp[xx]=0xFF000000u|(sr<<16)|(sg<<8)|sb; continue; }
        uint32_t dv=dp[xx];
        int dr=(dv>>16)&0xFF, dg=(dv>>8)&0xFF, db=dv&0xFF;
        dp[xx]=0xFF000000u|((int)(sr*oa+dr*(1-oa)+0.5)<<16)|((int)(sg*oa+dg*(1-oa)+0.5)<<8)|(int)(sb*oa+db*(1-oa)+0.5);
      }
    }
    return;
  }
  /* Full-screen MINIFIED overlay → flat coarse-mip veil (see sprite_region_coarse_texel).
   * Gate tightly on the "post-process overlay" pattern (the sprite is downscaled AND covers the
   * whole render target from the origin) so ordinary in-world minified sprites keep the per-pixel
   * filtered path. This is the case GameMaker's mipmapped full-screen sprites hit on the GPU. */
  if((sw>W || sh>H) && x0<=0 && y0<=0 && x0+W>=r->fbw && y0+H>=r->fbh){
    double ct[4]; sprite_region_coarse_texel(a,t->sx,t->sy,t->sw,t->sh,ct);
    if(shader_discards_alpha_value(r,ct[3])) return;
    int sample_a=(int)floor(ct[3]+0.5);
    int sample_r=(int)floor(ct[0]+0.5);
    int sample_g=(int)floor(ct[1]+0.5);
    int sample_b=(int)floor(ct[2]+0.5);
    if(mapped_texture_active(r)){
      uint32_t sampled=mapped_texture_pixel(r,((uint32_t)sample_a<<24)|
        ((uint32_t)sample_r<<16)|((uint32_t)sample_g<<8)|(uint32_t)sample_b);
      sample_a=(int)(sampled>>24);
      sample_r=(sampled>>16)&255;
      sample_g=(sampled>>8)&255;
      sample_b=sampled&255;
    }
    double sa=(sample_a/255.0)*alpha;
    if(sa>0){
      double srcR=sample_r*bR/255.0;
      double srcG=sample_g*bG/255.0;
      double srcB=sample_b*bB/255.0;
      int cx0=x0<0?0:x0, cy0=y0<0?0:y0;
      int cx1=x0+W; if(cx1>r->fbw) cx1=r->fbw;
      int cy1=y0+H; if(cy1>r->fbh) cy1=r->fbh;
      if(cx1>cx0 && cy1>cy0){
        gml_render_maybe_prepare_draw(r);
        int oR=(int)(srcR+0.5), oG=(int)(srcG+0.5), oB=(int)(srcB+0.5);
        uint32_t opaque=0xFF000000u|(oR<<16)|(oG<<8)|oB;
        double ia=1.0-sa;
        for(int yy=cy0; yy<cy1; yy++){
          uint32_t *dp=r->fb+(size_t)yy*r->fbw+cx0;
          for(int xx=cx0; xx<cx1; xx++,dp++){
            /* A dithered veil covers only its pattern cells and preserves the other
             * destination pixels in the framebuffer. */
            if(shader_ordered_dither_drops(r,(double)xx+0.5+r->cam_x,
                                             (double)yy+0.5+r->cam_y)) continue;
            if(!r->alphablend || sa>=1.0){ *dp=opaque; continue; }
            uint32_t dv=*dp; int dr=(dv>>16)&0xFF, dg=(dv>>8)&0xFF, db=dv&0xFF;
            *dp=0xFF000000u | ((int)(srcR*sa+dr*ia+0.5)<<16) | ((int)(srcG*sa+dg*ia+0.5)<<8) | (int)(srcB*sa+db*ia+0.5);
          }
        }
      }
    }
    return;
  }
  gml_render_maybe_prepare_draw(r);
  /* Bilinear magnification when interpolation is on (matches the GPU; e.g. a full-screen overlay
   * sprite stretched up for the "old TV" veil). Sample 4 sprite texels at the output pixel centre. */
  /* The banded bilinear kernel writes every destination pixel it covers. A dithered pass covers
   * only its pattern cells, so it takes the general loop below, which consults the pattern. */
  if(r->interp && W>sw && H>sh && !shader_ordered_dither_active(r)){
    /* per-column/row atlas offsets precomputed (with the tpag crop resolved to a byte offset or -1
     * for transparent), so the inner loop is 4 reads + float taps — no per-pixel div/floor/branches. */
    ptrdiff_t *colA=malloc((size_t)W*sizeof(ptrdiff_t)), *colB=malloc((size_t)W*sizeof(ptrdiff_t));
    float *cfx=malloc((size_t)W*sizeof(float));
    ptrdiff_t *rowA=malloc((size_t)H*sizeof(ptrdiff_t)), *rowB=malloc((size_t)H*sizeof(ptrdiff_t));
    float *cfy=malloc((size_t)H*sizeof(float));
    if(colA&&colB&&cfx&&rowA&&rowB&&cfy){
      for(int px=0; px<W; px++){
        double fsx=(px+0.5)*sw/(double)W-0.5; int xa=(int)floor(fsx); cfx[px]=(float)(fsx-xa); int xb=xa+1;
        if(xa<0)xa=0;else if(xa>sw-1)xa=sw-1; if(xb<0)xb=0;else if(xb>sw-1)xb=sw-1;
        int ia=xa-t->tx, ib=xb-t->tx;
        colA[px]=(ia>=0&&ia<t->sw)?(ptrdiff_t)(t->sx+ia)*4:-1;
        colB[px]=(ib>=0&&ib<t->sw)?(ptrdiff_t)(t->sx+ib)*4:-1;
      }
      for(int py=0; py<H; py++){
        double fsy=(py+0.5)*sh/(double)H-0.5; int ya=(int)floor(fsy); cfy[py]=(float)(fsy-ya); int yb=ya+1;
        if(ya<0)ya=0;else if(ya>sh-1)ya=sh-1; if(yb<0)yb=0;else if(yb>sh-1)yb=sh-1;
        int ja=ya-t->ty, jb=yb-t->ty;
        rowA[py]=(ja>=0&&ja<t->sh)?(ptrdiff_t)(t->sy+ja)*a->w*4:-1;
        rowB[py]=(jb>=0&&jb<t->sh)?(ptrdiff_t)(t->sy+jb)*a->w*4:-1;
      }
      SprBiCtx ctx={ r, a->px, colA, colB, rowA, rowB, cfx, cfy, W, x0, y0,
        (float)alpha, bR/255.0f,bG/255.0f,bB/255.0f, !r->alphablend };
      gml_run_row_bands(r,H,spr_bi_band,&ctx);
    }
    free(colA); free(colB); free(cfx); free(rowA); free(rowB); free(cfy);
    return;
  }
  SprStretchGeneralBand band={r,a,t,x0,y0,W,H,sw,sh,bR,bG,bB,alpha};
  if((int64_t)W*(int64_t)H>=262144 && mapped_texture_prepare_parallel(r))
    gml_run_row_bands(r,H,spr_stretch_general_band,&band);
  else
    spr_stretch_general_band(&band,0,H,0);
}

typedef struct {
  GmlRender *r; const struct GmlShaderPal *sp;
  int x0, y0, width, opaque;
  float pixel_size;
  uint32_t *scratch;
} PaintBand;
