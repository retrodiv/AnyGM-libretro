/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Instance-owned CPU row-band workers and unchanged software dispatch. */
#include "gml_render_cpu_workers.h"
#include "gml_render_internal.h"
#include "gml_thread.h"

#include <stdlib.h>
#include <string.h>

GML_THREAD_BRIDGE_IMPL

/* Persistent row-worker pool shared by the full-screen software compositor passes. Creating and
 * joining 15-31 OS threads for every frame is particularly expensive on Windows and dominated
 * fast-forward at high resolutions. The renderer is single-caller, so one small global pool can
 * safely serve every renderer instance, growing lazily and shutting down with renderer teardown. */
typedef struct GmlRowPool GmlRowPool;
typedef struct { GmlRowPool *pool; int slot; unsigned seen_epoch; } GmlRowWorker;
struct GmlRowPool {
  gml_thread_t thread[GML_ROW_THREADS_MAX-1];
  GmlRowWorker worker[GML_ROW_THREADS_MAX-1];
  int initialized, nworkers, stop, nt, H;
  int spin_budget;
  long hot_frame;                 /* renderer frame the dispatch count below belongs to */
  int hot_count;
  int remaining;
  unsigned epoch;
  GmlRowBandFn fn;
  void *ctx;
  gml_mutex_t mutex;
  gml_cond_t start_cond, done_cond;
};
/* A pass hands its rows to the workers many times in a row - one handover per textured draw wide
 * enough to be worth splitting - and the handover itself, not the pixels, is what a mutex-and-
 * condition-variable rendezvous makes expensive: every worker has to take the same lock to be told
 * the work exists and again to say it is finished. Publishing the descriptor through an epoch the
 * workers can read without the lock, and counting completions down atomically, lets a worker that
 * is still awake between two handovers pick the next one up with a single load. The lock stays for
 * the case the spin is for: a worker that has been idle long enough sleeps on it, and the wake-up
 * path is unchanged, so a pass that arrives after a long gap costs exactly what it used to.
 *
 * The split itself is untouched: the same band boundaries, the same callback, the same order of
 * rows within a band, so every pass writes the pixels it wrote before. */
#if defined(__i386__) || defined(__x86_64__)
#define gml_cpu_relax() __builtin_ia32_pause()
#elif defined(__aarch64__) || defined(__arm__)
#define gml_cpu_relax() __asm__ __volatile__("yield" ::: "memory")
#else
#define gml_cpu_relax() ((void)0)
#endif
/* Long enough to carry a worker across the gap between two handovers of the same pass, short
 * enough that an idle renderer gives the CPU back promptly. */
#define GML_ROW_SPIN_DEFAULT 30000
/* One pool serves every pass, so a pass may not hand out rows while it is already inside a
 * handover: the descriptor is a single slot and the band it would overwrite belongs to a caller
 * still writing through it. That never used to be reachable, because only a very large draw was
 * split at all; a draw small enough to sit inside another pass's band is now split too, so the
 * inner one runs on the calling thread. Workers set the flag for their whole life: everything they
 * ever run is a band body. */
static __thread int gml_row_band_active;
static void *gml_rowband_worker(void *p){
  GmlRowWorker *w=(GmlRowWorker*)p;
  GmlRowPool *pool=w->pool;
  gml_row_band_active=1;
  for(;;){
    unsigned epoch=w->seen_epoch;
    int spins=pool->spin_budget;
    for(;;){
      if(__atomic_load_n(&pool->stop,__ATOMIC_ACQUIRE)) return NULL;
      epoch=__atomic_load_n(&pool->epoch,__ATOMIC_ACQUIRE);
      if(epoch!=w->seen_epoch) break;
      if(spins-- <= 0){
        gml_mutex_lock(&pool->mutex);
        while(!__atomic_load_n(&pool->stop,__ATOMIC_ACQUIRE) &&
              __atomic_load_n(&pool->epoch,__ATOMIC_ACQUIRE)==w->seen_epoch)
          gml_cond_wait(&pool->start_cond,&pool->mutex);
        gml_mutex_unlock(&pool->mutex);
        if(__atomic_load_n(&pool->stop,__ATOMIC_ACQUIRE)) return NULL;
        epoch=__atomic_load_n(&pool->epoch,__ATOMIC_ACQUIRE);
        break;
      }
      gml_cpu_relax();
    }
    /* Teardown bumps the epoch too, so a worker can leave the wait on a handover that does not
     * exist. Read the stop flag once more before touching the descriptor: it was published before
     * that epoch, so a worker that sees the epoch sees the flag, and the pointers it would
     * otherwise call belong to a caller that has long returned. */
    if(__atomic_load_n(&pool->stop,__ATOMIC_ACQUIRE)) return NULL;
    w->seen_epoch=epoch;
    if(w->slot>=pool->nt) continue;          /* this handover wanted fewer bands than there are workers */
    pool->fn(pool->ctx,(int)((long)pool->H*w->slot/pool->nt),
             (int)((long)pool->H*(w->slot+1)/pool->nt),w->slot);
    if(__atomic_sub_fetch(&pool->remaining,1,__ATOMIC_ACQ_REL)==0){
      gml_mutex_lock(&pool->mutex);
      gml_cond_broadcast(&pool->done_cond);
      gml_mutex_unlock(&pool->mutex);
    }
  }
}
static GmlRowPool *gml_row_pool_init(GmlRender *r){
  if(!r) return NULL;
  if(!r->row_pool) r->row_pool=calloc(1,sizeof(GmlRowPool));
  GmlRowPool *p=(GmlRowPool*)r->row_pool;
  if(!p || p->initialized) return p;
  memset(p,0,sizeof *p);
  gml_mutex_init(&p->mutex);
  gml_cond_init(&p->start_cond);
  gml_cond_init(&p->done_cond);
  { const char *pinned=render_setting(r,"GML_ROW_SPIN");
    p->spin_budget=pinned?atoi(pinned):GML_ROW_SPIN_DEFAULT;
    if(p->spin_budget<0) p->spin_budget=0; }
  p->initialized=1;
  return p;
}
static int gml_row_pool_ensure(GmlRender *r,int nt){
  GmlRowPool *p=gml_row_pool_init(r);
  if(!p) return 1;
  int want=nt-1;
  if(want>GML_ROW_THREADS_MAX-1) want=GML_ROW_THREADS_MAX-1;
  while(p->nworkers<want){
    int i=p->nworkers;
    GmlRowWorker *w=&p->worker[i];
    w->pool=p; w->slot=i+1; w->seen_epoch=p->epoch;
    if(gml_thread_create(&p->thread[i],gml_rowband_worker,w)!=0) break;
    p->nworkers++;
  }
  return p->nworkers+1;
}
void gml_row_pool_free(GmlRender *r){
  GmlRowPool *p=r?(GmlRowPool*)r->row_pool:NULL;
  if(!p) return;
  if(!p->initialized){ free(p); r->row_pool=NULL; return; }
  gml_mutex_lock(&p->mutex);
  __atomic_store_n(&p->stop,1,__ATOMIC_RELEASE);
  __atomic_add_fetch(&p->epoch,1,__ATOMIC_ACQ_REL);
  gml_cond_broadcast(&p->start_cond);
  gml_mutex_unlock(&p->mutex);
  for(int i=0;i<p->nworkers;i++) gml_thread_join(p->thread[i]);
  gml_cond_destroy(&p->done_cond);
  gml_cond_destroy(&p->start_cond);
  gml_mutex_destroy(&p->mutex);
  free(p);
  r->row_pool=NULL;
}
void gml_run_row_bands_n(GmlRender *r,int H,int nt,GmlRowBandFn fn,void *ctx){
  if(nt<=1 || gml_row_band_active){ fn(ctx,0,H,0); return; }
  int actual=gml_row_pool_ensure(r,nt);
  GmlRowPool *p=(GmlRowPool*)r->row_pool;
  nt=actual;
  if(nt<=1){ fn(ctx,0,H,0); return; }
  gml_row_band_active=1;
  if(p->hot_frame!=r->frame){ p->hot_frame=r->frame; p->hot_count=0; }
  if(p->hot_count<GML_ROW_BAND_HOT_DISPATCHES) p->hot_count++;
  gml_mutex_lock(&p->mutex);
  p->fn=fn; p->ctx=ctx; p->H=H; p->nt=nt;
  __atomic_store_n(&p->remaining,nt-1,__ATOMIC_RELEASE);
  __atomic_add_fetch(&p->epoch,1,__ATOMIC_ACQ_REL);
  gml_cond_broadcast(&p->start_cond);
  gml_mutex_unlock(&p->mutex);
  fn(ctx,0,(int)((long)H/nt),0);
  for(int spins=p->spin_budget;;){
    if(__atomic_load_n(&p->remaining,__ATOMIC_ACQUIRE)<=0) break;
    if(spins-- <= 0){
      gml_mutex_lock(&p->mutex);
      while(__atomic_load_n(&p->remaining,__ATOMIC_ACQUIRE)>0)
        gml_cond_wait(&p->done_cond,&p->mutex);
      gml_mutex_unlock(&p->mutex);
      break;
    }
    gml_cpu_relax();
  }
  gml_row_band_active=0;
}
/* Whether splitting this draw across the workers is worth the handover. Only the caller knows how
 * many pixels the draw actually covers, and only the pool knows whether the workers are still
 * awake from the draws before it, so the two halves meet here. */
int gml_render_row_bands_profitable(GmlRender *r,unsigned long long visible_pixels){
  if(visible_pixels>=GML_ROW_BAND_MIN_PIXELS_COLD) return 1;
  if(visible_pixels<GML_ROW_BAND_MIN_PIXELS_HOT) return 0;
  GmlRowPool *p=r?(GmlRowPool*)r->row_pool:NULL;
  return p && p->initialized && p->hot_frame==r->frame &&
         p->hot_count>=GML_ROW_BAND_HOT_DISPATCHES;
}
/* An explicit band count can make compositor scheduling reproducible across hosts.
 * GML_ROW_THREADS=1 composites on the calling thread alone. */
void gml_run_row_bands(GmlRender *r,int H,GmlRowBandFn fn,void *ctx){
  int nt=1;
  if(H>=128){ nt=gml_ncpu(); if(nt>GML_ROW_THREADS_AUTO)nt=GML_ROW_THREADS_AUTO; if(nt<1)nt=1; }
  const char *pinned=render_setting(r,"GML_ROW_THREADS");
  if(pinned){ nt=atoi(pinned); if(nt<1) nt=1; if(nt>GML_ROW_THREADS_MAX) nt=GML_ROW_THREADS_MAX; }
  gml_run_row_bands_n(r,H,nt,fn,ctx);
}
