/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me> */
/* gml_render.c — atlas/TPAG/sprite decode + software blitter. */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"
#include "gml_render.h"
#include "gm_qoi.h"
#include "bzip2/bzlib.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <time.h>
#include <limits.h>
#if defined(__SSE2__)
#include <emmintrin.h>
#endif
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#endif

typedef struct {
  const char *label;
  int tpag, atlas, sx, sy, sw, sh;
  long calls;
  unsigned long long pixels;
  double ms;
} RenderProfSlot;
typedef struct {
  int sprite;
  const char *name;
  long calls;
  double ms;
} SpriteProfSlot;

#define RPROF_MAX 1024
static RenderProfSlot g_rprof[RPROF_MAX];
static int g_rprof_n;
static long g_rprof_last_frame=-1;
#define SPROF_MAX 512
static SpriteProfSlot g_sprof[SPROF_MAX];
static int g_sprof_n;
static long g_sprof_last_frame=-1;

static int rprof_enabled(void){
  static int on=-1;
  if(on<0) on=getenv("GML_PROFILE_RENDER") ? 1 : 0;
  return on;
}
static int log_spr_enabled(void){
  static int on=-1;
  if(on<0) on=getenv("GML_LOG_SPR") ? 1 : 0;
  return on;
}
static int log_axis_cache_enabled(void){
  static int on=-1;
  if(on<0) on=getenv("GML_LOG_AXIS_CACHE") ? 1 : 0;
  return on;
}
static int sprof_enabled(void){
  static int on=-1;
  if(on<0) on=getenv("GML_PROFILE_SPRITE") ? 1 : 0;
  return on;
}
static double rprof_now(void){
#ifdef _WIN32
  return 0.0;
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC,&ts);
  return ts.tv_sec + ts.tv_nsec/1000000000.0;
#endif
}
static int rprof_tpag_id(GmlRender *r, GmlTpag *t){
  if(!r || !t || !r->tpag || r->n_tpag<=0) return -1;
  uintptr_t p=(uintptr_t)t, b=(uintptr_t)r->tpag;
  uintptr_t e=b+(uintptr_t)r->n_tpag*sizeof(GmlTpag);
  return (p>=b && p<e) ? (int)((p-b)/sizeof(GmlTpag)) : -1;
}
static void rprof_dump_maybe(void){
  if(!rprof_enabled()) return;
  extern long g_vm_frame;
  long frame=g_vm_frame;
  if(frame<=0 || frame==g_rprof_last_frame || frame%300) return;
  g_rprof_last_frame=frame;
  const char *labs[16]={0};
  double lab_ms[16]={0};
  long lab_calls[16]={0};
  unsigned long long lab_px[16]={0};
  int lab_n=0;
  for(int i=0;i<g_rprof_n;i++){
    int li=-1;
    for(int j=0;j<lab_n;j++) if(labs[j]==g_rprof[i].label){ li=j; break; }
    if(li<0 && lab_n<16){ li=lab_n++; labs[li]=g_rprof[i].label; }
    if(li>=0){ lab_ms[li]+=g_rprof[i].ms; lab_calls[li]+=g_rprof[i].calls; lab_px[li]+=g_rprof[i].pixels; }
  }
  fprintf(stderr,"[rprof] f=%ld totals",frame);
  for(int i=0;i<lab_n;i++) fprintf(stderr," %s=%.2fms/%ld/%llupx",labs[i],lab_ms[i],lab_calls[i],lab_px[i]);
  fprintf(stderr,"\n");
  int used[12]; for(int i=0;i<12;i++) used[i]=-1;
  for(int rank=0;rank<12;rank++){
    int best=-1;
    for(int i=0;i<g_rprof_n;i++){
      int seen=0; for(int j=0;j<rank;j++) if(used[j]==i){ seen=1; break; }
      if(!seen && (best<0 || g_rprof[i].ms>g_rprof[best].ms)) best=i;
    }
    if(best<0 || g_rprof[best].ms<=0) break;
    used[rank]=best;
    RenderProfSlot *s=&g_rprof[best];
    fprintf(stderr,"[rprof]   %7.2fms %6ld calls %10llupx %-8s tpag=%d atlas=%d src=%d,%d %dx%d\n",
      s->ms,s->calls,s->pixels,s->label,s->tpag,s->atlas,s->sx,s->sy,s->sw,s->sh);
  }
  memset(g_rprof,0,sizeof g_rprof);
  g_rprof_n=0;
}
static void rprof_add(const char *label, GmlRender *r, GmlTpag *t, double ms, unsigned long long pixels){
  if(!rprof_enabled()) return;
  int tpag=rprof_tpag_id(r,t);
  int atlas=t?t->atlas:-1, sx=t?t->sx:0, sy=t?t->sy:0, sw=t?t->sw:0, sh=t?t->sh:0;
  int slot=-1;
  for(int i=0;i<g_rprof_n;i++){
    RenderProfSlot *s=&g_rprof[i];
    if(s->label==label && s->tpag==tpag && s->atlas==atlas && s->sx==sx && s->sy==sy && s->sw==sw && s->sh==sh){
      slot=i; break;
    }
  }
  if(slot<0){
    if(g_rprof_n<RPROF_MAX) slot=g_rprof_n++;
    else slot=RPROF_MAX-1;
    g_rprof[slot]=(RenderProfSlot){label,tpag,atlas,sx,sy,sw,sh,0,0,0};
  }
  g_rprof[slot].calls++;
  g_rprof[slot].pixels+=pixels;
  g_rprof[slot].ms+=ms;
  rprof_dump_maybe();
}
static void sprof_add(int sprite, const char *name, double ms){
  if(!sprof_enabled()) return;
  int slot=-1;
  for(int i=0;i<g_sprof_n;i++) if(g_sprof[i].sprite==sprite){ slot=i; break; }
  if(slot<0){
    if(g_sprof_n<SPROF_MAX) slot=g_sprof_n++;
    else slot=SPROF_MAX-1;
    g_sprof[slot]=(SpriteProfSlot){sprite,name,0,0};
  }
  g_sprof[slot].calls++;
  g_sprof[slot].ms+=ms;
  extern long g_vm_frame;
  long frame=g_vm_frame;
  if(frame<=0 || frame==g_sprof_last_frame || frame%300) return;
  g_sprof_last_frame=frame;
  fprintf(stderr,"[sprof] f=%ld top:\n",frame);
  int used[16]; for(int i=0;i<16;i++) used[i]=-1;
  for(int rank=0;rank<16;rank++){
    int best=-1;
    for(int i=0;i<g_sprof_n;i++){
      int seen=0; for(int j=0;j<rank;j++) if(used[j]==i){ seen=1; break; }
      if(!seen && (best<0 || g_sprof[i].ms>g_sprof[best].ms)) best=i;
    }
    if(best<0 || g_sprof[best].ms<=0) break;
    used[rank]=best;
    fprintf(stderr,"[sprof]   %7.2fms %6ld spr=%d %s\n",
            g_sprof[best].ms,g_sprof[best].calls,g_sprof[best].sprite,
            g_sprof[best].name?g_sprof[best].name:"?");
  }
  memset(g_sprof,0,sizeof g_sprof);
  g_sprof_n=0;
}

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
/* Decode PNG, fioq, or a bzip2-compressed 2zoq container into RGBA pixels. */
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
static uint8_t *atlas_pixels(GmlRender *r, int idx){
  if(!r || idx<0 || idx>=r->n_atlas || !r->atlas) return NULL;
  GmlAtlas *a=&r->atlas[idx];
  if(a->px) return a->px;
  if(a->decode_attempted || !a->blob || a->blob>=r->win->size) return NULL;
  a->decode_attempted=1;
  int w=0,h=0;
  uint8_t *px=decode_texture_blob(r->win->data+a->blob, a->avail, a->chunk_end, &w,&h);
  if(!px) return NULL;
  a->px=px; a->w=w; a->h=h;
  if(getenv("GML_LOG_ATLAS"))
    fprintf(stderr,"[atlas] decoded %d %dx%d (%.1f MiB)\n",idx,w,h,(double)((uint64_t)w*(uint64_t)h*4ull)/(1024.0*1024.0));
  if(getenv("GML_DUMP_ATLAS")){ char fn[64]; snprintf(fn,sizeof fn,"builds/_atlas%d.ppm",idx);
    FILE*f=fopen(fn,"wb"); if(f){ fprintf(f,"P6\n%d %d\n255\n",w,h);
      for(int q=0;q<w*h;q++) fwrite(px+q*4,1,3,f); fclose(f);
      fprintf(stderr,"[atlas] dumped %s (%dx%d)\n",fn,w,h); } }
  return a->px;
}
static int tpag_alpha_bounds(GmlRender *r, GmlTpag *t, GmlAtlas *a,
                             int *x0, int *y0, int *x1, int *y1){
  (void)r;
  if(!t || !a || !a->px || t->sw<=0 || t->sh<=0) return 0;
  if(!t->alpha_scanned){
    int minx=t->sw, miny=t->sh, maxx=-1, maxy=-1;
    int maxa=0;
    int log_alpha=getenv("GML_LOG_TPAG_ALPHA")!=NULL;
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
      fprintf(stderr,"[tpag-alpha] id=%d atlas=%d src=%d,%d %dx%d nz=%lu/%lu amax=%d bbox=%d,%d-%d,%d\n",
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
static inline void blend_argb_src_over_exact(uint32_t *dp, const uint32_t *sp, int run, uint32_t aa){
  if(run<=0 || !aa) return;
  if(aa>=255u){ memcpy(dp,sp,(size_t)run*sizeof(uint32_t)); return; }
  uint32_t ia=255u-aa;
  for(int k=0; k<run; k++){
    uint32_t src=sp[k], dst=dp[k];
    uint32_t sr=(src>>16)&0xFFu, sg=(src>>8)&0xFFu, sb=src&0xFFu;
    uint32_t dr=(dst>>16)&0xFFu, dg=(dst>>8)&0xFFu, db=dst&0xFFu;
    dp[k]=0xFF000000u|(((sr*aa+dr*ia)/255u)<<16)|(((sg*aa+dg*ia)/255u)<<8)|((sb*aa+db*ia)/255u);
  }
}
static inline void copy_argb_force_opaque(uint32_t *dp, const uint32_t *sp, int run){
  if(run<=0) return;
  for(int k=0; k<run; k++) dp[k]=0xFF000000u|(sp[k]&0x00FFFFFFu);
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
      else blend_argb_src_over_exact(dp,sp,n,(uint32_t)ar->alpha);
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
      blend_argb_src_over_exact(dp+i,sp+i,run,aa);
      i+=run;
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
    /* Scan record fields and select a pointer to recognized PNG, fioq or 2zoq data. */
    for(uint32_t off=4; off<=32; off+=4){ uint32_t v=u32(d,entry+off);
      if((size_t)v+4<=r->win->size){ const uint8_t *m=d+v;
        if((m[0]==0x89&&m[1]=='P'&&m[2]=='N'&&m[3]=='G')||!memcmp(m,"fioq",4)||!memcmp(m,"2zoq",4)){ blob=v; break; } } }
    if(!blob || (size_t)blob>=r->win->size) continue;
    GmlAtlas *a=&r->atlas[i];
    a->blob=blob; a->avail=r->win->size-blob; a->chunk_end=chunk_end;
    texture_blob_dims(d+blob,a->avail,&a->w,&a->h);
    if(getenv("GML_ATLAS_EAGER") || getenv("GML_DUMP_ATLAS")) atlas_pixels(r,(int)i);
  }
}

/* ---- TPAG ---- */
static uint32_t *g_tpag_ptr; /* parallel: file offset of each tpag, for sprite frame mapping */
static void runtime_axis_cache_free(GmlSprite *s);
static void parse_tpag(GmlRender *r){
  const GmlChunk *c=gml_chunk(r->win,"TPAG"); if(!c) return;
  const uint8_t *d=r->win->data; uint32_t n=u32(d,c->off);
  r->n_tpag=(int)n; r->tpag=calloc(n,sizeof(GmlTpag)); g_tpag_ptr=calloc(n,sizeof(uint32_t));
  if(!r->tpag || !g_tpag_ptr){ free(r->tpag); free(g_tpag_ptr); r->tpag=NULL; g_tpag_ptr=NULL; r->n_tpag=0; return; }
  for(uint32_t i=0;i<n;i++){
    uint32_t p=u32(d,c->off+4+i*4); g_tpag_ptr[i]=p;
    GmlTpag *t=&r->tpag[i];
    t->sx=u16(d,p); t->sy=u16(d,p+2); t->sw=u16(d,p+4); t->sh=u16(d,p+6);
    t->tx=u16(d,p+8); t->ty=u16(d,p+10); t->bw=u16(d,p+16); t->bh=u16(d,p+18);
    t->atlas=(int16_t)u16(d,p+20);
  }
}
static int tpag_index_for_ptr(GmlRender *r, uint32_t ptr){
  for(int i=0;i<r->n_tpag;i++) if(g_tpag_ptr[i]==ptr) return i;
  return -1;
}

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
    s->w=(int)u32(d,p+4); s->h=(int)u32(d,p+8);
    s->ml=(int)u32(d,p+12); s->mr=(int)u32(d,p+16); s->mb=(int)u32(d,p+20); s->mt=(int)u32(d,p+24);
    s->collision_kind=0; s->collision_tolerance=63; /* preserve the old alpha>=64 fallback */
    /* margins(16) transparent/smooth/preload(12) bboxmode(4) sepmasks(4) -> originX/Y */
    /* GMS1 sprite header: ...,BBoxMode(40),SepMasks(44),OriginX(48),OriginY(52),frameList(56) */
    s->originx=(int)u32(d,p+48); s->originy=(int)u32(d,p+52);
    uint32_t list=p+56;             /* GMS1: SimpleList<TextureEntry> here (count + pointers) */
    /* A -1 marker at +56 selects the versioned sprite layout. For simple sprites,
     * the texture list follows playback fields and optional sequence/nine-slice offsets. */
    if(u32(d,p+56)==0xFFFFFFFFu){
      uint32_t sver=u32(d,p+60), stype=u32(d,p+64);
      if(stype!=0){ s->n_frames=0; s->frame=calloc(1,sizeof(int)); continue; }  /* SWF/Spine: no simple list */
      uint32_t fl=76;                         /* after PlaybackSpeed(+68)+PlaybackSpeedType(+72) */
      if(sver>=2) fl+=4;                       /* SequenceOffset */
      if(sver>=3) fl+=4;                       /* NineSliceOffset */
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
/* whether sprite's COLLISION MASK is solid at sprite-local (lx,ly). Falls back to the visible
 * alpha if a sprite has no mask. */
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
  if(!s->mask||s->mask_count<=0) return gml_sprite_alpha(r,sprite,frame,lx,ly)>=64;
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
	    if(r->win->bytecode>=17 && p+64<c->off+c->size){
	      int ver=(int)u32(d,p+20), tw=(int)u32(d,p+24), th=(int)u32(d,p+28);
	      int bx=(int)u32(d,p+32), by=(int)u32(d,p+36), cols=(int)u32(d,p+40);
	      int items=(int)u32(d,p+44), count=(int)u32(d,p+48);
	      uint64_t id_bytes=(uint64_t)items*(uint64_t)count*4u;
	      if(ver>0 && tw>0 && th>0 && tw<=4096 && th<=4096 && bx>=0 && by>=0 &&
	         cols>0 && cols<=4096 && items>0 && items<=1024 && count>0 &&
	         id_bytes<=4000000u && p+64+id_bytes<=c->off+c->size){
	        r->bg[i].tile_w=tw; r->bg[i].tile_h=th;
	        r->bg[i].tile_border_x=bx; r->bg[i].tile_border_y=by;
	        r->bg[i].tile_columns=cols; r->bg[i].tile_items_per_tile=items; r->bg[i].tile_count=count;
	        r->bg[i].tile_ids=d+p+64;
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
    /* Interpret EmSize as an integer or floating-point field using the checks below. */
    int em_is_float=0;
    { uint32_t rawem=u32(d,p+8); float fem; memcpy(&fem,&rawem,4);
      if(fem<0){ f->line_height=(int)(0.5f-fem); em_is_float=1; }
      else if(fem>0 && fem<512.0f && rawem>0x30000000u){ f->line_height=(int)(fem+0.5f); em_is_float=1; }
      else f->line_height=(int)rawem; }
    uint32_t texptr=u32(d,p+28);                    /* glyph-page TPAG record */
    int tsx=u16(d,texptr), tsy=u16(d,texptr+2), tatlas=(int16_t)u16(d,texptr+20);
    f->atlas=tatlas;
    /* glyph table position: +40 (bc14-16) or +48 (GMS2.2.6+/2.3 insert AscenderOffset and
     * Ascender after the scales). Detect per record: the count must be sane and be followed
     * by an in-file pointer list. */
    uint32_t goff=40;
    { uint32_t c40=u32(d,p+40), c48=u32(d,p+48);
      uint32_t q40=(c40>0&&c40<100000)?u32(d,p+44):0, q48=(c48>0&&c48<100000)?u32(d,p+52):0;
      int ok40 = q40>p && q40+14<=r->win->size;
      int ok48 = q48>p && q48+14<=r->win->size;
      if(!ok40 && ok48) goff=48;
      else if(ok40 && ok48){
        /* both look plausible (rare): prefer the one whose first glyph has a sane char code */
        if(u16(d,q40)>0x2FFF && u16(d,q48)<=0x2FFF) goff=48;
      } }
    uint32_t gc=u32(d,p+goff);
    if(gc>100000) gc=0;                             /* guard */
    f->glyphs=calloc(gc?gc:1,sizeof(GmlGlyph));
    f->n_glyphs=(int)gc;
    for(uint32_t g=0;g<gc && f->glyphs;g++){
      uint32_t q=u32(d,p+goff+4+g*4);               /* pointer to glyph record */
      GmlGlyph *gl=&f->glyphs[g];
      gl->ch=u16(d,q);
      gl->sx=tsx+(int)u16(d,q+2); gl->sy=tsy+(int)u16(d,q+4);   /* absolute atlas coords */
      gl->w=(int)u16(d,q+6); gl->h=(int)u16(d,q+8);
      gl->shift=(int16_t)u16(d,q+10);               /* advance */
      gl->offset=(int16_t)u16(d,q+12);              /* left bearing */
      if(gl->ch<256) f->glyph_by_char[gl->ch]=(int)g;
    }
    /* For floating-point EmSize, increase line height to the tallest glyph when needed. */
    if(em_is_float){ int mh=0;
      for(int g2=0;g2<f->n_glyphs;g2++) if(f->glyphs[g2].h>mh) mh=f->glyphs[g2].h;
      if(mh>f->line_height) f->line_height=mh; }
    if(getenv("GML_LOG_FONT"))
      fprintf(stderr,"[font] real id=%d name=%s em=%d atlas=%d glyphs=%d\n",
        i, gml_str_by_ptr(r->win,u32(d,p)), f->line_height, f->atlas, f->n_glyphs);
  }
  if(r->n_fonts<nf) r->n_fonts=nf;                  /* sprite fonts number after real fonts */
}

/* ---- recognized display post-processes: none retained in this revision ---- */

/* ---- real FONT-chunk font helpers ---- */
static GmlGlyph *real_glyph(GmlFont *f, unsigned cp){
  if(cp<256){ int gi=f->glyph_by_char[cp]; return gi>=0? &f->glyphs[gi] : NULL; }
  for(int i=0;i<f->n_glyphs;i++) if(f->glyphs[i].ch==cp) return &f->glyphs[i];
  return NULL;
}
/* advance width of one line (up to '#'/NUL), '\#' counts as a literal '#'. */
static int real_line_width(GmlFont *f, const char *p, const char **end){
  int w=0;
  while(*p && *p!='#'){
    unsigned cp=text_next_cp(&p);
    GmlGlyph *g=real_glyph(f,cp);
    if(g) w+=g->shift;
  }
  *end=p; return w;
}
/* Draw glyph atlas rectangles top-aligned and advance the pen by each shift.
 * Rotation changes pen positions; glyph rectangles remain axis-aligned. */
static void draw_text_real(GmlRender *r, GmlFont *f, double x, double y, const char *str,
                           double xs, double ys, double ca, double sa, int use_rot,
                           uint32_t blend, double alpha){
  int lh=f->line_height>0? f->line_height:12;
  int nlines=1; for(const char *q=str;*q;q++){ if(*q=='\\'&&q[1]=='#'){q++;continue;} if(*q=='#') nlines++; }
  double base_y=0;
  if(r->valign==1) base_y=-(nlines*lh)/2.0; else if(r->valign==2) base_y=-nlines*lh;
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
        double dx=(cx+g->offset)*xs, dy=base_y*ys;
        if(use_rot) blit(r,&gt, x + dx*ca + dy*sa - r->cam_x, y - dx*sa + dy*ca - r->cam_y, xs,ys, blend, alpha);
        else        blit(r,&gt, x + dx - r->cam_x, y + dy - r->cam_y, xs,ys, blend, alpha);
      }
      if(g) cx += g->shift;
    }
    base_y += lh;
    if(*end=='#') p=end+1; else break;
  }
}

int gml_text_width(GmlRender *r, const char *str){
  if(r->font<0||r->font>=r->n_fonts||!str) return 0;
  GmlFont *f=&r->fonts[r->font];
  int best=0; const char *p=str;
  for(;;){
    const char *end; int w = f->real ? real_line_width(f,p,&end) : line_width(r,f,p,&end);
    if(w>best) best=w;
    if(*end!='#') break;
    p=end+1;
  }
  return best;
}
int gml_text_height(GmlRender *r, const char *str){
  if(r->font<0||r->font>=r->n_fonts) return 0;
  GmlFont *f=&r->fonts[r->font];
  int lh, nlines=1;
  if(f->real) lh=f->line_height>0?f->line_height:12;
  else { GmlSprite *s=&r->spr[f->sprite]; lh=s->h>0?s->h:8; }
  if(str) for(const char *p=str;*p;p++){ if(*p=='\\'&&p[1]=='#'){p++;continue;} if(*p=='#') nlines++; }
  return lh*nlines;
}
void gml_draw_text_transformed(GmlRender *r, double x, double y, const char *str,
                               double xs, double ys, double rot, uint32_t blend, double alpha){
  if(r->font<0 && str && str[0] && getenv("GML_LOG_FONT")) fprintf(stderr,"[font] draw_text font=%d str=\"%.30s\" — INVISIBLE (no default font)\n",r->font,str);
  if(r->font<0||r->font>=r->n_fonts||!str) return;
  if(alpha>1) alpha=1; else if(alpha<0) alpha=0;
  if(xs==0||ys==0||alpha<=0) return;
  double rr=fmod(rot,360.0); if(rr<0) rr+=360.0;
  double ang=rr*M_PI/180.0, ca=cos(ang), sa=sin(ang);
  int use_rot = fabs(rr)>0.001 && fabs(rr-360.0)>0.001;
  if(getenv("GML_LOG_TEXT")) fprintf(stderr,"[text] x=%.0f y=%.0f font=%d halign=%d valign=%d scale=(%.2f,%.2f) rot=%.1f col=%06X a=%.2f \"%s\"\n",
    x,y,r->font,r->halign,r->valign,xs,ys,rr,(unsigned)(blend&0xffffff),alpha,str);
  GmlFont *f=&r->fonts[r->font];
  if(f->real){ draw_text_real(r,f,x,y,str,xs,ys,ca,sa,use_rot,blend,alpha); return; }
  if(f->sprite<0 || f->sprite>=r->n_spr) return;
  GmlSprite *s=&r->spr[f->sprite];
  if(s->n_frames<=0) return;
  /* line height = sprite cell height (GM uses the sprite's declared h, not the
   * glyph's trimmed sub-rect sh — otherwise text lines collapse when glyphs are cropped). */
  int lh=s->h; if(lh<=0) lh=8;
  /* count lines for valign */
  int nlines=1; for(const char *p=str;*p;p++){ if(*p=='\\'&&p[1]=='#'){p++;continue;} if(*p=='#') nlines++; }
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
        if(s->runtime_rgba){
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
      cx += glyph_w(r,f,fr)+f->sep;
    }
    base_y += lh;
    if(*end=='#') p=end+1; else break;
  }
}
void gml_draw_text(GmlRender *r, double x, double y, const char *str){
  if(!r) return;
  gml_draw_text_transformed(r,x,y,str,1,1,0,r->color,r->alpha);
}
/* Wrap between words at pixel width w (-1 disables wrapping).
 * sep selects line separation; -1 selects the font default. */
void gml_draw_text_ext(GmlRender *r, double x, double y, const char *str, double sep, double w){
  if(!r || !str || r->font<0 || r->font>=r->n_fonts){ return; }
  char wrapped[2048]; size_t o=0;
  if(w>0){
    char word[256]; size_t wl=0;
    char line[1024]; size_t ll=0; line[0]=0;
    const char *p=str;
    for(;;){
      char c=*p;
      int end_word = (c==' '||c=='#'||c==0||(c=='\\'&&p[1]=='#'));
      if(!end_word){ if(wl<sizeof(word)-1) word[wl++]=c; p++; continue; }
      word[wl]=0;
      if(wl){
        char probe[1300];
        if(ll) snprintf(probe,sizeof probe,"%s %s",line,word);
        else snprintf(probe,sizeof probe,"%s",word);
        if(ll && gml_text_width(r,probe)>(int)w){
          /* flush current line */
          for(size_t k=0;k<ll && o<sizeof(wrapped)-2;k++) wrapped[o++]=line[k];
          wrapped[o++]='#';
          snprintf(line,sizeof line,"%s",word); ll=strlen(line);
        } else { snprintf(line,sizeof line,"%s",probe); ll=strlen(line); }
        wl=0;
      }
      if(c=='#'){ for(size_t k=0;k<ll && o<sizeof(wrapped)-2;k++) wrapped[o++]=line[k];
        wrapped[o++]='#'; ll=0; line[0]=0; }
      if(c==0) break;
      p += (c=='\\')?2:1;
    }
    for(size_t k=0;k<ll && o<sizeof(wrapped)-1;k++) wrapped[o++]=line[k];
    wrapped[o]=0;
    str=wrapped;
  }
  if(sep<=0){ gml_draw_text_transformed(r,x,y,str,1,1,0,r->color,r->alpha); return; }
  /* custom line separation: draw line by line at y + i*sep */
  int nlines=1; for(const char *q=str;*q;q++){ if(*q=='\\'&&q[1]=='#'){q++;continue;} if(*q=='#') nlines++; }
  double y0=y;
  if(r->valign==1) y0=y-(nlines*sep)/2.0; else if(r->valign==2) y0=y-nlines*sep;
  int sv=r->valign; r->valign=0;
  const char *p=str; int li=0;
  char lbuf[1024];
  while(1){
    size_t k=0;
    while(*p && !( *p=='#' && (p==str || p[-1]!='\\') ) && k<sizeof(lbuf)-1) lbuf[k++]=*p++;
    lbuf[k]=0;
    gml_draw_text_transformed(r,x,y0+li*sep,lbuf,1,1,0,r->color,r->alpha);
    if(*p!='#') break;
    p++; li++;
  }
  r->valign=sv;
}
