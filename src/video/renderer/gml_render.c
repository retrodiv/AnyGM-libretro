/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* gml_render.c — atlas/TPAG/sprite decode + software blitter. */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"
#include "gml_render.h"
#include "anygm_compatibility.h"
#include "gml_default_font_data.h"
#include "gml_classic_info_font_data.h"
#include "gm_qoi.h"
#include "bzip2/bzlib.h"
#include "gml_thread.h"
#include "anygm_host.h"
#include "anygm_vfs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <limits.h>
#if defined(__SSE2__)
#include <emmintrin.h>
#endif
GML_THREAD_BRIDGE_IMPL
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#endif

static int rprof_enabled(void){ return 0; }
static int log_spr_enabled(void){ return 0; }
static int log_axis_cache_enabled(void){ return 0; }
static const char *render_setting(const GmlRender *r,const char *name){
  return anygm_host_development_setting(r&&r->win?r->win->host:NULL,name);
}

/* Cardinal rotations must stay exactly on the pixel lattice.  libm leaves tiny residuals for
 * sin/cos(90*n) (for example cos(270) ~= -1.8e-16); inverse texture mapping then floors a sample
 * on the wrong side of an integer boundary and shifts the quadrants with a negative axis by one
 * pixel.  Hardware vertex transforms preserve these literal cardinal matrices. */
static inline void render_rotation_sincos(double degrees,double *cosine,double *sine){
  double quadrant=nearbyint(degrees/90.0);
  if(fabs(degrees-quadrant*90.0)<1e-10){
    switch(((int)quadrant%4+4)%4){
      case 0: *cosine=1.0;  *sine=0.0;  return;
      case 1: *cosine=0.0;  *sine=1.0;  return;
      case 2: *cosine=-1.0; *sine=0.0;  return;
      default:*cosine=0.0;  *sine=-1.0; return;
    }
  }
  double radians=degrees*M_PI/180.0;
  *cosine=cos(radians);
  *sine=sin(radians);
}

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
static int sprof_enabled(void){ return 0; }
static double rprof_now(void){
  return 0.0;
}
static int rprof_tpag_id(GmlRender *r, GmlTpag *t){
  if(!r || !t || !r->tpag || r->n_tpag<=0) return -1;
  uintptr_t pointer=(uintptr_t)t, begin=(uintptr_t)r->tpag;
  uintptr_t end=begin+(uintptr_t)r->n_tpag*sizeof(GmlTpag);
  return pointer>=begin && pointer<end ? (int)((pointer-begin)/sizeof(GmlTpag)) : -1;
}
static void rprof_add(const char *label, GmlRender *r, GmlTpag *t, double ms, unsigned long long pixels){
  (void)label; (void)r; (void)t; (void)ms; (void)pixels;
}
static void sprof_add(int sprite, const char *name, double ms){
  (void)sprite; (void)name; (void)ms;
}
static void crt_tables_free(GmlRender *r);

static uint32_t u32(const uint8_t *d, uint32_t o){
  return (uint32_t)d[o]|(uint32_t)d[o+1]<<8|(uint32_t)d[o+2]<<16|(uint32_t)d[o+3]<<24;
}
static uint16_t u16(const uint8_t *d, uint32_t o){ return (uint16_t)(d[o]|d[o+1]<<8); }
static uint32_t be32(const uint8_t *d){ return (uint32_t)d[0]<<24|(uint32_t)d[1]<<16|(uint32_t)d[2]<<8|(uint32_t)d[3]; }
#define RFP_SHIFT 20
#define RFP_ONE ((int64_t)1 << RFP_SHIFT)
#if defined(__GNUC__) && !defined(__clang__)
#define GML_HOT_RENDER __attribute__((hot,optimize("O3")))
#else
#define GML_HOT_RENDER
#endif
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
static inline uint32_t blend_fast8_cached(uint32_t dst, uint32_t srb, uint32_t sg, uint32_t ia){
  uint32_t rb=((srb+(dst&0x00FF00FFu)*ia)>>8)&0x00FF00FFu;
  uint32_t g=((sg+(dst&0x0000FF00u)*ia)>>8)&0x0000FF00u;
  return 0xFF000000u|rb|g;
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
/* ---- atlas (TXTR) ---- */
/* A texture blob is one of: PNG (bc14-16), a bare GameMaker-QOI "fioq" stream, or a bzip2-compressed
 * "2zoq" container wrapping a "fioq" stream (bc17). Decode any of them to RGBA. */
static uint8_t *decode_texture_blob(const uint8_t *blob, size_t avail, size_t chunk_end,
                                    int *ow, int *oh){
  if(avail<4) return NULL;
  if(blob[0]==0x89 && blob[1]=='P' && blob[2]=='N' && blob[3]=='G'){
    int w,h,ch; uint8_t *px=stbi_load_from_memory(blob,(int)avail,&w,&h,&ch,4);
    if(px){ *ow=w; *oh=h; } return px;
  }
  if(!memcmp(blob,"fioq",4)) return gm_qoi_decode(blob,avail,ow,oh);
  if(!memcmp(blob,"2zoq",4)){
    /* "2zoq": magic(4) + w(u16) + h(u16) + decompressed_len(u32) + bzip2 stream */
    if(avail<12) return NULL;
    uint32_t dlen=u32(blob,8);
    if(dlen<12 || dlen>64u*1024*1024) return NULL;
    char *dec=malloc(dlen); if(!dec) return NULL;
    unsigned int declen=dlen;
    unsigned int srclen=(unsigned int)(chunk_end>(size_t)(blob+12)? chunk_end-(size_t)(blob+12) : 0);
    int rc=BZ2_bzBuffToBuffDecompress(dec,&declen,(char*)(blob+12),srclen,0,0);
    uint8_t *px=NULL;
    if((rc==BZ_OK||rc==BZ_OUTBUFF_FULL) && declen>=12) px=gm_qoi_decode((uint8_t*)dec,declen,ow,oh);
    free(dec); return px;
  }
  return NULL;
}
static int texture_blob_dims(const uint8_t *blob, size_t avail, int *ow, int *oh){
  int w=0,h=0;
  if(avail>=24 && blob[0]==0x89 && blob[1]=='P' && blob[2]=='N' && blob[3]=='G' &&
     !memcmp(blob+12,"IHDR",4)){
    w=(int)be32(blob+16); h=(int)be32(blob+20);
  } else if(avail>=12 && !memcmp(blob,"fioq",4)){
    w=(int)u16(blob,4); h=(int)u16(blob,6);
  } else if(avail>=8 && !memcmp(blob,"2zoq",4)){
    w=(int)u16(blob,4); h=(int)u16(blob,6);
  }
  if(w<=0 || h<=0 || (uint64_t)w*(uint64_t)h>64ull*1024ull*1024ull) return 0;
  if(ow) *ow=w; if(oh) *oh=h;
  return 1;
}
/* ---- async atlas prefetch pool ----
 * A first draw touching an undecoded atlas costs a full BZ2+QOI decode (tens of ms on a big
 * GMS2 atlas — a visible frame hitch). Worker threads decode queued atlases in the background
 * (queued at room enter / state load); the draw path keeps a synchronous fallback so output
 * never depends on prefetch timing. Disable with GML_ATLAS_THREADS=0. */
typedef struct {
  gml_mutex_t mu; gml_cond_t work, done;
  gml_thread_t th[8]; int nth, shutdown;
  unsigned qhead, qtail; int *queue; unsigned qcap;
  uint8_t *state;                     /* per-atlas: 0 idle, 1 queued, 2 decoding */
  GmlRender *r;
} GmlAtlasPool;
static int log_atlas_on(void){ return 0; }
static size_t atlas_spec_budget(GmlRender *r){
  return r&&r->atlas_prefetch_budget?r->atlas_prefetch_budget:512u*1024u*1024u;
}
/* decode outside the lock, publish under it. Returns the published pixels (or NULL). */
static uint8_t *atlas_decode_publish(GmlRender *r, int idx, int locked, GmlAtlasPool *pool){
  GmlAtlas *a=&r->atlas[idx];
  int w=0,h=0;
  uint8_t *px=(a->blob && a->blob<r->win->size)?
    decode_texture_blob(r->win->data+a->blob, a->avail, a->chunk_end, &w,&h) : NULL;
  if(locked) gml_mutex_lock(&pool->mu);
  a->decode_attempted=1;
  if(px){
    a->w=w; a->h=h;
    r->atlas_decoded_bytes += (size_t)w*(size_t)h*4u;
    __atomic_store_n(&a->px,px,__ATOMIC_RELEASE);
    if(log_atlas_on())
      anygm_host_logf(r && r->win ? r->win->host : NULL,ANYGM_LOG_DEBUG,"[atlas] decoded %d %dx%d (%.1f MiB)\n",idx,w,h,(double)((uint64_t)w*(uint64_t)h*4ull)/(1024.0*1024.0));
  }
  if(locked){
    pool->state[idx]=0;
    gml_cond_broadcast(&pool->done);
  }
  return px;
}
static void *atlas_worker(void *arg){
  GmlAtlasPool *pool=(GmlAtlasPool*)arg;
  GmlRender *r=pool->r;
  gml_mutex_lock(&pool->mu);
  while(!pool->shutdown){
    if(pool->qhead==pool->qtail){ gml_cond_wait(&pool->work,&pool->mu); continue; }
    int idx=pool->queue[pool->qhead % pool->qcap]; pool->qhead++;
    GmlAtlas *a=&r->atlas[idx];
    if(pool->state[idx]!=1){ continue; }              /* claimed by a sync decode meanwhile */
    if(a->px || a->decode_attempted){ pool->state[idx]=0; continue; }
    pool->state[idx]=2;
    gml_mutex_unlock(&pool->mu);
    atlas_decode_publish(r,idx,1,pool);               /* relocks to publish */
  }
  gml_mutex_unlock(&pool->mu);
  return NULL;
}
static GmlAtlasPool *atlas_pool_get(GmlRender *r){
  if(r->prefetch_checked) return (GmlAtlasPool*)r->prefetch;
  r->prefetch_checked=1;
  const char *e=render_setting(r,"GML_ATLAS_THREADS");
  int nth = e? atoi(e) : 0;
  if(!e){
    int nc=gml_ncpu();
    nth = nc-1;
    if(nth>4) nth=4;
    if(nth<1) nth=1;
  }
  if(nth<=0 || r->n_atlas<=0) return NULL;
  if(nth>8) nth=8;
  GmlAtlasPool *pool=calloc(1,sizeof *pool);
  if(!pool) return NULL;
  pool->r=r;
  pool->qcap=(unsigned)r->n_atlas;
  pool->queue=malloc(pool->qcap*sizeof(int));
  pool->state=calloc((size_t)r->n_atlas,1);
  if(!pool->queue || !pool->state){ free(pool->queue); free(pool->state); free(pool); return NULL; }
  gml_mutex_init(&pool->mu); gml_cond_init(&pool->work); gml_cond_init(&pool->done);
  for(int i=0;i<nth;i++){
    if(gml_thread_create(&pool->th[pool->nth],atlas_worker,pool)==0) pool->nth++;
  }
  if(!pool->nth){
    gml_mutex_destroy(&pool->mu); gml_cond_destroy(&pool->work); gml_cond_destroy(&pool->done);
    free(pool->queue); free(pool->state); free(pool);
    return NULL;
  }
  r->prefetch=pool;
  return pool;
}
static void atlas_pool_free(GmlRender *r){
  GmlAtlasPool *pool=(GmlAtlasPool*)r->prefetch;
  if(!pool) return;
  gml_mutex_lock(&pool->mu);
  pool->shutdown=1;
  gml_cond_broadcast(&pool->work);
  gml_mutex_unlock(&pool->mu);
  for(int i=0;i<pool->nth;i++) gml_thread_join(pool->th[i]);
  gml_mutex_destroy(&pool->mu); gml_cond_destroy(&pool->work); gml_cond_destroy(&pool->done);
  free(pool->queue); free(pool->state); free(pool);
  r->prefetch=NULL; r->prefetch_checked=0;
}
void gml_render_prefetch_atlas(GmlRender *r, int idx){
  if(!r || idx<0 || idx>=r->n_atlas || !r->atlas) return;
  GmlAtlas *a=&r->atlas[idx];
  if(a->px || a->decode_attempted || !a->blob || a->blob>=r->win->size) return;
  if(r->atlas_decoded_bytes > 2*atlas_spec_budget(r)) return;   /* prefetch cap; draws still decode on demand */
  GmlAtlasPool *pool=atlas_pool_get(r);
  if(!pool) return;
  gml_mutex_lock(&pool->mu);
  if(!a->px && !a->decode_attempted && pool->state[idx]==0 && pool->qtail-pool->qhead<pool->qcap){
    pool->state[idx]=1;
    pool->queue[pool->qtail % pool->qcap]=idx; pool->qtail++;
    gml_cond_broadcast(&pool->work);
  }
  gml_mutex_unlock(&pool->mu);
}
static void prefetch_atlas_and_neighbors(GmlRender *r, int idx){
  gml_render_prefetch_atlas(r,idx);
  /* GM's texture packer clusters related pages: a page adjacent to a needed one is likely
   * needed moments later (spawned effects/enemies). Speculative, so budget-gated: past the
   * cap only directly-referenced pages keep prefetching (draws still decode on demand). */
  if(r->atlas_decoded_bytes < atlas_spec_budget(r)){
    gml_render_prefetch_atlas(r,idx-1);
    gml_render_prefetch_atlas(r,idx+1);
  }
}
void gml_render_prefetch_sprite(GmlRender *r, int sprite){
  if(!r || sprite<0 || sprite>=r->n_spr) return;
  GmlSprite *s=&r->spr[sprite];
  if(s->runtime_rgba || !s->frame) return;
  for(int f=0; f<s->n_frames; f++){
    int ti=s->frame[f];
    if(ti<0 || ti>=r->n_tpag) continue;
    prefetch_atlas_and_neighbors(r,r->tpag[ti].atlas);
  }
}
void gml_render_prefetch_bg(GmlRender *r, int bg){
  if(!r || bg<0 || bg>=r->n_bg) return;
  int ti=r->bg[bg].tpag;
  if(ti<0 || ti>=r->n_tpag) return;
  prefetch_atlas_and_neighbors(r,r->tpag[ti].atlas);
}
static void atlas_dump_maybe(GmlRender *r, int idx){
  (void)r;
  (void)idx;
}
static uint8_t *atlas_pixels(GmlRender *r, int idx){
  if(!r || idx<0 || idx>=r->n_atlas || !r->atlas) return NULL;
  GmlAtlas *a=&r->atlas[idx];
  uint8_t *p=__atomic_load_n(&a->px,__ATOMIC_ACQUIRE);
  if(p){ atlas_dump_maybe(r,idx); return p; }
  GmlAtlasPool *pool=(GmlAtlasPool*)r->prefetch;
  if(!pool){
    if(a->decode_attempted || !a->blob || a->blob>=r->win->size) return NULL;
    p=atlas_decode_publish(r,idx,0,NULL);
    if(p) atlas_dump_maybe(r,idx);
    return p;
  }
  gml_mutex_lock(&pool->mu);
  for(;;){
    p=a->px;
    if(p || a->decode_attempted) break;
    if(pool->state[idx]==2){ gml_cond_wait(&pool->done,&pool->mu); continue; }
    /* idle or queued: claim it and decode synchronously (a queued entry goes stale; workers skip it) */
    pool->state[idx]=2;
    gml_mutex_unlock(&pool->mu);
    p=atlas_decode_publish(r,idx,1,pool);   /* relocks to publish */
    break;
  }
  p=a->px;
  gml_mutex_unlock(&pool->mu);
  if(p) atlas_dump_maybe(r,idx);
  return p;
}
static void warm_atlas_direct(GmlRender *r, int idx){
  if(!r || idx<0 || idx>=r->n_atlas || !r->atlas) return;
  GmlAtlas *a=&r->atlas[idx];
  if(a->px || a->decode_attempted || !a->blob || a->blob>=r->win->size) return;
  (void)atlas_pixels(r,idx);
}
int gml_render_warm_atlas(GmlRender *r,int atlas){
  return r&&atlas>=0&&atlas<r->n_atlas&&atlas_pixels(r,atlas)!=NULL;
}
void gml_render_warm_sprite(GmlRender *r, int sprite){
  if(!r || sprite<0 || sprite>=r->n_spr) return;
  GmlSprite *s=&r->spr[sprite];
  if(s->runtime_rgba || !s->frame) return;
  for(int f=0; f<s->n_frames; f++){
    int ti=s->frame[f];
    if(ti<0 || ti>=r->n_tpag) continue;
    warm_atlas_direct(r,r->tpag[ti].atlas);
  }
}
void gml_render_warm_bg(GmlRender *r, int bg){
  if(!r || bg<0 || bg>=r->n_bg) return;
  int ti=r->bg[bg].tpag;
  if(ti<0 || ti>=r->n_tpag) return;
  warm_atlas_direct(r,r->tpag[ti].atlas);
}
static int tpag_alpha_bounds(GmlRender *r, GmlTpag *t, GmlAtlas *a,
                             int *x0, int *y0, int *x1, int *y1){
  (void)r;
  if(!t || !a || !a->px || t->sw<=0 || t->sh<=0) return 0;
  if(!t->alpha_scanned){
    int minx=t->sw, miny=t->sh, maxx=-1, maxy=-1;
    int maxa=0;
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
static inline uint32_t gml_sprite_target_alpha(const GmlRender *r,uint32_t dst,
                                               unsigned source_alpha){
  if(!r || r->target_sp<=0) return 0xFF000000u;
  if(source_alpha>255u) source_alpha=255u;
  unsigned destination_alpha=dst>>24;
  unsigned inverse=255u-source_alpha;
  unsigned output=(source_alpha*source_alpha+destination_alpha*inverse+127u)/255u;
  if(output>255u) output=255u;
  return output<<24;
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
      /* The Studio 2 format uses a UNORM target: the complete source-over sum is rounded to
       * the nearest representable channel. Classic rounds the terms independently above, while
       * Studio 1 truncates the combined result below. */
      dp[k]=gml_sprite_target_alpha(r,dst,aa)|(((sr*aa+dr*ia+127u)/255u)<<16)|
            (((sg*aa+dg*ia+127u)/255u)<<8)|((sb*aa+db*ia+127u)/255u);
    } else {
      dp[k]=gml_sprite_target_alpha(r,dst,aa)|(((sr*aa+dr*ia)/255u)<<16)|
            (((sg*aa+dg*ia)/255u)<<8)|((sb*aa+db*ia)/255u);
    }
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
    for(int k=0; k<run; k++){
      uint32_t src=sp[k], dst=dp[k];
      int sr=(src>>16)&0xFF, sg=(src>>8)&0xFF, sb=src&0xFF;
      int dr=(dst>>16)&0xFF, dg=(dst>>8)&0xFF, db=dst&0xFF;
      int or_=(int)(sr*sa+dr*ia); if(or_>255) or_=255; else if(or_<0) or_=0;
      int og=(int)(sg*sa+dg*ia); if(og>255) og=255; else if(og<0) og=0;
      int ob=(int)(sb*sa+db*ia); if(ob>255) ob=255; else if(ob<0) ob=0;
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
static void parse_txtr(GmlRender *r){
  const GmlChunk *c=gml_chunk(r->win,"TXTR"); if(!c) return;
  const uint8_t *d=r->win->data; uint32_t n=u32(d,c->off);
  size_t chunk_end=(size_t)(d + c->off + c->size);
  r->atlas=calloc(n?n:1,sizeof(GmlAtlas));
  if(!r->atlas) return;
  r->n_atlas=(int)n;
  for(uint32_t i=0;i<n;i++){
    uint32_t entry=u32(d,c->off+4+i*4);
    /* Locate the texture data through a candidate pointer in its record. */
    uint32_t blob=0;
    /* The EmbeddedTexture record holds the blob pointer at a version-dependent offset. Scan the
     * bounded record for the field that lands on a known PNG, fioq, or 2zoq magic. */
    for(uint32_t off=4; off<=32; off+=4){ uint32_t v=u32(d,entry+off);
      if((size_t)v+4<=r->win->size){ const uint8_t *m=d+v;
        if((m[0]==0x89&&m[1]=='P'&&m[2]=='N'&&m[3]=='G')||!memcmp(m,"fioq",4)||!memcmp(m,"2zoq",4)){ blob=v; break; } } }
    if(!blob || (size_t)blob>=r->win->size) continue;
    GmlAtlas *a=&r->atlas[i];
    a->blob=blob; a->avail=r->win->size-blob; a->chunk_end=chunk_end;
    texture_blob_dims(d+blob,a->avail,&a->w,&a->h);
    if(render_setting(r,"GML_ATLAS_EAGER") || render_setting(r,"GML_DUMP_ATLAS")) atlas_pixels(r,(int)i);
  }
}

/* ---- TPAG ---- */
static void runtime_axis_cache_free(GmlSprite *s);
static void parse_tpag(GmlRender *r){
  const GmlChunk *c=gml_chunk(r->win,"TPAG"); if(!c) return;
  const uint8_t *d=r->win->data; uint32_t n=u32(d,c->off);
  r->n_tpag=(int)n; r->tpag=calloc(n,sizeof(GmlTpag)); r->tpag_ptr=calloc(n,sizeof(uint32_t));
  if(!r->tpag || !r->tpag_ptr){ free(r->tpag); free(r->tpag_ptr); r->tpag=NULL; r->tpag_ptr=NULL; r->n_tpag=0; return; }
  for(uint32_t i=0;i<n;i++){
    uint32_t p=u32(d,c->off+4+i*4); r->tpag_ptr[i]=p;
    GmlTpag *t=&r->tpag[i];
    t->sx=u16(d,p); t->sy=u16(d,p+2); t->sw=u16(d,p+4); t->sh=u16(d,p+6);
    t->tx=u16(d,p+8); t->ty=u16(d,p+10); t->bw=u16(d,p+16); t->bh=u16(d,p+18);
    t->atlas=(int16_t)u16(d,p+20);
  }
}
static int tpag_index_for_ptr(GmlRender *r, uint32_t ptr){
  for(int i=0;i<r->n_tpag;i++) if(r->tpag_ptr[i]==ptr) return i;
  return -1;
}
uint32_t gml_render_named_tpag_ptr(GmlRender *r, const char *name){
  if(!r || !name || !*name || !r->tpag_ptr) return 0;
  for(int i=0;i<r->n_spr;i++){
    GmlSprite *s=&r->spr[i];
    if(!s->name || strcmp(s->name,name) || !s->frame || s->n_frames<=0) continue;
    int ti=s->frame[0];
    return ti>=0 && ti<r->n_tpag ? r->tpag_ptr[ti] : 0;
  }
  return 0;
}

int gml_render_named_sprite(GmlRender *r, const char *name){
  if(!r || !name || !*name) return -1;
  for(int i=0;i<r->n_spr;i++)
    if(r->spr[i].name && !strcmp(r->spr[i].name,name)) return i;
  return -1;
}

static int effect_wrap_coord(int value, int size){
  if(size<=0) return 0;
  value%=size;
  return value<0?value+size:value;
}
static uint8_t effect_unorm8(float value){
  if(value<=0.0f) return 0;
  if(value>=255.0f) return 255;
  return (uint8_t)floorf(value+0.5f);
}
void gml_render_layer_tint(GmlRender *r, uint32_t rgba){
  if(!r || !r->fb || r->fbw<=0 || r->fbh<=0 || rgba==0xFFFFFFFFu) return;
  gml_render_prepare_draw(r);
  uint32_t ta=(rgba>>24)&255, tr=(rgba>>16)&255, tg=(rgba>>8)&255, tb=rgba&255;
  size_t count=(size_t)r->fbw*(size_t)r->fbh;
  for(size_t i=0;i<count;i++){
    uint32_t p=r->fb[i];
    uint32_t a=((p>>24)&255)*ta/255u;
    uint32_t rr=((p>>16)&255)*tr/255u;
    uint32_t rg=((p>>8)&255)*tg/255u;
    uint32_t rb=(p&255)*tb/255u;
    r->fb[i]=(a<<24)|(rr<<16)|(rg<<8)|rb;
  }
  if(ta<255u){ r->fb_opaque_known=0; r->fb_all_opaque=0; }
  r->fb_all_transparent=0;
}

static inline float layer_clamp01(float v){ return v<0.0f?0.0f:(v>1.0f?1.0f:v); }
static inline float layer_fract(float v){ return v-floorf(v); }
static inline float layer_mix(float a,float b,float t){ return a+(b-a)*t; }
static inline float layer_smoothstep(float a,float b,float x){
  if(a==b) return x<a?0.0f:1.0f;
  float t=layer_clamp01((x-a)/(b-a)); return t*t*(3.0f-2.0f*t);
}
static inline uint32_t layer_pack(const float c[4]){
  uint32_t a=effect_unorm8(layer_clamp01(c[3])*255.0f);
  uint32_t rr=effect_unorm8(layer_clamp01(c[0])*255.0f);
  uint32_t gg=effect_unorm8(layer_clamp01(c[1])*255.0f);
  uint32_t bb=effect_unorm8(layer_clamp01(c[2])*255.0f);
  return (a<<24)|(rr<<16)|(gg<<8)|bb;
}
static inline void layer_unpack(uint32_t p,float c[4]){
  c[0]=((p>>16)&255)*(1.0f/255.0f); c[1]=((p>>8)&255)*(1.0f/255.0f);
  c[2]=(p&255)*(1.0f/255.0f); c[3]=(p>>24)*(1.0f/255.0f);
}
static inline void layer_unpack_colour(uint32_t p,float c[4]){ layer_unpack(p,c); }

static int layer_filter_reserve(GmlRender *r,size_t count){
  if(!r || count==0 || count>67108864u) return 0;
  if(count<=r->layer_filter_capacity && r->layer_filter_src &&
     r->layer_filter_work && r->layer_filter_aux) return 1;
  uint32_t *src=malloc(count*sizeof(*src));
  uint32_t *work=malloc(count*sizeof(*work));
  uint32_t *aux=malloc(count*sizeof(*aux));
  if(!src || !work || !aux){ free(src); free(work); free(aux); return 0; }
  free(r->layer_filter_src); free(r->layer_filter_work); free(r->layer_filter_aux);
  r->layer_filter_src=src; r->layer_filter_work=work; r->layer_filter_aux=aux;
  r->layer_filter_capacity=count;
  return 1;
}

static void layer_sprite_texel(GmlRender *r,int sprite,int x,int y,int repeat,float out[4]){
  out[0]=out[1]=out[2]=out[3]=0.0f;
  if(!r || sprite<0 || sprite>=r->n_spr) return;
  GmlSprite *s=&r->spr[sprite]; int w=s->w,h=s->h;
  if(w<=0 || h<=0 || s->n_frames<=0) return;
  if(repeat){ x=effect_wrap_coord(x,w); y=effect_wrap_coord(y,h); }
  else { if(x<0)x=0; else if(x>=w)x=w-1; if(y<0)y=0; else if(y>=h)y=h-1; }
  const uint8_t *p=NULL;
  if(s->runtime_rgba) p=s->runtime_rgba+((size_t)y*w+x)*4u;
  else if(s->frame){
    int ti=s->frame[0];
    if(ti>=0 && ti<r->n_tpag){
      GmlTpag *t=&r->tpag[ti];
      int lx=x-t->tx,ly=y-t->ty;
      if(lx>=0 && ly>=0 && lx<t->sw && ly<t->sh && t->atlas>=0 && t->atlas<r->n_atlas){
        uint8_t *ap=atlas_pixels(r,t->atlas); GmlAtlas *a=&r->atlas[t->atlas];
        int ax=t->sx+lx,ay=t->sy+ly;
        if(ap && ax>=0 && ay>=0 && ax<a->w && ay<a->h) p=ap+((size_t)ay*a->w+ax)*4u;
      }
    }
  }
  if(p){ out[0]=p[0]*(1.0f/255.0f); out[1]=p[1]*(1.0f/255.0f);
         out[2]=p[2]*(1.0f/255.0f); out[3]=p[3]*(1.0f/255.0f); }
}
static void layer_sprite_sample(GmlRender *r,int sprite,float u,float v,int repeat,int linear,float out[4]){
  if(!r || sprite<0 || sprite>=r->n_spr || r->spr[sprite].w<=0 || r->spr[sprite].h<=0){
    out[0]=out[1]=out[2]=out[3]=0.0f; return;
  }
  int w=r->spr[sprite].w,h=r->spr[sprite].h;
  if(repeat){ u=layer_fract(u); v=layer_fract(v); }
  else { u=layer_clamp01(u); v=layer_clamp01(v); }
  if(!linear){ layer_sprite_texel(r,sprite,(int)floorf(u*w),(int)floorf(v*h),repeat,out); return; }
  float fx=u*w-0.5f,fy=v*h-0.5f; int x0=(int)floorf(fx),y0=(int)floorf(fy);
  float tx=fx-x0,ty=fy-y0,c[4][4];
  layer_sprite_texel(r,sprite,x0,y0,repeat,c[0]);
  layer_sprite_texel(r,sprite,x0+1,y0,repeat,c[1]);
  layer_sprite_texel(r,sprite,x0,y0+1,repeat,c[2]);
  layer_sprite_texel(r,sprite,x0+1,y0+1,repeat,c[3]);
  for(int k=0;k<4;k++) out[k]=layer_mix(layer_mix(c[0][k],c[1][k],tx),
                                        layer_mix(c[2][k],c[3][k],tx),ty);
}
static void layer_surface_sample(const uint32_t *src,int w,int h,float u,float v,int linear,float out[4]){
  if(!src || w<=0 || h<=0){ out[0]=out[1]=out[2]=out[3]=0; return; }
  u=layer_clamp01(u); v=layer_clamp01(v);
  if(!linear){
    int x=(int)floorf(u*w),y=(int)floorf(v*h);
    if(x>=w)x=w-1; if(y>=h)y=h-1; layer_unpack(src[(size_t)y*w+x],out); return;
  }
  float fx=u*w-0.5f,fy=v*h-0.5f; int x0=(int)floorf(fx),y0=(int)floorf(fy);
  float tx=fx-x0,ty=fy-y0; int x1=x0+1,y1=y0+1;
  if(x0<0)x0=0; else if(x0>=w)x0=w-1; if(x1<0)x1=0; else if(x1>=w)x1=w-1;
  if(y0<0)y0=0; else if(y0>=h)y0=h-1; if(y1<0)y1=0; else if(y1>=h)y1=h-1;
  float c[4][4]; layer_unpack(src[(size_t)y0*w+x0],c[0]); layer_unpack(src[(size_t)y0*w+x1],c[1]);
  layer_unpack(src[(size_t)y1*w+x0],c[2]); layer_unpack(src[(size_t)y1*w+x1],c[3]);
  for(int k=0;k<4;k++) out[k]=layer_mix(layer_mix(c[0][k],c[1][k],tx),
                                        layer_mix(c[2][k],c[3][k],tx),ty);
}

static void layer_composite_normal(GmlRender *r,const uint32_t *src,int w,int h,double alpha){
  if(!r || !r->fb || !src || w!=r->fbw || h!=r->fbh || alpha<=0.0) return;
  if(alpha>1.0) alpha=1.0;
  size_t count=(size_t)w*h;
  for(size_t i=0;i<count;i++){
    uint32_t sv=src[i]; float sa=(float)((sv>>24)&255)*(1.0f/255.0f)*(float)alpha;
    if(sa<=0.0f) continue;
    uint32_t dv=r->fb[i]; float ia=1.0f-sa;
    int sr=(sv>>16)&255,sg=(sv>>8)&255,sb=sv&255;
    int dr=(dv>>16)&255,dg=(dv>>8)&255,db=dv&255;
    int rr=(int)floorf(sr*sa+dr*ia+0.5f),gg=(int)floorf(sg*sa+dg*ia+0.5f);
    int bb=(int)floorf(sb*sa+db*ia+0.5f),aa=255;
    if(r->target_sp>0){ int da=(dv>>24)&255; aa=(int)floorf(sa*255.0f+da*ia+0.5f); if(aa>255)aa=255; }
    r->fb[i]=((uint32_t)aa<<24)|((uint32_t)rr<<16)|((uint32_t)gg<<8)|(uint32_t)bb;
  }
  r->fb_opaque_known=0; r->fb_all_transparent=0;
}
/* Room-layer capture targets contain the result of source-over drawing onto transparent black, so
 * their RGB channels are already alpha-weighted. Filter shaders preserve that representation.
 * Composite those results with the premultiplied source-over equation to avoid applying alpha a
 * second time around blurred/antialiased edges. */
static void layer_composite_premultiplied(GmlRender *r,const uint32_t *src,int w,int h,double alpha){
  if(!r || !r->fb || !src || w!=r->fbw || h!=r->fbh || alpha<=0.0) return;
  if(alpha>1.0) alpha=1.0;
  size_t count=(size_t)w*h;
  for(size_t i=0;i<count;i++){
    uint32_t sv=src[i],dv=r->fb[i]; float scale=(float)alpha;
    float sa=((sv>>24)&255)*(1.0f/255.0f)*scale,ia=1.0f-sa;
    if(sa<=0.0f) continue;
    int sr=(int)floorf(((sv>>16)&255)*scale+0.5f),sg=(int)floorf(((sv>>8)&255)*scale+0.5f);
    int sb=(int)floorf((sv&255)*scale+0.5f);
    int rr=sr+(int)floorf(((dv>>16)&255)*ia+0.5f);
    int gg=sg+(int)floorf(((dv>>8)&255)*ia+0.5f),bb=sb+(int)floorf((dv&255)*ia+0.5f);
    if(rr>255)rr=255;if(gg>255)gg=255;if(bb>255)bb=255;
    r->fb[i]=0xFF000000u|((uint32_t)rr<<16)|((uint32_t)gg<<8)|(uint32_t)bb;
  }
  r->fb_opaque_known=0; r->fb_all_transparent=0;
}
static void layer_composite_max(GmlRender *r,const uint32_t *src,int w,int h,double tint){
  if(!r || !r->fb || !src || w!=r->fbw || h!=r->fbh) return;
  float t=(float)tint; if(t<0)t=0; if(t>1)t=1;
  size_t count=(size_t)w*h;
  for(size_t i=0;i<count;i++){
    uint32_t s=src[i],d=r->fb[i];
    unsigned sr=(unsigned)floorf(((s>>16)&255)*t+0.5f),sg=(unsigned)floorf(((s>>8)&255)*t+0.5f);
    unsigned sb=(unsigned)floorf((s&255)*t+0.5f),sa=s>>24;
    unsigned dr=(d>>16)&255,dg=(d>>8)&255,db=d&255,da=d>>24;
    if(sr<dr)sr=dr; if(sg<dg)sg=dg; if(sb<db)sb=db; if(sa<da)sa=da;
    r->fb[i]=(sa<<24)|(sr<<16)|(sg<<8)|sb;
  }
  r->fb_opaque_known=0; r->fb_all_transparent=0;
}

int gml_render_layer_filter_begin(GmlRender *r,const GmlLayerFilter *filter){
  if(!r || !filter || filter->kind==GML_LAYER_FILTER_NONE || !r->fb ||
     r->fbw<=0 || r->fbh<=0 || r->layer_filter_active || r->target_sp>=GML_SURFACE_STACK) return 0;
  size_t count=(size_t)r->fbw*(size_t)r->fbh;
  if(!layer_filter_reserve(r,count)) return 0;
  gml_render_prepare_draw(r);
  int captured_known=r->fb_opaque_known,captured_opaque=r->fb_all_opaque;
  int captured_transparent=r->fb_all_transparent;
  if(filter->affects_below) memcpy(r->layer_filter_src,r->fb,count*sizeof(uint32_t));
  else memset(r->layer_filter_src,0,count*sizeof(uint32_t));
  r->target_stack[r->target_sp++]=(typeof(r->target_stack[0])){
    r->fb,r->fbw,r->fbh,r->cam_x,r->cam_y,r->projection_cam_x,r->projection_cam_y,
    r->target_id,r->fb_opaque_known,r->fb_all_opaque,r->fb_all_transparent,
    r->pending_underlay,r->underlay_x,r->underlay_y,r->underlay_w,r->underlay_h,
    r->pending_fill,r->pending_fill_color
  };
  r->fb=r->layer_filter_src; r->target_id=-2;
  r->fb_opaque_known=filter->affects_below?captured_known:1;
  r->fb_all_opaque=filter->affects_below?captured_opaque:0;
  r->fb_all_transparent=filter->affects_below?captured_transparent:1;
  r->pending_underlay=0; r->underlay_x=r->underlay_y=r->underlay_w=r->underlay_h=0;
  r->pending_fill=0; r->pending_fill_color=0; r->layer_filter_active=1;
  return 1;
}
static void layer_filter_restore_target(GmlRender *r){
  gml_render_flush_pending_underlay(r); gml_render_flush_pending_fill(r);
  if(r->target_sp>0){
    typeof(r->target_stack[0]) t=r->target_stack[--r->target_sp];
    r->fb=t.fb; r->fbw=t.w; r->fbh=t.h;
    r->projection_cam_x=t.projection_cx; r->projection_cam_y=t.projection_cy;
    r->cam_x=t.cx; r->cam_y=t.cy; r->target_id=t.target_id;
    r->fb_opaque_known=t.opaque_known; r->fb_all_opaque=t.all_opaque;
    r->fb_all_transparent=t.all_transparent; r->pending_underlay=t.pending_underlay;
    r->underlay_x=t.underlay_x; r->underlay_y=t.underlay_y;
    r->underlay_w=t.underlay_w; r->underlay_h=t.underlay_h;
    r->pending_fill=t.pending_fill; r->pending_fill_color=t.fill_color;
    { extern void gml_d3_sync_render_camera(GmlRender *); gml_d3_sync_render_camera(r); }
  }
}

static void layer_filter_tint_pixels(const uint32_t *src,uint32_t *dst,size_t count,uint32_t colour){
  float tint[4]; layer_unpack_colour(colour,tint);
  for(size_t i=0;i<count;i++){
    float c[4]; layer_unpack(src[i],c); for(int k=0;k<4;k++) c[k]*=tint[k]; dst[i]=layer_pack(c);
  }
}

/* Forward declaration for the persistent compositor worker pool defined below. Filter shaders can
 * be much heavier per row than a normal blit even at a small authored resolution. */
typedef void (*GmlRowBandFn)(void *ctx, int py0, int py1, int slot);
static void gml_run_row_bands_n(GmlRender *r,int H,int nt,GmlRowBandFn fn,void *ctx);

typedef struct { GmlRender *r; const uint32_t *src; uint32_t *dst; int w,h;
  const GmlLayerFilter *f; float time,camx,camy; } LayerCloudCtx;


typedef struct { int16_t ix,iy; uint16_t w00,w10,w01,w11; } LayerBlurTap;
typedef struct { GmlRender *r; const uint32_t *src; uint32_t *dst; int w,h,nw,nh;
  const GmlLayerFilter *f; const LayerBlurTap *tap; } LayerLargeBlurCtx;

typedef struct {
  const uint32_t *src; uint32_t *dst; int w,h;
  int ix[36],iy[36]; float weight00[36],weight10[36],weight01[36],weight11[36];
  float inv_radius[36],exp_lut[1025];
} LayerGlowCtx;


/* ---- SPRT ---- */
static void parse_sprt(GmlRender *r){
  const GmlChunk *c=gml_chunk(r->win,"SPRT"); if(!c) return;
  const uint8_t *d=r->win->data; uint32_t n=u32(d,c->off);
  r->n_spr=(int)n; r->spr=calloc(n,sizeof(GmlSprite)); r->spr_cap=(int)n; r->spr_has_free=0;
  if(!r->spr){ r->n_spr=0; r->spr_cap=0; return; }
  for(uint32_t i=0;i<n;i++){
    uint32_t p=u32(d,c->off+4+i*4);
    GmlSprite *s=&r->spr[i];
    s->name=gml_str_by_ptr(r->win,u32(d,p));
    s->playback_speed=1.0f; s->playback_speed_type=1; s->playback_speed_valid=0;
    s->w=(int)u32(d,p+4); s->h=(int)u32(d,p+8);
    s->ml=(int)u32(d,p+12); s->mr=(int)u32(d,p+16); s->mb=(int)u32(d,p+20); s->mt=(int)u32(d,p+24);
    s->collision_kind=0; s->collision_tolerance=63; /* preserve the old alpha>=64 fallback */
    /* margins(16) transparent/smooth/preload(12) bboxmode(4) sepmasks(4) -> originX/Y */
    /* GMS1 sprite header: ...,BBoxMode(40),SepMasks(44),OriginX(48),OriginY(52),frameList(56) */
    s->originx=(int)u32(d,p+48); s->originy=(int)u32(d,p+52);
    uint32_t list=p+56;             /* GMS1: SimpleList<TextureEntry> here (count + pointers) */
    /* GMS2 sprite: a -1 marker at +56, then SVersion(+60), SpriteType(+64), and for a normal sprite
     * PlaybackSpeed(+68 float)+PlaybackSpeedType(+72), plus SequenceOffset (SVersion>=2) and
     * NineSliceOffset (SVersion>=3) — the texture list only starts after all that. Reading +56 as the
     * frame count (=-1) would zero the frame list and blank every sprite. */
    if(u32(d,p+56)==0xFFFFFFFFu){
      uint32_t sver=u32(d,p+60), stype=u32(d,p+64);
      if(stype!=0){ s->n_frames=0; s->frame=calloc(1,sizeof(int)); continue; }  /* SWF/Spine: no simple list */
      float playback; memcpy(&playback,d+p+68,sizeof playback);
      uint32_t playback_type=u32(d,p+72);
      if(isfinite(playback) && playback>=0.0f && playback_type<=1){
        s->playback_speed=playback;
        s->playback_speed_type=(int)playback_type;
        s->playback_speed_valid=1;
      }
      uint32_t fl=76;                         /* after PlaybackSpeed(+68)+PlaybackSpeedType(+72) */
      if(sver>=2) fl+=4;                       /* SequenceOffset */
      if(sver>=3){                             /* NineSliceOffset */
        uint32_t nso=u32(d,p+80);
        if(nso && nso+40<=r->win->size && u32(d,nso+16)){
          /* NineSlice layout: Left,Top,Right,Bottom (i32), Enabled, TileModes[5]
           * (left,top,right,bottom,center). GM keeps the borders at native size when the
           * sprite draws scaled — stretching them uniformly deforms UI boards/bubbles. */
          s->ns_l=(int)(int32_t)u32(d,nso); s->ns_t=(int)(int32_t)u32(d,nso+4);
          s->ns_r=(int)(int32_t)u32(d,nso+8); s->ns_b=(int)(int32_t)u32(d,nso+12);
          for(int k=0;k<5;k++) s->ns_tile[k]=(int)(int32_t)u32(d,nso+20+4u*k);
          if(s->ns_l>=0 && s->ns_t>=0 && s->ns_r>=0 && s->ns_b>=0 &&
             s->ns_l+s->ns_r<=s->w && s->ns_t+s->ns_b<=s->h)
            s->ns_enabled=1;
        }
        fl+=4;
      }
      list=p+fl;
    }
    uint32_t fn=u32(d,list);
    if(fn>10000) fn=0;              /* guard against special-type sprites */
    s->n_frames=(int)fn; s->frame=calloc(fn?fn:1,sizeof(int));
    if(!s->frame){ s->n_frames=0; continue; }
    for(uint32_t f=0;f<fn;f++){
      uint32_t tptr=u32(d,list+4+f*4);
      s->frame[f]=tpag_index_for_ptr(r,tptr);
    }
    /* SPRT collision masks (after the frame list): count + count×(rowbytes·height) of 1bpp data.
     * GameMaker collides with THESE, not the visible sprite alpha (which can be decorative). */
    uint32_t maskoff=list+4+fn*4; uint32_t mc=u32(d,maskoff);
    if(mc>0 && mc<100000 && s->w>0 && s->h>0){
      s->mask_count=(int)mc; s->mask_rowb=(s->w+7)/8; s->mask=d+maskoff+4;
    }
  }
}
double gml_sprite_animation_delta(GmlRender *r, int sprite, double image_speed, double game_fps){
  if(!r || sprite<0 || sprite>=r->n_spr) return image_speed;
  GmlSprite *s=&r->spr[sprite];
  if(!s->playback_speed_valid) return image_speed;
  double delta=image_speed*(double)s->playback_speed;
  if(s->playback_speed_type==0){
    if(!(game_fps>0.0) || !isfinite(game_fps)) game_fps=60.0;
    delta/=game_fps;
  }
  return delta;
}
/* whether sprite's COLLISION MASK is solid at sprite-local (lx,ly). Asset sprites without a
 * serialized 1bpp mask use their bounding box; runtime sprites can still use alpha precision
 * through sprite_collision_mask(kind=bboxkind_precise). */
int gml_sprite_collision(GmlRender *r, int sprite, int frame, int lx, int ly){
  if(sprite<0||sprite>=r->n_spr) return 0;
  GmlSprite *s=&r->spr[sprite];
  if(lx<0||ly<0||lx>=s->w||ly>=s->h) return 0;
  if(lx<s->ml||lx>s->mr||ly<s->mt||ly>s->mb) return 0;
  if(s->collision_kind==1) return 1;
  if(s->collision_kind==2){
    double hw=(s->mr-s->ml+1)/2.0, hh=(s->mb-s->mt+1)/2.0;
    if(hw<=0||hh<=0) return 0;
    double cx=s->ml+hw-0.5, cy=s->mt+hh-0.5;
    double dx=(lx-cx)/hw, dy=(ly-cy)/hh;
    return dx*dx+dy*dy<=1.0;
  }
  if(s->collision_kind==3){
    double hw=(s->mr-s->ml+1)/2.0, hh=(s->mb-s->mt+1)/2.0;
    if(hw<=0||hh<=0) return 0;
    double cx=s->ml+hw-0.5, cy=s->mt+hh-0.5;
    return fabs((lx-cx)/hw)+fabs((ly-cy)/hh)<=1.0;
  }
  if(s->runtime_rgba) return gml_sprite_alpha(r,sprite,frame,lx,ly)>s->collision_tolerance;
  if(!s->mask||s->mask_count<=0) return 1;
  int mi=(s->mask_count>1 && frame>=0 && frame<s->mask_count)? frame : 0;
  const uint8_t *m=s->mask + (size_t)mi*s->mask_rowb*s->h;
  return (m[(size_t)ly*s->mask_rowb + lx/8] >> (7-(lx%8))) & 1;
}

/* ---- BGND ---- */
static void parse_bgnd(GmlRender *r){
  const GmlChunk *c=gml_chunk(r->win,"BGND"); if(!c) return;
  const uint8_t *d=r->win->data; uint32_t n=u32(d,c->off);
  r->n_bg=(int)n; r->bg=calloc(n,sizeof(GmlBg));
  if(!r->bg){ r->n_bg=0; return; }
  for(uint32_t i=0;i<n;i++){
	    uint32_t p=u32(d,c->off+4+i*4);
	    /* name, transparent, smooth, preload, texture(TPAG ptr) */
	    uint32_t tptr=u32(d,p+16);
	    r->bg[i].tpag=tpag_index_for_ptr(r,tptr);
	    if(anygm_policy_has_modern_layer_semantics(r->win) && p+64<c->off+c->size){
	      int ver=(int)u32(d,p+20), tw=(int)u32(d,p+24), th=(int)u32(d,p+28);
          /* The separated-border layout inserts separationX/Y before the output-border fields.
           * Validate both layouts structurally. Interpreting an older record as the new layout
           * makes its exported-sprite slot become ItemsPerTile (normally zero); a new record has
           * a complete count*frames table at +72. */
          int obx=(int)u32(d,p+32), oby=(int)u32(d,p+36), ocols=(int)u32(d,p+40);
          int oitems=(int)u32(d,p+44), ocount=(int)u32(d,p+48);
          uint64_t obytes=(uint64_t)(uint32_t)oitems*(uint64_t)(uint32_t)ocount*4u;
          int old_ok=ver>0 && tw>0 && th>0 && tw<=4096 && th<=4096 && obx>=0 && oby>=0 &&
                     ocols>0 && ocols<=4096 && oitems>0 && oitems<=1024 && ocount>0 &&
                     obytes<=4000000u && (uint64_t)p+64u+obytes<=(uint64_t)c->off+c->size;
          int nsep_x=(int)u32(d,p+32), nsep_y=(int)u32(d,p+36);
          int nbx=(int)u32(d,p+40), nby=(int)u32(d,p+44), ncols=(int)u32(d,p+48);
          int nitems=(int)u32(d,p+52), ncount=(int)u32(d,p+56);
          uint64_t nbytes=(uint64_t)(uint32_t)nitems*(uint64_t)(uint32_t)ncount*4u;
          int new_ok=ver>0 && tw>0 && th>0 && tw<=4096 && th<=4096 &&
                     nsep_x>=0 && nsep_x<=4096 && nsep_y>=0 && nsep_y<=4096 &&
                     nbx>=0 && nby>=0 && nbx<=4096 && nby<=4096 &&
                     ncols>0 && ncols<=4096 && nitems>0 && nitems<=1024 && ncount>0 &&
                     nbytes<=4000000u && (uint64_t)p+72u+nbytes<=(uint64_t)c->off+c->size;
          int modern=new_ok && (!old_ok || (ncount>ocount && ncols>ocols));
          if(old_ok || modern){
            int bx=modern?nbx:obx, by=modern?nby:oby, cols=modern?ncols:ocols;
            int items=modern?nitems:oitems, count=modern?ncount:ocount;
            r->bg[i].tile_w=tw; r->bg[i].tile_h=th;
            r->bg[i].tile_border_x=bx; r->bg[i].tile_border_y=by;
            r->bg[i].tile_separation_x=modern?nsep_x:0;
            r->bg[i].tile_separation_y=modern?nsep_y:0;
            r->bg[i].tile_columns=cols; r->bg[i].tile_items_per_tile=items; r->bg[i].tile_count=count;
            r->bg[i].tile_ids=d+p+(modern?72:64);
          }
	    }
	  }
	}

/* FONT records reference a TPAG page and glyph rectangles with advance and bearing.
 * Parse these records from the loaded container. Resource fonts occupy the initial
 * font indices; dynamically added sprite fonts follow them. */
static void parse_font(GmlRender *r){
  const GmlChunk *c=gml_chunk(r->win,"FONT"); if(!c) return;
  const uint8_t *d=r->win->data; uint32_t n=u32(d,c->off);
  int nf=(int)n; if(nf>GML_MAX_FONTS) nf=GML_MAX_FONTS;
  for(int i=0;i<nf;i++){
    uint32_t p=u32(d,c->off+4+i*4);
    GmlFont *f=&r->fonts[i];
    memset(f,0,sizeof(*f));
    for(int k=0;k<256;k++) f->glyph_by_char[k]=-1;
    f->real=1;
    f->sprite=-1;   /* real fonts have no sprite: keeps state records unambiguous vs sprite fonts */
    /* EmSize is u32 in bc14-16 and float, negated for point-sized fonts, in newer exports.
     * Reading the float bits as an integer shifts the glyph table and can produce zero glyphs. */
    int em_is_float=0;
    { uint32_t rawem=u32(d,p+8); float fem; memcpy(&fem,&rawem,4);
      if(fem<0){ f->line_height=(int)(0.5f-fem); em_is_float=1; }
      else if(fem>0 && fem<512.0f && rawem>0x30000000u){ f->line_height=(int)(fem+0.5f); em_is_float=1; }
      else f->line_height=(int)rawem; }
    uint32_t texptr=u32(d,p+28);                    /* glyph-page TPAG record */
    int tsx=u16(d,texptr), tsy=u16(d,texptr+2), tatlas=(int16_t)u16(d,texptr+20);
    f->atlas=tatlas;
    /* glyph table position: +40 (bc14-16), +44 (GMS2 compatibility exports with one field after
     * the scales), +48 (exports with AscenderOffset+Ascender after the scales), +52 (exports with
     * SDFSpread too), +56 (headroom for the next added field).
     * Detect per record: the count must be sane, followed by an in-file pointer list whose
     * first glyph has a plausible char code. First sane candidate wins; with no sane char
     * code anywhere, the first structurally plausible one does. Restricting detection to the
     * earlier offsets can otherwise produce an empty glyph table for a valid record. */
    uint32_t goff=40;
    { int first_ok=-1, best=-1;
      static const uint32_t cand[5]={40,44,48,52,56};
      for(int ci=0;ci<5 && best<0;ci++){ uint32_t co=cand[ci];
        uint32_t cnt=u32(d,p+co);
        if(cnt==0 || cnt>=100000) continue;
        uint32_t q=u32(d,p+co+4);
        if(!(q>p && q+14<=r->win->size)) continue;
        if(first_ok<0) first_ok=(int)co;
        if(u16(d,q)<=0x2FFF) best=(int)co;
      }
      if(best>=0) goff=(uint32_t)best;
      else if(first_ok>=0) goff=(uint32_t)first_ok;
    }
    /* Bytecode 17 added AscenderOffset immediately before the glyph array.  Older records put
     * the array at +40, while every later layout retains the offset at +40 and moves the array
     * farther right as more metrics are appended. */
    int serialized_line_height=0;
    if(goff>40){
      int32_t ascender=(int32_t)u32(d,p+40);
      if(ascender>-32768 && ascender<32768) f->ascender_offset=(int)ascender;
    }
    /* Newer FONT records serialize their actual line advance after AscenderOffset,
     * Ascender and SDFSpread.  The point/em size and the tallest packed glyph are not equivalent:
     * using either for vertical alignment moves centred labels by a logical pixel and spaces
     * multiline text incorrectly.  Layouts ending before +56 do not yet carry this field. */
    if(goff>=56){
      int32_t line_height=(int32_t)u32(d,p+52);
      if(line_height>0 && line_height<=4096) serialized_line_height=(int)line_height;
    }
    uint32_t gc=u32(d,p+goff);
    if(gc>100000) gc=0;                             /* guard */
    f->glyphs=calloc(gc?gc:1,sizeof(GmlGlyph));
    f->n_glyphs=(int)gc;
    f->glyphs_sorted=1;
    uint16_t previous_ch=0;
    for(uint32_t g=0;g<gc && f->glyphs;g++){
      uint32_t q=u32(d,p+goff+4+g*4);               /* pointer to glyph record */
      GmlGlyph *gl=&f->glyphs[g];
      gl->ch=u16(d,q);
      gl->sx=tsx+(int)u16(d,q+2); gl->sy=tsy+(int)u16(d,q+4);   /* absolute atlas coords */
      gl->w=(int)u16(d,q+6); gl->h=(int)u16(d,q+8);
      gl->shift=(int16_t)u16(d,q+10);               /* advance */
      gl->offset=(int16_t)u16(d,q+12);              /* left bearing */
      if(g && gl->ch<previous_ch) f->glyphs_sorted=0;
      previous_ch=gl->ch;
      if(gl->ch<256) f->glyph_by_char[gl->ch]=(int)g;
    }
    int mh=0;
    for(int g2=0;g2<f->n_glyphs;g2++) if(f->glyphs[g2].h>mh) mh=f->glyphs[g2].h;
    if(serialized_line_height){
      f->line_height=serialized_line_height;
      f->align_height=serialized_line_height;
    }else{
      f->align_height=mh>f->line_height?mh:f->line_height;
      /* Early float-em fonts do not serialize a line advance. Their point size can be smaller
       * than the rendered glyphs, so retain the tallest-glyph fallback for those layouts. */
      if(em_is_float && mh>f->line_height) f->line_height=mh;
    }
    if(render_setting(r,"GML_LOG_FONT"))
      anygm_host_logf(r && r->win ? r->win->host : NULL,ANYGM_LOG_DEBUG,"[font] real id=%d name=%s line=%d align=%d maxglyph=%d ascender_offset=%d atlas=%d glyphs=%d\n",
        i, gml_str_by_ptr(r->win,u32(d,p)), f->line_height, f->align_height,mh,
        f->ascender_offset,f->atlas,f->n_glyphs);
    if(render_setting(r,"GML_LOG_FONT_GLYPHS"))
      for(int ch=32;ch<127;ch++){
        int gi=f->glyph_by_char[ch];
        if(gi>=0){ GmlGlyph *gl=&f->glyphs[gi];
          anygm_host_logf(r && r->win ? r->win->host : NULL,ANYGM_LOG_DEBUG,"[fontglyph] font=%d ch=%d('%c') gi=%d rect=(%d,%d %dx%d) shift=%d off=%d\n",
                  i,ch,ch,gi,gl->sx,gl->sy,gl->w,gl->h,gl->shift,gl->offset); }
      }
  }
  if(r->n_fonts<nf) r->n_fonts=nf;                  /* sprite fonts number after real fonts */
}

/* The built-in default font remains available when FONT contains no user resources. Keep it
 * separate from the asset-id namespace: draw_set_font(-1) selects this data, while ids 0..n-1
 * continue to resolve only to fonts supplied by the loaded package. */
static int build_default_font(GmlRender *r){
  enum { AW=512, AH=128 };
  const int ng=GML_DEFAULT_FONT_LAST-GML_DEFAULT_FONT_FIRST+1;
  GmlGlyph *glyphs=calloc((size_t)ng,sizeof(*glyphs));
  uint8_t *px=calloc((size_t)AW*AH,4);
  if(!glyphs || !px){ free(glyphs); free(px); return 0; }
  GmlAtlas *na=realloc(r->atlas,(size_t)(r->n_atlas+1)*sizeof(*na));
  if(!na){ free(glyphs); free(px); return 0; }
  r->atlas=na;
  int atlas_id=r->n_atlas++;
  GmlAtlas *a=&r->atlas[atlas_id];
  memset(a,0,sizeof(*a));
  a->px=px; a->w=AW; a->h=AH; a->decode_attempted=1;

  GmlFont *f=&r->default_font;
  memset(f,0,sizeof(*f));
  for(int i=0;i<256;i++) f->glyph_by_char[i]=-1;
  f->real=1; f->sprite=-1; f->atlas=atlas_id;
  f->line_height=GML_DEFAULT_FONT_LINE_HEIGHT;
  f->align_height=f->line_height;
  f->glyphs=glyphs; f->n_glyphs=ng; f->glyphs_sorted=1;
  int ax=0, ay=0, row_height=0;
  for(int i=0;i<ng;i++){
    const GmlDefaultGlyph *src=&gml_default_glyphs[i];
    int w=src->width, h=src->height;
    if(ax+w>AW){ ax=0; ay+=row_height; row_height=0; }
    if(h>row_height) row_height=h;
    if(ay+h>AH) break;
    GmlGlyph *g=&glyphs[i];
    g->ch=(uint16_t)(GML_DEFAULT_FONT_FIRST+i);
    g->sx=ax; g->sy=ay; g->w=w; g->h=h;
    g->shift=src->shift; g->offset=src->offset;
    f->glyph_by_char[g->ch]=i;
    const uint8_t *cov=gml_default_font_alpha+src->off;
    for(int y=0;y<h;y++) for(int x=0;x<w;x++){
      uint8_t alpha=cov[y*w+x];
      if(alpha){
        uint8_t *q=px+((size_t)(ay+y)*AW+ax+x)*4;
        q[0]=q[1]=q[2]=255; q[3]=alpha;
      }
    }
    ax+=w;
  }
  if(render_setting(r,"GML_LOG_FONT"))
    anygm_host_logf(r && r->win ? r->win->host : NULL,ANYGM_LOG_DEBUG,"[font] built-in atlas=%d glyphs=%d line=%d\n",atlas_id,ng,f->line_height);
  return 1;
}

/* ---- recognized display post-processes: none retained in this revision ---- */

/* ---- real FONT-chunk font helpers ---- */
static GmlGlyph *real_glyph(GmlFont *f, unsigned cp){
  if(cp<256){ int gi=f->glyph_by_char[cp]; return gi>=0? &f->glyphs[gi] : NULL; }
  if(f->glyphs_sorted){
    int lo=0, hi=f->n_glyphs;
    while(lo<hi){
      int mid=lo+(hi-lo)/2;
      unsigned ch=f->glyphs[mid].ch;
      if(ch<cp) lo=mid+1; else hi=mid;
    }
    return lo<f->n_glyphs && f->glyphs[lo].ch==cp ? &f->glyphs[lo] : NULL;
  }
  for(int i=0;i<f->n_glyphs;i++) if(f->glyphs[i].ch==cp) return &f->glyphs[i];
  return NULL;
}
/* advance width of one line (up to '#', LF, or NUL), '\#' counts as a literal '#'. */
static int real_line_width(GmlFont *f, const char *p, const char **end){
  int w=0;
  while(*p && !text_is_linebreak(p)){
    unsigned cp;
    if(p[0]=='\\' && p[1]=='#'){ cp='#'; p+=2; }
    else cp=text_next_cp(&p);
    GmlGlyph *g=real_glyph(f,cp);
    if(g) w+=g->shift;
  }
  *end=p; return w;
}

/* ClearType uses a separate coverage value for each LCD channel.  The bundled
 * classic-info atlases retain those three coverages; apply them to the requested
 * foreground colour over the already-painted information-page background. */
static int classic_info_gdi_black_component(int value){
  /* Match the integer endpoints of GDI's six-level ClearType transfer ramp. */
  if(value==15) return 14;
  if(value==26) return 25;
  if(value==46) return 45;
  if(value==56) return 54;
  if(value==29) return 28;
  if(value==72) return 71;
  return value;
}

static int classic_info_subpixel_glyph(GmlRender *r,GmlFont *font,GmlGlyph *glyph,
                                       double x,double y,double xscale,double yscale,
                                       uint32_t color,double alpha){
  if(!r||!font||!glyph||!font->subpixel||font->atlas<0||font->atlas>=r->n_atlas||
     alpha<0.999||!r->alphablend||r->blendmode!=0||xscale<=0||yscale<=0) return 0;
  GmlAtlas *atlas=&r->atlas[font->atlas];
  if(!atlas_pixels(r,font->atlas)) return 0;
  int x0=(int)floor(x+0.5),y0=(int)floor(y+0.5);
  int destination_width=(int)ceil(glyph->w*xscale);
  int destination_height=(int)ceil(glyph->h*yscale);
  int first_x=x0<0?-x0:0,first_y=y0<0?-y0:0;
  int last_x=destination_width,last_y=destination_height;
  if(x0+last_x>r->fbw) last_x=r->fbw-x0;
  if(y0+last_y>r->fbh) last_y=r->fbh-y0;
  if(first_x>=last_x||first_y>=last_y) return 1;
  int foreground_r=color&255,foreground_g=(color>>8)&255,foreground_b=(color>>16)&255;
  gml_render_maybe_prepare_draw(r);
  for(int yy=first_y;yy<last_y;yy++){
    uint32_t *destination=r->fb+(size_t)(y0+yy)*r->fbw+x0+first_x;
    int source_y=(int)floor((yy+0.5)/yscale);
    if(source_y<0) source_y=0;
    if(source_y>=glyph->h) source_y=glyph->h-1;
    for(int xx=first_x;xx<last_x;xx++,destination++){
      int source_x=(int)floor((xx+0.5)/xscale);
      if(source_x<0) source_x=0;
      if(source_x>=glyph->w) source_x=glyph->w-1;
      const uint8_t *source=atlas->px+
        ((size_t)(glyph->sy+source_y)*atlas->w+glyph->sx+source_x)*4;
      if(!source[3]) continue;
      int coverage_r=255-source[0],coverage_g=255-source[1],coverage_b=255-source[2];
      uint32_t previous=*destination;
      int background_r=(previous>>16)&255,background_g=(previous>>8)&255,background_b=previous&255;
      int output_r=(foreground_r*coverage_r+background_r*(255-coverage_r)+127)/255;
      int output_g=(foreground_g*coverage_g+background_g*(255-coverage_g)+127)/255;
      int output_b=(foreground_b*coverage_b+background_b*(255-coverage_b)+127)/255;
      if(foreground_r==0) output_r=classic_info_gdi_black_component(output_r);
      if(foreground_g==0) output_g=classic_info_gdi_black_component(output_g);
      if(foreground_b==0) output_b=classic_info_gdi_black_component(output_b);
      *destination=0xFF000000u|((uint32_t)output_r<<16)|((uint32_t)output_g<<8)|(uint32_t)output_b;
    }
  }
  return 1;
}

/* Draw glyph atlas rectangles top-aligned and advance the pen by each shift.
 * Rotation changes pen positions; glyph rectangles remain axis-aligned. */
static void draw_text_real(GmlRender *r, GmlFont *f, double x, double y, const char *str,
                           double xs, double ys, double ca, double sa, int use_rot,
                           uint32_t blend, double alpha){
  int lh=f->line_height>0? f->line_height:12;
  int ah=f->align_height>0?f->align_height:lh;
  int nlines=1; for(const char *q=str;*q;q++){ if(*q=='\\'&&q[1]=='#'){q++;continue;} if(text_is_linebreak(q)) nlines++; }
  double base_y=0;
  double block_height=(nlines-1)*lh+ah;
  if(r->valign==1) base_y=-block_height/2.0; else if(r->valign==2) base_y=-block_height;
  const char *p=str;
  for(int li=0; *p || li==0; li++){
    const char *end; int lw=real_line_width(f,p,&end);
    double cx=0;
    if(r->halign==1) cx=-lw/2.0; else if(r->halign==2) cx=-lw;
    while(p<end){
      unsigned cp=text_next_cp(&p);
      GmlGlyph *g=real_glyph(f,cp);
      if(g && g->w>0 && g->h>0){
        GmlTpag gt={ .sx=g->sx,.sy=g->sy,.sw=g->w,.sh=g->h,.tx=0,.ty=0,.bw=g->w,.bh=g->h,.atlas=f->atlas };
        double dx=(cx+g->offset)*xs, dy=(base_y-f->ascender_offset)*ys;
        double glyph_x=use_rot?x+dx*ca+dy*sa:x+dx;
        double glyph_y=use_rot?y-dx*sa+dy*ca:y+dy;
        uint32_t glyph_blend=f->subpixel?0xFFFFFFu:blend;
        if(f->subpixel && r->software_overlay && !use_rot &&
           classic_info_subpixel_glyph(r,f,g,glyph_x-r->cam_x,glyph_y-r->cam_y,
                                       xs,ys,blend,alpha)){
          /* Per-channel coverage was composed directly above. */
        } else if(r->software_overlay ||
           !gml_d3_draw_atlas_part_2d(r,f->atlas,g->sx,g->sy,g->w,g->h,
                                      glyph_x,glyph_y,xs,ys,glyph_blend,alpha)){
          if(use_rot) blit(r,&gt,glyph_x-r->cam_x,glyph_y-r->cam_y,xs,ys,glyph_blend,alpha);
          else        blit(r,&gt,glyph_x-r->cam_x,glyph_y-r->cam_y,xs,ys,glyph_blend,alpha);
        }
      }
      if(g) cx += g->shift;
    }
    base_y += lh;
    if(text_is_linebreak(end)) p=end+1; else break;
  }
}

static GmlFont *active_font(GmlRender *r){
  if(!r) return NULL;
  if(r->font<0)
    return r->default_font.glyphs && r->default_font.n_glyphs>0 ? &r->default_font : NULL;
  return r->font<r->n_fonts ? &r->fonts[r->font] : NULL;
}

int gml_text_width(GmlRender *r, const char *str){
  GmlFont *f=active_font(r);
  if(!f||!str) return 0;
  int best=0; const char *p=str;
  for(;;){
    const char *end; int w = f->real ? real_line_width(f,p,&end) : line_width(r,f,p,&end);
    if(w>best) best=w;
    if(!text_is_linebreak(end)) break;
    p=end+1;
  }
  if(render_setting(r,"GML_LOG_WIDTH")){
    int max=200; const char *m=render_setting(r,"GML_LOG_WIDTH_MAX"); if(m) max=atoi(m);
    if(r->text_width_log_count<max){
      int sw=-1, nf=-1;
      if(!f->real && f->sprite>=0 && f->sprite<r->n_spr){ sw=r->spr[f->sprite].w; nf=r->spr[f->sprite].n_frames; }
      anygm_host_logf(r && r->win ? r->win->host : NULL,ANYGM_LOG_DEBUG,"[width] font=%d real=%d sprite=%d sw=%d frames=%d prop=%d sep=%d map=%d width=%d \"%.*s\"\n",
        r->font,f->real,f->sprite,sw,nf,f->prop,f->sep,f->map_len,best,80,str);
      r->text_width_log_count++;
    }
  }
  return best;
}
int gml_text_height(GmlRender *r, const char *str){
  GmlFont *f=active_font(r);
  if(!f) return 0;
  int lh, nlines=1;
  if(f->real) lh=f->line_height>0?f->line_height:12;
  else { GmlSprite *s=&r->spr[f->sprite]; lh=s->h>0?s->h:8; }
  if(str) for(const char *p=str;*p;p++){ if(*p=='\\'&&p[1]=='#'){p++;continue;} if(text_is_linebreak(p)) nlines++; }
  return lh*nlines;
}

#define CLASSIC_INFO_MAX_LINES 128
#define CLASSIC_INFO_LINE_BYTES 768
#define CLASSIC_INFO_MAX_RUNS 32
typedef struct {
  int start, length, font_size, bold, italic, underline;
  uint32_t color;
} ClassicInfoRun;
typedef struct {
  char text[CLASSIC_INFO_LINE_BYTES];
  int length, font_size, bold, italic, align;
  uint32_t color;
  ClassicInfoRun runs[CLASSIC_INFO_MAX_RUNS]; int run_count;
} ClassicInfoLine;
typedef struct { int font_size, bold, italic, underline, align; uint32_t color; } ClassicInfoStyle;

static void classic_info_line_style(ClassicInfoLine *line,const ClassicInfoStyle *style){
  if(line->length) return;
  line->font_size=style->font_size;
  line->bold=style->bold;
  line->italic=style->italic;
  line->align=style->align;
  line->color=style->color;
}
static void classic_info_append(ClassicInfoLine *line,const ClassicInfoStyle *style,
                                const char *bytes,size_t count){
  classic_info_line_style(line,style);
  if(count>(size_t)(CLASSIC_INFO_LINE_BYTES-1-line->length))
    count=(size_t)(CLASSIC_INFO_LINE_BYTES-1-line->length);
  if(count){
    ClassicInfoRun *run=line->run_count?&line->runs[line->run_count-1]:NULL;
    if(!run||run->font_size!=style->font_size||run->bold!=style->bold||
       run->italic!=style->italic||run->underline!=style->underline||
       run->color!=style->color){
      if(line->run_count<CLASSIC_INFO_MAX_RUNS){
        run=&line->runs[line->run_count++];
        *run=(ClassicInfoRun){line->length,0,style->font_size,style->bold,
                              style->italic,style->underline,style->color};
      }
    }
    memcpy(line->text+line->length,bytes,count); line->length+=(int)count;
    if(run) run->length=line->length-run->start;
  }
  line->text[line->length]='\0';
}
static void classic_info_finish_line(ClassicInfoLine *lines,int *count,
                                     ClassicInfoLine *line,const ClassicInfoStyle *style){
  if(*count>=CLASSIC_INFO_MAX_LINES) return;
  classic_info_line_style(line,style);
  while(line->length>0 && (line->text[line->length-1]==' ' || line->text[line->length-1]=='\t'))
    line->text[--line->length]='\0';
  while(line->run_count>0){
    ClassicInfoRun *run=&line->runs[line->run_count-1];
    if(run->start>=line->length){ line->run_count--; continue; }
    run->length=line->length-run->start;
    break;
  }
  lines[(*count)++]=*line;
  memset(line,0,sizeof(*line));
  classic_info_line_style(line,style);
}
static int classic_info_word(const uint8_t *text,size_t size,size_t *at,
                             char *word,size_t word_cap,int *parameter,int *has_parameter){
  size_t i=*at,n=0;
  while(i<size && ((text[i]>='A'&&text[i]<='Z')||(text[i]>='a'&&text[i]<='z'))){
    if(n+1<word_cap) word[n++]=(char)text[i];
    i++;
  }
  word[n]='\0';
  int sign=1,value=0,have=0;
  if(i<size && text[i]=='-'){ sign=-1; i++; }
  while(i<size && text[i]>='0'&&text[i]<='9'){
    have=1;
    if(value<1000000) value=value*10+(text[i]-'0');
    i++;
  }
  if(i<size && text[i]==' ') i++;
  *at=i; *parameter=value*sign; *has_parameter=have;
  return n>0;
}
static int classic_info_parse(const uint8_t *record,size_t record_size,
                              ClassicInfoLine *lines,int *line_count){
  if(!record || record_size<12) return 0;
  uint32_t caption=u32(record,8);
  size_t text_length_at=12u+(size_t)caption+8u*4u+8u;
  if(text_length_at>record_size || record_size-text_length_at<4u) return 0;
  uint32_t text_length=u32(record,(uint32_t)text_length_at);
  size_t text_at=text_length_at+4u;
  if((size_t)text_length>record_size-text_at) return 0;
  const uint8_t *text=record+text_at;
  size_t size=text_length,start=0;
  for(size_t i=0;i+5<=size;i++) if(text[i]=='\\' && !memcmp(text+i+1,"pard",4)){
    start=i; break;
  }
  uint32_t colors[32]={0}; int color_count=1;
  for(size_t i=0;i+4<size && color_count<(int)(sizeof(colors)/sizeof(colors[0]));i++){
    if(text[i]!='\\'||memcmp(text+i+1,"red",3)) continue;
    size_t at=i+4; int red=0,green=0,blue=0,have=0;
    while(at<size&&text[at]>='0'&&text[at]<='9'){ have=1; red=red*10+text[at++]-'0'; }
    if(!have||at+6>=size||memcmp(text+at,"\\green",6)) continue;
    at+=6; have=0;
    while(at<size&&text[at]>='0'&&text[at]<='9'){ have=1; green=green*10+text[at++]-'0'; }
    if(!have||at+5>=size||memcmp(text+at,"\\blue",5)) continue;
    at+=5; have=0;
    while(at<size&&text[at]>='0'&&text[at]<='9'){ have=1; blue=blue*10+text[at++]-'0'; }
    if(!have) continue;
    if(red>255) red=255;
    if(green>255) green=255;
    if(blue>255) blue=255;
    colors[color_count++]=(uint32_t)red|((uint32_t)green<<8)|((uint32_t)blue<<16);
    i=at;
  }
  ClassicInfoStyle style={24,0,0,0,0,0};
  ClassicInfoStyle stack[32]; int stack_count=0;
  ClassicInfoLine line; memset(&line,0,sizeof(line));
  classic_info_line_style(&line,&style);
  int count=0;
  for(size_t i=start;i<size && count<CLASSIC_INFO_MAX_LINES;){
    unsigned char ch=text[i++];
    if(ch=='\r'||ch=='\n') continue;
    if(ch=='{'){
      if(stack_count<(int)(sizeof(stack)/sizeof(stack[0]))) stack[stack_count++]=style;
      continue;
    }
    if(ch=='}'){
      if(stack_count>0) style=stack[--stack_count];
      classic_info_line_style(&line,&style);
      continue;
    }
    if(ch!='\\'){
      char literal=(char)ch;
      classic_info_append(&line,&style,&literal,1);
      continue;
    }
    if(i>=size) break;
    ch=text[i];
    if(ch=='\\'||ch=='{'||ch=='}'){
      i++; char literal=(char)ch;
      classic_info_append(&line,&style,&literal,1);
      continue;
    }
    if(ch=='\'' && i+2<size){
      int hi=text[i+1],lo=text[i+2];
      hi=(hi>='0'&&hi<='9')?hi-'0':((hi|32)>='a'&&(hi|32)<='f')?(hi|32)-'a'+10:-1;
      lo=(lo>='0'&&lo<='9')?lo-'0':((lo|32)>='a'&&(lo|32)<='f')?(lo|32)-'a'+10:-1;
      if(hi>=0&&lo>=0){ char literal=(char)((hi<<4)|lo); classic_info_append(&line,&style,&literal,1); }
      i+=3; continue;
    }
    if(ch=='~'||ch=='_'){
      i++; char literal=ch=='~'?' ':'-'; classic_info_append(&line,&style,&literal,1); continue;
    }
    if(ch=='-'||ch=='*'){ i++; continue; }
    char word[24]; int parameter=0,has_parameter=0;
    if(!classic_info_word(text,size,&i,word,sizeof(word),&parameter,&has_parameter)){
      i++; continue;
    }
    if(!strcmp(word,"par")||!strcmp(word,"line")){
      classic_info_finish_line(lines,&count,&line,&style);
    } else if(!strcmp(word,"fs") && has_parameter){
      if(parameter<8) parameter=8;
      if(parameter>144) parameter=144;
      style.font_size=parameter; classic_info_line_style(&line,&style);
    } else if(!strcmp(word,"b")){
      style.bold=!has_parameter||parameter!=0; classic_info_line_style(&line,&style);
    } else if(!strcmp(word,"i")){
      style.italic=!has_parameter||parameter!=0; classic_info_line_style(&line,&style);
    } else if(!strcmp(word,"ul")){
      style.underline=!has_parameter||parameter!=0; classic_info_line_style(&line,&style);
    } else if(!strcmp(word,"ulnone")){
      style.underline=0; classic_info_line_style(&line,&style);
    } else if(!strcmp(word,"qc")){
      style.align=1; classic_info_line_style(&line,&style);
    } else if(!strcmp(word,"qr")){
      style.align=2; classic_info_line_style(&line,&style);
    } else if(!strcmp(word,"ql")||!strcmp(word,"pard")){
      style.align=0; classic_info_line_style(&line,&style);
    } else if(!strcmp(word,"plain")){
      style.font_size=24; style.bold=style.italic=style.underline=0; style.color=0;
      classic_info_line_style(&line,&style);
    } else if(!strcmp(word,"cf") && has_parameter){
      if(parameter>=0&&parameter<color_count) style.color=colors[parameter];
      classic_info_line_style(&line,&style);
    } else if(!strcmp(word,"tab")){
      classic_info_append(&line,&style,"    ",4);
    } else if(!strcmp(word,"emdash")){
      classic_info_append(&line,&style,"--",2);
    } else if(!strcmp(word,"endash")){
      classic_info_append(&line,&style,"-",1);
    } else if(!strcmp(word,"bullet")){
      classic_info_append(&line,&style,"*",1);
    }
  }
  if(line.length && count<CLASSIC_INFO_MAX_LINES)
    classic_info_finish_line(lines,&count,&line,&style);
  *line_count=count;
  return count>0;
}

static int classic_info_font_build(GmlRender *r,int source_index){
  for(int i=0;i<r->classic_info_font_cache_count;i++){
    if(r->classic_info_font_cache[i].source_index==source_index)
      return r->classic_info_font_cache[i].font_id;
  }
  if(source_index<0||source_index>=GML_CLASSIC_INFO_FONT_SOURCE_COUNT||
     r->n_fonts>=GML_MAX_FONTS) return -1;
  const GmlClassicInfoFontSource *source=&gml_classic_info_font_sources[source_index];
  const int first=GML_CLASSIC_INFO_FONT_FIRST,last=GML_CLASSIC_INFO_FONT_LAST;
  const int glyph_count=last-first+1,atlas_width=512;
  int ax=0,ay=0,row_height=0;
  for(int i=0;i<glyph_count;i++){
    const GmlClassicInfoGlyph *glyph=&source->glyphs[i];
    if(ax+glyph->width>atlas_width){ ax=0; ay+=row_height; row_height=0; }
    if(glyph->height>row_height) row_height=glyph->height;
    ax+=glyph->width;
  }
  int atlas_height=ay+row_height;
  if(atlas_height<1) return -1;
  int power_of_two=64; while(power_of_two<atlas_height) power_of_two*=2;
  atlas_height=power_of_two;
  uint8_t *pixels=calloc((size_t)atlas_width*atlas_height,4);
  GmlGlyph *glyphs=calloc((size_t)glyph_count,sizeof(*glyphs));
  if(!pixels||!glyphs){ free(pixels); free(glyphs); return -1; }
  GmlAtlas *atlases=realloc(r->atlas,(size_t)(r->n_atlas+1)*sizeof(*atlases));
  if(!atlases){ free(pixels); free(glyphs); return -1; }
  r->atlas=atlases;
  int atlas_id=r->n_atlas++;
  GmlAtlas *atlas=&r->atlas[atlas_id];
  memset(atlas,0,sizeof(*atlas));
  atlas->w=atlas_width; atlas->h=atlas_height; atlas->px=pixels; atlas->decode_attempted=1;
  ax=0; ay=0; row_height=0;
  for(int i=0;i<glyph_count;i++){
    const GmlClassicInfoGlyph *source_glyph=&source->glyphs[i];
    int width=source_glyph->width,height=source_glyph->height;
    if(ax+width>atlas_width){ ax=0; ay+=row_height; row_height=0; }
    if(height>row_height) row_height=height;
    GmlGlyph *glyph=&glyphs[i];
    glyph->ch=(uint16_t)(first+i); glyph->sx=ax; glyph->sy=ay;
    glyph->w=width; glyph->h=height;
    glyph->shift=source_glyph->shift; glyph->offset=source_glyph->offset;
    const uint8_t *coverage=source->rgb_coverage+source_glyph->off;
    for(int y=0;y<height;y++) for(int x=0;x<width;x++){
      const uint8_t *sample=coverage+((size_t)y*width+x)*3;
      if(sample[0]||sample[1]||sample[2]){
        uint8_t *pixel=pixels+((size_t)(ay+y)*atlas_width+ax+x)*4;
        pixel[0]=(uint8_t)(255-sample[0]);
        pixel[1]=(uint8_t)(255-sample[1]);
        pixel[2]=(uint8_t)(255-sample[2]);
        pixel[3]=255;
      }
    }
    ax+=width;
  }
  int id=r->n_fonts++;
  GmlFont *font=&r->fonts[id];
  memset(font,0,sizeof(*font));
  for(int i=0;i<256;i++) font->glyph_by_char[i]=-1;
  font->real=1; font->sprite=-1; font->atlas=atlas_id; font->subpixel=1;
  font->line_height=source->line_height; font->align_height=source->line_height;
  font->glyphs=glyphs; font->n_glyphs=glyph_count; font->glyphs_sorted=1;
  for(int i=0;i<glyph_count;i++) font->glyph_by_char[first+i]=i;
  if(r->classic_info_font_cache_count<(int)(sizeof(r->classic_info_font_cache)/
                                             sizeof(r->classic_info_font_cache[0]))){
    typeof(r->classic_info_font_cache[0]) *cached=
      &r->classic_info_font_cache[r->classic_info_font_cache_count++];
    cached->source_index=source_index; cached->font_id=id;
  }
  return id;
}

static int classic_info_font(GmlRender *r,int half_points,int bold,int italic,
                             double *scale,int *source_index){
  int best=-1,best_distance=INT_MAX;
  for(int i=0;i<GML_CLASSIC_INFO_FONT_SOURCE_COUNT;i++){
    const GmlClassicInfoFontSource *source=&gml_classic_info_font_sources[i];
    if(source->bold!=!!bold||source->italic!=!!italic) continue;
    int distance=abs(source->half_points-half_points);
    if(distance<best_distance){ best=i; best_distance=distance; }
  }
  if(best<0) return -1;
  if(source_index) *source_index=best;
  if(scale) *scale=(double)half_points/gml_classic_info_font_sources[best].half_points;
  return classic_info_font_build(r,best);
}

typedef struct { int font_id; double scale; int line_height,y_offset; } ClassicInfoResolvedFont;

static ClassicInfoResolvedFont classic_info_resolve_font(GmlRender *r,int half_points,
                                                          int bold,int italic){
  ClassicInfoResolvedFont resolved={-1,1,0,0};
  int source_index=-1;
  resolved.font_id=classic_info_font(r,half_points,bold,italic,&resolved.scale,&source_index);
  if(resolved.font_id>=0){
    resolved.line_height=r->fonts[resolved.font_id].line_height;
    resolved.y_offset=gml_classic_info_font_sources[source_index].y_offset;
  }
  return resolved;
}

static ClassicInfoRun *classic_info_run_at(ClassicInfoLine *line,int position){
  for(int i=0;i<line->run_count;i++){
    ClassicInfoRun *run=&line->runs[i];
    if(position>=run->start&&position<run->start+run->length) return run;
  }
  return NULL;
}

static double classic_info_advance(GmlRender *r,ClassicInfoLine *line,int position){
  ClassicInfoRun *run=classic_info_run_at(line,position);
  if(!run) return 0;
  ClassicInfoResolvedFont resolved=classic_info_resolve_font(r,run->font_size,run->bold,run->italic);
  if(resolved.font_id<0) return 0;
  GmlGlyph *glyph=real_glyph(&r->fonts[resolved.font_id],(unsigned char)line->text[position]);
  return glyph?glyph->shift*resolved.scale:0;
}

static double classic_info_range_width(GmlRender *r,ClassicInfoLine *line,int first,int last){
  double result=0;
  for(int position=first;position<last;position++)
    result+=classic_info_advance(r,line,position);
  return result;
}

static double classic_info_row_step(int half_points,int bold,int line_height){
  double step=ceil(half_points*(2.0/3.0)*1.06);
  if(line_height>step) step=line_height;
  return step<1?1:step;
}

static double classic_info_draw_row(GmlRender *r,ClassicInfoLine *line,int first,int last,
                                    double y,int page_width,uint32_t text_background){
  int maximum_height=0,maximum_half_points=line->font_size,step_bold=line->bold;
  for(int i=0;i<line->run_count;i++){
    ClassicInfoRun *run=&line->runs[i];
    int run_first=run->start,run_last=run->start+run->length;
    if(run_last<=first||run_first>=last) continue;
    ClassicInfoResolvedFont resolved=classic_info_resolve_font(r,run->font_size,run->bold,run->italic);
    if(resolved.line_height>maximum_height) maximum_height=resolved.line_height;
    if(run->font_size>maximum_half_points){ maximum_half_points=run->font_size; step_bold=run->bold; }
  }
  if(maximum_height<=0){
    ClassicInfoResolvedFont resolved=classic_info_resolve_font(r,line->font_size,line->bold,line->italic);
    maximum_height=resolved.line_height;
  }
  double row_width=classic_info_range_width(r,line,first,last);
  double pen=line->align==1?page_width*0.5-0.5-row_width*0.5:
             (line->align==2?page_width-3-row_width:3);
  int background_x0=(int)floor(pen+0.5);
  int background_x1=(int)ceil(pen+row_width)+1;
  int background_y0=(int)floor(y)-1;
  int background_y1=background_y0+maximum_height;
  if(background_x0<0) background_x0=0;
  if(background_y0<0) background_y0=0;
  if(background_x1>r->fbw) background_x1=r->fbw;
  if(background_y1>r->fbh) background_y1=r->fbh;
  for(int yy=background_y0;yy<background_y1;yy++){
    uint32_t *destination=r->fb+(size_t)yy*r->fbw+background_x0;
    for(int xx=background_x0;xx<background_x1;xx++)
      *destination++=0xFF000000u|text_background;
  }
  int position=first;
  while(position<last){
    ClassicInfoRun *run=classic_info_run_at(line,position);
    if(!run){ position++; continue; }
    int run_last=run->start+run->length; if(run_last>last) run_last=last;
    ClassicInfoResolvedFont resolved=classic_info_resolve_font(r,run->font_size,run->bold,run->italic);
    if(resolved.font_id>=0){
      int length=run_last-position;
      char text[CLASSIC_INFO_LINE_BYTES];
      if(length>=(int)sizeof(text)) length=(int)sizeof(text)-1;
      memcpy(text,line->text+position,(size_t)length); text[length]='\0';
      r->font=resolved.font_id; r->halign=0;
      double run_y=y+(maximum_height-resolved.line_height)-(run->bold?1:0)+resolved.y_offset;
      gml_draw_text_transformed(r,pen,run_y,text,resolved.scale,resolved.scale,0,run->color,1);
      if(run->underline){
        double width=classic_info_range_width(r,line,position,run_last);
        int x0=(int)floor(pen+0.5),x1=(int)floor(pen+width+0.5);
        int underline_y=(int)floor(run_y+resolved.line_height*resolved.scale-2.0+0.5);
        if(x0<0) x0=0;
        if(x1>r->fbw) x1=r->fbw;
        if(underline_y>=0&&underline_y<r->fbh&&x1>x0){
          uint32_t rgb=0xFF000000u|((run->color&255u)<<16)|(run->color&0xFF00u)|
                       ((run->color>>16)&255u);
          uint32_t *row=r->fb+(size_t)underline_y*r->fbw;
          for(int x=x0;x<x1;x++) row[x]=rgb;
        }
      }
      r->font=-1;
    }
    pen+=classic_info_range_width(r,line,position,run_last);
    position=run_last;
  }
  return classic_info_row_step(maximum_half_points,step_bold,maximum_height);
}

static int classic_info_host_render(GmlRender *r,uint32_t *pixels,int width,int height,
                                    const uint8_t *record,size_t record_size){
  const AnygmHostServices *host=r&&r->win?r->win->host:NULL;
  if(!host||!host->rich_text_render||!pixels||width<=0||height<=0||
     !record||record_size<12u) return 0;
  uint32_t caption_size=u32(record,8);
  size_t text_size_at=12u+(size_t)caption_size+8u*4u+8u;
  if(text_size_at>record_size||record_size-text_size_at<4u) return 0;
  uint32_t text_size=u32(record,(uint32_t)text_size_at);
  if((size_t)text_size>record_size-text_size_at-4u) return 0;
  const uint8_t *text=record+text_size_at+4u;
  uint32_t raw_background=u32(record,0);
  uint32_t background=((raw_background&255u)<<16)|(raw_background&0xFF00u)|
                      ((raw_background>>16)&255u);
  return host->rich_text_render(host->userdata,text,text_size,background,pixels,
                                (uint32_t)width,(uint32_t)height,
                                (size_t)width*sizeof(*pixels))==ANYGM_OK;
}

static int classic_info_native_cached(GmlRender *r,uint32_t *framebuffer,
                                      int width,int height,const uint8_t *record,
                                      size_t record_size){
  if(r->classic_info_native_record!=record||
     r->classic_info_native_record_size!=record_size||
     r->classic_info_native_w!=width||r->classic_info_native_h!=height){
    free(r->classic_info_native_pixels);
    r->classic_info_native_pixels=NULL;
    r->classic_info_native_record=record;
    r->classic_info_native_record_size=record_size;
    r->classic_info_native_w=width; r->classic_info_native_h=height;
    r->classic_info_native_attempted=0;
  }
  if(!r->classic_info_native_attempted){
    r->classic_info_native_attempted=1;
    size_t count=(size_t)width*(size_t)height;
    if(width>0&&height>0&&count<=SIZE_MAX/sizeof(uint32_t)){
      r->classic_info_native_pixels=(uint32_t*)malloc(count*sizeof(uint32_t));
      if(r->classic_info_native_pixels&&
         !classic_info_host_render(r,r->classic_info_native_pixels,width,height,
                                   record,record_size)){
        free(r->classic_info_native_pixels);
        r->classic_info_native_pixels=NULL;
      }
    }
  }
  if(!r->classic_info_native_pixels) return 0;
  memcpy(framebuffer,r->classic_info_native_pixels,
         (size_t)width*(size_t)height*sizeof(uint32_t));
  return 1;
}

void gml_draw_classic_game_information(GmlRender *r,uint32_t *framebuffer,
                                       int width,int height,
                                       const uint8_t *record,size_t record_size){
  if(!r||!framebuffer||width<=0||height<=0||record_size>8u*1024u*1024u) return;
  if(classic_info_native_cached(r,framebuffer,width,height,record,record_size)){
    gml_render_begin(r,framebuffer,width,height,0,0);
    r->fb_opaque_known=1; r->fb_all_opaque=1; r->fb_all_transparent=0;
    return;
  }
  ClassicInfoLine lines[CLASSIC_INFO_MAX_LINES]; int line_count=0;
  if(!classic_info_parse(record,record_size,lines,&line_count)) return;
  uint32_t saved_color=r->color;
  double saved_alpha=r->alpha;
  int saved_halign=r->halign, saved_valign=r->valign, saved_font=r->font;
  int saved_alphablend=r->alphablend;
  int saved_software_overlay=r->software_overlay;
  uint32_t information_color=u32(record,0);
  uint32_t background=0xFFFFFFu;
  if((information_color&0xFF000000u)!=0xFF000000u)
    background=((information_color&255u)<<16)|(information_color&0xFF00u)|
               ((information_color>>16)&255u);
  size_t pixels=(size_t)width*(size_t)height;
  for(size_t i=0;i<pixels;i++) framebuffer[i]=background;
  uint32_t outer_border=0xABADB3u;
  for(int x=0;x<width;x++){
    framebuffer[x]=outer_border;
    framebuffer[(size_t)(height-1)*width+x]=outer_border;
  }
  for(int y=0;y<height;y++){
    framebuffer[(size_t)y*width]=outer_border;
    framebuffer[(size_t)y*width+width-1]=outer_border;
  }
  if(width>2&&height>2){
    for(int x=1;x<width-1;x++){
      framebuffer[(size_t)width+x]=0xFFFFFFu;
      framebuffer[(size_t)(height-2)*width+x]=0xFFFFFFu;
    }
    for(int y=1;y<height-1;y++){
      framebuffer[(size_t)y*width+1]=0xFFFFFFu;
      framebuffer[(size_t)y*width+width-2]=0xFFFFFFu;
    }
  }
  uint32_t text_background=background;
  unsigned background_green=(text_background>>8)&255u;
  if(background_green>0&&background_green<128)
    text_background=(text_background&~0xFF00u)|((background_green+1u)<<8);
  gml_render_begin(r,framebuffer,width,height,0,0);
  r->font=-1; r->color=0; r->alpha=1; r->valign=0; r->alphablend=1;
  r->software_overlay=1;
  r->fb_opaque_known=1; r->fb_all_opaque=1; r->fb_all_transparent=0;
  double y=4;
  for(int i=0;i<line_count && y<height;i++){
    ClassicInfoLine *line=&lines[i];
    if(!line->length){
      ClassicInfoResolvedFont resolved=classic_info_resolve_font(r,line->font_size,line->bold,line->italic);
      double blank_step=classic_info_row_step(line->font_size,line->bold,resolved.line_height);
      /* A 9-point empty RichEdit paragraph retains its half-pixel leading;
       * keeping the fractional phase makes later lines snap like GDI. */
      if(line->font_size==18&&!line->bold&&!line->italic) blank_step+=0.5;
      y+=blank_step;
      continue;
    }
    int first=0;
    while(first<line->length&&y<height){
      int position=first,last_space=-1,last=line->length,next=line->length;
      double row_width=0,space_width=0,maximum_width=width>6?width-6:width;
      for(;position<line->length;position++){
        double advance=classic_info_advance(r,line,position);
        if(line->text[position]==' '){ last_space=position; space_width=row_width; }
        if(row_width+advance>maximum_width&&position>first){
          if(last_space>=first){
            last=last_space+1;
            row_width=space_width+classic_info_advance(r,line,last_space);
            next=last_space+1;
          }
          else { last=position; next=position; }
          while(next<line->length&&line->text[next]==' ') next++;
          break;
        }
        row_width+=advance;
      }
      (void)row_width;
      y+=classic_info_draw_row(r,line,first,last,y,width,text_background);
      first=next;
    }
  }
  r->color=saved_color; r->alpha=saved_alpha;
  r->halign=saved_halign; r->valign=saved_valign; r->font=saved_font;
  r->alphablend=saved_alphablend;
  r->software_overlay=saved_software_overlay;
}

void gml_draw_text_transformed(GmlRender *r, double x, double y, const char *str,
                               double xs, double ys, double rot, uint32_t blend, double alpha){
  gml_render_gui_map_point(r,&x,&y);
  gml_render_gui_map_scale(r,&xs,&ys);
  GmlFont *f=active_font(r);
  if(!f||!str) return;
  if(alpha>1) alpha=1; else if(alpha<0) alpha=0;
  if(xs==0||ys==0||alpha<=0) return;
  double rr=fmod(rot,360.0); if(rr<0) rr+=360.0;
  double ca,sa; render_rotation_sincos(rr,&ca,&sa);
  int use_rot = fabs(rr)>0.001 && fabs(rr-360.0)>0.001;
  if(render_setting(r,"GML_LOG_TEXT")) anygm_host_logf(r && r->win ? r->win->host : NULL,ANYGM_LOG_DEBUG,"[text] x=%.0f y=%.0f font=%d halign=%d valign=%d scale=(%.2f,%.2f) rot=%.1f col=%06X a=%.2f \"%s\"\n",
    x,y,r->font,r->halign,r->valign,xs,ys,rr,(unsigned)(blend&0xffffff),alpha,str);
  if(f->real){ draw_text_real(r,f,x,y,str,xs,ys,ca,sa,use_rot,blend,alpha); return; }
  if(f->sprite<0 || f->sprite>=r->n_spr) return;
  GmlSprite *s=&r->spr[f->sprite];
  if(s->n_frames<=0) return;
  /* line height = sprite cell height (GM uses the sprite's declared h, not the
   * glyph's trimmed sub-rect sh — otherwise text lines collapse when glyphs are cropped). */
  int lh=s->h; if(lh<=0) lh=8;
  /* count lines for valign */
  int nlines=1; for(const char *p=str;*p;p++){ if(*p=='\\'&&p[1]=='#'){p++;continue;} if(text_is_linebreak(p)) nlines++; }
  double base_y=0;
  if(r->valign==1) base_y=-(nlines*lh)/2.0; else if(r->valign==2) base_y=-nlines*lh;
  const char *p=str;
  for(int li=0; *p || li==0; li++){
    const char *end; int lw=line_width(r,f,p,&end);
    double base_x=0;
    if(r->halign==1) base_x=-lw/2.0; else if(r->halign==2) base_x=-lw;
    double cx=base_x;
    while(p<end){
      unsigned cp=text_next_cp(&p);
      int fr=glyph_frame(f,cp);
      if(fr>=0 && fr<s->n_frames){
        double glyph_ox=(cx+s->originx)*xs,glyph_oy=(base_y+s->originy)*ys;
        double glyph_x=use_rot?x+glyph_ox*ca+glyph_oy*sa:x+glyph_ox;
        double glyph_y=use_rot?y-glyph_ox*sa+glyph_oy*ca:y+glyph_oy;
        if(gml_d3_draw_sprite_2d(r,f->sprite,fr,glyph_x,glyph_y,xs,ys,rr,blend,alpha)){
          /* The D3 hook projects the glyph quad at the current draw depth. */
        } else if(s->runtime_rgba){
          const uint8_t *fr_rgba=runtime_frame_rgba(s,fr);
          if(fr_rgba){
            if(use_rot){
              double gx=x + cx*xs*ca + base_y*ys*sa;
              double gy=y - cx*xs*sa + base_y*ys*ca;
              const int *rmin=s->runtime_row_min?s->runtime_row_min+(size_t)fr*s->h:NULL;
              const int *rmax=s->runtime_row_max?s->runtime_row_max+(size_t)fr*s->h:NULL;
              blit_rgba_sprite(r,s,fr_rgba,s->w,s->h,gx,gy,xs,ys,rr,0,0,blend,alpha,1,rmin,rmax,s->runtime_opaque);
            } else {
              const int *rmin=s->runtime_row_min?s->runtime_row_min+(size_t)fr*s->h:NULL;
              const int *rmax=s->runtime_row_max?s->runtime_row_max+(size_t)fr*s->h:NULL;
              blit_rgba_sprite(r,s,fr_rgba,s->w,s->h,x+cx*xs,y+base_y*ys,xs,ys,0,0,0,blend,alpha,1,rmin,rmax,s->runtime_opaque);
            }
          }
        } else if(s->frame){ int ti=s->frame[fr];
          if(ti>=0 && ti<r->n_tpag){ GmlTpag *t=&r->tpag[ti];
          GmlTpag gt=*t;
          if(f->prop) gt.tx=0;   /* proportional sprite-font glyphs advance by the trimmed rect */
          if(use_rot){
            double ox=(cx+s->originx)*xs;
            double oy=(base_y+s->originy)*ys;
            double gx=x + ox*ca + oy*sa;
            double gy=y - ox*sa + oy*ca;
            blit_rotated(r,s,&gt,gx,gy,xs,ys,rr,blend,alpha);
          } else {
            blit(r,&gt, x + cx*xs - r->cam_x + gt.tx*xs,
              y + base_y*ys - r->cam_y + gt.ty*ys, xs,ys, blend, alpha);
          } } }
        }
      cx += glyph_w(r,f,fr,cp)+f->sep;
    }
    base_y += lh;
    if(text_is_linebreak(end)) p=end+1; else break;
  }
}
void gml_draw_text(GmlRender *r, double x, double y, const char *str){
  if(!r) return;
  gml_draw_text_transformed(r,x,y,str,1,1,0,r->color,r->alpha);
}
static const char *text_wrap_ext(GmlRender *r,const char *str,double w,char *wrapped,size_t cap){
  if(!r || !str || !wrapped || cap<2 || w<=0) return str;
  size_t o=0;
  if(w>0){
    char word[256]; size_t wl=0;
    char line[1024]; size_t ll=0; line[0]=0;
    const char *p=str;
    for(;;){
      char c=*p;
      int end_word = (c==' '||c=='#'||c=='\n'||c==0||(c=='\\'&&p[1]=='#'));
      if(!end_word){ if(wl<sizeof(word)-1) word[wl++]=c; p++; continue; }
      word[wl]=0;
      if(wl){
        char probe[1300];
        if(ll) snprintf(probe,sizeof probe,"%s %s",line,word);
        else snprintf(probe,sizeof probe,"%s",word);
        if(ll && gml_text_width(r,probe)>(int)w){
          /* flush current line */
          for(size_t k=0;k<ll && o<cap-2;k++) wrapped[o++]=line[k];
          if(o<cap-1) wrapped[o++]='#';
          snprintf(line,sizeof line,"%s",word); ll=strlen(line);
        } else { snprintf(line,sizeof line,"%s",probe); ll=strlen(line); }
        wl=0;
      }
      if(c=='#' || c=='\n'){ for(size_t k=0;k<ll && o<cap-2;k++) wrapped[o++]=line[k];
        if(o<cap-1) wrapped[o++]='#'; ll=0; line[0]=0; }
      if(c==0) break;
      p += (c=='\\')?2:1;
    }
    for(size_t k=0;k<ll && o<cap-1;k++) wrapped[o++]=line[k];
    wrapped[o]=0;
  }
  return wrapped;
}

double gml_text_width_ext(GmlRender *r,const char *str,double sep,double w){
  (void)sep;
  if(!r || !str) return 0;
  char wrapped[2048];
  const char *layout=text_wrap_ext(r,str,w,wrapped,sizeof wrapped);
  return gml_text_width(r,layout);
}

double gml_text_height_ext(GmlRender *r,const char *str,double sep,double w){
  if(!r || !str) return 0;
  char wrapped[2048];
  const char *layout=text_wrap_ext(r,str,w,wrapped,sizeof wrapped);
  if(sep<0) return gml_text_height(r,layout);
  int lines=1;
  for(const char *p=layout;*p;p++){
    if(*p=='\\' && p[1]=='#'){ p++; continue; }
    if(text_is_linebreak(p)) lines++;
  }
  double line_height=gml_text_height(r,"");
  return line_height+(lines-1)*sep;
}

void gml_draw_text_ext_transformed(GmlRender *r, double x, double y, const char *str,
                                   double sep, double w, double xs, double ys, double rot,
                                   uint32_t blend, double alpha){
  if(!r || !str || !active_font(r)) return;
  char wrapped[2048];
  str=text_wrap_ext(r,str,w,wrapped,sizeof wrapped);
  if(sep<0){ gml_draw_text_transformed(r,x,y,str,xs,ys,rot,blend,alpha); return; }
  /* custom line separation: draw line by line at y + i*sep */
  int nlines=1; for(const char *q=str;*q;q++){ if(*q=='\\'&&q[1]=='#'){q++;continue;} if(text_is_linebreak(q)) nlines++; }
  /* Separation is the distance between successive line origins, not the full block height.
   * The first line still occupies one font line-height; omitting it shifts even a single-line
   * centred or bottom-aligned string away from the requested anchor. */
  double block_height=gml_text_height(r,"")+(nlines-1)*sep;
  double base=0;
  if(r->valign==1) base=-block_height/2.0; else if(r->valign==2) base=-block_height;
  double rr=fmod(rot,360.0); if(rr<0) rr+=360.0;
  double ca,sa; render_rotation_sincos(rr,&ca,&sa);
  int sv=r->valign; r->valign=0;
  const char *p=str; int li=0;
  char lbuf[1024];
  while(1){
    size_t k=0;
    while(*p && !((*p=='#' && (p==str || p[-1]!='\\')) || *p=='\n') && k<sizeof(lbuf)-1) lbuf[k++]=*p++;
    lbuf[k]=0;
    double line_y=base+li*sep;
    gml_draw_text_transformed(r,x+line_y*ys*sa,y+line_y*ys*ca,lbuf,xs,ys,rr,blend,alpha);
    if(!text_is_linebreak(p)) break;
    p++; li++;
  }
  r->valign=sv;
}

/* draw_text_ext: word-wrap at pixel width `w` (-1 = none) with line separation `sep`
 * (-1 = font default). GM wraps between words. */
void gml_draw_text_ext(GmlRender *r, double x, double y, const char *str, double sep, double w){
  if(!r) return;
  gml_draw_text_ext_transformed(r,x,y,str,sep,w,1,1,0,r->color,r->alpha);
}
