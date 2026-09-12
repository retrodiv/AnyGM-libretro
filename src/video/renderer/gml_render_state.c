/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Canonical renderer payload encoding for root savestates. */
#include "gml_render_state.h"
#include "gml_render_internal.h"
#include "gml_font_raster.h"
#include "gml_image_codec.h"
#include "anygm_vfs.h"

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

typedef struct { uint8_t *data; size_t cap, pos; int ok; } CoreW;
typedef struct { const uint8_t *data; size_t cap, pos; int ok; } CoreR;
/* An independent expansion budget for the compressed sprite representation.
 * Larger live sets retain raw records rather than producing an unreadable state. */
#define GML_STATE_COMPRESSED_SPRITE_BUDGET (256u*1024u*1024u)
/* Match the content reader's per-string bound, plus its terminating NUL. */
#define GML_STATE_SPRITE_NAME_BYTES (16u*1024u*1024u+1u)

void gml_render_sprite_state_cache_clear(GmlSprite *sprite){
  if(!sprite) return;
  free(sprite->runtime_state_data);
  sprite->runtime_state_data=NULL;
  sprite->runtime_state_size=0;
  sprite->runtime_state_cached=0;
}

static int sprite_state_cache_prepare(GmlSprite *sprite,size_t bytes){
  if(sprite->runtime_state_cached) return 1;
  GmlMediaBuffer encoded={0};
  if(bytes>=4096 && bytes<=GML_STATE_COMPRESSED_SPRITE_BUDGET){
    /* Allocation failure must not permanently select a different wire representation. */
    if(!gml_deflate_encode_zlib(sprite->runtime_rgba,bytes,&encoded)) return 0;
    if(encoded.size+4<bytes){
      sprite->runtime_state_data=encoded.data;
      sprite->runtime_state_size=encoded.size;
    }else gml_media_buffer_release(&encoded);
  }
  sprite->runtime_state_cached=1;
  return 1;
}
static void cw_raw(CoreW *s, const void *p, size_t n){
  if(n>SIZE_MAX-s->pos){ s->ok=0; s->pos=SIZE_MAX; return; }
  if(s->data){ if(s->pos<=s->cap && n<=s->cap-s->pos) memcpy(s->data+s->pos,p,n); else s->ok=0; }
  s->pos+=n;
}
static void cr_raw(CoreR *s, void *p, size_t n){
  if(n>SIZE_MAX-s->pos){ memset(p,0,n); s->ok=0; s->pos=SIZE_MAX; return; }
  if(s->pos<=s->cap && n<=s->cap-s->pos) memcpy(p,s->data+s->pos,n);
  else { memset(p,0,n); s->ok=0; }
  s->pos+=n;
}
static void cw_u32(CoreW *s, uint32_t v){
  uint8_t bytes[4]={(uint8_t)v,(uint8_t)(v>>8),(uint8_t)(v>>16),(uint8_t)(v>>24)};
  cw_raw(s,bytes,sizeof bytes);
}
static void cw_i32(CoreW *s, int v){ cw_u32(s,(uint32_t)(int32_t)v); }
static void cw_u64(CoreW *s, uint64_t v){
  uint8_t bytes[8];
  for(unsigned i=0;i<8;i++) bytes[i]=(uint8_t)(v>>(i*8));
  cw_raw(s,bytes,sizeof bytes);
}
static void cw_d(CoreW *s, double v){ uint64_t bits=0; memcpy(&bits,&v,sizeof bits); cw_u64(s,bits); }
static uint32_t cr_u32(CoreR *s){
  uint8_t bytes[4]={0}; cr_raw(s,bytes,sizeof bytes);
  return (uint32_t)bytes[0]|((uint32_t)bytes[1]<<8)|((uint32_t)bytes[2]<<16)|((uint32_t)bytes[3]<<24);
}
static int cr_i32(CoreR *s){ return (int)(int32_t)cr_u32(s); }
static uint64_t cr_u64(CoreR *s){
  uint8_t bytes[8]={0}; cr_raw(s,bytes,sizeof bytes); uint64_t value=0;
  for(unsigned i=0;i<8;i++) value|=(uint64_t)bytes[i]<<(i*8);
  return value;
}
static double cr_d(CoreR *s){ uint64_t bits=cr_u64(s); double value=0; memcpy(&value,&bits,sizeof value); return value; }

static int state_bounded_product3(size_t first,size_t second,size_t third,
                                  size_t available,size_t *result){
  if(!result || (second && first>SIZE_MAX/second)) return 0;
  size_t product=first*second;
  if(third && product>SIZE_MAX/third) return 0;
  product*=third;
  if(product>available) return 0;
  *result=product;
  return 1;
}

static void state_store_u32(uint8_t *destination,uint32_t value){
  destination[0]=(uint8_t)value;
  destination[1]=(uint8_t)(value>>8);
  destination[2]=(uint8_t)(value>>16);
  destination[3]=(uint8_t)(value>>24);
}

struct GmlRenderFontCheckpoint {
  GmlRender saved;
};

void gml_render_font_checkpoint_free(GmlRenderFontCheckpoint *checkpoint){
  if(!checkpoint) return;
  gml_render_free(&checkpoint->saved);
  free(checkpoint);
}

int gml_render_font_checkpoint_create(const GmlRender *render,GmlRenderFontCheckpoint **out){
  if(out) *out=NULL;
  if(!render || !out || render->n_fonts<0 || render->n_fonts>GML_MAX_FONTS ||
     render->n_atlas<0 || (size_t)render->n_atlas>SIZE_MAX/sizeof(GmlAtlas)) return 0;
  int present=0;
  for(int i=0;i<GML_MAX_FONTS;i++) if(render->fonts[i].runtime_owned) present++;
  if(!present) return 1;
  GmlRenderFontCheckpoint *checkpoint=calloc(1,sizeof(*checkpoint));
  if(!checkpoint) return 0;
  GmlRender *saved=&checkpoint->saved;
  saved->n_fonts=render->n_fonts;
  saved->atlas=calloc((size_t)render->n_atlas,sizeof(*saved->atlas));
  if(!saved->atlas){ gml_render_font_checkpoint_free(checkpoint); return 0; }
  saved->n_atlas=render->n_atlas;
  int ok=1;
  for(int i=0;ok && i<GML_MAX_FONTS;i++){
    const GmlFont *source=&render->fonts[i];
    if(!source->runtime_owned) continue;
    if(i>=render->n_fonts || source->atlas<0 || source->atlas>=render->n_atlas ||
       !source->runtime_face || !source->runtime_source_path || !source->glyphs ||
       source->n_glyphs<1 || source->n_glyphs>65536 ||
       source->runtime_glyph_cap<source->n_glyphs || source->runtime_glyph_cap>131072 ||
       source->map_len<0 || source->map_len>4096 || source->n_kerning<0 ||
       source->n_kerning>1048576){ ok=0; break; }
    GmlFont *font=&saved->fonts[i];
    *font=*source;
    font->map=NULL; font->glyphs=NULL; font->kerning=NULL;
    font->runtime_source_path=NULL; font->runtime_face=NULL;
    if(!gml_font_raster_face_retain(source->runtime_face)){ ok=0; break; }
    font->runtime_face=source->runtime_face;
    font->runtime_source_path=strdup(source->runtime_source_path);
    font->glyphs=calloc((size_t)source->runtime_glyph_cap,sizeof(*font->glyphs));
    if(!font->runtime_source_path || !font->glyphs){ ok=0; break; }
    memcpy(font->glyphs,source->glyphs,(size_t)source->n_glyphs*sizeof(*font->glyphs));
    if(source->map_len){
      font->map=malloc((size_t)source->map_len*sizeof(*font->map));
      if(!source->map || !font->map){ ok=0; break; }
      memcpy(font->map,source->map,(size_t)source->map_len*sizeof(*font->map));
    }
    if(source->n_kerning){
      font->kerning=malloc((size_t)source->n_kerning*sizeof(*font->kerning));
      if(!source->kerning || !font->kerning){ ok=0; break; }
      memcpy(font->kerning,source->kerning,(size_t)source->n_kerning*sizeof(*font->kerning));
    }
    const GmlAtlas *atlas=&render->atlas[source->atlas];
    GmlAtlas *copy=&saved->atlas[source->atlas];
    size_t bytes=0;
    if(copy->px || !atlas->px || atlas->w<=0 || atlas->h<=0 ||
       !state_bounded_product3((size_t)atlas->w,(size_t)atlas->h,4,128u*1024u*1024u,&bytes)){
      ok=0; break;
    }
    *copy=*atlas; copy->px=NULL; copy->external_blob=NULL;
    copy->px=malloc(bytes);
    if(!copy->px){ ok=0; break; }
    memcpy(copy->px,atlas->px,bytes);
  }
  if(!ok){ gml_render_font_checkpoint_free(checkpoint); return 0; }
  *out=checkpoint;
  return 1;
}

int gml_render_font_checkpoint_restore(GmlRender *render,GmlRenderFontCheckpoint *checkpoint){
  if(!checkpoint) return 1;
  GmlRender *saved=&checkpoint->saved;
  /* Font loads may append/reuse pages, but never shrink the authored atlas table. */
  if(!render || render->n_atlas<saved->n_atlas) return 0;
  render->n_fonts=GML_MAX_FONTS;
  for(int i=0;i<GML_MAX_FONTS;i++)
    if(render->fonts[i].runtime_owned) gml_font_delete(render,i);
  for(int i=0;i<GML_MAX_FONTS;i++){
    GmlFont *font=&saved->fonts[i];
    if(!font->runtime_owned) continue;
    gml_font_delete(render,i);
    render->fonts[i]=*font;
    int atlas=font->atlas;
    free(render->atlas[atlas].px);
    render->atlas[atlas]=saved->atlas[atlas];
    memset(&saved->atlas[atlas],0,sizeof(saved->atlas[atlas]));
    memset(font,0,sizeof(*font));
  }
  render->n_fonts=saved->n_fonts;
  return 1;
}



/* A runtime sprite records the file it came from so a state can rebuild it without carrying its
 * pixels. The file used to be named by the absolute path the session happened to open, which ties
 * the state to one directory: a container extracted under a different name, a frontend configured
 * with another save directory, or simply another machine leaves every one of those sprites
 * unreadable and the whole state unloadable.
 *
 * Names are stored relative to the root they belong to instead, and rebuilt against the roots the
 * loading session has. */
enum { GML_RUNTIME_PATH_ABSOLUTE=0, GML_RUNTIME_PATH_CONTENT=1, GML_RUNTIME_PATH_SAVE=2 };

static const char *runtime_path_under(const char *path,const char *root){
  if(!path || !root || !root[0]) return NULL;
  size_t n=strlen(root);
  while(n>0 && (root[n-1]=='/' || root[n-1]=='\\')) n--;
  if(!n || strncmp(path,root,n)) return NULL;
  if(path[n]!='/' && path[n]!='\\') return NULL;
  const char *relative=path+n;
  while(*relative=='/' || *relative=='\\') relative++;
  return *relative?relative:NULL;
}

static int runtime_path_store(const GmlRender *render,const char *path,const char **stored){
  const GmlWin *win=render?render->win:NULL;
  const char *relative=win?runtime_path_under(path,win->content_dir):NULL;
  if(relative){ *stored=relative; return GML_RUNTIME_PATH_CONTENT; }
  relative=win?runtime_path_under(path,win->save_dir):NULL;
  if(relative){ *stored=relative; return GML_RUNTIME_PATH_SAVE; }
  *stored=path;
  return GML_RUNTIME_PATH_ABSOLUTE;
}

static int runtime_path_rebuild(const GmlRender *render,int root,const char *stored,
                                char *out,size_t capacity){
  const GmlWin *win=render?render->win:NULL;
  const char *base=NULL;
  if(root==GML_RUNTIME_PATH_CONTENT) base=win?win->content_dir:NULL;
  else if(root==GML_RUNTIME_PATH_SAVE) base=win?win->save_dir:NULL;
  else if(root!=GML_RUNTIME_PATH_ABSOLUTE) return 0;
  int built=(!base || !base[0]) ? snprintf(out,capacity,"%s",stored)<(int)capacity
                                : snprintf(out,capacity,"%s/%s",base,stored)<(int)capacity;
  if(!built) return 0;
  /* The name is whatever the content asked for, and content authored on a case-insensitive
   * filesystem asks with the case it likes. Reading the same bundle on a case-sensitive one has
   * to recover the on-disk spelling, exactly as the content's own file reads do, or a state
   * written on one platform is unreadable on another over nothing but a capital letter. */
  char resolved[4608];
  const AnygmHostServices *host=win?win->host:NULL;
  if(host && anygm_vfs_resolve_casefold(host,out,resolved,sizeof resolved) &&
     strlen(resolved)<capacity)
    memcpy(out,resolved,strlen(resolved)+1);
  return 1;
}

static void render_state_write(GmlRender *render,int view_surface,CoreW *s){
  if(render->n_fonts<0 || render->n_fonts>GML_MAX_FONTS){ s->ok=0; return; }
  cw_i32(s,render->n_fonts);
  for(int i=0;i<GML_MAX_FONTS;i++){
    GmlFont *f=&render->fonts[i];
    cw_i32(s,f->sprite); cw_i32(s,f->first);
    cw_i32(s,f->prop); cw_i32(s,f->sep);
    int map_len=(f->map && f->map_len>0) ? f->map_len : 0;
    cw_i32(s,map_len);
    for(int j=0;j<map_len;j++) cw_u32(s,f->map[j]);
    cw_i32(s,f->runtime_owned?1:0);
    if(f->runtime_owned){
      const char *stored=NULL;
      if(i>=render->n_fonts || !f->runtime_source_path || !f->runtime_face ||
         !f->glyphs || f->n_glyphs<1 || f->n_glyphs>65536){ s->ok=0; return; }
      int root=runtime_path_store(render,f->runtime_source_path,&stored);
      size_t length=strlen(stored);
      if(!length || length>4095){ s->ok=0; return; }
      cw_i32(s,root); cw_i32(s,(int)length); cw_raw(s,stored,length);
      cw_raw(s,f->runtime_source_sha256,32);
      cw_i32(s,f->runtime_pixel_height); cw_i32(s,f->runtime_first);
      cw_i32(s,f->runtime_last); cw_i32(s,f->bold); cw_i32(s,f->italic);
      cw_i32(s,f->n_glyphs);
      for(int j=0;j<f->n_glyphs;j++) cw_u32(s,f->glyphs[j].ch);
    }
  }
  cw_i32(s,render->app_draw_enable); cw_u32(s,render->color); cw_d(s,render->alpha);
  cw_i32(s,render->halign); cw_i32(s,render->valign); cw_i32(s,render->font);
  cw_i32(s,render->alphablend);
  cw_i32(s,render->circle_precision);
  cw_i32(s,render->next_surface_id);
  /* The surface assigned to view 0 (view_surface_id) is derived data:
   * anygm_run_frame mirrors the freshly rendered frame into it every frame, after the room pass and
   * before any Draw GUI reads it. Serializing this derived surface needlessly increases every
   * state and rewind delta. Skip its pixels: on load it starts transparent and is repopulated
   * mid-pipeline in the first anygm_run_frame, before the compositor can sample it. */
  for(int i=0;i<GML_MAX_SURFACES;i++){
    GmlSurface *sf=&render->surface[i];
    cw_i32(s,sf->live); cw_i32(s,sf->w); cw_i32(s,sf->h);
    if(sf->live && sf->px && sf->w>0 && sf->h>0){
      size_t n=(size_t)sf->w*sf->h;
      if(view_surface>0 && i==view_surface-1){
        cw_u32(s,1); cw_u32(s,(uint32_t)n); cw_u32(s,0);   /* one transparent run */
        continue;
      }
      /* RLE stores (run,value) pairs because mostly uniform surfaces otherwise dominate state
       * size and rewind deltas. Encoded bytes are cached per surface and rebuilt only after a draw
       * marks the surface dirty, avoiding a scan of every unchanged pixel on each snapshot. */
      if(sf->dirty || !sf->rle){
        size_t need=8;   /* worst grows below */
        size_t nr=0, pos=4;
        if(sf->rle_cap<16){ uint8_t *np=realloc(sf->rle,4096); if(!np){ s->ok=0; continue; } sf->rle=np; sf->rle_cap=4096; }
        for(size_t k=0;k<n;){
          uint32_t v=sf->px[k]; size_t j=k+1;
          while(j<n && sf->px[j]==v && j-k<0xFFFFFFFFu) j++;
          if(pos+8>sf->rle_cap){ size_t nc=sf->rle_cap*2; uint8_t *np=realloc(sf->rle,nc);
            if(!np){ s->ok=0; break; } sf->rle=np; sf->rle_cap=nc; }
          uint32_t run=(uint32_t)(j-k);
          state_store_u32(sf->rle+pos,run);
          state_store_u32(sf->rle+pos+4,v);
          pos+=8; nr++; k=j;
        }
        (void)need;
        state_store_u32(sf->rle,(uint32_t)nr);
        sf->rle_len=pos; sf->dirty=0;
      }
      cw_raw(s,sf->rle,sf->rle_len);
    }
  }
  int runtime_sprites=0;
  size_t compressed_budget=GML_STATE_COMPRESSED_SPRITE_BUDGET;
  for(int i=0;i<render->n_spr;i++) if(render->spr[i].runtime_rgba) runtime_sprites++;
  cw_i32(s,runtime_sprites);
  for(int i=0;i<render->n_spr;i++) if(render->spr[i].runtime_rgba){
    GmlSprite *sp=&render->spr[i];
    cw_i32(s,i); cw_i32(s,sp->runtime_extra);
    int frames=sp->n_frames>0?sp->n_frames:1;
    cw_i32(s,sp->w); cw_i32(s,sp->h); cw_i32(s,frames); cw_i32(s,sp->originx); cw_i32(s,sp->originy);
    cw_i32(s,sp->ml); cw_i32(s,sp->mt); cw_i32(s,sp->mr); cw_i32(s,sp->mb);
    cw_i32(s,sp->collision_kind); cw_i32(s,sp->collision_tolerance);
    if(sp->runtime_source_path && sp->runtime_source_path[0]){
      const char *stored=NULL;
      int root=runtime_path_store(render,sp->runtime_source_path,&stored);
      size_t plen=strlen(stored);
      if(plen>4095) plen=4095;
      cw_i32(s,1);
      cw_i32(s,root);
      cw_i32(s,(int)plen);
      cw_raw(s,stored,plen);
      cw_i32(s,sp->runtime_source_imgnum);
      cw_i32(s,sp->runtime_source_removeback);
    } else {
      size_t pixels=0;
      if(sp->w<=0 || sp->h<=0 ||
         !state_bounded_product3((size_t)sp->w,(size_t)sp->h,(size_t)frames,SIZE_MAX/4,&pixels)){
        s->ok=0;return;
      }
      size_t bytes=pixels*4;
      if(!sprite_state_cache_prepare(sp,bytes)){ s->ok=0; return; }
      if(sp->runtime_state_data && bytes<=compressed_budget){
        compressed_budget-=bytes;
        cw_i32(s,2);
        cw_u32(s,(uint32_t)sp->runtime_state_size);
        cw_raw(s,sp->runtime_state_data,sp->runtime_state_size);
      }else{
        cw_i32(s,0);
        cw_raw(s,sp->runtime_rgba,bytes);
      }
    }
    int mask_rowb=sp->mask && sp->mask_rowb>0 && sp->mask_count>0 ? sp->mask_rowb : 0;
    int mask_count=mask_rowb ? sp->mask_count : 0;
    cw_i32(s,mask_rowb); cw_i32(s,mask_count);
    if(mask_rowb)
      cw_raw(s,sp->mask,(size_t)mask_rowb*(size_t)sp->h*(size_t)mask_count);
    size_t name_bytes=sp->name?strlen(sp->name)+1:0;
    if(name_bytes>GML_STATE_SPRITE_NAME_BYTES){ s->ok=0; return; }
    cw_u32(s,(uint32_t)name_bytes);
    if(name_bytes) cw_raw(s,sp->name,name_bytes);
  }
}


/* Read font-pool records [from,to) into render->fonts. Returns 0 on parse error. */
static int render_state_read_font_records(GmlRender *render,CoreR *s,int from,int to,int count){
  for(int i=from;i<to;i++){
    int sprite=cr_i32(s),first=cr_i32(s),prop=cr_i32(s),sep=cr_i32(s);
    int raw_len=cr_i32(s);
    size_t remaining=s->pos<=s->cap?s->cap-s->pos:0;
    if(raw_len<0 || raw_len>4096 || (size_t)raw_len>remaining/4){ s->ok=0; return 0; }
    uint32_t *map=NULL;
    if(raw_len>0){
      map=malloc((size_t)raw_len*sizeof(*map));
      if(!map){ s->ok=0; return 0; }
    }
    for(int j=0;j<raw_len;j++) map[j]=cr_u32(s);
    int runtime=cr_i32(s);
    if(runtime<0 || runtime>1 ||
       (i>=count && (runtime || (render->fonts[i].real && !render->fonts[i].runtime_owned))))
      s->ok=0;
    if(s->ok && runtime){
      int root=cr_i32(s),length=cr_i32(s);
      char stored[4096],path[4608];
      uint8_t digest[32];
      if(length<1 || length>4095 || sprite!=-1 || first || prop || sep || raw_len){
        free(map); s->ok=0; return 0;
      }
      cr_raw(s,stored,(size_t)length); stored[length]=0;
      if(memchr(stored,0,(size_t)length)) s->ok=0;
      cr_raw(s,digest,sizeof digest);
      int pixels=cr_i32(s),range_first=cr_i32(s),range_last=cr_i32(s);
      int bold=cr_i32(s),italic=cr_i32(s),glyphs=cr_i32(s);
      remaining=s->pos<=s->cap?s->cap-s->pos:0;
      if(!s->ok || glyphs<1 || glyphs>65536 || (size_t)glyphs>remaining/4 ||
         !runtime_path_rebuild(render,root,stored,path,sizeof path)){
        free(map); s->ok=0; return 0;
      }
      uint32_t *characters=malloc((size_t)glyphs*sizeof(*characters));
      if(!characters){ free(map); s->ok=0; return 0; }
      for(int j=0;j<glyphs;j++) characters[j]=cr_u32(s);
      if(!s->ok || !gml_render_restore_runtime_font(render,i,path,digest,pixels,
                                                    bold,italic,range_first,range_last,characters,glyphs))
        s->ok=0;
      free(characters);
    }
    if(!s->ok){ free(map); return 0; }
    if(!runtime && render->fonts[i].runtime_owned){
      if(render->n_fonts<=i) render->n_fonts=i+1;
      gml_font_delete(render,i);
    }
    GmlFont *font=&render->fonts[i];
    free(font->map);
    font->sprite=sprite; font->first=first; font->prop=prop; font->sep=sep;
    font->map=map; font->map_len=raw_len;
  }
  return s->ok;
}


static int render_state_read(GmlRender *render,CoreR *s){
  int nf=cr_i32(s);
  if(!s->ok || nf<0 || nf>GML_MAX_FONTS){ s->ok=0; return 0; }
  if(!render_state_read_font_records(render,s,0,GML_MAX_FONTS,nf)) return 0;
  render->n_fonts=nf;
  gml_render_rebuild_font_maps(render);
  render->app_draw_enable=cr_i32(s); render->color=cr_u32(s); render->alpha=cr_d(s);
  render->halign=cr_i32(s); render->valign=cr_i32(s); render->font=cr_i32(s);
  render->alphablend=cr_i32(s)?1:0;
  render->circle_precision=cr_i32(s);
  if(render->circle_precision<4) render->circle_precision=4;
  if(render->circle_precision>64) render->circle_precision=64;
  render->circle_precision=(render->circle_precision/4)*4;
  if(render->circle_precision<4) render->circle_precision=4;
  render->next_surface_id=cr_i32(s);
  if(render->next_surface_id<1 || render->next_surface_id>GML_MAX_SURFACES) render->next_surface_id=1;
  for(int i=0;i<GML_MAX_SURFACES;i++){
    free(render->surface[i].px); free(render->surface[i].rle);
    memset(&render->surface[i],0,sizeof(render->surface[i]));
  }
  for(int i=0;i<GML_MAX_SURFACES;i++){
    int live=cr_i32(s), w=cr_i32(s), h=cr_i32(s);
    if(live){
      if(w<=0 || h<=0 || w>4096 || h>4096){ s->ok=0; return 0; }
      size_t n=(size_t)w*h;
      render->surface[i].px=malloc(n*sizeof(uint32_t));
      if(!render->surface[i].px){ s->ok=0; return 0; }
      render->surface[i].live=1; render->surface[i].w=w; render->surface[i].h=h;
      int all_opaque = 1, all_transparent = 1;
      uint32_t nrun=cr_u32(s); size_t k=0;
      for(uint32_t r2=0; r2<nrun && s->ok; r2++){
        uint32_t run=cr_u32(s), v=cr_u32(s);
        if(run>n-k){ s->ok=0; break; }
        if((v>>24)!=255u) all_opaque = 0;
        if((v>>24)!=0u) all_transparent = 0;
        for(uint32_t q=0;q<run;q++) render->surface[i].px[k++]=v;
        render->surface[i].dirty=1;
      }
      if(k!=n) { all_opaque = 0; memset(render->surface[i].px+k,0,(n-k)*sizeof(uint32_t)); if(s->ok && k>0) s->ok=1; }
      render->surface[i].opaque_known=1;
      render->surface[i].all_opaque=all_opaque;
      render->surface[i].all_transparent=all_transparent;
    }
  }
  uint8_t *seen_runtime = NULL;
  int seen_cap = 0;
  int runtime_sprites=cr_i32(s);
  size_t compressed_budget=GML_STATE_COMPRESSED_SPRITE_BUDGET;
  if(runtime_sprites<0 || runtime_sprites>4096){ s->ok=0; return 0; }
  seen_cap = render->spr_cap + runtime_sprites + 16;
  if(seen_cap < render->n_spr + runtime_sprites + 16) seen_cap = render->n_spr + runtime_sprites + 16;
  seen_runtime = calloc((size_t)(seen_cap>0?seen_cap:1),1);
  if(!seen_runtime){ s->ok=0; return 0; }
  for(int n=0;n<runtime_sprites;n++){
    int id=cr_i32(s), extra=cr_i32(s);
    int w=cr_i32(s), h=cr_i32(s), frames=cr_i32(s), ox=cr_i32(s), oy=cr_i32(s);
    int ml=cr_i32(s), mt=cr_i32(s), mr=cr_i32(s), mb=cr_i32(s);
    int kind=cr_i32(s), tolerance=cr_i32(s);
    if(id<0 || w<=0 || h<=0 || frames<=0){ s->ok=0; return 0; }
    if(id>=seen_cap){
      int nc=id+256;
      uint8_t *ns=realloc(seen_runtime,(size_t)nc);
      if(!ns){ free(seen_runtime); s->ok=0; return 0; }
      memset(ns+seen_cap,0,(size_t)(nc-seen_cap));
      seen_runtime=ns; seen_cap=nc;
    }
    seen_runtime[id]=1;
    int mode = cr_i32(s);
    if(mode==1){
      /* File-backed runtime sprites must remain within the image decoder's dimensions. Inline
       * sprites can be taller: scaled authored pages and surface copies already produce them, so
       * their bound is the complete pixel product carried by this section rather than one axis. */
      if(w>4096 || h>4096 || frames>4096){ free(seen_runtime); s->ok=0; return 0; }
      int root=cr_i32(s);
      int plen=cr_i32(s);
      if(plen<0 || plen>4095 || root<GML_RUNTIME_PATH_ABSOLUTE || root>GML_RUNTIME_PATH_SAVE){
        free(seen_runtime); s->ok=0; return 0;
      }
      char *stored=malloc((size_t)plen+1);
      if(!stored){ free(seen_runtime); s->ok=0; return 0; }
      cr_raw(s,stored,(size_t)plen); stored[plen]=0;
      char *path=malloc(4608);
      if(!path || !runtime_path_rebuild(render,root,stored,path,4608)){
        free(stored); free(path); free(seen_runtime); s->ok=0; return 0;
      }
      free(stored);
      int imgnum=cr_i32(s), removeback=cr_i32(s);
      int got=-1;
      if(id>=0 && id<render->n_spr){
        GmlSprite *cur=&render->spr[id];
        if(cur->runtime_rgba && cur->runtime_source_path && !strcmp(cur->runtime_source_path,path) &&
           cur->w==w && cur->h==h && cur->n_frames==frames && cur->originx==ox && cur->originy==oy){
          got=id;
        }
      }
      if(got<0){
        if(extra || id>=render->base_n_spr){
          if(id>=0 && id<render->n_spr && render->spr[id].runtime_extra) gml_sprite_delete(render,id);
          got=gml_sprite_add_file(render,path,imgnum,removeback,ox,oy);
        } else {
          got=gml_sprite_replace_from_file(render,id,path,imgnum,removeback,0,ox,oy) ? id : -1;
        }
      }
      free(path);
      if(got!=id || got<0 || got>=render->n_spr){ free(seen_runtime); s->ok=0; return 0; }
      GmlSprite *chk=&render->spr[got];
      if(chk->w!=w || chk->h!=h || chk->n_frames!=frames){ free(seen_runtime); s->ok=0; return 0; }
    } else if(mode==0 || mode==2){
      size_t rem=s->pos<=s->cap ? s->cap-s->pos : 0;
      size_t pixels=0;
      size_t limit=mode==2?compressed_budget:rem;
      if(!state_bounded_product3((size_t)w,(size_t)h,(size_t)frames,limit/4,&pixels)){
        free(seen_runtime); s->ok=0; return 0;
      }
      size_t bytes=pixels*4;
      const uint8_t *encoded=NULL;
      uint32_t encoded_size=0;
      int reuse=0;
      if(mode==2){
        compressed_budget-=bytes;
        encoded_size=cr_u32(s);
        rem=s->pos<=s->cap?s->cap-s->pos:0;
        if(!s->ok || !encoded_size || encoded_size>=bytes || encoded_size>rem){
          free(seen_runtime);s->ok=0;return 0;
        }
        encoded=s->data+s->pos;
        if(id<render->n_spr){
          GmlSprite *current=&render->spr[id];
          /* Equality with our own canonical cache proves the pixels without
           * inflating and replacing an unchanged plane on every rewind pop. */
          reuse=current->runtime_rgba && current->runtime_state_data &&
            current->runtime_extra==extra && current->w==w && current->h==h &&
            current->n_frames==frames && current->originx==ox && current->originy==oy &&
            current->runtime_state_size==encoded_size &&
            !memcmp(current->runtime_state_data,encoded,encoded_size);
        }
      }
      if(!reuse){
        uint8_t *rgba=malloc(bytes);
        if(!rgba){ free(seen_runtime); s->ok=0; return 0; }
        if(mode==2){
          size_t decoded=0;
          if(!gml_deflate_decode_to_buffer(encoded,encoded_size,GML_DEFLATE_ZLIB,rgba,bytes,&decoded) ||
             decoded!=bytes){ free(rgba);free(seen_runtime);s->ok=0;return 0; }
        }else cr_raw(s,rgba,bytes);
        if(extra || id>=render->base_n_spr){
          if(id>=0 && id<render->n_spr && render->spr[id].runtime_extra) gml_sprite_delete(render,id);
          int got=gml_sprite_append_from_rgba_frames(render,rgba,w,h,frames,ox,oy,NULL);
          if(got!=id){ free(seen_runtime); s->ok=0; return 0; }
        } else if(!gml_sprite_replace_from_rgba_frames(render,id,rgba,w,h,frames,ox,oy)){
          free(rgba); free(seen_runtime); s->ok=0; return 0;
        }
      }
      if(mode==2) s->pos+=encoded_size;
    } else {
      free(seen_runtime); s->ok=0; return 0;
    }
    if(id>=0 && id<render->n_spr){
      GmlSprite *sp=&render->spr[id];
      int mask_rowb=cr_i32(s), mask_count=cr_i32(s);
      int expected_rowb=(int)(((unsigned)w+7u)/8u);
      if(mask_rowb<0 || mask_count<0 ||
         ((mask_rowb==0)!=(mask_count==0)) ||
         (mask_rowb && (mask_rowb!=expected_rowb || mask_count>frames))){
        free(seen_runtime); s->ok=0; return 0;
      }
      size_t remaining=s->pos<=s->cap ? s->cap-s->pos : 0;
      size_t mask_bytes=0;
      if(!state_bounded_product3((size_t)mask_rowb,(size_t)h,(size_t)mask_count,
                                 remaining,&mask_bytes)){
        free(seen_runtime); s->ok=0; return 0;
      }
      uint8_t *mask=NULL;
      if(mask_bytes){
        mask=malloc(mask_bytes);
        if(!mask){ free(seen_runtime); s->ok=0; return 0; }
        cr_raw(s,mask,mask_bytes);
      }
      free(sp->runtime_mask);
      sp->runtime_mask=mask;
      sp->mask=mask;
      sp->mask_rowb=mask_rowb;
      sp->mask_count=mask_count;
      /* These values came from the running renderer and are part of the canonical snapshot.
       * Classic assets may use the full width or height as their right/bottom extent. Rewriting
       * such an extent while loading makes save-load-save non-canonical and changes collisions
       * after rewind. Reject invalid enum fields, but restore authored bounds byte-for-byte. */
      if(kind<0 || kind>3 || tolerance<0 || tolerance>255){
        free(seen_runtime); s->ok=0; return 0;
      }
      sp->ml=ml; sp->mt=mt; sp->mr=mr; sp->mb=mb;
      sp->collision_kind=kind; sp->collision_tolerance=tolerance;
      /* Names are language-visible identities, independent of a relocated source path or a
       * reusable pixel cache. Restore them even when the existing pixels were retained. */
      uint32_t name_bytes=cr_u32(s);
      remaining=s->pos<=s->cap?s->cap-s->pos:0;
      if(!s->ok || name_bytes>GML_STATE_SPRITE_NAME_BYTES || name_bytes>remaining ||
         (name_bytes && (s->data[s->pos+name_bytes-1] ||
                        memchr(s->data+s->pos,0,name_bytes-1)))){
        free(seen_runtime); s->ok=0; return 0;
      }
      const char *name=name_bytes?(const char*)s->data+s->pos:NULL;
      if((name && (!sp->name || strcmp(name,sp->name))) || (!name && sp->name)){
        char *owned=NULL;
        if(name){
          owned=malloc(name_bytes);
          if(!owned){ free(seen_runtime); s->ok=0; return 0; }
          memcpy(owned,name,name_bytes);
        }
        free(sp->owned_name);
        sp->owned_name=owned;
        sp->name=owned;
        render->spr_name_gen++;
      }
      s->pos+=name_bytes;
    }
  }
  int base=render->base_n_spr>0?render->base_n_spr:render->n_spr;
  if(base>render->n_spr) base=render->n_spr;
  for(int i=render->n_spr-1;i>=base;i--)
    if(render->spr[i].runtime_extra && (i>=seen_cap || !seen_runtime[i]))
      gml_sprite_delete(render,i);
  for(int i=0;i<base;i++) if(render->spr[i].runtime_rgba && (i>=seen_cap || !seen_runtime[i])){
    GmlSprite *sp=&render->spr[i];
    gml_render_sprite_state_cache_clear(sp);
    free(sp->runtime_rgba); free(sp->runtime_mask); free(sp->runtime_row_min); free(sp->runtime_row_max); free(sp->runtime_source_path);
    sp->runtime_rgba=NULL; sp->runtime_mask=NULL; sp->runtime_row_min=NULL; sp->runtime_row_max=NULL; sp->runtime_source_path=NULL; sp->runtime_owned=0; sp->runtime_extra=0;
    sp->runtime_source_imgnum=0; sp->runtime_source_removeback=0;
  }
  free(seen_runtime);
  return s->ok;
}

size_t gml_render_state_size(GmlRender *render,int derived_view_surface){
  if(!render) return 0;
  CoreW state={0};
  state.ok=1;
  render_state_write(render,derived_view_surface,&state);
  return state.ok?state.pos:0;
}

int gml_render_state_save(GmlRender *render,int derived_view_surface,
                          void *data,size_t length,size_t *written){
  if(!render || !data) return 0;
  CoreW state={(uint8_t*)data,length,0,1};
  render_state_write(render,derived_view_surface,&state);
  if(written) *written=state.pos;
  return state.ok && state.pos<=length;
}

int gml_render_state_load(GmlRender *render,const void *data,size_t length,size_t *used){
  if(!render || !data) return 0;
  CoreR state={(const uint8_t*)data,length,0,1};
  int result=render_state_read(render,&state);
  if(used) *used=state.pos;
  return result && state.ok && state.pos<=length;
}

int gml_render_state_profile_metrics(const GmlRender *render,
                                     GmlRenderStateProfileMetrics *metrics){
  if(!render || !metrics) return 0;
  memset(metrics,0,sizeof *metrics);
  for(int i=0;i<GML_MAX_SURFACES;i++){
    const GmlSurface *surface=&render->surface[i];
    if(surface->live && surface->px && surface->w>0 && surface->h>0){
      metrics->surface_count++;
      metrics->surface_bytes+=(size_t)surface->w*(size_t)surface->h*sizeof(uint32_t);
    }
  }
  for(int i=0;i<render->n_spr;i++){
    const GmlSprite *sprite=&render->spr[i];
    if(sprite->runtime_rgba && sprite->w>0 && sprite->h>0 && sprite->n_frames>0){
      size_t bytes=(size_t)sprite->w*(size_t)sprite->h*(size_t)sprite->n_frames*4;
      metrics->runtime_sprite_count++;
      if(sprite->runtime_source_path && sprite->runtime_source_path[0]){
        metrics->file_runtime_sprite_count++;
        metrics->file_runtime_sprite_bytes+=bytes;
      } else {
        metrics->inline_runtime_sprite_bytes+=bytes;
      }
    }
  }
  return 1;
}
