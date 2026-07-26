/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Atlas decode, cache accounting, worker pool, prefetch, and warm operations. */
#include "gml_render_internal.h"
#include "gm_qoi.h"
#include "bzip2/bzlib.h"
#include "gml_thread.h"
#include "anygm_host.h"
#include "gml_image_codec.h"

#include <stdlib.h>
#include <string.h>

/* ---- atlas (TXTR) ---- */
/* A texture blob is one of: PNG (bc14-16), a bare GameMaker-QOI "fioq" stream, or a bzip2-compressed
 * "2zoq" container wrapping a "fioq" stream (bc17). Decode any of them to RGBA. */
static uint8_t *decode_texture_blob(const uint8_t *blob, size_t avail, size_t chunk_end,
                                    int *ow, int *oh){
  if(avail<4) return NULL;
  if(blob[0]==0x89 && blob[1]=='P' && blob[2]=='N' && blob[3]=='G'){
    int w=0,h=0,ch=0;
    GmlMediaBuffer image={0};
    if(gml_image_decode_rgba(blob,avail,&image,&w,&h,&ch)){
      *ow=w; *oh=h;
      return image.data;
    }
    return NULL;
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
  if(ow) *ow=w;
  if(oh) *oh=h;
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
void atlas_pool_free(GmlRender *r){
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
uint8_t *atlas_pixels(GmlRender *r, int idx){
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
void parse_txtr(GmlRender *r){
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
