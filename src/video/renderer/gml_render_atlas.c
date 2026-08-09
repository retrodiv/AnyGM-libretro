/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Atlas decode, cache accounting, worker pool, prefetch, and warm operations. */
#include "gml_render_internal.h"
#include "gm_qoi.h"
#include "bzip2/bzlib.h"
#include "gml_thread.h"
#include "anygm_host.h"
#include "anygm_vfs.h"
#include "gml_image_codec.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GML_EXTERNAL_TEXTURE_MAX_BYTES (64u*1024u*1024u)
#define GML_EXTERNAL_TEXTURE_TOTAL_BYTES (256u*1024u*1024u)

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
    /* "2zoq" carries width, height, and a bzip2 stream, optionally preceded by a stored
     * decompressed length. The bzip2 magic distinguishes the two layouts. Treating "BZh9" as a
     * length would exceed the sanity bound and leave the atlas without decoded pixels. */
    if(avail<12) return NULL;
    size_t stream=0;
    uint32_t dlen=0;
    if(!memcmp(blob+8,"BZh",3)){
      stream=8;
      /* No length was stored, so bound it by what the image can be: a QOI stream never exceeds its
       * own raw size, and the dimensions are right there in the header. */
      uint32_t w=u16(blob,4), h=u16(blob,6);
      uint64_t bound=(uint64_t)w*(uint64_t)h*4ull+64ull;
      if(!w || !h || bound>64ull*1024*1024) return NULL;
      dlen=(uint32_t)bound;
    } else if(avail>=15 && !memcmp(blob+12,"BZh",3)){
      stream=12;
      dlen=u32(blob,8);
    } else return NULL;
    if(dlen<12 || dlen>64u*1024*1024) return NULL;
    char *dec=malloc(dlen); if(!dec) return NULL;
    unsigned int declen=dlen;
    unsigned int srclen=(unsigned int)(chunk_end>(size_t)(blob+stream)? chunk_end-(size_t)(blob+stream) : 0);
    int rc=BZ2_bzBuffToBuffDecompress(dec,&declen,(char*)(blob+stream),srclen,0,0);
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
static int log_atlas_on(const GmlRender *render){
  return render_setting(render,"GML_LOG_ATLAS")!=NULL;
}
static size_t atlas_spec_budget(GmlRender *r){
  return r&&r->atlas_prefetch_budget?r->atlas_prefetch_budget:512u*1024u*1024u;
}
static int atlas_has_source(const GmlRender *r,const GmlAtlas *a){
  return a && ((a->external_blob && a->external_size) ||
               (r && r->win && a->blob && a->blob<r->win->size));
}
/* decode outside the lock, publish under it. Returns the published pixels (or NULL). */
static uint8_t *atlas_decode_publish(GmlRender *r, int idx, int locked, GmlAtlasPool *pool){
  GmlAtlas *a=&r->atlas[idx];
  int w=0,h=0;
  uint8_t *px=NULL;
  if(a->external_blob && a->external_size)
    px=decode_texture_blob(a->external_blob,a->external_size,
                           (size_t)(a->external_blob+a->external_size),&w,&h);
  else if(a->blob && a->blob<r->win->size)
    px=decode_texture_blob(r->win->data+a->blob,a->avail,
                           (size_t)r->win->data+a->chunk_end,&w,&h);
  if(locked) gml_mutex_lock(&pool->mu);
  a->decode_attempted=1;
  /* Log an attempted decode that did not produce pixels; successful decodes
   * retain their separate record below. */
  if(!px && log_atlas_on(r))
    anygm_host_logf(r && r->win ? r->win->host : NULL,ANYGM_LOG_DEBUG,
      "[atlas] REFUSED %d (blob=%u avail=%zu external=%s)\n",idx,a->blob,a->avail,
      a->external_blob?"yes":"no");
  if(px){
    a->w=w; a->h=h;
    r->atlas_decoded_bytes += (size_t)w*(size_t)h*4u;
    __atomic_store_n(&a->px,px,__ATOMIC_RELEASE);
    if(log_atlas_on(r))
      anygm_host_logf(r && r->win ? r->win->host : NULL,ANYGM_LOG_DEBUG,"[atlas] decoded %d %dx%d (%.1f MiB)\n",idx,w,h,(double)((uint64_t)w*(uint64_t)h*4ull)/(1024.0*1024.0));
    /* Optional decoded-pixel digest for comparing atlas output across runs;
     * logging does not affect the pixels used by the renderer. */
    if(render_setting(r,"GML_LOG_ATLAS_HASH")){
      uint64_t hash=1469598103934665603ull;                     /* FNV-1a over the decoded RGBA */
      size_t bytes=(size_t)w*(size_t)h*4u;
      for(size_t i=0;i<bytes;i++){ hash^=px[i]; hash*=1099511628211ull; }
      anygm_host_logf(r && r->win ? r->win->host : NULL,ANYGM_LOG_DEBUG,
                      "[atlas-hash] %d %dx%d %016llx\n",idx,w,h,(unsigned long long)hash);
    }
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
  if(a->px || a->decode_attempted || !atlas_has_source(r,a)) return;
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
  if(!a->px && !atlas_has_source(r,a) && render_setting(r,"GML_LOG_ATLAS")){
    /* Report a requested page without a pixel source once per page. */
    if(!a->no_source_reported){
      a->no_source_reported=1;
      anygm_host_logf(r->win?r->win->host:NULL,ANYGM_LOG_DEBUG,
        "[atlas] NO SOURCE %d blob=%llu external=%p/%llu\n",idx,
        (unsigned long long)a->blob,(const void *)a->external_blob,
        (unsigned long long)a->external_size);
    }
  }
  uint8_t *p=__atomic_load_n(&a->px,__ATOMIC_ACQUIRE);
  if(p){ atlas_dump_maybe(r,idx); return p; }
  GmlAtlasPool *pool=(GmlAtlasPool*)r->prefetch;
  if(!pool){
    if(a->decode_attempted || !atlas_has_source(r,a)) return NULL;
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
  if(a->px || a->decode_attempted || !atlas_has_source(r,a)) return;
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

static int chunk_has_absolute(const GmlWin *win,const GmlChunk *chunk,
                              size_t offset,size_t length){
  size_t chunk_start=chunk?chunk->off:0;
  size_t chunk_size=chunk?chunk->size:0;
  return win && chunk && offset>=chunk_start && offset<=win->size &&
         length<=win->size-offset && offset-chunk_start<=chunk_size &&
         length<=chunk_size-(offset-chunk_start);
}

static int chunk_read_u32_absolute(const GmlWin *win,const GmlChunk *chunk,
                                   size_t offset,uint32_t *value){
  if(!value || !chunk_has_absolute(win,chunk,offset,4)) return 0;
  *value=u32(win->data,(uint32_t)offset);
  return 1;
}

static const char *texture_group_string(const GmlWin *win,uint32_t pointer){
  if(!win || !win->strs || !win->str_charoff || win->n_strs<=0) return NULL;
  int low=0,high=win->n_strs-1;
  while(low<=high){
    int middle=low+(high-low)/2;
    uint32_t current=win->str_charoff[middle];
    if(current==pointer) return win->strs[middle];
    if(current<pointer) low=middle+1;
    else high=middle-1;
  }
  return NULL;
}

static int texture_group_leaf_safe(const char *text){
  if(!text || !text[0] || !strcmp(text,".") || !strcmp(text,"..")) return 0;
  for(const unsigned char *p=(const unsigned char *)text;*p;p++)
    if(*p<' ' || *p=='/' || *p=='\\' || *p==':') return 0;
  return 1;
}

static int texture_group_directory_safe(const char *text){
  if(!text || text[0]=='/' || text[0]=='\\') return 0;
  const char *segment=text;
  for(const char *p=text;;p++){
    unsigned char ch=(unsigned char)*p;
    if(ch && ch<' ') return 0;
    if(ch==':') return 0;
    if(!ch || ch=='/' || ch=='\\'){
      size_t length=(size_t)(p-segment);
      if(length==2 && segment[0]=='.' && segment[1]=='.') return 0;
      if(!ch) break;
      segment=p+1;
    }
  }
  return 1;
}

static char *external_texture_path(const GmlWin *win,const char *directory,
                                   const char *group,int index,const char *extension){
  const char *base=win&&win->content_dir[0]?win->content_dir:".";
  if(!texture_group_directory_safe(directory) || !texture_group_leaf_safe(group) ||
     !texture_group_leaf_safe(extension) || index<0) return NULL;
  size_t base_length=strlen(base),directory_length=strlen(directory);
  size_t group_length=strlen(group),extension_length=strlen(extension);
  char index_text[32];
  int index_length=snprintf(index_text,sizeof index_text,"%d",index);
  if(index_length<=0 || (size_t)index_length>=sizeof index_text ||
     base_length>1024 || directory_length>1024 || group_length>1024 ||
     extension_length>128) return NULL;
  size_t total=base_length+directory_length+group_length+extension_length+
               (size_t)index_length+4u;
  if(total<base_length || total>4096) return NULL;
  char *path=malloc(total);
  if(!path) return NULL;
  size_t at=0;
  memcpy(path+at,base,base_length); at+=base_length;
  if(at && path[at-1]!='/' && path[at-1]!='\\') path[at++]='/';
  for(size_t i=0;i<directory_length;i++)
    path[at++]=directory[i]=='\\'?'/':directory[i];
  if(directory_length && path[at-1]!='/') path[at++]='/';
  memcpy(path+at,group,group_length); at+=group_length;
  path[at++]='_';
  memcpy(path+at,index_text,(size_t)index_length); at+=(size_t)index_length;
  memcpy(path+at,extension,extension_length); at+=extension_length;
  path[at]=0;
  return path;
}

static void load_external_texture(GmlRender *r,const GmlChunk *txtr,uint32_t atlas_index,
                                  const char *directory,const char *group,
                                  const char *extension,size_t *total_bytes){
  if(!r || !r->win || !txtr || atlas_index>=(uint32_t)r->n_atlas ||
     !total_bytes || r->atlas[atlas_index].blob ||
     r->atlas[atlas_index].external_blob) return;
  size_t table_entry=(size_t)txtr->off+4u+(size_t)atlas_index*4u;
  uint32_t record=0,encoded_size=0,width=0,height=0,index_raw=0,blob=0;
  if(!chunk_read_u32_absolute(r->win,txtr,table_entry,&record) ||
     !chunk_read_u32_absolute(r->win,txtr,(size_t)record+8u,&encoded_size) ||
     !chunk_read_u32_absolute(r->win,txtr,(size_t)record+12u,&width) ||
     !chunk_read_u32_absolute(r->win,txtr,(size_t)record+16u,&height) ||
     !chunk_read_u32_absolute(r->win,txtr,(size_t)record+20u,&index_raw) ||
     !chunk_read_u32_absolute(r->win,txtr,(size_t)record+24u,&blob) ||
     blob || !encoded_size || encoded_size>GML_EXTERNAL_TEXTURE_MAX_BYTES ||
     !width || !height || width>INT_MAX || height>INT_MAX ||
     (uint64_t)width*(uint64_t)height>64ull*1024ull*1024ull ||
     index_raw>INT_MAX || *total_bytes>GML_EXTERNAL_TEXTURE_TOTAL_BYTES-encoded_size) return;
  char *path=external_texture_path(r->win,directory,group,(int)index_raw,extension);
  if(!path) return;
  uint8_t *encoded=NULL;
  size_t actual_size=0;
  int loaded=anygm_vfs_read_all(r->win->host,path,&encoded,&actual_size,encoded_size);
  free(path);
  int actual_width=0,actual_height=0;
  if(!loaded || actual_size!=encoded_size ||
     !texture_blob_dims(encoded,actual_size,&actual_width,&actual_height) ||
     actual_width!=(int)width || actual_height!=(int)height){
    free(encoded);
    return;
  }
  GmlAtlas *atlas=&r->atlas[atlas_index];
  atlas->external_blob=encoded;
  atlas->external_size=actual_size;
  atlas->w=(int)width;
  atlas->h=(int)height;
  *total_bytes+=actual_size;
}

static void load_external_texture_groups(GmlRender *r,const GmlChunk *txtr){
  const GmlChunk *tgin=gml_chunk(r->win,"TGIN");
  uint32_t version=0,count=0;
  if(!tgin ||
     !chunk_read_u32_absolute(r->win,tgin,tgin->off,&version) || version!=1 ||
     !chunk_read_u32_absolute(r->win,tgin,(size_t)tgin->off+4u,&count) ||
     count>GML_WIN_MAX_REFERENCES ||
     !chunk_has_absolute(r->win,tgin,(size_t)tgin->off+8u,(size_t)count*4u)) return;
  size_t total_bytes=0;
  for(uint32_t i=0;i<count;i++){
    uint32_t record=0,name_pointer=0,directory_pointer=0,extension_pointer=0;
    uint32_t load_type=0,pages_pointer=0,page_count=0;
    if(!chunk_read_u32_absolute(r->win,tgin,(size_t)tgin->off+8u+(size_t)i*4u,&record) ||
       !chunk_read_u32_absolute(r->win,tgin,record,&name_pointer) ||
       !chunk_read_u32_absolute(r->win,tgin,(size_t)record+4u,&directory_pointer) ||
       !chunk_read_u32_absolute(r->win,tgin,(size_t)record+8u,&extension_pointer) ||
       !chunk_read_u32_absolute(r->win,tgin,(size_t)record+12u,&load_type) ||
       !chunk_read_u32_absolute(r->win,tgin,(size_t)record+16u,&pages_pointer) ||
       !load_type || load_type>2 ||
       !chunk_read_u32_absolute(r->win,tgin,pages_pointer,&page_count) ||
       page_count>GML_WIN_MAX_REFERENCES ||
       !chunk_has_absolute(r->win,tgin,(size_t)pages_pointer+4u,(size_t)page_count*4u))
      continue;
    const char *name=texture_group_string(r->win,name_pointer);
    const char *directory=texture_group_string(r->win,directory_pointer);
    const char *extension=texture_group_string(r->win,extension_pointer);
    if(!name || !directory || !extension) continue;
    for(uint32_t page=0;page<page_count;page++){
      uint32_t atlas_index=0;
      if(chunk_read_u32_absolute(r->win,tgin,(size_t)pages_pointer+4u+(size_t)page*4u,
                                 &atlas_index))
        load_external_texture(r,txtr,atlas_index,directory,name,extension,&total_bytes);
    }
  }
}

void parse_txtr(GmlRender *r){
  const GmlChunk *c=gml_chunk(r->win,"TXTR");
  uint32_t n=0;
  if(!c || !chunk_read_u32_absolute(r->win,c,c->off,&n) ||
     n>GML_WIN_MAX_REFERENCES ||
     !chunk_has_absolute(r->win,c,(size_t)c->off+4u,(size_t)n*4u)) return;
  const uint8_t *d=r->win->data;
  /* Store the chunk boundary as an offset, then rebuild its address from the
   * current payload buffer when decoding an atlas. */
  size_t chunk_end=(size_t)c->off + (size_t)c->size;
  r->atlas=calloc(n?n:1,sizeof(GmlAtlas));
  if(!r->atlas) return;
  r->n_atlas=(int)n;
  for(uint32_t i=0;i<n;i++){
    uint32_t entry=0;
    if(!chunk_read_u32_absolute(r->win,c,(size_t)c->off+4u+(size_t)i*4u,&entry))
      continue;
    /* Locate the texture data through a candidate pointer in its record. */
    uint32_t blob=0;
    /* The EmbeddedTexture record holds the blob pointer at a version-dependent offset. Scan the
     * bounded record for the field that lands on a known PNG, fioq, or 2zoq magic. */
    for(uint32_t off=4;off<=32;off+=4){
      uint32_t v=0;
      if(!chunk_read_u32_absolute(r->win,c,(size_t)entry+off,&v)) break;
      if((size_t)v+4<=r->win->size){ const uint8_t *m=d+v;
        if((m[0]==0x89&&m[1]=='P'&&m[2]=='N'&&m[3]=='G')||!memcmp(m,"fioq",4)||!memcmp(m,"2zoq",4)){ blob=v; break; } } }
    if(!blob || (size_t)blob>=r->win->size) continue;
    GmlAtlas *a=&r->atlas[i];
    a->blob=blob; a->avail=r->win->size-blob; a->chunk_end=chunk_end;
    texture_blob_dims(d+blob,a->avail,&a->w,&a->h);
  }
  load_external_texture_groups(r,c);
  if(render_setting(r,"GML_ATLAS_EAGER") || render_setting(r,"GML_DUMP_ATLAS"))
    for(uint32_t i=0;i<n;i++) atlas_pixels(r,(int)i);
}
